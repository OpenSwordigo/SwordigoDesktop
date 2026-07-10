# Swordigo Desktop v7.0 — Release Notes
<img width="929" height="247" alt="swordigo_desktop_text" src="https://github.com/user-attachments/assets/35a7e21c-41a3-45ae-85c0-73928260a303" />

**Release Date:** June 24, 2026  
**Codename:** *The Performance Revolution*  
**Tag:** `v7.0`  

---

## 🆕 What's New

### ⚡ Dynarmic JIT Compiler (Headline Feature)

The headline feature of v7.0 is a **Dynarmic Just-In-Time compiler** that replaces the Unicorn interpreter as the default ARM64 execution backend. ARM64 guest code now compiles to native x86_64 at runtime, achieving near-native performance.

| Metric | Unicorn (Interpreter) | Dynarmic (JIT) |
|--------|-----------------------|-----------------|
| **Framerate** | 40-50 fps | 60 fps locked |
| **Execution model** | Instruction-by-instruction | Block-compiled native |
| **Status** | Retained as `--no-dynarmic` fallback | **Default** |
| **Linking** | Dynamic | Static (no new runtime deps) |
| **License** | GPL-2.0 | BSD-0-Clause |

- Dynarmic source included in `deps/dynarmic/`
- Build with: `make -j$(nproc) DYNARMIC=1`
- Unicorn Engine is still available via `--no-dynarmic` for debugging or compatibility

### 🎮 RLSwordigo Support
<img width="2340" height="1319" alt="Screenshot From 2026-06-24 15-43-17 (1)" src="https://github.com/user-attachments/assets/c363e38d-e3cc-491d-b7b1-35f36b68b200" />

- Play the roguelike Swordigo spinoff through custom instances
- Fully supported via the instance management system

### 🔌 KiwiAPI Compatibility (Phase 1 & 2)

- For RLSwordigo
- SWKiwi modloader hooks for the mod ecosystem
- Enables third-party mod loading through the KiwiAPI interface

### 💎 Bauble API (Phase 3.3)

- Trinket/bauble system hooks for the SRE engine
- Enables mods to interact with the in-game trinket system

### 🏆 Achievement System (Phase 3.4)

- Achievement hooks integrated into the SRE runtime
- Foundation for tracking and displaying player achievements

### 🖥️ Native Aspect Ratio

- **F12** fullscreen toggle now preserves the display's native aspect ratio (16:10, 3:2, etc.)
- No longer forces 16:9 — adapts to the monitor's actual geometry

### 📂 Launcher Asset Path

- New `launcher/` subfolder for RPM/DEB installations
- Fixes icon and texture loading on packaged installs where assets were not found at the expected relative paths

### 🔧 Runtime Bridges & Error Recovery

| Feature | Details |
|---------|---------|
| **io.open bridge** | Full `io.open` implementation for guest file I/O |
| **fgets/fscanf bridges** | C standard library file reading for Combatch mod |
| **_longjmp registration** | Lua error recovery support — prevents crashes on Lua `error()` calls |

### 🗂️ Instance Management Overhaul

- Custom assets folders per instance
- Each instance can carry its own game data, mods, and configuration
- Clean separation between Swordigo, RLSwordigo, and modded instances

---

## 📁 Files Changed

### New Files
- `deps/dynarmic/` — Dynarmic JIT source tree (BSD-0-Clause)
- `launcher/` — Launcher asset subfolder for packaged installs
- Bauble API hooks
- Achievement system hooks
- KiwiAPI / SWKiwi modloader integration
- RLSwordigo instance support

### Modified Files
- `src/main.cpp` — Dynarmic JIT initialization, backend selection, aspect ratio logic
- `src/jni/jni_bridge_arm64.cpp` — `io.open`, `fgets`, `fscanf`, `_longjmp` bridges
- `src/sre/sre_init.c` — Bauble API and achievement hook registration
- `src/platform/launcher_ui.cpp` — Asset path resolution, instance management overhaul
- `src/platform/fbo_scaler.cpp` — Native aspect ratio support for F12 fullscreen
- `Makefile` — `DYNARMIC=1` flag, static linking of Dynarmic, launcher asset path

