/* scene_player.h — Ruby Scene Player subsystem.
 *
 * A playback engine that runs a .scene like the actual game: a shared scene
 * clock, per-object animation clocks, entity/AI simulation (walk, patrol,
 * physics-y motion), and an optional playable Hiro ("Spawn Hiro and Play").
 *
 * The editor's static visualizer renders bind-pose models; this module owns
 * the *runtime* state (positions, frames, AI timers) and writes it back into
 * lightweight per-object play state that the visualizer draws.
 *
 * Design goals (per .agents/npm.md + main.js/arm32 reverse engineering):
 *   - Everything lives here, NOT in asset_viewer.cpp.
 *   - Pure logic + a tiny ImGui overlay panel; no rendering of its own —
 *     the visualizer consumes the play state.
 *   - Two modes share one engine: kVisualise (AI/logic only) and kPlayHiro
 *     (hero spawned at the scene SpawnPoint, camera mimics the game).
 */
#ifndef SCENE_PLAYER_H_
#define SCENE_PLAYER_H_

#include <cstdint>
#include <string>
#include <vector>

#if !defined(SWORDIGO_NO_IMGUI) && !defined(GODOT_ENABLED)
#include "imgui.h"
#include "platform/IconsFontAwesome6.h"
#define SCENE_PLAYER_HAS_IMGUI 1
#endif


#include "tools/scene_loader.h"
#include "tools/av_renderer.h"
#include "tools/scene_game.h"
#include "tools/scene_entity.h"

namespace sp {

// Playback modes (Mode → Scene Player menu).
enum class Mode : int {
    Off = 0,        // static editor view
    Visualise,      // play scene AI / logic / animations (no hero)
    PlayHiro,       // spawn Hiro at the scene spawn point and play
};

// Enemy AI archetypes (from arm32 *MonsterController taxonomy).
enum class AiKind : int {
    None = 0,
    Walker,     // patrols X between home ±range, turns at edges
    Bat,        // hovers/bobs around home, chases hero when close
    Charger,    // idles until hero in range, then charges at hero
    Bouncer,    // bounces vertically around home
    Static,     // stays put, faces hero when in range
    Archer,     // stays put, faces + aims at hero
};

// Per-object runtime state the visualizer reads while playing.
struct PlayObject {
    int    index = -1;        // index into SceneData.objects
    float  pos[3] = {0, 0, 0}; // current (animated) position
    float  home[3] = {0, 0, 0}; // spawn/patrol anchor position
    float  rot = 0.0f;        // current rotation (radians; 0 = +X, pi = -X)
    float  frame = 0.0f;      // current POD animation frame
    bool   moving = false;    // AI thinks it's moving (affects anim choice)
    float  phase = 0.0f;      // per-object AI phase timer
    AiKind kind = AiKind::None;
    float  patrol_range = 80.0f;
    float  detect_range = 320.0f;
    float  chase_speed  = 60.0f;
    float  dir = 1.0f;        // current patrol direction
    bool   chasing = false;   // currently hunting the hero
    float  attack_cd = 0.0f;  // attack cooldown timer (visual only)
    float  vel_y = 0.0f;      // vertical velocity (bouncers use gravity)

    // ── SCL Lua AI runtime state (driven by the real game scripts) ──
    bool   lua_driven = false;          // object runs real Lua programs
    std::string name;                   // Identifier (Lua self:identifier())
    float  vel[3] = {0.0f, 0.0f, 0.0f}; // current velocity (setVelocity / physics)
    float  gravity[3] = {0.0f, -1472.0f, 0.0f}; // PhysicsObject gravity vector
    bool   physics_enabled = true;
    bool   grounded = false;
    float  decel = 0.0f;                // PhysicsObject.SetDecelerationForce
    float  accel = 0.0f;                // EntityController.SetAcceleration
    float  default_move_speed = 60.0f;
    float  move_speed = 0.0f;
    float  action_timer = 0.0f;         // >0 → performing an action (IsIdle=false)
    int    action_id = 0;
    std::string behavior;               // "" | "follow" | "patrol" | "freeze"
    int    move_anim_id = 0;            // EntityController.SetMoveAnimation
    int    anim_id = 0;                 // AnimationController.BlendToAnimation
    int    anim_frames = 30;            // frame count of the object's POD
    bool   anim_running = true;         // KeyframeAnimation.SetRunning
    bool   dead = false;
    bool   runtime = false;             // spawned by Lua (Scene.CreateObject),
                                       // no backing SceneObject (index == -1)
    float  lifespan = 0.0f;             // seconds until auto-despawn (0 = never)

