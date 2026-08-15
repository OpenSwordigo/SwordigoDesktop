#pragma once
/*
 * scene_generator.h — Procedural Swordigo scene mesh generator (Ruby SDK).
 *
 * v3 remaster: evidence-driven from direct ruby_cli decode of forest_part1,
 * grove_part1, grass_part1, plains_part1 (+114 more shipped scenes).
 * Key changes: Gaussian-smoothed terrain, correct biome textures (top vs
 * front), depth-varied decorations for multi-plane parallax, TemplateName-
 * based torches (EmissionFactor=0.5), corrected light payload field 1042.
 *
 * Pipeline (all byte-exact protobuf, identical to scene_loader's encoding):
 *   1. A Blueprint describes the scene: ground platforms (polygons or rects),
 *      round-hat domes, water sheets, spawn, background texture, light.
 *   2. Heightfield synthesis + seeded noise turn a height profile into a ground
 *      polygon (the engine's RandomSeed/HorizNoise semantics, applied to the
 *      source polygon so the displacement is baked into the mesh geometry).
 *   3. Each platform is baked into a full GroundMesh object (GroundPolygon +
 *      GroundMesh + GroundMeshGenerator + CollisionShape + TextureMapping x2)
 *      reusing boulder (src/tools/boulder.cpp) — the C++ port of DanielSpaniel's
 *      Boulder engine, itself a port of Caver::GroundMeshGenerator.
 *   4. Background / DirectionalLight / SpawnPoint / Water objects are emitted
 *      with the canonical layouts observed in real scenes.
 *   5. The root Bounds field (field 3: four fixed32 floats X,Y,W,H — REQUIRED in
 *      118/118 scenes) is auto-computed from the ground AABB + padding.
 *   6. Everything is assembled into complete .scene bytes.
 *
 * Same seed + blueprint  =>  byte-identical scene (deterministic generation).
 *
 * ── Procedural terrain (v2) ────────────────────────────────────────────────
 *   generate_biome_scene() builds a full level from a Biome preset + options:
 *     • Perlin (gradient/value) noise + fBm octaves + ridged + domain warp
 *       synthesize a heightfield, sliced into walkable platforms (Minecraft-
 *       style connected blobs / islands).
 *     • Decoration PODs (trees/bushes/rocks/pots from the real asset list)
 *       are scattered with cluster noise — MODEL components only, never NPCs
 *       (the modder adds characters/entities by hand afterwards).
 *     • Torch + glow lights are spilled along the walkable edges: a real
 *       torch object (Model torch + SimpleGlow + Light Type 3 point light +
 *       FireEmitter, layouts from forest/grove scenes).
 *     • Biome tables (real scene analysis): ground/front/background textures,
 *       water colors, tree/rock sets, glow colors.
 */
#include <string>
#include <vector>
#include <cstdint>

