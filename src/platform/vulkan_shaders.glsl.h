// vulkan_shaders.glsl.h — GLSL uber-shaders for Vulkan fixed-function GLES1 emulation
// These are embedded as C++ raw string literals, to be compiled to SPIR-V at runtime
// via glslang or offline via glslangValidator.
//
// Design:
//   Push constants (128 bytes, fits minimum guaranteed):
//     mat4 mvp            (64 bytes)  — combined projection * modelview
//     vec4 current_color   (16 bytes)  — glColor4f / glColor4ub state
//     vec4 fog_params      (16 bytes)  — x=start, y=end, z=density, w=mode(0/1/2/3)
//     vec4 fog_color       (16 bytes)  — glFogfv(GL_FOG_COLOR)
//
//   UBO (set=0, binding=1) for lighting/materials/modelview:
//     mat4 modelview, mat4 normal_matrix (upper 3x3),
//     lighting and material parameters, tex env color, alpha ref
//
//   Descriptor set layout:
//     set=0, binding=0: combined image sampler (texture unit 0)
//     set=0, binding=1: uniform buffer (SceneData)
//
//   Specialization constants (fragment shader):
//     0: TEXTURE_ENABLED    (int, 0 or 1)
//     1: LIGHTING_ENABLED   (int, 0 or 1)
//     2: ALPHA_TEST_ENABLED (int, 0 or 1)
//     3: ALPHA_TEST_FUNC    (int, 0-7 maps GL_NEVER..GL_ALWAYS)
//     4: FOG_ENABLED        (int, 0 or 1)
//     5: TEX_ENV_MODE       (int, 0=modulate, 1=replace, 2=decal, 3=blend)
//
//   Vertex inputs:
//     location 0: vec3 position
//     location 1: vec2 texcoord
//     location 2: vec4 color     (vertex color, may be constant 1,1,1,1)
//     location 3: vec3 normal

#pragma once

// ============================================================================
// Vertex Shader
// ============================================================================
static const char* VK_VERT_SHADER_GLSL = R"glsl(
#version 450

// --- Push constants (128 bytes) ---
// Shared range with fragment shader. Vertex shader uses mvp, current_color,
// and fog_params for eye-space fog distance calculation.
layout(push_constant) uniform PushConstants {
    mat4 mvp;            // 64 bytes, offset 0
    vec4 current_color;  // 16 bytes, offset 64
    vec4 fog_params;     // 16 bytes, offset 80 (x=start, y=end, z=density, w=mode)
    vec4 fog_color;      // 16 bytes, offset 96 (used by fragment only, but must match layout)
} pc;
// Total: 112 bytes. 16 bytes of headroom within 128-byte minimum.

// --- UBO for scene/lighting data ---
layout(std140, set = 1, binding = 0) uniform SceneData {
    mat4 modelview;       // eye-space transform
    mat4 normal_matrix;   // transpose(inverse(modelview)), upper 3x3 used
    vec4 light_ambient;   // global ambient + light[0] ambient
    vec4 light_diffuse;   // light[0] diffuse
    vec4 light_dir;       // light[0] direction in eye space (pre-normalized)
    vec4 mat_ambient;     // material ambient
    vec4 mat_diffuse;     // material diffuse
    vec4 mat_emission;    // material emission
    vec4 tex_env_color;   // glTexEnvfv(GL_TEXTURE_ENV_COLOR)
    float alpha_ref;      // glAlphaFunc reference value
    float _pad0;
    float _pad1;
    float _pad2;
} scene;

// --- Vertex inputs ---
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_texcoord;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec3 in_normal;

// --- Outputs to fragment shader ---
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_fog_factor;
layout(location = 3) out vec3 v_normal_eye;
layout(location = 4) out vec3 v_pos_eye;

void main() {
    vec4 pos = vec4(in_position, 1.0);
    gl_Position = pc.mvp * pos;

    // Eye-space position (needed for fog distance and lighting)
    vec4 pos_eye = scene.modelview * pos;
    v_pos_eye = pos_eye.xyz;

    // Pass through texture coordinates
    v_texcoord = in_texcoord;

    // Vertex color modulated by current glColor state
    // When no color array is bound, in_color should be (1,1,1,1)
    // and current_color carries the glColor4f value.
    v_color = in_color * pc.current_color;

    // Normal in eye space (for lighting)
    v_normal_eye = normalize(mat3(scene.normal_matrix) * in_normal);

    // Fog factor calculation
    // fog_params.w: 0=disabled, 1=linear, 2=exp, 3=exp2
    float fog_mode = pc.fog_params.w;
    float dist = length(pos_eye.xyz);

    if (fog_mode > 2.5) {
        // GL_EXP2
        float f = pc.fog_params.z * dist;
        v_fog_factor = clamp(exp(-f * f), 0.0, 1.0);
    } else if (fog_mode > 1.5) {
        // GL_EXP
        v_fog_factor = clamp(exp(-pc.fog_params.z * dist), 0.0, 1.0);
    } else if (fog_mode > 0.5) {
        // GL_LINEAR
        v_fog_factor = clamp(
            (pc.fog_params.y - dist) / (pc.fog_params.y - pc.fog_params.x),
            0.0, 1.0
        );
    } else {
        // No fog
        v_fog_factor = 1.0;
    }
}
)glsl";


