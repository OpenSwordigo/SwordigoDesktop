// Force rebuild: PostFXState struct changed in fbo_scaler.h
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#include <windows.h>
#endif
#include <iostream>
#include <iomanip>
#include <vector>
#include <stdint.h>
#include <filesystem>
namespace fs = std::filesystem;
#include "loader/elf_loader.h"
#include "loader/elf_loader_arm64.h"
#include "loader/arch_detect.h"
#include "jni/jni_layer.h"
#include "jni/jni_layer_arm64.h"
#include "jni/jni_bridge.h"
#include "jni/jni_bridge_arm64.h"
#include <unicorn/unicorn.h>
#include "platform/emulator.h"
#include "platform/emulator_arm64.h"
#include "platform/i_emulator_arm64.h"
#include "platform/emulator_dynarmic64.h"
#include "platform/display.h"
#include "android/asset_manager.h"
extern "C" void asset_manager_init_arm32(const char* base_path);
#include "game/camera_override.h"
#include "game/mod_tools.h"
#include "game/sky_renderer.h"
#include "platform/input_config.h"
#include <cstring>
#include <chrono>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"
#include <GL/glext.h>
#include <sys/stat.h>

#include "platform/video_background.h"
#include "platform/gui.h"
#include "platform/vulkan_backend.h"
#include "platform/fbo_scaler.h"
#include "platform/io_thread.h"
#include "platform/binary_selector.h"
#include "platform/launcher_ui.h"
#include "platform/srt_overlay.h"
#include "platform/save_editor.h"
#include "platform/swordfare_gui.h"  /* Swordfare: ImGui-based in-game overlay */
#include "imgui/imgui.h"

extern bool g_display_active;
extern int g_win_w;
extern int g_win_h;
extern int g_draw_w;  // Physical drawable pixels (for glViewport / FBO)
extern int g_draw_h;

// --- Global Context ---
uint8_t* g_guest_memory = nullptr;
const uint32_t GUEST_MEM_SIZE = 0xE0000000; // 3.5GB — ARM64 needs >3GB for area transitions

// Game internal render resolution — game engine sees this via setApplicationViewSize.
// Defaults to 1920×1080, but dynamically updated after display init to match
// the display's physical pixel dimensions and native aspect ratio.
// Touch coordinates are kept in legacy 960×544 space and auto-scaled.
int GAME_W = 1920;
int GAME_H = 1080;
static float TOUCH_SCALE_X = (float)GAME_W / 960.0f;
// Which game binary to load (switchable via launcher or --lib flag)
static std::string g_lib_name = "engine/v1.4.12/arm64-v8a/libswordigo.so";
std::string g_assets_dir = "assets";  // "assets" for vanilla, "rl_assets" for RLSwordigo
static float TOUCH_SCALE_Y = (float)GAME_H / 544.0f;   // ~1.985

// Add a specific area for guest-side global variables (libc stuff)
const uint32_t GUEST_GLOBALS_BASE = 0x50000; 
const uint32_t GUEST_GLOBALS_SIZE = 0x1000; 

// ARM32 subsystem
so_module g_main_mod;
ElfLoader* g_loader = nullptr;
JniBridge g_bridge;
Emulator* g_emulator = nullptr;

// ARM64 subsystem
so_module_arm64 g_main_mod_64;
so_module_arm64 g_sre_mod;
ElfLoaderArm64* g_loader_64 = nullptr;
JniBridge64 g_bridge_64;
IEmulatorArm64* g_emulator_64 = nullptr;

// Architecture flag — set during boot based on selected binary
bool g_is_arm64 = false;
bool g_use_dynarmic = false;  // Set via --engine=dynarmic
bool g_use_sre = true;        // Set via launcher or --no-sre
GuiRenderer g_gui;
SrtOverlay g_srt_overlay;
InputConfig g_input_config;
SDL_Gamepad* g_gamepad = nullptr;
FBOScale g_fbo_mode = FBOScale::SHARP_BILINEAR;
PostFXState g_postfx;
PostFXPreset g_postfx_preset = PostFXPreset::OFF;
SkyRenderer g_sky;
SwordfareGUI g_swordfare_gui;  /* Swordfare: ImGui in-game overlay */

// Graphics API selection (enum defined in vulkan_backend.h)
GraphicsAPI g_graphics_api = GraphicsAPI::OPENGL;
#ifdef VULKAN_BACKEND
VulkanBackend g_vk_backend;
#endif

// Binary selection system
BinarySelector g_binary_selector;

// ============================================================
//  SRE Lua Console — Execute Lua code inside the running engine
// ============================================================
static uint64_t g_lua_console_buf_addr = 0;       // Guest addr of SRE's command buffer
static uint64_t g_lua_console_result_addr = 0;    // Guest addr of SRE's result buffer
static uint64_t g_lua_console_pending_addr = 0;   // Guest addr of pending flag
static uint64_t g_lua_console_status_addr = 0;    // Guest addr of status (0=idle,1=ok,2=err)
static uint64_t g_lua_console_print_addr = 0;     // Guest addr of captured print() output
static bool     g_lua_console_ready = false;       // true when all addrs resolved
// NOTE: console open state + history now owned by SwordfareGUI (draw_lua_console)

static bool should_block_keyboard() {
    if (g_graphics_api != GraphicsAPI::OPENGL) return false;
    if (ImGui::GetCurrentContext() == nullptr) return false;
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || 
           g_swordfare_gui.is_mod_overlay_visible() || 
           g_swordfare_gui.is_visible() || 
           g_swordfare_gui.is_lua_console_open();
}

// SRE lua_resume error monitoring — guest addresses
static uint64_t g_sre_resume_err_count_addr = 0;
static uint64_t g_sre_resume_last_err_addr = 0;
static int g_sre_resume_err_last_seen = 0;  // host-side last read value

// SRE lua_call_safe error monitoring — guest addresses
static uint64_t g_sre_lua_error_buf_addr = 0;
static uint64_t g_sre_lua_error_count_addr = 0;
static int g_sre_lua_error_last_seen = 0;

// SRE __cxa_throw caller diagnostics — guest addresses
static uint64_t g_sre_cxa_caller_addr = 0;     // addr of g_sre_cxa_throw_caller
static uint64_t g_sre_cxa_unrecovered_addr = 0; // addr of g_sre_cxa_throw_unrecovered
static int g_sre_cxa_last_seen = 0;

// Player position guest addresses for GLES2 lighting
uint64_t g_sre_hero_pos_x_addr = 0;
uint64_t g_sre_hero_pos_y_addr = 0;
uint64_t g_sre_hero_pos_z_addr = 0;

static uint64_t g_sre_cam_x_addr = 0;
static uint64_t g_sre_cam_y_addr = 0;
static uint64_t g_sre_cam_z_addr = 0;

// SRE Background Renderer — guest addresses
static uint64_t bg_mode_addr = 0;       // Guest addr of g_sre_bg_mode
static uint64_t bg_brightness_addr = 0; // Guest addr of g_sre_bg_brightness
static uint64_t bg_depth_addr = 0;      // Guest addr of g_sre_bg_depth
static uint64_t bg_scale_addr = 0;      // Guest addr of g_sre_bg_scale
static uint64_t bg_cam_z_addr = 0;      // Guest addr of g_sre_bg_cam_z

// SRE Effect hooks — guest addresses
static uint64_t portal_active_addr = 0;
static uint64_t portal_x_addr = 0;
static uint64_t portal_y_addr = 0;
static uint64_t portal_z_addr = 0;
static uint64_t portal_count_addr = 0;
static uint64_t portal_color_r_addr = 0;
static uint64_t portal_color_g_addr = 0;
static uint64_t portal_color_b_addr = 0;
static uint64_t portal_vp_matrix_addr = 0;  // 16 floats — VP matrix for screen positioning
static uint64_t effect_frame_reset_addr = 0;

// SRE Music — guest addresses for command interface
static uint64_t sre_music_load_name_addr = 0;    // char[256] — music name to load
static uint64_t sre_music_load_pending_addr = 0;  // int — 1 = load requested
static uint64_t sre_music_play_pending_addr = 0;  // int — 1 = play requested
static uint64_t sre_music_pause_pending_addr = 0; // int — 1 = pause requested
static uint64_t sre_music_stop_pending_addr = 0;  // int — 1 = stop requested
static uint64_t sre_music_volume_addr = 0;        // float — current volume
static uint64_t sre_music_volume_dirty_addr = 0;  // int — 1 = volume changed
static uint64_t sre_music_master_volume_addr = 0; // float — guest s_master_volume reference
static uint64_t sre_music_looping_addr = 0;       // int — looping flag
static uint64_t sre_music_looping_dirty_addr = 0; // int — 1 = looping changed

// SRE LNI — guest addresses for command interface
static uint64_t sre_lni_command_addr = 0;
static uint64_t sre_lni_arg1_addr = 0;
static uint64_t sre_lni_arg2_addr = 0;
static uint64_t sre_lni_pending_addr = 0;
static uint64_t sre_controls_hidden_addr = 0;

// Host-side music ducking — triggered by 0item.wav asset loading
// When the engine loads this SFX (boss kill, XP sack), we duck the
// background music volume: fade down → hold low → fade back up.
#define DUCK_FADE_DOWN_TIME  2.0f    // seconds to fade volume down
#define DUCK_HOLD_TIME       7.0f    // seconds to hold at low volume
#define DUCK_FADE_UP_TIME    3.0f    // seconds to fade volume back up
#define DUCK_LOW_VOLUME      0.15f   // volume floor during ducking (15%)
int   g_music_duck_trigger = 0;      // set to 1 by bridge — starts ducking
static int   s_duck_phase = 0;       // 0=off, 1=fading down, 2=holding, 3=fading up
static float s_duck_timer = 0.0f;    // time remaining in current phase
static float s_duck_volume = 1.0f;   // current ducking multiplier (0..1)

// SRE GUI — player stats (read from GameState via GameSceneView::Update hook)
static uint64_t sre_player_hp_addr = 0;         // int — current HP
static uint64_t sre_player_max_hp_addr = 0;     // int — max HP (computed: level*2+4)
static uint64_t sre_player_mana_addr = 0;       // int — current mana
static uint64_t sre_player_max_mana_addr = 0;   // int — max mana (computed: level*20+10)
static uint64_t sre_player_coins_addr = 0;      // int — coins
static uint64_t sre_player_xp_addr = 0;         // int — experience points
static uint64_t sre_player_level_addr = 0;       // int — experience level
static uint64_t sre_player_atk_level_addr = 0;   // int — attack attribute
static uint64_t sre_gui_scene_active_addr = 0;  // int — 1 when scene is active
static uint64_t sre_gamestate_ptr_addr = 0;      // uint64_t — guest ptr to GameState
static uint64_t sre_menu_active_addr = 0;        // int — bitfield, nonzero = menu open
static uint64_t sre_text_input_active_addr = 0;  // int — nonzero = SRE text input active
static uint64_t sre_hardmode_addr = 0;            // int — nonzero = hard mode (no frame cap, double-tick)

// SRE ButtonController — guest addresses and layout struct
#define SRE_BTN_MAX       128
#define SRE_BTN_ID_LEN    32
#define SRE_BTN_LABEL_LEN 64

struct SreBtnSlot;
static bool simulate_virtual_key(const char* label, bool is_down);

struct SreBtnSlot {
    char     id[SRE_BTN_ID_LEN];       /* Lua string ID */
    char     label[SRE_BTN_LABEL_LEN]; /* Display text */
    float    x, y;                      /* Normalized position (0-1) */
    float    w, h;                      /* Normalized dimensions (base-relative) */
    float    alpha;                     /* Overall alpha 0-255 */
    float    scale_x, scale_y;         /* Scaling factors */
    int      text_color;               /* Packed ARGB */
    float    text_scale;               /* Text size multiplier */
    int      bg_alpha;                 /* Background alpha (0-255) */
    int      hidden;                   /* Per-button hidden flag */
    int      clickable;                /* Whether it accepts clicks */
    int      movable;                  /* Can be dragged */
    int      snapback;                 /* Returns to original pos on release */
    float    home_x, home_y;           /* Original position (for snapback) */
    int      padding_l, padding_t, padding_r, padding_b;
    int      alignment;                /* Text alignment / gravity */
    char     overlay_id[SRE_BTN_ID_LEN]; /* Belongs to overlay */
    int      confined;                  /* Confined to overlay bounds */
    /* ---- STATE (written by host, read by SRE) ---- */
    volatile int pressed;              /* Host writes: 1=down, 0=up */
    volatile int released;             /* Host writes: 1 on release */
    volatile int dragging;             /* Host writes: 1=dragging, 0=not */
    volatile float cur_x, cur_y;       /* Current position (after drag) */
    int      active;                   /* 1 = slot in use, 0 = free */
    int      dirty;                    /* 1 = needs visual update by host */
};

#define SRE_OVERLAY_MAX 8
struct SreOverlaySlot {
    char     id[SRE_BTN_ID_LEN];
    float    x, y;
    float    w, h;
    int      bg_color;
    int      bg_alpha;
    float    corner_radius;
    int      hidden;
    int      movable;
    int      pinchable;
    float    scale_factor;
    int      pinching;
    /* Separators inside overlay */
    float    separators[8];
    int      separator_count;
    int      active;
    int      dirty;
};

static uint64_t sre_btn_array_addr = 0;           // SreBtnSlot[SRE_BTN_MAX]
static uint64_t sre_overlay_array_addr = 0;       // SreOverlaySlot[SRE_OVERLAY_MAX]
static uint64_t sre_btn_globally_hidden_addr = 0;  // int — 1 = all buttons hidden

static bool simulate_virtual_key(const char* label, bool is_down) {
    if (!sre_btn_array_addr || !g_guest_memory) return false;
    SreBtnSlot* buttons = (SreBtnSlot*)(g_guest_memory + sre_btn_array_addr);
    bool found = false;
    for (int i = 0; i < SRE_BTN_MAX; i++) {
        if (buttons[i].active && !buttons[i].hidden) {
            if (strcasecmp(buttons[i].label, label) == 0) {
                found = true;
                if (is_down) {
                    if (buttons[i].pressed == 0) {
                        buttons[i].pressed = 1;
                        buttons[i].released = 0;
                        buttons[i].dirty = 1;
                    }
                } else {
                    if (buttons[i].pressed == 1) {
                        buttons[i].pressed = 0;
                        buttons[i].released = 1;
                        buttons[i].dirty = 1;
                    }
                }
                break;
            }
        }
    }
    return found;
}

int g_sre_viewport_x = 0;
int g_sre_viewport_y = 0;
int g_sre_viewport_w = 960;
int g_sre_viewport_h = 544;

int g_sre_coin_limit = 9999; // Global coin limit variable referenced by loader patches
volatile int g_sre_controls_disabled = 0; // Global controls disabled flag set by Lua UI API

// PostFX auto-disable state — tracks what the user chose so we can restore
static bool postfx_suppressed_by_menu = false;
static PostFXPreset postfx_user_preset = PostFXPreset::OFF;
static bool postfx_user_enabled = true;

// SRE Music Host API — declared in jni_bridge_arm64.cpp
extern bool sre_music_host_load(const std::string& name);
extern void sre_music_host_play();
extern void sre_music_host_pause();
extern void sre_music_host_stop();
extern void sre_music_host_set_volume(float vol);
extern void sre_music_host_set_looping(bool loop);
extern const std::string& sre_music_host_get_track();
extern float sre_music_host_get_volume();
extern bool sre_music_host_is_playing();

// FWKeyboard API — resolved from game binary symbols
// Using uint64_t to support both ARM32 and ARM64 address spaces
static uint64_t g_fw_sharedKeyboard = 0;   // Caver::FWKeyboard::sharedKeyboard()
static uint64_t g_fw_sendKeyDown = 0;       // Caver::FWKeyboard::SendKeyDownEvent(uint, uint, double)
static uint64_t g_fw_sendKeyUp = 0;         // Caver::FWKeyboard::SendKeyUpEvent(uint, uint, double)
static uint64_t g_fw_sendKeyChar = 0;       // Caver::FWKeyboard::SendKeyCharEvent(uint, double)
static uint64_t g_fw_handleMenuBtn = 0;     // Java_com_touchfoo_swordigo_Native_handleMenuButtonPress
static bool g_typing_mode = false;          // F6 toggle: keyboard sends FWKeyboard events

// Text input system — game calls startTextInput/stopTextInput via JNI,
// we route SDL text events back to the game via textInputTextDidChange/textInputDidFinish
static uint64_t g_fw_textInputDidChange = 0; // Java_com_touchfoo_swordigo_Native_textInputTextDidChange
static uint64_t g_fw_textInputDidFinish = 0; // Java_com_touchfoo_swordigo_Native_textInputDidFinish
extern bool g_text_input_active;             // Defined in jni_bridge.cpp
extern std::string g_text_input_buffer;      // Defined in jni_bridge.cpp
static bool g_text_input_was_active = false;  // Previous frame state for edge detection

extern std::string g_save_dir;  // Defined in jni_bridge.cpp
extern bool g_gl_hide_hud;      // Defined in jni_bridge.cpp — hides HUD draw calls
extern int g_death_detected_countdown;  // Defined in jni_bridge.cpp
std::string g_cache_dir;

#include "platform/data_path.h"  // get_data_path() — implemented in data_path.cpp

void call_handle_touch_event(uint32_t addr, uint32_t env, uint32_t obj, int action, int id, double time_val, float x, float y, float old_x, float old_y, int tap_count) {
    if (addr == 0) return;
    
    // Check if touch input is disabled globally by Lua scripts (e.g. UI active)
    extern volatile int g_sre_controls_disabled;
    if (g_sre_controls_disabled) {
        return;
    }
    
    // Auto-scale from legacy 960×544 touch space to actual game resolution
    x     *= TOUCH_SCALE_X;  y     *= TOUCH_SCALE_Y;
    old_x *= TOUCH_SCALE_X;  old_y *= TOUCH_SCALE_Y;

    // Save current SP
    uint32_t old_sp = g_emulator->get_reg(13);
    
    // Allocate stack space (aligned to 8 bytes)
    uint32_t new_sp = old_sp - 32;
    g_emulator->set_reg(13, new_sp);
    
    // Write stack arguments to guest memory
    uint8_t* memory = g_emulator->get_memory_base();
    *(double*)(memory + new_sp + 0) = time_val;
    *(float*)(memory + new_sp + 8) = x;
    *(float*)(memory + new_sp + 12) = y;
    *(float*)(memory + new_sp + 16) = old_x;
    *(float*)(memory + new_sp + 20) = old_y;
    *(uint32_t*)(memory + new_sp + 24) = tap_count;
    
    // R0-R3 registers
    g_emulator->set_reg(0, env);
    g_emulator->set_reg(1, obj);
    g_emulator->set_reg(2, action);
    g_emulator->set_reg(3, id);
    
    // Run guest function
    g_emulator->run(addr);
    
    // Restore SP
    g_emulator->set_reg(13, old_sp);
}

// Get FWKeyboard singleton pointer from the game engine
static uint32_t get_fw_keyboard() {
    if (!g_fw_sharedKeyboard) return 0;
    g_emulator->run(g_fw_sharedKeyboard);
    return g_emulator->get_reg(0);  // Returns FWKeyboard* in R0
}

// Call FWKeyboard::SendKeyDownEvent(keyCode, modifiers, timestamp) or SendKeyUpEvent
// ARM32 AAPCS: this=R0, keyCode=R1, modifiers=R2, double timestamp → aligned to stack
static void call_fw_key_event(uint32_t func_addr, uint32_t keyboard_ptr, uint32_t keyCode, uint32_t modifiers, double timestamp) {
    if (!func_addr || !keyboard_ptr) return;
    
    uint32_t old_sp = g_emulator->get_reg(13);
    uint32_t new_sp = (old_sp - 16) & ~7;  // 8-byte aligned
    g_emulator->set_reg(13, new_sp);
    
    uint8_t* memory = g_emulator->get_memory_base();
    // double on stack (R3 skipped for 8-byte alignment)
    *(double*)(memory + new_sp) = timestamp;
    
    g_emulator->set_reg(0, keyboard_ptr);  // this
    g_emulator->set_reg(1, keyCode);
    g_emulator->set_reg(2, modifiers);
    // R3 is padding for double alignment
    
    g_emulator->run(func_addr);
    g_emulator->set_reg(13, old_sp);
}

// Call FWKeyboard::SendKeyCharEvent(charCode, timestamp)
// ARM32: this=R0, charCode=R1, double timestamp → R2+R3 (8-byte aligned)
static void call_fw_key_char(uint32_t keyboard_ptr, uint32_t charCode, double timestamp) {
    if (!g_fw_sendKeyChar || !keyboard_ptr) return;
    
    uint32_t old_sp = g_emulator->get_reg(13);
    uint32_t new_sp = (old_sp - 16) & ~7;
    g_emulator->set_reg(13, new_sp);
    
    uint8_t* memory = g_emulator->get_memory_base();
    // charCode in R1, then double must be 8-byte aligned → goes to R2+R3
    union { double d; uint32_t u[2]; } conv;
    conv.d = timestamp;
    
    g_emulator->set_reg(0, keyboard_ptr);
    g_emulator->set_reg(1, charCode);
    g_emulator->set_reg(2, conv.u[0]);  // low word
    g_emulator->set_reg(3, conv.u[1]);  // high word
    
    g_emulator->run(g_fw_sendKeyChar);
    g_emulator->set_reg(13, old_sp);
}

// Send a complete key press (down + up) through FWKeyboard
static void fw_send_key_press(uint32_t keyCode, double timestamp) {
    uint32_t kb = get_fw_keyboard();
    if (!kb) return;
    call_fw_key_event(g_fw_sendKeyDown, kb, keyCode, 0, timestamp);
    call_fw_key_event(g_fw_sendKeyUp, kb, keyCode, 0, timestamp);
}

// Send a character through FWKeyboard (for text input fields)
static void fw_send_char(uint32_t charCode, double timestamp) {
    uint32_t kb = get_fw_keyboard();
    if (!kb) return;
    call_fw_key_char(kb, charCode, timestamp);
}

// --- Text Input Callbacks ---
// These call back into the game's native JNI functions to notify
// it of text changes (like Android's EditText → Native bridge).

// Guest memory region for text input strings (must be < 0x40000000 for resolve_jstring)
static uint32_t g_text_input_str_addr = 0x3F000000;

// Allocate a jstring in guest memory and return its guest address
static uint32_t write_guest_jstring(uint8_t* memory, const std::string& text) {
    // Write string at a fixed scratch area (text input is single-threaded)
    uint32_t addr = g_text_input_str_addr;
    size_t len = text.length();
    if (len > 4000) len = 4000;  // Safety clamp
    memcpy(memory + addr, text.c_str(), len + 1);
    return addr;
}

// Call textInputTextDidChange(JNIEnv* env, jclass cls, jstring text)
// Hooked by SRE — our reimplementation bypasses the corrupt vtable.
static void call_text_input_did_change(uint32_t env_ptr, const std::string& text) {
    if (!g_fw_textInputDidChange) return;
    uint8_t* memory = g_emulator->get_memory_base();
    uint32_t str_addr = write_guest_jstring(memory, text);
    g_emulator->call(g_fw_textInputDidChange, {env_ptr, 0, str_addr});
}

// Call textInputDidFinish(JNIEnv* env, jclass cls)
// Hooked by SRE — clears editing state and delegate pointer directly.
static void call_text_input_did_finish(uint32_t env_ptr) {
    if (!g_fw_textInputDidFinish) return;
    g_emulator->call(g_fw_textInputDidFinish, {env_ptr, 0});
}

void init_all() {
    // CRITICAL: calloc zero-initializes via OS lazy-zeroing (demand-paged).
    // new uint8_t[N] leaves garbage that causes 0xFFFFFFFF... in uninitialized
    // stack/BSS areas, creating corrupt 64-bit pointers on ARM64.
    g_guest_memory = (uint8_t*)calloc(GUEST_MEM_SIZE, 1);
    if (!g_guest_memory) {
        std::cerr << "FATAL: Failed to allocate " << GUEST_MEM_SIZE << " bytes for guest memory" << std::endl;
        exit(1);
    }
    g_loader = new ElfLoader(g_guest_memory, GUEST_MEM_SIZE);
    g_loader_64 = new ElfLoaderArm64(g_guest_memory, GUEST_MEM_SIZE);
    g_bridge.init_standard_bridges();
    g_bridge_64.init_standard_bridges();
    asset_manager_init(get_data_path(g_assets_dir).c_str());
    asset_manager_init_arm32(get_data_path(g_assets_dir).c_str());
    g_gui.init();
    g_input_config.load(g_save_dir + "/controls.ini");
    std::cout << "[Input] Loaded controls config (" << g_input_config.button_count() << " buttons)" << std::endl;
    
    // Start async IO thread
    io_thread_start();

    // Initialize FBO Scaler
    if (g_display_active) {
        fbo_init(GAME_W, GAME_H);
        draw_batcher_init();
        postfx_apply_preset(g_postfx, g_postfx_preset);
        postfx_user_preset = g_postfx_preset;
        postfx_user_enabled = g_postfx.enabled;
    }
    
    // Open any connected gamepad
    SDL_Init(SDL_INIT_GAMEPAD);
    int gpad_count = 0;
    SDL_JoystickID *gpad_ids = SDL_GetGamepads(&gpad_count);
    if (gpad_ids) {
        for (int i = 0; i < gpad_count; i++) {
            g_gamepad = SDL_OpenGamepad(gpad_ids[i]);
            if (g_gamepad) {
                std::cout << "[Input] Gamepad: " << SDL_GetGamepadName(g_gamepad) << std::endl;
                break;
            }
        }
        SDL_free(gpad_ids);
    }
    std::cout << "[Main] Infrastructure initialized." << std::endl;
}

