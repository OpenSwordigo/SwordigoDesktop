// input_config.cpp - Data-driven input configuration and virtual button mapping system
// All positions use the 960x544 game coordinate space internally.
// Window-space conversion is done only at render time.

#include "platform/input_config.h"
#include "platform/gl_inc.h"
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <SDL3/SDL.h>
#include <algorithm>

// Game coordinate space constants
static const float GAME_W = 960.0f;
static const float GAME_H = 544.0f;

// Number of segments for circle rendering
static const int CIRCLE_SEGMENTS = 24;

// Default display names for built-in buttons (only these names are hardcoded)
static const std::unordered_map<std::string, std::string> DEFAULT_DISPLAY_NAMES = {
    {"left", "Move Left"},
    {"right", "Move Right"},
    {"jump", "Jump"},
    {"attack", "Sword Attack"},
    {"magic", "Use Magic"},
    {"use_item", "Use Item"},
    {"menu", "Menu"},
    {"pause", "Pause"},
    {"magic_bolt", "Magic Bolt"},
    {"magic_bomb", "Bomb"},
    {"magic_dragon", "Dragon Gasp"},
    {"magic_rift", "Dimensional Rift"},
};

// Get display name — uses custom name if set, otherwise looks up default
static std::string get_display(const TouchButton& btn) {
    if (!btn.display_name.empty()) return btn.display_name;
    auto it = DEFAULT_DISPLAY_NAMES.find(btn.name);
    if (it != DEFAULT_DISPLAY_NAMES.end()) return it->second;
    return btn.name;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

InputConfig::InputConfig() {
    load_defaults();
}

InputConfig::~InputConfig() {
}

// ---------------------------------------------------------------------------
// Default button layout
// ---------------------------------------------------------------------------

void InputConfig::load_defaults() {
    buttons.clear();

    // Helper lambda
    auto add = [this](const char* name, float gx, float gy, float r,
                      int scan, int scan_alt, int tid,
                      int gpad_btn, int gpad_axis = -1, 
                      bool macro = false, int macro_open = -1, int macro_ms = 150,
                      float macro_tx = 0, float macro_ty = 0) {
        TouchButton b;
        b.name            = name;
        b.game_x          = gx;
        b.game_y          = gy;
        b.radius          = r;
        b.sdl_scancode     = scan;
        b.sdl_scancode_alt = scan_alt;
        b.touch_id        = tid;
        b.gamepad_button  = gpad_btn;
        b.gamepad_axis    = gpad_axis;
        b.is_macro        = macro;
        b.macro_open_touch_id = macro_open;
        b.macro_delay_ms  = macro_ms;
        b.macro_target_x  = macro_tx;
        b.macro_target_y  = macro_ty;
        buttons.push_back(b);
    };

    // Movement buttons (left side)
    add("left",  65, 80, 55,
        SDL_SCANCODE_A, SDL_SCANCODE_LEFT, 10,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT);

    add("right", 195, 80, 55,
        SDL_SCANCODE_D, SDL_SCANCODE_RIGHT, 11,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

    // Action buttons (right side)
    add("jump",  913, 132, 45,
        SDL_SCANCODE_SPACE, 0, 12,
        SDL_GAMEPAD_BUTTON_SOUTH);

    add("attack", 794, 110, 40,
        SDL_SCANCODE_K, SDL_SCANCODE_Z, 13,
        SDL_GAMEPAD_BUTTON_WEST);

    add("magic", 916, 210, 35,
        SDL_SCANCODE_J, SDL_SCANCODE_X, 14,
        SDL_GAMEPAD_BUTTON_NORTH);

    // Utility buttons
    add("use_item", 910, 489, 30,
        SDL_SCANCODE_I, 0, 15,
        SDL_GAMEPAD_BUTTON_EAST);

    add("menu", 480, 30, 25,
        SDL_SCANCODE_ESCAPE, 0, 16,
        SDL_GAMEPAD_BUTTON_START);

    add("pause", 55, 485, 25,
        SDL_SCANCODE_P, 0, 17,
        SDL_GAMEPAD_BUTTON_BACK);

    // Magic spell macros — TAP use_item (opens spell menu), wait 250ms, TAP spell slot
    // Drag these buttons onto the spell slot positions in the F2 editor
    add("magic_bolt", 898, 426, 30,
        SDL_SCANCODE_1, 0, 30, -1, -1,
        true, 15, 250, 898, 426);

    add("magic_bomb", 906, 329, 30,
        SDL_SCANCODE_2, 0, 31, -1, -1,
        true, 15, 250, 906, 329);

    add("magic_dragon", 889, 243, 30,
        SDL_SCANCODE_3, 0, 32, -1, -1,
        true, 15, 250, 889, 243);

    add("magic_rift", 891, 105, 30,
        SDL_SCANCODE_4, 0, 33, -1, -1,
        true, 15, 250, 891, 105);

    next_custom_touch_id = 40;
    std::cout << "[InputConfig] Loaded " << buttons.size() << " default buttons\n";
}

// ---------------------------------------------------------------------------
// INI Load (backwards compatible + new fields)
// ---------------------------------------------------------------------------

void InputConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "[InputConfig] Config not found at " << path
                  << ", using defaults\n";
        load_defaults();
        return;
    }

    buttons.clear();

    TouchButton current;
    bool have_section = false;
    int max_touch_id = 40;

    std::string line;
    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            if (have_section) {
                buttons.push_back(current);
            }
            current = TouchButton();
            size_t end = line.find(']');
            if (end != std::string::npos) {
                current.name = line.substr(1, end - 1);
            }
            have_section = true;
            continue;
        }

        if (!have_section) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        size_t vs = val.find_first_not_of(" \t");
        if (vs != std::string::npos) val = val.substr(vs);
        // Trim trailing whitespace from value
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();

        // Parse known keys (original + new fields)
        try {
            if      (key == "game_x")             current.game_x              = std::stof(val);
            else if (key == "game_y")             current.game_y              = std::stof(val);
            else if (key == "radius")             current.radius              = std::stof(val);
            else if (key == "action_type")        current.action_type         = std::stoi(val);
            else if (key == "touch_id")           current.touch_id            = std::stoi(val);
            else if (key == "scancode")           current.sdl_scancode        = std::stoi(val);
            else if (key == "scancode_alt")       current.sdl_scancode_alt    = std::stoi(val);
            else if (key == "gamepad_button")     current.gamepad_button      = std::stoi(val);
            else if (key == "gamepad_axis")       current.gamepad_axis        = std::stoi(val);
            else if (key == "axis_threshold")     current.axis_threshold      = std::stof(val);
            else if (key == "tv_remote_key")      current.tv_remote_key       = std::stoi(val);
            // New fields
            else if (key == "display_name")       current.display_name        = val;
            else if (key == "is_custom")          current.is_custom           = (val == "1" || val == "true");
            else if (key == "is_macro")           current.is_macro            = (val == "1" || val == "true");
            else if (key == "macro_open_touch_id") current.macro_open_touch_id = std::stoi(val);
            else if (key == "macro_delay_ms")     current.macro_delay_ms      = std::stoi(val);
            else if (key == "macro_target_x")     current.macro_target_x      = std::stof(val);
            else if (key == "macro_target_y")     current.macro_target_y      = std::stof(val);
        } catch (const std::exception& e) {
            std::cerr << "[InputConfig] Parse error in key '" << key << "': " << e.what() << "\n";
        }
    }

    if (have_section) {
        buttons.push_back(current);
    }

    // W is reserved for world-depth movement in desktop WASD controls.
    for (auto& btn : buttons) {
        if (btn.name == "jump" && btn.sdl_scancode_alt == SDL_SCANCODE_W) {
            btn.sdl_scancode_alt = 0;
        }
    }

    file.close();

    // Track highest touch_id for custom button creation
    for (const auto& btn : buttons) {
        if (btn.touch_id >= max_touch_id) max_touch_id = btn.touch_id + 1;
    }
    next_custom_touch_id = max_touch_id;

    std::cout << "[InputConfig] Loaded " << buttons.size()
              << " buttons from " << path << "\n";

    if (buttons.empty()) {
        std::cout << "[InputConfig] Config was empty, loading defaults\n";
        load_defaults();
    }
}

