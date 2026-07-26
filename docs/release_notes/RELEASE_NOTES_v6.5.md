# Swordigo Desktop v6.5 — Release Notes

**Release Date:** June 23, 2026  
**Codename:** *Documentation Drop*  
**Tag:** `v6.5`  
**Commit:** 66 files changed, +76,487 lines

---

## 🆕 What's New

### 📚 Complete Modding & API Documentation (`modapi/`)

The headline feature of v6.5 is a **22-file, 414KB documentation suite** covering every modding API, file format, and runtime system in the project. This is the first release with comprehensive public documentation.

| Document | Size | Description |
|----------|------|-------------|
| `README.md` | 5.5 KB | Index with quickstart guide |
| `modding-guide.md` | 26 KB | Full modding tutorial (music, scene, text, GUI mods) |
| `architecture.md` | 37 KB | SRT/SRE/Host runtime deep-dive |
| `sre-hooks.md` | 53 KB | All 34+ engine hooks with offsets and C signatures |
| `lua-api.md` | 22 KB | Mini.\*, LNI.\*, standard library extensions |
| `gui-api.md` | 19 KB | SRE GUI rendering API (15 functions) |
| `music-api.md` | 17 KB | End-to-end music system (hooks + host + mods) |
| `mod-config.md` | 19 KB | Shared memory protocol at guest address 0x49000 |
| `save-format.md` | 17 KB | .gplayer protobuf format + Save Editor API |
| `binary-selector.md` | 19 KB | Instance management, manifests, boot sequence |
| `data-layout.md` | 12 KB | Runtime directory structure, path resolution |
| `input-system.md` | 20 KB | Keybindings, macros, touch zone emulation |
| `postfx.md` | 14 KB | Post-processing (30+ params, 8 presets) |
| `fbo-scaler.md` | 14 KB | FBO pipeline and 4 scaling modes |
| `vfs-api.md` | 10 KB | Virtual filesystem (planned feature) |
| `overlay.md` | 8 KB | SRT runtime overlay + inventory editor |
| `debug-overlay.md` | 12 KB | F3 debug HUD reference |
| `building.md` | 13 KB | Build from source guide |
| `shortcuts.md` | 4 KB | All keyboard shortcuts |
| `formats/scene-format.md` | 15 KB | .scene protobuf wire format |
| `formats/pod-format.md` | 16 KB | PowerVR POD model format |
| `formats/pvr-format.md` | 18 KB | PVR texture format (ETC1) |
| `formats/protobuf-wire.md` | 17 KB | Zero-dependency protobuf reader/writer |

### 🖥️ Vulkan Renderer Backend

- Removed **(WIP)** label from Vulkan radio button in launcher
- Full GLES 1.x fixed-function pipeline emulator (~3000 lines)
- Uber-shader with 6 specialization constants
- State synchronization via `FixedFunctionState`
- Selectable from launcher: OpenGL or Vulkan

### 🔧 F3 Debug Overlay — Graphics API

- New **"Graphics API: OpenGL/Vulkan"** line in the debug HUD
- Shows which rendering backend is actively in use
- Visible on both ARM32 and ARM64 code paths

### 🎨 ImGui Launcher Enhancements

- Background texture support (`launcher_bg.png`)
- Instance icons (per game type: Swordigo, RLSwordigo, SwordigoMini)
- Working **Add Instance** button (opens engine directory)
- Asset Viewer launch button functional
- Instance sorting (ARM64 first, then by version)
- Vanilla instance deletion protection

### 🔌 Mod System: `sre_mod.c`

- New SRE module for mod config shared memory protocol
- Magic-validated header at guest address `0x49000`
- Supports up to 32 music + 16 scene + 16 background replacements
- Query API: `sre_mod_get_music/scene/bg_replacement()`

---

## 📁 Files Changed

