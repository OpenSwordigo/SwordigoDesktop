// =============================================================================
// Swordigo Desktop — ImGui Launcher UI  (v9.0 Platform Remaster)
// Full Dear ImGui replacement for the old raw-OpenGL launcher.
// SDL3 + OpenGL 3.3 Core + ImGui v1.91.x + Font Awesome 6
// =============================================================================

#include "platform/launcher_ui.h"
#include "platform/data_path.h"
#include "platform/save_editor.h"
#include "platform/IconsFontAwesome6.h"
#include "platform/pvr_loader.h"
#include "platform/embedded_assets.h"
#include "platform/unicorn_dyn.h"
#include "platform/os_external.h"
#include "platform/mod_manager.h"
#include "platform/launcher_config.h"
#include "platform/swordfare_theme.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl3.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <cmath>
#include <future>

namespace fs = std::filesystem;

// Resolve the ruby asset viewer and launch it as a separate process.
// Prefers a ruby binary next to the running executable so the viewer works
// regardless of the current working directory.
static void launch_ruby_viewer() {
    std::string path = "./ruby";
    if (!fs::exists(path)) {
        std::string exe_dir = os_external::exe_dir();
        if (!exe_dir.empty()) {
            path = (fs::path(exe_dir) / "ruby").string();
        }
    }
    if (fs::exists(path)) {
        os_external::spawn_detached(path, {});
    } else {
        os_external::spawn_detached("ruby", {});
        std::cerr << "[Launcher] ruby not found next to binary — tried PATH" << std::endl;
    }
}

// =============================================================================
// Forward declarations (internal helpers)
// =============================================================================

// =============================================================================
// Navigation pages + forward declarations
// =============================================================================

enum class LauncherPage {
    HOME,
    LIBRARY,
    MODS,
    MOD_BROWSER,
    PROFILE,
    SETTINGS,
    SDK_TOOLS
};

static LauncherPage g_page        = LauncherPage::HOME;
static bool         g_show_save_ed = false;

static void      ApplyPremiumTheme();
static GLuint    LoadTextureFromFile(const char* path, int* out_w, int* out_h);
static void      DrawWindowControls(bool& running, LaunchConfig& cfg);
static void      DrawSidebar(BinarySelector& selector, int& selected, bool& running, LaunchConfig& cfg);
static void      DrawContentArea(BinarySelector& selector, int& selected,
                                 LaunchConfig& cfg, bool& running,
                                 int& api_sel, int& engine_sel,
                                 bool& use_sre_sel, bool& adv_opts_sel);
static void      DrawHomePage(BinarySelector& selector, int& selected,
                              LaunchConfig& cfg, bool& running,
                              int& api_sel, int& engine_sel,
                              bool& use_sre_sel, bool& adv_opts_sel);
static void      DrawLibraryPage(BinarySelector& selector, int& selected,
                                 LaunchConfig& cfg, bool& running,
                                 int& api_sel, int& engine_sel,
                                 bool& use_sre_sel, bool& adv_opts_sel);
static void      DrawModsPage();
static void      DrawModBrowserPage();
static void      DrawProfilePage();
static void      DrawSettingsPage();
static void      DrawSDKToolsPage();
static void      DrawSaveEditor(bool& show_save_editor);
static void      DrawStatusFooter(int selected, const BinarySelector& selector);

// =============================================================================
// Module-level state
// =============================================================================

struct ModInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string type;
    std::string category;
    std::string dir_path;
    std::string icon_path;
    GLuint      icon_tex   = 0;
    int         icon_w = 0, icon_h = 0;
    bool        enabled = true;
    bool        is_toml = false;
};

// GL texture cache for mod icons (loaded lazily from icon.png)
static GLuint LoadModIcon(const ModInfo& mod);
static void   FreeModIcon(ModInfo& mod);

static GLuint g_tex_bg = 0;
static int    g_tex_bg_w = 0, g_tex_bg_h = 0;
static GLuint g_tex_logo = 0;
static int    g_tex_logo_w = 0, g_tex_logo_h = 0;
static GLuint g_tex_icon_swordigo   = 0;
static GLuint g_tex_icon_swmini     = 0;
static GLuint g_tex_icon_rlswordigo = 0;
static GLuint g_tex_icon_app        = 0;
static int    g_icon_w = 0, g_icon_h = 0;

static std::map<std::string, GLuint> g_custom_icon_cache;
static SDL_Window* g_sdl_window = nullptr;

static const float SIDEBAR_W   = 240.0f;
static const float STATUS_H    = 28.0f;
static const float WIN_CTRL_W  = 120.0f;

static ImFont* g_font_main    = nullptr;
static ImFont* g_font_heading = nullptr;

static std::vector<ModInfo> g_mods;
static bool g_mods_scanned = false;
static int  g_active_mod_idx = -1;  // index into g_mods of the currently selected mod for VFS

// Mod delete confirmation (separate from the instance one above)
static bool g_mod_confirm_delete    = false;
static int  g_mod_delete_target_idx = -1;

static bool                     g_save_loaded      = false;
static std::vector<std::string> g_save_paths;
static std::vector<SaveFile>    g_save_files;
static int                      g_save_sel         = -1;
static SaveFile                 g_edit_save;
static std::string              g_save_status;
static bool                     g_save_status_ok   = false;

static bool g_confirm_delete    = false;
static int  g_delete_target_idx = -1;

// CPU-backend availability: Dynarmic is always available when compiled in;
// Unicorn is optional and runtime-loaded, so it may be missing. When the user
// picks a backend that cannot run, we surface a modal instead of launching.
static bool g_show_backend_warning = false;
static std::string g_backend_warning;

static bool backend_selection_ok(bool use_dynarmic) {
    if (use_dynarmic) {
#ifdef SWORDIGO_HAS_DYNARMIC
        return true;
#else
        // Dynarmic not compiled in; fall through to the Unicorn requirement.
        return backend_selection_ok(false);
#endif
    }
    if (unicorn_backend_available()) return true;
    g_backend_warning =
        "The Unicorn CPU engine is not available.\n\n"
        "Drop libunicorn.so (Linux) or unicorn.dll (Windows) next to Swordigo Desktop,\n"
        "or install the Unicorn Engine package (e.g. libunicorn-dev), then restart.\n\n"
        "Alternatively pick Dynarmic (JIT) above - it is bundled and always works.";
    g_show_backend_warning = true;
    return false;
}

static bool  g_show_add_instance  = false;
static char  g_add_name[128]      = "";
static int   g_add_asset_type     = 0;
static char  g_add_custom_assets[512] = "";
static bool  g_add_use_sre        = true;
static char  g_add_game_type[64]  = "Swordigo";
static std::string g_add_status;
static bool  g_add_copying        = false;
static float g_add_copy_progress  = 0.0f;
static char  g_add_apk_path[1024] = "";
static bool  g_add_apk_dialog_pending = false;

struct ApkImportResult {
    bool success = false;
    std::string error;
};
static std::future<ApkImportResult> g_apk_import_future;

static void SDLCALL apk_dialog_callback(void*, const char* const* files, int) {
    g_add_apk_dialog_pending = false;
    if (!files || !files[0]) return;
    std::snprintf(g_add_apk_path, sizeof(g_add_apk_path), "%s", files[0]);
    if (g_add_name[0] == '\0') {
        std::string stem = fs::path(files[0]).stem().string();
        std::snprintf(g_add_name, sizeof(g_add_name), "%s", stem.c_str());
    }
}

static float g_anim_time = 0.0f;

// Profile state (future)
static char  g_profile_username[64]  = "Player";
static bool  g_profile_editing       = false;

// Mod browser — real Raijin-format store catalog state
static char  g_modbrowser_search[128] = "";
static int   g_modbrowser_cat         = 0;          // 0=All, then catalog categories
// Live Raijin store catalog — https, camelCase keys, top-level JSON array.
static char  g_catalog_url[256] =
    "https://raw.githubusercontent.com/raijinswordigo/requests/refs/heads/main/store.json";
static bool  g_catalog_loaded         = false;
static bool  g_catalog_loading        = false;
static std::string g_catalog_error;
static std::string g_catalog_cache_path;   // store_cache.json (disk fallback)
static std::vector<modman::StoreMod> g_catalog_mods;
static std::vector<std::string>      g_catalog_categories;
static std::vector<bool>             g_catalog_installed;   // per-entry installed mask
static std::vector<int>              g_catalog_state;       // 0=Get,1=Downloading,2=Installed,3=Failed
static std::vector<float>            g_catalog_progress;
static std::vector<std::string>      g_catalog_fail_reason;
static std::vector<std::future<std::string>> g_catalog_jobs; // install result/error strings

// Remote icon cache: <user_data>/mod_cache/icons/<id>.png + lazily-loaded GL tex.
static std::vector<std::string>      g_catalog_icon_paths;
static std::vector<GLuint>           g_catalog_icon_tex;
static std::vector<std::future<void>> g_catalog_icon_jobs;
// GL textures retired from a worker thread must be freed on the UI thread only.
static std::vector<GLuint>           g_icon_tex_pending_free;

// Local .zip install dialog (Raijin-format mod zips)
static bool        g_mod_zip_dialog_pending = false;
static std::string g_mod_zip_status;
static bool        g_mod_zip_status_ok = false;
static void SDLCALL mod_zip_dialog_callback(void*, const char* const* files, int) {
    g_mod_zip_dialog_pending = false;
    if (!files || !files[0]) return;
    std::string err;
    modman::ModMeta meta;
    if (modman::install_mod_zip(files[0], get_user_data_dir() + "/mods", &meta, &err)) {
        g_mod_zip_status = "Installed \"" + meta.name + "\" v" + meta.version + " — " + meta.id;
        g_mod_zip_status_ok = true;
    } else {
        g_mod_zip_status = "Install failed: " + err;
        g_mod_zip_status_ok = false;
    }
    g_mods_scanned = false;
}

static int   g_selected_skin = 0;

// =============================================================================
// Helpers
// =============================================================================

static GLuint LoadTextureFromFile(const char* path, int* out_w, int* out_h) {
    SDL_Surface* surf = IMG_Load(path);
    if (!surf) return 0;
    SDL_Surface* rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surf);
    if (!rgba) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    if (out_w) *out_w = rgba->w;
    if (out_h) *out_h = rgba->h;
    SDL_DestroySurface(rgba);
    return tex;
}

// Decode an embedded (or in-memory) PNG/JPEG into a GL texture.
// This is the permanent fallback: assets baked into the binary always work,
// even when deb/rpm packaging drops the launcher/ asset directory.
static GLuint LoadTextureFromMemory(const unsigned char* data, size_t size, int* out_w, int* out_h) {
    if (!data || size < 8) return 0;
    int w = 0, h = 0;
    unsigned char* px = nullptr;
    if (!asset_decode_image(data, size, &px, &w, &h)) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    asset_image_free(px);
    return tex;
}

// Decode an embedded image into an SDL surface (for window icons).
static SDL_Surface* LoadSurfaceFromMemory(const unsigned char* data, size_t size) {
    if (!data || size < 8) return nullptr;
    int w = 0, h = 0;
    unsigned char* px = nullptr;
    if (!asset_decode_image(data, size, &px, &w, &h)) return nullptr;
    SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px, w * 4);
    if (!surf) { asset_image_free(px); return nullptr; }
    // Copy pixels into a surface that owns its own buffer, then free the stb buffer.
    SDL_Surface* copy = SDL_DuplicateSurface(surf);
    SDL_DestroySurface(surf);
    asset_image_free(px);
    return copy;
}

// Try an embedded asset by any of its known names; returns first hit.
static GLuint LoadTextureEmbedded(const char* name, int* out_w = nullptr, int* out_h = nullptr) {
    const unsigned char* data = nullptr;
    size_t size = 0;
    if (embedded_asset(name, &data, &size))
        return LoadTextureFromMemory(data, size, out_w, out_h);
    return 0;
}

static std::string ReadFileToString(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static void FreeModIcon(ModInfo& mod) {
    if (mod.icon_tex) { glDeleteTextures(1, &mod.icon_tex); mod.icon_tex = 0; }
}

static GLuint LoadModIcon(const ModInfo& mod) {
    if (mod.icon_tex) return mod.icon_tex;
    if (mod.icon_path.empty()) return 0;
    return LoadTextureFromFile(mod.icon_path.c_str(), nullptr, nullptr);
}

static void ScanMods() {
    // Free icon textures before dropping the list
    for (auto& m : g_mods) FreeModIcon(m);
    g_mods.clear();

    std::vector<modman::ModMeta> metas = modman::list_mods(get_user_data_dir() + "/mods");
    g_mods.reserve(metas.size());
    for (auto& meta : metas) {
        ModInfo mod;
        mod.id          = meta.id;
        mod.name        = meta.name;
        mod.version     = meta.version;
        mod.author      = meta.author;
        mod.description = meta.description;
        mod.type        = meta.is_toml ? (meta.category.empty() ? "Raijin" : meta.category) : meta.type;
        mod.category    = meta.category;
        mod.dir_path    = meta.dir_path;
        mod.icon_path   = meta.icon_path;
        mod.enabled     = meta.enabled;
        mod.is_toml     = meta.is_toml;
        if (mod.name.empty()) mod.name = mod.id;
        g_mods.push_back(std::move(mod));
    }
    g_mods_scanned = true;
}

static std::string FormatFileSize(size_t bytes) {
    if (bytes < 1024)               return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)        return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0)); return buf;
    }
    char buf[32]; snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0)); return buf;
}

static GLuint GetIconForInstance(const BinaryInfo& b) {
    if (!b.icon_path.empty()) {
        auto it = g_custom_icon_cache.find(b.icon_path);
        if (it != g_custom_icon_cache.end()) {
            if (it->second) return it->second;
        } else {
            int w = 0, h = 0; GLuint tex = 0;
            std::string paths[] = {
                b.icon_path,
                get_user_data_dir() + "/launcher/icons/" + b.icon_path,
                get_user_data_dir() + "/launcher/" + b.icon_path,
                std::string("src/assets/icons/") + b.icon_path,
                std::string("src/assets/") + b.icon_path,
            };
            for (auto& p : paths) { tex = LoadTextureFromFile(p.c_str(), &w, &h); if (tex) break; }
            g_custom_icon_cache[b.icon_path] = tex;
            if (tex) return tex;
        }
    }
    if (b.game_type == "RLSwordigo"   && g_tex_icon_rlswordigo) return g_tex_icon_rlswordigo;
    if (b.game_type == "SwordigoMini" && g_tex_icon_swmini)     return g_tex_icon_swmini;
    if (g_tex_icon_swordigo) return g_tex_icon_swordigo;
    return g_tex_icon_app;
}

static std::string GetDisplayName(const BinaryInfo& b) {
    if (b.game_type == "Swordigo")     return "Swordigo " + b.version;
    if (b.game_type == "RLSwordigo")   return "RLSwordigo " + b.version;
    if (b.game_type == "SwordigoMini") return "Swordigo Mini " + b.version;
    std::string name = b.label;
    auto strip = [&name](const std::string& tag) {
        size_t pos;
        while ((pos = name.find(tag)) != std::string::npos) name.erase(pos, tag.size());
    };
    strip("[ARM64]"); strip("[ARM32]"); strip("(Tested)"); strip("(Testing)");
    strip("(Unknown)"); strip("(SRE)"); strip("[Custom]"); strip("[RL]"); strip("[Swordigo]");
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back()  == ' ') name.pop_back();
    std::string clean; bool prev_space = false;
    for (char c : name) {
        if (c == ' ') { if (!prev_space) clean += c; prev_space = true; }
        else { clean += c; prev_space = false; }
    }
    return clean.empty() ? b.label : clean;
}

static std::string GetSubtitle(const BinaryInfo& b) {
    std::string sub = BinarySelector::arch_string(b.arch);
    sub += "  ·  v" + b.version;
    switch (b.status) {
        case BinaryStatus::TESTED:  sub += "  ·  Stable";  break;
        case BinaryStatus::TESTING: sub += "  ·  Testing"; break;
        default: break;
    }
    if (b.assets_dir == "rl_assets") sub += "  ·  RL";
    else if (b.assets_dir != "assets") sub += "  ·  Custom";
    return sub;
}

// =============================================================================
// Premium Theme — deep space dark with crimson accent
// =============================================================================

static void ApplyPremiumTheme() {
    // Base on the shared design system (consistent rounding/spacing/palette
    // across launcher, loading screen, and overlays), then layer the
    // launcher's signature look on top below.
    sf_theme::ApplyTheme();
    ImGuiStyle& style = ImGui::GetStyle();

    // Rounding
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 12.0f;
    style.FrameRounding     = 8.0f;
    style.GrabRounding      = 8.0f;
    style.PopupRounding     = 14.0f;
    style.ScrollbarRounding = 14.0f;
    style.TabRounding       = 8.0f;

    // Spacing — breathable
    style.FramePadding     = ImVec2(14, 8);
    style.ItemSpacing      = ImVec2(12, 8);
    style.ItemInnerSpacing = ImVec2(10, 6);
    style.ScrollbarSize    = 10.0f;
    style.GrabMinSize      = 10.0f;
    style.IndentSpacing    = 22.0f;
    style.WindowPadding    = ImVec2(18, 16);

    // Borders
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 1.0f;
    style.FrameBorderSize  = 0.0f;
    style.PopupBorderSize  = 1.0f;
    style.TabBorderSize    = 0.0f;

    style.AntiAliasedLines = true;
    style.AntiAliasedFill  = true;

    ImVec4* c = style.Colors;

    // Deep space backgrounds
    c[ImGuiCol_WindowBg]          = ImVec4(0.040f, 0.051f, 0.071f, 1.00f); // #0A0D12
    c[ImGuiCol_ChildBg]           = ImVec4(0.063f, 0.082f, 0.114f, 1.00f); // #10152D/similar
    c[ImGuiCol_PopupBg]           = ImVec4(0.071f, 0.090f, 0.122f, 0.98f);
    c[ImGuiCol_Border]            = ImVec4(0.133f, 0.165f, 0.220f, 0.60f);
    c[ImGuiCol_BorderShadow]      = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);

    // Frames
    c[ImGuiCol_FrameBg]           = ImVec4(0.082f, 0.106f, 0.157f, 1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.110f, 0.141f, 0.204f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.145f, 0.180f, 0.255f, 1.00f);

    // Title
    c[ImGuiCol_TitleBg]           = ImVec4(0.040f, 0.051f, 0.071f, 1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.055f, 0.071f, 0.098f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.040f, 0.051f, 0.071f, 0.50f);
    c[ImGuiCol_MenuBarBg]         = ImVec4(0.063f, 0.082f, 0.114f, 1.00f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.040f, 0.051f, 0.071f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.133f, 0.165f, 0.220f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.200f, 0.240f, 0.310f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.280f, 0.330f, 0.420f, 1.00f);

    // Accent — crimson #E94560
    c[ImGuiCol_CheckMark]         = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);
    c[ImGuiCol_SliderGrab]        = ImVec4(0.345f, 0.651f, 1.000f, 0.80f);
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.345f, 0.651f, 1.000f, 1.00f);

    // Buttons — crimson
    c[ImGuiCol_Button]            = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(1.000f, 0.380f, 0.490f, 1.00f);
    c[ImGuiCol_ButtonActive]      = ImVec4(0.760f, 0.196f, 0.278f, 1.00f);

    // Headers
    c[ImGuiCol_Header]            = ImVec4(0.082f, 0.106f, 0.157f, 1.00f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.914f, 0.271f, 0.376f, 0.25f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.914f, 0.271f, 0.376f, 0.45f);

    // Separators
    c[ImGuiCol_Separator]         = ImVec4(0.133f, 0.165f, 0.220f, 0.50f);
    c[ImGuiCol_SeparatorHovered]  = ImVec4(0.914f, 0.271f, 0.376f, 0.50f);
    c[ImGuiCol_SeparatorActive]   = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);

    // Resize
    c[ImGuiCol_ResizeGrip]        = ImVec4(0.914f, 0.271f, 0.376f, 0.12f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.914f, 0.271f, 0.376f, 0.35f);
    c[ImGuiCol_ResizeGripActive]  = ImVec4(0.914f, 0.271f, 0.376f, 0.80f);

    // Tabs
    c[ImGuiCol_Tab]               = ImVec4(0.082f, 0.106f, 0.157f, 1.00f);
    c[ImGuiCol_TabHovered]        = ImVec4(0.914f, 0.271f, 0.376f, 0.45f);
    c[ImGuiCol_TabSelected]       = ImVec4(0.914f, 0.271f, 0.376f, 0.75f);

    // Text
    c[ImGuiCol_Text]         = ImVec4(0.918f, 0.941f, 0.969f, 1.00f); // #EAF0F7
    c[ImGuiCol_TextDisabled] = ImVec4(0.447f, 0.494f, 0.557f, 1.00f); // #72808E

    // Tables
    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.082f, 0.106f, 0.157f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.133f, 0.165f, 0.220f, 0.60f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.133f, 0.165f, 0.220f, 0.30f);
    c[ImGuiCol_TableRowBg]        = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(1.000f, 1.000f, 1.000f, 0.025f);
}

