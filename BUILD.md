# 🔨 Building SwordigoDesktop from Source

A precise guide for developers to build SwordigoDesktop with the Dynarmic JIT backend.

---

## Prerequisites

### System Requirements
- **OS**: Linux x86_64 (Fedora 39+, Ubuntu 24.04+, Arch, or equivalent)
- **Compiler**: GCC 12+ or Clang 15+ with C++20 support
- **Cross-compiler**: `aarch64-linux-gnu-gcc` (for building libsre.so)
- **CMake**: 3.12+ (for building Dynarmic)
- **Make**: GNU Make

### Install Dependencies

**Fedora / RHEL:**
```bash
sudo dnf install unicorn-devel SDL3-devel SDL3_image-devel openal-soft-devel \
    mesa-libGL-devel zlib-devel libvorbis-devel gcc-aarch64-linux-gnu cmake gcc-c++
```

**Ubuntu / Debian (24.04+):**
```bash
sudo apt install libunicorn-dev libsdl3-dev libsdl3-image-dev libopenal-dev \
    libgl-dev zlib1g-dev libvorbis-dev gcc-aarch64-linux-gnu cmake g++
```

**Arch Linux:**
```bash
sudo pacman -S unicorn sdl3 sdl3_image openal mesa zlib libvorbis cmake
# Install aarch64-linux-gnu-gcc from AUR
yay -S aarch64-linux-gnu-gcc
```

---

## Clone & Build

### 1. Clone the Repository
```bash
git clone https://github.com/TheCorrectSynovian/SwordigoDesktop.git
cd SwordigoDesktop
```

### 2. One-Command Build & Run
```bash
./run_swordigo.sh
```

This script automatically:
1. Builds Dynarmic JIT from source (first time only, ~2 minutes)
2. Compiles SwordigoDesktop with `DYNARMIC=1`
3. Builds the asset viewer
4. Installs `libsre.so` to all engine directories
5. Launches the game

### Alternative: Manual Build Steps

#### Step 1: Build Dynarmic JIT (first time only)
```bash
make dynarmic-build
```
This runs CMake + Make on `deps/dynarmic/`, producing static libraries in `deps/dynarmic/build/`. Takes ~2 minutes on a modern CPU.

#### Step 2: Build SwordigoDesktop
```bash
make -j$(nproc) DYNARMIC=1
```
This compiles the main binary `swordigo_boot` with Dynarmic JIT support.

#### Step 3: Build libsre.so (SRE hooks)
```bash
make libsre.so
```
Cross-compiles the Swordigo Runtime Engine for ARM64 using `aarch64-linux-gnu-gcc`.

#### Step 4: Build Asset Viewer (optional)
```bash
make asset_viewer
```

---

## Build Targets

| Target | Command | Description |
|--------|---------|-------------|
| `swordigo_boot` | `make -j$(nproc) DYNARMIC=1` | Main executable with Dynarmic JIT |
| `swordigo_boot` | `make -j$(nproc)` | Main executable with Unicorn only |
| `libsre.so` | `make libsre.so` | SRE hooks (ARM64 cross-compiled) |
| `asset_viewer` | `make asset_viewer` | PVR/PNG/audio/scene browser |
| `dynarmic-build` | `make dynarmic-build` | Build Dynarmic from source |
| `dynarmic-clean` | `make dynarmic-clean` | Remove Dynarmic build artifacts |
| `clean` | `make clean` | Remove all build artifacts |
| `install-sre` | `make install-sre` | Install libsre.so to engine dirs |

---

## Game Assets Setup

SwordigoDesktop does **not** distribute game assets. You must provide your own from a legally obtained copy of Swordigo.

