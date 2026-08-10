/* scene_entity.cpp — Swordigo entity subsystem parser
 *
 * Parses EntityComponent, EntityControllerComponent, HeroEntityComponent,
 * and MonsterEntityComponent from SceneObject component lists.
 *
 * Field numbers are from scene_schemas.cpp (authoritative) and confirmed
 * against IDA decompilations in OpenSwordigo/resources/ida_decompiled/functions/Caver/
 *
 * Component wrapping: each component's raw_data contains:
 *   field 1 (LEN)    : type name string
 *   field 2 (VARINT) : type id
 *   field N (LEN)    : payload nested message (N = payload_field from schema)
 *
 * The payload field numbers follow the schema: Component.EntityComponent at
 * tag (1218 << 3) | WIRE_LEN → field_number = 1218 >> ... actually the
 * Component schema uses (tag_key >> 3) as field number.
 * From scene_schemas.cpp Component schema:
 *   {1218, "EntityComponent"}         → payload field = 1218 >> 3 = 152 + remainder...
 *   Actually the schema key is the raw tag value: field 1218 = field_number 152+...
 *   Wait: key=1218 is the tag. field_number = key >> 3 = 152. wire_type = key & 7 = 2 (LEN).
 *   So payload field_number = 152. But actually the payload is accessed by:
 *   component_payload_field() in scene_loader.cpp: it scans raw_data for
 *   field_number >= 50 with WIRE_LEN — this is the actual payload field number
 *   as written to the protobuf stream.
 *
 *   Per block_formats.py: the payload field in the Component message for each
 *   sub-component type is (schema_tag >> 3). So EntityComponent's payload is
 *   tag 1218, field_number = 1218 / 8 = 152 (since 1218 >> 3 = 152, 1218 & 7 = 2).
 *
 * Protobuf wire types used:
 *   WIRE_VARINT (0) : bool, int, enum
 *   WIRE_I32    (5) : float (fixed32)
 *   WIRE_LEN    (2) : string, nested message
 */

#include "tools/scene_loader.h"
#include "tools/scene_entity.h"
#include "tools/scene_game.h"
#include "tools/scene_terrain.h"
#include "platform/protobuf_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace av {

// ─── Component payload field numbers (from scene_schemas.cpp) ───────────
// field_number = schema_tag >> 3  (schema_tag = raw protobuf tag key)
// EntityComponent tag = 1218, field_number = 1218 >> 3 = 152
// EntityControllerComponent tag = 1290, field_number = 1290 >> 3 = 161
// HeroEntityComponent tag = 1322, field_number = 1322 >> 3 = 165
// MonsterEntityComponent tag = 1266, field_number = 1266 >> 3 = 158

static constexpr int PAYLOAD_ENTITY_COMPONENT        = 152;  // 1218 >> 3
static constexpr int PAYLOAD_ENTITY_CONTROLLER       = 161;  // 1290 >> 3
static constexpr int PAYLOAD_HERO_ENTITY             = 165;  // 1322 >> 3
static constexpr int PAYLOAD_MONSTER_ENTITY          = 158;  // 1266 >> 3

// ─── Type name strings (from game source, scene files) ──────────────────
// SCENE files store SHORT stems ("MonsterEntity", "EntityController") while
// library templates and the schema use the "...Component" suffix
// ("MonsterEntityComponent").  Match either spelling.
static constexpr const char* TYPE_ENTITY_COMPONENT   = "Entity";
static constexpr const char* TYPE_ENTITY_CONTROLLER  = "EntityController";
static constexpr const char* TYPE_HERO_ENTITY        = "HeroEntity";
static constexpr const char* TYPE_MONSTER_ENTITY     = "MonsterEntity";

