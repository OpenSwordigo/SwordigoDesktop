/*
 * scene_generator_v2_3d.cpp — Swordigo "v2-3d" full-3D Minecraft-style world.
 *
 * RESEARCH NOTES (what this generator is built on)
 * ─────────────────────────────────────────────
 * Engine projection (OpenSwordigo/bitcvh_fking_src graphics/rendering_context.cpp):
 *   true perspective, FOV 20°, camera on +Z at 2835.6/zoom aimed at the z=0
 *   gameplay plane → apparent size ∝ 2835.6/(2835.6 − z). Depth is literal
 *   world Z; occlusion via the depth buffer. The GroundMesh top surface is a
 *   real horizontal quad extruded across [min_depth, max_depth] (boulder.cpp
 *   get_top_vertices), so a row's band IS its visible top thickness, and
 *   abutting bands fuse rows into one continuous terrain volume.
 *
 * Vanilla measurements (ruby_cli decode of forest/grove/grass_part1, fire_part31):
 *   gameplay ground bands   ±70..±100 (median), SurfaceWidth 130–190
 *   far background ground   z −1958, band ±25 (grove obj5#8)
 *   walkable top flatness   median Y-variation 28–44 per ground strip
 *   decoration density      2.5–3.7 objects / 1000 world units (all classes)
 *
 * THE Z-GRID ("recreate the X algorithm in Z")
 *   X axis: v1 slices strips that SHARE boundary samples → seamless X runs.
 *   Z axis: rows centred at z = ±2k·B each with band ±B span [(2k−1)B,(2k+1)B]
 *   → adjacent rows SHARE their boundary depth plane: no gap, no overlap. With
 *   block-quantized heights, a height change between neighbouring rows is a
 *   clean vertical voxel terrace — the world reads as one solid Minecraft
 *   chunk cross-section extruded across the full Z extent, both sides of the
 *   hero plane (z<0 depth rows AND z>0 front rows).
 *
 *   Front rows are the excavated face of the world toward the camera: tops are
 *   perspective-capped to descend below the gameplay floor line on screen
 *   (world cap = (gp_min_top − 2·block)/scale(z)), so the hero stays visible
 *   while the foreground terraces slide past — classic side-scroller framing.
 *
 * CAMERA: v1 semantics (v2's camera_follow_y_shape is buggy) — NO camera
 * shapes; root Bounds = plain AABB of walkable geometry + fixed padding.
 *
 * All protobuf encoding goes through the public sgen:: builders from
 * scene_generator.h; static helpers not exposed there are re-implemented
 * locally (local_* prefix), same approach as scene_generator_v2.cpp.
 */

#include "tools/scene_generator_v2_3d.h"
#include "tools/scene_generator.h"
#include "tools/boulder.h"
#include "platform/protobuf_reader.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <sstream>

