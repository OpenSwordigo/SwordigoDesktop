// ============================================================================
// ruby_gg.cpp — Ruby / Swordigo Studio v1.0 (Godot Native Frontend)
//   Industry-grade dark editor with wired Scene Generators, Batch Converter,
//   2D Texture Viewer, 3D Scene Viewport, and full asset pipeline integration.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <filesystem>
#include <set>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

// Godot C++ Core & Scene API
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/os/time.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "core/os/memory.h"
#include "servers/display/display_server.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/main/viewport.h"
#include "scene/gui/control.h"
#include "scene/gui/box_container.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/split_container.h"
#include "scene/gui/menu_bar.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/check_box.h"
#include "scene/gui/option_button.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/code_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/gui/subviewport_container.h"
#include "scene/gui/tree.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/center_container.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/panel.h"
#include "core/input/input_event.h"
#include "scene/gui/separator.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/resources/environment.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/mesh.h"
#include "scene/resources/font.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/style_box.h"
#include "scene/resources/theme.h"
#include "scene/resources/syntax_highlighter.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "platform/IconsFontAwesome6.h"

// Godot lifecycle exports from libgodot.so
extern "C" {
    int ruby_gg_setup(int argc, char** argv);
    int ruby_gg_setup2();
    int ruby_gg_start();
    bool ruby_gg_iteration();
    void ruby_gg_cleanup();
    void* ruby_gg_handler();
    typedef void (*VoidCB)(void);
    typedef void (*IntCB)(int);
    void ruby_gg_set_cb_exit(VoidCB);
    void ruby_gg_set_cb_reset(VoidCB);
    void ruby_gg_set_cb_file(VoidCB);
    void ruby_gg_set_cb_search(VoidCB);
    void ruby_gg_set_cb_menu(IntCB);
    void ruby_gg_set_cb_viewmenu(IntCB);
}

// Swordigo format decoders & tools
std::string g_instance_assets_dir = "assets";
#include "tools/pod_loader.h"
#include "tools/scene_loader.h"
#include "tools/scene_workspace.h"
#include "tools/obj_loader.h"
#include "tools/ani_loader.h"
#include "tools/scene_entity.h"
#include "tools/filerift.h"
#include "tools/intellij.h"
#include "platform/pvr_loader.h"

// Scene Generators (pure logic, no ImGui)
#include "tools/scene_generator.h"
#include "tools/scene_generator_v2.h"
#include "tools/scene_generator_v3.h"
#include "tools/scene_generator_v2_3d.h"

// Batch Converter (headless API)
#include "tools/batch_converter.h"

// ============================================================================
// String Helpers
// ============================================================================
static String S(const char* s) { return String(s ? s : ""); }
static String S(const std::string& s) { return String(s.c_str()); }
static String S(const String& s) { return s; }

static std::string lowerCopy(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return v;
}

// ============================================================================
// Theme Colors (Design tokens)
// ============================================================================
static const Color C_BASE        (0.118f, 0.122f, 0.141f);   // #1e1f24
static const Color C_PANEL       (0.149f, 0.157f, 0.180f);   // #262830
static const Color C_SURFACE     (0.176f, 0.188f, 0.212f);   // #2d3036
static const Color C_BORDER      (0.235f, 0.247f, 0.275f);   // #3c3f46
static const Color C_ACCENT      (0.235f, 0.475f, 0.976f);   // #3c79f9
static const Color C_ACCENT_DIM  (0.165f, 0.310f, 0.612f);   // #2a4f9c
static const Color C_SUCCESS     (0.22f,  0.76f,  0.42f);    // #38c26b
static const Color C_WARNING     (0.96f,  0.78f,  0.25f);    // #f5c740
static const Color C_ERROR       (0.95f,  0.30f,  0.30f);    // #f24d4d
static const Color C_TEXT        (0.918f, 0.933f, 0.961f);   // #eaeeF5
static const Color C_TEXT_DIM    (0.58f,  0.61f,  0.68f);    // #949bae
static const Color C_SELECTION   (0.235f, 0.475f, 0.976f, 0.28f);
static const Color C_HOVER       (1.0f,   1.0f,   1.0f,   0.06f);

// Badge colors per asset type
static const Color C_BADGE_POD   (1.00f, 0.65f, 0.20f);
static const Color C_BADGE_SCENE (0.25f, 0.85f, 0.50f);
static const Color C_BADGE_SCL   (0.35f, 0.88f, 0.72f);
static const Color C_BADGE_TEX   (0.82f, 0.42f, 0.95f);
static const Color C_BADGE_AUDIO (0.95f, 0.40f, 0.40f);
static const Color C_BADGE_FONT  (0.95f, 0.82f, 0.30f);
static const Color C_BADGE_ATLAS (0.65f, 0.48f, 0.95f);
static const Color C_BADGE_CODE  (0.35f, 0.75f, 0.95f);
static const Color C_BADGE_DIR   (0.45f, 0.72f, 1.00f);

// ============================================================================
// StyleBox Helpers
// ============================================================================
static Ref<StyleBoxFlat> makeBox(const Color& bg, float radius = 4.0f, float padV = 5.0f, float padH = 6.0f,
                                  const Color& border = Color(0,0,0,0), int bw = 0) {
    Ref<StyleBoxFlat> s; s.instantiate();
    s->set_bg_color(bg);
    s->set_corner_radius_all((int)radius);
    s->set_content_margin(SIDE_TOP, padV);
    s->set_content_margin(SIDE_BOTTOM, padV);
    s->set_content_margin(SIDE_LEFT, padH);
    s->set_content_margin(SIDE_RIGHT, padH);
    if (bw > 0) { s->set_border_width_all(bw); s->set_border_color(border); }
    return s;
}

static Ref<StyleBoxEmpty> emptyBox() {
    Ref<StyleBoxEmpty> s; s.instantiate();
    return s;
}

// ============================================================================
// Camera Orbit / Pan / Zoom
// ============================================================================
struct EditorCamera {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float dist = 25.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;

    void orbit(float dyaw, float dpitch) {
        yaw += dyaw;
        pitch = CLAMP(pitch + dpitch, -89.0f, 89.0f);
    }
    void pan(float dx, float dy) {
        float r = yaw * (3.14159265f / 180.0f);
        float speed = dist * 0.0018f;
        tx += (-cosf(r) * dx) * speed;
        tz += ( sinf(r) * dx) * speed;
        ty += dy * speed;
    }
    void zoom(float delta) { dist = CLAMP(dist * (1.0f - delta * 0.15f), 0.05f, 50000.0f); }
    void reset() { yaw = 0.0f; pitch = 0.0f; dist = 25.0f; tx = ty = tz = 0.0f; }

    void apply(Camera3D* c) const {
        if (!c) return;
        float r = yaw   * (3.14159265f / 180.0f);
        float p = pitch * (3.14159265f / 180.0f);
        float cx = tx + dist * cosf(p) * sinf(r);
        float cy = ty + dist * sinf(p);
        float cz = tz + dist * cosf(p) * cosf(r);
        c->set_position(Vector3(cx, cy, cz));
        c->look_at(Vector3(tx, ty, tz));
        c->set_perspective(45.0f, 0.01f, 60000.0f);
    }
};

// ============================================================================
// Application State
// ============================================================================
static struct AppState {
    // Workspace & navigation
    std::string workspace;
    std::string cwd;
    std::string selPath;
    std::string selName;
    std::string lastLoadedFile;
    std::string pendingDir;
    std::string pendingFile;
    int pendingSceneObjIndex = -1;

    // Preview state
    int previewType = 0; // 0=None 1=POD 2=Tex 3=Scene 4=Text 5=FNT 6=Atlas
    av::PODModel podModel;
    Ref<ArrayMesh> podMesh;
    Ref<ImageTexture> previewTexture;
    av::SceneData sceneData;
    int selectedSceneObjIndex = -1;

    // Camera
    EditorCamera camera;

    // UI state
    std::string consoleOutput;
    bool showGrid = true;
    bool wireframe = false;
    bool isRunning = true;
    Vector2i lastMousePos = Vector2i(0, 0);
    bool viewportHasFocus = false;

    // Cache
    std::map<std::string, Ref<ArrayMesh>>      meshCache;
    std::map<std::string, Ref<ImageTexture>>   textureCache;

    // Scene Generator State
    int  sgenVersion  = 1;       // 0=V1 1=V2 2=V3 3=V2-3D
    sgen::TerrainOptions    sgenOpt;
    sgen::v2::TerrainOptionsV2 sgenOptV2;
    sgen::v2_3d::TerrainOptions3D sgenOpt3D;
    std::string sgenResult;
    std::string sgenError;
    int  sgenObjects  = 0;
    bool sgenRunning  = false;
    std::string sgenOutputPath;

    // Batch Converter State
    batch::BatchState batchState;
    std::string batchLog;
    double batchWallTime = 0.0;

    // About
    bool aboutVisible = false;

    // Native document editor state. The buffer is always the text shown to the user;
    // binary FileRift assets are decoded on open and recoded only on save.
    std::string editorPath;
    std::string editorFiletype;
    std::string editorBuffer;
    std::string editorDiagnostic;
    bool editorIsFileRift = false;
    bool editorDecodedBinary = false;
    bool editorDirty = false;
    bool editorInternalChange = false;
} App;

// ============================================================================
// UI Widget Handles
// ============================================================================
static Window*              gWindow             = nullptr;
static Label*               gStatusLabel        = nullptr;
static LineEdit*            gPathBar            = nullptr;

// Left — Asset Browser
static Tree*                gFileTree           = nullptr;
static LineEdit*            gSearchEdit         = nullptr;

// Center — tab switcher
static TabContainer*        gCenterTabs         = nullptr;

// Center Tab 0 — Asset Viewer (3D / 2D)
static Control*             gViewerArea         = nullptr;    // parent that holds both
static SubViewportContainer* gViewportContainer = nullptr;
static SubViewport*         gSubViewport        = nullptr;
static Node3D*              gSceneRoot          = nullptr;
static Node3D*              gSceneEntitiesRoot  = nullptr;
static Camera3D*            gCamera3D           = nullptr;
static MeshInstance3D*      gModelInstance      = nullptr;
static MeshInstance3D*      gGridInstance       = nullptr;
static Ref<ImmediateMesh>   gGridMesh;
static TextureRect*         gSceneBackground    = nullptr;

// 2D Texture panel — SEPARATE CONTROL over viewport
static Panel*               gTexPanel           = nullptr;
static TextureRect*         g2DTextureRect      = nullptr;
static Label*               gTexInfoLabel       = nullptr;
static Label*               gTexZoomLabel       = nullptr;
static float                gTexZoom            = 1.0f;

// Center Tab 1 — Scene Generator
static OptionButton*        gSgenVersion        = nullptr;
static OptionButton*        gSgenBiome          = nullptr;
static SpinBox*             gSgenSeed           = nullptr;
static SpinBox*             gSgenWidth          = nullptr;
static SpinBox*             gSgenHeight         = nullptr;
static SpinBox*             gSgenPlatforms      = nullptr;
static SpinBox*             gSgenRoughness      = nullptr;
static SpinBox*             gSgenOctaves        = nullptr;
static CheckBox*            gSgenWater          = nullptr;
static CheckBox*            gSgenTorches        = nullptr;
static CheckBox*            gSgenMountains      = nullptr;
static CheckBox*            gSgenIslands        = nullptr;
static CheckBox*            gSgenPortal         = nullptr;
static LineEdit*            gSgenOutputPath     = nullptr;
static Label*               gSgenResultLabel    = nullptr;
static Button*              gSgenGenerateBtn    = nullptr;

// Center Tab 2 — Batch Converter
static LineEdit*            gBatchSrcDir        = nullptr;
static LineEdit*            gBatchDstDir        = nullptr;
static OptionButton*        gBatchMode          = nullptr;
static OptionButton*        gBatchFmt           = nullptr;
static CheckBox*            gBatchRecurse       = nullptr;
static CheckBox*            gBatchSkip          = nullptr;
static ProgressBar*         gBatchProgress      = nullptr;
static TextEdit*            gBatchLog           = nullptr;
static Label*               gBatchStatusLabel   = nullptr;
static Button*              gBatchStartBtn      = nullptr;
static Button*              gBatchCancelBtn     = nullptr;

// Right — Inspector
static Tree*                gInspectorTree      = nullptr;
static Tree*                gSceneTree          = nullptr;
static Tree*                gPropsTree          = nullptr;

// Bottom
static TextEdit*            gLogTextEdit        = nullptr;
static CodeEdit*             gCodeEdit          = nullptr;
static LineEdit*             gEditorSearch      = nullptr;
static LineEdit*             gEditorReplace     = nullptr;
static Label*                gEditorStatus      = nullptr;
static Label*                gEditorDocument    = nullptr;
static Button*               gEditorSaveBtn     = nullptr;
static intel::IntelliJ       gEditorIntelliJ;

class RubyTitleBar : public Control {
    GDCLASS(RubyTitleBar, Control);
    Window* owner = nullptr;

protected:
    static void _bind_methods() {}

    void gui_input(const Ref<InputEvent> &event) override {
        Ref<InputEventMouseButton> button = event;
        if (button.is_valid() && button->is_pressed() &&
            button->get_button_index() == MouseButton::LEFT && owner) {
            owner->start_drag();
            accept_event();
        }
    }

public:
    explicit RubyTitleBar(Window *p_owner) : owner(p_owner) {}
};

class RubyWindowButton : public Button {
    GDCLASS(RubyWindowButton, Button);
    Window* owner = nullptr;
    int action = 0;

protected:
    static void _bind_methods() {}
    void pressed() override {
        if (!owner) return;
        if (action == 0) owner->hide();
        else if (action == 1) {
            owner->set_mode(owner->get_mode() == Window::MODE_MAXIMIZED ?
                Window::MODE_WINDOWED : Window::MODE_MAXIMIZED);
        } else if (action == 2) owner->set_mode(Window::MODE_MINIMIZED);
    }

public:
    RubyWindowButton(Window *p_owner, int p_action) : owner(p_owner), action(p_action) {}
};

// ============================================================================
// Logger
// ============================================================================
static void app_log(const char* msg) {
    if (!msg) return;
    App.consoleOutput += std::string(msg) + "\n";
    fprintf(stderr, "[ruby_gg] %s\n", msg);
    if (gLogTextEdit) {
        gLogTextEdit->set_text(S(App.consoleOutput));
        gLogTextEdit->set_caret_line(gLogTextEdit->get_line_count() - 1);
    }
}

