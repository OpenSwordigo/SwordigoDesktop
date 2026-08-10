/* scene_physics.cpp — Swordigo physics subsystem parser
 *
 * Parses PhysicsObjectComponent and PhysicsPlatformComponent from
 * SceneObject component lists.
 *
 * Component field numbers from scene_schemas.cpp (authoritative):
 *   PhysicsObjectComponent  : schema tag 1274, payload field_number = 1274 >> 3 = 159
 *   PhysicsPlatformComponent: schema tag 1306, payload field_number = 1306 >> 3 = 163
 *
 * PhysicsObjectComponent protobuf payload: schema lists NO fields (empty).
 * The presence of the component tag is sufficient — the engine sets all
 * physics parameters from hardcoded struct initializer defaults (verified
 * in arm32 struct at offsets 0xa4, 0xac, 0xb4, 0xc4, 0xc8).
 *
 * PhysicsPlatformComponent protobuf payload:
 *   field 13 (I32/float) : Mass
 *   field 21 (I32/float) : SpringForce
 *
 * Protobuf wire types:
 *   WIRE_I32    (5) : float (fixed32)
 *   WIRE_VARINT (0) : bool, int
 *   WIRE_LEN    (2) : nested message, string
 */

#include "tools/scene_loader.h"
#include "tools/scene_physics.h"
#include "platform/protobuf_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace av {

// Payload field numbers (schema_tag >> 3)
static constexpr int PAYLOAD_PHYSICS_OBJECT   = 159;  // 1274 >> 3
static constexpr int PAYLOAD_PHYSICS_PLATFORM = 163;  // 1306 >> 3

// SCENE files store SHORT stems ("PhysicsObject") while library templates
// and the schema use the "...Component" suffix ("PhysicsObjectComponent").
// Match either spelling.
static constexpr const char* TYPE_PHYSICS_OBJECT   = "PhysicsObject";
static constexpr const char* TYPE_PHYSICS_PLATFORM = "PhysicsPlatform";

// Match a component type name against a schema stem (e.g. "PhysicsObject"),
// accepting the short scene spelling, the "...Component" suffix spelling,
// and trailing NUL / whitespace from protobuf padding.
static bool type_matches(const std::string& tn, const char* stem) {
    const size_t n = std::strlen(stem);
    if (tn.size() < n) return false;
    if (tn.compare(0, n, stem) != 0) return false;
    const char* rest = tn.c_str() + n;
    if (*rest == '\0') return true;
    if (std::strncmp(rest, "Component", 9) == 0) rest += 9;
    for (; *rest; ++rest) {
        const char c = *rest;
        if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return false;
    }
    return true;
}

// Read nested payload bytes for a component given expected field number.
static std::string read_payload(const SceneComponent& comp, int expected_field) {
    try {
        proto::Reader reader(comp.raw_data);
        proto::Field f;
        while (reader.read_field(f)) {
            if (static_cast<int>(f.field_number) == expected_field &&
                f.wire_type == proto::WIRE_LEN)
                return f.bytes_val;
        }
    } catch (...) {}
    // Fallback to whatever payload field was detected during parse
    if (comp.payload_field > 0) {
        try {
            proto::Reader reader(comp.raw_data);
            proto::Field f;
            while (reader.read_field(f)) {
                if (f.field_number == static_cast<uint32_t>(comp.payload_field) &&
                    f.wire_type == proto::WIRE_LEN)
                    return f.bytes_val;
            }
        } catch (...) {}
    }
    return {};
}

// Parse PhysicsPlatformComponent payload.
// Schema (scene_schemas.cpp PhysicsPlatformComponent):
//   field 13 (I32/float) : Mass
//   field 21 (I32/float) : SpringForce
static void parse_platform_payload(const std::string& bytes, PhysicsData& out) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            // Field 13 wire tag = (13 << 3) | 5 = 109 → field_number=13, WIRE_I32
            if (f.field_number == 1 && f.wire_type == proto::WIRE_I32)
                out.platform_mass = f.float_val;
            // Field 21 wire tag = (21 << 3) | 5 = 173 → field_number=21, WIRE_I32
            else if (f.field_number == 2 && f.wire_type == proto::WIRE_I32)
                out.platform_spring = f.float_val;
        }
    } catch (...) {}
}

