/* av_renderer.cpp — Modern OpenGL 3.3 renderer implementation
 *
 * Self-contained renderer for the Swordigo Asset Viewer.
 * Uses Lambertian lighting faithful to Swordigo's original model:
 *   lit = emission + ambient*mat_ambient + diffuse*mat_diffuse*max(dot(N,L),0)
 *
 * All matrix math is inline (no GLM dependency).
 * Requires GL_GLEXT_PROTOTYPES for modern GL 3.3 functions on Linux.
 */

// Enable GL extension prototypes BEFORE any GL includes
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "av_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace av {

// ============================================================================
// Constants
// ============================================================================

static constexpr float PI = 3.14159265358979323846f;
static constexpr float DEG2RAD = PI / 180.0f;

// Global lighting (warm directional, slight cool ambient — Swordigo-faithful by default)
// Vanilla Swordigo: a warm sun from above, deep dark cave ambient, strong
// top-vs-side distinction so platforms read as lit on top and shaded below.
float g_light_dir[3]   = { 0.35f,  0.85f,  0.39f };  // mostly-down sun
float g_light_color[3] = { 1.05f,  0.92f,  0.72f };  // warm golden
float g_ambient_color[3]     = { 0.16f,   0.17f,   0.22f };  // dark cool cave fill
float g_clear_color[3]       = { 0.055f,  0.058f,  0.08f };

// ============================================================================
// Shader sources — GLSL 330 core
// ============================================================================

// --- Model shaders (Lambertian + texture) ---

static const char* MODEL_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uNormalMat;

out vec3 vNormal;
out vec3 vWorldPos;
out vec2 vUV;

void main() {
    vNormal   = normalize(uNormalMat * aNorm);
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    vUV       = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* MODEL_FS = R"GLSL(
#version 330 core
in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUV;

uniform sampler2D uTexture;
uniform bool  uHasTexture;
uniform vec3  uLightDir;        // normalized direction TO the light
uniform vec3  uLightColor;      // directional light color
uniform vec3  uAmbient;         // ambient light color
uniform vec4  uMatColor;        // material base color (when untextured)
uniform float uAlpha;           // overall alpha multiplier
uniform vec3  uCamPos;          // camera position (for point lights)
uniform bool  uFogEnabled;
uniform vec3  uFogColor;        // atmospheric depth-darkening tint
uniform float uFogNear;         // distance where fog starts
uniform float uFogFar;          // distance where fog is full

#define MAX_POINT_LIGHTS 16
uniform int    uLightCount;
uniform vec3   uLightPos[MAX_POINT_LIGHTS];
uniform vec3   uLightCol[MAX_POINT_LIGHTS];
uniform float  uLightRadius[MAX_POINT_LIGHTS];

out vec4 FragColor;

void main() {
    vec4  base = uHasTexture ? texture(uTexture, vUV) : uMatColor;
    vec3  N    = normalize(vNormal);
    float NdotL = max(dot(N, uLightDir), 0.0);
    vec3  lit   = base.rgb * (uAmbient + uLightColor * NdotL);

    // Point lights (warm torch/fire glow) with smooth distance falloff.
    for (int i = 0; i < uLightCount; ++i) {
        vec3  toL   = uLightPos[i] - vWorldPos;
        float dist  = length(toL);
        float r     = max(uLightRadius[i], 0.001);
        float atten = 1.0 - smoothstep(0.0, r, dist);
        float diff  = max(dot(N, normalize(toL)), 0.0);
        lit += base.rgb * uLightCol[i] * diff * atten * 2.0;
    }

    // Atmospheric depth fog: distant geometry darkens toward the cave tint,
    // visually separating background layers like vanilla Swordigo.
    if (uFogEnabled) {
        float camDist = length(vWorldPos - uCamPos);
        float f = smoothstep(uFogNear, uFogFar, camDist);
        // Mix toward a dark fog color modulated by the ambient so fully distant
        // surfaces read as shadowed background rather than hue-preserved.
        vec3 fogged = uFogColor * (uAmbient * 2.0 + 0.02);
        lit = mix(lit, fogged, f * f);
    }

    FragColor = vec4(lit, base.a * uAlpha);
}
)GLSL";

// --- Grid shaders (XZ plane with fade-to-transparent) ---

static const char* GRID_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec3 vWorldPos;

void main() {
    vWorldPos   = vec3(uModel * vec4(aPos, 1.0));
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* GRID_FS = R"GLSL(
#version 330 core
in vec3 vWorldPos;

uniform float uGridSize;
uniform vec4  uGridColor;

out vec4 FragColor;

void main() {
    // Distance-based fade from center
    float dist = length(vWorldPos.xz);
    float fade = 1.0 - smoothstep(uGridSize * 0.65, uGridSize, dist);

    // One world-unit minor grid, with thicker ten-unit major lines.
    vec2 minorCoord = abs(fract(vWorldPos.xz) - 0.5);
    vec2 majorCoord = abs(fract(vWorldPos.xz / 10.0) - 0.5);
    float minorLine = 1.0 - smoothstep(0.46, 0.49, max(minorCoord.x, minorCoord.y));
    float majorLine = 1.0 - smoothstep(0.47, 0.495, max(majorCoord.x, majorCoord.y));
    float line = max(minorLine * 0.45, majorLine);

    FragColor = vec4(uGridColor.rgb, uGridColor.a * line * fade);
}
)GLSL";

// --- Background quad shaders (unlit, textured, depth-write off) ---
// Matches the reference editor's background handling: a MeshBasicMaterial-like
// textured plane drawn first, never occluding the level.

static const char* BG_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* BG_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    FragColor = texture(uTexture, vUV);
}
)GLSL";

// --- Glow sprite shaders (additive billboards for torch/fire bloom) ---
// A radial soft falloff sprite; alpha is the sprite mask and brightness is
// multiplied by uColor so values above 1.0 feed the bloom bright-pass.
static const char* GLOW_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* GLOW_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform vec3 uColor;

out vec4 FragColor;

void main() {
    // Radial falloff: bright core, soft edge.
    vec2  c = vUV - 0.5;
    float d = length(c) * 2.0;
    float falloff = 1.0 - smoothstep(0.15, 1.0, d);
    FragColor = vec4(uColor * falloff, falloff);
}
)GLSL";

// Water / fluid sheet shader (unlit, tinted, animated UV)
static const char* WATER_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* WATER_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform sampler2D uTexture;
uniform bool     uHasTex;
uniform vec4     uTint;      // RGBA material tint (front or surface color)
uniform float    uScroll;    // UV scroll phase (drives texture flow)

out vec4 FragColor;

void main() {
    vec2 uv = vUV + vec2(uScroll, uScroll * 0.35);
    vec4 tex = uHasTex ? texture(uTexture, uv) : vec4(1.0);
    vec3 col = tex.rgb * uTint.rgb;
    float a = uTint.a * (uHasTex ? tex.a : 1.0);
    // Soft surface sheen: brighten toward the top of the sheet.
    col += vec3(0.06) * (1.0 - vUV.y);
    FragColor = vec4(col, a);
}
)GLSL";

// ============================================================================
// Internal state
// ============================================================================

static GLuint s_model_prog = 0;
static GLuint s_grid_prog  = 0;

// Glow sprite program + geometry (camera-facing billboard)
static GLuint s_glow_prog = 0;
static GLuint s_glow_vao = 0, s_glow_vbo = 0, s_glow_ebo = 0;
static GLint  s_glow_loc_mvp = -1, s_glow_loc_color = -1;

// Water / fluid sheet program + dynamic geometry (rebuilt per frame)
static GLuint s_water_prog = 0;
static GLuint s_water_vao = 0, s_water_vbo = 0, s_water_ebo = 0;
static GLint  s_water_loc_mvp = -1, s_water_loc_tex = -1;
static GLint  s_water_loc_has_tex = -1, s_water_loc_tint = -1;
static GLint  s_water_loc_scroll = -1;

