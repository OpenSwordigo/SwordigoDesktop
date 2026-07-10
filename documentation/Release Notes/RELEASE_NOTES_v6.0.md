# ⚔️ Swordigo Desktop v6.0 — Release Notes

> *June 22, 2026*

---

## From Runtime to Platform

v6.0 completes the transition from "runtime that hooks a game" to **full platform ownership**. The GUI rendering pipeline, Lua environment, virtual filesystem, and save system are now ours. `libswordigo.so` is down to physics, AI, collision, and entity logic — everything the player *sees* runs through SRE.

```
Before (v5.0):    17 hooks — SRE owns audio, HUD, death
Now (v6.0):       30+ hooks — SRE owns GUI, Lua, menus, saves, PostFX
```

### 🎉 Two Long-Awaited Bugs — SOLVED

> **Wastelands Spinlock** — The infamous freeze in the Wastelands region that forced players to use ARM32 as a workaround? **Fixed.** The ARM64 emulation now handles the spinlock correctly.
>
> **Death Freeze** — Dying in the game caused a permanent freeze (the ad SDK path hung forever). **Fixed.** SRE now intercepts the death handler and directly triggers checkpoint respawn — instant, clean, no restart needed.

---

## 🐛 Critical Bug Fixes

### 💀 Death Loop Fix

A **duplicate hook entry** at offset `0x347efc` was causing death/respawn to be unreliable. Two hooks were mapped to the same offset — `sre_ShowAdMaybe` (correct) and `sre_GameOverVC_ShowAdMaybe` (stale duplicate from an older codebase snapshot). The stale entry was overwriting the correct hook at load time, breaking the respawn path. Fixed by removing the duplicate.

---

### 🔧 GameSceneView::Update Recovered

The **entire body** of `sre_GameSceneView_Update` was missing from the build. It existed only in an old TVPG snapshot and was never carried over to the ext4 working copy. This function is responsible for:

| Element | What It Does |
|---------|-------------|
| **Health bar** | HP display updates |
| **Mana bar + skill button** | Magic system UI |
| **Smart coin bar** | Shop-aware auto-hide (appears on pickup, stays in shops, fades after 3s) |
| **Damage flash** | Red screen overlay on HP decrease |
| **Controls hide** | Hides touch controls during combat cinematics |
| **Use/pickup button** | Context-sensitive interaction prompt |
| **Cinematic skip timer** | Skip countdown for cutscenes |
| **GUI effect animations** | Animated UI transitions |

Recovered and placed in new file `sre_scene_update.c`.

---

### 📤 Player Stats Exported

13 volatile globals (`g_sre_player_hp`, `g_sre_player_coins`, etc.) were referenced by the host but **never defined** in `libsre.so`. The linker silently resolved them to zero. Now properly defined and populated every frame.

---

### ❄️ Wastelands Freeze Fixed

The v5.0 showstopper — heavy ARM64 emulation stalls in the Wastelands region — is **resolved**. Players no longer need to switch to ARM32 for this area.

---

## 🆕 What's New

### 🖼️ Full GUI DrawRect Stack

8 native DrawRect hooks give SRE **total control** over the game's GUI rendering pipeline:

| Hook | Class |
|------|-------|
| 1 | `GUIWindow` |
| 2 | `GUIView` |
| 3 | `GUIButton` |
| 4 | `GUILabel` |
| 5 | `GUIFrameView` |
| 6 | `GUIAlertView` |
| 7 | `GUISlider` |
| 8 | `NewMenuView` |

Every GUI element's draw call passes through SRE before reaching the renderer.

---

### 🎨 Native GUI Rendering

Three core GUI classes are **fully reimplemented in C**:

| Class | What's Native |
|-------|--------------|
| **GUIButton** | Button state management (normal/hover/pressed/disabled) |
| **GUILabel** | Text rendering and layout |
| **GUIFrameView** | Frame borders and background fills |

---

### 🏷️ Button Text Override System

Any `GUILabel` or `GUIButton` text can be modified at draw time. Used to rename **"Offers" → "Options"** on the main menu, removing traces of the mobile IAP storefront.

---

### 🚫 Scoped Button Hiding

Specific buttons can be hidden by pointer match. Used to **remove the IAP shop button** from the main menu cleanly — no NOP patches, no layout hacks.

---

### 💾 Save Editor

Built directly into the launcher. Edit any save file's:

- 💰 Coins
- ❤️ Health
- 🔮 Mana
- ⭐ XP
- ⚔️ Weapon
- 🔑 Keys

---

### 🗂️ Asset Viewer

Standalone tool (`make asset_viewer`) for browsing game assets:

| Asset Type | Support |
|-----------|---------|
| **Textures** | PVR and PNG |
| **Audio** | Music and sound effects |
| **Scenes** | Scene data inspection |

Launchable from the launcher UI.

---

### 🎬 PostFX Pipeline

