#include "platform/data_path.h"
#include "platform/os_external.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#ifndef _WIN32
#include <unistd.h>
#include <pwd.h>
#endif

extern std::string g_assets_dir;
extern std::string g_instance_assets_dir;

namespace fs = std::filesystem;

// ============================================================
//  User data directory: ~/.local/share/swordigo-desktop/
//  Like Minecraft's ~/.minecraft/ — writable, user-owned.
// ============================================================

static std::string s_user_data_dir_cache;
static std::string s_system_data_dir_cache;
static bool s_dirs_resolved = false;

static void resolve_dirs() {
    if (s_dirs_resolved) return;
    s_dirs_resolved = true;

#ifndef _WIN32
    // User data dir: $XDG_DATA_HOME/swordigo-desktop/ or ~/.local/share/swordigo-desktop/
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        s_user_data_dir_cache = std::string(xdg) + "/swordigo-desktop/";
    } else {
        const char* home = getenv("HOME");
        if (!home) {
            struct passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        s_user_data_dir_cache = std::string(home) + "/.local/share/swordigo-desktop/";
    }

    // System install dir: check standard paths
    const char* sys_paths[] = {
        "/usr/share/swordigo/",
        "/usr/local/share/swordigo/",
        nullptr
    };
    for (int i = 0; sys_paths[i]; i++) {
        if (fs::exists(sys_paths[i])) {
            s_system_data_dir_cache = sys_paths[i];
            break;
        }
    }
#else
    // Windows fallback
    const char* appdata = getenv("APPDATA");
    s_user_data_dir_cache = std::string(appdata ? appdata : ".") + "/swordigo-desktop/";
#endif
}

std::string get_user_data_dir() {
    resolve_dirs();
    // Create if needed
    if (!s_user_data_dir_cache.empty()) {
        try { fs::create_directories(s_user_data_dir_cache); } catch (...) {}
    }
    return s_user_data_dir_cache;
}

// C-linkage wrapper for asset_manager.c (pure C can't call std::string functions)
static char s_data_dir_c[512] = {0};
extern "C" char* get_user_data_dir_c(void) {
    std::string dir = get_user_data_dir();
    strncpy(s_data_dir_c, dir.c_str(), sizeof(s_data_dir_c) - 1);
    return s_data_dir_c;
}

std::string get_system_data_dir() {
    resolve_dirs();
    return s_system_data_dir_cache;
}

SWORDIGO_WEAK std::string g_save_dir;

std::string get_vfs_save_dir(const std::string& custom_base) {
    std::string base = custom_base;
    if (base.empty()) {
        base = g_save_dir;
    }
    if (base.empty()) {
        base = get_user_data_dir() + "save";
    }
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    if (base.length() >= 10 && base.substr(base.length() - 10) == "/Documents") {
        return base;
    }
    return base + "/Documents";
}

// ============================================================
//  First-run setup: copy from /usr/share/swordigo/ → ~/.local/share/swordigo-desktop/
// ============================================================

bool ensure_user_data() {
    std::string user_dir = get_user_data_dir();
    std::string sys_dir = get_system_data_dir();

    if (user_dir.empty()) return false;

    // Check if user data already exists (has engine/ or assets/)
    if (fs::exists(user_dir + "engine/manifest.json") && fs::exists(user_dir + "assets/")) {
        std::cout << "[DataPath] User data exists at " << user_dir << std::endl;
        return false; // Already set up
    }

    // If no system install, check CWD (dev mode)
    if (sys_dir.empty()) {
        if (fs::exists("./engine/manifest.json") && fs::exists("./assets/")) {
            std::cout << "[DataPath] Running in dev mode (CWD has game data)" << std::endl;
            return false; // Dev mode — use CWD directly
        }
        std::cout << "[DataPath] No system install and no local data found" << std::endl;
        return false;
    }

    // ---- FIRST RUN: Copy system data to user dir ----
    std::cout << "============================================" << std::endl;
    std::cout << " First-run setup: copying game data..." << std::endl;
    std::cout << "   From: " << sys_dir << std::endl;
    std::cout << "   To:   " << user_dir << std::endl;
    std::cout << "============================================" << std::endl;

    try {
        fs::create_directories(user_dir);

        // Copy directories: engine/, assets/, res/, src/assets/ (launcher textures)
        const char* dirs_to_copy[] = { "engine", "assets", "res", "src/assets", nullptr };
        for (int i = 0; dirs_to_copy[i]; i++) {
            std::string src = sys_dir + dirs_to_copy[i];
            std::string dst = user_dir + dirs_to_copy[i];
            if (fs::exists(src)) {
                fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                int count = 0;
                for (auto& e : fs::recursive_directory_iterator(dst)) {
                    if (e.is_regular_file()) count++;
                }
                std::cout << "   ✓ " << dirs_to_copy[i] << "/ (" << count << " files)" << std::endl;
            }
        }

        // Create save/ and cache/ directories
        fs::create_directories(user_dir + "save");
        fs::create_directories(user_dir + "cache");

        std::cout << "   ✓ Setup complete!" << std::endl;
        std::cout << "============================================" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[DataPath] ERROR copying game data: " << e.what() << std::endl;
        return false;
    }
}

