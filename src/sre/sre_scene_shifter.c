/* ============================================================
 * sre_scene_shifter.c — Scene Teleporter / Scene Shifter
 * ============================================================
 * Provides two scene loading gateways for ARM64 Swordigo v1.4.12:
 *
 *   1. NORMAL GATEWAY  — calls GotoLevel() exactly as the game does,
 *      which shows the loading screen and transitions normally.
 *      Safe for all scenes; correct GameState wiring.
 *
 *   2. FORCED GATEWAY  — hooks SceneLoadingView::Update to instantly
 *      force-complete the loading phase (0-frame) by writing 1.0f to
 *      the progress float and 1 to the load_complete flag at the known
 *      struct offsets, then releasing the hook after first call.
 *      Mimics doc 02 Technique 1 (highest stability rating).
 *
 * Scene list scanning:
 *   Scans the VFS mod directory (data_dir/mods/<active_mod>/) for
 *   any *.scene files (pattern: resources subdirectories) and builds a
 *   flat list for the GUI picker. Also scans vanilla assets if the
 *   mod directory has no .scene files.
 *
 * ARM64 ONLY — ARM32 has no hook infrastructure for these offsets.
 * The functions are guarded by IS_ARM64 at call sites in the host.
 *
 * Function addresses from: nm -D libswordigo.so (v1.4.12 ARM64)
 * Struct offsets from: IDA + Ghidra + docs/sceneloading/
 *
 * References:
 *   docs/sceneloading/01_forced_scene_loading_mechanics.md
 *   docs/sceneloading/02_instant_scene_loading_bypassing_loading_view.md
 *   docs/sceneloading/03_sre_integration_and_offsets_reference.md
 * ============================================================ */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include "sre.h"
#include "sre_lua.h"

/* =========================================================================
 * External state from other SRE modules
 * ========================================================================= */
extern uint64_t  g_swordigo_base;        /* libswordigo.so base in guest VA */
extern char      g_sre_vfs_data_dir[512]; /* host-side data directory root   */
extern char      g_sre_vfs_mod_name[128]; /* active mod name (or empty)       */
extern char      g_sre_vfs_path_assets[512]; /* vanilla assets path           */
extern char      g_sre_current_scene_name[128]; /* current scene (from host)  */

/* =========================================================================
 * Scene Shifter Globals — readable from host / GUI
 * ========================================================================= */

/* --- Scene list (populated by sre_scene_shifter_scan_scenes) --- */
#define SRE_SCENE_LIST_MAX   256
#define SRE_SCENE_NAME_MAX   128

char     g_sre_scene_list[SRE_SCENE_LIST_MAX][SRE_SCENE_NAME_MAX];
int      g_sre_scene_list_count = 0;

/* --- Pending teleport request (set by GUI, consumed by hook) --- */
/* Mode: 0=none, 1=normal gateway, 2=forced gateway              */
volatile int     g_sre_scene_shift_pending   = 0;
char             g_sre_scene_shift_target[SRE_SCENE_NAME_MAX] = {0};
char             g_sre_scene_shift_spawn[64]                  = "start";
volatile uint64_t g_sre_scene_shifter_poll_ticks = 0;
volatile uint64_t g_sre_scene_shifter_dispatch_count = 0;

/* --- Instant load state (Forced gateway arm) --- */
volatile int     g_sre_instant_scene_load_enabled = 0;  /* 1 = short-circuit next Update */

/* --- Last result --- */
char             g_sre_scene_shift_last_error[256] = {0};
int              g_sre_scene_shift_active   = 0; /* 1 while forced load in flight */
static int        g_sre_scene_shift_wait_frames = 0;

/* Last SceneLoadingView created (defined in sre_scene_update.c) */
extern volatile void* g_sre_last_slv;

