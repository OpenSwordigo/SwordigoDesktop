# OpenGL ES 2.0 Rendering Pipeline & FBO Handshake Plan

This document details the research, technical specifications, and implementation steps required to bridge the OpenGL ES 2.0 API, upgrade the guest's basic rendering programs, and link them to the host's FBO post-processing pipeline for high-fidelity rendering.

---

## 1. Bridging the GLES 2.0 APIs (Host Bridge Layer)

Currently, the 64-bit emulator's bridge (`src/jni/jni_bridge_arm64.cpp`) maps several core GLES 2.0 APIs to `bridge_gl_noop` and leaves shader compilation APIs entirely unregistered. To support GLES 2.0 on the host, we must implement and register the following GLES 2.0 handlers:

### A. Shader & Program Management
These functions allow the guest binary to compile and link vertex and fragment shaders:
* **`glCreateShader(GLenum type)`**: Calls host `glCreateShader` and returns the ID.
* **`glCreateProgram()`**: Calls host `glCreateProgram` and returns the ID.
* **`glAttachShader(GLuint program, GLuint shader)`**: Attaches shader to program on the host.
* **`glLinkProgram(GLuint program)`**: Links the shader program.
* **`glCompileShader(GLuint shader)`**: Compiles the shader source.
* **`glUseProgram(GLuint program)`**: Activates the shader program. We must intercept this to inject dynamic lighting uniforms (see Section 3).

### B. Shader Source Extraction (`glShaderSource`)
`glShaderSource` takes double-pointers to characters which must be mapped from guest memory to host space.
* **Signature**: `void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)`
* **AArch64 Bridge Implementation**:
  ```cpp
  static void bridge_glShaderSource(void* emu_ptr) {
      IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
      uint8_t* memory = emu->get_memory_base();
      uint32_t shader = emu->get_reg(0);
      uint32_t count = emu->get_reg(1);
      uint64_t string_ptr = emu->get_reg(2);
      uint64_t length_ptr = emu->get_reg(3);

      std::vector<const GLchar*> sources;
      std::vector<GLint> lengths;

      for (uint32_t i = 0; i < count; i++) {
          uint64_t str_addr = *(uint64_t*)(memory + string_ptr + i * 8);
          sources.push_back((const GLchar*)(memory + str_addr));
          if (length_ptr != 0) {
              lengths.push_back(*(GLint*)(memory + length_ptr + i * 4));
          }
      }
      // Source replacement hooks can be inserted here (Section 2)
      if (g_display_active) {
          glShaderSource(shader, count, sources.data(), length_ptr ? lengths.data() : NULL);
      }
  }
  ```

### C. Vertex Attribute Binding
The attributes (`position`, `color`, `texCoord`) must be bound correctly:
* **`glVertexAttribPointer(index, size, type, normalized, stride, pointer)`**: Maps the pointer correctly. If no VBO is bound, the pointer parameter is a direct guest memory address:
  ```cpp
  GLint bound_vbo = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound_vbo);
  const void* host_ptr = (bound_vbo != 0) ? (const void*)pointer_ptr : (const void*)(memory + pointer_ptr);
  glVertexAttribPointer(index, size, type, normalized, stride, host_ptr);
  ```
* **`glEnableVertexAttribArray(GLuint index)`**: Calls host counterpart.
* **`glDisableVertexAttribArray(GLuint index)`**: Calls host counterpart.

---

## 2. Dynamic Shader Replacement (Upgrade guest shaders)

The guest's default GLES 2.0 shaders are basic flat-shading programs. In `bridge_glShaderSource`, we can intercept shader compilation and replace the source strings with upgraded GLSL shaders that implement lighting and weapon-tanning:

### A. Fragment Shader Upgrades (`PROGRAM_VARYING_COLOR_TEXTURING`)
We replace the fragment shader with a version that supports an ambient term and a point light source (spotlight):
```glsl
precision mediump float;
varying vec4 v_color;
varying vec2 v_texCoord;
uniform sampler2D s_texture;

// Dynamic lighting inputs injected from host bridge
uniform vec3 u_light_pos;
uniform vec3 u_light_color;
uniform vec3 u_ambient_color;
uniform float u_light_radius;

varying vec3 v_world_pos; // Passed from upgraded vertex shader

void main(void) {
    vec4 tex_color = texture2D(s_texture, v_texCoord);
    vec4 base_color = tex_color * v_color;

    // Point-light spotlight calculation
    float dist = distance(v_world_pos, u_light_pos);
    float attenuation = clamp(1.0 - (dist / u_light_radius), 0.0, 1.0);
    vec3 diffuse = u_light_color * attenuation;
    
    // Combine lighting
    vec3 final_light = clamp(u_ambient_color + diffuse, 0.0, 1.0);
    gl_FragColor = vec4(base_color.rgb * final_light, base_color.a);
}
```

---

## 3. Host-Side Uniform Injection

Since the guest engine does not set point lights or ambient color uniforms under GLES 2.0, our host bridge layer must inject these values automatically.

When the guest calls `glUseProgram(program)`:
1. Intercept the call in `bridge_glUseProgram`.
2. Activate the program on the host: `glUseProgram(program)`.
3. Query the program's uniform locations:
   * `u_light_pos`
   * `u_light_color`
   * `u_ambient_color`
   * `u_light_radius`
4. Fetch active gameplay data from the hooked engine variables:
   * `u_light_pos` = Hero's position (`g_hero_obj->position`).
   * `u_light_color` = Dynamic torch light color (usually bright yellow/orange).
   * `u_ambient_color` = Current level ambient light (low in caves, high outdoors).
   * `u_light_radius` = Torch radius (typically `150.0f`).
5. Upload them to the host shader context using `glUniform3f` / `glUniform1f`.

This ensures lighting remains fully dynamic and responsive to gameplay without modifying the guest binary.

---

## 4. Framebuffer Handshake (Link to FBO PostFX)

The handshake between the upgraded guest GLES 2.0 rendering pipeline and our host FBO post-processing shaders is **fully automated and seamless**:

```
┌───────────────────────────────────────────────────────────┐
│              Host FBO Scaler: fbo_begin_game()            │
│  Binds Main FBO color + depth texture                     │
└─────────────────────────────┬─────────────────────────────┘
                              │
                              ▼
┌───────────────────────────────────────────────────────────┐
│            Guest GLES 2.0 Rendering: drawApplication()    │
│  Draws models using upgraded shaders directly into FBO    │
└─────────────────────────────┬─────────────────────────────┘
                              │
                              ▼
┌───────────────────────────────────────────────────────────┐
│         Host FBO Scaler: fbo_end_game_and_blit()          │
│  1. Unbinds main FBO                                      │
│  2. Feeds color/depth textures into Tier 2 PostFX         │
│     (SSAO, directional shadows, god rays, bloom)          │
│  3. Upscales final output to desktop window size          │
└───────────────────────────────────────────────────────────┘
```

### Key Handshake Interactions:
1. **Depth Buffer Synergy**: The guest shaders write depth values into the FBO's `DEPTH24_STENCIL8` attachment. The host SSAO and shadow mapping shaders read this exact depth texture, ensuring that modern post-processed shadows align perfectly with the upgraded GLES 2.0 meshes.
2. **Color Integration**: Upgraded lighting outputs (like metallic weapon glows or torch light rings) are drawn directly into the FBO texture. When the bloom shader runs, it naturally extracts these glowing spots, creating beautiful atmospheric effects.
