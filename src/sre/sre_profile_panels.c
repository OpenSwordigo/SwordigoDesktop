/*
 * sre_profile_panels.c — Hero-selection (profile) screen loader mitigation.
 *
 * PROBLEM (evidence: IDA ProfileSelectionView::LoadProfiles 0x2986d8 arm32 /
 * 0x3a1684 arm64, ProfilePanelView::InitWithProfile 0x296270 / 0x39ac98):
 *
 *   ProfileSelectionView::LoadProfiles() runs ONCE, synchronously, on the
 *   main/UI thread from the view constructor and for EVERY save file:
 *     1. ProfileManager::GetAllProfiles()  — opendir + per-.gplayer
 *        FileExistsAtPath + NewByteBufferFromFile + protobuf parse
 *     2. ProfilePanelView::InitWithProfile() — Scene::LoadFromFile("hero")
 *        (full protobuf re-parse, NO scene cache), Scene::FinishLoad,
 *        SceneView, HeroEquipmentManager equip, GUI labels
 *   With many saves this is a single multi-second main-thread stall between
 *   the main menu and the hero-selection screen. Android pays it natively
 *   (small files + model/texture libraries dedupe the heavy assets); it is
 *   still an O(N) synchronous loop — there is NO background loading on
 *   mobile either.
 *
 * FIX (guest-behaviour preserving):
 *   The panel chrome (frame texture, "Set name" button) is built by the
 *   ProfilePanelView CONSTRUCTOR; only the per-save CONTENT (scene, labels)
 *   comes from InitWithProfile. We intercept InitWithProfile and queue it,
 *   then initialize lazily:
 *     - The first PP_EAGER_CAP panels (top of the list = the on-screen
 *       screenful) populate from their own Update ticks immediately.
 *     - The rest initialize on first DrawRect — the engine's
 *       GUIView::DrawSubviewRect only dispatches a child's DrawRect when it
 *       is actually visible (verified 0x30617c, `if (v72>0 && v73>0)`), so
 *       panels build as the user scrolls them into view (true lazy list).
 *     - Safety net: if no panel is ever drawn, everything is forced after
 *       PP_FORCE_FRAMES Update ticks, so nothing can stay blank.
 *
 *   Safety (verified against IDA):
 *     - ProfilePanelView ctor (0x295538) starts with __aeabi_memclr4 → every
 *       field is NULL until InitWithProfile runs.
 *     - ~ProfilePanelView (0x2981ac) null-guards EVERY release → panels
 *       destroyed before their deferred init are safe.
 *     - State markers are keyed by panel address; a destroyed panel stops
 *       receiving Update/DrawRect, and any future panel allocated at the
 *       same address always passes through InitWithProfile again, which
 *       replaces the stale entry before it can ever be dereferenced.
 *
 *   Kill switch: SRE_PROFILE_PANELS_SYNC=1 restores vanilla synchronous
 *   behaviour (timing instrumentation stays active).
 *
 * Hooks (nm arm64 v1.4.12, all verified in arm64_dyn_symbols.txt):
 *   0x3a1684  ProfileSelectionView::LoadProfiles      — timing + panel count
 *   0x39ac98  ProfilePanelView::InitWithProfile       — defer / time
 *   0x39dc7c  ProfilePanelView::Update                — deferred-init driver
 *   0x39e1a4  ProfilePanelView::DrawRect              — skip until initialized
 *   0x39d76c  ProfilePanelView::LayoutSubviews        — skip until initialized
 *   0x39da3c  ProfilePanelView::AnimateIn             — skip until initialized
 *   0x39db60  ProfilePanelView::AnimateOut            — skip until initialized
 *   0x39ac04  ProfilePanelView::ButtonPressed         — swallow early taps
 *
 * RELAYS: g_orig_* pointers are published by main.cpp's hook installer via
 * TrampolineMgr::install_hook(..., g_orig_..., 1) BEFORE the target is
 * patched, so a relay is always visible before the hook can fire. If a relay
 * failed to install (relocation error), the hook logs once and falls back to
 * the least-destructive behaviour for that entry.
 *
 * GENERATED FOR OPENSWORDIGO — hooks the vanilla engine, changes no logic.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* ── clock_gettime (same freestanding pattern as sre_frame_loop.c) ─────── */
