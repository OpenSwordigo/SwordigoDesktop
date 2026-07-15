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

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include "platform/pvr_loader.h"
#include "platform/data_path.h"
#include "platform/IconsFontAwesome6.h"
#include "tools/pod_loader.h"
#include "tools/av_renderer.h"
#include "tools/av_audio.h"
#include "tools/scene_loader.h"
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <iostream>

#include <map>

namespace fs = std::filesystem;

std::string g_assets_dir = "assets";

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
static const char* WIN_TITLE         = "Ruby - Swordigo Desktop Asset Viewer & Editor";
static const float LEFT_PANEL_W      = 320.0f;
static const float RIGHT_PANEL_W     = 320.0f;
static const float STATUS_BAR_H      = 24.0f;
static const char* GLSL_VERSION      = "#version 330";

// ============================================================================
// File entry & type classification
// ============================================================================

enum FileType { FTYPE_OTHER = 0, FTYPE_TEXTURE = 1, FTYPE_MODEL = 2, FTYPE_SCENE = 3, FTYPE_AUDIO = 4 };
enum PreviewType { PREVIEW_NONE = 0, PREVIEW_TEXTURE, PREVIEW_MODEL, PREVIEW_SCENE, PREVIEW_AUDIO };

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
    int           selected_object = -1;
    bool          scene_visualize_mode = false;
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
};

static ViewerState g_state;

static bool load_scene_model_to_cache(ViewerState& st, const std::string& mesh_name, const std::string& scene_dir_path);

