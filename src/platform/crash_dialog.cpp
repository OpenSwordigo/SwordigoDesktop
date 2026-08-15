// =============================================================================
// crash_dialog.cpp — implementation of the fatal-crash reporting window.
// See crash_dialog.h for the contract.
// =============================================================================

#include "platform/crash_dialog.h"
#include "platform/swordfare_theme.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl3.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <execinfo.h>
#include <unistd.h>
#endif

namespace crashui {

namespace {

// Guard against reentrancy: a crash inside the crash dialog must not loop.
volatile sig_atomic_t g_in_handler = 0;

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGFPE:  return "SIGFPE (floating-point exception)";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS (bus error)";
#endif
        default:      return "Fatal signal";
    }
}

std::string capture_backtrace() {
    std::string out;
#ifndef _WIN32
    void* frames[64];
    int n = backtrace(frames, 64);
    char** syms = backtrace_symbols(frames, n);
    if (syms) {
        // Skip the top couple frames (the handler itself).
        for (int i = 2; i < n; ++i) {
            out += "  ";
            out += syms[i];
            out += "\n";
        }
        free(syms);
    }
#endif
    if (out.empty()) out = "  (backtrace unavailable)\n";
    return out;
}

// Render the dialog on its own SDL window until the user closes it.
void run_dialog_window(const std::string& title, const std::string& detail) {
    // Ensure video is up (SDL_Init is idempotent for already-inited subsystems).
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            // Can't show a GUI — fall back to stderr and bail.
            std::fprintf(stderr, "\n===== %s =====\n%s\n", title.c_str(), detail.c_str());
            return;
        }
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const int W = 760, H = 520;
    SDL_Window* win = SDL_CreateWindow("Swordigo Desktop — Crash Report", W, H,
                                       SDL_WINDOW_OPENGL);
    if (!win) {
        std::fprintf(stderr, "\n===== %s =====\n%s\n", title.c_str(), detail.c_str());
        return;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_DestroyWindow(win);
        std::fprintf(stderr, "\n===== %s =====\n%s\n", title.c_str(), detail.c_str());
        return;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    // Dedicated ImGui context for the dialog.
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* dlg  = ImGui::CreateContext();
    ImGui::SetCurrentContext(dlg);
    ImGui::GetIO().IniFilename = nullptr;
    sf_theme::ApplyTheme();
    ImGui_ImplSDL3_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    bool open = true;
    while (open) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) open = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) open = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) open = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##crash", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Danger header bar.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wmin = ImGui::GetWindowPos();
        ImVec2 wmax = ImVec2(wmin.x + ImGui::GetWindowWidth(), wmin.y + 54.0f);
        sf_theme::VerticalGradient(dl, wmin, wmax,
                                   sf_theme::Col_Danger,
                                   sf_theme::Lerp(sf_theme::Col_Danger, sf_theme::Col_Bg, 0.6f),
                                   0.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, sf_theme::Vec_Text);
        ImGui::SetCursorPos(ImVec2(20, 15));
        ImGui::Text("Swordigo Desktop has crashed");
        ImGui::PopStyleColor();

        ImGui::SetCursorPosY(70);
        ImGui::Indent(20);

        sf_theme::SectionHeader(title.c_str());

        // Scrollable detail region.
        ImGui::BeginChild("##detail", ImVec2(ImGui::GetContentRegionAvail().x - 20, 
                                             ImGui::GetContentRegionAvail().y - 60), true);
        ImGui::PushStyleColor(ImGuiCol_Text, sf_theme::Vec_TextDim);
        ImGui::TextUnformatted(detail.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();

        ImGui::Spacing();
        if (sf_theme::PrimaryButton("Copy details", ImVec2(150, 0))) {
            SDL_SetClipboardText((title + "\n\n" + detail).c_str());
        }
        ImGui::SameLine();
        if (sf_theme::GhostButton("Close", ImVec2(120, 0))) {
            open = false;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, sf_theme::Vec_TextMuted);
        ImGui::TextWrapped("Tip: run  swordfare  in a terminal to see the full stuck/crash logs.");
        ImGui::PopStyleColor();

        ImGui::Unindent(20);
        ImGui::End();

        ImGui::Render();
        int fw, fh; SDL_GetWindowSizeInPixels(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.055f, 0.067f, 0.086f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(dlg);
    if (prev) ImGui::SetCurrentContext(prev);

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
}

void fatal_signal_handler(int sig) {
    if (g_in_handler) {
        // Second crash while handling — restore default and re-raise now.
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_in_handler = 1;

    std::string title = signal_name(sig);
    std::string detail;
    detail  = "Signal: ";
    detail += signal_name(sig);
    detail += "\n\nBacktrace:\n";
    detail += capture_backtrace();
    detail += "\nThe game runtime hit a fatal error. This is usually a bad guest\n";
    detail += "memory access inside the emulated game or a host bridge call.\n";

    // Also echo to stderr for terminal users / log capture.
    std::fprintf(stderr, "\n===== Swordigo Desktop crash =====\n%s\n", detail.c_str());

    run_dialog_window(title, detail);

    // Restore default disposition and re-raise so the OS still records the crash.
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void install_crash_handler() {
#ifndef _WIN32
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND; // one-shot; our handler re-raises anyway
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
#ifdef SIGBUS
    sigaction(SIGBUS,  &sa, nullptr);
#endif
#else
    std::signal(SIGSEGV, fatal_signal_handler);
    std::signal(SIGABRT, fatal_signal_handler);
    std::signal(SIGILL,  fatal_signal_handler);
    std::signal(SIGFPE,  fatal_signal_handler);
#endif
}

void show_crash_dialog(const std::string& title, const std::string& detail) {
    run_dialog_window(title, detail);
}

} // namespace crashui
