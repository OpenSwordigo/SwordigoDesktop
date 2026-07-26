# SwKiwi API Compatibility Audit (June 9 / July 9 Source)

This document contains a comprehensive audit of all Lua API libraries, namespaces, functions, and components from the SwKiwi (June 9 / July 9) codebase compared against SRE (Swordigo Remastered Engine) on PC.

---

## 1. The `Mini` Namespace (`mini`)

Exposed as a global table `Mini` or via `require("mini")`.

| Function | Signature | Purpose | SRE Status |
| :--- | :--- | :--- | :--- |
| `SetControlsHidden` | `SetControlsHidden(bool)` | Hides or shows touch overlay controls | **Implemented** (No-op stub on PC as touch overlay is simulated/hidden) |
| `GetProfileID` | `GetProfileID() -> string` | Retrieves the profile ID folder name | **Implemented** |
| `Arch` | `Arch() -> string` | Returns cpu architecture ("arm64" etc) | **Implemented** (Returns "x86_64" or similar for PC) |
| `SceneFindAll` | `SceneFindAll(string type) -> table` | Finds all game objects of a given component type | **Implemented** |
| `ToggleDebug` | `ToggleDebug() -> bool` | Toggles the debug console / overlay | **Implemented** |
| `SetCoinLimit` | `SetCoinLimit(number)` | Modifies max coin limit | **Implemented** (Stubbed / getter available) |
| `RecreateHero` | `RecreateHero()` | Safe deferred player recreation | **Implemented** (PC runtime hooks added) |
| `ReloadTextures` | `ReloadTextures()` | Resets texture caches | **Implemented** |
| `SetWeaponColor` | `SetWeaponColor(obj, r,g,b,a, intensity)` | Color/intensity glow on weapon | **Implemented** (Traverses components or Trinket list) |
| `SetWeaponColorForTrinket` | `SetWeaponColorForTrinket(item_id, r,g,b,a, intensity)` | Sets weapon glow for trinket string ID | **Implemented** (Alias for SetWeaponColor) |
| `map` | `map(table/string, fn) -> table` | High-level functional helper | **Implemented** |
| `SetModelName` | `SetModelName(obj, name)` | Updates 3D model component file name | **Implemented** |
| `SetObjectSpeed` | `SetObjectSpeed(obj, speed)` | Adjusts entity speed multiplier directly | **Implemented** (Direct SceneObject write) |
| `Test` | `Test()` | Debug testing probe | **Implemented** (Stubbed) |

---

## 2. The `Character` Namespace (`mini_character`)

Exposed as a global table `Character`. Controls experience, character level, stats, and physical movement triggers.

### A. Experience and Attributes (`mini_character/exp.c`, `attributes.c`)

| Function | Signature | Target Component & Offset | SRE Status |
| :--- | :--- | :--- | :--- |
| `GetLevel` | `GetLevel() -> int` | `HeroObject` + `0xb8` (64-bit) | **Missing** |
| `SetLevel` | `SetLevel(int)` | `HeroObject` + `0xb8` (64-bit) | **Missing** |
| `GetExp` | `GetExp() -> int` | `HeroObject` + `0xb4` (64-bit) | **Missing** |
| `SetExp` | `SetExp(int)` | `HeroObject` + `0xb4` (64-bit); also resets `scene_controller` + `0x17c` to `0.2f` to force level-up and calls `ExperienceBar::UpdateExperience()` | **Missing** |
| `ExpForLevel` | `ExpForLevel(int) -> int` | Calls `Caver::CharacterState::ExperiencePointsRequiredForLevel(CharacterState*, int)` | **Missing** |
| `GetLevelAttributes` | `GetLevelAttributes() -> h, a, m` | `hero` + `0xbc`, `0xc0`, `0xc4` (64-bit) | **Missing** |
| `SetLevelAttributes` | `SetLevelAttributes(h, a, m)` | `hero` + `0xbc`, `0xc0`, `0xc4` (64-bit) | **Missing** |
| `GetWalkSpeed` | `GetWalkSpeed() -> float` | `CharControllerComponent` + `0x278` (64-bit) | **Missing** |
| `SetWalkSpeed` | `SetWalkSpeed(float)` | `CharControllerComponent` + `0x278` (64-bit) | **Missing** |
| `GetRunSpeed` | `GetRunSpeed() -> float` | `CharControllerComponent` + `0x280` (64-bit) | **Missing** |
| `SetRunSpeed` | `SetRunSpeed(float)` | `CharControllerComponent` + `0x280` (64-bit) | **Missing** |
| `GetJumpHeight` | `GetJumpHeight() -> float` | `CharControllerComponent` + `0x26c` (64-bit) | **Missing** |
| `SetJumpHeight` | `SetJumpHeight(float)` | `CharControllerComponent` + `0x26c` (64-bit) | **Missing** |
| `GetAirJumpUsed` | `GetAirJumpUsed() -> bool` | `CharControllerComponent` + `0x260` (64-bit) | **Missing** |
| `SetAirJumpUsed` | `SetAirJumpUsed(bool)` | `CharControllerComponent` + `0x260` (64-bit) | **Missing** |

### B. Action Commands (`mini_character/functions.c`)

Binds native `CharControllerComponent` methods directly to Lua wrappers:

