# 🎮 SwMini Feature Deep-Dive — What We Can Build

Based on exhaustive analysis of every `.c` and `.h` file in SwMini's source.

> [!IMPORTANT]
> Our emulator runs **ARM32** (`libswordigo.so`). All offsets below are arm32.
> We have **full guest memory** via `g_guest_memory` — read/write any address.

---

## ✅ Ready to Implement (arm32 offsets confirmed)

### 1. 🏃 Walk Speed / Run Speed / Jump Height
**Offsets known, easy to implement via Settings panel sliders.**

| Stat | Offset from CharControllerComponent | Type | Default |
|------|--------------------------------------|------|---------|
| AirJumpUsed | `+0x158` | int (bool) | 0/1 |
| JumpHeight | `+0x164` | float | ~5.0 |
| WalkSpeed | `+0x170` | float | ~3.0 |
| RunSpeed | `+0x178` | float | ~6.0 |

**How to access:**
```
gameController (Lua global, we need to find its address)
  +0x68 → GameSceneController*
    +0x04 → HeroObject (SceneObject*)
    → ComponentWithInterface(hero, CharControllerComponent_Interface)
      +0x170 → WalkSpeed (float)
      +0x178 → RunSpeed (float)
      +0x164 → JumpHeight (float)
      +0x158 → AirJumpUsed (int)
```

**Infinite Double-Jump:** Set `AirJumpUsed` to `0` every frame → unlimited mid-air jumps!

---

### 2. 🧊 Infinite Mana
**arm32 offsets confirmed!**

| Field | Offset from ManaComponent | Type |
|-------|--------------------------|------|
| MaxMana | `+0x3c` | int |
| CurrentMana | `+0x40` | int |

**Implementation:** Every frame, write `MaxMana` value into `CurrentMana`:
```cpp
int max_mana = *(int*)(g_guest_memory + mana_comp_ptr + 0x3c);
*(int*)(g_guest_memory + mana_comp_ptr + 0x40) = max_mana;
```

---

### 3. 🕊️ Fly Mode (Zero Gravity + Infinite Jumps)
**arm32 offsets confirmed!**

| Field | Offset from PhysicsObjectComponent | Type |
|-------|-----------------------------------|------|
| Gravity | `+0xa4` | float |
| Elasticity | `+0xac` | float |
| GroundDeceleration | `+0xc4` | float |
| AirDeceleration | `+0xc8` | float |

**Implementation:**
```cpp
// Fly mode = zero gravity + infinite double-jumps
*(float*)(g_guest_memory + physics_ptr + 0xa4) = 0.0f;  // no gravity
*(int*)(g_guest_memory + char_ctrl_ptr + 0x158) = 0;     // reset air jump
```

Combined with high JumpHeight (`+0x164`), this gives true flight.

---

### 4. 💰 Coin Limit Breaker (999 → 65535)
**arm32 patch addresses confirmed! Binary-level patch.**

| What | Address (arm32) | Patch |
|------|----------------|-------|
| Lua coin check MOVW | `0x27fce0` | Change `999` → `65535` |
| GameEvent MOVW | `0x27e656` | Change `999` → `65535` |
| Too Rich MOVW | `0x27e666` | Change `999` → `65535` |

**Implementation:** Directly patch the ARM instructions in `g_guest_memory`:
```cpp
// Patch MOVW instruction immediate to 0xFFFF (65535)
// ARM MOVW encoding: the immediate is split across bits
patch_movw(g_guest_memory + 0x27fce0, 65535);
patch_movw(g_guest_memory + 0x27e656, 65535);
patch_movw(g_guest_memory + 0x27e666, 65535);
```

---

### 5. 📊 Level & XP Editor
**arm32 offsets confirmed!**

| Field | Offset from HeroReference | Type |
|-------|--------------------------|------|
| Experience | `+0x60` | int |
| Level | `+0x64` | int |

**How to access:**
```
gameController +0x68 → GameSceneController
  +0xa4 → HeroReference*
    +0x60 → Exp (int)
    +0x64 → Level (int)
```

---

### 6. 🗺️ Teleport / Position Editor
**arm32 offset confirmed!**

| Field | Offset from HeroReference | Type |
|-------|--------------------------|------|
| Position | `+0x40` | Vector3 (3 floats: x, y, z) |

```cpp
float* pos = (float*)(g_guest_memory + hero_ref + 0x40);
pos[0] = new_x;  // x
pos[1] = new_y;  // y
pos[2] = new_z;  // z
```

---

## 🟡 Needs Offset Discovery (arm32 values unknown)

### 7. ❤️ God Mode (Infinite Health)
**arm64 offsets known, arm32 offsets are `0x00` (placeholder).**

| Field | arm32 | arm64 | Type |
|-------|-------|-------|------|
| MaxHealth | ❓ unknown | `+0x7c` | int |
| CurrentHealth | ❓ unknown | `+0x80` | int |

**To find arm32 offsets:** We can search for health values in guest memory at runtime (take damage, scan for changed values), or calculate from arm64 offsets using struct alignment rules.

> [!TIP]
> Arm32 health offsets are likely around `+0x3c` to `+0x44` (half the arm64 offsets due to 32-bit pointers). We can probe at runtime.

---

## 🔵 Cool But Complex

### 8. 🎭 Armor Model Swap
SwMini hooks `HeroEquipmentManager::ModelNameForArmor` to replace model strings:
- `"platearmor"` → `"hiro_plated"`
- `"magicarmor"` → `"hiro_magicplated"`

Needs function hooking in the emulator (intercept the ARM function call, return custom string).

### 9. 🐛 Engine Debug Overlay
Toggle via `DebugInfoOverlay` at known offsets. Needs the `GameSceneView` pointer chain.

### 10. 💬 Lua Console
The game has a full Lua runtime. SwMini's `Mini.ExecuteLNI` runs arbitrary Lua code. We'd need to:
1. Find the `lua_State*` pointer
2. Call `luaL_dostring()` through our emulator bridge
3. Build a text input UI

---

## 📐 Complete Pointer Chain (arm32)

```
gameController (global — need to find/capture this address)
│
├── +0x68 → GameSceneController*
│   ├── +0x04 → HeroObject (SceneObject*)
│   │   ├── +0x04 → Identifier (CppString*)
│   │   └── ComponentWithInterface() → any component
│   │
│   └── +0xa4 → HeroReference*
│       ├── +0x40 → Vector3 position {x, y, z}
│       ├── +0x60 → int Exp
│       └── +0x64 → int Level
│
└── (via GameSceneView chain — need pointer)
    └── DebugInfoOverlay, OverlayView, etc.
```

---

## 🎯 Implementation Priority

| Priority | Feature | Difficulty | Lines of code |
|----------|---------|-----------|---------------|
| 🥇 | Walk/Run Speed sliders | Easy | ~30 |
| 🥇 | Jump Height slider | Easy | ~10 |
| 🥇 | Infinite Double-Jump | Easy | ~5 per frame |
| 🥈 | Infinite Mana | Easy | ~5 per frame |
| 🥈 | Fly Mode (0 gravity) | Easy | ~10 |
| 🥈 | Coin Limit 65535 | Easy (binary patch) | ~15 |
| 🥉 | Level/XP Editor | Medium | ~40 |
| 🥉 | Teleport/Position | Medium | ~50 |
| 🏅 | God Mode | Medium (find offsets) | ~20 |
| 🏅 | Lua Console | Hard | ~200 |

> [!IMPORTANT]
> **Key blocker:** We need to capture the `gameController` pointer at runtime (like we did for `CameraController`). Once we have that, the entire pointer chain opens up and ALL features become accessible.
