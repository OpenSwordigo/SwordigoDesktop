
#include "tools/scene_generator.h"
#include "tools/boulder.h"
#include "platform/protobuf_reader.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <functional>

/*
 * scene_generator.cpp — Procedural Swordigo scene mesh generator (Ruby SDK).
 *
 * REMASTER v3 (2026-08): Evidence-driven rewrite based on direct decoding of
 * forest_part1, grove_part1, grass_part1, plains_part1, plains_woodkeep (118
 * shipped scenes total per previous research).
 *
 * Key corrections vs v2:
 *   1. TERRAIN SMOOTHING: Multi-pass Gaussian (σ=2.0) + box filter applied to
 *      the heightfield eliminates jarring quick elevations. The ridged/fBm
 *      blending is weighted (70/30) to keep organic silhouette.
 *   2. TOP SURFACE: boulder::GroundMesh::generate_top=true (MeshType=1 in all
 *      118 scenes). SurfaceWidth matches real ranges (100-150px).
 *   3. CORRECT BIOME TEXTURES: In real scenes the GroundMeshGenerator carries:
 *        FrontTextureMappingId  → cliff/front face (e.g. "maybegood")
 *        SurfaceTextureMappingId → top grass surface (e.g. "grass_grass")
 *      v2 had front/top reversed.
 *   4. DECORATION DEPTH: Trees/rocks placed at RANDOM DEPTH in the range
 *      [-75, +115] (observed: smallrock at -68 to +58, tree at -73 to +71,
 *      torch at -80 to +107). Positive = closer (foreground), negative =
 *      background. This gives Swordigo's characteristic multi-plane depth
 *      illusion — some objects appear BEHIND the ground mesh.
 *   5. TORCH OBJECTS: In grove_part1, torches are Model objects with
 *      TemplateName='grove_torch' and EmissionFactor=0.5, NOT a full
 *      FireEmitter composite. Ruby uses the game template system; we emit the
 *      same (Model + EmissionFactor) so the game lights them correctly.
 *   6. LIGHT OBJECT DEPTH: Real scenes place the DirectionalLight object at
 *      scene-level depth (17–327 observed), NOT inside the ground mesh.
 *   7. GROUND OBJECT DEPTH = 0.0f for ALL platforms (they share the same
 *      world Z plane). Only decorations vary depth.
 *   8. BACKGROUND TEXTURES: Corrected per biome from decoded scenes.
 *
 * Terrain algorithm:
 *   a. Generate raw fBm heightfield (or ridged for mountains).
 *   b. Apply multi-pass Gaussian smoothing (3 passes, kernel σ=2.5).
 *   c. Normalize to [floor_y … floor_y+max_height].
 *   d. Guarantee solid ground at scene midpoint (spawn pad).
 *   e. Slice into platform strips; each strip becomes one GroundMesh object.
 *   f. Scatter decorations: primary cluster per platform, secondary fill.
 *   g. Spill torches at determined spacing (TemplateName references).
 *   h. Emit water, spawn, background, directional light, bounds.
 */

