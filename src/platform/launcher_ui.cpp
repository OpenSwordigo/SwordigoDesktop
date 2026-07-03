// =============================================================================
// Swordigo Desktop — ImGui Launcher UI  (v7.1 Remaster)
// Full Dear ImGui replacement for the old raw-OpenGL launcher.
// SDL3 + OpenGL 3.3 Core + ImGui v1.91.x + Font Awesome 7
// =============================================================================

#include "platform/launcher_ui.h"
#include "platform/data_path.h"
#include "platform/save_editor.h"
#include "platform/IconsFontAwesome6.h"

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
#include <unistd.h>
#include <cmath>

namespace fs = std::filesystem;

// =============================================================================
// Forward declarations (internal helpers)
// =============================================================================

static void      ApplyCustomTheme();
static GLuint    LoadTextureFromFile(const char* path, int* out_w, int* out_h);
static void      DrawToolbar(bool& running, LaunchConfig& cfg, bool& show_options);
static void      DrawInstancePanel(BinarySelector& selector, int& selected, float width);
static void      DrawDetailPanel(BinarySelector& selector, int selected,
                                 LaunchConfig& cfg, bool& running, int& api_sel,
                                 int& engine_sel, bool& use_sre_sel, bool& show_save_editor, float mods_width);
static void      DrawModsPanel(float width);
static void      DrawStatusBar(int selected, const BinarySelector& selector);
static void      DrawOptionsModal(bool& show_options);
static void      DrawSaveEditor(bool& show_save_editor);

// =============================================================================
// Mod info (parsed from mod.json)
// =============================================================================

struct ModInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string type;      // "music", "scene", "texture"
    std::string dir_path;
    bool        enabled = true;
};

// Loaded UI textures
static GLuint g_tex_bg = 0;
static int    g_tex_bg_w = 0, g_tex_bg_h = 0;
static GLuint g_tex_logo = 0;
static int    g_tex_logo_w = 0, g_tex_logo_h = 0;
static GLuint g_tex_icon_swordigo = 0;
static GLuint g_tex_icon_swmini = 0;
static GLuint g_tex_icon_rlswordigo = 0;
static GLuint g_tex_icon_app = 0;
static int    g_icon_w = 0, g_icon_h = 0; // reusable

// Per-instance custom icon cache (keyed by icon_path)
static std::map<std::string, GLuint> g_custom_icon_cache;

// SDL window pointer (for borderless drag)
static SDL_Window* g_sdl_window = nullptr;

// Toolbar height constant
static const float TOOLBAR_H = 64.0f;

// =============================================================================
// Module-level state (lives for the duration of show_launcher)
// =============================================================================

static ImFont* g_font_main    = nullptr;
static ImFont* g_font_heading = nullptr;

static std::vector<ModInfo> g_mods;
static bool g_mods_scanned = false;

// Save editor state
static bool                     g_save_loaded      = false;
static std::vector<std::string> g_save_paths;
static std::vector<SaveFile>    g_save_files;
static int                      g_save_sel         = -1;
static SaveFile                 g_edit_save;
static std::string              g_save_status;
static bool                     g_save_status_ok   = false;

// Delete confirmation
static bool g_confirm_delete    = false;
static int  g_delete_target_idx = -1;

// Add Instance popup state
static bool g_show_add_instance = false;
static char g_add_name[128] = "";
static int  g_add_asset_type = 0;        // 0=vanilla, 1=RL assets, 2=custom folder
static char g_add_custom_assets[512] = "";
static bool g_add_use_sre = true;
static char g_add_game_type[64] = "Swordigo";
static std::string g_add_status;
static bool g_add_copying = false;       // true during async asset copy
static float g_add_copy_progress = 0.0f; // 0.0-1.0 copy progress

// Animation state
static float g_anim_time = 0.0f;

// =============================================================================
// Texture helper
// =============================================================================

static GLuint LoadTextureFromFile(const char* path, int* out_w, int* out_h) {
    SDL_Surface* surf = IMG_Load(path);
    if (!surf) return 0;

    // Convert to RGBA32
    SDL_Surface* rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surf);
    if (!rgba) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);

    if (out_w) *out_w = rgba->w;
    if (out_h) *out_h = rgba->h;

    SDL_DestroySurface(rgba);
    return tex;
}

// =============================================================================
// Theme — Modern dark premium (inspired by JetBrains/GitHub Dark/Discord)
// =============================================================================

static void ApplyCustomTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Rounding — generous for modern feel
    style.WindowRounding    = 0.0f;   // Full-window panels, no outer rounding
    style.ChildRounding     = 10.0f;
    style.FrameRounding     = 8.0f;
    style.GrabRounding      = 8.0f;
    style.PopupRounding     = 12.0f;
    style.ScrollbarRounding = 12.0f;
    style.TabRounding       = 8.0f;

    // Sizing — comfortable padding
    style.FramePadding      = ImVec2(12, 7);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(8, 6);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 12.0f;
    style.IndentSpacing     = 20.0f;

    // Borders — minimal, refined
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    // Anti-aliasing
    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;

    // ─── Color palette ───
    // Base:    #0d1117 (deep space black)
    // Surface: #161b22 (elevated surface)
    // Frame:   #1c2333 (input fields)
    // Border:  #30363d (subtle dividers)
    // Text:    #e6edf3 (bright, readable)
    // Muted:   #8b949e (secondary text)
    // Accent:  #e94560 (SRT red / coral)
    // Accent2: #58a6ff (info blue)
    // Success: #3fb950 (green)
    // Warning: #d29922 (amber)
    ImVec4* c = style.Colors;

    // Backgrounds
    c[ImGuiCol_WindowBg]          = ImVec4(0.051f, 0.067f, 0.090f, 1.00f); // #0d1117
    c[ImGuiCol_ChildBg]           = ImVec4(0.086f, 0.106f, 0.133f, 1.00f); // #161b22
    c[ImGuiCol_PopupBg]           = ImVec4(0.098f, 0.122f, 0.157f, 0.98f); // #1a1f28
    c[ImGuiCol_Border]            = ImVec4(0.188f, 0.212f, 0.239f, 0.50f); // #30363d
    c[ImGuiCol_BorderShadow]      = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);

    // Frames (inputs, combos, sliders)
    c[ImGuiCol_FrameBg]           = ImVec4(0.110f, 0.137f, 0.200f, 1.00f); // #1c2333
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.140f, 0.170f, 0.240f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.170f, 0.200f, 0.290f, 1.00f);

    // Title bar
    c[ImGuiCol_TitleBg]           = ImVec4(0.051f, 0.067f, 0.090f, 1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.071f, 0.087f, 0.110f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.051f, 0.067f, 0.090f, 0.50f);

    c[ImGuiCol_MenuBarBg]         = ImVec4(0.086f, 0.106f, 0.133f, 1.00f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.051f, 0.067f, 0.090f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.188f, 0.212f, 0.239f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.250f, 0.280f, 0.320f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.340f, 0.370f, 0.410f, 1.00f);

    // Accent widgets
    c[ImGuiCol_CheckMark]         = ImVec4(0.914f, 0.271f, 0.376f, 1.00f); // #e94560
    c[ImGuiCol_SliderGrab]        = ImVec4(0.345f, 0.651f, 1.000f, 0.80f); // #58a6ff
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.345f, 0.651f, 1.000f, 1.00f);

    // Buttons — SRT red accent
    c[ImGuiCol_Button]            = ImVec4(0.914f, 0.271f, 0.376f, 1.00f); // #e94560
    c[ImGuiCol_ButtonHovered]     = ImVec4(1.000f, 0.380f, 0.478f, 1.00f); // lighter
    c[ImGuiCol_ButtonActive]      = ImVec4(0.780f, 0.200f, 0.290f, 1.00f); // darker

    // Headers (selectables, collapsing headers)
    c[ImGuiCol_Header]            = ImVec4(0.110f, 0.137f, 0.200f, 1.00f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.914f, 0.271f, 0.376f, 0.30f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.914f, 0.271f, 0.376f, 0.50f);

    // Separators
    c[ImGuiCol_Separator]         = ImVec4(0.188f, 0.212f, 0.239f, 0.40f);
    c[ImGuiCol_SeparatorHovered]  = ImVec4(0.914f, 0.271f, 0.376f, 0.50f);
    c[ImGuiCol_SeparatorActive]   = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);

    // Resize grip
    c[ImGuiCol_ResizeGrip]        = ImVec4(0.914f, 0.271f, 0.376f, 0.15f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.914f, 0.271f, 0.376f, 0.40f);
    c[ImGuiCol_ResizeGripActive]  = ImVec4(0.914f, 0.271f, 0.376f, 0.85f);

    // Tabs
    c[ImGuiCol_Tab]               = ImVec4(0.110f, 0.137f, 0.200f, 1.00f);
    c[ImGuiCol_TabHovered]        = ImVec4(0.914f, 0.271f, 0.376f, 0.50f);
    c[ImGuiCol_TabSelected]       = ImVec4(0.914f, 0.271f, 0.376f, 0.80f);

    // Text
    c[ImGuiCol_Text]              = ImVec4(0.902f, 0.929f, 0.953f, 1.00f); // #e6edf3
    c[ImGuiCol_TextDisabled]      = ImVec4(0.545f, 0.580f, 0.620f, 1.00f); // #8b949e

    // Tables
    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.110f, 0.137f, 0.200f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.188f, 0.212f, 0.239f, 0.60f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.188f, 0.212f, 0.239f, 0.30f);
    c[ImGuiCol_TableRowBg]        = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(1.000f, 1.000f, 1.000f, 0.02f);
}