// =============================================================================
// Helper: secondary button style push/pop
// =============================================================================

static void PushSecondaryBtn() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.082f, 0.106f, 0.157f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.133f, 0.165f, 0.235f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f, 0.141f, 0.200f, 1.0f));
}
static void PopSecondaryBtn() { ImGui::PopStyleColor(3); }

// Inline section label
static void SectionLabel(const char* icon, const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.914f, 0.271f, 0.376f, 0.85f));
    ImGui::Text("%s  %s", icon, label);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, ImVec2(p.x + avail, p.y), IM_COL32(233, 69, 96, 55), 1.0f);
    ImGui::Dummy(ImVec2(0, 6));
}

// =============================================================================
// Window controls overlay (top-right, always on top)
// =============================================================================

static void DrawWindowControls(bool& running, LaunchConfig& cfg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float x = vp->WorkPos.x + vp->WorkSize.x - WIN_CTRL_W;
    float y = vp->WorkPos.y;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(WIN_CTRL_W, 42));
    ImGui::Begin("##WinCtrl", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 0));

    // Minimize
    PushSecondaryBtn();
    ImGui::SetCursorPosY(7);
    if (ImGui::Button(ICON_FA_WINDOW_MINIMIZE "##min", ImVec2(34, 28))) {
        if (g_sdl_window) SDL_MinimizeWindow(g_sdl_window);
    }
    PopSecondaryBtn();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimize");

    // Close
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.12f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.16f, 0.16f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.10f, 0.10f, 1.00f));
    if (ImGui::Button(ICON_FA_XMARK "##close", ImVec2(34, 28))) {
        cfg.should_launch = false;
        running = false;
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close  (ESC)");

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// =============================================================================
// Left Sidebar — logo + nav + profile
// =============================================================================

static void DrawSidebar(BinarySelector& selector, int& selected,
                        bool& running, LaunchConfig& cfg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(SIDEBAR_W, vp->WorkSize.y));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,   ImVec4(0.031f, 0.039f, 0.055f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,    ImVec4(0.031f, 0.039f, 0.055f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##Sidebar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();

    // ── Logo / branding ──
    {
        float logo_area_h = 80.0f;
        float max_w = SIDEBAR_W - 20.0f;
        if (g_tex_logo) {
            // Scale to fill sidebar width, cap height
            float lw = max_w;
            float lh = lw * ((float)g_tex_logo_h / (float)g_tex_logo_w);
            if (lh > 60.0f) { lh = 60.0f; lw = lh * ((float)g_tex_logo_w / (float)g_tex_logo_h); }
            ImGui::SetCursorPos(ImVec2((SIDEBAR_W - lw) * 0.5f, (logo_area_h - lh) * 0.5f));
            ImGui::Image((ImTextureID)(intptr_t)g_tex_logo, ImVec2(lw, lh));
        } else {
            ImGui::SetCursorPos(ImVec2(16, (logo_area_h - ImGui::GetTextLineHeight()) * 0.5f));
            if (g_font_heading) ImGui::PushFont(g_font_heading);
            ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "SWORDIGO");
            if (g_font_heading) ImGui::PopFont();
        }
        // Bottom separator of logo area
        dl->AddLine(ImVec2(wp.x + 16, wp.y + logo_area_h),
                    ImVec2(wp.x + SIDEBAR_W - 16, wp.y + logo_area_h),
                    IM_COL32(255, 255, 255, 18), 1.0f);
        ImGui::SetCursorPosY(logo_area_h + 8);
    }

    // ── Nav items ──
    struct NavEntry { LauncherPage page; const char* icon; const char* label; const char* tooltip; };
    static const NavEntry nav[] = {
        { LauncherPage::HOME,        ICON_FA_HOUSE,        "Home",        "Quick launch & overview"     },
        { LauncherPage::LIBRARY,     ICON_FA_LAYER_GROUP,  "Library",     "Manage game instances"       },
        { LauncherPage::MODS,        ICON_FA_PUZZLE_PIECE, "Mods",        "Installed mods"              },
        { LauncherPage::MOD_BROWSER, ICON_FA_GLOBE,        "Mod Browser", "Browse & install mods"       },
        { LauncherPage::SDK_TOOLS,   ICON_FA_WRENCH,       "SDK / Tools", "Asset viewer & dev tools"    },
    };

    ImGui::SetCursorPosX(0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    for (auto& e : nav) {
        bool active = (g_page == e.page);
        ImVec2 item_pos = ImGui::GetCursorScreenPos();

        // Active bg highlight
        if (active) {
            dl->AddRectFilled(item_pos, ImVec2(item_pos.x + SIDEBAR_W, item_pos.y + 46),
                              IM_COL32(233, 69, 96, 28));
            // Left accent bar
            dl->AddRectFilled(item_pos, ImVec2(item_pos.x + 3, item_pos.y + 46),
                              IM_COL32(233, 69, 96, 255), 0.0f);
        }

        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));

        ImGui::SetCursorPosX(0);
        ImGui::PushID((int)e.page);
        if (ImGui::Selectable("##nav", active, 0, ImVec2(SIDEBAR_W, 46))) {
            g_page = e.page;
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.tooltip);
        ImGui::PopStyleColor(3);

        // Icon + label drawn over selectable
        float ty = item_pos.y + (46 - ImGui::GetTextLineHeight()) * 0.5f;
        ImU32 text_col = active ? IM_COL32(233, 69, 96, 255) : IM_COL32(180, 195, 215, 200);
        dl->AddText(ImVec2(item_pos.x + 22, ty), text_col, e.icon);
        dl->AddText(ImVec2(item_pos.x + 52, ty), text_col, e.label);

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
    }
    ImGui::PopStyleVar(2);

    // ── Settings separator + nav ──
    {
        ImGui::Dummy(ImVec2(0, 8));
        ImVec2 sp = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(wp.x + 16, sp.y), ImVec2(wp.x + SIDEBAR_W - 16, sp.y),
                    IM_COL32(255, 255, 255, 15), 1.0f);
        ImGui::Dummy(ImVec2(0, 8));
    }

    // Settings nav
    {
        bool active = (g_page == LauncherPage::SETTINGS);
        ImVec2 item_pos = ImGui::GetCursorScreenPos();
        if (active) {
            dl->AddRectFilled(item_pos, ImVec2(item_pos.x + SIDEBAR_W, item_pos.y + 46),
                              IM_COL32(233, 69, 96, 28));
            dl->AddRectFilled(item_pos, ImVec2(item_pos.x + 3, item_pos.y + 46),
                              IM_COL32(233, 69, 96, 255), 0.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
        ImGui::SetCursorPosX(0);
        if (ImGui::Selectable("##navSettings", active, 0, ImVec2(SIDEBAR_W, 46)))
            g_page = LauncherPage::SETTINGS;
        ImGui::PopStyleColor(3);
        float ty = item_pos.y + (46 - ImGui::GetTextLineHeight()) * 0.5f;
        ImU32 tc = active ? IM_COL32(233, 69, 96, 255) : IM_COL32(180, 195, 215, 200);
        dl->AddText(ImVec2(item_pos.x + 22, ty), tc, ICON_FA_GEAR);
        dl->AddText(ImVec2(item_pos.x + 52, ty), tc, "Settings");
    }

    // ── Profile card at bottom ──
    {
        float card_h  = 74.0f;
        float card_y  = ImGui::GetWindowHeight() - card_h - STATUS_H;
        ImVec2 card_pos(wp.x, wp.y + card_y);
        
        bool active = (g_page == LauncherPage::PROFILE);
        // Card bg - highlighted when active
        ImU32 card_bg_color = active
            ? IM_COL32(233, 69, 96, 28)
            : IM_COL32(18, 24, 38, 255);
            
        dl->AddRectFilled(card_pos, ImVec2(card_pos.x + SIDEBAR_W, card_pos.y + card_h),
                          card_bg_color);
        dl->AddLine(ImVec2(card_pos.x, card_pos.y),
                    ImVec2(card_pos.x + SIDEBAR_W, card_pos.y),
                    IM_COL32(255, 255, 255, 18), 1.0f);

        // Left accent bar if active
        if (active) {
            dl->AddRectFilled(card_pos, ImVec2(card_pos.x + 3, card_pos.y + card_h),
                              IM_COL32(233, 69, 96, 255), 0.0f);
        }

        // Avatar circle
        ImVec2 av_center(card_pos.x + 32, card_pos.y + card_h * 0.5f);
        dl->AddCircleFilled(av_center, 20.0f, IM_COL32(35, 46, 68, 255));
        dl->AddCircle(av_center, 20.0f, IM_COL32(233, 69, 96, 140), 32, 1.5f);
        // Person icon inside circle
        dl->AddText(ImVec2(av_center.x - 6, av_center.y - 7), IM_COL32(233, 69, 96, 255), ICON_FA_USER);

        // Username
        float tx = card_pos.x + 60;
        float ty_top    = card_pos.y + 14;
        float ty_bottom = card_pos.y + 36;
        dl->AddText(ImVec2(tx, ty_top),    IM_COL32(230, 237, 243, 255), g_profile_username);
        dl->AddText(ImVec2(tx, ty_bottom), IM_COL32(130, 150, 175, 180), "Local Profile");

        // "Manage" link — bottom right of card
        dl->AddText(ImVec2(card_pos.x + SIDEBAR_W - 72, card_pos.y + card_h - 18),
                    IM_COL32(88, 166, 255, 180), ICON_FA_PEN "  Edit");

        // Click to go to profile page
        ImGui::SetCursorPos(ImVec2(0, card_y));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1, 1, 1, 0.10f));
        if (ImGui::Selectable("##profileCard", false, 0, ImVec2(SIDEBAR_W, card_h)))
            g_page = LauncherPage::PROFILE;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Manage Profile");
        ImGui::PopStyleColor(3);
    }

    // Right border
    dl->AddLine(ImVec2(wp.x + SIDEBAR_W - 1, wp.y),
                ImVec2(wp.x + SIDEBAR_W - 1, wp.y + ImGui::GetWindowHeight()),
                IM_COL32(255, 255, 255, 14), 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// =============================================================================
// Status Footer
// =============================================================================

static void DrawStatusFooter(int selected, const BinarySelector& selector) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - STATUS_H));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, STATUS_H));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.024f, 0.031f, 0.043f, 1.0f));
    ImGui::Begin("##StatusFooter", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::SetCursorPosX(SIDEBAR_W + 16);

    const auto& bins = selector.get_binaries();
    if (!bins.empty() && selected >= 0 && selected < (int)bins.size()) {
        ImVec4 col;
        const char* str;
        switch (bins[selected].status) {
            case BinaryStatus::TESTED:  str = ICON_FA_CIRCLE_CHECK "  Ready"; col = ImVec4(0.24f, 0.72f, 0.31f, 0.9f); break;
            case BinaryStatus::TESTING: str = ICON_FA_CLOCK "  Testing"; col = ImVec4(0.82f, 0.60f, 0.13f, 0.9f); break;
            default:                    str = ICON_FA_CIRCLE_EXCLAMATION "  Unknown"; col = ImVec4(0.55f, 0.58f, 0.62f, 0.9f); break;
        }
        ImGui::TextColored(col, "%s", str);
    } else {
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK "  Ready");
    }

    // Right side — hints
    ImGui::SameLine(ImGui::GetWindowWidth() - 320);
    ImGui::TextDisabled("  v8.0 Remaster  |  Enter: Launch  |  ESC: Close");

    ImGui::End();
    ImGui::PopStyleColor();
}

// =============================================================================
// Content area dispatcher
// =============================================================================