uint32_t setup_jni_env(uint8_t* memory) {
    uint32_t env_ptr = 0x10000;
    uint32_t vtable_ptr = 0x10010;
    
    *(uint32_t*)(memory + env_ptr) = vtable_ptr;
    
    // Fill vtable with a default unhandled bridge address
    for (int i = 0; i < 300; i++) {
        std::string slot_name = "UnhandledJNI_" + std::to_string(i);
        *(uint32_t*)(memory + vtable_ptr + i * 4) = g_bridge.get_address(slot_name);
    }
    
    // Set specific JNI functions at their correct offsets
    *(uint32_t*)(memory + vtable_ptr + 0x18) = g_bridge.get_address("FindClass");
    *(uint32_t*)(memory + vtable_ptr + 0x38) = g_bridge.get_address("ThrowNew");
    *(uint32_t*)(memory + vtable_ptr + 0x4C) = g_bridge.get_address("PushLocalFrame");
    *(uint32_t*)(memory + vtable_ptr + 0x50) = g_bridge.get_address("PopLocalFrame");
    *(uint32_t*)(memory + vtable_ptr + 0x54) = g_bridge.get_address("NewGlobalRef");
    *(uint32_t*)(memory + vtable_ptr + 0x58) = g_bridge.get_address("DeleteGlobalRef");
    *(uint32_t*)(memory + vtable_ptr + 0x5C) = g_bridge.get_address("DeleteLocalRef");
    *(uint32_t*)(memory + vtable_ptr + 0x74) = g_bridge.get_address("NewObjectV");
    *(uint32_t*)(memory + vtable_ptr + 0x7C) = g_bridge.get_address("GetObjectClass");
    *(uint32_t*)(memory + vtable_ptr + 0x84) = g_bridge.get_address("GetMethodID");
    *(uint32_t*)(memory + vtable_ptr + 0x8C) = g_bridge.get_address("CallObjectMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x98) = g_bridge.get_address("CallBooleanMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0xC8) = g_bridge.get_address("CallIntMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0xD4) = g_bridge.get_address("CallLongMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0xF8) = g_bridge.get_address("CallVoidMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x178) = g_bridge.get_address("GetFieldID");
    *(uint32_t*)(memory + vtable_ptr + 0x17C) = g_bridge.get_address("GetBooleanField");
    *(uint32_t*)(memory + vtable_ptr + 0x190) = g_bridge.get_address("GetIntField");
    *(uint32_t*)(memory + vtable_ptr + 0x198) = g_bridge.get_address("GetFloatField");
    *(uint32_t*)(memory + vtable_ptr + 0x1C4) = g_bridge.get_address("GetStaticMethodID");
    *(uint32_t*)(memory + vtable_ptr + 0x1CC) = g_bridge.get_address("CallStaticObjectMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x1D8) = g_bridge.get_address("CallStaticBooleanMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x208) = g_bridge.get_address("CallStaticIntMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x21C) = g_bridge.get_address("CallStaticLongMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x220) = g_bridge.get_address("CallStaticFloatMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x238) = g_bridge.get_address("CallStaticVoidMethodV");
    *(uint32_t*)(memory + vtable_ptr + 0x240) = g_bridge.get_address("GetStaticFieldID");
    *(uint32_t*)(memory + vtable_ptr + 0x244) = g_bridge.get_address("GetStaticObjectField");
    *(uint32_t*)(memory + vtable_ptr + 0x29C) = g_bridge.get_address("NewStringUTF");
    *(uint32_t*)(memory + vtable_ptr + 0x2A0) = g_bridge.get_address("GetStringUTFLength");
    *(uint32_t*)(memory + vtable_ptr + 0x2A4) = g_bridge.get_address("GetStringUTFChars");
    *(uint32_t*)(memory + vtable_ptr + 0x2A8) = g_bridge.get_address("ReleaseStringUTFChars");
    *(uint32_t*)(memory + vtable_ptr + 0x2AC) = g_bridge.get_address("GetArrayLength");
    *(uint32_t*)(memory + vtable_ptr + 0x2B4) = g_bridge.get_address("GetObjectArrayElement");
    *(uint32_t*)(memory + vtable_ptr + 0x2C0) = g_bridge.get_address("NewByteArray");          // Slot 176
    *(uint32_t*)(memory + vtable_ptr + 0x2CC) = g_bridge.get_address("NewIntArray");
    *(uint32_t*)(memory + vtable_ptr + 0x2E0) = g_bridge.get_address("GetByteArrayElements");  // Slot 184
    *(uint32_t*)(memory + vtable_ptr + 0x2EC) = g_bridge.get_address("GetIntArrayElements");
    *(uint32_t*)(memory + vtable_ptr + 0x300) = g_bridge.get_address("ReleaseByteArrayElements"); // Slot 192
    *(uint32_t*)(memory + vtable_ptr + 0x30C) = g_bridge.get_address("ReleaseIntArrayElements");
    *(uint32_t*)(memory + vtable_ptr + 0x340) = g_bridge.get_address("SetByteArrayRegion");     // Slot 208
    *(uint32_t*)(memory + vtable_ptr + 0x34C) = g_bridge.get_address("SetIntArrayRegion");
    *(uint32_t*)(memory + vtable_ptr + 0x35C) = g_bridge.get_address("RegisterNatives");
    *(uint32_t*)(memory + vtable_ptr + 0x36C) = g_bridge.get_address("GetJavaVM");
    *(uint32_t*)(memory + vtable_ptr + 0x374) = g_bridge.get_address("GetStringUTFRegion");

    // Setup JavaVM structures at 0x11000
    uint32_t vm_ptr = 0x11000;
    uint32_t vm_vtable_ptr = 0x11010;
    *(uint32_t*)(memory + vm_ptr) = vm_vtable_ptr;
    
    for (int i = 0; i < 50; i++) {
        std::string slot_name = "UnhandledVM_" + std::to_string(i);
        *(uint32_t*)(memory + vm_vtable_ptr + i * 4) = g_bridge.get_address(slot_name);
    }
    *(uint32_t*)(memory + vm_vtable_ptr + 0x18) = g_bridge.get_address("GetEnv"); // GetEnv at offset 0x18

    return env_ptr;
}

uint64_t setup_jni_env_arm64(uint8_t* memory) {
    uint64_t env_ptr = 0x10000;
    uint64_t vtable_ptr = 0x10010;
    
    *(uint64_t*)(memory + env_ptr) = vtable_ptr;
    
    // Fill vtable with a default unhandled bridge address (300 slots × 8 bytes)
    for (int i = 0; i < 300; i++) {
        std::string slot_name = "UnhandledJNI_" + std::to_string(i);
        *(uint64_t*)(memory + vtable_ptr + i * 8) = g_bridge_64.get_address(slot_name);
    }
    
    // Set specific JNI functions at their correct ARM64 offsets (ARM32 offset × 2)
    *(uint64_t*)(memory + vtable_ptr + 0x30)  = g_bridge_64.get_address("FindClass");
    *(uint64_t*)(memory + vtable_ptr + 0x70)  = g_bridge_64.get_address("ThrowNew");
    *(uint64_t*)(memory + vtable_ptr + 0x98)  = g_bridge_64.get_address("PushLocalFrame");
    *(uint64_t*)(memory + vtable_ptr + 0xA0)  = g_bridge_64.get_address("PopLocalFrame");
    *(uint64_t*)(memory + vtable_ptr + 0xA8)  = g_bridge_64.get_address("NewGlobalRef");
    *(uint64_t*)(memory + vtable_ptr + 0xB0)  = g_bridge_64.get_address("DeleteGlobalRef");
    *(uint64_t*)(memory + vtable_ptr + 0xB8)  = g_bridge_64.get_address("DeleteLocalRef");
    *(uint64_t*)(memory + vtable_ptr + 0xE8)  = g_bridge_64.get_address("NewObjectV");
    *(uint64_t*)(memory + vtable_ptr + 0xF8)  = g_bridge_64.get_address("GetObjectClass");
    *(uint64_t*)(memory + vtable_ptr + 0x108) = g_bridge_64.get_address("GetMethodID");
    *(uint64_t*)(memory + vtable_ptr + 0x118) = g_bridge_64.get_address("CallObjectMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x130) = g_bridge_64.get_address("CallBooleanMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x190) = g_bridge_64.get_address("CallIntMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x1A8) = g_bridge_64.get_address("CallLongMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x1F0) = g_bridge_64.get_address("CallVoidMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x2F0) = g_bridge_64.get_address("GetFieldID");
    *(uint64_t*)(memory + vtable_ptr + 0x2F8) = g_bridge_64.get_address("GetBooleanField");
    *(uint64_t*)(memory + vtable_ptr + 0x320) = g_bridge_64.get_address("GetIntField");
    *(uint64_t*)(memory + vtable_ptr + 0x330) = g_bridge_64.get_address("GetFloatField");
    *(uint64_t*)(memory + vtable_ptr + 0x388) = g_bridge_64.get_address("GetStaticMethodID");
    *(uint64_t*)(memory + vtable_ptr + 0x398) = g_bridge_64.get_address("CallStaticObjectMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x3B0) = g_bridge_64.get_address("CallStaticBooleanMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x410) = g_bridge_64.get_address("CallStaticIntMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x438) = g_bridge_64.get_address("CallStaticLongMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x440) = g_bridge_64.get_address("CallStaticFloatMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x470) = g_bridge_64.get_address("CallStaticVoidMethodV");
    *(uint64_t*)(memory + vtable_ptr + 0x480) = g_bridge_64.get_address("GetStaticFieldID");
    *(uint64_t*)(memory + vtable_ptr + 0x488) = g_bridge_64.get_address("GetStaticObjectField");
    *(uint64_t*)(memory + vtable_ptr + 0x538) = g_bridge_64.get_address("NewStringUTF");
    *(uint64_t*)(memory + vtable_ptr + 0x540) = g_bridge_64.get_address("GetStringUTFLength");
    *(uint64_t*)(memory + vtable_ptr + 0x548) = g_bridge_64.get_address("GetStringUTFChars");
    *(uint64_t*)(memory + vtable_ptr + 0x550) = g_bridge_64.get_address("ReleaseStringUTFChars");
    *(uint64_t*)(memory + vtable_ptr + 0x558) = g_bridge_64.get_address("GetArrayLength");
    *(uint64_t*)(memory + vtable_ptr + 0x568) = g_bridge_64.get_address("GetObjectArrayElement");
    *(uint64_t*)(memory + vtable_ptr + 0x580) = g_bridge_64.get_address("NewByteArray");
    *(uint64_t*)(memory + vtable_ptr + 0x598) = g_bridge_64.get_address("NewIntArray");
    *(uint64_t*)(memory + vtable_ptr + 0x5C0) = g_bridge_64.get_address("GetByteArrayElements");
    *(uint64_t*)(memory + vtable_ptr + 0x5D8) = g_bridge_64.get_address("GetIntArrayElements");
    *(uint64_t*)(memory + vtable_ptr + 0x600) = g_bridge_64.get_address("ReleaseByteArrayElements");
    *(uint64_t*)(memory + vtable_ptr + 0x618) = g_bridge_64.get_address("ReleaseIntArrayElements");
    *(uint64_t*)(memory + vtable_ptr + 0x680) = g_bridge_64.get_address("SetByteArrayRegion"); // [208]
    *(uint64_t*)(memory + vtable_ptr + 0x698) = g_bridge_64.get_address("SetIntArrayRegion");
    *(uint64_t*)(memory + vtable_ptr + 0x6B8) = g_bridge_64.get_address("RegisterNatives");
    *(uint64_t*)(memory + vtable_ptr + 0x6D8) = g_bridge_64.get_address("GetJavaVM");
    *(uint64_t*)(memory + vtable_ptr + 0x6E8) = g_bridge_64.get_address("GetStringUTFRegion");

    // Setup JavaVM structures at 0x11000 (64-bit)
    uint64_t vm_ptr = 0x11000;
    uint64_t vm_vtable_ptr = 0x11010;
    *(uint64_t*)(memory + vm_ptr) = vm_vtable_ptr;
    
    for (int i = 0; i < 50; i++) {
        std::string slot_name = "UnhandledVM_" + std::to_string(i);
        *(uint64_t*)(memory + vm_vtable_ptr + i * 8) = g_bridge_64.get_address(slot_name);
    }
    *(uint64_t*)(memory + vm_vtable_ptr + 0x30) = g_bridge_64.get_address("GetEnv"); // GetEnv at ARM64 offset (0x18 * 2)

    return env_ptr;
}

void process_gui_action(GuiAction gui_action, bool& running) {
    if (gui_action == GUI_NONE) return;
    switch (gui_action) {
        case GUI_EXIT:
            running = false;
            break;
        case GUI_PAUSE:
            mod_toggle_pause();
            std::cout << "[GUI] Emulation " << (g_game_paused ? "PAUSED" : "RESUMED") << std::endl;
            break;
        case GUI_CUSTOMIZE_CONTROLS:
            g_input_config.set_editing(true);
            std::cout << "[GUI] Controls editor opened" << std::endl;
            break;
        case GUI_SAVE_STATE:
            std::cout << "[GUI] Save State requested" << std::endl;
            break;
        case GUI_LOAD_STATE:
            std::cout << "[GUI] Load State requested" << std::endl;
            break;
        case GUI_GAME_SPEED_UP:
            mod_speed_up();
            break;
        case GUI_GAME_SPEED_DOWN:
            mod_speed_down();
            break;
        case GUI_GAME_SPEED_RESET:
            mod_speed_reset();
            break;
        case GUI_TOGGLE_CAM:
            cam_toggle();
            break;
        case GUI_TOGGLE_PAUSE:
            mod_toggle_pause();
            break;
        case GUI_TOGGLE_SMOOTH_CAM:
            cam_toggle_smooth();
            break;
        case GUI_GFX_OPENGL:
            g_graphics_api = GraphicsAPI::OPENGL;
            std::cout << "[GFX] Graphics API set to OpenGL (active)" << std::endl;
            break;
        case GUI_GFX_VULKAN:
            g_graphics_api = GraphicsAPI::VULKAN;
            std::cout << "[GFX] Graphics API set to Vulkan (WIP) — restart required" << std::endl;
            break;
        // Mod menu actions (value adjustments)
        case GUI_MOD_WALK_SPEED_UP:
            g_gui.mod_walk_speed = std::min(g_gui.mod_walk_speed + 0.5f, 10.0f);
            break;
        case GUI_MOD_WALK_SPEED_DOWN:
            g_gui.mod_walk_speed = std::max(g_gui.mod_walk_speed - 0.5f, 0.5f);
            break;
        case GUI_MOD_RUN_SPEED_UP:
            g_gui.mod_run_speed = std::min(g_gui.mod_run_speed + 0.5f, 10.0f);
            break;
        case GUI_MOD_RUN_SPEED_DOWN:
            g_gui.mod_run_speed = std::max(g_gui.mod_run_speed - 0.5f, 0.5f);
            break;
        case GUI_MOD_JUMP_HEIGHT_UP:
            g_gui.mod_jump_height = std::min(g_gui.mod_jump_height + 0.5f, 10.0f);
            break;
        case GUI_MOD_JUMP_HEIGHT_DOWN:
            g_gui.mod_jump_height = std::max(g_gui.mod_jump_height - 0.5f, 0.5f);
            break;
        case GUI_MOD_LEVEL_UP:
        case GUI_MOD_LEVEL_DOWN:
        case GUI_MOD_EXP_UP:
        case GUI_MOD_EXP_DOWN: {
            if (sre_gamestate_ptr_addr && g_guest_memory) {
                uint64_t gs = *(uint64_t*)(g_guest_memory + sre_gamestate_ptr_addr);
                if (gs != 0) {
                    char* gs_host = (char*)g_guest_memory + gs;
                    extern uint32_t g_hero_obj;
                    if (gui_action == GUI_MOD_EXP_UP) {
                        *(int*)(gs_host + 0xB4) += 500;
                        if (g_hero_obj != 0) {
                            *(int*)(g_guest_memory + g_hero_obj + 0x60) = *(int*)(gs_host + 0xB4);
                        }
                    } else if (gui_action == GUI_MOD_EXP_DOWN) {
                        int xp = *(int*)(gs_host + 0xB4);
                        int new_xp = xp > 500 ? xp - 500 : 0;
                        *(int*)(gs_host + 0xB4) = new_xp;
                        if (g_hero_obj != 0) {
                            *(int*)(g_guest_memory + g_hero_obj + 0x60) = new_xp;
                        }
                    } else if (gui_action == GUI_MOD_LEVEL_UP) {
                        *(int*)(gs_host + 0xB8) += 1;
                        if (g_hero_obj != 0) {
                            *(int*)(g_guest_memory + g_hero_obj + 0x64) = *(int*)(gs_host + 0xB8);
                        }
                    } else if (gui_action == GUI_MOD_LEVEL_DOWN) {
                        int lv = *(int*)(gs_host + 0xB8);
                        int new_lv = lv > 1 ? lv - 1 : 1;
                        *(int*)(gs_host + 0xB8) = new_lv;
                        if (g_hero_obj != 0) {
                            *(int*)(g_guest_memory + g_hero_obj + 0x64) = new_lv;
                        }
                    }
                }
            }
            break;
        }
        case GUI_MOD_HEAL_FULL:
        case GUI_MOD_ADD_COINS:
        case GUI_MOD_REFILL_MANA: {
            if (sre_gamestate_ptr_addr && g_guest_memory) {
                uint64_t gs = *(uint64_t*)(g_guest_memory + sre_gamestate_ptr_addr);
                if (gs != 0) {
                    char* gs_host = (char*)g_guest_memory + gs;
                    extern uint32_t g_hero_health_comp;
                    extern uint32_t g_hero_mana_comp;
                    if (gui_action == GUI_MOD_HEAL_FULL) {
                        int hp_level = *(int*)(gs_host + 0xBC);
                        int max_hp = hp_level * 2 + 4;
                        *(int*)(gs_host + 0xA8) = max_hp;
                        if (g_hero_health_comp != 0) {
                            int actual_max_hp = *(int*)(g_guest_memory + g_hero_health_comp + 0x88);
                            *(int*)(g_guest_memory + g_hero_health_comp + 0x7c) = actual_max_hp;
                            *(int*)(g_guest_memory + g_hero_health_comp + 0x80) = actual_max_hp;
                        }
                        mod_toast("HP restored to full!", 1.5f);
                    } else if (gui_action == GUI_MOD_ADD_COINS) {
                        *(int*)(gs_host + 0xB0) += 100;
                        char cbuf[64]; snprintf(cbuf, sizeof(cbuf), "Coins: %d (+100)", *(int*)(gs_host + 0xB0));
                        mod_toast(cbuf, 1.5f);
                    } else if (gui_action == GUI_MOD_REFILL_MANA) {
                        int mana_level = *(int*)(gs_host + 0xC4);
                        int max_mana = mana_level * 20 + 10;
                        *(int*)(gs_host + 0xAC) = max_mana;
                        if (g_hero_mana_comp != 0) {
                            int actual_max_mana = *(int*)(g_guest_memory + g_hero_mana_comp + 0x3c);
                            *(int*)(g_guest_memory + g_hero_mana_comp + 0x40) = actual_max_mana;
                        }
                        mod_toast("Mana restored to full!", 1.5f);
                    }
                }
            }
            break;
        }
        case GUI_MUSIC_VOL_UP: {
            float v = 0.0f;
            if (sre_music_master_volume_addr) {
                v = *(float*)(g_guest_memory + sre_music_master_volume_addr);
                v = std::min(v + 0.1f, 1.0f);
                *(float*)(g_guest_memory + sre_music_master_volume_addr) = v;
                int one = 1;
                memcpy(g_guest_memory + sre_music_volume_dirty_addr, &one, 4);
            } else {
                v = sre_music_host_get_volume();
                v = std::min(v + 0.1f, 1.0f);
                sre_music_host_set_volume(v);
            }
            char vbuf[64]; snprintf(vbuf, sizeof(vbuf), "Music Volume: %d%%", (int)(v * 100));
            mod_toast(vbuf, 1.5f);
            break;
        }
        case GUI_MUSIC_VOL_DOWN: {
            float v = 0.0f;
            if (sre_music_master_volume_addr) {
                v = *(float*)(g_guest_memory + sre_music_master_volume_addr);
                v = std::max(v - 0.1f, 0.0f);
                *(float*)(g_guest_memory + sre_music_master_volume_addr) = v;
                int one = 1;
                memcpy(g_guest_memory + sre_music_volume_dirty_addr, &one, 4);
            } else {
                v = sre_music_host_get_volume();
                v = std::max(v - 0.1f, 0.0f);
                sre_music_host_set_volume(v);
            }
            char vbuf[64]; snprintf(vbuf, sizeof(vbuf), "Music Volume: %d%%", (int)(v * 100));
            mod_toast(vbuf, 1.5f);
            break;
        }
        case GUI_MUSIC_MUTE: {
            float v = 0.0f;
            if (sre_music_master_volume_addr) {
                v = *(float*)(g_guest_memory + sre_music_master_volume_addr);
                static float last_non_mute_vol = 1.0f;
                if (v > 0.01f) {
                    last_non_mute_vol = v;
                    v = 0.0f;
                    mod_toast("Music: MUTED", 1.5f);
                } else {
                    v = last_non_mute_vol;
                    char vbuf[64]; snprintf(vbuf, sizeof(vbuf), "Music: Unmuted (%d%%)", (int)(v * 100));
                    mod_toast(vbuf, 1.5f);
                }
                *(float*)(g_guest_memory + sre_music_master_volume_addr) = v;
                int one = 1;
                memcpy(g_guest_memory + sre_music_volume_dirty_addr, &one, 4);
            } else {
                v = sre_music_host_get_volume();
                if (v > 0.01f) {
                    sre_music_host_set_volume(0.0f);
                    mod_toast("Music: MUTED", 1.5f);
                } else {
                    sre_music_host_set_volume(1.0f);
                    mod_toast("Music: Unmuted (100%)", 1.5f);
                }
            }
            break;
        }
        default: break;
    }
}

void load_and_boot() {
    std::string so_path = get_data_path(g_lib_name);
    std::cout << "[Loader] Loading: " << so_path << std::endl;
    uint32_t load_addr = 0x1000000;
    
    // 1. Load ELF
    if (g_loader->load(&g_main_mod, so_path, load_addr) != 0) {
        std::cerr << "[Loader] Failed to load SO" << std::endl;
        exit(1);
    }
    
    // 2. Internal Relocations
    g_loader->relocate(&g_main_mod);
    
    // 3. External Symbol Resolution (to Bridge addresses)
    g_loader->resolve_all_to_bridge(&g_main_mod, &g_bridge, GUEST_GLOBALS_BASE);
    
    std::cout << "[Loader] Game binary ready (all symbols bridged)." << std::endl;

    // Initialize Emulator AFTER memory is prepared
    g_emulator = new Emulator(g_guest_memory, GUEST_MEM_SIZE);
    g_emulator->set_bridge(&g_bridge);

    // Run dynamic initializers (.init_array)
    if (g_main_mod.init_array_vaddr != 0 && g_main_mod.init_array_size > 0) {
        int count = g_main_mod.init_array_size / 4;
        std::cout << "[Boot] Executing " << count << " dynamic initializers..." << std::endl;
        uint32_t* init_array = (uint32_t*)(g_guest_memory + g_main_mod.init_array_vaddr);
        for (int i = 0; i < count; i++) {
            uint32_t init_func = init_array[i];
            if (init_func != 0) {
                if (i % 50 == 0 || i == count - 1) {
                    std::cout << "  -> Initializer " << i << "/" << count << " at 0x" << std::hex << init_func << std::dec << std::endl;
                }
                g_emulator->call(init_func, {});
            }
        }
        std::cout << "[Boot] Dynamic initializers completed successfully." << std::endl;
    }

    // 4. Boot Sequence
    uint32_t setFilesDir = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_setFilesDir");
    uint32_t setCacheDir = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_setCacheDir");
    uint32_t setAssetMgr = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_setAssetManager");
    uint32_t googleSignInCompleted = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_googleSignInCompleted");
    uint32_t handleAppLaunch = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_handleApplicationLaunch");
    uint32_t initMusicPlayer = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_MusicPlayer_initMusicPlayer");
    uint32_t setupNative = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_setupNativeInterface");
    uint32_t setupApp = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_setupApplication");
    uint32_t setApplicationViewSize = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_setApplicationViewSize");
    uint32_t applicationDidBecomeActive = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_applicationDidBecomeActive");

    // Resolve FWKeyboard API symbols (mangled C++ names from the Caver engine)
    g_fw_sharedKeyboard = g_loader->get_symbol_vaddr(&g_main_mod, "_ZN5Caver10FWKeyboard14sharedKeyboardEv");
    g_fw_sendKeyDown    = g_loader->get_symbol_vaddr(&g_main_mod, "_ZN5Caver10FWKeyboard16SendKeyDownEventEjjd");
    g_fw_sendKeyUp      = g_loader->get_symbol_vaddr(&g_main_mod, "_ZN5Caver10FWKeyboard14SendKeyUpEventEjjd");
    g_fw_sendKeyChar    = g_loader->get_symbol_vaddr(&g_main_mod, "_ZN5Caver10FWKeyboard16SendKeyCharEventEjd");
    g_fw_handleMenuBtn  = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_handleMenuButtonPress");

    // Resolve text input callback symbols (native JNI functions the game exports)
    g_fw_textInputDidChange = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_textInputTextDidChange");
    g_fw_textInputDidFinish = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_textInputDidFinish");

    std::cout << "[Boot] FWKeyboard API: sharedKeyboard=0x" << std::hex << g_fw_sharedKeyboard
              << " sendDown=0x" << g_fw_sendKeyDown << " sendUp=0x" << g_fw_sendKeyUp
              << " sendChar=0x" << g_fw_sendKeyChar << " menuBtn=0x" << g_fw_handleMenuBtn
              << std::dec << std::endl;
    std::cout << "[Boot] TextInput API: didChange=0x" << std::hex << g_fw_textInputDidChange
              << " didFinish=0x" << g_fw_textInputDidFinish << std::dec << std::endl;

    // Setup fake JNI structures in guest memory
    uint32_t env_ptr = setup_jni_env(g_guest_memory);
    std::cout << "[Debug] env_ptr = 0x" << std::hex << env_ptr << " vtable_ptr = 0x" << *(uint32_t*)(g_guest_memory + env_ptr) << std::dec << std::endl;
    std::cout << "[Debug] FindClass slot (0x10028) = 0x" << std::hex << *(uint32_t*)(g_guest_memory + 0x10028) << std::dec << std::endl;
    std::cout << "[Debug] GetStringUTFChars slot (0x102b4) = 0x" << std::hex << *(uint32_t*)(g_guest_memory + 0x102b4) << std::dec << std::endl;


    
    // Allocate some strings in guest memory for paths
    uint32_t path_ptr = 0x20000;
    uint32_t files_dir = path_ptr;
    strncpy((char*)(g_guest_memory + files_dir), g_save_dir.c_str(), 255);
    ((char*)(g_guest_memory + files_dir))[255] = '\0';
    path_ptr += 256;
    
    uint32_t cache_dir = path_ptr;
    strncpy((char*)(g_guest_memory + cache_dir), g_cache_dir.c_str(), 255);
    ((char*)(g_guest_memory + cache_dir))[255] = '\0';
    path_ptr += 256;

    if (setFilesDir) {
        std::cout << "[Boot] Calling setFilesDir" << std::endl;
        g_emulator->call(setFilesDir, {env_ptr, 0, files_dir});
    }
    if (setCacheDir) {
        std::cout << "[Boot] Calling setCacheDir" << std::endl;
        g_emulator->call(setCacheDir, {env_ptr, 0, cache_dir});
    }
    if (setAssetMgr) {
        std::cout << "[Boot] Calling setAssetManager" << std::endl;
        g_emulator->call(setAssetMgr, {env_ptr, 0, 0x55555555}); 
    }
    if (googleSignInCompleted) {
        std::cout << "[Boot] Calling googleSignInCompleted(true) — enable snapshots" << std::endl;
        g_emulator->call(googleSignInCompleted, {env_ptr, 0, 1}); // true = signed in
    }
    if (handleAppLaunch) {
        std::cout << "[Boot] Calling handleApplicationLaunch" << std::endl;
        g_emulator->call(handleAppLaunch, {env_ptr, 0});
    }
    if (initMusicPlayer) {
        std::cout << "[Boot] Calling initMusicPlayer" << std::endl;
        g_emulator->call(initMusicPlayer, {env_ptr, 0x22222222}); // pass non-zero fake MusicPlayer object to enable music callbacks
    }
    if (setupNative) {
        std::cout << "[Boot] Calling setupNativeInterface" << std::endl;
        g_emulator->call(setupNative, {env_ptr, 0});
    }
    if (setupApp) {
        std::cout << "[Boot] Calling setupApplication" << std::endl;
        g_emulator->call(setupApp, {env_ptr, 0});
    }
    if (setApplicationViewSize) {
        std::cout << "[Boot] Calling setApplicationViewSize" << std::endl;
        g_emulator->call(setApplicationViewSize, {env_ptr, 0, (uint32_t)GAME_W, (uint32_t)GAME_H, 1}); // Full HD internal resolution
    }
    if (applicationDidBecomeActive) {
        std::cout << "[Boot] Calling applicationDidBecomeActive" << std::endl;
        g_emulator->call(applicationDidBecomeActive, {env_ptr, 0});
    }

    // ========================================================================
    // Death screen fix: Make game think "Remove Ads" IAP is purchased
    // The game checks IsNoAdsUnlockedCheck() before showing interstitial ads.
    // If it returns true, the death flow skips ads entirely and respawns
    // directly from the last checkpoint. No callbacks or snapshots needed.
    // ========================================================================
    {
        uint8_t* memory = g_emulator->get_memory_base();

        // Patch IsNoAdsUnlockedCheck() -> return true (MOV R0, #1; BX LR)
        uint32_t noAdsCheck = g_loader->get_symbol_vaddr(&g_main_mod,
            "_ZN5Caver15StoreController20IsNoAdsUnlockedCheckEv");
        if (noAdsCheck) {
            uint32_t addr = noAdsCheck & ~1u;
            *(uint16_t*)(memory + addr + 0) = 0x2001; // MOVS R0, #1
            *(uint16_t*)(memory + addr + 2) = 0x4770; // BX LR
            std::cout << "[Fix] Patched IsNoAdsUnlockedCheck at 0x" << std::hex << noAdsCheck
                      << " -> return true (ad-free)" << std::dec << std::endl;
        }

        // Also patch IsProductPurchased -> return true (for any product check)
        uint32_t isPurchased = g_loader->get_symbol_vaddr(&g_main_mod,
            "_ZN5Caver23StoreController_Android18IsProductPurchasedERKSs");
        if (isPurchased) {
            uint32_t addr = isPurchased & ~1u;
            *(uint16_t*)(memory + addr + 0) = 0x2001; // MOVS R0, #1
            *(uint16_t*)(memory + addr + 2) = 0x4770; // BX LR
            std::cout << "[Fix] Patched IsProductPurchased at 0x" << std::hex << isPurchased
                      << " -> return true" << std::dec << std::endl;
        }
        // Patch ShowInterstitialAd to return immediately — prevents JNI ad code from
        // corrupting game state. Death respawn is handled by execv restart fallback.
        uint32_t showAd = g_loader->get_symbol_vaddr(&g_main_mod,
            "_ZN5Caver24OnlineController_Android18ShowInterstitialAdERKSsif");
        if (showAd) {
            uint32_t addr = showAd & ~1u;
            *(uint16_t*)(memory + addr + 0) = 0x4770; // BX LR
            std::cout << "[Fix] Patched ShowInterstitialAd at 0x" << std::hex << showAd
                      << " -> bx lr" << std::dec << std::endl;
        }
        uint32_t showAd2 = g_loader->get_symbol_vaddr(&g_main_mod,
            "_ZN5Caver25AndroidShowInterstitialAdEd");
        if (showAd2) {
            uint32_t addr = showAd2 & ~1u;
            *(uint16_t*)(memory + addr + 0) = 0x4770; // BX LR
            std::cout << "[Fix] Patched AndroidShowInterstitialAd at 0x" << std::hex << showAd2
                      << " -> bx lr" << std::dec << std::endl;
        }

        // Coin limit patches (ARM32)
        // Lua coin limit: offset 0x27fce0
        // Game event limit: offset 0x27e656
        // Too rich check: offset 0x27e666
        {
            uint32_t base_addr = g_main_mod.base_addr;
            if (base_addr) {
                extern int g_sre_coin_limit;
                uint16_t limit = (uint16_t)g_sre_coin_limit;
                
                // movw r1, limit (instruction: 0xf2400100 | (imm4 << 16) | (val << 12) | (imm3 << 12) | imm8)
                // For simplified inline patch, let's assemble Thumb2 MOVW:
                // format: 1111 0 i 10 0100 imm4 0 imm3 rd imm8
                // We can construct it precisely:
                auto emit_movw_thumb = [](uint8_t reg, uint16_t val) -> uint32_t {
                    uint32_t imm4 = (val >> 12) & 0xF;
                    uint32_t i = (val >> 11) & 0x1;
                    uint32_t imm3 = (val >> 8) & 0x7;
                    uint32_t imm8 = val & 0xFF;
                    uint16_t inst1 = 0xF240 | (i << 10) | imm4;
                    uint16_t inst2 = (imm3 << 12) | (reg << 8) | imm8;
                    return inst1 | (inst2 << 16);
                };
                
                uint32_t inst_lua = emit_movw_thumb(1, limit);  // movw r1, #limit
                *(uint32_t*)(memory + base_addr + 0x27fce0) = inst_lua;
                
                uint32_t inst_event = emit_movw_thumb(0, limit); // movw r0, #limit
                *(uint32_t*)(memory + base_addr + 0x27e656) = inst_event;
                
                uint32_t inst_rich = emit_movw_thumb(1, limit - 1); // movw r1, #(limit - 1)
                *(uint32_t*)(memory + base_addr + 0x27e666) = inst_rich;
                
                std::cout << "[CoinLimit] Applied ARM32 coin limit patches (limit=" << limit << ")" << std::endl;
            }
        }
    }

    // Resolve adVisibilityChanged (kept for death detection countdown fallback)
    uint32_t adVisibilityChanged = g_loader->get_symbol_vaddr(&g_main_mod,
        "Java_com_touchfoo_swordigo_Native_interstitialAdVisibilityChanged");

    // 5. Game Loop
    uint32_t updateApp = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_updateApplication");
    uint32_t drawApp = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_drawApplication");
    uint32_t handleTouchEvent = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_handleTouchEvent");
    uint32_t snapshotLoaded = g_loader->get_symbol_vaddr(&g_main_mod, "Java_com_touchfoo_swordigo_Native_snapshotLoaded");
    
    // Tell the game that Google Sign-In succeeded so snapshot system is enabled
    if (googleSignInCompleted) {
        std::cout << "[Boot] Calling googleSignInCompleted(true) — enable snapshots" << std::endl;
        g_emulator->call(googleSignInCompleted, {env_ptr, 0, 1}); // (env, this, true)
    }

    // NOTE: We used to call handleMenuButtonPress here to hide on-screen controls,
    // but that function opens the game menu AND hides controls. The game then shows
    // controls again on any touch input, making it useless. Instead, we use g_gl_hide_hud
    // in the JNI bridge to make HUD draw calls fully transparent.
    bool g_hide_touch_hud = true;  // Hide touch controls by default (desktop uses keyboard)
    g_gl_hide_hud = g_hide_touch_hud;

    if (updateApp && drawApp) {
        // Real delta time
        Uint64 last_ticks = SDL_GetTicks();
        double accumulated_time = 0.0;
        bool gui_visible = false;  // GUI hidden by default
        bool debug_visible = false;  // F3 debug overlay
        bool hint_shown = false;  // One-time "Press F1" hint
        Uint64 hint_start_time = SDL_GetTicks();  // Show hint for first 5 seconds
        float fps = 0.0f;
        Uint64 fps_last_time = SDL_GetTicks();
        int fps_frame_count = 0;
        
        const int TARGET_FRAMES = g_display_active ? 0 : 1000; // 0 = infinite
        int completed_frames = 0;
        
        // Totals for summary
        uint32_t total_draw_calls = 0;
        uint32_t total_tex_binds = 0;
        uint32_t total_tex_uploads = 0;
        uint32_t total_matrix_ops = 0;
        uint32_t total_state_changes = 0;
        uint32_t total_asset_opens = 0;

        std::cout << "\n========================================" << std::endl;
        if (g_display_active) {
            std::cout << "[Loop] Starting visual game loop (close window to stop)" << std::endl;
        } else {
            std::cout << "[Loop] Starting " << TARGET_FRAMES << "-frame stability test" << std::endl;
        }
        std::cout << "========================================\n" << std::endl;

        // Get the display pointer from main (passed via extern)
        extern Display* g_display_ptr;

        // Input states for touch mapping and GUI integration
        int mouse_x = 0;
        int mouse_y = 0;
        bool mouse_pressed = false;
        bool click_swallowed_by_gui = false;
        float last_mouse_x = -1.0f;
        float last_mouse_y = -1.0f;

        bool key_left = false;
        bool key_right = false;
        bool key_jump = false;
        bool key_attack = false;
        bool key_magic = false;
        bool key_use_item = false;
        bool key_menu = false;

        // Camera override keys (Arrow keys — no numpad needed!)
        bool arrow_left  = false;
        bool arrow_right = false;
        bool arrow_up    = false;
        bool arrow_down  = false;
        bool cam_key_pgup  = false;   // PageUp — raise camera
        bool cam_key_pgdn  = false;   // PageDown — lower camera

        bool running = true;
        while (running) {
            g_frame_stats.reset();
            
            // Real delta time
            Uint64 now_ticks = SDL_GetTicks();
            float dt_seconds = (now_ticks - last_ticks) / 1000.0f;
            if (dt_seconds > 0.1f) dt_seconds = 0.016666668f;
            if (dt_seconds < 0.001f) dt_seconds = 0.016666668f;
            last_ticks = now_ticks;
            accumulated_time += dt_seconds;
            
            // Show details for first 10 frames only
            if (completed_frames < 10) {
                g_emulator->quiet_mode = false;
            } else {
                g_emulator->quiet_mode = true;
            }
            
            // --- Send TOUCH_MOVED every frame for held buttons (uses InputConfig positions) ---
            for (int bi = 0; bi < g_input_config.button_count(); bi++) {
                const TouchButton* btn = g_input_config.get_button(bi);
                if (btn && btn->is_pressed) {
                    call_handle_touch_event(handleTouchEvent, env_ptr, 0, 4, btn->touch_id, accumulated_time,
                        btn->game_x, btn->game_y, btn->game_x, btn->game_y, 0);
                }
            }

            // --- Process pending macro step 2 (delayed spell slot tap) ---
            g_input_config.process_macros(accumulated_time,
                [&](int action, int id, double time, float x, float y, float ox, float oy, int tap) {
                    call_handle_touch_event(handleTouchEvent, env_ptr, 0, action, id, time, x, y, ox, oy, tap);
                });

            // --- Per-frame camera movement (Arrow keys, dt-scaled with acceleration) ---
            if (g_cam_active) {
                bool any_cam_key = arrow_left || arrow_right || arrow_up || arrow_down || cam_key_pgup || cam_key_pgdn;
                if (any_cam_key) {
                    float dx = 0, dy = 0, dz = 0;
                    if (arrow_left)   dx -= 1.0f;
                    if (arrow_right)  dx += 1.0f;
                    if (arrow_up)     dy += 1.0f;
                    if (arrow_down)   dy -= 1.0f;
                    if (cam_key_pgup) dz -= 1.0f;  // zoom in
                    if (cam_key_pgdn) dz += 1.0f;  // zoom out
                    cam_move_scaled(dx, dy, dz, dt_seconds);
                } else {
                    cam_reset_accel();
                }
                extern void cam_write_to_guest();
                cam_write_to_guest();
            }

            // --- Apply game speed multiplier ---
            float game_dt = dt_seconds * g_game_speed;
            uint32_t dt_hex;
            memcpy(&dt_hex, &game_dt, 4);

            // --- Pause / Frame Advance ---
            if (!g_game_paused || g_step_one_frame) {
                g_emulator->call(updateApp, {env_ptr, 0, dt_hex});
                if (g_step_one_frame) {
                    g_step_one_frame = false;
                    mod_toast("Stepped 1 frame", 0.8f);
                }
            }
            // --- Death recovery: execv restart after ~3s ---
            // The death state machine is tied to Android's ad system and can't be
            // replicated (same issue as PS Vita port). Restart the process cleanly.
            if (g_death_detected_countdown > 0) {
                g_death_detected_countdown--;
                if (g_death_detected_countdown == 0) {
                    std::cout << "\n[Fix] Death timeout — restarting from checkpoint..." << std::endl;
                    VideoBackground::cleanup();
                    SDL_Quit();
                    extern std::string g_lib_name;
                    extern char** g_saved_argv;
                    extern int g_saved_argc;
                    std::vector<std::string> arg_strs;
                    std::vector<char*> args;
                    for (int i = 0; i < g_saved_argc; i++) {
                        std::string a = g_saved_argv[i];
                        if ((a == "--lib" || a == "--assets") && i + 1 < g_saved_argc) { i++; continue; }
                        arg_strs.push_back(a);
                    }
                    arg_strs.push_back("--lib");
                    arg_strs.push_back(g_lib_name);
                    arg_strs.push_back("--assets");
                    arg_strs.push_back(g_assets_dir);
                    for (auto& s : arg_strs) args.push_back(&s[0]);
                    args.push_back(nullptr);
                    execv("/proc/self/exe", args.data());
                    exit(1);
                }
            }
            
            // Poll async IO thread completion callbacks
            io_thread_poll();

            // Handle async callbacks: if loadSnapshot was called, call snapshotLoaded
            extern int g_snapshot_load_pending_count;
            extern bool g_snapshot_has_data;
            extern std::vector<uint8_t> g_snapshot_data;
            while (g_snapshot_load_pending_count > 0 && snapshotLoaded) {
                g_snapshot_load_pending_count--;
                if (g_snapshot_has_data && !g_snapshot_data.empty()) {
                    // Allocate byte array in guest memory: [length(4)] [data(N)]
                    static uint32_t save_buf_addr = 0x40000000; // high address for save buffer
                    uint32_t array_len = g_snapshot_data.size();
                    if (save_buf_addr + 4 + array_len > 0xE0000000ULL) {
                        std::cerr << "[SAVE] Save data too large (" << array_len << " bytes), skipping" << std::endl;
                        continue;
                    }
                    *(uint32_t*)(g_guest_memory + save_buf_addr) = array_len;
                    memcpy(g_guest_memory + save_buf_addr + 4, g_snapshot_data.data(), array_len);
                    std::cout << "[Callback] snapshotLoaded with " << array_len << " bytes of save data" << std::endl;
                    g_emulator->call(snapshotLoaded, {env_ptr, 0, 0, save_buf_addr}); // (env, this, name=null, data=array)
                    g_snapshot_has_data = false;
                } else {
                    std::cout << "[Callback] snapshotLoaded(null, null) — no saved game" << std::endl;
                    g_emulator->call(snapshotLoaded, {env_ptr, 0, 0, 0}); // (env, this, name=null, data=null)
                }
            }
            
            if (g_display_active) {
#ifdef VULKAN_BACKEND
                if (g_graphics_api == GraphicsAPI::VULKAN) {
                    g_vk_backend.begin_frame();
                } else
#endif
                {
                    fbo_begin_game();
                }
            }
            
            g_emulator->call(drawApp, {env_ptr, 0});

            if (g_display_active) {
#ifdef VULKAN_BACKEND
                if (g_graphics_api == GraphicsAPI::VULKAN) {
                    // Vulkan frame end + present is done after overlays
                } else
#endif
                {
                    draw_batcher_flush(); // Flush any pending batched draws
                    fbo_end_game_and_blit(g_draw_w, g_draw_h, g_fbo_mode, &g_postfx);
                    // Vanilla portal — DISABLED pending proper fixed-function implementation
                    // if (!fbo_is_active()) {
                    //     fbo_draw_portal_vanilla(g_draw_w, g_draw_h);
                    // }
                }
            }

            // Apply camera position override (after draw, before swap)
            // cam_apply(g_emulator, g_guest_memory);
            mod_apply_frame(g_guest_memory);

            // Render GUI overlay (F1 toggle)
            if (g_display_active && gui_visible && g_graphics_api != GraphicsAPI::OPENGL) {
                g_gui.render(mouse_x, mouse_y, mouse_pressed, g_win_w, g_win_h);
            }
            
            // Render controls editor overlay (F2 toggle)
            if (g_display_active && g_input_config.is_editing()) {
                g_input_config.render_editor(g_win_w, g_win_h, mouse_x, mouse_y, mouse_pressed);
                // Draw instruction text on top (uses GUI font renderer)
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                // Controls editor header text
                {
                    std::string mode_str = "POSITION";
                    if (g_input_config.get_editor_mode() == EDITOR_REBIND) mode_str = "REBIND (click button, press key)";
                    g_gui.draw_string("CONTROLS EDITOR [" + mode_str + "]  |  F2=Save & Close  R=Toggle Rebind", 10, g_win_h - 20, 1.3f, 100, 200, 255, 255);
                    g_gui.draw_string("Blue=Move  Green=Action  Orange=Menu  Purple=Magic Macro  Yellow=Custom", 10, g_win_h - 38, 1.1f, 180, 180, 200, 200);
                }
                // Draw readable button labels on each circle
                {
                    float sx = (float)g_win_w / 960.0f;
                    float sy = (float)g_win_h / 544.0f;
                    for (int i = 0; i < g_input_config.button_count(); i++) {
                        TouchButton* btn = g_input_config.get_button(i);
                        if (!btn) continue;
                        float wx = btn->game_x * sx;
                        float wy = btn->game_y * sy;
                        float wr = btn->radius * std::min(sx, sy) * 0.7f;
                        // Display name
                        std::string dname = btn->display_name.empty() ? btn->name : btn->display_name;
                        // Key binding text
                        std::string key1 = InputConfig::scancode_name(btn->sdl_scancode);
                        std::string key2 = InputConfig::scancode_name(btn->sdl_scancode_alt);
                        std::string keytxt = "[" + key1;
                        if (btn->sdl_scancode_alt > 0) keytxt += "/" + key2;
                        keytxt += "]";
                        // Color
                        uint8_t cr = 255, cg = 255, cb = 255;
                        if (btn->is_macro) { cr = 200; cg = 140; cb = 255; }
                        else if (btn->is_custom) { cr = 255; cg = 255; cb = 100; }
                        // Draw name above center
                        g_gui.draw_string(dname, (int)(wx - dname.size() * 4), (int)(wy + 6), 1.0f, cr, cg, cb, 255);
                        // Draw key binding below center
                        g_gui.draw_string(keytxt, (int)(wx - keytxt.size() * 3.5f), (int)(wy - 10), 0.9f, 180, 180, 180, 200);
                        if (btn->is_macro) {
                            g_gui.draw_string("MACRO", (int)(wx - 16), (int)(wy - wr - 14), 0.8f, 180, 80, 220, 200);
                        }
                    }
                }
                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }
            
            // Render F3 debug overlay
            if (g_display_active && debug_visible && g_graphics_api != GraphicsAPI::OPENGL) {
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION);
                glPushMatrix();
                glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW);
                glPushMatrix();
                glLoadIdentity();
                glDisable(GL_TEXTURE_2D);
                glDisable(GL_LIGHTING);
                glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                // Dark background strip
                glColor4ub(0, 0, 0, 180);
                glBegin(GL_QUADS);
                glVertex2f(10, g_win_h - 10); glVertex2f(420, g_win_h - 10);
                glVertex2f(420, g_win_h - 290); glVertex2f(10, g_win_h - 290);
                glEnd();
                // FPS text using simple pixel rendering
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "FPS: %.1f", fps);
                g_gui.draw_string(dbg, 20, g_win_h - 35, 2.0f, 0, 255, 100, 255);
                snprintf(dbg, sizeof(dbg), "Frame: %d", completed_frames);
                g_gui.draw_string(dbg, 20, g_win_h - 60, 1.4f, 200, 200, 200, 255);
                snprintf(dbg, sizeof(dbg), "Draws: %d  TexBinds: %d", g_frame_stats.draw_calls, g_frame_stats.texture_binds);
                g_gui.draw_string(dbg, 20, g_win_h - 80, 1.2f, 180, 180, 180, 255);
                snprintf(dbg, sizeof(dbg), "Verts: %d  MatOps: %d", g_frame_stats.vertices_submitted, g_frame_stats.matrix_ops);
                g_gui.draw_string(dbg, 20, g_win_h - 97, 1.2f, 180, 180, 180, 255);
                snprintf(dbg, sizeof(dbg), "State: %d  TexUps: %d", g_frame_stats.state_changes, g_frame_stats.tex_uploads);
                g_gui.draw_string(dbg, 20, g_win_h - 114, 1.2f, 180, 180, 180, 255);
                snprintf(dbg, sizeof(dbg), "Render: %dx%d -> Window: %dx%d (Draw: %dx%d)", GAME_W, GAME_H, g_win_w, g_win_h, g_draw_w, g_draw_h);
                g_gui.draw_string(dbg, 20, g_win_h - 131, 1.2f, 140, 140, 160, 255);
                snprintf(dbg, sizeof(dbg), "Mouse: %d,%d  DT: %.4fs", mouse_x, mouse_y, dt_seconds);
                g_gui.draw_string(dbg, 20, g_win_h - 148, 1.2f, 140, 140, 160, 255);
                
                const char* mode_str = "Unknown";
                if (g_fbo_mode == FBOScale::SHARP_BILINEAR) mode_str = "Sharp-Bilinear";
                else if (g_fbo_mode == FBOScale::NEAREST) mode_str = "Nearest";
                else if (g_fbo_mode == FBOScale::CRT_SCANLINE) mode_str = "CRT Scanline";
                else if (g_fbo_mode == FBOScale::FSR) mode_str = "FSR 1.0";
                snprintf(dbg, sizeof(dbg), "Scale Mode: %s", mode_str);
                g_gui.draw_string(dbg, 20, g_win_h - 165, 1.2f, 255, 200, 100, 255);

                snprintf(dbg, sizeof(dbg), "Speed: %s  %s", mod_speed_label(), g_game_paused ? "|| PAUSED" : "");
                g_gui.draw_string(dbg, 20, g_win_h - 185, 1.2f, 255, 180, 50, 255);
                snprintf(dbg, sizeof(dbg), "F1:GUI F2:Ctrl F3:Dbg F4:Scale F5:Cam F6:PostFX F7:Video \\\\:Type F10:HUD");
                g_gui.draw_string(dbg, 20, g_win_h - 202, 1.1f, 100, 100, 120, 200);
                if (g_typing_mode) {
                    g_gui.draw_string("TYPING MODE ACTIVE", 20, g_win_h - 218, 1.2f, 255, 100, 100, 255);
                }
                char cam_dbg[128];
                cam_debug_string(cam_dbg, sizeof(cam_dbg));
                g_gui.draw_string(cam_dbg, 20, g_win_h - 222, 1.1f,
                    g_cam_active ? 0 : 120,
                    g_cam_active ? 220 : 120,
                    g_cam_active ? 100 : 120, 255);

                // Binary info
                const BinaryInfo* binfo = g_binary_selector.get_loaded_info();
                if (binfo) {
                    snprintf(dbg, sizeof(dbg), "Binary: %s (%s)",
                             binfo->filename.c_str(), binfo->label.c_str());
                    g_gui.draw_string(dbg, 20, g_win_h - 240, 1.2f, 100, 200, 255, 255);
                } else {
                    snprintf(dbg, sizeof(dbg), "Binary: %s", g_lib_name.c_str());
                    g_gui.draw_string(dbg, 20, g_win_h - 240, 1.2f, 100, 200, 255, 255);
                }

                // PostFX info
                if (g_postfx.enabled) {
                    snprintf(dbg, sizeof(dbg), "PostFX: %s", g_postfx.preset_name);
                    g_gui.draw_string(dbg, 20, g_win_h - 255, 1.2f, 255, 180, 100, 255);
                } else {
                    g_gui.draw_string("PostFX: Off", 20, g_win_h - 255, 1.2f, 120, 120, 120, 200);
                }

                // Graphics API
                const char* api_str = (g_graphics_api == GraphicsAPI::VULKAN) ? "Vulkan" : "OpenGL";
                snprintf(dbg, sizeof(dbg), "Graphics API: %s", api_str);
                g_gui.draw_string(dbg, 20, g_win_h - 272, 1.2f, 100, 220, 255, 255);

                glPopMatrix();
                glMatrixMode(GL_PROJECTION);
                glPopMatrix();
                glPopAttrib();
            }
            
            // Render mod tools overlay (speed indicator, toasts, pause screen)
            if (g_display_active) {
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                mod_render_overlay(g_gui, g_win_w, g_win_h, dt_seconds);
                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }

            // Render SRT overlay (inventory editor, etc.) — F11 toggle
            if (g_display_active && g_srt_overlay.is_visible()) {
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                g_srt_overlay.render(g_gui, g_win_w, g_win_h,
                                      mouse_x, mouse_y,
                                      mouse_pressed, dt_seconds);
                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }

            // Show hint for first 5 seconds
            if (g_display_active && !gui_visible && (SDL_GetTicks() - hint_start_time) < 5000) {
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION);
                glPushMatrix();
                glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW);
                glPushMatrix();
                glLoadIdentity();
                glDisable(GL_TEXTURE_2D);
                glDisable(GL_LIGHTING);
                glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glColor4ub(0, 0, 0, 160);
                int cx = g_win_w / 2;
                int cy = g_win_h / 2;
                glBegin(GL_QUADS);
                glVertex2f(cx - 280, cy - 15); glVertex2f(cx + 280, cy - 15);
                glVertex2f(cx + 280, cy + 15); glVertex2f(cx - 280, cy + 15);
                glEnd();
                g_gui.draw_string("WASD:Move  Arrows:Camera  +/-:Speed  F5:Cam  F11:Inventory  F12:Fullscreen", cx - 340, cy - 7, 1.1f, 0, 200, 255, 255);
                glPopMatrix();
                glMatrixMode(GL_PROJECTION);
                glPopMatrix();
                glPopAttrib();
            }
            
            // Draw Swordfare GUI F3 Overlay if visible
            if (g_display_active && g_graphics_api == GraphicsAPI::OPENGL) {
                g_swordfare_gui.begin_frame();
                if (g_swordfare_gui.is_visible()) {
                    SwordfareDebugStats st;
                    st.fps           = fps;
                    st.dt_seconds    = dt_seconds;
                    st.frame_count   = completed_frames;
                    st.draw_calls    = g_frame_stats.draw_calls;
                    st.tex_binds     = g_frame_stats.texture_binds;
                    st.vertices      = g_frame_stats.vertices_submitted;
                    st.matrix_ops    = g_frame_stats.matrix_ops;
                    st.state_changes = g_frame_stats.state_changes;
                    st.tex_uploads   = g_frame_stats.tex_uploads;
                    st.win_w         = g_win_w;
                    st.win_h         = g_win_h;
                    st.draw_w        = g_draw_w;
                    st.draw_h        = g_draw_h;
                    st.mouse_x       = mouse_x;
                    st.mouse_y       = mouse_y;
                    st.cam_x         = g_sre_cam_x_addr ? *(float*)(g_guest_memory + g_sre_cam_x_addr) : g_cam_off_x;
                    st.cam_y         = g_sre_cam_y_addr ? *(float*)(g_guest_memory + g_sre_cam_y_addr) : g_cam_off_y;
                    st.cam_z         = g_sre_cam_z_addr ? *(float*)(g_guest_memory + g_sre_cam_z_addr) : g_cam_off_z;
                    st.cam_zoom      = 1.0f;
                    st.hero_x        = g_sre_hero_pos_x_addr ? *(float*)(g_guest_memory + g_sre_hero_pos_x_addr) : 0.0f;
                    st.hero_y        = g_sre_hero_pos_y_addr ? *(float*)(g_guest_memory + g_sre_hero_pos_y_addr) : 0.0f;
                    st.hero_z        = g_sre_hero_pos_z_addr ? *(float*)(g_guest_memory + g_sre_hero_pos_z_addr) : 0.0f;
                    st.cam_active    = g_cam_active;
                    st.typing_mode   = g_typing_mode;
                    st.game_paused   = g_game_paused;
                    st.postfx_on     = g_postfx.enabled;
                    st.postfx_preset = g_postfx.preset_name;
                    if (g_fbo_mode == FBOScale::SHARP_BILINEAR) st.scale_mode = "Sharp-Bilinear";
                    else if (g_fbo_mode == FBOScale::NEAREST) st.scale_mode = "Nearest";
                    else if (g_fbo_mode == FBOScale::CRT_SCANLINE) st.scale_mode = "CRT Scanline";
                    else if (g_fbo_mode == FBOScale::FSR) st.scale_mode = "FSR 1.0";
                    st.binary_name    = g_lib_name.c_str();
                    st.speed_label    = mod_speed_label();
                    st.graphics_api   = "OpenGL";
                    g_swordfare_gui.draw_debug(st);
                }
                
                // Draw SRE LUA buttons and overlays if present
                if (sre_btn_array_addr && g_guest_memory) {
                    void* overlays_ptr = sre_overlay_array_addr ? (g_guest_memory + sre_overlay_array_addr) : nullptr;
                    bool globally_hidden = false;
                    if (sre_btn_globally_hidden_addr) {
                        globally_hidden = (*(int*)(g_guest_memory + sre_btn_globally_hidden_addr) != 0);
                    }
                    g_swordfare_gui.draw_buttons(g_guest_memory + sre_btn_array_addr, overlays_ptr, globally_hidden);
                }
                if (gui_visible) {
                    GuiAction act = g_swordfare_gui.draw_control_panel(&gui_visible);
                    if (act != GUI_NONE) {
                        process_gui_action(act, running);
                    }
                }
                g_swordfare_gui.draw_lua_console();
                g_swordfare_gui.draw_lua_script_editor();
                g_swordfare_gui.draw_lua_script_manager();
                g_swordfare_gui.draw_mod_overlay(g_save_dir);
                g_swordfare_gui.end_frame();
            }

            extern int g_gl_diag_frame;
            g_gl_diag_frame++;
            
            // Present the frame
            if (g_display_active && g_display_ptr) {
#ifdef VULKAN_BACKEND
                if (g_graphics_api == GraphicsAPI::VULKAN) {
                    g_vk_backend.end_frame_and_present();
                } else
#endif
                {
                    g_display_ptr->swap();
                }
                
                // Auto-toggle SDL text input when game requests it
                if (g_text_input_active && !g_text_input_was_active) {
                    SDL_StartTextInput(g_display_ptr->get_window());
                    std::cout << "[TextInput] SDL text input enabled" << std::endl;
                } else if (!g_text_input_active && g_text_input_was_active) {
                    SDL_StopTextInput(g_display_ptr->get_window());
                    std::cout << "[TextInput] SDL text input disabled" << std::endl;
                }
                g_text_input_was_active = g_text_input_active;

                // Poll and process SDL window and input events
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (g_graphics_api == GraphicsAPI::OPENGL && g_swordfare_gui.process_event(event)) {
                        continue;
                    }
                    switch (event.type) {
                        case SDL_EVENT_QUIT:
                            running = false;
                            break;
                        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                            running = false;
                            break;
                        case SDL_EVENT_WINDOW_RESIZED:
                            g_win_w = event.window.data1;
                            g_win_h = event.window.data2;
                            // Physical drawable size for GL rendering (HiDPI)
                            SDL_GetWindowSizeInPixels(g_display_ptr->get_window(), &g_draw_w, &g_draw_h);
                            std::cout << "[Display] Window resized to " << g_win_w << "x" << g_win_h
                                      << " (drawable: " << g_draw_w << "x" << g_draw_h << ")" << std::endl;
                            break;
                        case SDL_EVENT_MOUSE_BUTTON_DOWN:
                            if (event.button.button == SDL_BUTTON_LEFT) {
                                mouse_pressed = true;
                                mouse_x = event.button.x;
                                mouse_y = g_win_h - event.button.y;
                                
                                // Controls editor intercepts all mouse events
                                if (g_input_config.is_editing()) {
                                    float gx = event.button.x * 960.0f / (float)g_win_w;
                                    float gy = 544.0f - (event.button.y * 544.0f / (float)g_win_h);
                                    g_input_config.editor_mouse_down(gx, gy);
                                    click_swallowed_by_gui = true;
                                    break;
                                }
                                
                                GuiAction gui_action = GUI_NONE;
                                bool gui_consumed = false;
                                if (gui_visible && g_graphics_api != GraphicsAPI::OPENGL) {
                                    gui_action = g_gui.handle_click(mouse_x, mouse_y, g_win_w, g_win_h);
                                    gui_consumed = (gui_action != GUI_NONE) 
                                                 || (mouse_y >= g_win_h - (int)(32 * g_gui.get_scale()))
                                                 || g_gui.has_modal_open();
                                }
                                if (gui_consumed) {
                                    click_swallowed_by_gui = true;
                                    process_gui_action(gui_action, running);
                                } else {
                                    bool btn_hit = false;
                                    if (g_graphics_api == GraphicsAPI::OPENGL) {
                                        ImGuiIO& io = ImGui::GetIO();
                                        if (io.WantCaptureMouse || g_swordfare_gui.is_input_blocked((float)event.button.x, (float)event.button.y)) {
                                            btn_hit = true;
                                        }
                                    }
                                    if (btn_hit) {
                                        click_swallowed_by_gui = true;
                                    } else {
                                        click_swallowed_by_gui = false;
                                        float x = event.button.x * 960.0f / (float)g_win_w;
                                        float y = 544.0f - (event.button.y * 544.0f / (float)g_win_h);
                                        last_mouse_x = x;
                                        last_mouse_y = y;
                                        call_handle_touch_event(handleTouchEvent, env_ptr, 0, 1, 1, accumulated_time, x, y, x, y, 1);
                                    }
                                }
                            }
                            break;
                        case SDL_EVENT_MOUSE_MOTION:
                            mouse_x = event.motion.x;
                            mouse_y = g_win_h - event.motion.y;
                            if (g_input_config.is_editing() && mouse_pressed) {
                                float gx = event.motion.x * 960.0f / (float)g_win_w;
                                float gy = 544.0f - (event.motion.y * 544.0f / (float)g_win_h);
                                g_input_config.editor_mouse_move(gx, gy);
                            } else if (mouse_pressed && !click_swallowed_by_gui) {
                                float x = event.motion.x * 960.0f / (float)g_win_w;
                                float y = 544.0f - (event.motion.y * 544.0f / (float)g_win_h);
                                call_handle_touch_event(handleTouchEvent, env_ptr, 0, 4, 1, accumulated_time, x, y, last_mouse_x, last_mouse_y, 0);
                                last_mouse_x = x;
                                last_mouse_y = y;
                            }
                            break;
                        case SDL_EVENT_MOUSE_BUTTON_UP:
                            if (event.button.button == SDL_BUTTON_LEFT) {
                                mouse_pressed = false;
                                mouse_x = event.button.x;
                                mouse_y = g_win_h - event.button.y;
                                if (g_input_config.is_editing()) {
                                    g_input_config.editor_mouse_up();
                                } else if (!click_swallowed_by_gui) {
                                    float x = event.button.x * 960.0f / (float)g_win_w;
                                    float y = 544.0f - (event.button.y * 544.0f / (float)g_win_h);
                                    call_handle_touch_event(handleTouchEvent, env_ptr, 0, 2, 1, accumulated_time, x, y, last_mouse_x, last_mouse_y, 0);
                                    last_mouse_x = -1.0f;
                                    last_mouse_y = -1.0f;
                                }
                                click_swallowed_by_gui = false;
                            }
                            break;
                        case SDL_EVENT_MOUSE_WHEEL:
                            if (g_cam_active) {
                                cam_scroll_zoom((float)event.wheel.y);
                            }
                            break;
                        case SDL_EVENT_KEY_DOWN:
                            if (g_graphics_api == GraphicsAPI::OPENGL && g_swordfare_gui.process_event(event)) {
                                break;
                            }
                            if (should_block_keyboard()) {
                                bool is_toggle = (event.key.key == SDLK_F1 || event.key.key == SDLK_F2 || event.key.key == SDLK_F3 || event.key.key == SDLK_F4 || event.key.key == SDLK_GRAVE || event.key.key == SDLK_ESCAPE);
                                if (!is_toggle) break;
                            }
                            if (event.key.key == SDLK_F1 && !event.key.repeat) {
                                gui_visible = !gui_visible;
                                std::cout << "[GUI] " << (gui_visible ? "ON" : "OFF") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F4 && !event.key.repeat) {
                                g_swordfare_gui.toggle_mod_overlay();
                                std::cout << "[ModOverlay] " << (g_swordfare_gui.is_mod_overlay_visible() ? "ON" : "OFF") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F2 && !event.key.repeat) {
                                g_input_config.toggle_editor();
                                if (!g_input_config.is_editing()) {
                                    g_input_config.save(g_save_dir + "/controls.ini");
                                    std::cout << "[Input] Controls config saved" << std::endl;
                                }
                                std::cout << "[Controls Editor] " << (g_input_config.is_editing() ? "ON" : "OFF") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F3 && !event.key.repeat) {
                                debug_visible = !debug_visible;
                                if (g_graphics_api == GraphicsAPI::OPENGL) {
                                    g_swordfare_gui.toggle_visible();
                                }
                                std::cout << "[Debug] " << (debug_visible ? "ON" : "OFF") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F4 && !event.key.repeat) {
                                g_fbo_mode = static_cast<FBOScale>((static_cast<int>(g_fbo_mode) + 1) % 4);
                                const char* m_name = "Unknown";
                                if (g_fbo_mode == FBOScale::SHARP_BILINEAR) m_name = "Sharp-Bilinear";
                                else if (g_fbo_mode == FBOScale::NEAREST) m_name = "Nearest";
                                else if (g_fbo_mode == FBOScale::CRT_SCANLINE) m_name = "CRT Scanline";
                                else if (g_fbo_mode == FBOScale::FSR) m_name = "FSR 1.0 (Edge-Adaptive)";
                                std::cout << "[FBO] Scale mode changed to " << m_name << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F12 && !event.key.repeat) {
                                SDL_WindowFlags flags = SDL_GetWindowFlags(g_display_ptr->get_window());
                                if (flags & SDL_WINDOW_FULLSCREEN) {
                                    SDL_SetWindowFullscreen(g_display_ptr->get_window(), false);
                                    // Query actual window size — preserves native aspect ratio (16:10, etc.)
                                    int ww, wh;
                                    SDL_GetWindowSize(g_display_ptr->get_window(), &ww, &wh);
                                    g_win_w = ww;
                                    g_win_h = wh;
                                    SDL_GetWindowSizeInPixels(g_display_ptr->get_window(), &g_draw_w, &g_draw_h);
                                    std::cout << "[Display] Windowed " << ww << "x" << wh << " (drawable: " << g_draw_w << "x" << g_draw_h << ")" << std::endl;
                                } else {
                                    SDL_SetWindowFullscreen(g_display_ptr->get_window(), true);
                                    // Get actual fullscreen resolution (logical + physical)
                                    int fw, fh;
                                    SDL_GetWindowSize(g_display_ptr->get_window(), &fw, &fh);
                                    g_win_w = fw;
                                    g_win_h = fh;
                                    SDL_GetWindowSizeInPixels(g_display_ptr->get_window(), &g_draw_w, &g_draw_h);
                                    std::cout << "[Display] Fullscreen " << fw << "x" << fh << " (drawable: " << g_draw_w << "x" << g_draw_h << ")" << std::endl;
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F5 && !event.key.repeat) {
                                cam_toggle();
                                break;
                            }
                            if (event.key.key == SDLK_M && !event.key.repeat) {
                                if (g_cam_active) {
                                    g_cam_pov_mode = !g_cam_pov_mode;
                                    char msg[64];
                                    snprintf(msg, sizeof(msg), "Camera POV: %s", g_cam_pov_mode ? "ON" : "OFF");
                                    mod_toast(msg, 1.5f);
                                    cam_write_to_guest();
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F6 && !event.key.repeat) {
                                g_postfx_preset = static_cast<PostFXPreset>(
                                    (static_cast<int>(g_postfx_preset) + 1) % static_cast<int>(PostFXPreset::COUNT)
                                );
                                postfx_apply_preset(g_postfx, g_postfx_preset);
                                // Update saved user state for menu auto-restore
                                postfx_user_preset = g_postfx_preset;
                                postfx_user_enabled = g_postfx.enabled;
                                postfx_suppressed_by_menu = false;
                                std::cout << "[PostFX] Preset: " << g_postfx.preset_name << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_R && !event.key.repeat) {
                                std::cout << "[PostFX] Reloading custom postfx config..." << std::endl;
                                if (postfx_load_custom_json(g_postfx)) {
                                    std::cout << "[PostFX] Successfully loaded custom config!" << std::endl;
                                } else {
                                    std::cout << "[PostFX] Failed to load custom config, generating default template." << std::endl;
                                    postfx_save_default_json();
                                    postfx_load_custom_json(g_postfx);
                                }
                                break;
                            }
                            if (event.key.key == SDLK_BACKSLASH && !event.key.repeat) {
                                g_typing_mode = !g_typing_mode;
                                if (g_typing_mode) {
                                    SDL_StartTextInput(g_display_ptr->get_window());
                                    std::cout << "[Keyboard] Typing mode ON — keyboard sends FWKeyboard events" << std::endl;
                                } else {
                                    SDL_StopTextInput(g_display_ptr->get_window());
                                    std::cout << "[Keyboard] Typing mode OFF — keyboard sends touch events" << std::endl;
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F7 && !event.key.repeat) {
                                g_video_background_enabled = !g_video_background_enabled;
                                std::cout << "[VideoBackground] Video playback: " << (g_video_background_enabled ? "ENABLED" : "DISABLED") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F10 && !event.key.repeat) {
                                // Toggle host-side touch HUD visibility
                                // (paints over button areas instead of calling handleMenuButtonPress
                                //  which would open the game's settings menu)
                                g_hide_touch_hud = !g_hide_touch_hud;
                                g_gl_hide_hud = g_hide_touch_hud;
                                std::cout << "[Keyboard] Touch HUD: " << (g_hide_touch_hud ? "HIDDEN" : "VISIBLE") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F8 && !event.key.repeat) {
                                mod_toggle_pause();
                                break;
                            }
                            if (event.key.key == SDLK_F9 && !event.key.repeat) {
                                mod_step_frame();
                                break;
                            }
                            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                                g_swordfare_gui.toggle_mod_overlay();
                                break;
                            }
                            if (event.key.key == SDLK_EQUALS && !event.key.repeat) {
                                mod_speed_up();
                                break;
                            }
                            if (event.key.key == SDLK_MINUS && !event.key.repeat) {
                                mod_speed_down();
                                break;
                            }
                            if (event.key.key == SDLK_0 && !event.key.repeat) {
                                mod_speed_reset();
                                break;
                            }
                        case SDL_EVENT_KEY_UP: {
                            if (should_block_keyboard()) {
                                bool is_toggle = (event.key.key == SDLK_F1 || event.key.key == SDLK_F2 || event.key.key == SDLK_F3 || event.key.key == SDLK_F4 || event.key.key == SDLK_GRAVE || event.key.key == SDLK_ESCAPE);
                                if (!is_toggle) break;
                            }
                            bool is_down = (event.type == SDL_EVENT_KEY_DOWN);
                            int scancode = event.key.scancode;

                            // ---- Virtual Key translation for Android mods ----
                            bool handled_virtual = false;
                            extern bool g_sre_overlay_blocking;
                            bool block_virtual = g_sre_overlay_blocking && (event.key.key != SDLK_ESCAPE && event.key.key != SDLK_X);
                            if (!block_virtual) {
                                if (event.key.key == SDLK_TAB) {
                                    handled_virtual = simulate_virtual_key("Open", is_down);
                                } else if (event.key.key == SDLK_E) {
                                    handled_virtual = simulate_virtual_key("Use", is_down);
                                } else if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_X) {
                                    handled_virtual = simulate_virtual_key("X", is_down);
                                } else if (event.key.key == SDLK_V) {
                                    handled_virtual = simulate_virtual_key("Armory", is_down);
                                } else if (event.key.key == SDLK_K) {
                                    handled_virtual = simulate_virtual_key("Keybinds", is_down);
                                } else if (event.key.key == SDLK_S) {
                                    handled_virtual = simulate_virtual_key("Settings", is_down);
                                } else if (event.key.key == SDLK_P) {
                                    handled_virtual = simulate_virtual_key("+ Keybind", is_down);
                                }
                            }
                            if (handled_virtual) {
                                break;
                            }
                            
                            // ---- TEXT INPUT MODE: intercept keys when game requests text input ----
                            if (g_text_input_active && is_down) {
                                if (event.key.key == SDLK_RETURN) {
                                    std::cout << "[TextInput] Enter — confirming \"" << g_text_input_buffer << "\"" << std::endl;
                                    call_text_input_did_change(env_ptr, g_text_input_buffer);
                                    call_text_input_did_finish(env_ptr);
                                    g_text_input_active = false;
                                    g_text_input_buffer.clear();
                                    break;
                                } else if (event.key.key == SDLK_ESCAPE) {
                                    std::cout << "[TextInput] Escape — cancelling" << std::endl;
                                    g_text_input_buffer.clear();
                                    call_text_input_did_change(env_ptr, "");
                                    call_text_input_did_finish(env_ptr);
                                    g_text_input_active = false;
                                    break;
                                } else if (event.key.key == SDLK_BACKSPACE) {
                                    if (!g_text_input_buffer.empty()) {
                                        g_text_input_buffer.pop_back();
                                        call_text_input_did_change(env_ptr, g_text_input_buffer);
                                        std::cout << "[TextInput] Backspace → \"" << g_text_input_buffer << "\"" << std::endl;
                                    }
                                    break;
                                }
                                // Other keys: fall through to typing mode or normal handling
                                // (text characters come via SDL_EVENT_TEXT_INPUT, not KEY_DOWN)
                                break; // Eat all other key_down events during text input
                            }
                            if (g_text_input_active && !is_down) break; // Eat key-up too

                            // ---- TYPING MODE: route through FWKeyboard API ----
                            if (g_typing_mode && g_fw_sendKeyDown && g_fw_sendKeyUp) {
                                // Map SDL keysym to FWKeyboard key codes
                                // Based on Caver engine internal codes (iOS/Android keycodes)
                                uint32_t fw_key = 0;
                                switch (event.key.key) {
                                    case SDLK_RETURN:     fw_key = 0x24; break; // Enter
                                    case SDLK_BACKSPACE:  fw_key = 0x33; break; // Backspace/Delete
                                    case SDLK_ESCAPE:     fw_key = 0x35; break; // Escape
                                    case SDLK_LEFT:       fw_key = 0x7B; break; // Arrow left
                                    case SDLK_RIGHT:      fw_key = 0x7C; break; // Arrow right
                                    case SDLK_DOWN:       fw_key = 0x7D; break; // Arrow down
                                    case SDLK_UP:         fw_key = 0x7E; break; // Arrow up
                                    case SDLK_TAB:        fw_key = 0x30; break; // Tab
                                    case SDLK_SPACE:      fw_key = 0x31; break; // Space
                                    case SDLK_DELETE:     fw_key = 0x75; break; // Forward delete
                                    // Menu button (same as handleMenuButtonPress)
                                    case SDLK_F10:        fw_key = 0x5d; break;
                                    default: break;
                                }

                                if (fw_key != 0) {
                                    uint32_t kb = get_fw_keyboard();
                                    if (kb) {
                                        if (is_down)
                                            call_fw_key_event(g_fw_sendKeyDown, kb, fw_key, 0, accumulated_time);
                                        else
                                            call_fw_key_event(g_fw_sendKeyUp, kb, fw_key, 0, accumulated_time);
                                    }
                                }
                                break; // Don't fall through to InputConfig
                            }
                            
                            // ---- NORMAL MODE: route through InputConfig ----
                            extern bool g_sre_overlay_blocking;
                            if (!g_input_config.is_editing() && !g_sre_overlay_blocking) {
                                bool controls_hidden = false;
                                if (sre_controls_hidden_addr) {
                                    controls_hidden = (*(int*)(g_guest_memory + sre_controls_hidden_addr) != 0);
                                }
                                if (event.key.key == SDLK_A || event.key.key == SDLK_D) {
                                    if (is_down && g_cam_active && g_cam_pov_mode) {
                                        g_cam_pov_facing = (event.key.key == SDLK_A) ? -1.0f : 1.0f;
                                        cam_write_to_guest();
                                    }
                                }
                                if (controls_hidden) {
                                    break;
                                }

                                // Prevent camera keys from moving the character when camera mode is active
                                bool is_cam_key = (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
                                                   event.key.key == SDLK_UP || event.key.key == SDLK_DOWN ||
                                                   event.key.key == SDLK_EQUALS || event.key.key == SDLK_MINUS ||
                                                   event.key.key == SDLK_KP_PLUS || event.key.key == SDLK_KP_MINUS);
                                if (g_cam_active && is_cam_key) {
                                    // Bypassed, do not route to game controls
                                } else {
                                    int btn_idx = g_input_config.find_by_scancode(scancode);
                                    if (btn_idx >= 0) {
                                        TouchButton* btn = g_input_config.get_button(btn_idx);
                                        if (btn && btn->is_pressed != is_down) {
                                            btn->is_pressed = is_down;
                                            
                                            if (is_down && btn->is_macro && btn->macro_open_touch_id >= 0) {
                                                // Macro step 1: press the opener button (use_item)
                                                TouchButton* opener = nullptr;
                                                for (int j = 0; j < g_input_config.button_count(); j++) {
                                                    TouchButton* t = g_input_config.get_button(j);
                                                    if (t && t->touch_id == btn->macro_open_touch_id) { opener = t; break; }
                                                }
                                                if (opener) {
                                                    int macro_tid = 100 + btn->touch_id;
                                                    call_handle_touch_event(handleTouchEvent, env_ptr, 0,
                                                        1, macro_tid, accumulated_time,
                                                        opener->game_x, opener->game_y, opener->game_x, opener->game_y, 1);
                                                }
                                                btn->macro_pending = true;
                                                btn->macro_stage = 1;
                                                btn->macro_fire_time = (uint64_t)(accumulated_time * 1000.0);
                                            } else if (!btn->is_macro) {
                                                // Normal button press/release
                                                call_handle_touch_event(handleTouchEvent, env_ptr, 0,
                                                    is_down ? 1 : 2, btn->touch_id, accumulated_time,
                                                    btn->game_x, btn->game_y, btn->game_x, btn->game_y,
                                                    is_down ? 1 : 0);
                                            }
                                        }
                                        break;
                                    }
                                }
                            } else {
                                // Editor: capture key for rebinding
                                if (is_down && g_input_config.editor_handle_key(scancode)) break;
                            }
                            
                            // Camera + arrow keys (camera-only, not game movement)
                            switch (event.key.key) {
                                case SDLK_LEFT:
                                    if (g_cam_active) arrow_left = is_down;
                                    break;
                                case SDLK_RIGHT:
                                    if (g_cam_active) arrow_right = is_down;
                                    break;
                                case SDLK_UP:
                                    if (g_cam_active) arrow_up = is_down;
                                    break;
                                case SDLK_DOWN:
                                    if (g_cam_active) arrow_down = is_down;
                                    break;
                                case SDLK_PAGEUP:
                                    if (g_cam_active) cam_key_pgup = is_down;
                                    break;
                                case SDLK_PAGEDOWN:
                                    if (g_cam_active) cam_key_pgdn = is_down;
                                    break;
                                case SDLK_EQUALS:
                                case SDLK_KP_PLUS:
                                    if (g_cam_active) cam_key_pgup = is_down;
                                    break;
                                case SDLK_MINUS:
                                case SDLK_KP_MINUS:
                                    if (g_cam_active) cam_key_pgdn = is_down;
                                    break;
                                case SDLK_HOME:
                                    if (is_down && g_cam_active) cam_reset();
                                    break;
                            }
                            break;
                        }
                        // --- Text input events ---
                        case SDL_EVENT_TEXT_INPUT: {
                            const char* text = event.text.text;
                            if (g_text_input_active) {
                                // Game-driven text input: append to buffer and notify game
                                g_text_input_buffer += text;
                                call_text_input_did_change(env_ptr, g_text_input_buffer);
                                std::cout << "[TextInput] \"" << g_text_input_buffer << "\"" << std::endl;
                            } else if (g_typing_mode && g_fw_sendKeyChar) {
                                // Manual typing mode (F7): send through FWKeyboard
                                for (int ci = 0; text[ci] != '\0'; ci++) {
                                    uint32_t ch = (uint32_t)(unsigned char)text[ci];
                                    fw_send_char(ch, accumulated_time);
                                }
                            }
                            break;
                        }
                        // --- Multi-touch support (touchscreen laptops, up to 10 fingers) ---
                        case SDL_EVENT_FINGER_DOWN: {
                            // SDL finger coords are normalized 0.0-1.0, scale to game 960x544
                            float x = event.tfinger.x * 960.0f;
                            float y = (1.0f - event.tfinger.y) * 544.0f;  // flip Y
                            int finger_id = (int)(event.tfinger.fingerID % 10) + 20;  // IDs 20-29 for touch
                            extern bool g_sre_overlay_blocking;
                            if (!g_sre_overlay_blocking) {
                                call_handle_touch_event(handleTouchEvent, env_ptr, 0, 1, finger_id, accumulated_time, x, y, x, y, 1);
                            }
                            break;
                        }
                        case SDL_EVENT_FINGER_UP: {
                            float x = event.tfinger.x * 960.0f;
                            float y = (1.0f - event.tfinger.y) * 544.0f;
                            int finger_id = (int)(event.tfinger.fingerID % 10) + 20;
                            extern bool g_sre_overlay_blocking;
                            if (!g_sre_overlay_blocking) {
                                call_handle_touch_event(handleTouchEvent, env_ptr, 0, 2, finger_id, accumulated_time, x, y, x, y, 0);
                            }
                            break;
                        }
                        case SDL_EVENT_FINGER_MOTION: {
                            float x = event.tfinger.x * 960.0f;
                            float y = (1.0f - event.tfinger.y) * 544.0f;
                            float old_x = (event.tfinger.x - event.tfinger.dx) * 960.0f;
                            float old_y = (1.0f - (event.tfinger.y - event.tfinger.dy)) * 544.0f;
                            int finger_id = (int)(event.tfinger.fingerID % 10) + 20;
                            extern bool g_sre_overlay_blocking;
                            if (!g_sre_overlay_blocking) {
                                call_handle_touch_event(handleTouchEvent, env_ptr, 0, 4, finger_id, accumulated_time, x, y, old_x, old_y, 0);
                            }
                            break;
                        }
                        // --- Gamepad support ---
                        case SDL_EVENT_GAMEPAD_ADDED: {
                            if (!g_gamepad) {
                                g_gamepad = SDL_OpenGamepad(event.gdevice.which);
                                if (g_gamepad)
                                    std::cout << "[Input] Gamepad connected: " << SDL_GetGamepadName(g_gamepad) << std::endl;
                            }
                            break;
                        }
                        case SDL_EVENT_GAMEPAD_REMOVED: {
                            if (g_gamepad && event.gdevice.which == SDL_GetJoystickID(SDL_GetGamepadJoystick(g_gamepad))) {
                                SDL_CloseGamepad(g_gamepad);
                                g_gamepad = nullptr;
                                std::cout << "[Input] Gamepad disconnected" << std::endl;
                            }
                            break;
                        }
                        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                                float val = event.gaxis.value / 32768.0f;
                                if (g_cam_active) {
                                    cam_move_scaled(val, 0.0f, 0.0f, dt_seconds);
                                } else {
                                    extern bool g_sre_overlay_blocking;
                                    if (!g_sre_overlay_blocking) {
                                        bool new_left = (val < -0.2f);
                                        bool new_right = (val > 0.2f);
                                        
                                        if (key_left != new_left) {
                                            key_left = new_left;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0,
                                                key_left ? 1 : 2, 6, accumulated_time,
                                                60.0f, 94.0f, 60.0f, 94.0f, key_left ? 1 : 0);
                                        }
                                        if (key_right != new_right) {
                                            key_right = new_right;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0,
                                                key_right ? 1 : 2, 7, accumulated_time,
                                                155.0f, 94.0f, 155.0f, 94.0f, key_right ? 1 : 0);
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                            bool gp_down = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                            extern bool g_sre_overlay_blocking;
                            if (!g_sre_overlay_blocking) {
                                switch (event.gbutton.button) {
                                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                                        if (key_left != gp_down) {
                                            key_left = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 6, accumulated_time, 60.0f, 94.0f, 60.0f, 94.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                                        if (key_right != gp_down) {
                                            key_right = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 7, accumulated_time, 155.0f, 94.0f, 155.0f, 94.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                    case SDL_GAMEPAD_BUTTON_SOUTH:
                                        if (key_jump != gp_down) {
                                            key_jump = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 5, accumulated_time, 900.0f, 94.0f, 900.0f, 94.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                    case SDL_GAMEPAD_BUTTON_WEST:
                                        if (key_attack != gp_down) {
                                            key_attack = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 8, accumulated_time, 790.0f, 94.0f, 790.0f, 94.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                    case SDL_GAMEPAD_BUTTON_NORTH:
                                        if (key_magic != gp_down) {
                                            key_magic = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 10, accumulated_time, 900.0f, 184.0f, 900.0f, 184.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                    case SDL_GAMEPAD_BUTTON_EAST:
                                    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                                    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                                        if (key_use_item != gp_down) {
                                            key_use_item = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 12, accumulated_time, 425.0f, 54.0f, 425.0f, 54.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                    case SDL_GAMEPAD_BUTTON_START:
                                        if (key_menu != gp_down) {
                                            key_menu = gp_down;
                                            call_handle_touch_event(handleTouchEvent, env_ptr, 0, gp_down ? 1 : 2, 9, accumulated_time, 48.0f, 500.0f, 48.0f, 500.0f, gp_down ? 1 : 0);
                                        }
                                        break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
            
            // FPS counter
            fps_frame_count++;
            Uint64 fps_now = SDL_GetTicks();
            if (fps_now - fps_last_time >= 1000) {
                fps = fps_frame_count * 1000.0f / (fps_now - fps_last_time);
                fps_frame_count = 0;
                fps_last_time = fps_now;
            }
            
            completed_frames++;
            
            // Accumulate totals
            total_draw_calls += g_frame_stats.draw_calls;
            total_tex_binds += g_frame_stats.texture_binds;
            total_tex_uploads += g_frame_stats.tex_uploads;
            total_matrix_ops += g_frame_stats.matrix_ops;
            total_state_changes += g_frame_stats.state_changes;
            total_asset_opens += g_frame_stats.asset_opens;
            
            // Print stats periodically
            if (completed_frames == 1 || completed_frames == 10 || 
                completed_frames % 100 == 0) {
                std::cout << "[Frame " << completed_frames << "] "
                          << "draws=" << g_frame_stats.draw_calls
                          << " verts=" << g_frame_stats.vertices_submitted
                          << " tex_binds=" << g_frame_stats.texture_binds
                          << " tex_uploads=" << g_frame_stats.tex_uploads
                          << " vtx_calls=" << g_frame_stats.vertex_pointer_calls
                          << " texc_calls=" << g_frame_stats.texcoord_pointer_calls
                          << " matrix=" << g_frame_stats.matrix_ops
                          << " state=" << g_frame_stats.state_changes
                          << std::endl;
            }
            
            // In headless mode, stop after TARGET_FRAMES
            if (TARGET_FRAMES > 0 && completed_frames >= TARGET_FRAMES) running = false;
        }

        // Cleanup
        g_input_config.save(g_save_dir + "/controls.ini");
        if (g_gamepad) {
            SDL_CloseGamepad(g_gamepad);
            g_gamepad = nullptr;
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "[RESULT] Completed " << completed_frames << " frames" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  Total draw calls:      " << total_draw_calls << std::endl;
        std::cout << "  Total tex binds:       " << total_tex_binds << std::endl;
        std::cout << "  Total tex uploads:     " << total_tex_uploads << std::endl;
        std::cout << "  Total matrix ops:      " << total_matrix_ops << std::endl;
        std::cout << "  Total state changes:   " << total_state_changes << std::endl;
        std::cout << "  Total asset opens:     " << total_asset_opens << std::endl;
        std::cout << "  Unique textures seen:  " << g_frame_stats.unique_textures_bound << std::endl;
        std::cout << "  Avg draw/frame:        " << (completed_frames ? total_draw_calls / completed_frames : 0) << std::endl;
        std::cout << "  Avg tex_bind/frame:    " << (completed_frames ? total_tex_binds / completed_frames : 0) << std::endl;
        std::cout << "========================================" << std::endl;
        
        // Asset Proof Summary
        std::cout << "\n--- Asset Proof ---" << std::endl;
        std::cout << "  Menu scene:      " << (g_frame_stats.menu_scene_loaded ? "✓ LOADED" : "✗ missing") << std::endl;
        std::cout << "  Title texture:   " << (g_frame_stats.title_texture_loaded ? "✓ LOADED" : "✗ missing") << std::endl;
        std::cout << "  Common atlas:    " << (g_frame_stats.common_atlas_loaded ? "✓ LOADED" : "✗ missing") << std::endl;
        std::cout << "  Menu background: " << (g_frame_stats.menu_back_loaded ? "✓ LOADED" : "✗ missing") << std::endl;
        std::cout << "-------------------\n" << std::endl;
    }
}

static void copy_and_relocate(uint8_t* dest_cave, uint8_t* src_orig, uint64_t cave_vaddr, uint64_t orig_vaddr, int num_insns) {
    for (int i = 0; i < num_insns; i++) {
        uint32_t insn = *(uint32_t*)(src_orig + i * 4);
        uint64_t orig_pc = orig_vaddr + i * 4;
        uint64_t cave_pc = cave_vaddr + i * 4;
        
        // ADR or ADRP
        if ((insn & 0x9F000000) == 0x90000000) {
            bool is_adrp = (insn & 0x80000000) != 0;
            int32_t immlo = (insn >> 29) & 3;
            int32_t immhi = (insn >> 5) & 0x7FFFF;
            int32_t imm = (immhi << 2) | immlo;
            if (imm & 0x100000) {
                imm |= ~0x1FFFFF;
            }
            uint64_t target;
            if (is_adrp) {
                target = (orig_pc & ~0xFFFULL) + ((int64_t)imm << 12);
            } else {
                target = orig_pc + imm;
            }
            int64_t new_offset;
            if (is_adrp) {
                new_offset = (int64_t)target - (cave_pc & ~0xFFFULL);
                int64_t new_imm = new_offset >> 12;
                insn = (insn & 0x9F00001F) | (((new_imm & 3) << 29) | (((new_imm >> 2) & 0x7FFFF) << 5));
            } else {
                new_offset = (int64_t)target - (int64_t)cave_pc;
                insn = (insn & 0x9F00001F) | (((new_offset & 3) << 29) | (((new_offset >> 2) & 0x7FFFF) << 5));
            }
        }
        // B or BL
        else if ((insn & 0xFC000000) == 0x14000000 || (insn & 0xFC000000) == 0x94000000) {
            int32_t imm = insn & 0x3FFFFFF;
            if (imm & 0x2000000) {
                imm |= ~0x3FFFFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;
            insn = (insn & 0xFC000000) | (new_imm & 0x3FFFFFF);
        }
        // B.cond
        else if ((insn & 0xFF000010) == 0x54000000) {
            int32_t imm = (insn >> 5) & 0x7FFFF;
            if (imm & 0x40000) {
                imm |= ~0x7FFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;
            insn = (insn & 0xFF00001F) | ((new_imm & 0x7FFFF) << 5);
        }
        // CBZ or CBNZ
        else if ((insn & 0x7F000000) == 0x34000000 || (insn & 0x7F000000) == 0x35000000) {
            int32_t imm = (insn >> 5) & 0x7FFFF;
            if (imm & 0x40000) {
                imm |= ~0x7FFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;
            insn = (insn & 0xFF00001F) | ((new_imm & 0x7FFFF) << 5);
        }
        // TBZ or TBNZ
        else if ((insn & 0x7F000000) == 0x36000000 || (insn & 0x7F000000) == 0x37000000) {
            int32_t imm = (insn >> 5) & 0x3FFF;
            if (imm & 0x2000) {
                imm |= ~0x3FFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;
            insn = (insn & 0xFFF8001F) | ((new_imm & 0x3FFF) << 5);
        }
        // LDR literal
        else if ((insn & 0x3F000000) == 0x18000000) {
            int32_t imm = (insn >> 5) & 0x7FFFF;
            if (imm & 0x40000) {
                imm |= ~0x7FFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;
            insn = (insn & 0xFF00001F) | ((new_imm & 0x7FFFF) << 5);
        }
        
        *(uint32_t*)(dest_cave + i * 4) = insn;
    }
}

void load_and_boot_arm64() {
    std::string so_path = get_data_path(g_lib_name);
    std::cout << "[Loader64] Loading: " << so_path << std::endl;
    uint64_t load_addr = 0x1000000;
    
    // 1. Load ELF (ARM64)
    if (g_loader_64->load(&g_main_mod_64, so_path, load_addr) != 0) {
        std::cerr << "[Loader64] Failed to load SO" << std::endl;
        exit(1);
    }
    
    // 2. Internal Relocations
    g_loader_64->relocate(&g_main_mod_64);
    
    // 3. External Symbol Resolution (to Bridge addresses)
    g_loader_64->resolve_all_to_bridge(&g_main_mod_64, &g_bridge_64, GUEST_GLOBALS_BASE);
    
    std::cout << "[Loader64] Game binary ready (all symbols bridged)." << std::endl;

    // Diagnostic: find the symbol for address 0x1478ccc (offset 0x478ccc)
    std::cout << "[SRE DIAG] Finding closest symbol to 0x1478ccc..." << std::endl;
    Elf64_Sym* closest_sym = nullptr;
    uint64_t closest_diff = 0xffffffffffffffffULL;
    for (int i = 0; i < g_main_mod_64.num_dynsym; i++) {
        Elf64_Sym* sym = &g_main_mod_64.dynsym[i];
        if (sym->st_value == 0) continue;
        uint64_t addr = g_main_mod_64.base_addr + sym->st_value;
        if (0x1478ccc >= addr) {
            uint64_t diff = 0x1478ccc - addr;
            if (diff < closest_diff) {
                closest_diff = diff;
                closest_sym = sym;
            }
        }
    }
    if (closest_sym) {
        std::cout << "[SRE DIAG] Closest symbol to 0x1478ccc: '" 
                  << (g_main_mod_64.dynstr + closest_sym->st_name)
                  << "' @ 0x" << std::hex << (g_main_mod_64.base_addr + closest_sym->st_value)
                  << " (diff: +" << std::dec << closest_diff << " bytes)" << std::endl;
    }

    // Dump disassembly bytes at 0x1478ccc
    std::cout << "[SRE DIAG] Hex bytes at 0x1478ccc (offset 0x478ccc):" << std::endl;
    if (g_guest_memory) {
        for (int i = -16; i < 48; i++) {
            uint64_t addr = 0x1478ccc + i;
            uint8_t byte_val = g_guest_memory[0x1478ccc + i];
            if (i == 0) std::cout << "-> ";
            else std::cout << "   ";
            std::cout << "0x" << std::hex << addr << ": " 
                      << std::setw(2) << std::setfill('0') << (int)byte_val << std::dec << std::endl;
        }
    }

    // Initialize ARM64 Emulator AFTER memory is prepared
    // Multi-engine backend selection
    if (g_use_dynarmic) {
        g_emulator_64 = new EmulatorDynarmic64(g_guest_memory, GUEST_MEM_SIZE);
    } else {
        g_emulator_64 = new EmulatorArm64(g_guest_memory, GUEST_MEM_SIZE);
    }
    std::cout << "[Engine] Using " << g_emulator_64->engine_name() << " backend" << std::endl;
    g_emulator_64->set_bridge(&g_bridge_64);

    // --- Runtime binary patches (ARM64) ---
    // NO hardcoded address patches — they break when switching binary versions.
    // Only version-independent patches are applied:
    //   1. __cxa_guard_* (symbol lookup — works for any binary)
    //   2. STXR patcher (scans .text — works for any binary)
    //   3. .text protection (uses ELF loader text_size — works for any binary)
    //   4. Generic abort NOP (scans for bl abort patterns)
    {
        std::cerr << "\n======== [Patch64] APPLYING BINARY PATCHES ========" << std::endl;
        std::cout << "[Patch64] Version-independent patches starting..." << std::endl;
    }

    // =========================================================================
    // STXR/STLXR → STR patcher (eliminate atomic CAS spin loops)
    // =========================================================================
    // ARM64 uses LDXR/STXR for ALL atomics: shared_ptr refcount, mutexes, etc.
    // In single-threaded Unicorn, STXR may spin (exclusive monitor unreliable).
    // Fix: replace every STXR with STR (always succeeds) and fix the retry branch.
    {
        uint32_t NOP = 0xD503201F;
        std::cout << "[Patch64] STXR scan: text_base=0x" << std::hex << g_main_mod_64.text_base
                  << " text_size=0x" << g_main_mod_64.text_size 
                  << " (" << std::dec << (g_main_mod_64.text_size / 4) << " insns)" << std::endl;
        uint32_t* code = (uint32_t*)(g_guest_memory + g_main_mod_64.text_base);
        int num_insns = g_main_mod_64.text_size / 4;
        int patched = 0;
        int cbz_fixed = 0, cbnz_fixed = 0;
        
        for (int i = 0; i < num_insns; i++) {
            uint32_t insn = code[i];
            
            // Match STXR/STLXR/STXRB/STLXRB/STXRH/STLXRH:
            // Fixed bits: [29:24]=001000, [22]=0(store), [14:10]=11111
            if ((insn & 0x3FE07C00) == 0x08007C00) {
                uint32_t size = (insn >> 30) & 3;
                uint32_t Rt = insn & 0x1F;
                uint32_t Rn = (insn >> 5) & 0x1F;
                uint32_t Rs = (insn >> 16) & 0x1F;
                
                // Replace with size-appropriate STR (unsigned offset, imm=0)
                uint32_t str_insn;
                switch (size) {
                    case 0: str_insn = 0x39000000; break;  // STRB
                    case 1: str_insn = 0x79000000; break;  // STRH
                    case 2: str_insn = 0xB9000000; break;  // STR W
                    case 3: str_insn = 0xF9000000; break;  // STR X
                    default: continue;
                }
                code[i] = str_insn | (Rn << 5) | Rt;
                
                // Fix the following branch that checks the STXR status register
                if (i + 1 < num_insns) {
                    uint32_t next = code[i + 1];
                    // CBNZ Rs, retry → NOP (don't retry, store always succeeds)
                    if ((next & 0xFF000000) == 0x35000000 && (next & 0x1F) == Rs) {
                        code[i + 1] = NOP;
                        cbnz_fixed++;
                    }
                    // CBZ Rs, success → unconditional B (always branch to success)
                    else if ((next & 0xFF000000) == 0x34000000 && (next & 0x1F) == Rs) {
                        int32_t imm19 = (int32_t)((next >> 5) & 0x7FFFF);
                        if (imm19 & 0x40000) imm19 |= (int32_t)0xFFF80000;
                        code[i + 1] = 0x14000000 | (imm19 & 0x3FFFFFF);
                        cbz_fixed++;
                    }
                }
                patched++;
            }
        }
        std::cout << "[Patch64] STXR patcher: " << patched << " stores, "
                  << cbnz_fixed << " CBNZ→NOP, " << cbz_fixed << " CBZ→B" << std::endl;
    }

    // =========================================================================
    // PROTECT .text section from corruption
    // NOTE: Skip for RLSwordigo instances - they require writable text segments
    // for their modified game data, otherwise UC_ERR_WRITE_PROT crashes occur.
    // =========================================================================
    {
        const BinaryInfo* binfo = g_binary_selector.get_loaded_info();
        bool is_rlsw = (binfo && binfo->game_type == "RLSwordigo");
        
        if (!is_rlsw) {
            uint64_t text_start = g_main_mod_64.text_base;
            uint64_t text_size_rounded = (g_main_mod_64.text_size + 0xFFF) & ~0xFFF;
            g_emulator_64->protect_memory(text_start, text_size_rounded, UC_PROT_READ | UC_PROT_EXEC);
            std::cout << "[Patch64] Protected .text section (0x" << std::hex << text_start 
                      << "-0x" << (text_start + text_size_rounded) << std::dec << ")" << std::endl;
        } else {
            std::cout << "[Patch64] SKIPPED .text protection for RLSwordigo (requires writable text)" << std::endl;
        }
    }

    // --- Patch __cxa_guard_acquire/release/abort in the binary ---
    // Uses symbol lookup — works for any binary version.
    {
        uint64_t guard_acq = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "__cxa_guard_acquire");
        uint64_t guard_rel = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "__cxa_guard_release");
        uint64_t guard_abt = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "__cxa_guard_abort");
        
        if (guard_acq) {
            *(uint32_t*)(g_guest_memory + guard_acq + 0)  = 0x39400001; // LDRB W1, [X0]
            *(uint32_t*)(g_guest_memory + guard_acq + 4)  = 0x35000061; // CBNZ W1, #12
            *(uint32_t*)(g_guest_memory + guard_acq + 8)  = 0x52800020; // MOV W0, #1
            *(uint32_t*)(g_guest_memory + guard_acq + 12) = 0xD65F03C0; // RET
            *(uint32_t*)(g_guest_memory + guard_acq + 16) = 0x52800000; // MOV W0, #0
            *(uint32_t*)(g_guest_memory + guard_acq + 20) = 0xD65F03C0; // RET
            std::cout << "[Patch64] __cxa_guard_acquire at 0x" << std::hex << guard_acq 
                      << " → proper guard check" << std::dec << std::endl;
        }
        if (guard_rel) {
            *(uint32_t*)(g_guest_memory + guard_rel + 0) = 0x52800021; // MOV W1, #1
            *(uint32_t*)(g_guest_memory + guard_rel + 4) = 0x39000001; // STRB W1, [X0]
            *(uint32_t*)(g_guest_memory + guard_rel + 8) = 0xD65F03C0; // RET
            std::cout << "[Patch64] __cxa_guard_release at 0x" << std::hex << guard_rel 
                      << " → set guard + RET" << std::dec << std::endl;
        }
        if (guard_abt) {
            *(uint32_t*)(g_guest_memory + guard_abt) = 0xD65F03C0; // RET
            std::cout << "[Patch64] __cxa_guard_abort at 0x" << std::hex << guard_abt 
                      << " → RET" << std::dec << std::endl;
        }
    }

    // Debug: Check what's at the crash address 0x270
    {
        uint32_t code_at_270[4] = {0};
        memcpy(code_at_270, g_guest_memory + 0x270, 16);
        std::cout << "[Debug] Code at guest 0x270: " << std::hex 
                  << code_at_270[0] << " " << code_at_270[1] << " " << code_at_270[2] << " " << code_at_270[3]
                  << std::dec << std::endl;
        // Check first few GOT entries near where PLT points
        uint64_t got_sample[4] = {0};
        // The GOT is typically at the start of the DATA segment
        uint64_t got_vaddr = g_main_mod_64.data_bases.empty() ? 0 : g_main_mod_64.data_bases[0];
        if (got_vaddr > 0) {
            memcpy(got_sample, g_guest_memory + got_vaddr, 32);
            std::cout << "[Debug] First GOT entries at 0x" << std::hex << got_vaddr << ": "
                      << got_sample[0] << " " << got_sample[1] << " " << got_sample[2] << " " << got_sample[3]
                      << std::dec << std::endl;
        }
    }
    // (Duplicate STXR patcher removed — handled by patcher at line ~1745 above)


    // =========================================================================
    // libsre.so — Swordigo Runtime Engine (guest-side ARM64 library)
    // =========================================================================
    // Loads libsre.so into guest memory and installs hooks that redirect
    // problematic functions in libswordigo.so to clean C reimplementations.
    // This eliminates atomic spin loops (STXR), threading issues, and more.
    //
    // IMPORTANT: SRE hooks use hardcoded function offsets that are ONLY valid
    // for v1.4.12 ARM64. Loading SRE for other versions would crash.
    {
        bool sre_compatible = g_use_sre && g_is_arm64;
        
        if (!sre_compatible) {
            std::cerr << "\n============================================" << std::endl;
            if (!g_use_sre) {
                std::cerr << "  [SRE] Skipped — SRE disabled by user choice" << std::endl;
            } else {
                std::cerr << "  [SRE] Skipped — SRE only supports ARM64 instances" << std::endl;
            }
            std::cerr << "  Current binary: " << g_lib_name << std::endl;
            std::cerr << "============================================\n" << std::endl;
        } else {

        std::cerr << "\n============================================" << std::endl;
        std::cerr << "  [SRE] Swordigo Runtime Engine — Checking..." << std::endl;
        std::cerr << "============================================" << std::endl;

        // Derive libsre.so path from g_lib_name's directory
        // e.g. "engine/v1.4.12/arm64-v8a/libswordigo.so" → "engine/v1.4.12/arm64-v8a/libsre.so"
        std::string sre_rel = g_lib_name;
        auto slash = sre_rel.rfind('/');
        if (slash != std::string::npos) {
            sre_rel = sre_rel.substr(0, slash + 1) + "libsre.so";
        } else {
            sre_rel = "libsre.so";
        }
        std::string sre_path = get_data_path(sre_rel);
        std::cerr << "[SRE] Looking for: " << sre_rel << std::endl;
        std::cerr << "[SRE] Resolved to: " << (sre_path.empty() ? "(empty)" : sre_path) << std::endl;
        
        // Fallback: try current directory
        if (sre_path.empty() || access(sre_path.c_str(), F_OK) != 0) {
            if (access("libsre.so", F_OK) == 0) {
                sre_path = "libsre.so";
            }
        } else {
            // Sync current directory's libsre.so to the instance directory
            if (sre_path != "libsre.so" && access("libsre.so", F_OK) == 0) {
                try {
                    fs::copy_file("libsre.so", sre_path, fs::copy_options::overwrite_existing);
                    std::cout << "[SRE] Synchronized: copied ./libsre.so -> " << sre_path << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[SRE] Failed to sync ./libsre.so to " << sre_path << ": " << e.what() << std::endl;
                }
            }
        }

        if (!sre_path.empty() && access(sre_path.c_str(), F_OK) == 0) {
            std::cerr << "[SRE] FOUND: " << sre_path << std::endl;
            std::cout << "\n======== [SRE] Loading Swordigo Runtime Engine ========" << std::endl;
            std::cout << "[SRE] Path: " << sre_path << std::endl;

            remove("/home/quantumcreeper/SwordigoDesktop/sre_hook_debug.txt");
            // Load libsre.so at a separate guest address (after libswordigo.so)
            uint64_t sre_load_addr = 0x2000000;  // 32MB — well above swordigo's ~7MB

            int sre_ret = g_loader_64->load(&g_sre_mod, sre_path, sre_load_addr);
            if (sre_ret == 0) {
                std::cout << "[SRE] Loaded at 0x" << std::hex << sre_load_addr 
                          << " (text_size=0x" << g_sre_mod.text_size << ")" 
                          << std::dec << std::endl;

                // Relocate
                g_loader_64->relocate(&g_sre_mod);

                // Resolve imports (malloc, free, memcpy, strlen, etc.) through bridge
                g_loader_64->resolve_all_to_bridge(&g_sre_mod, &g_bridge_64, GUEST_GLOBALS_BASE);

                // ---- VFS pre-init ----
                // Write VFS path globals and activate BEFORE sre_init so that
                // sre_config_load_toml("/Assets/mini.toml") inside sre_init
                // can resolve MiniPaths correctly.
                {
                    uint8_t* gm = g_emulator_64->get_memory_base();

                    auto vfs_write_str = [&](const char* sym, const std::string& val, int maxlen) {
                        uint64_t a = g_loader_64->get_symbol_vaddr(&g_sre_mod, sym);
                        if (!a) return;
                        strncpy((char*)(gm + a), val.c_str(), maxlen - 1);
                        ((char*)(gm + a))[maxlen - 1] = '\0';
                    };
                    auto vfs_write_int = [&](const char* sym, int val) {
                        uint64_t a = g_loader_64->get_symbol_vaddr(&g_sre_mod, sym);
                        if (a) *(int*)(gm + a) = val;
                    };

                    std::string data_base = get_data_path("");
                    if (!data_base.empty() && data_base.back() == '/')
                        data_base.pop_back();

                    vfs_write_str("g_sre_vfs_path_assets",   get_data_path(g_assets_dir), 512);
                    vfs_write_str("g_sre_vfs_path_files",    g_save_dir,                  512);
                    vfs_write_str("g_sre_vfs_path_external", data_base + "/external",     512);
                    vfs_write_str("g_sre_vfs_path_cache",    g_cache_dir,                 512);
                    vfs_write_str("g_sre_vfs_data_dir",      data_base,                   512);
                    vfs_write_int("g_sre_vfs_active", 1);

                    // Scan mods/ for the first installed mod — activates 5-level hierarchy
                    std::string mods_dir = data_base + "/mods/";
                    if (fs::exists(mods_dir) && fs::is_directory(mods_dir)) {
                        for (const auto& me : fs::directory_iterator(mods_dir)) {
                            if (me.is_directory()) {
                                std::string mn = me.path().filename().string();
                                if (!mn.empty() && mn[0] != '.') {
                                    vfs_write_str("g_sre_vfs_mod_name", mn, 128);
                                    vfs_write_int("g_sre_vfs_hierarchy_enabled", 1);
                                    std::cout << "[SRE] VFS mod detected: " << mn << std::endl;
                                    set_active_mod_name(mn);
                                    break;
                                }
                            }
                        }
                    }
                    std::cout << "[SRE] VFS pre-init: active=1 data_dir=" << data_base << std::endl;
                }

                // Call sre_init(swordigo_base, empty_sentinel_bss_offset)
                uint64_t sre_init_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "sre_init");
                if (sre_init_addr) {
                    std::cout << "[SRE] Calling sre_init(0x" << std::hex << load_addr 
                              << ", 0x14880)" << std::dec << std::endl;
                    g_emulator_64->call(sre_init_addr, {load_addr, 0x14880});
                    std::cout << "[SRE] sre_init completed" << std::endl;

                    // DEBUG: Dump ITextInputDelegate vtable to verify fix
                    {
                        uint8_t* mem = g_emulator_64->get_memory_base();
                        uint64_t vtable_addr = load_addr + 0x7e1688;
                        uint64_t entry0 = *(uint64_t*)(mem + vtable_addr);
                        uint64_t entry1 = *(uint64_t*)(mem + vtable_addr + 8);
                        std::cout << "[DEBUG] ITextInputDelegate vtable at 0x" << std::hex << vtable_addr
                                  << ": [0]=0x" << entry0 << " [1]=0x" << entry1 
                                  << std::dec << std::endl;
                    }

                    // ========= Safety RET-page for wild vtable jumps =========
                    // Some vtable entries in GUITextFieldImpl (and possibly other
                    // classes) have unrelocated/corrupt pointers. Instead of
                    // chasing each individual entry, we fill the target page with
                    // ARM64 RET instructions. Any wild vtable dispatch to this
                    // address just returns cleanly to the caller.
                    //
                    // Known crash targets:
                    //   0x2d6ce4c — GUITextFieldImpl vtable dispatch during drawApplication
                    {
                        uint64_t crash_page = 0x2d6c000;  // 0x2d6ce4c aligned to 4KB
                        uint32_t ret_insn = 0xD65F03C0;   // ARM64 RET
                        
                        // Write directly to the shared guest memory buffer
                        // (works for both Unicorn and Dynarmic backends)
                        uint8_t* mem = g_emulator_64->get_memory_base();
                        for (uint64_t off = 0; off < 0x1000; off += 4) {
                            *(uint32_t*)(mem + crash_page + off) = ret_insn;
                        }
                        std::cout << "[Safety] Wrote RET-page at 0x" << std::hex 
                                  << crash_page << std::dec << std::endl;
                    }
                }

                // ========= Phase 1: Resolve Lua API symbols from libswordigo.so =========
                // The engine compiles Lua 5.1 with C++ linkage, so symbols are mangled.
                // Mangled names from SwMini's core/lua.c (ARM64 / __aarch64__).
                struct LuaSymEntry { const char* name; const char* mangled; };
                LuaSymEntry lua_syms[] = {
                    {"lua_pcall",     "_Z9lua_pcallP9lua_Stateiii"},
                    {"lua_resume",    "_Z10lua_resumeP9lua_Statei"},
                    {"lua_settop",    "_Z10lua_settopP9lua_Statei"},
                    {"lua_gettop",    "_Z10lua_gettopP9lua_State"},
                    {"lua_tolstring", "_Z13lua_tolstringP9lua_StateiPm"},
                    {"lua_call",      "_Z8lua_callP9lua_Stateii"},
                    {"lua_pushstring","_Z14lua_pushstringP9lua_StatePKc"},
                    {"lua_pushcclosure","_Z16lua_pushcclosureP9lua_StatePFiS0_Ei"},
                    {"lua_setfield",  "_Z12lua_setfieldP9lua_StateiPKc"},
                    {"lua_getfield",  "_Z12lua_getfieldP9lua_StateiPKc"},
                    {"lua_createtable","_Z15lua_createtableP9lua_Stateii"},
                    {"lua_pushnumber","_Z14lua_pushnumberP9lua_Stated"},
                    {"lua_pushboolean","_Z15lua_pushbooleanP9lua_Statei"},
                    {"lua_pushnil",   "_Z11lua_pushnilP9lua_State"},
                    {"lua_tonumber",  "_Z12lua_tonumberP9lua_Statei"},
                    {"lua_toboolean", "_Z13lua_tobooleanP9lua_Statei"},
                    {"lua_type",      "_Z8lua_typeP9lua_Statei"},
                    {"luaL_register", "_Z13luaL_registerP9lua_StatePKcPK8luaL_Reg"},
                    {"lua_touserdata","_Z14lua_touserdataP9lua_Statei"},
                    {"lua_pushlightuserdata","_Z21lua_pushlightuserdataP9lua_StatePv"},
                    {"lua_error",     "_Z9lua_errorP9lua_State"},
                    {"getSpeedMultiplier", "_ZNK5Caver11SceneObject21updateSpeedMultiplierEv"},
                };
                const int NUM_LUA_SYMS = sizeof(lua_syms) / sizeof(lua_syms[0]);
                
                // Allocate a struct in guest memory to pass addresses to sre_init_lua
                // SreLuaAddrs: 22 × uint64_t = 176 bytes
                uint64_t lua_addrs_guest = 0x48000;  // in our guest globals area
                uint64_t* lua_addrs = (uint64_t*)(g_guest_memory + lua_addrs_guest);
                
                int lua_resolved = 0;
                std::cout << "[SRE] Resolving " << NUM_LUA_SYMS << " Lua API symbols..." << std::endl;
                for (int i = 0; i < NUM_LUA_SYMS; i++) {
                    uint64_t addr = g_loader_64->get_symbol_vaddr(&g_main_mod_64, lua_syms[i].mangled);
                    lua_addrs[i] = addr;
                    if (addr) {
                        lua_resolved++;
                    } else {
                        std::cerr << "[SRE] WARNING: Lua symbol not found: " << lua_syms[i].name 
                                  << " (" << lua_syms[i].mangled << ")" << std::endl;
                    }
                }
                std::cout << "[SRE] Resolved " << lua_resolved << "/" << NUM_LUA_SYMS 
                          << " Lua symbols" << std::endl;
                
                // Call sre_init_lua() in libsre.so to pass the addresses
                uint64_t sre_init_lua_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "sre_init_lua");
                if (sre_init_lua_addr && lua_resolved > 0) {
                    g_emulator_64->call(sre_init_lua_addr, {lua_addrs_guest});
                    std::cout << "[SRE] Lua API initialized" << std::endl;

                    // ========= Phase 1b: Extended Lua API for standard libs =========
                    // Additional symbols needed by sre_lua_libs.c (math, table, etc.)
                    LuaSymEntry lua_ext_syms[] = {
                        {"lua_pushvalue",   "_Z13lua_pushvalueP9lua_Statei"},
                        {"lua_remove",      "_Z10lua_removeP9lua_Statei"},
                        {"lua_insert",      "_Z10lua_insertP9lua_Statei"},
                        {"lua_replace",     "_Z11lua_replaceP9lua_Statei"},
                        {"lua_checkstack",  "_Z14lua_checkstackP9lua_Statei"},
                        {"lua_rawget",      "_Z10lua_rawgetP9lua_Statei"},
                        {"lua_rawset",      "_Z10lua_rawsetP9lua_Statei"},
                        {"lua_rawgeti",     "_Z11lua_rawgetiP9lua_Stateii"},
                        {"lua_rawseti",     "_Z11lua_rawsetiP9lua_Stateii"},
                        {"lua_next",        "_Z8lua_nextP9lua_Statei"},
                        {"lua_objlen",      "_Z10lua_objlenP9lua_Statei"},
                        {"lua_settable",    "_Z12lua_settableP9lua_Statei"},
                        {"lua_gettable",    "_Z12lua_gettableP9lua_Statei"},
                        {"lua_isnumber",    "_Z12lua_isnumberP9lua_Statei"},
                        {"lua_isstring",    "_Z12lua_isstringP9lua_Statei"},
                        {"lua_tointeger",   "_Z13lua_tointegerP9lua_Statei"},
                        {"lua_pushinteger", "_Z15lua_pushintegerP9lua_Statel"},
                        {"lua_concat",      "_Z10lua_concatP9lua_Statei"},
                        {"lua_pushlstring", "_Z15lua_pushlstringP9lua_StatePKcm"},
                        {"lua_setmetatable","_Z16lua_setmetatableP9lua_Statei"},
                        {"lua_close",       "_Z9lua_closeP9lua_State"},
                        {"lua_dump",        "_Z8lua_dumpP9lua_StatePFiS0_PKvmPvES3_"},
                        {"lua_atpanic",     "_Z11lua_atpanicP9lua_StatePFiS0_E"},
                        {"lua_getmetatable","_Z16lua_getmetatableP9lua_Statei"},
                        {"lua_rawequal",    "_Z12lua_rawequalP9lua_Stateii"},
                        {"lua_equal",       "_Z9lua_equalP9lua_Stateii"},
                        {"lua_lessthan",    "_Z12lua_lessthanP9lua_Stateii"},
                        {"lua_isuserdata",  "_Z14lua_isuserdataP9lua_Statei"},
                    };
                    const int NUM_LUA_EXT_SYMS = sizeof(lua_ext_syms) / sizeof(lua_ext_syms[0]);

                    // SreLuaExtAddrs: 20 × uint64_t = 160 bytes
                    uint64_t lua_ext_addrs_guest = 0x48200;  // after lua_addrs
                    uint64_t* lua_ext_addrs = (uint64_t*)(g_guest_memory + lua_ext_addrs_guest);

                    int ext_resolved = 0;
                    for (int i = 0; i < NUM_LUA_EXT_SYMS; i++) {
                        uint64_t addr = g_loader_64->get_symbol_vaddr(&g_main_mod_64, lua_ext_syms[i].mangled);
                        lua_ext_addrs[i] = addr;
                        if (addr) ext_resolved++;
                        else std::cerr << "[SRE] WARNING: Ext Lua symbol not found: " << lua_ext_syms[i].name
                                       << " (" << lua_ext_syms[i].mangled << ")" << std::endl;
                    }
                    std::cout << "[SRE] Extended Lua API: " << ext_resolved << "/" << NUM_LUA_EXT_SYMS 
                              << " symbols resolved" << std::endl;

                    uint64_t sre_init_lua_ext_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "sre_init_lua_ext");
                    if (sre_init_lua_ext_addr && ext_resolved > 0) {
                        g_emulator_64->call(sre_init_lua_ext_addr, {lua_ext_addrs_guest});
                        std::cout << "[SRE] Extended Lua API initialized" << std::endl;
                    }
                    
                    // === Late-install lua_call → sre_lua_call_safe trampoline ===
                    // MUST be done AFTER sre_init_lua() so g_lua_pcall is populated.
                    // This wraps ALL lua_call invocations engine-wide with pcall.
                    // Fixes Wastelands crash: lua_call → error → panic → abort()
                    {
                        uint64_t lua_call_vaddr = g_loader_64->get_symbol_vaddr(
                            &g_main_mod_64, "_Z8lua_callP9lua_Stateii");
                        uint64_t safe_vaddr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, "sre_lua_call_safe");
                        if (lua_call_vaddr && safe_vaddr) {
                            // Write 16-byte trampoline: LDR X16, [PC,#8]; BR X16; .quad addr
                            uint32_t* code = (uint32_t*)(g_guest_memory + lua_call_vaddr);
                            code[0] = 0x58000050;  // LDR X16, [PC, #8]
                            code[1] = 0xD61F0200;  // BR X16
                            *(uint64_t*)(code + 2) = safe_vaddr;
                            std::cout << "[SRE] lua_call → sre_lua_call_safe @ 0x" 
                                      << std::hex << lua_call_vaddr << " → 0x" << safe_vaddr 
                                      << std::dec << " (LATE INSTALL — pcall ready)" << std::endl;
                        } else {
                            std::cerr << "[SRE] WARNING: Could not install lua_call hook"
                                      << " (lua_call=" << lua_call_vaddr 
                                      << " safe=" << safe_vaddr << ")" << std::endl;
                        }
                    }

                    // === Late-install lua_resume → sre_lua_resume_safe trampoline ===
                    // BL-scanning doesn't work because ARM64 PIC calls go through PLT.
                    // Instead: trampoline at lua_resume itself + relay stub for original.
                    //

                    {
                        uint64_t lua_resume_vaddr = g_loader_64->get_symbol_vaddr(
                            &g_main_mod_64, "_Z10lua_resumeP9lua_Statei");
                        uint64_t safe_resume_vaddr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, "sre_lua_resume_safe");
                        uint64_t g_lua_resume_addr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, "g_lua_resume");

                        if (lua_resume_vaddr && safe_resume_vaddr && g_lua_resume_addr) {
                            const uint64_t RELAY_CAVE = 0x3000000;  // unused guest memory

                            // Step 1: Copy and relocate original first 16 bytes to relay stub
                            uint8_t* orig = g_guest_memory + lua_resume_vaddr;
                            uint8_t* cave = g_guest_memory + RELAY_CAVE;
                            copy_and_relocate(cave, orig, RELAY_CAVE, lua_resume_vaddr, 4);

                            // Step 2: Append jump-back: LDR X16, [PC,#8]; BR X16; .quad dest
                            uint32_t* cave32 = (uint32_t*)(cave + 16);
                            cave32[0] = 0x58000050;  // LDR X16, [PC, #8]
                            cave32[1] = 0xD61F0200;  // BR X16
                            *(uint64_t*)(cave32 + 2) = lua_resume_vaddr + 16;

                            // Step 3: Update g_lua_resume to point to relay stub
                            *(uint64_t*)(g_guest_memory + g_lua_resume_addr) = RELAY_CAVE;

                            // Step 4: Write trampoline at lua_resume → sre_lua_resume_safe
                            uint32_t* code = (uint32_t*)(g_guest_memory + lua_resume_vaddr);
                            code[0] = 0x58000050;  // LDR X16, [PC, #8]
                            code[1] = 0xD61F0200;  // BR X16
                            *(uint64_t*)(code + 2) = safe_resume_vaddr;

                            std::cout << "[SRE] lua_resume trampoline installed:" << std::endl;
                            std::cout << "[SRE]   lua_resume @ 0x" << std::hex << lua_resume_vaddr
                                      << " → sre_lua_resume_safe @ 0x" << safe_resume_vaddr << std::endl;
                            std::cout << "[SRE]   relay stub @ 0x" << RELAY_CAVE
                                      << " → lua_resume+16 @ 0x" << (lua_resume_vaddr + 16)
                                      << std::dec << std::endl;

                            // Print first 4 original instructions for verification
                            uint32_t* saved = (uint32_t*)cave;
                            std::cout << "[SRE]   saved insns: "
                                      << std::hex << saved[0] << " " << saved[1] << " "
                                      << saved[2] << " " << saved[3] << std::dec << std::endl;
                        } else {
                            std::cerr << "[SRE] WARNING: lua_resume trampoline skipped"
                                      << " (resume=0x" << std::hex << lua_resume_vaddr
                                      << " safe=0x" << safe_resume_vaddr
                                      << " g_ptr=0x" << g_lua_resume_addr
                                      << std::dec << ")" << std::endl;
                        }
                    }
                }

                // ========= Resolve ProgramState symbols for hook targets =========
                // Map from SRE symbol name → libswordigo mangled symbol name
                struct SymHookMap { const char* sre_sym; const char* engine_sym; };
                SymHookMap sym_hooks[] = {
                    {"sre_ProgramState_Execute", "_ZN5Caver12ProgramState7ExecuteEi"},
                    {"sre_ProgramState_Resume",  "_ZN5Caver12ProgramState6ResumeEi"},
                    {"sre_ProgramState_Update",  "_ZN5Caver12ProgramState6UpdateEf"},
                    {"sre_updateApplication",    "Java_com_touchfoo_swordigo_Native_updateApplication"},
                    {"sre_CameraController_Update", "_ZN5Caver16CameraController6UpdateEf"},
                    {"sre_SceneGrid_UpdateVisibleAreasWithCamera", "_ZN5Caver9SceneGrid28UpdateVisibleAreasWithCameraEPNS_6CameraE"},
                    {"sre_GUIView_Update",             "_ZN5Caver7GUIView6UpdateEf"},
                    {"sre_AchievementsManager_Update", "_ZN5Caver19AchievementsManager6UpdateEf"},
                    {"sre_SetResourcesPath",           "_ZN5Caver16SetResourcesPathERKSs"},
                    {"sre_IsAndroidAssetsPath",        "_ZN5Caver19IsAndroidAssetsPathERKSs"},
                    // Core Lua functions
                    {"lua_pcall",     "_Z9lua_pcallP9lua_Stateiii"},
                    {"lua_resume",    "_Z10lua_resumeP9lua_Statei"},
                    {"lua_settop",    "_Z10lua_settopP9lua_Statei"},
                    {"lua_gettop",    "_Z10lua_gettopP9lua_State"},
                    {"lua_tolstring", "_Z13lua_tolstringP9lua_StateiPm"},
                    {"lua_call",      "_Z8lua_callP9lua_Stateii"},
                    {"lua_pushstring","_Z14lua_pushstringP9lua_StatePKc"},
                    {"lua_pushcclosure","_Z16lua_pushcclosureP9lua_StatePFiS0_Ei"},
                    {"lua_setfield",  "_Z12lua_setfieldP9lua_StateiPKc"},
                    {"lua_getfield",  "_Z12lua_getfieldP9lua_StateiPKc"},
                    {"lua_createtable","_Z15lua_createtableP9lua_Stateii"},
                    {"lua_pushnumber","_Z14lua_pushnumberP9lua_Stated"},
                    {"lua_pushboolean","_Z15lua_pushbooleanP9lua_Statei"},
                    {"lua_pushnil",   "_Z11lua_pushnilP9lua_State"},
                    {"lua_tonumber",  "_Z12lua_tonumberP9lua_Statei"},
                    {"lua_toboolean", "_Z13lua_tobooleanP9lua_Statei"},
                    {"lua_type",      "_Z8lua_typeP9lua_Statei"},
                    {"luaL_register", "_Z13luaL_registerP9lua_StatePKcPK8luaL_Reg"},
                    {"lua_touserdata","_Z14lua_touserdataP9lua_Statei"},
                    {"lua_pushlightuserdata","_Z21lua_pushlightuserdataP9lua_StatePv"},
                    {"lua_error",     "_Z9lua_errorP9lua_State"},
                    // Extended Lua functions
                    {"lua_pushvalue",   "_Z13lua_pushvalueP9lua_Statei"},
                    {"lua_remove",      "_Z10lua_removeP9lua_Statei"},
                    {"lua_insert",      "_Z10lua_insertP9lua_Statei"},
                    {"lua_replace",     "_Z11lua_replaceP9lua_Statei"},
                    {"lua_checkstack",  "_Z14lua_checkstackP9lua_Statei"},
                    {"lua_rawget",      "_Z10lua_rawgetP9lua_Statei"},
                    {"lua_rawset",      "_Z10lua_rawsetP9lua_Statei"},
                    {"lua_rawgeti",     "_Z11lua_rawgetiP9lua_Stateii"},
                    {"lua_rawseti",     "_Z11lua_rawsetiP9lua_Stateii"},
                    {"lua_next",        "_Z8lua_nextP9lua_Statei"},
                    {"lua_objlen",      "_Z10lua_objlenP9lua_Statei"},
                    {"lua_settable",    "_Z12lua_settableP9lua_Statei"},
                    {"lua_gettable",    "_Z12lua_gettableP9lua_Statei"},
                    {"lua_isnumber",    "_Z12lua_isnumberP9lua_Statei"},
                    {"lua_isstring",    "_Z12lua_isstringP9lua_Statei"},
                    {"lua_tointeger",   "_Z13lua_tointegerP9lua_Statei"},
                    {"lua_pushinteger", "_Z15lua_pushintegerP9lua_Statel"},
                    {"lua_concat",      "_Z10lua_concatP9lua_Statei"},
                    {"lua_pushlstring", "_Z15lua_pushlstringP9lua_StatePKcm"},
                    {"lua_setmetatable","_Z16lua_setmetatableP9lua_Statei"},
                    {"lua_close",       "_Z9lua_closeP9lua_State"},
                    {"lua_dump",        "_Z8lua_dumpP9lua_StatePFiS0_PKvmPvES3_"},
                    {"lua_atpanic",     "_Z11lua_atpanicP9lua_StatePFiS0_E"},
                    {"lua_getmetatable","_Z16lua_getmetatableP9lua_Statei"},
                    {"lua_rawequal",    "_Z12lua_rawequalP9lua_Stateii"},
                    {"lua_equal",       "_Z9lua_equalP9lua_Stateii"},
                    {"lua_lessthan",    "_Z12lua_lessthanP9lua_Stateii"},
                    {"lua_isuserdata",  "_Z14lua_isuserdataP9lua_Statei"},
                    // New standard Lua/LuaL functions
                    {"luaL_newstate",   "_Z13luaL_newstatev"},
                    {"luaL_loadstring", "_Z15luaL_loadstringP9lua_StatePKc"},
                    {"luaL_loadbuffer", "_Z15luaL_loadbufferP9lua_StatePKcmS2_"},
                    {"luaL_loadfile",   "_Z13luaL_loadfileP9lua_StatePKc"},
                    {"luaopen_base",    "_Z12luaopen_baseP9lua_State"},
                    {"luaopen_package", "_Z15luaopen_packageP9lua_State"},
                    {"luaopen_table",   "_Z13luaopen_tableP9lua_State"},
                    {"luaopen_io",      "_Z10luaopen_ioP9lua_State"},
                    {"luaopen_os",      "_Z10luaopen_osP9lua_State"},
                    {"luaopen_string",  "_Z14luaopen_stringP9lua_State"},
                    {"luaopen_math",    "_Z12luaopen_mathP9lua_State"},
                    {"luaopen_debug",   "_Z13luaopen_debugP9lua_State"},
                    {"luaL_openlibs",   "_Z13luaL_openlibsP9lua_State"},
                    {"lua_newthread",   "_Z13lua_newthreadP9lua_State"},
                    {"lua_tothread",    "_Z12lua_tothreadP9lua_Statei"},
                    {"lua_pushthread",  "_Z14lua_pushthreadP9lua_State"},
                    {"lua_status",      "_Z10lua_statusP9lua_State"},
                    {"lua_gc",          "_Z6lua_gcP9lua_Stateii"},
                };
                const int NUM_SYM_HOOKS = sizeof(sym_hooks) / sizeof(sym_hooks[0]);

                // Read the hook table from libsre.so and install trampolines
                uint64_t table_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "sre_hook_table");
                uint64_t count_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "sre_hook_count");

                if (table_addr && count_addr) {
                    int hook_count = *(int32_t*)(g_guest_memory + count_addr);
                    std::cout << "[SRE] Installing " << hook_count << " hooks..." << std::endl;

                    struct GuestHookEntry {
                        uint64_t target_offset;
                        uint64_t symbol_name_ptr;
                        uint64_t orig_func;      // reserved for future relay stubs
                    };

                    // Pre-save __cxa_throw's first 16 bytes BEFORE the hook table
                    // overwrites them with a trampoline. This relay stub lets
                    // sre_cxa_throw fall through to the original when depth==0.
                    const uint64_t CXA_THROW_OFFSET = 0x51e108;
                    const uint64_t CXA_RELAY = 0x3000040;  // code cave (after lua_resume relay)
                    uint64_t cxa_throw_vaddr = load_addr + CXA_THROW_OFFSET;
                    {
                        uint8_t* orig_cxa = g_guest_memory + cxa_throw_vaddr;
                        uint8_t* cave = g_guest_memory + CXA_RELAY;
                        copy_and_relocate(cave, orig_cxa, CXA_RELAY, cxa_throw_vaddr, 4);
                        uint32_t* cave32 = (uint32_t*)(cave + 16);
                        cave32[0] = 0x58000050;  // LDR X16, [PC, #8]
                        cave32[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(cave32 + 2) = cxa_throw_vaddr + 16;  // jump past trampoline

                        uint32_t* saved = (uint32_t*)cave;
                        std::cout << "[SRE] __cxa_throw relay pre-saved at 0x" << std::hex
                                  << CXA_RELAY << " (insns: " << saved[0] << " " << saved[1]
                                  << " " << saved[2] << " " << saved[3] << ")" 
                                  << std::dec << std::endl;
                    }

                    // Pre-save ProgramState::Update's first 16 bytes for relay stub.
                    // sre_ProgramState_Update handles the timer/resume safely, then
                    // calls g_orig_ProgramState_Update for the child iteration loop.
                    // (Same approach as SwKiwi: handle resume in hook, call original
                    // with isSuspended=0 so it skips the resume and only does children.)
                    const uint64_t UPDATE_RELAY = 0x3000080;  // code cave (after cxa_throw relay)
                    uint64_t update_vaddr = g_loader_64->get_symbol_vaddr(
                        &g_main_mod_64, "_ZN5Caver12ProgramState6UpdateEf");
                    if (update_vaddr) {
                        uint8_t* orig_update = g_guest_memory + update_vaddr;
                        uint8_t* cave = g_guest_memory + UPDATE_RELAY;
                        copy_and_relocate(cave, orig_update, UPDATE_RELAY, update_vaddr, 4);
                        uint32_t* cave32 = (uint32_t*)(cave + 16);
                        cave32[0] = 0x58000050;  // LDR X16, [PC, #8]
                        cave32[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(cave32 + 2) = update_vaddr + 16;

                    }

                    // Pre-save CameraController::Update's first 16 bytes for relay stub.
                    const uint64_t CAM_UPDATE_RELAY = 0x3000340;
                    uint64_t cam_update_vaddr = g_loader_64->get_symbol_vaddr(
                        &g_main_mod_64, "_ZN5Caver16CameraController6UpdateEf");
                    if (cam_update_vaddr) {
                        uint8_t* orig_cam_update = g_guest_memory + cam_update_vaddr;
                        uint8_t* cave = g_guest_memory + CAM_UPDATE_RELAY;
                        copy_and_relocate(cave, orig_cam_update, CAM_UPDATE_RELAY, cam_update_vaddr, 4);
                        uint32_t* cave32 = (uint32_t*)(cave + 16);
                        cave32[0] = 0x58000050;  // LDR X16, [PC, #8]
                        cave32[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(cave32 + 2) = cam_update_vaddr + 16;

                        uint64_t g_orig_cam_update_addr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, "g_orig_CameraController_Update");
                        if (g_orig_cam_update_addr) {
                            *(uint64_t*)(g_guest_memory + g_orig_cam_update_addr) = CAM_UPDATE_RELAY;
                            std::cout << "[SRE] g_orig_CameraController_Update → relay @ 0x"
                                      << std::hex << CAM_UPDATE_RELAY << std::dec << std::endl;
                        }
                    }

                    // Pre-save SceneGrid::UpdateVisibleAreasWithCamera's first 16 bytes for relay stub.
                    const uint64_t SCENEGRID_UPDATE_RELAY = 0x3000380;
                    uint64_t scenegrid_update_vaddr = g_loader_64->get_symbol_vaddr(
                        &g_main_mod_64, "_ZN5Caver9SceneGrid28UpdateVisibleAreasWithCameraEPNS_6CameraE");
                    if (scenegrid_update_vaddr) {
                        uint8_t* orig_sg_update = g_guest_memory + scenegrid_update_vaddr;
                        uint8_t* cave = g_guest_memory + SCENEGRID_UPDATE_RELAY;
                        copy_and_relocate(cave, orig_sg_update, SCENEGRID_UPDATE_RELAY, scenegrid_update_vaddr, 4);
                        uint32_t* cave32 = (uint32_t*)(cave + 16);
                        cave32[0] = 0x58000050;  // LDR X16, [PC, #8]
                        cave32[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(cave32 + 2) = scenegrid_update_vaddr + 16;

                        uint64_t g_orig_sg_update_addr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, "g_orig_SceneGrid_UpdateVisibleAreasWithCamera");
                        if (g_orig_sg_update_addr) {
                            *(uint64_t*)(g_guest_memory + g_orig_sg_update_addr) = SCENEGRID_UPDATE_RELAY;
                            std::cout << "[SRE] g_orig_SceneGrid_UpdateVisibleAreasWithCamera → relay @ 0x"
                                      << std::hex << SCENEGRID_UPDATE_RELAY << std::dec << std::endl;
                        }
                    }

                    // ========= GUI Relay Stubs (table-driven) =========
                    // For each hooked GUI function, save the original's first
                    // 16 bytes to a code cave so our hook can call-through.
                    // Adding a new GUI hook = one line here + one in sre_init.c.
                    struct GuiRelay {
                        uint64_t nm_offset;      // offset in libswordigo.so
                        const char* orig_sym;    // g_orig_* symbol in libsre.so
                        uint64_t cave_addr;      // code cave address
                    };
                    GuiRelay gui_relays[] = {
                        { 0x4a28bc, "g_orig_GUIWindow_DrawRect",    0x30000C0 },
                        { 0x49f310, "g_orig_GUIView_DrawRect",      0x3000100 },
                        { 0x49565c, "g_orig_GUIButton_DrawRect",    0x3000140 },
                        { 0x497aa0, "g_orig_GUILabel_DrawRect",     0x3000180 },
                        { 0x497658, "g_orig_GUIFrameView_DrawRect", 0x30001C0 },
                        { 0x491b54, "g_orig_GUIAlertView_DrawRect", 0x3000200 },
                        { 0x49cd40, "g_orig_GUISlider_DrawRect",    0x3000240 },
                        { 0x42bae4, "g_orig_NewMenuView_DrawRect",  0x3000280 },
                        { 0x393094, "g_orig_MainMenuView_ButtonPressed", 0x30002C0 },
                        { 0x2fc03c, "g_orig_RenderingContext_C1", 0x3000300 },
                        /* CreditsVC relays — DISABLED for v6, Options menu WIP.
                        { 0x38d604, "g_orig_CreditsVC_LoadView",         0x3000300 },
                        { 0x38d904, "g_orig_CreditsVC_ButtonPressed",    0x3000340 },
                        */
                    };
                    for (auto& r : gui_relays) {
                        uint64_t vaddr = g_main_mod_64.base_addr + r.nm_offset;
                        uint8_t* orig = g_guest_memory + vaddr;
                        uint8_t* cave = g_guest_memory + r.cave_addr;

                        // Save and relocate first 4 instructions (16 bytes)
                        copy_and_relocate(cave, orig, r.cave_addr, vaddr, 4);
                        // Append: LDR X16, [PC, #8]; BR X16; .quad target+16
                        uint32_t* c32 = (uint32_t*)(cave + 16);
                        c32[0] = 0x58000050;  // LDR X16, [PC, #8]
                        c32[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(c32 + 2) = vaddr + 16;

                        // Set g_orig_* in libsre.so
                        uint64_t orig_addr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, r.orig_sym);
                        if (orig_addr) {
                            *(uint64_t*)(g_guest_memory + orig_addr) = r.cave_addr;
                        }

                        std::cout << "[SRE] Relay: " << r.orig_sym
                                  << " @ 0x" << std::hex << r.cave_addr
                                  << std::dec << std::endl;
                    }

                    int installed = 0;
                    for (int i = 0; i < hook_count; i++) {
                        GuestHookEntry* entry = (GuestHookEntry*)(g_guest_memory + table_addr) + i;
                        uint64_t target_offset = entry->target_offset;
                        if (entry->symbol_name_ptr == 0) continue; // skip sentinel
                        const char* sym_name = (const char*)(g_guest_memory + entry->symbol_name_ptr);

                        // If target_offset is 0, resolve by symbol name from libswordigo.so
                        if (target_offset == 0) {
                            // Find matching engine symbol
                            bool found = false;
                            for (int j = 0; j < NUM_SYM_HOOKS; j++) {
                                if (strcmp(sym_name, sym_hooks[j].sre_sym) == 0) {
                                    uint64_t engine_vaddr = g_loader_64->get_symbol_vaddr(
                                        &g_main_mod_64, sym_hooks[j].engine_sym);
                                    if (engine_vaddr) {
                                        target_offset = engine_vaddr - load_addr;
                                        found = true;
                                    } else {
                                        std::cerr << "[SRE] WARNING: Engine symbol not found: " 
                                                  << sym_hooks[j].engine_sym << std::endl;
                                    }
                                    break;
                                }
                            }
                            if (!found || target_offset == 0) {
                                std::cerr << "[SRE] SKIP: No engine offset for " << sym_name << std::endl;
                                continue;
                            }
                        }

                        uint64_t target_addr = load_addr + target_offset;
                        entry->orig_func = target_addr;

                        // Look up the replacement function in libsre.so
                        uint64_t replacement = g_loader_64->get_symbol_vaddr(&g_sre_mod, sym_name);
                        {
                            FILE* f = fopen("/home/quantumcreeper/SwordigoDesktop/sre_hook_debug.txt", "a");
                            if (f) {
                                uint32_t* insns = (uint32_t*)(g_guest_memory + target_addr);
                                fprintf(f, "Hook: %s | target_addr=0x%lx | base_addr=0x%lx | replacement=0x%lx\n",
                                        sym_name, target_addr, g_sre_mod.base_addr, replacement);
                                fprintf(f, "  Original instructions: %08x %08x %08x %08x\n",
                                        insns[0], insns[1], insns[2], insns[3]);
                                fclose(f);
                            }
                        }
                        if (!replacement) {
                            continue;
                        }

                        // Write a 16-byte trampoline at the target address:
                        //   LDR X16, [PC, #8]    ; 0x58000050
                        //   BR  X16              ; 0xD61F0200
                        //   .quad replacement    ; 8-byte address
                        uint32_t* code = (uint32_t*)(g_guest_memory + target_addr);
                        code[0] = 0x58000050;  // LDR X16, [PC, #8]
                        code[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(code + 2) = replacement;

                        std::cout << "[SRE] Hook: 0x" << std::hex << target_offset 
                                  << " -> " << sym_name << " @ 0x" << replacement 
                                  << std::dec << std::endl;
                        installed++;
                    }

                    // Set g_original_cxa_throw to point to the relay stub
                    uint64_t g_orig_cxa_addr = g_loader_64->get_symbol_vaddr(
                        &g_sre_mod, "g_original_cxa_throw");
                    if (g_orig_cxa_addr) {
                        *(uint64_t*)(g_guest_memory + g_orig_cxa_addr) = CXA_RELAY;
                        std::cout << "[SRE] g_original_cxa_throw → relay @ 0x" 
                                  << std::hex << CXA_RELAY << std::dec << std::endl;
                    }

                    // Set g_orig_ProgramState_Update to point to the relay stub
                    // This lets sre_ProgramState_Update call the original for
                    // the child ProgramState iteration loop (entity AI, cleanup).
                    uint64_t g_orig_update_addr = g_loader_64->get_symbol_vaddr(
                        &g_sre_mod, "g_orig_ProgramState_Update");
                    if (g_orig_update_addr && update_vaddr) {
                        *(uint64_t*)(g_guest_memory + g_orig_update_addr) = UPDATE_RELAY;
                        std::cout << "[SRE] g_orig_ProgramState_Update → relay @ 0x"
                                  << std::hex << UPDATE_RELAY << std::dec << std::endl;
                    }

                    // Set g_OverlayTextComponent_Interface_addr in libsre.so
                    uint64_t g_ot_iface_sym_addr = g_loader_64->get_symbol_vaddr(
                        &g_sre_mod, "g_OverlayTextComponent_Interface_addr");
                    uint64_t ot_iface_engine_addr = g_loader_64->get_symbol_vaddr(
                        &g_main_mod_64, "_ZN5Caver20OverlayTextComponent9InterfaceEv");
                    if (g_ot_iface_sym_addr && ot_iface_engine_addr) {
                        *(uint64_t*)(g_guest_memory + g_ot_iface_sym_addr) = ot_iface_engine_addr;
                        std::cout << "[SRE] Resolved g_OverlayTextComponent_Interface_addr → 0x"
                                  << std::hex << ot_iface_engine_addr << std::dec << std::endl;
                    }

                    // Dynamic Interface Resolver for all components
                    const char* component_classes[] = {
                        "GlowComponent", "ManaComponent", "LightComponent", "ModelComponent",
                        "ShapeComponent", "SkillComponent", "SpellComponent", "SwingComponent",
                        "AttackComponent", "DamageComponent", "EntityComponent", "HealthComponent",
                        "PortalComponent", "ShadowComponent", "SpriteComponent", "OverlayComponent",
                        "ProgramComponent", "ShatterComponent", "ItemDropComponent", "ParticleComponent",
                        "AnimationComponent", "MagicBoltComponent", "MagicBombComponent", "TouchableComponent",
                        "TransformComponent", "WaterMeshComponent", "BackgroundComponent", "EntityInfoComponent",
                        "FireBreathComponent", "GroundMeshComponent", "HeroEntityComponent", "PropertiesComponent",
                        "SimpleGlowComponent", "SpawnPointComponent", "TextBubbleComponent", "WeaponGlowComponent",
                        "FireEmitterComponent", "OverlayTextComponent", "SoundEffectComponent", "WeaponTrailComponent",
                        "EntityActionComponent", "PortalEffectComponent", "ShadowVolumeComponent", "UtilityShapeComponent",
                        "GroundPolygonComponent", "HookshotTrailComponent", "MagicHookshotComponent", "MonsterEntityComponent",
                        "ParticleFieldComponent", "PhysicsObjectComponent", "BlendAnimationComponent", "BushControllerComponent",
                        "CharControllerComponent", "CollisionShapeComponent", "DimensionSpellComponent", "DoorControllerComponent",
                        "MagicExplosionComponent", "MagicSpellCastComponent", "ObjectModifierComponent", "ParticleObjectComponent",
                        "TextureMappingComponent", "BreakableObjectComponent", "CollectableItemComponent", "DimensionObjectComponent",
                        "OrbitControllerComponent", "ParticleEmitterComponent", "PhysicsPlatformComponent", "PressureTriggerComponent",
                        "SwingableWeaponComponent", "EntityControllerComponent", "KeyframeAnimationComponent", "MonsterControllerComponent",
                        "CharAnimControllerComponent", "ElevatorControllerComponent", "OverlayTargetArrowComponent", "RotatingBackgroundComponent",
                        "AnimationControllerComponent", "GroundMeshGeneratorComponent", "TransformControllerComponent", "BatMonsterControllerComponent",
                        "MagicParticleEmitterComponent", "ObjectLinkControllerComponent", "ProjectileControllerComponent", "MonsterDeathControllerComponent",
                        "SkellyMonsterControllerComponent", "StaticMonsterControllerComponent", "GenericMonsterControllerComponent", "LeapingMonsterControllerComponent",
                        "ModelTransformControllerComponent", "WalkingMonsterControllerComponent", "BouncingMonsterControllerComponent", "ChargingMonsterControllerComponent",
                        "ShootingMonsterControllerComponent", "SnappingMonsterControllerComponent", "SwingableWeaponControllerComponent", "ProjectileMonsterControllerComponent",
                        "BoneControlledCollisionShapeComponent"
                    };

                    int resolved_interfaces = 0;
                    for (const char* cls : component_classes) {
                        std::string sre_var_name = std::string(cls) + "_Interface";
                        std::string mangled_sym = "_ZN5Caver" + std::to_string(strlen(cls)) + cls + "9InterfaceEv";

                        uint64_t sre_var_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, sre_var_name.c_str());
                        uint64_t engine_fn_addr = g_loader_64->get_symbol_vaddr(&g_main_mod_64, mangled_sym.c_str());

                        if (sre_var_addr && engine_fn_addr) {
                            uint64_t iface_id_ptr = g_emulator_64->call(engine_fn_addr, {});
                            *(uint64_t*)(g_guest_memory + sre_var_addr) = iface_id_ptr;
                            resolved_interfaces++;
                        } else {
                            std::cerr << "[SRE-Resolver] Failed to find symbols for: " << cls << std::endl;
                        }
                    }
                    std::cout << "[SRE-Resolver] Dynamically resolved " << resolved_interfaces << " component interfaces." << std::endl;
 
                    // Resolve other global engine functions to SRE globals
                    struct DynamicFns {
                        const char* sre_var_name;
                        const char* engine_mangled;
                    };
                    const DynamicFns dynamic_fns[] = {
                        { "g_Camera_SetPerspectiveProjection", "_ZN5Caver6Camera24SetPerspectiveProjectionEffff" },
                        { "g_SceneObject_ComponentWithInterface", "_ZNK5Caver11SceneObject22ComponentWithInterfaceEl" },
                        { "g_ProgramState_FromLuaState", "_ZN5Caver12ProgramState12FromLuaStateEP9lua_State" },
                        { "g_sre_GameOverVC_DidContinue", "_ZN5Caver22GameOverViewController23GameOverViewDidContinueEPNS_12GameOverViewE" },
                        { "g_sre_TextBubble_SetHandleTouches", "_ZN5Caver19TextBubbleComponent16SetHandleTouchesEb" },
                        { "g_sre_TextBubble_IsTextFinishedShowing", "_ZN5Caver19TextBubbleComponent21IsTextFinishedShowingEv" },
                        { "g_sre_TextBubble_ShowText", "_ZN5Caver19TextBubbleComponent8ShowTextERKSsf" },
                        { "g_sre_HealthBar_SetMaxHealth", "_ZN5Caver9HealthBar12SetMaxHealthEi" },
                        { "g_sre_HealthBar_SetCurrentHealth", "_ZN5Caver9HealthBar16SetCurrentHealthEi" },
                        { "g_sre_ManaBar_SetMaxMana", "_ZN5Caver7ManaBar10SetMaxManaEi" },
                        { "g_sre_ManaBar_SetCurrentMana", "_ZN5Caver7ManaBar14SetCurrentManaEi" },
                        { "g_sre_CoinBar_SetCurrentCoins", "_ZN5Caver7CoinBar15SetCurrentCoinsEi" },
                        { "g_sre_GameOverlayView_SetControlsHidden", "_ZN5Caver15GameOverlayView17SetControlsHiddenEb" },
                        { "g_sre_GameOverlayView_SetShowsUseButton", "_ZN5Caver15GameOverlayView17SetShowsUseButtonEb" },
                        { "g_sre_GameOverlayView_SetSkillButtonDisabled", "_ZN5Caver15GameOverlayView22SetSkillButtonDisabledEb" },
                        { "g_sre_GUIEffect_FadeOut", "_ZN5Caver9GUIEffect7FadeOutEf" },
                        { "g_sre_GUIEffect_FadeIn", "_ZN5Caver9GUIEffect6FadeInEf" },
                        { "g_sre_GUIEffect_Update", "_ZN5Caver9GUIEffect6UpdateEf" },
                        { "g_sre_GUIView_Update", "_ZN5Caver7GUIView6UpdateEf" },
                        { "g_sre_CharController_CanUse", "_ZN5Caver23CharControllerComponent6CanUseEv" },
                        { "g_sre_CharController_CanPickup", "_ZN5Caver23CharControllerComponent9CanPickupEv" },
                        { "g_sre_CharController_StartMovingToDirection", "_ZN5Caver23CharControllerComponent22StartMovingToDirectionEi" },
                        { "g_sre_CharController_StopMovingToDirection", "_ZN5Caver23CharControllerComponent21StopMovingToDirectionEi" },
                        { "g_sre_CharController_StartJumping", "_ZN5Caver23CharControllerComponent12StartJumpingEv" },
                        { "g_sre_CharController_StopJumping", "_ZN5Caver23CharControllerComponent11StopJumpingEv" },
                        { "g_sre_CharController_CanJump", "_ZN5Caver23CharControllerComponent7CanJumpEv" },
                        { "g_sre_CharController_CanDoSomething", "_ZN5Caver23CharControllerComponent14CanDoSomethingEv" },
                        { "g_sre_CharController_CanBeginCasting", "_ZN5Caver23CharControllerComponent15CanBeginCastingEv" },
                        { "g_sre_CharController_Use", "_ZN5Caver23CharControllerComponent3UseEv" },
                        { "g_sre_CharController_Die", "_ZN5Caver23CharControllerComponent3DieEv" },
                        { "g_sre_CharController_Hurt", "_ZN5Caver23CharControllerComponent4HurtEv" },
                        { "g_sre_CharController_Swing", "_ZN5Caver23CharControllerComponent5SwingEv" },
                        { "g_sre_CharController_StopSwing", "_ZN5Caver23CharControllerComponent9StopSwingEv" },
                        { "g_sre_CharController_DropQuickly", "_ZN5Caver23CharControllerComponent11DropQuicklyEv" },
                        { "g_sre_CharController_CancelCasting", "_ZN5Caver23CharControllerComponent13CancelCastingEv" },
                        { "g_sre_CharController_FinishCasting", "_ZN5Caver23CharControllerComponent13FinishCastingEv" },
                        { "g_sre_CharController_CanSwing", "_ZN5Caver23CharControllerComponent8CanSwingEv" },
                        { "g_sre_GameSceneController_CanCastSkill", "_ZN5Caver19GameSceneController12CanCastSkillERKN5boost10shared_ptrINS_5SkillEEE" },
                        { "g_sre_GameSceneView_HideCinematicSkipButton", "_ZN5Caver13GameSceneView23HideCinematicSkipButtonEb" }
                    };

                    int fn_resolved = 0;
                    for (const auto& fn : dynamic_fns) {
                        uint64_t sre_var_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, fn.sre_var_name);
                        uint64_t engine_fn_addr = g_loader_64->get_symbol_vaddr(&g_main_mod_64, fn.engine_mangled);
                        if (sre_var_addr && engine_fn_addr) {
                            *(uint64_t*)(g_guest_memory + sre_var_addr) = engine_fn_addr;
                            fn_resolved++;
                        } else {
                            std::cerr << "[SRE-Resolver] Failed to resolve fn: " << fn.sre_var_name << std::endl;
                        }
                    }
                    std::cout << "[SRE-Resolver] Dynamically resolved " << fn_resolved << " global engine functions." << std::endl;

                    // Set up updateApplication relay stub for safe call-through
                    const uint64_t UPDATE_APP_RELAY = 0x3000300;
                    uint64_t update_app_vaddr = g_loader_64->get_symbol_vaddr(
                        &g_main_mod_64, "Java_com_touchfoo_swordigo_Native_updateApplication");
                    if (update_app_vaddr) {
                        uint8_t* orig_update_app = g_guest_memory + update_app_vaddr;
                        uint8_t* cave = g_guest_memory + UPDATE_APP_RELAY;
                        copy_and_relocate(cave, orig_update_app, UPDATE_APP_RELAY, update_app_vaddr, 4);
                        uint32_t* cave32 = (uint32_t*)(cave + 16);
                        cave32[0] = 0x58000050;  // LDR X16, [PC, #8]
                        cave32[1] = 0xD61F0200;  // BR X16
                        *(uint64_t*)(cave32 + 2) = update_app_vaddr + 16;
                        
                        uint64_t g_orig_update_app_addr = g_loader_64->get_symbol_vaddr(
                            &g_sre_mod, "g_orig_updateApplication");
                        if (g_orig_update_app_addr) {
                            *(uint64_t*)(g_guest_memory + g_orig_update_app_addr) = UPDATE_APP_RELAY;
                            std::cout << "[SRE] g_orig_updateApplication → relay @ 0x"
                                      << std::hex << UPDATE_APP_RELAY << std::dec << std::endl;
                        }
                    }

                    std::cout << "[SRE] Installed " << installed << "/" << hook_count 
                              << " hooks successfully" << std::endl;
                } else {
                    std::cerr << "[SRE] WARNING: Could not find hook table symbols" << std::endl;
                }

                // ========= Lua Console Setup =========
                // Console uses Lua's own loadstring() via pcall — no luaL_loadbuffer needed.
                // lua_getfield is resolved above as part of the Lua API symbols.
                
                // Store guest addresses of console buffers for main loop access
                g_lua_console_buf_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_lua_console_buf");
                g_lua_console_result_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_lua_console_result");
                g_lua_console_pending_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_lua_console_pending");
                g_lua_console_status_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_lua_console_status");
                
                std::cout << "[SRE] Console addrs: buf=0x" << std::hex << g_lua_console_buf_addr
                          << " result=0x" << g_lua_console_result_addr 
                          << " pending=0x" << g_lua_console_pending_addr
                          << " status=0x" << g_lua_console_status_addr 
                          << std::dec << std::endl;
                
                g_lua_console_print_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_lua_console_print_buf");

                if (g_lua_console_buf_addr) {
                    g_lua_console_ready = true;
                    // Initialize the ImGui console in SwordfareGUI
                    g_swordfare_gui.init_lua_console(
                        g_guest_memory,
                        g_lua_console_buf_addr,
                        g_lua_console_result_addr,
                        g_lua_console_pending_addr,
                        g_lua_console_status_addr,
                        g_lua_console_print_addr);
                    std::cout << "[SRE] Lua console ready — backtick (`) to open ImGui terminal" << std::endl;
                }

                // Resolve lua_resume error monitoring symbols
                g_sre_resume_err_count_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_resume_err_count");
                g_sre_resume_last_err_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_resume_last_err");
                if (g_sre_resume_err_count_addr) {
                    std::cout << "[SRE] lua_resume error monitoring active"
                              << " (count=0x" << std::hex << g_sre_resume_err_count_addr
                              << " msg=0x" << g_sre_resume_last_err_addr
                              << std::dec << ")" << std::endl;
                }

                // Resolve __cxa_throw caller diagnostic symbols
                g_sre_cxa_caller_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_cxa_throw_caller");
                g_sre_cxa_unrecovered_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_cxa_throw_unrecovered");
                if (g_sre_cxa_caller_addr) {
                    std::cout << "[SRE] __cxa_throw caller diagnostics active"
                              << " (caller=0x" << std::hex << g_sre_cxa_caller_addr
                              << " count=0x" << g_sre_cxa_unrecovered_addr
                              << std::dec << ")" << std::endl;
                }

                // Resolve lua_call_safe error monitoring symbols
                g_sre_lua_error_buf_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_last_lua_error");
                g_sre_lua_error_count_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_lua_error_count");
                if (g_sre_lua_error_count_addr) {
                    std::cout << "[SRE] lua_call_safe error monitoring active"
                              << " (buf=0x" << std::hex << g_sre_lua_error_buf_addr
                              << " count=0x" << g_sre_lua_error_count_addr
                              << std::dec << ")" << std::endl;
                }

                // Resolve achievement popup symbols
                {
                    uint64_t ach_pending = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_achievement_pending");
                    uint64_t ach_title = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_achievement_pending_title");
                    uint64_t ach_desc = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_achievement_pending_desc");
                    if (ach_pending && ach_title && ach_desc) {
                        mod_achievement_set_offsets(ach_pending, ach_title, ach_desc);
                        std::cout << "[SRE] Achievement popup active (pending=0x" << std::hex 
                                  << ach_pending << ")" << std::dec << std::endl;
                    }
                }

                // ========= Background Renderer Setup =========
                // Our SRE re-implements BackgroundComponent::Draw using
                // engine building blocks: SetMatrix, SetColor, Sprite::Draw
                uint64_t sre_init_bg_addr = g_loader_64->get_symbol_vaddr(
                    &g_sre_mod, "sre_init_background");
                if (sre_init_bg_addr) {
                    // SreBgAddrs struct: 4 × uint64_t = 32 bytes
                    uint64_t bg_addrs_guest = 0x48100;  // in our guest globals area
                    uint64_t* bg_addrs = (uint64_t*)(g_guest_memory + bg_addrs_guest);
                    
                    // Addresses from: nm -D libswordigo.so | grep RenderingContext/Sprite
                    bg_addrs[0] = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
                        "_ZN5Caver16RenderingContext9SetMatrixERKNS_7Matrix4E");
                    bg_addrs[1] = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
                        "_ZN5Caver16RenderingContext8SetColorERKNS_5ColorE");
                    bg_addrs[2] = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
                        "_ZNK5Caver6Sprite4DrawEPNS_16RenderingContextE");
                    // glDepthMask PLT entry: 0x1fb820 (from objdump -d -j .plt)
                    bg_addrs[3] = g_main_mod_64.base_addr + 0x1fb820;
                    
                    g_emulator_64->call(sre_init_bg_addr, {bg_addrs_guest});
                    
                    std::cout << "[SRE] Background renderer: SetMatrix=0x" << std::hex << bg_addrs[0]
                              << " SetColor=0x" << bg_addrs[1]
                              << " Sprite::Draw=0x" << bg_addrs[2] 
                              << " glDepthMask=0x" << bg_addrs[3]
                              << std::dec << std::endl;
                }
                
                // ========= GUI Subsystem Setup =========
                // Resolve SRE GUI init and pass all engine GUI function addresses
                uint64_t sre_init_gui_addr = g_loader_64->get_symbol_vaddr(
                    &g_sre_mod, "sre_init_gui");
                if (sre_init_gui_addr) {
                    // SreGuiAddrs struct: 49 × uint64_t = 392 bytes
                    uint64_t gui_addrs_guest = 0x48400;  // after lua_ext_addrs (0x48200 + 152 = 0x48298)
                    uint64_t* ga = (uint64_t*)(g_guest_memory + gui_addrs_guest);
                    uint64_t B = g_main_mod_64.base_addr;  // base for brevity
                    
                    // [0-12] RenderingContext
                    ga[0]  = B + 0x48b184;  // SetProjectionMatrix
                    ga[1]  = B + 0x48c6a0;  // FillRect(rect, color, alpha)
                    ga[2]  = B + 0x48c9f8;  // DrawRect(rect, color, alpha)
                    ga[3]  = B + 0x48b8d8;  // SetBlendingEnabled
                    ga[4]  = B + 0x48b900;  // SetTexturingEnabled
                    ga[5]  = B + 0x48b934;  // SetLightingEnabled
                    ga[6]  = B + 0x48b990;  // SetDepthTestEnabled
                    ga[7]  = B + 0x48b9b8;  // SetDepthWriteEnabled
                    ga[8]  = B + 0x48b4bc;  // SetAlpha
                    ga[9]  = B + 0x48bfa4;  // BindTexture
                    ga[10] = B + 0x48c330;  // DrawArrays
                    ga[11] = B + 0x48bcec;  // UseProgram
                    ga[12] = B + 0x48c2bc;  // SetVertexAttribPointer
                    ga[13] = 0;             // reserved
                    
                    // [14-19] FontText
                    ga[14] = B + 0x4c66dc;  // Draw
                    ga[15] = B + 0x4c53e4;  // AddText(str, fontSize, pos)
                    ga[16] = B + 0x4c5124;  // Clear
                    ga[17] = B + 0x4c5170;  // SetColor
                    ga[18] = B + 0x4c6210;  // Translate
                    ga[19] = B + 0x4c6280;  // AlignHorizontally
                    
                    // [20-22] GUILabel
                    ga[20] = B + 0x497fdc;  // AddText(str)
                    ga[21] = B + 0x4980fc;  // AddText(str, color)
                    ga[22] = B + 0x497b54;  // UpdateText
                    
                    // [23-26] GUIButton
                    ga[23] = B + 0x4950a4;  // Constructor(type)
                    ga[24] = B + 0x495a74;  // SetTitle(str)
                    ga[25] = B + 0x495b00;  // SetImage(str)
                    ga[26] = B + 0x33f9e4;  // titleLabel()
                    
                    // [27-29] GUIView
                    ga[27] = B + 0x49f5c8;  // SetFrame(rect)
                    ga[28] = B + 0x49f6ec;  // AddSubview(shared_ptr)
                    ga[29] = B + 0x49fa60;  // RemoveFromSuperview
                    
                    // [30-33] GUIWindow
                    ga[30] = B + 0x4a21ec;  // mainWindow()
                    ga[31] = B + 0x4a31b4;  // PresentModalView(shared_ptr, bool)
                    ga[32] = B + 0x4a3540;  // DismissModalView(view)
                    ga[33] = B + 0x4a3590;  // Dismiss
                    
                    // [34-36] GUIAlertView
                    ga[34] = B + 0x490628;  // SetTitle(str)
                    ga[35] = B + 0x490850;  // SetMessage(str)
                    ga[36] = B + 0x490bf4;  // AddButton(shared_ptr)
                    
                    // [37-43] GameInterfaceBuilder
                    ga[37] = B + 0x33ea24;  // NormalLabel(str, color, color)
                    ga[38] = B + 0x33ee2c;  // SmallLabel(str, color, color)
                    ga[39] = B + 0x33ff98;  // FramedButtonWithTitle(str, bool)
                    ga[40] = B + 0x340504;  // MainMenuButtonWithTitle(str)
                    ga[41] = B + 0x3408e0;  // AlertView(str, str, int, str*, int)
                    ga[42] = B + 0x341108;  // Slider(float, float)
                    ga[43] = B + 0x34188c;  // Switch()
                    
                    // [44-45] TextureLibrary
                    ga[44] = B + 0x4cc308;  // sharedLibrary()
                    ga[45] = B + 0x4cc494;  // TextureForName(str, bool)
                    
                    // [46-48] FontLibrary
                    ga[46] = B + 0x4c33b0;  // sharedLibrary()
                    ga[47] = B + 0x4c4364;  // DefaultFont()
                    ga[48] = B + 0x4c44a8;  // SmallDefaultFont()
                    
                    g_emulator_64->call(sre_init_gui_addr, {gui_addrs_guest});
                    std::cout << "[SRE] GUI subsystem initialized (49 functions resolved)"
                              << std::endl;
                }
                
                // === Native GUI init (GUIRoundedRect, GUITexturedRect, GL) ===
                uint64_t sre_init_gui_native_addr = g_loader_64->get_symbol_vaddr(
                    &g_sre_mod, "sre_init_gui_native");
                if (sre_init_gui_native_addr) {
                    // SreGuiNativeAddrs: 6 x uint64_t = 48 bytes
                    uint64_t native_addrs_guest = 0x3001000;
                    uint64_t* na = (uint64_t*)(g_guest_memory + native_addrs_guest);
                    uint64_t B = g_main_mod_64.base_addr;
                    na[0] = B + 0x4a9c2c;  // GUIRoundedRect::Draw
                    na[1] = B + 0x4a9bdc;  // GUIRoundedRect::SetColor
                    na[2] = B + 0x4ab308;  // GUITexturedRect::Draw
                    na[3] = 0;  // glEnable — PLT TBD, scissor optional
                    na[4] = 0;  // glDisable — PLT TBD, scissor optional
                    na[5] = 0;  // glScissor — PLT TBD, scissor optional
                    g_emulator_64->call(sre_init_gui_native_addr, {native_addrs_guest});
                    std::cout << "[SRE] Native GUI renderer initialized (3 draw + 0 GL)"
                              << std::endl;
                }

                // =============================================================
                // MOD SYSTEM: Read mod configs and pass to SRE
                // =============================================================
                {
                    uint64_t sre_init_mods_addr = g_loader_64->get_symbol_vaddr(
                        &g_sre_mod, "sre_init_mods");
                    
                    // Guest address for mod config block (0x49000, 8KB)
                    const uint64_t MOD_CONFIG_GUEST = 0x49000;
                    const uint32_t MOD_MAGIC   = 0x4D4F4453; // "MODS"
                    const uint32_t MOD_VERSION = 1;
                    
                    // Clear config block
                    memset(g_guest_memory + MOD_CONFIG_GUEST, 0, 0x2000);
                    
                    // Scan mods directory
                    std::string mods_dir = get_user_data_dir() + "/mods";
                    uint32_t music_count = 0;
                    uint32_t scene_count = 0;
                    uint32_t bg_count = 0;
                    uint32_t total_mods = 0;
                    
                    if (std::filesystem::exists(mods_dir)) {
                        for (auto& entry : std::filesystem::directory_iterator(mods_dir)) {
                            if (!entry.is_directory()) continue;
                            // Skip disabled mods (dot-prefixed folders)
                            std::string dirname = entry.path().filename().string();
                            if (!dirname.empty() && dirname[0] == '.') continue;
                            std::string mod_json_path = entry.path().string() + "/mod.json";
                            if (!std::filesystem::exists(mod_json_path)) continue;
                            
                            // Read mod.json
                            std::ifstream f(mod_json_path);
                            if (!f.is_open()) continue;
                            std::string content((std::istreambuf_iterator<char>(f)),
                                                std::istreambuf_iterator<char>());
                            f.close();
                            
                            // Check if mod is enabled (default: enabled)
                            // For now, all mods in the directory are active
                            total_mods++;
                            
                            // Simple JSON parser: find "type" field
                            auto json_get = [&](const std::string& key) -> std::string {
                                std::string search = "\"" + key + "\"";
                                size_t pos = content.find(search);
                                if (pos == std::string::npos) return "";
                                pos = content.find("\"", pos + search.length() + 1);
                                if (pos == std::string::npos) return "";
                                size_t end = content.find("\"", pos + 1);
                                if (end == std::string::npos) return "";
                                return content.substr(pos + 1, end - pos - 1);
                            };
                            
                            std::string mod_type = json_get("type");
                            std::string mod_name = json_get("name");
                            
                            if (mod_type == "music") {
                                // Parse music replacements from "replace" object
                                // Simple: find "replace" block, extract key:value pairs
                                size_t repl_pos = content.find("\"replace\"");
                                if (repl_pos != std::string::npos) {
                                    size_t brace = content.find("{", repl_pos);
                                    size_t end_brace = content.find("}", brace);
                                    if (brace != std::string::npos && end_brace != std::string::npos) {
                                        std::string block = content.substr(brace + 1, end_brace - brace - 1);
                                        // Parse key:value pairs
                                        size_t scan = 0;
                                        while (scan < block.size() && music_count < 32) {
                                            size_t ks = block.find("\"", scan);
                                            if (ks == std::string::npos) break;
                                            size_t ke = block.find("\"", ks + 1);
                                            if (ke == std::string::npos) break;
                                            size_t vs = block.find("\"", ke + 1);
                                            if (vs == std::string::npos) break;
                                            size_t ve = block.find("\"", vs + 1);
                                            if (ve == std::string::npos) break;
                                            
                                            std::string orig = block.substr(ks + 1, ke - ks - 1);
                                            std::string repl = block.substr(vs + 1, ve - vs - 1);
                                            
                                            // Write to guest memory (music entries at 0x40, 64 bytes each)
                                            uint64_t entry_addr = MOD_CONFIG_GUEST + 0x40 + music_count * 64;
                                            strncpy((char*)(g_guest_memory + entry_addr), orig.c_str(), 31);
                                            strncpy((char*)(g_guest_memory + entry_addr + 32), repl.c_str(), 31);
                                            music_count++;
                                            
                                            scan = ve + 1;
                                        }
                                    }
                                }
                                std::cout << "[MOD] Loaded music mod: " << mod_name 
                                          << " (" << music_count << " replacements)" << std::endl;
                            } else if (mod_type == "scene" || mod_type == "asset") {
                                // Parse scene/asset replacements from "replace" object
                                size_t repl_pos = content.find("\"replace\"");
                                if (repl_pos != std::string::npos) {
                                    size_t brace = content.find("{", repl_pos);
                                    size_t end_brace = content.find("}", brace);
                                    if (brace != std::string::npos && end_brace != std::string::npos) {
                                        std::string block = content.substr(brace + 1, end_brace - brace - 1);
                                        size_t scan = 0;
                                        while (scan < block.size() && scene_count < 16) {
                                            size_t ks = block.find("\"", scan);
                                            if (ks == std::string::npos) break;
                                            size_t ke = block.find("\"", ks + 1);
                                            if (ke == std::string::npos) break;
                                            size_t vs = block.find("\"", ke + 1);
                                            if (vs == std::string::npos) break;
                                            size_t ve = block.find("\"", vs + 1);
                                            if (ve == std::string::npos) break;
                                            
                                            std::string orig = block.substr(ks + 1, ke - ks - 1);
                                            std::string repl = block.substr(vs + 1, ve - vs - 1);
                                            
                                            // Write to guest memory (scene entries at 0x840, 128 bytes each)
                                            uint64_t entry_addr = MOD_CONFIG_GUEST + 0x840 + scene_count * 128;
                                            strncpy((char*)(g_guest_memory + entry_addr), orig.c_str(), 63);
                                            strncpy((char*)(g_guest_memory + entry_addr + 64), repl.c_str(), 63);
                                            scene_count++;
                                            
                                            scan = ve + 1;
                                        }
                                    }
                                }
                                std::cout << "[MOD] Loaded scene mod: " << mod_name 
                                          << " (" << scene_count << " replacements)" << std::endl;
                            } else if (mod_type == "background") {
                                // Parse background replacements from "replace" object
                                size_t repl_pos = content.find("\"replace\"");
                                if (repl_pos != std::string::npos) {
                                    size_t brace = content.find("{", repl_pos);
                                    size_t end_brace = content.find("}", brace);
                                    if (brace != std::string::npos && end_brace != std::string::npos) {
                                        std::string block = content.substr(brace + 1, end_brace - brace - 1);
                                        size_t scan = 0;
                                        while (scan < block.size() && bg_count < 16) {
                                            size_t ks = block.find("\"", scan);
                                            if (ks == std::string::npos) break;
                                            size_t ke = block.find("\"", ks + 1);
                                            if (ke == std::string::npos) break;
                                            size_t vs = block.find("\"", ke + 1);
                                            if (vs == std::string::npos) break;
                                            size_t ve = block.find("\"", vs + 1);
                                            if (ve == std::string::npos) break;
                                            
                                            std::string orig = block.substr(ks + 1, ke - ks - 1);
                                            std::string repl = block.substr(vs + 1, ve - vs - 1);
                                            
                                            // Write to guest memory (bg entries at 0x1040, 128 bytes each)
                                            uint64_t entry_addr = MOD_CONFIG_GUEST + 0x1040 + bg_count * 128;
                                            strncpy((char*)(g_guest_memory + entry_addr), orig.c_str(), 63);
                                            strncpy((char*)(g_guest_memory + entry_addr + 64), repl.c_str(), 63);
                                            bg_count++;
                                            
                                            scan = ve + 1;
                                        }
                                    }
                                }
                                std::cout << "[MOD] Loaded background mod: " << mod_name 
                                          << " (" << bg_count << " replacements)" << std::endl;
                            }
                            // Log unknown types
                            if (mod_type != "music" && mod_type != "scene" && mod_type != "asset" && mod_type != "background") {
                                std::cout << "[MOD] Unknown mod type '" << mod_type << "' for: " << mod_name << std::endl;
                            }
                        }
                    }
                    
                    // Write header
                    uint32_t* hdr = (uint32_t*)(g_guest_memory + MOD_CONFIG_GUEST);
                    hdr[0] = MOD_MAGIC;
                    hdr[1] = MOD_VERSION;
                    hdr[2] = music_count;
                    hdr[3] = scene_count;
                    hdr[4] = bg_count;
                    hdr[5] = 0; // flags
                    hdr[6] = total_mods;
                    
                    // Call sre_init_mods
                    if (sre_init_mods_addr) {
                        g_emulator_64->call(sre_init_mods_addr, {MOD_CONFIG_GUEST});
                        std::cout << "[MOD] SRE mod system initialized: " << total_mods 
                                  << " mods, " << music_count << " music replacements"
                                  << std::endl;
                    } else {
                        std::cout << "[MOD] sre_init_mods not found — mod support disabled"
                                  << std::endl;
                    }
                }
                

                // Resolve GUI overlay state addresses
                static uint64_t gui_overlay_addr = 0;
                static uint64_t gui_screen_w_addr = 0;
                static uint64_t gui_screen_h_addr = 0;
                gui_overlay_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_gui_overlay_enabled");
                gui_screen_w_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_gui_screen_w");
                gui_screen_h_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_gui_screen_h");

                
                // Resolve bg_mode address for F11 toggle
                bg_mode_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_bg_mode");
                bg_brightness_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_bg_brightness");
                bg_depth_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_bg_depth");
                bg_scale_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_bg_scale");
                bg_cam_z_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_bg_cam_z");
                
                // Resolve effect hook addresses
                portal_active_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_active");
                portal_x_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_x");
                portal_y_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_y");
                portal_z_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_z");
                portal_count_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_count");
                portal_color_r_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_color_r");
                portal_color_g_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_color_g");
                portal_color_b_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_color_b");
                portal_vp_matrix_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_portal_vp_matrix");
                effect_frame_reset_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "sre_effects_frame_reset");
                
                std::cout << "[SRE] Effects: portal=" << std::hex 
                          << portal_active_addr << " reset=" << effect_frame_reset_addr
                          << std::dec << std::endl;
                
                // Resolve SRE music command interface addresses
                sre_music_load_name_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_load_name");
                sre_music_load_pending_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_load_pending");
                sre_music_play_pending_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_play_pending");
                sre_music_pause_pending_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_pause_pending");
                sre_music_stop_pending_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_stop_pending");
                sre_music_volume_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_volume");
                sre_music_volume_dirty_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_volume_dirty");
                sre_music_master_volume_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_master_volume");
                sre_music_looping_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_looping");
                sre_music_looping_dirty_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_music_looping_dirty");
                
                // Resolve SRE LNI command interface addresses
                sre_lni_command_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_lni_command");
                sre_lni_arg1_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_lni_arg1");
                sre_lni_arg2_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_lni_arg2");
                sre_lni_pending_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_lni_pending");
                sre_controls_hidden_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_controls_hidden");

                // Resolve player position for GLES2 lighting
                g_sre_hero_pos_x_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_hero_pos_x");
                g_sre_hero_pos_y_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_hero_pos_y");
                g_sre_hero_pos_z_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_hero_pos_z");
                
                // Resolve camera coordinates
                g_sre_cam_x_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_cam_x");
                g_sre_cam_y_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_cam_y");
                g_sre_cam_z_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_cam_z");

                std::cout << "[SRE] Music: load_name=0x" << std::hex << sre_music_load_name_addr
                          << " load_pending=0x" << sre_music_load_pending_addr
                          << " LNI: cmd=0x" << sre_lni_command_addr
                          << " pending=0x" << sre_lni_pending_addr
                          << " controls_hidden=0x" << sre_controls_hidden_addr
                          << std::dec << std::endl;
                
                // Resolve SRE GUI player stat addresses
                sre_player_hp_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_hp");
                sre_player_max_hp_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_max_hp");
                sre_player_mana_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_mana");
                sre_player_max_mana_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_max_mana");
                sre_player_coins_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_coins");
                sre_player_xp_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_xp");
                sre_player_level_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_level");
                sre_player_atk_level_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_player_atk_level");
                sre_gui_scene_active_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_gui_scene_active");
                sre_gamestate_ptr_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_gamestate_ptr");
                sre_menu_active_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_menu_active");
                if (sre_menu_active_addr)
                    std::cout << "[SRE] Menu detection: 0x" << std::hex << sre_menu_active_addr << std::dec << std::endl;
                
                // Text input state — SRE sets this when StartTextInputWithDelegate fires
                sre_text_input_active_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_text_input_active");
                if (sre_text_input_active_addr)
                    std::cout << "[SRE] TextInput detection: 0x" << std::hex << sre_text_input_active_addr << std::dec << std::endl;
                
                // Hard mode — controls frame limiter + ProgramState double-tick
                sre_hardmode_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_hardmode");
                if (sre_hardmode_addr)
                    std::cout << "[SRE] Hardmode flag: 0x" << std::hex << sre_hardmode_addr << std::dec << std::endl;
                
                // ButtonController — SRE-side button and overlay array for host rendering + hit-testing
                sre_btn_array_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_buttons");
                sre_overlay_array_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_overlays");
                sre_btn_globally_hidden_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_btn_globally_hidden");
                if (sre_btn_array_addr) {
                    std::cout << "[SRE] ButtonController: array=0x" << std::hex << sre_btn_array_addr
                              << " overlays=0x" << sre_overlay_array_addr
                              << " hidden=0x" << sre_btn_globally_hidden_addr << std::dec << std::endl;
                    std::cout << "[SRE/Host] SreBtnSlot size=" << sizeof(SreBtnSlot)
                              << " offsets: id=" << offsetof(SreBtnSlot, id)
                              << " label=" << offsetof(SreBtnSlot, label)
                              << " x=" << offsetof(SreBtnSlot, x)
                              << " y=" << offsetof(SreBtnSlot, y)
                              << " w=" << offsetof(SreBtnSlot, w)
                              << " h=" << offsetof(SreBtnSlot, h)
                              << " alpha=" << offsetof(SreBtnSlot, alpha)
                              << " text_color=" << offsetof(SreBtnSlot, text_color)
                              << " hidden=" << offsetof(SreBtnSlot, hidden)
                              << " overlay_id=" << offsetof(SreBtnSlot, overlay_id)
                              << " confined=" << offsetof(SreBtnSlot, confined)
                              << " pressed=" << offsetof(SreBtnSlot, pressed)
                              << " released=" << offsetof(SreBtnSlot, released)
                              << " dragging=" << offsetof(SreBtnSlot, dragging)
                              << " cur_x=" << offsetof(SreBtnSlot, cur_x)
                              << " active=" << offsetof(SreBtnSlot, active)
                              << " dirty=" << offsetof(SreBtnSlot, dirty)
                              << std::endl;
                }
                
                // VFS MiniPath globals — populate so SRE can translate
                // /ExternalFiles/, /Files/, /Cache/ without calling getenv
                {
                    auto write_vfs_path = [&](const char* sym, const std::string& path) {
                        uint64_t addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, sym);
                        if (addr) {
                            strncpy((char*)(g_guest_memory + addr), path.c_str(), 511);
                            ((char*)(g_guest_memory + addr))[511] = '\0';
                            std::cout << "[SRE] VFS " << sym << " = " << path << std::endl;
                        }
                    };
                    std::string data_base = get_data_path("");
                    // Remove trailing slash if present
                    if (!data_base.empty() && data_base.back() == '/')
                        data_base.pop_back();
                    
                    write_vfs_path("g_sre_vfs_path_external", data_base + "/external");
                    write_vfs_path("g_sre_vfs_path_files",    g_save_dir);
                    write_vfs_path("g_sre_vfs_path_cache",    g_cache_dir);
                    write_vfs_path("g_sre_vfs_path_assets",   get_data_path(g_assets_dir));
                    
                    // Create required directories if missing
                    std::string ext_dir = data_base + "/external";
                    mkdir(ext_dir.c_str(), 0755);
                    mkdir(g_save_dir.c_str(), 0755);
                    mkdir(g_cache_dir.c_str(), 0755);
                    
                    // Populate guest profile ID globals with the active save UUID.
                    // g_sre_mod_profile_id → Mini.GetProfileID()
                    // g_sre_vfs_profile_id → VFS levels 1 & 3 (profile-specific resources)
                    uint64_t profile_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_mod_profile_id");
                    uint64_t vfs_profile_addr = g_loader_64->get_symbol_vaddr(&g_sre_mod, "g_sre_vfs_profile_id");
                    if (profile_addr || vfs_profile_addr) {
                        std::string docs = g_save_dir + "/Documents";
                        if (fs::exists(docs) && fs::is_directory(docs)) {
                            for (const auto& entry : fs::directory_iterator(docs)) {
                                if (entry.path().extension() == ".gplayer") {
                                    SaveFile sf;
                                    if (save_load(entry.path().string(), sf)) {
                                        std::string id = sf.identifier.empty() ? entry.path().stem().string() : sf.identifier;
                                        if (profile_addr) {
                                            strncpy((char*)(g_guest_memory + profile_addr), id.c_str(), 63);
                                            ((char*)(g_guest_memory + profile_addr))[63] = '\0';
                                        }
                                        if (vfs_profile_addr) {
                                            strncpy((char*)(g_guest_memory + vfs_profile_addr), id.c_str(), 63);
                                            ((char*)(g_guest_memory + vfs_profile_addr))[63] = '\0';
                                        }
                                        std::cout << "[SRE] Profile ID = " << id << std::endl;
                                        set_active_profile_id(id);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                
                std::cout << "[SRE] GUI: hp=0x" << std::hex << sre_player_hp_addr
                          << " coins=0x" << sre_player_coins_addr
                          << std::dec << std::endl;
                
                std::cout << "======== [SRE] Runtime Engine Ready ========\n" << std::endl;
            } else {
                std::cerr << "[SRE] Failed to load libsre.so (error " << sre_ret << ")" << std::endl;
            }
        } else {
            std::cerr << "\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
            std::cerr << "  [SRE] WARNING: libsre.so NOT FOUND!" << std::endl;
            std::cerr << "  libsre.so is REQUIRED for ARM64 instances." << std::endl;
            std::cerr << "  Without it, atomic spin loops and threading" << std::endl;
            std::cerr << "  issues WILL cause hangs and crashes." << std::endl;
            std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
            std::cerr << "[SRE] Expected at: " << sre_rel << std::endl;
            std::cerr << "[SRE] Also tried:  ./libsre.so (current dir)" << std::endl;
            std::cerr << "[SRE] Fix: run './run_swordigo.sh' to build and install libsre.so" << std::endl;
            std::cerr << "============================================\n" << std::endl;
        }
        } // else (sre_compatible)
    }

    // Run dynamic initializers (.init_array) — entries are uint64_t (8 bytes each)
    if (g_main_mod_64.init_array_vaddr != 0 && g_main_mod_64.init_array_size > 0) {
        int count = g_main_mod_64.init_array_size / 8;
        std::cout << "[Boot64] Executing " << count << " dynamic initializers..." << std::endl;
        uint64_t* init_array = (uint64_t*)(g_guest_memory + g_main_mod_64.init_array_vaddr);
        for (int i = 0; i < count; i++) {
            uint64_t init_func = init_array[i];
            if (init_func != 0) {
                if (i % 50 == 0 || i == count - 1) {
                    std::cout << "  -> Initializer " << i << "/" << count << " at 0x" << std::hex << init_func << std::dec << std::endl;
                }
                g_emulator_64->call(init_func, {});
            }
        }
        std::cout << "[Boot64] Dynamic initializers completed successfully." << std::endl;
    }

    // 4. Boot Sequence — resolve symbols (all addresses are uint64_t)
    uint64_t setFilesDir = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_setFilesDir");
    uint64_t setCacheDir = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_setCacheDir");
    uint64_t setAssetMgr = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_setAssetManager");
    uint64_t googleSignInCompleted = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_googleSignInCompleted");
    uint64_t handleAppLaunch = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_handleApplicationLaunch");
    uint64_t initMusicPlayer = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_MusicPlayer_initMusicPlayer");
    uint64_t setupNative = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_setupNativeInterface");
    uint64_t setupApp = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_setupApplication");
    uint64_t setApplicationViewSize = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_setApplicationViewSize");
    uint64_t applicationDidBecomeActive = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_applicationDidBecomeActive");

    // Resolve FWKeyboard API symbols (mangled C++ names from the Caver engine)
    // Note: ARM64 uses different mangling for some types but symbol names are the same
    g_fw_sharedKeyboard = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "_ZN5Caver10FWKeyboard14sharedKeyboardEv");
    g_fw_sendKeyDown    = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "_ZN5Caver10FWKeyboard16SendKeyDownEventEjjd");
    g_fw_sendKeyUp      = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "_ZN5Caver10FWKeyboard14SendKeyUpEventEjjd");
    g_fw_sendKeyChar    = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "_ZN5Caver10FWKeyboard16SendKeyCharEventEjd");
    g_fw_handleMenuBtn  = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_handleMenuButtonPress");

    // Resolve text input callback symbols (native JNI functions the game exports)
    g_fw_textInputDidChange = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_textInputTextDidChange");
    g_fw_textInputDidFinish = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_textInputDidFinish");

    std::cout << "[Boot64] FWKeyboard API: sharedKeyboard=0x" << std::hex << g_fw_sharedKeyboard
              << " sendDown=0x" << g_fw_sendKeyDown << " sendUp=0x" << g_fw_sendKeyUp
              << " sendChar=0x" << g_fw_sendKeyChar << " menuBtn=0x" << g_fw_handleMenuBtn
              << std::dec << std::endl;
    std::cout << "[Boot64] TextInput API: didChange=0x" << std::hex << g_fw_textInputDidChange
              << " didFinish=0x" << g_fw_textInputDidFinish << std::dec << std::endl;

    // Setup fake JNI structures in guest memory (64-bit)
    uint64_t env_ptr = setup_jni_env_arm64(g_guest_memory);
    std::cout << "[Debug64] env_ptr = 0x" << std::hex << env_ptr << " vtable_ptr = 0x" << *(uint64_t*)(g_guest_memory + env_ptr) << std::dec << std::endl;

    // Allocate some strings in guest memory for paths
    // Guest path pointers are still uint32_t addresses (guest memory < 4GB)
    uint32_t path_ptr = 0x20000;
    uint32_t files_dir = path_ptr;
    strncpy((char*)(g_guest_memory + files_dir), g_save_dir.c_str(), 255);
    ((char*)(g_guest_memory + files_dir))[255] = '\0';
    path_ptr += 256;
    
    uint32_t cache_dir = path_ptr;
    strncpy((char*)(g_guest_memory + cache_dir), g_cache_dir.c_str(), 255);
    ((char*)(g_guest_memory + cache_dir))[255] = '\0';
    path_ptr += 256;

    // Reload controls for ARM64 (separate from ARM32 due to different scaling)
    g_input_config.load(g_save_dir + "/controls_arm64.ini");
    std::cout << "[Input64] Loaded ARM64 controls config (" << g_input_config.button_count() << " buttons)" << std::endl;

    if (setFilesDir) {
        std::cout << "[Boot64] Calling setFilesDir" << std::endl;
        g_emulator_64->call(setFilesDir, {env_ptr, 0, (uint64_t)files_dir});
    }
    if (setCacheDir) {
        std::cout << "[Boot64] Calling setCacheDir" << std::endl;
        g_emulator_64->call(setCacheDir, {env_ptr, 0, (uint64_t)cache_dir});
    }
    if (setAssetMgr) {
        std::cout << "[Boot64] Calling setAssetManager" << std::endl;
        g_emulator_64->call(setAssetMgr, {env_ptr, 0, 0x55555555});
    }
    if (googleSignInCompleted) {
        std::cout << "[Boot64] Calling googleSignInCompleted(true) — enable snapshots" << std::endl;
        g_emulator_64->call(googleSignInCompleted, {env_ptr, 0, 1}); // true = signed in
    }
    if (handleAppLaunch) {
        std::cout << "[Boot64] Calling handleApplicationLaunch" << std::endl;
        g_emulator_64->call(handleAppLaunch, {env_ptr, 0});
    }
    if (initMusicPlayer) {
        std::cout << "[Boot64] Calling initMusicPlayer" << std::endl;
        g_emulator_64->call(initMusicPlayer, {env_ptr, 0x22222222});
    }
    if (setupNative) {
        std::cout << "[Boot64] Calling setupNativeInterface" << std::endl;
        g_emulator_64->call(setupNative, {env_ptr, 0});
    }
    if (setupApp) {
        std::cout << "[Boot64] Calling setupApplication" << std::endl;
        g_emulator_64->call(setupApp, {env_ptr, 0});
    }
    if (setApplicationViewSize) {
        std::cout << "[Boot64] Calling setApplicationViewSize" << std::endl;
        g_emulator_64->call(setApplicationViewSize, {env_ptr, 0, (uint64_t)GAME_W, (uint64_t)GAME_H, 1});
    }
    if (applicationDidBecomeActive) {
        std::cout << "[Boot64] Calling applicationDidBecomeActive" << std::endl;
        g_emulator_64->call(applicationDidBecomeActive, {env_ptr, 0});
    }
    std::cout << "[Diag64] Post-applicationDidBecomeActive" << std::endl;
    std::cout.flush();

    // ========================================================================
    // Death screen fix: Make game think "Remove Ads" IAP is purchased (ARM64)
    // ========================================================================
    {
        uint8_t* memory = g_emulator_64->get_memory_base();

        // Patch IsNoAdsUnlockedCheck() -> return true (MOV W0, #1; RET)
        uint64_t noAdsCheck = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
            "_ZN5Caver15StoreController20IsNoAdsUnlockedCheckEv");
        if (noAdsCheck) {
            *(uint32_t*)(memory + noAdsCheck + 0) = 0x52800020; // MOV W0, #1
            *(uint32_t*)(memory + noAdsCheck + 4) = 0xD65F03C0; // RET
            std::cout << "[Fix64] Patched IsNoAdsUnlockedCheck at 0x" << std::hex << noAdsCheck
                      << " -> return true (ad-free)" << std::dec << std::endl;
        }

        // Also patch IsProductPurchased -> return true
        uint64_t isPurchased = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
            "_ZN5Caver23StoreController_Android18IsProductPurchasedERKSs");
        if (!isPurchased) {
            // Try the base class version
            isPurchased = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
                "_ZN5Caver15StoreController18IsProductPurchasedERKSs");
        }
        if (isPurchased) {
            *(uint32_t*)(memory + isPurchased + 0) = 0x52800020; // MOV W0, #1
            *(uint32_t*)(memory + isPurchased + 4) = 0xD65F03C0; // RET
            std::cout << "[Fix64] Patched IsProductPurchased at 0x" << std::hex << isPurchased
                      << " -> return true" << std::dec << std::endl;
        }

        // Patch ShowInterstitialAd to return immediately — the death state machine
        // can't be replicated on non-Android (same issue as PS Vita port).
        // Death recovery uses execv process restart.
        uint64_t showAd = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
            "_ZN5Caver24OnlineController_Android18ShowInterstitialAdERKSsif");
        if (showAd) {
            *(uint32_t*)(memory + showAd) = 0xD65F03C0; // RET
            std::cout << "[Fix64] Patched ShowInterstitialAd at 0x" << std::hex << showAd
                      << " -> ret" << std::dec << std::endl;
        }
        uint64_t showAd2 = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
            "_ZN5Caver25AndroidShowInterstitialAdEd");
        if (showAd2) {
            *(uint32_t*)(memory + showAd2) = 0xD65F03C0; // RET
            std::cout << "[Fix64] Patched AndroidShowInterstitialAd at 0x" << std::hex << showAd2
                      << " -> ret" << std::dec << std::endl;
        }

        // Coin limit patches (ARM64)
        // Lua coin limit check 1 & 2: offsets 0x35e25c, 0x35e260
        // Game event limit check 3 & 4: offsets 0x3597a0, 0x3597a4
        // Too rich patch: 0x3597ac, 0x3597b4, 0x3597b8
        {
            uint64_t base_addr = g_main_mod_64.base_addr;
            if (base_addr) {
                extern int g_sre_coin_limit;
                uint16_t limit = (uint16_t)g_sre_coin_limit;

                // Instruction helper functions inlined
                auto emit_mov_wide_immediate = [](uint16_t imm16, uint8_t rd) -> uint32_t {
                    // mov w[rd], #imm16
                    return 0x52800000 | (imm16 << 5) | rd;
                };
                auto emit_cmp_shifted_register = [](uint8_t rn, uint8_t rm) -> uint32_t {
                    // cmp w[rn], w[rm] -> subs wzr, w[rn], w[rm]
                    return 0x6B00001F | (rm << 16) | (rn << 5);
                };

                // Lua path:
                // mov w8, #limit -> cmp w0, w8
                *(uint32_t*)(memory + base_addr + 0x35e25c) = emit_mov_wide_immediate(limit, 8);
                *(uint32_t*)(memory + base_addr + 0x35e260) = emit_cmp_shifted_register(0, 8);

                // Game event path:
                // mov w10, #limit -> cmp w8, w10
                *(uint32_t*)(memory + base_addr + 0x3597a0) = emit_mov_wide_immediate(limit, 10);
                *(uint32_t*)(memory + base_addr + 0x3597a4) = emit_cmp_shifted_register(8, 10);

                // Too rich achievement threshold patch:
                // NOP at 0x3597ac
                // mov w11, #(limit - 1) at 0x3597b4
                // cmp w8, w11 at 0x3597b8
                *(uint32_t*)(memory + base_addr + 0x3597ac) = 0xD503201F; // NOP
                *(uint32_t*)(memory + base_addr + 0x3597b4) = emit_mov_wide_immediate(limit - 1, 11);
                *(uint32_t*)(memory + base_addr + 0x3597b8) = emit_cmp_shifted_register(8, 11);

                std::cout << "[CoinLimit64] Applied ARM64 coin limit patches (limit=" << limit << ")" << std::endl;
            }
        }
    }

    // Resolve adVisibilityChanged (kept for death detection countdown fallback)
    uint64_t adVisibilityChanged = g_loader_64->get_symbol_vaddr(&g_main_mod_64,
        "Java_com_touchfoo_swordigo_Native_interstitialAdVisibilityChanged");

    // 5. Game Loop
    uint64_t updateApp = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_updateApplication");
    uint64_t drawApp = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_drawApplication");
    uint64_t handleTouchEvent = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_handleTouchEvent");
    uint64_t snapshotLoaded = g_loader_64->get_symbol_vaddr(&g_main_mod_64, "Java_com_touchfoo_swordigo_Native_snapshotLoaded");
    
    // Tell the game Google Sign-In succeeded so snapshot system is enabled
    if (googleSignInCompleted) {
        std::cout << "[Boot64] Calling googleSignInCompleted(true) — enable snapshots" << std::endl;
        std::cout.flush();
        g_emulator_64->call(googleSignInCompleted, {env_ptr, 0, 1}); // true = signed in
        std::cout << "[Diag64] Post-googleSignInCompleted" << std::endl;
        std::cout.flush();
    }

    bool g_hide_touch_hud = true;
    g_gl_hide_hud = g_hide_touch_hud;

    std::cout << "[Diag64] Entering game loop. updateApp=0x" << std::hex << updateApp
              << " drawApp=0x" << drawApp << std::dec << std::endl;
    std::cout.flush();

    if (updateApp && drawApp) {
        Uint64 last_ticks = SDL_GetTicks();
        double accumulated_time = 0.0;
        bool gui_visible = false;
        bool debug_visible = false;
        bool hint_shown = false;
        Uint64 hint_start_time = SDL_GetTicks();
        float fps = 0.0f;
        Uint64 fps_last_time = SDL_GetTicks();
        int fps_frame_count = 0;
        
        const int TARGET_FRAMES = g_display_active ? 0 : 1000;
        int completed_frames = 0;
        
        uint32_t total_draw_calls = 0;
        uint32_t total_tex_binds = 0;
        uint32_t total_tex_uploads = 0;
        uint32_t total_matrix_ops = 0;
        uint32_t total_state_changes = 0;
        uint32_t total_asset_opens = 0;

        std::cout << "\n========================================" << std::endl;
        if (g_display_active) {
            std::cout << "[Loop64] Starting visual game loop (close window to stop)" << std::endl;
        } else {
            std::cout << "[Loop64] Starting " << TARGET_FRAMES << "-frame stability test" << std::endl;
        }
        std::cout << "========================================\n" << std::endl;

        extern Display* g_display_ptr;

        int mouse_x = 0;
        int mouse_y = 0;
        bool mouse_pressed = false;
        bool click_swallowed_by_gui = false;
        float last_mouse_x = -1.0f;
        float last_mouse_y = -1.0f;

        bool key_left = false;
        bool key_right = false;
        bool key_jump = false;
        bool key_attack = false;
        bool key_magic = false;
        bool key_use_item = false;
        bool key_menu = false;

        bool arrow_left  = false;
        bool arrow_right = false;
        bool arrow_up    = false;
        bool arrow_down  = false;
        bool cam_key_pgup  = false;
        bool cam_key_pgdn  = false;

        bool running = true;
        while (running) {
            g_frame_stats.reset();
            
            Uint64 now_ticks = SDL_GetTicks();
            float dt_seconds = (now_ticks - last_ticks) / 1000.0f;
            if (dt_seconds > 0.1f) dt_seconds = 0.016666668f;
            if (dt_seconds < 0.001f) dt_seconds = 0.016666668f;
            last_ticks = now_ticks;
            accumulated_time += dt_seconds;
            
            if (completed_frames < 150) {
                g_emulator_64->quiet_mode = false;
            } else {
                g_emulator_64->quiet_mode = true;
            }
            
            // Touch held buttons — ARM64 AAPCS64 calling convention:
            // Integer args: X0=env, X1=obj, X2=action, X3=id, X4=tapCount
            // Float args (separate sequence): D0=time, S1=x, S2=y, S3=oldX, S4=oldY
            static int touch_debug_count = 0;
            auto call_touch_64 = [&](int action, int id, double time_val, float x, float y, float old_x, float old_y, int tap_count) {
                if (!handleTouchEvent) return;
                
                // Check if touch input is disabled globally by Lua scripts
                extern volatile int g_sre_controls_disabled;
                if (g_sre_controls_disabled) {
                    return;
                }
                
                // Scale from legacy 960×544 touch space to actual game resolution
                // (same as ARM32 call_handle_touch_event)
                x     *= TOUCH_SCALE_X;  y     *= TOUCH_SCALE_Y;
                old_x *= TOUCH_SCALE_X;  old_y *= TOUCH_SCALE_Y;
                
                if (touch_debug_count < 5) {
                    std::cout << "[Touch64] action=" << action << " id=" << id
                              << " x=" << x << " y=" << y << std::endl;
                }
                // Set float/double args (AAPCS64: D0=1st FP, S1=2nd FP, S2=3rd FP, etc.)
                g_emulator_64->set_dreg(0, time_val);
                g_emulator_64->set_sreg(1, x);
                g_emulator_64->set_sreg(2, y);
                g_emulator_64->set_sreg(3, old_x);
                g_emulator_64->set_sreg(4, old_y);
                
                if (touch_debug_count < 5) {
                    std::cout << "[Touch64-Verify] D0=" << g_emulator_64->get_dreg(0)
                              << " S1=" << g_emulator_64->get_sreg(1)
                              << " S2=" << g_emulator_64->get_sreg(2)
                              << " S3=" << g_emulator_64->get_sreg(3)
                              << " S4=" << g_emulator_64->get_sreg(4) << std::endl;
                    touch_debug_count++;
                }
                // Integer args via call()
                g_emulator_64->call(handleTouchEvent, {env_ptr, 0, (uint64_t)action, (uint64_t)id, (uint64_t)tap_count});
            };

            // Per-frame camera movement
            if (g_cam_active) {
                bool any_cam_key = arrow_left || arrow_right || arrow_up || arrow_down || cam_key_pgup || cam_key_pgdn;
                if (any_cam_key) {
                    float dx = 0, dy = 0, dz = 0;
                    if (arrow_left)   dx -= 1.0f;
                    if (arrow_right)  dx += 1.0f;
                    if (arrow_up)     dy += 1.0f;
                    if (arrow_down)   dy -= 1.0f;
                    if (cam_key_pgup) dz -= 1.0f;
                    if (cam_key_pgdn) dz += 1.0f;
                    cam_move_scaled(dx, dy, dz, dt_seconds);
                } else {
                    cam_reset_accel();
                }
                extern void cam_write_to_guest();
                cam_write_to_guest();
            }

            float game_dt = dt_seconds * g_game_speed;
            uint32_t dt_hex;
            memcpy(&dt_hex, &game_dt, 4);

            if (!g_game_paused || g_step_one_frame) {
                // ARM64 AAPCS: float dt goes in S0, not X2
                g_emulator_64->set_sreg(0, game_dt);
                g_emulator_64->call(updateApp, {env_ptr, 0});
                if (g_step_one_frame) {
                    g_step_one_frame = false;
                    mod_toast("Stepped 1 frame", 0.8f);
                }
            }
            
            // Render custom sky (after game frame, using depth buffer trick)
            if (g_sky.enabled && g_display_active) {
                g_sky.update(dt_seconds);
                g_sky.render(g_win_w, g_win_h);
            }
            // --- Death recovery: handled by SRE ShowAdMaybe hook ---
            // The old system used execv() to restart the entire process after 3s.
            // Now, SRE hooks GameOverViewController::ShowAdMaybe() and directly
            // calls GameOverViewDidContinue() — proper in-game respawn from checkpoint.
            // This countdown is kept only for logging/diagnostics.
            if (g_death_detected_countdown > 0) {
                g_death_detected_countdown--;
                if (g_death_detected_countdown == 0) {
                    std::cout << "\n[SRE] Death timeout reached — respawn should have been handled by ShowAdMaybe hook" << std::endl;
                    std::cout << "[SRE] If you're stuck, tap the game over screen to trigger respawn" << std::endl;
                }
            }
            
            io_thread_poll();

            // Handle async snapshot callbacks
            extern int g_snapshot_load_pending_count; // counter, not boolean
            extern bool g_snapshot_has_data;
            extern std::vector<uint8_t> g_snapshot_data;
            while (g_snapshot_load_pending_count > 0 && snapshotLoaded) {
                g_snapshot_load_pending_count--;
                if (g_snapshot_has_data && !g_snapshot_data.empty()) {
                    static uint32_t save_buf_addr = 0x40000000;
                    uint32_t array_len = g_snapshot_data.size();
                    if (save_buf_addr + 4 + array_len > 0xE0000000ULL) {
                        std::cerr << "[SAVE] Save data too large (" << array_len << " bytes), skipping" << std::endl;
                        continue;
                    }
                    *(uint32_t*)(g_guest_memory + save_buf_addr) = array_len;
                    memcpy(g_guest_memory + save_buf_addr + 4, g_snapshot_data.data(), array_len);
                    std::cout << "[Callback64] snapshotLoaded with " << array_len << " bytes of save data" << std::endl;
                    g_emulator_64->call(snapshotLoaded, {env_ptr, 0, 0, (uint64_t)save_buf_addr});
                    g_snapshot_has_data = false;
                } else {
                    std::cout << "[Callback64] snapshotLoaded(null, null) — no saved game" << std::endl;
                    g_emulator_64->call(snapshotLoaded, {env_ptr, 0, 0, 0});
                }
            }
            
            if (g_display_active) {
#ifdef VULKAN_BACKEND
                if (g_graphics_api == GraphicsAPI::VULKAN) {
                    g_vk_backend.begin_frame();
                } else
#endif
                {
                    fbo_begin_game();
                }
            }
            
            // --- Run deferred threads (from pthread_create) ---
            // Scene transitions spawn loading threads. Running them inline
            // (nested uc_emu_start) corrupts state. Run them here between frames.
            if (g_emulator_64->has_pending_threads()) {
                if (completed_frames < 20) {
                    std::cout << "[Diag64] Frame " << completed_frames 
                              << " running deferred threads" << std::endl;
                }
                g_emulator_64->run_pending_threads();
            }
            
            if (sre_menu_active_addr) {
                *(int32_t*)(g_guest_memory + sre_menu_active_addr) = 0;
            }
            g_emulator_64->call(drawApp, {env_ptr, 0});

            if (g_display_active) {
#ifdef VULKAN_BACKEND
                if (g_graphics_api == GraphicsAPI::VULKAN) {
                    // Vulkan frame end + present is done after overlays
                } else
#endif
                {
                    draw_batcher_flush(); // Flush any pending batched draws
                    
                    // Read background data from SRE → feed FBO shaders
                    {
                        extern float g_bg_depth, g_bg_scale;
                        extern float g_bg_ambient_r, g_bg_ambient_g, g_bg_ambient_b;
                        extern float g_portal_active, g_portal_world_x, g_portal_world_y, g_portal_world_z;
                        extern float g_portal_color_r, g_portal_color_g, g_portal_color_b;
                        extern float g_portal_intensity, g_portal_speed;
                        
                        if (bg_depth_addr)
                            g_bg_depth = *(float*)(g_guest_memory + bg_depth_addr);
                        if (bg_scale_addr)
                            g_bg_scale = *(float*)(g_guest_memory + bg_scale_addr);
                        
                        // Read portal effect state
                        if (portal_active_addr) {
                            int active = *(int*)(g_guest_memory + portal_active_addr);
                            g_portal_active = active ? 1.0f : 0.0f;
                            if (active && portal_x_addr) {
                                g_portal_world_x = *(float*)(g_guest_memory + portal_x_addr);
                                g_portal_world_y = *(float*)(g_guest_memory + portal_y_addr);
                                g_portal_world_z = *(float*)(g_guest_memory + portal_z_addr);
                                // Read actual portal color from game data
                                if (portal_color_r_addr) {
                                    g_portal_color_r = *(float*)(g_guest_memory + portal_color_r_addr);
                                    g_portal_color_g = *(float*)(g_guest_memory + portal_color_g_addr);
                                    g_portal_color_b = *(float*)(g_guest_memory + portal_color_b_addr);
                                }
                                // Read full VP matrix (16 floats) for portal positioning
                                if (portal_vp_matrix_addr) {
                                    extern float g_portal_vp_matrix[16];
                                    float* src = (float*)(g_guest_memory + portal_vp_matrix_addr);
                                    for (int i = 0; i < 16; i++) g_portal_vp_matrix[i] = src[i];
                                }
                            }
                        }
                    }
                    
                    /* === PostFX Auto-Disable in Menus ===
                     * DISABLED — Unreliable due to incomplete GUI hook coverage.
                     * We only control ~8 DrawRect hooks but the engine has many
                     * more GUI views (map, pause, equipment, shops, etc.) that
                     * we can't detect yet. Re-enable once we hook a reliable
                     * frame-level signal (e.g. ProgramState screen type, or a
                     * top-level "is gameplay active" flag from the engine).
                     *
                     * Infrastructure in place:
                     *   SRE:  g_sre_menu_active (volatile int, exported)
                     *         Set by NewMenuView/GUIAlertView/GUISlider DrawRect hooks
                     *   Host: sre_menu_active_addr, postfx_user_preset, etc.
                     *         Reads flag, suppresses PostFX, restores on menu close
                     */

                    fbo_end_game_and_blit(g_draw_w, g_draw_h, g_fbo_mode, &g_postfx);
                    
                    // Render portal in vanilla mode — DISABLED pending RE of vertex data
                    // if (!fbo_is_active()) {
                    //     fbo_draw_portal_vanilla(g_draw_w, g_draw_h);
                    // }
                    
                    // Reset per-frame effect counters for next frame
                    if (effect_frame_reset_addr) {
                        g_emulator_64->call(effect_frame_reset_addr, {});
                    }
                    
                    // === SRE Music Command Processing ===
                    // Read guest globals and dispatch to host OpenAL
                    if (sre_music_load_pending_addr) {
                        int load_pending = *(int*)(g_guest_memory + sre_music_load_pending_addr);
                        if (load_pending) {
                            // Read music name from guest memory
                            char name_buf[256] = {0};
                            memcpy(name_buf, g_guest_memory + sre_music_load_name_addr, 255);
                            name_buf[255] = 0;
                            
                            // Clear pending flag in guest
                            int zero = 0;
                            memcpy(g_guest_memory + sre_music_load_pending_addr, &zero, 4);
                            
                            // Load and play via host API
                            sre_music_host_load(std::string(name_buf));
                        }
                        
                        int play_pending = *(int*)(g_guest_memory + sre_music_play_pending_addr);
                        if (play_pending) {
                            sre_music_host_play();
                            int zero = 0;
                            memcpy(g_guest_memory + sre_music_play_pending_addr, &zero, 4);
                        }
                        
                        int pause_pending = *(int*)(g_guest_memory + sre_music_pause_pending_addr);
                        if (pause_pending) {
                            sre_music_host_pause();
                            int zero = 0;
                            memcpy(g_guest_memory + sre_music_pause_pending_addr, &zero, 4);
                        }
                        
                        int stop_pending = *(int*)(g_guest_memory + sre_music_stop_pending_addr);
                        if (stop_pending) {
                            sre_music_host_stop();
                            int zero = 0;
                            memcpy(g_guest_memory + sre_music_stop_pending_addr, &zero, 4);
                        }
                        
                        int vol_dirty = *(int*)(g_guest_memory + sre_music_volume_dirty_addr);
                        if (vol_dirty) {
                            float vol = *(float*)(g_guest_memory + sre_music_volume_addr);
                            // Apply duck multiplier on top of guest volume
                            vol *= s_duck_volume;
                            sre_music_host_set_volume(vol);
                            int zero = 0;
                            memcpy(g_guest_memory + sre_music_volume_dirty_addr, &zero, 4);
                        }
                        
                        int loop_dirty = *(int*)(g_guest_memory + sre_music_looping_dirty_addr);
                        if (loop_dirty) {
                            int looping = *(int*)(g_guest_memory + sre_music_looping_addr);
                            sre_music_host_set_looping(looping != 0);
                            int zero = 0;
                            memcpy(g_guest_memory + sre_music_looping_dirty_addr, &zero, 4);
                        }
                        
                        // === Host-side music ducking ===
                        // Triggered by 0item.wav asset loading (boss kill / XP sack SFX).
                        // 3-phase: fade down → hold low → fade back up.
                        if (g_music_duck_trigger) {
                            g_music_duck_trigger = 0;
                            s_duck_phase = 1;
                            s_duck_timer = DUCK_FADE_DOWN_TIME;
                            std::cout << "[SRE-Music] Duck triggered (0item.wav detected)" << std::endl;
                        }
                        
                        if (s_duck_phase > 0) {
                            s_duck_timer -= dt_seconds;
                            float prev_duck = s_duck_volume;
                            
                            if (s_duck_phase == 1) {
                                // Phase 1: fading down
                                float progress = 1.0f - (s_duck_timer / DUCK_FADE_DOWN_TIME);
                                if (progress < 0.0f) progress = 0.0f;
                                if (progress > 1.0f) progress = 1.0f;
                                s_duck_volume = 1.0f - progress * (1.0f - DUCK_LOW_VOLUME);
                                if (s_duck_timer <= 0.0f) {
                                    s_duck_phase = 2;
                                    s_duck_timer = DUCK_HOLD_TIME;
                                    s_duck_volume = DUCK_LOW_VOLUME;
                                }
                            } else if (s_duck_phase == 2) {
                                // Phase 2: holding low
                                s_duck_volume = DUCK_LOW_VOLUME;
                                if (s_duck_timer <= 0.0f) {
                                    s_duck_phase = 3;
                                    s_duck_timer = DUCK_FADE_UP_TIME;
                                }
                            } else if (s_duck_phase == 3) {
                                // Phase 3: fading back up
                                float progress = 1.0f - (s_duck_timer / DUCK_FADE_UP_TIME);
                                if (progress < 0.0f) progress = 0.0f;
                                if (progress > 1.0f) progress = 1.0f;
                                s_duck_volume = DUCK_LOW_VOLUME + progress * (1.0f - DUCK_LOW_VOLUME);
                                if (s_duck_timer <= 0.0f) {
                                    s_duck_phase = 0;
                                    s_duck_volume = 1.0f;
                                    std::cout << "[SRE-Music] Duck complete — volume restored" << std::endl;
                                }
                            }
                            
                            // Apply ducked volume to OpenAL
                            if (s_duck_volume != prev_duck && sre_music_volume_addr) {
                                float guest_vol = *(float*)(g_guest_memory + sre_music_volume_addr);
                                sre_music_host_set_volume(guest_vol * s_duck_volume);
                            }
                        }
                        
                        // Music loop watchdog: if looping is enabled but source
                        // has stopped (e.g. OGG stream ended), restart it.
                        // AL_LOOPING doesn't always work with streamed audio.
                        if (sre_music_looping_addr) {
                            int looping = *(int*)(g_guest_memory + sre_music_looping_addr);
                            if (looping && !sre_music_host_is_playing()) {
                                // Source stopped but should loop — restart
                                sre_music_host_play();
                            }
                        }
                    }

                    // === SRE LNI Command Processing ===
                    if (sre_lni_pending_addr) {
                        int lni_pending = *(int*)(g_guest_memory + sre_lni_pending_addr);
                        if (lni_pending) {
                            char cmd_buf[64] = {0};
                            memcpy(cmd_buf, g_guest_memory + sre_lni_command_addr, 63);
                            cmd_buf[63] = 0;
                            
                            std::string cmd(cmd_buf);
                            std::cout << "[SRE-LNI] Processing command: '" << cmd << "'" << std::endl;
                            
                            if (cmd == "quit") {
                                std::cout << "[SRE-LNI] Exiting requested by game mod." << std::endl;
                                exit(0);
                            } else if (cmd == "copyToClipboard") {
                                char arg1_buf[512] = {0};
                                memcpy(arg1_buf, g_guest_memory + sre_lni_arg1_addr, 511);
                                ImGui::SetClipboardText(arg1_buf);
                                std::cout << "[SRE-LNI] Copied to clipboard: '" << arg1_buf << "'" << std::endl;
                            } else if (cmd == "openUrl" || cmd == "openURL") {
                                char arg1_buf[512] = {0};
                                memcpy(arg1_buf, g_guest_memory + sre_lni_arg1_addr, 511);
                                if (strcmp(arg1_buf, "overlay:") == 0 || strcmp(arg1_buf, "overlay") == 0) {
                                    g_swordfare_gui.set_mod_overlay_visible(true);
                                    std::cout << "[SRE-LNI] Mod requested opening overlay menu." << std::endl;
                                }
                            }
                            
                            // Clear pending flag in guest
                            int zero = 0;
                            memcpy(g_guest_memory + sre_lni_pending_addr, &zero, 4);
                        }
                    }

                    // Sync overlay state with guest controls_hidden
                    if (sre_controls_hidden_addr && g_guest_memory) {
                        if (g_swordfare_gui.is_mod_overlay_visible()) {
                            int one = 1;
                            memcpy(g_guest_memory + sre_controls_hidden_addr, &one, 4);
                        }
                    }
                }
            }

            // Camera override not yet ported to ARM64 (cam_apply uses Emulator* / ARM32 registers)
            // cam_apply(g_emulator_64, g_guest_memory);

            // Render GUI overlay (F1 toggle)
            if (g_display_active && gui_visible && g_graphics_api != GraphicsAPI::OPENGL) {
                g_gui.render(mouse_x, mouse_y, mouse_pressed, g_win_w, g_win_h);
            }
            
            // Render controls editor overlay (F2 toggle)
            if (g_display_active && g_input_config.is_editing()) {
                g_input_config.render_editor(g_win_w, g_win_h, mouse_x, mouse_y, mouse_pressed);
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                // Controls editor header text
                {
                    std::string mode_str = "POSITION";
                    if (g_input_config.get_editor_mode() == EDITOR_REBIND) mode_str = "REBIND (click button, press key)";
                    g_gui.draw_string("CONTROLS EDITOR [" + mode_str + "]  |  F2=Save & Close  R=Toggle Rebind", 10, g_win_h - 20, 1.3f, 100, 200, 255, 255);
                    g_gui.draw_string("Blue=Move  Green=Action  Orange=Menu  Purple=Magic Macro  Yellow=Custom", 10, g_win_h - 38, 1.1f, 180, 180, 200, 200);
                }
                // Draw readable button labels on each circle
                {
                    float sx = (float)g_win_w / 960.0f;
                    float sy = (float)g_win_h / 544.0f;
                    for (int i = 0; i < g_input_config.button_count(); i++) {
                        TouchButton* btn = g_input_config.get_button(i);
                        if (!btn) continue;
                        float wx = btn->game_x * sx;
                        float wy = btn->game_y * sy;
                        float wr = btn->radius * std::min(sx, sy) * 0.7f;
                        // Display name
                        std::string dname = btn->display_name.empty() ? btn->name : btn->display_name;
                        // Key binding text
                        std::string key1 = InputConfig::scancode_name(btn->sdl_scancode);
                        std::string key2 = InputConfig::scancode_name(btn->sdl_scancode_alt);
                        std::string keytxt = "[" + key1;
                        if (btn->sdl_scancode_alt > 0) keytxt += "/" + key2;
                        keytxt += "]";
                        // Color
                        uint8_t cr = 255, cg = 255, cb = 255;
                        if (btn->is_macro) { cr = 200; cg = 140; cb = 255; }
                        else if (btn->is_custom) { cr = 255; cg = 255; cb = 100; }
                        // Draw name above center
                        g_gui.draw_string(dname, (int)(wx - dname.size() * 4), (int)(wy + 6), 1.0f, cr, cg, cb, 255);
                        // Draw key binding below center
                        g_gui.draw_string(keytxt, (int)(wx - keytxt.size() * 3.5f), (int)(wy - 10), 0.9f, 180, 180, 180, 200);
                        if (btn->is_macro) {
                            g_gui.draw_string("MACRO", (int)(wx - 16), (int)(wy - wr - 14), 0.8f, 180, 80, 220, 200);
                        }
                    }
                }
                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }
            
            // Render F3 debug overlay
            if (g_display_active && debug_visible && g_graphics_api != GraphicsAPI::OPENGL) {
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                // ── Panel background ──
                int panel_h = 380;
                glColor4ub(10, 10, 18, 210);
                glBegin(GL_QUADS);
                glVertex2f(8, g_win_h - 8); glVertex2f(440, g_win_h - 8);
                glVertex2f(440, g_win_h - panel_h); glVertex2f(8, g_win_h - panel_h);
                glEnd();
                // Accent bar (top)
                glColor4ub(80, 200, 120, 255);
                glBegin(GL_QUADS);
                glVertex2f(8, g_win_h - 8); glVertex2f(440, g_win_h - 8);
                glVertex2f(440, g_win_h - 11); glVertex2f(8, g_win_h - 11);
                glEnd();

                char dbg[256];
                float y = g_win_h - 30;
                float x = 20;
                float lh = 17;  // line height

                // ── Section: Performance ──
                snprintf(dbg, sizeof(dbg), "FPS: %.1f", fps);
                g_gui.draw_string(dbg, x, y, 2.0f, 80, 255, 120, 255); y -= 28;

                snprintf(dbg, sizeof(dbg), "Frame: %d", completed_frames);
                g_gui.draw_string(dbg, x, y, 1.3f, 200, 200, 210, 255); y -= lh;

                // ── Section: Render Stats ──
                snprintf(dbg, sizeof(dbg), "Draws: %d   Verts: %d   TexBinds: %d",
                         g_frame_stats.draw_calls, g_frame_stats.vertices_submitted, g_frame_stats.texture_binds);
                g_gui.draw_string(dbg, x, y, 1.1f, 160, 160, 175, 255); y -= lh;
                snprintf(dbg, sizeof(dbg), "State: %d   MatOps: %d   TexUps: %d",
                         g_frame_stats.state_changes, g_frame_stats.matrix_ops, g_frame_stats.tex_uploads);
                g_gui.draw_string(dbg, x, y, 1.1f, 160, 160, 175, 255); y -= lh + 4;

                // ── Section: Display ──
                snprintf(dbg, sizeof(dbg), "Render: %dx%d  Window: %dx%d  Draw: %dx%d",
                         GAME_W, GAME_H, g_win_w, g_win_h, g_draw_w, g_draw_h);
                g_gui.draw_string(dbg, x, y, 1.1f, 130, 130, 155, 255); y -= lh;

                snprintf(dbg, sizeof(dbg), "Mouse: %d,%d   DT: %.4fs", mouse_x, mouse_y, dt_seconds);
                g_gui.draw_string(dbg, x, y, 1.1f, 130, 130, 155, 255); y -= lh + 4;

                // ── Section: Engine & Backend ──
                // Separator line
                glColor4ub(60, 60, 80, 200);
                glBegin(GL_QUADS);
                glVertex2f(x, y + 6); glVertex2f(420, y + 6);
                glVertex2f(420, y + 5); glVertex2f(x, y + 5);
                glEnd();
                y -= 4;

                // CPU Engine
                const char* engine_str = g_emulator_64 ? g_emulator_64->engine_name() : "None";
                bool is_dynarmic = g_emulator_64 && strcmp(g_emulator_64->engine_name(), "Dynarmic") == 0;
                snprintf(dbg, sizeof(dbg), "CPU Engine: %s", engine_str);
                g_gui.draw_string(dbg, x, y, 1.3f,
                    is_dynarmic ? 255 : 200,   // Dynarmic = gold, Unicorn = cyan
                    is_dynarmic ? 200 : 220,
                    is_dynarmic ? 60  : 255, 255);
                y -= lh;

                // Graphics API
                const char* api_str = (g_graphics_api == GraphicsAPI::VULKAN) ? "Vulkan" : "OpenGL";
                snprintf(dbg, sizeof(dbg), "Graphics: %s", api_str);
                g_gui.draw_string(dbg, x, y, 1.3f, 100, 200, 255, 255); y -= lh;

                // Scale mode
                const char* fbo_mode_str = "Unknown";
                if (g_fbo_mode == FBOScale::SHARP_BILINEAR) fbo_mode_str = "Sharp-Bilinear";
                else if (g_fbo_mode == FBOScale::NEAREST) fbo_mode_str = "Nearest";
                else if (g_fbo_mode == FBOScale::CRT_SCANLINE) fbo_mode_str = "CRT Scanline";
                else if (g_fbo_mode == FBOScale::FSR) fbo_mode_str = "FSR 1.0";
                snprintf(dbg, sizeof(dbg), "Scaler: %s", fbo_mode_str);
                g_gui.draw_string(dbg, x, y, 1.2f, 255, 200, 100, 255); y -= lh;

                // PostFX
                if (g_postfx.enabled) {
                    snprintf(dbg, sizeof(dbg), "PostFX: %s", g_postfx.preset_name);
                    g_gui.draw_string(dbg, x, y, 1.2f, 255, 160, 80, 255);
                } else {
                    g_gui.draw_string("PostFX: Off", x, y, 1.2f, 100, 100, 110, 200);
                }
                y -= lh;

                // Binary info
                const BinaryInfo* binfo = g_binary_selector.get_loaded_info();
                if (binfo) {
                    snprintf(dbg, sizeof(dbg), "Binary: %s (%s)", binfo->filename.c_str(), binfo->label.c_str());
                } else {
                    snprintf(dbg, sizeof(dbg), "Binary: %s", g_lib_name.c_str());
                }
                g_gui.draw_string(dbg, x, y, 1.1f, 120, 180, 255, 255); y -= lh;

                // Speed
                snprintf(dbg, sizeof(dbg), "Speed: %s  %s", mod_speed_label(), g_game_paused ? "|| PAUSED" : "");
                g_gui.draw_string(dbg, x, y, 1.2f, 255, 180, 50, 255); y -= lh + 4;

                // ── Section: Player Stats ──
                if (sre_gui_scene_active_addr && g_guest_memory) {
                    int scene_active = *(int*)(g_guest_memory + sre_gui_scene_active_addr);
                    if (scene_active && sre_player_hp_addr) {
                        int hp     = *(int*)(g_guest_memory + sre_player_hp_addr);
                        int max_hp = *(int*)(g_guest_memory + sre_player_max_hp_addr);
                        int mana   = *(int*)(g_guest_memory + sre_player_mana_addr);
                        int max_mn = *(int*)(g_guest_memory + sre_player_max_mana_addr);
                        int coins  = *(int*)(g_guest_memory + sre_player_coins_addr);
                        int xp     = sre_player_xp_addr ? *(int*)(g_guest_memory + sre_player_xp_addr) : 0;
                        int level  = sre_player_level_addr ? *(int*)(g_guest_memory + sre_player_level_addr) : 0;
                        int atk    = sre_player_atk_level_addr ? *(int*)(g_guest_memory + sre_player_atk_level_addr) : 0;

                        // Separator
                        glColor4ub(60, 60, 80, 200);
                        glBegin(GL_QUADS);
                        glVertex2f(x, y + 6); glVertex2f(420, y + 6);
                        glVertex2f(420, y + 5); glVertex2f(x, y + 5);
                        glEnd();
                        y -= 4;

                        snprintf(dbg, sizeof(dbg), "HP: %d/%d  Mana: %d/%d  Coins: %d",
                                 hp, max_hp, mana, max_mn, coins);
                        int hp_pct = max_hp > 0 ? (hp * 100 / max_hp) : 0;
                        uint8_t hr = hp_pct > 50 ? 80 : 255;
                        uint8_t hg = hp_pct > 25 ? 255 : 80;
                        g_gui.draw_string(dbg, x, y, 1.3f, hr, hg, 80, 255); y -= lh;

                        snprintf(dbg, sizeof(dbg), "Lv.%d  XP: %d  ATK: %d", level, xp, atk);
                        g_gui.draw_string(dbg, x, y, 1.3f, 180, 140, 255, 255); y -= lh;
                    }
                }

                // ── Hotkey legend ──
                y -= 2;
                g_gui.draw_string("F1:GUI F2:Ctrl F3:Debug F4:Scale F5:Cam F6:PostFX", x, y, 1.0f, 80, 80, 100, 180); y -= 14;
                g_gui.draw_string("F7:Video \\\\:Type F8:Pause F10:HUD F12:Fullscreen", x, y, 1.0f, 80, 80, 100, 180);

                if (g_typing_mode) {
                    g_gui.draw_string("TYPING MODE", 350, g_win_h - 30, 1.2f, 255, 80, 80, 255);
                }
                if (g_cam_active) {
                    char cam_dbg[128];
                    cam_debug_string(cam_dbg, sizeof(cam_dbg));
                    g_gui.draw_string(cam_dbg, x, y - 14, 1.0f, 80, 200, 120, 255);
                }

                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }
            
            // Render mod tools overlay
            if (g_display_active) {
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                mod_render_overlay(g_gui, g_win_w, g_win_h, dt_seconds);
                mod_achievement_poll(g_guest_memory, 1);  // offsets are absolute guest VA
                mod_achievement_render(g_gui, g_win_w, g_win_h, dt_seconds);
                
                
                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }

            extern int g_gl_diag_frame;
            g_gl_diag_frame++;

            // Render SRT overlay (inventory editor, etc.) — F11 toggle
            if (g_display_active && g_srt_overlay.is_visible()) {
                // Convert sustained mouse_pressed to single-frame click
                static bool overlay_prev_pressed = false;
                bool overlay_click = mouse_pressed && !overlay_prev_pressed;
                overlay_prev_pressed = mouse_pressed;
                
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                glViewport(0, 0, g_draw_w, g_draw_h);
                glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
                glOrtho(0, g_win_w, 0, g_win_h, -1, 1);
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                g_srt_overlay.render(g_gui, g_win_w, g_win_h,
                                      mouse_x, mouse_y,
                                      overlay_click, dt_seconds);
                glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
                glPopAttrib();
            }
            
            // Draw Swordfare GUI F3 Overlay if visible
            if (g_display_active && g_graphics_api == GraphicsAPI::OPENGL) {
                g_swordfare_gui.begin_frame();
                if (g_swordfare_gui.is_visible()) {
                    SwordfareDebugStats st;
                    st.fps           = fps;
                    st.dt_seconds    = dt_seconds;
                    st.frame_count   = completed_frames;
                    st.draw_calls    = g_frame_stats.draw_calls;
                    st.tex_binds     = g_frame_stats.texture_binds;
                    st.vertices      = g_frame_stats.vertices_submitted;
                    st.matrix_ops    = g_frame_stats.matrix_ops;
                    st.state_changes = g_frame_stats.state_changes;
                    st.tex_uploads   = g_frame_stats.tex_uploads;
                    st.win_w         = g_win_w;
                    st.win_h         = g_win_h;
                    st.draw_w        = g_draw_w;
                    st.draw_h        = g_draw_h;
                    st.mouse_x       = mouse_x;
                    st.mouse_y       = mouse_y;
                    st.cam_x         = g_sre_cam_x_addr ? *(float*)(g_guest_memory + g_sre_cam_x_addr) : g_cam_off_x;
                    st.cam_y         = g_sre_cam_y_addr ? *(float*)(g_guest_memory + g_sre_cam_y_addr) : g_cam_off_y;
                    st.cam_z         = g_sre_cam_z_addr ? *(float*)(g_guest_memory + g_sre_cam_z_addr) : g_cam_off_z;
                    st.cam_zoom      = 1.0f;
                    st.hero_x        = g_sre_hero_pos_x_addr ? *(float*)(g_guest_memory + g_sre_hero_pos_x_addr) : 0.0f;
                    st.hero_y        = g_sre_hero_pos_y_addr ? *(float*)(g_guest_memory + g_sre_hero_pos_y_addr) : 0.0f;
                    st.hero_z        = g_sre_hero_pos_z_addr ? *(float*)(g_guest_memory + g_sre_hero_pos_z_addr) : 0.0f;
                    st.cam_active    = g_cam_active;
                    st.typing_mode   = g_typing_mode;
                    st.game_paused   = g_game_paused;
                    st.postfx_on     = g_postfx.enabled;
                    st.postfx_preset = g_postfx.preset_name;
                    if (g_fbo_mode == FBOScale::SHARP_BILINEAR) st.scale_mode = "Sharp-Bilinear";
                    else if (g_fbo_mode == FBOScale::NEAREST) st.scale_mode = "Nearest";
                    else if (g_fbo_mode == FBOScale::CRT_SCANLINE) st.scale_mode = "CRT Scanline";
                    else if (g_fbo_mode == FBOScale::FSR) st.scale_mode = "FSR 1.0";
                    st.binary_name    = g_lib_name.c_str();
                    st.speed_label    = mod_speed_label();
                    st.graphics_api   = "OpenGL";
                    g_swordfare_gui.draw_debug(st);
                }
                
                // Draw SRE LUA buttons and overlays if present
                if (sre_btn_array_addr && g_guest_memory) {
                    void* overlays_ptr = sre_overlay_array_addr ? (g_guest_memory + sre_overlay_array_addr) : nullptr;
                    bool globally_hidden = false;
                    if (sre_btn_globally_hidden_addr) {
                        globally_hidden = (*(int*)(g_guest_memory + sre_btn_globally_hidden_addr) != 0);
                    }
                    g_swordfare_gui.draw_buttons(g_guest_memory + sre_btn_array_addr, overlays_ptr, globally_hidden);
                }
                if (gui_visible) {
                    GuiAction act = g_swordfare_gui.draw_control_panel(&gui_visible);
                    if (act != GUI_NONE) {
                        process_gui_action(act, running);
                    }
                }
                g_swordfare_gui.draw_lua_console();
                g_swordfare_gui.draw_lua_script_editor();
                g_swordfare_gui.draw_lua_script_manager();
                g_swordfare_gui.draw_mod_overlay(g_save_dir);
                g_swordfare_gui.end_frame();
            }
            
            // Present the frame
            if (g_display_active && g_display_ptr) {
#ifdef VULKAN_BACKEND
                if (g_graphics_api == GraphicsAPI::VULKAN) {
                    g_vk_backend.end_frame_and_present();
                } else
#endif
                {
                    g_display_ptr->swap();
                }

                // ========= Frame rate limiter =========
                // The original game ran at 30fps with VSync. Without a frame cap,
                // the loop runs at 500+ FPS and the dt floor clamp inflates tiny
                // deltas to 16.6ms, making everything move ~8x faster.
                // Target 60fps for smooth gameplay at correct speed.
                // In HARD MODE this is bypassed — entities run at full uncapped speed.
                {
                    bool hardmode = false;
                    if (sre_hardmode_addr) {
                        hardmode = *(volatile int*)(g_emulator_64->get_memory_base() + sre_hardmode_addr) != 0;
                    }
                    if (!hardmode) {
                        const float TARGET_FRAME_TIME_MS = 1000.0f / 60.0f;  // ~16.67ms
                        Uint64 frame_end = SDL_GetTicks();
                        float frame_elapsed_ms = (float)(frame_end - now_ticks);
                        if (frame_elapsed_ms < TARGET_FRAME_TIME_MS) {
                            SDL_Delay((uint32_t)(TARGET_FRAME_TIME_MS - frame_elapsed_ms));
                        }
                    }
                }
                // Sync text input state from SRE (replaces JNI bridge path)
                // SRE's StartTextInputWithDelegate hook sets g_sre_text_input_active = 1,
                // StopTextInputWithDelegate / textInputDidFinish set it to 0.
                if (sre_text_input_active_addr) {
                    uint8_t* mem = g_emulator_64->get_memory_base();
                    int sre_ti = *(volatile int*)(mem + sre_text_input_active_addr);
                    if (sre_ti && !g_text_input_active) {
                        g_text_input_active = true;
                        g_text_input_buffer.clear();
                        std::cout << "[TextInput] SRE: text input activated" << std::endl;
                    } else if (!sre_ti && g_text_input_active) {
                        g_text_input_active = false;
                        g_text_input_buffer.clear();
                        std::cout << "[TextInput] SRE: text input deactivated" << std::endl;
                    }
                }

                // Auto-toggle SDL text input when game requests it
                if (g_text_input_active && !g_text_input_was_active) {
                    SDL_StartTextInput(g_display_ptr->get_window());
                    std::cout << "[TextInput] SDL text input enabled" << std::endl;
                } else if (!g_text_input_active && g_text_input_was_active) {
                    SDL_StopTextInput(g_display_ptr->get_window());
                    std::cout << "[TextInput] SDL text input disabled" << std::endl;
                }
                g_text_input_was_active = g_text_input_active;

                // ARM64 text input callback helpers
                // Hooked by SRE — our reimplementation in sre_gui_native.c
                // bypasses the corrupt ITextInputDelegate vtable entirely.
                auto call_text_change_64 = [&](const std::string& text) {
                    if (!g_fw_textInputDidChange) return;
                    uint8_t* memory = g_emulator_64->get_memory_base();
                    uint32_t str_addr = write_guest_jstring(memory, text);
                    g_emulator_64->call(g_fw_textInputDidChange, {env_ptr, 0, (uint64_t)str_addr});
                };
                auto call_text_finish_64 = [&]() {
                    if (!g_fw_textInputDidFinish) return;
                    g_emulator_64->call(g_fw_textInputDidFinish, {env_ptr, 0});
                };

                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (g_graphics_api == GraphicsAPI::OPENGL && g_swordfare_gui.process_event(event)) {
                        // ImGui consumed the event
                        continue;
                    }
                    switch (event.type) {
                        case SDL_EVENT_QUIT:
                            running = false;
                            break;
                        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                            running = false;
                            break;
                        case SDL_EVENT_WINDOW_RESIZED:
                            g_win_w = event.window.data1;
                            g_win_h = event.window.data2;
                            SDL_GetWindowSizeInPixels(g_display_ptr->get_window(), &g_draw_w, &g_draw_h);
                            std::cout << "[Display] Window resized to " << g_win_w << "x" << g_win_h
                                      << " (drawable: " << g_draw_w << "x" << g_draw_h << ")" << std::endl;
                            break;
                        case SDL_EVENT_KEY_DOWN:
                            if (event.key.key == SDLK_A || event.key.key == SDLK_D) {
                                if (g_cam_active && g_cam_pov_mode) {
                                    g_cam_pov_facing = (event.key.key == SDLK_A) ? -1.0f : 1.0f;
                                    cam_write_to_guest();
                                }
                            }
                            if (should_block_keyboard()) {
                                bool is_toggle = (event.key.key == SDLK_F1 || event.key.key == SDLK_F2 || event.key.key == SDLK_F3 || event.key.key == SDLK_F4 || event.key.key == SDLK_GRAVE || event.key.key == SDLK_ESCAPE);
                                if (!is_toggle) break;
                            }
                            if (event.key.key == SDLK_F1 && !event.key.repeat) { gui_visible = !gui_visible; break; }
                            if (event.key.key == SDLK_F4 && !event.key.repeat) {
                                g_swordfare_gui.toggle_mod_overlay();
                                std::cout << "[ModOverlay] " << (g_swordfare_gui.is_mod_overlay_visible() ? "ON" : "OFF") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F2 && !event.key.repeat) {
                                g_input_config.toggle_editor();
                                if (!g_input_config.is_editing()) g_input_config.save(g_save_dir + "/controls_arm64.ini");
                                break;
                            }
                            // Controls editor: R toggles rebind mode, Delete removes custom button
                            if (g_input_config.is_editing() && !event.key.repeat) {
                                if (event.key.key == SDLK_R) {
                                    auto mode = g_input_config.get_editor_mode();
                                    g_input_config.set_editor_mode(mode == EDITOR_REBIND ? EDITOR_POSITION : EDITOR_REBIND);
                                    std::cout << "[Controls] Editor mode: " 
                                              << (g_input_config.get_editor_mode() == EDITOR_REBIND ? "REBIND" : "POSITION") << std::endl;
                                    break;
                                }
                                // Capture key for rebinding
                                if (g_input_config.editor_handle_key(event.key.scancode)) break;
                            }
                            if (event.key.key == SDLK_F3 && !event.key.repeat) {
                                debug_visible = !debug_visible;
                                if (g_graphics_api == GraphicsAPI::OPENGL) {
                                    g_swordfare_gui.toggle_visible();
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F4 && !event.key.repeat) {
                                g_fbo_mode = static_cast<FBOScale>((static_cast<int>(g_fbo_mode) + 1) % 4);
                                const char* m_name = "Unknown";
                                if (g_fbo_mode == FBOScale::SHARP_BILINEAR) m_name = "Sharp-Bilinear";
                                else if (g_fbo_mode == FBOScale::NEAREST) m_name = "Nearest";
                                else if (g_fbo_mode == FBOScale::CRT_SCANLINE) m_name = "CRT Scanline";
                                else if (g_fbo_mode == FBOScale::FSR) m_name = "FSR 1.0 (Edge-Adaptive)";
                                std::cout << "[FBO] Scale mode changed to " << m_name << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F5 && !event.key.repeat) { cam_toggle(); break; }
                            if (event.key.key == SDLK_M && !event.key.repeat) {
                                if (g_cam_active) {
                                    g_cam_pov_mode = !g_cam_pov_mode;
                                    char msg[64];
                                    snprintf(msg, sizeof(msg), "Camera POV: %s", g_cam_pov_mode ? "ON" : "OFF");
                                    mod_toast(msg, 1.5f);
                                    cam_write_to_guest();
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F6 && !event.key.repeat) {
                                g_postfx_preset = static_cast<PostFXPreset>(
                                    (static_cast<int>(g_postfx_preset) + 1) % static_cast<int>(PostFXPreset::COUNT)
                                );
                                postfx_apply_preset(g_postfx, g_postfx_preset);
                                // Update saved user state for menu auto-restore
                                postfx_user_preset = g_postfx_preset;
                                postfx_user_enabled = g_postfx.enabled;
                                postfx_suppressed_by_menu = false;
                                std::cout << "[PostFX] Preset: " << g_postfx.preset_name << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_R && !event.key.repeat) {
                                std::cout << "[PostFX] Reloading custom postfx config..." << std::endl;
                                if (postfx_load_custom_json(g_postfx)) {
                                    std::cout << "[PostFX] Successfully loaded custom config!" << std::endl;
                                } else {
                                    std::cout << "[PostFX] Failed to load custom config, generating default template." << std::endl;
                                    postfx_save_default_json();
                                    postfx_load_custom_json(g_postfx);
                                }
                                break;
                            }
                            if (event.key.key == SDLK_BACKSLASH && !event.key.repeat) {
                                g_typing_mode = !g_typing_mode;
                                if (g_typing_mode) {
                                    SDL_StartTextInput(g_display_ptr->get_window());
                                    std::cout << "[Keyboard] Typing mode ON" << std::endl;
                                } else {
                                    SDL_StopTextInput(g_display_ptr->get_window());
                                    std::cout << "[Keyboard] Typing mode OFF" << std::endl;
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F7 && !event.key.repeat) {
                                g_video_background_enabled = !g_video_background_enabled;
                                std::cout << "[VideoBackground] Video playback: " << (g_video_background_enabled ? "ENABLED" : "DISABLED") << std::endl;
                                break;
                            }
                            if (event.key.key == SDLK_F8 && !event.key.repeat) { mod_toggle_pause(); break; }
                            if (event.key.key == SDLK_F9 && !event.key.repeat) { mod_step_frame(); break; }
                            if (event.key.key == SDLK_F10 && !event.key.repeat) {
                                g_hide_touch_hud = !g_hide_touch_hud;
                                g_gl_hide_hud = g_hide_touch_hud;
                                break;
                            }
                            if (event.key.key == SDLK_F12 && !event.key.repeat) {
                                SDL_WindowFlags flags = SDL_GetWindowFlags(g_display_ptr->get_window());
                                if (flags & SDL_WINDOW_FULLSCREEN) {
                                    SDL_SetWindowFullscreen(g_display_ptr->get_window(), false);
                                    // Query actual window size — preserves native aspect ratio (16:10, etc.)
                                    int ww, wh;
                                    SDL_GetWindowSize(g_display_ptr->get_window(), &ww, &wh);
                                    g_win_w = ww; g_win_h = wh;
                                    SDL_GetWindowSizeInPixels(g_display_ptr->get_window(), &g_draw_w, &g_draw_h);
                                    std::cout << "[Display] Windowed " << ww << "x" << wh << " (drawable: " << g_draw_w << "x" << g_draw_h << ")" << std::endl;
                                } else {
                                    SDL_SetWindowFullscreen(g_display_ptr->get_window(), true);
                                    int fw, fh;
                                    SDL_GetWindowSize(g_display_ptr->get_window(), &fw, &fh);
                                    g_win_w = fw; g_win_h = fh;
                                    SDL_GetWindowSizeInPixels(g_display_ptr->get_window(), &g_draw_w, &g_draw_h);
                                    std::cout << "[Display] Fullscreen " << fw << "x" << fh << " (drawable: " << g_draw_w << "x" << g_draw_h << ")" << std::endl;
                                }
                                break;
                            }
                            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                                g_swordfare_gui.toggle_mod_overlay();
                                break;
                            }
                            // N — Time of Day cycle: Day → Afternoon → Night → Day
                            if (event.key.key == SDLK_N && !event.key.repeat && !g_typing_mode && !g_swordfare_gui.is_lua_console_open()) {
                                // 0=Day, 1=Afternoon, 2=Night
                                static int time_of_day = 0;
                                time_of_day = (time_of_day + 1) % 3;
                                
                                // Start from current PostFX preset
                                postfx_apply_preset(g_postfx, g_postfx_preset);
                                
                                if (time_of_day == 0) {
                                    // === DAY (default) ===
                                    // Restore background brightness
                                    if (bg_brightness_addr) {
                                        *(float*)(g_guest_memory + bg_brightness_addr) = 1.0f;
                                    }
                                    mod_toast("Day", 1.5f);
                                    std::cout << "[TimeOfDay] Day — default" << std::endl;
                                    
                                } else if (time_of_day == 1) {
                                    // === AFTERNOON (1PM warm light) ===
                                    // No background filter — keep BG bright and warm
                                    if (bg_brightness_addr) {
                                        *(float*)(g_guest_memory + bg_brightness_addr) = 1.1f;
                                    }
                                    g_postfx.enabled = true;
                                    // Warm vibrant color grading
                                    g_postfx.color_adjust = true;
                                    g_postfx.warmth = 0.35f;        // warm golden tone
                                    g_postfx.brightness = 0.05f;    // slightly brighter
                                    g_postfx.contrast = 1.1f;       // punchy contrast
                                    g_postfx.saturation = 1.2f;     // vivid colors
                                    // Strong god rays — intense afternoon sun
                                    g_postfx.god_rays = true;
                                    g_postfx.god_rays_intensity = 0.8f;
                                    g_postfx.god_rays_decay = 0.97f;
                                    g_postfx.sun_x = 0.75f;         // sun to the right
                                    g_postfx.sun_y = 0.7f;          // lower in sky
                                    // Strong SSAO — harsh afternoon shadows
                                    g_postfx.ssao = true;
                                    g_postfx.ssao_radius = 0.03f;
                                    g_postfx.ssao_intensity = 1.5f;
                                    // Bloom — bright sun reflections
                                    g_postfx.bloom = true;
                                    g_postfx.bloom_threshold = 0.55f;
                                    g_postfx.bloom_intensity = 0.35f;
                                    // Shadows — strong directional
                                    g_postfx.shadows = true;
                                    g_postfx.shadow_intensity = 0.55f;
                                    g_postfx.shadow_light_x = 0.5f;
                                    g_postfx.shadow_light_y = -0.6f;
                                    // Subtle vignette
                                    g_postfx.vignette = true;
                                    g_postfx.vignette_intensity = 0.15f;
                                    mod_toast("Afternoon", 1.5f);
                                    std::cout << "[TimeOfDay] Afternoon — warm, bright, strong shadows" << std::endl;
                                    
                                } else {
                                    // === NIGHT ===
                                    // Dim the SRE background renderer
                                    if (bg_brightness_addr) {
                                        *(float*)(g_guest_memory + bg_brightness_addr) = 0.3f;
                                    }
                                    g_postfx.enabled = true;
                                    // Cool blue desaturated tint
                                    g_postfx.color_adjust = true;
                                    g_postfx.brightness = -0.12f;    // dim overall
                                    g_postfx.contrast = 0.9f;        // softer contrast
                                    g_postfx.saturation = 0.6f;      // desaturate
                                    g_postfx.warmth = -0.25f;        // cool blue shift
                                    // Heavy vignette — darkness at edges
                                    g_postfx.vignette = true;
                                    g_postfx.vignette_intensity = 0.55f;
                                    // SSAO for dark corners
                                    g_postfx.ssao = true;
                                    g_postfx.ssao_radius = 0.03f;
                                    g_postfx.ssao_intensity = 1.6f;
                                    // No god rays at night — moonlight bloom instead
                                    g_postfx.god_rays = false;
                                    g_postfx.bloom = true;
                                    g_postfx.bloom_threshold = 0.5f;
                                    g_postfx.bloom_intensity = 0.4f;
                                    // Film grain for atmosphere
                                    g_postfx.film_grain = true;
                                    g_postfx.grain_intensity = 0.03f;
                                    mod_toast("Night", 1.5f);
                                    std::cout << "[TimeOfDay] Night — dim, cool, atmospheric" << std::endl;
                                }
                                break;
                            }
                            // ---- Lua Console: backtick toggles, feed keys when open ----
                            if (event.key.key == SDLK_GRAVE && !event.key.repeat && g_lua_console_ready) {
                                g_swordfare_gui.toggle_lua_console();
                                break;
                            }
                            if (event.key.key == SDLK_EQUALS && !event.key.repeat && !(event.key.mod & SDL_KMOD_SHIFT)) { mod_speed_up(); break; }
                            if (event.key.key == SDLK_MINUS && !event.key.repeat && !(event.key.mod & SDL_KMOD_SHIFT)) { mod_speed_down(); break; }
                            if (event.key.key == SDLK_0 && !event.key.repeat) { mod_speed_reset(); break; }
                            // fall through
                        case SDL_EVENT_KEY_UP: {
                            if (should_block_keyboard()) {
                                bool is_toggle = (event.key.key == SDLK_F1 || event.key.key == SDLK_F2 || event.key.key == SDLK_F3 || event.key.key == SDLK_F4 || event.key.key == SDLK_GRAVE || event.key.key == SDLK_ESCAPE);
                                if (!is_toggle) break;
                            }
                            bool is_down = (event.type == SDL_EVENT_KEY_DOWN);
                            int scancode = event.key.scancode;

                            // ---- Virtual Key translation for Android mods ----
                            bool handled_virtual = false;
                            extern bool g_sre_overlay_blocking;
                            bool block_virtual = g_sre_overlay_blocking && (event.key.key != SDLK_ESCAPE && event.key.key != SDLK_X);
                            if (!block_virtual) {
                                if (event.key.key == SDLK_TAB) {
                                    handled_virtual = simulate_virtual_key("Open", is_down);
                                } else if (event.key.key == SDLK_E) {
                                    handled_virtual = simulate_virtual_key("Use", is_down);
                                } else if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_X) {
                                    handled_virtual = simulate_virtual_key("X", is_down);
                                } else if (event.key.key == SDLK_V) {
                                    handled_virtual = simulate_virtual_key("Armory", is_down);
                                } else if (event.key.key == SDLK_K) {
                                    handled_virtual = simulate_virtual_key("Keybinds", is_down);
                                } else if (event.key.key == SDLK_S) {
                                    handled_virtual = simulate_virtual_key("Settings", is_down);
                                } else if (event.key.key == SDLK_P) {
                                    handled_virtual = simulate_virtual_key("+ Keybind", is_down);
                                }
                            }
                            if (handled_virtual) {
                                break;
                            }

                            // ---- TEXT INPUT MODE: intercept keys when game requests text input ----
                            if (g_text_input_active && is_down) {
                                if (event.key.key == SDLK_RETURN) {
                                    std::cout << "[TextInput] Enter — confirming \"" << g_text_input_buffer << "\"" << std::endl;
                                    call_text_change_64(g_text_input_buffer);
                                    call_text_finish_64();
                                    g_text_input_active = false;
                                    g_text_input_buffer.clear();
                                    // Recovery: press Back to exit the glitched text field screen
                                    if (g_fw_handleMenuBtn) {
                                        g_emulator_64->call(g_fw_handleMenuBtn, {env_ptr, 0});
                                    }
                                    break;
                                } else if (event.key.key == SDLK_ESCAPE) {
                                    std::cout << "[TextInput] Escape — cancelling + pressing Back to recover" << std::endl;
                                    g_text_input_buffer.clear();
                                    call_text_change_64("");
                                    call_text_finish_64();
                                    g_text_input_active = false;
                                    // Recovery: press the menu/back button to escape the
                                    // glitched set-name screen. The text field rendering is
                                    // broken (primary vtable corrupt), so we navigate away.
                                    if (g_fw_handleMenuBtn) {
                                        g_emulator_64->call(g_fw_handleMenuBtn, {env_ptr, 0});
                                    }
                                    break;
                                } else if (event.key.key == SDLK_BACKSPACE) {
                                    if (!g_text_input_buffer.empty()) {
                                        g_text_input_buffer.pop_back();
                                        call_text_change_64(g_text_input_buffer);
                                        std::cout << "[TextInput] Backspace → \"" << g_text_input_buffer << "\"" << std::endl;
                                    }
                                    break;
                                }
                                break; // Eat all other key_down events during text input
                            }
                            if (g_text_input_active && !is_down) break; // Eat key-up too

                            // ---- TYPING MODE: route through FWKeyboard + text buffer ----
                            if (g_typing_mode && is_down) {
                                break;
                            }
                            if (g_typing_mode && !is_down) {
                                break;
                            }

                            // Camera + arrow keys (camera-only when active, not game movement)
                            switch (event.key.key) {
                                case SDLK_LEFT:
                                    if (g_cam_active) arrow_left = is_down;
                                    break;
                                case SDLK_RIGHT:
                                    if (g_cam_active) arrow_right = is_down;
                                    break;
                                case SDLK_UP:
                                    if (g_cam_active) arrow_up = is_down;
                                    break;
                                case SDLK_DOWN:
                                    if (g_cam_active) arrow_down = is_down;
                                    break;
                                case SDLK_PAGEUP:
                                    if (g_cam_active) cam_key_pgup = is_down;
                                    break;
                                case SDLK_PAGEDOWN:
                                    if (g_cam_active) cam_key_pgdn = is_down;
                                    break;
                                case SDLK_EQUALS:
                                case SDLK_KP_PLUS:
                                    if (g_cam_active) cam_key_pgup = is_down;
                                    break;
                                case SDLK_MINUS:
                                case SDLK_KP_MINUS:
                                    if (g_cam_active) cam_key_pgdn = is_down;
                                    break;
                                case SDLK_HOME:
                                    if (is_down && g_cam_active) cam_reset();
                                    break;
                            }
                            break;
                        }
                        // --- Text input events ---
                        case SDL_EVENT_TEXT_INPUT: {
                            const char* text = event.text.text;
                            if (g_swordfare_gui.is_lua_console_open()) {
                                // ImGui handles text input now, do nothing here.
                            } else if (g_text_input_active) {
                                g_text_input_buffer += text;
                                call_text_change_64(g_text_input_buffer);
                                std::cout << "[TextInput] \"" << g_text_input_buffer << "\"" << std::endl;
                            }
                            break;
                        }
                        case SDL_EVENT_MOUSE_WHEEL:
                            if (g_cam_active) cam_scroll_zoom((float)event.wheel.y);
                            break;
                        case SDL_EVENT_MOUSE_BUTTON_DOWN:
                            if (event.button.button == SDL_BUTTON_LEFT) {
                                mouse_pressed = true;
                                mouse_x = event.button.x;
                                mouse_y = g_win_h - event.button.y;
                                // Controls editor intercepts all mouse events
                                if (g_input_config.is_editing()) {
                                    float gx = event.button.x * 960.0f / (float)g_win_w;
                                    float gy = 544.0f - (event.button.y * 544.0f / (float)g_win_h);
                                    g_input_config.editor_mouse_down(gx, gy);
                                    click_swallowed_by_gui = true;
                                    break;
                                }
                                GuiAction gui_action = GUI_NONE;
                                bool gui_consumed = false;
                                if (gui_visible && g_graphics_api != GraphicsAPI::OPENGL) {
                                    gui_action = g_gui.handle_click(mouse_x, mouse_y, g_win_w, g_win_h);
                                    gui_consumed = (gui_action != GUI_NONE)
                                                 || (mouse_y >= g_win_h - (int)(32 * g_gui.get_scale()))
                                                 || g_gui.has_modal_open();
                                }
                                if (gui_consumed) {
                                    click_swallowed_by_gui = true;
                                    process_gui_action(gui_action, running);
                                } else if (g_srt_overlay.is_visible()) {
                                    // Overlay is open — swallow click, don't send to game
                                    click_swallowed_by_gui = true;
                                } else {
                                    bool btn_hit = false;
                                    if (g_graphics_api == GraphicsAPI::OPENGL) {
                                        ImGuiIO& io = ImGui::GetIO();
                                        if (io.WantCaptureMouse || g_swordfare_gui.is_input_blocked((float)event.button.x, (float)event.button.y)) {
                                            btn_hit = true;
                                        }
                                    }
                                    if (btn_hit) {
                                        click_swallowed_by_gui = true;
                                    } else {
                                        click_swallowed_by_gui = false;
                                        float x = event.button.x * 960.0f / (float)g_win_w;
                                        float y = 544.0f - (event.button.y * 544.0f / (float)g_win_h);
                                        last_mouse_x = x; last_mouse_y = y;
                                        call_touch_64(1, 1, accumulated_time, x, y, x, y, 1);
                                    }
                                }
                            }
                            break;
                        case SDL_EVENT_MOUSE_MOTION:
                            mouse_x = event.motion.x;
                            mouse_y = g_win_h - event.motion.y;
                            if (g_input_config.is_editing() && mouse_pressed) {
                                float gx = event.motion.x * 960.0f / (float)g_win_w;
                                float gy = 544.0f - (event.motion.y * 544.0f / (float)g_win_h);
                                g_input_config.editor_mouse_move(gx, gy);
                            } else if (mouse_pressed && !click_swallowed_by_gui && !g_srt_overlay.is_visible()) {
                                float x = event.motion.x * 960.0f / (float)g_win_w;
                                float y = 544.0f - (event.motion.y * 544.0f / (float)g_win_h);
                                call_touch_64(4, 1, accumulated_time, x, y, last_mouse_x, last_mouse_y, 0);
                                last_mouse_x = x; last_mouse_y = y;
                            }
                            break;
                        case SDL_EVENT_MOUSE_BUTTON_UP:
                            if (event.button.button == SDL_BUTTON_LEFT) {
                                mouse_pressed = false;
                                mouse_x = event.button.x;
                                mouse_y = g_win_h - event.button.y;
                                if (g_input_config.is_editing()) {
                                    g_input_config.editor_mouse_up();
                                } else if (!click_swallowed_by_gui && !g_srt_overlay.is_visible()) {
                                    float x = event.button.x * 960.0f / (float)g_win_w;
                                    float y = 544.0f - (event.button.y * 544.0f / (float)g_win_h);
                                    call_touch_64(2, 1, accumulated_time, x, y, last_mouse_x, last_mouse_y, 0);
                                    last_mouse_x = -1.0f; last_mouse_y = -1.0f;
                                }
                                click_swallowed_by_gui = false;
                            }
                            break;
                        // --- Multi-touch / finger events (touchscreen support) ---
                        case SDL_EVENT_FINGER_DOWN: {
                            float x = event.tfinger.x * 960.0f;
                            float y = (1.0f - event.tfinger.y) * 544.0f;
                            int finger_id = (int)(event.tfinger.fingerID % 10) + 20;
                            call_touch_64(1, finger_id, accumulated_time, x, y, x, y, 1);
                            break;
                        }
                        case SDL_EVENT_FINGER_UP: {
                            float x = event.tfinger.x * 960.0f;
                            float y = (1.0f - event.tfinger.y) * 544.0f;
                            int finger_id = (int)(event.tfinger.fingerID % 10) + 20;
                            call_touch_64(2, finger_id, accumulated_time, x, y, x, y, 0);
                            break;
                        }
                        case SDL_EVENT_FINGER_MOTION: {
                            float x = event.tfinger.x * 960.0f;
                            float y = (1.0f - event.tfinger.y) * 544.0f;
                            float old_x = (event.tfinger.x - event.tfinger.dx) * 960.0f;
                            float old_y = (1.0f - (event.tfinger.y - event.tfinger.dy)) * 544.0f;
                            int finger_id = (int)(event.tfinger.fingerID % 10) + 20;
                            call_touch_64(4, finger_id, accumulated_time, x, y, old_x, old_y, 0);
                            break;
                        }
                        case SDL_EVENT_GAMEPAD_ADDED:
                            if (!g_gamepad) {
                                g_gamepad = SDL_OpenGamepad(event.gdevice.which);
                                if (g_gamepad)
                                    std::cout << "[Input] Gamepad connected: " << SDL_GetGamepadName(g_gamepad) << std::endl;
                            }
                            break;
                        case SDL_EVENT_GAMEPAD_REMOVED:
                            if (g_gamepad && event.gdevice.which == SDL_GetJoystickID(SDL_GetGamepadJoystick(g_gamepad))) {
                                SDL_CloseGamepad(g_gamepad);
                                g_gamepad = nullptr;
                                std::cout << "[Input] Gamepad disconnected" << std::endl;
                            }
                            break;
                    }
                }
                
                // Keyboard→touch button dispatch (game buttons like WASD, space, etc.)
                {
                    extern bool g_sre_overlay_blocking;
                    bool input_blocked = g_sre_overlay_blocking || g_typing_mode || g_swordfare_gui.is_lua_console_open() || g_text_input_active;
                    const bool* keys = input_blocked ? nullptr : SDL_GetKeyboardState(nullptr);
                    for (int bi = 0; bi < g_input_config.button_count(); bi++) {
                        TouchButton* btn = g_input_config.get_button(bi);
                        if (!btn) continue;
                        
                        // Skip buttons during controls editor
                        if (g_input_config.is_editing()) continue;
                        
                        // Check both primary and alt key bindings
                        bool is_down = false;
                        if (keys) {
                            if (btn->sdl_scancode > 0 && btn->sdl_scancode < 512) {
                                bool block_primary = g_cam_active && 
                                    (btn->sdl_scancode == SDL_SCANCODE_LEFT || btn->sdl_scancode == SDL_SCANCODE_RIGHT ||
                                     btn->sdl_scancode == SDL_SCANCODE_UP || btn->sdl_scancode == SDL_SCANCODE_DOWN);
                                if (!block_primary) {
                                    is_down = keys[btn->sdl_scancode];
                                }
                            }
                            if (!is_down && btn->sdl_scancode_alt > 0 && btn->sdl_scancode_alt < 512) {
                                bool block_alt = g_cam_active && 
                                    (btn->sdl_scancode_alt == SDL_SCANCODE_LEFT || btn->sdl_scancode_alt == SDL_SCANCODE_RIGHT ||
                                     btn->sdl_scancode_alt == SDL_SCANCODE_UP || btn->sdl_scancode_alt == SDL_SCANCODE_DOWN);
                                if (!block_alt) {
                                    is_down = keys[btn->sdl_scancode_alt];
                                }
                            }
                        }
                        
                        if (is_down && !btn->is_pressed) {
                            btn->is_pressed = true;
                            
                            if (btn->is_macro && btn->macro_open_touch_id >= 0) {
                                // Macro step 1: press the magic menu button to open spell selection
                                TouchButton* opener = nullptr;
                                for (int j = 0; j < g_input_config.button_count(); j++) {
                                    TouchButton* t = g_input_config.get_button(j);
                                    if (t && t->touch_id == btn->macro_open_touch_id) { opener = t; break; }
                                }
                                if (opener) {
                                    std::cout << "[Macro] Step 1: press " << opener->name 
                                              << " at (" << opener->game_x << ", " << opener->game_y 
                                              << ") touch_id=" << opener->touch_id << std::endl;
                                    // Press opener — process_macros releases at 50ms (stage 1→2)
                                    int macro_tid = 100 + btn->touch_id;
                                    call_touch_64(1, macro_tid, accumulated_time,
                                        opener->game_x, opener->game_y, opener->game_x, opener->game_y, 1);
                                }
                                // Start macro sequence
                                btn->macro_pending = true;
                                btn->macro_stage = 1;  // opener pressed, waiting for release
                                btn->macro_fire_time = (uint64_t)(accumulated_time * 1000.0);
                                std::cout << "[Macro] Started: " << btn->name 
                                          << " delay=" << btn->macro_delay_ms << "ms" << std::endl;
                            } else {
                                // Normal button press
                                call_touch_64(1, btn->touch_id, accumulated_time,
                                    btn->game_x, btn->game_y, btn->game_x, btn->game_y, 1);
                            }
                        } else if (!is_down && btn->is_pressed) {
                            btn->is_pressed = false;
                            if (!btn->is_macro) {
                                call_touch_64(2, btn->touch_id, accumulated_time,
                                    btn->game_x, btn->game_y, btn->game_x, btn->game_y, 0);
                            }
                        }
                    }
                    
                    // Process pending macro step 2 (delayed spell slot tap)
                    g_input_config.process_macros(accumulated_time,
                        [&](int action, int id, double time, float x, float y, float ox, float oy, int tap) {
                            call_touch_64(action, id, time, x, y, ox, oy, tap);
                        });
                }
            }
            
            // FPS counter
            fps_frame_count++;
            Uint64 fps_now = SDL_GetTicks();
            if (fps_now - fps_last_time >= 1000) {
                fps = fps_frame_count * 1000.0f / (fps_now - fps_last_time);
                fps_frame_count = 0;
                fps_last_time = fps_now;
            }
            
            completed_frames++;
            total_draw_calls += g_frame_stats.draw_calls;
            total_tex_binds += g_frame_stats.texture_binds;
            total_tex_uploads += g_frame_stats.tex_uploads;
            total_matrix_ops += g_frame_stats.matrix_ops;
            total_state_changes += g_frame_stats.state_changes;
            total_asset_opens += g_frame_stats.asset_opens;
            
            if (completed_frames == 1 || completed_frames == 10 || 
                completed_frames <= 20 ||
                completed_frames % 100 == 0) {
                std::cout << "[Frame64 " << completed_frames << "] "
                          << "draws=" << g_frame_stats.draw_calls
                          << " tex_binds=" << g_frame_stats.texture_binds
                          << " tex_ups=" << g_frame_stats.tex_uploads
                          << " verts=" << g_frame_stats.vertices_submitted
                          << " vtx_calls=" << g_frame_stats.vertex_pointer_calls
                          << " texc_calls=" << g_frame_stats.texcoord_pointer_calls
                          << " matrix=" << g_frame_stats.matrix_ops
                          << " state=" << g_frame_stats.state_changes
                          << " assets=" << g_frame_stats.asset_opens
                          << " clears=" << g_frame_stats.clear_calls
                          << std::endl;
                draw_batcher_print_stats();
            }

            // Poll lua_resume error counter from SRE
            if (g_sre_resume_err_count_addr) {
                int count = *(int32_t*)(g_guest_memory + g_sre_resume_err_count_addr);
                if (count > g_sre_resume_err_last_seen) {
                    std::string err_msg = "(unknown)";
                    if (g_sre_resume_last_err_addr) {
                        const char* msg = (const char*)(g_guest_memory + g_sre_resume_last_err_addr);
                        if (msg[0]) err_msg = msg;
                    }
                    std::cerr << "[SRE] lua_resume error #" << count
                              << " (frame " << completed_frames << "): "
                              << err_msg << std::endl;
                    g_sre_resume_err_last_seen = count;
                }
            }

            // Poll __cxa_throw unrecovered caller from SRE
            if (g_sre_cxa_unrecovered_addr) {
                int count = *(int32_t*)(g_guest_memory + g_sre_cxa_unrecovered_addr);
                if (count > g_sre_cxa_last_seen) {
                    uint64_t caller = *(uint64_t*)(g_guest_memory + g_sre_cxa_caller_addr);
                    uint64_t nm_offset = caller > 0x1000000 ? caller - 0x1000000 : caller;
                    std::cerr << "[SRE] !! __cxa_throw UNRECOVERED #" << count
                              << " (frame " << completed_frames << ")"
                              << " caller=0x" << std::hex << caller
                              << " (nm offset 0x" << nm_offset << ")"
                              << std::dec << std::endl;
                    g_sre_cxa_last_seen = count;
                }
            }

            // Poll lua_call_safe errors from SRE
            if (g_sre_lua_error_count_addr) {
                int err_count = *(int32_t*)(g_guest_memory + g_sre_lua_error_count_addr);
                if (err_count > g_sre_lua_error_last_seen && g_sre_lua_error_buf_addr) {
                    const char* err_msg = (const char*)(g_guest_memory + g_sre_lua_error_buf_addr);
                    std::cerr << "[SRE-LUA] Error #" << err_count
                              << " (frame " << completed_frames << "): "
                              << std::string(err_msg, strnlen(err_msg, 200)) << std::endl;
                    g_sre_lua_error_last_seen = err_count;
                }
            }
            
            if (TARGET_FRAMES > 0 && completed_frames >= TARGET_FRAMES) running = false;
        }

        g_input_config.save(g_save_dir + "/controls_arm64.ini");
        if (g_gamepad) {
            SDL_CloseGamepad(g_gamepad);
            g_gamepad = nullptr;
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "[RESULT64] Completed " << completed_frames << " frames (ARM64)" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  Total draw calls:      " << total_draw_calls << std::endl;
        std::cout << "  Total tex binds:       " << total_tex_binds << std::endl;
        std::cout << "  Avg draw/frame:        " << (completed_frames ? total_draw_calls / completed_frames : 0) << std::endl;
        std::cout << "========================================" << std::endl;
    }
}



