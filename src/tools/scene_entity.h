#pragma once
/* scene_entity.h — Swordigo entity subsystem data structures
 *
 * Faithfully reconstructed from:
 *   - IDA decompilations: EntityComponent, EntityControllerComponent,
 *     HeroEntityComponent, MonsterEntityComponent (OpenSwordigo/resources/ida_decompiled/)
 *   - scene_schemas.cpp field layouts (confirmed component field numbers)
 *   - arm32 libswordigo_ida32.c entity offset references
 *
 * These structures are read-only parse results — they describe runtime entity
 * behavior as stored in scene files. They are NOT a full runtime simulation.
 *
 * Usage:
 *   EntityData ed = entity_parse(scene_object);
 *   if (ed.is_hero) { ... }
 *
 * Compatible with Ruby SDK, Studio, and future Swordfare host use.
 */

#include <string>
#include <vector>

#include "tools/scene_physics.h"

namespace sg { struct GameWorld; }   // forward decl — full def in scene_game.h

namespace av {
struct SceneObject;   // forward decl — full definition lives in scene_loader.h
struct SceneData;     // forward decl — full definition lives in scene_loader.h

// ─── EntityComponent (Component field 1218 >> 3 = 152, payload) ─────────
// Source: Caver::EntityComponent::LoadFromProtobufMessage @ 0x267CE4
//         EntityComponent schema: field 8 = FacingDirection (varint),
//                                 field 16 = PhysicsEnabled (varint/bool)
struct EntityComponentData {
    int  facing_direction  = 1;    // field 8  : 1=right, -1=left
    bool physics_enabled   = true; // field 16 : default true
};

// ─── EntityControllerComponent (Component field 1290 >> 3 = 161) ────────
// Source: EntityControllerComponent schema: field 8 = EntityId (string),
//         field 16 = AnimationControllerId (string)
struct EntityControllerData {
    std::string entity_id;             // field 8  : linked entity identifier
    std::string animation_controller;  // field 16 : animation controller id
};

// ─── HeroEntityComponent (Component field 1322 >> 3 = 165) ─────────────
// Source: HeroEntityComponent schema: field 10 = OnItemGet (Program)
// The hero component marks the player character — there is only one.
struct HeroEntityData {
    bool present = false;   // true if this component exists
};

// ─── MonsterEntityComponent (Component field 1266 >> 3 = 158) ───────────
// Source: MonsterEntityComponent schema:
//   field 10 = OnKill (Program),
//   field 18 = OnHurt (Program),
//   field 24 = GivesExperience (varint),
//   field 32 = DefaultDeathAnimation (varint/enum)
struct MonsterEntityData {
    bool  present             = false;
    bool  gives_experience    = false; // field 24
    int   default_death_anim  = 0;     // field 32 : animation enum index
};

// ─── Aggregate entity data for a SceneObject ────────────────────────────
struct EntityData {
    bool is_entity           = false;  // true if any entity component found
    bool is_hero             = false;  // HeroEntityComponent present
    bool is_monster          = false;  // MonsterEntityComponent present

    EntityComponentData    entity;
    EntityControllerData   controller;
    HeroEntityData         hero;
    MonsterEntityData      monster;
};

/// Parse entity-related component data from a SceneObject.
/// Returns an EntityData with is_entity=false if no entity components found.
EntityData entity_parse(const SceneObject& obj);

// ─── Animation binding (KeyframeAnimationComponent, Component field 818) ──
// Source: KeyframeAnimationComponent schema (scene_schemas.cpp):
//   field 1 = ModelId (varint), field 2 = Name (LEN — the POD model name),
//   field 3 = Repeating (varint), field 4 = SpeedMultiplier (fixed32).
// The Name IS the exact model the game plays for this object — verified
// against real scenes: firebat → "bat_fly", prisoner → "npc_stand",
// shadowblob → "snowball_land". Controllers (MonsterController etc.) link
// to it at runtime and switch PODs via Lua; the scene data binds the
// default/loop animation, and we expose that binding plus the controller
// speed scalars that the game reads (WalkSpeed, RunSpeed, JumpSpeed).
struct AnimBindings {
    bool         present          = false;
    bool         repeating        = true;
    float        speed_multiplier = 1.0f;
    std::string  pod;             // bound POD model name (no extension)
    float        walk_speed       = 0.0f; // MonsterControllerComponent.WalkSpeed (tag 13)
    float        jump_speed       = 0.0f; // BouncingMonsterControllerComponent.JumpSpeed (tag 37)
    float        run_speed        = 0.0f; // ChargingMonsterControllerComponent.RunSpeed (tag 37)
};

/// Parse the animation binding + controller speed scalars from a SceneObject.
AnimBindings anim_bindings(const SceneObject& obj);

// ============================================================================
// RUNTIME ENTITY MANAGEMENT + AI LOOP + GAMESTATE
// ============================================================================
// Reconstructed from Caver::SceneObject::Update/Process, the *MonsterController
// component Update functions (Walking @0x2A1394, Bouncing @0x294B58, Charging
// @0x2962EC, Static @0x2A0C58), MonsterEntityComponent::HandleDamageImpact
// (@0x27C118) and CharacterState (ExperiencePointsForKillingMonster
// @0x3AC838, BaseExperienceForKillingMonster @0x3AC800,
// EntityHealthMultiplierAtLevel @0x3B16B0).

// Enemy archetype taxonomy (arm32 *MonsterController). Numeric values match
// sp::AiKind so the scene player can map between them with a cast.
enum class EntityAiKind : int {
    None = 0,
    Walker,     // patrols X between home ±range, turns at the edges
    Bat,        // Lissajous figure-8 around home, chases when hero close
    Charger,    // idles until the hero enters its detection box, then charges
    Bouncer,    // gravity jump loop; faces the nearest enemy
    Leaper,     // hops toward the hero when in range (horizontal lunge)
    Static,     // stays put, faces the hero when in range
    Archer,     // stays put, faces + aims at the hero
};

// One runtime entity — the live, mutable record the AI loop + physics own
// each tick. Modeled on SceneObject::Update/Process lifecycle fields.
struct Entity {
    int    object_index = -1;    // SceneData.objects index (or -1 for spawned)
    std::string name;
    std::string anim_pod;        // bound POD name (KeyframeAnimation)
    float  pos[3]  = {0, 0, 0};  // live world position
    float  home[3] = {0, 0, 0};  // spawn / patrol anchor
    float  rot = 0.0f;           // facing (0 = +X, pi = −X)
    float  frame = 0.0f;         // animation frame
    bool   moving = false;
    bool   active = true;        // Activate/Deactivate lifecycle
    bool   dead = false;
    bool   lua_driven = false;   // the SCL Lua host owns this object