// ============================================================================
// Texture Decoder & Cache
// ============================================================================
static bool decodeTexContainer(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
    gzFile file = gzopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t header[3] = {};
    if (gzread(file, header, sizeof(header)) != (int)sizeof(header)) { gzclose(file); return false; }
    const uint32_t type = header[0], width = header[1], height = header[2];
    if (!width || !height || width > 32768 || height > 32768 ||
        (type != 1 && type != 3 && type != 5)) { gzclose(file); return false; }
    const int bpp = type == 1 ? 4 : 2;
    std::vector<uint8_t> src((size_t)width * height * bpp);
    const int got = gzread(file, src.data(), (unsigned int)src.size());
    gzclose(file);
    if (got != (int)src.size()) return false;
    rgba.resize((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        if (type == 1) { memcpy(&rgba[i*4], &src[i*4], 4); }
        else {
            const uint16_t v = (uint16_t)src[i*2] | ((uint16_t)src[i*2+1] << 8);
            if (type == 3) {
                rgba[i*4+0] = (uint8_t)(((v>>12)&15)*17);
                rgba[i*4+1] = (uint8_t)(((v>>8)&15)*17);
                rgba[i*4+2] = (uint8_t)(((v>>4)&15)*17);
                rgba[i*4+3] = (uint8_t)((v&15)*17);
            } else {
                rgba[i*4+0] = (uint8_t)(((v>>11)&31)*255/31);
                rgba[i*4+1] = (uint8_t)(((v>>5)&63)*255/63);
                rgba[i*4+2] = (uint8_t)((v&31)*255/31);
                rgba[i*4+3] = 255;
            }
        }
    }
    w = (int)width; h = (int)height;
    return true;
}

static Ref<ImageTexture> loadTextureFile(const std::string& path) {
    if (path.empty()) return Ref<ImageTexture>();
    const std::string key = std::filesystem::path(path).lexically_normal().string();
    auto it = App.textureCache.find(key);
    if (it != App.textureCache.end()) return it->second;

    const std::string low = lowerCopy(path);
    Ref<Image> img;
    if ((low.size() >= 8 && low.compare(low.size()-8, 8, ".tex.png") == 0) ||
        (low.size() >= 4 && low.compare(low.size()-4, 4, ".tex") == 0)) {
        std::vector<uint8_t> rgba; int w=0, h=0;
        if (decodeTexContainer(path, rgba, w, h)) {
            PackedByteArray bytes; bytes.resize(rgba.size());
            memcpy(bytes.ptrw(), rgba.data(), rgba.size());
            img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
        }
    } else if (low.size() >= 4 && low.compare(low.size()-4, 4, ".pvr") == 0) {
        FILE* fp = fopen(path.c_str(), "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END); const long fsz = ftell(fp); fseek(fp, 0, SEEK_SET);
            std::vector<uint8_t> raw(fsz > 0 ? (size_t)fsz : 0);
            if (!raw.empty() && fread(raw.data(), 1, raw.size(), fp) == raw.size()) {
                std::vector<uint8_t> rgba; int w=0, h=0;
                if (pvr_decode_to_rgba(raw.data(), raw.size(), rgba, w, h) && w > 0 && h > 0) {
                    PackedByteArray bytes; bytes.resize(rgba.size());
                    memcpy(bytes.ptrw(), rgba.data(), rgba.size());
                    img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
                }
            }
            fclose(fp);
        }
    } else {
        img = Image::load_from_file(S(path));
    }
    if (img.is_null() || img->is_empty()) { app_log(("Texture decode failed: " + path).c_str()); return Ref<ImageTexture>(); }
    Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
    if (tex.is_valid()) App.textureCache[key] = tex;
    return tex;
}

static Ref<ImageTexture> loadTexture2DPreview(const std::string& path) {
    if (path.empty()) return Ref<ImageTexture>();
    const std::string key = std::filesystem::path(path).lexically_normal().string() + ":2d";
    auto it = App.textureCache.find(key);
    if (it != App.textureCache.end()) return it->second;

    const std::string low = lowerCopy(path);
    Ref<Image> img;
    bool isContainer = false;
    if ((low.size() >= 8 && low.compare(low.size()-8, 8, ".tex.png") == 0) ||
        (low.size() >= 4 && low.compare(low.size()-4, 4, ".tex") == 0)) {
        std::vector<uint8_t> rgba; int w=0, h=0;
        if (decodeTexContainer(path, rgba, w, h)) {
            PackedByteArray bytes; bytes.resize(rgba.size());
            memcpy(bytes.ptrw(), rgba.data(), rgba.size());
            img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
            isContainer = true;
        }
    } else if (low.size() >= 4 && low.compare(low.size()-4, 4, ".pvr") == 0) {
        FILE* fp = fopen(path.c_str(), "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END); const long fsz = ftell(fp); fseek(fp, 0, SEEK_SET);
            std::vector<uint8_t> raw(fsz > 0 ? (size_t)fsz : 0);
            if (!raw.empty() && fread(raw.data(), 1, raw.size(), fp) == raw.size()) {
                std::vector<uint8_t> rgba; int w=0, h=0;
                if (pvr_decode_to_rgba(raw.data(), raw.size(), rgba, w, h) && w > 0 && h > 0) {
                    PackedByteArray bytes; bytes.resize(rgba.size());
                    memcpy(bytes.ptrw(), rgba.data(), rgba.size());
                    img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
                    isContainer = true;
                }
            }
            fclose(fp);
        }
    } else {
        img = Image::load_from_file(S(path));
    }
    if (img.is_null() || img->is_empty()) { app_log(("Texture 2D decode failed: " + path).c_str()); return Ref<ImageTexture>(); }
    if (isContainer) {
        // Swordigo PVR and .tex containers store pixel rows inverted / rotated 180 degrees.
        // In 2D display space (matching Ruby ImGui's tex_edit_flip_buffer_hv and st.texture_flip_h/v = true),
        // flip both X and Y so the texture displays upright and correctly oriented!
        img->flip_x();
        img->flip_y();
    }
    Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
    if (tex.is_valid()) App.textureCache[key] = tex;
    return tex;
}

static std::string canonicalTexBase(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::string low = lowerCopy(name);
    const char* suffixes[] = {".tex.png", ".pvr", ".tex", ".png", ".jpg", ".jpeg"};
    for (const char* suf : suffixes) {
        const size_t n = strlen(suf);
        if (low.size() >= n && low.compare(low.size()-n, n, suf) == 0) { name.resize(name.size()-n); break; }
    }
    if (name.size() >= 3 && lowerCopy(name.substr(name.size()-3)) == "_2x") name.resize(name.size()-3);
    return name;
}

static Ref<ImageTexture> findTexture(const std::string& assetName, const std::string& ownerPath = "") {
    if (assetName.empty()) return Ref<ImageTexture>();
    namespace fs = std::filesystem;
    std::string norm = assetName; std::replace(norm.begin(), norm.end(), '\\', '/');
    fs::path named(norm);
    const std::string base = canonicalTexBase(named.filename().string());
    const char* suffixes[] = {"_2x.tex.png", ".tex.png", "_2x.pvr", ".pvr", "_2x.tex", ".tex", "_2x.png", ".png", ".jpg", ".jpeg"};

    std::vector<fs::path> dirs;
    if (!ownerPath.empty()) {
        fs::path od = fs::path(ownerPath).parent_path();
        dirs.push_back(od / named.parent_path());
        dirs.push_back(od);
        dirs.push_back(od / "resources");
        dirs.push_back(od.parent_path() / "resources");
    }
    dirs.push_back(fs::path(App.cwd));
    dirs.push_back(fs::path(App.cwd) / "resources");
    dirs.push_back(fs::path(App.workspace) / "assets" / "resources");
    dirs.push_back(fs::path(App.workspace) / "rln_assets" / "resources");

    std::set<std::string> seen;
    for (const fs::path& dir : dirs) {
        for (const char* suf : suffixes) {
            std::string cand = (dir / (base + suf)).lexically_normal().string();
            if (!seen.insert(cand).second) continue;
            if (FileAccess::exists(S(cand))) {
                Ref<ImageTexture> t = loadTextureFile(cand);
                if (t.is_valid()) return t;
            }
        }
    }
    return Ref<ImageTexture>();
}

static std::vector<Ref<ImageTexture>> resolvePodTextures(const av::PODModel& pod, const std::string& modelPath) {
    std::vector<Ref<ImageTexture>> textures;
    textures.reserve(pod.texture_filenames.size());
    for (const std::string& n : pod.texture_filenames) textures.push_back(findTexture(n, modelPath));
    const bool noneLoaded = std::none_of(textures.begin(), textures.end(), [](const Ref<ImageTexture>& t){return t.is_valid();});
    if (textures.empty() || noneLoaded) {
        Ref<ImageTexture> fb = findTexture(std::filesystem::path(modelPath).stem().string(), modelPath);
        if (textures.empty()) textures.push_back(fb);
        else if (fb.is_valid()) textures[0] = fb;
    }
    return textures;
}

// ============================================================================
// POD -> Godot ArrayMesh
// ============================================================================
static Vector3 transformPoint(const float m[16], const Vector3& p) {
    return Vector3(m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
                   m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
                   m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]);
}
static Vector3 transformNormal(const float m[16], const Vector3& n) {
    return Vector3(m[0]*n.x+m[4]*n.y+m[8]*n.z,
                   m[1]*n.x+m[5]*n.y+m[9]*n.z,
                   m[2]*n.x+m[6]*n.y+m[10]*n.z).normalized();
}

static Ref<StandardMaterial3D> podMaterial(const av::PODModel& pod, int matIdx,
                                            const std::vector<Ref<ImageTexture>>& textures) {
    Ref<StandardMaterial3D> mat; mat.instantiate();
    mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
    mat->set_diffuse_mode(BaseMaterial3D::DIFFUSE_LAMBERT);
    mat->set_specular_mode(BaseMaterial3D::SPECULAR_SCHLICK_GGX);
    mat->set_roughness(0.40f);
    mat->set_specular(0.35f);
    mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_PER_PIXEL);
    Color col(0.95f, 0.95f, 0.95f, 1.0f);
    float opacity = 1.0f;
    int texIdx = -1;
    if (matIdx >= 0 && matIdx < (int)pod.materials.size()) {
        const av::PODMaterial& pm = pod.materials[matIdx];
        col = Color(pm.diffuse[0], pm.diffuse[1], pm.diffuse[2], pm.opacity);
        opacity = pm.opacity;
        texIdx = pm.diffuse_texture_index;
        if (pm.opacity < 0.999f) mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
    }
    Ref<ImageTexture> tex;
    if (texIdx >= 0 && texIdx < (int)textures.size()) tex = textures[texIdx];
    if (tex.is_null()) {
        auto f = std::find_if(textures.begin(), textures.end(), [](const Ref<ImageTexture>& t){return t.is_valid();});
        if (f != textures.end()) tex = *f;
    }
    if (tex.is_valid()) {
        mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tex);
        mat->set_albedo(Color(1.0f, 1.0f, 1.0f, opacity));
    } else {
        mat->set_albedo(col);
    }
    return mat;
}

static void addPodSurface(const av::PODModel& pod, Ref<ArrayMesh>& mesh, const av::PODMesh& src,
                           const float* xf, int matIdx, const std::vector<Ref<ImageTexture>>& textures) {
    if (src.positions.empty()) return;
    Array a; a.resize(Mesh::ARRAY_MAX);
    PackedVector3Array verts; verts.resize(src.positions.size()/3);
    for (int i = 0; i < verts.size(); ++i) {
        Vector3 p(src.positions[i*3], src.positions[i*3+1], src.positions[i*3+2]);
        verts.set(i, xf ? transformPoint(xf, p) : p);
    }
    a[Mesh::ARRAY_VERTEX] = verts;

    std::vector<float> computed_normals;
    const float* nptr = nullptr;
    if (!src.normals.empty()) {
        nptr = src.normals.data();
    } else if (!src.positions.empty() && !src.indices.empty()) {
        swk::compute_smooth_normals(src.positions, src.indices, computed_normals);
        if (!computed_normals.empty()) nptr = computed_normals.data();
    }

    if (nptr) {
        PackedVector3Array norms; norms.resize(src.positions.size()/3);
        for (int i = 0; i < norms.size(); ++i) {
            Vector3 n(nptr[i*3], nptr[i*3+1], nptr[i*3+2]);
            norms.set(i, xf ? transformNormal(xf, n) : n);
        }
        a[Mesh::ARRAY_NORMAL] = norms;
    }
    if (!src.uvs.empty()) {
        PackedVector2Array uv; uv.resize(src.uvs.size()/2);
        for (int i = 0; i < uv.size(); ++i) uv.set(i, Vector2(src.uvs[i*2], 1.0f - src.uvs[i*2+1]));
        a[Mesh::ARRAY_TEX_UV] = uv;
    }
    if (!src.indices.empty()) {
        PackedInt32Array idx; idx.resize(src.indices.size());
        for (int i = 0; i < idx.size(); ++i) idx.set(i, (int32_t)src.indices[i]);
        a[Mesh::ARRAY_INDEX] = idx;
    }
    const Mesh::PrimitiveType prim = src.mesh_type == 2 ? Mesh::PRIMITIVE_LINES : Mesh::PRIMITIVE_TRIANGLES;
    mesh->add_surface_from_arrays(prim, a);
    mesh->surface_set_material(mesh->get_surface_count()-1, podMaterial(pod, matIdx, textures));
}

static Ref<ArrayMesh> podToMesh(const av::PODModel& pod, const std::string& modelPath = "") {
    Ref<ArrayMesh> mesh; mesh.instantiate();
    const auto textures = resolvePodTextures(pod, modelPath);
    bool emitted = false;
    const int nc = pod.num_mesh_nodes > 0 ? std::min(pod.num_mesh_nodes, (int)pod.nodes.size()) : (int)pod.nodes.size();
    for (int ni = 0; ni < nc; ++ni) {
        const av::PODNode& node = pod.nodes[ni];
        if (node.object_index < 0 || node.object_index >= (int)pod.meshes.size()) continue;
        float nm[16]; av::get_node_matrix(pod, ni, 0.0f, nm);
        if (pod.has_center_point) { nm[12]-=pod.center_point[0]; nm[13]-=pod.center_point[1]; nm[14]-=pod.center_point[2]; }
        addPodSurface(pod, mesh, pod.meshes[node.object_index], nm, node.material_index, textures);
        emitted = true;
    }
    if (!emitted) {
        for (const av::PODMesh& m : pod.meshes) addPodSurface(pod, mesh, m, nullptr, -1, textures);
    }
    return mesh;
}

static Ref<ArrayMesh> groundMeshToArrayMesh(const av::PODMesh& pm, const std::string& texName="", const std::string& ownerPath="") {
    Ref<ArrayMesh> m; m.instantiate();
    Array a; a.resize(Mesh::ARRAY_MAX);
    if (!pm.positions.empty()) {
        PackedVector3Array v; v.resize(pm.positions.size()/3);
        for (int i=0; i<v.size(); i++) v.set(i, Vector3(pm.positions[i*3], pm.positions[i*3+1], pm.positions[i*3+2]));
        a[Mesh::ARRAY_VERTEX] = v;
    }

    std::vector<float> computed_normals;
    const float* nptr = nullptr;
    if (!pm.normals.empty()) {
        nptr = pm.normals.data();
    } else if (!pm.positions.empty() && !pm.indices.empty()) {
        swk::compute_smooth_normals(pm.positions, pm.indices, computed_normals);
        if (!computed_normals.empty()) nptr = computed_normals.data();
    }

    if (nptr) {
        PackedVector3Array n; n.resize(pm.positions.size()/3);
        for (int i=0; i<n.size(); i++) n.set(i, Vector3(nptr[i*3], nptr[i*3+1], nptr[i*3+2]));
        a[Mesh::ARRAY_NORMAL] = n;
    }
    if (!pm.uvs.empty()) {
        PackedVector2Array uv; uv.resize(pm.uvs.size()/2);
        for (int i=0; i<uv.size(); i++) uv.set(i, Vector2(pm.uvs[i*2], 1.0f - pm.uvs[i*2+1]));
        a[Mesh::ARRAY_TEX_UV] = uv;
    }
    if (!pm.indices.empty()) {
        PackedInt32Array idx; idx.resize(pm.indices.size());
        for (int i=0; i<idx.size(); i++) idx.set(i, pm.indices[i]);
        a[Mesh::ARRAY_INDEX] = idx;
    }
    m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, a);
    Ref<StandardMaterial3D> mat; mat.instantiate();
    mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
    mat->set_diffuse_mode(BaseMaterial3D::DIFFUSE_LAMBERT);
    mat->set_specular_mode(BaseMaterial3D::SPECULAR_SCHLICK_GGX);
    mat->set_roughness(0.55f);
    mat->set_specular(0.25f);
    mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_PER_PIXEL);
    Ref<ImageTexture> tex = findTexture(texName, ownerPath);
    if (tex.is_valid()) { mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tex); mat->set_albedo(Color(1,1,1,1)); }
    else mat->set_albedo(Color(0.55f, 0.72f, 0.50f));
    m->surface_set_material(0, mat);
    return m;
}

static Transform3D mat4ToTransform(const float m[16]) {
    Basis b(Vector3(m[0],m[1],m[2]), Vector3(m[4],m[5],m[6]), Vector3(m[8],m[9],m[10]));
    return Transform3D(b, Vector3(m[12],m[13],m[14]));
}

// ============================================================================
// 3D Grid
// ============================================================================
static void buildGrid() {
    gGridMesh.instantiate();
    gGridInstance = memnew(MeshInstance3D);
    gGridInstance->set_mesh(Ref<Mesh>(gGridMesh));
    gSceneRoot->add_child(gGridInstance);

    gGridMesh->clear_surfaces();
    gGridMesh->surface_begin(Mesh::PRIMITIVE_LINES);
    for (float i = -100.0f; i <= 100.0f; i += 2.0f) {
        Color c = ((int)i % 10 == 0) ? Color(0.32f,0.34f,0.40f) : Color(0.19f,0.20f,0.23f);
        gGridMesh->surface_set_color(c); gGridMesh->surface_add_vertex(Vector3(i, 0, -100));
        gGridMesh->surface_set_color(c); gGridMesh->surface_add_vertex(Vector3(i, 0,  100));
        gGridMesh->surface_set_color(c); gGridMesh->surface_add_vertex(Vector3(-100, 0, i));
        gGridMesh->surface_set_color(c); gGridMesh->surface_add_vertex(Vector3( 100, 0, i));
    }
    // Axes
    Color cx(0.90f,0.28f,0.28f), cy(0.28f,0.90f,0.28f), cz(0.28f,0.58f,0.90f);
    gGridMesh->surface_set_color(cx); gGridMesh->surface_add_vertex(Vector3(0,0,0));
    gGridMesh->surface_set_color(cx); gGridMesh->surface_add_vertex(Vector3(20,0,0));
    gGridMesh->surface_set_color(cy); gGridMesh->surface_add_vertex(Vector3(0,0,0));
    gGridMesh->surface_set_color(cy); gGridMesh->surface_add_vertex(Vector3(0,20,0));
    gGridMesh->surface_set_color(cz); gGridMesh->surface_add_vertex(Vector3(0,0,0));
    gGridMesh->surface_set_color(cz); gGridMesh->surface_add_vertex(Vector3(0,0,20));
    gGridMesh->surface_end();
}

