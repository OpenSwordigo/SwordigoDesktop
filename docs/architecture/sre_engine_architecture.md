# Architecture — Swordigo Runtime (SRT)

> **Scope**: This document describes the complete architecture of Swordigo
> Desktop — how an ARM Android game runs on an x86_64 Linux desktop through
> CPU emulation, JNI bridging, function hooking, and graphics translation.
>
> **Key source files**:
> - [main.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/main.cpp) — boot sequence, game loop, SRE integration
> - [sre.h](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre.h) — SRE public API
> - [sre_init.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_init.c) — hook table, initialisation
> - [emulator_arm64.h](file:///home/quantumcreeper/SwordigoDesktop/src/platform/emulator_arm64.h) — ARM64 emulator wrapper
> - [elf_loader_arm64.h](file:///home/quantumcreeper/SwordigoDesktop/src/loader/elf_loader_arm64.h) — ELF loader
> - [jni_bridge_arm64.h](file:///home/quantumcreeper/SwordigoDesktop/src/jni/jni_bridge_arm64.h) — JNI function bridges
> - [fbo_scaler.h](file:///home/quantumcreeper/SwordigoDesktop/src/platform/fbo_scaler.h) — FBO + PostFX pipeline

---

## 1. Overview

Swordigo Desktop runs an unmodified Android ARM game binary (`libswordigo.so`)
on an x86_64 Linux host through **CPU emulation**. The architecture has three
main layers:

```
┌─────────────────────────────────────────────────────────┐
│                    Host (x86_64 Linux)                   │
│                                                         │
│  swordigo_boot          SDL3 Window     OpenGL/Vulkan   │
│  ├── ELF Loader         ├── Input       ├── FBO         │
│  ├── Unicorn/Dynarmic   ├── Events      ├── PostFX      │
│  ├── JNI Bridge         └── Display     └── Upscaler    │
│  └── SRT Overlay                                        │
│                                                         │
├─────────────────────────────────────────────────────────┤
│              CPU Emulator (AArch64 / ARM32)             │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │               Guest Memory (3.5 GB)               │  │
│  │                                                   │  │
│  │  libswordigo.so    libsre.so    JNI structs      │  │
│  │  @ 0x1000000       @ 0x2000000  @ 0x10000        │  │
│  │                                                   │  │
│  │  ← ARM64 code executes here via JIT/Emulation →   │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
├─────────────────────────────────────────────────────────┤
│                   Bridge Layer                           │
│                                                         │
│  ~400 bridge functions at 0xFF000000                    │
│  Map Android APIs → host implementations:               │
│  ├── GLES 1.x → host OpenGL calls                      │
│  ├── OpenSL ES → host OpenAL calls                     │
│  ├── Bionic libc → host glibc wrappers                  │
│  └── JNI methods → host-side handlers                   │
└─────────────────────────────────────────────────────────┘
```

---

## 2. Components

### 2.1 swordigo_boot (Host Executable)
The main executable compiled for x86_64 Linux with GCC/G++. Responsibilities:
- **ELF Loader**: Parses ARM ELF shared libraries, maps segments into guest memory, performs relocations.
- **Dynarmic / Unicorn Engine**: High performance ARM64 JIT or Unicorn fallback interpreter.
- **JNI Bridge**: Intercepts guest calls to Android APIs (GLES, OpenSL, JNI, libc) and executes them natively.
- **Display & FBO Pipeline**: SDL3 window management, input handling, GLSL post-processing.
- **SRT Overlay**: ImGui-based debug overlay (F1), controls editor (F2), Lua console (backtick).
- **SRE Integration**: Loads `libsre.so` into guest memory, installs trampolines.

### 2.2 libswordigo.so (Guest Binary)
The original Android game library compiled for ARM containing:
- **Caver Engine**: Touch Foo's proprietary game engine (C++).
- **Lua 5.1**: Compiled with C++ linkage.
- **Game Logic**: Scenes, entities, AI, physics, rendering.

### 2.3 libsre.so (Swordigo Runtime Engine — Guest Library)
An ARM64 shared library compiled with `aarch64-linux-gnu-gcc` running inside guest memory (`0x2000000`) that replaces problematic functions in `libswordigo.so` with clean, non-atomic, single-threaded implementations.
