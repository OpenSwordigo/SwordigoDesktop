# Swordigo Engine (Caver) — Complete Reverse Engineering Report

> Compiled from: SwMini modloader source, Ghidra decompiled ARM64 binaries (v1.4.6 + v1.4.12), FileRift protobuf schemas, Vita port source, Java decompilation.

---

## 1. Engine Overview

| Property | Value |
|----------|-------|
| **Engine Name** | Caver Engine |
| **C++ Namespace** | `Caver::` |
| **Language** | C++ with Boost (shared_ptr, intrusive_ptr, function) |
| **Scripting** | Lua 5.1 (statically linked into libswordigo.so) |
| **Rendering** | OpenGL ES 1.x (fixed-function pipeline) |
| **3D Models** | PowerVR POD format (`.pod` files) |
| **Textures** | PVR compressed + PNG fallback |
| **Audio** | OpenAL (sound effects), Java MediaPlayer (music) |
| **Serialization** | Google Protocol Buffers (all game data) |
| **Architecture** | Component-on-Object (not pure ECS) |
| **Threading** | Single-threaded gameplay; worker threads for background loading only |

---

## 2. Game Loop (JNI Entry Points)

| Phase | JNI Function | Purpose |
|-------|-------------|---------|
| Init 1 | `setFilesDir(path)` | Set save/data directory |
| Init 2 | `setCacheDir(path)` | Set cache directory |
| Init 3 | `setAssetManager(manager)` | Attach Android asset manager |
| Init 4 | `handleApplicationLaunch()` | Initial launch setup |
| Init 5 | `setupNativeInterface()` | Register native methods (FindClass, GetMethodID) |
| Init 6 | `setupApplication()` | Full engine init — loads gdata, gopt, gstate |
| Init 7 | `setApplicationViewSize(w, h, is_pad)` | Set viewport |
| Init 8 | `applicationDidBecomeActive()` | App became foreground |
| **Loop** | `updateApplication(deltaTime)` | **Game logic tick** |
| **Loop** | `drawApplication()` | **Render frame** |
| Input | `handleTouchEvent(type, id, time, x, y, ...)` | Touch input |

**Delta time clamping** (critical for stability):
```c
if (deltaSecond > 0.1f)
    deltaSecond = 0.016666668f;  // Clamp to ~60fps
```

---

## 3. Entity/Component System

### Architecture
- `SceneObject` — base game object with position, rotation, scaling, depth
- `Component` — base component with `OutletBinding` for inter-component communication
- `ComponentCollection` — keyed by `long` (Interface pointer) for type-safe lookups
- `ObjectTemplate` / `ObjectLibrary` — prototyping pattern for `.scl` files
- `EntityClass` — defines attributes (Freezable, Stunnable, Grabbable, resistances)

### Component Lookup
```cpp
Caver::SceneObject::ComponentWithInterface(long interface_id) → Component*
```
Each component type has a static `Interface()` function returning a vtable pointer.

### All 97 Component Types

#### Rendering
ModelComponent, SpriteComponent, ShadowComponent, ShadowVolumeComponent, LightComponent, GlowComponent, SimpleGlowComponent, WeaponGlowComponent, BackgroundComponent, RotatingBackgroundComponent, TextureMappingComponent, GroundMeshComponent, GroundMeshGeneratorComponent, WaterMeshComponent, GroundPolygonComponent, WeaponTrailComponent, HookshotTrailComponent, PortalEffectComponent

#### Physics
ShapeComponent, CollisionShapeComponent, BoneControlledCollisionShapeComponent, PhysicsObjectComponent, PhysicsPlatformComponent

#### Combat
AttackComponent, DamageComponent, HealthComponent, ManaComponent, SwingComponent, SwingableWeaponComponent, SwingableWeaponControllerComponent, SkillComponent, SpellComponent

#### Entity AI
EntityComponent, EntityInfoComponent, EntityControllerComponent, EntityActionComponent, HeroEntityComponent, MonsterEntityComponent