static void DrawContentArea(BinarySelector& selector, int& selected,
                            LaunchConfig& cfg, bool& running,
                            int& api_sel, int& engine_sel,
                            bool& use_sre_sel, bool& adv_opts_sel) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + SIDEBAR_W, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - SIDEBAR_W, vp->WorkSize.y - STATUS_H));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.040f, 0.051f, 0.071f, 1.0f));
    ImGui::Begin("##Content", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Save editor hijacks the content area when open
    if (g_show_save_ed) {
        DrawSaveEditor(g_show_save_ed);
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    switch (g_page) {
        case LauncherPage::HOME:
            DrawHomePage(selector, selected, cfg, running, api_sel, engine_sel, use_sre_sel, adv_opts_sel);
            break;
        case LauncherPage::LIBRARY:
            DrawLibraryPage(selector, selected, cfg, running, api_sel, engine_sel, use_sre_sel, adv_opts_sel);
            break;
        case LauncherPage::MODS:
            DrawModsPage();
            break;
        case LauncherPage::MOD_BROWSER:
            DrawModBrowserPage();
            break;
        case LauncherPage::PROFILE:
            DrawProfilePage();
            break;
        case LauncherPage::SETTINGS:
            DrawSettingsPage();
            break;
        case LauncherPage::SDK_TOOLS:
            DrawSDKToolsPage();
            break;
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// =============================================================================
// News-style scrolling ticker — "Also Try Raijins KIWI LAWNCHER!!!"
// Classic news-channel marquee: text glides in from the right edge and exits
// left, seamless loop, with a pulsing LIVE-style KIWI badge on the left.
// =============================================================================

struct TickerSeg { const char* text; ImU32 col; };

static float DrawTickerMessage(ImDrawList* dl, float x, float y,
                               const TickerSeg* segs, int n) {
    float cx = x;
    for (int i = 0; i < n; ++i) {
        ImVec2 sz = ImGui::CalcTextSize(segs[i].text);
        dl->AddText(ImVec2(cx, y), segs[i].col, segs[i].text);
        cx += sz.x;
    }
    return cx - x;  // total message width
}

static void DrawNewsTicker() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = 42.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    // ── Background plate + rim ──
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(8, 12, 20, 255), 9.0f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), IM_COL32(233, 69, 96, 42), 9.0f, 0, 1.2f);
    // Subtle top sheen so it reads as a glossy news strip
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + width, p.y + h * 0.5f),
                                IM_COL32(255, 255, 255, 10), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 10));

    // ── Left KIWI badge (fixed) with pulsing dot ──
    const float badge_w = 168.0f;
    dl->AddRectFilled(p, ImVec2(p.x + badge_w, p.y + h), IM_COL32(233, 69, 96, 26), 9.0f,
                      ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(p.x + badge_w, p.y + 6), ImVec2(p.x + badge_w, p.y + h - 6),
                IM_COL32(233, 69, 96, 70), 1.2f);

    float pulse = 0.5f + 0.5f * sinf(g_anim_time * 4.0f);   // soft heartbeat
    ImVec2 dot(p.x + 22, p.y + h * 0.5f);
    dl->AddCircleFilled(dot, 5.0f, IM_COL32(233, 69, 96, 255));
    dl->AddCircle(dot, 8.0f + pulse * 3.0f, IM_COL32(233, 69, 96, (int)(55 + 130 * pulse)), 24, 1.6f);

    dl->AddText(ImVec2(p.x + 38, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                IM_COL32(124, 221, 111, 255), "KIWI");
    ImVec2 kw = ImGui::CalcTextSize("KIWI");
    dl->AddText(ImVec2(p.x + 38 + kw.x + 8, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                IM_COL32(230, 237, 243, 235), "LAWNCHER");

    // ── Scrolling message (right → left, seamless) ──
    static const TickerSeg msg[] = {
        { "  v9.0 Platform Remaster is live ",   IM_COL32(230, 237, 243, 255) },
        { ICON_FA_BOLT " ",                       IM_COL32(88, 166, 255, 255)  },
        { "Hiro run animation fixed   ",          IM_COL32(196, 206, 221, 255) },
        { ICON_FA_STAR " ",                       IM_COL32(233, 69, 96, 180)   },
        { "6 mods available in the Mod Browser ",IM_COL32(124, 221, 111, 255) },
        { ICON_FA_PUZZLE_PIECE " ",               IM_COL32(233, 69, 96, 255)   },
        { "ARM64 + Dynarmic JIT supported   ",    IM_COL32(214, 222, 233, 255) },
        { ICON_FA_MICROCHIP " ",                  IM_COL32(88, 166, 255, 255)  },
        { "Save Editor • Scene Inspector • Ruby Viewer   ",
                                                  IM_COL32(196, 206, 221, 255) },
        { ICON_FA_CODE " ",                       IM_COL32(124, 221, 111, 255) },
        { "Join the Swordigo Desktop community!   ",IM_COL32(230, 237, 243, 255)},
        { ICON_FA_GEM " ",                         IM_COL32(233, 69, 96, 255)   },
    };
    const int n = (int)(sizeof(msg) / sizeof(msg[0]));
    float total_w = 0.0f;
    for (int i = 0; i < n; ++i) total_w += ImGui::CalcTextSize(msg[i].text).x;

    const float speed = 74.0f;                      // px/sec
    float offset = fmodf(g_anim_time * speed, total_w);
    const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;

    const ImVec2 clip_min(p.x + badge_w + 4, p.y);
    const ImVec2 clip_max(p.x + width - 4, p.y + h);
    const float region_w = clip_max.x - clip_min.x;
    dl->PushClipRect(clip_min, clip_max, true);
    // Tile enough copies to cover the scroll region on any window width
    // (2 base copies + one per additional total_w of region width).
    const int copies = 2 + (int)(region_w / total_w);
    const float x = p.x + badge_w + 12.0f;
    for (int i = 0; i < copies; ++i)
        DrawTickerMessage(dl, x - offset + (float)i * total_w, ty, msg, n);
    dl->PopClipRect();

    ImGui::Dummy(ImVec2(width, h + 8));
}

// =============================================================================
// Home page — hero card + quick launch
// =============================================================================

static void DrawHomePage(BinarySelector& selector, int& selected,
                         LaunchConfig& cfg, bool& running,
                         int& api_sel, int& engine_sel,
                         bool& use_sre_sel, bool& adv_opts_sel) {
    const auto& bins = selector.get_binaries();

    // ── Animated hero banner ──
    {
        float banner_h = 120.0f;
        ImVec2 banner_pos = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Animated gradient
        float t = g_anim_time * 0.4f;
        float r = 0.5f + 0.5f * sinf(t);
        ImU32 col_tl = IM_COL32(14, 20, 35, 255);
        ImU32 col_tr = IM_COL32((int)(80 + 40 * r), (int)(25 + 10 * r), (int)(55 + 20 * r), 255);
        ImU32 col_bl = IM_COL32(10, 14, 25, 255);
        ImU32 col_br = IM_COL32(14, 20, 35, 255);
        dl->AddRectFilledMultiColor(banner_pos,
                                    ImVec2(banner_pos.x + width, banner_pos.y + banner_h),
                                    col_tl, col_tr, col_br, col_bl);

        // Background logo texture faint watermark
        if (g_tex_bg) {
            dl->AddImage((ImTextureID)(intptr_t)g_tex_bg,
                         banner_pos, ImVec2(banner_pos.x + width, banner_pos.y + banner_h),
                         ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 22));
        }

        // Headline
        float text_y = banner_pos.y + 20;
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        dl->AddText(ImVec2(banner_pos.x + 28, text_y),
                    IM_COL32(230, 237, 243, 255), "Welcome back,");
        float name_x = banner_pos.x + 28;
        ImVec2 name_sz = ImGui::CalcTextSize(g_profile_username);
        (void)name_sz;
        dl->AddText(ImVec2(name_x, text_y + (g_font_heading ? ImGui::GetFontSize() : 20) + 6),
                    IM_COL32(233, 69, 96, 255), g_profile_username);
        if (g_font_heading) ImGui::PopFont();

        // Subtext
        dl->AddText(ImVec2(banner_pos.x + 28, text_y + 60),
                    IM_COL32(139, 148, 158, 220),
                    "Ready to play Swordigo Desktop");

        // Banner bottom gradient fade
        dl->AddRectFilledMultiColor(
            ImVec2(banner_pos.x, banner_pos.y + banner_h - 30),
            ImVec2(banner_pos.x + width, banner_pos.y + banner_h),
            IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
            IM_COL32(10, 14, 25, 220), IM_COL32(10, 14, 25, 220));

        ImGui::Dummy(ImVec2(0, banner_h));
    }

    ImGui::Spacing();
    DrawNewsTicker();
    ImGui::Spacing();

    // ── Quick stats row ──
    {
        float cw = (ImGui::GetContentRegionAvail().x - 36) / 3.0f;
        const std::string instance_count = std::to_string(bins.size());
        const std::string mod_count = std::to_string(g_mods.size());
        struct StatCard { const char* icon; std::string val; const char* lbl; ImVec4 col; };
        StatCard cards[] = {
            { ICON_FA_GAMEPAD,  instance_count, "Instances",    ImVec4(0.35f, 0.65f, 1.0f, 1.0f) },
            { ICON_FA_PUZZLE_PIECE, mod_count, "Mods Active", ImVec4(0.24f, 0.72f, 0.31f, 1.0f) },
            { ICON_FA_CLOCK,    "—",                                "Hours Played", ImVec4(0.82f, 0.60f, 0.13f, 1.0f) },
        };
        for (int i = 0; i < 3; i++) {
            if (i > 0) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.082f, 0.120f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
            ImGui::BeginChild(("##stat" + std::to_string(i)).c_str(), ImVec2(cw, 58), ImGuiChildFlags_Borders);
            ImGui::SetCursorPos(ImVec2(14, 8));
            ImGui::TextColored(cards[i].col, "%s", cards[i].icon);
            ImGui::SameLine();
            if (g_font_heading) ImGui::PushFont(g_font_heading);
            ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "%s", cards[i].val.c_str());
            if (g_font_heading) ImGui::PopFont();
            ImGui::SetCursorPosX(14);
            ImGui::TextDisabled("%s", cards[i].lbl);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    if (bins.empty()) {
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 240) * 0.5f);
        ImGui::TextDisabled("No instances found. Go to Library to add one.");
        return;
    }

    // Clamp selected
    if (selected < 0 || selected >= (int)bins.size()) selected = 0;
    const BinaryInfo& b = bins[selected];

    // ── Featured instance card ──
    SectionLabel(ICON_FA_ROCKET, "FEATURED INSTANCE");
    {
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.082f, 0.120f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
        ImGui::BeginChild("##featured", ImVec2(cw, 130), ImGuiChildFlags_Borders);

        // Icon
        GLuint icon = GetIconForInstance(b);
        ImGui::SetCursorPos(ImVec2(18, 18));
        if (icon) ImGui::Image((ImTextureID)(intptr_t)icon, ImVec2(60, 60));

        ImGui::SameLine();
        ImGui::BeginGroup();
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::Text("%s", GetDisplayName(b).c_str());
        if (g_font_heading) ImGui::PopFont();
        ImGui::TextDisabled("%s", GetSubtitle(b).c_str());

        // Arch badge
        ImVec4 bc = (b.arch == BinaryArch::ARM64)
            ? ImVec4(0.20f, 0.40f, 0.85f, 1.0f) : ImVec4(0.85f, 0.55f, 0.15f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, bc);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bc);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bc);
        ImGui::SmallButton(BinarySelector::arch_string(b.arch));
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (b.is_default) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.45f, 0.00f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.45f, 0.00f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.45f, 0.00f, 0.6f));
            ImGui::SmallButton(ICON_FA_STAR "  Default");
            ImGui::PopStyleColor(3);
        }
        ImGui::EndGroup();

        // LAUNCH button — right-aligned inside card
        float btn_w  = 140.0f;
        float btn_h  = 42.0f;
        float btn_x  = cw - btn_w - 18;
        float btn_y  = (130 - btn_h) * 0.5f;
        ImGui::SetCursorPos(ImVec2(btn_x, btn_y));
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.914f, 0.271f, 0.376f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.000f, 0.380f, 0.490f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.760f, 0.196f, 0.278f, 1.0f));
        if (ImGui::Button(ICON_FA_PLAY "  PLAY", ImVec2(btn_w, btn_h))) {
            cfg.graphics_api = (api_sel == 0) ? GraphicsAPI::OPENGL : GraphicsAPI::VULKAN;
            cfg.use_dynarmic = (engine_sel == 1);
            if (backend_selection_ok(cfg.use_dynarmic)) {
                cfg.use_sre = use_sre_sel;
                cfg.advanced_redstell_opts = adv_opts_sel;
                cfg.selected_binary = b.filepath;
                cfg.assets_dir = b.assets_dir;
                cfg.game_type = b.game_type;
                cfg.should_launch = true;
                running = false;
            }
        }
        ImGui::PopStyleColor(3);
        if (g_font_heading) ImGui::PopFont();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // ── Launch options row ──
    SectionLabel(ICON_FA_SLIDERS, "LAUNCH OPTIONS");
    {
        float item_w = 180.0f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_MICROCHIP "  CPU Engine");
        ImGui::PopStyleColor();
        ImGui::SameLine(item_w);
        ImGui::RadioButton("Unicorn (TCG)##h", &engine_sel, 0); ImGui::SameLine();
        ImGui::RadioButton("Dynarmic (JIT)##h", &engine_sel, 1);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_PAINT_BRUSH "  Graphics API");
        ImGui::PopStyleColor();
        ImGui::SameLine(item_w);
        ImGui::RadioButton("OpenGL##h", &api_sel, 0); ImGui::SameLine();
        ImGui::RadioButton("Vulkan (Experimental)##h",  &api_sel, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vulkan support is experimental and may crash on some GPUs.");

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_CODE "  SRE Hooks");
        ImGui::PopStyleColor();
        ImGui::SameLine(item_w);
        ImGui::Checkbox("Enable##h", &use_sre_sel);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_WRENCH "  Redstell Opts");
        ImGui::PopStyleColor();
        ImGui::SameLine(item_w);
        ImGui::Checkbox("Advanced Memory Fixes##h", &adv_opts_sel);
    }

    ImGui::Spacing();
    SectionLabel(ICON_FA_NEWSPAPER, "RECENT ACTIVITY");
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::BeginChild("##activity", ImVec2(-1, 56), ImGuiChildFlags_Borders);
        ImGui::SetCursorPos(ImVec2(16, 18));
        ImGui::TextDisabled(ICON_FA_CLOCK_ROTATE_LEFT "  No recent sessions. Played sessions will appear here.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

// =============================================================================
// Library page — full instance management
// =============================================================================

static void DrawLibraryPage(BinarySelector& selector, int& selected,
                            LaunchConfig& cfg, bool& running,
                            int& api_sel, int& engine_sel,
                            bool& use_sre_sel, bool& adv_opts_sel) {
    const auto& bins = selector.get_binaries();

    // ── Header row ──
    ImGui::Spacing();
    {
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::Text(ICON_FA_LAYER_GROUP "  Game Library");
        if (g_font_heading) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.24f, 0.72f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.85f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.58f, 0.25f, 1.0f));
        if (ImGui::Button(ICON_FA_PLUS "  Add Instance", ImVec2(150, 32))) {
            g_show_add_instance = true;
            memset(g_add_name, 0, sizeof(g_add_name));
            strncpy(g_add_name, "My Instance", sizeof(g_add_name) - 1);
            memset(g_add_custom_assets, 0, sizeof(g_add_custom_assets));
            g_add_asset_type = 0;
            g_add_use_sre = true;
            strncpy(g_add_game_type, "Swordigo", sizeof(g_add_game_type) - 1);
            g_add_status.clear();
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new game instance");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (bins.empty()) {
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX((cw - 300) * 0.5f);
        ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextDisabled(ICON_FA_BOX_OPEN "  No instances found.");
        ImGui::SetCursorPosX((cw - 300) * 0.5f + 20);
        ImGui::TextDisabled("Click '+ Add Instance' to create one.");
        return;
    }

    // Sort by version descending, ARM64 first
    std::vector<int> sorted_idx(bins.size());
    std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
    std::sort(sorted_idx.begin(), sorted_idx.end(), [&bins](int a, int b_idx) {
        if (bins[a].version != bins[b_idx].version) return bins[a].version > bins[b_idx].version;
        if (bins[a].arch != bins[b_idx].arch) return bins[a].arch == BinaryArch::ARM64;
        return false;
    });

    // Auto-select on first frame: prefer the persisted default binary, then
    // the newest ARM64, then the newest ARM32. Never hardcode a version.
    static bool first_frame = true;
    if (first_frame && !bins.empty()) {
        first_frame = false;
        const std::string default_filepath = selector.get_default();
        if (!default_filepath.empty()) {
            for (int i = 0; i < (int)bins.size(); i++) {
                if (bins[i].filepath == default_filepath) { selected = i; break; }
            }
        }
        if (selected < 0 || selected >= (int)bins.size()) {
            int best = -1;
            for (int i = 0; i < (int)bins.size(); i++) {
                if (bins[i].arch == BinaryArch::ARM64) {
                    if (best < 0 || bins[i].version > bins[best].version) best = i;
                }
            }
            if (best < 0) {
                for (int i = 0; i < (int)bins.size(); i++)
                    if (best < 0 || bins[i].version > bins[best].version) best = i;
            }
            if (best >= 0) selected = best;
        }
    }

    // Two-column layout: instance list | detail
    float list_w = 280.0f;
    float detail_w = ImGui::GetContentRegionAvail().x - list_w - 12;

    // ── Instance list ──
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.047f, 0.067f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("##InstList", ImVec2(list_w, -1), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
    ImGui::SetCursorPos(ImVec2(12, 10));
    ImGui::Text(ICON_FA_LIST "  INSTANCES  (%d)", (int)bins.size());
    ImGui::PopStyleColor();
    ImGui::Separator();

    for (int idx : sorted_idx) {
        const auto& b = bins[idx];
        ImGui::PushID(idx);
        bool is_sel = (selected == idx);

        ImVec2 item_pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Hover/select highlight
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.914f, 0.271f, 0.376f, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.914f, 0.271f, 0.376f, 0.20f));
        if (ImGui::Selectable("##inst", is_sel, 0, ImVec2(0, 62))) selected = idx;
        ImGui::PopStyleColor(2);

        // Context menu
        if (ImGui::BeginPopupContextItem("InstCtx")) {
            if (ImGui::MenuItem(ICON_FA_STAR "  Set Default")) selector.set_default(b.filepath);
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Folder")) {
                std::string full = b.filepath[0] == '/' ? b.filepath : (get_user_data_dir() + "/" + b.filepath);
                std::string dir = fs::path(full).parent_path().string();
                os_external::open_in_file_manager(dir);
            }
            ImGui::Separator();
            bool is_vanilla = (b.game_type == "Swordigo" || b.game_type == "RLSwordigo" || b.game_type == "SwordigoMini");
            if (!is_vanilla && ImGui::MenuItem(ICON_FA_TRASH "  Remove")) {
                g_confirm_delete = true; g_delete_target_idx = idx;
            }
            ImGui::EndPopup();
        }

        // Selected left bar
        if (is_sel) {
            dl->AddRectFilled(ImVec2(item_pos.x, item_pos.y + 4),
                              ImVec2(item_pos.x + 3, item_pos.y + 58),
                              IM_COL32(233, 69, 96, 255), 2.0f);
        }

        // Icon
        GLuint icon = GetIconForInstance(b);
        if (icon) {
            dl->AddImageRounded((ImTextureID)(intptr_t)icon,
                ImVec2(item_pos.x + 12, item_pos.y + 11),
                ImVec2(item_pos.x + 52, item_pos.y + 51),
                ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), 8.0f);
        }

        float tx = item_pos.x + 60;
        dl->AddText(ImVec2(tx, item_pos.y + 10),
                    IM_COL32(225, 232, 242, 255), GetDisplayName(b).c_str());
        dl->AddText(ImVec2(tx, item_pos.y + 30),
                    IM_COL32(139, 148, 158, 200), GetSubtitle(b).c_str());

        // Status dot
        ImVec4 dc;
        switch (b.status) {
            case BinaryStatus::TESTED:  dc = ImVec4(0.24f, 0.72f, 0.31f, 1.0f); break;
            case BinaryStatus::TESTING: dc = ImVec4(0.82f, 0.60f, 0.13f, 1.0f); break;
            default:                    dc = ImVec4(0.55f, 0.30f, 0.30f, 1.0f); break;
        }
        dl->AddCircleFilled(ImVec2(item_pos.x + list_w - 22, item_pos.y + 16),
                            4.5f, ImGui::ColorConvertFloat4ToU32(dc));

        if (b.is_default)
            dl->AddText(ImVec2(item_pos.x + list_w - 44, item_pos.y + 9),
                        IM_COL32(255, 215, 0, 255), ICON_FA_STAR);

        ImGui::PopID();
    }

    // Delete confirmation
    if (g_confirm_delete) { ImGui::OpenPopup("Confirm Delete"); g_confirm_delete = false; }
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  Remove this instance?");
        ImGui::Separator();
        if (g_delete_target_idx >= 0 && g_delete_target_idx < (int)bins.size())
            ImGui::Text("  %s", bins[g_delete_target_idx].label.c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button(ICON_FA_TRASH "  Remove", ImVec2(120, 0))) {
            selector.remove_instance(g_delete_target_idx);
            if (selected >= (int)selector.get_binaries().size())
                selected = std::max(0, (int)selector.get_binaries().size() - 1);
            g_delete_target_idx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        PushSecondaryBtn();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_delete_target_idx = -1; ImGui::CloseCurrentPopup(); }
        PopSecondaryBtn();
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ── Detail panel ──
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("##Detail", ImVec2(detail_w, -1), ImGuiChildFlags_Borders);

    if (selected < 0 || selected >= (int)bins.size()) {
        ImGui::Spacing(); ImGui::Spacing();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX((avail - ImGui::CalcTextSize("Select an instance").x) * 0.5f);
        ImGui::TextDisabled("Select an instance");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    const BinaryInfo& b = bins[selected];

    // Instance header in detail
    {
        GLuint di = GetIconForInstance(b);
        ImGui::SetCursorPos(ImVec2(16, 16));
        if (di) { ImGui::Image((ImTextureID)(intptr_t)di, ImVec2(64, 64)); ImGui::SameLine(); }
        ImGui::BeginGroup();
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::Text("%s", GetDisplayName(b).c_str());
        if (g_font_heading) ImGui::PopFont();
        ImGui::TextDisabled("%s", GetSubtitle(b).c_str());
        ImVec4 bc = (b.arch == BinaryArch::ARM64)
            ? ImVec4(0.20f, 0.40f, 0.85f, 1.0f) : ImVec4(0.85f, 0.55f, 0.15f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, bc);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bc);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bc);
        ImGui::SmallButton(BinarySelector::arch_string(b.arch));
        ImGui::PopStyleColor(3);
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // LAUNCH
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.914f, 0.271f, 0.376f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.000f, 0.380f, 0.490f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.760f, 0.196f, 0.278f, 1.0f));
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    float launch_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(ICON_FA_PLAY "  PLAY", ImVec2(launch_w, 52))) {
        cfg.graphics_api = (api_sel == 0) ? GraphicsAPI::OPENGL : GraphicsAPI::VULKAN;
        cfg.use_dynarmic  = (engine_sel == 1);
        if (backend_selection_ok(cfg.use_dynarmic)) {
            cfg.use_sre       = use_sre_sel;
            cfg.advanced_redstell_opts = adv_opts_sel;
            cfg.selected_binary = b.filepath;
            cfg.assets_dir      = b.assets_dir;
            cfg.game_type       = b.game_type;
            cfg.should_launch   = true;
            running = false;
        }
    }
    if (g_font_heading) ImGui::PopFont();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();

    // Launch options (compact inline rows)
    {
        float lw = 160.0f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_MICROCHIP "  CPU Engine"); ImGui::PopStyleColor();
        ImGui::SameLine(lw);
        ImGui::RadioButton("Unicorn##L", &engine_sel, 0); ImGui::SameLine();
        ImGui::RadioButton("Dynarmic##L", &engine_sel, 1);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_PAINT_BRUSH "  Graphics API"); ImGui::PopStyleColor();
        ImGui::SameLine(lw);
        ImGui::RadioButton("OpenGL##L", &api_sel, 0); ImGui::SameLine();
        ImGui::RadioButton("Vulkan (Experimental)##L",  &api_sel, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vulkan support is experimental and may crash on some GPUs.");

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_CODE "  SRE Hooks"); ImGui::PopStyleColor();
        ImGui::SameLine(lw);
        ImGui::Checkbox("Enable##L", &use_sre_sel);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::Text(ICON_FA_WRENCH "  Memory Fixes"); ImGui::PopStyleColor();
        ImGui::SameLine(lw);
        ImGui::Checkbox("Advanced Redstell##L", &adv_opts_sel);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Details table
    SectionLabel(ICON_FA_CIRCLE_INFO, "INSTANCE DETAILS");
    if (ImGui::BeginTable("DetTbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Prop",  ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        auto Row = [](const char* p, const char* v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", p);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
        };
        Row("Version",   b.version.c_str());
        Row("Arch",      BinarySelector::arch_string(b.arch));
        Row("Game Type", b.game_type.c_str());
        Row("Assets",    b.assets_dir.c_str());
        Row("File Size", FormatFileSize(b.file_size).c_str());
        Row("SHA256",    b.sha256.empty() ? "—" : b.sha256.substr(0, 16).c_str());
        const char* ss;
        switch (b.status) {
            case BinaryStatus::TESTED:  ss = "Tested (Stable)"; break;
            case BinaryStatus::TESTING: ss = "Testing"; break;
            default:                    ss = "Unknown"; break;
        }
        Row("Status", ss);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Action buttons row
    SectionLabel(ICON_FA_TOOLBOX, "ACTIONS");
    float abw = (ImGui::GetContentRegionAvail().x - 24) / 3.0f;
    PushSecondaryBtn();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save Editor", ImVec2(abw, 36))) {
        g_show_save_ed = true;
        g_save_loaded = false; g_save_sel = -1; g_save_status.clear();
        std::string save_dir = get_vfs_save_dir();
        g_save_paths = save_list_dir(save_dir);
        g_save_files.clear();
        for (auto& p : g_save_paths) { SaveFile sf; if (save_load(p, sf)) g_save_files.push_back(sf); }
        g_save_loaded = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit .gplayer save files");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(abw, 36))) {
        std::string full = b.filepath[0] == '/' ? b.filepath : (get_user_data_dir() + "/" + b.filepath);
        std::string dir  = fs::path(full).parent_path().string();
        os_external::open_in_file_manager(dir);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open instance folder");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_EYE "  Ruby Viewer", ImVec2(abw, 36))) {
        launch_ruby_viewer();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse game assets");
    PopSecondaryBtn();

    // Remove (non-vanilla only)
    {
        bool is_vanilla = (b.game_type == "Swordigo" || b.game_type == "RLSwordigo" || b.game_type == "SwordigoMini");
        if (!is_vanilla) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.40f, 0.08f, 0.08f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.13f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f, 0.09f, 0.09f, 1.0f));
            if (ImGui::Button(ICON_FA_TRASH "  Remove Instance", ImVec2(-1, 34))) {
                g_confirm_delete = true; g_delete_target_idx = selected;
            }
            ImGui::PopStyleColor(3);
        }
    }

    if (!b.dependencies.empty()) {
        ImGui::Spacing();
        SectionLabel(ICON_FA_PUZZLE_PIECE, "DEPENDENCIES");
        for (const auto& dep : b.dependencies) ImGui::BulletText("%s", dep.c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// =============================================================================
// Mods page
// =============================================================================

// --- Helper: find ModInfo by id ---
static int find_mod_by_id(const std::string& id) {
    for (int i = 0; i < (int)g_mods.size(); i++)
        if (g_mods[i].id == id) return i;
    return -1;
}

// --- Helper: render a single mod card in a panel ---
// Returns true if the card was clicked (for selection).
static bool draw_mod_card(ModInfo& mod, float panel_w, bool in_active, int global_idx, bool is_selected) {
    bool clicked = false;
    bool double_clicked = false;
    ImGui::PushID(global_idx + (in_active ? 10000 : 0));

    // Card background — selected gets VERY bright
    ImVec4 bg = is_selected
        ? (in_active ? ImVec4(0.12f, 0.22f, 0.35f, 1.0f) : ImVec4(0.11f, 0.16f, 0.30f, 1.0f))
        : (in_active ? ImVec4(0.05f, 0.07f, 0.11f, 1.0f) : ImVec4(0.04f, 0.05f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    float card_h = 56.0f;
    ImGui::BeginChild(("##mc" + std::to_string(global_idx)).c_str(),
                      ImVec2(panel_w - 8, card_h), ImGuiChildFlags_Borders);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();

    // Colors
    ImU32 accent    = in_active ? IM_COL32(50, 180, 70, 255) : IM_COL32(60, 120, 210, 255);
    ImU32 sel_accent = in_active ? IM_COL32(100, 240, 120, 255) : IM_COL32(120, 180, 255, 255);

    // Left accent bar (always)
    dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + 5, wp.y + ws.y),
                      is_selected ? sel_accent : accent, 3.0f);

    // Selection highlight — thick glowing border + bright fill
    if (is_selected) {
        // Outer glow
        dl->AddRect(ImVec2(wp.x - 1, wp.y - 1), ImVec2(wp.x + ws.x + 1, wp.y + ws.y + 1),
                    sel_accent, 10.0f, 0, 1.0f);
        // Main border
        dl->AddRect(ImVec2(wp.x + 0.5f, wp.y + 0.5f),
                    ImVec2(wp.x + ws.x - 0.5f, wp.y + ws.y - 0.5f),
                    sel_accent, 8.0f, 0, 3.0f);
        // Bright fill overlay — much more visible
        dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + ws.x, wp.y + ws.y),
                          in_active ? IM_COL32(50, 150, 70, 40) : IM_COL32(60, 120, 220, 40));
    }

    // Icon
    if (!mod.icon_tex) mod.icon_tex = LoadModIcon(mod);
    float icon_sz = 38.0f;
    float icon_x = wp.x + 10, icon_y = wp.y + (card_h - icon_sz) * 0.5f;
    if (mod.icon_tex)
        dl->AddImage((ImTextureID)(intptr_t)mod.icon_tex,
                     ImVec2(icon_x, icon_y), ImVec2(icon_x + icon_sz, icon_y + icon_sz));
    else {
        dl->AddRectFilled(ImVec2(icon_x, icon_y), ImVec2(icon_x + icon_sz, icon_y + icon_sz),
                          IM_COL32(25, 32, 48, 255), 6.0f);
        dl->AddText(ImVec2(icon_x + 10, icon_y + 10), IM_COL32(100, 120, 160, 200),
                    ICON_FA_PUZZLE_PIECE);
    }

    // Text
    float text_x = icon_x + icon_sz + 8;
    float text_y = wp.y + 7;
    dl->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(ImGuiCol_Text), mod.name.c_str());
    text_y += ImGui::GetTextLineHeightWithSpacing();
    char meta[128];
    snprintf(meta, sizeof(meta), "v%s  by %s",
             mod.version.c_str(), mod.author.empty() ? "Unknown" : mod.author.c_str());
    dl->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), meta);

    // Hover tooltip
    if (ImGui::IsWindowHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s  v%s", mod.name.c_str(), mod.version.c_str());
        if (!mod.author.empty()) ImGui::TextDisabled("by %s", mod.author.c_str());
        if (!mod.description.empty()) ImGui::TextWrapped("%s", mod.description.c_str());
        if (!mod.category.empty()) ImGui::TextDisabled("Category: %s", mod.category.c_str());
        ImGui::TextDisabled("ID: %s", mod.id.c_str());
        ImGui::TextDisabled(in_active ? "Status: Active" : "Status: Available");
        ImGui::EndTooltip();
    }

    // Click + double-click detection (manual bounds check)
    ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    bool mouse_over = (mouse_pos.x >= wp.x && mouse_pos.x <= wp.x + ws.x &&
                       mouse_pos.y >= wp.y && mouse_pos.y <= wp.y + ws.y);
    if (mouse_over) {
        if (ImGui::IsMouseClicked(0)) clicked = true;
        if (ImGui::IsMouseDoubleClicked(0)) double_clicked = true;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();

    // Return double-click as a special signal (caller checks via flag)
    if (double_clicked) clicked = true;
    return clicked;
}

