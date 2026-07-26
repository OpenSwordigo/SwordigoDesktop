# Building Swordigo Desktop & SRE Engine

> Build guide for the Swordigo Runtime (SRT) project — a Linux desktop port of Swordigo running the ARM64 game binary with native SDL3 rendering.

---

## 1. Prerequisites

### Required System Packages (Fedora / Debian / Ubuntu)
- **Host Compiler**: GCC / G++ (C++17 support)
- **ARM64 Cross Compiler**: `aarch64-linux-gnu-gcc` (compiles `libsre.so`)
- **Libraries**: `SDL3`, `SDL3_image`, `OpenGL` (`libGL`), `Unicorn Engine` (`libunicorn`), `Dynarmic`, `OpenAL Soft`, `libvorbisfile`, `zlib`, `pthreads`.

#### Fedora Install Command:
```bash
sudo dnf install gcc gcc-c++ gcc-aarch64-linux-gnu \
    SDL3-devel SDL3_image-devel mesa-libGL-devel \
    unicorn-devel openal-soft-devel \
    libvorbis-devel zlib-devel pkg-config
```

#### Ubuntu / Debian Install Command:
```bash
sudo apt install build-essential gcc-aarch64-linux-gnu \
    libsdl3-dev libsdl3-image-dev libgl-dev \
    libunicorn-dev libopenal-dev \
    libvorbis-dev zlib1g-dev pkg-config
```

---

## 2. Source Directory Structure
```
~/SwordigoDesktop/
├── Makefile                    # GNU Make build system
├── run_swordigo.sh             # Build + Install + Launch script
├── swordigo_boot               # Main desktop runtime binary (x86_64)
├── libsre.so                   # ARM64 guest SRE library
├── src/                        # Source codebase
│   ├── main.cpp                # Main boot sequence & loop
│   ├── jni/                    # JNI bridge marshalling
│   ├── platform/               # Display, SDL3, emulator, PostFX, FBO
│   ├── loader/                 # ELF64 loader
│   ├── render/ & audio/        # Graphics & OpenAL audio
│   └── sre/                    # SRE guest library (C11 + ARM64 ASM)
```

---

## 3. Build Commands

```bash
# Build main runtime and guest library (parallel build)
make -j$(nproc)

# Build specific components
make swordigo_boot      # Main host executable
make libsre.so          # ARM64 guest library

# Install SRE guest library to local engine cache
make install-sre

# Clean all build artifacts
make clean
```
