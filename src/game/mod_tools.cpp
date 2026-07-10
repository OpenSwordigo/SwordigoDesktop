#include "mod_tools.h"
#include "../platform/gui.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <GL/gl.h>

// ============================================================
//  SwordigoDesktop — Mod Tools Implementation
//  Game speed, pause/step, toast notifications.
// ============================================================

// ---------------------------------------------------------------
// Game Speed
// ---------------------------------------------------------------
float g_game_speed    = 1.0f;
bool  g_game_paused   = false;
bool  g_step_one_frame = false;

static const float SPEED_MIN  = 0.25f;
static const float SPEED_MAX  = 4.0f;
static const float SPEED_STEP = 0.25f;

static char s_speed_label[16] = "1.0x";

static void refresh_speed_label() {
    snprintf(s_speed_label, sizeof(s_speed_label), "%.2gx", g_game_speed);
}

void mod_speed_up() {
    g_game_speed = std::min(g_game_speed + SPEED_STEP, SPEED_MAX);
    refresh_speed_label();
    char msg[64];
    snprintf(msg, sizeof(msg), "Speed: %s", s_speed_label);
    mod_toast(msg, 1.5f);
    std::cout << "[ModTools] " << msg << std::endl;
}

void mod_speed_down() {
    g_game_speed = std::max(g_game_speed - SPEED_STEP, SPEED_MIN);
    refresh_speed_label();
    char msg[64];
    snprintf(msg, sizeof(msg), "Speed: %s", s_speed_label);
    mod_toast(msg, 1.5f);
    std::cout << "[ModTools] " << msg << std::endl;
}

void mod_speed_reset() {
    g_game_speed = 1.0f;
    refresh_speed_label();
    mod_toast("Speed: 1x (Normal)", 1.5f);
    std::cout << "[ModTools] Speed reset to 1.0x" << std::endl;
}

const char* mod_speed_label() {
    return s_speed_label;
}

// ---------------------------------------------------------------
// Pause / Frame Advance
// ---------------------------------------------------------------
void mod_toggle_pause() {
    g_game_paused = !g_game_paused;
    g_step_one_frame = false;
    if (g_game_paused) {
        mod_toast("PAUSED  (F9 = step)", 0.0f); // duration 0 = stays until unpaused
    } else {
        mod_toast("RESUMED", 1.0f);
    }
    std::cout << "[ModTools] " << (g_game_paused ? "PAUSED" : "RESUMED") << std::endl;
}

void mod_step_frame() {
    if (!g_game_paused) return;
    g_step_one_frame = true;
}

// ---------------------------------------------------------------
// Toast Notifications
// ---------------------------------------------------------------
struct Toast {
    std::string message;
    float elapsed;
    float duration;   // 0 = persistent (removed manually)
};

static std::vector<Toast> s_toasts;

void mod_toast(const std::string& message, float duration_sec) {
    // Remove any persistent toast (duration 0) when adding a new one
    s_toasts.erase(
        std::remove_if(s_toasts.begin(), s_toasts.end(),
            [](const Toast& t) { return t.duration <= 0.0f; }),
        s_toasts.end()
    );
    s_toasts.push_back({message, 0.0f, duration_sec});
    // Keep max 5 toasts
    while (s_toasts.size() > 5) {
        s_toasts.erase(s_toasts.begin());
    }
}

// ---------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------
void mod_update(float /*dt*/) {
    // Nothing needed yet — speed/pause are applied in main.cpp
}

// ---------------------------------------------------------------
// Render overlay
// ---------------------------------------------------------------
void mod_render_overlay(GuiRenderer& gui, int win_w, int win_h, float dt) {
    // --- Speed indicator (always visible when not 1.0x) ---
    if (std::fabs(g_game_speed - 1.0f) > 0.01f) {
        char buf[32];
        snprintf(buf, sizeof(buf), "[ %s ]", s_speed_label);
        // Top-right corner
        int x = win_w - 120;
        int y = win_h - 30;
        // Color: green if fast, orange if slow
        int r = g_game_speed < 1.0f ? 255 : 100;
        int g = g_game_speed > 1.0f ? 255 : 180;
        gui.draw_string(buf, x, y, 1.4f, r, g, 80, 255);
    }

    // --- Pause indicator (always visible when paused) ---
    if (g_game_paused) {
        gui.draw_string("|| PAUSED", win_w / 2 - 50, win_h / 2 + 20, 2.0f, 255, 80, 80, 255);
        gui.draw_string("F9 = Step Frame    F8 = Resume", win_w / 2 - 130, win_h / 2 - 10, 1.0f, 200, 200, 200, 255);
    }

    // --- Toast messages (center of screen, stack upward) ---
    int toast_y = win_h / 2 - 40;
    for (auto it = s_toasts.begin(); it != s_toasts.end(); ) {
        if (it->duration > 0.0f) {
            it->elapsed += dt;
            if (it->elapsed >= it->duration) {
                it = s_toasts.erase(it);
                continue;
            }
        }
        // Fade out in the last 0.4 seconds
        int alpha = 255;
        if (it->duration > 0.0f && it->elapsed > it->duration - 0.4f) {
            alpha = (int)(255.0f * (it->duration - it->elapsed) / 0.4f);
            if (alpha < 0) alpha = 0;
        }
        gui.draw_string(it->message.c_str(), win_w / 2 - 80, toast_y, 1.3f, 255, 255, 100, alpha);
        toast_y -= 22;
        ++it;
    }
}

// ---------------------------------------------------------------
// Achievement Popup System
// ---------------------------------------------------------------

struct AchievementPopup {
    std::string title;
    std::string desc;
    float elapsed;
    float duration;
};

