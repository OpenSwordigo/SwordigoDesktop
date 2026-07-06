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

#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>

bool g_sre_overlay_blocking = false;

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

    // Find font files
    std::string main_font_path, button_font_path, fa_path;
    const char* home_val = getenv("HOME");
    std::string home_dir = home_val ? home_val : "";

    std::vector<std::string> main_font_candidates = {
        "src/assets/fonts/Redaction10-Regular.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Redaction10-Regular.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf"
    };
    if (!home_dir.empty()) {
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
        "src/assets/fonts/Redaction10-Bold.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Redaction10-Bold.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf"
    };
    if (!home_dir.empty()) {
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
        "/usr/share/swordigo-desktop/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/fa-solid-900.ttf"
    };
    if (!home_dir.empty()) {
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

    // Scale ImGui styles according to the high DPI scale factor
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi_scale);

    ImGui_ImplSDL3_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    m_initialized = true;
}

void SwordfareGUI::shutdown() {
    if (!m_initialized) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
    ImGui_ImplOpenGL3_Shutdown();
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

void SwordfareGUI::begin_frame() {
    if (!m_initialized) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void SwordfareGUI::end_frame() {
    if (!m_initialized) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---------------------------------------------------------------------------
// draw_debug — the F3 debug window
// ---------------------------------------------------------------------------

void SwordfareGUI::draw_debug(const SwordfareDebugStats& st) {
    // -- Update FPS ring buffer --
    swardfare_push_fps(m_fps_history, m_fps_idx, st.fps, FPS_HISTORY);

    static bool expanded = false;

    // -- Window position (top-left, draggable) --
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(expanded ? 410 : 200, 0), ImGuiCond_Always);
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

        ImGui::SameLine(expanded ? 355 : 155);
        if (ImGui::Button(expanded ? " < ##exp" : " > ##exp", ImVec2(30, 20))) {
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
                ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthFixed, 140);
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
                ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthFixed, 140);
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

                ImGui::EndTable();
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
                        fprintf(f, "Slot %d: id='%s' active=%d hidden=%d x=%.3f y=%.3f w=%.3f h=%.3f bg_color=0x%08X bg_alpha=%d corner_radius=%.3f\n",
                                i, ovrs[i].id, ovrs[i].active, ovrs[i].hidden,
                                ovrs[i].x, ovrs[i].y, ovrs[i].w, ovrs[i].h,
                                (unsigned int)ovrs[i].bg_color, ovrs[i].bg_alpha, ovrs[i].corner_radius);
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
    float base = std::min(vp_w, vp_h);

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
        for (int i = 0; i < SRE_BTN_MAX; i++) {
            if (buttons[i].active) {
                std::cout << "  - Button " << i << ": id='" << buttons[i].id 
                          << "', label='" << buttons[i].label 
                          << "', active=" << buttons[i].active 
                          << ", hidden=" << buttons[i].hidden 
                          << ", overlay_id='" << buttons[i].overlay_id 
                          << "', x=" << buttons[i].cur_x 
                          << ", y=" << buttons[i].cur_y 
                          << ", w=" << buttons[i].w 
                          << ", h=" << buttons[i].h 
                          << ", bg_alpha=" << buttons[i].bg_alpha 
                          << ", alpha=" << buttons[i].alpha 
                          << ", text_color=0x" << std::hex << buttons[i].text_color << std::dec
                          << std::endl;
            }
        }
    }
    // ---------------------------------------------------------------------------
    // Determine if any full-screen / blocking overlay is active (for input gating)
    // ---------------------------------------------------------------------------
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
    // Expose to main.cpp for SDL input gating
    extern bool g_sre_overlay_blocking;
    g_sre_overlay_blocking = any_overlay_blocking;

    // ---------------------------------------------------------------------------
    // 1. Draw Active Overlay BACKGROUNDS using BackgroundDrawList
    //    (BackgroundDrawList is ALWAYS rendered behind all ImGui windows and the
    //     foreground draw list, so it can never cover buttons.)
    // ---------------------------------------------------------------------------
    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList();

    if (overlays) {
        for (int i = 0; i < SRE_OVERLAY_MAX; i++) {
            SreOverlaySlot& ovr = overlays[i];
            if (!ovr.active || ovr.hidden) continue;

            float ow = vp_w * ovr.w;
            float oh = vp_h * ovr.h;
            float ox = vp_x + vp_w * ovr.x - ow / 2.0f;
            float oy = vp_y + vp_h * ovr.y - oh / 2.0f;

            // Parse background color from packed ARGB (0xAARRGGBB)
            unsigned int c_val = (unsigned int)ovr.bg_color;
            uint8_t ba = (c_val >> 24) & 0xFF;
            uint8_t br = (c_val >> 16) & 0xFF;
            uint8_t bg_c = (c_val >> 8) & 0xFF;
            uint8_t bb = c_val & 0xFF;

            // Fallback: if no color set, use dark semi-transparent
            if (c_val == 0) {
                br = 22; bg_c = 27; bb = 34;
                ba = (uint8_t)ovr.bg_alpha;
            }

            ImU32 fill_col = IM_COL32(br, bg_c, bb, ba);
            float rounding = (ovr.corner_radius > 0.0f) ? ovr.corner_radius : 6.0f;
            // For full-screen overlays, skip rounding (looks bad)
            if (ovr.w >= 0.98f && ovr.h >= 0.98f) rounding = 0.0f;

            bg_dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + ow, oy + oh), fill_col, rounding);

            // Border (subtle, except full-screen)
            if (ovr.w < 0.98f || ovr.h < 0.98f) {
                bg_dl->AddRect(ImVec2(ox, oy), ImVec2(ox + ow, oy + oh),
                               IM_COL32(233, 69, 96, 100), rounding, 0, 1.0f);
            }

            // Separators
            for (int s = 0; s < ovr.separator_count; s++) {
                float sy = ovr.separators[s];
                float line_y = oy + oh * sy;
                bg_dl->AddLine(ImVec2(ox, line_y), ImVec2(ox + ow, line_y),
                               IM_COL32(255, 255, 255, 50), 1.5f);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // 2. Draw Buttons using ForegroundDrawList + manual hit-testing
    //    (ForegroundDrawList is ALWAYS on top of everything — no Z-order issues)
    // ---------------------------------------------------------------------------
    ImDrawList* fg_dl = ImGui::GetForegroundDrawList();
    ImGuiIO& io2 = ImGui::GetIO();
    ImVec2 mouse_pos = io2.MousePos;
    bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    // Load button font (same font object as before)
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
            // Button belongs to an overlay that no longer exists → skip
            if (!parent_ovr) continue;
        }

        // Hide button if parent overlay is hidden/inactive
        if (parent_ovr && (parent_ovr->hidden || !parent_ovr->active)) continue;

        // Compute pixel rect — use script-driven sizes, no hardcoded scale
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

        // Clamp to visible area (avoids off-screen draw commands)
        float bx2 = bx + pw;
        float by2 = by + ph;
        if (bx2 < 0 || by2 < 0 || bx > win_w || by > win_h) continue;

        // --- Hit test ---
        bool hovered = btn.clickable &&
                       (mouse_pos.x >= bx && mouse_pos.x <= bx2 &&
                        mouse_pos.y >= by  && mouse_pos.y <= by2);
        bool pressing = hovered && mouse_down;

        // --- Draw background ---
        if (btn.bg_alpha > 0) {
            float alpha = (btn.alpha / 255.0f) * (btn.bg_alpha / 255.0f);
            
            // Premium glassmorphism style
            ImU32 bg_col;
            if (pressing) {
                bg_col = IM_COL32(0, 100, 200, (uint8_t)(alpha * 255.0f * 0.9f)); // Sleek blue press state
            } else if (hovered) {
                bg_col = IM_COL32(40, 45, 60, (uint8_t)(alpha * 255.0f * 0.85f)); // Hover state blue-grey tint
            } else {
                bg_col = IM_COL32(20, 20, 25, (uint8_t)(alpha * 255.0f * 0.75f)); // Translucent glass background
            }
            
            float rounding = ph * 0.25f; // Proportional rounded corners
            if (rounding < 4.0f) rounding = 4.0f;
            if (rounding > 12.0f) rounding = 12.0f;

            fg_dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx2, by2), bg_col, rounding);

            // Glowing mod-blue border on hover/press, translucent white otherwise
            ImU32 border_col;
            if (pressing) {
                border_col = IM_COL32(0, 180, 255, 255);
            } else if (hovered) {
                border_col = IM_COL32(0, 140, 255, 220); // Glowing border
            } else {
                border_col = IM_COL32(255, 255, 255, (uint8_t)(alpha * 120.0f)); // Subtle border
            }
            fg_dl->AddRect(ImVec2(bx, by), ImVec2(bx2, by2), border_col, rounding, 0, 1.5f);
        }

        // --- Draw label ---
        const char* lbl = btn.label;

        if (lbl[0] != '\0' && btn_font) {
            float fscale = btn.text_scale > 0.0f ? btn.text_scale : 1.0f;
            // Clamp font scale to avoid insanely large text
            if (fscale > 3.0f) fscale = 3.0f;

            // Compute text size at script-driven scale
            float fs = btn_font->LegacySize * fscale;
            ImVec2 tsize = btn_font->CalcTextSizeA(fs, FLT_MAX, 0.0f, lbl);

            float tx = bx + (pw - tsize.x) * 0.5f;
            float ty = by + (ph - tsize.y) * 0.5f;

            // Parse text color (packed ARGB 0xAARRGGBB)
            unsigned int tc = (unsigned int)btn.text_color;
            uint8_t ta = (tc >> 24) & 0xFF;
            uint8_t tr = (tc >> 16) & 0xFF;
            uint8_t tg = (tc >> 8)  & 0xFF;
            uint8_t tb = tc & 0xFF;
            if (ta == 0) ta = (uint8_t)btn.alpha;
            ImU32 text_col = IM_COL32(tr, tg, tb, ta);

            fg_dl->AddText(btn_font, fs, ImVec2(tx, ty), text_col, lbl);
        }

        // --- Input state machine ---
        if (btn.clickable) {
            if (pressing) {
                btn.pressed = 1;
                if (btn.movable) {
                    btn.dragging = 1;
                    ImVec2 delta = io2.MouseDelta;
                    if (parent_ovr) {
                        float ow = vp_w * parent_ovr->w;
                        float oh = vp_h * parent_ovr->h;
                        if (ow > 0) btn.cur_x += delta.x / ow;
                        if (oh > 0) btn.cur_y += delta.y / oh;
                    } else {
                        if (vp_w > 0) btn.cur_x += delta.x / vp_w;
                        if (vp_h > 0) btn.cur_y += delta.y / vp_h;
                    }
                    btn.dirty = 1;
                }
            } else {
                if (btn.pressed == 1 && hovered && mouse_released) {
                    btn.pressed = 0;
                    btn.released = 1;
                    std::cout << "[SRE GUI] Click: '" << btn.label
                              << "' (slot " << i << ")\n";
                } else if (!pressing) {
                    btn.pressed = 0;
                }
                if (btn.dragging && !mouse_down) {
                    btn.dragging = 0;
                    if (btn.snapback) {
                        btn.cur_x = btn.home_x;
                        btn.cur_y = btn.home_y;
                        btn.dirty = 1;
                    }
                }
            }
        }
    }
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
    if (ImGui::Begin("Swordfare Mod Loader Overlay", &open, flags)) {
        ImGui::PushFont(static_cast<ImFont*>(m_font_button));
        ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.00f), "SWORDFARE MOD LOADER");
        ImGui::PopFont();
        ImGui::Text("v7.4 — PC Mod Compatibility Layer");
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

            if (ImGui::BeginTabItem("Save Profile Manager")) {
                ImGui::Spacing();
                if (m_save_files.empty()) {
                    scan_saves(save_dir);
                }
                
                if (m_save_files.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No save profiles (.gplayer) found in directory:");
                    ImGui::TextUnformatted(save_dir.c_str());
                    if (ImGui::Button("Rescan Profiles")) {
                        scan_saves(save_dir);
                    }
                } else {
                    ImGui::Text("Select Save Profile:");
                    ImGui::Spacing();
                    
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.17f, 0.4f));
                    if (ImGui::BeginChild("##saves_list_child", ImVec2(240, 0), true)) {
                        for (int s = 0; s < (int)m_save_files.size(); s++) {
                            bool selected = (m_selected_save == s);
                            if (ImGui::Selectable(m_save_files[s].c_str(), selected)) {
                                if (m_selected_save != s) {
                                    m_selected_save = s;
                                    load_save(save_dir + "/" + m_save_files[s]);
                                }
                            }
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    
                    ImGui::SameLine();
                    
                    if (m_selected_save >= 0 && m_selected_save < (int)m_save_files.size()) {
                        ImGui::BeginGroup();
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Profile Details: %s", m_save_files[m_selected_save].c_str());
                        ImGui::Separator();
                        ImGui::Text("Level: %d", m_inventory.level);
                        ImGui::Text("Coins: %d", m_inventory.coins);
                        ImGui::Text("XP: %d", m_inventory.xp);
                        ImGui::Text("Current HP Hearts: %d", m_inventory.current_health);
                        ImGui::Text("Current Mana: %d", m_inventory.current_mana);
                        ImGui::Spacing();
                        
                        if (m_inventory_dirty) {
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "⚠️ Profile has unsaved changes!");
                            if (ImGui::Button("Save Changes", ImVec2(160, 36))) {
                                if (write_save(save_dir + "/" + m_save_files[m_selected_save])) {
                                    m_status_msg = "Successfully saved changes!";
                                    m_status_timer = 3.0f;
                                } else {
                                    m_status_msg = "Failed to write save profile!";
                                    m_status_timer = 3.0f;
                                }
                            }
                        } else {
                            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "✅ Profile is in-sync.");
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                            ImGui::Button("No Changes to Save", ImVec2(160, 36));
                            ImGui::PopStyleColor();
                        }
                        
                        if (m_status_timer > 0.0f) {
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "%s", m_status_msg.c_str());
                        }
                        
                        ImGui::EndGroup();
                    } else {
                        ImGui::Text("Please select a save profile from the list.");
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Stats & Inventory Editor")) {
                ImGui::Spacing();
                if (m_selected_save < 0 || m_selected_save >= (int)m_save_files.size()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Please load a save profile in the 'Save Profile Manager' tab first.");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.17f, 0.3f));
                    if (ImGui::BeginChild("##editor_scrollable", ImVec2(0, 0), true)) {
                        
                        ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "Character Attributes");
                        ImGui::Separator();
                        ImGui::Spacing();
                        
                        if (ImGui::InputInt("Coins", &m_inventory.coins)) m_inventory_dirty = true;
                        if (ImGui::InputInt("Level", &m_inventory.level)) m_inventory_dirty = true;
                        if (ImGui::InputInt("XP", &m_inventory.xp)) m_inventory_dirty = true;
                        if (ImGui::InputInt("HP / Heart Container Attribute", &m_inventory.health_attr)) m_inventory_dirty = true;
                        if (ImGui::InputInt("Attack Power Attribute", &m_inventory.attack_attr)) m_inventory_dirty = true;
                        if (ImGui::InputInt("Magic Power Attribute", &m_inventory.magic_attr)) m_inventory_dirty = true;
                        if (ImGui::InputInt("Current HP Hearts", &m_inventory.current_health)) m_inventory_dirty = true;
                        if (ImGui::InputInt("Current Mana", &m_inventory.current_mana)) m_inventory_dirty = true;
                        
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "Equipment & Spells");
                        ImGui::Separator();
                        ImGui::Spacing();
                        
                        const char* weapon_names[] = { "None", "Brass Sword", "Iron Sword", "The Needle", "Broad Sword", "The Thorn", "Magic Sword", "The Mageblade" };
                        const char* weapon_ids[] = { "", "brasssword", "ironsword", "needle", "broadsword", "thorn", "magicsword", "legendsword" };
                        int current_weapon_idx = 0;
                        for (int w = 0; w < 8; w++) {
                            if (m_inventory.equipped_weapon == weapon_ids[w]) { current_weapon_idx = w; break; }
                        }
                        if (ImGui::Combo("Equipped Weapon", &current_weapon_idx, weapon_names, IM_ARRAYSIZE(weapon_names))) {
                            m_inventory.equipped_weapon = weapon_ids[current_weapon_idx];
                            if (current_weapon_idx > 0) {
                                m_inventory.add_item(weapon_ids[current_weapon_idx], 1);
                            }
                            m_inventory_dirty = true;
                        }
                        
                        const char* armor_names[] = { "None", "Plate Armor", "Magic Armor" };
                        const char* armor_ids[] = { "", "platearmor", "magicarmor" };
                        int current_armor_idx = 0;
                        for (int a = 0; a < 3; a++) {
                            if (m_inventory.equipped_armor == armor_ids[a]) { current_armor_idx = a; break; }
                        }
                        if (ImGui::Combo("Equipped Armor", &current_armor_idx, armor_names, IM_ARRAYSIZE(armor_names))) {
                            m_inventory.equipped_armor = armor_ids[current_armor_idx];
                            if (current_armor_idx > 0) {
                                m_inventory.add_item(armor_ids[current_armor_idx], 1);
                            }
                            m_inventory_dirty = true;
                        }
                        
                        ImGui::Spacing();
                        ImGui::Text("Spells:");
                        bool has_bolt = m_inventory.has_skill("bolt");
                        if (ImGui::Checkbox("Magic Bolt", &has_bolt)) {
                            if (has_bolt) m_inventory.add_skill("bolt");
                            else m_inventory.remove_skill("bolt");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool has_bomb = m_inventory.has_skill("bomb");
                        if (ImGui::Checkbox("Magic Bomb", &has_bomb)) {
                            if (has_bomb) m_inventory.add_skill("bomb");
                            else m_inventory.remove_skill("bomb");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool has_hook = m_inventory.has_skill("hookshot");
                        if (ImGui::Checkbox("Dragon's Grasp (Hookshot)", &has_hook)) {
                            if (has_hook) m_inventory.add_skill("hookshot");
                            else m_inventory.remove_skill("hookshot");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool has_dim = m_inventory.has_skill("dimension");
                        if (ImGui::Checkbox("Dimension Rift", &has_dim)) {
                            if (has_dim) m_inventory.add_skill("dimension");
                            else m_inventory.remove_skill("dimension");
                            m_inventory_dirty = true;
                        }
                        
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "Consumables & Trinkets");
                        ImGui::Separator();
                        ImGui::Spacing();
                        
                        int potion_count = 0;
                        for (auto& item : m_inventory.items) {
                            if (item.name == "healingpotion") potion_count = item.count;
                        }
                        if (ImGui::InputInt("Healing Potions", &potion_count)) {
                            if (potion_count < 0) potion_count = 0;
                            m_inventory.remove_item("healingpotion");
                            if (potion_count > 0) m_inventory.add_item("healingpotion", potion_count);
                            m_inventory_dirty = true;
                        }
                        
                        int xp_sack_count = 0;
                        for (auto& item : m_inventory.items) {
                            if (item.name == "experiencesack") xp_sack_count = item.count;
                        }
                        if (ImGui::InputInt("Sacks of Experience", &xp_sack_count)) {
                            if (xp_sack_count < 0) xp_sack_count = 0;
                            m_inventory.remove_item("experiencesack");
                            if (xp_sack_count > 0) m_inventory.add_item("experiencesack", xp_sack_count);
                            m_inventory_dirty = true;
                        }
                        
                        int ankh_count = 0;
                        for (auto& item : m_inventory.items) {
                            if (item.name == "ankh") ankh_count = item.count;
                        }
                        if (ImGui::InputInt("Ankhs of Resurrection", &ankh_count)) {
                            if (ankh_count < 0) ankh_count = 0;
                            m_inventory.remove_item("ankh");
                            if (ankh_count > 0) m_inventory.add_item("ankh", ankh_count);
                            m_inventory_dirty = true;
                        }
                        
                        ImGui::Spacing();
                        ImGui::Text("Equipped Trinkets:");
                        bool fire_t = m_inventory.has_item("firetrinket");
                        if (ImGui::Checkbox("Trinket of Fire", &fire_t)) {
                            if (fire_t) m_inventory.add_item("firetrinket", 1);
                            else m_inventory.remove_item("firetrinket");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool ice_t = m_inventory.has_item("icetrinket");
                        if (ImGui::Checkbox("Trinket of Ice", &ice_t)) {
                            if (ice_t) m_inventory.add_item("icetrinket", 1);
                            else m_inventory.remove_item("icetrinket");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool shadow_t = m_inventory.has_item("shadowtrinket");
                        if (ImGui::Checkbox("Trinket of Shadow", &shadow_t)) {
                            if (shadow_t) m_inventory.add_item("shadowtrinket", 1);
                            else m_inventory.remove_item("shadowtrinket");
                            m_inventory_dirty = true;
                        }
                        
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.914f, 0.271f, 0.376f, 1.0f), "Quest Items");
                        ImGui::Separator();
                        ImGui::Spacing();
                        
                        bool has_key = m_inventory.has_item("key_yellow");
                        if (ImGui::Checkbox("A Yellow Key", &has_key)) {
                            if (has_key) m_inventory.add_item("key_yellow", 1);
                            else m_inventory.remove_item("key_yellow");
                            m_inventory_dirty = true;
                        }
                        
                        bool shard1 = m_inventory.has_item("iselon_shard_1");
                        if (ImGui::Checkbox("Mageblade Shard 1", &shard1)) {
                            if (shard1) m_inventory.add_item("iselon_shard_1", 1);
                            else m_inventory.remove_item("iselon_shard_1");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool shard2 = m_inventory.has_item("iselon_shard_2");
                        if (ImGui::Checkbox("Mageblade Shard 2", &shard2)) {
                            if (shard2) m_inventory.add_item("iselon_shard_2", 1);
                            else m_inventory.remove_item("iselon_shard_2");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool shard3 = m_inventory.has_item("iselon_shard_3");
                        if (ImGui::Checkbox("Mageblade Shard 3", &shard3)) {
                            if (shard3) m_inventory.add_item("iselon_shard_3", 1);
                            else m_inventory.remove_item("iselon_shard_3");
                            m_inventory_dirty = true;
                        }
                        ImGui::SameLine();
                        bool shard4 = m_inventory.has_item("iselon_shard_4");
                        if (ImGui::Checkbox("Mageblade Shard 4", &shard4)) {
                            if (shard4) m_inventory.add_item("iselon_shard_4", 1);
                            else m_inventory.remove_item("iselon_shard_4");
                            m_inventory_dirty = true;
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
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

    // 3. Check if coords hit any active clickable button outside an overlay
    if (buttons) {
        for (int i = 0; i < SRE_BTN_MAX; i++) {
            SreBtnSlot& btn = buttons[i];
            if (!btn.active || btn.hidden || !btn.clickable) continue;
            // Buttons with a parent overlay are already covered by check 2 above
            if (btn.overlay_id[0] != '\0') continue;

            float pw = vp_w * btn.w * btn.scale_x;
            float ph = vp_h * btn.h * btn.scale_y;
            float bx = vp_x + vp_w * btn.cur_x - pw / 2.0f;
            float by = vp_y + vp_h * btn.cur_y - ph / 2.0f;

            if (mx >= bx && mx <= bx + pw && my >= by && my <= by + ph) {
                return true;
            }
        }
    }
    
    return false;
}

extern SrtOverlay g_srt_overlay;

void SwordfareGUI::scan_saves(const std::string& save_dir) {
    g_srt_overlay.save_dir = save_dir;
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
    using namespace SwordfareTheme;

    GuiAction action = GUI_NONE;
    ImGuiIO& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;

    extern GuiRenderer g_gui;
    extern bool g_game_paused;

    float scale = win_h / 720.0f;          // rebased for a 720p-logical UI
    if (scale < 1.0f) scale = 1.0f;
    float bar_h = 38.0f * scale;

    // ---- Docked to the TOP now, not win_h - bar_h -----------------------
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(win_w, bar_h));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      kBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       kBgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border,        kBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,          kText);
    // These are now subtle tints instead of solid opaque blocks — this is
    // the actual fix for "clicking a button selects the whole section".
    ImGui::PushStyleColor(ImGuiCol_Header,        kActiveTint);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHoverTint);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  kActiveTint);

    ImFont* bar_font = static_cast<ImFont*>(m_font_button);
    float font_size = 14.0f * scale;
    if (bar_font) ImGui::PushFont(bar_font);

    ImGui::Begin("##sfw_header_bar", nullptr,
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    float pad_y = (bar_h - font_size) * 0.5f;
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);

    // ── Brand mark ───────────────────────────────────────────────────
    ImGui::TextColored(kCyan, "SWORDFARE");
    ImGui::SameLine(0.0f, 14.0f * scale);
    ImGui::TextColored(kBorder, "|");
    ImGui::SameLine(0.0f, 14.0f * scale);
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);

    auto menu = [&](const char* label, auto fn) {
        if (ImGui::BeginMenu(label)) {
            fn();
            ImGui::EndMenu();
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);
    };

    // ── File ─────────────────────────────────────────────────────────
    menu("File", [&]() {
        if (ImGui::MenuItem("Save State"))        action = GUI_SAVE_STATE;
        if (ImGui::MenuItem("Load State"))        action = GUI_LOAD_STATE;
        ImGui::Separator();
        if (ImGui::MenuItem(g_game_paused ? "Resume" : "Pause", "F8"))
            action = GUI_PAUSE;
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        if (ImGui::MenuItem("Exit Game")) action = GUI_EXIT;
        ImGui::PopStyleColor();
    });

    // ── Tools ────────────────────────────────────────────────────────
    menu("Tools", [&]() {
        if (ImGui::MenuItem("Customize Controls", "F2")) action = GUI_CUSTOMIZE_CONTROLS;
        if (ImGui::MenuItem("Toggle Cam Override", "F5")) action = GUI_TOGGLE_CAM;
        if (ImGui::MenuItem("Toggle Smooth Cam"))          action = GUI_TOGGLE_SMOOTH_CAM;
    });

    // ── Cheats ───────────────────────────────────────────────────────
    menu("Cheats", [&]() {
        ImGui::TextColored(kTextDim, "TOGGLES");
        ImGui::Separator();
        ImGui::Checkbox("God Mode",       &g_gui.mod_god_mode);
        ImGui::Checkbox("Infinite Mana",  &g_gui.mod_infinite_mana);
        ImGui::Checkbox("Fly Mode",       &g_gui.mod_fly_mode);
        ImGui::Checkbox("Infinite Jump",  &g_gui.mod_infinite_jump);
        ImGui::Checkbox("Coin Break",     &g_gui.mod_coin_break);
        ImGui::Spacing();
        ImGui::TextColored(kTextDim, "QUICK ACTIONS");
        ImGui::Separator();
        if (ImGui::MenuItem("Heal Full HP"))  action = GUI_MOD_HEAL_FULL;
        if (ImGui::MenuItem("Refill Mana"))   action = GUI_MOD_REFILL_MANA;
        if (ImGui::MenuItem("+100 Coins"))    action = GUI_MOD_ADD_COINS;
        ImGui::Separator();
        if (ImGui::MenuItem("Level +1"))      action = GUI_MOD_LEVEL_UP;
        if (ImGui::MenuItem("Level -1"))      action = GUI_MOD_LEVEL_DOWN;
        if (ImGui::MenuItem("XP +500"))       action = GUI_MOD_EXP_UP;
        if (ImGui::MenuItem("XP -500"))       action = GUI_MOD_EXP_DOWN;
    });

    // ── Speed ────────────────────────────────────────────────────────
    menu("Speed", [&]() {
        ImGui::SetNextItemWidth(170.0f * scale);
        ImGui::SliderFloat("Walk", &g_gui.mod_walk_speed,  0.5f, 10.0f, "%.1f");
        ImGui::SetNextItemWidth(170.0f * scale);
        ImGui::SliderFloat("Run",  &g_gui.mod_run_speed,   0.5f, 10.0f, "%.1f");
        ImGui::SetNextItemWidth(170.0f * scale);
        ImGui::SliderFloat("Jump", &g_gui.mod_jump_height, 0.5f, 10.0f, "%.1f");
        ImGui::Separator();
        if (ImGui::MenuItem("Speed Up"))    action = GUI_GAME_SPEED_UP;
        if (ImGui::MenuItem("Speed Down"))  action = GUI_GAME_SPEED_DOWN;
        if (ImGui::MenuItem("Speed Reset")) action = GUI_GAME_SPEED_RESET;
    });

    // ── Audio ────────────────────────────────────────────────────────
    menu("Audio", [&]() {
        if (ImGui::MenuItem("Mute / Unmute")) action = GUI_MUSIC_MUTE;
        if (ImGui::MenuItem("Volume Up"))     action = GUI_MUSIC_VOL_UP;
        if (ImGui::MenuItem("Volume Down"))   action = GUI_MUSIC_VOL_DOWN;
    });

    // ── About / Help — plain buttons, not menus, since they just open a panel
    ImGui::SameLine();
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kHoverTint);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kActiveTint);
    if (ImGui::SmallButton("Help"))  m_show_help = true;
    ImGui::SameLine();
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);
    if (ImGui::SmallButton("About")) m_show_about = true;
    ImGui::PopStyleColor(3);

    // ── Right-aligned status + close ────────────────────────────────
    char status_buf[64];
    snprintf(status_buf, sizeof(status_buf), g_game_paused ? "PAUSED" : "RUNNING");
    ImVec2 status_sz = ImGui::CalcTextSize(status_buf);

    const char* close_lbl = "  F1 Close  ";
    ImVec2 close_sz = ImGui::CalcTextSize(close_lbl);

    float right_edge = win_w - 12.0f * scale;
    float close_x    = right_edge - close_sz.x;
    float status_x   = close_x - status_sz.x - 16.0f * scale;

    ImGui::SetCursorPosX(status_x);
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);
    ImGui::TextColored(g_game_paused ? kDanger : kOk, "%s", status_buf);

    ImGui::SetCursorPosX(close_x);
    ImGui::SetCursorPosY(pad_y > 0 ? pad_y : 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDangerHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kDanger);
    ImGui::PushStyleColor(ImGuiCol_Text,          kDanger);
    if (ImGui::SmallButton(close_lbl)) *p_open = false;
    ImGui::PopStyleColor(4);

    // Accent underline across the whole bar (this replaces the old flat
    // block-highlight look with a single thin brand-colored seam).
    ImVec2 win_pos = ImGui::GetWindowPos();
    ImVec2 win_sz  = ImGui::GetWindowSize();
    DrawAccentUnderline(ImVec2(win_pos.x, win_pos.y + win_sz.y - 2.0f),
                        ImVec2(win_pos.x + win_sz.x, win_pos.y + win_sz.y));

    ImGui::End();

    if (bar_font) ImGui::PopFont();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(6);

    // Draw the About/Help panels (self-contained, opened via the buttons above)
    if (m_show_about) draw_about_panel(&m_show_about, bar_h);
    if (m_show_help)  draw_help_panel(&m_show_help, bar_h);

    return action;
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