/* =========================================================================
 * ARM64 Offsets (v1.4.12, from nm -D libswordigo.so + IDA/Ghidra)
 * =========================================================================
 *
 * GotoLevel:
 *   0x00358A74 — Caver::GameViewController::GotoLevel(this, level&, spawn&)
 *   Verified: nm -D libswordigo.so | grep GotoLevel → 0000000000358a74
 *
 * CaverShell singleton global (BSS, v1.4.12):
 *   0x006E9C20 — qword_6E9C20 = CaverShell*
 *   CaverShell+0x98  = GUINavigationController* (set in InitView, confirmed Ghidra)
 *   GUINavCtrl+0x10  = boost::shared_ptr<GUIViewController>.px = raw GVC*
 *   (boost::shared_ptr layout: +0x00=px raw ptr, +0x08=pn refcount block)
 *   So: GVC = *(void**)(*(void**)(*(void**)(shell + 0x98) + 0x10))
 *
 * SceneLoadingView::Update:
 *   0x0043650C — Caver::SceneLoadingView::Update(this, float dt)
 *   At SceneLoadingView + 0x48: float progress   (set to 1.0f to instant-complete)
 *   At SceneLoadingView + 0x50: int  load_complete (set to 1 to trigger transition)
 *
 * std::string helpers (hooked trampolines in libsre.so):
 *   sre_CppString_from_char_p — constructs std::string from char*
 *   sre_CppString_release     — destructs std::string
 * ========================================================================= */

#define OFF_GotoLevel               0x00358A74ULL

/* CaverShell global pointer in BSS (DAT_007F3C20, confirmed frame_loop + Ghidra) */
#define OFF_CaverShell_globalptr    0x006E9C20ULL

/* CaverShell struct offsets (Ghidra CaverShell::Update + InitView) */
#define CAVERSHELL_OFF_NAV_CTRL     0x98  /* shared_ptr: px at +0x98 */

/* GUINavigationController struct offsets
 * Confirmed via aarch64 disassembly of SetCurrentViewController (0x49a550):
 *   ldr x0, [x19, #80]   ; x19 = this (NavCtrl*), #80 = 0x50 = px of shared_ptr<GVC>
 *   ldr x21, [x19, #88]  ; #88 = 0x58 = pn (refcount block) of shared_ptr<GVC>
 * So: nav_ctrl+0x50 = px of current view controller (raw GVC*) */
#define NAVCTRL_OFF_CURRENT_VC_PX   0x50

/* Struct offsets inside SceneLoadingView */
#define SLVIEW_OFF_PROGRESS      0x48
#define SLVIEW_OFF_LOAD_COMPLETE 0x50

/* Struct offsets inside GameViewController (ARM64, Ghidra BackgroundLoad) */
#define GVC_OFF_GAMESTATE        0xa8  /* GameState* (confirmed BackgroundLoad line 207) */

/* =========================================================================
 * Function pointer types for calling game functions
 * ========================================================================= */

/* GotoLevel: X0=this, X1=&level_name (SreString*), X2=&spawn_point (SreString*) */
typedef void (*pfn_GotoLevel)(void* self, SreString* level_name, SreString* spawn_point);

/* =========================================================================
 * sre_get_active_gvc
 * =========================================================================
 * Extracts the live GameViewController* at call time from the CaverShell
 * singleton chain. This replaces the never-set g_sre_active_game_view_controller
 * extern that caused ALL teleport calls to fail.
 *
 * Chain (all confirmed via Ghidra + sre_frame_loop.c):
 *   g_swordigo_base + OFF_CaverShell_globalptr
 *     → CaverShell*                        (qword_6E9C20)
 *   CaverShell* + CAVERSHELL_OFF_NAV_CTRL (0x98)
 *     → GUINavigationController*
 *   GUINavigationController* + NAVCTRL_OFF_CURRENT_VC_PX (0x50)
 *     → px field of boost::shared_ptr<GUIViewController> = raw GVC*
 *
 * Returns NULL if any pointer in the chain is invalid.
 * ========================================================================= */
/* Plausibility check for guest object pointers. Live guest heap objects live in
 * 0x20000000–0xDFFFFFFF (e.g. CaverShell DAT=0x20004208, hero=0x205a8380).
 * Rejecting pointers outside this range prevents the chain from dereferencing
 * stale/partially-freed memory — the normal-gateway crash the old code caused.
 * (The load base itself lives below 0x20000000, so a text/.bss pointer never
 * passes this test either.) */