// =============================================================================
// Mods page — Minecraft resource-pack-style dual-panel selector.
//
// Left panel  = "Available Mods"  (installed but not in load order)
// Right panel = "Active Mods"     (in load order, top = highest priority)
// Between     = >> << arrow buttons + Up/Down reorder arrows
// =============================================================================

static void DrawModsPage() {
    if (!g_mods_scanned) ScanMods();

    // Load persistent config
    static LauncherConfig g_cfg;
    static bool cfg_loaded = false;
    if (!cfg_loaded) {
        g_cfg = launcher_config_load();
        cfg_loaded = true;
    }
    // Sync on rescan
    static int last_scan_gen = 0;
    if (!g_mods_scanned) last_scan_gen++;

    // ── Header ──────────────────────────────────────────────────────────────
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_PUZZLE_PIECE "  Resource Packs");
    if (g_font_heading) ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 470);
    PushSecondaryBtn();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Mods Folder", ImVec2(150, 32))) {
        std::string mods_dir = get_user_data_dir() + "/mods";
        os_external::open_in_file_manager(mods_dir);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open mods directory in file manager");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Rescan", ImVec2(90, 32)))
        g_mods_scanned = false;
    PopSecondaryBtn();

    // Install zip
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.24f, 0.72f, 0.31f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.85f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.58f, 0.25f, 1.0f));
    if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS "  Install .zip", ImVec2(140, 32))) {
        static const SDL_DialogFileFilter filters[] = {{"Mod zip", "zip"}};
        g_mod_zip_dialog_pending = true;
        SDL_ShowOpenFileDialog(mod_zip_dialog_callback, nullptr, g_sdl_window,
                               filters, 1, nullptr, false);
    }
    ImGui::PopStyleColor(3);

    // Install status toast
    if (!g_mod_zip_status.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,
            g_mod_zip_status_ok ? ImVec4(0.24f, 0.72f, 0.31f, 1.0f)
                                : ImVec4(0.91f, 0.27f, 0.38f, 1.0f));
        ImGui::TextWrapped("%s", g_mod_zip_status.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) g_mod_zip_status.clear();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_mods.empty()) {
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
        ImGui::BeginChild("##noMods", ImVec2(cw, 180), ImGuiChildFlags_Borders);
        ImGui::SetCursorPos(ImVec2((cw - 220) * 0.5f, 50));
        ImGui::TextDisabled(ICON_FA_BOX_OPEN "  No mods installed.");
        ImGui::SetCursorPosX((cw - 320) * 0.5f);
        ImGui::TextDisabled("Install Raijin-format mod zips (icon.png + properties.toml + resources/).");
        ImGui::SetCursorPosX((cw - 400) * 0.5f + 40);
        ImGui::TextDisabled("Path:  ~/.local/share/swordigo-desktop/mods/");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    // ── Split into Available vs Active ───────────────────────────────────────
    struct ModEntry { int idx; ModInfo* mod; };
    std::vector<ModEntry> available, active;
    for (int i = 0; i < (int)g_mods.size(); i++) {
        if (launcher_config_is_mod_enabled(g_cfg, g_mods[i].id))
            active.push_back({i, &g_mods[i]});
        else
            available.push_back({i, &g_mods[i]});
    }
    // Sort active vector by config load order (top = highest priority)
    std::sort(active.begin(), active.end(), [&](const ModEntry& a, const ModEntry& b) {
        for (int i = 0; i < (int)g_cfg.mod_load_order.size(); i++) {
            if (g_cfg.mod_load_order[i] == a.mod->id) return true;   // a comes first
            if (g_cfg.mod_load_order[i] == b.mod->id) return false;  // b comes first
        }
        return false;
    });

    // ── Panel layout ────────────────────────────────────────────────────────
    float total_w = ImGui::GetContentRegionAvail().x;
    float arrow_col_w = 60.0f;
    float panel_w = (total_w - arrow_col_w) * 0.5f;
    static std::string selected_mod_id;  // track selected mod by ID (survives reorder)
    // Resolve current indices from the mod ID (rebuilt every frame from config)
    int selected_avail = -1, selected_active = -1;
    if (!selected_mod_id.empty()) {
        for (int i = 0; i < (int)available.size(); i++)
            if (available[i].mod->id == selected_mod_id) { selected_avail = i; break; }
        for (int i = 0; i < (int)active.size(); i++)
            if (active[i].mod->id == selected_mod_id) { selected_active = i; break; }
    }

    // ── Headers ─────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.45f, 0.65f, 0.95f, 1.0f),
        ICON_FA_CHEVRON_LEFT "  Available Mods  (%d)", (int)available.size());
    ImGui::SameLine(panel_w + arrow_col_w);
    ImGui::TextColored(ImVec4(0.24f, 0.72f, 0.31f, 1.0f),
        ICON_FA_CHEVRON_RIGHT "  Active Mods  (%d)  ", (int)active.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(top = highest priority)");
    ImGui::Spacing();

    // ── Left panel (Available) ──────────────────────────────────────────────
    ImGui::BeginChild("##avail_panel", ImVec2(panel_w, -60), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (int i = 0; i < (int)available.size(); i++) {
        bool sel = (selected_avail == i);
        if (draw_mod_card(*available[i].mod, panel_w, false, available[i].idx, sel)) {
            selected_mod_id = available[i].mod->id;
            // Double-click auto-activates
            if (ImGui::IsMouseDoubleClicked(0)) {
                launcher_config_enable_mod(g_cfg, available[i].mod->id);
                launcher_config_save(g_cfg);
            }
        }
        ImGui::Spacing();
    }
    if (available.empty()) {
        ImGui::SetCursorPos(ImVec2(panel_w * 0.5f - 80, 40));
        ImGui::TextDisabled("All mods are active");
    }
    ImGui::EndChild();

    // ── Center arrows ───────────────────────────────────────────────────────
    ImGui::SameLine();
    ImGui::BeginChild("##arrow_col", ImVec2(arrow_col_w, -60), 0);
    {
        float btn_w = 44.0f, btn_h = 30.0f;
        float cx = (arrow_col_w - btn_w) * 0.5f;
        ImGui::SetCursorPos(ImVec2(cx, 12));

        // Activate (>>)
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.50f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.62f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.42f, 0.22f, 1.0f));
        if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##act", ImVec2(btn_w, btn_h))) {
            if (selected_avail >= 0 && selected_avail < (int)available.size()) {
                launcher_config_enable_mod(g_cfg, available[selected_avail].mod->id);
                launcher_config_save(g_cfg);
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Activate selected mod");

        ImGui::SetCursorPos(ImVec2(cx, 52));
        // Deactivate (<<)
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.50f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.42f, 0.14f, 0.14f, 1.0f));
        if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##deact", ImVec2(btn_w, btn_h))) {
            if (selected_active >= 0 && selected_active < (int)active.size()) {
                launcher_config_disable_mod(g_cfg, active[selected_active].mod->id);
                selected_mod_id.clear();
                launcher_config_save(g_cfg);
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deactivate selected mod");

        ImGui::SetCursorPos(ImVec2(cx, 96));
        // Move up (higher priority = toward index 0)
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.38f, 0.58f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.48f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.32f, 0.50f, 1.0f));
        if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##up", ImVec2(btn_w, btn_h))) {
            std::cout << "[Launcher] MOVE UP clicked: selected_mod_id='" << selected_mod_id
                      << "' selected_active=" << selected_active
                      << "' active.size=" << active.size() << std::endl;
            if (selected_active >= 0 && selected_active < (int)active.size()) {
                std::string id = active[selected_active].mod->id;
                std::cout << "[Launcher] reorder up: id='" << id << "' delta=-1" << std::endl;
                bool ok = launcher_config_reorder_mod(g_cfg, id, -1);
                std::cout << "[Launcher] reorder result: " << (ok ? "OK" : "FAIL") << std::endl;
                launcher_config_save(g_cfg);
            } else {
                std::cout << "[Launcher] MOVE UP: no mod selected!" << std::endl;
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move up (higher priority)");

        ImGui::SetCursorPos(ImVec2(cx, 136));
        // Move down (lower priority = toward end)
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.38f, 0.25f, 0.58f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.48f, 0.30f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.32f, 0.20f, 0.50f, 1.0f));
        if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##dn", ImVec2(btn_w, btn_h))) {
            std::cout << "[Launcher] MOVE DOWN clicked: selected_mod_id='" << selected_mod_id
                      << "' selected_active=" << selected_active
                      << "' active.size=" << active.size() << std::endl;
            if (selected_active >= 0 && selected_active < (int)active.size()) {
                std::string id = active[selected_active].mod->id;
                std::cout << "[Launcher] reorder down: id='" << id << "' delta=1" << std::endl;
                bool ok = launcher_config_reorder_mod(g_cfg, id, 1);
                std::cout << "[Launcher] reorder result: " << (ok ? "OK" : "FAIL") << std::endl;
                launcher_config_save(g_cfg);
            } else {
                std::cout << "[Launcher] MOVE DOWN: no mod selected!" << std::endl;
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move down (lower priority)");
    }
    ImGui::EndChild();

    // ── Right panel (Active / Load Order) ───────────────────────────────────
    ImGui::SameLine();
    ImGui::BeginChild("##active_panel", ImVec2(panel_w, -60), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (int i = 0; i < (int)active.size(); i++) {
        bool sel = (selected_active == i);
        // Priority number badge (inline with card)
        float pnum_w = 28.0f;
        float card_y = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6, card_y + 18));
        ImGui::TextColored(ImVec4(0.24f, 0.72f, 0.31f, 0.9f), "#%d", i + 1);
        ImGui::SameLine();
        ImGui::SetCursorPosY(card_y);

        if (draw_mod_card(*active[i].mod, panel_w - pnum_w, true, active[i].idx, sel)) {
            selected_mod_id = active[i].mod->id;
            // Double-click auto-deactivates
            if (ImGui::IsMouseDoubleClicked(0)) {
                launcher_config_disable_mod(g_cfg, active[i].mod->id);
                selected_mod_id.clear();
                launcher_config_save(g_cfg);
            }
        }
        ImGui::Spacing();
    }
    if (active.empty()) {
        ImGui::SetCursorPos(ImVec2(panel_w * 0.5f - 100, 40));
        ImGui::TextDisabled("No active mods — vanilla will be used");
    }
    ImGui::EndChild();

    // ── Footer ──────────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Mods override resources in load order. Top mod wins conflicts. Double-click to toggle.");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save Order", ImVec2(120, 0))) {
        launcher_config_save(g_cfg);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH "  Delete Mod", ImVec2(120, 0))) {
        // Delete the currently selected mod from either panel
        int del_idx = -1;
        if (selected_active >= 0 && selected_active < (int)active.size())
            del_idx = active[selected_active].idx;
        else if (selected_avail >= 0 && selected_avail < (int)available.size())
            del_idx = available[selected_avail].idx;
        if (del_idx >= 0) {
            g_mod_confirm_delete = true;
            g_mod_delete_target_idx = del_idx;
        }
    }

    // ── Delete confirmation modal ───────────────────────────────────────────
    if (g_mod_confirm_delete) { ImGui::OpenPopup("Confirm Mod Delete"); g_mod_confirm_delete = false; }
    if (ImGui::BeginPopupModal("Confirm Mod Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  Remove this mod?");
        ImGui::Separator();
        if (g_mod_delete_target_idx >= 0 && g_mod_delete_target_idx < (int)g_mods.size()) {
            ImGui::Text("  %s  (v%s)", g_mods[g_mod_delete_target_idx].name.c_str(),
                        g_mods[g_mod_delete_target_idx].version.c_str());
            ImGui::TextDisabled("  The mod folder will be deleted permanently.");
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button(ICON_FA_TRASH "  Delete", ImVec2(120, 0))) {
            if (g_mod_delete_target_idx >= 0 && g_mod_delete_target_idx < (int)g_mods.size()) {
                ModInfo& m = g_mods[g_mod_delete_target_idx];
                launcher_config_disable_mod(g_cfg, m.id);
                modman::delete_mod(modman::ModMeta{ m.id, m.name, m.version, m.author,
                                                    m.description, m.category, m.type,
                                                    {}, m.dir_path, m.icon_path,
                                                    m.enabled, m.is_toml });
                g_mod_delete_target_idx = -1;
                g_mods_scanned = false;
                launcher_config_save(g_cfg);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        PushSecondaryBtn();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_mod_delete_target_idx = -1; ImGui::CloseCurrentPopup(); }
        PopSecondaryBtn();
        ImGui::EndPopup();
    }
}

// =============================================================================
// =============================================================================
// Mod Browser page — real Raijin-format store catalog
// =============================================================================

static void LoadCatalogFromString(const std::string& json, bool remote) {
    g_catalog_mods = modman::parse_catalog(json);
    g_catalog_categories.clear();
    for (const auto& m : g_catalog_mods) {
        if (m.category.empty()) continue;
        bool known = false;
        for (const auto& c : g_catalog_categories)
            if (c == m.category) { known = true; break; }
        if (!known) g_catalog_categories.push_back(m.category);
    }
    g_catalog_installed.assign(g_catalog_mods.size(), false);
    g_catalog_state.assign(g_catalog_mods.size(), 0);
    g_catalog_progress.assign(g_catalog_mods.size(), 0.0f);
    g_catalog_fail_reason.assign(g_catalog_mods.size(), "");
    g_catalog_jobs.clear();
    g_catalog_jobs.resize(g_catalog_mods.size());
    // Icon cache state (downloaded PNGs + GL textures + background jobs)
    // NOTE: runs on the catalog-fetch worker thread — never touch GL here;
    // retire textures to g_icon_tex_pending_free and let the UI thread free them.
    for (GLuint t : g_catalog_icon_tex) { if (t) g_icon_tex_pending_free.push_back(t); }
    g_catalog_icon_tex.assign(g_catalog_mods.size(), 0);
    g_catalog_icon_paths.assign(g_catalog_mods.size(), "");
    // Never clear live async futures (their destructor blocks until the task
    // finishes, stalling the UI on slow networks). Prune only finished jobs;
    // running ones just write to their own <id>.png path and are harmless.
    g_catalog_icon_jobs.erase(
        std::remove_if(g_catalog_icon_jobs.begin(), g_catalog_icon_jobs.end(),
                       [](std::future<void>& f) {
                           return f.valid() &&
                                  f.wait_for(std::chrono::milliseconds(0)) ==
                                      std::future_status::ready;
                       }),
        g_catalog_icon_jobs.end());
    g_catalog_loaded = true;
    g_catalog_loading = false;
    g_catalog_error.clear();
    // Refresh the installed mask against the local mods dir
    auto mask = modman::catalog_installed_mask(g_catalog_mods, get_user_data_dir() + "/mods");
    for (size_t i = 0; i < mask.size(); ++i) {
        if (mask[i]) g_catalog_state[i] = 2;
    }
    g_catalog_installed = std::move(mask);

    // Kick off background downloads of remote mod icons into the icon cache dir.
    std::string icons_dir = get_user_data_dir() + "/mod_cache/icons";
    std::error_code ec;
    fs::create_directories(icons_dir, ec);
    std::string ud = get_user_data_dir();
    for (size_t i = 0; i < g_catalog_mods.size(); ++i) {
        const std::string& icon_url = g_catalog_mods[i].icon_url;
        if (icon_url.empty()) continue;
        std::string id = g_catalog_mods[i].id;
        std::string dest = icons_dir + "/" + id + ".png";
        g_catalog_icon_paths[i] = dest;
        g_catalog_icon_jobs.push_back(std::async(std::launch::async, [icon_url, dest, ud]() {
            // Already cached?
            std::error_code ec2;
            if (fs::exists(dest, ec2) && fs::file_size(dest, ec2) > 0) return;
            std::string err;
            if (!modman::http_download(icon_url, dest, nullptr, nullptr, 20000, &err)) {
                std::error_code ec3;
                fs::remove(dest, ec3);
            }
        }));
    }
}

// Kicks off a background fetch of the live Raijin store catalog. Cache chain:
// remote HTTPS fetch → store_cache.json on disk → bundled demo catalog.
static void LoadCatalogAsync() {
    if (g_catalog_loading) return;
    g_catalog_loading = true;
    g_catalog_error.clear();
    g_catalog_cache_path = get_user_data_dir() + "/store_cache.json";
    std::string url = g_catalog_url;
    std::string cache_path = g_catalog_cache_path;
    std::async(std::launch::async, [url, cache_path]() {
        std::string json, err;
        bool from_remote = false;
        if (!url.empty()) {
            if (modman::http_get(url, &json, nullptr, nullptr, 25000, &err)) {
                from_remote = true;
                // Persist a fresh cache copy (Raijin does the same)
                std::ofstream f(cache_path, std::ios::binary | std::ios::trunc);
                if (f) f.write(json.data(), (std::streamsize)json.size());
            }
        }
        if (!from_remote) {
            // Try the on-disk cache before falling back to the bundled demo
            std::string cached;
            {
                std::ifstream f(cache_path, std::ios::binary);
                if (f) { std::ostringstream ss; ss << f.rdbuf(); cached = ss.str(); }
            }
            if (!cached.empty()) {
                json = cached;
                g_catalog_error = "Offline: showing cached catalog (" + err + ")";
            } else {
                if (!url.empty())
                    g_catalog_error = "Catalog fetch failed: " + err +
                                      " — using bundled demo catalog.";
                json = modman::demo_catalog_json();
            }
        }
        LoadCatalogFromString(json, from_remote || !url.empty());
    });
}

