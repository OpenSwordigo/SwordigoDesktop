#include "platform/unicorn_dyn.h"
#include <iostream>
#include <string>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdint>

/* ============================================================================
 * unicorn_dyn.cpp — runtime loader for the optional Unicorn Engine backend.
 *
 * Resolution order (mirrors "drop the library next to the app" UX):
 *   1. <exe_dir>/unicorn.dll   (Windows)  or  <exe_dir>/libunicorn.so  (Linux)
 *   2. system search           (LoadLibrary / dlopen with a plain name)
 *
 * A single binary therefore ships on both platforms; Unicorn only needs to
 * exist at runtime, never at build time.
 * ============================================================================ */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

static void* g_handle = nullptr;
static bool g_resolved = false;
static bool g_available = false;
static std::string g_fail_reason = "Unicorn backend is disabled";
static std::mutex g_mutex;

unicorn_api_t unicorn_api;

namespace {

// Location of the running executable, for the "<exe_dir> fallback" search.
std::string exe_directory() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD len = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
    if (len == 0 || len >= sizeof(buf) / sizeof(buf[0])) return std::string();
    std::wstring ws(buf, len);
    size_t slash = ws.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    std::wstring dir = ws.substr(0, slash + 1);
    int need = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(), &out[0], need, nullptr, nullptr);
    return out;
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return std::string();
    buf[len] = '\0';
    std::string path(buf);
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash + 1);
#endif
}

void* open_library(const std::string& name) {
#ifdef _WIN32
    return (void*)LoadLibraryA(name.empty() ? "unicorn.dll" : name.c_str());
#else
    return dlopen(name.empty() ? "libunicorn.so" : name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* handle, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

#define RESOLVE(member, symbol)                                 \
    do {                                                        \
        void* p = find_symbol(g_handle, symbol);                \
        if (!p) {                                               \
            g_fail_reason = std::string("missing symbol ") + symbol; \
            return;                                             \
        }                                                       \
        unicorn_api.member = (decltype(unicorn_api.member))p;   \
    } while (0)

void resolve_all() {
    memset(&unicorn_api, 0, sizeof(unicorn_api));
    RESOLVE(fn_uc_version, "uc_version");
    RESOLVE(fn_uc_open, "uc_open");
    RESOLVE(fn_uc_close, "uc_close");
    RESOLVE(fn_uc_emu_start, "uc_emu_start");
    RESOLVE(fn_uc_emu_stop, "uc_emu_stop");
    RESOLVE(fn_uc_mem_map, "uc_mem_map");
    RESOLVE(fn_uc_mem_map_ptr, "uc_mem_map_ptr");
    RESOLVE(fn_uc_mem_protect, "uc_mem_protect");
    RESOLVE(fn_uc_mem_read, "uc_mem_read");
    RESOLVE(fn_uc_mem_write, "uc_mem_write");
    RESOLVE(fn_uc_reg_read, "uc_reg_read");
    RESOLVE(fn_uc_reg_write, "uc_reg_write");
    RESOLVE(fn_uc_hook_add, "uc_hook_add");
    RESOLVE(fn_uc_strerror, "uc_strerror");
}

} // namespace

int unicorn_backend_available(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_resolved) return g_available ? 1 : 0;

    g_resolved = true;

    // 1. Explicit app-dir path (drop-in lib support).
    std::string exe_dir = exe_directory();
    std::vector<std::string> candidates;
    if (!exe_dir.empty()) {
#ifdef _WIN32
        candidates.push_back(exe_dir + "unicorn.dll");
#else
        candidates.push_back(exe_dir + "libunicorn.so");
#endif
    }
    // 2. System search (package installs, PATH, LD_LIBRARY_PATH...).
    candidates.push_back("");

    for (const auto& cand : candidates) {
        g_handle = open_library(cand);
        if (g_handle) {
            resolve_all();
            if (unicorn_api.fn_uc_version && unicorn_api.fn_uc_open) {
                g_available = true;
                g_fail_reason = "";
                unsigned int major = 0, minor = 0;
                unsigned int ver = unicorn_api.fn_uc_version(&major, &minor);
                std::cout << "[Unicorn] Runtime backend loaded (version "
                          << (int)((ver >> 24) & 0xff) << "." << (int)((ver >> 16) & 0xff)
                          << ")" << std::endl;
                return 1;
            }
        }
    }

    g_fail_reason = "libunicorn not found (drop libunicorn.so / unicorn.dll next to the app, "
                    "or install the Unicorn Engine package)";
    std::cerr << "[Unicorn] Backend unavailable: " << g_fail_reason << std::endl;
    return 0;
}

const char* unicorn_backend_error(void) {
    return g_fail_reason.c_str();
}