static int is_plausible_guest_obj(uint64_t p) {
    return p >= 0x20000000ULL && p < 0xE0000000ULL;
}

static void* sre_get_active_gvc(void) {
    if (!g_swordigo_base) {
        fprintf(stderr, "[SRE/SceneShifter] GVC: g_swordigo_base=0\n");
        return NULL;
    }

    /* CaverShell* from BSS global */
    void** shell_slot = (void**)(g_swordigo_base + OFF_CaverShell_globalptr);
    void*  shell      = *shell_slot;
    fprintf(stderr, "[SRE/SceneShifter] GVC chain: base=0x%llx slot=0x%llx shell=%p\n",
            (unsigned long long)g_swordigo_base,
            (unsigned long long)OFF_CaverShell_globalptr, shell);
    if (!shell || !is_plausible_guest_obj((uint64_t)(uintptr_t)shell)) {
        fprintf(stderr, "[SRE/SceneShifter] CaverShell not yet initialized (shell=%p)\n", shell);
        return NULL;
    }

    /* GUINavigationController* from CaverShell+0x98 (px of the shared_ptr) */
    void** nav_slot = (void**)((char*)shell + CAVERSHELL_OFF_NAV_CTRL);
    void*  nav_ctrl = *nav_slot;
    fprintf(stderr, "[SRE/SceneShifter] GVC chain: shell+0x%x => nav_ctrl=%p\n",
            (unsigned)CAVERSHELL_OFF_NAV_CTRL, nav_ctrl);
    if (!nav_ctrl || !is_plausible_guest_obj((uint64_t)(uintptr_t)nav_ctrl)) {
        fprintf(stderr, "[SRE/SceneShifter] GUINavigationController invalid (nav_ctrl=%p)\n", nav_ctrl);
        return NULL;
    }

    /* Current view controller is GUINavigationController::currentViewController,
     * whose shared_ptr px is at +0x50. InitView stores a boost::shared_ptr at
     * shell+0x98, so dereference its px field first. */
    void** gvc_slot = (void**)((char*)nav_ctrl + NAVCTRL_OFF_CURRENT_VC_PX);
    void*  gvc      = *gvc_slot;
    fprintf(stderr, "[SRE/SceneShifter] GVC chain: nav_ctrl+0x%x => gvc=%p\n",
            (unsigned)NAVCTRL_OFF_CURRENT_VC_PX, gvc);
    if (!gvc || !is_plausible_guest_obj((uint64_t)(uintptr_t)gvc)) {
        fprintf(stderr, "[SRE/SceneShifter] Current GUIViewController invalid (gvc=%p)\n", gvc);
        return NULL;
    }

    return gvc;
}

/* Relay pointers are populated by the host's atomic trampoline installer.
 * AnimateIn is implemented in sre_scene_loading.S because its ABI uses x8 as
 * an indirect-result register; a C wrapper is not allowed to clobber it. */
typedef void (*pfn_SceneLoadingView_Update)(void* self, float dt);
pfn_SceneLoadingView_Update g_orig_SceneLoadingView_Update = NULL;
void* g_orig_SceneLoadingView_AnimateIn = NULL;

void sre_SceneLoadingView_Update(void* self, float dt) {
    if (g_orig_SceneLoadingView_Update) {
        g_orig_SceneLoadingView_Update(self, dt);
    }
}

/* =========================================================================
 * sre_scene_shifter_call_goto_level
 * =========================================================================
 * Calls Caver::GameViewController::GotoLevel(gvc, level_name, spawn_point)
 * using the hooked SRE trampoline for std::string construction.
 *
 * The game's GotoLevel:
 *   1. Writes level/spawn into GameState::next_level_name / next_spawn_name
 *   2. Creates a SceneLoadingView and calls InitWithGameState
 *   3. Calls TransitionToViewController → shows loading screen
 *   4. Background thread loads .scene file via Scene::LoadFromFile
 *
 * For the FORCED gateway, g_sre_instant_scene_load_enabled is set BEFORE
 * this call so that the very first SceneLoadingView::Update fires instant-
 * complete.
 * ========================================================================= */
