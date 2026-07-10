# Swordigo Mod Loader Research Report

> [!IMPORTANT]
> **Correction (2026-07-09):** SwKiwi targets **ARM64 (aarch64)** — the same 64-bit architecture as SRE. The `$` macro in `hooks.h` takes two offsets `(b32, b64)` and selects based on the compile target; on aarch64 it always uses the second (`b64`) argument.

This document details the research findings on the Swordigo Kiwi (**SwKiwi**) mod loader, how it relates to the PC port's compatibility layer (**libsre**), how various file formats are parsed, and the weird/broken implementations identified in `libsre`.

---

## 1. How the Mod Loader Works

### Android Mod Loader (SwKiwi / SwMini)
SwKiwi operates as a dynamic hooking and lifecycle wrapper on Android:
1. **Java Host Replacement**: Replaces vanilla Java bootstrap classes (`MainActivity`, `LibraryManager`) to handle asset paths, virtual directories, and preload dependencies (like OpenAL and the native engine library `libswordigo.so`).
2. **C++ Companion Hooking (`libmini.so`)**:
   - Uses **GlossHook** to intercept and hook functions inside `libswordigo.so` at runtime.
   - Intercepts file lookups via `Caver::FileExistsAtPath` and `Caver::NewByteBufferFromAndroidAsset` to redirect asset calls to a **Virtual Filesystem (MiniPaths)** structure.
   - Overrides `luaL_loadfile` to inject custom path resolution.
3. **Lua Environment Injection**:
   - Injects custom Lua modules (`Mini`, `LNI`, `package.preload` libraries like `lfs`, `socket.core`, `mime.core`, `toml`) into every newly initialized `lua_State` during `ProgramState::RegisterLibrary` or `RegisterProgramLibrary`.

### PC Port Mod Loader (`libsre`)
The PC port runs `libsre.so` **inside the same ARM64 guest address space** as `libswordigo.so`, injected at runtime by the host emulator (Unicorn/Dynarmic). It uses the same ARM64 struct offsets as SwKiwi:
1. **Trampoline Hooking**: The host reads the `sre_hook_table` in [sre_init.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_init.c) and patches hooks (using trampolines) in the emulated guest memory.
2. **VFS Translation**: Overrides file-existence and file-read checks, routing them to standard filesystem calls mapped to the host's `.local/share/swordigo-desktop` directories.
3. **Lua Engine Injection**: Hooks `lua_call` and error handling (via custom `setjmp/longjmp` trampolines in `sre_lua_call_safe` to bypass broken C++ exception unwinding in Unicorn) and dynamically injects stubs for `Mini.*` tables.

---

## 2. How the Mod Loader Parses `mini.toml`

### SwKiwi / SwMini (Android)
- **Java Side**: [ModProperties.java](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/reference/SwKiwi-main_jul8/SwKiwi-main/app/src/main/java/net/itsjustsomedude/swrdg/ModProperties.java) parses `mini.toml` using `io.github.wasabithumb.jtoml` to read title-screen properties (mismatching links, visible options, overlay name, version, readme).
- **C++ Side**: [mini_config.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/reference/SwKiwi-main_jul8/SwKiwi-main/app/src/main/cpp/config/mini_config.c) uses `toml.c` to parse options from `mini.toml` (specifically `coin_limit` and `too_rich_threshold` under the `[options]` table, custom models mapping under `[armor_models]`, and multipliers under `[armor_attributes]`).

### SwKiwi Hero Pointer Path (ARM64 — both pointer chains are correct)
SwKiwi has **two** different ways to reach the hero `SceneObject*`, both valid on 64-bit:
- **`program_state.c` path (canonical, used by SRE):** `gameController +0xc8` → `GameSceneController*`, then `+0x8` → `hero SceneObject*`
- **`functions.c` path (alternate):** `gameController +0xc8` → `GameSceneController*`, then `+0xd8` → `hero SceneObject*` (follows a different internal member but reaches the same object)

SRE's `sre_caver.h` uses the canonical `+0x8` path from `program_state.c`, which is **correct**.

> [!WARNING]
> ### Weird/Broken TOML Parsing in SRE
> In SRE's [sre_config.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_config.c), the TOML parser looks for tables named `[mod]` and `[config]`. However, SwKiwi's [mini.toml](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/reference/SwKiwi-main_jul8/SwKiwi-main/app/src/main/assets/mini.toml) defines them as `[mod_overlay]` and `[options]`!
>
> As a result:
> - `coin_limit` is read from `config` instead of `options.coin_limit`.
> - `engine_speed` is read from `config` instead of `options.engine_speed`.
> - **Result**: Default properties and options defined in `mini.toml` are completely ignored or revert to fallback values.

---

## 3. How the Mod Loader Parses `.scl` Files

- **Android Mod Loader (SwKiwi)**:
  Does **not** parse SCL files. The SCL files are protobuf-serialized data structures representing Compiled Object Libraries (`ObjectLibrary` messages) containing `Program` entries. The Android app executes alongside the original `libswordigo.so` engine which decodes them natively.
- **PC Port Mod Loader (`libsre`)**:
  Exposes VFS-aware overrides for `loadfile` and `dofile` inside [sre_lua_libs.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_lua_libs.c). It parses `.scl` files using a guest-side custom varint/protobuf decoder `sre_scl_extract_lua`.
  - It searches for wire type `2` (length-delimited field).
  - Inspects `field == 5` (`Program`).
  - Inside the program sub-message, it extracts `field == 2` (Lua source) or `field == 3` (compiled bytecode) and compiles it dynamically using Lua's standard `loadstring`.

