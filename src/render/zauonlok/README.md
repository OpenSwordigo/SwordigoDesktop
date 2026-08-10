# zauonlok/renderer — Vendored Algorithm Reference

**Upstream:** https://github.com/zauonlok/renderer (Zhou Le) · **License:** MIT (see
`LICENSE.zauonlok-renderer`) · **Version vendored:** `renderer-master` (2021-01-18 snapshot).

This directory holds the **authoritative C reference sources** for the algorithms that were
ported into Ruby's GPU renderer (`src/tools/av_renderer.cpp`). They are kept for:
1. **Attribution** — the MIT license requires preserving the copyright notice, which we do.
2. **Verification** — the C implementations are the deterministic oracle for the GLSL port.
3. **Future ports** — the pieces not yet ported (skeletal `.ani` evaluator, HDR reader) live here.

## What was ported (live in av_renderer.cpp)

| Algorithm | Vendor reference | Port location |
| :--- | :--- | :--- |
| GGX NDF (`get_distribution`) | `pbr_shader.c` | `PBR_FS → distribution_ggx()` |
| Smith visibility (`get_visibility`) | `pbr_shader.c` | `PBR_FS → visibility_smith()` |
| Schlick+F90 Fresnel (`get_fresnel`) | `pbr_shader.c` | `PBR_FS → fresnel_schlick()` |
| Metallic-roughness material | `pbr_shader.c → get_pbrm_material()` | `PBR_FS → decode_material()` |
| Specular-glossiness material | `pbr_shader.c → get_pbrs_material()` | `PBR_FS → decode_material()` |
| Tangent normal mapping (TBN + `tangent.w`) | `pbr_shader.c → get_normal_dir()` | `PBR_FS → get_normal_dir()` |
| IBL split-sum (BRDF LUT + prefiltered env) | `pbr_shader.c → get_ibl_shade()` | `PBR_FS → ibl_shade()` + `generate_brdf_lut()` |
| Shadow mapping (light-VP + N·L bias) | `pbr_shader.c → is_in_shadow()` + `shadow_*` passes | `PBR_FS → is_in_shadow()` + `begin/end_shadow_pass()` |
| GPU 4-bone skinning (joint matrices) | `pbr_shader.c → get_model_matrix()` | `PBR_VS` (uJoints[32]) |
| ACES tone mapping | end of `pbr_shader.c` | `PBR_FS` main() |

## What is reference-only (not yet compiled into ruby)

- `skeleton.c/.h` — the `.ani` keyframe evaluator (quat slerp, TRS compose, inverse-bind,
  joint + normal matrices). Ruby already imports glTF skins/animations natively
  (`src/tools/gltf_import.cpp`) and CPU-skins via `src/tools/pod_loader.h` (`skin_mesh`); this
  file is the reference for a future GPU-skinning path and for `.ani` support.
- `image.c/.h` — TGA + Radiance **HDR** readers. The HDR reader is the missing piece for a
  native IBL environment pipeline (feed `pbr_set_environment()` with a prefiltered cubemap
  from Filament `cmgen` or this reader).
- `blinn_shader.c/.h`, `skybox_shader.c` — the classic Blinn-Phong fallback and cubemap skybox
  reference (Ruby's legacy model program already covers the Blinn case).
- `maths.c/.h` — dependency-free vec/mat/quat math used by the above.

## GLSL port notes

- The vendor shaders are pure functions of (attribs, varyings, uniforms) — the same contract
  GLSL uses — which is why the port is near line-for-line.
- Texture units: basecolor 0, metalness 1, roughness 2, normal 3, occlusion 4, emission 5,
  specular 6, glossiness 7, env cubemap 8, BRDF LUT 9, shadow depth 10.
- `uEnvMaxMip` is 6 for the 64px procedural default environment; scale it with your cubemap
  size if you supply a real one via `av::pbr_set_environment()`.
- The BRDF LUT is the Karis 2013 split-sum integration (256×256 RG16F) — see
  `av_renderer.cpp → generate_brdf_lut()`.

## Verification

`renderer-master/` (the full upstream tree) builds standalone on this machine with plain GCC
(`renderer-master/build_linux.sh`, verified 2026-08-09). Renders from it can be compared
pixel-wise against Ruby's PBR preview for regression testing.
