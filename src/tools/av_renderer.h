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

    // Orientation of the texture upload. Swordigo containers (.tex.png/.pvr)
    // and (after the 2026-08 normalization) all plain images are uploaded
    // bottom-origin (v = 0 = bottom of the image, the game convention).
    // DCC-style importers (FBX / glTF, whose authored UVs have v = 0 at the
    // top) set this so the model shader flips V at sample time — fixing the
    // "FBX textures mapped upside-down" bug without touching POD assets.
    bool flip_uv_v = false;

    // PBR vertex layout info (set by upload_mesh_ex).
    bool has_tangents = false;   // tangent vec4 attribute present (PBR layout)
    bool has_skinning = false;   // joint/weight vec4 attributes present
    int  stride_floats = 8;      // interleaved floats per vertex
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

/// Stride-aware update for PBR-layout meshes (tangent/joint/weight attributes
/// preserved). Pass nullptr for any stream that should keep its old values.
void update_mesh_vertices_ex(const GPUMesh& mesh, const float* positions,
                             const float* normals, const float* uvs,
                             const float* tangents4, const float* joints4,
                             const float* weights4, int num_verts);

/// Delete GPU resources for a mesh.  Zeros the handle fields.
void free_mesh(GPUMesh& mesh);

/// Upload geometry with optional tangent (vec4) and skinning (vec4 joint /
/// vec4 weight) attributes — the vertex layout the PBR renderer consumes.
/// Interleaved layouts: [pos3 nrm3 uv2] (+ tan4) (+ joint4 weight4).
/// When @p tangents4 is null the PBR program degrades to vertex-normal
/// shading; when @p joints4/weights4 are null the mesh is rigid.
/// @p flip_uv_v flips V at sample time (see GPUMesh::flip_uv_v).
GPUMesh upload_mesh_ex(const float* positions, const float* normals, const float* uvs,
                       const float* tangents4, const float* joints4, const float* weights4,
                       int num_verts, const uint32_t* indices, int num_indices,
                       bool flip_uv_v);

/// Toggle V-flipping on an already-uploaded mesh (no GL calls).
void set_mesh_flip_uv(GPUMesh& mesh, bool flip);

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

/// Upload scene directional lights (up to 4). When count == 0 the renderer
/// falls back to the single g_light_dir/g_light_color editor preview light.
void set_directional_lights(const float dir[][3], const float col[][3], int count);

/// Reset directional lights to none (use editor preview light).
void clear_directional_lights();

/// Render a camera-facing additive glow sprite (billboard) at a world position.
/// Used for torch/fire/emissive glows that feed the PostFX bloom bright-pass.
/// @param pos    World position [x,y,z]
/// @param color  RGB color (values > 1.0 push it into the bloom threshold)
/// @param size   Billboard world size (diameter)
void render_glow_sprite(const float pos[3], const float color[3], float size);

/// Convenience: render glow sprites for every point light uploaded via
/// set_point_lights(). The sprites are compact emitter markers (screen-sized
/// billboards at the source) whose bright cores feed the bloom bright-pass.
void render_point_light_glows();

/// Editor debug overlay: draw an influence-radius ring + small emitter cross
/// at every uploaded point light. Visualized separately from the real
/// illumination so the marker never reads as the light itself.
void render_light_debug();

/// Render a soft ground-aligned contact shadow (dark ellipse) at @p pos.
/// Scaled by the two radii, rotated on the Y axis, drawn multiplicatively so
/// it darkens whatever is beneath it (ground / water).
void render_shadow_blob(const float pos[3], float width_radius, float depth_radius,
                        float rot_y, const float color[3]);

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

/// Set viewport diagnostics for the model shader (applies to every render_mesh
/// call until changed). Both are off by default.
/// @param flat_shade    true  -> skip the whole lighting equation: texture /
///                      material color straight to screen ("is the texture
///                      dark, or is the lighting wrong?" check).
/// @param debug_normals true  -> render world-space normals as color
///                      (RGB = N*0.5+0.5). Inverted / missing normals become
///                      obvious (walls facing away show purple-blue instead of
///                      their surface tint). Ignored while flat_shade is on.
void set_view_flags(bool flat_shade, bool debug_normals);

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
// PBR renderer — vendored algorithms from zauonlok/renderer (MIT, Zhou Le)
//   · GGX distribution / Smith visibility / Schlick+F90 Fresnel
//   · metallic-roughness AND specular-glossiness workflows
//   · tangent-space normal mapping (TBN, aTangent.w handedness)
//   · image-based lighting: split-sum (prefiltered env cubemap + BRDF LUT)
//   · directional shadow mapping (light-VP depth pass + N·L-biased compare)
//   · ACES tone mapping
// Reference sources live in src/render/zauonlok/ (see its README).
// ============================================================================

struct PBRMaterial {
    // Base color (RGBA). alpha < 1 blends; alpha < uAlphaCutoff discards.
    float base_color[4] = {1, 1, 1, 1};
    float metalness  = 0.0f;    // metallic-roughness workflow
    float roughness  = 0.6f;    // metallic-roughness workflow
    float occlusion  = 1.0f;    // ambient-occlusion multiplier
    float emission[3] = {0, 0, 0};

