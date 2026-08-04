/* =====================================================================
 * sre_effects.c — SRE Visual Effect Hooks
 * =====================================================================
 * Hooks game visual effects (portals, glows, etc.) and reimplements
 * them using our own rendering pipeline, or extracts their data for
 * the host FBO shader pipeline to use.
 *
 * Architecture:
 *   Game calls PortalEffectComponent::Draw() 
 *     → SRE intercepts → reads position/color/intensity from `this`
 *     → Writes to g_sre_effect_* globals
 *     → Host reads globals and applies effect via FBO shaders
 *     → OR: SRE calls engine drawing functions directly (like we do for bg)
 *
 * Hook addresses (ARM64 v1.4.12):
 *   PortalEffectComponent::Draw   = 0x2ae884
 *   SimpleGlowComponent::Draw     = 0x2b0e90
 *   WeaponGlowComponent::Draw     = 0x2cb828
 *   WaterMeshComponent::Draw      = 0x2c2df4
 *   ShadowVolumeComponent::Draw   = 0x236964
 *   ParticleEmitterComponent::Draw = 0x2ab2d8
 *   ParticleFieldComponent::Draw  = 0x2ac790
 *   MagicExplosionComponent::Draw = 0x28849c
 *   WeaponTrailComponent::Draw    = 0x2ccf30
 *   HookshotTrailComponent::Draw  = 0x2835e4
 * =====================================================================
 */

/* ========== Effect Data — exported to host ========== */

/* Portal effect data (host reads for FBO portal glow shader) */
int     g_sre_portal_active = 0;       /* 1 = portal is being drawn this frame */
int     g_sre_portal_count = 0;        /* How many portals drawn this frame */
float   g_sre_portal_x = 0.0f;        /* Portal world X position (from Matrix4 col 3) */
float   g_sre_portal_y = 0.0f;        /* Portal world Y position */
float   g_sre_portal_z = 0.0f;        /* Portal world Z position */
float   g_sre_portal_color_r = 0.5f;  /* Portal color (default purple) */
float   g_sre_portal_color_g = 0.2f;
float   g_sre_portal_color_b = 0.8f;
float   g_sre_portal_color_a = 1.0f;
float   g_sre_portal_scale = 1.0f;    /* Portal visual scale */
/* Full view-projection matrix from the Draw call — 16 floats, column-major.
 * The game passes this matrix to RenderingContext::SetMatrix for rendering.
 * We store it so the host can use it to position the portal quad. */
float   g_sre_portal_vp_matrix[16] = {
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1  /* identity default */
};

/* Glow effect data */
int     g_sre_glow_active = 0;         /* 1 = glow being drawn */
int     g_sre_glow_count = 0;
float   g_sre_glow_x = 0.0f;
float   g_sre_glow_y = 0.0f;
float   g_sre_glow_color_r = 1.0f;
float   g_sre_glow_color_g = 1.0f;
float   g_sre_glow_color_b = 1.0f;
float   g_sre_glow_color_a = 1.0f;

/* Weapon glow data */
int     g_sre_weapon_glow_active = 0;
float   g_sre_weapon_color_r = 0.3f;
float   g_sre_weapon_color_g = 0.6f;
float   g_sre_weapon_color_b = 1.0f;
float   g_sre_weapon_color_a = 1.0f;

/* Effect mode: 0=passthrough (let game draw), 1=extract only, 2=our reimpl */
int     g_sre_effect_mode = 0;

/* Frame reset — host calls this each frame to reset per-frame counters */
void sre_effects_frame_reset(void) {
    g_sre_portal_active = 0;
    g_sre_portal_count = 0;
    g_sre_glow_active = 0;
    g_sre_glow_count = 0;
    g_sre_weapon_glow_active = 0;
    extern int g_sre_weapon_trail_active;
    g_sre_weapon_trail_active = 0;
}

/* ========== Portal Draw Hook ==========
 * Original: PortalEffectComponent::Draw at 0x2ae884
 *
 * The Draw function receives the VIEW-PROJECTION matrix in x2.
 * It then calls SceneObject::WorldMatrix() and multiplies:
 *     combined = VP * WorldMatrix
 * Since we skip the original Draw, we compute this MVP ourselves.
 *
 * SceneObject position is at offset 0x70 in the SceneObject pointed to by
 * Component+0x28. We build a simple translation matrix from it.
 *
 * Args (ARM64):
 *   x0 = this, x1 = RenderingContext*, x2 = Matrix4& (VP), x3 = ?, x4 = ?
 */