// ---------------------------------------------------------------------------
// INI Save (includes all new fields)
// ---------------------------------------------------------------------------

void InputConfig::save(const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "[InputConfig] Failed to save config to " << path << "\n";
        return;
    }

    file << "# Swordigo Desktop - Input Configuration v2\n";
    file << "# Positions are in 960x544 game coordinate space\n";
    file << "# Scancodes use SDL_Scancode values\n";
    file << "# Two keys per action: scancode (primary) + scancode_alt (secondary)\n";
    file << "# Macro buttons: press macro_open_touch_id, wait macro_delay_ms, tap macro_target\n\n";

    for (size_t i = 0; i < buttons.size(); i++) {
        const TouchButton& b = buttons[i];

        file << "[" << b.name << "]\n";
        if (!b.display_name.empty())
            file << "display_name="       << b.display_name       << "\n";
        file << "game_x="                << (int)b.game_x         << "\n";
        file << "game_y="                << (int)b.game_y         << "\n";
        file << "radius="                << (int)b.radius         << "\n";
        file << "action_type="           << b.action_type         << "\n";
        file << "touch_id="              << b.touch_id            << "\n";
        file << "scancode="              << b.sdl_scancode        << "\n";
        file << "scancode_alt="          << b.sdl_scancode_alt    << "\n";
        file << "gamepad_button="        << b.gamepad_button      << "\n";
        file << "gamepad_axis="          << b.gamepad_axis        << "\n";
        file << "axis_threshold="        << b.axis_threshold      << "\n";
        file << "tv_remote_key="         << b.tv_remote_key       << "\n";
        if (b.is_custom)
            file << "is_custom=1\n";
        if (b.is_macro) {
            file << "is_macro=1\n";
            file << "macro_open_touch_id=" << b.macro_open_touch_id << "\n";
            file << "macro_delay_ms="      << b.macro_delay_ms      << "\n";
            file << "macro_target_x="      << (int)b.macro_target_x << "\n";
            file << "macro_target_y="      << (int)b.macro_target_y << "\n";
        }
        file << "\n";
    }

    file.close();
    std::cout << "[InputConfig] Saved " << buttons.size()
              << " buttons to " << path << "\n";
}

