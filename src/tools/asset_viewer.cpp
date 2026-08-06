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
#include <GL/gl.h>
#include <GL/glext.h>

#include <unistd.h>
#include <set>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include "platform/pvr_loader.h"
#include "platform/data_path.h"
#include "platform/embedded_assets.h"
#include "platform/IconsFontAwesome6.h"
#include "platform/protobuf_reader.h"
#include "tools/pod_loader.h"
#include "tools/av_renderer.h"
#include "tools/av_audio.h"
#include "tools/scene_loader.h"
#include "tools/scene_schemas.h"
#include "tools/intellij.h"
#include "tools/boulder.h"
#include "tools/filerift.h"
#include "tools/batch_converter.h"
#include "tools/scene_workspace.h"
#include "Guizmo/src/ImGuizmo.h"
#include <zlib.h>

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

#include <sys/wait.h>

#include "tools/gltf_glb.h"
#include "tools/pod_writer.h"

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
static const char* GLSL_VERSION      = "#version 330";

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
};

static int classify_file(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return FTYPE_OTHER;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    if (ext == ".pvr" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") return FTYPE_TEXTURE;
    if (name.size() > 8) {
        std::string low = name;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find(".tex.png") != std::string::npos) return FTYPE_TEXTURE;
    }
    if (ext == ".pod") return FTYPE_MODEL;
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
    av::GPUMesh scene_proxy_mesh;
    // Background texture cache: BackgroundComponent name stem -> GL texture
    // (e.g. "graveyardback" -> graveyardback_2x.tex.png). The background is a
    // camera-following textured quad, not a POD model.
    std::map<std::string, GLuint> scene_background_textures;
    bool       scene_show_hidden = true;
    bool       scene_xray = false;      // ghost see-through viewport mode

    // Post-processing (bloom / DOF / HD grade / vignette / grain)
    bool            postfx_enabled = true;
    av::PostFXParams postfx;
    bool  scene_lights_enabled = true;  // render Light/SimpleGlow point lights
    bool  scene_glow_enabled = true;    // emissive glow sprites (bloom feed)
    bool  scene_depth_fog_enabled = true; // vanilla atmospheric depth darkening
    bool  scene_water_enabled = true;   // render WaterMesh fluid sheets (water/lava)
    // Textures for parsed WaterMesh sheets (parallel to st.scene.waters).
    std::vector<GLuint> scene_water_textures;
    float render_scale = 3.0f;          // high-DPI FBO render scale (default 3x)
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
    int         ui_theme = 1; // 0 = Graphite, 1 = Ruby Cyber, 2 = ImGui Light, 3 = ImGui Classic
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
    // PTY terminal state
    int                      pty_master_fd   = -1;
    pid_t                    pty_child_pid   = -1;
    std::string              pty_output_buf; // raw bytes from PTY, stripped of control seqs
    char                     terminal_input[512] = {};
    bool                     terminal_scroll_to_bottom = false;

    // --- Scene Editor state ---
    bool        scene_dirty = false;       // true when unsaved changes exist
    bool        scene_save_requested = false;
    std::string scene_save_msg;            // status message after last save
    float       scene_save_msg_timer = 0.0f; // countdown to clear msg
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
};


static ViewerState g_state;

static bool load_scene_model_to_cache(ViewerState& st, const std::string& mesh_name, const std::string& scene_dir_path);
static GLuint load_scene_background_texture(ViewerState& st, const std::string& bg_name,
                                            const std::string& scene_dir_path);
static void upload_scene_ground_meshes(ViewerState& st, const std::string& scene_dir_path);
static void frame_scene_camera(ViewerState& st);
static void frame_scene_at_spawn(ViewerState& st, int index);

// Scene mesh-editing / workspace helpers (defined with the visualizer section)
static void resync_scene_ground_meshes(ViewerState& st);
static void reupload_object_ground_meshes(ViewerState& st, int object_index);
static void upload_scene_waters(ViewerState& st, const std::string& scene_dir_path);

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

