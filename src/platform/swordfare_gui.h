// =============================================================================
// Swordfare GUI — Modern ImGui-Based In-Game Overlay  (v7.3)
//
// A sleek, modern overlay rendered on top of the running game using Dear ImGui
// (SDL3 + OpenGL3 backends). Replaces the old bitmap-drawn F3 debug panel with
// a proper dockable, styled ImGui window.
//
// Design goals:
//   • Non-bitmap: uses ImGui vector rendering — crisp at all resolutions.
//   • Dark + glassmorphism-inspired theme consistent with the launcher UI.
//   • Minimal overhead — only renders when visible.
//   • Easily extensible: future panels (controls editor, camera, mods) can be
//     added as new SwardfarePanel subclasses without touching main.cpp.
//
// Usage:
//   SwordfareGUI gui;
//   gui.init(sdl_window, gl_context);        // call once after GL context ready
//   // per-frame (after game draw, before swap):
//   gui.begin_frame();
//   if (gui.is_visible()) gui.draw_debug(fps, dt, ...);
//   gui.end_frame();
//   gui.shutdown();                          // call on exit
// =============================================================================

#pragma once

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"

#ifdef VULKAN_BACKEND
#include "volk.h"
#else
// Stub so VkDescriptorPool / VK_NULL_HANDLE exist without the full Vulkan SDK
typedef struct VkDescriptorPool_T* VkDescriptorPool;
#define VK_NULL_HANDLE nullptr
#endif

#include "platform/srt_overlay.h"
#include "platform/gui.h"
#include <vector>
#include <string>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>

// ---------------------------------------------------------------------------
// Debug stats snapshot (passed to draw_debug each frame)
// ---------------------------------------------------------------------------
struct SwordfareDebugStats {
    float   fps           = 0.0f;
    float   dt_seconds    = 0.0f;
    int     frame_count   = 0;
    int     draw_calls    = 0;
    int     tex_binds     = 0;
    int     vertices      = 0;
    int     matrix_ops    = 0;
    int     state_changes = 0;
    int     tex_uploads   = 0;
    int     win_w         = 0, win_h = 0;
    int     draw_w        = 0, draw_h = 0;
    int     mouse_x       = 0, mouse_y = 0;
    float   cam_x         = 0.0f, cam_y = 0.0f, cam_z = 0.0f;
    float   cam_zoom      = 1.0f;
    float   hero_x        = 0.0f, hero_y = 0.0f, hero_z = 0.0f;
    bool    cam_active    = false;
    bool    typing_mode   = false;
    bool    game_paused   = false;
    bool    postfx_on     = false;
    const char* postfx_preset  = "Off";
    const char* scale_mode     = "Sharp-Bilinear";
    const char* binary_name    = "";
    const char* speed_label    = "1×";
    const char* graphics_api   = "OpenGL";
};

struct ImGuiInputTextCallbackData;

// ---------------------------------------------------------------------------
// SwordfareGUI — main class
// ---------------------------------------------------------------------------
class SwordfareGUI {
public:
    SwordfareGUI()  = default;
    ~SwordfareGUI() = default;

    // Lifecycle
    void init(SDL_Window* window, SDL_GLContext gl_ctx);
    void init_vulkan(SDL_Window* window, class VulkanBackend* vk_backend);
    void shutdown();
    bool is_initialized() const { return m_initialized; }

    // Event pass-through — call before your own SDL event handling
    // Returns true if ImGui consumed the event (caller should not process it).
    bool process_event(const SDL_Event& event);

    // Frame management — wrap the game's GL draw calls
    void begin_frame();
    void end_frame();     // renders ImGui draw data into the GL context

    // Toggle visibility (called on F3)
    void toggle_visible() { m_visible = !m_visible; }
    bool is_visible()     const { return m_visible; }

    // Mod Overlay (Kiwi-compatible custom menu) visibility
    void toggle_mod_overlay() { m_mod_overlay_visible = !m_mod_overlay_visible; }
    bool is_mod_overlay_visible() const { return m_mod_overlay_visible; }
    void set_mod_overlay_visible(bool visible) { m_mod_overlay_visible = visible; }

    // Draw the debug panel — call between begin_frame()/end_frame() when visible
    void draw_debug(const SwordfareDebugStats& stats);

    // Draw the custom LUA-registered buttons and overlays using ImGui vector elements
    void draw_buttons(void* guest_buttons_ptr, void* guest_overlays_ptr, bool globally_hidden = false);

    // Draw the Lua script editor
    void draw_lua_script_editor();

    // Draw the Lua script manager
    void draw_lua_script_manager();

    // Draw the remastered mod manager overlay
    void draw_mod_overlay(const std::string& save_dir);

    // Draw the F1 modern control panel/overlay
    GuiAction draw_control_panel(bool* p_open);

    void draw_about_panel(bool* p_open, float top_offset);
    void draw_help_panel(bool* p_open, float top_offset);

