/* =============================================================================
 * sre_frame_loop.c — Full PC/Emulator-Safe Game Update Loop
 * =============================================================================
 *
 * PURPOSE
 * ───────
 * Replaces the original Android-style monolithic update pipeline:
 *
 *   Java_com_touchfoo_swordigo_Native_updateApplication
 *     └─ CaverShell::Update(dt)
 *          ├─ AchievementsManager::Update(dt)  [DEAD on PC — hooked to no-op]
 *          ├─ AudioSystem::Update(dt)           [forwarded to host OpenAL]
 *          ├─ GameSceneView::Update(dt)         [full reimpl in sre_scene_update.c]
 *          │    ├─ GameSceneController::Update(dt)
 *          │    │    └─ Scene::Update(dt)
 *          │    │         ├─ ProgramState::Update(dt)  [TIME-SLICED HERE]
 *          │    │         ├─ SceneGrid::UpdateVisibleAreasWithCamera
 *          │    │         └─ Physics / collision bounds
 *          │    ├─ HealthBar / ManaBar / CoinBar setters
 *          │    ├─ CanCastSkill shared_ptr refcount (non-atomic)
 *          │    └─ GUIEffect::Update (3× for bars/flash/cinematic)
 *          └─ GUIApplication::DispatchEvents  [no-op on PC — SDL handles input]
 *
 * ARCHITECTURE
 * ────────────
 * We use three-level trampolines:
 *
 *   Level 1 — updateApplication  (0x1478ccc)
 *     → sre_updateApplication  (in sre_lua.c, already hooked)
 *       Pre/post frame: Lua console, Mini injection, game speed scaling
 *       Calls → sre_frame_update() below for the actual tick
 *
 *   Level 2 — CaverShell::Update  (vtable slot 13 @ this+0x68)
 *     → sre_CaverShell_Update
 *       Strips Android-specific calls (Achievements, GUIApplication::DispatchEvents)
 *       Routes AudioSystem::Update, then dispatches GameSceneView vtable
 *
 *   Level 3 — ProgramState::Update  (0x4c15fc)
 *     → sre_ProgramState_Update  (in sre_lua.c, already hooked)
 *       Time-sliced: caps at FRAME_COROUTINE_BUDGET_MS per host frame
 *
 * OFFSETS (v1.4.12 ARM64, from nm -D libswordigo.so + Ghidra)
 * ──────────────────────────────────────────────────────────────
 *   Java_..._updateApplication     0x1478ccc  (JNI stub, 7 insns)
 *   CaverShell::Update              0x210efc   (vtable slot 13)
 *   GameSceneView::Update           0x34ed2c   (vtable slot @ this+0x48)
 *   GameSceneController::Update     0x349d84
 *   Scene::Update                   0x465968
 *   ProgramState::Update            0x4c15fc
 *   AudioSystem::Update             (called from CaverShell at runtime via singleton)
 *   AchievementsManager::Update     (no-op replacement)
 *   GUIApplication::DispatchEvents  (no-op replacement)
 *
 * GUEST MEMORY LAYOUT (CaverShell, from Ghidra FWShell.c + CaverShell.c)
 * ─────────────────────────────────────────────────────────────────────────
 *   CaverShell global ptr: DAT_007f3c20  (runtime)
 *   vtable[13]  =  CaverShell::Update (offset 0x68 in vtable)
 *   +0x20       =  FWShellPreferences
 *   +0x88       =  GUIView* (root scene view, cast to GameSceneView)
 *   +0xa8       =  paused flag (byte): if nonzero, dt → 0.0
 *
 * =============================================================================
 */

#include "sre.h"
#include "sre_lua.h"
#include "sre_caver.h"

/* clock_gettime forward declarations (freestanding ARM64 — no libc headers) */
struct timespec { long tv_sec; long tv_nsec; };
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
extern int clock_gettime(int clk_id, struct timespec *tp);

