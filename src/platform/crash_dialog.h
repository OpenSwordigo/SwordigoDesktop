#pragma once
// =============================================================================
// crash_dialog.h — sleek fatal-crash reporting window for Swordigo Desktop.
//
// install_crash_handler() registers POSIX signal handlers (SIGSEGV, SIGABRT,
// SIGILL, SIGFPE, SIGBUS). When one fires, instead of the process silently
// dying, a self-contained SDL3 + Dear ImGui window is spun up showing:
//   * the fatal signal + a human-readable description,
//   * the faulting address (where available),
//   * a symbolized backtrace (via backtrace_symbols),
//   * a short guidance line ("Run swordfare in a terminal for full logs"), and
//   * Copy details / Close buttons.
//
// The dialog uses the shared sf_theme design system so it matches the launcher,
// loading screen, and overlays. It creates its OWN SDL window + GL context so
// it works even when the game's GL context is wedged. After the dialog closes
// the process is terminated with the original signal disposition.
//
// You can also raise the dialog manually for a non-signal fatal error via
// show_crash_dialog(title, detail).
// =============================================================================

#include <string>

namespace crashui {

// Install signal handlers. Call once, early in main() (after SDL is available).
void install_crash_handler();

// Show the crash dialog for an arbitrary fatal condition (blocks until closed).
// Safe to call from the main thread when the GL context may be usable or not.
void show_crash_dialog(const std::string& title, const std::string& detail);

} // namespace crashui
