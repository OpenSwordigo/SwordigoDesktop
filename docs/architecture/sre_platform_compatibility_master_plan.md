# SRE Platform Compatibility Audit & Engineering Master Plan
**Philosophy**: Move from "Forced Mod Compatibility" to "Accurate Platform Compatibility"

## Executive Philosophy
Two major edge-case mod scenarios expose flaws in SRE:
1. **Manson Mod**: Guest execution eventually becomes corrupted and crashes with an ARM64 `NoExecuteFault` involving repeated pattern `0x2020202020202020`.
2. **RLSwordigo Mod**: Play-scene loading freezes or fails to progress, with suspected Lua coroutine/object identity/lifecycle issues and excessive VFS/Lua allocation.

**Core Directives**:
- DO NOT fix these mods individually using `if (mod == "Manson")` or `if (mod == "RLSwordigo")`.
- DO NOT introduce mod-name or scene-specific compatibility hacks.
- DO NOT make missing objects resolve to unrelated objects (e.g., returning hero for missing `obj1`).
- Accurately implement the platform behavior expected by Swordigo mods designed around:
  - Original Swordigo / Caver Engine + SwKiwi API + Lua + mod-provided Lua libraries.

---

## 1. Important Reference Sources
- **Caver Decompiled Source**: `GhidraDecomp src`
- **SwKiwi Reference Source**: `reference/SwKiwi-main-jun9`
- **RLSwordigo Runtime Assets**: `rln_assets`

---

## 2. Audit of Existing Mod-Specific Hacks
Search and audit SRE codebase for hardcoded workarounds:
- Classify each case:
  - **A**. Correct platform emulation
  - **B**. Necessary generic compatibility behavior
  - **C**. Temporary diagnostic code
  - **D**. Mod-specific workaround (Remove)
  - **E**. Incorrect or dangerous hack (Remove)
  - **F**. Unknown (Investigate reference)

---

## 3. Manson Mod Failure Analysis & Guest Fault Containment
- **Observed Failure**: Control flow reaches invalid address `0x2020202020202020` (ASCII space `0x20` repetition).
- **Goal**: Identify the FIRST point of corruption (buffer overflow, padded sprintf, uninitialized string/memory write).
- **Guest Fault Containment**:
  - `NoExecuteFault` / `MemoryAbort` must not cascade into host `SIGSEGV`.
  - Mark guest invocation as failed, unwind cleanly, log diagnostic info, and preserve host stability.

---

## 4. RLSwordigo Engine & Coroutine Stalls

### 4.1 VFS / Lua File Object Allocations
- In `src/sre/sre_lua_libs.c`, `io_open` creating closures per file creates excessive garbage collection pressure.
- **Optimization**: Implement a shared file-handle metatable (`__sre_file_mt`) storing `FILE*` in self state while preserving Lua-visible semantics.

### 4.2 Object Identity Collision
- Multiple objects instantiated from the same template (`bc_thr`, `nt_thr`, `common_thread`) sharing the same environment key.
- Derive runtime object identity from `SceneObject*` pointer identity (`sre_object_identity(obj)`), preventing environment key collisions between live instances.

### 4.3 Coroutine & Thread Semantics
- Verify SwKiwi thread creation, coroutine resumption, and `ProgramState_Update` lifecycle without injecting fake `func`/`args`.

### 4.4 Coroutine Error Diagnostics
- In `sre_ProgramState_Update`, if `lua_resume` fails (`r != LUA_YIELD && r != 0`), log the error string, coroutine ID, associated `SceneObject` identity, and stack traceback instead of silently killing threads.

---

## 5. Dynamic Caver Engine Function Resolution & ARM64 Layouts
- Continue migrating SRE away from hardcoded guest offsets.
- Resolve required Caver symbols dynamically at startup (`sre_scene_update.c`, `sre_mini_api.c`, `sre_caver.c`).
- Eliminate ARM32 structure offset assumptions on ARM64 (`HealthComponent`, `TransformComponent`); call verified Caver ABI methods directly.
