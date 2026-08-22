// win_dll_dir.cpp — Windows-only: make the app load its shipped DLLs from a
// `libs\` folder next to the executable, mirroring the Linux `bin/libs` layout
// (where an $ORIGIN/libs rpath does the same job).
//
// Unlike ELF rpath, a Windows PE has no built-in "look in ./libs" behaviour:
// the loader only searches the exe's own directory, the system dirs and PATH.
// We register the exe-relative `libs\` directory with the OS loader *before*
// main() runs (via a GCC constructor) so both statically-imported DLLs and any
// later LoadLibrary calls resolve from there.
//
// Compiled into the executable targets only on WIN32; a no-op everywhere else.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>

namespace {

__attribute__((constructor))
void swordigo_register_libs_dir() {
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    // Strip the executable file name, keeping the directory (with trailing sep).
    DWORD dir_len = 0;
    for (DWORD i = len; i > 0; --i) {
        if (exe_path[i - 1] == L'\\' || exe_path[i - 1] == L'/') {
            dir_len = i;
            break;
        }
    }
    if (dir_len == 0 || dir_len + 5 >= MAX_PATH) return; // room for "libs\0"

    wchar_t libs_dir[MAX_PATH];
    wmemcpy(libs_dir, exe_path, dir_len);
    libs_dir[dir_len] = L'\0';
    wcscat(libs_dir, L"libs");

    // Primary path: extend the *default* DLL search set so statically-imported
    // DLLs (SDL3, the sw* libs, …) resolve from libs\ at load time. Available
    // on Windows 8+ / KB2533623-patched Win7.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        typedef BOOL (WINAPI *SetDefaultDllDirectories_t)(DWORD);
        typedef void*(WINAPI *AddDllDirectory_t)(PCWSTR);
        auto p_set = reinterpret_cast<SetDefaultDllDirectories_t>(
            GetProcAddress(k32, "SetDefaultDllDirectories"));
        auto p_add = reinterpret_cast<AddDllDirectory_t>(
            GetProcAddress(k32, "AddDllDirectory"));
        if (p_set && p_add) {
            // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000
            p_set(0x00001000);
            p_add(libs_dir);
        }
    }

    // Fallback that also covers older loaders and plain-name LoadLibrary calls.
    SetDllDirectoryW(libs_dir);
}

} // namespace
#endif // _WIN32
