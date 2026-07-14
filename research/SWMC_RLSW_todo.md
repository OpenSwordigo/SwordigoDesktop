# Detailed TODO — SCL Engine Hooking, VFS, & Mini/Kiwi API Compatibility

This TODO details the required architectural modifications to enable full compatibility for Swordigo Master's Curse (SWMC) and RLSwordigo (RLSW) on SwordigoDesktop.

---

## Part 1: SCL Engine Hooking & Generic Loader
To stabilize script execution across all mod formats and prevent JIT exceptions, we must redirect the script compiler and executor systems in the native engine (`libswordigo.so`) to SRE, and refactor the SCL payload extractor.

### 1.1 Hook the Core SCL Loading Functions
Hook the following engine routines to ensure they run on SRE's unified Lua state and avoid ABI mismatches:
- **`Caver::Program::InitWithString`** (offset `0x551202`):
  - *Description*: Compiles raw Lua strings into bytecode via a temporary state and `lua_dump`.
  - *Action*: Intercept to compile bytecode entirely within SRE's Lua library context.
- **`Caver::Program::LoadIntoState`** (offset `0x551985`):
  - *Description*: Reads binary bytecode from Protobuf fields and loads it via `luaL_loadbuffer`.
  - *Action*: Intercept to feed the extracted bytecode buffer directly to SRE's `luaL_loadbuffer`.
- **`Caver::Scene::NewProgramStateForProgram`** (offset `0x552880`):
  - *Description*: Creates child coroutines and anchors them in the global registry table.
  - *Action*: Intercept to register the child states correctly in SRE's scheduler.
- **`Caver::ProgramState::Update`** (offset `0x5519ea` / `_ZN5Caver12ProgramState6UpdateEf`):
  - *Description*: Resumes yielded coroutines after timers drop below zero and cleans up finished threads.
  - *Action*: Intercept to run the update scheduler within SRE, maintaining the thread chain.

### 1.2 Refactor SCL Protobuf Parser (`sre_scl_extract_lua`)
Remove the rigid nesting-dependent `field == 5` assumption in `sre_scl_extract_lua` (located in `src/sre/sre_lua_libs.c`). Replace it with a generic wire-2 payload scanner:
- Scan the binary buffer as a series of protobuf tag/value pairs.
- When encountering length-delimited payloads (`wire == 2`):
  - Check if the payload starts with the precompiled Lua signature: `\033Lua` (`0x1B 0x4C 0x75 0x61`).
  - Check if the payload is raw text starting with common Lua keywords (e.g. `function`, `local`, `end`).
  - Extract the matching boundary dynamically and return it as the Lua chunk.

---

## Part 2: Missing Lua C APIs & Typo Correction
Add the remaining dynamic symbol mappings for Lua functions used by the engine so they route to SRE's library.

### 2.1 Correct the `lua_setmetatable` Mangling Typo
- Locate the `lua_ext_syms` mapping array in `src/main.cpp`.
- Change:
  ```cpp
  {"lua_setmetatable", "_Z17lua_setmetatableP9lua_Statei"}
  ```
  to:
  ```cpp
  {"lua_setmetatable", "_Z16lua_setmetatableP9lua_Statei"}
  ```

### 2.2 Register Missing Lua APIs in the Hook Table
Add the following functions to `sym_hooks` in `src/main.cpp` and `sre_hook_table` in `src/sre/sre_init.c`:
- `lua_close` (`_Z9lua_closeP9lua_State`)
- `lua_dump` (`_Z8lua_dumpP9lua_StatePFiS0_PKvmPvES3_`)
- `lua_atpanic` (`_Z11lua_atpanicP9lua_StatePFiS0_E`)
- `lua_getmetatable` (`_Z16lua_getmetatableP9lua_Statei`)
- `lua_rawequal` (`_Z12lua_rawequalP9lua_Stateii`)
- `lua_equal` (`_Z9lua_equalP9lua_Stateii`)
- `lua_lessthan` (`_Z12lua_lessthanP9lua_Stateii`)
- `lua_isuserdata` (`_Z14lua_isuserdataP9lua_Statei`)

---

## Part 3: Mini/Kiwi API PC Compatibility Layer
SWMC and other advanced mods depend on libmini features. Implement and map the following sub-modules in `libsre.so` to provide full PC compatibility:

### 3.1 `Mini.Character` Sub-APIs
Implement compatibility mappings referring to `SwKiwi`'s `recreate_hero.c` and `set_speed.c`:
- `Mini.Character.RecreateHero()`: Reinitializes the player's entity state without reloading the level.
- Speed multipliers handling: Sync with the player component state values dynamically.

### 3.2 `Mini.Models` (Color/RGBA Tinting)
Implement RGBA color overrides referring to `SwKiwi`'s `weapon_color.c` and `models.c`:
- Intercept color setters in `Caver::ModelComponent` so custom color overrides (such as making the Fire Boss green) function properly on GLES2 renderers.

### 3.3 `Mini.Controls` (PC Input Mapping)
Expose keyboard-to-touch layout mappings referring to `SwKiwi`'s `controls.c`:
- Bind keyboard inputs and mouse clicks to touch coordinates dynamically, allowing customized HUD overlays to work with PC keybinds.

---

## Part 4: Filesystem Namespaces
- Isolate virtual directories in SRE's virtual filesystem (VFS) to separate Android's `/Files/`, `/ExternalFiles/`, `/Cache/`, and `/ExternalCache/` into distinct directories on disk (e.g. `<instance_dir>/files/`, `<instance_dir>/ext_files/`, `<instance_dir>/cache/`).
- Prevent mod data directory collisions and preserve mod-specific save games/configurations.

REFERENCE
/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/reference/SwKiwi-main-jun9/SwKiwi-main
and 
/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoTools/GhidraDecomp