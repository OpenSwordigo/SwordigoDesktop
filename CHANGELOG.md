# Changelog

All notable changes to Swordigo Desktop.

## [v8.0 Beta 3] — 2026-08-06

### Added — Ruby SDK Scene Editor (2-day drop)
- **Vanilla Lighting & Atmosphere**: Light components parsed from real scene data (type/intensity/color/offset/radius) plus SimpleGlow torch/fire as warm point lights; up to 16 point lights with smooth falloff; vanilla golden-sun + deep cave-ambient defaults; Scene Lights panel + render toggle.
- **Camera Ports**: Scene opens framed at `spawn_default`; View → Camera Ports submenu lists every SpawnPoint for one-click jumps (framing distance 160, pitch 30°).
- **Background Layer Picker**: Inspector section to pick any background layer object by name/texture (Auto = first visible default).
- **Emissive Bloom for Torches**: Additive camera-facing glow billboards at Light/SimpleGlow positions feeding the PostFX bloom bright-pass.
- **Depth Fog**: Distance-based atmospheric darkening in the model shader (vanilla cave depth), camera-distance-scaled, toggleable.
- **Fluid Rendering (Water / Lava)**: WaterMesh components parsed (shape rectangle + FrontColor/SurfaceColor + texture mapping) and rendered as animated semi-transparent fluid sheets with sine-wave surface + scrolling UV; `water_2x.pvr` auto-resolved; Settings toggle.
- **High-DPI Render Scale**: 1x/1.5x/2x/3x FBO + PostFX rendering (default 3x) with logical-size UI display.
- **HiDPI Texture Filtering**: Trilinear mipmaps + 4x anisotropy on uncompressed texture paths (compressed ETC1/PVRTC kept GL_LINEAR).
- **Ground Mesh Generator Rework**: Mesh tab hides the right inspector panel and donates full width to the editor; live 3D preview enlarged; preview textures now probe `_2x.pvr` variants (fixes white preview); GM depth editing.
- **Template/Proxy Cleanup**: Purely non-visual objects (Light/Portal/CollisionShape/SpawnPoint/controllers) render as tiny neutral markers instead of orange "missing model" dots; `scanscene` classifies them as non-visual.
- **GLTF import/export + POD writer wired into build** (were referenced but never linked into `bin/ruby`).
- **About page credits**: DanielSpaniel (official Python FileRift + Boulder engine, translated to C++), MrSinup, OpenSwordigo.

### Fixed
- **Scene-viewport input bug**: viewport hover captured immediately after `ImGui::Image` so orbit/zoom/pan/picking work reliably.
- **Render-scale mismatch**: picking/overlays use logical size while FBO/PostFX use scaled size — no more broken picking at non-1x scales.
- **Spawn camera too close**: reframed at in-game hero distance instead of hugging the spawn point.
- **Model viewport**: fog/light state no longer leaks into POD + GM previews.
- **`scanscene` stale-binary crash** resolved by forced rebuild; reports `non-visual` vs `missing-models` correctly.
- **`scene_loader` water parsing**: WaterMesh rect + colors + texture resolved from real scene data (verified against fire_part1 + florennum_jail_boss).

---

## [v8.0 Beta 2] — 2026-07-31

### Added
- **RakNet Network Framework Integration**: Integrated RakNet cross-platform UDP engine into core build tree with low-level packet serialization, GUID handshakes, and peer connection state management.
- **Modular Shared Object (.so) Build System**: Split monolithic code into 9 modular shared libraries (`libswcore.so`, `libswemu.so`, `libswgfx.so`, `libswgui.so`, `libfilerift.so`, `libswordfare.so`, `libswfmt.so`, `libsre.so`, `libswordigo.so`) under `bin/` with `swordfare` primary executable.
- **Swordfare Editor (IntelliJ) Overhaul**: Stateful multiline tokenization (`LexState`), custom `.styx` theme parser (`FileRift (Grove)`, `BatSyntax`), quote-aware comment stripping, string/comment-aware bracket error checking, line token caching, and Swordigo SDK autocomplete/symbol metadata (`Game`, `Character`, `Scene`, `PhysicsObject`, `Vector3`, `Shardshi`).
- **OptiX Architectural Framework**: 10 master technical research specifications for modern graphics, PBR lighting, reflection hooks, and Dynarmic ARM64 JIT hardware acceleration.
- **Dynarmic ARM64 JIT Tuning**: 512MB code cache allocation, unsafe FMA3 host instruction flags, and compiler optimization flags (`Unsafe_UnfuseFMA`, `Unsafe_ReducedErrorFP`, `Unsafe_InaccurateNaN`).