#### Monster Controllers (16 types!)
MonsterControllerComponent, WalkingMonsterController, ChargingMonsterController, SnappingMonsterController, LeapingMonsterController, SkellyMonsterController, StaticMonsterController, ShootingMonsterController, BatMonsterController, BouncingMonsterController, GenericMonsterController, ProjectileMonsterController, MonsterDeathControllerComponent

#### Level Mechanics
DoorControllerComponent, ElevatorControllerComponent, PressureTriggerComponent, ObjectLinkControllerComponent, PortalComponent, SpawnPointComponent, BreakableObjectComponent, BushControllerComponent, OrbitControllerComponent, ProjectileControllerComponent, CollectableItemComponent, ItemDropComponent

#### Animation
KeyframeAnimationComponent, BlendAnimationComponent, AnimationControllerComponent, AnimationComponent, CharAnimControllerComponent, CharControllerComponent, TransformComponent, TransformControllerComponent, ModelTransformControllerComponent

#### Scripting
ProgramComponent

#### Effects/Magic
ParticleComponent, ParticleEmitterComponent, ParticleFieldComponent, ParticleObjectComponent, FireEmitterComponent, FireBreathComponent, MagicParticleEmitterComponent, MagicBoltComponent, MagicBombComponent, MagicExplosionComponent, MagicSpellCastComponent, MagicHookshotComponent, DimensionSpellComponent, DimensionObjectComponent

#### UI
OverlayComponent, OverlayTextComponent, OverlayTargetArrowComponent, TextBubbleComponent, TouchableComponent, SoundEffectComponent, PropertiesComponent

---

## 4. Key Data Structures (ARM64 Offsets)

### ProgramState (Lua VM Wrapper)

| Offset | Field | Type |
|--------|-------|------|
| 0x0 | `lua_State *L` | pointer |
| 0x8 | thread indicator | pointer (NULL = not thread) |
| 0x20 | `sceneObject` | pointer |
| 0x48 | `isSuspended` | int |
| 0x4c | `sleepTime` | float |
| 0x51 | active flag | bool |
| 0x52 | `paused` | bool |
| 0x53 | `completed` | bool |
| 0x54 | `speedScaling` | float |

### Controller Hierarchy

| Path | ARM32 | ARM64 |
|------|-------|-------|
| GameViewController → GameSceneController | 0x68 | 0xc8 |
| GameSceneController → HeroObject | 0x4 | 0x8 |
| GameSceneController → HeroReference | 0xa4 | 0xd8 |
| HeroReference → Position (Vector3) | 0x40 | 0x70 |
| SceneObject → Identifier | 0x4 | 0x8 |
| SceneObject → ID string (CppString) | 0x2c | 0x50 |

### Component Field Offsets

| Component.Field | ARM32 | ARM64 |
|-----------------|-------|-------|
| HealthComponent.CurrentHealth | 0x80 | 0x80 |
| HealthComponent.MaxHealth | 0x7c | 0x7c |
| EntityComponent.FacingDirection | 0x3c | 0x70 |
| EntityComponent.SpeedCap | 0x68 | 0xac |
| PhysicsObject.GroundDeceleration | 0xc4 | 0x10c |
| PhysicsObject.AirDeceleration | 0xc8 | 0x110 |
| PhysicsObject.Gravity | 0xa4 | 0xe8 |
| PhysicsObject.Elasticity | 0xac | 0xf0 |
| CharController.WalkSpeed | 0x170 | 0x278 |
| CharController.RunSpeed | 0x178 | 0x280 |
| CharController.JumpHeight | 0x164 | 0x26c |
| CharController.AirJumpUsed | 0x158 | 0x260 |
| ModelComponent → ModelInstance | 0x48 | 0x88 |
| ModelInstance → SkeletonInstance | 0x0 | 0x18 |