/* ──────────────────────────────────────────────────────────────────────────
 * External linkage from other SRE modules
 * ────────────────────────────────────────────────────────────────────────── */

extern uint64_t   g_swordigo_base;
extern lua_State* g_sre_last_lua_state;
extern int        g_lua_console_pending;
extern void       sre_run_console(lua_State* L);
extern void       sre_mini_ensure_injected(lua_State* L);

/* From sre_scene_update.c */
extern void sre_GameSceneView_Update(void* self, float deltaTime);
extern void sre_AchievementsManager_Update(void* self, float deltaTime);
/* JOB 1 (freeze fix) — direct GVC world-drive fallback, see step 6.5 below. */
extern volatile int g_sre_scene_loading;
/* JOB 1 (freeze fix) — Scene* captured by sre_Scene_FinishLoad; used as the
 * direct-drive fallback when the stuck NavController transition leaves the menu
 * VC current so the nav-chain can't reach the new scene. */
extern volatile uint64_t g_sre_finishloaded_scene;
extern volatile uint64_t g_sre_world_drive_count;
/* sre_is_valid_vtable_ptr / sre_is_valid_code_ptr are declared in sre.h. */

/* From sre_lua.c */
extern void sre_ProgramState_Update(void* self, float deltaTime);

/* Z-walk is implemented in Lua (see sre_zwalk_loop in sre_mini_api.c),
 * driven by g_sre_z_walk_axis written by the host each frame. */

/* ──────────────────────────────────────────────────────────────────────────
 * Time-slice budget for ProgramState (Lua coroutine) tree per frame
 *
 * On a 60 Hz frame (16.6 ms), we allow coroutines to consume at most this
 * many milliseconds. If exceeded, we stop iterating child states and defer
 * them to the next frame. This guarantees SDL/X11 event pump is never
 * starved, eliminating "Swordigo Desktop is Not Responding" hangs.
 * ────────────────────────────────────────────────────────────────────────── */
#define FRAME_COROUTINE_BUDGET_MS  8.0f

/* Global: read by sre_ProgramState_Update to check if time budget is exhausted */
volatile int  g_sre_frame_budget_expired  = 0;
volatile float g_sre_frame_budget_ms_remaining = FRAME_COROUTINE_BUDGET_MS;

/* Frame timing state */
static struct timespec s_frame_start;
static int             s_frame_timing_init = 0;

/* Host-visible diagnostics. These counters make it possible to distinguish a
 * dead JNI hook from a live frame loop whose scene poll simply saw no work. */
volatile uint64_t g_sre_frame_update_ticks = 0;
volatile uint64_t g_sre_frame_shell_ticks  = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * Game speed multiplier (from sre_lua.c)
 * ────────────────────────────────────────────────────────────────────────── */
extern float g_sre_game_speed;     /* 1.0 = normal, 0.5 = half, 2.0 = double */
extern int   g_game_paused;        /* Set by mod_toggle_pause() */

/* ──────────────────────────────────────────────────────────────────────────
 * Original function relay pointers
 *
 * These are set by the trampoline installer in main.cpp via the hook table.
 * Each original function is saved to a relay cave (code at 0x3000xxx).
 * ────────────────────────────────────────────────────────────────────────── */

/* CaverShell::Update — original relay (set by trampoline installer) */
typedef void (*pfn_CaverShell_Update)(void* self, float dt);
pfn_CaverShell_Update g_orig_CaverShell_Update = NULL;

/* AudioSystem::Update — called inside original CaverShell::Update */
typedef void (*pfn_AudioSystem_Update)(void* self, float dt);

/* GUIView vtable update — slot at +0x48 from root GUIView */
typedef void (*pfn_GUIView_Update)(void* self, float dt);

/* GUIApplication::DispatchEvents — Android touch event queue; PC no-op */
typedef void (*pfn_DispatchEvents)(void* self);