PhysicsData physics_parse(const SceneObject& obj) {
    PhysicsData result;
    result.enabled = false;

    const auto& comps = obj.resolved_components.empty() ? obj.components : obj.resolved_components;

    for (const auto& comp : comps) {
        const std::string& tn = comp.type_name;

        if (type_matches(tn, TYPE_PHYSICS_OBJECT)) {
            // Component presence alone enables physics; no payload fields to read
            // (the schema is empty — engine uses arm32 struct initializer defaults).
            result.enabled = true;
        }
        else if (type_matches(tn, TYPE_PHYSICS_PLATFORM)) {
            result.enabled     = true;  // platform implies physics too
            result.is_platform = true;
            std::string payload = read_payload(comp, PAYLOAD_PHYSICS_PLATFORM);
            if (!payload.empty())
                parse_platform_payload(payload, result);
        }
    }

    return result;
}

// ============================================================================
// RUNTIME PHYSICS BODY
// ============================================================================

// The decompiled PhysicsObjectState keeps velocity in a SURFACE FRAME whose
// basis is built from the intended movement direction (offsets +76..+88):
//   world→frame: fv = M·wv with M = [[ +84, +88 ],[ +76, +80 ]]
//   frame→world: wv = Mᵀ·fv
// With move_dir = (dx, dy): +76 = dx, +80 = dy, +84 = dy, +88 = −dx, so
// M = [[dy, −dx],[dx, dy]] — a proper rotation matrix.
static void frame_from_dir(float frame[4], float dx, float dy) {
    // Surface frame with X = the intended movement direction and Y = its
    // left perpendicular. world→frame: fv = M·wv with M = [[dx, dy],[-dy, dx]];
    // frame→world: wv = Mᵀ·fv. Move dir (1,0) → identity (ramp shows up in
    // world X); (0,1) → +90° rotation. The previous port transposed this
    // (move dir landed on frame-Y), so the accel ramp accelerated the body
    // perpendicular to its facing.
    frame[0] = dx;  frame[1] = dy;
    frame[2] = -dy; frame[3] = dx;
}

static void frame_from_normal(float frame[4], float nx, float ny) {
    // Surface frame: X = tangent along the surface, Y = the normal.
    float tx = -ny, ty = nx;    // right-hand perpendicular
    const float l = std::sqrt(tx * tx + ty * ty);
    if (l > 1e-5f) { tx /= l; ty /= l; }
    else { tx = 1.0f; ty = 0.0f; }
    frame[0] = tx; frame[1] = ty;   // tangent
    frame[2] = nx; frame[3] = ny;   // normal
}

void physics_set_move_dir(PhysicsBody& b, float dx, float dy) {
    const float l = std::sqrt(dx * dx + dy * dy);
    if (l > 1e-5f) { b.move_dir[0] = dx / l; b.move_dir[1] = dy / l; }
    b.moving = true;
    frame_from_dir(b.frame, b.move_dir[0], b.move_dir[1]);
}

