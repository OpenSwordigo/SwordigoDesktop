// =============================================================================
// Swordfare GUI — Modern ImGui Overlay Implementation  (v7.3)
//
// Dear ImGui (SDL3 + OpenGL 3.3) overlay rendered on top of the running game.
// Manages its own ImGui context so it does not interfere with the launcher's
// context (which is already destroyed before the game boots).
//
// Design notes:
//   - Theme: dark with crimson accent (#e94560), matching the launcher.
//   - The debug window is pinned top-left at startup; user can drag it freely.
//   - All helper stubs for future panels (Controls, Camera, Mods) are marked
//     with TODO so they're easy to find.
// =============================================================================

#include "platform/swordfare_gui.h"
#include "platform/IconsFontAwesome6.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl3.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#ifdef VULKAN_BACKEND
#define VK_NO_PROTOTYPES
#include "volk.h"
#include "platform/vulkan_backend.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#endif
#include "platform/rgc.h"

#include <cstring>
#include "display.h"
#include "input_config.h"
#include "data_path.h"
#include "fbo_scaler.h"

#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <arpa/inet.h>

bool g_sre_overlay_blocking = false;

// Guest heap size reporters (defined in jni_bridge_arm64.cpp / jni_bridge.cpp)
extern "C" uint32_t get_guest_heap_size_64();
extern "C" uint32_t get_guest_heap_size_32();

// Weak fallback definitions for SRE Scene Shifter symbols (resolved from guest space at runtime)
extern "C" {
    __attribute__((weak)) int    g_sre_scene_list_count = 0;
    __attribute__((weak)) char   g_sre_scene_list[256][128] = {{0}};
    __attribute__((weak)) volatile int   g_sre_scene_shift_pending = 0;
    __attribute__((weak)) char   g_sre_scene_shift_target[128] = {0};
    __attribute__((weak)) char   g_sre_scene_shift_spawn[64] = "start";
    __attribute__((weak)) char   g_sre_scene_shift_last_error[256] = {0};
    __attribute__((weak)) volatile int   g_sre_scene_shift_active = 0;
    __attribute__((weak)) char   g_sre_current_scene_name[128] = {0};
    __attribute__((weak)) void   sre_scene_shifter_scan_scenes(void) {}
    // AnimateIn hook trampoline pointer — set by host after relay install
    __attribute__((weak)) void*  g_orig_SceneLoadingView_AnimateIn = NULL;
}

// ---------------------------------------------------------------------------
// Helpers — no stdlib in SRE land, but this is host-side C++ so fine
// ---------------------------------------------------------------------------

static void swardfare_push_fps(float* history, int& idx, float value, int size) {
    history[idx] = value;
    idx = (idx + 1) % size;
}

// ---------------------------------------------------------------------------
// Theme — Swordfare Dark  (mirrors the launcher palette)
// ---------------------------------------------------------------------------

void SwordfareGUI::apply_swordfare_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // --- Geometry ---
    s.WindowRounding    = 10.0f;
    s.ChildRounding     =  8.0f;
    s.FrameRounding     =  6.0f;
    s.PopupRounding     =  8.0f;
    s.GrabRounding      =  4.0f;
    s.TabRounding       =  6.0f;
    s.ScrollbarRounding =  6.0f;

    s.WindowPadding     = ImVec2(16, 14);
    s.FramePadding      = ImVec2(10, 5);
    s.ItemSpacing       = ImVec2(10, 6);
    s.ItemInnerSpacing  = ImVec2(6,  4);
    s.IndentSpacing     = 16.0f;
    s.ScrollbarSize     = 10.0f;
    s.GrabMinSize       = 10.0f;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.TabBorderSize     = 0.0f;

    // --- Colours ---
    ImVec4* c = s.Colors;

    // Backgrounds
    c[ImGuiCol_WindowBg]          = ImVec4(0.04f,  0.05f,  0.07f,  0.92f);  // deep near-black, semi-transparent
    c[ImGuiCol_ChildBg]           = ImVec4(0.07f,  0.09f,  0.12f,  1.00f);  // #121720
    c[ImGuiCol_PopupBg]           = ImVec4(0.06f,  0.08f,  0.11f,  0.97f);
    c[ImGuiCol_FrameBg]           = ImVec4(0.10f,  0.13f,  0.18f,  1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.13f,  0.17f,  0.24f,  1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.16f,  0.20f,  0.28f,  1.00f);

    // Title bar
    c[ImGuiCol_TitleBg]           = ImVec4(0.04f,  0.05f,  0.07f,  1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.07f,  0.08f,  0.10f,  1.00f);
    c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.04f,  0.05f,  0.07f,  0.70f);

    // Borders
    c[ImGuiCol_Border]            = ImVec4(0.914f, 0.271f, 0.376f, 0.25f);  // subtle crimson outline
    c[ImGuiCol_BorderShadow]      = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.04f,  0.05f,  0.07f,  0.40f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.20f,  0.23f,  0.27f,  0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.32f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.38f, 0.42f, 0.48f, 1.00f);

    // Accent — Swordfare Crimson (#e94560)
    ImVec4 accent      = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);
    ImVec4 accent_dim  = ImVec4(0.914f, 0.271f, 0.376f, 0.50f);
    ImVec4 accent_pale = ImVec4(0.914f, 0.271f, 0.376f, 0.20f);

    c[ImGuiCol_CheckMark]         = accent;
    c[ImGuiCol_SliderGrab]        = ImVec4(0.35f,  0.65f,  1.00f,  0.80f);
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.35f,  0.65f,  1.00f,  1.00f);

    c[ImGuiCol_Button]            = accent;
    c[ImGuiCol_ButtonHovered]     = ImVec4(1.00f,  0.38f,  0.48f,  1.00f);
    c[ImGuiCol_ButtonActive]      = ImVec4(0.78f,  0.20f,  0.29f,  1.00f);

    c[ImGuiCol_Header]            = accent_pale;
    c[ImGuiCol_HeaderHovered]     = accent_dim;
    c[ImGuiCol_HeaderActive]      = accent;

    c[ImGuiCol_Separator]         = ImVec4(0.914f, 0.271f, 0.376f, 0.18f);
    c[ImGuiCol_SeparatorHovered]  = accent_dim;
    c[ImGuiCol_SeparatorActive]   = accent;

    c[ImGuiCol_ResizeGrip]        = accent_pale;
    c[ImGuiCol_ResizeGripHovered] = accent_dim;
    c[ImGuiCol_ResizeGripActive]  = accent;

    c[ImGuiCol_Tab]               = ImVec4(0.10f,  0.13f,  0.18f,  1.00f);
    c[ImGuiCol_TabHovered]        = accent_dim;
    c[ImGuiCol_TabSelected]       = ImVec4(0.914f, 0.271f, 0.376f, 0.80f);

    // Text
    c[ImGuiCol_Text]              = ImVec4(0.90f,  0.93f,  0.95f,  1.00f);
    c[ImGuiCol_TextDisabled]      = ImVec4(0.54f,  0.58f,  0.62f,  1.00f);

    // Table
    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.10f,  0.13f,  0.18f,  1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f,  0.23f,  0.27f,  0.60f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.20f,  0.23f,  0.27f,  0.30f);
    c[ImGuiCol_TableRowBg]        = ImVec4(0.00f,  0.00f,  0.00f,  0.00f);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(1.00f,  1.00f,  1.00f,  0.02f);

    // Plot
    c[ImGuiCol_PlotLines]         = ImVec4(0.35f,  0.65f,  1.00f,  1.00f);  // blue FPS graph
    c[ImGuiCol_PlotLinesHovered]  = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);
    c[ImGuiCol_PlotHistogram]     = ImVec4(0.35f,  0.65f,  1.00f,  0.80f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.914f, 0.271f, 0.376f, 1.00f);
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------

void SwordfareGUI::init(SDL_Window* window, SDL_GLContext gl_ctx) {
    if (m_initialized) return;

    m_window  = window;
    m_gl_ctx  = gl_ctx;

    // Create a dedicated ImGui context — does not share state with the launcher
    m_imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;          // no imgui.ini on disk for the in-game overlay
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // game cursor is hidden

    // Compute DPI Scale
    float dpi_scale = 1.0f;
    int ww, wh, pw, ph;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    if (ww > 0 && pw > 0) {
        dpi_scale = (float)pw / (float)ww;
    }
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;

    // Intelligent layout scaling relative to window size
    float layout_scale = 1.0f;
    if (wh >= 1440) {
        layout_scale = 1.5f;
    } else if (wh >= 1080) {
        layout_scale = 1.25f;
    } else {
        layout_scale = 1.0f;
    }

    // Find font files
    std::string main_font_path, button_font_path, fa_path;
    const char* home_val = getenv("HOME");
    std::string home_dir = home_val ? home_val : "";

    std::vector<std::string> main_font_candidates = {
        "src/assets/fonts/MegalopolisExtra-Regular.otf",
        "src/assets/fonts/Redaction10-Regular.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/Redaction10-Regular.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/Redaction10-Regular.otf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Redaction10-Regular.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf"
    };
    if (!home_dir.empty()) {
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/Redaction10-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/MegalopolisExtra-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/Redaction10-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop-launcher/src/assets/fonts/Redaction10-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop-launcher/src/assets/fonts/Inter-Regular.ttf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-launcher/src/assets/fonts/Redaction10-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/swordigo-launcher/src/assets/fonts/Inter-Regular.ttf");
        main_font_candidates.push_back(home_dir + "/.local/share/launcher/src/assets/fonts/Redaction10-Regular.otf");
        main_font_candidates.push_back(home_dir + "/.local/share/launcher/src/assets/fonts/Inter-Regular.ttf");
    }
    for (const auto& fp : main_font_candidates) {
        if (std::filesystem::exists(fp)) {
            main_font_path = fp;
            break;
        }
    }

    std::vector<std::string> button_font_candidates = {
        "src/assets/fonts/MegalopolisExtra-Regular.otf",
        "src/assets/fonts/Redaction10-Bold.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/Redaction10-Bold.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/Redaction10-Bold.otf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Redaction10-Bold.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf"
    };
    if (!home_dir.empty()) {
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/Redaction10-Bold.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/MegalopolisExtra-Regular.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/Redaction10-Bold.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop-launcher/src/assets/fonts/Redaction10-Bold.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-desktop-launcher/src/assets/fonts/Inter-Regular.ttf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-launcher/src/assets/fonts/Redaction10-Bold.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/swordigo-launcher/src/assets/fonts/Inter-Regular.ttf");
        button_font_candidates.push_back(home_dir + "/.local/share/launcher/src/assets/fonts/Redaction10-Bold.otf");
        button_font_candidates.push_back(home_dir + "/.local/share/launcher/src/assets/fonts/Inter-Regular.ttf");
    }
    for (const auto& fp : button_font_candidates) {
        if (std::filesystem::exists(fp)) {
            button_font_path = fp;
            break;
        }
    }

    std::vector<std::string> fa_candidates = {
        "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "src/assets/fonts/fa-solid-900.ttf",
        "/usr/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf",
        "/usr/local/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "/usr/local/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf",
        "/usr/share/swordigo-desktop/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/fa-solid-900.ttf"
    };
    if (!home_dir.empty()) {
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/fa-solid-900.ttf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-desktop-launcher/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-desktop-launcher/src/assets/fonts/fa-solid-900.ttf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-launcher/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf");
        fa_candidates.push_back(home_dir + "/.local/share/swordigo-launcher/src/assets/fonts/fa-solid-900.ttf");
        fa_candidates.push_back(home_dir + "/.local/share/launcher/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf");
        fa_candidates.push_back(home_dir + "/.local/share/launcher/src/assets/fonts/fa-solid-900.ttf");
    }
    for (const auto& fp : fa_candidates) {
        if (std::filesystem::exists(fp)) {
            fa_path = fp;
            break;
        }
    }

    if (!main_font_path.empty()) {
        float main_font_size = 14.0f * dpi_scale;
        m_font_main = io.Fonts->AddFontFromFileTTF(main_font_path.c_str(), main_font_size);

        // Merge FontAwesome into main font
        if (m_font_main && !fa_path.empty()) {
            static const ImWchar icon_ranges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
            ImFontConfig icon_cfg;
            icon_cfg.MergeMode = true;
            icon_cfg.PixelSnapH = true;
            icon_cfg.GlyphMinAdvanceX = main_font_size;
            icon_cfg.GlyphOffset = ImVec2(0, 1);
            io.Fonts->AddFontFromFileTTF(fa_path.c_str(), main_font_size * 0.85f, &icon_cfg, icon_ranges);
        }
    } else {
        m_font_main = io.Fonts->AddFontDefault();
    }

    if (!button_font_path.empty()) {
        float button_font_size = 20.0f * dpi_scale;
        m_font_button = io.Fonts->AddFontFromFileTTF(button_font_path.c_str(), button_font_size);

        // Merge FontAwesome into button font
        if (m_font_button && !fa_path.empty()) {
            static const ImWchar icon_ranges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
            ImFontConfig icon_cfg;
            icon_cfg.MergeMode = true;
            icon_cfg.PixelSnapH = true;
            icon_cfg.GlyphMinAdvanceX = button_font_size;
            icon_cfg.GlyphOffset = ImVec2(0, 2);
            io.Fonts->AddFontFromFileTTF(fa_path.c_str(), button_font_size * 0.85f, &icon_cfg, icon_ranges);
        }
    } else {
        m_font_button = io.Fonts->AddFontDefault();
    }

    apply_swordfare_theme();

    // Scale ImGui styles according to layout scale rather than full physical DPI scale
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(layout_scale);

    // Apply global font scale to make rasterized high-res fonts display at the correct logical size
    io.FontGlobalScale = layout_scale / dpi_scale;

    ImGui_ImplSDL3_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    m_initialized = true;
}

// ---------------------------------------------------------------------------
// init_vulkan — same setup as init() but wires the Dear ImGui Vulkan backend
// ---------------------------------------------------------------------------
#ifdef VULKAN_BACKEND
void SwordfareGUI::init_vulkan(SDL_Window* window, VulkanBackend* vk_backend) {
    if (m_initialized) return;

    m_window        = window;
    m_vk_backend    = vk_backend;
    m_vulkan_active = true;

    // Dedicated ImGui context — does not share state with the launcher
    m_imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // DPI / layout scale (identical logic to init)
    float dpi_scale = 1.0f;
    int ww, wh, pw, ph;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    if (ww > 0 && pw > 0) dpi_scale = (float)pw / (float)ww;
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;

    float layout_scale = (wh >= 1440) ? 1.5f : (wh >= 1080) ? 1.25f : 1.0f;

    // Font candidates — same search paths as init()
    const char* home_val = getenv("HOME");
    std::string home_dir = home_val ? home_val : "";

    auto find_font = [&](std::vector<std::string> candidates) -> std::string {
        if (!home_dir.empty()) {
            candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf");
            candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/MegalopolisExtra-Regular.otf");
            candidates.push_back(home_dir + "/.local/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf");
            candidates.push_back(home_dir + "/.local/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf");
        }
        for (const auto& fp : candidates)
            if (std::filesystem::exists(fp)) return fp;
        return {};
    };

    std::string main_font_path = find_font({
        "src/assets/fonts/MegalopolisExtra-Regular.otf",
        "src/assets/fonts/Redaction10-Regular.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf"
    });
    std::string button_font_path = find_font({
        "src/assets/fonts/MegalopolisExtra-Regular.otf",
        "src/assets/fonts/Redaction10-Bold.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/launcher/fonts/MegalopolisExtra-Regular.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/Inter-Regular.ttf"
    });
    std::string fa_path = find_font({
        "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "src/assets/fonts/fa-solid-900.ttf",
        "/usr/share/swordigo-desktop/launcher/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "/usr/share/swordigo-desktop/launcher/fonts/fa-solid-900.ttf"
    });

    // Load fonts (same logic as init)
    auto merge_fa = [&](float size) {
        if (fa_path.empty()) return;
        static const ImWchar icon_ranges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
        ImFontConfig cfg;
        cfg.MergeMode = true; cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = size; cfg.GlyphOffset = ImVec2(0, 1);
        io.Fonts->AddFontFromFileTTF(fa_path.c_str(), size * 0.85f, &cfg, icon_ranges);
    };

    float main_sz   = 14.0f * dpi_scale;
    float button_sz = 20.0f * dpi_scale;
    m_font_main   = !main_font_path.empty()
                        ? io.Fonts->AddFontFromFileTTF(main_font_path.c_str(), main_sz)
                        : io.Fonts->AddFontDefault();
    if (m_font_main) merge_fa(main_sz);
    m_font_button = !button_font_path.empty()
                        ? io.Fonts->AddFontFromFileTTF(button_font_path.c_str(), button_sz)
                        : io.Fonts->AddFontDefault();
    if (m_font_button) merge_fa(button_sz);

    apply_swordfare_theme();
    ImGui::GetStyle().ScaleAllSizes(layout_scale);
    io.FontGlobalScale = layout_scale / dpi_scale;

    // SDL3 Vulkan platform layer
    ImGui_ImplSDL3_InitForVulkan(window);

    // Initialize ImGui Vulkan functions with volk
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, [](const char* function_name, void* user_data) {
        return vkGetInstanceProcAddr((VkInstance)user_data, function_name);
    }, vk_backend->get_instance());

    // Create ImGui-specific descriptor pool
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,       1000 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = 1000 * 11;
    pool_info.poolSizeCount = 11;
    pool_info.pPoolSizes    = pool_sizes;
    if (vkCreateDescriptorPool(vk_backend->get_device(), &pool_info, nullptr,
                               &m_imgui_vk_descriptor_pool) != VK_SUCCESS) {
        fprintf(stderr, "[SwordfareGUI] Failed to create ImGui Vulkan descriptor pool\n");
    }

    // Wire ImGui Vulkan backend
    ImGui_ImplVulkan_InitInfo vk_init = {};
    vk_init.ApiVersion                         = VK_API_VERSION_1_0;
    vk_init.Instance                           = vk_backend->get_instance();
    vk_init.PhysicalDevice                     = vk_backend->get_physical_device();
    vk_init.Device                             = vk_backend->get_device();
    vk_init.QueueFamily                        = vk_backend->get_graphics_family();
    vk_init.Queue                              = vk_backend->get_graphics_queue();
    vk_init.DescriptorPool                     = m_imgui_vk_descriptor_pool;
    vk_init.MinImageCount                      = vk_backend->get_image_count();
    vk_init.ImageCount                         = vk_backend->get_image_count();
    vk_init.PipelineCache                      = vk_backend->get_pipeline_cache();
    vk_init.PipelineInfoMain.RenderPass        = vk_backend->get_render_pass();
    vk_init.PipelineInfoMain.MSAASamples       = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&vk_init);

    m_initialized = true;
}
#else
void SwordfareGUI::init_vulkan(SDL_Window* window, class VulkanBackend*) {
    // Vulkan backend not compiled — fall back to OpenGL
    fprintf(stderr, "[SwordfareGUI] init_vulkan called but VULKAN_BACKEND not compiled — no-op\n");
    (void)window;
}
#endif

