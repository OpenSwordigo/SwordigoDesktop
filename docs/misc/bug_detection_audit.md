# SRE ARM64 1.4.12 Codebase Bug Detection & Compatibility Audit

This document compiles the findings of a comprehensive code audit of the Swordigo Runtime Engine (SRE), the launcher emulator (`swordigo_boot`), and the guest thunk library (`libsre.so`) against the original `arm64` version 1.4.12 decompiled Caver engine.

---

## 1. Hardcoded Developer Filesystem Paths

Several source code files contain absolute, hardcoded file paths referencing the developer's home directory (`/home/quantumcreeper/`), which will trigger file-write failures, compilation issues, or runtime crashes when executed on any other machine.

### Key References
1. **`src/jni/jni_bridge_arm64.cpp` (Line 3430):**
   ```cpp
   std::ofstream log_file("/home/quantumcreeper/.local/share/swordigo-desktop/file_debug.log", std::ios::app);
   ```
   *Impact:* Fails silently or throws file access exceptions if `/home/quantumcreeper` does not exist or lacks write permissions.

2. **`src/main.cpp` (Lines 2732, 3277):**
   ```cpp
   remove("/home/quantumcreeper/SwordigoDesktop/sre_hook_debug.txt");
   ...
   FILE* f = fopen("/home/quantumcreeper/SwordigoDesktop/sre_hook_debug.txt", "a");
   ```
   *Impact:* Host launcher will attempt to create files in a non-existent directory during thunk hooking, causing hook diagnostics to fail.

3. **`src/sre/sre_init.c` (Lines 409, 420):**
   ```c
   FILE* f_diag = fopen("/home/quantumcreeper/SwordigoDesktop/sre_scan_diagnostic.txt", "w");
   ```
   *Impact:* Guest-side library will crash or fail during initialization when scanning for memory structures.

### Recommended Fix:
Dynamically resolve paths using the resolved `get_user_data_dir()` / `get_user_data_dir_c()` paths, or fall back to standard local folders (e.g. `./sre_hook_debug.txt` or `tmp`).

---

## 2. Memory Leaks and Resource Tracking Failures

### 2.1 Original TVPG Allocator Recycle Mismatch
In `src/jni/jni_bridge_arm64.cpp` (inside `host_malloc_locked` and `host_free_locked`):
When `g_advanced_redstell_opts = false` (the default launcher state), heap recycling is governed by:
```cpp
if (g_guest_heap_ptr - 0x20000000 > 2048ULL * 1024 * 1024) {
```
*Impact:* The allocator acts as a raw bump allocator and refuses to recycle any memory until the guest heap grows past **2 GB**. On a 32-bit-aligned space or limited host systems, this leads to immediate memory exhaustion, virtual address space fragmentation, and host RSS bloat of up to 1.8GB–3.2GB.
*Inefficient Search:* Even when the 2GB threshold is breached, the allocator only scans the oldest 50 blocks chronologically. If the oldest blocks are not ready or mismatch in size, it aborts the search entirely and continues allocating new virtual pages.

### 2.2 Unmanaged Cleanups in `RedstellGC::shutdown`
In `src/platform/rgc.cpp`, the destructor/shutdown lifecycle method `RedstellGC::shutdown()` terminates worker threads but performs **no cleanup** of the tracked file handles (`m_file_handles`), OpenGL textures, or OpenAL sound buffers.
*Impact:* If the host launcher is ever extended to support in-process guest module reloading or soft resets without exiting the process, it will leak dozens of file descriptors and GPU memory.

### 2.3 Integer Overflow recursion in `sre_itoa`
In `src/sre/sre_lua.c`:
```c
static int sre_itoa(int val, char* buf) {
    if (val < 0) { buf[0] = '-'; return 1 + sre_itoa(-val, buf + 1); }
    ...
}
```
*Impact:* If `val` is `INT_MIN` (`-2147483648`), calling `-val` results in undefined behavior. On typical compilers, `-INT_MIN` overflows back to `INT_MIN`, causing infinite recursive thunk calls, crashing guest execution via stack overflow.

---

## 3. Structural Offset Mismatches (ARM32 vs. ARM64)

The guest library `libsre.so` has several hardcoded offsets that map to the older 32-bit (ARM32/Thumb) structures of Swordigo, resulting in critical memory corruption when run against the 64-bit 1.4.12 engine.

### 3.1 `HealthComponent` Offset Corruption
In `src/sre/sre_caver.h` (Lines 360–374):
```c
/* hp = HealthComponent + 0x1c (float)
 * maxHp = HealthComponent + 0x20 (float) */
```
*Ghidra Decompilation reality:* 
In the 1.4.12 ARM64 binary, structures have grown due to 8-byte pointer alignments. The `HealthComponent` constructor and property binders clearly locate fields as:
* `CurrentHealth`: `HealthComponent + 0x7c` (32-bit integer)
* `MaxHealth`: `HealthComponent + 0x88` (32-bit integer)
* `CurrentDamage`: `HealthComponent + 0x8c` (float)
* `MaxDamage`: `HealthComponent + 0x90` (float)

