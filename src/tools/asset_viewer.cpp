/* asset_viewer.cpp — Ruby: Professional Asset Viewer & Editor for Swordigo Desktop
 *
 * Features:
 *   - Blender-like flat neutral dark theme (Blender style)
 *   - FontAwesome 6/7 solid icons integration (merged with Inter font)
 *   - Compact breadcrumb navigation for system-wide browsing
 *   - Parameter parsing: opens directly to a file and selects/previews it
 *   - Interactive lighting adjustments (elevation, azimuth, light & ambient colors)
 *   - PVR/PNG texture preview with zoom/pan and checkerboard background
 *   - 3D POD model viewport with orbit camera, wireframe, texturing
 *   - Audio WAV playback with waveform visualization
 *   - Scene file inspection with object tree and component details
 *
 * Build:  make ruby
 * Usage:  ./ruby [optional_path_to_dir_or_file]
 */

#define IMGUI_DEFINE_MATH_OPERATORS
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#define GL_GLEXT_PROTOTYPES 1
#include "platform/gl_inc.h"
#include <GL/glext.h>

#ifndef _WIN32
#include <unistd.h>
#endif
#include <set>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include "platform/pvr_loader.h"
#include "platform/data_path.h"
#include "platform/os_external.h"
#include "platform/embedded_assets.h"
#include "platform/IconsFontAwesome6.h"
#include "platform/protobuf_reader.h"
#include "tools/pod_loader.h"
#include "tools/gltf_glb.h"
#include "tools/obj_loader.h"
#include "tools/ani_loader.h"
#include "tools/scn_loader.h"
#include "tools/av_renderer.h"
#include "tools/av_audio.h"
#include "tools/scene_loader.h"
#include "tools/scene_schemas.h"
#include "tools/intellij.h"
#include "tools/boulder.h"
#include "tools/filerift.h"
#include "tools/batch_converter.h"
#include "tools/pod_convert.h"
#include "tools/scene_workspace.h"
#include "tools/scene_player.h"
#include "tools/scene_game.h"
#include "tools/ruby_mcp.h"
#include "tools/scene_categories.h"
#include "Guizmo/src/ImGuizmo.h"
#include <zlib.h>

// PNG writing for the texture image editor — the implementation lives in
// batch_converter.cpp (STB_IMAGE_WRITE_IMPLEMENTATION), declarations only.
#include "stb/stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <ctime>

#include <map>
#include <thread>
#include <atomic>
#include <mutex>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "tools/gltf_glb.h"
#include "tools/pod_writer.h"
#include "tools/fbx_import.h"

namespace fs = std::filesystem;

std::string g_assets_dir = "assets";
std::string g_instance_assets_dir = "assets";

// --- Additional FontAwesome definitions ---
#ifndef ICON_FA_PAUSE
#define ICON_FA_PAUSE "\xef\x81\x8c"
#endif
#ifndef ICON_FA_STOP
#define ICON_FA_STOP "\xef\x81\x8d"
#endif
#ifndef ICON_FA_FOLDER_OPEN
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc"
#endif
#ifndef ICON_FA_WARNING
#define ICON_FA_WARNING ICON_FA_TRIANGLE_EXCLAMATION
#endif
#ifndef ICON_FA_CHEVRON_RIGHT
#define ICON_FA_CHEVRON_RIGHT "\xef\x81\x94"
#endif
#ifndef ICON_FA_WARNING
#define ICON_FA_WARNING "\xef\x81\xb1"
#endif

// ============================================================================
// Constants
// ============================================================================

static const int   WIN_W             = 1400;
static const int   WIN_H             = 900;
static const char* WIN_TITLE         = "Ruby | Swordigo Studio";
static const float LEFT_PANEL_W      = 300.0f;
static const float RIGHT_PANEL_W     = 300.0f;
static const float STATUS_BAR_H      = 24.0f;
static const float PANEL_SPLITTER_W  = 5.0f;
static const float MIN_BROWSER_W     = 190.0f;
static const float MIN_INSPECTOR_W   = 220.0f;
static const float MIN_EDITOR_W      = 320.0f;
static const float MIN_EDITOR_H      = 180.0f;
static const char* GLSL_VERSION      = "#version 330";

struct RubyTheme {
    ImVec4 background, panel, panel_alt, surface, surface_hover, surface_active;
    ImVec4 border, text, text_muted, accent, warning, error, success, selection;
    bool dark = true;
};

static RubyTheme g_theme;

// ── Semantic theme helpers ──────────────────────────────────────────────────
// Components should consume these instead of ad-hoc RGB literals so a theme
// change (dark/light) stays consistent everywhere.
static ImVec4 th_mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
static ImVec4 th_alpha(const ImVec4& c, float a) { ImVec4 r = c; r.w = a; return r; }
static float th_luma(const ImVec4& c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }
static bool  th_is_dark() { return g_theme.dark; }
// Readable foreground for a given surface (contrast-safe fallback).
static ImVec4 th_text_on(const ImVec4& bg) {
    return th_luma(bg) > 0.5f ? ImVec4(0.10f, 0.12f, 0.16f, 1.0f) : ImVec4(0.88f, 0.90f, 0.94f, 1.0f);
}

// ============================================================================
// File entry & type classification
// ============================================================================

enum FileType { FTYPE_OTHER = 0, FTYPE_TEXTURE = 1, FTYPE_MODEL = 2, FTYPE_SCENE = 3, FTYPE_AUDIO = 4, FTYPE_TEXT = 5 };
enum PreviewType { PREVIEW_NONE = 0, PREVIEW_TEXTURE, PREVIEW_MODEL, PREVIEW_SCENE, PREVIEW_AUDIO, PREVIEW_TEXT = 5 };

struct FileEntry {
    std::string name;
    std::string full_path;
    bool        is_dir;
    size_t      size;
    int         type; // FileType
    std::string vendor_scn;   // non-empty: dir holds a renderer-master demo .scn (folder pack)
};

static int classify_file(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return FTYPE_OTHER;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    if (ext == ".pvr" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tex" || ext == ".tga") return FTYPE_TEXTURE;
    if (name.size() > 8) {
        std::string low = name;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find(".tex.png") != std::string::npos) return FTYPE_TEXTURE;
    }
    if (ext == ".pod") return FTYPE_MODEL;
    if (ext == ".fbx") return FTYPE_MODEL;
    if (ext == ".glb" || ext == ".gltf") return FTYPE_MODEL;   // glTF 2.0 (binary/JSON)
    if (ext == ".obj") return FTYPE_MODEL;   // Wavefront (renderer-master ext.joint/weight)
    if (ext == ".scene") return FTYPE_SCENE;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return FTYPE_AUDIO;
    if (ext == ".txt" || ext == ".lua" || ext == ".xml" || ext == ".plist" ||
        ext == ".json" || ext == ".shader" || ext == ".vs" || ext == ".fs" ||
        ext == ".cfg" || ext == ".ini" || ext == ".md" || ext == ".log" || ext == ".sh" ||
        ext == ".cpp" || ext == ".h" || ext == ".c" ||
        ext == ".scl" || ext == ".gdata" || ext == ".gstate" || ext == ".gplayer" ||
        ext == ".gopt" || ext == ".sounds" || ext == ".scmap" || ext == ".atlas" || ext == ".fnt" ||
        ext == ".gmesh") {
        return FTYPE_TEXT;
    }
    return FTYPE_OTHER;
}

// ============================================================================
// Viewer state
// ============================================================================

struct ViewerState {
    // File browser
    std::string            current_dir;
    std::vector<FileEntry> files;
    std::vector<FileEntry> filtered_files; // after search + type filter
    int                    selected_idx = -1;
    char                   search_buf[256] = {};
    char                   browser_path_buf[1024] = {};
    std::string            browser_path_source;
    int                    type_filter = 0; // 0=all, 1-4=specific

    // Preview
    PreviewType preview_type = PREVIEW_NONE;
    std::string status_msg;

    // Texture preview
    GLuint preview_tex = 0;
    int    tex_w = 0, tex_h = 0;
    float  tex_zoom = 1.0f;
    float  tex_pan_x = 0.0f, tex_pan_y = 0.0f;
    bool   tex_dragging = false;
    std::string tex_format_str;

    // Model preview
    av::PODModel             model;
    std::vector<av::GPUMesh> gpu_meshes;
    av::Camera               camera;
    GLuint                   fbo = 0, fbo_tex = 0;
    int                      fbo_w = 0, fbo_h = 0;
    bool                     show_wireframe = false;
    bool                     show_textured  = true;
    bool                     show_skeleton  = false;
    GLuint                   model_texture  = 0;
    int                      highlighted_mesh = -1;

    // PBR preview — vendored algorithms from zauonlok/renderer (MIT).
    // Toggles physically-based shading (GGX/Smith/Fresnel) with image-based
    // lighting (split-sum) and directional shadow maps for the model preview.
    bool                     pbr_preview = false;
    bool                     pbr_shadows = false;
    float                    pbr_env_intensity = 0.8f;

    // Vendored renderer-master formats (.obj / .ani / .scn) + glTF PBR data
    bool                          vendor_mode = false;          // preview is .obj or vendor .scn
    float                         vendor_anim_time = 0.0f;
    bool                          vendor_anim_playing = true;
    std::vector<av::AniSkeleton>  vendor_skeletons;             // per node (empty = none)
    std::vector<bool>             vendor_skel_valid;
    std::vector<av::PBRMaterial>  vendor_pbr_mats;              // per node (pbrm/pbrs)
    std::vector<bool>             vendor_pbr_valid;
    av::ScnScene                  vendor_scn;                   // parsed vendor scene
    std::vector<av::PBRMaterial>  gltf_pbr_materials;           // per PODModel material (.glb)
    std::vector<std::string>      gltf_pbr_img_paths;           // glTF image index -> temp path
    std::vector<GLuint>           vendor_owned_textures;        // PBR map textures to free on close
    float                         vendor_prev_bg[3] = {0, 0, 0};   // viewport bg before vendor .scn
    bool                          vendor_bg_saved = false;
    // Vendor .scn light state: `punctual`/`ambient` scales + shadow toggle are
    // adopted while the pack preview is open, then restored on close/swap.
    bool                          vendor_light_saved = false;
    float                         vendor_prev_light_scale = 1.0f;
    float                         vendor_prev_ambient_scale = 1.0f;
    bool                          vendor_prev_pbr_shadows = false;
    std::string                   current_dir_vendor_scn;   // demo-pack .scn inside current_dir ("" = none)
    GLuint                   shadow_fbo = 0, shadow_depth_tex = 0;
    int                      shadow_fbo_w = 0, shadow_fbo_h = 0;
    // glTF (GLB) preview: embedded textures spilled to temp files.
    std::vector<std::string> model_temp_files;
    std::map<std::string, std::string> model_gltf_alias;  // embedded name -> temp path

    // Scene preview
    av::SceneData scene;
    int           scene_preview_tab = 1; // 0 = Editor, 1 = 3D Visualizer, 2 = Tree Inspector
    bool          scene_text_dirty = false;
    int           selected_object = -1;
    std::map<std::string, av::PODModel> scene_model_cache;
    std::map<std::string, std::vector<av::GPUMesh>> scene_gpu_mesh_cache;
    std::map<std::string, std::vector<GLuint>> scene_texture_cache;
    std::vector<std::vector<av::GPUMesh>> scene_ground_gpu_meshes;
    std::vector<std::vector<GLuint>> scene_ground_textures;
    std::vector<std::vector<std::string>> scene_ground_tex_names; // parallel: tex id -> name (reuse cache)
    av::GPUMesh scene_proxy_mesh;
    // Background texture cache: BackgroundComponent name stem -> GL texture
    // (e.g. "graveyardback" -> graveyardback_2x.tex.png). The background is a
    // camera-following textured quad, not a POD model.
    std::map<std::string, GLuint> scene_background_textures;
    bool       scene_show_hidden = true;
    bool       obj_group_by_cat = false;    // Objects panel: flat list vs 11-category tree
    // Scene visualizer animation: per-object animation clocks so every model
    // plays its POD animation (no more bind-pose T-shape). scene_anim_playing
    // gates the loop; speed is in frames/second multiplier.
    bool       scene_anim_playing = true;
    float      scene_anim_speed = 1.0f;
    std::vector<float> scene_obj_anim_frame;   // per-object current frame

    // --- Scene Player subsystem (see tools/scene_player.h) ---
    sp::Player scene_player;
    bool       scene_player_window_open = false;
    bool       scene_xray = false;      // ghost see-through viewport mode

    // Post-processing (bloom / DOF / HD grade / vignette / grain)
    bool            postfx_enabled = true;
    av::PostFXParams postfx;
    bool  scene_lights_enabled = true;  // render scene Light/SimpleGlow components (vanilla-faithful default)
    bool  scene_glow_enabled = true;    // emissive glow sprites (bloom feed)
    bool  scene_light_debug = false;    // editor rings: influence radius + emitter cross
    bool  scene_depth_fog_enabled = true; // vanilla atmospheric depth darkening
    bool  scene_water_enabled = true;   // render WaterMesh fluid sheets (water/lava)
    bool  scene_shadows_enabled = true; // render ShadowComponent contact blobs
    bool  scene_overlay_enabled = true; // apply overlay-light darkness veil
    bool  scene_lighting_enabled = true; // dynamic Lambert lighting (off = flat texture)
    bool  scene_normals_debug = false;   // color = world normal (orientation check)
    // Textures for parsed WaterMesh sheets (parallel to st.scene.waters).
    std::vector<GLuint> scene_water_textures;
    float render_scale = 3.0f;          // high-DPI FBO render scale (default 3x)
    // ── Adaptive-quality governor ──
    // Instead of guessing from the GPU brand, Ruby *measures* real framed this
    // session. It starts at the highest tier a machine can plausibly drive,
    // then watches frame time. When the viewport struggles the tier is dropped
    // step-by-step (not all at once); once the machine has headroom again it
    // slowly drifts back up. The settled tier is saved so the next launch
    // starts at a good spot instead of re-probing from scratch.
    int   perf_tier          = 3;       // 0..3 index into the tier table
    float perf_frame_ms      = 16.0f;   // smoothed frame time
    int   perf_struggle_count= 0;       // consecutive struggling frames lately
    float perf_idle_ms       = 0.0f;    // time spent settling at current tier
    bool  perf_auto          = true;    // auto-adapt (vs. user-fixed scale)
    int   selected_background_obj = -1; // background layer picker (-1 = auto first visible)
    int        scene_transform_mode = 0; // 0 navigate, 1 move, 2 rotate, 3 scale
    bool       scene_pointer_active = false;
    bool       scene_transform_drag = false;
    ImVec2     scene_pointer_start = ImVec2(0, 0);
    int        scene_component_type = 0;
    bool       scene_has_object_clipboard = false;
    av::SceneObject scene_object_clipboard;
    std::vector<av::SceneObject> scene_object_clipboard_multi; // multi-select clipboard
    int        scene_paste_count = 0;      // nudge offset counter per paste
    bool       scene_has_component_clipboard = false;
    av::SceneComponent scene_component_clipboard;
    int        scene_script_index = -1;
    char       scene_script_buf[16384] = {};

    // --- Ground Mesh Editing (vertex-level) ---
    bool  scene_mesh_edit = false;      // vertex editing mode toggle (M key / toolbar)
    int   mesh_edit_object = -1;        // object being edited (usually == selected_object)
    int   mesh_edit_mesh = -1;          // sub-mesh holding the selected vertex
    int   mesh_edit_vertex = -1;        // selected vertex index
    bool  mesh_edit_drag = false;       // currently dragging the selected vertex
    float mesh_edit_plane_y = 0.0f;     // world Y of the vertex-drag plane

    // --- Transform gizmo / workspace tooling ---
    bool  scene_show_gizmo = true;      // show the axis gizmo on the selected object
    int   gizmo_axis = -1;              // axis under cursor / being dragged (0=X 1=Y 2=Z)
    bool  gizmo_drag = false;           // dragging a gizmo axis
    int   transform_axis = -1;          // keyboard axis lock during drag (-1 = free)
    bool  scene_snap = false;           // grid snapping toggle (or hold Ctrl)
    float scene_snap_step = 1.0f;       // snap grid size
    bool  scene_show_axis = true;       // corner axis-indicator overlay
    int   scene_rendered_objects = 0;   // latest viewport status counts
    int   scene_proxy_objects = 0;

    // --- Multi-select (Ctrl+click in viewport or tree) ---
    std::vector<int> scene_selection;   // indices of all selected objects

    // --- Mesh-edit tool + element selection ---
    int   mesh_edit_tool = 0;           // 0=vertex 1=face 2=edge
    int   mesh_edit_triangle = -1;      // picked triangle index (face tool)
    int   mesh_edit_edge_a = -1;        // picked edge (v0) (edge tool)
    int   mesh_edit_edge_b = -1;        // picked edge (v1)

    // --- Ground Mesh Generator (SMM2-style 2D sketch) ---
    int   gm_workspace_mode = 0;        // 0=create/sketch, 1=edit existing geometry
    bool  gm_sketch_dirty = false;      // sketch changed since last build
    std::vector<boulder::PolygonPoint> gm_points;   // sketch polygon (XY, Y-up)
    float gm_z = 40.0f;                 // constant world Z (depth) for the mesh
    float gm_min_depth = -45.0f;        // extrusion along Z
    float gm_max_depth = 45.0f;
    float gm_top_angle  = 20.0f;
    bool  gm_generate_top = true;
    char  gm_top_tex[96]    = "fire_grass";
    char  gm_bottom_tex[96] = "graveyard_ground";
    int   gm_drag_point = -1;           // point being dragged (-1 = none)
    bool  gm_dragging = false;
    int   gm_tool = 0;                  // 0=move/add, 1=add-only, 2=erase, 3=freehand
    bool  gm_freehand_active = false;
    float gm_simplify = 2.5f;
    int   gm_target_points = 0;
    bool  gm_mirror_x = false;          // mirror newly placed points across X
    float gm_canvas_scale = 1.0f;       // world units per screen px (2D canvas)
    float gm_canvas_cx = 0.0f, gm_canvas_cy = 0.0f; // canvas world origin
    char  gm_obj_name[96] = "ground_mesh";
    std::vector<boulder::Hat> gm_hats;  // round-hat domes on the sketch
    int   gm_drag_hat = -1;             // hat being moved/resized (-1 = none)
    bool  gm_hat_dragging = false;
    bool  gm_hat_resizing = false;      // dragging a hat's radius handle
    float gm_hat_radius = 60.0f;        // defaults for newly placed hats
    float gm_hat_height  = 40.0f;
    int   gm_edit_apply_object = -1;    // object being edited via the 2D canvas
    bool  gm_inline_edit = false;       // inline 2D polygon editing in the visualizer
    bool  gm_inline_dirty = false;      // sketch changed since last live re-apply
    int   gm_inline_hover_vertex = -1;  // vertex under cursor (inline mode)
    int   gm_inline_hover_edge = -1;    // edge under cursor (RMB inserts here)
    bool  gm_drag_from_insert = false;  // drag armed by an RMB insert (RMB-held drag)
    double gm_drag_off_x = 0.0;         // cursor→vertex grab offset (local units)
    double gm_drag_off_y = 0.0;
    av::Camera gm_inline_saved_cam;     // camera snapshot restored on exit
    bool  gm_inline_saved_valid = false;

    // --- Ground Mesh Generator: live 3D preview of the extruded mesh ---
    bool  gm_show_3d = true;            // split view: 2D sketch | 3D preview
    GLuint gm_preview_fbo = 0;
    GLuint gm_preview_fbo_tex = 0;
    int   gm_preview_w = 0, gm_preview_h = 0;
    std::vector<av::GPUMesh> gm_preview_meshes;
    std::vector<GLuint>      gm_preview_textures;
    av::Camera gm_preview_cam;          // orbit camera for the 3D preview
    bool  gm_preview_valid = false;     // last build succeeded

    // --- POD preview chrome ---
    bool  model_auto_rotate = false;    // slow turntable orbit (R toggles)

    // --- Object Browser (SMM2-style add-object palette) ---
    bool  obj_browser_open = false;
    char  obj_search_buf[128] = {};      // Objects panel search box
    char  obj_prop_search_buf[128] = {}; // PROPERTIES panel component filter
    bool  obj_browser_scanned = false;
    std::vector<std::string> obj_browser_pods;     // *.pod files found
    std::vector<std::string> obj_browser_swdm;     // *.swdm files found
    std::vector<GLuint>      obj_browser_thumbs;   // mini-render textures (per pod, 0 = pending)
    size_t obj_browser_thumb_next = 0;             // lazy thumbnail progress
    char  obj_browser_search[128] = {};
    std::string obj_browser_dir;

    // Checkerboard texture (for transparency)
    GLuint checker_tex = 0;

    // Selected file info
    std::string sel_name;
    std::string sel_path;
    size_t      sel_size = 0;

    // Multi-texture & Animation Support
    std::vector<GLuint>      model_textures;
    std::vector<std::string> missing_textures;
    float                    current_frame = 0.0f;
    bool                     anim_playing = false;
    float                    anim_timer = 0.0f;
    float                    anim_fps = 30.0f;
    float                    uploaded_skin_frame = -1.0f;

    // Settings / Preferences
    bool        show_settings = false;
    int         ui_theme = 0; // 0 = Professional Dark, 1 = Professional Light
    float       ui_font_scale = 1.0f;
    float       display_scale = 1.0f;
    bool        show_full_path_status = false;
    bool        auto_refresh_dirs = true;
    float       bg_color[3] = {0.035f, 0.045f, 0.065f};
    bool        show_grid = true;
    float       grid_size = 20.0f;
    bool        enable_vsync = true;
    float       cam_orbit_speed = 1.0f;
    float       cam_zoom_speed = 1.0f;
    float       cam_pan_speed = 1.0f;
    bool        cam_invert_x = false;
    bool        cam_invert_y = false;
    bool        tex_pixel_art_mode = true;
    bool        anim_autoplay = true;

    // Asset Transformations
    int         texture_rotation = 0; // 0, 90, 180, 270 degrees
    bool        texture_flip_h = true;
    bool        texture_flip_v = true;
    float       texture_tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // ── Texture image editor (CPU-side RGBA8 buffer, top-left origin) ──
    std::vector<uint8_t> tex_edit_pixels;
    int    tex_edit_w = 0, tex_edit_h = 0;
    bool   tex_edit_valid = false;    // buffer ready and matches preview_tex
    bool   tex_edit_dirty = false;    // pixels differ from the loaded file
    // Swordigo texture containers (.tex/.tex.png/.pvr) are stored flipped;
    // while edit mode is on the buffer is pre-flipped to display space so
    // strokes land exactly where the user sees them. Saved back to file space.
    bool   tex_edit_display_space = false;
    bool   tex_edit_enabled = false;  // editing tools active
    std::string tex_edit_src;         // original file path (save-back target)
    std::string tex_edit_ext;         // original extension (lowercased)
    int    tex_edit_tool = 0;         // 0 brush, 1 eraser, 2 fill, 3 pick, 4 crop
    float  tex_edit_color[4] = {0.16f, 0.55f, 1.0f, 1.0f};
    float  tex_edit_size = 6.0f;
    int    tex_edit_last_x = -1, tex_edit_last_y = -1;
    double tex_edit_last_upload = 0.0;   // throttle for live stroke uploads
    bool   tex_edit_crop_active = false;
    bool   tex_edit_crop_ready = false;
    float  tex_edit_crop_x0 = 0, tex_edit_crop_y0 = 0;
    float  tex_edit_crop_x1 = 0, tex_edit_crop_y1 = 0;
    std::vector<uint8_t> tex_edit_undo;    // single-step undo snapshot
    char   tex_edit_save_path[4096] = {};

    float       model_rotate_x = 0.0f;
    float       model_rotate_y = 0.0f;
    float       model_rotate_z = 0.0f;
    float       model_scale = 1.0f;

    // Texture Reset & general file viewer content
    bool        tex_reset_required = false;
    std::string text_preview_content;
    std::string text_edit_buffer;
    bool        text_is_binary = false;
    bool        text_select_mode = false;
    bool        text_edit_modified = false;
    ImFont*     mono_font = nullptr;
    
    // Bottom panel (Terminal / Logger)
    bool                     show_bottom_panel = false;  // hidden by default, Ctrl+` to open
    bool                     show_asset_browser = true;
    bool                     show_inspector = true;
    float                    asset_browser_width = LEFT_PANEL_W;
    float                    inspector_width = RIGHT_PANEL_W;
    float                    bottom_panel_height = 220.0f;
    bool                     workspace_layout_dirty = false;
    // PTY terminal state
int                      pty_master_fd   = -1;
#ifndef _WIN32
pid_t                    pty_child_pid   = -1;
#endif
    std::string              pty_output_buf; // raw bytes from PTY, stripped of control seqs
    char                     terminal_input[512] = {};
    bool                     terminal_scroll_to_bottom = false;

    // --- Scene Editor state ---
    bool        scene_dirty = false;       // true when unsaved changes exist
    bool        scene_save_requested = false;
    std::string scene_save_msg;            // status message after last save
    float       scene_save_msg_timer = 0.0f; // countdown to clear msg

    // Scene loading screen: set right before a heavy synchronous load
    // (scene_load with .scl library resolution, or player_begin world bake);
    // the modal is drawn for a few frames so the user sees it complete.
    bool        scene_loading = false;
    float       scene_loading_frames = 0.0f; // remaining frames of the modal
    std::string scene_loading_msg;
    // Per-object OnLoad script editor
    char        scene_onload_buf[16384]   = {}; // decoded Lua source from selected obj onload
    bool        scene_onload_modified = false; // true if user edited the script
    char        scene_obj_name_buf[256]    = {}; // editable name field
    char        scene_obj_template_buf[256]= {}; // editable template field
    std::vector<av::SceneData> scene_undo_stack;
    std::vector<av::SceneData> scene_redo_stack;
    
    // --- IntelliJ Text Editor state ---
    intel::IntelliJ intellij_editor;
    std::vector<std::pair<std::string, std::string>> intellij_styles; // {name, path}
    int             intellij_style_idx = 0;
    ImVec4          editor_custom_bg = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // alpha > 0 triggers custom BG
    
    // Compile diagnostics feedback
    bool            has_compile_result = false;
    bool            compile_success = false;
    std::string     compile_error_msg;
    double          compile_time_ms = 0.0;

    // Batch Converter
    batch::BatchState batch_converter;

    // Blender round-trip bridge (Stage 4)
    char        blender_path[512] = "/usr/bin/blender";
    std::string blender_staging_dir;    // absolute staging dir
    bool        blender_active = false; // a round-trip is in flight
    int         blender_run_id = 0;
    std::string blender_source_pod;     // pod we wrote to / will reload from
    std::string blender_status;         // status line shown in UI
    std::atomic<bool> blender_stop{false};
    std::atomic<bool> blender_done{false};   // daemon finished a result
    std::atomic<bool> blender_ok{false};     // daemon result success
    std::string blender_result_msg;          // daemon result message
    std::mutex  blender_mutex;               // guards blender_source_pod/status writes
    std::thread blender_daemon;

    // FBX → game POD converter (right-panel "Convert to POD" button)
    bool        convert_tex_pgm  = true;     // re-encode textures to .tex.png
    bool        convert_flip_v   = true;      // flip V (FBX top → game bottom)
    std::string convert_status;                // last conversion result line
};


static ViewerState g_state;

static bool load_scene_model_to_cache(ViewerState& st, const std::string& mesh_name,
                                      const std::string& scene_dir_path,
                                      const std::string& base_hint = "");
static GLuint load_scene_background_texture(ViewerState& st, const std::string& bg_name,
                                            const std::string& scene_dir_path);
static void upload_scene_ground_meshes(ViewerState& st, const std::string& scene_dir_path);
static GLuint load_ground_mesh_texture(const fs::path& scene_dir,
                                       const std::string& tex_name);
static void frame_scene_camera(ViewerState& st);
static void frame_scene_at_spawn(ViewerState& st, int index);
static std::string scene_play_anim_pod(const ViewerState& st, int obj_index,
                                       const std::string& base_mname);

// True when the play object's KeyframeAnimation is non-repeating: the game
// plays it once and HOLDS the last frame (looping it would be wrong for
// one-shot anims). The draw pass clamps the frame to the model's last key.
static bool scene_anim_holds_last(const ViewerState& st, int obj_index) {
    if (st.scene_player.mode == sp::Mode::Off) return false;
    for (const auto& p : st.scene_player.objects) {
        if (p.index == obj_index) return !p.anim_repeating;
    }
    return false;
}

// Scene mesh-editing / workspace helpers (defined with the visualizer section)
static void resync_scene_ground_meshes(ViewerState& st);
static void reupload_object_ground_meshes(ViewerState& st, int object_index);
static void upload_scene_waters(ViewerState& st, const std::string& scene_dir_path);

// Texture image editor (defined with the texture viewer section, after
// blender_texture_to_rgba). Forward declarations for the file-open path,
// the preview canvas and the toolbar.
static void tex_editor_clear(ViewerState& st);
static void tex_editor_load(ViewerState& st, const std::string& path);
static void tex_editor_upload(ViewerState& st);
static void tex_edit_snapshot(ViewerState& st);
static void tex_edit_undo(ViewerState& st);
static void tex_edit_brush_at(ViewerState& st, int x, int y, bool erase);
static void tex_edit_brush_line(ViewerState& st, int x0, int y0, int x1, int y1, bool erase);
static void tex_edit_flood_fill(ViewerState& st, int x, int y);
static void tex_edit_bake_rotate(ViewerState& st, int steps_cw);
static void tex_edit_bake_flip(ViewerState& st, bool horizontal);
static void tex_edit_apply_crop(ViewerState& st);
static bool tex_edit_save_png(ViewerState& st, const std::string& path);
static bool tex_edit_save_tex(ViewerState& st, const std::string& path);
static bool tex_edit_is_container(const ViewerState& st);
static void tex_edit_flip_buffer_hv(std::vector<uint8_t>& px, int w, int h);

static void flush_scene_onload_editor(ViewerState& st) {
    if (!st.scene_onload_modified || st.selected_object < 0 ||
        st.selected_object >= static_cast<int>(st.scene.objects.size()))
        return;
    proto::Writer program;
    program.write_string_field(1, std::string(st.scene_onload_buf));
    st.scene.objects[st.selected_object].onload = program.to_string();
    st.scene_onload_modified = false;
}

static void sync_scene_object_editor(ViewerState& st) {
    st.scene_onload_buf[0] = '\0';
    st.scene_onload_modified = false;
    st.scene_obj_name_buf[0] = '\0';
    st.scene_obj_template_buf[0] = '\0';
    if (st.selected_object < 0 || st.selected_object >= static_cast<int>(st.scene.objects.size()))
        return;

    const auto& object = st.scene.objects[st.selected_object];
    snprintf(st.scene_obj_name_buf, sizeof(st.scene_obj_name_buf), "%s", object.name.c_str());
    snprintf(st.scene_obj_template_buf, sizeof(st.scene_obj_template_buf), "%s", object.template_name.c_str());
}

static void select_scene_object(ViewerState& st, int index) {
    flush_scene_onload_editor(st);
    st.selected_object = index;
    st.scene_selection.clear();
    if (index >= 0) st.scene_selection.push_back(index);
    sync_scene_object_editor(st);
}

/// Switch the scene preview tab with transition side-effects: leaving the
/// Mesh editor clears its forced edit mode; entering it arms mesh editing.
/// Every scene_preview_tab write should go through here.
static void switch_scene_tab(ViewerState& st, int mode) {
    if (st.scene_preview_tab == 3 && mode != 3)
        st.scene_mesh_edit = false;      // leaving the Mesh editor
    if (mode == 3) {
        st.scene_mesh_edit = true;       // entering the Mesh editor
        st.mesh_edit_object = st.selected_object;
    }
    st.scene_preview_tab = mode;
}

static void open_ground_mesh_studio(ViewerState& st) {
    st.gm_workspace_mode = 0;
    switch_scene_tab(st, 3);
}

// Multi-select helpers -------------------------------------------------------

static bool is_scene_selected(const ViewerState& st, int index) {
    for (int sel : st.scene_selection) if (sel == index) return true;
    return false;
}

/// Ctrl+click toggle: add/remove @p index from the selection.  The active
/// object always stays in the set (last clicked index becomes active).
static void toggle_scene_selection(ViewerState& st, int index) {
    if (index < 0 || index >= (int)st.scene.objects.size()) return;
    auto it = std::find(st.scene_selection.begin(), st.scene_selection.end(), index);
    if (it != st.scene_selection.end()) {
        // Removing the active object — keep another member active.
        st.scene_selection.erase(it);
        if (st.selected_object == index) {
            if (!st.scene_selection.empty())
                st.selected_object = st.scene_selection.back();
            else
                st.selected_object = -1;
            sync_scene_object_editor(st);
        }
    } else {
        st.scene_selection.push_back(index);
        st.selected_object = index;   // newly clicked object becomes active
        sync_scene_object_editor(st);
    }
}

static void frame_scene_selection(ViewerState& st);

/// Drop out-of-range indices after undo/delete; keep the active object in set.
static void prune_scene_selection(ViewerState& st) {
    std::vector<int> kept;
    kept.reserve(st.scene_selection.size());
    for (int sel : st.scene_selection)
        if (sel >= 0 && sel < (int)st.scene.objects.size())
            kept.push_back(sel);
    st.scene_selection = std::move(kept);
    if (st.scene_selection.empty() && st.selected_object >= 0 &&
        st.selected_object < (int)st.scene.objects.size())
        st.scene_selection.push_back(st.selected_object);
}

static void snapshot_scene(ViewerState& st) {
    flush_scene_onload_editor(st);
    st.scene_undo_stack.push_back(st.scene);
    if (st.scene_undo_stack.size() > 64)
        st.scene_undo_stack.erase(st.scene_undo_stack.begin());
    st.scene_redo_stack.clear();
}

static bool restore_scene_history(ViewerState& st, bool redo) {
    auto& source = redo ? st.scene_redo_stack : st.scene_undo_stack;
    auto& destination = redo ? st.scene_undo_stack : st.scene_redo_stack;
    if (source.empty())
        return false;

    flush_scene_onload_editor(st);
    destination.push_back(st.scene);
    if (destination.size() > 64)
        destination.erase(destination.begin());
    st.scene = std::move(source.back());
    source.pop_back();
    av::scene_refresh(st.scene);
    if (st.scene.objects.empty())
        st.selected_object = -1;
    else
        st.selected_object = std::clamp(st.selected_object, 0, static_cast<int>(st.scene.objects.size()) - 1);
    prune_scene_selection(st);
    sync_scene_object_editor(st);
    // Vertex-edit selection may point at a mesh that no longer exists.
    st.mesh_edit_mesh = -1;
    st.mesh_edit_vertex = -1;
    st.mesh_edit_triangle = -1;
    st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
    st.mesh_edit_drag = false;
    st.gizmo_drag = false;
    st.gizmo_axis = -1;
    // GPU caches may reference ground meshes that changed; re-sync them.
    if (st.preview_type == PREVIEW_SCENE && !st.scene_ground_gpu_meshes.empty())
        resync_scene_ground_meshes(st);
    st.scene_dirty = true;
    return true;
}

static void copy_scene_selection(ViewerState& st) {
    if (st.selected_object < 0 || st.selected_object >= (int)st.scene.objects.size()) return;
    st.scene_object_clipboard_multi.clear();
    const std::vector<int>& selection = st.scene_selection.empty()
        ? std::vector<int>{st.selected_object} : st.scene_selection;
    for (int index : selection) {
        if (index >= 0 && index < (int)st.scene.objects.size())
            st.scene_object_clipboard_multi.push_back(st.scene.objects[index]);
    }
    st.scene_object_clipboard = st.scene.objects[st.selected_object];
    st.scene_has_object_clipboard = true;
    st.scene_paste_count = 0;
    st.status_msg = "Copied " + std::to_string(st.scene_object_clipboard_multi.size()) + " object(s)";
}

static void paste_scene_selection(ViewerState& st) {
    if (!st.scene_has_object_clipboard || st.scene.filepath.empty()) return;
    snapshot_scene(st);
    const float nudge = 24.0f * (float)(++st.scene_paste_count);
    const auto& source = st.scene_object_clipboard_multi.empty()
        ? std::vector<av::SceneObject>{st.scene_object_clipboard}
        : st.scene_object_clipboard_multi;
    st.scene_selection.clear();
    for (const auto& copied : source) {
        av::SceneObject pasted = copied;
        pasted.name = av::scene_fresh_identifier(st.scene);
        pasted.pos_x += nudge;
        pasted.pos_y += nudge;
        st.scene.objects.push_back(std::move(pasted));
        st.scene_selection.push_back((int)st.scene.objects.size() - 1);
    }
    av::scene_refresh(st.scene);
    st.selected_object = st.scene_selection.empty() ? -1 : st.scene_selection.back();
    sync_scene_object_editor(st);
    resync_scene_ground_meshes(st);
    st.scene_dirty = true;
    st.status_msg = "Pasted " + std::to_string(source.size()) + " object(s)";
}

static void duplicate_scene_selection(ViewerState& st) {
    if (st.selected_object < 0 || st.selected_object >= (int)st.scene.objects.size()) return;
    copy_scene_selection(st);
    paste_scene_selection(st);
    st.status_msg = "Duplicated selection";
}

static void delete_scene_selection(ViewerState& st) {
    if (st.scene_selection.empty()) return;
    snapshot_scene(st);
    std::vector<int> deleted = st.scene_selection;
    std::sort(deleted.begin(), deleted.end(), std::greater<int>());
    for (int index : deleted) {
        if (index >= 0 && index < (int)st.scene.objects.size())
            av::scene_delete_object(st.scene, static_cast<size_t>(index));
    }
    const int survivor = st.scene.objects.empty() ? -1
        : std::clamp(deleted.back(), 0, (int)st.scene.objects.size() - 1);
    select_scene_object(st, survivor);
    resync_scene_ground_meshes(st);
    st.scene_dirty = true;
    st.status_msg = "Deleted " + std::to_string(deleted.size()) + " object(s)";
}

static void move_scene_object(ViewerState& st, int direction) {
    const int destination = st.selected_object + direction;
    if (st.selected_object < 0 || destination < 0 || destination >= (int)st.scene.objects.size()) return;
    snapshot_scene(st);
    av::scene_move_object(st.scene, st.selected_object, destination);
    select_scene_object(st, destination);
    st.scene_dirty = true;
}

// ============================================================================
// Helpers
// ============================================================================

static std::string resolve_style_path(const std::string& filename) {
    std::string paths[] = {
        "/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoTools/ss/" + filename,
        "../SwordigoTools/ss/" + filename,
        "SwordigoTools/ss/" + filename,
        "ss/" + filename,
        "./" + filename
    };
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return "";
}

static std::string expand_home(const std::string& p) {
    if (!p.empty() && p[0] == '~') {
        const char* home = getenv("HOME");
        if (home) return std::string(home) + p.substr(1);
    }
    return p;
}

static fs::path find_pod_resource(const fs::path& root, const std::string& resource) {
    if (root.empty() || resource.empty()) return {};
    auto lowercase = [](std::string value) {
        for (char& ch : value) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return value;
    };
    std::error_code ec;
    const fs::path direct = root / resource;
    if (fs::is_regular_file(direct, ec)) return direct;
    const std::string wanted_name = lowercase(fs::path(resource).filename().string());
    const std::string wanted_stem = lowercase(fs::path(resource).stem().string());
    for (fs::recursive_directory_iterator iterator(root,
             fs::directory_options::skip_permission_denied, ec), end;
         iterator != end && !ec; iterator.increment(ec)) {
        if (iterator.depth() > 6) { iterator.disable_recursion_pending(); continue; }
        if (!iterator->is_regular_file(ec)) continue;
        const fs::path candidate = iterator->path();
        if (lowercase(candidate.extension().string()) != ".pod") continue;
        if (lowercase(candidate.filename().string()) == wanted_name ||
            lowercase(candidate.stem().string()) == wanted_stem)
            return candidate;
    }
    return {};
}

static std::string format_size(size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) {
        char buf[64]; snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        return buf;
    }
    char buf[64]; snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

static std::string format_time(float seconds) {
    int m = (int)(seconds / 60.0f);
    int s = (int)seconds % 60;
    char buf[32]; snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

static std::vector<std::string> g_file_logs;
static void log_file_event(const std::string& type, const std::string& message) {
    time_t rawtime;
    struct tm * timeinfo;
    char time_str[32];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    if (timeinfo) {
        strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
    } else {
        strcpy(time_str, "00:00:00");
    }
    std::string line = "[" + std::string(time_str) + "] [" + type + "] " + message;
    g_file_logs.push_back(line);
    if (g_file_logs.size() > 500) {
        g_file_logs.erase(g_file_logs.begin());
    }
    std::cout << "[RubyLog] " << line << std::endl;
}

static const char* filetype_label(int ft, const char* path) {
    // Model previews can be POD or FBX — report the concrete type.
    if (ft == FTYPE_MODEL && path) {
        std::string low(path);
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.size() >= 4 && low.substr(low.size() - 4) == ".fbx") return "FBX Model";
        return "POD Model";
    }
    switch (ft) {
        case FTYPE_TEXTURE: return "Texture";
        case FTYPE_MODEL:   return "Model";
        case FTYPE_SCENE:   return "Scene";
        case FTYPE_AUDIO:   return "Audio";
        default:            return "File";
    }
}

// ============================================================================
// Create checkerboard texture for alpha backgrounds
// ============================================================================

static GLuint create_checkerboard() {
    const int sz = 64;
    const int cell = 8;
    unsigned char pixels[sz * sz * 4];
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            bool light = ((x / cell) + (y / cell)) % 2 == 0;
            unsigned char v = light ? 200 : 150;
            int i = (y * sz + x) * 4;
            pixels[i] = pixels[i+1] = pixels[i+2] = v;
            pixels[i+3] = 255;
        }
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

// ============================================================================
// Directory listing
// ============================================================================

// renderer-master demo packs are whole folders: a <name>.scn scene file plus
// the .obj/.tga/.ani files it references. Return the first vendor .scn found
// in `dir` (first line starts with "type:"), or "" when dir is not a pack.
// The demo packs all follow the <dirname>.scn convention, so the common case
// is one fs::exists stat; the full directory scan only runs as a fallback.
static std::string find_vendor_scn_in_dir(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return "";
    auto vendor = [&](const fs::path& p) -> bool {
        std::ifstream f(p.string(), std::ios::binary);
        char head[64] = {0};
        if (!f || !f.read(head, 63)) return false;
        std::string h(head);
        return h.rfind("type:", 0) == 0;
    };
    // Fast path: <dirname>.scn (azura/azura.scn, crab/crab.scn, ...).
    fs::path stem = fs::path(dir).filename();
    if (!stem.empty()) {
        fs::path candidate = fs::path(dir) / (stem.string() + ".scn");
        if (fs::exists(candidate, ec) && vendor(candidate)) return candidate.string();
    }
    // Fallback: any *.scn with the vendor header (e.g. common/sphere.scn).
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".scn") continue;
        if (vendor(entry.path())) return entry.path().string();
    }
    return "";
}

static void refresh_directory(ViewerState& st) {
    st.files.clear();
    st.filtered_files.clear();
    st.selected_idx = -1;

    std::error_code ec;
    if (!fs::is_directory(st.current_dir, ec)) return;

    // ".." entry
    fs::path parent = fs::path(st.current_dir).parent_path();
    if (!parent.empty() && parent != st.current_dir) {
        FileEntry up;
        up.name = "..";
        up.full_path = parent.string();
        up.is_dir = true;
        up.size = 0;
        up.type = FTYPE_OTHER;
        st.files.push_back(up);
    }

    for (auto& entry : fs::directory_iterator(st.current_dir, ec)) {
        FileEntry fe;
        fe.name = entry.path().filename().string();
        fe.full_path = entry.path().string();
        fe.is_dir = entry.is_directory(ec);
        fe.size = fe.is_dir ? 0 : (size_t)entry.file_size(ec);
        fe.type = fe.is_dir ? FTYPE_OTHER : classify_file(fe.name);
        if (fe.is_dir) fe.vendor_scn = find_vendor_scn_in_dir(fe.full_path);
        st.files.push_back(fe);
    }
    st.current_dir_vendor_scn = find_vendor_scn_in_dir(st.current_dir);

    // Sort: directories first (except ".."), then alphabetical case-insensitive
    std::sort(st.files.begin(), st.files.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.is_dir != b.is_dir) return a.is_dir;
        std::string la = a.name, lb = b.name;
        for (auto& c : la) c = (char)tolower((unsigned char)c);
        for (auto& c : lb) c = (char)tolower((unsigned char)c);
        return la < lb;
    });
}

// --- Blender round-trip bridge (Stage 4) forward declarations -------------
static void blender_start_roundtrip(ViewerState& st);
static void blender_daemon_main(ViewerState& st);
static void blender_poll_result(ViewerState& st);
static void blender_shutdown(ViewerState& st);
static bool blender_install_extension(ViewerState& st, std::string* err_out);

static void apply_filters(ViewerState& st) {
    st.filtered_files.clear();
    std::string search_lower(st.search_buf);
    for (auto& c : search_lower) c = (char)tolower((unsigned char)c);

    for (auto& f : st.files) {
        if (st.type_filter != 0 && !f.is_dir && f.type != st.type_filter) continue;
        if (!search_lower.empty()) {
            std::string name_lower = f.name;
            for (auto& c : name_lower) c = (char)tolower((unsigned char)c);
            if (name_lower.find(search_lower) == std::string::npos) continue;
        }
        st.filtered_files.push_back(f);
    }
}

// ============================================================================
// Orthographic projection in the renderer's column-major convention — used
// for the PBR directional shadow-map frustum (light space).
static void light_ortho(float out[16], float l, float r, float b, float t,
                        float n, float f) {
    memset(out, 0, 16 * sizeof(float));
    out[0]  = 2.0f / (r - l);
    out[5]  = 2.0f / (t - b);
    out[10] = -2.0f / (f - n);
    out[12] = -(r + l) / (r - l);
    out[13] = -(t + b) / (t - b);
    out[14] = -(f + n) / (f - n);
    out[15] = 1.0f;
}

// Resource cleanup
// ============================================================================

static void free_preview_resources(ViewerState& st) {
    for (auto& m : st.gpu_meshes) av::free_mesh(m);
    st.gpu_meshes.clear();
    st.model = av::PODModel{};

    if (st.preview_tex) { glDeleteTextures(1, &st.preview_tex); st.preview_tex = 0; }
    tex_editor_clear(st);
    st.tex_w = st.tex_h = 0;
    st.tex_zoom = 1.0f;
    st.tex_pan_x = st.tex_pan_y = 0.0f;
    st.tex_format_str.clear();

    if (st.model_texture) {
        bool in_vector = false;
        for (auto tex : st.model_textures) {
            if (tex == st.model_texture) { in_vector = true; break; }
        }
        if (!in_vector) {
            glDeleteTextures(1, &st.model_texture);
        }
        st.model_texture = 0;
    }

    av::audio_stop();

    for (auto tex : st.model_textures) {
        if (tex) glDeleteTextures(1, &tex);
    }
    st.model_textures.clear();
    st.missing_textures.clear();

    // Remove glTF embedded-texture temp files + the PBR shadow FBO.
    for (const auto& p : st.model_temp_files) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    st.model_temp_files.clear();
    st.model_gltf_alias.clear();

    // Vendored formats + glTF PBR: free owned textures and reset state.
    for (auto tex : st.vendor_owned_textures) {
        if (tex) glDeleteTextures(1, &tex);
    }
    st.vendor_owned_textures.clear();
    st.gltf_pbr_materials.clear();
    st.gltf_pbr_img_paths.clear();
    st.vendor_mode = false;
    st.vendor_anim_time = 0.0f;
    st.vendor_anim_playing = true;
    st.vendor_skeletons.clear();
    st.vendor_skel_valid.clear();
    st.vendor_pbr_mats.clear();
    st.vendor_pbr_valid.clear();
    st.vendor_scn = av::ScnScene{};
    if (st.vendor_bg_saved) {
        std::memcpy(st.bg_color, st.vendor_prev_bg, sizeof(float) * 3);
        st.vendor_bg_saved = false;
    }
    if (st.vendor_light_saved) {
        av::g_light_scale = st.vendor_prev_light_scale;
        av::g_ambient_scale = st.vendor_prev_ambient_scale;
        st.pbr_shadows = st.vendor_prev_pbr_shadows;
        st.vendor_light_saved = false;
    }
    av::pbr_set_joint_matrices(nullptr, 0);

    if (st.shadow_fbo) {
        glDeleteFramebuffers(1, &st.shadow_fbo);
        st.shadow_fbo = 0;
    }
    if (st.shadow_depth_tex) {
        glDeleteTextures(1, &st.shadow_depth_tex);
        st.shadow_depth_tex = 0;
    }
    st.shadow_fbo_w = st.shadow_fbo_h = 0;

    st.current_frame = 0.0f;
    st.anim_playing = false;
    st.anim_timer = 0.0f;

    st.scene = av::SceneData{};
    st.selected_object = -1;
    st.highlighted_mesh = -1;
    st.scene_mesh_edit = false;
    st.mesh_edit_object = -1;
    st.mesh_edit_mesh = -1;
    st.mesh_edit_vertex = -1;
    st.gm_edit_apply_object = -1;   // never apply a sketch to a new scene
    st.gm_inline_edit = false;      // never keep inline editing across scenes
    st.gm_inline_dirty = false;
    st.gm_inline_saved_valid = false;
    st.gm_drag_from_insert = false;
    st.gm_drag_off_x = st.gm_drag_off_y = 0.0;
    st.gm_inline_hover_vertex = st.gm_inline_hover_edge = -1;
    st.gm_hats.clear();
    st.gm_hat_dragging = st.gm_hat_resizing = false;
    st.gm_drag_hat = -1;
    st.mesh_edit_drag = false;
    st.gizmo_axis = -1;
    st.gizmo_drag = false;
    st.transform_axis = -1;

    for (auto& vec : st.scene_ground_gpu_meshes) {
        for (auto& m : vec) {
            av::free_mesh(m);
        }
    }
    st.scene_ground_gpu_meshes.clear();

    for (auto& vec : st.scene_ground_textures) {
        for (auto tex : vec) {
            if (tex) glDeleteTextures(1, &tex);
        }
    }
    st.scene_ground_textures.clear();
    st.scene_ground_tex_names.clear();

    for (auto& pair : st.scene_background_textures) {
        if (pair.second) glDeleteTextures(1, &pair.second);
    }
    st.scene_background_textures.clear();

    for (auto tex : st.scene_water_textures) {
        if (tex) glDeleteTextures(1, &tex);
    }
    st.scene_water_textures.clear();

    av::free_mesh(st.scene_proxy_mesh);

    // GM generator live-3D preview resources.
    for (auto& m : st.gm_preview_meshes) av::free_mesh(m);
    st.gm_preview_meshes.clear();
    for (auto t : st.gm_preview_textures) if (t) glDeleteTextures(1, &t);
    st.gm_preview_textures.clear();
    if (st.gm_preview_fbo) { av::delete_fbo(st.gm_preview_fbo, st.gm_preview_fbo_tex); st.gm_preview_fbo = 0; st.gm_preview_fbo_tex = 0; }
    st.gm_preview_valid = false;

    for (auto& pair : st.scene_gpu_mesh_cache) {
        for (auto& m : pair.second) {
            av::free_mesh(m);
        }
    }
    st.scene_gpu_mesh_cache.clear();

    for (auto& pair : st.scene_texture_cache) {
        for (auto tex : pair.second) {
            if (tex) glDeleteTextures(1, &tex);
        }
    }
    st.scene_texture_cache.clear();
    st.scene_model_cache.clear();
    st.scene_preview_tab = 1;
    st.scene_text_dirty = false;

    // Reset transformations
    st.texture_rotation = 0;
    st.texture_flip_h = true;
    st.texture_flip_v = true;
    st.texture_tint[0] = st.texture_tint[1] = st.texture_tint[2] = st.texture_tint[3] = 1.0f;
    st.model_rotate_x = 0.0f;
    st.model_rotate_y = 0.0f;
    st.model_rotate_z = 0.0f;
    st.model_scale = 1.0f;
    st.text_preview_content.clear();
    st.text_is_binary = false;

    st.preview_type = PREVIEW_NONE;
    st.status_msg.clear();
}

// ============================================================================
// File selection / loading
// ============================================================================

static GLuint load_tex_png(const std::string& path, int* out_w, int* out_h, std::string& format_str) {
    gzFile file = gzopen(path.c_str(), "rb");
    if (!file) return 0;

    uint32_t header[3];
    int read = gzread(file, header, 12);
    if (read != 12) {
        gzclose(file);
        return 0;
    }

    uint32_t img_type = header[0];
    uint32_t width = header[1];
    uint32_t height = header[2];

    *out_w = (int)width;
    *out_h = (int)height;

    std::string src_tag = ".tex";
    std::string low = path;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    if (low.size() >= 8 && low.substr(low.size() - 8) == ".tex.png") src_tag = ".tex.png";

    int bpp = 0;
    if (img_type == 1) {
        bpp = 4;
        format_str = "RGBA8888 (" + src_tag + ")";
    } else if (img_type == 3 || img_type == 5) {
        bpp = 2;
        format_str = (img_type == 3) ? "RGBA4444 (" + src_tag + ")" : "RGB565 (" + src_tag + ")";
    } else {
        gzclose(file);
        return 0;
    }

    std::vector<uint8_t> raw_data(width * height * bpp);
    int data_read = gzread(file, raw_data.data(), (unsigned int)raw_data.size());
    gzclose(file);

    if (data_read != (int)raw_data.size()) {
        return 0;
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);

    if (img_type == 1) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, raw_data.data());
    } else if (img_type == 3) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                     GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, raw_data.data());
    } else if (img_type == 5) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)width, (GLsizei)height, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, raw_data.data());
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    return tex_id;
}

// Flip a surface's rows in place. Standard images (IMG_Load) are stored
// top-down (v = 0 at the top, the DCC convention), but the whole pipeline
// uploads game containers (.tex.png/.pvr) bottom-origin (v = 0 at the bottom).
// Flipping standard images here makes EVERY uploaded texture bottom-origin, so
// a single UV convention (v = 0 bottom — the game's) works everywhere; the
// model shader's uFlipV then restores DCC orientation for FBX/glTF previews.
static void flip_surface_vertical(SDL_Surface* s) {
    if (!s || !s->pixels || s->h < 2) return;
    std::vector<uint8_t> row(static_cast<size_t>(s->pitch));
    for (int y = 0; y < s->h / 2; ++y) {
        uint8_t* top = (uint8_t*)s->pixels + (size_t)y * s->pitch;
        uint8_t* bot = (uint8_t*)s->pixels + (size_t)(s->h - 1 - y) * s->pitch;
        std::memcpy(row.data(), top, (size_t)s->pitch);
        std::memcpy(top, bot, (size_t)s->pitch);
        std::memcpy(bot, row.data(), (size_t)s->pitch);
    }
}

static GLuint load_texture_file(const std::string& path, int* out_w = nullptr, int* out_h = nullptr, std::string* out_format = nullptr) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    std::string low_path = path;
    for (auto& c : low_path) c = (char)tolower((unsigned char)c);

    bool is_tex_png = (low_path.size() >= 8 && low_path.substr(low_path.size() - 8) == ".tex.png");

    if (is_tex_png || ext == ".tex") {
        int w = 0, h = 0;
        std::string format;
        GLuint tex = load_tex_png(path, &w, &h, format);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_format) *out_format = format;
        return tex;
    }

    if (ext == ".pvr") {
        int w = 0, h = 0;
        GLuint tex = pvr_load_texture(path.c_str(), &w, &h);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_format) *out_format = "ETC1/PVRTC (PVR)";
        return tex;
    }

    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) return 0;

    SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!conv) return 0;

    // Normalize to bottom-origin (see flip_surface_vertical comment).
    flip_surface_vertical(conv);

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, conv->w, conv->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, conv->pixels);
    // Trilinear mipmapping for the cohesive, softer vanilla look.
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);

    if (out_w) *out_w = conv->w;
    if (out_h) *out_h = conv->h;
    if (out_format) *out_format = "RGBA8 (standard)";

    SDL_DestroySurface(conv);
    return tex_id;
}

static void try_load_matching_texture(ViewerState& st, const std::string& pod_path) {
    fs::path p(pod_path);
    std::string stem = p.stem().string();
    fs::path dir = p.parent_path();

    const char* suffixes[] = {"_2x.tex.png", ".tex.png", "_2x.pvr", ".pvr", "_2x.tex", ".tex", "_2x.png", ".png"};
    for (auto& suf : suffixes) {
        fs::path candidate = dir / (stem + suf);
        if (fs::exists(candidate)) {
            st.model_texture = load_texture_file(candidate.string());
            if (st.model_texture) return;
        }
    }
}

// Load textures for st.model.texture_filenames. Embedded glTF spills and
// vendor-.scn resolved paths win via the model_gltf_alias override; otherwise
// the model dir + the usual tex.png/pvr candidates are searched.
static void resolve_model_textures(ViewerState& st, const std::string& model_path) {
    st.model_textures.clear();
    st.missing_textures.clear();
    fs::path model_dir = fs::path(model_path).parent_path();

    for (const auto& tex_name : st.model.texture_filenames) {
        if (tex_name.empty()) {
            st.model_textures.push_back(0);
            continue;
        }

        fs::path tex_path = model_dir / tex_name;
        auto alias_it = st.model_gltf_alias.find(tex_name);
        if (alias_it != st.model_gltf_alias.end()) tex_path = alias_it->second;
        std::string stem = tex_path.stem().string();

        // Candidates list (original name, tex.png, PVR, tex, PNG)
        std::vector<fs::path> candidates = {
            tex_path,
            model_dir / (stem + "_2x.tex.png"),
            model_dir / (stem + ".tex.png"),
            model_dir / (stem + "_2x.pvr"),
            model_dir / (stem + ".pvr"),
            model_dir / (stem + "_2x.tex"),
            model_dir / (stem + ".tex"),
            model_dir / (stem + "_2x.png"),
            model_dir / (stem + ".png")
        };

        GLuint tex_id = 0;
        bool found = false;
        for (const auto& cand : candidates) {
            if (fs::exists(cand)) {
                tex_id = load_texture_file(cand.string());
                if (tex_id) {
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            st.model_textures.push_back(tex_id);
        } else {
            st.model_textures.push_back(0);
            st.missing_textures.push_back(tex_name);
        }
    }

    // Fallback to legacy matched texture search if no textures are found in model metadata
    if (st.model_textures.empty() || (st.model_textures.size() == 1 && st.model_textures[0] == 0)) {
        try_load_matching_texture(st, model_path);
        if (st.model_texture) {
            if (st.model_textures.empty()) st.model_textures.push_back(st.model_texture);
            else st.model_textures[0] = st.model_texture;
        }
    }
}

// Build a single-channel (grayscale) GL texture from one RGB channel of an
// image. glTF packs metallic-roughness (B/G) and occlusion (R) into shared
// images; the vendored PBR shaders sample separate .r maps.
static GLuint load_single_channel_texture(const std::string& path, int ch) {
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) return 0;
    SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!conv) return 0;
    std::vector<uint8_t> gray((size_t)conv->w * (size_t)conv->h);
    const uint8_t* px = (const uint8_t*)conv->pixels;
    for (int i = 0; i < conv->w * conv->h; ++i) gray[(size_t)i] = px[(size_t)i * 4 + ch];
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, conv->w, conv->h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, gray.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);
    SDL_DestroySurface(conv);
    return tex;
}

// Open a zauonlok/renderer .scn text scene into the model preview. Builds an
// aggregate PODModel (one mesh + node per scene model), per-node PBR/blinn
// materials, per-node .ani skeletons, and resolves textures against the .scn.
static bool open_vendor_scn(ViewerState& st, const std::string& path, const std::string& name) {
    av::ScnScene scn;
    std::string err;
    fs::path base = fs::path(path).parent_path();
    if (!av::scn_load(path, base.string(), scn, &err)) {
        st.status_msg = "Failed to load " + name + (err.empty() ? "" : " — " + err);
        log_file_event("ModelRead", "ERROR: vendor .scn parse failed: " + err);
        st.preview_type = PREVIEW_NONE;
        return false;
    }
    st.vendor_scn = std::move(scn);
    st.preview_type = PREVIEW_MODEL;
    st.vendor_mode = true;
    st.model = av::PODModel{};
    st.vendor_skeletons.clear();
    st.vendor_skel_valid.clear();
    st.vendor_pbr_mats.clear();
    st.vendor_pbr_valid.clear();
    st.model_textures.clear();
    st.missing_textures.clear();
    st.model_temp_files.clear();
    st.model_gltf_alias.clear();
    st.scene = av::SceneData{};
    av::pbr_set_joint_matrices(nullptr, 0);

    const av::ScnScene& sc = st.vendor_scn;

    std::vector<std::string> tex_names;
    auto tex_index_for = [&](const std::string& resolved) -> int {
        if (resolved.empty()) return -1;
        for (size_t k = 0; k < tex_names.size(); ++k)
            if (tex_names[k] == resolved) return (int)k;
        std::string fname = fs::path(resolved).filename().string();
        tex_names.push_back(fname);
        st.model_gltf_alias[fname] = resolved;
        return (int)tex_names.size() - 1;
    };

    int node_idx = 0;
    for (const auto& sm : sc.models) {
        if (sm.mesh_res.empty()) continue;
        av::PODModel sub;
        if (!av::obj_load(sm.mesh_res, sub)) continue;
        if (sub.meshes.empty()) continue;
        int mesh_base = (int)st.model.meshes.size();
        st.model.meshes.push_back(std::move(sub.meshes[0]));

        av::PODNode node;
        node.name = "scn_model_" + std::to_string(node_idx);
        node.object_index = mesh_base;
        node.parent_index = -1;
        node.material_index = sm.material;
        if (sm.transform >= 0 && sm.transform < (int)sc.transforms.size()) {
            node.has_matrix = true;
            std::memcpy(node.matrix, sc.transforms[sm.transform].m, sizeof(float) * 16);
        }
        st.model.nodes.push_back(std::move(node));

        av::AniSkeleton ani;
        bool has_ani = false;
        if (!sm.skeleton_res.empty() && av::ani_load(sm.skeleton_res, ani)) has_ani = true;
        st.vendor_skeletons.push_back(has_ani ? std::move(ani) : av::AniSkeleton{});
        st.vendor_skel_valid.push_back(has_ani);
        if (has_ani) st.pbr_preview = true;   // skinned vendor meshes need the PBR path

        av::PBRMaterial pmat;
        bool pbr_ok = false;
        if (sm.material >= 0 && sm.material < (int)sc.materials.size()) {
            const auto& smat = sc.materials[sm.material];
            std::memcpy(pmat.base_color, smat.base_color, sizeof(float) * 4);
            if (smat.kind == 1) {                     // pbrm (metallic-roughness)
                pmat.metalness = smat.metalness;
                pmat.roughness = smat.roughness;
                pbr_ok = true;
            } else if (smat.kind == 2) {              // pbrs (specular-glossiness)
                pmat.workflow = 1;
                std::memcpy(pmat.specular, smat.specular, sizeof(float) * 3);
                pmat.glossiness = smat.glossiness;
                pbr_ok = true;
            }
        }
        st.vendor_pbr_mats.push_back(pmat);
        st.vendor_pbr_valid.push_back(pbr_ok);
        ++node_idx;
    }

    if (st.model.meshes.empty()) {
        st.status_msg = "Failed to load " + name + " — no loadable meshes";
        st.preview_type = PREVIEW_NONE;
        return false;
    }

    st.model.materials.clear();
    for (size_t mi = 0; mi < sc.materials.size(); ++mi) {
        const auto& smat = sc.materials[mi];
        av::PODMaterial pm;
        pm.name = "scn_mat_" + std::to_string(mi);
        pm.diffuse[0] = smat.base_color[0];
        pm.diffuse[1] = smat.base_color[1];
        pm.diffuse[2] = smat.base_color[2];
        pm.opacity = smat.base_color[3];
        const std::string& diff_res = (smat.kind == 0) ? smat.diffuse_res : smat.basecolor_res;
        pm.diffuse_texture_index = tex_index_for(diff_res);
        st.model.materials.push_back(std::move(pm));
    }
    st.model.texture_filenames = tex_names;

    float minx = 1e9f, miny = 1e9f, minz = 1e9f;
    float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
    for (const auto& msh : st.model.meshes) {
        minx = std::min(minx, msh.min_x); miny = std::min(miny, msh.min_y); minz = std::min(minz, msh.min_z);
        maxx = std::max(maxx, msh.max_x); maxy = std::max(maxy, msh.max_y); maxz = std::max(maxz, msh.max_z);
        st.model.total_vertices += msh.num_vertices;
        st.model.total_faces += msh.num_faces;
    }
    st.model.min_x = minx; st.model.min_y = miny; st.model.min_z = minz;
    st.model.max_x = maxx; st.model.max_y = maxy; st.model.max_z = maxz;
    st.model.center_x = (minx + maxx) * 0.5f;
    st.model.center_y = (miny + maxy) * 0.5f;
    st.model.center_z = (minz + maxz) * 0.5f;
    float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
    st.model.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (st.model.radius < 1.0f) st.model.radius = 1.0f;
    st.model.num_mesh_nodes = (int)st.model.nodes.size();

    st.camera = av::Camera{};
    st.camera.target[0] = st.model.center_x;
    st.camera.target[1] = st.model.center_y;
    st.camera.target[2] = st.model.center_z;
    st.camera.distance = st.model.radius * 2.5f;
    if (st.camera.distance < 1.0f) st.camera.distance = 3.0f;
    st.camera.yaw = 30.0f;
    st.camera.pitch = 18.0f;
    st.model_auto_rotate = false;
    st.vendor_anim_time = 0.0f;

    // Vendor .scn lighting — matches the reference renderer's semantics
    // (shaders/pbr_shader.c): `ambient` scales the IBL/env contribution and
    // `punctual` scales the directional sun. Adopt the scene's background as
    // the viewport clear color and its shadow switch too; everything is saved
    // and restored in free_preview_resources.
    if (!st.vendor_bg_saved) {
        std::memcpy(st.vendor_prev_bg, st.bg_color, sizeof(float) * 3);
        st.vendor_bg_saved = true;
    }
    st.bg_color[0] = sc.background[0];
    st.bg_color[1] = sc.background[1];
    st.bg_color[2] = sc.background[2];

    if (!st.vendor_light_saved) {
        st.vendor_prev_light_scale = av::g_light_scale;
        st.vendor_prev_ambient_scale = av::g_ambient_scale;
        st.vendor_prev_pbr_shadows = st.pbr_shadows;
        st.vendor_light_saved = true;
    }
    // Note: env intensity is deliberately NOT part of this save/restore — the
    // render pass re-issues pbr_set_env_intensity every frame from the user's
    // slider (× scene ambient), so it self-heals when the preview closes.
    av::g_light_scale   = sc.punctual >= 0.0f ? sc.punctual : 1.0f;
    av::g_ambient_scale = sc.ambient  >= 0.0f ? sc.ambient  : 1.0f;
    // Faithful mapping: `shadow: off` disables shadows even if the user had
    // them on (the previous toggle is saved above and restored on close).
    st.pbr_shadows = (sc.shadow != "off");

    // Diffuse textures (aliases already registered) + PBR map textures.
    resolve_model_textures(st, path);
    auto load_map = [&](const std::string& p, bool single_channel, int ch) -> GLuint {
        if (p.empty()) return 0;
        GLuint t = single_channel ? load_single_channel_texture(p, ch) : load_texture_file(p);
        if (t) st.vendor_owned_textures.push_back(t);
        return t;
    };
    for (size_t i = 0; i < st.model.nodes.size(); ++i) {
        int mat_idx = st.model.nodes[i].material_index;
        if (mat_idx < 0 || mat_idx >= (int)sc.materials.size()) continue;
        if (!st.vendor_pbr_valid[i]) continue;
        const auto& smat = sc.materials[mat_idx];
        auto& pm = st.vendor_pbr_mats[i];
        if (smat.kind == 1) {
            pm.metalness_tex = load_map(smat.metalness_res, true, 0);
            pm.roughness_tex = load_map(smat.roughness_res, true, 0);
            pm.normal_tex = load_map(smat.normal_res, false, 0);
            pm.occlusion_tex = load_map(smat.occlusion_res, true, 0);
            pm.emission_tex = load_map(smat.emission_res, false, 0);
        } else if (smat.kind == 2) {
            pm.specular_tex = load_map(smat.specular_res, false, 0);
            pm.glossiness_tex = load_map(smat.glossiness_res, true, 0);
            pm.normal_tex = load_map(smat.normal_res, false, 0);
            pm.occlusion_tex = load_map(smat.occlusion_res, true, 0);
            pm.emission_tex = load_map(smat.emission_res, false, 0);
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Loaded %s — vendor .scn (%zu model(s), %zu mesh(es))",
             name.c_str(), sc.models.size(), st.model.meshes.size());
    st.status_msg = buf;
    log_file_event("ModelRead", "Loaded vendor scene: " + path);
    return true;
}

static void select_file(ViewerState& st, const FileEntry& fe) {
    if (fe.is_dir) {
        st.current_dir = fe.full_path;
        refresh_directory(st);
        apply_filters(st);
        return;
    }

    free_preview_resources(st);
    st.sel_name = fe.name;
    st.sel_path = fe.full_path;
    st.sel_size = fe.size;

    switch (fe.type) {
    case FTYPE_TEXTURE: {
        st.preview_type = PREVIEW_TEXTURE;
        st.tex_reset_required = true;
        st.preview_tex = load_texture_file(fe.full_path, &st.tex_w, &st.tex_h, &st.tex_format_str);
        tex_editor_load(st, fe.full_path);   // CPU-side RGBA buffer for editing
        if (st.preview_tex) {
            st.status_msg = "Loaded " + fe.name + " — " + std::to_string(st.tex_w) + "x" + std::to_string(st.tex_h);
            log_file_event("TextureRead", "Loaded texture: " + fe.name + " (" + std::to_string(st.tex_w) + "x" + std::to_string(st.tex_h) + ", " + format_size(fe.size) + ")");
        } else {
            st.status_msg = "Failed to load " + fe.name;
            log_file_event("TextureRead", "ERROR: Failed to load texture: " + fe.name);
        }
    } break;

    case FTYPE_MODEL: {
        st.preview_type = PREVIEW_MODEL;
        std::string ext = fs::path(fe.full_path).extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);

        st.model_temp_files.clear();
        st.model_gltf_alias.clear();

        // Reset vendored-format state from any previous preview.
        for (auto tex : st.vendor_owned_textures) {
            if (tex) glDeleteTextures(1, &tex);
        }
        st.vendor_owned_textures.clear();
        st.gltf_pbr_materials.clear();
        st.gltf_pbr_img_paths.clear();
        st.vendor_mode = false;
        st.vendor_anim_time = 0.0f;
        st.vendor_anim_playing = true;
        st.vendor_skeletons.clear();
        st.vendor_skel_valid.clear();
        st.vendor_pbr_mats.clear();
        st.vendor_pbr_valid.clear();
        st.vendor_scn = av::ScnScene{};
        av::pbr_set_joint_matrices(nullptr, 0);

        if (ext == ".fbx") {
            st.model = av::fbx_load(fe.full_path);
        } else if (ext == ".glb" || ext == ".gltf") {
            av::PODModel m;
            std::vector<av::GLTFImageBuffer> imgs;
            std::string msg;
            av::GLTFPBRInfo pbr;
            const bool gltf_ok = (ext == ".glb")
                ? av::gltf_import_glb(fe.full_path, m, imgs, &msg, &pbr)
                : av::gltf_import_gltf(fe.full_path, m, imgs, &msg, &pbr);
            if (gltf_ok) {
                st.model = std::move(m);
                st.gltf_pbr_materials.clear();
                st.gltf_pbr_img_paths.clear();
                st.vendor_owned_textures.clear();
                // Spill PBR-referenced images (keyed by glTF image index) and
                // build the per-material GL textures for the vendored PBR path.
                // glTF's metallicRoughnessTexture packs metal (B) + roughness
                // (G) into one image — swizzled into two single-channel maps.
                if (!pbr.image_gltf_index.empty()) {
                    size_t max_img = 0;
                    for (int gi : pbr.image_gltf_index) max_img = std::max(max_img, (size_t)gi);
                    st.gltf_pbr_img_paths.assign(max_img + 1, "");
                }
                static int s_pbr_gltf_seq = 0;
                for (size_t pi = 0; pi < pbr.images.size(); ++pi) {
                    if (pbr.images[pi].data.empty()) continue;
                    int gimg = pi < pbr.image_gltf_index.size() ? pbr.image_gltf_index[pi] : -1;
                    if (gimg < 0 || gimg >= (int)st.gltf_pbr_img_paths.size()) continue;
                    std::string img_ext =
                        (pbr.images[pi].mime.find("jpeg") != std::string::npos) ? ".jpg" : ".png";
                    std::string tmp = (fs::temp_directory_path() /
                        ("ruby_gltfpbr_" + std::to_string(getpid()) + "_" +
                         std::to_string(s_pbr_gltf_seq++) + img_ext)).string();
                    { std::ofstream f(tmp, std::ios::binary);
                      if (f) f.write((const char*)pbr.images[pi].data.data(),
                                     (std::streamsize)pbr.images[pi].data.size()); }
                    st.gltf_pbr_img_paths[gimg] = tmp;
                    st.model_temp_files.push_back(tmp);
                }
                auto load_slot = [&](int img_idx) -> GLuint {
                    if (img_idx < 0 || img_idx >= (int)st.gltf_pbr_img_paths.size()) return 0;
                    const std::string& p = st.gltf_pbr_img_paths[img_idx];
                    if (p.empty()) return 0;
                    GLuint t = load_texture_file(p);
                    if (t) st.vendor_owned_textures.push_back(t);
                    return t;
                };
                for (size_t mi = 0; mi < pbr.materials.size(); ++mi) {
                    const auto& pm = pbr.materials[mi];
                    av::PBRMaterial mat;
                    std::memcpy(mat.base_color, pm.base_color, sizeof(float) * 4);
                    mat.metalness = pm.metallic;
                    mat.roughness = pm.roughness;
                    mat.occlusion = pm.occlusion;
                    std::memcpy(mat.emission, pm.emissive, sizeof(float) * 3);
                    mat.normal_tex = load_slot(pm.normal_tex);
                    mat.emission_tex = load_slot(pm.emissive_tex);
                    const int mr_idx = pm.metalrough_tex;
                    if (mr_idx >= 0 && mr_idx < (int)st.gltf_pbr_img_paths.size() &&
                        !st.gltf_pbr_img_paths[mr_idx].empty()) {
                        mat.metalness_tex = load_single_channel_texture(st.gltf_pbr_img_paths[mr_idx], 2);
                        mat.roughness_tex = load_single_channel_texture(st.gltf_pbr_img_paths[mr_idx], 1);
                        if (mat.metalness_tex) st.vendor_owned_textures.push_back(mat.metalness_tex);
                        if (mat.roughness_tex) st.vendor_owned_textures.push_back(mat.roughness_tex);
                    }
                    const int oc_idx = pm.occl_tex;
                    if (oc_idx >= 0 && oc_idx < (int)st.gltf_pbr_img_paths.size() &&
                        !st.gltf_pbr_img_paths[oc_idx].empty()) {
                        mat.occlusion_tex = load_single_channel_texture(st.gltf_pbr_img_paths[oc_idx], 0);
                        if (mat.occlusion_tex) st.vendor_owned_textures.push_back(mat.occlusion_tex);
                    }
                    st.gltf_pbr_materials.push_back(std::move(mat));
                }
                // Spill embedded textures (PNG/JPEG) to temp files so the
                // standard texture loader can upload them; remember the name
                // → temp-path mapping for the candidate search below.
                fs::path tmpdir = fs::temp_directory_path();
                static int s_gltf_seq = 0;
                for (size_t i = 0; i < imgs.size(); ++i) {
                    if (imgs[i].data.empty()) continue;
                    std::string img_ext =
                        (imgs[i].mime.find("jpeg") != std::string::npos) ? ".jpg" : ".png";
                    std::string tmp = (tmpdir /
                        ("ruby_gltf_" + std::to_string(getpid()) + "_" +
                         std::to_string(s_gltf_seq++) + img_ext)).string();
                    { std::ofstream f(tmp, std::ios::binary);
                      if (f) f.write((const char*)imgs[i].data.data(),
                                     (std::streamsize)imgs[i].data.size()); }
                    if (i < st.model.texture_filenames.size())
                        st.model_gltf_alias[st.model.texture_filenames[i]] = tmp;
                    st.model_temp_files.push_back(tmp);
                }
            } else {
                st.status_msg = "Failed to load " + fe.name +
                                (msg.empty() ? "" : " — " + msg);
                log_file_event("ModelRead", "ERROR: glTF import failed: " + msg);
                st.preview_type = PREVIEW_NONE;
                break;
            }
        } else if (ext == ".obj") {
            std::string msg;
            if (!av::obj_load(fe.full_path, st.model, &msg)) {
                st.status_msg = "Failed to load " + fe.name + (msg.empty() ? "" : " — " + msg);
                log_file_event("ModelRead", "ERROR: OBJ import failed: " + msg);
                st.preview_type = PREVIEW_NONE;
                break;
            }
            st.vendor_mode = true;
            // Auto-pair a same-stem .ani skeleton (renderer-master convention).
            fs::path ani_path = fs::path(fe.full_path);
            ani_path.replace_extension(".ani");
            if (fs::exists(ani_path)) {
                av::AniSkeleton ani;
                if (av::ani_load(ani_path.string(), ani)) {
                    st.vendor_skeletons.assign(st.model.nodes.size(), av::AniSkeleton{});
                    st.vendor_skel_valid.assign(st.model.nodes.size(), false);
                    if (!st.vendor_skeletons.empty()) {
                        st.vendor_skeletons[0] = std::move(ani);
                        st.vendor_skel_valid[0] = true;
                        st.pbr_preview = true;   // skinned OBJ needs the PBR path
                    }
                }
            }
        } else {
            st.model = av::pod_load(fe.full_path);
        }

        // DCC formats (FBX/glTF) author UVs with v = 0 at the TOP; POD uses
        // the game's bottom-origin convention. Textures are uploaded
        // bottom-origin everywhere, so the model shader flips V for DCC
        // meshes — keeping FBX/glTF textures upright against their source
        // program (the old FBX path sampled flipped .tex.png containers with
        // top-origin UVs and showed textures upside-down). Exception: when
        // ufbx mirrored the FBX's handedness (left-handed sources) it already
        // flipped V to v = 0 at the bottom — no further flip needed.
        const bool dcc_uv = (ext == ".glb") || (ext == ".gltf") || (ext == ".obj") ||
                            (ext == ".fbx" && !st.model.uv_v_flipped);

        if (st.model.meshes.empty()) {
            st.status_msg = "Failed to load " + fe.name;
            log_file_event("ModelRead", "ERROR: Failed to load model: " + fe.name);
            st.preview_type = PREVIEW_NONE;
            break;
        }
        st.anim_fps = st.model.fps;

        const bool vendor_skinned = st.vendor_mode;   // obj/scn skin via GPU attributes
        for (auto& mesh : st.model.meshes) {
            const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
            const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
            const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
            const float*    tan = mesh.tangents.empty()   ? nullptr : mesh.tangents.data();
            const float*    jnt = (vendor_skinned && !mesh.bone_indices.empty()) ? mesh.bone_indices.data() : nullptr;
            const float*    wgt = (vendor_skinned && !mesh.bone_weights.empty()) ? mesh.bone_weights.data() : nullptr;
            const uint32_t* idx = mesh.indices.empty()    ? nullptr : mesh.indices.data();
            av::GPUMesh gm = av::upload_mesh_ex(pos, nrm, uv, tan, jnt, wgt,
                                                mesh.num_vertices,
                                                idx, (int)mesh.indices.size(), dcc_uv);
            st.gpu_meshes.push_back(gm);
        }

        st.camera = av::Camera{};
        st.camera.target[0] = st.model.center_x;
        st.camera.target[1] = st.model.center_y;
        st.camera.target[2] = st.model.center_z;
        st.camera.distance  = st.model.radius * 2.5f;
        if (st.camera.distance < 1.0f) st.camera.distance = 3.0f;
        st.camera.yaw   = 30.0f;   // professional 3/4 preview angle
        st.camera.pitch = 18.0f;
        st.model_auto_rotate = false;

        // Auto search dependencies of the POD model (aliases win for glTF
        // spills and vendor-.scn resolved paths; .tga loads via SDL_image).
        resolve_model_textures(st, fe.full_path);

        char buf[256];
        snprintf(buf, sizeof(buf), "Loaded %s — %d mesh(es), %d verts, %d faces",
                 fe.name.c_str(), (int)st.model.meshes.size(),
                 st.model.total_vertices, st.model.total_faces);
        st.status_msg = buf;
        log_file_event("ModelRead", "Loaded model: " + fe.name + " (" + std::to_string(st.model.meshes.size()) + " mesh(es), " + std::to_string(st.model.total_vertices) + " vertices, " + format_size(fe.size) + ")");
    } break;

    case FTYPE_SCENE: {
        // Vendor .scn (renderer-master text scene)? Route to the model preview.
        {
            std::ifstream peek_file(fe.full_path, std::ios::binary);
            char head[64] = {0};
            if (peek_file && peek_file.read(head, 63)) {
                std::string h(head);
                if (h.rfind("type:", 0) == 0 || h.find("type: ") != std::string::npos) {
                    open_vendor_scn(st, fe.full_path, fe.name);
                    break;
                }
            }
        }
        st.preview_type = PREVIEW_SCENE;
        st.scene_loading = true;          // loading screen (see main frame)
        st.scene_loading_frames = 3.0f;
        st.scene_loading_msg = fe.name;
        st.scene = av::scene_load(fe.full_path);
        // Reset scene editor state
        st.selected_object     = -1;
        st.scene_selection.clear();
        st.mesh_edit_tool = 0;
        st.mesh_edit_triangle = -1;
        st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
        st.scene_dirty         = false;
        st.scene_save_msg.clear();
        st.scene_save_msg_timer = 0.0f;
        st.scene_onload_buf[0] = '\0';
        st.scene_onload_modified = false;
        st.scene_obj_name_buf[0]     = '\0';
        st.scene_obj_template_buf[0] = '\0';
        st.scene_script_index = -1;
        st.scene_script_buf[0] = '\0';
        st.scene_undo_stack.clear();
        st.scene_redo_stack.clear();

        if (st.scene.objects.empty()) {
            st.status_msg = "Failed to parse " + fe.name;
            log_file_event("SceneRead", "ERROR: Failed to parse scene file: " + fe.name);
        } else {
            st.scene_model_cache.clear();
            st.scene_gpu_mesh_cache.clear();
            st.scene_texture_cache.clear();
            st.scene_preview_tab = 1; // Scenes open directly in the visual editor.
            if (const char* itab = getenv("RUBY_INIT_TAB")) {
                int v = atoi(itab);
                if (v >= 0 && v <= 3) st.scene_preview_tab = v;   // dev/QA override
            }
            if (const char* isel = getenv("RUBY_SEL_OBJ")) {      // dev/QA: auto-select
                int v = atoi(isel);
                if (v >= 0 && v < (int)st.scene.objects.size()) {
                    select_scene_object(st, v);
                    st.selected_object = v;
                }
            }
            if (const char* iplay = getenv("RUBY_PLAY_MODE")) {   // dev/QA: auto-start player
                const int v = atoi(iplay);
                if (v == 1 || v == 2) {
                    sp::player_begin(st.scene_player, st.scene,
                                     v == 1 ? sp::Mode::Visualise : sp::Mode::PlayHiro,
                                     fs::path(st.scene.filepath).parent_path().string());
                    st.scene_player_window_open = true;
                    st.scene_anim_playing = true;
                }
            }
            st.scene_text_dirty = false;

            // Read scene binary bytes and decode to text markup
            {
                std::ifstream f_in(fe.full_path, std::ios::binary);
                if (f_in) {
                    std::string bytes((std::istreambuf_iterator<char>(f_in)), std::istreambuf_iterator<char>());
                    st.text_preview_content = filerift::decode_protobuf(bytes, "scene");
                    st.text_edit_buffer = st.text_preview_content;
                    st.text_edit_modified = false;
                }
            }

            fs::path scene_dir = fs::path(fe.full_path).parent_path();
            for (const auto& obj : st.scene.objects) {
                if (!obj.mesh_name.empty())
                    load_scene_model_to_cache(st, obj.mesh_name, scene_dir.string());
                if (!obj.background_name.empty())
                    load_scene_background_texture(st, obj.background_name, scene_dir.string());
            }
            fprintf(stderr, "[RubyDebug] scene model cache size after load: %zu\n", st.scene_model_cache.size());
            upload_scene_ground_meshes(st, scene_dir.string());
            upload_scene_waters(st, scene_dir.string());

            st.camera = av::Camera{};
            frame_scene_at_spawn(st, -1);  // camera port: spawn_default preferred

            size_t resolved_models = 0;
            size_t ground_meshes = 0;
            size_t inherited_objects = 0;
            for (const auto& object : st.scene.objects) {
                if (!object.mesh_name.empty() || !object.background_name.empty()) ++resolved_models;
                ground_meshes += object.ground_meshes.size();
                if (object.components.empty() && !object.resolved_components.empty()) ++inherited_objects;
            }

            char buf[256];
            snprintf(buf, sizeof(buf), "Loaded %s - %d objects, %zu models, %zu ground meshes",
                     fe.name.c_str(), (int)st.scene.objects.size(), resolved_models, ground_meshes);
            st.status_msg = buf;
            log_file_event("SceneRead", "Loaded scene: " + fe.name + " (" + std::to_string(st.scene.objects.size()) + " objects, " + format_size(fe.size) + ")");
            log_file_event("SceneRender", "Resolved " + std::to_string(resolved_models) +
                " model objects, " + std::to_string(ground_meshes) + " ground meshes, " +
                std::to_string(inherited_objects) + " inherited template objects");
        }
    } break;


    case FTYPE_AUDIO: {
        st.preview_type = PREVIEW_AUDIO;
        if (av::audio_load(fe.full_path)) {
            auto& as = av::audio_get_state();
            char buf[256];
            snprintf(buf, sizeof(buf), "Loaded %s — %s, %d Hz, %d-bit",
                     fe.name.c_str(), format_time(as.duration).c_str(),
                     as.sample_rate, as.bits_per_sample);
            st.status_msg = buf;
            log_file_event("AudioRead", "Loaded audio: " + fe.name + " (" + format_time(as.duration) + ", " + std::to_string(as.sample_rate) + " Hz, " + format_size(fe.size) + ")");
        } else {
            st.status_msg = "Failed to load " + fe.name;
            st.preview_type = PREVIEW_NONE;
            log_file_event("AudioRead", "ERROR: Failed to load audio: " + fe.name);
        }
    } break;

    case FTYPE_TEXT:
    default: {
        st.preview_type = PREVIEW_TEXT;
        st.text_preview_content.clear();
        st.text_is_binary = false;

        auto dot = fe.full_path.rfind('.');
        std::string ext = (dot != std::string::npos) ? fe.full_path.substr(dot) : "";
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        bool is_protobuf = (ext == ".scl" || ext == ".gdata" || ext == ".gstate" || 
                            ext == ".gplayer" || ext == ".gopt" || ext == ".sounds" || 
                            ext == ".scmap" || ext == ".atlas" || ext == ".fnt");
        if (is_protobuf) {
            std::ifstream f(fe.full_path, std::ios::binary);
            if (f) {
                std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                f.close();
                st.text_preview_content = filerift::decode_protobuf(bytes, ext.substr(1));
                st.status_msg = "Decoded " + fe.name + " via FileRift";
                log_file_event("FileDecode", "Decoded binary Protobuf to markup: " + fe.name + " (" + format_size(fe.size) + ")");
            } else {
                st.text_preview_content = "Failed to open protobuf file.";
                st.status_msg = "Error reading " + fe.name;
                log_file_event("FileDecode", "ERROR: Failed to read protobuf file: " + fe.name);
            }
            break;
        }

        std::ifstream f(fe.full_path, std::ios::binary);
        if (f) {
            std::vector<char> buffer((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (buffer.empty()) {
                st.text_preview_content = "(empty file)";
                st.status_msg = "Opened " + fe.name + " (Empty)";
                log_file_event("FileRead", "Opened empty text file: " + fe.name);
            } else {
                // Check if binary (null bytes or control chars in first 1024 bytes)
                size_t check_len = std::min(buffer.size(), (size_t)1024);
                for (size_t i = 0; i < check_len; ++i) {
                    if (buffer[i] == '\0') {
                        st.text_is_binary = true;
                        break;
                    }
                }

                if (st.text_is_binary) {
                    // Generate formatted hex dump
                    std::stringstream ss;
                    for (size_t i = 0; i < buffer.size(); i += 16) {
                        char addr_buf[16];
                        snprintf(addr_buf, sizeof(addr_buf), "%08X  ", (unsigned int)i);
                        ss << addr_buf;

                        // Hex values
                        for (size_t j = 0; j < 16; ++j) {
                            if (i + j < buffer.size()) {
                                char hex_buf[8];
                                snprintf(hex_buf, sizeof(hex_buf), "%02X ", (unsigned char)buffer[i+j]);
                                ss << hex_buf;
                            } else {
                                ss << "   ";
                            }
                            if (j == 7) ss << " "; // spacing between octets
                        }
                        ss << " |";

                        // ASCII values
                        for (size_t j = 0; j < 16; ++j) {
                            if (i + j < buffer.size()) {
                                char c = buffer[i+j];
                                ss << ((c >= 32 && c <= 126) ? c : '.');
                            }
                        }
                        ss << "|\n";

                        if (ss.str().size() > 256 * 1024) { // limit size for performance
                            ss << "... (truncated: file too large for Hex View) ...\n";
                            break;
                        }
                    }
                    st.text_preview_content = ss.str();
                    st.status_msg = "Opened " + fe.name + " (Hex View)";
                    log_file_event("FileRead", "Opened binary file in hex viewer: " + fe.name + " (" + format_size(fe.size) + ")");
                } else {
                    // Text file view
                    // Limit text content size for safety
                    if (buffer.size() > 512 * 1024) {
                        st.text_preview_content = std::string(buffer.begin(), buffer.begin() + 512 * 1024) + "\n... (truncated: file too large) ...\n";
                    } else {
                        st.text_preview_content = std::string(buffer.begin(), buffer.end());
                    }
                    st.status_msg = "Opened " + fe.name + " (Text View)";
                    log_file_event("FileRead", "Opened text file: " + fe.name + " (" + format_size(fe.size) + ")");
                }
            }
        } else {
            if (fe.is_dir) {
                st.preview_type = PREVIEW_NONE;
            } else {
                st.preview_type = PREVIEW_NONE;
                st.status_msg = "Failed to load " + fe.name;
                log_file_event("FileRead", "ERROR: Failed to open file: " + fe.name);
            }
        }
    } break;
    }
}

// ============================================================================
// UI Path navigation breadcrumbs
// ============================================================================

static void draw_breadcrumbs(ViewerState& st) {
    fs::path p(st.current_dir);
    std::vector<fs::path> parts;
    
    std::error_code ec;
    // Walk up the path hierarchy to parse folders
    while (!p.empty() && p != p.root_path()) {
        parts.push_back(p);
        p = p.parent_path();
    }
    parts.push_back(p.root_path());
    std::reverse(parts.begin(), parts.end());

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    
    // Quick shortcut button to local resources
    if (ImGui::SmallButton(ICON_FA_FOLDER " Vanilla")) {
        st.current_dir = expand_home("~/.local/share/swordigo-desktop/assets/resources/");
        if (!fs::is_directory(st.current_dir, ec)) {
            const char* home = getenv("HOME");
            st.current_dir = home ? home : "/";
        }
        refresh_directory(st);
        apply_filters(st);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
    ImGui::SameLine();

    // Collapse the OS/install prefix (/home/user/.local/share/swordigo-desktop)
    // into a single leading "…" so the breadcrumb reads as a logical path
    // (e.g. assets > resources > subdir) instead of exposing the install layout.
    size_t hidden_prefix = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string pname = parts[i].filename().string();
        if (pname == "assets" || pname == "resources") { hidden_prefix = i; break; }
    }

    float remaining = ImGui::GetContentRegionAvail().x;
    size_t first_visible = hidden_prefix;
    float required = 0.0f;
    for (size_t i = first_visible; i < parts.size(); ++i) {
        std::string name = parts[i].filename().string();
        if (name.empty()) name = "/";
        required += ImGui::CalcTextSize(name.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f;
    }
    while (first_visible + 1 < parts.size() && required > remaining) {
        std::string name = parts[first_visible].filename().string();
        if (name.empty()) name = "/";
        required -= ImGui::CalcTextSize(name.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f;
        ++first_visible;
    }
    if (first_visible > 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::SmallButton(ICON_FA_LIST)) {
            // Jump straight to the first visible segment's parent listing.
            st.current_dir = parts[first_visible].string();
            refresh_directory(st);
            apply_filters(st);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hidden path prefix (click to jump)\n%s", parts[first_visible - 1].string().c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
        ImGui::SameLine();
    }

    for (size_t i = first_visible; i < parts.size(); i++) {
        std::string name = parts[i].filename().string();
        if (name.empty()) {
            name = "/";
        }
        if (i > first_visible) {
            ImGui::SameLine();
            ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
            ImGui::SameLine();
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0)); // transparent flat button
        if (ImGui::SmallButton(name.c_str())) {
            st.current_dir = parts[i].string();
            refresh_directory(st);
            apply_filters(st);
            ImGui::PopStyleColor();
            break;
        }
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar();
}

// ============================================================================
// UI: File browser panel (left)
// ============================================================================

static void draw_file_browser(ViewerState& st) {
    ImGui::BeginChild("FileBrowser", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::TextDisabled("FILES");
    draw_breadcrumbs(st);
    ImGui::Spacing();

    // Search bar
    ImGui::PushItemWidth(-1);
    bool search_changed = ImGui::InputTextWithHint("##search", "Search files...",
                                                    st.search_buf, sizeof(st.search_buf));
    ImGui::PopItemWidth();

    // Filter buttons — flat category chips. Only the active one gets a strong
    // accent fill; idle chips are quiet. Each chip is only placed on the row if
    // it fully fits, otherwise it wraps (never half-clipped).
    ImGui::Spacing();
    const char* labels[] = {
        ICON_FA_LAYER_GROUP " All",
        ICON_FA_IMAGE " Textures",
        ICON_FA_CUBE " Models",
        ICON_FA_FILE " Scenes",
        ICON_FA_MUSIC " Audio",
        ICON_FA_CODE " Code"
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 3));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th_alpha(g_theme.surface_hover, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_theme.surface_hover);
    for (int i = 0; i < 6; i++) {
        const float btn_w = ImGui::CalcTextSize(labels[i]).x +
                            ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f;
        if (i > 0) {
            // Only share a row when this chip fully fits; otherwise it wraps to
            // the next line untouched (never rendered half-clipped).
            if (ImGui::GetContentRegionAvail().x >= btn_w) ImGui::SameLine();
        }
        const bool active = (st.type_filter == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, th_mix(g_theme.surface_active, g_theme.accent, 0.35f));
        if (ImGui::SmallButton(labels[i])) {
            st.type_filter = i;
            search_changed = true;
        }
        if (active) ImGui::PopStyleColor();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::Separator();

    // Folder pack shortcut: renderer-master demo folders hold a <name>.scn
    // plus the models/textures it references — load the whole folder as a scene.
    if (!st.current_dir_vendor_scn.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.30f, 0.58f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f, 0.40f, 0.72f, 1.0f));
        if (ImGui::Button(ICON_FA_LAYER_GROUP " Load Folder as Scene Pack")) {
            std::string pack_name = fs::path(st.current_dir_vendor_scn).filename().string();
            open_vendor_scn(st, st.current_dir_vendor_scn, pack_name);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This folder is a renderer-master demo pack (%s)\nLoad all its models / textures / animations at once",
                              fs::path(st.current_dir_vendor_scn).filename().string().c_str());
        ImGui::Spacing();
    }

    if (search_changed) apply_filters(st);

    // File list
    ImGui::BeginChild("FileList", ImVec2(0, 0));
    for (int i = 0; i < (int)st.filtered_files.size(); i++) {
        auto& f = st.filtered_files[i];

        ImVec4 color(0.85f, 0.85f, 0.85f, 1.0f); // Default neutral gray
        const char* icon = ICON_FA_FILE " ";
        if (f.is_dir) {
            icon = ICON_FA_FOLDER " ";
            color = ImVec4(0.88f, 0.72f, 0.40f, 1.0f); // yellow folder
        } else if (f.type == FTYPE_TEXTURE) {
            icon = ICON_FA_IMAGE " ";
            color = ImVec4(0.40f, 0.70f, 0.40f, 1.0f); // green texture
        } else if (f.type == FTYPE_MODEL) {
            icon = ICON_FA_CUBE " ";
            color = ImVec4(0.40f, 0.60f, 0.88f, 1.0f); // blue model
        } else if (f.type == FTYPE_SCENE) {
            icon = ICON_FA_LAYER_GROUP " ";
            color = ImVec4(0.70f, 0.50f, 0.88f, 1.0f); // purple scene
        } else if (f.type == FTYPE_AUDIO) {
            icon = ICON_FA_MUSIC " ";
            color = ImVec4(0.88f, 0.60f, 0.30f, 1.0f); // orange audio
        } else if (f.type == FTYPE_TEXT) {
            icon = ICON_FA_CODE " ";
            color = ImVec4(0.40f, 0.85f, 0.85f, 1.0f); // cyan text/code
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        char label[512];
        snprintf(label, sizeof(label), "%s%s##%d", icon, f.name.c_str(), i);
        bool selected = (i == st.selected_idx);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            st.selected_idx = i;
            select_file(st, f);
        }
        if (f.is_dir && !f.vendor_scn.empty()) {
            // Right-click: preview the whole folder as a vendor scene pack.
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(ICON_FA_LAYER_GROUP " Preview folder as scene pack")) {
                    std::string pack_name = fs::path(f.vendor_scn).filename().string();
                    open_vendor_scn(st, f.vendor_scn, pack_name);
                }
                if (ImGui::MenuItem("Open folder")) select_file(st, f);
                ImGui::EndPopup();
            }
            // Small purple pack badge at the row's right edge.
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.50f, 0.88f, 1.0f));
            ImGui::TextUnformatted(ICON_FA_LAYER_GROUP);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Demo scene pack: %s\nRight-click for options",
                                  fs::path(f.vendor_scn).filename().string().c_str());
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::EndChild();
}

// ============================================================================
// UI: Center panel — 3D model viewport
// ============================================================================

// Run the PostFX chain on the main preview FBO and return the texture to
// display. Falls back to the raw FBO color texture when FX is disabled.
// The FX buffers must match the FBO's actual (possibly HiDPI-scaled) size,
// not the logical UI size.
static GLuint postfx_display_tex(ViewerState& st) {
    if (!st.postfx_enabled || !st.postfx.enabled)
        return st.fbo_tex;
    return av::postfx_apply(st.fbo_tex, av::fbo_depth_texture(st.fbo),
                            st.fbo_w, st.fbo_h, st.postfx,
                            st.camera.near_plane, st.camera.far_plane,
                            (float)ImGui::GetTime());
}

static void draw_model_viewport(ViewerState& st) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Logical size drives the displayed image and any mouse math.
    int w = (int)avail.x;
    int h = (int)avail.y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    const float rs = st.render_scale > 0.0f ? st.render_scale : 1.0f;
    const int fw = std::max(1, (int)(avail.x * rs));
    const int fh = std::max(1, (int)(avail.y * rs));

    // POD previews have no scene lights or fog — don't inherit the last scene
    // frame's state.
    av::clear_point_lights();
    av::clear_directional_lights();
    av::set_depth_fog(false, nullptr, 0.0f, 0.0f);

    ImGuiIO& io = ImGui::GetIO();
    // Professional preview: turntable auto-orbit (R) and reframe (F).
    if (st.model_auto_rotate && !st.model.meshes.empty())
        st.camera.yaw += io.DeltaTime * 22.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) st.model_auto_rotate = !st.model_auto_rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        st.camera.target[0] = st.model.center_x;
        st.camera.target[1] = st.model.center_y;
        st.camera.target[2] = st.model.center_z;
        st.camera.distance = std::max(1.0f, st.model.radius * 2.5f);
    }

    if (!st.fbo) {
        st.fbo = av::create_fbo(fw, fh, &st.fbo_tex);
        st.fbo_w = fw; st.fbo_h = fh;
    } else if (fw != st.fbo_w || fh != st.fbo_h) {
        av::resize_fbo(st.fbo, fw, fh, &st.fbo_tex);
        st.fbo_w = fw; st.fbo_h = fh;
    }

    av::begin_3d(st.fbo, fw, fh, st.camera);
    if (st.show_grid) {
        av::render_grid(st.grid_size, st.model.min_y * st.model_scale);
    }
    float identity[16];
    av::mat4_identity(identity);
    float white[4] = {1, 1, 1, 1};
    float highlight[4] = {0.4f, 0.7f, 1.0f, 1.0f};

    // Compute global model transformation matrix (rz * ry * rx * scale)
    float rx[16], ry[16], rz[16], temp1[16], global_model_matrix[16];
    av::mat4_rotate_x(rx, st.model_rotate_x);
    av::mat4_rotate_y(ry, st.model_rotate_y);
    av::mat4_rotate_z(rz, st.model_rotate_z);

    av::mat4_multiply(temp1, ry, rx);
    av::mat4_multiply(global_model_matrix, rz, temp1);

    // Apply scale to global model matrix
    global_model_matrix[0] *= st.model_scale;
    global_model_matrix[1] *= st.model_scale;
    global_model_matrix[2] *= st.model_scale;
    global_model_matrix[4] *= st.model_scale;
    global_model_matrix[5] *= st.model_scale;
    global_model_matrix[6] *= st.model_scale;
    global_model_matrix[8] *= st.model_scale;
    global_model_matrix[9] *= st.model_scale;
    global_model_matrix[10] *= st.model_scale;

    // ── PBR preview: vendored lighting pipeline (zauonlok/renderer, MIT) ──
    // Directional shadow-map pass first (depth from the sun's point of view),
    // then the PBR program samples it with the N·L-scaled bias. IBL intensity
    // is driven from the View menu slider; the procedural sky cubemap built at
    // renderer_init is the default environment.
    if (st.pbr_preview) {
        // Vendor .scn lighting: the scene's `punctual` factor scales the IBL
        // contribution (in the reference it modulates the direct light).
        float env_intensity = st.pbr_env_intensity;
        // Vendor .scn: `ambient` scales the IBL/env contribution (reference
        // pbr_shader.c line 472); `environment: null` disables IBL entirely.
        if (st.vendor_mode && !st.vendor_scn.models.empty()) {
            if (st.vendor_scn.environment == "null")
                env_intensity = 0.0f;
            else if (st.vendor_scn.ambient > 0.0f)
                env_intensity *= st.vendor_scn.ambient;
        }
        av::pbr_set_env_intensity(env_intensity);
        if (st.pbr_shadows && !st.model.meshes.empty()) {
            if (!st.shadow_fbo || st.shadow_fbo_w != fw || st.shadow_fbo_h != fh) {
                if (st.shadow_fbo) glDeleteFramebuffers(1, &st.shadow_fbo);
                if (st.shadow_depth_tex) glDeleteTextures(1, &st.shadow_depth_tex);
                st.shadow_fbo = 0; st.shadow_depth_tex = 0;
                st.shadow_fbo = av::create_shadow_fbo(fw, fh, &st.shadow_depth_tex);
                st.shadow_fbo_w = fw; st.shadow_fbo_h = fh;
            }
            if (st.shadow_fbo) {
                // Ortho light frustum around the model, aimed along the sun.
                float ldir[3] = { av::g_light_dir[0], av::g_light_dir[1], av::g_light_dir[2] };
                float llen = sqrtf(ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2]);
                if (llen > 1e-6f) { ldir[0] /= llen; ldir[1] /= llen; ldir[2] /= llen; }
                float cx = st.model.center_x, cy = st.model.center_y, cz = st.model.center_z;
                float radius = std::max(0.5f, st.model.radius * st.model_scale);
                float dist = radius * 4.0f;
                float lview[16], lproj[16];
                av::mat4_look_at(lview, cx - ldir[0] * dist, cy - ldir[1] * dist, cz - ldir[2] * dist,
                                 cx, cy, cz, 0.0f, 1.0f, 0.0f);
                light_ortho(lproj, -radius * 1.5f, radius * 1.5f,
                            -radius * 1.5f, radius * 1.5f, 0.1f, dist * 2.0f);
                av::pbr_set_shadow(lview, lproj, st.shadow_depth_tex);
                av::pbr_enable_shadows(true);

                av::begin_shadow_pass(st.shadow_fbo, fw, fh);
                if (!st.model.nodes.empty()) {
                    for (int i = 0; i < (int)st.model.nodes.size(); ++i) {
                        const auto& node = st.model.nodes[i];
                        if (node.object_index < 0 || node.object_index >= (int)st.gpu_meshes.size()) continue;
                        float node_matrix[16];
                        av::get_node_matrix(st.model, i, st.current_frame, node_matrix);
                        float final_matrix[16];
                        if (st.model.has_center_point) {
                            float center_offset[16], centered_global[16];
                            av::mat4_translate(center_offset, -st.model.center_point[0],
                                               -st.model.center_point[1], -st.model.center_point[2]);
                            av::mat4_multiply(centered_global, global_model_matrix, center_offset);
                            av::mat4_multiply(final_matrix, centered_global, node_matrix);
                        } else {
                            av::mat4_multiply(final_matrix, global_model_matrix, node_matrix);
                        }
                        av::shadow_render_mesh(st.gpu_meshes[node.object_index], final_matrix);
                    }
                } else {
                    for (auto& gm : st.gpu_meshes) av::shadow_render_mesh(gm, global_model_matrix);
                }
                av::end_shadow_pass();
            } else {
                av::pbr_enable_shadows(false);
            }
        } else {
            av::pbr_enable_shadows(false);
        }
    }

    if (!st.model.nodes.empty()) {
        for (int i = 0; i < (int)st.model.nodes.size(); i++) {
            const auto& node = st.model.nodes[i];
            if (node.object_index < 0 || node.object_index >= (int)st.gpu_meshes.size()) continue;

            auto& gm = st.gpu_meshes[node.object_index];
            if (st.show_textured) {
                int mat_idx = node.material_index;
                int tex_idx = -1;
                if (mat_idx >= 0 && mat_idx < (int)st.model.materials.size()) {
                    tex_idx = st.model.materials[mat_idx].diffuse_texture_index;
                }
                if (tex_idx >= 0 && tex_idx < (int)st.model_textures.size()) {
                    gm.texture_id = st.model_textures[tex_idx];
                } else if (!st.model_textures.empty()) {
                    gm.texture_id = st.model_textures[0];
                } else {
                    gm.texture_id = 0;
                }
            } else {
                gm.texture_id = 0;
            }

            float node_matrix[16];
            av::get_node_matrix(st.model, i, st.current_frame, node_matrix);

            float final_matrix[16];
            float centered_global[16];
            if (st.model.has_center_point) {
                float center_offset[16];
                av::mat4_translate(center_offset, -st.model.center_point[0],
                                   -st.model.center_point[1], -st.model.center_point[2]);
                av::mat4_multiply(centered_global, global_model_matrix, center_offset);
            } else {
                std::memcpy(centered_global, global_model_matrix, sizeof(centered_global));
            }
            av::mat4_multiply(final_matrix, centered_global, node_matrix);

            // POD skin streams are CPU-deformed here so the viewer follows the
            // same bone-matrix path as the ARM32 ModelInstance renderer. The
            // stride-aware update preserves tangents/joints for PBR-layout
            // meshes (glb normal maps).
            if (node.object_index < static_cast<int>(st.model.meshes.size()) &&
                st.model.meshes[node.object_index].bones_per_vertex > 0) {
                std::vector<float> skinned_positions, skinned_normals;
                if (av::skin_mesh(st.model, i, st.current_frame, skinned_positions, skinned_normals)) {
                    const auto& source_mesh = st.model.meshes[node.object_index];
                    av::update_mesh_vertices_ex(gm, skinned_positions.data(),
                                                skinned_normals.empty() ? nullptr : skinned_normals.data(),
                                                source_mesh.uvs.empty() ? nullptr : source_mesh.uvs.data(),
                                                nullptr, nullptr, nullptr,
                                                source_mesh.num_vertices);
                }
            }

            // Vendor .obj/.scn models skin on the GPU via the .ani skeleton
            // (the PBR program's uJoints). Per-node skeleton switch is safe:
            // each node renders right after its matrices are uploaded.
            if (st.vendor_mode && i < (int)st.vendor_skeletons.size()) {
                if (st.vendor_skel_valid[i]) {
                    std::vector<float> joint_matrices;
                    av::ani_evaluate(st.vendor_skeletons[i], st.vendor_anim_time,
                                     joint_matrices, nullptr);
                    av::pbr_set_joint_matrices(joint_matrices.data(),
                                               (int)(joint_matrices.size() / 16));
                } else if (gm.has_skinning) {
                    av::pbr_set_joint_matrices(nullptr, 0);
                }
            }

            float mat_color[4] = {1, 1, 1, 1};
            int mat_idx = node.material_index;
            if (mat_idx >= 0 && mat_idx < (int)st.model.materials.size()) {
                const auto& m = st.model.materials[mat_idx];
                mat_color[0] = m.diffuse[0]; mat_color[1] = m.diffuse[1]; mat_color[2] = m.diffuse[2];
                mat_color[3] = m.opacity;
            }
            float* col = (node.object_index == st.highlighted_mesh) ? highlight : mat_color;
            if (st.pbr_preview) {
                // Vendored PBR preview: GGX/Smith/Fresnel + IBL + DSM. Defaults
                // are neutral (metal 0.02, rough 0.55); real PBR data replaces
                // them for .glb materials (gltf_pbr_materials) and vendor .scn
                // pbrm/pbrs materials (vendor_pbr_mats).
                av::PBRMaterial pmat;
                pmat.base_color[0] = col[0]; pmat.base_color[1] = col[1];
                pmat.base_color[2] = col[2]; pmat.base_color[3] = col[3];
                pmat.basecolor_tex = gm.texture_id;
                pmat.metalness = 0.02f;
                pmat.roughness = 0.55f;
                if (st.vendor_mode) {
                    if (i < (int)st.vendor_pbr_mats.size() && st.vendor_pbr_valid[i]) {
                        pmat = st.vendor_pbr_mats[i];
                        pmat.basecolor_tex = gm.texture_id;
                    }
                } else if (!st.gltf_pbr_materials.empty() &&
                           st.gltf_pbr_materials.size() == st.model.materials.size() &&
                           mat_idx >= 0 && mat_idx < (int)st.gltf_pbr_materials.size()) {
                    pmat = st.gltf_pbr_materials[mat_idx];
                    pmat.basecolor_tex = gm.texture_id;
                }
                av::pbr_render_mesh(gm, final_matrix, pmat, false);
                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::pbr_render_mesh(gm, final_matrix, pmat, true);
                }
            } else {
                av::render_mesh(gm, final_matrix, col, false);
                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, final_matrix, wire_col, true);
                }
            }
        }
        if (st.show_skeleton) {
            std::vector<float> lines;
            if (st.vendor_mode) {
                // .ani bone chain: world positions from the evaluated skeleton
                // (composed transforms, before inverse-bind), transformed into
                // the viewport by each node's matrix — exactly like the mesh.
                float centered_global[16];
                if (st.model.has_center_point) {
                    float center_offset[16];
                    av::mat4_translate(center_offset, -st.model.center_point[0],
                                       -st.model.center_point[1], -st.model.center_point[2]);
                    av::mat4_multiply(centered_global, global_model_matrix, center_offset);
                } else {
                    std::memcpy(centered_global, global_model_matrix, sizeof(centered_global));
                }
                for (int i = 0; i < (int)st.model.nodes.size(); ++i) {
                    if (i >= (int)st.vendor_skeletons.size() || !st.vendor_skel_valid[i]) continue;
                    const auto& skel = st.vendor_skeletons[i];
                    std::vector<float> jm, world;
                    av::ani_evaluate(skel, st.vendor_anim_time, jm, nullptr, &world);
                    float node_matrix[16];
                    av::get_node_matrix(st.model, i, 0.0f, node_matrix);
                    float m[16];
                    av::mat4_multiply(m, centered_global, node_matrix);
                    auto xform = [&m](float x, float y, float z,
                                      float& ox, float& oy, float& oz) {
                        ox = m[0] * x + m[4] * y + m[8] * z + m[12];
                        oy = m[1] * x + m[5] * y + m[9] * z + m[13];
                        oz = m[2] * x + m[6] * y + m[10] * z + m[14];
                    };
                    for (int j = 0; j < (int)skel.joints.size(); ++j) {
                        const int parent = skel.joints[j].parent;
                        if (parent < 0 || parent >= (int)skel.joints.size()) continue;
                        const float* a = &world[(size_t)parent * 16];
                        const float* b = &world[(size_t)j * 16];
                        float ax, ay, az, bx, by, bz;
                        xform(a[12], a[13], a[14], ax, ay, az);
                        xform(b[12], b[13], b[14], bx, by, bz);
                        lines.insert(lines.end(), {ax, ay, az, bx, by, bz});
                    }
                }
            } else {
                for (int i = 0; i < static_cast<int>(st.model.nodes.size()); ++i) {
                    const int parent = st.model.nodes[i].parent_index;
                    if (parent < 0 || parent >= static_cast<int>(st.model.nodes.size())) continue;
                    float child[16], parent_matrix[16];
                    av::get_node_matrix(st.model, i, st.current_frame, child);
                    av::get_node_matrix(st.model, parent, st.current_frame, parent_matrix);
                    lines.insert(lines.end(), {parent_matrix[12], parent_matrix[13], parent_matrix[14],
                                               child[12], child[13], child[14]});
                }
            }
            if (!lines.empty()) {
                float skeleton_color[4] = {1.0f, 0.45f, 0.08f, 1.0f};
                // Vendor lines are already world-transformed; POD lines stay in
                // model space and need the global model matrix.
                av::render_lines(lines.data(), static_cast<int>(lines.size() / 3),
                                 skeleton_color, st.vendor_mode ? nullptr : global_model_matrix, 2.0f);
            }
        }
    } else {
        // Fallback for models without nodes
        for (int i = 0; i < (int)st.gpu_meshes.size(); i++) {
            auto& gm = st.gpu_meshes[i];
            if (st.show_textured && !st.model_textures.empty()) {
                gm.texture_id = st.model_textures[0];
            } else {
                gm.texture_id = 0;
            }

            float centered_model[16];
            if (st.model.has_center_point) {
                float center_offset[16];
                av::mat4_translate(center_offset, -st.model.center_point[0],
                                   -st.model.center_point[1], -st.model.center_point[2]);
                av::mat4_multiply(centered_model, global_model_matrix, center_offset);
            } else {
                std::memcpy(centered_model, global_model_matrix, sizeof(centered_model));
            }
            float* col = (i == st.highlighted_mesh) ? highlight : white;
            if (st.pbr_preview) {
                av::PBRMaterial pmat;
                pmat.base_color[0] = col[0]; pmat.base_color[1] = col[1];
                pmat.base_color[2] = col[2]; pmat.base_color[3] = col[3];
                pmat.basecolor_tex = gm.texture_id;
                pmat.metalness = 0.02f;
                pmat.roughness = 0.55f;
                av::pbr_render_mesh(gm, centered_model, pmat, false);
                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::pbr_render_mesh(gm, centered_model, pmat, true);
                }
            } else {
                av::render_mesh(gm, centered_model, col, false);
                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, centered_model, wire_col, true);
                }
            }
        }
    }
    av::end_3d();

    const GLuint display_tex = postfx_display_tex(st);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)display_tex, ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0));

    // ── Professional viewport chrome: model stats header ──
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        char header[288];
        snprintf(header, sizeof(header), "%s   %s%d meshes \xc2\xb7 %d verts \xc2\xb7 %d faces",
                 st.sel_name.c_str(), st.model_auto_rotate ? ICON_FA_SPINNER " " : "",
                 (int)st.model.meshes.size(), st.model.total_vertices, st.model.total_faces);
        const float tw = ImGui::CalcTextSize(header).x;
        dl->AddRectFilled(ImVec2(pos.x + 8.0f, pos.y + 8.0f),
                          ImVec2(pos.x + 18.0f + tw + 24.0f, pos.y + 30.0f),
                          IM_COL32(20, 24, 31, 215), 5.0f);
        dl->AddText(ImVec2(pos.x + 16.0f, pos.y + 12.0f), IM_COL32(225, 232, 240, 255), header);

        if (!st.model.texture_filenames.empty() || st.model.num_frames > 0) {
            char anim_info[192];
            snprintf(anim_info, sizeof(anim_info), "%s%s%s%s%s",
                     st.model.num_frames > 0 ?
                         (st.anim_playing ? ICON_FA_PLAY " anim \xc2\xb7 " : ICON_FA_PAUSE " anim \xc2\xb7 ") : "",
                     st.model.texture_filenames.empty() ? "" : "tex \xc2\xb7 ",
                     st.show_textured ? "" : "flat ",
                     st.show_wireframe ? "wire \xc2\xb7 " : "",
                     st.show_skeleton ? "bones" : "");
            dl->AddRectFilled(ImVec2(pos.x + w - 12.0f - ImGui::CalcTextSize(anim_info).x - 20.0f, pos.y + 8.0f),
                              ImVec2(pos.x + w - 8.0f, pos.y + 30.0f),
                              IM_COL32(20, 24, 31, 215), 5.0f);
            dl->AddText(ImVec2(pos.x + w - ImGui::CalcTextSize(anim_info).x - 20.0f, pos.y + 12.0f),
                        IM_COL32(180, 200, 220, 255), anim_info);
        }

        // Bottom-right control hint.
        dl->AddText(ImVec2(pos.x + w - 268.0f, pos.y + h - 20.0f),
                    IM_COL32(140, 152, 172, 190),
                    "LMB orbit  RMB pan  wheel zoom  R auto-rotate  F frame");
    }

    // Draw overlay if texture dependencies are missing
    if (!st.missing_textures.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 10.0f, pos.y + 44.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.11f, 0.11f, 0.85f)); // Semi-transparent dark red
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
        
        float overlay_h = st.missing_textures.size() > 4 ? 80.0f : 55.0f;
        ImGui::BeginChild("##missing_warning", ImVec2((float)w - 20.0f, overlay_h), ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), ICON_FA_WARNING " Warning: Missing Texture Dependencies!");
        ImGui::SameLine();
        ImGui::TextDisabled("| Please check dependencies:");
        
        ImGui::Spacing();
        for (size_t k = 0; k < st.missing_textures.size(); k++) {
            if (k > 0) ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.65f, 0.35f, 1.0f), " %s", st.missing_textures[k].c_str());
        }
        
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + (float)h)); // restore cursor
    }

    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();

        if (io.MouseWheel != 0.0f) {
            const float factor = std::pow(0.94f, io.MouseWheel * st.cam_zoom_speed);
            st.camera.distance *= factor;
            if (st.camera.distance < 0.1f) st.camera.distance = 0.1f;
            if (st.camera.distance > 500.0f) st.camera.distance = 500.0f;
        }

        if (ImGui::IsMouseDragging(0)) {
            ImVec2 delta = io.MouseDelta;
            float sign_x = st.cam_invert_x ? -1.0f : 1.0f;
            float sign_y = st.cam_invert_y ? -1.0f : 1.0f;
            st.camera.yaw   += delta.x * 0.5f * st.cam_orbit_speed * sign_x;
            st.camera.pitch += delta.y * 0.5f * st.cam_orbit_speed * sign_y;
            if (st.camera.pitch >  89.0f) st.camera.pitch =  89.0f;
            if (st.camera.pitch < -89.0f) st.camera.pitch = -89.0f;
        }

        if (ImGui::IsMouseDragging(1)) {
            ImVec2 delta = io.MouseDelta;
            float scale = st.camera.distance * 0.003f * st.cam_pan_speed;
            float yaw_rad = st.camera.yaw * 3.14159f / 180.0f;
            float pitch_rad = st.camera.pitch * 3.14159f / 180.0f;

            // Compute local camera Right and Up vectors
            float rx = cosf(yaw_rad);
            float rz = -sinf(yaw_rad);

            float ux = -sinf(yaw_rad) * sinf(pitch_rad);
            float uy = cosf(pitch_rad);
            float uz = -cosf(yaw_rad) * sinf(pitch_rad);

            // Translate camera target locally
            st.camera.target[0] -= rx * delta.x * scale;
            st.camera.target[2] -= rz * delta.x * scale;

            st.camera.target[0] += ux * delta.y * scale;
            st.camera.target[1] += uy * delta.y * scale;
            st.camera.target[2] += uz * delta.y * scale;
        }
    }
}

// ============================================================================
// UI: Center panel — Texture preview
// ============================================================================

static void compute_transformed_uvs(int rotation, bool flip_h, bool flip_v, ImVec2 uv[4]) {
    // Default UVs
    uv[0] = ImVec2(0, 0); // top-left
    uv[1] = ImVec2(1, 0); // top-right
    uv[2] = ImVec2(1, 1); // bottom-right
    uv[3] = ImVec2(0, 1); // bottom-left

    // Rotate (CW)
    if (rotation == 90) {
        uv[0] = ImVec2(0, 1); uv[1] = ImVec2(0, 0); uv[2] = ImVec2(1, 0); uv[3] = ImVec2(1, 1);
    } else if (rotation == 180) {
        uv[0] = ImVec2(1, 1); uv[1] = ImVec2(0, 1); uv[2] = ImVec2(0, 0); uv[3] = ImVec2(1, 0);
    } else if (rotation == 270) {
        uv[0] = ImVec2(1, 0); uv[1] = ImVec2(1, 1); uv[2] = ImVec2(0, 1); uv[3] = ImVec2(0, 0);
    }

    // Flip horizontal (swap X values on the UVs)
    if (flip_h) {
        for (int i = 0; i < 4; ++i) uv[i].x = 1.0f - uv[i].x;
    }
    // Flip vertical (swap Y values on the UVs)
    if (flip_v) {
        for (int i = 0; i < 4; ++i) uv[i].y = 1.0f - uv[i].y;
    }
}

static void draw_texture_preview(ViewerState& st) {
    if (!st.preview_tex) {
        ImGui::TextDisabled("Failed to load texture.");
        return;
    }

    ImGui::BeginChild("TextureViewportChild", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);

    // ── Texture toolbar (flat, scene-visualizer style) ──────────────────
    // Edit tools, bake ops, save and the batch converter live in the preview
    // viewport header instead of the sidebar, so every texture action sits
    // directly above the canvas.
    {
        const bool can_edit = st.tex_edit_valid;
        const bool small_grid = can_edit && st.tex_edit_w <= 100 && st.tex_edit_h <= 100;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th_alpha(g_theme.surface_hover, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_theme.surface_hover);

        if (!can_edit) ImGui::BeginDisabled();
        if (ImGui::Checkbox(ICON_FA_PAINTBRUSH " Edit", &st.tex_edit_enabled)) {
            if (st.tex_edit_enabled) {
                // Entering edit mode: pre-flip flipped containers to display
                // space and drop the view transforms so the mouse mapping is
                // 1:1 with the canvas.
                if (can_edit && tex_edit_is_container(st) && !st.tex_edit_display_space) {
                    tex_edit_flip_buffer_hv(st.tex_edit_pixels, st.tex_edit_w, st.tex_edit_h);
                    st.tex_edit_display_space = true;
                    tex_editor_upload(st);
                }
                st.texture_rotation = 0;
                st.texture_flip_h = false;
                st.texture_flip_v = false;
                st.tex_reset_required = true;
            } else {
                // Exiting edit mode: restore the file-space buffer and the
                // default viewer flips so the display matches a fresh open.
                if (st.tex_edit_display_space) {
                    tex_edit_flip_buffer_hv(st.tex_edit_pixels, st.tex_edit_w, st.tex_edit_h);
                    st.tex_edit_display_space = false;
                    tex_editor_upload(st);
                }
                st.texture_rotation = 0;
                st.texture_flip_h = true;
                st.texture_flip_v = true;
                st.tex_reset_required = true;
            }
        }
        if (!can_edit) ImGui::EndDisabled();
        if (st.tex_edit_enabled && can_edit) {
            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();
            const char* tool_names[] = {
                ICON_FA_PEN " Brush", ICON_FA_TRASH " Eraser",
                ICON_FA_WAND_SPARKLES " Fill", ICON_FA_EYE " Pick", " Crop"};
            const char* tool_tips[] = {
                "Brush: drag on the image to paint",
                "Eraser: drag on the image to erase",
                "Fill: click a region to flood-fill it",
                "Pick: click to sample a color",
                "Crop: drag a rectangle, then Apply Crop"};
            for (int t = 0; t < 5; ++t) {
                if (t) ImGui::SameLine();
                if (ImGui::Selectable(tool_names[t], st.tex_edit_tool == t))
                    st.tex_edit_tool = t;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tool_tips[t]);
            }

            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Bake")) ImGui::OpenPopup("##tex_bake");
            if (ImGui::BeginPopup("##tex_bake")) {
                if (ImGui::MenuItem("Rotate 90° CW"))  { tex_edit_snapshot(st); tex_edit_bake_rotate(st, 1); }
                if (ImGui::MenuItem("Rotate 180°"))    { tex_edit_snapshot(st); tex_edit_bake_rotate(st, 2); }
                if (ImGui::MenuItem("Rotate 90° CCW")) { tex_edit_snapshot(st); tex_edit_bake_rotate(st, 3); }
                ImGui::Separator();
                if (ImGui::MenuItem("Flip Horizontal")) { tex_edit_snapshot(st); tex_edit_bake_flip(st, true);  }
                if (ImGui::MenuItem("Flip Vertical"))   { tex_edit_snapshot(st); tex_edit_bake_flip(st, false); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_IMAGE " Save")) ImGui::OpenPopup("##tex_save");
            if (ImGui::BeginPopup("##tex_save")) {
                ImGui::SetNextItemWidth(300.0f);
                ImGui::InputText("Path", st.tex_edit_save_path,
                                 sizeof(st.tex_edit_save_path));
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_IMAGE " Save PNG…")) {
                    if (tex_edit_save_png(st, st.tex_edit_save_path)) {
                        st.status_msg = "Saved PNG: " + std::string(st.tex_edit_save_path);
                        log_file_event("TextureEdit", "Saved PNG: " + std::string(st.tex_edit_save_path));
                    } else st.status_msg = "PNG save failed: " + std::string(st.tex_edit_save_path);
                }
                if (ImGui::MenuItem(ICON_FA_FILE " Save .tex…")) {
                    if (tex_edit_save_tex(st, st.tex_edit_save_path)) {
                        st.status_msg = "Saved .tex: " + std::string(st.tex_edit_save_path);
                        log_file_event("TextureEdit", "Saved .tex: " + std::string(st.tex_edit_save_path));
                    } else st.status_msg = ".tex save failed";
                }
                if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Overwrite Source", nullptr,
                                    false, tex_edit_is_container(st) && st.tex_edit_dirty)) {
                    if (tex_edit_save_tex(st, st.tex_edit_src)) {
                        st.status_msg = "Saved back to " + st.tex_edit_src;
                        st.tex_edit_dirty = false;
                        log_file_event("TextureEdit", "Overwrote: " + st.tex_edit_src);
                    } else st.status_msg = "Overwrite failed";
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            if (!st.tex_edit_undo.empty()) {
                if (ImGui::Button(ICON_FA_CLOCK_ROTATE_LEFT " Undo")) tex_edit_undo(st);
                ImGui::SameLine();
            }
            if (ImGui::Button("Reload")) {
                tex_editor_load(st, st.tex_edit_src);
                tex_editor_upload(st);
                st.tex_reset_required = true;
            }

            // Contextual row: brush options / crop actions / pick hint.
            if (st.tex_edit_tool == 0 || st.tex_edit_tool == 1) {
                if (small_grid) {
                    st.tex_edit_size = 1.0f;   // pixel-map editing is 1px only
                    ImGui::SameLine();
                    ImGui::TextDisabled("1 px");
                } else {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::SliderFloat("Size", &st.tex_edit_size, 1.0f, 128.0f, "%.0f");
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150.0f);
                ImGui::ColorEdit4("Color", st.tex_edit_color, ImGuiColorEditFlags_AlphaPreviewHalf);
            } else if (st.tex_edit_tool == 4 && st.tex_edit_crop_ready) {
                ImGui::SameLine();
                if (ImGui::Button("Apply Crop")) tex_edit_apply_crop(st);
                ImGui::SameLine();
                if (ImGui::Button("Cancel Crop")) st.tex_edit_crop_ready = false;
            } else if (st.tex_edit_tool == 3) {
                ImGui::SameLine();
                ImGui::TextDisabled("Click the image to sample a color");
            }
        } else {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TOOLBOX " Convert…")) st.batch_converter.open_window = true;
        }

        ImGui::SameLine();
        if (st.tex_edit_dirty && can_edit)
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "(modified)");
        else if (can_edit)
            ImGui::TextDisabled("(original)");
        ImGui::PopStyleColor(3);
    }
    
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Reset logic: center and fit to screen (always fit, so images never
    // load too large or too small; pixel-map textures get a min zoom so
    // individual cells stay comfortable to click).
    if (st.tex_reset_required) {
        float scale_w = avail.x / (float)st.tex_w;
        float scale_h = avail.y / (float)st.tex_h;
        float fit_scale = std::min(scale_w, scale_h) * 0.9f;
        st.tex_zoom = fit_scale;
        if (st.tex_zoom > 64.0f) st.tex_zoom = 64.0f;
        if (st.tex_zoom < 0.05f) st.tex_zoom = 0.05f;
        if (st.tex_edit_valid && st.tex_edit_w <= 100 && st.tex_edit_h <= 100 &&
            st.tex_zoom < 3.0f)
            st.tex_zoom = 3.0f;
        st.tex_pan_x = avail.x * 0.5f;
        st.tex_pan_y = avail.y * 0.5f;
        st.tex_reset_required = false;
    }

    // Adjust render size based on 90/270 degree rotation
    float w_render = (float)st.tex_w;
    float h_render = (float)st.tex_h;
    if (st.texture_rotation == 90 || st.texture_rotation == 270) {
        w_render = (float)st.tex_h;
        h_render = (float)st.tex_w;
    }

    ImVec2 img_size(w_render * st.tex_zoom, h_render * st.tex_zoom);

    // Compute absolute corners of the quad centered at (pan_x, pan_y)
    ImVec2 p1(cursor.x + st.tex_pan_x - img_size.x * 0.5f, cursor.y + st.tex_pan_y - img_size.y * 0.5f);
    ImVec2 p2(p1.x + img_size.x, p1.y);
    ImVec2 p3(p1.x + img_size.x, p1.y + img_size.y);
    ImVec2 p4(p1.x, p1.y + img_size.y);

    // Bind texture to dynamically adjust filtering quality
    glBindTexture(GL_TEXTURE_2D, st.preview_tex);
    const bool tex_small_grid = st.tex_edit_valid && st.tex_edit_enabled &&
                                st.tex_edit_w <= 100 && st.tex_edit_h <= 100;
    if (st.tex_zoom > 1.0f && (st.tex_pixel_art_mode || tex_small_grid)) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);
    }

    // Draw Checkerboard Background
    if (st.checker_tex) {
        float uv_scale_x = img_size.x / 32.0f;
        float uv_scale_y = img_size.y / 32.0f;
        dl->AddImageQuad((ImTextureID)(intptr_t)st.checker_tex,
                         p1, p2, p3, p4,
                         ImVec2(0, 0), ImVec2(uv_scale_x, 0), ImVec2(uv_scale_x, uv_scale_y), ImVec2(0, uv_scale_y));
    }

    // Compute transformed UVs and draw Image
    ImVec2 uv[4];
    compute_transformed_uvs(st.texture_rotation, st.texture_flip_h, st.texture_flip_v, uv);
    ImU32 tint_col = ImGui::ColorConvertFloat4ToU32(ImVec4(st.texture_tint[0], st.texture_tint[1], st.texture_tint[2], st.texture_tint[3]));
    dl->AddImageQuad((ImTextureID)(intptr_t)st.preview_tex, p1, p2, p3, p4, uv[0], uv[1], uv[2], uv[3], tint_col);

    // Zoom/Pan interaction area
    ImGui::InvisibleButton("##tex_area", avail);
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            float old_zoom = st.tex_zoom;
            st.tex_zoom *= (1.0f + io.MouseWheel * 0.15f);
            if (st.tex_zoom < 0.05f) st.tex_zoom = 0.05f;
            if (st.tex_zoom > 64.0f) st.tex_zoom = 64.0f;

            float mx = io.MousePos.x - cursor.x;
            float my = io.MousePos.y - cursor.y;
            float tx = mx - st.tex_pan_x;
            float ty = my - st.tex_pan_y;
            float ratio = st.tex_zoom / old_zoom;
            st.tex_pan_x = mx - tx * ratio;
            st.tex_pan_y = my - ty * ratio;
        }
        // While editing, LMB draws/picks/crops instead of panning (MMB pans).
        if (st.tex_edit_enabled && st.tex_edit_valid) {
            if (ImGui::IsMouseDragging(2)) {
                st.tex_pan_x += io.MouseDelta.x;
                st.tex_pan_y += io.MouseDelta.y;
            }
        } else if (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(2)) {
            st.tex_pan_x += io.MouseDelta.x;
            st.tex_pan_y += io.MouseDelta.y;
        }
    }

    // ── Image editor: mouse → pixel mapping + tools ──
    // uv[0] maps to image pixel (0,0) and uv[2] to (w-1, h-1), so the quad
    // corners give the pixel coordinates for ANY rotation/flip combo.
    if (st.tex_edit_valid && st.tex_edit_enabled) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hovered = ImGui::IsItemHovered();
        const float u = (img_size.x > 0.0f) ? (mouse.x - p1.x) / img_size.x : -1.0f;
        const float v = (img_size.y > 0.0f) ? (mouse.y - p1.y) / img_size.y : -1.0f;
        const int px = (int)(u * st.tex_edit_w);
        const int py = (int)(v * st.tex_edit_h);
        const bool inside = hovered && u >= 0.0f && u <= 1.0f && v >= 0.0f &&
                            v <= 1.0f && px >= 0 && py >= 0 &&
                            px < st.tex_edit_w && py < st.tex_edit_h;

        switch (st.tex_edit_tool) {
        case 0: case 1: {   // brush / eraser (continuous strokes, live upload)
            const bool erasing = st.tex_edit_tool == 1;
            if (ImGui::IsMouseClicked(0) && inside) {
                tex_edit_snapshot(st);
                tex_edit_brush_at(st, px, py, erasing);
                st.tex_edit_last_x = px;
                st.tex_edit_last_y = py;
                st.tex_edit_last_upload = ImGui::GetTime();
                tex_editor_upload(st);   // live: show the stroke immediately
            } else if (ImGui::IsMouseDown(0) && st.tex_edit_last_x >= 0 &&
                       (px != st.tex_edit_last_x || py != st.tex_edit_last_y)) {
                tex_edit_brush_line(st, st.tex_edit_last_x, st.tex_edit_last_y,
                                    px, py, erasing);
                st.tex_edit_last_x = px;
                st.tex_edit_last_y = py;
                // Live uploads throttled to ~60 Hz so big textures don't
                // stall the drag; the brush buffer is still fully updated.
                const double now = ImGui::GetTime();
                if (now - st.tex_edit_last_upload >= 0.016) {
                    st.tex_edit_last_upload = now;
                    tex_editor_upload(st);
                }
            }
            if (ImGui::IsMouseReleased(0)) {
                st.tex_edit_last_x = st.tex_edit_last_y = -1;
            }
            break;
        }
        case 2:   // paint bucket
            if (ImGui::IsMouseClicked(0) && inside) {
                tex_edit_snapshot(st);
                tex_edit_flood_fill(st, px, py);
                tex_editor_upload(st);
            }
            break;
        case 3: {  // eyedropper
            if (ImGui::IsMouseClicked(0) && inside) {
                const uint8_t* p =
                    &st.tex_edit_pixels[((size_t)py * st.tex_edit_w + px) * 4];
                st.tex_edit_color[0] = p[0] / 255.0f;
                st.tex_edit_color[1] = p[1] / 255.0f;
                st.tex_edit_color[2] = p[2] / 255.0f;
                st.tex_edit_color[3] = p[3] / 255.0f;
            }
            break;
        }
        case 4: {  // crop
            if (ImGui::IsMouseClicked(0) && inside) {
                st.tex_edit_crop_active = true;
                st.tex_edit_crop_x0 = st.tex_edit_crop_x1 = (float)px;
                st.tex_edit_crop_y0 = st.tex_edit_crop_y1 = (float)py;
                st.tex_edit_crop_ready = false;
            } else if (ImGui::IsMouseDown(0) && st.tex_edit_crop_active && inside) {
                st.tex_edit_crop_x1 = (float)px;
                st.tex_edit_crop_y1 = (float)py;
                st.tex_edit_crop_ready = true;
            }
            if (ImGui::IsMouseReleased(0)) st.tex_edit_crop_active = false;
            break;
        }
        }

        // Overlays: crop rectangle + brush cursor.
        if (st.tex_edit_tool == 4 && st.tex_edit_crop_ready) {
            const ImVec2 c0(p1.x + st.tex_edit_crop_x0 / (float)st.tex_edit_w * img_size.x,
                            p1.y + st.tex_edit_crop_y0 / (float)st.tex_edit_h * img_size.y);
            const ImVec2 c1(p1.x + st.tex_edit_crop_x1 / (float)st.tex_edit_w * img_size.x,
                            p1.y + st.tex_edit_crop_y1 / (float)st.tex_edit_h * img_size.y);
            dl->AddRectFilled(c0, c1, IM_COL32(255, 120, 120, 36));
            dl->AddRect(c0, c1, IM_COL32(255, 80, 80, 255), 0.0f, 0, 2.0f);
        }
        if ((st.tex_edit_tool == 0 || st.tex_edit_tool == 1) && hovered) {
            const ImVec2 cc(p1.x + u * img_size.x, p1.y + v * img_size.y);
            const float cr = tex_small_grid
                ? std::max(1.0f, st.tex_zoom * 0.5f)      // one cell wide
                : std::max(1.0f, st.tex_edit_size * 0.5f * st.tex_zoom);
            dl->AddCircle(cc, cr, IM_COL32(255, 255, 255, 210), 0, 1.5f);
        }

        // Pixel-map mode (≤100×100): overlay the cell grid so each clickable
        // pixel is visible, drawn once the cells are large enough.
        if (tex_small_grid && st.tex_zoom >= 4.0f) {
            const int GW = st.tex_edit_w, GH = st.tex_edit_h;
            const ImU32 grid_col = IM_COL32(0, 0, 0, 70);
            for (int x = 1; x < GW; ++x) {
                const float gx = p1.x + img_size.x * (float)x / (float)GW;
                dl->AddLine(ImVec2(gx, p1.y), ImVec2(gx, p1.y + img_size.y), grid_col);
            }
            for (int y = 1; y < GH; ++y) {
                const float gy = p1.y + img_size.y * (float)y / (float)GH;
                dl->AddLine(ImVec2(p1.x, gy), ImVec2(p1.x + img_size.x, gy), grid_col);
            }
        }
    }

    ImGui::EndChild();
}

// ============================================================================
// UI: Center panel — Scene inspector
// ============================================================================

// ── Object statistics (browser chips + inspector) ────────────────────────
// Collects the resolved POD model + embedded ground-mesh totals for an object.
static void object_stats_for(const ViewerState& st, int index,
                             int* out_verts, int* out_tris, bool* out_loaded) {
    if (out_verts) *out_verts = 0;
    if (out_tris)  *out_tris = 0;
    if (out_loaded) *out_loaded = false;
    if (index < 0 || index >= (int)st.scene.objects.size()) return;
    const auto& obj = st.scene.objects[index];
    if (!obj.mesh_name.empty()) {
        auto it = st.scene_model_cache.find(obj.mesh_name);
        if (it != st.scene_model_cache.end()) {
            if (out_loaded) *out_loaded = true;
            if (out_verts) *out_verts += it->second.total_vertices;
            if (out_tris)  *out_tris  += it->second.total_faces;
        }
    }
    for (const auto& gm : obj.ground_meshes) {
        if (out_verts) *out_verts += gm.num_vertices;
        if (out_tris)  *out_tris  += gm.num_faces;
        if (out_loaded) *out_loaded = true;
    }
}

// ── LocalAABB (Rectangle { x:1, y:2, w:3, h:4 }, float32) helpers ──────
static bool parse_local_aabb(const std::string& bytes, float out[4]) {
    if (bytes.empty()) return false;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    try {
        proto::Reader r(bytes);
        proto::Field f;
        while (r.read_field(f)) {
            if (f.wire_type != proto::WIRE_I32) continue;
            if (f.field_number == 1)      out[0] = f.float_val;
            else if (f.field_number == 2) out[1] = f.float_val;
            else if (f.field_number == 3) out[2] = f.float_val;
            else if (f.field_number == 4) out[3] = f.float_val;
        }
        return true;
    } catch (...) { return false; }
}

static std::string serialize_local_aabb(float x, float y, float w, float h) {
    proto::Writer wr;
    wr.write_float_field(1, x);
    wr.write_float_field(2, y);
    wr.write_float_field(3, w);
    wr.write_float_field(4, h);
    return wr.to_string();
}

// ── Object inspector (bottom pane of the Objects panel) ──────────────────
// Professional properties editor: template/name identity, position (drag),
// LocalAABB, model stats, components, scene metadata and object actions.
static void draw_object_inspector(ViewerState& st) {
    if (st.scene.objects.empty()) {
        ImGui::TextDisabled("No objects in scene.");
        return;
    }

    if (st.selected_object < 0 || st.selected_object >= (int)st.scene.objects.size()) {
        if (st.scene_selection.size() > 1) {
            int verts = 0, tris = 0;
            for (int sel : st.scene_selection) object_stats_for(st, sel, &verts, &tris, nullptr);
            ImGui::TextColored(g_theme.accent, ICON_FA_OBJECT_GROUP " %zu objects selected",
                               st.scene_selection.size());
            ImGui::Separator();
            ImGui::Text("Total verts: %d", verts);
            ImGui::Text("Total tris:  %d", tris);
            ImGui::TextDisabled("Edit one object at a time — click a row to make it active.");
        } else {
            ImGui::TextDisabled("No object selected.\nClick an object in the browser above\nor pick one in the Visual viewport.");
        }
        return;
    }

    auto& obj = st.scene.objects[st.selected_object];
    const swk::ObjCategory cat = swk::classify_object(obj);
    const int obj_idx = st.selected_object;
    const bool from_library = !obj.template_name.empty() && obj.components.empty() &&
                              !obj.resolved_components.empty();

    // ── Header ──────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, g_theme.accent);
    ImGui::TextUnformatted(
        (std::string(swk::obj_category_icon(cat)) + "  " +
         (obj.name.empty() ? "(unnamed object)" : obj.name)).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("  #%d", obj_idx);

    const char* cat_label = swk::obj_category_label(cat);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.65f, 0.85f, 1.0f));
    ImGui::TextUnformatted(cat_label);
    ImGui::PopStyleColor();
    if (from_library) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.7f, 0.35f, 1.0f));
        ImGui::TextUnformatted(ICON_FA_LINK " template");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This object is linked to a scene template — components are inherited.\nUse 'Materialize' to copy them into the object.");
    }
    ImGui::Separator();

    const float label_w = 74.0f;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - label_w);

    // ── Template + Name (main.js PROPERTIES parity) ──────────────────
    {
        static std::string s_tpl_key;
        static std::vector<std::string> s_tpl_names;
        size_t lib_hash = 0;
        for (const auto& lib : st.scene.object_libraries) lib_hash += lib.size();
        for (const auto& lib : st.scene.external_libraries) lib_hash += lib.size();
        const std::string tkey = st.scene.filepath + "#" +
                                 std::to_string(st.scene.object_libraries.size()) + "#" +
                                 std::to_string(st.scene.external_libraries.size()) + "#" +
                                 std::to_string(lib_hash);
        if (tkey != s_tpl_key) {
            s_tpl_key = tkey;
            s_tpl_names.clear();
            for (const auto& t : av::scene_list_templates(st.scene))
                s_tpl_names.push_back(t.name);
        }
        ImGui::PushItemWidth(-1.0f);   // full-width combo, per npm.md mockup
        int cur = -1;
        for (size_t i = 0; i < s_tpl_names.size(); ++i)
            if (s_tpl_names[i] == obj.template_name) { cur = (int)i; break; }
        if (ImGui::BeginCombo("Template", cur >= 0 ? s_tpl_names[cur].c_str()
                                                   : obj.template_name.c_str())) {
            for (size_t i = 0; i < s_tpl_names.size(); ++i) {
                const bool sel = (cur == (int)i);
                if (ImGui::Selectable(s_tpl_names[i].c_str(), sel)) {
                    if (obj.template_name != s_tpl_names[i]) {
                        snapshot_scene(st);
                        obj.template_name = s_tpl_names[i];
                        st.scene_dirty = true;
                        sync_scene_object_editor(st);
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scene template (ObjectLibrary). Template components are inherited until Materialized.");
    }

    // Name (snapshot once when the field itself is activated)
    if (ImGui::InputText("Name", st.scene_obj_name_buf, sizeof(st.scene_obj_name_buf))) {
        obj.name = st.scene_obj_name_buf;
        st.scene_dirty = true;
    }
    if (ImGui::IsItemActivated()) snapshot_scene(st);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Object identifier (shown in the scene hierarchy).");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Position (main.js Position: X / Y / Depth / Rotation / Scale) ──
    if (ImGui::CollapsingHeader("Position", ImGuiTreeNodeFlags_DefaultOpen)) {
        const float w2 = ImGui::GetContentRegionAvail().x - label_w;
        const float step = 0.5f, fast = 5.0f;
        ImGui::PushItemWidth(w2);
        ImGui::TextUnformatted("X");
        ImGui::SameLine(label_w);
        if (ImGui::DragFloat("##pos_x", &obj.pos_x, step, -99999.f, 99999.f, "%.2f")) {
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        ImGui::TextUnformatted("Y");
        ImGui::SameLine(label_w);
        if (ImGui::DragFloat("##pos_y", &obj.pos_y, step, -99999.f, 99999.f, "%.2f")) {
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        ImGui::TextUnformatted("Depth");
        ImGui::SameLine(label_w);
        if (ImGui::DragFloat("##pos_z", &obj.pos_z, 0.1f, -999.f, 999.f, "%.3f")) {
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Depth (world Z / parallax layer)");
        ImGui::TextUnformatted("Rotation");
        ImGui::SameLine(label_w);
        if (ImGui::DragFloat("##rot_y", &obj.rot_y, 0.01f, -6.2832f, 6.2832f, "%.4f")) {
            obj.rot_x = obj.rot_z = 0.0f;
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation (radians, around depth axis)");
        ImGui::TextUnformatted("Scale");
        ImGui::SameLine(label_w);
        if (ImGui::DragFloat("##scale_x", &obj.scale_x, 0.01f, 0.001f, 100.f, "%.3f")) {
            obj.scale_y = obj.scale_z = obj.scale_x;
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        ImGui::PopItemWidth();
        if (obj.template_scaling != 1.0f)
            ImGui::TextDisabled("template scale ×%.2f", obj.template_scaling);
        ImGui::Spacing();
    }

    // ── LocalAABB (Rectangle { x, y, w, h }) ──────────────────────────
    float aabb[4] = {0, 0, 0, 0};
    const bool has_aabb = parse_local_aabb(obj.local_aabb, aabb);
    if (ImGui::CollapsingHeader("LocalAABB", has_aabb ? ImGuiTreeNodeFlags_DefaultOpen
                                                      : ImGuiTreeNodeFlags_None)) {
        const float w2 = ImGui::GetContentRegionAvail().x - label_w;
        ImGui::PushItemWidth(w2);
        if (!has_aabb)
            ImGui::TextDisabled("(no AABB — editing creates one)");
        const auto store_aabb = [&]() {
            obj.local_aabb = serialize_local_aabb(aabb[0], aabb[1], aabb[2], aabb[3]);
            st.scene_dirty = true;
        };
        if (ImGui::DragFloat("X", &aabb[0], 0.1f, -99999.f, 99999.f, "%.2f"))
            store_aabb();
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        if (ImGui::DragFloat("Y", &aabb[1], 0.1f, -99999.f, 99999.f, "%.2f"))
            store_aabb();
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        if (ImGui::DragFloat("Width", &aabb[2], 0.1f, -99999.f, 99999.f, "%.2f"))
            store_aabb();
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        if (ImGui::DragFloat("Height", &aabb[3], 0.1f, -99999.f, 99999.f, "%.2f"))
            store_aabb();
        if (ImGui::IsItemActivated()) snapshot_scene(st);
        if (has_aabb && ImGui::SmallButton(ICON_FA_TRASH " Clear")) {
            snapshot_scene(st);
            obj.local_aabb.clear();
            st.scene_dirty = true;
        }
        ImGui::PopItemWidth();
        ImGui::Spacing();
    }

    // ── Hidden (OFF / gray toggle) ────────────────────────────────────
    {
        const bool hidden = obj.hidden;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Hidden");
        ImGui::SameLine(label_w);
        const ImVec4 on_col  = ImVec4(0.55f, 0.65f, 0.85f, 1.0f);
        const ImVec4 off_col = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, hidden ? on_col : off_col);
        if (ImGui::Button(hidden ? ICON_FA_EYE " ON" : ICON_FA_EYE_SLASH " OFF")) {
            snapshot_scene(st);
            obj.hidden = !obj.hidden;
            st.scene_dirty = true;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hidden ? "object hidden in scene" : "visible in scene");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Model / mesh statistics ─────────────────────────────────────
    int verts = 0, tris = 0; bool loaded = false;
    object_stats_for(st, obj_idx, &verts, &tris, &loaded);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(ICON_FA_CUBE " Mesh Statistics",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!obj.mesh_name.empty())
            ImGui::TextDisabled("Model: %s", obj.mesh_name.c_str());
        auto it = st.scene_model_cache.find(obj.mesh_name);
        if (!obj.mesh_name.empty() && it != st.scene_model_cache.end()) {
            const auto& m = it->second;
            bool skinned = false;
            for (const auto& mesh : m.meshes)
                if (mesh.bones_per_vertex > 0) { skinned = true; break; }
            ImGui::Text("%s", skinned ? ICON_FA_BOLT " Skinned (skeletal) model" : "Rigid model");
            ImGui::Text("Nodes: %d    Meshes: %d", (int)m.nodes.size(), (int)m.meshes.size());
            ImGui::Text("Verts: %d    Tris: %d", m.total_vertices, m.total_faces);
            ImGui::Text("Materials: %d    Frames: %d", (int)m.materials.size(), m.num_frames);
            if (m.texture_filenames.empty() == false)
                ImGui::TextDisabled("Textures: %zu", m.texture_filenames.size());
            ImGui::TextDisabled("Bounds  X %.1f..%.1f  Y %.1f..%.1f  Z %.1f..%.1f",
                                m.min_x, m.max_x, m.min_y, m.max_y, m.min_z, m.max_z);
            ImGui::TextDisabled("Center (%.1f, %.1f, %.1f)  radius %.1f",
                                m.center_x, m.center_y, m.center_z, m.radius);
        } else if (!obj.mesh_name.empty()) {
            ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.35f, 1.0f),
                               ICON_FA_TRIANGLE_EXCLAMATION " Model file not resolved");
        }
        if (!obj.ground_meshes.empty()) {
            int gv = 0, gt = 0;
            for (const auto& gm : obj.ground_meshes) { gv += gm.num_vertices; gt += gm.num_faces; }
            ImGui::Separator();
            ImGui::Text("Embedded ground meshes: %d", (int)obj.ground_meshes.size());
            ImGui::Text("  verts %d   tris %d", gv, gt);
        }
        if (!obj.texture_name.empty())
            ImGui::TextDisabled("Texture: %s", obj.texture_name.c_str());
        if (loaded) {
            ImGui::Spacing();
            ImGui::TextColored(g_theme.accent, "Totals: %d verts, %d tris", verts, tris);
        }
    }

    // ── Components (badges + editable fields) ───────────────────────
    ImGui::Spacing();
    const int ncomps = (int)obj.components.size();
    const int ninher = (int)obj.resolved_components.size() - ncomps;
    const std::string comp_title = std::string(ICON_FA_PUZZLE_PIECE " Components (") +
                                   std::to_string(ncomps) +
                                   (ninher > 0 ? " + " + std::to_string(ninher) + " inherited)" : ")");
    if (ImGui::CollapsingHeader(comp_title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ncomps == 0 && ninher == 0) {
            ImGui::TextDisabled("No components.");
        } else {
            // PROPERTIES search filter (matches component type or field names)
            std::string pq(st.obj_prop_search_buf);
            for (auto& c : pq) c = (char)tolower((unsigned char)c);
            const bool prop_filtering = !pq.empty();

            // Category badges — one chip per distinct component category.
            std::map<swk::ObjCategory, int> badge_counts;
            for (const auto& c : obj.resolved_components.empty()
                                     ? obj.components : obj.resolved_components)
                badge_counts[swk::category_for_component(c.type_name)]++;
            for (const auto& kv : badge_counts) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.75f, 0.9f, 1.0f));
                ImGui::TextUnformatted(
                    (std::string(swk::obj_category_icon(kv.first)) + " " +
                     swk::obj_category_label(kv.first) + "  x" + std::to_string(kv.second)).c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::Separator();

            // Per-component fields (read/edit). Inherited components shown dim.
            for (size_t ci = 0; ci < obj.resolved_components.size(); ++ci) {
                const bool inherited = ci >= obj.components.size();
                const auto& c = obj.resolved_components[ci];
                ImGui::PushID((int)(ci + 1));
                const std::string clabel = (c.type_name.empty() ? "(unnamed)" : c.type_name) +
                                           (inherited ? "  (template)" : "");
                // PROPERTIES search: show the node open when its type matches.
                bool filtered_match = false;
                if (prop_filtering) {
                    std::string hay = c.type_name;
                    for (auto& cc : hay) cc = (char)tolower((unsigned char)cc);
                    filtered_match = hay.find(pq) != std::string::npos;
                }
                const bool node_open =
                    filtered_match ? ImGui::TreeNodeEx(clabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)
                                   : ImGui::TreeNode(clabel.c_str());
                if (node_open) {
                    ImGui::TextDisabled("type id %d  |  %zu bytes%s", c.type_id, c.raw_data.size(),
                                        inherited ? "  — inherited from template" : "");
                    const auto fields = av::scene_component_fields(c);
                    if (!fields.empty()) {
                        for (auto field : fields) {
                            ImGui::PushID(static_cast<int>(field.field_number * 100 + field.occurrence));
                            bool changed = false;
                            if (field.is_message) {
                                ImGui::TextDisabled("%s: <%s> (%zu bytes)", field.name.c_str(),
                                                    field.class_name.c_str(), field.bytes_value.size());
                            } else if (field.wire_type == proto::WIRE_VARINT) {
                                int value = static_cast<int>(field.varint_value);
                                if (ImGui::InputInt(field.name.c_str(), &value)) {
                                    field.varint_value = static_cast<uint64_t>(std::max(0, value));
                                    changed = true;
                                }
                            } else if (field.wire_type == proto::WIRE_I32) {
                                float value = field.float_value;
                                if (ImGui::DragFloat(field.name.c_str(), &value, 0.01f)) {
                                    field.float_value = value;
                                    changed = true;
                                }
                            } else if (field.wire_type == proto::WIRE_I64) {
                                double value = field.double_value;
                                if (ImGui::InputDouble(field.name.c_str(), &value)) {
                                    field.double_value = value;
                                    changed = true;
                                }
                            } else if (field.bytes_value.find('\0') == std::string::npos &&
                                       field.bytes_value.size() < 4096) {
                                char value[4096];
                                snprintf(value, sizeof(value), "%s", field.bytes_value.c_str());
                                if (ImGui::InputText(field.name.c_str(), value, sizeof(value))) {
                                    field.bytes_value = value;
                                    changed = true;
                                }
                            } else {
                                ImGui::TextDisabled("%s: <%zu bytes>", field.name.c_str(), field.bytes_value.size());
                            }
                            if (changed && !inherited) {
                                snapshot_scene(st);
                                if (av::scene_set_component_field(obj.components[ci], field)) {
                                    av::scene_refresh(st.scene);
                                    st.scene_dirty = true;
                                }
                            }
                            ImGui::PopID();
                        }
                    } else {
                        ImGui::TextDisabled("Component payload is empty or unavailable.");
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
    }

    // ── Scene metadata (spawn / portal / background / aabb) ─────────
    if (obj.is_spawn_point || obj.is_portal || !obj.background_name.empty() ||
        !obj.local_aabb.empty() || !obj.onload.empty()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(ICON_FA_CIRCLE_INFO " Scene Metadata",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (obj.is_spawn_point)
                ImGui::Text("%s Spawn point — facing %s", ICON_FA_CROSSHAIR,
                            obj.spawn_facing > 0 ? "right" : "left");
            if (obj.is_portal) {
                ImGui::Text(ICON_FA_DOOR_OPEN " Portal → %s%s%s",
                            obj.portal_destination.c_str(),
                            obj.portal_spawn_point.empty() ? "" : " @ ",
                            obj.portal_spawn_point.c_str());
                ImGui::TextDisabled("%s", obj.portal_tap_to_enter ? "tap-to-enter enabled" : "auto-enter");
            }
            if (!obj.background_name.empty())
                ImGui::Text(ICON_FA_IMAGE " Background layer: %s", obj.background_name.c_str());
            if (!obj.local_aabb.empty()) {
                float aa[4] = {0, 0, 0, 0};
                if (parse_local_aabb(obj.local_aabb, aa))
                    ImGui::TextDisabled("LocalAabb: X %.2f  Y %.2f  W %.2f  H %.2f", aa[0], aa[1], aa[2], aa[3]);
                else
                    ImGui::TextDisabled("LocalAabb: %zu bytes", obj.local_aabb.size());
            }
            if (!obj.onload.empty())
                ImGui::TextDisabled("OnLoad script: %zu bytes", obj.onload.size());
        }
    }

    ImGui::PopItemWidth();
    ImGui::Separator();

    // ── Actions ─────────────────────────────────────────────────────
    if (ImGui::Button(ICON_FA_CROSSHAIR " Frame"))
        frame_scene_selection(st);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame the selection in the Visual viewport");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY " Duplicate")) {
        snapshot_scene(st);
        size_t new_idx = 0;
        if (av::scene_duplicate_object(st.scene, (size_t)obj_idx, &new_idx)) {
            select_scene_object(st, (int)new_idx);
            st.scene_dirty = true;
            st.status_msg = "Duplicated object.";
        }
    }
    if (from_library) {
        if (ImGui::Button(ICON_FA_LINK " Materialize Template")) {
            snapshot_scene(st);
            obj.components = obj.resolved_components;   // copy inherited components inline
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
            st.status_msg = "Materialized template components into '" + obj.template_name + "'.";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy the template's components into this object so it no longer depends on the library (main.js 'materializeComponents').");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    if (ImGui::Button(ICON_FA_TRASH " Delete")) {
        snapshot_scene(st);
        const int removed = obj_idx;
        av::scene_delete_object(st.scene, (size_t)obj_idx);
        st.scene_selection.clear();
        st.selected_object = (removed < (int)st.scene.objects.size()) ? removed : (int)st.scene.objects.size() - 1;
        sync_scene_object_editor(st);
        st.scene_dirty = true;
        st.status_msg = "Deleted object #" + std::to_string(removed) + ".";
    }
    ImGui::PopStyleColor();
}

// ── Objects panel (Tree tab) — categorized browser + inspector ───────────
// ── Objects panel (Tree tab) — main.js OBJECTS / PROPERTIES layout ──────
// Vertical split: OBJECTS list (search + flat rows, blue selection) on top,
// PROPERTIES inspector (template, name, position, aabb, hidden) below.
static void draw_scene_inspector(ViewerState& st) {
    if (st.scene.objects.empty()) {
        ImGui::TextDisabled("No objects in scene.");
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 5));
    const ImGuiIO& io = ImGui::GetIO();
    const float avail_y = ImGui::GetContentRegionAvail().y;
    const float list_h = std::max(120.0f, avail_y * 0.46f);

    // ── OBJECTS (top) ──────────────────────────────────────────────────
    ImGui::BeginChild("##ObjectsPanel", ImVec2(0, list_h), ImGuiChildFlags_Borders);
    {
        ImGui::TextDisabled(ICON_FA_OBJECT_GROUP " OBJECTS  (%d)", (int)st.scene.objects.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PLUS " Add")) st.obj_browser_open = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add models (.pod), ground meshes (.swdm) or game-library templates.");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_CROSSHAIR " Frame"))
            frame_scene_selection(st);
        ImGui::SameLine();
        if (ImGui::SmallButton(st.obj_group_by_cat ? ICON_FA_LIST : ICON_FA_LAYER_GROUP))
            st.obj_group_by_cat = !st.obj_group_by_cat;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(st.obj_group_by_cat ? "Grouped by category — click for flat list"
                                                  : "Flat list — click to group by category");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##obj_search", ICON_FA_MAGNIFYING_GLASS " Search...",
                                 st.obj_search_buf, sizeof(st.obj_search_buf));
        ImGui::Separator();

        std::string q(st.obj_search_buf);
        for (auto& c : q) c = (char)tolower((unsigned char)c);

        // Row renderer shared by flat + grouped modes.
        auto row = [&](int i) {
            auto& o = st.scene.objects[i];
            const bool in_sel = is_scene_selected(st, i);
            const swk::ObjCategory cat = swk::classify_object(o);
            ImGui::PushID(i + 1);

            const char* eye = o.hidden ? ICON_FA_EYE_SLASH : ICON_FA_EYE;
            const ImVec4 eye_col = o.hidden ? ImVec4(0.42f, 0.42f, 0.42f, 1.0f)
                                           : ImVec4(0.62f, 0.68f, 0.75f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, eye_col);
            if (ImGui::SmallButton(eye)) {
                snapshot_scene(st);
                o.hidden = !o.hidden;
                st.scene_dirty = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(o.hidden ? "Show object" : "Hide object");
            ImGui::SameLine();

            const std::string row_label =
                std::string(swk::obj_category_icon(cat)) + "  " +
                (o.name.empty() ? "(unnamed)" : o.name) + "##o" + std::to_string(i);
            const bool clicked = ImGui::Selectable(row_label.c_str(), in_sel);
            if (clicked) {
                if (io.KeyCtrl) toggle_scene_selection(st, i);
                else select_scene_object(st, i);
                if (io.MouseDoubleClicked[0]) {
                    switch_scene_tab(st, 1);
                    frame_scene_selection(st);
                }
            }
            if (ImGui::IsItemHovered()) {
                std::string tip = "#" + std::to_string(i);
                if (!o.template_name.empty()) tip += "  template: " + o.template_name;
                if (!o.mesh_name.empty())  tip += "  model: " + o.mesh_name;
                if (in_sel && st.scene_selection.size() > 1) tip += "  (multi-select)";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (!o.template_name.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.48f, 0.52f, 1.0f));
                ImGui::TextUnformatted(o.template_name.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        };

        static bool s_cat_open[11] = {true, true, true, true, true, true,
                                      true, true, true, true, true};
        static const swk::ObjCategory kOrder[] = {
            swk::ObjCategory::Enemies, swk::ObjCategory::Entities,
            swk::ObjCategory::Items, swk::ObjCategory::Geometry,
            swk::ObjCategory::Effects, swk::ObjCategory::Lighting,
            swk::ObjCategory::Controllers, swk::ObjCategory::Audio,
            swk::ObjCategory::Portals, swk::ObjCategory::Utility,
            swk::ObjCategory::Other
        };

        if (ImGui::BeginChild("##ObjectsList", ImVec2(0, 0))) {
            if (!st.obj_group_by_cat) {
                for (int i = 0; i < (int)st.scene.objects.size(); ++i) {
                    const auto& o = st.scene.objects[i];
                    if (!q.empty()) {
                        std::string hay = o.name + " " + o.template_name + " " + o.mesh_name;
                        for (auto& c : hay) c = (char)tolower((unsigned char)c);
                        if (hay.find(q) == std::string::npos) continue;
                    }
                    row(i);
                }
            } else {
                for (int ci = 0; ci < 11; ++ci) {
                    const swk::ObjCategory cat = kOrder[ci];
                    std::vector<int> rows;
                    for (int i = 0; i < (int)st.scene.objects.size(); ++i) {
                        const auto& o = st.scene.objects[i];
                        if (swk::classify_object(o) != cat) continue;
                        if (!q.empty()) {
                            std::string hay = o.name + " " + o.template_name + " " + o.mesh_name;
                            for (auto& c : hay) c = (char)tolower((unsigned char)c);
                            if (hay.find(q) == std::string::npos) continue;
                        }
                        rows.push_back(i);
                    }
                    if (rows.empty()) continue;
                    if (ImGui::TreeNodeEx((void*)(intptr_t)(ci + 5000),
                                          s_cat_open[ci] ? ImGuiTreeNodeFlags_DefaultOpen
                                                         : ImGuiTreeNodeFlags_None,
                                          "%s %s  (%d)", swk::obj_category_icon(cat),
                                          swk::obj_category_label(cat), (int)rows.size())) {
                        s_cat_open[ci] = true;
                        for (int idx : rows) row(idx);
                        ImGui::TreePop();
                    } else {
                        s_cat_open[ci] = false;
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── PROPERTIES (bottom) ────────────────────────────────────────────
    ImGui::BeginChild("##PropertiesPanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        ImGui::TextDisabled(ICON_FA_SLIDERS " PROPERTIES");
        ImGui::SameLine();
        if (st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
            auto& so = st.scene.objects[st.selected_object];
            ImGui::TextColored(g_theme.accent, "%s",
                               (so.name.empty() ? "(unnamed)" : so.name).c_str());
        } else {
            ImGui::TextDisabled("no selection");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##prop_search", ICON_FA_MAGNIFYING_GLASS " Filter components...",
                                 st.obj_prop_search_buf, sizeof(st.obj_prop_search_buf));
        ImGui::Separator();
        if (ImGui::BeginChild("##PropertiesScroll", ImVec2(0, 0))) {
            draw_object_inspector(st);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ============================================================================
// UI: Center panel — Audio player
// ============================================================================

static void draw_audio_player(ViewerState& st) {
    auto& as = av::audio_get_state();
    if (!as.loaded) {
        ImGui::TextDisabled("No audio loaded.");
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float waveform_h = avail.y - 90.0f;
    if (waveform_h < 50.0f) waveform_h = 50.0f;

    if (!as.waveform.empty()) {
        ImVec2 wf_pos = ImGui::GetCursorScreenPos();
        ImGui::PlotLines("##waveform", as.waveform.data(), (int)as.waveform.size(),
                         0, nullptr, -1.0f, 1.0f, ImVec2(avail.x, waveform_h));

        if (as.duration > 0.0f) {
            float progress = as.position / as.duration;
            float line_x = wf_pos.x + progress * avail.x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(line_x, wf_pos.y),
                        ImVec2(line_x, wf_pos.y + waveform_h),
                        IM_COL32(71, 114, 179, 255), 2.0f); // Blender highlight blue
        }
    } else {
        ImGui::TextDisabled("No waveform data available.");
        ImGui::Dummy(ImVec2(0, waveform_h));
    }

    ImGui::Spacing();
    float button_w = 40.0f;

    // Play/Pause
    if (as.playing && !as.paused) {
        if (ImGui::Button(ICON_FA_PAUSE, ImVec2(button_w, 0)))
            av::audio_pause();
    } else {
        if (ImGui::Button(ICON_FA_PLAY, ImVec2(button_w, 0)))
            av::audio_play();
    }
    ImGui::SameLine();

    // Stop
    if (ImGui::Button(ICON_FA_STOP, ImVec2(button_w, 0)))
        av::audio_stop();
    ImGui::SameLine();

    ImGui::Text("%s / %s", format_time(as.position).c_str(), format_time(as.duration).c_str());
    ImGui::SameLine();

    float seek_w = avail.x - 350.0f;
    if (seek_w < 100.0f) seek_w = 100.0f;
    ImGui::PushItemWidth(seek_w);
    float pos = as.position;
    if (ImGui::SliderFloat("##seek", &pos, 0.0f, as.duration, "%.1fs")) {
        av::audio_seek(pos);
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();

    ImGui::PushItemWidth(100.0f);
    float vol = as.volume;
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "Vol:%.0f%%")) {
        av::audio_set_volume(vol);
    }
    ImGui::PopItemWidth();
}

static bool load_scene_model_to_cache(ViewerState& st, const std::string& mesh_name,
                                      const std::string& scene_dir_path,
                                      const std::string& base_hint) {
    if (st.scene_model_cache.count(mesh_name)) return true;

    fs::path scene_dir(scene_dir_path);
    const fs::path data_dir = fs::path(expand_home("~/.local/share/swordigo-desktop/assets"));
    std::vector<fs::path> search_paths = {
        scene_dir / mesh_name,
        scene_dir / (mesh_name + ".pod"),
        scene_dir / (mesh_name + ".POD"),
        scene_dir.parent_path() / mesh_name,
        scene_dir.parent_path() / (mesh_name + ".pod"),
        scene_dir.parent_path() / (mesh_name + ".POD"),
        scene_dir.parent_path() / "models" / mesh_name,
        scene_dir.parent_path() / "models" / (mesh_name + ".pod"),
        scene_dir.parent_path() / "models" / (mesh_name + ".POD"),
        scene_dir.parent_path() / "resources" / mesh_name,
        scene_dir.parent_path() / "resources" / (mesh_name + ".pod"),
        scene_dir.parent_path() / "resources" / (mesh_name + ".POD"),
        data_dir / mesh_name,
        data_dir / (mesh_name + ".pod"),
        data_dir / (mesh_name + ".POD"),
        data_dir / "resources" / mesh_name,
        data_dir / "resources" / (mesh_name + ".pod"),
        data_dir / "resources" / (mesh_name + ".POD"),
        data_dir / "models" / mesh_name,
        data_dir / "models" / (mesh_name + ".pod"),
        data_dir / "models" / (mesh_name + ".POD"),
        fs::path(g_assets_dir) / "resources" / mesh_name,
        fs::path(g_assets_dir) / "resources" / (mesh_name + ".pod"),
        fs::path(g_assets_dir) / "resources" / (mesh_name + ".POD"),
        fs::path(g_assets_dir) / "resources" / "models" / mesh_name,
        fs::path(g_assets_dir) / "resources" / "models" / (mesh_name + ".pod"),
        fs::path(g_assets_dir) / "resources" / "models" / (mesh_name + ".POD")
    };

    fs::path resolved_path;
    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            resolved_path = path;
            break;
        }
    }

    if (resolved_path.empty()) {
        for (const fs::path& root : {scene_dir, scene_dir.parent_path(),
                                    data_dir / "resources", data_dir,
                                    fs::path(g_assets_dir) / "resources"}) {
            resolved_path = find_pod_resource(root, mesh_name);
            if (!resolved_path.empty()) break;
        }
    }

    if (resolved_path.empty()) {
        fprintf(stderr, "[RubyDebug] model '%s' NOT FOUND in any search path\n", mesh_name.c_str());
        return false;
    }

    // Animation-only PODs (npc_stand, snowball_land, ...) carry just the
    // keyframe streams — merge them with the scene object's base mesh
    // (base_hint, e.g. knight/shadowblob) so the monster actually renders
    // instead of showing as a dot.
    av::PODModel model = av::pod_load(resolved_path.string(), base_hint);
    if (model.meshes.empty()) {
        fprintf(stderr, "[RubyDebug] model '%s' found at %s but pod_load returned no meshes\n",
                mesh_name.c_str(), resolved_path.string().c_str());
        return false;
    }

    std::vector<av::GPUMesh> gpu_meshes;
    for (auto& mesh : model.meshes) {
        // PODs normally carry normals; a few converted/converted-tool models
        // don't. Recover them from the triangle winding so lighting is never
        // silently Y-up.
        std::vector<float> computed_normals;
        if (mesh.normals.empty() && !mesh.positions.empty() &&
            mesh.num_vertices == (int)(mesh.positions.size() / 3)) {
            swk::compute_smooth_normals(mesh.positions, mesh.indices,
                                        computed_normals);
        }
        const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
        const float*    nrm = !computed_normals.empty() ? computed_normals.data()
                          : (mesh.normals.empty() ? nullptr : mesh.normals.data());
        const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
        const uint32_t* idx = mesh.indices.empty()    ? nullptr : mesh.indices.data();
        av::GPUMesh gm = av::upload_mesh(pos, nrm, uv, mesh.num_vertices,
                                          idx, (int)mesh.indices.size());
        gpu_meshes.push_back(gm);
    }

    std::vector<GLuint> model_textures;
    fs::path model_dir = resolved_path.parent_path();
    for (const auto& tex_name : model.texture_filenames) {
        if (tex_name.empty()) {
            model_textures.push_back(0);
            continue;
        }
        fs::path tex_path = model_dir / tex_name;
        std::string stem = tex_path.stem().string();

        std::vector<fs::path> candidates = {
            tex_path,
            model_dir / (stem + "_2x.tex.png"),
            model_dir / (stem + ".tex.png"),
            model_dir / (stem + "_2x.pvr"),
            model_dir / (stem + ".pvr"),
            model_dir / (stem + "_2x.tex"),
            model_dir / (stem + ".tex"),
            model_dir / (stem + "_2x.png"),
            model_dir / (stem + ".png")
        };

        GLuint tex_id = 0;
        for (const auto& cand : candidates) {
            if (fs::exists(cand)) {
                tex_id = load_texture_file(cand.string());
                if (tex_id) break;
            }
        }
        model_textures.push_back(tex_id);
    }

    if (model_textures.empty() || (model_textures.size() == 1 && model_textures[0] == 0)) {
        std::string stem = resolved_path.stem().string();
        std::vector<fs::path> candidates = {
            model_dir / (stem + "_2x.pvr"),
            model_dir / (stem + ".pvr"),
            model_dir / (stem + "_2x.png"),
            model_dir / (stem + ".png")
        };
        GLuint tex_id = 0;
        for (const auto& cand : candidates) {
            if (fs::exists(cand)) {
                std::string cpath = cand.string();
                std::string ext = cand.extension().string();
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (ext == ".pvr") {
                    tex_id = pvr_load_texture(cpath.c_str(), nullptr, nullptr);
                } else {
                    SDL_Surface* surf = IMG_Load(cpath.c_str());
                    if (surf) {
                        SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
                        SDL_DestroySurface(surf);
                        if (conv) {
                            glGenTextures(1, &tex_id);
                            glBindTexture(GL_TEXTURE_2D, tex_id);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, conv->w, conv->h, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, conv->pixels);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            SDL_DestroySurface(conv);
                        }
                    }
                }
                if (tex_id) break;
            }
        }
        if (tex_id) {
            if (model_textures.empty()) model_textures.push_back(tex_id);
            else model_textures[0] = tex_id;
        }
    }

    st.scene_model_cache[mesh_name] = std::move(model);
    st.scene_gpu_mesh_cache[mesh_name] = std::move(gpu_meshes);
    st.scene_texture_cache[mesh_name] = std::move(model_textures);

    return true;
}

// Load the BackgroundComponent texture (a bare stem like "graveyardback" that
// resolves to graveyardback_2x.tex.png / .pvr next to the scene or in the
// resources root). The background renders as a camera-following textured quad.
static GLuint load_scene_background_texture(ViewerState& st, const std::string& bg_name,
                                            const std::string& scene_dir_path) {
    if (bg_name.empty()) return 0;
    const auto it = st.scene_background_textures.find(bg_name);
    if (it != st.scene_background_textures.end()) return it->second;

    fs::path scene_dir(scene_dir_path);
    const fs::path data_dir = fs::path(expand_home("~/.local/share/swordigo-desktop/assets"));
    std::vector<fs::path> search_dirs = {
        scene_dir,
        scene_dir.parent_path(),
        scene_dir.parent_path() / "resources",
        data_dir / "resources",
        data_dir / "background",
        data_dir / "resources" / "background",
    };
    // Swordigo packs background art with a @2x suffix on the stem.
    static const char* suffixes[] = {
        "_2x.tex.png", ".tex.png", "_2x.pvr", ".pvr", "_2x.tex", ".tex", "_2x.png", ".png"
    };

    GLuint tex = 0;
    for (const auto& dir : search_dirs) {
        for (const char* suf : suffixes) {
            fs::path candidate = dir / (bg_name + suf);
            std::error_code ec;
            if (!fs::is_regular_file(candidate, ec)) continue;
            tex = load_texture_file(candidate.string());
            if (tex) {
                fprintf(stderr, "[RubyDebug] background texture: %s\n", candidate.string().c_str());
                break;
            }
        }
        if (tex) break;
    }
    if (!tex)
        fprintf(stderr, "[RubyDebug] background texture '%s' NOT FOUND\n", bg_name.c_str());

    st.scene_background_textures[bg_name] = tex;
    return tex;
}

static void upload_scene_ground_meshes(ViewerState& st, const std::string& scene_dir_path) {
    // GPUMesh is a plain struct (no RAII): free old buffers BEFORE dropping
    // the handles, or every re-upload (live inline mesh edit re-applies at
    // ~5 Hz) leaks VAO/VBO/EBO + textures. Mirrors the reset path / resync.
    for (auto& vec : st.scene_ground_gpu_meshes)
        for (auto& m : vec) av::free_mesh(m);
    for (auto& vec : st.scene_ground_textures)
        for (auto tex : vec) if (tex) glDeleteTextures(1, &tex);
    st.scene_ground_gpu_meshes.clear();
    st.scene_ground_textures.clear();

    st.scene_ground_gpu_meshes.resize(st.scene.objects.size());
    st.scene_ground_textures.resize(st.scene.objects.size());
    st.scene_ground_tex_names.resize(st.scene.objects.size());

    fs::path scene_dir(scene_dir_path);

    for (size_t idx = 0; idx < st.scene.objects.size(); ++idx) {
        const auto& obj = st.scene.objects[idx];
        for (size_t gi = 0; gi < obj.ground_meshes.size(); ++gi) {
        const auto& mesh = obj.ground_meshes[gi];
        // Embedded ground meshes often ship WITHOUT baked normals. Uploading
        // them as-is lights every surface as Y-up (flat stone, no form). When
        // the parser produced no normals we derive smooth face normals from
        // the triangle winding so the Lambert lighting reads the geometry.
        std::vector<float> computed_normals;
        if (mesh.normals.empty() && !mesh.positions.empty() &&
            mesh.num_vertices == (int)(mesh.positions.size() / 3)) {
            swk::compute_smooth_normals(mesh.positions, mesh.indices,
                                        computed_normals);
        }
        const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
        const float*    nrm = !computed_normals.empty() ? computed_normals.data()
                          : (mesh.normals.empty() ? nullptr : mesh.normals.data());
        const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
        const uint32_t* idx_ptr = mesh.indices.empty() ? nullptr : mesh.indices.data();
        av::GPUMesh gm = av::upload_mesh(pos, nrm, uv, mesh.num_vertices,
                                          idx_ptr, (int)mesh.indices.size());
        st.scene_ground_gpu_meshes[idx].push_back(gm);
        GLuint tex_id = load_ground_mesh_texture(scene_dir,
                                                 obj.ground_mesh_textures[gi]);
        st.scene_ground_textures[idx].push_back(tex_id);
        st.scene_ground_tex_names[idx].push_back(
            gi < obj.ground_mesh_textures.size() ? obj.ground_mesh_textures[gi]
                                                 : std::string());
        }
    }
}

// Fully resync ONE object's ground-mesh GPU buffers (grow/shrink safe). Used
// by the inline 2D editor's live preview so dragging a vertex never churns
// every object's buffers in the scene.
static void resync_object_ground_meshes(ViewerState& st, int idx) {
    if (idx < 0 || idx >= (int)st.scene.objects.size()) return;
    if (idx >= (int)st.scene_ground_gpu_meshes.size()) return;
    // Snapshot old textures + names BEFORE clearing: the live preview regens
    // ~10x/sec and unchanged texture slots must reuse their GL id instead of
    // re-decoding + re-uploading from disk every tick (visible hitches).
    const std::vector<GLuint> old_tex =
        (idx < (int)st.scene_ground_textures.size()) ? st.scene_ground_textures[idx]
                                                     : std::vector<GLuint>();
    const std::vector<std::string> old_nms =
        (idx < (int)st.scene_ground_tex_names.size()) ? st.scene_ground_tex_names[idx]
                                                      : std::vector<std::string>();
    std::vector<bool> reused(old_tex.size(), false);

    for (auto& m : st.scene_ground_gpu_meshes[idx]) av::free_mesh(m);
    st.scene_ground_gpu_meshes[idx].clear();
    st.scene_ground_textures[idx].clear();
    if (idx < (int)st.scene_ground_tex_names.size()) st.scene_ground_tex_names[idx].clear();

    const auto& obj = st.scene.objects[idx];
    const fs::path scene_dir = fs::path(st.scene.filepath).parent_path();
    for (size_t gi = 0; gi < obj.ground_meshes.size(); ++gi) {
        const auto& mesh = obj.ground_meshes[gi];
        std::vector<float> computed_normals;
        if (mesh.normals.empty() && !mesh.positions.empty() &&
            mesh.num_vertices == (int)(mesh.positions.size() / 3)) {
            swk::compute_smooth_normals(mesh.positions, mesh.indices,
                                        computed_normals);
        }
        const float* nrm = !computed_normals.empty() ? computed_normals.data()
                          : (mesh.normals.empty() ? nullptr : mesh.normals.data());
        av::GPUMesh gm = av::upload_mesh(mesh.positions.empty() ? nullptr : mesh.positions.data(),
                                          nrm,
                                          mesh.uvs.empty() ? nullptr : mesh.uvs.data(),
                                          mesh.num_vertices,
                                          mesh.indices.empty() ? nullptr : mesh.indices.data(),
                                          (int)mesh.indices.size());
        const std::string name = gi < obj.ground_mesh_textures.size()
                                     ? obj.ground_mesh_textures[gi] : std::string();
        GLuint tex = 0;
        if (gi < old_nms.size() && gi < old_tex.size() &&
            old_nms[gi] == name && old_tex[gi]) {
            tex = old_tex[gi];          // texture unchanged — reuse, don't reload
            reused[gi] = true;
        }
        if (!tex) tex = load_ground_mesh_texture(scene_dir, name);
        gm.texture_id = tex;
        st.scene_ground_gpu_meshes[idx].push_back(gm);
        st.scene_ground_textures[idx].push_back(tex);
        if (idx < (int)st.scene_ground_tex_names.size())
            st.scene_ground_tex_names[idx].push_back(name);
    }
    // Delete only the textures that were NOT reused this tick.
    for (size_t i = 0; i < old_tex.size(); ++i)
        if (old_tex[i] && !reused[i]) glDeleteTextures(1, &old_tex[i]);
}

// Resolve a ground-mesh texture file to a GL texture id. Probes the scene
// dir, its parent, the textures/models dirs and the global assets dir, with
// the _2x/.tex/.pvr suffix variants — shared by the full scene upload and the
// single-object live resync below.
static GLuint load_ground_mesh_texture(const fs::path& scene_dir,
                                       const std::string& tex_name) {
    GLuint tex_id = 0;
    if (tex_name.empty()) return 0;
    fs::path tex_path = scene_dir / tex_name;
    std::string stem = tex_path.stem().string();
    const fs::path data_dir = fs::path(expand_home("~/.local/share/swordigo-desktop/assets"));
    std::vector<fs::path> candidates = {
        tex_path,
        scene_dir.parent_path() / tex_name,
        scene_dir.parent_path() / "textures" / tex_name,
        scene_dir.parent_path() / "models" / tex_name,
        fs::path(g_assets_dir) / "resources" / tex_name,
        data_dir / "resources" / tex_name,
        data_dir / tex_name,
        scene_dir / (stem + "_2x.tex.png"),  scene_dir / (stem + ".tex.png"),
        scene_dir / (stem + "_2x.pvr"),      scene_dir / (stem + ".pvr"),
        scene_dir / (stem + "_2x.tex"),      scene_dir / (stem + ".tex"),
        scene_dir / (stem + "_2x.png"),      scene_dir / (stem + ".png"),
        scene_dir.parent_path() / (stem + "_2x.tex.png"),
        scene_dir.parent_path() / (stem + ".tex.png"),
        scene_dir.parent_path() / (stem + "_2x.pvr"),
        scene_dir.parent_path() / (stem + ".pvr"),
        scene_dir.parent_path() / (stem + "_2x.tex"),
        scene_dir.parent_path() / (stem + ".tex"),
        scene_dir.parent_path() / (stem + "_2x.png"),
        scene_dir.parent_path() / (stem + ".png")
    };
    for (const auto& cand : candidates) {
        if (fs::exists(cand)) {
            tex_id = load_texture_file(cand.string());
            if (tex_id) break;
        }
    }
    return tex_id;
}

// Load textures for parsed WaterMesh sheets (parallel to st.scene.waters).
// Probes the same candidate dirs/suffixes as ground-mesh textures so
// "water" resolves to water_2x.pvr, etc.
static void upload_scene_waters(ViewerState& st, const std::string& scene_dir_path) {
    for (auto tex : st.scene_water_textures)
        if (tex) glDeleteTextures(1, &tex);
    st.scene_water_textures.clear();

    fs::path scene_dir(scene_dir_path);
    const fs::path data_dir = fs::path(expand_home("~/.local/share/swordigo-desktop/assets"));
    for (const auto& w : st.scene.waters) {
        GLuint tex_id = 0;
        if (!w.texture.empty()) {
            fs::path tex_path = scene_dir / w.texture;
            std::string stem = tex_path.stem().string();
            std::vector<fs::path> candidates = {
                tex_path,
                scene_dir / (stem + "_2x.tex.png"), scene_dir / (stem + ".tex.png"),
                scene_dir / (stem + "_2x.pvr"),    scene_dir / (stem + ".pvr"),
                scene_dir / (stem + "_2x.tex"),    scene_dir / (stem + ".tex"),
                scene_dir / (stem + "_2x.png"),    scene_dir / (stem + ".png"),
                scene_dir.parent_path() / (stem + "_2x.tex.png"),
                scene_dir.parent_path() / (stem + ".tex.png"),
                scene_dir.parent_path() / (stem + "_2x.pvr"),
                scene_dir.parent_path() / (stem + ".pvr"),
                scene_dir.parent_path() / (stem + "_2x.tex"),
                scene_dir.parent_path() / (stem + ".tex"),
                scene_dir.parent_path() / (stem + "_2x.png"),
                scene_dir.parent_path() / (stem + ".png"),
                data_dir / "resources" / (stem + "_2x.pvr"),
                data_dir / "resources" / (stem + ".pvr"),
                data_dir / "resources" / (stem + "_2x.tex.png"),
                data_dir / "resources" / (stem + ".tex.png"),
                data_dir / "resources" / (stem + "_2x.tex"),
                data_dir / "resources" / (stem + ".tex"),
                data_dir / "resources" / (stem + ".png")
            };
            for (const auto& cand : candidates) {
                if (fs::exists(cand)) {
                    tex_id = load_texture_file(cand.string());
                    if (tex_id) break;
                }
            }
        }
        st.scene_water_textures.push_back(tex_id);
    }
}

/// Re-upload all ground meshes after an undo/redo or scene reload.
static void resync_scene_ground_meshes(ViewerState& st) {
    for (auto& vec : st.scene_ground_gpu_meshes)
        for (auto& m : vec) av::free_mesh(m);
    for (auto& vec : st.scene_ground_textures)
        for (auto tex : vec) if (tex) glDeleteTextures(1, &tex);
    st.scene_ground_gpu_meshes.clear();
    st.scene_ground_textures.clear();
    st.scene_ground_tex_names.clear();
    if (st.scene.filepath.empty()) return;
    upload_scene_ground_meshes(st, fs::path(st.scene.filepath).parent_path().string());
}

/// In-place re-upload of one object's ground meshes (vertex editing).
static void reupload_object_ground_meshes(ViewerState& st, int object_index) {
    if (object_index < 0 || object_index >= (int)st.scene.objects.size()) return;
    if (object_index >= (int)st.scene_ground_gpu_meshes.size()) return;
    const auto& obj = st.scene.objects[object_index];
    auto& gpu  = st.scene_ground_gpu_meshes[object_index];
    auto& texs = st.scene_ground_textures[object_index];
    const size_t common = std::min(gpu.size(), obj.ground_meshes.size());
    for (size_t gi = 0; gi < common; ++gi) {
        const auto& mesh = obj.ground_meshes[gi];
        // Same normal recovery as upload_scene_ground_meshes (meshes edited in
        // the vertex tool can lose their baked normals).
        std::vector<float> computed_normals;
        if (mesh.normals.empty() && !mesh.positions.empty() &&
            mesh.num_vertices == (int)(mesh.positions.size() / 3)) {
            swk::compute_smooth_normals(mesh.positions, mesh.indices,
                                        computed_normals);
        }
        const float* nrm = !computed_normals.empty() ? computed_normals.data()
                          : (mesh.normals.empty() ? nullptr : mesh.normals.data());
        av::free_mesh(gpu[gi]);
        gpu[gi] = av::upload_mesh(mesh.positions.empty() ? nullptr : mesh.positions.data(),
                                   nrm,
                                   mesh.uvs.empty() ? nullptr : mesh.uvs.data(),
                                   mesh.num_vertices,
                                   mesh.indices.empty() ? nullptr : mesh.indices.data(),
                                   (int)mesh.indices.size());
        gpu[gi].texture_id = (gi < texs.size()) ? texs[gi] : 0;
    }
    // Free any stale GPU meshes when the mesh count shrank (undo/redo paths).
    for (size_t gi = common; gi < gpu.size(); ++gi)
        av::free_mesh(gpu[gi]);
    if (gpu.size() > obj.ground_meshes.size())
        gpu.resize(obj.ground_meshes.size());
}

static void ensure_scene_proxy_mesh(ViewerState& st) {
    if (st.scene_proxy_mesh.vao)
        return;
    static const float positions[] = {
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f
    };
static const uint32_t indices[] = {
        0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
        3,7,6, 3,6,2, 0,4,7, 0,7,3, 1,2,6, 1,6,5
    };
    st.scene_proxy_mesh = av::upload_mesh(positions, nullptr, nullptr, 8, indices, 36);
}

static void frame_scene_camera(ViewerState& st) {
    const float cx = (st.scene.bounds_min[0] + st.scene.bounds_max[0]) * 0.5f;
    const float cy = (st.scene.bounds_min[1] + st.scene.bounds_max[1]) * 0.5f;
    const float cz = (st.scene.bounds_min[2] + st.scene.bounds_max[2]) * 0.5f;
    const float dx = st.scene.bounds_max[0] - st.scene.bounds_min[0];
    const float dy = st.scene.bounds_max[1] - st.scene.bounds_min[1];
    const float dz = st.scene.bounds_max[2] - st.scene.bounds_min[2];
    const float radius = std::max(1.0f, std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f);
    st.camera.target[0] = cx;
    st.camera.target[1] = cy;
    st.camera.target[2] = cz;
    st.camera.distance = std::max(4.0f, radius * 2.6f);
    st.camera.near_plane = std::max(0.01f, st.camera.distance / 10000.0f);
    st.camera.far_plane = std::max(1000.0f, st.camera.distance + radius * 8.0f);
    // main.js uses a Z-forward scene with objects rotating around Z.
    st.camera.yaw = 0.0f;
    st.camera.pitch = 0.0f;
}

// Frame the camera at a Swordigo camera port (SpawnPoint object).
// index = -1: use "spawn_default" (or the first spawn point) for a nice
// zoomed-in initial view instead of zooming out to the whole level.
// index >= 0: jump to that specific spawn point (Camera Ports menu).
static void frame_scene_at_spawn(ViewerState& st, int index) {
    int spawn_idx = -1;
    int default_idx = -1;
    for (int i = 0; i < (int)st.scene.objects.size(); ++i) {
        if (!st.scene.objects[i].is_spawn_point) continue;
        if (default_idx < 0) default_idx = i;
        if (index >= 0 && i == index) { spawn_idx = i; break; }
        if (index < 0 && st.scene.objects[i].name == "spawn_default") { spawn_idx = i; break; }
    }
    if (spawn_idx < 0) spawn_idx = (index >= 0) ? -1 : default_idx;

    if (spawn_idx < 0) {
        frame_scene_camera(st);  // no spawn points — fall back to framing the level
        return;
    }

    const auto& sp = st.scene.objects[spawn_idx];
    // The default camera used to look straight at the hero-spawn point, which
    // plants the camera eye right on top of the objects clustered around the
    // spawn (walls, crates, particles) — an awkward, too-close start. Instead,
    // frame slightly AWAY from the exact spawn: nudge the look-at target toward
    // the middle of the level so the hero spawn stays in view but the camera
    // isn't jammed against the spawn cluster.
    const float cx = (st.scene.bounds_min[0] + st.scene.bounds_max[0]) * 0.5f;
    const float cy = (st.scene.bounds_min[1] + st.scene.bounds_max[1]) * 0.5f;
    const float cz = (st.scene.bounds_min[2] + st.scene.bounds_max[2]) * 0.5f;
    const bool have_bounds =
        st.scene.bounds_max[0] > st.scene.bounds_min[0] ||
        st.scene.bounds_max[1] > st.scene.bounds_min[1] ||
        st.scene.bounds_max[2] > st.scene.bounds_min[2];
    const float pull = 0.12f;  // 12% toward level center = "slightly away" from the spawn
    st.camera.target[0] = have_bounds ? sp.pos_x + (cx - sp.pos_x) * pull : sp.pos_x;
    st.camera.target[1] = have_bounds ? sp.pos_y + (cy - sp.pos_y) * pull : sp.pos_y;
    st.camera.target[2] = have_bounds ? sp.pos_z + (cz - sp.pos_z) * pull : sp.pos_z;
    // Comfortable pull-back framing. Instead of a fixed distance (which is
    // either jammed-in on a big level or absurdly far on a small one), scale the
    // camera distance to the level's size so the pack-around-spawn stays in view
    // but we start zoomed out enough to read the whole play area. A fixed minimum
    // keeps tiny levels from filling the screen edge to edge.
    const float lvl_dx = st.scene.bounds_max[0] - st.scene.bounds_min[0];
    const float lvl_dy = st.scene.bounds_max[1] - st.scene.bounds_min[1];
    const float lvl_dz = st.scene.bounds_max[2] - st.scene.bounds_min[2];
    const float ext = have_bounds
        ? std::sqrt(lvl_dx*lvl_dx + lvl_dy*lvl_dy + lvl_dz*lvl_dz)
        : 30.0f;
    const float distance = std::max(60.0f, ext * 1.6f);
    st.camera.distance = distance;
    st.camera.near_plane = std::max(0.01f, st.camera.distance / 10000.0f);
    st.camera.far_plane = std::max(1000.0f, st.camera.distance + ext * 12.0f);
    st.camera.yaw = 0.0f;
    st.camera.pitch = 24.0f;
    st.status_msg = "Camera port: " + sp.name;
}

/// Frame the selected object (or the whole scene when nothing is selected).
static void frame_scene_selection(ViewerState& st) {
    // Frame the union bounds of every selected object (multi-select aware).
    if (st.scene_selection.empty()) {
        frame_scene_camera(st);
        return;
    }
    bool have_bounds = false;
    float min_p[3] = {0.0f, 0.0f, 0.0f};
    float max_p[3] = {0.0f, 0.0f, 0.0f};
    for (int idx : st.scene_selection) {
        if (idx < 0 || idx >= (int)st.scene.objects.size()) continue;
        const auto& obj = st.scene.objects[idx];
        const float s = std::abs(obj.scale_x * obj.template_scaling);
        if (!have_bounds) {
            min_p[0] = max_p[0] = obj.pos_x;
            min_p[1] = max_p[1] = obj.pos_y;
            min_p[2] = max_p[2] = obj.pos_z;
            have_bounds = true;
        }
        for (const auto& gm : obj.ground_meshes) {
            min_p[0] = std::min(min_p[0], obj.pos_x + gm.min_x * s);
            min_p[1] = std::min(min_p[1], obj.pos_y + gm.min_y * s);
            min_p[2] = std::min(min_p[2], obj.pos_z + gm.min_z * s);
            max_p[0] = std::max(max_p[0], obj.pos_x + gm.max_x * s);
            max_p[1] = std::max(max_p[1], obj.pos_y + gm.max_y * s);
            max_p[2] = std::max(max_p[2], obj.pos_z + gm.max_z * s);
        }
        if (!obj.mesh_name.empty()) {
            const auto model = st.scene_model_cache.find(obj.mesh_name);
            if (model != st.scene_model_cache.end()) {
                min_p[0] = std::min(min_p[0], obj.pos_x + (model->second.center_x - model->second.radius) * s);
                min_p[1] = std::min(min_p[1], obj.pos_y + (model->second.center_y - model->second.radius) * s);
                min_p[2] = std::min(min_p[2], obj.pos_z + (model->second.center_z - model->second.radius) * s);
                max_p[0] = std::max(max_p[0], obj.pos_x + (model->second.center_x + model->second.radius) * s);
                max_p[1] = std::max(max_p[1], obj.pos_y + (model->second.center_y + model->second.radius) * s);
                max_p[2] = std::max(max_p[2], obj.pos_z + (model->second.center_z + model->second.radius) * s);
            }
        }
    }
    if (!have_bounds) { frame_scene_camera(st); return; }
    st.camera.target[0] = (min_p[0] + max_p[0]) * 0.5f;
    st.camera.target[1] = (min_p[1] + max_p[1]) * 0.5f;
    st.camera.target[2] = (min_p[2] + max_p[2]) * 0.5f;
    const float dx = max_p[0] - min_p[0], dy = max_p[1] - min_p[1], dz = max_p[2] - min_p[2];
    const float radius = std::max(1.0f, std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f);
    st.camera.distance = std::max(3.0f, radius * 3.0f);
    st.camera.near_plane = std::max(0.01f, st.camera.distance / 10000.0f);
    st.camera.far_plane = std::max(1000.0f, st.camera.distance + radius * 8.0f);
}

/// Unproject the cursor onto the camera's focal plane (the plane through the
/// camera target, perpendicular to the view direction). Built analytically from
/// the orbit-camera frame (forward/right/up), so it needs no matrix inversion
/// and cannot drift from the renderer's camera convention. Returns false when
/// the cursor ray is (near-)parallel to the focal plane.
static bool cursor_focal_point(const av::Camera& cam, int w, int h,
                               const ImVec2& viewport_pos, const ImVec2& mouse,
                               float out[3]) {
    const float yaw_r   = cam.yaw   * 3.14159f / 180.0f;
    const float pitch_r = cam.pitch * 3.14159f / 180.0f;
    const float cos_p = cosf(pitch_r), sin_p = sinf(pitch_r);
    const float sin_y = sinf(yaw_r),   cos_y = cosf(yaw_r);

    // Eye position (matches camera_get_view_matrix).
    const float eye[3] = {cam.target[0] + cam.distance * cos_p * sin_y,
                          cam.target[1] + cam.distance * sin_p,
                          cam.target[2] + cam.distance * cos_p * cos_y};
    // Forward = eye->target, right = forward x world-up, up = right x forward.
    float fwd[3] = {cam.target[0] - eye[0], cam.target[1] - eye[1], cam.target[2] - eye[2]};
    const float fwd_len = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (fwd_len < 1e-6f) return false;
    fwd[0] /= fwd_len; fwd[1] /= fwd_len; fwd[2] /= fwd_len;
    float right[3] = {-fwd[2], 0.0f, fwd[0]};          // fwd x (0,1,0)
    const float r_len = std::sqrt(right[0]*right[0] + right[2]*right[2]);
    if (r_len < 1e-6f) return false;                    // looking straight up/down
    right[0] /= r_len; right[2] /= r_len;
    float up[3] = {right[1]*fwd[2] - right[2]*fwd[1],
                   right[2]*fwd[0] - right[0]*fwd[2],
                   right[0]*fwd[1] - right[1]*fwd[0]};

    const float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    const float ndc_x = ((mouse.x - viewport_pos.x) / (float)w) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - ((mouse.y - viewport_pos.y) / (float)h) * 2.0f;
    const float tan_fov = tanf(cam.fov * 0.5f * 3.14159f / 180.0f);

    // Ray from the eye through the cursor pixel.
    float ray[3] = {fwd[0] + right[0] * (ndc_x * tan_fov * aspect) + up[0] * (ndc_y * tan_fov),
                    fwd[1] + right[1] * (ndc_x * tan_fov * aspect) + up[1] * (ndc_y * tan_fov),
                    fwd[2] + right[2] * (ndc_x * tan_fov * aspect) + up[2] * (ndc_y * tan_fov)};
    const float r_len2 = std::sqrt(ray[0]*ray[0] + ray[1]*ray[1] + ray[2]*ray[2]);
    if (r_len2 < 1e-6f) return false;
    ray[0] /= r_len2; ray[1] /= r_len2; ray[2] /= r_len2;

    // Intersect with the focal plane through the target (normal = fwd).
    const float denom = ray[0]*fwd[0] + ray[1]*fwd[1] + ray[2]*fwd[2];
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = ((cam.target[0]-eye[0])*fwd[0] +
                     (cam.target[1]-eye[1])*fwd[1] +
                     (cam.target[2]-eye[2])*fwd[2]) / denom;
    if (t < 0.0f) return false;
    out[0] = eye[0] + ray[0] * t;
    out[1] = eye[1] + ray[1] * t;
    out[2] = eye[2] + ray[2] * t;
    return true;
}

/// Pick the mesh-edit vertex nearest the mouse (within 10px).
static bool pick_mesh_edit_vertex(const ViewerState& st, int w, int h, const ImVec2& viewport_pos,
                                  const ImVec2& mouse, int& out_mesh, int& out_vertex) {
    out_mesh = out_vertex = -1;
    if (st.mesh_edit_object < 0 || st.mesh_edit_object >= (int)st.scene.objects.size()) return false;
    const auto& obj = st.scene.objects[st.mesh_edit_object];
    if (obj.ground_meshes.empty()) return false;
    float obj_mat[16];
    swk::object_world_matrix(obj, obj_mat);
    float best = 14.0f;
    for (size_t mi = 0; mi < obj.ground_meshes.size(); ++mi) {
        const auto& mesh = obj.ground_meshes[mi];
        for (size_t vi = 0; vi < (size_t)mesh.num_vertices; ++vi) {
            const float lp[3] = {mesh.positions[vi*3], mesh.positions[vi*3+1], mesh.positions[vi*3+2]};
            const float wp[3] = { obj_mat[0]*lp[0] + obj_mat[4]*lp[1] + obj_mat[8]*lp[2]  + obj_mat[12],
                                  obj_mat[1]*lp[0] + obj_mat[5]*lp[1] + obj_mat[9]*lp[2]  + obj_mat[13],
                                  obj_mat[2]*lp[0] + obj_mat[6]*lp[1] + obj_mat[10]*lp[2] + obj_mat[14] };
            ImVec2 sp;
            if (!swk::world_to_screen(st.camera, w, h, viewport_pos, wp, sp)) continue;
            const float d = std::hypotf(mouse.x - sp.x, mouse.y - sp.y);
            if (d < best) { best = d; out_mesh = (int)mi; out_vertex = (int)vi; }
        }
    }
    return best < 14.0f;
}

/// Derive yaw/pitch/distance from a (possibly view-cube-modified) view matrix.
///
/// mat4_look_at stores the rotation as rows (side, up, -forward) with
/// translation -(s·e, r·e, -f·e); the eye position is therefore
///   eye = -(t0·s + t1·r + t2·(-f)).
/// Then yaw/pitch/distance follow directly from eye - target, matching the
/// spherical-orbit camera convention (verified by round-trip unit test).
static void decompose_view_to_camera(const float view[16], const float old_target[3], av::Camera& cam) {
    const float s[3]  = {view[0], view[4], view[8]};
    const float r[3]  = {view[1], view[5], view[9]};
    const float nf[3] = {view[2], view[6], view[10]};
    const float t[3]  = {view[12], view[13], view[14]};
    const float eye[3] = {-(t[0]*s[0] + t[1]*r[0] + t[2]*nf[0]),
                          -(t[0]*s[1] + t[1]*r[1] + t[2]*nf[1]),
                          -(t[0]*s[2] + t[1]*r[2] + t[2]*nf[2])};
    const float dx = eye[0] - old_target[0], dy = eye[1] - old_target[1], dz = eye[2] - old_target[2];
    cam.distance = std::max(0.1f, std::sqrt(dx*dx + dy*dy + dz*dz));
    if (cam.distance > 1e-6f) {
        cam.yaw   = atan2f(dx, dz) * 180.0f / 3.14159265358979323846f;
        cam.pitch = asinf(std::clamp(dy / cam.distance, -1.0f, 1.0f)) * 180.0f / 3.14159265358979323846f;
        if (cam.pitch > 89.0f) cam.pitch = 89.0f;
        if (cam.pitch < -89.0f) cam.pitch = -89.0f;
    }
}

/// Mesh-edit "Insert": subdivide the picked face (face tool) or split the
/// picked edge (edge tool).  Shared by the toolbar button and the I key.
static void mesh_edit_insert_vertex(ViewerState& st) {
    if (st.mesh_edit_object < 0 || st.mesh_edit_object >= (int)st.scene.objects.size())
        return;
    auto& eobj = st.scene.objects[st.mesh_edit_object];
    if (st.mesh_edit_mesh < 0 || st.mesh_edit_mesh >= (int)eobj.ground_meshes.size())
        return;
    auto& pm = eobj.ground_meshes[st.mesh_edit_mesh];
    snapshot_scene(st);
    bool ok = false;
    if (st.mesh_edit_tool == 1 && st.mesh_edit_triangle >= 0)
        ok = swk::ground_mesh_subdivide_triangle(pm, st.mesh_edit_triangle) >= 0;
    else if (st.mesh_edit_tool == 2 && st.mesh_edit_edge_a >= 0 && st.mesh_edit_edge_b >= 0)
        ok = swk::ground_mesh_split_edge(pm, st.mesh_edit_edge_a, st.mesh_edit_edge_b) >= 0;
    if (ok) {
        swk::recompute_ground_mesh_geometry(pm);
        reupload_object_ground_meshes(st, st.mesh_edit_object);
        av::scene_mark_ground_mesh_dirty(st.scene, st.mesh_edit_object);
        av::scene_refresh(st.scene);
        st.mesh_edit_triangle = -1;
        st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
        st.scene_dirty = true;
    }
}

// ============================================================================
// ============================================================================
// Ground Mesh Generator — SMM2-style 2D sketch canvas + .swdm round-trip
// ============================================================================

static double gm_polygon_area(const std::vector<boulder::PolygonPoint>& pts);
static void gm_ensure_ccw(std::vector<boulder::PolygonPoint>& pts);
static bool gm_regenerate_object_geometry(ViewerState& st, int idx, std::string* err,
                                          bool live = false);
static void gm_apply_to_object(ViewerState& st);          // GMG tab: apply to object
static bool gm_begin_inline_edit(ViewerState& st);        // inline 2D edit in viewport
static void gm_end_inline_edit(ViewerState& st);
static void draw_inline_polygon_overlay(ViewerState& st, ImDrawList* overlay,
                                        const ImVec2& pos, int w, int h);
static void inline_edit_input(ViewerState& st, const ImVec2& pos, int w, int h);

static std::string gm_build_swdm_text(const ViewerState& st) {
    boulder::GroundMesh gm;
    gm.polygon = st.gm_points;
    gm.hats    = st.gm_hats;
    gm_ensure_ccw(gm.polygon);
    gm.min_depth   = st.gm_min_depth;
    gm.max_depth   = st.gm_max_depth;
    gm.top_angle   = st.gm_top_angle;
    gm.generate_top = st.gm_generate_top;
    gm.z           = st.gm_z;
    gm.top_texture = st.gm_top_tex;
    gm.bottom_texture = st.gm_bottom_tex;
    return boulder::serialize_swdm(gm);
}

// Inject a freshly generated GroundMesh object into the open scene.
// Returns the new object index or -1 on failure.
static int gm_add_to_scene(ViewerState& st) {
    if (st.gm_points.size() < 3) {
        st.status_msg = "Sketch needs at least 3 points.";
        return -1;
    }
    if (st.scene.filepath.empty()) {
        st.status_msg = "Open a scene first.";
        return -1;
    }
    const std::string swdm = gm_build_swdm_text(st);
    std::string bin = boulder::generate_ground_mesh_object(swdm, "gm_merge", st.gm_z);
    if (bin.empty()) {
        st.status_msg = "Ground mesh generation failed (degenerate polygon?).";
        return -1;
    }
    const std::string tmp_path = st.scene.filepath + ".ruby-gm.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) { st.status_msg = "Cannot write temp merge file."; return -1; }
        out.write(bin.data(), static_cast<std::streamsize>(bin.size()));
        out.close();
    }
    av::SceneData parsed;
    try {
        parsed = av::scene_load(tmp_path);
    } catch (const std::exception&) {
        std::error_code ec;
        fs::remove(tmp_path, ec);
        st.status_msg = "Generated mesh failed to parse.";
        return -1;
    }
    std::error_code ec;
    fs::remove(tmp_path, ec);
    if (parsed.objects.empty()) {
        st.status_msg = "Generated mesh failed to parse.";
        return -1;
    }

    snapshot_scene(st);
    av::SceneObject obj = std::move(parsed.objects[0]);
    obj.name = st.gm_obj_name[0] ? st.gm_obj_name : av::scene_fresh_identifier(st.scene);
    // Spawn under the camera focus (X/Y) so the new ground mesh appears where
    // the user is looking; keep the depth layer chosen in the generator.
    obj.pos_x = st.camera.target[0];
    obj.pos_y = st.camera.target[1];
    st.scene.objects.push_back(std::move(obj));
    av::scene_refresh(st.scene);

    // Upload GPU meshes + textures for the new object. Water textures are not
    // re-uploaded here: waters are parsed only at scene_load, so the parallel
    // st.scene_water_textures cache stays valid across object edits.
    const fs::path scene_dir = fs::path(st.scene.filepath).parent_path();
    upload_scene_ground_meshes(st, scene_dir.string());
    const int idx = static_cast<int>(st.scene.objects.size()) - 1;
    select_scene_object(st, idx);
    st.scene_dirty = true;
    st.status_msg = "Added ground mesh '" + std::string(st.gm_obj_name) + "' to scene.";
    // Jump to the Visual editor and frame the new mesh so it can be grabbed.
    switch_scene_tab(st, 1);
    frame_scene_selection(st);
    return idx;
}

// Import an existing GroundMesh object's polygon into the sketch editor.
static void gm_import_from_scene(ViewerState& st) {
    if (st.selected_object < 0 || st.selected_object >= (int)st.scene.objects.size()) return;
    const auto& obj = st.scene.objects[st.selected_object];
    st.gm_points.clear();
    for (const auto& comp : obj.components) {
        const int payload = comp.payload_field;
        if (payload != 110) continue;   // GroundPolygonComponent
        try {
            proto::Reader wrapper(comp.raw_data);
            proto::Field f;
            while (wrapper.read_field(f)) {
                if (f.field_number != 110 || f.wire_type != proto::WIRE_LEN) continue;
                proto::Reader gpc(f.bytes_val);
                proto::Field g;
                while (gpc.read_field(g)) {
                    if (g.field_number != 2 || g.wire_type != proto::WIRE_LEN) continue; // Polygon
                    proto::Reader poly(g.bytes_val);
                    proto::Field p;
                    while (poly.read_field(p)) {
                        if (p.field_number != 1 || p.wire_type != proto::WIRE_LEN) continue; // Vertex
                        proto::Reader vec2(p.bytes_val);
                        proto::Field v;
                        float x = 0, y = 0;
                        while (vec2.read_field(v)) {
                            if (v.field_number == 1) x = v.float_val;
                            else if (v.field_number == 2) y = v.float_val;
                        }
                        st.gm_points.push_back({x, y});
                    }
                }
            }
        } catch (...) {}
        if (!st.gm_points.empty()) break;
    }
    if (!st.gm_points.empty()) {
        st.gm_sketch_dirty = true;
        st.status_msg = "Imported " + std::to_string(st.gm_points.size()) + " points from selected object.";
        // Keep the object's depth as the default Z.
        st.gm_z = obj.pos_z;
    } else {
        st.status_msg = "Selected object has no GroundPolygon to import.";
    }
}

// Core: regenerate an object's ground-mesh geometry from the current sketch
// (polygon + hats) and re-upload it. The object keeps its identity and world
// transform; its GroundPolygon / GroundMesh / generator components are replaced
// by the freshly generated ones. Used by both the GMG tab's "Apply to Object"
// and the inline (in-viewport) 2D editor's live preview. Callers own the undo
// snapshot. Returns false with *err filled on any failure. When live=true only
// the edited object's GPU buffers are resynced (grow/shrink safe) so the 3D
// mesh reshapes under the outline without re-uploading the whole scene.
static bool gm_regenerate_object_geometry(ViewerState& st, int idx, std::string* err,
                                          bool live) {
    const auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (idx < 0 || idx >= (int)st.scene.objects.size())
        return fail("Nothing to apply to.");
    if (st.gm_points.size() < 3)
        return fail("Polygon needs at least 3 points.");
    const std::string swdm = gm_build_swdm_text(st);
    std::string bin = boulder::generate_ground_mesh_object(swdm,
                                                           st.scene.objects[idx].name,
                                                           st.gm_z);
    if (bin.empty())
        return fail("Ground mesh generation failed (degenerate polygon?).");
    const std::string tmp_path = st.scene.filepath + ".ruby-gm-apply.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return fail("Cannot write temp file.");
        out.write(bin.data(), static_cast<std::streamsize>(bin.size()));
    }
    av::SceneData parsed;
    try {
        parsed = av::scene_load(tmp_path);
    } catch (const std::exception&) {
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return fail("Generated mesh failed to parse.");
    }
    std::error_code ec;
    fs::remove(tmp_path, ec);
    if (parsed.objects.empty())
        return fail("Generated mesh failed to parse.");

    av::SceneObject& target = st.scene.objects[idx];
    av::SceneObject fresh = std::move(parsed.objects[0]);
    target.components = std::move(fresh.components);
    target.pos_z = st.gm_z;               // depth layer chosen in the editor
    av::scene_refresh(st.scene);
    // The re-parsed geometry (SurfaceMesh / FrontMesh / hats) wins over what
    // scene_refresh re-derived, so the edited polygon renders immediately.
    target.ground_meshes = std::move(fresh.ground_meshes);
    if (live) {
        // Live preview: resync only this object's buffers (no scene-wide churn).
        resync_object_ground_meshes(st, idx);
    } else {
        const fs::path scene_dir = fs::path(st.scene.filepath).parent_path();
        upload_scene_ground_meshes(st, scene_dir.string());
    }
    av::scene_mark_ground_mesh_dirty(st.scene, idx);
    st.scene_dirty = true;
    return true;
}

// GMG tab: regenerate the edited object and return to the Visual editor.
static void gm_apply_to_object(ViewerState& st) {
    const int idx = st.gm_edit_apply_object;
    std::string err;
    snapshot_scene(st);
    if (!gm_regenerate_object_geometry(st, idx, &err)) {
        st.status_msg = err.empty() ? "Apply failed." : err;
        return;
    }
    st.gm_edit_apply_object = -1;
    st.gm_workspace_mode = 0;
    switch_scene_tab(st, 1);
    st.scene_mesh_edit = false;
    select_scene_object(st, idx);
    frame_scene_selection(st);
    st.status_msg = "Applied sketch to '" + st.scene.objects[idx].name + "'.";
}

// ── Inline 2D polygon editing inside the scene visualizer ────────────────
// The default "Mesh Edit" path: the selected ground-mesh object's polygon is
// edited right on top of the 3D viewport (screen-space overlay), with live
// re-apply so the mesh reshapes as you drag. RMB on an edge inserts a new
// node at the click spot; RMB/Del on a vertex removes it; Esc/M ends.
static bool gm_begin_inline_edit(ViewerState& st) {
    if (st.selected_object < 0 || st.selected_object >= (int)st.scene.objects.size()) {
        st.status_msg = "Select a ground mesh object first.";
        return false;
    }
    const int sel = st.selected_object;
    gm_import_from_scene(st);
    if (st.gm_points.size() < 3) {
        st.status_msg = "Selected object has no ground polygon to edit in 2D.";
        return false;
    }
    snapshot_scene(st);           // one undo step for the whole edit session
    st.gm_edit_apply_object = sel;
    st.gm_inline_edit = true;
    st.gm_inline_dirty = false;
    st.gm_drag_from_insert = false;
    st.scene_mesh_edit = false;
    st.mesh_edit_object = -1;
    st.gm_workspace_mode = 1;

    // Pure 2D view: Swordigo ground polygons are the XY cross-section of the
    // mesh (the slab is extruded along local Z), so the correct 2D view is the
    // camera looking straight along +Z — zero elevation, dead-on. That renders
    // the outline as a flat, undistorted 2D drawing (a top-down camera would
    // show it edge-on as a thin line). Yaw is set so local +X runs right.
    constexpr float kPi2 = 3.14159265f;
    st.gm_inline_saved_cam = st.camera;
    st.gm_inline_saved_valid = true;
    float obj_mat[16];
    swk::object_world_matrix(st.scene.objects[sel], obj_mat);
    float minx = 1e30f, miny = 1e30f, minz = 1e30f;
    float maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
    for (const auto& p : st.gm_points) {
        const float lp[3] = {(float)p.x, (float)p.y, 0.0f};
        const float wx = obj_mat[0]*lp[0] + obj_mat[4]*lp[1] + obj_mat[8]*lp[2]  + obj_mat[12];
        const float wy = obj_mat[1]*lp[0] + obj_mat[5]*lp[1] + obj_mat[9]*lp[2]  + obj_mat[13];
        const float wz = obj_mat[2]*lp[0] + obj_mat[6]*lp[1] + obj_mat[10]*lp[2] + obj_mat[14];
        minx = std::min(minx, wx); maxx = std::max(maxx, wx);
        miny = std::min(miny, wy); maxy = std::max(maxy, wy);
        minz = std::min(minz, wz); maxz = std::max(maxz, wz);
    }
    st.camera.target[0] = (minx + maxx) * 0.5f;
    st.camera.target[1] = (miny + maxy) * 0.5f;
    st.camera.target[2] = (minz + maxz) * 0.5f;
    // Ground-mesh objects spin around Z via rot_y (radians); the polygon plane
    // is their local XY, so the camera must look along local -Z (from the +Z
    // side) for local +X to point right and local +Y to point up on screen.
    // Looking along +Z would mirror the outline (side vector flips).
    const float rot_y_deg = st.scene.objects[sel].rot_y * 180.0f / kPi2;
    st.camera.yaw   = -rot_y_deg;
    st.camera.pitch = 0.0f;                         // straight elevation (2D)
    // Fit the XY outline: fit on the larger of the polygon's local extents.
    const float fit = std::max(std::max(maxx - minx, maxy - miny), 1.0f);
    const float vfov = st.camera.fov * kPi2 / 180.0f;
    st.camera.distance = std::clamp((fit * 0.5f) / std::tan(vfov * 0.5f) * 1.1f,
                                    3.0f, 5000.0f);   // 5000 frames giant meshes too
    st.camera.near_plane = std::max(0.01f, st.camera.distance / 10000.0f);
    st.camera.far_plane  = std::max(1000.0f, st.camera.distance + 4000.0f);

    st.status_msg = "2D editing '" + st.scene.objects[sel].name +
                    "' — drag vertices \xc2\xb7 right-click an edge to add a node \xc2\xb7 "
                    "MMB pan / wheel zoom \xc2\xb7 Esc to finish.";
    return true;
}

static void gm_end_inline_edit(ViewerState& st) {
    if (st.gm_inline_dirty) {
        std::string err;
        if (!gm_regenerate_object_geometry(st, st.gm_edit_apply_object, &err)) {
            // Keep the session open so the user can fix the polygon instead of
            // silently losing their edits (e.g. a degenerate outline).
            st.status_msg = err.empty() ? "Apply failed — fix the polygon." : err;
            return;
        }
    }
    st.gm_inline_edit = false;
    st.gm_inline_dirty = false;
    st.gm_edit_apply_object = -1;
    st.gm_drag_point = -1;
    st.gm_dragging = false;
    st.gm_drag_from_insert = false;
    st.gm_drag_off_x = st.gm_drag_off_y = 0.0;
    if (st.gm_inline_saved_valid) {          // restore the pre-edit camera view
        st.camera = st.gm_inline_saved_cam;
        st.gm_inline_saved_valid = false;
    }
    st.status_msg = "Mesh edit finished.";
}

// Intersect a world ray with the plane spanned by an object matrix's X/Y axes
// through its origin; returns the hit in object-local (x, y). False when the
// ray is nearly parallel to the plane (edge-on view).
static bool inline_ray_object_plane(const float mat[16], const float origin[3],
                                    const float dir[3], float& lx, float& ly) {
    const float px = mat[12], py = mat[13], pz = mat[14];
    const float ax = mat[0],  ay = mat[1],  az = mat[2];
    const float bx = mat[4],  by = mat[5],  bz = mat[6];
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nl < 1e-6f) return false;
    nx /= nl; ny /= nl; nz /= nl;
    const float denom = nx * dir[0] + ny * dir[1] + nz * dir[2];
    if (std::fabs(denom) < 1e-5f) return false;
    const float t = (nx * (px - origin[0]) + ny * (py - origin[1]) +
                     nz * (pz - origin[2])) / denom;
    if (t < 0.0f) return false;
    const float hx = origin[0] + dir[0] * t - px;
    const float hy = origin[1] + dir[1] * t - py;
    const float hz = origin[2] + dir[2] * t - pz;
    const float al2 = ax * ax + ay * ay + az * az;
    const float bl2 = bx * bx + by * by + bz * bz;
    if (al2 < 1e-8f || bl2 < 1e-8f) return false;
    lx = (hx * ax + hy * ay + hz * az) / al2;
    ly = (hx * bx + hy * by + hz * bz) / bl2;
    return true;
}

// Squared distance from point p to segment [a, b] (screen space).
static float point_segment_dist2(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 1e-9f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float qx = a.x + dx * t, qy = a.y + dy * t;
    const float ex = p.x - qx, ey = p.y - qy;
    return ex * ex + ey * ey;
}

// Screen-space overlay for the inline 2D editor: the object's ground polygon
// drawn on top of the 3D viewport with draggable vertex handles and edge-insert
// highlights. Hover targets are stored on the ViewerState for the input block.
static void draw_inline_polygon_overlay(ViewerState& st, ImDrawList* overlay,
                                        const ImVec2& pos, int w, int h) {
    st.gm_inline_hover_vertex = -1;
    st.gm_inline_hover_edge = -1;
    const int idx = st.gm_edit_apply_object;
    if (!st.gm_inline_edit || idx < 0 || idx >= (int)st.scene.objects.size()) return;
    const auto& obj = st.scene.objects[idx];
    if (st.gm_points.size() < 3) return;

    float obj_mat[16];
    swk::object_world_matrix(obj, obj_mat);
    // Screen-space points paired with their gm_points index. Off-screen
    // vertices are SKIPPED (not aborting the whole outline): dragging a vertex
    // beyond the viewport edge must not make the polygon + handles vanish.
    struct SP { ImVec2 p; int oi; };
    std::vector<SP> pts;
    std::vector<ImVec2> poly;   // plain list for the convex fill
    pts.reserve(st.gm_points.size());
    for (int oi = 0; oi < (int)st.gm_points.size(); ++oi) {
        const auto& p = st.gm_points[oi];
        const float lp[3] = {(float)p.x, (float)p.y, 0.0f};
        const float wp[3] = { obj_mat[0]*lp[0] + obj_mat[4]*lp[1] + obj_mat[8]*lp[2]  + obj_mat[12],
                              obj_mat[1]*lp[0] + obj_mat[5]*lp[1] + obj_mat[9]*lp[2]  + obj_mat[13],
                              obj_mat[2]*lp[0] + obj_mat[6]*lp[1] + obj_mat[10]*lp[2] + obj_mat[14] };
        ImVec2 sp;
        if (!swk::world_to_screen(st.camera, w, h, pos, wp, sp)) continue;
        pts.push_back({sp, oi});
        poly.push_back(sp);
    }
    if (pts.size() < 3) return;

    const ImVec2 mouse = ImGui::GetMousePos();
    // Larger grab targets: the mesh outline is easy to click by accident, so
    // vertices win within 11px and edges within 10px (generous at any DPI).

    // ── 2D grid on the object plane (screen-space, clipped to the viewport) ──
    // The camera is locked to the 2D front view during the session, so this
    // reads as a flat graph-paper overlay behind the polygon. Grid spacing
    // adapts to the zoom.
    constexpr float kPi2 = 3.14159265f;
    const float vfov = st.camera.fov * kPi2 / 180.0f;
    const float px_per_wu = (h * 0.5f) /
        std::max(1e-3f, st.camera.distance * std::tan(vfov * 0.5f));
    float step = 50.0f;
    while (step * px_per_wu < 34.0f) step *= 2.0f;
    while (step * px_per_wu > 96.0f) step *= 0.5f;
    float lminx = 1e30f, lmaxx = -1e30f, lminy = 1e30f, lmaxy = -1e30f;
    for (const auto& p : st.gm_points) {
        lminx = std::min(lminx, (float)p.x); lmaxx = std::max(lmaxx, (float)p.x);
        lminy = std::min(lminy, (float)p.y); lmaxy = std::max(lmaxy, (float)p.y);
    }
    const float wu_margin = 40.0f / std::max(1e-3f, px_per_wu);
    lminx -= wu_margin; lmaxx += wu_margin;
    lminy -= wu_margin; lmaxy += wu_margin;
    auto proj_local = [&](float lx_, float ly_, ImVec2& out) -> bool {
        const float ww[3] = { obj_mat[0]*lx_ + obj_mat[4]*ly_ + obj_mat[12],
                              obj_mat[1]*lx_ + obj_mat[5]*ly_ + obj_mat[13],
                              obj_mat[2]*lx_ + obj_mat[6]*ly_ + obj_mat[14] };
        return swk::world_to_screen(st.camera, w, h, pos, ww, out);
    };
    ImVec2 ga, gb;
    const int k0 = (int)std::floor(lminx / step), k1 = (int)std::ceil(lmaxx / step);
    const int j0 = (int)std::floor(lminy / step), j1 = (int)std::ceil(lmaxy / step);
    const ImU32 grid_col = IM_COL32(150, 190, 170, 46);
    for (int k = k0; k <= k1; ++k) {
        const float x = k * step;
        if (!proj_local(x, lminy, ga) || !proj_local(x, lmaxy, gb)) continue;
        overlay->AddLine(ga, gb, grid_col, 1.0f);
    }
    for (int j = j0; j <= j1; ++j) {
        const float y = j * step;
        if (!proj_local(lminx, y, ga) || !proj_local(lmaxx, y, gb)) continue;
        overlay->AddLine(ga, gb, grid_col, 1.0f);
    }
    if (proj_local(0.0f, lminy, ga) && proj_local(0.0f, lmaxy, gb))
        overlay->AddLine(ga, gb, IM_COL32(235, 110, 110, 80), 1.5f);   // local X axis
    if (proj_local(lminx, 0.0f, ga) && proj_local(lmaxx, 0.0f, gb))
        overlay->AddLine(ga, gb, IM_COL32(110, 200, 120, 80), 1.5f);   // local Y axis

    // Hover targets: vertices win over edges. Indices map back to gm_points
    // via SP.oi so skipped off-screen vertices never alias a wrong node.
    float best_edge_d2 = 10.0f * 10.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1) % pts.size();
        const float d2 = point_segment_dist2(mouse, pts[i].p, pts[j].p);
        if (d2 < best_edge_d2) { best_edge_d2 = d2; st.gm_inline_hover_edge = pts[i].oi; }
        const float vdx = mouse.x - pts[i].p.x, vdy = mouse.y - pts[i].p.y;
        if (vdx * vdx + vdy * vdy <= 11.0f * 11.0f) st.gm_inline_hover_vertex = pts[i].oi;
    }

    // Translucent fill + outline; hovered edge glows gold.
    overlay->AddConvexPolyFilled(poly.data(), (int)poly.size(), IM_COL32(70, 130, 90, 70));
    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1) % pts.size();
        const bool hover_edge = (st.gm_inline_hover_edge == pts[i].oi &&
                                 st.gm_inline_hover_vertex < 0);
        overlay->AddLine(pts[i].p, pts[j].p,
                         hover_edge ? IM_COL32(255, 200, 60, 255) : IM_COL32(120, 210, 150, 255),
                         hover_edge ? 3.5f : 2.0f);
    }
    // Vertex handles with index labels.
    for (const auto& sp : pts) {
        const bool hover = (sp.oi == st.gm_inline_hover_vertex);
        const bool dragging = (sp.oi == st.gm_drag_point && st.gm_dragging);
        const float r = (hover || dragging) ? 6.5f : 5.0f;
        const ImU32 col = dragging ? IM_COL32(255, 255, 120, 255)
                          : hover   ? IM_COL32(255, 190, 120, 255)
                          :           IM_COL32(255, 130, 70, 255);
        overlay->AddCircleFilled(sp.p, r, col);
        overlay->AddCircle(sp.p, r, IM_COL32(20, 20, 30, 255), 0, 1.5f);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", sp.oi);
        overlay->AddText(ImVec2(sp.p.x + 7.0f, sp.p.y - 11.0f),
                         IM_COL32(225, 235, 245, 190), lbl);
    }
    // Mode tag + hint bar.
    overlay->AddText(ImVec2(pos.x + (float)w - 120.0f, pos.y + 14.0f),
                     IM_COL32(110, 210, 255, 255), "2D EDIT");
    overlay->AddRectFilled(ImVec2(pos.x, pos.y + (float)h - 24.0f),
                           ImVec2(pos.x + (float)w, pos.y + (float)h),
                           IM_COL32(18, 21, 28, 215));
    overlay->AddText(ImVec2(pos.x + 8.0f, pos.y + (float)h - 20.0f),
                     IM_COL32(160, 175, 195, 255),
                     "LMB drag vertex \xc2\xb7 RMB edge = new node \xc2\xb7 RMB/Del vertex = remove \xc2\xb7 Esc done");
}

// Input for the inline 2D editor — owns the whole viewport input block while
// active (LMB vertex drag, RMB edge-insert / vertex-delete, Del, Esc/M exit)
// and drives the throttled live re-apply.
static void inline_edit_input(ViewerState& st, const ImVec2& pos, int w, int h) {
    const int idx = st.gm_edit_apply_object;
    if (idx < 0 || idx >= (int)st.scene.objects.size()) return;
    const auto& obj = st.scene.objects[idx];
    float obj_mat[16];
    swk::object_world_matrix(obj, obj_mat);
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // Clamp the ray cursor into the viewport: when the pointer drifts outside
    // the image (over a panel or the toolbar), an extrapolated ray would hit
    // the object plane FAR away and teleport the grabbed vertex. Points just
    // outside (<= 8px) still track so edge-drags stay smooth.
    const float margin = 8.0f;
    const bool cursor_in_view = mouse.x >= pos.x - margin &&
                                mouse.x <= pos.x + (float)w + margin &&
                                mouse.y >= pos.y - margin &&
                                mouse.y <= pos.y + (float)h + margin;
    const ImVec2 ray_mouse(std::clamp(mouse.x, pos.x, pos.x + (float)w),
                           std::clamp(mouse.y, pos.y, pos.y + (float)h));
    float origin[3], dir[3];
    swk::screen_ray(st.camera, w, h, pos, ray_mouse, origin, dir);
    float lx = 0.0f, ly = 0.0f;
    const bool hit_plane = cursor_in_view &&
                           inline_ray_object_plane(obj_mat, origin, dir, lx, ly);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const bool grabbed = st.gm_inline_hover_vertex >= 0 &&
                             st.gm_inline_hover_vertex < (int)st.gm_points.size() &&
                             hit_plane;
        if (grabbed) {
            st.gm_drag_point = st.gm_inline_hover_vertex;
            st.gm_dragging = true;
            // Keep the vertex's offset from the cursor (no snap-to-center jump).
            st.gm_drag_off_x = st.gm_points[st.gm_drag_point].x - lx;
            st.gm_drag_off_y = st.gm_points[st.gm_drag_point].y - ly;
        } else {
            st.gm_drag_point = -1;
            st.gm_dragging = false;
        }
        st.gm_drag_from_insert = false;   // LMB grab — normal drag contract
    }
    if (st.gm_dragging && st.gm_drag_point >= 0 &&
        st.gm_drag_point < (int)st.gm_points.size()) {
        // A drag armed by an RMB insert stays alive while RMB is held (the
        // insert gesture continues into the drag); all other grabs use LMB.
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                          (st.gm_drag_from_insert && ImGui::IsMouseDown(ImGuiMouseButton_Right));
        if (!held) {
            st.gm_dragging = false;
            st.gm_drag_point = -1;
            st.gm_drag_from_insert = false;
        } else if (hit_plane) {
            // Cursor outside the viewport: hold position (no teleport) but keep
            // the grab alive so the drag resumes when the pointer returns.
            st.gm_points[st.gm_drag_point] = {lx + st.gm_drag_off_x,
                                              ly + st.gm_drag_off_y};
            st.gm_inline_dirty = true;
        }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (st.gm_inline_hover_vertex >= 0 && st.gm_points.size() > 3) {
            st.gm_points.erase(st.gm_points.begin() + st.gm_inline_hover_vertex);
            st.gm_drag_point = -1;          // indices shifted — drop any stale drag
            st.gm_dragging = false;
            st.gm_inline_dirty = true;
        } else if (st.gm_inline_hover_edge >= 0 && hit_plane) {
            // Insert a node at the click spot, snapped onto the hovered edge,
            // and grab it immediately so the user can drag it into place.
            const int a = st.gm_inline_hover_edge;
            const int b = (a + 1) % (int)st.gm_points.size();
            const double ax = st.gm_points[a].x, ay = st.gm_points[a].y;
            const double bx = st.gm_points[b].x, by = st.gm_points[b].y;
            const double dx = bx - ax, dy = by - ay;
            const double len2 = dx * dx + dy * dy;
            double t = len2 > 1e-12 ? ((lx - ax) * dx + (ly - ay) * dy) / len2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            st.gm_points.insert(st.gm_points.begin() + b,
                                {ax + dx * t, ay + dy * t});
            st.gm_drag_point = b;           // grab the new node right away
            st.gm_dragging = true;
            st.gm_drag_from_insert = true;  // ...and keep it grabbed while RMB is held
            st.gm_drag_off_x = 0.0;         // node starts exactly under the cursor
            st.gm_drag_off_y = 0.0;
            st.gm_inline_dirty = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && st.gm_inline_hover_vertex >= 0 &&
        st.gm_points.size() > 3) {
        st.gm_points.erase(st.gm_points.begin() + st.gm_inline_hover_vertex);
        st.gm_drag_point = -1;              // indices shifted — drop any stale drag
        st.gm_dragging = false;
        st.gm_inline_dirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_M)) {
        gm_end_inline_edit(st);
        return;
    }

    // ── 2D navigation: MMB drag pans, wheel zooms, arrow keys nudge. The
    // camera is locked to the 2D front view (pitch 0, looking along the
    // object's Z) for the session, so this is pure 2D movement of the view
    // (same right/up convention as the RMB-pan block).
    constexpr float kPi2 = 3.14159265f;
    const float yaw_r = st.camera.yaw * kPi2 / 180.0f;
    const float pit_r = st.camera.pitch * kPi2 / 180.0f;
    const float rx = std::cos(yaw_r);
    const float rz = -std::sin(yaw_r);
    const float ux = -std::sin(yaw_r) * std::sin(pit_r);
    const float uy = std::cos(pit_r);
    const float uz = -std::cos(yaw_r) * std::sin(pit_r);
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const float scale = st.camera.distance * 0.003f * st.cam_pan_speed; // same as RMB pan
        st.camera.target[0] -= (rx * io.MouseDelta.x + ux * io.MouseDelta.y) * scale;
        st.camera.target[1] -= uy * io.MouseDelta.y * scale;
        st.camera.target[2] -= (rz * io.MouseDelta.x + uz * io.MouseDelta.y) * scale;
    }
    if (io.MouseWheel != 0.0f) {
        const float factor = std::pow(0.94f, io.MouseWheel * st.cam_zoom_speed);
        st.camera.distance = std::clamp(st.camera.distance * factor, 0.1f, 2000.0f);
        st.camera.near_plane = std::max(0.01f, st.camera.distance / 10000.0f);
        st.camera.far_plane  = std::max(1000.0f, st.camera.distance + 4000.0f);
        // The global zoom block is gated on !gm_inline_edit, so no double-apply.
    }
    float panx = 0.0f, pany = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    pany += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  pany -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  panx -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) panx += 1.0f;
    if (panx != 0.0f || pany != 0.0f) {
        const float step = st.camera.distance * 0.012f;
        st.camera.target[0] += (rx * panx + ux * pany) * step;
        st.camera.target[1] += uy * pany * step;
        st.camera.target[2] += (rz * panx + uz * pany) * step;
    }

    // ── Live 3D preview ───────────────────────────────────────────────
    // Regenerate ONLY the edited object (throttled ~10 Hz) so the mesh reshapes
    // under the outline as you edit, with no scene-wide GPU churn. A final
    // full apply still runs in gm_end_inline_edit on finish. On a FAILED live
    // regen (transient degenerate polygon mid-drag) dirty stays set so the
    // finish apply retries and reports the error instead of losing edits.
    static double s_live_last = 0.0;
    const double now = ImGui::GetTime();
    if (st.gm_inline_dirty && (now - s_live_last > 0.1)) {
        if (gm_regenerate_object_geometry(st, idx, nullptr, /*live=*/true)) {
            s_live_last = now;
            st.gm_inline_dirty = false;
        }
    }
}

// Small 2D polygon helpers (clockwise -> counter-clockwise for boulder).
static double gm_polygon_area(const std::vector<boulder::PolygonPoint>& pts) {
    double a = 0.0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1) % pts.size();
        a += pts[i].x * pts[j].y - pts[j].x * pts[i].y;
    }
    return a * 0.5;
}

static void gm_ensure_ccw(std::vector<boulder::PolygonPoint>& pts) {
    if (gm_polygon_area(pts) < 0.0) std::reverse(pts.begin(), pts.end());
}

static void gm_simplify_freehand(ViewerState& st) {
    if (st.gm_points.size() < 3) return;
    const auto source = st.gm_points;
    const double tolerance_sq = (double)st.gm_simplify * st.gm_simplify;
    std::vector<unsigned char> keep(source.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> ranges = {{0, source.size() - 1}};
    while (!ranges.empty()) {
        const auto range = ranges.back();
        ranges.pop_back();
        const auto& a = source[range.first];
        const auto& b = source[range.second];
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double length_sq = vx * vx + vy * vy;
        double farthest = tolerance_sq;
        size_t split = range.first;
        for (size_t i = range.first + 1; i < range.second; ++i) {
            double t = length_sq > 1e-12
                ? ((source[i].x - a.x) * vx + (source[i].y - a.y) * vy) / length_sq : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const double dx = source[i].x - (a.x + vx * t);
            const double dy = source[i].y - (a.y + vy * t);
            const double distance_sq = dx * dx + dy * dy;
            if (distance_sq > farthest) { farthest = distance_sq; split = i; }
        }
        if (split != range.first) {
            keep[split] = 1;
            ranges.push_back({range.first, split});
            ranges.push_back({split, range.second});
        }
    }
    std::vector<boulder::PolygonPoint> simplified;
    for (size_t i = 0; i < source.size(); ++i) if (keep[i]) simplified.push_back(source[i]);
    const int cap = st.gm_target_points > 0 ? std::max(3, st.gm_target_points) : 96;
    if ((int)simplified.size() > cap) {
        std::vector<boulder::PolygonPoint> capped;
        capped.reserve(cap);
        for (int i = 0; i < cap; ++i) {
            const size_t index = (size_t)std::lround((double)i * (simplified.size() - 1) / (cap - 1));
            capped.push_back(simplified[index]);
        }
        simplified = std::move(capped);
    }
    if (simplified.size() >= 3) st.gm_points = std::move(simplified);
}

// Free cached object-browser thumbnail GL textures/FBOs.
static void obj_browser_free_thumbs(ViewerState& st) {
    for (GLuint tex : st.obj_browser_thumbs)
        if (tex) glDeleteTextures(1, &tex);
    st.obj_browser_thumbs.clear();
    st.obj_browser_thumb_next = 0;
}

// Scan resources + scene dirs for *.pod and *.swdm assets.
// Render a POD model into a small thumbnail texture (SMM2-style mini-render).
static GLuint obj_browser_make_thumb(const std::string& pod_path) {
    av::PODModel model = av::pod_load(pod_path);
    if (model.meshes.empty()) return 0;

    const int TW = 128, TH = 128;
    unsigned int fbo_tex = 0;
    unsigned int fbo = av::create_fbo(TW, TH, &fbo_tex);
    if (!fbo || !fbo_tex) return 0;

    // Upload the meshes (no textures — thumbnails are lit flat-shaded).
    std::vector<av::GPUMesh> gpu;
    for (auto& m : model.meshes) {
        gpu.push_back(av::upload_mesh(
            m.positions.empty() ? nullptr : m.positions.data(),
            m.normals.empty()   ? nullptr : m.normals.data(),
            m.uvs.empty()       ? nullptr : m.uvs.data(),
            m.num_vertices,
            m.indices.empty() ? nullptr : m.indices.data(),
            (int)m.indices.size()));
    }

    av::Camera cam;
    cam.yaw = 40.0f;
    cam.pitch = 28.0f;
    cam.target[0] = model.center_x;
    cam.target[1] = model.center_y;
    cam.target[2] = model.center_z;
    cam.distance = model.radius * 2.4f;
    if (cam.distance < 1.0f) cam.distance = 3.0f;
    cam.near_plane = 0.01f;
    cam.far_plane = 1000.0f;

    float old_clear[3] = {av::g_clear_color[0], av::g_clear_color[1], av::g_clear_color[2]};
    av::g_clear_color[0] = 0.10f; av::g_clear_color[1] = 0.11f; av::g_clear_color[2] = 0.14f;

    av::begin_3d(fbo, TW, TH, cam);
    float identity[16];
    av::mat4_identity(identity);
    float white[4] = {0.95f, 0.95f, 0.95f, 1.0f};
    for (auto& gm : gpu) {
        gm.texture_id = 0;
        av::render_mesh(gm, identity, white, false);
    }
    av::end_3d();

    av::g_clear_color[0] = old_clear[0];
    av::g_clear_color[1] = old_clear[1];
    av::g_clear_color[2] = old_clear[2];

    // Read back the pixels into a standalone texture.
    GLuint thumb = 0;
    std::vector<uint8_t> pixels(TW * TH * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Flip vertically (FBO origin is bottom-left; ImGui expects top-left).
    std::vector<uint8_t> flipped(TW * TH * 4);
    for (int y = 0; y < TH; ++y)
        std::memcpy(flipped.data() + y * TW * 4,
                    pixels.data() + (TH - 1 - y) * TW * 4, TW * 4);
    glGenTextures(1, &thumb);
    glBindTexture(GL_TEXTURE_2D, thumb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // restore default
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    for (auto& gm : gpu) av::free_mesh(gm);
    av::delete_fbo(fbo, fbo_tex);
    return thumb;
}

static void obj_browser_scan(ViewerState& st) {
    st.obj_browser_pods.clear();
    st.obj_browser_swdm.clear();
    obj_browser_free_thumbs(st);

    std::vector<fs::path> roots;
    if (!st.scene.filepath.empty())
        roots.push_back(fs::path(st.scene.filepath).parent_path());
    roots.push_back(fs::path(g_assets_dir) / "resources");
    roots.push_back(fs::path(expand_home("~/.local/share/swordigo-desktop/assets/resources")));
    roots.push_back(fs::path(expand_home("~/.local/share/swordigo-desktop/assets")));

    std::set<std::string> seen;
    std::error_code ec;
    for (const auto& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".pod") {
                std::string key = it->path().filename().string();
                if (seen.insert(key).second) st.obj_browser_pods.push_back(it->path().string());
            } else if (ext == ".swdm") {
                std::string key = it->path().filename().string();
                if (seen.insert(key).second) st.obj_browser_swdm.push_back(it->path().string());
            }
        }
    }
    std::sort(st.obj_browser_pods.begin(), st.obj_browser_pods.end());
    std::sort(st.obj_browser_swdm.begin(), st.obj_browser_swdm.end());

    // Mini-renders are generated lazily across frames (see draw_object_browser)
    // so opening the browser never blocks the UI thread.
    st.obj_browser_thumbs.assign(st.obj_browser_pods.size(), 0);
    st.obj_browser_thumb_next = 0;
    st.obj_browser_scanned = true;
}

// Build a SceneObject that references a POD model (ClassName 'Model').
static av::SceneObject build_pod_object(const std::string& pod_path, const std::string& identifier) {
    const std::string stem = fs::path(pod_path).stem().string();
    av::SceneObject obj;
    obj.template_name = "SceneObject";
    obj.name = identifier;
    proto::Writer payload;   // ModelComponent{ Name: 1 }
    payload.write_string_field(1, stem);
    proto::Writer wrapper;   // Component{ ClassName, Identifier:101, ModelComponent }
    wrapper.write_string_field(1, "Model");
    wrapper.write_varint_field(2, 101);
    wrapper.write_nested_field(101, payload);
    av::SceneComponent comp;
    comp.type_name = "Model";
    comp.type_id = 101;
    comp.payload_field = 101;
    comp.raw_data = wrapper.to_string();
    obj.components.push_back(std::move(comp));
    obj.mesh_name = stem;
    return obj;
}

// Add a POD model or .swdm ground mesh to the open scene.
static void obj_browser_add(ViewerState& st, const std::string& path, bool is_pod) {
    if (st.scene.filepath.empty()) { st.status_msg = "Open a scene first."; return; }
    if (!is_pod) {
        // .swdm: load sketch into the generator and inject immediately.
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        boulder::GroundMesh gm = boulder::parse_ground_mesh(content);
        if (gm.polygon.size() < 3) { st.status_msg = "Invalid .swdm file."; return; }
        st.gm_points = gm.polygon;
        st.gm_min_depth = gm.min_depth;
        st.gm_max_depth = gm.max_depth;
        st.gm_top_angle = gm.top_angle;
        st.gm_generate_top = gm.generate_top;
        st.gm_z = gm.z;
        snprintf(st.gm_top_tex, sizeof(st.gm_top_tex), "%s", gm.top_texture.c_str());
        snprintf(st.gm_bottom_tex, sizeof(st.gm_bottom_tex), "%s", gm.bottom_texture.c_str());
        snprintf(st.gm_obj_name, sizeof(st.gm_obj_name), "%s",
                 fs::path(path).stem().string().c_str());
        gm_add_to_scene(st);
        open_ground_mesh_studio(st);
        return;
    }
    snapshot_scene(st);
    av::SceneObject obj = build_pod_object(path, av::scene_fresh_identifier(st.scene));
    // Spawn where the user is looking (camera focus), so the fresh object
    // appears right in front of the view instead of at the world origin.
    obj.pos_x = st.camera.target[0];
    obj.pos_y = st.camera.target[1];
    obj.pos_z = st.camera.target[2];
    st.scene.objects.push_back(std::move(obj));
    av::scene_refresh(st.scene);
    const fs::path scene_dir = fs::path(st.scene.filepath).parent_path();
    if (load_scene_model_to_cache(st, obj.mesh_name, scene_dir.string())) {
        const int idx = static_cast<int>(st.scene.objects.size()) - 1;
        select_scene_object(st, idx);
        st.scene_dirty = true;
        st.status_msg = "Added model '" + obj.mesh_name + "' to scene.";
    } else {
        // Model couldn't be resolved — keep the object but report it.
        const int idx = static_cast<int>(st.scene.objects.size()) - 1;
        select_scene_object(st, idx);
        st.scene_dirty = true;
        st.status_msg = "Added '" + obj.mesh_name + "' (model file not found locally).";
    }
    // Jump to the Visual editor and frame the new object so it can be grabbed.
    switch_scene_tab(st, 1);
    frame_scene_selection(st);
}

// ── Ground Mesh Generator: live 3D preview ──
// Rebuild the extruded mesh from the current sketch and upload it for the
// side-by-side 3D viewport. Falls back gracefully when the sketch is too
// small to generate (keeps the last valid preview).
static void gm_rebuild_preview(ViewerState& st) {
    st.gm_preview_valid = false;
    if (st.gm_points.size() < 3 || st.scene.filepath.empty()) return;

    const std::string swdm = gm_build_swdm_text(st);
    std::string bin = boulder::generate_ground_mesh_object(swdm, "gm_preview", st.gm_z);
    if (bin.empty()) return;

    const std::string tmp_path = st.scene.filepath + ".ruby-gm-preview.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(bin.data(), static_cast<std::streamsize>(bin.size()));
        out.close();
    }
    av::SceneData parsed;
    try {
        parsed = av::scene_load(tmp_path);
    } catch (const std::exception&) {
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return;
    }
    std::error_code ec;
    fs::remove(tmp_path, ec);
    if (parsed.objects.empty()) return;

    const auto& obj = parsed.objects[0];
    if (obj.ground_meshes.empty()) return;

    for (auto& m : st.gm_preview_meshes) av::free_mesh(m);
    st.gm_preview_meshes.clear();
    for (auto t : st.gm_preview_textures) if (t) glDeleteTextures(1, &t);
    st.gm_preview_textures.clear();

    const fs::path scene_dir = fs::path(st.scene.filepath).parent_path();
    for (size_t gi = 0; gi < obj.ground_meshes.size(); ++gi) {
        const auto& mesh = obj.ground_meshes[gi];
        const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
        const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
        const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
        const uint32_t* idx_ptr = mesh.indices.empty() ? nullptr : mesh.indices.data();
        av::GPUMesh gm = av::upload_mesh(pos, nrm, uv, mesh.num_vertices,
                                          idx_ptr, (int)mesh.indices.size());
        st.gm_preview_meshes.push_back(gm);

        GLuint tex_id = 0;
        const std::string tex_name = gi < obj.ground_mesh_textures.size()
            ? obj.ground_mesh_textures[gi] : std::string();
        if (!tex_name.empty()) {
            const fs::path data_dir = fs::path(expand_home("~/.local/share/swordigo-desktop/assets"));
            std::string stem = fs::path(tex_name).stem().string();
            // Swordigo ships textures with a @2x suffix (fire_grass_2x.pvr),
            // so probe every plausible stem + suffix combination.
            const std::vector<fs::path> dirs = {
                scene_dir,
                scene_dir.parent_path(),
                scene_dir.parent_path() / "resources",
                fs::path(g_assets_dir) / "resources",
                data_dir / "resources",
                data_dir,
            };
            const std::vector<std::string> suffixes = {
                ".tex.png", "_2x.tex.png", ".pvr", "_2x.pvr", ".tex", "_2x.tex", ".png", "_2x.png"
            };
            std::vector<fs::path> candidates;
            candidates.push_back(scene_dir / tex_name);
            candidates.push_back(data_dir / "resources" / tex_name);
            for (const auto& dir : dirs)
                for (const auto& suf : suffixes)
                    candidates.push_back(dir / (stem + suf));
            for (const auto& cand : candidates) {
                if (fs::exists(cand)) {
                    tex_id = load_texture_file(cand.string());
                    if (tex_id) break;
                }
            }
        }
        st.gm_preview_textures.push_back(tex_id);
    }

    // Frame the preview camera on the mesh bounds.
    float min_p[3] = {1e9f, 1e9f, 1e9f}, max_p[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& mesh : obj.ground_meshes) {
        min_p[0] = std::min(min_p[0], mesh.min_x); min_p[1] = std::min(min_p[1], mesh.min_y);
        min_p[2] = std::min(min_p[2], mesh.min_z);
        max_p[0] = std::max(max_p[0], mesh.max_x); max_p[1] = std::max(max_p[1], mesh.max_y);
        max_p[2] = std::max(max_p[2], mesh.max_z);
    }
    st.gm_preview_cam = av::Camera{};
    st.gm_preview_cam.target[0] = (min_p[0] + max_p[0]) * 0.5f;
    st.gm_preview_cam.target[1] = (min_p[1] + max_p[1]) * 0.5f;
    st.gm_preview_cam.target[2] = (min_p[2] + max_p[2]) * 0.5f;
    const float dx = max_p[0]-min_p[0], dy = max_p[1]-min_p[1], dz = max_p[2]-min_p[2];
    st.gm_preview_cam.distance = std::max(3.0f, std::sqrt(dx*dx+dy*dy+dz*dz) * 1.6f);
    st.gm_preview_cam.yaw = -35.0f;
    st.gm_preview_cam.pitch = 24.0f;
    st.gm_preview_valid = true;
}

// ── Ground Mesh Generator window (SMM2-style 2D sketch) ──
static void draw_ground_mesh_generator(ViewerState& st) {

    ImGuiIO& io = ImGui::GetIO();
    // Professional hotkeys: V move · E add · D erase · B freehand · F frame.
    // Only honored when the generator window itself has focus, so they never
    // steal keystrokes from the scene viewport or other panels.
    const bool gm_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool do_frame_key = gm_focused && ImGui::IsKeyPressed(ImGuiKey_F) && !io.WantTextInput;
    if (gm_focused && ImGui::IsKeyPressed(ImGuiKey_V) && !io.WantTextInput) st.gm_tool = 0;
    if (gm_focused && ImGui::IsKeyPressed(ImGuiKey_E) && !io.WantTextInput) st.gm_tool = 1;
    if (gm_focused && ImGui::IsKeyPressed(ImGuiKey_D) && !io.WantTextInput) st.gm_tool = 2;
    if (gm_focused && ImGui::IsKeyPressed(ImGuiKey_B) && !io.WantTextInput) st.gm_tool = 3;
    if (gm_focused && ImGui::IsKeyPressed(ImGuiKey_H) && !io.WantTextInput) st.gm_tool = 4;
    if (gm_focused && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !io.WantTextInput && !st.gm_points.empty()) {
        st.gm_points.pop_back();
        st.gm_sketch_dirty = true;
    }

    // Rebuild the live 3D preview whenever the sketch / extrusion changed.
    // Throttle to ~7 Hz so continuous slider drags don't rebuild every frame.
    if (st.gm_sketch_dirty && st.gm_show_3d) {
        static double s_gm_last_rebuild = 0.0;
        const double now = ImGui::GetTime();
        if (now - s_gm_last_rebuild > 0.15 || !st.gm_preview_valid) {
            gm_rebuild_preview(st);
            s_gm_last_rebuild = now;
            st.gm_sketch_dirty = false;
        } else if (!ImGui::IsMouseDragging(0) && !ImGui::IsMouseDragging(1) &&
                   !ImGui::IsMouseDragging(2)) {
            // Drag ended — snap the preview up to date immediately.
            gm_rebuild_preview(st);
            s_gm_last_rebuild = now;
            st.gm_sketch_dirty = false;
        }
    }

    // Reserve bottom space for the properties + action bars (Depth section
    // takes two rows plus the action bar, so give it generous room).
    const float props_h = 210.0f;
    const float tools_w = 98.0f;
    ImVec2 win_avail = ImGui::GetContentRegionAvail();
    float canvas_h = win_avail.y - props_h;
    if (canvas_h < 170.0f) canvas_h = 170.0f;

    // Split the workspace: 2D sketch canvas + (optional) live 3D preview.
    // The preview gets a healthy share (and the Mesh tab now spans the full
    // window, so it can be comfortably wide instead of a cramped box).
    float canvas_w = win_avail.x - tools_w - 12.0f;
    float preview_w = 0.0f;
    if (st.gm_show_3d) {
        preview_w = std::clamp(canvas_w * 0.45f, 240.0f, 780.0f);
        canvas_w -= preview_w + 6.0f;
    }
    if (canvas_w < 260.0f) canvas_w = 260.0f;

    // ── Left tools rail ──
    ImGui::BeginChild("##gm_tools", ImVec2(tools_w, canvas_h), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Tools");
    ImGui::Separator();
    auto tool_button = [&](int id, const char* icon, const char* label) {
        if (st.gm_tool == id) ImGui::PushStyleColor(ImGuiCol_Button, th_mix(g_theme.surface_active, g_theme.accent, 0.35f));
        if (ImGui::Button(icon, ImVec2(tools_w - 14.0f, 24))) st.gm_tool = id;
        if (st.gm_tool == id) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
        const float cx = ImGui::GetCursorPosX() + (tools_w - 14.0f - ImGui::CalcTextSize(label).x) * 0.5f;
        ImGui::SetCursorPosX(cx);
        ImGui::TextDisabled("%s", label);
    };
    tool_button(0, ICON_FA_HAND_POINTER, "Move  (V)");
    tool_button(1, ICON_FA_PLUS, "Add  (E)");
    tool_button(2, ICON_FA_TRASH, "Erase (D)");
    tool_button(4, ICON_FA_MOUNTAIN_SUN, "Hat  (H)");
    ImGui::Separator();
    ImGui::Checkbox("Mirror X", &st.gm_mirror_x);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mirror new points across the Y axis");
    ImGui::Checkbox("3D Preview", &st.gm_show_3d);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show a live 3D view of the extruded mesh beside the sketch");
    if (st.gm_show_3d && !st.gm_preview_valid && st.gm_points.size() >= 3)
        st.gm_sketch_dirty = true;
    ImGui::Separator();
    ImGui::TextDisabled("%d pts", (int)st.gm_points.size());
    if (st.gm_points.size() >= 3) {
        const double depth = std::fabs((double)st.gm_max_depth - (double)st.gm_min_depth);
        ImGui::TextDisabled("depth %.0f", depth);
        ImGui::TextDisabled("top %.0f\xc2\xb0", st.gm_top_angle);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ── 2D sketch canvas ──
    ImGui::BeginChild("##gm_canvas_host", ImVec2(canvas_w, canvas_h), ImGuiChildFlags_Borders);
    ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##gm_canvas", ImVec2(canvas_w, canvas_h - 8.0f));
    // Shared framing: fit the whole sketch polygon into the canvas.
    auto gm_frame_sketch = [&]() {
        if (st.gm_points.empty()) return;
        double minx = st.gm_points[0].x, maxx = st.gm_points[0].x;
        double miny = st.gm_points[0].y, maxy = st.gm_points[0].y;
        for (auto& p : st.gm_points) {
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
        }
        st.gm_canvas_cx = (minx + maxx) / 2.0;
        st.gm_canvas_cy = (miny + maxy) / 2.0;
        st.gm_canvas_scale = std::clamp((float)(0.8 * canvas_w / std::max(maxx - minx, maxy - miny)),
                                        0.05f, 50.0f);
    };
    const bool canvas_hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Map world <-> screen: y is world-up, screen y grows down.
    auto to_screen = [&](double wx, double wy) {
        float sx = canvas_origin.x + (float)((wx - st.gm_canvas_cx) * st.gm_canvas_scale);
        float sy = canvas_origin.y + canvas_h * 0.5f - (float)((wy - st.gm_canvas_cy) * st.gm_canvas_scale);
        return ImVec2(sx, sy);
    };
    auto to_world = [&](ImVec2 sp) {
        double wx = st.gm_canvas_cx + (sp.x - canvas_origin.x) / st.gm_canvas_scale;
        double wy = st.gm_canvas_cy + (canvas_origin.y + canvas_h * 0.5f - sp.y) / st.gm_canvas_scale;
        return std::make_pair(wx, wy);
    };

    // Zoom with wheel; pan with middle mouse.
    if (canvas_hovered && io.MouseWheel != 0.0f) {
        const float factor = (io.MouseWheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
        const ImVec2 mouse = ImGui::GetMousePos();
        auto before = to_world(mouse);
        st.gm_canvas_scale = std::clamp(st.gm_canvas_scale * factor, 0.02f, 50.0f);
        auto after = to_world(mouse);
        st.gm_canvas_cx += before.first - after.first;
        st.gm_canvas_cy += before.second - after.second;
    }
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        auto d = io.MouseDelta;
        st.gm_canvas_cx -= d.x / st.gm_canvas_scale;
        st.gm_canvas_cy += d.y / st.gm_canvas_scale;
    }

    // Procedural 1-2-5 grid: minor lines stay crisp while major spacing and
    // coordinate labels adapt continuously to the current zoom.
    dl->AddRectFilled(canvas_origin, ImVec2(canvas_origin.x + canvas_w, canvas_origin.y + canvas_h),
                      ImGui::ColorConvertFloat4ToU32(g_theme.background));
    const double target_world = 90.0 / std::max(0.02f, st.gm_canvas_scale);
    const double decade = std::pow(10.0, std::floor(std::log10(target_world)));
    const double normalized = target_world / decade;
    const double major_step = (normalized < 2.0 ? 2.0 : normalized < 5.0 ? 5.0 : 10.0) * decade;
    const double minor_step = major_step / 5.0;
    const double min_x = st.gm_canvas_cx;
    const double max_x = st.gm_canvas_cx + canvas_w / st.gm_canvas_scale;
    const double half_world_h = canvas_h * 0.5 / st.gm_canvas_scale;
    const double min_y = st.gm_canvas_cy - half_world_h;
    const double max_y = st.gm_canvas_cy + half_world_h;
    const ImU32 minor_col = g_theme.dark ? IM_COL32(88, 96, 112, 46) : IM_COL32(70, 78, 92, 34);
    const ImU32 major_col = g_theme.dark ? IM_COL32(116, 126, 145, 92) : IM_COL32(70, 78, 92, 70);
    const ImU32 label_col = ImGui::ColorConvertFloat4ToU32(g_theme.text_muted);
    char lbl[32];
    for (double gx = std::floor(min_x / minor_step) * minor_step; gx <= max_x; gx += minor_step) {
        float sx = to_screen(gx, 0).x;
        const double major_index = std::round(gx / major_step);
        const bool major = std::fabs(gx - major_index * major_step) < minor_step * 0.01;
        dl->AddLine(ImVec2(sx, canvas_origin.y), ImVec2(sx, canvas_origin.y + canvas_h),
                    major ? major_col : minor_col, major ? 1.0f : 1.0f);
        if (major && std::fabs(gx) > minor_step * 0.01) {
            snprintf(lbl, sizeof(lbl), major_step < 1.0 ? "%.2f" : "%.0f", gx);
            dl->AddText(ImVec2(sx + 4.0f, canvas_origin.y + canvas_h - 20.0f), label_col, lbl);
        }
    }
    for (double gy = std::floor(min_y / minor_step) * minor_step; gy <= max_y; gy += minor_step) {
        float sy = to_screen(0, gy).y;
        const double major_index = std::round(gy / major_step);
        const bool major = std::fabs(gy - major_index * major_step) < minor_step * 0.01;
        dl->AddLine(ImVec2(canvas_origin.x, sy), ImVec2(canvas_origin.x + canvas_w, sy),
                    major ? major_col : minor_col, major ? 1.0f : 1.0f);
        if (major && std::fabs(gy) > minor_step * 0.01) {
            snprintf(lbl, sizeof(lbl), major_step < 1.0 ? "%.2f" : "%.0f", gy);
            dl->AddText(ImVec2(canvas_origin.x + 5.0f, sy - ImGui::GetTextLineHeight()), label_col, lbl);
        }
    }
    if (min_x <= 0.0 && max_x >= 0.0)
        dl->AddLine(ImVec2(to_screen(0, 0).x, canvas_origin.y),
                    ImVec2(to_screen(0, 0).x, canvas_origin.y + canvas_h), IM_COL32(74, 176, 96, 220), 1.5f);
    if (min_y <= 0.0 && max_y >= 0.0)
        dl->AddLine(ImVec2(canvas_origin.x, to_screen(0, 0).y),
                    ImVec2(canvas_origin.x + canvas_w, to_screen(0, 0).y), IM_COL32(214, 76, 78, 220), 1.5f);

    // Polygon fill + outline + top-segment hints.
    if (st.gm_points.size() >= 3) {
        std::vector<ImVec2> pts;
        for (auto& p : st.gm_points) pts.push_back(to_screen(p.x, p.y));
        dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), IM_COL32(70, 130, 90, 70));
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(120, 210, 150, 255), false, 2.0f);
        dl->AddLine(pts.back(), pts.front(), IM_COL32(120, 210, 150, 255), 2.0f);
        // Top-segment hints (flat-ish edges get the surface mesh).
        for (size_t i = 0; i < st.gm_points.size(); ++i) {
            const size_t a = i, b = (i + 1) % st.gm_points.size();
            double ang = std::atan2(st.gm_points[b].y - st.gm_points[a].y,
                                    st.gm_points[b].x - st.gm_points[a].x) * 180.0 / M_PI;
            if (ang < 0.0) ang += 360.0;
            if (std::abs(ang - 180.0) < st.gm_top_angle) {
                dl->AddLine(to_screen(st.gm_points[a].x, st.gm_points[a].y),
                            to_screen(st.gm_points[b].x, st.gm_points[b].y),
                            IM_COL32(255, 200, 80, 220), 3.0f);
            }
        }
    }
    // Vertex handles with index labels.
    int hover = -1;
    for (int i = 0; i < (int)st.gm_points.size(); ++i) {
        ImVec2 sp = to_screen(st.gm_points[i].x, st.gm_points[i].y);
        const bool inside = sp.x >= canvas_origin.x && sp.x <= canvas_origin.x + canvas_w &&
                            sp.y >= canvas_origin.y && sp.y <= canvas_origin.y + canvas_h;
        if (!inside) continue;
        const bool near_mouse = canvas_hovered &&
            std::fabs(ImGui::GetMousePos().x - sp.x) <= 9.0f &&
            std::fabs(ImGui::GetMousePos().y - sp.y) <= 9.0f;
        if (near_mouse) hover = i;
        ImU32 col = IM_COL32(255, 130, 70, 255);
        if (i == st.gm_drag_point) col = IM_COL32(255, 255, 120, 255);
        else if (near_mouse) col = IM_COL32(255, 190, 120, 255);
        const float r = (i == st.gm_drag_point || near_mouse) ? 6.5f : 5.0f;
        dl->AddCircleFilled(sp, r, col);
        dl->AddCircle(sp, r, IM_COL32(20, 20, 30, 255), 0, 1.5f);
        snprintf(lbl, sizeof(lbl), "%d", i);
        dl->AddText(ImVec2(sp.x + 7.0f, sp.y - 11.0f), IM_COL32(225, 235, 245, 190), lbl);
    }

    // ── Round-hat domes: footprint circle + radius handle + apex marker ──
    int hat_hover = -1, hat_handle_hit = -1;
    for (int i = 0; i < (int)st.gm_hats.size(); ++i) {
        const auto& h = st.gm_hats[i];
        const ImVec2 c = to_screen(h.x, h.y);
        const float rr = (float)(h.radius * st.gm_canvas_scale);
        const bool inside = c.x >= canvas_origin.x && c.x <= canvas_origin.x + canvas_w &&
                            c.y >= canvas_origin.y && c.y <= canvas_origin.y + canvas_h;
        if (!inside) continue;
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool near_center = canvas_hovered &&
            std::fabs(mouse.x - c.x) <= 12.0f && std::fabs(mouse.y - c.y) <= 12.0f;
        const ImVec2 handle = ImVec2(c.x + rr, c.y);
        const bool near_handle = canvas_hovered &&
            std::fabs(mouse.x - handle.x) <= 8.0f && std::fabs(mouse.y - handle.y) <= 8.0f;
        if (near_center) hat_hover = i;
        if (near_handle) { hat_handle_hit = i; }
        const bool active = (i == st.gm_drag_hat) || near_center || near_handle;
        dl->AddCircle(c, rr, active ? IM_COL32(255, 170, 60, 255)
                                    : IM_COL32(210, 150, 110, 190), 0, 2.0f);
        dl->AddLine(ImVec2(c.x - 9, c.y), ImVec2(c.x + 9, c.y), IM_COL32(210, 150, 110, 190));
        dl->AddLine(ImVec2(c.x, c.y - 9), ImVec2(c.x, c.y + 9), IM_COL32(210, 150, 110, 190));
        dl->AddCircleFilled(c, 3.5f, IM_COL32(235, 175, 95, 255));
        dl->AddCircleFilled(handle, 4.5f, IM_COL32(255, 120, 70, 255));      // radius handle
        const ImVec2 apex = ImVec2(c.x, c.y - (float)(h.height * st.gm_canvas_scale));
        dl->AddLine(c, apex, IM_COL32(210, 150, 110, 130));
        dl->AddCircleFilled(apex, 3.0f, IM_COL32(150, 205, 120, 230));       // apex (height)
    }

    // Mouse interactions (tool-aware).
    if (canvas_hovered) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (st.gm_tool == 4) {                       // Hat: grab / place
                if (hat_handle_hit >= 0) {
                    st.gm_drag_hat = hat_handle_hit;
                    st.gm_hat_resizing = true;
                    st.gm_hat_dragging = false;
                } else if (hat_hover >= 0) {
                    st.gm_drag_hat = hat_hover;
                    st.gm_hat_dragging = true;
                } else {
                    auto w = to_world(mouse);
                    st.gm_hats.push_back({w.first, w.second,
                                          st.gm_hat_radius, st.gm_hat_height});
                    st.gm_drag_hat = (int)st.gm_hats.size() - 1;
                    st.gm_hat_dragging = true;
                    st.gm_sketch_dirty = true;
                }
            } else if (st.gm_tool == 0 && hat_hover >= 0) {
                st.gm_drag_hat = hat_hover;              // Move tool: grab a hat
                st.gm_hat_dragging = true;
            } else if (st.gm_tool == 3) {                // Direct freehand outline
                if (!io.KeyShift) st.gm_points.clear();
                auto w = to_world(mouse);
                st.gm_points.push_back({w.first, w.second});
                st.gm_freehand_active = true;
                st.gm_sketch_dirty = true;
            } else if (st.gm_tool == 2) {                // Erase
                if (hover >= 0) {
                    st.gm_points.erase(st.gm_points.begin() + hover);
                    st.gm_drag_point = -1;
                    st.gm_sketch_dirty = true;
                }
            } else if (hover >= 0 && st.gm_tool == 0) {  // Move: drag existing
                st.gm_drag_point = hover;
                st.gm_dragging = true;
            } else {                                     // Add point
                auto w = to_world(mouse);
                st.gm_points.push_back({w.first, w.second});
                if (st.gm_mirror_x) st.gm_points.push_back({-w.first, w.second});
                st.gm_drag_point = (int)st.gm_points.size() - 1;
                st.gm_dragging = (st.gm_tool == 0);
                st.gm_sketch_dirty = true;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (hat_hover >= 0) {                        // RMB on a hat → remove it
                st.gm_hats.erase(st.gm_hats.begin() + hat_hover);
                st.gm_drag_hat = -1;
                st.gm_sketch_dirty = true;
            } else if (hover >= 0) {
                st.gm_points.erase(st.gm_points.begin() + hover);
                st.gm_drag_point = -1;
                st.gm_sketch_dirty = true;
            }
        }
    }
    if (st.gm_freehand_active && st.gm_tool == 3) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto w = to_world(ImGui::GetMousePos());
            if (st.gm_points.empty() || std::hypot(w.first - st.gm_points.back().x,
                                                   w.second - st.gm_points.back().y) >=
                                        std::max(0.25f, 3.0f / st.gm_canvas_scale)) {
                st.gm_points.push_back({w.first, w.second});
                st.gm_sketch_dirty = true;
            }
        } else {
            st.gm_freehand_active = false;
            gm_simplify_freehand(st);
        }
    }
    if (st.gm_dragging && st.gm_tool == 0 && st.gm_drag_point >= 0 &&
        st.gm_drag_point < (int)st.gm_points.size()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto w = to_world(ImGui::GetMousePos());
            st.gm_points[st.gm_drag_point] = {w.first, w.second};
            st.gm_sketch_dirty = true;
        } else {
            st.gm_dragging = false;
            st.gm_drag_point = -1;
        }
    }
    if (st.gm_dragging && st.gm_tool != 0) { st.gm_dragging = false; st.gm_drag_point = -1; }
    // Delete key removes the hovered point.
    if (canvas_hovered && hover >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        st.gm_points.erase(st.gm_points.begin() + hover);
        st.gm_sketch_dirty = true;
    }
    // ── Hat drag / resize loops ──
    if (st.gm_hat_dragging && st.gm_drag_hat >= 0 && st.gm_drag_hat < (int)st.gm_hats.size()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto w = to_world(ImGui::GetMousePos());
            st.gm_hats[st.gm_drag_hat].x = w.first;
            st.gm_hats[st.gm_drag_hat].y = w.second;
            st.gm_sketch_dirty = true;
        } else {
            st.gm_hat_dragging = false;
            st.gm_drag_hat = -1;
        }
    }
    if (st.gm_hat_resizing && st.gm_drag_hat >= 0 && st.gm_drag_hat < (int)st.gm_hats.size()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto w = to_world(ImGui::GetMousePos());
            auto& h = st.gm_hats[st.gm_drag_hat];
            h.radius = std::clamp(std::hypot(w.first - h.x, w.second - h.y), 5.0, 400.0);
            st.gm_sketch_dirty = true;
        } else {
            st.gm_hat_resizing = false;
            st.gm_drag_hat = -1;
        }
    }
    if ((st.gm_hat_dragging || st.gm_hat_resizing) && st.gm_tool != 0 && st.gm_tool != 4) {
        st.gm_hat_dragging = st.gm_hat_resizing = false;
        st.gm_drag_hat = -1;
    }
    if (canvas_hovered && hat_hover >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        st.gm_hats.erase(st.gm_hats.begin() + hat_hover);
        st.gm_sketch_dirty = true;
    }
    // Frame the sketch (F key).
    if (do_frame_key) gm_frame_sketch();

    // Canvas hint bar + live stats.
    const char* gm_hint =
        (st.gm_tool == 0) ? "LMB move \xc2\xb7 click empty = add \xc2\xb7 RMB/Del = remove \xc2\xb7 Wheel zoom \xc2\xb7 MMB pan"
        : (st.gm_tool == 1) ? "LMB add points \xc2\xb7 RMB/Del = remove \xc2\xb7 Wheel zoom \xc2\xb7 MMB pan"
        : (st.gm_tool == 2) ? "LMB erase points \xc2\xb7 RMB/Del = remove \xc2\xb7 Wheel zoom \xc2\xb7 MMB pan"
        : (st.gm_tool == 4) ? "LMB place a round-hat dome \xc2\xb7 drag center = move \xc2\xb7 drag orange handle = resize \xc2\xb7 RMB/Del = remove"
        : "Drag to draw outline \xc2\xb7 Shift+drag appends \xc2\xb7 release converts to polygon nodes";
    dl->AddRectFilled(ImVec2(canvas_origin.x, canvas_origin.y + canvas_h - 40.0f),
                      ImVec2(canvas_origin.x + canvas_w, canvas_origin.y + canvas_h),
                      IM_COL32(18, 21, 28, 215));
    dl->AddText(ImVec2(canvas_origin.x + 8, canvas_origin.y + canvas_h - 34.0f),
                IM_COL32(160, 170, 190, 255), gm_hint);
    char gm_stats[160];
    snprintf(gm_stats, sizeof(gm_stats),
             "%d points \xc2\xb7 Z %.1f \xc2\xb7 depth %.0f..%.0f \xc2\xb7 top %.0f\xc2\xb0",
             (int)st.gm_points.size(), st.gm_z, st.gm_min_depth, st.gm_max_depth, st.gm_top_angle);
    dl->AddText(ImVec2(canvas_origin.x + 8, canvas_origin.y + canvas_h - 18.0f),
                IM_COL32(140, 150, 170, 220), gm_stats);
    ImGui::EndChild(); // gm_canvas_host

    // ── Live 3D preview of the extruded mesh ──
    if (st.gm_show_3d) {
        ImGui::SameLine();
        ImGui::BeginChild("##gm_3d_host", ImVec2(preview_w, canvas_h), ImGuiChildFlags_Borders);
        ImGui::TextDisabled(ICON_FA_CUBE " 3D Preview");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.gm_preview_valid ? "live" : "(build...)");
        ImGui::Separator();

        int pw = (int)ImGui::GetContentRegionAvail().x;
        int ph = (int)ImGui::GetContentRegionAvail().y;
        if (pw < 4) pw = 4;
        if (ph < 4) ph = 4;
        if (!st.gm_preview_fbo) {
            st.gm_preview_fbo = av::create_fbo(pw, ph, &st.gm_preview_fbo_tex);
            st.gm_preview_w = pw; st.gm_preview_h = ph;
        } else if (pw != st.gm_preview_w || ph != st.gm_preview_h) {
            av::resize_fbo(st.gm_preview_fbo, pw, ph, &st.gm_preview_fbo_tex);
            st.gm_preview_w = pw; st.gm_preview_h = ph;
        }

        if (st.gm_preview_valid && !st.gm_preview_meshes.empty()) {
            // Orbit: LMB drag · zoom: wheel (anchored at the pivot point).
            const bool prev_hovered = ImGui::IsItemHovered() || ImGui::IsWindowHovered();
            if (prev_hovered && io.MouseWheel != 0.0f) {
                const float factor = std::pow(0.94f, io.MouseWheel);
                st.gm_preview_cam.distance = std::clamp(st.gm_preview_cam.distance * factor,
                                                        0.5f, 2000.0f);
            }
            if (prev_hovered && ImGui::IsMouseDragging(0)) {
                st.gm_preview_cam.yaw   += io.MouseDelta.x * 0.5f;
                st.gm_preview_cam.pitch += io.MouseDelta.y * 0.5f;
                st.gm_preview_cam.pitch = std::clamp(st.gm_preview_cam.pitch, -89.0f, 89.0f);
            }
            if (prev_hovered && ImGui::IsMouseDragging(1)) {
                const float scale = st.gm_preview_cam.distance * 0.003f;
                st.gm_preview_cam.target[0] -= std::cos(st.gm_preview_cam.yaw * 3.14159f / 180.0f) * io.MouseDelta.x * scale;
                st.gm_preview_cam.target[2] += std::sin(st.gm_preview_cam.yaw * 3.14159f / 180.0f) * io.MouseDelta.x * scale;
                st.gm_preview_cam.target[1] += io.MouseDelta.y * scale;
            }

            // The sketch preview is a close-up editor view: no scene fog/lights.
            av::clear_point_lights();
            av::set_depth_fog(false, nullptr, 0.0f, 0.0f);
            av::begin_3d(st.gm_preview_fbo, pw, ph, st.gm_preview_cam);
            av::render_grid_xy(60.0f, st.gm_preview_cam.target[2] - st.gm_preview_cam.distance * 0.5f);
            float identity[16];
            av::mat4_identity(identity);
            for (size_t i = 0; i < st.gm_preview_meshes.size(); ++i) {
                auto& gm = st.gm_preview_meshes[i];
                gm.texture_id = (i < st.gm_preview_textures.size()) ? st.gm_preview_textures[i] : 0;
                float col[4] = {1, 1, 1, 1};
                av::render_mesh(gm, identity, col, false);
            }
            av::end_3d();

            const ImVec2 ppos = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)st.gm_preview_fbo_tex, ImVec2((float)pw, (float)ph),
                         ImVec2(0, 1), ImVec2(1, 0));
            // Preview hint bar.
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            pdl->AddRectFilled(ImVec2(ppos.x, ppos.y + ph - 22.0f),
                               ImVec2(ppos.x + pw, ppos.y + ph),
                               IM_COL32(18, 21, 28, 215));
            char hint[96];
            snprintf(hint, sizeof(hint),
                     "LMB orbit \xc2\xb7 Wheel zoom \xc2\xb7 RMB pan \xc2\xb7 Z %.0f", st.gm_z);
            pdl->AddText(ImVec2(ppos.x + 6, ppos.y + ph - 18.0f),
                         IM_COL32(150, 165, 190, 255), hint);
        } else {
            ImGui::TextDisabled("\nSketch a polygon (3+ points)\nto preview the extruded mesh.");
        }
        ImGui::EndChild(); // gm_3d_host
    }

    // ── Extrusion / Depth properties ──
    ImGui::Separator();
    ImGui::TextDisabled(ICON_FA_SLIDERS " Depth & Extrusion");
    ImGui::SameLine();
    ImGui::TextDisabled("(mesh depth = Z layer; thickness = Min..Max)");

    // Z layer (world depth where the mesh sits) — always visible & prominent.
    ImGui::SetNextItemWidth(150.0f);
    const float old_z = st.gm_z;
    ImGui::SliderFloat("Z (depth layer)", &st.gm_z, -100.0f, 300.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("World depth (parallax layer) where the generated ground mesh sits.\n"
                          "Real Swordigo levels use Z in the 30-50 range; pick the layer that\n"
                          "matches the objects around it.");
    }

    // Thickness quick control — keeps Min/Max symmetric around a center.
    float thickness = std::fabs(st.gm_max_depth - st.gm_min_depth);
    ImGui::SetNextItemWidth(150.0f);
    const float old_th = thickness;
    if (ImGui::SliderFloat("Thickness", &thickness, 0.0f, 400.0f, "%.1f")) {
        const float center = (st.gm_min_depth + st.gm_max_depth) * 0.5f;
        st.gm_min_depth = center - thickness * 0.5f;
        st.gm_max_depth = center + thickness * 0.5f;
    }
    ImGui::SameLine();
    bool gm_depth_changed = (st.gm_z != old_z || thickness != old_th);
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::DragFloat("MinDepth", &st.gm_min_depth, 1.0f, -500.0f, 500.0f, "%.1f"))
        gm_depth_changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::DragFloat("MaxDepth", &st.gm_max_depth, 1.0f, -500.0f, 500.0f, "%.1f"))
        gm_depth_changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(84.0f);
    if (ImGui::DragFloat("TopAngle", &st.gm_top_angle, 1.0f, 0.0f, 90.0f, "%.0f\xc2\xb0"))
        gm_depth_changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("GenerateTop", &st.gm_generate_top))
        gm_depth_changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Build the top/surface cap mesh on flat-top segments");

    // Any depth/extrusion change rebuilds the live preview (if visible).
    if (gm_depth_changed) st.gm_sketch_dirty = true;

    ImGui::SetNextItemWidth(150.0f);
    ImGui::SliderFloat("Draw accuracy", &st.gm_simplify, 0.1f, 20.0f, "%.1f units");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderInt("Point limit", &st.gm_target_points, 0, 256,
                     st.gm_target_points == 0 ? "Auto" : "%d");

    // ── Round hats (dome bumps on the surface) ──
    ImGui::SetNextItemWidth(132.0f);
    ImGui::SliderFloat("Hat radius", &st.gm_hat_radius, 5.0f, 300.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Hat height", &st.gm_hat_height, 5.0f, 200.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button("Clear hats")) { st.gm_hats.clear(); st.gm_sketch_dirty = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu hat%s \xc2\xb7 (H tool places them)",
                        st.gm_hats.size(), st.gm_hats.size() == 1 ? "" : "s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Round-hat domes sit on the polygon's top edge (max Y). "
                          "They render as rounded bumps in-game and in the 3D preview.");

    // ── Object + materials ──
    ImGui::SetNextItemWidth(172.0f);
    ImGui::InputText("Name", st.gm_obj_name, sizeof(st.gm_obj_name));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(152.0f);
    ImGui::InputText("TopTexture", st.gm_top_tex, sizeof(st.gm_top_tex));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(152.0f);
    ImGui::InputText("BottomTexture", st.gm_bottom_tex, sizeof(st.gm_bottom_tex));
    ImGui::SameLine();
    ImGui::TextDisabled(ICON_FA_CIRCLE_INFO " resolves from resources/");

    // ── Action bar ──
    ImGui::Separator();
    if (ImGui::Button(ICON_FA_PLUS " Add to Scene", ImVec2(132, 26))) gm_add_to_scene(st);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save .swdm", ImVec2(120, 26))) {
        std::string path = st.scene.filepath.empty()
            ? std::string("ground_mesh.swdm")
            : fs::path(st.scene.filepath).parent_path().string() + "/" + st.gm_obj_name + ".swdm";
        std::ofstream out(path, std::ios::trunc);
        if (out) { out << gm_build_swdm_text(st); st.status_msg = "Saved " + path; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load .swdm", ImVec2(100, 26))) ImGui::OpenPopup("Load .swdm");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Import selected", ImVec2(142, 26))) gm_import_from_scene(st);
    if (st.gm_edit_apply_object >= 0) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CHECK " Apply to Object", ImVec2(152, 26))) gm_apply_to_object(st);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK " Cancel", ImVec2(84, 26))) {
            st.gm_edit_apply_object = -1;
            st.gm_workspace_mode = 0;
            st.scene_mesh_edit = false;
            switch_scene_tab(st, 1);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear", ImVec2(78, 26))) { st.gm_points.clear(); st.gm_hats.clear(); st.gm_sketch_dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LOCATION_DOT " Frame", ImVec2(76, 26))) gm_frame_sketch();

    // Load .swdm popup.
    if (ImGui::BeginPopup("Load .swdm")) {
        static char load_buf[512] = {};
        ImGui::InputText("Path", load_buf, sizeof(load_buf));
        if (ImGui::Button("Load")) {
            std::ifstream f(load_buf, std::ios::binary);
            if (f) {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                boulder::GroundMesh gm = boulder::parse_ground_mesh(content);
                if (gm.polygon.size() >= 3) {
                    st.gm_points = gm.polygon;
                    st.gm_min_depth = gm.min_depth;
                    st.gm_max_depth = gm.max_depth;
                    st.gm_top_angle = gm.top_angle;
                    st.gm_generate_top = gm.generate_top;
                    st.gm_z = gm.z;
                    snprintf(st.gm_top_tex, sizeof(st.gm_top_tex), "%s", gm.top_texture.c_str());
                    snprintf(st.gm_bottom_tex, sizeof(st.gm_bottom_tex), "%s", gm.bottom_texture.c_str());
                    st.gm_sketch_dirty = true;
                    st.status_msg = "Loaded " + std::string(load_buf);
                } else {
                    st.status_msg = "Invalid .swdm file.";
                }
            } else {
                st.status_msg = "Cannot open " + std::string(load_buf);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

}

// ── Add a game-library template as a linked scene object ────────────────
// main.js `createObject(name)` parity: the object stores only the template
// name; scene_refresh() resolves components from the embedded .scl library.
static void template_add_to_scene(ViewerState& st, const std::string& template_name) {
    if (st.scene.filepath.empty()) { st.status_msg = "Open a scene first."; return; }
    snapshot_scene(st);
    const size_t idx = av::scene_create_object(st.scene, template_name);
    if (idx >= st.scene.objects.size()) {
        st.status_msg = "Failed to create object from template.";
        return;
    }
    auto& obj = st.scene.objects[idx];
    // Spawn in front of the camera (along target->eye), slightly toward the
    // viewer, so the fresh object is visible and immediately grabbable.
    const float pi = 3.14159265358979323846f;
    const float yaw = st.camera.yaw * pi / 180.0f;
    const float pitch = st.camera.pitch * pi / 180.0f;
    const float cosp = cosf(pitch);
    // dir points from target toward the eye (into the view).
    const float dir[3] = {cosp * sinf(yaw), sinf(pitch), cosp * cosf(yaw)};
    const float k = std::max(8.0f, st.camera.distance * 0.22f);
    obj.pos_x = st.camera.target[0] + dir[0] * k;
    obj.pos_y = st.camera.target[1] + dir[1] * k;
    obj.pos_z = st.camera.target[2] + dir[2] * k;
    av::scene_refresh(st.scene);
    select_scene_object(st, (int)idx);
    st.scene_dirty = true;
    st.status_msg = "Added template '" + template_name + "' to scene (components inherited).";
    switch_scene_tab(st, 1);
    frame_scene_selection(st);
}

// ── Object Browser window (SMM2-style palette) ──
static void draw_object_browser(ViewerState& st) {
    if (!st.obj_browser_open) return;
    if (!st.obj_browser_scanned || st.obj_browser_dir != st.scene.filepath) {
        obj_browser_scan(st);
        st.obj_browser_dir = st.scene.filepath;
    }

    // Lazy thumbnail generation: render a few mini-renders per frame so the
    // first open never blocks on dozens of POD model loads.
    {
        int budget = 2;
        while (budget-- > 0 && st.obj_browser_thumb_next < st.obj_browser_pods.size()) {
            const size_t i = st.obj_browser_thumb_next++;
            if (i >= st.obj_browser_thumbs.size()) break;
            st.obj_browser_thumbs[i] = obj_browser_make_thumb(st.obj_browser_pods[i]);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin(ICON_FA_CUBE " Add Object", &st.obj_browser_open);

    ImGui::InputTextWithHint("##obj_search", "Search objects...", st.obj_browser_search,
                             sizeof(st.obj_browser_search));
    std::string q(st.obj_browser_search);
    for (auto& c : q) c = (char)tolower((unsigned char)c);

    if (st.obj_browser_pods.empty() && st.obj_browser_swdm.empty())
        ImGui::TextDisabled("No .pod or .swdm files found under the resources directory.");

    ImGui::Separator();

    // ── Game templates from the scene's embedded ObjectLibrary (.scl) ──
    // main.js ObjectLibrary parity: categorized palette of the scene's own
    // templates, added as linked objects (components inherited, editable).
    {
        static std::string s_tpl_key;
        static std::vector<av::SceneTemplateInfo> s_tpls;
        // Content hash: library byte sizes summed, so re-saving a scene with
        // the same library *counts* still refreshes the palette.
        size_t lib_hash = 0;
        for (const auto& lib : st.scene.object_libraries) lib_hash += lib.size();
        for (const auto& lib : st.scene.external_libraries) lib_hash += lib.size();
        const std::string key = st.scene.filepath + "#" +
                                std::to_string(st.scene.object_libraries.size()) + "#" +
                                std::to_string(st.scene.external_libraries.size()) + "#" +
                                std::to_string(lib_hash);
        if (key != s_tpl_key) {
            s_tpl_key = key;
            s_tpls = av::scene_list_templates(st.scene);
        }
        if (!s_tpls.empty()) {
            if (ImGui::CollapsingHeader(
                    (std::string(ICON_FA_BOX " Game Templates (") +
                     std::to_string(s_tpls.size()) + ")").c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string tq(q);
                static const swk::ObjCategory kOrder[] = {
                    swk::ObjCategory::Enemies, swk::ObjCategory::Entities,
                    swk::ObjCategory::Items, swk::ObjCategory::Geometry,
                    swk::ObjCategory::Effects, swk::ObjCategory::Lighting,
                    swk::ObjCategory::Controllers, swk::ObjCategory::Audio,
                    swk::ObjCategory::Portals, swk::ObjCategory::Utility,
                    swk::ObjCategory::Other
                };
                const float tpl_h = std::min(ImGui::GetContentRegionAvail().y, 420.0f);
                if (ImGui::BeginChild("##tpl_list", ImVec2(0, tpl_h), ImGuiChildFlags_Borders)) {
                    for (int ci = 0; ci < 11; ++ci) {
                        const swk::ObjCategory cat = kOrder[ci];
                        std::vector<const av::SceneTemplateInfo*> trows;
                        for (const auto& tpl : s_tpls) {
                            if (swk::classify_components(tpl.component_types) != cat) continue;
                            if (!tq.empty()) {
                                std::string tname = tpl.name;
                                for (auto& c : tname) c = (char)tolower((unsigned char)c);
                                if (tname.find(tq) == std::string::npos) continue;
                            }
                            trows.push_back(&tpl);
                        }
                        if (trows.empty()) continue;
                        if (ImGui::TreeNodeEx((void*)(intptr_t)(ci + 3000),
                                              ImGuiTreeNodeFlags_DefaultOpen,
                                              "%s %s  (%d)", swk::obj_category_icon(cat),
                                              swk::obj_category_label(cat), (int)trows.size())) {
                            for (const auto* tpl : trows) {
                                const bool clicked = ImGui::Selectable(
                                    (tpl->name + "##tpl" + std::to_string(ci)).c_str());
                                if (ImGui::IsItemHovered()) {
                                    std::string tip = "Add linked object from template\n";
                                    tip += "scale x" + std::to_string(tpl->scaling) + "\n";
                                    if (tpl->component_types.empty())
                                        tip += "(no components)";
                                    else {
                                        tip += "components: ";
                                        for (size_t k = 0; k < tpl->component_types.size() && k < 6; ++k) {
                                            if (k) tip += ", ";
                                            tip += tpl->component_types[k];
                                        }
                                        if (tpl->component_types.size() > 6) tip += ", …";
                                    }
                                    ImGui::SetTooltip("%s", tip.c_str());
                                }
                                if (clicked) template_add_to_scene(st, tpl->name);
                            }
                            ImGui::TreePop();
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
        }
    }

    auto draw_grid = [&](const std::vector<std::string>& items, bool pods, const char* label) {
        if (items.empty()) return;
        ImGui::TextUnformatted(label);
        const float tile = 104.0f;
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, (int)(avail_w / tile));
        int shown = 0;
        ImGui::BeginChild(label, ImVec2(0, 160.0f), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < items.size(); ++i) {
            std::string name = fs::path(items[i]).stem().string();
            std::string lname = name;
            for (auto& c : lname) c = (char)tolower((unsigned char)c);
            if (!q.empty() && lname.find(q) == std::string::npos) continue;
            if (shown % cols != 0) ImGui::SameLine();
            ImGui::PushID((int)(i + (pods ? 100000 : 0)));
            ImGui::BeginGroup();
            GLuint tex = 0;
            if (pods && i < st.obj_browser_thumbs.size()) tex = st.obj_browser_thumbs[i];
            if (tex) {
                ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(96, 96));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.24f, 1.0f));
                ImGui::Button(pods ? ICON_FA_CUBE : ICON_FA_MOUNTAIN_SUN, ImVec2(96, 96));
                ImGui::PopStyleColor();
            }
            char label_buf[64];
            snprintf(label_buf, sizeof(label_buf), "%s", name.c_str());
            ImGui::TextWrapped("%s", label_buf);
            ImGui::EndGroup();
            // Single add path: the group covers the image/button + label, so a
            // click anywhere on the tile adds exactly once.
            if (ImGui::IsItemClicked()) obj_browser_add(st, items[i], pods);
            ImGui::PopID();
            shown++;
        }
        ImGui::EndChild();
        ImGui::Spacing();
    };

    draw_grid(st.obj_browser_swdm, false, ICON_FA_MOUNTAIN_SUN " Ground Meshes (.swdm)");
    draw_grid(st.obj_browser_pods, true,  ICON_FA_CUBE " Models (.pod)");

    ImGui::End();
}

// ── X-Ray / ghost pass ────────────────────────────────────────────────────
// Re-draws every renderable object as a translucent fill plus a bright
// wireframe outline with depth writes disabled, so the whole scene reads as a
// see-through ghost overlay (classic DCC-tool x-ray mode).
static void draw_scene_xray_pass(const ViewerState& st) {
    if (!st.scene_xray) return;

    float ghost_fill[4] = {0.16f, 0.45f, 0.95f, 0.06f};
    float ghost_wire[4] = {0.45f, 0.80f, 1.00f, 0.55f};

    glDepthMask(GL_FALSE); // draw everything through everything

    for (int idx = 0; idx < (int)st.scene.objects.size(); ++idx) {
        const auto& obj = st.scene.objects[idx];
        if (obj.hidden && !st.scene_show_hidden)
            continue;
        if (obj.mesh_name.empty() && obj.background_name.empty() &&
            obj.ground_meshes.empty())
            continue; // non-renderable (spawn points, lights, triggers)

        float obj_mat[16];
        swk::object_render_matrix(obj, obj_mat);   // incl. ModelComponent Y-rotation

        // Embedded ground meshes
        if (idx < (int)st.scene_ground_gpu_meshes.size()) {
            for (const auto& gm_raw : st.scene_ground_gpu_meshes[idx]) {
                auto& gm = const_cast<av::GPUMesh&>(gm_raw);
                gm.texture_id = 0;
                av::render_mesh(gm, obj_mat, ghost_fill, false);
                av::render_mesh(gm, obj_mat, ghost_wire, true);
            }
        }

        std::string mname = obj.mesh_name.empty() ? obj.background_name : obj.mesh_name;
        if (!mname.empty())
            mname = scene_play_anim_pod(st, idx, mname);   // play-mode anim POD
        if (mname.empty()) continue;
        auto mit = st.scene_model_cache.find(mname);
        if (mit == st.scene_model_cache.end()) continue;
        const auto& model = mit->second;
        auto gmit = st.scene_gpu_mesh_cache.find(mname);
        if (gmit == st.scene_gpu_mesh_cache.end()) continue;
        const auto& gpu_meshes = gmit->second;
        float obj_frame = (idx < (int)st.scene_obj_anim_frame.size())
                              ? st.scene_obj_anim_frame[idx] : 0.0f;
        // Non-repeating KeyframeAnimation: hold the last frame (one-shot).
        if (scene_anim_holds_last(st, idx) && model.num_frames > 0)
            obj_frame = std::min(obj_frame, (float)(model.num_frames - 1));

        if (!model.nodes.empty()) {
            for (int ni = 0; ni < (int)model.nodes.size(); ++ni) {
                const auto& node = model.nodes[ni];
                if (node.object_index < 0 || node.object_index >= (int)gpu_meshes.size())
                    continue;
                float node_matrix[16], final_matrix[16];
                av::get_node_matrix(model, ni, obj_frame, node_matrix);
                if (model.has_center_point) {
                    float center_offset[16], centered_node[16];
                    av::mat4_translate(center_offset, -model.center_point[0],
                                       -model.center_point[1], -model.center_point[2]);
                    av::mat4_multiply(centered_node, center_offset, node_matrix);
                    av::mat4_multiply(final_matrix, obj_mat, centered_node);
                } else {
                    av::mat4_multiply(final_matrix, obj_mat, node_matrix);
                }
                auto& gm = const_cast<av::GPUMesh&>(gpu_meshes[node.object_index]);
                gm.texture_id = 0;
                // Skin so the ghost matches the solid pass (animated entities
                // deform; static props keep their bind pose).
                if (node.object_index < (int)model.meshes.size() &&
                    model.meshes[node.object_index].bones_per_vertex > 0) {
                    std::vector<float> sp, sn;
                    if (av::skin_mesh(model, ni, obj_frame, sp, sn)) {
                        const auto& sm = model.meshes[node.object_index];
                        av::update_mesh_vertices(gm, sp.data(),
                                                 sn.empty() ? nullptr : sn.data(),
                                                 sm.uvs.empty() ? nullptr : sm.uvs.data(),
                                                 sm.num_vertices);
                    }
                }
                av::render_mesh(gm, final_matrix, ghost_fill, false);
                av::render_mesh(gm, final_matrix, ghost_wire, true);
            }
        } else {
            for (const auto& gm_raw : gpu_meshes) {
                auto& gm = const_cast<av::GPUMesh&>(gm_raw);
                gm.texture_id = 0;
                av::render_mesh(gm, obj_mat, ghost_fill, false);
                av::render_mesh(gm, obj_mat, ghost_wire, true);
            }
        }
    }

    glDepthMask(GL_TRUE);
}

// Draw the scene's BackgroundComponent texture as a large camera-facing quad
// placed just inside the far plane, behind the level. Mirrors the reference
// editor's buildBackground(): parallax follows the background object's
// transform and the plane is clamped so its edges never show inside the frustum.
static void draw_scene_background_quad(ViewerState& st, int w, int h) {
    int bg_idx = -1;
    for (int i = 0; i < (int)st.scene.objects.size(); ++i) {
        if (!st.scene.objects[i].background_name.empty() && !st.scene.objects[i].hidden) {
            bg_idx = i;
            break;
        }
    }
    // Background layer picker: honor the user's explicit choice.
    if (st.selected_background_obj >= 0 &&
        st.selected_background_obj < (int)st.scene.objects.size() &&
        !st.scene.objects[st.selected_background_obj].background_name.empty())
        bg_idx = st.selected_background_obj;
    if (bg_idx < 0) return;
    const auto& bg = st.scene.objects[bg_idx];
    auto it = st.scene_background_textures.find(bg.background_name);
    if (it == st.scene_background_textures.end() || !it->second) return;

    // Camera basis (same spherical math as camera_get_view_matrix).
    const float pi = 3.14159265358979323846f;
    const float yaw   = st.camera.yaw   * pi / 180.0f;
    const float pitch = st.camera.pitch * pi / 180.0f;
    const float cos_p = cosf(pitch);
    float eye[3] = {
        st.camera.target[0] + st.camera.distance * cos_p * sinf(yaw),
        st.camera.target[1] + st.camera.distance * sinf(pitch),
        st.camera.target[2] + st.camera.distance * cos_p * cosf(yaw),
    };
    float fwd[3] = { st.camera.target[0]-eye[0], st.camera.target[1]-eye[1], st.camera.target[2]-eye[2] };
    float fl = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (fl < 1e-6f) fl = 1.0f;
    fwd[0] /= fl; fwd[1] /= fl; fwd[2] /= fl;
    const float world_up[3] = {0, 1, 0};
    float right[3] = {
        world_up[1]*fwd[2] - world_up[2]*fwd[1],
        world_up[2]*fwd[0] - world_up[0]*fwd[2],
        world_up[0]*fwd[1] - world_up[1]*fwd[0],
    };
    float rl = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
    if (rl < 1e-6f) rl = 1.0f;
    right[0] /= rl; right[1] /= rl; right[2] /= rl;
    float upv[3] = {
        right[1]*fwd[2] - right[2]*fwd[1],
        right[2]*fwd[0] - right[0]*fwd[2],
        right[0]*fwd[1] - right[1]*fwd[0],
    };

    // Place the plane just inside the far plane, sized to cover the frustum
    // with a 1.2x margin so parallax never reveals an edge.
    const float dist   = st.camera.far_plane * 0.9f;
    const float half_h = dist * tanf(st.camera.fov * pi / 360.0f);
    const float half_w = half_h * ((float)w / (float)std::max(1, h));
    const float sw = half_w * 1.5f, sh = half_h * 1.5f;

    // Parallax from the background object's transform (reference formula:
    // 0.5 * dist * aspect * (bgObjPos.x / u)). The quad margin is 1.5x so the
    // clamped shift (0.15x) never reveals an edge.
    const float u = fabsf(bg.pos_z) > 0.001f ? bg.pos_z : 1.0f;
    float shift_x = 0.5f * half_w * (bg.pos_x / u);
    float shift_y = 0.5f * half_h * (bg.pos_y / u);
    const float max_shift_x = half_w * 0.15f, max_shift_y = half_h * 0.15f;
    shift_x = std::clamp(shift_x, -max_shift_x, max_shift_x);
    shift_y = std::clamp(shift_y, -max_shift_y, max_shift_y);

    float cx = eye[0] + fwd[0]*dist + right[0]*shift_x + upv[0]*shift_y;
    float cy = eye[1] + fwd[1]*dist + right[1]*shift_x + upv[1]*shift_y;
    float cz = eye[2] + fwd[2]*dist + right[2]*shift_x + upv[2]*shift_y;

    // Model matrix: columns = [right*sw, upv*sh, fwd, center]
    float M[16];
    M[0]=right[0]*sw; M[4]=upv[0]*sh; M[8] =fwd[0]; M[12]=cx;
    M[1]=right[1]*sw; M[5]=upv[1]*sh; M[9] =fwd[1]; M[13]=cy;
    M[2]=right[2]*sw; M[6]=upv[2]*sh; M[10]=fwd[2]; M[14]=cz;
    M[3]=0; M[7]=0; M[11]=0; M[15]=1;

    av::render_background_quad(it->second, M);
}

// ── Play-mode animation POD resolver ──────────────────────────────────
// The game stores the *base* mesh name on scene objects ("bat",
// "grasswalker", "skelly") and swaps to per-state PODs while the AI is
// running (bat_fly.POD, grasswalker_walk.POD, skelly_walk.POD ...). During
// scene playback, resolve the animation POD for a moving monster so the
// walk/run/fly cycle plays instead of the frozen base (T-pose) model.
// Returns the mesh cache key to draw; the caller falls back to the base
// mesh when the anim POD is unavailable.
static std::string scene_play_anim_pod(const ViewerState& st, int obj_index,
                                       const std::string& base_mname) {
    if (st.scene_player.mode == sp::Mode::Off || base_mname.empty()) return base_mname;
    // Find the PlayObject for this scene object.
    const sp::PlayObject* po = nullptr;
    for (const auto& p : st.scene_player.objects) {
        if (p.index == obj_index) { po = &p; break; }
    }
    if (!po || po->kind == sp::AiKind::None) return base_mname;
    // Data-driven binding first: the scene's KeyframeAnimation component
    // names the exact POD the game plays for this object (firebat →
    // bat_fly, prisoner → npc_stand, shadowblob → snowball_land). Static
    // monsters play their bound stand POD even while idle — that's what
    // makes prisoners render instead of staying as dots.
    std::string pod;
    if (!po->anim_pod.empty()) {
        pod = po->anim_pod;
    } else if (po->moving) {
        // No binding in the scene data (library-inherited monsters): derive
        // the per-state POD the way the game names them (bat_fly, _walk).
        const char* suffix = (po->kind == sp::AiKind::Bat) ? "_fly" : "_walk";
        pod = base_mname + suffix;
    }
    if (pod.empty()) return base_mname;
    if (st.scene_model_cache.count(pod)) return pod;
    // Load it on demand (scene dir = parent of the scene file) — but only
    // if the POD actually exists, so we don't spam "NOT FOUND" for monsters
    // that have no per-state animation (shadowblob, slimes, ...).
    if (!st.scene.filepath.empty()) {
        const std::string scene_dir =
            fs::path(st.scene.filepath).parent_path().string();
        // Probe the same roots load_scene_model_to_cache() searches, so anim
        // PODs living in a global resources/ or models/ dir still resolve.
        const std::string data_dir = expand_home("~/.local/share/swordigo-desktop/assets");
        bool exists = false;
        for (const fs::path& root :
             {fs::path(scene_dir), fs::path(scene_dir).parent_path(),
              fs::path(data_dir), fs::path(data_dir) / "resources",
              fs::path(g_assets_dir) / "resources"}) {
            for (const char* ext : {"", ".POD", ".pod"}) {
                if (fs::is_regular_file(root / (pod + ext))) { exists = true; break; }
            }
            if (exists) break;
        }
        if (exists) {
            // base_mname hints the merge base for animation-only PODs
            // (npc_stand + knight, snowball_land + shadowblob, ...).
            load_scene_model_to_cache(const_cast<ViewerState&>(st), pod, scene_dir,
                                      base_mname);
            if (st.scene_model_cache.count(pod)) return pod;
        }
    }
    return base_mname;
}

static void draw_scene_visualizer(ViewerState& st) {
    ensure_scene_proxy_mesh(st);
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::AllowAxisFlip(false);
    ImGuizmo::SetGizmoSizeClipSpace(0.14f);

    // Blender-style command menus keep the full command surface available
    // without turning the viewport header into a multi-row button panel.
    // Flat toolbar styling: no resting chrome — hover gets a soft fill and only
    // selected/active tools carry a strong accent background, so the rendered
    // scene stays the strongest element (less chrome competition around it).
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th_alpha(g_theme.surface_hover, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_theme.surface_hover);
    if (ImGui::Button("View")) ImGui::OpenPopup("##scene_view");
    if (ImGui::BeginPopup("##scene_view")) {
        if (ImGui::MenuItem("Frame Selected", "F")) frame_scene_selection(st);
        if (ImGui::MenuItem("Frame All", "Home")) frame_scene_camera(st);
        if (ImGui::BeginMenu("Camera Ports")) {
            bool any = false;
            for (int i = 0; i < (int)st.scene.objects.size(); ++i) {
                if (!st.scene.objects[i].is_spawn_point) continue;
                any = true;
                if (ImGui::MenuItem(st.scene.objects[i].name.c_str()))
                    frame_scene_at_spawn(st, i);
            }
            if (!any) ImGui::TextDisabled("No spawn points in this scene");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Top"))         { st.camera.yaw = 0.0f;  st.camera.pitch = 89.0f; }  // straight down
        if (ImGui::MenuItem("Front"))       { st.camera.yaw = 180.0f; st.camera.pitch = 0.0f; }  // looking along +Z
        if (ImGui::MenuItem("Side"))        { st.camera.yaw = 90.0f; st.camera.pitch = 0.0f; }
        if (ImGui::MenuItem("Perspective")) { st.camera.yaw = 45.0f; st.camera.pitch = 30.0f; }
        ImGui::Separator();
        ImGui::MenuItem("Grid", nullptr, &st.show_grid);
        ImGui::MenuItem("Corner Axis", nullptr, &st.scene_show_axis);
        ImGui::MenuItem("Transform Gizmo", nullptr, &st.scene_show_gizmo);
        ImGui::MenuItem("Show Hidden", nullptr, &st.scene_show_hidden);
        ImGui::Separator();
        ImGui::MenuItem("Textured", "T", &st.show_textured);
        ImGui::MenuItem("Wireframe", "W", &st.show_wireframe);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add")) ImGui::OpenPopup("##scene_add");
    if (ImGui::BeginPopup("##scene_add")) {
        if (ImGui::MenuItem(ICON_FA_PLUS " Empty Object")) {
            snapshot_scene(st);
            select_scene_object(st, static_cast<int>(av::scene_create_object(st.scene)));
            st.scene_dirty = true;
        }
        if (ImGui::MenuItem(ICON_FA_CUBE " Add Object...")) st.obj_browser_open = true;
        if (ImGui::MenuItem(ICON_FA_MOUNTAIN_SUN " Ground Mesh...")) open_ground_mesh_studio(st);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Object")) ImGui::OpenPopup("##scene_object");
    if (ImGui::BeginPopup("##scene_object")) {
        const bool has_selection = !st.scene_selection.empty();
        ImGui::BeginDisabled(!has_selection);
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copy_scene_selection(st);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_scene_selection(st);
        if (ImGui::MenuItem("Delete", "Delete")) delete_scene_selection(st);
        ImGui::Separator();
        if (ImGui::MenuItem("Move Earlier", "Page Up")) move_scene_object(st, -1);
        if (ImGui::MenuItem("Move Later", "Page Down")) move_scene_object(st, 1);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!st.scene_has_object_clipboard);
        if (ImGui::MenuItem("Paste", "Ctrl+V")) paste_scene_selection(st);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(st.scene_anim_playing ? ICON_FA_PLAY " Anim" : ICON_FA_PAUSE " Anim")) {
        st.scene_anim_playing = !st.scene_anim_playing;
        if (!st.scene_anim_playing)
            std::fill(st.scene_obj_anim_frame.begin(), st.scene_obj_anim_frame.end(), 0.0f);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(st.scene_anim_playing
            ? "Scene animation ON — every model plays its POD animation.\nClick to freeze at bind pose."
            : "Scene animation OFF — models frozen at bind pose (T-shape).\nClick to play all animations.");
    ImGui::SameLine();
    if (ImGui::Button("Anim ▾")) ImGui::OpenPopup("##scene_anim");
    if (ImGui::BeginPopup("##scene_anim")) {
        ImGui::MenuItem("Play scene animations", nullptr, &st.scene_anim_playing);
        ImGui::SliderFloat("Speed", &st.scene_anim_speed, 0.05f, 4.0f, "%.2fx");
        if (ImGui::MenuItem("Reset all frames")) {
            std::fill(st.scene_obj_anim_frame.begin(), st.scene_obj_anim_frame.end(), 0.0f);
            st.scene_anim_playing = true;
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");

    const char* transform_modes[] = {"Select", "Move", "Rotate", "Scale"};
    const char* transform_shortcuts[] = {
        "Select / Navigate (1)", "Move (2)", "Rotate (3)", "Scale (4)"
    };
    // Transform tools are owned by the 2D editor while it is active.
    ImGui::BeginDisabled(st.gm_inline_edit);
    for (int mode = 0; mode < 4; ++mode) {
        ImGui::SameLine();
        const bool active = (st.scene_transform_mode == mode) && !st.scene_mesh_edit;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, th_mix(g_theme.surface_active, g_theme.accent, 0.40f));
        if (ImGui::Button(transform_modes[mode])) {
            st.scene_transform_mode = mode;
            st.scene_mesh_edit = false;
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", transform_shortcuts[mode]);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const bool mesh_active = st.scene_mesh_edit || st.gm_inline_edit;
    if (mesh_active) ImGui::PushStyleColor(ImGuiCol_Button, g_theme.warning);
    if (ImGui::Button(st.gm_inline_edit ? "Finish 2D Edit" : "Mesh Edit")) {
        if (st.gm_inline_edit) {
            gm_end_inline_edit(st);          // finish the inline 2D session
        } else {
            // Inline 2D polygon editing right inside the visualizer — the ONLY
            // mesh editor. Objects without a ground polygon get a status
            // message; there is no legacy-3D fallback anymore.
            const bool has_sel = st.selected_object >= 0 &&
                                 st.selected_object < (int)st.scene.objects.size();
            if (has_sel) gm_begin_inline_edit(st);
        }
    }
    if (mesh_active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(st.gm_inline_edit
            ? "Finish the 2D edit session (Esc) — changes are applied to the mesh."
            : "Edit the selected ground mesh in pure 2D (M) — top-down view, drag vertices, "
              "right-click an edge to add a node, MMB pan, wheel zoom");
    if (st.scene_mesh_edit) {
        ImGui::SameLine();
        if (ImGui::Button("Mode")) ImGui::OpenPopup("##mesh_mode");
        if (ImGui::BeginPopup("##mesh_mode")) {
            const char* tool_labels[] = {"Vertex", "Face", "Edge"};
            for (int tool = 0; tool < 3; ++tool) {
                if (ImGui::MenuItem(tool_labels[tool], nullptr, st.mesh_edit_tool == tool)) {
                    st.mesh_edit_tool = tool;
                    st.mesh_edit_vertex = -1;
                    st.mesh_edit_triangle = -1;
                    st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
                }
            }
            if (ImGui::MenuItem("Insert", "I")) mesh_edit_insert_vertex(st);
            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(st.scene_snap ? "Snap *" : "Snap")) ImGui::OpenPopup("##snap_options");
    if (ImGui::BeginPopup("##snap_options")) {
        ImGui::MenuItem("Enabled", nullptr, &st.scene_snap);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Step", &st.scene_snap_step, 0.1f, 0.01f, 100.0f, "%.2f");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Gizmo")) ImGui::OpenPopup("##gizmo_options");
    if (ImGui::BeginPopup("##gizmo_options")) {
        ImGui::MenuItem("Transform gizmo", nullptr, &st.scene_show_gizmo);
        ImGui::MenuItem("Corner axis", nullptr, &st.scene_show_axis);
        ImGui::EndPopup();
    }

    // ── Mode → Scene Player ───────────────────────────────────────────
    ImGui::SameLine();
    const bool player_active = st.scene_player.mode != sp::Mode::Off;
    if (player_active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.65f, 1.0f));
    if (ImGui::Button("Mode")) ImGui::OpenPopup("##scene_player_mode");
    if (player_active) ImGui::PopStyleColor();
    if (ImGui::BeginPopup("##scene_player_mode")) {
        if (ImGui::BeginMenu("Scene Player")) {
            const bool vis_active = st.scene_player.mode == sp::Mode::Visualise;
            const bool hiro_active = st.scene_player.mode == sp::Mode::PlayHiro;
            if (ImGui::MenuItem("Visualise Scene Playing", nullptr, vis_active)) {
                st.scene_loading = true;
                st.scene_loading_frames = 3.0f;
                st.scene_loading_msg = st.scene.filename + " — visualising";
                sp::player_end(st.scene_player);
                sp::player_begin(st.scene_player, st.scene, sp::Mode::Visualise,
                                 st.scene.filepath.empty()
                                     ? std::string()
                                     : fs::path(st.scene.filepath).parent_path().string());
                st.scene_player_window_open = true;
                st.scene_anim_playing = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Play the scene's animations + AI simulation (no hero).");
            if (ImGui::MenuItem("Spawn Hiro and Play", nullptr, hiro_active)) {
                st.scene_loading = true;
                st.scene_loading_frames = 3.0f;
                st.scene_loading_msg = st.scene.filename + " — spawning Hiro";
                sp::player_end(st.scene_player);
                sp::player_begin(st.scene_player, st.scene, sp::Mode::PlayHiro,
                                 st.scene.filepath.empty()
                                     ? std::string()
                                     : fs::path(st.scene.filepath).parent_path().string());
                st.scene_player_window_open = true;
                st.scene_anim_playing = true;
                switch_scene_tab(st, 1);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Spawn Hiro at the scene spawn point. A/D move, Space jump, camera follows.");
            ImGui::Separator();
            if (ImGui::MenuItem("Stop Playback")) {
                sp::player_end(st.scene_player);
                st.scene_player_window_open = false;
                st.status_msg = "Scene player stopped.";
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Frame")) frame_scene_selection(st);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame selection or scene (F)");

    ImGui::SameLine();
    if (ImGui::Button(st.scene_xray ? "X-Ray: On" : "X-Ray: Off"))
        st.scene_xray = !st.scene_xray;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle X-Ray / ghost see-through view");

    ImGui::SameLine();
    if (ImGui::Button(st.postfx_enabled ? ICON_FA_WAND_MAGIC_SPARKLES " PostFX: On" : ICON_FA_WAND_MAGIC_SPARKLES " PostFX: Off"))
        st.postfx_enabled = !st.postfx_enabled;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle PostFX (bloom / DOF / HD grade / vignette)");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Logical viewport size drives all UI/picking/overlay math.
    int w = (int)avail.x;
    int h = (int)avail.y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    // High-DPI render scale: render the FBO at a multiple of the logical
    // viewport size (1.0 / 1.5 / 2.0) for a crisp image on HiDPI displays.
    const float rs = st.render_scale > 0.0f ? st.render_scale : 1.0f;
    const int rw = (int)(avail.x * rs);
    const int rh = (int)(avail.y * rs);
    const int fw = rw < 1 ? 1 : rw;
    const int fh = rh < 1 ? 1 : rh;

    if (!st.fbo) {
        st.fbo = av::create_fbo(fw, fh, &st.fbo_tex);
        st.fbo_w = fw; st.fbo_h = fh;
    } else if (fw != st.fbo_w || fh != st.fbo_h) {
        av::resize_fbo(st.fbo, fw, fh, &st.fbo_tex);
        st.fbo_w = fw; st.fbo_h = fh;
    }

    // Scene Player: game-style camera follow (Hiro mode). Applied BEFORE the
    // 3D pass so both the scene and the Hiro model render through the follow
    // camera (previously applied after end_3d → stale view for one frame and
    // no effect on the pass that draws the level).
    if (st.scene_player.mode == sp::Mode::PlayHiro)
        sp::player_apply_camera(st.scene_player, st.camera);

    av::begin_3d(st.fbo, fw, fh, st.camera);

    // Viewport diagnostics (Settings → Rendering): flat texture view and the
    // world-normal color view. begin_3d resets these to OFF for every pass, so
    // this opts the scene visualizer in — POD previews and thumbnails stay
    // clean. Every mesh here (POD, ground mesh, hiro) shares the diagnostic.
    av::set_view_flags(!st.scene_lighting_enabled, st.scene_normals_debug);

    // Scene background texture (camera-following quad) drawn first, behind the
    // level. The reference editor renders it with depthWrite disabled; our
    // render_background_quad() matches that, so the grid + level draw on top.
    draw_scene_background_quad(st, w, h);

    if (st.show_grid) {
        av::render_grid_xy(st.grid_size * 5.0f, st.scene.bounds_min[2]);
    }

    float white[4] = {1, 1, 1, 1};
    float highlight[4] = {0.4f, 0.7f, 1.0f, 1.0f};
    int rendered_objects = 0;
    int proxy_objects = 0;

    // Objects whose components are purely non-visual (lights, portals,
    // collision shapes, spawn points, AI/controllers, emitters) should NOT get
    // an orange proxy dot — the template resolved fine, there's just nothing
    // to draw. Only objects that reference a mesh but failed to resolve get a
    // marker (so real missing models stay visible for debugging).

    // Preserve the user's preview-light controls: scene ambient/directional
    // lights apply only during this scene pass.
    float saved_light_dir[3], saved_light_color[3], saved_ambient_color[3];
    float saved_ambient_ground[3];
    std::memcpy(saved_light_dir, av::g_light_dir, sizeof(saved_light_dir));
    std::memcpy(saved_light_color, av::g_light_color, sizeof(saved_light_color));
    std::memcpy(saved_ambient_color, av::g_ambient_color, sizeof(saved_ambient_color));
    std::memcpy(saved_ambient_ground, av::g_ambient_ground_color, sizeof(saved_ambient_ground));

    // Apply Swordigo light types faithfully: ambient and directional lights
    // feed their shader channels, point lights use local falloff, and overlay
    // lights are not misclassified as point lights.
    if (st.scene_lights_enabled) {
        float lpos[16][3] = {{0}};
        float lcol[16][3] = {{0}};
        float lrad[16] = {0};
        int point_count = 0;
        float ambient_sum[3] = {0.0f, 0.0f, 0.0f};
        int ambient_count = 0;
        // Accumulate up to 4 directional lights (multi-directional support).
        float ddir[4][3] = {{0}};
        float dcol[4][3] = {{0}};
        int dir_count = 0;
        const float t = (float)ImGui::GetTime();
        struct RankedPoint {
            const av::SceneData::SceneLight* light;
            float intensity;
            float score;
        };
        std::vector<RankedPoint> ranked_points;
        ranked_points.reserve(st.scene.lights.size());
        auto hash01 = [](float value) {
            const float sample = std::sin(value * 12.9898f) * 43758.5453f;
            return sample - std::floor(sample);
        };
        auto smooth_noise = [&](float value, float seed) {
            const float cell = std::floor(value);
            const float fraction = value - cell;
            const float smooth = fraction * fraction * (3.0f - 2.0f * fraction);
            const float a = hash01(cell + seed);
            const float b = hash01(cell + 1.0f + seed);
            return a + (b - a) * smooth;
        };
        for (const auto& light : st.scene.lights) {
            if (light.type == 1) {
                for (int axis = 0; axis < 3; ++axis)
                    ambient_sum[axis] += light.color[axis] * light.intensity;
                ++ambient_count;
            } else if (light.type == 2 && dir_count < 4) {
                float length = std::sqrt(light.pos[0] * light.pos[0] +
                                         light.pos[1] * light.pos[1] +
                                         light.pos[2] * light.pos[2]);
                float direction[3] = {light.pos[0], light.pos[1], light.pos[2]};
                if (length < 1e-6f) {
                    direction[2] = 1.0f;
                    length = 1.0f;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    ddir[dir_count][axis] = direction[axis] / length;
                    dcol[dir_count][axis] = light.color[axis] * light.intensity;
                }
                ++dir_count;
            } else if (light.type == 3) {
                float intensity = light.base_intensity;
                if (light.flicker) {
                    const float seed = light.pos[0] * 0.071f + light.pos[1] * 0.113f +
                                       light.pos[2] * 0.173f;
                    const float slow = smooth_noise(t * light.flicker_speed * 0.62f, seed);
                    const float fast = smooth_noise(t * light.flicker_speed * 1.91f, seed + 19.7f);
                    const float flame = std::clamp(slow * 0.72f + fast * 0.28f, 0.0f, 1.0f);
                    intensity *= 1.0f + light.flicker_amount * (flame * 2.0f - 1.0f);
                }
                const float dx = light.pos[0] - st.camera.target[0];
                const float dy = light.pos[1] - st.camera.target[1];
                const float dz = light.pos[2] - st.camera.target[2];
                const float radius = std::max(1.0f, light.radius);
                const float normalized_distance = std::sqrt(dx * dx + dy * dy + dz * dz) / radius;
                const float luminance = light.color[0] * 0.2126f + light.color[1] * 0.7152f +
                                        light.color[2] * 0.0722f;
                const float influence = 1.0f / (1.0f + normalized_distance * normalized_distance);
                ranked_points.push_back({&light, intensity, luminance * intensity * influence});
            }
        }
        if (ambient_count > 0) {
            for (int axis = 0; axis < 3; ++axis) {
                // Additive ambient is order-independent; the shoulder prevents
                // stacked components from bleaching the scene.
                const float value = std::max(0.0f, ambient_sum[axis]);
                av::g_ambient_color[axis] = value / (1.0f + value * 0.28f);
                av::g_ambient_ground_color[axis] = av::g_ambient_color[axis] * 0.42f;
            }
        }
        std::stable_sort(ranked_points.begin(), ranked_points.end(),
                         [](const RankedPoint& a, const RankedPoint& b) {
                             return a.score > b.score;
                         });
        for (const RankedPoint& ranked : ranked_points) {
            if (point_count >= 16) break;
            const auto& light = *ranked.light;
            for (int axis = 0; axis < 3; ++axis) {
                lpos[point_count][axis] = light.pos[axis];
                lcol[point_count][axis] = std::min(light.color[axis] * ranked.intensity, 2.5f);
            }
            lrad[point_count] = std::max(1.0f, light.radius);
            ++point_count;
        }
        // When directional lights exist, upload them so the renderer uses the
        // full array (multi-directional). Otherwise keep the editor preview.
        av::set_directional_lights(ddir, dcol, dir_count);
        av::set_point_lights(lpos, lcol, lrad, point_count);
    } else {
        av::clear_point_lights();
        av::clear_directional_lights();
    }

    // Vanilla atmospheric depth: distant level geometry darkens toward the
    // background. Fog range follows the camera distance so it stays consistent
    // when zooming in/out. (Toggle in Settings -> Rendering & Visuals.)
    if (st.scene_depth_fog_enabled) {
        const float f_near = st.camera.distance * 2.2f;
        const float f_far  = st.camera.distance * 7.5f;
        const float fog_col[3] = {0.05f, 0.055f, 0.085f};
        av::set_depth_fog(true, fog_col, f_near, f_far);
    } else {
        av::set_depth_fog(false, nullptr, 0.0f, 0.0f);
    }

    // ── Overlay darkness veil ──
    // Overlay (type 4) lights are a scene-wide darkness seed: unlit geometry
    // reads darker while point-light falloff (and their bloom cores) punch
    // through — vanilla cave/temple lighting. We darken the ambient toward the
    // overlay tint; point lights were uploaded above so they re-brighten.
    if (st.scene_overlay_enabled && !st.scene.overlays.empty()) {
        float overlay[3] = {0.0f, 0.0f, 0.0f};
        float max_inten = 0.0f;
        for (const auto& ov : st.scene.overlays) {
            for (int axis = 0; axis < 3; ++axis)
                overlay[axis] += ov.color[axis] * ov.intensity;
            max_inten = std::max(max_inten, ov.intensity);
        }
        // Blend the ambient channel toward the overlay darkness. The additive
        // point lights uploaded above sit on top, keeping torches readable.
        float veil = std::min(1.0f, max_inten);
        for (int axis = 0; axis < 3; ++axis) {
            float dark = overlay[axis] * veil;
            // Soft veil: darkens the ambient toward the overlay tint but keeps
            // a readable floor — vanilla darkness, never crushed black.
            av::g_ambient_color[axis] = std::max(0.0f,
                av::g_ambient_color[axis] * (1.0f - veil * 0.65f) + dark * 0.30f);
            av::g_ambient_ground_color[axis] = std::max(0.0f,
                av::g_ambient_ground_color[axis] * (1.0f - veil * 0.65f) + dark * 0.30f);
        }
    }

    for (int idx = 0; idx < (int)st.scene.objects.size(); ++idx) {
        const auto& obj = st.scene.objects[idx];
        if (obj.hidden && !st.scene_show_hidden)
            continue;
        bool rendered = false;

        float obj_mat[16];
        swk::object_render_matrix(obj, obj_mat);   // incl. ModelComponent Y-rotation

        // Draw embedded ground meshes (if any)
        if (idx < (int)st.scene_ground_gpu_meshes.size()) {
            const auto& gm_vec = st.scene_ground_gpu_meshes[idx];
            const auto& tex_vec = st.scene_ground_textures[idx];
            for (size_t gi = 0; gi < gm_vec.size(); ++gi) {
                auto& gm = const_cast<av::GPUMesh&>(gm_vec[gi]);
                if (st.show_textured && gi < tex_vec.size()) {
                    gm.texture_id = tex_vec[gi];
                } else {
                    gm.texture_id = 0;
                }

                float* col = (idx == st.selected_object) ? highlight : white;
                av::render_mesh(gm, obj_mat, col, false);
                rendered = true;

                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, obj_mat, wire_col, true);
                }
            }
        }

        // Backgrounds are camera-following textured quads (drawn by
        // draw_scene_background_quad), never POD models — exclude them here.
        const bool bg_only_obj = obj.mesh_name.empty() && !obj.background_name.empty();
        // During scene playback, moving AI entities swap to their animation
        // POD (bat_fly, grasswalker_walk, ...) so the run/fly cycles play.
        std::string mname = scene_play_anim_pod(st, idx, obj.mesh_name);
        if (!mname.empty() && st.scene_model_cache.count(mname)) {
            const auto& model = st.scene_model_cache[mname];
            const auto& gpu_meshes = st.scene_gpu_mesh_cache[mname];
            const auto& textures = st.scene_texture_cache[mname];

            if (!model.nodes.empty()) {
                for (int ni = 0; ni < (int)model.nodes.size(); ni++) {
                const auto& node = model.nodes[ni];
                if (node.object_index < 0 || node.object_index >= (int)gpu_meshes.size()) continue;

                auto& gm = const_cast<av::GPUMesh&>(gpu_meshes[node.object_index]);
                if (st.show_textured) {
                    int mat_idx = node.material_index;
                    int tex_idx = -1;
                    if (mat_idx >= 0 && mat_idx < (int)model.materials.size()) {
                        tex_idx = model.materials[mat_idx].diffuse_texture_index;
                    }
                    if (tex_idx >= 0 && tex_idx < (int)textures.size()) {
                        gm.texture_id = textures[tex_idx];
                    } else if (!textures.empty()) {
                        gm.texture_id = textures[0];
                    } else {
                        gm.texture_id = 0;
                    }
                } else {
                    gm.texture_id = 0;
                }

                float node_matrix[16], final_matrix[16];
                float obj_frame = (idx < (int)st.scene_obj_anim_frame.size())
                                      ? st.scene_obj_anim_frame[idx] : 0.0f;
                if (scene_anim_holds_last(st, idx) && model.num_frames > 0)
                    obj_frame = std::min(obj_frame, (float)(model.num_frames - 1));
                av::get_node_matrix(model, ni, obj_frame, node_matrix);
                float centered_node[16];
                if (model.has_center_point) {
                    float center_offset[16];
                    av::mat4_translate(center_offset, -model.center_point[0],
                                       -model.center_point[1], -model.center_point[2]);
                    av::mat4_multiply(centered_node, center_offset, node_matrix);
                } else {
                    std::memcpy(centered_node, node_matrix, sizeof(centered_node));
                }
                av::mat4_multiply(final_matrix, obj_mat, centered_node);

                if (node.object_index < static_cast<int>(model.meshes.size()) &&
                    model.meshes[node.object_index].bones_per_vertex > 0) {
                    std::vector<float> skinned_positions, skinned_normals;
                    if (av::skin_mesh(model, ni, obj_frame, skinned_positions, skinned_normals)) {
                        const auto& source_mesh = model.meshes[node.object_index];
                        av::update_mesh_vertices(gm, skinned_positions.data(),
                                                 skinned_normals.empty() ? nullptr : skinned_normals.data(),
                                                 source_mesh.uvs.empty() ? nullptr : source_mesh.uvs.data(),
                                                 source_mesh.num_vertices);
                    }
                }

                float mat_color[4] = {1, 1, 1, 1};
                if (node.material_index >= 0 && node.material_index < (int)model.materials.size()) {
                    const auto& m = model.materials[node.material_index];
                    mat_color[0] = m.diffuse[0]; mat_color[1] = m.diffuse[1]; mat_color[2] = m.diffuse[2];
                    mat_color[3] = m.opacity;
                }
                float* col = (idx == st.selected_object) ? highlight : mat_color;
                av::render_mesh(gm, final_matrix, col, false);
                rendered = true;

                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, final_matrix, wire_col, true);
                }
                }
            } else {
                for (int mi = 0; mi < (int)gpu_meshes.size(); mi++) {
                auto& gm = const_cast<av::GPUMesh&>(gpu_meshes[mi]);
                if (st.show_textured && !textures.empty()) {
                    gm.texture_id = textures[0];
                } else {
                    gm.texture_id = 0;
                }

                float* col = (idx == st.selected_object) ? highlight : white;
                av::render_mesh(gm, obj_mat, col, false);
                rendered = true;

                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, obj_mat, wire_col, true);
                }
                }
            }
        }

        // Background objects resolve to the scene backdrop quad (rendered
        // before the level); treat them as rendered so no proxy dot is drawn.
        if (!rendered && bg_only_obj && st.scene_show_hidden)
            rendered = true;

        if (!rendered && st.scene_proxy_mesh.vao) {
            if (obj.is_non_visual()) {
                // Purely non-visual (light / portal / collider / spawn point).
                // Draw a tiny, dim neutral marker so they stay findable, but
                // don't flag them as "missing model" dots.
                float marker_scale = std::max(0.035f, st.camera.distance * 0.0030f);
                float marker_s[16], marker_t[16], marker_matrix[16];
                av::mat4_identity(marker_s);
                marker_s[0] = marker_s[5] = marker_s[10] = marker_scale;
                av::mat4_translate(marker_t, obj.pos_x, obj.pos_y, obj.pos_z);
                av::mat4_multiply(marker_matrix, marker_t, marker_s);
                float proxy_color[4] = {0.45f, 0.55f, 0.70f, obj.hidden ? 0.12f : 0.30f};
                if (is_scene_selected(st, idx)) {
                    proxy_color[0] = 0.25f; proxy_color[1] = 0.72f; proxy_color[2] = 1.0f;
                    proxy_color[3] = 0.95f;
                }
                av::render_mesh(st.scene_proxy_mesh, marker_matrix, proxy_color, false);
                rendered = true;
            } else {
                // References a mesh but it didn't load — flag as a missing model.
                float marker_scale = std::max(0.05f, st.camera.distance * 0.0048f);
                float marker_s[16], marker_t[16], marker_matrix[16];
                av::mat4_identity(marker_s);
                marker_s[0] = marker_s[5] = marker_s[10] = marker_scale;
                av::mat4_translate(marker_t, obj.pos_x, obj.pos_y, obj.pos_z);
                av::mat4_multiply(marker_matrix, marker_t, marker_s);
                float proxy_color[4] = {0.95f, 0.48f, 0.16f, obj.hidden ? 0.18f : 0.50f};
                if (is_scene_selected(st, idx)) {
                    proxy_color[0] = 0.25f; proxy_color[1] = 0.72f; proxy_color[2] = 1.0f;
                    proxy_color[3] = 0.95f;
                }
                av::render_mesh(st.scene_proxy_mesh, marker_matrix, proxy_color, false);
                rendered = true;
                ++proxy_objects;
            }
        }
        if (rendered)
        ++rendered_objects;
    }

    // ── X-Ray ghost pass (translucent fill + wireframe outlines) ──
    draw_scene_xray_pass(st);

    // Render diagnostics (RUBY_DEBUG_VIS=1): verify model objects draw, not just proxies.
    static int s_vis_diag_frames = 0;
    static const bool s_vis_diag = ([]{ const char* v = getenv("RUBY_DEBUG_VIS");
                                       return v && v[0] == '1'; })();
    if (s_vis_diag && (s_vis_diag_frames++ % 120) == 0)
        fprintf(stderr, "[RubyDebug] visualizer: %d/%d objects rendered, %d proxy markers\n",
                rendered_objects, (int)st.scene.objects.size(), proxy_objects);
    st.scene_rendered_objects = rendered_objects;
    st.scene_proxy_objects = proxy_objects;

    // ── Ground-mesh editing overlay: strong wireframe on the edited object ──
    if (st.scene_mesh_edit && st.mesh_edit_object >= 0 &&
        st.mesh_edit_object < (int)st.scene.objects.size()) {
        const auto& eobj = st.scene.objects[st.mesh_edit_object];
        if (!eobj.ground_meshes.empty() && st.mesh_edit_object < (int)st.scene_ground_gpu_meshes.size()) {
            float eobj_mat[16];
            swk::object_world_matrix(eobj, eobj_mat);
            float wire_col[4] = {0.25f, 0.90f, 1.00f, 0.9f};
            float edge_col[4] = {0.10f, 0.55f, 0.75f, 0.55f};
            const auto& gm_vec = st.scene_ground_gpu_meshes[st.mesh_edit_object];
            for (size_t gi = 0; gi < gm_vec.size(); ++gi) {
                av::render_mesh(gm_vec[gi], eobj_mat, edge_col, false);   // translucent fill
                av::render_mesh(gm_vec[gi], eobj_mat, wire_col, true);    // wire overlay
            }
        }
    }

    // ── ShadowComponent contact blobs ──
    // Soft ground-aligned ellipses that darken the surface under an object.
    // Drawn after opaque geometry (multiplicative) so they sit on ground/water.
    if (st.scene_shadows_enabled) {
        const float sh_col[3] = {0.0f, 0.0f, 0.0f};
        for (const auto& sd : st.scene.shadows) {
            if (sd.object_index < 0 || sd.object_index >= (int)st.scene.objects.size())
                continue;
            const auto& sobj = st.scene.objects[sd.object_index];
            if (sobj.hidden && !st.scene_show_hidden)
                continue;
            av::render_shadow_blob(sd.pos, sd.width_radius, sd.depth_radius,
                                   sd.rot_y, sh_col);
        }
    }

    // ── Fluid sheets (WaterMesh: water / lava pools) ──
    // Animated semi-transparent sheets from the parsed water rectangles.
    // Drawn after all opaque geometry so alpha blending reads correctly.
    if (st.scene_water_enabled) {
        // Re-derive water textures when the parsed sheet list changed size
        // (object add/delete re-runs parse_scene_waters via scene_refresh).
        if (st.scene_water_textures.size() != st.scene.waters.size())
            upload_scene_waters(st, fs::path(st.scene.filepath).parent_path().string());
        const float wtime = (float)ImGui::GetTime();
        for (size_t wi = 0; wi < st.scene.waters.size() && wi < st.scene_water_textures.size(); ++wi) {
            const auto& w = st.scene.waters[wi];
            if (w.object_index < 0 || w.object_index >= (int)st.scene.objects.size())
                continue;
            const auto& wobj = st.scene.objects[w.object_index];
            if (wobj.hidden && !st.scene_show_hidden)
                continue;
            float wmat[16];
            swk::object_world_matrix(wobj, wmat);
            av::WaterSheetData ws;
            memcpy(ws.rect, w.rect, sizeof(ws.rect));
            memcpy(ws.front_color, w.front_color, sizeof(ws.front_color));
            memcpy(ws.surface_color, w.surface_color, sizeof(ws.surface_color));
            ws.tile_size = w.tile_size;
            ws.tex_offset[0] = w.tex_offset[0];
            ws.tex_offset[1] = w.tex_offset[1];
            ws.texture_id = st.scene_water_textures[wi];
            av::render_water_sheet(ws, wmat, wtime);
        }
    }

    // ── Emissive glow sprites (torches / fire / SimpleGlow) ──
    // Additive camera-facing billboards at every point light; their bright
    // cores feed the PostFX bloom bright-pass for the vanilla torch glow.
    if (st.scene_lights_enabled && st.scene_glow_enabled)
        av::render_point_light_glows();

    // ── Light debug overlay: influence-radius rings + emitter crosses ──
    if (st.scene_lights_enabled && st.scene_light_debug)
        av::render_light_debug();

    // ── Hiro model (Spawn Hiro and Play) ──
    if (st.scene_player.mode == sp::Mode::PlayHiro && st.scene_player.hiro.active) {
        const std::string anim_model = st.scene_player.hiro.anim;
        const std::string hero_dir =
            st.scene.filepath.empty() ? std::string()
                                      : fs::path(st.scene.filepath).parent_path().string();
        // Resolve the current animation model (hiro_run etc.) into the cache.
        if (!st.scene_model_cache.count(anim_model) && !hero_dir.empty())
            load_scene_model_to_cache(st, anim_model, hero_dir);
        auto hit = st.scene_model_cache.find(anim_model);
        // Fall back to hiro_stand if the current animation model is missing.
        if (hit == st.scene_model_cache.end() && !st.scene_model_cache.count("hiro_stand")
            && !hero_dir.empty())
            load_scene_model_to_cache(st, "hiro_stand", hero_dir);
        if (hit == st.scene_model_cache.end())
            hit = st.scene_model_cache.find("hiro_stand");
        if (hit != st.scene_model_cache.end()) {
            const auto& gpu_meshes = st.scene_gpu_mesh_cache[hit->first];
            const auto& textures  = st.scene_texture_cache[hit->first];
            constexpr float kHiroPi = 3.14159265358979323846f;
            float M[16], R[16], MR[16];
            // Physics treats pos[1] as the FEET (ground glue / collision). The
            // POD's visual feet sit feet_offset BELOW the model origin — the
            // rest-pose node transforms + center-point shift (e.g. hiro
            // ≈ 35.1, matching the hitbox bottom −34). Jump/spinjump poses
            // drop the feet several units below rest, so prefer the CURRENT
            // animation frame's own feet depth (hiro_pick_animation
            // precomputed the table) and only fall back to the rest-pose
            // measure. This keeps the feet glued to the floor in every pose.
            float feet_off = st.scene_player.hiro.feet_offset;
            const auto& hf = st.scene_player.hiro.anim_feet;
            const float hframe = st.scene_player.hiro.frame;
            if (!hf.empty()) {
                const float fr = std::clamp(hframe, 0.0f,
                                            (float)(hf.size() - 1));
                const int i0 = (int)fr;
                const int i1 = std::min(i0 + 1, (int)hf.size() - 1);
                const float t = fr - (float)i0;
                feet_off = hf[i0] * (1.0f - t) + hf[i1] * t;
            } else {
                float fo = av::pod_feet_offset(hit->second);
                if (fo > 0.0f) feet_off = fo;
            }
            const float render_y = st.scene_player.hiro.pos[1] + feet_off;
            av::mat4_translate(M, st.scene_player.hiro.pos[0], render_y,
                               st.scene_player.hiro.pos[2]);
            // The hiro PODs are authored facing the camera (+Z). The game
            // shows Hiro in profile facing his travel direction (±X), so the
            // model needs a +90° Y pre-rotation before the facing flip.
            const float yaw_deg = (st.scene_player.hiro.rot + kHiroPi * 0.5f) * 180.0f / kHiroPi;
            av::mat4_rotate_y(R, yaw_deg);
            av::mat4_multiply(MR, M, R);

            // Contact shadow under Hiro (report: the character floats above
            // the floor with no grounding). A soft ground-aligned ellipse at
            // the FEET (physics pos[1], not the render origin), sized from the
            // real scl hitbox width. Airborne distance (abs vel) shrinks it
            // so jumps read as leaving the ground — full size at rest, fading
            // as he climbs away and regrowing on the way down.
            if (st.scene_shadows_enabled) {
                const float hw = st.scene_player.hero_stats.hitbox[2];
                const float air = std::clamp(std::fabsf(st.scene_player.hiro.vel_y) * 0.003f,
                                             0.0f, 0.4f);
                const float sh_radius = std::max(hw * 1.3f, 12.0f) * (1.0f - air);
                const float sh_pos[3] = { st.scene_player.hiro.pos[0],
                                          st.scene_player.hiro.pos[1] + 1.5f,
                                          st.scene_player.hiro.pos[2] };
                const float sh_col[3] = { 0.0f, 0.0f, 0.0f };
                av::render_shadow_blob(sh_pos, sh_radius, sh_radius * 0.62f, 0.0f, sh_col);
            }

            const float hiro_frame = st.scene_player.hiro.frame;
            for (int ni = 0; ni < (int)hit->second.nodes.size(); ++ni) {
                const auto& node = hit->second.nodes[ni];
                if (node.object_index < 0 || node.object_index >= (int)gpu_meshes.size()) continue;
                float node_m[16], centered_node[16], final_m[16];
                av::get_node_matrix(hit->second, ni, hiro_frame, node_m);
                // Centre-point correction, mirroring the scene solid pass:
                // PODs authored around a world-space CenterPoint node must be
                // shifted back so the model sits on its physics position.
                if (hit->second.has_center_point) {
                    float center_offset[16];
                    av::mat4_translate(center_offset, -hit->second.center_point[0],
                                       -hit->second.center_point[1],
                                       -hit->second.center_point[2]);
                    av::mat4_multiply(centered_node, center_offset, node_m);
                } else {
                    std::memcpy(centered_node, node_m, sizeof(centered_node));
                }
                av::mat4_multiply(final_m, MR, centered_node);
                auto& gm = const_cast<av::GPUMesh&>(gpu_meshes[node.object_index]);
                // Textures: same per-node material mapping as the scene solid
                // pass, so Hiro isn't a flat white figure (model viewer uses
                // the identical table).
                if (st.show_textured) {
                    int mat_idx = node.material_index;
                    int tex_idx = -1;
                    if (mat_idx >= 0 && mat_idx < (int)hit->second.materials.size())
                        tex_idx = hit->second.materials[mat_idx].diffuse_texture_index;
                    if (tex_idx >= 0 && tex_idx < (int)textures.size())
                        gm.texture_id = textures[tex_idx];
                    else if (!textures.empty())
                        gm.texture_id = textures[0];
                    else
                        gm.texture_id = 0;
                } else {
                    gm.texture_id = 0;
                }
                // CPU-skin the hero so animation frames deform the mesh
                // (hiro.POD is exported in T-pose; the hiro_* anim streams
                // must be applied, exactly like the scene textured pass).
                if (node.object_index < (int)hit->second.meshes.size() &&
                    hit->second.meshes[node.object_index].bones_per_vertex > 0) {
                    std::vector<float> sp, sn;
                    if (av::skin_mesh(hit->second, ni, hiro_frame, sp, sn)) {
                        const auto& sm = hit->second.meshes[node.object_index];
                        av::update_mesh_vertices(gm, sp.data(),
                                                 sn.empty() ? nullptr : sn.data(),
                                                 sm.uvs.empty() ? nullptr : sm.uvs.data(),
                                                 sm.num_vertices);
                    }
                }
                float hiro_col[4] = {1.0f, 0.95f, 0.8f, 1.0f};
                av::render_mesh(gm, final_m, hiro_col, false);
                if (st.show_wireframe) {
                    float wc[4] = {0.3f, 0.9f, 0.6f, 0.5f};
                    av::render_mesh(gm, final_m, wc, true);
                }
            }
        }

        // ── Hiro collision debug (Scene Player panel toggle) ──
        // Renders Hiro's real physics AABB (scl hitbox rect at his feet) and
        // a probe line down to the sampled ground, so "am I standing on the
        // platform or falling through it?" is visible in the viewport instead
        // of guessed from the footage.
        if (st.scene_player.show_collider && st.scene_player.hiro.active) {
            const float hw = st.scene_player.hero_stats.hitbox[2];
            const float hh = st.scene_player.hero_stats.hitbox[3];
            const float cx = st.scene_player.hiro.pos[0];
            const float cy = st.scene_player.hiro.pos[1];
            const float cz = st.scene_player.hiro.pos[2];
            const float hx = hw * 0.5f;
            // Physics AABB is a 2D rect (x = hitbox width, y = hitbox height)
            // at the body's z — Swordigo gameplay has no z-depth hitbox, so
            // four distinct corners and four edges, not an 8-corner box.
            const float c0[3] = { cx - hx, cy,      cz };
            const float c1[3] = { cx + hx, cy,      cz };
            const float c2[3] = { cx + hx, cy + hh, cz };
            const float c3[3] = { cx - hx, cy + hh, cz };
            // Vertical probe from the feet to the sampled ground height.
            float gnd_h = cy;
            const bool have_gnd =
                sg::game_ground_found(st.scene_player.game_world, cx, cz, cy,
                                      80.0f, gnd_h);
            const float probe_bottom = have_gnd ? std::min(gnd_h, cy) : cy - hh * 2.0f;
            float segs[6 * 3 * 3];   // 4 box edges + 2 ground probes
            int n = 0;
            const float* ring[4] = { c0, c1, c2, c3 };
            for (int e = 0; e < 4; ++e) {
                const float* a = ring[e];
                const float* b = ring[(e + 1) % 4];
                segs[n++] = a[0]; segs[n++] = a[1]; segs[n++] = a[2];
                segs[n++] = b[0]; segs[n++] = b[1]; segs[n++] = b[2];
            }
            // Drop lines from the two bottom corners down to the ground sample.
            for (int p = 0; p < 2; ++p) {
                const float* pc = (p == 0) ? c0 : c1;
                segs[n++] = pc[0]; segs[n++] = pc[1]; segs[n++] = pc[2];
                segs[n++] = pc[0]; segs[n++] = probe_bottom; segs[n++] = pc[2];
            }
            const float box_col[4] = {0.4f, 1.0f, 0.5f, 0.9f};
            av::render_lines(segs, n / 3, box_col, nullptr, 1.5f);
        }

        // ── Visible collision walls (Scene Player toggle) ──
        // The intelligent wall world (scene_collision.h runtime) drawn in
        // world space: solid=white, ground=green, one-way platform=blue,
        // unsafe (lava/spikes)=red. Built once from the scene's collision
        // shapes + ground polygons and mapped onto the scene while playing.
        if (st.scene_player.show_walls && st.scene_player.mode != sp::Mode::Off)
            sp::player_draw_walls(st.scene_player);
    }

    std::memcpy(av::g_light_dir, saved_light_dir, sizeof(saved_light_dir));
    std::memcpy(av::g_light_color, saved_light_color, sizeof(saved_light_color));
    std::memcpy(av::g_ambient_color, saved_ambient_color, sizeof(saved_ambient_color));
    std::memcpy(av::g_ambient_ground_color, saved_ambient_ground, sizeof(saved_ambient_ground));

    av::end_3d();

    // Scene lights are pass-local. Clearing the arrays here prevents normal
    // model previews, thumbnails and editor utility passes from inheriting the
    // last scene's directional or point-light configuration.
    av::clear_point_lights();
    av::clear_directional_lights();
    av::set_depth_fog(false, nullptr, 0.0f, 0.0f);

    const GLuint display_tex = postfx_display_tex(st);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)display_tex, ImVec2((float)w, (float)h), ImVec2(0, 1), ImVec2(1, 0));
    // The viewport is the Image item itself — capture hover right here so the
    // input block below works no matter what ImGui items get drawn afterwards.
    const bool viewport_hovered = ImGui::IsItemHovered();

    ImDrawList* overlay = ImGui::GetWindowDrawList();

    // Hard-clip every overlay to the viewport rect so stats, gizmo, labels, and
    // the axis indicator can never spill past the FBO image into the side panels.
    const ImVec2 viewport_min(pos.x, pos.y);
    const ImVec2 viewport_max(pos.x + (float)w, pos.y + (float)h);
    overlay->PushClipRect(viewport_min, viewport_max, true);

    const char* view_label = "Perspective";
    const ImVec2 view_size = ImGui::CalcTextSize(view_label);
    overlay->AddText(ImVec2(pos.x + (float)w - view_size.x - 16.0f, pos.y + 14.0f),
                     IM_COL32(175, 185, 200, 220), view_label);

    // ── Name labels for every selected object (screen-anchored) ──
    for (int sel : st.scene_selection) {
        if (sel < 0 || sel >= (int)st.scene.objects.size()) continue;
        const auto& sobj = st.scene.objects[sel];
        const float wpt[3] = {sobj.pos_x, sobj.pos_y, sobj.pos_z};
        ImVec2 sp;
        if (!swk::world_to_screen(st.camera, w, h, pos, wpt, sp)) continue;
        const ImVec2 label_pos(sp.x + 10.0f, sp.y - 8.0f);
        const ImVec2 text_size = ImGui::CalcTextSize(sobj.name.c_str());
        overlay->AddRectFilled(ImVec2(label_pos.x - 4.0f, label_pos.y - 2.0f),
                               ImVec2(label_pos.x + text_size.x + 4.0f, label_pos.y + text_size.y + 2.0f),
                               IM_COL32(18, 22, 30, 190), 3.0f);
        const bool is_active = (sel == st.selected_object);
        overlay->AddText(label_pos, is_active ? IM_COL32(110, 210, 255, 255)
                                              : IM_COL32(190, 200, 215, 255), sobj.name.c_str());
    }

    // ── Selection / edit status line ──
    if (st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
        const auto& sobj = st.scene.objects[st.selected_object];
        char sel_info[192];
        if (st.scene_mesh_edit && st.mesh_edit_mesh >= 0 &&
            st.mesh_edit_mesh < (int)sobj.ground_meshes.size()) {
            const auto& pm = sobj.ground_meshes[st.mesh_edit_mesh];
            if (st.mesh_edit_tool == 0 && st.mesh_edit_vertex >= 0) {
                const int v = st.mesh_edit_vertex;
                snprintf(sel_info, sizeof(sel_info), "Vtx %d  (%.2f, %.2f, %.2f)  mesh %d/%zu  verts %d",
                         v, pm.positions[v*3], pm.positions[v*3+1], pm.positions[v*3+2],
                         st.mesh_edit_mesh, sobj.ground_meshes.size(), pm.num_vertices);
            } else if (st.mesh_edit_tool == 1 && st.mesh_edit_triangle >= 0) {
                snprintf(sel_info, sizeof(sel_info), "Tri %d  mesh %d/%zu  tris %d",
                         st.mesh_edit_triangle, st.mesh_edit_mesh, sobj.ground_meshes.size(), pm.num_faces);
            } else if (st.mesh_edit_tool == 2 && st.mesh_edit_edge_a >= 0) {
                snprintf(sel_info, sizeof(sel_info), "Edge %d-%d  mesh %d/%zu",
                         st.mesh_edit_edge_a, st.mesh_edit_edge_b,
                         st.mesh_edit_mesh, sobj.ground_meshes.size());
            } else if (st.scene_selection.size() > 1) {
                snprintf(sel_info, sizeof(sel_info), "Sel: %zu objects  (active: %s)",
                         st.scene_selection.size(), sobj.name.c_str());
            } else {
                snprintf(sel_info, sizeof(sel_info), "Sel: %s  pos (%.1f, %.1f, %.1f)",
                         sobj.name.c_str(), sobj.pos_x, sobj.pos_y, sobj.pos_z);
            }
        } else if (st.scene_selection.size() > 1) {
            snprintf(sel_info, sizeof(sel_info), "Sel: %zu objects  (active: %s)",
                     st.scene_selection.size(), sobj.name.c_str());
        } else {
            snprintf(sel_info, sizeof(sel_info), "Sel: %s  pos (%.1f, %.1f, %.1f)",
                     sobj.name.c_str(), sobj.pos_x, sobj.pos_y, sobj.pos_z);
        }
        overlay->AddText(ImVec2(pos.x + 20.0f, pos.y + (float)h - 52.0f),
                         IM_COL32(160, 220, 160, 235), sel_info);
    }

    // ── ImGuizmo transform gizmo (Move / Rotate / Scale) ──
    const bool snap_active = st.scene_snap || io.KeyCtrl;
    if (!st.scene_mesh_edit && !st.gm_inline_edit && st.scene_show_gizmo && st.scene_transform_mode >= 1 &&
        st.scene_transform_mode <= 3 && !st.scene_transform_drag &&
        st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
        auto& gobj = st.scene.objects[st.selected_object];
        float view[16], proj[16];
        av::camera_get_view_matrix(st.camera, view);
        av::camera_get_projection(st.camera, (h > 0) ? (float)w / (float)h : 1.0f, proj);

        ImGui::PushID(0x51ED17);
        ImGuizmo::SetRect(pos.x, pos.y, (float)w, (float)h);
        ImGuizmo::SetDrawlist(overlay);

        float matrix[16];
        swk::object_world_matrix(gobj, matrix);
        const ImGuizmo::OPERATION op = st.scene_transform_mode == 1 ? ImGuizmo::TRANSLATE
                                     : st.scene_transform_mode == 2 ? ImGuizmo::ROTATE
                                     : ImGuizmo::SCALE;
        const bool was_using = ImGuizmo::IsUsing();
        float snap[3] = {0.0f, 0.0f, 0.0f};
        if (snap_active) {
            if (op == ImGuizmo::ROTATE) snap[0] = snap[1] = snap[2] = 15.0f;      // degrees
            else snap[0] = snap[1] = snap[2] = st.scene_snap_step;
        }
        // Capture the active object's transform before this frame's drag so
        // the same delta can be applied to every other selected object.
        const float pre_pos[3] = {gobj.pos_x, gobj.pos_y, gobj.pos_z};
        const float pre_rot = gobj.rot_y;
        const float pre_scl = gobj.scale_x;
        if (ImGuizmo::Manipulate(view, proj, op, ImGuizmo::WORLD, matrix, nullptr,
                                 snap_active ? snap : nullptr)) {
            if (!was_using && ImGuizmo::IsUsing()) snapshot_scene(st);   // snapshot at drag start
            // Decompose the manipulated matrix back into scene fields.
            gobj.pos_x = matrix[12]; gobj.pos_y = matrix[13]; gobj.pos_z = matrix[14];
            const float sx = std::sqrt(matrix[0]*matrix[0] + matrix[1]*matrix[1] + matrix[2]*matrix[2]);
            const float sy = std::sqrt(matrix[4]*matrix[4] + matrix[5]*matrix[5] + matrix[6]*matrix[6]);
            const float sz = std::sqrt(matrix[8]*matrix[8] + matrix[9]*matrix[9] + matrix[10]*matrix[10]);
            if (op == ImGuizmo::ROTATE) {
                // The scene format stores a single Z rotation (rot_y, radians).
                if (sx > 1e-6f && sy > 1e-6f)
                    gobj.rot_y = atan2f(matrix[1] / sy, matrix[0] / sx);
            } else if (op == ImGuizmo::SCALE) {
                const float ts = std::fabsf(gobj.template_scaling) > 1e-6f ? gobj.template_scaling : 1.0f;
                gobj.scale_x = std::clamp(sx / ts, 0.001f, 100.0f);
                gobj.scale_y = std::clamp(sy / ts, 0.001f, 100.0f);
                gobj.scale_z = std::clamp(sz / ts, 0.001f, 100.0f);
            }
            // Mirror the delta to the rest of the selection.
            if (st.scene_selection.size() > 1) {
                const float dpos[3] = {gobj.pos_x - pre_pos[0], gobj.pos_y - pre_pos[1], gobj.pos_z - pre_pos[2]};
                const float drot = gobj.rot_y - pre_rot;
                const float dscl = (pre_scl > 1e-6f) ? gobj.scale_x / pre_scl : 1.0f;
                for (int oi : st.scene_selection) {
                    if (oi == st.selected_object) continue;
                    auto& other = st.scene.objects[oi];
                    other.pos_x += dpos[0]; other.pos_y += dpos[1]; other.pos_z += dpos[2];
                    other.rot_y += drot;
                    if (op == ImGuizmo::SCALE) {
                        other.scale_x = std::clamp(other.scale_x * dscl, 0.001f, 100.0f);
                        other.scale_y = other.scale_z = other.scale_x;
                    }
                }
            }
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }
        ImGui::PopID();
    }


    // ── ImGuizmo view cube (top-right) ──
    {
        float view[16];
        av::camera_get_view_matrix(st.camera, view);
        const ImVec2 cube_pos(pos.x + (float)w - 118.0f, pos.y + 12.0f);
        ImGuizmo::ViewManipulate(view, st.camera.distance, cube_pos, ImVec2(96.0f, 96.0f),
                                 IM_COL32(18, 22, 30, 235));
        if (ImGuizmo::IsUsingViewManipulate() || ImGuizmo::IsViewManipulateHovered())
            decompose_view_to_camera(view, st.camera.target, st.camera);
    }
    const bool using_guizmo = ImGuizmo::IsUsing() || ImGuizmo::IsUsingViewManipulate();

    // ── Mesh-edit vertex handles ──
    if (st.scene_mesh_edit && st.mesh_edit_object >= 0 &&
        st.mesh_edit_object < (int)st.scene.objects.size()) {
        const auto& eobj = st.scene.objects[st.mesh_edit_object];
        if (!eobj.ground_meshes.empty()) {
            float eobj_mat[16];
            swk::object_world_matrix(eobj, eobj_mat);
            for (size_t mi = 0; mi < eobj.ground_meshes.size(); ++mi) {
                const auto& mesh = eobj.ground_meshes[mi];
                for (size_t vi = 0; vi < (size_t)mesh.num_vertices; ++vi) {
                    const float lp[3] = {mesh.positions[vi*3], mesh.positions[vi*3+1], mesh.positions[vi*3+2]};
                    const float wp[3] = { eobj_mat[0]*lp[0] + eobj_mat[4]*lp[1] + eobj_mat[8]*lp[2]  + eobj_mat[12],
                                          eobj_mat[1]*lp[0] + eobj_mat[5]*lp[1] + eobj_mat[9]*lp[2]  + eobj_mat[13],
                                          eobj_mat[2]*lp[0] + eobj_mat[6]*lp[1] + eobj_mat[10]*lp[2] + eobj_mat[14] };
                    ImVec2 sp;
                    if (!swk::world_to_screen(st.camera, w, h, pos, wp, sp)) continue;
                    const bool sel = (st.mesh_edit_mesh == (int)mi && st.mesh_edit_vertex == (int)vi);
                    const float r = sel ? 4.5f : 2.4f;
                    const ImU32 col = sel ? IM_COL32(255, 214, 64, 255) : IM_COL32(92, 222, 255, 235);
                    overlay->AddRectFilled(ImVec2(sp.x - r, sp.y - r), ImVec2(sp.x + r, sp.y + r), col);
                    if (sel)
                        overlay->AddRect(ImVec2(sp.x - 8.0f, sp.y - 8.0f),
                                         ImVec2(sp.x + 8.0f, sp.y + 8.0f),
                                         IM_COL32(255, 255, 255, 200));
                }
            }
            // Highlight the picked face / edge (face & edge tools).
            if (st.mesh_edit_tool == 1 && st.mesh_edit_triangle >= 0 &&
                st.mesh_edit_mesh >= 0 && st.mesh_edit_mesh < (int)eobj.ground_meshes.size()) {
                const auto& mesh = eobj.ground_meshes[st.mesh_edit_mesh];
                const size_t f = (size_t)st.mesh_edit_triangle * 3;
                if (f + 2 < mesh.indices.size()) {
                    ImVec2 pts[3];
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        const int vi = (int)mesh.indices[f + k];
                        const float lp[3] = {mesh.positions[vi*3], mesh.positions[vi*3+1], mesh.positions[vi*3+2]};
                        const float wp[3] = { eobj_mat[0]*lp[0] + eobj_mat[4]*lp[1] + eobj_mat[8]*lp[2]  + eobj_mat[12],
                                              eobj_mat[1]*lp[0] + eobj_mat[5]*lp[1] + eobj_mat[9]*lp[2]  + eobj_mat[13],
                                              eobj_mat[2]*lp[0] + eobj_mat[6]*lp[1] + eobj_mat[10]*lp[2] + eobj_mat[14] };
                        if (!swk::world_to_screen(st.camera, w, h, pos, wp, pts[k])) { ok = false; break; }
                    }
                    if (ok) {
                        overlay->AddTriangleFilled(pts[0], pts[1], pts[2], IM_COL32(255, 180, 40, 70));
                        overlay->AddTriangle(pts[0], pts[1], pts[2], IM_COL32(255, 200, 60, 255), 2.0f);
                    }
                }
            } else if (st.mesh_edit_tool == 2 && st.mesh_edit_edge_a >= 0 && st.mesh_edit_edge_b >= 0 &&
                       st.mesh_edit_mesh >= 0 && st.mesh_edit_mesh < (int)eobj.ground_meshes.size()) {
                const auto& mesh = eobj.ground_meshes[st.mesh_edit_mesh];
                ImVec2 pa, pb;
                const int va = st.mesh_edit_edge_a, vb = st.mesh_edit_edge_b;
                if (va >= 0 && vb >= 0 && va < mesh.num_vertices && vb < mesh.num_vertices) {
                    const float la[3] = {mesh.positions[va*3], mesh.positions[va*3+1], mesh.positions[va*3+2]};
                    const float lb[3] = {mesh.positions[vb*3], mesh.positions[vb*3+1], mesh.positions[vb*3+2]};
                    const float wa[3] = { eobj_mat[0]*la[0] + eobj_mat[4]*la[1] + eobj_mat[8]*la[2]  + eobj_mat[12],
                                          eobj_mat[1]*la[0] + eobj_mat[5]*la[1] + eobj_mat[9]*la[2]  + eobj_mat[13],
                                          eobj_mat[2]*la[0] + eobj_mat[6]*la[1] + eobj_mat[10]*la[2] + eobj_mat[14] };
                    const float wb[3] = { eobj_mat[0]*lb[0] + eobj_mat[4]*lb[1] + eobj_mat[8]*lb[2]  + eobj_mat[12],
                                          eobj_mat[1]*lb[0] + eobj_mat[5]*lb[1] + eobj_mat[9]*lb[2]  + eobj_mat[13],
                                          eobj_mat[2]*lb[0] + eobj_mat[6]*lb[1] + eobj_mat[10]*lb[2] + eobj_mat[14] };
                    if (swk::world_to_screen(st.camera, w, h, pos, wa, pa) &&
                        swk::world_to_screen(st.camera, w, h, pos, wb, pb))
                        overlay->AddLine(pa, pb, IM_COL32(255, 200, 60, 255), 3.0f);
                }
            }
            if (eobj.ground_meshes.empty())
                overlay->AddText(ImVec2(pos.x + 20.0f, pos.y + (float)h - 72.0f),
                                 IM_COL32(255, 160, 90, 255), "Object has no ground meshes");
        }
    }

    // ── Inline 2D polygon edit overlay (ground-mesh polygon in screen space) ──
    draw_inline_polygon_overlay(st, overlay, pos, w, h);

    // ── Axis indicator (bottom-left corner) ──
    if (st.scene_show_axis) {
        const float ax = pos.x + 26.0f;
        const float ay = pos.y + (float)h - 26.0f;
        const float L = 22.0f;
        overlay->AddLine(ImVec2(ax, ay), ImVec2(ax + L, ay), IM_COL32(235, 75, 75, 255), 2.0f);
        overlay->AddLine(ImVec2(ax, ay), ImVec2(ax, ay - L), IM_COL32(90, 205, 95, 255), 2.0f);
        overlay->AddLine(ImVec2(ax, ay), ImVec2(ax - L * 0.72f, ay + L * 0.72f), IM_COL32(85, 135, 240, 255), 2.0f);
        overlay->AddText(ImVec2(ax + L + 3.0f, ay - 4.0f), IM_COL32(235, 75, 75, 255), "X");
        overlay->AddText(ImVec2(ax + 2.0f, ay - L - 13.0f), IM_COL32(90, 205, 95, 255), "Y");
        overlay->AddText(ImVec2(ax - L * 0.72f - 14.0f, ay + L * 0.72f - 3.0f), IM_COL32(85, 135, 240, 255), "Z");
    }

    overlay->PopClipRect();

    // ── Inline 2D polygon editing owns the whole input block while active ──
    // The block keeps running while a vertex is grabbed even if the cursor
    // leaves the viewport, so fast drags never drop mid-gesture.
    if ((viewport_hovered || st.gm_dragging) && !using_guizmo && st.gm_inline_edit &&
        st.gm_edit_apply_object >= 0) {
        inline_edit_input(st, pos, w, h);
    }
    if (viewport_hovered && !using_guizmo && !st.gm_inline_edit) {

        // ── Keyboard shortcuts ──
        if (ImGui::IsKeyPressed(ImGuiKey_F)) frame_scene_selection(st);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) frame_scene_camera(st);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move_scene_object(st, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move_scene_object(st, 1);
        if (st.scene_mesh_edit) {
            // In mesh-edit: 1/2/3 switch vertex/face/edge tools, I inserts.
            if (ImGui::IsKeyPressed(ImGuiKey_1)) { st.mesh_edit_tool = 0; st.mesh_edit_vertex = -1; st.mesh_edit_triangle = -1; st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_2)) { st.mesh_edit_tool = 1; st.mesh_edit_vertex = -1; st.mesh_edit_triangle = -1; st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_3)) { st.mesh_edit_tool = 2; st.mesh_edit_vertex = -1; st.mesh_edit_triangle = -1; st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_I)) mesh_edit_insert_vertex(st);
        } else {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) { st.scene_transform_mode = 0; }
            if (ImGui::IsKeyPressed(ImGuiKey_2)) { st.scene_transform_mode = 1; }
            if (ImGui::IsKeyPressed(ImGuiKey_3)) { st.scene_transform_mode = 2; }
            if (ImGui::IsKeyPressed(ImGuiKey_4)) { st.scene_transform_mode = 3; }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_G)) { // Grab: switch to Move and start dragging the selection
            st.scene_transform_mode = 1; st.scene_mesh_edit = false;
            if (!st.scene_selection.empty()) {
                st.scene_pointer_active = true;
                st.scene_pointer_start = io.MousePos;
                st.scene_transform_drag = true;
                snapshot_scene(st);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S)) { st.scene_transform_mode = 3; st.scene_mesh_edit = false; } // Scale
        if (ImGui::IsKeyPressed(ImGuiKey_M)) {
            // M = inline 2D edit (2D-only, no legacy-3D fallback).
            if (st.selected_object >= 0 &&
                st.selected_object < (int)st.scene.objects.size()) {
                gm_begin_inline_edit(st);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X)) st.transform_axis = (st.transform_axis == 0) ? -1 : 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) st.transform_axis = (st.transform_axis == 1) ? -1 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) st.transform_axis = (st.transform_axis == 2) ? -1 : 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (st.gizmo_drag || st.mesh_edit_drag || st.scene_transform_drag) {
                st.gizmo_drag = st.mesh_edit_drag = st.scene_transform_drag = false;
                st.transform_axis = -1;
                st.gizmo_axis = -1;
            } else {
                st.scene_transform_mode = 0;
                st.scene_mesh_edit = false;
            }
        }

        // ── Delete: mesh element (vertex/face/edge) or selected object(s) ──
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (st.scene_mesh_edit && st.mesh_edit_object >= 0 &&
                st.mesh_edit_object < (int)st.scene.objects.size()) {
                auto& eobj = st.scene.objects[st.mesh_edit_object];
                if (st.mesh_edit_mesh >= 0 && st.mesh_edit_mesh < (int)eobj.ground_meshes.size()) {
                    snapshot_scene(st);
                    auto& pm = eobj.ground_meshes[st.mesh_edit_mesh];
                    bool ok = false;
                    if (st.mesh_edit_tool == 0 && st.mesh_edit_vertex >= 0) {
                        ok = swk::ground_mesh_delete_vertex(pm, st.mesh_edit_vertex);
                    } else if (st.mesh_edit_tool == 1 && st.mesh_edit_triangle >= 0) {
                        ok = swk::ground_mesh_delete_triangle(pm, st.mesh_edit_triangle);
                    } else if (st.mesh_edit_tool == 2 && st.mesh_edit_edge_a >= 0 && st.mesh_edit_edge_b >= 0) {
                        // Collapse the edge: delete both endpoints (higher index first).
                        const int hi = std::max(st.mesh_edit_edge_a, st.mesh_edit_edge_b);
                        const int lo = std::min(st.mesh_edit_edge_a, st.mesh_edit_edge_b);
                        ok = swk::ground_mesh_delete_vertex(pm, hi);
                        if (ok) ok = swk::ground_mesh_delete_vertex(pm, lo);
                    }
                    if (ok) {
                        swk::recompute_ground_mesh_geometry(pm);
                        reupload_object_ground_meshes(st, st.mesh_edit_object);
                        av::scene_mark_ground_mesh_dirty(st.scene, st.mesh_edit_object);
                        av::scene_refresh(st.scene);
                        st.mesh_edit_vertex = -1;
                        st.mesh_edit_triangle = -1;
                        st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
                        st.scene_dirty = true;
                    }
                }
            } else if (!st.scene_selection.empty()) {
                snapshot_scene(st);
                // Delete every selected object, highest index first so earlier
                // indices stay valid while erasing.
                std::vector<int> to_delete = st.scene_selection;
                std::sort(to_delete.begin(), to_delete.end(), std::greater<int>());
                for (int deleted : to_delete) {
                    if (deleted < 0 || deleted >= (int)st.scene.objects.size()) continue;
                    if (st.scene_ground_gpu_meshes.size() > (size_t)deleted)
                        for (auto& m : st.scene_ground_gpu_meshes[deleted]) av::free_mesh(m);
                    av::scene_delete_object(st.scene, static_cast<size_t>(deleted));
                }
                const int survivor = st.scene.objects.empty() ? -1
                    : std::max(0, std::min(to_delete.back(), (int)st.scene.objects.size() - 1));
                select_scene_object(st, survivor);
                st.mesh_edit_object = st.selected_object;
                st.mesh_edit_mesh = st.mesh_edit_vertex = -1;
                st.scene_dirty = true;
                resync_scene_ground_meshes(st);
            }
        }

        // ── LMB press ──
        if (ImGui::IsMouseClicked(0)) {
            st.scene_pointer_active = true;
            st.scene_pointer_start = io.MousePos;
            st.transform_axis = -1;

            // 1) Mesh-edit: element picking (vertex / face / edge by tool)
            if (st.scene_mesh_edit && st.mesh_edit_object >= 0 &&
                st.mesh_edit_object < (int)st.scene.objects.size()) {
                const auto& eobj = st.scene.objects[st.mesh_edit_object];
                float obj_mat[16];
                swk::object_world_matrix(eobj, obj_mat);
                if (st.mesh_edit_tool == 0) {
                    // Vertex handle picking
                    int m = -1, v = -1;
                    if (pick_mesh_edit_vertex(st, w, h, pos, io.MousePos, m, v)) {
                        st.mesh_edit_mesh = m;
                        st.mesh_edit_vertex = v;
                        st.mesh_edit_drag = true;
                        const auto& pm = eobj.ground_meshes[m];
                        const float lp[4] = {pm.positions[v*3], pm.positions[v*3+1], pm.positions[v*3+2], 1.0f};
                        st.mesh_edit_plane_y = obj_mat[1]*lp[0] + obj_mat[5]*lp[1] + obj_mat[9]*lp[2] + obj_mat[13];
                        snapshot_scene(st);
                    } else {
                        st.mesh_edit_mesh = st.mesh_edit_vertex = -1;
                    }
                } else {
                    // Face / edge picking across all ground meshes (best hit wins).
                    int best_m = -1, best_a = -1, best_b = -1;
                    float best_centroid_d = 1e30f;
                    for (int mi = 0; mi < (int)eobj.ground_meshes.size(); ++mi) {
                        const auto& pm = eobj.ground_meshes[mi];
                        if (st.mesh_edit_tool == 1) {
                            const int tri = swk::pick_ground_mesh_triangle(pm, obj_mat, st.camera, w, h,
                                                                           pos, io.MousePos);
                            if (tri >= 0) {
                                // Compare by triangle centroid proximity.
                                float wx[3], wy[3], wz[3];
                                const uint32_t ia = pm.indices[tri*3], ib = pm.indices[tri*3+1], ic = pm.indices[tri*3+2];
                                const float la[3] = {pm.positions[ia*3], pm.positions[ia*3+1], pm.positions[ia*3+2]};
                                const float lb[3] = {pm.positions[ib*3], pm.positions[ib*3+1], pm.positions[ib*3+2]};
                                const float lc[3] = {pm.positions[ic*3], pm.positions[ic*3+1], pm.positions[ic*3+2]};
                                const float c0[3] = {(la[0]+lb[0]+lc[0])/3.0f, (la[1]+lb[1]+lc[1])/3.0f, (la[2]+lb[2]+lc[2])/3.0f};
                                float cw[3];
                                cw[0] = obj_mat[0]*c0[0] + obj_mat[4]*c0[1] + obj_mat[8]*c0[2]  + obj_mat[12];
                                cw[1] = obj_mat[1]*c0[0] + obj_mat[5]*c0[1] + obj_mat[9]*c0[2]  + obj_mat[13];
                                cw[2] = obj_mat[2]*c0[0] + obj_mat[6]*c0[1] + obj_mat[10]*c0[2] + obj_mat[14];
                                ImVec2 sp;
                                if (swk::world_to_screen(st.camera, w, h, pos, cw, sp)) {
                                    const float d = std::hypotf(io.MousePos.x - sp.x, io.MousePos.y - sp.y);
                                    if (d < best_centroid_d) {
                                        best_centroid_d = d; best_m = mi; best_a = tri; best_b = -1;
                                    }
                                }
                            }
                        } else {
                            int a = -1, b = -1;
                            if (swk::pick_ground_mesh_edge(pm, obj_mat, st.camera, w, h, pos, io.MousePos, a, b)) {
                                best_m = mi; best_a = a; best_b = b;
                                break;  // first mesh edge hit is fine
                            }
                        }
                    }
                    if (best_m >= 0) {
                        st.mesh_edit_mesh = best_m;
                        if (st.mesh_edit_tool == 1) {
                            st.mesh_edit_triangle = best_a;
                            st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
                        } else {
                            st.mesh_edit_edge_a = best_a;
                            st.mesh_edit_edge_b = best_b;
                            st.mesh_edit_triangle = -1;
                        }
                    } else {
                        st.mesh_edit_triangle = -1;
                        st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
                    }
                }
            }

            // 2) Object picking (Ctrl = toggle multi-select; transform drag is
            //    ImGuizmo-driven in Move/Rotate/Scale modes)
            //    In face/edge tools object picking only runs when empty space
            //    was clicked (no element hit).
            if (!st.mesh_edit_drag &&
                (st.mesh_edit_tool == 0 || (st.mesh_edit_triangle < 0 && st.mesh_edit_edge_a < 0))) {
                const int hit = swk::pick_scene_object(st.scene.objects, &st.scene_model_cache,
                                                       st.scene_show_hidden, st.camera, w, h,
                                                       pos, io.MousePos);
                if (io.KeyCtrl) {
                    if (hit >= 0) toggle_scene_selection(st, hit);
                } else if (hit != st.selected_object) {
                    select_scene_object(st, hit);
                }
                if (st.scene_mesh_edit) st.mesh_edit_object = st.selected_object;
            }
        }

        // ── LMB drag: vertex edit / gizmo axis / free transform ──
        if (st.scene_pointer_active && ImGui::IsMouseDragging(0) &&
            st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
            auto& object = st.scene.objects[st.selected_object];

            if (st.mesh_edit_drag && st.mesh_edit_object == st.selected_object &&
                st.mesh_edit_mesh >= 0 && st.mesh_edit_vertex >= 0 &&
                st.mesh_edit_mesh < (int)object.ground_meshes.size()) {
                // Drag vertex on the world plane y = plane_y (ground-plane editing).
                float origin[3], dir[3];
                swk::screen_ray(st.camera, w, h, pos, io.MousePos, origin, dir);
                float world_point[3];
                if (swk::ray_plane_y(origin, dir, st.mesh_edit_plane_y, world_point)) {
                    if (snap_active) {
                        world_point[0] = swk::snap_value(world_point[0], st.scene_snap_step);
                        world_point[2] = swk::snap_value(world_point[2], st.scene_snap_step);
                    }
                    float obj_mat[16], inv[16];
                    swk::object_world_matrix(object, obj_mat);
                    if (av::mat4_inverse(inv, obj_mat)) {
                        auto& pm = object.ground_meshes[st.mesh_edit_mesh];
                        const int v = st.mesh_edit_vertex;
                        const float wp[4] = {world_point[0], world_point[1], world_point[2], 1.0f};
                        pm.positions[v*3]   = inv[0]*wp[0] + inv[4]*wp[1] + inv[8]*wp[2]  + inv[12];
                        pm.positions[v*3+1] = inv[1]*wp[0] + inv[5]*wp[1] + inv[9]*wp[2]  + inv[13];
                        pm.positions[v*3+2] = inv[2]*wp[0] + inv[6]*wp[1] + inv[10]*wp[2] + inv[14];
                        swk::recompute_ground_mesh_geometry(pm);
                        reupload_object_ground_meshes(st, st.selected_object);
                        av::scene_mark_ground_mesh_dirty(st.scene, st.selected_object);
                    }
                }
            } else if (st.scene_transform_drag) {
                // Compute this frame's delta on the active object, then apply
                // the same delta to every selected object.
                float dpos[3] = {0.0f, 0.0f, 0.0f};
                float drot = 0.0f;
                float dscl = 1.0f;
                if (st.scene_transform_mode == 1) {
                    if (st.transform_axis >= 0) {
                        // Keyboard axis lock: same math as the gizmo axis path.
                        const int axis = st.transform_axis;
                        float axis_vec[3] = {0, 0, 0};
                        axis_vec[axis] = 1.0f;
                        const float world_pos[3] = {object.pos_x, object.pos_y, object.pos_z};
                        ImVec2 origin_sp, probe_sp;
                        if (swk::world_to_screen(st.camera, w, h, pos, world_pos, origin_sp)) {
                            const float probe_world[3] = {world_pos[0] + axis_vec[0] * 10.0f,
                                                          world_pos[1] + axis_vec[1] * 10.0f,
                                                          world_pos[2] + axis_vec[2] * 10.0f};
                            float dx = 1.0f, dy = 0.0f;
                            if (swk::world_to_screen(st.camera, w, h, pos, probe_world, probe_sp)) {
                                dx = probe_sp.x - origin_sp.x;
                                dy = probe_sp.y - origin_sp.y;
                                const float len = std::sqrt(dx*dx + dy*dy);
                                if (len > 1e-4f) { dx /= len; dy /= len; }
                            }
                            const float units = 2.0f * st.camera.distance *
                                tanf(st.camera.fov * 3.14159265358979323846f / 360.0f) / std::max(1, h);
                            dpos[axis] = (io.MouseDelta.x * dx + io.MouseDelta.y * dy) * units;
                        }
                    } else {
                        float right[3], up[3], forward[3];
                        swk::camera_basis(st.camera, right, up, forward);
                        const float units = 2.0f * st.camera.distance *
                            tanf(st.camera.fov * 3.14159265358979323846f / 360.0f) / std::max(1, h);
                        for (int axis = 0; axis < 3; ++axis)
                            dpos[axis] = right[axis] * io.MouseDelta.x * units - up[axis] * io.MouseDelta.y * units;
                    }
                } else if (st.scene_transform_mode == 2) {
                    drot = io.MouseDelta.x * 0.01f;
                } else if (st.scene_transform_mode == 3) {
                    dscl = expf(io.MouseDelta.x * 0.01f);
                }
                for (int idx : st.scene_selection) {
                    if (idx < 0 || idx >= (int)st.scene.objects.size()) continue;
                    auto& o = st.scene.objects[idx];
                    o.pos_x += dpos[0]; o.pos_y += dpos[1]; o.pos_z += dpos[2];
                    o.rot_y += drot;
                    if (st.scene_transform_mode == 3) {
                        o.scale_x = std::clamp(o.scale_x * dscl, 0.001f, 100.0f);
                        o.scale_y = o.scale_z = o.scale_x;
                    }
                    if (snap_active && st.scene_transform_mode == 1) {
                        if (st.transform_axis < 0) {
                            o.pos_x = swk::snap_value(o.pos_x, st.scene_snap_step);
                            o.pos_y = swk::snap_value(o.pos_y, st.scene_snap_step);
                            o.pos_z = swk::snap_value(o.pos_z, st.scene_snap_step);
                        } else {
                            if (st.transform_axis == 0) o.pos_x = swk::snap_value(o.pos_x, st.scene_snap_step);
                            else if (st.transform_axis == 1) o.pos_y = swk::snap_value(o.pos_y, st.scene_snap_step);
                            else o.pos_z = swk::snap_value(o.pos_z, st.scene_snap_step);
                        }
                    }
                }
            }
            av::scene_refresh(st.scene);
            st.scene_dirty = true;
        }

        // ── LMB release ──
        if (st.scene_pointer_active && ImGui::IsMouseReleased(0)) {
            const float dx = io.MousePos.x - st.scene_pointer_start.x;
            const float dy = io.MousePos.y - st.scene_pointer_start.y;
            // Ctrl+click toggling already happened on press; on release only a
            // plain (no-Ctrl) click activates an object (idempotent single
            // select, so double-firing here is harmless).  Without this guard
            // a Ctrl+click would toggle twice and cancel out.
            if (dx*dx + dy*dy <= 16.0f && !st.scene_transform_drag && !st.mesh_edit_drag &&
                !io.KeyCtrl) {
                const int hit = swk::pick_scene_object(st.scene.objects, &st.scene_model_cache,
                                                       st.scene_show_hidden, st.camera, w, h,
                                                       pos, io.MousePos);
                if (hit != st.selected_object) select_scene_object(st, hit);
            }
            st.scene_pointer_active = false;
            st.scene_transform_drag = false;
            st.mesh_edit_drag = false;
            st.gizmo_drag = false;
            st.gizmo_axis = -1;
        }

        // ── Zoom (exponential: smooth at any distance) ──
        // The inline 2D editor owns the wheel while active (even when the cursor
        // leaves the viewport over a side panel, the global zoom must not fire).
        if (!st.gm_inline_edit && io.MouseWheel != 0.0f) {
            const float factor = std::pow(0.94f, io.MouseWheel * st.cam_zoom_speed);
            const float new_dist = std::clamp(st.camera.distance * factor, 0.1f, 2000.0f);

            if (st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
                // Scroll zoom converges on the active object instead of drifting
                // around an old scene center.
                const auto& selected = st.scene.objects[st.selected_object];
                const float focus = std::min(0.35f, 0.12f * std::fabs(io.MouseWheel));
                st.camera.target[0] += (selected.pos_x - st.camera.target[0]) * focus;
                st.camera.target[1] += (selected.pos_y - st.camera.target[1]) * focus;
                st.camera.target[2] += (selected.pos_z - st.camera.target[2]) * focus;
            } else {
                // Nothing selected: dolly toward the point under the cursor so
                // zooming feels anchored (Blender-style), instead of drifting
                // around a stale scene center.
                float pivot[3];
                if (cursor_focal_point(st.camera, w, h, pos, io.MousePos, pivot)) {
                    const float t = 1.0f - new_dist / st.camera.distance;
                    st.camera.target[0] += (pivot[0] - st.camera.target[0]) * t;
                    st.camera.target[1] += (pivot[1] - st.camera.target[1]) * t;
                    st.camera.target[2] += (pivot[2] - st.camera.target[2]) * t;
                }
            }
            st.camera.distance = new_dist;
            // Keep the clip planes in sync so deep zoom-out never culls the
            // scene and close zoom-in never clips through the camera.
            st.camera.near_plane = std::max(0.01f, new_dist / 10000.0f);
            st.camera.far_plane = std::max(1000.0f, new_dist + 4000.0f);
        }

        // ── Esc ends inline 2D editing (even when the cursor leaves the viewport) ──
        if (st.gm_inline_edit && ImGui::IsKeyPressed(ImGuiKey_Escape)) gm_end_inline_edit(st);

        // ── Orbit: LMB in Navigate mode, or MMB anywhere ──
        // (LMB/RMB are owned by the inline 2D editor while it is active.)
        if ((st.scene_transform_mode == 0 && !st.scene_mesh_edit && !st.gm_inline_edit &&
             ImGui::IsMouseDragging(0)) ||
            ImGui::IsMouseDragging(2)) {
            ImVec2 delta = io.MouseDelta;
            float sign_x = st.cam_invert_x ? -1.0f : 1.0f;
            float sign_y = st.cam_invert_y ? -1.0f : 1.0f;
            st.camera.yaw   += delta.x * 0.5f * st.cam_orbit_speed * sign_x;
            st.camera.pitch += delta.y * 0.5f * st.cam_orbit_speed * sign_y;
            if (st.camera.pitch >  89.0f) st.camera.pitch =  89.0f;
            if (st.camera.pitch < -89.0f) st.camera.pitch = -89.0f;
        }

        // ── Pan (RMB) ──
        if (!st.gm_inline_edit && ImGui::IsMouseDragging(1)) {
            ImVec2 delta = io.MouseDelta;
            float scale = st.camera.distance * 0.003f * st.cam_pan_speed;
            float yaw_rad = st.camera.yaw * 3.14159f / 180.0f;
            float pitch_rad = st.camera.pitch * 3.14159f / 180.0f;

            // Compute local camera Right and Up vectors
            float rx = cosf(yaw_rad);
            float rz = -sinf(yaw_rad);

            float ux = -sinf(yaw_rad) * sinf(pitch_rad);
            float uy = cosf(pitch_rad);
            float uz = -cosf(yaw_rad) * sinf(pitch_rad);

            // Translate camera target locally
            st.camera.target[0] -= rx * delta.x * scale;
            st.camera.target[2] -= rz * delta.x * scale;

            st.camera.target[0] += ux * delta.y * scale;
            st.camera.target[1] += uy * delta.y * scale;
            st.camera.target[2] += uz * delta.y * scale;
        }
    }
    ImGui::PopStyleColor(3);
}

// ============================================================================
// UI: Center panel — Text / Hex general file previewer
// ============================================================================

struct Token {
    std::string text;
    ImVec4 color;
};

static std::vector<Token> tokenize_line(const std::string& line, bool is_dark_theme) {
    std::vector<Token> tokens;
    
    // Define theme colors matching IntelliJ Darcula (dark) / Light themes
    ImVec4 c_text     = ImGui::GetStyle().Colors[ImGuiCol_Text];
    ImVec4 c_keyword  = is_dark_theme ? ImVec4(0.80f, 0.47f, 0.20f, 1.0f) : ImVec4(0.00f, 0.20f, 0.70f, 1.0f); // Orange / Blue
    ImVec4 c_string   = is_dark_theme ? ImVec4(0.41f, 0.53f, 0.35f, 1.0f) : ImVec4(0.02f, 0.49f, 0.09f, 1.0f); // Green / Dark Green
    ImVec4 c_number   = is_dark_theme ? ImVec4(0.41f, 0.59f, 0.73f, 1.0f) : ImVec4(0.09f, 0.31f, 0.92f, 1.0f); // Cyan / Blue
    ImVec4 c_comment  = is_dark_theme ? ImVec4(0.50f, 0.50f, 0.50f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f); // Gray
    ImVec4 c_bracket  = is_dark_theme ? ImVec4(0.66f, 0.72f, 0.78f, 1.0f) : ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    ImVec4 c_property = is_dark_theme ? ImVec4(0.80f, 0.80f, 0.80f, 1.0f) : ImVec4(0.30f, 0.30f, 0.30f, 1.0f);

    size_t i = 0;
    size_t n = line.length();
    
    // 1. Single-line comment check (# or --)
    size_t first_non_ws = line.find_first_not_of(" \t");
    if (first_non_ws != std::string::npos) {
        if (line[first_non_ws] == '#' || (first_non_ws + 1 < n && line[first_non_ws] == '-' && line[first_non_ws + 1] == '-')) {
            tokens.push_back({line, c_comment});
            return tokens;
        }
    }

    std::string current;
    auto emit_current = [&](ImVec4 col) {
        if (!current.empty()) {
            tokens.push_back({current, col});
            current.clear();
        }
    };

    while (i < n) {
        char ch = line[i];

        // String literals
        if (ch == '\'' || ch == '"') {
            emit_current(c_text);
            char quote = ch;
            current += ch;
            i++;
            while (i < n) {
                current += line[i];
                if (line[i] == quote && line[i - 1] != '\\') {
                    i++;
                    break;
                }
                i++;
            }
            emit_current(c_string);
            continue;
        }

        // Brackets / Punctuation
        if (ch == '{' || ch == '}' || ch == '(' || ch == ')' || ch == '[' || ch == ']') {
            emit_current(c_text);
            current += ch;
            i++;
            emit_current(c_bracket);
            continue;
        }

        // Colon / Separator check
        if (ch == ':') {
            emit_current(c_property);
            current += ch;
            i++;
            emit_current(c_bracket);
            continue;
        }

        // Identifiers and numbers
        if (isalnum(ch) || ch == '_' || ch == '.' || ch == '-') {
            current += ch;
            i++;
            continue;
        }

        // Whitespace and symbols
        emit_current(c_text);
        while (i < n && !isalnum(line[i]) && line[i] != '_' && line[i] != '\'' && line[i] != '"' &&
               line[i] != '{' && line[i] != '}' && line[i] != '(' && line[i] != ')' && line[i] != '[' && line[i] != ']' && line[i] != ':') {
            current += line[i];
            i++;
        }
        emit_current(c_text);
    }
    emit_current(c_text);

    // Color keywords and numbers
    for (auto& tok : tokens) {
        if (tok.color.x == c_text.x && tok.color.y == c_text.y && tok.color.z == c_text.z) {
            if (tok.text == "Template" || tok.text == "Object" || tok.text == "Component" ||
                tok.text == "local" || tok.text == "self" || tok.text == "target" ||
                tok.text == "if" || tok.text == "then" || tok.text == "end" ||
                tok.text == "return" || tok.text == "function" || tok.text == "nil" ||
                tok.text == "true" || tok.text == "false" || tok.text == "and" ||
                tok.text == "or" || tok.text == "not") {
                tok.color = c_keyword;
            } else if (!tok.text.empty() && (isdigit(tok.text[0]) || (tok.text[0] == '-' && tok.text.size() > 1 && isdigit(tok.text[1])))) {
                tok.color = c_number;
            }
        }
    }

    return tokens;
}

struct InputTextCallback_UserData {
    std::string*            Str;
    ImGuiInputTextCallback  ChainCallback;
    void*                   ChainCallbackUserData;
};

static int InputTextCallback(ImGuiInputTextCallbackData* data) {
    InputTextCallback_UserData* user_data = (InputTextCallback_UserData*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = user_data->Str;
        IM_ASSERT(data->Buf == str->c_str());
        str->resize(data->BufTextLen);
        data->Buf = (char*)str->c_str();
    }
    if (user_data->ChainCallback) {
        data->UserData = user_data->ChainCallbackUserData;
        return user_data->ChainCallback(data);
    }
    return 0;
}

static bool InputTextMultilineStr(const char* label, std::string* str, const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr) {
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    flags |= ImGuiInputTextFlags_CallbackResize;
    InputTextCallback_UserData cb_user_data;
    cb_user_data.Str = str;
    cb_user_data.ChainCallback = callback;
    cb_user_data.ChainCallbackUserData = user_data;
    return ImGui::InputTextMultiline(label, (char*)str->c_str(), str->capacity() + 1, size, flags, InputTextCallback, &cb_user_data);
}

static void replace_all(std::string& str, const std::string& from, const std::string& to) {
    if(from.empty()) return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

static void draw_text_preview(ViewerState& st) {
    auto dot = st.sel_path.rfind('.');
    std::string ext = (dot != std::string::npos) ? st.sel_path.substr(dot) : "";
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    
    bool is_protobuf = (ext == ".scene" || ext == ".scl" || ext == ".gdata" || ext == ".gstate" || 
                        ext == ".gplayer" || ext == ".gopt" || ext == ".sounds" || 
                        ext == ".scmap" || ext == ".atlas" || ext == ".fnt");
    bool is_gmesh = (ext == ".gmesh");

    // Initialize edit buffer on file change
    static std::string last_loaded_path;
    if (st.sel_path != last_loaded_path) {
        last_loaded_path = st.sel_path;
        st.text_edit_buffer = st.text_preview_content;
        st.text_edit_modified = false;
        st.has_compile_result = false;
    }

    // Toolbar Header
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(g_theme.accent, ICON_FA_FILE " %s", st.sel_name.c_str());
    ImGui::SameLine();
    
    // Save button (professional visual feedback)
    bool can_save = !st.text_is_binary && (st.text_edit_modified || is_protobuf);
    
    if (!can_save) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, st.text_edit_modified ? ImVec4(0.85f, 0.35f, 0.15f, 1.0f) : ImVec4(0.12f, 0.52f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st.text_edit_modified ? ImVec4(0.95f, 0.45f, 0.25f, 1.0f) : ImVec4(0.16f, 0.64f, 0.28f, 1.0f));
    if (ImGui::Button(ICON_FA_FLOPPY_DISK)) {
        if (is_protobuf) {
            auto start = std::chrono::high_resolution_clock::now();
            try {
                std::string binary_data = filerift::recode_markup(st.text_edit_buffer, ext.substr(1));
                std::ofstream out(st.sel_path, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("failed to open output file");
                out.write(binary_data.data(), binary_data.size());
                if (!out) throw std::runtime_error("failed to write complete output file");
                st.text_preview_content = st.text_edit_buffer;
                st.text_edit_modified = false;
                st.status_msg = "Compiled markup and saved " + st.sel_name;
                log_file_event("FileEncode", "Compiled markup to binary and saved: " + st.sel_name);

                auto end = std::chrono::high_resolution_clock::now();
                st.compile_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
                st.compile_success = true;
                st.has_compile_result = true;
            } catch (const std::exception& e) {
                st.status_msg = "Compilation Error: " + std::string(e.what());

                auto end = std::chrono::high_resolution_clock::now();
                st.compile_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
                st.compile_success = false;
                st.compile_error_msg = e.what();
                st.has_compile_result = true;
            }
        } else {
            std::ofstream out(st.sel_path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(st.text_edit_buffer.data(), st.text_edit_buffer.size());
                st.text_preview_content = st.text_edit_buffer;
                st.text_edit_modified = false;
                st.status_msg = "Saved " + st.sel_name + " successfully!";
                log_file_event("FileSave", "Saved file: " + st.sel_name);
                
                st.has_compile_result = false;
            } else st.status_msg = "Failed to write file: " + st.sel_path;
        }
    }
    ImGui::PopStyleColor(2);
    if (!can_save) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save File");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLOCK)) {
        last_loaded_path.clear();
        st.status_msg = "Reloaded " + st.sel_name;
        st.has_compile_result = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload file from disk");

    // Format-specific helpers
    if (is_gmesh) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PUZZLE_PIECE " Compile GroundMesh to SCL")) {
            std::string markup = boulder::generate_ground_mesh(st.text_edit_buffer);
            if (!markup.empty()) {
                try {
                    std::string binary_data = filerift::recode_markup(markup, "scl");
                    // Save to the same filename but with .scl extension
                    std::string scl_path = st.sel_path.substr(0, st.sel_path.rfind('.')) + ".scl";
                    std::ofstream out(scl_path, std::ios::binary);
                    if (out) {
                        out.write(binary_data.data(), binary_data.size());
                        st.status_msg = "Successfully compiled GroundMesh and saved to " + fs::path(scl_path).filename().string();
                        log_file_event("GMeshCompile", "Compiled GroundMesh to SCL: " + scl_path);
                    }
                } catch (const std::exception& e) {
                    st.status_msg = "GroundMesh Compilation Error: " + std::string(e.what());
                }
            } else {
                st.status_msg = "GroundMesh compilation yielded empty markup.";
            }
        }
    }

    // Style Selection combo box and Import button
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 280.0f);
    ImGui::SetNextItemWidth(140.0f);
    
    std::string combo_items;
    for (const auto& pair : st.intellij_styles) {
        combo_items += pair.first;
        combo_items += '\0';
    }
    combo_items += '\0';
    
    int prev_idx = st.intellij_style_idx;
    if (ImGui::Combo("Style", &st.intellij_style_idx, combo_items.c_str())) {
        if (st.intellij_style_idx != prev_idx) {
            const std::string& path = st.intellij_styles[st.intellij_style_idx].second;
            if (path == "embedded://grove") {
                st.intellij_editor.load_style_from_memory(intel::FILERIFT_GROOVE_STYX_CONTENT);
            } else if (path == "embedded://batsyntax") {
                st.intellij_editor.load_style_from_memory(intel::BAT_SYNTAX_STYX_CONTENT);
            } else if (path == "embedded://fallback") {
                st.intellij_editor.load_default_style();
            } else {
                st.intellij_editor.load_style(path);
            }
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS)) {
        ImGui::OpenPopup("Import Style Sheet");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import .styx style sheet");

    // Modal popup for importing style
    if (ImGui::BeginPopupModal("Import Style Sheet", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        static bool scanned = false;
        static std::vector<std::pair<std::string, std::string>> found_files;
        static int selected_item = -1;
        static char import_path[512] = "";
        static std::string validation_status = "Select a file to validate";
        static bool validation_ok = false;
        static std::vector<std::string> validation_details;
        
        static bool popup_was_open = false;
        bool is_popup_open = ImGui::IsPopupOpen("Import Style Sheet");
        if (is_popup_open && !popup_was_open) {
            scanned = false;
            import_path[0] = '\0';
        }
        popup_was_open = is_popup_open;
        
        // Lambda for validation
        auto validate_style = [&](const std::string& path) {
            validation_details.clear();
            validation_ok = false;
            if (path.empty()) {
                validation_status = "Path is empty";
                return;
            }
            if (!fs::exists(path) || fs::is_directory(path)) {
                validation_status = "File does not exist";
                return;
            }
            intel::IntelliJ temp_val;
            if (temp_val.load_style(path)) {
                validation_ok = true;
                const auto& sh = temp_val.get_style();
                validation_status = "Valid Styx Style Sheet";
                validation_details.push_back("Name: " + sh.name);
                validation_details.push_back("Rules: " + std::to_string(sh.styles.size()));
                validation_details.push_back("Keywords: " + std::to_string(sh.keywords.size()));
                if (!sh.line_comment.empty()) {
                    validation_details.push_back("Line comment: " + sh.line_comment);
                }
            } else {
                validation_status = "Failed to parse JSON/Styx structure";
            }
        };
        
        // Scan directories once
        if (!scanned) {
            found_files.clear();
            std::vector<std::string> dirs_to_scan = {
                "/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoTools/ss",
                "/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoTools",
                "/home/quantumcreeper/SwordigoDesktop",
                st.current_dir
            };
            std::set<std::string> unique_paths;
            for (const auto& d : dirs_to_scan) {
                if (d.empty()) continue;
                try {
                    if (fs::exists(d) && fs::is_directory(d)) {
                        for (const auto& entry : fs::directory_iterator(d)) {
                            if (entry.is_regular_file() && entry.path().extension() == ".styx") {
                                std::string full_p = entry.path().string();
                                if (unique_paths.find(full_p) == unique_paths.end()) {
                                    unique_paths.insert(full_p);
                                    found_files.push_back({entry.path().filename().string(), full_p});
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
            scanned = true;
            selected_item = -1;
            validation_status = "Select a file to validate";
            validation_ok = false;
            validation_details.clear();
        }
        
        ImGui::TextColored(g_theme.accent, ICON_FA_PAINT_BRUSH " Styx Stylesheet Hub");
        ImGui::TextDisabled("Auto-scanned project and media directories for stylesheets.");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Layout: Two columns
        ImGui::Columns(2, "import_layout", true);
        ImGui::SetColumnWidth(0, 260.0f);
        
        // Left Column: Discovered files
        ImGui::Text(ICON_FA_FOLDER_OPEN " Discovered files:");
        ImGui::BeginChild("DiscoveredList", ImVec2(0, 180.0f), ImGuiChildFlags_Borders);
        if (found_files.empty()) {
            ImGui::TextDisabled("No .styx files found");
        } else {
            for (int i = 0; i < (int)found_files.size(); i++) {
                const auto& item = found_files[i];
                bool is_selected = (selected_item == i);
                if (ImGui::Selectable(item.first.c_str(), is_selected)) {
                    selected_item = i;
                    strncpy(import_path, item.second.c_str(), sizeof(import_path) - 1);
                    import_path[sizeof(import_path) - 1] = '\0';
                    validate_style(import_path);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", item.second.c_str());
                }
            }
        }
        ImGui::EndChild();
        if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Rescan")) {
            scanned = false;
        }
        
        ImGui::NextColumn();
        
        // Right Column: Path details and Validation
        ImGui::Text(ICON_FA_FILE " Stylesheet Path:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##style_import_path", import_path, sizeof(import_path))) {
            selected_item = -1; // path manual edit deselects list item
            validate_style(import_path);
        }
        
        ImGui::Spacing();
        ImGui::Text("Validation:");
        ImGui::BeginChild("ValidationResults", ImVec2(0, 110.0f), ImGuiChildFlags_Borders);
        if (validation_ok) {
            ImGui::TextColored(ImVec4(0.20f, 0.80f, 0.40f, 1.0f), ICON_FA_CIRCLE_CHECK " %s", validation_status.c_str());
            for (const auto& detail : validation_details) {
                ImGui::BulletText("%s", detail.c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.30f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", validation_status.c_str());
        }
        ImGui::EndChild();
        
        ImGui::Columns(1);
        ImGui::Separator();
        ImGui::Spacing();
        
        // Action Buttons
        if (!validation_ok) ImGui::BeginDisabled();
        if (ImGui::Button("Import & Apply", ImVec2(140.0f, 32.0f))) {
            if (st.intellij_editor.load_style(import_path)) {
                std::string stem = fs::path(import_path).stem().string();
                bool exists = false;
                for (int i = 0; i < (int)st.intellij_styles.size(); i++) {
                    if (st.intellij_styles[i].second == import_path) {
                        st.intellij_style_idx = i;
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    st.intellij_styles.push_back({stem, import_path});
                    st.intellij_style_idx = (int)st.intellij_styles.size() - 1;
                }
                st.status_msg = "Successfully imported & applied: " + stem;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!validation_ok) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 32.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    ImGui::SameLine();
    ImGui::ColorEdit4("BG", &st.editor_custom_bg.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Custom Editor Background (alpha > 0 to enable, 0 to use style default)");

    ImGui::Separator();
    
    // Call unified IntelliJ editor pane
    st.intellij_editor.draw_editor("text_preview", &st.text_edit_buffer, st.text_edit_modified, st.sel_path, st.mono_font, st.editor_custom_bg,
                                   st.has_compile_result, st.compile_success, st.compile_error_msg, st.compile_time_ms);
}

// ── Real PTY Terminal ─────────────────────────────────────────────────────
#ifndef _WIN32
#include <pty.h>
#include <utmp.h>
#include <sys/ioctl.h>
#include <poll.h>
#endif
#include <fcntl.h>
#include <signal.h>

// Strip common ANSI escape sequences from raw PTY output so ImGui can render it.
static std::string strip_ansi(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_esc = false;
    bool in_csi = false;
    for (size_t i = 0; i < src.size(); i++) {
        unsigned char c = (unsigned char)src[i];
        if (in_csi) {
            // CSI sequences end on a byte 0x40–0x7E
            if (c >= 0x40 && c <= 0x7E) { in_csi = false; in_esc = false; }
        } else if (in_esc) {
            if (c == '[') { in_csi = true; }
            else { in_esc = false; } // other escape — just discard
        } else if (c == 0x1B) {
            in_esc = true;
        } else if (c == '\r') {
            // carriage-return: move to start of current line in out
            auto nl = out.rfind('\n');
            if (nl != std::string::npos) out.resize(nl + 1);
            else out.clear();
        } else if (c == 0x07 || c == 0x08) {
            // BEL / BS — skip
            if (c == 0x08 && !out.empty()) out.pop_back();
        } else {
            out += (char)c;
        }
    }
    return out;
}

#if defined(_WIN32)

// Windows has no POSIX forkpty/PTY subsystem; the Terminal pane is inert.
static void pty_spawn(ViewerState& st) { (void)st; }
static void pty_poll(ViewerState& st) { (void)st; }
static void pty_send(ViewerState& st, const char* t, size_t n) { (void)st; (void)t; (void)n; }
static void pty_shutdown(ViewerState& st) { (void)st; }

#else

static void pty_spawn(ViewerState& st) {
    if (st.pty_master_fd >= 0) return; // already running

    struct winsize ws = {};
    ws.ws_col = 220; ws.ws_row = 50;

    int master_fd;
    pid_t pid = forkpty(&master_fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        st.pty_output_buf += "[ruby] forkpty failed: " + std::string(strerror(errno)) + "\n";
        return;
    }
    if (pid == 0) {
        // child — exec bash with a clean environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        const char* shell = getenv("SHELL");
        if (!shell) shell = "/bin/bash";
        execl(shell, shell, "--login", nullptr);
        _exit(1);
    }
    // parent
    st.pty_master_fd  = master_fd;
    st.pty_child_pid  = pid;
    // set non-blocking
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    st.terminal_scroll_to_bottom = true;
}

static void pty_poll(ViewerState& st) {
    if (st.pty_master_fd < 0) return;
    char buf[4096];
    ssize_t n;
    bool got_data = false;
    while ((n = read(st.pty_master_fd, buf, sizeof(buf))) > 0) {
        st.pty_output_buf += strip_ansi(std::string(buf, n));
        got_data = true;
    }
    // Limit buffer size
    static const size_t MAX_BUF = 256 * 1024;
    if (st.pty_output_buf.size() > MAX_BUF) {
        st.pty_output_buf.erase(0, st.pty_output_buf.size() - MAX_BUF);
    }
    if (got_data) st.terminal_scroll_to_bottom = true;
    // Check if child died
    if (n == 0 || (n < 0 && errno == EIO)) {
        close(st.pty_master_fd);
        st.pty_master_fd = -1;
        st.pty_child_pid = -1;
        st.pty_output_buf += "\n[ruby] shell exited.\n";
        st.terminal_scroll_to_bottom = true;
    }
}

static void pty_send(ViewerState& st, const char* text, size_t len) {
    if (st.pty_master_fd < 0) return;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(st.pty_master_fd, text + written, len - written);
        if (n <= 0) break;
        written += n;
    }
}

static void pty_shutdown(ViewerState& st) {
    if (st.pty_master_fd >= 0) {
        close(st.pty_master_fd);
        st.pty_master_fd = -1;
    }
    if (st.pty_child_pid > 0) {
        kill(st.pty_child_pid, SIGTERM);
        st.pty_child_pid = -1;
    }
}

#endif // !_WIN32

static void draw_bottom_panel(ViewerState& st) {
    // Lazily spawn the PTY bash shell the first time the panel is opened
    if (st.pty_master_fd < 0) pty_spawn(st);
    // Drain any pending PTY output every frame
    pty_poll(st);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
    ImGui::BeginChild("BottomPanelArea", ImVec2(0, 220.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    if (ImGui::BeginTabBar("BottomPanelTabBar")) {

        // ── Terminal Tab ────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_TERMINAL " Terminal")) {
            const float input_h = ImGui::GetFrameHeightWithSpacing() + 6.0f;

            // ── Output pane ────────────────────────────────────────────────
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1.0f)); // solid terminal black
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
            ImGui::BeginChild("PtyOutputScroll", ImVec2(0, -input_h), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

            if (st.mono_font) ImGui::PushFont(st.mono_font);
            // Render line by line from the raw buffer with terminal syntax coloring
            {
                const std::string& buf = st.pty_output_buf;
                size_t line_start = 0;
                while (line_start <= buf.size()) {
                    size_t nl = buf.find('\n', line_start);
                    size_t line_end = (nl == std::string::npos) ? buf.size() : nl;
                    if (line_end > line_start) {
                        std::string line = buf.substr(line_start, line_end - line_start);
                        
                        // Parse bash prompt line
                        size_t at_pos = line.find('@');
                        size_t colon_pos = (at_pos != std::string::npos) ? line.find(':', at_pos) : std::string::npos;
                        size_t dollar_pos = (colon_pos != std::string::npos) ? line.find('$', colon_pos) : std::string::npos;
                        
                        if (at_pos != std::string::npos && colon_pos != std::string::npos && dollar_pos != std::string::npos && dollar_pos < line.size() - 1) {
                            // User
                            std::string user = line.substr(0, at_pos);
                            ImGui::TextColored(ImVec4(0.18f, 0.80f, 0.44f, 1.0f), "%s", user.c_str());
                            ImGui::SameLine(0, 0);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "@");
                            ImGui::SameLine(0, 0);
                            
                            // Host
                            std::string host = line.substr(at_pos + 1, colon_pos - at_pos - 1);
                            ImGui::TextColored(ImVec4(0.18f, 0.80f, 0.44f, 1.0f), "%s", host.c_str());
                            ImGui::SameLine(0, 0);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), ":");
                            ImGui::SameLine(0, 0);
                            
                            // Path
                            std::string path = line.substr(colon_pos + 1, dollar_pos - colon_pos - 1);
                            ImGui::TextColored(ImVec4(0.20f, 0.60f, 0.86f, 1.0f), "%s", path.c_str());
                            ImGui::SameLine(0, 0);
                            
                            // Dollar
                            std::string p_char = line.substr(dollar_pos, 2);
                            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%s", p_char.c_str());
                            ImGui::SameLine(0, 0);
                            
                            // Command
                            std::string cmd = line.substr(dollar_pos + 2);
                            ImGui::TextUnformatted(cmd.c_str());
                        }
                        // Parse compiler tags (e.g. [CXX], [LINK])
                        else if (line[0] == '[' && line.find(']') != std::string::npos) {
                            size_t r_bracket = line.find(']');
                            std::string tag = line.substr(0, r_bracket + 1);
                            std::string rest = line.substr(r_bracket + 1);
                            
                            ImVec4 tag_col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                            if (tag == "[CXX]") tag_col = ImVec4(0.20f, 0.60f, 0.86f, 1.0f);
                            else if (tag == "[LINK]") tag_col = ImVec4(0.90f, 0.45f, 0.15f, 1.0f);
                            else if (tag == "[AV]" || tag == "[PVR]") tag_col = ImVec4(0.68f, 0.38f, 0.78f, 1.0f);
                            
                            ImGui::TextColored(tag_col, "%s", tag.c_str());
                            ImGui::SameLine(0, 0);
                            ImGui::TextUnformatted(rest.c_str());
                        }
                        // Highlight errors
                        else if (line.find("error:") != std::string::npos || line.find("Error:") != std::string::npos) {
                            ImGui::TextColored(ImVec4(0.90f, 0.25f, 0.25f, 1.0f), "%s", line.c_str());
                        }
                        // Highlight warnings
                        else if (line.find("warning:") != std::string::npos || line.find("Warning:") != std::string::npos) {
                            ImGui::TextColored(ImVec4(0.90f, 0.65f, 0.15f, 1.0f), "%s", line.c_str());
                        }
                        else {
                            ImGui::TextUnformatted(line.c_str());
                        }
                    } else if (nl == std::string::npos) {
                        break;
                    } else {
                        ImGui::TextUnformatted(""); // blank line
                    }
                    line_start = line_end + 1;
                }
            }
            if (st.terminal_scroll_to_bottom) {
                ImGui::SetScrollHereY(1.0f);
                st.terminal_scroll_to_bottom = false;
            }
            if (st.mono_font) ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // ── Input row ──────────────────────────────────────────────────
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));
            ImGui::BeginChild("PtyInputRow", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

            if (st.mono_font) ImGui::PushFont(st.mono_font);

            // Replicate local bash prompt
            char host[128] = "yoga-7-pro-fedora";
#ifndef _WIN32
            gethostname(host, sizeof(host));
#else
            DWORD host_len = sizeof(host);
            if (GetComputerNameA(host, &host_len) == 0) strcpy(host, "windows");
#endif
            const char* user = getenv("USER");
            if (!user) user = "quantumcreeper";
            
            if (st.pty_master_fd >= 0) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.18f, 0.80f, 0.44f, 1.0f), "%s@%s", user, host);
                ImGui::SameLine(0, 0);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), ":");
                ImGui::SameLine(0, 0);
                ImGui::TextColored(ImVec4(0.20f, 0.60f, 0.86f, 1.0f), "~/SwordigoDesktop");
                ImGui::SameLine(0, 0);
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "$ ");
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.85f, 0.25f, 0.25f, 1.0f), "[dead] ");
            }
            ImGui::SameLine();

            bool reclaim_focus = false;
            ImGui::SetNextItemWidth(-1.0f);
            ImGuiInputTextFlags iflags = ImGuiInputTextFlags_EnterReturnsTrue
                                       | ImGuiInputTextFlags_CallbackHistory;

            // Command history for up/down arrow
            static std::vector<std::string> cmd_history;
            static int                      history_pos = -1;

            struct HistoryCB {
                static int callback(ImGuiInputTextCallbackData* data) {
                    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
                    int hp = *(int*)data->UserData;
                    if (data->EventKey == ImGuiKey_UpArrow) {
                        if (hp < (int)cmd_history.size() - 1) hp++;
                    } else if (data->EventKey == ImGuiKey_DownArrow) {
                        if (hp > -1) hp--;
                    }
                    *(int*)data->UserData = hp;
                    const std::string& hist_line = (hp >= 0) ? cmd_history[cmd_history.size() - 1 - hp] : std::string{};
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, hist_line.c_str());
                    return 0;
                }
            };

            bool entered = ImGui::InputText(
                "##pty_input", st.terminal_input, sizeof(st.terminal_input),
                iflags, HistoryCB::callback, &history_pos);

            // Blinking block cursor overlay
            if (ImGui::IsItemActive() && st.terminal_input[0] == '\0') {
                if ((int)(ImGui::GetTime() * 2.0f) % 2 == 0) {
                    ImVec2 min_pos = ImGui::GetItemRectMin() + ImVec2(4.0f, 3.0f);
                    ImVec2 max_pos = min_pos + ImVec2(8.0f, ImGui::GetItemRectSize().y - 6.0f);
                    ImGui::GetWindowDrawList()->AddRectFilled(min_pos, max_pos, IM_COL32(0, 215, 100, 255)); // terminal green color
                }
            }

            if (entered) {
                std::string line(st.terminal_input);
                if (!line.empty()) {
                    // push to history
                    cmd_history.push_back(line);
                    if (cmd_history.size() > 200) cmd_history.erase(cmd_history.begin());
                    history_pos = -1;
                }
                line += "\n";
                pty_send(st, line.c_str(), line.size());
                st.terminal_input[0] = '\0';
                reclaim_focus = true;
            }

            if (st.mono_font) ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (reclaim_focus) ImGui::SetKeyboardFocusHere(-1);

            ImGui::EndTabItem();
        }

        // ── Output / Logger Tab ─────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_TRIANGLE_EXCLAMATION " Output / Logger")) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));
            ImGui::BeginChild("LoggerLogScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

            if (st.mono_font) ImGui::PushFont(st.mono_font);
            for (const auto& entry : g_file_logs) {
                if (entry.find("ERROR") != std::string::npos)
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", entry.c_str());
                else if (entry.find("FileSave") != std::string::npos || entry.find("FileEncode") != std::string::npos)
                    ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1.0f), "%s", entry.c_str());
                else if (entry.find("FileDecode") != std::string::npos)
                    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s", entry.c_str());
                else
                    ImGui::TextUnformatted(entry.c_str());
            }
            // Auto-scroll when near bottom
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
                ImGui::SetScrollHereY(1.0f);
            if (st.mono_font) ImGui::PopFont();

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================================
// UI: Center panel dispatcher
// ============================================================================

static void draw_center_panel(ViewerState& st) {
    if (st.preview_type == PREVIEW_SCENE) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, g_theme.panel_alt);
        ImGui::BeginChild("##ScenePathBar", ImVec2(0, 28.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        draw_breadcrumbs(st);
        if (!st.sel_name.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
            ImGui::SameLine();
            ImGui::TextUnformatted(st.sel_name.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        static const char* labels[] = {
            ICON_FA_CODE " Source",
            ICON_FA_CUBE " Visual",
            ICON_FA_LAYER_GROUP " Tree",
            ICON_FA_WRENCH " Mesh"
        };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));
        // Flat segmented switcher: idle tabs have no resting chrome, only the
        // active editor keeps a strong accent fill. Reads as tabs, not pills.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th_alpha(g_theme.surface_hover, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_theme.surface_hover);
        for (int mode = 0; mode < 4; ++mode) {
            if (mode > 0) ImGui::SameLine();
            const bool active = st.scene_preview_tab == mode;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, th_mix(g_theme.surface_active, g_theme.accent, 0.35f));
            if (ImGui::Button(labels[mode])) {
                if (mode == 0 && st.scene_preview_tab != 0 && !st.scene_text_dirty) {
                    const std::string binary = av::scene_serialize(st.scene);
                    st.text_edit_buffer = filerift::decode_protobuf(binary, "scene");
                    st.text_preview_content = st.text_edit_buffer;
                }
                switch_scene_tab(st, mode);
            }
            if (active) ImGui::PopStyleColor();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (st.scene_text_dirty && st.scene_preview_tab != 0) {
            ImGui::TextColored(g_theme.warning,
                ICON_FA_TRIANGLE_EXCLAMATION " Source changes are not reflected in Visual/Tree until compiled with Save.");
        }
        ImGui::Separator();
    }

    switch (st.preview_type) {
        case PREVIEW_MODEL:   draw_model_viewport(st);  break;
        case PREVIEW_TEXTURE: draw_texture_preview(st);  break;
        case PREVIEW_TEXT:    draw_text_preview(st);     break;
        case PREVIEW_SCENE:
            if (st.scene_preview_tab == 0) {
                bool scene_text_modified = st.scene_text_dirty;
                st.intellij_editor.draw_editor("scene_text", &st.text_edit_buffer, scene_text_modified, st.sel_path, st.mono_font, st.editor_custom_bg,
                                               st.has_compile_result, st.compile_success, st.compile_error_msg, st.compile_time_ms);
                if (scene_text_modified != st.scene_text_dirty) {
                    st.scene_text_dirty = scene_text_modified;
                    st.scene_dirty = st.scene_dirty || scene_text_modified;
                }
            } else if (st.scene_preview_tab == 1) {
                draw_scene_visualizer(st);
            } else if (st.scene_preview_tab == 3) {
                draw_ground_mesh_generator(st);
            } else {
                draw_scene_inspector(st);
            }
            break;
        case PREVIEW_AUDIO:   draw_audio_player(st);     break;
        default: {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 text_size = ImGui::CalcTextSize("Select a file on the left to preview");
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetCursorPosX() + (avail.x - text_size.x) * 0.5f,
                ImGui::GetCursorPosY() + (avail.y - text_size.y) * 0.5f));
            ImGui::TextDisabled("Select a file on the left to preview");
        } break;
    }
}

static bool compile_scene_source(ViewerState& st, std::string* error_message = nullptr) {
    try {
        const std::string binary = filerift::recode_markup(st.text_edit_buffer, "scene");
        const fs::path destination(st.scene.filepath);
        const fs::path temporary = destination.string() + ".ruby-source.tmp";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create temporary scene file");
        out.write(binary.data(), static_cast<std::streamsize>(binary.size()));
        out.close();
        if (!out) throw std::runtime_error("failed while writing temporary scene file");
        std::error_code rename_error;
        fs::rename(temporary, destination, rename_error);
        if (rename_error) {
            std::error_code remove_error;
            fs::remove(temporary, remove_error);
            throw std::runtime_error(rename_error.message());
        }

        st.scene = av::scene_load(st.scene.filepath);
        st.selected_object = -1;
        st.scene_selection.clear();
        sync_scene_object_editor(st);
        st.scene_undo_stack.clear();
        st.scene_redo_stack.clear();
        st.scene_dirty = false;
        st.scene_text_dirty = false;

        for (auto& entry : st.scene_gpu_mesh_cache)
            for (auto& mesh : entry.second) av::free_mesh(mesh);
        st.scene_gpu_mesh_cache.clear();
        for (auto& entry : st.scene_texture_cache)
            for (GLuint texture : entry.second) if (texture) glDeleteTextures(1, &texture);
        st.scene_texture_cache.clear();
        st.scene_model_cache.clear();
        for (auto& meshes : st.scene_ground_gpu_meshes)
            for (auto& mesh : meshes) av::free_mesh(mesh);
        for (auto& textures : st.scene_ground_textures)
            for (GLuint texture : textures) if (texture) glDeleteTextures(1, &texture);

        const fs::path scene_dir = destination.parent_path();
        for (const auto& object : st.scene.objects) {
            if (!object.mesh_name.empty()) load_scene_model_to_cache(st, object.mesh_name, scene_dir.string());
            if (!object.background_name.empty()) load_scene_background_texture(st, object.background_name, scene_dir.string());
        }
        upload_scene_ground_meshes(st, scene_dir.string());
        frame_scene_camera(st);
        return true;
    } catch (const std::exception& error) {
        if (error_message) *error_message = error.what();
        return false;
    }
}

// ============================================================================
// UI: Properties panel (right)
// ============================================================================

static void draw_properties_panel(ViewerState& st) {
    ImGui::BeginChild("Properties", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 7)); // roomier section rhythm

    // ── Scene Player: while playing, the right panel becomes the transport
    //    console (the normal component inspector is hidden until stop). ──
    if (st.scene_player.mode != sp::Mode::Off) {
        ImGui::TextColored(g_theme.accent, ICON_FA_SLIDERS "  Scene Player");
        ImGui::Separator();
        sp::player_draw_panel(st.scene_player);
        ImGui::Separator();
        if (ImGui::Button(ICON_FA_STOP "  Stop and Reload Scene")) {
            sp::player_end(st.scene_player);
            st.scene_player_window_open = false;
            st.scene_anim_playing = false;
            // Reload the scene from its .scene file so all runtime motion,
            // AI state and camera changes are discarded and the editor returns
            // to a clean edit state (same path as opening the file fresh).
            if (!st.scene.filepath.empty()) {
                FileEntry fe;
                fe.name = fs::path(st.scene.filepath).filename().string();
                fe.full_path = st.scene.filepath;
                fe.is_dir = false;
                std::error_code ec;
                fe.size = (size_t)fs::file_size(st.scene.filepath, ec);
                fe.type = FTYPE_SCENE;
                select_file(st, fe);
            }
            st.status_msg = "Scene player stopped — scene reloaded for editing.";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop playback and reload the scene from disk, returning to edit mode.");
        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        return;
    }

    ImGui::TextColored(g_theme.accent, ICON_FA_SLIDERS "  Inspector");
    ImGui::Separator();

    if (st.preview_type == PREVIEW_NONE && st.sel_name.empty()) {
        ImGui::TextDisabled("Select a file to view metadata.");
        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        return;
    }

    if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("%s%s", st.sel_name.c_str(),
                st.preview_type == PREVIEW_SCENE && st.scene_dirty ? " *" : "");
    ImGui::TextDisabled("%s  |  %s", format_size(st.sel_size).c_str(), filetype_label(
        st.preview_type == PREVIEW_TEXTURE ? FTYPE_TEXTURE :
        st.preview_type == PREVIEW_MODEL   ? FTYPE_MODEL   :
        st.preview_type == PREVIEW_SCENE   ? FTYPE_SCENE   :
        st.preview_type == PREVIEW_AUDIO   ? FTYPE_AUDIO   : FTYPE_OTHER,
        st.sel_path.c_str()));
    ImGui::TextDisabled("Path");
    std::string visible_path = st.sel_path;
    const float path_width = ImGui::GetContentRegionAvail().x;
    while (visible_path.size() > 12 && ImGui::CalcTextSize(visible_path.c_str()).x > path_width)
        visible_path.erase(3, 1);
    if (visible_path != st.sel_path) visible_path.replace(0, 3, "...");
    ImGui::TextUnformatted(visible_path.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.sel_path.c_str());
    }
    ImGui::Separator();

    switch (st.preview_type) {
    case PREVIEW_MODEL: {
        ImGui::Text("Meshes:   %d", (int)st.model.meshes.size());
        ImGui::Text("Vertices: %d", st.model.total_vertices);
        ImGui::Text("Faces:    %d", st.model.total_faces);
        ImGui::Text("Frames:   %d", st.model.num_frames);
        ImGui::Separator();

        // Animation Player (Blender Style)
        if (st.model.num_frames > 0) {
            ImGui::TextColored(g_theme.accent, "Animation Player");

            if (st.anim_playing) {
                if (ImGui::Button(ICON_FA_PAUSE " Pause")) st.anim_playing = false;
            } else {
                if (ImGui::Button(ICON_FA_PLAY " Play")) st.anim_playing = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Next")) {
                st.current_frame = std::fmod(st.current_frame + 1.0f, (float)st.model.num_frames);
            }
            ImGui::SameLine();
            if (ImGui::Button("Prev")) {
                st.current_frame = std::fmod(st.current_frame - 1.0f + st.model.num_frames,
                                             (float)st.model.num_frames);
            }

            ImGui::SliderFloat("Frame", &st.current_frame, 0.0f,
                               (float)st.model.num_frames - 1.0f, "%.2f");
            ImGui::SliderFloat("FPS", &st.anim_fps, 1.0f, 120.0f, "%.0f");
            ImGui::Separator();
        }

        // 3D Lighting Editor (Blender Style)
        ImGui::TextColored(g_theme.accent, "Interactive Lighting");
        
        static float el = 45.0f;
        static float az = 45.0f;
        
        bool changed = false;
        if (ImGui::SliderFloat("Elevation", &el, -90.0f, 90.0f, "%.1f°")) changed = true;
        if (ImGui::SliderFloat("Azimuth", &az, -180.0f, 180.0f, "%.1f°")) changed = true;
        
        if (changed) {
            float el_rad = el * 3.14159265f / 180.0f;
            float az_rad = az * 3.14159265f / 180.0f;
            av::g_light_dir[0] = cosf(el_rad) * sinf(az_rad);
            av::g_light_dir[1] = sinf(el_rad);
            av::g_light_dir[2] = cosf(el_rad) * cosf(az_rad);
            
            float len = sqrtf(av::g_light_dir[0]*av::g_light_dir[0] + av::g_light_dir[1]*av::g_light_dir[1] + av::g_light_dir[2]*av::g_light_dir[2]);
            if (len > 0.0001f) {
                av::g_light_dir[0] /= len;
                av::g_light_dir[1] /= len;
                av::g_light_dir[2] /= len;
            }
        }
        
        ImGui::ColorEdit3("Key Light Color", av::g_light_color);
        ImGui::ColorEdit3("Fill Light Color", av::g_fill_color);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cool fill from the side opposite the key light — keeps undersides and the shaded side of the hero readable instead of dead black.");
        ImGui::SliderFloat("Rim Light", &av::g_rim_strength, 0.0f, 1.5f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Camera-opposed edge light that pops silhouettes off the background.");
        ImGui::SliderFloat("Specular", &av::g_spec_strength, 0.0f, 0.5f, "%.3f");
        ImGui::ColorEdit3("Ambient (Sky)", av::g_ambient_color);
        ImGui::ColorEdit3("Ambient (Ground)", av::g_ambient_ground_color);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hemisphere fill: surfaces facing down (undersides, ceilings, wall bases)\nfall toward this darker value. Keeps corners readable without crushing them.");
        
        if (ImGui::Button("Reset Lights")) {
            el = 45.0f;
            az = 45.0f;
            av::g_light_dir[0] = 0.577f; av::g_light_dir[1] = 0.577f; av::g_light_dir[2] = 0.577f;
            av::g_light_color[0] = 1.0f; av::g_light_color[1] = 0.95f; av::g_light_color[2] = 0.9f;
            av::g_fill_color[0] = 0.30f; av::g_fill_color[1] = 0.38f; av::g_fill_color[2] = 0.55f;
            av::g_rim_strength = 0.32f;
            av::g_spec_strength = 0.06f;
            av::g_ambient_color[0] = 0.19f; av::g_ambient_color[1] = 0.20f; av::g_ambient_color[2] = 0.25f;
            av::g_ambient_ground_color[0] = 0.09f; av::g_ambient_ground_color[1] = 0.095f; av::g_ambient_ground_color[2] = 0.14f;
        }
        ImGui::Separator();

        if (ImGui::TreeNode("Bounding Box")) {
            ImGui::Text("Min: (%.3f, %.3f, %.3f)", st.model.min_x, st.model.min_y, st.model.min_z);
            ImGui::Text("Max: (%.3f, %.3f, %.3f)", st.model.max_x, st.model.max_y, st.model.max_z);
            ImGui::Text("Center: (%.3f, %.3f, %.3f)", st.model.center_x, st.model.center_y, st.model.center_z);
            ImGui::Text("Radius: %.3f", st.model.radius);
            ImGui::TreePop();
        }
        ImGui::Separator();

        if (ImGui::TreeNode("Mesh Elements")) {
            for (int i = 0; i < (int)st.model.meshes.size(); i++) {
                auto& m = st.model.meshes[i];
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "Mesh %d (%d verts)##m%d", i, m.num_vertices, i);
                if (ImGui::Selectable(lbl, i == st.highlighted_mesh)) {
                    st.highlighted_mesh = (st.highlighted_mesh == i) ? -1 : i;
                }
            }
            ImGui::TreePop();
        }
        ImGui::Separator();

        ImGui::TextColored(g_theme.accent, "Render Settings");
        ImGui::Checkbox("Textured Mode", &st.show_textured);
        ImGui::Checkbox("Wireframe Mode", &st.show_wireframe);
        ImGui::Checkbox("Show Skeleton", &st.show_skeleton);

        if (ImGui::TreeNode("Texture Dependencies")) {
            for (size_t k = 0; k < st.model.texture_filenames.size(); k++) {
                const auto& tname = st.model.texture_filenames[k];
                bool is_missing = std::find(st.missing_textures.begin(), st.missing_textures.end(), tname) != st.missing_textures.end();
                if (is_missing) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "[X] %s (MISSING!)", tname.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "[O] %s", tname.c_str());
                }
            }
            if (st.model.texture_filenames.empty()) {
                ImGui::TextDisabled("None (using matching texture)");
            }
            ImGui::TreePop();
        }

        ImGui::Separator();
        ImGui::TextColored(g_theme.accent, "Model Transforms");
        ImGui::SliderFloat("Rotate X", &st.model_rotate_x, -180.0f, 180.0f, "%.1f°");
        ImGui::SliderFloat("Rotate Y", &st.model_rotate_y, -180.0f, 180.0f, "%.1f°");
        ImGui::SliderFloat("Rotate Z", &st.model_rotate_z, -180.0f, 180.0f, "%.1f°");
        ImGui::SliderFloat("Scale", &st.model_scale, 0.1f, 10.0f, "%.2fx");
        
        if (ImGui::Button("Reset Transform")) {
            st.model_rotate_x = 0.0f;
            st.model_rotate_y = 0.0f;
            st.model_rotate_z = 0.0f;
            st.model_scale = 1.0f;
        }

        ImGui::Separator();
        ImGui::TextColored(g_theme.accent, "Export Options");
        ImGui::BeginDisabled(st.blender_active);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Export to Blender", ImVec2(-1, 32))) {
            blender_start_roundtrip(st);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(st.blender_active
                ? "A Blender round-trip is in progress."
                : "Export the POD to Blender via GLB, edit it there, and save to round-trip back.");
        }
        if (!st.blender_status.empty()) {
            ImGui::TextColored(st.blender_active ? ImVec4(0.9f, 0.8f, 0.3f, 1.0f)
                                                 : ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", st.blender_status.c_str());
        }

        // ── FBX → game POD conversion (only meaningful for .fbx previews) ──
        {
            std::string sel_low = st.sel_path;
            for (auto& c : sel_low) c = (char)tolower((unsigned char)c);
            bool is_fbx = sel_low.size() >= 4 && sel_low.substr(sel_low.size() - 4) == ".fbx";
            if (is_fbx) {
                ImGui::Separator();
                ImGui::TextColored(g_theme.accent, ICON_FA_GEAR "  Convert to Game");
                ImGui::Checkbox("Flip V (FBX top → game bottom)", &st.convert_flip_v);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The game samples POD UVs with v = 0 at the bottom; FBX/glTF UVs have v = 0 at the top, so this flips them to match.");
                ImGui::Checkbox("Convert textures to game .pvr", &st.convert_tex_pgm);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Re-encodes referenced textures to the game's ETC1/PVR .v3 (.pvr) container next to the output POD.");
                if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Convert to POD", ImVec2(-1, 32))) {
                    av::PodConvertOptions opts;
                    opts.flip_v           = st.convert_flip_v;
                    opts.convert_textures = st.convert_tex_pgm;
                    opts.output_pvr       = true;
                    std::string out = fs::path(st.sel_path).parent_path() /
                                      (fs::path(st.sel_path).stem().string() + ".POD");
                    std::vector<std::string> written;
                    std::string conv_err;
                    if (av::fbx_to_pod(st.sel_path, out, opts, &written, &conv_err)) {
                        st.convert_status = "Wrote " + out;
                        if (!written.empty())
                            st.convert_status += "  (" + std::to_string(written.size()) + " textures)";
                        st.status_msg = st.convert_status;
                    } else {
                        st.convert_status = "Failed: " + (conv_err.empty() ? std::string("unknown error") : conv_err);
                        st.status_msg = st.convert_status;
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Convert the selected FBX into a game .POD (plus textures) next to the source file.");
                if (!st.convert_status.empty())
                    ImGui::TextWrapped("%s", st.convert_status.c_str());
            }
        }
    } break;

    case PREVIEW_TEXTURE: {
        ImGui::Text("Width:  %d px", st.tex_w);
        ImGui::Text("Height: %d px", st.tex_h);
        ImGui::Text("Format: %s", st.tex_format_str.c_str());
        ImGui::Text("Zoom:   %.0f%%", st.tex_zoom * 100.0f);
        ImGui::Separator();
        if (ImGui::Button("Reset View")) {
            st.tex_zoom = 1.0f;
            st.tex_pan_x = st.tex_pan_y = 0.0f;
            st.tex_reset_required = true;
        }
        ImGui::Checkbox("Pixel Art Mode", &st.tex_pixel_art_mode);
        
        ImGui::Separator();
        ImGui::TextColored(g_theme.accent, "Texture Operations");
        
        ImGui::BeginDisabled(st.tex_edit_enabled && st.tex_edit_valid);
        if (ImGui::Button("Rotate 90° CW")) {
            st.texture_rotation = (st.texture_rotation + 90) % 360;
        }
        ImGui::SameLine();
        if (ImGui::Button("Rotate 90° CCW")) {
            st.texture_rotation = (st.texture_rotation + 270) % 360;
        }
        
        if (ImGui::Button("Rotate 180°")) {
            st.texture_rotation = (st.texture_rotation + 180) % 360;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Rotation")) {
            st.texture_rotation = 0;
        }
        
        ImGui::Checkbox("Flip Horizontally", &st.texture_flip_h);
        ImGui::Checkbox("Flip Vertically", &st.texture_flip_v);
        
        ImGui::ColorEdit4("Color Tint", st.texture_tint);
        
        if (ImGui::Button("Reset Filters")) {
            st.texture_flip_h = true;
            st.texture_flip_v = true;
            st.texture_rotation = 0;
            st.texture_tint[0] = st.texture_tint[1] = st.texture_tint[2] = st.texture_tint[3] = 1.0f;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextDisabled(ICON_FA_PAINTBRUSH " Texture editor: use the toolbar at the top of the preview viewport.");


    } break;

    case PREVIEW_SCENE: {
        // ── Scene header / summary ──────────────────────────────────
        ImGui::TextColored(ImVec4(0.55f,0.80f,0.55f,1.0f), ICON_FA_CUBE " %s", st.scene.filename.c_str());
        ImGui::Text("Objects: %d  |  Libraries: %d  |  Groups: %d",
            (int)st.scene.objects.size(),
            (int)st.scene.object_libraries.size(),
            (int)st.scene.groups.size());
        ImGui::Separator();

        if (!st.scene_save_msg.empty()) {
            ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.40f, 1.0f),
                               ICON_FA_CHECK " %s", st.scene_save_msg.c_str());
        }
        if (st.scene_dirty)
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                               ICON_FA_TRIANGLE_EXCLAMATION " Unsaved changes");

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Object libraries: %zu", st.scene.object_libraries.size());
            ImGui::Text("Bounds records: %zu", st.scene.bounds.size());
            ImGui::Text("Groups: %zu", st.scene.groups.size());
            ImGui::Text("Unknown fields: %zu", st.scene.other_fields.size());
            ImGui::Text("Scene OnLoad scripts: %zu", st.scene.onload_scripts.size());

            if (ImGui::Button(ICON_FA_PLUS " Scene OnLoad")) {
                snapshot_scene(st);
                std::string program;
                av::scene_set_program_source(program, "-- Scene OnLoad\n");
                st.scene.onload_scripts.push_back(std::move(program));
                st.scene_script_index = static_cast<int>(st.scene.onload_scripts.size()) - 1;
                snprintf(st.scene_script_buf, sizeof(st.scene_script_buf), "%s",
                         av::scene_program_source(st.scene.onload_scripts.back()).c_str());
                st.scene_dirty = true;
            }
            if (!st.scene.onload_scripts.empty()) {
                ImGui::SameLine();
                const char* preview = st.scene_script_index >= 0 ? "Selected script" : "Choose script";
                if (ImGui::BeginCombo("##scene_script", preview)) {
                    for (int script = 0; script < static_cast<int>(st.scene.onload_scripts.size()); ++script) {
                        const std::string label = "OnLoad #" + std::to_string(script + 1);
                        if (ImGui::Selectable(label.c_str(), st.scene_script_index == script)) {
                            st.scene_script_index = script;
                            snprintf(st.scene_script_buf, sizeof(st.scene_script_buf), "%s",
                                av::scene_program_source(st.scene.onload_scripts[script]).c_str());
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            if (st.scene_script_index >= 0 &&
                st.scene_script_index < static_cast<int>(st.scene.onload_scripts.size())) {
                if (ImGui::InputTextMultiline("##scene_onload", st.scene_script_buf,
                        sizeof(st.scene_script_buf), ImVec2(-1, 130), ImGuiInputTextFlags_AllowTabInput)) {
                    snapshot_scene(st);
                    av::scene_set_program_source(st.scene.onload_scripts[st.scene_script_index],
                                                 st.scene_script_buf);
                    st.scene_dirty = true;
                }
                if (ImGui::Button("Remove Scene OnLoad")) {
                    snapshot_scene(st);
                    st.scene.onload_scripts.erase(st.scene.onload_scripts.begin() + st.scene_script_index);
                    st.scene_script_index = -1;
                    st.scene_script_buf[0] = '\0';
                    st.scene_dirty = true;
                }
            }
        }
        ImGui::Separator();

        // ── Background layers: pick which background quad renders ──
        int bg_count = 0;
        for (const auto& o : st.scene.objects)
            if (!o.background_name.empty()) ++bg_count;
        if (bg_count > 0) {
            if (ImGui::CollapsingHeader(ICON_FA_IMAGE " Background Layers",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Selectable("Auto (first visible)", st.selected_background_obj < 0))
                    st.selected_background_obj = -1;
                for (int i = 0; i < (int)st.scene.objects.size(); ++i) {
                    const auto& o = st.scene.objects[i];
                    if (o.background_name.empty()) continue;
                    char label[256];
                    snprintf(label, sizeof(label), "%s (%s)%s", o.name.c_str(),
                             o.background_name.c_str(), o.hidden ? " - hidden" : "");
                    if (ImGui::Selectable(label, st.selected_background_obj == i))
                        st.selected_background_obj = i;
                }
            }
        }

        // ── Scene lights: torches / fire / ambient point lights ──
        if (!st.scene.lights.empty()) {
            if (ImGui::CollapsingHeader(ICON_FA_WAND_MAGIC_SPARKLES " Scene Lights",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Render Light components", &st.scene_lights_enabled);
                ImGui::Checkbox("Show radius rings", &st.scene_light_debug);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Editor debug: draws a small emitter cross + influence-radius ring at every point light.\nThe ring is a marker only — it is NOT the light's actual visual size.");
                int ambient = 0, directional = 0, point = 0, overlay = 0;
                for (const auto& light : st.scene.lights) {
                    if (light.type == 1) ++ambient;
                    else if (light.type == 2) ++directional;
                    else if (light.type == 3) ++point;
                    else if (light.type == 4) ++overlay;
                }
                ImGui::TextDisabled("%d ambient, %d directional, %d point, %d overlay",
                                    ambient, directional, point, overlay);
            }
        }
        ImGui::Separator();

        // ── Object groups (SceneObjectGroup): select / hide bunches ──
        if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Groups",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (st.scene.parsed_groups.empty()) {
                ImGui::TextDisabled("No groups defined in this scene.");
            }
            for (int gi = 0; gi < (int)st.scene.parsed_groups.size(); ++gi) {
                const auto& grp = st.scene.parsed_groups[gi];
                const std::string gid = "##group" + std::to_string(gi);

                ImGui::PushID(gi);
                bool any_hidden_member = false;
                for (const auto& member : grp.members) {
                    for (const auto& o : st.scene.objects) {
                        if (o.name == member && o.hidden) { any_hidden_member = true; break; }
                    }
                    if (any_hidden_member) break;
                }

                ImGui::TextUnformatted(grp.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("  (%zu objects%s)", grp.members.size(),
                                    grp.hidden ? ", group hidden" :
                                    any_hidden_member ? ", members hidden" : "");

                // Select all members
                if (ImGui::SmallButton(ICON_FA_CIRCLE_CHECK " Select")) {
                    st.scene_selection.clear();
                    for (const auto& member : grp.members) {
                        for (int oi = 0; oi < (int)st.scene.objects.size(); ++oi) {
                            if (st.scene.objects[oi].name == member)
                                st.scene_selection.push_back(oi);
                        }
                    }
                    if (!st.scene_selection.empty())
                        select_scene_object(st, st.scene_selection.back());
                }

                // Hide / show every member
                const bool all_hidden = any_hidden_member;
                ImGui::SameLine();
                if (ImGui::SmallButton(all_hidden ? ICON_FA_EYE " Show" : ICON_FA_CIRCLE_XMARK " Hide")) {
                    snapshot_scene(st);
                    const bool new_hidden = !all_hidden;
                    for (auto& o : st.scene.objects) {
                        for (const auto& member : grp.members) {
                            if (o.name == member) { o.hidden = new_hidden; break; }
                        }
                    }
                    st.scene_dirty = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Toggle visibility of every object in this group");

                // Member list
                if (!grp.members.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_LIST " List"))
                        ImGui::OpenPopup(gid.c_str());
                    if (ImGui::BeginPopup(gid.c_str())) {
                        ImGui::TextDisabled("Members:");
                        for (const auto& member : grp.members) {
                            bool exists = false;
                            for (const auto& o : st.scene.objects)
                                if (o.name == member) { exists = true; break; }
                            if (exists)
                                ImGui::Text(ICON_FA_CUBE " %s", member.c_str());
                            else
                                ImGui::TextDisabled(ICON_FA_CIRCLE_INFO " %s (missing)", member.c_str());
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::PopID();
                ImGui::Separator();
            }
        }
        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "Selection");
        if (st.selected_object >= 0 && st.selected_object < static_cast<int>(st.scene.objects.size())) {
            const auto& selected = st.scene.objects[st.selected_object];
            ImGui::TextWrapped("%s", selected.name.empty() ? "(unnamed object)" : selected.name.c_str());
        } else {
            ImGui::TextDisabled("No object selected");
        }
        ImGui::Separator();

        // ── Selected object editor ───────────────────────────────────
        if (st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
            auto& obj = st.scene.objects[st.selected_object];

            ImGui::TextColored(ImVec4(0.40f, 0.70f, 1.00f, 1.0f),
                               ICON_FA_CUBE " Object #%d", st.selected_object);
            ImGui::Separator();

            // ── Identity fields ──────────────────────────────────────
            ImGui::PushItemWidth(-1.0f);

            ImGui::Text("Identifier");
            if (ImGui::IsItemActivated()) snapshot_scene(st);
            if (ImGui::InputText("##obj_name", st.scene_obj_name_buf,
                                 sizeof(st.scene_obj_name_buf))) {
                obj.name = st.scene_obj_name_buf;
                st.scene_dirty = true;
            }
            ImGui::Text("Template");
            if (ImGui::InputText("##obj_tmpl", st.scene_obj_template_buf,
                                 sizeof(st.scene_obj_template_buf))) {
                obj.template_name = st.scene_obj_template_buf;
                st.scene_dirty = true;
            }

            ImGui::PopItemWidth();

            // ── Transform ────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.85f, 1.0f), "Transform");
            ImGui::PushItemWidth(-1.0f);

            if (ImGui::IsItemActivated()) snapshot_scene(st);
            if (ImGui::DragFloat2("##pos_xy", &obj.pos_x, 0.5f, -99999.f, 99999.f, "XY: %.1f")) {
                av::scene_refresh(st.scene);
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position X / Y");

            if (ImGui::DragFloat("##depth", &obj.pos_z, 0.1f, -999.f, 999.f, "Depth: %.3f")) {
                av::scene_refresh(st.scene);
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Depth (world Z / parallax layer)");

            if (ImGui::DragFloat("##rot", &obj.rot_y, 0.01f, -6.2832f, 6.2832f, "Rot: %.4f rad")) {
                obj.rot_x = obj.rot_z = 0.0f;
                av::scene_refresh(st.scene);
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Y-axis rotation (radians)");

            if (ImGui::DragFloat("##scale", &obj.scale_x, 0.01f, 0.001f, 100.f, "Scale: %.3f")) {
                obj.scale_y = obj.scale_z = obj.scale_x;
                av::scene_refresh(st.scene);
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uniform scale");

            ImGui::PopItemWidth();

            // ── Flags ─────────────────────────────────────────────────
            ImGui::Spacing();
            bool hidden_tmp = obj.hidden;
            if (ImGui::Checkbox("Hidden", &hidden_tmp)) {
                obj.hidden = hidden_tmp;
                st.scene_dirty = true;
            }

            // ── Asset refs ────────────────────────────────────────────
            ImGui::Spacing();
            if (!obj.mesh_name.empty())
                ImGui::TextDisabled("Mesh: %s", obj.mesh_name.c_str());
            if (!obj.texture_name.empty())
                ImGui::TextDisabled("Tex:  %s", obj.texture_name.c_str());
            if (!obj.background_name.empty())
                ImGui::TextDisabled("BG:   %s", obj.background_name.c_str());

            // ── Components list ───────────────────────────────────────
            ImGui::Spacing();
            if (ImGui::CollapsingHeader(
                    (std::string(ICON_FA_PUZZLE_PIECE " Components (") +
                     std::to_string(obj.components.size()) + ")").c_str())) {
                static const std::vector<std::string> component_types = av::scene_component_types();
                if (!component_types.empty()) {
                    st.scene_component_type = std::clamp(st.scene_component_type, 0,
                                                         static_cast<int>(component_types.size()) - 1);
                    if (ImGui::BeginCombo("##component_type", component_types[st.scene_component_type].c_str())) {
                        for (int type = 0; type < static_cast<int>(component_types.size()); ++type) {
                            if (ImGui::Selectable(component_types[type].c_str(), type == st.scene_component_type))
                                st.scene_component_type = type;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_PLUS " Component")) {
                        snapshot_scene(st);
                        if (av::scene_add_component(st.scene, st.selected_object,
                                                    component_types[st.scene_component_type]))
                            st.scene_dirty = true;
                    }
                }

                int remove_component = -1;
                for (int ci = 0; ci < (int)obj.components.size(); ++ci) {
                    const auto& c = obj.components[ci];
                    ImGui::PushID(ci);
                    const std::string label = (c.type_name.empty() ? "(unnamed)" : c.type_name) +
                                              "##component";
                    if (ImGui::TreeNode(label.c_str())) {
                        ImGui::TextDisabled("Instance ID: %d  |  %zu bytes", c.type_id, c.raw_data.size());
                        const auto schema = av::g_schemas.find(c.type_name);
                        const std::vector<av::SceneComponentField> fields = av::scene_component_fields(c);
                        if (!fields.empty()) {
                            for (auto field : fields) {
                                ImGui::PushID(static_cast<int>(field.field_number * 100 + field.occurrence));
                                bool changed = false;
                                if (field.is_message) {
                                    ImGui::TextDisabled("%s: <%s> (%zu bytes)", field.name.c_str(),
                                        field.class_name.c_str(), field.bytes_value.size());
                                } else if (field.wire_type == proto::WIRE_VARINT) {
                                    int value = static_cast<int>(field.varint_value);
                                    if (ImGui::InputInt(field.name.c_str(), &value)) {
                                        field.varint_value = static_cast<uint64_t>(std::max(0, value));
                                        changed = true;
                                    }
                                } else if (field.wire_type == proto::WIRE_I32) {
                                    float value = field.float_value;
                                    if (ImGui::DragFloat(field.name.c_str(), &value, 0.01f)) {
                                        field.float_value = value;
                                        changed = true;
                                    }
                                } else if (field.wire_type == proto::WIRE_I64) {
                                    double value = field.double_value;
                                    if (ImGui::InputDouble(field.name.c_str(), &value)) {
                                        field.double_value = value;
                                        changed = true;
                                    }
                                } else if (field.bytes_value.find('\0') == std::string::npos &&
                                           field.bytes_value.size() < 4096) {
                                    char value[4096];
                                    snprintf(value, sizeof(value), "%s", field.bytes_value.c_str());
                                    if (ImGui::InputText(field.name.c_str(), value, sizeof(value))) {
                                        field.bytes_value = value;
                                        changed = true;
                                    }
                                } else {
                                    ImGui::TextDisabled("%s: <%zu bytes>", field.name.c_str(), field.bytes_value.size());
                                }
                                if (changed) {
                                    snapshot_scene(st);
                                    if (av::scene_set_component_field(obj.components[ci], field)) {
                                        av::scene_refresh(st.scene);
                                        st.scene_dirty = true;
                                    }
                                }
                                ImGui::PopID();
                            }
                        } else {
                            ImGui::TextDisabled("Component payload is empty or unavailable.");
                        }
                        if (ImGui::Button("Copy Component")) {
                            st.scene_component_clipboard = c;
                            st.scene_has_component_clipboard = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Remove Component")) remove_component = ci;
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (remove_component >= 0) {
                    snapshot_scene(st);
                    if (av::scene_remove_component(st.scene, st.selected_object,
                                                   static_cast<size_t>(remove_component)))
                        st.scene_dirty = true;
                }
                ImGui::BeginDisabled(!st.scene_has_component_clipboard);
                if (ImGui::Button("Paste Component")) {
                    snapshot_scene(st);
                    if (av::scene_paste_component(st.scene, st.selected_object,
                                                  st.scene_component_clipboard))
                        st.scene_dirty = true;
                }
                ImGui::EndDisabled();
            }

            // ── Ground Meshes inspector ───────────────────────────────
            if (!obj.ground_meshes.empty()) {
                if (ImGui::CollapsingHeader(
                        (std::string(ICON_FA_LAYER_GROUP " Ground Meshes (") +
                         std::to_string(obj.ground_meshes.size()) + ")").c_str(),
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ImGui::Button(st.gm_inline_edit ? ICON_FA_CHECK " 2D Edit Active"
                                                        : ICON_FA_PEN " Edit in Viewport")) {
                        if (st.selected_object >= 0) {
                            switch_scene_tab(st, 1);
                            gm_begin_inline_edit(st);   // pure 2D outline editing
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Regen Normals")) {
                        snapshot_scene(st);
                        for (auto& gm : obj.ground_meshes) swk::recompute_ground_mesh_geometry(gm);
                        reupload_object_ground_meshes(st, st.selected_object);
                        av::scene_mark_ground_mesh_dirty(st.scene, st.selected_object);
                        av::scene_refresh(st.scene);
                        st.scene_dirty = true;
                    }

                    for (int mi = 0; mi < (int)obj.ground_meshes.size(); ++mi) {
                        auto& gm = obj.ground_meshes[mi];
                        const char* tex = (mi < (int)obj.ground_mesh_textures.size() &&
                                           !obj.ground_mesh_textures[mi].empty())
                                              ? obj.ground_mesh_textures[mi].c_str() : "(none)";
                        const bool is_edited = (st.mesh_edit_mesh == mi);
                        if (ImGui::TreeNode((void*)(intptr_t)(mi + 1),
                                            "Mesh %d — %d verts, %d tris [%s]%s", mi,
                                            gm.num_vertices, gm.num_faces, tex,
                                            is_edited ? "  *" : "")) {
                            ImGui::TextDisabled("Bounds X: %.1f..%.1f  Y: %.1f..%.1f  Z: %.1f..%.1f",
                                gm.min_x, gm.max_x, gm.min_y, gm.max_y, gm.min_z, gm.max_z);
                            if (ImGui::SmallButton("Edit in 2D")) {
                                // The 2D outline drives the whole object's
                                // geometry, so this opens the inline editor.
                                if (st.selected_object >= 0) {
                                    switch_scene_tab(st, 1);
                                    gm_begin_inline_edit(st);
                                }
                            }

                            // Selected vertex position editor
                            if (st.mesh_edit_mesh == mi && st.mesh_edit_vertex >= 0 &&
                                st.mesh_edit_vertex < gm.num_vertices) {
                                const int v = st.mesh_edit_vertex;
                                ImGui::PushID(mi * 100000 + v);
                                if (ImGui::Button("Deselect Vertex")) st.mesh_edit_vertex = -1;
                                float px[3] = {gm.positions[v*3], gm.positions[v*3+1], gm.positions[v*3+2]};
                                const bool vtx_edited = ImGui::DragFloat3("Local Position", px, 0.5f);
                                if (ImGui::IsItemActivated()) snapshot_scene(st);
                                if (vtx_edited) {
                                    gm.positions[v*3] = px[0];
                                    gm.positions[v*3+1] = px[1];
                                    gm.positions[v*3+2] = px[2];
                                    swk::recompute_ground_mesh_geometry(gm);
                                    reupload_object_ground_meshes(st, st.selected_object);
                                    av::scene_mark_ground_mesh_dirty(st.scene, st.selected_object);
                                    av::scene_refresh(st.scene);
                                    st.scene_dirty = true;
                                }
                                ImGui::PopID();
                            }

                            // Compact vertex list (scrollable)
                            if (ImGui::TreeNode("Vertex List")) {
                                ImGui::BeginChild("##vtx_list", ImVec2(0, std::min(160.0f,
                                    (float)gm.num_vertices * 18.0f)), ImGuiChildFlags_Borders);
                                for (int vi = 0; vi < gm.num_vertices; ++vi) {
                                    char lbl[64];
                                    snprintf(lbl, sizeof(lbl), "V%d  (%.1f, %.1f, %.1f)##%d", vi,
                                             gm.positions[vi*3], gm.positions[vi*3+1],
                                             gm.positions[vi*3+2], vi);
                                    const bool sel = (st.mesh_edit_mesh == mi && st.mesh_edit_vertex == vi);
                                    if (ImGui::Selectable(lbl, sel)) {
                                        // Selecting a vertex only feeds the
                                        // numeric editor below (no 3D mode).
                                        st.mesh_edit_mesh = mi;
                                        st.mesh_edit_vertex = vi;
                                    }
                                }
                                ImGui::EndChild();
                                ImGui::TreePop();
                            }
                            ImGui::TreePop();
                        }
                    }
                }
            }

            // ── OnLoad script editor ──────────────────────────────────
            if (!obj.onload.empty()) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.30f, 1.0f),
                                   ICON_FA_CODE " OnLoad Script");

                // Decode Program source on first show / object switch
                if (st.scene_onload_buf[0] == '\0' && !obj.onload.empty()) {
                    try {
                        proto::Reader pr(obj.onload);
                        proto::Field  pf;
                        while (pr.read_field(pf)) {
                            if (pf.field_number == 1 && pf.wire_type == proto::WIRE_LEN) {
                                size_t copy_len = std::min(pf.bytes_val.size(),
                                                           sizeof(st.scene_onload_buf) - 1);
                                memcpy(st.scene_onload_buf, pf.bytes_val.data(), copy_len);
                                st.scene_onload_buf[copy_len] = '\0';
                                break;
                            }
                        }
                    } catch (...) {}
                    st.scene_onload_modified = false;
                }

                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f,0.10f,0.14f,1.0f));
                float avail_h = std::max(100.0f, ImGui::GetContentRegionAvail().y - 28.0f);
                if (ImGui::InputTextMultiline(
                        "##onload_src",
                        st.scene_onload_buf,
                        sizeof(st.scene_onload_buf),
                        ImVec2(-1.0f, avail_h),
                        ImGuiInputTextFlags_AllowTabInput)) {
                    st.scene_onload_modified = true;
                    st.scene_dirty = true;
                }
                ImGui::PopStyleColor();

                if (st.scene_onload_modified) {
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                                       ICON_FA_PENCIL " Script edited—save to apply");
                }
            }

        } else {
            // No object selected — show hint
            ImGui::Spacing();
            ImGui::TextDisabled(ICON_FA_HAND_POINTER " Click an object in the scene list");
            ImGui::TextDisabled("or in the 3D viewport to select it.");
        }
    } break;


    case PREVIEW_AUDIO: {
        auto& as = av::audio_get_state();
        ImGui::Text("Sample Rate: %d Hz", as.sample_rate);
        ImGui::Text("Channels:    %d", as.channels);
        ImGui::Text("Bit Depth:   %d", as.bits_per_sample);
        ImGui::Text("Duration:    %s", format_time(as.duration).c_str());
        ImGui::Text("Status:      %s",
                     as.playing ? (as.paused ? "Paused" : "Playing") : "Stopped");
    } break;

    default:
        break;
    }

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

// ============================================================================
// UI: Status bar (bottom)
// ============================================================================

static void draw_status_bar(ViewerState& st) {
    ImGui::Separator();
    ImGui::BeginChild("StatusBar", ImVec2(0, STATUS_BAR_H), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (ImGui::BeginTable("StatusBarTable", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoKeepColumnsVisible)) {
        ImGui::TableSetupColumn("StatusColumn", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("SceneColumn", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("ControlsColumn", ImGuiTableColumnFlags_WidthStretch, 0.35f);

        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", st.status_msg.empty() ? "Ready" : st.status_msg.c_str());

        ImGui::TableNextColumn();
        if (st.preview_type == PREVIEW_SCENE)
            ImGui::TextDisabled("%d/%zu visible  \xc2\xb7  %d proxies",
                                st.scene_rendered_objects, st.scene.objects.size(), st.scene_proxy_objects);

        ImGui::TableNextColumn();
        const char* controls = st.preview_type == PREVIEW_SCENE && st.scene_preview_tab == 1
            ? "LMB Select  \xc2\xb7  MMB Orbit  \xc2\xb7  Wheel Zoom"
            : "W Wire  \xc2\xb7  T Textures  \xc2\xb7  R Reset";
        float short_w = ImGui::CalcTextSize(controls).x;
        float cell_w = ImGui::GetContentRegionAvail().x;
        if (cell_w > short_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_w - short_w));
        }
        ImGui::TextDisabled("%s", controls);

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

// ============================================================================
// Apply Blender Theme (flat neutral gray dark style)
// ============================================================================

static void apply_blender_theme(float dpi_scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Modern rounded editor look (Blender 4.x / Godot inspired).
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;

    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 5);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;
    style.IndentSpacing     = 14.0f;
    style.WindowPadding     = ImVec2(10, 8);
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;

    ImVec4* c = style.Colors;

    // Deep-slate editor palette with a vibrant blue accent.
    c[ImGuiCol_WindowBg]             = ImVec4(0.105f, 0.115f, 0.135f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.080f, 0.090f, 0.110f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.075f, 0.085f, 0.105f, 0.97f);
    c[ImGuiCol_Border]               = ImVec4(0.20f, 0.23f, 0.28f, 0.65f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.155f, 0.170f, 0.205f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.200f, 0.225f, 0.275f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.250f, 0.285f, 0.350f, 1.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.145f, 0.160f, 0.190f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.190f, 0.215f, 0.270f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.145f, 0.160f, 0.190f, 0.55f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.135f, 0.150f, 0.180f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.060f, 0.070f, 0.090f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.300f, 0.335f, 0.395f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.370f, 0.410f, 0.480f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.450f, 0.490f, 0.570f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.320f, 0.570f, 0.980f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.340f, 0.380f, 0.450f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.360f, 0.610f, 0.990f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.175f, 0.195f, 0.235f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.240f, 0.275f, 0.335f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.300f, 0.540f, 0.900f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.180f, 0.205f, 0.250f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.250f, 0.460f, 0.760f, 0.45f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.290f, 0.520f, 0.860f, 0.80f);

    c[ImGuiCol_Separator]            = ImVec4(0.200f, 0.230f, 0.280f, 0.60f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.320f, 0.570f, 0.980f, 0.65f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.320f, 0.570f, 0.980f, 1.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.920f, 0.930f, 0.955f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.520f, 0.560f, 0.640f, 1.00f);

    // Tabs — crisp selected state with an accent overline.
    c[ImGuiCol_Tab]                  = ImVec4(0.140f, 0.155f, 0.190f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.260f, 0.300f, 0.370f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(0.240f, 0.420f, 0.680f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.140f, 0.155f, 0.190f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.190f, 0.215f, 0.270f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.360f, 0.610f, 0.990f, 1.00f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.130f, 0.145f, 0.175f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.160f, 0.180f, 0.220f, 1.00f);

    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.140f, 0.155f, 0.190f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.000f, 1.000f, 1.000f, 0.02f);

    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.290f, 0.520f, 0.860f, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.45f);
}

static void apply_ruby_cyber_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 6.0f;
    style.GrabRounding      = 5.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.TabRounding       = 6.0f;

    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(9, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.ScrollbarSize     = 12.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]             = ImVec4(0.035f, 0.045f, 0.065f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.050f, 0.063f, 0.088f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.045f, 0.057f, 0.080f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.16f, 0.22f, 0.31f, 0.72f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.075f, 0.098f, 0.140f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.105f, 0.140f, 0.195f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.130f, 0.180f, 0.245f, 1.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.045f, 0.058f, 0.080f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.065f, 0.088f, 0.120f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.045f, 0.058f, 0.080f, 0.65f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.045f, 0.058f, 0.080f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.08f, 0.08f, 0.12f, 0.30f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.16f, 0.24f, 0.38f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.22f, 0.32f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.28f, 0.40f, 0.62f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.32f, 0.80f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.22f, 0.32f, 0.50f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.095f, 0.135f, 0.190f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.125f, 0.205f, 0.280f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.095f, 0.135f, 0.190f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.32f, 0.80f, 1.00f, 0.22f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.32f, 0.80f, 1.00f, 0.42f);

    c[ImGuiCol_Separator]            = ImVec4(0.16f, 0.24f, 0.35f, 0.80f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.28f, 0.40f, 0.62f, 0.80f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.94f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.49f, 0.57f, 0.67f, 1.00f);
}

static void apply_selected_theme(int theme_idx, float font_scale) {
    ImGuiIO& io = ImGui::GetIO();
    // The UI scale slider multiplies on top of the OS/display DPI scale rather
    // than overriding it, so the widget stays legible on HiDPI monitors.
    io.FontGlobalScale = font_scale / std::max(1.0f, g_state.display_scale);

    g_theme = theme_idx == 1
        ? RubyTheme{
            ImVec4(0.885f,0.90f,0.915f,1), ImVec4(0.955f,0.962f,0.972f,1), ImVec4(0.925f,0.935f,0.950f,1),
            ImVec4(0.83f,0.855f,0.885f,1), ImVec4(0.77f,0.805f,0.85f,1), ImVec4(0.62f,0.70f,0.80f,1),
            ImVec4(0.60f,0.63f,0.68f,1), ImVec4(0.10f,0.12f,0.15f,1), ImVec4(0.42f,0.46f,0.52f,1),
            ImVec4(0.08f,0.42f,0.76f,1), ImVec4(0.78f,0.48f,0.06f,1), ImVec4(0.78f,0.18f,0.20f,1),
            ImVec4(0.08f,0.55f,0.30f,1), ImVec4(0.15f,0.48f,0.82f,0.30f), false}
        : RubyTheme{
            ImVec4(0.075f,0.082f,0.095f,1), ImVec4(0.105f,0.115f,0.132f,1), ImVec4(0.13f,0.14f,0.16f,1),
            ImVec4(0.16f,0.175f,0.20f,1), ImVec4(0.21f,0.235f,0.27f,1), ImVec4(0.25f,0.32f,0.42f,1),
            ImVec4(0.25f,0.27f,0.31f,1), ImVec4(0.91f,0.92f,0.94f,1), ImVec4(0.55f,0.59f,0.65f,1),
            ImVec4(0.25f,0.58f,0.92f,1), ImVec4(0.95f,0.64f,0.20f,1), ImVec4(0.94f,0.32f,0.34f,1),
            ImVec4(0.34f,0.76f,0.48f,1), ImVec4(0.25f,0.58f,0.92f,0.42f), true};

    const RubyTheme& T = g_theme;
    const bool dark = T.dark;

    // Standardized geometry for both themes — compact, professional, not
    // "rounded-everything".
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(10, 8);
    style.FramePadding      = ImVec2(7, 4);
    style.ItemSpacing       = ImVec2(7, 4);
    style.ItemInnerSpacing  = ImVec2(5, 4);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;
    style.CellPadding       = ImVec2(6, 4);
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;
    style.WindowBorderSize  = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;

    // Derived shades from the semantic palette (theme-safe by construction).
    const ImVec4 border       = T.border;
    const ImVec4 accent       = T.accent;
    const ImVec4 accent_hover = dark ? th_mix(accent, T.text, 0.18f) : th_mix(accent, ImVec4(0,0,0,1), 0.25f);
    const ImVec4 surface_hot  = dark ? th_mix(T.surface_active, accent, 0.45f) : th_mix(T.surface_active, accent, 0.55f);
    const ImVec4 tab_active   = dark ? th_mix(T.surface_active, accent, 0.35f) : th_mix(T.surface_active, accent, 0.30f);
    const ImVec4 frame_hot    = th_mix(T.surface, T.surface_hover, 0.5f);
    const ImVec4 popup_bg     = dark ? th_mix(T.panel, T.surface, 0.18f) : th_mix(T.panel, ImVec4(1,1,1,1), 0.3f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = T.text;
    c[ImGuiCol_TextDisabled]         = T.text_muted;
    c[ImGuiCol_WindowBg]             = T.background;
    c[ImGuiCol_ChildBg]              = T.panel;
    c[ImGuiCol_PopupBg]              = popup_bg;
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = T.surface;
    c[ImGuiCol_FrameBgHovered]       = T.surface_hover;
    c[ImGuiCol_FrameBgActive]        = T.surface_active;
    c[ImGuiCol_TitleBg]              = T.panel_alt;
    c[ImGuiCol_TitleBgActive]        = dark ? th_mix(T.surface_active, accent, 0.30f) : th_mix(T.surface_active, accent, 0.25f);
    c[ImGuiCol_TitleBgCollapsed]     = T.panel_alt;
    c[ImGuiCol_MenuBarBg]            = T.panel_alt;
    c[ImGuiCol_ScrollbarBg]          = th_alpha(T.background, dark ? 0.55f : 0.85f);
    c[ImGuiCol_ScrollbarGrab]        = T.surface;
    c[ImGuiCol_ScrollbarGrabHovered] = T.surface_hover;
    c[ImGuiCol_ScrollbarGrabActive]  = T.surface_active;
    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = T.surface_hover;
    c[ImGuiCol_SliderGrabActive]     = accent;
    c[ImGuiCol_Button]               = T.surface;
    c[ImGuiCol_ButtonHovered]        = T.surface_hover;
    c[ImGuiCol_ButtonActive]         = surface_hot;
    c[ImGuiCol_Header]               = T.surface;
    c[ImGuiCol_HeaderHovered]        = T.surface_hover;
    c[ImGuiCol_HeaderActive]         = T.surface_active;
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = accent_hover;
    c[ImGuiCol_SeparatorActive]      = accent;
    c[ImGuiCol_ResizeGrip]           = th_alpha(border, 0.30f);
    c[ImGuiCol_ResizeGripHovered]    = th_alpha(accent, 0.70f);
    c[ImGuiCol_ResizeGripActive]     = accent;
    c[ImGuiCol_Tab]                  = T.panel_alt;
    c[ImGuiCol_TabHovered]           = T.surface_hover;
    c[ImGuiCol_TabActive]            = tab_active;
    c[ImGuiCol_TabUnfocused]         = T.panel_alt;
    c[ImGuiCol_TabUnfocusedActive]   = T.surface;
    c[ImGuiCol_TabSelectedOverline]  = accent;
    c[ImGuiCol_TabDimmed]            = T.panel_alt;
    c[ImGuiCol_TabDimmedSelected]    = T.surface;
    c[ImGuiCol_TableHeaderBg]        = T.panel_alt;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = th_alpha(border, 0.5f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = dark ? ImVec4(1, 1, 1, 0.025f) : ImVec4(0, 0, 0, 0.025f);
    c[ImGuiCol_TextSelectedBg]       = T.selection;
    c[ImGuiCol_DragDropTarget]       = accent;
    c[ImGuiCol_NavCursor]            = accent;
    c[ImGuiCol_NavWindowingHighlight]= accent;
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0, 0, 0, 0.18f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, dark ? 0.45f : 0.35f);
}

static fs::path workspace_layout_path() {
    return fs::path(get_user_data_dir()) / "ruby_workspace.ini";
}

static void reset_workspace_layout(ViewerState& st) {
    st.show_asset_browser = true;
    st.show_inspector = true;
    st.show_bottom_panel = false;
    st.asset_browser_width = LEFT_PANEL_W;
    st.inspector_width = RIGHT_PANEL_W;
    st.bottom_panel_height = 220.0f;
    st.workspace_layout_dirty = true;
    st.status_msg = "Workspace layout reset";
}

static bool workspace_layout_values_valid(float browser, float inspector, float bottom) {
    return std::isfinite(browser) && std::isfinite(inspector) && std::isfinite(bottom) &&
           browser >= MIN_BROWSER_W && browser <= 720.0f &&
           inspector >= MIN_INSPECTOR_W && inspector <= 720.0f &&
           bottom >= 120.0f && bottom <= 720.0f;
}

static void save_workspace_layout(const ViewerState& st) {
    std::error_code ec;
    fs::create_directories(workspace_layout_path().parent_path(), ec);
    std::ofstream out(workspace_layout_path(), std::ios::trunc);
    if (!out) return;
    out << "version=1\n"
        << "browser_visible=" << (st.show_asset_browser ? 1 : 0) << "\n"
        << "inspector_visible=" << (st.show_inspector ? 1 : 0) << "\n"
        << "bottom_visible=" << (st.show_bottom_panel ? 1 : 0) << "\n"
        << "browser_width=" << st.asset_browser_width << "\n"
        << "inspector_width=" << st.inspector_width << "\n"
        << "bottom_height=" << st.bottom_panel_height << "\n";
}

static void load_workspace_layout(ViewerState& st) {
    std::ifstream in(workspace_layout_path());
    if (!in) return;

    int version = 0;
    int browser_visible = 1, inspector_visible = 1, bottom_visible = 0;
    float browser = LEFT_PANEL_W, inspector = RIGHT_PANEL_W, bottom = 220.0f;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "version") version = std::stoi(value);
            else if (key == "browser_visible") browser_visible = std::stoi(value);
            else if (key == "inspector_visible") inspector_visible = std::stoi(value);
            else if (key == "bottom_visible") bottom_visible = std::stoi(value);
            else if (key == "browser_width") browser = std::stof(value);
            else if (key == "inspector_width") inspector = std::stof(value);
            else if (key == "bottom_height") bottom = std::stof(value);
        } catch (...) {
            version = 0;
            break;
        }
    }

    const bool flags_valid = (browser_visible == 0 || browser_visible == 1) &&
                             (inspector_visible == 0 || inspector_visible == 1) &&
                             (bottom_visible == 0 || bottom_visible == 1);
    if (version != 1 || !flags_valid || !workspace_layout_values_valid(browser, inspector, bottom)) {
        reset_workspace_layout(st);
        st.status_msg = "Invalid saved workspace was reset";
        return;
    }

    st.show_asset_browser = browser_visible != 0;
    st.show_inspector = inspector_visible != 0;
    st.show_bottom_panel = bottom_visible != 0;
    st.asset_browser_width = browser;
    st.inspector_width = inspector;
    st.bottom_panel_height = bottom;
}

// ============================================================================
// Adaptive-render-quality persistence
//
// The viewport's render scale (HiDPI tier) is normally chosen adaptively from
// *measured* frame time — not guessed from the GPU vendor string. Once a tier
// settles it is written here, so the next launch starts near the sweet spot
// instead of re-probing the full ladder from the top.
// ============================================================================
static const char* const kRenderScaleLabels[] = { "1.0x (native)", "1.5x", "2.0x", "3.0x", "4.0x" };
static const float    kRenderScaleVals[] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
static constexpr int  kRenderTierCount = 5;

static fs::path quality_config_path() {
    return fs::path(expand_home("~/.local/share/swordigo-desktop")) / "ruby_quality.ini";
}

static void save_quality_config(const ViewerState& st) {
    std::error_code ec;
    fs::create_directories(quality_config_path().parent_path(), ec);
    std::ofstream out(quality_config_path(), std::ios::trunc);
    if (!out) return;
    out << "version=1\n"
        << "render_tier=" << st.perf_tier << "\n"
        << "render_auto=" << (st.perf_auto ? 1 : 0) << "\n";
}

static void load_quality_config(ViewerState& st) {
    std::ifstream in(quality_config_path());
    if (!in) return;
    int version = 0, tier = 3, auto_mode = 1;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "version") version = std::stoi(value);
            else if (key == "render_tier") tier = std::stoi(value);
            else if (key == "render_auto") auto_mode = std::stoi(value);
        } catch (...) { version = 0; break; }
    }
    if (version != 1) return;
    if (tier >= 0 && tier < kRenderTierCount) {
        st.perf_tier = tier;
        st.render_scale = kRenderScaleVals[tier];
    }
    st.perf_auto = (auto_mode != 0);
}

// ============================================================================
// Blender round-trip bridge (Stage 4)
//
// Protocol (all under ~/.local/share/swordigo-desktop/blender/):
//   in/model.glb      Ruby -> Blender: POD exported to glTF/GLB
//   in/request.json   Ruby -> Blender: {"run": N, "source_pod": "...", "format": "glb"}
//   out/model.glb     Blender -> Ruby: scene re-exported by the save_post handler
//   out/done.json     Blender -> Ruby: {"run": N, "status": "ok" | "error", ...}
//   ext/              a copy of the Swordigo Round-Trip Blender extension
//   swordigo_roundtrip.zip  built package used for install-file
// ============================================================================

static std::string blender_staging_root() {
    return expand_home("~/.local/share/swordigo-desktop/blender");
}

static std::string blender_ext_repo_dir() {
    // Canonical repo location of the extension source.
    const char* env = getenv("SWORDIGO_BLENDER_EXT_DIR");
    if (env && env[0]) return env;
    fs::path cand = fs::current_path() / "src/tools/blender_ext";
    if (fs::exists(cand / "blender_manifest.toml")) return cand.string();
    cand = fs::current_path() / "blender_ext";
    if (fs::exists(cand / "blender_manifest.toml")) return cand.string();
    return "";
}

// Run a Blender headless subcommand (extension build / install-file). Returns
// true if the command exited 0.
#if defined(_WIN32)
static bool blender_run_cli(const std::vector<std::string>& args, std::string* err_out) {
    (void)args;
    if (err_out) *err_out = "blender CLI unsupported on Windows";
    return false;
}
#else
static bool blender_run_cli(const std::vector<std::string>& args, std::string* err_out) {
    pid_t pid = fork();
    if (pid < 0) { if (err_out) *err_out = "fork failed"; return false; }
    if (pid == 0) {
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok && err_out) {
        *err_out = "blender exited " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return ok;
}
#endif

// Stage the extension sources into the staging dir (if missing or stale) and
// install it into Blender. Returns true once Blender knows the addon.
static bool blender_install_extension(ViewerState& st, std::string* err_out) {
    std::string repo = blender_ext_repo_dir();
    if (repo.empty()) {
        if (err_out) *err_out = "extension source dir not found (set SWORDIGO_BLENDER_EXT_DIR)";
        return false;
    }
    if (!fs::exists(st.blender_path)) {
        if (err_out) *err_out = "blender executable not found at: " + std::string(st.blender_path);
        return false;
    }
    fs::path staging = blender_staging_root();
    fs::create_directories(staging / "in");
    fs::create_directories(staging / "out");
    fs::create_directories(staging / "ext");

    // Refresh the staged copy only when the repo manifest is newer.
    fs::path repo_manifest = fs::path(repo) / "blender_manifest.toml";
    fs::path staged_manifest = staging / "ext/blender_manifest.toml";
    std::error_code ec;
    bool stale = !fs::exists(staged_manifest, ec);
    if (!stale) {
        auto t_repo = fs::last_write_time(repo_manifest, ec);
        auto t_stage = fs::last_write_time(staged_manifest, ec);
        stale = ec || t_stage < t_repo;
    }
    if (stale) {
        fs::remove_all(staging / "ext", ec);
        fs::create_directories(staging / "ext");
        for (auto& de : fs::directory_iterator(repo, ec))
            fs::copy_file(de.path(), staging / "ext" / de.path().filename(), fs::copy_options::overwrite_existing, ec);
    }

    // Build a .zip and install into the user_default repo, enabling it.
    fs::path zip = staging / "swordigo_roundtrip.zip";
    fs::remove(zip);
    std::vector<std::string> build = {
        st.blender_path, "--command", "extension", "build",
        "--source-dir", (staging / "ext").string(),
        "--output-filepath", zip.string()
    };
    if (!blender_run_cli(build, err_out)) return false;
    std::vector<std::string> install = {
        st.blender_path, "--command", "extension", "install-file",
        "-r", "user_default", "-e", zip.string()
    };
    if (!blender_run_cli(install, err_out)) return false;
    return true;
}

// Launch Blender's GUI with the staging env var set. Detached (no wait).
// Launch Blender's GUI with the staging env var set. Detached (no wait).
#if defined(_WIN32)
static bool blender_launch_gui(ViewerState& st, std::string* err_out) {
    (void)st;
    if (err_out) *err_out = "Blender GUI launch unsupported on Windows";
    return false;
}
#else
static bool blender_launch_gui(ViewerState& st, std::string* err_out) {
    pid_t pid = fork();
    if (pid < 0) { if (err_out) *err_out = "fork failed"; return false; }
    if (pid == 0) {
        setenv("SWORDIGO_BLENDER_STAGING", blender_staging_root().c_str(), 1);
        std::string launch_path = st.blender_path;
        char* const argv[] = { const_cast<char*>(launch_path.c_str()), nullptr };
        execvp(argv[0], argv);
        _exit(127);
    }
    // Parent: do not wait — Blender stays open while the user edits.
    return true;
}
#endif

// Decode a single texture file to RGBA for GLB embedding. Handles .pvr,
// .tex.png (gzipped), and plain .png / .pvr.png variants.
static bool blender_texture_to_rgba(const std::string& path,
                                    std::vector<uint8_t>& rgba, int& w, int& h) {
    std::string low = path;
    for (auto& c : low) c = (char)tolower((unsigned char)c);

    if (low.size() >= 4 && low.substr(low.size() - 4) == ".pvr" && low.find(".tex.png") == std::string::npos) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data((size_t)n);
        if (n > 0 && fread(data.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return false; }
        fclose(f);
        return pvr_decode_to_rgba(data.data(), data.size(), rgba, w, h);
    }

    if (low.size() >= 8 && low.substr(low.size() - 8) == ".tex.png") {
        // Gzipped raw texture: 12-byte header (img_type, width, height) + pixels.
        gzFile gz = gzopen(path.c_str(), "rb");
        if (!gz) return false;
        uint32_t header[3];
        if (gzread(gz, header, 12) != 12) { gzclose(gz); return false; }
        uint32_t img_type = header[0]; int W = (int)header[1]; int H = (int)header[2];
        int bpp = (img_type == 1) ? 4 : 2;
        std::vector<uint8_t> raw((size_t)W * H * bpp);
        int got = gzread(gz, raw.data(), (unsigned int)raw.size());
        gzclose(gz);
        if (got != (int)raw.size()) return false;
        rgba.assign((size_t)W * H * 4, 0);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            if (img_type == 1) {
                rgba[i*4+0] = raw[i*4+0]; rgba[i*4+1] = raw[i*4+1];
                rgba[i*4+2] = raw[i*4+2]; rgba[i*4+3] = raw[i*4+3];
            } else if (img_type == 3) { // RGBA4444
                uint16_t v = (uint16_t)((raw[i*2+1] << 8) | raw[i*2]);
                rgba[i*4+0] = (uint8_t)(((v >> 12) & 0xF) << 4);
                rgba[i*4+1] = (uint8_t)(((v >> 8) & 0xF) << 4);
                rgba[i*4+2] = (uint8_t)(((v >> 4) & 0xF) << 4);
                rgba[i*4+3] = (uint8_t)((v & 0xF) << 4);
            } else if (img_type == 5) { // RGB565
                uint16_t v = (uint16_t)((raw[i*2+1] << 8) | raw[i*2]);
                rgba[i*4+0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
                rgba[i*4+1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                rgba[i*4+2] = (uint8_t)((v & 0x1F) << 3);
                rgba[i*4+3] = 255;
            }
        }
        w = W; h = H;
        return true;
    }

    // Gzipped raw .tex texture: identical layout to .tex.png.
    if (low.size() >= 4 && low.substr(low.size() - 4) == ".tex") {
        gzFile gz = gzopen(path.c_str(), "rb");
        if (!gz) return false;
        uint32_t header[3];
        if (gzread(gz, header, 12) != 12) { gzclose(gz); return false; }
        uint32_t img_type = header[0]; int W = (int)header[1]; int H = (int)header[2];
        int bpp = (img_type == 1) ? 4 : 2;
        std::vector<uint8_t> raw((size_t)W * H * bpp);
        int got = gzread(gz, raw.data(), (unsigned int)raw.size());
        gzclose(gz);
        if (got != (int)raw.size()) return false;
        rgba.assign((size_t)W * H * 4, 0);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            if (img_type == 1) {
                rgba[i*4+0] = raw[i*4+0]; rgba[i*4+1] = raw[i*4+1];
                rgba[i*4+2] = raw[i*4+2]; rgba[i*4+3] = raw[i*4+3];
            } else if (img_type == 3) { // RGBA4444
                uint16_t v = (uint16_t)((raw[i*2+1] << 8) | raw[i*2]);
                rgba[i*4+0] = (uint8_t)(((v >> 12) & 0xF) << 4);
                rgba[i*4+1] = (uint8_t)(((v >> 8) & 0xF) << 4);
                rgba[i*4+2] = (uint8_t)(((v >> 4) & 0xF) << 4);
                rgba[i*4+3] = (uint8_t)((v & 0xF) << 4);
            } else if (img_type == 5) { // RGB565
                uint16_t v = (uint16_t)((raw[i*2+1] << 8) | raw[i*2]);
                rgba[i*4+0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
                rgba[i*4+1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                rgba[i*4+2] = (uint8_t)((v & 0x1F) << 3);
                rgba[i*4+3] = 255;
            }
        }
        w = W; h = H;
        return true;
    }

    // Plain image (png/pvr.png/jpg) via SDL.
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) return false;
    SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!conv) return false;
    w = conv->w; h = conv->h;
    const uint8_t* px = (const uint8_t*)conv->pixels;
    rgba.assign((size_t)w * h * 4, 0);
    memcpy(rgba.data(), px, (size_t)w * h * 4);
    SDL_DestroySurface(conv);
    return true;
}

// ============================================================================
// Texture image editor — CPU-side RGBA8 editing for the texture preview.
// Tools: brush, eraser, paint-bucket fill, eyedropper, crop; bake operations
// (rotate/flip), single-step undo, and save as PNG / .tex (gzip RGBA8888).
// The buffer is decoded once on file-open (tex_editor_load) and re-uploaded
// to the preview GL texture on every modification.
// ============================================================================

// True when the opened file is a Swordigo texture container (.tex/.tex.png/.pvr)
// whose pixels are stored flipped relative to how the game displays them.
static bool tex_edit_is_container(const ViewerState& st) {
    return st.tex_edit_ext == ".tex" || st.tex_edit_ext == ".tex.png" ||
           st.tex_edit_ext == ".pvr";
}

// Rotate the pixel buffer 180° (horizontal + vertical flip in one pass).
// Toggles between file space and display space for flipped containers.
static void tex_edit_flip_buffer_hv(std::vector<uint8_t>& px, int w, int h) {
    if (w <= 0 || h <= 0 || px.size() < (size_t)w * (size_t)h * 4) return;
    std::vector<uint8_t> out(px.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            memcpy(&out[((size_t)(h - 1 - y) * w + (size_t)(w - 1 - x)) * 4],
                   &px[((size_t)y * w + (size_t)x) * 4], 4);
    px.swap(out);
}

static void tex_editor_clear(ViewerState& st) {
    st.tex_edit_pixels.clear();
    st.tex_edit_w = st.tex_edit_h = 0;
    st.tex_edit_valid = false;
    st.tex_edit_dirty = false;
    st.tex_edit_display_space = false;
    st.tex_edit_enabled = false;
    st.tex_edit_src.clear();
    st.tex_edit_ext.clear();
    st.tex_edit_crop_ready = false;
    st.tex_edit_crop_active = false;
    st.tex_edit_undo.clear();
    st.tex_edit_last_x = st.tex_edit_last_y = -1;
    st.tex_edit_last_upload = 0.0;
}

static void tex_editor_load(ViewerState& st, const std::string& path) {
    tex_editor_clear(st);
    st.tex_edit_src = path;
    std::string low = path;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    st.tex_edit_ext = (low.size() >= 8 && low.substr(low.size() - 8) == ".tex.png")
                          ? std::string(".tex.png")
                          : fs::path(path).extension().string();
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (blender_texture_to_rgba(path, rgba, w, h) && w > 0 && h > 0 &&
        (int)rgba.size() == w * h * 4) {
        st.tex_edit_pixels = std::move(rgba);
        st.tex_edit_w = w;
        st.tex_edit_h = h;
        st.tex_edit_valid = true;
        // While edit mode is active, pre-flip flipped containers to display
        // space so painting/cropping matches what is on screen. This also
        // covers "Reload Original" invoked from within edit mode.
        if (st.tex_edit_enabled && tex_edit_is_container(st)) {
            tex_edit_flip_buffer_hv(st.tex_edit_pixels, w, h);
            st.tex_edit_display_space = true;
        }
        snprintf(st.tex_edit_save_path, sizeof(st.tex_edit_save_path), "%s.png",
                 path.c_str());
    }
}

static void tex_editor_upload(ViewerState& st) {
    if (!st.tex_edit_valid || st.tex_edit_pixels.empty() || !st.preview_tex) return;
    glBindTexture(GL_TEXTURE_2D, st.preview_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, st.tex_edit_w, st.tex_edit_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, st.tex_edit_pixels.data());
}

static void tex_edit_snapshot(ViewerState& st) { st.tex_edit_undo = st.tex_edit_pixels; }

static void tex_edit_undo(ViewerState& st) {
    if (st.tex_edit_undo.empty()) return;
    st.tex_edit_pixels = st.tex_edit_undo;
    st.tex_edit_undo.clear();
    st.tex_edit_dirty = true;
    tex_editor_upload(st);
}

// Paint a soft (alpha-blended) or erasing circle at pixel (x, y).
static void tex_edit_brush_at(ViewerState& st, int x, int y, bool erase) {
    if (!st.tex_edit_valid) return;
    const int W = st.tex_edit_w, H = st.tex_edit_h;
    const float r = std::max(0.5f, st.tex_edit_size * 0.5f);
    const int x0 = (int)std::floor(x - r), x1 = (int)std::ceil(x + r);
    const int y0 = (int)std::floor(y - r), y1 = (int)std::ceil(y + r);
    const uint8_t col[4] = {
        (uint8_t)(st.tex_edit_color[0] * 255.0f),
        (uint8_t)(st.tex_edit_color[1] * 255.0f),
        (uint8_t)(st.tex_edit_color[2] * 255.0f),
        (uint8_t)(st.tex_edit_color[3] * 255.0f)};
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
            const float dx = (float)(cx - x), dy = (float)(cy - y);
            if (dx * dx + dy * dy > r * r) continue;
            uint8_t* p = &st.tex_edit_pixels[((size_t)cy * W + cx) * 4];
            if (erase) {
                p[0] = p[1] = p[2] = 0; p[3] = 0;
            } else {
                const float a = col[3] / 255.0f;
                p[0] = (uint8_t)(p[0] * (1.0f - a) + col[0] * a);
                p[1] = (uint8_t)(p[1] * (1.0f - a) + col[1] * a);
                p[2] = (uint8_t)(p[2] * (1.0f - a) + col[2] * a);
                p[3] = (uint8_t)(p[3] * (1.0f - a) + col[3] * a);
            }
        }
    }
    st.tex_edit_dirty = true;
}

// Bresenham line between two pixels so fast drags paint continuous strokes.
static void tex_edit_brush_line(ViewerState& st, int x0, int y0, int x1, int y1, bool erase) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        tex_edit_brush_at(st, x0, y0, erase);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// 4-connected flood fill (paint bucket) from (x, y).
static void tex_edit_flood_fill(ViewerState& st, int x, int y) {
    if (!st.tex_edit_valid) return;
    const int W = st.tex_edit_w, H = st.tex_edit_h;
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t* px = st.tex_edit_pixels.data();
    const uint8_t* start = &px[((size_t)y * W + x) * 4];
    const uint8_t tc[4] = {start[0], start[1], start[2], start[3]};
    const uint8_t nc[4] = {
        (uint8_t)(st.tex_edit_color[0] * 255.0f),
        (uint8_t)(st.tex_edit_color[1] * 255.0f),
        (uint8_t)(st.tex_edit_color[2] * 255.0f),
        (uint8_t)(st.tex_edit_color[3] * 255.0f)};
    if (tc[0] == nc[0] && tc[1] == nc[1] && tc[2] == nc[2] && tc[3] == nc[3]) return;
    std::vector<int> stack;
    stack.push_back(x); stack.push_back(y);
    while (!stack.empty()) {
        const int cy = stack.back(); stack.pop_back();
        const int cx = stack.back(); stack.pop_back();
        if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
        uint8_t* p = &px[((size_t)cy * W + cx) * 4];
        if (p[0] != tc[0] || p[1] != tc[1] || p[2] != tc[2] || p[3] != tc[3]) continue;
        p[0] = nc[0]; p[1] = nc[1]; p[2] = nc[2]; p[3] = nc[3];
        stack.push_back(cx + 1); stack.push_back(cy);
        stack.push_back(cx - 1); stack.push_back(cy);
        stack.push_back(cx);     stack.push_back(cy + 1);
        stack.push_back(cx);     stack.push_back(cy - 1);
    }
    st.tex_edit_dirty = true;
}

// Bake a 90°·steps rotation (1 = 90° CW, 2 = 180°, 3 = 90° CCW) into the buffer.
static void tex_edit_bake_rotate(ViewerState& st, int steps_cw) {
    if (!st.tex_edit_valid) return;
    const int W = st.tex_edit_w, H = st.tex_edit_h;
    steps_cw = ((steps_cw % 4) + 4) % 4;
    if (steps_cw == 0) return;
    std::vector<uint8_t> out;
    if (steps_cw == 2) {
        out.resize(st.tex_edit_pixels.size());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                memcpy(&out[((size_t)(H - 1 - y) * W + (W - 1 - x)) * 4],
                       &st.tex_edit_pixels[((size_t)y * W + x) * 4], 4);
    } else {
        const int NW = H, NH = W;
        out.assign((size_t)NW * NH * 4, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t* s = &st.tex_edit_pixels[((size_t)y * W + x) * 4];
                int nx, ny;
                if (steps_cw == 1) { nx = H - 1 - y; ny = x; }        // 90° CW
                else               { nx = y;          ny = W - 1 - x; } // 90° CCW
                memcpy(&out[((size_t)ny * NW + nx) * 4], s, 4);
            }
        st.tex_edit_w = NW;
        st.tex_edit_h = NH;
    }
    st.tex_edit_pixels = std::move(out);
    st.tex_edit_dirty = true;
    st.tex_w = st.tex_edit_w;
    st.tex_h = st.tex_edit_h;
    st.tex_zoom = 1.0f;
    st.tex_reset_required = true;
    tex_editor_upload(st);
}

static void tex_edit_bake_flip(ViewerState& st, bool horizontal) {
    if (!st.tex_edit_valid) return;
    const int W = st.tex_edit_w, H = st.tex_edit_h;
    auto swap_px = [&](int a, int b) {
        uint8_t* p = st.tex_edit_pixels.data();
        for (int c = 0; c < 4; ++c) std::swap(p[a + c], p[b + c]);
    };
    if (horizontal) {
        // Mirror columns within each row (half-width, every row once).
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W / 2; ++x)
                swap_px((y * W + x) * 4, (y * W + (W - 1 - x)) * 4);
    } else {
        // Mirror rows over the FULL width, visiting each row pair once.
        for (int y = 0; y < H / 2; ++y)
            for (int x = 0; x < W; ++x)
                swap_px((y * W + x) * 4, ((H - 1 - y) * W + x) * 4);
    }
    st.tex_edit_dirty = true;
    tex_editor_upload(st);
}

static void tex_edit_apply_crop(ViewerState& st) {
    if (!st.tex_edit_valid || !st.tex_edit_crop_ready) return;
    int x0 = (int)std::round(std::min(st.tex_edit_crop_x0, st.tex_edit_crop_x1));
    int x1 = (int)std::round(std::max(st.tex_edit_crop_x0, st.tex_edit_crop_x1));
    int y0 = (int)std::round(std::min(st.tex_edit_crop_y0, st.tex_edit_crop_y1));
    int y1 = (int)std::round(std::max(st.tex_edit_crop_y0, st.tex_edit_crop_y1));
    x0 = std::max(0, std::min(st.tex_edit_w - 1, x0));
    y0 = std::max(0, std::min(st.tex_edit_h - 1, y0));
    x1 = std::max(0, std::min(st.tex_edit_w - 1, x1));
    y1 = std::max(0, std::min(st.tex_edit_h - 1, y1));
    if (x1 <= x0 || y1 <= y0) return;
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    std::vector<uint8_t> out((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        memcpy(&out[(size_t)y * W * 4],
               &st.tex_edit_pixels[((size_t)(y0 + y) * st.tex_edit_w + x0) * 4],
               (size_t)W * 4);
    st.tex_edit_pixels = std::move(out);
    st.tex_edit_w = W;
    st.tex_edit_h = H;
    st.tex_w = W;
    st.tex_h = H;
    st.tex_edit_crop_ready = false;
    st.tex_edit_dirty = true;
    st.tex_reset_required = true;
    tex_editor_upload(st);
}

static bool tex_edit_save_png(ViewerState& st, const std::string& path) {
    if (!st.tex_edit_valid) return false;
    // PNGs are written in display space: what the user sees is what they get.
    std::vector<uint8_t> buf = st.tex_edit_pixels;
    if (!st.tex_edit_display_space && tex_edit_is_container(st))
        tex_edit_flip_buffer_hv(buf, st.tex_edit_w, st.tex_edit_h);
    return stbi_write_png(path.c_str(), st.tex_edit_w, st.tex_edit_h, 4,
                          buf.data(), st.tex_edit_w * 4) != 0;
}

// Save the editor buffer as a raw .tex container (gzip, img_type 1 = RGBA8888).
static bool tex_edit_save_tex(ViewerState& st, const std::string& path) {
    if (!st.tex_edit_valid) return false;
    // .tex containers are written in file space (flipped back from display
    // space) so the saved file stays compatible with the game engine.
    std::vector<uint8_t> buf = st.tex_edit_pixels;
    if (st.tex_edit_display_space)
        tex_edit_flip_buffer_hv(buf, st.tex_edit_w, st.tex_edit_h);
    gzFile gz = gzopen(path.c_str(), "wb");
    if (!gz) return false;
    const uint32_t header[3] = {1u, (uint32_t)st.tex_edit_w, (uint32_t)st.tex_edit_h};
    bool ok = gzwrite(gz, header, 12) == 12;
    if (ok)
        ok = gzwrite(gz, buf.data(), (unsigned int)buf.size()) ==
             (int)buf.size();
    gzclose(gz);
    return ok;
}

// Build the GLTFTextureImage list for the current model by searching the same
// candidate filenames the viewer uses to load textures.
static std::vector<av::GLTFTextureImage> blender_build_texture_images(ViewerState& st) {
    std::vector<av::GLTFTextureImage> out;
    if (st.sel_path.empty()) return out;
    fs::path model_dir = fs::path(st.sel_path).parent_path();
    for (const auto& tex_name : st.model.texture_filenames) {
        if (tex_name.empty()) continue;
        av::GLTFTextureImage img;
        img.name = tex_name;
        fs::path tex_path = model_dir / tex_name;
        std::string stem = tex_path.stem().string();
        std::vector<fs::path> candidates = {
            tex_path,
            model_dir / (stem + "_2x.tex.png"),
            model_dir / (stem + ".tex.png"),
            model_dir / (stem + "_2x.pvr"),
            model_dir / (stem + ".pvr"),
            model_dir / (stem + "_2x.tex"),
            model_dir / (stem + ".tex"),
            model_dir / (stem + "_2x.png"),
            model_dir / (stem + ".png")
        };
        for (auto& cand : candidates) {
            if (fs::exists(cand) &&
                blender_texture_to_rgba(cand.string(), img.rgba, img.w, img.h)) {
                break;
            }
        }
        out.push_back(std::move(img));
    }
    return out;
}

// Kick off a round-trip: export the current preview model to the staging dir,
// install the addon if needed, and launch Blender. Does not block on Blender.
static void blender_start_roundtrip(ViewerState& st) {
    if (st.preview_type != PREVIEW_MODEL || st.model.meshes.empty()) {
        st.blender_status = "Open a POD model first to export to Blender.";
        return;
    }
    if (st.blender_active) {
        st.blender_status = "Blender round-trip already in progress.";
        return;
    }
    if (!fs::exists(st.blender_path)) {
        st.blender_status = "Blender not found at: " + std::string(st.blender_path);
        return;
    }
    fs::path staging = blender_staging_root();
    fs::create_directories(staging / "in");
    fs::create_directories(staging / "out");
    // Clear any leftovers from a previous round-trip so the daemon never
    // mistakes an old done.json / out GLB for this run.
    fs::remove(staging / "out/done.json");
    fs::remove(staging / "out/model.glb");
    fs::remove(staging / "in/model.glb");

    std::string err;
    if (!blender_install_extension(st, &err)) {
        st.blender_status = "Blender extension install failed: " + err;
        return;
    }

    // 1) Export the model to GLB with embedded textures.
    std::vector<av::GLTFTextureImage> images = blender_build_texture_images(st);
    std::string glb_out = (staging / "in/model.glb").string();
    fs::remove(glb_out);
    if (!av::gltf_export_glb(st.model, images, glb_out, &err)) {
        st.blender_status = "POD->GLB export failed: " + err;
        return;
    }

    // 2) Write the request file the addon polls.
    int run;
    {
        std::lock_guard<std::mutex> lk(st.blender_mutex);
        run = ++st.blender_run_id;
    }
    {
        std::ofstream req((staging / "in/request.json").string());
        req << "{\"run\":" << run << ",\"source_pod\":\"";
        for (char ch : st.sel_path) {
            if (ch == '"' || ch == '\\') req << '\\';
            req << ch;
        }
        req << "\",\"format\":\"glb\"}";
    }

    // 3) Stash state and launch Blender detached.
    {
        std::lock_guard<std::mutex> lk(st.blender_mutex);
        st.blender_source_pod = st.sel_path;
        st.blender_active = true;
    }
    if (!st.blender_daemon.joinable())
        st.blender_daemon = std::thread(blender_daemon_main, std::ref(st));
    st.blender_status = "Exporting to Blender (run " + std::to_string(run) + ")...";
    if (!blender_launch_gui(st, &err)) {
        {
            std::lock_guard<std::mutex> lk(st.blender_mutex);
            st.blender_active = false;
        }
        st.blender_status = "Blender launch failed: " + err;
    }
}

// Daemon thread: polls the staging out-dir for a done.json matching the active
// run, then imports the re-exported GLB back to a PODModel, writes the .pod
// back to the source path, and flags the main thread.
static void blender_daemon_main(ViewerState& st) {
    fs::path staging = blender_staging_root();
    fs::path done_path = staging / "out/done.json";
    fs::path glb_path = staging / "out/model.glb";
    while (!st.blender_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        int run = -1;
        std::string status;
        {
            std::lock_guard<std::mutex> lk(st.blender_mutex);
            run = st.blender_run_id;
            status = st.blender_active ? "active" : "";
        }
        if (status != "active") continue;

        if (!fs::exists(done_path)) continue;
        // done.json matches run?
        int done_run = -1;
        std::string done_status;
        {
            std::ifstream f(done_path.string());
            std::stringstream ss; ss << f.rdbuf();
            std::string txt = ss.str();
            auto pos = txt.find("\"run\"");
            if (pos != std::string::npos) {
                auto c = txt.find(':', pos);
                if (c != std::string::npos) {
                    while (c + 1 < txt.size() && (txt[c+1] == ' ' || txt[c+1] == '\t')) ++c;
                    done_run = atoi(txt.c_str() + c + 1);
                }
            }
            pos = txt.find("\"status\"");
            if (pos != std::string::npos) {
                auto q = txt.find(':', pos);
                auto oq = txt.find('"', q + 1);
                auto cq = txt.find('"', oq + 1);
                if (oq != std::string::npos && cq != std::string::npos)
                    done_status = txt.substr(oq + 1, cq - oq - 1);
            }
        }
        if (done_run != run) continue; // stale done from a previous run

        std::string msg;
        bool ok = false;
        if (done_status == "ok" && fs::exists(glb_path)) {
            av::PODModel pod;
            std::vector<av::GLTFImageBuffer> imgs;
            if (av::gltf_import_glb(glb_path.string(), pod, imgs, &msg)) {
                std::string pod_err;
                std::lock_guard<std::mutex> lk(st.blender_mutex);
                if (!st.blender_source_pod.empty() && av::pod_write(pod, st.blender_source_pod, &pod_err)) {
                    ok = true;
                    msg = "Round-trip complete -> " + st.blender_source_pod;
                } else {
                    msg = "POD write back failed: " + pod_err;
                }
            } else {
                msg = "GLB import back failed: " + msg;
            }
        } else {
            msg = "Blender reported status '" + done_status + "' (run " + std::to_string(done_run) + ")";
        }

        fs::remove(done_path);
        {
            std::lock_guard<std::mutex> lk(st.blender_mutex);
            st.blender_ok.store(ok);
            st.blender_result_msg = msg;
            st.blender_done.store(true);
            st.blender_active = false;
        }
    }
}

// Main-thread poller: if the daemon finished, refresh the viewer model and
// surface the result message. Called once per frame.
static void blender_poll_result(ViewerState& st) {
    if (!st.blender_done.load()) return;
    st.blender_done.store(false);
    bool ok = st.blender_ok.load();
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(st.blender_mutex);
        msg = st.blender_result_msg;
    }

    if (ok) {
        // Re-load the (possibly edited) pod back into the preview.
        std::string pod;
        {
            std::lock_guard<std::mutex> lk(st.blender_mutex);
            pod = st.blender_source_pod;
        }
        if (!pod.empty() && fs::exists(pod)) {
            FileEntry fe;
            fe.name = fs::path(pod).filename().string();
            fe.full_path = pod;
            fe.is_dir = false;
            fe.type = FTYPE_MODEL;
            std::error_code ec;
            fe.size = (size_t)fs::file_size(pod, ec);
            select_file(st, fe);
        }
        st.status_msg = msg;
        st.blender_status = msg;
    } else {
        st.blender_status = msg;
    }
}

// Persist the Blender bridge settings (currently just the executable path) to
// a small ini-style file under the user data dir so it survives restarts.
static fs::path blender_config_path() {
    return fs::path(blender_staging_root()) / "viewer_config.ini";
}

static void blender_save_config(const ViewerState& st) {
    std::error_code ec;
    fs::create_directories(blender_staging_root(), ec);
    std::ofstream out(blender_config_path());
    if (!out.is_open()) return;
    out << "blender_path=" << st.blender_path << "\n";
}

static void blender_load_config(ViewerState& st) {
    std::ifstream in(blender_config_path());
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("blender_path=", 0) == 0) {
            std::string v = line.substr(13);
            if (!v.empty()) {
                std::strncpy(st.blender_path, v.c_str(), sizeof(st.blender_path) - 1);
                st.blender_path[sizeof(st.blender_path) - 1] = '\0';
            }
        }
    }
}

static void blender_shutdown(ViewerState& st) {
    blender_save_config(st);
    st.blender_stop.store(true);
    if (st.blender_daemon.joinable()) st.blender_daemon.join();
}

static void draw_settings_dialog(ViewerState& st) {
    if (!st.show_settings) return;

    ImGui::OpenPopup("Settings / Preferences");
    
    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Settings / Preferences", &st.show_settings)) {
        
        static int active_tab = 0;
        
        ImGui::BeginChild("SettingsTabs", ImVec2(150, 0), ImGuiChildFlags_Borders);
        if (ImGui::Selectable("General", active_tab == 0)) active_tab = 0;
        if (ImGui::Selectable("Render Settings", active_tab == 1)) active_tab = 1;
        if (ImGui::Selectable("Camera Settings", active_tab == 2)) active_tab = 2;
        if (ImGui::Selectable("Asset Tweaks", active_tab == 3)) active_tab = 3;
        if (ImGui::Selectable("Blender Export", active_tab == 4)) active_tab = 4;
        if (ImGui::Selectable("PostFX", active_tab == 5)) active_tab = 5;
        ImGui::EndChild();
        
        ImGui::SameLine();
        
        ImGui::BeginChild("SettingsContent", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 5), ImGuiChildFlags_None);
        
        if (active_tab == 0) {
            ImGui::TextColored(g_theme.accent, "General Options");
            ImGui::Separator();
            
            const char* themes[] = { "Ruby Professional Dark", "Ruby Professional Light" };
            if (ImGui::Combo("UI Theme", &st.ui_theme, themes, IM_ARRAYSIZE(themes))) {
                apply_selected_theme(st.ui_theme, st.ui_font_scale);
            }
            
            if (ImGui::SliderFloat("UI Scale", &st.ui_font_scale, 0.5f, 2.0f, "%.2f")) {
                apply_selected_theme(st.ui_theme, st.ui_font_scale);
            }
            
            ImGui::Checkbox("Show full path in status bar", &st.show_full_path_status);
            ImGui::Checkbox("Auto refresh directories", &st.auto_refresh_dirs);
            
            ImGui::Spacing();
            ImGui::TextDisabled("Changes are applied immediately.");
        } 
        else if (active_tab == 1) {
            ImGui::TextColored(g_theme.accent, "Rendering & Visuals");
            ImGui::Separator();
            
            if (ImGui::ColorEdit3("Viewport Background", st.bg_color)) {
                av::g_clear_color[0] = st.bg_color[0];
                av::g_clear_color[1] = st.bg_color[1];
                av::g_clear_color[2] = st.bg_color[2];
            }
            ImGui::Checkbox("Render Grid Lines", &st.show_grid);
            ImGui::Checkbox("Show Hidden Scene Objects", &st.scene_show_hidden);
            ImGui::Checkbox("X-Ray / Ghost Mode", &st.scene_xray);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draw every object as a see-through ghost overlay (translucent fill + wireframe outlines)");
            ImGui::Checkbox("Scene Lights (ambient / directional / point)", &st.scene_lights_enabled);
            ImGui::Checkbox("Dynamic Lighting (Lambert)", &st.scene_lighting_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The full Lambert equation (hemisphere ambient + directional/point diffuse). Turn OFF to see the raw texture \u00d7 material color — the diagnostic that separates \"the texture is dark\" from \"the lighting is wrong\".");
            ImGui::Checkbox("Normal View (debug)", &st.scene_normals_debug);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Color = world-space normal (RGB = N*0.5+0.5). Surfaces whose normals point INTO the mesh show purple/blue instead of their surface tint — the classic \"black wood\" inverted-normal symptom.");
            ImGui::Checkbox("Emissive Glow Sprites (bloom)", &st.scene_glow_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render additive glow billboards at Light/SimpleGlow positions so torches and fire actually bloom.");
            ImGui::Checkbox("Depth Fog (vanilla atmosphere)", &st.scene_depth_fog_enabled);
            ImGui::Checkbox("Water & Lava (fluid sheets)", &st.scene_water_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render animated WaterMesh fluid surfaces (water pools, lava) as semi-transparent sheets.");
            ImGui::Checkbox("Contact Shadows (ShadowComponent + Hiro)", &st.scene_shadows_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render soft ground-aligned shadow blobs under objects with a ShadowComponent, plus the grounding shadow under Hiro in Play mode.");
            ImGui::Checkbox("Overlay Darkness Veil (overlay lights)", &st.scene_overlay_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Darken the scene toward overlay-light tint while point lights (torches/fire) punch through.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Distant geometry darkens toward the background, separating depth layers like the original game.");

            // High-DPI render scale: renders the viewport FBO at a multiple of
            // its logical size for crisp output on HiDPI displays. In Auto mode
            // Ruby measures real frame time and self-tunes; picking a fixed
            // value below switches to manual.
            if (ImGui::Checkbox("Adaptive Quality (auto)", &st.perf_auto))
                save_quality_config(st);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Automatically pick the render scale from measured frame time — start high, ease down if the machine struggles, then slowly creep back up. Always overrides a manual fixed value.");
            ImGui::BeginDisabled(st.perf_auto);
            int scale_opt = st.perf_tier;
            if (scale_opt < 0 || scale_opt >= kRenderTierCount) scale_opt = 0;
            if (ImGui::Combo("Render Scale (HiDPI)", &scale_opt, kRenderScaleLabels, kRenderTierCount)) {
                st.perf_auto = false;
                st.perf_tier = scale_opt;
                st.render_scale = kRenderScaleVals[st.perf_tier];
                st.status_msg = "Render scale set to " + std::string(kRenderScaleLabels[scale_opt]);
                save_quality_config(st);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render the 3D viewport at a higher internal resolution, then scale it down to the UI size. 2.0x looks noticeably sharper on HiDPI displays (costs ~4x fill rate).");
            ImGui::EndDisabled();
            ImGui::SliderFloat("Grid Plane Size", &st.grid_size, 5.0f, 200.0f, "%.0f");
            
            static int msaa_opt = 1; // MSAA 4x
            const char* msaa_modes[] = { "Disabled", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
            ImGui::Combo("Anti-Aliasing", &msaa_opt, msaa_modes, IM_ARRAYSIZE(msaa_modes));
            
            ImGui::Checkbox("Enable V-Sync", &st.enable_vsync);
        }
        else if (active_tab == 2) {
            ImGui::TextColored(g_theme.accent, "3D Orbit Camera");
            ImGui::Separator();
            
            ImGui::SliderFloat("Orbit Sensitivity", &st.cam_orbit_speed, 0.1f, 2.0f, "%.1f");
            ImGui::SliderFloat("Zoom Sensitivity", &st.cam_zoom_speed, 0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Pan Sensitivity", &st.cam_pan_speed, 0.001f, 0.02f, "%.3f");
            ImGui::Checkbox("Invert Orbit X (Yaw)", &st.cam_invert_x);
            ImGui::Checkbox("Invert Orbit Y (Pitch)", &st.cam_invert_y);
            
            ImGui::SliderFloat("Camera Field of View", &st.camera.fov, 10.0f, 120.0f, "%.0f°");
        }
        else if (active_tab == 3) {
            ImGui::TextColored(g_theme.accent, "Asset Operations");
            ImGui::Separator();
            
            ImGui::Checkbox("Pixel Art Filtering for Textures", &st.tex_pixel_art_mode);
            ImGui::Checkbox("Auto-play Animations", &st.anim_autoplay);
            ImGui::Checkbox("Use PVR Software Decompression", &g_pvr_software_decode);
            
            static float default_fps = 30.0f;
            ImGui::SliderFloat("Default Animation FPS", &default_fps, 1.0f, 120.0f, "%.0f");
        }
        else if (active_tab == 4) {
            ImGui::TextColored(g_theme.accent, "Blender Round-Trip");
            ImGui::Separator();

            ImGui::InputText("Blender Executable Path", st.blender_path, sizeof(st.blender_path));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Path to the Blender binary used for the POD <-> Blender round-trip.");
            if (ImGui::IsItemDeactivatedAfterEdit())
                blender_save_config(st);

            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_DOWNLOAD " (Re)Install Swordigo Round-Trip Addon", ImVec2(-1, 0))) {
                std::string err;
                if (blender_install_extension(st, &err)) {
                    st.blender_status = "Swordigo Round-Trip addon installed & enabled.";
                } else {
                    st.blender_status = "Addon install failed: " + err;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Builds and installs the 'swordigo_roundtrip' extension into Blender (user_default repo).");

            ImGui::Spacing();
            ImGui::TextDisabled("Workflow: Export the current POD to Blender via GLB (with embedded");
            ImGui::TextDisabled("textures), edit it in Blender, then save — the addon re-exports the");
            ImGui::TextDisabled("scene and writes the result back to the same .pod file.");
            ImGui::Spacing();
            if (!st.blender_status.empty())
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "%s", st.blender_status.c_str());
        }
        else if (active_tab == 5) {
            ImGui::TextColored(g_theme.accent, "Post Processing");
            ImGui::Separator();

            if (ImGui::Checkbox("Enable PostFX", &st.postfx_enabled))
                ;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Master switch for the preview post-processing chain (bloom, depth of field, HD grade, vignette, grain)");

            // ── Profile presets ──
            // One-click starting points. PostFX is the finishing stage — the
            // base lighting (ambient / directional / point) does the real work.
            static const char* kFxProfiles[] = {
                "Vanilla Swordigo", "Cinematic", "Clean / Neutral"
            };
            static int fx_profile = 0;
            if (ImGui::Combo("Profile", &fx_profile, kFxProfiles, 3)) {
                av::PostFXParams& pf = st.postfx;
                pf.bloom = true; pf.dof = false; pf.hd = true;
                pf.color_grade = true; pf.sharpen = true;
                pf.vignette = true; pf.grain = false;
                if (fx_profile == 0) {          // Vanilla Swordigo (default)
                    pf.exposure = 1.0f;
                    pf.bloom_strength = 0.22f; pf.bloom_threshold = 0.95f;
                    pf.saturation = 1.05f; pf.contrast = 1.04f;
                    pf.brightness = 0.03f; pf.warmth = 0.05f;
                    pf.sharpen_amount = 0.30f; pf.vignette_strength = 0.22f;
                } else if (fx_profile == 1) {   // Cinematic
                    pf.exposure = 1.15f;
                    pf.bloom_strength = 0.45f; pf.bloom_threshold = 0.80f;
                    pf.saturation = 1.18f; pf.contrast = 1.12f;
                    pf.brightness = 0.0f; pf.warmth = 0.10f;
                    pf.sharpen_amount = 0.45f; pf.vignette_strength = 0.45f;
                } else {                        // Clean / Neutral
                    pf.exposure = 1.0f;
                    pf.bloom_strength = 0.12f; pf.bloom_threshold = 1.00f;
                    pf.saturation = 1.0f; pf.contrast = 1.0f;
                    pf.brightness = 0.02f; pf.warmth = 0.0f;
                    pf.sharpen_amount = 0.20f; pf.vignette_strength = 0.10f;
                }
            }
            ImGui::Separator();
            ImGui::Checkbox("HD Render (exposure + tone map + gamma)", &st.postfx.hd);
            if (st.postfx.hd)
                ImGui::SliderFloat("Exposure", &st.postfx.exposure, 0.2f, 3.0f, "%.2f");

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.45f, 1.0f), "Bloom");
            ImGui::Checkbox("Enable Bloom", &st.postfx.bloom);
            if (st.postfx.bloom) {
                ImGui::SliderFloat("Bloom Strength", &st.postfx.bloom_strength, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Bloom Threshold", &st.postfx.bloom_threshold, 0.1f, 1.5f, "%.2f");
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.45f, 1.0f), "Depth of Field");
            ImGui::Checkbox("Enable Depth of Field", &st.postfx.dof);
            if (st.postfx.dof) {
                ImGui::SliderFloat("Focus Distance", &st.postfx.dof_focus, 0.1f, 100.0f, "%.1f");
                ImGui::SliderFloat("Blur Strength", &st.postfx.dof_scale, 0.0f, 8.0f, "%.1f");
                ImGui::TextDisabled("Blurs geometry away from the focus distance.");
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.45f, 1.0f), "Cinematic Grade");
            ImGui::Checkbox("Enable Color Grade", &st.postfx.color_grade);
            if (st.postfx.color_grade) {
                ImGui::SliderFloat("Saturation", &st.postfx.saturation, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Contrast", &st.postfx.contrast, 0.5f, 1.5f, "%.2f");
                ImGui::SliderFloat("Brightness", &st.postfx.brightness, -0.25f, 0.25f, "%.2f");
                ImGui::SliderFloat("Warmth", &st.postfx.warmth, -1.0f, 1.0f, "%.2f");
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.45f, 1.0f), "Crispness & Mood");
            ImGui::Checkbox("Sharpen (HD detail)", &st.postfx.sharpen);
            if (st.postfx.sharpen)
                ImGui::SliderFloat("Sharpen Amount", &st.postfx.sharpen_amount, 0.0f, 1.5f, "%.2f");
            ImGui::Checkbox("Vignette", &st.postfx.vignette);
            if (st.postfx.vignette)
                ImGui::SliderFloat("Vignette Strength", &st.postfx.vignette_strength, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Film Grain", &st.postfx.grain);
            if (st.postfx.grain)
                ImGui::SliderFloat("Grain Amount", &st.postfx.grain_amount, 0.0f, 0.3f, "%.3f");

            ImGui::Separator();
            if (ImGui::Button("Reset PostFX to Defaults")) {
                st.postfx = av::PostFXParams{};
                st.postfx_enabled = true;
            }
            ImGui::Spacing();
            ImGui::TextDisabled("PostFX runs on the model (POD) viewer and the 3D scene visualizer.");
        }
        
        ImGui::EndChild();
        
        ImGui::Separator();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 130);
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            st.show_settings = false;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

// ============================================================================
// Keyboard shortcut handling (called when ImGui doesn't want keyboard)
// ============================================================================

static bool handle_shortcuts(ViewerState& st) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard && !ImGui::IsKeyPressed(ImGuiKey_GraveAccent) &&
        !st.scene_save_requested) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (st.preview_type == PREVIEW_SCENE && st.scene_preview_tab == 1 && st.scene_transform_mode != 0) {
            st.scene_transform_mode = 0;
            return false;
        }
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_W)) st.show_wireframe = !st.show_wireframe;
    if (ImGui::IsKeyPressed(ImGuiKey_T)) st.show_textured = !st.show_textured;
    
    // Toggle bottom panel via Ctrl+` (or just GraveAccent alone when not focused in input fields)
    if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) || ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) {
        st.show_bottom_panel = !st.show_bottom_panel;
        st.workspace_layout_dirty = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        st.camera = av::Camera{};
        if (st.preview_type == PREVIEW_MODEL) {
            st.camera.target[0] = st.model.center_x;
            st.camera.target[1] = st.model.center_y;
            st.camera.target[2] = st.model.center_z;
            st.camera.distance  = st.model.radius * 2.5f;
            if (st.camera.distance < 1.0f) st.camera.distance = 3.0f;
        } else if (st.preview_type == PREVIEW_SCENE) {
            frame_scene_camera(st);
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1)) { st.type_filter = 0; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) { st.type_filter = 1; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) { st.type_filter = 2; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) { st.type_filter = 3; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) { st.type_filter = 4; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F6)) { st.type_filter = 5; apply_filters(st); }

    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P)) {
        st.show_inspector = !st.show_inspector;
        st.workspace_layout_dirty = true;
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P)) {
        st.show_settings = true;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B)) {
        st.show_asset_browser = !st.show_asset_browser;
        st.workspace_layout_dirty = true;
    }

    // Ctrl+S — save current scene (if scene is loaded and has a path)
    if (st.scene_save_requested || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))) {
        st.scene_save_requested = false;
        if (st.preview_type == PREVIEW_SCENE && !st.scene.filepath.empty()) {
            if (st.scene_preview_tab == 0) {
                st.has_compile_result = false;
                const auto start = std::chrono::high_resolution_clock::now();
                std::string compile_error;
                if (compile_scene_source(st, &compile_error)) {
                    st.compile_success = true;
                    st.scene_save_msg = "Compiled and saved " + st.scene.filename;
                } else {
                    st.compile_success = false;
                    st.compile_error_msg = compile_error;
                    st.scene_save_msg = "Compile error: " + compile_error;
                }
                st.compile_time_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - start).count();
                st.has_compile_result = true;
                st.scene_save_msg_timer = 4.0f;
                return false;
            }
            // Flush pending OnLoad edits before saving
            if (st.scene_onload_modified &&
                st.selected_object >= 0 &&
                st.selected_object < (int)st.scene.objects.size()) {
                auto& obj = st.scene.objects[st.selected_object];
                proto::Writer prog;
                prog.write_string_field(1, std::string(st.scene_onload_buf));
                obj.onload = prog.to_string();
                st.scene_onload_modified = false;
            }
            std::string save_error;
            const bool saved = av::scene_save(st.scene.filepath, st.scene, &save_error);
            if (saved) st.scene_dirty = false;
            st.scene_save_msg = saved ? "Saved " + st.scene.filename : "Save failed: " + save_error;
            st.scene_save_msg_timer = 4.0f;
            log_file_event("SceneSave", saved ? "Saved scene (Ctrl+S): " + st.scene.filename
                                               : "ERROR: " + save_error);
        }
    }

    const bool typing = ImGui::IsAnyItemActive() || io.WantTextInput;
    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift && !typing)
        restore_scene_history(st, false);
    // Redo: Ctrl+Y (menu) or Ctrl+Shift+Z (standard alternative).
    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && !typing &&
        (io.KeyShift ? ImGui::IsKeyPressed(ImGuiKey_Z) : ImGui::IsKeyPressed(ImGuiKey_Y)))
        restore_scene_history(st, true);

    // Ctrl+C — copy selected scene object(s)
    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        copy_scene_selection(st);
        return false;
    }
    // Ctrl+V — paste scene object(s) with nudge offset
    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
        paste_scene_selection(st);
        return false;
    }
    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
        duplicate_scene_selection(st);

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        if (st.selected_idx > 0) {
            st.selected_idx--;
            select_file(st, st.filtered_files[st.selected_idx]);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        if (st.selected_idx < (int)st.filtered_files.size() - 1) {
            st.selected_idx++;
            select_file(st, st.filtered_files[st.selected_idx]);
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        if (st.selected_idx >= 0 && st.selected_idx < (int)st.filtered_files.size()) {
            auto& f = st.filtered_files[st.selected_idx];
            if (f.is_dir) select_file(st, f);
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        fs::path parent = fs::path(st.current_dir).parent_path();
        if (!parent.empty() && parent != st.current_dir) {
            st.current_dir = parent.string();
            refresh_directory(st);
            apply_filters(st);
        }
    }

    return false;
}

// ============================================================================
// Main
// ============================================================================

// Adaptive render-quality governor. Called once per frame with the current
// frame time. Starts fast (highest tier the machine must plausibly drive),
// drops tier by tier while struggling, then slowly drifts back up when there
// is headroom. Only runs in auto mode. Writes the settled tier to disk so the
// next launch starts near the sweet spot.
static void tick_render_quality_governor(ViewerState& st, float frame_ms) {
    // Track only the render-heavy view. Frame time is smoothed to resist
    // single-frame hiccups (e.g. an object uploading a texture).
    st.perf_frame_ms = st.perf_frame_ms * 0.92f + frame_ms * 0.08f;

    if (!st.perf_auto) {
        st.perf_struggle_count = 0;
        st.perf_idle_ms = 0.0f;
        return;
    }

    // Frame-time thresholds (ms). 60fps = 16.7ms; 30fps = 33.3ms.
    const float kStruggleThresh = 24.0f;   // above this the viewport is falling behind
    const float kRiseGrace   = 6000.0f;    // wait 6s at a tier before probing higher
    const float kRiseGain    = 7000.0f;    // extra headroom needed to climb a tier

    // Run every frame; frame time is scaled so the governor reacts at ~60Hz.
    st.perf_idle_ms += frame_ms;

    if (st.perf_frame_ms > kStruggleThresh) {
        st.perf_struggle_count++;
        st.perf_idle_ms = 0.0f;   // drop reset grace — first chance to step down
    } else {
        st.perf_struggle_count = 0;
    }

    const bool struggling = st.perf_frame_ms > kStruggleThresh;
    // Step down only after a sustained stretch of struggle (~0.5s), so a brief
    // hitch (a texture upload, an import) never collapses quality on its own.
    if (struggling && st.perf_struggle_count >= 30 && st.perf_tier > 0) {
        // Consistent slowdown: step down one tier (not all at once).
        st.perf_struggle_count = 0;
        st.perf_idle_ms = 0.0f;
        st.perf_tier--;
        st.render_scale = kRenderScaleVals[st.perf_tier];
        save_quality_config(st);
        st.status_msg = "Adaptive quality: reduced render scale for smoother viewport";
    } else if (!struggling && st.perf_tier < kRenderTierCount - 1 &&
               st.perf_idle_ms > kRiseGrace + kRiseGain) {
        st.perf_idle_ms = 0.0f;
        st.perf_tier++;
        st.render_scale = kRenderScaleVals[st.perf_tier];
        save_quality_config(st);
        st.status_msg = "Adaptive quality: increased render scale";
    }
}

// ── MCP Console (Help menu) — interactive JSON-RPC tester for the MCP server ──
static bool g_mcp_console_open = false;
static char g_mcp_req[8192] =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
// Response display buffer — sized dynamically so multi-MB tool results are
// never silently truncated (scene_summary alone can be ~2.4 MB).
static std::string        g_mcp_resp_text;
static std::vector<char>  g_mcp_resp_buf(1, '\0');
static void draw_mcp_console() {
    if (!ImGui::Begin("MCP Console", &g_mcp_console_open, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("JSON-RPC 2.0 over stdio — the same protocol Claude Desktop /");
            ImGui::TextDisabled("Cline / Continue use when they spawn a local MCP server.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::Text("Server entry:  bin/ruby --mcp-server   |   bin/ruby_cli mcp");
    ImGui::TextDisabled("Resource root: %s", mcp::DefaultRootDir().c_str());
    ImGui::Separator();
    ImGui::TextWrapped("Tools — call via tools/call, e.g. {"
        "\"method\":\"tools/call\",\"params\":{\"name\":\"scene_summary\","
        "\"arguments\":{\"path\":\"...\"}}}):");
    if (ImGui::BeginChild("##mcp_tools", ImVec2(0.0f, 104.0f), true)) {
        ImGui::TextDisabled("%s", mcp::ToolListText().c_str());
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::InputTextMultiline("##mcp_req", g_mcp_req, sizeof(g_mcp_req), ImVec2(-1.0f, 120.0f));
    if (ImGui::Button("Send Request", ImVec2(140, 0))) {
        std::string out;
        if (mcp::HandleLine(g_mcp_req, out))
            g_mcp_resp_text = out;
        else
            g_mcp_resp_text =
                "No response (invalid JSON, or notification without an id).";
        if (g_mcp_resp_text.size() + 1 > g_mcp_resp_buf.size()) {
            g_mcp_resp_buf.resize(g_mcp_resp_text.size() * 2 + 1, 0);
        }
        std::memcpy(g_mcp_resp_buf.data(), g_mcp_resp_text.c_str(),
                    g_mcp_resp_text.size() + 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(90, 0))) { g_mcp_resp_text.clear(); g_mcp_resp_buf[0] = '\0'; }
    ImGui::InputTextMultiline("##mcp_resp", g_mcp_resp_buf.data(),
                              (size_t)g_mcp_resp_buf.size(),
                              ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
    ImGui::End();
}

int main(int argc, char* argv[]) {
    os_external::set_dev_working_dir(); // Windows: chdir to repo root so src/assets resolves

    // Headless MCP servers (AI agents connect via JSON-RPC).
    //   bin/ruby --mcp-server [--mcp-root DIR]          stdio transport
    //   bin/ruby --mcp-http-server [--port N] [--mcp-root DIR]   HTTP for ChatGPT
    bool mcp_mode = false;
    bool mcp_http = false;
    int  mcp_port = 8765;
    std::string mcp_root;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mcp-server") == 0) mcp_mode = true;
        else if (strcmp(argv[i], "--mcp-http-server") == 0) mcp_http = true;
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) mcp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--mcp-root") == 0 && i + 1 < argc) mcp_root = argv[++i];
    }
    if (mcp_http) return mcp::RunHttpServer(mcp_port, mcp_root);
    if (mcp_mode) return mcp::RunStdioServer(mcp_root);

    // Headless FBX → POD converter (see tools/pod_convert.cpp).
    //   bin/ruby --fbx2pod <in.fbx> [out.pod] [--no-flip] [--no-textures] [--force]
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fbx2pod") == 0)
            return av::pod_convert_cli(argc - (i + 1), argv + (i + 1));
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* window = SDL_CreateWindow(WIN_TITLE, WIN_W, WIN_H,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Ruby window icon — same launcher icon, from embedded assets (permanent fix)
    {
        const unsigned char* icond = nullptr;
        size_t iconsz = 0;
        if (!embedded_asset("launcer_icon.png", &icond, &iconsz))
            embedded_asset("icon_gnome.png", &icond, &iconsz);
        int iw = 0, ih = 0;
        unsigned char* ipx = nullptr;
        if (icond && asset_decode_image(icond, iconsz, &ipx, &iw, &ih)) {
            SDL_Surface* surf = SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_ABGR8888, ipx, iw * 4);
            if (surf) {
                SDL_Surface* copy = SDL_DuplicateSurface(surf);
                SDL_DestroySurface(surf);
                if (copy) {
                    SDL_SetWindowIcon(window, copy);
                    SDL_DestroySurface(copy);
                }
            }
            asset_image_free(ipx);
        }
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1); // VSync
    sword_init_gl_after();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    float dpi_scale = 1.0f;
    int display_id = SDL_GetDisplayForWindow(window);
    if (display_id) {
        float content_scale = SDL_GetDisplayContentScale(display_id);
        if (content_scale > 0) dpi_scale = content_scale;
    }
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;
    if (dpi_scale > 3.0f) dpi_scale = 3.0f;
    g_state.display_scale = dpi_scale;

    apply_selected_theme(g_state.ui_theme, g_state.ui_font_scale);
    av::g_clear_color[0] = g_state.bg_color[0];
    av::g_clear_color[1] = g_state.bg_color[1];
    av::g_clear_color[2] = g_state.bg_color[2];

    ImGui_ImplSDL3_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);

    // Font loading — Inter + Font Awesome solid icons, DPI-aware
    {
        float font_size_main = 16.0f * dpi_scale;

        ImFontConfig text_cfg;
        text_cfg.OversampleH = 3;
        text_cfg.OversampleV = 2;
        text_cfg.PixelSnapH = true;

        static const ImWchar icon_ranges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
        ImFontConfig icon_cfg;
        icon_cfg.MergeMode = true;
        icon_cfg.OversampleH = 2;
        icon_cfg.OversampleV = 2;
        icon_cfg.PixelSnapH = true;
        icon_cfg.GlyphMinAdvanceX = font_size_main;
        icon_cfg.GlyphOffset = ImVec2(0, 2);

        std::string inter_paths[] = {
            "src/assets/fonts/Inter-Regular.ttf",
            get_data_path("src/assets/fonts/Inter-Regular.ttf"),
            get_user_data_dir() + "launcher/fonts/Inter-Regular.ttf",
            get_user_data_dir() + "src/assets/fonts/Inter-Regular.ttf",
            "src/assets/fonts/MegalopolisExtra-Regular.otf",
            "/usr/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
        };

        std::string fa_paths[] = {
            get_user_data_dir() + "launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            "/usr/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            get_data_path("src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf"),
            get_user_data_dir() + "launcher/fonts/fa-solid-900.ttf",
            get_user_data_dir() + "launcher/fonts/fa-solid-900.otf",
            "/usr/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf",
            "/usr/share/swordigo-desktop/launcher/fonts/fa-solid-900.otf",
            "src/assets/fonts/fa-solid-900.ttf",
            "src/assets/fonts/fa-solid-900.otf",
            get_data_path("src/assets/fonts/fa-solid-900.ttf"),
            get_data_path("src/assets/fonts/fa-solid-900.otf"),
        };

        std::string inter_path, fa_path;
        for (auto& fp : inter_paths) {
            if (fs::exists(fp)) { inter_path = fp; break; }
        }
        for (auto& fp : fa_paths) {
            if (fs::exists(fp)) { fa_path = fp; break; }
        }

        if (!inter_path.empty()) {
            ImFont* font = io.Fonts->AddFontFromFileTTF(inter_path.c_str(), font_size_main, &text_cfg);
            if (font && !fa_path.empty()) {
                io.Fonts->AddFontFromFileTTF(fa_path.c_str(), font_size_main * 0.85f, &icon_cfg, icon_ranges);
                std::cout << "[Ruby] FontAwesome icons merged successfully from: " << fa_path << std::endl;
            }
            io.FontGlobalScale = 1.0f / dpi_scale;
            g_state.mono_font = font; // Use the verified main font for everything, ensuring zero icon breakages
        }
    }

    if (!av::renderer_init()) {
        fprintf(stderr, "Failed to initialize renderer.\n");
    }
    if (!av::audio_init()) {
        fprintf(stderr, "Failed to initialize audio.\n");
    }

    g_state.checker_tex = create_checkerboard();

    // Initialize default styles list with embedded configurations
    g_state.intellij_styles.push_back({"Redstell", "embedded://batsyntax"});
    g_state.intellij_styles.push_back({"FileRift (Grove)", "embedded://grove"});
    g_state.intellij_styles.push_back({"Ruby", "embedded://fallback"});
    g_state.intellij_style_idx = 0; // Redstell
    g_state.intellij_editor.load_style_from_memory(intel::BAT_SYNTAX_STYX_CONTENT);

    // ── Parse startup folder/file parameter ─────────────────────────────
    blender_load_config(g_state);
    load_workspace_layout(g_state);
    load_quality_config(g_state);
    if (g_state.perf_auto) {
        // Adaptive quality: start high so capable machines get the best
        // picture immediately, and let the governor walk down if needed.
        g_state.perf_tier = kRenderTierCount - 1;
        g_state.render_scale = kRenderScaleVals[g_state.perf_tier];
    }
    std::string start_path;
    if (argc > 1) {
        start_path = expand_home(argv[1]);
    } else {
        start_path = expand_home("~/.local/share/swordigo-desktop/assets/resources/");
    }

    std::error_code ec;
    if (fs::is_regular_file(start_path, ec)) {
        g_state.current_dir = fs::path(start_path).parent_path().string();
        refresh_directory(g_state);
        apply_filters(g_state);
        
        std::string target_name = fs::path(start_path).filename().string();
        for (int i = 0; i < (int)g_state.filtered_files.size(); i++) {
            if (g_state.filtered_files[i].name == target_name) {
                g_state.selected_idx = i;
                select_file(g_state, g_state.filtered_files[i]);
                break;
            }
        }
    } else if (fs::is_directory(start_path, ec)) {
        g_state.current_dir = start_path;
        refresh_directory(g_state);
        apply_filters(g_state);
    } else {
        const char* home = getenv("HOME");
        g_state.current_dir = home ? home : "/";
        refresh_directory(g_state);
        apply_filters(g_state);
    }

    g_state.status_msg = "Ruby Ready — browse files on the left";

    Uint64 last_time = SDL_GetTicks();
    bool running = true;
    while (running) {
        Uint64 current_time = SDL_GetTicks();
        float dt = (current_time - last_time) / 1000.0f;
        last_time = current_time;

        // Scene visualizer: advance every object's POD animation clock.
        if (g_state.scene_anim_playing && !g_state.scene.objects.empty()) {
            if (g_state.scene_obj_anim_frame.size() != g_state.scene.objects.size())
                g_state.scene_obj_anim_frame.resize(g_state.scene.objects.size(), 0.0f);
            const float step = dt * 30.0f * g_state.scene_anim_speed;
            for (int i = 0; i < (int)g_state.scene.objects.size(); ++i) {
                const auto& o = g_state.scene.objects[i];
                const std::string mname = o.mesh_name.empty() ? o.background_name : o.mesh_name;
                if (mname.empty()) { g_state.scene_obj_anim_frame[i] = 0.0f; continue; }
                auto mit = g_state.scene_model_cache.find(mname);
                if (mit == g_state.scene_model_cache.end() || mit->second.num_frames <= 0) {
                    g_state.scene_obj_anim_frame[i] = 0.0f;
                    continue;
                }
                // Only real animated entities (monsters/hero) loop their POD
                // animation. Doors, chests, switches and other props hold
                // their closed frame 0 — never open/close on their own.
                if (!sp::is_animated_entity(o)) {
                    g_state.scene_obj_anim_frame[i] = 0.0f;
                    continue;
                }
                g_state.scene_obj_anim_frame[i] =
                    std::fmod(g_state.scene_obj_anim_frame[i] + step, (float)mit->second.num_frames);
            }
        }

        // Scene Player: publish keyboard state + advance the engine.
        // Gated on the 3D visualizer being the active tab and no text field
        // being typed in (so hero keys never fire while editing names/search).
        {
            const bool viz_active = (g_state.scene_preview_tab == 1);
            const bool typing = ImGui::GetIO().WantTextInput;
            if (viz_active && !typing)
                sp::sp_poll_keys(ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow),
                                 ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow),
                                 ImGui::IsKeyDown(ImGuiKey_Space),
                                 ImGui::IsKeyPressed(ImGuiKey_A, false)
                                     || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false),
                                 ImGui::IsKeyPressed(ImGuiKey_D, false)
                                     || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false));
            if (viz_active && g_state.scene_player.mode != sp::Mode::Off && !g_state.scene.objects.empty()) {
                // Let the engine run the scene; write positions back so the
                // visualizer (which reads the same scene) shows motion.
                const bool apply = true;
                sp::player_tick(g_state.scene_player, g_state.scene, dt, apply);
                // Copy engine animation frames into the per-object clock.
                if (g_state.scene_obj_anim_frame.size() != g_state.scene.objects.size())
                    g_state.scene_obj_anim_frame.resize(g_state.scene.objects.size(), 0.0f);
                for (const auto& po : g_state.scene_player.objects)
                    if (po.index >= 0 && po.index < (int)g_state.scene_obj_anim_frame.size())
                        g_state.scene_obj_anim_frame[po.index] = po.frame;
            }
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
        }

        av::audio_update();

        // Update animation frame rate-independently
        if (g_state.preview_type == PREVIEW_MODEL && g_state.anim_playing && g_state.model.num_frames > 0) {
            g_state.current_frame = std::fmod(g_state.current_frame + dt * g_state.anim_fps,
                                              (float)g_state.model.num_frames);
        } else if (g_state.preview_type == PREVIEW_MODEL && g_state.vendor_mode && g_state.vendor_anim_playing) {
            g_state.vendor_anim_time += dt;   // .ani clips wrap internally (fmod)
        }

        // Tick scene save message timer
        if (g_state.scene_save_msg_timer > 0.0f) {
            g_state.scene_save_msg_timer -= dt;
            if (g_state.scene_save_msg_timer <= 0.0f) {
                g_state.scene_save_msg_timer = 0.0f;
                g_state.scene_save_msg.clear();
            }
        }

        // Scene loading screen: keep the modal visible for a few frames so
        // the user sees it complete before the loaded content appears.
        if (g_state.scene_loading) {
            g_state.scene_loading_frames -= 1.0f;
            if (g_state.scene_loading_frames <= 0.0f) {
                g_state.scene_loading = false;
                g_state.scene_loading_msg.clear();
            }
        }

        // Poll the Blender round-trip daemon for completed exports.
        blender_poll_result(g_state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ── Scene loading screen ─────────────────────────────────────
        // Shown for a few frames after a heavy synchronous load (scene_load
        // with .scl library resolution, or player_begin world bake) so the
        // user gets visual confirmation before the scene appears.
        if (g_state.scene_loading) {
            const ImVec2 csz = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(csz.x * 0.5f, csz.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGui::Begin("##loading", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.35f, 1.0f), ICON_FA_SPINNER "  Preparing");
            if (!g_state.scene_loading_msg.empty())
                ImGui::TextWrapped("%s", g_state.scene_loading_msg.c_str());
            ImGui::TextDisabled("Scene loaded — finalizing libraries and world geometry…");
            ImGui::End();
        }

        if (handle_shortcuts(g_state)) running = false;

        // Draw top Blender-style main menu bar
        bool open_about = false;
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Vanilla Assets Folder")) {
                    std::error_code ec;
                    g_state.current_dir = expand_home("~/.local/share/swordigo-desktop/assets/resources/");
                    if (!fs::is_directory(g_state.current_dir, ec)) {
                        const char* home = getenv("HOME");
                        g_state.current_dir = home ? home : "/";
                    }
                    refresh_directory(g_state);
                    apply_filters(g_state);
                }
                ImGui::BeginDisabled(g_state.preview_type != PREVIEW_SCENE || g_state.scene.filepath.empty());
                if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", "Ctrl+S"))
                    g_state.scene_save_requested = true;
                ImGui::EndDisabled();
                if (ImGui::BeginMenu("Export")) {
                    ImGui::BeginDisabled(g_state.blender_active ||
                                         g_state.preview_type != PREVIEW_MODEL ||
                                         g_state.model.meshes.empty());
                    if (ImGui::MenuItem("Export to Blender...")) {
                        blender_start_roundtrip(g_state);
                    }
                    ImGui::EndDisabled();
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_IMAGE "  Batch Converter...")) {
                    g_state.batch_converter.open_window = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Esc")) {
                    running = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                const bool scene_open = g_state.preview_type == PREVIEW_SCENE;
                ImGui::BeginDisabled(!scene_open || g_state.scene_undo_stack.empty());
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) restore_scene_history(g_state, false);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!scene_open || g_state.scene_redo_stack.empty());
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) restore_scene_history(g_state, true);
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::BeginDisabled(!scene_open || g_state.scene_selection.empty());
                if (ImGui::MenuItem("Copy", "Ctrl+C")) copy_scene_selection(g_state);
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_scene_selection(g_state);
                if (ImGui::MenuItem("Delete", "Delete")) delete_scene_selection(g_state);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!scene_open || !g_state.scene_has_object_clipboard);
                if (ImGui::MenuItem("Paste", "Ctrl+V")) paste_scene_selection(g_state);
                ImGui::EndDisabled();
                ImGui::Separator();
                if (ImGui::MenuItem("Preferences / Settings...", "Ctrl+P")) {
                    g_state.show_settings = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Mode")) {
                const bool scene_open = g_state.preview_type == PREVIEW_SCENE;
                const bool vis_active  = g_state.scene_player.mode == sp::Mode::Visualise;
                const bool hiro_active = g_state.scene_player.mode == sp::Mode::PlayHiro;
                ImGui::BeginDisabled(!scene_open || g_state.scene.objects.empty());
                if (ImGui::MenuItem("Visualise Scene Playing", nullptr, vis_active)) {
                    g_state.scene_loading = true;
                    g_state.scene_loading_frames = 3.0f;
                    g_state.scene_loading_msg = g_state.scene.filename + " — visualising";
                    sp::player_end(g_state.scene_player);
                    sp::player_begin(g_state.scene_player, g_state.scene, sp::Mode::Visualise,
                                     g_state.scene.filepath.empty()
                                         ? std::string()
                                         : fs::path(g_state.scene.filepath).parent_path().string());
                    g_state.scene_player_window_open = true;
                    g_state.scene_anim_playing = true;
                    g_state.status_msg = "Mode: visualising scene playback.";
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Play the scene's animations + AI simulation (no hero).");
                if (ImGui::MenuItem("Spawn Hiro and Play", nullptr, hiro_active)) {
                    g_state.scene_loading = true;
                    g_state.scene_loading_frames = 3.0f;
                    g_state.scene_loading_msg = g_state.scene.filename + " — spawning Hiro";
                    sp::player_end(g_state.scene_player);
                    sp::player_begin(g_state.scene_player, g_state.scene, sp::Mode::PlayHiro,
                                     g_state.scene.filepath.empty()
                                         ? std::string()
                                         : fs::path(g_state.scene.filepath).parent_path().string());
                    g_state.scene_player_window_open = true;
                    g_state.scene_anim_playing = true;
                    switch_scene_tab(g_state, 1);
                    g_state.status_msg = "Mode: Hiro spawned — A/D move, Space jump, Esc to stop.";
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spawn Hiro at the scene spawn point. Camera follows like the game.");
                ImGui::Separator();
                if (ImGui::MenuItem("Stop Playback")) {
                    sp::player_end(g_state.scene_player);
                    g_state.scene_player_window_open = false;
                    g_state.status_msg = "Scene player stopped.";
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Asset Browser", "Ctrl+B", &g_state.show_asset_browser))
                    g_state.workspace_layout_dirty = true;
                if (ImGui::MenuItem("Inspector", "Ctrl+Shift+P", &g_state.show_inspector))
                    g_state.workspace_layout_dirty = true;
                ImGui::MenuItem("Textured Mode", "T", &g_state.show_textured);
                ImGui::MenuItem("Wireframe Mode", "W", &g_state.show_wireframe);
                ImGui::Separator();
                ImGui::MenuItem("PBR Preview (vendored renderer)", nullptr, &g_state.pbr_preview);
                ImGui::MenuItem("PBR Directional Shadows", nullptr, &g_state.pbr_shadows);
                ImGui::SliderFloat("PBR Env Intensity", &g_state.pbr_env_intensity, 0.0f, 3.0f, "%.2f");
                if (g_state.vendor_mode) {
                    ImGui::Separator();
                    ImGui::MenuItem("Vendor .ani Animation", nullptr, &g_state.vendor_anim_playing);
                    ImGui::TextDisabled("Vendor .obj / .scn / .ani preview active");
                }
                if (ImGui::MenuItem("Bottom Panel (Terminal/Logger)", "Ctrl+`", &g_state.show_bottom_panel))
                    g_state.workspace_layout_dirty = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Workspace Layout"))
                    reset_workspace_layout(g_state);
                if (ImGui::MenuItem("Reset Camera", "R")) {
                    g_state.camera = av::Camera{};
                    if (g_state.preview_type == PREVIEW_MODEL) {
                        g_state.camera.target[0] = g_state.model.center_x;
                        g_state.camera.target[1] = g_state.model.center_y;
                        g_state.camera.target[2] = g_state.model.center_z;
                        g_state.camera.distance  = g_state.model.radius * 2.5f;
                        if (g_state.camera.distance < 1.0f) g_state.camera.distance = 3.0f;
                    } else if (g_state.preview_type == PREVIEW_SCENE) {
                        float bounds_center_x = (g_state.scene.bounds_min[0] + g_state.scene.bounds_max[0]) * 0.5f;
                        float bounds_center_y = (g_state.scene.bounds_min[1] + g_state.scene.bounds_max[1]) * 0.5f;
                        float bounds_center_z = (g_state.scene.bounds_min[2] + g_state.scene.bounds_max[2]) * 0.5f;
                        g_state.camera.target[0] = bounds_center_x;
                        g_state.camera.target[1] = bounds_center_y;
                        g_state.camera.target[2] = bounds_center_z;
                        float dx = g_state.scene.bounds_max[0] - g_state.scene.bounds_min[0];
                        float dy = g_state.scene.bounds_max[1] - g_state.scene.bounds_min[1];
                        float dz = g_state.scene.bounds_max[2] - g_state.scene.bounds_min[2];
                        g_state.camera.distance = std::sqrt(dx*dx + dy*dy + dz*dz) * 1.2f;
                        if (g_state.camera.distance < 5.0f) g_state.camera.distance = 25.0f;
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("About Ruby")) {
                    open_about = true;
                }
                if (ImGui::MenuItem("MCP Console", "AI agents")) {
                    g_mcp_console_open = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (open_about) {
            ImGui::OpenPopup("About Ruby");
        }

        // About Ruby Popup Modal
        if (ImGui::BeginPopupModal("About Ruby", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Ruby - Swordigo Engine SDK");
            ImGui::Separator();
            ImGui::Text("Single app for all of swordigo.");
            ImGui::Spacing();
            ImGui::TextColored(g_theme.success, "Credits");
            ImGui::BulletText("DanielSpaniel — creator of the original FileRift.py and Boulder");
            ImGui::Indent();
            ImGui::TextDisabled("(scene / object library) and the Boulder");
            ImGui::TextDisabled("ground-mesh engine. This C++ SDK translates and extends his work.");
            ImGui::Unindent();
            ImGui::BulletText("MrSinup — OpenSwordigo developer community member, Ruby Maintainer.");
            ImGui::BulletText("Swordiforge Modding Community");
            ImGui::Indent();
            ImGui::TextDisabled("Keeping up the game's modding scene lively.");
            ImGui::Unindent();
            ImGui::BulletText("Redstell & Coropatasy — Testing and StyleSheet presets");
            ImGui::BulletText("PowerVR Native SDK — PVRTC decompressors (MIT).");
            ImGui::BulletText("Dear ImGui, ImGuizmo, SDL3, STB, Dynarmic & the open-source");
            ImGui::Indent();
            ImGui::TextDisabled("ecosystem this project builds on.");
            ImGui::Unindent();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        if (g_mcp_console_open)
            draw_mcp_console();

        draw_settings_dialog(g_state);



        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);

        ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::Begin("##MainWindow", nullptr, wflags);

        float total_h = ImGui::GetContentRegionAvail().y;
        float content_h = total_h - STATUS_BAR_H;

        ImGui::BeginChild("ContentArea", ImVec2(0, content_h));

        const float available_w = ImGui::GetContentRegionAvail().x;
        const float split = PANEL_SPLITTER_W;
        const bool gm_fullwidth = g_state.preview_type == PREVIEW_SCENE &&
                                  g_state.scene_preview_tab == 3;

        // Deterministic workspace layout.
        // The center editor is the *primary* region and always receives the
        // remaining width. Side panels are bounded/resizable strips that never
        // steal more than what keeps the editor above its minimum, and the
        // geometry is exact (every column == a computed width, sum == width) so
        // dragging/resizing can never manufacture dead space.
        bool show_browser_now = g_state.show_asset_browser;
        bool show_inspector_now = g_state.show_inspector && !gm_fullwidth;
        // Drop the inspector first, then the browser, when the window is too
        // narrow to honour every panel's minimum alongside the editor.
        if (show_browser_now && show_inspector_now &&
            available_w < MIN_EDITOR_W + MIN_BROWSER_W + MIN_INSPECTOR_W + 2.0f * split)
            show_inspector_now = false;
        if (show_browser_now &&
            available_w < MIN_EDITOR_W + MIN_BROWSER_W + split)
            show_browser_now = false;

        auto limit_browser = [&](float w) {
            const float other = show_inspector_now ? MIN_INSPECTOR_W + 2.0f * split : split;
            return std::clamp(w, MIN_BROWSER_W, std::max(MIN_BROWSER_W, available_w - MIN_EDITOR_W - other));
        };
        auto limit_inspector = [&](float w) {
            const float other = show_browser_now ? MIN_BROWSER_W + 2.0f * split : split;
            return std::clamp(w, MIN_INSPECTOR_W, std::max(MIN_INSPECTOR_W, available_w - MIN_EDITOR_W - other));
        };
        if (show_browser_now)  g_state.asset_browser_width = limit_browser(g_state.asset_browser_width);
        if (show_inspector_now) g_state.inspector_width    = limit_inspector(g_state.inspector_width);

        const float w_browser   = show_browser_now ? g_state.asset_browser_width : 0.0f;
        const float w_inspector = show_inspector_now ? g_state.inspector_width    : 0.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));

        float xcur = 0.0f;

        // Left file browser + splitter.
        if (show_browser_now) {
            ImGui::SetCursorPos(ImVec2(xcur, 0.0f));
            ImGui::BeginChild("AssetBrowser", ImVec2(w_browser, content_h), ImGuiChildFlags_Borders);
            draw_file_browser(g_state);
            ImGui::EndChild();
            xcur += w_browser;

            ImGui::SetCursorPos(ImVec2(xcur, 0.0f));
            ImGui::InvisibleButton("##asset_splitter", ImVec2(split, content_h));
            const bool sz_hover = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (sz_hover) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDown[0]) {
                g_state.asset_browser_width = limit_browser(g_state.asset_browser_width + ImGui::GetIO().MouseDelta.x);
                g_state.workspace_layout_dirty = true;
            }
            xcur += split;
        }

        // Center editor — flexes to swallow all remaining width.
        const float editor_w = std::max(MIN_EDITOR_W, available_w - xcur - w_inspector - (show_inspector_now ? split : 0.0f));
        ImGui::SetCursorPos(ImVec2(xcur, 0.0f));
        ImGui::BeginChild("CenterPanel", ImVec2(editor_w, content_h));

        float center_panel_total_h = ImGui::GetContentRegionAvail().y;
        const float max_bottom_h = std::max(120.0f, center_panel_total_h - MIN_EDITOR_H - split);
        g_state.bottom_panel_height = std::clamp(g_state.bottom_panel_height, 120.0f, max_bottom_h);
        float bottom_panel_h = g_state.show_bottom_panel ? g_state.bottom_panel_height : 0.0f;
        float top_part_h = std::max(MIN_EDITOR_H, center_panel_total_h - bottom_panel_h -
                                    (g_state.show_bottom_panel ? split : 0.0f));

        ImGui::BeginChild("TopPartPreview", ImVec2(0, top_part_h));
        draw_center_panel(g_state);
        ImGui::EndChild();

        if (g_state.show_bottom_panel) {
            ImGui::InvisibleButton("##bottom_splitter", ImVec2(-1.0f, split));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                g_state.bottom_panel_height = std::clamp(g_state.bottom_panel_height - ImGui::GetIO().MouseDelta.y,
                                                          120.0f, max_bottom_h);
                g_state.workspace_layout_dirty = true;
            }
            draw_bottom_panel(g_state);
        }

        ImGui::EndChild();
        xcur += editor_w;

        // Right properties/inspector panel + splitter.
        if (show_inspector_now) {
            ImGui::SetCursorPos(ImVec2(xcur, 0.0f));
            ImGui::InvisibleButton("##inspector_splitter", ImVec2(split, content_h));
            const bool sz_hover2 = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (sz_hover2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDown[0]) {
                g_state.inspector_width = limit_inspector(g_state.inspector_width - ImGui::GetIO().MouseDelta.x);
                g_state.workspace_layout_dirty = true;
            }
            xcur += split;

            ImGui::SetCursorPos(ImVec2(xcur, 0.0f));
            ImGui::BeginChild("PropertiesHost", ImVec2(w_inspector, content_h), ImGuiChildFlags_Borders);
            draw_properties_panel(g_state);
            ImGui::EndChild();
        }

        ImGui::PopStyleVar(2);
        ImGui::EndChild(); // ContentArea

        draw_status_bar(g_state);

        if (g_state.workspace_layout_dirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            save_workspace_layout(g_state);
            g_state.workspace_layout_dirty = false;
        }

        ImGui::End();

        // The object browser remains a utility window; mesh authoring is embedded.
        draw_object_browser(g_state);

        // ── Batch Converter modal window ───────────────────────────────────────
        {
            double now_sec = (double)SDL_GetTicks() / 1000.0;
            if (g_state.batch_converter.open_window) {
                ImGui::OpenPopup("  ⚙  Batch Converter — Ruby");
                g_state.batch_converter.open_window = false;
            }
            batch::draw_batch_converter(g_state.batch_converter, now_sec);
        }

        ImGui::Render();
        int fb_w, fb_h;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.18f, 0.18f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        // Adaptive-quality governor: measure real rendered frame time (including
        // the viewport FBO + postfx + swap) and let the render tier self-tune.
        {
            static Uint64 last_swap = 0;
            const Uint64 now = SDL_GetTicks();
            float frame_ms = 16.0f;
            if (last_swap != 0) frame_ms = (float)(now - last_swap);
            last_swap = now;
            if (frame_ms > 0.0f && frame_ms < 500.0f)
                tick_render_quality_governor(g_state, frame_ms);
        }
    }

    batch::shutdown_batch(g_state.batch_converter);
    blender_shutdown(g_state);
    free_preview_resources(g_state);

    if (g_state.fbo) av::delete_fbo(g_state.fbo, g_state.fbo_tex);
    if (g_state.checker_tex) glDeleteTextures(1, &g_state.checker_tex);

    av::audio_shutdown();
    av::renderer_shutdown();

    pty_shutdown(g_state);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