void SwordfareGUI::shutdown() {
    if (!m_initialized) return;
    stop_tcp_server();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
#ifdef VULKAN_BACKEND
    if (m_vulkan_active) {
        ImGui_ImplVulkan_Shutdown();
        if (m_imgui_vk_descriptor_pool != VK_NULL_HANDLE && m_vk_backend) {
            vkDestroyDescriptorPool(m_vk_backend->get_device(), m_imgui_vk_descriptor_pool, nullptr);
            m_imgui_vk_descriptor_pool = VK_NULL_HANDLE;
        }
    } else
#endif
    {
        ImGui_ImplOpenGL3_Shutdown();
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(static_cast<ImGuiContext*>(m_imgui_ctx));
    m_imgui_ctx   = nullptr;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Event processing
// ---------------------------------------------------------------------------

bool SwordfareGUI::process_event(const SDL_Event& event) {
    if (!m_initialized) return false;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
    ImGui_ImplSDL3_ProcessEvent(&event);
    
    ImGuiIO& io = ImGui::GetIO();

    // Forward keyboard capture when debug/mod overlay is visible
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        return m_visible && io.WantCaptureKeyboard;

    // For mouse events: block if ImGui wants it OR if the click lands on any
    // SRE overlay or button area (even when the mod-menu is hidden)
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (io.WantCaptureMouse) return true;
        if (event.button.button == SDL_BUTTON_LEFT) {
            float ex = (float)event.button.x;
            float ey = (float)event.button.y;
            if (is_input_blocked(ex, ey)) return true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// begin_frame / end_frame
// ---------------------------------------------------------------------------

void SwordfareGUI::update_console_backend() {
    if (!m_console_ready || !m_guest_memory) return;

    // ---- Poll and dispatch TCP client commands ----
    if (m_tcp_running) {
        // 1. Dispatch pending TCP command to the guest
        m_tcp_mutex.lock();
        bool has_cmd = !m_tcp_pending_cmd.empty();
        m_tcp_mutex.unlock();

        if (has_cmd) {
            // Check if SRE is ready to accept a new command
            int32_t pending = *(int32_t*)(m_guest_memory + m_console_pending_addr);
            int32_t status = *(int32_t*)(m_guest_memory + m_console_status_addr);
            if (pending == 0 && status == 0) {
                m_tcp_mutex.lock();
                std::string cmd = m_tcp_pending_cmd;
                m_tcp_pending_cmd.clear();
                m_tcp_mutex.unlock();

                m_tcp_cmd_in_flight = true;
                console_submit(cmd);
            }
        }
    }

    // ---- Unified SRE command result polling (Local GUI + TCP Console) ----
    int32_t status = *(int32_t*)(m_guest_memory + m_console_status_addr);
    if (status != 0) {
        const char* result = (const char*)(m_guest_memory + m_console_result_addr);
        std::string res_str = (result && result[0]) ? result : "";

        // Feed to GUI console history & terminal log
        if (!res_str.empty()) {
            m_console_history.push_back({res_str, (status == 2), false});
            while ((int)m_console_history.size() > CONSOLE_MAX_HISTORY)
                m_console_history.erase(m_console_history.begin());
            m_console_scroll_bottom = true;

            // Print output to main terminal for easy copying/logging
            if (status == 2) {
                std::cout << "[SRE-Lua Error] " << res_str << std::endl;
            } else {
                std::cout << "[SRE-Lua Output]\n" << res_str << std::endl;
            }
        }

        // If the command was sent by TCP, send the result back to the client
        if (m_tcp_running && m_tcp_cmd_in_flight) {
            m_tcp_cmd_in_flight = false;
            int client_fd = m_tcp_client_fd.load();
            if (client_fd >= 0) {
                std::string output;
                if (status == 2) {
                    // Output error in red
                    output = "\033[1;31mError: " + res_str + "\033[0m\n\033[1;36mraijin-sdk\033[0m> ";
                } else {
                    // Normal execution output
                    output = res_str.empty() ? "\033[1;36mraijin-sdk\033[0m> " : (res_str + "\n\033[1;36mraijin-sdk\033[0m> ");
                }
                write(client_fd, output.c_str(), output.size());
            }
        }

        *(int32_t*)(m_guest_memory + m_console_status_addr) = 0;
    }
}

void SwordfareGUI::begin_frame() {
    if (!m_initialized) return;

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
#ifdef VULKAN_BACKEND
    if (m_vulkan_active) {
        ImGui_ImplVulkan_NewFrame();
    } else
#endif
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void SwordfareGUI::end_frame() {
    if (!m_initialized) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
    ImGui::Render();
#ifdef VULKAN_BACKEND
    if (m_vulkan_active && m_vk_backend) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_vk_backend->get_current_command_buffer());
    } else
#endif
    {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

// ---------------------------------------------------------------------------
// draw_debug — the F3 debug window
// ---------------------------------------------------------------------------

void SwordfareGUI::draw_debug(const SwordfareDebugStats& st) {
    // -- Update FPS ring buffer --
    swardfare_push_fps(m_fps_history, m_fps_idx, st.fps, FPS_HISTORY);

    static bool expanded = false;

    // Intelligent layout scaling relative to window size
    float layout_scale = 1.0f;
    if (st.win_h >= 1440) {
        layout_scale = 1.5f;
    } else if (st.win_h >= 1080) {
        layout_scale = 1.25f;
    } else {
        layout_scale = 1.0f;
    }

    // -- Window position (top-left, draggable) --
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2((expanded ? 410 : 200) * layout_scale, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoScrollbar;

    if (!ImGui::Begin("  Swordfare Debug  [F3]", nullptr, wflags)) {
        ImGui::End();
        return;
    }

    // -- Mini Header: FPS + Expand Button --
    {
        // FPS badge — coloured by performance
        ImVec4 fps_col = (st.fps >= 55.0f) ? ImVec4(0.30f, 0.90f, 0.50f, 1.0f)   // green
                       : (st.fps >= 30.0f) ? ImVec4(1.00f, 0.78f, 0.20f, 1.0f)   // amber
                                             : ImVec4(0.91f, 0.27f, 0.38f, 1.0f);  // red

        ImGui::PushStyleColor(ImGuiCol_Text, fps_col);
        char fps_label[32];
        snprintf(fps_label, sizeof(fps_label), "%.1f FPS", st.fps);
        ImGui::Text("%s", fps_label);
        ImGui::PopStyleColor();

        ImGui::SameLine((expanded ? 355 : 155) * layout_scale);
        if (ImGui::Button(expanded ? " < ##exp" : " > ##exp", ImVec2(30 * layout_scale, 20 * layout_scale))) {
            expanded = !expanded;
        }
    }

    // -- Expanded Panel System --
    if (expanded) {
        ImGui::Separator();
        
        // Frame Sparkline
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.09f, 0.12f, 1.0f));
        ImGui::PlotLines("##fps_graph", m_fps_history, FPS_HISTORY, m_fps_idx,
                         nullptr, 0.0f, 75.0f, ImVec2(-1, 42));
        ImGui::PopStyleColor();

        ImGui::Separator();

        // -- Render stats table --
        if (ImGui::CollapsingHeader("Render Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("rendertable", 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthFixed, 140 * layout_scale);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                auto row = [](const char* key, const char* fmt, ...) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                    ImGui::TextUnformatted(key);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    char buf[128];
                    va_list args;
                    va_start(args, fmt);
                    vsnprintf(buf, sizeof(buf), fmt, args);
                    va_end(args);
                    ImGui::TextUnformatted(buf);
                };

                row("Internal Res",  "%d\xc3\x97%d", st.draw_w, st.draw_h);
                row("Window Size",   "%d\xc3\x97%d", st.win_w, st.win_h);
                row("Draw Calls",    "%d",  st.draw_calls);
                row("Tex Binds",     "%d",  st.tex_binds);
                row("Vertices",      "%d",  st.vertices);
                row("State Changes", "%d",  st.state_changes);
                row("Tex Uploads",   "%d",  st.tex_uploads);
                row("Frame Delta",   "%.4f s", st.dt_seconds);
                row("Mouse Coordinates", "%d, %d", st.mouse_x, st.mouse_y);

                ImGui::EndTable();
            }
        }

        ImGui::Spacing();

        // -- System info --
        if (ImGui::CollapsingHeader("System & Modding", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("systable", 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthFixed, 140 * layout_scale);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                auto row = [](const char* key, const char* fmt, ...) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                    ImGui::TextUnformatted(key);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    char buf[256];
                    va_list args;
                    va_start(args, fmt);
                    vsnprintf(buf, sizeof(buf), fmt, args);
                    va_end(args);
                    ImGui::TextUnformatted(buf);
                };

                row("Engine Binary",  "%s", st.binary_name);
                row("Graphics API",   "%s", st.graphics_api);
                row("Scaling Mode",   "%s", st.scale_mode);

                // PostFX with coloured badge
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::TextUnformatted("PostFX Mode");
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1);
                if (st.postfx_on) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.30f, 1.0f));
                    ImGui::Text("ON (%s)", st.postfx_preset);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.58f, 0.62f, 1.0f));
                    ImGui::TextUnformatted("Disabled");
                    ImGui::PopStyleColor();
                }

                row("Game Instance",  "%s%s",
                    st.speed_label,
                    st.game_paused ? "  (PAUSED)" : "");

                // Camera
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::TextUnformatted("Camera Focus");
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1);
                if (st.cam_active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.90f, 0.50f, 1.0f));
                    ImGui::Text("Overridden  (%.1f, %.1f, %.1f)",
                                 st.cam_x, st.cam_y, st.cam_z);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.58f, 0.62f, 1.0f));
                    ImGui::TextUnformatted("Hero Follow");
                    ImGui::PopStyleColor();
                }

                row("Hero Coords", "%.1f, %.1f, %.1f", st.hero_x, st.hero_y, st.hero_z);

                ImGui::EndTable();
            }
        }

        ImGui::Spacing();

        // -- Redstell GC Panel --
        if (ImGui::CollapsingHeader("Redstell Garbage Collector", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("rgctable", 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthFixed, 140 * layout_scale);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                auto row = [](const char* key, const char* fmt, ...) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                    ImGui::TextUnformatted(key);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    char buf[256];
                    va_list args;
                    va_start(args, fmt);
                    vsnprintf(buf, sizeof(buf), fmt, args);
                    va_end(args);
                    ImGui::TextUnformatted(buf);
                };

                auto& rgc = RedstellGC::instance();

                // Guest heap: pages actually accessed in the mmap'd guest address space
                uint32_t ghs64 = get_guest_heap_size_64();
                uint32_t ghs32 = get_guest_heap_size_32();
                uint32_t guest_heap_total = ghs64 + ghs32;
                row("Guest Heap Used", "%.2f MB", (float)guest_heap_total / (1024.0f * 1024.0f));
                row("Current RAM", "%.2f MB", (float)rgc.get_current_ram() / (1024.0f * 1024.0f));
                row("Peak RAM", "%.2f MB", (float)rgc.get_peak_ram() / (1024.0f * 1024.0f));
                row("Largest Alloc", "%.2f MB", (float)rgc.get_largest_allocation() / (1024.0f * 1024.0f));
                row("Fragmentation", "%.1f%%", rgc.get_fragmentation_estimate() * 100.0f);
                row("Alloc / Free Rate", "%lu / %lu per sec", (unsigned long)rgc.get_alloc_rate(), (unsigned long)rgc.get_free_rate());
                row("Live Resources", "%lu", (unsigned long)rgc.get_resource_count());
                row("Optimizations Done", "%lu", (unsigned long)rgc.get_optimizations_performed());
                row("Pending Cleanups", "%u items", rgc.get_cleanup_queue_size());
                row("Last GC Duration", "%.2f ms", rgc.get_last_cleanup_duration());

                ImGui::EndTable();
            }

            ImGui::Spacing();
            if (ImGui::Button("Generate Allocation Report", ImVec2(-1, 0))) {
                RedstellGC::instance().generate_allocation_report();
            }

            // Resource Breakdown Sub-Header
            if (ImGui::TreeNode("Resource Breakdown")) {
                auto& rgc = RedstellGC::instance();
                ImGui::BulletText("Textures: %u", rgc.get_texture_count());
                ImGui::BulletText("POD Models: %u", rgc.get_pod_count());
                ImGui::BulletText("Lua States/Objects: %u", rgc.get_lua_object_count());
                ImGui::BulletText("OpenGL Handles: %u", rgc.get_opengl_count());
                ImGui::BulletText("Audio Channels: %u", rgc.get_openal_count());
                ImGui::BulletText("VFS File Handles: %u", rgc.get_open_file_count());
                ImGui::TreePop();
            }

            // Recent Logs Sub-Header
            if (ImGui::TreeNode("Subsystem Diagnostics")) {
                auto logs = RedstellGC::instance().get_logs();
                ImGui::BeginChild("RgcLogChild", ImVec2(0, 100 * layout_scale), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (int i = (int)logs.size() - 1; i >= 0; i--) {
                    ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.8f, 1.0f), "[%lu ms] %s", 
                        (unsigned long)logs[i].timestamp, logs[i].message.c_str());
                }
                ImGui::EndChild();
                ImGui::TreePop();
            }
        }

        ImGui::Spacing();

        // -- Status flags row --
        {
            if (st.typing_mode) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.91f, 0.27f, 0.38f, 1.0f));
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextUnformatted("TYPING MODE ACTIVE");
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // -- Keybind hint (compact) --
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.38f, 0.43f, 1.0f));
            ImGui::SetWindowFontScale(0.88f);
            ImGui::TextWrapped("F1:GUI  F2:Ctrl  F3:Debug  F4:Scale  F5:Cam  F6:PostFX  F7:Video  \\:Type  F10:HUD");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Custom Button Array structures (matches SreBtnSlot in sre_mini_api.c)
// ---------------------------------------------------------------------------
#define SRE_BTN_MAX       128
#define SRE_BTN_ID_LEN    32
#define SRE_BTN_LABEL_LEN 64

struct SreBtnSlot {
    char     id[SRE_BTN_ID_LEN];       /* Lua string ID */
    char     label[SRE_BTN_LABEL_LEN]; /* Display text */
    float    x, y;                      /* Normalized position (0-1) */
    float    w, h;                      /* Normalized dimensions (base-relative) */
    float    alpha;                     /* Overall alpha 0-255 */
    float    scale_x, scale_y;         /* Scaling factors */
    int      text_color;               /* Packed ARGB */
    float    text_scale;               /* Text size multiplier */
    int      bg_alpha;                 /* Background alpha (0-255) */
    int      hidden;                   /* Per-button hidden flag */
    int      clickable;                /* Whether it accepts clicks */
    int      movable;                  /* Can be dragged */
    int      snapback;                 /* Returns to original pos on release */
    float    home_x, home_y;           /* Original position (for snapback) */
    int      padding_l, padding_t, padding_r, padding_b;
    int      alignment;                /* Text alignment / gravity */
    char     overlay_id[SRE_BTN_ID_LEN]; /* Belongs to overlay */
    int      confined;                  /* Confined to overlay bounds */
    /* ---- STATE (written by host, read by SRE) ---- */
    volatile int pressed;              /* Host writes: 1=down, 0=up */
    volatile int released;             /* Host writes: 1 on release */
    volatile int dragging;             /* Host writes: 1=dragging, 0=not */
    volatile float cur_x, cur_y;       /* Current position (after drag) */
    int      active;                   /* 1 = slot in use, 0 = free */
    int      dirty;                    /* 1 = needs visual update by host */
};

#define SRE_OVERLAY_MAX 8
struct SreOverlaySlot {
    char     id[SRE_BTN_ID_LEN];
    float    x, y;
    float    w, h;
    int      bg_color;
    int      bg_alpha;
    float    corner_radius;
    int      hidden;
    int      movable;
    int      pinchable;
    float    scale_factor;
    int      pinching;
    /* Separators inside overlay */
    float    separators[8];
    int      separator_count;
    int      active;
    int      dirty;
};

