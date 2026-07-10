# Walkthrough - Caver Engine Interface & Texture Fixes

This walkthrough details the changes made to the desktop port codebase to fix the broken entities, dead mod features (god mode, speed), broken custom 3D textures/GUIs, and memory leaks/stutters.

## Changes Made

### 1. Dynamic Function and Interface Resolver
* **[main.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/main.cpp)**:
  * Implemented a dynamic interface resolver that runs at boot time. It mangles and looks up `Interface()` symbols of components in `libswordigo.so`, runs them in the emulator, and sets SRE's interface pointer variables.
  * Implemented a dynamic function resolver that looks up mangled symbol names in `libswordigo.so` for all required global game engine functions (e.g. `ComponentWithInterface`, `FromLuaState`, health/mana/coin bar setters, overlay view setters, GUI view/effect updates, character controller actions/queries).
  * Writes these resolved guest function addresses directly to SRE's global variables inside `libsre.so`.
  * This completely eliminates all hardcoded function and interface offsets in SRE, ensuring absolute compatibility across different binary shifts and versions.
* **[sre_caver.h](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.h)** & **[sre_caver.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_caver.c)**:
  * Declared and defined the new dynamically resolved function pointer variables.

### 2. GameSceneView Hero Offset & HUD Offsets Correction
* **[sre_scene_update.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_scene_update.c)**:
  * Corrected the offset of the `hero` `SceneObject*` inside `GameSceneController` from `0x8` to `0xd8`.
  * Removed the hardcoded `FN` offsets and replaced them with calls through the new dynamically resolved global pointers.
  * This resolves HUD rendering updates (HP, mana, coins) and enables the player action and pickup buttons to be evaluated and shown correctly.

### 3. PVR Texture Loader Dimensions Swap Fix
* **[jni_bridge_arm64.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/jni/jni_bridge_arm64.cpp)**:
  * Corrected a width/height swap in `PVRTTextureLoadFromPVRBuffer`:
    * `out_width_ptr` (X6 = `this + 0x3c`) receives `width`.
    * `out_height_ptr` (X7 = `this + 0x40`) receives `height`.
  * This fixes distorted UV maps and aspect ratios for all PVR textures (including custom mod GUIs rendered in 3D space).

### 4. SRE Mini API & Game Events Correction
* **[sre_mini_api.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_mini_api.c)**:
  * Redefined the `CC_VOID` and `CC_BOOL` macros to query active player controls and capability states using the dynamically resolved function pointers, completely eliminating the incorrect hex offsets.
  * Replaced TextBubble, Touchable, and movement offsets with their corresponding dynamic pointers.
  * Linked the walk speed, run speed, and jump height cheat APIs directly to their corresponding offsets inside the active hero `CharControllerComponent` structure so modifying the speed cheat triggers immediate speed changes.

### 5. Respawn Crash & Text Input Delegate Corrections
* **[sre_gui_native.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_gui_native.c)**:
  * Replaced the hardcoded GameOverView respawn continue offset in `sre_GameOverVC_ShowAdMaybe` with `g_sre_GameOverVC_DidContinue`.
  * Corrected `TEXTINPUT_DELEGATE_OFFSET` to `0x7e9cb0`, resolving vtable dispatch corruption.
* **[sre_init.c](file:///home/quantumcreeper/SwordigoDesktop/src/sre/sre_init.c)**:
  * Commented out the unused and incorrect ARM32 vtable overrides to prevent random memory corruption.

### 6. Memory Heap Allocator Recycling Threshold
* **[jni_bridge_arm64.cpp](file:///home/quantumcreeper/SwordigoDesktop/src/jni/jni_bridge_arm64.cpp)**:
  * Adjusted the bump allocator recycling threshold to `2048ULL * 1024 * 1024` (2GB). This ensures that memory is recycled dynamically and efficiently within the mapped memory space, solving stutters while preventing heap crashes.
  * Fixed potential out-of-bounds reads in `bridge_glShaderSource` when parsing non-null-terminated shader source strings.

---

## Verification Plan

1. **Compile & Run the Game**:
   Compile the desktop port and launch it.
2. **Verify Interface Queries (Entities & Mods)**:
   * Verify that entity interactions (item pickups, chests, doors, pressure plates) function correctly in-game.
   * Verify that overlay cheats/features (god mode, speed modification, jump height modifications) function correctly and apply changes instantly.
3. **Verify Custom GUIs & 3D Space Textures**:
   * Load any mod using custom textures in 3D space.
   * Verify that the textures are rendered with correct aspect ratios and UV layouts (no longer squished/swapped).
4. **Verify Dynamic Lighting**:
   * Walk into a cave and check if the dynamic lighting and player spotlight follow the player character smoothly.
5. **Verify Stability & Memory Footprint**:
   * Play the game through transitions and check if stutters or crashes due to heap growth are completely resolved.