// ============================================================================
// Theme Application
// ============================================================================
static void applyTheme(Window* w) {
    auto t = Ref<Theme>(memnew(Theme));

    // Load UI fonts & FontAwesome 6 icon glyph fallback
    Ref<FontFile> faFont;
    Ref<FontFile> mainFont;
    /* Font search paths: check source tree, binary location, user data dir,
     * and common install locations. Binary-relative paths are important
     * when running from bin/ without the full source tree. */
    const char* home = getenv("HOME");
    std::string user_dir = home ? (std::string(home) + "/.local/share/swordigo-desktop/") : "";
    const std::vector<std::string> fontSearchPaths = {
        std::string(App.cwd) + "/src/assets/fonts",
        std::string(App.workspace) + "/src/assets/fonts",
        "src/assets/fonts",
        "../src/assets/fonts",
        /* Binary-relative: dev root */
        "../src/assets/fonts",
        "assets/fonts",
        /* User data dir (runtime assets installed here) */
        user_dir + "src/assets/fonts",
        user_dir + "launcher/fonts",
        user_dir + "launcher/fontawesome/otfs",
        /* System install */
        "/usr/share/swordigo-desktop/src/assets/fonts",
        "/usr/local/share/swordigo-desktop/src/assets/fonts",
        "/usr/share/swordigo-desktop/launcher/fonts",
        /* Hardcoded dev fallback */
        "/home/quantumcreeper/SwordigoDesktop/src/assets/fonts"
    };
    /* FA font candidates: FA6 TTF, FA7 OTF Solid, FA7 OTF Regular */
    static const char* fa_names[] = {
        "/fa-solid-900.ttf",
        "/Font Awesome 7 Free-Solid-900.otf",
        "/Font Awesome 6 Free-Solid-900.otf",
        "/fa-solid-900.otf",
    };
    for (const auto& base : fontSearchPaths) {
        for (const char* fa_name : fa_names) {
            if (faFont.is_null() && FileAccess::exists(S(base + fa_name))) {
                faFont.instantiate();
                faFont->load_dynamic_font(S(base + fa_name));
                break;
            }
        }
        if (mainFont.is_null() && FileAccess::exists(S(base + "/Inter-Regular.ttf"))) {
            mainFont.instantiate();
            mainFont->load_dynamic_font(S(base + "/Inter-Regular.ttf"));
        }
    }
    if (mainFont.is_null()) {
        mainFont.instantiate();
    }
    if (faFont.is_valid()) {
        TypedArray<Font> fallbacks;
        fallbacks.push_back(faFont);
        mainFont->set_fallbacks(fallbacks);
    }
    t->set_default_font(mainFont);
    t->set_default_font_size(13);

    // Panel / Container backgrounds
    t->set_stylebox("panel",           "PanelContainer",  makeBox(C_PANEL,   0, 5, 5, C_BORDER, 1));
    t->set_stylebox("panel",           "Panel",           makeBox(C_SURFACE, 0, 4, 4));
    t->set_stylebox("panel",           "TabContainer",    makeBox(C_BASE,    0, 0, 0));
    t->set_stylebox("panel",           "ScrollContainer", makeBox(C_BASE,    0, 0, 0));

    // Tabs
    t->set_stylebox("tab_selected",    "TabContainer",    makeBox(C_SURFACE,  3, 6, 12, C_ACCENT,    1));
    t->set_stylebox("tab_unselected",  "TabContainer",    makeBox(C_BASE,     3, 6, 12, C_BORDER,    1));
    t->set_stylebox("tab_hovered",     "TabContainer",    makeBox(C_PANEL,    3, 6, 12, C_ACCENT_DIM, 1));
    t->set_stylebox("tabbar_background","TabContainer",   makeBox(C_BASE,     0, 2, 2));

    // Buttons
    t->set_stylebox("normal",  "Button", makeBox(C_SURFACE, 4, 6, 10, C_BORDER,     1));
    t->set_stylebox("hover",   "Button", makeBox(C_PANEL,   4, 6, 10, C_ACCENT_DIM, 1));
    t->set_stylebox("pressed", "Button", makeBox(C_ACCENT,  4, 6, 10));
    t->set_stylebox("focus",   "Button", makeBox(C_SURFACE, 4, 6, 10, C_ACCENT,     1));

    // LineEdit
    t->set_stylebox("normal", "LineEdit", makeBox(Color(0.09f,0.10f,0.12f), 4, 5, 8, C_BORDER, 1));
    t->set_stylebox("focus",  "LineEdit", makeBox(Color(0.09f,0.10f,0.12f), 4, 5, 8, C_ACCENT, 1));

    // TextEdit
    t->set_stylebox("normal", "TextEdit", makeBox(Color(0.09f,0.10f,0.12f), 3, 4, 6, C_BORDER, 1));
    t->set_stylebox("focus",  "TextEdit", makeBox(Color(0.09f,0.10f,0.12f), 3, 4, 6, C_ACCENT, 1));

    // Tree
    t->set_stylebox("panel",           "Tree",   makeBox(C_BASE, 0, 0, 0));
    t->set_stylebox("selected",        "Tree",   makeBox(C_SELECTION, 0, 2, 2));
    t->set_stylebox("selected_focus",  "Tree",   makeBox(C_SELECTION, 0, 2, 2));
    t->set_color("title_button_color", "Tree",   C_TEXT_DIM);
    t->set_color("children_hl_color", "Tree",    Color(C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 0.10f));

    // MenuBar
    t->set_stylebox("panel",           "MenuBar",   makeBox(C_BASE, 0, 2, 4));
    t->set_stylebox("panel",           "PopupMenu", makeBox(Color(0.13f,0.14f,0.17f), 4, 4, 4, C_BORDER, 1));
    t->set_stylebox("hover",           "PopupMenu", makeBox(C_ACCENT, 3, 4, 4));

    // SpinBox / OptionButton
    t->set_stylebox("normal",  "OptionButton", makeBox(C_SURFACE, 4, 5, 8, C_BORDER, 1));
    t->set_stylebox("hover",   "OptionButton", makeBox(C_PANEL,   4, 5, 8, C_ACCENT_DIM, 1));
    t->set_stylebox("pressed", "OptionButton", makeBox(C_ACCENT,  4, 5, 8));

    // CheckBox
    t->set_stylebox("normal",  "CheckBox", emptyBox());

    // ProgressBar
    t->set_stylebox("background", "ProgressBar", makeBox(C_BASE,   3, 4, 4, C_BORDER, 1));
    t->set_stylebox("fill",       "ProgressBar", makeBox(C_ACCENT, 3, 4, 4));

    // Separator
    t->set_stylebox("separator", "HSeparator", makeBox(C_BORDER, 0, 1, 0));
    t->set_stylebox("separator", "VSeparator", makeBox(C_BORDER, 0, 0, 1));

    // Fonts & Colors
    t->set_color("font_color",          "Label",        C_TEXT);
    t->set_color("font_color",          "Button",       C_TEXT);
    t->set_color("font_hover_color",    "Button",       Color(1,1,1));
    t->set_color("font_pressed_color",  "Button",       Color(1,1,1));
    t->set_color("font_color",          "Tree",         C_TEXT);
    t->set_color("font_color",          "LineEdit",     C_TEXT);
    t->set_color("font_color",          "TextEdit",     C_TEXT);
    t->set_color("font_color",          "OptionButton", C_TEXT);
    t->set_color("font_color",          "CheckBox",     C_TEXT);
    t->set_color("font_color",          "MenuBar",      C_TEXT);
    t->set_color("font_selected_color", "LineEdit",     C_TEXT);
    t->set_color("selection_color",     "LineEdit",     C_SELECTION);
    t->set_color("selection_color",     "TextEdit",     C_SELECTION);
    t->set_color("caret_color",         "LineEdit",     C_ACCENT);
    t->set_color("caret_color",         "TextEdit",     C_ACCENT);

    // Sizes
    t->set_constant("separation", "HBoxContainer", 6);
    t->set_constant("separation", "VBoxContainer", 4);

    w->set_theme(t);
    w->set_min_size(Vector2i(1280, 720));
}

// ============================================================================
// Scene Graph manipulation helpers
// ============================================================================
static void clearSceneEntities() {
    if (gSceneBackground) { gSceneBackground->set_texture(Ref<Texture2D>()); gSceneBackground->set_visible(false); }
    if (!gSceneEntitiesRoot) return;
    while (gSceneEntitiesRoot->get_child_count() > 0) {
        Node* child = gSceneEntitiesRoot->get_child(0);
        gSceneEntitiesRoot->remove_child(child);
        memdelete(child);
    }
}

static void setViewMode(bool texMode) {
    // The texture panel sits inside gViewerArea at z-index above viewport
    if (gViewportContainer) gViewportContainer->set_visible(!texMode);
    if (gTexPanel)          gTexPanel->set_visible(texMode);
    if (gSceneBackground)   gSceneBackground->set_visible(!texMode);
}

static void show2DTexture(Ref<ImageTexture> tex, const std::string& name) {
    setViewMode(true);
    // Reset zoom
    gTexZoom = 1.0f;
    if (g2DTextureRect && tex.is_valid()) {
        g2DTextureRect->set_texture(tex);
        // Explicit 1:1 pixel size so it isn't stretched
        g2DTextureRect->set_custom_minimum_size(Vector2(tex->get_width(), tex->get_height()));
        g2DTextureRect->set_stretch_mode(TextureRect::STRETCH_KEEP);
        g2DTextureRect->set_expand_mode(TextureRect::EXPAND_KEEP_SIZE);
    } else if (g2DTextureRect) {
        g2DTextureRect->set_texture(Ref<Texture2D>());
    }
    if (gTexInfoLabel && tex.is_valid()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "  %s   |   %d × %d px   |   RGBA8 decoded   |   Zoom: 100%%",
                 name.c_str(), tex->get_width(), tex->get_height());
        gTexInfoLabel->set_text(S(buf));
    }
}

// ============================================================================
// 3D Scene Reconstruction
// ============================================================================
static void buildSceneRepresentation() {
    setViewMode(false);
    clearSceneEntities();
    if (App.sceneData.objects.empty()) return;

    const std::string scenePath = App.sceneData.filepath.empty() ? App.selPath : App.sceneData.filepath;

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (size_t oi = 0; oi < App.sceneData.objects.size(); ++oi) {
        const auto& obj = App.sceneData.objects[oi];
        if (obj.hidden) continue;
        if (obj.pos_x < minX) minX = obj.pos_x; if (obj.pos_x > maxX) maxX = obj.pos_x;
        if (obj.pos_y < minY) minY = obj.pos_y; if (obj.pos_y > maxY) maxY = obj.pos_y;

        float wm[16], rm[16];
        swk::object_world_matrix(obj, wm);
        swk::object_render_matrix(obj, rm);
        const Transform3D wt = mat4ToTransform(wm);

        // Ground meshes
        for (size_t gi = 0; gi < obj.ground_meshes.size(); ++gi) {
            const std::string& texN = gi < obj.ground_mesh_textures.size() ? obj.ground_mesh_textures[gi] : "";
            Ref<ArrayMesh> gm = groundMeshToArrayMesh(obj.ground_meshes[gi], texN, scenePath);
            auto* mi = memnew(MeshInstance3D);
            mi->set_mesh(gm); mi->set_transform(wt);
            gSceneEntitiesRoot->add_child(mi);
        }
        // Background
        if (!obj.background_name.empty() && gSceneBackground && !gSceneBackground->is_visible()) {
            Ref<ImageTexture> bt = findTexture(obj.background_name, scenePath);
            if (bt.is_valid()) { gSceneBackground->set_texture(bt); gSceneBackground->set_visible(true); }
        }
        // POD model
        if (!obj.mesh_name.empty()) {
            std::string podPath;
            std::filesystem::path nm(obj.mesh_name);
            if (!nm.has_extension()) nm += ".POD";
            std::filesystem::path sdir = std::filesystem::path(scenePath).parent_path();
            for (const auto& cand : {sdir/nm, sdir/"resources"/nm.filename(),
                                     sdir.parent_path()/"resources"/nm.filename(),
                                     std::filesystem::path(App.workspace)/"assets"/"resources"/nm.filename()}) {
                if (FileAccess::exists(S(cand.string()))) { podPath = cand.lexically_normal().string(); break; }
            }
            if (!podPath.empty()) {
                const std::string mk = lowerCopy(podPath);
                Ref<ArrayMesh> meshRef;
                auto cached = App.meshCache.find(mk);
                if (cached != App.meshCache.end()) meshRef = cached->second;
                else {
                    av::PODModel pm = av::pod_load(podPath, obj.mesh_name);
                    if (pm.total_vertices > 0) { meshRef = podToMesh(pm, podPath); App.meshCache[mk] = meshRef; }
                }
                if (meshRef.is_valid()) {
                    auto* mi = memnew(MeshInstance3D);
                    mi->set_mesh(meshRef); mi->set_transform(mat4ToTransform(rm));
                    gSceneEntitiesRoot->add_child(mi);
                }
            }
        }
    }
    // Water
    for (size_t wi = 0; wi < App.sceneData.waters.size(); ++wi) {
        const auto& water = App.sceneData.waters[wi];
        if (water.object_index < 0 || water.object_index >= (int)App.sceneData.objects.size()) continue;
        const auto& ow = App.sceneData.objects[water.object_index];
        if (ow.hidden) continue;
        float wm[16]; swk::object_world_matrix(ow, wm);
        av::PODMesh quad;
        const float x=water.rect[0], y=water.rect[1], ww=water.rect[2], wh=water.rect[3];
        quad.positions = {x,y,0, x+ww,y,0, x+ww,y+wh,0, x,y+wh,0};
        quad.normals   = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
        const float tile=water.tile_size > 0.001f ? water.tile_size : 64.0f;
        const float u0=water.tex_offset[0]/tile, v0=water.tex_offset[1]/tile;
        quad.uvs    = {u0,v0, u0+ww/tile,v0, u0+ww/tile,v0+wh/tile, u0,v0+wh/tile};
        quad.indices= {0,1,2, 0,2,3}; quad.num_vertices=4; quad.num_faces=2;
        Ref<ArrayMesh> wm2 = groundMeshToArrayMesh(quad, water.texture, scenePath);
        Ref<StandardMaterial3D> wmat = wm2->surface_get_material(0);
        if (wmat.is_valid()) {
            wmat->set_albedo(Color(water.front_color[0], water.front_color[1], water.front_color[2], water.front_color[3]));
            wmat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
        }
        auto* mi = memnew(MeshInstance3D);
        mi->set_mesh(wm2); mi->set_transform(mat4ToTransform(wm));
        gSceneEntitiesRoot->add_child(mi);
    }
    // Dynamic Scene Point Lights (OmniLight3D)
    for (const auto& light : App.sceneData.lights) {
        if (light.type == 3 || light.type == 1) {
            auto* pl = memnew(OmniLight3D);
            pl->set_position(Vector3(light.pos[0], light.pos[1], light.pos[2]));
            pl->set_color(Color(light.color[0], light.color[1], light.color[2]));
            pl->set_param(Light3D::PARAM_ENERGY, std::max(0.4f, light.intensity * 1.6f));
            pl->set_param(Light3D::PARAM_RANGE, light.radius > 0 ? light.radius : 350.0f);
            pl->set_param(Light3D::PARAM_ATTENUATION, 1.15f);
            gSceneEntitiesRoot->add_child(pl);
        }
    }
    // Auto-frame camera
    if (maxX > minX && maxY > minY) {
        App.camera.tx = (minX+maxX)*0.5f; App.camera.ty = (minY+maxY)*0.5f; App.camera.tz = 0;
        float span = std::max({maxX-minX, maxY-minY, 20.0f});
        App.camera.dist  = span * 1.05f;
        App.camera.yaw   = 0.0f;
        App.camera.pitch = 0.0f;
    }
}

// ============================================================================
// Syntax Highlighting Builders
// ============================================================================
static Ref<CodeHighlighter> createFileRiftHighlighter() {
    Ref<CodeHighlighter> h;
    h.instantiate();

    h->set_number_color(Color(0.82f, 0.60f, 0.40f));          // #d19a66 (Orange)
    h->set_symbol_color(Color(0.67f, 0.70f, 0.75f));          // #abb2bf (Brackets/Operators)
    h->set_function_color(Color(0.38f, 0.69f, 0.93f));        // #61afef (Functions/Constructors)
    h->set_member_variable_color(Color(0.88f, 0.44f, 0.47f)); // #e06c75 (Member fields)

    // Protobuf Message / Component Types (Purple)
    const Color cType(0.78f, 0.47f, 0.87f);
    const char* types[] = {
        "Scene", "SceneObject", "ObjectLibrary", "Component", "GroundMesh", "GroundPolygon",
        "CollisionShape", "WaterMesh", "Light", "DirectionalLight", "PointLight", "SimpleGlow",
        "ParticleEmitter", "FireEmitter", "SoundEffect", "PhysicsPlatform", "Portal",
        "SpawnPoint", "Background", "TextureMapping", "RoundHat", "GameData", "GameOptions",
        "PlayerProfile", "GameState", "Map", "SoundLibrary", "Font", "Texture", "All",
        "LocalAabb", "Color", "Vector2", "Vector3", "Rect", "Sound"
    };
    for (const char* t : types) {
        h->add_keyword_color(S(t), cType);
    }

    // Protobuf Field Names (Blue / Cyan)
    const Color cField(0.38f, 0.69f, 0.93f);
    const char* fields[] = {
        "Identifier", "TemplateName", "Position", "Depth", "Rotation", "Scaling",
        "LocalAabb", "Hidden", "TextureMapping", "RoundHat", "Radius", "Height",
        "Intensity", "Ambient", "KeyLight", "Shadow", "SurfaceWidth", "MinDepth",
        "MaxDepth", "RandomSeed", "HorizNoise", "SoundName", "DestinationSceneName",
        "TopTexture", "FrontTexture", "TileSize", "FrontColor", "SurfaceColor",
        "Flags", "Tag", "Type", "Name", "Value", "Enabled", "Visible", "Index",
        "ImportedLibrary", "SoundIdentifier", "FilePath", "Loop", "Volume",
        "EmissionFactor", "Texture", "Scale", "Orientation", "Layer", "Z"
    };
    for (const char* f : fields) {
        h->add_member_keyword_color(S(f), cField);
    }

    // Literals
    const Color cKw(0.90f, 0.75f, 0.40f);
    h->add_keyword_color("true", cKw);
    h->add_keyword_color("false", cKw);
    h->add_keyword_color("null", cKw);

    // Color Regions
    // Comments: # to EOL (Slate Gray/Green)
    h->add_color_region("#", "", Color(0.43f, 0.46f, 0.51f), true);
    // Header banner: ## to EOL (Teal)
    h->add_color_region("##", "", Color(0.35f, 0.75f, 0.70f), true);
    // Strings: "..." (Amber)
    h->add_color_region("\"", "\"", Color(0.59f, 0.76f, 0.49f), false);
    // Embedded Lua code: $ ... $end (Cyan/Teal)
    h->add_color_region("$", "$end", Color(0.34f, 0.71f, 0.76f), false);
    // Compiler directives: @compile (Coral)
    h->add_color_region("@compile", "", Color(0.88f, 0.44f, 0.47f), true);

    return h;
}