void SwordfareGUI::draw_buttons(void* guest_buttons_ptr, void* guest_overlays_ptr, bool globally_hidden) {
    m_last_buttons_ptr = guest_buttons_ptr;
    m_last_overlays_ptr = guest_overlays_ptr;
    
    // Diagnostic log
    static int log_ticks = 0;
    if (guest_buttons_ptr && log_ticks++ % 60 == 0) {
        FILE* f = fopen("sre_gui_debug.log", "w");
        if (f) {
            SreOverlaySlot* ovrs = static_cast<SreOverlaySlot*>(guest_overlays_ptr);
            SreBtnSlot* btns = static_cast<SreBtnSlot*>(guest_buttons_ptr);
            fprintf(f, "=== Overlays ===\n");
            if (ovrs) {
                for (int i = 0; i < SRE_OVERLAY_MAX; i++) {
                    if (ovrs[i].active) {
                        fprintf(f, "Slot %d: id='%s' active=%d hidden=%d x=%.3f y=%.3f w=%.3f h=%.3f bg_color=0x%08X bg_alpha=%d corner_radius=%.3f scale=%.3f\n",
                                i, ovrs[i].id, ovrs[i].active, ovrs[i].hidden,
                                ovrs[i].x, ovrs[i].y, ovrs[i].w, ovrs[i].h,
                                (unsigned int)ovrs[i].bg_color, ovrs[i].bg_alpha, ovrs[i].corner_radius, ovrs[i].scale_factor);
                    } else {
                        fprintf(f, "Slot %d: id='%s' active=0\n", i, ovrs[i].id);
                    }
                }
            } else {
                fprintf(f, "overlays is NULL\n");
            }
            fprintf(f, "\n=== Buttons ===\n");
            for (int i = 0; i < SRE_BTN_MAX; i++) {
                if (btns[i].active) {
                    fprintf(f, "Slot %d: id='%s' label='%s' active=%d hidden=%d overlay_id='%s' x=%.3f y=%.3f w=%.3f h=%.3f bg_alpha=%d alpha=%.3f clickable=%d\n",
                            i, btns[i].id, btns[i].label, btns[i].active, btns[i].hidden,
                            btns[i].overlay_id, btns[i].cur_x, btns[i].cur_y, btns[i].w, btns[i].h,
                            btns[i].bg_alpha, btns[i].alpha, btns[i].clickable);
                }
            }
            fclose(f);
        }
    }

    if (!m_initialized || !guest_buttons_ptr || globally_hidden) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    // Retrieve active viewport bounds for correct positioning
    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    extern int GAME_W, GAME_H;
    float game_asp = (float)GAME_W / (float)GAME_H;
    float win_asp  = win_w / win_h;
    float vp_x = 0, vp_y = 0, vp_w = win_w, vp_h = win_h;

    if (win_asp > game_asp) {
        vp_w = win_h * game_asp;
        vp_x = (win_w - vp_w) / 2;
    } else {
        vp_h = win_w / game_asp;
        vp_y = (win_h - vp_h) / 2;
    }

    SreOverlaySlot* overlays = static_cast<SreOverlaySlot*>(guest_overlays_ptr);
    SreBtnSlot* buttons = static_cast<SreBtnSlot*>(guest_buttons_ptr);

    static int last_active_count = -1;
    int current_active_count = 0;
    for (int i = 0; i < SRE_BTN_MAX; i++) {
        if (buttons[i].active) current_active_count++;
    }
    if (current_active_count != last_active_count) {
        last_active_count = current_active_count;
        std::cout << "[GUI-Debug] Active buttons count changed to " << current_active_count << std::endl;
    }

    // Determine if any full-screen / blocking overlay is active (for input gating)
    bool any_overlay_blocking = false;
    if (overlays) {
        for (int i = 0; i < SRE_OVERLAY_MAX; i++) {
            SreOverlaySlot& ovr = overlays[i];
            if (!ovr.active || ovr.hidden) continue;
            if (ovr.w >= 0.6f && ovr.h >= 0.6f) {
                any_overlay_blocking = true;
                break;
            }
        }
    }
    extern bool g_sre_overlay_blocking;
    g_sre_overlay_blocking = any_overlay_blocking;

    // Set up transparent fullscreen window for drawing and hit-testing
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoDecoration;

    ImVec2 mpos = io.MousePos;
    if (!is_input_blocked(mpos.x, mpos.y)) {
        window_flags |= ImGuiWindowFlags_NoInputs;
    }

    ImGui::Begin("##SreOverlayButtonsWindow", nullptr, window_flags);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // 1. Draw Overlays
    if (overlays) {
        for (int i = 0; i < SRE_OVERLAY_MAX; i++) {
            SreOverlaySlot& ovr = overlays[i];
            if (!ovr.active || ovr.hidden) continue;

            float ow = vp_w * ovr.w * ovr.scale_factor;
            float oh = vp_h * ovr.h * ovr.scale_factor;
            float ox = vp_x + vp_w * ovr.x - ow / 2.0f;
            float oy = vp_y + vp_h * ovr.y - oh / 2.0f;

            // Handle dragging & pinching on the overlay background
            if (ovr.movable || ovr.pinchable) {
                ImGui::SetCursorScreenPos(ImVec2(ox, oy));
                ImGui::PushID(ovr.id);
                ImGui::InvisibleButton("##ovr_bg", ImVec2(ow, oh));
                
                bool ovr_hovered = ImGui::IsItemHovered();
                bool ovr_active = ImGui::IsItemActive();

                if (ovr_active && ovr.movable && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    ImVec2 delta = io.MouseDelta;
                    if (vp_w > 0) ovr.x += delta.x / vp_w;
                    if (vp_h > 0) ovr.y += delta.y / vp_h;
                    ovr.dirty = 1;
                }

                if (ovr_hovered && ovr.pinchable) {
                    float wheel = io.MouseWheel;
                    if (wheel != 0.0f) {
                        ovr.scale_factor += wheel * 0.1f;
                        if (ovr.scale_factor < 0.2f) ovr.scale_factor = 0.2f;
                        if (ovr.scale_factor > 3.0f) ovr.scale_factor = 3.0f;
                        ovr.dirty = 1;
                    }
                }
                ImGui::PopID();
            }

            // Parse background color from packed ARGB (0xAARRGGBB)
            unsigned int c_val = (unsigned int)ovr.bg_color;
            uint8_t ba = (c_val >> 24) & 0xFF;
            uint8_t br = (c_val >> 16) & 0xFF;
            uint8_t bg_c = (c_val >> 8) & 0xFF;
            uint8_t bb = c_val & 0xFF;

            if (c_val == 0) {
                br = 22; bg_c = 27; bb = 34;
                ba = (uint8_t)ovr.bg_alpha;
            }

            ImU32 fill_col = IM_COL32(br, bg_c, bb, ba);
            float rounding = (ovr.corner_radius > 0.0f) ? ovr.corner_radius : 6.0f;
            if (ovr.w >= 0.98f && ovr.h >= 0.98f) rounding = 0.0f;

            draw_list->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + ow, oy + oh), fill_col, rounding);

            if (ovr.w < 0.98f || ovr.h < 0.98f) {
                draw_list->AddRect(ImVec2(ox, oy), ImVec2(ox + ow, oy + oh),
                                   IM_COL32(233, 69, 96, 100), rounding, 0, 1.0f);
            }

            // Separators
            for (int s = 0; s < ovr.separator_count; s++) {
                float sy = ovr.separators[s];
                float line_y = oy + oh * sy;
                draw_list->AddLine(ImVec2(ox, line_y), ImVec2(ox + ow, line_y),
                                   IM_COL32(255, 255, 255, 50), 1.5f);
            }
        }
    }

    // 2. Draw Buttons
    ImFont* btn_font = static_cast<ImFont*>(m_font_button);

    for (int i = 0; i < SRE_BTN_MAX; i++) {
        SreBtnSlot& btn = buttons[i];
        if (!btn.active || btn.hidden) continue;

        // Resolve parent overlay
        SreOverlaySlot* parent_ovr = nullptr;
        if (btn.overlay_id[0] != '\0' && overlays) {
            for (int o = 0; o < SRE_OVERLAY_MAX; o++) {
                if (overlays[o].active && strcmp(overlays[o].id, btn.overlay_id) == 0) {
                    parent_ovr = &overlays[o];
                    break;
                }
            }
            if (!parent_ovr) continue;
        }

        // Hide button if parent overlay is hidden/inactive
        if (parent_ovr && (parent_ovr->hidden || !parent_ovr->active)) continue;

        // Compute sizes, accounting for overlay scale
        float o_scale = parent_ovr ? parent_ovr->scale_factor : 1.0f;
        float pw = vp_w * btn.w * btn.scale_x * o_scale;
        float ph = vp_h * btn.h * btn.scale_y * o_scale;
        float bx, by;

        if (parent_ovr) {
            float ow = vp_w * parent_ovr->w * parent_ovr->scale_factor;
            float oh = vp_h * parent_ovr->h * parent_ovr->scale_factor;
            float ox = vp_x + vp_w * parent_ovr->x - ow / 2.0f;
            float oy = vp_y + vp_h * parent_ovr->y - oh / 2.0f;
            bx = ox + ow * btn.cur_x - pw / 2.0f;
            by = oy + oh * btn.cur_y - ph / 2.0f;
        } else {
            bx = vp_x + vp_w * btn.cur_x - pw / 2.0f;
            by = vp_y + vp_h * btn.cur_y - ph / 2.0f;
        }

        float bx2 = bx + pw;
        float by2 = by + ph;
        if (bx2 < 0 || by2 < 0 || bx > win_w || by > win_h) continue;

        // Interactive Invisible Button
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushID(btn.id);
        
        bool clicked = false;
        if (btn.clickable) {
            clicked = ImGui::InvisibleButton("##btn", ImVec2(pw, ph));
        } else {
            // Non-interactive spacing/dummy widget
            ImGui::Dummy(ImVec2(pw, ph));
        }

        bool hovered = btn.clickable && ImGui::IsItemHovered();
        bool pressing = btn.clickable && ImGui::IsItemActive();

        // Draw premium background style
        if (btn.bg_alpha > 0) {
            float alpha = (btn.alpha / 255.0f) * (btn.bg_alpha / 255.0f);
            
            ImU32 bg_col;
            if (pressing) {
                bg_col = IM_COL32(0, 100, 200, (uint8_t)(alpha * 255.0f * 0.9f));
            } else if (hovered) {
                bg_col = IM_COL32(40, 45, 60, (uint8_t)(alpha * 255.0f * 0.85f));
            } else {
                bg_col = IM_COL32(20, 20, 25, (uint8_t)(alpha * 255.0f * 0.75f));
            }
            
            float rounding = ph * 0.25f;
            if (rounding < 4.0f) rounding = 4.0f;
            if (rounding > 12.0f) rounding = 12.0f;

            draw_list->AddRectFilled(ImVec2(bx, by), ImVec2(bx2, by2), bg_col, rounding);

            ImU32 border_col;
            if (pressing) {
                border_col = IM_COL32(0, 180, 255, 255);
            } else if (hovered) {
                border_col = IM_COL32(0, 140, 255, 220);
            } else {
                border_col = IM_COL32(255, 255, 255, (uint8_t)(alpha * 120.0f));
            }
            draw_list->AddRect(ImVec2(bx, by), ImVec2(bx2, by2), border_col, rounding, 0, 1.5f);
        }

        // Draw label
        const char* lbl = btn.label;
        if (lbl[0] != '\0' && btn_font) {
            float fscale = btn.text_scale > 0.0f ? btn.text_scale : 1.0f;
            if (fscale > 3.0f) fscale = 3.0f;
            float fs = btn_font->LegacySize * fscale * o_scale;
            ImVec2 tsize = btn_font->CalcTextSizeA(fs, FLT_MAX, 0.0f, lbl);

            float pl = btn.padding_l * o_scale;
            float pt = btn.padding_t * o_scale;
            float pr = btn.padding_r * o_scale;
            float pb = btn.padding_b * o_scale;

            int gravity = btn.alignment;
            float tx = bx + (pw - tsize.x) * 0.5f;
            float ty = by + (ph - tsize.y) * 0.5f;

            if (gravity != 0) {
                if ((gravity & 3) == 3 || gravity == 3) {
                    tx = bx + pl;
                } else if ((gravity & 5) == 5 || gravity == 5) {
                    tx = bx2 - tsize.x - pr;
                }
                
                if ((gravity & 48) == 48 || gravity == 48) {
                    ty = by + pt;
                } else if ((gravity & 80) == 80 || gravity == 80) {
                    ty = by2 - tsize.y - pb;
                }
            }

            unsigned int tc = (unsigned int)btn.text_color;
            uint8_t ta = (tc >> 24) & 0xFF;
            uint8_t tr = (tc >> 16) & 0xFF;
            uint8_t tg = (tc >> 8)  & 0xFF;
            uint8_t tb = tc & 0xFF;
            if (ta == 0) ta = (uint8_t)btn.alpha;
            ImU32 text_col = IM_COL32(tr, tg, tb, ta);

            draw_list->AddText(btn_font, fs, ImVec2(tx, ty), text_col, lbl);
        }

        // Input state machine
        if (btn.clickable) {
            if (pressing) {
                btn.pressed = 1;
                if (btn.movable && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    btn.dragging = 1;
                    ImVec2 delta = io.MouseDelta;
                    if (parent_ovr) {
                        float ow = vp_w * parent_ovr->w * parent_ovr->scale_factor;
                        float oh = vp_h * parent_ovr->h * parent_ovr->scale_factor;
                        if (ow > 0) btn.cur_x += delta.x / ow;
                        if (oh > 0) btn.cur_y += delta.y / oh;
                    } else {
                        if (vp_w > 0) btn.cur_x += delta.x / vp_w;
                        if (vp_h > 0) btn.cur_y += delta.y / vp_h;
                    }

                    if (btn.confined) {
                        if (parent_ovr) {
                            float min_x = (pw / 2.0f) / (vp_w * parent_ovr->w * parent_ovr->scale_factor);
                            float max_x = 1.0f - min_x;
                            float min_y = (ph / 2.0f) / (vp_h * parent_ovr->h * parent_ovr->scale_factor);
                            float max_y = 1.0f - min_y;
                            if (btn.cur_x < min_x) btn.cur_x = min_x;
                            if (btn.cur_x > max_x) btn.cur_x = max_x;
                            if (btn.cur_y < min_y) btn.cur_y = min_y;
                            if (btn.cur_y > max_y) btn.cur_y = max_y;
                        } else {
                            float min_x = (pw / 2.0f) / vp_w;
                            float max_x = 1.0f - min_x;
                            float min_y = (ph / 2.0f) / vp_h;
                            float max_y = 1.0f - min_y;
                            if (btn.cur_x < min_x) btn.cur_x = min_x;
                            if (btn.cur_x > max_x) btn.cur_x = max_x;
                            if (btn.cur_y < min_y) btn.cur_y = min_y;
                            if (btn.cur_y > max_y) btn.cur_y = max_y;
                        }
                    }
                    btn.dirty = 1;
                }
            } else {
                btn.pressed = 0;
                if (btn.dragging) {
                    btn.dragging = 0;
                    if (btn.snapback) {
                        btn.cur_x = btn.home_x;
                        btn.cur_y = btn.home_y;
                        btn.dirty = 1;
                    }
                }
            }

            if (clicked) {
                btn.released = 1;
                std::cout << "[SRE GUI] ImGui Click: '" << btn.label << "' (slot " << i << ")\n";
            }
        }

        ImGui::PopID();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}


