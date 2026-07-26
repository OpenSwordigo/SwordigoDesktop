# Swordigo OpenSwordigo Research: GroundMesh Materials, Blend Modes & GLSL Shaders

## 1. Ground Shader Shading Architecture

Ground meshes use GLSL shaders capable of rendering multi-layer terrain (surface grass cap vs front rock face) with distance fog, tinting, and dynamic shadow attenuation.

---

## 2. GLSL Ground Mesh Shaders

### Vertex Shader (`caver_ground.vert`)
```glsl
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_texcoord;
attribute vec4 a_color;

uniform mat4 u_mvp;
uniform mat4 u_model_matrix;
uniform vec2 u_uv_scale;
uniform vec2 u_uv_offset;

varying vec2 v_texcoord;
varying vec4 v_color;
varying vec3 v_normal;

void main() {
    v_texcoord = (a_texcoord * u_uv_scale) + u_uv_offset;
    v_color = a_color;
    v_normal = mat3(u_model_matrix) * a_normal;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
```

### Fragment Shader (`caver_ground.frag`)
```glsl
precision mediump float;

varying vec2 v_texcoord;
varying vec4 v_color;
varying vec3 v_normal;

uniform sampler2D u_diffuse_texture;
uniform vec4 u_material_color;
uniform vec3 u_light_direction;

void main() {
    vec4 tex_color = texture2D(u_diffuse_texture, v_texcoord);
    
    // Directional lighting calculation
    vec3 norm = normalize(v_normal);
    float diff = max(dot(norm, -u_light_direction), 0.3); // Minimum 0.3 ambient light
    
    vec4 final_color = tex_color * v_color * u_material_color;
    final_color.rgb *= diff;
    
    gl_FragColor = final_color;
}
```