// Background quad program + geometry (unlit textured plane, depth-write off)
static GLuint s_bg_prog = 0;
static GLuint s_bg_vao = 0, s_bg_vbo = 0, s_bg_ebo = 0;
static GLint  s_bg_loc_mvp = -1, s_bg_loc_tex = -1;

// Model shader uniform locations
static GLint s_loc_mvp        = -1;
static GLint s_loc_model      = -1;
static GLint s_loc_normalmat  = -1;
static GLint s_loc_texture    = -1;
static GLint s_loc_has_tex    = -1;
static GLint s_loc_light_dir  = -1;
static GLint s_loc_light_col  = -1;
static GLint s_loc_ambient    = -1;
static GLint s_loc_mat_color  = -1;
static GLint s_loc_alpha      = -1;
static GLint s_loc_cam_pos    = -1;
static GLint s_loc_light_cnt  = -1;
static GLint s_loc_light_pos  = -1;
static GLint s_loc_light_cols = -1;
static GLint s_loc_light_rad  = -1;
static GLint s_loc_fog_en     = -1;
static GLint s_loc_fog_col    = -1;
static GLint s_loc_fog_near   = -1;
static GLint s_loc_fog_far    = -1;

// Depth fog state (set per-frame; disabled by default so model previews stay clear)
static bool   s_fog_enabled = false;
static float  s_fog_color[3] = {0.05f, 0.06f, 0.09f};  // dark cave tint
static float  s_fog_near = 400.0f;
static float  s_fog_far  = 1400.0f;

// Scene point lights uploaded per-frame (positions in world space).
static float s_point_light_pos[16][3]  = {{0}};
static float s_point_light_col[16][3]  = {{0}};
static float s_point_light_radius[16]  = {0};
static int   s_point_light_count = 0;

// Grid shader uniform locations
static GLint s_loc_grid_mvp   = -1;
static GLint s_loc_grid_model = -1;
static GLint s_loc_grid_size  = -1;
static GLint s_loc_grid_color = -1;

// Grid geometry (a simple XZ quad — large enough, faded at edges)
static GLuint s_grid_vao = 0;
static GLuint s_grid_vbo = 0;

// Currently active view/proj matrices (set by begin_3d)
static float s_view[16];
static float s_cam_eye[3] = {0, 0, 0};   // camera eye, set by begin_3d (point lights)
static float s_proj[16];
static float s_vp[16];   // view * proj combined

// Saved viewport for end_3d restore
static GLint s_saved_viewport[4] = {0, 0, 0, 0};
static GLuint s_saved_fbo = 0;

// Depth textures for FBOs (sampleable — the PostFX depth-of-field pass reads
// them). We maintain a map-free approach: store one depth texture per FBO via
// a small lookup (asset viewer typically has 1–2 FBOs).
struct FBORecord { GLuint fbo; GLuint depth_tex; };
static std::vector<FBORecord> s_fbo_records;

static GLuint find_depth_for_fbo(GLuint fbo) {
    for (auto& r : s_fbo_records)
        if (r.fbo == fbo) return r.depth_tex;
    return 0;
}

// ============================================================================
// Mat4 math — column-major, right-handed
// ============================================================================

void mat4_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_multiply(float out[16], const float a[16], const float b[16]) {
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_translate(float out[16], float tx, float ty, float tz) {
    mat4_identity(out);
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
}

void mat4_rotate_x(float out[16], float angle_deg) {
    mat4_identity(out);
    float rad = angle_deg * DEG2RAD;
    float c = cosf(rad), s = sinf(rad);
    out[5]  =  c;  out[6]  = s;
    out[9]  = -s;  out[10] = c;
}

void mat4_rotate_y(float out[16], float angle_deg) {
    mat4_identity(out);
    float rad = angle_deg * DEG2RAD;
    float c = cosf(rad), s = sinf(rad);
    out[0]  =  c;  out[2]  = -s;
    out[8]  =  s;  out[10] =  c;
}

void mat4_rotate_z(float out[16], float angle_deg) {
    mat4_identity(out);
    float rad = angle_deg * DEG2RAD;
    float c = cosf(rad), s = sinf(rad);
    out[0]  =  c;  out[1]  =  s;
    out[4]  = -s;  out[5]  =  c;
}

void mat4_perspective(float out[16], float fov_deg, float aspect,
                      float near_p, float far_p) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fov_deg * DEG2RAD * 0.5f);
    out[0]  = f / aspect;
    out[5]  = f;
    out[10] = (far_p + near_p) / (near_p - far_p);
    out[11] = -1.0f;
    out[14] = (2.0f * far_p * near_p) / (near_p - far_p);
}

void mat4_look_at(float out[16],
                  float ex, float ey, float ez,
                  float cx, float cy, float cz,
                  float ux, float uy, float uz) {
    // Forward = normalize(center - eye)
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    if (flen > 1e-8f) { fx /= flen; fy /= flen; fz /= flen; }

    // Side = normalize(forward × up)
    float sx = fy*uz - fz*uy;
    float sy = fz*ux - fx*uz;
    float sz = fx*uy - fy*ux;
    float slen = sqrtf(sx*sx + sy*sy + sz*sz);
    if (slen > 1e-8f) { sx /= slen; sy /= slen; sz /= slen; }

    // Recompute up = side × forward
    float rx = sy*fz - sz*fy;
    float ry = sz*fx - sx*fz;
    float rz = sx*fy - sy*fx;

    // Column-major layout
    mat4_identity(out);
    out[0] =  sx;  out[4] =  sy;  out[8]  =  sz;
    out[1] =  rx;  out[5] =  ry;  out[9]  =  rz;
    out[2] = -fx;  out[6] = -fy;  out[10] = -fz;
    out[12] = -(sx*ex + sy*ey + sz*ez);
    out[13] = -(rx*ex + ry*ey + rz*ez);
    out[14] =  (fx*ex + fy*ey + fz*ez);
}

/// Extract the upper-left 3×3 from a column-major 4×4 and compute its
/// inverse-transpose for correct normal transformation.
/// Output is 9 floats in column-major order (what glUniformMatrix3fv expects).
void mat4_normal_matrix(float out[9], const float m[16]) {
    // Extract 3x3 (column-major)
    float a00 = m[0], a01 = m[4], a02 = m[8];
    float a10 = m[1], a11 = m[5], a12 = m[9];
    float a20 = m[2], a21 = m[6], a22 = m[10];

    // Cofactors
    float c00 =  (a11*a22 - a12*a21);
    float c01 = -(a10*a22 - a12*a20);
    float c02 =  (a10*a21 - a11*a20);
    float c10 = -(a01*a22 - a02*a21);
    float c11 =  (a00*a22 - a02*a20);
    float c12 = -(a00*a21 - a01*a20);
    float c20 =  (a01*a12 - a02*a11);
    float c21 = -(a00*a12 - a02*a10);
    float c22 =  (a00*a11 - a01*a10);

    float det = a00*c00 + a01*c01 + a02*c02;
    if (fabsf(det) < 1e-12f) {
        // Degenerate — fall back to identity
        memset(out, 0, 9 * sizeof(float));
        out[0] = out[4] = out[8] = 1.0f;
        return;
    }

    float inv_det = 1.0f / det;

    // Inverse-transpose = cofactor / det  (already transposed by cofactor layout)
    // Output column-major: col 0 = row 0 of cofactor matrix / det, etc.
    out[0] = c00 * inv_det;  out[3] = c01 * inv_det;  out[6] = c02 * inv_det;
    out[1] = c10 * inv_det;  out[4] = c11 * inv_det;  out[7] = c12 * inv_det;
    out[2] = c20 * inv_det;  out[5] = c21 * inv_det;  out[8] = c22 * inv_det;
}

