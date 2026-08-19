# SwordigoDesktop & Swordigo Runtime Engine (SRE)

Standard Binary Compatibility Layer, ARM64 JIT Execution Runtime, and Software Development Kit for Linux.

---

## 1. Overview

**SwordigoDesktop** is a open-source native Linux compatibility layer, reverse-engineering SDK, and modding toolchain for Touch Foo's *Swordigo*.

Instead of relying on heavy OS virtualization or high-overhead Android emulators, SwordigoDesktop implements a hybrid binary-translation architecture:
* **Host Engine**: Parses and maps original ARM64 ELF binaries (`libswordigo.so`) into a contiguous 64-bit virtual memory space.
* **JIT CPU Acceleration**: Executes ARM64 machine instructions using **Dynarmic**, a high-performance A64 Just-In-Time compiler configured with 512MB code cache allocation and targeted x86_64 FMA fast-paths.
* **API Bridge Layer**: Intercepts Android Bionic libc calls, GLES 1.1/2.0 graphics routines, OpenSL audio, and JNI methods, routing them directly to host-native SDL3, OpenGL, and OpenAL Soft APIs.
* **Guest Trampoline Runtime (SRE)**: Injects `libsre.so` at guest address `0x2000000` via 16-byte ARM64 function trampolines to replace legacy GNU atomic string spinlocks, thread contention bottlenecks, and mobile rendering restrictions.

---

## 2. System Architecture

```
+-----------------------------------------------------------------------+
|                         Host Process (x86_64 Linux)                   |
|                                                                       |
|  swordigo_boot          SDL3 Window          OpenGL / Vulkan Pipeline |
|  |-- ELF Loader         |-- Input Events     |-- FBO Viewport Scaler  |
|  |-- JNI Bridge         |-- Keybindings      |-- PostFX (SSAO, Rays)  |
|  `-- ImGui Overlay      `-- Display Config   `-- Upscaler (FSR/Sharp) |
|                                                                       |
+-----------------------------------------------------------------------+
|                  Dynarmic A64 JIT CPU Core (512MB Cache)              |
|                                                                       |
|  +-----------------------------------------------------------------+  |
|  |                     Guest Address Space (3.5 GB)                |  |
|  |                                                                 |  |
|  |  libswordigo.so (0x1000000)   libsre.so (0x2000000)  JNI Structs|  |
|  |                                                                 |  |
|  |  <-- ARM64 Instructions Translated via Dynarmic JIT Core -->   |  |
|  +-----------------------------------------------------------------+  |
|                                                                       |
+-----------------------------------------------------------------------+
|                       Bridge & Trampoline Layer                       |
|  ~400 JNI & Native Function Bridges mapped to Host C++ implementations|
+-----------------------------------------------------------------------+
```

---

## 3. Subsystem Breakdown

### 3.1 Host Frontend & Binary Loader (`src/loader/`)
* **ELF Parsing (`elf_loader_arm64.cpp`)**: Parses ELF64 headers, maps `PT_LOAD` segments with page-aligned permissions, and resolves `R_AARCH64_RELATIVE`, `R_AARCH64_GLOB_DAT`, and `R_AARCH64_JUMP_SLOT` relocations.
* **Virtual Memory Map**: Allocates a single 3.5GB page-aligned host buffer (`calloc`) ensuring low memory alignment.

### 3.2 CPU Execution Core (`src/platform/emulator_dynarmic64.cpp`)
* **JIT Tuning**: Configured with 512MB code cache to eliminate JIT block re-compilation stalls.
* **Unsafe Fast-Paths**: Enables `Unsafe_UnfuseFMA` to map ARM NEON FMA instructions to native x86 hardware FMA, and `Unsafe_InaccurateNaN` to bypass ARM canonical NaN bit pattern transformations.
* **Instruction Cache Invalidation**: Listens for `IC IVAU` / `IC IALLU` system instructions via `InstructionCacheOperationRaised` callbacks to invalidate JIT code blocks dynamically.

### 3.3 SRE Guest Runtime (`src/sre/`)
* **Non-Atomic String Replacements (`sre_string.c`)**: Replaces GNU libstdc++ copy-on-write `std::string` atomic refcounting (`LDAXR`/`STLXR`) with single-threaded direct integer operations, eliminating exclusive monitor spinlocks.
* **GLES Render Hooking (`sre_background.c`, `sre_gui_native.c`)**: Intercepts immediate matrix calls and raw CPU vertex arrays, converting them into modern GPU-side Vertex Buffer Objects (VBO).

### 3.4 FBO Viewport Scaler & PostFX (`src/platform/fbo_scaler.cpp`)
* Decouples native 960x544 game rendering from host desktop display resolution.
* **Upscaling Algorithms**: AMD FidelityFX Super Resolution (FSR 1.0), Sharp Bilinear, CRT Scanline simulation.
* **Post-Processing Pass**: Multi-pass Screen Space Ambient Occlusion (SSAO), 64-sample radial god rays, bloom extraction, and chromatic aberration.

---

## 4. Building and Toolchain Setup

### 4.1 System Prerequisites

#### Debian / Ubuntu:
```bash
sudo apt update
sudo apt install build-essential gcc-aarch64-linux-gnu \
    libsdl3-dev libsdl3-image-dev libgl-dev \
    libunicorn-dev libopenal-dev libvorbis-dev zlib1g-dev pkg-config