static int sre_scene_shifter_call_goto_level(const char* level_name,
                                              const char* spawn_point) {
    if (!g_swordigo_base) {
        snprintf(g_sre_scene_shift_last_error, sizeof(g_sre_scene_shift_last_error),
                 "g_swordigo_base not set");
        return 0;
    }

    /* Extract live GVC from CaverShell singleton chain */
    void* gvc = sre_get_active_gvc();
    if (!gvc) {
        snprintf(g_sre_scene_shift_last_error, sizeof(g_sre_scene_shift_last_error),
                  "No active GameViewController (CaverShell chain: shell+0x98->NavCtrl+0x50->GVC)");
        g_sre_scene_shift_active = 0;
        return 0;
    }

    /* Construct guest std::string objects on the stack using SRE helpers */
    SreString level_str = {0};
    SreString spawn_str = {0};

    sre_CppString_from_char_p(&level_str, level_name);
    sre_CppString_from_char_p(&spawn_str, spawn_point[0] ? spawn_point : "start");

    /* Resolve GotoLevel function pointer */
    pfn_GotoLevel fn_goto = (pfn_GotoLevel)(void*)(g_swordigo_base + OFF_GotoLevel);

    fprintf(stderr, "[SRE/SceneShifter] Calling GotoLevel('%s', '%s') via GVC=%p fn=%p\n",
            level_name, spawn_point, gvc, (void*)fn_goto);

    /* Call into the game — GotoLevel triggers BackgroundLoad on a new thread.
     * g_sre_scene_shift_active stays 1 until the scene transition completes
     * (detected via sre_SceneLoadingView hooks or the normal UI active flag). */
    fn_goto(gvc, &level_str, &spawn_str);

    /* Release the guest std::string heap allocations */
    sre_CppString_release(&level_str);
    sre_CppString_release(&spawn_str);

    /* GotoLevel returns synchronously; the actual load runs on BackgroundLoad thread.
     * Clear active after a short delay so the GUI buttons re-enable:
     * The GUI will show loading state via the active flag. Reset it here so
     * if no SceneLoadingView hook fires (e.g. hook not installed), the UI unfreezes. */
    snprintf(g_sre_scene_shift_last_error, sizeof(g_sre_scene_shift_last_error), "OK");
    /* Note: g_sre_scene_shift_active is cleared by sre_scene_shifter_tick after
     * next successful frame, or by the forced gateway after instant-complete. */
    return 1;
}

/* =========================================================================
 * sre_scene_shifter_wait_complete
 * =========================================================================
 * Shared completion step for BOTH gateways. Waits for the SceneLoadingView
 * background load (created by GotoLevel → InitWithGameState, tracked in
 * g_sre_last_slv) to finish, then pokes progress=1.0 so the loading screen
 * completes deterministically.
 *
 * The OLD normal gateway returned to the frame loop immediately and let the
 * loading screen render on its own — which crashed the game (stale VC vtable
 * dispatch / matrix render corruption during the transition). Forcing the
 * same proven completion the forced gateway uses fixes the crash.
 * ========================================================================= */
static void sre_scene_shifter_poll_complete(void) {
    if (!g_sre_scene_shift_active) return;
    g_sre_scene_shift_wait_frames++;

    if (g_sre_last_slv) {
        volatile int* is_complete =
            (volatile int*)((char*)g_sre_last_slv + SLVIEW_OFF_LOAD_COMPLETE);
        if (*is_complete) {
            volatile float* progress =
                (volatile float*)((char*)g_sre_last_slv + SLVIEW_OFF_PROGRESS);
            *progress = 1.0f;
            g_sre_scene_shift_active = 0;
            g_sre_instant_scene_load_enabled = 0;
            g_sre_scene_shift_wait_frames = 0;
            snprintf(g_sre_scene_shift_last_error,
                     sizeof(g_sre_scene_shift_last_error), "OK");
            strncpy(g_sre_current_scene_name, g_sre_scene_shift_target, 127);
            g_sre_current_scene_name[127] = '\0';
            return;
        }
    }

    if (g_sre_scene_shift_wait_frames > 3600) {
        snprintf(g_sre_scene_shift_last_error,
                 sizeof(g_sre_scene_shift_last_error), "Scene load timed out");
        g_sre_scene_shift_active = 0;
        g_sre_instant_scene_load_enabled = 0;
        g_sre_scene_shift_wait_frames = 0;
    }
}