static const char* filetype_label(int ft) {
    switch (ft) {
        case FTYPE_TEXTURE: return "Texture";
        case FTYPE_MODEL:   return "POD Model";
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
        st.files.push_back(fe);
    }

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
// Resource cleanup
// ============================================================================

static void free_preview_resources(ViewerState& st) {
    for (auto& m : st.gpu_meshes) av::free_mesh(m);
    st.gpu_meshes.clear();
    st.model = av::PODModel{};

    if (st.preview_tex) { glDeleteTextures(1, &st.preview_tex); st.preview_tex = 0; }
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

    int bpp = 0;
    if (img_type == 1) {
        bpp = 4;
        format_str = "RGBA8888 (.tex.png)";
    } else if (img_type == 3 || img_type == 5) {
        bpp = 2;
        format_str = (img_type == 3) ? "RGBA4444 (.tex.png)" : "RGB565 (.tex.png)";
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

static GLuint load_texture_file(const std::string& path, int* out_w = nullptr, int* out_h = nullptr, std::string* out_format = nullptr) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    std::string low_path = path;
    for (auto& c : low_path) c = (char)tolower((unsigned char)c);

    bool is_tex_png = (low_path.size() >= 8 && low_path.substr(low_path.size() - 8) == ".tex.png");

    if (is_tex_png) {
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

    const char* suffixes[] = {"_2x.tex.png", ".tex.png", "_2x.pvr", ".pvr", "_2x.png", ".png"};
    for (auto& suf : suffixes) {
        fs::path candidate = dir / (stem + suf);
        if (fs::exists(candidate)) {
            st.model_texture = load_texture_file(candidate.string());
            if (st.model_texture) return;
        }
    }
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
        st.model = av::pod_load(fe.full_path);

        if (st.model.meshes.empty()) {
            st.status_msg = "Failed to load " + fe.name;
            log_file_event("ModelRead", "ERROR: Failed to load model: " + fe.name);
            st.preview_type = PREVIEW_NONE;
            break;
        }
        st.anim_fps = st.model.fps;

        for (auto& mesh : st.model.meshes) {
            const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
            const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
            const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
            const uint32_t* idx = mesh.indices.empty()    ? nullptr : mesh.indices.data();
            av::GPUMesh gm = av::upload_mesh(pos, nrm, uv, mesh.num_vertices,
                                              idx, (int)mesh.indices.size());
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

        // Auto search dependencies of the POD model
        st.model_textures.clear();
        st.missing_textures.clear();
        fs::path model_dir = fs::path(fe.full_path).parent_path();

        for (const auto& tex_name : st.model.texture_filenames) {
            if (tex_name.empty()) {
                st.model_textures.push_back(0);
                continue;
            }

            fs::path tex_path = model_dir / tex_name;
            std::string stem = tex_path.stem().string();

            // Candidates list (original name, tex.png, PVR, PNG)
            std::vector<fs::path> candidates = {
                tex_path,
                model_dir / (stem + "_2x.tex.png"),
                model_dir / (stem + ".tex.png"),
                model_dir / (stem + "_2x.pvr"),
                model_dir / (stem + ".pvr"),
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
            try_load_matching_texture(st, fe.full_path);
            if (st.model_texture) {
                if (st.model_textures.empty()) st.model_textures.push_back(st.model_texture);
                else st.model_textures[0] = st.model_texture;
            }
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "Loaded %s — %d mesh(es), %d verts, %d faces",
                 fe.name.c_str(), (int)st.model.meshes.size(),
                 st.model.total_vertices, st.model.total_faces);
        st.status_msg = buf;
        log_file_event("ModelRead", "Loaded model: " + fe.name + " (" + std::to_string(st.model.meshes.size()) + " mesh(es), " + std::to_string(st.model.total_vertices) + " vertices, " + format_size(fe.size) + ")");
    } break;

    case FTYPE_SCENE: {
        st.preview_type = PREVIEW_SCENE;
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
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    for (size_t i = 0; i < parts.size(); i++) {
        std::string name = parts[i].filename().string();
        if (name.empty()) {
            name = "/";
        }
        if (i > 0) {
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
    ImGui::BeginChild("FileBrowser", ImVec2(st.asset_browser_width, 0), ImGuiChildFlags_Borders);

    ImGui::TextDisabled("FILES");
    draw_breadcrumbs(st);
    ImGui::Spacing();

    // Search bar
    ImGui::PushItemWidth(-1);
    bool search_changed = ImGui::InputTextWithHint("##search", "Search files...",
                                                    st.search_buf, sizeof(st.search_buf));
    ImGui::PopItemWidth();

    // Filter buttons
    ImGui::Spacing();
    const char* labels[] = {
        ICON_FA_LAYER_GROUP " All",
        ICON_FA_IMAGE " Textures",
        ICON_FA_CUBE " Models",
        ICON_FA_FILE " Scenes",
        ICON_FA_MUSIC " Audio",
        ICON_FA_CODE " Code"
    };
    for (int i = 0; i < 6; i++) {
        if (i > 0 && ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(labels[i]).x +
                ImGui::GetStyle().FramePadding.x * 2.0f) ImGui::SameLine();
        bool active = (st.type_filter == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.45f, 0.70f, 1.0f)); // Blender highlighted Blue
        if (ImGui::SmallButton(labels[i])) {
            st.type_filter = i;
            search_changed = true;
        }
        if (active) ImGui::PopStyleColor();
    }
    ImGui::Separator();

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
            // same bone-matrix path as the ARM32 ModelInstance renderer.
            if (node.object_index < static_cast<int>(st.model.meshes.size()) &&
                st.model.meshes[node.object_index].bones_per_vertex > 0) {
                std::vector<float> skinned_positions, skinned_normals;
                if (av::skin_mesh(st.model, i, st.current_frame, skinned_positions, skinned_normals)) {
                    const auto& source_mesh = st.model.meshes[node.object_index];
                    av::update_mesh_vertices(gm, skinned_positions.data(),
                                             skinned_normals.empty() ? nullptr : skinned_normals.data(),
                                             source_mesh.uvs.empty() ? nullptr : source_mesh.uvs.data(),
                                             source_mesh.num_vertices);
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
            av::render_mesh(gm, final_matrix, col, false);

            if (st.show_wireframe) {
                float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                av::render_mesh(gm, final_matrix, wire_col, true);
            }
        }
        if (st.show_skeleton) {
            std::vector<float> lines;
            for (int i = 0; i < static_cast<int>(st.model.nodes.size()); ++i) {
                const int parent = st.model.nodes[i].parent_index;
                if (parent < 0 || parent >= static_cast<int>(st.model.nodes.size())) continue;
                float child[16], parent_matrix[16];
                av::get_node_matrix(st.model, i, st.current_frame, child);
                av::get_node_matrix(st.model, parent, st.current_frame, parent_matrix);
                lines.insert(lines.end(), {parent_matrix[12], parent_matrix[13], parent_matrix[14],
                                           child[12], child[13], child[14]});
            }
            float skeleton_color[4] = {1.0f, 0.45f, 0.08f, 1.0f};
            av::render_lines(lines.data(), static_cast<int>(lines.size() / 3),
                             skeleton_color, global_model_matrix, 2.0f);
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
            av::render_mesh(gm, centered_model, col, false);

            if (st.show_wireframe) {
                float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                av::render_mesh(gm, centered_model, wire_col, true);
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
    
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Reset logic: center and fit initially
    if (st.tex_reset_required) {
        float scale_w = avail.x / (float)st.tex_w;
        float scale_h = avail.y / (float)st.tex_h;
        float fit_scale = std::min(scale_w, scale_h) * 0.9f;
        st.tex_zoom = std::min(1.0f, fit_scale);
        if (st.tex_zoom < 0.05f) st.tex_zoom = 0.05f;
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
    if (st.tex_zoom > 1.0f && st.tex_pixel_art_mode) {
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
        if (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(2)) {
            st.tex_pan_x += io.MouseDelta.x;
            st.tex_pan_y += io.MouseDelta.y;
        }
    }

    ImGui::EndChild();
}

// ============================================================================
// UI: Center panel — Scene inspector
// ============================================================================

static void draw_scene_inspector(ViewerState& st) {
    if (st.scene.objects.empty()) {
        ImGui::TextDisabled("No objects in scene.");
        return;
    }

    ImGui::Text("Scene: %s — %d objects", st.scene.filename.c_str(), (int)st.scene.objects.size());
    ImGui::Separator();

    ImGui::BeginChild("SceneTree", ImVec2(0, 0));
    const ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < (int)st.scene.objects.size(); i++) {
        auto& obj = st.scene.objects[i];
        bool in_selection = is_scene_selected(st, i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
        if (in_selection) flags |= ImGuiTreeNodeFlags_Selected;
        if (obj.components.empty()) flags |= ImGuiTreeNodeFlags_Leaf;

        // Visibility (eye) toggle — professional outliner behaviour.
        ImGui::PushID(i);
        const char* eye = obj.hidden ? "  " : ICON_FA_EYE;
        if (ImGui::SmallButton(eye)) {
            snapshot_scene(st);
            obj.hidden = !obj.hidden;
            st.scene_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(obj.hidden ? "Show object" : "Hide object");
        ImGui::SameLine();

        std::string label = obj.name.empty() ? "(unnamed)" : obj.name;
        if (obj.hidden) label += "  [hidden]";
        bool open = ImGui::TreeNodeEx((void*)(intptr_t)(i + 1), flags, "%s", label.c_str());
        if (ImGui::IsItemClicked()) {
            if (io.KeyCtrl) toggle_scene_selection(st, i);
            else if (st.selected_object != i) select_scene_object(st, i);
        }
        ImGui::PopID();

        if (open) {
            if (in_selection && st.scene_selection.size() > 1)
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Part of %zu-object selection", st.scene_selection.size());
            ImGui::TextDisabled("Position: (%.2f, %.2f, %.2f)", obj.pos_x, obj.pos_y, obj.pos_z);
            ImGui::TextDisabled("Rotation: (%.2f, %.2f, %.2f)", obj.rot_x, obj.rot_y, obj.rot_z);
            ImGui::TextDisabled("Scale:    (%.2f, %.2f, %.2f)", obj.scale_x, obj.scale_y, obj.scale_z);

            for (auto& comp : obj.components) {
                ImGui::BulletText("%s (id=%d)", comp.type_name.c_str(), comp.type_id);
            }

            if (!obj.mesh_name.empty())
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Mesh: %s", obj.mesh_name.c_str());
            if (!obj.texture_name.empty())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Texture: %s", obj.texture_name.c_str());

            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
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

static bool load_scene_model_to_cache(ViewerState& st, const std::string& mesh_name, const std::string& scene_dir_path) {
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

    av::PODModel model = av::pod_load(resolved_path.string());
    if (model.meshes.empty()) {
        fprintf(stderr, "[RubyDebug] model '%s' found at %s but pod_load returned no meshes\n",
                mesh_name.c_str(), resolved_path.string().c_str());
        return false;
    }

    std::vector<av::GPUMesh> gpu_meshes;
    for (auto& mesh : model.meshes) {
        const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
        const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
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
        "_2x.tex.png", ".tex.png", "_2x.pvr", ".pvr", "_2x.png", ".png"
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
    st.scene_ground_gpu_meshes.clear();
    st.scene_ground_textures.clear();

    st.scene_ground_gpu_meshes.resize(st.scene.objects.size());
    st.scene_ground_textures.resize(st.scene.objects.size());

    fs::path scene_dir(scene_dir_path);

    for (size_t idx = 0; idx < st.scene.objects.size(); ++idx) {
        const auto& obj = st.scene.objects[idx];
        for (size_t gi = 0; gi < obj.ground_meshes.size(); ++gi) {
            const auto& mesh = obj.ground_meshes[gi];
            const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
            const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
            const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
            const uint32_t* idx_ptr = mesh.indices.empty() ? nullptr : mesh.indices.data();
            av::GPUMesh gm = av::upload_mesh(pos, nrm, uv, mesh.num_vertices,
                                              idx_ptr, (int)mesh.indices.size());
            st.scene_ground_gpu_meshes[idx].push_back(gm);

            std::string tex_name = obj.ground_mesh_textures[gi];
            GLuint tex_id = 0;
            if (!tex_name.empty()) {
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
                    scene_dir / (stem + "_2x.tex.png"),
                    scene_dir / (stem + ".tex.png"),
                    scene_dir / (stem + "_2x.pvr"),
                    scene_dir / (stem + ".pvr"),
                    scene_dir / (stem + "_2x.png"),
                    scene_dir / (stem + ".png"),
                    scene_dir.parent_path() / (stem + "_2x.tex.png"),
                    scene_dir.parent_path() / (stem + ".tex.png"),
                    scene_dir.parent_path() / (stem + "_2x.pvr"),
                    scene_dir.parent_path() / (stem + ".pvr"),
                    scene_dir.parent_path() / (stem + "_2x.png"),
                    scene_dir.parent_path() / (stem + ".png")
                };

                for (const auto& cand : candidates) {
                    if (fs::exists(cand)) {
                        tex_id = load_texture_file(cand.string());
                        if (tex_id) break;
                    }
                }
            }
            st.scene_ground_textures[idx].push_back(tex_id);
        }
    }
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
                scene_dir / (stem + "_2x.png"),    scene_dir / (stem + ".png"),
                scene_dir.parent_path() / (stem + "_2x.tex.png"),
                scene_dir.parent_path() / (stem + ".tex.png"),
                scene_dir.parent_path() / (stem + "_2x.pvr"),
                scene_dir.parent_path() / (stem + ".pvr"),
                scene_dir.parent_path() / (stem + "_2x.png"),
                scene_dir.parent_path() / (stem + ".png"),
                data_dir / "resources" / (stem + "_2x.pvr"),
                data_dir / "resources" / (stem + ".pvr"),
                data_dir / "resources" / (stem + "_2x.tex.png"),
                data_dir / "resources" / (stem + ".tex.png"),
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
        av::free_mesh(gpu[gi]);
        gpu[gi] = av::upload_mesh(mesh.positions.empty() ? nullptr : mesh.positions.data(),
                                   mesh.normals.empty() ? nullptr : mesh.normals.data(),
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
        0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
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
    st.camera.target[0] = sp.pos_x;
    st.camera.target[1] = sp.pos_y;
    st.camera.target[2] = sp.pos_z;
    // In-game style framing: a comfortable wide view around the spawn point.
    // The hero is a small object, so frame at a generous distance (~160 world
    // units of visible area) instead of hugging the spawn marker.
    const float radius = 70.0f;
    st.camera.distance = 160.0f;
    st.camera.near_plane = std::max(0.01f, st.camera.distance / 10000.0f);
    st.camera.far_plane = std::max(1000.0f, st.camera.distance + radius * 12.0f);
    st.camera.yaw = 0.0f;
    st.camera.pitch = 30.0f;
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

static std::string gm_build_swdm_text(const ViewerState& st) {
    boulder::GroundMesh gm;
    gm.polygon = st.gm_points;
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
                ".tex.png", "_2x.tex.png", ".pvr", "_2x.pvr", ".png", "_2x.png"
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
        if (st.gm_tool == id) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.42f, 0.66f, 1.0f));
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

    // Background + adaptive grid with axis labels.
    dl->AddRectFilled(canvas_origin, ImVec2(canvas_origin.x + canvas_w, canvas_origin.y + canvas_h),
                      IM_COL32(24, 26, 34, 255));
    const double grid_step = std::pow(10.0, std::floor(std::log10(40.0 / st.gm_canvas_scale)));
    char lbl[32];
    for (double gx = std::floor(st.gm_canvas_cx / grid_step) * grid_step;
         gx < st.gm_canvas_cx + canvas_w / st.gm_canvas_scale; gx += grid_step) {
        float sx = to_screen(gx, 0).x;
        dl->AddLine(ImVec2(sx, canvas_origin.y), ImVec2(sx, canvas_origin.y + canvas_h),
                    std::fabs(gx) < 1e-6 ? IM_COL32(130, 90, 90, 200) : IM_COL32(48, 52, 66, 255));
        snprintf(lbl, sizeof(lbl), "%.0f", gx);
        dl->AddText(ImVec2(sx + 3.0f, canvas_origin.y + canvas_h - 44.0f), IM_COL32(90, 100, 120, 160), lbl);
    }
    for (double gy = std::floor(st.gm_canvas_cy / grid_step) * grid_step;
         gy < st.gm_canvas_cy + canvas_h / (2.0 * st.gm_canvas_scale); gy += grid_step) {
        float sy = to_screen(0, gy).y;
        dl->AddLine(ImVec2(canvas_origin.x, sy), ImVec2(canvas_origin.x + canvas_w, sy),
                    std::fabs(gy) < 1e-6 ? IM_COL32(90, 130, 90, 200) : IM_COL32(48, 52, 66, 255));
        snprintf(lbl, sizeof(lbl), "%.0f", gy);
        dl->AddText(ImVec2(canvas_origin.x + 4.0f, sy - 7.0f), IM_COL32(90, 100, 120, 160), lbl);
    }
    // Axes.
    dl->AddLine(to_screen(0, -1000), to_screen(0, 1000), IM_COL32(90, 130, 90, 220));
    dl->AddLine(to_screen(-1000, 0), to_screen(1000, 0), IM_COL32(130, 90, 90, 220));

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

    // Mouse interactions (tool-aware).
    if (canvas_hovered) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (st.gm_tool == 3) {                       // Direct freehand outline
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
            if (hover >= 0) {
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
    // Frame the sketch (F key).
    if (do_frame_key) gm_frame_sketch();

    // Canvas hint bar + live stats.
    const char* gm_hint =
        (st.gm_tool == 0) ? "LMB move \xc2\xb7 click empty = add \xc2\xb7 RMB/Del = remove \xc2\xb7 Wheel zoom \xc2\xb7 MMB pan"
        : (st.gm_tool == 1) ? "LMB add points \xc2\xb7 RMB/Del = remove \xc2\xb7 Wheel zoom \xc2\xb7 MMB pan"
        : (st.gm_tool == 2) ? "LMB erase points \xc2\xb7 RMB/Del = remove \xc2\xb7 Wheel zoom \xc2\xb7 MMB pan"
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
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear", ImVec2(78, 26))) { st.gm_points.clear(); st.gm_sketch_dirty = true; }
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

        float T[16], R[16], S[16], temp[16], obj_mat[16];
        av::mat4_translate(T, obj.pos_x, obj.pos_y, obj.pos_z);
        av::mat4_rotate_z(R, obj.rot_y * 180.0f / 3.14159265358979323846f);
        av::mat4_identity(S);
        S[0] = obj.scale_x * obj.template_scaling;
        S[5] = obj.scale_y * obj.template_scaling;
        S[10] = obj.scale_z * obj.template_scaling;
        av::mat4_multiply(temp, T, R);
        av::mat4_multiply(obj_mat, temp, S);

        // Embedded ground meshes
        if (idx < (int)st.scene_ground_gpu_meshes.size()) {
            for (const auto& gm_raw : st.scene_ground_gpu_meshes[idx]) {
                auto& gm = const_cast<av::GPUMesh&>(gm_raw);
                gm.texture_id = 0;
                av::render_mesh(gm, obj_mat, ghost_fill, false);
                av::render_mesh(gm, obj_mat, ghost_wire, true);
            }
        }

        const std::string mname = obj.mesh_name.empty() ? obj.background_name : obj.mesh_name;
        if (mname.empty()) continue;
        auto mit = st.scene_model_cache.find(mname);
        if (mit == st.scene_model_cache.end()) continue;
        const auto& model = mit->second;
        auto gmit = st.scene_gpu_mesh_cache.find(mname);
        if (gmit == st.scene_gpu_mesh_cache.end()) continue;
        const auto& gpu_meshes = gmit->second;

        if (!model.nodes.empty()) {
            for (int ni = 0; ni < (int)model.nodes.size(); ++ni) {
                const auto& node = model.nodes[ni];
                if (node.object_index < 0 || node.object_index >= (int)gpu_meshes.size())
                    continue;
                float node_matrix[16], final_matrix[16];
                av::get_node_matrix(model, ni, st.current_frame, node_matrix);
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

static void draw_scene_visualizer(ViewerState& st) {
    ensure_scene_proxy_mesh(st);
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::AllowAxisFlip(false);
    ImGuizmo::SetGizmoSizeClipSpace(0.14f);

    // Blender-style command menus keep the full command surface available
    // without turning the viewport header into a multi-row button panel.
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
        if (ImGui::MenuItem("Top"))         { st.camera.yaw = 0.0f;  st.camera.pitch = 0.0f; }
        if (ImGui::MenuItem("Front"))       { st.camera.yaw = 0.0f;  st.camera.pitch = 89.0f; }
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
    ImGui::TextDisabled("|");

    const char* transform_modes[] = {"Select", "Move", "Rotate", "Scale"};
    const char* transform_shortcuts[] = {
        "Select / Navigate (1)", "Move (2)", "Rotate (3)", "Scale (4)"
    };
    for (int mode = 0; mode < 4; ++mode) {
        ImGui::SameLine();
        const bool active = (st.scene_transform_mode == mode) && !st.scene_mesh_edit;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.78f, 1.0f));
        if (ImGui::Button(transform_modes[mode])) {
            st.scene_transform_mode = mode;
            st.scene_mesh_edit = false;
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", transform_shortcuts[mode]);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const bool mesh_active = st.scene_mesh_edit;
    if (mesh_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
    if (ImGui::Button("Mesh Edit")) {
        st.scene_mesh_edit = !st.scene_mesh_edit;
        if (st.scene_mesh_edit) {
            st.mesh_edit_object = st.selected_object;
            if (st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size() &&
                st.scene.objects[st.selected_object].ground_meshes.empty())
                st.status_msg = "Selected object has no ground meshes to edit";
        }
    }
    if (mesh_active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle mesh editing (M)");
    if (mesh_active) {
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

    av::begin_3d(st.fbo, fw, fh, st.camera);

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

    // Upload the scene's Light + SimpleGlow components as warm point lights
    // (vanilla torches/fire glow), and give ambient a slight dark-blue cave
    // tint.  Lights toggle in the Settings -> Rendering tab.
    if (st.scene_lights_enabled) {
        const int n = std::min((int)st.scene.lights.size(), 16);
        float lpos[16][3] = {{0}};
        float lcol[16][3] = {{0}};
        float lrad[16] = {0};
        for (int i = 0; i < n; ++i) {
            lpos[i][0] = st.scene.lights[i].pos[0];
            lpos[i][1] = st.scene.lights[i].pos[1];
            lpos[i][2] = st.scene.lights[i].pos[2];
            lcol[i][0] = st.scene.lights[i].color[0] * st.scene.lights[i].intensity;
            lcol[i][1] = st.scene.lights[i].color[1] * st.scene.lights[i].intensity;
            lcol[i][2] = st.scene.lights[i].color[2] * st.scene.lights[i].intensity;
            lrad[i] = std::max(20.0f, st.scene.lights[i].radius);
        }
        av::set_point_lights(lpos, lcol, lrad, n);
    } else {
        av::clear_point_lights();
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

    for (int idx = 0; idx < (int)st.scene.objects.size(); ++idx) {
        const auto& obj = st.scene.objects[idx];
        if (obj.hidden && !st.scene_show_hidden)
            continue;
        bool rendered = false;

        float T[16], R[16], S[16], temp[16], obj_mat[16];
        av::mat4_translate(T, obj.pos_x, obj.pos_y, obj.pos_z);
        av::mat4_rotate_z(R, obj.rot_y * 180.0f / 3.14159265358979323846f);
        
        av::mat4_identity(S);
        S[0] = obj.scale_x * obj.template_scaling;
        S[5] = obj.scale_y * obj.template_scaling;
        S[10] = obj.scale_z * obj.template_scaling;

        av::mat4_multiply(temp, T, R);
        av::mat4_multiply(obj_mat, temp, S);

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
        std::string mname = obj.mesh_name;
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
                av::get_node_matrix(model, ni, st.current_frame, node_matrix);
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
                    if (av::skin_mesh(model, ni, st.current_frame, skinned_positions, skinned_normals)) {
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

    av::end_3d();

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
    if (!st.scene_mesh_edit && st.scene_show_gizmo && st.scene_transform_mode >= 1 &&
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

    if (viewport_hovered && !using_guizmo) {

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
            st.scene_mesh_edit = !st.scene_mesh_edit;
            if (st.scene_mesh_edit) {
                st.mesh_edit_object = st.selected_object;
                st.mesh_edit_mesh = st.mesh_edit_vertex = -1;
                st.mesh_edit_triangle = -1;
                st.mesh_edit_edge_a = st.mesh_edit_edge_b = -1;
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
        if (io.MouseWheel != 0.0f) {
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

        // ── Orbit: LMB in Navigate mode, or MMB anywhere ──
        if ((st.scene_transform_mode == 0 && !st.scene_mesh_edit && ImGui::IsMouseDragging(0)) ||
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
    
    bool is_protobuf = (ext == ".scl" || ext == ".gdata" || ext == ".gstate" || 
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
    ImGui::TextColored(ImVec4(0.00f, 0.62f, 1.00f, 1.0f), ICON_FA_FILE " %s", st.sel_name.c_str());
    ImGui::SameLine();
    
    // Save button (professional visual feedback)
    bool can_save = !st.text_is_binary && (st.text_edit_modified || is_protobuf);
    
    if (!can_save) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, st.text_edit_modified ? ImVec4(0.85f, 0.35f, 0.15f, 1.0f) : ImVec4(0.12f, 0.52f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st.text_edit_modified ? ImVec4(0.95f, 0.45f, 0.25f, 1.0f) : ImVec4(0.16f, 0.64f, 0.28f, 1.0f));
    if (ImGui::Button(ICON_FA_FLOPPY_DISK)) {
        std::ofstream out(st.sel_path, std::ios::binary);
        if (out) {
            if (is_protobuf) {
                auto start = std::chrono::high_resolution_clock::now();
                try {
                    std::string binary_data = filerift::recode_markup(st.text_edit_buffer, ext.substr(1));
                    out.write(binary_data.data(), binary_data.size());
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
                out.write(st.text_edit_buffer.data(), st.text_edit_buffer.size());
                st.text_preview_content = st.text_edit_buffer;
                st.text_edit_modified = false;
                st.status_msg = "Saved " + st.sel_name + " successfully!";
                log_file_event("FileSave", "Saved file: " + st.sel_name);
                
                st.has_compile_result = false;
            }
        } else {
            st.status_msg = "Failed to write file: " + st.sel_path;
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
        
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_PAINT_BRUSH " Styx Stylesheet Hub");
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
    st.intellij_editor.draw_editor("text_preview", &st.text_edit_buffer, st.text_edit_modified, st.sel_path, st.mono_font, st.editor_custom_bg);
}

// ── Real PTY Terminal ─────────────────────────────────────────────────────
#include <pty.h>
#include <utmp.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
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
            gethostname(host, sizeof(host));
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
        static const char* labels[] = {
            ICON_FA_CODE " Source",
            ICON_FA_CUBE " Visual",
            ICON_FA_LAYER_GROUP " Tree",
            ICON_FA_WRENCH " Mesh"
        };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));
        for (int mode = 0; mode < 4; ++mode) {
            if (mode > 0) ImGui::SameLine();
            const bool active = st.scene_preview_tab == mode;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.58f, 0.92f, 1.0f));
            }
            if (ImGui::Button(labels[mode])) {
                if (mode == 0 && st.scene_preview_tab != 0 && !st.scene_text_dirty) {
                    const std::string binary = av::scene_serialize(st.scene);
                    st.text_edit_buffer = filerift::decode_protobuf(binary, "scene");
                    st.text_preview_content = st.text_edit_buffer;
                }
                switch_scene_tab(st, mode);
            }
            if (active) ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleVar();
        if (st.scene_text_dirty && st.scene_preview_tab != 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.62f, 0.20f, 1.0f),
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
    ImGui::BeginChild("Properties", ImVec2(st.inspector_width, 0), ImGuiChildFlags_Borders);

    ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Properties");
    ImGui::Separator();

    if (st.preview_type == PREVIEW_NONE && st.sel_name.empty()) {
        ImGui::TextDisabled("Select a file to view metadata.");
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
        st.preview_type == PREVIEW_AUDIO   ? FTYPE_AUDIO   : FTYPE_OTHER));
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
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Animation Player");

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
        ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Interactive Lighting");
        
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
        
        ImGui::ColorEdit3("Light Color", av::g_light_color);
        ImGui::ColorEdit3("Ambient Color", av::g_ambient_color);
        
        if (ImGui::Button("Reset Lights")) {
            el = 45.0f;
            az = 45.0f;
            av::g_light_dir[0] = 0.577f; av::g_light_dir[1] = 0.577f; av::g_light_dir[2] = 0.577f;
            av::g_light_color[0] = 1.0f; av::g_light_color[1] = 0.95f; av::g_light_color[2] = 0.9f;
            av::g_ambient_color[0] = 0.3f; av::g_ambient_color[1] = 0.3f; av::g_ambient_color[2] = 0.35f;
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

        ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Render Settings");
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
        ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Model Transforms");
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
        ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Export Options");
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
        ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Texture Operations");
        
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
                ImGui::TextDisabled("%zu point lights (Light / SimpleGlow)",
                                    st.scene.lights.size());
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
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position X / Y");

            if (ImGui::DragFloat("##depth", &obj.pos_z, 0.1f, -999.f, 999.f, "Depth: %.3f")) {
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Depth (world Z / parallax layer)");

            if (ImGui::DragFloat("##rot", &obj.rot_y, 0.01f, -6.2832f, 6.2832f, "Rot: %.4f rad")) {
                obj.rot_x = obj.rot_z = 0.0f;
                st.scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Y-axis rotation (radians)");

            if (ImGui::DragFloat("##scale", &obj.scale_x, 0.01f, 0.001f, 100.f, "Scale: %.3f")) {
                obj.scale_y = obj.scale_z = obj.scale_x;
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
                                    if (av::scene_set_component_field(obj.components[ci], field))
                                        st.scene_dirty = true;
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
                    if (ImGui::Button(st.scene_mesh_edit ? ICON_FA_CHECK " Mesh Edit Active"
                                                         : ICON_FA_PEN " Edit in Viewport")) {
                        st.scene_mesh_edit = true;
                        st.mesh_edit_object = st.selected_object;
                        if (st.selected_object >= 0)
                            switch_scene_tab(st, 1);
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
                            if (ImGui::SmallButton("Edit This Mesh")) {
                                st.scene_mesh_edit = true;
                                st.mesh_edit_object = st.selected_object;
                                st.mesh_edit_mesh = mi;
                                st.mesh_edit_vertex = 0;
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
                                        st.scene_mesh_edit = true;
                                        st.mesh_edit_object = st.selected_object;
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
            ImGui::TextDisabled("%d/%zu visible  |  %d proxies",
                                st.scene_rendered_objects, st.scene.objects.size(), st.scene_proxy_objects);

        ImGui::TableNextColumn();
        const char* controls = st.preview_type == PREVIEW_SCENE && st.scene_preview_tab == 1
            ? "LMB Select  |  MMB Orbit  |  Wheel Zoom"
            : "W Wire  |  T Textures  |  R Reset";
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
    
    if (theme_idx == 0) {
        apply_blender_theme(1.0f);
    } else if (theme_idx == 1) {
        apply_ruby_cyber_theme();
    } else if (theme_idx == 2) {
        ImGui::StyleColorsLight();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 4.0f;
        style.ChildRounding     = 4.0f;
        style.FrameRounding     = 4.0f;
        style.GrabRounding      = 3.0f;
        style.PopupRounding     = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.TabRounding       = 4.0f;
    } else {
        ImGui::StyleColorsClassic();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 4.0f;
        style.ChildRounding     = 4.0f;
        style.FrameRounding     = 4.0f;
        style.GrabRounding      = 3.0f;
        style.PopupRounding     = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.TabRounding       = 4.0f;
    }
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
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "General Options");
            ImGui::Separator();
            
            const char* themes[] = { "Blender Dark", "Ruby Cyber", "ImGui Light", "ImGui Classic" };
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
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Rendering & Visuals");
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
            ImGui::Checkbox("Scene Point Lights (torches / fire)", &st.scene_lights_enabled);
            ImGui::Checkbox("Emissive Glow Sprites (bloom)", &st.scene_glow_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render additive glow billboards at Light/SimpleGlow positions so torches and fire actually bloom.");
            ImGui::Checkbox("Depth Fog (vanilla atmosphere)", &st.scene_depth_fog_enabled);
            ImGui::Checkbox("Water & Lava (fluid sheets)", &st.scene_water_enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render animated WaterMesh fluid surfaces (water pools, lava) as semi-transparent sheets.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Distant geometry darkens toward the background, separating depth layers like the original game.");

            // High-DPI render scale: renders the viewport FBO at a multiple of
            // its logical size for crisp output on HiDPI displays.
            const char* scale_labels[] = {"1.0x (native)", "1.5x", "2.0x", "3.0x"};
            const float scale_vals[]   = {1.0f, 1.5f, 2.0f, 3.0f};
            int scale_opt = 0;
            for (int i = 0; i < 4; ++i)
                if (fabsf(st.render_scale - scale_vals[i]) < 0.01f) scale_opt = i;
            if (ImGui::Combo("Render Scale (HiDPI)", &scale_opt, scale_labels, 4)) {
                st.render_scale = scale_vals[scale_opt];
                st.status_msg = "Render scale set to " + std::string(scale_labels[scale_opt]);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render the 3D viewport at a higher internal resolution, then scale it down to the UI size. 2.0x looks noticeably sharper on HiDPI displays (costs ~4x fill rate).");
            ImGui::SliderFloat("Grid Plane Size", &st.grid_size, 5.0f, 200.0f, "%.0f");
            
            static int msaa_opt = 1; // MSAA 4x
            const char* msaa_modes[] = { "Disabled", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
            ImGui::Combo("Anti-Aliasing", &msaa_opt, msaa_modes, IM_ARRAYSIZE(msaa_modes));
            
            ImGui::Checkbox("Enable V-Sync", &st.enable_vsync);
        }
        else if (active_tab == 2) {
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "3D Orbit Camera");
            ImGui::Separator();
            
            ImGui::SliderFloat("Orbit Sensitivity", &st.cam_orbit_speed, 0.1f, 2.0f, "%.1f");
            ImGui::SliderFloat("Zoom Sensitivity", &st.cam_zoom_speed, 0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Pan Sensitivity", &st.cam_pan_speed, 0.001f, 0.02f, "%.3f");
            ImGui::Checkbox("Invert Orbit X (Yaw)", &st.cam_invert_x);
            ImGui::Checkbox("Invert Orbit Y (Pitch)", &st.cam_invert_y);
            
            ImGui::SliderFloat("Camera Field of View", &st.camera.fov, 10.0f, 120.0f, "%.0f°");
        }
        else if (active_tab == 3) {
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Asset Operations");
            ImGui::Separator();
            
            ImGui::Checkbox("Pixel Art Filtering for Textures", &st.tex_pixel_art_mode);
            ImGui::Checkbox("Auto-play Animations", &st.anim_autoplay);
            ImGui::Checkbox("Use PVR Software Decompression", &g_pvr_software_decode);
            
            static float default_fps = 30.0f;
            ImGui::SliderFloat("Default Animation FPS", &default_fps, 1.0f, 120.0f, "%.0f");
        }
        else if (active_tab == 4) {
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Blender Round-Trip");
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
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Post Processing");
            ImGui::Separator();

            if (ImGui::Checkbox("Enable PostFX", &st.postfx_enabled))
                ;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Master switch for the preview post-processing chain (bloom, depth of field, HD grade, vignette, grain)");
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
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P)) {
        st.show_settings = true;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B)) {
        st.show_asset_browser = !st.show_asset_browser;
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

    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
        restore_scene_history(st, false);
    if (st.preview_type == PREVIEW_SCENE && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
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

int main(int argc, char* argv[]) {
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
        }

        // Tick scene save message timer
        if (g_state.scene_save_msg_timer > 0.0f) {
            g_state.scene_save_msg_timer -= dt;
            if (g_state.scene_save_msg_timer <= 0.0f) {
                g_state.scene_save_msg_timer = 0.0f;
                g_state.scene_save_msg.clear();
            }
        }

        // Poll the Blender round-trip daemon for completed exports.
        blender_poll_result(g_state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

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
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Asset Browser", "Ctrl+B", &g_state.show_asset_browser);
                ImGui::MenuItem("Inspector", "Ctrl+Shift+P", &g_state.show_inspector);
                ImGui::MenuItem("Textured Mode", "T", &g_state.show_textured);
                ImGui::MenuItem("Wireframe Mode", "W", &g_state.show_wireframe);
                ImGui::MenuItem("Bottom Panel (Terminal/Logger)", "Ctrl+`", &g_state.show_bottom_panel);
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
            ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.55f, 1.0f), "Credits");
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
        const float max_side = std::max(180.0f, available_w * 0.35f);
        g_state.asset_browser_width = std::clamp(g_state.asset_browser_width, 180.0f, max_side);
        g_state.inspector_width = std::clamp(g_state.inspector_width, 200.0f, max_side);
        if (available_w < 760.0f && g_state.show_asset_browser && g_state.show_inspector)
            g_state.show_inspector = false;

        if (g_state.show_asset_browser) {
            draw_file_browser(g_state);
            ImGui::SameLine();
            ImGui::InvisibleButton("##asset_splitter", ImVec2(5.0f, -1.0f));
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive())
                g_state.asset_browser_width = std::clamp(g_state.asset_browser_width + ImGui::GetIO().MouseDelta.x,
                                                          180.0f, max_side);
            ImGui::SameLine();
        }

        // In the Mesh (ground-mesh generator) tab, the inspector panel is
        // hidden and its width is donated to the 3D live preview + sketch.
        const bool gm_fullwidth = g_state.preview_type == PREVIEW_SCENE &&
                                  g_state.scene_preview_tab == 3;
        const bool show_inspector_now = g_state.show_inspector && !gm_fullwidth;

        float center_w = ImGui::GetContentRegionAvail().x -
                         (show_inspector_now ? g_state.inspector_width + 5.0f
                                                   + ImGui::GetStyle().ItemSpacing.x * 2.0f : 0.0f);
        center_w = std::max(160.0f, center_w);
        ImGui::BeginChild("CenterPanel", ImVec2(center_w, 0));
        
        float center_panel_total_h = ImGui::GetContentRegionAvail().y;
        g_state.bottom_panel_height = std::clamp(g_state.bottom_panel_height, 120.0f,
                                                  std::max(120.0f, center_panel_total_h * 0.65f));
        float bottom_panel_h = g_state.show_bottom_panel ? g_state.bottom_panel_height : 0.0f;
        float top_part_h = center_panel_total_h - bottom_panel_h;
        if (top_part_h < 100.0f) top_part_h = 100.0f;
        
        ImGui::BeginChild("TopPartPreview", ImVec2(0, top_part_h));
        draw_center_panel(g_state);
        ImGui::EndChild();
        
        if (g_state.show_bottom_panel) {
            ImGui::InvisibleButton("##bottom_splitter", ImVec2(-1.0f, 5.0f));
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive())
                g_state.bottom_panel_height = std::clamp(g_state.bottom_panel_height - ImGui::GetIO().MouseDelta.y,
                                                          120.0f, center_panel_total_h * 0.65f);
            draw_bottom_panel(g_state);
        }
        
        ImGui::EndChild();

        if (show_inspector_now) {
            ImGui::SameLine();
            ImGui::InvisibleButton("##inspector_splitter", ImVec2(5.0f, -1.0f));
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive())
                g_state.inspector_width = std::clamp(g_state.inspector_width - ImGui::GetIO().MouseDelta.x,
                                                      200.0f, max_side);
            ImGui::SameLine();
            draw_properties_panel(g_state);
        }

        ImGui::EndChild();

        draw_status_bar(g_state);

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
