# Vanilla PostFX & Shading Research Report

This document analyzes the differences between the **ARM32 (GLES 1.1)** and **ARM64 (GLES 2.0)** guest rendering pipelines in the Caver engine. It explains why vanilla lighting effects (caves darkness, player spotlights, and weapon color/tanning) work in our ARM32 build but fail in ARM64, and outlines how we can use these findings to enhance our host-side FBO shaders.

---

## 1. The Core Architecture Conflict: GLES 1.1 vs. GLES 2.0

Our desktop port launcher supports running both `armeabi-v7a` (32-bit) and `arm64-v8a` (64-bit) guest libraries. However, they initialize completely different rendering paths:

### ARM32 (Fixed-Function GLES 1.1)
The 32-bit guest library initializes the fixed-function pipeline. All rendering is routed to GLES 1.1 fixed-function APIs which map to host OpenGL 2.1 compatibility calls:
* Lighting and material states are uploaded using fixed-function APIs: `glLightfv`, `glMaterialfv`, and `glLightModelfv`.
* The GPU's fixed-function hardware pipeline natively calculates vertex lighting, ambient colors, and material-color modulation (tinting).
* **Result**: In-game darkness, torches, and metallic weapon tinting render perfectly.

### ARM64 (Programmable GLES 2.0)
The 64-bit guest library initializes the programmable pipeline (`*(int*)param_1 == 1` in `RenderingContext`). In this mode, the Caver engine bypasses GLES 1.1 fixed-function hardware pipeline calls entirely:
* **Lighting State Skipped**: In `LightComponent::Enable`, all `glLightfv` and `glLightModelfv` calls are conditionally skipped under GLES 2.0. No light properties are uploaded to the shader pipeline.
* **Material Colors Skipped**: In `ModelInstance::Draw`, when lighting is enabled, the code skips `glMaterialfv` and does **not** call `SetColor` on the rendering context.
* **Basic Shaders**: The embedded shaders compiled in `BasicRenderingPrograms.c` (e.g. `PROGRAM_VARYING_COLOR_TEXTURING`) are extremely basic. They do not implement any diffuse, specular, or ambient lighting math.
* **Result**: All models (hero, weapons, enemies) render completely flat and unlit, ignoring scene lights, darkness levels, and weapon tints.

---

## 2. How Vanilla Effects Work (Decompiled Analysis)

By analyzing [LightComponent.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoTools/GhidraDecomp%20src/render/LightComponent.c#L573-L673) and [ModelInstance.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoTools/GhidraDecomp%20src/render/ModelInstance.c#L423-L499), we can see exactly how the engine implements these effects in GLES 1.1:

### A. Cave Darkness & Spotlight
In dark areas (like caves), the engine does the following:
1. Sets a low global ambient light level using:
   ```c
   glLightModelfv(GL_LIGHT_MODEL_AMBIENT, &ambient_color);
   ```
2. Attaches a dynamic spotlight (`GL_LIGHT0`) to the hero's position.
3. Sets the light properties in `LightComponent::Enable()`:
   ```c
   glLightfv(light_id, GL_POSITION, &light_pos);
   glLightfv(light_id, GL_DIFFUSE, &light_diffuse);
   glLightf(light_id, GL_CONSTANT_ATTENUATION, constant_att);
   glLightf(light_id, GL_LINEAR_ATTENUATION, linear_att);
   glLightf(light_id, GL_QUADRATIC_ATTENUATION, quadratic_att);
   ```
4. This restricts the visible light area to a circle around the player, while the rest of the cave remains dark.

### B. Weapon Tanning & Tinting (Brass Sword)
The custom colors of materials (such as the bronze/brass tint of the starting sword, or magical weapon glows) are applied per-model during draw:
1. `ModelInstance::Draw()` reads the material properties from the model file:
   ```c
   // Front & back ambient/diffuse colors
   glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, &diffuse_color);
   glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, &ambient_color);
   ```
2. The fixed-function pipeline blends these material colors with the texture map based on active light sources, creating the classic "tanned" brass sword look.

---

## 3. How We Can Enhance Our FBO Shaders (Key Takeaways)

Since GLES 2.0 mode on ARM64 lacks these effects, we can implement them directly within our host-side FBO post-processing shaders (`fbo_scaler.cpp`) to create a superior, hardware-accelerated result:

### A. Dynamic Torch / Spotlight Simulation
We can emulate cave darkness in our FBO shader (`FRAG_POSTFX`) dynamically:
1. **Pass Player Screen Position**: We can capture the player character's world position from the hooked update loops (e.g. `g_hero_obj`) and project it to screen space using the camera view-projection matrix. Pass this as `uniform vec2 u_player_screen_pos`.
2. **Pass Cave Darkness State**: Pass the level darkness factor (or query background ambient intensity) as `uniform float u_cave_darkness`.
3. **Radial Shading**: In the fragment shader, calculate the distance from the current fragment `v_uv` to `u_player_screen_pos`:
   ```glsl
   float dist = length(v_uv - u_player_screen_pos);
   float light_mask = smoothstep(0.25, 0.08, dist); // Spotlight radius
   col.rgb *= mix(1.0, light_mask * 1.2 + 0.15, u_cave_darkness);
   ```
   This recreates the torch effect with smooth, resolution-independent shading.

### B. Material Highlight & Glow Extraction (Bloom Enhancement)
In GLES 2.0, magical effects and charged weapon glow can be amplified:
1. **Enhance Bloom Shader**: Modify our bloom extraction filter (`FRAG_BLOOM_EXTRACT` in `fbo_scaler.cpp`) to isolate vibrant colors typical of glowing magic swords (e.g. high-frequency blues/purples/reds) even if their luminance is below the normal threshold:
   ```glsl
   // Target sword glowing ranges specifically for bloom
   bool is_glow = (col.b > 0.8 && col.r < 0.3) || (col.r > 0.8 && col.g < 0.3);
   if (is_glow) {
       FragColor = col * 1.5;
   }
   ```
2. **Weapon outlines**: Since we already run an edge-detection outline filter on depth transitions, we can colorize outline segments corresponding to the player's weapon with a glowing color (e.g. bright blue for magic weapon, orange/red for fire trinket) to make the weapon appear charged.