### Fixed
- **Regex Engine Stability**: Resolved `std::regex_error` lookbehind issues in `FileRift (Grove)` theme.
- **Quote-Aware Config Comment Stripping**: Prevented comment stripping from destroying strings containing `//` or `--`.
- **Modern Memory Access**: Standardized buffer access using C++17 `buffer->data()`.
- **RPM Build Spec**: Fixed `/usr/bin/ruby` unpackaged file error in RPM packaging.

---

## [v8.0 Beta 1] — 2026-07-11

### Added
- **Universal Mod Virtual File System (VFS)**: 5-layer prioritized asset loading hierarchy.
- **Native Uncompressed PVR & PVRTC Decoding**: Host-side PVR/PVRTC decompression.
- **SwKiwi Native UI Overlays**: Interactive ImGui window overlays with input gating.
- **True Lua Console & SRE Native Invocation**: `caver.call()` API and direct raw memory Read/Write APIs.

---

## [v7.1] — 2026-06-25

### Added
- **SRE Compatibility Check for Custom Instances** — added `custom-` path prefix check in `main.cpp`, ensuring `libsre.so` correctly loads for user-added modded instances.
- **SRE Dependency Registration** — added automatic dependency inclusion for `libsre.so` in `launcher_ui.cpp` when importing custom instances with SRE enabled.

### Changed
- Package version bumped to 7.1.0 with nickname **hot-fix**.
- **Config Persistence Priority** — config loaded from `instances.json` now merges and takes priority over filesystem-scanned metadata on duplicates, retaining game type and assets settings.

### Fixed
- **Custom Modded Instance Crash** — resolved startup abort crashes (empty `.POD` errors) on custom modded instances by ensuring the correct guest binary (e.g. RLSwordigo binary instead of vanilla) is copied and `libsre.so` hooks are properly initialized.
- **Bolt/Timer misbehavior and Text input crash** — removed old known limitations from documentation as SRE hooks now fully intercept and resolve them.

---

## [v7.0] — 2026-06-24

### Added
- **Dynarmic JIT compiler** — ARM64 code at near-native speed (60fps), replaces Unicorn as default
- **RLSwordigo support** — roguelike spinoff playable through custom instances
- **KiwiAPI / Combatch mod compatibility** — SWKiwi modloader hooks (Phase 1 & 2)
- **Bauble API** — Phase 3.3 trinket/bauble system hooks
- **Achievement System** — Phase 3.4 achievement hooks
- **io.open + fgets/fscanf bridges** — full file I/O for Combatch mod
- **_longjmp registration** — Lua error recovery support
- **Instance management overhaul** — custom assets folders per instance

### Changed
- Dynarmic is the default ARM64 backend (`make DYNARMIC=1`)
- Unicorn Engine retained as `--no-dynarmic` fallback
- F12 fullscreen toggle preserves native display aspect ratio (16:10, etc.)
- Launcher assets moved to `launcher/` subfolder for clean RPM/DEB installs
- Package version bumped to 7.0.0

### Fixed
- F12 fullscreen exit forced 16:9 aspect ratio — now queries actual window size
- Launcher icon/texture loading on packaged installs (RPM/DEB)

---

## [v6.5] — 2026-06-23

### Added
- **Complete modding documentation** — 22-file, 414KB `modapi/` suite
- **Vulkan renderer backend** — full GLES 1.x fixed-function pipeline emulator
- **Graphics API in F3 debug overlay** — shows OpenGL/Vulkan in HUD
- **ImGui launcher enhancements** — background texture, instance icons, Add Instance button
- **sre_mod.c** — mod config shared memory protocol at guest address 0x49000

### Changed
- Launcher version bumped to v6.5
- Vulkan radio button no longer labeled "(WIP)"

---

## [v6.0] — 2026-06-22

### Added
- **Full GUI DrawRect stack** — 8 native hooks for total GUI rendering control
- **Native GUI rendering** — GUIButton, GUILabel, GUIFrameView reimplemented in C
- **Button text override system** — rename any button at draw time ("Offers" → "Options")
- **Scoped button hiding** — remove IAP shop button cleanly
- **Save Editor** — built into launcher (coins, health, mana, XP, weapon, keys)
- **Asset Viewer** — standalone tool for browsing game textures, audio, scenes
- **PostFX pipeline** — 6 shader presets (Cinematic, Retro, Fantasy, Noir, Ethereal, Atmospheric)
- **SRE Lua Libraries** — custom Lua environment with Mini.*, LNI.*, Components.* tables
- **Virtual Filesystem** — `sre_vfs.c` for future mod asset layering
- 30+ active SRE hooks (up from 17 in v5.0)