// Match a component type name against a schema stem (e.g. "MonsterEntity"),
// accepting the short scene spelling, the "...Component" suffix spelling,
// and trailing NUL / whitespace from protobuf padding.
static bool type_matches(const std::string& tn, const char* stem) {
    const size_t n = std::strlen(stem);
    if (tn.size() < n) return false;
    if (tn.compare(0, n, stem) != 0) return false;
    const char* rest = tn.c_str() + n;
    if (*rest == '\0') return true;
    // Suffix must be "Component" or padding, never another identifier word
    // (e.g. "MonsterEntityInfo" must not match "MonsterEntity").
    if (std::strncmp(rest, "Component", 9) == 0) rest += 9;
    for (; *rest; ++rest) {
        const char c = *rest;
        if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return false;
    }
    return true;
}

// Read the payload bytes from a component's raw_data given the expected
// payload field number. Returns empty string if not found.
static std::string read_payload(const SceneComponent& comp, int expected_payload_field) {
    try {
        proto::Reader reader(comp.raw_data);
        proto::Field f;
        while (reader.read_field(f)) {
            if (static_cast<int>(f.field_number) == expected_payload_field &&
                f.wire_type == proto::WIRE_LEN)
                return f.bytes_val;
        }
    } catch (...) {}
    return {};
}

// Parse entity component from the EntityComponent payload bytes.
// Schema (scene_schemas.cpp EntityComponent):
//   field 8  (VARINT) = FacingDirection
//   field 16 (VARINT) = PhysicsEnabled
static EntityComponentData parse_entity_component_payload(const std::string& bytes) {
    EntityComponentData d;
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_VARINT)
                d.facing_direction = static_cast<int>(f.varint_val);
            else if (f.field_number == 2 && f.wire_type == proto::WIRE_VARINT)
                d.physics_enabled = f.varint_val != 0;
        }
    } catch (...) {}
    return d;
}

// Parse entity controller component payload.
// Schema (scene_schemas.cpp EntityControllerComponent):
//   field 8  (LEN)    = EntityId (string)
//   field 16 (LEN)    = AnimationControllerId (string)
static EntityControllerData parse_entity_controller_payload(const std::string& bytes) {
    EntityControllerData d;
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_LEN)
                d.entity_id = f.bytes_val;
            else if (f.field_number == 2 && f.wire_type == proto::WIRE_LEN)
                d.animation_controller = f.bytes_val;
        }
    } catch (...) {}
    return d;
}

// Parse MonsterEntityComponent payload.
// Schema: field 10 = OnKill (LEN/Program), field 18 = OnHurt (LEN/Program),
//         field 24 = GivesExperience (VARINT), field 32 = DefaultDeathAnimation (VARINT)
static MonsterEntityData parse_monster_entity_payload(const std::string& bytes) {
    MonsterEntityData d;
    d.present = true;
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 3 && f.wire_type == proto::WIRE_VARINT)
                d.gives_experience = f.varint_val != 0;
            else if (f.field_number == 4 && f.wire_type == proto::WIRE_VARINT)
                d.default_death_anim = static_cast<int>(f.varint_val);
        }
    } catch (...) {}
    return d;
}