### Required Files
```
~/.local/share/swordigo-desktop/
├── engine/
│   └── v1.4.12/
│       └── arm64-v8a/
│           ├── libswordigo.so    ← Game binary (from APK)
│           └── libsre.so         ← SRE hooks (built by us)
├── assets/
│   └── resources/                ← Game assets (from APK)
│       ├── *.pvr                 ← Textures
│       ├── *.POD                 ← 3D models
│       ├── *.scene               ← Scene definitions
│       ├── *.scl                 ← Lua scripts (compiled)
│       └── ...
├── save/                         ← Save data (auto-created)
└── cache/                        ← Cache (auto-created)
```

### Extract from APK
```bash
# Extract assets from the Swordigo APK
unzip Swordigo.apk -d swordigo_extracted

# Copy game binary
mkdir -p ~/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/
cp swordigo_extracted/lib/arm64-v8a/libswordigo.so \
   ~/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/

# Copy game assets
mkdir -p ~/.local/share/swordigo-desktop/assets/
cp -r swordigo_extracted/assets/* ~/.local/share/swordigo-desktop/assets/
```

---

## Run Options

```bash
# Default: Dynarmic JIT
./run_swordigo.sh

# Unicorn interpreter (fallback)
./run_swordigo.sh --no-dynarmic

# Skip build, just run
./run_swordigo.sh --no-build

# Clean rebuild
./run_swordigo.sh --clean

# Only rebuild SRE
./run_swordigo.sh --sre-only

# Direct execution (no build)
./swordigo_boot                    # Uses launcher to select engine
./swordigo_boot --dynarmic         # Force Dynarmic
./swordigo_boot --vulkan           # Force Vulkan renderer
```

---

## Packaging

### RPM (Fedora)
```bash
cd builder/rpm
./build_rpm.sh
# Output: ~/rpmbuild/RPMS/x86_64/swordigo-desktop-7.1.0-1.x86_64.rpm
```

### DEB (Ubuntu/Debian)
```bash
cd builder/deb
./build_deb.sh
# Output: swordigo-desktop_7.1.0-1_amd64.deb
```

### AppImage
```bash
cd builder/appimage
./build_appimage.sh
# Output: Swordigo-x86_64.AppImage
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `libunicorn.so not found` | Install `unicorn-devel` (Fedora) or `libunicorn-dev` (Ubuntu) |
| `SDL3 not found` | Install SDL3 dev packages or build from source |
| `aarch64-linux-gnu-gcc not found` | Install the ARM64 cross-compiler for building libsre.so |
| `Dynarmic build fails` | Ensure CMake 3.12+ and a C++20 compiler are installed |
| Black screen on launch | Check that game assets are in the correct directory |
| No audio | Install OpenAL Soft: `openal-soft-devel` (Fedora) or `libopenal-dev` (Ubuntu) |

---

## Architecture Overview

```
SwordigoDesktop/
├── src/
│   ├── main.cpp                 ← Main entry point + game loop
│   ├── platform/
│   │   ├── emulator_dynarmic64.cpp  ← Dynarmic JIT backend
│   │   ├── emulator_arm64.cpp       ← Unicorn backend
│   │   ├── launcher_ui.cpp          ← ImGui launcher
│   │   └── fbo_scaler.cpp           ← FBO rendering pipeline
│   ├── jni/
│   │   ├── jni_bridge_arm64.cpp     ← ARM64 bridge (200+ functions)
│   │   └── jni_bridge.cpp           ← ARM32 bridge
│   ├── sre/
│   │   ├── sre_mini_api.c           ← SRE Lua API (30+ hooks)
│   │   ├── sre_gui_native.c         ← Native GUI rendering
│   │   └── sre_music.c              ← Music system replacement
│   └── loader/
│       ├── elf_loader_arm64.cpp     ← ARM64 ELF parser
│       └── elf_loader.cpp           ← ARM32 ELF parser
├── deps/
│   └── dynarmic/                    ← Dynarmic JIT source (included)
├── engine/
│   └── manifest.json                ← Binary discovery manifest
├── web/                             ← Project website
├── Makefile                         ← Build system
├── run_swordigo.sh                  ← One-shot build + run
└── BUILD.md                         ← This file
```