    // ── Entity classification (scene_entity.h data, not string matching) ──
    bool   is_entity = false;
    bool   is_hero = false;
    bool   is_monster = false;

    // ── Data-driven animation binding (scene_entity.h anim_bindings) ──
    // The scene's KeyframeAnimation component names the exact POD the game
    // plays for this object (firebat → bat_fly, prisoner → npc_stand, ...).
    std::string anim_pod;        // bound POD model name (no extension)
    float  anim_speed = 1.0f;    // KeyframeAnimation.SpeedMultiplier
    bool   anim_repeating = true;
    float  run_speed = 0.0f;     // ChargingMonsterController.RunSpeed
    float  jump_speed = 800.0f;  // BouncingMonsterController.JumpSpeed (engine default 800)

    // ── Physics hitbox (from the object's CollisionShape rect) ──
    float  hit_half_w = 8.0f;    // half width  (colliders use half-width)
    float  hit_h = 26.0f;        // full height
};

// Hiro (player) runtime state.
struct HiroState {
    bool   active = false;
    float  pos[3] = {0, 0, 0};      // world position (x, y, depth z)
    float  spawn_x = 0.0f;          // scene spawn point (respawn anchor)
    float  spawn_y = 0.0f;
    float  spawn_z = 0.0f;
    float  vel_y = 0.0f;            // vertical velocity
    float  rot = 0.0f;              // facing: 0 = right, pi = left
    float  frame = 0.0f;            // current animation frame
    bool   grounded = false;
    bool   moving = false;
    float  speed = 0.0f;            // horizontal speed (for anim choice)
    bool   running = false;         // double-tap A/D toggles fast run
    float  run_timer = 0.0f;        // double-tap detection window
    int    last_tap_dir = 0;        // last double-tapped direction (+1/-1)
    std::string anim;               // current animation model name (hiro_run etc.)
    int    anim_frames = 30;        // frame count of current animation
    bool   anim_hold = false;       // one-shot anims (jump/land): play once,
                                    // then HOLD the last frame — never loop
                                    // (looping a 2-frame jump flickers)
    std::vector<float> anim_feet;   // per-frame feet depth of current anim
                                    // (pod_anim_feet); empty until loaded
    float  jump_held_time = 0.0f;   // variable jump height: seconds the jump
                                    // button has been held while airborne
    float  vx = 0.0f;               // smoothed horizontal velocity (accel/decel)
    float  respawn_timer = 0.0f;    // cooldown after respawn (prevents loop)
    float  feet_offset = 35.14f;    // distance from the model origin down to
                                    // the visual feet at rest pose (node
                                    // transforms + center-point; hiro_stand
                                    // ≈ 35.14 ≈ hitbox bottom −34). The
                                    // renderer measures it live from the
                                    // loaded POD via av::pod_feet_offset();
                                    // this is only the pre-load fallback.
    float  camera_target[3] = {0, 0, 0}; // smoothed camera focus (game-like)
    float  camera_smoothed[3] = {0, 0, 0};
};

// Engine state.
struct Player {
    Mode   mode = Mode::Off;
    double clock = 0.0;                 // seconds since playback started
    bool   paused = false;
    float  speed = 1.0f;
    std::vector<PlayObject> objects;
    HiroState hiro;
    std::string hero_dir;               // directory with hiro_*.POD models
    bool   hero_loaded = false;
    // Camera override for PlayHiro (game-style follow). Mirrors the real
    // Caver::CameraController (OpenSwordigo camera_controller.cpp): a
    // double-lag smoothing between the followed position and the eye, plus
    // a look-ahead in the facing direction and optional rumble.
    bool   camera_lock = false;
    bool   show_collider = false;
    float  camera_yaw = 0.0f;           // degrees; 0 = camera on +Z side
    float  camera_pitch = 28.0f;        // degrees above horizon
    float  camera_distance = 260.0f;    // world units (hero radius ~53 × 5)
    // CameraController state (game-accurate)
    float  cam_target[3] = {0,0,0};     // smoothed look-at (hero chest)
    float  cam_pos[3] = {0,0,0};        // smoothed eye position
    float  cam_target_goal[3] = {0,0,0};
    float  cam_pos_goal[3] = {0,0,0};
    float  cam_look_ahead[3] = {0,0,0}; // facing-based look-ahead offset
    bool   cam_rumble = false;
    float  cam_rumble_t = 0.0f;