// Installs a store mod: downloads the zip (https supported via libcurl) then
// installs via the Raijin-format installer into mods/.
static void InstallStoreMod(int idx) {
    if (idx < 0 || idx >= (int)g_catalog_mods.size()) return;
    if (g_catalog_state[idx] != 0) return;
    const modman::StoreMod& m = g_catalog_mods[idx];
    g_catalog_state[idx] = 1;
    g_catalog_progress[idx] = 0.0f;
    g_catalog_fail_reason[idx].clear();
    std::string url = m.download_url;
    std::string mods_dir = get_user_data_dir() + "/mods";
    g_catalog_jobs[idx] = std::async(std::launch::async, [url, mods_dir, idx]() {
        std::string result;
        if (url.empty()) {
            result = "No download link in catalog for this demo entry.";
            return result;
        }
        // Stream to a temp zip then install
        std::string tmp = (fs::temp_directory_path() / ("swordfare_store_" + std::to_string(idx) + ".zip")).string();
        std::string err;
        if (!modman::http_download(url, tmp, nullptr, nullptr, 60000, &err)) {
            result = "Download failed: " + err;
            return result;
        }
        modman::ModMeta meta;
        if (!modman::install_mod_zip(tmp, mods_dir, &meta, &err)) {
            result = "Install failed: " + err;
            return result;
        }
        result.clear();  // success
        return result;
    });
}