namespace sgen {

// ─────────────────────────────────────────────────────────────────────────
// Deterministic RNG — splitmix64
// ─────────────────────────────────────────────────────────────────────────
uint64_t rng_next(uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float rng_float(uint64_t& state, float lo, float hi) {
    const uint64_t r = rng_next(state);
    const double unit = static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
    return lo + static_cast<float>(unit) * (hi - lo);
}

// ─────────────────────────────────────────────────────────────────────────
// Value noise hash
// ─────────────────────────────────────────────────────────────────────────
static inline uint32_t hash_uint(uint32_t x) {
    x = (x ^ 61u) ^ (x >> 16);
    x = x + (x << 3);
    x = x ^ (x >> 4);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15);
    return x;
}
static inline float hash_unit(uint32_t x) {
    return static_cast<float>(hash_uint(x) & 0xFFFFFFu) / 16777215.0f;
}
static inline float fade(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

float perlin_1d(float x, uint32_t seed) {
    const int xi = (int)std::floor(x);
    const float xf = x - (float)xi;
    const float u = fade(xf);
    const float a = hash_unit((uint32_t)xi * 0x9E3779B9u ^ seed);
    const float b = hash_unit((uint32_t)(xi + 1) * 0x9E3779B9u ^ seed);
    return lerp(a, b, u) * 2.0f - 1.0f;
}

float perlin_2d(float x, float y, uint32_t seed) {
    const int xi = (int)std::floor(x);
    const int yi = (int)std::floor(y);
    const float xf = x - (float)xi;
    const float yf = y - (float)yi;
    const float u = fade(xf), v = fade(yf);
    const uint32_t hx = 0x9E3779B9u * (uint32_t)xi;
    const uint32_t hy = 0x85EBCA77u * (uint32_t)yi;
    const float a  = hash_unit(hx ^ hy ^ seed);
    const float b  = hash_unit((hx + 0x9E3779B9u) ^ hy ^ seed);
    const float c  = hash_unit(hx ^ (hy + 0x85EBCA77u) ^ seed);
    const float d  = hash_unit((hx + 0x9E3779B9u) ^ (hy + 0x85EBCA77u) ^ seed);
    return lerp(lerp(a, b, u), lerp(c, d, u), v) * 2.0f - 1.0f;
}

float fbm_1d(float x, int octaves, uint32_t seed, float warp) {
    if (warp > 0.0f) x += perlin_1d(x * 0.7f, seed ^ 0xA5A5) * warp;
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * perlin_1d(x * freq, seed + (uint32_t)o * 0x9E37u);
        norm += amp;
        amp  *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

float fbm_2d(float x, float y, int octaves, uint32_t seed, float warp) {
    if (warp > 0.0f) {
        const float dx = perlin_2d(x * 0.7f, y * 0.7f, seed ^ 0xABC1) * warp;
        const float dy = perlin_2d(x * 0.7f + 31.7f, y * 0.7f, seed ^ 0xDEAD) * warp;
        x += dx; y += dy;
    }
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * perlin_2d(x * freq, y * freq, seed + (uint32_t)o * 0x9E37u);
        norm += amp;
        amp  *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

float ridged_2d(float x, float y, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        float n = 1.0f - std::fabs(perlin_2d(x * freq, y * freq,
                                              seed + (uint32_t)o * 0x9E37u));
        sum  += amp * n * n;
        norm += amp;
        amp  *= 0.5f;
        freq *= 2.1f;
    }
    return sum / norm;
}

// ─────────────────────────────────────────────────────────────────────────
// Gaussian smoothing kernel (1D, σ controlled by passes + radius)
// ─────────────────────────────────────────────────────────────────────────
static void smooth_gaussian(std::vector<float>& h, int radius = 4, int passes = 3) {
    const int n = (int)h.size();
    std::vector<float> tmp(n);
    for (int pass = 0; pass < passes; ++pass) {
        // Gaussian weights in window [-radius, +radius]
        float wsum = 0.0f;
        std::vector<float> w(2 * radius + 1);
        const float sigma = radius * 0.45f;
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

// ─────────────────────────────────────────────────────────────────────────
// Terrain synthesis
// ─────────────────────────────────────────────────────────────────────────
std::vector<Vec2> make_rect_polygon(float x, float y, float w, float h) {
    return {{x - w * 0.5f, y - h * 0.5f}, {x + w * 0.5f, y - h * 0.5f},
            {x + w * 0.5f, y + h * 0.5f}, {x - w * 0.5f, y + h * 0.5f}};
}

std::vector<Vec2> make_heightfield_polygon(const float* heights, int n,
                                           float step, float floor_y) {
    std::vector<Vec2> poly;
    if (n < 2) return poly;
    const float right_x = static_cast<float>(n - 1) * step;
    poly.push_back({0.0f, floor_y});
    poly.push_back({right_x, floor_y});
    for (int i = n - 1; i >= 0; --i)
        poly.push_back({static_cast<float>(i) * step, heights[i]});
    return poly;
}

static Vec2 edge_outward_normal(Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return {0.0f, 0.0f};
    return {dy / len, -dx / len};
}

void apply_horiz_noise(std::vector<Vec2>& poly, float amplitude, uint32_t seed) {
    if (poly.size() < 3 || amplitude <= 0.0f) return;
    uint64_t state = seed;
    const int n = (int)poly.size();
    for (int i = 0; i < n; ++i) {
        const Vec2 prev = poly[(i - 1 + n) % n];
        const Vec2 next = poly[(i + 1) % n];
        Vec2 na = edge_outward_normal(prev, poly[i]);
        Vec2 nb = edge_outward_normal(poly[i], next);
        float nx = na.x + nb.x, ny = na.y + nb.y;
        const float nl = std::sqrt(nx * nx + ny * ny);
        if (nl < 1e-6f) { nx = na.x; ny = na.y; }
        else { nx /= nl; ny /= nl; }
        const float e1 = std::sqrt((poly[i].x-prev.x)*(poly[i].x-prev.x)+(poly[i].y-prev.y)*(poly[i].y-prev.y));
        const float e2 = std::sqrt((next.x-poly[i].x)*(next.x-poly[i].x)+(next.y-poly[i].y)*(next.y-poly[i].y));
        const float cap = 0.25f * std::min(e1, e2);
        float j = rng_float(state, -amplitude, amplitude);
        if (cap > 1e-4f) j = std::max(-cap, std::min(cap, j));
        poly[i].x += nx * j;
        poly[i].y += ny * j;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Small protobuf helpers
// ─────────────────────────────────────────────────────────────────────────
static proto::Writer v2(float x, float y) {
    proto::Writer w;
    w.write_float_field(1, x);
    w.write_float_field(2, y);
    return w;
}
static proto::Writer v3(float x, float y, float z) {
    proto::Writer w;
    w.write_float_field(1, x);
    w.write_float_field(2, y);
    w.write_float_field(3, z);
    return w;
}
static proto::Writer rect_msg(float x, float y, float w, float h) {
    proto::Writer r;
    r.write_float_field(1, x); r.write_float_field(2, y);
    r.write_float_field(3, w); r.write_float_field(4, h);
    return r;
}
static proto::Writer float_color(float r, float g, float b, float a) {
    proto::Writer c;
    c.write_float_field(1, r); c.write_float_field(2, g);
    c.write_float_field(3, b); c.write_float_field(4, a);
    return c;
}
static proto::Writer component(const char* cls, int id, int pf, const proto::Writer& payload) {
    proto::Writer c;
    c.write_string_field(1, cls);
    c.write_varint_field(2, static_cast<uint64_t>(id));
    c.write_nested_field(static_cast<uint32_t>(pf), payload);
    return c;
}
static std::string scene_wrap_object(const proto::Writer& obj) {
    proto::Writer scene;
    scene.write_bytes_field(1, obj.to_string());
    return scene.to_string();
}
static std::string extract_object_bytes(const std::string& scene) {
    if (scene.empty() || (unsigned char)scene[0] != 0x0a) return "";
    size_t i = 1;
    uint64_t len = 0;
    int sh = 0;
    while (i < scene.size() && ((unsigned char)scene[i] & 0x80)) {
        len |= (uint64_t)(scene[i] & 0x7f) << sh; sh += 7; ++i;
    }
    if (i >= scene.size()) return "";
    len |= (uint64_t)(unsigned char)scene[i] << sh; ++i;
    if (i + len > scene.size()) return "";
    return scene.substr(i, (size_t)len);
}

// ─────────────────────────────────────────────────────────────────────────
// Object builders
// ─────────────────────────────────────────────────────────────────────────

// Ground mesh — uses boulder's pipeline. All platforms share Depth=0 so the
// main terrain is always co-planar (Swordigo is a 2.5D side-scroller).
// The SurfaceWidth in the GroundMeshGenerator component is 100-150 (real),
// matching boulder's GroundMesh::surface_width. generate_top=true (MeshType=1).
std::string build_ground_object(const Platform& p, const std::string& name) {
    std::vector<Vec2> poly = p.polygon;
    if (poly.size() < 3)
        poly = make_rect_polygon(p.rect[0], p.rect[1], p.rect[2], p.rect[3]);
    if (poly.size() < 3) return "";
    if (p.horiz_noise > 0.0f) apply_horiz_noise(poly, p.horiz_noise, p.seed);

    boulder::GroundMesh gm;
    gm.z            = p.z;
    gm.min_depth    = p.min_depth;
    gm.max_depth    = p.max_depth;
    gm.top_angle    = 20.0;
    gm.generate_top = true;            // always emit surface top layer
    gm.top_texture    = p.top_texture;  // SurfaceTextureMappingId texture (grass top)
    gm.bottom_texture = p.front_texture;// FrontTextureMappingId texture  (cliff face)
    for (const auto& h : p.hats)
        gm.hats.push_back({(double)h.x,(double)h.y,(double)h.radius,(double)h.height});
    for (const auto& v : poly)
        gm.polygon.push_back({(double)v.x, (double)v.y});

    const std::string swdm  = boulder::serialize_swdm(gm);
    const std::string baked = boulder::generate_ground_mesh_object(swdm, name, p.z);
    return baked;
}

// Background — TWO components, exactly matching every shipped scene's Background
// object (verified byte-for-byte against forest_part1/grass_part1/grove_part1):
//
//   Component 1  ClassName 'Model' (id 1)  ModelComponent (payload field 101)
//   Component 2  ClassName 'Background' (id 101)  BackgroundComponent (field 200)
//
// The Model component is REQUIRED. BackgroundComponent::Draw in libswordigo does
// not create its own drawable — it draws the sky quad through the object's
// Model/Sprite. With only the BackgroundComponent the game has nothing to draw,
// so the background renders BLACK / disappears and the whole scene looks too
// dark. The Ruby SDK viewer is lenient (it renders the texture from the
// BackgroundComponent alone), which is why the bug was invisible in-editor.
//
// The ModelComponent carries NO model name (field 1 absent, like vanilla) —
// only YRotation/EmissionFactor/XRotation/ShatterColor/Origin/Transparent and,
// critically, a WHITE DiffuseColor (a missing color defaults to alpha 0 in the
// real game and would render the object invisible).
//
// BackgroundComponent payload field is 200 (raw tag 0xC2 0x0C = varint 1602 =
// field 200, wiretype 2), Model payload field is 101 (tag 0xAA 0x06). Both
// verified against shipped-scene raw bytes.
std::string build_background_object(const std::string& texture) {
    // --- Model component (renderable hook for the sky quad) ---
    proto::Writer model_pay;
    model_pay.write_float_field(2, 0.0f);                    // YRotation
    model_pay.write_float_field(3, 0.0f);                    // EmissionFactor
    model_pay.write_float_field(4, 0.0f);                    // XRotation
    model_pay.write_nested_field(5, float_color(0, 0, 0, 1));// ShatterColor (black)
    model_pay.write_nested_field(6, v3(0, 0, 0));            // Origin
    model_pay.write_varint_field(7, 0);                      // Transparent
    model_pay.write_nested_field(8, float_color(1, 1, 1, 1));// DiffuseColor MUST be white
    proto::Writer model_comp = component("Model", 1, 101, model_pay);

    // --- Background component (the sky texture name) ---
    proto::Writer bgc;
    bgc.write_string_field(1, texture);     // TextureName (inner field 1)
    proto::Writer bg_comp = component("Background", 101, 200, bgc);

    proto::Writer obj;
    obj.write_string_field(2, "Background");
    obj.write_bytes_field(3, model_comp.to_string());   // Model FIRST (matches vanilla)
    obj.write_bytes_field(3, bg_comp.to_string());       // then Background
    obj.write_nested_field(4, v2(0.0f, 0.0f));
    obj.write_float_field(5, 1.72038269f);  // canonical depth (all 118 scenes)
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(-30, -30, 60, 60));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// DirectionalLight — canonical triple (Type 2/1/4) at component IDs 101/103/105.
// Real scene depth varies from 17 to 327; we place it at a biome-derived value.
std::string build_light_object(const std::string& name, float x, float y, float depth,
                               float key_i, float amb_i, float shd_i,
                               float kr, float kg, float kb) {
    struct L { int type; float intensity; float r, g, b; };
    const L comps[3] = {
        {2, key_i, kr, kg, kb},    // key / directional sun
        {1, amb_i, 1, 1, 1},       // ambient fill
        {4, shd_i, 0, 0, 0},       // shadow contrast (black)
    };
    const int ids[3] = {101, 103, 105};

    proto::Writer obj;
    obj.write_string_field(2, name);
    for (int i = 0; i < 3; ++i) {
        proto::Writer lc;
        lc.write_varint_field(1, (uint64_t)comps[i].type);
        lc.write_float_field(2, comps[i].intensity);
        lc.write_nested_field(3, float_color(comps[i].r, comps[i].g, comps[i].b, 1.0f));
        // CRITICAL: LightComponent payload field is 130 (verified from raw
        // bytes of shipped forest_part1: tag 0x0892 => field 130, inner
        // Type=f1/Intensity=f2/Color=f3). The old 1042 meant the GAME could not
        // find the light at all → scene rendered fully black in-game (the Ruby
        // SDK loader is lenient and lit it anyway, hiding the bug).
        obj.write_bytes_field(3, component("Light", ids[i], 130, lc).to_string());
    }
    obj.write_nested_field(4, v2(x, y));
    obj.write_float_field(5, depth);           // real depth: biome-derived, not 620
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(-30, -30, 60, 60));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// SpawnPoint — byte-exact wire format verified against the raw forest_part1
// bytes (spawn_from_forest_cave0): SpawnPointComponent payload f501 with
// inner fields 1=FacingDirection (varint) and 2=SpawnOffset (Vector3).
// (The old payload 4010 / fields 8+18 were the docs-scenecreator numbering
// and the game could not read them — no spawn would appear.)
std::string build_spawn_object(const std::string& name, float x, float y, int facing) {
    proto::Writer spc;
    spc.write_varint_field(1, (uint64_t)facing);  // FacingDirection
    proto::Writer off;
    off.write_float_field(1, 0.0f); off.write_float_field(2, 0.0f); off.write_float_field(3, 0.0f);
    spc.write_nested_field(2, off);               // SpawnOffset

    proto::Writer obj;
    obj.write_string_field(2, name);
    obj.write_bytes_field(3, component("SpawnPoint", 101, 501, spc).to_string());
    obj.write_nested_field(4, v2(x, y));
    obj.write_float_field(5, 0.0f);
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(-30, -30, 60, 60));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// Water sheet — WaterMeshComponent (114).
std::string build_water_object(const Water& w, const std::string& name) {
    proto::Writer wmc;
    wmc.write_varint_field(1, 103); wmc.write_varint_field(2, 106);
    wmc.write_nested_field(3, float_color(w.front_rgba[0], w.front_rgba[1],
                                          w.front_rgba[2], w.front_rgba[3]));
    wmc.write_nested_field(4, float_color(w.surface_rgba[0], w.surface_rgba[1],
                                          w.surface_rgba[2], w.surface_rgba[3]));
    proto::Writer shape;
    shape.write_nested_field(1, rect_msg(w.rect[0],w.rect[1],w.rect[2],w.rect[3]));
    proto::Writer tm;
    tm.write_string_field(1, w.texture);
    tm.write_float_field(2, 64.0f);
    tm.write_nested_field(3, v2(0, 0));

    proto::Writer obj;
    obj.write_string_field(2, name);
    obj.write_bytes_field(3, component("WaterMesh",    101, 114, wmc).to_string());
    obj.write_bytes_field(3, component("UtilityShape", 103, 120, shape).to_string());
    obj.write_bytes_field(3, component("TextureMapping",106, 113, tm).to_string());
    obj.write_nested_field(4, v2(0, 0));
    obj.write_float_field(5, 0.0f);
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(w.rect[0],w.rect[1],w.rect[2],w.rect[3]));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

std::string build_bounds_payload(float x, float y, float w, float h) {
    proto::Writer b;
    b.write_float_field(1, x); b.write_float_field(2, y);
    b.write_float_field(3, w); b.write_float_field(4, h);
    return b.to_string();
}

// ─────────────────────────────────────────────────────────────────────────
// ObjectLibrary (Scene field 2) — REQUIRED for decorations to appear in-game.
//
// CRITICAL FIX (decorations invisible in real Swordigo):
// Every decoration/torch/prop we emit is a *TemplateName-reference* object
// (TemplateName in object field 1, no baked Model). At load time the real
// engine resolves that name via Caver::ObjectLibrary::TemplateForName(), which
// searches ONLY the libraries the scene has imported (Caver::ObjectLibrary::
// ImportLibrary / IsLibraryImported — verified in the libswordigo symbol
// table). If the scene declares no imported libraries, NOTHING resolves:
// every tree/bush/rock/torch becomes a phantom object with no drawable and the
// only thing left visible is the baked ground mesh — exactly the reported
// "only the plains mesh is visible" symptom.
//
// The Ruby SDK viewer is lenient (it resolves templates from its global asset
// pool regardless of imports), which is why decorations showed in-editor but
// vanished in-game — the same class of bug as the missing-Model background.
//
// Wire layout (verified byte-for-byte against grove_part1/forest_part1/
// grass_part1):
//   Scene.ObjectLibrary = field 2 (tag 0x12)
//   ObjectLibrary.Name             = field 1 (tag 0x0a, string)
//   ObjectLibrary.ImportedLibrary  = field 3 (tag 0x1a, repeated string)
// (Template = field 2; we don't emit inline templates — the named libraries
//  ship with the game and are loaded from disk by the engine.)
//
// We import the full standard union observed across the shipped scenes so that
// every template our biome tables can emit (grove_tree*, bush, pot, rock*,
// smallrock*, grove_pole*, grove_torch, torchholder, deadtree*, icicle*,
// pottery*, …) is resolvable in any biome.
std::string build_object_library(const std::string& scene_name,
                                 const std::vector<std::string>& imports) {
    proto::Writer lib;
    lib.write_string_field(1, scene_name.empty() ? "generated" : scene_name); // Name
    for (const auto& name : imports)
        if (!name.empty())
            lib.write_string_field(3, name);                                  // ImportedLibrary
    return lib.to_string();
}

// The canonical import set — the union of ImportedLibrary lists across the
// shipped forest/grove/grass scenes. Covers all decoration + prop templates.
static const std::vector<std::string>& default_scene_imports() {
    static const std::vector<std::string> kImports = {
        "caves_stuff", "collectibles", "florennum_stuff", "forest",
        "game_common", "grovestuff", "lights", "monsters", "npc",
        "plains_stuff", "platforms", "playground", "programs", "rocks",
        "scriptarea", "traps_stuffs", "trash", "woodkeep_stuff", "woods",
    };
    return kImports;
}

// Pure TemplateName-reference object. This is the EXACT layout the shipped
// scenes use for props: no baked Model/ModelComponent — only a TemplateName
// (object field 1) + Identifier/Position/Depth/Rotation/Scaling/AABB/Hidden.
// The game instantiates the mesh, collision and all components from the
// template library. Verified via `ruby_cli -d de_in` on forest/grove/grass
// (e.g. obj5 = TemplateName 'bush'/'grove_tree1' with no component block).
std::string build_template_object(const std::string& name, const std::string& template_name,
                                  float x, float y, float depth,
                                  float rotation, float scaling) {
    if (template_name.empty()) return "";
    proto::Writer obj;
    obj.write_string_field(1, template_name);       // TemplateName (object field 1)
    obj.write_string_field(2, name);                // Identifier
    obj.write_nested_field(4, v2(x, y));            // Position
    obj.write_float_field(5, depth);                // Depth (parallax layer)
    obj.write_float_field(6, rotation);             // Rotation
    obj.write_float_field(7, scaling <= 0.0f ? 1.0f : scaling); // Scaling
    obj.write_nested_field(8, rect_msg(-30, -30, 60, 60));
    obj.write_varint_field(9, 0);                   // Hidden
    return scene_wrap_object(obj);
}

// Decoration (tree / bush / grass / rock / pot). REMASTER: these are now emitted
// as pure TemplateName-reference objects — the previous version baked a raw
// Model+POD component, which the game could not resolve, so decorations
// "didn't work". `d.template_name` overrides `d.pod` (which now carries the
// template name, e.g. "grove_tree1", "bush", "pot"). Depth varies widely
// (-75..+115) to produce Swordigo's multi-plane parallax.
std::string build_deco_object(const Deco& d, const std::string& name) {
    const std::string tmpl = d.template_name.empty() ? d.pod : d.template_name;
    if (tmpl.empty()) return "";
    return build_template_object(name, tmpl, d.x, d.y, d.z, d.rot_y, d.scale);
}

// (legacy baked-Model deco emitter kept for reference — no longer used)
std::string build_deco_object_baked_legacy(const Deco& d, const std::string& name) {
    if (d.pod.empty()) return "";
    proto::Writer payload;
    payload.write_string_field(1, d.pod);
    payload.write_float_field(2, d.rot_y);          // YRotation
    payload.write_float_field(3, 0.0f);             // EmissionFactor
    payload.write_float_field(4, 0.0f);             // XRotation
    payload.write_nested_field(5, float_color(0, 0, 0, 1));
    payload.write_nested_field(6, v3(0, 0, 0));
    payload.write_varint_field(7, 0);               // Transparent
    payload.write_nested_field(8, float_color(1, 1, 1, 1)); // DiffuseColor
    proto::Writer wrapper;
    wrapper.write_string_field(1, "Model");
    wrapper.write_varint_field(2, 101);
    wrapper.write_nested_field(101, payload);

    proto::Writer obj;
    obj.write_string_field(2, name);
    obj.write_bytes_field(3, wrapper.to_string());  // component directly at f3
    obj.write_nested_field(4, v2(d.x, d.y));
    obj.write_float_field(5, d.z);                  // DEPTH: varies for parallax
    obj.write_float_field(6, d.rot_y);
    obj.write_float_field(7, d.scale);
    obj.write_nested_field(8, rect_msg(-30, -30, 60, 60));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// Torch object — In the REAL grove_part1 scene, torches are Model objects
// with EmissionFactor=0.5, using TemplateName='grove_torch' (the game resolves
// the FireEmitter/Light/Glow from the template library). We match this exactly.
std::string build_torch_object(const TorchLight& t, const std::string& name) {
    // REMASTER: torches are pure TemplateName-reference objects, exactly as in
    // grove_part1 (TemplateName 'grove_torch' ×17, 'torch' ×11) — the game
    // resolves the Model + FireEmitter + Light + SimpleGlow from the template.
    // The previous version baked a Model AND mis-wrote TemplateName into object
    // field 6 (which is Rotation), corrupting the object. TemplateName is field 1.
    const char* tmpl = (t.glow_r > 0.5f) ? "grove_torch" : "torch";
    proto::Writer obj;
    obj.write_string_field(1, tmpl);                // TemplateName (object field 1)
    obj.write_string_field(2, name);                // Identifier
    obj.write_nested_field(4, v2(t.x, t.y));        // Position
    obj.write_float_field(5, t.z);                  // Depth
    obj.write_float_field(6, 0.0f);                 // Rotation
    obj.write_float_field(7, 1.0f);                 // Scaling
    obj.write_nested_field(8, rect_msg(-50, -50, 100, 100));
    obj.write_varint_field(9, 0);                   // Hidden
    return scene_wrap_object(obj);
}

// Pure glow+point-light (used for decorative ambience in caves/fires).
std::string build_glow_light(const TorchLight& t, const std::string& name) {
    proto::Writer glow;
    glow.write_nested_field(1, float_color(t.glow_r, t.glow_g, t.glow_b, t.glow_a));
    glow.write_float_field(2, 40.0f); glow.write_varint_field(3, 15);
    glow.write_float_field(4, 5.0f);  glow.write_float_field(5, 0.12f);
    glow.write_float_field(6, 1.5f);  glow.write_nested_field(7, v2(0, 30));

    proto::Writer light;
    light.write_varint_field(1, 3);
    light.write_float_field(2, t.intensity);
    light.write_nested_field(3, float_color(t.glow_r, t.glow_g, t.glow_b, 1.0f));
    light.write_float_field(4, 0.0f);
    light.write_float_field(5, 0.0002f);
    light.write_nested_field(6, v3(0, 30, 0));
    light.write_float_field(7, t.radius);

    proto::Writer obj;
    obj.write_string_field(2, name);
    obj.write_bytes_field(3, component("SimpleGlow", 2, 254, glow).to_string());
    obj.write_bytes_field(3, component("Light", 103, 130, light).to_string()); // payload field 130 (real vanilla)
    obj.write_nested_field(4, v2(t.x, t.y));
    obj.write_float_field(5, t.z);
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(-30, -30, 60, 60));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// ─────────────────────────────────────────────────────────────────────────
// Decorative portal hub (research-backed, byte-exact vs forest_part1)
// ─────────────────────────────────────────────────────────────────────────

// Functional portal object — byte-exact layout verified against the raw
// forest_part1.scene bytes (obj14):
//   Component 'Model'          id 1   payload f101 ModelComponent (no Name)
//   Component 'CollisionShape' id 101 payload f120 ShapeComponent{Rectangle}
//                                     + f121 CollisionShapeComponent
//   Component 'Portal'         id 104 payload f500 PortalComponent
// Object: f2 Identifier, f3 components, f4 Position, f5 Depth, f6 Rotation,
//         f7 Scaling, f8 LocalAabb, f9 Hidden.
static std::string build_portal_object_bytes(const std::string& name, float x, float y,
                                             const std::string& destination) {
    // Component 1: Model (id 1) — empty ModelComponent, exactly like vanilla.
    proto::Writer model_pay;
    model_pay.write_float_field(2, 0.0f);                     // YRotation
    model_pay.write_float_field(3, 0.0f);                     // EmissionFactor
    model_pay.write_float_field(4, 0.0f);                     // XRotation
    model_pay.write_nested_field(5, float_color(0, 0, 0, 1)); // ShatterColor
    model_pay.write_nested_field(6, v3(0, 0, 0));             // Origin
    model_pay.write_varint_field(7, 0);                       // Transparent
    model_pay.write_nested_field(8, float_color(1, 1, 1, 1)); // DiffuseColor
    proto::Writer model_comp = component("Model", 1, 101, model_pay);

    // Component 2: CollisionShape (id 101) — shape rect + collision props.
    proto::Writer rect;
    rect.write_float_field(1, -45.0f); rect.write_float_field(2, -200.0f);
    rect.write_float_field(3, 90.0f);  rect.write_float_field(4, 400.0f);
    proto::Writer shape_pay;
    shape_pay.write_nested_field(1, rect);                    // ShapeComponent.Rectangle
    proto::Writer coll_pay;
    coll_pay.write_float_field(6, -15.0f);                    // MinDepth
    coll_pay.write_float_field(7, 15.0f);                     // MaxDepth
    coll_pay.write_varint_field(8, 2);                        // SpecialType = 2 (portal)
    coll_pay.write_varint_field(11, 1);                       // Enabled
    proto::Writer cs_comp;
    cs_comp.write_string_field(1, "CollisionShape");
    cs_comp.write_varint_field(2, 101);
    cs_comp.write_nested_field(120, shape_pay);
    cs_comp.write_nested_field(121, coll_pay);

    // Component 3: Portal (id 104) — PortalComponent payload f500.
    proto::Writer portal_pay;
    portal_pay.write_string_field(1, destination);            // DestinationSceneName
    portal_pay.write_string_field(2, "");                     // SpawnPointName (default)
    portal_pay.write_varint_field(3, 0);                      // TapToEnter
    portal_pay.write_varint_field(4, 101);                    // TriggerShapeId → CollisionShape
    proto::Writer portal_comp = component("Portal", 104, 500, portal_pay);

    proto::Writer obj;
    obj.write_string_field(2, name);
    obj.write_bytes_field(3, model_comp.to_string());
    obj.write_bytes_field(3, cs_comp.to_string());
    obj.write_bytes_field(3, portal_comp.to_string());
    obj.write_nested_field(4, v2(x, y));
    obj.write_float_field(5, 0.0f);
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(-45, -200, 90, 400));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// Visual portal FX — TemplateName 'portal' (game_common.scl). The engine
// instantiates the swirling portal quad + PortalEffect from the template.
// Vanilla forest_part1: AABB -100.2/-0.7/202.2/155.4, Depth 0, anchor below
// the functional portal so the FX quad centers on it.
static std::string build_portal_fx_object(const std::string& name, float x, float y) {
    proto::Writer obj;
    obj.write_string_field(1, "portal");                 // TemplateName
    obj.write_string_field(2, name);                     // Identifier
    obj.write_nested_field(4, v2(x, y));
    obj.write_float_field(5, 0.0f);
    obj.write_float_field(6, 0.0f);
    obj.write_float_field(7, 1.0f);
    obj.write_nested_field(8, rect_msg(-100.2f, -0.7f, 202.2f, 155.4f));
    obj.write_varint_field(9, 0);
    return scene_wrap_object(obj);
}

// Per-biome "brick mesh" palettes for the portal hub, taken from real scene
// evidence: forest_part1 bakes stoneblock/stonepillair/stoneplatform/treewall
// clusters at foreground depth +47..+57; fire_part31 uses template
// 'stonepillairs2/3'; grove uses grove_pole*/grove_hang*/grove_platform1;
// wasteland has wasteland_ruin1/2. Each entry is {name, template|pod}:
//   template=true → TemplateName reference (name exists in a library)
//   template=false → baked Model object (name is a POD the engine loads directly)
struct HubBrick { const char* name; bool is_template; };
// NOTE: the flat stoneplatform/stoneplatform_white slabs are deliberately NOT
// in the palettes — they read as a "mount" and cover the sign/arrow bar next
// to the portal (user feedback). Only upright blocks/pillars/walls are used.
static const HubBrick* hub_brick_palette(Biome b, int* count) {
    static const HubBrick kForest[] = {
        {"stoneblock", false}, {"stonepillair", false},
        {"treewall2", false}, {"bush", true}, {"smallrock1", true},
    };
    static const HubBrick kGrass[] = {
        {"stonepile", true}, {"pot", true}, {"rock1", true},
        {"bush", true}, {"smallrock1", true},
    };
    static const HubBrick kGrove[] = {
        {"grove_pole1", true}, {"grove_pole2", true}, {"grove_platform1", true},
        {"grove_hang3", true}, {"grove_hang4", true}, {"bush", true},
    };
    static const HubBrick kWasteland[] = {
        {"wasteland_ruin1", false}, {"wasteland_ruin2", false}, {"deadtree1", true},
        {"deadtree2", true}, {"rock1", true}, {"stonepile", true},
    };
    static const HubBrick kIce[] = {
        {"stoneblock_white", false}, {"stonepillair_white", false},
        {"icicles1", true}, {"snowy_tree1", false},
    };
    static const HubBrick kCave[] = {
        {"rock1", true}, {"rock2", true}, {"icicle", false},
        {"icicles1", true}, {"stonepile", true},
    };
    static const HubBrick kFire[] = {
        {"stonepillairs2", true}, {"stonepillairs3", true}, {"stonepillairs4", true},
        {"stoneblock", false}, {"rock1", true},
    };
    static const HubBrick kFlorennum[] = {
        {"pottery1", false}, {"pottery2", false}, {"stonepillair", false},
        {"pot", true}, {"stonepile", true},
    };
    switch (b) {
        case Biome::Forest:   *count = (int)(sizeof(kForest)/sizeof(kForest[0]));   return kForest;
        case Biome::Grove:    *count = (int)(sizeof(kGrove)/sizeof(kGrove[0]));     return kGrove;
        case Biome::Wasteland:*count = (int)(sizeof(kWasteland)/sizeof(kWasteland[0])); return kWasteland;
        case Biome::IceCastle:*count = (int)(sizeof(kIce)/sizeof(kIce[0]));         return kIce;
        case Biome::Cave:     *count = (int)(sizeof(kCave)/sizeof(kCave[0]));       return kCave;
        case Biome::Fire:     *count = (int)(sizeof(kFire)/sizeof(kFire[0]));       return kFire;
        case Biome::Florennum:*count = (int)(sizeof(kFlorennum)/sizeof(kFlorennum[0])); return kFlorennum;
        default:              *count = (int)(sizeof(kGrass)/sizeof(kGrass[0]));     return kGrass;
    }
}

// Shared decoration ring around any portal: sign + per-biome brick/stone
// ruin cluster + shrubs + torches. `flanking` places two torches symmetrically
// at ±75 (exactly like the vanilla 'portal' template's portal_torch_left/right)
// instead of a single torch.
static void append_portal_cluster(std::vector<std::string>& out,
                                  const PortalHubOptions& o, Biome biome,
                                  uint64_t& rng, bool flanking) {
    const std::string base = o.portal_name.empty() ? "portal" : o.portal_name;
    const float dir = (float)o.facing;

    // Sign pointing back at the portal (background depth, like vanilla
    // sign_left1 at depth -98.7 near its portal).
    const float sign_x = o.x + 150.0f * dir;
    {
        const char* sign = (o.facing > 0) ? "sign_left" : "sign_right";
        out.push_back(build_template_object(base + "_sign", sign,
                                            sign_x, o.y - 25.0f,
                                            -30.0f, 0.0f, 1.0f));
    }

    // Brick/stone ruin cluster ringing the portal. Real scenes surround
    // portals with 6-10 stone meshes at foreground depth (+40..+60) plus a few
    // background (-90..-20) accents. Bricks are kept clear of the sign so it
    // stays visible.
    int nbr = 0;
    const HubBrick* palette = hub_brick_palette(biome, &nbr);
    if (nbr > 0) {
        const int ring = 6 + (int)(rng_float(rng, 0.0f, 4.0f));   // 6..9 bricks
        for (int i = 0; i < ring; ++i) {
            const HubBrick& hb = palette[(uint32_t)(rng_next(rng) % (uint64_t)nbr)];
            Deco d;
            // Ring around the portal: alternate sides, spread 55..240 units.
            const float side = (i % 2 == 0) ? 1.0f : -1.0f;
            d.x = o.x + side * rng_float(rng, 55.0f, 240.0f);
            // Never cover the sign/arrow bar with a brick.
            if (std::fabs(d.x - sign_x) < 85.0f) d.x = o.x - side * rng_float(rng, 55.0f, 240.0f);
            d.y = o.y + rng_float(rng, -6.0f, 6.0f);
            // Foreground ~65% (+40..+62), background ~35% (-90..-20).
            d.z = (rng_float(rng, 0.0f, 1.0f) < 0.65f)
                ? rng_float(rng, 40.0f, 62.0f)
                : rng_float(rng, -90.0f, -20.0f);
            d.scale = 0.85f + rng_float(rng, 0.0f, 0.75f);
            d.rot_y = 0.0f;
            d.pod   = hb.name;
            d.name  = base + "_brick" + std::to_string(i + 1);
            std::string bytes;
            if (hb.is_template) {
                bytes = build_template_object(d.name, d.pod, d.x, d.y, d.z, d.rot_y, d.scale);
            } else {
                // Baked Model object — how vanilla emits POD-only stone meshes.
                bytes = build_deco_object_baked_legacy(d, d.name);
            }
            if (!bytes.empty()) out.push_back(bytes);
        }
    }

    // Bushes/rocks right at the portal mouth.
    for (int i = 0; i < 3; ++i) {
        const char* shrub = (i == 1) ? "rock1" : "bush";
        Deco d;
        d.x = o.x + dir * rng_float(rng, 30.0f, 90.0f) * (i == 2 ? -1.0f : 1.0f);
        d.y = o.y + rng_float(rng, -3.0f, 3.0f);
        d.z = rng_float(rng, 25.0f, 55.0f);
        d.scale = 0.7f + rng_float(rng, 0.0f, 0.5f);
        d.rot_y = 0.0f;
        d.pod   = shrub;
        d.name  = base + "_shrub" + std::to_string(i + 1);
        const std::string bytes = build_template_object(d.name, d.pod, d.x, d.y, d.z, d.rot_y, d.scale);
        if (!bytes.empty()) out.push_back(bytes);
    }

    // Torches: either one torch opposite the sign, or two flanking the portal
    // at ±75 (vanilla portal template: portal_torch_left/right at ±75/+40/-70).
    if (flanking) {
        for (int i = 0; i < 2; ++i) {
            TorchLight tl;
            tl.x = o.x + (i == 0 ? -75.0f : 75.0f);
            tl.y = o.y + 40.0f;
            tl.z = -70.0f;
            tl.glow_r = 0.389f; tl.glow_g = 0.111f; tl.glow_b = 0.111f;
            const std::string b = build_torch_object(tl, base + "_torch" + std::to_string(i + 1));
            if (!b.empty()) out.push_back(b);
        }
    } else {
        TorchLight tl;
        tl.x = o.x - 60.0f * dir;
        tl.y = o.y + 6.0f;
        tl.z = 48.0f;
        tl.glow_r = 0.389f; tl.glow_g = 0.111f; tl.glow_b = 0.111f;
        const std::string b = build_torch_object(tl, base + "_torch1");
        if (!b.empty()) out.push_back(b);
    }
}

std::vector<std::string> build_portal_hub(const PortalHubOptions& o,
                                          Biome biome, uint64_t& rng) {
    std::vector<std::string> out;
    const std::string dest = o.destination.empty() ? "next_level" : o.destination;
    const std::string base = o.portal_name.empty() ? "portal" : o.portal_name;
    const float dir = (float)o.facing;

    // 1. Functional portal (collision + PortalComponent).
    out.push_back(build_portal_object_bytes(base, o.x, o.y, dest));

    // 2. Visual portal FX quad (slightly below the anchor so it sits on ground).
    out.push_back(build_portal_fx_object(base + "_fx", o.x, o.y - 10.0f));

    // 3. Return spawn beside the portal — vanilla naming 'spawn_from_<dest>'
    //    (forest_part1: spawn_from_forest_cave0 at +81/-54 from its portal).
    out.push_back(build_spawn_object("spawn_from_" + dest,
                                     o.x + 90.0f * dir, o.y - 50.0f, o.facing));

    // 4. Sign + brick ruin cluster + shrubs + torch.
    append_portal_cluster(out, o, biome, rng, /*flanking=*/false);
    return out;
}

std::vector<std::string> build_game_portal_hub(const PortalHubOptions& o,
                                               Biome biome, uint64_t& rng) {
    std::vector<std::string> out;
    const std::string base = o.portal_name.empty() ? "portal" : o.portal_name;

    // 1. The special world-travel portal MODEL — TemplateName 'portal'
    //    (game_common.scl): portal.POD + its own GroundPolygon platform +
    //    Portal.Activate touch script. One per scene, like vanilla.
    out.push_back(build_portal_fx_object(base, o.x, o.y - 10.0f));

    // 2. Scripted camera focus at the portal: 'cameraFocusShape'
    //    (scriptarea.scl) → Camera.FocusAtShape(self) on hero enter,
    //    Camera.ResetFocus() on exit — a dramatic locked shot of the portal.
    {
        const float f = 2.0f;   // 1200x600 focus zone
        proto::Writer obj;
        obj.write_string_field(1, "cameraFocusShape");          // TemplateName
        obj.write_string_field(2, base + "_camfocus");          // Identifier
        obj.write_nested_field(4, v2(o.x, o.y + 40.0f));
        obj.write_float_field(5, 0.0f);
        obj.write_float_field(6, 0.0f);
        obj.write_float_field(7, f);
        obj.write_nested_field(8, rect_msg(-300.0f * f, -150.0f * f, 600.0f * f, 300.0f * f));
        obj.write_varint_field(9, 0);
        out.push_back(scene_wrap_object(obj));
    }

    // 3. Sign + brick ruin cluster + shrubs + flanking torches (vanilla
    //    portal template spawns portal_torch_left/right at ±75/+40/-70).
    append_portal_cluster(out, o, biome, rng, /*flanking=*/true);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
// Scene assembly (blueprint)
// ─────────────────────────────────────────────────────────────────────────
static void track_aabb(float& minx, float& miny, float& maxx, float& maxy,
                       const std::vector<Vec2>& poly) {
    for (const auto& v : poly) {
        minx = std::min(minx, v.x); miny = std::min(miny, v.y);
        maxx = std::max(maxx, v.x); maxy = std::max(maxy, v.y);
    }
}

Result generate_scene(const Blueprint& bp) {
    Result r;
    if (bp.platforms.empty()) { r.error = "blueprint has no platforms"; return r; }

    proto::Writer scene;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;

    scene.write_bytes_field(1, extract_object_bytes(build_background_object(bp.background)));
    ++r.objects;
    scene.write_bytes_field(1, extract_object_bytes(
        build_light_object(bp.light_name, 0, 0, 137.0f,
                           3.0f, 0.3f, 0.4f,   1.0f, 1.0f, 1.0f)));
    ++r.objects;

    for (size_t i = 0; i < bp.platforms.size(); ++i) {
        const std::string nm = bp.platforms.size()==1 ? "ground" : "ground"+std::to_string(i+1);
        std::string bytes = build_ground_object(bp.platforms[i], nm);
        if (bytes.empty()) { r.error = "failed to bake platform " + std::to_string(i); return r; }
        std::vector<Vec2> poly = bp.platforms[i].polygon;
        if (poly.size() < 3)
            poly = make_rect_polygon(bp.platforms[i].rect[0],bp.platforms[i].rect[1],
                                     bp.platforms[i].rect[2],bp.platforms[i].rect[3]);
        if (bp.platforms[i].horiz_noise > 0.0f)
            apply_horiz_noise(poly, bp.platforms[i].horiz_noise, bp.platforms[i].seed);
        track_aabb(minx, miny, maxx, maxy, poly);
        scene.write_bytes_field(1, extract_object_bytes(bytes));
        ++r.objects;
    }
    for (size_t i = 0; i < bp.waters.size(); ++i) {
        const std::string nm = bp.waters.size()==1?"water":"water"+std::to_string(i+1);
        std::string bytes = build_water_object(bp.waters[i], nm);
        if (!bytes.empty()) { scene.write_bytes_field(1, extract_object_bytes(bytes)); ++r.objects; }
    }
    float sx = bp.spawn.x, sy = bp.spawn.y;
    if (sx==0.0f && sy==0.0f) {
        std::vector<Vec2> poly = bp.platforms[0].polygon;
        if (poly.size()<3)
            poly = make_rect_polygon(bp.platforms[0].rect[0],bp.platforms[0].rect[1],
                                     bp.platforms[0].rect[2],bp.platforms[0].rect[3]);
        float top=-1e9f, cx=0.0f;
        for (const auto& v:poly){top=std::max(top,v.y); cx+=v.x;}
        cx /= (float)poly.size();
        sx=cx; sy=top+56.0f;
    }
    scene.write_bytes_field(1, extract_object_bytes(
        build_spawn_object("spawn_default", sx, sy, bp.spawn_facing)));
    ++r.objects;

    // ObjectLibrary (Scene field 2) — REQUIRED so the game can resolve every
    // TemplateName-reference object (decorations/torches/props). Without the
    // imported libraries the engine's ObjectLibrary::TemplateForName() finds
    // nothing and all such objects render as invisible phantoms. Must be
    // written before Bounds (field 3) to keep field order ascending like vanilla.
    scene.write_bytes_field(2, build_object_library(bp.scene_name, default_scene_imports()));

    if (maxx <= minx || maxy <= miny) { r.error = "platform AABB is empty"; return r; }
    const float pad=bp.bounds_pad;
    const float bx=minx-pad, by=miny-pad, bw=(maxx-minx)+2*pad, bh=(maxy-miny)+2*pad;
    scene.write_bytes_field(3, build_bounds_payload(bx,by,bw,bh));
    r.bounds[0]=bx; r.bounds[1]=by; r.bounds[2]=bw; r.bounds[3]=bh;
    r.scene_bytes = scene.to_string();
    return r;
}

Result validate_scene(const Blueprint& bp, std::vector<std::string>* messages) {
    Result r;
    auto fail = [&](const std::string& m){ r.error=m; if(messages) messages->push_back(m); };
    if (bp.platforms.empty()) { fail("validate: no platforms"); return r; }
    auto resolved_poly = [](const Platform& p) {
        std::vector<Vec2> poly = p.polygon;
        if (poly.size()<3) poly = make_rect_polygon(p.rect[0],p.rect[1],p.rect[2],p.rect[3]);
        if (p.horiz_noise>0.0f) apply_horiz_noise(poly, p.horiz_noise, p.seed);
        return poly;
    };
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    for (const auto& p:bp.platforms){auto poly=resolved_poly(p); if(poly.size()<3){fail("empty polygon");return r;} track_aabb(minx,miny,maxx,maxy,poly);}
    if (bp.bounds_pad<0.0f) fail("bounds_pad<0");
    float sx=bp.spawn.x,sy=bp.spawn.y;
    if (sx==0&&sy==0) sy=maxy+56.0f;
    {float lt=-1e9f; bool on=false;
     for (const auto&p:bp.platforms){auto poly=resolved_poly(p);float mx=1e9f,Mx=-1e9f,top=-1e9f,cx=0;for(const auto&v:poly){mx=std::min(mx,v.x);Mx=std::max(Mx,v.x);top=std::max(top,v.y);cx+=v.x;}cx/=(float)poly.size();if(sx>=mx-12&&sx<=Mx+12){lt=std::max(lt,top);on=true;}}
     const float need = on ? lt : maxy;
     if (sy <= need + 1.0f) fail("spawn below platform"); }
    for (const auto&w:bp.waters) if(w.rect[1]>=miny) fail("water above ground");
    if(bp.background.empty()) fail("no background");
    if(bp.light_name.empty()) fail("no light");
    r.objects=(int)(bp.platforms.size()+bp.waters.size())+3;
    r.bounds[0]=minx-bp.bounds_pad; r.bounds[1]=miny-bp.bounds_pad;
    r.bounds[2]=(maxx-minx)+2*bp.bounds_pad; r.bounds[3]=(maxy-miny)+2*bp.bounds_pad;
    if(r.error.empty()) r.scene_bytes="valid";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────
// v2: Biome table (corrected textures from real scene analysis)
//
// Texture naming convention:
//   ground_top   = SurfaceTextureMappingId texture (e.g. grass_grass, forest_grass)
//   ground_front = FrontTextureMappingId texture  (e.g. maybegood, forest_ground)
//
// Real biome evidence:
//   grass_part1:   top=grass_subtle/grass_grass  front=maybegood
//   forest_part1:  top=forest_grass              front=forest_ground
//   grove_part1:   top=forest_grass              front=forest_ground
//   plains_part1:  top=grass_subtle/grass_grass  front=maybegood
//   snowy_part1:   top=snowy_snow/icecastle_ground front=atlon_ground
//   cave scenes:   top=cavewalls                 front=lowercave_ground1
//   fire scenes:   top=fire_grass                front=graveyard_ground
//   florennum:     top=florennum_grass            front=florennum_ground
// ─────────────────────────────────────────────────────────────────────────

static const BiomeSpec kBiomes[static_cast<int>(Biome::Count)] = {
    // Grasslands — grass_part1 / plains_part1
    {
        "Grasslands",
        "grasslandsbackground_day",         // background
        "grass_grass",                       // top surface (SurfaceTextureMappingId)
        "maybegood",                         // front/cliff (FrontTextureMappingId)
        {"grass_tree1","bush","grove_tree1","grove_tree2","",""},
        {"smallrock1","rock1","pot",""},
        {"bush","bush_cut","smallrock1",""},
        "grove_torch",
        0.389f, 0.111f, 0.111f,
        {0.00f,0.314f,0.233f,0.744f},
        {0.00f,0.376f,0.256f,0.744f},
        {1.0f,0.98f,0.90f,1.0f},
        {0.45f,0.48f,0.55f,1.0f},
        0.10f, 0.035f, 0.20f, 260.0f,
    },
    // Forest — forest_part1 (dense grove trees, mossy ground)
    {
        "Forest",
        "forest_background",
        "forest_grass",                      // top surface
        "forest_ground",                     // front face
        {"grove_tree1","grove_tree2","grove_tree3","bush","",""},
        {"rock1","rock2","",""},
        {"bush","bush_cut","smallrock1",""},
        "grove_torch",
        0.389f,0.111f,0.111f,
        {0.00f,0.314f,0.233f,0.744f},
        {0.00f,0.376f,0.256f,0.744f},
        {0.92f,0.96f,0.85f,1.0f},
        {0.32f,0.40f,0.32f,1.0f},
        0.13f, 0.04f, 0.18f, 300.0f,
    },
    // Grove — grove_part1 (grove_grass, torches + poles)
    {
        "Grove",
        "grove_bg",
        "grove_grass",                       // top
        "grove_ground",                      // front
        {"grove_tree1","grove_tree2","grove_tree3","bush","",""},
        {"grove_pole1","grove_pole2","",""},
        {"bush","bush_cut","pot",""},
        "grove_torch",
        0.344f,0.111f,0.100f,
        {0.00f,0.314f,0.233f,0.744f},
        {0.00f,0.376f,0.256f,0.744f},
        {0.98f,0.92f,0.80f,1.0f},
        {0.38f,0.34f,0.30f,1.0f},
        0.11f, 0.05f, 0.16f, 220.0f,
    },
    // Wasteland — dead trees, sand
    {
        "Wasteland",
        "wasteland_bg",
        "grass_orange",                      // top (orange sandy grass)
        "grasslandsbackground_day",          // front (re-used as cliff)
        {"deadtree1","deadtree2","bush_cut","","",""},
        {"hugerock1","rock1","rock2",""},
        {"bush_cut","smallrock1","rock1",""},
        "torchholder",
        0.611f,0.056f,0.056f,
        {0.10f,0.06f,0.03f,0.70f},
        {0.14f,0.08f,0.04f,0.70f},
        {0.95f,0.85f,0.70f,1.0f},
        {0.40f,0.36f,0.30f,1.0f},
        0.05f, 0.07f, 0.10f, 340.0f,
    },
    // Ice Castle — snowy_part1 (snow, icicles, atlon_ground)
    {
        "Ice Castle",
        "atlon",
        "snowy_snow",                        // top
        "atlon_ground",                      // front
        {"snowy_tree1","icicles1","icicles2","icicle","",""},
        {"icicle","rock1","",""},
        {"icicle","smallrock1","snowy_tree1",""},
        "torchholder",
        0.30f,0.55f,0.85f,
        {0.10f,0.45f,0.65f,0.60f},
        {0.15f,0.55f,0.75f,0.60f},
        {0.85f,0.92f,1.0f,1.0f},
        {0.50f,0.55f,0.65f,1.0f},
        0.05f, 0.07f, 0.12f, 380.0f,
    },
    // Cave — thecave / florennum_cave
    {
        "Cave",
        "cavesbackground2",
        "cavewalls",                         // top (cave ceiling texture)
        "lowercave_ground1",                 // front
        {"icicles1","icicles2","","","",""},
        {"rock1","rock2","",""},
        {"icicle","smallrock1","",""},
        "torchholder",
        0.389f,0.111f,0.111f,
        {0.00f,0.31f,0.35f,0.70f},
        {0.00f,0.38f,0.42f,0.70f},
        {0.90f,0.85f,0.75f,1.0f},
        {0.30f,0.30f,0.34f,1.0f},
        0.04f, 0.08f, 0.08f, 200.0f,
    },
    // Fire — fire_part* (ember grass, lava water)
    {
        "Fire",
        "cauldron",
        "fire_grass",                        // top
        "graveyard_ground",                  // front (real scene: fire uses graveyard front)
        {"deadtree1","deadtree2","","","",""},
        {"rock1","rock2","",""},
        {"rock1","rock2","smallrock1",""},
        "torchholder",
        0.90f,0.20f,0.05f,
        {0.40f,0.05f,0.02f,0.75f},
        {0.50f,0.08f,0.03f,0.75f},
        {1.0f,0.85f,0.70f,1.0f},
        {0.50f,0.30f,0.22f,1.0f},
        0.04f, 0.07f, 0.06f, 250.0f,
    },
    // Florennum — florennum_* (town stone, warm lights)
    {
        "Florennum",
        "florennum_night_bg",
        "florennum_grass",                   // top
        "florennum_ground",                  // front
        {"bush","pottery1","pottery2","pole1","",""},
        {"pot","pottery1","pole2",""},
        {"pot","pottery1","pottery2","bush"},
        "torchholder",
        0.389f,0.111f,0.111f,
        {0.00f,0.314f,0.233f,0.744f},
        {0.00f,0.376f,0.256f,0.744f},
        {1.0f,0.95f,0.85f,1.0f},
        {0.42f,0.38f,0.35f,1.0f},
        0.06f, 0.08f, 0.14f, 280.0f,
    },
};

const BiomeSpec& biome_spec(Biome b) {
    int idx = (int)b;
    if (idx<0||idx>=(int)Biome::Count) idx=0;
    return kBiomes[idx];
}
const char* biome_name(Biome b) { return biome_spec(b).name; }

// ─────────────────────────────────────────────────────────────────────────
// v3: Full procedural biome scene generator (remastered)
// ─────────────────────────────────────────────────────────────────────────

Result generate_biome_scene(const TerrainOptions& opt) {
    Result r;
    const BiomeSpec& bio  = biome_spec(opt.biome);
    const uint32_t seed   = opt.seed ? opt.seed : 1u;
    const int n           = std::max(2, opt.platform_count);
    const float half_w    = opt.width * 0.5f;
    const float floor_y   = -opt.height * 0.5f;

    // ── 1. Heightfield synthesis ───────────────────────────────────────────
    // More samples → smoother geometry, especially after Gaussian smoothing.
    const int samples = std::max(48, (int)(opt.width / 40.0f));
    std::vector<float> prof(samples);
    const float step = opt.width / (float)(samples - 1);

    // Raw noise generation (fBm or ridged mountain).
    for (int i = 0; i < samples; ++i) {
        const float x  = -half_w + i * step;
        const float nx = x / opt.width * 6.0f;          // normalized noise frequency
        float v;
        if (opt.mountains) {
            // Ridged: sharp ridges + fBm blend for sub-ridge variation.
            const float ridge = ridged_2d(nx, 0.5f, opt.octaves, seed);
            const float low   = fbm_1d(nx, std::max(2, opt.octaves-1), seed^0xFF, 0.2f);
            v = ridge * 0.70f + low * 0.30f;            // 70% ridged, 30% smooth fBm
        } else {
            // Standard: domain-warped fBm for organic silhouette.
            v = fbm_1d(nx, opt.octaves, seed, 0.35f);
        }
        // Map v (-1..+1) to terrain height, centered around a slight baseline.
        prof[i] = v * (opt.height * 0.28f * opt.roughness);
    }

    // ── 1b. Gaussian smoothing ─────────────────────────────────────────────
    // 3 passes of Gaussian (radius=5, σ≈2.25) eliminates quick spikes while
    // preserving natural-looking rolling hills. Without this, raw fBm produces
    // jagged terrain that causes the "missing top surface" symptom — the top
    // cap polygon becomes too thin to triangulate on near-vertical edges.
    smooth_gaussian(prof, /*radius=*/5, /*passes=*/3);

    // ── 1c. Midpoint spawn-pad guarantee ──────────────────────────────────
    // The center ±12% of the scene is raised to a guaranteed walkable level.
    // Only ever raises, never lowers (preserves natural high ground).
    {
        const float plateau_half = opt.width * 0.12f;
        const float base         = opt.height * 0.02f;  // small bump above average
        for (int i = 0; i < samples; ++i) {
            const float x = -half_w + i * step;
            const float d = std::fabs(x) / plateau_half;  // 0 at center
            if (d >= 1.0f) continue;
            const float plat_h = base + (1.0f - d*d) * opt.height * 0.04f;
            prof[i] = std::max(prof[i], plat_h);
        }
    }

    // ── 2. Slice into walkable platform strips ─────────────────────────────
    // SEAMLESS SLICING (fixes horizontal gaps + abrupt height jumps):
    //   • Strips are sliced on SAMPLE boundaries and every strip SHARES its
    //     boundary sample with the next one (s1 of strip p == s0 of strip p+1).
    //     Because both strips read the SAME prof[] value at that shared X, their
    //     touching edges have identical (x,y) — no gap, no vertical step at the
    //     seam. Previously the strips were sliced independently and could round
    //     to leave a one-sample hole, and a per-strip cumulative "lift" pushed
    //     each strip up by a fixed amount so adjacent seams never matched.
    //   • The cumulative staircase lift is REMOVED. All height variation now
    //     comes purely from the smoothed heightfield, which is C1-continuous, so
    //     neighbours meet flush. (The midpoint spawn-pad guarantee above already
    //     keeps the centre walkable.)
    // MESH DEPTH (Z): the ground collision/geometry band is ±112 — ~2.5× the old
    // ±45 and squarely inside the real vanilla range (most shipped ground meshes
    // use ±100..±125, verified via ruby_cli decode of forest/grove/grass).
    constexpr float kGroundDepth = 112.0f;                 // was 45 → 2.5×
    std::vector<Platform> plats;
    // Sample-index boundaries so consecutive strips share one sample exactly.
    for (int p = 0; p < n; ++p) {
        const int s0 = std::clamp((int)std::llround((double)p     * (samples - 1) / n), 0, samples-1);
        const int s1 = std::clamp((int)std::llround((double)(p+1) * (samples - 1) / n), 0, samples-1);
        if (s1 <= s0) continue;
        const int cnt = s1 - s0 + 1;                       // inclusive → shares s1 with next s0
        std::vector<float> h(cnt);
        for (int i = 0; i < cnt; ++i) h[i] = prof[s0 + i]; // NO per-strip lift: seams stay flush
        const float x0 = -half_w + (float)s0 * step;       // strip world origin = shared sample X

        Platform plat;
        auto poly = make_heightfield_polygon(h.data(), cnt, step, floor_y - opt.height*0.05f);
        for (auto& v : poly) v.x += x0;
        plat.polygon      = std::move(poly);
        plat.seed         = (uint32_t)(seed ^ ((uint32_t)p * 0x9E37u));
        plat.horiz_noise  = 0.0f;            // no horiz noise: keeps strips seamless
        plat.top_texture  = bio.ground_top;  // SurfaceTextureMappingId (grass top)
        plat.front_texture= bio.ground_front;// FrontTextureMappingId (cliff face)
        plat.z            = 0.0f;            // ALL platforms at depth 0 (Swordigo convention)
        plat.min_depth    = -kGroundDepth;   // thicker mesh (2.5×), matches vanilla
        plat.max_depth    =  kGroundDepth;
        plat.surface_width= 100.0f + (float)p * 15.0f; // 100-150 matching real scenes
        plats.push_back(std::move(plat));
    }
    if (plats.empty()) { r.error = "procedural: no platforms generated"; return r; }

    // ── 3. Build blueprint ─────────────────────────────────────────────────
    Blueprint bp;
    bp.platforms  = std::move(plats);
    bp.background = bio.background;
    bp.light_name = "DirectionalLight";
    bp.bounds_pad = 220.0f;
    bp.scene_name = opt.scene_name;

    // Water sheet — positioned below the lowest strip floor.
    if (opt.add_water && bio.water_front[3] > 0.01f) {
        const float water_top = floor_y - 80.0f;   // well below platform bottom
        Water w;
        w.rect[0] = -half_w - 120.0f;
        w.rect[1] = water_top;
        w.rect[2] = opt.width + 240.0f;
        w.rect[3] = 50.0f;
        memcpy(w.front_rgba,   bio.water_front,   sizeof(float)*4);
        memcpy(w.surface_rgba, bio.water_surface, sizeof(float)*4);
        bp.waters.push_back(w);
    }

    // ── 4. Assemble scene objects ──────────────────────────────────────────
    proto::Writer scene;
    float minx=1e9f, miny=1e9f, maxx=-1e9f, maxy=-1e9f;
    uint64_t drng = (uint64_t)seed ^ 0xBEEF1337ull;

    // Background
    scene.write_bytes_field(1, extract_object_bytes(build_background_object(bp.background)));

    // DirectionalLight — depth between 17 and 140 (real scene range).
    // Derive from scene width so wider scenes put it slightly further.
    const float light_depth = 17.0f + opt.width * 0.018f;
    scene.write_bytes_field(1, extract_object_bytes(
        build_light_object(bp.light_name, 0, 0, light_depth,
                           // Real forest_part1 key (Type 2) intensity is 3.0. The
                           // previous *2.5 scale under-lit scenes (~2.25), which is
                           // why generated backgrounds looked "too dark". Scale to
                           // the real ~3.0 baseline and clamp to a sane [2.6, 3.4].
                           bio.key_light[0]   > 0.0f ? std::clamp(bio.key_light[0]*3.2f, 2.6f, 3.4f) : 3.0f,
                           bio.ambient[0]     > 0.0f ? bio.ambient[0]*0.8f  : 0.3f,
                           0.4f,
                           bio.key_light[0], bio.key_light[1], bio.key_light[2])));
    r.objects = 2;

    // Ground platforms
    for (size_t i = 0; i < bp.platforms.size(); ++i) {
        Platform plat = bp.platforms[i];
        // Islands mode: add dome hats on every other platform
        if (opt.islands && i % 2 == 0) {
            float cx=0, top=-1e9f;
            for (const auto& v:plat.polygon){cx+=v.x;top=std::max(top,v.y);}
            if (!plat.polygon.empty()) cx/=(float)plat.polygon.size();
            Hat hat;
            hat.x=cx; hat.y=top;
            hat.radius=80.0f+rng_float(drng,0,60.0f);
            hat.height=45.0f+rng_float(drng,0,35.0f);
            plat.hats.push_back(hat);
        }
        const std::string nm = bp.platforms.size()==1?"ground":"ground"+std::to_string(i+1);
        std::string bytes = build_ground_object(plat, nm);
        if (!bytes.empty()) {
            scene.write_bytes_field(1, extract_object_bytes(bytes));
            ++r.objects;
            // AABB tracking
            std::vector<Vec2> poly = plat.polygon;
            if (poly.size()<3)
                poly = make_rect_polygon(plat.rect[0],plat.rect[1],plat.rect[2],plat.rect[3]);
            track_aabb(minx, miny, maxx, maxy, poly);
        }
    }

    // Water
    for (size_t i = 0; i < bp.waters.size(); ++i) {
        const std::string nm = bp.waters.size()==1?"water":"water"+std::to_string(i+1);
        std::string bytes = build_water_object(bp.waters[i], nm);
        if (!bytes.empty()) { scene.write_bytes_field(1,extract_object_bytes(bytes)); ++r.objects; }
    }

    // ── 5. Decorations ─────────────────────────────────────────────────────
    // Collect top-edge samples from every platform, then scatter:
    //   - Trees:  sparse (density from biome), random depth [-75, +115]
    //   - Rocks:  medium density, random depth [-70, +80]
    //   - Grass:  high density, small scale, random depth [-50, +50]
    //   - Torches: every N world units along the edge, biome-controlled spacing
    //
    // DEPTH STRATEGY: Observed in real scenes (grass_part1, grove_part1):
    //   obj1#2  tree  depth=+69.6   (foreground, large parallax)
    //   obj1#3  tree  depth=-26.7   (background)
    //   obj1#5  tree  depth=+66.1
    //   obj1#7  tree  depth=-73.5   (deep background)
    //   torch   depth=+107.0
    //   torch   depth=-80.4
    // So we distribute across the full observed range with a slight positive
    // bias (more foreground than background) matching the real distribution.
    //
    // CLUSTER PATTERN: Trees appear in clusters of 1-3 at a given X, with
    // varying depth — this is the "multi-plane" illusion. We implement this
    // by drawing cluster counts from a Poisson-like distribution seeded per
    // X-bucket.

    const float deco_density = std::clamp(opt.deco_density, 0.0f, 2.0f);
    int deco_idx = 0, torch_idx = 0, glow_idx = 0;

    // Helper: depth drawn from a biased distribution favoring foreground.
    // Depth distribution — derived from real scenes (grove/grass_part1 decode):
    // decoration Depth ranges ~ -75..+120 with a MEDIAN around +20 (slight
    // foreground bias). We draw ~65% from the near/foreground band and ~35% from
    // the far/background band to reproduce that multi-plane spread.
    auto biased_depth = [&](float near_lo, float near_hi,
                            float far_lo,  float far_hi) -> float {
        const bool foreground = (rng_float(drng, 0, 1.0f) < 0.65f);
        return foreground ? rng_float(drng, near_lo, near_hi)
                          : rng_float(drng, far_lo, far_hi);
    };

    // ── EVEN-DENSITY 1D SCATTER (blue-noise / Poisson-disk along the ground) ──
    // The old code placed decos wherever a smooth-noise value crossed a
    // threshold, which CLUMPED them (many where the noise sat near the target,
    // none elsewhere). Instead we now walk each platform's top edge left→right
    // with a per-class cursor: the next item is placed a controlled distance
    // (mean spacing ± jitter) past the previous one. This guarantees:
    //   • no clumps  (a hard minimum spacing is always respected), and
    //   • no long empty stretches (the cursor keeps advancing by ~mean spacing),
    // matching the vanilla feel (grass/grove decoration X-spacing median ≈64–86).
    // Density (opt.deco_density × biome density) scales the mean spacing:
    // denser biome → smaller spacing → more items, still evenly distributed.
    //
    // A continuous top-edge sampler: returns ground Y at world X (linear interp
    // over the platform's top vertices), or -1e9 if X is outside the platform.
    struct TopEdge { std::vector<Vec2> pts; float min_x, max_x; };
    auto build_top_edge = [](const Platform& plat) -> TopEdge {
        std::vector<Vec2> poly = plat.polygon;
        if (poly.size()<3) poly = make_rect_polygon(plat.rect[0],plat.rect[1],plat.rect[2],plat.rect[3]);
        // Bucket to the highest Y per X (the walkable surface).
        const float bucket = 24.0f;
        float mn=1e9f, mx=-1e9f;
        for (const auto& v:poly){mn=std::min(mn,v.x);mx=std::max(mx,v.x);}
        const int nb = std::max(1,(int)((mx-mn)/bucket));
        std::vector<float> top(nb, -1e9f);
        for (const auto& v:poly){const int b=std::clamp((int)((v.x-mn)/bucket),0,nb-1);top[b]=std::max(top[b],v.y);}
        TopEdge te; te.min_x=mn; te.max_x=mx;
        for (int b=0;b<nb;++b) if (top[b]>-1e8f) te.pts.push_back({mn+(b+0.5f)*bucket, top[b]});
        return te;
    };
    auto ground_y_at = [](const TopEdge& te, float x) -> float {
        if (te.pts.empty()) return -1e9f;
        if (x <= te.pts.front().x) return te.pts.front().y;
        if (x >= te.pts.back().x)  return te.pts.back().y;
        for (size_t k=1;k<te.pts.size();++k) {
            if (x <= te.pts[k].x) {
                const float t=(x-te.pts[k-1].x)/std::max(1e-3f,(te.pts[k].x-te.pts[k-1].x));
                return te.pts[k-1].y + t*(te.pts[k].y-te.pts[k-1].y);
            }
        }
        return te.pts.back().y;
    };

    // One evenly-spaced pass for a given decoration class.
    // mean_spacing: target world-units between successive items (>= ~40).
    // emit(x, groundY): places the item(s) at that anchor.
    auto scatter_even = [&](const Platform& plat, float mean_spacing,
                            uint32_t salt,
                            const std::function<void(float,float)>& emit) {
        if (mean_spacing < 24.0f) mean_spacing = 24.0f;
        const TopEdge te = build_top_edge(plat);
        if (te.pts.size() < 2) return;
        // Deterministic per-(platform,class) phase so scenes stay reproducible.
        uint64_t s = ((uint64_t)seed << 1) ^ salt ^ ((uint64_t)(&plat - &bp.platforms[0]) * 0x9E37u);
        float x = te.min_x + rng_float(s, 0.0f, mean_spacing); // random phase
        while (x < te.max_x) {
            const float gy = ground_y_at(te, x);
            if (gy > -1e8f) emit(x, gy);
            // Advance by mean ± 40% jitter → even spacing, never clumps/gaps.
            x += mean_spacing * rng_float(s, 0.60f, 1.40f);
        }
    };

    const float dens = (deco_density <= 0.0f) ? 1.0f : deco_density;

    for (size_t i = 0; i < bp.platforms.size(); ++i) {
        const Platform& plat = bp.platforms[i];
        int ntree=0; while(ntree<6 && bio.deco_trees[ntree][0]) ++ntree;
        int nrock=0; while(nrock<4 && bio.deco_rocks[nrock][0]) ++nrock;
        int ngras=0; while(ngras<4 && bio.deco_grass[ngras][0]) ++ngras;

        // ── Trees (sparse accents, occasional 1–2 cluster) ──────────────────
        // Mean spacing inversely proportional to biome tree_density. e.g.
        // tree_density 0.13 → ~200u apart; 0.05 → ~520u apart.
        if (ntree > 0) {
            const float sp = std::clamp(26.0f / (bio.tree_density * dens + 1e-3f), 150.0f, 900.0f);
            scatter_even(plat, sp, 0xA11CEu, [&](float x, float gy){
                const int cluster = (rng_float(drng,0,1.0f) < 0.30f) ? 2 : 1; // occasional pair
                for (int ci=0; ci<cluster; ++ci) {
                    Deco d;
                    d.x = x + (ci? rng_float(drng, 30.0f, 70.0f) : rng_float(drng,-16.0f,16.0f));
                    d.y = gy + rng_float(drng, -4.0f, 4.0f);
                    d.z = biased_depth(15.0f, 120.0f, -75.0f, -10.0f);
                    d.scale = opt.randomize_deco_scale ? (0.80f + rng_float(drng, 0.0f, 0.55f)) : 1.0f;
                    d.rot_y = (opt.randomize_deco_rotation && rng_float(drng,0,1.0f) < 0.45f) ? -1.5707963f : 0.0f;
                    d.pod  = bio.deco_trees[hash_uint((uint32_t)(deco_idx*131+7)) % (unsigned)ntree];
                    d.name = "deco_tree" + std::to_string(++deco_idx);
                    std::string b = build_deco_object(d, d.name);
                    if (!b.empty()) { scene.write_bytes_field(1,extract_object_bytes(b)); ++r.objects; }
                }
            });
        }

        // ── Rocks / pots / pillars (medium spacing) ─────────────────────────
        if (nrock > 0) {
            const float sp = std::clamp(30.0f / (bio.rock_density * dens + 1e-3f), 180.0f, 1000.0f);
            scatter_even(plat, sp, 0xB0B0u, [&](float x, float gy){
                Deco d;
                d.x = x + rng_float(drng, -18.0f, 18.0f);
                d.y = gy + rng_float(drng, -2.0f, 2.0f);
                d.z = biased_depth(10.0f, 85.0f, -70.0f, -5.0f);
                d.scale = opt.randomize_deco_scale ? (0.60f + rng_float(drng, 0.0f, 0.65f)) : 1.0f;
                d.rot_y = 0.0f;
                d.pod  = bio.deco_rocks[hash_uint((uint32_t)(deco_idx*337+13)) % (unsigned)nrock];
                d.name = "deco_rock" + std::to_string(++deco_idx);
                std::string b = build_deco_object(d, d.name);
                if (!b.empty()) { scene.write_bytes_field(1,extract_object_bytes(b)); ++r.objects; }
            });
        }

        // ── Small grass / tufts (dense ground cover, tight even spacing) ────
        if (ngras > 0) {
            const float sp = std::clamp(26.0f / (bio.grass_density * dens + 1e-3f), 70.0f, 400.0f);
            scatter_even(plat, sp, 0x6A55u, [&](float x, float gy){
                Deco d;
                d.x = x + rng_float(drng, -24.0f, 24.0f);
                d.y = gy + rng_float(drng, -2.0f, 2.0f);
                d.z = biased_depth(5.0f, 55.0f, -50.0f, -5.0f);
                d.scale = opt.randomize_deco_scale ? (0.28f + rng_float(drng, 0.0f, 0.40f)) : 1.0f;
                d.rot_y = 0.0f;
                d.pod  = bio.deco_grass[hash_uint((uint32_t)(deco_idx*613+23)) % (unsigned)ngras];
                d.name = "deco_grass" + std::to_string(++deco_idx);
                std::string b = build_deco_object(d, d.name);
                if (!b.empty()) { scene.write_bytes_field(1,extract_object_bytes(b)); ++r.objects; }
            });
        }

        // ── Torches / ambient lights (Raijin "Decayed v2" balance) ─────────
        // Full torch objects (Model + FireEmitter + Light + Glow) read as
        // "off" when they blanket a level, and the scene's real light budget
        // is meant to come from the DirectionalLight (key + ambient) emitted
        // above. So we lean on that + "small patches of light" (a lightweight
        // point Light + SimpleGlow, NO torch model/fire) for most positions,
        // and only drop an actual torch every few slots as an accent. Net
        // effect: far fewer torches, ambience carried by directional + glow.
        if (opt.spill_torches && bio.torch_pod[0]) {
            const float base_sp = (opt.torch_spacing > 0.0f) ? opt.torch_spacing : bio.torch_spacing;
            // Widen spacing so lights are sparser overall (decayed-v2 look).
            const float sp = base_sp * 1.6f;
            // 1-in-N slots becomes a real torch; the rest are glow patches.
            const int torch_every = 3;
            int slot = 0;
            scatter_even(plat, sp, 0x70C4u, [&](float x, float gy){
                const bool real_torch = (slot++ % torch_every) == 0;
                TorchLight tl;
                tl.x = x + rng_float(drng, -sp*0.15f, sp*0.15f);
                tl.y = gy + 6.0f;
                tl.z = biased_depth(30.0f, 107.0f, -80.0f, -20.0f);  // grove range
                tl.glow_r=bio.glow_r; tl.glow_g=bio.glow_g; tl.glow_b=bio.glow_b;
                std::string b;
                if (real_torch) {
                    tl.radius = 300.0f + rng_float(drng, 0, 100.0f);
                    tl.intensity = 2.0f;
                    b = build_torch_object(tl, "torch"+std::to_string(++torch_idx));
                } else {
                    // Small patch of light — soft, low-intensity point glow.
                    tl.radius = 180.0f + rng_float(drng, 0, 80.0f);
                    tl.intensity = 1.1f;
                    tl.glow_a = 0.35f;
                    b = build_glow_light(tl, "glow"+std::to_string(++glow_idx));
                }
                if (!b.empty()) { scene.write_bytes_field(1,extract_object_bytes(b)); ++r.objects; }
            });
        }
    } // end per-platform decoration scatter

    // ── 5b. Portal hub (optional) ─────────────────────────────────────────
    // Portals anchor at the scene EXTREMES (leftmost/rightmost platform),
    // never on the center spawn pad:
    //   • scene-transition portal (build_portal_hub) at one extreme,
    //   • the special world-travel portal MODEL (build_game_portal_hub —
    //     the one-per-scene 'portal' template) at the opposite extreme,
    // each ringed with a per-biome brick/stone ruin cluster, sign, shrubs and
    // torches.
    if (opt.add_portal && !bp.platforms.empty()) {
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
        const size_t p0 = left_first ? 0 : bp.platforms.size() - 1;
        const int    e1 = -e0;
        const size_t p1 = left_first ? bp.platforms.size() - 1 : 0;

        float px = 0.0f, py = 0.0f;
        // Scene-transition portal at the first extreme.
        if (portal_extreme_anchor(bp.platforms[p0], e0, px, py)) {
            PortalHubOptions ph;
            ph.x = px; ph.y = py + 4.0f;
            ph.facing = -e0;   // spawn + sign face toward the scene interior
            ph.destination = opt.portal_destination;
            ph.portal_name = "portal";
            const auto hub = build_portal_hub(ph, opt.biome, drng);
            for (const auto& bytes : hub) {
                scene.write_bytes_field(1, extract_object_bytes(bytes));
                ++r.objects;
            }
        }
        // World-travel portal model at the opposite extreme.
        bool ok2 = false;
        if (bp.platforms.size() == 1) {
            // Single platform: place it on the far side of the same strip.
            ok2 = portal_extreme_anchor(bp.platforms[p0], e1, px, py);
        } else {
            ok2 = portal_extreme_anchor(bp.platforms[p1], e1, px, py)
               || portal_extreme_anchor(bp.platforms[p0], e1, px, py);
        }
        if (ok2) {
            PortalHubOptions ph;
            ph.x = px; ph.y = py + 4.0f;
            ph.facing = -e1;   // decos face toward the scene interior
            ph.destination = opt.portal_destination;
            ph.portal_name = "portal";
            const auto hub = build_game_portal_hub(ph, opt.biome, drng);
            for (const auto& bytes : hub) {
                scene.write_bytes_field(1, extract_object_bytes(bytes));
                ++r.objects;
            }
        }
    }

    // ── 6. Spawn point ────────────────────────────────────────────────────
    // Find the platform whose X range contains x=0 (scene center) and pick
    // its highest point. This is the guaranteed walkable spawn pad.
    float sx = bp.spawn.x, sy = bp.spawn.y;
    if (sx == 0.0f && sy == 0.0f) {
        float best_top = -1e9f;
        for (const auto& pp : bp.platforms) {
            std::vector<Vec2> poly = pp.polygon;
            if (poly.size()<3) poly=make_rect_polygon(pp.rect[0],pp.rect[1],pp.rect[2],pp.rect[3]);
            float min_x=1e9f, max_x=-1e9f, top=-1e9f;
            for (const auto& v:poly){min_x=std::min(min_x,v.x);max_x=std::max(max_x,v.x);top=std::max(top,v.y);}
            if (0.0f >= min_x - 12.0f && 0.0f <= max_x + 12.0f)
                best_top = std::max(best_top, top);
        }
        sx = 0.0f;
        sy = (best_top > -1e8f ? best_top : maxy) + 56.0f;
    }
    bp.spawn = {sx, sy};
    scene.write_bytes_field(1, extract_object_bytes(
        build_spawn_object("spawn_default", sx, sy, bp.spawn_facing)));
    ++r.objects;

    // ── 6b. ObjectLibrary (Scene field 2) — REQUIRED for decorations ──────
    // This procedural scene is full of TemplateName-reference decorations,
    // torches and props. The game only resolves those names against the
    // libraries the scene imports (ObjectLibrary::TemplateForName searches the
    // imported libraries). Emit the standard import union so every deco/torch
    // template resolves in-game; otherwise they are invisible phantoms and only
    // the baked ground mesh shows. Written before Bounds to keep field order.
    scene.write_bytes_field(2, build_object_library(opt.scene_name, default_scene_imports()));

    // ── 7. Root Bounds (required in 118/118 scenes) ───────────────────────
    if (maxx <= minx || maxy <= miny) { r.error = "procedural: empty AABB"; return r; }
    const float pad = bp.bounds_pad;
    const float bx  = minx - pad, by = miny - pad;
    const float bw  = (maxx - minx) + 2.0f*pad;
    const float bh  = (maxy - miny) + 2.0f*pad;
    scene.write_bytes_field(3, build_bounds_payload(bx, by, bw, bh));
    r.bounds[0]=bx; r.bounds[1]=by; r.bounds[2]=bw; r.bounds[3]=bh;
    r.scene_bytes = scene.to_string();

    // ── 8. Validation (safety net) ────────────────────────────────────────
    {
        std::vector<std::string> msgs;
        Result v = validate_scene(bp, &msgs);
        if (!v.ok()) {
            r.error = "procedural validation: " + v.error;
            if (!msgs.empty()) r.error += " (" + msgs[0] + ")";
            return r;
        }
    }

    return r;
}

} // namespace sgen