EntityData entity_parse(const SceneObject& obj) {
    EntityData result;
    const auto& comps = obj.resolved_components.empty() ? obj.components : obj.resolved_components;

    for (const auto& comp : comps) {
        const std::string& tn = comp.type_name;

        // EntityComponent — base entity; carries FacingDirection + PhysicsEnabled
        if (type_matches(tn, TYPE_ENTITY_COMPONENT)) {
            result.is_entity = true;
            std::string payload = read_payload(comp, PAYLOAD_ENTITY_COMPONENT);
            if (payload.empty()) {
                // Fallback: scan for any >= 50 payload field (non-batched component)
                payload = read_payload(comp, comp.payload_field);
            }
            if (!payload.empty())
                result.entity = parse_entity_component_payload(payload);
        }

        // EntityControllerComponent — links entity to animation controller
        else if (type_matches(tn, TYPE_ENTITY_CONTROLLER)) {
            result.is_entity = true;
            std::string payload = read_payload(comp, PAYLOAD_ENTITY_CONTROLLER);
            if (payload.empty()) payload = read_payload(comp, comp.payload_field);
            if (!payload.empty())
                result.controller = parse_entity_controller_payload(payload);
        }

        // HeroEntityComponent — marks the player character
        else if (type_matches(tn, TYPE_HERO_ENTITY)) {
            result.is_entity = true;
            result.is_hero = true;
            result.hero.present = true;
        }

        // MonsterEntityComponent — marks a monster NPC
        else if (type_matches(tn, TYPE_MONSTER_ENTITY)) {
            result.is_entity = true;
            result.is_monster = true;
            std::string payload = read_payload(comp, PAYLOAD_MONSTER_ENTITY);
            if (payload.empty()) payload = read_payload(comp, comp.payload_field);
            if (!payload.empty())
                result.monster = parse_monster_entity_payload(payload);
            else
                result.monster.present = true;
        }
    }

    return result;
}

// ─── Animation bindings (KeyframeAnimationComponent + controller scalars) ──
// Payload field numbers = schema_tag >> 3, verified against real scene data:
//   KeyframeAnimation tag 818  → payload 102 (Name lives here, field 2)
//   MonsterController tag 2418  → payload 302 (WalkSpeed tag 13 → field 1)
//   ChargingMonsterController   → payload 304 (RunSpeed tag 37 → field 4)
//   BouncingMonsterController   → payload 312 (JumpSpeed tag 37 → field 4)
static constexpr int PAYLOAD_KEYFRAME_ANIM   = 102;
static constexpr int PAYLOAD_MONSTER_CTRL    = 302;
static constexpr int PAYLOAD_CHARGING_CTRL   = 304;
static constexpr int PAYLOAD_BOUNCING_CTRL   = 312;

static std::string payload_string(const std::string& payload, uint32_t field_no) {
    try {
        proto::Reader r(payload);
        proto::Field f;
        while (r.read_field(f)) {
            if (f.field_number == field_no && f.wire_type == proto::WIRE_LEN)
                return f.bytes_val;
        }
    } catch (...) {}
    return {};
}

static float payload_float(const std::string& payload, uint32_t field_no) {
    try {
        proto::Reader r(payload);
        proto::Field f;
        while (r.read_field(f)) {
            if (f.field_number == field_no && f.wire_type == proto::WIRE_I32)
                return f.float_val;
        }
    } catch (...) {}
    return 0.0f;
}

static bool payload_flag(const std::string& payload, uint32_t field_no, bool def) {
    try {
        proto::Reader r(payload);
        proto::Field f;
        while (r.read_field(f)) {
            if (f.field_number == field_no && f.wire_type == proto::WIRE_VARINT)
                return f.varint_val != 0;
        }
    } catch (...) {}
    return def;
}

AnimBindings anim_bindings(const SceneObject& obj) {
    AnimBindings b;
    const auto& comps = obj.resolved_components.empty() ? obj.components
                                                        : obj.resolved_components;
    for (const auto& c : comps) {
        const std::string& tn = c.type_name;
        if (type_matches(tn, "KeyframeAnimation")) {
            std::string payload = read_payload(c, c.payload_field ? c.payload_field
                                                                  : PAYLOAD_KEYFRAME_ANIM);
            if (payload.empty()) payload = read_payload(c, PAYLOAD_KEYFRAME_ANIM);
            b.pod = payload_string(payload, 2);
            b.repeating = payload_flag(payload, 3, true);
            const float sp = payload_float(payload, 4);
            if (sp > 0.0f) b.speed_multiplier = sp;
            if (!b.pod.empty()) b.present = true;
        } else if (type_matches(tn, "MonsterController")) {
            std::string payload = read_payload(c, c.payload_field ? c.payload_field
                                                                  : PAYLOAD_MONSTER_CTRL);
            if (payload.empty()) payload = read_payload(c, PAYLOAD_MONSTER_CTRL);
            const float ws = payload_float(payload, 1);
            if (ws > 0.0f) b.walk_speed = ws;
        } else if (type_matches(tn, "BouncingMonsterController")) {
            std::string payload = read_payload(c, PAYLOAD_BOUNCING_CTRL);
            if (payload.empty()) payload = read_payload(c, c.payload_field);
            const float js = payload_float(payload, 4);
            if (js > 0.0f) b.jump_speed = js;
        } else if (type_matches(tn, "ChargingMonsterController")) {
            std::string payload = read_payload(c, PAYLOAD_CHARGING_CTRL);
            if (payload.empty()) payload = read_payload(c, c.payload_field);
            const float rs = payload_float(payload, 4);
            if (rs > 0.0f) b.run_speed = rs;
        }
    }
    return b;
}