static Ref<CodeHighlighter> createLuaHighlighter() {
    Ref<CodeHighlighter> h;
    h.instantiate();

    h->set_number_color(Color(0.82f, 0.60f, 0.40f));
    h->set_symbol_color(Color(0.67f, 0.70f, 0.75f));
    h->set_function_color(Color(0.38f, 0.69f, 0.93f));
    h->set_member_variable_color(Color(0.88f, 0.44f, 0.47f));

    const Color cKw(0.78f, 0.47f, 0.87f);
    const char* kws[] = {
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "if", "in", "local", "nil", "not", "or", "repeat",
        "return", "then", "true", "until", "while"
    };
    for (const char* k : kws) {
        h->add_keyword_color(S(k), cKw);
    }

    const Color cBuiltin(0.34f, 0.71f, 0.76f);
    const char* builtins[] = {
        "print", "pairs", "ipairs", "type", "tostring", "tonumber", "setmetatable",
        "getmetatable", "rawget", "rawset", "pcall", "xpcall", "error", "assert",
        "table", "string", "math", "coroutine", "os", "debug", "io", "require"
    };
    for (const char* b : builtins) {
        h->add_member_keyword_color(S(b), cBuiltin);
    }

    h->add_color_region("--", "", Color(0.43f, 0.46f, 0.51f), true);
    h->add_color_region("--[[", "]]", Color(0.43f, 0.46f, 0.51f), false);
    h->add_color_region("\"", "\"", Color(0.59f, 0.76f, 0.49f), false);
    h->add_color_region("'", "'", Color(0.59f, 0.76f, 0.49f), false);
    h->add_color_region("[[", "]]", Color(0.59f, 0.76f, 0.49f), false);

    return h;
}

static Ref<CodeHighlighter> createJsonHighlighter() {
    Ref<CodeHighlighter> h;
    h.instantiate();

    h->set_number_color(Color(0.82f, 0.60f, 0.40f));
    h->set_symbol_color(Color(0.67f, 0.70f, 0.75f));
    h->set_member_variable_color(Color(0.38f, 0.69f, 0.93f));

    const Color cKw(0.78f, 0.47f, 0.87f);
    h->add_keyword_color("true", cKw);
    h->add_keyword_color("false", cKw);
    h->add_keyword_color("null", cKw);

    h->add_color_region("\"", "\"", Color(0.59f, 0.76f, 0.49f), false);
    h->add_color_region("//", "", Color(0.43f, 0.46f, 0.51f), true);
    return h;
}

static std::string editorFiletypeForPath(const std::string& path) {
    const std::string ext = lowerCopy(std::filesystem::path(path).extension().string());
    if (ext == ".scene") return "scene";
    if (ext == ".scl") return "scl";
    if (ext == ".gdata") return "gdata";
    if (ext == ".gopt") return "gopt";
    if (ext == ".gplayer") return "gplayer";
    if (ext == ".gstate") return "gstate";
    if (ext == ".scmap") return "scmap";
    if (ext == ".sounds") return "sounds";
    if (ext == ".fnt") return "fnt";
    if (ext == ".atlas") return "atlas";
    if (ext == ".fr") return "fr";
    return {};
}

static bool editorLooksLikeMarkup(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return true;
    const std::string prefix = text.substr(first, 96);
    return prefix.find("## FileRift decoded") != std::string::npos ||
           prefix.find("ObjectLibrary") != std::string::npos ||
           prefix.find("SceneObject") != std::string::npos ||
           prefix.find("Object {") != std::string::npos;
}

static bool editorAtomicWrite(const std::string& path, const std::string& bytes, std::string& error) {
    namespace fs = std::filesystem;
    const fs::path destination(path);
    const fs::path temp = destination.parent_path() / (destination.filename().string() + ".swordigo-editor.tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "Cannot create temporary file"; return false; }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) { error = "Write failed"; out.close(); std::error_code ignored; fs::remove(temp, ignored); return false; }
    }
    std::error_code ec;
    fs::rename(temp, destination, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(destination, ignored);
        fs::rename(temp, destination, ec);
    }
    if (ec) { error = "Cannot replace destination: " + ec.message(); fs::remove(temp, ec); return false; }
    return true;
}

static void updateEditorStatus(const std::string& message = {}) {
    if (!gEditorStatus || !gCodeEdit) return;
    const int curLine = gCodeEdit->get_caret_line() + 1;
    const int curCol  = gCodeEdit->get_caret_column() + 1;
    const int lineCnt = gCodeEdit->get_line_count();
    
    std::string typeDesc = App.editorIsFileRift ? ("FileRift [" + App.editorFiletype + "]") :
                           (!App.editorFiletype.empty() ? App.editorFiletype : "Plain Text");
    char infoBuf[256];
    snprintf(infoBuf, sizeof(infoBuf), "   |   Ln %d, Col %d   |   %d lines   |   UTF-8   |   %s",
             curLine, curCol, lineCnt, typeDesc.c_str());

    std::string stateMsg;
    Color stateCol = C_SUCCESS;
    if (!message.empty()) {
        stateMsg = "✗ " + message;
        stateCol = C_ERROR;
    } else if (App.editorDirty) {
        stateMsg = "● Modified (unsaved)";
        stateCol = C_WARNING;
    } else {
        stateMsg = "✓ Saved & Synced";
        stateCol = C_SUCCESS;
    }

    gEditorStatus->set_text(S(stateMsg + infoBuf));
    gEditorStatus->add_theme_color_override("font_color", stateCol);

    if (gEditorDocument) {
        std::string fname = std::filesystem::path(App.editorPath).filename().string();
        if (fname.empty()) fname = "Untitled";
        if (App.editorDirty) fname += " *";
        gEditorDocument->set_text(S("  " + fname + (App.editorDecodedBinary ? " (Decoded Protobuf)" : "")));
    }
}

static void updateEditorDiagnostics() {
    if (!gCodeEdit || App.editorBuffer.empty()) return;
    App.editorDiagnostic.clear();
    const std::vector<intel::BracketError> brackets = gEditorIntelliJ.check_syntax_errors(App.editorBuffer);
    if (!brackets.empty()) App.editorDiagnostic = brackets.front().message;
    if (App.editorIsFileRift && !App.editorBuffer.empty()) {
        try {
            (void)filerift::recode_markup(App.editorBuffer, App.editorFiletype);
        } catch (const std::exception& e) {
            App.editorDiagnostic = e.what();
        }
    }
    updateEditorStatus(App.editorDiagnostic.empty() ? std::string() : App.editorDiagnostic);
}

static void loadEditorDocument(const std::string& path) {
    if (!gCodeEdit || path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) { updateEditorStatus("Cannot open file: " + path); return; }
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    const std::string type = editorFiletypeForPath(path);
    std::string text = bytes;
    bool decoded = false;
    if (!type.empty() && !editorLooksLikeMarkup(bytes)) {
        try {
            text = filerift::decode_protobuf(bytes, type);
            decoded = true;
        } catch (const std::exception& e) {
            updateEditorStatus(e.what());
            return;
        }
    }
    App.editorPath = path;
    App.editorFiletype = type;
    App.editorIsFileRift = !type.empty() && (decoded || editorLooksLikeMarkup(text));
    App.editorDecodedBinary = decoded;
    App.editorBuffer = text;
    App.editorDiagnostic.clear();
    App.editorDirty = false;
    App.editorInternalChange = true;

    // Apply appropriate syntax highlighter
    const std::string ext = lowerCopy(std::filesystem::path(path).extension().string());
    if (App.editorIsFileRift) {
        gCodeEdit->set_syntax_highlighter(createFileRiftHighlighter());
    } else if (ext == ".lua") {
        gCodeEdit->set_syntax_highlighter(createLuaHighlighter());
    } else if (ext == ".json") {
        gCodeEdit->set_syntax_highlighter(createJsonHighlighter());
    }

    gCodeEdit->set_text(S(text));
    gCodeEdit->tag_saved_version();
    gCodeEdit->clear_undo_history();
    App.editorInternalChange = false;

    updateEditorDiagnostics();
    app_log(("Editor loaded document: " + path + (decoded ? " (Decoded Protobuf)" : "")).c_str());
}

static void saveEditorDocument() {
    if (!gCodeEdit || App.editorPath.empty()) return;
    App.editorBuffer = gCodeEdit->get_text().utf8().get_data();
    std::string output = App.editorBuffer;
    if (App.editorIsFileRift) {
        try {
            output = filerift::recode_markup(App.editorBuffer, App.editorFiletype);
        } catch (const std::exception& e) {
            App.editorDiagnostic = e.what();
            updateEditorStatus("Recode failed: " + App.editorDiagnostic);
            app_log(("Save blocked — Schema Error: " + App.editorDiagnostic).c_str());
            return;
        }
    }
    std::string error;
    if (!editorAtomicWrite(App.editorPath, output, error)) {
        updateEditorStatus(error);
        return;
    }
    App.editorDirty = false;
    App.editorDiagnostic.clear();
    gCodeEdit->tag_saved_version();
    updateEditorStatus();
    app_log(("Saved document: " + App.editorPath).c_str());

    // If saving a scene/scl file, update the 3D scene visualizer automatically!
    if (App.editorFiletype == "scene" || App.editorFiletype == "scl" || App.editorPath == App.selPath) {
        if (App.editorPath == App.selPath) {
            App.sceneData = av::scene_load(App.editorPath);
            clearSceneEntities();
            buildSceneRepresentation();
            app_log("Updated 3D Scene Viewport with modified scene geometry.");
        }
    }
}

static void editorFindNext(bool backwards = false) {
    if (!gCodeEdit || !gEditorSearch) return;
    const String needle = gEditorSearch->get_text();
    if (needle.is_empty()) return;

    uint32_t flags = 0;
    if (backwards) flags |= TextEdit::SEARCH_BACKWARDS;

    gCodeEdit->set_search_text(needle);
    gCodeEdit->set_search_flags(flags);

    int fromLine = gCodeEdit->get_caret_line();
    int fromCol  = gCodeEdit->get_caret_column();
    if (!backwards && gCodeEdit->has_selection()) {
        fromCol = gCodeEdit->get_selection_to_column();
        fromLine = gCodeEdit->get_selection_to_line();
    }

    Point2i res = gCodeEdit->search(needle, flags, fromLine, fromCol);
    if (res.x < 0 || res.y < 0) {
        // Wrap around
        res = gCodeEdit->search(needle, flags, backwards ? gCodeEdit->get_line_count() - 1 : 0, backwards ? 999999 : 0);
    }
    if (res.x >= 0 && res.y >= 0) {
        gCodeEdit->select(res.y, res.x, res.y, res.x + needle.length());
        gCodeEdit->set_caret_line(res.y);
        gCodeEdit->set_caret_column(res.x + needle.length());
    } else {
        updateEditorStatus("Text not found: " + std::string(needle.utf8().get_data()));
    }
}

static void editorReplace(bool all) {
    if (!gCodeEdit || !gEditorSearch || !gEditorReplace) return;
    const String needle = gEditorSearch->get_text();
    const String replacement = gEditorReplace->get_text();
    if (needle.is_empty()) return;

    if (!all) {
        if (gCodeEdit->has_selection() && gCodeEdit->get_selected_text() == needle) {
            gCodeEdit->insert_text_at_caret(replacement);
        }
        editorFindNext(false);
    } else {
        gCodeEdit->begin_complex_operation();
        int count = 0;
        int fromLine = 0, fromCol = 0;
        while (true) {
            Point2i res = gCodeEdit->search(needle, 0, fromLine, fromCol);
            if (res.x < 0 || res.y < 0) break;
            gCodeEdit->select(res.y, res.x, res.y, res.x + needle.length());
            gCodeEdit->insert_text_at_caret(replacement);
            fromLine = res.y;
            fromCol = res.x + replacement.length();
            count++;
        }
        gCodeEdit->end_complex_operation();
        char buf[64];
        snprintf(buf, sizeof(buf), "Replaced %d occurrences", count);
        updateEditorStatus(buf);
    }
    App.editorBuffer = gCodeEdit->get_text().utf8().get_data();
    updateEditorDiagnostics();
}

static Control* buildCodeEditorTab() {
    auto* root = memnew(Control);
    root->set_name(S(std::string(ICON_FA_CODE) + "  Text Editor"));
    root->set_anchors_preset(Control::PRESET_FULL_RECT);

    auto* col = memnew(VBoxContainer);
    col->set_anchors_preset(Control::PRESET_FULL_RECT);
    col->add_theme_constant_override("separation", 2);
    root->add_child(col);

    // Header Toolbar
    auto* header = memnew(PanelContainer);
    header->add_theme_style_override("panel", makeBox(C_BASE, 0, 4, 8, C_BORDER, 1));
    col->add_child(header);

    auto* head = memnew(HBoxContainer);
    head->add_theme_constant_override("separation", 6);
    header->add_child(head);

    gEditorDocument = memnew(Label);
    gEditorDocument->set_text("  No document open");
    gEditorDocument->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    head->add_child(gEditorDocument);

    auto makeToolBtn = [&](const char* label, int actionId) -> Button* {
        auto* b = memnew(Button);
        b->set_text(S(label));
        b->set_custom_minimum_size(Vector2(0, 26));
        if (ruby_gg_handler()) {
            b->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(actionId));
        }
        head->add_child(b);
        return b;
    };

    gEditorSaveBtn = makeToolBtn(ICON_FA_FLOPPY_DISK "  Save", 430);
    makeToolBtn(ICON_FA_ARROW_ROTATE_LEFT "  Reload", 434);
    makeToolBtn(ICON_FA_CHECK "  Validate", 433);

    auto* vsep = memnew(VSeparator);
    head->add_child(vsep);

    // Find / Replace Bar (Collapsible / inline)
    auto* searchBar = memnew(PanelContainer);
    searchBar->add_theme_style_override("panel", makeBox(C_PANEL, 0, 3, 6, C_BORDER, 1));
    col->add_child(searchBar);

    auto* searchBox = memnew(HBoxContainer);
    searchBox->add_theme_constant_override("separation", 6);
    searchBar->add_child(searchBox);

    auto* findLbl = memnew(Label);
    findLbl->set_text(S(std::string(ICON_FA_MAGNIFYING_GLASS) + " Find:"));
    searchBox->add_child(findLbl);

    gEditorSearch = memnew(LineEdit);
    gEditorSearch->set_placeholder("Search in document...");
    gEditorSearch->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    searchBox->add_child(gEditorSearch);

    auto* btnFindPrev = memnew(Button);
    btnFindPrev->set_text(S(std::string(ICON_FA_CHEVRON_LEFT) + " Prev"));
    if (ruby_gg_handler()) btnFindPrev->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(435));
    searchBox->add_child(btnFindPrev);

    auto* btnFindNext = memnew(Button);
    btnFindNext->set_text(S(std::string(ICON_FA_CHEVRON_RIGHT) + " Next"));
    if (ruby_gg_handler()) btnFindNext->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(436));
    searchBox->add_child(btnFindNext);

    auto* repLbl = memnew(Label);
    repLbl->set_text(S(std::string(ICON_FA_ARROWS_ROTATE) + " Replace:"));
    searchBox->add_child(repLbl);

    gEditorReplace = memnew(LineEdit);
    gEditorReplace->set_placeholder("Replace with...");
    gEditorReplace->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    searchBox->add_child(gEditorReplace);

    auto* btnRepOne = memnew(Button);
    btnRepOne->set_text(S(std::string(ICON_FA_CHECK) + " Replace"));
    if (ruby_gg_handler()) btnRepOne->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(431));
    searchBox->add_child(btnRepOne);

    auto* btnRepAll = memnew(Button);
    btnRepAll->set_text(S(std::string(ICON_FA_ARROWS_ROTATE) + " Replace All"));
    if (ruby_gg_handler()) btnRepAll->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(432));
    searchBox->add_child(btnRepAll);

    // CodeEdit Core Instance
    gCodeEdit = memnew(CodeEdit);
    gCodeEdit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    gCodeEdit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    gCodeEdit->set_draw_line_numbers(true);
    gCodeEdit->set_draw_fold_gutter(true);
    gCodeEdit->set_line_folding_enabled(true);
    gCodeEdit->set_draw_minimap(true);
    gCodeEdit->set_minimap_width(120);
    gCodeEdit->set_highlight_current_line(true);
    gCodeEdit->set_highlight_all_occurrences(true);
    gCodeEdit->set_auto_indent_enabled(true);
    gCodeEdit->set_indent_size(4);
    gCodeEdit->set_indent_using_spaces(true);
    gCodeEdit->set_auto_brace_completion_enabled(true);
    gCodeEdit->set_highlight_matching_braces_enabled(true);
    gCodeEdit->add_auto_brace_completion_pair("{", "}");
    gCodeEdit->add_auto_brace_completion_pair("(", ")");
    gCodeEdit->add_auto_brace_completion_pair("[", "]");
    gCodeEdit->add_auto_brace_completion_pair("\"", "\"");
    gCodeEdit->add_auto_brace_completion_pair("'", "'");
    gCodeEdit->add_comment_delimiter("--", "", true);
    gCodeEdit->add_comment_delimiter("#", "", true);
    gCodeEdit->add_comment_delimiter("//", "", true);
    gCodeEdit->add_string_delimiter("\"", "\"");
    gCodeEdit->add_string_delimiter("'", "'");
    gCodeEdit->add_string_delimiter("$", "$end");

    gCodeEdit->set_syntax_highlighter(createFileRiftHighlighter());
    col->add_child(gCodeEdit);

    // Bottom Status & Diagnostics Bar
    auto* statusPC = memnew(PanelContainer);
    statusPC->add_theme_style_override("panel", makeBox(C_BASE, 0, 3, 8, C_BORDER, 1));
    col->add_child(statusPC);

    gEditorStatus = memnew(Label);
    gEditorStatus->set_text("  Ready  |  No document open");
    gEditorStatus->add_theme_color_override("font_color", C_TEXT_DIM);
    statusPC->add_child(gEditorStatus);

    return root;
}

