/* =============================================================================
 * sre_gui_nav.c — GUINavigationController Safety Hook
 * =============================================================================
 *
 * CRASH ANALYSIS (from register dump and IDA Pro decomp)
 * ──────────────────────────────────────────────────────
 * The crash occurs in GUINavigationController::Update when a stale/freed
 * pending_vc pointer is dispatched to. The PC ends up in .dynstr string data.
 *
 * IDA Pro decomp of Update (address 0x49923C in IDA flat view):
 *   v3 = *((_QWORD *)this + 10);   // this+0x50 = pending_vc raw pointer
 *   if (v3)
 *     (*(void (**)(...)(*(_QWORD *)v3 + 72LL))(v3, a2); // vtable[9] = slot 72
 *   Caver::GUIViewController::Update(this, a2);
 *
 * vtable slot 72 (0x48) = 9th virtual function = the VC's own Update method.
 *
 * STRATEGY
 * ────────
 * Only GUINavigationController::Update needs guarding — it's the only place
 * where a stale pending_vc can cause a raw vtable dispatch. We guard it by:
 *   1. Checking the pending_vc vtable pointer is in the known vtable range
 *   2. If valid: call the vtable slot safely
 *   3. Always call GUIViewController::Update directly (by nm offset)
 *
 * ViewControllerViewLoaded and FinishTransitionToViewController are complex
 * functions (350+ lines in IDA). They operate on LIVE objects passed as
 * arguments (not stale pointers from object fields), so they do NOT need
 * guarding. They are passed through to the original implementation via relay.
 *
 * ADDRESSES (nm -D libswordigo.so v1.4.12 ARM64, base 0x1000000 subtracted):
 *   GUINavigationController::Update                   nm=0x303099  guest=0x1303099
 *   GUINavigationController::ViewControllerViewLoaded nm=0x3030bd  guest=0x13030bd
 *   GUINavigationController::FinishTransitionToVC     nm=0x303681  guest=0x1303681
 *   GUIViewController::Update                         nm=0x307695  guest=0x1307695
 *
 * GUEST HEAP LAYOUT (from live register dump)
 *   Guest objects are allocated in the range 0x20000000 – 0x2FFFFFFF
 *   (e.g. CaverShell DAT=0x20004208, hero=0x205a8380)
 *   This is ABOVE the libswordigo.so load range (0x1000000–0x1800000).
 *
 * VTABLE LAYOUT (from objdump -h of libswordigo.so at load base 0x1000000):
 *   .data.rel.ro: guest VMA 0x16b6a80 – 0x16DC000  (vtable arrays)
 *   .rodata:      guest VMA 0x1583480 – 0x15A2000   (some vtables/typeinfo)
 * =============================================================================
 */

#include "sre.h"
#include "sre_caver.h"
#include <stdio.h>
#include <stdint.h>

/* ─── External: swordigo guest base (= load_addr = 0x1000000) ──────────── */
extern uint64_t g_swordigo_base;

/* ─── Relay stubs set by main.cpp relay-cave builder ────────────────────── */
uint64_t g_orig_GUINavigationController_Update   = 0;
uint64_t g_orig_GUINavigationController_VCLoaded = 0;
uint64_t g_orig_GUINavigationController_Finish   = 0;

/* ─── GUIViewController::Update nm offset ───────────────────────────────── */
/* Verified from nm -D libswordigo.so v1.4.12 arm64-v8a: 0x4a1810 */
#define OFF_GUIViewController_Update  0x4a1810ULL

/* ─── Vtable range guard ────────────────────────────────────────────────── */
/*
 * A valid C++ vtable pointer for libswordigo.so objects lives in .data.rel.ro
 * (0x16b6a80 – 0x16DC000) or .rodata (0x1583480 – 0x15A2000).
 * Corrupt/stale pointers from freed objects will be NULL or in .dynstr / heap.
 *
 * We ONLY use this check in Update's pending_vc guard — nowhere else.
 */
static inline int vtable_is_in_swordigo(uint64_t vt) {
    /* .data.rel.ro vtable section */
    if (vt >= 0x16b6a80ULL && vt < 0x16DC000ULL) return 1;
    /* .rodata (typeinfo/vtables that spill here) */
    if (vt >= 0x1583480ULL && vt < 0x15A2000ULL) return 1;
    return 0;
}

/* ─── Function pointer range guard ─────────────────────────────────────── */
/*
 * A valid ARM64 function pointer in libswordigo.so falls in .text:
 *   VMA 0x1203e90 – 0x1583478
 */
static inline int code_is_in_swordigo(uint64_t fn) {
    return (fn >= 0x1203e90ULL && fn < 0x1584000ULL);
}

