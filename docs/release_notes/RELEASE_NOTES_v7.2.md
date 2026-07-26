# Swordigo Desktop v7.2 Hotfix II — Release Notes

**Release Date:** July 1, 2026  
**Codename:** *Hotfix*  
**Tag:** `v7.2`  

---

## 🎯 Overview

v7.2 Hotfix II is a focused optimization and stability update bringing critical I/O improvements, a complete migration to decentralized per-instance `.ini` configuration (inspired by modern launchers like PolyMC), and expanded music format support. This release improves load times, fixes memory leaks, and adds experimental KiwiAPI button support.

**Key Improvements:**
- **Decentralized `.ini` architecture** — no more centralized JSON manifests
- **Music format expansion** — MP3, OGG, WAV all supported via direct libmpg123
- **I/O optimization** — suppressed verbose file operation logging
- **KiwiAPI Buttons & Touchables** — experimental interactive elements (early beta)
- **Modded instance testing** — RLSwordigo, Mason mod, Phonkdigo validated

---

## 🆕 What's New

### 🎵 Custom Music Format Support (Headline Fix)

**Now supports MP3, OGG, and WAV** via direct libmpg123 decoding instead of shell-based ffmpeg conversion.

| Format | Status | Details |
|--------|--------|---------|
| **MP3** | ✅ Fully supported | Direct libmpg123 decoding (no temp files) |
| **OGG Vorbis** | ✅ Fully supported | Existing libvorbis integration |
| **WAV** | ✅ Fully supported | Existing WAV loader |
| **FLAC** | ⏳ Planned | Pending libFLAC integration |

**Before:** Music search was format-specific (hardcoded paths for OGG, separate paths for MP3). Game-specific music directories were skipped for MP3.

**After:** Modular format-agnostic search — all directories checked for all formats before moving to fallback locations. Custom music in `rl_assets/music/battlesky.mp3` now loads correctly.

### 📁 Decentralized Configuration (Architecture Shift)

**Migrated from centralized JSON manifest to per-instance `.ini` files.**

```
Old (v7.0):
launcher/
  manifest.json          ← Generated during build (80+ lines per package)
  instances/
    vanilla/
    rlswordigo/

New (v7.2):
engine/
  vanilla-1.4.12/
    instance.ini         ← Self-describing, decentralized
    libswordigo.so
    assets/
  rlswordigo-6.6/
    instance.ini
    libswordigo.so
    assets/
```

**Rationale:**
- Eliminates build-time manifest generation (`generate_manifest.sh` now DEPRECATED)
- Each instance is self-contained — config + binary + assets in one directory
- Matches modern launcher architecture (PolyMC, Prism, MultiMC)
- Reduces build script complexity by ~80 lines
- More maintainable for multi-instance deployments

**File structure (instance.ini):**
```ini
[instance]
name=Vanilla Swordigo
engine_version=1.4.12
arch=arm64
binary_name=libswordigo.so
assets_dir=assets/
```

### 💨 I/O Optimization

- **Suppressed high-volume [File] logging** — fopen, fwrite, mkdir, stat, opendir logs now commented out (not deleted, for debug re-enablement)
- **Reduced console I/O overhead** — typical game session log volume down ~95% (from hundreds of file operation logs to near-silent)
- **Preserved logging infrastructure** — logs remain in code but are commented; easy to re-enable for debugging

### 🔧 KiwiAPI Extensions (Experimental)

- **KiwiAPI Buttons & Touchables** — new experimental interactive element support
- **Native GUI rendering** — buttons now support touch input mapping
- **Status:** Early beta — core features stable, edge cases under testing
- **Compatibility:** Full backward compatibility with existing KiwiAPI mods

### 🐛 Memory Leak Fixes

- Fixed libmpg123 handle cleanup — ensures proper initialization/deinitialization
- Reduced guest memory fragmentation in music loader
- Improved instance cleanup on instance switch

---

## 🏆 Stability & Mod Support

### Tested & Validated Instances

| Instance | Status | Notes |
|----------|--------|-------|
| **Vanilla Swordigo 1.4.12 ARM64** | ✅ **Fully stable** | Beatable end-to-end, all bosses, all areas |
| **RLSwordigo 6.6** | 🟡 **Near stable** | Gameplay stable; some modded features incomplete |
| **Mason Mod** | 🟡 **Near stable** | Core mechanics work; cosmetic features untested |
| **Phonkdigo** | 🟡 **Near stable** | Music & aesthetics work; edge cases pending |
| **Combatch v3** | 🔴 **Works, unstable** | Loads + runs; crashes on certain mods, not fully supported this release |

### Supported for Bug Reports

**Bug reports accepted ONLY for officially supported instances:**
- ✅ **Vanilla Swordigo 1.4.12 ARM64**
- ✅ **RLSwordigo 6.6**
- ✅ **Mason Mod**

**Unsupported:**
- ❌ ARM32 instances (no SRE support, not actively maintained)
- ❌ Combatch v3 or other unstable instances

If you encounter crashes in supported instances, **report on GitHub** with:
1. Instance name and version
2. Reproduction steps
3. Log output (console, or `~/.local/share/swordigo-desktop/.../*/` logs)

---

## 📝 Technical Changes

### Architecture Changes

| Component | Change | Impact |
|-----------|--------|--------|
| **Config System** | JSON → `.ini` per-instance | Decentralized, self-describing configs |
| **Music Loader** | Shell ffmpeg → libmpg123 direct | 10× faster MP3 load, no temp files, lower I/O |
| **Search Algorithm** | Format-specific → Format-agnostic modular | All dirs checked for all formats |
| **File Logging** | Verbose → Suppressed | 95% console I/O reduction |

### Files Changed