// ---------------------------------------------------------------------------
// Input Lookup
// ---------------------------------------------------------------------------

int InputConfig::find_button_at(float gx, float gy) {
    for (int i = 0; i < (int)buttons.size(); i++) {
        float dx = gx - buttons[i].game_x;
        float dy = gy - buttons[i].game_y;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= buttons[i].radius) {
            return i;
        }
    }
    return -1;
}

int InputConfig::find_by_scancode(int scancode) {
    for (int i = 0; i < (int)buttons.size(); i++) {
        if (buttons[i].sdl_scancode == scancode ||
            buttons[i].sdl_scancode_alt == scancode) {
            return i;
        }
    }
    return -1;
}

int InputConfig::find_by_gamepad_button(int button) {
    for (int i = 0; i < (int)buttons.size(); i++) {
        if (buttons[i].gamepad_button == button) {
            return i;
        }
    }
    return -1;
}

int InputConfig::find_by_gamepad_axis(int axis, float value) {
    for (int i = 0; i < (int)buttons.size(); i++) {
        if (buttons[i].gamepad_axis == axis) {
            if (std::fabs(value) >= buttons[i].axis_threshold) {
                return i;
            }
        }
    }
    return -1;
}

TouchButton* InputConfig::get_button(int index) {
    if (index < 0 || index >= (int)buttons.size()) return nullptr;
    return &buttons[index];
}

// ---------------------------------------------------------------------------
// Scancode name helper
// ---------------------------------------------------------------------------

std::string InputConfig::scancode_name(int scancode) {
    if (scancode <= 0) return "-";
    const char* name = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (name && name[0] != '\0') return name;
    return "Key" + std::to_string(scancode);
}

// ---------------------------------------------------------------------------
// Custom button management
// ---------------------------------------------------------------------------

int InputConfig::add_custom_button(const std::string& display_name, float gx, float gy, int scancode) {
    TouchButton b;
    b.name = "custom_" + std::to_string(next_custom_touch_id);
    b.display_name = display_name;
    b.is_custom = true;
    b.game_x = gx;
    b.game_y = gy;
    b.radius = 35;
    b.touch_id = next_custom_touch_id++;
    b.sdl_scancode = scancode;
    b.action_type = 1;
    buttons.push_back(b);
    std::cout << "[InputConfig] Added custom button '" << display_name 
              << "' (id=" << b.touch_id << ", key=" << scancode_name(scancode) << ")\n";
    return (int)buttons.size() - 1;
}