6 shader presets with full post-processing. Cycle with **F6**.

| Preset | Character |
|--------|-----------|
| **Cinematic** | Film-grade color grading |
| **Retro** | Pixel-era nostalgia |
| **Fantasy** | Vibrant, saturated tones |
| **Noir** | Desaturated high-contrast |
| **Ethereal** | Soft bloom, dreamlike |
| **Atmospheric** | Fog and depth haze |

Available effects: SSAO, God Rays, Vignette, Film Grain, Chromatic Aberration, Sharpen.

---

### 🔧 Other New Features

| Feature | Details |
|---------|---------|
| **SRE Lua Libraries** | Extended Lua environment with custom SRE libraries |
| **SwMini API Compatibility** | `Mini.*`, `LNI.*`, `Components.*` Lua tables for mod compatibility |
| **Virtual Filesystem** | Mod asset layering system (`sre_vfs.c`) for future mod support |
| **ProgramState::Update Hook** | Frame-level hook for game loop interception |
| **luaD_throw + ProgramPanic hooks** | Crash safety net for Lua errors and engine panics |
| **AudioSystem::SetMusicVolume hook** | Routes engine UI volume slider to OpenAL |
| **Launcher Icon Fix** | Multi-path fallback chain for window icons (launcher + asset viewer) |

---

## 🏗️ Architecture Improvements

### Hook Table Expansion

| Metric | v5.0 | v6.0 |
|--------|------|------|
| **Active hooks** | 17 | 30+ |
| **SRE source files** | — | 16 |
| **Lines of SRE C code** | — | ~4000+ |
| **JNI bridged functions** | — | 200+ |

---

### Relay Stubs

`SreHookEntry` now carries 3 fields — **offset**, **name**, and **orig_func** — enabling call-through to original functions. Hooks can intercept, modify, and optionally forward to the original implementation.

---

### Non-const Hook Table

The hook table is no longer `const`. The host can write relay addresses at runtime, enabling dynamic hook composition.

---

### Clean Source Organization

| File | Responsibility |
|------|---------------|
| `sre_gui_native.c` | Native GUI rendering (Button, Label, Frame) |
| `sre_gui.c` | Drawing API (DrawRect hooks) |
| `sre_gui.h` | GUI type definitions |
| `sre_scene_update.c` | GameSceneView::Update reimplementation |
| `sre_lua_libs.c` | Custom Lua libraries |
| `sre_mini_api.c` | SwMini API compatibility layer |
| `sre_vfs.c` | Virtual filesystem for mod assets |

---

## ⚠️ Known Limitations

### ARM64 (arm64-v8a) — Primary Target

| Issue | Status | Notes |
|-------|--------|-------|
| **Bolt/timer misbehavior** | 🟡 Known | Due to timing misalignment in the ARM64 emulation layer, certain boss attacks, bolt-shooting enemies, and bolt-based traps may fire projectiles at abnormally fast rates. Sword-wielding enemies may also behave differently from the Android version. This is not intentional — we will patch these in future releases. |
| **PostFX in menus** | 🟡 v7 | Should auto-disable in menus — detection infrastructure exists but not wired |
| **Options menu** | 🟡 v7 | CreditsVC hijack disabled for stability — v7 target |
| **F11 overlay** | 🟡 v7 | Disabled for stability — v7 target |
| **Text input crash** | 🔴 Open | Set Name for save file → wild jump to `0x2d6ce4c`, needs investigation |

### ARM32 (armeabi-v7a)

| Issue | Status | Notes |
|-------|--------|-------|
| **No SRE** | 🟡 By design | `libsre.so` only supports ARM64 — ARM32 runs without engine hooks |

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
│  ✅ GUI Rendering  ✅ GUI Text Override  │
│  ✅ PostFX         ✅ Lua Environment   │
│  ✅ Save Editor    ✅ Virtual FS        │
│  ✅ Crash Recovery ✅ Audio Volume      │
├─────────────────────────────────────────┤
│  libswordigo.so (Gameplay Kernel)       │
│  🎯 Physics       🎯 AI                │
│  🎯 Combat        🎯 Collision          │
│  🎯 Lua Scripts   🎯 Scene Loading      │
│  🎯 Save System   🎯 Entity Systems     │
└─────────────────────────────────────────┘
```

---

## 📦 Download

| Format | Size | Platform |
|--------|------|----------|
| `.rpm` | 78M | Fedora/RHEL x86_64 |
| `.deb` | 78M | Debian/Ubuntu x86_64 |

Includes: `swordigo_boot`, `libsre.so`, engine binaries (v1.4.6 + v1.4.12, ARM32 + ARM64), 939 game assets, 9 music tracks, save editor, asset viewer, PostFX shaders.

---

## 👥 Credits

See [README.md](README.md) for the full credits list.

---

*Powered by the Swordigo Runtime (SRT) v6.0*
