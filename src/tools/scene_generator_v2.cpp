/*
 * scene_generator_v2.cpp — Swordigo procedural scene generator v2.
 *
 * Major upgrades over v1 (scene_generator.cpp):
 *   1. Z-layer system: background silhouette terrain (Z≤-80), main walkable
 *      ground (Z≈0), optional foreground cliff framing (Z≥+80).
 *   2. Z-path (enable_z_path): each walkable platform strip gets its Z
 *      offset by a smooth low-frequency fbm curve — the ground path winds
 *      toward/away from the camera along the scene's length.
 *   3. Background terrain strips: rougher (fewer smooth passes, higher
 *      roughness), taller, wider — create distant mountain silhouettes.
 *   4. Foreground clip terrain: lower, narrower, cliff-textured framing.
 *   5. Multi-band decoration scatter:
 *        • Main layer: same even-spaced scatter as v1.
 *        • Background pass: large-scale (1.4–2.5×) deep-negative-Z decos on
 *          every main-layer platform — parallax forest/mountain feel.
 *        • Foreground pass: small-scale (0.4–0.8×) rocks/shrubs at Z>+40.
 *   6. Terracing: optional quantized step bands in the main heightfield.
 *   7. Overhangs: cliff edges jutted outward for ledge silhouettes.
 *
 * All protobuf encoding is done through the existing sgen:: builder functions.
 * Only the static helpers that are not exposed in scene_generator.h are
 * re-implemented locally (local_* prefix).
 */

#include "tools/scene_generator_v2.h"
#include "tools/scene_generator.h"
#include "tools/boulder.h"
#include "platform/protobuf_reader.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <functional>
#include <sstream>

namespace sgen {
namespace v2 {

// ─────────────────────────────────────────────────────────────────────────────
// Local static helpers (re-implemented so we don't need to expose sgen statics)
// ─────────────────────────────────────────────────────────────────────────────

// Unwrap scene_wrap_object() back to the bare object bytes.
// scene_wrap_object() writes:  field-1 (tag=0x0A) + varint length + object bytes
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

// AABB tracking helper
static void local_track_aabb(float& minx, float& miny, float& maxx, float& maxy,
                              const std::vector<Vec2>& poly) {
    for (const auto& v : poly) {
        minx = std::min(minx, v.x); miny = std::min(miny, v.y);
        maxx = std::max(maxx, v.x); maxy = std::max(maxy, v.y);
    }
}

// Local protobuf writers (sgen::v2/rect_msg are static in scene_generator.cpp).
static proto::Writer local_v2(float x, float y) {
    proto::Writer w;
    w.write_float_field(1, x);
    w.write_float_field(2, y);
    return w;
}
static proto::Writer local_rect(float x, float y, float w, float h) {
    proto::Writer r;
    r.write_float_field(1, x); r.write_float_field(2, y);
    r.write_float_field(3, w); r.write_float_field(4, h);
    return r;
}

// Deterministic integer hash — same algorithm as sgen::hash_uint (static in
// scene_generator.cpp, not linkable here, so re-implemented locally).
// Used to select decoration templates by index in a reproducible way.
static inline uint32_t local_hash_uint(uint32_t x) {
    x = (x ^ 61u) ^ (x >> 16);
    x = x + (x << 3);
    x = x ^ (x >> 4);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15);
    return x;
}

// 1D Gaussian blur — same algorithm as sgen::smooth_gaussian

static void local_smooth_gaussian(std::vector<float>& h, int radius = 4, int passes = 3) {
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
                v += w[k + radius] * h[j];
            }
            tmp[i] = v;
        }
        h = tmp;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Terrain variation passes
// ─────────────────────────────────────────────────────────────────────────────

// Quantize portions of the heightfield into discrete step bands.
// `strength` 0=none, 1=fully stepped (quantized).
static void apply_terracing(std::vector<float>& h, float strength, uint32_t /*seed*/) {
    if (h.empty() || strength <= 0.0f) return;
    const float mn = *std::min_element(h.begin(), h.end());
    const float mx = *std::max_element(h.begin(), h.end());
    const float range = mx - mn;
    if (range < 10.0f) return;
    const int steps = 6;
    const float inv_range = 1.0f / range;
    for (auto& v : h) {
        float norm = (v - mn) * inv_range;          // [0,1]
        float terr = std::floor(norm * steps + 0.5f) / steps; // quantized
        v = mn + (norm + (terr - norm) * strength) * range;
    }
}