void camera_get_view_matrix(const Camera& cam, float out[16]) {
    // Spherical coordinates → eye position
    float yaw_rad   = cam.yaw   * DEG2RAD;
    float pitch_rad = cam.pitch * DEG2RAD;

    float cos_p = cosf(pitch_rad);
    float eye_x = cam.target[0] + cam.distance * cos_p * sinf(yaw_rad);
    float eye_y = cam.target[1] + cam.distance * sinf(pitch_rad);
    float eye_z = cam.target[2] + cam.distance * cos_p * cosf(yaw_rad);

    mat4_look_at(out,
                 eye_x, eye_y, eye_z,
                 cam.target[0], cam.target[1], cam.target[2],
                 0.0f, 1.0f, 0.0f);
}

void camera_get_projection(const Camera& cam, float aspect, float out[16]) {
    mat4_perspective(out, cam.fov, aspect, cam.near_plane, cam.far_plane);
}

bool mat4_inverse(float out[16], const float m[16]) {
    // Cofactor expansion with partial-pivot-free determinant check.
    const float a00 = m[0], a01 = m[4], a02 = m[8],  a03 = m[12];
    const float a10 = m[1], a11 = m[5], a12 = m[9],  a13 = m[13];
    const float a20 = m[2], a21 = m[6], a22 = m[10], a23 = m[14];
    const float a30 = m[3], a31 = m[7], a32 = m[11], a33 = m[15];

    const float b00 = a00*a11 - a01*a10;
    const float b01 = a00*a12 - a02*a10;
    const float b02 = a00*a13 - a03*a10;
    const float b03 = a01*a12 - a02*a11;
    const float b04 = a01*a13 - a03*a11;
    const float b05 = a02*a13 - a03*a12;
    const float b06 = a20*a31 - a21*a30;
    const float b07 = a20*a32 - a22*a30;
    const float b08 = a20*a33 - a23*a30;
    const float b09 = a21*a32 - a22*a31;
    const float b10 = a21*a33 - a23*a31;
    const float b11 = a22*a33 - a23*a32;

    const float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    if (det == 0.0f) return false;
    const float inv = 1.0f / det;

    // The cofactor block below yields the inverse in a transposed (row-major)
    // arrangement.  Compute it into a local, then transpose into the standard
    // column-major layout so that mat4_multiply(out, m) == identity.
    float t[16];
    t[0]  =  (a11*b11 - a12*b10 + a13*b09) * inv;
    t[1]  = -(a01*b11 - a02*b10 + a03*b09) * inv;
    t[2]  =  (a31*b05 - a32*b04 + a33*b03) * inv;
    t[3]  = -(a21*b05 - a22*b04 + a23*b03) * inv;
    t[4]  = -(a10*b11 - a12*b08 + a13*b07) * inv;
    t[5]  =  (a00*b11 - a02*b08 + a03*b07) * inv;
    t[6]  = -(a30*b05 - a32*b02 + a33*b01) * inv;
    t[7]  =  (a20*b05 - a22*b02 + a23*b01) * inv;
    t[8]  =  (a10*b10 - a11*b08 + a13*b06) * inv;
    t[9]  = -(a00*b10 - a01*b08 + a03*b06) * inv;
    t[10] =  (a30*b04 - a31*b02 + a33*b00) * inv;
    t[11] = -(a20*b04 - a21*b02 + a23*b00) * inv;
    t[12] = -(a10*b09 - a11*b07 + a12*b06) * inv;
    t[13] =  (a00*b09 - a01*b07 + a02*b06) * inv;
    t[14] = -(a30*b03 - a31*b01 + a32*b00) * inv;
    t[15] =  (a20*b03 - a21*b01 + a22*b00) * inv;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out[c * 4 + r] = t[r * 4 + c];
    return true;
}