// =============================================================================
// Mod scanner
// =============================================================================

static std::string ReadFileToString(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Minimal JSON value extractor (no external dependency)
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

static void ScanMods() {
    g_mods.clear();
    std::string mods_dir = get_user_data_dir() + "/mods";
    if (!fs::exists(mods_dir) || !fs::is_directory(mods_dir)) {
        g_mods_scanned = true;
        return;
    }

    for (auto& entry : fs::directory_iterator(mods_dir)) {
        if (!entry.is_directory()) continue;
        
        std::string dirname = entry.path().filename().string();
        
        // Check if this is a disabled mod (dot-prefixed)
        bool is_disabled = (!dirname.empty() && dirname[0] == '.');
        
        std::string mod_json_path = entry.path().string() + "/mod.json";
        if (!fs::exists(mod_json_path)) continue;

        std::string json = ReadFileToString(mod_json_path);
        if (json.empty()) continue;

        ModInfo mod;
        mod.id          = JsonGetString(json, "id");
        mod.name        = JsonGetString(json, "name");
        mod.version     = JsonGetString(json, "version");
        mod.author      = JsonGetString(json, "author");
        mod.description = JsonGetString(json, "description");
        mod.type        = JsonGetString(json, "type");
        mod.dir_path    = entry.path().string();
        mod.enabled     = !is_disabled;

        if (mod.name.empty()) {
            // Strip dot prefix for display name
            mod.name = is_disabled ? dirname.substr(1) : dirname;
        }
        g_mods.push_back(mod);
    }

    g_mods_scanned = true;
}

// =============================================================================
// Human-readable file size
// =============================================================================

static std::string FormatFileSize(size_t bytes) {
    if (bytes < 1024)               return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)        return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// =============================================================================
// Toolbar — sleek top bar with logo image + control buttons + window drag
// =============================================================================

static void DrawToolbar(bool& running, LaunchConfig& cfg, bool& show_options) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, TOOLBAR_H));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.043f, 0.055f, 0.075f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.188f, 0.212f, 0.239f, 0.3f));
    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Window drag (borderless mode) ──
    // If mouse is in toolbar area and not over a button, allow dragging
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (g_sdl_window) {
            float dx = ImGui::GetIO().MouseDelta.x;
            float dy = ImGui::GetIO().MouseDelta.y;
            if (dx != 0.0f || dy != 0.0f) {
                int wx, wy;
                SDL_GetWindowPosition(g_sdl_window, &wx, &wy);
                SDL_SetWindowPosition(g_sdl_window, wx + (int)dx, wy + (int)dy);
            }
        }
    }

    // ── Logo image (SWORDIGO / DESKTOP text) ──
    if (g_tex_logo) {
        // Fit within toolbar with padding: 52px tall max, 15:4 aspect
        float logo_h = TOOLBAR_H - 12.0f; // 52px
        float logo_w = logo_h * ((float)g_tex_logo_w / (float)g_tex_logo_h);
        ImGui::SetCursorPos(ImVec2(12, (TOOLBAR_H - logo_h) * 0.5f));
        ImGui::Image((ImTextureID)(intptr_t)g_tex_logo, ImVec2(logo_w, logo_h));
    } else {
        // Fallback: text title if logo not found
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::SetCursorPos(ImVec2(12, (TOOLBAR_H - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(ImVec4(0.902f, 0.929f, 0.953f, 1.0f), "SWORDIGO DESKTOP");
        if (g_font_heading) ImGui::PopFont();
    }

    // ── Right-aligned buttons ──
    float rhs = ImGui::GetWindowWidth();
    float btn_y = (TOOLBAR_H - 38) * 0.5f;

    // Close button (red)
    ImGui::SetCursorPos(ImVec2(rhs - 50, btn_y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.10f, 0.10f, 1.0f));
    if (ImGui::Button(ICON_FA_XMARK "##close", ImVec2(38, 38))) {
        cfg.should_launch = false;
        running = false;
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (ESC)");

    // Settings button (subtle)
    ImGui::SetCursorPos(ImVec2(rhs - 98, btn_y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.140f, 0.170f, 0.250f, 1.0f));
    if (ImGui::Button(ICON_FA_GEAR "##opts", ImVec2(38, 38))) {
        show_options = true;
        ImGui::OpenPopup("Options");
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Options");

    // ── Subtle bottom border line ──
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(
        ImVec2(wp.x, wp.y + TOOLBAR_H - 1),
        ImVec2(wp.x + vp->WorkSize.x, wp.y + TOOLBAR_H - 1),
        IM_COL32(48, 54, 61, 180), 1.0f);

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// =============================================================================
// Icon helper — maps game_type to a loaded icon texture
// =============================================================================

static GLuint GetIconForInstance(const BinaryInfo& b) {
    // Priority 1: custom icon_path from manifest (lazy load + cache)
    if (!b.icon_path.empty()) {
        auto it = g_custom_icon_cache.find(b.icon_path);
        if (it != g_custom_icon_cache.end()) {
            if (it->second) return it->second;
            // Cached as 0 = failed to load, fall through to defaults
        } else {
            // Try loading from multiple search paths
            int w = 0, h = 0;
            GLuint tex = 0;
            std::string paths[] = {
                b.icon_path,  // as-is (absolute or relative)
                get_user_data_dir() + "/launcher/icons/" + b.icon_path,
                get_user_data_dir() + "/launcher/" + b.icon_path,
                std::string("src/assets/icons/") + b.icon_path,
                std::string("src/assets/") + b.icon_path,
            };
            for (auto& p : paths) {
                tex = LoadTextureFromFile(p.c_str(), &w, &h);
                if (tex) {
                    std::cout << "[Launcher] Custom icon loaded: " << p << std::endl;
                    break;
                }
            }
            g_custom_icon_cache[b.icon_path] = tex;  // cache (0 if failed)
            if (tex) return tex;
        }
    }
    // Priority 2: game_type defaults
    if (b.game_type == "RLSwordigo" && g_tex_icon_rlswordigo) return g_tex_icon_rlswordigo;
    if (b.game_type == "SwordigoMini" && g_tex_icon_swmini) return g_tex_icon_swmini;
    if (g_tex_icon_swordigo) return g_tex_icon_swordigo;
    return g_tex_icon_app;
}

// =============================================================================
// Display name helper — extracts a clean, user-friendly name for sidebar
// =============================================================================

static std::string GetDisplayName(const BinaryInfo& b) {
    // For known game types, use a clean name + version
    if (b.game_type == "Swordigo")     return "Swordigo " + b.version;
    if (b.game_type == "RLSwordigo")   return "RLSwordigo " + b.version;
    if (b.game_type == "SwordigoMini") return "Swordigo Mini " + b.version;

    // For custom instances, clean the label by stripping technical tags
    std::string name = b.label;
    // Remove [ARM64], [ARM32], (Tested), (Testing), (Unknown), (SRE), [Custom], [RL], etc.
    auto strip = [&name](const std::string& tag) {
        size_t pos;
        while ((pos = name.find(tag)) != std::string::npos) {
            name.erase(pos, tag.size());
        }
    };
    strip("[ARM64]"); strip("[ARM32]");
    strip("(Tested)"); strip("(Testing)"); strip("(Unknown)"); strip("(SRE)");
    strip("[Custom]"); strip("[RL]"); strip("[Swordigo]");
    // Trim whitespace
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    // Collapse multiple spaces
    std::string clean;
    bool prev_space = false;
    for (char c : name) {
        if (c == ' ') {
            if (!prev_space) clean += c;
            prev_space = true;
        } else {
            clean += c;
            prev_space = false;
        }
    }
    return clean.empty() ? b.label : clean;
}

static std::string GetSubtitle(const BinaryInfo& b) {
    std::string sub;
    sub += BinarySelector::arch_string(b.arch);
    sub += "  ·  v" + b.version;
    switch (b.status) {
        case BinaryStatus::TESTED:  sub += "  ·  Stable"; break;
        case BinaryStatus::TESTING: sub += "  ·  Testing"; break;
        default: break;
    }
    if (b.assets_dir == "rl_assets") sub += "  ·  RL";
    else if (b.assets_dir != "assets") sub += "  ·  Custom";
    return sub;
}

// =============================================================================
// Instance list panel (left sidebar)
// =============================================================================

static void DrawInstancePanel(BinarySelector& selector, int& selected, float width) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_pos(vp->WorkPos.x, vp->WorkPos.y + TOOLBAR_H);
    ImVec2 panel_size(width, vp->WorkSize.y - TOOLBAR_H - 30);

    ImGui::SetNextWindowPos(panel_pos);
    ImGui::SetNextWindowSize(panel_size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.059f, 0.075f, 0.098f, 1.0f));
    ImGui::Begin("##Instances", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Header row
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.580f, 0.620f, 1.0f));
    ImGui::Text(ICON_FA_LAYER_GROUP "  INSTANCES");
    ImGui::PopStyleColor();

    ImGui::SameLine(width - 50);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.914f, 0.271f, 0.376f, 0.6f));
    if (ImGui::Button(ICON_FA_PLUS "##add", ImVec2(30, 30))) {
        g_show_add_instance = true;
        // Reset form fields
        memset(g_add_name, 0, sizeof(g_add_name));
        strncpy(g_add_name, "My Instance", sizeof(g_add_name) - 1);
        memset(g_add_custom_assets, 0, sizeof(g_add_custom_assets));
        g_add_asset_type = 0;
        g_add_use_sre = true;
        strncpy(g_add_game_type, "Swordigo", sizeof(g_add_game_type) - 1);
        g_add_status.clear();
        g_add_copying = false;
        g_add_copy_progress = 0.0f;
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add new game instance");

    ImGui::Spacing();
    // Subtle separator line
    ImDrawList* sep_dl = ImGui::GetWindowDrawList();
    ImVec2 sep_p = ImGui::GetCursorScreenPos();
    sep_dl->AddLine(ImVec2(sep_p.x, sep_p.y), ImVec2(sep_p.x + width - 20, sep_p.y),
                    IM_COL32(48, 54, 61, 120), 1.0f);
    ImGui::Spacing();

    // Instance list
    const auto& bins = selector.get_binaries();
    if (bins.empty()) {
        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextDisabled("No instances found.");
        ImGui::TextDisabled("Click '+' to add one.");
    }

    // Build sorted index: group by version first, with ARM64 before ARM32 within each version
    std::vector<int> sorted_idx(bins.size());
    std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
    std::sort(sorted_idx.begin(), sorted_idx.end(), [&bins](int a, int b_idx) {
        const auto& ba = bins[a];
        const auto& bb = bins[b_idx];
        if (ba.version != bb.version) {
            return ba.version > bb.version;
        }
        if (ba.arch != bb.arch) {
            return ba.arch == BinaryArch::ARM64;
        }
        return false;
    });

    // Initial selection: prefer ARM64 v1.4.12
    static bool first_frame = true;
    if (first_frame && !bins.empty()) {
        first_frame = false;
        for (int i = 0; i < (int)bins.size(); i++) {
            if (bins[i].arch == BinaryArch::ARM64 && bins[i].version.find("1.4.12") != std::string::npos) {
                selected = i;
                break;
            }
        }
    }

    for (int idx : sorted_idx) {
        const auto& b = bins[idx];
        ImGui::PushID(idx);

        bool is_selected = (selected == idx);

        // Hover highlight style
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.914f, 0.271f, 0.376f, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.914f, 0.271f, 0.376f, 0.25f));
        }

        if (ImGui::Selectable("##instance", is_selected, 0, ImVec2(0, 60))) {
            selected = idx;
        }

        if (is_selected) {
            ImGui::PopStyleColor(2);
        }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem("InstanceCtx")) {
            if (ImGui::MenuItem(ICON_FA_STAR "  Set Default")) {
                selector.set_default(b.filepath);
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Folder")) {
                std::string full = b.filepath[0] == '/' ? b.filepath : (get_user_data_dir() + "/" + b.filepath);
                std::string dir = fs::path(full).parent_path().string();
                pid_t pid = fork();
                if (pid == 0) {
                    execlp("xdg-open", "xdg-open", dir.c_str(), nullptr);
                    _exit(1);
                }
            }
            ImGui::Separator();
            // Only show Remove for non-vanilla (custom) instances
            bool is_vanilla_ctx = (b.game_type == "Swordigo" || b.game_type == "RLSwordigo" || b.game_type == "SwordigoMini");
            if (!is_vanilla_ctx) {
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove")) {
                    g_confirm_delete = true;
                    g_delete_target_idx = idx;
                }
            }
            ImGui::EndPopup();
        }

        // Overlay content on the selectable
        ImVec2 item_min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Selected indicator bar (left edge accent)
        if (is_selected) {
            dl->AddRectFilled(
                ImVec2(item_min.x, item_min.y + 4),
                ImVec2(item_min.x + 3, item_min.y + 56),
                IM_COL32(233, 69, 96, 255), 2.0f);
        }

        // Instance icon (40x40) with rounded corners
        float icon_x = item_min.x + 10;
        float icon_y = item_min.y + 10;
        GLuint icon = GetIconForInstance(b);
        if (icon) {
            dl->AddImageRounded(
                (ImTextureID)(intptr_t)icon,
                ImVec2(icon_x, icon_y),
                ImVec2(icon_x + 40, icon_y + 40),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, 255), 8.0f);
        }

        // Text content area (right of icon)
        float text_x = icon_x + 48;

        // Line 1: Clean display name (prominent)
        std::string display_name = GetDisplayName(b);
        dl->AddText(ImVec2(text_x, item_min.y + 8),
            IM_COL32(230, 237, 243, 255), display_name.c_str());

        // Line 2: Subtitle (arch · version · status — muted)
        std::string subtitle = GetSubtitle(b);
        dl->AddText(ImVec2(text_x, item_min.y + 28),
            IM_COL32(139, 148, 158, 200), subtitle.c_str());

        // Status dot (top-right area)
        ImVec4 dot_color;
        switch (b.status) {
            case BinaryStatus::TESTED:  dot_color = ImVec4(0.247f, 0.725f, 0.314f, 1.0f); break;
            case BinaryStatus::TESTING: dot_color = ImVec4(0.824f, 0.600f, 0.133f, 1.0f); break;
            default:                    dot_color = ImVec4(0.600f, 0.300f, 0.300f, 1.0f); break;
        }
        dl->AddCircleFilled(
            ImVec2(item_min.x + width - 28, item_min.y + 14),
            4.0f, ImGui::ColorConvertFloat4ToU32(dot_color));

        // Default star indicator (top-right)
        if (b.is_default) {
            dl->AddText(ImVec2(item_min.x + width - 48, item_min.y + 8),
                IM_COL32(255, 215, 0, 255), ICON_FA_STAR);
        }

        ImGui::PopID();
    }

    // Delete confirmation popup
    if (g_confirm_delete) {
        ImGui::OpenPopup("Confirm Delete");
        g_confirm_delete = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  Remove this instance?");
        ImGui::Separator();
        if (g_delete_target_idx >= 0 && g_delete_target_idx < (int)bins.size()) {
            ImGui::Text("  %s", bins[g_delete_target_idx].label.c_str());
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button(ICON_FA_TRASH "  Remove", ImVec2(120, 0))) {
            selector.remove_instance(g_delete_target_idx);
            if (selected >= (int)selector.get_binaries().size()) {
                selected = std::max(0, (int)selector.get_binaries().size() - 1);
            }
            g_delete_target_idx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            g_delete_target_idx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// =============================================================================
// Detail panel (center)
// =============================================================================

static void DrawDetailPanel(BinarySelector& selector, int selected,
                            LaunchConfig& cfg, bool& running, int& api_sel,
                            int& engine_sel, bool& use_sre_sel, bool& show_save_editor, float mods_width) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float inst_width = 260.0f;
    ImVec2 panel_pos(vp->WorkPos.x + inst_width, vp->WorkPos.y + TOOLBAR_H);
    ImVec2 panel_size(vp->WorkSize.x - inst_width - mods_width, vp->WorkSize.y - TOOLBAR_H - 30);

    ImGui::SetNextWindowPos(panel_pos);
    ImGui::SetNextWindowSize(panel_size);
    ImGui::Begin("##Details", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // -- Save Editor sub-view --
    if (show_save_editor) {
        DrawSaveEditor(show_save_editor);
        ImGui::End();
        return;
    }

    const auto& bins = selector.get_binaries();
    if (bins.empty()) {
        ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
        float avail = ImGui::GetContentRegionAvail().x;
        float txt_w = ImGui::CalcTextSize("No instance selected.").x;
        ImGui::SetCursorPosX((avail - txt_w) * 0.5f);
        ImGui::TextDisabled("No instance selected.");
        txt_w = ImGui::CalcTextSize("Add a game binary to get started.").x;
        ImGui::SetCursorPosX((avail - txt_w) * 0.5f);
        ImGui::TextDisabled("Add a game binary to get started.");
        ImGui::End();
        return;
    }

    if (selected < 0 || selected >= (int)bins.size()) {
        ImGui::End();
        return;
    }

    const BinaryInfo& b = bins[selected];

    // === Instance icon + name (heading) ===
    {
        GLuint detail_icon = GetIconForInstance(b);
        if (detail_icon) {
            ImGui::Image((ImTextureID)(intptr_t)detail_icon, ImVec2(72, 72));
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::Text("%s", GetDisplayName(b).c_str());
        if (g_font_heading) ImGui::PopFont();

        // Arch badge inline
        {
            ImVec4 badge_col = (b.arch == BinaryArch::ARM64)
                ? ImVec4(0.20f, 0.40f, 0.85f, 1.0f) : ImVec4(0.85f, 0.55f, 0.15f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, badge_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, badge_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, badge_col);
            ImGui::SmallButton(BinarySelector::arch_string(b.arch));
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            const char* status_str;
            switch (b.status) {
                case BinaryStatus::TESTED:  status_str = "Stable"; break;
                case BinaryStatus::TESTING: status_str = "Testing"; break;
                default:                    status_str = "Unknown"; break;
            }
            ImGui::TextDisabled("(%s)", status_str);
        }
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // === LAUNCH button — prominent, full-width ===
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.914f, 0.271f, 0.376f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(1.000f, 0.380f, 0.478f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.780f, 0.200f, 0.290f, 1.0f));
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    float launch_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(ICON_FA_ROCKET "  LAUNCH", ImVec2(launch_w, 52))) {
        cfg.graphics_api = (api_sel == 0) ? GraphicsAPI::OPENGL : GraphicsAPI::VULKAN;
        cfg.use_dynarmic = (engine_sel == 1);
        cfg.use_sre = use_sre_sel;
        cfg.selected_binary = b.filepath;
        cfg.assets_dir = b.assets_dir;
        cfg.game_type = b.game_type;
        cfg.should_launch = true;
        running = false;
    }
    if (g_font_heading) ImGui::PopFont();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();

    // === Engine selection row (compact) ===
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.580f, 0.620f, 1.0f));
        ImGui::Text(ICON_FA_MICROCHIP "  CPU Engine");
        ImGui::PopStyleColor();
        ImGui::SameLine(180);
        ImGui::RadioButton("Unicorn (TCG)", &engine_sel, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Dynarmic (JIT)", &engine_sel, 1);
    }

    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.580f, 0.620f, 1.0f));
        ImGui::Text(ICON_FA_PAINT_BRUSH "  Graphics API");
        ImGui::PopStyleColor();
        ImGui::SameLine(180);
        ImGui::RadioButton("OpenGL", &api_sel, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Vulkan", &api_sel, 1);
    }

    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.580f, 0.620f, 1.0f));
        ImGui::Text(ICON_FA_CODE "  SRE Hooks");
        ImGui::PopStyleColor();
        ImGui::SameLine(180);
        ImGui::Checkbox("Enable SRE (libsre.so)", &use_sre_sel);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // === Detail table ===
    if (ImGui::BeginTable("DetailsTable", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Value",    ImGuiTableColumnFlags_WidthStretch);

        auto Row = [](const char* prop, const char* val) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", prop);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(val);
        };

        Row("Version",   b.version.c_str());
        Row("Arch",      BinarySelector::arch_string(b.arch));
        Row("Game Type", b.game_type.c_str());
        Row("Assets",    b.assets_dir.c_str());
        Row("Path",      b.filepath.c_str());
        Row("Size",      FormatFileSize(b.file_size).c_str());
        Row("SHA256",    b.sha256.empty() ? "(not computed)" : b.sha256.substr(0, 16).c_str());

        const char* status_str;
        switch (b.status) {
            case BinaryStatus::TESTED:  status_str = "Tested (Stable)"; break;
            case BinaryStatus::TESTING: status_str = "Testing"; break;
            default:                    status_str = "Unknown"; break;
        }
        Row("Status", status_str);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // === Action buttons row ===
    float btn_w = (ImGui::GetContentRegionAvail().x - 30) / 4.0f;

    // Secondary button style (dark blue)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.320f, 1.0f));

    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save Editor", ImVec2(btn_w, 36))) {
        show_save_editor = true;
        // Load saves
        g_save_loaded = false;
        g_save_sel = -1;
        g_save_status.clear();
        std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
        std::string xdg = getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME") : (home + "/.local/share");
        std::string save_dir = xdg + "/swordigo-desktop/save/Documents";
        g_save_paths = save_list_dir(save_dir);
        g_save_files.clear();
        for (auto& p : g_save_paths) {
            SaveFile sf;
            if (save_load(p, sf)) {
                g_save_files.push_back(sf);
            }
        }
        g_save_loaded = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit save files (.gplayer)");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(btn_w, 36))) {
        std::string full = b.filepath[0] == '/' ? b.filepath : (get_user_data_dir() + "/" + b.filepath);
        std::string dir = fs::path(full).parent_path().string();
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xdg-open", "xdg-open", dir.c_str(), nullptr);
            _exit(1);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open instance folder in file manager");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_EYE "  Asset Viewer", ImVec2(btn_w, 36))) {
        pid_t pid = fork();
        if (pid == 0) {
            // Try local build first, then installed path
            execlp("./asset_viewer", "asset_viewer", nullptr);
            execlp("asset_viewer", "asset_viewer", nullptr);
            _exit(1);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse game assets (textures, scenes)");

    // Only show Remove for non-vanilla (custom) instances
    {
        bool is_vanilla = (b.game_type == "Swordigo" || b.game_type == "RLSwordigo" || b.game_type == "SwordigoMini");
        if (!is_vanilla) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button(ICON_FA_TRASH "  Remove", ImVec2(btn_w, 36))) {
                g_confirm_delete = true;
                g_delete_target_idx = selected;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this instance");
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::PopStyleColor(2);

    // === Dependencies section ===
    if (!b.dependencies.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.580f, 0.620f, 1.0f));
        ImGui::Text(ICON_FA_PUZZLE_PIECE "  Dependencies");
        ImGui::PopStyleColor();
        for (const auto& dep : b.dependencies) {
            ImGui::BulletText("%s", dep.c_str());
        }
    }

    ImGui::End();
}