**Modified:**
- `src/jni/jni_bridge_arm64.cpp`
  - Refactored `sre_music_host_load()` — modular format-agnostic search (lines 6669–6815)
  - Added `load_mp3_to_buffer()` with libmpg123 direct decoding (lines 1052–1120)
  - Commented out high-volume [File] logging (fopen, fwrite, mkdir, stat, opendir)
  - Added `#include <mpg123.h>` and `#include <algorithm>`

- `Makefile`
  - Added libmpg123 pkg-config queries: `MP3_CFLAGS`, `MP3_LIBS`
  - Integrated MP3 libs into `ALL_CXXFLAGS` and `LIBS`

- `builder/package.sh`
  - Updated VERSION: 7.0.0 → 7.2.0
  - Removed ~80 lines of JSON manifest generation
  - Added per-instance `.ini` file copying during staging
  - Updated post-install script, RPM/DEB descriptions

- `builder/generate_manifest.sh`
  - **Status: DEPRECATED** — no longer used with `.ini` architecture
  - Marked with deprecation notice for potential emergency rollback

- `documentation/README.md`
  - Updated version badge to v7.2
  - Added mod support categories (Kiwi/Mini API vs SDMOD)
  - Updated credits: MrSinup, Mano K, QuantumCreeper
  - Added supported instances documentation

---

## 🔄 Upgrade Notes

- **From v7.0/v7.1:** Drop-in upgrade. No data migration. Existing `.ini` files are preserved and used directly.
- **From v6.5 or earlier:** Run `swordigo-setup` after install to refresh instance data.
- **Build:** `make -j$(nproc)` (requires libmpg123-devel)

### Dependencies (New)
- **libmpg123-devel** — MP3 decoding library
- **pkg-config** — for libmpg123 detection during build

---

## 🎮 Modding Ecosystem

Swordigo Desktop now supports **two parallel modding frameworks:**

### 1. **KiwiAPI / Mini API (Primary)**
- **Status:** Beta (actively developed, primary focus)
- **Framework:** SwKiwi/SwMini modloader hooks
- **Capabilities:** Full game hooks, custom entities, audio, GUI, saves
- **Examples:** RLSwordigo, Phonkdigo, experimental button mods
- **Focus:** This is where new mod support is prioritized

### 2. **SDMOD (Lightweight Custom)**
- **Status:** Early beta (less powerful, not actively expanded)
- **Framework:** Direct file patching + simple Lua injection
- **Capabilities:** Lightweight tweaks, cosmetics, basic behavior changes
- **Limitations:** No advanced hooks, fragile across game updates
- **Note:** Superseded by KiwiAPI for complex mods

**Recommendation:** Use **KiwiAPI/Mini API** for new mods. SDMOD is for lightweight tweaks only.

---

## ⚠️ Known Limitations

### ARM64 (arm64-v8a) — Primary Target

| Issue | Status | Workaround |
|-------|--------|-----------|
| **Bolt/timer misbehavior** | 🟡 Known | Timing differences cause certain enemies (bolt shooters, timer traps) to fire abnormally; gameplay still winnable |
| **Text input crash** | 🟡 Known | Avoid F7 (typing mode) in menus; use launcher text input for saves |

### ARM32 (armeabi-v7a)

| Issue | Status | Details |
|-------|--------|---------|
| **No SRE support** | 🟡 By design | libsre.so is ARM64 only; ARM32 runs without engine hooks |
| **Not actively maintained** | 🔴 Decision | Focus is 100% on ARM64 for performance |

---

## 👥 Credits

**v7.2 Hotfix II Development:**
- **MrSinup** — MP3 playback via libmpg123, direct decoding integration
- **Mano K** — KiwiAPI buttons & touchables, music system improvements
- **QuantumCreeper (TheCorrectSynovian)** — I/O optimization, launcher refactoring, `.ini` migration, build script updates

**Original v7.0 Foundation:**
- **TheMegineBraine** — Lead developer
- **QuantumCreeper** — Project creator, platform engineering

**Open Source & Community:**
- **Swordigo** — Original game by Touch Foo (Ville Mäkynen)
- Built with: Dynarmic, Unicorn Engine, SDL3, OpenAL Soft, libmpg123

---

## 📦 Packaging & Installation

### Pre-built Packages

| Format | Platform | Command |
|--------|----------|---------|
| `.rpm` | Fedora x86_64 | `sudo dnf install swordigo-desktop-7.2.0-1.x86_64.rpm` |
| `.deb` | Debian/Ubuntu x86_64 | `sudo dpkg -i swordigo-desktop_7.2.0-1_amd64.deb` |

### Package Contents (v7.2)
- `swordigo_boot` — Main executable (with Dynarmic JIT, libmpg123)
- `asset_viewer` — Asset browser
- `swordigo-setup` — First-run data installer
- `engine/` — Instance directories with `.ini` configs (replaces manifest.json)
- `assets/` — Game assets, resources, music
- `.desktop` entry + app icon

### Build from Source

```bash
# Install dependencies (Fedora)
sudo dnf install libmpg123-devel unicorn-devel SDL3-devel SDL3_image-devel \
    openal-soft-devel mesa-libGL-devel zlib-devel libvorbis-devel \
    gcc-aarch64-linux-gnu cmake

# Build
cd SwordigoDesktop
make -j$(nproc)
sudo make install
```

---

## 🔮 What's Next (v7.5+ Roadmap)

- [ ] ARM32 deprecation (no new features, security fixes only)
- [ ] Player name input fix (text entry crash mitigation)
- [ ] FLAC format support for music
- [ ] DPI scaling for overlay/GUI
- [ ] F11 SRT overlay re-enablement
- [ ] Advanced modding documentation for KiwiAPI

---

*Powered by the Swordigo Runtime (SRT) v7.2*

Built with ❤️ for game preservation and modding freedom.

