# ⚔️ Swordigo Desktop v5.0 — Release Notes

> *June 21, 2026*

---

## The Swordigo Runtime (SRT)

v5.0 is a **paradigm shift**. We're no longer patching individual functions — we're **replacing entire subsystems**. The introduction of `libsre.so` (Swordigo Runtime Engine) means the game's original `libswordigo.so` is now treated as a **gameplay kernel** while SRE owns presentation, audio, HUD, and death handling.

```
Before (v1.0–v4.5):    Find bug → Patch bug
Now (v5.0):             Find subsystem → Understand subsystem → Replace subsystem
```

---

## 🆕 What's New

### 🏗️ libsre.so — The Swordigo Runtime Engine

The headline feature. A **guest-side ARM64 shared library** loaded alongside `libswordigo.so` that installs **17 active hooks** to intercept and replace problematic functions with clean C reimplementations.

| Hook Category | Count | What It Replaces |
|--------------|-------|-----------------|
| **CppString** | 4 | Atomic refcount ops (`STXR` spin loops → simple `MOV`) |
| **MusicPlayer** | 6 | Entire music system → OpenAL command interface to host |
| **Background** | 3 | Custom sky/parallax rendering |
| **GameSceneView** | 1 | Full `GameSceneView::Update` reimplementation |
| **Death/Respawn** | 1 | `ShowAdMaybe` → native `GameOverViewDidContinue` |
| **Exception** | 1 | `__cxa_throw` interception for crash recovery |
| **Lua Safety** | 1 | `lua_pcall` with `setjmp`/`longjmp` error recovery |

---

### 🎵 Full Music System Replacement

The game's `MusicPlayer` class is completely replaced. SRE writes music commands to shared memory, and the host reads them to drive OpenAL.

| Feature | Details |
|---------|---------|
| **Load/Play/Pause/Stop** | Full playback control via command interface |
| **Volume control** | Host-side `F9`/`F10` volume adjust + mute toggle |
| **Looping** | `AL_LOOPING` + watchdog timer that restarts stopped loops |
| **No more silence** | Music loop watchdog catches and restarts music that stops unexpectedly |

---

### 💀 Instant Death Respawn

No more process restarts on death. SRE hooks `GameOverViewController::ShowAdMaybe` and directly calls `GameOverViewDidContinue`, respawning the player at their last checkpoint instantly.

---

### 🎮 HUD System Reimplementation

`GameSceneView::Update` is fully reimplemented in C, giving SRE control over:

| Element | Behavior |
|---------|----------|
| **Coin bar** | Smart auto-hide: appears on pickup, stays in shops, fades after 3s |
| **Damage flash** | Red screen overlay when HP decreases |
| **Scene detection** | Tracks world changes and shop entry/exit |
| **Player stats export** | HP, Mana, Coins, XP, Level, ATK → readable from host F3 overlay |

---

### 🔒 SRE Version Gate

SRE hooks use **hardcoded function offsets** for v1.4.12 ARM64. Loading them on other binaries would crash. v5.0 adds an automatic version check:

```
v1.4.12 ARM64 → SRE loads ✅ (17 hooks active)
Any other binary → "[SRE] Skipped" ⛔ (runs without hooks, safe)
```

---

### 🧹 UI Cleanup

Non-functional mod menu sections (Combat, Movement, Economy, Cheats) are **hidden** for this release. Only the working **GAME** section remains:

- ⚡ Game Speed (0.25× – 4×)
- ⏸️ Pause
- 📷 Free Camera
- 🎥 Smooth Camera

---

### 🔧 Other Improvements

| Change | Details |
|--------|---------|
| **Non-atomic strings** | 4 hooks replace `STXR`-based atomic refcounting → eliminates hangs |
| **Lua error recovery** | ARM64 `setjmp`/`longjmp` implementation for safe `lua_pcall` |
| **GameState pointer** | Exported from SRE, enabling host-side direct memory read/write |
| **Package builder** | Updated to v5.0 with `libsre.so` bundled in ARM64 engine dirs |

---

## ⚠️ Known Limitations

### ARM64 (arm64-v8a) — Primary Target

| Issue | Status | Workaround |
|-------|--------|------------|
| **Wastelands freeze** | 🔴 Open | Game spinlocks in Wastelands. Switch to ARM32 for this region. |
| **Heavy function stalls** | 🟡 Known | Some entity functions take 800ms+ in dungeon areas |

### ARM32 (armeabi-v7a)

| Issue | Status | Workaround |
|-------|--------|------------|
| **Timer-based spikes** | 🔴 Open | Repeating timer spikes don't activate |
| **Boss gates** | 🔴 Open | Post-boss gates don't trigger |
| **No SRE** | 🟡 By design | `libsre.so` only supports ARM64 — ARM32 runs without engine hooks |

---

## 🎮 Recommended Play Strategy

1. **Start on ARM64 v1.4.12** — best SRE support, instant respawn, full music
2. **Play through to Willcliff Campsite** on ARM64
3. **Switch to ARM32** for Wastelands and regions where ARM64 freezes
4. **F3** to monitor player stats in real-time

---

## 📦 Download

| Format | Size | Platform |
|--------|------|----------|
| `.rpm` | 78M | Fedora/RHEL x86_64 |
| `.deb` | 78M | Debian/Ubuntu x86_64 |

Includes: `swordigo_boot`, `libsre.so`, engine binaries (v1.4.6 + v1.4.12, ARM32 + ARM64), 939 game assets, 9 music tracks.

---

## 📊 SRT Ownership Map

What we own vs what `libswordigo.so` still handles:

```
┌─────────────────────────────────────────┐
│  SRE Owns (libsre.so)                  │
│  ✅ Music         ✅ Death/Respawn      │
│  ✅ HUD           ✅ Coin Bar           │
│  ✅ Damage Flash   ✅ Backgrounds       │
│  ✅ String Ops     ✅ Player Stats      │
├─────────────────────────────────────────┤
│  libswordigo.so (Gameplay Kernel)       │
│  🎯 Physics       🎯 AI                │
│  🎯 Combat        🎯 Collision          │
│  🎯 Lua Scripts   🎯 Scene Loading      │
│  🎯 Save System   🎯 Entity Systems     │
└─────────────────────────────────────────┘
```

---

## 👥 Credits

See [README.md](README.md) for the full credits list.

---

*Powered by the Swordigo Runtime (SRT) v5.0*
