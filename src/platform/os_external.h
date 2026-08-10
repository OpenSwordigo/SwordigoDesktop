#ifndef OS_EXTERNAL_H
#define OS_EXTERNAL_H

/* ============================================================================
 * os_external.h — thin cross-platform shim over OS-specific primitives.
 *
 * Everything here that is Linux-only in the codebase is funneled through these
 * helpers so the rest of the app can compile on Windows without #ifdef soup.
 * Linux uses mmap/fork/xdg-open/readlink; Windows uses VirtualAlloc/CreateProcess
 * /ShellExecuteW/GetModuleFileNameW.
 * ============================================================================ */

#include <string>
#include <vector>
#include <cstdint>

namespace os_external {

// Directory containing the running executable (empty string on failure).
// Linux: readlink("/proc/self/exe")   Windows: GetModuleFileNameW
std::string exe_dir();

// Closest ancestor of the executable folder that holds the repo's src/assets
// tree (empty string when not applicable). Let dev builds find src/assets.
std::string dev_root_dir();

// On Windows, chdir() to the dev root so CWD-relative "src/assets/..." asset
// paths resolve even when the binary is launched from bin/. No-op elsewhere.
bool set_dev_working_dir();

// User home directory. Linux: $HOME   Windows: %USERPROFILE%.
std::string home_dir();

// Open a folder (or file) in the OS file manager, detached.
// Linux: fork + xdg-open   Windows: ShellExecuteW "open"
void open_in_file_manager(const std::string& path);

// Launch an executable detached (no waiting, no console), best-effort.
// Linux: fork + execlp   Windows: CreateProcessW (detached)
void spawn_detached(const std::string& program, const std::vector<std::string>& args);

// Replace the current process with a fresh copy of itself, re-running with
// `args`. Used by the in-game death recovery restart. Does not return on
// success (Linux); on Windows it spawns a new process and the caller exits.
void restart_process(const std::vector<std::string>& args);

// --- Dynamic library loading (dlopen family). Linux uses dlopen/dlsym/dlclose;
//     Windows uses LoadLibraryW/GetProcAddress/FreeLibrary. ---
void* load_library(const std::string& path);       // nullptr on failure
void* find_symbol(void* library, const char* name); // nullptr on failure
void  close_library(void* library);
std::string library_error();                        // last error message

// Reserve a large virtual address region without committing (lazy zero pages),
// e.g. the 4 GiB ARM64 guest address space. Returns nullptr on failure.
// Linux: mmap MAP_ANONYMOUS|MAP_NORESERVE   Windows: VirtualAlloc MEM_RESERVE
void* reserve_large_region(uint64_t size);

} // namespace os_external

// Portable weak-symbol declaration. GNU toolchains use __attribute__((weak));
// MSVC has no direct equivalent, so it degrades to a normal strong definition
// (the app still links; the guest-override trick simply isn't available there).
#ifndef SWORDIGO_WEAK
#  if defined(_MSC_VER)
#    define SWORDIGO_WEAK
#  else
#    define SWORDIGO_WEAK __attribute__((weak))
#  endif
#endif

#endif // OS_EXTERNAL_H
