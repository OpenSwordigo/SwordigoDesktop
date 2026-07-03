// =============================================================================
// Swordfare GUI — Modern ImGui-Based In-Game Overlay  (v7.3)
//
// A sleek, modern overlay rendered on top of the running game using Dear ImGui
// (SDL3 + OpenGL3 backends). Replaces the old bitmap-drawn F3 debug panel with
// a proper dockable, styled ImGui window.
//
// Design goals:
//   • Non-bitmap: uses ImGui vector rendering — crisp at all resolutions.
//   • Dark + glassmorphism-inspired theme consistent with the launcher UI.
//   • Minimal overhead — only renders when visible.
//   • Easily extensible: future panels (controls editor, camera, mods) can be
//     added as new SwardfarePanel subclasses without touching main.cpp.
//
// Usage:
//   SwordfareGUI gui;
//   gui.init(sdl_window, gl_context);        // call once after GL context ready
//   // per-frame (after game draw, before swap):
//   gui.begin_frame();
//   if (gui.is_visible()) gui.draw_debug(fps, dt, ...);
//   gui.end_frame();
//   gui.shutdown();                          // call on exit
// =============================================================================

#pragma once

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"

#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
// Debug stats snapshot (passed to draw_debug each frame)
// ---------------------------------------------------------------------------
struct SwordfareDebugStats {
    float   fps           = 0.0f;
    float   dt_seconds    = 0.0f;
    int     frame_count   = 0;
    int     draw_calls    = 0;
    int     tex_binds     = 0;
    int     vertices      = 0;
    int     matrix_ops    = 0;
    int     state_changes = 0;
    int     tex_uploads   = 0;
    int     win_w         = 0, win_h = 0;
    int     draw_w        = 0, draw_h = 0;
    int     mouse_x       = 0, mouse_y = 0;
    float   cam_x         = 0.0f, cam_y = 0.0f, cam_z = 0.0f;
    float   cam_zoom      = 1.0f;
    bool    cam_active    = false;
    bool    typing_mode   = false;
    bool    game_paused   = false;
    bool    postfx_on     = false;
    const char* postfx_preset  = "Off";
    const char* scale_mode     = "Sharp-Bilinear";
    const char* binary_name    = "";
    const char* speed_label    = "1×";
    const char* graphics_api   = "OpenGL";
};

// ---------------------------------------------------------------------------
// SwordfareGUI — main class
// ---------------------------------------------------------------------------
class SwordfareGUI {
public:
    SwordfareGUI()  = default;
    ~SwordfareGUI() = default;

    // Lifecycle
    void init(SDL_Window* window, SDL_GLContext gl_ctx);
    void shutdown();

    // Event pass-through — call before your own SDL event handling
    // Returns true if ImGui consumed the event (caller should not process it).
    bool process_event(const SDL_Event& event);

    // Frame management — wrap the game's GL draw calls
    void begin_frame();
    void end_frame();     // renders ImGui draw data into the GL context

    // Toggle visibility (called on F3)
    void toggle_visible() { m_visible = !m_visible; }
    bool is_visible()     const { return m_visible; }

    // Draw the debug panel — call between begin_frame()/end_frame() when visible
    void draw_debug(const SwordfareDebugStats& stats);

    // Draw the custom LUA-registered buttons using ImGui vector elements
    void draw_buttons(void* guest_buttons_ptr);

private:
    // Internal helpers
    void apply_swordfare_theme();

    SDL_Window*   m_window   = nullptr;
    SDL_GLContext m_gl_ctx   = nullptr;
    void*         m_imgui_ctx = nullptr;   // ImGuiContext*
    void*         m_font_main = nullptr;   // ImFont*
    void*         m_font_button = nullptr; // ImFont*

    bool          m_initialized = false;
    bool          m_visible     = false;

    // Smooth FPS graph data
    static constexpr int FPS_HISTORY = 90;
    float m_fps_history[FPS_HISTORY] = {};
    int   m_fps_idx = 0;
};
