# FBO Scaler & Viewport Upscaler Pipeline

> **Header**: `src/platform/fbo_scaler.h`  
> **Source**: `src/platform/fbo_scaler.cpp`  
> **Toggle Key**: **F4** (cycle upscale filters)

The FBO scaler is the core rendering pipeline that decouples the game's internal resolution (native **960×544**) from the desktop window size. The game renders into a Framebuffer Object (FBO), then the scaler applies post-processing effects and upscales to the display window.

---

## 1. Pipeline Overview

```
┌───────────────────────────────────────────────────────────────┐
│                     Game Rendering (960×544)                  │
│  drawApplication() renders to FBO with color + depth textures│
└──────────────────────────┬────────────────────────────────────┘
                           │
┌──────────────────────────┴────────────────────────────────────┐
│                PostFX Passes (if enabled)                     │
│  Tier 2 (multi-pass, separate FBOs):                          │
│    ├─ SSAO → Gaussian blur → half-res AO texture             │
│    ├─ God Rays → half-res rays texture                       │
│    ├─ Bloom → extract → quarter-res blur → bloom texture     │
│    └─ Composite: scene + AO + rays + bloom + shadows         │
│                                                               │
│  Tier 1 (single pass, full-res):                              │
│    └─ CA → Sharpen → Color → Grain → Vignette → Outlines     │
└──────────────────────────┬────────────────────────────────────┘
                           │
┌──────────────────────────┴────────────────────────────────────┐
│              Upscale to Window (win_w × win_h)                │
│  Scale mode selected by FBOScale enum (F4 to cycle)           │
└───────────────────────────────────────────────────────────────┘
```

---

## 2. FBOScale Enum & Filter Modes

```cpp
enum class FBOScale : int {
    SHARP_BILINEAR = 0,
    NEAREST        = 1,
    CRT_SCANLINE   = 2,
    FSR            = 3,
};
```

| Enum Mode | Name | Shader Implementation | Description |
| :--- | :--- | :--- | :--- |
| `0` | `SHARP_BILINEAR` | `FRAG_SHARP_BILINEAR` | Sub-pixel-aware bilinear filtering. Sharpens texels while maintaining smooth edges. |
| `1` | `NEAREST` | `FRAG_NEAREST` | Crisp nearest-neighbor pixel snapping. |
| `2` | `CRT_SCANLINE` | `FRAG_CRT` | CRT monitor simulation: scanlines, barrel distortion, vignette. |
| `3` | `FSR` | `FRAG_FSR` | AMD FidelityFX Super Resolution 1.0 (EASU/RCAS) directional upscaling. |

---

## 3. Core API Functions

### `bool fbo_init(int game_w, int game_h)`
Initializes the FBO pipeline, allocates GPU resources (`RGBA8` main color attachment, `DEPTH24_STENCIL8` depth texture, ping-pong PostFX buffers), and compiles GLSL shader programs.

### `void fbo_begin_game()`
Binds the main game FBO as the active render target before `drawApplication()`.

### `void fbo_end_game_and_blit(int win_w, int win_h, FBOScale mode, const PostFXState* postfx)`
Executes PostFX passes, unbinds the game FBO, and upscales the composite texture to the target window dimensions.
