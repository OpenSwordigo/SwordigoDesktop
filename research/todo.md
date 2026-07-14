# Swordigo Compatibility, Performance, and Memory Optimization Plan (ARM64)

This plan details technical solutions to resolve memory leaks, frame rate drops, missing Lua APIs, rigid script parsing, and custom mod-compatibility issues on the ARM64 desktop port of Swordigo.

## Proposed Changes

We will modify both the host-side launcher/emulator (`src/main.cpp`, `src/platform/data_path.cpp`, etc.) and the guest-side library (`src/sre/*`) to implement these improvements.

---

### Component: Guest-Side Engine Hooks & APIs (`libsre.so`)

#### [MODIFY] [sre_lua.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_lua.c)
- **Performance Optimization**: Remove the expensive `sre_setjmp` and `recovery_push`/`recovery_pop` overhead in `sre_ProgramState_Update` when calling `g_orig_ProgramState_Update`. Since the original `Update` is called with `isSuspended = 0`, it does not trigger any coroutine resumes directly, making the error-recovery wrap redundant. Removing it eliminates a massive per-frame CPU registers save/restore overhead.

#### [MODIFY] [sre_lua_libs.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_lua_libs.c)
- **Robust SCL Protobuf Parser**: Refactor `sre_scl_extract_lua` to use a schema-independent, recursive wire-2 protobuf scanner. Instead of looking only at `field == 5`, it will traverse all length-delimited payloads, search for the precompiled Lua signature (`\033Lua`), and extract the bytecode, falling back to raw text if no bytecode is present.
- **Unpack New Lua Pointers**: Update `SreLuaExtAddrs` struct and `sre_init_lua_ext()` to receive and initialize the new dynamic function pointers (`lua_close`, `lua_dump`, etc.).

#### [MODIFY] [sre_lua.h](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_lua.h) & [sre_lua_compat.h](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_lua_compat.h)
- Declare `typedef`s, `extern` globals, and `#define` macros to route the missing Lua APIs (`lua_close`, `lua_dump`, `lua_atpanic`, `lua_getmetatable`, `lua_rawequal`, `lua_equal`, `lua_lessthan`, `lua_isuserdata`) to SRE's library.

#### [MODIFY] [sre_init.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_init.c)
- Add the missing Lua APIs to the `sre_hook_table` so the host redirects all thunks to SRE's unified Lua state, resolving the mismatch during `ProgramState` destruction.

#### [MODIFY] [sre_mini_api.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_mini_api.c)
- **Global Table Alias**: Register the global `Component` table as an alias of `Components` (or vice-versa) to support libmini naming.
- **Direct Component Field Access**: Rewrite `Components.Health`, `Components.Physics`, and `Components.Entity` read/write handlers to resolve the actual component pointer via `g_SceneObject_ComponentWithInterface` and read/write offsets directly on guest memory rather than using global variables.
- **Component helper functions**: Implement `IsPresent(obj)`, `GetValues(obj)`, and `SetValues(obj, val_table)` generically.
- **SetWeaponColor Interceptor**: Implement `l_mini_set_weapon_color` to distinguish between string (trinket) and userdata (`SceneObject`). For userdata, fetch `SwingableWeaponControllerComponent` -> `SwingableWeaponComponent` (at offset `0x98`), and call `SetGlowColor` and `SetGlowIntensity` via the dynamic function pointers.

#### [MODIFY] [sre_caver.h](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.h) & [sre_caver.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.c)
- Declare `g_SwingableWeapon_SetGlowColor` and `g_SwingableWeapon_SetGlowIntensity` function pointers.

#### [MODIFY] [sre_vfs.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_vfs.c)
- **VFS Mod Isolation**: Rewrite `sre_vfs_init_full` to check if `mod_name` is non-empty, and rewrite `/Files/`, `/ExternalFiles/`, and `/Cache/` into mod-isolated subfolders: `mods/<mod_name>/files/`, `mods/<mod_name>/ext_files/`, and `mods/<mod_name>/cache/`.

---

### Component: Host-Side Launcher & Emulator (`swordigo_boot`)

#### [MODIFY] [jni_bridge_arm64.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/jni/jni_bridge_arm64.cpp)
- **Memory Leak Fix**: Change the recycling threshold check in `host_malloc_locked` from `2048ULL * 1024 * 1024` (2 GB) to `16ULL * 1024 * 1024` (16 MB). This stops memory virtual address bloat and RSS growth, resolving the 1.8GB / 3.2GB leaks.

#### [MODIFY] [main.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/main.cpp)
- **Fix Symbol Typo**: Correct mangled name of `lua_setmetatable` from `_Z17` to `_Z16` in `sym_hooks` and `lua_ext_syms` tables.
- **Register Missing Lua thunks**: Add `lua_close`, `lua_dump`, `lua_atpanic`, `lua_getmetatable`, `lua_rawequal`, `lua_equal`, `lua_lessthan`, and `lua_isuserdata` to `sym_hooks` and `lua_ext_syms`.
- **Dynamic C++ Methods Resolution**: Resolve `SwingableWeaponComponent::SetGlowColor` (`_ZN5Caver24SwingableWeaponComponent12SetGlowColorENS_10FloatColorE`) and `SwingableWeaponComponent::SetGlowIntensity` (`_ZN5Caver24SwingableWeaponComponent16SetGlowIntensityEf`) and write their guest addresses to `g_SwingableWeapon_SetGlowColor` and `g_SwingableWeapon_SetGlowIntensity` inside `libsre.so`.

#### [MODIFY] [data_path.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/platform/data_path.cpp)
- **Isolate VFS Path Resolver**: In `resolve_vfs_path`, check if `g_active_mod_name` is non-empty, and map `/Files/`, `/ExternalFiles/`, and `/Cache/` to the corresponding mod-isolated folders, matching the guest VFS logic.

---

## Verification Plan

### Automated Tests
- Run `make clean && make -j$(nproc) DYNARMIC=1` and `make libsre.so` to ensure compile-time correctness and cross-compilation success.
- Run `swordigo_boot` to launch vanilla and modded games.

### Manual Verification
- Observe Resident Set Size (RSS) memory usage on the host to verify the memory leak is fixed.
- Check scene loading speed and HUD components.
- Verify custom mod dialogs, tint overrides (e.g., green Fire Boss), and custom items/keybinds function correctly.