// ============================================================================
// Helpers
// ============================================================================

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
    st.scene_visualize_mode = false;

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
        st.preview_tex = load_texture_file(fe.full_path, &st.tex_w, &st.tex_h, &st.tex_format_str);
        if (st.preview_tex)
            st.status_msg = "Loaded " + fe.name + " — " + std::to_string(st.tex_w) + "x" + std::to_string(st.tex_h);
        else
            st.status_msg = "Failed to load " + fe.name;
    } break;

    case FTYPE_MODEL: {
        st.preview_type = PREVIEW_MODEL;
        st.model = av::pod_load(fe.full_path);

        if (st.model.meshes.empty()) {
            st.status_msg = "Failed to load " + fe.name;
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
    } break;

    case FTYPE_SCENE: {
        st.preview_type = PREVIEW_SCENE;
        st.scene = av::scene_load(fe.full_path);
        if (st.scene.objects.empty()) {
            st.status_msg = "Failed to parse " + fe.name;
        } else {
            st.scene_model_cache.clear();
            st.scene_gpu_mesh_cache.clear();
            st.scene_texture_cache.clear();
            st.scene_visualize_mode = false;

            fs::path scene_dir = fs::path(fe.full_path).parent_path();
            for (const auto& obj : st.scene.objects) {
                if (!obj.mesh_name.empty()) {
                    load_scene_model_to_cache(st, obj.mesh_name, scene_dir.string());
                }
                if (!obj.background_name.empty()) {
                    load_scene_model_to_cache(st, obj.background_name, scene_dir.string());
                }
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
        } else {
            st.status_msg = "Failed to load " + fe.name;
            st.preview_type = PREVIEW_NONE;
        }
    } break;

    default:
        st.preview_type = PREVIEW_NONE;
        st.status_msg = fe.name + " — no preview available";
        break;
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
        ICON_FA_MUSIC " Audio"
    };
    for (int i = 0; i < 5; i++) {
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
    av::render_grid(20.0f, st.model.min_y);
    float identity[16];
    av::mat4_identity(identity);
    float white[4] = {1, 1, 1, 1};
    float highlight[4] = {0.4f, 0.7f, 1.0f, 1.0f};

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

            float* col = (node.object_index == st.highlighted_mesh) ? highlight : white;
            av::render_mesh(gm, node_matrix, col, false);

            if (st.show_wireframe) {
                float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                av::render_mesh(gm, node_matrix, wire_col, true);
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
            av::render_mesh(gm, identity, col, false);

            if (st.show_wireframe) {
                float wire_col[4] = {0.2f, 0.8f, 1.0f, 0.5f};
                av::render_mesh(gm, identity, wire_col, true);
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
            st.camera.distance *= (1.0f - io.MouseWheel * 0.1f);
            if (st.camera.distance < 0.1f) st.camera.distance = 0.1f;
            if (st.camera.distance > 500.0f) st.camera.distance = 500.0f;
        }

        if (ImGui::IsMouseDragging(0)) {
            ImVec2 delta = io.MouseDelta;
            st.camera.yaw   += delta.x * 0.5f;
            st.camera.pitch += delta.y * 0.5f;
            if (st.camera.pitch >  89.0f) st.camera.pitch =  89.0f;
            if (st.camera.pitch < -89.0f) st.camera.pitch = -89.0f;
        }

        if (ImGui::IsMouseDragging(1)) {
            ImVec2 delta = io.MouseDelta;
            float scale = st.camera.distance * 0.003f;
            float yaw_rad = st.camera.yaw * 3.14159f / 180.0f;
            st.camera.target[0] -= (cosf(yaw_rad) * delta.x) * scale;
            st.camera.target[2] -= (-sinf(yaw_rad) * delta.x) * scale;
            st.camera.target[1] += delta.y * scale;
        }
    }
}

// ============================================================================
// UI: Center panel — Texture preview
// ============================================================================

static void draw_texture_preview(ViewerState& st) {
    if (!st.preview_tex) {
        ImGui::TextDisabled("Failed to load texture.");
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 img_size((float)st.tex_w * st.tex_zoom, (float)st.tex_h * st.tex_zoom);

    float off_x = (avail.x - img_size.x) * 0.5f + st.tex_pan_x;
    float off_y = (avail.y - img_size.y) * 0.5f + st.tex_pan_y;

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 img_min(cursor.x + off_x, cursor.y + off_y);
    ImVec2 img_max(img_min.x + img_size.x, img_min.y + img_size.y);
    
    if (st.checker_tex) {
        float uv_scale_x = img_size.x / 32.0f;
        float uv_scale_y = img_size.y / 32.0f;
        dl->AddImage((ImTextureID)(intptr_t)st.checker_tex,
                     img_min, img_max, ImVec2(0, 0), ImVec2(uv_scale_x, uv_scale_y));
    }

    dl->AddImage((ImTextureID)(intptr_t)st.preview_tex, img_min, img_max);

    ImGui::InvisibleButton("##tex_area", avail);
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            float old_zoom = st.tex_zoom;
            st.tex_zoom *= (1.0f + io.MouseWheel * 0.1f);
            if (st.tex_zoom < 0.1f) st.tex_zoom = 0.1f;
            if (st.tex_zoom > 32.0f) st.tex_zoom = 32.0f;

            float mx = io.MousePos.x - cursor.x - avail.x * 0.5f;
            float my = io.MousePos.y - cursor.y - avail.y * 0.5f;
            float factor = st.tex_zoom / old_zoom;
            st.tex_pan_x = mx - factor * (mx - st.tex_pan_x);
            st.tex_pan_y = my - factor * (my - st.tex_pan_y);
        }
        if (ImGui::IsMouseDragging(0)) {
            st.tex_pan_x += io.MouseDelta.x;
            st.tex_pan_y += io.MouseDelta.y;
        }
    }
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
        if (ImGui::IsItemClicked()) st.selected_object = i;

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
    av::render_grid(100.0f, st.scene.bounds_min[1]);

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
            st.camera.distance *= (1.0f - io.MouseWheel * 0.1f);
            if (st.camera.distance < 0.1f) st.camera.distance = 0.1f;
            if (st.camera.distance > 2000.0f) st.camera.distance = 2000.0f;
        }

        if (ImGui::IsMouseDragging(0)) {
            ImVec2 delta = io.MouseDelta;
            st.camera.yaw   += delta.x * 0.5f;
            st.camera.pitch += delta.y * 0.5f;
            if (st.camera.pitch >  89.0f) st.camera.pitch =  89.0f;
            if (st.camera.pitch < -89.0f) st.camera.pitch = -89.0f;
        }

        if (ImGui::IsMouseDragging(1)) {
            ImVec2 delta = io.MouseDelta;
            float scale = st.camera.distance * 0.003f;
            float yaw_rad = st.camera.yaw * 3.14159f / 180.0f;
            st.camera.target[0] -= (cosf(yaw_rad) * delta.x) * scale;
            st.camera.target[2] -= (-sinf(yaw_rad) * delta.x) * scale;
            st.camera.target[1] += delta.y * scale;
        }
    }
}

// ============================================================================
// UI: Center panel dispatcher
// ============================================================================

