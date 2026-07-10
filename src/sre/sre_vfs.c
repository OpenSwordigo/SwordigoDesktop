/*
 * sre_vfs.c — Virtual Filesystem for mod support (SWKiwi MiniPaths replacement)
 *
 * Implements SWKiwi/SWMini's MiniPaths virtual filesystem on desktop.
 * Provides a 5-level resource search hierarchy and MiniPath translation.
 *
 * Desktop MiniPath Translation:
 *   /Assets/       → <data_dir>/assets/          (vanilla, read-only)
 *   /Files/        → <data_dir>/files/           (read-write)
 *   /ExternalFiles/→ <data_dir>/external/        (read-write)
 *   /Cache/        → <data_dir>/cache/
 *   resources/     → 5-level search hierarchy
 *
 * 5-Level Resource Search Hierarchy (SWKiwi compatible):
 *   1. <data_dir>/mods/<active_mod>/resources/<profile_id>/
 *   2. <data_dir>/mods/<active_mod>/resources/
 *   3. <data_dir>/resources/<profile_id>/
 *   4. <data_dir>/resources/
 *   5. <data_dir>/assets/resources/  (vanilla)
 *
 * Hooked functions:
 *   - Caver::FileExistsAtPath(const std::string&)
 *   - Caver::NewByteBufferFromAndroidAsset(const std::string&, uint32_t*)
 *   - Caver::SetResourcesPath(const std::string&)  [forced to "resources"]
 *
 * Communication model:
 *   SRE (guest) writes path requests to shared globals.
 *   Host polls these and performs actual filesystem operations.
 */

#include "sre.h"
#include "sre_lua.h"
#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * VFS Configuration Globals (set by host via sre_vfs_init)
 * ========================================================================= */

/* Active mod name — e.g. "rl_swordigo" or "" for vanilla.
 * Corresponds to a directory under <data_dir>/mods/<mod_name>/ */
char g_sre_vfs_mod_name[128] = {0};

/* Current profile ID — e.g. "550e8400-e29b-41d4-a716-446655440000".
 * Used for per-profile resource directories. */
char g_sre_vfs_profile_id[64] = {0};

/* Data directory base path — e.g. "/home/user/.local/share/swordigo-desktop".
 * All VFS paths are relative to this. */
char g_sre_vfs_data_dir[512] = {0};

/* Legacy mod prefix for backward compat — e.g. "rl_assets" */
char g_sre_vfs_mod_prefix[256] = {0};

/* Flag: 1 = VFS active (mod loaded or configured), 0 = passthrough */
int g_sre_vfs_active = 0;

/* Flag: 1 = search hierarchy enabled, 0 = simple prefix rewrite only */
int g_sre_vfs_hierarchy_enabled = 0;

/* Original function pointers — set by host after trampoline install */
uint64_t g_orig_FileExistsAtPath = 0;
uint64_t g_orig_NewByteBuffer = 0;
extern uint64_t g_swordigo_base;

/* =========================================================================
 * VFS Command Buffer — written by SRE, read/processed by host
 * ========================================================================= */

/* Path request buffer — SRE writes the path, host performs file operations */
char g_sre_vfs_path_request[512] = {0};

/* File existence check */
volatile int g_sre_vfs_check_pending = 0;    /* 1 = check requested */
volatile int g_sre_vfs_check_result = 0;     /* 1 = file exists */

/* File load request */
volatile int g_sre_vfs_load_pending = 0;     /* 1 = load requested */
volatile uint64_t g_sre_vfs_load_result_ptr = 0;   /* Guest ptr to loaded data */
volatile uint32_t g_sre_vfs_load_result_size = 0;   /* Size of loaded data */

/* Search hierarchy results — host populates these */
char g_sre_vfs_resolved_path[512] = {0};     /* Actual resolved file path */
volatile int g_sre_vfs_resolve_pending = 0;  /* 1 = resolve requested */
volatile int g_sre_vfs_resolve_result = 0;   /* 1 = resolved successfully */

/* =========================================================================
 * MiniPath Translation Table
 *
 * The host populates these base paths during initialization.
 * SRE uses them for path prefix substitution.
 * ========================================================================= */