// ============================================================================
// RUNTIME ENTITY MANAGEMENT + AI LOOP
// ============================================================================

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool comp_has(const SceneObject& o, const char* needle) {
    const auto& comps = o.resolved_components.empty() ? o.components
                                                      : o.resolved_components;
    for (const auto& c : comps)
        if (c.type_name.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

bool entity_is_animated(const SceneObject& o) {
    if (comp_has(o, "MonsterDeathController")) return false;
    return comp_has(o, "MonsterController") || comp_has(o, "CharAnimController") ||
           comp_has(o, "CharController") || comp_has(o, "HeroEntity") ||
           comp_has(o, "EntityController");
}

EntityAiKind entity_classify_ai(const SceneObject& o) {
    const auto& comps = o.resolved_components.empty() ? o.components
                                                      : o.resolved_components;
    for (const auto& c : comps) {
        const std::string& t = c.type_name;
        if (t.find("BatMonsterController") != std::string::npos)      return EntityAiKind::Bat;
        if (t.find("ChargingMonsterController") != std::string::npos) return EntityAiKind::Charger;
        if (t.find("BouncingMonsterController") != std::string::npos) return EntityAiKind::Bouncer;
        if (t.find("ShootingMonsterController") != std::string::npos) return EntityAiKind::Archer;
        if (t.find("StaticMonsterController") != std::string::npos)   return EntityAiKind::Static;
        if (t.find("WalkingMonsterController") != std::string::npos)  return EntityAiKind::Walker;
        if (t.find("SkellyMonsterController") != std::string::npos)   return EntityAiKind::Walker;
        if (t.find("GenericMonsterController") != std::string::npos)  return EntityAiKind::Walker;
        if (t.find("LeapingMonsterController") != std::string::npos)  return EntityAiKind::Leaper;
        if (t.find("SnappingMonsterController") != std::string::npos) return EntityAiKind::Static;
        if (t.find("ProjectileMonsterController") != std::string::npos) return EntityAiKind::Archer;
        // The generic MonsterControllerComponent is the base walker (the
        // specific controllers all derive from it and are matched above).
        if (t.find("MonsterController") != std::string::npos)         return EntityAiKind::Walker;
    }
    return EntityAiKind::None;
}

void EntityManager::clear() { entities.clear(); }

void EntityManager::spawn_from_object(const SceneObject& o, int index) {
    Entity e;
    e.object_index = index;
    e.name = o.name;
    e.pos[0] = o.pos_x; e.pos[1] = o.pos_y; e.pos[2] = o.pos_z;
    e.home[0] = o.pos_x; e.home[1] = o.pos_y; e.home[2] = o.pos_z;
    e.rot = o.rot_y;
    e.phase = static_cast<float>((index * 2654435761u) % 1000) / 1000.0f * 6.28318f;

    const EntityData ed = entity_parse(o);
    e.is_hero = ed.is_hero;
    e.is_monster = ed.is_monster;
    if (ed.is_entity) e.body.physics_enabled = ed.entity.physics_enabled;

    e.kind = entity_classify_ai(o);
    const AnimBindings ab = anim_bindings(o);
    e.anim_pod = ab.pod;
    if (ab.walk_speed > 1.0f) e.speed = ab.walk_speed;
    if (ab.run_speed  > 1.0f) e.run_speed = ab.run_speed;
    if (ab.jump_speed > 1.0f) e.jump_speed = ab.jump_speed;

    const PhysicsData pd = physics_parse(o);
    if (pd.enabled) {
        e.gravity = pd.gravity > 0.0f ? pd.gravity * 1.0f : 1472.0f;
        e.body.friction = pd.ground_deceleration > 0.0f ? pd.ground_deceleration : 0.0f;
    }
    // The stock engine accelerates its characters at ~900 u/s² (hero walk
    // reachable in a fraction of a second); the neutral 60 u/s² default would
    // make every monster sluggish (tens of seconds to reach walk speed).
    e.body.accel = 900.0f;

    // Collision hitbox from the CollisionShape rect (scaled) — monsters
    // collide with walls / ceilings / ground like the player.
    const CollisionData cd = collision_parse(o);
    for (const auto& s : cd.shapes) {
        if (s.type == COLL_RECT && s.rect[2] > 1.0f && s.rect[3] > 1.0f) {
            e.hit_half_w = std::max(2.0f, s.rect[2] * o.scale_x * 0.5f);
            e.hit_h      = std::max(4.0f, s.rect[3] * o.scale_y);
            break;
        }
    }

    // MonsterEntityComponent: XP flag (GivesExperience) + default death anim.
    if (ed.is_monster) {
        e.gives_xp = ed.monster.gives_experience;
        e.hp = e.max_hp = 100;
    } else if (ed.is_hero) {
        e.hp = e.max_hp = 100;
    }
    e.body.pos[0] = e.pos[0];
    e.body.pos[1] = e.pos[1];
    entities.push_back(std::move(e));
}

void EntityManager::build_from_scene(const SceneData& scene) {
    clear();
    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        if (!entity_is_animated(scene.objects[i])) continue;
        spawn_from_object(scene.objects[i], i);
    }
}

Entity* EntityManager::find(const std::string& name) {
    for (auto& e : entities)
        if (e.name == name) return &e;
    return nullptr;
}

// The entity's swept vertical physics — same gravity/land/ceiling rules the
// player gets (game_resolve_y + terrain ceilings), plus the new wall world
// (wall_clamp_circle) so monsters respect polygon walls, not just AABBs.
static void entity_physics(Entity& e, const sg::GameWorld& w, float dt) {
    // Sync the PhysicsObjectState body to the entity position, then run the
    // horizontal step (ramp/accel/friction) and clamp the circle out of every
    // solid wall active at this depth.
    e.body.pos[0] = e.pos[0];
    e.body.pos[1] = e.pos[1];
    physics_step(e.body, dt);
    e.pos[0] = e.body.pos[0];
    e.pos[1] = e.body.pos[1];
    av::wall_clamp_circle(w.walls, e.pos[0], e.pos[1], e.pos[2], e.hit_half_w);
    e.body.pos[0] = e.pos[0];
    e.body.pos[1] = e.pos[1];

    // Ground probe (collider tops + terrain heightfield).
    float hg = 0.0f;
    const bool has_ground = sg::game_ground_found(w, e.pos[0], e.pos[2],
                                                  e.pos[1], 80.0f, hg);
    if (!has_ground && e.pos[1] < e.home[1] - 150.0f) {
        e.pos[1] = e.home[1];   // lost over a pit → back to the anchor
        e.vy = 0.0f;
    }

    // Vertical sweep: gravity → terminal → integrate → land / head bump.
    const float prev_feet = e.pos[1];
    e.vy -= e.gravity * dt;
    e.vy = std::max(e.vy, -900.0f);
    e.pos[1] += e.vy * dt;
    bool hit_gnd = false;
    const float ry = sg::game_resolve_y(w, e.pos[0], e.pos[1], e.pos[2],
                                        e.hit_half_w, e.hit_h, prev_feet,
                                        hit_gnd);
    if (hit_gnd) {
        e.pos[1] = ry;
        e.vy = 0.0f;
        e.body.grounded = true;
    } else if (ry < e.pos[1] && e.vy > 0.0f) {
        e.pos[1] = ry;         // head bump
        e.vy = 0.0f;
    }
    // Terrain ceiling (ground-mesh undersides).
    if (e.vy > 0.0f) {
        float ceil_h = 0.0f;
        if (av::terrain_ceiling_near(w.terrain, e.pos[0], e.pos[2],
                                     prev_feet + e.hit_h, 6.0f, ceil_h)) {
            const float head = e.pos[1] + e.hit_h;
            if (head > ceil_h) { e.pos[1] = ceil_h - e.hit_h; e.vy = 0.0f; }
        }
    }
    if (e.pos[1] <= hg && has_ground) {
        e.pos[1] = hg;
        e.vy = 0.0f;
        e.body.grounded = true;
    } else if (has_ground) {
        const float climb = std::clamp(hg - e.pos[1], -60.0f, 4.0f);
        e.pos[1] += climb * std::min(1.0f, dt * 18.0f);
    }
    e.body.pos[0] = e.pos[0];
    e.body.pos[1] = e.pos[1];
}

void EntityManager::update_all(float dt, const float hero_pos[2], float hero_vy,
                               bool hero_active, int hero_level,
                               const sg::GameWorld& world, HeroContact& out) {
    out = HeroContact{};
    if (dt <= 0.0f) return;
    const float hx = hero_pos[0], hy = hero_pos[1];

    for (auto& e : entities) {
        if (!e.active || e.dead || e.lua_driven) continue;
        if (e.hurt_timer > 0.0f) e.hurt_timer -= dt;
        if (e.attack_cd > 0.0f) e.attack_cd -= dt;

        const float dx = hx - e.pos[0];
        const float dy = hy - e.pos[1];
        const float dist2d = std::sqrt(dx * dx + dy * dy);
        const bool hero_close = hero_active && dist2d < e.detect_range;

        // ── AI by archetype (ported *MonsterControllerComponent::Update) ──
        switch (e.kind) {
        case EntityAiKind::Walker: {
            const float xmin = e.home[0] - e.patrol_range;
            const float xmax = e.home[0] + e.patrol_range;
            constexpr float kDeadzone = 14.0f;
            constexpr float kCooldown = 0.4f;
            if (e.chasing && hero_close) {
                // Chasing: turn toward the hero (with a turn cooldown) and
                // walk only when outside the deadzone.
                if (dx >  kDeadzone && e.dir < 0.0f && e.attack_cd <= 0.0f) {
                    e.dir = 1.0f; e.attack_cd = kCooldown;
                } else if (dx < -kDeadzone && e.dir > 0.0f && e.attack_cd <= 0.0f) {
                    e.dir = -1.0f; e.attack_cd = kCooldown;
                }
                e.moving = std::fabs(dx) >= kDeadzone;
            } else {
                // Patrolling: always walk between the home bounds, flipping
                // direction at the edges.
                e.chasing = hero_close;
                e.moving = true;
                if (e.pos[0] <= xmin) { e.pos[0] = xmin; e.dir = 1.0f; }
                if (e.pos[0] >= xmax) { e.pos[0] = xmax; e.dir = -1.0f; }
            }
            // WalkingMonsterController: velocity = walkSpeed along the facing.
            // vel_target_x is a MAGNITUDE: the surface frame built from
            // move_dir supplies the direction (frame-x = along the facing),
            // so dir ∈ {±1} must NOT be baked into the target here. When
            // chasing but inside the deadzone the monster stops (velocity 0).
            const float spd = e.chasing && e.moving ? e.speed * 1.5f
                            : e.chasing           ? 0.0f
                            : e.speed;
            e.body.vel_target_x = spd;
            av::physics_set_move_dir(e.body, e.dir, 0.0f);
            e.rot = (e.dir < 0.0f) ? kPi : 0.0f;
            break;
        }
        case EntityAiKind::Bat: {
            // BatMonsterController: Lissajous figure-8 around home.
            e.phase += dt;
            const float tx = e.home[0] + std::cos(e.phase * 0.2f * 6.28318f) * 200.0f;
            const float ty = e.home[1] + std::sin(e.phase * 0.5f * 6.28318f) * 250.0f;
            float vx = tx - e.pos[0], vy = ty - e.pos[1];
            const float vl = std::sqrt(vx * vx + vy * vy);
            const float maxstep = 600.0f * dt;
            if (vl > maxstep && vl > 1e-3f) {
                e.pos[0] += vx / vl * maxstep;
                e.pos[1] += vy / vl * maxstep;
            } else { e.pos[0] = tx; e.pos[1] = ty; }
            e.rot = (vx < 0.0f) ? kPi : 0.0f;
            e.moving = true;
            continue;   // bats fly — no ground physics
        }
        case EntityAiKind::Charger: {
            // ChargingMonsterController: 100×250 detection box.
            const bool in_box = hero_active && std::fabs(dx) < 100.0f &&
                                std::fabs(dy) < 250.0f;
            if (e.chasing) {
                const bool passed = hero_active && (dx * e.dir) < 0.0f;
                if (passed && e.attack_cd > 0.4f) {
                    e.chasing = false;
                    e.attack_cd = 0.0f;
                    e.dir = -e.dir;
                    e.body.vel_target_x = 0.0f;
                } else {
                    const float spd = e.run_speed > 0.0f ? e.run_speed
                                                         : e.speed * 2.6f;
                    e.body.vel_target_x = spd;   // magnitude along the facing
                    av::physics_set_move_dir(e.body, e.dir, 0.0f);
                }
                e.moving = e.chasing;
                e.rot = (e.dir < 0.0f) ? kPi : 0.0f;
            } else {
                e.attack_cd = 0.0f;
                e.moving = false;
                e.body.vel_target_x = 0.0f;
                if (in_box) {
                    e.chasing = true;
                    e.dir = (dx > 0.0f) ? 1.0f : -1.0f;
                    e.rot = (dx < 0.0f) ? kPi : 0.0f;
                }
            }
            break;
        }
        case EntityAiKind::Bouncer: {
            // BouncingMonsterController: gravity Jump() loop; faces the hero.
            if (e.body.grounded) {
                e.vy = e.jump_speed;
                e.body.grounded = false;
                e.pos[1] += 1.0f;
            }
            e.moving = true;
            if (hero_close) e.rot = (dx < 0.0f) ? kPi : 0.0f;
            break;
        }
        case EntityAiKind::Leaper: {
            // LeapingMonsterController: hops toward the hero when in range.
            // The lunge = jump impulse + walk-speed horizontal drive via the
            // same PhysicsObjectState body the walkers use.
            if (e.body.grounded) {
                if (hero_close && std::fabs(dx) > 24.0f) {
                    e.dir = (dx > 0.0f) ? 1.0f : -1.0f;
                    e.vy = e.jump_speed > 0.0f ? e.jump_speed : 850.0f;
                    e.body.vel_target_x = e.speed;
                    av::physics_set_move_dir(e.body, e.dir, 0.0f);
                    e.body.grounded = false;
                    e.pos[1] += 1.0f;
                    e.moving = true;
                } else {
                    e.body.vel_target_x = 0.0f;
                    e.moving = false;
                }
            } else {
                e.moving = true;      // mid-air: keep the lunge
            }
            if (hero_close) e.rot = (dx < 0.0f) ? kPi : 0.0f;
            break;
        }
        case EntityAiKind::Archer:
        case EntityAiKind::Static:
        default: {
            // StaticMonsterController: stays put; faces the hero in range.
            e.moving = false;
            e.body.vel_target_x = 0.0f;
            if (hero_close) e.rot = (dx < 0.0f) ? kPi : 0.0f;
            break;
        }
        }

        // ── Physics integration (swept vertical + wall world + body) ──
        entity_physics(e, world, dt);

        // ── Hero interactions → gamestate events ──
        if (!hero_active || e.is_hero) continue;
        const float hw = e.hit_half_w + 8.0f;    // hero half width ≈ 8
        if (std::fabs(hx - e.pos[0]) < hw &&
            std::fabs(hy - e.pos[1]) < e.hit_h + 8.0f) {
            if (hero_vy < -250.0f && !e.is_monster) continue;  // only monsters
            if (hero_vy < -250.0f) {
                // Stomp (falling onto the monster): damage it, bounce hero.
                if (e.hurt_timer <= 0.0f) {
                    e.hp -= 20;
                    e.hurt_timer = 0.35f;
                }
                out.stomped = true;
                if (e.hp <= 0 && !e.dead) {
                    e.dead = true;
                    e.moving = false;
                    ++out.kills;
                    if (e.gives_xp) {
                        out.xp_gained = GameState::xp_for_kill(hero_level, 1, 15);
                    }
                    out.last_kill = e.name;
                }
            } else if (e.is_monster && !e.dead) {
                out.hurt_hero = true;   // monster contact damage
            }
        }
    }
}

// ============================================================================
// GameState — CharacterState ports
// ============================================================================

float GameState::entity_health_multiplier(int level) {
    const float v2 = ((level - 1) * 0.5f + 1.0f) * 7.5f;
    if (level <= 3) return std::max(v2 * 0.5f, 1.0f);
    if (level == 4) return std::max(v2 * 0.75f, 1.0f);
    return std::max(v2, 1.0f);
}

int GameState::base_xp_for_kill(int monster_level, int base_xp) {
    const float mult = entity_health_multiplier(monster_level);
    if (mult <= 0.0f) return 0;
    return (int)std::llround((float)((monster_level + 3) * base_xp) / mult);
}

int GameState::xp_for_kill(int hero_level, int monster_level, int base_xp) {
    const int result = base_xp_for_kill(monster_level, base_xp);
    if (hero_level == monster_level) return result;
    if (hero_level > monster_level) {
        const int diff = hero_level - monster_level;
        if (monster_level < 1 || diff > 4) return 0;   // too weak → no XP
        const float pen = 1.0f - std::min((float)diff / 6.0f, 1.0f);
        return (int)std::llround(pen * result);
    }
    const int diff = monster_level - hero_level;
    const float bonus = diff <= 4 ? 1.0f + diff * 0.05f : 1.2f;
    return (int)std::llround(bonus * result);
}

int GameState::xp_required_for_level(int level) {
    // Reconstructed from the CharacterState::LoadFromProtobufMessage
    // accumulation loop: xp += round((level·8 + 24) · round(xp·/ mult)).
    const float mult = entity_health_multiplier(level);
    if (mult <= 0.0f) return 100;
    return (int)std::llround((level * 8.0f + 24.0f) * 100.0f / mult);
}

void GameState::add_xp(int amount) {
    xp += std::max(0, amount);
    while (xp >= xp_required_for_level(level) && level < 99) {
        xp -= xp_required_for_level(level);
        ++level;
    }
}

void GameState::add_item(const std::string& name) { items.push_back(name); }

int GameState::item_count(const std::string& name) const {
    int n = 0;
    for (const auto& i : items) if (i == name) ++n;
    return n;
}

bool GameState::has_item(const std::string& name) const {
    return item_count(name) > 0;
}

} // namespace av