void sre_PortalEffectComponent_Draw(void* self, void* ctx, void* matrix_ref, void* vec3_a, void* vec3_b) {
    if (!self) return;
    float* vp = (float*)matrix_ref;  /* VP matrix from x2 */
    char* s = (char*)self;
    
    /* Get SceneObject pointer (Component + 0x28) */
    char* scene_obj = *(char**)(s + 0x28);
    
    /* Read portal world position from SceneObject + 0x70 (Vector3) */
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    if (scene_obj) {
        wx = *(float*)(scene_obj + 0x70);
        wy = *(float*)(scene_obj + 0x74);
        wz = *(float*)(scene_obj + 0x78);
    }
    g_sre_portal_x = wx;
    g_sre_portal_y = wy;
    g_sre_portal_z = wz;
    
    /* Build MV = View * Translation(wx, wy, wz)
     * x2 is the VIEW matrix (confirmed by Scene::Draw decompilation).
     * Caver/OpenGL uses COLUMN-MAJOR layout:
     *   Column 0: [0,1,2,3], Column 1: [4,5,6,7], 
     *   Column 2: [8,9,10,11], Column 3: [12,13,14,15]
     *
     * For MV = V * T where T is pure translation:
     *   MV columns 0,1,2 = V columns 0,1,2 (unchanged)
     *   MV column 3 = V_col0*wx + V_col1*wy + V_col2*wz + V_col3
     */
    int i;
    /* Copy View matrix as base */
    for (i = 0; i < 16; i++) g_sre_portal_vp_matrix[i] = vp[i];
    
    /* Apply world translation (column-major multiply) */
    g_sre_portal_vp_matrix[12] = vp[0]*wx + vp[4]*wy + vp[8]*wz  + vp[12];
    g_sre_portal_vp_matrix[13] = vp[1]*wx + vp[5]*wy + vp[9]*wz  + vp[13];
    g_sre_portal_vp_matrix[14] = vp[2]*wx + vp[6]*wy + vp[10]*wz + vp[14];
    g_sre_portal_vp_matrix[15] = vp[3]*wx + vp[7]*wy + vp[11]*wz + vp[15];
    
    /* Read color from 0xb8 (FloatColor, confirmed) */
    g_sre_portal_color_r = *(float*)(s + 0xb8);
    g_sre_portal_color_g = *(float*)(s + 0xbc);
    g_sre_portal_color_b = *(float*)(s + 0xc0);
    g_sre_portal_color_a = *(float*)(s + 0xc4);
    
    /* Read speed from 0xc8 (Vector3, confirmed) */
    g_sre_portal_scale = *(float*)(s + 0xc8);
    if (g_sre_portal_scale == 0.0f) g_sre_portal_scale = 1.0f;
    
    g_sre_portal_active = 1;
    g_sre_portal_count++;
    
    /* We do NOT call the original Draw — it's broken at bridge level.
     * The FBO pipeline renders our portal effect instead. */
}

/* ========== SimpleGlow Draw Hook ==========
 * Original: SimpleGlowComponent::Draw at 0x2b0e90
 */
void sre_SimpleGlowComponent_Draw(void* self, void* ctx, void* matrix_ref, void* vec3_a, void* vec3_b) {
    if (!self || !matrix_ref) return;
    float* mat = (float*)matrix_ref;
    g_sre_glow_x = mat[12];
    g_sre_glow_y = mat[13];
    
    g_sre_glow_active = 1;
    g_sre_glow_count++;
}

/* ========== WeaponGlow Draw Hook ==========
 * Original: WeaponGlowComponent::Draw at 0x2cb828
 *
 * Reads glow color from component and optionally overrides with 
 * equipped trinket's color from g_sre_trinket_glows[].
 */

/* From sre_mini_api.c — equipped baubles */
extern int g_sre_bauble_count;

/* Trinket glow table (already defined in sre_mini_api.c) */
typedef struct {
    char item_id[32];
    float r, g, b, a;
    float intensity;
} SreTrinketGlowRef;
extern SreTrinketGlowRef g_sre_trinket_glows[16];
extern int g_sre_trinket_glow_count;

