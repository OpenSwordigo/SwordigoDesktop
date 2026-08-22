#pragma once
/*
 * scene_generator_v2.h — Swordigo v2 procedural scene generator.
 *
 * Extends scene_generator.h with:
 *   • ZLayer system: background silhouette terrain (Z ≤ -80),
 *     main walkable ground (Z ≈ 0), foreground cliff framing (Z ≥ +80).
 *   • Z-path: each walkable platform strip has its object-Z varied by a
 *     smooth fbm curve — the terrain path winds toward/away from the camera.
 *   • Background terrain: rougher, taller, wider background GroundMesh strips
 *     placed at object Z = -100 .. -200. Confirmed in vanilla:
 *       forest_part1: obj5#11 Depth=-136.1, obj5#12 Depth=-128.9
 *       grove_part1:  'aasdf'/'afdf'/'ffasdf' Depth=-161.4
 *   • Multi-band decoration scatter: main-layer even-spaced (as v1) PLUS
 *     explicit background scatter (Z -80..-160, scale 1.4-2.5×) and
 *     foreground scatter (Z +40..+100, scale 0.4-0.8×).
 *   • Terracing: quantized step bands in the main heightfield.
 *   • Overhangs: cliff-edge ledge jut on steep terrain drops.
 *   • BiomeSpecV2: extends BiomeSpec with deco_background[], deco_foreground[],
 *     bg_ground_texture, bg_terrain_scale.
 *
 * Vanilla-confirmed 6-tier depth hierarchy:
 *   Tier 0 Sky        depth  1.72038     BackgroundComponent
 *   Tier 1 Far BG     depth -161..-129   GroundMesh silhouettes
 *   Tier 2 Mid BG     depth -112..-49    treewall, stonepillars, bg bush
 *   Tier 3 Near BG    depth  -27..-11    minor props / pots
 *   Tier 4 Gameplay   depth  0.0         Walkable ground, hero, enemies
 *   Tier 5 Foreground depth  +22..+71    fg bush, torch, stoneblock
 *   Tier 6 Far FG     depth  +137..+813  DirectionalLight, camera volumes
 */
#include "tools/scene_generator.h"
#include <string>
#include <vector>
#include <cstdint>

namespace sgen {
namespace v2 {

// ── Extended biome spec ───────────────────────────────────────────────────────
// BiomeSpec (from scene_generator.h) covers the main layer.
// BiomeSpecV2 adds parallax decoration palettes for background/foreground bands
// and the background terrain visual parameters.
struct BiomeSpecV2 : public BiomeSpec {
    // Large-scale decorations placed at deep negative Z (-80 .. -160):
    // creates the "far forest" / "distant mountains" parallax silhouette.
    const char* deco_background[4];

    // Small-scale decorations placed at shallow positive Z (+40 .. +100):
    // framing rocks/shrubs in front of the main ground.
    const char* deco_foreground[4];

    // Texture for background GroundMesh strips (no grass cap — looks like a
    // solid cliff/rock face at distance). Vanilla uses the front/cliff texture
    // for bg strips (e.g. "forest_ground", "grove_ground", "maybegood").
    const char* bg_ground_texture;

    // Height multiplier for background terrain (1.5–2.5×  the main height).
    // Larger = taller background mountains.
    float bg_terrain_scale;
};

// ── v2 terrain options ────────────────────────────────────────────────────────
struct TerrainOptionsV2 : public TerrainOptions {
    // Z-layer configuration
    bool  enable_bg_terrain  = true;    // emit background visual terrain layers
    bool  enable_fg_terrain  = false;   // emit foreground cliff framing strip
    bool  enable_z_path      = false;   // Z-varying spline path for main ground
    float z_path_amplitude   = 0.0f;   // 0 = flat, >0 = path winds ±z_path_amplitude

    // Background terrain: how many bg strips and their Z spacing
    int   bg_layers          = 2;       // 2 → Z=-100 and Z=-200
    float bg_z_step          = -100.0f; // Z offset between consecutive bg layers

    // Decoration scatter bands (both default true)
    bool  scatter_background_decos = true;   // deep-Z large-scale trees
    bool  scatter_foreground_decos = true;   // shallow-Z small-scale rocks

    // Terrain variation
    bool  add_overhangs      = false;  // cliff ledge overhangs at steep drops
    bool  add_terracing      = false;  // stepped/quantized terrain bands
    float terrace_strength   = 0.5f;   // 0 = none, 1 = fully quantized steps

    // Vanilla camera controller: emit a 'camera_follow_y_shape' covering the
    // walkable band so the in-game camera pans vertically with the hero, and
    // compute scene Bounds from the walkable top profile (not the deep
    // heightfield floor) so the default camera starts centered on the action.
    // OFF by default — the y-shape emission is still under investigation; the
    // walkable-top-profile Bounds are derived unconditionally below.
    bool  emit_camera_shapes = false;
};

// ── API ───────────────────────────────────────────────────────────────────────

// Generate a full v2 scene. Superset of generate_biome_scene().
Result generate_biome_scene_v2(const TerrainOptionsV2& opt);

// The extended biome table (lazily initialized on first call).
const BiomeSpecV2& biome_spec_v2(Biome b);

} // namespace v2
} // namespace sgen