// ============================================================================
// Fragment Shader
// ============================================================================
static const char* VK_FRAG_SHADER_GLSL = R"glsl(
#version 450

// --- Specialization constants ---
// These are set at pipeline creation time. The SPIR-V compiler will
// eliminate dead branches, producing an optimized shader for each
// combination of fixed-function state.
layout(constant_id = 0) const int TEXTURE_ENABLED    = 1; // default: textured
layout(constant_id = 1) const int LIGHTING_ENABLED   = 0;
layout(constant_id = 2) const int ALPHA_TEST_ENABLED = 0;
layout(constant_id = 3) const int ALPHA_TEST_FUNC    = 7; // default: GL_ALWAYS (always pass)
layout(constant_id = 4) const int FOG_ENABLED        = 0;
layout(constant_id = 5) const int TEX_ENV_MODE       = 0; // default: GL_MODULATE

// Alpha test function mapping (GL enum -> 0-7):
//   0 = GL_NEVER    (0x0200)
//   1 = GL_LESS     (0x0201)
//   2 = GL_EQUAL    (0x0202)
//   3 = GL_LEQUAL   (0x0203)
//   4 = GL_GREATER  (0x0204)
//   5 = GL_NOTEQUAL (0x0205)
//   6 = GL_GEQUAL   (0x0206)
//   7 = GL_ALWAYS   (0x0207)

// Texture environment mode mapping:
//   0 = GL_MODULATE
//   1 = GL_REPLACE
//   2 = GL_DECAL
//   3 = GL_BLEND
//   4 = GL_ADD

// --- Push constants (must match vertex shader layout) ---
layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 current_color;
    vec4 fog_params;
    vec4 fog_color;
} pc;

// --- UBO (must match vertex shader layout) ---
layout(std140, set = 1, binding = 0) uniform SceneData {
    mat4 modelview;
    mat4 normal_matrix;
    vec4 light_ambient;
    vec4 light_diffuse;
    vec4 light_dir;
    vec4 mat_ambient;
    vec4 mat_diffuse;
    vec4 mat_emission;
    vec4 tex_env_color;
    float alpha_ref;
    float _pad0;
    float _pad1;
    float _pad2;
} scene;

// --- Texture sampler ---
layout(set = 0, binding = 0) uniform sampler2D tex0;

// --- Inputs from vertex shader ---
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_fog_factor;
layout(location = 3) in vec3 v_normal_eye;
layout(location = 4) in vec3 v_pos_eye;

// --- Output ---
layout(location = 0) out vec4 out_color;