namespace sgen {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

// A "round hat" dome on a platform's surface (Caver::InsertRoundHatVertices /
// InsertCapForRoundHat): a circular footprint dome rising `height` above the
// polygon's top edge. Game defaults observed in scenes: radius~40-80, height 20-40.
struct Hat {
    float x = 0.0f, y = 0.0f;
    float radius = 60.0f;
    float height = 40.0f;
};

// A fluid sheet (WaterMeshComponent). Colors match the game's defaults for water
// (florennum_cave1: front RGBA (0, .31, .23, .74), surface (0, .38, .26, .74)).
struct Water {
    float rect[4] = {0, 0, 0, 0};        // x, y, w, h (object-local sheet)
    std::string texture = "water";
    float front_rgba[4] = {0.0f, 0.314f, 0.233f, 0.744f};
    float surface_rgba[4] = {0.0f, 0.376f, 0.256f, 0.744f};
};

// One walkable ground platform (bakes to one GroundMesh scene object).
struct Platform {
    std::vector<Vec2> polygon;           // CCW closed polygon; if empty → rect
    float rect[4] = {0, 0, 0, 0};        // x, y, w, h (used when polygon empty)
    std::vector<Hat> hats;               // round-hat domes on the surface
    float min_depth = -45.0f;            // collision/geometry depth band (game default)
    float max_depth = 45.0f;
    std::string top_texture = "fire_grass";
    std::string front_texture = "graveyard_ground";
    uint32_t seed = 1291618994u;         // engine's editor default (matches 344 scene instances)
    float horiz_noise = 0.0f;            // deterministic vertex-jitter amplitude (baked)
    float surface_width = 80.0f;         // GMG component metadata (50-250 in scenes)
    float hat_height = 25.0f;            // GMG component metadata (20-40 in scenes)
    float z = 0.0f;                      // object Depth
};

// Parametric scene description consumed by generate_scene().
struct Blueprint {
    std::vector<Platform> platforms;
    std::vector<Water> waters;
    Vec2 spawn = {0, 0};                 // {0,0} → auto: first platform top edge + 56
    int spawn_facing = 1;                // 1 = facing right (game convention)
    std::string background = "grasslandsbackground_day";
    std::string light_name = "DirectionalLight";   // or DirectionalLight_day/_night
    float bounds_pad = 200.0f;           // padding around ground AABB for the root Bounds
    std::string scene_name = "new_scene";// used only for diagnostics
};

struct Result {
    std::string scene_bytes;             // complete .scene protobuf (objects + bounds)
    std::string error;
    float bounds[4] = {0, 0, 0, 0};      // X, Y, W, H actually emitted
    int objects = 0;
    bool ok() const { return error.empty() && !scene_bytes.empty(); }
};

// ── Main entry ────────────────────────────────────────────────────────────
// Generate a complete, vanilla-compatible .scene file from a blueprint.
Result generate_scene(const Blueprint& bp);

// ── Validation ────────────────────────────────────────────────────────────
// Re-checks a generated scene (or any blueprint) against the invariants that
// hold across all 118 shipped scenes + engine expectations:
//   1. blueprint has >= 1 platform
//   2. root Bounds cover the ground AABB (pad >= 0)
//   3. spawn is above the highest platform top edge (+56 clearance)
//   4. water is below the lowest platform bottom edge
//   5. at least one light/background object emitted
// Returns ok()==true only if every check passes; `messages` collects failures.
Result validate_scene(const Blueprint& bp, std::vector<std::string>* messages = nullptr);

// ── Deterministic RNG (splitmix64) ────────────────────────────────────────
uint64_t rng_next(uint64_t& state);
float rng_float(uint64_t& state, float lo, float hi);

// ── Terrain synthesis ─────────────────────────────────────────────────────
// Closed CCW polygon strip from a height profile:
//   heights[i] = top Y at x = i*step; floor_y = bottom of the strip.
// Produces rolling terrain (hills) walkable along the top edge.
std::vector<Vec2> make_heightfield_polygon(const float* heights, int n,
                                           float step, float floor_y);

// Rect platform polygon: (-w/2,-h/2) .. (w/2,h/2) around (x,y) center.
std::vector<Vec2> make_rect_polygon(float x, float y, float w, float h);

// Seed-displacement ("HorizNoise"): displace each vertex along its edge normal
// by deterministic per-vertex noise in [-amplitude, amplitude]. Seeded so the
// same seed always produces the same terrain. The displacement is BAKED into
// the source polygon and the emitted GroundMeshGeneratorComponent carries
// HorizNoise=0 — the engine regenerates geometry from the (already noisy)
// polygon, so editor preview and in-game mesh stay identical by construction.
// Jitter magnitude is clamped to 1/4 of the shortest adjacent edge to prevent
// self-intersection / winding flips on pathological amplitudes.
void apply_horiz_noise(std::vector<Vec2>& poly, float amplitude, uint32_t seed);

// ── Object builders (each returns scene-level bytes: root field 1 = Object) ─
std::string build_ground_object(const Platform& p, const std::string& name);
std::string build_background_object(const std::string& texture);
// DirectionalLight triple (Type 2/1/4 at IDs 101/103/105).
// depth: scene-level Z (real range 17–327). key_i/amb_i/shd_i: intensities.
// kr/kg/kb: key-light color (ambient is always white, shadow always black).
std::string build_light_object(const std::string& name, float x, float y,
                               float depth = 137.0f,
                               float key_i = 2.0f, float amb_i = 0.3f,
                               float shd_i = 0.4f,
                               float kr = 1.0f, float kg = 1.0f, float kb = 1.0f);
std::string build_spawn_object(const std::string& name, float x, float y,
                               int facing);
std::string build_water_object(const Water& w, const std::string& name);
// Root field 3 payload: 4 fixed32 floats X, Y, W, H (20 bytes).
std::string build_bounds_payload(float x, float y, float w, float h);

// Scene field 2 payload: ObjectLibrary. REQUIRED for TemplateName-reference
// objects (decorations/torches/props) to resolve and render in the real game.
// Layout: field 1 = Name (string), field 3 = ImportedLibrary (repeated string).
// Without it, ObjectLibrary::TemplateForName finds nothing in-game and every
// decoration becomes an invisible phantom (only the baked ground mesh shows).
std::string build_object_library(const std::string& scene_name,
                                 const std::vector<std::string>& imports);

// ============================================================================
// ── v2: Perlin-style noise (value-noise based, deterministic) ──────────────
// ============================================================================

// Deterministic Perlin-style value noise in 1D/2D. `perlin_1d(x, seed)` returns
// a smooth [-1,1] value; multi-octave fBm sums octaves with halving amplitude.
float perlin_1d(float x, uint32_t seed);
float perlin_2d(float x, float y, uint32_t seed);
// Fractal Brownian motion: octaves of perlin with lacunarity 2.0, gain 0.5.
// Returns [-1,1]. `warp` (domain warping) adds secondary-noise displacement to
// x/y for organic, non-repeating terrain silhouettes.
float fbm_1d(float x, int octaves, uint32_t seed, float warp = 0.0f);
float fbm_2d(float x, float y, int octaves, uint32_t seed, float warp = 0.0f);
// Ridged multifractal: abs(noise) inverted — sharp ridges, mountain crests.
float ridged_2d(float x, float y, int octaves, uint32_t seed);

// ============================================================================
// ── v2: Biome presets (real scene analysis) ────────────────────────────────
// ============================================================================

enum class Biome {
    Grasslands = 0,   // grass_part1 / plains_part1
    Forest,           // forest_part1 (dense grove trees)
    Grove,            // grove_part1 (torches + poles)
    Wasteland,        // wasteland_* (dead trees, sand)
    IceCastle,        // icecastle_* (snow, icicles)
    Cave,             // thecave / florennum_cave (cave walls, dark water)
    Fire,             // fire_part* (ember textures, lava-ish water)
    Florennum,        // florennum_* (town stone, warm lights)
    Count
};

// A biome's "identity": textures + decoration palette + light/water colors,
// all harvested from the real scene files (see docs/scenecreator/09-12 and
// the census tables in this file's implementation).
struct BiomeSpec {
    const char* name;                    // display name
    const char* background;              // BackgroundComponent texture
    const char* ground_top;              // top-surface texture
    const char* ground_front;            // front/cliff texture
    const char* deco_trees[6];           // tree/bush POD names ("" = end)
    const char* deco_rocks[4];           // rock/statue/pot POD names
    const char* deco_grass[4];           // small grass/tuft/bush POD names ("" = end)
    const char* torch_pod;               // torch model POD ("" = none)
    float glow_r, glow_g, glow_b;        // torch glow color
    float water_front[4];                // water front RGBA (or {0,0,0,0} = none)
    float water_surface[4];
    float key_light[4];                  // DirectionalLight key color
    float ambient[4];
    float tree_density;                  // 0..1 scatter probability per cell
    float rock_density;
    float grass_density;                 // 0..1 scatter probability for small tufts
    float torch_spacing;                 // world units between torches
};

// The biome table — indexed by Biome::*.
const BiomeSpec& biome_spec(Biome b);
const char* biome_name(Biome b);

// A single decoration: Model object (POD) placed on the terrain.
struct Deco {
    float x = 0.0f, y = 0.0f;
    float z = 0.0f;                      // depth layer
    float scale = 1.0f;
    float rot_y = 0.0f;                  // radians
    // In REAL scenes (verified via ruby_cli decode of forest/grove/grass_part1)
    // trees, bushes, grass, rocks and pots are pure TemplateName-reference
    // objects — the game resolves the mesh + components from the template
    // library. `pod` here carries that TemplateName (e.g. "grove_tree1",
    // "bush", "pot"). template_name overrides it when set explicitly.
    std::string pod;                     // TemplateName / POD stem, e.g. "grove_tree1"
    std::string template_name;           // explicit TemplateName override (optional)
    std::string name;                    // object identifier
};

// Emit a pure TemplateName-reference object (no baked Model component). This is
// exactly how the shipped scenes store decorations: TemplateName (field 1) +
// Identifier (2) + Position (4) + Depth (5) + Rotation (6) + Scaling (7) +
// LocalAabb (8) + Hidden (9). The game instantiates everything else.
std::string build_template_object(const std::string& name, const std::string& template_name,
                                  float x, float y, float depth,
                                  float rotation, float scaling);

// A torch/point light object spilled on the walkable edges.
struct TorchLight {
    float x = 0.0f, y = 0.0f;
    float z = 0.0f;
    float radius = 350.0f;               // LightComponent Radius (observed 350)
    float intensity = 2.0f;              // observed torch intensity
    float glow_r = 0.389f, glow_g = 0.111f, glow_b = 0.111f, glow_a = 0.5f;
};

// ── v2 procedural terrain options ─────────────────────────────────────────
struct TerrainOptions {
    Biome biome = Biome::Grasslands;
    uint32_t seed = 1337;
    // World scale of the generated level (width in world units).
    float width = 4200.0f;
    // Vertical height range of the terrain.
    float height = 900.0f;
    int   platform_count = 6;            // how many walkable platform strips
    int   octaves = 4;                   // fBm octaves
    float roughness = 1.0f;              // noise amplitude multiplier
    bool  add_water = true;              // emit a WaterMesh at the valley floor
    bool  spill_torches = true;          // torch+glow+light objects along edges
    float torch_spacing = 240.0f;        // override of the biome default (0 = biome)
    float deco_density = 1.0f;           // 0..1 multiplier for trees/rocks
    bool  mountains = false;             // use ridged multifractal profile
    bool  islands = false;               // disconnected island blobs (hats)
    std::string scene_name = "procedural";
};

// Build a full procedural level as a complete .scene (root object bytes +
// bounds). Combines: heightfield terrain → platforms, decorations, torches,
// water, background, DirectionalLight triple, spawn, auto-bounds.
// `objects` is the byte-blob of the whole scene; use av::scene_load() to parse.
Result generate_biome_scene(const TerrainOptions& opt);

// Standalone builders used by generate_biome_scene (also reusable by the UI):
// A torch object: Model(torch POD) + SimpleGlow + Light(Type 3 point) +
// FireEmitter — the exact composite from forest/grove scenes.
std::string build_torch_object(const TorchLight& t, const std::string& name);
// A pure glow+light object (no torch model): Light Type 3 + SimpleGlow.
std::string build_glow_light(const TorchLight& t, const std::string& name);
// A decoration Model object (POD reference). Name field 1 (Ruby form) and
// YRotation field 2 (game form) — same layout build_pod_object produces.
std::string build_deco_object(const Deco& d, const std::string& name);

} // namespace sgen