// ============================================================
//  get_data_path: resolve relative path to actual file
// ============================================================

std::string get_data_path(const std::string& relative_path) {
    // If the path is already absolute, return it as-is
    if (!relative_path.empty() && (relative_path[0] == '/' || (relative_path.length() > 1 && relative_path[1] == ':'))) {
        return relative_path;
    }

    // 1. Environment variable override
    const char* env_dir = getenv("SWORDIGO_DATA_DIR");
    if (env_dir) {
        std::string base = env_dir;
        if (!base.empty() && base.back() != '/') base += "/";
        return base + relative_path;
    }

    // 2. Compile-time define
#ifdef SWORDIGO_DATA_DIR_PATH
    {
        std::string path = std::string(SWORDIGO_DATA_DIR_PATH) + "/" + relative_path;
        if (fs::exists(path)) return path;
    }
#endif

    // 3. User data directory (~/.local/share/swordigo-desktop/)
    {
        std::string user_dir = get_user_data_dir();
        if (!user_dir.empty()) {
            std::string path = user_dir + relative_path;
            if (fs::exists(path)) return path;
        }
    }

    // 4. Current directory (development mode)
    {
        std::string path = "./" + relative_path;
        if (fs::exists(path)) return path;
    }

    // 5. Relative to binary location (also dev root with src/assets on Windows)
    {
        std::string dev_root = os_external::dev_root_dir();
        if (!dev_root.empty()) {
            std::string root = dev_root;
            if (root.back() != '/' && root.back() != '\\') root += "/";
            std::string path = root + relative_path;
            if (fs::exists(path)) return path;
        }
        std::string exe_dir = os_external::exe_dir();
        if (!exe_dir.empty()) {
            fs::path exe_dir_p(exe_dir);
            std::string path = (exe_dir_p / relative_path).string();
            if (fs::exists(path)) return path;
            path = (exe_dir_p.parent_path() / relative_path).string();
            if (fs::exists(path)) return path;
        }
    }

#ifndef _WIN32
    // 6. System install paths (read-only fallback)
    {
        std::string sys_dir = get_system_data_dir();
        if (!sys_dir.empty()) {
            std::string path = sys_dir + relative_path;
            if (fs::exists(path)) return path;
        }
    }
#endif

    // Fallback
    return "./" + relative_path;
}

// ============================================================
//  Unified VFS Path Resolver for Host-Side asset loading
// ============================================================

static std::string g_active_mod_name = "";
static std::string g_active_profile_id = "";

void set_active_mod_name(const std::string& name) {
    g_active_mod_name = name;
    std::cout << "[VFS/Host] Set active mod: \"" << name << "\"" << std::endl;
}

void set_active_profile_id(const std::string& id) {
    g_active_profile_id = id;
    std::cout << "[VFS/Host] Set active profile: \"" << id << "\"" << std::endl;
}