// ============================================================================
// Internal helpers — shader compilation
// ============================================================================

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        fprintf(stderr, "[av_renderer] Shader compile error:\n%s\n", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[av_renderer] Program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static GLuint build_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = link_program(vs, fs);

    // Shaders are ref-counted; safe to delete after linking
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// ============================================================================
// Grid geometry setup (a single large XZ quad)
// ============================================================================

static void create_grid_geometry() {
    // Unit quad from -1 to +1 on XZ, centered at origin.
    // Actual size is scaled by render_grid's `size` parameter via the model matrix.
    // clang-format off
    static const float verts[] = {
        // x,   y,  z
        -1.0f, 0.0f, -1.0f,
         1.0f, 0.0f, -1.0f,
         1.0f, 0.0f,  1.0f,
        -1.0f, 0.0f,  1.0f,
    };
    static const uint16_t indices[] = { 0, 1, 2,  0, 2, 3 };
    // clang-format on

    glGenVertexArrays(1, &s_grid_vao);
    glBindVertexArray(s_grid_vao);

    glGenBuffers(1, &s_grid_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // location 0 = position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glBindVertexArray(0);
}

// ============================================================================
// Public API — lifecycle
// ============================================================================

bool renderer_init() {
    // --- Model program ---
    s_model_prog = build_program(MODEL_VS, MODEL_FS);
    if (!s_model_prog) {
        fprintf(stderr, "[av_renderer] Failed to build model shader program.\n");
        return false;
    }

    s_loc_mvp       = glGetUniformLocation(s_model_prog, "uMVP");
    s_loc_model     = glGetUniformLocation(s_model_prog, "uModel");
    s_loc_normalmat = glGetUniformLocation(s_model_prog, "uNormalMat");
    s_loc_texture   = glGetUniformLocation(s_model_prog, "uTexture");
    s_loc_has_tex   = glGetUniformLocation(s_model_prog, "uHasTexture");
    s_loc_light_dir = glGetUniformLocation(s_model_prog, "uLightDir");
    s_loc_light_col = glGetUniformLocation(s_model_prog, "uLightColor");
    s_loc_ambient   = glGetUniformLocation(s_model_prog, "uAmbient");
    s_loc_mat_color = glGetUniformLocation(s_model_prog, "uMatColor");
    s_loc_cam_pos   = glGetUniformLocation(s_model_prog, "uCamPos");
    s_loc_light_cnt = glGetUniformLocation(s_model_prog, "uLightCount");
    s_loc_light_pos = glGetUniformLocation(s_model_prog, "uLightPos[0]");
    s_loc_light_cols= glGetUniformLocation(s_model_prog, "uLightCol[0]");
    s_loc_light_rad = glGetUniformLocation(s_model_prog, "uLightRadius[0]");
    s_loc_fog_en    = glGetUniformLocation(s_model_prog, "uFogEnabled");
    s_loc_fog_col   = glGetUniformLocation(s_model_prog, "uFogColor");
    s_loc_fog_near  = glGetUniformLocation(s_model_prog, "uFogNear");
    s_loc_fog_far   = glGetUniformLocation(s_model_prog, "uFogFar");
    s_loc_alpha     = glGetUniformLocation(s_model_prog, "uAlpha");

    // --- Grid program ---
    s_grid_prog = build_program(GRID_VS, GRID_FS);
    if (!s_grid_prog) {
        fprintf(stderr, "[av_renderer] Failed to build grid shader program.\n");
        glDeleteProgram(s_model_prog);
        s_model_prog = 0;
        return false;
    }

    s_loc_grid_mvp   = glGetUniformLocation(s_grid_prog, "uMVP");
    s_loc_grid_model = glGetUniformLocation(s_grid_prog, "uModel");
    s_loc_grid_size  = glGetUniformLocation(s_grid_prog, "uGridSize");
    s_loc_grid_color = glGetUniformLocation(s_grid_prog, "uGridColor");

    // --- Grid geometry ---
    create_grid_geometry();

    // --- Background quad geometry (unit quad ±1 on X/Y, UV 0..1) ---
    s_bg_prog = build_program(BG_VS, BG_FS);
    if (s_bg_prog) {
        s_bg_loc_mvp = glGetUniformLocation(s_bg_prog, "uMVP");
        s_bg_loc_tex = glGetUniformLocation(s_bg_prog, "uTexture");

        static const float bg_verts[] = {
            // x,    y,    z,   u,   v
            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 0.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
        };
        static const uint16_t bg_idx[] = { 0, 1, 2, 0, 2, 3 };

        glGenVertexArrays(1, &s_bg_vao);
        glBindVertexArray(s_bg_vao);
        glGenBuffers(1, &s_bg_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_bg_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bg_verts), bg_verts, GL_STATIC_DRAW);
        glGenBuffers(1, &s_bg_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_bg_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(bg_idx), bg_idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[av_renderer] Failed to build background quad program.\n");
    }

    // --- Glow sprite geometry (unit quad ±1 on X/Y, UV 0..1) ---
    s_glow_prog = build_program(GLOW_VS, GLOW_FS);
    if (s_glow_prog) {
        s_glow_loc_mvp   = glGetUniformLocation(s_glow_prog, "uMVP");
        s_glow_loc_color = glGetUniformLocation(s_glow_prog, "uColor");

        static const float glow_verts[] = {
            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 0.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
        };
        static const uint16_t glow_idx[] = { 0, 1, 2, 0, 2, 3 };

        glGenVertexArrays(1, &s_glow_vao);
        glBindVertexArray(s_glow_vao);
        glGenBuffers(1, &s_glow_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_glow_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(glow_verts), glow_verts, GL_STATIC_DRAW);
        glGenBuffers(1, &s_glow_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_glow_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(glow_idx), glow_idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[av_renderer] Failed to build glow sprite program.\n");
    }

    // --- Water / fluid sheet program + dynamic geometry ---
    // The vertex buffer is re-filled every frame (grid waves), so it is
    // created with GL_DYNAMIC_DRAW in render_water_sheet; here we only set
    // up the attribute layout once.
    s_water_prog = build_program(WATER_VS, WATER_FS);
    if (s_water_prog) {
        s_water_loc_mvp     = glGetUniformLocation(s_water_prog, "uMVP");
        s_water_loc_tex     = glGetUniformLocation(s_water_prog, "uTexture");
        s_water_loc_has_tex = glGetUniformLocation(s_water_prog, "uHasTex");
        s_water_loc_tint    = glGetUniformLocation(s_water_prog, "uTint");
        s_water_loc_scroll  = glGetUniformLocation(s_water_prog, "uScroll");

        glGenVertexArrays(1, &s_water_vao);
        glBindVertexArray(s_water_vao);
        glGenBuffers(1, &s_water_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_water_vbo);
        // Positions (3) + UV (2) = 5 floats; indices are uint16 (<= 65k verts).
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glGenBuffers(1, &s_water_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_water_ebo);
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[av_renderer] Failed to build water sheet program.\n");
    }

    fprintf(stderr, "[av_renderer] Initialized (model prog=%u, grid prog=%u).\n",
            s_model_prog, s_grid_prog);
    return true;
}

void renderer_shutdown() {
    if (s_model_prog) { glDeleteProgram(s_model_prog); s_model_prog = 0; }
    if (s_grid_prog)  { glDeleteProgram(s_grid_prog);  s_grid_prog  = 0; }

    if (s_grid_vao) {
        glDeleteVertexArrays(1, &s_grid_vao);
        s_grid_vao = 0;
    }
    if (s_grid_vbo) {
        glDeleteBuffers(1, &s_grid_vbo);
        s_grid_vbo = 0;
    }

    // Clean up tracked FBO depth textures
    for (auto& r : s_fbo_records) {
        if (r.depth_tex) glDeleteTextures(1, &r.depth_tex);
    }
    s_fbo_records.clear();

    if (s_bg_prog) { glDeleteProgram(s_bg_prog); s_bg_prog = 0; }
    if (s_bg_vao)  { glDeleteVertexArrays(1, &s_bg_vao); s_bg_vao = 0; }
    if (s_bg_vbo)  { glDeleteBuffers(1, &s_bg_vbo); s_bg_vbo = 0; }
    if (s_bg_ebo)  { glDeleteBuffers(1, &s_bg_ebo); s_bg_ebo = 0; }

    if (s_glow_prog) { glDeleteProgram(s_glow_prog); s_glow_prog = 0; }
    if (s_glow_vao)  { glDeleteVertexArrays(1, &s_glow_vao); s_glow_vao = 0; }
    if (s_glow_vbo)  { glDeleteBuffers(1, &s_glow_vbo); s_glow_vbo = 0; }
    if (s_glow_ebo)  { glDeleteBuffers(1, &s_glow_ebo); s_glow_ebo = 0; }

    if (s_water_prog) { glDeleteProgram(s_water_prog); s_water_prog = 0; }
    if (s_water_vao)  { glDeleteVertexArrays(1, &s_water_vao); s_water_vao = 0; }
    if (s_water_vbo)  { glDeleteBuffers(1, &s_water_vbo); s_water_vbo = 0; }
    if (s_water_ebo)  { glDeleteBuffers(1, &s_water_ebo); s_water_ebo = 0; }

    postfx_shutdown();
}

// ============================================================================
// Mesh upload / free
// ============================================================================

GPUMesh upload_mesh(const float* positions, const float* normals, const float* uvs,
                    int num_verts, const uint32_t* indices, int num_indices) {
    GPUMesh mesh{};
    if (!positions || num_verts <= 0) return mesh;

    // Interleaved layout: [pos3][norm3][uv2] = 8 floats per vertex
    const int stride_floats = 3 + 3 + 2;  // 32 bytes
    std::vector<float> buf(num_verts * stride_floats);

    for (int i = 0; i < num_verts; i++) {
        float* dst = &buf[i * stride_floats];

        // Position (always present)
        dst[0] = positions[i * 3 + 0];
        dst[1] = positions[i * 3 + 1];
        dst[2] = positions[i * 3 + 2];

        // Normal (default to Y-up if missing)
        if (normals) {
            dst[3] = normals[i * 3 + 0];
            dst[4] = normals[i * 3 + 1];
            dst[5] = normals[i * 3 + 2];
        } else {
            dst[3] = 0.0f; dst[4] = 1.0f; dst[5] = 0.0f;
        }

        // UV (default to 0,0 if missing)
        if (uvs) {
            dst[6] = uvs[i * 2 + 0];
            dst[7] = uvs[i * 2 + 1];
        } else {
            dst[6] = 0.0f; dst[7] = 0.0f;
        }
    }

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    // VBO — interleaved vertex data
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 buf.size() * sizeof(float), buf.data(), GL_STATIC_DRAW);

    const GLsizei stride = stride_floats * sizeof(float);

    // location 0 = aPos (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)(0));

    // location 1 = aNorm (vec3)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)(3 * sizeof(float)));

    // location 2 = aUV (vec2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (void*)(6 * sizeof(float)));

    // EBO — index buffer (optional)
    if (indices && num_indices > 0) {
        glGenBuffers(1, &mesh.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     num_indices * sizeof(uint32_t), indices, GL_STATIC_DRAW);
        mesh.index_count = num_indices;
    } else {
        mesh.index_count = num_verts;  // non-indexed: draw all verts
    }

    glBindVertexArray(0);
    return mesh;
}

void update_mesh_vertices(const GPUMesh& mesh, const float* positions,
                          const float* normals, const float* uvs, int num_verts) {
    if (!mesh.vbo || !positions || num_verts <= 0) return;
    constexpr int stride_floats = 8;
    std::vector<float> data(static_cast<size_t>(num_verts) * stride_floats);
    for (int i = 0; i < num_verts; ++i) {
        float* dst = &data[static_cast<size_t>(i) * stride_floats];
        std::memcpy(dst, positions + i * 3, 3 * sizeof(float));
        if (normals) std::memcpy(dst + 3, normals + i * 3, 3 * sizeof(float));
        else { dst[3] = 0; dst[4] = 1; dst[5] = 0; }
        if (uvs) std::memcpy(dst + 6, uvs + i * 2, 2 * sizeof(float));
        else { dst[6] = dst[7] = 0; }
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, data.size() * sizeof(float), data.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void free_mesh(GPUMesh& mesh) {
    if (mesh.ebo) { glDeleteBuffers(1, &mesh.ebo); }
    if (mesh.vbo) { glDeleteBuffers(1, &mesh.vbo); }
    if (mesh.vao) { glDeleteVertexArrays(1, &mesh.vao); }
    mesh = GPUMesh{};
}

// ============================================================================
// FBO management
// ============================================================================

unsigned int create_fbo(int width, int height, unsigned int* out_tex) {
    GLuint fbo, tex, depth_tex;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // Color attachment — RGBA8 texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    // Depth attachment — texture (sampleable by the PostFX DOF pass)
    glGenTextures(1, &depth_tex);
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[av_renderer] FBO incomplete: 0x%x\n", status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteTextures(1, &depth_tex);
        if (out_tex) *out_tex = 0;
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Track the depth texture so we can resize/delete later
    s_fbo_records.push_back({fbo, depth_tex});

    if (out_tex) *out_tex = tex;
    return fbo;
}

void resize_fbo(unsigned int fbo, int w, int h, unsigned int* tex) {
    if (!fbo || !tex || w <= 0 || h <= 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // Resize color texture
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Resize depth texture
    GLuint depth_tex = find_depth_for_fbo(fbo);
    if (depth_tex) {
        glBindTexture(GL_TEXTURE_2D, depth_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

unsigned int fbo_depth_texture(unsigned int fbo) {
    return find_depth_for_fbo(fbo);
}

void delete_fbo(unsigned int fbo, unsigned int tex) {
    if (tex) glDeleteTextures(1, &tex);

    // Find and delete associated depth texture
    for (auto it = s_fbo_records.begin(); it != s_fbo_records.end(); ++it) {
        if (it->fbo == fbo) {
            if (it->depth_tex) glDeleteTextures(1, &it->depth_tex);
            s_fbo_records.erase(it);
            break;
        }
    }

    if (fbo) glDeleteFramebuffers(1, &fbo);
}

// ============================================================================
// Rendering
// ============================================================================

void set_point_lights(const float pos[][3], const float col[][3],
                      const float* radius, int count) {
    s_point_light_count = count < 0 ? 0 : (count > 16 ? 16 : count);
    for (int i = 0; i < s_point_light_count; ++i) {
        s_point_light_pos[i][0] = pos[i][0];
        s_point_light_pos[i][1] = pos[i][1];
        s_point_light_pos[i][2] = pos[i][2];
        s_point_light_col[i][0] = col[i][0];
        s_point_light_col[i][1] = col[i][1];
        s_point_light_col[i][2] = col[i][2];
        s_point_light_radius[i] = radius[i];
    }
}

void clear_point_lights() {
    s_point_light_count = 0;
}

void set_depth_fog(bool enabled, const float color[3], float near_dist, float far_dist) {
    s_fog_enabled = enabled;
    if (color) {
        s_fog_color[0] = color[0];
        s_fog_color[1] = color[1];
        s_fog_color[2] = color[2];
    }
    s_fog_near = near_dist;
    s_fog_far  = far_dist;
}

void begin_3d(unsigned int fbo, int w, int h, const Camera& cam) {
    // Save current state for restore in end_3d
    glGetIntegerv(GL_VIEWPORT, s_saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&s_saved_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);

    // Clear with dark background
    glClearColor(g_clear_color[0], g_clear_color[1], g_clear_color[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Standard 3D state
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Swordigo assets contain both winding conventions (POD and embedded
    // GroundMesh). Keep both visible in the editor; this is an inspection and
    // authoring viewport, not a back-face-culled game pass.
    glDisable(GL_CULL_FACE);

    // Compute view and projection matrices
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    camera_get_view_matrix(cam, s_view);
    camera_get_projection(cam, aspect, s_proj);
    mat4_multiply(s_vp, s_proj, s_view);

    // Camera eye position for point-light calculations (inverse view translate).
    s_cam_eye[0] = cam.target[0] + cam.distance * cosf(cam.pitch * 3.14159265358979323846f / 180.0f) * sinf(cam.yaw * 3.14159265358979323846f / 180.0f);
    s_cam_eye[1] = cam.target[1] + cam.distance * sinf(cam.pitch * 3.14159265358979323846f / 180.0f);
    s_cam_eye[2] = cam.target[2] + cam.distance * cosf(cam.pitch * 3.14159265358979323846f / 180.0f) * cosf(cam.yaw * 3.14159265358979323846f / 180.0f);
}

void render_mesh(const GPUMesh& mesh, const float* model_matrix,
                 const float color[4], bool wireframe) {
    if (!mesh.vao || !s_model_prog) return;

    // Default model = identity
    float model[16];
    if (model_matrix) {
        memcpy(model, model_matrix, 16 * sizeof(float));
    } else {
        mat4_identity(model);
    }

    // MVP = proj * view * model
    float mvp[16];
    mat4_multiply(mvp, s_vp, model);

    // Normal matrix (inverse-transpose of upper 3x3 of model)
    float nmat[9];
    mat4_normal_matrix(nmat, model);

    // Default material color
    float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (color) { col[0] = color[0]; col[1] = color[1]; col[2] = color[2]; col[3] = color[3]; }

    glUseProgram(s_model_prog);

    glUniformMatrix4fv(s_loc_mvp,       1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_loc_model,     1, GL_FALSE, model);
    glUniformMatrix3fv(s_loc_normalmat, 1, GL_FALSE, nmat);

    glUniform3fv(s_loc_light_dir, 1, g_light_dir);
    glUniform3fv(s_loc_light_col, 1, g_light_color);
    glUniform3fv(s_loc_ambient,   1, g_ambient_color);
    glUniform4fv(s_loc_mat_color, 1, col);
    glUniform1f(s_loc_alpha,      col[3]);

    // Point lights + camera position for local warm illumination.
    glUniform1i(s_loc_light_cnt, s_point_light_count);
    if (s_point_light_count > 0) {
        glUniform3fv(s_loc_light_pos,  s_point_light_count, &s_point_light_pos[0][0]);
        glUniform3fv(s_loc_light_cols, s_point_light_count, &s_point_light_col[0][0]);
        glUniform1fv(s_loc_light_rad,  s_point_light_count, s_point_light_radius);
    }
    glUniform3fv(s_loc_cam_pos, 1, s_cam_eye);

    // Depth fog.
    glUniform1i(s_loc_fog_en,  s_fog_enabled ? 1 : 0);
    glUniform3fv(s_loc_fog_col, 1, s_fog_color);
    glUniform1f(s_loc_fog_near, s_fog_near);
    glUniform1f(s_loc_fog_far,  s_fog_far);

    // Texture binding
    bool has_tex = (mesh.texture_id != 0);
    glUniform1i(s_loc_has_tex, has_tex ? 1 : 0);
    if (has_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mesh.texture_id);
        glUniform1i(s_loc_texture, 0);
    }

    // Draw
    glBindVertexArray(mesh.vao);

    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDisable(GL_CULL_FACE);
    }

    if (mesh.ebo) {
        glDrawElements(GL_TRIANGLES, mesh.index_count,
                       GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.index_count);
    }

    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_CULL_FACE);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void render_lines(const float* positions, int vertex_count, const float color[4],
                  const float* model_matrix, float width) {
    if (!positions || vertex_count < 2 || !s_model_prog) return;
    float model[16];
    if (model_matrix) std::memcpy(model, model_matrix, sizeof(model));
    else mat4_identity(model);
    float mvp[16], normal[9];
    mat4_multiply(mvp, s_vp, model);
    mat4_normal_matrix(normal, model);

    std::vector<float> vertices(static_cast<size_t>(vertex_count) * 8, 0.0f);
    for (int i = 0; i < vertex_count; ++i) {
        std::memcpy(&vertices[static_cast<size_t>(i) * 8], positions + i * 3, 3 * sizeof(float));
        vertices[static_cast<size_t>(i) * 8 + 4] = 1.0f;
    }
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(6*sizeof(float)));
    glUseProgram(s_model_prog);
    glUniformMatrix4fv(s_loc_mvp, 1, GL_FALSE, mvp); glUniformMatrix4fv(s_loc_model, 1, GL_FALSE, model);
    glUniformMatrix3fv(s_loc_normalmat, 1, GL_FALSE, normal);
    glUniform1i(s_loc_has_tex, 0); glUniform4fv(s_loc_mat_color, 1, color); glUniform1f(s_loc_alpha, color[3]);
    glUniform3f(s_loc_light_dir, 0, 0, 0); glUniform3f(s_loc_light_col, 0, 0, 0); glUniform3f(s_loc_ambient, 1, 1, 1);
    glLineWidth(width); glDrawArrays(GL_LINES, 0, vertex_count); glLineWidth(1.0f);
    glBindVertexArray(0); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao); glUseProgram(0);
}

void render_grid(float size, float y_level) {
    if (!s_grid_vao || !s_grid_prog) return;

    // Model matrix: scale to `size` and translate to y_level
    float scale_m[16], trans_m[16], model[16];
    mat4_identity(scale_m);
    scale_m[0] = size;       // scale X
    scale_m[5] = 1.0f;       // Y unchanged
    scale_m[10] = size;      // scale Z
    mat4_translate(trans_m, 0.0f, y_level, 0.0f);
    mat4_multiply(model, trans_m, scale_m);

    // MVP
    float mvp[16];
    mat4_multiply(mvp, s_vp, model);

    glUseProgram(s_grid_prog);
    glUniformMatrix4fv(s_loc_grid_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_loc_grid_model, 1, GL_FALSE, model);
    glUniform1f(s_loc_grid_size, size);
    glUniform4f(s_loc_grid_color, 0.4f, 0.4f, 0.5f, 0.5f);

    // Grid is translucent — disable depth write, keep depth test
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(s_grid_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

void render_background_quad(unsigned int texture_id, const float* model_matrix) {
    if (!s_bg_prog || !s_bg_vao || !texture_id) return;

    float model[16];
    if (model_matrix) {
        memcpy(model, model_matrix, 16 * sizeof(float));
    } else {
        mat4_identity(model);
    }

    float mvp[16];
    mat4_multiply(mvp, s_vp, model);

    glUseProgram(s_bg_prog);
    glUniformMatrix4fv(s_bg_loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(s_bg_loc_tex, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Drawn behind everything: no depth writes, always passes the depth test
    // at the far plane where the level geometry cannot overlap it.
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_bg_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

void render_glow_sprite(const float pos[3], const float color[3], float size) {
    if (!s_glow_prog || !s_glow_vao || size <= 0.0f) return;

    // Camera-facing billboard: build a model matrix from the view basis.
    // View matrix rows give the camera right/up vectors.
    float M[16];
    const float* V = s_view;
    float right[3] = { V[0], V[4], V[8] };   // first column = camera right
    float up[3]    = { V[1], V[5], V[9] };   // second column = camera up

    const float h = size * 0.5f;
    M[0] = right[0]*h; M[4] = up[0]*h; M[8]  = 0.0f; M[12] = pos[0];
    M[1] = right[1]*h; M[5] = up[1]*h; M[9]  = 0.0f; M[13] = pos[1];
    M[2] = right[2]*h; M[6] = up[2]*h; M[10] = 0.0f; M[14] = pos[2];
    M[3] = 0.0f; M[7] = 0.0f; M[11] = 0.0f; M[15] = 1.0f;

    float mvp[16];
    mat4_multiply(mvp, s_vp, M);

    glUseProgram(s_glow_prog);
    glUniformMatrix4fv(s_glow_loc_mvp, 1, GL_FALSE, mvp);
    glUniform3fv(s_glow_loc_color, 1, color);

    // Additive glow that reads the bloom bright-pass.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_glow_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
}

void render_water_sheet(const WaterSheetData& ws, const float* model_matrix,
                        float time_sec) {
    if (!s_water_prog || !s_water_vao) return;
    const float x = ws.rect[0], y = ws.rect[1], w = ws.rect[2], h = ws.rect[3];
    if (w <= 0.0f || h <= 0.0f) return;

    // Grid: ~20 world units per column like the reference editor, clamped so
    // huge pools stay cheap.
    int cols = std::max(1, (int)std::floor(w / 20.0f));
    if (cols > 48) cols = 48;
    const float f = w / (float)cols;
    const int m = cols + 1;
    const int num_verts = m * 4;

    // Sheet thickness (front/back faces) — matches the reference PS() water
    // effect which spans local z from +40 (front) to -20 (back).
    const float front_z = 40.0f, back_z = -20.0f;

    std::vector<float> verts(static_cast<size_t>(num_verts) * 5, 0.0f);
    std::vector<uint16_t> idx(static_cast<size_t>(cols) * 12, 0);

    const float tile = ws.tile_size > 0.0f ? ws.tile_size : 64.0f;
    const float ox = ws.tex_offset[0], oy = ws.tex_offset[1];
    const float bottom = y + h;

    for (int O = 0; O < m; ++O) {
        const float N = x + O * f;  // column x position
        const float phase = N / 160.0f + time_sec * 0.2f;
        const float phase2 = N / 320.0f + time_sec * 0.6f;
        const float wave = std::sin(phase * 6.2831853f) * 6.0f +
                           std::sin(phase2 * 6.2831853f) * 6.0f;
        const float bottom_y = bottom + wave;   // undulating surface edge
        const float scroll = std::sin((O / 20.0f + time_sec * 0.5f) * 6.2831853f) * 0.2f;

        const size_t base = static_cast<size_t>(O) * 4 * 5;
        // v0 front-top, v1 front-bottom, v2 back-top, v3 back-bottom
        const float vx[4] = { N, N, N, N };
        const float vy[4] = { y, bottom_y, y + wave * 0.4f, bottom_y };
        const float vz[4] = { front_z, front_z, back_z, back_z };
        for (int D = 0; D < 4; ++D) {
            float* out = &verts[base + static_cast<size_t>(D) * 5];
            out[0] = vx[D]; out[1] = vy[D]; out[2] = vz[D];
            out[3] = (vx[D] - ox) / tile + 0.5f + scroll;
            out[4] = (vy[D] - oy) / tile + 0.5f;
        }
        if (O < cols) {
            const uint16_t n = (uint16_t)(O * 4);
            // front face: (1,0,4) (4,5,1)
            idx[O*12 + 0] = n+1; idx[O*12 + 1] = n;   idx[O*12 + 2] = n+4;
            idx[O*12 + 3] = n+4; idx[O*12 + 4] = n+5; idx[O*12 + 5] = n+1;
            // back face: (3,2,6) (6,7,3)
            idx[O*12 + 6] = n+3; idx[O*12 + 7] = n+2; idx[O*12 + 8] = n+6;
            idx[O*12 + 9] = n+6; idx[O*12 + 10] = n+7; idx[O*12 + 11] = n+3;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_water_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_water_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint16_t)),
                 idx.data(), GL_DYNAMIC_DRAW);

    float mvp[16], ident[16];
    if (model_matrix) mat4_multiply(mvp, s_vp, model_matrix);
    else { mat4_identity(ident); mat4_multiply(mvp, s_vp, ident); }

    glUseProgram(s_water_prog);
    glUniformMatrix4fv(s_water_loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(s_water_loc_tex, 0);
    glUniform1f(s_water_loc_scroll, time_sec * 0.05f);

    const bool has_tex = (ws.texture_id != 0);
    glUniform1i(s_water_loc_has_tex, has_tex ? 1 : 0);
    if (has_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ws.texture_id);
    }

    // Transparent sheet: no depth write, depth test still on (opaque geometry
    // drawn before us). Slight polygon offset keeps it off the ground plane.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glBindVertexArray(s_water_vao);
    // Front face with FrontColor
    glUniform4fv(s_water_loc_tint, 1, ws.front_color);
    glDrawElements(GL_TRIANGLES, cols * 6, GL_UNSIGNED_SHORT, nullptr);
    // Back face with SurfaceColor
    glUniform4fv(s_water_loc_tint, 1, ws.surface_color);
    glDrawElements(GL_TRIANGLES, cols * 6, GL_UNSIGNED_SHORT,
                   (void*)(size_t)(cols * 6 * sizeof(uint16_t)));

    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void render_point_light_glows() {
    for (int i = 0; i < s_point_light_count; ++i) {
        float bright[3] = {
            s_point_light_col[i][0] * 1.8f + 0.12f,
            s_point_light_col[i][1] * 1.8f + 0.12f,
            s_point_light_col[i][2] * 1.8f + 0.12f,
        };
        const float size = std::max(14.0f, s_point_light_radius[i] * 0.22f);
        render_glow_sprite(s_point_light_pos[i], bright, size);
    }
}

void render_grid_xy(float size, float z_level) {
    if (!s_grid_vao || !s_grid_prog) return;
    float scale_m[16], rotate_m[16], trans_m[16], temp[16], model[16];
    mat4_identity(scale_m);
    scale_m[0] = size;
    scale_m[10] = size;
    mat4_rotate_x(rotate_m, 90.0f);
    mat4_translate(trans_m, 0.0f, 0.0f, z_level);
    mat4_multiply(temp, rotate_m, scale_m);
    mat4_multiply(model, trans_m, temp);

    float mvp[16];
    mat4_multiply(mvp, s_vp, model);
    glUseProgram(s_grid_prog);
    glUniformMatrix4fv(s_loc_grid_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_loc_grid_model, 1, GL_FALSE, model);
    glUniform1f(s_loc_grid_size, size);
    glUniform4f(s_loc_grid_color, 0.35f, 0.42f, 0.55f, 0.5f);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_grid_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

void end_3d() {
    // Restore previous FBO and viewport
    glBindFramebuffer(GL_FRAMEBUFFER, s_saved_fbo);
    glViewport(s_saved_viewport[0], s_saved_viewport[1],
               s_saved_viewport[2], s_saved_viewport[3]);

    // Reset state to sane defaults for ImGui
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(0);
}

// ============================================================================
// Post-processing pipeline
// ============================================================================
//
//   src ──► [bright-pass] ──► bloomA(half) ──► [blur H] ──► bloomB
//      │                                             └──► [blur V] ──► bloomA
//      └────────────────────────────────────────────────────────┐
//                                                                ▼
//   out  ◄── [composite: DOF + bloom + HD tone map + grade +
//              sharpen + vignette + grain]
//
// Internal FBOs are lazily created/resized; postfx_apply owns no caller state.

// --- Fullscreen-quad shader sources ----------------------------------------

static const char* FX_VS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

static const char* FX_BRIGHT_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;      // 1 / source size
uniform float uThreshold;
void main() {
    vec2 o = uTexel * 0.5;
    vec3 a = texture(uTex, vUV + vec2(-o.x, -o.y)).rgb;
    vec3 b = texture(uTex, vUV + vec2( o.x, -o.y)).rgb;
    vec3 c = texture(uTex, vUV + vec2(-o.x,  o.y)).rgb;
    vec3 d = texture(uTex, vUV + vec2( o.x,  o.y)).rgb;
    vec3 col = (a + b + c + d) * 0.25;
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float bright = smoothstep(uThreshold, uThreshold + 0.6, luma);
    FragColor = vec4(col * bright, 1.0);
}
)GLSL";

static const char* FX_BLUR_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uDir;         // texel-sized blur direction
void main() {
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 acc = texture(uTex, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = uDir * float(i);
        acc += texture(uTex, vUV + off).rgb * w[i];
        acc += texture(uTex, vUV - off).rgb * w[i];
    }
    FragColor = vec4(acc, 1.0);
}
)GLSL";

static const char* FX_COMP_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uColor;
uniform sampler2D uBloom;
uniform sampler2D uDepth;
uniform vec2  uTexel;         // 1 / full size
uniform float uExposure;
uniform int   uHdOn;
uniform int   uBloomOn;
uniform float uBloomStrength;
uniform int   uDofOn;
uniform float uDofFocus;
uniform float uDofScale;
uniform float uNear;
uniform float uFar;
uniform int   uGradeOn;
uniform float uSaturation;
uniform float uContrast;
uniform float uBrightness;
uniform float uWarmth;
uniform int   uSharpenOn;
uniform float uSharpenAmount;
uniform int   uVignetteOn;
uniform float uVignetteStrength;
uniform int   uGrainOn;
uniform float uGrainAmount;
uniform float uTime;

float linearDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 col = texture(uColor, vUV).rgb;

    // ── Depth of field: CoC-driven disc blur ──
    if (uDofOn == 1) {
        float depth = linearDepth(texture(uDepth, vUV).r);
        float coc = clamp(abs(depth - uDofFocus) / max(uDofFocus, 0.001), 0.0, 1.0) * uDofScale;
        if (coc > 0.002) {
            // Ring of 12 taps + the center tap, radius clamped so even a large
            // CoC can't blow out into a 400px smear on small previews.
            float radius = min(coc * 60.0, 24.0);
            vec3 acc = col;          // center tap
            float wsum = 1.0;
            for (int i = 0; i < 12; ++i) {
                float ang = 6.2831853 * float(i) / 12.0;
                vec2 off = vec2(cos(ang), sin(ang)) * radius;
                vec2 tc = clamp(vUV + off * uTexel, vec2(0.001), vec2(0.999));
                float w = 1.0 - float(i) / 13.0;
                acc += texture(uColor, tc).rgb * w;
                wsum += w;
            }
            col = acc / wsum;
        }
    }

    // ── Bloom (additive) ──
    if (uBloomOn == 1)
        col += texture(uBloom, vUV).rgb * uBloomStrength;

    // ── HD render: exposure + ACES tone map + gamma ──
    if (uHdOn == 1) {
        col *= uExposure;
        col = aces(col);
        col = pow(col, vec3(1.0 / 2.2));
    }

    // ── Cinematic color grade ──
    if (uGradeOn == 1) {
        float luma = dot(col, vec3(0.299, 0.587, 0.114));
        col = mix(vec3(luma), col, uSaturation);
        col = (col - 0.5) * uContrast + 0.5 + uBrightness;
        col = max(col, vec3(0.0));
        col.r += uWarmth * 0.2;
        col.b -= uWarmth * 0.2;
    }

    // ── Sharpen (unsharp mask) ──
    if (uSharpenOn == 1) {
        vec3 lap = texture(uColor, vUV + vec2( uTexel.x, 0.0)).rgb
                 + texture(uColor, vUV - vec2( uTexel.x, 0.0)).rgb
                 + texture(uColor, vUV + vec2(0.0,  uTexel.y)).rgb
                 + texture(uColor, vUV - vec2(0.0, -uTexel.y)).rgb
                 - 4.0 * col;
        col += lap * uSharpenAmount * 0.35;
    }

    // ── Vignette ──
    if (uVignetteOn == 1) {
        vec2 d = vUV - 0.5;
        float dist2 = dot(d, d);
        col *= 1.0 - uVignetteStrength * smoothstep(0.25, 0.85, dist2 * 2.4);
    }

    // ── Film grain ──
    if (uGrainOn == 1) {
        float g = (hash12(vUV * 1280.0 + vec2(fract(uTime) * 97.0)) - 0.5) * uGrainAmount;
        col += g;
    }

    FragColor = vec4(col, 1.0);
}
)GLSL";

// --- PostFX internal state ---------------------------------------------------

static GLuint s_fx_bright  = 0;
static GLuint s_fx_blur    = 0;
static GLuint s_fx_comp    = 0;
static GLuint s_fx_vao     = 0;
static GLuint s_fx_vbo     = 0;
static GLuint s_fx_ebo     = 0;
static GLuint s_fx_bloom_a_fbo = 0, s_fx_bloom_a_tex = 0;
static GLuint s_fx_bloom_b_fbo = 0, s_fx_bloom_b_tex = 0;
static GLuint s_fx_out_fbo = 0, s_fx_out_tex = 0;
static int    s_fx_w = 0, s_fx_h = 0;

static GLuint fx_make_prog(const char* fs) {
    return build_program(FX_VS, fs);
}

// Create an RGBA8 color-only FBO (no depth needed for ping-pong passes).
static bool fx_make_fbo(int w, int h, GLuint* fbo, GLuint* tex) {
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    const bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!ok) {
        fprintf(stderr, "[av_renderer] PostFX FBO incomplete\n");
        glDeleteFramebuffers(1, fbo); glDeleteTextures(1, tex);
        *fbo = *tex = 0;
        return false;
    }
    return true;
}

static bool fx_ensure(int w, int h) {
    if (w < 1 || h < 1) return false;

    if (!s_fx_comp) {
        s_fx_bright = fx_make_prog(FX_BRIGHT_FS);
        s_fx_blur   = fx_make_prog(FX_BLUR_FS);
        s_fx_comp   = fx_make_prog(FX_COMP_FS);
        if (!s_fx_comp || !s_fx_bright || !s_fx_blur) {
            fprintf(stderr, "[av_renderer] PostFX shader build failed\n");
            return false;
        }

        // Fullscreen quad (two triangles)
        static const float verts[] = {
            -1, -1, 0, 0,    1, -1, 1, 0,
             1,  1, 1, 1,   -1,  1, 0, 1,
        };
        static const uint16_t idx[] = { 0, 1, 2,  0, 2, 3 };
        glGenVertexArrays(1, &s_fx_vao);
        glBindVertexArray(s_fx_vao);
        glGenBuffers(1, &s_fx_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_fx_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glGenBuffers(1, &s_fx_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_fx_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
    }

    // (Re)size the internal FBOs if needed
    if (s_fx_out_fbo && (s_fx_w != w || s_fx_h != h)) {
        glDeleteFramebuffers(1, &s_fx_out_fbo); glDeleteTextures(1, &s_fx_out_tex);
        s_fx_out_fbo = s_fx_out_tex = 0;
        glDeleteFramebuffers(1, &s_fx_bloom_a_fbo); glDeleteTextures(1, &s_fx_bloom_a_tex);
        s_fx_bloom_a_fbo = s_fx_bloom_a_tex = 0;
        glDeleteFramebuffers(1, &s_fx_bloom_b_fbo); glDeleteTextures(1, &s_fx_bloom_b_tex);
        s_fx_bloom_b_fbo = s_fx_bloom_b_tex = 0;
    }
    if (!s_fx_out_fbo) {
        const int bw = std::max(1, w / 2), bh = std::max(1, h / 2);
        if (!fx_make_fbo(w, h, &s_fx_out_fbo, &s_fx_out_tex)) return false;
        if (!fx_make_fbo(bw, bh, &s_fx_bloom_a_fbo, &s_fx_bloom_a_tex)) return false;
        if (!fx_make_fbo(bw, bh, &s_fx_bloom_b_fbo, &s_fx_bloom_b_tex)) return false;
        s_fx_w = w; s_fx_h = h;
    }
    return true;
}

static void fx_quad() {
    glBindVertexArray(s_fx_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
}

void postfx_shutdown() {
    if (s_fx_comp)   { glDeleteProgram(s_fx_comp);   s_fx_comp = 0; }
    if (s_fx_bright) { glDeleteProgram(s_fx_bright); s_fx_bright = 0; }
    if (s_fx_blur)   { glDeleteProgram(s_fx_blur);   s_fx_blur = 0; }
    if (s_fx_vao)    { glDeleteVertexArrays(1, &s_fx_vao); s_fx_vao = 0; }
    if (s_fx_vbo)    { glDeleteBuffers(1, &s_fx_vbo); s_fx_vbo = 0; }
    if (s_fx_ebo)    { glDeleteBuffers(1, &s_fx_ebo); s_fx_ebo = 0; }
    if (s_fx_out_fbo)     { glDeleteFramebuffers(1, &s_fx_out_fbo);     s_fx_out_fbo = 0; }
    if (s_fx_out_tex)     { glDeleteTextures(1, &s_fx_out_tex);         s_fx_out_tex = 0; }
    if (s_fx_bloom_a_fbo) { glDeleteFramebuffers(1, &s_fx_bloom_a_fbo); s_fx_bloom_a_fbo = 0; }
    if (s_fx_bloom_a_tex) { glDeleteTextures(1, &s_fx_bloom_a_tex);     s_fx_bloom_a_tex = 0; }
    if (s_fx_bloom_b_fbo) { glDeleteFramebuffers(1, &s_fx_bloom_b_fbo); s_fx_bloom_b_fbo = 0; }
    if (s_fx_bloom_b_tex) { glDeleteTextures(1, &s_fx_bloom_b_tex);     s_fx_bloom_b_tex = 0; }
    s_fx_w = s_fx_h = 0;
}

unsigned int postfx_apply(unsigned int src_tex, unsigned int depth_tex,
                          int w, int h, const PostFXParams& p,
                          float near_plane, float far_plane, float time_sec) {
    if (!p.enabled) return src_tex;
    const bool any = p.hd || p.bloom || p.dof || p.color_grade ||
                     p.sharpen || p.vignette || p.grain;
    if (!any) return src_tex;
    if (!fx_ensure(w, h)) return src_tex;

    // Save caller GL state so ImGui / overlays after the call are unaffected.
    GLint saved_viewport[4], saved_fbo = 0;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    const int bw = std::max(1, w / 2), bh = std::max(1, h / 2);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // ── Bloom chain (bright pass + separable blur, half-res ping-pong) ──
    if (p.bloom) {
        // Bright pass: full-res -> half-res bloomA
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_a_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_bright);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform1i(glGetUniformLocation(s_fx_bright, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_bright, "uTexel"), 1.0f / w, 1.0f / h);
        glUniform1f(glGetUniformLocation(s_fx_bright, "uThreshold"), p.bloom_threshold);
        fx_quad();

        // Blur H: bloomA -> bloomB
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_b_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_blur);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_a_tex);
        glUniform1i(glGetUniformLocation(s_fx_blur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 1.0f / bw, 0.0f);
        fx_quad();

        // Blur V: bloomB -> bloomA
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_a_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_blur);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_b_tex);
        glUniform1i(glGetUniformLocation(s_fx_blur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 0.0f, 1.0f / bh);
        fx_quad();
    }

    // ── Composite ──
    glBindFramebuffer(GL_FRAMEBUFFER, s_fx_out_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(s_fx_comp);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src_tex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, p.bloom ? s_fx_bloom_a_tex : 0);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, depth_tex);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uColor"), 0);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uBloom"), 1);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uDepth"), 2);
    glUniform2f(glGetUniformLocation(s_fx_comp, "uTexel"), 1.0f / w, 1.0f / h);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uExposure"), p.exposure);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uHdOn"), p.hd ? 1 : 0);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uBloomOn"), p.bloom ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uBloomStrength"), p.bloom_strength);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uDofOn"), (p.dof && depth_tex) ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uDofFocus"), p.dof_focus);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uDofScale"), p.dof_scale);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uNear"), near_plane);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uFar"), far_plane);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uGradeOn"), p.color_grade ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uSaturation"), p.saturation);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uContrast"), p.contrast);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uBrightness"), p.brightness);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uWarmth"), p.warmth);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uSharpenOn"), p.sharpen ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uSharpenAmount"), p.sharpen_amount);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uVignetteOn"), p.vignette ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uVignetteStrength"), p.vignette_strength);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uGrainOn"), p.grain ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uGrainAmount"), p.grain_amount);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uTime"), time_sec);
    fx_quad();

    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    return s_fx_out_tex;
}

} // namespace av
