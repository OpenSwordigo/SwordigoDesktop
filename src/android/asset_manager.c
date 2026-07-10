/*
 * asset_manager.c — PC-Native Android Asset Manager Bridge
 *
 * ROOT CAUSE OF CRASH (BinaryFile.c Ghidra decompilation, lines 68-83):
 *   asset = AAssetManager_open(mgr, path, 2);
 *   if (asset == NULL) __fd = 0;      <- engine BUG: NULL maps to fd=0 (stdin!)
 *   else __fd = AAsset_openFileDescriptor(asset, ...);
 *   gzdopen(__fd, "rb");              <- gzdopen(0) reads stdin -> crash
 *
 *   Fix: NEVER return NULL. Use /dev/null as safe dummy -> gzread gets EOF.
 *
 * LINUX CONFLICT: APK has both "X.scene" (file) and "X.scene/" (dir).
 *   Linux can't. We search multiple path patterns to find both.
 *
 * SEARCH ORDER:
 *   1. Mod override: <data_dir>/mods/<mod>/assets/<filename>
 *   2. Direct:       <base_path>/<filename>
 *   3. Scene binary: try X.scenebin, X.scene.gz, X.scene.bin
 *   4. Scene res:    try X.scene_res/, X.scene.dir/, scene_resources/X.scene/
 *   5. Resources/:   <base_path>/resources/<filename>
 *   6. /dev/null dummy (crash prevention — never return NULL)
 */
#include "asset_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifdef _WIN32
#  include <io.h>
#  define dup   _dup
#  define fileno _fileno
#else
#  include <unistd.h>
#endif

static struct AAssetManager g_mgr;
char g_last_opened_asset[256] = {0};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static char* pjoin(char* dst, size_t dsz, const char* a, const char* b) {
    size_t al = strlen(a);
    while (al > 0 && a[al-1] == '/') al--;
    while (*b == '/') b++;
    snprintf(dst, dsz, "%.*s/%s", (int)al, a, b);
    return dst;
}

static FILE* try_open(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) return f;

    const char* ext = strrchr(path, '.');
    if (!ext) return NULL;

    char base[512];
    strncpy(base, path, ext - path);
    base[ext - path] = '\0';

    int is_tex_png = (strcmp(ext, ".png") == 0 && (ext - path > 4) && strcmp(ext - 4, ".tex.png") == 0);
    if (is_tex_png) {
        base[ext - path - 4] = '\0';
    }

    int blen = strlen(base);
    int has_2x = (blen > 3 && strcmp(base + blen - 3, "_2x") == 0);

    char base_no_2x[512];
    strcpy(base_no_2x, base);
    if (has_2x) {
        base_no_2x[blen - 3] = '\0';
    }

    char alt[512];
    if (has_2x) {
        /* _2x requested. Try opposite extension, then 1x */
        if (is_tex_png) {
            snprintf(alt, sizeof(alt), "%s_2x.pvr", base_no_2x);
            f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
        } else {
            snprintf(alt, sizeof(alt), "%s_2x.tex.png", base_no_2x);
            f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
        }
        snprintf(alt, sizeof(alt), "%s.tex.png", base_no_2x);
        f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
        snprintf(alt, sizeof(alt), "%s.pvr", base_no_2x);
        f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
    } else {
        /* 1x requested. Try opposite extension, then 2x */
        if (is_tex_png) {
            snprintf(alt, sizeof(alt), "%s.pvr", base_no_2x);
            f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
        } else {
            snprintf(alt, sizeof(alt), "%s.tex.png", base_no_2x);
            f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
        }
        snprintf(alt, sizeof(alt), "%s_2x.tex.png", base_no_2x);
        f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
        snprintf(alt, sizeof(alt), "%s_2x.pvr", base_no_2x);
        f = fopen(alt, "rb"); if (f) { printf("[AssetMgr] Fallback: %s\n", alt); return f; }
    }

    return NULL;
}

static const char* leaf(const char* path) {
    const char* p = path + strlen(path);
    while (p > path && p[-1] != '/') p--;
    return p;
}

static FILE* try_mod_overlay(const char* filename) {
    extern char* get_user_data_dir_c(void);
    char* dd = get_user_data_dir_c();
    if (!dd) return NULL;
    char mods[512]; snprintf(mods, sizeof(mods), "%s/mods", dd);
    DIR* d = opendir(mods); if (!d) return NULL;
    struct dirent* de; FILE* f = NULL;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char c[512]; snprintf(c, sizeof(c), "%s/%s/assets/%s", mods, de->d_name, filename);
        f = try_open(c);
        if (f) { printf("[AssetMgr] MOD: %s -> %s\n", filename, c); break; }
    }
    closedir(d);
    return f;
}

