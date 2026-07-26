# Swordigo Linux Shared Object (.so) Loader Architecture

## 1. Overview
The goal is to load and execute the ARM `libswordigo.so` binary on an x86_64 Linux host through a combination of **ELF Loading**, **Relocation Processing**, **Symbol Resolution**, and **ARM Emulation**.

---

## 2. Loader Architecture & Core Components

```
┌─────────────────────────────────────────────────────────┐
│                    Host Loader Engine                   │
├─────────────────────────────────────────────────────────┤
│ 1. ELF Header & Program Header Parser (PT_LOAD)         │
│ 2. Relocator (R_AARCH64_RELATIVE, R_ARM_ABS32)           │
│ 3. Fake JNI Environment & Method Resolution             │
│ 4. Static Hooking Trampolines & Code Cave Bridges       │
└─────────────────────────────────────────────────────────┘
```

### 2.1 Core Components
- **ELF Loader (`elf_loader_arm64.h`)**: Manages reading the ELF header, program headers, and sections.
- **Relocator**: Handles ARM-specific relocation types (`R_AARCH64_RELATIVE`, `R_AARCH64_GLOB_DAT`, `R_AARCH64_JUMP_SLOT`).
- **Fake JNI Environment**: Fabricates a `JavaVM` and `JNIEnv` struct with function pointers pointing to native compatibility shims.
- **Static Hooking**: Uses a jump table (trampolines) to redirect native ARM calls to host-native (or emulated) C functions.

---

## 3. Implementation Details

### 3.1 Memory Mapping & Alignment
- **Segments**: `PT_LOAD` segments are mapped with exact permissions (RX for code, RW for data).
- **Alignment**: Page alignment (typically 4KB / 64KB) is strictly respected.

### 3.2 Relocations & Symbol Resolution
- **Internal Symbols**: Resolved by adding the base load address (`0x1000000`) to the symbol's `st_value`.
- **External Imports**: Redirected to "bridge" functions that handle the transition between the engine's ARM code and the host's x86_64 environment.

### 3.3 Minimum Viable Boot Sequence
To reach `setupApplication()`, the loader executes:
1. Load `libswordigo.so` and `libsre.so`.
2. Resolve relocations and bridge symbols to `0xFF000000`.
3. Call `setFilesDir(env, path)`.
4. Call `setCacheDir(env, path)`.
5. Call `setAssetManager(env, asset_mgr)`.
6. Call `setupNativeInterface(env)`.
7. Call `setupApplication(env)`.