/* Scene::Update — called from GameSceneController.
 * NOTE: the live relay pointer is g_orig_Scene_Update_fn (declared in sre.h).
 * A previously-defined unused global `g_orig_Scene_Update` was removed here
 * because it had zero references and only shadowed the real relay name. */
typedef void (*pfn_Scene_Update)(void* self, float dt);

/* ──────────────────────────────────────────────────────────────────────────
 * sre_frame_budget_start — called at the top of each host frame
 *
 * Records the wall-clock start time for this frame's coroutine time-slice.
 * Must be called before sre_updateApplication dispatches into the engine.
 * ────────────────────────────────────────────────────────────────────────── */
void sre_frame_budget_start(void) {
    clock_gettime(CLOCK_MONOTONIC, &s_frame_start);
    g_sre_frame_budget_expired   = 0;
    g_sre_frame_budget_ms_remaining = FRAME_COROUTINE_BUDGET_MS;
    s_frame_timing_init = 1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * sre_frame_budget_check — inline poll used by sre_ProgramState_Update
 *
 * Returns 1 if the frame's coroutine time budget is exhausted, 0 otherwise.
 * sre_ProgramState_Update should call this at the top of the child-iteration
 * loop to bail out early when we're out of budget.
 * ────────────────────────────────────────────────────────────────────────── */
int sre_frame_budget_check(void) {
    if (!s_frame_timing_init) return 0;
    if (g_sre_frame_budget_expired) return 1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    float elapsed_ms = (float)(now.tv_sec - s_frame_start.tv_sec) * 1000.0f
                     + (float)(now.tv_nsec - s_frame_start.tv_nsec) / 1e6f;

    if (elapsed_ms >= FRAME_COROUTINE_BUDGET_MS) {
        g_sre_frame_budget_expired = 1;
        g_sre_frame_budget_ms_remaining = 0.0f;
        return 1;
    }
    g_sre_frame_budget_ms_remaining = FRAME_COROUTINE_BUDGET_MS - elapsed_ms;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * sre_GUIApplication_DispatchEvents — PC no-op
 *
 * On Android, this drained the touch MotionEvent queue into the engine's
 * FWShell touch set. On PC we use SDL events directly. Calling the original
 * is both unnecessary and dangerous (the event queue may be stale/corrupt).
 * ────────────────────────────────────────────────────────────────────────── */
void sre_GUIApplication_DispatchEvents(void* self) {
    (void)self;
    /* PC no-op: SDL processes input before updateApplication is called. */
}

/* ──────────────────────────────────────────────────────────────────────────
 * sre_CaverShell_Update — full PC-safe reimplementation
 *
 * Replaces Caver::CaverShell::Update(float) at 0x210efc.
 *
 * Original execution in CaverShell::Update(float):
 *   1. if (this[0xa8]) param_1 = 0.0;              // paused check
 *   2. AchievementsManager::Update(sharedManager(), dt)  → NO-OP on PC
 *   3. AudioSystem::Update(sharedSystem(), dt)     → forwarded
 *   4. Resize check → GUIView::SetFrame() if size changed
 *   5. (*root_view->vtable[+0x48])(dt)             → GameSceneView::Update
 *   6. GUIApplication::DispatchEvents()            → NO-OP on PC
 *
 * ARM64 ABI: X0 = this (CaverShell*), S0 = float dt
 * ────────────────────────────────────────────────────────────────────────── */
void sre_CaverShell_Update(void* self, float dt) {
    if (!self) return;
    char* shell = (char*)self;

    /* 1. Paused check (offset +0xa8): zero-out dt when engine is paused */
    if ((uint8_t)shell[0xa8] != 0) {
        dt = 0.0f;
    }

    /* 2. Apply SRE game speed multiplier (from mod settings / cheat mode) */
    if (!g_game_paused && g_sre_game_speed > 0.0f && g_sre_game_speed != 1.0f) {
        dt *= g_sre_game_speed;
    }

    /* 3. AchievementsManager::Update → PC NO-OP (sre_AchievementsManager_Update is a no-op) */
    /* Not calling: dead code on PC, contains STXR/LDXR spin loops for GPlay Games */

    /* 4. AudioSystem::Update — find singleton via sharedSystem() and call */
    {
        /* Offsets verified with nm -D -C libswordigo.so. */
        #define OFF_AudioSystem_sharedSystem  0x47e32c
        #define OFF_AudioSystem_Update        0x47fb9c

        typedef void* (*pfn_sharedSystem)(void);
        typedef void  (*pfn_audioUpdate)(void* self, float dt);

        pfn_sharedSystem sharedSystem = (pfn_sharedSystem)(g_swordigo_base + OFF_AudioSystem_sharedSystem);
        void* audio_sys = sharedSystem();
        if (audio_sys) {
            pfn_audioUpdate audioUpdate = (pfn_audioUpdate)(g_swordigo_base + OFF_AudioSystem_Update);
            audioUpdate(audio_sys, dt);
        }
    }

    /* 5. Dynamic view frame resizing (from decompiled CaverShell::Update lines 1200-1212)
     *    FWShellPreferences::Get(this+0x20, field) returns screen dimensions.
     *    If root_view's frame dimensions changed, call GUIView::SetFrame.
     *    We replicate this using direct member reads for maximum compatibility. */
    {
        /* FWShellPreferences::Get(pref, 2) = screen width (int)  */
        /* FWShellPreferences::Get(pref, 3) = screen height (int) */
        #define OFF_FWShellPreferences_Get  0x47e2d0

        typedef int (*pfn_PrefGet)(void* self, int field);
        pfn_PrefGet prefGet = (pfn_PrefGet)(g_swordigo_base + OFF_FWShellPreferences_Get);
        void* prefs = (void*)(shell + 0x20);

        int sw = prefGet(prefs, 2);  /* screen width */
        int sh = prefGet(prefs, 3);  /* screen height */

        void** root_view_ptr = (void**)(shell + 0x88);
        void* root_view = *root_view_ptr;

        if (root_view) {
            /* GUIView::frame is at +0x7c (float w) and +0x80 (float h) */
            float cur_w = *(float*)((char*)root_view + 0x7c);
            float cur_h = *(float*)((char*)root_view + 0x80);

            float eps = 0.0001f;
            float fw = (float)sw;
            float fh = (float)sh;
            if ((cur_w < fw - eps || cur_w > fw + eps) ||
                (cur_h < fh - eps || cur_h > fh + eps)) {
                /* GUIView::SetFrame(this, rect) — rect is a {float x, y, w, h} struct */
                #define OFF_GUIView_SetFrame  0x49f5c8
                typedef void (*pfn_SetFrame)(void* self, void* rect);
                pfn_SetFrame setFrame = (pfn_SetFrame)(g_swordigo_base + OFF_GUIView_SetFrame);
                float rect[4] = { 0.0f, 0.0f, fw, fh };
                setFrame(root_view, rect);
                /* Re-read after SetFrame (may realloc) */
                root_view = *(void**)(shell + 0x88);
            }

            /* 6. Dispatch via vtable[+0x48] = GameSceneView::Update (or whatever the root view is).
             *    Our sre_GameSceneView_Update is already installed via trampoline at 0x34ed2c,
             *    so calling through the vtable naturally lands in our reimplementation. */
            if (root_view) {
                void**  vtable = *(void***)root_view;
                /* vtable[+0x48 / 8] = slot 9 in 64-bit vtable (offset 0x48) */
                /* HARDENING: In some boot/state paths the root-view pointer at
                 * shell+0x88 (or its vtable slot 9) is stale/poisoned — the
                 * resolved function pointer lands in .dynstr (0x1077168–0x1162a8c,
                 * e.g. the observed 0x10771ac "CaverShell" type-name string) or is
                 * the 0xdeadc0de12345678 canary. Calling through it jumps the JIT
                 * into rodata and halts every frame. Validate the vtable pointer
                 * AND that the resolved slot points into executable .text
                 * (0x1203e90–0x1583478) before dispatching; otherwise skip this
                 * frame's root-view update so the game loop keeps running.
                 * FIX6: the .text/alignment test now delegates to the shared
                 * sre_is_valid_code_ptr() helper (identical union range). */
                uint64_t vtbl_addr = (uint64_t)(uintptr_t)vtable;
                pfn_GUIView_Update root_update =
                    sre_is_valid_vtable_ptr(vtbl_addr)
                        ? (pfn_GUIView_Update)vtable[9]
                        : (pfn_GUIView_Update)0;
                uint64_t fn_addr = (uint64_t)(uintptr_t)root_update;
                /* FIX6: use the shared code-pointer validator (same union range,
                 * same 4-byte alignment requirement — no bound narrowed). */
                int fn_in_text = sre_is_valid_code_ptr(fn_addr);
                if (root_update && fn_in_text) {
                    root_update(root_view, dt);
                } else if (fn_addr != 0) {
                    static int sre_bad_rootview_warn = 0;
                    if (sre_bad_rootview_warn++ < 5) {
                        printf("[SRE/FrameLoop] Skipped root-view update: bad vtable/fn ptr "
                               "(vtable=0x%llx slot9=0x%llx) — not executable .text\n",
                               (unsigned long long)vtbl_addr,
                               (unsigned long long)fn_addr);
                    }
                }
                /* Network polling belongs to the frame owner, once per frame.
                 * Keeping it here avoids competing consumers draining the same
                 * non-blocking UDP socket from both HUD and shell updates. */
                {
                    extern void sre_raknet_lan_sync_update(void* view);
                    sre_raknet_lan_sync_update(root_view);
                }
            }
        }
    }

    /* 7. GUIApplication::DispatchEvents → PC NO-OP */
    /* sre_GUIApplication_DispatchEvents() is a no-op; SDL drives input */
}

/* ──────────────────────────────────────────────────────────────────────────
 * sre_frame_update — called from sre_updateApplication (sre_lua.c)
 *
 * This is the main entry point invoked every host frame. It:
 *   1. Resets the per-frame coroutine time budget
 *   2. Fetches the CaverShell singleton
 *   3. Calls through sre_CaverShell_Update (which triggers the full tree)
 *   4. Budget expiry is polled inside sre_ProgramState_Update child loop
 *
 * Call from sre_updateApplication in sre_lua.c:
 *   extern void sre_frame_update(void* env, void* obj, float dt);
 *   sre_frame_update(env, obj, dt);
 * ────────────────────────────────────────────────────────────────────────── */
void sre_frame_update(void* env, void* obj, float dt) {
    (void)env; (void)obj;

    g_sre_frame_update_ticks++;

    /* Reset per-frame coroutine time budget */
    sre_frame_budget_start();

    /* IDA: Native_updateApplication loads qword_6E9C20, then dispatches
     * vtable+0x68. This is the runtime CaverShell singleton for v1.4.12. */
    #define OFF_CaverShell_globalptr  0x6e9c20

    void** caverShell_ptr_addr = (void**)(g_swordigo_base + OFF_CaverShell_globalptr);
    void* shell = *caverShell_ptr_addr;

    if (shell) {
        g_sre_frame_shell_ticks++;

        /* Call our PC-safe CaverShell::Update.
     * This will cascade into GameSceneView::Update (hooked via trampoline)
     * and ProgramState::Update (hooked via trampoline), both of which are
     * safe to call here. */
        sre_CaverShell_Update(shell, dt);
    }

    /* Scene Shifter — consume pending teleport requests from the host GUI.
     * Called after CaverShell::Update so we never interrupt an in-flight scene
     * load. Sets g_sre_instant_scene_load_enabled before calling GotoLevel
     * for the forced gateway, or calls GotoLevel directly for the normal one. */
    extern void sre_scene_shifter_tick(void);
    sre_scene_shifter_tick();

    /* ─── JOB 1 FIX: menu→game scene-transition freeze bypass ────────────────
     * A GotoLevel launched from the MENU state completes Scene::FinishLoad but
     * then the guest SPINS forever in a C++ dynamic_cast RTTI walk:
     *   __vmi_class_type_info::__do_upcast  (nm 0x54c6d4, in .text) reached from
     *   Caver::GUINavigationController::FinishTransitionToViewController (0x49a42c)
     * which runs after FinishLoad to swap the menu ViewController to the
     * GameViewController. Under the Dynarmic ARM64 JIT that upcast hierarchy
     * walk never terminates (a mis-relocated type_info/vtable pointer), so the
     * VC swap never completes, GameSceneView::Update is never dispatched through
     * shell+0x88, and the world-drive gate in sre_GameSceneView_Update never
     * sees a scene → FrameDiag stays world_drive=0 / ps_ticks=0 / scene=(none).
     *
     * FIX (option (c) from the task): bypass the stuck NavController transition
     * entirely. Resolve the live GameViewController directly from the CaverShell
     * singleton chain (shell 0x6E9C20 → +0x98 nav_ctrl → +0x50 current VC = GVC),
     * read its GameSceneController (GVC → GameSceneView field → +0xF0 = GSC, or
     * the GSC held directly), and if a valid Scene exists, drive
     * GameSceneController::Update (nm 0x349d84) once per frame. That runs
     * Scene::Update — object updates, physics, and the deferred-deletion loop —
     * so the world ticks and destruction runs even though the menu→game VC swap
     * is still stuck in __do_upcast. We never touch __do_upcast itself and we
     * never call the stuck FinishTransition path, so the menu render is
     * unaffected: this fallback only fires once a real Scene is reachable.
     *
     * Guarded so it is a strict no-op until gameplay is actually reachable:
     *   - only when a scene load is NOT in progress
     *   - only when the CaverShell → GVC chain resolves to plausible heap ptrs
     *   - only when GVC+0xE9 (GameViewController active flag) is set — i.e. the
     *     GVC has been made current (BackgroundLoad finished), which is exactly
     *     the post-FinishLoad state the freeze leaves us in
     *   - Scene pointer + vtable validated before the drive (never dispatch
     *     into garbage). The per-frame double-step guard lives inside
     *     GameSceneController::Update's own Scene+0x310 frame-counter check via
     *     sre_GameSceneView_Update, so calling GSC::Update here is safe even if
     *     the normal chain later revives. */
    if (!g_sre_scene_loading && g_swordigo_base) {
        /* Whether the nav-chain path (below) already drove the world this frame;
         * if not, the captured-scene fallback drives Scene::Update directly. */
        int s_job1_scene_driven = 0;
        /* Frame-counter dedup for the direct-scene fallback (avoid double-step). */
        static uint64_t s_job1_last_scene = 0;
        static int      s_job1_last_frame = -1;
        void** shell_slot = (void**)(g_swordigo_base + OFF_CaverShell_globalptr);
        void*  shell2 = *shell_slot;
        if (shell2 && (uint64_t)(uintptr_t)shell2 >= 0x20000000ULL &&
            (uint64_t)(uintptr_t)shell2 < 0xE0000000ULL) {
            void* nav = *(void**)((char*)shell2 + 0x98);           /* CaverShell+0x98 = nav_ctrl */
            if (nav && (uint64_t)(uintptr_t)nav >= 0x20000000ULL &&
                (uint64_t)(uintptr_t)nav < 0xE0000000ULL) {
                void* gvc = *(void**)((char*)nav + 0x50);          /* nav_ctrl+0x50 = current VC (GVC) */
                if (gvc && (uint64_t)(uintptr_t)gvc >= 0x20000000ULL &&
                    (uint64_t)(uintptr_t)gvc < 0xE0000000ULL) {
                    /* GameViewController active flag (Ghidra BackgroundLoad: GVC+0xE9=1
                     * once the game scene is made current). Only drive when it is set,
                     * so the menu (where GVC is a MainMenuVC) never triggers this. */
                    uint8_t gvc_active = *(uint8_t*)((char*)gvc + 0xE9);
                    if (gvc_active) {
                        /* GVC holds a shared_ptr<GameSceneView> whose px is the
                         * GameSceneView; its GameSceneController is at GSV+0xF0.
                         * The GVC layout mirrors the shell root-view: the
                         * GameSceneController shared_ptr px is reachable at GVC+0xF0
                         * in the same object family. Resolve GSC, then Scene at
                         * GSC+0x20, validate, and drive GSC::Update. */
                        void* gsc = *(void**)((char*)gvc + 0xF0);  /* GameSceneController* */
                        if (gsc && (uint64_t)(uintptr_t)gsc >= 0x20000000ULL &&
                            (uint64_t)(uintptr_t)gsc < 0xE0000000ULL) {
                            void* scene = *(void**)((char*)gsc + 0x20); /* GSC+0x20 = Scene* */
                            if (scene && (uint64_t)(uintptr_t)scene > 0x10000ULL) {
                                uint64_t scene_vt = *(uint64_t*)scene;
                                if (sre_is_valid_vtable_ptr(scene_vt)) {
                                    static int s_bypass_logged = -1;
                                    if (s_bypass_logged < 0)
                                        s_bypass_logged = getenv("SRE_WORLD_DEBUG") != NULL ? 0 : 2;
                                    if (s_bypass_logged == 0) {
                                        fprintf(stderr,
                                            "[SRE/FrameLoop] JOB1 bypass: driving GameSceneController::Update "
                                            "directly (gvc=%p gsc=%p scene=%p) — NavController transition "
                                            "stuck in __do_upcast(0x54c6d4)\n", gvc, gsc, scene);
                                        s_bypass_logged = 1; /* log once */
                                    }
                                    typedef void (*pfn_gsc_update)(void*, float);
                                    pfn_gsc_update gsc_update =
                                        (pfn_gsc_update)(g_swordigo_base + 0x349d84);
                                    /* This ticks Scene::Update; sre_GameSceneView_Update's
                                     * Scene+0x310 frame-counter guard prevents double-step
                                     * if the normal chain also revives later. */
                                    gsc_update(gsc, dt);
                                    g_sre_world_drive_count++;
                                    s_job1_scene_driven = 1;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* ─── JOB 1 FALLBACK: drive the captured Scene directly ──────────────
         * The chain above resolves the NavController's CURRENT view controller
         * (nav_ctrl+0x50). But when a GotoLevel is launched from the MENU, the
         * transition that would make the GameViewController current is exactly
         * what stalls inside __do_upcast(0x54c6d4) — so nav_ctrl+0x50 still
         * points at the MENU VC and GVC+0xE9 never gets set. The chain path can
         * therefore never see the freshly-loaded scene, and the world stays
         * frozen (world_drive=0 / scene=(none)).
         *
         * sre_Scene_FinishLoad publishes the live Scene* it just finished
         * loading in g_sre_finishloaded_scene. Drive Scene::Update (nm 0x465968)
         * on it directly — Scene::Update owns the per-object component ticks,
         * physics/collision AND the deferred-destruction loop, so this both
         * un-freezes gameplay and lets timed SceneObject destruction run. Guard
         * with the Scene+0x310 frame counter so we never double-step if the
         * normal chain (or the GSC path above) already ticked this scene. */
        if (!s_job1_scene_driven && !g_sre_scene_loading) {
            uint64_t scene_u = g_sre_finishloaded_scene;
            if (scene_u > 0x10000ULL) {
                void* scene = (void*)(uintptr_t)scene_u;
                uint64_t scene_vt = *(uint64_t*)scene;
                if (sre_is_valid_vtable_ptr(scene_vt)) {
                    int frame = *(int*)((char*)scene + 0x310);
                    int already = ((uint64_t)scene == s_job1_last_scene &&
                                   frame != s_job1_last_frame);
                    if (!already) {
                        static int s_fb_logged = -1;
                        if (s_fb_logged < 0)
                            s_fb_logged = getenv("SRE_WORLD_DEBUG") != NULL ? 0 : 2;
                        if (s_fb_logged == 0) {
                            fprintf(stderr,
                                "[SRE/FrameLoop] JOB1 fallback: driving Scene::Update directly "
                                "(scene=%p vt=0x%llx) — NavController transition stuck in "
                                "__do_upcast(0x54c6d4), menu VC still current\n",
                                scene, (unsigned long long)scene_vt);
                            s_fb_logged = 1;
                        }
                        typedef void (*pfn_scene_update)(void*, float);
                        ((pfn_scene_update)(g_swordigo_base + 0x465968))(scene, dt);
                        g_sre_world_drive_count++;
                        s_job1_last_scene = (uint64_t)scene;
                        s_job1_last_frame = *(int*)((char*)scene + 0x310);
                    }
                }
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * sre_CaverShell_Update_trampoline — hook target
 *
 * This is the symbol registered in sre_hook_table (sre_init.c) under
 * "sre_CaverShell_Update". The trampoline installer patches
 * CaverShell::Update at 0x210efc to jump here.
 *
 * The relay cave saves the original first instruction so callers that
 * go through the vtable normally also land here.
 * ────────────────────────────────────────────────────────────────────────── */
void sre_CaverShell_Update_trampoline(void* self, float dt) {
    sre_CaverShell_Update(self, dt);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Scene::Update hook  (sre_Scene_Update)
 *
 * Installed via hook table on Scene::Update at 0x465968.
 *
 * The full Scene::Update does:
 *   1. ProgramState::Update(this+0x28, dt)  ← hooked via sre_ProgramState_Update
 *   2. Camera AABB calculation
 *   3. SceneGrid::UpdateVisibleAreasWithCamera (×2 grids)
 *   4. Pending activation queue flush (adds objects to scene)
 *   5. GetObjectsNearCamera → per-object component updates
 *   6. Deferred deletion loop
 *   7. World bounds update
 *
 * We call-through to the original relay. The only thing we intercept is
 * having our ProgramState::Update hook in the chain for time-slicing.
 * Budget is already set by sre_frame_budget_start() called above.
 * ────────────────────────────────────────────────────────────────────────── */
typedef void (*pfn_orig_Scene_Update)(void* self, float dt);
pfn_orig_Scene_Update g_orig_Scene_Update_fn = NULL;

void sre_Scene_Update(void* self, float dt) {
    if (!self) return;
    if (g_orig_Scene_Update_fn) {
        g_orig_Scene_Update_fn(self, dt);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * sre_GameSceneController_Update — interception hook
 *
 * Installed at GameSceneController::Update (0x349d84).
 *
 * Original calls:
 *   - Hero existence check / AddHeroObjectToScene
 *   - Death-zone / level-up detection → GameEvent::SendEvent
 *   - CameraController::Update (player tracking + pan limits)
 *   - UpdateTarget() (enemy targeting)
 *   - Scene::Update() (physics + ProgramState tree)
 *
 * We only call-through to the original here. Budget checking happens
 * inside sre_ProgramState_Update which is already hooked.
 *
 * Relay pointer is set by main.cpp after trampoline installation.
 * ────────────────────────────────────────────────────────────────────────── */
typedef void (*pfn_orig_GSC_Update)(void* self, float dt);
pfn_orig_GSC_Update g_orig_GameSceneController_Update = NULL;

void sre_GameSceneController_Update(void* self, float dt) {
    if (!self) return;
    if (g_orig_GameSceneController_Update) {
        g_orig_GameSceneController_Update(self, dt);
    }
}