void sre_WeaponGlowComponent_Draw(void* self, void* ctx, void* matrix_ref, void* vec3_a, void* vec3_b) {
    if (!self) return;
    char* s = (char*)self;
    
    /* Read default glow color from component (FloatColor at ~0x60) */
    g_sre_weapon_color_r = *(float*)(s + 0x60);
    g_sre_weapon_color_g = *(float*)(s + 0x64);
    g_sre_weapon_color_b = *(float*)(s + 0x68);
    g_sre_weapon_color_a = *(float*)(s + 0x6c);
    
    /* Override with first equipped trinket glow if available */
    if (g_sre_trinket_glow_count > 0) {
        g_sre_weapon_color_r = g_sre_trinket_glows[0].r;
        g_sre_weapon_color_g = g_sre_trinket_glows[0].g;
        g_sre_weapon_color_b = g_sre_trinket_glows[0].b;
        g_sre_weapon_color_a = g_sre_trinket_glows[0].a;
    }
    
    g_sre_weapon_glow_active = 1;
}

/* ========== WeaponTrail Draw Hook ==========
 * Original: WeaponTrailComponent::Draw at 0x2ccf30
 *
 * The weapon trail is a ribbon mesh drawn behind the sword swing.
 * We extract position/color data and export to host for FBO rendering.
 */
int     g_sre_weapon_trail_active = 0;
float   g_sre_weapon_trail_r = 1.0f;
float   g_sre_weapon_trail_g = 1.0f;
float   g_sre_weapon_trail_b = 1.0f;
float   g_sre_weapon_trail_a = 0.8f;

void sre_WeaponTrailComponent_Draw(void* self, void* ctx, void* matrix_ref, void* vec3_a, void* vec3_b) {
    if (!self) return;
    char* s = (char*)self;
    
    /* Read trail color from component (FloatColor at ~0x70) */
    g_sre_weapon_trail_r = *(float*)(s + 0x70);
    g_sre_weapon_trail_g = *(float*)(s + 0x74);
    g_sre_weapon_trail_b = *(float*)(s + 0x78);
    g_sre_weapon_trail_a = *(float*)(s + 0x7c);
    
    /* Apply trinket color override if set */
    if (g_sre_trinket_glow_count > 0) {
        g_sre_weapon_trail_r = g_sre_trinket_glows[0].r;
        g_sre_weapon_trail_g = g_sre_trinket_glows[0].g;
        g_sre_weapon_trail_b = g_sre_trinket_glows[0].b;
        g_sre_weapon_trail_a = g_sre_trinket_glows[0].a * 0.8f;
    }
    
    g_sre_weapon_trail_active = 1;
}

/* ========== MusicPlayer::PlayMusicWithName Hook ==========
 * Original: Caver::MusicPlayer::PlayMusicWithName at 0x4811a0
 * 
 * REMOVED — no-op stub killed all music. The function's STXR loops
 * are now patched inline by the host STXR patcher (main.cpp) which
 * converts LDXR/STXR to plain LDR/STR across the entire .text.
 */

/* ========== __cxa_throw hook ==========
 * Original: __cxa_throw at nm offset 0x51e108
 *
 * C++ exception throw entry point. In our Unicorn emulator, the
 * unwind machinery can't capture guest registers → always fails → abort().
 *
 * Fix: pop the most recent recovery entry from our stack and longjmp to it.
 * Each entry was pushed by sre_lua_call_safe, sre_ProgramState_Execute, etc.
 *
 * When recovery_depth > 0: longjmp to recovery (Lua error handling).
 * When recovery_depth == 0: call ORIGINAL __cxa_throw so the engine's
 * own C++ try/catch blocks can process the exception normally.
 * This is critical for ProgramState::Update which has try/catch around
 * its child iteration — without this, Wastelands crashes.
 */
#include "sre.h"
#include "sre_setjmp.h"

/* Recovery stack — defined in sre_lua.c */
extern sre_recovery_entry g_sre_recovery_stack[];
extern int g_sre_recovery_depth;