// Helper to check if a file exists, with recursive scene remapping, texture format swaps, and Retina fallback
static bool check_file_exists(const std::string& path, std::string& out_resolved) {
    if (fs::exists(path) && !fs::is_directory(path)) {
        out_resolved = path;
        return true;
    }

    // 1. Bare scene renaming: levels/menu.scene -> levels/menu.scenebin, etc.
    if (path.length() > 6 && path.substr(path.length() - 6) == ".scene") {
        std::string stem = path.substr(0, path.length() - 6);
        const char* ext_alts[] = { ".scenebin", ".scene.gz", ".scene.bin", ".scenez" };
        for (const char* ext : ext_alts) {
            std::string alt = stem + ext;
            if (fs::exists(alt) && !fs::is_directory(alt)) {
                out_resolved = alt;
                return true;
            }
        }
    }

    // 2. .tex.png <-> .pvr texture format swaps (to allow modded PVRs to override PNGs and vice-versa)
    if (path.length() > 8 && path.substr(path.length() - 8) == ".tex.png") {
        std::string alt = path.substr(0, path.length() - 8) + ".pvr";
        if (fs::exists(alt) && !fs::is_directory(alt)) {
            out_resolved = alt;
            return true;
        }
    } else if (path.length() > 4 && path.substr(path.length() - 4) == ".pvr") {
        std::string alt = path.substr(0, path.length() - 4) + ".tex.png";
        if (fs::exists(alt) && !fs::is_directory(alt)) {
            out_resolved = alt;
            return true;
        }
    }

    // 3. Retina _2x -> _1x fallback (recursively checks renames/swaps on the base name)
    size_t hi2x = path.find("_2x.");
    if (hi2x != std::string::npos) {
        std::string alt = path.substr(0, hi2x) + path.substr(hi2x + 3);
        if (check_file_exists(alt, out_resolved)) {
            return true;
        }
    }

    // 4. Retina _1x -> _2x fallback (non-recursive check for upgrade if _2x is not in path)
    if (hi2x == std::string::npos) {
        size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            std::string alt = path.substr(0, dot) + "_2x" + path.substr(dot);
            if (fs::exists(alt) && !fs::is_directory(alt)) {
                out_resolved = alt;
                return true;
            }
            // Also check texture format swaps with _2x
            if (path.length() - dot >= 8 && path.substr(dot) == ".tex.png") {
                std::string alt_pvr = path.substr(0, dot) + "_2x.pvr";
                if (fs::exists(alt_pvr) && !fs::is_directory(alt_pvr)) {
                    out_resolved = alt_pvr;
                    return true;
                }
            } else if (path.length() - dot >= 4 && path.substr(dot) == ".pvr") {
                std::string alt_png = path.substr(0, dot) + "_2x.tex.png";
                if (fs::exists(alt_png) && !fs::is_directory(alt_png)) {
                    out_resolved = alt_png;
                    return true;
                }
            }
        }
    }

    return false;
}

