#pragma once
/* scene_physics.h — Swordigo physics subsystem data structures
 *
 * Faithfully reconstructed from:
 *   - IDA decompilations: PhysicsObjectComponent, PhysicsObjectState,
 *     PhysicsPlatformComponent (OpenSwordigo/resources/ida_decompiled/)
 *   - scene_schemas.cpp Component schema field numbers:
 *       {1274, "PhysicsObjectComponent"} → payload field_number = 159
 *       {1306, "PhysicsPlatformComponent"} → payload field_number = 163
 *   - arm32 libswordigo_ida32.c PhysicsObjectComponent offset references
 *     @ 0xc4 GroundDeceleration, @ 0xc8 AirDeceleration, @ 0xa4 Gravity,
 *     @ 0xac Elasticity
 *
 * These are read-only parse results, not a runtime physics simulation.
 * Use them for Studio visualization, Ruby SDK property inspection,
 * and Swordfare host initialization.
 */

#include <string>

namespace av {
struct SceneObject;   // forward decl — full definition lives in scene_loader.h

// ─── PhysicsObjectComponent ──────────────────────────────────────────────
// Source: IDA Caver::PhysicsObjectComponent @ 0x27DF4C (arm64)
//         arm32 PhysicsObjectComponent struct offsets:
//           +0xa4 Gravity              (float, default 25.0)
//           +0xac Elasticity           (float, default 0.0)
//           +0xb4 MaxVelocity         (float)
//           +0xc4 GroundDeceleration  (float, default 20.0)
//           +0xc8 AirDeceleration     (float, default 2.0)
//
// Protobuf schema (scene_schemas.cpp PhysicsObjectComponent):
//   Schema has no explicit fields listed — the component is present/absent.
//   Runtime defaults from arm32 struct initializer are used when present.
//   The PhysicsEnabled binding on EntityComponent (field 16) gates physics.
struct PhysicsData {
    bool  enabled              = true;   // PhysicsObjectComponent present → physics on

    // These are the engine-default values from arm32 struct initializer.
    // They cannot be overridden per-object from scene files in stock Swordigo
    // (the protobuf schema for PhysicsObjectComponent is empty), but are
    // exposed here so Swordfare host can set them.
    float gravity              = 25.0f;  // arm32 +0xa4 default
    float elasticity           = 0.0f;   // arm32 +0xac default
    float ground_deceleration  = 20.0f;  // arm32 +0xc4 default
    float air_deceleration     = 2.0f;   // arm32 +0xc8 default

    // PhysicsPlatformComponent: object acts as a moving or spring platform.
    bool  is_platform          = false;
    float platform_mass        = 0.0f;   // field 13 : Mass
    float platform_spring      = 0.0f;   // field 21 : SpringForce
};

/// Parse physics data from a SceneObject's component list.
/// Returns PhysicsData with enabled=false if no physics component is found.
PhysicsData physics_parse(const SceneObject& obj);

// ============================================================================
// RUNTIME PHYSICS BODY — Caver::PhysicsObjectState port
// ============================================================================
// Faithful reconstruction of PhysicsObjectState::Update (UpdateSpeedComponents
// @0x2800CC + UpdateObjectState @0x280348), HandleGroundCollision @0x280658
// and AdjustGroundCollisionVector @0x2805A0. Field layout mirrors the
// decompiled struct (offsets annotated below).
//
// Update order per frame (the game's PhysicsObjectComponent::Update):
//   1. physics_step()            — accel ramp toward the controller's target
//      velocity, friction decay, max-velocity clamp (UpdateSpeedComponents
//      + UpdateObjectState)
//   2. physics_ground_collision() — respond to a wall/floor contact normal
//      (HandleGroundCollision: project velocity onto the surface frame,
//      slide along the wall, zero the normal component)
struct PhysicsBody {
    // ── state (mirrors PhysicsObjectState) ──
    float pos[2]       = {0, 0};   // +112,+116 on the SceneObject (world)
    float vel[2]       = {0, 0};   // +104,+108 velocity (vx, vy)
    float move_dir[2]  = {1, 0};   // +16,+20  intended movement direction
    float speed_mult   = 1.0f;     // +112    speed multiplier
    float ramp         = 0.0f;     // +48     accel ramp 0..1 (+0.5·dt/frame)
    float accel        = 60.0f;    // +60     UpdateSpeedComponents accel rate
    float vel_target_x = 0.0f;     // +56     velocity target — a MAGNITUDE
                                   //         along the surface-frame +X (the
                                   //         move direction from move_dir);
                                   //         the frame supplies the sign
    float vel_target_y = 0.0f;     // +64     vertical velocity target (bits)
    float friction     = 0.0f;     // +72     surface friction (decay per s)
    float max_velocity = 0.0f;     // +128    max |velocity| (0 = unlimited)
    bool  max_velocity_enabled = false; // +124
    bool  moving       = false;    // +101   has a movement direction
    bool  grounded     = false;    // +68
    bool  physics_enabled = true;  // +116   master enable
    // ── ground collision frame (the 2×2 surface matrix +76..+88) ──
    float ground_normal[2] = {0, 1};     // the surface normal we rest on
    float frame[4] = {1, 0, 0, 1};       // surface basis (identity = flat)
    // ── platform link (PhysicsPlatformComponent) ──
    bool  on_platform = false;
    float platform_vel[2] = {0, 0};      // moving-platform velocity
};

/// PhysicsObjectState::Update — speed components + object state in one step.
/// dt in seconds. Ramp accelerates toward vel_target_x/vel_target_y at
/// `accel` (capped by max_velocity), friction decays the surface-frame
/// velocity, then position integrates.
void physics_step(PhysicsBody& b, float dt);

/// Set the movement direction (normalized) — drives the surface frame + ramp.
void physics_set_move_dir(PhysicsBody& b, float dx, float dy);

/// PhysicsObjectState::HandleGroundCollision — given a contact normal
/// (world space) and penetration, project the velocity into the surface
/// frame, kill the normal component (slide along the wall / rest on floor)
/// and update the grounded flag + frame.
void physics_ground_collision(PhysicsBody& b, const float normal[2],
                              float dt);

/// PhysicsObjectState::AdjustGroundCollisionVector — adjust a collision
/// vector + magnitude against the body's surface frame (used when sliding
/// along slopes).
float physics_adjust_ground_collision(const PhysicsBody& b, float vec[2],
                                      float mag);

} // namespace av