static void DrawModBrowserPage() {
    // Free GL textures retired by catalog worker thread
    if (!g_icon_tex_pending_free.empty()) {
        glDeleteTextures((GLsizei)g_icon_tex_pending_free.size(),
                         g_icon_tex_pending_free.data());
        g_icon_tex_pending_free.clear();
    }
    if (!g_catalog_loaded && !g_catalog_loading) LoadCatalogAsync();

    // ── Header ────────────────────────────────────────────────────────────
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_GLOBE "  Mod Store");
    if (g_font_heading) ImGui::PopFont();

    // Search bar (full width)
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.063f, 0.082f, 0.120f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 320);
    ImGui::InputTextWithHint("##mbsearch", ICON_FA_MAGNIFYING_GLASS "  Search mods by name, author, or description...",
                             g_modbrowser_search, sizeof(g_modbrowser_search));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Category filter + Refresh on same line
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (g_modbrowser_cat > (int)g_catalog_categories.size()) g_modbrowser_cat = 0;
    {
        std::vector<const char*> cats;
        cats.push_back("All");
        for (const auto& c : g_catalog_categories) cats.push_back(c.c_str());
        ImGui::Combo("##mbcat", &g_modbrowser_cat, cats.data(), (int)cats.size());
    }
    ImGui::SameLine();
    PushSecondaryBtn();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Refresh", ImVec2(90, 0))) {
        g_catalog_loaded = false;
        g_catalog_loading = false;
        LoadCatalogAsync();
    }
    PopSecondaryBtn();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Status ────────────────────────────────────────────────────────────
    if (g_catalog_loading) {
        ImGui::TextColored(ImVec4(0.82f, 0.60f, 0.13f, 1.0f), ICON_FA_SPINNER "  Loading catalog...");
    } else if (!g_catalog_error.empty()) {
        ImGui::TextColored(ImVec4(0.91f, 0.27f, 0.38f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION "  %s", g_catalog_error.c_str());
    }
    if (g_catalog_loaded && !g_catalog_mods.empty()) {
        ImGui::TextDisabled("  %zu mods available  |  %zu categories", g_catalog_mods.size(), g_catalog_categories.size());
    }
    ImGui::Spacing();

    if (!g_catalog_loaded || g_catalog_mods.empty()) {
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
        ImGui::BeginChild("##mbempty", ImVec2(cw, 160), ImGuiChildFlags_Borders);
        ImGui::SetCursorPos(ImVec2((cw - 300) * 0.5f, 50));
        ImGui::TextDisabled(ICON_FA_CLOUD "  Loading the mod store...");
        ImGui::SetCursorPosX((cw - 400) * 0.5f);
        ImGui::TextDisabled("Fetching from the live Raijin store (falls back to disk cache, then bundled demo).");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    // Selected mod tracking (by index, resolved within the function)
    static int g_selected_catalog_mod = -1;

    // ── Featured banner ───────────────────────────────────────────────────
    {
        const modman::StoreMod* feat = nullptr;
        int feat_idx = -1;
        for (int i = 0; i < (int)g_catalog_mods.size(); i++) {
            if (g_catalog_mods[i].featured) { feat = &g_catalog_mods[i]; feat_idx = i; break; }
        }
        if (feat) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            float bw = ImGui::GetContentRegionAvail().x;
            float bh = 100.0f;
            // Gradient background
            dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), IM_COL32(12, 18, 32, 255), 12.0f);
            dl->AddRect(bp, ImVec2(bp.x + bw, bp.y + bh), IM_COL32(233, 69, 96, 80), 12.0f, 0, 2.0f);
            // Featured icon
            if (feat_idx >= 0 && g_catalog_icon_tex[feat_idx]) {
                dl->AddImage((ImTextureID)(intptr_t)g_catalog_icon_tex[feat_idx],
                             ImVec2(bp.x + 16, bp.y + 16), ImVec2(bp.x + 64, bp.y + 64));
            }
            float text_x = bp.x + (feat_idx >= 0 && g_catalog_icon_tex[feat_idx] ? 80 : 20);
            dl->AddText(ImVec2(text_x, bp.y + 12), IM_COL32(233, 69, 96, 255), ICON_FA_STAR "  FEATURED");
            dl->AddText(nullptr, ImGui::GetFontSize() + 6.0f,
                        ImVec2(text_x, bp.y + 34), IM_COL32(240, 240, 245, 255), feat->name.c_str());
            dl->AddText(ImVec2(text_x, bp.y + 64), IM_COL32(150, 160, 175, 220),
                        (feat->description.empty() ? feat->long_description : feat->description).c_str());
            dl->AddText(ImVec2(bp.x + bw - 200, bp.y + 40), IM_COL32(88, 166, 255, 220),
                        ("by " + feat->author + "  |  v" + feat->version).c_str());
            // Click on banner to select
            ImGui::Dummy(ImVec2(0, bh + 8));
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && feat_idx >= 0) {
                g_selected_catalog_mod = (g_selected_catalog_mod == feat_idx) ? -1 : feat_idx;
            }
        }
    }

    float detail_w = (g_selected_catalog_mod >= 0 && g_selected_catalog_mod < (int)g_catalog_mods.size()) ? 340.0f : 0.0f;
    float gap = detail_w > 0 ? 10.0f : 0.0f;
    float list_w = ImGui::GetContentRegionAvail().x - detail_w - gap;

    // ── Mod cards grid ────────────────────────────────────────────────────
    ImGui::BeginChild("##mb_grid", ImVec2(list_w, -40), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    float card_pad = 10.0f;
    float avail_w = ImGui::GetContentRegionAvail().x;
    int cols = 2;
    if (avail_w > 900) cols = 3;
    if (avail_w < 500) cols = 1;
    float card_w = (avail_w - card_pad * (cols - 1)) / cols;

    int shown = 0;
    for (int i = 0; i < (int)g_catalog_mods.size(); i++) {
        const modman::StoreMod& m = g_catalog_mods[i];

        // Filter
        if (g_modbrowser_cat > 0) {
            std::string want = g_catalog_categories[g_modbrowser_cat - 1];
            if (m.category != want) continue;
        }
        if (strlen(g_modbrowser_search) > 0) {
            std::string s = g_modbrowser_search;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            std::string n = m.name, d = m.description + m.long_description;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            std::transform(d.begin(), d.end(), d.begin(), ::tolower);
            if (n.find(s) == std::string::npos && d.find(s) == std::string::npos) continue;
        }

        if (shown % cols != 0) ImGui::SameLine(0, card_pad);
        shown++;

        bool is_sel = (g_selected_catalog_mod == i);
        bool installed = (g_catalog_state[i] == 2);

        // Card background
        ImVec4 bg = is_sel ? ImVec4(0.10f, 0.14f, 0.24f, 1.0f)
                   : installed ? ImVec4(0.05f, 0.07f, 0.11f, 1.0f)
                   : ImVec4(0.04f, 0.055f, 0.085f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.10f, 0.13f, 0.18f, 0.80f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, is_sel ? 2.0f : 1.0f);
        ImGui::PushID(i + 3000);

        float card_h = 150.0f;
        ImGui::BeginChild(("##mcard" + std::to_string(i)).c_str(),
                          ImVec2(card_w, card_h), ImGuiChildFlags_Borders);

        // Draw-list: left accent bar only (no separate rect border — child border handles it)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        ImU32 accent = installed ? IM_COL32(50, 180, 70, 255) : IM_COL32(60, 130, 220, 255);
        if (is_sel) accent = installed ? IM_COL32(80, 230, 100, 255) : IM_COL32(100, 170, 255, 255);
        dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + 4, wp.y + ws.y), accent, 2.0f);
        if (is_sel) {
            // Override child border color to accent for selected cards
            ImGui::PopStyleColor(); // pop generic border color
            ImU32 bc = installed ? IM_COL32(80, 230, 100, 200) : IM_COL32(100, 170, 255, 200);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4((bc >> 0) / 255.0f, (bc >> 8) / 255.0f,
                                                          (bc >> 16) / 255.0f, (bc >> 24) / 255.0f));
        }

        // Icon (draw-list only — no nested BeginChild, no boundary errors)
        if (!g_catalog_icon_tex[i] && !g_catalog_icon_paths[i].empty()) {
            std::error_code ec2;
            if (fs::exists(g_catalog_icon_paths[i], ec2) &&
                fs::file_size(g_catalog_icon_paths[i], ec2) > 0)
                g_catalog_icon_tex[i] = LoadTextureFromFile(g_catalog_icon_paths[i].c_str(), nullptr, nullptr);
        }
        ImVec2 icon_pos = ImGui::GetCursorScreenPos();
        if (g_catalog_icon_tex[i]) {
            dl->AddImage((ImTextureID)(intptr_t)g_catalog_icon_tex[i],
                         icon_pos, ImVec2(icon_pos.x + 44, icon_pos.y + 44));
        } else {
            // Draw placeholder icon directly on draw-list (no nested child!)
            dl->AddRectFilled(icon_pos, ImVec2(icon_pos.x + 44, icon_pos.y + 44),
                              IM_COL32(25, 30, 45, 200), 6.0f);
            dl->AddText(ImVec2(icon_pos.x + 12, icon_pos.y + 14),
                        IM_COL32(70, 90, 130, 200), ICON_FA_PUZZLE_PIECE);
        }
        ImGui::Dummy(ImVec2(44, 44));

        // Name + author — to the right of icon
        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            ImGui::Text("%s", m.name.c_str());
            ImGui::TextDisabled("v%s  by %s", m.version.c_str(),
                                m.author.empty() ? "Unknown" : m.author.c_str());
        }
        ImGui::EndGroup();

        // Category badge (top-right, draw-list overlay)
        if (!m.category.empty()) {
            ImVec2 csz = ImGui::CalcTextSize(m.category.c_str());
            float badge_x = wp.x + ws.x - csz.x - 16;
            dl->AddRectFilled(ImVec2(badge_x, wp.y + 8),
                              ImVec2(badge_x + csz.x + 10, wp.y + 8 + csz.y + 4),
                              IM_COL32(35, 45, 65, 220), 5.0f);
            dl->AddText(ImVec2(badge_x + 5, wp.y + 10),
                        IM_COL32(140, 160, 200, 240), m.category.c_str());
        }

        ImGui::Spacing();

        // Description (wraps to card width)
        std::string desc = m.description.empty() ? m.long_description : m.description;
        if (!desc.empty()) {
            ImGui::PushTextWrapPos(wp.x + ws.x - 10);
            ImGui::TextDisabled("%s", desc.c_str());
            ImGui::PopTextWrapPos();
        }

        // Small spacer before button (only if room left, no big gap)
        float remaining = ws.y - ImGui::GetCursorPosY() - 30;
        if (remaining > 4.0f)
            ImGui::Dummy(ImVec2(0, remaining > 20.0f ? 20.0f : remaining));

        // Action button (cursor-based, full width)
        float btn_w = ws.x - 24;
        if (g_catalog_state[i] == 0) {
            ImVec4 btn_col = installed ? ImVec4(0.13f, 0.16f, 0.22f, 0.8f) : ImVec4(0.24f, 0.72f, 0.31f, 1.0f);
            ImVec4 btn_hov = installed ? ImVec4(0.13f, 0.16f, 0.22f, 0.8f) : ImVec4(0.30f, 0.85f, 0.40f, 1.0f);
            ImVec4 btn_act = installed ? ImVec4(0.13f, 0.16f, 0.22f, 0.8f) : ImVec4(0.18f, 0.58f, 0.25f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btn_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, btn_act);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            if (installed) {
                ImGui::Button((ICON_FA_CHECK "  Installed##" + std::to_string(i)).c_str(), ImVec2(btn_w, 26));
            } else {
                if (ImGui::Button((ICON_FA_DOWNLOAD "  Install##" + std::to_string(i)).c_str(), ImVec2(btn_w, 26))) {
                    InstallStoreMod(i);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(m.download_url.empty()
                        ? "No download link. Install the mod zip from the Mods page."
                        : "Download & install this mod");
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        } else if (g_catalog_state[i] == 1) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.914f, 0.271f, 0.376f, 1.0f));
            char pt[32]; snprintf(pt, sizeof(pt), "%d%%", (int)(g_catalog_progress[i] * 100));
            ImGui::ProgressBar(g_catalog_progress[i], ImVec2(btn_w, 24), pt);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        } else if (g_catalog_state[i] == 3) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            if (ImGui::Button((ICON_FA_ARROWS_ROTATE "  Retry##" + std::to_string(i)).c_str(), ImVec2(btn_w, 24)))
                g_catalog_state[i] = 0;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (!g_catalog_fail_reason[i].empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", g_catalog_fail_reason[i].c_str());
        }

        // Click to select
        ImVec2 mpos = ImGui::GetIO().MousePos;
        bool mouse_over = (mpos.x >= wp.x && mpos.x <= wp.x + ws.x &&
                           mpos.y >= wp.y && mpos.y <= wp.y + ws.y);
        if (mouse_over && ImGui::IsMouseClicked(0)) {
            g_selected_catalog_mod = (g_selected_catalog_mod == i) ? -1 : i;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(3); // ChildRounding, WindowPadding, ChildBorderSize
        ImGui::PopStyleColor(2); // ChildBg, Border
        ImGui::PopID();

        if (shown % cols != 0 && i < (int)g_catalog_mods.size() - 1) continue;
    }

    if (shown == 0) {
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPos(ImVec2(cw * 0.5f - 80, 60));
        ImGui::TextDisabled("No mods match your search");
    }
    ImGui::EndChild();

    // ── Detail panel (right side) ─────────────────────────────────────────
    if (detail_w > 0) {
        ImGui::SameLine();
        const modman::StoreMod& m = g_catalog_mods[g_selected_catalog_mod];
        int si = g_selected_catalog_mod;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.065f, 0.10f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
        ImGui::BeginChild("##mb_detail", ImVec2(detail_w, -40), ImGuiChildFlags_Borders);

        // Header
        if (g_catalog_icon_tex[si]) {
            ImGui::Image((ImTextureID)(intptr_t)g_catalog_icon_tex[si], ImVec2(56, 56));
            ImGui::SameLine();
        }
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::TextWrapped("%s", m.name.c_str());
        if (g_font_heading) ImGui::PopFont();
        ImGui::TextDisabled("by %s  |  v%s", m.author.empty() ? "Unknown" : m.author.c_str(), m.version.c_str());

        if (!m.category.empty()) {
            ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 0.85f), "%s", m.category.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Description
        ImGui::TextWrapped("%s", (!m.description.empty() ? m.description : m.long_description).c_str());

        // Stats
        if (m.rating > 0 || m.installs > 0) {
            ImGui::Spacing();
            ImGui::TextDisabled(ICON_FA_STAR " %.1f rating  |  %s installs", m.rating,
                                m.installs >= 1000
                                    ? (std::to_string(m.installs / 1000) + "k").c_str()
                                    : std::to_string(m.installs).c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Action button (full width)
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 8));
        if (g_catalog_state[si] == 0) {
            bool inst = false;
            for (auto& lm : g_mods) { if (lm.id == m.id) { inst = true; break; } }
            if (inst) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.20f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.12f, 0.12f, 1.0f));
                if (ImGui::Button(ICON_FA_TRASH "  Uninstall", ImVec2(-1, 36))) {
                    for (auto& lm : g_mods) {
                        if (lm.id == m.id) {
                            modman::delete_mod(modman::ModMeta{ lm.id, lm.name, lm.version, lm.author,
                                                                lm.description, lm.category, lm.type,
                                                                {}, lm.dir_path, lm.icon_path,
                                                                lm.enabled, lm.is_toml });
                            g_catalog_state[si] = 0;
                            g_mods_scanned = false;
                            break;
                        }
                    }
                }
                ImGui::PopStyleColor(3);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.72f, 0.31f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.85f, 0.40f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.58f, 0.25f, 1.0f));
                if (ImGui::Button(ICON_FA_DOWNLOAD "  Install", ImVec2(-1, 36))) {
                    InstallStoreMod(si);
                }
                ImGui::PopStyleColor(3);
            }
        } else if (g_catalog_state[si] == 1) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.914f, 0.271f, 0.376f, 1.0f));
            char pt[32]; snprintf(pt, sizeof(pt), "Downloading %d%%", (int)(g_catalog_progress[si] * 100));
            ImGui::ProgressBar(g_catalog_progress[si], ImVec2(-1, 32), pt);
            ImGui::PopStyleColor();
        } else if (g_catalog_state[si] == 3) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Retry", ImVec2(-1, 36)))
                g_catalog_state[si] = 0;
            ImGui::PopStyleColor(2);
            if (!g_catalog_fail_reason[si].empty())
                ImGui::TextDisabled("%s", g_catalog_fail_reason[si].c_str());
        } else {
            // Installed — show uninstall
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button(ICON_FA_TRASH "  Uninstall", ImVec2(-1, 36))) {
                for (auto& lm : g_mods) {
                    if (lm.id == m.id) {
                        modman::delete_mod(modman::ModMeta{ lm.id, lm.name, lm.version, lm.author,
                                                            lm.description, lm.category, lm.type,
                                                            {}, lm.dir_path, lm.icon_path,
                                                            lm.enabled, lm.is_toml });
                        g_catalog_state[si] = 0;
                        g_mods_scanned = false;
                        break;
                    }
                }
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleVar(2);

        // Close button
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_XMARK "  Close", ImVec2(-1, 0))) {
            g_selected_catalog_mod = -1;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ── Footer ────────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextDisabled("Click a mod card to see details. Install from the detail panel or the card button.");

    // Poll background install jobs
    for (int i = 0; i < (int)g_catalog_jobs.size(); i++) {
        if (g_catalog_state[i] == 1 && g_catalog_jobs[i].valid() &&
            g_catalog_jobs[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            std::string res = g_catalog_jobs[i].get();
            if (res.empty()) {
                g_catalog_state[i] = 2;
                g_catalog_progress[i] = 1.0f;
                g_mods_scanned = false;
                auto mask = modman::catalog_installed_mask(g_catalog_mods, get_user_data_dir() + "/mods");
                g_catalog_installed = std::move(mask);
            } else {
                g_catalog_state[i] = 3;
                g_catalog_fail_reason[i] = res;
            }
        }
    }
}
// =============================================================================
// Profile page — real save stats + skin selector
// =============================================================================

static void DrawProfilePage() {
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_USER "  Profile");
    if (g_font_heading) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float cw = ImGui::GetContentRegionAvail().x;

    // ── Avatar + name card ────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16.0f);
    ImGui::BeginChild("##profileCard", ImVec2(cw, 140), ImGuiChildFlags_Borders);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();

        // Animated crimson gradient strip across the top
        float at = g_anim_time * 0.5f;
        ImU32 gc1 = IM_COL32((int)(180 + 50*std::sin(at)),     (int)(30+15*std::sin(at+1.0f)),  (int)(60+30*std::sin(at+2.0f)),  200);
        ImU32 gc2 = IM_COL32((int)(233 + 10*std::sin(at+0.5f)),(int)(69+20*std::sin(at+1.5f)),  96, 200);
        dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + cw, wp.y + 6), gc1, gc2, gc2, gc1);

        // Large avatar circle
        ImVec2 av(wp.x + 62, wp.y + 76);
        dl->AddCircleFilled(av, 46.0f, IM_COL32(18, 25, 42, 255));
        dl->AddCircle(av, 46.0f, IM_COL32(233, 69, 96, 180), 52, 2.5f);
        // Pulsing outer ring
        float pulse = 0.5f + 0.5f * std::sin(g_anim_time * 2.2f);
        dl->AddCircle(av, 50.0f + pulse * 4.0f, IM_COL32(233, 69, 96, (int)(40 * pulse)), 52, 1.2f);
        // Icon
        float icon_sz = 30.0f;
        dl->AddText(nullptr, icon_sz, ImVec2(av.x - 15, av.y - 15),
                    IM_COL32(233, 69, 96, 255), ICON_FA_USER);

        ImGui::SetCursorPos(ImVec2(124, 26));
        if (!g_profile_editing) {
            if (g_font_heading) ImGui::PushFont(g_font_heading);
            ImGui::Text("%s", g_profile_username);
            if (g_font_heading) ImGui::PopFont();
            ImGui::SetCursorPosX(124);
            ImGui::TextDisabled("Local Profile  ·  " ICON_FA_SHIELD "  Offline");
            ImGui::SetCursorPos(ImVec2(124, 84));
            PushSecondaryBtn();
            if (ImGui::Button(ICON_FA_PEN "  Edit Username", ImVec2(160, 30)))
                g_profile_editing = true;
            PopSecondaryBtn();
        } else {
            ImGui::SetCursorPos(ImVec2(124, 36));
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##uname", g_profile_username, sizeof(g_profile_username));
            ImGui::SetCursorPos(ImVec2(124, 78));
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.24f, 0.72f, 0.31f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.85f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.58f, 0.25f, 1.0f));
            if (ImGui::Button(ICON_FA_CHECK "  Save", ImVec2(90, 30))) {
                g_profile_editing = false;
                // Persist username to launcher.toml
                LauncherConfig lc = launcher_config_load();
                lc.profile_name = g_profile_username;
                launcher_config_save(lc);
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            PushSecondaryBtn();
            if (ImGui::Button("Cancel", ImVec2(80, 30))) g_profile_editing = false;
            PopSecondaryBtn();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ── Game save stats (from first found .gplayer) ─────────────────────────────
    SectionLabel(ICON_FA_GAUGE_HIGH, "GAME PROGRESS");
    // Load save stats lazily
    static bool      stats_loaded = false;
    static int       stats_coins = 0, stats_health = 0, stats_level = 0, stats_xp = 0;
    static float     stats_pct   = 0.0f;
    static std::string stats_name;
    if (!stats_loaded) {
        std::string sd = get_vfs_save_dir();
        auto paths = save_list_dir(sd);
        if (!paths.empty()) {
            SaveFile sf;
            if (save_load(paths[0], sf)) {
                stats_coins  = sf.game_state.character.coins;
                stats_health = sf.game_state.character.health;
                stats_level  = sf.game_state.character.level;
                stats_xp     = sf.game_state.character.xp;
                stats_pct    = sf.percent_completed;
                stats_name   = sf.name;
            }
        }
        stats_loaded = true;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("##savstats", ImVec2(-1, 102), ImGuiChildFlags_Borders);
    if (stats_name.empty()) {
        ImGui::SetCursorPos(ImVec2(16, 36));
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  No save file found. Start the game to create one.");
    } else {
        // 4-column stat grid
        struct GStat { const char* icon; const char* label; std::string val; ImVec4 col; };
        GStat gstats[] = {
            { ICON_FA_GEM,            "Coins",    std::to_string(stats_coins),  ImVec4(0.82f, 0.60f, 0.13f, 1.0f) },
            { ICON_FA_STAR,           "Health",   std::to_string(stats_health), ImVec4(0.914f, 0.271f, 0.376f, 1.0f) },
            { ICON_FA_GAUGE_HIGH,     "Level",    std::to_string(stats_level),  ImVec4(0.35f, 0.65f, 1.0f, 1.0f) },
            { ICON_FA_FILE,           "XP",       std::to_string(stats_xp),     ImVec4(0.24f, 0.72f, 0.31f, 1.0f) },
        };
        float gw = (ImGui::GetContentRegionAvail().x - 24) / 4.0f;
        ImGui::SetCursorPos(ImVec2(12, 10));
        ImGui::TextDisabled("Save: %s  ·  %.0f%% complete", stats_name.c_str(), stats_pct * 100.0f);
        ImGui::Spacing();
        for (int gi = 0; gi < 4; gi++) {
            if (gi > 0) ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(gstats[gi].col, "%s", gstats[gi].icon);
            ImGui::SameLine();
            if (g_font_heading) ImGui::PushFont(g_font_heading);
            ImGui::Text("%s", gstats[gi].val.c_str());
            if (g_font_heading) ImGui::PopFont();
            ImGui::TextDisabled("%s", gstats[gi].label);
            ImGui::EndGroup();
            if (gi < 3) { ImGui::SameLine(gw * (gi+1) + 12); }
        }
        // Completion bar
        ImGui::Spacing();
        char cbuf[32]; snprintf(cbuf, sizeof(cbuf), "%.0f%%", stats_pct * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.914f, 0.271f, 0.376f, 0.85f));
        ImGui::ProgressBar(stats_pct, ImVec2(-1, 6), "");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine(cw - 90);
    PushSecondaryBtn();
    ImGui::SetCursorPosX(cw - 92);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Refresh", ImVec2(88, 26)))
        stats_loaded = false;
    PopSecondaryBtn();

    ImGui::Spacing();

    // ── Skin selector ───────────────────────────────────────────────────────────
    SectionLabel(ICON_FA_SHIRT, "CHARACTER SKIN");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("##skins", ImVec2(-1, 110), ImGuiChildFlags_Borders);
    {
        const float sw = 80.0f, sh = 80.0f;
        struct SkinDef { const char* name; const char* icon; ImVec4 col; bool locked; };
        static const SkinDef skins[] = {
            { "Default",  ICON_FA_USER,       ImVec4(0.914f, 0.271f, 0.376f, 1.0f), false },
            { "Knight",   ICON_FA_SHIELD,     ImVec4(0.55f,  0.65f,  0.90f,  1.0f), false },
            { "Shadow",   ICON_FA_STAR,       ImVec4(0.40f,  0.35f,  0.55f,  1.0f), false },
            { "Golden",   ICON_FA_GEM,        ImVec4(0.82f,  0.60f,  0.13f,  1.0f), false },
            { "Upcoming", ICON_FA_LOCK,       ImVec4(0.35f,  0.40f,  0.50f,  1.0f), true  },
        };
        for (int si = 0; si < 5; si++) {
            if (si > 0) ImGui::SameLine();
            const auto& sk = skins[si];
            bool is_sel = (g_selected_skin == si);
            ImGui::BeginGroup();
            ImVec4 box_bg = is_sel
                ? ImVec4(0.914f * 0.18f, 0.271f * 0.18f, 0.376f * 0.18f, 1.0f)
                : ImVec4(0.063f, 0.082f, 0.120f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, box_bg);
            ImGui::PushStyleColor(ImGuiCol_Border,
                is_sel ? ImVec4(0.914f, 0.271f, 0.376f, 1.0f)
                       : ImVec4(0.133f, 0.165f, 0.220f, 0.35f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
            ImGui::BeginChild(("##sk" + std::to_string(si)).c_str(),
                              ImVec2(sw, sh - 10), ImGuiChildFlags_Borders);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                ImGui::IsMouseClicked(0) && !sk.locked)
                g_selected_skin = si;
            ImGui::SetCursorPos(ImVec2((sw-22)*0.5f, (sh-32)*0.5f));
            ImGui::TextColored(sk.locked ? ImVec4(0.35f,0.40f,0.50f,1.0f) : sk.col,
                               "%s", sk.icon);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            const float tw = ImGui::CalcTextSize(sk.name).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (sw - tw) * 0.5f);
            if (is_sel)
                ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "%s", sk.name);
            else
                ImGui::TextDisabled("%s", sk.name);
            ImGui::EndGroup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ── Account (future) ───────────────────────────────────────────────────────────
    SectionLabel(ICON_FA_CLOUD, "ACCOUNT (FUTURE)");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.063f, 0.090f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("##account", ImVec2(-1, 78), ImGuiChildFlags_Borders);
    ImGui::SetCursorPos(ImVec2(16, 14));
    ImGui::TextDisabled(ICON_FA_LOCK "  Account sync is not yet available.");
    ImGui::SetCursorPosX(16);
    ImGui::TextDisabled("      Sign in to sync your profile, skins, and mods across devices.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// =============================================================================
// Settings page  (v9.0 — persistent audio config via simple INI)
// =============================================================================

// Simple INI-style config persistence for audio settings.
static std::string settings_ini_path() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/swordigo-desktop/settings.ini";
}
static void settings_save_audio(float master, float music, float sfx) {
    std::string p = settings_ini_path();
    fs::create_directories(fs::path(p).parent_path());
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "[audio]\nmaster=%.4f\nmusic=%.4f\nsfx=%.4f\n", master, music, sfx);
    fclose(f);
}
static void settings_load_audio(float& master, float& music, float& sfx) {
    std::string p = settings_ini_path();
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v = 1.0f;
        if (sscanf(line, "master=%f", &v) == 1) master = v;
        else if (sscanf(line, "music=%f",  &v) == 1) music  = v;
        else if (sscanf(line, "sfx=%f",    &v) == 1) sfx    = v;
    }
    fclose(f);
}

static void DrawSettingsPage() {
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_GEAR "  Settings");
    if (g_font_heading) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("SettingsTabs")) {
        // ── Profile tab ──────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_USER "  Profile")) {
            ImGui::Spacing();
            ImGui::Text("Player Name");
            ImGui::SetNextItemWidth(240);
            ImGui::InputText("##settings_uname", g_profile_username, sizeof(g_profile_username));
            ImGui::TextDisabled("   Display name shown in the sidebar and launcher.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.72f, 0.31f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.85f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.58f, 0.25f, 1.0f));
            if (ImGui::Button(ICON_FA_CHECK "  Save Profile", ImVec2(160, 32))) {
                LauncherConfig lc = launcher_config_load();
                lc.profile_name = g_profile_username;
                launcher_config_save(lc);
            }
            ImGui::PopStyleColor(3);
            ImGui::EndTabItem();
        }

        // ── Graphics tab ─────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_PAINT_BRUSH "  Graphics")) {
            ImGui::Spacing();
            static bool postfx_enabled = true;
            ImGui::Checkbox("Enable PostFX (bloom, color grading)", &postfx_enabled);
            ImGui::TextDisabled("   Applies screen-space post-processing effects during gameplay.");

            ImGui::Spacing();
            ImGui::Checkbox("PVR Software Decompression", &g_pvr_software_decode);
            ImGui::TextDisabled("   When off, raw compressed PVR textures are sent directly to GPU (hardware decode).");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text(ICON_FA_DISPLAY "  Display:");
            int dc = 0;
            SDL_DisplayID* disps = SDL_GetDisplays(&dc);
            if (disps && dc > 0) {
                const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disps[0]);
                if (mode) {
                    ImGui::BulletText("Resolution: %dx%d", mode->w, mode->h);
                    ImGui::BulletText("Refresh Rate: %.1f Hz", mode->refresh_rate);
                }
                SDL_free(disps);
            } else { ImGui::TextDisabled("Could not query display."); }

            ImGui::Spacing();
            ImGui::Text(ICON_FA_MICROCHIP "  OpenGL:");
            ImGui::BulletText("Renderer: %s", (const char*)glGetString(GL_RENDERER));
            ImGui::BulletText("Version:  %s", (const char*)glGetString(GL_VERSION));
            ImGui::EndTabItem();
        }

        // ── SRE Hooks tab ─────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_CODE "  SRE Hooks")) {
            ImGui::TextWrapped("All 34 SRE hooks are active when libsre.so is enabled.");
            ImGui::Spacing();
            if (ImGui::BeginTable("HooksTbl", 3,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 340))) {
                ImGui::TableSetupColumn("Hook Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Category",  ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Status",    ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();
                struct HookEntry { const char* name; const char* category; };
                static const HookEntry hooks[] = {
                    {"CppString::assign","CppString"},{"CppString::append","CppString"},
                    {"CppString::c_str","CppString"},{"CppString::destructor","CppString"},
                    {"lua_pcall","Lua"},{"luaL_loadbuffer","Lua"},{"lua_newstate","Lua"},{"luaL_openlibs","Lua"},
                    {"BackgroundLayer::render","Background"},{"BackgroundLayer::update","Background"},{"BackgroundLayer::setTexture","Background"},
                    {"GUI::render","GUI"},{"GUI::update","GUI"},{"GUI::handleInput","GUI"},
                    {"GUI::showDialog","GUI"},{"GUI::hideDialog","GUI"},{"GUI::showHUD","GUI"},
                    {"GUI::hideHUD","GUI"},{"GUI::setButtonState","GUI"},
                    {"Player::onDeath","Death"},
                    {"TextInput::show","Text Input"},{"TextInput::hide","Text Input"},
                    {"TextInput::getText","Text Input"},{"TextInput::isActive","Text Input"},
                    {"MusicPlayer::play","Music"},{"MusicPlayer::stop","Music"},{"MusicPlayer::pause","Music"},
                    {"MusicPlayer::resume","Music"},{"MusicPlayer::setVolume","Music"},
                    {"MusicPlayer::isPlaying","Music"},{"MusicPlayer::crossfade","Music"},
                    {"Stats::track","Stats"},
                    {"MainMenu::show","Menu"},{"MainMenu::handleSelection","Menu"},
                };
                for (const auto& h : hooks) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(h.name);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(h.category);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.24f, 0.72f, 0.31f, 1.0f), ICON_FA_CIRCLE_CHECK "  Active");
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ── Audio tab (persistent via INI) ───────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_VOLUME_HIGH "  Audio")) {
            static float master_vol = 1.0f, music_vol = 0.85f, sfx_vol = 1.0f;
            static bool  audio_loaded = false;
            if (!audio_loaded) {
                settings_load_audio(master_vol, music_vol, sfx_vol);
                audio_loaded = true;
            }
            ImGui::Spacing();
            bool changed = false;
            ImGui::Text(ICON_FA_VOLUME_HIGH "  Master Volume");
            changed |= ImGui::SliderFloat("##mvol",  &master_vol, 0.0f, 1.0f);
            ImGui::Text(ICON_FA_MUSIC "  Music Volume");
            changed |= ImGui::SliderFloat("##muvol", &music_vol,  0.0f, 1.0f);
            ImGui::Text(ICON_FA_BOLT "  SFX Volume");
            changed |= ImGui::SliderFloat("##sfxv",  &sfx_vol,    0.0f, 1.0f);
            if (changed)
                settings_save_audio(master_vol, music_vol, sfx_vol);
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.24f, 0.72f, 0.31f, 0.9f),
                ICON_FA_CIRCLE_CHECK "  Volume settings saved to ~/.config/swordigo-desktop/settings.ini");
            ImGui::Spacing();
            ImGui::TextDisabled(ICON_FA_CIRCLE_INFO
                "  These values will be forwarded to the audio backend in a future update.");
            ImGui::EndTabItem();
        }

        // ── Loading Screen tab ─────────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_IMAGE "  Loading Screen")) {
            ImGui::Spacing();
            static bool slideshow_enabled = false;
            static bool slideshow_loaded = false;
            static bool slideshow_persistent = false;
            if (!slideshow_loaded) {
                // Load persistent setting from INI
                std::string ini_path = get_user_data_dir() + "/settings.ini";
                std::ifstream ini(ini_path);
                if (ini.is_open()) {
                    std::string line;
                    while (std::getline(ini, line)) {
                        if (line.find("loading_slideshow") != std::string::npos)
                            slideshow_enabled = (line.find("=true") != std::string::npos || line.find("=1") != std::string::npos);
                    }
                }
                slideshow_loaded = true;
            }

            ImGui::Text(ICON_FA_IMAGE "  Background Slideshow");
            ImGui::TextDisabled("   Cycles through in-game scenery backgrounds every 6 seconds on the boot loading screen.");
            ImGui::Spacing();
            if (ImGui::Checkbox("Enable background slideshow", &slideshow_enabled)) {
                // Save to INI
                std::string ini_path = get_user_data_dir() + "/settings.ini";
                std::string content;
                { std::ifstream ini(ini_path); if (ini.is_open()) content.assign(std::istreambuf_iterator<char>(ini), std::istreambuf_iterator<char>()); }
                // Replace or append
                std::string key = "loading_slideshow";
                std::string val = slideshow_enabled ? "true" : "false";
                bool found = false;
                std::istringstream iss(content);
                std::string new_content, line;
                while (std::getline(iss, line)) {
                    if (line.find(key + "=") != std::string::npos) {
                        new_content += key + "=" + val + "\n";
                        found = true;
                    } else {
                        new_content += line + "\n";
                    }
                }
                if (!found) new_content += key + "=" + val + "\n";
                std::ofstream out(ini_path);
                if (out.is_open()) out << new_content;
            }
            ImGui::Spacing();
            ImGui::Text(ICON_FA_IMAGE "  Logo Source");
            ImGui::TextDisabled("   The loading screen now shows the Swordfare Desktop icon (icon_app.png)");
            ImGui::TextDisabled("   instead of the vanilla Swordigo title logo.");
            ImGui::Spacing();
            ImGui::Text(ICON_FA_PERSON_RUNNING "  Hiro Animation");
            ImGui::TextDisabled("   Fixed: hiro is now smaller (38% of screen) and positioned at the bottom.");
            ImGui::EndTabItem();
        }

        // ── About tab ───────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO "  About")) {
            ImGui::Spacing();
            if (g_font_heading) ImGui::PushFont(g_font_heading);
            ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f),
                               ICON_FA_GAMEPAD "  Swordigo Desktop  v9.0 Platform Remaster");
            if (g_font_heading) ImGui::PopFont();
            ImGui::Spacing();
            ImGui::TextWrapped(
                "A desktop runtime for Swordigo using ARM binary translation (Unicorn / Dynarmic) "
                "with custom SRE hooks for full playability, mod support, and save editing.");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            // Changelog
            SectionLabel(ICON_FA_LIST, "v9.0 CHANGES");
            ImGui::BulletText("Hiro run animation: fixed scale, orientation (-90°), and floor grounding");
            ImGui::BulletText("Loading screen: shadow ellipse, shimmer sweep, vignette, fixed flavor text");
            ImGui::BulletText("Profile page: live save stats from .gplayer, animated avatar card");
            ImGui::BulletText("Settings: persistent audio INI at ~/.config/swordigo-desktop/settings.ini");
            ImGui::BulletText("Mod browser: featured mod carousel + real Raijin store catalog (6 mods)");
            ImGui::BulletText("SDK tools: Open Mods Folder quick action wired");
            ImGui::BulletText("News ticker: updated with current project status");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::Text(ICON_FA_STAR "  Credits:");
            ImGui::BulletText("Touch Foo Games — original Swordigo");
            ImGui::BulletText("Unicorn Engine — TCG CPU emulation");
            ImGui::BulletText("Dynarmic — JIT CPU emulation");
            ImGui::BulletText("Dear ImGui — immediate-mode UI");
            ImGui::BulletText("SDL3 — cross-platform windowing & input");
            ImGui::BulletText("Font Awesome — icon set");
            ImGui::BulletText("Space Grotesk — launcher typeface");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::Text(ICON_FA_CUBE "  Architecture:");
            ImGui::BulletText("SRT — Swordigo Runtime (overall architecture)");
            ImGui::BulletText("SRE — Swordigo Runtime Engine (libsre.so hooks)");
            ImGui::BulletText("Primary target: v1.4.12 ARM64");
            ImGui::BulletText("Secondary: v1.4.12 ARM32, v1.2.x legacy");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}
// =============================================================================
// SDK Tools page  (v9.0 — all actions wired)
// =============================================================================