void SwordfareGUI::draw_mod_overlay(const std::string& save_dir) {
    if (!m_initialized || !m_mod_overlay_visible) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    float panel_w = win_w * 0.70f;
    float panel_h = win_h * 0.75f;
    if (panel_w < 640.0f) panel_w = win_w * 0.95f;
    if (panel_h < 420.0f) panel_h = win_h * 0.95f;

    ImGui::SetNextWindowPos(ImVec2((win_w - panel_w) / 2.0f, (win_h - panel_h) / 2.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.086f, 0.106f, 0.133f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.914f, 0.271f, 0.376f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.086f, 0.106f, 0.133f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.086f, 0.106f, 0.133f, 1.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | 
                             ImGuiWindowFlags_NoSavedSettings;

    bool open = true;
    if (ImGui::Begin("Swordiforge Engine & Mod Launcher", &open, flags)) {
        ImGui::PushFont(static_cast<ImFont*>(m_font_button));
        ImGui::TextColored(ImVec4(0.000f, 0.850f, 1.000f, 1.00f), "SWORDIFORGE MOD STORE & LAUNCHER");
        ImGui::PopFont();
        ImGui::Text("v7.5 — PC Mod Ecosystem & SRE Runtime Environment");
        ImGui::Separator();
        ImGui::Spacing();

        if (m_status_timer > 0.0f) {
            m_status_timer -= ImGui::GetIO().DeltaTime;
        }

        if (ImGui::BeginTabBar("##mod_overlay_tabs")) {
            if (ImGui::BeginTabItem("Readme")) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.125f, 0.150f, 0.190f, 0.3f));
                if (ImGui::BeginChild("##readme_child", ImVec2(0, 0), true)) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Overview:");
                    ImGui::Text("This companion overlay provides seamless JNI-compatibility with the Swordigo SwKiwi Android mod loader ecosystem.");
                    ImGui::Text("It translates Java bridge callbacks, redirects on-screen touch commands, handles LNI native requests (speed hack, clipboard actions, etc.), and renders UI extensions natively on PC.");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "Instructions:");
                    ImGui::BulletText("Toggle Debug Launcher: [F1]");
                    ImGui::BulletText("Toggle In-Game Controls Config: [F2]");
                    ImGui::BulletText("Toggle This Mod Loader Overlay: [F4] or mod button");
                    ImGui::BulletText("Toggle Save & Stats Editor Tab: [F11]");
                    ImGui::BulletText("Modify game speed using [-] and [0] keys.");
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(ICON_FA_GEAR " Options")) {
                ImGui::Spacing();
                extern void apply_render_preset(int preset);
                extern int  g_render_preset;
                extern int  GAME_W, GAME_H;
                extern int  g_win_w, g_win_h;
                extern PostFXState  g_postfx;
                extern PostFXPreset g_postfx_preset;

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.12f, 0.5f));
                if (ImGui::BeginChild("##options_child", ImVec2(0, 0), true)) {

                    // ── RENDER RESOLUTION ──────────────────────────────────────────────
                    ImGui::TextColored(ImVec4(0.00f, 0.85f, 1.00f, 1.0f), ICON_FA_DISPLAY "  RENDER RESOLUTION");
                    ImGui::TextDisabled("Internal FBO resolution the game renders at. Upscaled to output by the active filter.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Active render: "); ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.30f, 0.90f, 0.50f, 1.0f), "%d x %d", GAME_W, GAME_H);
                    ImGui::Spacing();

                    struct ResPreset { const char* label; const char* desc; int idx; int w; int h; };
                    static const ResPreset render_presets[] = {
                        { "4K      (3840 x 2160)", "Ultra HD — huge GPU cost",                                    10, 3840, 2160 },
                        { "1440p   (2560 x 1440)", "2K — sharp, moderate GPU cost",                              9,  2560, 1440 },
                        { "1080p   (1920 x 1080)", "Full HD — best quality/perf tradeoff",                       0,  1920, 1080 },
                        { "900p    (1600 x  900)", "High — ~30% less GPU than 1080p",                           11,  1600,  900 },
                        { "768p    (1366 x  768)", "Laptop native — balanced",                                  12,  1366,  768 },
                        { "720p    (1280 x  720)", "HD — ~44% GPU savings vs 1080p",                             1,  1280,  720 },
                        { "600p    (1024 x  600)", "Tablet — good for low-end GPU",                             13,  1024,  600 },
                        { "540p    ( 960 x  544)", "Android native — fastest, ~75% savings",                    2,   960,  544 },
                        { "480p    ( 854 x  480)", "WVGA — very fast",                                          14,   854,  480 },
                        { "360p    ( 640 x  360)", "Low — maximum performance",                                 15,   640,  360 },
                        { "240p    ( 426 x  240)", "Retro-low — extreme performance mode",                     16,   426,  240 },
                        { "WUXGA   (1920 x 1200)", "16:10 — slightly taller than 1080p",                       17,  1920, 1200 },
                        { "1440x900 (1440 x  900)", "Older widescreen laptop",                                 18,  1440,  900 },
                        { "1280x800 (1280 x  800)", "iPad-like — 16:10",                                       19,  1280,  800 },
                        { "Window  (match output)",  "1:1 pixel mapping — no upscaling",                        3,     0,    0 },
                    };
                    static const int render_preset_count = (int)(sizeof(render_presets)/sizeof(render_presets[0]));

                    // 3-column preset grid
                    for (int pi = 0; pi < render_preset_count; pi++) {
                        const ResPreset& pr = render_presets[pi];
                        bool selected = (g_render_preset == pr.idx);
                        if (pi % 3 != 0) ImGui::SameLine(0, 6);
                        if (selected) {
                            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.52f, 0.18f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.22f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.42f, 0.14f, 1.0f));
                        }
                        ImGui::PushID(100 + pi);
                        if (ImGui::Button(pr.label, ImVec2(250, 30))) {
                            apply_render_preset(pr.idx);
                            m_status_msg = std::string("Render res: ") + pr.label;
                            m_status_timer = 3.0f;
                        }
                        ImGui::PopID();
                        if (selected) ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pr.desc);
                    }

                    // Custom render resolution
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Custom render resolution:");
                    static int custom_rw = 1280, custom_rh = 720;
                    ImGui::SetNextItemWidth(120); ImGui::InputInt("W##crw", &custom_rw); ImGui::SameLine();
                    ImGui::SetNextItemWidth(120); ImGui::InputInt("H##crh", &custom_rh); ImGui::SameLine();
                    if (ImGui::Button("Apply Custom Render Res")) {
                        custom_rw = std::max(160, std::min(custom_rw, 7680));
                        custom_rh = std::max(120, std::min(custom_rh, 4320));
                        GAME_W = custom_rw & ~1; GAME_H = custom_rh & ~1;
                        extern void fbo_destroy(); extern bool fbo_init(int, int);
                        fbo_destroy(); fbo_init(GAME_W, GAME_H);
                        g_render_preset = -1;
                        m_status_msg = "Custom render res: " + std::to_string(GAME_W) + "x" + std::to_string(GAME_H);
                        m_status_timer = 3.0f;
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // ── OUTPUT / WINDOW RESOLUTION ────────────────────────────────────
                    ImGui::TextColored(ImVec4(0.00f, 0.85f, 1.00f, 1.0f), ICON_FA_IMAGE "  OUTPUT RESOLUTION (WINDOW SIZE)");
                    ImGui::TextDisabled("OS window size. The render FBO is upscaled to this by the active filter.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Current output: "); ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.30f, 0.90f, 0.50f, 1.0f), "%d x %d", g_win_w, g_win_h);
                    ImGui::Spacing();

                    struct OutPreset { const char* label; int w; int h; };
                    static const OutPreset out_presets[] = {
                        { "3840x2160 (4K)",  3840, 2160 }, { "2560x1440 (2K)",  2560, 1440 },
                        { "1920x1080",        1920, 1080 }, { "1600x900",         1600,  900 },
                        { "1280x720",         1280,  720 }, { "960x544 (native)",  960,  544 },
                    };
                    for (int oi = 0; oi < 6; oi++) {
                        if (oi % 3 != 0) ImGui::SameLine(0, 6);
                        ImGui::PushID(200 + oi);
                        if (ImGui::Button(out_presets[oi].label, ImVec2(210, 30))) {
                            if (m_window) {
                                SDL_SetWindowSize(m_window, out_presets[oi].w, out_presets[oi].h);
                                m_status_msg = std::string("Output: ") + out_presets[oi].label;
                                m_status_timer = 3.0f;
                            }
                        }
                        ImGui::PopID();
                    }

                    // Custom output resolution
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Custom output size:");
                    static int custom_ow = 1280, custom_oh = 720;
                    ImGui::SetNextItemWidth(120); ImGui::InputInt("W##cow", &custom_ow); ImGui::SameLine();
                    ImGui::SetNextItemWidth(120); ImGui::InputInt("H##coh", &custom_oh); ImGui::SameLine();
                    if (ImGui::Button("Apply Custom Output")) {
                        if (m_window) {
                            SDL_SetWindowSize(m_window,
                                std::max(320, std::min(custom_ow, 7680)),
                                std::max(240, std::min(custom_oh, 4320)));
                            m_status_msg = "Output: " + std::to_string(custom_ow) + "x" + std::to_string(custom_oh);
                            m_status_timer = 3.0f;
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // ── UPSCALE / SCALE FILTER ────────────────────────────────────────
                    ImGui::TextColored(ImVec4(0.00f, 0.85f, 1.00f, 1.0f), ICON_FA_LAYER_GROUP "  UPSCALE FILTER");
                    ImGui::TextDisabled("Algorithm used to upscale the render FBO to the output window.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    extern FBOScale g_fbo_mode;
                    const char* scale_labels[] = {
                        "Sharp Bilinear   (2-pass, anti-alias)",
                        "Nearest Neighbor (crisp pixel-art)",
                        "CRT Scanline     (retro CRT effect)",
                        "FSR 1.0          (AMD FidelityFX EASU)",
                    };
                    for (int si = 0; si < 4; si++) {
                        bool sel = (g_fbo_mode == static_cast<FBOScale>(si));
                        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
                        ImGui::PushID(300 + si);
                        if (ImGui::Selectable(scale_labels[si], sel)) {
                            g_fbo_mode = static_cast<FBOScale>(si);
                            m_status_msg = std::string("Scale: ") + scale_labels[si];
                            m_status_timer = 2.0f;
                        }
                        ImGui::PopID();
                        if (sel) ImGui::PopStyleColor();
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // ── POST-FX PRESETS ───────────────────────────────────────────────
                    ImGui::TextColored(ImVec4(0.00f, 0.85f, 1.00f, 1.0f), ICON_FA_WAND_SPARKLES "  POST-PROCESSING (PostFX)");
                    ImGui::TextDisabled("Visual effects applied after the game renders. Tuned for Swordigo's art style.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Preset selector
                    const char* preset_labels[] = {
                        "Off", "SW+ Medium", "SW+ High (PBR)", "Atmospheric",
                        "Ethereal", "Cinematic", "Retro", "Fantasy", "Noir", "Custom"
                    };
                    ImGui::Text("Preset:"); ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", g_postfx.preset_name);
                    ImGui::Spacing();

                    for (int pi = 0; pi < (int)PostFXPreset::COUNT; pi++) {
                        bool sel = ((int)g_postfx_preset == pi);
                        if (pi % 5 != 0) ImGui::SameLine(0, 6);
                        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.18f, 1.0f));
                        ImGui::PushID(400 + pi);
                        if (ImGui::Button(preset_labels[pi], ImVec2(138, 30))) {
                            g_postfx_preset = (PostFXPreset)pi;
                            postfx_apply_preset(g_postfx, g_postfx_preset);
                            m_status_msg = std::string("PostFX: ") + g_postfx.preset_name;
                            m_status_timer = 2.0f;
                        }
                        ImGui::PopID();
                        if (sel) ImGui::PopStyleColor();
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Fine-tune individual effects:");
                    ImGui::Spacing();

                    // Tier 1: color effects
                    ImGui::Checkbox("Vignette",    &g_postfx.vignette);
                    if (g_postfx.vignette) { ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                        ImGui::SliderFloat("##vig", &g_postfx.vignette_intensity, 0.0f, 1.0f, "Strength %.2f"); }

                    ImGui::Checkbox("Bloom",       &g_postfx.bloom);
                    if (g_postfx.bloom) {
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##blmi", &g_postfx.bloom_intensity, 0.0f, 1.0f, "Intensity %.2f");
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##blmt", &g_postfx.bloom_threshold, 0.0f, 1.0f, "Threshold %.2f");
                    }

                    ImGui::Checkbox("Film Grain",  &g_postfx.film_grain);
                    if (g_postfx.film_grain) { ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                        ImGui::SliderFloat("##grain", &g_postfx.grain_intensity, 0.0f, 0.5f, "Strength %.3f"); }

                    ImGui::Checkbox("Chromatic Aberration", &g_postfx.chromatic_aberration);
                    if (g_postfx.chromatic_aberration) { ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                        ImGui::SliderFloat("##ca", &g_postfx.ca_offset, 0.0f, 0.02f, "Offset %.4f"); }

                    ImGui::Checkbox("Sharpen", &g_postfx.sharpen);
                    if (g_postfx.sharpen) { ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                        ImGui::SliderFloat("##sharp", &g_postfx.sharpen_strength, 0.0f, 2.0f, "Strength %.2f"); }

                    ImGui::Checkbox("Color Adjust", &g_postfx.color_adjust);
                    if (g_postfx.color_adjust) {
                        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Saturation##sat", &g_postfx.saturation, 0.0f, 3.0f);
                        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Contrast##con",   &g_postfx.contrast,   0.0f, 3.0f);
                        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Brightness##bri", &g_postfx.brightness,-0.5f, 0.5f);
                        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Warmth##warm",    &g_postfx.warmth,    -1.0f, 1.0f);
                    }

                    ImGui::Spacing();
                    // Tier 2: advanced effects
                    ImGui::Checkbox("God Rays",    &g_postfx.god_rays);
                    if (g_postfx.god_rays) {
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##gri", &g_postfx.god_rays_intensity, 0.0f, 1.5f, "Inten %.2f");
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##grd", &g_postfx.god_rays_decay, 0.8f, 1.0f, "Decay %.3f");
                    }

                    ImGui::Checkbox("SSAO",        &g_postfx.ssao);
                    if (g_postfx.ssao) {
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##ssaoi", &g_postfx.ssao_intensity, 0.0f, 3.0f, "Inten %.2f");
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##ssaor", &g_postfx.ssao_radius, 0.0f, 0.1f, "Radius %.3f");
                    }

                    ImGui::Checkbox("Shadows",     &g_postfx.shadows);
                    if (g_postfx.shadows) {
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##shi", &g_postfx.shadow_intensity, 0.0f, 1.0f, "Dark %.2f");
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##shs", &g_postfx.shadow_softness, 0.0f, 0.02f, "Soft %.4f");
                    }

                    ImGui::Checkbox("Outlines",    &g_postfx.outlines);
                    if (g_postfx.outlines) {
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##olt", &g_postfx.outline_thickness, 0.5f, 4.0f, "Thick %.1f");
                        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
                        ImGui::SliderFloat("##oli", &g_postfx.outline_intensity, 0.0f, 1.0f, "Opac %.2f");
                    }

                    ImGui::Checkbox("Volumetric Light", &g_postfx.volumetric_light);
                    if (g_postfx.volumetric_light) { ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                        ImGui::SliderFloat("##voli", &g_postfx.volumetric_intensity, 0.0f, 1.0f, "Intensity %.2f"); }

                    ImGui::Spacing();
                    // Tier 3: Remaster
                    ImGui::Checkbox("PBR Shading",    &g_postfx.pbr_enabled);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cook-Torrance BRDF + LabPBR materials. High GPU cost.");
                    ImGui::SameLine();
                    ImGui::Checkbox("Water Reflections", &g_postfx.reflections_enabled);
                    if (g_postfx.reflections_enabled) { ImGui::SameLine(); ImGui::SetNextItemWidth(160);
                        ImGui::SliderFloat("##refi", &g_postfx.reflection_intensity, 0.0f, 1.0f, "Blend %.2f"); }

                    // Status line
                    if (m_status_timer > 0.0f) {
                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.30f, 0.90f, 0.50f, 1.0f), "\xE2\x9C\xA8 %s", m_status_msg.c_str());
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // Scene Shifter tab — ARM64 only feature
            // Scans mod + vanilla directories for .scene files and provides
            // Normal (loading screen) and Forced (instant) teleport gateways.
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem(ICON_FA_MAP " Scene Shifter")) {
                static bool   scene_list_loaded = false;
                static int    selected_scene     = -1;
                static char   spawn_buf[64]      = "start";
                static char   filter_buf[64]     = "";

                // Resolve guest memory pointers if initialized, otherwise fallback to weak globals
                int*   p_list_count  = (m_scene_shifter_ready && m_guest_memory) ? (int*)(m_guest_memory + m_ss_list_count_va) : &g_sre_scene_list_count;
                char (*p_list)[128]  = (m_scene_shifter_ready && m_guest_memory) ? (char(*)[128])(m_guest_memory + m_ss_list_va) : g_sre_scene_list;
                volatile int* p_pending = (m_scene_shifter_ready && m_guest_memory) ? (volatile int*)(m_guest_memory + m_ss_pending_va) : &g_sre_scene_shift_pending;
                char*  p_target      = (m_scene_shifter_ready && m_guest_memory) ? (char*)(m_guest_memory + m_ss_target_va) : g_sre_scene_shift_target;
                char*  p_spawn       = (m_scene_shifter_ready && m_guest_memory) ? (char*)(m_guest_memory + m_ss_spawn_va) : g_sre_scene_shift_spawn;
                char*  p_error       = (m_scene_shifter_ready && m_guest_memory) ? (char*)(m_guest_memory + m_ss_error_va) : g_sre_scene_shift_last_error;
                int*   p_active      = (m_scene_shifter_ready && m_guest_memory) ? (int*)(m_guest_memory + m_ss_active_va) : (int*)&g_sre_scene_shift_active;
                char*  p_cur_scene   = (m_scene_shifter_ready && m_guest_memory && m_ss_current_scene_va) ? (char*)(m_guest_memory + m_ss_current_scene_va) : g_sre_current_scene_name;

                auto do_rescan = [&]() {
                    if (m_scene_shifter_ready && !m_ss_assets_dir.empty()) {
                        namespace fs = std::filesystem;
                        *p_list_count = 0;
                        auto try_scan = [&](const fs::path& dir) {
                            if (!fs::exists(dir) || !fs::is_directory(dir)) return;
                            std::error_code ec;
                            for (auto& ent : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
                                if (*p_list_count >= 256) break;
                                if (!ent.is_regular_file(ec)) continue;
                                auto name = ent.path().filename().string();
                                if (name.size() <= 6 || name.substr(name.size() - 6) != ".scene") continue;
                                std::string stem = name.substr(0, name.size() - 6);
                                bool dup = false;
                                for (int i = 0; i < *p_list_count; i++) {
                                    if (std::string(p_list[i]) == stem) { dup = true; break; }
                                }
                                if (dup) continue;
                                strncpy(p_list[*p_list_count], stem.c_str(), 127);
                                p_list[*p_list_count][127] = '\0';
                                (*p_list_count)++;
                            }
                        };
                        try_scan(fs::path(m_ss_assets_dir) / "resources");
                        qsort(p_list, *p_list_count, 128, [](const void* a, const void* b) -> int {
                            return strcmp((const char*)a, (const char*)b);
                        });
                    } else {
                        sre_scene_shifter_scan_scenes();
                    }
                };

                if (!scene_list_loaded) {
                    do_rescan();
                    scene_list_loaded = true;
                }

                ImGui::Spacing();

                // ── Header row: current scene + controls ──────────────────
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f),
                                   ICON_FA_LOCATION_DOT " Current scene:");
                ImGui::SameLine();
                ImGui::TextUnformatted(p_cur_scene[0]
                                       ? p_cur_scene
                                       : "(unknown)");

                ImGui::SameLine(0, 20.0f);
                if (ImGui::SmallButton(ICON_FA_ARROWS_ROTATE " Refresh")) {
                    do_rescan();
                    scene_list_loaded = true;
                    selected_scene = -1;
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                   "%d scenes", *p_list_count);

                ImGui::Separator();
                ImGui::Spacing();

                // ── Filter + scene list ───────────────────────────────────
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##ss_filter", ICON_FA_MAGNIFYING_GLASS " Filter scenes...",
                                         filter_buf, sizeof(filter_buf));

                float list_h = ImGui::GetContentRegionAvail().y - 130.0f;
                if (list_h < 120.0f) list_h = 120.0f;

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.06f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.15f, 0.45f, 0.85f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.2f, 0.55f, 1.0f,  0.7f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.3f, 0.65f, 1.0f,  0.9f));

                if (ImGui::BeginChild("##scene_list", ImVec2(0, list_h), true)) {
                    if (*p_list_count == 0) {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                            "No scenes found.\nMake sure a mod is active or vanilla assets are accessible.\n"
                            "Press Refresh to scan again.");
                    } else {
                        for (int i = 0; i < *p_list_count; i++) {
                            const char* name = p_list[i];
                            // Apply filter
                            if (filter_buf[0] && !strstr(name, filter_buf)) continue;

                            // Highlight current scene
                            bool is_current = (p_cur_scene[0] &&
                                               strcmp(name, p_cur_scene) == 0);

                            if (is_current) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
                            }

                            char label[160];
                            snprintf(label, sizeof(label), "%s %s",
                                     is_current ? ICON_FA_LOCATION_DOT : "   ", name);

                            bool sel = (selected_scene == i);
                            if (ImGui::Selectable(label, sel,
                                    ImGuiSelectableFlags_AllowDoubleClick)) {
                                selected_scene = i;
                                // Double-click → fill spawn and set as target
                                if (ImGui::IsMouseDoubleClicked(0)) {
                                    strncpy(p_target, name, 127);
                                    p_target[127] = '\0';
                                }
                            }

                            if (is_current) ImGui::PopStyleColor();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor(4);

                ImGui::Spacing();

                // ── Spawn point input ─────────────────────────────────────
                ImGui::SetNextItemWidth(180.0f);
                ImGui::InputTextWithHint("##ss_spawn", "Spawn point (e.g. start)",
                                         spawn_buf, sizeof(spawn_buf));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), " \u2190 spawn");

                ImGui::Spacing();

                // ── Target display & teleport buttons ─────────────────────
                const char* target_name = (selected_scene >= 0 &&
                                           selected_scene < *p_list_count)
                                          ? p_list[selected_scene]
                                          : nullptr;

                bool can_tp = (target_name != nullptr &&
                               *p_pending == 0 &&
                               *p_active  == 0);

                if (target_name) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                                       ICON_FA_MAP_LOCATION_DOT " Target: %s", target_name);
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                       "(select a scene above)");
                }

                if (*p_active) {
                    ImGui::SameLine(0, 16.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.0f, 1.0f),
                                       ICON_FA_SPINNER " Loading...");
                }

                ImGui::Spacing();

                if (!can_tp) ImGui::BeginDisabled();

                // Normal Gateway — standard loading screen transition
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.45f, 0.15f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f,  0.2f,  1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.3f, 0.75f, 0.3f,  1.0f));
                bool do_normal = ImGui::Button(ICON_FA_PLAY " Normal Gateway", ImVec2(220.0f, 0));
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Calls GotoLevel() \u2192 shows loading screen.\n"
                                      "Standard transition, identical to entering a portal.");

                ImGui::SameLine(0, 12.0f);

                // Forced Gateway — instant 0-frame load
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.15f, 0.15f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f,  0.2f,  1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.3f,  0.3f,  1.0f));
                bool do_forced = ImGui::Button(ICON_FA_BOLT " Forced Gateway", ImVec2(220.0f, 0));
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Calls GotoLevel() for instant scene transition.\n"
                                      "Fast teleport gateway.");

                if (!can_tp) ImGui::EndDisabled();

                // Dispatch requests — set pending for sre_scene_shifter_tick()
                if (can_tp && target_name) {
                    if (do_normal || do_forced) {
                        strncpy(p_target, target_name, 127);
                        p_target[127] = '\0';
                        strncpy(p_spawn, spawn_buf, 63);
                        p_spawn[63] = '\0';
                        // Mode: 1=normal, 2=forced
                        *p_pending = do_forced ? 2 : 1;
                    }
                }

                // Status / last error
                if (p_error[0] && strcmp(p_error, "OK") != 0) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                       ICON_FA_TRIANGLE_EXCLAMATION " %s",
                                       p_error);
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Diagnostics & Logs")) {
                ImGui::Spacing();
                if (ImGui::Button("Clear Logs")) {
                    std::string log_file_path = save_dir + "/external/sre_lua_errors.log";
                    FILE* f = fopen(log_file_path.c_str(), "w");
                    if (f) fclose(f);
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Reading: external/sre_lua_errors.log");
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 0.5f));
                if (ImGui::BeginChild("##logs_child", ImVec2(0, 0), true)) {
                    std::string log_file_path = save_dir + "/external/sre_lua_errors.log";
                    FILE* f = fopen(log_file_path.c_str(), "r");
                    if (f) {
                        char buf[512];
                        while (fgets(buf, sizeof(buf), f)) {
                            size_t len = strlen(buf);
                            if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
                            if (strstr(buf, "ERROR") || strstr(buf, "exception") || strstr(buf, "failed")) {
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", buf);
                            } else {
                                ImGui::TextUnformatted(buf);
                            }
                        }
                        fclose(f);
                    } else {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No logs recorded yet.");
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) {
        m_mod_overlay_visible = false;
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(4);
}

bool SwordfareGUI::is_input_blocked(float mx, float my) {
    if (!m_initialized) return false;
    
    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    // 1. If companion mod overlay is open and mouse is inside it, block input
    if (m_mod_overlay_visible) {
        float panel_w = win_w * 0.70f;
        float panel_h = win_h * 0.75f;
        if (panel_w < 640.0f) panel_w = win_w * 0.95f;
        if (panel_h < 420.0f) panel_h = win_h * 0.95f;
        float px = (win_w - panel_w) / 2.0f;
        float py = (win_h - panel_h) / 2.0f;
        if (mx >= px && mx <= px + panel_w && my >= py && my <= py + panel_h) {
            return true;
        }
    }

    if (!m_last_buttons_ptr) return false;

    extern int GAME_W, GAME_H;
    float game_asp = (float)GAME_W / (float)GAME_H;
    float win_asp  = win_w / win_h;
    float vp_x = 0, vp_y = 0, vp_w = win_w, vp_h = win_h;
    
    if (win_asp > game_asp) {
        vp_w = win_h * game_asp;
        vp_x = (win_w - vp_w) / 2;
    } else {
        vp_h = win_w / game_asp;
        vp_y = (win_h - vp_h) / 2;
    }
    
    SreOverlaySlot* overlays = static_cast<SreOverlaySlot*>(m_last_overlays_ptr);
    SreBtnSlot* buttons = static_cast<SreBtnSlot*>(m_last_buttons_ptr);

    // 2. Block clicks inside any active overlay background region
    if (overlays) {
        for (int i = 0; i < SRE_OVERLAY_MAX; i++) {
            SreOverlaySlot& ovr = overlays[i];
            if (!ovr.active || ovr.hidden) continue;
            float ow = vp_w * ovr.w;
            float oh = vp_h * ovr.h;
            float ox = vp_x + vp_w * ovr.x - ow / 2.0f;
            float oy = vp_y + vp_h * ovr.y - oh / 2.0f;
            if (mx >= ox && mx <= ox + ow && my >= oy && my <= oy + oh) {
                return true; // Click is inside an overlay panel — always block
            }
        }
    }

    // 3. Check if coords hit any active clickable button
    if (buttons) {
        for (int i = 0; i < SRE_BTN_MAX; i++) {
            SreBtnSlot& btn = buttons[i];
            if (!btn.active || btn.hidden || !btn.clickable) continue;

            // Resolve parent overlay
            SreOverlaySlot* parent_ovr = nullptr;
            if (btn.overlay_id[0] != '\0' && overlays) {
                for (int o = 0; o < SRE_OVERLAY_MAX; o++) {
                    if (overlays[o].active && strcmp(overlays[o].id, btn.overlay_id) == 0) {
                        parent_ovr = &overlays[o];
                        break;
                    }
                }
                if (parent_ovr && (parent_ovr->hidden || !parent_ovr->active)) continue;
            }

            float pw = vp_w * btn.w * btn.scale_x;
            float ph = vp_h * btn.h * btn.scale_y;
            float bx, by;

            if (parent_ovr) {
                float ow = vp_w * parent_ovr->w;
                float oh = vp_h * parent_ovr->h;
                float ox = vp_x + vp_w * parent_ovr->x - ow / 2.0f;
                float oy = vp_y + vp_h * parent_ovr->y - oh / 2.0f;
                bx = ox + ow * btn.cur_x - pw / 2.0f;
                by = oy + oh * btn.cur_y - ph / 2.0f;
            } else {
                bx = vp_x + vp_w * btn.cur_x - pw / 2.0f;
                by = vp_y + vp_h * btn.cur_y - ph / 2.0f;
            }

            if (mx >= bx && mx <= bx + pw && my >= by && my <= by + ph) {
                return true;
            }
        }
    }
    
    return false;
}

extern SrtOverlay g_srt_overlay;

void SwordfareGUI::scan_saves(const std::string& save_dir) {
    g_srt_overlay.save_dir = get_vfs_save_dir(save_dir);
    g_srt_overlay.scan_saves();
    m_save_files = g_srt_overlay.save_files;
}

bool SwordfareGUI::load_save(const std::string& path) {
    bool ok = g_srt_overlay.load_save(path);
    if (ok) {
        m_inventory = g_srt_overlay.inventory;
        m_inventory_dirty = false;
    }
    return ok;
}

bool SwordfareGUI::write_save(const std::string& path) {
    g_srt_overlay.inventory = m_inventory;
    bool ok = g_srt_overlay.write_save(path);
    if (ok) {
        m_inventory_dirty = false;
    }
    return ok;
}

// ============================================================================
//  Swordfare GUI — Remastered Command Overlay (v2)
// ----------------------------------------------------------------------------
//  Drop-in replacement for SwordfareGUI::draw_control_panel().
//
//  WHAT CHANGED VS THE OLD VERSION
//  --------------------------------
//  1) Bar now docks to the TOP of the screen, not the bottom.
//  2) The old code pushed one single Header/HeaderHovered color pair before
//     Begin() and left it active for the *entire* window — every top-level
//     menu button and every item inside every dropdown shared the exact same
//     solid, fully-opaque highlight block. That's why clicking one button
//     made the whole section look "selected": there was nothing visually
//     distinguishing hover-state from open-state from click-state, they all
//     rendered the same full-width solid rectangle.
//     Fixed by: giving each state its own color (subtle hover tint, a
//     slightly stronger but still translucent active tint, and an underline
//     accent for the currently-open menu) instead of one flat opaque block.
//  3) Split into a real header bar (branding + top-level menus + status +
//     close) plus dedicated modal-style panels for About / Help, so the bar
//     itself stays razor-thin and everything heavy lives in popups.
//  4) All existing GuiAction values are preserved 1:1 — no gameplay-facing
//     capability was removed, only reorganized and restyled. Nothing new
//     was added to the GuiAction enum.
//
//  HEADER FILE — please add these two members to the SwordfareGUI class:
//      bool m_show_about = false;
//      bool m_show_help  = false;
//  (They're purely UI state for the new About/Help panels below and don't
//  need a GuiAction — the panels are opened/closed entirely inside this file.)
//
//  Everything else (m_initialized, m_imgui_ctx, m_font_button, GuiRenderer,
//  g_game_paused, GuiAction enum) is used exactly as it was declared before.
// ============================================================================

#include "imgui.h"
#include "imgui_internal.h"   // only used for ImGui::PushItemFlag-style tweaks; drop if you don't have it

// ---------------------------------------------------------------------------
// Brand palette — lifted from the project's own README badges (#00e5ff cyan,
// #8b3dff purple) so the overlay visually matches the rest of the project.
// ---------------------------------------------------------------------------
namespace SwordfareTheme {
    constexpr ImVec4 kBg          = ImVec4(0.043f, 0.047f, 0.063f, 0.97f); // near-black glass
    constexpr ImVec4 kBgPopup     = ImVec4(0.055f, 0.063f, 0.086f, 0.99f);
    constexpr ImVec4 kBorder      = ImVec4(0.30f,  0.30f,  0.36f,  0.35f);
    constexpr ImVec4 kCyan        = ImVec4(0.00f,  0.898f, 1.00f,  1.00f); // #00e5ff
    constexpr ImVec4 kPurple      = ImVec4(0.545f, 0.239f, 1.00f,  1.00f); // #8b3dff
    constexpr ImVec4 kText        = ImVec4(0.90f,  0.92f,  0.96f,  1.00f);
    constexpr ImVec4 kTextDim     = ImVec4(0.55f,  0.58f,  0.65f,  1.00f);
    constexpr ImVec4 kHoverTint   = ImVec4(1.00f,  1.00f,  1.00f,  0.07f); // very light — no more solid blocks
    constexpr ImVec4 kActiveTint  = ImVec4(0.00f,  0.898f, 1.00f,  0.16f);
    constexpr ImVec4 kDanger      = ImVec4(0.95f,  0.32f,  0.36f,  1.00f);
    constexpr ImVec4 kDangerHover = ImVec4(0.80f,  0.16f,  0.20f,  0.55f);
    constexpr ImVec4 kOk          = ImVec4(0.30f,  0.85f,  0.55f,  1.00f);
}

// Small helper: draws a 1px cyan->purple gradient line under a rect, used
// under the whole bar and under the active menu to give it a "current tab"
// accent instead of a filled highlight block.
static void DrawAccentUnderline(ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c0 = ImGui::ColorConvertFloat4ToU32(SwordfareTheme::kCyan);
    ImU32 c1 = ImGui::ColorConvertFloat4ToU32(SwordfareTheme::kPurple);
    dl->AddLine(p0, ImVec2(p1.x, p0.y), c0, 1.5f);
    dl->AddRectFilledMultiColor(p0, p1, c0, c1, c1, c0);
}

// ============================================================================
//  TOP HEADER BAR  —  File · Tools · Cheats · Speed · Camera · Audio · About · Help
// ============================================================================
GuiAction SwordfareGUI::draw_control_panel(bool* p_open) {
    if (!m_initialized || !*p_open) return GUI_NONE;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    GuiAction action = GUI_NONE;
    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    extern GuiRenderer g_gui;
    extern bool g_game_paused;

    float scale = win_h / 720.0f;
    if (scale < 1.0f) scale = 1.0f;

    // Modern clean dark charcoal palette
    ImVec4 kBg          = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    ImVec4 kBgPopup     = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    ImVec4 kBorder      = ImVec4(0.20f, 0.22f, 0.26f, 0.50f);
    ImVec4 kText        = ImVec4(0.85f, 0.88f, 0.92f, 1.00f);
    ImVec4 kCyan        = ImVec4(0.00f, 0.55f, 1.00f, 1.00f); // Bright blue/cyan matching the image

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 3.5f * scale)); // reduced height and padding
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14.0f * scale, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f);

    ImGui::PushStyleColor(ImGuiCol_MenuBarBg,     kBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       kBgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border,        kBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,          kText);
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.00f, 0.55f, 1.00f, 0.20f)); // active tint
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.55f, 1.00f, 0.35f)); // hover tint
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.00f, 0.55f, 1.00f, 0.50f));

    ImFont* bar_font = static_cast<ImFont*>(m_font_button);
    float font_size = 9.5f * scale; // compact font size
    if (bar_font) ImGui::PushFont(bar_font);

    float bar_h = 24.0f * scale;

    if (ImGui::BeginMainMenuBar()) {
        bar_h = ImGui::GetWindowHeight();

        auto menu = [&](const char* label, auto fn) {
            if (ImGui::BeginMenu(label)) {
                fn();
                ImGui::EndMenu();
            }
        };

        // ── File ─────────────────────────────────────────────────────────
        menu("File", [&]() {
            if (ImGui::MenuItem("Save State"))        action = GUI_SAVE_STATE;
            if (ImGui::MenuItem("Load State"))        action = GUI_LOAD_STATE;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit Game"))         action = GUI_EXIT;
        });

        // ── Emulation ────────────────────────────────────────────────────
        menu("Emulation", [&]() {
            if (ImGui::MenuItem(g_game_paused ? "Resume" : "Pause", "F8"))
                action = GUI_PAUSE;
            ImGui::Separator();
            if (ImGui::MenuItem("Speed Up",   "+")) action = GUI_GAME_SPEED_UP;
            if (ImGui::MenuItem("Speed Down", "-")) action = GUI_GAME_SPEED_DOWN;
            if (ImGui::MenuItem("Speed Reset","0")) action = GUI_GAME_SPEED_RESET;
        });

        // ── Config ───────────────────────────────────────────────────────
        menu("Config", [&]() {
            if (ImGui::MenuItem("Customize Controls", "F2")) action = GUI_CUSTOMIZE_CONTROLS;
        });

        // ── Mods ─────────────────────────────────────────────────────────
        menu("Mods", [&]() {
            ImGui::TextDisabled("CHEAT TOGGLES");
            ImGui::Separator();
            ImGui::MenuItem("God Mode",       nullptr, &g_gui.mod_god_mode);
            ImGui::MenuItem("Infinite Mana",  nullptr, &g_gui.mod_infinite_mana);
            ImGui::MenuItem("Fly Mode",       nullptr, &g_gui.mod_fly_mode);
            ImGui::MenuItem("Infinite Jump",  nullptr, &g_gui.mod_infinite_jump);
            ImGui::MenuItem("Coin Break",     nullptr, &g_gui.mod_coin_break);
            
            ImGui::Separator();
            
            if (ImGui::BeginMenu("Stats Editor")) {
                if (ImGui::MenuItem("Heal Full HP"))  action = GUI_MOD_HEAL_FULL;
                if (ImGui::MenuItem("Refill Mana"))   action = GUI_MOD_REFILL_MANA;
                if (ImGui::MenuItem("+100 Coins"))    action = GUI_MOD_ADD_COINS;
                ImGui::Separator();
                if (ImGui::MenuItem("Level Up (+1)")) action = GUI_MOD_LEVEL_UP;
                if (ImGui::MenuItem("Level Down (-1)")) action = GUI_MOD_LEVEL_DOWN;
                if (ImGui::MenuItem("XP +500"))       action = GUI_MOD_EXP_UP;
                if (ImGui::MenuItem("XP -500"))       action = GUI_MOD_EXP_DOWN;
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Physics Editor")) {
                ImGui::SetNextItemWidth(120.0f * scale);
                ImGui::SliderFloat("Walk Speed", &g_gui.mod_walk_speed,  0.5f, 10.0f, "%.1f");
                ImGui::SetNextItemWidth(120.0f * scale);
                ImGui::SliderFloat("Run Speed",  &g_gui.mod_run_speed,   0.5f, 10.0f, "%.1f");
                ImGui::SetNextItemWidth(120.0f * scale);
                ImGui::SliderFloat("Jump Height", &g_gui.mod_jump_height, 0.5f, 10.0f, "%.1f");
                ImGui::EndMenu();
            }
        });

        // ── Settings ─────────────────────────────────────────────────────
        menu("Settings", [&]() {
            if (ImGui::MenuItem("Toggle Cam Override", "F5")) action = GUI_TOGGLE_CAM;
            if (ImGui::MenuItem("Toggle Smooth Cam"))          action = GUI_TOGGLE_SMOOTH_CAM;
            ImGui::Separator();
            if (ImGui::MenuItem("Mute Music"))     action = GUI_MUSIC_MUTE;
            if (ImGui::MenuItem("Volume Up"))      action = GUI_MUSIC_VOL_UP;
            if (ImGui::MenuItem("Volume Down"))    action = GUI_MUSIC_VOL_DOWN;
        });

        // ── Help ─────────────────────────────────────────────────────────
        menu("Help", [&]() {
            if (ImGui::MenuItem("Help / Hotkeys")) m_show_help = true;
            if (ImGui::MenuItem("About Mod"))      m_show_about = true;
        });

        // ── Right-aligned branding "Swordigo" ────────────────────────────
        const char* brand_lbl = "Swordigo";
        ImVec2 brand_sz = ImGui::CalcTextSize(brand_lbl);
        float brand_x = win_w - brand_sz.x - 12.0f * scale;

        ImGui::SameLine(brand_x);
        ImGui::TextColored(kCyan, "%s", brand_lbl);

        ImGui::EndMainMenuBar();
    }

    if (bar_font) ImGui::PopFont();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(3);

    // Draw the About/Help panels (self-contained, opened via the buttons above)
    if (m_show_about) draw_about_panel(&m_show_about, bar_h);
    if (m_show_help)  draw_help_panel(&m_show_help, bar_h);

    return action;
}