    EntityAiKind kind = EntityAiKind::None;
    float patrol_range = 90.0f;   // walker roam half-range
    float detect_range = 320.0f;  // aggro range
    float speed        = 55.0f;   // walk / chase speed
    float run_speed    = 0.0f;    // ChargingMonsterController.RunSpeed
    float jump_speed   = 800.0f;  // BouncingMonsterController.JumpSpeed
    float dir = 1.0f;             // current patrol / facing direction
    bool  chasing = false;
    float attack_cd = 0.0f;       // flip / turn cooldown
    float phase = 0.0f;

    // ── physics (PhysicsObjectState port) + swept vertical ──
    PhysicsBody body;             // horizontal ramp/accel/friction/max-vel
    float  vy = 0.0f;             // vertical velocity (gravity-owned)
    float  gravity = 1472.0f;     // per-second² (engine default; scl overrides)
    float  hit_half_w = 8.0f;     // collision half width
    float  hit_h = 26.0f;         // collision full height

    // ── combat / gamestate ──
    int    hp = 100, max_hp = 100;
    bool   gives_xp = true;
    float  hurt_timer = 0.0f;     // hurt invulnerability window
    bool   is_hero = false;
    bool   is_monster = false;
};

struct EntityManager {
    std::vector<Entity> entities;

    void clear();
    /// Spawn one entity from a scene object (index into SceneData.objects).
    void spawn_from_object(const SceneObject& o, int index);
    /// Build the full entity set from a scene (AI classification, physics
    /// hitbox, animation binding, controller speeds — all data-driven).
    void build_from_scene(const SceneData& scene);
    Entity* find(const std::string& name);

    /// The full AI + physics + gamestate loop (per-frame). Runs every entity's
    /// Update, integrates PhysicsObjectState physics against the game world's
    /// walls/terrain, and reports hero interactions for the scene player to
    /// apply (stomps, contact damage, kills → XP).
    struct HeroContact {
        bool   stomped = false;    // hero stomped a monster this frame
        bool   hurt_hero = false;  // hero touched a monster (contact damage)
        int    xp_gained = 0;      // XP from kills this frame
        int    kills = 0;          // monsters killed this frame
        std::string last_kill;     // identifier of the last monster killed
    };
    void update_all(float dt, const float hero_pos[2], float hero_vy,
                    bool hero_active, int hero_level,
                    const sg::GameWorld& world, HeroContact& out);
};

/// Classify the monster archetype from the *MonsterController component.
EntityAiKind entity_classify_ai(const SceneObject& o);

// ─── GameState (Caver::CharacterState + GameSceneController ports) ─────
// The mini-Swordigo gamestate subsystem: level / XP / items / coins and the
// exact economy formulas from the decompiled CharacterState.
struct GameState {
    int  level = 1;
    int  xp = 0;
    int  coins = 0;
    int  kills = 0;
    std::vector<std::string> items;

    /// GameData::EntityHealthMultiplierAtLevel @0x3B16B0 — HP/XP scale per
    /// monster level (faithful port).
    static float entity_health_multiplier(int level);

    /// BaseExperienceForKillingMonster @0x3AC800 — (level+3)·baseXP / mult.
    static int base_xp_for_kill(int monster_level, int base_xp);

    /// ExperiencePointsForKillingMonster @0x3AC838 — level-differential
    /// scaling: killing a stronger monster pays up to +20%, a weaker one is
    /// penalized (≥5 levels below → 0 XP).
    static int xp_for_kill(int hero_level, int monster_level, int base_xp);

    /// XP needed to go from @p level to level+1. Reconstructed from the
    /// CharacterState::LoadFromProtobufMessage accumulation loop
    /// (xp += round((level·8 + 24) · base)).
    static int xp_required_for_level(int level);

    /// Add XP and run the level-up loop (ApplyLevelUp: level increments while
    /// XP remains).
    void add_xp(int amount);

    void add_item(const std::string& name);
    int  item_count(const std::string& name) const;
    bool has_item(const std::string& name) const;
};

/// True when the object is an animated entity (monster/hero) whose collision
/// shapes are dynamic — same taxonomy the collision world uses to exclude
/// them from static walls.
bool entity_is_animated(const SceneObject& o);

} // namespace av