// ============================================================================
// Inspector Builders
// ============================================================================
static void updateSceneObjectProperties(int idx) {
    if (!gPropsTree || idx < 0 || idx >= (int)App.sceneData.objects.size()) return;
    const auto& obj = App.sceneData.objects[idx];
    gPropsTree->clear();
    TreeItem* r = gPropsTree->create_item();
    r->set_text(0, S(obj.name.empty() ? "(Unnamed)" : obj.name.c_str()));
    r->set_text(1, S(obj.template_name));

    auto addRow = [&](const char* k, const std::string& v) {
        TreeItem* it = gPropsTree->create_item(r);
        it->set_text(0, S(k)); it->set_text(1, S(v));
    };
    auto addF = [&](const char* k, float v) { char b[32]; snprintf(b,32,"%.3f",v); addRow(k,b); };

    addRow("Name",       obj.name);
    addRow("Template",   obj.template_name);
    addF("X",        obj.pos_x);
    addF("Y",        obj.pos_y);
    addF("Depth Z",  obj.pos_z);
    addF("Rotation", obj.rot_y);
    addF("Scale",    obj.scale_x * obj.template_scaling);
    addRow("Hidden", obj.hidden ? "true" : "false");
    if (!obj.mesh_name.empty())    addRow("POD Model",  obj.mesh_name);
    if (!obj.texture_name.empty()) addRow("Texture",    obj.texture_name);
    if (obj.is_spawn_point)        addRow("Spawn Point","true");
    if (!obj.components.empty()) {
        TreeItem* ch = gPropsTree->create_item(r);
        ch->set_text(0, "Components");
        ch->set_text(1, S(std::to_string(obj.components.size())));
        for (const auto& comp : obj.components) {
            TreeItem* ci = gPropsTree->create_item(ch);
            ci->set_text(0, S(comp.type_name.empty() ? "Component" : comp.type_name));
            ci->set_text(1, S("Tag " + std::to_string(comp.payload_field)));
        }
    }
}

// ============================================================================
// Directory & File Loading
// ============================================================================
static void loadDir(const char* path) {
    if (!path || !gFileTree) return;
    App.cwd = path;
    App.previewType = 0;
    App.selName.clear();
    App.lastLoadedFile.clear();

    gFileTree->clear();
    gFileTree->set_hide_root(true);
    TreeItem* root = gFileTree->create_item();

    Error e;
    Ref<DirAccess> da = DirAccess::open(S(path), &e);
    if (da.is_null()) { app_log(("Cannot open: " + std::string(path)).c_str()); return; }

    // Collect dirs first, then files
    std::vector<std::pair<String,String>> dirs_list, files_list;
    da->list_dir_begin();
    String nm = da->get_next();
    while (!nm.is_empty()) {
        if (!nm.begins_with(".")) {
            String full = S(path) + "/" + nm;
            if (da->current_is_dir()) dirs_list.push_back({nm, full});
            else files_list.push_back({nm, full});
        }
        nm = da->get_next();
    }

    // Sort
    auto cmpStr = [](const std::pair<String,String>& a, const std::pair<String,String>& b){ return a.first < b.first; };
    std::sort(dirs_list.begin(), dirs_list.end(), cmpStr);
    std::sort(files_list.begin(), files_list.end(), cmpStr);

    int count = 0;
    for (const auto& [name, full] : dirs_list) {
        TreeItem* item = gFileTree->create_item(root);
        item->set_text(0, S(std::string(ICON_FA_FOLDER) + "  " + name.utf8().get_data()));
        item->set_metadata(0, full);
        item->set_custom_color(0, C_BADGE_DIR);
        count++;
    }
    for (const auto& [name, full] : files_list) {
        String ext = name.to_lower();
        const char* icon = ICON_FA_FILE;
        Color col = C_TEXT_DIM;
        if      (ext.ends_with("pod"))   { icon = ICON_FA_CUBE; col = C_BADGE_POD; }
        else if (ext.ends_with("scene")) { icon = ICON_FA_MAP; col = C_BADGE_SCENE; }
        else if (ext.ends_with("scl"))   { icon = ICON_FA_CODE; col = C_BADGE_SCL; }
        else if (ext.ends_with("pvr") || ext.ends_with(".tex") || ext.ends_with(".tex.png")) { icon = ICON_FA_IMAGE; col = C_BADGE_TEX; }
        else if (ext.ends_with("png") || ext.ends_with("jpg") || ext.ends_with("jpeg")) { icon = ICON_FA_IMAGE; col = C_BADGE_TEX; }
        else if (ext.ends_with("wav") || ext.ends_with("ogg")) { icon = ICON_FA_MUSIC; col = C_BADGE_AUDIO; }
        else if (ext.ends_with("fnt"))   { icon = ICON_FA_PEN; col = C_BADGE_FONT; }
        else if (ext.ends_with("atlas")) { icon = ICON_FA_OBJECT_GROUP; col = C_BADGE_ATLAS; }
        else if (ext.ends_with("lua"))   { icon = ICON_FA_CODE; col = C_BADGE_CODE; }
        else if (ext.ends_with("json") || ext.ends_with("txt")) { icon = ICON_FA_FILE; col = C_TEXT_DIM; }
        else icon = ICON_FA_FILE;

        TreeItem* item = gFileTree->create_item(root);
        item->set_text(0, S(std::string(icon) + "  " + name.utf8().get_data()));
        item->set_metadata(0, full);
        item->set_custom_color(0, col);
        count++;
    }
    if (gPathBar) gPathBar->set_text(S(path));
    if (gStatusLabel) gStatusLabel->set_text(S(" " + std::string(path) + "   |   " + std::to_string(count) + " items"));
}

static void loadFile(const char* path) {
    if (!path) return;
    if (App.lastLoadedFile == path) return;
    App.lastLoadedFile = path;
    App.selPath = path;
    App.selName = std::filesystem::path(path).filename().string();

    String ext = S(App.selName).get_extension().to_lower();
    if (gStatusLabel) gStatusLabel->set_text(S(" " + App.selName));
    const bool editableText = !editorFiletypeForPath(path).empty() || ext == "lua" || ext == "json" || ext == "txt" || ext == "gmesh";
    if (editableText) loadEditorDocument(path);

    if (ext == "pod") {
        App.previewType = 1;
        setViewMode(false);
        clearSceneEntities();
        App.podModel = av::pod_load(path);
        App.podMesh  = podToMesh(App.podModel, path);
        App.camera.reset();
        if (App.podModel.total_vertices > 0) {
            App.camera.dist = App.podModel.radius * 2.2f;
            App.camera.tx   = App.podModel.center_x;
            App.camera.ty   = App.podModel.center_y;
            App.camera.tz   = App.podModel.center_z;
            App.camera.yaw  = 30.0f;
            App.camera.pitch = 18.0f;
        }

        gInspectorTree->clear();
        TreeItem* r = gInspectorTree->create_item();
        r->set_text(0, S(std::string(ICON_FA_CUBE) + "  " + App.selName));
        r->set_text(1, S(std::to_string(App.podModel.total_vertices) + " verts"));
        r->set_custom_color(0, C_BADGE_POD);

        auto addRow = [&](const char* k, const std::string& v) {
            TreeItem* it = gInspectorTree->create_item(r);
            it->set_text(0, S(k)); it->set_text(1, S(v));
        };
        addRow("Asset Type", "PowerVR POD 3D Model");
        addRow("Version",    App.podModel.version);
        addRow("Sub-meshes", std::to_string(App.podModel.meshes.size()));
        addRow("Vertices",   std::to_string(App.podModel.total_vertices));
        addRow("Triangles",  std::to_string(App.podModel.total_faces));
        addRow("Frames",     std::to_string(App.podModel.num_frames));
        addRow("Textures",   std::to_string(App.podModel.texture_filenames.size()));
        app_log(("Loaded POD: " + App.selName + " (" + std::to_string(App.podModel.meshes.size()) + " meshes)").c_str());

    } else if (ext == "scene" || ext == "scl") {
        App.previewType = 3;
        App.sceneData   = av::scene_load(path);
        App.selectedSceneObjIndex = -1;

        // 3D reconstruction
        buildSceneRepresentation();

        // Inspector
        gInspectorTree->clear();
        TreeItem* r = gInspectorTree->create_item();
        r->set_text(0, S(std::string(ICON_FA_MAP) + "  " + App.selName));
        r->set_text(1, S(std::to_string(App.sceneData.objects.size()) + " entities"));
        r->set_custom_color(0, C_BADGE_SCENE);
        auto addRow = [&](const char* k, const std::string& v) {
            TreeItem* it = gInspectorTree->create_item(r);
            it->set_text(0, S(k)); it->set_text(1, S(v));
        };
        addRow("Asset Type", "Swordigo Scene Graph (Protobuf)");
        addRow("File",       App.sceneData.filename);
        addRow("Entities",   std::to_string(App.sceneData.objects.size()));
        addRow("Lights",     std::to_string(App.sceneData.lights.size()));
        addRow("Waters",     std::to_string(App.sceneData.waters.size()));

        // Scene hierarchy tree
        gSceneTree->clear();
        TreeItem* stRoot = gSceneTree->create_item();
        stRoot->set_text(0, S(std::string(ICON_FA_LAYER_GROUP) + "  " + App.selName + " (" + std::to_string(App.sceneData.objects.size()) + " entities)"));
        for (size_t i = 0; i < App.sceneData.objects.size(); ++i) {
            const auto& obj = App.sceneData.objects[i];
            TreeItem* it = gSceneTree->create_item(stRoot);
            std::string icon = ICON_FA_FILE;
            Color col = C_TEXT;
            if (obj.is_spawn_point) {
                icon = ICON_FA_LOCATION_DOT;
                col = C_ERROR;
            } else if (!obj.mesh_name.empty()) {
                icon = ICON_FA_CUBE;
                col = C_BADGE_POD;
            } else if (!obj.ground_meshes.empty()) {
                icon = ICON_FA_MAP;
                col = C_BADGE_SCENE;
            }
            std::string lbl = obj.name.empty() ? ("[" + obj.template_name + "]") : (obj.name + "  (" + obj.template_name + ")");
            it->set_text(0, S(icon + "  " + lbl));
            it->set_metadata(0, (int)i);
            it->set_custom_color(0, col);
        }
        if (!App.sceneData.objects.empty()) updateSceneObjectProperties(0);
        app_log(("Loaded Scene: " + App.selName + " (" + std::to_string(App.sceneData.objects.size()) + " entities)").c_str());

    } else if (ext == "pvr" || ext == "png" || ext == "jpg" || ext == "tex" || ext == "jpeg") {
        App.previewType = 2;
        clearSceneEntities();
        Ref<ImageTexture> tex = loadTexture2DPreview(path);
        show2DTexture(tex, App.selName);
        // Inspector
        gInspectorTree->clear();
        TreeItem* r = gInspectorTree->create_item();
        r->set_text(0, S(std::string(ICON_FA_IMAGE) + "  " + App.selName));
        r->set_custom_color(0, C_BADGE_TEX);
        auto addRow = [&](const char* k, const std::string& v) {
            TreeItem* it = gInspectorTree->create_item(r);
            it->set_text(0, S(k)); it->set_text(1, S(v));
        };
        addRow("Asset Type", "PowerVR PVR Compressed Texture");
        if (tex.is_valid()) {
            addRow("Width",  std::to_string(tex->get_width()) + " px");
            addRow("Height", std::to_string(tex->get_height()) + " px");
            addRow("Format", "ETC1 → RGBA8888");
            addRow("Status", "GPU Upload OK");
            r->set_text(1, S(std::to_string(tex->get_width()) + "×" + std::to_string(tex->get_height())));
        } else {
            addRow("Status", "Decode Failed");
        }
        app_log(("Decoded: " + App.selName + (tex.is_valid() ? " (" + std::to_string(tex->get_width()) + "×" + std::to_string(tex->get_height()) + ")" : " FAILED")).c_str());

    } else if (ext == "fnt") {
        App.previewType = 5;
        clearSceneEntities();
        std::ifstream ifs(path, std::ios::binary);
        if (ifs) {
            std::string bytes((std::istreambuf_iterator<char>(ifs)), {});
            std::string decoded = filerift::decode_protobuf(bytes, "fnt");
            gInspectorTree->clear();
            TreeItem* r = gInspectorTree->create_item();
            r->set_text(0, S(std::string(ICON_FA_PEN) + "  " + App.selName)); r->set_custom_color(0, C_BADGE_FONT);
            auto addRow = [&](const char* k, const std::string& v) {
                TreeItem* it = gInspectorTree->create_item(r);
                it->set_text(0, S(k)); it->set_text(1, S(v));
            };
            addRow("Asset Type", "Swordigo Font (Protobuf)");
            addRow("Raw Size",   std::to_string(bytes.size()) + " bytes");
            app_log(("=== Font: " + App.selName + " ===").c_str());
            app_log(decoded.c_str());
        }
        Ref<ImageTexture> ftex = findTexture(App.selName);
        show2DTexture(ftex, App.selName);

    } else if (ext == "atlas") {
        App.previewType = 6;
        clearSceneEntities();
        std::ifstream ifs(path, std::ios::binary);
        if (ifs) {
            std::string bytes((std::istreambuf_iterator<char>(ifs)), {});
            std::string decoded = filerift::decode_protobuf(bytes, "atlas");
            gInspectorTree->clear();
            TreeItem* r = gInspectorTree->create_item();
            r->set_text(0, S(std::string(ICON_FA_OBJECT_GROUP) + "  " + App.selName)); r->set_custom_color(0, C_BADGE_ATLAS);
            auto addRow = [&](const char* k, const std::string& v) {
                TreeItem* it = gInspectorTree->create_item(r);
                it->set_text(0, S(k)); it->set_text(1, S(v));
            };
            addRow("Asset Type", "Swordigo Texture Atlas");
            addRow("Raw Size",   std::to_string(bytes.size()) + " bytes");
            app_log(("=== Atlas: " + App.selName + " ===").c_str());
            app_log(decoded.c_str());
        }
        Ref<ImageTexture> atex = findTexture(App.selName);
        show2DTexture(atex, App.selName);

    } else if (ext == "lua" || ext == "json" || ext == "txt") {
        App.previewType = 4;
        clearSceneEntities();
        setViewMode(false);
        gInspectorTree->clear();
        TreeItem* r = gInspectorTree->create_item();
        r->set_text(0, S(std::string(ICON_FA_CODE) + "  " + App.selName)); r->set_custom_color(0, C_BADGE_CODE);
        FILE* fp = fopen(path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
            std::string buf; buf.resize(sz < 65536 ? sz : 65536);
            size_t rd = fread(&buf[0], 1, buf.size(), fp); fclose(fp); buf.resize(rd);
            app_log(("=== " + App.selName + " ===").c_str());
            app_log(buf.c_str());
        }
    } else {
        App.previewType = 0;
        clearSceneEntities();
        app_log(("Selected: " + App.selName).c_str());
    }
}

// ============================================================================
// Scene Generator Logic (runs synchronously — show result then offer save)
// ============================================================================
static void runSceneGenerator() {
    if (!gSgenResultLabel || !gSgenVersion) return;

    sgen::TerrainOptions opt = App.sgenOpt;
    opt.seed           = (uint32_t)(gSgenSeed      ? gSgenSeed->get_value()      : 1337);
    opt.width          = (float)   (gSgenWidth     ? gSgenWidth->get_value()     : 4200);
    opt.height         = (float)   (gSgenHeight    ? gSgenHeight->get_value()    : 900);
    opt.platform_count = (int)     (gSgenPlatforms ? gSgenPlatforms->get_value() : 6);
    opt.roughness      = (float)   (gSgenRoughness ? gSgenRoughness->get_value() : 1.0);
    opt.octaves        = (int)     (gSgenOctaves   ? gSgenOctaves->get_value()   : 4);
    opt.add_water      = gSgenWater     ? gSgenWater->is_pressed()     : true;
    opt.spill_torches  = gSgenTorches   ? gSgenTorches->is_pressed()   : true;
    opt.mountains      = gSgenMountains ? gSgenMountains->is_pressed() : false;
    opt.islands        = gSgenIslands   ? gSgenIslands->is_pressed()   : false;
    opt.add_portal     = gSgenPortal    ? gSgenPortal->is_pressed()    : false;
    opt.biome          = (sgen::Biome)(gSgenBiome ? gSgenBiome->get_selected() : 0);

    sgen::Result res;
    int version = gSgenVersion ? gSgenVersion->get_selected() : 1;
    if (version == 0) {
        // V1 — blueprint mode: simple single-platform grasslands demo
        sgen::Blueprint bp;
        sgen::Platform p; p.top_texture = "forest_grass"; p.front_texture = "forest_ground";
        p.rect[0] = -2100; p.rect[1] = 0; p.rect[2] = 4200; p.rect[3] = 200;
        bp.platforms.push_back(p);
        bp.background    = opt.biome == sgen::Biome::Forest ? "forestbackground_day" : "grasslandsbackground_day";
        bp.scene_name    = opt.scene_name;
        res = sgen::generate_scene(bp);
    } else if (version == 1) {
        res = sgen::generate_biome_scene(opt);
    } else if (version == 2) {
        sgen::v2::TerrainOptionsV2 opt2;
        static_cast<sgen::TerrainOptions&>(opt2) = opt;
        res = sgen::v2::generate_biome_scene_v2(opt2);
    } else if (version == 3) {
        res = sgen::v3::generate_biome_scene_v3(opt);
    }

    if (res.ok()) {
        // Save to output path
        std::string outPath = gSgenOutputPath ? std::string(gSgenOutputPath->get_text().utf8().get_data()) : "";
        if (outPath.empty()) {
            outPath = App.cwd.empty() ? "/tmp" : App.cwd;
            outPath += "/" + (opt.scene_name.empty() ? "procedural" : opt.scene_name) + ".scene";
        }
        std::ofstream ofs(outPath, std::ios::binary);
        if (ofs) {
            ofs.write(res.scene_bytes.c_str(), (std::streamsize)res.scene_bytes.size());
            ofs.close();
            char buf[512];
            snprintf(buf, sizeof(buf), "✓ Generated %d objects | Bounds %.0f×%.0f | %zu bytes → %s",
                     res.objects, res.bounds[2], res.bounds[3], res.scene_bytes.size(), outPath.c_str());
            gSgenResultLabel->set_text(S(buf));
            gSgenResultLabel->add_theme_color_override("font_color", C_SUCCESS);
            app_log(buf);
        } else {
            gSgenResultLabel->set_text(S("✗ Cannot write: " + outPath));
            gSgenResultLabel->add_theme_color_override("font_color", C_ERROR);
        }
    } else {
        gSgenResultLabel->set_text(S("✗ " + res.error));
        gSgenResultLabel->add_theme_color_override("font_color", C_ERROR);
        app_log(("Scene gen failed: " + res.error).c_str());
    }
}

