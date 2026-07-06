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
 *
 * Communication model:
 *   SRE (guest) writes path requests to shared globals.
 *   Host polls these and performs actual filesystem operations.
 */

#include "sre.h"
#include "sre_lua.h"

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
 */
int sre_FileExistsAtPath(SreString* path_str) {
    const char* path = path_str->data;
    if (!path) return 0;

    /* Try VFS rewrite */
    if (g_sre_vfs_active) {
        char rewritten[512];
        if (sre_vfs_rewrite_path(path, rewritten, 512)) {
            vfs_strcpy(g_sre_vfs_path_request, rewritten);
            g_sre_vfs_check_pending = 1;
            /* Optimistic return — host will handle fallback during actual load */
            return 1;
        }
    }

    /* Passthrough */
    vfs_strcpy(g_sre_vfs_path_request, path);
    g_sre_vfs_check_pending = 1;
    return 1;
}

/*
 * sre_NewByteBufferFromAndroidAsset — replacement for Caver::NewByteBufferFromAndroidAsset
 *
 * ARM64 ABI: X0 = const std::string& path, X1 = uint32_t* out_len
 * Returns: void* (pointer to loaded data buffer, or NULL on failure)
 *
 * The host intercepts this to load files from the VFS hierarchy.
 */
void* sre_NewByteBufferFromAndroidAsset(SreString* path_str, uint32_t* out_len) {
    const char* path = path_str->data;
    if (!path) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Try VFS rewrite — write the path for the host to load */
    if (g_sre_vfs_active) {
        char rewritten[512];
        if (sre_vfs_rewrite_path(path, rewritten, 512)) {
            vfs_strcpy(g_sre_vfs_path_request, rewritten);
            g_sre_vfs_load_pending = 1;
            /* Host processes synchronously. After host sets result, we return. */
            if (g_sre_vfs_load_result_ptr && g_sre_vfs_load_result_size > 0) {
                if (out_len) *out_len = g_sre_vfs_load_result_size;
                return (void*)g_sre_vfs_load_result_ptr;
            }
        }
    }

    /* Passthrough — write original path for host */
    vfs_strcpy(g_sre_vfs_path_request, path);
    g_sre_vfs_load_pending = 1;

    if (g_sre_vfs_load_result_ptr && g_sre_vfs_load_result_size > 0) {
        if (out_len) *out_len = g_sre_vfs_load_result_size;
        return (void*)g_sre_vfs_load_result_ptr;
    }

    if (out_len) *out_len = 0;
    return NULL;
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
