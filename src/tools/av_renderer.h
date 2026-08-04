/* av_renderer.h — Modern OpenGL 3.3 renderer for the Swordigo Asset Viewer
 *
 * Provides:
 *   - Shader compilation (vertex + fragment from inline GLSL 330 core)
 *   - Mesh GPU upload (VBO/VAO/EBO from PODMesh-style arrays)
 *   - Orbit camera with yaw/pitch/distance/target
 *   - XZ grid plane with edge-fade
 *   - Render-to-FBO for embedding in ImGui viewports
 *
 * Self-contained: no GLM, no external math libs.
 * Requires: OpenGL 3.3 core, Linux GL headers.
 *
 * Build with: -std=c++17 -lGL
 */
#pragma once

#include <cstdint>

namespace av {

// ============================================================================
// Camera — orbit-style (yaw / pitch / distance around a target point)
// ============================================================================

struct Camera {
    float yaw       = 0.0f;         // degrees, around Y axis
    float pitch     = 25.0f;        // degrees, above horizon
    float distance  = 5.0f;         // distance from target
    float target[3] = {0, 0, 0};    // look-at center
    float fov       = 45.0f;        // vertical FOV in degrees
    float near_plane = 0.01f;
    float far_plane  = 1000.0f;
};

// ============================================================================
// GPUMesh — handle to uploaded geometry on the GPU
// ============================================================================

struct GPUMesh {
    unsigned int vao = 0;
    unsigned int vbo = 0;
    unsigned int ebo = 0;
    int index_count  = 0;
    unsigned int texture_id = 0;    // 0 = no texture bound
};

// ============================================================================
// Lifecycle
// ============================================================================

/// Compile all internal shaders (model + grid). Call AFTER GL context is valid.
/// Returns false on shader compile/link failure (error printed to stderr).
bool renderer_init();

/// Delete internal shaders and grid VAO.
void renderer_shutdown();

// ============================================================================
// Mesh upload / free
// ============================================================================

/// Upload interleaved vertex data to the GPU.
/// @param positions   Flat float array [x,y,z] * num_verts  (required)
/// @param normals     Flat float array [nx,ny,nz] * num_verts (may be null)
/// @param uvs         Flat float array [u,v] * num_verts      (may be null)
/// @param num_verts   Number of vertices
/// @param indices     Index array (uint32), may be null for non-indexed
/// @param num_indices Number of indices (0 if non-indexed)
/// @return GPUMesh ready for rendering (vao == 0 on failure)
GPUMesh upload_mesh(const float* positions, const float* normals, const float* uvs,
                    int num_verts, const uint32_t* indices, int num_indices);

/// Replace an uploaded mesh's dynamic position/normal data in-place.
void update_mesh_vertices(const GPUMesh& mesh, const float* positions,
                          const float* normals, const float* uvs, int num_verts);

/// Delete GPU resources for a mesh.  Zeros the handle fields.
void free_mesh(GPUMesh& mesh);

// ============================================================================
// FBO management
// ============================================================================

/// Create an RGBA8 color + depth renderbuffer FBO.
/// @param width, height   Initial dimensions
/// @param out_tex         Receives the GL texture ID for the color attachment
/// @return FBO id (0 on failure)
unsigned int create_fbo(int width, int height, unsigned int* out_tex);

/// Resize an existing FBO's attachments in-place.
void resize_fbo(unsigned int fbo, int w, int h, unsigned int* tex);

/// Delete the FBO and its color texture.
void delete_fbo(unsigned int fbo, unsigned int tex);

// ============================================================================
// Rendering
// ============================================================================

/// Begin a 3D render pass into the given FBO.
/// Sets viewport, clears color+depth, computes view/proj from cam.
void begin_3d(unsigned int fbo, int w, int h, const Camera& cam);

/// Render a GPUMesh with the model shader.
/// @param mesh          Uploaded mesh
/// @param model_matrix  4x4 column-major model transform (identity if null)
/// @param color         RGBA material color [r,g,b,a] (white if null)
/// @param wireframe     If true, render as GL_LINES overlay
void render_mesh(const GPUMesh& mesh, const float* model_matrix,
                 const float color[4], bool wireframe);

/// Render debug line segments, two xyz points per segment.
void render_lines(const float* positions, int vertex_count, const float color[4],
                  const float* model_matrix = nullptr, float width = 1.0f);

/// Render the XZ grid plane centered at origin.
/// @param size     Total grid extent (e.g. 20 = ±10 units)
/// @param y_level  Y height of the grid plane
void render_grid(float size, float y_level);
void render_grid_xy(float size, float z_level);

/// End the 3D render pass (unbind FBO, restore viewport).
void end_3d();

// Global lighting parameters (can be adjusted by GUI)
extern float g_light_dir[3];
extern float g_light_color[3];
extern float g_ambient_color[3];
extern float g_clear_color[3];

// ============================================================================
// Math helpers (column-major 4x4 matrices, right-handed coordinate system)
// ============================================================================

void mat4_identity(float out[16]);
void mat4_multiply(float out[16], const float a[16], const float b[16]);
void mat4_translate(float out[16], float tx, float ty, float tz);
void mat4_rotate_x(float out[16], float angle_deg);
void mat4_rotate_y(float out[16], float angle_deg);
void mat4_rotate_z(float out[16], float angle_deg);
void mat4_perspective(float out[16], float fov_deg, float aspect,
                      float near_plane, float far_plane);
void mat4_look_at(float out[16],
                  float ex, float ey, float ez,
                  float cx, float cy, float cz,
                  float ux, float uy, float uz);

/// Extract upper-left 3x3 from a 4x4 matrix and invert-transpose it
/// (for transforming normals).  Writes 9 floats row-major.
void mat4_normal_matrix(float out[9], const float model[16]);

/// Convenience: compute view matrix from Camera state.
void camera_get_view_matrix(const Camera& cam, float out[16]);

/// Convenience: compute projection matrix from Camera state.
void camera_get_projection(const Camera& cam, float aspect, float out[16]);

} // namespace av
