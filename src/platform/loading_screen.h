#pragma once
// =============================================================================
// loading_screen.h — Swordigo Desktop boot loading screen.
//
// A self-contained loading screen rendered on the MAIN game window's existing
// OpenGL 2.1 compatibility context (the same one Display::init creates). It is
// shown in the gap between window creation and the game's first rendered frame
// (ELF load → relocate → symbol resolve → applicationDidBecomeActive), which
// is otherwise a blank/disappeared window.
//
// It renders, via Dear ImGui on top of a GL-cleared frame:
//   * the Swordigo Desktop logo + wordmark,
//   * the hiro "run" POD animation (loaded through av::pod_load + the PVR
//     texture loader) skinned/animated per frame, drawn with fixed-function
//     GL 2.1 — with a graceful spinner fallback when the asset can't be found,
//   * an accent progress bar (sf_theme) driven by the booting thread,
//   * randomized "fun" flavor lines, and
//   * watchdog messages at 10s / 20s / 30s of wall-clock boot time.
//
// Threading model: the heavy boot runs on the calling thread. It periodically
// calls LoadingScreen::set_progress()/set_stage() (thread-safe) and, on the
// render thread (the one that owns the GL context), calls frame() to draw one
// loading frame. This keeps the window responsive and showing motion while the
// synchronous boot proceeds.
// =============================================================================

#include <string>
#include <atomic>
#include <mutex>

struct SDL_Window;

class LoadingScreen {
public:
    LoadingScreen() = default;
    ~LoadingScreen();

    // Initialize on the current GL context (must be current on the calling
    // thread). `window` is the main game window. `imgui_ctx` is the ALREADY
    // initialized ImGui context owned by SwordfareGUI (whose SDL3+OpenGL3
    // backends are the shared, global-static ImGui backends). The loading
    // screen reuses that context/backend and must NEVER init or shut it down —
    // doing so previously tore the backend out from under the game GUI and
    // crashed begin_frame(). Loads the logo + hiro run POD (non-fatal if
    // missing). Returns false only if imgui_ctx is null.
    bool init(SDL_Window* window, void* imgui_ctx);

    // Render exactly one loading frame: clears the GL backbuffer, draws the
    // hiro animation, then the ImGui overlay (logo, progress, messages), and
    // swaps the window. Call from the thread owning the GL context.
    void frame();

    // Thread-safe progress reporting from the boot thread.
    void set_progress(float p01);                 // clamped 0..1
    void set_stage(const std::string& stage);     // e.g. "Relocating symbols"
    void set_slideshow_mode(bool enabled) { bg_slideshow_mode_ = enabled; }

    // Mark the boot as finished; the caller should stop calling frame() after.
    void finish();

    // Tear down ImGui/GL resources created by init(). Safe to call once.
    void shutdown();

    // Wall-clock seconds since init() (for external watchdog logic if needed).
    float elapsed() const;

private:
    void draw_overlay();          // ImGui pass
    void draw_hiro();             // GL 2.1 fixed-function POD pass
    bool load_hiro_animation();   // resolve + parse the hiro run POD

    SDL_Window* window_ = nullptr;
    bool        inited_        = false;
    bool        own_imgui_ctx_ = false;
    void*       imgui_ctx_     = nullptr; // shared SwordfareGUI ImGui context

    // Progress state (written by boot thread, read by render thread).
    std::atomic<float> progress_{0.0f};
    std::mutex         stage_mtx_;
    std::string        stage_ = "Starting up";

    double start_time_ = 0.0;   // ImGui::GetTime() at init
    int    last_watchdog_ = -1; // highest watchdog tier already latched (0/1/2)

    // hiro POD animation state.
    bool   hiro_ok_        = false;
    float  hiro_frame_     = 0.0f;
    void*  hiro_model_     = nullptr;  // av::PODModel* (opaque to avoid header dep)
    // hiro is a 5-material model (body cloth, face, legs, plate, trim) — each
    // mesh node references its own material/texture. We load ONE GL texture per
    // material and bind the correct one per node while drawing, otherwise every
    // body part gets the same (wrong) texture. Indexed by material index.
    void*  hiro_mat_tex_   = nullptr;  // std::vector<unsigned int>* (GLuint per material)

    // logo texture (GLuint; 0 = none, falls back to text wordmark).
    // Prefer the real in-game swordigo_title PVR; else an embedded PNG.
    unsigned int logo_tex_ = 0;
    int          logo_w_   = 0;
    int          logo_h_   = 0;

    // Full-screen scenery background (real game .tex.png) for a rich backdrop.
    unsigned int bg_tex_   = 0;
    int          bg_w_     = 0;
    int          bg_h_     = 0;
    // Slideshow: cycle backgrounds every few seconds.
    int    bg_slideshow_idx_ = 0;  // current index in kBackgrounds
    double bg_slideshow_next_ = 0; // next swap time
    bool   bg_slideshow_mode_ = false; // true = cycle, false = single static

    // Soft glow sprite used behind the logo / on the progress fill.
    unsigned int spark_tex_ = 0;

    // Ambient floating motes (positions are normalized 0..1).
    void*  motes_          = nullptr;  // std::vector<Mote>*
    int    mote_count_     = 0;

    // Randomized flavor line, reshuffled occasionally.
    int    flavor_idx_      = 0;
    double flavor_next_swap_ = 0.0;
};
