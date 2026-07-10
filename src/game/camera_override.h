#pragma once
#include <cstdint>
#include <cstring>

// Forward declaration — full definition in camera_override.cpp
class Emulator;

// ============================================================
//  SwordigoDesktop — Camera Override System (Modernized)
//  Based on SwMini's camera approach + custom desktop enhancements.
//
//  Architecture:
//    - CameraController 'this' captured via constructor hook
//    - CameraController::FocusAtPoint called each frame
//    - Dt-based speed with acceleration for smooth feel
//    - Mouse wheel zoom, smooth interpolation mode
//    - Camera presets (save/load positions)
//
//  Engine Symbols (from readelf -s libswordigo.so):
//    CameraController::CameraController()  0x002e35c5
//    CameraController::FocusAtPoint(...)   0x002e3a25
//    CameraController::ResetFocus()        0x002e3b75
//    CameraController::StopFollowing()     0x002e3b85
//    CameraController::GotoTarget()        0x002e3d3f
//
//  SwMini offsets (arm32) for hero navigation:
//    GameViewController + 0x68  → GameSceneController*
//    GameSceneController + 0xa4 → Hero SceneObject*
//    HeroReference + 0x40       → Position (Vector3)
//    CharController + 0x164     → Jump Height (float)
//    CharController + 0x170     → Walk Speed (float)
//    CharController + 0x178     → Run Speed (float)
//    CharController + 0x158     → Air Jump Used (int)
// ============================================================

// Known addresses in libswordigo.so (Thumb: bit0 set)
namespace CaverSym {
    // CameraController methods
    static constexpr uint32_t CameraControllerCtor         = 0x002e35c5;
    static constexpr uint32_t CameraControllerUpdate       = 0x002e36e5;
    static constexpr uint32_t CameraControllerFocusAtPoint = 0x002e3a25;
    static constexpr uint32_t CameraControllerResetFocus   = 0x002e3b75;
    static constexpr uint32_t CameraControllerStopFollowing= 0x002e3b85;
    static constexpr uint32_t CameraControllerGotoTarget   = 0x002e3d3f;
    static constexpr uint32_t CameraEvaluateViewMatrix     = 0x002e34bd;
    static constexpr uint32_t CameraSetPerspectiveProjection = 0x002e33c9;
}

// Scratch area in guest memory for Vector3 arguments
static constexpr uint32_t CAM_SCRATCH_VEC3 = 0x3FFF0000;

// ---------------------------------------------------------------
// Camera modes
// ---------------------------------------------------------------
enum class CamMode {
    FREE,           // Full free camera — user controls everything
    FOLLOW_OFFSET   // Camera follows hero with user offset applied
};

// ---------------------------------------------------------------
// Camera state (all extern — defined in camera_override.cpp)
// ---------------------------------------------------------------
extern uint32_t  g_cam_ctrl_ptr;    // Guest VA of CameraController (0 = not captured)
extern bool      g_cam_active;      // Camera override enabled
extern CamMode   g_cam_mode;        // Current camera mode
extern float     g_cam_off_x;       // X offset (horizontal pan)
extern float     g_cam_off_y;       // Y offset (vertical height)
extern float     g_cam_off_z;       // Z offset (depth/zoom)
extern bool      g_cam_smooth;      // Smooth interpolation mode
extern float     g_cam_speed_base;  // Base movement speed (units/sec)

// ---------------------------------------------------------------
// Camera preset slot
// ---------------------------------------------------------------
struct CamPreset {
    float x, y, z;
    bool valid;
};
extern CamPreset g_cam_presets[5];  // Ctrl+1..5 to save, 1..5 to load

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

// Called from emulator hook when CameraController ctor runs
void cam_capture_controller(uint32_t this_ptr);

// Enable / disable camera override
void cam_set_active(bool active);
void cam_toggle();

// Movement (dt-scaled, with acceleration)
void cam_move_scaled(float dx, float dy, float dz, float dt);

// Scroll-wheel zoom (delta = ±1.0 per notch)
void cam_scroll_zoom(float delta);

// Toggle smooth mode
void cam_toggle_smooth();

// Reset camera to origin
void cam_reset();

// Save/load presets (slot 0-4)
void cam_save_preset(int slot);
void cam_load_preset(int slot);

// Reset camera acceleration (call when no movement keys are held)
void cam_reset_accel();

// Called once per frame AFTER drawApp
void cam_apply(Emulator* emu, uint8_t* guest_mem);

// Draw camera debug overlay
void cam_debug_string(char* out, int max_len);