void main() {
    // Start with interpolated vertex color
    vec4 base_color = v_color;

    // ---- Lighting ----
    // GLES1 simplified: global_ambient + light0 (directional only)
    // color = emission + ambient * mat_ambient + diffuse * mat_diffuse * NdotL
    if (LIGHTING_ENABLED != 0) {
        vec3 n = normalize(v_normal_eye);
        vec3 l = normalize(scene.light_dir.xyz);
        float NdotL = max(dot(n, l), 0.0);

        vec3 lit = scene.mat_emission.rgb
                 + scene.light_ambient.rgb * scene.mat_ambient.rgb
                 + scene.light_diffuse.rgb * scene.mat_diffuse.rgb * NdotL;

        // Lighting replaces vertex color RGB, keeps vertex alpha
        base_color.rgb = lit;
        base_color.a = v_color.a * scene.mat_diffuse.a;
    }

    // ---- Texturing ----
    vec4 color = base_color;
    if (TEXTURE_ENABLED != 0) {
        vec4 tex_color = texture(tex0, v_texcoord);

        if (TEX_ENV_MODE == 0) {
            // GL_MODULATE: color = texture * base
            color = base_color * tex_color;
        } else if (TEX_ENV_MODE == 1) {
            // GL_REPLACE: color = texture
            color = tex_color;
        } else if (TEX_ENV_MODE == 2) {
            // GL_DECAL: blend texture over base using texture alpha
            color.rgb = mix(base_color.rgb, tex_color.rgb, tex_color.a);
            color.a = base_color.a;
        } else if (TEX_ENV_MODE == 3) {
            // GL_BLEND: blend with tex env color
            color.rgb = mix(base_color.rgb, scene.tex_env_color.rgb, tex_color.rgb);
            color.a = base_color.a * tex_color.a;
        } else {
            // GL_ADD: additive combination
            color.rgb = base_color.rgb + tex_color.rgb;
            color.a = base_color.a * tex_color.a;
            color.rgb = min(color.rgb, vec3(1.0));
        }
    }

    // ---- Alpha test ----
    if (ALPHA_TEST_ENABLED != 0) {
        float ref = scene.alpha_ref;
        bool pass = true;

        if      (ALPHA_TEST_FUNC == 0) pass = false;              // GL_NEVER
        else if (ALPHA_TEST_FUNC == 1) pass = (color.a <  ref);   // GL_LESS
        else if (ALPHA_TEST_FUNC == 2) pass = (color.a == ref);   // GL_EQUAL
        else if (ALPHA_TEST_FUNC == 3) pass = (color.a <= ref);   // GL_LEQUAL
        else if (ALPHA_TEST_FUNC == 4) pass = (color.a >  ref);   // GL_GREATER
        else if (ALPHA_TEST_FUNC == 5) pass = (color.a != ref);   // GL_NOTEQUAL
        else if (ALPHA_TEST_FUNC == 6) pass = (color.a >= ref);   // GL_GEQUAL
        // ALPHA_TEST_FUNC == 7: GL_ALWAYS — pass remains true

        if (!pass) discard;
    }

    // ---- Fog ----
    // v_fog_factor: 1.0 = no fog, 0.0 = fully fogged
    if (FOG_ENABLED != 0) {
        color.rgb = mix(pc.fog_color.rgb, color.rgb, v_fog_factor);
    }

    out_color = color;
}
)glsl";


// ============================================================================
// C++ helper struct matching the push constant layout (for convenience)
// ============================================================================
// Use this when filling VkPushConstantRange and vkCmdPushConstants.
//
//   struct VkPushConstantData {
//       float mvp[16];          // offset 0,  size 64
//       float current_color[4]; // offset 64, size 16
//       float fog_params[4];    // offset 80, size 16 (start, end, density, mode)
//       float fog_color[4];     // offset 96, size 16
//   };
//   static_assert(sizeof(VkPushConstantData) == 112);
//
// The push constant range should be:
//   VkPushConstantRange range = {};
//   range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
//   range.offset = 0;
//   range.size = 112;  // or sizeof(VkPushConstantData)

// ============================================================================
// C++ helper struct matching the UBO layout (std140)
// ============================================================================
// Use this when filling the uniform buffer.
//
//   struct alignas(16) VkSceneUBO {
//       float modelview[16];      // offset 0,   size 64
//       float normal_matrix[16];  // offset 64,  size 64 (upper 3x3 used)
//       float light_ambient[4];   // offset 128, size 16
//       float light_diffuse[4];   // offset 144, size 16
//       float light_dir[4];       // offset 160, size 16 (normalized direction in eye space)
//       float mat_ambient[4];     // offset 176, size 16
//       float mat_diffuse[4];     // offset 192, size 16
//       float mat_emission[4];    // offset 208, size 16
//       float tex_env_color[4];   // offset 224, size 16
//       float alpha_ref;          // offset 240, size 4
//       float _pad[3];            // offset 244, size 12
//   };
//   static_assert(sizeof(VkSceneUBO) == 256);

// ============================================================================
// Specialization constant IDs (for VkSpecializationMapEntry)
// ============================================================================
enum VkShaderSpecConstants : uint32_t {
    SPEC_TEXTURE_ENABLED    = 0,
    SPEC_LIGHTING_ENABLED   = 1,
    SPEC_ALPHA_TEST_ENABLED = 2,
    SPEC_ALPHA_TEST_FUNC    = 3,
    SPEC_FOG_ENABLED        = 4,
    SPEC_TEX_ENV_MODE       = 5,
    SPEC_CONSTANT_COUNT     = 6,
};

// Alpha test function encoding (GL enum & 0x7)
enum VkAlphaTestFunc : int {
    VK_ALPHA_NEVER    = 0, // GL_NEVER    0x0200
    VK_ALPHA_LESS     = 1, // GL_LESS     0x0201
    VK_ALPHA_EQUAL    = 2, // GL_EQUAL    0x0202
    VK_ALPHA_LEQUAL   = 3, // GL_LEQUAL   0x0203
    VK_ALPHA_GREATER  = 4, // GL_GREATER  0x0204
    VK_ALPHA_NOTEQUAL = 5, // GL_NOTEQUAL 0x0205
    VK_ALPHA_GEQUAL   = 6, // GL_GEQUAL   0x0206
    VK_ALPHA_ALWAYS   = 7, // GL_ALWAYS   0x0207
};