namespace sgen {
namespace v2_3d {

// Game camera distance from the gameplay plane (viewOffset_.z in
// camera_controller.h / setCamera camZ = 2835.6/zoom). Drives all
// perspective compensation in this file.
static constexpr float kCamZ = 2835.6f;

// Apparent-size factor of something at depth z (1.0 at the gameplay plane).
static inline float persp_scale(float z) {
    return kCamZ / (kCamZ - z);
}

// ─────────────────────────────────────────────────────────────────────────────
// Local static helpers (not exposed by scene_generator.h)
// ─────────────────────────────────────────────────────────────────────────────

static std::string local_extract_object_bytes(const std::string& scene) {
    if (scene.empty() || (unsigned char)scene[0] != 0x0Au) return "";
    size_t i = 1;
    uint64_t len = 0; int sh = 0;
    while (i < scene.size() && ((unsigned char)scene[i] & 0x80)) {
        len |= (uint64_t)(scene[i] & 0x7fu) << sh; sh += 7; ++i;
    }
    if (i >= scene.size()) return "";
    len |= (uint64_t)(unsigned char)scene[i] << sh; ++i;
    if (i + len > scene.size()) return "";
    return scene.substr(i, (size_t)len);
}

static void local_track_aabb(float& minx, float& miny, float& maxx, float& maxy,
                             const std::vector<Vec2>& poly) {
    for (const auto& v : poly) {
        minx = std::min(minx, v.x); miny = std::min(miny, v.y);
        maxx = std::max(maxx, v.x); maxy = std::max(maxy, v.y);
    }
}

static inline uint32_t local_hash_uint(uint32_t x) {
    x = (x ^ 61u) ^ (x >> 16);
    x = x + (x << 3);
    x = x ^ (x >> 4);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15);
    return x;
}

// ── Local protobuf micro-helpers (copies of v1 statics) ──────────────────────

// 1D Gaussian blur — same algorithm as sgen::smooth_gaussian (static in
// scene_generator.cpp, re-implemented locally like v2 did). The core of the
// SMOOTH world style: rolling low elevations with no per-column jitter.
static void local_smooth_gaussian(std::vector<float>& h, int radius = 5, int passes = 3) {
    const int n = (int)h.size();
    if (n == 0) return;
    std::vector<float> tmp(n);
    for (int pass = 0; pass < passes; ++pass) {
        const float sigma = radius * 0.45f;
        float wsum = 0.0f;
        std::vector<float> w(2 * radius + 1);
        for (int k = -radius; k <= radius; ++k) {
            w[k + radius] = std::exp(-0.5f * (k * k) / (sigma * sigma));
            wsum += w[k + radius];
        }
        for (auto& ww : w) ww /= wsum;
        for (int i = 0; i < n; ++i) {
            float v = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int j = std::clamp(i + k, 0, n - 1);
                v += w[k + radius] * h[(size_t)j];
            }
            tmp[(size_t)i] = v;
        }
        h = tmp;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-edge sampler for decoration scatter (v1/v2 semantics)
// ─────────────────────────────────────────────────────────────────────────────

struct TopEdge { std::vector<Vec2> pts; float min_x = 0, max_x = 0; };

static TopEdge build_top_edge(const Platform& plat) {
    std::vector<Vec2> poly = plat.polygon;
    if (poly.size() < 3)
        poly = make_rect_polygon(plat.rect[0], plat.rect[1], plat.rect[2], plat.rect[3]);
    const float bucket = 24.0f;
    float mn = 1e9f, mx = -1e9f;
    for (const auto& v : poly) { mn = std::min(mn, v.x); mx = std::max(mx, v.x); }
    const int nb = std::max(1, (int)((mx - mn) / bucket));
    std::vector<float> top(nb, -1e9f);
    for (const auto& v : poly) {
        const int bk = std::clamp((int)((v.x - mn) / bucket), 0, nb - 1);
        top[bk] = std::max(top[bk], v.y);
    }
    TopEdge te; te.min_x = mn; te.max_x = mx;
    for (int b = 0; b < nb; ++b)
        if (top[b] > -1e8f) te.pts.push_back({mn + (b + 0.5f) * bucket, top[b]});
    return te;
}

static float ground_y_at(const TopEdge& te, float x) {
    if (te.pts.empty()) return -1e9f;
    if (x <= te.pts.front().x) return te.pts.front().y;
    if (x >= te.pts.back().x)  return te.pts.back().y;
    for (size_t k = 1; k < te.pts.size(); ++k) {
        if (x <= te.pts[k].x) {
            const float t = (x - te.pts[k-1].x) / std::max(1e-3f, te.pts[k].x - te.pts[k-1].x);
            return te.pts[k-1].y + t * (te.pts[k].y - te.pts[k-1].y);
        }
    }
    return te.pts.back().y;
}

// ─────────────────────────────────────────────────────────────────────────────
// Vanilla-confirmed TOP textures (ruby_cli decode of grass/grove/forest_part1).
// Every row's walk surface gets GRASS — Minecraft-style grass-on-top with the
// cliff/dirt texture only on faces. Vanilla mixes a subtle variant on some
// grounds (grass_part1: 10× grass_grass, 1× grass_subtle, 2× grass_road;
// grove even keeps forest_grass on the z=−1958 far ground). nullptr = use the
// biome table's ground_top only.
// ─────────────────────────────────────────────────────────────────────────────

// Per-biome top-surface material set: {lush, dry/subtle, high/sparse}. These
// are real vanilla textures (decoded from shipped scenes). The three slots let
// the CONTINUOUS environmental fields (moisture / altitude / temperature)
// choose a material that blends across space instead of hard-switching on
// region boundaries. A nullptr slot falls back to the biome's ground_top so
// biomes without a variant simply stay uniform (no crash, no fake strip).
struct TopSet { const char* lush; const char* dry; const char* high; };
static const TopSet kTopSets[(int)Biome::Count] = {
    /* Grasslands */ {"grass_grass",   "grass_subtle",  "grass_yellow"},
    /* Forest */     {"forest_grass",  "grass_subtle",  "grass_yellow"},
    /* Grove */      {"grove_grass",   "forest_grass",  "grass_yellow"},
    /* Wasteland */  {nullptr,         nullptr,         nullptr},
    /* IceCastle */  {nullptr,         nullptr,         "snowy_snow"},
    /* Cave */       {nullptr,         nullptr,         nullptr},
    /* Fire */       {"fire_grass",    nullptr,         nullptr},
    /* Florennum */  {nullptr,         nullptr,         nullptr},
};

// Continuous, field-driven top material. `moist`, `alt`, `temp` are 0..1
// samples from the WorldField at the strip's world centre. Selection uses
// smooth thresholds with a tiny hash dither at the crossover so the boundary
// between two materials is organic/irregular (soft sub-biome edge) rather than
// a straight painted line — but NEVER a per-strip random flicker.
static const char* top_texture_for(Biome b, const BiomeSpec& bio,
                                   uint32_t seed, float wx, float wz,
                                   float moist, float alt, float temp) {
    const int idx = std::clamp((int)b, 0, (int)Biome::Count - 1);
    const TopSet& ts = kTopSets[idx];
    const char* lush = ts.lush ? ts.lush : bio.ground_top;
    const char* dry  = ts.dry  ? ts.dry  : lush;
    const char* high = ts.high ? ts.high : lush;

    // Small spatial dither (±0.06) breaks the crossover into an organic edge.
    const uint32_t hh = local_hash_uint(seed
        ^ (uint32_t)(int)(wx * 0.5f) ^ ((uint32_t)(int)(wz * 0.5f) << 16));
    const float dither = ((hh & 0xFFFFu) / 65535.0f - 0.5f) * 0.12f;

    // High/cold ground (mountain tops, cold regions) → sparse/snow material.
    if (alt + dither > 0.74f || temp + dither < 0.24f) return high;
    // Dry regions (low moisture, warm) → subtle/dry material.
    if (moist + dither < 0.42f) return dry;
    // Otherwise the lush default.
    return lush;
}

// Continuous side/cliff material: steep or high faces expose ROCK; gentle,
// lower faces show SOIL/dirt. Driven by real terrain slope + altitude so the
// exposed sides vary with the landform instead of being one repeated wall.
// Falls back to the biome ground_front when no rock/soil pair is defined.
struct SideSet { const char* soil; const char* rock; };
static const SideSet kSideSets[(int)Biome::Count] = {
    /* Grasslands */ {"maybegood",        "graveyard_ground"},
    /* Forest */     {"forest_ground",    "graveyard_ground"},
    /* Grove */      {"grove_ground",     "graveyard_ground"},
    /* Wasteland */  {"wasteland_ground", "wasteland_ground2"},
    /* IceCastle */  {"icecastle_ground", "atlon_ground"},
    /* Cave */       {"lowercave_ground1","lowercave_ground2"},
    /* Fire */       {"graveyard_ground", "wasteland_ground2"},
    /* Florennum */  {"florennum_ground", "graveyard_ground"},
};

static const char* front_texture_for(Biome b, const BiomeSpec& bio,
                                     float slope, float alt) {
    const int idx = std::clamp((int)b, 0, (int)Biome::Count - 1);
    const SideSet& ss = kSideSets[idx];
    const char* soil = ss.soil ? ss.soil : bio.ground_front;
    const char* rock = ss.rock ? ss.rock : bio.ground_front;
    // Rock exposure grows with slope and altitude (rock ~ steep/high geology).
    const float rock_factor = std::clamp(slope * 2.2f + alt * 0.5f - 0.35f, 0.0f, 1.0f);
    return (rock_factor > 0.5f) ? rock : soil;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blocky (Minecraft) polygon: vertical step faces at every height change.
// Walking the top right→left, after (x_i, h_i) insert (x_i, h_{i-1}) whenever
// the neighbour column differs — the wall lives on the column boundary x_i.
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<Vec2> make_blocky_polygon(const float* h, int n, float step,
                                             float floor_y) {
    std::vector<Vec2> poly;
    if (n < 2) return poly;
    const float right_x = (float)(n - 1) * step;
    poly.push_back({0.0f, floor_y});
    poly.push_back({right_x, floor_y});
    for (int i = n - 1; i >= 0; --i) {
        poly.push_back({(float)i * step, h[i]});
        if (i > 0 && h[i - 1] != h[i])
            poly.push_back({(float)i * step, h[i - 1]});
    }
    return poly;
}

// ─────────────────────────────────────────────────────────────────────────────
// Plateau synthesis — the vanilla-flatness transform.
// Heights arrive pre-quantized to block levels. A hysteresis debounce keeps
// the current level until a DIFFERENT level persists `min_run` consecutive
// columns, producing long flat plateaus joined by clean vertical cliffs —
// no per-column jitter (vanilla median top variation is 28–44 units).
// ─────────────────────────────────────────────────────────────────────────────

static void plateau_hysteresis(std::vector<float>& h, int min_run) {
    const int n = (int)h.size();
    if (n < 2 || min_run <= 1) return;
    float cur = h[0], cand = h[0];
    int cand_run = 0;
    for (int i = 0; i < n; ++i) {
        if (h[i] != cand) { cand = h[i]; cand_run = 1; }
        else ++cand_run;
        if (cand != cur && cand_run >= min_run) cur = cand;
        h[i] = cur;
    }
}

// Cap adjacent-column level jumps at `cap` (walkability). Applied outward
// from the spawn pad in both directions so the pad itself stays untouched;
// taller raw cliffs become ≤cap-per-column staircases (switchbacks).
static void cap_steps_bidirectional(std::vector<float>& h, int c0, float cap) {
    const int n = (int)h.size();
    for (int i = c0 + 1; i < n; ++i)
        h[(size_t)i] = std::clamp(h[(size_t)i], h[(size_t)i - 1] - cap, h[(size_t)i - 1] + cap);
    for (int i = c0 - 1; i >= 0; --i)
        h[(size_t)i] = std::clamp(h[(size_t)i], h[(size_t)i + 1] - cap, h[(size_t)i + 1] + cap);
}

// ─────────────────────────────────────────────────────────────────────────────
// The world: one continuous 2D heightfield + cave/island fields over (x, z)
// ─────────────────────────────────────────────────────────────────────────────

struct WorldField {
    uint32_t seed  = 1;
    float    H     = 900.0f;   // vertical world budget (opt.height)
    int      octaves   = 4;

    float cont_amp = 0.0f, plains_amp = 0.0f, ridge_amp = 0.0f;

    void init(const TerrainOptions3D& opt) {
        seed     = opt.seed ? opt.seed : 1u;
        H        = std::max(200.0f, opt.height);
        octaves  = std::clamp(opt.octaves, 1, 8);
        // SMOOTH LOW WORLD: gentle low-level elevations and depressions
        // ("Minecraft" is the style/vibe, not blocky geometry). Amplitudes
        // are fractions of the height budget; mountains mode still rolls
        // visibly higher ridges without ever becoming jagged.
        cont_amp   = H * 0.10f;
        plains_amp = H * 0.07f;
        ridge_amp  = H * (opt.mountains ? 0.24f : 0.10f);
    }

    // Full Minecraft-style multi-noise stack. Returns world-Y of the surface.
    float height_at(float x, float z) const {
        // Domain warp — one shared low-frequency noise displaces both axes so
        // coastlines / ridges never look axis-aligned.
        const float w  = fbm_2d(x * 0.00052f, z * 0.00052f, 2, seed ^ 0xA17A5u);
        const float px = x + w * 260.0f;
        const float pz = z + w * 190.0f;

        // fBm rarely reaches ±1 (octave sum concentrates near ±0.55), so each
        // layer is stretched back to a full ±1 before shaping — otherwise the
        // whole world compresses into a bland band around Y=0.
        const float cont  = std::clamp(fbm_2d(px * 0.00042f, pz * 0.00042f, 3,
                                             seed ^ 0xC0EE1u, 0.30f) * 1.7f, -1.0f, 1.0f);
        const float ero   = std::clamp(fbm_2d(px * 0.00055f, pz * 0.00055f, 2,
                                             seed ^ 0xE90Fu, 0.25f) * 1.7f, -1.0f, 1.0f);
        const float hills = std::clamp(fbm_2d(px * 0.0018f,  pz * 0.0018f,  octaves,
                                             seed, 0.30f) * 1.8f, -1.0f, 1.0f);
        const float ridge = ridged_2d(px * 0.0011f, pz * 0.0011f,
                                      octaves, seed ^ 0x91D1u);       // 0..1

        // Mountain mask: high continentalness + low erosion → peaks.
        float mm = std::clamp(0.5f + (cont * 0.55f - ero * 0.45f), 0.0f, 1.0f);
        mm = mm * mm * (3.0f - 2.0f * mm);                             // smoothstep

        const float land = cont * 0.5f + 0.5f;                         // 0..1
        return (land - 0.50f) * cont_amp                               // base/ocean
             + hills * plains_amp * (0.35f + 0.65f * (1.0f - mm))      // plains
             + ridge * ridge_amp * mm;                                 // mountains
    }

    // Cave/ravine field: carve where |cave| < width — the zero-set of fBm
    // winds through the world like a ravine/tunnel map.
    float cave_at(float x, float z) const {
        return fbm_2d(x * 0.0016f, z * 0.0024f, 2, seed ^ 0xCA9Eu, 0.40f);
    }

    // Sky-island density field.
    float island_at(float x, float z) const {
        return fbm_2d(x * 0.0009f, z * 0.0013f, 3, seed ^ 0x1517u, 0.30f);
    }

    // ── Continuous environmental fields (drive materials + ecology) ──────────
    // Low-frequency, independent noise channels evaluated in world space. They
    // vary SMOOTHLY and organically (no axis-aligned strips), so material and
    // decoration choices that read them blend progressively across the world
    // instead of switching on region/row boundaries.

    // Moisture 0..1 — wet valleys vs dry ridges. Large scale so a whole region
    // trends wet or dry; combined with altitude below for the final biome mix.
    float moisture_at(float x, float z) const {
        return std::clamp(fbm_2d(x * 0.00036f, z * 0.00036f, 3,
                                 seed ^ 0x3057u, 0.25f) * 0.85f + 0.5f, 0.0f, 1.0f);
    }

    // Temperature 0..1 — a very-low-frequency gradient plus gentle noise. Warm
    // lowlands, cooler highs (altitude term applied by the caller).
    float temperature_at(float x, float z) const {
        return std::clamp(fbm_2d(x * 0.00028f, z * 0.00030f, 2,
                                 seed ^ 0x7E11u, 0.20f) * 0.80f + 0.5f, 0.0f, 1.0f);
    }

    // Altitude 0..1 — the surface height normalized into the world budget,
    // so "how high are we" can influence material/vegetation continuously.
    float altitude01(float x, float z) const {
        const float y = height_at(x, z);
        return std::clamp(0.5f + y / (H * 0.5f), 0.0f, 1.0f);
    }

    // Local slope magnitude (finite-difference of the height field, world
    // units of rise per ~unit run). Steeper → more rock/soil exposure, fewer
    // trees. Sampled with a small epsilon so it reflects real geometry.
    float slope_at(float x, float z) const {
        const float e = 26.0f;
        const float dx = height_at(x + e, z) - height_at(x - e, z);
        const float dz = height_at(x, z + e) - height_at(x, z - e);
        return std::sqrt(dx * dx + dz * dz) / (2.0f * e);   // ~tan(slope)
    }

    // Small-scale vegetation-density field 0..1 — clumps vegetation into
    // clusters with natural gaps rather than uniform scatter.
    float veg_density_at(float x, float z) const {
        return std::clamp(fbm_2d(x * 0.0011f, z * 0.0011f, 3,
                                 seed ^ 0x5EEDu, 0.30f) * 0.9f + 0.5f, 0.0f, 1.0f);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Main v2-3d generator
// ─────────────────────────────────────────────────────────────────────────────

Result generate_biome_scene_v2_3d(const TerrainOptions3D& opt) {
    Result r;
    const BiomeSpec& bio = biome_spec(opt.biome);
    const uint32_t seed  = opt.seed ? opt.seed : 1u;

    // ── World constants ───────────────────────────────────────────────────────
    WorldField wf;
    wf.init(opt);

    const float block     = std::clamp(opt.block_size, 16.0f, 120.0f);
    // Lowland reference line (no water objects anymore — sheets between the
    // hero and camera obscured the view). Still used to keep the gameplay
    // floor and ravine floors from digging too deep.
    const float land_min  = -std::clamp(opt.height, 200.0f, 6000.0f) * 0.18f;
    const float floor_y   = -std::clamp(opt.height, 200.0f, 6000.0f) * 0.60f;
    const float cliff_cap = (float)std::max(1, opt.max_cliff_blocks) * block;
    const float pad_half  = opt.width * 0.12f;   // spawn plateau half-width

    auto quantize = [&](float h) -> float {
        return opt.blocky ? std::floor(h / block) * block : h;
    };

    // ── The Z grid: X-seam system recreated in depth ──────────────────────────
    // Row k is centred at z = ±2k·B with band ±B → spans [(2k−1)B, (2k+1)B]:
    // adjacent rows SHARE their boundary depth plane (the Z seam), exactly
    // like X strips share boundary samples. The hero plane (z=0) only ever
    // intersects the gameplay row's band.
    const float band = std::clamp(opt.row_band, 30.0f, 140.0f);
    const int   n_back  = std::clamp(opt.depth_rows, 0, 24);
    const int   n_front = std::clamp(opt.front_rows, 0, 12);

    struct RowDesc {
        int   k;              // 0 = gameplay, >0 = depth row, <0 = front row
        float z;              // row centre depth
        float width;          // world width (×1/persp so the row covers screen)
        bool  walkable;
        bool  front;          // z > 0 (excavated face toward the camera)
    };
    auto make_row = [&](int k) -> RowDesc {
        const float z = (float)(2 * k) * band;   // sign of k picks the side
        return {k, z, opt.width / persp_scale(z) * 1.06f, k == 0, false};
    };

    // Emission order: farthest back → gameplay → farthest front.
    std::vector<RowDesc> rows;
    for (int k = n_back; k >= 1; --k) rows.push_back(make_row(-k));
    rows.push_back(make_row(0));
    for (int j = 1; j <= n_front; ++j) {
        RowDesc rd = make_row(j);
        rd.front = true;
        rows.push_back(rd);
    }

    // ── Scene assembly state ──────────────────────────────────────────────────
    proto::Writer scene;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    uint64_t drng = (uint64_t)seed ^ 0x3D2D3D2Dull;
    r.objects = 0;

    std::vector<Platform> walkable_plats;
    float spawn_pad_y = 0.0f;                        // surface level at x=0
    struct CaveGlow { float x, y; };
    std::vector<CaveGlow> cave_glows;

    // ── Row height synthesis (one continuous world, per-row transforms) ──────
    // Gameplay row first (pre-pass): the front rows' perspective caps need the
    // gameplay floor, and the spawn pad / ravines only exist on z = 0.
    float gp_min_top_cache = 0.0f;
    auto synthesize_heights = [&](const RowDesc& row) -> std::vector<float> {
        const int cols = std::max(24, (int)(row.width / block) + 1);
        const float x0 = -row.width * 0.5f;
        // CONTINUOUS LANDFORM: every row — depth, gameplay AND front — samples
        // the SAME heightfield H(x,z) at its true world z with NO per-row
        // amplitude scaling or vertical bias. Front rows are simply the world
        // continuing toward the camera, so the surface flows smoothly across
        // depth instead of reading as offset "excavated face" slabs/terraces.
        std::vector<float> h((size_t)cols);
        for (int i = 0; i < cols; ++i)
            h[(size_t)i] = wf.height_at(x0 + (float)i * block, row.z);

        if (opt.blocky) {
            // Voxel style (opt-in): snap to the block grid and debounce into
            // plateaus with vertical cliff faces.
            for (auto& v : h) v = quantize(v);
            plateau_hysteresis(h, std::max(1, opt.min_run_cols - (row.walkable ? 0 : 1)));
        } else {
            // SMOOTH WORLD (default): gentle rolling low elevations and
            // depressions — Gaussian-smoothed, no per-column jitter.
            local_smooth_gaussian(h, 5,
                                  std::max(1, (int)std::lround(3.0f * opt.smoothness)));
        }

        if (row.walkable) {
            if (opt.blocky) {
                // 1. Spawn pad: flatten the centre band to one land level.
                float pad_level = -1e9f;
                for (int i = 0; i < cols; ++i)
                    if (std::fabs(x0 + (float)i * block) <= pad_half)
                        pad_level = std::max(pad_level, h[(size_t)i]);
                pad_level = std::max(quantize(std::max(pad_level, land_min + block)), land_min);
                spawn_pad_y = pad_level;
                for (int i = 0; i < cols; ++i)
                    if (std::fabs(x0 + (float)i * block) < pad_half)
                        h[(size_t)i] = pad_level;

                // 2. Traversability: cap level jumps outward from the pad;
                //    taller raw cliffs become ≤cliff_cap staircases.
                cap_steps_bidirectional(h, cols / 2, cliff_cap);
                for (auto& v : h) v = std::max(v, land_min);
                for (int i = 0; i < cols; ++i)
                    if (std::fabs(x0 + (float)i * block) < pad_half)
                        h[(size_t)i] = pad_level;
            } else {
                // Smooth spawn pad: flat core, cosine-blended into the rolling
                // terrain (no hard step at the pad edge).
                float pad_sum = 0.0f; int pad_n = 0;
                for (int i = 0; i < cols; ++i)
                    if (std::fabs(x0 + (float)i * block) <= pad_half) {
                        pad_sum += h[(size_t)i]; ++pad_n;
                    }
                const float pad_level = std::max(pad_n ? pad_sum / (float)pad_n : 0.0f,
                                                 land_min);
                spawn_pad_y = pad_level;
                // Gentle walkability: cap the per-column slope (≤45°) outward
                // from the pad, then one soft pass rounds any clamp kinks.
                cap_steps_bidirectional(h, cols / 2, block);
                local_smooth_gaussian(h, 3, 1);
                for (int i = 0; i < cols; ++i) {
                    const float ax = std::fabs(x0 + (float)i * block);
                    const float t  = std::clamp((ax - pad_half * 0.55f) / (pad_half * 0.45f),
                                                0.0f, 1.0f);
                    const float wgt = t * t * (3.0f - 2.0f * t);
                    h[(size_t)i] = std::max(pad_level + (h[(size_t)i] - pad_level) * wgt,
                                            land_min);
                }
            }

            // 3. Ravines: carve winding canyons where |cave| crosses zero.
            //    Floors clamp above the lowland line; never touch the spawn pad.
            if (opt.add_caves) {
                int run = 0;
                for (int i = 0; i <= cols; ++i) {
                    const bool carved = i < cols
                        && std::fabs(wf.cave_at(x0 + (float)i * block, row.z))
                           < opt.cave_width
                        && std::fabs(x0 + (float)i * block) > pad_half;
                    if (carved) { ++run; continue; }
                    if (run > 0) {
                        float floorv = 1e9f;
                        for (int j2 = i - run; j2 < i; ++j2)
                            floorv = std::min(floorv, h[(size_t)j2]);
                        const float drop = opt.blocky
                            ? quantize(floorv - opt.cave_depth)
                            : (floorv - opt.cave_depth);
                        const float target = std::max(drop, land_min);
                        if (target < floorv)
                            for (int j2 = i - run; j2 < i; ++j2)
                                h[(size_t)j2] = target;
                        if (run >= 4)
                            cave_glows.push_back({x0 + (float)(i - run / 2) * block,
                                                  target + 8.0f});
                        run = 0;
                    }
                }
                // Feather the carved walls into the rolling terrain (the
                // smooth world has no raw vertical cuts).
                if (!opt.blocky) local_smooth_gaussian(h, 4, 2);
            }
        } else if (row.front) {
            // Front rows are the SAME landform continuing toward the camera —
            // NOT an offset/squashed excavated face. To keep the hero visible
            // we only relieve the peaks that would actually rise above the
            // gameplay skyline on screen, and we do it with a gradual
            // compression (not a hard clamp to a flat ceiling line) so the
            // surface still reads as one continuous slope flowing forward.
            // The relief also fades in with proximity (nearer rows occlude
            // more), so distant front rows stay fully continuous.
            const float gp_min = gp_min_top_cache;
            const float ceil    = (gp_min - 2.0f * block) / persp_scale(row.z);
            const float soft    = std::max(block * 2.0f, wf.H * 0.10f); // rolloff width
            for (auto& v : h) {
                if (v > ceil) {
                    // tanh-style soft knee: peaks asymptotically approach the
                    // ceiling instead of snapping flat — no visible ledge line.
                    const float over = (v - ceil) / soft;
                    v = ceil + soft * (over / (1.0f + over));
                }
            }
            if (!opt.blocky) local_smooth_gaussian(h, 3, 1);
        } else if (opt.add_caves) {
            // Depth rows get the same world's ravines (visual, no land clamp).
            for (int i = 0; i < cols; ++i) {
                if (std::fabs(wf.cave_at(x0 + (float)i * block, row.z)) >= opt.cave_width)
                    continue;
                h[(size_t)i] -= opt.cave_depth * 0.6f;
            }
        }
        return h;
    };

    // Pre-pass: gameplay row (records spawn pad + cave glows, feeds front caps).
    const RowDesc gp_row = make_row(0);
    const std::vector<float> gp_h = synthesize_heights(gp_row);
    gp_min_top_cache = *std::min_element(gp_h.begin(), gp_h.end());
    // (front rows read gp_min_top_cache inside synthesize_heights)

    // Background + DirectionalLight (v1 wire format and depth logic).
    scene.write_bytes_field(1, local_extract_object_bytes(
        build_background_object(bio.background)));
    ++r.objects;
    const float light_depth = 17.0f + opt.width * 0.018f;
    // Lighting tuned for DEPTH: a slightly lower key + lower ambient + higher
    // shadow term widens the light/shadow spread so terrain slopes read their
    // orientation to the sun (relief) instead of everything being uniformly
    // bright/flat. Slopes facing the key now visibly separate from those in
    // shade, which — with the softened, less-saturated materials — makes the
    // landform's shape legible rather than a flat neon sheet.
    scene.write_bytes_field(1, local_extract_object_bytes(
        build_light_object("DirectionalLight", 0.0f, 0.0f, light_depth,
                           bio.key_light[0] > 0.0f ? std::clamp(bio.key_light[0] * 2.8f, 2.2f, 3.0f) : 2.6f,
                           bio.ambient[0]   > 0.0f ? bio.ambient[0] * 0.60f : 0.22f,
                           0.6f,
                           bio.key_light[0], bio.key_light[1], bio.key_light[2])));
    ++r.objects;

    // ── Per-row terrain emission ──────────────────────────────────────────────
    for (const auto& row : rows) {
        const std::vector<float> h = row.walkable ? gp_h : synthesize_heights(row);
        const int cols = (int)h.size();
        const float x0 = -row.width * 0.5f;

        // ── Slice the row into strips (v1 seamless boundary sharing in X) ────
        const int n_strips = row.walkable ? std::max(2, opt.platform_count)
                                          : std::max(1, opt.platform_count / 2);
        std::vector<Platform> row_plats;

        for (int p = 0; p < n_strips; ++p) {
            const int s0 = std::clamp((int)std::llround((double)p       * (cols - 1) / n_strips), 0, cols - 1);
            const int s1 = std::clamp((int)std::llround((double)(p + 1) * (cols - 1) / n_strips), 0, cols - 1);
            if (s1 <= s0) continue;
            const int cnt = s1 - s0 + 1;

            Platform plat;
            if (opt.blocky)
                plat.polygon = make_blocky_polygon(&h[(size_t)s0], cnt, block,
                                                   floor_y - wf.H * 0.05f);
            else
                plat.polygon = make_heightfield_polygon(&h[(size_t)s0], cnt, block,
                                                        floor_y - wf.H * 0.05f);
            for (auto& v : plat.polygon) v.x += x0 + (float)s0 * block;

            plat.seed          = (uint32_t)(seed ^ ((uint32_t)p * 0x9E37u)
                                          ^ (uint32_t)(int)row.z ^ 0x5D11u);
            plat.horiz_noise   = 0.0f;            // voxel purity: no jitter
            // ── Field-driven materials (continuous, no painted strips) ───────
            // Sample the environmental fields at this strip's WORLD centre, so
            // the surface + cliff textures track moisture/altitude/temperature/
            // slope smoothly across the whole world instead of switching on
            // per-strip/per-row hashes. Adjacent strips read almost-identical
            // field values → material transitions are gradual and organic.
            const float cx_w = x0 + (float)(s0 + s1) * 0.5f * block;
            const float moist = wf.moisture_at(cx_w, row.z);
            const float alt   = wf.altitude01(cx_w, row.z);
            const float temp  = std::clamp(wf.temperature_at(cx_w, row.z) - alt * 0.35f,
                                           0.0f, 1.0f);   // higher = colder
            const float slope = wf.slope_at(cx_w, row.z);
            plat.top_texture   = top_texture_for(opt.biome, bio, seed, cx_w, row.z,
                                                 moist, alt, temp);
            plat.front_texture = front_texture_for(opt.biome, bio, slope, alt);
            plat.z             = row.z;
            // Uniform band ±B on EVERY row: adjacent rows share their boundary
            // depth plane — the Z seam (X-seam system recreated in depth).
            plat.min_depth     = -band;
            plat.max_depth     =  band;
            plat.surface_width = 130.0f + (float)p * 10.0f;   // vanilla 130–190

            std::string nm;
            if (row.walkable)       nm = "ground" + std::to_string(p + 1);
            else if (row.front)     nm = "ground_fr" + std::to_string(row.k) + "_" + std::to_string(p);
            else                    nm = "ground_bg" + std::to_string(-row.k) + "_" + std::to_string(p);
            const std::string bytes = build_ground_object(plat, nm);
            if (bytes.empty()) continue;
            scene.write_bytes_field(1, local_extract_object_bytes(bytes));
            ++r.objects;

            if (row.walkable) {
                local_track_aabb(minx, miny, maxx, maxy, plat.polygon);
                walkable_plats.push_back(plat);
            }
            row_plats.push_back(std::move(plat));
        }

        // ── Sky islands: near/mid depth rows only (front rows never — they
        //    would occlude the hero; ultra-far rows are too small to matter).─
        if (opt.sky_islands && !row.walkable && !row.front && -row.k <= 6) {
            const float thresh = 1.0f - std::clamp(opt.island_density, 0.25f, 0.80f);
            auto ground_max_at = [&](float x) -> float {
                const int i = std::clamp((int)std::llround((x - x0) / block), 0, cols - 1);
                return h[(size_t)i];
            };
            int run = 0, run_start = 0, emitted = 0;
            const int max_isles = 2;
            for (int i = 0; i <= cols; ++i) {
                const bool on = i < cols
                    && wf.island_at(x0 + (float)i * block, row.z) > thresh;
                if (on) {
                    if (run == 0) run_start = i;
                    ++run;
                    continue;
                }
                if (run >= 2 && emitted < max_isles) {
                    const float cx    = x0 + (float)(run_start + run / 2) * block;
                    const float w_raw = (float)run * block + block;
                    float y_top = wf.H * 0.40f
                        + wf.island_at(cx * 1.7f, row.z + 911.0f) * wf.H * 0.12f;
                    const float gmax = ground_max_at(cx);
                    y_top = std::max(quantize(y_top), gmax + wf.H * 0.20f);
                    const float cap_h = block;
                    const float base_w = std::max(block, std::floor(w_raw * 0.6f / block) * block);
                    const float base_h = block * (1.0f + std::floor(std::fabs(
                        wf.island_at(cx, row.z + 55.0f)) + 1.0f));

                    Platform cap;
                    cap.rect[0] = std::round(cx / block) * block;
                    cap.rect[1] = std::round((y_top - cap_h * 0.5f) / block) * block;
                    cap.rect[2] = std::round(w_raw / block) * block;
                    cap.rect[3] = cap_h;
                    // Smooth style: a round-hat dome turns the flat slab into a
                    // soft floating hill (blocky style keeps the pure voxel box).
                    if (!opt.blocky) {
                        Hat dome;
                        dome.x = cx;
                        dome.y = y_top;
                        dome.radius  = w_raw * 0.38f;
                        dome.height  = block * 2.2f;
                        cap.hats.push_back(dome);
                    }
                    Platform base;
                    base.rect[0] = cap.rect[0];
                    base.rect[1] = cap.rect[1] - cap_h * 0.5f - base_h * 0.5f;
                    base.rect[2] = base_w;
                    base.rect[3] = base_h;
                    const float isl_moist = wf.moisture_at(cx, row.z);
                    const float isl_alt   = std::clamp(0.72f + wf.altitude01(cx, row.z) * 0.3f, 0.0f, 1.0f);
                    const float isl_temp  = std::clamp(wf.temperature_at(cx, row.z) - isl_alt * 0.35f, 0.0f, 1.0f);
                    for (auto* pp : {&cap, &base}) {
                        pp->seed        = (uint32_t)(seed ^ 0x1517u ^ (uint32_t)(int)cx);
                        pp->top_texture = top_texture_for(opt.biome, bio, seed, cx, row.z,
                                                          isl_moist, isl_alt, isl_temp);
                        pp->front_texture = front_texture_for(opt.biome, bio, 0.6f, isl_alt);
                        pp->z           = row.z;
                        pp->min_depth   = -band;
                        pp->max_depth   =  band;
                        pp->surface_width = 100.0f;
                    }
                    const std::string tag = "isle_bg" + std::to_string(-row.k)
                                          + "_" + std::to_string(++emitted);
                    const std::string bc = build_ground_object(cap, tag + "_cap");
                    if (!bc.empty()) {
                        scene.write_bytes_field(1, local_extract_object_bytes(bc));
                        ++r.objects;
                    }
                    const std::string bb = build_ground_object(base, tag + "_base");
                    if (!bb.empty()) {
                        scene.write_bytes_field(1, local_extract_object_bytes(bb));
                        ++r.objects;
                    }
                    // Occasional tree on the wider islands (vanilla-sparse).
                    if (run >= 4 && bio.deco_trees[0][0] &&
                        rng_float(drng, 0.0f, 1.0f) < 0.40f) {
                        Deco d;
                        d.x = cx;
                        d.y = y_top;
                        d.z = row.z + rng_float(drng, -10.0f, 10.0f);
                        d.scale = persp_scale(row.z)
                                * (opt.randomize_deco_scale ? 0.9f + rng_float(drng, 0.0f, 0.4f) : 1.0f);
                        d.pod = bio.deco_trees[0];
                        const std::string b = build_deco_object(d, tag + "_tree");
                        if (!b.empty()) {
                            scene.write_bytes_field(1, local_extract_object_bytes(b));
                            ++r.objects;
                        }
                    }
                    run = 0;
                } else if (run > 0) {
                    run = 0;
                }
            }
        }

        // ── Decoration scatter (vanilla budget ≈3.5 decos / 1000u) ───────────
        const float dens = (opt.deco_density <= 0.0f) ? 1.0f : std::clamp(opt.deco_density, 0.0f, 2.0f);
        int ntree = 0; while (ntree < 6 && bio.deco_trees[ntree][0]) ++ntree;
        int nrock = 0; while (nrock < 4 && bio.deco_rocks[nrock][0]) ++nrock;
        int ngras = 0; while (ngras < 4 && bio.deco_grass[ngras][0]) ++ngras;

        // v1 biased depth draw: ~65% foreground, ~35% background.
        auto biased_depth = [&](float near_lo, float near_hi,
                                float far_lo,  float far_hi) -> float {
            return (rng_float(drng, 0.0f, 1.0f) < 0.65f)
                ? rng_float(drng, near_lo, near_hi)
                : rng_float(drng, far_lo, far_hi);
        };

        int deco_idx = 0, torch_idx = 0, glow_idx = 0;
        const uint32_t row_salt = (uint32_t)(0x3000u + (uint32_t)row.k * 0x9Eu);
        const std::string npfx = row.front ? ("fr" + std::to_string(row.k) + "_")
                                           : ("bg" + std::to_string(-row.k) + "_");

        for (const auto& plat : row_plats) {
            const TopEdge te = build_top_edge(plat);
            if (te.pts.size() < 2) continue;

            // ── Ecological scatter ───────────────────────────────────────────
            // Spacing is MODULATED by the local vegetation-density field and
            // slope: dense/moist flats get tighter spacing (clusters), while
            // dry, sparse or steep spots stretch spacing out or skip entirely
            // (natural gaps). This replaces uniform random scatter with
            // spatially-correlated placement. `veg_bias` lets rocks invert the
            // relationship (more rocks where vegetation is sparse/steep).
            auto scatter_pass_eco = [&](float base_sp, uint32_t salt, float veg_bias,
                                        float slope_limit,
                                        const std::function<void(float, float)>& emit) {
                if (base_sp < 24.0f) base_sp = 24.0f;
                uint64_t s = ((uint64_t)seed << 1) ^ (uint64_t)salt ^ (uint64_t)row_salt;
                float x = te.min_x + rng_float(s, 0.0f, base_sp);
                while (x < te.max_x) {
                    const float gy = ground_y_at(te, x);
                    if (gy > -1e8f) {
                        const float vd    = wf.veg_density_at(x, row.z);      // 0..1
                        const float slope = wf.slope_at(x, row.z);
                        // density weight in 0..1: high veg field + gentle slope
                        // => weight→1 (place, tight); low field/steep => →0.
                        float w = veg_bias >= 0.0f ? vd : (1.0f - vd);
                        w *= std::clamp(1.0f - slope / std::max(0.05f, slope_limit), 0.0f, 1.0f);
                        // Probabilistic gate → organic clusters and empty gaps.
                        if (w > 0.18f && rng_float(s, 0.0f, 1.0f) < (0.35f + 0.65f * w))
                            emit(x, gy);
                        // Local spacing shrinks in dense areas (clustering),
                        // grows in sparse ones — spatially correlated, not uniform.
                        const float local_sp = base_sp * (1.55f - 0.85f * w);
                        x += local_sp * rng_float(s, 0.65f, 1.35f);
                    } else {
                        x += base_sp;
                    }
                }
            };
            // Back-compat uniform pass (torches/glows don't need ecology).
            auto scatter_pass = [&](float mean_sp, uint32_t salt,
                                    const std::function<void(float, float)>& emit) {
                if (mean_sp < 24.0f) mean_sp = 24.0f;
                uint64_t s = ((uint64_t)seed << 1) ^ (uint64_t)salt ^ (uint64_t)row_salt;
                float x = te.min_x + rng_float(s, 0.0f, mean_sp);
                while (x < te.max_x) {
                    const float gy = ground_y_at(te, x);
                    if (gy > -1e8f) emit(x, gy);
                    x += mean_sp * rng_float(s, 0.60f, 1.40f);
                }
            };
            // Ground a decoration in the row's depth band (never a random far
            // plane): jitter within ±0.7·band around the row centre so it sits
            // on THIS terrain surface instead of floating between planes.
            auto band_depth = [&]() -> float {
                return row.z + rng_float(drng, -band * 0.7f, band * 0.7f);
            };

            if (row.walkable) {
                // ── Gameplay row: budget 1/900 + 1/1400 + 1/900 + torches
                //    ≈ 3.5 objects / 1000u — vanilla-measured density. ─────
                // TREES: follow the vegetation-density field (clusters in
                // moist flats, gaps in dry/steep spots), grounded in-band on
                // the actual surface, with sane scale/rotation variation.
                if (ntree > 0) {
                    const float sp = std::clamp(760.0f / dens, 420.0f, 2000.0f);
                    scatter_pass_eco(sp, 0xA11CEu, +1.0f, /*slope_limit*/0.55f,
                                     [&](float x, float gy) {
                        // Cluster size grows with the local density field.
                        const float vd = wf.veg_density_at(x, row.z);
                        const int cluster = 1 + (int)(vd * 2.0f * rng_float(drng, 0.4f, 1.0f));
                        for (int ci = 0; ci < cluster; ++ci) {
                            const float ox = ci ? rng_float(drng, 26.0f, 64.0f) * (rng_float(drng,0,1)<0.5f?-1.f:1.f)
                                                : rng_float(drng, -14.0f, 14.0f);
                            Deco d;
                            d.x = x + ox;
                            d.y = ground_y_at(te, d.x) + rng_float(drng, -3.0f, 3.0f);
                            if (d.y < -1e8f) continue;
                            d.z = band_depth();                 // sit on THIS surface
                            d.scale = opt.randomize_deco_scale ? (0.85f + rng_float(drng, 0.0f, 0.35f)) : 1.0f;
                            d.rot_y = (opt.randomize_deco_rotation && rng_float(drng, 0.0f, 1.0f) < 0.45f) ? -1.5707963f : 0.0f;
                            d.pod = bio.deco_trees[local_hash_uint((uint32_t)(deco_idx * 131 + 7)) % (unsigned)ntree];
                            const std::string b = build_deco_object(d, "deco_tree" + std::to_string(++deco_idx));
                            if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                        }
                    });
                }
                // ROCKS: inverse ecology — thrive where vegetation is sparse or
                // the slope is steep (rocky outcrops), so they fill the gaps
                // trees leave. Lenient slope limit (rocks sit on steeper ground).
                if (nrock > 0) {
                    const float sp = std::clamp(1400.0f / dens, 700.0f, 3000.0f);
                    scatter_pass_eco(sp, 0xB0B0u, -1.0f, /*slope_limit*/1.20f,
                                     [&](float x, float gy) {
                        Deco d;
                        d.x = x + rng_float(drng, -18.0f, 18.0f);
                        d.y = ground_y_at(te, d.x) + rng_float(drng, -2.0f, 2.0f);
                        if (d.y < -1e8f) return;
                        d.z = band_depth();
                        d.scale = opt.randomize_deco_scale ? (0.65f + rng_float(drng, 0.0f, 0.45f)) : 1.0f;
                        d.pod = bio.deco_rocks[local_hash_uint((uint32_t)(deco_idx * 337 + 13)) % (unsigned)nrock];
                        const std::string b = build_deco_object(d, "deco_rock" + std::to_string(++deco_idx));
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }
                // GRASS/TUFTS: follow vegetation density like trees but denser
                // and tolerant of moderate slope; small, grounded, in-band.
                if (ngras > 0) {
                    const float sp = std::clamp(720.0f / dens, 380.0f, 1800.0f);
                    scatter_pass_eco(sp, 0x6A55u, +1.0f, /*slope_limit*/0.80f,
                                     [&](float x, float gy) {
                        Deco d;
                        d.x = x + rng_float(drng, -22.0f, 22.0f);
                        d.y = ground_y_at(te, d.x) + rng_float(drng, -2.0f, 2.0f);
                        if (d.y < -1e8f) return;
                        d.z = band_depth();
                        d.scale = opt.randomize_deco_scale ? (0.30f + rng_float(drng, 0.0f, 0.30f)) : 1.0f;
                        d.pod = bio.deco_grass[local_hash_uint((uint32_t)(deco_idx * 613 + 23)) % (unsigned)ngras];
                        const std::string b = build_deco_object(d, "deco_grass" + std::to_string(++deco_idx));
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }
                if (opt.spill_torches && bio.torch_pod[0]) {
                    const float sp = std::clamp(1100.0f / dens, 600.0f, 2400.0f);
                    int slot = 0;
                    scatter_pass(sp, 0x70C4u, [&](float x, float gy) {
                        const bool real_torch = (slot++ % 3) == 0;
                        TorchLight tl;
                        tl.x = x + rng_float(drng, -sp * 0.15f, sp * 0.15f);
                        tl.y = gy + 6.0f;
                        tl.z = biased_depth(30.0f, 107.0f, -80.0f, -20.0f);
                        tl.glow_r = bio.glow_r; tl.glow_g = bio.glow_g; tl.glow_b = bio.glow_b;
                        std::string b;
                        if (real_torch) {
                            tl.radius = 300.0f + rng_float(drng, 0.0f, 100.0f);
                            tl.intensity = 2.0f;
                            b = build_torch_object(tl, "torch" + std::to_string(++torch_idx));
                        } else {
                            tl.radius = 180.0f + rng_float(drng, 0.0f, 80.0f);
                            tl.intensity = 1.1f;
                            tl.glow_a = 0.35f;
                            b = build_glow_light(tl, "glow" + std::to_string(++glow_idx));
                        }
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }
            } else if (opt.scatter_far_trees && ntree > 0 && !row.front) {
                // ── Depth rows: perspective-compensated forest. Spacing grows
                //    with 1/scale (screen density) AND with row index (far
                //    rows are screen-tiny; the stacked rows already read as
                //    forest). Front rows stay bare — trees would occlude. ──
                const float sc = persp_scale(row.z);
                const float sp = std::clamp(900.0f / sc * (1.0f + 0.35f * (float)(-row.k - 1)) / dens,
                                            500.0f, 9000.0f);
                scatter_pass(sp, 0xA99Bu, [&](float x, float gy) {
                    Deco d;
                    d.x = x + rng_float(drng, -60.0f, 60.0f);
                    d.y = gy + rng_float(drng, -5.0f, 10.0f);
                    d.z = row.z + rng_float(drng, -12.0f, 12.0f);
                    d.scale = sc * (opt.randomize_deco_scale
                        ? (0.9f + rng_float(drng, 0.0f, 0.4f)) : 1.0f);
                    d.rot_y = (opt.randomize_deco_rotation && rng_float(drng, 0.0f, 1.0f) < 0.5f) ? -1.5707963f : 0.0f;
                    d.pod = bio.deco_trees[local_hash_uint((uint32_t)(deco_idx * 131 + 7)) % (unsigned)ntree];
                    const std::string b = build_deco_object(d, "deco_" + npfx + std::to_string(++deco_idx));
                    if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                });
            }
        }
    } // end row loop

    // ── Cave glow lights (gameplay-row canyon floors) ─────────────────────────
    int cave_glow_idx = 0;
    for (const auto& cg : cave_glows) {
        TorchLight tl;
        tl.x = cg.x; tl.y = cg.y;
        tl.z = rng_float(drng, 25.0f, 55.0f);
        tl.radius = 160.0f + rng_float(drng, 0.0f, 70.0f);
        tl.intensity = 1.0f;
        tl.glow_r = bio.glow_r; tl.glow_g = bio.glow_g; tl.glow_b = bio.glow_b;
        tl.glow_a = 0.30f;
        const std::string b = build_glow_light(tl, "caveglow" + std::to_string(++cave_glow_idx));
        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
    }

    // ── Portal hub (optional; anchored on the gameplay row extremes) ──────────
    if (opt.add_portal && !walkable_plats.empty()) {
        auto portal_extreme_anchor = [&](const Platform& plat, int edge,
                                         float& x, float& gy) -> bool {
            const TopEdge te = build_top_edge(plat);
            if (te.pts.size() < 2 || (te.max_x - te.min_x) < 320.0f) return false;
            x  = (edge < 0) ? te.min_x + 180.0f : te.max_x - 180.0f;
            gy = ground_y_at(te, x);
            return gy > -1e8f;
        };
        const bool left_first = rng_float(drng, 0.0f, 1.0f) < 0.5f;
        const int    e0 = left_first ? -1 : 1;
        const size_t p0 = left_first ? 0 : walkable_plats.size() - 1;
        const int    e1 = -e0;
        const size_t p1 = left_first ? walkable_plats.size() - 1 : 0;

        float px = 0.0f, py = 0.0f;
        if (portal_extreme_anchor(walkable_plats[p0], e0, px, py)) {
            PortalHubOptions ph;
            ph.x = px; ph.y = py + 4.0f;
            ph.facing = -e0;
            ph.destination = opt.portal_destination;
            ph.portal_name = "portal";
            for (const auto& bytes : build_portal_hub(ph, opt.biome, drng)) {
                scene.write_bytes_field(1, local_extract_object_bytes(bytes));
                ++r.objects;
            }
        }
        bool ok2 = false;
        if (walkable_plats.size() == 1)
            ok2 = portal_extreme_anchor(walkable_plats[p0], e1, px, py);
        else
            ok2 = portal_extreme_anchor(walkable_plats[p1], e1, px, py)
               || portal_extreme_anchor(walkable_plats[p0], e1, px, py);
        if (ok2) {
            PortalHubOptions ph;
            ph.x = px; ph.y = py + 4.0f;
            ph.facing = -e1;
            ph.destination = opt.portal_destination;
            ph.portal_name = "portal";
            for (const auto& bytes : build_game_portal_hub(ph, opt.biome, drng)) {
                scene.write_bytes_field(1, local_extract_object_bytes(bytes));
                ++r.objects;
            }
        }
    }

    // ── Spawn: on the centre pad, +56 clearance (v1 convention) ───────────────
    if (walkable_plats.empty()) { r.error = "v2-3d: no walkable terrain generated"; return r; }
    scene.write_bytes_field(1, local_extract_object_bytes(
        build_spawn_object("spawn_default", 0.0f, spawn_pad_y + 56.0f, 1)));
    ++r.objects;

    // ── ObjectLibrary (required for TemplateName refs to resolve in-game) ─────
    static const std::vector<std::string> kImports = {
        "caves_stuff", "collectibles", "florennum_stuff", "forest",
        "game_common", "grovestuff", "lights", "monsters", "npc",
        "plains_stuff", "platforms", "playground", "programs", "rocks",
        "scriptarea", "traps_stuffs", "trash", "woodkeep_stuff", "woods",
    };
    scene.write_bytes_field(2, build_object_library(opt.scene_name, kImports));

    // ── Root Bounds — v1 CAMERA SEMANTICS ─────────────────────────────────────
    // v2's camera_follow_y_shape emission is buggy, so v2-3d emits NO camera
    // shapes at all and computes Bounds exactly like v1: plain AABB of the
    // walkable geometry (gameplay row) + fixed padding.
    if (maxx <= minx || maxy <= miny) { r.error = "v2-3d: empty scene AABB"; return r; }
    const float pad = 220.0f;
    const float bx  = minx - pad, by = miny - pad;
    const float bw  = (maxx - minx) + 2.0f * pad;
    const float bh  = (maxy - miny) + 2.0f * pad;
    scene.write_bytes_field(3, build_bounds_payload(bx, by, bw, bh));

    r.bounds[0] = bx; r.bounds[1] = by; r.bounds[2] = bw; r.bounds[3] = bh;
    r.scene_bytes = scene.to_string();
    return r;
}

} // namespace v2_3d
} // namespace sgen
