Mini* API Implementation & Integration Plan

Date: 2026-06-27
Author: on behalf of agent

Summary

Goal: provide a minimal, safe, and incremental implementation of the SwKiwi/SwMini "Mini.*" Lua APIs and related helper APIs (LNI, Components, Character, ButtonController, fs, Game/Health) inside libsre so existing mods keep working without destabilizing the engine.

Constraints & principles

- Non-invasive: avoid rewriting guest upvalues or hooking deep internals that change semantics.
- Idempotent & late-binding: do not overwrite native tables if they already exist; inject only when safe.
- Defensive defaults: return safe values (false/0/empty tables/strings) rather than nil so many mod init scripts do not error on pairs() or arithmetic.
- Minimal host/guest ABI: expose small set of shared globals (g_sre_*) for host polling when host-side action is required.
- Opt-in diagnostics and feature gates: quiet_mode and environment toggles for verbose logs.

High-level approach

1) Discovery & contract mapping
   - Inspect reference/SwKiwi (Java + Lua) to enumerate expected Mini.*, ButtonController and LNI behaviors.
   - Use old/src as baseline for exact names and behaviors used by core scripts (rlsw.scl, code.scl, mason.scl).

2) Prioritize small, high-impact APIs (fast wins)
   - ButtonController guest-side array & Lua helpers:
     - g_sre_buttons[] (struct): id, x,y,w,h,pressed,dragging,active,visible,dirty
     - Lua functions: ButtonController.Add(id,x,y,w,h), ButtonController.IsPressed(id), ButtonController.SetText(id,s), ButtonController.Remove(id)
     - Host polls g_sre_buttons and may render/dispatch; keep host code optional and disabled until contract stable.
   - fs helpers:
     - fs.exists(path), fs.read(path) -> string or empty table, fs.write(path, content)
     - Implement translation of Mini asset paths (minipath -> VFS path)
   - Basic Mini.Game and Mini.Health getters/setters used in many scripts (coin/xp/hp)

3) Implement defensive stubs for wider surface
   - Mini.Character / Components / ItemDrop: small set of functions returning safe defaults and recording requested deferred actions to g_sre_* for host-side processing (SetHP, Die, Swing, etc.).
   - LNI.* helpers: simple string command buffer for host to read and execute non-critical actions.
   - RegisterTreasure, HasItem, HasFlag, HasSceneFlag — implement as lua-callable functions returning safe values unless overridden by game/native registration.

4) Injection & registration strategy
   - Lazy injection only: perform injection when a Lua state is about to execute mod code (e.g., inside ProgramState::Execute/Resume, when safe and after engine registration).
   - Detection: if global Mini (or target table) exists and appears native, skip injection. If absent or appears truncated, install shims.
   - Use minimal footprint: register only missing functions/tables rather than wholesale replacing.
   - Record every injection in an internal trace (sre_injection.log) with lua_State pointer for debugging, but respect quiet_mode.

5) Host/Guest contract & synchronization
   - Shared globals in C (volatile) for host to poll/act on: g_sre_buttons[], g_sre_char_action_pending, g_sre_lni_command, g_sre_game_action_pending.
   - Design these as write-by-guest (lua) and read-by-host; host writes back status bits (e.g., released/handled) only when necessary.
   - Avoid concurrent-memory hazards: use simple integers/flags with clear transitions (0->1->0) and small time-to-live semantics.

6) Testing & validation
   - Automated headless runs (1000 frames) with minimal mods loaded; watch sre_lua_errors.log and host logs.
   - Regression tests: compare frame counts, draw calls, and key logs against baseline (old/src build before changes).
   - Manual checks: enable host ButtonController rendering only when guest contract stabilized; test common mods that use buttons (SwKiwi overlay, mods that add UI buttons).

Implementation details (examples)

- Button slot struct (guest-visible)
  struct SreBtnSlot { char id[32]; int x,y,w,h; volatile int pressed; volatile int released; volatile int dragging; int active; int dirty; char text[128]; };
  Expose sre_find_btn(id) in C and Lua wrappers.

- fs.read: return string when non-binary small files, otherwise return empty table to make pairs() safe in init scripts that call pairs(fs.read(...))

- Injection guard (pseudocode):
  void safer_inject(lua_State* L) {
    if (state_seen(L)) return;
    if (global_exists(L, "Mini") && looks_native(L, "Mini")) return; // leave native
    push_Mini_table(L);
    register_missing_functions(L);
    mark_state_injected(L);
  }

- Registration policy: always register functions under Mini if missing; never overwrite existing function pointers.

Rollout plan (phased)

Phase 0 — Research & plan (this doc).
Phase 1 — Implement small core (ButtonController guest-side, fs helpers, pref stubs). Build & test.
Phase 2 — Add Character/Health/Game getters/setters + LNI buffer. Test with a few mods.
Phase 3 — Add optional host-side ButtonController rendering/hit-testing behind feature gate; re-enable only when contract stable.
Phase 4 — Iterate on coverage: add any missing functions reported in sre_lua_errors.log.

Risks & mitigations

- Overwriting native tables too early — mitigate: lazy injection + native detection.
- Stubs that cause mods to take unsafe code paths — mitigate: return conservative defaults and log when a stubbed call is exercised frequently so it can be implemented properly.
- Performance impact from logging — respect quiet_mode and limit logs; provide diagnostics toggle.
- Concurrency/memory hazards between host/guest — keep synchronization protocol tiny (0/1 flags), no complex shared structures.

Next actionable tasks (pick order)

1. Implement ButtonController guest-side and fs.exists/read/write shims (high value).  (code)
2. Add lazy injection guard and state tracking (code)
3. Run diff between old/src and current src to list missing native API functions and implement the most-hit stubs from sre_lua_errors.log (analysis+code)
4. Add automated headless smoke test harness that loads a representative mod set and records sre_lua_errors.log and frame metrics (test)
5. Revisit host-side ButtonController rendering behind a feature flag once guest contract stable (code+manual test)

Appendix: common failure modes observed in logs

- "attempt to index global 'Character' (a nil value)" — Character table is missing; provide Character stub with commonly used fields/functions.
- "attempt to perform arithmetic on a nil value" — scripts expecting numeric return (e.g., GetX()) but receiving nil; return 0 instead.
- "bad argument #1 to 'pairs' (table expected, got nil)" — return empty table where appropriate (fs.read init path).

Contact & follow-ups

- I can implement Phase 1 (ButtonController + fs shims) and push a PR in this repo. Confirm and I will: (a) implement, (b) build libsre.so, (c) run short headless test, (d) report results and next-delta.