// =============================================================================
// Mods panel (right sidebar)
// =============================================================================

static void DrawModsPanel(float width) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_pos(vp->WorkPos.x + vp->WorkSize.x - width, vp->WorkPos.y + TOOLBAR_H);
    ImVec2 panel_size(width, vp->WorkSize.y - TOOLBAR_H - 30);

    ImGui::SetNextWindowPos(panel_pos);
    ImGui::SetNextWindowSize(panel_size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.059f, 0.075f, 0.098f, 1.0f));
    ImGui::Begin("##Mods", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.580f, 0.620f, 1.0f));
    ImGui::Text(ICON_FA_PUZZLE_PIECE "  MODS");
    ImGui::PopStyleColor();

    ImGui::SameLine(width - 120);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
    if (ImGui::Button(ICON_FA_FOLDER " Load Mod", ImVec2(105, 28))) {
        std::string mods_dir = get_user_data_dir() + "/mods";
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xdg-open", "xdg-open", mods_dir.c_str(), nullptr);
            _exit(1);
        }
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open mods folder");

    ImGui::Spacing();
    // Subtle separator
    ImDrawList* sep_dl = ImGui::GetWindowDrawList();
    ImVec2 sep_p = ImGui::GetCursorScreenPos();
    sep_dl->AddLine(ImVec2(sep_p.x, sep_p.y), ImVec2(sep_p.x + width - 20, sep_p.y),
                    IM_COL32(48, 54, 61, 120), 1.0f);
    ImGui::Spacing();

    // Scan mods on first draw
    if (!g_mods_scanned) ScanMods();

    if (g_mods.empty()) {
        ImGui::Spacing(); ImGui::Spacing();
        float avail = ImGui::GetContentRegionAvail().x;
        float txt_w = ImGui::CalcTextSize("No mods installed").x;
        ImGui::SetCursorPosX((avail - txt_w) / 2.0f);
        ImGui::TextDisabled("No mods installed");

        ImGui::Spacing();
        txt_w = ImGui::CalcTextSize("Place mod folders with mod.json").x;
        ImGui::SetCursorPosX((avail - txt_w) / 2.0f);
        ImGui::TextDisabled("Place mod folders with mod.json");

        txt_w = ImGui::CalcTextSize("in ~/.local/share/swordigo-desktop/mods/").x;
        ImGui::SetCursorPosX((avail - txt_w) / 2.0f);
        ImGui::TextDisabled("in ~/.local/share/swordigo-desktop/mods/");
    } else {
        for (int i = 0; i < (int)g_mods.size(); i++) {
            auto& mod = g_mods[i];
            ImGui::PushID(i);

            bool prev_enabled = mod.enabled;
            ImGui::Checkbox("##enabled", &mod.enabled);
            
            // Toggle: rename folder to add/remove dot prefix
            if (mod.enabled != prev_enabled) {
                fs::path old_path(mod.dir_path);
                std::string dirname = old_path.filename().string();
                std::string new_dirname;
                
                if (!mod.enabled) {
                    // Disable: add dot prefix
                    if (dirname[0] != '.') {
                        new_dirname = "." + dirname;
                    }
                } else {
                    // Enable: remove dot prefix
                    if (dirname[0] == '.') {
                        new_dirname = dirname.substr(1);
                    }
                }
                
                if (!new_dirname.empty()) {
                    fs::path new_path = old_path.parent_path() / new_dirname;
                    std::error_code ec;
                    fs::rename(old_path, new_path, ec);
                    if (!ec) {
                        mod.dir_path = new_path.string();
                        std::cout << "[Launcher] Mod " 
                                  << (mod.enabled ? "enabled" : "disabled")
                                  << ": " << mod.name << " -> " << new_dirname 
                                  << std::endl;
                    } else {
                        // Revert checkbox if rename failed
                        mod.enabled = prev_enabled;
                        std::cerr << "[Launcher] Failed to rename mod folder: " 
                                  << ec.message() << std::endl;
                    }
                }
            }
            
            ImGui::SameLine();

            ImGui::BeginGroup();
            if (!mod.enabled) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            }
            ImGui::Text("%s", mod.name.c_str());
            ImGui::TextDisabled("v%s by %s", mod.version.c_str(), mod.author.c_str());
            if (!mod.enabled) {
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();

            if (ImGui::IsItemHovered() && !mod.description.empty()) {
                ImGui::SetTooltip("%s\nType: %s\nStatus: %s", 
                    mod.description.c_str(), mod.type.c_str(),
                    mod.enabled ? "Enabled" : "Disabled");
            }

            ImGui::PopID();
        }
    }

    // Rescan button at bottom
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 42);
    // Subtle separator
    ImDrawList* bot_dl = ImGui::GetWindowDrawList();
    ImVec2 bot_p = ImGui::GetCursorScreenPos();
    bot_dl->AddLine(ImVec2(bot_p.x, bot_p.y), ImVec2(bot_p.x + width - 20, bot_p.y),
                    IM_COL32(48, 54, 61, 120), 1.0f);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS "  Rescan Mods", ImVec2(-1, 28))) {
        g_mods_scanned = false;
    }
    ImGui::PopStyleColor(2);

    ImGui::End();
    ImGui::PopStyleColor();
}

