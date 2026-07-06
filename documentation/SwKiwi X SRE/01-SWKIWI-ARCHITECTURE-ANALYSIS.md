# SwKiwi (SwMini) Modloader - Architecture Analysis

**Target**: Understanding SwKiwi's infrastructure for PC port integration with SRE

**Author**: Research Analysis  
**Date**: July 2026  
**Status**: Research Complete - Do Not Implement Yet

---

## Executive Summary

**SwKiwi** (codebase: "SwMini") is a sophisticated reverse-engineered mod loader for Swordigo built with **~190+ files** (181 C/C++ + 22 Java). It uses a **three-layer architecture**:

1. **Java Layer** - Android system bridge, asset management, configuration parsing
2. **JNI/C Bridge** - Native interface, Lua state management
3. **C/C++ Core** - Function hooking, engine interception, feature implementation

---

## 1. THREE-PHASE INITIALIZATION FLOW

```
JNI_OnLoad()
  ↓
MainActivity.onCreate()
  ├─ LibraryManager.loadMini()        [earlyLoad]
  │  ├─ System.loadLibrary("mini")
  │  ├─ setMiniAssetManager()
  │  └─ setMiniFilePaths()
  ├─ System.loadLibrary("swordigo")   [Load vanilla engine]
  ├─ NativeBridge.midLoad()           [midLoad]
  │  ├─ Hook registration via GlossHook
  │  ├─ Core system initialization
  │  └─ File system setup
  └─ NativeBridge.lateLoad()          [lateLoad]
     ├─ Lua library registration
     └─ Feature activation
```

**Key Insight**: Three distinct phases allow proper hook installation and initialization ordering.

---

## 2. HOOK SYSTEM (GlossHook)

### Hook Types

| Type | Method | Use Case |
|------|--------|----------|
| **Symbol-Based** | `DL_HOOK_SYMBOL("_Z5SceneC1Ev")` | Named exports (preferred) |
| **Offset-Based** | `DL_HOOK_OFFSET(0x12345)` | Hardcoded addresses, undeclared functions |
| **Address-Based** | `DL_HOOK_ADDR(runtime_addr)` | Dynamic discovery |

### Hook Workflow

```c
// Header (declaration)
H_DL_FUNCTION_HOOK(Scene_Create, void, (void *Scene))

// Implementation (hook body)
DL_HOOK_SYMBOL(Scene_Create, "_ZN5Caver5SceneC1Ev", void, (void *Scene) {
    printf("Scene created!");
    orig_Scene_Create(Scene);  // Call original
    printf("Finished!");
})

// Registration (init)
hook_Scene_Create();  // Sets up GlossHook via GlossHookAddr()
```

### Function Return Values
- **orig_functionName** - Original function pointer (for calling original)
- **stub_functionName** - Trampoline stub address (set by host after installation)

---

## 3. CRITICAL SWKIWI COMPONENTS FOR PC PORT

### A. Virtual Filesystem (impl_files/)

**Purpose**: Path translation for mod resource loading

**Translations**:
| MiniPath | Real Path | RW | Purpose |
|----------|-----------|----|---------| 
| `/Assets/` | APK assets | R | Game resources |
| `/Files/` | Private storage | RW | Mod data |
| `/ExternalFiles/` | External storage | RW | User files |
| `/Cache/` | Cache directory | RW | Temporary |
| `resources/` | Search hierarchy | RW | Mod files |

**Resource Search Order** (for scenes, models, textures):
1. `/ExternalFiles/resources/<profile_id>/`
2. `/Files/resources/<profile_id>/`
3. `/ExternalFiles/resources/`
4. `/Files/resources/`
5. `/Assets/resources/` (fallback)

**PC Port Decision**: Will need SDL/filesystem equivalent, not Android paths.

### B. Lua-Native Interface (LNI System)

**Architecture**:
```
Java Method (@LuaNativeInterface)
    ↓ (Reflection discovery)
C Registration (nativeRegister)
    ↓ (Lua binding creation)
Lua Function Call
    ↓ (JNI callback)
Java Method Execution
```

**Type System** (Limited):
- `void`, `boolean`, `double`, `String` only
- Deferred registration (queue handles early Lua calls)

