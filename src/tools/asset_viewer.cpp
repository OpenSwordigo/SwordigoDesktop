/* asset_viewer.cpp — Ruby: Professional Asset Viewer & Editor for Swordigo Desktop
 *
 * Features:
 *   - Blender-like flat neutral dark theme (Blender style)
 *   - FontAwesome 6/7 solid icons integration (merged with Inter font)
 *   - Path Breadcrumbs + Absolute Path input bar for system-wide browsing
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
#include "platform/IconsFontAwesome6.h"
#include "platform/protobuf_reader.h"
#include "tools/pod_loader.h"
#include "tools/av_renderer.h"
#include "tools/av_audio.h"
#include "tools/scene_loader.h"
#include "tools/intellij.h"
#include "tools/boulder.h"
#include "tools/filerift.h"
#include "tools/batch_converter.h"
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
static const char* WIN_TITLE         = "Ruby : Swordigo SDK";
static const float LEFT_PANEL_W      = 320.0f;
static const float RIGHT_PANEL_W     = 320.0f;
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
    GLuint                   model_texture  = 0;
    int                      highlighted_mesh = -1;

    // Scene preview
    av::SceneData scene;
    int           scene_preview_tab = 0; // 0 = Swordfare Editor, 1 = 3D Visualizer, 2 = Tree Inspector
    int           selected_object = -1;
    std::map<std::string, av::PODModel> scene_model_cache;
    std::map<std::string, std::vector<av::GPUMesh>> scene_gpu_mesh_cache;
    std::map<std::string, std::vector<GLuint>> scene_texture_cache;
    std::vector<std::vector<av::GPUMesh>> scene_ground_gpu_meshes;
    std::vector<std::vector<GLuint>> scene_ground_textures;

    // Checkerboard texture (for transparency)
    GLuint checker_tex = 0;

    // Selected file info
    std::string sel_name;
    std::string sel_path;
    size_t      sel_size = 0;

    // Multi-texture & Animation Support
    std::vector<GLuint>      model_textures;
    std::vector<std::string> missing_textures;
    int                      current_frame = 0;
    bool                     anim_playing = false;
    float                    anim_timer = 0.0f;
    float                    anim_fps = 30.0f;

    // Settings / Preferences
    bool        show_settings = false;
    int         ui_theme = 0; // 0 = Blender Dark, 1 = Ruby Cyber, 2 = ImGui Light, 3 = ImGui Classic
    float       ui_font_scale = 1.0f;
    bool        show_full_path_status = false;
    bool        auto_refresh_dirs = true;
    float       bg_color[3] = {0.08f, 0.08f, 0.11f};
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
    // PTY terminal state
    int                      pty_master_fd   = -1;
    pid_t                    pty_child_pid   = -1;
    std::string              pty_output_buf; // raw bytes from PTY, stripped of control seqs
    char                     terminal_input[512] = {};
    bool                     terminal_scroll_to_bottom = false;

    // --- Scene Editor state ---
    bool        scene_dirty = false;       // true when unsaved changes exist
    std::string scene_save_msg;            // status message after last save
    float       scene_save_msg_timer = 0.0f; // countdown to clear msg
    // Per-object OnLoad script editor
    char        scene_onload_buf[16384]   = {}; // decoded Lua source from selected obj onload
    bool        scene_onload_modified = false; // true if user edited the script
    char        scene_obj_name_buf[256]    = {}; // editable name field
    char        scene_obj_template_buf[256]= {}; // editable template field
    
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
};


static ViewerState g_state;

static bool load_scene_model_to_cache(ViewerState& st, const std::string& mesh_name, const std::string& scene_dir_path);

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
    st.current_frame = 0;
    st.anim_playing = false;
    st.anim_timer = 0.0f;

    st.scene = av::SceneData{};
    st.selected_object = -1;
    st.highlighted_mesh = -1;

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
    st.scene_preview_tab = 0;

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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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

        for (auto& mesh : st.model.meshes) {
            const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
            const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
            const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
            const uint16_t* idx = mesh.indices.empty()    ? nullptr : mesh.indices.data();
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
        st.scene_dirty         = false;
        st.scene_save_msg.clear();
        st.scene_save_msg_timer = 0.0f;
        st.scene_onload_buf[0] = '\0';
        st.scene_onload_modified = false;
        st.scene_obj_name_buf[0]     = '\0';
        st.scene_obj_template_buf[0] = '\0';

        if (st.scene.objects.empty()) {
            st.status_msg = "Failed to parse " + fe.name;
            log_file_event("SceneRead", "ERROR: Failed to parse scene file: " + fe.name);
        } else {
            st.scene_model_cache.clear();
            st.scene_gpu_mesh_cache.clear();
            st.scene_texture_cache.clear();
            st.scene_preview_tab = 0; // Default to Swordfare Editor

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
                    load_scene_model_to_cache(st, obj.background_name, scene_dir.string());
            }

            st.camera = av::Camera{};
            float bounds_center_x = (st.scene.bounds_min[0] + st.scene.bounds_max[0]) * 0.5f;
            float bounds_center_y = (st.scene.bounds_min[1] + st.scene.bounds_max[1]) * 0.5f;
            float bounds_center_z = (st.scene.bounds_min[2] + st.scene.bounds_max[2]) * 0.5f;
            st.camera.target[0] = bounds_center_x;
            st.camera.target[1] = bounds_center_y;
            st.camera.target[2] = bounds_center_z;

            float dx = st.scene.bounds_max[0] - st.scene.bounds_min[0];
            float dy = st.scene.bounds_max[1] - st.scene.bounds_min[1];
            float dz = st.scene.bounds_max[2] - st.scene.bounds_min[2];
            st.camera.distance = std::sqrt(dx*dx + dy*dy + dz*dz) * 1.2f;
            if (st.camera.distance < 5.0f) st.camera.distance = 25.0f;

            char buf[256];
            snprintf(buf, sizeof(buf), "Loaded %s — %d objects",
                     fe.name.c_str(), (int)st.scene.objects.size());
            st.status_msg = buf;
            log_file_event("SceneRead", "Loaded scene: " + fe.name + " (" + std::to_string(st.scene.objects.size()) + " objects, " + format_size(fe.size) + ")");
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
    ImGui::BeginChild("FileBrowser", ImVec2(LEFT_PANEL_W, 0), ImGuiChildFlags_Borders);

    // Absolute Path Input Bar
    char path_buf[512];
    strncpy(path_buf, st.current_dir.c_str(), sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';
    
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("##folder_path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        if (fs::is_directory(path_buf, ec)) {
            st.current_dir = path_buf;
            refresh_directory(st);
            apply_filters(st);
        }
    }
    ImGui::PopItemWidth();

    // Breadcrumbs Navigation
    ImGui::Spacing();
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
        ICON_FA_IMAGE " Tex",
        ICON_FA_CUBE " Model",
        ICON_FA_FILE " Scene",
        ICON_FA_MUSIC " Audio",
        ICON_FA_CODE " Code"
    };
    for (int i = 0; i < 6; i++) {
        if (i > 0) ImGui::SameLine();
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

static void draw_model_viewport(ViewerState& st) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = (int)avail.x;
    int h = (int)avail.y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (!st.fbo) {
        st.fbo = av::create_fbo(w, h, &st.fbo_tex);
        st.fbo_w = w; st.fbo_h = h;
    } else if (w != st.fbo_w || h != st.fbo_h) {
        av::resize_fbo(st.fbo, w, h, &st.fbo_tex);
        st.fbo_w = w; st.fbo_h = h;
    }

    av::begin_3d(st.fbo, w, h, st.camera);
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
            av::mat4_multiply(final_matrix, global_model_matrix, node_matrix);

            float* col = (node.object_index == st.highlighted_mesh) ? highlight : white;
            av::render_mesh(gm, final_matrix, col, false);

            if (st.show_wireframe) {
                float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                av::render_mesh(gm, final_matrix, wire_col, true);
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

            float* col = (i == st.highlighted_mesh) ? highlight : white;
            av::render_mesh(gm, global_model_matrix, col, false);

            if (st.show_wireframe) {
                float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                av::render_mesh(gm, global_model_matrix, wire_col, true);
            }
        }
    }
    av::end_3d();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)st.fbo_tex, ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0));

    // Draw overlay if texture dependencies are missing
    if (!st.missing_textures.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 10.0f, pos.y + 10.0f));
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
            float zoom_speed = 0.1f * st.cam_zoom_speed;
            st.camera.distance *= (1.0f - io.MouseWheel * zoom_speed);
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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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
    for (int i = 0; i < (int)st.scene.objects.size(); i++) {
        auto& obj = st.scene.objects[i];
        bool is_selected = (i == st.selected_object);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
        if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (obj.components.empty()) flags |= ImGuiTreeNodeFlags_Leaf;

        bool open = ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", obj.name.c_str());
        if (ImGui::IsItemClicked()) {
            if (st.selected_object != i) {
                st.selected_object = i;
                // Populate editor input buffers
                snprintf(st.scene_obj_name_buf, sizeof(st.scene_obj_name_buf),
                         "%s", obj.name.c_str());
                snprintf(st.scene_obj_template_buf, sizeof(st.scene_obj_template_buf),
                         "%s", obj.template_name.c_str());
                // Reset script editor so it re-reads from the new object
                st.scene_onload_buf[0] = '\0';
                st.scene_onload_modified = false;
            }
        }


        if (open) {
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
    std::vector<fs::path> search_paths = {
        scene_dir / mesh_name,
        scene_dir.parent_path() / mesh_name,
        scene_dir.parent_path() / "models" / mesh_name,
        scene_dir.parent_path() / "resources" / mesh_name,
        fs::path(g_assets_dir) / "resources" / mesh_name,
        fs::path(g_assets_dir) / "resources" / "models" / mesh_name
    };

    fs::path resolved_path;
    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            resolved_path = path;
            break;
        }
    }

    if (resolved_path.empty()) {
        return false;
    }

    av::PODModel model = av::pod_load(resolved_path.string());
    if (model.meshes.empty()) return false;

    std::vector<av::GPUMesh> gpu_meshes;
    for (auto& mesh : model.meshes) {
        const float*    pos = mesh.positions.empty()  ? nullptr : mesh.positions.data();
        const float*    nrm = mesh.normals.empty()    ? nullptr : mesh.normals.data();
        const float*    uv  = mesh.uvs.empty()        ? nullptr : mesh.uvs.data();
        const uint16_t* idx = mesh.indices.empty()    ? nullptr : mesh.indices.data();
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
            const uint16_t* idx_ptr = mesh.indices.empty() ? nullptr : mesh.indices.data();
            av::GPUMesh gm = av::upload_mesh(pos, nrm, uv, mesh.num_vertices,
                                              idx_ptr, (int)mesh.indices.size());
            st.scene_ground_gpu_meshes[idx].push_back(gm);

            std::string tex_name = obj.ground_mesh_textures[gi];
            GLuint tex_id = 0;
            if (!tex_name.empty()) {
                fs::path tex_path = scene_dir / tex_name;
                std::string stem = tex_path.stem().string();

                std::vector<fs::path> candidates = {
                    tex_path,
                    scene_dir.parent_path() / tex_name,
                    scene_dir.parent_path() / "textures" / tex_name,
                    scene_dir.parent_path() / "models" / tex_name,
                    fs::path(g_assets_dir) / "resources" / tex_name,
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

static void draw_scene_visualizer(ViewerState& st) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = (int)avail.x;
    int h = (int)avail.y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (!st.fbo) {
        st.fbo = av::create_fbo(w, h, &st.fbo_tex);
        st.fbo_w = w; st.fbo_h = h;
    } else if (w != st.fbo_w || h != st.fbo_h) {
        av::resize_fbo(st.fbo, w, h, &st.fbo_tex);
        st.fbo_w = w; st.fbo_h = h;
    }

    av::begin_3d(st.fbo, w, h, st.camera);
    if (st.show_grid) {
        av::render_grid(st.grid_size * 5.0f, st.scene.bounds_min[1]);
    }

    float white[4] = {1, 1, 1, 1};
    float highlight[4] = {0.4f, 0.7f, 1.0f, 1.0f};

    for (int idx = 0; idx < (int)st.scene.objects.size(); ++idx) {
        const auto& obj = st.scene.objects[idx];

        float T[16], R[16], S[16], temp[16], obj_mat[16];
        av::mat4_translate(T, obj.pos_x, obj.pos_y, obj.pos_z);
        av::mat4_rotate_y(R, obj.rot_y);
        
        av::mat4_identity(S);
        S[0] = obj.scale_x;
        S[5] = obj.scale_y;
        S[10] = obj.scale_z;

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

                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, obj_mat, wire_col, true);
                }
            }
        }

        std::string mname = obj.mesh_name.empty() ? obj.background_name : obj.mesh_name;
        if (mname.empty()) continue;

        if (!st.scene_model_cache.count(mname)) continue;

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
                av::get_node_matrix(model, ni, 0, node_matrix);
                av::mat4_multiply(final_matrix, obj_mat, node_matrix);

                float* col = (idx == st.selected_object) ? highlight : white;
                av::render_mesh(gm, final_matrix, col, false);

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

                if (st.show_wireframe) {
                    float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                    av::render_mesh(gm, obj_mat, wire_col, true);
                }
            }
        }
    }
    av::end_3d();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)st.fbo_tex, ImVec2((float)w, (float)h), ImVec2(0, 1), ImVec2(1, 0));

    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();

        if (io.MouseWheel != 0.0f) {
            float zoom_speed = 0.1f * st.cam_zoom_speed;
            st.camera.distance *= (1.0f - io.MouseWheel * zoom_speed);
            if (st.camera.distance < 0.1f) st.camera.distance = 0.1f;
            if (st.camera.distance > 2000.0f) st.camera.distance = 2000.0f;
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
    switch (st.preview_type) {
        case PREVIEW_MODEL:   draw_model_viewport(st);  break;
        case PREVIEW_TEXTURE: draw_texture_preview(st);  break;
        case PREVIEW_TEXT:    draw_text_preview(st);     break;
        case PREVIEW_SCENE:
            if (st.scene_preview_tab == 0) {
                bool scene_text_modified = st.scene_dirty;
                st.intellij_editor.draw_editor("scene_text", &st.text_edit_buffer, scene_text_modified, st.sel_path, st.mono_font, st.editor_custom_bg,
                                               st.has_compile_result, st.compile_success, st.compile_error_msg, st.compile_time_ms);
                if (scene_text_modified != st.scene_dirty) {
                    st.scene_dirty = scene_text_modified;
                }
            } else if (st.scene_preview_tab == 1) {
                draw_scene_visualizer(st);
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

// ============================================================================
// UI: Properties panel (right)
// ============================================================================

static void draw_properties_panel(ViewerState& st) {
    ImGui::BeginChild("Properties", ImVec2(RIGHT_PANEL_W, 0), ImGuiChildFlags_Borders);

    ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Properties");
    ImGui::Separator();

    if (st.preview_type == PREVIEW_NONE && st.sel_name.empty()) {
        ImGui::TextDisabled("Select a file to view metadata.");
        ImGui::EndChild();
        return;
    }

    ImGui::Text("Name: %s", st.sel_name.c_str());
    ImGui::Text("Size: %s", format_size(st.sel_size).c_str());
    ImGui::Text("Type: %s", filetype_label(
        st.preview_type == PREVIEW_TEXTURE ? FTYPE_TEXTURE :
        st.preview_type == PREVIEW_MODEL   ? FTYPE_MODEL   :
        st.preview_type == PREVIEW_SCENE   ? FTYPE_SCENE   :
        st.preview_type == PREVIEW_AUDIO   ? FTYPE_AUDIO   : FTYPE_OTHER));
    ImGui::TextWrapped("Path: %s", st.sel_path.c_str());
    ImGui::Separator();

    // Context helper buttons (Open Folder in VSCode & Export to GIMP)
    {
        auto dot_b = st.sel_name.rfind('.');
        std::string ext_b = (dot_b != std::string::npos) ? st.sel_name.substr(dot_b) : "";
        for (auto& c : ext_b) c = (char)tolower((unsigned char)c);
        
        if (ext_b == ".scene" || ext_b == ".scl") {
            ImGui::BeginDisabled(true);
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open Folder in VSCode", ImVec2(-1, 32))) {
                // Disabled placeholder
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Open this file's folder in Visual Studio Code (Future Feature).");
            }
            ImGui::Separator();
        }
        else if (ext_b == ".pvr" || ext_b == ".png" || ext_b == ".jpg" || ext_b == ".jpeg") {
            ImGui::BeginDisabled(true);
            if (ImGui::Button(ICON_FA_IMAGE " Export to GIMP", ImVec2(-1, 32))) {
                // Disabled placeholder
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Export texture and open it in GIMP (Future Feature).");
            }
            ImGui::Separator();
        }
    }

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
                st.current_frame = (st.current_frame + 1) % st.model.num_frames;
            }
            ImGui::SameLine();
            if (ImGui::Button("Prev")) {
                st.current_frame = (st.current_frame - 1 + st.model.num_frames) % st.model.num_frames;
            }

            ImGui::SliderInt("Frame", &st.current_frame, 0, st.model.num_frames - 1);
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
        ImGui::BeginDisabled(true); // Freeze / disable the button
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Export to Blender", ImVec2(-1, 32))) {
            // Future feature placeholder
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Future Plan: Convert the POD file to Blender native 3D model and export it directly.");
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

        // ── Save scene controls ─────────────────────────────────────
        bool can_save = !st.scene.filepath.empty();
        {
            ImVec4 btn_col = st.scene_dirty
                ? ImVec4(0.85f, 0.35f, 0.15f, 1.0f)
                : ImVec4(0.22f, 0.55f, 0.22f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btn_col.x+0.10f, btn_col.y+0.10f, btn_col.z+0.10f, 1.0f));
            if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save Scene  [Ctrl+S]",
                              ImVec2(-1.0f, 0.0f)) && can_save) {
                if (st.scene_preview_tab == 0) {
                    // Save by compiling the text editor markup back to binary
                    st.has_compile_result = false;
                    auto start = std::chrono::high_resolution_clock::now();
                    try {
                        std::string binary_data = filerift::recode_markup(st.text_edit_buffer, "scene");
                        std::ofstream out(st.scene.filepath, std::ios::binary);
                        if (out) {
                            out.write(binary_data.data(), binary_data.size());
                            st.scene = av::scene_load(st.scene.filepath);
                            st.scene_dirty = false;
                            st.scene_save_msg = "Compiled and Saved scene!";
                            st.scene_save_msg_timer = 4.0f;
                            log_file_event("SceneSave", "Compiled and saved scene: " + st.scene.filename);
                            
                            // Reload cached assets
                            st.scene_model_cache.clear();
                            st.scene_gpu_mesh_cache.clear();
                            st.scene_texture_cache.clear();
                            fs::path scene_dir = fs::path(st.scene.filepath).parent_path();
                            for (const auto& obj : st.scene.objects) {
                                if (!obj.mesh_name.empty())
                                    load_scene_model_to_cache(st, obj.mesh_name, scene_dir.string());
                                if (!obj.background_name.empty())
                                    load_scene_model_to_cache(st, obj.background_name, scene_dir.string());
                            }
                            
                            auto end = std::chrono::high_resolution_clock::now();
                            st.compile_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
                            st.compile_success = true;
                            st.has_compile_result = true;
                        }
                    } catch (const std::exception& e) {
                        st.status_msg = "Compile Error: " + std::string(e.what());
                        st.scene_save_msg = "Compile Error!";
                        st.scene_save_msg_timer = 4.0f;
                        
                        auto end = std::chrono::high_resolution_clock::now();
                        st.compile_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
                        st.compile_success = false;
                        st.compile_error_msg = e.what();
                        st.has_compile_result = true;
                    }
                } else {
                    // Flush any pending OnLoad script edit back to object
                    if (st.scene_onload_modified &&
                        st.selected_object >= 0 &&
                        st.selected_object < (int)st.scene.objects.size()) {
                        auto& obj = st.scene.objects[st.selected_object];
                        proto::Writer prog;
                        prog.write_string_field(1, std::string(st.scene_onload_buf));
                        obj.onload = prog.to_string();
                        st.scene_onload_modified = false;
                    }
                    av::scene_save(st.scene.filepath, st.scene);
                    st.scene_dirty = false;
                    st.scene_save_msg = "Saved " + st.scene.filename;
                    st.scene_save_msg_timer = 4.0f;
                    log_file_event("SceneSave", "Saved scene: " + st.scene.filename);
                }
            }
            ImGui::PopStyleColor(2);
        }
        if (!st.scene_save_msg.empty()) {
            ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.40f, 1.0f),
                               ICON_FA_CHECK " %s", st.scene_save_msg.c_str());
        }
        if (st.scene_dirty)
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                               ICON_FA_TRIANGLE_EXCLAMATION " Unsaved changes");

        // ── View mode selection (Tab Bar) ───────────────────────────
        ImGui::Spacing();
        if (ImGui::BeginTabBar("##SceneTabBar", ImGuiTabBarFlags_NoTooltip)) {
            bool tab_editor = (st.scene_preview_tab == 0);
            bool tab_visual = (st.scene_preview_tab == 1);
            bool tab_tree = (st.scene_preview_tab == 2);
            
            ImGuiTabItemFlags f_editor = tab_editor ? ImGuiTabItemFlags_SetSelected : 0;
            ImGuiTabItemFlags f_visual = tab_visual ? ImGuiTabItemFlags_SetSelected : 0;
            ImGuiTabItemFlags f_tree = tab_tree ? ImGuiTabItemFlags_SetSelected : 0;
            
            if (ImGui::BeginTabItem(ICON_FA_CODE " Editor", nullptr, f_editor)) {
                st.scene_preview_tab = 0;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(ICON_FA_CUBE " 3D View", nullptr, f_visual)) {
                st.scene_preview_tab = 1;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(ICON_FA_LAYER_GROUP " Inspector", nullptr, f_tree)) {
                st.scene_preview_tab = 2;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Spacing();
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
                for (int ci = 0; ci < (int)obj.components.size(); ++ci) {
                    const auto& c = obj.components[ci];
                    ImGui::BulletText("%s  (id=%d, %zu bytes)",
                                      c.type_name.empty() ? "(unnamed)" : c.type_name.c_str(),
                                      c.type_id, c.raw_data.size());
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
        ImGui::TableSetupColumn("PathColumn", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("StatusColumn", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("ShortcutsColumn", ImGuiTableColumnFlags_WidthFixed, 220.0f);

        ImGui::TableNextRow();

        // 1. Path Column
        ImGui::TableNextColumn();
        if (st.show_full_path_status && !st.sel_path.empty()) {
            ImGui::TextDisabled("%s", st.sel_path.c_str());
        } else {
            ImGui::TextDisabled("%s", st.current_dir.c_str());
        }

        // 2. Status Message Column
        ImGui::TableNextColumn();
        if (!st.status_msg.empty()) {
            float text_w = ImGui::CalcTextSize(st.status_msg.c_str()).x;
            float cell_w = ImGui::GetContentRegionAvail().x;
            if (cell_w > text_w) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_w - text_w) * 0.5f);
            }
            ImGui::Text("%s", st.status_msg.c_str());
        }

        // 3. Shortcuts Column
        ImGui::TableNextColumn();
        float short_w = ImGui::CalcTextSize("[W]ire [T]ex [R]eset [Esc]Quit").x;
        float cell_w = ImGui::GetContentRegionAvail().x;
        if (cell_w > short_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_w - short_w));
        }
        ImGui::TextDisabled("[W]ire [T]ex [R]eset [Esc]Quit");

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

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 0.0f;
    style.GrabRounding      = 0.0f;
    style.PopupRounding     = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding       = 0.0f;

    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 4);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 10.0f;
    style.IndentSpacing     = 12.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]             = ImVec4(0.18f, 0.18f, 0.18f, 1.00f); // #2e2e2e
    c[ImGuiCol_ChildBg]              = ImVec4(0.15f, 0.15f, 0.15f, 1.00f); // #262626
    c[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.11f, 0.11f, 0.98f); // #1c1c1c
    c[ImGuiCol_Border]               = ImVec4(0.09f, 0.09f, 0.09f, 0.50f); // #171717
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.24f, 0.24f, 0.24f, 1.00f); // #3d3d3d
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.18f, 0.18f, 0.18f, 0.50f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.12f, 0.12f, 0.12f, 0.30f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.31f, 0.31f, 0.31f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.28f, 0.45f, 0.70f, 1.00f); // blender blue #4772b3
    c[ImGuiCol_SliderGrab]           = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.28f, 0.45f, 0.70f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.28f, 0.45f, 0.70f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.28f, 0.45f, 0.70f, 0.40f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.28f, 0.45f, 0.70f, 0.80f);

    c[ImGuiCol_Separator]            = ImVec4(0.09f, 0.09f, 0.09f, 0.50f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.28f, 0.45f, 0.70f, 0.60f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.28f, 0.45f, 0.70f, 1.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
}

static void apply_ruby_cyber_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowRounding    = 4.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;

    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 4);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.ScrollbarSize     = 12.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.12f, 1.00f); // Deep cyber dark blue
    c[ImGuiCol_ChildBg]              = ImVec4(0.06f, 0.06f, 0.09f, 1.00f); // Darker blue for children
    c[ImGuiCol_PopupBg]              = ImVec4(0.05f, 0.05f, 0.08f, 0.96f);
    c[ImGuiCol_Border]               = ImVec4(0.16f, 0.24f, 0.35f, 0.80f); // Neon border
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.16f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.16f, 0.22f, 0.36f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.20f, 0.28f, 0.44f, 1.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.14f, 0.14f, 0.22f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.10f, 0.15f, 0.50f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.08f, 0.08f, 0.12f, 0.30f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.16f, 0.24f, 0.38f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.22f, 0.32f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.28f, 0.40f, 0.62f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.35f, 0.65f, 0.95f, 1.00f); // Bright cyber blue
    c[ImGuiCol_SliderGrab]           = ImVec4(0.22f, 0.32f, 0.50f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.14f, 0.20f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.20f, 0.28f, 0.44f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.14f, 0.20f, 0.32f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.20f, 0.28f, 0.44f, 0.60f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.35f, 0.65f, 0.95f, 0.80f);

    c[ImGuiCol_Separator]            = ImVec4(0.16f, 0.24f, 0.35f, 0.80f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.28f, 0.40f, 0.62f, 0.80f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.60f, 0.70f, 1.00f);
}

static void apply_selected_theme(int theme_idx, float font_scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = font_scale;
    
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
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Blender Export (Future Plan)");
            ImGui::Separator();
            
            static char blender_path[256] = "/usr/bin/blender";
            ImGui::InputText("Blender Executable Path", blender_path, sizeof(blender_path));
            
            static int export_fmt = 0; // glTF
            const char* formats[] = { "glTF (.gltf / .glb)", "FBX (.fbx)", "Wavefront OBJ (.obj)" };
            ImGui::Combo("Export Format", &export_fmt, formats, IM_ARRAYSIZE(formats));
            
            static float export_scale = 1.0f;
            ImGui::InputFloat("Export Scale Factor", &export_scale);
            
            static bool embed_tex = true;
            ImGui::Checkbox("Embed Textures in Export", &embed_tex);
            
            ImGui::Spacing();
            ImGui::TextDisabled("This integration will launch Blender automatically and import assets.");
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
    if (io.WantCaptureKeyboard && !ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) return true;

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
            float bounds_center_x = (st.scene.bounds_min[0] + st.scene.bounds_max[0]) * 0.5f;
            float bounds_center_y = (st.scene.bounds_min[1] + st.scene.bounds_max[1]) * 0.5f;
            float bounds_center_z = (st.scene.bounds_min[2] + st.scene.bounds_max[2]) * 0.5f;
            st.camera.target[0] = bounds_center_x;
            st.camera.target[1] = bounds_center_y;
            st.camera.target[2] = bounds_center_z;
            float dx = st.scene.bounds_max[0] - st.scene.bounds_min[0];
            float dy = st.scene.bounds_max[1] - st.scene.bounds_min[1];
            float dz = st.scene.bounds_max[2] - st.scene.bounds_min[2];
            st.camera.distance = std::sqrt(dx*dx + dy*dy + dz*dz) * 1.2f;
            if (st.camera.distance < 5.0f) st.camera.distance = 25.0f;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1)) { st.type_filter = 0; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) { st.type_filter = 1; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) { st.type_filter = 2; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) { st.type_filter = 3; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) { st.type_filter = 4; apply_filters(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F6)) { st.type_filter = 5; apply_filters(st); }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P)) {
        st.show_settings = true;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B)) {
        st.batch_converter.open_window = true;
    }

    // Ctrl+S — save current scene (if scene is loaded and has a path)
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (st.preview_type == PREVIEW_SCENE && !st.scene.filepath.empty()) {
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
            av::scene_save(st.scene.filepath, st.scene);
            st.scene_dirty = false;
            st.scene_save_msg = "Saved " + st.scene.filename;
            st.scene_save_msg_timer = 4.0f;
            log_file_event("SceneSave", "Saved scene (Ctrl+S): " + st.scene.filename);
        }
    }

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
            g_state.anim_timer += dt;
            float step = 1.0f / g_state.anim_fps;
            if (g_state.anim_timer >= step) {
                int frames_to_advance = static_cast<int>(g_state.anim_timer / step);
                g_state.current_frame = (g_state.current_frame + frames_to_advance) % g_state.model.num_frames;
                g_state.anim_timer = fmodf(g_state.anim_timer, step);
            }
        }

        // Tick scene save message timer
        if (g_state.scene_save_msg_timer > 0.0f) {
            g_state.scene_save_msg_timer -= dt;
            if (g_state.scene_save_msg_timer <= 0.0f) {
                g_state.scene_save_msg_timer = 0.0f;
                g_state.scene_save_msg.clear();
            }
        }

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
                if (ImGui::BeginMenu("Export")) {
                    ImGui::BeginDisabled(true);
                    ImGui::MenuItem("Export to Blender...");
                    ImGui::EndDisabled();
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_IMAGE "  Batch Converter...", "Ctrl+B")) {
                    g_state.batch_converter.open_window = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Esc")) {
                    running = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Preferences / Settings...", "Ctrl+P")) {
                    g_state.show_settings = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
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
            ImGui::Text("Ruby - Swordigo Desktop Asset Viewer & Editor");
            ImGui::Separator();
            ImGui::Text("Remastered 3D POD model hierarchy, texture, scene and audio player.");
            ImGui::Text("Credits: PowerVR Native SDK decompressors (MIT), syntax styling formats (.mtsx) by Redstell and COropatasy.");
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

        draw_file_browser(g_state);

        ImGui::SameLine();

        float center_w = ImGui::GetContentRegionAvail().x - RIGHT_PANEL_W;
        if (center_w < 100.0f) center_w = 100.0f;
        ImGui::BeginChild("CenterPanel", ImVec2(center_w, 0));
        
        float center_panel_total_h = ImGui::GetContentRegionAvail().y;
        float bottom_panel_h = g_state.show_bottom_panel ? 220.0f : 0.0f;
        float top_part_h = center_panel_total_h - bottom_panel_h;
        if (top_part_h < 100.0f) top_part_h = 100.0f;
        
        ImGui::BeginChild("TopPartPreview", ImVec2(0, top_part_h));
        draw_center_panel(g_state);
        ImGui::EndChild();
        
        if (g_state.show_bottom_panel) {
            draw_bottom_panel(g_state);
        }
        
        ImGui::EndChild();

        ImGui::SameLine();

        draw_properties_panel(g_state);

        ImGui::EndChild();

        draw_status_bar(g_state);

        ImGui::End();

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
