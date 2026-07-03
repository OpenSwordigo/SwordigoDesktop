<div align="center">
<img width="225" height="225" alt="image" src="https://github.com/user-attachments/assets/24f6bc9f-fb38-4618-b3e1-e7de5d3c67f8" />

# ⚔️ Swordigo Desktop

### The Swordigo Runtime (SRT)

*The classic 2012 action-adventure — running natively on Linux through a custom ARM compatibility runtime.*

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/TheCorrectSynovian/SwordigoDesktop/blob/master/LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86__64-purple.svg)](#)
[![Version](https://img.shields.io/badge/Version-v7.3-00e5ff.svg)](https://github.com/TheCorrectSynovian/SwordigoDesktop/releases)
[![Engine](https://img.shields.io/badge/Engine-SRT%20v7.3-8b3dff.svg)](#-srt-architecture)

[Website](https://thecorrectsynovian.github.io/SwordigoDesktop/web/) · [Download](https://github.com/TheCorrectSynovian/SwordigoDesktop/releases) · [Research](https://thecorrectsynovian.github.io/SwordigoDesktop/web/research.html) · [Changelog](https://thecorrectsynovian.github.io/SwordigoDesktop/web/changelog.html)

</div>

---

**Swordigo Desktop** is a native Linux port of the beloved mobile action-adventure platformer by Touch Foo. Rather than running through Android emulation layers, this project uses the **Swordigo Runtime (SRT)** — a layered runtime architecture that treats `libswordigo.so` as a gameplay kernel while progressively replacing subsystems with clean, native reimplementations.

v7.3 Combatch Update brings **Swordfare GUI (ImGui-based vector overlay)**, **Host-side Coin Limit Breakers**, **UI Input Swallowing**, **custom video background support**, **dynamic edge clear void correction**, a default-on **Low-end PC PostFX preset**, and full system vertical sync (VSync) — building on top of the decentralized instance architecture and libmpg123 music decoding from v7.2.

---

## 🎯 What's New in v7.3

### 🖥️ Swordfare GUI & Vector Overlay
- Replaces legacy raw-text panels with a sleek dark-themed ImGui overlay.
- High-DPI support with dynamic vector scaling and custom Swordfare typography.
- Remastered F3 Debug panel with collapsible tray metrics (Resolutions, Frametimes graph, Render calls, PostFX state, CPU delta).

### 🪙 9999 Coin Limit Breaker
- Host-side JNI memory patches for both ARM32 and ARM64 engines to bypass the hard-coded 999 coin limit.

### 🔒 UI Control Lockout
- Native touch event swallowing when `UI.SetControlsDisabled` is active. Prevents gameplay movements while custom UI buttons or settings menus are open.

### 🎥 Video Background Support (F7 Toggle)
- Dynamic frame-by-frame background loop playback (0.4x speed)
- Shell-based FFmpeg extraction keeps host memory safe and stable
- Toggle dynamic backgrounds on/off with `F7` (instantly falls back to vanilla assets, no freezing)

### 🎨 Background Border Void Correction
- Samples boundary edge colors of vanilla backgrounds and video frames
- Dynamically overrides `glClearColor` if a screen clear is black/dark
- Eliminates the ugly black void underneath levels when jumping or falling

### 💨 Default-On Optimized "Low" PostFX Preset
- Configures default-on color correction, vignette, and screen-space shadows
- SSAO, Bloom, and God rays are disabled for a fast, lag-free experience on low-end GPUs
- Requires zero extra rendering passes, compiling to a single composite pass

### ⌨️ Re-allocated Hotkeys
- `F7` is now dedicated to toggling video backgrounds
- `\` (Backslash) toggles manual keyboard typing mode

### 📦 Modular Packaging Updates
- RPM and DEB packages exclude heavy video files (`.mp4`) automatically
- Users grab `vbg.zip` separately and unpack it to their local assets folder

> See [v7.3 Release Notes](RELEASE_NOTES_v7.3.md) for full details.

---

## ⚡ SRT Architecture

```
┌───────────────────────────────────────────────────────────────┐
│  🖥️  Platform Layer (Host)                                   │
│      SDL3 · OpenGL · OpenAL · Linux x86_64                    │
├───────────────────────────────────────────────────────────────┤
│  🎮  Controls Manager         │  🎨  Presentation Layer      │
│      Keyboard/Gamepad/Touch   │      PolyMC Launcher          │
│      Configurable bindings    │      F-key overlays           │
│      Macro support            │      PostFX pipeline          │
├───────────────────────────────┼───────────────────────────────┤
│  🔧  JNI Bridge               │  📦  ELF Loader              │
│      200+ bridged functions   │      ARM32 + ARM64            │
│      libc/GL/AL/IO/pthread    │      Full ELF relocation      │
├───────────────────────────────┴───────────────────────────────┤
│  ⚙️  Dynarmic JIT (Default) / Unicorn Interpreter (Fallback) │
│      ARM64 JIT → x86_64 native · 60fps · Near-native speed   │
├───────────────────────────────────────────────────────────────┤
│  🏗️  libsre.so — Swordigo Runtime Engine (Guest ARM64)       │
│      30+ active hooks · GUI · Music · HUD · Death · Saves    │
│      Replaces subsystems, not patches them                    │
├───────────────────────────────────────────────────────────────┤
│  📜  libswordigo.so — Gameplay Kernel (Original ARM Binary)   │
│      Physics · AI · Lua · Combat · Entities · Saves           │
└───────────────────────────────────────────────────────────────┘
```

### Runtime Layers

| Layer | Component | What It Does |
|-------|-----------|-------------|
| **Platform** | SDL3, OpenGL, OpenAL | Windowing, rendering, audio on host |
| **Controls** | Input Config + Macro Engine | Fully remappable keyboard/gamepad/touch |
| **Bridge** | JNI Bridge (200+ functions) | Translates ARM JNI calls to host APIs |
| **Loader** | ELF Loader (ARM32 + ARM64) | Parses, relocates, loads ARM shared objects |
| **Emulator** | Dynarmic JIT (default) / Unicorn | JIT compiles ARM64 → x86_64 at near-native speed |
| **SRE** | libsre.so (30+ hooks) | **Replaces** game subsystems with clean C |
| **Kernel** | libswordigo.so | Original game: physics, AI, Lua, combat |

### 🏗️ SRE — What It Owns

| Subsystem | Status | How |
|-----------|--------|-----|
| 🎵 Music | **Fully replaced** | 6 hooks replace MusicPlayer, command interface to host OpenAL |
| 💀 Death/Respawn | **Fully replaced** | 1 hook skips ads, calls native respawn from checkpoint |
| 🎮 HUD (HP/Mana/Coins) | **Fully replaced** | Full GameSceneView::Update reimplementation |
| 💰 Smart Coin Bar | **Owned** | Shop-aware auto-hide, 3s fade, world-change detection |
| 🌄 Backgrounds | **Fully replaced** | 3 hooks for custom sky/depth rendering |
| 🔴 Damage Flash | **Owned** | Red screen flash on HP decrease |
| 📊 Player Stats | **Exported** | HP, Mana, Coins, XP, Level, ATK — readable from host |
| 🧵 String System | **Replaced** | 4 hooks eliminate atomic STXR spin loops |
| 🖼️ GUI Rendering | **Fully replaced** | 8 DrawRect hooks — buttons, labels, frames, sliders natively in C |
| 💾 Save Editor | **Integrated** | Built into launcher — edit coins, HP, mana, XP, weapons, keys |
| 🔍 Asset Viewer | **New tool** | Browse PVR/PNG textures, audio, scenes (`make asset_viewer`) |
| 🛡️ Crash Safety | **Active** | luaD_throw + ProgramPanic + __cxa_throw interception |

---

## ✨ Features

### 🎮 Full Gameplay
- Complete game loop — explore, fight, solve puzzles, defeat bosses
- **Instant death respawn** — native checkpoint respawn, no process restart
- Save system with persistent progress (`~/.local/share/swordigo-desktop/save/`)
- Full audio: music tracks + sound effects through OpenAL
- **Music loop watchdog** — ensures background music never stops unexpectedly
- **Music format support** — MP3, OGG, WAV via direct libmpg123 (v7.2+)

### 🚀 Multi-Instance Support
- **Vanilla Swordigo 1.4.12 ARM64** — fully stable, beatable end-to-end
- **RLSwordigo 6.6** — roguelike mode, near-stable with experimental features
- **Modded Instances** — Mason Mod, Phonkdigo, and more (KiwiAPI framework)
- **Decentralized `.ini` configs** (v7.2+) — each instance is self-contained

### 🎨 Desktop-Native Experience
- **1920×1080 internal rendering** with FBO-based scaling (Sharp Bilinear, Nearest, CRT Scanline)
- **Keyboard controls** — fully remappable via the in-game Controls Editor (F2)
- **Gamepad support** — Xbox/PlayStation controllers with analog stick + D-pad
- **Multi-touch support** — 10 independent touch inputs for touchscreen laptops

### 🎨 PostFX Pipeline
- **6 Presets** (F6) — Cinematic, Retro, Fantasy, Noir, Ethereal, Atmospheric
- **SSAO** — Screen Space Ambient Occlusion with 16-sample hemisphere
- **God Rays** — 64-sample radial blur from configurable sun position
- **Color Effects** — Vignette, Film Grain, Chromatic Aberration, Sharpen

### 🛠️ Engine Features
| Key | Feature |
|-----|---------|
| **F1** | Toggle GUI menu bar (File/Emulation/Config/Misc/Help) |
| **F2** | Controls Editor (drag to reposition buttons) |
| **F3** | Debug overlay (FPS, draw calls, player stats, binary info) |
| **F4** | Cycle scaling modes |
| **F5** | Camera override toggle |
| **F6** | Cycle PostFX presets |
| **F7** | Toggle video background playback (ON/OFF) |
| **\\** | Toggle keyboard typing mode |
| **F10** | Toggle native on-screen controls |
| **F12** | Fullscreen toggle |

### 🚀 SRT Launcher
- **PolyMC-inspired Instance Manager** — Card grid layout with instance icons
- **Multi-Binary Support** — v1.4.6, v1.4.12 in ARM32 + ARM64
- **Engine Selection** — Dynarmic JIT (default) or Unicorn interpreter
- **Decentralized Config** — `.ini` files per instance (v7.2+)
- **Custom Instance Import** — Add any `.so` binary with custom naming

---

## 🎮 Modding Ecosystem

### Two Modding Frameworks

Swordigo Desktop supports **two parallel modding approaches:**

#### 1️⃣ **KiwiAPI / Mini API (Primary — Active Development)**
- **Status:** Beta (actively developed, primary focus for new mods)
- **Framework:** SwKiwi/SwMini modloader hooks into SRE
- **Capabilities:** Full game hooks, custom entities, audio, GUI, saves
- **Examples:** RLSwordigo, Phonkdigo, KiwiAPI button mods
- **Recommendation:** Use this for new mods — it's the future of Swordigo modding

#### 2️⃣ **SDMOD (Lightweight Custom — Maintained, Not Expanded)**
- **Status:** Early beta (lightweight, not actively expanded)
- **Framework:** Direct binary patching + simple Lua tweaks
- **Capabilities:** Cosmetics, lightweight behavior changes, simple tweaks
- **Limitations:** Fragile, no advanced hooks, breaks on game updates
- **Note:** Useful for quick tweaks, but superseded by KiwiAPI for complex work

**Bottom line:** Use **KiwiAPI** for serious modding. Use **SDMOD** only for lightweight adjustments.

---

## 📦 Install (v7.2)

### Pre-built Packages
| Format | Platform | Command |
|--------|----------|---------|
| `.rpm` | Fedora x86_64 | `sudo dnf install swordigo-desktop-7.2.0-1.x86_64.rpm` |
| `.deb` | Debian/Ubuntu x86_64 | `sudo dpkg -i swordigo-desktop_7.2.0-1_amd64.deb` |

### Build from Source

See [BUILD.md](BUILD.md) for the full developer build guide.

**Quick Start:**
```bash
# Install dependencies (Fedora)
sudo dnf install libmpg123-devel unicorn-devel SDL3-devel SDL3_image-devel \
    openal-soft-devel mesa-libGL-devel zlib-devel libvorbis-devel \
    gcc-aarch64-linux-gnu cmake

# Clone and build
git clone https://github.com/TheCorrectSynovian/SwordigoDesktop.git
cd SwordigoDesktop
./run_swordigo.sh   # Auto-builds, compiles, installs SRE, and launches
```

> **Note**: `aarch64-linux-gnu-gcc` is required to cross-compile libsre.so for ARM64. Dynarmic JIT is built from included source automatically on first run.

---

## ⚠️ Supported Instances & Bug Reporting

### ✅ Officially Supported (Bug Reports Welcome)

| Instance | Status | Details |
|----------|--------|---------|
| **Vanilla Swordigo 1.4.12 ARM64** | ✅ Fully stable | Beatable end-to-end, all bosses accessible, all progression paths verified |
| **RLSwordigo 6.6** | 🟡 Near stable | Gameplay stable; some RogueSpells mod features incomplete |
| **Mason Mod** | 🟡 Near stable | Core mechanics work; cosmetic features untested |

### ❌ Not Supported (No Bug Reports)

| Instance | Status | Reason |
|----------|--------|--------|
| **Combatch v3** | Works, unstable | Partially functional; crashes on certain mod combinations |
| **ARM32 instances** | Deprecated | No SRE support, not actively maintained (ARM64 is the future) |
| **Other custom mods** | As-is | Use KiwiAPI framework for new mod development |

### How to Report Bugs

Only report crashes/bugs for **supported instances listed above**:
1. Clearly state the instance name and version
2. Provide exact reproduction steps
3. Include console output or log files
4. Submit on [GitHub Issues](https://github.com/TheCorrectSynovian/SwordigoDesktop/issues)

**Note:** ARM32 support is intentionally deprecated in favor of ARM64 performance. We focus 100% on ARM64.

---

## 🎯 Controls

### Default Keyboard Layout
| Key | Action | Alt Key |
|-----|--------|---------|
| ← / **A** | Move Left | |
| → / **D** | Move Right | |
| **Space** / **W** | Jump | |
| **J** / **Z** | Attack | |
| **K** / **X** | Magic | |
| **I** | Use Item | |
| **Escape** | Menu / Settings | |
| **P** | Pause | |

### Gamepad
| Button | Action |
|--------|--------|
| D-Pad / Left Stick | Movement |
| A / Cross | Jump |
| X / Square | Attack |
| Y / Triangle | Magic |
| B / Circle | Use Item |
| Start | Menu |

All controls are fully remappable — press **F2** to open the Controls Editor. Config is saved to `controls.ini`.

---

## ⚠️ Known Limitations

### ARM64 (arm64-v8a) — Primary Target
| Issue | Severity | Details |
|-------|----------|---------|
| Bolt/timer misbehavior | 🟡 Medium | Timing misalignment causes certain bosses, bolt-shooting enemies, and bolt traps to fire at abnormal rates. Will be patched. |
| Text input crash | 🟡 Medium | Typing into certain UI fields can crash — avoid F7 in menus |

### ARM32 (armeabi-v7a) — Deprecated
| Issue | Severity | Details |
|-------|----------|---------|
| Timer-based spikes | 🔴 High | Repeating timer spikes don't activate |
| Boss gates | 🔴 High | Post-boss gates don't trigger |
| No SRE | 🔴 High | libsre.so is ARM64 only — ARM32 runs without engine hooks |
| **Not actively maintained** | ⚠️ Design | Focus is 100% on ARM64 for best performance. ARM32 is effectively deprecated. |

---

## 🆕 What's New in v7.2

### ⚡ Dynarmic JIT (v7.0 foundation, still present)
- **ARM64 JIT compiler** — near-native execution
- **60fps stable framerate** — smooth gameplay
- **Instant input response** — zero perceptible latency

### 🎵 v7.2 NEW: Custom Music Format Support
- MP3, OGG, WAV unified support via direct libmpg123
- **10× faster music loading**
- Modular search algorithm — all directories checked for all formats
- Fixed RLSwordigo MP3 loading

### 📁 v7.2 NEW: Decentralized Configuration
- Per-instance `.ini` files (like PolyMC/Prism)
- Eliminated centralized JSON manifests
- Each instance self-contained

### 💨 v7.2 NEW: I/O Optimization
- Suppressed verbose file operation logs
- Reduced console spam by 95%
- Memory leak fixes

### 🔧 v7.2 NEW: KiwiAPI Buttons & Touchables (Beta)
- Experimental interactive GUI elements
- Full backward compatibility with existing mods

> See [v7.2 Release Notes](RELEASE_NOTES_v7.2.md) for comprehensive details.

---

## 👥 Credits

### Core Team

| Role | Name | GitHub |
|------|------|--------|
| **Lead Developer** | TheMegineBraine | -- |
| **Developer / Project Creator** | TheCorrectSynovian | [@QuantumCreeper](https://github.com/TheCorrectSynovian) |
| **Developer** | MrSinup | -- |

### v7.2 Contributors

| Contribution | Name |
|-------------|------|
| **MP3 Playback & Direct libmpg123 Integration** | MrSinup |
| **KiwiAPI Buttons & Touchables** | Mano K |
| **Music System Improvements** | Mano K |
| **I/O Optimization & Build Script Refactoring** | QuantumCreeper |
| **Launcher & Configuration Architecture** | QuantumCreeper |

### Research & Community

| Contribution | Name | GitHub |
|-------------|------|--------|
| **SwMini** — Swordigo Mini mod loader & reverse engineering | ItsJustSomeDude (IJSD) | [@ItsJustSomeDude](https://github.com/ItsJustSomeDude) |
| **SwKiwi API** — KiwiAPI modding framework for Swordigo | Kiziyon | [@Kiziyon](https://github.com/Kiziyon) |
| **Swordigo Vita Port** — Original ARM→desktop porting research | Rinnegatamante | [@Rinnegatamante](https://github.com/Rinnegatamante) |

### Original Game

> **Swordigo** — Copyright © 2012-2025 Ville Mäkynen / Touch Foo
> All Rights Reserved — [touchfoo.com/swordigo](http://www.touchfoo.com/swordigo)

### Open Source Dependencies

| Project | License | Purpose |
|---------|---------|---------|
| [Dynarmic](https://github.com/lioncash/dynarmic) | BSD-0-Clause | ARM64 JIT compiler (default engine) |
| [Unicorn Engine](https://www.unicorn-engine.org/) | GPL-2.0 | ARM CPU interpreter (fallback engine) |
| [SDL3](https://www.libsdl.org/) | Zlib | Window, input, gamepad |
| [SDL3_image](https://github.com/libsdl-org/SDL_image) | Zlib | Texture loading |
| [OpenAL Soft](https://openal-soft.org/) | LGPL-2.1 | Audio playback |

---

## ⚖️ Legal Notice

This project **does not include or distribute** any original game assets, binaries, music, or copyrighted content from Swordigo or Touch Foo. Users must provide their own `libswordigo.so` and `assets/` directory extracted from a legally obtained copy of the game.

This is a personal research and preservation project. Swordigo is the property of Ville Mäkynen / Touch Foo.

---

<div align="center">

*Powered by the Swordigo Runtime (SRT)*

Built with ❤️ for game preservation

</div>