struct timespec { long tv_sec; long tv_nsec; };
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
extern int clock_gettime(int clk_id, struct timespec *tp);

static uint64_t pp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ── Relay pointers (published by main.cpp / TrampolineMgr) ────────────── */
uint64_t g_orig_ProfileSelectionView_LoadProfiles = 0;
uint64_t g_orig_ProfilePanelView_InitWithProfile  = 0;
uint64_t g_orig_ProfilePanelView_Update           = 0;
uint64_t g_orig_ProfilePanelView_DrawRect         = 0;
uint64_t g_orig_ProfilePanelView_LayoutSubviews   = 0;
uint64_t g_orig_ProfilePanelView_AnimateIn        = 0;
uint64_t g_orig_ProfilePanelView_AnimateOut       = 0;
uint64_t g_orig_ProfilePanelView_ButtonPressed    = 0;

/* ── Tunables ──────────────────────────────────────────────────────────── */
#define PP_INIT_BUDGET        2      /* panels initialized per budget window  */
#define PP_BUDGET_WINDOW_MS   17     /* ≈1 frame at 60 Hz                     */
#define PP_TABLE_SIZE         1024   /* power of two, open addressing         */

/* ── Panel state table ─────────────────────────────────────────────────── */
enum {
    PP_UNKNOWN = 0,      /* not in table → hooks pass through            */
    PP_QUEUED  = 1,      /* InitWithProfile captured, waiting for budget */
    PP_INITIALIZED = 2   /* original InitWithProfile has run             */
};

/* Tunables for the deferred-init scheduler.
 *   PP_EAGER_CAP       — the first N captured panels (capture order = display
 *                        order, top of the list) are initialized from their
 *                        own Update ticks unconditionally. This guarantees the
 *                        on-screen screenful ALWAYS populates even if the
 *                        engine never draws a panel (safety floor).
 *   PP_FORCE_FRAMES    — if NO ProfilePanelView DrawRect has fired for a long
 *                        time, force-init every queued panel after this many
 *                        Update ticks so nothing can be stuck blank forever.
 *                        When the lazy path works (DrawRect fires → panels,
 *                        scroll-driven), this never triggers. */
#define PP_EAGER_CAP         6       /* ≈ the visible screenful (100pt panels) */
#define PP_FORCE_FRAMES      150     /* ~2.5 s of Updates with zero DrawRects  */

struct PpEntry {
    uint64_t panel;     /* guest ProfilePanelView*                                  */
    uint64_t sp[2];     /* OWNED copy of boost::shared_ptr<PlayerProfile> (ARM64:
                           px @sp[0], sp_counted_base* py @sp[1]). Cloned at capture
                           so the profile survives LoadProfiles()'s vector dtor.   */
    uint16_t  seq;          /* capture order (1..N): top of list = first captured */
    uint16_t  queued_frames;/* Update ticks seen while PP_QUEUED                  */
    uint8_t   state;
};

/* boost::shared_ptr clone / release (ARM64 Itanium ABI).
 *   shared_ptr   = { void *px; sp_counted_base *py; }            (16 bytes)
 *   sp_counted_base layout: vptr @0, long use_count_ @8, long weak_count_ @16
 *                          (64-bit counts — verified in libswordigo.so
 *                           disasm: InitWithProfile 0x39acc4 clone path
 *                           `ldr x10,[py,#8]; add x10,#1; str`, and the
 *                           0x39acf4 release path `blr <vtable+16>=dispose`,
 *                           `ldr x8,[py,#16]; subs x8,#1`, `blr <vtable+24>
 *                           =destroy` only when weak_count_ hit zero).
 *   vtable (sp_counted_impl_p<PlayerProfile> @0x6b90c8, from relocations):
 *   [0]=dtor(complete) [1]=dtor(deleting) [2]=dispose() [3]=destroy()
 * Mirrors boost::detail::sp_counted_base::release() exactly: bump/dec the
 * shared refcount; on last shared ref call dispose(), then weak_release()
 * (dec weak_count_; on last weak ref call destroy()). */
