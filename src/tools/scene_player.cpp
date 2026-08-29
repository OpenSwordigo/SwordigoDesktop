/* scene_player.cpp — Ruby Scene Player subsystem implementation.
 *
 * See scene_player.h for the design. The engine simulates the scene in
 * real time: a shared clock, per-object POD animation clocks, simple AI
 * (patrol / bob / flicker for controllers), and an optional playable Hiro
 * with A/D + Space controls, gravity and animation switching (hiro_* PODs).
 *
 * Rendering stays in the visualizer (asset_viewer.cpp) — this module only
 * mutates the scene's transforms + frames and exposes camera/overlay state.
 */
#include "scene_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "tools/pod_loader.h"
#include "tools/scene_lua.h"
#include "tools/scene_terrain.h"
#include "tools/scene_game.h"
#include "tools/scene_entity.h"
#include "tools/scene_collision.h"

// (imgui.h + icons already pulled in via scene_player.h)

namespace fs = std::filesystem;

namespace sp {

static constexpr float kPi = 3.14159265358979323846f;

// ============================================================================
// Helpers
// ============================================================================

int player_find_spawn(const av::SceneData& scene) {
    for (int i = 0; i < (int)scene.objects.size(); ++i)
        if (scene.objects[i].is_spawn_point)
            return i;
    return -1;
}

const char* player_mode_name(Mode m) {
    switch (m) {
        case Mode::Off:        return "Off";
        case Mode::Visualise:  return "Visualise Scene Playing";
        case Mode::PlayHiro:   return "Spawn Hiro and Play";
    }
    return "Off";
}

static bool is_animated(const av::PODModel& m) { return m.num_frames > 1; }

// True when the object is a real animated entity (monster/hero), NOT a
// prop/switch. Doors and chests carry DoorController + AnimationController
// but no MonsterController/Char* component — they must stay closed.
bool is_animated_entity(const av::SceneObject& o) {
    for (const auto& c : o.resolved_components.empty() ? o.components
                                                       : o.resolved_components) {
        const std::string t = c.type_name;   // copy: may carry trailing NUL/space
        // *MonsterController = real AI entity (Bat/Walking/Skelly/Charging/
        // Leaping/...). Substring match is robust against protobuf-padded
        // type names ("BatMonsterController\0" etc.). MonsterDeathController
        // is NOT a living entity — exclude it explicitly.
        if (t.find("MonsterDeathController") == std::string::npos &&
            t.find("MonsterController") != std::string::npos)
            return true;
        // Char*/HeroEntity = playable/scripted actors. NOTE: MonsterEntity /
        // EntityInfo alone are NOT enough (lava, shadowblobs, emitters carry
        // them) — real monsters always pair them with a *MonsterController.
        if (t.find("CharAnimController") != std::string::npos ||
            t.find("CharController") != std::string::npos ||
            t.find("HeroEntity") != std::string::npos ||
            t.find("EntityController") != std::string::npos)
            return true;
    }
    return false;
}

// Does the object have AI that should move during playback?
static bool has_ai(const av::SceneObject& o) { return is_animated_entity(o); }

// Walkable ground Y at a world (x, z): the game world's collider tops + terrain
// heightfield. Uses the surface closest to the entity's current feet height
// (@p ref_y) so an entity on a lower floor never snaps up onto a bridge above it.
static float ground_y(const Player& p, float x, float z, float ref_y,
                      float fallback) {
    return sg::game_ground_at(p.game_world, x, z, ref_y, 80.0f, fallback);
}

// True + fills when the world actually covers (x, z) near the reference feet
// height — used to detect "no ground under feet" (free fall).
static bool ground_y_found(const Player& p, float x, float z, float ref_y,
                           float& h) {
    return sg::game_ground_found(p.game_world, x, z, ref_y, 80.0f, h);
}

// Shared AI vertical physics — the SAME swept collision the player gets:
// gravity with terminal velocity, collider landing + head bump, terrain
// ceilings, wall push-out and ground-surface smoothing. Monsters no longer
// teleport-snap to a sampled height or clip through walls; they land, fall
// and bump exactly like Hiro (game_resolve_x/y + terrain_ceiling_near).
// Returns true when a walkable surface exists near the entity.
static bool ai_physics(Player& p, PlayObject& po, float dt) {
    const float half_w = po.hit_half_w;
    const float h      = po.hit_h;
    const float gravity = (po.gravity[1] < 0.0f) ? -po.gravity[1] : 1472.0f;

    // Horizontal sweep against solid colliders (walls) — depth-filtered.
    po.pos[0] = sg::game_resolve_x(p.game_world, po.pos[0], po.pos[1],
                                   po.pos[2], half_w, h);

    float hg = 0.0f;
    const bool has_ground = ground_y_found(p, po.pos[0], po.pos[2],
                                           po.pos[1], hg);
    if (!has_ground && po.pos[1] < po.home[1] - 150.0f) {
        // Lost over a pit (no surface under the home anchor): bounce back to
        // the home anchor instead of falling forever (bouncers over pits).
        po.pos[1] = po.home[1];
        po.vel_y = 0.0f;
    }

    const float prev_feet = po.pos[1];
    po.vel_y -= gravity * dt;
    po.vel_y = std::max(po.vel_y, -900.0f);   // terminal velocity
    po.pos[1] += po.vel_y * dt;

    bool hit_gnd = false;
    const float ry = sg::game_resolve_y(p.game_world, po.pos[0], po.pos[1],
                                        po.pos[2], half_w, h, prev_feet,
                                        hit_gnd);
    if (hit_gnd) {
        po.pos[1] = ry;
        po.vel_y = 0.0f;
        po.grounded = true;
    } else if (ry < po.pos[1] && po.vel_y > 0.0f) {
        po.pos[1] = ry;   // head bump → stop ascent
        po.vel_y = 0.0f;
    }

    // Terrain ceiling (ground-mesh undersides) — same guard as the player.
    if (po.vel_y > 0.0f) {
        const float prev_head = prev_feet + h;
        float ceil_h = 0.0f;
        if (av::terrain_ceiling_near(p.game_world.terrain, po.pos[0],
                                     po.pos[2], prev_head, 6.0f, ceil_h)) {
            const float head = po.pos[1] + h;
            if (head > ceil_h) {
                po.pos[1] = ceil_h - h;
                po.vel_y = 0.0f;
            }
        }
    }

    if (po.pos[1] <= hg && has_ground) {
        po.pos[1] = hg;
        po.vel_y = 0.0f;
        po.grounded = true;
    } else if (has_ground) {
        // Smoothly follow the surface (up to 4 u/frame) — fluid slopes.
        const float climb = std::clamp(hg - po.pos[1], -60.0f, 4.0f);
        po.pos[1] += climb * std::min(1.0f, dt * 18.0f);
    }
    return has_ground;
}

// Classify the enemy archetype from the *MonsterController component
// (arm32 taxonomy: Bat/Walking/Charging/Bouncing/Skelly/Static/Shooting).
static AiKind classify_ai(const av::SceneObject& o) {
    const auto& comps = o.resolved_components.empty() ? o.components
                                                      : o.resolved_components;
    for (const auto& c : comps) {
        const std::string& t = c.type_name;
        if (t.find("BatMonsterController") != std::string::npos)      return AiKind::Bat;
        if (t.find("ChargingMonsterController") != std::string::npos) return AiKind::Charger;
        if (t.find("BouncingMonsterController") != std::string::npos) return AiKind::Bouncer;
        if (t.find("ShootingMonsterController") != std::string::npos) return AiKind::Archer;
        if (t.find("StaticMonsterController") != std::string::npos)   return AiKind::Static;
        if (t.find("WalkingMonsterController") != std::string::npos)  return AiKind::Walker;
        if (t.find("SkellyMonsterController") != std::string::npos)   return AiKind::Walker;
        if (t.find("GenericMonsterController") != std::string::npos)  return AiKind::Walker;
        if (t.find("LeapingMonsterController") != std::string::npos)  return AiKind::Charger;
        if (t.find("SnappingMonsterController") != std::string::npos) return AiKind::Static;
        if (t.find("ProjectileMonsterController") != std::string::npos) return AiKind::Archer;
    }
    return AiKind::None;
}

// Read a float field from a component's raw protobuf payload (varint/fixed32).
static float comp_float(const av::SceneComponent& c, unsigned field_num,
                        float fallback) {
    // Minimal protobuf walker over raw_data: field number = (tag >> 3),
    // wire type = tag & 7. Only handles varint + fixed32 (+ skips length
    // delimited), which is all MonsterController payloads use.
    const std::string& d = c.raw_data;
    size_t i = 0;
    while (i + 1 < d.size()) {
        const unsigned tag = (unsigned char)d[i++];
        const unsigned fnum = tag >> 3;
        const unsigned wt = tag & 7;
        if (tag == 0 && i >= d.size()) break;
        if (wt == 0) {            // varint
            uint64_t v = 0; int sh = 0;
            while (i < d.size() && sh < 64) {
                const unsigned char b = d[i++];
                v |= (uint64_t)(b & 0x7f) << sh;
                sh += 7;
                if (!(b & 0x80)) break;
            }
            if (fnum == field_num)
                return (float)(int64_t)v;
        } else if (wt == 5) {     // fixed32
            if (i + 4 <= d.size()) {
                const float f = *(const float*)(d.data() + i);
                i += 4;
                if (fnum == field_num) return f;
            } else break;
        } else if (wt == 1) {     // fixed64
            i += 8;
        } else if (wt == 2) {     // length-delimited: skip (varint length)
            uint64_t len = 0; int sh2 = 0;
            while (i < d.size() && sh2 < 64) {
                const unsigned char b = d[i++];
                len |= (uint64_t)(b & 0x7f) << sh2;
                sh2 += 7;
                if (!(b & 0x80)) break;
            }
            i += (size_t)len;
        } else {
            break;
        }
    }
    return fallback;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool player_begin(Player& p, const av::SceneData& scene, Mode mode,
                  const std::string& scene_dir) {
    p.mode = mode;
    p.clock = 0.0;
    p.paused = false;
    p.speed = 1.0f;
    p.objects.clear();
    p.hero_dir = scene_dir;
    p.hero_loaded = false;
    p.camera_lock = (mode == Mode::PlayHiro);

    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        const auto& o = scene.objects[i];
        PlayObject po;
        po.index = i;
        po.name = o.name;
        po.pos[0] = o.pos_x; po.pos[1] = o.pos_y; po.pos[2] = o.pos_z;
        po.home[0] = o.pos_x; po.home[1] = o.pos_y; po.home[2] = o.pos_z;
        po.rot = o.rot_y;
        po.frame = 0.0f;
        po.moving = false;
        po.phase = static_cast<float>((i * 2654435761u) % 1000) / 1000.0f * 6.28318f;
        // Enemy archetype + behaviour tuning (arm32 *MonsterController).
        po.kind = classify_ai(o);
        po.patrol_range = 90.0f + (float)(i % 5) * 35.0f;
        po.detect_range = 320.0f + (float)(i % 3) * 90.0f;
        po.chase_speed  = 55.0f + (float)(i % 6) * 14.0f;
        // Real tuning from the component payload where present (the
        // MonsterControllerComponent WalkSpeed field #13 is the only
        // controller scalar the game exposes; everything else uses the
        // hard-coded engine constants from the arm32 decompilation).
        // ── Animation binding + controller scalars (scene_entity.h): the
        //    scene's KeyframeAnimation names the exact POD the game plays
        //    (bat_fly, npc_stand, snowball_land...); the controllers carry
        //    the runtime speeds (Walk/Run/Jump). All data-driven. ──
        const av::AnimBindings ab = av::anim_bindings(o);
        po.anim_pod = ab.pod;
        // SpeedMultiplier is clamped to a sane range — a corrupt/huge value
        // would otherwise make the frame clock explode (flicker animations).
        po.anim_speed = std::clamp(ab.speed_multiplier, 0.1f, 4.0f);
        po.anim_repeating = ab.repeating;
        if (ab.walk_speed > 1.0f) po.chase_speed = ab.walk_speed;
        if (ab.run_speed  > 1.0f) po.run_speed  = ab.run_speed;
        if (ab.jump_speed > 1.0f) po.jump_speed = ab.jump_speed;

        // ── Physics hitbox from the object's CollisionShape rect (scaled).
        //    Monsters collide with walls/ceilings/ground like the player. ──
        const av::CollisionData cd = av::collision_parse(o);
        for (const auto& s : cd.shapes) {
            if (s.type == av::COLL_RECT && s.rect[2] > 1.0f && s.rect[3] > 1.0f) {
                po.hit_half_w = std::max(2.0f, s.rect[2] * o.scale_x * 0.5f);
                po.hit_h      = std::max(4.0f, s.rect[3] * o.scale_y);
                break;
            }
        }

        // ── Entity / physics subsystem data (scene_entity.h / scene_physics.h):
        //    real per-object flags straight from the parsed components — no
        //    string guessing for who is an entity / hero / monster. ──
        const av::EntityData ed = av::entity_parse(o);
        po.is_entity  = ed.is_entity;
        po.is_hero    = ed.is_hero;
        po.is_monster = ed.is_monster;
        if (ed.is_entity) po.physics_enabled = ed.entity.physics_enabled;
        const av::PhysicsData pd = av::physics_parse(o);
        if (pd.enabled) {
            // Engine-default gravity (arm32 +0xa4, 25.0). *MonsterController
            // updates and the Lua scripts override it with per-kind values.
            po.gravity[0] = 0.0f;
            po.gravity[1] = -pd.gravity;
            po.gravity[2] = 0.0f;
        }
        p.objects.push_back(po);
    }

    // Mini-Swordigo world: bake static colliders + terrain heightfield so
    // the player and AI stand on real ground and bump into real walls
    // (scene_game.h — the game-state layer). Also builds the intelligent
    // wall world (CollisionWorld) — visible collision walls + entity
    // wall clamping (scene_collision.h runtime).
    sg::game_build_world(scene, p.game_world);
    p.terrain_ctx = &p.game_world.terrain;   // scene_lua.cpp ground checks

    // ── Remastered subsystems: full entity management + gamestate ──
    // Spawn every monster/hero into the EntityManager (AI loop + physics)
    // and start a fresh CharacterState-style game state.
    p.entities.build_from_scene(scene);
    // Carry the PlayObject tuning into the entities (patrol range, detect
    // range, controller speeds — the deterministic variety the player uses).
    for (auto& e : p.entities.entities) {
        for (const auto& po : p.objects) {
            if (po.index != e.object_index) continue;
            e.patrol_range = po.patrol_range;
            e.detect_range = po.detect_range;
            e.speed        = po.chase_speed;
            if (po.run_speed  > 0.0f) e.run_speed  = po.run_speed;
            if (po.jump_speed > 0.0f) e.jump_speed = po.jump_speed;
            break;
        }
    }
    p.game_state = av::GameState{};

    // Real hero physics, DATA-DRIVEN from hiro.scl (CharController payload):
    // walk 230 / fast-run 320 / jump impulse + hitbox rect {-8,-34,16,56}.
    // Falls back to engine defaults when the scl isn't found.
    p.hero_stats_from_scl_ok = sg::hero_stats_from_scl(scene_dir, p.hero_stats);
    if (getenv("RUBY_HERO_STATS")) {
        fprintf(stderr,
                "[HeroStats] scl=%d walk=%.1f fast=%.1f jump=%.1f maxJumpT=%.2f "
                "fastJumpT=%.2f hitbox=[%.1f %.1f %.1f %.1f]\n",
                p.hero_stats_from_scl_ok ? 1 : 0,
                p.hero_stats.walk_speed, p.hero_stats.fast_run_speed,
                p.hero_stats.jump_speed, p.hero_stats.max_jump_time,
                p.hero_stats.fast_max_jump_time,
                p.hero_stats.hitbox[0], p.hero_stats.hitbox[1],
                p.hero_stats.hitbox[2], p.hero_stats.hitbox[3]);
    }

    // Boot the SCL Lua AI host: objects with keep-active AI programs become
    // lua_driven and are simulated by the real game scripts (see scene_lua.cpp).
    sl::lua_begin(p, scene);
    // Lua-driven entities are owned by the Lua host — exclude them from the
    // C++ AI loop so the two never fight over a transform.
    for (auto& e : p.entities.entities) {
        for (const auto& po : p.objects) {
            if (po.index == e.object_index && po.lua_driven) {
                e.lua_driven = true;
                break;
            }
        }
    }

    if (mode == Mode::PlayHiro) {
        const int spawn = player_find_spawn(scene);
        if (spawn >= 0) {
            const auto& s = scene.objects[spawn];
            p.hiro.pos[0] = s.pos_x; p.hiro.pos[1] = s.pos_y; p.hiro.pos[2] = s.pos_z;
            p.hiro.spawn_x = s.pos_x;   // FIX: spawn anchor was never set →
            p.hiro.spawn_y = s.pos_y;   // terrain-miss snapped Hiro to y=0
            p.hiro.spawn_z = s.pos_z;   // (the random-teleport bug)
            p.hiro.rot = s.spawn_facing > 0 ? 0.0f : kPi;
        } else {
            p.hiro.pos[0] = p.hiro.pos[1] = p.hiro.pos[2] = 0.0f;
            p.hiro.spawn_x = p.hiro.spawn_y = p.hiro.spawn_z = 0.0f;
        }
        // Stand on the real ground at spawn (terrain/colliders), so Hiro
        // doesn't fall through the floor when the spawn point floats above it.
        float spawn_ground = p.hiro.spawn_y;
        if (ground_y_found(p, p.hiro.pos[0], p.hiro.pos[2], p.hiro.pos[1],
                           spawn_ground)) {
            spawn_ground = ground_y(p, p.hiro.pos[0], p.hiro.pos[2],
                                    p.hiro.spawn_y, p.hiro.spawn_y);
        }
        if (getenv("RUBY_SPAWN_DEBUG")) {
            fprintf(stderr,
                    "[SpawnDebug] spawnY=%.1f ground=%.1f hiroColliders=%zu\n",
                    p.hiro.spawn_y, spawn_ground, p.game_world.colliders.size());
        }
        // NOTE: the scene spawn point's own Y IS the ground the designer
        // placed (feet level). Only lift Hiro to the sampled ground when the
        // spawn Y is clearly ABOVE it (floating spawn points); never snap
        // DOWN into a lower surface — that buries him in the floor.
        if (spawn_ground < p.hiro.spawn_y - 1.0f) {
            p.hiro.pos[1] = spawn_ground;
            p.hiro.spawn_y = spawn_ground;
        } else {
            // Spawn Y is already at/below ground — use it as-is (the designer
            // placed the feet on the floor).
            p.hiro.spawn_y = p.hiro.pos[1];
        }
        p.hiro.active = true;
        p.hiro.grounded = true;
        p.hiro.vel_y = 0.0f;
        p.hiro.moving = false;
        p.hiro.frame = 0.0f;
        p.hiro.anim = "hiro_stand";   // hiro_stand.POD = the game's idle model
        const float cam_look_y = p.hiro.pos[1] + 20.0f;   // chest height
        p.hiro.camera_target[0] = p.hiro.pos[0];
        p.hiro.camera_target[1] = cam_look_y;
        p.hiro.camera_target[2] = p.hiro.pos[2];
        p.hiro.camera_smoothed[0] = p.hiro.camera_target[0];
        p.hiro.camera_smoothed[1] = p.hiro.camera_target[1];
        p.hiro.camera_smoothed[2] = p.hiro.camera_target[2];

        // Game-like camera: sit behind-and-above Hiro at a distance based on
        // the hero model's actual size (radius ~53 world units), never inside
        // the model. The game camera stays on the +Z side of the action.
        float hero_radius = 50.0f;
        if (!p.hero_dir.empty()) {
            fs::path pod(p.hero_dir + "/hiro_stand.POD");
            if (!fs::is_regular_file(pod))
                pod = fs::path(p.hero_dir + "/hiro_stand.pod");
            if (fs::is_regular_file(pod)) {
                av::PODModel m = av::pod_load(pod.string());
                if (m.radius > 1.0f) hero_radius = m.radius;
            }
        }
        // Game framing: the real Caver camera sits viewOffset {0,0,2835.6}
        // behind the target with an additional 0.25–2.5 zoom on top. We
        // emulate that with an editor distance of ~14× the hero radius
        // (hero ~106 units tall → fills ~15% of the frame, like the game)
        // and a moderate downward pitch.
        p.camera_distance = hero_radius * 14.0f;  // ~740 for the stock hero
        p.camera_pitch    = 24.0f;                // look down at Hiro, game-style
        p.camera_yaw      = 0.0f;                 // camera on the +Z side

        // ── CameraController state (port of OpenSwordigo
        //    camera_controller.cpp — Caver::CameraController) ──
        // The controller keeps a *target* (what we look at) and a *position*
        // (the eye), each smoothed toward a goal with its own decay. The game
        // fixes the eye offset from the target (viewOffset {0,0,2835.6}); we
        // keep the same structure but scale the offset to our editor units.
        const float pitch_rad = p.camera_pitch * kPi / 180.0f;
        const float cp = cosf(pitch_rad), sp = sinf(pitch_rad);
        p.cam_target_goal[0] = p.hiro.pos[0];
        p.cam_target_goal[1] = cam_look_y;
        p.cam_target_goal[2] = p.hiro.pos[2];
        // Eye goal = target + viewOffset (camera behind/above on the +Z side).
        p.cam_pos_goal[0] = p.cam_target_goal[0];
        p.cam_pos_goal[1] = p.cam_target_goal[1] + p.camera_distance * sp;
        p.cam_pos_goal[2] = p.cam_target_goal[2] + p.camera_distance * cp;
        p.cam_target[0] = p.cam_target_goal[0];  // gotoTargetImmediately()
        p.cam_target[1] = p.cam_target_goal[1];
        p.cam_target[2] = p.cam_target_goal[2];
        p.cam_pos[0] = p.cam_pos_goal[0];
        p.cam_pos[1] = p.cam_pos_goal[1];
        p.cam_pos[2] = p.cam_pos_goal[2];
        p.cam_look_ahead[0] = 0; p.cam_look_ahead[1] = 0; p.cam_look_ahead[2] = 0;
        p.cam_rumble = false;
        p.cam_rumble_t = 0.0f;
        p.status = "Hiro spawned — A/D move, Space jump, Esc to exit";
    } else {
        p.hiro.active = false;
        p.status = "Scene playing — animations + AI simulation";
    }
    return !scene.objects.empty();
}

void player_end(Player& p) {
    p.mode = Mode::Off;
    sl::lua_end(p);
    p.terrain_ctx = nullptr;   // points into game_world; nothing to free
    p.game_world.colliders.clear();
    p.game_world.terrain_built = false;
    p.objects.clear();
    p.entities.clear();
    p.hiro.active = false;
    p.camera_lock = false;
    p.hero_loaded = false;
    p.show_walls = false;
    p.cam_rumble = false;
    p.cam_rumble_t = 0.0f;
    p.clock = 0.0;
    p.seek_time = -1.0;
}

void player_rumble(Player& p, float duration) {
    p.cam_rumble = true;
    p.cam_rumble_t = duration;
}

// ============================================================================
// Hiro animation selection (map state → hiro_* model)
// ============================================================================

static void hiro_pick_animation(Player& p, const av::SceneData& scene) {
    const float hs = p.hiro.speed;
    std::string want = "hiro_stand";
    if (!p.hiro.grounded) {
        // Rising: regular jump, or the spin-jump (the game's airJumpAnimation
        // binding) when airborne off a fast run — Hiro somersaults while
        // travelling at speed. Falling always uses the landing pose.
        if (p.hiro.vel_y > 0.1f)
            want = p.hiro.running ? "hiro_spinjump" : "hiro_jump";
        else
            want = "hiro_jumpland";
    } else if (p.hiro.moving) {
        want = (hs != 0.0f) ? "hiro_run" : "hiro_stand";
    }
    if (want != p.hiro.anim) {
        p.hiro.anim = want;
        p.hiro.frame = 0.0f;
        // One-shot anims play once and HOLD the last frame. Looping them
        // flickers: hiro_jump is 2 frames whose poses differ a lot, so a
        // 30fps loop toggles the model 15×/sec — the "model bugs when
        // jumping" report. The game's jump/land are single plays.
        // NOTE: add any future one-shot hero anims (hurt/die/swing) here.
        p.hiro.anim_hold = (want == "hiro_jump" || want == "hiro_jumpland");
        p.hiro.anim_feet.clear();
        // Probe the actual POD so we can size the frame loop + precompute the
        // per-frame feet depth (jump/spinjump poses drop the feet below the
        // rest pose — lifting by the frame's own depth keeps them on the
        // floor in every pose).
        if (!p.hero_dir.empty()) {
            fs::path pod(p.hero_dir + "/" + want + ".POD");
            if (!fs::is_regular_file(pod))
                pod = fs::path(p.hero_dir + "/" + want + ".pod");
            if (fs::is_regular_file(pod)) {
                av::PODModel m = av::pod_load(pod.string());
                p.hiro.anim_frames = m.num_frames > 0 ? m.num_frames : 30;
                // O(frames × verts) skinned pass — only on anim switch, never
                // in the per-frame hot loop.
                std::vector<float> feet;
                if (av::pod_anim_feet(m, feet))
                    p.hiro.anim_feet = std::move(feet);
            } else {
                p.hiro.anim_frames = 30;
            }
        }
    }
}

// ============================================================================
// Keyboard state bridge: the editor's frame loop calls sp_poll_keys() each
// frame to publish current key state into this module.
// ============================================================================

static bool s_key_left = false, s_key_right = false, s_key_jump = false;
static bool s_key_left_pressed = false, s_key_right_pressed = false;

void sp_poll_keys(bool left, bool right, bool jump,
                  bool left_pressed, bool right_pressed) {
    s_key_left = left; s_key_right = right; s_key_jump = jump;
    s_key_left_pressed  = left_pressed;
    s_key_right_pressed = right_pressed;
}

static bool key_left()  { return s_key_left; }
static bool key_right() { return s_key_right; }
static bool key_jump()  { return s_key_jump; }
static bool key_left_pressed()  { return s_key_left_pressed; }
static bool key_right_pressed() { return s_key_right_pressed; }

// ============================================================================
// Per-frame simulation
// ============================================================================

void player_tick(Player& p, av::SceneData& scene, float dt, bool apply_to_scene) {
    if (p.mode == Mode::Off || p.paused) return;
    dt *= p.speed;
    p.clock += dt;

    // ── Playback timeline ──────────────────────────────────────────────
    if (p.total_time > 0.0 && p.clock >= p.total_time) {
        if (p.loop) p.clock = std::fmod(p.clock, p.total_time);
        else { p.clock = p.total_time; p.paused = true; }
    }
    if (p.seek_time >= 0.0) {
        p.clock = std::max(0.0, p.seek_time);
        p.seek_time = -1.0;
        // Reset deterministic object state (phases) so the scrub matches.
        for (auto& po : p.objects) po.phase = 0.0f;
        if (p.hiro.active) p.hiro.frame = 0.0f;
    }

    // Hero position (for AI detection) — live from the engine.
    const float hx = p.hiro.pos[0];
    const float hy = p.hiro.pos[1];

    // ── SCL Lua AI host: resume the real game scripts and integrate
    //    their velocity/gravity for lua_driven objects. ────────────────
    sl::lua_tick(p, scene, dt);

    // ── Remastered entity loop: EntityManager AI + physics + gamestate ──
    // Runs every animated entity's Update (archetype AI ported from the
    // *MonsterControllerComponent::Update functions), integrates the
    // PhysicsObjectState body against the wall world (scene_collision.h
    // runtime) and reports hero interactions — stomps, contact damage and
    // kills → XP — for the gamestate to apply.
    av::EntityManager::HeroContact hc;
    const float hero_pos[2] = {hx, hy};
    p.entities.update_all(dt, hero_pos, p.hiro.vel_y, p.hiro.active,
                          p.game_state.level, p.game_world, hc);
    if (p.hiro.active) {
        if (hc.stomped) {
            // Hero bounced off a stomped monster (ground pound).
            p.hiro.vel_y = p.hero_stats.jump_speed * 0.6f;
            p.hiro.grounded = false;
            p.hiro.pos[1] += 1.0f;
        }
        if (hc.kills > 0) {
            p.game_state.kills += hc.kills;
            p.game_state.add_xp(hc.xp_gained);
            p.status = "Monster defeated! +" + std::to_string(hc.xp_gained) +
                       " XP (Lv " + std::to_string(p.game_state.level) + ")";
        }
        if (hc.hurt_hero) {
            // Health system removed: monster contact reports but deals no HP.
            p.status = "Ouch! Monster touched you.";
            player_rumble(p, 0.25f);
        }
    }

    // ── Object clocks + AI (the EntityManager above owns transforms) ────
    for (auto& po : p.objects) {
        if (po.index < 0 || po.index >= (int)scene.objects.size()) continue;
        const auto& o = scene.objects[po.index];
        const std::string mname = o.mesh_name.empty() ? o.background_name : o.mesh_name;

        // Animation clock. Only AI entities loop their animation; static
        // props and 2-frame models (doors, switches) must hold frame 0 so
        // doors stay closed instead of oscillating open/close.
        if (!mname.empty() && (po.lua_driven || has_ai(o))) {
            // KeyframeAnimation.SpeedMultiplier (data-driven) scales the
            // playback rate; the draw pass clamps to the POD's frame count.
            po.frame += dt * 30.0f * po.anim_speed;
        } else {
            po.frame = 0.0f;
        }

        // ── Remastered AI: the EntityManager (scene_entity.cpp) owns every
        //    animated entity's transform — archetype loop (ported
        //    *MonsterControllerComponent::Update), PhysicsObjectState physics
        //    and the wall world. Copy its live state back into the PlayObject
        //    so the visualizer draws what the manager computed. Lua-driven
        //    objects are owned by the Lua host and skipped here. ──
        if (po.lua_driven) {
            // nothing: sl::lua_tick already wrote position/rotation
        } else {
            const av::Entity* ent = nullptr;
            for (const auto& e : p.entities.entities)
                if (e.object_index == po.index) { ent = &e; break; }
            if (ent) {
                po.pos[0] = ent->pos[0];
                po.pos[1] = ent->pos[1];
                po.pos[2] = ent->pos[2];
                po.rot = ent->rot;
                po.moving = ent->moving && !ent->dead;
                if (ent->dead) po.frame = 0.0f;   // corpse holds frame 0
            } else if (!mname.empty() && !has_ai(o)) {
                // Props / non-AI objects stay exactly where they were placed.
                po.pos[0] = po.home[0]; po.pos[1] = po.home[1]; po.pos[2] = po.home[2];
                po.rot = o.rot_y;
                po.moving = false;
            }
        }

        if (apply_to_scene) {
            auto& so = scene.objects[po.index];
            so.pos_x = po.pos[0];
            so.pos_y = po.pos[1];
            so.pos_z = po.pos[2];
            so.rot_y = po.rot;
        }
    }

    // ── Diagnostics (RUBY_AI_DEBUG): verify monsters move + animate + stand
    //    on terrain — dump every ~2 s of playback. ───────────────────────
    if (getenv("RUBY_AI_DEBUG") && (int)(p.clock * 0.5) != (int)((p.clock - dt) * 0.5)) {
        for (const auto& po : p.objects) {
            if (po.index < 0 || po.index >= (int)scene.objects.size()) continue;
            float gh = 0.0f;
            const bool hasg = ground_y_found(p, po.pos[0], po.pos[2], po.pos[1], gh);
            fprintf(stderr,
                    "[AI] t=%.1f kind=%d lua=%d name=%s pos=(%.0f,%.0f,%.0f) homeY=%.0f "
                    "gnd=%.0f%s frame=%.0f moving=%d\n",
                    p.clock, (int)po.kind, po.lua_driven ? 1 : 0, po.name.c_str(),
                    po.pos[0], po.pos[1], po.pos[2], po.home[1],
                    hasg ? gh : -9999.0f, hasg ? "" : "(noGnd)", po.frame, po.moving ? 1 : 0);
        }
    }

    // ── Hiro ───────────────────────────────────────────────────────────
    if (p.hiro.active) {
        // Real physics, DATA-DRIVEN from hiro.scl (CharController payload)
        // loaded in player_begin; falls back to engine defaults. No guesses.
        const float walk_speed = p.hero_stats.walk_speed;        // 230 (scl)
        const float run_speed  = p.hero_stats.fast_run_speed;    // 320 (scl)
        const float gravity    = p.hero_stats.gravity;
        const float jump_v     = p.hero_stats.jump_speed;
        const float hit_w      = p.hero_stats.hitbox[2];         // 16 wide
        const float hit_h      = p.hero_stats.hitbox[3];         // 56 tall
        const float half_w     = hit_w * 0.5f;

        // Keyboard input (A/D + Space, also arrows) — published by the
        // editor's frame loop via sp_poll_keys().
        const bool left  = key_left();
        const bool right = key_right();
        const bool jump  = key_jump();
        p.hiro.moving = (right || left);
        const float dir = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
        if (dir != 0.0f)
            p.hiro.rot = (dir < 0.0f) ? kPi : 0.0f;

        // ── Double-tap-to-run (platformer convention) ──
        // Two presses of the same direction inside 0.30 s toggle fast run;
        // releasing both directions resets it. Switching direction cancels
        // the pending double-tap.
        const float kTapWindow = 0.30f;
        if (p.hiro.run_timer > 0.0f) p.hiro.run_timer -= dt;
        const int tap_dir =
            key_right_pressed() ? 1 : (key_left_pressed() ? -1 : 0);
        if (tap_dir != 0) {
            // Reversed direction while running → drop back to walk.
            if (p.hiro.running && dir != 0.0f && tap_dir != (dir > 0 ? 1 : -1))
                p.hiro.running = false;
            if (tap_dir == p.hiro.last_tap_dir && p.hiro.run_timer > 0.0f)
                p.hiro.running = true;         // double-tap → fast run
            p.hiro.last_tap_dir = tap_dir;
            p.hiro.run_timer = kTapWindow;
        } else if (dir == 0.0f) {
            p.hiro.running = false;            // stopped → back to normal
        }

        // ── Responsive horizontal movement ─────────────────────────────
        // Linear ramp toward the target speed (data-driven from hiro.scl via
        // hero_stats: ground ~2500 u/s², air ~1600 — the game's EntityController
        // snap, ~0.1 s to a full run) and the same-rate deceleration when no
        // input. This is the game's SetAcceleration + deceleration behaviour;
        // the old exponential lerp took ~0.9 s to reach top speed and made
        // Hiro feel sluggish even though the scl values were right.
        const float target_vx = dir * (p.hiro.running ? run_speed : walk_speed);
        const float accel = p.hiro.grounded ? p.hero_stats.ground_accel
                                            : p.hero_stats.air_accel;
        const float dv = accel * dt;
        if (target_vx != 0.0f) {
            // Ramp toward the target, but never overshoot it.
            if (p.hiro.vx < target_vx)
                p.hiro.vx = std::min(target_vx, p.hiro.vx + dv);
            else
                p.hiro.vx = std::max(target_vx, p.hiro.vx - dv);
        } else {
            // Decelerate to a stop at the same snappy rate.
            const float decel = p.hiro.grounded ? p.hero_stats.ground_decel
                                                : p.hero_stats.air_decel;
            const float dvd = decel * dt;
            if (std::fabs(p.hiro.vx) <= dvd) p.hiro.vx = 0.0f;
            else p.hiro.vx -= (p.hiro.vx > 0.0f ? dvd : -dvd);
        }
        p.hiro.pos[0] += p.hiro.vx * dt;
        p.hiro.speed = p.hiro.vx;

        // ── Horizontal wall collision (world colliders, depth-filtered). ──
        // Push Hiro out of solid walls (never off the floor he's standing
        // on — game_resolve_x skips surfaces under his feet).
        p.hiro.pos[0] = sg::game_resolve_x(p.game_world, p.hiro.pos[0],
                                           p.hiro.pos[1], p.hiro.pos[2],
                                           half_w, hit_h);
        // Polygon-accurate wall clamp (same as monsters in scene_entity.cpp):
        // the AABB resolve above only knows rect colliders; sloped polygon
        // walls / ground-polygon side edges are in the wall world as segments
        // with outward normals. Clamp Hiro's body CIRCLE (center at chest,
        // radius = half width) out of every solid wall at this depth — fixes
        // walking through sloped polygon walls that the AABB pass missed.
        // Only X is written back: Y is owned by the ground/ceiling resolve.
        bool wall_blocked = false;
        if (!p.game_world.walls.walls.empty()) {
            const float px = p.hiro.pos[0];
            float cx = px;
            float cy = p.hiro.pos[1] + hit_h * 0.5f;   // chest center
            av::wall_clamp_circle(p.game_world.walls, cx, cy,
                                  p.hiro.pos[2], half_w);
            p.hiro.pos[0] = cx;   // only X is owned here
            if (std::fabsf(cx - px) > 0.001f) wall_blocked = true;
        }
        if (wall_blocked) {
            // A wall stopped us — kill the horizontal velocity so we don't
            // push into it every frame (wall-buzz jitter).
            p.hiro.vx = 0.0f;
        }

        // ── Ground + gravity with hysteresis + smoothing ──
        // Real ground under Hiro (collider tops + terrain heightfield). The
        // sampled height is approached smoothly (max 4 u/frame climb) so
        // slopes and mesh-cell steps don't jitter the camera; when the
        // sampler briefly misses (ledges, cell gaps) we FALL, never snap.
        float hg = 0.0f;
        const bool has_ground =
            ground_y_found(p, p.hiro.pos[0], p.hiro.pos[2], p.hiro.pos[1], hg);
        const float last_ground = p.hiro.grounded ? p.hiro.pos[1] : hg;
        if (!has_ground) hg = p.hiro.grounded ? last_ground : p.hiro.spawn_y;

        if (!p.hiro.grounded) {
            const float prev_feet = p.hiro.pos[1];   // for the swept tests
            p.hiro.vel_y -= gravity * dt;
            // Terminal velocity — bounds the per-frame fall so the swept
            // landing/ceiling tests always have a sane window to catch, and
            // keeps long falls from feeling like a rocket dive.
            p.hiro.vel_y = std::max(p.hiro.vel_y, -900.0f);
            p.hiro.pos[1] += p.hiro.vel_y * dt;
            // Vertical resolve: land on collider tops and bump heads. The
            // swept tests (prev positions) catch fast falls crossing a thin
            // platform's top plane AND rising heads crossing an underside in
            // one frame — no more "fell through" or "jumped up through it".
            bool hit_gnd = false;
            const float resolved_y =
                sg::game_resolve_y(p.game_world, p.hiro.pos[0], p.hiro.pos[1],
                                   p.hiro.pos[2], half_w, hit_h, prev_feet,
                                   hit_gnd);
            if (hit_gnd) {
                p.hiro.pos[1] = resolved_y;
                p.hiro.vel_y = 0.0f;
                p.hiro.grounded = true;
            } else if (resolved_y < p.hiro.pos[1] && p.hiro.vel_y > 0.0f) {
                p.hiro.pos[1] = resolved_y;   // head bump → stop ascent
                p.hiro.vel_y = 0.0f;
            }
            // ── Terrain ceiling (ground-mesh undersides) ──
            // The heightfield stores underside planes (down-facing triangles
            // + ground-polygon bottoms), so a head rising under a platform
            // bumps at its true BOTTOM instead of tunnelling through the
            // mesh to its top. The scan window is dt-aware: at low FPS / high
            // play speed the head can rise > 6 u in one tick, and a fixed
            // step would let it skip past the window and clip through again.
            if (p.hiro.vel_y > 0.0f) {
                const float prev_head = prev_feet + hit_h;
                const float rise = p.hiro.vel_y * dt;
                const float step = std::max(6.0f, rise + 1.0f);
                float ceil_h = 0.0f;
                if (av::terrain_ceiling_near(p.game_world.terrain,
                                             p.hiro.pos[0], p.hiro.pos[2],
                                             prev_head, step, ceil_h)) {
                    const float head = p.hiro.pos[1] + hit_h;
                    if (head > ceil_h) {
                        p.hiro.pos[1] = ceil_h - hit_h;
                        p.hiro.vel_y = 0.0f;
                    }
                }
            }
            if (p.hiro.pos[1] <= hg && has_ground) {
                p.hiro.pos[1] = hg;
                p.hiro.vel_y = 0.0f;
                p.hiro.grounded = true;
            }
        } else if (has_ground) {
            // Smoothly follow the surface (up to 4 u/frame) instead of an
            // instant snap — this is what makes walking over hills/joints
            // feel fluid and keeps the camera from hopping between cells.
            const float climb = std::clamp(hg - p.hiro.pos[1], -60.0f, 4.0f);
            p.hiro.pos[1] += climb * std::min(1.0f, dt * 18.0f);
        }
        // Variable-height jump, exactly like the game's CharController:
        // holding jump keeps the upward velocity FLOORED at a decaying
        // fraction of JumpSpeed for up to NormalMaxJumpTime seconds (0.23
        // from hiro.scl) — tap for a short hop, hold for a full jump. This
        // is why the scl carries both JumpSpeed and NormalMaxJumpTime as
        // separate bindings (arm32 StartJumping + UpdateSpeedComponents).
        if (jump && p.hiro.grounded) {
            p.hiro.vel_y = jump_v;
            p.hiro.grounded = false;
            p.hiro.pos[1] += 1.0f;   // detach from ground so gravity applies
            p.hiro.jump_held_time = 0.0f;
        }
        const float max_jump_time = p.hiro.running
            ? p.hero_stats.fast_max_jump_time   // FastMaxJumpTime (0.11 s)
            : p.hero_stats.max_jump_time;       // NormalMaxJumpTime (0.23 s)
        if (!p.hiro.grounded && jump &&
            p.hiro.jump_held_time < max_jump_time) {
            // While held (within the scl window) keep the ascent strong: the
            // upward velocity never drops below a linearly-decaying floor.
            // Running uses the shorter FastMaxJumpTime — a quicker hop than
            // the walk-jump, both from hiro.scl.
            const float floor_v = jump_v * (1.0f - p.hiro.jump_held_time /
                                            std::max(0.01f, max_jump_time));
            if (p.hiro.vel_y < floor_v) p.hiro.vel_y = floor_v;
            p.hiro.jump_held_time += dt;
        }

        // ── Unsafe surfaces + death pits (real Swordigo behaviour) ──
        // Lava/spikes (unsafe colliders) or falling far below the current
        // ground respawn Hiro at the scene spawn point. A 1.5 s respawn
        // cooldown prevents a frame-to-frame loop (which looked like violent
        // shaking) if Hiro spawns resting on a flagged surface; the kill
        // plane is measured from the real ground under him, not the spawn Y
        // (spawn points can sit high above the floor).
        if (p.hiro.respawn_timer > 0.0f) p.hiro.respawn_timer -= dt;
        float under = 0.0f;
        const bool have_under = ground_y_found(p, p.hiro.pos[0], p.hiro.pos[2],
                                               p.hiro.pos[1], under);
        const float kill_plane =
            (have_under ? under : p.hiro.spawn_y) - hit_h * 6.0f;
        const bool death =
            (sg::game_hits_unsafe(p.game_world, p.hiro.pos[0], p.hiro.pos[1],
                                  p.hiro.pos[2], half_w, hit_h) &&
             !p.hiro.grounded) ||   // touching lava while airborne
            (p.hiro.pos[1] < kill_plane && p.hiro.respawn_timer <= 0.0f);
        if (death) {
            p.hiro.pos[0] = p.hiro.spawn_x;
            p.hiro.pos[1] = p.hiro.spawn_y;
            p.hiro.pos[2] = p.hiro.spawn_z;
            p.hiro.vel_y = 0.0f;
            p.hiro.vx = 0.0f;
            p.hiro.grounded = false;
            p.hiro.respawn_timer = 1.5f;
            player_rumble(p, 0.4f);
            p.status = "Ouch! Respawned at the scene spawn point.";
        }

        // Animation — one-shot anims (jump/land) advance to the end and then
        // HOLD the last frame; looping anims (stand/run/spinjump) wrap.
        p.hiro.frame += dt * 30.0f;
        hiro_pick_animation(p, scene);
        if (p.hiro.anim_frames > 0) {
            if (p.hiro.anim_hold) {
                const float last = (float)(p.hiro.anim_frames - 1);
                if (p.hiro.frame > last) p.hiro.frame = last;
            } else {
                p.hiro.frame = std::fmod(p.hiro.frame, (float)p.hiro.anim_frames);
            }
        }

        // Hiro diagnostics (RUBY_HIRO_DEBUG): position vs ground vs velocity
        // every ~0.5 s — used to verify the spawn-on-ground and fluid-motion
        // fixes headlessly.
        if (getenv("RUBY_HIRO_DEBUG") &&
            (int)(p.clock * 2.0) != (int)((p.clock - dt) * 2.0)) {
            float gh = 0.0f;
            const bool hg = ground_y_found(p, p.hiro.pos[0], p.hiro.pos[2],
                                           p.hiro.pos[1], gh);
            fprintf(stderr,
                    "[Hiro] t=%.1f pos=(%.1f, %.1f, %.1f) gnd=%s%.1f vx=%.1f vy=%.1f "
                    "grounded=%d anim=%s\n",
                    p.clock, p.hiro.pos[0], p.hiro.pos[1], p.hiro.pos[2],
                    hg ? "" : "(none)", gh, p.hiro.vx, p.hiro.vel_y,
                    p.hiro.grounded ? 1 : 0, p.hiro.anim.c_str());
        }

        // ── Camera follow — Caver::CameraController port ─────────────
        // The real controller (OpenSwordigo camera_controller.cpp) holds a
        // target (look-at) and a position (eye), each smoothing toward its
        // own goal with a per-axis decay; the eye goal = target + viewOffset
        // (game: {0,0,2835.6} — we scale it to editor units). A look-ahead
        // nudges the target in Hiro's facing direction, and rumble perturbs
        // the target's Y like the game's Camera.Rumble.
        const float look = (p.hiro.rot < kPi * 0.5f) ? 1.0f : -1.0f; // facing +X / -X
        const float look_ahead = 0.0f;
        p.cam_target_goal[0] = p.hiro.pos[0] + look * look_ahead;
        p.cam_target_goal[1] = p.hiro.pos[1] + 20.0f;   // chest height
        p.cam_target_goal[2] = p.hiro.pos[2];
        const float pitch_rad = p.camera_pitch * kPi / 180.0f;
        p.cam_pos_goal[0] = p.cam_target_goal[0];
        p.cam_pos_goal[1] = p.cam_target_goal[1] + p.camera_distance * std::sin(pitch_rad);
        p.cam_pos_goal[2] = p.cam_target_goal[2] + p.camera_distance * std::cos(pitch_rad);

        // Double-lag smoothing (game decay rates: position 0.9, target 0.89).
        const float pos_alpha = std::min(1.0f, dt * 60.0f * (1.0f - 0.9f));   // dt*6
        const float tgt_alpha = std::min(1.0f, dt * 60.0f * (1.0f - 0.89f));  // dt*6.6
        for (int i = 0; i < 3; ++i) {
            p.cam_pos[i]    += (p.cam_pos_goal[i]    - p.cam_pos[i])    * pos_alpha;
            p.cam_target[i] += (p.cam_target_goal[i] - p.cam_target[i]) * tgt_alpha;
        }

        // Rumble: target_.y += sin(phase*60) * (1-phase*0.7) * 6.0 while active.
        if (p.cam_rumble && p.cam_rumble_t > 0.0f) {
            p.cam_rumble_t -= dt;
            const float phase = std::fmod((float)p.clock, 1.0f);
            p.cam_target[1] += std::sin(phase * 60.0f) * (1.0f - phase * 0.7f) * 6.0f;
            if (p.cam_rumble_t <= 0.0f) p.cam_rumble = false;
        }

        // Keep the legacy smoothed fields in sync (overlay reads them).
        p.hiro.camera_target[0] = p.cam_target[0];
        p.hiro.camera_target[1] = p.cam_target[1];
        p.hiro.camera_target[2] = p.cam_target[2];
        for (int i = 0; i < 3; ++i)
            p.hiro.camera_smoothed[i] = p.hiro.camera_target[i];
    }
}

void player_apply_camera(const Player& p, av::Camera& cam) {
    if (!p.camera_lock || !p.hiro.active) return;
    // The CameraController's eye/target vectors are the source of truth; the
    // editor camera is spherical (yaw/pitch/distance around a target), so we
    // invert the eye = target + offset relation:
    //   offset = (dx, dy, dz) with dx = d·cos(p)·sin(y), dz = d·cos(p)·cos(y)
    //   dy = d·sin(p)  ⇒  y = atan2(dx, dz), p = asin(dy/d), d = |offset|.
    const float ox = p.cam_pos[0] - p.cam_target[0];
    const float oy = p.cam_pos[1] - p.cam_target[1];
    const float oz = p.cam_pos[2] - p.cam_target[2];
    const float d = std::sqrt(ox * ox + oy * oy + oz * oz);
    cam.target[0] = p.cam_target[0];
    cam.target[1] = p.cam_target[1];
    cam.target[2] = p.cam_target[2];
    cam.distance = d;
    cam.yaw   = std::atan2(ox, oz) * (180.0f / kPi);
    cam.pitch = std::asin(oy / d) * (180.0f / kPi);
    cam.far_plane = std::max(4000.0f, cam.distance * 8.0f);
}

// ============================================================================
// Playback panel (ImGui)
// ============================================================================
#if defined(SCENE_PLAYER_HAS_IMGUI)
void player_draw_panel(Player& p) {
    ImGui::TextColored(ImVec4(0.75f, 0.8f, 0.95f, 1.0f), ICON_FA_PLAY " %s",
                       player_mode_name(p.mode));
    ImGui::SameLine();
    int mm = p.clock / 60, ss = (int)p.clock % 60;
    ImGui::TextDisabled("%02d:%02d", mm, ss);
    ImGui::Separator();

    if (ImGui::Button(p.paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
        p.paused = !p.paused;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Speed", &p.speed, 0.1f, 4.0f, "%.1fx");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLOCK_ROTATE_LEFT " Restart")) {
        p.seek_time = 0.0;
        p.paused = false;
    }

    // Timeline
    ImGui::Separator();
    ImGui::TextDisabled("TIMELINE");
    float dur = (float)p.total_time;
    const bool has_len = dur > 0.0f;
    float t = (float)p.clock;
    if (has_len) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##tl", &t, 0.0f, dur, "%.1f / %.0fs", ImGuiSliderFlags_NoInput))
            p.seek_time = (double)t;
    } else {
        ImGui::Text("%.1fs  (infinite)", t);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##tl2", &t, 0.0f, 3600.0f, "");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &p.loop)) {}

    if (p.hiro.active) {
        ImGui::Separator();
        ImGui::TextDisabled("HIRO");
        ImGui::Text("pos  (%.1f, %.1f, %.1f)", p.hiro.pos[0], p.hiro.pos[1], p.hiro.pos[2]);
        ImGui::Text("anim %s  %s", p.hiro.anim.c_str(),
                    p.hiro.grounded ? "grounded" : "airborne");
        ImGui::Checkbox("Show Collider & Ground Probe", &p.show_collider);
    }

    ImGui::Separator();
    ImGui::Checkbox("Show Collision Walls", &p.show_walls);

    if (!p.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", p.status.c_str());
    }
}
#else
void player_draw_panel(Player&) {}
#endif


// ============================================================================
// Visible collision walls — the scene's collision layout drawn in world space
// ============================================================================

void player_draw_walls(const Player& p) {
    if (p.mode == Mode::Off) return;
    const auto& walls = p.game_world.walls.walls;
    if (walls.empty()) return;

    std::vector<float> solid, ground, unsafe, oneway;
    for (const auto& w : walls) {
        const float z = (w.z_min + w.z_max) * 0.5f;
        std::vector<float>* target = nullptr;
        if (w.unsafe)      target = &unsafe;
        else if (w.ground && w.one_way) target = &oneway;
        else if (w.ground) target = &ground;
        else if (w.solid)  target = &solid;
        if (!target) continue;
        target->push_back(w.a[0]); target->push_back(w.a[1]); target->push_back(z);
        target->push_back(w.b[0]); target->push_back(w.b[1]); target->push_back(z);
    }
    // Colors: solid = white, ground = green, one-way platform = blue,
    // unsafe (lava / spikes) = red (thicker).
    static const float c_solid[4]  = {1.00f, 1.00f, 1.00f, 0.85f};
    static const float c_ground[4] = {0.30f, 1.00f, 0.45f, 0.85f};
    static const float c_oneway[4] = {0.35f, 0.70f, 1.00f, 0.85f};
    static const float c_unsafe[4] = {1.00f, 0.25f, 0.25f, 0.95f};
    if (!solid.empty())
        av::render_lines(solid.data(),  (int)solid.size() / 3,  c_solid,  nullptr, 1.6f);
    if (!ground.empty())
        av::render_lines(ground.data(), (int)ground.size() / 3, c_ground, nullptr, 1.6f);
    if (!oneway.empty())
        av::render_lines(oneway.data(), (int)oneway.size() / 3, c_oneway, nullptr, 1.6f);
    if (!unsafe.empty())
        av::render_lines(unsafe.data(), (int)unsafe.size() / 3, c_unsafe, nullptr, 2.2f);
}

} // namespace sp