// =============================================================================
// Status bar — minimal bottom strip
// =============================================================================

static void DrawStatusBar(int selected, const BinarySelector& selector) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 bar_pos(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 30);
    ImVec2 bar_size(vp->WorkSize.x, 30);

    ImGui::SetNextWindowPos(bar_pos);
    ImGui::SetNextWindowSize(bar_size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.043f, 0.060f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Left: status
    const auto& bins = selector.get_binaries();
    if (!bins.empty() && selected >= 0 && selected < (int)bins.size()) {
        const char* status_str;
        ImVec4 status_col;
        switch (bins[selected].status) {
            case BinaryStatus::TESTED:  status_str = ICON_FA_CIRCLE_CHECK " Ready"; status_col = ImVec4(0.247f, 0.725f, 0.314f, 0.8f); break;
            case BinaryStatus::TESTING: status_str = ICON_FA_CLOCK " Testing"; status_col = ImVec4(0.824f, 0.600f, 0.133f, 0.8f); break;
            default:                    status_str = "Unknown status"; status_col = ImVec4(0.545f, 0.580f, 0.620f, 0.8f); break;
        }
        ImGui::TextColored(status_col, "%s", status_str);
    } else {
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK " Ready");
    }

    // Right: version + shortcuts
    ImGui::SameLine(ImGui::GetWindowWidth() - 340);
    ImGui::TextDisabled("v7.1  |  Enter: Launch  |  ESC: Close  |  Del: Remove");

    ImGui::End();
    ImGui::PopStyleColor();
}