static void draw_center_panel(ViewerState& st) {
    switch (st.preview_type) {
        case PREVIEW_MODEL:   draw_model_viewport(st);  break;
        case PREVIEW_TEXTURE: draw_texture_preview(st);  break;
        case PREVIEW_SCENE:
            if (st.scene_visualize_mode) {
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
    } break;

    case PREVIEW_TEXTURE: {
        ImGui::Text("Width:  %d px", st.tex_w);
        ImGui::Text("Height: %d px", st.tex_h);
        ImGui::Text("Format: %s", st.tex_format_str.c_str());
        ImGui::Text("Zoom:   %.0f%%", st.tex_zoom * 100.0f);
        ImGui::Separator();
        if (ImGui::Button("Reset Zoom")) {
            st.tex_zoom = 1.0f;
            st.tex_pan_x = st.tex_pan_y = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit Screen")) {
            st.tex_zoom = 1.0f;
            st.tex_pan_x = st.tex_pan_y = 0.0f;
        }
    } break;

    case PREVIEW_SCENE: {
        ImGui::Text("Objects: %d", (int)st.scene.objects.size());
        ImGui::Separator();
        
        if (st.scene_visualize_mode) {
            if (ImGui::Button("Switch to Tree Inspector")) {
                st.scene_visualize_mode = false;
            }
        } else {
            if (ImGui::Button("Switch to 3D Visualizer")) {
                st.scene_visualize_mode = true;
            }
        }
        ImGui::Separator();

        if (st.selected_object >= 0 && st.selected_object < (int)st.scene.objects.size()) {
            auto& obj = st.scene.objects[st.selected_object];
            ImGui::TextColored(ImVec4(0.40f, 0.60f, 0.88f, 1.0f), "Selected Object");
            ImGui::Text("Name: %s", obj.name.c_str());
            ImGui::Text("Components: %d", (int)obj.components.size());
            ImGui::Separator();
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", obj.pos_x, obj.pos_y, obj.pos_z);
            ImGui::Text("Rotation: (%.2f, %.2f, %.2f)", obj.rot_x, obj.rot_y, obj.rot_z);
            ImGui::Text("Scale:    (%.2f, %.2f, %.2f)", obj.scale_x, obj.scale_y, obj.scale_z);
            if (!obj.mesh_name.empty())
                ImGui::Text("Mesh: %s", obj.mesh_name.c_str());
            if (!obj.texture_name.empty())
                ImGui::Text("Texture: %s", obj.texture_name.c_str());
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
    ImGui::BeginChild("StatusBar", ImVec2(0, STATUS_BAR_H));

    ImGui::TextDisabled("%s", st.current_dir.c_str());

    if (!st.status_msg.empty()) {
        float text_w = ImGui::CalcTextSize(st.status_msg.c_str()).x;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(avail * 0.3f);
        ImGui::Text("%s", st.status_msg.c_str());
    }

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200.0f + ImGui::GetCursorPosX());
    ImGui::TextDisabled("[W]ire [T]ex [R]eset [Esc]Quit");

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

// ============================================================================
// Keyboard shortcut handling (called when ImGui doesn't want keyboard)
// ============================================================================

static bool handle_shortcuts(ViewerState& st) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) return true;

    if (ImGui::IsKeyPressed(ImGuiKey_W)) st.show_wireframe = !st.show_wireframe;
    if (ImGui::IsKeyPressed(ImGuiKey_T)) st.show_textured = !st.show_textured;

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

    apply_blender_theme(dpi_scale);

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
        }
    }

    if (!av::renderer_init()) {
        fprintf(stderr, "Failed to initialize renderer.\n");
    }
    if (!av::audio_init()) {
        fprintf(stderr, "Failed to initialize audio.\n");
    }

    g_state.checker_tex = create_checkerboard();

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
                if (ImGui::MenuItem("Exit", "Esc")) {
                    running = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Textured Mode", "T", &g_state.show_textured);
                ImGui::MenuItem("Wireframe Mode", "W", &g_state.show_wireframe);
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
            ImGui::Text("Credits: PowerVR Native SDK decompressors (MIT).");
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

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
        draw_center_panel(g_state);
        ImGui::EndChild();

        ImGui::SameLine();

        draw_properties_panel(g_state);

        ImGui::EndChild();

        draw_status_bar(g_state);

        ImGui::End();

        ImGui::Render();
        int fb_w, fb_h;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.18f, 0.18f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    free_preview_resources(g_state);

    if (g_state.fbo) av::delete_fbo(g_state.fbo, g_state.fbo_tex);
    if (g_state.checker_tex) glDeleteTextures(1, &g_state.checker_tex);

    av::audio_shutdown();
    av::renderer_shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
