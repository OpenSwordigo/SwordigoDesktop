# ⚔️ Swordigo Desktop v2.0r — Release Notes

> *June 17, 2026*

---

## 🎨 Advanced Rendering Pipeline

The biggest feature in v2.0r is a complete rewrite of the rendering pipeline from a single-pass FBO scaler to a **multi-pass post-processing engine** with 8 GLSL shader programs.

### New Effects

| Effect | Description |
|--------|-------------|
| **SSAO** | Screen Space Ambient Occlusion — 16-sample hemisphere with bilateral blur at half resolution. Adds realistic contact shadows in crevices and corners. |
| **God Rays** | 64-sample screen-space radial blur from a configurable sun position. Creates dramatic light shafts streaming through the scene. |
| **Volumetric Light Shafts** | Depth-masked crepuscular rays — far pixels (sky/background) cast light while close objects act as occluders. Warm golden tint. |
| **Vignette** | Configurable edge darkening for cinematic focus. |
| **Film Grain** | Animated noise overlay for vintage film aesthetic. |
| **Chromatic Aberration** | RGB channel offset based on distance from center. |
| **Color Grading** | Full saturation, contrast, brightness, and warmth controls. |
| **Sharpen** | Unsharp mask filter using 4-tap neighborhood sampling. |

### 7 Visual Presets (F6 Key)

| Preset | Effects | Mood |
|--------|---------|------|
| **Off** | None | Default game rendering |
| **Cinematic** 🎬 | Vignette + Film Grain + Warm tones | Hollywood blockbuster |
| **Retro** 📼 | Grain + High saturation + Chromatic aberration | 90s arcade |
| **Fantasy** ✨ | Vignette + Saturation + Warmth + Sharpen | Enchanted world |
| **Noir** 🖤 | Heavy vignette + Grain + Desaturated + High contrast | Dark detective |
| **Ethereal** 🌅 | God Rays + Warm glow + Vignette | Heavenly light shafts |
| **Atmospheric** 🌫️ | SSAO + Volumetric Light + Vignette + Contrast | Immersive depth |

### Pipeline Architecture

```
Game FBO (Color + Depth Texture, 960x544)
    |
PASS 1: SSAO -> Half-res (480x272) -> Bilateral Blur
    |
PASS 2: God Rays -> Half-res (480x272) -> 64-sample radial blur
    |
PASS 3: Composite -> Full-res (Scene x AO + Rays additive)
    |
PASS 4: Color PostFX -> Vignette, Grain, CA, Color Grade, Sharpen
    |
PASS 5: Upscale -> Sharp-Bilinear / Nearest / CRT -> Window
```

### Technical Details

- **Depth buffer converted from renderbuffer to texture** (GL_DEPTH24_STENCIL8) for direct shader sampling
- SSAO and God Rays rendered at **half resolution** for performance
- Two half-res ping-pong FBOs for separable blur passes
- All 8 shaders pre-compiled at initialization

---

## 🚀 Unified Launcher GUI

A new pre-launch configuration window appears before the game starts:

- **Binary Selection** — Choose between multiple libswordigo*.so versions
- **Graphics API Picker** — Select OpenGL or Vulkan (experimental)
- **SHA-256 Validation** — Each binary is hashed and tracked
- **Auto-detection** — Supports v1.4.6 (tested) and v1.4.12 (testing)

### Binary Registry

A JSON-based registry (swordigo_binaries.json) tracks:
- Filename, version label, SHA-256 hash
- File size, tested/untested status
- Default binary preference (persisted across sessions)

---

## ⚡ Engine Improvements

- **GUI click fix** — Corrected 1px sidebar coordinate offset that made buttons hard to click
- **glGetFloatv bridge** — New JNI handler for matrix queries (identity fallback for headless)
- **Modal input blocking** — Dropdown menus now properly consume clicks (has_modal_open fix)
- **Vulkan backend wiring** — All GL bridge functions ifdef'd for optional Vulkan path (experimental)
- **Window title** — Updated to "Swordigo Desktop v2.0r"
- **Debug overlay (F3)** — Now shows loaded binary info, PostFX preset, and version details
- **F-key help** — Updated to F1:GUI F2:Ctrl F3:Dbg F4:Scale F5:Cam F6:PostFX F7:Type F10:HUD

---

## 📦 Packages

| Format | File | Size |
|--------|------|------|
| Debian/Ubuntu | swordigo-desktop_2.0.0_amd64.deb | ~48 MB |
| Fedora/RHEL | swordigo-desktop-2.0.0-1.fc44.x86_64.rpm | ~51 MB |
| Binary | swordigo_boot | ~1 MB |
| Headless | swordigo_headless | ~1 MB |
| Vulkan | swordigo_vk | ~1.7 MB |

### Makefile Changes
- make install now installs all libswordigo*.so variants (excludes 64-bit)
- Multi-binary packaging for binary selector support

---

## 🎮 Controls (Unchanged)

| Key | Action |
|-----|--------|
| Left / A | Move Left |
| Right / D | Move Right |
| Space / W | Jump |
| J / Z | Attack |
| K / X | Magic |
| I | Use Item |
| Escape | Menu |
| **F6** | **Cycle PostFX Presets** *(NEW)* |

---

## ⚠️ Known Limitations

- When SSAO/God Rays are active, Tier 1 color effects (vignette, grain, etc.) are applied via the composite pass rather than separately. Presets are tuned accordingly.
- Vulkan backend is experimental and requires --vulkan CLI flag or launcher selection.
- SSAO near/far planes are approximated (0.1-100.0) — may need tuning for some scenes.

---

## 👥 Credits

| Role | Name |
|------|------|
| Lead Developer | TheMegineBraine |
| Developer | TheCorrectSynovian |
| SwMini Research | ItsJustSomeDude, Kiziyon |
| Vita Port Research | Rinnegatamante |

---

Built with love for game preservation.
