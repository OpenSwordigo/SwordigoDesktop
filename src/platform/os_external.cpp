#include "platform/os_external.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <dlfcn.h>
#endif

namespace os_external {

std::string exe_dir() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (len == 0 || len >= sizeof(buf) / sizeof(buf[0])) return std::string();
    std::wstring ws(buf, len);
    size_t slash = ws.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    std::wstring dir = ws.substr(0, slash + 1);
    int need = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(),
                        &out[0], need, nullptr, nullptr);
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

std::string home_dir() {
#ifdef _WIN32
    const char* p = std::getenv("USERPROFILE");
    return p ? p : std::string();
#else
    const char* p = std::getenv("HOME");
    return p ? p : std::string();
#endif
}

void open_in_file_manager(const std::string& path) {
#ifdef _WIN32
    // Expand path to a full wide string so ShellExecuteW finds it.
    std::wstring ws;
    {
        int need = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(),
                                       nullptr, 0);
        if (need <= 0) return;
        ws.resize(need);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(),
                            &ws[0], need);
    }
    ShellExecuteW(nullptr, L"open", ws.c_str(), nullptr, nullptr, SW_SHOW);
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
        _exit(1);
    }
#endif
}

void spawn_detached(const std::string& program,
                    const std::vector<std::string>& args) {
#ifdef _WIN32
    std::wstring ws_program;
    {
        int need = MultiByteToWideChar(CP_UTF8, 0, program.c_str(),
                                       (int)program.size(), nullptr, 0);
        if (need > 0) {
            ws_program.resize(need);
            MultiByteToWideChar(CP_UTF8, 0, program.c_str(), (int)program.size(),
                                &ws_program[0], need);
        }
    }
    // Build a quoted command line.
    std::wstring cmdline = L"\"" + ws_program + L"\"";
    for (const auto& a : args) {
        std::wstring ws;
        {
            int need = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(),
                                           nullptr, 0);
            if (need > 0) {
                ws.resize(need);
                MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(),
                                    &ws[0], need);
            }
        }
        cmdline += L" \"" + ws + L"\"";
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(ws_program.empty() ? nullptr : ws_program.c_str(),
                       &cmdline[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<const char*> argv;
        argv.push_back(program.c_str());
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        execvp(program.c_str(), (char* const*)argv.data());
        _exit(1);
    }
#endif
}

void restart_process(const std::vector<std::string>& args) {
#ifdef _WIN32
    spawn_detached(exe_dir() + "swordfare.exe", args);
#else
    std::vector<char*> argv;
    for (auto& s : const_cast<std::vector<std::string>&>(args)) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    execv("/proc/self/exe", argv.data());
    _exit(1);
#endif
}

void* reserve_large_region(uint64_t size) {
#ifdef _WIN32
    // Match Linux mmap semantics: the guest writes across the whole 4GB window
    // (bridge at 0xFF000000, ELF load, heap/stack). MEM_RESERVE alone leaves
    // pages unavtoridably inaccessible and the first touch faults (0xC0000005).
    // RESERVE|COMMIT charges the address space; physical pages are still
    // demand-zeroed on first access, so it's the analogue of MAP_ANONYMOUS.
    return VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

void* load_library(const std::string& path) {
#ifdef _WIN32
    std::wstring ws;
    {
        int need = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(),
                                       nullptr, 0);
        if (need <= 0) return nullptr;
        ws.resize(need);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(),
                            &ws[0], need);
    }
    return (void*)LoadLibraryW(ws.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* library, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)library, name);
#else
    return dlsym(library, name);
#endif
}

void close_library(void* library) {
#ifdef _WIN32
    if (library) FreeLibrary((HMODULE)library);
#else
    if (library) dlclose(library);
#endif
}

std::string library_error() {
#ifdef _WIN32
    DWORD err = GetLastError();
    if (err == 0) return std::string();
    char* msg = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                             (LPSTR)&msg, 0, nullptr);
    std::string out;
    if (n && msg) {
        out = std::string(msg, n);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    }
    if (msg) LocalFree(msg);
    return out;
#else
    const char* d = dlerror();
    return d ? std::string(d) : std::string();
#endif
}

// Return the "dev root" — the closest ancestor of the executable folder that
// holds the repo's src/assets tree (and usually engine/assets). Used so the
// launcher and asset viewer can resolve dev-mode `src/assets/...` paths no
// matter where the binary was launched from. Empty string when not applicable.
std::string dev_root_dir() {
    std::string dir = exe_dir();
    if (dir.empty()) return std::string();
    namespace fs = std::filesystem;
    fs::path p(dir);
    while (p.has_relative_path() && p != p.root_path()) {
        if (fs::exists(p / "src" / "assets"))
            return p.string();
        p = p.parent_path();
        if (p.empty()) break;
    }
    return std::string();
}

// On Windows, chdir() to the dev root (if found) so CWD-relative asset paths
// like "src/assets/..." resolve from the repository layout. No-op elsewhere.
bool set_dev_working_dir() {
#ifdef _WIN32
    std::string root = dev_root_dir();
    if (root.empty()) return false;
    int need = MultiByteToWideChar(CP_UTF8, 0, root.c_str(), (int)root.size(), nullptr, 0);
    if (need > 0) {
        std::wstring wroot(need, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, root.c_str(), (int)root.size(), &wroot[0], need);
        SetCurrentDirectoryW(wroot.c_str());
    }
    std::cout << "[os_external] Dev working dir set to: " << root << std::endl;
    return true;
#else
    return false;
#endif
}

} // namespace os_external