bool InputConfig::remove_button(int index) {
    if (index < 0 || index >= (int)buttons.size()) return false;
    if (!buttons[index].is_custom) {
        std::cerr << "[InputConfig] Cannot remove built-in button '" << buttons[index].name << "'\n";
        return false;
    }
    std::cout << "[InputConfig] Removed custom button '" << buttons[index].display_name << "'\n";
    buttons.erase(buttons.begin() + index);
    return true;
}

// ---------------------------------------------------------------------------
// Key rebinding
// ---------------------------------------------------------------------------

bool InputConfig::editor_handle_key(int scancode) {
    if (editor_mode != EDITOR_REBIND || rebind_target < 0) return false;
    
    // Don't allow system keys (F1-F12, Escape used for exit)
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) return true;
    
    TouchButton& btn = buttons[rebind_target];
    
    // If primary is already this key, set it as alt instead
    if (btn.sdl_scancode == scancode) {
        std::cout << "[InputConfig] Key " << scancode_name(scancode) << " already primary for " << get_display(btn) << "\n";
    } else if (btn.sdl_scancode_alt == 0 || btn.sdl_scancode_alt == scancode) {
        // Set as alt if no alt or same alt
        btn.sdl_scancode_alt = scancode;
        std::cout << "[InputConfig] Rebound " << get_display(btn) << " alt → " << scancode_name(scancode) << "\n";
    } else {
        // Replace primary, shift old primary to alt
        btn.sdl_scancode_alt = btn.sdl_scancode;
        btn.sdl_scancode = scancode;
        std::cout << "[InputConfig] Rebound " << get_display(btn) << " → " << scancode_name(scancode) << "\n";
    }
    
    rebind_target = -1;
    editor_mode = EDITOR_POSITION;
    return true;
}

// ---------------------------------------------------------------------------
// Macro processing
// ---------------------------------------------------------------------------