### New Files (48)
- `modapi/` — 22 documentation files + `formats/` subdirectory (4 files)
- `src/platform/launcher_ui.cpp/.h` — ImGui launcher replacement
- `src/sre/sre_mod.c` — Mod config protocol
- `src/imgui/` — Dear ImGui library (11 files)
- `src/tools/pod_loader.cpp/.h` — Standalone POD parser
- `src/tools/scene_loader.cpp/.h` — Scene file parser
- `src/tools/av_renderer.cpp/.h` — OpenGL 3.3 renderer
- `src/tools/av_audio.cpp/.h` — SDL3 audio player

### Modified Files (18)
- `src/main.cpp` — Graphics API debug line, expanded HUD background
- `src/platform/launcher_ui.cpp` — Removed "(WIP)" from Vulkan label
- `src/jni/jni_bridge_arm64.cpp` — Vulkan backend routing
- `src/sre/sre_init.c` — Hook table updates
- `src/platform/fbo_scaler.cpp/.h` — PostFX state expansion
- `Makefile` — ImGui sources, asset viewer target
- `run_swordigo.sh` — Asset viewer integration
- Various SRE modules — Bug fixes and improvements

---

## 🏗️ Architecture

```
┌──────────────────────────────────────────────────────┐
│                    HOST (x86_64)                      │
│  ┌──────────┐  ┌───────────┐  ┌──────────────────┐  │
│  │ Launcher │  │ FBO/PostFX│  │   JNI Bridge     │  │
│  │ (ImGui)  │  │  Scaler   │  │ (~400 bridges)   │  │
│  └──────────┘  └───────────┘  └────────┬─────────┘  │
│                                        │             │
│  ┌─────────────────────────────┐       │             │
│  │     Unicorn Emulator        │◄──────┘             │
│  │      (ARM64 → x86)         │                      │
│  └──────────┬──────────────────┘                      │
│             │                                         │
│  ┌──────────▼──────────────────┐                      │
│  │    Guest Memory (512MB)     │                      │
│  │  ┌─────────────────────┐   │                      │
│  │  │  libswordigo.so     │   │                      │
│  │  │  (ARM64 game binary)│   │                      │
│  │  ├─────────────────────┤   │                      │
│  │  │  libsre.so (SRE)    │   │                      │
│  │  │  34+ hooks, Mini/LNI│   │                      │
│  │  ├─────────────────────┤   │                      │
│  │  │  Mod Config (8KB)   │   │                      │
│  │  │  @ 0x49000          │   │                      │
│  │  └─────────────────────┘   │                      │
│  └────────────────────────────┘                      │
└──────────────────────────────────────────────────────┘
```

---

## 📦 Packaging

### Install

```bash
# Fedora / RHEL
sudo rpm -Uvh swordigo-desktop-6.5.0-1.x86_64.rpm

# Ubuntu / Debian
sudo dpkg -i swordigo-desktop_6.5.0-1_amd64.deb

# First run setup (automatic on install, manual if needed)
swordigo-setup
```

### Package Contents
- `swordigo_boot` — Main executable
- `asset_viewer` — Asset browser
- `swordigo-setup` — First-run data installer
- Game assets, engine binaries (v1.4.6 + v1.4.12, ARM32 + ARM64), music
- `.desktop` entry + app icon

### Dependencies
- SDL3, SDL3_image
- OpenGL (Mesa)
- Unicorn Engine
- OpenAL Soft

---

## 🔄 Upgrade Notes

- **From v6.0:** Drop-in upgrade. No data migration needed.
- **From v5.x or earlier:** Run `swordigo-setup` after install to refresh data.
- **modapi/ docs** are included in the repo but NOT in the package (development reference only).

---

## 🔮 What's Next (v7.0 Roadmap)

- [ ] Expose modding APIs to launcher mod management UI
- [ ] DPI scaling for the overlay/GUI
- [ ] Export options for asset viewer
- [ ] VFS re-enablement for seamless mod asset layering
- [ ] Mod workshop / sharing integration

---

## 👤 Credits

- **QuantumCreeper** — Lead developer
- **Swordigo** — Original game by Touch Foo
- Built with: SDL3, Dear ImGui, Unicorn Engine, OpenAL Soft