*Impact:* Reading or writing to `0x1c` or `0x20` on a 64-bit `HealthComponent` accesses function pointers or invalid fields, causing Lua getters (`caver.getHp`) to return garbage float values and writers (`caver.setHp`) to corrupt memory.

### 3.2 `TransformComponent` Offset Corruption (Crash Source)
In `src/sre/sre_caver.h` (Lines 380–391):
```c
/* position: TransformComponent + 0x10 (Vec3 = 3 floats) */
```
*Ghidra Decompilation reality:*
In the ARM64 `TransformComponent` struct, offsets `0x10` and `0x18` hold vtable function pointers (`UpdateBindingDependencies` and `UpdateWhenPaused`). 
The local coordinate translation is not stored directly at `+0x10`. Instead:
1. `TransformComponent` stores a pointer to its parent `SceneObject` at offset `0x28`.
2. The absolute translation is stored directly inside the `SceneObject` at `SceneObject + 0x70` (X, Y) and `SceneObject + 0x78` (Z) as floats.
3. The local translation is stored as part of a 4x4 matrix starting at `TransformComponent + 0x70`.

*Impact:* Invoking `caver.setPosition()` writes float coordinates directly into the virtual function pointer slots at `tc + 0x10` / `tc + 0x18`. The next time the engine attempts to update the scene, it branches to the corrupted float values, causing an immediate segmentation fault (crash).

---

## 4. SwMini API / SRE Mini Compatibility Gaps

To achieve 100% compatibility with SwMini mods (such as *Swordigo Master's Curse*), SRE's emulation layer has several unimplemented APIs and hardcoded simplifications.

### 4.1 Missing Component Helpers
In `src/sre/sre_mini_api.c`:
The registered global tables for components (`Components.Health`, `Components.Physics`, `Components.Entity`) lack:
* `IsPresent(entity)`
* `GetValues(entity)`
* `SetValues(entity, value_table)`

*Impact:* Mods that query component existence generically via `Components.Health.IsPresent(entity)` immediately throw nil method execution errors and abort.

### 4.2 Non-Generic Component Accessors
`Components.Health.GetValue` and `SetValue` are implemented as:
```c
if (sre_streq(field, "CurrentHealth")) {
    g_lua_pushnumber(L, (double)g_sre_player_hp);
    return 1;
}
```
*Impact:* The functions completely ignore the `entity` parameter (the first argument). If a mod attempts to check the health of a custom boss or skelly monster, it receives the player's current health instead.

### 4.3 `SetWeaponColor` Interceptor Mismatch
In `src/sre/sre_mini_api.c` (Line 1876):
```c
static int l_mini_set_weapon_color(lua_State* L) {
    const char* item_id = lua_tostring(L, 1);
    if (!item_id) return 0;
    ...
}
```
*Ghidra Decompilation & Mod usage reality:*
Mods call `SetWeaponColor` in two ways:
1. Passing a string (e.g. `"sword_fire"`) to apply a global trinket tint.
2. Passing a `SceneObject` userdata pointer (e.g., the active weapon controller instance).

*Impact:* If called with a userdata pointer, `lua_tostring` returns `NULL` and the function returns early. Weapon coloring/glow updates fail for all dynamically summoned weapons.
*Required Fix:* If argument 1 is userdata:
1. Cast to `SceneObject`.
2. Extract the `SwingableWeaponControllerComponent` component.
3. Read the `SwingableWeaponComponent` pointer at offset `0x98` inside the controller.
4. Call resolved `SetGlowColor` and `SetGlowIntensity` thunks on it.

### 4.4 Missing Host-Side Thunks
`SwingableWeaponComponent::SetGlowColor` and `SetGlowIntensity` are not resolved in host `src/main.cpp` and passed to `libsre.so`. They must be resolved via:
* `_ZN5Caver24SwingableWeaponComponent12SetGlowColorENS_10FloatColorE`
* `_ZN5Caver24SwingableWeaponComponent16SetGlowIntensityEf`

---

## 5. VFS and Save Path Conflation

In `src/platform/data_path.cpp` and `src/sre/sre_vfs.c`:
Virtual directories like `/Files/`, `/ExternalFiles/`, and `/Cache/` map directly to combined directories or root save folders without proper isolation under mod scenarios.
*Impact:* Save data and configuration overrides from one mod conflict with or corrupt the configuration files of another mod if they write to the same virtual paths. Distinct separation (e.g. `mods/<mod_name>/files/`) is required.