---

## 4. Native Engine & Host/JNI Requirements

To make Kiwi features work on PC, `libsre` must emulate several native layers:
1. **Audio system**: SwKiwi uses Android JNI sound assets. `libsre` intercepts these (e.g. `PlayMusicWithName`) and relays commands to the host which executes them via **OpenAL**.
2. **Virtual Filesystem / MiniPaths**: Redirects asset/file calls.
3. **Control States**: Interface to hide buttons, toggle cinematic overlays.
4. **OpenGL/Vulkan state hooks**: Captured on host-side.
5. **JNI Layer**: Emulates Android JNI calls for Clipboard (`copyToClipboard`), Open URL, and App life cycle features.

---

## 5. Weird & Broken Implementations in `libsre`

Beyond the incorrect TOML table parsing, a critical design issue was found:

> [!IMPORTANT]
> ### Dead Global Synchronization Loop
> Many `Mini.Character` actions (like `Die`, `Swing`, `Hurt`, `CancelCasting`, `StartMovingToDirection`, etc.) and setters (`SetHP`, `SetMana`, `SetCoins`) inside SRE's [sre_mini_api.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_mini_api.c) are implemented by writing to SRE global variables (e.g. `g_sre_char_action_pending = 1`, `g_sre_char_set_pending = 1`).
>
> However, **neither libsre itself nor the host emulator codebase reads or acts upon these variables!**
>
> SRE implements the methods like so:
> ```c
> static int l_mini_char_die(lua_State* L) {
>     g_sre_char_action = SRE_CHAR_ACTION_DIE;
>     g_sre_char_action_pending = 1;
>     return 0;
> }
> ```
> But the host never processes `g_sre_char_action_pending`, making these API calls completely inert in-game.

### Why did this happen?
It appears they planned to delegate actions to the host for emulated execution, but never completed the host-side polling and dispatch loop. 

### The Solution: Direct Guest-Side C++ Invocations
Because `libsre` runs in the same guest address space as `libswordigo.so`, it has direct access to the engine's resolved component pointer structure in memory. In fact, [sre_caver.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.c) already resolves all the required component interfaces at startup:
- `CharControllerComponent_Interface`
- `HealthComponent_Interface`
- `g_SceneObject_ComponentWithInterface`

Instead of writing to dead host-polling globals, SRE can resolve the engine's C++ member function pointers dynamically and invoke them directly in guest memory, exactly as SwKiwi's [functions.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/reference/SwKiwi-main_jul8/SwKiwi-main/app/src/main/cpp/lua_libs/mini_character/functions.c) does:

```c
static int l_mini_char_die(lua_State* L) {
    SceneObject* hero = sre_hero_object_from_L(L);
    if (!hero) return 0;
    void* cc = sre_scene_object_component(hero, CharControllerComponent_Interface);
    if (cc) {
        typedef void (*pfn_Die)(void*);
        pfn_Die fn = (pfn_Die)(g_swordigo_base + 0x1ef714); // Offset of CharControllerComponent::Die
        fn(cc);
    }
    return 0;
}
```

---

## 6. Actionable Implementation Plan

We propose a two-phase plan to stabilize mod loader integrations in `libsre`:

### Phase 1: Correct `mini.toml` Parsing
Update [sre_config.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_config.c) to search for `[mod_overlay]` instead of `[mod]` and `[options]` instead of `[config]`.

### Phase 2: Refactor `Mini.Character` Action Hooks
Replace the dead host-polling variables with direct callouts to `libswordigo.so` C++ symbols. We will resolve and map the following offsets (relative to `g_swordigo_base` on ARM64 v1.4.12):

| Method | Demangled Symbol | Offset | Action |
| --- | --- | --- | --- |
| `Die` | `Caver::CharControllerComponent::Die()` | `0x1ef714` | Trigger hero death |
| `Swing` | `Caver::CharControllerComponent::Swing()` | `0x110664` | Attack / swing weapon |
| `StopSwing` | `Caver::CharControllerComponent::StopSwing()` | `0x112066` | Stop swing |
| `Use` | `Caver::CharControllerComponent::Use()` | `0x112205` | Interact / Use |
| `Hurt` | `Caver::CharControllerComponent::Hurt()` | `0x111356` | Trigger hurt animation |
| `StartJumping` | `Caver::CharControllerComponent::StartJumping()` | `0x111780` | Start jump |
| `StopJumping` | `Caver::CharControllerComponent::StopJumping()` | `0x111083` | Stop jump |
| `DropQuickly` | `Caver::CharControllerComponent::DropQuickly()` | `0x111015` | Fast drop |
| `CancelCasting` | `Caver::CharControllerComponent::CancelCasting()` | `0x113050` | Cancel spell |
| `FinishCasting` | `Caver::CharControllerComponent::FinishCasting()` | `0x112980` | Cast spell |
| `StartMovingToDirection` | `Caver::CharControllerComponent::StartMovingToDirection(int)` | `0x111709` | Move hero |
| `StopMovingToDirection` | `Caver::CharControllerComponent::StopMovingToDirection(int)` | `0x111764` | Stop hero |

*Note: Component getters (`CanJump`, `CanSwing`, `CanUse`, etc.) will also be updated to query the C++ component functions directly and return the boolean result back to Lua.*