    // Playback timeline
    double total_time = 0.0;            // loop length in seconds (0 = infinite)
    bool   loop = true;
    double seek_time = -1.0;            // >= 0 → jump playback to this time
    std::string status;                 // last status message

    void* lua_ctx = nullptr;            // opaque SCL Lua AI host (scene_lua.cpp)
    void* terrain_ctx = nullptr;        // opaque terrain heightfield (scene_terrain.cpp)
    sg::GameWorld game_world;           // static colliders + terrain + walls
    sg::HeroStats hero_stats;           // real hero physics (from hiro.scl)
    bool  hero_stats_from_scl_ok = false;

    // ── Remastered subsystems (scene_entity.h runtime) ──
    // The full entity manager: spawns every monster/hero from the scene and
    // runs the AI loop + PhysicsObjectState physics each tick. The gamestate
    // (level / XP / HP / coins / items) is the CharacterState port and is
    // updated by kills (stomps) and monster contact.
    av::EntityManager entities;
    av::GameState     game_state;
    bool  show_walls = false;           // draw the visible collision walls

};

// ── lifecycle ────────────────────────────────────────────────────────────
/// (Re)build play state from a scene. Called when entering a mode or after
/// the scene was edited. Returns false when the scene is empty.
bool player_begin(Player& p, const av::SceneData& scene, Mode mode,
                  const std::string& scene_dir);

/// Publish current keyboard state (left/right/jump + press edges for
/// double-tap-to-run detection) — called by the editor frame loop each frame.
void sp_poll_keys(bool left, bool right, bool jump,
                  bool left_pressed, bool right_pressed);

/// Trigger the game-style camera rumble (Caver::CameraController::rumble).
void player_rumble(Player& p, float duration = 0.35f);

/// Stop playback and clear state.
void player_end(Player& p);

// ── per-frame ────────────────────────────────────────────────────────────
/// Advance the engine by dt seconds. Reads/writes scene object transforms
/// (positions, rotation) — call after the editor's own edits so the player
/// sees them. If apply_to_scene is true, writes motion back into the scene
/// objects (positions + animation frames) so the visualizer shows it.
void player_tick(Player& p, av::SceneData& scene, float dt, bool apply_to_scene);

/// Apply the current Hiro camera (game-style follow) into an editor camera.
void player_apply_camera(const Player& p, av::Camera& cam);

/// Draw the small playback overlay (mode, clock, controls) — ImGui.
/// Draw the scene-player controls INSIDE the current ImGui window (the
/// right-hand inspector panel while playing). No window frame is created —
/// the caller owns the panel. Includes pause/speed/timeline/loop/Hiro status.
void player_draw_panel(Player& p);

/// Draw the visible collision walls over the 3D viewport (called inside the
/// active 3D block, after rendering). Uses av::render_lines with per-class
/// colors: solid=white, ground=green, unsafe=red, one-way=blue.
void player_draw_walls(const Player& p);

// ── helpers ──────────────────────────────────────────────────────────────
/// True when the object is a real animated entity (has a *MonsterController,
/// Char*/HeroEntity component). Doors, chests, switches and other props have
/// only DoorController/BushController/etc + AnimationController and must be
/// EXCLUDED — they hold their closed frame 0 instead of looping.
bool is_animated_entity(const av::SceneObject& o);

/// Find the scene's SpawnPoint object index, or -1.
int player_find_spawn(const av::SceneData& scene);

/// Human-readable mode name.
const char* player_mode_name(Mode m);

} // namespace sp

#endif // SCENE_PLAYER_H_
