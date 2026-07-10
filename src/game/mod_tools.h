#pragma once
#include <string>
#include <cstdint>

// ============================================================
//  SwordigoDesktop — Mod Tools System
//  Ports key SwMini modloader features to the desktop port.
//
//  Features:
//    1. Game Speed Control  (+/- keys, 0.25x – 4.0x)
//    2. Pause / Frame Advance (F8 pause, F9 step)
//    3. Toast Notifications (on-screen status messages)
// ============================================================

// Forward declarations
class GuiRenderer;

// ---------------------------------------------------------------
// Game Speed
// ---------------------------------------------------------------
extern float g_game_speed;       // 1.0 = normal, 0.5 = half, 2.0 = double

void mod_speed_up();             // +0.25x (max 4.0x)
void mod_speed_down();           // -0.25x (min 0.25x)
void mod_speed_reset();          // Reset to 1.0x
const char* mod_speed_label();   // "1.0x", "0.5x", etc.

// ---------------------------------------------------------------
// Pause / Frame Advance
// ---------------------------------------------------------------
extern bool g_game_paused;       // true = game logic frozen
extern bool g_step_one_frame;    // true = advance exactly 1 frame then re-pause

void mod_toggle_pause();
void mod_step_frame();           // Only works while paused

// ---------------------------------------------------------------
// Toast Notifications
//   Short on-screen messages that fade out after a duration.
//   Used to show speed changes, camera mode, etc.
// ---------------------------------------------------------------
void mod_toast(const std::string& message, float duration_sec = 2.0f);

// ---------------------------------------------------------------
// Per-frame update & render
//   Call mod_update() at the TOP of the game loop.
//   Call mod_render_overlay() AFTER all game rendering.
// ---------------------------------------------------------------
void mod_update(float dt);
void mod_render_overlay(GuiRenderer& gui, int win_w, int win_h, float dt);
void mod_apply_frame(uint8_t* guest_mem);

// ---------------------------------------------------------------
// Achievement Popup
//   Polls SRE g_sre_achievement_pending and renders a banner.
// ---------------------------------------------------------------
void mod_achievement_poll(uint8_t* sre_base, uint64_t sre_load_addr);
void mod_achievement_render(GuiRenderer& gui, int win_w, int win_h, float dt);
void mod_achievement_set_offsets(uint64_t pending, uint64_t title, uint64_t desc);
