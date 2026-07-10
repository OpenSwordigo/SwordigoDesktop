#include "camera_override.h"
#include "mod_tools.h"
#include "../platform/emulator.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <cstdio>
#include <algorithm>

// ============================================================
//  SwordigoDesktop — Modernized Camera Override
//  Dt-based speed, acceleration, smooth mode, scroll zoom,
//  position presets, and better limits.
// ============================================================

// ----- Global state ------------------------------------------
uint32_t g_cam_ctrl_ptr  = 0;
bool     g_cam_active    = false;
CamMode  g_cam_mode      = CamMode::FREE;
float    g_cam_off_x     = 0.0f;
float    g_cam_off_y     = 0.0f;
float    g_cam_off_z     = 0.0f;
bool     g_cam_smooth    = false;   // false = instant, true = smooth interp
float    g_cam_speed_base= 120.0f;  // Units per second at 1x speed

CamPreset g_cam_presets[5] = {};    // All initialized to {0,0,0,false}

// Internal: acceleration tracking
static float s_accel_timer = 0.0f;  // How long movement keys have been held
static const float ACCEL_RAMP = 3.0f;  // Seconds to reach max speed
static const float ACCEL_MAX  = 4.0f;  // Max speed multiplier from acceleration

// Limits
static const float CAM_LIMIT_XZ = 2000.0f;
static const float CAM_LIMIT_Y  = 1000.0f;

// Scroll zoom speed
static const float SCROLL_ZOOM_SPEED = 40.0f;

// ----- Helpers -----------------------------------------------

static inline uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Write a Vector3 into guest scratch memory
static uint32_t write_vec3(uint8_t* guest_mem, float x, float y, float z) {
    memcpy(guest_mem + CAM_SCRATCH_VEC3 + 0, &x, 4);
    memcpy(guest_mem + CAM_SCRATCH_VEC3 + 4, &y, 4);
    memcpy(guest_mem + CAM_SCRATCH_VEC3 + 8, &z, 4);
    return CAM_SCRATCH_VEC3;
}

// ----- Public API --------------------------------------------

void cam_capture_controller(uint32_t this_ptr) {
    if (g_cam_ctrl_ptr == 0 && this_ptr != 0) {
        g_cam_ctrl_ptr = this_ptr;
        std::cout << "[Camera] CameraController captured at 0x"
                  << std::hex << this_ptr << std::dec << std::endl;
    }
}

void cam_set_active(bool active) {
    g_cam_active = active;
    s_accel_timer = 0.0f;
    if (!active) {
        g_cam_off_x = g_cam_off_y = g_cam_off_z = 0.0f;
        mod_toast("Camera: Game Control", 1.5f);
        std::cout << "[Camera] Override DISABLED" << std::endl;
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Camera: FREE  [%s]",
                 g_cam_smooth ? "Smooth" : "Instant");
        mod_toast(msg, 2.0f);
        std::cout << "[Camera] Override ENABLED (Free, "
                  << (g_cam_smooth ? "Smooth" : "Instant") << ")" << std::endl;
    }
}

void cam_toggle() {
    cam_set_active(!g_cam_active);
}

void cam_move_scaled(float dx, float dy, float dz, float dt) {
    if (!g_cam_active) return;

    // Acceleration: speed ramps up the longer keys are held
    s_accel_timer += dt;
    float accel = 1.0f + (ACCEL_MAX - 1.0f) * std::min(s_accel_timer / ACCEL_RAMP, 1.0f);

    // Final speed = base * dt * acceleration
    float speed = g_cam_speed_base * dt * accel;

    g_cam_off_x = clampf(g_cam_off_x + dx * speed, -CAM_LIMIT_XZ, CAM_LIMIT_XZ);
    g_cam_off_y = clampf(g_cam_off_y + dy * speed, -CAM_LIMIT_Y,  CAM_LIMIT_Y);
    g_cam_off_z = clampf(g_cam_off_z + dz * speed, -CAM_LIMIT_XZ, CAM_LIMIT_XZ);
}

void cam_scroll_zoom(float delta) {
    if (!g_cam_active) return;
    g_cam_off_z = clampf(g_cam_off_z - delta * SCROLL_ZOOM_SPEED,
                         -CAM_LIMIT_XZ, CAM_LIMIT_XZ);
}

void cam_toggle_smooth() {
    g_cam_smooth = !g_cam_smooth;
    char msg[64];
    snprintf(msg, sizeof(msg), "Camera: %s", g_cam_smooth ? "Smooth" : "Instant");
    mod_toast(msg, 1.5f);
    std::cout << "[Camera] Smooth mode: " << (g_cam_smooth ? "ON" : "OFF") << std::endl;
}

void cam_reset() {
    g_cam_off_x = g_cam_off_y = g_cam_off_z = 0.0f;
    s_accel_timer = 0.0f;
    mod_toast("Camera: Reset", 1.0f);
    std::cout << "[Camera] Position reset" << std::endl;
}