### Fixed
- **Wastelands spinlock** — infamous ARM64 freeze in Wastelands region resolved
- **Death freeze** — ad SDK path hang fixed, instant checkpoint respawn
- **Death loop** — duplicate hook entry at 0x347efc removed
- **GameSceneView::Update** — entire function body recovered from TVPG snapshot
- **Player stats** — 13 volatile globals properly defined and populated

---

## [v5.0] — 2026-06-21

### Added
- **libsre.so** — Swordigo Runtime Engine: 17 active hooks replacing entire subsystems
- **Full music system** — MusicPlayer replaced with OpenAL command interface (6 hooks)
- **Instant death respawn** — `ShowAdMaybe` → `GameOverViewDidContinue` (no ads, no restart)
- **HUD reimplementation** — `GameSceneView::Update` fully rewritten in C
- **Smart coin bar** — shop-aware auto-hide with 3s fade timer
- **Damage flash** — red overlay on HP decrease
- **Player stats export** — HP/Mana/Coins/XP/Level/ATK visible in F3 overlay
- **GameState pointer** — direct host-side game memory access
- **Music loop watchdog** — detects and restarts stopped looping music
- **SRE version gate** — only loads for v1.4.12 ARM64, safe skip for other binaries
- **Lua error recovery** — ARM64 `setjmp`/`longjmp` for safe `lua_pcall`
- **Background rendering hooks** — 3 custom hooks for sky/parallax

### Changed
- Architecture renamed: **SRT** (Swordigo Runtime) as the overall framework
- Mod menu cleaned: only GAME section (Speed/Pause/Camera) visible
- Package builder updated: includes `libsre.so` in ARM64 engine dirs
- README rewritten for SRT architecture

### Fixed
- Music not repeating when track ends in same world
- Non-atomic string operations eliminate STXR spin loop hangs (4 hooks)

## [v4.5r] — 2026-06-19

### Added
- **Save Editor in Launcher** — browse and edit `.gplayer` save files directly from the launcher window
  - Lists all saves with name, area, level, progress percentage, and playtime
  - Editable fields: Coins, Health, Mana, XP, Equipped Weapon, Keys
  - Text input fields with keyboard navigation (TAB/ESC/ENTER)
  - Creates `.bak` backup before writing
  - Success/failure status feedback

### Changed
- Launcher version bumped to v4.5r
- ARM64 `pthread_create` now discards thread functions (matching ARM32 behavior)

### Fixed
- ARM64 entity processing: NOP'd setup call at `0x580708` with `MOV X0, XZR` to prevent spinlock on empty entity lists

### Removed
- In-game save editor (Mods menu) — replaced by the launcher-integrated version

### Known Issues
- **ARM64**: Game freezes (spinlock) when entering Wastelands
- **ARM32**: Timer-based repeating spikes and boss gate triggers don't work
- **Launcher**: Instance icons show placeholder in .deb package installs

---

## [v4.0r] — 2026-06-18

### Added
- **ARM64 (AArch64) emulation** via Unicorn Engine ARM64 backend
- ARM64 ELF loader with full RELA relocations
- ARM64 JNI bridge (200+ functions)
- Dual-arch launcher — ARM32 and ARM64 instances side-by-side
- **GPU draw call batcher** — streaming VBO reduces draw calls from 80-140 to 15-25 per frame
- **FSR 1.0 upscaling** option
- **PolyMC-inspired launcher** — instance card grid with icons, version badges, arch labels
- Custom instance import via file dialog
- GLSL 330 shader migration

### Changed
- Launcher redesigned with card grid + detail panel layout
- Threading bridges now functional (mutex, cond, create, once)

---

## [v3.0r] — 2026-06-17

### Added
- **HiDPI / native resolution rendering**
- **SDL2 → SDL3 migration**
- Standalone RPM and DEB packaging
- Binary Selector v2 with v1.4.12 as default

### Fixed
- Death screen hang resolved
- Improved FBO scaler with Sharp Bilinear mode

---

## [v2.0r] — 2026-06-17

### Added
- **SRE PostFX pipeline** — SSAO, God Rays, Volumetric Light Shafts
- 7 visual presets (Cinematic, Retro, Fantasy, Noir, Ethereal, Atmospheric)
- **Unified Launcher GUI** with binary selection + graphics API picker
- SHA-256 binary validation
- Depth buffer as texture for shader effects

---

## [v1.0r] — 2026-06-16

### Added
- Initial release
- Custom ARM ELF loader with Unicorn Engine
- JNI bridge layer (200+ functions)
- OpenGL rendering, OpenAL audio
- Keyboard + gamepad controls
- Save system persistence