extern "C" bool resolve_vfs_path(const char* original_path, char* out_resolved_path, int max_len) {
    if (!original_path || original_path[0] == '\0') {
        return false;
    }

    std::string path(original_path);

#ifdef _WIN32
    // The guest receives an absolute host filesystem path from JNI
    // (getFilesDir/getDataDir, see main.cpp setFilesDir) and hands those
    // directly to fopen. Windows absolute paths (C:\... or \\server\share)
    // must pass through untouched; the match logic below only understands
    // '/'-rooted POSIX paths and would otherwise re-prefix the data base,
    // producing "<base>/C:\..." and failing every asset open.
    if (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                            (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        strncpy(out_resolved_path, original_path, max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }
    if (path.rfind("\\\\", 0) == 0) {  // UNC path
        strncpy(out_resolved_path, original_path, max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }
#endif

    // If the path is absolute, check if it contains virtual resource folders
    bool is_absolute = (path[0] == '/');
    if (is_absolute) {
        size_t res_pos = path.find("/resources/");
        if (res_pos != std::string::npos) {
            path = path.substr(res_pos + 11);
            is_absolute = false; // Treat as relative from now on
        } else {
            size_t assets_pos = path.find("/assets/");
            if (assets_pos != std::string::npos) {
                path = "/Assets/" + path.substr(assets_pos + 8);
            }
        }
    }

    // Strip virtual folder prefixes (if any)
    std::string prefix1 = "assets/resources/";
    std::string prefix2 = g_instance_assets_dir + "/resources/";
    if (path.rfind(prefix1, 0) == 0) {
        path = path.substr(prefix1.length());
    } else if (path.rfind(prefix2, 0) == 0) {
        path = path.substr(prefix2.length());
    } else if (path.rfind("resources/", 0) == 0) {
        path = path.substr(10);
    }

    // MiniPath translations: virtual paths defined by touchfoo/SWKiwi
    if (path.rfind("/Assets/", 0) == 0) {
        std::string res = get_user_data_dir() + g_instance_assets_dir + "/" + path.substr(8);
        if (g_instance_assets_dir != "assets" && !fs::exists(res)) {
            res = get_user_data_dir() + "assets/" + path.substr(8);
        }
        strncpy(out_resolved_path, res.c_str(), max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }
    if (path.rfind("/Files/", 0) == 0) {
        std::string res = get_user_data_dir() + "save/" + path.substr(7);
        strncpy(out_resolved_path, res.c_str(), max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }
    if (path.rfind("/ExternalFiles/", 0) == 0) {
        std::string res = get_user_data_dir() + "external/" + path.substr(15);
        strncpy(out_resolved_path, res.c_str(), max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }
    if (path.rfind("/Cache/", 0) == 0 || path.rfind("/ExternalCache/", 0) == 0) {
        size_t offset = (path.rfind("/Cache/", 0) == 0) ? 7 : 15;
        std::string res = get_user_data_dir() + "cache/" + path.substr(offset);
        strncpy(out_resolved_path, res.c_str(), max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }

    // Absolute paths are passed through as-is
    if (is_absolute && path[0] == '/') {
        strncpy(out_resolved_path, original_path, max_len - 1);
        out_resolved_path[max_len - 1] = '\0';
        return true;
    }

    // Clean data base path
    std::string data_dir = get_user_data_dir();
    if (!data_dir.empty() && data_dir.back() == '/') {
        data_dir.pop_back();
    }

    // Build SWKiwi-compatible 5-level search hierarchy candidates:
    std::vector<std::string> candidates;
    bool has_mod = !g_active_mod_name.empty();
    bool has_profile = !g_active_profile_id.empty();

    // 1. mods/<mod>/resources/<profile>/X
    if (has_mod && has_profile) {
        candidates.push_back(data_dir + "/mods/" + g_active_mod_name + "/resources/" + g_active_profile_id + "/" + path);
    }
    // 2. mods/<mod>/resources/X
    if (has_mod) {
        candidates.push_back(data_dir + "/mods/" + g_active_mod_name + "/resources/" + path);
    }
    // 3. resources/<profile>/X
    if (has_profile) {
        candidates.push_back(data_dir + "/resources/" + g_active_profile_id + "/" + path);
    }
    // 4. resources/X
    candidates.push_back(data_dir + "/resources/" + path);
    // 5. custom_assets/resources/X (fallback to configured assets dir)
    candidates.push_back(data_dir + "/" + g_instance_assets_dir + "/resources/" + path);
    // 6. Vanilla base-game assets/resources/X — final fallback so a modded
    //    instance (g_instance_assets_dir != "assets", e.g. "rln_assets") that
    //    does NOT ship a given asset transparently falls back to the base game
    //    assets, mirroring Android AssetManager's mod-overlay -> base-APK
    //    behaviour. Prevents "[AssetMgr/fopen] Failed to open ... -> rln_assets/
    //    resources/..." misses for files that exist only under assets/resources/.
    if (g_instance_assets_dir != "assets") {
        candidates.push_back(data_dir + "/assets/resources/" + path);
    }


    // Search through candidates in priority order
    for (const auto& candidate : candidates) {
        std::string resolved;
        if (check_file_exists(candidate, resolved)) {
            strncpy(out_resolved_path, resolved.c_str(), max_len - 1);
            out_resolved_path[max_len - 1] = '\0';
            return true;
        }
    }

    // Ultimate fallback: configured assets resources path (level 5)
    std::string fallback = data_dir + "/" + g_instance_assets_dir + "/resources/" + path;
    strncpy(out_resolved_path, fallback.c_str(), max_len - 1);
    out_resolved_path[max_len - 1] = '\0';
    return true;
}