**Exposed Functions**:
- System: `openUrl()`, `copyToClipboard()`, `quit()`
- Speed: `getSpeed()`, `setSpeed()`
- Video: `playVideo(path, x, y)`

### C. Event System

**Scene Lifecycle**:
```c
Scene_Create() → Initialize level
    ↓
Scene_Destroy() → Cleanup level (clear buttons, bones)
```

**Profile Events** (events/profile.c/h):
- Save/load progression hooks
- Profile ID tracking

---

## 4. LUA API SURFACE (Mini.* + LNI.*)

### Mini Namespace
```lua
Mini.Arch()                    → "arm64-v8a" | "armeabi-v7a"
Mini.GetProfileID()            → UUID string of current save
Mini.SetControlsHidden(bool)   → Toggle UI visibility
Mini.RecreateHero()            → Respawn hero (re-run OnLoad)
Mini.SetCoinLimit(number)      → Set max Soul Shards (≤65535)
Mini.ExecuteLNI(funcName, ...) → Call Java function
Mini.BindLNI(funcName)         → Return callable wrapper

Mini.Health.CurrentMana()      → Get current mana
Mini.Health.CurrentManaPercent() → Get mana % filled
```

### LNI Namespace
```lua
LNI.copyToClipboard(text)  → Copy text
LNI.openUrl(url)          → Open browser
LNI.getSpeed()            → Get engine speed (1.0 = normal)
LNI.setSpeed(speed)       → Set engine speed
LNI.quit()                → Exit game

-- PascalCase aliases also available:
LNI.CopyToClipboard(text)
LNI.OpenUrl(url)
-- etc.
```

### Standard Lua Libraries (Expanded)
- `debug`, `io`, `math`, `os`, `table`
- `fs` (LuaFileSystem with MiniPath translation)
- `broken_socket` (LuaSocket C portion)

---

## 5. CONFIGURATION SYSTEM (mini.toml)

```toml
[mod_overlay]
name = "SwMini"
authors = ["ItsJustSomeDude"]
version = "1.5"
readme = "HTML with <b>, <i>, <a href> tags"

[links]
facebook = "https://..."
twitter = "https://..."

[options]
show_google_button = false
coin_limit = 999
engine_speed = 1.0
pauseOnLostFocus = true

[armor_models]
"" = "hiro"              # Default
"platearmor" = "hiro_plated"

[armor_attributes]
"platearmor" = 0.5      # 50% damage reduction
"magicarmor" = 0.25
```

---

## 6. KEY HOOKS FOR SWKIWI PORT (MUST-HAVE)

### Render Pipeline Hooks
- **Scene_Create** - Initialize level (bone maps, buttons, physics)
- **Scene_Destroy** - Cleanup (clear UI state)
- **Model_Load** - Apply armor model swapping

### Physics Hooks
- **Damage_Calculate** - Apply armor reduction
- **Collision_Check** - Weapon/enemy collisions

### Lua State Hooks
- **RegisterProgramLibrary** - Inject Mini.*/LNI.* APIs
- **lua_call/lua_pcall** - Intercept Lua calls for profiling/debugging

### Asset Loading Hooks
- **Resource_Load** - Virtual filesystem path translation
- **Texture_Load** - Model texture swapping

---

## 7. COMPARISON: SWKIWI vs SRE ARCHITECTURE

### SwKiwi (Android)
```
Java (MainActivity)
  ↓
JNI (LibraryManager → NativeBridge)
  ↓
C/C++ (GlossHook-based function interception)
  ↓
Vanilla Swordigo (libswordigo.so)
```

### SRE (PC - Currently)
```
C/C++ Host (Unicorn emulator)
  ↓
Function pointers for Lua API
  ↓
Vanilla Engine (ARM binary)
```

### Integration Points
1. **Mini.Arch()** → Return "x86_64" on PC
2. **Mini.GetProfileID()** → Use current save slot name
3. **Virtual Filesystem** → Map to PC paths (Documents/Library/etc.)
4. **LNI System** → Bridge to host C/C++ functions directly
5. **Hook System** → Use SRE's existing hooking instead of GlossHook

---