static std::vector<AchievementPopup> s_achievement_queue;
static uint64_t s_ach_pending_offset = 0;
static uint64_t s_ach_title_offset = 0;
static uint64_t s_ach_desc_offset = 0;

void mod_achievement_poll(uint8_t* sre_base, uint64_t sre_load_addr) {
    if (!sre_base || !sre_load_addr) return;

    // Resolve symbol offsets lazily (set by main.cpp after SRE load)
    if (s_ach_pending_offset == 0) return;

    int* pending = (int*)(sre_base + s_ach_pending_offset);
    if (*pending) {
        char* title = (char*)(sre_base + s_ach_title_offset);
        char* desc = (char*)(sre_base + s_ach_desc_offset);

        AchievementPopup popup;
        popup.title = title;
        popup.desc = desc;
        popup.elapsed = 0.0f;
        popup.duration = 4.0f;  // 4 second display

        s_achievement_queue.push_back(popup);

        // Clear pending flag
        *pending = 0;
        title[0] = '\0';
        desc[0] = '\0';

        std::cout << "[Achievement] \xF0\x9F\x8F\x86 Unlocked: " << popup.title << std::endl;
    }
}

void mod_achievement_set_offsets(uint64_t pending, uint64_t title, uint64_t desc) {
    s_ach_pending_offset = pending;
    s_ach_title_offset = title;
    s_ach_desc_offset = desc;
}

void mod_achievement_render(GuiRenderer& gui, int win_w, int win_h, float dt) {
    if (s_achievement_queue.empty()) return;

    auto& popup = s_achievement_queue.front();
    popup.elapsed += dt;

    if (popup.elapsed >= popup.duration) {
        s_achievement_queue.erase(s_achievement_queue.begin());
        return;
    }

    // Animation: slide in from top (0-0.5s), hold (0.5-3.5s), fade out (3.5-4.0s)
    float slide_t = popup.elapsed < 0.5f ? popup.elapsed / 0.5f : 1.0f;
    int alpha = 255;
    if (popup.elapsed > popup.duration - 0.5f) {
        alpha = (int)(255.0f * (popup.duration - popup.elapsed) / 0.5f);
        if (alpha < 0) alpha = 0;
    }

    // Banner dimensions
    float banner_w = 380.0f;
    float banner_h = 60.0f;
    float banner_x = (win_w - banner_w) / 2.0f;
    float target_y = win_h - 75.0f;
    float banner_y = target_y + (1.0f - slide_t) * 80.0f;  // slide down from above

    // Dark background (raw OpenGL since draw_rect is private)
    int bg_alpha = (int)(alpha * 0.85f);
    glColor4ub(20, 20, 30, bg_alpha);
    glBegin(GL_QUADS);
    glVertex2f(banner_x, banner_y);
    glVertex2f(banner_x + banner_w, banner_y);
    glVertex2f(banner_x + banner_w, banner_y + banner_h);
    glVertex2f(banner_x, banner_y + banner_h);
    glEnd();

    // Gold border (2px outline)
    glColor4ub(255, 200, 50, alpha);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(banner_x, banner_y);
    glVertex2f(banner_x + banner_w, banner_y);
    glVertex2f(banner_x + banner_w, banner_y + banner_h);
    glVertex2f(banner_x, banner_y + banner_h);
    glEnd();

    // Title (gold text)
    std::string title_str = "Achievement Unlocked: " + popup.title;
    gui.draw_string(title_str, banner_x + 12, banner_y + banner_h - 18, 1.3f, 255, 215, 0, alpha);

    // Description (grey text)
    gui.draw_string(popup.desc, banner_x + 12, banner_y + banner_h - 40, 1.0f, 200, 200, 200, alpha);
}

extern GuiRenderer g_gui;

extern uint32_t g_hero_obj;
extern uint32_t g_hero_char_ctrl_comp;
extern uint32_t g_hero_health_comp;
extern uint32_t g_hero_mana_comp;

extern float s_vanilla_walk_speed;
extern float s_vanilla_run_speed;
extern float s_vanilla_jump_height;

void mod_apply_frame(uint8_t* guest_mem) {
    // 1. God Mode
    if (g_gui.mod_god_mode && g_hero_health_comp != 0) {
        int max_hp = *(int*)(guest_mem + g_hero_health_comp + 0x88);
        if (max_hp > 0) {
            *(int*)(guest_mem + g_hero_health_comp + 0x7c) = max_hp;
            *(int*)(guest_mem + g_hero_health_comp + 0x80) = max_hp;
        }
    }

    // 2. Infinite Mana
    if (g_gui.mod_infinite_mana && g_hero_mana_comp != 0) {
        int max_mana = *(int*)(guest_mem + g_hero_mana_comp + 0x3c);
        if (max_mana > 0) {
            *(int*)(guest_mem + g_hero_mana_comp + 0x40) = max_mana;
        }
    }

    // 3. Infinite Jump
    if (g_gui.mod_infinite_jump && g_hero_char_ctrl_comp != 0) {
        *(int*)(guest_mem + g_hero_char_ctrl_comp + 0x158) = 0; // Reset air jump count
    }

    // 4. Walk, Run, Jump speed multipliers
    if (g_hero_char_ctrl_comp != 0 && s_vanilla_walk_speed > 0.0f) {
        *(float*)(guest_mem + g_hero_char_ctrl_comp + 0x170) = s_vanilla_walk_speed * g_gui.mod_walk_speed;
        *(float*)(guest_mem + g_hero_char_ctrl_comp + 0x178) = s_vanilla_run_speed * g_gui.mod_run_speed;
        *(float*)(guest_mem + g_hero_char_ctrl_comp + 0x164) = s_vanilla_jump_height * g_gui.mod_jump_height;
    }
}