---

## 🏗️ Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    HOST (x86_64)                              │
│  ┌──────────┐  ┌───────────┐  ┌──────────────────┐          │
│  │ Launcher │  │ FBO/PostFX│  │   JNI Bridge     │          │
│  │ (ImGui)  │  │  Scaler   │  │ (~400 bridges)   │          │
│  └──────────┘  └───────────┘  └────────┬─────────┘          │
│                                        │                     │
│  ┌──────────────────────────────────────┴────────┐           │
│  │  Dynarmic JIT (Default)  │  Unicorn (Fallback)│           │
│  │  ARM64 → x86_64 native   │  Interpreter mode  │           │
│  │  60fps, near-native       │  10-15fps           │           │
│  └──────────┬───────────────────────────────────┘           │
│             │                                               │
│  ┌──────────▼──────────────────┐                            │
│  │    Guest Memory (512MB)     │                            │
│  │  ┌─────────────────────┐   │                            │
│  │  │  libswordigo.so     │   │                            │
│  │  │  (ARM64 game binary)│   │                            │
│  │  ├─────────────────────┤   │                            │
│  │  │  libsre.so (SRE)    │   │                            │
│  │  │  34+ hooks, Mini/LNI│   │                            │
│  │  └─────────────────────┘   │                            │
│  └────────────────────────────┘                            │
└──────────────────────────────────────────────────────────────┘
```

---

## 📦 Packaging

### Install

```bash
# Fedora / RHEL
sudo rpm -Uvh swordigo-desktop-7.0.0-1.x86_64.rpm

# Ubuntu / Debian
sudo dpkg -i swordigo-desktop_7.0.0-1_amd64.deb

# First run setup (automatic on install, manual if needed)
swordigo-setup
```

### Package Contents
- `swordigo_boot` — Main executable (with Dynarmic JIT compiled in)
- `asset_viewer` — Asset browser
- `swordigo-setup` — First-run data installer
- Game assets, engine binaries (v1.4.6 + v1.4.12, ARM32 + ARM64), music
- `launcher/` — Launcher icons and textures
- `.desktop` entry + app icon

### Dependencies
- SDL3, SDL3_image
- OpenGL (Mesa)
- Unicorn Engine
- OpenAL Soft
- **Dynarmic is statically linked — NOT a runtime dependency**

---

## 🔄 Upgrade Notes

- **From v6.5:** Drop-in upgrade. No data migration needed.
- **From v5.x or earlier:** Run `swordigo-setup` after install to refresh data.
- **Build with `make DYNARMIC=1`** for JIT (recommended) or plain `make` for Unicorn-only.

---

## 🔮 What's Next (v7.5 Roadmap)

- [ ] Player name input fix (text entry crash)
- [ ] DPI scaling for overlay/GUI
- [ ] Options menu via CreditsVC hijack
- [ ] F11 SRT overlay re-enablement
- [ ] VFS re-enablement for mod asset layering

---

## ⚠️ Known Limitations

### ARM64 (arm64-v8a) — Primary Target

| Issue | Status | Notes |
|-------|--------|-------|
| **Text input crash** | 🔴 Open | Save name entry → wild jump crash, needs investigation |

### ARM32 (armeabi-v7a)

| Issue | Status | Notes |
|-------|--------|-------|
| **No SRE** | 🟡 By design | `libsre.so` is ARM64 only — ARM32 runs without engine hooks |
| **Some event processing failiures** | 🟡 Open | Open Issue, contributors can help |

---

## 👤 Credits

- **TheMegineBraine** — Lead developer
- **QuantumCreeper / TheCorrectSynovian** — Project creator, platform engineering
- **MrSinup** — Dynarmic JIT integration
- **Swordigo** — Original game by Touch Foo
- Built with: SDL3, Dear ImGui, Dynarmic, Unicorn Engine, OpenAL Soft

---

*Powered by the Swordigo Runtime (SRT) v7.0*
