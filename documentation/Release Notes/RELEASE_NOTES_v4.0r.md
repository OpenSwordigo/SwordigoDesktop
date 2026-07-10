# ⚔️ Swordigo Desktop v4.0r — Release Notes

> *June 18, 2026*

---

## What's New in v4.0r (since v3.0r)

### 🧠 ARM64 Emulation — The Big One

Full **ARM64 (AArch64)** emulation support via Unicorn Engine, running alongside the existing ARM32 backend. This unlocks native 64-bit Android binaries on Linux — a first for any Swordigo port.

| Feature | Details |
|---------|---------|
| **Unicorn ARM64 backend** | Complete UC_ARCH_ARM64 emulation with ELF loader, relocation, and JNI bridge |
| **ARM64 JNI bridge** | 200+ bridged functions — GLES, OpenAL, Bionic libc, pthreads, JNI |
| **ARM64 register ABI** | Proper X0–X30 register mapping, LR return address handling |
| **ARM64 ELF loader** | Loads `arm64-v8a/libswordigo.so` with full RELA relocations |
| **Dual-arch launcher** | Instance manager shows ARM32 and ARM64 binaries side-by-side |
| **v1.4.12 ARM64** | Status: **Tested** — full game playable on 64-bit binary |
| **v1.4.6 ARM64** | Status: **Available** — selectable via launcher |

#### Architecture

```
┌─────────────────────────────────────────────┐
│               Swordigo Desktop              │
├─────────────────────────────────────────────┤
│  main.cpp (SDL3 window, input, frame loop)  │
├──────────────────┬──────────────────────────┤
│  ARM32 Path      │  ARM64 Path (NEW)        │
│  Unicorn ARM     │  Unicorn ARM64           │
│  jni_bridge.cpp  │  jni_bridge_arm64.cpp    │
│  emulator.cpp    │  emulator_arm64.cpp      │
│  elf_loader.cpp  │  elf_loader_arm64.cpp    │
├──────────────────┴──────────────────────────┤
│  Shared: FBO Scaler, PostFX, Launcher, GUI  │
│  Shared: Audio (OpenAL), Input, Save/Load   │
└─────────────────────────────────────────────┘
```

---

### 🚀 GPU Performance: Draw Call Batcher

Massive reduction in CPU→GPU overhead via a streaming VBO draw call batcher.

| Metric | Before | After |
|--------|--------|-------|
| **Draw calls/frame** | 80–140 | ~15–25 |
| **CPU overhead** | High (many small glDrawArrays) | Low (few large batched draws) |
| **VBO strategy** | None (client-side vertex arrays) | 4MB streaming GL_STREAM_DRAW VBO |

#### How It Works

1. Game calls `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)` → batcher captures vertices
2. CPU-side MVP matrix transform applied to each vertex
3. Strips/fans converted to `GL_TRIANGLES` for batch merging
4. When texture, blend mode, or matrix changes → batch flushed as **one** `glDrawArrays(GL_TRIANGLES, 0, N)`
5. Frame end → remaining batch flushed before PostFX

State tracked for batch breaks: `glBindTexture`, `glBlendFunc`, `glEnable/Disable(GL_BLEND)`, `glLoadMatrixf`, `glEnableClientState(GL_COLOR_ARRAY)`.

---

### 🔍 FSR 1.0 — Edge-Adaptive Spatial Upscaling

AMD FidelityFX Super Resolution 1.0 integrated as a new upscale filter mode.

| Feature | Details |
|---------|---------|
| **Algorithm** | 16-tap Lanczos2 with edge-directional weighting |
| **Edge detection** | Gradient-based horizontal/vertical edge analysis |
| **Directional sharpness** | Kernel stretched along detected edges for sharp text/lines |
| **Anti-ringing** | Min/max clamp from 4 nearest neighbors prevents overshoot |
| **Integrated RCAS** | Subtle contrast-adaptive sharpening (15% blend) |
| **Input filtering** | `GL_NEAREST` — shader does its own filtering |

Press **F4** to cycle: `Sharp-Bilinear → Nearest → CRT Scanline → FSR 1.0`

---

### 🧵 Threading Bridges

Real host-side threading primitives for ARM64 guest code:

| Bridge | Implementation |
|--------|---------------|
| `pthread_create` | Inline execution — calls thread function directly in emulator context |
| `pthread_join` | Returns immediately (inline threads already completed) |
| `pthread_detach` | No-op (already inline) |
| `nanosleep` | Real `nanosleep()` on host, capped at 100ms to prevent game stalls |
| `usleep` | Real `usleep()` on host, capped at 100ms |
| `sched_yield` | Real `sched_yield()` to host scheduler |

---

### 🎨 GLSL Shader Upgrade

All 10 PostFX shaders migrated from `#version 120` to `#version 330 core`.

| Shader | Purpose |
|--------|---------|
| Sharp Bilinear | Default upscale filter |
| Nearest | Pixel-perfect upscale |
| CRT Scanline | Retro CRT simulation |
| **FSR 1.0** | **NEW** — Edge-adaptive upscaling |
| PostFX Composite | Vignette, grain, chromatic aberration, color grading |
| God Rays | Volumetric light scattering |
| SSAO | Screen-space ambient occlusion |
| Gaussian Blur | Multi-pass blur for SSAO/bloom |
| Bloom Extract | HDR brightness extraction |
| Final Composite | SSAO + god rays + bloom merge |

Changes: `attribute` → `in`, `varying` → `in/out`, `texture2D()` → `texture()`, `gl_FragColor` → explicit `out vec4 FragColor`.

---

### 🎛️ PolyMC-Style Launcher

Complete launcher rewrite — instance manager inspired by PolyMC/Prism Launcher.

| Feature | Details |
|---------|---------|
| **Instance grid** | Visual grid of all engine binaries with icons |
| **Detail panel** | Version, arch, game type, assets dir, dependencies, file size, status |
| **Open folder** | `xdg-open` on instance directory — works on all Linux DEs |
| **Delete instance** | Remove from in-memory list (grid updates immediately) |
| **Assets selection** | `assets_dir` flows into LaunchConfig (vanilla or RL) |
| **Mod support** | vManson mod entries with dependency tracking (libmini.so, libGlossHook.so) |
| **Status badges** | Tested ●, Stable ●, Unknown ● color indicators |

---

### 🌟 PostFX Enhancements (since v3.0r)

| Effect | What Changed |
|--------|-------------|
| **God Rays** | Increased intensity and decay for stronger volumetric light |
| **SSAO** | Higher depth shadow intensity |
| **Bloom** | HDR extraction + multi-pass Gaussian blur + additive composite |
| **FSR upscale** | Replaces bilinear for sharper edges at all resolutions |

---

## 📦 Packages

| Format | File | Notes |
|--------|------|-------|
| Fedora/RHEL | `swordigo-desktop-4.0.0-1.x86_64.rpm` | `sudo rpm -Uvh --force --nodeps <file>` |
| Debian/Ubuntu | `swordigo-desktop_4.0.0-1_amd64.deb` | `sudo dpkg -i <file>` |

> **Vanilla only** — ships v1.4.6 + v1.4.12 (ARM32 + ARM64), `assets/` directory.
> Mods (vManson, RLSwordigo) are not included — add them manually to `engine/`.

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
| F4 | Cycle scale modes (Sharp-Bilinear → Nearest → CRT → **FSR**) |
| F5 | Camera toggle |
| F6 | Cycle PostFX presets |
| F7 | Typing mode |
| F8 | Pause |
| F10 | Toggle HUD |
| F12 | Fullscreen toggle |
| +/- | Game speed |

---

## ⚠️ Known Limitations

- ARM64 threading is inline (cooperative) — guest threads run sequentially, not in parallel
- Draw call batcher only batches `GL_TRIANGLES/STRIP/FAN` — other modes fall through to direct calls
- FSR 1.0 is spatial only (no temporal accumulation) — may show minor shimmer on sub-pixel detail
- Vulkan backend remains experimental (OpenGL is default and recommended)
- On 5K+ displays, render resolution is capped at 4096 wide

---

## 📊 Performance Comparison

| Metric | v3.0r | v4.0r |
|--------|-------|-------|
| Draw calls/frame | 80–140 | 15–25 |
| Shader version | GLSL 120 | GLSL 330 |
| Upscale quality | Sharp-Bilinear | FSR 1.0 (edge-adaptive) |
| Architecture | ARM32 only | ARM32 + ARM64 |
| Threading | Stubbed | Real (capped) |
| Launcher | Basic selector | PolyMC-style instance manager |

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