### CppString Layout
```c
typedef struct CppString_Header {
    size_t length;
    size_t capacity;
    int uc;          // usage count (reference count) — ATOMIC!
    char string[];   // actual string data follows
} CppString_Header;
```
- Empty string sentinel at BSS offset: ARM32=0x6a04, ARM64=0x14880
- **Uses LDAXR/STLXR for atomic reference counting** ← ROOT CAUSE of STXR spin loops!

### Math Types
```c
typedef struct Vector2 { float x, y; } Vector2;
typedef struct Vector3 { float x, y, z; } Vector3;
typedef struct FloatColor { float R, G, B, A; } FloatColor;
typedef struct { float m[16]; } Matrix4;
```

---

## 5. Lua Integration (Lua 5.1)

### Key Facts
- All Lua C API functions are exported from libswordigo.so with **C++ name mangling**
- Different mangling between 32/64 bit for `size_t` params: `j` (32-bit) vs `m` (64-bit)
- Lua globals: `gameController`, `scene`, `cameraController` (all lightuserdata)
- `ProgramState::FromLuaState(lua_State*)` recovers ProgramState from Lua state
- `Program` protobuf has both `String` (Lua source for 64-bit) and `Bytes` (bytecode for 32-bit)

### Timer/Suspend Mechanism
When `isSuspended == 1`, each `Update(deltaTime)` decrements `sleepTime`. When `sleepTime < 0`, `lua_resume()` is called. This drives `Scene.Wait()`, `Cutscene.Wait()`, etc.

### Key Symbols
```
_ZN5Caver12ProgramState7ExecuteEi          // ProgramState::Execute(int)
_ZN5Caver12ProgramState6UpdateEf           // ProgramState::Update(float)
_ZN5Caver12ProgramState6ResumeEi           // ProgramState::Resume(int)
_ZN5Caver12ProgramState22RegisterProgramLibraryEv  // RegisterProgramLibrary()
_ZN5Caver12ProgramState12FromLuaStateEP9lua_State  // FromLuaState()
```

---

## 6. Serialization Formats (Protocol Buffers)

### Scene File (`.scene`)
```
Scene {
    Object[]       → SceneObject instances
    ObjectLibrary  → local templates (.scl data)
    Bounds         → Rectangle
    Group[]        → SceneObjectGroup (hide/show)
    OnLoad         → Program (Lua executed at scene load)
}
```

### Save File (`.gplayer`)
```
PlayerProfile {
    Name, ExperienceLevel, TimePlayed
    GameState {
        CharacterState {
            CurrentHealth, CurrentMana, CurrentCoins
            ExperiencePoints, ExperienceLevel
            Item[] → (Name, Count)
            EquippedWeapon, EquippedArmor, CurrentSkill
            WeaponTrinket, ArmorTrinket, SkillTrinket
        }
        LevelState[] → (LevelName, Visited, NumTreasures, TreasuresFound)
        CurrentLevel, CurrentSpawnPoint, CurrentMapNodeName
        QuestState[], QuestText[]
    }
    Counter[] → (Name, Value)
}
```

### Map File (`.scmap`)
```
MapZone → MapNode (with Portal connections)
MapNode has LevelName, Type (DEFAULT/TOWN/WAYPOINT/BOSS), portal directions
```

---

## 7. Hooking Technique (GlossHook)

SwMini's approach to hooking libswordigo.so:

### Loading Sequence
1. `System.loadLibrary("mini")` → `JNI_OnLoad` → `earlyLoad()`
2. `System.loadLibrary("swordigo")` loads libswordigo.so
3. Java calls `midLoad()`:
   - `dlopen("libswordigo.so", RTLD_NOLOAD)` → get handle
   - `dladdr(__bss_start)` → get load bias
   - All symbol lookups via `dlsym(handle, mangled_name)`
   - All offset access via `load_bias + offset`
4. Java calls `lateLoad()` after engine initialization

### Hook API
```c
GlossHookAddr(target_addr, replacement_func, instruction_set);
// instruction_set: I_THUMB (32-bit) or I_ARM64 (64-bit)

WriteMemory(addr, data, size);  // Direct instruction patching
```

---