void SwordfareGUI::draw_lua_script_editor() {
    if (!m_initialized || !m_script_editor_open) return;

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;
    float editor_w = W * 0.7f;
    float editor_h = H * 0.6f;

    ImGui::SetNextWindowPos(ImVec2(W * 0.15f, H * 0.15f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(editor_w, editor_h),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.98f);

    // Styling matches the console
    const ImVec4 col_bg        = ImVec4(0.040f, 0.040f, 0.060f, 1.00f);
    const ImVec4 col_border    = ImVec4(0.180f, 0.220f, 0.380f, 1.00f);
    const ImVec4 col_input_bg  = ImVec4(0.020f, 0.020f, 0.035f, 1.00f);
    const ImVec4 col_accent    = ImVec4(0.36f,  0.76f,  1.00f,  1.00f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, col_bg);
    ImGui::PushStyleColor(ImGuiCol_Border, col_border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));

    bool pushed_mono = false;
    if (m_font_mono) { ImGui::PushFont((ImFont*)m_font_mono); pushed_mono = true; }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin(ICON_FA_CODE " Lua Script Editor", &m_script_editor_open, flags)) {
        
        // ── Top Bar ────────────────────────────────────────────────────────
        ImGui::TextColored(col_accent, ICON_FA_FILE " Filename:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(editor_w - 300.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, col_input_bg);
        ImGui::InputText("##filename", m_script_editor_name, sizeof(m_script_editor_name));
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(m_script_editor_status_color[0], m_script_editor_status_color[1], m_script_editor_status_color[2], m_script_editor_status_color[3]), "[ %s ]", m_script_editor_status.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Editor ─────────────────────────────────────────────────────────
        float content_avail_y = ImGui::GetContentRegionAvail().y;
        float footer_h = ImGui::GetFrameHeightWithSpacing() + 16.0f; // extra padding to stop clipping
        float editor_area_h = content_avail_y - footer_h;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, col_input_bg);
        ImGui::InputTextMultiline("##source", m_script_editor_buf, sizeof(m_script_editor_buf), 
                                  ImVec2(-FLT_MIN, editor_area_h), 
                                  ImGuiInputTextFlags_AllowTabInput);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Footer ─────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.36f, 0.72f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.95f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.22f, 0.52f, 1.00f));
        
        if (ImGui::Button(" " ICON_FA_FLOPPY_DISK " Save Script ")) {
            const char* home = std::getenv("HOME");
            if (home) {
                std::filesystem::path save_dir = std::filesystem::path(home) / ".local" / "share" / "swordigo-desktop" / "lua_scripts";
                std::error_code ec;
                std::filesystem::create_directories(save_dir, ec);
                
                std::filesystem::path file_path = save_dir / m_script_editor_name;
                std::ofstream out(file_path);
                if (out.is_open()) {
                    out << m_script_editor_buf;
                    out.close();
                    m_script_editor_status = "Saved OK";
                    m_script_editor_status_color[0] = 0.2f; m_script_editor_status_color[1] = 0.9f; m_script_editor_status_color[2] = 0.2f;
                } else {
                    m_script_editor_status = "Save Failed!";
                    m_script_editor_status_color[0] = 0.9f; m_script_editor_status_color[1] = 0.2f; m_script_editor_status_color[2] = 0.2f;
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(" " ICON_FA_CIRCLE_CHECK " Validate & Save ")) {
            // Heuristic syntax validator
            int brackets_sq = 0, brackets_curl = 0, brackets_paren = 0;
            int quotes_single = 0, quotes_double = 0;
            bool in_comment = false;
            
            std::string code = m_script_editor_buf;
            for (size_t i = 0; i < code.length(); i++) {
                if (in_comment && code[i] == '\n') in_comment = false;
                if (!in_comment && i + 1 < code.length() && code[i] == '-' && code[i+1] == '-') { in_comment = true; i++; continue; }
                if (in_comment) continue;

                if (code[i] == '[') brackets_sq++;
                if (code[i] == ']') brackets_sq--;
                if (code[i] == '{') brackets_curl++;
                if (code[i] == '}') brackets_curl--;
                if (code[i] == '(') brackets_paren++;
                if (code[i] == ')') brackets_paren--;
                if (code[i] == '\'') quotes_single++;
                if (code[i] == '"') quotes_double++;
            }

            if (brackets_sq != 0 || brackets_curl != 0 || brackets_paren != 0 || 
               (quotes_single % 2) != 0 || (quotes_double % 2) != 0) {
                m_script_editor_status = "Syntax Error: Unmatched Bracket/Quote";
                m_script_editor_status_color[0] = 0.9f; m_script_editor_status_color[1] = 0.2f; m_script_editor_status_color[2] = 0.2f;
            } else {
                // Syntax OK, proceed to save
                const char* home = std::getenv("HOME");
                if (home) {
                    std::filesystem::path save_dir = std::filesystem::path(home) / ".local" / "share" / "swordigo-desktop" / "lua_scripts";
                    std::error_code ec;
                    std::filesystem::create_directories(save_dir, ec);
                    std::ofstream out(save_dir / m_script_editor_name);
                    if (out.is_open()) {
                        out << m_script_editor_buf;
                        out.close();
                        m_script_editor_status = "Validated & Saved";
                        m_script_editor_status_color[0] = 0.2f; m_script_editor_status_color[1] = 0.9f; m_script_editor_status_color[2] = 0.9f;
                    }
                }
            }
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.72f, 0.36f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.95f, 0.50f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.52f, 0.22f, 1.00f));
        if (ImGui::Button(" " ICON_FA_PLAY " Run ")) {
            console_submit(m_script_editor_buf);
            m_script_editor_status = "Executed";
            m_script_editor_status_color[0] = 0.2f; m_script_editor_status_color[1] = 0.9f; m_script_editor_status_color[2] = 0.9f;
        }
        ImGui::PopStyleColor(3);
        
        ImGui::PopStyleColor(3);

        ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
        if (ImGui::Button(" " ICON_FA_XMARK " Close ")) {
            m_script_editor_open = false;
        }

    }
    ImGui::End();

    if (pushed_mono) ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void SwordfareGUI::draw_lua_script_manager() {
    if (!m_initialized || !m_script_manager_open) return;

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;
    
    ImGui::SetNextWindowPos(ImVec2(W * 0.15f, H * 0.15f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(W * 0.4f, H * 0.6f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.98f);

    const ImVec4 col_bg        = ImVec4(0.040f, 0.040f, 0.060f, 1.00f);
    const ImVec4 col_border    = ImVec4(0.180f, 0.220f, 0.380f, 1.00f);
    const ImVec4 col_accent    = ImVec4(0.36f,  0.76f,  1.00f,  1.00f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, col_bg);
    ImGui::PushStyleColor(ImGuiCol_Border, col_border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));

    if (ImGui::Begin(ICON_FA_FILE " Script Manager", &m_script_manager_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        
        if (ImGui::Button("Refresh")) {
            m_script_list.clear();
            const char* home = std::getenv("HOME");
            if (home) {
                std::filesystem::path save_dir = std::filesystem::path(home) / ".local" / "share" / "swordigo-desktop" / "lua_scripts";
                std::error_code ec;
                if (std::filesystem::exists(save_dir, ec)) {
                    for (const auto& entry : std::filesystem::directory_iterator(save_dir, ec)) {
                        if (entry.path().extension() == ".lua") {
                            // Run validation
                            std::ifstream in(entry.path());
                            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                            
                            int brackets_sq = 0, brackets_curl = 0, brackets_paren = 0;
                            int quotes_single = 0, quotes_double = 0;
                            bool in_comment = false;
                            
                            for (size_t i = 0; i < content.length(); i++) {
                                if (in_comment && content[i] == '\n') in_comment = false;
                                if (!in_comment && i + 1 < content.length() && content[i] == '-' && content[i+1] == '-') { in_comment = true; i++; continue; }
                                if (in_comment) continue;
                                if (content[i] == '[') brackets_sq++;
                                if (content[i] == ']') brackets_sq--;
                                if (content[i] == '{') brackets_curl++;
                                if (content[i] == '}') brackets_curl--;
                                if (content[i] == '(') brackets_paren++;
                                if (content[i] == ')') brackets_paren--;
                                if (content[i] == '\'') quotes_single++;
                                if (content[i] == '"') quotes_double++;
                            }
                            
                            bool valid = (brackets_sq == 0 && brackets_curl == 0 && brackets_paren == 0 && (quotes_single % 2) == 0 && (quotes_double % 2) == 0);
                            m_script_list.push_back({entry.path().filename().string(), valid});
                        }
                    }
                }
            }
        }
        
        ImGui::SameLine();
        ImGui::TextColored(col_accent, " %d scripts found", (int)m_script_list.size());
        
        ImGui::Separator();
        
        ImGui::BeginChild("##scriptlist", ImVec2(0, 0), true);
        for (const auto& script : m_script_list) {
            ImGui::PushID(script.filename.c_str());
            if (script.valid) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), ICON_FA_CIRCLE_CHECK " Passed");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), ICON_FA_CIRCLE_XMARK " Error ");
            }
            ImGui::SameLine();
            ImGui::Text("%s", script.filename.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 150.0f);
            
            if (ImGui::Button("Edit")) {
                const char* home = std::getenv("HOME");
                if (home) {
                    std::filesystem::path file_path = std::filesystem::path(home) / ".local" / "share" / "swordigo-desktop" / "lua_scripts" / script.filename;
                    std::ifstream in(file_path);
                    if (in.is_open()) {
                        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        strncpy(m_script_editor_name, script.filename.c_str(), sizeof(m_script_editor_name)-1);
                        strncpy(m_script_editor_buf, content.c_str(), sizeof(m_script_editor_buf)-1);
                        m_script_editor_open = true;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Run")) {
                const char* home = std::getenv("HOME");
                if (home) {
                    std::filesystem::path file_path = std::filesystem::path(home) / ".local" / "share" / "swordigo-desktop" / "lua_scripts" / script.filename;
                    std::ifstream in(file_path);
                    if (in.is_open()) {
                        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        console_submit(content);
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ============================================================================
//  ABOUT PANEL — pulled from the project README, written up fresh for the UI
// ============================================================================
void SwordfareGUI::draw_about_panel(bool* p_open, float top_offset) {
    using namespace SwordfareTheme;
    ImGui::SetNextWindowPos(ImVec2(60.0f, top_offset + 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480.0f, 420.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kBg);

    if (ImGui::Begin("About Swordigo Desktop", p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(kCyan, "Swordigo Desktop");
        ImGui::TextColored(kTextDim, "The Swordigo Runtime (SRT) — v7.3 \"Combatch\"");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "A native Linux port of the mobile action-adventure platformer, "
            "built around a layered runtime that treats the original ARM "
            "game binary as a gameplay kernel while progressively swapping "
            "its subsystems for clean, native C reimplementations.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(kCyan, "This overlay");
        ImGui::BulletText("Rendering, music, HUD, backgrounds and save/load");
        ImGui::BulletText("are handled by libsre.so, a from-scratch native");
        ImGui::BulletText("subsystem layer sitting above the original binary.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(kCyan, "Core Team");
        ImGui::BulletText("Lead Developer — TheMegineBraine");
        ImGui::BulletText("Developers — TheCorrectSynovian, MrSinup, X Dukinja");
        ImGui::BulletText("Designer — ETPV");
        ImGui::Spacing();
        ImGui::TextColored(kTextDim,
            "Swordigo is (c) Ville Makynen / Touch Foo. This project ships");
        ImGui::TextColored(kTextDim,
            "no original assets or binaries and is a research/preservation");
        ImGui::TextColored(kTextDim, "effort only.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(90, 28))) *p_open = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
}

// ============================================================================
//  HELP PANEL — hotkey reference
// ============================================================================
void SwordfareGUI::draw_help_panel(bool* p_open, float top_offset) {
    using namespace SwordfareTheme;
    ImGui::SetNextWindowPos(ImVec2(560.0f, top_offset + 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 380.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kBg);

    static const struct { const char* key; const char* desc; } kHotkeys[] = {
        {"F1",  "Toggle this header / command overlay"},
        {"F2",  "Controls Editor (drag to reposition buttons)"},
        {"F3",  "Debug overlay (FPS, draw calls, player stats)"},
        {"F4",  "Cycle scaling modes"},
        {"F5",  "Camera override toggle"},
        {"F6",  "Cycle PostFX presets"},
        {"F7",  "Toggle video background playback"},
        {"F8",  "Pause / Resume"},
        {"F10", "Toggle native on-screen controls"},
        {"F12", "Fullscreen toggle"},
        {"\\",  "Toggle keyboard typing mode"},
    };

    if (ImGui::Begin("Hotkeys", p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(kCyan, "Keyboard Shortcuts");
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::BeginTable("hotkeys_tbl", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (auto& hk : kHotkeys) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(kCyan, "%s", hk.key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(kText, "%s", hk.desc);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(90, 28))) *p_open = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
}

/*
GuiAction SwordfareGUI::draw_control_panel(bool* p_open) {
    if (!m_initialized || !*p_open) return GUI_NONE;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    GuiAction action = GUI_NONE;
    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    extern GuiRenderer g_gui;
    extern bool g_game_paused;

    // Scale bar height with display size (32px logical minimum)
    float scale  = win_h / 544.0f;
    if (scale < 1.0f) scale = 1.0f;
    float bar_h  = 32.0f * scale;

    // ---------------------------------------------------------------
    // Bottom menu bar window (no title, no scrollbar, no resize)
    // ---------------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(0.0f, win_h - bar_h));
    ImGui::SetNextWindowSize(ImVec2(win_w, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.090f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,  ImVec4(0.068f, 0.078f, 0.110f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.18f,  0.54f,  0.82f,  0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.38f, 0.68f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.14f, 0.30f, 0.58f, 0.90f));

    // Scale font for this bar
    ImFont* bar_font = static_cast<ImFont*>(m_font_button);
    float font_size  = 13.0f * scale;
    if (bar_font) ImGui::PushFont(bar_font);

    ImGui::Begin("##sfw_menubar", nullptr,
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoScrollbar  |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Vertically center content in bar
    float text_h = font_size;
    float pad_y  = (bar_h - text_h) * 0.5f;
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);

    // Helper: begin a popup menu item in the bar
    auto bar_menu = [&](const char* label, auto fn) {
        if (ImGui::BeginMenu(label)) {
            fn();
            ImGui::EndMenu();
        }
    };

    // ── Game ──────────────────────────────────────────────────────
    bar_menu(g_game_paused ? "Game [PAUSED]" : "Game", [&]() {
        if (ImGui::MenuItem(g_game_paused ? "Resume" : "Pause", "F8"))
            action = GUI_PAUSE;
        ImGui::Separator();
        if (ImGui::MenuItem("Speed Up",   "+")) action = GUI_GAME_SPEED_UP;
        if (ImGui::MenuItem("Speed Down", "-")) action = GUI_GAME_SPEED_DOWN;
        if (ImGui::MenuItem("Speed Reset","0")) action = GUI_GAME_SPEED_RESET;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit Game"))       action = GUI_EXIT;
    });

    ImGui::SameLine();

    // ── Cheats ────────────────────────────────────────────────────
    bar_menu("Cheats", [&]() {
        ImGui::MenuItem(g_gui.mod_god_mode      ? "[ON]  God Mode"      : "[OFF] God Mode",      nullptr, &g_gui.mod_god_mode);
        ImGui::MenuItem(g_gui.mod_infinite_mana ? "[ON]  Infinite Mana" : "[OFF] Infinite Mana", nullptr, &g_gui.mod_infinite_mana);
        ImGui::MenuItem(g_gui.mod_fly_mode      ? "[ON]  Fly Mode"      : "[OFF] Fly Mode",      nullptr, &g_gui.mod_fly_mode);
        ImGui::MenuItem(g_gui.mod_infinite_jump ? "[ON]  Infinite Jump" : "[OFF] Infinite Jump", nullptr, &g_gui.mod_infinite_jump);
        ImGui::MenuItem(g_gui.mod_coin_break    ? "[ON]  Coin Break"    : "[OFF] Coin Break",    nullptr, &g_gui.mod_coin_break);
        ImGui::Separator();
        if (ImGui::MenuItem("Heal Full HP"))    action = GUI_MOD_HEAL_FULL;
        if (ImGui::MenuItem("Refill Mana"))     action = GUI_MOD_REFILL_MANA;
        if (ImGui::MenuItem("+100 Coins"))      action = GUI_MOD_ADD_COINS;
        ImGui::Separator();
        if (ImGui::MenuItem("Level +1"))        action = GUI_MOD_LEVEL_UP;
        if (ImGui::MenuItem("Level -1"))        action = GUI_MOD_LEVEL_DOWN;
        if (ImGui::MenuItem("XP +500"))         action = GUI_MOD_EXP_UP;
        if (ImGui::MenuItem("XP -500"))         action = GUI_MOD_EXP_DOWN;
    });

    ImGui::SameLine();

    // ── Speed ─────────────────────────────────────────────────────
    bar_menu("Speed", [&]() {
        ImGui::SetNextItemWidth(160.0f * scale);
        ImGui::SliderFloat("Walk", &g_gui.mod_walk_speed,   0.5f, 10.0f, "%.1f");
        ImGui::SetNextItemWidth(160.0f * scale);
        ImGui::SliderFloat("Run",  &g_gui.mod_run_speed,    0.5f, 10.0f, "%.1f");
        ImGui::SetNextItemWidth(160.0f * scale);
        ImGui::SliderFloat("Jump", &g_gui.mod_jump_height,  0.5f, 10.0f, "%.1f");
        ImGui::Separator();
        if (ImGui::MenuItem("Speed Up"))   action = GUI_GAME_SPEED_UP;
        if (ImGui::MenuItem("Speed Down")) action = GUI_GAME_SPEED_DOWN;
        if (ImGui::MenuItem("Speed Reset"))action = GUI_GAME_SPEED_RESET;
    });

    ImGui::SameLine();

    // ── Camera ────────────────────────────────────────────────────
    bar_menu("Camera", [&]() {
        if (ImGui::MenuItem("Toggle Cam Override", "F5"))  action = GUI_TOGGLE_CAM;
        if (ImGui::MenuItem("Toggle Smooth Cam"))          action = GUI_TOGGLE_SMOOTH_CAM;
    });

    ImGui::SameLine();

    // ── Audio ─────────────────────────────────────────────────────
    bar_menu("Audio", [&]() {
        if (ImGui::MenuItem("Mute / Unmute"))  action = GUI_MUSIC_MUTE;
        if (ImGui::MenuItem("Volume Up"))      action = GUI_MUSIC_VOL_UP;
        if (ImGui::MenuItem("Volume Down"))    action = GUI_MUSIC_VOL_DOWN;
    });

    ImGui::SameLine();

    // ── Controls ──────────────────────────────────────────────────
    bar_menu("Controls", [&]() {
        if (ImGui::MenuItem("Customize Controls", "F2")) action = GUI_CUSTOMIZE_CONTROLS;
        if (ImGui::MenuItem("Save State"))               action = GUI_SAVE_STATE;
        if (ImGui::MenuItem("Load State"))               action = GUI_LOAD_STATE;
    });

    // Right-aligned close button
    const char* close_lbl = " [F1] Close ";
    ImVec2 close_sz = ImGui::CalcTextSize(close_lbl);
    ImGui::SetCursorPosX(win_w - close_sz.x - 10.0f * scale);
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.14f, 0.20f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.18f, 0.22f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.18f, 0.22f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.95f, 0.38f, 0.42f, 1.00f));
    if (ImGui::SmallButton(close_lbl)) *p_open = false;
    ImGui::PopStyleColor(4);

    ImGui::End();

    if (bar_font) ImGui::PopFont();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);

    return action;
}

GuiAction SwordfareGUI::draw_settings_panel(bool* p_open) {
    if (!m_initialized || !*p_open) return GUI_NONE;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    GuiAction action = GUI_NONE;

    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(450.0f, 500.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.086f, 0.106f, 0.133f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.54f, 0.82f, 0.60f)); // Blue theme for settings
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.086f, 0.106f, 0.133f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.086f, 0.106f, 0.133f, 1.0f));

    extern GuiRenderer g_gui;

    if (ImGui::Begin("Swordfare Control Panel (F1)", p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.18f, 0.54f, 0.82f, 1.0f), "GAMEPLAY CHEATS");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox("God Mode (Invincible)", &g_gui.mod_god_mode);
        ImGui::Checkbox("Infinite Mana", &g_gui.mod_infinite_mana);
        ImGui::Checkbox("Fly Mode (Noclip-ish)", &g_gui.mod_fly_mode);
        ImGui::Checkbox("Infinite Jump", &g_gui.mod_infinite_jump);
        ImGui::Checkbox("Coin Break (Insta-break objects)", &g_gui.mod_coin_break);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.18f, 0.54f, 0.82f, 1.0f), "MOVEMENT & PHYSICS MULTIPLIERS");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SliderFloat("Walk Speed", &g_gui.mod_walk_speed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("Run Speed", &g_gui.mod_run_speed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("Jump Height", &g_gui.mod_jump_height, 0.5f, 10.0f, "%.1f");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.18f, 0.54f, 0.82f, 1.0f), "QUICK ACTIONS");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Heal Full HP", ImVec2(130, 30))) action = GUI_MOD_HEAL_FULL;
        ImGui::SameLine();
        if (ImGui::Button("Refill Mana", ImVec2(130, 30))) action = GUI_MOD_REFILL_MANA;
        ImGui::SameLine();
        if (ImGui::Button("+100 Coins", ImVec2(130, 30))) action = GUI_MOD_ADD_COINS;

        ImGui::Spacing();
        if (ImGui::Button("Level Up (+1)", ImVec2(130, 30))) action = GUI_MOD_LEVEL_UP;
        ImGui::SameLine();
        if (ImGui::Button("Level Down (-1)", ImVec2(130, 30))) action = GUI_MOD_LEVEL_DOWN;

        ImGui::Spacing();
        if (ImGui::Button("XP +500", ImVec2(130, 30))) action = GUI_MOD_EXP_UP;
        ImGui::SameLine();
        if (ImGui::Button("XP -500", ImVec2(130, 30))) action = GUI_MOD_EXP_DOWN;

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.18f, 0.54f, 0.82f, 1.0f), "AUDIO & EMULATION SETTINGS");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Mute Music", ImVec2(130, 30))) action = GUI_MUSIC_MUTE;
        ImGui::SameLine();
        if (ImGui::Button("Volume Up", ImVec2(130, 30))) action = GUI_MUSIC_VOL_UP;
        ImGui::SameLine();
        if (ImGui::Button("Volume Down", ImVec2(130, 30))) action = GUI_MUSIC_VOL_DOWN;

        ImGui::Spacing();
        if (ImGui::Button("Pause/Resume Game", ImVec2(160, 30))) action = GUI_PAUSE;
        ImGui::SameLine();
        if (ImGui::Button("Customize Controls", ImVec2(160, 30))) action = GUI_CUSTOMIZE_CONTROLS;

        ImGui::Spacing();
        if (ImGui::Button("Toggle Cam Override", ImVec2(160, 30))) action = GUI_TOGGLE_CAM;
        ImGui::SameLine();
        if (ImGui::Button("Toggle Smooth Cam", ImVec2(160, 30))) action = GUI_TOGGLE_SMOOTH_CAM;

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.18f, 0.54f, 0.82f, 1.0f), "GAME SPEED");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Game Speed Up", ImVec2(130, 30))) action = GUI_GAME_SPEED_UP;
        ImGui::SameLine();
        if (ImGui::Button("Game Speed Down", ImVec2(130, 30))) action = GUI_GAME_SPEED_DOWN;
        ImGui::SameLine();
        if (ImGui::Button("Reset Speed", ImVec2(130, 30))) action = GUI_GAME_SPEED_RESET;

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Exit Game", ImVec2(100, 30))) action = GUI_EXIT;
    }
    ImGui::End();

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(4);

    return action;
}
*/

// =============================================================================
// SwordfareGUI — Lua Console (ImGui-native, remastered)
// =============================================================================

void SwordfareGUI::init_lua_console(
    uint8_t* guest_memory,
    uint64_t buf_addr,   uint64_t result_addr,
    uint64_t pending_addr, uint64_t status_addr,
    uint64_t print_addr)
{
    m_guest_memory         = guest_memory;
    m_console_buf_addr     = buf_addr;
    m_console_result_addr  = result_addr;
    m_console_pending_addr = pending_addr;
    m_console_status_addr  = status_addr;
    m_console_print_addr   = print_addr;
    m_console_ready        = (buf_addr != 0);

    if (m_console_ready) {
        m_console_history.push_back({"Raijin  •  World's first Swordigo Lua console  •  Swordfare subsystem", false, false});
        m_console_history.push_back({"Type Lua and press Enter. Up/Down for history. Backtick (`) to close.", false, false});
    }
}

// ---------------------------------------------------------------------------
// SwordfareGUI::init_scene_shifter
// ---------------------------------------------------------------------------
// Wires up all scene shifter globals in guest memory.  After this call the
// GUI reads/writes those globals directly via (m_guest_memory + VA) instead
// of the always-zero host-side weak fallback copies.
//
// Also runs an initial host-side filesystem scan (std::filesystem) of
//   assets_dir/resources/  and, if present,  data_dir/mods/<mod>/resources/
// to populate g_sre_scene_list in guest memory so the panel shows scenes
// immediately without needing the guest scan function.
// ---------------------------------------------------------------------------
#include <filesystem>
void SwordfareGUI::init_scene_shifter(
    uint8_t* guest_memory,
    uint64_t scene_list_count_va,
    uint64_t scene_list_va,
    uint64_t shift_pending_va,
    uint64_t shift_target_va,
    uint64_t shift_spawn_va,
    uint64_t shift_error_va,
    uint64_t shift_active_va,
    uint64_t current_scene_va,
    const std::string& assets_dir)
{
    namespace fs = std::filesystem;

    // Store guest memory base (may already be set by init_lua_console)
    if (!m_guest_memory) m_guest_memory = guest_memory;

    m_ss_list_count_va    = scene_list_count_va;
    m_ss_list_va          = scene_list_va;
    m_ss_pending_va       = shift_pending_va;
    m_ss_target_va        = shift_target_va;
    m_ss_spawn_va         = shift_spawn_va;
    m_ss_error_va         = shift_error_va;
    m_ss_active_va        = shift_active_va;
    m_ss_current_scene_va = current_scene_va;
    m_ss_assets_dir       = assets_dir;  // e.g. ~/.local/share/swordigo-desktop/assets

    m_scene_shifter_ready = (guest_memory &&
                             scene_list_count_va &&
                             scene_list_va &&
                             shift_pending_va &&
                             shift_target_va);

    if (!m_scene_shifter_ready) {
        fprintf(stderr, "[SwordfareGUI] init_scene_shifter: missing VAs — scene shifter disabled\n");
        return;
    }

    // Run host-side scan immediately so the list is populated on first open.
    // We write directly into guest memory (g_sre_scene_list / g_sre_scene_list_count).
    int*  g_count = (int*)(guest_memory + scene_list_count_va);
    char (*g_list)[128] = (char(*)[128])(guest_memory + scene_list_va);
    *g_count = 0;

    auto try_scan = [&](const fs::path& dir) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) return;
        std::error_code ec;
        for (auto& ent : fs::recursive_directory_iterator(dir,
                fs::directory_options::skip_permission_denied, ec)) {
            if (*g_count >= 256) break;
            if (!ent.is_regular_file(ec)) continue;
            auto name = ent.path().filename().string();
            if (name.size() <= 6) continue;
            if (name.substr(name.size() - 6) != ".scene") continue;
            // strip extension
            std::string stem = name.substr(0, name.size() - 6);
            // deduplicate
            bool dup = false;
            for (int i = 0; i < *g_count; i++) {
                if (std::string(g_list[i]) == stem) { dup = true; break; }
            }
            if (dup) continue;
            strncpy(g_list[*g_count], stem.c_str(), 127);
            g_list[*g_count][127] = '\0';
            (*g_count)++;
        }
    };

    // 1. Vanilla assets/resources/
    try_scan(fs::path(assets_dir) / "resources");

    // 2. Sort alphabetically for readability
    qsort(g_list, *g_count, 128, [](const void* a, const void* b) -> int {
        return strcmp((const char*)a, (const char*)b);
    });

    fprintf(stderr, "[SwordfareGUI] Scene Shifter ready — %d scenes found in %s/resources\n",
            *g_count, assets_dir.c_str());
}

void SwordfareGUI::toggle_lua_console() {
    if (!m_console_ready) return;
    m_console_open  = !m_console_open;
    m_console_focus = m_console_open;
    m_console_scroll_bottom = true;
}

void SwordfareGUI::console_submit(const std::string& cmd) {
    if (cmd.empty() || !m_guest_memory || !m_console_buf_addr) return;

    // Intercept 'openport' command
    if (cmd == "openport" || cmd.rfind("openport ", 0) == 0) {
        m_console_history.push_back({"> " + cmd, false, true});
        int port = 12345;
        if (cmd.size() > 9) {
            try {
                port = std::stoi(cmd.substr(9));
            } catch (...) {
                m_console_history.push_back({"[TCP-Console] Error: Invalid port number.", true, false});
                return;
            }
        }
        start_tcp_server(port);
        return;
    }

    std::cout << "[SRE-Lua] > " << cmd << std::endl;
    m_console_history.push_back({"> " + cmd, false, true});
    while ((int)m_console_history.size() > CONSOLE_MAX_HISTORY)
        m_console_history.erase(m_console_history.begin());

    if (m_console_cmd_history.empty() || m_console_cmd_history.back() != cmd)
        m_console_cmd_history.push_back(cmd);
    m_console_hist_idx = -1;

    char* buf = (char*)(m_guest_memory + m_console_buf_addr);
    size_t len = cmd.size();
    if (len > 4094) len = 4094;
    memcpy(buf, cmd.c_str(), len);
    buf[len] = 0;

    *(int32_t*)(m_guest_memory + m_console_status_addr)  = 0;
    *(int32_t*)(m_guest_memory + m_console_pending_addr) = 1;
    m_console_scroll_bottom = true;
}

void SwordfareGUI::lua_console_text(const char* text) {
    if (!m_console_open || !text) return;
    size_t cur = strlen(m_console_input);
    size_t add = strlen(text);
    if (cur + add < sizeof(m_console_input) - 1)
        strcat(m_console_input, text);
}

bool SwordfareGUI::lua_console_key(SDL_Keycode key, const std::string& /*unused*/) {
    if (!m_console_open) return false;

    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (m_console_input[0]) { console_submit(m_console_input); m_console_input[0] = 0; }
        return true;
    }
    if (key == SDLK_BACKSPACE) {
        size_t len = strlen(m_console_input);
        if (len) m_console_input[len-1] = 0;
        return true;
    }
    if (key == SDLK_ESCAPE) { m_console_open = false; return true; }
    if (key == SDLK_UP) {
        if (!m_console_cmd_history.empty()) {
            if (m_console_hist_idx < 0)
                m_console_hist_idx = (int)m_console_cmd_history.size() - 1;
            else if (m_console_hist_idx > 0)
                m_console_hist_idx--;
            strncpy(m_console_input, m_console_cmd_history[m_console_hist_idx].c_str(),
                    sizeof(m_console_input)-1);
        }
        return true;
    }
    if (key == SDLK_DOWN) {
        if (m_console_hist_idx >= 0) {
            m_console_hist_idx++;
            if (m_console_hist_idx >= (int)m_console_cmd_history.size()) {
                m_console_hist_idx = -1; m_console_input[0] = 0;
            } else {
                strncpy(m_console_input, m_console_cmd_history[m_console_hist_idx].c_str(),
                        sizeof(m_console_input)-1);
            }
        }
        return true;
    }
    return true; // consume all keys when console is open
}

int SwordfareGUI::console_input_callback(ImGuiInputTextCallbackData* data) {
    SwordfareGUI* gui = (SwordfareGUI*)data->UserData;
    return gui->on_console_input_callback(data);
}

int SwordfareGUI::on_console_input_callback(ImGuiInputTextCallbackData* data) {
    // 1. History Recall via Up / Down arrow keys
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        int prev_idx = m_console_hist_idx;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (m_console_hist_idx < 0) {
                m_console_hist_idx = (int)m_console_cmd_history.size() - 1;
            } else if (m_console_hist_idx > 0) {
                m_console_hist_idx--;
            }
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (m_console_hist_idx >= 0) {
                m_console_hist_idx++;
                if (m_console_hist_idx >= (int)m_console_cmd_history.size()) {
                    m_console_hist_idx = -1;
                }
            }
        }

        if (prev_idx != m_console_hist_idx) {
            std::string cmd = (m_console_hist_idx >= 0) ? m_console_cmd_history[m_console_hist_idx] : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, cmd.c_str());
            data->CursorPos = data->SelectionStart = data->SelectionEnd = (int)cmd.size();
        }
    }

    // 2. Tab completion / Indent
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Insert 4 spaces at cursor position
        data->InsertChars(data->CursorPos, "    ");
    }

    // 3. Ctrl+L to Clear Screen & Ctrl+Up/Down for History Recall (for Multiline support)
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L)) {
            m_console_history.clear();
        }
        
        bool up_pressed = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        bool down_pressed = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if (io.KeyCtrl && (up_pressed || down_pressed)) {
            int prev_idx = m_console_hist_idx;
            if (up_pressed) {
                if (m_console_hist_idx < 0) {
                    m_console_hist_idx = (int)m_console_cmd_history.size() - 1;
                } else if (m_console_hist_idx > 0) {
                    m_console_hist_idx--;
                }
            } else if (down_pressed) {
                if (m_console_hist_idx >= 0) {
                    m_console_hist_idx++;
                    if (m_console_hist_idx >= (int)m_console_cmd_history.size()) {
                        m_console_hist_idx = -1;
                    }
                }
            }

            if (prev_idx != m_console_hist_idx) {
                std::string cmd = (m_console_hist_idx >= 0) ? m_console_cmd_history[m_console_hist_idx] : "";
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, cmd.c_str());
                data->CursorPos = data->SelectionStart = data->SelectionEnd = (int)cmd.size();
            }
        }
    }

    return 0;
}

void SwordfareGUI::draw_lua_console() {
    if (!m_initialized || !m_console_open || !m_console_ready) return;

    ImGuiIO& io = ImGui::GetIO();
    float W        = io.DisplaySize.x;
    float H        = io.DisplaySize.y;
    float panel_h  = H * 0.50f;               // taller for bigger context
    float font_scale = 0.82f;                 // compact — smaller than the global UI font

    ImGui::SetNextWindowPos(ImVec2(0, H - panel_h), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, panel_h),    ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);

    // ── Colour palette ─────────────────────────────────────────────────────
    const ImVec4 col_bg        = ImVec4(0.040f, 0.040f, 0.060f, 1.00f); // deep navy-black
    const ImVec4 col_border    = ImVec4(0.180f, 0.220f, 0.380f, 1.00f); // slate blue border
    const ImVec4 col_header_bg = ImVec4(0.060f, 0.060f, 0.090f, 1.00f); // slightly lighter header strip
    const ImVec4 col_input_bg  = ImVec4(0.030f, 0.030f, 0.050f, 1.00f); // input even darker
    const ImVec4 col_sep       = ImVec4(0.160f, 0.200f, 0.340f, 1.00f);
    const ImVec4 col_prompt    = ImVec4(0.36f,  0.76f,  1.00f,  1.00f); // sky-blue prompt
    const ImVec4 col_out       = ImVec4(0.85f,  0.97f,  0.85f,  1.00f); // faint green output
    const ImVec4 col_err       = ImVec4(1.00f,  0.38f,  0.38f,  1.00f); // red error
    const ImVec4 col_meta      = ImVec4(0.40f,  0.44f,  0.60f,  1.00f); // dim info lines
    const ImVec4 col_title     = ImVec4(0.36f,  0.76f,  1.00f,  1.00f); // title accent

    ImGui::PushStyleColor(ImGuiCol_WindowBg,        col_bg);
    ImGui::PushStyleColor(ImGuiCol_Border,          col_border);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         col_input_bg);
    ImGui::PushStyleColor(ImGuiCol_Text,            ImVec4(0.90f, 0.90f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,     ImVec4(0.03f, 0.03f, 0.05f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,   ImVec4(0.18f, 0.22f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.28f, 0.36f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  col_prompt);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(6.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(4.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,    8.0f);

    bool pushed_mono = false;
    if (m_font_mono) { ImGui::PushFont((ImFont*)m_font_mono); pushed_mono = true; }

    // Apply compact font scale
    ImGui::SetWindowFontScale(font_scale);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize   |
                             ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoCollapse  | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##SRE_LuaConsole", nullptr, flags)) {

        ImGui::SetWindowFontScale(font_scale);

        // ── Header strip ────────────────────────────────────────────────────
        float line_h = ImGui::GetTextLineHeight();
        float hdr_h  = line_h + 10.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + W, ImGui::GetWindowPos().y + hdr_h),
            ImGui::ColorConvertFloat4ToU32(col_header_bg)
        );

        ImGui::SetCursorPos(ImVec2(10.0f, 5.0f));
        ImGui::TextColored(col_title, ICON_FA_TERMINAL " raijin");
        ImGui::SameLine(0, 2);
        ImGui::TextColored(col_meta, "  swordfare lua-console v8  |  lua 5.1  |  full gamestate  |  ` to close");

        float btn_x = W - 320.0f;
        ImGui::SameLine(btn_x);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.10f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.24f, 0.44f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.14f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          col_meta);
        if (ImGui::SmallButton(" " ICON_FA_FILE " Manager ")) m_script_manager_open = !m_script_manager_open;
        ImGui::SameLine(0, 6);
        if (ImGui::SmallButton(" " ICON_FA_CODE " Editor ")) m_script_editor_open = !m_script_editor_open;
        ImGui::SameLine(0, 6);
        if (ImGui::SmallButton(" " ICON_FA_CIRCLE_XMARK " Clear ")) m_console_history.clear();
        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.30f, 0.30f, 1.0f));
        if (ImGui::SmallButton("  " ICON_FA_XMARK "  ")) m_console_open = false;
        ImGui::PopStyleColor(5);

        // thin separator line under header
        ImVec2 sep_a(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + hdr_h);
        ImVec2 sep_b(sep_a.x + W, sep_a.y);
        ImGui::GetWindowDrawList()->AddLine(sep_a, sep_b,
            ImGui::ColorConvertFloat4ToU32(col_sep), 1.0f);

        // ── Scrollback area ─────────────────────────────────────────────────
        ImGui::SetCursorPos(ImVec2(0.0f, hdr_h + 2.0f));

        // Input row height: multiline (3 lines) + padding + separator
        float input_inner_h = line_h * 3.0f + 8.0f;
        float input_area_h  = input_inner_h + 6.0f + line_h + 6.0f; // +prompt row + gaps
        float scroll_h = panel_h - hdr_h - input_area_h - 8.0f;
        if (scroll_h < 40.0f) scroll_h = 40.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, col_bg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
        ImGui::BeginChild("##scroll", ImVec2(0, scroll_h), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::SetWindowFontScale(font_scale);

        for (auto& e : m_console_history) {
            if (e.is_input) {
                ImGui::TextColored(col_prompt, ">");
                ImGui::SameLine(0, 5);
                // user command in slightly brighter white
                ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.0f), "%s", e.text.c_str() + 2);
            } else if (e.is_error) {
                ImGui::TextColored(col_err, "%s", e.text.c_str());
            } else {
                // dim for banner/info lines vs normal output
                bool is_banner = (m_console_history.size() > 1 &&
                                  &e == &m_console_history[0] || &e == &m_console_history[1]);
                ImGui::TextColored(is_banner ? col_meta : col_out, "%s", e.text.c_str());
            }
        }
        if (m_console_scroll_bottom) {
            ImGui::SetScrollHereY(1.0f);
            m_console_scroll_bottom = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(); // ChildBg

        // thin separator above input
        float cur_y = ImGui::GetCursorPosY();
        ImVec2 isep_a(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + cur_y);
        ImVec2 isep_b(isep_a.x + W, isep_a.y);
        ImGui::GetWindowDrawList()->AddLine(isep_a, isep_b,
            ImGui::ColorConvertFloat4ToU32(col_sep), 1.0f);
        ImGui::SetCursorPosY(cur_y + 2.0f);

        // ── Input row ────────────────────────────────────────────────────────
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));

        // Prompt glyph
        ImGui::SetCursorPosX(6.0f);
        ImGui::TextColored(col_prompt, ">>>");
        ImGui::SameLine(0, 6);

        if (m_console_focus) { ImGui::SetKeyboardFocusHere(); m_console_focus = false; }

        // Multiline input — 3 visible lines, grows via scrollbar inside
        float run_btn_w = 52.0f;
        float input_w   = W - ImGui::GetCursorPosX() - run_btn_w - 12.0f;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, col_input_bg);
        ImGui::PushStyleColor(ImGuiCol_Border,  col_sep);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        bool submit = ImGui::InputTextMultiline(
            "##input", m_console_input, sizeof(m_console_input),
            ImVec2(input_w, input_inner_h),
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CtrlEnterForNewLine |
            ImGuiInputTextFlags_EscapeClearsAll |
            ImGuiInputTextFlags_CallbackCompletion |
            ImGuiInputTextFlags_CallbackAlways,
            console_input_callback, this
        );

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 6);

        // Run button
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.36f, 0.72f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.95f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.22f, 0.52f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        if (ImGui::Button(" " ICON_FA_PLAY " ##run", ImVec2(run_btn_w, input_inner_h)) && m_console_input[0]) {
            // strip trailing newline that Enter adds in multiline mode
            int len = (int)strlen(m_console_input);
            while (len > 0 && (m_console_input[len-1] == '\n' || m_console_input[len-1] == '\r'))
                m_console_input[--len] = '\0';
            if (len > 0) {
                console_submit(m_console_input);
                m_console_input[0] = 0;
                m_console_focus    = true;
            }
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(); // WindowPadding (input row)

        if (submit && m_console_input[0]) {
            int len = (int)strlen(m_console_input);
            while (len > 0 && (m_console_input[len-1] == '\n' || m_console_input[len-1] == '\r'))
                m_console_input[--len] = '\0';
            if (len > 0) {
                console_submit(m_console_input);
                m_console_input[0] = 0;
                m_console_focus    = true;
            }
        }
    }
    ImGui::End();

    if (pushed_mono) ImGui::PopFont();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(8);
}

