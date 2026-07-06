/*
 * srt_overlay.h — SRT In-Game Overlay System
 *
 * Provides a premium overlay rendering layer for tools like
 * the inventory editor, asset browser, and debug panels.
 * 
 * Renders AFTER PostFX using the host's OpenGL context.
 * Uses the existing GuiRenderer for text + drawing primitives.
 *
 * Toggle: F9 opens the overlay menu
 */

#ifndef SRT_OVERLAY_H
#define SRT_OVERLAY_H

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// Forward declarations
class GuiRenderer;

// ============================================================================
// Swordigo item/spell/trinket catalog (from gamedata.gdata)
// ============================================================================

struct GameItem {
    const char* id;         // Internal name (e.g. "brasssword")
    const char* name;       // Display name (e.g. "Brass Sword")
    int type;               // 1=consumable, 2=weapon, 3=armor, 5=quest
    int stat;               // Damage bonus (weapons) or armor % (armor)
};

// All known items in vanilla Swordigo
static const GameItem ALL_WEAPONS[] = {
    {"brasssword",  "Brass Sword",   2, 0},
    {"ironsword",   "Iron Sword",    2, 1},
    {"needle",      "The Needle",    2, 1},
    {"broadsword",  "Broad Sword",   2, 2},
    {"thorn",       "The Thorn",     2, 2},
    {"magicsword",  "Magic Sword",   2, 3},
    {"legendsword", "The Mageblade", 2, 4},
};

static const GameItem ALL_ARMOR[] = {
    {"platearmor",  "Plate Armor",  3, 50},
    {"magicarmor",  "Magic Armor",  3, 75},
};

static const GameItem ALL_TRINKETS[] = {
    {"firetrinket",   "Trinket of Fire",   1, 0},
    {"icetrinket",    "Trinket of Ice",    1, 0},
    {"shadowtrinket", "Trinket of Shadow", 1, 0},
};

static const GameItem ALL_SPELLS[] = {
    {"bolt",      "Magic Bolt",     0, 0},
    {"bomb",      "Magic Bomb",     0, 0},
    {"hookshot",  "Dragon's Grasp", 0, 0},
    {"dimension", "Dimension Rift", 0, 0},
};

static const GameItem ALL_CONSUMABLES[] = {
    {"healingpotion",  "Healing Potion",      1, 0},
    {"experiencesack", "Sack of Experience",   1, 0},
    {"ankh",           "Ankh of Resurrection", 1, 0},
};

static const GameItem ALL_QUEST_ITEMS[] = {
    {"key_yellow",     "A Key",            5, 0},
    {"iselon_shard_1", "Mageblade Shard 1", 5, 0},
    {"iselon_shard_2", "Mageblade Shard 2", 5, 0},
    {"iselon_shard_3", "Mageblade Shard 3", 5, 0},
    {"iselon_shard_4", "Mageblade Shard 4", 5, 0},
};

// ============================================================================
// Inventory data (read from / written to .gplayer save files)
// ============================================================================

struct InventoryItem {
    std::string name;
    int count;
};

struct InventoryState {
    // Character stats
    int current_health = 3;
    int current_mana = 0;
    int coins = 0;
    int xp = 0;
    int level = 1;
    int health_attr = 0;
    int attack_attr = 0;
    int magic_attr = 0;

    // Equipment
    std::string equipped_weapon;
    std::string equipped_armor;
    std::string weapon_trinket;
    std::string armor_trinket;
    std::string skill_trinket;
    std::string current_skill;

    // Inventory
    std::vector<InventoryItem> items;
    std::vector<std::string> skills;

    // Level
    std::string current_level;
    std::string spawn_point;

    // Has item?
    bool has_item(const std::string& id) const {
        for (auto& it : items) if (it.name == id) return true;
        return false;
    }

    void add_item(const std::string& id, int count = 1) {
        for (auto& it : items) {
            if (it.name == id) { it.count += count; return; }
        }
        items.push_back({id, count});
    }

    void remove_item(const std::string& id) {
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].name == id) {
                items.erase(items.begin() + i);
                return;
            }
        }
    }

    bool has_skill(const std::string& id) const {
        for (auto& s : skills) if (s == id) return true;
        return false;
    }

    void add_skill(const std::string& id) {
        if (!has_skill(id)) skills.push_back(id);
    }

    void remove_skill(const std::string& id) {
        for (size_t i = 0; i < skills.size(); i++) {
            if (skills[i] == id) {
                skills.erase(skills.begin() + i);
                return;
            }
        }
    }
};

// ============================================================================
// Overlay panel types
// ============================================================================

enum OverlayPanel {
    PANEL_NONE = 0,
    PANEL_INVENTORY,
    PANEL_STATS,
    PANEL_ITEMS_ALL,    // "Add all items" confirmation
};

// ============================================================================
// SRT Overlay — main class
// ============================================================================

class SrtOverlay {
public:
    SrtOverlay();

    // Called each frame from main.cpp render loop
    void render(GuiRenderer& gui, int win_w, int win_h,
                int mouse_x, int mouse_y, bool mouse_click, float dt);

    // Handle key press (returns true if consumed)
    bool handle_key(int scancode);

    // Toggle visibility
    void toggle() { visible = !visible; }
    bool is_visible() const { return visible; }

    // State
    bool visible = false;
    OverlayPanel active_panel = PANEL_INVENTORY;

    // Save file paths
    std::string save_dir;
    std::string active_save_file;

    // Current inventory being edited
    InventoryState inventory;
    bool inventory_dirty = false;
    std::string status_message;
    float status_timer = 0;

    // Save file list
    std::vector<std::string> save_files;
    int selected_save = 0;

    // Scroll state
    int scroll_y = 0;

private:
    // Render sub-panels
    void render_inventory_panel(GuiRenderer& gui, float x, float y, float w, float h,
                                 int mouse_x, int mouse_y, bool mouse_click);
    void render_stats_panel(GuiRenderer& gui, float x, float y, float w, float h,
                             int mouse_x, int mouse_y, bool mouse_click);
    void render_sidebar(GuiRenderer& gui, float x, float y, float h,
                         int mouse_x, int mouse_y, bool mouse_click);
    void render_save_selector(GuiRenderer& gui, float x, float y, float w,
                               int mouse_x, int mouse_y, bool mouse_click);
    void render_status_bar(GuiRenderer& gui, float x, float y, float w);

    // Drawing helpers
    void draw_panel_bg(float x, float y, float w, float h);
    void draw_item_button(GuiRenderer& gui, float x, float y, float w, float h,
                           const char* label, bool active, bool hover, 
                           uint8_t r, uint8_t g, uint8_t b);
    bool is_hover(float x, float y, float w, float h, int mx, int my);

public:
    // Save file I/O
    void scan_saves();
    bool load_save(const std::string& path);
    bool write_save(const std::string& path);
};

// Protobuf read/write helpers for .gplayer files
namespace gplayer {
    bool read(const std::string& path, InventoryState& out);
    bool write(const std::string& path, const InventoryState& inv,
               const std::vector<uint8_t>& original_data);
}

#endif // SRT_OVERLAY_H
