#pragma once
// scene_creator.h — Scene Creator API (Scene-Anatomy research, docs/scenecreator)
// Builds gameplay-compatible Swordigo .scene files from parameterized templates.
// All field numbers verified against src/tools/scene_schemas.cpp and the decoded
// scenes in docs/scenecreator/01_scene_file_anatomy.md.

#include <string>
#include <vector>

namespace scenecreate {

// ---------------------------------------------------------------------------
// Template enumeration — matches docs/scenecreator/06_scene_templates.md §1-5
// ---------------------------------------------------------------------------
enum class SceneTemplate {
    Minimal,      // bare: ground + spawn + light + background + bounds
    Standard,     // Minimal + outdoor scale bounds (default)
    Outdoor,      // Standard with sky-scale bounds
    Indoor,       // compact bounds + extra point-light slot
    Dungeon,      // medium bounds + cave background preset
    BossArena,    // arena-scale bounds + portal exits
    Portal,       // hub/transition: minimal + portal objects
    Menu,         // attract/idle (menu.scene pattern)
};

// ---------------------------------------------------------------------------
// LightParams — one LightComponent triple entry
// Type semantics (docs/scenecreator/03_lighting_system.md §3):
//   1 = Ambient   2 = Key/Sun   3 = Main/Point   4 = Black-fill (shadow)
// ---------------------------------------------------------------------------
struct LightParams {
    int   type      = 2;
    float intensity = 2.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

// ---------------------------------------------------------------------------
// PortalParams — a portal object (docs/scenecreator/04 §2)
// Creates: Portal (id 101) + CollisionShape (id 102) + SpawnPoint (id 105)
// ---------------------------------------------------------------------------
struct PortalParams {
    std::string destination;   // target scene stem (e.g. "grass_part1")
    std::string spawn_name;    // "" → destination default spawn
    bool  tap_to_enter = true;
    float x = 0.0f, y = 0.0f; // world position
    // CollisionShape rectangle (portal touch zone, SpecialType:2)
    float rect_x = -41.5f, rect_y = -20.0f, rect_w = 83.0f, rect_h = 110.0f;
    float min_depth = -15.0f, max_depth = 50.0f;
    int   facing    = 1;
};

// ---------------------------------------------------------------------------
// Options — full build options passed to create()
// ---------------------------------------------------------------------------
struct Options {
    // ── Identity & output ──────────────────────────────────────────────────
    std::string output_path;
    std::string level_name;
    std::string scene_namespace;

    // ── Template ───────────────────────────────────────────────────────────
    SceneTemplate scene_template = SceneTemplate::Standard;

    // ── Visuals ────────────────────────────────────────────────────────────
    std::string base_mesh;          // optional decoration Model (.pod stem)
    std::string background;         // background texture stem

    // ── Ground mesh platform ───────────────────────────────────────────────
    std::string ground_top_texture  = "fire_grass";
    std::string ground_side_texture = "graveyard_ground";
    float platform_width  = 320.0f;
    float platform_height = 48.0f;
    float platform_depth  = 90.0f;  // min+max depth (symmetric)

    // ── Spawn (spawn_default) ──────────────────────────────────────────────
    float spawn_x = 0.0f;
    float spawn_y = 56.0f;
    float spawn_z = 0.0f;
    int   spawn_facing = 1;         // 1 = right, -1 = left

    // ── Scene bounds override ─────────────────────────────────────────────
    // If all-zero the bounds are derived from the ground AABB + 200 px margin
    // (docs/scenecreator/02 §1.4).  Set explicitly for custom layouts.
    float bounds_x = 0.0f, bounds_y = 0.0f;
    float bounds_w = 0.0f, bounds_h = 0.0f;

    // ── Directional light overrides (canonical triple) ─────────────────────
    // Key light (Type 2)
    // Key intensity 3.0 matches the real shipped forest_part1 DirectionalLight
    // (decoded via ruby_cli). The old 2.0 under-lit scenes so backgrounds looked
    // black — this is the "too dark / background not visible" fix.
    LightParams key_light   { 2, 3.0f, 1, 1, 1, 1 };
    // Ambient fill (Type 1)
    LightParams ambient     { 1, 0.3f, 1, 1, 1, 1 };
    // Black fill / shadow (Type 4, Color = black)
    LightParams shadow_fill { 4, 0.4f, 0, 0, 0, 1 };

    // ── Portals ───────────────────────────────────────────────────────────
    std::vector<PortalParams> portals;

    // ── Map link ──────────────────────────────────────────────────────────
    std::string map_path;
    bool link_to_map = false;
};

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------
struct Result {
    std::string scene_path;
    std::string manifest_path;
    int object_count = 0;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Normalize a user-entered resource path to the bare Swordigo resource stem.
// Example: "/assets/models/bridge.POD" → "bridge".
std::string resource_stem(const std::string& value);

// Return a human-readable label for a template enum value.
const char* template_label(SceneTemplate t);

// Return the suggested background texture stem for a template category.
const char* template_default_background(SceneTemplate t);

// Validate and create a playable Swordigo scene.
// On success writes <level_name>.scene + <level_name>.swscene manifest.
bool create(const Options& options, Result& result, std::string& error);

} // namespace scenecreate