/* ========== UnwindRaiseException hook ==========
 * Original: sub_5818C4 at nm offset 0x5818C4 (v1.4.12 arm64-v8a)
 *
 * IDA: _QWORD* __fastcall sub_5818C4(_QWORD *a1, ..., char a9)
 *   sub_58128C(v14, &a9, x30);    // init unwind frame (hooked → zeros it)
 *   memcpy(v15, v14, 0x3C0);
 *   if (a1[2]) sub_581474(a1,v15) else sub_5813B4(a1,v15);
 *   if (v11 != 7) abort();
 *   sub_57FF60(...);  return a1;
 *
 * Problem chain (even after sre_ExceptionFrameInit suppresses the abort in
 * sub_58128C):
 *   1. sub_58128C hook zeros v14 → memcpy gives sub_5813B4 a zeroed frame
 *   2. sub_5813B4 reads personality function ptr from zeroed frame → 0
 *   3. sub_5813B4 computes jump target from zeroed/garbage data
 *   4. BLR lands at 0x16e9ce0 (BlendAnimationComponent::default_instance_+0xce0)
 *      which is raw DATA, not code → undefined instruction → Dynarmic halts
 *   5. Dynarmic force-returns to LR = sre_ExceptionFrameInit → re-enters
 *      hook → infinite loop
 *
 * Root cause: EHABI tables for the guest binary are NOT registered with the
 * host dynamic linker (custom ELF loader, not ld.so), so dl_iterate_phdr
 * can never find them.  The entire unwinding machinery is broken.
 * sre_cxa_throw (0x51e108) already intercepts __cxa_throw and longjmps to
 * a recovery point, so sub_5818C4 should NOT be reached in normal gameplay.
 * It is only reached via the audio landing-pad / sre_cxa_throw returning
 * silently in contexts with no recovery depth.
 *
 * Fix: return a1 (the exception object) immediately — identical to what
 * sub_5818C4 would return on success, but without calling the broken
 * unwinding sub-functions.  This silently drops the unhandled exception,
 * exactly what the previous abort-recovery was doing anyway (just faster).
 *
 * nm -D libswordigo.so v1.4.12 arm64-v8a: static function, not in dynctab.
 * Address cross-checked: LR=0x1581918 saved on stack at crash (instruction
 * after "BL sub_58128C" inside sub_5818C4) and sub_5818C4 confirmed by IDA.
 */
void* sre_UnwindRaiseException(void* a1) {
    /* Return the exception object pointer immediately — same as the normal
     * success path of sub_5818C4 (return a1), but without touching the
     * broken EHABI unwind machinery that causes crash/loop. */
    return a1;
}

/* ========== ExceptionFrameInit hook ==========
 * Original: sub_58128C at nm offset 0x58128C (v1.4.12 arm64-v8a)
 *
 * sub_58128C initializes the unwind control block and calls sub_5806A8 which
 * uses dl_iterate_phdr to look up EHABI tables for the caller's address.
 * Since the guest binary is not registered with the host linker, this always
 * returns 5 → abort().  This hook zeros the frame and returns without
 * aborting.  sub_5818C4 is now hooked directly (sre_UnwindRaiseException),
 * but sub_58128C is also called from sub_581658, sub_5817D0, sub_5819C0, and
 * sub_581ADC — those paths still need to be suppressed here.
 */
void sre_ExceptionFrameInit(uint64_t* a1, uint64_t a2, uint64_t a3) {
    __builtin_memset(a1, 0, 0x3C0);
    a1[104] = (uint64_t)0x4000000000000000ULL;
}


typedef void (*pfn_cxa_throw)(void*, void*, void(*)(void*));
pfn_cxa_throw g_original_cxa_throw = 0;

/* Diagnostic — capture caller address when recovery fails */
unsigned long long g_sre_cxa_throw_caller = 0;  /* LR of whoever called __cxa_throw */
int g_sre_cxa_throw_unrecovered = 0;  /* count of unrecovered throws */