void cam_save_preset(int slot) {
    if (slot < 0 || slot >= 5) return;
    g_cam_presets[slot] = { g_cam_off_x, g_cam_off_y, g_cam_off_z, true };
    char msg[64];
    snprintf(msg, sizeof(msg), "Camera: Saved slot %d", slot + 1);
    mod_toast(msg, 1.0f);
    std::cout << "[Camera] Preset " << slot + 1 << " saved: ("
              << g_cam_off_x << ", " << g_cam_off_y << ", " << g_cam_off_z << ")" << std::endl;
}

void cam_load_preset(int slot) {
    if (slot < 0 || slot >= 5) return;
    if (!g_cam_presets[slot].valid) {
        mod_toast("Camera: Slot empty", 1.0f);
        return;
    }
    g_cam_off_x = g_cam_presets[slot].x;
    g_cam_off_y = g_cam_presets[slot].y;
    g_cam_off_z = g_cam_presets[slot].z;
    char msg[64];
    snprintf(msg, sizeof(msg), "Camera: Loaded slot %d", slot + 1);
    mod_toast(msg, 1.0f);
}

// Reset acceleration when no movement keys are held
// (Called from main.cpp when no cam keys are active)
void cam_reset_accel() {
    s_accel_timer = 0.0f;
}

static bool s_was_active = false;

void cam_apply(Emulator* emu, uint8_t* guest_mem) {
    if (g_cam_ctrl_ptr == 0) return;

    if (g_cam_active) {
        s_was_active = true;

        // Custom offsets: base offset + user control offsets
        // Vanilla: 0, 0, -1000. 3D default: 0, 300, -600.
        float offset_x = g_cam_off_x;
        float offset_y = 300.0f + g_cam_off_y;
        float offset_z = -600.0f + g_cam_off_z;

        // 1. Write the custom position offset to CameraController + 0x04
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x04, &offset_x, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x08, &offset_y, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x0c, &offset_z, 4);

        // 2. Write the Up Vector (0, 1, 0) to CameraController + 0x48
        float up_x = 0.0f;
        float up_y = 1.0f;
        float up_z = 0.0f;
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x48, &up_x, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x4c, &up_y, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x50, &up_z, 4);

        // 3. Set the 3D perspective projection on the Camera object
        uint32_t camera_ptr = *(uint32_t*)(guest_mem + g_cam_ctrl_ptr + 0x54);
        if (camera_ptr != 0) {
            float fov_rad = 45.0f * 3.14159265f / 180.0f;
            extern int g_win_w;
            extern int g_win_h;
            float aspect = (float)g_win_w / (float)g_win_h;
            float near_plane = 50.0f;
            float far_plane = 20000.0f;

            bool prev_quiet = emu->quiet_mode;
            emu->quiet_mode = true;
            
            // Allocate 4 bytes on guest stack for stack arguments (far)
            uint32_t sp = emu->get_reg(13);
            uint32_t new_sp = sp - 4;
            *(uint32_t*)(guest_mem + new_sp) = f2u(far_plane);
            emu->set_reg(13, new_sp);

            // Call SetPerspectiveProjection(Camera* this, float fov, float aspect, float near)
            emu->call(CaverSym::CameraSetPerspectiveProjection,
                      {camera_ptr, f2u(fov_rad), f2u(aspect), f2u(near_plane)});

            // Restore guest stack pointer
            emu->set_reg(13, sp);

            emu->quiet_mode = prev_quiet;
        }
    } else if (s_was_active) {
        // Restore vanilla camera offsets and perspective projection once
        s_was_active = false;

        float offset_x = 0.0f;
        float offset_y = 0.0f;
        float offset_z = -1000.0f;
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x04, &offset_x, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x08, &offset_y, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x0c, &offset_z, 4);

        float up_x = 0.0f;
        float up_y = 1.0f;
        float up_z = 0.0f;
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x48, &up_x, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x4c, &up_y, 4);
        memcpy(guest_mem + g_cam_ctrl_ptr + 0x50, &up_z, 4);

        uint32_t camera_ptr = *(uint32_t*)(guest_mem + g_cam_ctrl_ptr + 0x54);
        if (camera_ptr != 0) {
            bool prev_quiet = emu->quiet_mode;
            emu->quiet_mode = true;

            uint32_t sp = emu->get_reg(13);
            uint32_t new_sp = sp - 4;
            *(uint32_t*)(guest_mem + new_sp) = f2u(20000.0f);
            emu->set_reg(13, new_sp);

            // Vanilla: 20 degrees FOV (0.34906584 rad), aspect = 1.0, near = 50.0, far = 20000.0
            emu->call(CaverSym::CameraSetPerspectiveProjection,
                      {camera_ptr, f2u(0.34906584f), f2u(1.0f), f2u(50.0f)});

            emu->set_reg(13, sp);
            emu->quiet_mode = prev_quiet;
        }
    }
}

void cam_debug_string(char* out, int max_len) {
    snprintf(out, max_len,
             "CAM [%s] %s  X:%.1f  Y:%.1f  Z:%.1f  Speed:%.0f  Ctrl:0x%08x",
             g_cam_active ? "ON " : "OFF",
             g_cam_smooth ? "~Smooth" : "Instant",
             g_cam_off_x, g_cam_off_y, g_cam_off_z,
             g_cam_speed_base,
             g_cam_ctrl_ptr);
}
