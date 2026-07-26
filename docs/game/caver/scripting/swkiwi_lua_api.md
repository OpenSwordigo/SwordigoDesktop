# Caver SwKiWi & Native Engine Lua API Reference Documentation

## 1. System Overview & Purpose

This document provides a complete audit and technical reference for all Lua API namespaces, functions, component bindings, and meta-object getters/setters exposed by the native Swordigo engine and the reverse-engineered **SwKiWi** modding framework.

This specification enables mod developers and C++ port authors (`swd`) to maintain $100\%$ script compatibility with all existing Swordigo mods and level scripts.

---

## 2. Global Lua API Namespaces Overview

```
Global Lua API Root
 ├── Mini (Global Engine Settings & Utility API)
 ├── Character (Player Stats, Movement, Actions & Experience API)
 ├── Health (Mana & Health Properties Query API)
 ├── Scene (Spatial World Query & Entity Search API)
 ├── Item (Inventory & Equipment Query API)
 ├── Sound (3D Spatial Audio & Sound Effect Trigger API)
 ├── Music (Background Music Playlist & Stream Controller API)
 └── Component (Dynamic Meta-Component Field Query & Writer API)
```

---

## 3. Comprehensive Namespace Function Reference

### 1. The `Mini` Namespace (`mini`)

Exposed as a global table `Mini` or via `require("mini")`:

| Function | Signature | Purpose & Behavior |
| :--- | :--- | :--- |
| `SetControlsHidden` | `SetControlsHidden(bool hidden)` | Toggles touch overlay visibility. |
| `GetProfileID` | `GetProfileID() -> string` | Returns active save profile folder name (e.g. `"profile_1"`). |
| `Arch` | `Arch() -> string` | Returns system CPU architecture (`"arm64"`, `"x86_64"`). |
| `SceneFindAll` | `SceneFindAll(string componentType) -> table` | Finds all active scene entities containing specified component. |
| `ToggleDebug` | `ToggleDebug() -> bool` | Toggles developer debug console overlay. |
| `SetCoinLimit` | `SetCoinLimit(number maxCoins)` | Modifies maximum soul coin storage limit. |
| `RecreateHero` | `RecreateHero()` | Safely despawns and recreates player hero entity at last spawn point. |
| `ReloadTextures` | `ReloadTextures()` | Flushes and reloads texture atlas caches. |
| `SetWeaponColor` | `SetWeaponColor(obj, r, g, b, a, intensity)`| Sets RGBA color tint and glow intensity on weapon mesh. |
| `SetModelName` | `SetModelName(obj, string modelPath)` | Updates `ModelComponent` mesh file path dynamically. |
| `SetObjectSpeed` | `SetObjectSpeed(obj, float speedMult)` | Modifies entity movement speed multiplier. |

---

### 2. The `Character` Namespace (`mini_character`)

Exposed as global table `Character`. Controls character stats, movement parameters, and action commands:

#### A. Stats, Level & Attributes
- `GetLevel() -> int`: Returns current player character level ($1 - 50$).
- `SetLevel(int level)`: Sets player character level directly.
- `GetExp() -> int`: Returns current accumulated experience points.
- `SetExp(int exp)`: Sets player experience points and triggers `ExperienceBar::UpdateExperience()`.
- `ExpForLevel(int level) -> int`: Returns total XP required to reach specified level.
- `GetLevelAttributes() -> h, a, m`: Returns spent attribute points for Health ($h$), Attack ($a$), and Magic ($m$).
- `SetLevelAttributes(h, a, m)`: Sets attribute points spent.

#### B. Physics & Movement Tuning
- `GetWalkSpeed() -> float` / `SetWalkSpeed(float speed)`: Queries/sets ground walk speed ($8.5\text{f}$ default).
- `GetRunSpeed() -> float` / `SetRunSpeed(float speed)`: Queries/sets sprint speed ($12.0\text{f}$ default).
- `GetJumpHeight() -> float` / `SetJumpHeight(float impulse)`: Queries/sets jump velocity impulse ($14.0\text{f}$ default).
- `GetAirJumpUsed() -> bool` / `SetAirJumpUsed(bool used)`: Queries/resets double-jump flag state.
- `SetStunTime(float duration)`: Stuns character for specified time duration in seconds.

#### C. Action Commands
- `StartJumping()` / `StopJumping()`: Triggers/releases jump action.
- `Swing()` / `StopSwing()`: Triggers sword swing attack animation and hitbox.
- `DropQuickly()`: Triggers downward fast-fall impulse while airborne.
- `CancelCasting()` / `FinishCasting()`: Controls spell cast state machine.
- `Die()`: Forces instant character death sequence.
- `Use()`: Triggers interaction with nearby NPC or chest.

---

### 3. Meta-Component Generic API (`Component`)

Exposes a unified inspection system to query or write properties on any entity component dynamically:

```lua
-- Query specific field on entity component
local health = Component.HealthComponent.GetValue(entityObj, "health")

-- Write new value to entity component
Component.HealthComponent.SetValue(entityObj, "health", 100)

-- Batch fetch all component values into Lua table
local propsTable = Component.PhysicsObjectComponent.GetValues(entityObj)

-- Batch write values from Lua table to component
Component.PhysicsObjectComponent.SetValues(entityObj, { mass = 5.0, friction = 0.8 })
```

---

## 4. Reverse Engineering & Tools Integration Notes

- **SwKiWi Integration**: SwKiWi implements `mini_character/exp.c`, `attributes.c`, and `functions.c` by resolving native C++ method mangled symbols from `libswordigo.so`.
- **SRE PC Integration**: SRE maps these Lua bindings to C++ native structures in `swd` to allow running all mobile mods on PC without script modification.

---

## 5. PC Port (`swd`) Implementation Strategy

1. **Complete API Parity**: Implement all functions listed in the audit table above within the PC engine's Lua state setup.
2. **Type Checking & Error Guarding**: Wrap all Lua API entries in standard `luaL_checktype` and `luaL_checkudata` guards to prevent null-pointer crashes from malformed script inputs.