static FILE* smart_search(const char* base, const char* filename) {
    char c[512];

    /* 1. Direct: base_path/filename — same as old AAssetManager_open */
    pjoin(c, sizeof(c), base, filename);
    { FILE* f = try_open(c); if (f) { printf("[AssetMgr] FOUND(direct): %s\n", c); return f; } }

    size_t fl = strlen(filename);
    const char* sdot = strstr(filename, ".scene/");
    int bare_scene = (fl > 6) && !strcmp(filename + fl - 6, ".scene")
                     && !strchr(filename, '/');

    /* 2. Bare scene binary: renamed on disk to avoid file/dir naming conflict */
    if (bare_scene) {
        char stem[256]; snprintf(stem, sizeof(stem), "%.*s", (int)(fl-6), filename);
        const char* vs[] = { ".scenebin", ".scene.gz", ".scene.bin", ".scenez", NULL };
        for (int i = 0; vs[i]; i++) {
            char alt[64]; snprintf(alt, sizeof(alt), "%s%s", stem, vs[i]);
            pjoin(c, sizeof(c), base, alt);
            FILE* f = try_open(c);
            if (f) { printf("[AssetMgr] FOUND(scenebin): %s\n", c); return f; }
        }
    }


    /* 4. Scene resource directory alternatives for "X.scene/Resources/Y" paths.
     *    Linux can't have both file "X.scene" and dir "X.scene/" at same path.
     *    Try renamed directory variants. */
    if (sdot) {
        ptrdiff_t slen = (sdot - filename) + 6; /* "X.scene" length */
        char sname[256]; snprintf(sname, sizeof(sname), "%.*s", (int)slen, filename);
        const char* rest = sdot + 7; /* skip ".scene/" */

        /* 4a. Alternative dir names: X.scene_res, X.scene.res, X.scene.dir */
        const char* dvars[] = { "%s_res", "%s.res", "%s.dir", "%s_resources", NULL };
        for (int i = 0; dvars[i]; i++) {
            char adir[256]; snprintf(adir, sizeof(adir), dvars[i], sname);
            char full[512]; pjoin(full, sizeof(full), base, adir);
            pjoin(c, sizeof(c), full, rest);
            FILE* f = try_open(c);
            if (f) { printf("[AssetMgr] FOUND(scene_res_alt): %s\n", c); return f; }

        }

        /* 4b. scene_resources/X.scene/rest */
        { char sr[512]; pjoin(sr, sizeof(sr), base, "scene_resources");
          pjoin(sr, sizeof(sr), sr, sname); pjoin(c, sizeof(c), sr, rest);
          FILE* f = try_open(c); if (f) { printf("[AssetMgr] FOUND(scene_resources/): %s\n", c); return f; } }

        /* 4c. Just the leaf filename at the base level */
        { const char* lf = leaf(filename);
          pjoin(c, sizeof(c), base, lf);
          FILE* f = try_open(c); if (f) { printf("[AssetMgr] FOUND(leaf@base): %s -> %s\n", filename, c); return f; }
        }
    }

    /* 5. _2x HiDPI → _1x fallback.
     *    The game detects our 4K display and requests _2x (Retina) textures first.
     *    If only _1x versions exist on disk, strip "_2x" from the name and retry. */
    const char* hi2x = strstr(filename, "_2x.");
    if (hi2x) {
        /* Build alt name without "_2x": e.g. foo_2x.tex.png → foo.tex.png */
        char alt[512];
        size_t plen = hi2x - filename;
        snprintf(alt, sizeof(alt), "%.*s%s", (int)plen, filename, hi2x + 3);

        /* 5a. Direct path with _1x name */
        pjoin(c, sizeof(c), base, alt);
        { FILE* f = try_open(c); if (f) { printf("[AssetMgr] FOUND(_2x→1x direct): %s\n", c); return f; } }

        /* 5b. Leaf of _1x name at base (flat asset layout) */
        { const char* lf1x = leaf(alt);
          pjoin(c, sizeof(c), base, lf1x);
          FILE* f = try_open(c); if (f) { printf("[AssetMgr] FOUND(_2x→1x leaf): %s -> %s\n", filename, c); return f; } }

        printf("[AssetMgr] MISS(+1x tried): %s (also tried %s and %s)\n",
               filename, alt, leaf(alt));
    } else {
        printf("[AssetMgr] MISS: %s (base=%s)\n", filename, base);
    }
    return NULL;
}