```

#### Fedora / RHEL:
```bash
sudo dnf install gcc gcc-c++ gcc-aarch64-linux-gnu \
    SDL3-devel SDL3_image-devel mesa-libGL-devel \
    unicorn-devel openal-soft-devel libvorbis-devel zlib-devel pkg-config
```

### 4.2 Building from Source

```bash
# Clone the repository
git clone https://github.com/TheCorrectSynovian/SwordigoDesktop.git
cd SwordigoDesktop

# Compile host executable and ARM64 guest library
make -j$(nproc)

# Launch the runtime environment
./run_swordigo.sh
```

---

## 5. Keybindings and Control Mapping

| Key | Action | Subsystem |
| :--- | :--- | :--- |
| **WASD** / **Arrow Keys** | Character Movement | Game Input |
| **Space** / **W** | Jump / Double Jump | Game Input |
| **J** / **Z** | Sword Attack | Game Input |
| **K** / **X** | Magic Spell | Game Input |
| **I** | Inventory & Item Management | Game Input |
| **Escape** | Pause Menu / Configuration | Game Input |
| **F1** | Toggle SRT Debug Overlay | Host Overlay |
| **F4** | Cycle Viewport Upscaler Mode | FBO Scaler |
| **F8** | Pause / Resume CPU Emulation | Emulator Core |
| **`** (Backtick) | Open Raijin Interactive Lua Debug Console | Developer Console |

### Runtime stability status

Previously reported Swordfare startup crashes caused by unresolved FFmpeg symbols in `libswgfx.so` are fixed in the current build. Scene transitions, launcher video startup, and Ruby asset resolution have also received dedicated hardening. If an older binary still exits with `undefined symbol: avformat_open_input`, rebuild all targets instead of reusing libraries from an earlier package.

The Scene & Display Toolbox is available with **F11**. It includes responsive render/output presets for 4:3, 5:4, 16:9, 16:10, ultrawide, laptop, native Android, and low-performance resolutions, plus custom dimensions and exact fullscreen display modes.

---

## 6. Technical Documentation Index

All technical documentation, reverse-engineering analyses, and API specifications are unified in the [`docs/`](docs/README.md) directory:

* **[Architecture Specifications](docs/architecture/)**: Engine architectural designs, SRE platform master plans, Yuzu Dynarmic JIT research, Linux ELF loader specifications.
* **[ARM64 & Emulation](docs/emulation_and_arm64/)**: JIT correctness logs, `LDXR`/`STXR` exclusive monitors, ELF relocation inventories, memory layout audits.
* **[Graphics & Rendering](docs/graphics_and_rendering/)**: FBO viewport scaler design, GLES2 pipeline plan, PostFX remastering plans, GPU thread architecture.
* **[APIs & Subsystems](docs/apis_and_subsystems/)**: Native SRE hook reference (34 active hooks), Lua scripting catalog, GUI overlay APIs, VFS virtual filesystem.
* **[Formats & Schemas](docs/formats_and_schemas/)**: PowerVR POD 3D model spec, PVR texture spec, Scene spec, Protobuf wire schema, Save file spec.
* **[Modding & SwKiwi](docs/modding_and_swkiwi/)**: SwKiwi modloader architecture, RLSwordigo 7.0 reversing, modding guides, API audits.
* **[Release Notes](docs/release_notes/)**: Version release notes from v1.0.0 through v8.0 Beta 2.
* **[Misc & Logs](docs/misc/)**: Diagnostic traces, platform compatibility matrices, build guides.

---
## 7. Contributors & Credits
Super thanks to our awesome developer's who made this project possible! OpenSwordigo is nothing without them.
* ManoK , MrSinup ,TheCorrectSynovian , Raijin - Direct Contributors
* Daniel Spaniel , Its Just Some Dude , Kizion - Contribution to Swordigo Reverse engineering
* SwordiForge Community - Keeping the modding scene alive upto date!
* Coropatasy and Redstell - Helping out to create stylesheet's for FileRift format

### OPENSOURCE PROJECTS WHICH BUILT THE FOUNDATION FOR MODDING SWORDIGO ###
* Filerift by Daniel Spaeniel
* SwMini/SwKiwi - IJSD/Kizion

---

## 8. License and Copyright

* **SwordigoDesktop Engine & SRE Codebase**: Released under the [GNU General Public License v2.0 or later](LICENSE).
* **ufbx** (FBX importer, [Click to get!](https://github.com/ufbx/ufbx)): MIT-licensed single-file FBX loader, vendored in `src/tools/ufbx/`.
* **Swordigo Game Assets**: Original game assets, binaries, and trademarks remain the intellectual property of Touch Foo / Ville Mäkynen. This compatibility layer requires user-supplied game data files.