static void DrawSDKToolsPage() {
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_WRENCH "  SDK & Developer Tools");
    if (g_font_heading) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Quick-access banner row (3 primary actions)
    {
        float bw = (ImGui::GetContentRegionAvail().x - 16) / 3.0f;
        struct QA { const char* icon; const char* label; const char* tip; ImVec4 col; int action; };
        static const QA qa[] = {
            { ICON_FA_EYE,         "Asset Viewer",   "Browse textures, scenes, PODs",  ImVec4(0.35f, 0.65f, 1.0f,  1.0f), 0 },
            { ICON_FA_FLOPPY_DISK, "Save Editor",    "Edit .gplayer save files",        ImVec4(0.82f, 0.60f, 0.13f, 1.0f), 1 },
            { ICON_FA_FOLDER_OPEN, "Mods Folder",    "Open mods directory in Files",    ImVec4(0.24f, 0.72f, 0.31f, 1.0f), 2 },
        };
        for (int qi = 0; qi < 3; qi++) {
            if (qi > 0) ImGui::SameLine();
            const auto& q = qa[qi];
            ImGui::PushID(qi + 900);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.082f, 0.120f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
            ImGui::BeginChild("##qa", ImVec2(bw, 68), ImGuiChildFlags_Borders);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            dl->AddCircleFilled(ImVec2(wp.x + 28, wp.y + 34), 18.0f, IM_COL32(14, 20, 35, 255));
            dl->AddText(ImVec2(wp.x + 20, wp.y + 26), ImGui::ColorConvertFloat4ToU32(q.col), q.icon);
            ImGui::SetCursorPos(ImVec2(54, 12));
            ImGui::Text("%s", q.label);
            ImGui::SetCursorPos(ImVec2(54, 34));
            ImGui::TextDisabled("%s", q.tip);
            ImGui::SetCursorPos(ImVec2(bw - 70, 18));
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.914f, 0.271f, 0.376f, 0.80f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.000f, 0.380f, 0.490f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.760f, 0.196f, 0.278f, 1.0f));
            if (ImGui::Button(ICON_FA_PLAY "##qa", ImVec2(52, 28))) {
                if (q.action == 0) {
                    launch_ruby_viewer();
                } else if (q.action == 1) {
                    g_show_save_ed = true;
                    g_save_loaded = false; g_save_sel = -1; g_save_status.clear();
                    std::string sd = get_vfs_save_dir();
                    g_save_paths = save_list_dir(sd);
                    g_save_files.clear();
                    for (auto& p : g_save_paths) { SaveFile sf; if (save_load(p, sf)) g_save_files.push_back(sf); }
                    g_save_loaded = true;
                } else if (q.action == 2) {
                    os_external::open_in_file_manager(get_user_data_dir() + "/mods");
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    SectionLabel(ICON_FA_LAYER_GROUP, "ALL TOOLS");

    // Full tool grid
    struct ToolCard {
        const char* icon; const char* name; const char* desc;
        const char* status; bool available;
        ImVec4 accent;
        int action; // 0=ruby, 1=save ed, 2=mods folder, -1=none
    };
    static const ToolCard tools[] = {
        { ICON_FA_EYE,          "Ruby Asset Viewer",  "Browse textures, scenes and binary assets",     "Available",   true,  ImVec4(0.35f, 0.65f, 1.0f,  1.0f), 0 },
        { ICON_FA_CODE,         "Lua Script Editor",  "Edit and inject Lua scripts into the runtime",  "In-Game Only",false, ImVec4(0.24f, 0.72f, 0.31f, 1.0f),-1 },
        { ICON_FA_FLOPPY_DISK,  "Save Editor",        "Edit .gplayer save files directly",             "Available",   true,  ImVec4(0.82f, 0.60f, 0.13f, 1.0f), 1 },
        { ICON_FA_TERMINAL,     "Lua Console",        "REPL console for live Lua commands",            "In-Game Only",false, ImVec4(0.55f, 0.45f, 0.90f, 1.0f),-1 },
        { ICON_FA_NETWORK_WIRED,"TCP Console",        "Remote Lua console over TCP (openport cmd)",    "In-Game Only",false, ImVec4(0.35f, 0.65f, 1.0f,  1.0f),-1 },
        { ICON_FA_FOLDER_OPEN,  "Open Mods Folder",   "Open mods directory in the file manager",       "Available",   true,  ImVec4(0.24f, 0.72f, 0.31f, 1.0f), 2 },
        { ICON_FA_CUBE,         "Scene Inspector",    "Visual scene graph and entity explorer",         "Coming Soon", false, ImVec4(0.914f, 0.271f, 0.376f, 1.0f),-1 },
        { ICON_FA_PAINTBRUSH,   "Texture Packer",     "Pack and convert texture atlases for modding",  "Coming Soon", false, ImVec4(0.35f, 0.65f, 1.0f,  1.0f),-1 },
    };
    const int ntool = (int)(sizeof(tools) / sizeof(tools[0]));

    float cw = (ImGui::GetContentRegionAvail().x - 12) / 2.0f;
    for (int i = 0; i < ntool; i++) {
        if (i % 2 != 0) ImGui::SameLine();
        const auto& t = tools[i];
        ImGui::PushID(i + 400);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            t.available ? ImVec4(0.063f, 0.082f, 0.120f, 1.0f)
                        : ImVec4(0.039f, 0.051f, 0.075f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
        ImGui::BeginChild(("##tool" + std::to_string(i)).c_str(),
                          ImVec2(cw, 92), ImGuiChildFlags_Borders);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddCircleFilled(ImVec2(wp.x + 30, wp.y + 46), 20.0f, IM_COL32(14, 20, 35, 255));
        dl->AddText(ImVec2(wp.x + 22, wp.y + 38), ImGui::ColorConvertFloat4ToU32(t.accent), t.icon);

        ImGui::SetCursorPos(ImVec2(58, 14));
        ImGui::Text("%s", t.name);
        ImGui::SetCursorPosX(58);
        ImGui::TextDisabled("%s", t.desc);

        ImVec4 stc = t.available ? ImVec4(0.24f, 0.72f, 0.31f, 0.7f)
                    : (strncmp(t.status, "Coming", 6) == 0)
                        ? ImVec4(0.50f, 0.35f, 0.80f, 0.7f)
                        : ImVec4(0.35f, 0.65f, 1.0f, 0.7f);
        ImGui::SetCursorPos(ImVec2(58, 64));
        ImGui::PushStyleColor(ImGuiCol_Button, stc);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, stc);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  stc);
        ImGui::SmallButton(t.status);
        ImGui::PopStyleColor(3);

        if (t.available && t.action >= 0) {
            float bww = ImGui::GetWindowWidth();
            ImGui::SetCursorPos(ImVec2(bww - 90, 30));
            PushSecondaryBtn();
            bool clicked = ImGui::Button(ICON_FA_PLAY "  Open", ImVec2(80, 30));
            PopSecondaryBtn();
            if (clicked) {
                if (t.action == 0) {
                    launch_ruby_viewer();
                } else if (t.action == 1) {
                    g_show_save_ed = true;
                    g_save_loaded = false; g_save_sel = -1; g_save_status.clear();
                    std::string sd = get_vfs_save_dir();
                    g_save_paths = save_list_dir(sd);
                    g_save_files.clear();
                    for (auto& p : g_save_paths) { SaveFile sf; if (save_load(p, sf)) g_save_files.push_back(sf); }
                    g_save_loaded = true;
                } else if (t.action == 2) {
                    os_external::open_in_file_manager(get_user_data_dir() + "/mods");
                }
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (i % 2 == 1) ImGui::Spacing();
    }
}

// =============================================================================
// Save Editor
// =============================================================================

static void DrawSaveEditor(bool& show_save_editor) {
    ImGui::SetCursorPos(ImVec2(0, 0));
    PushSecondaryBtn();
    if (ImGui::Button(ICON_FA_ARROW_LEFT "  Back", ImVec2(110, 32))) {
        show_save_editor = false;
        g_save_sel = -1;
        PopSecondaryBtn();
        return;
    }
    PopSecondaryBtn();
    ImGui::SameLine();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_FLOPPY_DISK "  Save Editor");
    if (g_font_heading) ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!g_save_loaded) { ImGui::TextDisabled("Loading saves…"); return; }

    if (g_save_files.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No .gplayer save files found.");
        ImGui::TextDisabled("Save directory: %s", get_vfs_save_dir().c_str());
        return;
    }

    // Editing a specific save
    if (g_save_sel >= 0 && g_save_sel < (int)g_save_files.size()) {
        SaveFile& sf = g_edit_save;
        if (!g_save_status.empty()) {
            ImVec4 col = g_save_status_ok ? ImVec4(0.24f, 0.72f, 0.31f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(col, "%s", g_save_status.c_str());
            ImGui::Spacing();
        }
        ImGui::Text(ICON_FA_FILE "  File: %s", fs::path(sf.filepath).filename().c_str());
        ImGui::Text("Player: %s  |  Level: %d  |  %.0f%% complete",
            sf.name.c_str(), sf.experience_level, sf.percent_completed * 100.0f);
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        SectionLabel(ICON_FA_PERSON_RUNNING, "CHARACTER STATS");
        ImGui::InputInt("Coins",  &sf.game_state.character.coins);
        ImGui::InputInt("Health", &sf.game_state.character.health);
        ImGui::InputInt("Mana",   &sf.game_state.character.mana);
        ImGui::InputInt("XP",     &sf.game_state.character.xp);
        ImGui::InputInt("Level",  &sf.game_state.character.level);
        ImGui::Spacing();
        SectionLabel(ICON_FA_WAND_MAGIC_SPARKLES, "ATTRIBUTES");
        ImGui::InputInt("Health Attr", &sf.game_state.character.health_attr);
        ImGui::InputInt("Attack Attr", &sf.game_state.character.attack_attr);
        ImGui::InputInt("Magic Attr",  &sf.game_state.character.magic_attr);
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.68f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.44f, 0.27f, 1.0f));
        if (ImGui::Button(ICON_FA_CHECK "  Apply & Save", ImVec2(160, 36))) {
            if (save_write(sf.filepath, sf)) {
                g_save_status = "Save written successfully!"; g_save_status_ok = true;
                g_save_files[g_save_sel] = sf;
            } else { g_save_status = "Failed to write save file!"; g_save_status_ok = false; }
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        PushSecondaryBtn();
        if (ImGui::Button("Discard", ImVec2(120, 36))) { g_save_sel = -1; g_save_status.clear(); }
        PopSecondaryBtn();
        return;
    }

    // Save file list table
    if (ImGui::BeginTable("SavesTbl", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
            ImVec2(0, ImGui::GetContentRegionAvail().y - 10))) {
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Level",    ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("File",     ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g_save_files.size(); i++) {
            auto& sf = g_save_files[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(sf.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                g_save_sel = i; g_edit_save = sf; g_save_status.clear();
            }
            ImGui::TableNextColumn(); ImGui::Text("%d", sf.experience_level);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", sf.percent_completed * 100.0f);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", fs::path(sf.filepath).filename().c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}






// =============================================================================
// show_launcher — main entry point
// =============================================================================

LaunchConfig show_launcher(BinarySelector& selector) {
    LaunchConfig cfg;
    cfg.graphics_api = GraphicsAPI::OPENGL;
    cfg.should_launch = false;  // Default to not launching (user must click)

    // Reset module state
    g_font_main       = nullptr;
    g_font_heading    = nullptr;
    g_mods.clear();
    g_mods_scanned    = false;
    g_save_loaded     = false;
    g_save_paths.clear();
    g_save_files.clear();
    g_save_sel        = -1;
    g_save_status.clear();
    g_confirm_delete  = false;
    g_delete_target_idx = -1;

    // Load persistent config (profile name, accent color, mod order, etc.)
    {
        LauncherConfig lc = launcher_config_load();
        if (!lc.profile_name.empty()) {
            strncpy(g_profile_username, lc.profile_name.c_str(), sizeof(g_profile_username) - 1);
        }
    }

    // Pre-select default binary
    const auto& bins = selector.get_binaries();
    int bin_sel = 0;
    for (size_t i = 0; i < bins.size(); i++) {
        if (bins[i].is_default) bin_sel = (int)i;
    }

    // ── SDL3 init ──
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[Launcher] SDL_Init failed: " << SDL_GetError() << std::endl;
        return cfg;
    }

    // OpenGL 3.3 Core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,         0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,          1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,            24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,          8);

    SDL_Window* window = SDL_CreateWindow(
        "Swordigo Desktop",
        1200, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS);
    if (!window) {
        std::cerr << "[Launcher] Window creation failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return cfg;
    }
    g_sdl_window = window;
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    // Set window icon — embedded asset first (permanent), then disk fallbacks
    {
        SDL_Surface* icon_surf = nullptr;
        const unsigned char* icon_data = nullptr;
        size_t icon_size = 0;
        if (embedded_asset("launcer_icon.png", &icon_data, &icon_size)) {
            icon_surf = LoadSurfaceFromMemory(icon_data, icon_size);
            if (icon_surf) std::cout << "[Launcher] Icon loaded from embedded assets" << std::endl;
        }
        if (!icon_surf) {
            std::string icon_via_data = get_data_path("src/assets/launcer_icon.png");
            std::string icon_via_launcher = get_user_data_dir() + "/launcher/launcer_icon.png";
            const char* icon_paths[] = {
                icon_via_data.c_str(),
                "src/assets/launcer_icon.png",
                icon_via_launcher.c_str(),
                "src/assets/icon_gnome.png",
                "/usr/share/icons/hicolor/128x128/apps/swordigo-desktop.png",
                "/usr/share/pixmaps/swordigo-desktop.png",
                nullptr
            };
            for (int i = 0; icon_paths[i]; i++) {
                icon_surf = IMG_Load(icon_paths[i]);
                if (icon_surf) {
                    std::cout << "[Launcher] Icon loaded from: " << icon_paths[i] << std::endl;
                    break;
                }
            }
        }
        if (icon_surf) {
            SDL_SetWindowIcon(window, icon_surf);
            SDL_DestroySurface(icon_surf);
        }
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "[Launcher] GL context failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return cfg;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Vsync
    sword_init_gl_after();

    // ── ImGui init ──
    IMGUI_CHECKVERSION();
    ImGuiContext* imgui_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Theme
    ApplyPremiumTheme();

    // Font loading — Inter + Font Awesome 7 icons, DPI-aware
    {
        float dpi_scale = 1.0f;
        int display_id = SDL_GetDisplayForWindow(window);
        if (display_id) {
            float content_scale = SDL_GetDisplayContentScale(display_id);
            if (content_scale > 0) dpi_scale = content_scale;
        }
        if (dpi_scale < 1.0f) dpi_scale = 1.0f;
        if (dpi_scale > 3.0f) dpi_scale = 3.0f;
        
        float font_size_main    = 17.0f * dpi_scale;
        float font_size_heading = 26.0f * dpi_scale;
        
        // Primary font config (Inter)
        ImFontConfig text_cfg;
        text_cfg.OversampleH = 3;
        text_cfg.OversampleV = 2;
        text_cfg.PixelSnapH = true;
        // Embedded fonts are static .rodata in the binary — never let the
        // atlas free() them (default is owned=true → free(): invalid size crash)
        text_cfg.FontDataOwnedByAtlas = false;
        
        // Icon font config (Font Awesome — merged into same atlas)
        static const ImWchar icon_ranges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
        ImFontConfig icon_cfg;
        icon_cfg.MergeMode = true;
        icon_cfg.OversampleH = 2;
        icon_cfg.OversampleV = 2;
        icon_cfg.PixelSnapH = true;
        icon_cfg.GlyphMinAdvanceX = font_size_main;
        icon_cfg.GlyphOffset = ImVec2(0, 2);
        icon_cfg.FontDataOwnedByAtlas = false;
        
        // Search paths — SpaceGrotesk is the primary launcher font
        std::string inter_paths[] = {
            "src/assets/fonts/SpaceGrotesk-VariableFont_wght.ttf",
            get_data_path("src/assets/fonts/SpaceGrotesk-VariableFont_wght.ttf"),
            get_user_data_dir() + "launcher/fonts/SpaceGrotesk-VariableFont_wght.ttf",
            get_user_data_dir() + "src/assets/fonts/SpaceGrotesk-VariableFont_wght.ttf",
            // Fallbacks
            "src/assets/fonts/MegalopolisExtra-Regular.otf",
            get_data_path("src/assets/fonts/MegalopolisExtra-Regular.otf"),
            get_user_data_dir() + "launcher/fonts/MegalopolisExtra-Regular.otf",
            get_user_data_dir() + "src/assets/fonts/MegalopolisExtra-Regular.otf",
            get_data_path("src/assets/fonts/Inter-Regular.ttf"),
            get_user_data_dir() + "launcher/fonts/Inter-Regular.ttf",
            get_user_data_dir() + "src/assets/fonts/Inter-Regular.ttf",
            "src/assets/fonts/Inter-Regular.ttf",
            "/usr/share/swordigo-desktop/launcher/fonts/SpaceGrotesk-VariableFont_wght.ttf",
            "/usr/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
            "/usr/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
            "/usr/local/share/swordigo-desktop/launcher/fonts/SpaceGrotesk-VariableFont_wght.ttf",
            "/usr/local/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
            "/usr/local/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
            "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf",
        };
        
        // Search paths for Font Awesome (FA7 .otf or FA6 .ttf)
        std::string fa_paths[] = {
            "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            get_data_path("src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf"),
            // launcher/ subfolder (RPM/DEB friendly)
            get_user_data_dir() + "launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            get_user_data_dir() + "launcher/fonts/fa-solid-900.ttf",
            get_user_data_dir() + "launcher/fonts/fa-solid-900.otf",
            // FA6/FA7 in fonts directory (legacy/simple naming)
            "src/assets/fonts/fa-solid-900.ttf",
            "src/assets/fonts/fa-solid-900.otf",
            get_data_path("src/assets/fonts/fa-solid-900.ttf"),
            get_data_path("src/assets/fonts/fa-solid-900.otf"),
            get_user_data_dir() + "src/assets/fonts/fa-solid-900.ttf",
            get_user_data_dir() + "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            // System install paths (RPM/DEB friendly launcher/ subfolders)
            "/usr/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            "/usr/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf",
            "/usr/share/swordigo-desktop/launcher/fonts/fa-solid-900.otf",
            "/usr/local/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            "/usr/local/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf",
            "/usr/local/share/swordigo-desktop/launcher/fonts/fa-solid-900.otf",
            "/usr/share/swordigo-desktop/src/assets/fonts/fa-solid-900.ttf",
            "/usr/share/swordigo-desktop/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        };
        
        // Find font files (disk fallback only)
        std::string inter_path, fa_path;
        for (auto& fp : inter_paths) {
            if (fs::exists(fp)) { inter_path = fp; break; }
        }
        for (auto& fp : fa_paths) {
            if (fs::exists(fp)) { fa_path = fp; break; }
        }

        // ── Embedded fonts FIRST (permanent fix — always available) ──
        const unsigned char* emb_main = nullptr; size_t emb_main_size = 0;
        const unsigned char* emb_fa   = nullptr; size_t emb_fa_size   = 0;
        const char* emb_font_names[] = {
            "fonts/SpaceGrotesk-VariableFont_wght.ttf",
            "fonts/MegalopolisExtra-Regular.otf",
            "fonts/Inter-Regular.ttf",
        };
        for (const char* n : emb_font_names) {
            if (embedded_asset(n, &emb_main, &emb_main_size)) break;
        }
        if (!embedded_asset("fonts/fa-solid-900.ttf", &emb_fa, &emb_fa_size))
            embedded_asset("fonts/fa-solid-900.otf", &emb_fa, &emb_fa_size);

        bool font_loaded = false;
        if (emb_main && emb_main_size > 0) {
            g_font_main = io.Fonts->AddFontFromMemoryTTF((void*)emb_main, (int)emb_main_size,
                                                         font_size_main, &text_cfg);
            // Merge Font Awesome icons into main font
            if (g_font_main && emb_fa && emb_fa_size > 0) {
                ImFontConfig icon_cfg_emb = icon_cfg;
                icon_cfg_emb.GlyphMinAdvanceX = font_size_main;
                icon_cfg_emb.GlyphOffset = ImVec2(0, 2);
                io.Fonts->AddFontFromMemoryTTF((void*)emb_fa, (int)emb_fa_size,
                                               font_size_main * 0.85f, &icon_cfg_emb, icon_ranges);
                std::cout << "[Launcher] FontAwesome icons merged from embedded font" << std::endl;
            }
            // Load heading font from the same embedded face
            ImFontConfig heading_cfg = text_cfg;
            g_font_heading = io.Fonts->AddFontFromMemoryTTF((void*)emb_main, (int)emb_main_size,
                                                            font_size_heading, &heading_cfg);
            if (g_font_heading && emb_fa && emb_fa_size > 0) {
                ImFontConfig icon_heading_cfg = icon_cfg;
                icon_heading_cfg.GlyphMinAdvanceX = font_size_heading;
                icon_heading_cfg.GlyphOffset = ImVec2(0, 3);
                io.Fonts->AddFontFromMemoryTTF((void*)emb_fa, (int)emb_fa_size,
                                               font_size_heading * 0.85f, &icon_heading_cfg, icon_ranges);
            }
            if (g_font_main && g_font_heading) {
                font_loaded = true;
                std::cout << "[Launcher] Embedded font loaded (scale=" << dpi_scale
                          << "x, size=" << font_size_main << "px)" << std::endl;
            }
        }

        // Disk fallback (old behaviour)
        if (!font_loaded && !inter_path.empty()) {
            g_font_main = io.Fonts->AddFontFromFileTTF(inter_path.c_str(), font_size_main, &text_cfg);
            
            // Merge Font Awesome icons into main font
            if (g_font_main && !fa_path.empty()) {
                icon_cfg.GlyphMinAdvanceX = font_size_main;
                icon_cfg.GlyphOffset = ImVec2(0, 2);
                io.Fonts->AddFontFromFileTTF(fa_path.c_str(), font_size_main * 0.85f, &icon_cfg, icon_ranges);
                std::cout << "[Launcher] Icons merged (FA) from: " << fa_path << std::endl;
            }
            
            // Load heading font
            ImFontConfig heading_cfg = text_cfg;
            g_font_heading = io.Fonts->AddFontFromFileTTF(inter_path.c_str(), font_size_heading, &heading_cfg);
            
            // Merge FA icons into heading font too
            if (g_font_heading && !fa_path.empty()) {
                ImFontConfig icon_heading_cfg = icon_cfg;
                icon_heading_cfg.GlyphMinAdvanceX = font_size_heading;
                icon_heading_cfg.GlyphOffset = ImVec2(0, 3);
                io.Fonts->AddFontFromFileTTF(fa_path.c_str(), font_size_heading * 0.85f, &icon_heading_cfg, icon_ranges);
            }
            
            if (g_font_main && g_font_heading) {
                font_loaded = true;
                std::cout << "[Launcher] Font loaded: " << inter_path 
                          << " (scale=" << dpi_scale << "x, size=" << font_size_main << "px)" << std::endl;
            }
        }
        
        if (!font_loaded) {
            std::cout << "[Launcher] WARNING: Using ImGui default font (Inter not found)" << std::endl;
            g_font_main    = io.Fonts->AddFontDefault();
            g_font_heading = g_font_main;
        }
        
        if (fa_path.empty()) {
            std::cout << "[Launcher] WARNING: Font Awesome not found — icons will show as '?'" << std::endl;
            std::cout << "[Launcher] Place Font Awesome 7 Free-Solid-900.otf in src/assets/fontawesome/otfs/" << std::endl;
        }
        
        io.FontGlobalScale = 1.0f / dpi_scale;
    }

    // Platform/renderer backends
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Load UI textures
    {
        auto try_load = [](const char* sub, int* ow = nullptr, int* oh = nullptr) -> GLuint {
            int w = 0, h = 0;
            GLuint tex = 0;
            // Try 0: embedded in binary — ALWAYS available (permanent fix)
            tex = LoadTextureEmbedded(sub, &w, &h);
            if (!tex) {
                // Also try without a leading dir if the caller passed an "icons/" path
                const char* slash = strrchr(sub, '/');
                if (slash) tex = LoadTextureEmbedded(slash + 1, &w, &h);
            }
            // Try 1: relative to CWD (dev mode)
            if (!tex)
                tex = LoadTextureFromFile((std::string("src/assets/") + sub).c_str(), &w, &h);
            // Try 2: launcher/ subfolder in user data dir (RPM/DEB friendly)
            if (!tex) {
                std::string p2 = get_user_data_dir() + "launcher/" + sub;
                tex = LoadTextureFromFile(p2.c_str(), &w, &h);
            }
            // Try 3: user data dir src/assets (legacy layout)
            if (!tex) {
                std::string p3 = get_user_data_dir() + "src/assets/" + sub;
                tex = LoadTextureFromFile(p3.c_str(), &w, &h);
            }
            // Try 3b: user data dir /assets/ (original fallback)
            if (!tex) {
                std::string p3b = get_user_data_dir() + "assets/" + sub;
                tex = LoadTextureFromFile(p3b.c_str(), &w, &h);
            }
            // Try 4: via get_data_path (resolves system install paths)
            if (!tex) {
                std::string p4 = get_data_path(std::string("assets/") + sub);
                tex = LoadTextureFromFile(p4.c_str(), &w, &h);
            }
            // Try 5: also try src/assets under get_data_path
            if (!tex) {
                std::string p5 = get_data_path(std::string("src/assets/") + sub);
                tex = LoadTextureFromFile(p5.c_str(), &w, &h);
            }
            // Try 5b: launcher/ folder under get_data_path
            if (!tex) {
                std::string p5b = get_data_path(std::string("launcher/") + sub);
                tex = LoadTextureFromFile(p5b.c_str(), &w, &h);
            }
            // Try 6: system install path (deb/rpm packages)
            if (!tex) {
                std::string p6 = std::string("/usr/share/swordigo-desktop/launcher/") + sub;
                tex = LoadTextureFromFile(p6.c_str(), &w, &h);
            }
            // Try 6b: local system install path
            if (!tex) {
                std::string p6b = std::string("/usr/local/share/swordigo-desktop/launcher/") + sub;
                tex = LoadTextureFromFile(p6b.c_str(), &w, &h);
            }
            // Try 7: fallback to old packaging paths
            if (!tex) {
                std::string p7 = std::string("/usr/share/swordigo-desktop/src/assets/") + sub;
                tex = LoadTextureFromFile(p7.c_str(), &w, &h);
            }
            if (ow) *ow = w;
            if (oh) *oh = h;
            return tex;
        };
        g_tex_bg = try_load("launcher_bg.png", &g_tex_bg_w, &g_tex_bg_h);
        g_tex_icon_swordigo = try_load("icons/swordigo_default.png");
        g_tex_icon_swmini = try_load("icons/swmini_default.png");
        g_tex_icon_rlswordigo = try_load("icons/rl_swordigo_default.png");
        g_tex_icon_app = try_load("icon_app.png");
        g_tex_logo = try_load("swordigo_desktop_text.png", &g_tex_logo_w, &g_tex_logo_h);
        if (g_tex_bg) std::cout << "[LAUNCHER] Background texture loaded (" << g_tex_bg_w << "x" << g_tex_bg_h << ")" << std::endl;
        else std::cout << "[LAUNCHER] Warning: Background texture not found" << std::endl;
        if (g_tex_logo) std::cout << "[LAUNCHER] Logo texture loaded (" << g_tex_logo_w << "x" << g_tex_logo_h << ")" << std::endl;
        else std::cout << "[LAUNCHER] Warning: Logo texture not found, using text fallback" << std::endl;
    }

    // ── Main loop ──
    bool running = true;
    int api_sel = 0;        // 0 = OpenGL, 1 = Vulkan
    int engine_sel = 1;     // 0 = Unicorn, 1 = Dynarmic (default: JIT for performance)
    bool use_sre_sel = true; // whether to load libsre.so (user choice)
    bool adv_redstell_opts_sel = false; // Advanced Redstell Optimisations (off by default)

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    cfg.should_launch = false;
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        cfg.should_launch = false;
                        running = false;
                    }
                    else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                        // Launch the selected binary
                        const auto& cur_bins = selector.get_binaries();
                        if (!cur_bins.empty() && bin_sel >= 0 && bin_sel < (int)cur_bins.size()) {
                            cfg.graphics_api = (api_sel == 0) ? GraphicsAPI::OPENGL : GraphicsAPI::VULKAN;
                            cfg.use_dynarmic = (engine_sel == 1);
                            if (backend_selection_ok(cfg.use_dynarmic)) {
                                cfg.use_sre = use_sre_sel;
                                cfg.advanced_redstell_opts = adv_redstell_opts_sel;
                                cfg.selected_binary = cur_bins[bin_sel].filepath;
                                cfg.assets_dir = cur_bins[bin_sel].assets_dir;
                                cfg.game_type = cur_bins[bin_sel].game_type;
                                cfg.should_launch = true;
                                running = false;
                            }
                        }
                    }
                    else if (event.key.key == SDLK_DELETE) {
                        const auto& cur_bins = selector.get_binaries();
                        if (!cur_bins.empty() && bin_sel >= 0 && bin_sel < (int)cur_bins.size()) {
                            g_confirm_delete = true;
                            g_delete_target_idx = bin_sel;
                        }
                    }
                    break;
            }
        }

        if (!running) break;

        // Animation timer
        static Uint64 animation_time = SDL_GetTicks();
        Uint64 now = SDL_GetTicks();
        g_anim_time += std::min(0.1f, (float)(now - animation_time) / 1000.0f);
        animation_time = now;

        // Poll background catalog jobs + mod install results
        for (int i = 0; i < (int)g_catalog_jobs.size(); i++) {
            if (g_catalog_state[i] == 1 && g_catalog_jobs[i].valid() &&
                g_catalog_jobs[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                std::string res = g_catalog_jobs[i].get();
                if (res.empty()) {
                    g_catalog_state[i] = 2;
                    g_catalog_progress[i] = 1.0f;
                    g_mods_scanned = false;
                } else {
                    g_catalog_state[i] = 3;
                    g_catalog_fail_reason[i] = res;
                }
            }
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // New sidebar-based layout
        DrawSidebar(selector, bin_sel, running, cfg);
        DrawContentArea(selector, bin_sel, cfg, running, api_sel, engine_sel, use_sre_sel, adv_redstell_opts_sel);
        DrawStatusFooter(bin_sel, selector);
        DrawWindowControls(running, cfg);

        // Add Instance popup
        if (g_show_add_instance) {
            ImGui::OpenPopup("Add Instance");
        }
        ImVec2 popup_center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(popup_center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Add Instance", &g_show_add_instance, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));

            if (g_add_copying && g_apk_import_future.valid() &&
                g_apk_import_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const ApkImportResult result = g_apk_import_future.get();
                g_add_copying = false;
                if (result.success) {
                    selector.reload_instances();
                    bin_sel = static_cast<int>(selector.get_binaries().size()) - 1;
                    g_add_apk_path[0] = '\0';
                    g_show_add_instance = false;
                    ImGui::CloseCurrentPopup();
                } else {
                    g_add_status = "APK import failed: " + result.error;
                }
            }

            ImGui::TextDisabled("Create from installed assets, a custom folder, or a complete Android APK.");
            ImGui::Spacing();

            // -- APK IMPORT --
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.345f, 0.651f, 1.000f, 1.0f));
            ImGui::Text(ICON_FA_FILE "  IMPORT FROM APK");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::SetNextItemWidth(-112);
            ImGui::InputTextWithHint("##apk_path", "Swordigo APK path", g_add_apk_path, sizeof(g_add_apk_path));
            ImGui::SameLine();
            ImGui::BeginDisabled(g_add_apk_dialog_pending);
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse", ImVec2(104, 0))) {
                static const SDL_DialogFileFilter filters[] = {{"Android packages", "apk"}};
                g_add_apk_dialog_pending = true;
                SDL_ShowOpenFileDialog(apk_dialog_callback, nullptr, g_sdl_window,
                                       filters, 1, nullptr, false);
            }
            ImGui::EndDisabled();
            if (g_add_apk_path[0] || g_add_copying) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.78f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.63f, 0.92f, 1.0f));
                ImGui::BeginDisabled(g_add_copying);
                if (ImGui::Button(g_add_copying ? "Importing APK..."
                                                : ICON_FA_BOX_OPEN "  Import APK as Instance", ImVec2(-1, 38))) {
                    g_add_status.clear();
                    if (g_add_name[0] == '\0') {
                        g_add_status = "Enter an instance name before importing.";
                    } else {
                        const std::string apk = g_add_apk_path;
                        const std::string instance_name = g_add_name;
                        const std::string data_root = get_user_data_dir();
                        g_add_copying = true;
                        g_apk_import_future = std::async(std::launch::async,
                            [apk, instance_name, data_root]() {
                                BinarySelector importer;
                                importer.set_data_dir(data_root);
                                ApkImportResult result;
                                result.success = importer.import_apk_instance(apk, instance_name, &result.error);
                                return result;
                            });
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopStyleColor(2);
            }
            ImGui::Spacing();
            ImGui::Spacing();

            // -- INSTANCE DETAILS --
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.345f, 0.651f, 1.000f, 1.0f));
            ImGui::Text(ICON_FA_CUBE "  INSTANCE DETAILS");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::AlignTextToFramePadding();
            ImGui::Text("Instance Name");
            ImGui::SameLine(160);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##inst_name", g_add_name, sizeof(g_add_name));

            // Game Type
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Game Type");
            ImGui::SameLine(160);
            ImGui::SetNextItemWidth(-1);
            const char* game_types[] = { "Swordigo", "RLSwordigo", "Combatch", "Custom" };
            static int gt_sel = 0;
            if (ImGui::Combo("##game_type", &gt_sel, game_types, 4)) {
                strncpy(g_add_game_type, game_types[gt_sel], sizeof(g_add_game_type) - 1);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // -- ASSETS --
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.345f, 0.651f, 1.000f, 1.0f));
            ImGui::Text(ICON_FA_FOLDER "  ASSETS");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::AlignTextToFramePadding();
            ImGui::Text("Asset Source");
            ImGui::SameLine(160);
            ImGui::RadioButton("Vanilla (assets/)", &g_add_asset_type, 0);
            ImGui::SameLine();
            ImGui::RadioButton("RL (rl_assets/)", &g_add_asset_type, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Custom folder", &g_add_asset_type, 2);

            if (g_add_asset_type == 2) {
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Folder Path");
                ImGui::SameLine(160);
                ImGui::SetNextItemWidth(-70);
                ImGui::InputText("##custom_assets", g_add_custom_assets, sizeof(g_add_custom_assets));
                ImGui::SameLine();
                if (ImGui::Button("Browse##ca", ImVec2(60, 0))) {
                    std::string data_dir = get_user_data_dir();
                    os_external::open_in_file_manager(data_dir);
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // -- ENGINE (collapsed) --
            if (ImGui::CollapsingHeader(ICON_FA_MICROCHIP "  Engine (Advanced)")) {
                ImGui::Indent(12);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Use SRE");
                ImGui::SameLine(160);
                ImGui::Checkbox("##use_sre", &g_add_use_sre);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Swordigo Runtime Engine (libsre.so)");
                    ImGui::Text("Provides hooks, fixes, and mod support.");
                    ImGui::Text("Required for mods, save editing, and custom content.");
                    ImGui::EndTooltip();
                }

                if (g_add_use_sre) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.247f, 0.725f, 0.314f, 1.0f));
                    ImGui::TextWrapped("  " ICON_FA_CHECK " SRE replaces libmini.so / libGlossHook.so");
                    ImGui::PopStyleColor();
                }
                ImGui::Unindent(12);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Status message
            if (!g_add_status.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", g_add_status.c_str());
                ImGui::Spacing();
            }

            // Action buttons
            float btn_w = 120;
            float total_w = btn_w * 2 + 8;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total_w) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.34f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.68f, 0.40f, 1.0f));
            if (ImGui::Button(ICON_FA_PLUS "  Create", ImVec2(btn_w, 36))) {
                g_add_status.clear();
                // Validate
                if (strlen(g_add_name) == 0) {
                    g_add_status = "Instance name is required.";
                } else {
                    // Determine assets_dir
                    std::string assets_dir_name;
                    if (g_add_asset_type == 0) {
                        assets_dir_name = "assets";
                    } else if (g_add_asset_type == 1) {
                        assets_dir_name = "rl_assets";
                    } else {
                        // Custom: copy folder to inst-<name>/
                        std::string custom_src = g_add_custom_assets;
                        if (custom_src.empty()) {
                            g_add_status = "Please specify the custom assets folder.";
                        } else {
                            assets_dir_name = std::string("inst-") + g_add_name;
                            std::string dest = get_user_data_dir() + "/" + assets_dir_name;
                            try {
                                if (fs::exists(dest)) {
                                    fs::remove_all(dest);
                                }
                                fs::create_directories(dest);
                                fs::copy(custom_src, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                                std::cout << "[Launcher] Copied assets: " << custom_src << " -> " << dest << std::endl;
                            } catch (const std::exception& e) {
                                g_add_status = std::string("Error copying assets: ") + e.what();
                            }
                        }
                    }

                    if (g_add_status.empty()) {
                        std::string user_data = get_user_data_dir();

                        // Base engine: prefer the default binary, then the
                        // newest ARM64, then the newest ARM32. Never hardcode.
                        std::string base_so;
                        std::string base_arch_dir;
                        const std::string default_filepath = selector.get_default();
                        const auto& available = selector.get_binaries();
                        const BinaryInfo* source = nullptr;
                        if (!default_filepath.empty()) {
                            for (const auto& b : available) {
                                if (b.filepath == default_filepath) { source = &b; break; }
                            }
                        }
                        if (!source) {
                            for (const auto& b : available) {
                                if (b.arch == BinaryArch::ARM64) {
                                    if (!source || b.version > source->version) source = &b;
                                }
                            }
                        }
                        if (!source) {
                            for (const auto& b : available) {
                                if (!source || b.version > source->version) source = &b;
                            }
                        }
                        if (!source) {
                            g_add_status = "No engine installed. Import an APK first.";
                        } else {
                            base_so = (fs::path(user_data) / source->filepath).string();
                            base_arch_dir = source->arch == BinaryArch::ARM64 ? "arm64-v8a" : "armeabi-v7a";

                            // Set data_dir on selector before adding custom instance
                            selector.set_data_dir(user_data);

                            if (selector.add_custom_instance(base_so, g_add_name, assets_dir_name)) {
                                if (g_add_use_sre) {
                                    std::string sre_src = (fs::path(user_data) / "engine" / source->version_dir / base_arch_dir / "libsre.so").string();
                                    std::string arch_dir = user_data + "/engine/custom-" + std::string(g_add_name) + "/" + base_arch_dir;
                                    if (fs::exists(sre_src)) {
                                        try {
                                            fs::copy_file(sre_src, arch_dir + "/libsre.so", fs::copy_options::overwrite_existing);
                                        } catch (...) {}
                                    }

                                    selector.strip_sre_conflicts_from_last();
                                }

                            std::string config_dir;
                            const char* xdg_config = getenv("XDG_CONFIG_HOME");
                            if (xdg_config) {
                                config_dir = std::string(xdg_config) + "/swordigo-desktop";
                            } else {
                                const char* home = getenv("HOME");
                                config_dir = std::string(home ? home : ".") + "/.config/swordigo-desktop";
                            }
                            selector.save_user_instances(config_dir + "/instances.json");

                            g_show_add_instance = false;
                            ImGui::CloseCurrentPopup();
                            } else {
                                g_add_status = "Failed to create instance.";
                            }
                        }
                    }
                }
            }
            ImGui::PopStyleColor(2);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
            ImGui::BeginDisabled(g_add_copying);
            if (ImGui::Button("Cancel", ImVec2(btn_w, 36))) {
                g_show_add_instance = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);

            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        // CPU backend unavailable modal (Unicorn selected but not present)
        if (g_show_backend_warning) {
            ImGui::OpenPopup("Backend Unavailable");
        }
        ImVec2 warn_center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(warn_center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Backend Unavailable", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.45f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", g_backend_warning.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 32))) {
                g_show_backend_warning = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Render
        ImGui::Render();
        int fb_w, fb_h;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.051f, 0.067f, 0.090f, 1.0f); // Match WindowBg #0d1117
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    // ── Cleanup ──
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(imgui_ctx);

    SDL_GL_DestroyContext(gl_context);
    g_sdl_window = nullptr;
    SDL_DestroyWindow(window);
    SDL_Quit();

    // Ensure we return a valid config if launching
    if (cfg.should_launch) {
        const auto& final_bins = selector.get_binaries();
        if (cfg.selected_binary.empty() && !final_bins.empty() && bin_sel >= 0) {
            cfg.selected_binary = final_bins[bin_sel].filepath;
            cfg.assets_dir = final_bins[bin_sel].assets_dir;
            cfg.game_type = final_bins[bin_sel].game_type;
        }
        // Pass the top-priority mod from the config load order to VFS pre-init.
        // Always load fresh from disk to reflect any UI changes (enable/disable/reorder).
        {
            LauncherConfig fresh_cfg = launcher_config_load();
            if (!fresh_cfg.mod_load_order.empty()) {
                cfg.selected_mod = fresh_cfg.mod_load_order[0];
                std::cout << "[Launcher] Primary VFS mod: " << cfg.selected_mod
                          << " (" << fresh_cfg.mod_load_order.size() << " mod(s) in load order)" << std::endl;
            } else {
                std::cout << "[Launcher] No mods in load order — vanilla mode" << std::endl;
            }
        }
    }

    return cfg;
}