    // Draw the settings panel
    GuiAction draw_settings_panel(bool* p_open);
    bool m_show_about = false;
    bool m_show_help  = false;

    // ---- Lua Console (ImGui-native, replaces old bitmap console) ----
    //
    // Call init_lua_console() once when SRE console addrs are resolved.
    // draw_lua_console() is called every frame from main loop.
    // submit_lua_console() can be called externally (e.g. from keyboard handler).
    //
    struct ConsoleEntry { std::string text; bool is_error; bool is_input; };

    void init_lua_console(
        uint8_t* guest_memory,
        uint64_t buf_addr,
        uint64_t result_addr,
        uint64_t pending_addr,
        uint64_t status_addr,
        uint64_t print_addr);

    // Returns true if a command was consumed (caller should not re-process key).
    bool lua_console_key(SDL_Keycode key, const std::string& text_input);
    // Append text from SDL_EVENT_TEXT_INPUT
    void lua_console_text(const char* text);

    // Draw the full ImGui terminal window. Call between begin_frame()/end_frame().
    void draw_lua_console();

    bool is_lua_console_open() const { return m_console_open; }
    bool is_lua_console_ready() const { return m_console_ready; }
    void toggle_lua_console();
    void update_console_backend();

    // Returns true if coordinates fall inside any active overlay, button, or console
    bool is_input_blocked(float mx, float my);

private:
    // Internal helpers
    void apply_swordfare_theme();
    void scan_saves(const std::string& save_dir);
    bool load_save(const std::string& path);
    bool write_save(const std::string& path);
    void console_submit(const std::string& cmd); // write to guest + set pending
    static int console_input_callback(ImGuiInputTextCallbackData* data);
    int on_console_input_callback(ImGuiInputTextCallbackData* data);

    SDL_Window*   m_window   = nullptr;
    SDL_GLContext m_gl_ctx   = nullptr;
    class VulkanBackend* m_vk_backend = nullptr;
    bool                 m_vulkan_active = false;
    VkDescriptorPool     m_imgui_vk_descriptor_pool = VK_NULL_HANDLE;
    void*         m_imgui_ctx = nullptr;   // ImGuiContext*
    void*         m_font_main = nullptr;   // ImFont*
    void*         m_font_button = nullptr; // ImFont*
    void*         m_font_mono = nullptr;   // ImFont* (monospace for console)

    bool          m_initialized = false;
    bool          m_visible     = false;
    bool          m_mod_overlay_visible = false;

    void*         m_last_buttons_ptr = nullptr;
    void*         m_last_overlays_ptr = nullptr;

    // Save editor state
    std::string              m_save_dir;
    std::vector<std::string> m_save_files;
    int                      m_selected_save = -1;
    InventoryState           m_inventory;
    bool                     m_inventory_dirty = false;
    std::string              m_status_msg;
    float                    m_status_timer = 0.0f;

    // Smooth FPS graph data
    static constexpr int FPS_HISTORY = 90;
    float m_fps_history[FPS_HISTORY] = {};
    int   m_fps_idx = 0;

    // ---- Lua Console state ----
    bool          m_console_ready  = false;
    bool          m_console_open   = false;
    bool          m_console_focus  = false; // request ImGui focus next frame

    uint8_t*      m_guest_memory   = nullptr;
    uint64_t      m_console_buf_addr     = 0;
    uint64_t      m_console_result_addr  = 0;
    uint64_t      m_console_pending_addr = 0;
    uint64_t      m_console_status_addr  = 0;
    uint64_t      m_console_print_addr   = 0;

    static constexpr int CONSOLE_MAX_HISTORY = 4096;

    std::vector<ConsoleEntry>  m_console_history;
    char                       m_console_input[16384] = {};
    std::vector<std::string>   m_console_cmd_history; // up-arrow recall
    int                        m_console_hist_idx = -1;
    bool                       m_console_scroll_bottom = false;

    // ---- Lua Script Editor ----
    bool                       m_script_editor_open = false;
    char                       m_script_editor_name[128] = "mod_script.lua";
    char                       m_script_editor_buf[32768] = "";
    std::string                m_script_editor_status = "Ready";
    float                      m_script_editor_status_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};

    // ---- Lua Script Manager ----
    struct LuaScriptMeta {
        std::string filename;
        bool valid;
    };
    bool                       m_script_manager_open = false;
    std::vector<LuaScriptMeta> m_script_list;

    // ---- TCP Console Server (openport command) ----
    void start_tcp_server(int port);
    void stop_tcp_server();
    void tcp_server_loop();

    std::thread                m_tcp_thread;
    std::atomic<bool>          m_tcp_running{false};
    int                        m_tcp_server_fd = -1;
    std::atomic<int>           m_tcp_client_fd{-1};
    std::string                m_tcp_pending_cmd;
    std::mutex                 m_tcp_mutex;
    bool                       m_tcp_cmd_in_flight = false;
};
