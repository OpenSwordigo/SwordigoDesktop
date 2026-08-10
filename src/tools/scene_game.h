#pragma once
/* scene_game.h — mini-Swordigo game-state layer (Ruby Scene Player backend)
 *
 * The scene player's aim is to be a functional re-creation of the real game
 * inside the Ruby SDK. This module is the *data + simulation core* behind it:
 *
 *   - HeroStats      — the player's real physics, DATA-DRIVEN from hiro.scl
 *                      (CharControllerComponent payload: NormalRunSpeed=230,
 *                      FastRunSpeed=320, NormalMaxJumpTime=0.23, hitbox rect
 *                      {-8,-34,16,56}, depth ±15). No hardcoded guesses —
 *                      parse the .scl like the game does. Falls back to
 *                      engine defaults from the arm32 struct initializer.
 *
 *   - GameWorld      — world-space collision AABBs built once from every
 *                      scene object's CollisionShapeComponent / ShapeComponent
 *                      / GroundPolygonComponent (scene_collision.h) plus the
 *                      terrain heightfield (scene_terrain.h). Static-only:
 *                      entity hitboxes (BoneControlledCollisionShape on
 *                      animated monsters) are excluded — they are dynamic
 *                      damage volumes, not walls.
 *
 *   - resolve_*      — AABB sweeps: horizontal wall collision (push-out) and
 *                      vertical resolve, depth-filtered by the object's Z.
 *
 *   - PlayerData     — the live player record the engine mutates each tick.
 *
 * The scene player (scene_player.cpp) consumes this module; the editor
 * visualizer stays render-only.
 */
#ifndef SCENE_GAME_H_
#define SCENE_GAME_H_

#include <string>
#include <vector>

#include "tools/scene_loader.h"
#include "tools/scene_terrain.h"

namespace sg {

// ─── Hero physics (data-driven from hiro.scl CharControllerComponent) ──────
// CharController payload field numbers (tag >> 3, all WIRE_I32 fixed32):
//   tag 29  → field 3  NormalRunSpeed
//   tag 37  → field 4  JumpSpeed
//   tag 45  → field 5  NormalMaxJumpTime
//   tag 133 → field 16 FastRunSpeed
//   tag 141 → field 17 FastMaxJumpTime
// Nested Rectangle hitbox: {x=-8, y=-34, w=16, h=56} (object-local, origin
// at the feet → rect spans 34 below / 22 above the origin).
struct HeroStats {
    float walk_speed     = 230.0f;   // NormalRunSpeed — the game's base cycle
    float fast_run_speed = 320.0f;   // FastRunSpeed — double-tap fast run
    float jump_speed     = 250.0f;   // JumpSpeed — initial jump impulse
    float max_jump_time  = 0.23f;    // NormalMaxJumpTime — jump hold window
    float fast_max_jump_time = 0.11f; // FastMaxJumpTime — run-jump hold window
    // Ground/air acceleration: the game's EntityController snaps toward the
    // target speed at ~2000 u/s² (scene bindings); we default slightly
    // snappier (2500/1600) so a full run is reached in ~0.1 s. A linear ramp
    // with these rates keeps Hiro responsive — the old exponential lerp took
    // ~0.9 s to reach top speed and felt sluggish.
    float ground_accel  = 2500.0f;
    float air_accel     = 1600.0f;
    float ground_decel  = 2500.0f;
    float air_decel     = 1600.0f;
    // Gravity is coherent with the scl jump impulse: with v=250 and g=250,
    // a tap jump apexes at ~125 units (≈1.2× the 56-unit hitbox height),
    // matching the tuned jump feel — and a full hold rises ~1.6× higher,
    // exactly the game's variable-jump curve.
    float gravity        = 250.0f;
    float hitbox[4]      = {-8.0f, -34.0f, 16.0f, 56.0f}; // x,y,w,h (local)
    float min_depth      = -15.0f;   // hero's depth (Z) collision range
    float max_depth      =  15.0f;
};

/// Load real hero physics from <dir>/hiro.scl (CharControllerComponent).
/// Returns false (leaving @p out untouched) when the scl is missing or has
/// no CharController payload — caller keeps engine defaults.
bool hero_stats_from_scl(const std::string& scl_dir, HeroStats& out);

// ─── World-space static colliders ─────────────────────────────────────────
struct WorldCollider {
    float x = 0, y = 0, w = 0, h = 0;   // world AABB
    float z_min = -1e9f, z_max = 1e9f;  // depth range (entity Z must be inside)
    bool  is_ground = false;            // walkable top surface
    bool  solid     = true;             // blocks horizontal movement
    bool  unsafe    = false;            // kills on touch (lava/spikes)
};

struct GameWorld {
    std::vector<WorldCollider> colliders;
    av::TerrainGrid terrain;            // ground-mesh heightfield
    bool            terrain_built = false;
    // The intelligent wall world (scene_collision.h runtime): every solid /
    // ground / unsafe collision shape and ground polygon baked into clean
    // world-space segments. This is what the Scene Player maps over the
    // scene — visible collision walls — and what entity physics clamps
    // against (av::wall_clamp_circle / wall_raycast).
    av::CollisionWorld walls;
};

/// Build static colliders + terrain from the scene. Excludes animated entity
/// hitboxes (BoneControlledCollisionShape on *MonsterController objects) so
/// monsters don't become walls. Cheap enough to rebuild on scene edits.
void game_build_world(const av::SceneData& scene, GameWorld& out);

/// Walkable ground Y at (x,z): highest collider-top within the entity's depth
/// range that overlaps x (and is within @p step of ref_y), else the terrain
/// heightfield, else @p fallback.
float game_ground_at(const GameWorld& w, float x, float z, float ref_y,
                     float step, float fallback);

/// True + fills when any collider or terrain surface covers (x,z) near ref_y.
bool game_ground_found(const GameWorld& w, float x, float z, float ref_y,
                       float step, float& h);

/// Horizontal AABB sweep for a moving body: returns the X position after
/// push-out against every solid collider whose depth range covers z and
/// whose Y band overlaps [feet_y, feet_y + height].
float game_resolve_x(const GameWorld& w, float x, float feet_y, float z,
                     float half_w, float height);

/// Vertical AABB sweep (falling / rising): returns the Y after resolve; sets
/// @p hit_ground when the body came to rest on a ground collider top.
float game_resolve_y(const GameWorld& w, float x, float y, float z,
                     float half_w, float height, float prev_bottom,
                     bool& hit_ground);

/// True when the body AABB touches an unsafe collider (lava/spikes).
bool game_hits_unsafe(const GameWorld& w, float x, float feet_y, float z,
                      float half_w, float height);

// ─── Player record (the live mini-Swordigo player) ────────────────────────
struct PlayerData {
    float pos[3] = {0, 0, 0};
    float vel[3] = {0, 0, 0};
    bool  grounded = false;
    bool  running  = false;
    float facing   = 1.0f;         // 1 = right, -1 = left
    HeroStats stats;
    int   hp = 100, max_hp = 100;
    int   coins = 0;
    float air_time = 0.0f;         // jump hold timer (max_jump_time window)
};

} // namespace sg

#endif // SCENE_GAME_H_