## 8. RESOURCE LOADING WORKFLOW

### Asset Hierarchy
```
Game starts
  ↓
Scene.CreateObject("scene_name")
  ↓
Engine searches resources/:
  1. /ExternalFiles/resources/<profile_id>/scene_name.scl
  2. /Files/resources/<profile_id>/scene_name.scl
  3. /ExternalFiles/resources/scene_name.scl
  4. /Files/resources/scene_name.scl
  5. /Assets/resources/scene_name.scl ← Fallback
  ↓
Scene loaded
```

### Mod Override Strategy
- Dev: Place custom files in `/Files/resources/{profile}/`
- Runtime: Engine tries custom paths first
- Fallback: Vanilla assets from `/Assets/`

---

## 9. CRITICAL INSIGHTS

### #1: Hook-Based Modding Model
SwKiwi succeeds because it hooks **at the function level**, not at the data level. This means:
- Mods can intercept ANY function call
- No need to modify binary structures
- Mods stack cleanly (multiple hooks same function)

**Implication for SRE**: Use SRE's existing hooking + add Mini API layer on top.

### #2: Virtual Filesystem as Abstraction Layer
By translating paths (MiniPath → real filesystem), SwKiwi allows:
- Seamless mod loading without code changes
- Per-profile mod directories
- Dev/release workflow without recompilation

**Implication for SRE**: Implement MiniPath translation for PC filesystem.

### #3: Deferred LNI Registration
Functions can be called from Lua BEFORE they're registered:
- Registration queue stores calls
- Registered functions execute queued calls
- Allows init ordering flexibility

**Implication for SRE**: Need similar queue-based JNI/LNI interface.

### #4: Configuration Decoupling
TOML config allows tweaking without recompilation:
- String customization
- Armor models/attributes
- Feature toggles

**Implication for SRE**: Implement config loading for mods.

---

## 10. SWKIWI COMPONENTS TO ADAPT FOR PC

| Component | Status | Adaptation |
|-----------|--------|-----------|
| Hook System | Core | Use SRE's existing hooks, not GlossHook |
| Virtual FS | Core | Map MiniPaths to PC filesystem |
| LNI System | Core | Adapt for PC (C/C++ directly, not JNI) |
| Mini API | Core | Re-implement functions in C/C++ |
| Event System | Core | Reuse (Scene_Create/Destroy) |
| Config (TOML) | Important | Implement TOML parser |
| Lua Libs | Important | Include expanded Lua stdlib |
| Logging | Nice | Redirect to cout/file |
| Music System | Optional | Replace with SDL audio |
| Crash Handler | Optional | Platform-specific |

---

## 11. QUICK REFERENCE: SWKIWI COMPONENT LOCATIONS

| Component | Path | Type |
|-----------|------|------|
| Entry | `MainActivity.java` | Java |
| JNI Bridge | `NativeBridge.java` | Java |
| Hook System | `core/hooks.h` | C Header |
| Virtual FS | `core/impl_files/` | C Core |
| LNI System | `LuaNativeInterface.java` | Java |
| Events | `events/scene.c` | C |
| Features | `features/` | C (11 dirs) |
| Lua Core | `lua/*.h` | Headers |
| Config | `assets/mini.toml` | TOML |
| Tests | `app/src/main/assets/tests/` | Lua |

---

## 12. DEPENDENCIES FOR SWKIWI

### Required Libraries
- **GlossHook** - Function hooking (already included in repo)
- **toml-c** - TOML parser (submodule)
- **LuaSocket** - Socket library (submodule, C only)
- **LuaFileSystem** - File system access (submodule)
- **Lua 5.1** - Scripting engine (bundled)

### For PC Port
- Will **NOT** need GlossHook (use SRE's hooks)
- **WILL** need toml-c, LuaSocket, LuaFileSystem, Lua 5.1

---

## NEXT STEPS

1. ✅ Analyze SwKiwi architecture (DONE)
2. ⏳ Analyze RLSW mod requirements (IN PROGRESS)
3. ⏳ Design SRE integration points
4. ⏳ Plan persistent data storage
5. ⏳ Design UI/buttons implementation
6. ⏳ Create master implementation plan