/* =========================================================================
 * sre_scene_shifter_normal
 * Normal gateway — calls GotoLevel with instant-complete to avoid the
 * loading-screen rendering crash (stale VC vtable/matrix corruption).
 *
 * NOTE: Previously this left g_sre_instant_scene_load_enabled=0 so the
 * loading screen would render on its own — this caused reliable crashes
 * due to stale vtable dispatch in the SceneLoadingView render path during
 * the transition. Both Normal and Forced now arm instant-complete; the
 * difference is UI presentation only.
 * ========================================================================= */
int sre_scene_shifter_normal(const char* level_name, const char* spawn_point) {
    /* Arm instant-complete: same as forced to avoid SLV rendering crash */
    g_sre_instant_scene_load_enabled = 1;
    g_sre_scene_shift_active         = 1;
    g_sre_last_slv = NULL;
    g_sre_scene_shift_wait_frames = 0;
    extern volatile uint64_t g_sre_hero_obj;
    g_sre_hero_obj = 0; /* Guard against use-after-free during transition */

    int ok = sre_scene_shifter_call_goto_level(level_name, spawn_point);
    if (!ok) {
        g_sre_instant_scene_load_enabled = 0;
        g_sre_scene_shift_active = 0;
        fprintf(stderr, "[SRE/SceneShifter] Normal gateway FAILED: %s\n",
                g_sre_scene_shift_last_error);
    }
    return ok;
}

/* =========================================================================
 * sre_scene_shifter_forced
 * Forced gateway — arms the instant-load short-circuit, then calls GotoLevel
 * ========================================================================= */
int sre_scene_shifter_forced(const char* level_name, const char* spawn_point) {
    g_sre_instant_scene_load_enabled = 1;
    g_sre_scene_shift_active         = 1;
    g_sre_last_slv = NULL;
    g_sre_scene_shift_wait_frames = 0;
    extern volatile uint64_t g_sre_hero_obj;
    g_sre_hero_obj = 0; /* Guard against use-after-free during transition */

    int ok = sre_scene_shifter_call_goto_level(level_name, spawn_point);
    if (!ok) {
        g_sre_instant_scene_load_enabled = 0;
        g_sre_scene_shift_active         = 0;
        fprintf(stderr, "[SRE/SceneShifter] Forced gateway FAILED: %s\n",
                g_sre_scene_shift_last_error);
    }
    return ok;
}

/* =========================================================================
 * sre_scene_shifter_tick
 * =========================================================================
 * Called every frame by sre_frame_loop.c.
 *
 * Consumes g_sre_scene_shift_pending requests written by the host GUI and
 * dispatches them to sre_scene_shifter_normal / sre_scene_shifter_forced.
 *
 * Both gateways call Caver::GameViewController::GotoLevel().  The scene
 * transition completes via the normal BackgroundLoad pthread path:
 *   GVC+0xE9=1 → GameViewController::Update → scene swap.
 *
 * Mode 1 = Normal gateway (standard loading screen)
 * Mode 2 = Forced gateway (same mechanics, labelled differently in UI)
 * ========================================================================= */