/* Base paths for MiniPath translation (set by host) */
char g_sre_vfs_path_assets[512] = {0};       /* /Assets/ → vanilla assets dir */
char g_sre_vfs_path_files[512] = {0};        /* /Files/ → user files dir */
char g_sre_vfs_path_external[512] = {0};     /* /ExternalFiles/ → external dir */
char g_sre_vfs_path_cache[512] = {0};        /* /Cache/ → cache dir */

/* =========================================================================
 * String helpers (freestanding — no libc)
 * ========================================================================= */

static int vfs_strlen(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int vfs_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void vfs_strcpy(char* dst, const char* src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void vfs_strcat(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

/* Build a path: dst = a + "/" + b (bounded) */
static int vfs_path_join(char* dst, int max_len, const char* a, const char* b) {
    int alen = vfs_strlen(a);
    int blen = vfs_strlen(b);
    if (alen + 1 + blen >= max_len) return 0;

    vfs_strcpy(dst, a);
    /* Add separator if 'a' doesn't end with '/' */
    if (alen > 0 && a[alen - 1] != '/') {
        vfs_strcat(dst, "/");
    }
    vfs_strcat(dst, b);
    return 1;
}

/* Build a path: dst = a + "/" + b + "/" + c (bounded) */
static int vfs_path_join3(char* dst, int max_len, const char* a, const char* b, const char* c) {
    char tmp[512];
    if (!vfs_path_join(tmp, 512, a, b)) return 0;
    return vfs_path_join(dst, max_len, tmp, c);
}

/* =========================================================================
 * VFS Initialization
 * ========================================================================= */

/*
 * sre_vfs_init — Configure VFS for mod support (legacy single-prefix mode)
 * Called by host when a mod binary (e.g. RLSwordigo) is selected.
 *
 * @param mod_prefix  Mod asset directory name (e.g. "rl_assets"), or NULL for vanilla
 */
void sre_vfs_init(const char* mod_prefix) {
    if (mod_prefix && mod_prefix[0] != '\0') {
        int i;
        for (i = 0; i < 255 && mod_prefix[i]; i++) {
            g_sre_vfs_mod_prefix[i] = mod_prefix[i];
        }
        g_sre_vfs_mod_prefix[i] = '\0';
        g_sre_vfs_active = 1;
    } else {
        g_sre_vfs_mod_prefix[0] = '\0';
        g_sre_vfs_active = 0;
    }
}

/*
 * sre_vfs_init_full — Configure VFS with full SWKiwi MiniPaths support
 * Called by host during initialization to set up the 5-level search hierarchy.
 *
 * @param data_dir     Base data directory
 * @param mod_name     Active mod name (or "" for vanilla)
 * @param profile_id   Current profile UUID (or "" if unknown)
 */
void sre_vfs_init_full(const char* data_dir, const char* mod_name, const char* profile_id) {
    /* Copy data_dir */
    if (data_dir) {
        int i;
        for (i = 0; i < 511 && data_dir[i]; i++)
            g_sre_vfs_data_dir[i] = data_dir[i];
        g_sre_vfs_data_dir[i] = '\0';
    }

    /* Copy mod_name */
    if (mod_name) {
        int i;
        for (i = 0; i < 127 && mod_name[i]; i++)
            g_sre_vfs_mod_name[i] = mod_name[i];
        g_sre_vfs_mod_name[i] = '\0';
    }

    /* Copy profile_id */
    if (profile_id) {
        int i;
        for (i = 0; i < 63 && profile_id[i]; i++)
            g_sre_vfs_profile_id[i] = profile_id[i];
        g_sre_vfs_profile_id[i] = '\0';
    }

    /* Build MiniPath base paths */
    if (data_dir && data_dir[0]) {
        vfs_path_join(g_sre_vfs_path_assets,   512, data_dir, "assets");
        vfs_path_join(g_sre_vfs_path_files,    512, data_dir, "save");
        vfs_path_join(g_sre_vfs_path_external, 512, data_dir, "external");
        vfs_path_join(g_sre_vfs_path_cache,    512, data_dir, "cache");
    }

    /* Enable VFS if we have a data directory */
    g_sre_vfs_active = (data_dir && data_dir[0]) ? 1 : 0;
    g_sre_vfs_hierarchy_enabled = (mod_name && mod_name[0]) ? 1 : 0;
}

/*
 * sre_vfs_set_profile — Update the active profile ID
 * Called when the player switches save files.
 */
void sre_vfs_set_profile(const char* profile_id) {
    if (profile_id) {
        int i;
        for (i = 0; i < 63 && profile_id[i]; i++)
            g_sre_vfs_profile_id[i] = profile_id[i];
        g_sre_vfs_profile_id[i] = '\0';
    } else {
        g_sre_vfs_profile_id[0] = '\0';
    }
}

/* =========================================================================
 * MiniPath Translation
 *
 * Translates SWKiwi virtual paths to real desktop paths.
 * Returns 1 if translation was applied, 0 if not a MiniPath.
 * ========================================================================= */

static int sre_vfs_translate_minipath(const char* vpath, char* real_path, int max_len) {
    /* /Assets/X → <assets_dir>/X */
    if (vfs_strncmp(vpath, "/Assets/", 8) == 0) {
        if (g_sre_vfs_path_assets[0])
            return vfs_path_join(real_path, max_len, g_sre_vfs_path_assets, vpath + 8);
        return 0;
    }

    /* /Files/X → <files_dir>/X */
    if (vfs_strncmp(vpath, "/Files/", 7) == 0) {
        if (g_sre_vfs_path_files[0])
            return vfs_path_join(real_path, max_len, g_sre_vfs_path_files, vpath + 7);
        return 0;
    }

    /* /ExternalFiles/X → <external_dir>/X */
    if (vfs_strncmp(vpath, "/ExternalFiles/", 15) == 0) {
        if (g_sre_vfs_path_external[0])
            return vfs_path_join(real_path, max_len, g_sre_vfs_path_external, vpath + 15);
        return 0;
    }

    /* /Cache/X → <cache_dir>/X */
    if (vfs_strncmp(vpath, "/Cache/", 7) == 0) {
        if (g_sre_vfs_path_cache[0])
            return vfs_path_join(real_path, max_len, g_sre_vfs_path_cache, vpath + 7);
        return 0;
    }

    /* /ExternalCache/X → <cache_dir>/X (same as Cache on desktop) */
    if (vfs_strncmp(vpath, "/ExternalCache/", 15) == 0) {
        if (g_sre_vfs_path_cache[0])
            return vfs_path_join(real_path, max_len, g_sre_vfs_path_cache, vpath + 15);
        return 0;
    }

    return 0; /* Not a MiniPath */
}

/* =========================================================================
 * Resource Search Hierarchy
 *
 * When the engine requests "resources/X", we build a search list of up to
 * 5 candidate paths and write them to the VFS command buffer for the host
 * to check.
 *
 * The host iterates the search list and returns the first path that exists.
 * ========================================================================= */

/* Search list buffer — holds up to 5 candidate paths, pipe-separated.
 * Host parses this to check each candidate in order.
 * Format: "path1|path2|path3|path4|path5" */
char g_sre_vfs_search_list[2560] = {0};

/*
 * Build the 5-level search list for a resource path.
 *
 * SWKiwi search order:
 *   1. <data_dir>/mods/<mod>/resources/<profile>/  (mod + profile specific)
 *   2. <data_dir>/mods/<mod>/resources/            (mod-wide)
 *   3. <data_dir>/resources/<profile>/             (user + profile specific)
 *   4. <data_dir>/resources/                       (user-wide)
 *   5. <data_dir>/assets/resources/                (vanilla)
 *
 * @param resource_subpath  The path after "resources/" (e.g. "levels/town.scene")
 * @return Number of candidate paths written (1-5)
 */
static int sre_vfs_build_search_list(const char* resource_subpath) {
    int count = 0;
    char candidate[512];

    g_sre_vfs_search_list[0] = '\0';

    int has_mod = (g_sre_vfs_mod_name[0] != '\0');
    int has_profile = (g_sre_vfs_profile_id[0] != '\0');

    /* Level 1: mods/<mod>/resources/<profile>/X */
    if (has_mod && has_profile) {
        char mod_res[512];
        vfs_path_join3(mod_res, 512, g_sre_vfs_data_dir, "mods", g_sre_vfs_mod_name);
        char mod_prof[512];
        vfs_path_join3(mod_prof, 512, mod_res, "resources", g_sre_vfs_profile_id);
        if (vfs_path_join(candidate, 512, mod_prof, resource_subpath)) {
            vfs_strcat(g_sre_vfs_search_list, candidate);
            count++;
        }
    }

    /* Level 2: mods/<mod>/resources/X */
    if (has_mod) {
        char mod_res[512];
        vfs_path_join3(mod_res, 512, g_sre_vfs_data_dir, "mods", g_sre_vfs_mod_name);
        char mod_dir[512];
        vfs_path_join(mod_dir, 512, mod_res, "resources");
        if (vfs_path_join(candidate, 512, mod_dir, resource_subpath)) {
            if (count > 0) vfs_strcat(g_sre_vfs_search_list, "|");
            vfs_strcat(g_sre_vfs_search_list, candidate);
            count++;
        }
    }

    /* Level 3: resources/<profile>/X */
    if (has_profile) {
        char user_prof[512];
        vfs_path_join3(user_prof, 512, g_sre_vfs_data_dir, "resources", g_sre_vfs_profile_id);
        if (vfs_path_join(candidate, 512, user_prof, resource_subpath)) {
            if (count > 0) vfs_strcat(g_sre_vfs_search_list, "|");
            vfs_strcat(g_sre_vfs_search_list, candidate);
            count++;
        }
    }

    /* Level 4: resources/X */
    {
        char user_res[512];
        vfs_path_join(user_res, 512, g_sre_vfs_data_dir, "resources");
        if (vfs_path_join(candidate, 512, user_res, resource_subpath)) {
            if (count > 0) vfs_strcat(g_sre_vfs_search_list, "|");
            vfs_strcat(g_sre_vfs_search_list, candidate);
            count++;
        }
    }

    /* Level 5: assets/resources/X (vanilla) */
    {
        char vanilla_res[512];
        vfs_path_join(vanilla_res, 512, g_sre_vfs_path_assets, "resources");
        if (vfs_path_join(candidate, 512, vanilla_res, resource_subpath)) {
            if (count > 0) vfs_strcat(g_sre_vfs_search_list, "|");
            vfs_strcat(g_sre_vfs_search_list, candidate);
            count++;
        }
    }

    return count;
}

/* =========================================================================
 * Path Rewriting — determines the best path for a resource request
 * ========================================================================= */

/*
 * Rewrite a resource path using the search hierarchy.
 *
 * If hierarchy is enabled, builds a search list for the host to resolve.
 * If only simple prefix mode, does a single prefix rewrite.
 *
 * Returns 1 if rewrite was done, 0 if passthrough.
 */
static int sre_vfs_rewrite_path(const char* original, char* rewritten, int max_len) {
    if (!g_sre_vfs_active) return 0;

    /* Check for "resources/" prefix — this is the main game content path */
    if (vfs_strncmp(original, "resources/", 10) == 0) {
        const char* subpath = original + 10;  /* Path after "resources/" */

        if (g_sre_vfs_hierarchy_enabled && g_sre_vfs_data_dir[0]) {
            /* Full 5-level search hierarchy */
            int n = sre_vfs_build_search_list(subpath);
            if (n > 0) {
                /* Write the search list to the resolve buffer.
                 * The host will check each path in order. */
                vfs_strcpy(g_sre_vfs_path_request, g_sre_vfs_search_list);
                g_sre_vfs_resolve_pending = 1;
                /* Copy the first candidate as the rewritten path for now */
                vfs_strcpy(rewritten, g_sre_vfs_search_list);
                /* Truncate at first '|' */
                for (int i = 0; rewritten[i]; i++) {
                    if (rewritten[i] == '|') { rewritten[i] = '\0'; break; }
                }
                return 1;
            }
        } else if (g_sre_vfs_mod_prefix[0]) {
            /* Legacy simple prefix rewrite: "resources/X" → "mod_prefix/resources/X" */
            if (vfs_strlen(g_sre_vfs_mod_prefix) + 1 + vfs_strlen(original) < max_len) {
                vfs_strcpy(rewritten, g_sre_vfs_mod_prefix);
                vfs_strcat(rewritten, "/");
                vfs_strcat(rewritten, original);
                return 1;
            }
        } else if (g_sre_vfs_path_assets[0]) {
            /* Vanilla fallback: "resources/X" → "<assets_dir>/resources/X"
             * This mirrors what search-list level 5 does in hierarchy mode.
             * Necessary so sre_FileExistsAtPath and sre_NewByteBufferFromAndroidAsset
             * can find vanilla textures/levels/sounds using their full desktop path
             * instead of the bare relative path "resources/..." (which has no
             * relationship to the process CWD). */
            if (vfs_path_join(rewritten, max_len, g_sre_vfs_path_assets, original))
                return 1;
        }
    }

    /* Check for MiniPaths (virtual path prefixes) */
    if (original[0] == '/') {
        if (sre_vfs_translate_minipath(original, rewritten, max_len)) {
            return 1;
        }
    }

    return 0;
}

/* =========================================================================
 * Hooked Functions
 *
 * These replace the engine's file I/O functions.
 * The VFS intercepts resource lookups and tries mod directories first.
 *
 * IMPORTANT: These functions are called from the guest ARM64 context.
 * They receive SreString* arguments (Caver's std::string layout).
 *
 * The actual file existence check and byte buffer loading happens on
 * the HOST side — we communicate via shared globals.
 * ========================================================================= */

/*
 * sre_FileExistsAtPath — replacement for Caver::FileExistsAtPath
 *
 * ARM64 ABI: X0 = const std::string& path
 * Returns: int (1 = exists, 0 = not found)
 *
 * Design rationale:
 *   The purpose of hooking this function is mod asset prioritization.
 *   In vanilla mode the original Android implementation returned 1 for
 *   all packaged APK assets (no separate filesystem pre-check). We
 *   replicate that optimistic behavior so that UI button textures with
 *   non-standard paths are not silently blocked, and the engine's own
 *   load failure handling runs if a file is truly absent.
 *
 *   Real fopen checks are ONLY performed in mod hierarchy mode, where
 *   the 5-level search list covers both mod overrides AND vanilla fallback
 *   (level 5 = <assets_dir>/resources/X), so a legitimate vanilla file
 *   will still return 1.
 */
int sre_FileExistsAtPath(SreString* path_str) {
    const char* path = path_str->data;
    if (!path || !path[0]) return 0;

    char rewritten[512];

    /* Mod hierarchy — do a real multi-level search */
    if (g_sre_vfs_active && g_sre_vfs_hierarchy_enabled) {
        if (sre_vfs_rewrite_path(path, rewritten, 512) && g_sre_vfs_search_list[0]) {
            const char* p = g_sre_vfs_search_list;
            char candidate[512];
            while (*p) {
                int ci = 0;
                while (*p && *p != '|' && ci < 511)
                    candidate[ci++] = *p++;
                candidate[ci] = '\0';
                if (*p == '|') p++;
                if (ci > 0) {
                    FILE* f = fopen(candidate, "rb");
                    if (f) { fclose(f); return 1; }
                }
            }
            return 0;  /* not found in any of the 5 levels */
        }
    }

    /* Vanilla mode — optimistic.
     * The gzdopen bridge detects PVR magic and converts it to a TEX-format stream
     * transparently (with correct pixelFmt/width/height header), so the engine's
     * LoadFromTEXFile path works for both real .tex.png AND .pvr files without
     * needing FileExistsAtPath to return 0 for missing extensions.
     *
     * The asset_manager's try_open already handles the .tex.png → .pvr fallback
     * at the file-open level, so the right bytes always reach gzdopen. */
    return 1;
}


/*
 * sre_NewByteBufferFromAndroidAsset — replacement for Caver::NewByteBufferFromAndroidAsset
 *
 * ARM64 ABI: X0 = const std::string& path, X1 = uint32_t* out_len
 * Returns: void* (malloc'd buffer with file contents, or NULL on failure)
 *
 * Uses real fopen/fread to load files from the VFS search hierarchy.
 * The returned buffer is malloc'd and the engine is responsible for freeing it
 * (or it leaks, but only mod data — acceptable for now).
 */
void* sre_NewByteBufferFromAndroidAsset(SreString* path_str, uint32_t* out_len) {
    const char* path = path_str->data;
    if (!path || !path[0]) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    const char* load_path = path;
    char rewritten[512];

    if (g_sre_vfs_active && sre_vfs_rewrite_path(path, rewritten, 512)) {
        /* Hierarchy: try each candidate in pipe-separated search list */
        if (g_sre_vfs_hierarchy_enabled && g_sre_vfs_search_list[0]) {
            const char* p = g_sre_vfs_search_list;
            char candidate[512];
            while (*p) {
                int ci = 0;
                while (*p && *p != '|' && ci < 511)
                    candidate[ci++] = *p++;
                candidate[ci] = '\0';
                if (*p == '|') p++;
                if (ci == 0) continue;
                FILE* f = fopen(candidate, "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz <= 0) { fclose(f); continue; }
                void* buf = malloc((size_t)sz);
                if (!buf) { fclose(f); continue; }
                fread(buf, 1, (size_t)sz, f);
                fclose(f);
                if (out_len) *out_len = (uint32_t)sz;
                return buf;
            }
            /* Not found in any level — fall through to original path */
        } else {
            load_path = rewritten;
        }
    }

    /* Load from load_path (original or simple-rewrite) */
    FILE* f = fopen(load_path, "rb");
    if (!f) { if (out_len) *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); if (out_len) *out_len = 0; return NULL; }
    void* buf = malloc((size_t)sz);
    if (!buf) { fclose(f); if (out_len) *out_len = 0; return NULL; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (out_len) *out_len = (uint32_t)sz;
    return buf;
}

/* Hook SetResourcesPath to force it to "resources" */
void sre_SetResourcesPath(SreString* path_str) {
    if (g_swordigo_base != 0) {
        SreString* global_res_path = (SreString*)(g_swordigo_base + 0x7e9d10);
        sre_CppString_assign(global_res_path, "resources", 9);
        printf("[SRE VFS] Hooked SetResourcesPath (original was '%s'): forced to 'resources'\n", path_str->data);
    }
}

static const char* vfs_strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    int nlen = vfs_strlen(needle);
    if (nlen == 0) return haystack;
    for (int i = 0; haystack[i]; i++) {
        if (vfs_strncmp(haystack + i, needle, nlen) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

/* Hook IsAndroidAssetsPath to route all game asset files to AAssetManager.
 * CRITICAL: Any extension missing here causes NewByteBufferFromFile to fall
 * through to fopen(relative_path) which fails on desktop. Add ALL asset types. */
int sre_IsAndroidAssetsPath(SreString* path_str) {
    const char* path = path_str->data;
    if (!path) return 0;

    /* "resources/" prefix always means AAssetManager — covers every resource type */
    if (vfs_strncmp(path, "resources/", 10) == 0 ||
        vfs_strncmp(path, "assets/", 7) == 0 ||
        vfs_strncmp(path, "/Assets/", 8) == 0) {
        return 1;
    }

    /* Non-prefixed asset extensions — include everything the engine loads */
    if (vfs_strstr(path, ".scene")   || vfs_strstr(path, ".POD")     ||
        vfs_strstr(path, ".wav")     || vfs_strstr(path, ".scl")     ||
        vfs_strstr(path, ".pvr")     || vfs_strstr(path, ".tex.png") ||
        vfs_strstr(path, ".ogg")     || vfs_strstr(path, ".png")     ||
        vfs_strstr(path, ".atlas")   || vfs_strstr(path, ".fnt")     ||
        /* Protobuf binary asset types — critical for fonts, textures, materials */
        vfs_strstr(path, ".font")    || vfs_strstr(path, ".material") ||
        vfs_strstr(path, ".texture") || vfs_strstr(path, ".object")  ||
        vfs_strstr(path, ".plist")   || vfs_strstr(path, ".lua")     ||
        vfs_strstr(path, ".mp3")     || vfs_strstr(path, ".caf")) {
        return 1;
    }

    return 0;
}

/* =========================================================================
 * Lua VFS API — allows Lua scripts to use MiniPaths
 *
 * These are registered under the global Lua table for loadfile/dofile
 * path translation. The host intercepts fopen calls to apply VFS.
 * ========================================================================= */

/* Current VFS search path — exported so Lua loadfile/dofile hooks can
 * translate paths before the engine's file operations. */
volatile int g_sre_vfs_lua_translate_pending = 0;
char g_sre_vfs_lua_input_path[512] = {0};
char g_sre_vfs_lua_output_path[512] = {0};
