# ⚔️ Swordigo Desktop v8.0 Beta 1 Release Notes

Welcome to the **v8.0 Beta 1** release of Swordigo Desktop! This monumental update fundamentally reshapes the modding ecosystem and runtime stability of the engine. By building a complete host-side Virtual File System (VFS), intercepting low-level graphic requests, and rewriting core guest routines natively in C, we've broken past decades-old engine limitations.

---

## 🌟 Major Highlights

### 1. 📂 Universal Mod Virtual File System (VFS)
The entire Android asset loading pipeline (`AAssetManager` and `fopen`) has been intercepted and bridged to the host. Assets are now loaded through a 5-layer prioritized hierarchy:
1. `mods/<active_mod>/resources/<profile>/`
2. `mods/<active_mod>/resources/`
3. `resources/<profile>/`
4. `resources/`
5. `assets/resources/` (Vanilla Fallback)

This enables safe, non-destructive modding. **Mod assets instantly override vanilla assets without replacing files.**

### 2. 🗜️ Native Uncompressed PVR & PVRTC Decoding
The guest engine's native iOS asset routines have been fully restored on PC:
- **PVR v3 + RGBA/BGRA:** Fonts and shadow atlases render flawlessly using our new host-side PVR extractor. Aspect ratio corruption is fixed.
- **PVRTC Extraction:** Raw iOS PVRTC textures are now seamlessly decompressed on the host using an integrated PowerVR decoder, uploading clean pixels directly to OpenGL. No more black character silhouettes or missing models!
- **Universal Format Fallbacks:** When a `.tex.png` (compressed PVR) fails to load, the engine automatically falls back to `.pvr`, and seamlessly scans `_2x` (retina) vs `_1x` variants. 

### 3. ⌨️ SwKiwi: Native UI Overlays (ImGui Vector Buttons)
The legacy Android-based `ButtonController` and `OverlayController` have been ported natively!
- Custom Lua mods can now spawn fully interactive ImGui window overlays on top of the game.
- Overlays are **draggable** and **pinchable** (mouse-scroll to zoom).
- **Input Gating:** When hovering or clicking an active SwKiwi button, game inputs are seamlessly swallowed (`is_input_blocked`), preventing your character from swinging their sword while using a menu.
- Complete support for layout confinement, label margin padding, and gravity alignment constraints.

### 4. 📜 True Lua Console & SRE Native Invocation
The Lua execution environment has been unleashed:
- Replaced the broken shared-memory `print()` redirect. Lua `print` works seamlessly without crashing.
- **`caver.call()` API**: You can now execute *any* native ARM64 engine function directly from the Lua console using Ghidra offsets! (e.g. `caver.call(caver.getBase() + 0x111533, hero)` to trigger native death).
- Raw memory Read/Write APIs (`caver.read32`, `caver.write8`) grant 100% control over the runtime state to scripts.

### 5. ⚙️ TOML Mod Manifests
We have integrated **TOMLC** directly into the engine startup. Mod profiles (`mini.toml`) are parsed immediately on boot to configure custom mod identities, version strings, coin limits, and game speeds before the VFS activates.

### 6. 🛡️ Isolated ARM32 Asset Emulation
To ensure perfect backward compatibility for legacy 32-bit `armeabi-v7a` mods, the ARM32 asset manager has been completely decoupled from the SRE pathing logic. ARM32 now runs using an isolated, stable original-asset manager implementation.

---

## 🛠️ Bug Fixes

- Fixed `video_background.cpp` EGL surface leaks (audited).
- Fixed the EGL Extension string advertisement typo (`GL_OES_compressed_ETC1_texture_data` → `GL_OES_compressed_ETC1_RGB8_texture`), ensuring hardware decoding works for ETC1 textures.
- Fixed the `SetResourcesPath` conflict where `menu.scene` was erroneously mapped as a directory, causing recursive `resources/resources/` path breakages.
- Fixed `FileExistsAtPath` to genuinely query the host ext4 filesystem instead of blindly returning `1` and forcing the engine into blind alleys.

---

**Happy Modding!**
— *TheCorrectSynovian & The SRE Team*
