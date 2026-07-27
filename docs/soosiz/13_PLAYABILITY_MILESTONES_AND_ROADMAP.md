# 13: Playability Milestones & Integration Roadmap (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/13_PLAYABILITY_MILESTONES_AND_ROADMAP.md`  
> **Status:** Remastered Direct Binary Loading Integration Roadmap  
> **Target Executable:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & Remastered Timeline

This document establishes a **6-Phase, 28-Developer-Day Implementation Plan** for directly loading and executing the native **`SoosizHD`** Mach-O binary within **OpenSwordigo Desktop**.

Direct binary execution reduces total project timeline from 38 days down to **28 days** by eliminating code translation, decompilation cleanup, and custom parser development.

---

## 2. Remastered Roadmap Schedule

```
Phase 0: Mach-O Loader (Days 1-5)  ==>  Phase 1: Dynarmic JIT Harness (Days 6-11)
                                                       |
Phase 3: VFS & Graphics (Days 18-22) <== Phase 2: Dylib Symbol Bridge (Days 12-17)
       |
       V
Phase 4: Audio & Input (Days 23-25)  ==>  Phase 5: Release & Polish (Days 26-28)
```

---

## 3. Detailed Phase Breakdown

### Phase 0: Mach-O ARM32 Loader Subsystem (Days 1–5)
- **Tasks:** Build `src/loader/macho_loader.cpp`. Parse Mach-O header (`0xFEEDFACE`), map `__TEXT` (`0x1000`) and `__DATA` (`0xC8000`) segments into RAM, apply `mprotect`.
- **Target:** Load `SoosizHD` binary bytes into memory and verify segment boundaries.
- **Benchmark:** Loader parses all 26 load commands and maps 100% of binary segments cleanly.

### Phase 1: Dynarmic ARM32 JIT Harness (Days 6–11)
- **Tasks:** Configure Dynarmic ARM32 JIT in `src/loader/arch_detect.h`. Set `PC = 0x2100`, initialize stack frame at `0x700FF000`.
- **Target:** Execute initial `start()` routine in binary via Dynarmic JIT.
- **Benchmark:** JIT executes first 10,000 ARM instructions through `start()` to `main()` without memory faults.

### Phase 2: Dylib Symbol Bridge & `UIApplicationMain` Trap (Days 12–17)
- **Tasks:** Build dyld symbol interceptor (`src/soosiz/soosiz_dylib_bridge.cpp`). Intercept `libobjc.A`, `Foundation`, `OpenGLES`, `OpenAL`, and hook `UIApplicationMain`.
- **Target:** Intercept `UIApplicationMain` and instantiate `SoosizAppDelegate` and `ApplicationController` inside JIT.
- **Benchmark:** Binary executes through `viewDidLoad` with clean log output in `soosiz_boot.log`.

### Phase 3: Asset VFS & Graphics Pipeline Integration (Days 18–22)
- **Tasks:** Hook `fopen`/`open` calls to redirect asset requests to `SoosizHD_assets/`. Route GLES 1.1 render stubs to `sre_init.c`.
- **Target:** Render main menu and in-game 360-degree camera rotated scenes at 60–144 FPS.
- **Benchmark:** Visual rendering of levels, hero sprite, and rotating planet background.

### Phase 4: Audio, Input & Game Controller Binding (Days 23–25)
- **Tasks:** Hook OpenAL audio calls to `sre_music.c`. Inject SDL2 keyboard (WASD / Space) and gamepad state into `ApplicationController` touch handlers.
- **Target:** Complete interactive controls and sound effect / music playback during gameplay.
- **Benchmark:** Hero character responds smoothly to WASD / controller inputs with sound effects playing in sync.

### Phase 5: Save System & Full Playability Verification (Days 26–28)
- **Tasks:** Intercept `sqlite3_open` and `NSUserDefaults` to save in `~/.config/openswordigo/soosiz/`. Integrate Soosiz tab into OpenSwordigo Launcher (`src/launcher_ui.cpp`).
- **Target:** 100% playable game across all 7 worlds.
- **Benchmark:** Complete Level 1-1 through World 7 Boss, verified save persistence, zero crashes.

---

## 4. Effort Comparison: Static Recompilation vs Direct Binary Execution

| Integration Aspect | Static Recompilation Plan | Direct Mach-O Binary Execution Plan | Time Saved |
| :--- | :---: | :---: | :---: |
| **Parsing & Setup** | Decompiled C cleanup (121k lines) | Direct Mach-O Loader mapping | **-5 Days** |
| **Asset Loading** | Write custom `.pb` parser | Binary parses its own `.pb` natively | **-3 Days** |
| **Logic & Physics** | Debug decompilation drift | Native compiled ARM32 logic in JIT | **-2 Days** |
| **Total Effort** | **38 Days** | **28 Days** | **10 Days Faster!** |