// Texture environment mode encoding
enum VkTexEnvMode : int {
    VK_TEXENV_MODULATE = 0, // GL_MODULATE
    VK_TEXENV_REPLACE  = 1, // GL_REPLACE
    VK_TEXENV_DECAL    = 2, // GL_DECAL
    VK_TEXENV_BLEND    = 3, // GL_BLEND
    VK_TEXENV_ADD      = 4, // GL_ADD
};

// Fog mode encoding (stored in fog_params.w)
enum VkFogMode : int {
    VK_FOG_NONE   = 0,
    VK_FOG_LINEAR = 1, // GL_LINEAR  0x2601
    VK_FOG_EXP    = 2, // GL_EXP     0x0800
    VK_FOG_EXP2   = 3, // GL_EXP2    0x0801
};

// ============================================================================
// PostFX Vertex Shader
// ============================================================================
static const char* VK_VERT_POSTFX_GLSL = R"glsl(
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// ============================================================================
// PostFX Fragment Shader
// ============================================================================
static const char* VK_FRAG_POSTFX_GLSL = R"glsl(
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_color_tex;

layout(push_constant) uniform PostFXPushConstants {
    vec4  color_adjust; // x=brightness, y=contrast, z=saturation, w=warmth
    vec4  effects_a;    // x=vignette_intensity, y=grain_intensity, z=ca_offset, w=sharpen_strength
    vec4  effects_b;    // x=time, y=enabled(0/1), z=scale_mode(0=bilinear,1=nearest,2=crt,3=fsr), w=unused
    vec2  tex_size;     // texture resolution (w, h)
    vec2  out_size;     // window resolution (w, h)
} pc;

float rand(vec2 co) {
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    if (pc.effects_b.y < 0.5) {
        out_color = texture(u_color_tex, v_uv);
        return;
    }

    vec2 uv = v_uv;
    int scale_mode = int(pc.effects_b.z);
    
    // 1. Chromatic Aberration
    vec3 col;
    float ca = pc.effects_a.z;
    if (ca > 0.0001) {
        vec2 dist = (uv - 0.5) * ca;
        col.r = texture(u_color_tex, uv - dist).r;
        col.g = texture(u_color_tex, uv).g;
        col.b = texture(u_color_tex, uv + dist).b;
    } else {
        col = texture(u_color_tex, uv).rgb;
    }

    // 2. Color Adjustments (Brightness, Contrast, Saturation, Warmth)
    col += pc.color_adjust.x;

    float contrast = pc.color_adjust.y;
    if (abs(contrast - 1.0) > 0.001) {
        col = (col - 0.5) * contrast + 0.5;
    }

    float sat = pc.color_adjust.z;
    if (abs(sat - 1.0) > 0.001) {
        float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(luma), col, sat);
    }

    float warmth = pc.color_adjust.w;
    if (abs(warmth) > 0.001) {
        col.r += warmth * 0.1;
        col.b -= warmth * 0.1;
    }

    // 3. Sharpening
    float sharpen = pc.effects_a.w;
    if (sharpen > 0.001 && pc.tex_size.x > 0.0) {
        vec2 step_val = 1.0 / pc.tex_size;
        vec3 center = col;
        vec3 n = texture(u_color_tex, uv + vec2(0.0, -step_val.y)).rgb;
        vec3 s = texture(u_color_tex, uv + vec2(0.0, step_val.y)).rgb;
        vec3 e = texture(u_color_tex, uv + vec2(step_val.x, 0.0)).rgb;
        vec3 w = texture(u_color_tex, uv + vec2(-step_val.x, 0.0)).rgb;
        vec3 sharp = center * 5.0 - n - s - e - w;
        col = mix(col, sharp, sharpen * 0.5);
    }

    // 4. Vignette
    float vig_intensity = pc.effects_a.x;
    if (vig_intensity > 0.001) {
        float d = distance(uv, vec2(0.5));
        float vig = smoothstep(0.8, 0.2, d * (1.0 + vig_intensity));
        col *= mix(1.0 - vig_intensity, 1.0, vig);
    }

    // 5. Film Grain
    float grain_intensity = pc.effects_a.y;
    if (grain_intensity > 0.001) {
        float g = (rand(uv + pc.effects_b.x * 0.05) - 0.5) * grain_intensity;
        col += vec3(g);
    }

    // 6. CRT Scanlines
    if (scale_mode == 2) {
        float scanline = sin(uv.y * pc.out_size.y * 3.14159) * 0.15;
        col -= scanline;
    }

    out_color = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)glsl";

