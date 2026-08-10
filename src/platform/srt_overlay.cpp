/*
 * srt_overlay.cpp — SRT In-Game Overlay & Inventory Editor
 *
 * Renders a premium overlay on top of the game for editing
 * inventory, stats, equipment, and spells.
 *
 * Reads/writes .gplayer save files (Google Protocol Buffers).
 * Uses a minimal hand-rolled protobuf parser — no external deps.
 */

#include "srt_overlay.h"
#include "gui.h"

#include "platform/gl_inc.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <vector>

// ============================================================================
// Minimal Protobuf Wire Format Parser/Writer
// ============================================================================
// .gplayer files use protobuf wire format. We parse just enough to extract
// and modify CharacterState fields without losing unknown fields.

namespace proto {

// Wire types
enum WireType { VARINT = 0, FIXED64 = 1, LENGTH_DELIM = 2, FIXED32 = 5 };

struct Field {
    int number;
    int wire_type;
    // Varint value (for VARINT, FIXED32, FIXED64)
    uint64_t varint;
    // Bytes value (for LENGTH_DELIM)
    std::vector<uint8_t> bytes;
    // Sub-fields (for nested messages)
    std::vector<Field> children;
    bool parsed_children = false;
};

// Read varint from buffer
static uint64_t read_varint(const uint8_t*& p, const uint8_t* end) {
    uint64_t val = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return val;
}

// Write varint to buffer
static void write_varint(std::vector<uint8_t>& out, uint64_t val) {
    do {
        uint8_t b = val & 0x7F;
        val >>= 7;
        if (val) b |= 0x80;
        out.push_back(b);
    } while (val);
}

// Write field tag
static void write_tag(std::vector<uint8_t>& out, int field_num, int wire_type) {
    write_varint(out, (uint64_t)(field_num << 3 | wire_type));
}

// Parse raw protobuf bytes into field list
static std::vector<Field> parse(const uint8_t* data, size_t len) {
    std::vector<Field> fields;
    const uint8_t* p = data;
    const uint8_t* end = data + len;

    while (p < end) {
        uint64_t tag = read_varint(p, end);
        Field f;
        f.number = (int)(tag >> 3);
        f.wire_type = (int)(tag & 7);

        switch (f.wire_type) {
            case VARINT:
                f.varint = read_varint(p, end);
                break;
            case FIXED64:
                if (p + 8 <= end) {
                    memcpy(&f.varint, p, 8);
                    p += 8;
                }
                break;
            case LENGTH_DELIM: {
                uint64_t len2 = read_varint(p, end);
                if (p + len2 <= end) {
                    f.bytes.assign(p, p + len2);
                    p += len2;
                }
                break;
            }
            case FIXED32:
                if (p + 4 <= end) {
                    uint32_t v32;
                    memcpy(&v32, p, 4);
                    f.varint = v32;
                    p += 4;
                }
                break;
            default:
                // Unknown wire type, bail
                p = end;
                continue;
        }
        fields.push_back(f);
    }
    return fields;
}

// Serialize fields back to bytes
static std::vector<uint8_t> serialize(const std::vector<Field>& fields) {
    std::vector<uint8_t> out;
    for (const auto& f : fields) {
        write_tag(out, f.number, f.wire_type);
        switch (f.wire_type) {
            case VARINT:
                write_varint(out, f.varint);
                break;
            case FIXED64:
                for (int i = 0; i < 8; i++)
                    out.push_back((f.varint >> (i * 8)) & 0xFF);
                break;
            case LENGTH_DELIM: {
                // If children were parsed, re-serialize them
                if (f.parsed_children) {
                    auto child_bytes = serialize(f.children);
                    write_varint(out, child_bytes.size());
                    out.insert(out.end(), child_bytes.begin(), child_bytes.end());
                } else {
                    write_varint(out, f.bytes.size());
                    out.insert(out.end(), f.bytes.begin(), f.bytes.end());
                }
                break;
            }
            case FIXED32:
                for (int i = 0; i < 4; i++)
                    out.push_back((f.varint >> (i * 8)) & 0xFF);
                break;
        }
    }
    return out;
}

// Find field by number
static Field* find(std::vector<Field>& fields, int num) {
    for (auto& f : fields) if (f.number == num) return &f;
    return nullptr;
}

// Get string from length-delimited field
static std::string get_string(const Field& f) {
    return std::string(f.bytes.begin(), f.bytes.end());
}

// Set string on a length-delimited field
static void set_string(Field& f, const std::string& s) {
    f.bytes.assign(s.begin(), s.end());
}

// Parse children of a length-delimited field as sub-message
static void parse_children(Field& f) {
    if (f.wire_type == LENGTH_DELIM && !f.parsed_children) {
        f.children = parse(f.bytes.data(), f.bytes.size());
        f.parsed_children = true;
    }
}

// Create a new varint field
static Field make_varint(int num, uint64_t val) {
    Field f;
    f.number = num;
    f.wire_type = VARINT;
    f.varint = val;
    return f;
}

// Create a new string field
static Field make_string(int num, const std::string& s) {
    Field f;
    f.number = num;
    f.wire_type = LENGTH_DELIM;
    f.bytes.assign(s.begin(), s.end());
    return f;
}

// Remove all fields with given number
static void remove_all(std::vector<Field>& fields, int num) {
    fields.erase(
        std::remove_if(fields.begin(), fields.end(),
                        [num](const Field& f) { return f.number == num; }),
        fields.end());
}

} // namespace proto