// =============================================================================
// Options modal
// =============================================================================

static void DrawOptionsModal(bool& show_options) {
    // Center the modal
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
               vp->WorkPos.y + vp->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(650, 500), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Options", &show_options,
            ImGuiWindowFlags_NoResize)) {
        return;
    }

    if (ImGui::BeginTabBar("OptionsTabs")) {
        // --- SRE Hooks tab ---
        if (ImGui::BeginTabItem(ICON_FA_CODE "  SRE Hooks")) {
            ImGui::TextWrapped("All 34 SRE hooks are always active when using libsre.so.");
            ImGui::Spacing();

            if (ImGui::BeginTable("HooksTable", 3,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 350))) {
                ImGui::TableSetupColumn("Hook Name",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Category",   ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Status",     ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();

                struct HookEntry { const char* name; const char* category; };
                static const HookEntry hooks[] = {
                    // CppString (4)
                    {"CppString::assign",            "CppString"},
                    {"CppString::append",            "CppString"},
                    {"CppString::c_str",             "CppString"},
                    {"CppString::destructor",        "CppString"},
                    // Lua (4)
                    {"lua_pcall",                    "Lua"},
                    {"luaL_loadbuffer",              "Lua"},
                    {"lua_newstate",                 "Lua"},
                    {"luaL_openlibs",                "Lua"},
                    // Background (3)
                    {"BackgroundLayer::render",      "Background"},
                    {"BackgroundLayer::update",      "Background"},
                    {"BackgroundLayer::setTexture",  "Background"},
                    // GUI (8)
                    {"GUI::render",                  "GUI"},
                    {"GUI::update",                  "GUI"},
                    {"GUI::handleInput",             "GUI"},
                    {"GUI::showDialog",              "GUI"},
                    {"GUI::hideDialog",              "GUI"},
                    {"GUI::showHUD",                 "GUI"},
                    {"GUI::hideHUD",                 "GUI"},
                    {"GUI::setButtonState",          "GUI"},
                    // Death (1)
                    {"Player::onDeath",              "Death"},
                    // Text Input (4)
                    {"TextInput::show",              "Text Input"},
                    {"TextInput::hide",              "Text Input"},
                    {"TextInput::getText",           "Text Input"},
                    {"TextInput::isActive",          "Text Input"},
                    // Music (7)
                    {"MusicPlayer::play",            "Music"},
                    {"MusicPlayer::stop",            "Music"},
                    {"MusicPlayer::pause",           "Music"},
                    {"MusicPlayer::resume",          "Music"},
                    {"MusicPlayer::setVolume",       "Music"},
                    {"MusicPlayer::isPlaying",       "Music"},
                    {"MusicPlayer::crossfade",       "Music"},
                    // Stats (1)
                    {"Stats::track",                 "Stats"},
                    // Menu (2)
                    {"MainMenu::show",               "Menu"},
                    {"MainMenu::handleSelection",    "Menu"},
                };

                for (const auto& h : hooks) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(h.name);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(h.category);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.247f, 0.725f, 0.314f, 1.0f), ICON_FA_CIRCLE_CHECK " Active");
                }

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Graphics tab ---
        if (ImGui::BeginTabItem(ICON_FA_PAINT_BRUSH "  Graphics")) {
            ImGui::Spacing();
            static bool postfx_enabled = true;
            ImGui::Checkbox("Enable PostFX (bloom, color grading)", &postfx_enabled);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text(ICON_FA_EYE "  Display Information:");
            int display_count = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
            if (displays && display_count > 0) {
                const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displays[0]);
                if (mode) {
                    ImGui::BulletText("Resolution: %dx%d", mode->w, mode->h);
                    ImGui::BulletText("Refresh Rate: %.1f Hz", mode->refresh_rate);
                }
                SDL_free(displays);
            } else {
                ImGui::TextDisabled("Could not query display info.");
            }

            ImGui::Spacing();
            ImGui::Text(ICON_FA_MICROCHIP "  OpenGL Info:");
            ImGui::BulletText("Renderer: %s", (const char*)glGetString(GL_RENDERER));
            ImGui::BulletText("Version:  %s", (const char*)glGetString(GL_VERSION));

            ImGui::EndTabItem();
        }

        // --- About tab ---
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO "  About")) {
            ImGui::Spacing();
            if (g_font_heading) ImGui::PushFont(g_font_heading);
            ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f),
                ICON_FA_GAMEPAD " Swordigo Desktop v7.1");
            if (g_font_heading) ImGui::PopFont();

            ImGui::Spacing();
            ImGui::TextWrapped(
                "A desktop runtime for Swordigo, using ARM binary translation "
                "(Unicorn/Dynarmic) with custom SRE hooks for full playability.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text(ICON_FA_STAR "  Credits:");
            ImGui::BulletText("Touch Foo Games — original Swordigo");
            ImGui::BulletText("FEX-Emu — ARM64 translation");
            ImGui::BulletText("Dear ImGui — immediate-mode UI");
            ImGui::BulletText("SDL3 — cross-platform windowing & input");
            ImGui::BulletText("Font Awesome — icon set");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text(ICON_FA_CUBE "  Architecture:");
            ImGui::BulletText("SRT — Swordigo Runtime (overall architecture)");
            ImGui::BulletText("SRE — Swordigo Runtime Engine (libsre.so hooks)");
            ImGui::BulletText("Primary target: v1.4.12 ARM64");

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndPopup();
}

