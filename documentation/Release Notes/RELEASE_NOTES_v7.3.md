# Swordigo Desktop v7.3 Combatch Update — Release Notes

**Release Date:** July 3, 2026  
**Codename:** *Combatch Update*  
**Tag:** `v7.3`  

---

## 🎯 Overview

v7.3 Combatch Update introduces the brand new **Swordfare GUI** overlay (powered by Dear ImGui), customizable video backgrounds, key rendering optimizations, JNI touch input control locks, and dynamic host-side memory patching to break the 999 coin limit. This release adds dynamic screen-space void color matching to eliminate ugly border seams, rebinds keyboard modes, and updates the packaging ecosystem.

**Key Improvements:**
- **Swordfare GUI & High DPI Overlay** — Replaces legacy debug print panels with a beautiful, dark crimson/slate themed vector ImGui overlay. Supports high-resolution, dynamic scaling, and custom fonts.
- **Coin Limit Breaker** — Host-side machine code patches for both ARM32 and ARM64 paths to bypass the game's hard-coded 999 coin count limit.
- **UI Control Lockout** — Implements JNI touch event swallowing when `UI.SetControlsDisabled` is toggled by guest scripts (prevents accidental movements during custom overlay usage).
- **Video Background Engine** — Play dynamic 0.4x speed loop videos behind levels, toggleable via `F7`
- **Dynamic Void Correction** — Automatic boundary edge sampling to clear camera gaps with color matching instead of raw black
- **Default "Low" PostFX Preset** — Performance-focused default preset with screen-space shadows, vignette, and color-correction but zero multi-pass overhead (no Bloom, no SSAO)
- **Re-allocated Shortcuts** — Backslash (`\`) now toggles Manual Typing Mode, freeing up `F7` for video background control
- **Debian / RPM Packaging Filters** — Bundled assets exclude video files (`.mp4`) for lightweight distribution; users grab `vbg.zip` separately
- **Hardware VSync** — Hard-locked swap interval for screen-tearing prevention across displays

---

## 🆕 What's New

### 🎥 Video Background Engine (Modular Companion System)
Users can now experience animated video backgrounds in place of static texture sheets.
- **Dynamic frame-by-frame loop** playing at a cinematic 0.4x speed.
- **FFmpeg Frame Extraction** — Offloads decoding to a secure runtime shell command to keep host engine memory perfectly isolated and stable.
- **Dynamic Toggle (`F7`)** — Press `F7` at any point during gameplay to toggle video backgrounds on or off. Disabling them instantly swaps the GPU texture state back to the original vanilla assets (no freezing or pausing).
- **Directory Agnostic** — Automatically probes the level directory or a nested `background/` subfolder (e.g. `assets/resources/background/grasslandsbackground_day_2x.mp4`).

### 🎨 Dynamic Background Edge-Color Void Correction
In 2.5D games like Swordigo, jumping or falling can pan the camera beyond the borders of the sky background quad, exposing a black void at the bottom/sides.
- **Edge Sampling**: The engine now samples a 4px deep boundary around the edges of the loaded background (whether it is a static `.tex.png` or an active `.mp4` video frame).
- **State Interception**: Intercepts `glClear` calls in the JNI emulator bridge. If a background color is active and the game attempts to clear the screen with black (`glClearColor` near 0.0), it dynamically overrides the clear color with the sampled edge color.
- **Transitions**: Safely resets the state during `.scene` transitions to prevent cave maps or cutscenes from inheriting the sky-blue color.

### 💨 Optimized "Low" Default PostFX Preset
To solve the problem where running the game without post-processing looks flat, we have transformed the default (`OFF`) option into a high-performance, lightweight **"Low"** preset.
- **Enabled by Default**: Starts automatically on game boot.
- **Included Effects**: 
  - **Directional Screen-Space Shadows** (`intensity = 0.35f`, `softness = 0.002f`)
  - **Color Correction** (`warmth = 0.08f`, `contrast = 1.03f`, `saturation = 1.05f`)
  - **Vignette** (`0.15f`)
- **Performance**: Completely disables expensive passes (SSAO, Bloom, God rays / Volumetrics) to bypass extra blur and ray-marching shaders. Run-time cost is near-zero, drawing in a single compositing pass.

### ⌨️ Re-allocated Keyboard Shortcuts
To accommodate the new video background toggle:
- **`F7`**: Toggle video background playback (ON/OFF)
- **`\` (Backslash)**: Toggle Manual Keyboard Typing Mode (formerly on `F7`)

---

## 📦 Packaging & Installation Changes

To keep system packages lightweight (DEB and RPM), video backgrounds are distributed separately:
- **Exclusion Filter**: `builder/package.sh` automatically checks for local `.mp4` video background assets in your local config folders, but deletes them from the staging tree so they are **not** bundled into the final `.deb` or `.rpm` binaries.
- **User Installation**: Users can download the `vbg.zip` companion file directly from the GitHub releases page, and extract it into their local user data folder:
  `~/.local/share/swordigo-desktop/assets/resources/background/`

---

## 📝 Technical Changes

### Architecture Changes

| Component | Change | Impact |
|-----------|--------|--------|
| **PostFX Default** | Off $\rightarrow$ Low | Enhanced shadows and colors by default with zero performance cost |
| **Clear Color** | Intercepted glClear | Automatically fills empty border seams with matching color |
| **Hotkeys** | Rebound F7 / Backslash | typing mode reallocated, video toggle mapped to F7 |
| **Packaging** | Exclude `*.mp4` files | Package sizes remain clean; custom videos loaded dynamically |

### Files Changed

**Modified:**
- `src/platform/video_background.h`
  - Added global `g_video_background_enabled` state.
  - Added `reset_void_fill_color()` C-linkage helper.
- `src/platform/video_background.cpp`
  - Switched asset lookups to use `get_data_path()` for dev/prod directory safety.
  - Implemented dynamic edge-color boundary sampling for vanilla `.tex.png` assets.
  - Implemented `reset_void_fill_color()` to clear active state on scene load.
  - Implemented GPU fallback texture re-upload on video disable to prevent frame freezing.
- `src/jni/jni_bridge.cpp` & `src/jni/jni_bridge_arm64.cpp`
  - Intercepted `bridge_gl_clear` to conditionally override the clear color with `g_void_fill_color` before screen clear.
- `src/android/asset_manager.c`
  - Hooked `.scene` file openings to call `reset_void_fill_color()` on level load.
- `src/main.cpp`
  - Integrated `platform/video_background.h` and default-initialized PostFX at boot.
  - Re-mapped key handlers for `SDLK_BACKSLASH` (typing) and `SDLK_F7` (video background).
  - Updated UI text legends.
- `builder/package.sh`
  - Updated package version to `7.3.0` (Hotfix III).
  - Added packaging filters to purge `.mp4` background files during staging.
  - Updated Spec and Deb metadata.

---

## 🔄 Upgrade Notes

- **From v7.2 or earlier:** Drop-in upgrade. Run your system package manager or build script.
- **Video Background Setup**: Extract the release `vbg.zip` contents into `~/.local/share/swordigo-desktop/assets/resources/background/`.
