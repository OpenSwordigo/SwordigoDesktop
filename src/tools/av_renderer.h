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

/// Return the depth texture attached to an FBO created by create_fbo()
/// (0 if the FBO is unknown). Used by the PostFX depth-of-field pass.
unsigned int fbo_depth_texture(unsigned int fbo);

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

/// Upload scene point lights for the model shader (world-space positions,
/// warm colors, radii). Call once per frame before rendering geometry.
/// @param pos      [count][3] world positions
/// @param col      [count][3] RGB colors
/// @param radius   [count] light radii (falloff distance)
/// @param count    Number of lights (clamped to 16)
void set_point_lights(const float pos[][3], const float col[][3],
                      const float* radius, int count);

/// Reset point lights to none (editor-only previews).
void clear_point_lights();

/// Render a camera-facing additive glow sprite (billboard) at a world position.
/// Used for torch/fire/emissive glows that feed the PostFX bloom bright-pass.
/// @param pos    World position [x,y,z]
/// @param color  RGB color (values > 1.0 push it into the bloom threshold)
/// @param size   Billboard world size (diameter)
void render_glow_sprite(const float pos[3], const float color[3], float size);

/// Convenience: render glow sprites for every point light uploaded via
/// set_point_lights().
void render_point_light_glows();

/// One water/fluid sheet (parsed from WaterMeshComponent + shape rectangle).
/// @p rect       Fluid sheet rectangle [x, y, w, h] in object-local coords.
/// @p front      Front-face RGBA tint (FrontColor).
/// @p surface    Back-face RGBA tint (SurfaceColor).
/// @p tile       Texture tile size in world units (TextureMapping f2).
/// @p offset     Texture UV offset [x, y] (TextureMapping f3).
/// @p texture_id GL texture for the fluid surface (0 = solid tint only).
struct WaterSheetData {
    float rect[4]        = {0, 0, 10, 10};
    float front_color[4] = {0.5f, 0.7f, 1.0f, 0.7f};
    float surface_color[4] = {0.7f, 0.9f, 1.0f, 0.7f};
    float tile_size      = 64.0f;
    float tex_offset[2]  = {0, 0};
    unsigned int texture_id = 0;
};

/// Render an animated fluid sheet (water / lava) following the reference
/// editor's PS() water effect: a vertical grid sheet spanning the rectangle
/// with sine-wave surface undulation + scrolling UV. Double-sided, alpha
/// blended, depth-write off (drawn after opaque geometry).
/// @param model_matrix  Object transform (object-local rect -> world).
/// @param time_sec      Animation clock (drives waves + UV scroll).
void render_water_sheet(const WaterSheetData& ws, const float* model_matrix,
                        float time_sec);

/// Configure atmospheric depth fog for the model shader (vanilla cave depth).
/// @param enabled  Master switch (disabled by default)
/// @param color    Fog tint (dark cave color)
/// @param near     Distance where fog starts
/// @param far      Distance where fog is fully opaque
void set_depth_fog(bool enabled, const float color[3], float near_dist, float far_dist);

/// Render debug line segments, two xyz points per segment.
void render_lines(const float* positions, int vertex_count, const float color[4],
                  const float* model_matrix = nullptr, float width = 1.0f);

/// Render the XZ grid plane centered at origin.
/// @param size     Total grid extent (e.g. 20 = ±10 units)
/// @param y_level  Y height of the grid plane
void render_grid(float size, float y_level);
void render_grid_xy(float size, float z_level);

/// Render a full-screen-style textured background quad (unlit, depth-write off)
/// using the given world-space model matrix. Mirrors the reference editor's
/// BackgroundComponent handling (a camera-following textured plane drawn first).
/// @param texture_id  GL texture to sample (0 disables)
/// @param model_matrix  4x4 column-major transform (unit quad is ±1 in X/Y)
void render_background_quad(unsigned int texture_id, const float* model_matrix);

/// End the 3D render pass (unbind FBO, restore viewport).
void end_3d();

// ============================================================================
// Post-processing (bloom / depth-of-field / HD grade / vignette / grain)
// ============================================================================

struct PostFXParams {
    // Master switch — postfx_apply returns the source texture untouched when off.
    bool enabled = true;

    // HD render: exposure + ACES tone mapping + gamma. This is what makes the
    // preview stop looking like a raw framebuffer.
    bool  hd          = true;
    float exposure    = 1.0f;

    // Bloom: bright-pass extract, downsample, separable 9-tap gaussian blur.
    bool  bloom           = true;
    float bloom_strength  = 0.30f;
    float bloom_threshold = 0.85f;

    // Depth of field: CoC blur driven by the FBO depth texture. `dof_focus` is
    // the in-focus distance from the camera (world units).
    bool  dof        = false;
    float dof_focus  = 10.0f;
    float dof_scale  = 2.5f;

    // Cinematic color grade (applied after tone mapping).
    bool  color_grade = true;
    float saturation  = 1.10f;
    float contrast    = 1.06f;
    float brightness  = 0.02f;
    float warmth      = 0.0f;   // -1..1, warm(+)/cool(-) tint

    // Unsharp-mask sharpen / crispness.
    bool  sharpen        = true;
    float sharpen_amount = 0.35f;

    // Vignette.
    bool  vignette         = true;
    float vignette_strength = 0.30f;

    // Animated film grain.
    bool  grain        = false;
    float grain_amount = 0.05f;
};

/// Run the post-processing chain on a rendered FBO color texture and return the
/// texture to display. Returns @p src_tex unchanged when nothing is enabled.
/// The depth texture (from fbo_depth_texture) is only required for DOF.
/// Internal ping-pong FBOs are lazily created and resized; no caller-owned
/// state is needed. @p time_sec drives the animated grain.
unsigned int postfx_apply(unsigned int src_tex, unsigned int depth_tex,
                          int w, int h, const PostFXParams& p,
                          float near_plane, float far_plane, float time_sec);

/// Free PostFX programs and internal FBOs (called from renderer_shutdown).
void postfx_shutdown();

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

/// Invert a general 4x4 column-major matrix.  Writes the result to `out`.
/// Returns false if the matrix is singular (out is left untouched).
bool mat4_inverse(float out[16], const float m[16]);

/// Convenience: compute view matrix from Camera state.
void camera_get_view_matrix(const Camera& cam, float out[16]);

/// Convenience: compute projection matrix from Camera state.
void camera_get_projection(const Camera& cam, float aspect, float out[16]);

} // namespace av