// ============================================================================
// Batch Converter UI Update (called each frame when running)
// ============================================================================
static void updateBatchUI() {
    if (!gBatchProgress || !gBatchLog || !gBatchStatusLabel) return;
    batch::BatchState& bs = App.batchState;

    int total = bs.total_files.load();
    int done  = bs.done_files.load();
    int ok    = bs.ok_count.load();
    int err   = bs.err_count.load();
    bool running  = bs.running.load();
    bool finished = bs.finished.load();

    gBatchProgress->set_max(total > 0 ? total : 1);
    gBatchProgress->set_value(done);

    // Update log from batch state
    {
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        std::string newLog;
        for (const auto& entry : bs.log) {
            const char* pfx = entry.level == batch::LogLevel::ERR  ? "[ERR ] " :
                               entry.level == batch::LogLevel::WARN ? "[WARN] " :
                               entry.level == batch::LogLevel::OK   ? "[ OK ] " : "[INFO] ";
            newLog += pfx + entry.text + "\n";
        }
        if (newLog != App.batchLog) {
            App.batchLog = newLog;
            gBatchLog->set_text(S(App.batchLog));
            gBatchLog->set_caret_line(gBatchLog->get_line_count()-1);
        }
    }

    char buf[256];
    if (!running && !finished) {
        snprintf(buf, sizeof(buf), "Ready");
    } else if (running) {
        std::string cur;
        { std::lock_guard<std::mutex> lk(bs.log_mutex); cur = bs.current_file; }
        snprintf(buf, sizeof(buf), "Processing: %d / %d  |  OK: %d  |  ERR: %d  |  %s", done, total, ok, err, cur.c_str());
    } else {
        double elapsed = bs.job_end_wall - bs.job_start_wall;
        snprintf(buf, sizeof(buf), "Done: %d / %d  |  OK: %d  |  ERR: %d  |  %.1f s", done, total, ok, err, elapsed);
    }
    gBatchStatusLabel->set_text(S(buf));

    if (gBatchStartBtn)  gBatchStartBtn->set_disabled(running);
    if (gBatchCancelBtn) gBatchCancelBtn->set_disabled(!running);
}

// ============================================================================
// Callbacks
// ============================================================================
static void cb_exit()   { App.isRunning = false; }
static void cb_reset()  { App.camera.reset(); app_log("Camera reset."); }

static void cb_file() {
    // Both trees share the bridge callback. Route by focus so a stale scene
    // selection cannot swallow later clicks in the asset browser.
    if (gFileTree && gFileTree->has_focus()) {
        TreeItem* sel = gFileTree->get_selected();
        if (!sel) return;
        const String path = (String)sel->get_metadata(0);
        if (path.is_empty()) return;
        const std::string spath = path.utf8().get_data();
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::path(spath), ec)) {
            App.pendingDir = spath;
        } else {
            App.pendingFile = spath;
        }
        return;
    }

    if (gSceneTree && gSceneTree->has_focus() && gSceneTree->get_selected()) {
        TreeItem* sel = gSceneTree->get_selected();
        Variant vm = sel->get_metadata(0);
        if (vm.get_type() == Variant::INT) {
            App.pendingSceneObjIndex = (int)vm;
            return;
        }
    }

    // Keyboard activation can arrive after focus has moved. Fall back to the
    // asset selection, whose metadata is always a filesystem path.
    if (!gFileTree) return;
    if (TreeItem* sel = gFileTree->get_selected()) {
        const Variant metadata = sel->get_metadata(0);
        if (metadata.get_type() != Variant::STRING) return;
        const String path = (String)metadata;
        if (path.is_empty()) return;
        const std::string spath = path.utf8().get_data();
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::path(spath), ec)) {
            App.pendingDir = spath;
        } else {
            App.pendingFile = spath;
        }
    }
}

static void cb_scene_select() {
    // Kept for compatibility; actual routing goes through cb_file
    if (!gSceneTree) return;
    if (TreeItem* sel = gSceneTree->get_selected()) {
        Variant vm = sel->get_metadata(0);
        if (vm.get_type() == Variant::INT) App.pendingSceneObjIndex = (int)vm;
    }
}

static void cb_search() {
    if (!gSearchEdit || !gFileTree) return;
    String query = gSearchEdit->get_text().to_lower().strip_edges();
    TreeItem* root = gFileTree->get_root();
    if (!root) return;
    TreeItem* child = root->get_first_child();
    while (child) {
        if (query.is_empty()) child->set_visible(true);
        else child->set_visible(child->get_text(0).to_lower().contains(query));
        child = child->get_next();
    }
}

// ============================================================================
// Menu Callback — handles all IDs including tool-tab routing, sgen, batch, zoom
// ============================================================================
static void cb_menu_full(int id) {
    // Workspace / system
    if      (id == 100) { loadDir(App.workspace.c_str()); }
    else if (id == 199) { cb_exit(); }
    // View
    else if (id == 300) { cb_reset(); }
    else if (id == 301) { App.showGrid = !App.showGrid; }
    // Tools — tab routing
    else if (id == 401) { if (gCenterTabs) gCenterTabs->set_current_tab(2); }
    else if (id == 402) { if (gCenterTabs) gCenterTabs->set_current_tab(3); }
    // Scene Generator — run
    else if (id == 400) {
        if (gCenterTabs) gCenterTabs->set_current_tab(2);
        runSceneGenerator();
    }
    // Batch Converter — start
    else if (id == 410) {
        batch::BatchState& bs = App.batchState;
        if (gBatchSrcDir) strncpy(bs.src_dir, gBatchSrcDir->get_text().utf8().get_data(), 4095);
        if (gBatchDstDir) strncpy(bs.dst_dir, gBatchDstDir->get_text().utf8().get_data(), 4095);
        bs.mode            = gBatchMode    ? (batch::Mode)gBatchMode->get_selected()       : batch::Mode::EXPORT_TO_PNG;
        bs.compress_fmt    = gBatchFmt     ? (batch::CompressFmt)gBatchFmt->get_selected() : batch::CompressFmt::ETC1;
        bs.recurse_subdirs = gBatchRecurse ? gBatchRecurse->is_pressed() : true;
        bs.skip_existing   = gBatchSkip    ? gBatchSkip->is_pressed()    : false;
        bs.finished.store(false);
        App.batchLog.clear();
        if (gBatchLog) gBatchLog->set_text("");
        const double now = (double)Time::get_singleton()->get_ticks_msec() / 1000.0;
        batch::start_batch_job(bs, now);
        updateBatchUI();
    }
    // Batch Converter — cancel
    else if (id == 411) { batch::cancel_batch_job(App.batchState); }
    // Texture zoom — Fit
    else if (id == 420) {
        if (g2DTextureRect) {
            g2DTextureRect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            g2DTextureRect->set_expand_mode(TextureRect::EXPAND_FIT_WIDTH_PROPORTIONAL);
            g2DTextureRect->set_custom_minimum_size(Vector2(0, 0));
        }
    }
    // Texture zoom — 1× / 2× / 4×
    else if (id == 421 || id == 422 || id == 423) {
        if (g2DTextureRect) {
            g2DTextureRect->set_stretch_mode(TextureRect::STRETCH_KEEP);
            g2DTextureRect->set_expand_mode(TextureRect::EXPAND_KEEP_SIZE);
            const float z = (id == 421) ? 1.0f : (id == 422) ? 2.0f : 4.0f;
            Ref<Texture2D> t = g2DTextureRect->get_texture();
            if (t.is_valid())
                g2DTextureRect->set_custom_minimum_size(Vector2((float)t->get_width() * z, (float)t->get_height() * z));
        }
    }
    else if (id == 430) saveEditorDocument();
    else if (id == 431) editorReplace(false);
    else if (id == 432) editorReplace(true);
    else if (id == 433) {
        updateEditorDiagnostics();
        if (App.editorDiagnostic.empty()) updateEditorStatus("Markup & Schema Valid");
    }
    else if (id == 434) {
        if (!App.editorPath.empty()) loadEditorDocument(App.editorPath);
    }
    else if (id == 435) editorFindNext(true);
    else if (id == 436) editorFindNext(false);
    // About
    else if (id == 500) { if (gCenterTabs) gCenterTabs->set_current_tab(4); }
}

static void cb_viewmenu(int id) { cb_menu_full(id); }

// ============================================================================
// UI Construction — Scene Generator Tab
// ============================================================================
static Control* buildSceneGeneratorTab() {
    auto* root = memnew(ScrollContainer);
    root->set_name(S(std::string(ICON_FA_WAND_MAGIC_SPARKLES) + "  Scene Generator"));
    root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    root->set_v_size_flags(Control::SIZE_EXPAND_FILL);

    auto* vbox = memnew(VBoxContainer);
    vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    root->add_child(vbox);

    auto* margin = memnew(MarginContainer);
    margin->add_theme_constant_override("margin_left",   18);
    margin->add_theme_constant_override("margin_right",  18);
    margin->add_theme_constant_override("margin_top",    12);
    margin->add_theme_constant_override("margin_bottom", 12);
    margin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    vbox->add_child(margin);

    auto* col = memnew(VBoxContainer);
    col->add_theme_constant_override("separation", 10);
    margin->add_child(col);

    // Header
    auto* hdr = memnew(Label);
    hdr->set_text("  SCENE GENERATOR");
    hdr->add_theme_color_override("font_color", C_ACCENT);
    col->add_child(hdr);

    auto* sep0 = memnew(HSeparator); col->add_child(sep0);

    // Generator Version
    {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text("Generator:"); lbl->set_custom_minimum_size(Vector2(120,0)); hb->add_child(lbl);
        gSgenVersion = memnew(OptionButton);
        gSgenVersion->add_item("V1 — Blueprint");
        gSgenVersion->add_item("V2 — Biome Procedural");
        gSgenVersion->add_item("V3 — Ultimate Biome DB");
        gSgenVersion->add_item("V2-3D — Full 3D World");
        gSgenVersion->select(1);
        gSgenVersion->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(gSgenVersion);
    }

    // Biome
    {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text("Biome:"); lbl->set_custom_minimum_size(Vector2(120,0)); hb->add_child(lbl);
        gSgenBiome = memnew(OptionButton);
        const char* biomes[] = {"Grasslands","Forest","Grove","Wasteland","Ice Castle","Cave","Fire","Florennum"};
        for (const char* b : biomes) gSgenBiome->add_item(S(b));
        gSgenBiome->select(1);
        gSgenBiome->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(gSgenBiome);
    }

    auto* sep1 = memnew(HSeparator); col->add_child(sep1);

    // Numeric params
    auto makeSpinRow = [&](const char* label, SpinBox*& out, double val, double minv, double maxv, double step=1.0) {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text(S(label)); lbl->set_custom_minimum_size(Vector2(140,0)); hb->add_child(lbl);
        out = memnew(SpinBox);
        out->set_min(minv); out->set_max(maxv); out->set_step(step); out->set_value(val);
        out->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(out);
    };

    makeSpinRow("Seed:",           gSgenSeed,      1337,  0,  9999999, 1);
    makeSpinRow("Width (units):",  gSgenWidth,     4200,  500, 20000, 100);
    makeSpinRow("Height (units):", gSgenHeight,     900,  200,  5000, 50);
    makeSpinRow("Platforms:",      gSgenPlatforms,    6,    1,    20,  1);
    makeSpinRow("Roughness:",      gSgenRoughness,  1.0,  0.1,   3.0, 0.1);
    makeSpinRow("Octaves:",        gSgenOctaves,      4,    1,     8,  1);

    auto* sep2 = memnew(HSeparator); col->add_child(sep2);

    // Toggles
    auto makeToggle = [&](const char* label, CheckBox*& out, bool val) {
        out = memnew(CheckBox);
        out->set_text(S(label));
        out->set_pressed(val);
        col->add_child(out);
    };
    makeToggle("Add Water Sheet",     gSgenWater,     true);
    makeToggle("Spill Torches",       gSgenTorches,   true);
    makeToggle("Ridged Mountains",    gSgenMountains, false);
    makeToggle("Floating Islands",    gSgenIslands,   false);
    makeToggle("Add Portal Hub",      gSgenPortal,    false);

    auto* sep3 = memnew(HSeparator); col->add_child(sep3);

    // Output path
    {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text("Output Path:"); lbl->set_custom_minimum_size(Vector2(120,0)); hb->add_child(lbl);
        gSgenOutputPath = memnew(LineEdit);
        gSgenOutputPath->set_placeholder("/path/to/output/scene_name.scene  (leave empty for auto)");
        gSgenOutputPath->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(gSgenOutputPath);
    }

    // Generate button
    {
        gSgenGenerateBtn = memnew(Button);
        gSgenGenerateBtn->set_text(S(std::string(ICON_FA_WAND_MAGIC_SPARKLES) + "  Generate Scene"));
        gSgenGenerateBtn->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        gSgenGenerateBtn->set_custom_minimum_size(Vector2(0, 40));
        if (ruby_gg_handler()) {
            gSgenGenerateBtn->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(400));
        }
        col->add_child(gSgenGenerateBtn);
    }

    // Result label
    gSgenResultLabel = memnew(Label);
    gSgenResultLabel->set_text("  Ready — configure parameters and press Generate.");
    gSgenResultLabel->add_theme_color_override("font_color", C_TEXT_DIM);
    gSgenResultLabel->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    col->add_child(gSgenResultLabel);

    return root;
}

// ============================================================================
// UI Construction — Batch Converter Tab
// ============================================================================
static Control* buildBatchConverterTab() {
    auto* root = memnew(ScrollContainer);
    root->set_name(S(std::string(ICON_FA_TOOLBOX) + "  Batch Converter"));
    root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    root->set_v_size_flags(Control::SIZE_EXPAND_FILL);

    auto* vbox = memnew(VBoxContainer);
    vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    root->add_child(vbox);

    auto* margin = memnew(MarginContainer);
    margin->add_theme_constant_override("margin_left",   18);
    margin->add_theme_constant_override("margin_right",  18);
    margin->add_theme_constant_override("margin_top",    12);
    margin->add_theme_constant_override("margin_bottom", 12);
    margin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    vbox->add_child(margin);

    auto* col = memnew(VBoxContainer);
    col->add_theme_constant_override("separation", 10);
    margin->add_child(col);

    auto* hdr = memnew(Label);
    hdr->set_text(S(std::string(ICON_FA_TOOLBOX) + "  BATCH TEXTURE CONVERTER"));
    hdr->add_theme_color_override("font_color", C_ACCENT);
    col->add_child(hdr);

    auto* sep0 = memnew(HSeparator); col->add_child(sep0);

    auto makePathRow = [&](const char* label, LineEdit*& out, const char* placeholder) {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text(S(label)); lbl->set_custom_minimum_size(Vector2(110,0)); hb->add_child(lbl);
        out = memnew(LineEdit);
        out->set_placeholder(S(placeholder));
        out->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(out);
    };
    makePathRow("Source Dir:",    gBatchSrcDir, "/path/to/input/assets");
    makePathRow("Dest Dir:",      gBatchDstDir, "/path/to/output/dir");

    // Mode
    {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text("Mode:"); lbl->set_custom_minimum_size(Vector2(110,0)); hb->add_child(lbl);
        gBatchMode = memnew(OptionButton);
        gBatchMode->add_item("Export → PNG  (PVR/TEX → editable PNG)");
        gBatchMode->add_item("Import → Game  (PNG → PVR/TEX)");
        gBatchMode->select(0);
        gBatchMode->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(gBatchMode);
    }

    // Compression (import mode)
    {
        auto* hb = memnew(HBoxContainer); col->add_child(hb);
        auto* lbl = memnew(Label); lbl->set_text("Compress As:"); lbl->set_custom_minimum_size(Vector2(110,0)); hb->add_child(lbl);
        gBatchFmt = memnew(OptionButton);
        gBatchFmt->add_item("ETC1");
        gBatchFmt->add_item("PVRTC 4bpp");
        gBatchFmt->add_item("PVRTC 2bpp");
        gBatchFmt->add_item("RGBA8888");
        gBatchFmt->select(0);
        gBatchFmt->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(gBatchFmt);
    }

    // Toggles
    gBatchRecurse = memnew(CheckBox); gBatchRecurse->set_text("Recurse sub-directories"); gBatchRecurse->set_pressed(true); col->add_child(gBatchRecurse);
    gBatchSkip    = memnew(CheckBox); gBatchSkip->set_text("Skip existing output files");   gBatchSkip->set_pressed(false); col->add_child(gBatchSkip);

    auto* sep1 = memnew(HSeparator); col->add_child(sep1);

    // Buttons row
    {
        auto* hb = memnew(HBoxContainer); hb->add_theme_constant_override("separation", 8); col->add_child(hb);

        gBatchStartBtn = memnew(Button);
        gBatchStartBtn->set_text(S(std::string(ICON_FA_PLAY) + "  Start"));
        gBatchStartBtn->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        gBatchStartBtn->set_custom_minimum_size(Vector2(0,36));
        if (ruby_gg_handler()) {
            gBatchStartBtn->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(410));
        }
        hb->add_child(gBatchStartBtn);

        gBatchCancelBtn = memnew(Button);
        gBatchCancelBtn->set_text(S(std::string(ICON_FA_XMARK) + "  Cancel"));
        gBatchCancelBtn->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        gBatchCancelBtn->set_disabled(true);
        if (ruby_gg_handler()) {
            gBatchCancelBtn->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(411));
        }
        hb->add_child(gBatchCancelBtn);
    }

    // Progress bar
    gBatchProgress = memnew(ProgressBar);
    gBatchProgress->set_min(0); gBatchProgress->set_max(1); gBatchProgress->set_value(0);
    gBatchProgress->set_custom_minimum_size(Vector2(0,16));
    col->add_child(gBatchProgress);

    // Status label
    gBatchStatusLabel = memnew(Label);
    gBatchStatusLabel->set_text("  Ready");
    gBatchStatusLabel->add_theme_color_override("font_color", C_TEXT_DIM);
    col->add_child(gBatchStatusLabel);

    auto* sep2 = memnew(HSeparator); col->add_child(sep2);

    // Batch log
    auto* logHdr = memnew(Label); logHdr->set_text("  Conversion Log"); col->add_child(logHdr);
    gBatchLog = memnew(TextEdit);
    gBatchLog->set_editable(false);
    gBatchLog->set_custom_minimum_size(Vector2(0, 280));
    gBatchLog->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    col->add_child(gBatchLog);

    return root;
}