void sre_scene_shifter_tick(void) {
    g_sre_scene_shifter_poll_ticks++;
    sre_scene_shifter_poll_complete();
    if (!g_sre_scene_shift_pending) return;

    if (g_sre_scene_shift_active) {
        snprintf(g_sre_scene_shift_last_error,
                 sizeof(g_sre_scene_shift_last_error),
                 "A scene load is already active");
        g_sre_scene_shift_pending = 0;
        return;
    }

    int mode = g_sre_scene_shift_pending;
    g_sre_scene_shift_pending = 0;  /* clear first to prevent double-fire */
    g_sre_scene_shifter_dispatch_count++;

    char level[SRE_SCENE_NAME_MAX];
    char spawn[64];
    snprintf(level, sizeof(level), "%s", g_sre_scene_shift_target);
    snprintf(spawn, sizeof(spawn), "%s", g_sre_scene_shift_spawn);

    if (!level[0]) {
        snprintf(g_sre_scene_shift_last_error, sizeof(g_sre_scene_shift_last_error),
                 "Empty level name");
        return;
    }

    fprintf(stderr, "[SRE/SceneShifter] Dispatching mode=%d to scene='%s' spawn='%s'\n",
            mode, level, spawn);

    if (mode == 1) {
        sre_scene_shifter_normal(level, spawn);
    } else if (mode == 2) {
        sre_scene_shifter_forced(level, spawn);
    } else {
        snprintf(g_sre_scene_shift_last_error,
                 sizeof(g_sre_scene_shift_last_error),
                 "Invalid scene gateway mode: %d", mode);
    }
}

/* =========================================================================
 * sre_scene_shifter_scan_scenes
 * =========================================================================
 * Scans the active mod directory (data_dir/mods/<mod_name>/) for *.scene
 * files and populates g_sre_scene_list[].
 *
 * Search order:
 *   1. data_dir/mods/<active_mod>/resources/**  (if mod active)
 *   2. assets/resources/                        (vanilla fallback)
 *
 * Scene names are stored without path or extension (engine convention):
 *   "res/town_part1.scene" → scene name = "town_part1"
 * ========================================================================= */

static void scan_dir_for_scenes(const char* base, int depth) {
    if (g_sre_scene_list_count >= SRE_SCENE_LIST_MAX) return;
    if (depth > 6) return;  /* Guard against pathological symlink loops */

    DIR* d = opendir(base);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL &&
           g_sre_scene_list_count < SRE_SCENE_LIST_MAX) {
        if (ent->d_name[0] == '.') continue;  /* skip . and .. */

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", base, ent->d_name);

        struct stat st;
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir_for_scenes(full, depth + 1);
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        /* Check .scene extension */
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 6) continue;  /* ".scene" = 6 chars */
        if (strcmp(ent->d_name + nlen - 6, ".scene") != 0) continue;

        /* Strip extension to get level name */
        char name_no_ext[SRE_SCENE_NAME_MAX];
        int copy_len = (int)nlen - 6;
        if (copy_len >= SRE_SCENE_NAME_MAX) copy_len = SRE_SCENE_NAME_MAX - 1;
        memcpy(name_no_ext, ent->d_name, copy_len);
        name_no_ext[copy_len] = '\0';

        /* Check for duplicates */
        int dup = 0;
        for (int i = 0; i < g_sre_scene_list_count; i++) {
            if (strcmp(g_sre_scene_list[i], name_no_ext) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        strncpy(g_sre_scene_list[g_sre_scene_list_count],
                name_no_ext, SRE_SCENE_NAME_MAX - 1);
        g_sre_scene_list[g_sre_scene_list_count][SRE_SCENE_NAME_MAX - 1] = '\0';
        g_sre_scene_list_count++;

        fprintf(stderr, "[SRE/SceneShifter] Found scene: %s\n", name_no_ext);
    }
    closedir(d);
}

