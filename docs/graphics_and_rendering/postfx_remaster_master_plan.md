# PostFX Remaster Plan

## 🔴 Critical Bugs Found

### SSAO — Wrong Depth Linearization
The SSAO shader uses `near=0.1, far=100.0` but the game uses `near=50, far=20000`. 
**This is why SSAO never worked!**

### Bloom — FBOs Never Allocated
`FRAG_BLOOM_EXTRACT` shader exists and compiles, but `g_bloom_fbo_a/b` are **never created**. 
The bloom pass is completely absent from the pipeline.

### Color FX Skipped During Composite
When SSAO or God Rays are active, color PostFX (vignette, grain, etc.) are **skipped** because there's no second full-res FBO for ping-pong.

---

## Quick Wins (Implementing Now)

| Fix | Impact | Effort |
|-----|--------|--------|
| Fix SSAO near/far → 50/20000 | HIGH | 2 lines |
| Allocate bloom FBOs (240×136) | HIGH | 2 lines |
| Wire bloom extract→blur→composite | HIGH | ~50 lines |
| Add g_postfx_fbo_b (second full-res) | MEDIUM | 3 lines |

## Data We Have

| Source | Data Available |
|--------|---------------|
| SRE Background | depth, scale, cam_z, cam_fov, cam_aspect, ambient RGB |
| SRE Effects | Portal pos/color/MV matrix, Glow pos/color |
| Camera | View matrix, position, FOV=20°, near=50, far=20000 |
| Depth buffer | `g_fbo_depth_tex` (GL_DEPTH24_STENCIL8) — game DOES write depth |
| GL bridge | glDepthFunc + glDepthMask bridged — depth testing IS active |

## Proposed Pipeline (7 passes)

```
Game FBO (color+depth)
 ├─→ SSAO → half_fbo_a (blur) → ao_tex
 ├─→ God Rays → half_fbo_b → gr_tex  
 ├─→ Bloom Extract → bloom_fbo_a (blur×2) → bloom_tex
 ├─→ Portal quad (additive)
 ├─→ Composite (scene×AO + godrays + bloom) → postfx_fbo
 ├─→ Color FX (vignette+grain+CA+color) → postfx_fbo_b
 └─→ Upscale blit → screen
```

## RE Targets (VS Code search)

| Symbol | Why |
|--------|-----|
| `SetAmbientColor` | Light mood for god rays |
| `LightComponent` | Light direction |
| `ShadowVolumeComponent` | Shadow/light direction (offset 0x236964) |
| `FocusAtPoint` | Hero position for DoF |
| `FogComponent` | Atmospheric parameters |

## Future Effects Priority

1. **Bloom** — biggest visual impact for fantasy game (portals, glows, lava, magic)
2. **SSAO** (as contact shadows) — small radius, high intensity, great for 2.5D
3. **Depth of Field** — focus on hero, blur far objects
4. **Dynamic God Rays** — derive sun from SkyRenderer or ambient color
5. **Atmospheric Fog** — depth-based tinting with ambient color
6. **True Volumetric Light** — ray marching (expensive, outdoor only)
