# ⚔️ Swordigo Desktop v3.0r — Release Notes

> *June 17, 2026*

---

## What's New in v3.0r (since v2.0r)

### 🖥️ HiDPI / High-Resolution Display Support

The headline feature of v3.0r — **full native resolution rendering** on any display.

| Feature | Details |
|---------|---------|
| **Dynamic render resolution** | Game render resolution auto-detected from display physical pixels at startup — no hardcoded values |
| **Aspect ratio detection** | Natively supports 16:9, 16:10, 3:2, and any other display aspect ratio — no letterboxing on native panels |
| **HiDPI awareness** | `SDL_WINDOW_HIGH_PIXEL_DENSITY` ensures GL framebuffer matches physical pixels, not logical |
| **Supersampling AA** | On HiDPI displays, renders above window size → natural anti-aliasing when downscaled to drawable |
| **1:1 pixel mapping** | When render res = drawable res, FBO scaler blits pixel-perfect with zero upscale |

#### How It Works

```
Display detected: 3000×1876 (200% scaling)
  → Window: 1920×1080 logical
  → Drawable: 3840×2160 physical (200% of logical)
  → Render: 3840×2160 (matched to drawable)
  → FBO blit: sharp-bilinear → pixel-perfect output

Display detected: 1920×1080 (100% / no scaling)
  → Window: 1920×1080 logical
  → Drawable: 1920×1080 physical
  → Render: 1920×1080 (1:1)
```

The render pipeline uses **three coordinate spaces**:
- `g_win_w / g_win_h` — logical window size (mouse coordinates, GUI layout)
- `g_draw_w / g_draw_h` — physical drawable pixels (glViewport, FBO blit)
- `GAME_W / GAME_H` — internal render resolution (matched to drawable at startup)

---

### 🚀 SDL2 → SDL3 Migration

Complete migration from SDL2 to SDL3 across the entire codebase.

| Change | Details |
|--------|---------|
| **SDL3 API** | All SDL2 calls updated to SDL3 equivalents |
| **Event system** | `SDL_EVENT_*` enum names, `SDL_PollEvent` updated |
| **Window API** | `SDL_CreateWindow` new signature, `SDL_WINDOW_*` flags updated |
| **Audio** | SDL3 audio subsystem |
| **Gamepad** | `SDL_Gamepad` API (was `SDL_GameController`) |
| **Surface** | `SDL_DestroySurface` (was `SDL_FreeSurface`) |
| **HiDPI** | `SDL_GetWindowSizeInPixels()`, `SDL_WINDOW_HIGH_PIXEL_DENSITY` |

---

### 🎮 Binary Selector v2

| Feature | Details |
|---------|---------|
| **libswordigo_nx.so (v1.4.12)** | New default binary — TESTED and stable |
| **libswordigo.so (v1.4.6)** | Previous default, still available |
| **RL Swordigo support** | `rl_libswordigo.so` (v6.1-rl) with mod dependencies |
| **[Latest] badge** | Shows which binary is the recommended latest |
| **Game type detection** | Vanilla vs RLSwordigo with separate asset directories |

---

### 📦 Standalone Packaging

| Feature | Details |
|---------|---------|
| **RPM + DEB builder** | `builder/package.sh` — no CMake/CPack dependency |
| **Variant support** | Separate packages for vanilla Swordigo and RLSwordigo |
| **All assets included** | Game resources, music, launcher textures, icons |
| **Desktop integration** | .desktop file, icon in hicolor, proper categories |

---

### 🐛 Bug Fixes

| Fix | Details |
|-----|---------|
| **Death screen hang** | Patched `ShowInterstitialAd` to `bx lr` (no-op) + auto-restart on death detection via gameover music. The Vita port couldn't fix this. |
| **Blurry rendering on HiDPI** | Separated logical/physical pixel dimensions — `glViewport` uses physical, mouse mapping uses logical |
| **Window overflow on HiDPI** | No longer creates windows larger than the logical desktop |
| **FBO quarter-screen render** | Fixed by using `SDL_GetWindowSizeInPixels()` for all GL viewport calls |
| **Fullscreen resolution** | F12 toggle now correctly handles physical pixel dimensions |

---

### 🔧 Engine Improvements

| Improvement | Details |
|-------------|---------|
| **Render resolution in F3 overlay** | Shows `Render: WxH → Window: WxH (Draw: WxH)` — all three coordinate spaces |
| **Dynamic touch scaling** | `TOUCH_SCALE_X/Y` recomputed when render resolution changes |
| **Resolution clamping** | Render res clamped to 1920–4096 width, even dimensions enforced |
| **Aspect ratio logging** | Startup log shows logical, drawable, render dimensions and aspect ratio |
| **Version** | v3.0r displayed in launcher footer |

---

## 📦 Packages

| Format | File | Notes |
|--------|------|-------|
| Fedora/RHEL | `swordigo-desktop-3.0.0-1.x86_64.rpm` | `sudo rpm -Uvh --force --nodeps <file>` |
| Debian/Ubuntu | `swordigo-desktop_3.0.0-1_amd64.deb` | `sudo dpkg -i <file>` |
| RL Edition RPM | `swordigo-desktop-rl-3.0.0-1.x86_64.rpm` | Includes RL assets + mods |
| RL Edition DEB | `swordigo-desktop-rl_3.0.0-1_amd64.deb` | Includes RL assets + mods |

---

## 🎮 Controls

| Key | Action |
|-----|--------|
| WASD / Arrows | Movement / Camera |
| Space / W | Jump |
| J / Z | Attack |
| K / X | Magic |
| I | Use Item |
| Escape | Menu |
| F1 | GUI overlay |
| F2 | Controls editor |
| F3 | Debug overlay |
| F4 | Cycle scale modes |
| F5 | Camera toggle |
| F6 | Cycle PostFX presets |
| F7 | Typing mode |
| F8 | Pause |
| F10 | Toggle HUD |
| F12 | Fullscreen toggle |
| +/- | Game speed |

---

## ⚠️ Known Limitations

- Vulkan backend is experimental (OpenGL is the default and recommended)
- When SSAO/God Rays are active, Tier 1 color effects are composited rather than applied separately
- On very high resolution displays (5K+), render resolution is capped at 4096 wide for GPU compatibility

---

## 👥 Credits

| Role | Name |
|------|------|
| Lead Developer | TheMegineBraine |
| Developer | TheCorrectSynovian |
| SwMini Research | ItsJustSomeDude, Kiziyon |
| Vita Port Research | Rinnegatamante |

---

Built with love for game preservation. 🗡️