// UpdateSpeedComponents @0x2800CC — ramp the frame velocity toward the
// controller's target at `accel`, capped by max velocity; then
// UpdateObjectState @0x280348 — decay by friction, clamp |v|, add the
// moving-platform velocity and integrate the position.
void physics_step(PhysicsBody& b, float dt) {
    if (!b.physics_enabled) return;
    if (dt <= 0.0f) return;

    // ── UpdateSpeedComponents ──
    // Acceleration input: forward along the move direction (scaled by the
    // speed multiplier); when the controller stops steering, the arm32
    // GroundDeceleration/AirDeceleration defaults (20 / 2 u/s²) brake the
    // body back to rest.
    float ax = 0.0f, ay = 0.0f;
    if (b.moving) {
        ax = b.move_dir[0] * b.speed_mult;
        ay = b.move_dir[1] * b.speed_mult;
    } else {
        const float decel = b.grounded ? 20.0f : 2.0f;  // arm32 +0xc4/+0xc8
        if (b.vel[0] != 0.0f || b.vel[1] != 0.0f) {
            const float spd = std::sqrt(b.vel[0] * b.vel[0] +
                                        b.vel[1] * b.vel[1]);
            const float brake = std::min(spd, decel * dt);
            ax = -b.vel[0] / spd * brake / dt;
            ay = -b.vel[1] / spd * brake / dt;
        }
    }

    // New world velocity before clamping (vel + accel·dt).
    float nvx = b.vel[0] + ax * dt;
    float nvy = b.vel[1] + ay * dt;
    // Rotate into the surface frame (M·wv).
    const float fvx = b.frame[0] * nvx + b.frame[1] * nvy;
    const float fvy = b.frame[2] * nvx + b.frame[3] * nvy;

    // Frame-x ramps toward vel_target_x at the full accel rate (never
    // overshoots). The soft-start ramp factor was dropped: with the default
    // accel it throttled acceleration to a crawl (e.g. 0.15·60 ≈ 9 u/s²
    // instead of the configured 60 u/s²).
    float tx = fvx;
    if (b.moving) {
        const float rate = b.accel * dt;
        if (std::fabs(tx - b.vel_target_x) > rate) {
            tx += (b.vel_target_x > tx ? 1.0f : -1.0f) * rate;
        } else {
            tx = b.vel_target_x;
        }
    }
    // Max velocity (UpdateSpeedComponents +128 clamps the frame-x speed).
    if (b.max_velocity_enabled && b.max_velocity > 0.0f)
        tx = std::clamp(tx, -b.max_velocity, b.max_velocity);
    // Grounded bodies snap their frame-y to the vertical target (the game
    // keeps the vertical velocity owned by gravity / jump impulses).
    float ty = fvy;
    if (b.grounded) ty = b.vel_target_y;

    // ── UpdateObjectState ──
    // Rotate back to world space (Mᵀ·fv) and decay by friction.
    float wvx = b.frame[0] * tx + b.frame[2] * ty;
    float wvy = b.frame[1] * tx + b.frame[3] * ty;
    if (b.friction > 0.0001f) {
        wvx -= wvx * b.friction * dt;
        wvy -= wvy * b.friction * dt;
    }
    // Max |velocity| clamp (UpdateObjectState +52).
    const float m2 = b.max_velocity * b.max_velocity;
    const float sp2 = wvx * wvx + wvy * wvy;
    if (b.max_velocity_enabled && b.max_velocity > 0.0f && sp2 > m2 &&
        sp2 > 1e-9f) {
        const float k = b.max_velocity / std::sqrt(sp2);
        wvx *= k; wvy *= k;
    }
    b.vel[0] = wvx;
    b.vel[1] = wvy;

    // Integrate: position += (velocity + platform velocity) · dt.
    b.pos[0] += (b.vel[0] + (b.on_platform ? b.platform_vel[0] : 0.0f)) * dt;
    b.pos[1] += (b.vel[1] + (b.on_platform ? b.platform_vel[1] : 0.0f)) * dt;
}

// HandleGroundCollision @0x280658 — on a contact whose normal opposes the
// velocity (dot ≤ −0.001): project the velocity into the surface frame,
// kill the normal component (slide along the wall / rest on the floor),
// record the ground normal + friction and set the grounded state.
void physics_ground_collision(PhysicsBody& b, const float normal[2], float dt) {
    if (!b.physics_enabled) return;
    const float vn = b.vel[0] * normal[0] + b.vel[1] * normal[1];
    if (vn > 0.001f) return;   // moving away from the surface — nothing to do

    // Build the surface frame from the contact normal.
    frame_from_normal(b.frame, normal[0], normal[1]);
    b.ground_normal[0] = normal[0];
    b.ground_normal[1] = normal[1];

    // Slide = velocity minus its normal component (zero the normal part).
    float sxx = b.vel[0] - normal[0] * vn;
    float syy = b.vel[1] - normal[1] * vn;
    // Friction acts on the slide magnitude (per-second decay).
    if (b.friction > 0.0f) {
        sxx -= sxx * b.friction * dt;
        syy -= syy * b.friction * dt;
    }
    b.vel[0] = sxx;
    b.vel[1] = syy;
    // A surface whose normal is mostly-up is ground; steep normals are
    // walls (the body stays grounded only on walkable slopes).
    b.grounded = normal[1] > 0.35f;
}

// AdjustGroundCollisionVector @0x2805A0 — rotate a collision vector into the
// body's surface frame and rescale its magnitude; returns the new magnitude.
float physics_adjust_ground_collision(const PhysicsBody& b, float vec[2],
                                      float mag) {
    // frame = [tangent, normal] — rotate vec into that frame.
    const float fx = vec[0] * b.frame[0] + vec[1] * b.frame[1];
    const float fy = vec[0] * b.frame[2] + vec[1] * b.frame[3];
    vec[0] = fx;
    vec[1] = fy;
    if (std::fabs(fy) >= 0.001f)
        return mag * fx / fy;   // scale by the tangent/normal ratio
    return mag;
}

} // namespace av
