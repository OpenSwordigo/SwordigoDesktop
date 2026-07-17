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
float g_light_dir[3]   = { 0.577f,  0.577f,  0.577f };  // normalized (1,1,1)
float g_light_color[3] = { 1.0f,    0.95f,   0.9f  };
float g_ambient_color[3]     = { 0.30f,   0.30f,   0.35f };
float g_clear_color[3]       = { 0.08f,   0.08f,   0.11f };

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

out vec4 FragColor;

void main() {
    vec4  base = uHasTexture ? texture(uTexture, vUV) : uMatColor;
    float NdotL = max(dot(normalize(vNormal), uLightDir), 0.0);
    vec3  lit   = base.rgb * (uAmbient + uLightColor * NdotL);
    FragColor   = vec4(lit, base.a * uAlpha);
}
)GLSL";

// --- Grid shaders (XZ plane with fade-to-transparent) ---

static const char* GRID_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;

uniform mat4 uMVP;

out vec3 vWorldPos;

void main() {
    vWorldPos   = aPos;
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
    float fade = 1.0 - smoothstep(uGridSize * 0.5, uGridSize, dist);

    // Grid lines: fract in grid-cell space, thin line at cell edges
    vec2  grid = abs(fract(vWorldPos.xz / uGridSize * 10.0) - 0.5);
    float line = 1.0 - smoothstep(0.0, 0.02, min(grid.x, grid.y));

    FragColor = vec4(uGridColor.rgb, uGridColor.a * line * fade);
}
)GLSL";

// ============================================================================
// Internal state
// ============================================================================

static GLuint s_model_prog = 0;
static GLuint s_grid_prog  = 0;

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

// Grid shader uniform locations
static GLint s_loc_grid_mvp   = -1;
static GLint s_loc_grid_size  = -1;
static GLint s_loc_grid_color = -1;

// Grid geometry (a simple XZ quad — large enough, faded at edges)
static GLuint s_grid_vao = 0;
static GLuint s_grid_vbo = 0;

// Currently active view/proj matrices (set by begin_3d)
static float s_view[16];
static float s_proj[16];
static float s_vp[16];   // view * proj combined

// Saved viewport for end_3d restore
static GLint s_saved_viewport[4] = {0, 0, 0, 0};
static GLuint s_saved_fbo = 0;

// Depth renderbuffer for FBOs
// We maintain a map-free approach: store one RBO per FBO via a small lookup
// (asset viewer typically has 1–2 FBOs).
struct FBORecord { GLuint fbo; GLuint rbo; };
static std::vector<FBORecord> s_fbo_records;

static GLuint find_rbo_for_fbo(GLuint fbo) {
    for (auto& r : s_fbo_records)
        if (r.fbo == fbo) return r.rbo;
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
    s_loc_grid_size  = glGetUniformLocation(s_grid_prog, "uGridSize");
    s_loc_grid_color = glGetUniformLocation(s_grid_prog, "uGridColor");

    // --- Grid geometry ---
    create_grid_geometry();

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

    // Clean up tracked FBO renderbuffers
    for (auto& r : s_fbo_records) {
        if (r.rbo) glDeleteRenderbuffers(1, &r.rbo);
    }
    s_fbo_records.clear();
}

// ============================================================================
// Mesh upload / free
// ============================================================================

GPUMesh upload_mesh(const float* positions, const float* normals, const float* uvs,
                    int num_verts, const uint16_t* indices, int num_indices) {
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
                     num_indices * sizeof(uint16_t), indices, GL_STATIC_DRAW);
        mesh.index_count = num_indices;
    } else {
        mesh.index_count = num_verts;  // non-indexed: draw all verts
    }

    glBindVertexArray(0);
    return mesh;
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
    GLuint fbo, tex, rbo;

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

    // Depth attachment — renderbuffer
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[av_renderer] FBO incomplete: 0x%x\n", status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteRenderbuffers(1, &rbo);
        if (out_tex) *out_tex = 0;
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Track the RBO so we can resize/delete later
    s_fbo_records.push_back({fbo, rbo});

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

    // Resize depth renderbuffer
    GLuint rbo = find_rbo_for_fbo(fbo);
    if (rbo) {
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void delete_fbo(unsigned int fbo, unsigned int tex) {
    if (tex) glDeleteTextures(1, &tex);

    // Find and delete associated RBO
    for (auto it = s_fbo_records.begin(); it != s_fbo_records.end(); ++it) {
        if (it->fbo == fbo) {
            if (it->rbo) glDeleteRenderbuffers(1, &it->rbo);
            s_fbo_records.erase(it);
            break;
        }
    }

    if (fbo) glDeleteFramebuffers(1, &fbo);
}

// ============================================================================
// Rendering
// ============================================================================

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
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Compute view and projection matrices
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    camera_get_view_matrix(cam, s_view);
    camera_get_projection(cam, aspect, s_proj);
    mat4_multiply(s_vp, s_proj, s_view);
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
                       GL_UNSIGNED_SHORT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.index_count);
    }

    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
    }

    glBindVertexArray(0);
    glUseProgram(0);
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
    glUniform1f(s_loc_grid_size, size);
    glUniform4f(s_loc_grid_color, 0.4f, 0.4f, 0.5f, 0.5f);

    // Grid is translucent — disable depth write, keep depth test
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(s_grid_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
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
}

} // namespace av