void sre_cxa_throw(void* thrown_exception, void* tinfo, void(*dest)(void*)) {
    const char* type_name = "unknown";
    if (tinfo) {
        struct abi_type_info {
            void* vtable;
            const char* name;
        } *ti = (struct abi_type_info*)tinfo;
        if (ti->name) {
            type_name = ti->name;
        }
    }
    
    fprintf(stderr, "[SRE/cxa_throw] Exception of type '%s' thrown! recovery_depth=%d\n", type_name, g_sre_recovery_depth);
    
    // Log to file for persistent diagnostics
    FILE* fp = fopen("/home/quantumcreeper/.local/share/swordigo-desktop/sre_exceptions.log", "a");
    if (fp) {
        fprintf(fp, "[SRE/cxa_throw] Exception of type '%s' thrown! recovery_depth=%d\n", type_name, g_sre_recovery_depth);
        fclose(fp);
    }

    if (g_sre_recovery_depth > 0) {
        int target = g_sre_recovery_depth - 1;
        
        /* Restore L->errorJmp for this recovery level */
        sre_recovery_entry* entry = &g_sre_recovery_stack[target];
        if (entry->lua_state) {
            void** ejp = (void**)((char*)entry->lua_state + LUA_ERRORJMP_OFFSET);
            *ejp = entry->saved_errorJmp;
        }
        
        /* Invoke exception object destructor before discarding — the ABI requires
         * this; skipping it leaks the small block from __cxa_allocate_exception.
         * Do this BEFORE longjmp since dest may call back into the Lua/C stack. */
        if (dest && thrown_exception) {
            dest(thrown_exception);
        }

        /* Don't decrement depth here — the setjmp handler does it */
        sre_longjmp(entry->buf, 1);
        /* never reaches here */
    }

    /* No recovery point — capture caller info for diagnostics */
    unsigned long long lr;
    asm volatile("mov %0, x30" : "=r"(lr));
    g_sre_cxa_throw_caller = lr;
    g_sre_cxa_throw_unrecovered++;

    /* CRITICAL: Do NOT silently return from __cxa_throw when there is no
     * recovery point.  __cxa_throw is [[noreturn]], so the ARM64 instruction
     * immediately after "BL __cxa_throw" is unreachable dead code.  If we
     * return here, execution falls through into that dead code.  In practice
     * that dead code is often "BL <function_ptr_in_register>" where the
     * register still holds 0 (the empty boost::function's vtable dispatch
     * computes (0 & ~1)+8 = 8).  Dynarmic then attempts to execute guest
     * code at host address 8, which is a null-pointer fault → silent freeze.
     *
     * Instead, call abort().  The SRE abort-recovery mechanism (installed in
     * sre_abort.c) intercepts abort() and unwinds the stack back to the SRE
     * dispatch loop, giving a visible "[SRE/Abort-Recovery]" log line rather
     * than a silent hang.  This is safe: the abort-recovery was specifically
     * designed for exactly this kind of unhandled fatal condition.
     *
     * NOTE: if this fires frequently, it means a C++ exception is being thrown
     * from a code path that has no active Lua pcall frame on the recovery
     * stack.  Fix the root cause (ensure exceptions don't escape non-Lua
     * contexts) rather than suppressing this abort. */
    fprintf(stderr,
            "[SRE/cxa_throw] No recovery point for exception '%s' (caller=0x%llx, "
            "unrecovered=%d) — calling abort() for SRE recovery\n",
            type_name, lr, g_sre_cxa_throw_unrecovered);
    fflush(stderr);
    abort();
}

/* ========== ProgramPanic hook ==========
 * Original: Caver::ProgramPanic at nm offset 0x5c0ab4
 *
 * This is the lua_atpanic handler registered by ProgramState's constructor.
 * Called by luaD_throw when L->errorJmp == NULL (no protected call frame).
 * 
 * Original code:
 *   int* ex = __cxa_allocate_exception(4);
 *   *ex = 0;
 *   __cxa_throw(ex, &int::typeinfo, 0);
 *
 * This throws a C++ int exception → sre_cxa_throw → BRK (crash).
 *
 * Our replacement: if recovery stack is available, use sre_cxa_throw
 * directly (which will longjmp). If not, just return — luaD_throw
 * will call exit(1), which the host can intercept via JNI bridge.
 */
int g_sre_panic_count = 0;  /* diagnostic counter */

int sre_ProgramPanic(void* L) {
    g_sre_panic_count++;
    
    /* If we have a recovery point, trigger it through sre_cxa_throw.
     * sre_cxa_throw will longjmp to the nearest setjmp point. */
    if (g_sre_recovery_depth > 0) {
        /* Trigger recovery — this does NOT return */
        sre_cxa_throw((void*)0, (void*)0, (void(*)(void*))0);
        /* unreachable */
    }

    /* No recovery available. Return 0 — luaD_throw will call exit(1).
     * This is better than crashing via __cxa_throw → BRK. */
    return 0;
}