// At steep cliff edges, push the top vertex outward to form a ledge overhang.
// Operates on the polygon's top-surface vertices (those above the mid-height).
static void apply_overhangs(std::vector<Vec2>& poly, float strength, uint64_t& rng) {
    if (poly.size() < 4 || strength <= 0.0f) return;
    const int n = (int)poly.size();
    // Find approximate mid-height to identify top-surface vertices
    float max_y = -1e9f;
    for (const auto& v : poly) max_y = std::max(max_y, v.y);
    const float thresh_y = max_y - (max_y * 0.5f); // top half

    for (int i = 2; i < n - 2; ++i) {
        // Only nudge top-surface vertices (large positive Y = high up)
        if (poly[i].y < thresh_y) continue;
        // Steep downward step to the next vertex?
        float dy = poly[i].y - poly[(i + 1) % n].y;
        if (dy > 55.0f * strength) {
            const float overhang = rng_float(rng, 8.0f * strength, 36.0f * strength);
            poly[i].x -= overhang;  // jut outward (CCW — left = outward on descent)
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BiomeSpecV2 table
// ─────────────────────────────────────────────────────────────────────────────

const BiomeSpecV2& biome_spec_v2(Biome b) {
    // We can't aggregate-init BiomeSpecV2 from biome_spec() at compile time
    // because biome_spec() is a runtime call.  Build the table once lazily.
    static bool built = false;
    static BiomeSpecV2 tbl[static_cast<int>(Biome::Count)];
    if (!built) {
        struct Entry {
            Biome biome;
            const char* deco_bg[4];
            const char* deco_fg[4];
            const char* bg_tex;
            float bg_scale;
        };
        // Vanilla-confirmed bg/fg decoration templates and textures:
        // forest_part1: treewall1/treewall2 at Z=-111/-76, bush at Z=+66/+71
        // grove_part1:  grove_hang1/3/4 at Z=-77/+71, grove_pole at Z=-77
        // fire_part31:  stonepillairs2/3 at Z=-112/-69
        static const Entry kExtra[static_cast<int>(Biome::Count)] = {
            // Grasslands — bg: large trees; fg: rocks/pots
            { Biome::Grasslands,
              {"grove_tree1","grove_tree2","bush",""},
              {"smallrock1","rock1","",""},
              "maybegood", 1.8f },
            // Forest — bg: treewall models (vanilla confirmed), fg: bush/rock
            { Biome::Forest,
              {"grove_tree1","grove_tree2","grove_tree3",""},
              {"smallrock1","bush","",""},
              "forest_ground", 1.8f },
            // Grove — bg: hanging decorations + poles (vanilla confirmed), fg: rocks
            { Biome::Grove,
              {"grove_tree1","grove_tree2","grove_tree3",""},
              {"rock1","pot","",""},
              "grove_ground", 1.9f },
            // Wasteland — bg: dead trees + rocks; fg: smallrocks
            { Biome::Wasteland,
              {"deadtree1","deadtree2","rock1",""},
              {"smallrock1","rock1","",""},
              "grasslandsbackground_day", 2.0f },
            // IceCastle — bg: snowy trees + icicles; fg: icicles
            { Biome::IceCastle,
              {"snowy_tree1","icicles1","icicle",""},
              {"icicle","smallrock1","",""},
              "atlon_ground", 2.2f },
            // Cave — bg: stalactite icicles + rocks; fg: rocks
            { Biome::Cave,
              {"icicles1","icicles2","rock1",""},
              {"smallrock1","rock1","",""},
              "lowercave_ground1", 1.5f },
            // Fire — bg: stonepillairs (vanilla confirmed); fg: smallrocks
            { Biome::Fire,
              {"deadtree1","deadtree2","rock1",""},
              {"smallrock1","rock1","",""},
              "graveyard_ground", 1.8f },
            // Florennum — bg: trees + pottery; fg: small pots/rocks
            { Biome::Florennum,
              {"grove_tree1","pot","pottery1",""},
              {"smallrock1","rock1","",""},
              "florennum_ground", 1.5f },
        };
        for (int i = 0; i < static_cast<int>(Biome::Count); ++i) {
            // Copy base BiomeSpec
            static_cast<BiomeSpec&>(tbl[i]) = biome_spec(static_cast<Biome>(i));
            // Fill v2 extras
            for (int j = 0; j < 4; ++j) {
                tbl[i].deco_background[j] = kExtra[i].deco_bg[j];
                tbl[i].deco_foreground[j] = kExtra[i].deco_fg[j];
            }
            tbl[i].bg_ground_texture = kExtra[i].bg_tex;
            tbl[i].bg_terrain_scale  = kExtra[i].bg_scale;
        }
        built = true;
    }
    int idx = static_cast<int>(b);
    if (idx < 0 || idx >= static_cast<int>(Biome::Count)) idx = 0;
    return tbl[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-edge sampler for even-spaced scatter (same logic as v1's scatter_even)
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
// Main v2 biome scene generator
// ─────────────────────────────────────────────────────────────────────────────

Result generate_biome_scene_v2(const TerrainOptionsV2& opt) {
    Result r;
    const BiomeSpecV2& bio = biome_spec_v2(opt.biome);
    const uint32_t seed     = opt.seed ? opt.seed : 1u;
    const int    n_plat     = std::max(2, opt.platform_count);
    const float  half_w     = opt.width * 0.5f;
    const float  floor_y    = -opt.height * 0.5f;  // NEGATIVE = downward (matches v1)
    const float  kGroundDepth = 112.0f;

    // ── Determine Z-layer stack ───────────────────────────────────────────────
    struct LayerDesc {
        float z;
        float scale_x;       // X width multiplier
        float height_scale;  // terrain amplitude multiplier
        bool  walkable;
        bool  emit_decos;
        int   smooth_passes;
        float roughness_mult;
        const char* top_tex;
        const char* front_tex;
    };

    std::vector<LayerDesc> layers;

    // Background visual layers (Z negative — behind main ground).
    // Vanilla-confirmed Z values: forest_part1 obj5#11=-136.1, obj5#12=-128.9;
    //                            grove_part1 'aasdf'=-161.4.
    // We place the 2 default bg layers at Z=-130 and Z=-165 (matching vanilla).
    // bg_z_step default (-100) overridden here if the caller left it default.
    const float bg_z0 = (opt.bg_z_step == -100.0f) ? -130.0f : opt.bg_z_step;
    const float bg_z1 = (opt.bg_layers > 1) ? bg_z0 - 35.0f : bg_z0; // second layer 35 further
    if (opt.enable_bg_terrain) {
        for (int i = 0; i < opt.bg_layers; ++i) {
            LayerDesc bg;
            // Layer 0 ≈ -130 (vanilla: -128.9); Layer 1 ≈ -165 (vanilla: -161.4)
            bg.z             = (i == 0) ? bg_z0 : bg_z1 - (float)(i - 1) * 30.0f;
            bg.scale_x       = 1.2f + (float)i * 0.2f;    // wider each layer back
            bg.height_scale  = bio.bg_terrain_scale + (float)i * 0.35f;
            bg.walkable      = false;
            bg.emit_decos    = opt.scatter_background_decos;
            bg.smooth_passes = std::max(1, 2 - i);         // rougher further back
            bg.roughness_mult = 1.8f + (float)i * 0.4f;
            bg.top_tex       = bio.bg_ground_texture;
            bg.front_tex     = bio.bg_ground_texture;
            layers.push_back(bg);
        }
    }

    // Main walkable layer (Z ≈ 0)
    {
        LayerDesc main;
        main.z            = 0.0f;
        main.scale_x      = 1.0f;
        main.height_scale = 1.0f;
        main.walkable     = true;
        main.emit_decos   = true;
        main.smooth_passes = 3;
        main.roughness_mult = 1.0f;
        main.top_tex      = bio.ground_top;
        main.front_tex    = bio.ground_front;
        layers.push_back(main);
    }

    // Foreground cliff framing layer (Z positive — in front of main ground)
    if (opt.enable_fg_terrain) {
        LayerDesc fg;
        fg.z             =  80.0f;
        fg.scale_x       =  0.85f;    // narrower
        fg.height_scale  =  0.35f;    // lower
        fg.walkable      = false;
        fg.emit_decos    = opt.scatter_foreground_decos;
        fg.smooth_passes =  2;
        fg.roughness_mult = 1.3f;
        fg.top_tex       = bio.ground_front;
        fg.front_tex     = bio.ground_front;
        layers.push_back(fg);
    }

    // ── Shared scene writer ───────────────────────────────────────────────────
    proto::Writer scene;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    // Walkable top profile (min/max Y of the ground SURFACE) — drives the
    // scene Bounds so the in-game camera clamps to the playable band instead
    // of the deep heightfield base.
    float top_min = 1e30f, top_max = -1e30f;
    uint64_t drng = (uint64_t)seed ^ 0xBEEF1337ull;
    r.objects = 0;

    // Background + light emitted once
    scene.write_bytes_field(1, local_extract_object_bytes(
        build_background_object(bio.background)));
    ++r.objects;

    const float light_depth = 17.0f + opt.width * 0.018f;
    scene.write_bytes_field(1, local_extract_object_bytes(
        build_light_object("DirectionalLight", 0.0f, 0.0f, light_depth,
                           bio.key_light[0] > 0.0f ? std::clamp(bio.key_light[0] * 3.2f, 2.6f, 3.4f) : 3.0f,
                           bio.ambient[0]   > 0.0f ? bio.ambient[0] * 0.8f : 0.3f,
                           0.4f,
                           bio.key_light[0], bio.key_light[1], bio.key_light[2])));
    ++r.objects;

    // ── Per-layer terrain generation ──────────────────────────────────────────
    static const float kPlateauHalf = 0.12f;  // center plateau fraction

    // Walkable platforms are collected so spawn + the optional portal hub can
    // be anchored on real ground (v1 parity: v1 finds the platform under x=0;
    // v2 previously placed the spawn at the scene's global max Y, which could
    // strand the hero on a distant peak).
    std::vector<Platform> walkable_plats;

    for (const auto& layer : layers) {
        // --- 1. Heightfield synthesis ---
        const float layer_width = opt.width * layer.scale_x;
        const int samples = std::max(48, (int)(layer_width / 40.0f));
        const float step  = layer_width / (float)(samples - 1);

        std::vector<float> prof(samples);
        for (int i = 0; i < samples; ++i) {
            const float x  = -layer_width * 0.5f + (float)i * step;
            const float nx = x / layer_width * 6.0f;
            float v;
            if (opt.mountains) {
                const float ridge = ridged_2d(nx, 0.5f, opt.octaves, seed ^ (uint32_t)(int)layer.z);
                const float low   = fbm_1d(nx, std::max(2, opt.octaves - 1), seed ^ 0xFFu ^ (uint32_t)(int)layer.z, 0.2f);
                v = ridge * 0.70f + low * 0.30f;
            } else {
                v = fbm_1d(nx, opt.octaves, seed ^ (uint32_t)(int)layer.z, 0.35f);
            }
            prof[i] = v * (opt.height * 0.28f * opt.roughness * layer.roughness_mult * layer.height_scale);
        }

        // --- 2. Smoothing ---
        local_smooth_gaussian(prof, 5, layer.smooth_passes);

        // --- 3. Spawn-pad guarantee (main walkable layer only) ---
        if (layer.walkable) {
            const float plateau_half = layer_width * kPlateauHalf;
            const float base         = opt.height * 0.02f;
            for (int i = 0; i < samples; ++i) {
                const float x = -layer_width * 0.5f + (float)i * step;
                const float d = std::fabs(x) / plateau_half;
                if (d >= 1.0f) continue;
                const float plat_h = base + (1.0f - d * d) * opt.height * 0.04f;
                prof[i] = std::max(prof[i], plat_h);
            }
        }

        // --- 4. Terracing (walkable main layer only) ---
        if (layer.walkable && opt.add_terracing)
            apply_terracing(prof, opt.terrace_strength, seed);

        // --- 5. Platform slicing (same seamless boundary-sharing as v1) ---
        const int n_strips = layer.walkable ? n_plat : std::max(1, n_plat / 2);

        for (int p = 0; p < n_strips; ++p) {
            const int s0 = std::clamp((int)std::llround((double)p       * (samples - 1) / n_strips), 0, samples - 1);
            const int s1 = std::clamp((int)std::llround((double)(p + 1) * (samples - 1) / n_strips), 0, samples - 1);
            if (s1 <= s0) continue;
            const int cnt = s1 - s0 + 1;

            std::vector<float> h(cnt);
            for (int i = 0; i < cnt; ++i) h[i] = prof[s0 + i];

            const float x0 = -layer_width * 0.5f + (float)s0 * step;

            // Build CCW polygon (top profile + floor base)
            auto poly = make_heightfield_polygon(h.data(), cnt, step,
                                                 floor_y * layer.height_scale - opt.height * 0.05f);
            for (auto& v : poly) v.x += x0;

            // Z-path for walkable main layer
            float strip_z = layer.z;
            if (layer.walkable && opt.enable_z_path && opt.z_path_amplitude > 0.0f) {
                const float mid_x = x0 + (float)(cnt - 1) * step * 0.5f;
                const float nx    = mid_x / opt.width * 4.0f;
                strip_z += fbm_1d(nx, 2, seed ^ 0xC0DEu, 0.0f) * opt.z_path_amplitude;
                strip_z  = std::clamp(strip_z, -40.0f, 40.0f); // keep hero-accessible
            }

            // Overhangs (walkable main layer only)
            if (layer.walkable && opt.add_overhangs)
                apply_overhangs(poly, opt.terrace_strength, drng);

            Platform plat;
            plat.polygon       = poly;
            plat.seed          = (uint32_t)(seed ^ ((uint32_t)p * 0x9E37u) ^ (uint32_t)(int)layer.z);
            plat.horiz_noise   = 0.0f;              // no noise: seams stay flush
            plat.top_texture   = layer.top_tex;
            plat.front_texture = layer.front_tex;
            plat.z             = strip_z;
            plat.min_depth     = -kGroundDepth;
            plat.max_depth     =  kGroundDepth;
            plat.surface_width = 100.0f + (float)p * 15.0f;

            // Islands mode (v1 parity): dome hats on every other walkable strip.
            if (layer.walkable && opt.islands && p % 2 == 0) {
                float cx = 0.0f, top = -1e9f;
                for (const auto& v : poly) { cx += v.x; top = std::max(top, v.y); }
                if (!poly.empty()) cx /= (float)poly.size();
                Hat hat;
                hat.x = cx; hat.y = top;
                hat.radius  = 80.0f + rng_float(drng, 0.0f, 60.0f);
                hat.height  = 45.0f + rng_float(drng, 0.0f, 35.0f);
                plat.hats.push_back(hat);
            }

            // Bake the ground mesh
            const std::string nm = "ground_L" + std::to_string((int)layer.z) + "_" + std::to_string(p);
            const std::string bytes = build_ground_object(plat, nm);
            if (bytes.empty()) continue;
            scene.write_bytes_field(1, local_extract_object_bytes(bytes));
            ++r.objects;

            // Track AABB (main walkable layer determines scene bounds)
            if (layer.walkable) {
                local_track_aabb(minx, miny, maxx, maxy, poly);
                walkable_plats.push_back(plat);
                const TopEdge te2 = build_top_edge(plat);
                for (const auto& pt : te2.pts) {
                    top_min = std::min(top_min, pt.y);
                    top_max = std::max(top_max, pt.y);
                }
            }

            // ── Decoration scatter ─────────────────────────────────────────
            if (!layer.emit_decos) continue;

            const TopEdge te     = build_top_edge(plat);
            if (te.pts.size() < 2) continue;
            const float   dens   = std::clamp(opt.deco_density, 0.0f, 2.0f);

            // Helper: even-spaced walk over the top edge
            auto scatter_pass = [&](float mean_sp, uint32_t salt,
                                    const std::function<void(float, float)>& emit) {
                if (mean_sp < 24.0f) mean_sp = 24.0f;
                uint64_t s = ((uint64_t)seed << 1) ^ (uint64_t)salt
                           ^ ((uint64_t)p * 0x9E37u)
                           ^ ((uint64_t)(uint32_t)(int)layer.z * 0xDEADBEEFu);
                float x = te.min_x + rng_float(s, 0.0f, mean_sp);
                while (x < te.max_x) {
                    const float gy = ground_y_at(te, x);
                    if (gy > -1e8f) emit(x, gy);
                    x += mean_sp * rng_float(s, 0.60f, 1.40f);
                }
            };

            // biased depth draw: ~65% foreground, ~35% background
            auto biased_depth = [&](float near_lo, float near_hi,
                                    float far_lo,  float far_hi) -> float {
                return (rng_float(drng, 0.0f, 1.0f) < 0.65f)
                    ? rng_float(drng, near_lo, near_hi)
                    : rng_float(drng, far_lo, far_hi);
            };

            int deco_idx = 0, torch_idx = 0, glow_idx = 0;

            if (layer.walkable) {
                // ── Main-layer decorations (mirrors v1 scatter) ─────────────
                int ntree = 0; while (ntree < 6 && bio.deco_trees[ntree][0]) ++ntree;
                int nrock = 0; while (nrock < 4 && bio.deco_rocks[nrock][0]) ++nrock;
                int ngras = 0; while (ngras < 4 && bio.deco_grass[ngras][0]) ++ngras;

                // Trees — use vanilla-confirmed Tier 3/4/5 depth distribution:
                //   Near-BG (Tier 3):  -27..-11   bush/tree partially behind ground
                //   Gameplay (Tier 4):  0            on the main plane
                //   Foreground (Tier 5): +22..+71   in front, creates depth illusion
                if (ntree > 0) {
                    const float sp = std::clamp(26.0f / (bio.tree_density * dens + 1e-3f), 150.0f, 900.0f);
                    scatter_pass(sp, 0xA11CEu, [&](float x, float gy) {
                        const int cluster = (rng_float(drng, 0.0f, 1.0f) < 0.30f) ? 2 : 1;
                        for (int ci = 0; ci < cluster; ++ci) {
                            Deco d;
                            d.x     = x + (ci ? rng_float(drng, 30.0f, 70.0f) : rng_float(drng, -16.0f, 16.0f));
                            d.y     = gy + rng_float(drng, -4.0f, 4.0f);
                            // biased: 65% foreground (Tier 5: +22..+71), 35% near-bg (Tier 3: -27..-11)
                            d.z     = biased_depth(22.0f, 71.0f, -27.0f, -11.0f);
                            d.scale = opt.randomize_deco_scale ? (0.80f + rng_float(drng, 0.0f, 0.55f)) : 1.0f;
                            d.rot_y = (opt.randomize_deco_rotation && rng_float(drng, 0.0f, 1.0f) < 0.45f) ? -1.5707963f : 0.0f;
                            d.pod   = bio.deco_trees[local_hash_uint((uint32_t)(deco_idx * 131 + 7)) % (unsigned)ntree];
                            const std::string b = build_deco_object(d, "deco_tree" + std::to_string(++deco_idx));
                            if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                        }
                    });
                }

                // Rocks / pillars — Tier 3 (-27..-11) and Tier 5 (+22..+48)
                // Vanilla: pot at -11/-1/+21, stoneblock at +47
                if (nrock > 0) {
                    const float sp = std::clamp(30.0f / (bio.rock_density * dens + 1e-3f), 180.0f, 1000.0f);
                    scatter_pass(sp, 0xB0B0u, [&](float x, float gy) {
                        Deco d;
                        d.x     = x + rng_float(drng, -18.0f, 18.0f);
                        d.y     = gy + rng_float(drng, -2.0f, 2.0f);
                        // Vanilla pot depths: Tier 3 -11, Tier 4 0, Tier 5 +21/+47
                        d.z     = biased_depth(21.0f, 47.0f, -27.0f, -5.0f);
                        d.scale = opt.randomize_deco_scale ? (0.60f + rng_float(drng, 0.0f, 0.65f)) : 1.0f;
                        d.pod   = bio.deco_rocks[local_hash_uint((uint32_t)(deco_idx * 337 + 13)) % (unsigned)nrock];
                        const std::string b = build_deco_object(d, "deco_rock" + std::to_string(++deco_idx));
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }

                // Small grass / tufts
                if (ngras > 0) {
                    const float sp = std::clamp(26.0f / (bio.grass_density * dens + 1e-3f), 70.0f, 400.0f);
                    scatter_pass(sp, 0x6A55u, [&](float x, float gy) {
                        Deco d;
                        d.x     = x + rng_float(drng, -24.0f, 24.0f);
                        d.y     = gy + rng_float(drng, -2.0f, 2.0f);
                        d.z     = biased_depth(5.0f, 55.0f, -50.0f, -5.0f);
                        d.scale = opt.randomize_deco_scale ? (0.28f + rng_float(drng, 0.0f, 0.40f)) : 1.0f;
                        d.pod   = bio.deco_grass[local_hash_uint((uint32_t)(deco_idx * 613 + 23)) % (unsigned)ngras];
                        const std::string b = build_deco_object(d, "deco_grass" + std::to_string(++deco_idx));
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }

                // Torches / glow — vanilla-confirmed torch depths: +48.92, +56.16
                // (Tier 5 foreground, to appear in front of the player slightly)
                // 1-in-3 slots becomes a real torch, rest are soft glow patches.
                if (opt.spill_torches && bio.torch_pod[0]) {
                    const float base_sp = (opt.torch_spacing > 0.0f) ? opt.torch_spacing : bio.torch_spacing;
                    const float sp = base_sp * 1.6f;
                    int slot = 0;
                    scatter_pass(sp, 0x70C4u, [&](float x, float gy) {
                        const bool real_torch = (slot++ % 3) == 0;
                        TorchLight tl;
                        tl.x = x + rng_float(drng, -sp * 0.15f, sp * 0.15f);
                        tl.y = gy + 6.0f;
                        // Vanilla: torch depth = +48.92 / +56.16 (Tier 5)
                        tl.z = rng_float(drng, 48.0f, 57.0f);
                        tl.glow_r = bio.glow_r; tl.glow_g = bio.glow_g; tl.glow_b = bio.glow_b;
                        std::string b;
                        if (real_torch) {
                            tl.radius    = 300.0f + rng_float(drng, 0.0f, 100.0f);
                            tl.intensity = 2.0f;  // vanilla confirmed
                            b = build_torch_object(tl, "torch" + std::to_string(++torch_idx));
                        } else {
                            // Soft ambient glow patch — not a full torch
                            tl.radius    = 180.0f + rng_float(drng, 0.0f, 80.0f);
                            tl.intensity = 1.1f;
                            tl.glow_a    = 0.35f;
                            b = build_glow_light(tl, "glow" + std::to_string(++glow_idx));
                        }
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }

                // ── NEW: Background parallax deco scatter on main layer ──────
                // Vanilla-confirmed depth ranges (Tier 2 mid-BG: -112..-49):
                //   forest_part1: treewall at -111/-76, bg bush at -73/-49
                //   grove_part1:  grove_pole at -77, grove_hang at -77
                // We scatter large-scale decos across Tier 1+2: -160..-49
                if (opt.scatter_background_decos) {
                    int nbg = 0; while (nbg < 4 && bio.deco_background[nbg][0]) ++nbg;
                    if (nbg > 0) {
                        const float sp = std::clamp(30.0f / (bio.tree_density * dens + 1e-3f), 120.0f, 600.0f);
                        scatter_pass(sp, 0xBEEFu, [&](float x, float gy) {
                            // Occasional cluster of 1-3 large bg trees
                            const int cnt = 1 + (int)(rng_float(drng, 0.0f, 1.0f) < 0.35f ? 1 : 0)
                                              + (int)(rng_float(drng, 0.0f, 1.0f) < 0.15f ? 1 : 0);
                            for (int ci = 0; ci < cnt; ++ci) {
                                Deco d;
                                d.x     = x + rng_float(drng, -80.0f, 80.0f);
                                d.y     = gy + rng_float(drng, -10.0f, 20.0f);
                                // Tier 1 (-160..-113): far distant; Tier 2 (-112..-49): mid-bg
                                d.z     = (rng_float(drng, 0.0f, 1.0f) < 0.40f)
                                        ? rng_float(drng, -160.0f, -113.0f)   // Tier 1 (40%)
                                        : rng_float(drng, -112.0f, -49.0f);   // Tier 2 (60%)
                                // Larger scale for farther-back decos
                                d.scale = opt.randomize_deco_scale
                                    ? ((d.z < -112.0f) ? (1.6f + rng_float(drng, 0.0f, 1.2f))
                                                       : (1.0f + rng_float(drng, 0.0f, 0.8f)))
                                    : 1.0f;
                                d.rot_y = (opt.randomize_deco_rotation && rng_float(drng, 0.0f, 1.0f) < 0.5f) ? -1.5707963f : 0.0f;
                                d.pod   = bio.deco_background[local_hash_uint((uint32_t)(deco_idx * 479 + 31)) % (unsigned)nbg];
                                const std::string b = build_deco_object(d, "bg_deco" + std::to_string(++deco_idx));
                                if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                            }
                        });
                    }
                }

                // ── NEW: Foreground parallax deco scatter on main layer ───────
                // Vanilla-confirmed Tier 5 depth range: +22 .. +71
                //   forest_part1: fg bush at +28/+65/+71, stoneblock +47
                //   grove_part1:  grove_hang at +71, torch at +48
                // We scatter small-scale rocks/shrubs across Tier 5: +22..+71
                if (opt.scatter_foreground_decos) {
                    int nfg = 0; while (nfg < 4 && bio.deco_foreground[nfg][0]) ++nfg;
                    if (nfg > 0) {
                        const float sp = std::clamp(28.0f / (bio.rock_density * dens + 1e-3f), 100.0f, 500.0f);
                        scatter_pass(sp, 0xF0E1u, [&](float x, float gy) {
                            Deco d;
                            d.x     = x + rng_float(drng, -40.0f, 40.0f);
                            d.y     = gy + rng_float(drng, -5.0f, 5.0f);
                            // Tier 5 (vanilla): +22 to +71
                            d.z     = rng_float(drng, 22.0f, 71.0f);
                            d.scale = opt.randomize_deco_scale ? (0.4f + rng_float(drng, 0.0f, 0.5f)) : 1.0f;
                            d.rot_y = 0.0f;
                            d.pod   = bio.deco_foreground[local_hash_uint((uint32_t)(deco_idx * 233 + 17)) % (unsigned)nfg];
                            const std::string b = build_deco_object(d, "fg_deco" + std::to_string(++deco_idx));
                            if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                        });
                    }
                }

            } else {
                // ── Non-walkable layer (bg/fg): sparse deco scatter only ─────
                int ntree = 0; while (ntree < 6 && bio.deco_trees[ntree][0]) ++ntree;
                if (ntree > 0) {
                    const float sp = std::clamp(40.0f / (bio.tree_density * dens + 1e-3f), 200.0f, 1200.0f);
                    scatter_pass(sp, 0xA99Bu, [&](float x, float gy) {
                        Deco d;
                        d.x     = x + rng_float(drng, -60.0f, 60.0f);
                        d.y     = gy + rng_float(drng, -5.0f, 10.0f);
                        d.z     = layer.z + rng_float(drng, -15.0f, 15.0f);
                        d.scale = opt.randomize_deco_scale
                            ? ((layer.z < -50.0f) ? (1.2f + rng_float(drng, 0.0f, 0.8f))
                                                  : (0.4f + rng_float(drng, 0.0f, 0.4f)))
                            : 1.0f;
                        d.rot_y = (opt.randomize_deco_rotation && rng_float(drng, 0, 1.0f) < 0.5f) ? -1.5707963f : 0.0f;
                        d.pod   = bio.deco_trees[local_hash_uint((uint32_t)(deco_idx * 131 + 7)) % (unsigned)ntree];
                        const std::string b = build_deco_object(d, "deco_bg" + std::to_string(++deco_idx));
                        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
                    });
                }
            }
        } // end platform strips
    } // end layer loop

    // ── Water ─────────────────────────────────────────────────────────────────
    if (opt.add_water && bio.water_front[3] > 0.01f) {
        const float water_top = floor_y - 80.0f;
        Water w;
        w.rect[0] = -half_w - 120.0f;
        w.rect[1] = water_top;
        w.rect[2] = opt.width + 240.0f;
        w.rect[3] = 50.0f;
        memcpy(w.front_rgba,   bio.water_front,   sizeof(float) * 4);
        memcpy(w.surface_rgba, bio.water_surface, sizeof(float) * 4);
        const std::string b = build_water_object(w, "water");
        if (!b.empty()) { scene.write_bytes_field(1, local_extract_object_bytes(b)); ++r.objects; }
    }

    // ── Portal hub (optional) ─────────────────────────────────────────────────
    // Portals anchor at the scene EXTREMES (leftmost/rightmost walkable strip):
    // the scene-transition portal at one extreme and the one-per-scene
    // world-travel portal MODEL at the other, each ringed with a per-biome
    // brick/stone ruin cluster, sign, shrubs and torches.
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
        // Scene-transition portal at the first extreme.
        if (portal_extreme_anchor(walkable_plats[p0], e0, px, py)) {
            PortalHubOptions ph;
            ph.x = px; ph.y = py + 4.0f;
            ph.facing = -e0;   // spawn + sign face toward the scene interior
            ph.destination = opt.portal_destination;
            ph.portal_name = "portal";
            const auto hub = build_portal_hub(ph, opt.biome, drng);
            for (const auto& bytes : hub) {
                scene.write_bytes_field(1, local_extract_object_bytes(bytes));
                ++r.objects;
            }
        }
        // World-travel portal model at the opposite extreme.
        bool ok2 = false;
        if (walkable_plats.size() == 1) {
            ok2 = portal_extreme_anchor(walkable_plats[p0], e1, px, py);
        } else {
            ok2 = portal_extreme_anchor(walkable_plats[p1], e1, px, py)
               || portal_extreme_anchor(walkable_plats[p0], e1, px, py);
        }
        if (ok2) {
            PortalHubOptions ph;
            ph.x = px; ph.y = py + 4.0f;
            ph.facing = -e1;   // decos face toward the scene interior
            ph.destination = opt.portal_destination;
            ph.portal_name = "portal";
            const auto hub = build_game_portal_hub(ph, opt.biome, drng);
            for (const auto& bytes : hub) {
                scene.write_bytes_field(1, local_extract_object_bytes(bytes));
                ++r.objects;
            }
        }
    }

    // ── Spawn ─────────────────────────────────────────────────────────────────
    // Find the walkable platform whose X range contains x=0 and place the
    // spawn on its top (v1 parity — v2 previously used the global max Y which
    // could strand the hero on a distant peak).
    float sx = 0.0f, sy = (maxy > -1e8f ? maxy : 0.0f) + 56.0f;
    {
        float best_top = -1e9f;
        for (const auto& pp : walkable_plats) {
            std::vector<Vec2> poly = pp.polygon;
            if (poly.size() < 3)
                poly = make_rect_polygon(pp.rect[0], pp.rect[1], pp.rect[2], pp.rect[3]);
            float min_x = 1e9f, max_x = -1e9f, top = -1e9f;
            for (const auto& v : poly) {
                min_x = std::min(min_x, v.x);
                max_x = std::max(max_x, v.x);
                top   = std::max(top, v.y);
            }
            if (0.0f >= min_x - 12.0f && 0.0f <= max_x + 12.0f)
                best_top = std::max(best_top, top);
        }
        if (best_top > -1e8f) sy = best_top + 56.0f;
    }
    scene.write_bytes_field(1, local_extract_object_bytes(
        build_spawn_object("spawn_default", sx, sy, 1)));
    ++r.objects;

    // ── ObjectLibrary (required for TemplateName refs to resolve in-game) ────
    // ObjectLibrary — required so the game resolves TemplateName references.
    // Import list matches default_scene_imports() from scene_generator.cpp.
    static const std::vector<std::string> kImports = {
        "caves_stuff", "collectibles", "florennum_stuff", "forest",
        "game_common", "grovestuff", "lights", "monsters", "npc",
        "plains_stuff", "platforms", "playground", "programs", "rocks",
        "scriptarea", "traps_stuffs", "trash", "woodkeep_stuff", "woods",
    };
    scene.write_bytes_field(2, build_object_library(opt.scene_name, kImports));

    // ── Camera follow shape (vanilla camera controller) ───────────────────────
    // 'camera_follow_y_shape' (scriptarea.scl) makes the game camera pan
    // vertically with the hero while he is inside its rect; its built-in
    // program keeps the shape glued to hero.y+100. One copy scaled to cover
    // the whole walkable band gives smooth vertical camera everywhere.
    if (opt.emit_camera_shapes && top_min < 1e30f && top_max > -1e30f) {
        const float scene_w = (maxx - minx) + 2.0f * 220.0f;
        const float scale = std::max(1.0f, scene_w / 1000.0f);   // base rect 1000x500
        const float cx = (minx + maxx) * 0.5f;
        const float cy = (top_min + top_max) * 0.5f;
        proto::Writer obj;
        obj.write_string_field(1, "camera_follow_y_shape");     // TemplateName
        obj.write_string_field(2, "camera_follow");             // Identifier
        obj.write_nested_field(4, local_v2(cx, cy));
        obj.write_float_field(5, 0.0f);
        obj.write_float_field(6, 0.0f);
        obj.write_float_field(7, scale);
        obj.write_nested_field(8, local_rect(-500.0f * scale, -250.0f * scale,
                                             1000.0f * scale, 500.0f * scale));
        obj.write_varint_field(9, 0);
        proto::Writer wrapped;
        wrapped.write_bytes_field(1, obj.to_string());
        scene.write_bytes_field(1, local_extract_object_bytes(wrapped.to_string()));
        ++r.objects;
    }

    // ── Root Bounds ───────────────────────────────────────────────────────────
    // Scene Bounds is what the game camera clamps to (Camera.ResetFocus /
    // CameraController). Derive it from the WALKABLE band: X covers the
    // platforms, Y spans the walkable top profile with headroom up/down.
    // (The old AABB reached to the heightfield base far below the ground, so
    // the default camera could frame empty space and the scene looked "broken".)
    if (maxx <= minx || maxy <= miny) { r.error = "v2: empty scene AABB"; return r; }
    if (top_min >= 1e30f) { top_min = miny; top_max = maxy; }  // no walkable strip
    const float pad  = 220.0f;
    const float head_up = 350.0f;    // look above the highest ground
    const float head_dn = 650.0f;    // look below the lowest ground
    const float bx = minx - pad;
    const float by = top_min - head_dn;
    const float bw = (maxx - minx) + 2.0f * pad;
    const float bh = (top_max + head_up) - by;
    scene.write_bytes_field(3, build_bounds_payload(bx, by, bw, bh));

    r.bounds[0] = bx; r.bounds[1] = by; r.bounds[2] = bw; r.bounds[3] = bh;
    r.scene_bytes = scene.to_string();
    return r;
}

} // namespace v2
} // namespace sgen