// ============================================================================
// UI Construction — About Tab
// ============================================================================
static Control* buildAboutTab() {
    auto* root = memnew(ScrollContainer);
    root->set_name(S(std::string(ICON_FA_CIRCLE_INFO) + "  About"));
    root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    root->set_v_size_flags(Control::SIZE_EXPAND_FILL);

    auto* margin = memnew(MarginContainer);
    margin->add_theme_constant_override("margin_left",   32);
    margin->add_theme_constant_override("margin_right",  32);
    margin->add_theme_constant_override("margin_top",    32);
    margin->add_theme_constant_override("margin_bottom", 32);
    root->add_child(margin);

    auto* vbox = memnew(VBoxContainer);
    vbox->add_theme_constant_override("separation", 14);
    vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    margin->add_child(vbox);

    auto title = memnew(Label);
    title->set_text("Ruby / Swordigo Studio");
    title->add_theme_font_size_override("font_size", 28);
    title->add_theme_color_override("font_color", C_ACCENT);
    title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    vbox->add_child(title);

    auto sub = memnew(Label);
    sub->set_text("Godot Native Frontend · v1.0.0");
    sub->add_theme_color_override("font_color", C_TEXT_DIM);
    sub->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    vbox->add_child(sub);

    auto* sep = memnew(HSeparator); vbox->add_child(sep);

    auto addSection = [&](const char* heading, const char* body) {
        auto* hdr = memnew(Label);
        hdr->set_text(S(heading));
        hdr->add_theme_color_override("font_color", C_WARNING);
        vbox->add_child(hdr);
        auto* txt = memnew(Label);
        txt->set_text(S(body));
        txt->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
        txt->add_theme_color_override("font_color", C_TEXT);
        vbox->add_child(txt);
    };

    addSection("About",
        "Ruby / Swordigo Studio is a professional-grade asset editor and scene toolkit "
        "for Swordigo, built on Godot as a native C++ rendering substrate.\n\n"
        "It provides faithful 3D scene visualization, PVR/TEX texture decoding, "
        "POD model import, procedural scene generation (V1/V2/V3/V2-3D), batch "
        "texture conversion, and full protobuf schema decoding.");

    addSection("Asset Support",
        "• POD — PowerVR 3D Model (mesh, material, animation, camera)\n"
        "• SCENE — Swordigo Scene Graph (entities, ground meshes, lights, water)\n"
        "• PVR — PowerVR Compressed Texture (ETC1/PVRTC, hardware-decoded)\n"
        "• TEX / TEX.PNG — Swordigo gzip-wrapped texture containers\n"
        "• FNT — Swordigo Font (protobuf schema)\n"
        "• ATLAS — Sprite atlas (protobuf schema)\n"
        "• SCL — Swordigo Script Library\n"
        "• WAV / OGG — Audio assets (metadata)");

    addSection("Scene Generator Versions",
        "• V1 — Manual blueprint mode (polygon platforms, hat domes, water)\n"
        "• V2 — Biome-driven Perlin+fBm terrain, 8 biomes, torch/glow placement\n"
        "• V3 — Ultimate biome database (correct textures, caves, multi-parallax)\n"
        "• V2-3D — Full Minecraft-style 3D world on XYZ depth grid");

    addSection("Credits",
        "Engine: Godot 4.x (MIT License)\n"
        "Format research: Swordigo community\n"
        "Boulder terrain: C++ port of DanielSpaniel's Boulder / Caver engine\n"
        "Texture decoder: PVRTexTool SDK (Imagination Technologies)\n"
        "Asset parser: Custom protobuf + filerift decoder");

    addSection("Build Info",
        "Compiled with SWORDIGO_NO_IMGUI + GODOT_ENABLED\n"
        "Godot substrate: libgodot.so (linked at runtime)\n"
        "Backends: swcore, swfmt, swpod, filerift");

    return root;
}