Display* g_display_ptr = nullptr;
char** g_saved_argv = nullptr;
int g_saved_argc = 0;

int main(int argc, char* argv[]) {
    g_saved_argc = argc;
    g_saved_argv = argv;
    // Check for --headless flag
    bool headless = false;
#ifdef HEADLESS_DEFAULT
    headless = true;
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) headless = true;
#ifdef VULKAN_BACKEND
        if (strcmp(argv[i], "--vulkan") == 0) g_graphics_api = GraphicsAPI::VULKAN;
#endif
        if (strcmp(argv[i], "--engine=dynarmic") == 0 || strcmp(argv[i], "--dynarmic") == 0) {
            g_use_dynarmic = true;
            std::cout << "[Main] Engine: Dynarmic JIT" << std::endl;
        }
        if (strcmp(argv[i], "--sre") == 0) {
            g_use_sre = true;
            std::cout << "[Main] SRE: Enabled" << std::endl;
        }
        if (strcmp(argv[i], "--no-sre") == 0) {
            g_use_sre = false;
            std::cout << "[Main] SRE: Disabled" << std::endl;
        }
        if (strcmp(argv[i], "--test-lib") == 0) {
            g_lib_name = "engine/v1.4.12/armeabi-v7a/libswordigo.so";
            std::cout << "[Main] Using v1.4.12 ARM32" << std::endl;
        }
        if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            g_lib_name = argv[++i];
            std::cout << "[Main] Custom lib: " << g_lib_name << std::endl;
        }
        if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            g_assets_dir = argv[++i];
            std::cout << "[Main] Custom assets: " << g_assets_dir << std::endl;
        }
        // Generate manifest.json from engine/ dir and exit
        // Usage: swordigo-desktop --generate-manifest [engine_dir] [output_path]
        if (strcmp(argv[i], "--generate-manifest") == 0) {
            std::string eng_path = (i + 1 < argc) ? argv[i + 1] : "engine";
            std::string out_path = (i + 2 < argc) ? argv[i + 2] : eng_path + "/manifest.json";
            BinarySelector gen;
            gen.generate_manifest(eng_path, out_path);
            return 0;
        }
    }
    
    // Set up user-writable directory paths