static void pp_sp_clone(uint64_t *dst, const void *src_shared_ptr) {
    const uint64_t *s = (const uint64_t *)src_shared_ptr;
    dst[0] = s[0];   /* px */
    dst[1] = s[1];   /* py */
    void *py = (void *)s[1];
    if (py)
        ++*(volatile uint64_t *)((char *)py + 8);   /* ++use_count_ (64-bit) */
}

/* Counts are plain (non-atomic) loads/stores exactly like the guest engine's
 * own shared_ptr copy path (InitWithProfile 0x39acc4: ldr;add;str) — the guest
 * is single-threaded, so this is safe and avoids the __atomic -> bridge relay
 * (__aarch64_ldadd8_relax UNHANDLED spam in the log). */
static void pp_sp_release(uint64_t *sp) {
    void *py = (void *)sp[1];
    if (!py) return;
    uint64_t *uc = (uint64_t *)((char *)py + 8);
    if (--(*uc) == 0) {
        void **vt = *(void ***)py;
        /* Last shared reference: dispose the managed PlayerProfile. */
        ((void (*)(void *))vt[2])(py);   /* dispose()  */
        /* weak_release(): only the last weak ref frees the control block. */
        uint64_t *wc = (uint64_t *)((char *)py + 16);
        if (--(*wc) == 0)
            ((void (*)(void *))vt[3])(py);   /* destroy()  */
    }
    sp[0] = 0; sp[1] = 0;
}

static struct PpEntry s_table[PP_TABLE_SIZE];
static int            s_sync_mode     = -1;  /* -1 = not yet resolved        */
static int            s_queued_count  = 0;   /* panels captured during the
                                                 current LoadProfiles call    */
static int            s_deferred_done = 0;
static int            s_any_draw_seen = 0;   /* 1 = a panel DrawRect fired —
                                                 lazy path is working          */
static uint64_t       s_window_end_ms = 0;
static int            s_window_budget = 0;

static uint64_t pp_hash(uint64_t v) {
    v ^= v >> 4;                 /* strip low alignment bits */
    v *= 0x9E3779B97F4A7C15ULL;
    return v;
}

/* Find the entry for a panel, or NULL. */
static struct PpEntry *pp_find(uint64_t panel) {
    uint64_t mask = PP_TABLE_SIZE - 1;
    uint64_t i = pp_hash(panel) & mask;
    for (int probes = 0; probes < 16; probes++) {
        if (s_table[i].panel == panel && s_table[i].state != PP_UNKNOWN)
            return &s_table[i];
        if (s_table[i].state == PP_UNKNOWN)
            return 0;            /* empty slot → key never stored here */
        i = (i + 1) & mask;
    }
    return 0;
}

/* Find-or-create the entry for a panel. On a full table, recycle the first
 * PP_INITIALIZED slot (an initialized panel passing through the hooks again
 * behaves exactly like an unknown one — hooks call the original). */
static struct PpEntry *pp_slot(uint64_t panel) {
    uint64_t mask = PP_TABLE_SIZE - 1;
    uint64_t i = pp_hash(panel) & mask;
    struct PpEntry *recyclable = 0;
    for (int probes = 0; probes < 64; probes++) {
        if (s_table[i].state == PP_UNKNOWN || s_table[i].panel == panel)
            return &s_table[i];
        if (!recyclable && s_table[i].state == PP_INITIALIZED)
            recyclable = &s_table[i];
        i = (i + 1) & mask;
    }
    if (recyclable) {
        pp_sp_release(recyclable->sp);   /* drop the owned profile ref before reuse */
        recyclable->state = PP_UNKNOWN;
        return recyclable;
    }
    return 0;
}

/* Release every owned shared_ptr still held by the table. Call on module
 * unload / full reload so we never leak a profile ref (or, if the guest is
 * still live, dangle one). */
static void pp_release_all(void) {
    for (size_t i = 0; i < PP_TABLE_SIZE; i++) {
        if (s_table[i].state != PP_UNKNOWN)
            pp_sp_release(s_table[i].sp);
        s_table[i].state = PP_UNKNOWN;
        s_table[i].panel = 0;
    }
}

static int pp_sync_mode(void) {
    if (s_sync_mode < 0) {
        const char *e = getenv("SRE_PROFILE_PANELS_SYNC");
        s_sync_mode = (e && e[0]) ? 1 : 0;
    }
    return s_sync_mode;
}