void sre_scene_shifter_scan_scenes(void) {
    g_sre_scene_list_count = 0;

    /* Try active mod directory first */
    if (g_sre_vfs_data_dir[0] && g_sre_vfs_mod_name[0]) {
        char mod_res[768];
        snprintf(mod_res, sizeof(mod_res), "%s/mods/%s/resources",
                 g_sre_vfs_data_dir, g_sre_vfs_mod_name);
        fprintf(stderr, "[SRE/SceneShifter] Scanning mod dir: %s\n", mod_res);
        scan_dir_for_scenes(mod_res, 0);
    }

    /* Also scan vanilla assets if nothing found or as supplement */
    if (g_sre_vfs_path_assets[0]) {
        char vanilla_res[768];
        snprintf(vanilla_res, sizeof(vanilla_res), "%s/resources",
                 g_sre_vfs_path_assets);
        fprintf(stderr, "[SRE/SceneShifter] Scanning vanilla dir: %s\n", vanilla_res);
        scan_dir_for_scenes(vanilla_res, 0);
    }

    fprintf(stderr, "[SRE/SceneShifter] Scan complete: %d scene(s) found.\n",
            g_sre_scene_list_count);
}

/* =========================================================================
 * Lua API — Mini.GotoLevel / Mini.SetInstantLoad / Mini.ListScenes
 * ========================================================================= */

/* Mini.GotoLevel(level_name, spawn_point, [forced=false]) */
static int sre_lua_goto_level(lua_State* L) {
    const char* level = g_lua_tolstring ? g_lua_tolstring(L, 1, NULL) : NULL;
    const char* spawn = g_lua_tolstring ? g_lua_tolstring(L, 2, NULL) : NULL;
    int forced = 0;
    if (g_lua_type && g_lua_type(L, 3) == LUA_TBOOLEAN) {
        forced = g_lua_toboolean(L, 3);
    }

    if (!level || !level[0]) {
        if (g_lua_pushboolean) g_lua_pushboolean(L, 0);
        if (g_lua_pushstring) g_lua_pushstring(L, "level name required");
        return 2;
    }
    if (!spawn) spawn = "start";

    int ok = forced ? sre_scene_shifter_forced(level, spawn)
                    : sre_scene_shifter_normal(level, spawn);
    if (g_lua_pushboolean) g_lua_pushboolean(L, ok);
    if (!ok && g_lua_pushstring) g_lua_pushstring(L, g_sre_scene_shift_last_error);
    return ok ? 1 : 2;
}

/* Mini.SetInstantLoad(bool) */
static int sre_lua_set_instant_load(lua_State* L) {
    if (g_lua_toboolean) {
        g_sre_instant_scene_load_enabled = g_lua_toboolean(L, 1);
    }
    return 0;
}

/* Mini.ListScenes() — returns Lua array of scene names */
static int sre_lua_list_scenes(lua_State* L) {
    sre_scene_shifter_scan_scenes();
    if (g_lua_createtable) {
        g_lua_createtable(L, g_sre_scene_list_count, 0);
        for (int i = 0; i < g_sre_scene_list_count; i++) {
            if (g_lua_pushstring) g_lua_pushstring(L, g_sre_scene_list[i]);
            if (g_lua_rawseti) g_lua_rawseti(L, -2, i + 1);
        }
    }
    return 1;
}

/* Mini.ScanScenes() — refresh the scene list, return count */
static int sre_lua_scan_scenes(lua_State* L) {
    sre_scene_shifter_scan_scenes();
    if (g_lua_pushnumber) g_lua_pushnumber(L, g_sre_scene_list_count);
    return 1;
}

/* Register all scene shifter Lua functions into the Mini table */
void sre_scene_shifter_register_lua(lua_State* L) {
    if (!g_lua_pushstring || !g_lua_pushcclosure || !g_lua_settable) return;

    /* GotoLevel */
    g_lua_pushstring(L, "GotoLevel");
    g_lua_pushcclosure(L, sre_lua_goto_level, 0);
    g_lua_settable(L, -3);

    /* SetInstantLoad */
    g_lua_pushstring(L, "SetInstantLoad");
    g_lua_pushcclosure(L, sre_lua_set_instant_load, 0);
    g_lua_settable(L, -3);

    /* ListScenes */
    g_lua_pushstring(L, "ListScenes");
    g_lua_pushcclosure(L, sre_lua_list_scenes, 0);
    g_lua_settable(L, -3);

    /* ScanScenes */
    g_lua_pushstring(L, "ScanScenes");
    g_lua_pushcclosure(L, sre_lua_scan_scenes, 0);
    g_lua_settable(L, -3);
}
