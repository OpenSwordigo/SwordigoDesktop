# Implementation Plan - Caver Engine Interface & Texture Fixes

This document outlines the changes made to resolve broken entities (pickups, chests), dead mod features, incorrect custom GUI rendering in 3D space, and memory heap leaks.

## Proposed Changes

### [Dynamic Engine Function Resolution System]

#### [MODIFY] [sre_caver.h](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.h)
- Declare dynamic function pointer variables for all required global game engine functions (including `ComponentWithInterface`, health bar setters, mana bar setters, coin bar setters, game overlay view controls hidden/use button setters, GUI effect fade functions, and character controller actions/queries).

#### [MODIFY] [sre_caver.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.c)
- Define the dynamically resolved function pointer variables.

#### [MODIFY] [main.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/main.cpp)
- Add a boot-time symbol resolution loop that resolves all global engine functions by their mangled names and writes the resolved addresses to the corresponding SRE variables inside `libsre.so`.

#### [MODIFY] [sre_scene_update.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_scene_update.c)
- Replace all hardcoded function offsets and `FN` macro calls with direct calls through the dynamically resolved function pointer variables.

#### [MODIFY] [sre_mini_api.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_mini_api.c)
- Redefine `CC_VOID` and `CC_BOOL` macros to call through dynamically resolved function pointer variables.
- Replace all hardcoded movement and interaction offsets with dynamic pointers.

#### [MODIFY] [sre_gui_native.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_gui_native.c)
- Call `g_sre_GameOverVC_DidContinue` directly in `ShowAdMaybe` to avoid hardcoded continue offsets.

### [JNI Bridge & Memory Allocator]

#### [MODIFY] [jni_bridge_arm64.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/jni/jni_bridge_arm64.cpp)
- Swap back width and height arguments in `PVRTTextureLoadFromPVRBuffer`:
  - `out_width_ptr` (X6 = `this + 0x3c`) -> writes `width`.
  - `out_height_ptr` (X7 = `this + 0x40`) -> writes `height`.
- Change the heap memory recycling threshold to `2048ULL * 1024 * 1024` (2GB) to allow large heaps while preventing leaks and stutters.

---

## Verification Plan

### Manual Verification
- Compile and run the desktop port with the ARM64 guest library.
- Verify that entity systems (pickups, chests, events) and overlay mod features function correctly.
- Verify that custom 3D GUIs and dynamic spotlights are rendered perfectly.
- Verify that continuing after death works without crashing.
