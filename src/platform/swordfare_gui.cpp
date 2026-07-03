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
    const char* main_font_paths[] = {
        "src/assets/fonts/Redaction10-Regular.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Redaction10-Regular.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf"
    };
    for (auto* fp : main_font_paths) {
        if (std::filesystem::exists(fp)) {
            main_font_path = fp;
            break;
        }
    }

    const char* button_font_paths[] = {
        "src/assets/fonts/Redaction10-Bold.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Redaction10-Bold.otf",
        "src/assets/fonts/Inter-Regular.ttf",
        "/usr/share/swordigo-desktop/src/assets/fonts/Inter-Regular.ttf"
    };
    for (auto* fp : button_font_paths) {
        if (std::filesystem::exists(fp)) {
            button_font_path = fp;
            break;
        }
    }

    const char* fa_paths[] = {
        "src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "src/assets/fonts/fa-solid-900.ttf",
        "/usr/share/swordigo-desktop/src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf",
        "/usr/share/swordigo-desktop/src/assets/fonts/fa-solid-900.ttf"
    };
    for (auto* fp : fa_paths) {
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
    
    // If the main overlay is not visible, we only swallow mouse clicks if they land on active overlay buttons
    ImGuiIO& io = ImGui::GetIO();
    if (!m_visible) {
        // If debug/mods menu is hidden, we only capture click if it actually interacts with an active ImGui button
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            return io.WantCaptureMouse;
        }
        return false;
    }
    
    // If main overlay is visible, normal capture applies
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        return io.WantCaptureMouse;
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        return io.WantCaptureKeyboard;
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
#define SRE_BTN_MAX       32
#define SRE_BTN_ID_LEN    32
#define SRE_BTN_LABEL_LEN 64

struct SreBtnSlot {
    char     id[SRE_BTN_ID_LEN];
    char     label[SRE_BTN_LABEL_LEN];
    float    x, y;
    float    w, h;
    float    alpha;
    float    scale_x, scale_y;
    int      text_color;
    float    text_scale;
    int      bg_alpha;
    int      hidden;
    int      clickable;
    int      movable;
    int      snapback;
    float    home_x, home_y;
    int      padding_l, padding_t, padding_r, padding_b;
    int      alignment;
    /* ---- STATE (written by host, read by SRE) ---- */
    volatile int pressed;
    volatile int released;
    volatile int dragging;
    volatile float cur_x, cur_y;
    int      active;
    int      dirty;
};

void SwordfareGUI::draw_buttons(void* guest_buttons_ptr) {
    if (!m_initialized || !guest_buttons_ptr) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_ctx));

    // Retrieve active viewport bounds for correct button positioning
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

    SreBtnSlot* buttons = static_cast<SreBtnSlot*>(guest_buttons_ptr);

    for (int i = 0; i < SRE_BTN_MAX; i++) {
        SreBtnSlot& btn = buttons[i];
        if (!btn.active || btn.hidden) continue;

        // Compute screen coordinates (Y=0 at top in ImGui space)
        float pw = base * btn.w * btn.scale_x;
        float ph = base * btn.h * btn.scale_y;
        float bx = vp_x + vp_w * btn.cur_x - pw / 2.0f;
        float by = vp_y + vp_h * btn.cur_y - ph / 2.0f;

        // Custom window flag setup to overlay the button cleanly
        char win_name[64];
        snprintf(win_name, sizeof(win_name), "##sre_btn_%d", i);

        ImGui::SetNextWindowPos(ImVec2(bx, by), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));

        // Use custom styling matching the premium dark theme
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.13f, 0.18f, (btn.bg_alpha / 255.0f) * (btn.alpha / 255.0f)));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.914f, 0.271f, 0.376f, 0.40f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                 ImGuiWindowFlags_NoSavedSettings;
        if (!btn.clickable) flags |= ImGuiWindowFlags_NoInputs;

        if (ImGui::Begin(win_name, nullptr, flags)) {
            // Button interaction
            bool hovered = ImGui::IsWindowHovered();
            bool active = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hovered;

            if (hovered) {
                // Glow border color if hovered
                ImGui::GetWindowDrawList()->AddRect(ImVec2(bx, by), ImVec2(bx + pw, by + ph), 
                                                    ImGui::GetColorU32(ImVec4(0.914f, 0.271f, 0.376f, 0.80f)), 6.0f, 0, 1.5f);
            }

            // Sync interactions back to SRE guest memory
            if (active) {
                btn.pressed = 1;
            } else {
                if (btn.pressed == 1) {
                    btn.pressed = 0;
                    btn.released = 1; // triggers click event guest-side
                    std::cout << "[Swordfare GUI] Button clicked - Index: " << i 
                              << ", Label: '" << btn.label 
                              << "', HomePos: (" << btn.home_x << ", " << btn.home_y << ")" << std::endl;
                }
            }

            // Draw label text aligned in center
            const char* display_label = btn.label;
            if (i == 0 && strlen(display_label) == 0) {
                display_label = ICON_FA_GEAR;
            }

            if (strlen(display_label) > 0) {
                ImGui::PushFont(static_cast<ImFont*>(m_font_button));
                ImVec2 tsize = ImGui::CalcTextSize(display_label);
                float tx = (pw - tsize.x) / 2.0f;
                float ty = (ph - tsize.y) / 2.0f;
                ImGui::SetCursorPos(ImVec2(tx, ty));

                // Color parsing (packed ARGB or text_color parameter)
                unsigned int c_val = btn.text_color;
                float cr = ((c_val >> 16) & 0xFF) / 255.0f;
                float cg = ((c_val >> 8) & 0xFF) / 255.0f;
                float cb = (c_val & 0xFF) / 255.0f;
                float ca = ((c_val >> 24) & 0xFF) / 255.0f;
                if (ca == 0.0f) ca = btn.alpha / 255.0f;

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(cr, cg, cb, ca));
                ImGui::TextUnformatted(display_label);
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }
}