void SwordfareGUI::start_tcp_server(int port) {
    stop_tcp_server();

    m_tcp_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_tcp_server_fd < 0) {
        m_console_history.push_back({"[TCP-Console] Error: Failed to create socket.", true, false});
        return;
    }

    int opt = 1;
    setsockopt(m_tcp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(m_tcp_server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        m_console_history.push_back({"[TCP-Console] Error: Failed to bind to port " + std::to_string(port), true, false});
        close(m_tcp_server_fd);
        m_tcp_server_fd = -1;
        return;
    }

    if (listen(m_tcp_server_fd, 3) < 0) {
        m_console_history.push_back({"[TCP-Console] Error: Failed to listen on socket.", true, false});
        close(m_tcp_server_fd);
        m_tcp_server_fd = -1;
        return;
    }

    m_tcp_running = true;
    m_tcp_thread = std::thread(&SwordfareGUI::tcp_server_loop, this);
    m_console_history.push_back({"[TCP-Console] Server started on port " + std::to_string(port) + ". Run 'nc localhost " + std::to_string(port) + "' to connect.", false, false});
}

void SwordfareGUI::stop_tcp_server() {
    m_tcp_running = false;
    if (m_tcp_server_fd >= 0) {
        ::shutdown(m_tcp_server_fd, SHUT_RDWR);
        close(m_tcp_server_fd);
        m_tcp_server_fd = -1;
    }
    
    int client_fd = m_tcp_client_fd.load();
    if (client_fd >= 0) {
        ::shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        m_tcp_client_fd = -1;
    }

    if (m_tcp_thread.joinable()) {
        m_tcp_thread.join();
    }
}