| Function | Signature | Target C++ Method Symbol | SRE Status |
| :--- | :--- | :--- | :--- |
| `DropQuickly` | `DropQuickly()` | `_ZN5Caver23CharControllerComponent11DropQuicklyEv` | **Missing** |
| `StartJumping` | `StartJumping()` | `_ZN5Caver23CharControllerComponent12StartJumpingEv` | **Missing** |
| `StopJumping` | `StopJumping()` | `_ZN5Caver23CharControllerComponent11StopJumpingEv` | **Missing** |
| `CancelCasting` | `CancelCasting()` | `_ZN5Caver23CharControllerComponent13CancelCastingEv` | **Missing** |
| `FinishCasting` | `FinishCasting()` | `_ZN5Caver23CharControllerComponent13FinishCastingEv` | **Missing** |
| `Die` | `Die()` | `_ZN5Caver23CharControllerComponent3DieEv` | **Missing** |
| `Use` | `Use()` | `_ZN5Caver23CharControllerComponent3UseEv` | **Missing** |
| `Hurt` | `Hurt()` | `_ZN5Caver23CharControllerComponent4HurtEv` | **Missing** |
| `Swing` | `Swing()` | `_ZN5Caver23CharControllerComponent5SwingEv` | **Missing** |
| `StopSwing` | `StopSwing()` | `_ZN5Caver23CharControllerComponent9StopSwingEv` | **Missing** |
| `CanDoSomething` | `CanDoSomething() -> bool` | `_ZN5Caver23CharControllerComponent14CanDoSomethingEv` | **Missing** |
| `CanBeginCasting` | `CanBeginCasting() -> bool` | `_ZN5Caver23CharControllerComponent15CanBeginCastingEv` | **Missing** |
| `CanUse` | `CanUse() -> bool` | `_ZN5Caver23CharControllerComponent6CanUseEv` | **Missing** |
| `CanJump` | `CanJump() -> bool` | `_ZN5Caver23CharControllerComponent7CanJumpEv` | **Missing** |
| `CanSwing` | `CanSwing() -> bool` | `_ZN5Caver23CharControllerComponent8CanSwingEv` | **Missing** |
| `CanPickup` | `CanPickup() -> bool` | `_ZN5Caver23CharControllerComponent9CanPickupEv` | **Missing** |
| `StartMovingToDirection` | `StartMovingToDirection(dir)` | `_ZN5Caver23CharControllerComponent22StartMovingToDirectionEi` | **Missing** |
| `StopMovingToDirection` | `StopMovingToDirection(dir)` | `_ZN5Caver23CharControllerComponent21StopMovingToDirectionEi` | **Missing** |
| `SetMovementFacingLock` | `SetMovementFacingLock(bool)` | `CharControllerComponent` + `0x245` (64-bit) | **Missing** |
| `SetStunTime` | `SetStunTime(number)` | `CharControllerComponent` + `0x2f8` (64-bit) | **Missing** |

---

## 3. The `Health` Namespace (`mini_health`)

| Function | Signature | Target Component & Offset | SRE Status |
| :--- | :--- | :--- | :--- |
| `CurrentMana` | `CurrentMana(obj) -> int` | `ManaComponent` + `0x74` (64-bit) | **Missing** |
| `CurrentManaPercent` | `CurrentManaPercent(obj) -> float` | `ManaComponent` + `0x74` / `0x70` | **Missing** |

---

## 4. Meta-Component API (`Component`)

Exposes a unified layout system to query/write fields on components of a `SceneObject` userdata.

```lua
Component.<Type>.GetValue(obj, field)
Component.<Type>.SetValue(obj, field, value)
Component.<Type>.GetValues(obj) -> table
Component.<Type>.SetValues(obj, table)
Component.<Type>.IsPresent(obj) -> bool
```

### Supported Components and Fields:

1. **`SwingableWeaponController`**:
   - `SomeString` (offset `0x88` in 64-bit)
   - `SomeString2` (offset `0x90` in 64-bit)
2. **`Health`**:
   - `CurrentHealth` (offset `0x80` in 64-bit)
   - `MaxHealth` (offset `0x7c` in 64-bit)
   - `SomeString` (offset `0x38` in 64-bit)
3. **`Physics`**:
   - `GroundDeceleration` (offset `0x10c` in 64-bit)
   - `AirDeceleration` (offset `0x110` in 64-bit)
   - `Gravity` (offset `0xe8` in 64-bit)
   - `Elasticity` (offset `0xf0` in 64-bit)
4. **`Entity`**:
   - `SpeedCap` (offset `0xac` in 64-bit)
   - `Gravity` (offset `0xe8` in 64-bit)

- **SRE Status**: **All Missing**.

---

## 5. Graphics drawing (`g`)

Exposes primitive canvas graphics, custom viewport calculations, and model/joint control.

1. **`Button`**: Custom GUI overlays, shapes, colors, textures, and coordinates.
2. **`Camera`**: UI orthographic projection and text alignment.
3. **`CharAnim`**: Controllable model-specific clip speeds and timeline positions.
4. **`Keyboard`**: OS-level text prompts.
5. **`Skeleton`**: Inspection and custom rendering of bones/vertices.

- **SRE Status**: **All Missing**.

> [!NOTE]
> SRE already implements a native C++ `Button` overlay system (defined in [sre_gui.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_gui.c) and [sre_gui_native.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_gui_native.c)) to handle button layout, click detection, and drawing. We should use this system in the future to resolve any parity issues with the SwKiwi API and shift button operations to the Lua `g` namespace.

---

## 6. LNI (Lua Native Interface) Bridge

SwKiwi utilizes LNI (`ExecuteLNI`, `BindLNI`) to dynamically resolve and invoke Java static methods via JNI.
SRE provides a stubbed LNI layer that captures the most common commands (like `quit()`, `copyToClipboard()`) and executes them natively on the host PC system. It lacks a real Java JNI bridge since the PC build is standalone C++ and runs without an Android Virtual Machine.