/* ── Hook: ProfileSelectionView::LoadProfiles (0x3a1684) ───────────────── */
typedef void (*pfn_pp_load)(void *);

void sre_ProfileSelectionView_LoadProfiles(void *self) {
    uint64_t t0 = pp_now_ms();
    s_queued_count = 0;               /* count only this LoadProfiles pass  */
    s_any_draw_seen = 0;              /* re-arm the lazy-path fallback       */
    pp_release_all();                 /* fresh table + release prior owned refs */

    if (g_orig_ProfileSelectionView_LoadProfiles)
        ((pfn_pp_load)g_orig_ProfileSelectionView_LoadProfiles)(self);

    uint64_t dt = pp_now_ms() - t0;
    fprintf(stderr, "[PERF/Profile] LoadProfiles: %u ms (%d panel(s), mode=%s)\n",
            (unsigned)dt, s_queued_count,
            pp_sync_mode() ? "sync" : "deferred");
}

/* ── Hook: ProfilePanelView::InitWithProfile (0x39ac98) ────────────────── */
typedef void (*pfn_pp_init)(void *, void *);

void sre_ProfilePanelView_InitWithProfile(void *self, void *profile_sp) {
    struct PpEntry *e = pp_slot((uint64_t)self);
    if (!e) {                       /* table exhausted — degrade to sync */
        if (g_orig_ProfilePanelView_InitWithProfile)
            ((pfn_pp_init)g_orig_ProfilePanelView_InitWithProfile)(self, profile_sp);
        return;
    }
    /* Address reuse / re-init: drop any previously owned profile ref before
     * cloning the new one, so we never hold two refs (leak) or keep a stale
     * profile alive past a profile switch. */
    if (e->state != PP_UNKNOWN)
        pp_sp_release(e->sp);
    e->panel      = (uint64_t)self;
    pp_sp_clone(e->sp, profile_sp);      /* own a ref so profile outlives LoadProfiles() */
    s_queued_count++;
    e->seq = (uint16_t)s_queued_count;   /* capture order = display order (sorted list) */
    e->queued_frames = 0;

    if (pp_sync_mode()) {
        uint64_t t0 = pp_now_ms();
        if (g_orig_ProfilePanelView_InitWithProfile)
            ((pfn_pp_init)g_orig_ProfilePanelView_InitWithProfile)(self, e->sp);
        uint64_t dt = pp_now_ms() - t0;
        e->state = PP_INITIALIZED;
        fprintf(stderr, "[PERF/ProfilePanel] InitWithProfile 0x%08x: %u ms\n",
                (unsigned)(uintptr_t)self, (unsigned)dt);
        return;
    }
    e->state = PP_QUEUED;           /* heavy build deferred to Update ticks */
}

/* Run one queued InitWithProfile against the per-window budget.
 * Returns 1 when it ran (entry is PP_INITIALIZED afterwards). */
static int pp_try_deferred_init(struct PpEntry *e) {
    if (!g_orig_ProfilePanelView_InitWithProfile) {
        static int s_warned = 0;
        if (!s_warned) {
            s_warned = 1;
            fprintf(stderr, "[PERF/ProfilePanel] CRITICAL: InitWithProfile relay "
                            "missing — panels cannot initialize\n");
        }
        return 0;
    }
    uint64_t now = pp_now_ms();
    if (now >= s_window_end_ms) {
        s_window_end_ms = now + PP_BUDGET_WINDOW_MS;
        s_window_budget = PP_INIT_BUDGET;
    }
    if (s_window_budget <= 0)
        return 0;
    s_window_budget--;

    /* Mark initialized BEFORE the call: InitWithProfile's own tail dispatches
     * LayoutSubviews through the panel vtable, which must pass through. */
    e->state = PP_INITIALIZED;

    uint64_t t0 = pp_now_ms();
    ((pfn_pp_init)g_orig_ProfilePanelView_InitWithProfile)(
        (void *)e->panel, e->sp);            /* pass our owned shared_ptr copy */
    uint64_t dt = pp_now_ms() - t0;
    s_deferred_done++;
    fprintf(stderr, "[PERF/ProfilePanel] deferred init #%d panel=0x%08x: %u ms\n",
            s_deferred_done, (unsigned)(uintptr_t)e->panel, (unsigned)dt);
    return 1;
}