void InputConfig::process_macros(double current_time, TouchCallback callback) {
    uint64_t now_ms = (uint64_t)(current_time * 1000.0);
    
    for (auto& btn : buttons) {
        if (!btn.is_macro || !btn.macro_pending) continue;
        
        uint64_t elapsed = now_ms - btn.macro_fire_time;
        int macro_tid = 100 + btn.touch_id;
        
        // Stage 1: opener was pressed in main loop. Release it after 50ms to complete the TAP.
        if (btn.macro_stage == 1 && elapsed >= 50) {
            // Release the opener → completes the "i" button tap → menu opens
            for (auto& other : buttons) {
                if (other.touch_id == btn.macro_open_touch_id) {
                    if (callback) {
                        callback(2, macro_tid, current_time,
                                other.game_x, other.game_y,
                                other.game_x, other.game_y, 0);
                        std::cout << "[Macro] Stage 1→2: released opener " << other.name << std::endl;
                    }
                    break;
                }
            }
            btn.macro_stage = 2;  // now waiting for delay before slot tap
        }
        
        // Stage 2: menu is open, wait for delay then tap the spell slot
        if (btn.macro_stage == 2 && elapsed >= (uint64_t)btn.macro_delay_ms) {
            if (callback) {
                std::cout << "[Macro] Stage 2→done: tap " << get_display(btn) 
                          << " at (" << btn.game_x << ", " << btn.game_y << ")" << std::endl;
                // Tap the spell slot (press + release)
                callback(1, btn.touch_id, current_time, 
                        btn.game_x, btn.game_y,
                        btn.game_x, btn.game_y, 1);
            }
            btn.macro_stage = 3;  // will release on next frame
        }
        
        // Stage 3: release the spell slot tap
        if (btn.macro_stage == 3 && elapsed >= (uint64_t)btn.macro_delay_ms + 50) {
            if (callback) {
                callback(2, btn.touch_id, current_time,
                        btn.game_x, btn.game_y,
                        btn.game_x, btn.game_y, 0);
            }
            btn.macro_pending = false;
            btn.macro_stage = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// OpenGL circle drawing helper
// ---------------------------------------------------------------------------

void InputConfig::draw_circle(float cx, float cy, float r,
                              uint8_t red, uint8_t g, uint8_t b, uint8_t a,
                              bool filled) {
    GLenum mode = filled ? GL_TRIANGLE_FAN : GL_LINE_LOOP;

    glColor4ub(red, g, b, a);
    glBegin(mode);

    if (filled) {
        glVertex2f(cx, cy);
    }

    for (int i = 0; i <= CIRCLE_SEGMENTS; i++) {
        float angle = 2.0f * 3.14159265f * (float)i / (float)CIRCLE_SEGMENTS;
        float px = cx + r * std::cos(angle);
        float py = cy + r * std::sin(angle);
        glVertex2f(px, py);
    }

    glEnd();
}

// ---------------------------------------------------------------------------
// Render touch zone overlay (in-game HUD)
// ---------------------------------------------------------------------------

void InputConfig::render_touch_zones(int win_w, int win_h, bool show_labels) {
    float sx = (float)win_w / GAME_W;
    float sy = (float)win_h / GAME_H;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);

    for (int i = 0; i < (int)buttons.size(); i++) {
        const TouchButton& btn = buttons[i];

        float wx = btn.game_x * sx;
        float wy = btn.game_y * sy;
        float wr = btn.radius * sx;

        // Color by category
        uint8_t r, g, b, a;
        if (btn.name == "left" || btn.name == "right") {
            r = 50; g = 120; b = 255; a = btn.is_pressed ? 120 : 60;
        } else if (btn.name == "menu" || btn.name == "pause") {
            r = 255; g = 160; b = 40; a = btn.is_pressed ? 120 : 50;
        } else if (btn.is_macro) {
            r = 180; g = 80; b = 220; a = btn.is_pressed ? 120 : 50; // Purple for macros
        } else if (btn.is_custom) {
            r = 220; g = 220; b = 60; a = btn.is_pressed ? 120 : 50; // Yellow for custom
        } else {
            r = 60; g = 220; b = 80; a = btn.is_pressed ? 120 : 50;
        }

        draw_circle(wx, wy, wr, r, g, b, a, true);
        draw_circle(wx, wy, wr, r, g, b, (uint8_t)(a + 60), false);
    }

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
}

// ---------------------------------------------------------------------------
// Render editor overlay (enhanced with labels, key bindings, mode display)
// ---------------------------------------------------------------------------

void InputConfig::render_editor(int win_w, int win_h,
                                int mouse_x, int mouse_y,
                                bool mouse_pressed) {
    float sx = (float)win_w / GAME_W;
    float sy = (float)win_h / GAME_H;

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, win_w, 0, win_h, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    // Semi-transparent dark overlay
    glColor4ub(0, 0, 0, 120);
    glBegin(GL_QUADS);
    glVertex2f(0, 0);
    glVertex2f((float)win_w, 0);
    glVertex2f((float)win_w, (float)win_h);
    glVertex2f(0, (float)win_h);
    glEnd();

    float mouse_gx = (float)mouse_x / sx;
    float mouse_gy = (float)mouse_y / sy;
    int hovered = find_button_at(mouse_gx, mouse_gy);

    for (int i = 0; i < (int)buttons.size(); i++) {
        const TouchButton& btn = buttons[i];

        float wx = btn.game_x * sx;
        float wy = btn.game_y * sy;
        float wr = btn.radius * std::min(sx, sy) * 0.7f;

        // Color by category
        uint8_t r, g, b;
        if (btn.name == "left" || btn.name == "right") {
            r = 60; g = 130; b = 255;
        } else if (btn.name == "menu" || btn.name == "pause") {
            r = 255; g = 160; b = 40;
        } else if (btn.is_macro) {
            r = 180; g = 80; b = 220;
        } else if (btn.is_custom) {
            r = 220; g = 220; b = 60;
        } else {
            r = 60; g = 200; b = 80;
        }

        // Highlight
        uint8_t fill_a = 50;
        uint8_t line_a = 150;
        if (i == hovered || i == dragging_button) {
            fill_a = 100;
            line_a = 255;
        }
        if (editor_mode == EDITOR_REBIND && i == rebind_target) {
            // Blinking highlight for rebind target
            fill_a = 150;
            r = 255; g = 255; b = 100;
        }

        draw_circle(wx, wy, wr, r, g, b, fill_a, true);
        draw_circle(wx, wy, wr, r, g, b, line_a, false);

        // Crosshair
        float ch = 4.0f;
        glColor4ub(255, 255, 255, 180);
        glBegin(GL_LINES);
        glVertex2f(wx - ch, wy); glVertex2f(wx + ch, wy);
        glVertex2f(wx, wy - ch); glVertex2f(wx, wy + ch);
        glEnd();

        // Display name + key binding label
        std::string display = get_display(btn);
        std::string key_str = "[" + scancode_name(btn.sdl_scancode);
        if (btn.sdl_scancode_alt > 0) {
            key_str += "/" + scancode_name(btn.sdl_scancode_alt);
        }
        key_str += "]";
        std::string label = display + " " + key_str;
        if (btn.is_macro) label = "⚡" + label;

        float label_x = wx - (label.length() * 3.5f);
        float label_y = wy - wr - 16.0f;
        float lw = label.length() * 7.0f + 6.0f;
        
        // Label background
        glColor4ub(0, 0, 0, 200);
        glBegin(GL_QUADS);
        glVertex2f(label_x - 3, label_y - 2);
        glVertex2f(label_x + lw, label_y - 2);
        glVertex2f(label_x + lw, label_y + 12);
        glVertex2f(label_x - 3, label_y + 12);
        glEnd();
        
        // Label text (crude pixel blocks — real text via launcher font)
        glColor4ub(r, g, b, 255);
        for (size_t c = 0; c < label.size(); c++) {
            float cx = label_x + c * 7.0f;
            glBegin(GL_QUADS);
            glVertex2f(cx, label_y);
            glVertex2f(cx + 5, label_y);
            glVertex2f(cx + 5, label_y + 10);
            glVertex2f(cx, label_y + 10);
            glEnd();
        }
    }

    // Instruction bar at top
    float bar_y = (float)win_h - 30.0f;
    glColor4ub(0, 0, 0, 200);
    glBegin(GL_QUADS);
    glVertex2f(0, bar_y);
    glVertex2f((float)win_w, bar_y);
    glVertex2f((float)win_w, (float)win_h);
    glVertex2f(0, (float)win_h);
    glEnd();

    // Restore GL state
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
}

// ---------------------------------------------------------------------------
// Editor mouse handlers (all coordinates in game space)
// ---------------------------------------------------------------------------

void InputConfig::editor_mouse_down(float game_x, float game_y) {
    int clicked = find_button_at(game_x, game_y);
    
    if (editor_mode == EDITOR_REBIND) {
        // In rebind mode, clicking a button selects it for rebinding
        if (clicked >= 0) {
            rebind_target = clicked;
            std::cout << "[InputConfig] Rebind mode: press a key for '" 
                      << get_display(buttons[clicked]) << "'\n";
        }
        return;
    }
    
    // Position mode — drag as before
    dragging_button = clicked;
    if (dragging_button >= 0) {
        drag_offset_x = buttons[dragging_button].game_x - game_x;
        drag_offset_y = buttons[dragging_button].game_y - game_y;
        std::cout << "[InputConfig] Editor: grabbed '"
                  << get_display(buttons[dragging_button]) << "'\n";
    }
}

void InputConfig::editor_mouse_move(float game_x, float game_y) {
    if (dragging_button < 0) return;

    float new_x = game_x + drag_offset_x;
    float new_y = game_y + drag_offset_y;

    float r = buttons[dragging_button].radius;
    if (new_x - r < 0)      new_x = r;
    if (new_x + r > GAME_W) new_x = GAME_W - r;
    if (new_y - r < 0)      new_y = r;
    if (new_y + r > GAME_H) new_y = GAME_H - r;

    buttons[dragging_button].game_x = new_x;
    buttons[dragging_button].game_y = new_y;
}

void InputConfig::editor_mouse_up() {
    if (dragging_button >= 0) {
        std::cout << "[InputConfig] Editor: released '"
                  << get_display(buttons[dragging_button])
                  << "' at (" << buttons[dragging_button].game_x
                  << ", " << buttons[dragging_button].game_y << ")\n";
        dragging_button = -1;
    }
}