// ============================================================================
// .gplayer Protobuf Read/Write
// ============================================================================
// PlayerProfile schema:
//   4: GameState (sub-message)
//     1: CharacterState (sub-message)
//       2: CurrentHealth (varint)
//       4: CurrentMana (varint)  
//       5: CurrentCoins (varint)
//       6: ExperiencePoints (varint)
//       7: ExperienceLevel (varint)
//       11: Item (sub-message, repeated) { 1: Name, 2: Count }
//       12: EquippedWeapon (string)
//       13: EquippedArmor (string)
//       15: Skill (string, repeated)
//       16: CurrentSkill (string)
//       17: WeaponTrinket (string)
//       18: ArmorTrinket (string)
//       19: SkillTrinket (string)
//       20: HealthAttribute (varint)
//       21: AttackAttribute (varint)
//       22: MagicAttribute (varint)
//     3: CurrentLevel (string)
//     4: CurrentSpawnPoint (string)

// Full file data kept for round-tripping
static std::vector<uint8_t> g_original_file_data;
static std::vector<proto::Field> g_profile_fields;

namespace gplayer {

bool read(const std::string& path, InventoryState& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    g_original_file_data.assign(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    f.close();

    if (g_original_file_data.empty()) return false;

    // Parse top-level PlayerProfile
    g_profile_fields = proto::parse(g_original_file_data.data(), g_original_file_data.size());

    // Get GameState (field 4)
    auto* game_state = proto::find(g_profile_fields, 4);
    if (!game_state) return false;
    proto::parse_children(*game_state);

    // Get CharacterState (GameState field 1)
    auto* char_state = proto::find(game_state->children, 1);
    if (!char_state) return false;
    proto::parse_children(*char_state);

    // Read scalar fields
    auto& cs = char_state->children;
    auto* fld = proto::find(cs, 2);
    if (fld) out.current_health = (int)fld->varint;
    fld = proto::find(cs, 4);
    if (fld) out.current_mana = (int)fld->varint;
    fld = proto::find(cs, 5);
    if (fld) out.coins = (int)fld->varint;
    fld = proto::find(cs, 6);
    if (fld) out.xp = (int)fld->varint;
    fld = proto::find(cs, 7);
    if (fld) out.level = (int)fld->varint;
    fld = proto::find(cs, 20);
    if (fld) out.health_attr = (int)fld->varint;
    fld = proto::find(cs, 21);
    if (fld) out.attack_attr = (int)fld->varint;
    fld = proto::find(cs, 22);
    if (fld) out.magic_attr = (int)fld->varint;

    // Read strings
    fld = proto::find(cs, 12);
    if (fld) out.equipped_weapon = proto::get_string(*fld);
    fld = proto::find(cs, 13);
    if (fld) out.equipped_armor = proto::get_string(*fld);
    fld = proto::find(cs, 16);
    if (fld) out.current_skill = proto::get_string(*fld);
    fld = proto::find(cs, 17);
    if (fld) out.weapon_trinket = proto::get_string(*fld);
    fld = proto::find(cs, 18);
    if (fld) out.armor_trinket = proto::get_string(*fld);
    fld = proto::find(cs, 19);
    if (fld) out.skill_trinket = proto::get_string(*fld);

    // Read items (field 11, repeated)
    out.items.clear();
    for (auto& item_field : cs) {
        if (item_field.number == 11) {
            proto::parse_children(item_field);
            InventoryItem it;
            auto* name_f = proto::find(item_field.children, 1);
            auto* count_f = proto::find(item_field.children, 2);
            if (name_f) it.name = proto::get_string(*name_f);
            it.count = count_f ? (int)count_f->varint : 1;
            if (!it.name.empty()) out.items.push_back(it);
        }
    }

    // Read skills (field 15, repeated strings)
    out.skills.clear();
    for (auto& skill_field : cs) {
        if (skill_field.number == 15) {
            out.skills.push_back(proto::get_string(skill_field));
        }
    }

    // Read level info from GameState
    fld = proto::find(game_state->children, 3);
    if (fld) out.current_level = proto::get_string(*fld);
    fld = proto::find(game_state->children, 4);
    if (fld) out.spawn_point = proto::get_string(*fld);

    return true;
}

bool write(const std::string& path, const InventoryState& inv,
           const std::vector<uint8_t>& /* original_data */) {
    // Modify the parsed field tree in-place, preserving unknown fields

    auto* game_state = proto::find(g_profile_fields, 4);
    if (!game_state) return false;
    auto* char_state = proto::find(game_state->children, 1);
    if (!char_state) return false;

    auto& cs = char_state->children;

    // Update scalar fields (create if missing)
    auto set_varint = [&cs](int num, int val) {
        auto* f = proto::find(cs, num);
        if (f) { f->varint = (uint64_t)val; }
        else { cs.push_back(proto::make_varint(num, (uint64_t)val)); }
    };

    set_varint(2, inv.current_health);
    set_varint(4, inv.current_mana);
    set_varint(5, inv.coins);
    set_varint(6, inv.xp);
    set_varint(7, inv.level);
    set_varint(20, inv.health_attr);
    set_varint(21, inv.attack_attr);
    set_varint(22, inv.magic_attr);

    // Update string fields
    auto set_str = [&cs](int num, const std::string& val) {
        auto* f = proto::find(cs, num);
        if (f) { proto::set_string(*f, val); }
        else if (!val.empty()) { cs.push_back(proto::make_string(num, val)); }
    };

    set_str(12, inv.equipped_weapon);
    set_str(13, inv.equipped_armor);
    set_str(16, inv.current_skill);
    set_str(17, inv.weapon_trinket);
    set_str(18, inv.armor_trinket);
    set_str(19, inv.skill_trinket);

    // Replace items (field 11) — remove all, re-add
    proto::remove_all(cs, 11);
    for (const auto& item : inv.items) {
        proto::Field item_field;
        item_field.number = 11;
        item_field.wire_type = proto::LENGTH_DELIM;
        item_field.parsed_children = true;
        item_field.children.push_back(proto::make_string(1, item.name));
        item_field.children.push_back(proto::make_varint(2, (uint64_t)item.count));
        cs.push_back(item_field);
    }

    // Replace skills (field 15) — remove all, re-add
    proto::remove_all(cs, 15);
    for (const auto& skill : inv.skills) {
        cs.push_back(proto::make_string(15, skill));
    }

    // Also update the top-level PlayerProfile fields
    auto* wpn = proto::find(g_profile_fields, 5);
    if (wpn) proto::set_string(*wpn, inv.equipped_weapon);
    auto* arm = proto::find(g_profile_fields, 6);
    if (arm) proto::set_string(*arm, inv.equipped_armor);
    auto* wt = proto::find(g_profile_fields, 7);
    if (wt) proto::set_string(*wt, inv.weapon_trinket);
    auto* at = proto::find(g_profile_fields, 8);
    if (at) proto::set_string(*at, inv.armor_trinket);

    // Serialize everything back
    auto bytes = proto::serialize(g_profile_fields);

    // Write to file
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write((const char*)bytes.data(), bytes.size());
    f.close();

    return true;
}

} // namespace gplayer


// ============================================================================
// SrtOverlay — Constructor
// ============================================================================

#include "platform/data_path.h"

SrtOverlay::SrtOverlay() {
    // Dynamic VFS save directory
    save_dir = get_vfs_save_dir();
}


// ============================================================================
// Drawing Helpers
// ============================================================================

void SrtOverlay::draw_panel_bg(float x, float y, float w, float h) {
    // Dark semi-transparent background
    glColor4ub(18, 18, 26, 230);
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();

    // Subtle border
    glColor4ub(60, 60, 80, 200);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}

bool SrtOverlay::is_hover(float x, float y, float w, float h, int mx, int my) {
    return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

void SrtOverlay::draw_item_button(GuiRenderer& gui, float x, float y, float w, float h,
                                    const char* label, bool active, bool hover,
                                    uint8_t r, uint8_t g, uint8_t b) {
    // Button background
    if (active) {
        glColor4ub(r, g, b, 180);
    } else if (hover) {
        glColor4ub(r/2, g/2, b/2, 120);
    } else {
        glColor4ub(35, 35, 50, 150);
    }
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();

    // Border
    if (active) {
        glColor4ub(r, g, b, 255);
    } else {
        glColor4ub(60, 60, 80, 150);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();

    // Label text
    float text_x = x + 8;
    float text_y = y + h/2 - 4;
    if (active) {
        gui.draw_string(label, text_x, text_y, 1.2f, 255, 255, 255, 255);
    } else {
        gui.draw_string(label, text_x, text_y, 1.2f, 180, 180, 200, 255);
    }
}


// ============================================================================
// Save File Management
// ============================================================================

void SrtOverlay::scan_saves() {
    save_files.clear();
    std::string eff_dir = get_vfs_save_dir(save_dir);
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(eff_dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(eff_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.size() > 8 && name.substr(name.size() - 8) == ".gplayer") {
            // Only include if name doesn't contain "_b" followed by a digit before .gplayer
            bool is_backup = false;
            size_t bp = name.rfind("_b");
            if (bp != std::string::npos && bp + 2 < name.size() - 8) {
                char c = name[bp + 2];
                if (c >= '0' && c <= '9') is_backup = true;
            }
            if (!is_backup) {
                save_files.push_back(name);
            }
        }
        ec.clear();
    }
    std::sort(save_files.begin(), save_files.end());
}

bool SrtOverlay::load_save(const std::string& path) {
    InventoryState new_inv;
    if (!gplayer::read(path, new_inv)) return false;
    inventory = new_inv;
    active_save_file = path;
    inventory_dirty = false;
    return true;
}

bool SrtOverlay::write_save(const std::string& path) {
    if (!gplayer::write(path, inventory, g_original_file_data)) return false;
    inventory_dirty = false;
    return true;
}


// ============================================================================
// Render — Save Selector (top bar)
// ============================================================================

void SrtOverlay::render_save_selector(GuiRenderer& gui, float x, float y, float w,
                                        int mouse_x, int mouse_y, bool mouse_click) {
    float bar_h = 32;
    draw_panel_bg(x, y, w, bar_h);

    // Title
    gui.draw_string("SAVE:", x + 10, y + bar_h/2 - 4, 1.2f, 100, 180, 255, 255);

    // Save file buttons
    float bx = x + 65;
    for (size_t i = 0; i < save_files.size() && i < 5; i++) {
        std::string short_name = save_files[i];
        if (short_name.size() > 20) short_name = short_name.substr(0, 17) + "...";
        // Remove .gplayer extension for display
        size_t ext = short_name.rfind(".gplayer");
        if (ext != std::string::npos) short_name = short_name.substr(0, ext);

        float bw = short_name.size() * 10 + 20;
        bool sel = ((int)i == selected_save);
        bool hov = is_hover(bx, y + 4, bw, 24, mouse_x, mouse_y);

        if (sel) {
            glColor4ub(40, 100, 200, 180);
        } else if (hov) {
            glColor4ub(40, 60, 100, 120);
        } else {
            glColor4ub(30, 30, 45, 150);
        }
        glBegin(GL_QUADS);
        glVertex2f(bx, y + 4); glVertex2f(bx + bw, y + 4);
        glVertex2f(bx + bw, y + 28); glVertex2f(bx, y + 28);
        glEnd();

        gui.draw_string(short_name, bx + 10, y + 12, 1.1f,
                          sel ? 255 : 160, sel ? 255 : 160, sel ? 255 : 180, 255);

        if (hov && mouse_click && (int)i != selected_save) {
            selected_save = (int)i;
            load_save(save_dir + save_files[i]);
            status_message = "Loaded: " + save_files[i];
            status_timer = 3.0f;
        }

        bx += bw + 6;
    }

    // Save button (right side)
    if (inventory_dirty) {
        float save_bx = x + w - 90;
        bool save_hov = is_hover(save_bx, y + 4, 80, 24, mouse_x, mouse_y);
        glColor4ub(save_hov ? 60 : 30, save_hov ? 180 : 140, save_hov ? 60 : 30, 200);
        glBegin(GL_QUADS);
        glVertex2f(save_bx, y + 4); glVertex2f(save_bx + 80, y + 4);
        glVertex2f(save_bx + 80, y + 28); glVertex2f(save_bx, y + 28);
        glEnd();
        gui.draw_string("SAVE", save_bx + 22, y + 12, 1.3f, 255, 255, 255, 255);

        if (save_hov && mouse_click && !active_save_file.empty()) {
            if (write_save(active_save_file)) {
                status_message = "Saved! Restart game to apply.";
                status_timer = 4.0f;
            } else {
                status_message = "ERROR: Failed to write save file!";
                status_timer = 5.0f;
            }
        }
    }
}


// ============================================================================
// Render — Sidebar (category tabs)
// ============================================================================

void SrtOverlay::render_sidebar(GuiRenderer& gui, float x, float y, float h,
                                  int mouse_x, int mouse_y, bool mouse_click) {
    float tab_w = 140;
    draw_panel_bg(x, y, tab_w, h);

    struct Tab { const char* label; OverlayPanel panel; uint8_t r, g, b; };
    Tab tabs[] = {
        {"Inventory",  PANEL_INVENTORY, 60, 160, 255},
        {"Stats",      PANEL_STATS,     255, 180, 60},
    };
    int num_tabs = sizeof(tabs) / sizeof(tabs[0]);

    float ty = y + h - 40;  // Start from top (Y increases downward in our coord)
    for (int i = 0; i < num_tabs; i++) {
        bool sel = (active_panel == tabs[i].panel);
        bool hov = is_hover(x + 5, ty, tab_w - 10, 30, mouse_x, mouse_y);

        draw_item_button(gui, x + 5, ty, tab_w - 10, 30,
                          tabs[i].label, sel, hov,
                          tabs[i].r, tabs[i].g, tabs[i].b);

        if (hov && mouse_click) {
            active_panel = tabs[i].panel;
        }

        ty -= 38;
    }

    // Info section at bottom of sidebar
    gui.draw_string("F9: Toggle", x + 10, y + 10, 1.0f, 80, 80, 100, 200);
}


// ============================================================================
// Render — Inventory Panel
// ============================================================================

void SrtOverlay::render_inventory_panel(GuiRenderer& gui, float x, float y, float w, float h,
                                          int mouse_x, int mouse_y, bool mouse_click) {
    draw_panel_bg(x, y, w, h);

    float section_w = w / 3.0f;
    float cy = y + h - 15;  // Start from top
    float line_h = 28;

    // ===== Column 1: Weapons =====
    float cx = x + 10;
    gui.draw_string("WEAPONS", cx, cy, 1.3f, 255, 140, 40, 255);
    cy -= 6;

    // Draw accent line under header
    glColor4ub(255, 140, 40, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(cx + section_w - 30, cy);
    glVertex2f(cx + section_w - 30, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= line_h;

    for (int i = 0; i < 7; i++) {
        bool has = inventory.has_item(ALL_WEAPONS[i].id);
        bool equipped = (inventory.equipped_weapon == ALL_WEAPONS[i].id);
        bool hov = is_hover(cx, cy - 3, section_w - 30, 22, mouse_x, mouse_y);

        std::string label = std::string(ALL_WEAPONS[i].name);
        if (equipped) label = "> " + label;
        if (ALL_WEAPONS[i].stat > 0) {
            label += " (+" + std::to_string(ALL_WEAPONS[i].stat) + ")";
        }

        draw_item_button(gui, cx, cy - 3, section_w - 30, 22,
                          label.c_str(), has, hov,
                          255, equipped ? 220 : 140, equipped ? 100 : 40);

        if (hov && mouse_click) {
            inventory_dirty = true;
            if (has) {
                // Toggle equip if already owned
                if (equipped) {
                    inventory.equipped_weapon = "";
                } else {
                    inventory.equipped_weapon = ALL_WEAPONS[i].id;
                }
            } else {
                // Add item
                inventory.add_item(ALL_WEAPONS[i].id);
                inventory.equipped_weapon = ALL_WEAPONS[i].id;
            }
        }

        cy -= line_h;
    }

    // Armor
    cy -= 10;
    gui.draw_string("ARMOR", cx, cy, 1.3f, 80, 180, 255, 255);
    cy -= 6;
    glColor4ub(80, 180, 255, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(cx + section_w - 30, cy);
    glVertex2f(cx + section_w - 30, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= line_h;

    for (int i = 0; i < 2; i++) {
        bool has = inventory.has_item(ALL_ARMOR[i].id);
        bool equipped = (inventory.equipped_armor == ALL_ARMOR[i].id);
        bool hov = is_hover(cx, cy - 3, section_w - 30, 22, mouse_x, mouse_y);

        std::string label = std::string(ALL_ARMOR[i].name);
        if (equipped) label = "> " + label;
        label += " (-" + std::to_string(ALL_ARMOR[i].stat) + "%)";

        draw_item_button(gui, cx, cy - 3, section_w - 30, 22,
                          label.c_str(), has, hov,
                          80, equipped ? 220 : 180, 255);

        if (hov && mouse_click) {
            inventory_dirty = true;
            if (has) {
                if (equipped) inventory.equipped_armor = "";
                else inventory.equipped_armor = ALL_ARMOR[i].id;
            } else {
                inventory.add_item(ALL_ARMOR[i].id);
                inventory.equipped_armor = ALL_ARMOR[i].id;
            }
        }
        cy -= line_h;
    }

    // ===== Column 2: Spells + Trinkets =====
    cx = x + section_w + 10;
    cy = y + h - 15;

    gui.draw_string("SPELLS", cx, cy, 1.3f, 180, 80, 255, 255);
    cy -= 6;
    glColor4ub(180, 80, 255, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(cx + section_w - 30, cy);
    glVertex2f(cx + section_w - 30, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= line_h;

    for (int i = 0; i < 4; i++) {
        bool has = inventory.has_skill(ALL_SPELLS[i].id);
        bool active = (inventory.current_skill == ALL_SPELLS[i].id);
        bool hov = is_hover(cx, cy - 3, section_w - 30, 22, mouse_x, mouse_y);

        std::string label = std::string(ALL_SPELLS[i].name);
        if (active) label = "> " + label;

        draw_item_button(gui, cx, cy - 3, section_w - 30, 22,
                          label.c_str(), has, hov,
                          180, 80, 255);

        if (hov && mouse_click) {
            inventory_dirty = true;
            if (has) {
                if (active) inventory.current_skill = "";
                else inventory.current_skill = ALL_SPELLS[i].id;
            } else {
                inventory.add_skill(ALL_SPELLS[i].id);
                inventory.current_skill = ALL_SPELLS[i].id;
            }
        }
        cy -= line_h;
    }

    // Trinkets
    cy -= 10;
    gui.draw_string("TRINKETS", cx, cy, 1.3f, 255, 220, 80, 255);
    cy -= 6;
    glColor4ub(255, 220, 80, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(cx + section_w - 30, cy);
    glVertex2f(cx + section_w - 30, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= line_h;

    for (int i = 0; i < 3; i++) {
        bool has = inventory.has_item(ALL_TRINKETS[i].id);
        bool hov = is_hover(cx, cy - 3, section_w - 30, 22, mouse_x, mouse_y);

        std::string label = std::string(ALL_TRINKETS[i].name);
        bool is_weapon_t = (inventory.weapon_trinket == ALL_TRINKETS[i].id);
        bool is_armor_t = (inventory.armor_trinket == ALL_TRINKETS[i].id);
        if (is_weapon_t) label += " [W]";
        if (is_armor_t) label += " [A]";

        draw_item_button(gui, cx, cy - 3, section_w - 30, 22,
                          label.c_str(), has, hov,
                          255, 220, 80);

        if (hov && mouse_click) {
            inventory_dirty = true;
            if (has) {
                // Cycle: unequipped → weapon trinket → armor trinket → remove
                if (!is_weapon_t && !is_armor_t) {
                    inventory.weapon_trinket = ALL_TRINKETS[i].id;
                } else if (is_weapon_t) {
                    inventory.weapon_trinket = "";
                    inventory.armor_trinket = ALL_TRINKETS[i].id;
                } else {
                    inventory.armor_trinket = "";
                }
            } else {
                inventory.add_item(ALL_TRINKETS[i].id);
                inventory.weapon_trinket = ALL_TRINKETS[i].id;
            }
        }
        cy -= line_h;
    }

    // ===== Column 3: Consumables + Quest Items =====
    cx = x + section_w * 2 + 10;
    cy = y + h - 15;

    gui.draw_string("CONSUMABLES", cx, cy, 1.3f, 80, 220, 120, 255);
    cy -= 6;
    glColor4ub(80, 220, 120, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(cx + section_w - 30, cy);
    glVertex2f(cx + section_w - 30, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= line_h;

    for (int i = 0; i < 3; i++) {
        bool has = inventory.has_item(ALL_CONSUMABLES[i].id);
        bool hov = is_hover(cx, cy - 3, section_w - 30, 22, mouse_x, mouse_y);

        // Show count if owned
        std::string label = std::string(ALL_CONSUMABLES[i].name);
        if (has) {
            for (auto& it : inventory.items) {
                if (it.name == ALL_CONSUMABLES[i].id) {
                    label += " x" + std::to_string(it.count);
                    break;
                }
            }
        }

        draw_item_button(gui, cx, cy - 3, section_w - 30, 22,
                          label.c_str(), has, hov,
                          80, 220, 120);

        if (hov && mouse_click) {
            inventory_dirty = true;
            if (has) {
                // Add more (shift-click to remove would be nice but keyboard state is complex)
                inventory.add_item(ALL_CONSUMABLES[i].id, 1);
            } else {
                inventory.add_item(ALL_CONSUMABLES[i].id, 3);
            }
        }
        cy -= line_h;
    }

    // Quest Items
    cy -= 10;
    gui.draw_string("QUEST ITEMS", cx, cy, 1.3f, 200, 200, 220, 255);
    cy -= 6;
    glColor4ub(200, 200, 220, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(cx + section_w - 30, cy);
    glVertex2f(cx + section_w - 30, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= line_h;

    for (int i = 0; i < 5; i++) {
        bool has = inventory.has_item(ALL_QUEST_ITEMS[i].id);
        bool hov = is_hover(cx, cy - 3, section_w - 30, 22, mouse_x, mouse_y);

        draw_item_button(gui, cx, cy - 3, section_w - 30, 22,
                          ALL_QUEST_ITEMS[i].name, has, hov,
                          200, 200, 220);

        if (hov && mouse_click) {
            inventory_dirty = true;
            if (has) inventory.remove_item(ALL_QUEST_ITEMS[i].id);
            else inventory.add_item(ALL_QUEST_ITEMS[i].id);
        }
        cy -= line_h;
    }

    // ===== Quick Actions =====
    cy -= 20;
    float btn_w = (section_w - 30);

    // "Give All" button
    bool hov_all = is_hover(cx, cy - 3, btn_w, 26, mouse_x, mouse_y);
    glColor4ub(hov_all ? 200 : 120, hov_all ? 60 : 40, hov_all ? 60 : 40, 200);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy - 3); glVertex2f(cx + btn_w, cy - 3);
    glVertex2f(cx + btn_w, cy + 23); glVertex2f(cx, cy + 23);
    glEnd();
    gui.draw_string("GIVE ALL ITEMS", cx + 20, cy + 5, 1.2f, 255, 255, 255, 255);

    if (hov_all && mouse_click) {
        inventory_dirty = true;
        for (int i = 0; i < 7; i++) inventory.add_item(ALL_WEAPONS[i].id);
        for (int i = 0; i < 2; i++) inventory.add_item(ALL_ARMOR[i].id);
        for (int i = 0; i < 3; i++) inventory.add_item(ALL_TRINKETS[i].id);
        for (int i = 0; i < 3; i++) inventory.add_item(ALL_CONSUMABLES[i].id, 10);
        for (int i = 0; i < 5; i++) inventory.add_item(ALL_QUEST_ITEMS[i].id);
        for (int i = 0; i < 4; i++) inventory.add_skill(ALL_SPELLS[i].id);
        inventory.equipped_weapon = "legendsword";
        inventory.equipped_armor = "magicarmor";
        inventory.current_skill = "dimension";
        status_message = "All items + spells added!";
        status_timer = 3.0f;
    }
}


// ============================================================================
// Render — Stats Panel
// ============================================================================

void SrtOverlay::render_stats_panel(GuiRenderer& gui, float x, float y, float w, float h,
                                      int mouse_x, int mouse_y, bool mouse_click) {
    draw_panel_bg(x, y, w, h);

    float cy = y + h - 20;
    float cx = x + 20;
    float val_x = x + 200;

    gui.draw_string("CHARACTER STATS", cx, cy, 1.5f, 255, 200, 80, 255);
    cy -= 12;
    glColor4ub(255, 200, 80, 100);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy); glVertex2f(x + w - 20, cy);
    glVertex2f(x + w - 20, cy - 2); glVertex2f(cx, cy - 2);
    glEnd();
    cy -= 35;

    // Each stat: label + value + [-] [+] buttons
    struct StatRow {
        const char* label;
        int* value;
        int min_val, max_val;
        uint8_t r, g, b;
    };

    StatRow rows[] = {
        {"Health",           &inventory.current_health,  1, 999,  220, 60, 60},
        {"Mana",             &inventory.current_mana,    0, 999,  60, 120, 255},
        {"Coins",            &inventory.coins,           0, 99999, 255, 220, 40},
        {"XP",               &inventory.xp,              0, 99999, 180, 255, 120},
        {"Level",            &inventory.level,            1, 50,   255, 180, 60},
        {"HP Attribute",     &inventory.health_attr,      0, 10,   220, 80, 80},
        {"ATK Attribute",    &inventory.attack_attr,      0, 10,   255, 140, 60},
        {"MAG Attribute",    &inventory.magic_attr,       0, 10,   120, 80, 255},
    };
    int num_rows = sizeof(rows) / sizeof(rows[0]);

    for (int i = 0; i < num_rows; i++) {
        // Label
        gui.draw_string(rows[i].label, cx, cy, 1.3f, rows[i].r, rows[i].g, rows[i].b, 255);

        // Value
        gui.draw_string(std::to_string(*rows[i].value), val_x, cy, 1.3f, 255, 255, 255, 255);

        // [-] button
        float btn_x = val_x + 80;
        bool hov_minus = is_hover(btn_x, cy - 3, 26, 20, mouse_x, mouse_y);
        glColor4ub(hov_minus ? 180 : 80, 40, 40, 200);
        glBegin(GL_QUADS);
        glVertex2f(btn_x, cy - 3); glVertex2f(btn_x + 26, cy - 3);
        glVertex2f(btn_x + 26, cy + 17); glVertex2f(btn_x, cy + 17);
        glEnd();
        gui.draw_string("-", btn_x + 9, cy + 2, 1.3f, 255, 255, 255, 255);

        if (hov_minus && mouse_click) {
            int step = (*rows[i].value > 100) ? 10 : 1;
            *rows[i].value = std::max(rows[i].min_val, *rows[i].value - step);
            inventory_dirty = true;
        }

        // [+] button
        btn_x += 32;
        bool hov_plus = is_hover(btn_x, cy - 3, 26, 20, mouse_x, mouse_y);
        glColor4ub(40, hov_plus ? 180 : 80, 40, 200);
        glBegin(GL_QUADS);
        glVertex2f(btn_x, cy - 3); glVertex2f(btn_x + 26, cy - 3);
        glVertex2f(btn_x + 26, cy + 17); glVertex2f(btn_x, cy + 17);
        glEnd();
        gui.draw_string("+", btn_x + 9, cy + 2, 1.3f, 255, 255, 255, 255);

        if (hov_plus && mouse_click) {
            int step = (*rows[i].value >= 100) ? 10 : 1;
            *rows[i].value = std::min(rows[i].max_val, *rows[i].value + step);
            inventory_dirty = true;
        }

        cy -= 32;
    }

    // Current level display
    cy -= 20;
    gui.draw_string("Current Level:", cx, cy, 1.2f, 150, 150, 180, 255);
    gui.draw_string(inventory.current_level.empty() ? "(none)" : inventory.current_level,
                     val_x - 40, cy, 1.2f, 200, 200, 255, 255);
    cy -= 22;
    gui.draw_string("Spawn Point:", cx, cy, 1.2f, 150, 150, 180, 255);
    gui.draw_string(inventory.spawn_point.empty() ? "(none)" : inventory.spawn_point,
                     val_x - 40, cy, 1.2f, 200, 200, 255, 255);

    // Max stats button
    cy -= 40;
    bool hov_max = is_hover(cx, cy - 3, 200, 28, mouse_x, mouse_y);
    glColor4ub(hov_max ? 200 : 120, hov_max ? 160 : 100, hov_max ? 40 : 20, 200);
    glBegin(GL_QUADS);
    glVertex2f(cx, cy - 3); glVertex2f(cx + 200, cy - 3);
    glVertex2f(cx + 200, cy + 25); glVertex2f(cx, cy + 25);
    glEnd();
    gui.draw_string("MAX ALL STATS", cx + 35, cy + 6, 1.3f, 255, 255, 255, 255);

    if (hov_max && mouse_click) {
        inventory.current_health = 100;
        inventory.current_mana = 100;
        inventory.coins = 9999;
        inventory.xp = 50000;
        inventory.level = 20;
        inventory.health_attr = 10;
        inventory.attack_attr = 10;
        inventory.magic_attr = 10;
        inventory_dirty = true;
        status_message = "Stats maxed out!";
        status_timer = 3.0f;
    }
}


// ============================================================================
// Render — Status Bar (bottom)
// ============================================================================

void SrtOverlay::render_status_bar(GuiRenderer& gui, float x, float y, float w) {
    if (status_timer <= 0 && !inventory_dirty) return;

    float bar_h = 24;
    draw_panel_bg(x, y, w, bar_h);

    if (status_timer > 0) {
        uint8_t alpha = (uint8_t)(std::min(1.0f, status_timer) * 255);
        gui.draw_string(status_message, x + 10, y + 8, 1.1f, 
                          100, 255, 140, alpha);
    } else if (inventory_dirty) {
        gui.draw_string("* Unsaved changes — click SAVE to write", x + 10, y + 8, 1.1f,
                          255, 200, 80, 200);
    }
}


// ============================================================================
// Main Render Entry Point
// ============================================================================

void SrtOverlay::render(GuiRenderer& gui, int win_w, int win_h,
                         int mouse_x, int mouse_y, bool mouse_click, float dt) {
    if (!visible) return;

    // Update timers
    if (status_timer > 0) status_timer -= dt;

    // First open? Scan saves
    if (save_files.empty()) {
        scan_saves();
        if (!save_files.empty() && active_save_file.empty()) {
            load_save(save_dir + save_files[0]);
        }
    }

    // Layout
    float margin = 40;
    float sidebar_w = 150;
    float panel_x = margin + sidebar_w + 5;
    float panel_w = win_w - panel_x - margin;
    float panel_h = win_h - margin * 2 - 40 - 30;  // Leave room for save bar + status
    float panel_y = margin + 30;  // Above status bar

    // mouse_y is already in GL space (Y=0 at bottom) from the event handler

    // Draw dim overlay behind everything
    glColor4ub(0, 0, 0, 120);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(win_w, 0);
    glVertex2f(win_w, win_h); glVertex2f(0, win_h);
    glEnd();

    // Save selector (top)
    render_save_selector(gui, panel_x, panel_y + panel_h + 5, panel_w,
                          mouse_x, mouse_y, mouse_click);

    // Sidebar (left)
    render_sidebar(gui, margin, panel_y, panel_h, mouse_x, mouse_y, mouse_click);

    // Main content panel
    switch (active_panel) {
        case PANEL_INVENTORY:
            render_inventory_panel(gui, panel_x, panel_y, panel_w, panel_h,
                                    mouse_x, mouse_y, mouse_click);
            break;
        case PANEL_STATS:
            render_stats_panel(gui, panel_x, panel_y, panel_w, panel_h,
                                mouse_x, mouse_y, mouse_click);
            break;
        default:
            break;
    }

    // Status bar (bottom)
    render_status_bar(gui, panel_x, margin, panel_w);

    // Title
    gui.draw_string("SRT INVENTORY EDITOR", panel_x + panel_w/2 - 100,
                      panel_y + panel_h + 42, 1.5f, 60, 180, 255, 255);
}


// ============================================================================
// Handle Key
// ============================================================================

bool SrtOverlay::handle_key(int scancode) {
    // F9 toggles overlay
    // (handled in main.cpp, this is for internal keys)

    if (!visible) return false;

    // Tab to switch panels
    if (scancode == 43) { // SDL_SCANCODE_TAB
        if (active_panel == PANEL_INVENTORY) active_panel = PANEL_STATS;
        else active_panel = PANEL_INVENTORY;
        return true;
    }

    // Escape to close
    if (scancode == 41) { // SDL_SCANCODE_ESCAPE
        visible = false;
        return true;
    }

    return false;
}
