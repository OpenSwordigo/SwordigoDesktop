/* scene_game.cpp — mini-Swordigo game-state layer implementation.
 *
 * See scene_game.h. Two jobs:
 *
 *  1. Read the player's real physics from hiro.scl (CharControllerComponent
 *     payload), mirroring parse_object_library from scene_loader.cpp so we
 *     stay data-driven instead of hardcoding walk/run/jump values.
 *
 *  2. Bake the scene's static collision shapes + terrain into a world space
 *     the scene player can query per tick (ground height, horizontal walls,
 *     unsafe surfaces) — the pieces the editor never needed.
 */
#include "scene_game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "platform/protobuf_reader.h"

namespace sg {

// ============================================================================
// Hero stats — parse hiro.scl like the real engine (data-driven)
// ============================================================================

// Walk one ObjectLibrary message: field 2 = ObjectTemplate → field 1 =
// SceneObject. Returns the first SceneObject whose components contain a
// CharControllerComponent payload.
static bool find_char_controller(const std::string& library_bytes,
                                 std::string& payload_out) {
    try {
        proto::Reader lib(library_bytes);
        proto::Field tf;
        while (lib.read_field(tf)) {
            if (tf.field_number != 2 || tf.wire_type != proto::WIRE_LEN)
                continue;
            // ObjectTemplate
            proto::Reader tpl(tf.bytes_val);
            proto::Field tpl_f;
            while (tpl.read_field(tpl_f)) {
                if (tpl_f.field_number != 1 || tpl_f.wire_type != proto::WIRE_LEN)
                    continue;
                // SceneObject → field 3 = Component (repeated)
                proto::Reader obj(tpl_f.bytes_val);
                proto::Field of;
                while (obj.read_field(of)) {
                    if (of.field_number != 3 || of.wire_type != proto::WIRE_LEN)
                        continue;
                    // Component → type_name (field 1) + payload (field >= 50)
                    proto::Reader comp(of.bytes_val);
                    proto::Field cf;
                    std::string type_name, payload;
                    while (comp.read_field(cf)) {
                        if (cf.field_number == 1 && cf.wire_type == proto::WIRE_LEN)
                            type_name = cf.bytes_val;
                        else if (cf.field_number >= 50 && cf.wire_type == proto::WIRE_LEN &&
                                 payload.empty())
                            payload = cf.bytes_val;
                    }
                    if (type_name.find("CharController") != std::string::npos) {
                        payload_out = std::move(payload);
                        return true;
                    }
                }
            }
        }
    } catch (...) {}
    return false;
}

// Read a fixed32 field from a payload message.
static float payload_float(const std::string& payload, unsigned field_num,
                           float fallback) {
    try {
        proto::Reader r(payload);
        proto::Field f;
        while (r.read_field(f)) {
            if (f.field_number == field_num && f.wire_type == proto::WIRE_I32)
                return f.float_val;
        }
    } catch (...) {}
    return fallback;
}

// Parse a nested Rectangle message ({x=1, y=2, w=3, h=4} fixed32) from a
// length-delimited field of the payload. The CharController payload's own
// fields 3/4 are NormalRunSpeed/JumpSpeed — NEVER read those as a rect.
static void payload_rect(const std::string& payload, float out[4]) {
    try {
        proto::Reader r(payload);
        proto::Field f;
        while (r.read_field(f)) {
            if (f.wire_type != proto::WIRE_LEN) continue;
            // Candidate nested Rectangle: exactly 4 fixed32 in {x,y,w,h}.
            float r4[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
            bool  got[4] = {false, false, false, false};
            try {
                proto::Reader inner(f.bytes_val);
                proto::Field g;
                while (inner.read_field(g)) {
                    if (g.wire_type != proto::WIRE_I32) continue;
                    if (g.field_number >= 1 && g.field_number <= 4) {
                        r4[g.field_number - 1] = g.float_val;
                        got[g.field_number - 1] = true;
                    }
                }
            } catch (...) {}
            // Plausible hitbox: small width/height, sane origin (the hero's
            // is {-8,-34,16,56}). Reject anything that looks like an anim
            // controller id or a huge coordinate.
            if (got[2] && got[3] && r4[2] > 0.0f && r4[2] < 400.0f &&
                r4[3] > 0.0f && r4[3] < 400.0f &&
                std::fabs(r4[0]) < 2000.0f && std::fabs(r4[1]) < 2000.0f) {
                for (int i = 0; i < 4; ++i) out[i] = r4[i];
                return;
            }
        }
    } catch (...) {}
}

bool hero_stats_from_scl(const std::string& scl_dir, HeroStats& out) {
    // Candidate roots: the scene dir itself, or the global resources tree.
    std::vector<std::string> candidates;
    if (!scl_dir.empty()) {
        candidates.push_back(scl_dir + "/hiro.scl");
        candidates.push_back(scl_dir + "/resources/hiro.scl");
    }
    const char* home = getenv("HOME");
    if (home) {
        candidates.push_back(std::string(home) +
            "/.local/share/swordigo-desktop/assets/resources/hiro.scl");
    }
    candidates.push_back("assets/resources/hiro.scl");

    for (const auto& path : candidates) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        if (bytes.empty()) continue;
        std::string payload;
        if (!find_char_controller(bytes, payload) || payload.empty())
            continue;
        // Real values — data-driven, from the game's own scl.
        out.walk_speed         = payload_float(payload, 3,  out.walk_speed);
        out.jump_speed         = payload_float(payload, 4,  out.jump_speed);
        out.max_jump_time      = payload_float(payload, 5,  out.max_jump_time);
        out.fast_run_speed     = payload_float(payload, 16, out.fast_run_speed);
        out.fast_max_jump_time = payload_float(payload, 17, out.fast_max_jump_time);
        payload_rect(payload, out.hitbox);
        return true;
    }
    return false;
}

// ============================================================================
// World colliders
// ============================================================================

// True when the object is an animated entity whose hitboxes are dynamic
// (monsters/hero) — their BoneControlledCollisionShape is a damage volume,
// not a static wall, so it must never block the player.
static bool is_animated_entity(const av::SceneObject& o) {
    const auto& comps = o.resolved_components.empty() ? o.components
                                                      : o.resolved_components;
    for (const auto& c : comps) {
        const std::string& t = c.type_name;
        if (t.find("MonsterDeathController") != std::string::npos)
            continue;
        if (t.find("MonsterController") != std::string::npos ||
            t.find("CharAnimController") != std::string::npos ||
            t.find("CharController") != std::string::npos ||
            t.find("HeroEntity") != std::string::npos)
            return true;
    }
    return false;
}

// Expand an object-local shape into a world AABB + depth range.
static void add_shape_collider(std::vector<WorldCollider>& out,
                               const av::SceneObject& o,
                               const av::CollisionShapeData& s) {
    WorldCollider c;
    c.z_min = o.pos_z + s.min_depth;
    c.z_max = o.pos_z + s.max_depth;
    c.is_ground = s.is_ground;
    c.solid     = s.collides && s.enabled;
    c.unsafe    = s.unsafe_ground;

    const float sx = o.scale_x > 0.0f ? o.scale_x : 1.0f;
    const float sy = o.scale_y > 0.0f ? o.scale_y : 1.0f;

    switch (s.type) {
    case av::COLL_RECT: {
        c.x = o.pos_x + s.rect[0] * sx;
        c.y = o.pos_y + s.rect[1] * sy;
        c.w = s.rect[2] * sx;
        c.h = s.rect[3] * sy;
        break;
    }
    case av::COLL_POLYGON: {
        // Tight AABB around the local polygon.
        if (s.polygon_points.size() < 4) return;
        float minx = s.polygon_points[0], maxx = s.polygon_points[0];
        float miny = s.polygon_points[1], maxy = s.polygon_points[1];
        for (size_t i = 2; i + 1 < s.polygon_points.size(); i += 2) {
            minx = std::min(minx, s.polygon_points[i]);
            maxx = std::max(maxx, s.polygon_points[i]);
            miny = std::min(miny, s.polygon_points[i + 1]);
            maxy = std::max(maxy, s.polygon_points[i + 1]);
        }
        c.x = o.pos_x + minx * sx;
        c.y = o.pos_y + miny * sy;
        c.w = (maxx - minx) * sx;
        c.h = (maxy - miny) * sy;
        break;
    }
    case av::COLL_CIRCLE: {
        // Circle → tight AABB collider (object collision parity with rects).
        const float r = s.circle_radius * std::max(sx, sy);
        if (r <= 0.0f) return;
        c.x = o.pos_x + s.circle_center[0] * sx - r;
        c.y = o.pos_y + s.circle_center[1] * sy - r;
        c.w = 2.0f * r;
        c.h = 2.0f * r;
        break;
    }
    default:
        return;   // unknown shapes: no collider
    }
    if (c.w <= 0.0f || c.h <= 0.0f) return;
    out.push_back(c);
}

void game_build_world(const av::SceneData& scene, GameWorld& out) {
    out.colliders.clear();
    for (const auto& o : scene.objects) {
        if (o.hidden) continue;
        const bool animated = is_animated_entity(o);
        const av::CollisionData cd = av::collision_parse(o);
        for (const auto& s : cd.shapes) {
            if (animated)
                continue;   // monster/hero hitboxes are dynamic, not walls
            add_shape_collider(out.colliders, o, s);
        }
        // NOTE: GroundPolygonComponent surfaces are NOT added as solid
        // colliders here — terrain_build() below already bakes them into the
        // walkable heightfield (their top edge, not their AABB top). Adding
        // them again as AABB colliders would (a) glue entities to the highest
        // polygon vertex and (b) false-positive unsafe_ground under the whole
        // polygon → instant respawn loop → the "scene shakes" bug.
    }
    // Terrain heightfield (ground meshes + ground polygons) for slopes.
    av::terrain_build(scene, out.terrain);
    out.terrain_built = true;

    // The intelligent wall world: every solid/ground/unsafe collision shape
    // and ground polygon baked into clean world-space segments (with merge
    // simplification + outward normals). Drives the visible collision walls
    // overlay and entity wall clamping.
    av::collision_build_world(scene, out.walls);
}

// ============================================================================
// Queries
// ============================================================================

// A ground surface under the feet: the collider's top edge must be at or
// below the feet (within step) — a platform 300 units ABOVE the head must
// never read as "ground" (that glued Hiro to ceilings → shaking/stuck).
static bool collider_covers(const WorldCollider& c, float x, float z,
                            float ref_y, float step, float& top) {
    if (z < c.z_min || z > c.z_max) return false;
    if (x < c.x || x > c.x + c.w) return false;
    top = c.y + c.h;
    if (top < ref_y - step) return false;   // too far below feet (pit)
    if (top > ref_y + 4.0f) return false;   // above feet → ceiling, not floor
    return true;
}

// Pick the walkable surface NEAREST the feet (same rule as terrain_height_near:
// closest, not highest — a low floor wins over a distant platform).
static bool better_ground(float cand, float ref_y, bool any, float& best) {
    if (!any || std::fabs(cand - ref_y) < std::fabs(best - ref_y)) {
        best = cand;
        return true;
    }
    return false;
}

float game_ground_at(const GameWorld& w, float x, float z, float ref_y,
                     float step, float fallback) {
    float best = 0.0f;
    bool  any  = false;
    // 1) Solid collider tops (platform shapes + static objects; collider_covers
    //    rejects wall tops above ref_y+4, so pure walls never become ground).
    for (const auto& c : w.colliders) {
        if (!c.solid) continue;
        float top = 0.0f;
        if (collider_covers(c, x, z, ref_y, step, top) &&
            better_ground(top, ref_y, any, best))
            any = true;
    }
    // 2) Terrain heightfield (ground meshes + ground polygons).
    float h = 0.0f;
    if (w.terrain_built &&
        av::terrain_height_near(w.terrain, x, z, ref_y, step, h) &&
        better_ground(h, ref_y, any, best))
        any = true;
    return any ? best : fallback;
}

bool game_ground_found(const GameWorld& w, float x, float z, float ref_y,
                       float step, float& h) {
    float best = 0.0f;
    bool  any  = false;
    // Any SOLID collider can be walked on when its top is at/below the feet
    // (collider_covers already rejects tops above ref_y+4). This makes static
    // collision-shape objects — crates, pillars, steps — fully standable.
    for (const auto& c : w.colliders) {
        if (!c.solid) continue;
        float top = 0.0f;
        if (collider_covers(c, x, z, ref_y, step, top) &&
            better_ground(top, ref_y, any, best))
            any = true;
    }
    float th = 0.0f;
    if (w.terrain_built &&
        av::terrain_height_near(w.terrain, x, z, ref_y, step, th) &&
        better_ground(th, ref_y, any, best))
        any = true;
    if (any) { h = best; return true; }
    return false;
}

float game_resolve_x(const GameWorld& w, float x, float feet_y, float z,
                     float half_w, float height) {
    // Standing tolerance: when the feet rest ON a surface (within 4 units of
    // its top) that collider is a floor, not a wall — never push sideways off
    // it. Without this, ground glue + resolve fought each frame (stuck/jitter).
    const float kStandTol = 4.0f;
    // Multi-pass (fixed-point): with two adjacent solids (wall + floor edge)
    // a single pass can push the body into the neighbour and back — iterate
    // until no overlap or a few passes, so corners don't jitter.
    for (int pass = 0; pass < 3; ++pass) {
        bool any_overlap = false;
        const float left  = x - half_w;
        const float right = x + half_w;
        const float bottom = feet_y;
        const float top    = feet_y + height;
        for (const auto& c : w.colliders) {
            if (!c.solid) continue;
            if (z < c.z_min || z > c.z_max) continue;
            // Skip when standing on this surface (feet at/below its top).
            if (bottom >= c.y + c.h - kStandTol) continue;
            if (bottom >= c.y + c.h || top <= c.y) continue;
            if (right > c.x && left < c.x + c.w) {
                const float push_left  = (c.x + c.w) - left;    // move left out
                const float push_right = right - c.x;           // move right out
                x += (push_left < push_right) ? -push_left : push_right;
                any_overlap = true;
            }
        }
        if (!any_overlap) break;
    }
    return x;
}

float game_resolve_y(const GameWorld& w, float x, float y, float z,
                     float half_w, float height, float prev_bottom,
                     bool& hit_ground) {
    const float left  = x - half_w;
    const float right = x + half_w;
    hit_ground = false;
    for (const auto& c : w.colliders) {
        if (!c.solid) continue;
        if (z < c.z_min || z > c.z_max) continue;
        if (right <= c.x || left >= c.x + c.w) continue;
        const float c_top = c.y + c.h;
        const float top   = y + height;
        const float prev_top = prev_bottom + height;

        // ── Landing on top (falling onto the surface) — swept. ──
        // The swept test catches a fast fall that crosses a THIN platform's
        // top entirely within one frame (the old discrete test missed it →
        // "fell through then teleported back"). Land ONLY when the body came
        // from ABOVE the top plane (prev_bottom at/above c_top) and its feet
        // are now at/below it. A body RISING from below (jumping up under a
        // platform) must never land here — previously the extra
        // "straddles_now && prev_top >= c.y" clause snapped it ON TOP of
        // platforms it was jumping up under (the teleport-through-platform
        // bug). A body that drifted sideways into the x-range with feet below
        // the top (the corner case) is a side-wall contact, resolved
        // horizontally — never a snap-up.
        // Landing works on ANY solid collider (crates, pillars, platforms —
        // not just is_ground/unsafe ones), so the hero and entities can stand
        // on collision-shape objects instead of falling through them. The
        // came-from-above guard keeps pure walls (tall sides) from being
        // stepped on when walked into sideways.
        if (c.solid) {
            const bool came_from_above = prev_bottom >= c_top - 0.5f;
            if (came_from_above && y < c_top) {
                y = c_top;
                hit_ground = true;
                continue;   // landed — this collider can't also be a ceiling
            }
        }
        // ── Ceiling (rising into the underside) — swept. ──
        // The body top was BELOW the collider's bottom last frame and is now
        // inside its vertical span (or just past it in one frame): push back
        // down. This makes jumping up under a platform collide instead of
        // clipping through — previously ground/unsafe colliders never bumped
        // heads at all ("stand under a mesh and jump → you go straight up
        // through it"). The `top < c_top + height` bound keeps a body whose
        // head crossed the WHOLE collider in one huge step from being
        // mis-classified (it is a landing above, handled by the swept land).
        if (prev_top <= c.y + 0.5f && top > c.y && top < c_top + height) {
            y = c.y - height;
            hit_ground = false;   // not a landing — a ceiling
            continue;
        }
    }
    return y;
}

bool game_hits_unsafe(const GameWorld& w, float x, float feet_y, float z,
                      float half_w, float height) {
    const float left = x - half_w, right = x + half_w;
    const float bottom = feet_y, top = feet_y + height;
    for (const auto& c : w.colliders) {
        if (!c.unsafe) continue;
        if (z < c.z_min || z > c.z_max) continue;
        if (right > c.x && left < c.x + c.w &&
            bottom < c.y + c.h && top > c.y)
            return true;
    }
    return false;
}

} // namespace sg
