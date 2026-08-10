#pragma once
/* scene_lua.h — SCL Lua AI host for the Ruby Scene Player.
 *
 * Swordigo stores the REAL entity AI as plaintext Lua programs inside the
 * .scl/.scene protobuf data (Program components — the exact scripts the
 * shipped game executes in its embedded Lua VM). This module hosts those
 * scripts in a Lua 5.1 state and drives the player's PlayObjects with the
 * Caver Lua API surface the scripts expect:
 *
 *   Program.*                 (Wait / Print / SetKeepActive / Execute)
 *   EntityController.*        (Target / IsIdle / PerformAction /
 *                              SetFacingDirection / SetMoveDirection /
 *                              SetMoveSpeed / SetMovementBehavior /
 *                              SetAcceleration / SetMoveAnimation /
 *                              DefaultMoveSpeed / ...)
 *   Entity.*                  (SetPhysicsEnabled / IsOnGround / ...)
 *   PhysicsObject.*           (SetGravityDirection / SetGravityMagnitude /
 *                              SetDecelerationForce)
 *   KeyframeAnimation.*       (SetCurrentTime / SetRunning / TimeToCompletion /
 *                              TimeToFrame)
 *   AnimationController.BlendToAnimation
 *   CollisionShape.SetEnabled
 *   Math.*  SoundLibrary.PlayEffect  Vector3.New  DirectionToTargetFromPosition
 *   self:position/setPosition/setVelocity/identifier  (hero proxy: "hero")
 *
 * Only *keep-active* programs (AI loops with `while true … Program.Wait`)
 * are auto-started at play. One-shot event programs (OnHurt / OnKill /
 * OnAttack / OnCollide …) are stored for Program.Execute() and never run
 * on their own, so monsters don't play hurt/attack anims at spawn.
 *
 * The host is stored opaquely in Player::lua_ctx; everything else stays in
 * scene_player.{h,cpp} and the visualizer consumes PlayObject state.
 */
#ifndef SCENE_LUA_H_
#define SCENE_LUA_H_

#include "scene_player.h"

namespace sl {

/// Boot the Lua AI host for a player (idempotent; no-op when the scene has
/// no keep-active AI programs). Marks the owning PlayObjects lua_driven,
/// extracts program sources and starts one coroutine per AI script.
void lua_begin(sp::Player& p, const av::SceneData& scene);

/// Advance the VM: resume runnable coroutines, integrate velocity/gravity
/// physics for Lua-driven objects, apply "follow"/"patrol"/"freeze"
/// behaviors and write motion back into the scene objects.
void lua_tick(sp::Player& p, av::SceneData& scene, float dt);

/// Shut the VM down and free the state.
void lua_end(sp::Player& p);

/// True when the host is running scripts (at least one object is Lua-driven).
bool lua_active(const sp::Player& p);

} // namespace sl

#endif // SCENE_LUA_H_