// ============================================================================
// UI Construction — Main buildUI()
// ============================================================================
static void buildUI() {
    gWindow = SceneTree::get_singleton()->get_root();
    gWindow->set_title("Ruby  /  Swordigo Studio  —  Godot Native");
    // Borderless custom-title-bar window (mirrors the ImGui Ruby edition's
    // OS-chrome-free app frame: we draw our own branded bar + window controls).
    gWindow->set_flag(Window::FLAG_BORDERLESS, true);
    applyTheme(gWindow);
    gWindow->connect("close_requested", Callable((Node*)ruby_gg_handler(), "_on_exit"));

    // ── Root Layout ──────────────────────────────────────────────────────────
    auto* rootVBox = memnew(VBoxContainer);
    rootVBox->set_anchors_preset(Control::PRESET_FULL_RECT);
    rootVBox->add_theme_constant_override("separation", 0);
    gWindow->add_child(rootVBox);

    // ── 0. Custom Title Bar (branding + menus + window controls) ─────────────
    {
        auto* titleBar = memnew(RubyTitleBar(gWindow));
        titleBar->set_custom_minimum_size(Size2i(0, 34));
        titleBar->add_theme_style_override("panel", makeBox(C_BASE, 0, 0, 0, C_BORDER, 1));
        rootVBox->add_child(titleBar);

        auto* hb = memnew(HBoxContainer);
        hb->add_theme_constant_override("separation", 10);
        hb->set_anchors_preset(Control::PRESET_FULL_RECT);
        hb->add_child(memnew(Control)); // left spacer so drag region starts
        titleBar->add_child(hb);

        // Branding (styled like the ImGui Ruby app bar)
        auto* brand = memnew(Label);
        brand->set_text("Ruby");
        brand->set_modulate(C_ACCENT);
        brand->add_theme_font_size_override("font_size", 15);
        hb->add_child(brand);
        auto* brand2 = memnew(Label);
        brand2->set_text("SWORDIGO STUDIO");
        brand2->set_modulate(C_TEXT);
        brand2->add_theme_color_override("font_color", Color(0.75f, 0.78f, 0.85f, 1.0f));
        brand2->add_theme_font_size_override("font_size", 11);
        hb->add_child(brand2);
        auto* sep = memnew(Label);
        sep->set_text("|");
        sep->set_modulate(C_TEXT);
        sep->add_theme_color_override("font_color", Color(0.4f, 0.42f, 0.5f, 1.0f));
        hb->add_child(sep);

        auto* menuBar = memnew(MenuBar);
        menuBar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        menuBar->set_anchors_preset(Control::PRESET_FULL_RECT);
        hb->add_child(menuBar);

        auto* fm = memnew(PopupMenu);
        fm->add_item("Open Workspace", 100);
        fm->add_item("Save Document", 430);
        fm->add_separator();
        fm->add_item("Exit", 199);
        menuBar->add_child(fm);
        menuBar->set_menu_title(0, "File");

        auto* vm = memnew(PopupMenu);
        vm->add_item("Reset Camera",  300);
        vm->add_item("Toggle Grid",   301);
        menuBar->add_child(vm);
        menuBar->set_menu_title(1, "View");

        auto* tm = memnew(PopupMenu);
        tm->add_item("Scene Generator", 401);
        tm->add_item("Batch Converter", 402);
        menuBar->add_child(tm);
        menuBar->set_menu_title(2, "Tools");

        auto* hm = memnew(PopupMenu);
        hm->add_item("About Swordigo Studio", 500);
        menuBar->add_child(hm);
        menuBar->set_menu_title(3, "Help");

        if (ruby_gg_handler()) {
            Node* hn = (Node*)ruby_gg_handler();
            fm->connect("id_pressed", Callable(hn, "_on_menu"));
            vm->connect("id_pressed", Callable(hn, "_on_menu"));
            tm->connect("id_pressed", Callable(hn, "_on_menu"));
            hm->connect("id_pressed", Callable(hn, "_on_menu"));
        }

        // Window controls (min / max / close) — right aligned.
        auto* minBtn = memnew(RubyWindowButton(gWindow, 2));
        minBtn->set_text("_");
        minBtn->set_custom_minimum_size(Size2i(34, 26));
        minBtn->add_theme_style_override("normal", makeBox(C_BASE, 0, 0, 0, C_BORDER, 1));
        minBtn->add_theme_style_override("hover",  makeBox(Color(0.28f, 0.30f, 0.36f, 1.0f), 0, 0, 0));
        minBtn->add_theme_style_override("pressed", makeBox(Color(0.20f, 0.22f, 0.28f, 1.0f), 0, 0, 0));
        hb->add_child(minBtn);

        auto* maxBtn = memnew(RubyWindowButton(gWindow, 1));
        maxBtn->set_text("▢");
        maxBtn->set_custom_minimum_size(Size2i(34, 26));
        maxBtn->add_theme_style_override("normal", makeBox(C_BASE, 0, 0, 0, C_BORDER, 1));
        maxBtn->add_theme_style_override("hover",  makeBox(Color(0.28f, 0.30f, 0.36f, 1.0f), 0, 0, 0));
        maxBtn->add_theme_style_override("pressed", makeBox(Color(0.20f, 0.22f, 0.28f, 1.0f), 0, 0, 0));
        hb->add_child(maxBtn);

        auto* closeBtn = memnew(RubyWindowButton(gWindow, 0));
        closeBtn->set_text("✕");
        closeBtn->set_custom_minimum_size(Size2i(34, 26));
        closeBtn->add_theme_color_override("font_color", Color(0.85f, 0.45f, 0.45f, 1.0f));
        closeBtn->add_theme_style_override("normal", makeBox(C_BASE, 0, 0, 0, C_BORDER, 1));
        closeBtn->add_theme_style_override("hover",  makeBox(Color(0.45f, 0.18f, 0.18f, 1.0f), 0, 0, 0));
        closeBtn->add_theme_style_override("pressed", makeBox(Color(0.32f, 0.12f, 0.12f, 1.0f), 0, 0, 0));
        hb->add_child(closeBtn);
    }

    // ── 2. Toolbar ───────────────────────────────────────────────────────────
    {
        auto* pc = memnew(PanelContainer);
        pc->add_theme_style_override("panel", makeBox(C_BASE, 0, 4, 6, C_BORDER, 1));
        rootVBox->add_child(pc);

        auto* tb = memnew(HBoxContainer);
        tb->set_custom_minimum_size(Vector2(0, 36));
        tb->add_theme_constant_override("separation", 4);
        pc->add_child(tb);

        auto mkBtn = [](const char* txt) -> Button* {
            auto* b = memnew(Button); b->set_text(S(txt));
            b->set_custom_minimum_size(Vector2(0, 28));
            return b;
        };

        auto* btnWS = mkBtn("📂  Open Workspace");
        tb->add_child(btnWS);
        if (ruby_gg_handler()) btnWS->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_menu").bind(100));

        auto* btnCam = mkBtn("↺  Reset View");
        tb->add_child(btnCam);
        if (ruby_gg_handler()) btnCam->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_reset"));

        auto* btnGrid = mkBtn("⊞  Grid");
        tb->add_child(btnGrid);
        if (ruby_gg_handler()) btnGrid->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_menu").bind(301));

        auto* vsep1 = memnew(VSeparator); tb->add_child(vsep1);

        gPathBar = memnew(LineEdit);
        gPathBar->set_editable(false);
        gPathBar->set_placeholder("No workspace open — use File → Open Workspace");
        gPathBar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        tb->add_child(gPathBar);
    }

    // ── 3. Main Workspace (left | center | right) ─────────────────────────────
    auto* mainHSplit = memnew(HSplitContainer);
    mainHSplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    mainHSplit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    mainHSplit->add_theme_constant_override("separation", 3);
    rootVBox->add_child(mainHSplit);

    // ── LEFT PANEL: Asset Browser ────────────────────────────────────────────
    {
        auto* pc = memnew(PanelContainer);
        pc->set_custom_minimum_size(Vector2(255, 0));
        pc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        pc->add_theme_style_override("panel", makeBox(C_PANEL, 0, 0, 0, C_BORDER, 1));
        mainHSplit->add_child(pc);

        auto* vbox = memnew(VBoxContainer);
        vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        vbox->add_theme_constant_override("separation", 4);
        pc->add_child(vbox);

        auto* headerPC = memnew(PanelContainer);
        headerPC->add_theme_style_override("panel", makeBox(C_BASE, 0, 4, 8));
        vbox->add_child(headerPC);

        auto* headerHB = memnew(HBoxContainer);
        headerPC->add_child(headerHB);
        auto* headerLbl = memnew(Label);
        headerLbl->set_text("ASSET BROWSER");
        headerLbl->add_theme_color_override("font_color", C_TEXT_DIM);
        headerLbl->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        headerHB->add_child(headerLbl);

        gSearchEdit = memnew(LineEdit);
        gSearchEdit->set_placeholder("Filter assets…");
        gSearchEdit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        vbox->add_child(gSearchEdit);
        if (ruby_gg_handler()) {
            gSearchEdit->connect("text_changed", Callable((Node*)ruby_gg_handler(), "_on_search"));
        }

        gFileTree = memnew(Tree);
        gFileTree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        gFileTree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        gFileTree->set_hide_root(true);
        gFileTree->set_allow_rmb_select(true);
        vbox->add_child(gFileTree);
        if (ruby_gg_handler()) {
            gFileTree->connect("item_activated",  Callable((Node*)ruby_gg_handler(), "_on_file"));
            gFileTree->connect("cell_selected",   Callable((Node*)ruby_gg_handler(), "_on_file"));
        }
    }

    // ── CENTER + RIGHT SPLIT ─────────────────────────────────────────────────
    auto* centerRightSplit = memnew(HSplitContainer);
    centerRightSplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    centerRightSplit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    centerRightSplit->add_theme_constant_override("separation", 3);
    mainHSplit->add_child(centerRightSplit);

    // ── CENTER: tab container (Asset Preview | Scene Generator | Batch | About)
    {
        auto* centerVSplit = memnew(VSplitContainer);
        centerVSplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        centerVSplit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        centerVSplit->add_theme_constant_override("separation", 3);
        centerRightSplit->add_child(centerVSplit);

        gCenterTabs = memnew(TabContainer);
        gCenterTabs->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        gCenterTabs->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        centerVSplit->add_child(gCenterTabs);

        // Tab 0 — Asset Viewer (3D viewport + 2D texture panel switcher)
        {
            // Outer wrapper used as the tab page
            auto* viewerPage = memnew(Control);
            viewerPage->set_name(S(std::string(ICON_FA_EYE) + "  Asset Preview"));
            viewerPage->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            viewerPage->set_h_size_flags(Control::SIZE_EXPAND_FILL);
            gCenterTabs->add_child(viewerPage);

            // ViewerArea fills the full tab page via anchors
            gViewerArea = memnew(Control);
            gViewerArea->set_anchors_preset(Control::PRESET_FULL_RECT);
            viewerPage->add_child(gViewerArea);

            // ── 3D SubViewport Container ──────────────────────────────────
            gViewportContainer = memnew(SubViewportContainer);
            gViewportContainer->set_stretch(true);
            gViewportContainer->set_anchors_preset(Control::PRESET_FULL_RECT);
            gViewerArea->add_child(gViewportContainer);

            gSubViewport = memnew(SubViewport);
            gSubViewport->set_use_hdr_2d(false);
            gSubViewport->set_size(Vector2i(1280, 720));
            gViewportContainer->add_child(gSubViewport);

            gSceneRoot = memnew(Node3D);
            gSubViewport->add_child(gSceneRoot);

            gSceneEntitiesRoot = memnew(Node3D);
            gSceneRoot->add_child(gSceneEntitiesRoot);

            gCamera3D = memnew(Camera3D);
            gCamera3D->set_current(true);
            gSceneRoot->add_child(gCamera3D);

            // World Environment with ACES Tonemapping & Cinematic Ambient
            auto* envNode = memnew(WorldEnvironment);
            Ref<Environment> env; env.instantiate();
            env->set_background(Environment::BG_COLOR);
            env->set_bg_color(Color(0.065f, 0.070f, 0.090f));
            env->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
            env->set_ambient_light_color(Color(0.35f, 0.38f, 0.45f));
            env->set_ambient_light_energy(1.25f);
            env->set_tonemapper(Environment::TONE_MAPPER_ACES);
            env->set_tonemap_exposure(1.15f);
            envNode->set_environment(env);
            gSceneRoot->add_child(envNode);

            // Key Sun Light (Warm Golden, angled from top-front-right)
            auto* dlKey = memnew(DirectionalLight3D);
            dlKey->set_rotation(Vector3(-0.785f, 0.610f, 0.0f)); // pitch -45°, yaw +35°
            dlKey->set_color(Color(1.0f, 0.94f, 0.82f));
            dlKey->set_param(Light3D::PARAM_ENERGY, 1.50f);
            dlKey->set_param(Light3D::PARAM_SPECULAR, 0.50f);
            dlKey->set_shadow(true);
            gSceneRoot->add_child(dlKey);

            // Fill Sky Light (Cool blue fill from opposite hemisphere)
            auto* dlFill = memnew(DirectionalLight3D);
            dlFill->set_rotation(Vector3(0.523f, -2.530f, 0.0f)); // pitch +30°, yaw -145°
            dlFill->set_color(Color(0.40f, 0.50f, 0.70f));
            dlFill->set_param(Light3D::PARAM_ENERGY, 0.75f);
            dlFill->set_param(Light3D::PARAM_SPECULAR, 0.15f);
            dlFill->set_shadow(false);
            gSceneRoot->add_child(dlFill);

            // Front Camera Fill Light (ensures front of models are always readable)
            auto* dlFront = memnew(DirectionalLight3D);
            dlFront->set_rotation(Vector3(-0.261f, 0.0f, 0.0f)); // pitch -15°, yaw 0°
            dlFront->set_color(Color(0.85f, 0.88f, 0.95f));
            dlFront->set_param(Light3D::PARAM_ENERGY, 0.65f);
            dlFront->set_param(Light3D::PARAM_SPECULAR, 0.25f);
            dlFront->set_shadow(false);
            gSceneRoot->add_child(dlFront);

            buildGrid();

            gModelInstance = memnew(MeshInstance3D);
            gModelInstance->set_visible(false);
            gSceneRoot->add_child(gModelInstance);

            // Background TextureRect (in 3D scene as fullscreen backdrop)
            gSceneBackground = memnew(TextureRect);
            gSceneBackground->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
            gSceneBackground->set_anchors_preset(Control::PRESET_FULL_RECT);
            gSceneBackground->set_visible(false);
            gSceneBackground->set_z_index(-10);
            gViewportContainer->add_child(gSceneBackground);

            // ── 2D Texture Viewer Panel ──────────────────────────────────
            // Sits on top of gViewportContainer in the same gViewerArea,
            // but HIDDEN until a texture is selected. z_index ensures it
            // draws on top of the 3D sub-viewport.
            gTexPanel = memnew(Panel);
            gTexPanel->set_anchors_preset(Control::PRESET_FULL_RECT);
            gTexPanel->add_theme_style_override("panel", makeBox(Color(0.08f,0.08f,0.10f), 0, 0, 0));
            gTexPanel->set_visible(false);
            gTexPanel->set_z_index(10);
            gViewerArea->add_child(gTexPanel);

            auto* texVBox = memnew(VBoxContainer);
            texVBox->set_anchors_preset(Control::PRESET_FULL_RECT);
            texVBox->add_theme_constant_override("separation", 0);
            gTexPanel->add_child(texVBox);

            // Info bar at top
            auto* infoBar = memnew(PanelContainer);
            infoBar->add_theme_style_override("panel", makeBox(C_PANEL, 0, 4, 8, C_BORDER, 1));
            texVBox->add_child(infoBar);

            auto* infoHBox = memnew(HBoxContainer);
            infoBar->add_child(infoHBox);

            gTexInfoLabel = memnew(Label);
            gTexInfoLabel->set_text("  2D Texture Viewer");
            gTexInfoLabel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
            infoHBox->add_child(gTexInfoLabel);

            // Zoom buttons
            auto makeZoomBtn = [&](const char* txt, int zid) -> Button* {
                auto* b = memnew(Button);
                b->set_text(S(txt));
                if (ruby_gg_handler())
                    b->connect("pressed", Callable((Node*)ruby_gg_handler(), "_on_vmenu").bind(zid));
                return b;
            };
            infoHBox->add_child(makeZoomBtn("Fit", 420));
            infoHBox->add_child(makeZoomBtn("1×",  421));
            infoHBox->add_child(makeZoomBtn("2×",  422));
            infoHBox->add_child(makeZoomBtn("4×",  423));

            // Scroll + Center for the texture itself
            auto* scrollArea = memnew(ScrollContainer);
            scrollArea->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            scrollArea->set_h_size_flags(Control::SIZE_EXPAND_FILL);
            texVBox->add_child(scrollArea);

            auto* center = memnew(CenterContainer);
            center->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            center->set_h_size_flags(Control::SIZE_EXPAND_FILL);
            center->set_custom_minimum_size(Vector2(512, 512));
            scrollArea->add_child(center);

            g2DTextureRect = memnew(TextureRect);
            g2DTextureRect->set_stretch_mode(TextureRect::STRETCH_KEEP);
            g2DTextureRect->set_expand_mode(TextureRect::EXPAND_KEEP_SIZE);
            center->add_child(g2DTextureRect);
        }

        // Tab 1 — Native text and FileRift source editor
        {
            gEditorIntelliJ.load_style_from_memory(intel::BAT_SYNTAX_STYX_CONTENT);
            gCenterTabs->add_child(buildCodeEditorTab());
        }

        // Tab 2 — Scene Generator
        {
            auto* sgenTab = buildSceneGeneratorTab();
            gCenterTabs->add_child(sgenTab);
        }

        // Tab 3 — Batch Converter
        {
            auto* batchTab = buildBatchConverterTab();
            gCenterTabs->add_child(batchTab);
        }

        // Tab 4 — About
        {
            auto* aboutTab = buildAboutTab();
            gCenterTabs->add_child(aboutTab);
        }

        // Bottom dock — Output Console
        auto* bottomPC = memnew(PanelContainer);
        bottomPC->set_custom_minimum_size(Vector2(0, 130));
        bottomPC->set_v_size_flags(Control::SIZE_FILL);
        bottomPC->add_theme_style_override("panel", makeBox(C_BASE, 0, 0, 0, C_BORDER, 1));
        centerVSplit->add_child(bottomPC);

        auto* botVBox = memnew(VBoxContainer);
        botVBox->add_theme_constant_override("separation", 0);
        bottomPC->add_child(botVBox);

        auto* consoleTitleBar = memnew(PanelContainer);
        consoleTitleBar->add_theme_style_override("panel", makeBox(C_PANEL, 0, 3, 6));
        botVBox->add_child(consoleTitleBar);
        auto* consoleTitleLbl = memnew(Label);
        consoleTitleLbl->set_text("  OUTPUT CONSOLE");
        consoleTitleLbl->add_theme_color_override("font_color", C_TEXT_DIM);
        consoleTitleBar->add_child(consoleTitleLbl);

        gLogTextEdit = memnew(TextEdit);
        gLogTextEdit->set_editable(false);
        gLogTextEdit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        gLogTextEdit->add_theme_color_override("font_color", Color(0.78f, 0.88f, 0.72f));
        botVBox->add_child(gLogTextEdit);
    }

    // ── RIGHT PANEL: Inspector + Scene Hierarchy ─────────────────────────────
    {
        auto* pc = memnew(PanelContainer);
        pc->set_custom_minimum_size(Vector2(270, 0));
        pc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        pc->add_theme_style_override("panel", makeBox(C_PANEL, 0, 0, 0, C_BORDER, 1));
        centerRightSplit->add_child(pc);

        auto* rightVSplit = memnew(VSplitContainer);
        rightVSplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
        rightVSplit->add_theme_constant_override("separation", 3);
        pc->add_child(rightVSplit);

        // Top-right: Asset Inspector
        {
            auto* vbox = memnew(VBoxContainer);
            vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            vbox->add_theme_constant_override("separation", 2);
            rightVSplit->add_child(vbox);

            auto* hdr = memnew(PanelContainer);
            hdr->add_theme_style_override("panel", makeBox(C_BASE, 0, 3, 6));
            vbox->add_child(hdr);
            auto* lbl = memnew(Label); lbl->set_text("ASSET INSPECTOR");
            lbl->add_theme_color_override("font_color", C_TEXT_DIM);
            hdr->add_child(lbl);

            gInspectorTree = memnew(Tree);
            gInspectorTree->set_columns(2);
            gInspectorTree->set_column_titles_visible(true);
            gInspectorTree->set_column_title(0, "Property");
            gInspectorTree->set_column_title(1, "Value");
            gInspectorTree->set_column_expand(0, false);
            gInspectorTree->set_column_custom_minimum_width(0, 110);
            gInspectorTree->set_column_expand(1, true);
            gInspectorTree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            vbox->add_child(gInspectorTree);
        }

        // Middle-right: Scene Hierarchy
        {
            auto* vbox = memnew(VBoxContainer);
            vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            vbox->add_theme_constant_override("separation", 2);
            rightVSplit->add_child(vbox);

            auto* hdr = memnew(PanelContainer);
            hdr->add_theme_style_override("panel", makeBox(C_BASE, 0, 3, 6));
            vbox->add_child(hdr);
            auto* lbl = memnew(Label); lbl->set_text("SCENE HIERARCHY");
            lbl->add_theme_color_override("font_color", C_TEXT_DIM);
            hdr->add_child(lbl);

            gSceneTree = memnew(Tree);
            gSceneTree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            gSceneTree->set_hide_root(false);
            vbox->add_child(gSceneTree);
            if (ruby_gg_handler()) {
                gSceneTree->connect("cell_selected", Callable((Node*)ruby_gg_handler(), "_on_file"));
            }

            auto* hdr2 = memnew(PanelContainer);
            hdr2->add_theme_style_override("panel", makeBox(C_BASE, 0, 3, 6));
            vbox->add_child(hdr2);
            auto* lbl2 = memnew(Label); lbl2->set_text("ENTITY PROPERTIES");
            lbl2->add_theme_color_override("font_color", C_TEXT_DIM);
            hdr2->add_child(lbl2);

            gPropsTree = memnew(Tree);
            gPropsTree->set_columns(2);
            gPropsTree->set_column_titles_visible(true);
            gPropsTree->set_column_title(0, "Attribute");
            gPropsTree->set_column_title(1, "Value");
            gPropsTree->set_column_expand(0, false);
            gPropsTree->set_column_custom_minimum_width(0, 100);
            gPropsTree->set_column_expand(1, true);
            gPropsTree->set_custom_minimum_size(Vector2(0, 160));
            gPropsTree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
            vbox->add_child(gPropsTree);
        }
    }

    // ── 4. Status Bar ────────────────────────────────────────────────────────
    {
        auto* pc = memnew(PanelContainer);
        pc->add_theme_style_override("panel", makeBox(C_BASE, 0, 3, 8, C_BORDER, 1));
        rootVBox->add_child(pc);

        auto* hb = memnew(HBoxContainer);
        pc->add_child(hb);

        auto* dot = memnew(Label);
        dot->set_text("●");
        dot->add_theme_color_override("font_color", C_SUCCESS);
        hb->add_child(dot);

        gStatusLabel = memnew(Label);
        gStatusLabel->set_text(" Ready  |  Ruby / Swordigo Studio  |  Godot Substrate");
        gStatusLabel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        hb->add_child(gStatusLabel);
    }

    // ── Register Callbacks ───────────────────────────────────────────────────
    ruby_gg_set_cb_exit(cb_exit);
    ruby_gg_set_cb_reset(cb_reset);
    ruby_gg_set_cb_file(cb_file);
    ruby_gg_set_cb_search(cb_search);
    ruby_gg_set_cb_menu(cb_menu_full);
    ruby_gg_set_cb_viewmenu(cb_viewmenu);

    loadDir(App.workspace.c_str());
    app_log("Ruby / Swordigo Studio v1.0 — Godot Native Frontend ready.");
    app_log("Open a workspace via File → Open Workspace, or click a directory in the Asset Browser.");
}

// ============================================================================
// Entry Point
// ============================================================================
int main(int argc, char* argv[]) {
    fprintf(stderr,
        "\n  ╔══════════════════════════════════════════════════════╗\n"
        "  ║   Ruby / Swordigo Studio  v1.0  (Godot Native)      ║\n"
        "  ╚══════════════════════════════════════════════════════╝\n\n");

    const char* home = getenv("HOME");
    App.workspace = std::string(home ? home : ".") + "/.local/share/swordigo-desktop";

    bool headless = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            fprintf(stderr, "Usage: %s [--workspace PATH] [--headless]\n", argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            fprintf(stderr, "ruby_gg v1.0.0 (Godot Native)\n");
            return 0;
        }
        if (!strcmp(argv[i], "--workspace") && i+1 < argc) App.workspace = argv[++i];
        if (!strcmp(argv[i], "--headless"))                headless = true;
    }

    std::vector<char*> args = {argv[0]};
    if (headless) { static std::string hl = "--headless"; args.push_back(const_cast<char*>(hl.c_str())); }

    if (ruby_gg_setup(args.size(), args.data()))  { fprintf(stderr, "[ruby_gg] setup failed\n");  return 1; }
    if (ruby_gg_setup2())                          { fprintf(stderr, "[ruby_gg] setup2 failed\n"); return 1; }
    if (ruby_gg_start())                           { fprintf(stderr, "[ruby_gg] start failed\n");  return 1; }

    buildUI();

    // Main loop
    while (App.isRunning && ruby_gg_iteration()) {
        // ── Deferred navigation ──────────────────────────────────────────────
        if (!App.pendingDir.empty()) {
            std::string d = App.pendingDir; App.pendingDir.clear();
            loadDir(d.c_str());
        }
        if (!App.pendingFile.empty()) {
            std::string f = App.pendingFile; App.pendingFile.clear();
            loadFile(f.c_str());
            if (gCenterTabs) gCenterTabs->set_current_tab(editorFiletypeForPath(f).empty() &&
                                                          lowerCopy(std::filesystem::path(f).extension().string()) != "lua" &&
                                                          lowerCopy(std::filesystem::path(f).extension().string()) != "json" &&
                                                          lowerCopy(std::filesystem::path(f).extension().string()) != "txt" &&
                                                          lowerCopy(std::filesystem::path(f).extension().string()) != "gmesh" ? 0 : 1);
        }
        if (App.pendingSceneObjIndex >= 0) {
            int idx = App.pendingSceneObjIndex; App.pendingSceneObjIndex = -1;
            updateSceneObjectProperties(idx);
        }

        // ── Camera input (only when 3D viewport is active & visible) ─────────
        if (gViewportContainer && gViewportContainer->is_visible() &&
            gCenterTabs && gCenterTabs->get_current_tab() == 0) {

            DisplayServer* ds = DisplayServer::get_singleton();
            if (ds) {
                Vector2i cur = ds->mouse_get_position();
                auto btn = ds->mouse_get_button_state();
                int dx = cur.x - App.lastMousePos.x;
                int dy = cur.y - App.lastMousePos.y;
                bool smallMove = abs(dx) < 100 && abs(dy) < 100;
                if (btn.has_flag(MouseButtonMask::LEFT) && smallMove)
                    App.camera.orbit(-dx * 0.4f, -dy * 0.4f);
                else if ((btn.has_flag(MouseButtonMask::RIGHT) || btn.has_flag(MouseButtonMask::MIDDLE)) && smallMove)
                    App.camera.pan(dx * 0.5f, dy * 0.5f);
                App.lastMousePos = cur;
            }
        } else {
            // Reset last mouse pos when viewport not focused to avoid jump
            if (DisplayServer* ds = DisplayServer::get_singleton())
                App.lastMousePos = ds->mouse_get_position();
        }

        if (gCamera3D) App.camera.apply(gCamera3D);

        // ── POD model instance update ─────────────────────────────────────────
        if (gModelInstance) {
            if (App.previewType == 1 && App.podMesh.is_valid()) {
                gModelInstance->set_mesh(App.podMesh);
                gModelInstance->set_visible(true);
            } else {
                gModelInstance->set_visible(false);
            }
        }
        if (gGridInstance) gGridInstance->set_visible(App.showGrid && App.previewType != 2 && App.previewType != 5 && App.previewType != 6);

        // ── Batch converter UI update ─────────────────────────────────────────
        if (App.batchState.running.load() || App.batchState.finished.load()) {
            updateBatchUI();
        }

        if (gCodeEdit && !App.editorPath.empty()) {
            App.editorDirty = gCodeEdit->get_version() != gCodeEdit->get_saved_version();
            if (App.editorDirty && gEditorStatus && App.editorDiagnostic.empty()) updateEditorStatus();
        }

        usleep(14000); // ~71 fps ceiling
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    app_log("Shutting down...");
    batch::shutdown_batch(App.batchState);
    gGridMesh.unref();
    App.podMesh.unref();
    App.previewTexture.unref();
    App.meshCache.clear();
    App.textureCache.clear();
    clearSceneEntities();
    ruby_gg_cleanup();
    fprintf(stderr, "[ruby_gg] Exited cleanly.\n");
    _Exit(0);
}
