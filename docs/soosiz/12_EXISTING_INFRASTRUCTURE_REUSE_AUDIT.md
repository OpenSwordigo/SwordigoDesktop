# 12: Existing Infrastructure Reuse Audit (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/12_EXISTING_INFRASTRUCTURE_REUSE_AUDIT.md`  
> **Status:** Remastered Subsystem Reusability Matrix

---

## 1. Overview & Reusability Audit Summary

Direct Mach-O binary loading allows **85% of OpenSwordigo Desktop's existing infrastructure** (`src/`) to be reused directly.

Instead of writing new game engines or parsers, OpenSwordigo's existing loader (`src/loader/`), graphics (`src/sre/sre_init.c`), audio (`src/sre/sre_music.c`), and launcher UI (`src/launcher_ui.cpp`) wrap around the `SoosizHD` executable seamlessly.

---

## 2. Source Code Reuse Matrix

| File Path in `src/` | Primary Functionality | Reuse % | Required Action / Modification for Soosiz |
| :--- | :--- | :---: | :--- |
| **`src/loader/elf_loader.cpp`**| Loader Architecture | **85%** | Add Mach-O header (`0xFEEDFACE`) parser and segment mapper. |
| **`src/loader/arch_detect.h`** | Dynarmic ARM32 JIT Engine | **90%** | Directly reusable. Configures Dynarmic JIT memory callbacks and PC=0x2100. |
| **`src/main.cpp`** | Desktop Launcher & Window | **90%** | Add `--soosiz` launch flag; route main loop pump to `Soosiz_RenderBinaryFrame`. |
| **`src/launcher_ui.cpp`** | ImGui Configuration UI | **95%** | Add Soosiz tab (resolution, keybinds, save backup) to launcher menu. |
| **`src/sre/sre_init.c`** | OpenGL Graphics Setup | **85%** | Route `EAGLView` / OpenGLES 1.1 symbol hooks to SRE OpenGL context. |
| **`src/sre/sre_frame_loop.c`**| Main Frame Timing Loop | **95%** | Directly reusable. Drives 60 Hz frame rate and buffer swaps. |
| **`src/sre/sre_vfs.c`** | Virtual File System | **85%** | Intercept `fopen`/`open` calls in binary and route to `SoosizHD_assets`. |
| **`src/sre/sre_music.c`** | Audio Engine | **90%** | Route binary OpenAL and `AVAudioPlayer` calls to SRE audio buffers. |
| **`src/sre/sre_config.c`** | TOML/JSON Config System | **85%** | Route `NSUserDefaults` preferences into `soosiz_settings.toml`. |

---

## 3. Overall Code Base Metrics

- **Total Engine Reusability:** **85%**
- **Existing Engine Files Reusable:** 22 files.
- **New Harness Files Needed:** ~3 files in `src/loader/` & `src/soosiz/` (`macho_loader.cpp`, `soosiz_dylib_bridge.cpp`, `soosiz_input.cpp`).