static bool is_lua_chunk_complete(const std::string& code) {
    int braces = 0;
    int parens = 0;
    int brackets = 0; // [ ]
    int blocks = 0;
    
    size_t i = 0;
    size_t n = code.length();
    while (i < n) {
        char ch = code[i];
        
        // 1. Skip comments
        if (ch == '-' && i + 1 < n && code[i + 1] == '-') {
            i += 2;
            // Check for multi-line comment: --[[ ... ]] or --[=[ ... ]=]
            if (i < n && code[i] == '[') {
                size_t start = i;
                size_t equals_count = 0;
                i++;
                while (i < n && code[i] == '=') {
                    equals_count++;
                    i++;
                }
                if (i < n && code[i] == '[') {
                    // It is a multi-line comment. Find matching closer: ]=...]
                    i++;
                    std::string closer = "]" + std::string(equals_count, '=') + "]";
                    size_t closer_pos = code.find(closer, i);
                    if (closer_pos != std::string::npos) {
                        i = closer_pos + closer.length();
                    } else {
                        i = n; // Incomplete multi-line comment
                    }
                    continue;
                }
                // Not a multi-line comment, fall back to single-line comment
                i = start;
            }
            // Single-line comment: skip to newline
            while (i < n && code[i] != '\n') {
                i++;
            }
            continue;
        }
        
        // 2. Skip string literals
        if (ch == '\'' || ch == '"') {
            char quote = ch;
            i++;
            while (i < n) {
                if (code[i] == '\\' && i + 1 < n) {
                    i += 2; // skip escaped char
                    continue;
                }
                if (code[i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        
        // 3. Skip Lua long brackets (strings): [[ ... ]] or [=[ ... ]=]
        if (ch == '[') {
            size_t start = i;
            size_t equals_count = 0;
            i++;
            while (i < n && code[i] == '=') {
                equals_count++;
                i++;
            }
            if (i < n && code[i] == '[') {
                i++;
                std::string closer = "]" + std::string(equals_count, '=') + "]";
                size_t closer_pos = code.find(closer, i);
                if (closer_pos != std::string::npos) {
                    i = closer_pos + closer.length();
                } else {
                    i = n; // Incomplete long string
                }
                continue;
            }
            i = start; // Just a regular bracket [
        }
        
        // 4. Track braces, parens, brackets
        if (ch == '{') braces++;
        else if (ch == '}') braces--;
        else if (ch == '(') parens++;
        else if (ch == ')') parens--;
        else if (ch == '[') brackets++;
        else if (ch == ']') brackets--;
        
        // 5. Track Lua block keywords
        if (isalpha(ch) || ch == '_') {
            std::string word;
            while (i < n && (isalnum(code[i]) || code[i] == '_')) {
                word += code[i];
                i++;
            }
            if (word == "do" || word == "then" || word == "repeat" || word == "function") {
                blocks++;
            } else if (word == "end" || word == "until") {
                blocks--;
            } else if (word == "elseif") {
                blocks--;
            }
            continue;
        }
        
        i++;
    }
    return (braces <= 0 && parens <= 0 && brackets <= 0 && blocks <= 0);
}

void SwordfareGUI::tcp_server_loop() {
    while (m_tcp_running) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(m_tcp_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (!m_tcp_running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Handle client connection (support one client at a time for simplicity)
        int old_client = m_tcp_client_fd.exchange(client_fd);
        if (old_client >= 0) {
            const char* msg = "Another client connected. Disconnecting...\n";
            write(old_client, msg, strlen(msg));
            close(old_client);
        }

        // Welcome greeting
        const char* greeting = 
            "\033[1;36m=====================================================\033[0m\n"
            "\033[1;32m      RAIJIN Lua SDK Console (Swordfare TCP Server)  \033[0m\n"
            "\033[1;36m=====================================================\033[0m\n"
            " \033[33m*\033[0m Enter Lua commands or blocks to evaluate dynamically.\n"
            " \033[33m*\033[0m Supports full multiline input (braces, quotes, functions).\n"
            " \033[33m*\033[0m Type \033[1;31m.\033[0m on a new line to clear current input buffer.\n"
            " \033[33m*\033[0m Type \033[1;32mclear\033[0m to clear screen.\n"
            " \033[33m*\033[0m Type \033[1;31mexit\033[0m or \033[1;31mquit\033[0m to close connection.\n\n"
            "\033[1;36mraijin-sdk\033[0m> ";
        write(client_fd, greeting, strlen(greeting));

        // Read loop for client commands
        char read_buf[4096];
        std::string line_accumulator;
        std::string multiline_cmd;
        while (m_tcp_running && m_tcp_client_fd.load() == client_fd) {
            ssize_t bytes_read = read(client_fd, read_buf, sizeof(read_buf) - 1);
            if (bytes_read <= 0) {
                // Client disconnected or read error
                break;
            }
            read_buf[bytes_read] = '\0';
            line_accumulator += read_buf;

            // Process any complete lines
            size_t newline_pos;
            while ((newline_pos = line_accumulator.find('\n')) != std::string::npos) {
                std::string line = line_accumulator.substr(0, newline_pos);
                line_accumulator.erase(0, newline_pos + 1);

                // Strip carriage return if present
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                // Strip leading/trailing whitespaces
                size_t first = line.find_first_not_of(" \t");
                if (first != std::string::npos) {
                    line = line.substr(first);
                    size_t last = line.find_last_not_of(" \t");
                    line = line.substr(0, last + 1);
                } else {
                    line.clear();
                }

                // Check for input buffer reset command
                if (line == ".") {
                    multiline_cmd.clear();
                    const char* reset_msg = "Input buffer cleared.\n\033[1;36mraijin-sdk\033[0m> ";
                    write(client_fd, reset_msg, strlen(reset_msg));
                    continue;
                }

                if (line == "exit" || line == "quit") {
                    const char* bye = "Goodbye!\n";
                    write(client_fd, bye, strlen(bye));
                    close(client_fd);
                    m_tcp_client_fd.compare_exchange_strong(client_fd, -1);
                    break;
                }

                if (line == "clear") {
                    // Send ANSI clear screen command
                    const char* clear_ansi = "\033[2J\033[H\033[1;36mraijin-sdk\033[0m> ";
                    write(client_fd, clear_ansi, strlen(clear_ansi));
                    multiline_cmd.clear();
                    continue;
                }

                // Accumulate block line
                if (multiline_cmd.empty()) {
                    multiline_cmd = line;
                } else {
                    multiline_cmd += "\n" + line;
                }

                // Execute block if it is complete
                bool execute = false;
                if (multiline_cmd.empty()) {
                    execute = true;
                } else {
                    if (is_lua_chunk_complete(multiline_cmd)) {
                        execute = true;
                    }
                }

                if (execute) {
                    if (!multiline_cmd.empty()) {
                        // Block until the previous command has been processed
                        while (m_tcp_running && m_tcp_client_fd.load() == client_fd) {
                            m_tcp_mutex.lock();
                            bool empty = m_tcp_pending_cmd.empty();
                            m_tcp_mutex.unlock();
                            if (empty) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }

                        if (!m_tcp_running || m_tcp_client_fd.load() != client_fd) break;

                        m_tcp_mutex.lock();
                        m_tcp_pending_cmd = multiline_cmd;
                        m_tcp_mutex.unlock();

                        multiline_cmd.clear();
                    } else {
                        // Just print a new prompt if empty line is submitted in clean state
                        const char* prompt = "\033[1;36mraijin-sdk\033[0m> ";
                        write(client_fd, prompt, strlen(prompt));
                    }
                } else {
                    // Send block continuation prompt
                    const char* cont_prompt = "\033[1;33m          \033[0m>> ";
                    write(client_fd, cont_prompt, strlen(cont_prompt));
                }
            }
        }

        // Cleanup this client connection if we broke out of the read loop
        if (m_tcp_client_fd.load() == client_fd) {
            close(client_fd);
            m_tcp_client_fd.store(-1);
        }
    }
}