/* =============================================================================
 * sre_GUINavigationController_Update
 * =============================================================================
 * COMPLETE REIMPLEMENTATION of GUINavigationController::Update(float).
 *
 * IDA Pro source (address 0x49923C):
 *   v3 = *((QWORD*)this + 10);          // pending_vc at this+0x50
 *   if (v3)
 *     (**(void(**)(...)(*v3 + 72))(v3, dt);  // vtable slot 9 (byte offset 72=0x48)
 *   GUIViewController::Update(this, dt);
 *
 * We guard the vtable dispatch by verifying the pending_vc vtable pointer
 * is within the known .data.rel.ro range before calling through.
 *
 * We do NOT call g_orig_GUINavigationController_Update because the relay cave
 * executes the first instruction of the original function and jumps back to
 * instruction #2 — meaning the relay re-runs the SAME vtable dispatch without
 * our guard. This would cause a double-dispatch and negate our protection.
 *
 * Instead, we call GUIViewController::Update directly by nm offset.
 */
void sre_GUINavigationController_Update(void* self, float dt) {
    if (!self) return;

    /* IDA: v3 = *((QWORD*)this + 10) → byte offset 80 = 0x50 */
    uint64_t pending_vc = *(uint64_t*)((char*)self + 0x50);

    /* NOTE: Previous forced VC swap attempt was REVERTED — writing
     * captured_gvc into nav+0x50 before BackgroundLoad finishes setting
     * up the new VC causes AudioSystem::Update to dispatch through
     * uninitialized vtable slots → .dynstr crash cascade.
     * The VC swap MUST complete through FinishTransitionToViewController. */

    if (pending_vc) {
        /* Read vtable pointer (first 8 bytes of the VC object) */
        uint64_t vtable = *(uint64_t*)(uintptr_t)pending_vc;

        if (vtable_is_in_swordigo(vtable)) {
            /* vtable slot at byte offset 72 (0x48) = the VC's Update virtual method */
            uint64_t fn = *(uint64_t*)(uintptr_t)(vtable + 0x48);
            if (code_is_in_swordigo(fn)) {
                typedef void (*pfn_vc_update)(uint64_t vc, float dt);
                ((pfn_vc_update)(uintptr_t)fn)(pending_vc, dt);
            } else {
                fprintf(stderr, "[SRE/GUINav] Update: pending_vc vtable[0x48]=0x%llx not in .text — blocked\n",
                        (unsigned long long)fn);
            }
        } else {
            fprintf(stderr, "[SRE/GUINav] Update: pending_vc=0x%llx has corrupt vtable=0x%llx — blocked\n",
                    (unsigned long long)pending_vc, (unsigned long long)vtable);
        }
    }

    /* Always call parent GUIViewController::Update directly by known nm offset */
    typedef void (*pfn_base_update)(void* self, float dt);
    pfn_base_update base_fn = (pfn_base_update)(uintptr_t)(g_swordigo_base + OFF_GUIViewController_Update);
    base_fn(self, dt);
}

/* =============================================================================
 * sre_GUINavigationController_VCLoaded
 * =============================================================================
 * Pass-through relay for ViewControllerViewLoaded(GUIViewController*).
 *
 * This function (350+ lines in IDA) is only called with a LIVE ViewController
 * that was just loaded — it is never called with a stale/freed pointer.
 * The dangerous vtable dispatch in Update's pending_vc path is already guarded.
 *
 * We just relay to the original. This hook exists so we can add diagnostics
 * or additional guards in future without re-adding it to the hook table.
 */
void sre_GUINavigationController_VCLoaded(void* self, void* vc) {
    if (g_orig_GUINavigationController_VCLoaded) {
        typedef void (*pfn)(void*, void*);
        ((pfn)(uintptr_t)g_orig_GUINavigationController_VCLoaded)(self, vc);
    }
}

/* =============================================================================
 * sre_GUINavigationController_FinishTransition
 * =============================================================================
 * Pass-through relay for FinishTransitionToViewController(...).
 *
 * Same reasoning as VCLoaded: arguments are live objects from the transition
 * system, not stale pointers. The original implementation is called directly.
 *
 * CAPTURE: this is an early handle on the freshly-loaded GameViewController
 * (arg2 = const shared_ptr<GUIViewController>& — px at offset 0). The frame
 * loop's GUI repair drive reads GVC+0xD8 (shared_ptr<GameSceneView> px, from
 * GameViewController::BackgroundLoad) off the captured VC to dispatch the
 * game view the stuck chain never reaches. Only GameViewControllers are
 * captured (vtable identity) and the value is refreshed on every call, so
 * successive level transitions replace it.
 * 'vtable for Caver::GameViewController' = RVA 0x6cb870 → address point 0x6cb880.
 */
volatile uint64_t g_sre_captured_gvc = 0;

void sre_GUINavigationController_FinishTransition(void* self,
                                                   void* vc_shared_ptr,
                                                   void* userdata,
                                                   void* event) {
    if (vc_shared_ptr) {
        extern uint64_t g_swordigo_base;
        uint64_t vc = *(uint64_t*)vc_shared_ptr;
        if (vc >= 0x10000ULL && vc < 0x0000800000000000ULL &&
            *(uint64_t*)(uintptr_t)vc == g_swordigo_base + 0x6cb880ULL) {
            g_sre_captured_gvc = vc;
        }
    }
    if (g_orig_GUINavigationController_Finish) {
        typedef void (*pfn)(void*, void*, void*, void*);
        ((pfn)(uintptr_t)g_orig_GUINavigationController_Finish)(self, vc_shared_ptr, userdata, event);
    }
}