## 8. CppString Functions (Key Offsets for v1.4.12)

| Function | ARM32 | ARM64 |
|----------|-------|-------|
| CppString from char* | 0x37bc60 | 0x566bb8 |
| CppString append | 0x379988 | 0x567254 |
| CppString assign | 0x37aa1c | 0x56918c |
| CppString unsafe_release | 0x3787c8 | 0x565220 |
| CppString safe_release | 0x379768 | N/A (inlined) |
| Empty sentinel (BSS) | 0x6a04 | 0x14880 |

---

## 9. Thread Analysis

### Thread Usage in Swordigo
| Thread | Purpose | Critical? |
|--------|---------|-----------|
| GL/Main Thread | All game logic + rendering | **YES** |
| Audio Thread | OpenAL processing | Handled by host |
| IO Thread | Background asset loading | **NO** (can be synchronous) |
| Entity Worker | Populates entity linked list | **PROBLEM THREAD** |

### The Entity Worker Problem
- `entity_setup` at `0x1583248` traverses a linked list populated by a worker thread
- Without the thread running, entities aren't looked up properly
- This is the ROOT CAUSE of ARM64 entity processing freeze
- **Solution**: Either run the worker thread properly OR rewrite entity setup natively

---

## 10. Root Causes of ARM64 Freeze (Confirmed)

| Cause | Mechanism | Fix Strategy |
|-------|-----------|-------------|
| **CppString atomic refcount** | LDAXR/STLXR in every string copy/destroy | STXR→STR patcher OR native CppString |
| **boost::shared_ptr atomics** | LDAXR/STLXR in every shared_ptr copy | STXR→STR patcher OR native shared_ptr |
| **Entity worker thread** | Linked list populated by pthread that we stub | Native entity setup in libsre.so |
| **boost::intrusive_ptr atomics** | Same as shared_ptr | Same fix |

---

## 11. Key C++ Mangled Symbols (for dlsym hooking)

```
// Scene & Object
_ZN5Caver5SceneC1Ev                          // Scene::Scene()
_ZN5Caver5SceneD0Ev                          // Scene::~Scene()
_ZNK5Caver11SceneObject22ComponentWithInterfaceEl  // ComponentWithInterface

// Lua
_ZN5Caver12ProgramState7ExecuteEi            // Execute(int)
_ZN5Caver12ProgramState6UpdateEf             // Update(float)
_ZN5Caver12ProgramState6ResumeEi             // Resume(int)
_ZN5Caver12ProgramState12FromLuaStateEP9lua_State

// Hero & Combat
_ZN5Caver19GameSceneController18CreateHeroObjectAtERKNS_7Vector3Eib
_ZN5Caver20HeroEquipmentManager17ModelNameForArmorERKN5boost10shared_ptrINS_4ItemEEE
_ZN5Caver14CharacterState29ArmorDamageMultiplierWithItemERKN5boost10shared_ptrINS_4ItemEEES6_

// Rendering
_ZN5Caver16SkeletonInstance16EvaluateMatricesEv
_ZN5Caver6Camera24SetPerspectiveProjectionEffff

// Assets
_ZN5Caver16FileExistsAtPathERKSs
_ZN5Caver29NewByteBufferFromAndroidAssetERKSsPj

// SceneObject Lib
_ZN5Caver14SceneObjectLib15PushSceneObjectEPNS_12ProgramStateEPNS_11SceneObjectE
_ZNK5Caver11SceneObject21updateSpeedMultiplierEv
```

---

## 12. Modding Tools Available

| Tool | Purpose |
|------|---------|
| **FileRift 5.8.5** | Decode/recode all protobuf game files (.scene, .gdata, .gplayer, .scmap, etc.) |
| **SwMini** | Runtime hooking framework with 97 component type mappings |
| **Ghidra decompilation** | Full C code for both v1.4.6 and v1.4.12 (ARM64) |
| **Java decompilation** | v1.1 and v1.4.6 Java source |
| **Vita port** | Complete C port with game loop implementation |