/* ── Hook: ProfilePanelView::Update (0x39dc7c) — init driver ─────────────
 * Update() fires for EVERY subview every frame (the engine's GUIView::Update
 * walks the whole tree, no culling). It is the reliable heartbeat.
 * Strategy:
 *   - The first PP_EAGER_CAP panels (top of the list = the on-screen screenful)
 *     initialize unconditionally → saves are ALWAYS visible within a few frames,
 *     even if the engine never draws a panel.
 *   - The remaining panels initialize lazily from DrawRect (scroll-driven:
 *     DrawSubviewRect only dispatches a child's DrawRect when it is actually
 *     visible/on-screen). While off-screen they cost nothing here.
 *   - Force-fallback: if NO panel DrawRect has ever fired (s_any_draw_seen==0,
 *     an unexpected deadlock), every queued panel is forced after
 *     PP_FORCE_FRAMES ticks so nothing can stay blank forever. */
typedef void (*pfn_pp_update)(void *, float);

void sre_ProfilePanelView_Update(void *self, float dt) {
    struct PpEntry *e = pp_find((uint64_t)self);
    if (e && e->state == PP_QUEUED) {
        e->queued_frames++;
        if (e->seq <= PP_EAGER_CAP) {
            if (!pp_try_deferred_init(e))
                return;         /* budget exhausted — finish next frame      */
        } else if (!s_any_draw_seen && e->queued_frames >= PP_FORCE_FRAMES) {
            if (!pp_try_deferred_init(e))
                return;
        } else {
            return;             /* off-screen & lazy path alive — skip       */
        }
    }
    if (g_orig_ProfilePanelView_Update)
        ((pfn_pp_update)g_orig_ProfilePanelView_Update)(self, dt);
}

/* ── Content-only hooks ────────────────────────────────────────────────── */
typedef void (*pfn_pp_v)(void *);
typedef void (*pfn_pp_draw)(void *, void *, void *, void *);
typedef void (*pfn_pp_btn)(void *, void *);

void sre_ProfilePanelView_DrawRect(void *self, void *ctx, void *rect, void *mtx) {
    struct PpEntry *e = pp_find((uint64_t)self);
    if (e) {
        s_any_draw_seen = 1;            /* engine is drawing panels — lazy works */
        if (e->state == PP_QUEUED) {
            if (!pp_try_deferred_init(e))
                return;                 /* budget — stays blank this frame       */
        }
    }
    if (g_orig_ProfilePanelView_DrawRect)
        ((pfn_pp_draw)g_orig_ProfilePanelView_DrawRect)(self, ctx, rect, mtx);
}

/* Lifecycle hooks pass through ALWAYS (vanilla). They must NOT be gated on
 * INITIALIZED: the panel needs its normal animation/layout lifecycle to become
 * drawable, otherwise the engine never draws it, DrawRect never fires, and the
 * lazy path deadlocks. Gating DrawRect + ButtonPressed (below) is sufficient —
 * content simply isn't rasterized or tappable until init lands. */
void sre_ProfilePanelView_LayoutSubviews(void *self) {
    if (g_orig_ProfilePanelView_LayoutSubviews)
        ((pfn_pp_v)g_orig_ProfilePanelView_LayoutSubviews)(self);
}

void sre_ProfilePanelView_AnimateIn(void *self) {
    if (g_orig_ProfilePanelView_AnimateIn)
        ((pfn_pp_v)g_orig_ProfilePanelView_AnimateIn)(self);
}

void sre_ProfilePanelView_AnimateOut(void *self) {
    if (g_orig_ProfilePanelView_AnimateOut)
        ((pfn_pp_v)g_orig_ProfilePanelView_AnimateOut)(self);
}

/* Swallow taps on content-less panels (no profile bound yet). */
void sre_ProfilePanelView_ButtonPressed(void *self, void *event) {
    struct PpEntry *e = pp_find((uint64_t)self);
    if (e && e->state != PP_INITIALIZED)
        return;
    if (g_orig_ProfilePanelView_ButtonPressed)
        ((pfn_pp_btn)g_orig_ProfilePanelView_ButtonPressed)(self, event);
}