// =============================================================================
// Save editor (embedded in detail panel)
// =============================================================================

static void DrawSaveEditor(bool& show_save_editor) {
    // Back button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
    if (ImGui::Button(ICON_FA_ARROW_LEFT " Back", ImVec2(100, 30))) {
        show_save_editor = false;
        g_save_sel = -1;
        ImGui::PopStyleColor(2);
        return;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::Text(ICON_FA_FLOPPY_DISK "  Save Editor");
    if (g_font_heading) ImGui::PopFont();

    ImGui::Separator();

    if (!g_save_loaded) {
        ImGui::TextDisabled("Loading saves...");
        return;
    }

    if (g_save_files.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No .gplayer save files found.");
        ImGui::TextDisabled("Save directory: ~/.local/share/swordigo-desktop/save/Documents/");
        return;
    }

    // -- Editing a specific save --
    if (g_save_sel >= 0 && g_save_sel < (int)g_save_files.size()) {
        SaveFile& sf = g_edit_save;

        // Status message
        if (!g_save_status.empty()) {
            ImVec4 col = g_save_status_ok
                ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f)
                : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(col, "%s", g_save_status.c_str());
            ImGui::Spacing();
        }

        ImGui::Text(ICON_FA_FILE "  File: %s", fs::path(sf.filepath).filename().c_str());
        ImGui::Text("Player: %s  |  Level: %d  |  %.0f%% complete",
            sf.name.c_str(), sf.experience_level, sf.percent_completed * 100.0f);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Character Stats:");
        ImGui::InputInt("Coins",  &sf.game_state.character.coins);
        ImGui::InputInt("Health", &sf.game_state.character.health);
        ImGui::InputInt("Mana",   &sf.game_state.character.mana);
        ImGui::InputInt("XP",     &sf.game_state.character.xp);
        ImGui::InputInt("Level",  &sf.game_state.character.level);

        ImGui::Spacing();
        ImGui::Text("Attributes:");
        ImGui::InputInt("Health Attr", &sf.game_state.character.health_attr);
        ImGui::InputInt("Attack Attr", &sf.game_state.character.attack_attr);
        ImGui::InputInt("Magic Attr",  &sf.game_state.character.magic_attr);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Apply / Discard
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.68f, 0.40f, 1.0f));
        if (ImGui::Button(ICON_FA_CHECK " Apply & Save", ImVec2(160, 36))) {
            if (save_write(sf.filepath, sf)) {
                g_save_status = "Save written successfully!";
                g_save_status_ok = true;
                // Update the list entry
                g_save_files[g_save_sel] = sf;
            } else {
                g_save_status = "Failed to write save file!";
                g_save_status_ok = false;
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
        if (ImGui::Button("Discard", ImVec2(120, 36))) {
            g_save_sel = -1;
            g_save_status.clear();
        }
        ImGui::PopStyleColor(2);

        return;
    }

    // -- Save file list --
    if (ImGui::BeginTable("SavesTable", 4,
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
            if (ImGui::Selectable(sf.name.c_str(), false,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                g_save_sel = i;
                g_edit_save = sf;  // Copy for editing
                g_save_status.clear();
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d", sf.experience_level);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", sf.percent_completed * 100.0f);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", fs::path(sf.filepath).filename().c_str());

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

    // Set window icon
    {
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
            SDL_Surface* icon_surf = IMG_Load(icon_paths[i]);
            if (icon_surf) {
                SDL_SetWindowIcon(window, icon_surf);
                SDL_DestroySurface(icon_surf);
                std::cout << "[Launcher] Icon loaded from: " << icon_paths[i] << std::endl;
                break;
            }
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

    // ── ImGui init ──
    IMGUI_CHECKVERSION();
    ImGuiContext* imgui_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Theme
    ApplyCustomTheme();

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
        
        // Icon font config (Font Awesome — merged into same atlas)
        static const ImWchar icon_ranges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
        ImFontConfig icon_cfg;
        icon_cfg.MergeMode = true;
        icon_cfg.OversampleH = 2;
        icon_cfg.OversampleV = 2;
        icon_cfg.PixelSnapH = true;
        icon_cfg.GlyphMinAdvanceX = font_size_main;
        icon_cfg.GlyphOffset = ImVec2(0, 2);
        
        // Search paths for Inter font
        std::string inter_paths[] = {
            get_data_path("src/assets/fonts/Inter-Regular.ttf"),
            get_user_data_dir() + "/launcher/fonts/Inter-Regular.ttf",
            get_user_data_dir() + "/src/assets/fonts/Inter-Regular.ttf",
            "src/assets/fonts/Inter-Regular.ttf",
            "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf",
        };
        
        // Search paths for Font Awesome (FA7 .otf or FA6 .ttf)
        std::string fa_paths[] = {
            "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            get_data_path("src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf"),
            // launcher/ subfolder (RPM/DEB friendly)
            get_user_data_dir() + "/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            get_user_data_dir() + "/launcher/fonts/fa-solid-900.ttf",
            get_user_data_dir() + "/launcher/fonts/fa-solid-900.otf",
            // FA6/FA7 in fonts directory (legacy/simple naming)
            "src/assets/fonts/fa-solid-900.ttf",
            "src/assets/fonts/fa-solid-900.otf",
            get_data_path("src/assets/fonts/fa-solid-900.ttf"),
            get_data_path("src/assets/fonts/fa-solid-900.otf"),
            get_user_data_dir() + "/src/assets/fonts/fa-solid-900.ttf",
            get_user_data_dir() + "/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
            // System install paths
            "/usr/share/swordigo-desktop/src/assets/fonts/fa-solid-900.ttf",
            "/usr/share/swordigo-desktop/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        };
        
        // Find font files
        std::string inter_path, fa_path;
        for (auto& fp : inter_paths) {
            if (fs::exists(fp)) { inter_path = fp; break; }
        }
        for (auto& fp : fa_paths) {
            if (fs::exists(fp)) { fa_path = fp; break; }
        }
        
        bool font_loaded = false;
        if (!inter_path.empty()) {
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
            // Try 1: relative to CWD (dev mode)
            tex = LoadTextureFromFile((std::string("src/assets/") + sub).c_str(), &w, &h);
            // Try 2: launcher/ subfolder in user data dir (RPM/DEB friendly)
            if (!tex) {
                std::string p2 = get_user_data_dir() + "/launcher/" + sub;
                tex = LoadTextureFromFile(p2.c_str(), &w, &h);
            }
            // Try 3: user data dir src/assets (legacy layout)
            if (!tex) {
                std::string p3 = get_user_data_dir() + "/src/assets/" + sub;
                tex = LoadTextureFromFile(p3.c_str(), &w, &h);
            }
            // Try 3b: user data dir /assets/ (original fallback)
            if (!tex) {
                std::string p3b = get_user_data_dir() + "/assets/" + sub;
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
            // Try 6: system install path (deb/rpm packages)
            if (!tex) {
                std::string p6 = std::string("/usr/share/swordigo-desktop/src/assets/") + sub;
                tex = LoadTextureFromFile(p6.c_str(), &w, &h);
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
    bool show_options = false;
    bool show_save_editor = false;

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
                            cfg.use_sre = use_sre_sel;
                            cfg.selected_binary = cur_bins[bin_sel].filepath;
                            cfg.assets_dir = cur_bins[bin_sel].assets_dir;
                            cfg.game_type = cur_bins[bin_sel].game_type;
                            cfg.should_launch = true;
                            running = false;
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
        g_anim_time += 1.0f / 60.0f;

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Panel widths
        float inst_panel_w = 260.0f;
        float mods_panel_w = 280.0f;

        // Draw background
        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            if (g_tex_bg) {
                ImGui::GetBackgroundDrawList()->AddImage(
                    (ImTextureID)(intptr_t)g_tex_bg,
                    ImVec2(vp->WorkPos.x, vp->WorkPos.y),
                    ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, 60)  // ~24% opacity — subtle
                );
                // Dark overlay for readability
                ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(
                    ImVec2(vp->WorkPos.x, vp->WorkPos.y),
                    ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
                    IM_COL32(13, 17, 23, 200),    // top
                    IM_COL32(13, 17, 23, 200),
                    IM_COL32(13, 17, 23, 235),    // bottom: darker
                    IM_COL32(13, 17, 23, 235)
                );
            }
        }

        DrawToolbar(running, cfg, show_options);
        DrawInstancePanel(selector, bin_sel, inst_panel_w);
        DrawDetailPanel(selector, bin_sel, cfg, running, api_sel, engine_sel, use_sre_sel, show_save_editor, mods_panel_w);
        DrawModsPanel(mods_panel_w);
        DrawStatusBar(bin_sel, selector);

        // Options modal
        if (show_options) {
            DrawOptionsModal(show_options);
        }

        // Add Instance popup
        if (g_show_add_instance) {
            ImGui::OpenPopup("Add Instance");
        }
        ImVec2 popup_center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(popup_center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Add Instance", &g_show_add_instance, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));

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
                    pid_t pid = fork();
                    if (pid == 0) {
                        execlp("xdg-open", "xdg-open", data_dir.c_str(), nullptr);
                        _exit(1);
                    }
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
                        std::string base_so = user_data + "/engine/v1.4.12/arm64-v8a/libswordigo.so";

                        // Set data_dir on selector before adding custom instance
                        selector.set_data_dir(user_data);

                        if (selector.add_custom_instance(base_so, g_add_name, assets_dir_name)) {
                            if (g_add_use_sre) {
                                std::string sre_src = user_data + "/engine/v1.4.12/arm64-v8a/libsre.so";
                                std::string arch_dir = user_data + "/engine/custom-" + std::string(g_add_name) + "/arm64-v8a";
                                if (fs::exists(sre_src)) {
                                    try {
                                        fs::copy_file(sre_src, arch_dir + "/libsre.so", fs::copy_options::overwrite_existing);
                                    } catch (...) {}
                                }

                                auto& bins_mut = const_cast<std::vector<BinaryInfo>&>(selector.get_binaries());
                                if (!bins_mut.empty()) {
                                    auto& last = bins_mut.back();
                                    auto& deps = last.dependencies;
                                    deps.erase(std::remove_if(deps.begin(), deps.end(), [](const std::string& d) {
                                        return d == "libmini.so" || d == "libGlossHook.so";
                                    }), deps.end());
                                    auto& dpaths = last.dep_paths;
                                    dpaths.erase(std::remove_if(dpaths.begin(), dpaths.end(), [](const std::string& p) {
                                        return p.find("libmini.so") != std::string::npos ||
                                               p.find("libGlossHook.so") != std::string::npos;
                                    }), dpaths.end());
                                }
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
            ImGui::PopStyleColor(2);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.110f, 0.137f, 0.200f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.180f, 0.210f, 0.300f, 1.0f));
            if (ImGui::Button("Cancel", ImVec2(btn_w, 36))) {
                g_show_add_instance = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);

            ImGui::PopStyleVar();
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
    }

    return cfg;
}