static AAsset* make_null_asset(const char* filename) {
    FILE* f = NULL;
    if (strstr(filename, ".tex.png") != NULL) {
        /* Use a valid small texture as dummy to prevent gzip decode failure */
        char dummy_path[512];
        snprintf(dummy_path, sizeof(dummy_path), "%s/blackbg_2x.tex.png", g_mgr.base_path);
        f = fopen(dummy_path, "rb");
    }
    if (!f) {
        f = fopen("/dev/null", "rb");
    }
    if (!f) return NULL;
    AAsset* a = (AAsset*)malloc(sizeof(AAsset));
    if (!a) { fclose(f); return NULL; }
    a->fp = f;
    snprintf(a->name, sizeof(a->name), "[NULL]%.200s", filename);
    printf("[AssetMgr] DUMMY: %s (using %s)\n", filename, strstr(filename, ".tex.png") ? "blackbg_2x.tex.png" : "/dev/null");
    return a;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void asset_manager_init(const char* base_path) {
    /* All game assets live in <data_dir>/<assets_dir>/resources/
     * e.g. /home/user/.local/share/swordigo-desktop/assets/resources/
     * Bake /resources into the base so every open resolves correctly. */
    snprintf(g_mgr.base_path, sizeof(g_mgr.base_path), "%s/resources", base_path);
    g_mgr.base_path[sizeof(g_mgr.base_path) - 1] = '\0';
    printf("[AssetMgr] init: base=%s\n", g_mgr.base_path);
}

/* Expose the assets parent directory (one level above "resources/").
 * e.g. "/home/user/.local/.../assets" so that
 * "resources/foo.font" -> "/home/user/.../assets/resources/foo.font" */
const char* get_assets_base_path(void) {
    /* g_mgr.base_path is already "<assets>/resources", we need "<assets>".
     * Walk back past the trailing "/resources" component. */
    static char parent[256];
    if (parent[0]) return parent;  /* cached */
    strncpy(parent, g_mgr.base_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    /* Strip trailing "/resources" */
    size_t plen = strlen(parent);
    const char* suffix = "/resources";
    size_t slen = strlen(suffix);
    if (plen >= slen && strcmp(parent + plen - slen, suffix) == 0)
        parent[plen - slen] = '\0';
    return parent;
}

AAssetManager* AAssetManager_fromJava(void* env, void* assetManager) {
    return &g_mgr;
}

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
    if (strncmp(filename, "assets/resources/", 17) == 0) {
        filename += 17;
    } else if (strncmp(filename, "resources/", 10) == 0) {
        filename += 10;
    }

    strncpy(g_last_opened_asset, filename, sizeof(g_last_opened_asset) - 1);
    g_last_opened_asset[sizeof(g_last_opened_asset) - 1] = '\0';

    /* Scene transition hook — reset void fill color when any scene asset opens */
    if (strstr(filename, ".scene") != NULL) {
        extern void reset_void_fill_color(void);
        reset_void_fill_color();
    }

    char resolved[512];
    FILE* fp = NULL;
    extern int resolve_vfs_path(const char* original_path, char* out_resolved_path, int max_len);
    if (resolve_vfs_path(filename, resolved, sizeof(resolved))) {
        fp = fopen(resolved, "rb");
    }

    /* NEVER return NULL — use /dev/null dummy to prevent gzdopen(0) crash.
     * (BinaryFile::Open does: if(asset==NULL) fd=0; gzdopen(fd,...) — crash!) */
    if (!fp) return make_null_asset(filename);

    AAsset* a = (AAsset*)malloc(sizeof(AAsset));
    if (!a) { fclose(fp); return make_null_asset(filename); }
    a->fp = fp;
    strncpy(a->name, filename, sizeof(a->name) - 1);
    a->name[sizeof(a->name) - 1] = '\0';
    return a;
}


int AAsset_read(AAsset* asset, void* buf, size_t count) {
    if (!asset || !asset->fp) return -1;
    return (int)fread(buf, 1, count, asset->fp);
}

void AAsset_close(AAsset* asset) {
    if (!asset) return;
    if (asset->fp) fclose(asset->fp);
    free(asset);
}

off_t AAsset_getLength(AAsset* asset) {
    if (!asset || !asset->fp) return 0;
    long cur = ftell(asset->fp);
    fseek(asset->fp, 0, SEEK_END);
    off_t len = (off_t)ftell(asset->fp);
    fseek(asset->fp, cur, SEEK_SET);
    return len;
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (!asset || !asset->fp) {
        if (outStart)  *outStart  = 0;
        if (outLength) *outLength = 0;
        return -1;  /* engine maps -1 -> 0 but at least NULL asset is now /dev/null dummy */
    }
    if (outStart)  *outStart  = 0;
    if (outLength) *outLength = AAsset_getLength(asset);
    return dup(fileno(asset->fp));
}