#ifdef _WIN32
    std::string appdata = getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : ".";
    std::string base_dir = appdata + "/swordigo-desktop";
#else
    std::string home_dir = getenv("HOME") ? getenv("HOME") : ".";
    std::string xdg_data = getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME") : (home_dir + "/.local/share");
    std::string base_dir = xdg_data + "/swordigo-desktop";
#endif

    fs::create_directories(base_dir + "/save");
    fs::create_directories(base_dir + "/cache");
    g_save_dir = base_dir + "/save";
    g_cache_dir = base_dir + "/cache";
    std::cout << "[Main] Save directory: " << g_save_dir << std::endl;
    std::cout << "[Main] Cache directory: " << g_cache_dir << std::endl;
    
    Display display;
    
    // ========================================================================
    // First-run setup: copy data from system install to user dir
    // (like Minecraft: /usr/share/swordigo → ~/.local/share/swordigo-desktop/)
    // ========================================================================
    ensure_user_data();
    
    // ========================================================================
    // Binary discovery (manifest-first, scan-fallback)
    // ========================================================================
    // Priority: user data dir > CWD > env override > system install
    std::string data_dir = base_dir;  // ~/.local/share/swordigo-desktop/
    const char* env_dir = getenv("SWORDIGO_DATA_DIR");
    if (env_dir) data_dir = env_dir;
    
    // If user data dir has no engine/, fall back to CWD (dev mode)
    if (!fs::exists(data_dir + "/engine")) {
        if (fs::exists("./engine")) {
            data_dir = ".";
            std::cout << "[Boot] Using CWD for game data (dev mode)" << std::endl;
        }
    }
    
    // Paths for manifest-based loading
    std::string engine_dir = data_dir + "/engine";
    std::string system_manifest = engine_dir + "/manifest.json";
    std::string user_config_dir;
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config) {
        user_config_dir = std::string(xdg_config) + "/swordigo-desktop";
    } else {
        const char* home = getenv("HOME");
        user_config_dir = std::string(home ? home : ".") + "/.config/swordigo-desktop";
    }
    std::string user_instances_path = user_config_dir + "/instances.json";
    std::string registry_path = base_dir + "/swordigo_binaries.json";
    
    // Set data_dir on BinarySelector so save_instance_ini() can use it
    g_binary_selector.set_data_dir(data_dir);
    
    // Strategy: always use INI-based scan on engine/ dir if exists
    if (fs::exists(engine_dir) && fs::is_directory(engine_dir)) {
        std::cout << "[Boot] Scanning engine directory for instance.ini files..." << std::endl;
        g_binary_selector.load_manifest(system_manifest);
    } else {
        // Legacy: flat directory scan
        std::cout << "[Boot] No engine/ dir, flat scan: " << data_dir << std::endl;
        g_binary_selector.scan_directory(data_dir);
        if (data_dir != "." && data_dir != base_dir) {
            g_binary_selector.scan_directory(base_dir);
        }
    }
    
    // Always load user-added custom instances
    g_binary_selector.load_user_instances(user_instances_path);
    
    // Load old-format registry for backward compat
    g_binary_selector.load_registry(registry_path);

    // ========================================================================
    // Show unified launcher GUI (graphics API + binary selection)
    // ========================================================================
    if (!headless) {
        bool skip_launcher = false;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--vulkan") == 0 || strcmp(argv[i], "--no-launcher") == 0 ||
                strcmp(argv[i], "--lib") == 0) {
                skip_launcher = true;
                break;
            }
        }
        if (!skip_launcher) {
            LaunchConfig lconf = show_launcher(g_binary_selector);
            if (!lconf.should_launch) {
                std::cout << "[Main] Launch cancelled by user" << std::endl;
                // Save any instances created during this session
                g_binary_selector.save_user_instances(user_instances_path);
                return 0;
            }
            g_graphics_api = lconf.graphics_api;
            g_lib_name = lconf.selected_binary;
            g_assets_dir = lconf.assets_dir;
            g_use_dynarmic = lconf.use_dynarmic;
            g_use_sre = lconf.use_sre;
            // Re-initialize the asset manager with the correct assets directory
            // (asset_manager_init was called at startup with the default "assets" path)
            asset_manager_init(get_data_path(g_assets_dir).c_str());
            asset_manager_init_arm32(get_data_path(g_assets_dir).c_str());
            g_binary_selector.set_loaded(g_lib_name);
            
            // Persist any user instances created during this session
            g_binary_selector.save_user_instances(user_instances_path);
            std::cout << "[Main] Launcher: " 
                      << (g_graphics_api == GraphicsAPI::VULKAN ? "Vulkan" : "OpenGL")
                      << " | Engine: " << (g_use_dynarmic ? "Dynarmic" : "Unicorn")
                      << " | Binary: " << g_lib_name
                      << " | Assets: " << g_assets_dir << std::endl;
        }
    }

    if (!headless) {
#ifdef VULKAN_BACKEND
        if (g_graphics_api == GraphicsAPI::VULKAN) {
            if (display.init_vulkan(GAME_W, GAME_H, "Swordigo")) {
                g_display_active = true;
                g_display_ptr = &display;
                if (!g_vk_backend.init(display.get_window(), GAME_W, GAME_H)) {
                    std::cerr << "[Main] Vulkan backend init failed, falling back to OpenGL" << std::endl;
                    g_graphics_api = GraphicsAPI::OPENGL;
                    // Recreate as OpenGL window
                    display = Display();
                    if (display.init(GAME_W, GAME_H, "Swordigo")) {
                        g_display_active = true;
                        g_display_ptr = &display;
                    }
                } else {
                    std::cout << "[Main] Vulkan backend initialized" << std::endl;
                }
            }
        } else
#endif
        // Get the display's native resolution for correct aspect ratio (16:10, 16:9, etc.)
        int native_w = 1920, native_h = 1080;
        {
            SDL_DisplayID display_id = SDL_GetPrimaryDisplay();
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display_id);
            if (mode) {
                native_w = mode->w;
                native_h = mode->h;
                std::cout << "[Main] Native display: " << native_w << "x" << native_h 
                          << " @ " << mode->refresh_rate << "Hz" << std::endl;
            }
        }
        if (display.init(native_w, native_h, "Swordigo")) {
            g_display_active = true;
            g_display_ptr = &display;
            // Get physical drawable dimensions (HiDPI)
            SDL_GetWindowSizeInPixels(display.get_window(), &g_draw_w, &g_draw_h);

            // --- Dynamic render resolution: match drawable pixels & aspect ratio ---
            // Use the physical drawable size directly as the game render resolution.
            // This gives 1:1 pixel mapping — no upscaling, maximum sharpness.
            // The aspect ratio naturally matches the display (16:10, 16:9, etc.)
            GAME_W = g_draw_w;
            GAME_H = g_draw_h;
            // Clamp to reasonable range: at least 1920 wide, at most 4096 wide
            if (GAME_W < 1920) { GAME_W = 1920; GAME_H = (int)(1920.0f * g_draw_h / g_draw_w); }
            if (GAME_W > 4096) { float s = 4096.0f / GAME_W; GAME_W = 4096; GAME_H = (int)(GAME_H * s); }
            // Ensure even dimensions (GPU-friendly)
            GAME_W &= ~1;
            GAME_H &= ~1;
            // Recompute touch coordinate scaling
            TOUCH_SCALE_X = (float)GAME_W / 960.0f;
            TOUCH_SCALE_Y = (float)GAME_H / 544.0f;

            std::cout << "[Main] Display initialized - native " << native_w << "x" << native_h
                      << " | drawable: " << g_draw_w << "x" << g_draw_h
                      << " | render: " << GAME_W << "x" << GAME_H
                      << " | aspect: " << std::fixed << std::setprecision(3)
                      << ((float)GAME_W / GAME_H) << std::endl;
            if (g_graphics_api == GraphicsAPI::OPENGL) {
                g_swordfare_gui.init(display.get_window(), SDL_GL_GetCurrentContext());
            }
        } else {
            std::cerr << "[Main] Display init failed, falling back to headless" << std::endl;
        }
    } else {
        std::cout << "[Main] Running in headless mode" << std::endl;
    }

    // ========================================================================
    // Binary fallback (only if launcher was skipped via CLI flags)
    // ========================================================================
    {
        const auto& bins = g_binary_selector.get_binaries();
        if (g_lib_name.find("engine/") != std::string::npos || g_lib_name.find("/") != std::string::npos) {
            // Full path specified (engine/ structure or CLI --lib path)
            std::cout << "[BinSel] Using: " << g_lib_name << std::endl;
            g_binary_selector.set_loaded(g_lib_name);
        } else if (!bins.empty()) {
            g_lib_name = g_binary_selector.get_default();
            g_binary_selector.set_loaded(g_lib_name);
            std::cout << "[BinSel] Using default: " << g_lib_name << std::endl;
        }
        g_binary_selector.save_registry(registry_path);
        g_binary_selector.save_user_instances(user_instances_path);
    }

    init_all();

    // Detect architecture from binary ELF header
    {
        std::string so_path = get_data_path(g_lib_name);
        g_is_arm64 = is_arm64_binary(so_path);
        std::cout << "[Boot] Architecture: " << (g_is_arm64 ? "ARM64" : "ARM32") << std::endl;
    }
    
    if (g_is_arm64) {
        load_and_boot_arm64();
    } else {
        load_and_boot();
    }

    // Cleanup FBO/Vulkan and background IO thread
    if (g_display_active) {
#ifdef VULKAN_BACKEND
        if (g_graphics_api == GraphicsAPI::VULKAN) {
            g_vk_backend.destroy();
        } else
#endif
        {
            g_swordfare_gui.shutdown();
            fbo_destroy();
        }
    }
    VideoBackground::cleanup();
    io_thread_stop();
    
    std::cout << "[Main] Swordigo — session complete." << std::endl;
    
    return 0;
}

extern "C" const char* sre_resolve_symbol(uint64_t addr) {
    // Search in g_main_mod_64
    for (int i = 0; i < g_main_mod_64.num_dynsym; i++) {
        if (g_main_mod_64.dynsym[i].st_name == 0) continue;
        uint64_t val = g_main_mod_64.base_addr + g_main_mod_64.dynsym[i].st_value;
        uint64_t size = g_main_mod_64.dynsym[i].st_size;
        if (size == 0) size = 8; // fallback size
        if (addr >= val && addr < val + size) {
            return g_main_mod_64.dynstr + g_main_mod_64.dynsym[i].st_name;
        }
    }
    // Search in g_sre_mod
    for (int i = 0; i < g_sre_mod.num_dynsym; i++) {
        if (g_sre_mod.dynsym[i].st_name == 0) continue;
        uint64_t val = g_sre_mod.base_addr + g_sre_mod.dynsym[i].st_value;
        uint64_t size = g_sre_mod.dynsym[i].st_size;
        if (size == 0) size = 8; // fallback size
        if (addr >= val && addr < val + size) {
            return g_sre_mod.dynstr + g_sre_mod.dynsym[i].st_name;
        }
    }
    return nullptr;
}