    // Textures (0 = none). All bottom-origin (game convention).
    unsigned int basecolor_tex = 0;
    unsigned int metalness_tex = 0;
    unsigned int roughness_tex = 0;
    unsigned int normal_tex    = 0;
    unsigned int occlusion_tex = 0;
    unsigned int emission_tex  = 0;

    // 0 = metallic-roughness, 1 = specular-glossiness (diffuse + specular
    // factors; the fragment shader converts glossiness → roughness).
    int workflow = 0;
    // Specular-glossiness inputs (workflow == 1).
    float specular[3]  = {0.04f, 0.04f, 0.04f};
    float glossiness   = 0.6f;
    unsigned int specular_tex = 0;
    unsigned int glossiness_tex = 0;
};

/// GPU joint matrices for vertex-shader skinning in the PBR program.
/// @p matrices is a flat [count * 16] column-major array; count is clamped
/// to 256 (uJoints uniform array — sized for renderer-master .ani rigs like
/// the 235-joint assassin). Default (count 0) = rigid mesh.
void pbr_set_joint_matrices(const float* matrices, int count);

/// IBL environment: a mip-mapped cubemap (prefiltered radiance; the
/// roughness LOD is picked by the shader) plus the procedural split-sum
/// BRDF LUT generated at renderer_init. Pass 0 to disable IBL.
/// @p intensity scales the environment contribution (1.0 = neutral).
void pbr_set_environment(unsigned int env_cubemap, float intensity);

/// Adjust the IBL contribution without replacing the environment cubemap
/// (the procedural sky built at renderer_init is used until one is supplied).
void pbr_set_env_intensity(float intensity);

/// Directional shadow mapping: light-space view/proj matrices + the depth
/// texture rendered by the shadow pass. Shadows are sampled by the PBR
/// program with the vendor's N·L-scaled bias.
void pbr_set_shadow(const float light_view[16], const float light_proj[16],
                    unsigned int shadow_depth_tex);
void pbr_enable_shadows(bool on);

/// Render a mesh with the PBR program (tangent/joint-aware).
/// @p model_matrix 4x4 column-major model transform (identity if null).
void pbr_render_mesh(const GPUMesh& mesh, const float* model_matrix,
                     const PBRMaterial& mat, bool wireframe = false);

/// Shadow pass: render the scene's depth from the directional light's point
/// of view into a depth texture, then pbr_render_mesh samples it. Create the
/// FBO once (create_shadow_fbo), then per frame:
///   av::begin_shadow_pass(fbo, w, h);
///   av::shadow_render_mesh(mesh, model_matrix);  // one per opaque mesh
///   av::end_shadow_pass();
unsigned int create_shadow_fbo(int width, int height, unsigned int* out_depth_tex);
void begin_shadow_pass(unsigned int fbo, int w, int h);
void end_shadow_pass();
void shadow_render_mesh(const GPUMesh& mesh, const float* model_matrix);

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
    // Default profile is Swordigo-faithful: restrained bloom so torch cores
    // glow without washing the level into haze.
    bool  bloom           = true;
    float bloom_strength  = 0.22f;
    float bloom_threshold = 0.95f;

    // Depth of field: CoC blur driven by the FBO depth texture. `dof_focus` is
    // the in-focus distance from the camera (world units).
    bool  dof        = false;
    float dof_focus  = 10.0f;
    float dof_scale  = 2.5f;

    // Cinematic color grade (applied after tone mapping).
    // Default profile: vanilla-like — readable shadows, restrained saturation,
    // a hint of warmth. PostFX finishes the lighting, never compensates for it.
    bool  color_grade = true;
    float saturation  = 1.05f;
    float contrast    = 1.04f;
    float brightness  = 0.03f;
    float warmth      = 0.05f;  // -1..1, warm(+)/cool(-) tint

    // Unsharp-mask sharpen / crispness.
    bool  sharpen        = true;
    float sharpen_amount = 0.30f;

    // Vignette.
    bool  vignette         = true;
    float vignette_strength = 0.22f;

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
extern float g_fill_color[3];          // cool fill from the opposite side
extern float g_rim_strength;           // camera-opposed edge light (0..~1)
extern float g_spec_strength;          // subtle Blinn-Phong sheen (0..~1)
extern float g_ambient_color[3];       // hemisphere sky fill
/// Vendor .scn light scales: `punctual` modulates the directional sun
/// (g_light_color), `ambient` the hemisphere fill (g_ambient_color / ground).
/// Both default to 1.0; the .scn preview saves/restores them around itself.
extern float g_light_scale;
extern float g_ambient_scale;
// Hemisphere ground fill (bounced ambient): surfaces facing down — platform
// undersides, ceilings, wall bases — fall toward this darker value instead of
// the sky fill, keeping corners readable without crushed blacks.
extern float g_ambient_ground_color[3];
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
