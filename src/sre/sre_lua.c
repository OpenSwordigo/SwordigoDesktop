/* sre_lua.c — Lua API function pointers + ProgramState replacements
 *
 * This is the crown jewel of libsre.so Phase 1.
 * 
 * What it does:
 *   1. Stores Lua API function pointers (resolved by host from libswordigo.so)
 *   2. sre_lua_call_safe — REPLACES lua_call with lua_pcall globally
 *   3. Replaces ProgramState::Execute — uses lua_pcall instead of lua_call
 *   4. Replaces ProgramState::Resume — catches Lua errors gracefully
 *   5. Replaces ProgramState::Update — handles timer-based coroutine resumption
 *
 * The key insight: the engine calls lua_call from MANY places, not just
 * ProgramState::Execute. Hooking lua_call itself catches ALL errors globally.
 *
 *   Before: lua_call → Lua error → panic → abort() → return → retry → ∞
 *   After:  sre_lua_call_safe → lua_pcall → error caught → continue!
 *
 * Based on SwMini's panic.c by Ijsd (itsjustsomedude).
 */

#include "lua.h"
#include "lstate.h"
#include "lfunc.h"
#include "ldo.h"
extern int pthread_mutex_lock(void *mutex);
extern int pthread_mutex_unlock(void *mutex);
extern void *g_lua_mutex_ptr;
#define g_lua_mutex (*(char*)g_lua_mutex_ptr)

#include "sre.h"
#include "sre_lua.h"
#include "sre_setjmp.h"
#include "sre_caver.h"

/* clock_gettime forward declarations (freestanding — no <time.h> in sre.h) */
struct timespec { long tv_sec; long tv_nsec; };
#define CLOCK_MONOTONIC 1
extern int clock_gettime(int clk_id, struct timespec *tp);

extern uint64_t g_swordigo_base;


/* ========== Lua API Function Pointers ========== */

pfn_ProgramState_destructor g_orig_ProgramState_destructor = 0;
pfn_lua_pcall       g_lua_pcall = 0;
pfn_lua_resume      g_lua_resume = 0;
pfn_lua_settop      g_lua_settop = 0;
pfn_lua_gettop      g_lua_gettop = 0;
pfn_lua_tolstring   g_lua_tolstring = 0;
pfn_lua_call        g_lua_call = 0;
pfn_lua_pushstring  g_lua_pushstring = 0;
pfn_lua_pushcclosure g_lua_pushcclosure = 0;
pfn_lua_setfield    g_lua_setfield = 0;
pfn_lua_getfield    g_lua_getfield = 0;
pfn_lua_createtable g_lua_createtable = 0;
pfn_lua_pushnumber  g_lua_pushnumber = 0;
pfn_lua_pushboolean g_lua_pushboolean = 0;
pfn_lua_pushnil     g_lua_pushnil = 0;
pfn_lua_tonumber    g_lua_tonumber = 0;
pfn_lua_toboolean   g_lua_toboolean = 0;
pfn_lua_type        g_lua_type = 0;
pfn_luaL_register   g_luaL_register = 0;
pfn_lua_touserdata  g_lua_touserdata = 0;
pfn_lua_topointer   g_lua_topointer = 0;
pfn_lua_pushlightuserdata g_lua_pushlightuserdata = 0;
pfn_lua_error       g_lua_error = 0;

/* Pointer to SceneObject::updateSpeedMultiplier (resolved by host) */
typedef float (*pfn_getSpeedMultiplier)(void* sceneObject);
pfn_getSpeedMultiplier g_getSpeedMultiplier = 0;

/* Destruction-debug counters (read by host for diagnosing the timed-destruction
 * bug: sleep()/wait() Lua coroutine timers must fire for script-driven object
 * removal). Host prints them via [SRE/FrameDiag] when enabled. */
volatile uint64_t g_sre_ps_ticks          = 0;  /* ProgramState::Update entries */
volatile uint64_t g_sre_ps_suspended_seen = 0;  /* states with active sleep timers */
volatile uint64_t g_sre_ps_timer_fires    = 0;  /* sleep timers fired -> lua_resume */
volatile uint64_t g_sre_ps_resume_errors  = 0;  /* lua_resume returned an error */

/* ========== sre_init_lua — called by host to set up function pointers ========== */

/* Struct passed from host with all resolved addresses */
typedef struct {
    sre_u64 lua_pcall;
    sre_u64 lua_resume;
    sre_u64 lua_settop;
    sre_u64 lua_gettop;
    sre_u64 lua_tolstring;
    sre_u64 lua_call;
    sre_u64 lua_pushstring;
    sre_u64 lua_pushcclosure;
    sre_u64 lua_setfield;
    sre_u64 lua_getfield;
    sre_u64 lua_createtable;
    sre_u64 lua_pushnumber;
    sre_u64 lua_pushboolean;
    sre_u64 lua_pushnil;
    sre_u64 lua_tonumber;
    sre_u64 lua_toboolean;
    sre_u64 lua_type;
    sre_u64 luaL_register;
    sre_u64 lua_touserdata;
    sre_u64 lua_pushlightuserdata;
    sre_u64 lua_error;
    sre_u64 lua_topointer;
    sre_u64 getSpeedMultiplier;
} SreLuaAddrs;

extern void sre_mini_ensure_injected(lua_State* L);
extern void sre_mini_ensure_scene_wrapped(lua_State* L);
extern void sre_log_lua_error(const char* source, const char* err_msg);
extern lua_State* g_sre_last_lua_state;

/* Defined below (ProgramState::Execute section) — forward decl for the
 * console-target guards used by sre_lua_resume_safe / servicers. */
static int sre_state_is_valid_game_state(lua_State* L);

/* ---- Live-state registry ---------------------------------------------
 * The game destroys and recreates lua_States during play (scene churn,
 * script cleanup). The console's cached g_sre_last_lua_state is cleared by
 * the ProgramState destructor hook, but nothing re-captured the NEW state
 * afterwards in an idle scene (no Execute/Resume fires) — so subsequent
 * console commands hung with pending=1 forever.
 *
 * Fix: sre_luaL_newstate records every state the engine creates, the
 * destructor evicts destroyed ones, and sre_updateApplication re-locks the
 * console onto the newest live game-ready state when the cache is empty.
 */
#define SRE_STATE_RING_SIZE 24
static lua_State* g_sre_state_ring[SRE_STATE_RING_SIZE] = {0};
static int g_sre_state_ring_head = 0;   /* next write slot */
volatile uint64_t g_sre_state_creations = 0;   /* diag */
volatile uint64_t g_sre_state_evictions = 0;   /* diag */

/* sre_luaL_newstate — wraps the vendored luaL_newstate (which the engine's
 * luaL_newstate is patched to call) to record every newly created state.
 *
 * NOTE: we must NOT call luaL_newstate() by name here — the engine's copy is
 * patched to jump to this wrapper, so a PLT call would recurse forever.
 * Instead the host resolves libsre's OWN vendored luaL_newstate (never
 * patched) and stores it in g_orig_luaL_newstate. */
typedef lua_State* (*pfn_luaL_newstate_orig)(void);
pfn_luaL_newstate_orig g_orig_luaL_newstate = 0;
typedef void (*pfn_lua_close_orig)(lua_State* L);
pfn_lua_close_orig g_orig_lua_close = 0;

lua_State* sre_luaL_newstate(void) {
    lua_State* L = g_orig_luaL_newstate ? g_orig_luaL_newstate() : NULL;
    if (L) {
        g_sre_state_ring[g_sre_state_ring_head] = L;
        g_sre_state_ring_head = (g_sre_state_ring_head + 1) % SRE_STATE_RING_SIZE;
        g_sre_state_creations++;
    }
    return L;
}

/* sre_state_evict — drop a destroyed state from the registry. */
void sre_state_evict(lua_State* L) {
    int i;
    for (i = 0; i < SRE_STATE_RING_SIZE; i++) {
        if (g_sre_state_ring[i] == L) {
            g_sre_state_ring[i] = NULL;
            g_sre_state_evictions++;
        }
    }
}

/* sre_lua_close — wraps the vendored lua_close (which the engine's lua_close
 * is patched to call) to evict the state from the registry when the game
 * closes it directly (outside the ProgramState destructor path). Same
 * recursion-avoidance as sre_luaL_newstate: call through g_orig_lua_close. */
void sre_lua_close(lua_State* L) {
    if (L) sre_state_evict(L);
    if (g_orig_lua_close) g_orig_lua_close(L);
}

/* sre_find_live_game_state — newest first scan of the registry for a state
 * that is still alive and exposes the game API. Used by the console servicer
 * to re-lock after the previous target was destroyed. */
static lua_State* sre_find_live_game_state(void) {
    int i;
    for (i = 1; i <= SRE_STATE_RING_SIZE; i++) {
        int idx = (g_sre_state_ring_head - i + SRE_STATE_RING_SIZE) % SRE_STATE_RING_SIZE;
        lua_State* L = g_sre_state_ring[idx];
        if (L && sre_state_is_valid_game_state(L))
            return L;
    }
    return NULL;
}

void sre_init_lua(SreLuaAddrs* addrs) {
    g_lua_pcall       = (pfn_lua_pcall)addrs->lua_pcall;
    g_lua_resume      = (pfn_lua_resume)addrs->lua_resume;
    g_lua_settop      = (pfn_lua_settop)addrs->lua_settop;
    g_lua_gettop      = (pfn_lua_gettop)addrs->lua_gettop;
    g_lua_tolstring   = (pfn_lua_tolstring)addrs->lua_tolstring;
    g_lua_call        = (pfn_lua_call)addrs->lua_call;
    g_lua_pushstring  = (pfn_lua_pushstring)addrs->lua_pushstring;
    g_lua_pushcclosure = (pfn_lua_pushcclosure)addrs->lua_pushcclosure;
    g_lua_setfield    = (pfn_lua_setfield)addrs->lua_setfield;
    g_lua_getfield    = (pfn_lua_getfield)addrs->lua_getfield;
    g_lua_createtable = (pfn_lua_createtable)addrs->lua_createtable;
    g_lua_pushnumber  = (pfn_lua_pushnumber)addrs->lua_pushnumber;
    g_lua_pushboolean = (pfn_lua_pushboolean)addrs->lua_pushboolean;
    g_lua_pushnil     = (pfn_lua_pushnil)addrs->lua_pushnil;
    g_lua_tonumber    = (pfn_lua_tonumber)addrs->lua_tonumber;
    g_lua_toboolean   = (pfn_lua_toboolean)addrs->lua_toboolean;
    g_lua_type        = (pfn_lua_type)addrs->lua_type;
    g_luaL_register   = (pfn_luaL_register)addrs->luaL_register;
    g_lua_touserdata  = (pfn_lua_touserdata)addrs->lua_touserdata;
    g_lua_topointer   = (pfn_lua_topointer)addrs->lua_topointer;
    g_lua_pushlightuserdata = (pfn_lua_pushlightuserdata)addrs->lua_pushlightuserdata;
    g_lua_error       = (pfn_lua_error)addrs->lua_error;
    if (!g_lua_error && g_swordigo_base) {
        g_lua_error = (pfn_lua_error)(g_swordigo_base + 0x4e9d44);
    }
    g_getSpeedMultiplier = (pfn_getSpeedMultiplier)addrs->getSpeedMultiplier;

    /* Diagnostic: log that libsre initialized Lua function pointers */
    sre_log_lua_error("sre_init_lua", "sre_init_lua called — function pointers installed");

    /* If we already have a lua_State captured, attempt an early injection */
    /* Disabled early injection — can cause native init ordering issues. */
    /* if (g_sre_last_lua_state) {
        sre_log_lua_error("sre_init_lua", "Attempting early injection for existing lua_State");
        sre_mini_ensure_injected(g_sre_last_lua_state);
    } */
}

/* ========== sre_lua_call_safe — GLOBAL lua_call replacement ==========
 *
 * The trampoline replaces lua_call's first instructions with a branch
 * to this function. Every single lua_call in the engine now goes through
 * pcall protection.
 *
 * Before: lua_call(L, nargs, nresults) → error → panic → abort()
 * After:  sre_lua_call_safe(L, nargs, nresults) → pcall → error caught!
 *
 * CRITICAL: We also set up a setjmp recovery point. If pcall internally
 * triggers __cxa_throw (because the Android Lua uses C++ exceptions for
 * error handling), our sre_cxa_throw hook will longjmp back here instead
 * of attempting the broken unwind → abort path.
 *
 * Note: We can't call g_lua_call here — that's OUR OWN address (trampoline
 * destroyed the original). We MUST use g_lua_pcall which is a different
 * function at a different address.
 */
#include "sre_setjmp.h"

static int g_lua_call_safe_errors = 0;

/* Lua error diagnostics — host can read these */
volatile char g_sre_last_lua_error[256] = {0};
volatile int g_sre_lua_error_count = 0;

extern char g_sre_vfs_path_external[512];

static int sre_itoa(int val, char* buf) {
    if (val < 0) { buf[0] = '-'; return 1 + sre_itoa(-val, buf + 1); }
    if (val < 10) { buf[0] = '0' + val; return 1; }
    int len = sre_itoa(val / 10, buf);
    buf[len] = '0' + (val % 10);
    return len + 1;
}

void sre_log_lua_error(const char* source, const char* err_msg) {
    fprintf(stderr, "[SRE-LUA-ERROR] Source: '%s' | Error: %s\n", source ? source : "unknown", err_msg ? err_msg : "none");
    static int log_counter = 0;
    log_counter++;
    
    /* Build log path: <external_dir>/sre_lua_errors.log */
    char log_path[512];
    char line[1024];
    int i = 0, j;
    
    if (!g_sre_vfs_path_external[0]) return;
    
    for (j = 0; i < 480 && g_sre_vfs_path_external[j]; j++)
        log_path[i++] = g_sre_vfs_path_external[j];
    if (i > 0 && log_path[i-1] != '/')
        log_path[i++] = '/';
    const char* fname = "sre_lua_errors.log";
    for (j = 0; i < 510 && fname[j]; j++)
        log_path[i++] = fname[j];
    log_path[i] = '\0';
    
    /* Build: "[source] #N: msg\n" */
    i = 0;
    line[i++] = '[';
    for (j = 0; i < 490 && source[j]; j++)
        line[i++] = source[j];
    line[i++] = ']'; line[i++] = ' '; line[i++] = '#';
    i += sre_itoa(log_counter, line + i);
    line[i++] = ':'; line[i++] = ' ';
    if (err_msg) {
        for (j = 0; i < 900 && err_msg[j]; j++)
            line[i++] = err_msg[j];
    }
    /* Append state pointer for correlation if available */
    line[i++] = ' ';
    line[i++] = '{';
    line[i++] = 's'; line[i++] = 't'; line[i++] = 'a'; line[i++] = 't'; line[i++] = 'e'; line[i++] = '=';
    /* hex pointer */
    unsigned long val = (unsigned long)g_sre_last_lua_state;
    char hex[32]; int hx = 0; hex[hx++] = '0'; hex[hx++] = 'x';
    int started = 0;
    for (int shift = (int)(sizeof(val)*8 - 4); shift >= 0 && hx < (int)sizeof(hex)-1; shift -= 4) {
        int nib = (int)((val >> shift) & 0xF);
        if (nib || started || shift == 0) { started = 1; char c = (nib < 10) ? ('0' + nib) : ('a' + (nib - 10)); hex[hx++] = c; }
    }
    hex[hx] = '\0';
    for (j = 0; j < hx && i < 1018; j++) line[i++] = hex[j];
    line[i++] = '}';
    line[i++] = '\n';
    line[i] = '\0';
    
    FILE* fp = fopen(log_path, "a");
    if (fp) {
        fwrite(line, 1, i, fp);
        fclose(fp);
    }
}

/* Recovery stack — shared with sre_cxa_throw (sre_effects.c) */
sre_recovery_entry g_sre_recovery_stack[SRE_MAX_RECOVERY];
int g_sre_recovery_depth = 0;
const unsigned int g_sre_recovery_stack_bytes = sizeof(g_sre_recovery_stack);

/* Push a recovery entry. Returns the depth index, or -1 if stack full.
 * NOT static: sre_scene_update.c and other SRE compilation units call this
 * via extern declaration. Making it static caused it to go through the PLT
 * bridge stub → [Bridge64] !! UNHANDLED → return 0 in X0 → recovery stack
 * grew unbounded across scene transitions → 4.7M bridge calls per load. */
int recovery_push(lua_State* L) {
    if (g_sre_recovery_depth >= SRE_MAX_RECOVERY) return -1;
    int idx = g_sre_recovery_depth;
    sre_recovery_entry* e = &g_sre_recovery_stack[idx];
    e->lua_state = L;
    if (L) {
        void** ejp = (void**)((char*)L + LUA_ERRORJMP_OFFSET);
        e->saved_errorJmp = *ejp;
    } else {
        e->saved_errorJmp = 0;
    }
    g_sre_recovery_depth++;
    return idx;
}

/* Pop recovery stack back to a given depth.
 * NOT static: see note on recovery_push above. */
void recovery_pop(int depth) {
    g_sre_recovery_depth = depth;
}

/* ========== luaD_throw hook ==========
 * Original: luaD_throw at nm offset 0x4eb814
 * 
 * This is the ROOT of ALL Lua error handling. Every Lua error goes through
 * luaD_throw(L, errcode). The original has two paths:
 * 
 * Path A (L->errorJmp != NULL):
 *   errorJmp->status = errcode;
 *   __cxa_throw(lua_longjmp*, typeinfo, 0);
 *   // Caught by lua_resume's try/catch in native builds
 *   // But __cxa_throw crashes in Unicorn (no C++ unwinding)
 *
 * Path B (L->errorJmp == NULL):
 *   L->status = errcode;
 *   ProgramPanic(L);   // throws __cxa_throw(int) — crashes
 *   exit(1);           // kills host
 *
 * Our replacement: use the SRE recovery stack (setjmp/longjmp) instead
 * of C++ exceptions. This handles ALL Lua errors safely.
 */
void sre_luaD_throw(lua_State* L, int errcode) {
    /* Check if there's a Lua error handler (errorJmp) */
    void** ejp = (void**)((char*)L + LUA_ERRORJMP_OFFSET);
    void* errorJmp = *ejp;
    
    if (errorJmp) {
        /* Path A: errorJmp exists — set status field.
         * errorJmp is a struct { status at offset +12 (int) }
         * From disasm: str w20, [x8, #12] where x8 = errorJmp */
        *(int*)((char*)errorJmp + 12) = errcode;
    } else {
        /* Path B: no errorJmp — set L->status directly.
         * From disasm: strb w20, [x19, #10] where x19 = L */
        *((char*)L + 10) = (char)errcode;
    }
    
    /* Use SRE recovery stack to longjmp back to the nearest safe point
     * (sre_lua_resume_safe, sre_ProgramState_Update, etc.) */
    if (g_sre_recovery_depth > 0) {
        int target = g_sre_recovery_depth - 1;
        sre_recovery_entry* entry = &g_sre_recovery_stack[target];
        
        /* Restore saved errorJmp for this recovery level */
        if (entry->lua_state) {
            void** saved_ejp = (void**)((char*)entry->lua_state + LUA_ERRORJMP_OFFSET);
            *saved_ejp = entry->saved_errorJmp;
        }
        
        sre_longjmp(entry->buf, 1);
        /* never reaches here */
    }
    
    /* No recovery point — just return. The calling code (lua_resume,
     * lua_pcall, etc.) will see the error status and handle it.
     * This is imperfect but prevents the crash. */
}

void sre_lua_call_safe(lua_State* L, int nargs, int nresults) {
    if (!g_lua_pcall) {
        return;
    }

    pthread_mutex_lock(&g_lua_mutex);

    /* Lazy Mini.* injection — injects on first call per lua_State */
    extern void sre_mini_ensure_injected(lua_State* L);
    sre_mini_ensure_injected(L);
    
    /* Save Lua VM state for recovery */
    CallInfo* saved_ci = L->ci;
    StkId saved_top = L->top;
    StkId saved_base = L->base;
    unsigned short saved_nCcalls = L->nCcalls;
    
    /* Push recovery entry */
    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        /* Stack full — fall through to raw pcall without recovery */
        int result = g_lua_pcall(L, nargs, nresults, 0);
        if (result != 0 && g_lua_settop) g_lua_settop(L, -2);
        pthread_mutex_unlock(&g_lua_mutex);
        return;
    }
    
    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        /* Caught C++ exception via longjmp from sre_cxa_throw!
         * errorJmp was already restored by sre_cxa_throw. */
        recovery_pop(my_depth);
        pthread_mutex_unlock(&g_lua_mutex);
        g_lua_call_safe_errors++;
        g_sre_lua_error_count++;
        
        /* ─── Lua VM State Recovery ─────────────────────────────────── */
        luaF_close(L, saved_top);
        L->ci = saved_ci;
        L->top = saved_top;
        L->base = saved_base;
        L->nCcalls = saved_nCcalls;
        
        /* Pop function and arguments */
        StkId new_top = saved_top - (nargs + 1);
        if (new_top >= L->stack) {
            L->top = new_top;
        }
        
        /* Push nils for expected return values */
        if (nresults > 0 && nresults != -1 && g_lua_pushnil) {
            int i;
            for (i = 0; i < nresults; i++) {
                g_lua_pushnil(L);
            }
        }
        return;
    }
    
    int result = g_lua_pcall(L, nargs, nresults, 0);
    recovery_pop(my_depth);
    
    if (result != 0) {
        g_lua_call_safe_errors++;
        /* Log the error message before discarding */
        if (g_lua_tolstring) {
            unsigned long dummy;
            const char* err = g_lua_tolstring(L, -1, &dummy);
            if (err) {
                /* Copy to visible error buffer for host diagnostic */
                int i;
                for (i = 0; i < 254 && err[i]; i++)
                    g_sre_last_lua_error[i] = err[i];
                g_sre_last_lua_error[i] = '\0';
                g_sre_lua_error_count++;
                sre_log_lua_error("lua_call", err);
            }
        }
        if (g_lua_settop) {
            int target_top = (int)(saved_top - L->base) - (nargs + 1);
            if (target_top >= 0) g_lua_settop(L, target_top);
            else g_lua_settop(L, 0);
        }
        if (nresults != -1 && g_lua_pushnil) {
            int i;
            for (i = 0; i < nresults; i++) {
                g_lua_pushnil(L);
            }
        }
    }
    pthread_mutex_unlock(&g_lua_mutex);
}

/* ========== sre_lua_resume_safe — PROTECTED lua_resume wrapper ==========
 *
 * Unlike sre_lua_call_safe (which replaces lua_call entirely with pcall),
 * this function wraps the ORIGINAL lua_resume with setjmp recovery.
 *
 * The host patches the lua_resume function entry through TrampolineMgr.
 * g_lua_resume is changed to the pre-patch relay, so this wrapper can call
 * the original implementation without recursion.
 *
 * This catches Lua errors from:
 *   - ProgramState::Update (timer-based coroutine resumption) — THE WASTELANDS FIX
 *   - ProgramState::ExecuteString (console/debug)
 *   - coroutine.resume helper (FUN_005fb6b4)
 *   - Any other engine code that calls lua_resume
 *
 * ProgramState::Execute and ::Resume are already hooked with their own
 * recovery, but those go through g_lua_resume (function pointer), not
 * BL, so there's no double-wrapping.
 */
static int g_lua_resume_safe_errors = 0;

/* Shared error state — host can read these via symbol lookup */
int g_sre_resume_err_count = 0;
char g_sre_resume_last_err[256] = {0};

static void capture_lua_error(lua_State* L) {
    if (!g_lua_tolstring) return;
    size_t len = 0;
    const char* err = g_lua_tolstring(L, -1, &len);
    if (err) {
        int i;
        for (i = 0; err[i] && i < 255; i++)
            g_sre_resume_last_err[i] = err[i];
        g_sre_resume_last_err[i] = 0;
        sre_log_lua_error("lua_resume", err);
    }
}

static void sre_lua_timeout_hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
    if (g_lua_pushstring && g_lua_error) {
        g_lua_pushstring(L, "[SRE] Runaway script execution limit reached (infinite loop prevented)");
        g_lua_error(L);
    }
}

/* ─── Guest-fault recovery flag ───────────────────────────────────────────
 * The host (main.cpp) sets this to 1 immediately after clearing a Dynarmic
 * guest fault (g_emulator_64->clear_faulted()). sre_lua_resume_safe checks
 * it on the next coroutine resume: when set, it resets the Lua VM state to
 * a safe baseline and returns LUA_ERRRUN instead of trying to resume through
 * the corrupted CallInfo stack left by the bad lua_CFunction dereference.
 *
 * Host writes via symbol:
 *   uint64_t addr = get_symbol_vaddr(&g_sre_mod, "g_sre_lua_vm_faulted");
 *   *(volatile int*)(g_guest_memory + addr) = 1;
 * ─────────────────────────────────────────────────────────────────────────── */
volatile int g_sre_lua_vm_faulted = 0;

/* Emergency reset: wind L back to a safe baseline for Lua 5.1.
 * Lua 5.1 uses a flat CallInfo array (base_ci..end_ci), not a linked list.
 * We reset ci to base_ci (the outermost frame) and clear the value stack.
 * Does NOT call luaF_close/GC — avoids double-free on corrupted upvalues.
 * Called immediately AFTER a guest-fault clear before the next lua_resume. */
static void sre_lua_emergency_reset(lua_State* L) {
    if (!L) return;
    /* Reset to the outermost call frame (base_ci = &base_ci[0]) */
    if (L->base_ci) {
        L->ci = L->base_ci;
    }
    /* Reset value stack to just the base slot — discard all pending values
     * without invoking GC or __gc metamethods. */
    if (L->stack) {
        L->top  = L->stack + 1;  /* leave room for one sentinel slot */
        L->base = L->stack + 1;
    }
    L->nCcalls = 0;
    L->status  = 0;  /* LUA_OK — safe to resume again (or mark completed) */
    fprintf(stderr, "[SRE/LuaFault] Emergency Lua 5.1 VM reset on state %p\n", (void*)L);
}

int sre_lua_resume_safe(lua_State* L, int narg) {
    if (!g_lua_resume) {
        /* Should never happen — lua_resume not resolved */
        return 2;  /* LUA_ERRRUN */
    }

    /* ─── Guest-fault recovery ──────────────────────────────────────────────
     * If the host signalled that a Dynarmic guest fault just occurred (likely
     * a bad lua_CFunction pointer was called), perform an emergency reset on
     * this coroutine BEFORE acquiring the mutex. This clears corrupted CallInfo
     * frames so the next resume starts from a sane baseline rather than trying
     * to continue through corrupted state → bridge SIGSEGV. */
    if (g_sre_lua_vm_faulted) {
        g_sre_lua_vm_faulted = 0;
        if (L) sre_lua_emergency_reset(L);
        /* Clear the per-frame Lua state pointer so that sre_mini_zwalk_poll
         * and other per-frame callers don't attempt to pcall into the freshly
         * reset (empty) state on the very next frame. The mainthread probe in
         * the block below will re-populate g_sre_last_lua_state on the next
         * successful lua_resume. */
        extern lua_State* g_sre_last_lua_state;
        if (g_sre_last_lua_state == L) g_sre_last_lua_state = NULL;
        /* Return LUA_ERRRUN — the caller (ProgramState::Update) will mark
         * the coroutine as completed and not resume it again. */
        return 2;  /* LUA_ERRRUN */
    }


    pthread_mutex_lock(&g_lua_mutex);

    /* Console target refresh: coroutine resumes happen every frame during
     * gameplay, so this keeps a live game-ready MAIN state available even
     * after scene transitions (where Execute may not fire again). Only run
     * the (cheap-ish) probe when we currently lack a valid target. The
     * resumed L is a thread; its global_State.mainthread is the owner. */
    if (!g_sre_last_lua_state && L) {
        lua_State* mainL = NULL;
        if (((lu_byte*)L)[8] == LUA_TTHREAD) {
            uint64_t lg = (uint64_t)*(void**)((char*)L + 32);
            if (lg >= 0x1000000ULL && lg < 0x30000000ULL)
                mainL = *(lua_State**)((char*)lg + 176);  /* g->mainthread */
        }
        if (mainL && sre_state_is_valid_game_state(mainL))
            g_sre_last_lua_state = mainL;
    }

    /* Refresh the Scene/Find/New wrap freshness BEFORE resuming. Mod scripts
     * (Thronfield's hiro.scl) run their main loop as resumed coroutines — the
     * lua_call-triggered re-wrap in sre_lua_call_safe never fires for them.
     * If the engine swapped the global Scene table during a level transition
     * (no lua_call in between), objects they create are unhooked and
     * obj:destroy() is nil. This is a cheap C-only marker check; it only
     * evals when the wrap marker is actually missing. */
    sre_mini_ensure_scene_wrapped(L);

    /* Save Lua VM state for recovery */
    CallInfo* saved_ci = L->ci;
    StkId saved_top = L->top;
    StkId saved_base = L->base;
    unsigned short saved_nCcalls = L->nCcalls;

    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        /* Recovery stack full — call without protection (fallback) */
        if (g_lua_sethook) g_lua_sethook(L, (lua_Hook)sre_lua_timeout_hook, LUA_MASKCOUNT, 100000);
        int result = g_lua_resume(L, narg);
        if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
        pthread_mutex_unlock(&g_lua_mutex);
        return result;
    }

    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        /* Caught C++ exception via longjmp from sre_cxa_throw!
         * The coroutine threw a Lua error during resume.
         * errorJmp was already restored by sre_cxa_throw. */
        recovery_pop(my_depth);
        if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
        pthread_mutex_unlock(&g_lua_mutex);
        g_lua_resume_safe_errors++;
        g_sre_resume_err_count++;
        /* Try to get the error message from the Lua stack */
        capture_lua_error(L);

        /* ─── Lua VM State Recovery ─────────────────────────────────── */
        luaF_close(L, saved_top);
        L->ci = saved_ci;
        L->top = saved_top;
        L->base = saved_base;
        L->nCcalls = saved_nCcalls;

        return 2;  /* LUA_ERRRUN */
    }

    if (g_lua_sethook) g_lua_sethook(L, (lua_Hook)sre_lua_timeout_hook, LUA_MASKCOUNT, 100000);
    int result = g_lua_resume(L, narg);
    if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
    recovery_pop(my_depth);

    /* Also capture errors from normal lua_resume return (non-exception path) */
    if (result >= 2) {
        g_sre_resume_err_count++;
        capture_lua_error(L);
    }

    pthread_mutex_unlock(&g_lua_mutex);
    return result;
}

/* ========== Lua Console — Remastered Backend ==========
 * 
 * Protocol (host ↔ SRE guest via shared guest memory):
 *   g_lua_console_buf      — host writes Lua source here (up to 4095 chars)
 *   g_lua_console_pending  — host sets to 1; SRE clears after exec
 *   g_lua_console_status   — 0=idle 1=ok 2=error (set by SRE)
 *   g_lua_console_result   — SRE writes result/error string here
 *   g_lua_console_print_buf— captures print() calls during exec
 *
 * Extras vs old version:
 *   • Bare expressions auto-wrapped as "return <expr>" for REPL feel
 *   • print() output captured into result (newline-joined)
 *   • Multiple return values joined with "\t"
 *   • Host can poll g_lua_console_status each frame for async results
 */
#define CONSOLE_BUF_SIZE   4096
#define CONSOLE_PRINT_SIZE 8192

char g_lua_console_buf[CONSOLE_BUF_SIZE];        /* input: Lua source */
char g_lua_console_result[CONSOLE_BUF_SIZE];     /* output: result or error */
char g_lua_console_print_buf[CONSOLE_PRINT_SIZE];/* captured print() output */
int  g_lua_console_pending  = 0;  /* host sets 1 to submit */
int  g_lua_console_status   = 0;  /* 0=idle 1=ok 2=error */

/* Last captured lua_State — for host inspection */
lua_State* g_sre_last_lua_state = 0;

/* ---- print() override ----
 * Replaces the Lua "print" global while the console runs.
 * Appends all arguments (tostring'd, tab-separated) + newline
 * into g_lua_console_print_buf. */
static int sre_console_print(lua_State* L) {
    int n = g_lua_gettop(L);
    int pos = 0;
    /* find current end of print buf */
    while (pos < CONSOLE_PRINT_SIZE - 1 && g_lua_console_print_buf[pos]) pos++;
    for (int i = 1; i <= n; i++) {
        if (i > 1 && pos < CONSOLE_PRINT_SIZE - 2) g_lua_console_print_buf[pos++] = '\t';
        size_t len = 0;
        const char* s = g_lua_tolstring(L, i, &len);
        if (!s) { s = "nil"; len = 3; }
        for (size_t j = 0; j < len && pos < CONSOLE_PRINT_SIZE - 2; j++)
            g_lua_console_print_buf[pos++] = s[j];
    }
    if (pos < CONSOLE_PRINT_SIZE - 1) g_lua_console_print_buf[pos++] = '\n';
    g_lua_console_print_buf[pos] = 0;
    return 0;
}

/* ---- str_copy helper ---- */
static int sre_strcopy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

#define SRE_SERIALIZE_MAX_DEPTH 10
#define SRE_SERIALIZE_MAX_VISITED 32

static void sre_append_str(char* buf, int* pos, int max, const char* str) {
    if (!str) return;
    int p = *pos;
    while (*str && p < max - 1) {
        buf[p++] = *str++;
    }
    buf[p] = '\0';
    *pos = p;
}

static void sre_append_indent(char* buf, int* pos, int max, int indent) {
    int p = *pos;
    for (int i = 0; i < indent && p < max - 1; i++) {
        buf[p++] = ' ';
        if (p < max - 1) buf[p++] = ' ';
    }
    buf[p] = '\0';
    *pos = p;
}

static void sre_escape_string(char* buf, int* pos, int max, const char* str, size_t len) {
    sre_append_str(buf, pos, max, "\"");
    int p = *pos;
    for (size_t i = 0; i < len && p < max - 2; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == '"') {
            buf[p++] = '\\'; buf[p++] = '"';
        } else if (c == '\\') {
            buf[p++] = '\\'; buf[p++] = '\\';
        } else if (c == '\n') {
            buf[p++] = '\\'; buf[p++] = 'n';
        } else if (c == '\r') {
            buf[p++] = '\\'; buf[p++] = 'r';
        } else if (c == '\t') {
            buf[p++] = '\\'; buf[p++] = 't';
        } else if (c < 32 || c >= 127) {
            char hex[8];
            int hlen = snprintf(hex, sizeof(hex), "\\x%02X", c);
            for (int k = 0; k < hlen && p < max - 1; k++) buf[p++] = hex[k];
        } else {
            buf[p++] = c;
        }
    }
    buf[p] = '\0';
    *pos = p;
    sre_append_str(buf, pos, max, "\"");
}

static void sre_format_number(double num, char* out, size_t out_size) {
    int64_t inum = (int64_t)num;
    if ((double)inum == num) {
        if (inum == 0) {
            if (out_size > 1) { out[0] = '0'; out[1] = '\0'; }
            return;
        }
        char temp[32];
        int tpos = 0;
        uint64_t abs_val = (inum < 0) ? (uint64_t)(-inum) : (uint64_t)inum;
        while (abs_val > 0) {
            temp[tpos++] = '0' + (char)(abs_val % 10);
            abs_val /= 10;
        }
        if (inum < 0) temp[tpos++] = '-';
        
        size_t opos = 0;
        while (tpos > 0 && opos < out_size - 1) {
            out[opos++] = temp[--tpos];
        }
        out[opos] = '\0';
    } else {
        snprintf(out, out_size, "%.14g", num);
    }
}

static void sre_format_pointer(const char* prefix, const void* ptr, char* out, size_t out_size) {
    uintptr_t val = (uintptr_t)ptr;
    if (val == 0) {
        snprintf(out, out_size, "%s: 0x0>", prefix);
        return;
    }
    char hex[20];
    int hpos = 0;
    while (val > 0) {
        int d = (int)(val & 0xF);
        hex[hpos++] = (d < 10) ? ('0' + d) : ('a' + (d - 10));
        val >>= 4;
    }
    size_t pos = 0;
    while (*prefix && pos < out_size - 1) out[pos++] = *prefix++;
    if (pos < out_size - 4) {
        out[pos++] = ':'; out[pos++] = ' '; out[pos++] = '0'; out[pos++] = 'x';
    }
    while (hpos > 0 && pos < out_size - 2) {
        out[pos++] = hex[--hpos];
    }
    if (pos < out_size - 1) out[pos++] = '>';
    out[pos] = '\0';
}

static void sre_serialize_value_recursive(lua_State* L, int idx, char* buf, int* pos, int max, int depth, const void** visited, int visited_count) {
    if (depth > SRE_SERIALIZE_MAX_DEPTH) {
        sre_append_str(buf, pos, max, "{...}");
        return;
    }

    int top_before = g_lua_gettop(L);
    int abs_idx = (idx < 0 && idx > LUA_GLOBALSINDEX) ? (top_before + idx + 1) : idx;
    if (abs_idx < 1 || abs_idx > top_before) {
        sre_append_str(buf, pos, max, "nil");
        return;
    }

    int type = g_lua_type ? g_lua_type(L, abs_idx) : 0;

    switch (type) {
        case 0: /* LUA_TNIL */
            sre_append_str(buf, pos, max, "nil");
            break;
        case 1: /* LUA_TBOOLEAN */
            if (g_lua_toboolean && g_lua_toboolean(L, abs_idx)) {
                sre_append_str(buf, pos, max, "true");
            } else {
                sre_append_str(buf, pos, max, "false");
            }
            break;
        case 3: { /* LUA_TNUMBER */
            char nbuf[64];
            double num = g_lua_tonumber ? g_lua_tonumber(L, abs_idx) : 0.0;
            sre_format_number(num, nbuf, sizeof(nbuf));
            sre_append_str(buf, pos, max, nbuf);
            break;
        }
        case 4: { /* LUA_TSTRING */
            size_t slen = 0;
            const char* str = g_lua_tolstring ? g_lua_tolstring(L, abs_idx, &slen) : NULL;
            if (str) {
                /* Cap output to 256 visible chars to guard against binary memory blobs */
                size_t safe_len = slen;
                if (safe_len > 256) safe_len = 256;
                sre_escape_string(buf, pos, max, str, safe_len);
                if (slen > 256) sre_append_str(buf, pos, max, "...");
            } else {
                sre_append_str(buf, pos, max, "\"\"");
            }
            break;
        }
        case 5: { /* LUA_TTABLE */
            const void* ptr = g_lua_topointer ? g_lua_topointer(L, abs_idx) : NULL;
            if (ptr) {
                for (int v = 0; v < visited_count; v++) {
                    if (visited[v] == ptr) {
                        sre_append_str(buf, pos, max, "<circular>");
                        g_lua_settop(L, top_before);
                        return;
                    }
                }
                if (visited_count < SRE_SERIALIZE_MAX_VISITED) {
                    visited[visited_count] = ptr;
                }
            }

            /* Single pass check if table is a pure array (1..N contiguous integer keys).
             * NOTE: Swordigo's Lua allocator injects hidden engine-userdata entries into
             * every table's hash part. lua_next exposes them. We MUST use array_len from
             * lua_objlen (which reads the sequence part only) and rawgeti to stay safe.
             */
            size_t array_len = g_lua_objlen ? g_lua_objlen(L, abs_idx) : 0;

            /* Count only Lua-visible integer keys 1..array_len in the sequence part.
             * Don't walk the hash part at all for the is_pure_array decision — use
             * objlen as ground truth for the sequence and check for any mixed keys. */
            int has_non_seq_keys = 0;

            if (g_lua_pushnil && g_lua_next) {
                g_lua_pushnil(L);
                while (g_lua_next(L, abs_idx) != 0) {
                    int ktype = g_lua_type(L, -2);
                    /* Only count non-integer or out-of-range keys as "non-sequence" */
                    if (ktype != 3 /* LUA_TNUMBER */) {
                        /* Non-numeric key: string (user field) or engine userdata.
                         * Userdata keys are engine-private — treat them as non-sequence
                         * but only flag mixed if it's a string (user-defined field). */
                        if (ktype == 4 /* LUA_TSTRING */) {
                            has_non_seq_keys = 1;
                        }
                        /* ktype==7 (userdata/engine): ignore — don't set has_non_seq_keys */
                    } else {
                        double k = g_lua_tonumber ? g_lua_tonumber(L, -2) : 0;
                        /* Flag mixed if non-integer or outside 1..array_len */
                        if (k < 1.0 || (double)(int64_t)k != k) {
                            has_non_seq_keys = 1;
                        } else {
                            int64_t ki = (int64_t)k;
                            if (array_len == 0 || ki > (int64_t)array_len) {
                                has_non_seq_keys = 1;
                            }
                        }
                    }
                    lua_pop(L, 1);
                }
            }

            if (array_len == 0 && !has_non_seq_keys) {
                /* Completely empty table */
                sre_append_str(buf, pos, max, "{}");
                g_lua_settop(L, top_before);
                return;
            }

            /* ---- Pure sequence table: use rawgeti to bypass hash/engine entries ---- */
            if (array_len > 0 && !has_non_seq_keys) {
                /* Compact inline: {1, 2, 3}  or  multi-line if long */
                int multiline = (array_len > 6);
                sre_append_str(buf, pos, max, "{");
                if (multiline) sre_append_str(buf, pos, max, "\n");
                for (size_t i = 1; i <= array_len; i++) {
                    if (multiline) {
                        sre_append_indent(buf, pos, max, depth + 1);
                    } else if (i > 1) {
                        sre_append_str(buf, pos, max, ", ");
                    }
                    if (g_lua_rawgeti) {
                        g_lua_rawgeti(L, abs_idx, (int)i);
                        sre_serialize_value_recursive(L, -1, buf, pos, max, depth + 1, visited, visited_count + 1);
                        lua_pop(L, 1);
                    }
                    if (multiline && i < array_len) sre_append_str(buf, pos, max, ",\n");
                }
                if (multiline) {
                    sre_append_str(buf, pos, max, "\n");
                    sre_append_indent(buf, pos, max, depth);
                }
                sre_append_str(buf, pos, max, "}");
            } else {
                /* ---- Mixed table: sequence + string keys, skip engine userdata keys ---- */
                sre_append_str(buf, pos, max, "{\n");
                int first = 1;

                /* First, emit array part via rawgeti (safe, bypasses engine entries) */
                if (array_len > 0 && g_lua_rawgeti) {
                    for (size_t i = 1; i <= array_len; i++) {
                        if (!first) sre_append_str(buf, pos, max, ",\n");
                        first = 0;
                        sre_append_indent(buf, pos, max, depth + 1);
                        char ikey[24];
                        sre_format_number((double)i, ikey, sizeof(ikey));
                        sre_append_str(buf, pos, max, "[");
                        sre_append_str(buf, pos, max, ikey);
                        sre_append_str(buf, pos, max, "] = ");
                        g_lua_rawgeti(L, abs_idx, (int)i);
                        sre_serialize_value_recursive(L, -1, buf, pos, max, depth + 1, visited, visited_count + 1);
                        lua_pop(L, 1);
                    }
                }

                /* Second, emit string-keyed fields only (skip userdata/engine keys) */
                if (g_lua_pushnil && g_lua_next) {
                    g_lua_pushnil(L);
                    while (g_lua_next(L, abs_idx) != 0) {
                        int ktype = g_lua_type(L, -2);

                        /* SKIP: engine-injected non-string non-integer keys (userdata, etc) */
                        if (ktype != 4 /* LUA_TSTRING */ && ktype != 3 /* LUA_TNUMBER */) {
                            lua_pop(L, 1); /* pop value, key stays for next iteration */
                            continue;
                        }
                        /* SKIP: integer keys 1..array_len (already emitted via rawgeti) */
                        if (ktype == 3) {
                            double k = g_lua_tonumber ? g_lua_tonumber(L, -2) : -1.0;
                            int64_t ki = (int64_t)k;
                            if ((double)ki == k && ki >= 1 && ki <= (int64_t)array_len) {
                                lua_pop(L, 1);
                                continue;
                            }
                        }

                        if (!first) sre_append_str(buf, pos, max, ",\n");
                        first = 0;
                        sre_append_indent(buf, pos, max, depth + 1);

                        /* Emit key — push copy to avoid mutating lua_next key at -2 */
                        if (g_lua_pushvalue) g_lua_pushvalue(L, -2);
                        if (ktype == 4 /* LUA_TSTRING */) {
                            size_t klen = 0;
                            const char* kstr = g_lua_tolstring ? g_lua_tolstring(L, -1, &klen) : NULL;
                            int is_ident = (kstr && kstr[0] && !(kstr[0] >= '0' && kstr[0] <= '9'));
                            for (size_t c = 0; is_ident && kstr && c < klen; c++) {
                                char ch = kstr[c];
                                if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
                                    is_ident = 0;
                            }
                            if (is_ident && kstr) {
                                sre_append_str(buf, pos, max, kstr);
                            } else if (kstr) {
                                sre_append_str(buf, pos, max, "[");
                                sre_escape_string(buf, pos, max, kstr, klen);
                                sre_append_str(buf, pos, max, "]");
                            }
                        } else {
                            sre_append_str(buf, pos, max, "[");
                            sre_serialize_value_recursive(L, -1, buf, pos, max, depth + 1, visited, visited_count + 1);
                            sre_append_str(buf, pos, max, "]");
                        }
                        lua_pop(L, 1); /* pop key copy */

                        sre_append_str(buf, pos, max, " = ");
                        sre_serialize_value_recursive(L, -1, buf, pos, max, depth + 1, visited, visited_count + 1);
                        lua_pop(L, 1); /* pop value */
                    }
                }
                sre_append_str(buf, pos, max, "\n");
                sre_append_indent(buf, pos, max, depth);
                sre_append_str(buf, pos, max, "}");
            }
            break;
        }
        case 6: { /* LUA_TFUNCTION */
            char fbuf[64];
            const void* ptr = g_lua_topointer ? g_lua_topointer(L, abs_idx) : NULL;
            sre_format_pointer("<function", ptr, fbuf, sizeof(fbuf));
            sre_append_str(buf, pos, max, fbuf);
            break;
        }
        case 2: /* LUA_TLIGHTUSERDATA */
        case 7: { /* LUA_TUSERDATA */
            char ubuf[64];
            const void* ptr = g_lua_topointer ? g_lua_topointer(L, abs_idx) : (g_lua_touserdata ? g_lua_touserdata(L, abs_idx) : NULL);
            sre_format_pointer("<userdata", ptr, ubuf, sizeof(ubuf));
            sre_append_str(buf, pos, max, ubuf);
            break;
        }
        case 8: { /* LUA_TTHREAD */
            char tbuf[64];
            const void* ptr = g_lua_topointer ? g_lua_topointer(L, abs_idx) : NULL;
            sre_format_pointer("<thread", ptr, tbuf, sizeof(tbuf));
            sre_append_str(buf, pos, max, tbuf);
            break;
        }
        default:
            sre_append_str(buf, pos, max, "<unknown>");
            break;
    }

    g_lua_settop(L, top_before);
}

/* ---- join N stack values from base+1 to top into result ---- */
static void sre_collect_returns(lua_State* L, int base) {
    int top = g_lua_gettop(L);
    int pos = 0;
    for (int i = base + 1; i <= top; i++) {
        if (i > base + 1 && pos < CONSOLE_BUF_SIZE - 2) {
            g_lua_console_result[pos++] = '\t';
        }
        const void* visited[SRE_SERIALIZE_MAX_VISITED] = {0};
        sre_serialize_value_recursive(L, i, g_lua_console_result, &pos, CONSOLE_BUF_SIZE, 0, visited, 0);
    }
    g_lua_console_result[pos] = 0;
}

volatile uint64_t g_sre_console_entry = 0;  /* diag */
volatile uint64_t g_sre_console_exit_ok = 0;  /* diag */

static void sre_run_console(lua_State* L) {
    g_sre_console_entry++;
    if (!g_lua_getfield || !g_lua_pcall || !g_lua_pushstring || !g_lua_gettop) {
        sre_strcopy(g_lua_console_result, "ERR: Lua API not resolved", CONSOLE_BUF_SIZE);
        g_lua_console_status = 2;
        return;
    }

    /* Clear capture buffers */
    g_lua_console_print_buf[0] = 0;
    g_lua_console_result[0]    = 0;

    int base = g_lua_gettop(L);

    /* Save original print() so we can restore it after execution */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "print");  /* stack: ... [orig_print] */

    /* Override print() with our capture version */
    g_lua_pushcclosure(L, sre_console_print, 0);
    g_lua_setfield(L, LUA_GLOBALSINDEX, "print");

    /* Auto-wrap bare expression: try "return <code>" first */
    char wrapped[CONSOLE_BUF_SIZE + 8];
    wrapped[0] = 'r'; wrapped[1] = 'e'; wrapped[2] = 't'; wrapped[3] = 'u';
    wrapped[4] = 'r'; wrapped[5] = 'n'; wrapped[6] = ' ';
    sre_strcopy(wrapped + 7, g_lua_console_buf, CONSOLE_BUF_SIZE);

    /* Try loadstring("return <code>") */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
    g_lua_pushstring(L, wrapped);
    int r = g_lua_pcall(L, 1, 2, 0);
    int use_return = (r == 0 && g_lua_type(L, -2) == LUA_TFUNCTION);

    if (!use_return) {
        /* Fall back to plain code */
        g_lua_settop(L, base + 1);  /* keep orig_print saved below base+1 */
        g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
        g_lua_pushstring(L, g_lua_console_buf);
        r = g_lua_pcall(L, 1, 2, 0);
        if (r != 0 || g_lua_type(L, -2) != LUA_TFUNCTION) {
            const char* err = g_lua_tolstring ? g_lua_tolstring(L, -1, 0) : "syntax error";
            if (err) sre_strcopy(g_lua_console_result, err, CONSOLE_BUF_SIZE);
            else     sre_strcopy(g_lua_console_result, "syntax error", CONSOLE_BUF_SIZE);
            /* Restore original print before returning */
            g_lua_settop(L, base + 1);
            g_lua_setfield(L, LUA_GLOBALSINDEX, "print");
            g_lua_settop(L, base);
            g_lua_console_status = 2;
            return;
        }
    }

    /* Pop nil (second return of loadstring), func is on top */
    lua_pop(L, 1);
    int call_base = g_lua_gettop(L);

    /* Execute the compiled chunk synchronously via pcall. This is the plain,
     * proven path: a compiled function is called and returns in one shot.
     * (No coroutine/lua_resume — Program.Wait-style yielding scripts are not
     * supported from the console, which matches the original behavior.) */
    r = g_lua_pcall(L, 0, LUA_MULTRET, 0);
    if (r != 0) {
        const char* err = g_lua_tolstring ? g_lua_tolstring(L, -1, 0) : "runtime error";
        if (err) sre_strcopy(g_lua_console_result, err, CONSOLE_BUF_SIZE);
        else     sre_strcopy(g_lua_console_result, "runtime error", CONSOLE_BUF_SIZE);
        /* Restore original print */
        g_lua_settop(L, base + 1);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "print");
        g_lua_settop(L, base);
        g_lua_console_status = 2;
        return;
    }

    /* Collect return values */
    sre_collect_returns(L, call_base - 1);

    /* Restore original print — it's sitting at base+1 on our save slot */
    g_lua_settop(L, base + 1);
    g_lua_setfield(L, LUA_GLOBALSINDEX, "print");
    g_lua_settop(L, base);

    /* If print() produced output and result is empty, use print output as result */
    if (!g_lua_console_result[0] && g_lua_console_print_buf[0]) {
        /* strip trailing newline */
        int len = 0;
        while (g_lua_console_print_buf[len]) len++;
        if (len > 0 && g_lua_console_print_buf[len-1] == '\n') g_lua_console_print_buf[len-1] = 0;
        sre_strcopy(g_lua_console_result, g_lua_console_print_buf, CONSOLE_BUF_SIZE);
    } else if (g_lua_console_print_buf[0] && g_lua_console_result[0]) {
        /* Both — prepend print output */
        char merged[CONSOLE_BUF_SIZE];
        sre_strcopy(merged, g_lua_console_print_buf, CONSOLE_BUF_SIZE);
        int ml = 0; while (merged[ml]) ml++;
        if (ml > 0 && merged[ml-1] == '\n') ml--;
        merged[ml++] = '\n';
        sre_strcopy(merged + ml, g_lua_console_result, CONSOLE_BUF_SIZE - ml);
        sre_strcopy(g_lua_console_result, merged, CONSOLE_BUF_SIZE);
    }

    if (!g_lua_console_result[0]) {
        g_lua_console_result[0] = 'O';
        g_lua_console_result[1] = 'K';
        g_lua_console_result[2] = 0;
    }
    g_lua_console_status = 1;
    g_sre_console_exit_ok++;
}



/* ========== ProgramState::Execute replacement ========== 
 * Original: calls lua_call(L, nargs, 0) which aborts on error
 * Ours: calls lua_pcall(L, nargs, 0, 0) which catches errors
 *
 * Symbol: _ZN5Caver12ProgramState7ExecuteEi
 * SwMini: patches/panic.c
 */
/* ---- Defensive lua_State validation for the console ----
 * The console must only ever run on a LIVE, GAME-READY lua_State. The game
 * creates and destroys ProgramStates (and their Lua states) during play; if
 * g_sre_last_lua_state is left pointing at a destroyed or wrong (child/UI)
 * state, sre_run_console would pcall on freed memory → GC corruption →
 * remarkupvals spin → console "stops working" after the first commands.
 *
 * Guards applied here:
 *   1. Only capture states that expose the game's Scene scripting API.
 *   2. Clear the captured pointer whenever ANY ProgramState is destroyed.
 *   3. Re-validate before every console service in sre_updateApplication.
 */
static int sre_state_is_valid_game_state(lua_State* L) {
    static int s_diag_shown = 0;
    if (!L) { if (!s_diag_shown) { fprintf(stderr, "[SRE/Validate] NULL state\n"); s_diag_shown = 1; } return 0; }
    if (((lu_byte*)L)[8] != LUA_TTHREAD) {
        if (!s_diag_shown) { fprintf(stderr, "[SRE/Validate] tt=%d (want %d) L=%p\n", ((lu_byte*)L)[8], LUA_TTHREAD, (void*)L); s_diag_shown = 1; }
        return 0;
    }
    uint64_t lg = (uint64_t)*(void**)((char*)L + 32);
    if (lg < 0x1000000ULL || lg >= 0x30000000ULL) {
        if (!s_diag_shown) { fprintf(stderr, "[SRE/Validate] bad l_G=0x%llx L=%p\n", (unsigned long long)lg, (void*)L); s_diag_shown = 1; }
        return 0;
    }
    if (!g_lua_getfield || !g_lua_type || !g_lua_settop || !g_lua_gettop) {
        if (!s_diag_shown) { fprintf(stderr, "[SRE/Validate] lua api not resolved\n"); s_diag_shown = 1; }
        return 0;
    }
    int base = g_lua_gettop(L);
    if (base < 0) { if (!s_diag_shown) { fprintf(stderr, "[SRE/Validate] bad gettop\n"); s_diag_shown = 1; } return 0; }
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Scene");
    int t = g_lua_type(L, -1);
    int ok = (t == LUA_TTABLE || t == LUA_TUSERDATA ||
              t == LUA_TLIGHTUSERDATA || t == LUA_TFUNCTION);
    if (!ok && !s_diag_shown) {
        fprintf(stderr, "[SRE/Validate] Scene type=%d (L=%p) — state rejected\n", t, (void*)L);
        s_diag_shown = 1;
    }
    g_lua_settop(L, base);
    return ok;
}

volatile uint64_t g_sre_exec_entries = 0;  /* diag */
volatile uint64_t g_sre_exec_console_seen = 0;  /* diag */

void sre_ProgramState_Execute(void* self, int stackIndex) {
    lua_State* L = PS_GET(self, PS_LUA_STATE, lua_State*);
    
    pthread_mutex_lock(&g_lua_mutex);

    /* Capture the lua_State for the host and console — only if it is a
     * live, game-ready state (has the Scene API). Child/UI states must not
     * hijack the console's target. */
    if (sre_state_is_valid_game_state(L)) {
        g_sre_last_lua_state = L;
    }
    g_sre_exec_entries++;

    /* Lazy Mini.* injection — ensure mod API exists before script runs */
    extern void sre_mini_ensure_injected(lua_State* L);
    sre_mini_ensure_injected(L);
    /* Check for pending console command */
    if (g_lua_console_pending && L && sre_state_is_valid_game_state(L)) {
        g_sre_exec_console_seen++;
        g_lua_console_pending = 0;
        sre_run_console(L);
    }

    
    /* Save Lua VM state for recovery */
    CallInfo* saved_ci = L->ci;
    StkId saved_top = L->top;
    StkId saved_base = L->base;
    unsigned short saved_nCcalls = L->nCcalls;
    
    /* Push recovery entry */
    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        /* Stack full — no recovery, just run */
        g_lua_pcall(L, stackIndex, 0, 0);
        pthread_mutex_unlock(&g_lua_mutex);
        return;
    }
    
    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        /* C++ exception caught — errorJmp restored by sre_cxa_throw */
        recovery_pop(my_depth);
        pthread_mutex_unlock(&g_lua_mutex);

        /* ─── Lua VM State Recovery ─────────────────────────────────── */
        luaF_close(L, saved_top);
        L->ci = saved_ci;
        L->top = saved_top;
        L->base = saved_base;
        L->nCcalls = saved_nCcalls;

        return;
    }
    
    /* Check if this is a coroutine (field at 0x08 != NULL) */
    void* coroutine = PS_GET(self, PS_COROUTINE, void*);
    
    if (coroutine == 0) {
        /* Not a thread — use pcall for error catching */
        int result = g_lua_pcall(L, stackIndex, 0, 0);
        recovery_pop(my_depth);

        if (result == 0) {
            pthread_mutex_unlock(&g_lua_mutex);
            return;
        }

        /* Surface script errors instead of swallowing them silently. Item
         * collection handlers (HeroEntityComponent::HandleItemCollection →
         * ProgramState::Execute(state,2)) only flag the SceneObject for
         * removal via Lua obj:destroy() at the END of the handler — a silent
         * mid-script error aborts it and the pickup stays in the scene
         * forever. Log (capped) so the failing call is diagnosable. */
        {
            static int s_exec_err_logged = 0;
            if (s_exec_err_logged < 32) {
                s_exec_err_logged++;
                const char* err_msg =
                    (g_lua_tolstring && g_lua_gettop && g_lua_gettop(L) > 0)
                        ? g_lua_tolstring(L, -1, 0) : NULL;
                fprintf(stderr,
                        "[SRE/Lua] ProgramState::Execute error (status %d, "
                        "state=%p): %s\n",
                        result, self, err_msg ? err_msg : "(no message)");
            }
        }

        /* Clean up the stack as the engine would */
        g_lua_settop(L, -2);
    } else {
        /* Coroutine — use lua_resume */
        PS_SET(self, PS_IS_SUSPENDED, int, 0);
        
        int result = g_lua_resume(L, stackIndex);
        recovery_pop(my_depth);
        
        if (result != LUA_YIELD) {
            PS_SET(self, PS_COMPLETED, char, 1);
        }
    }
    pthread_mutex_unlock(&g_lua_mutex);
}

/* ========== ProgramState::Resume replacement ==========
 * Symbol: _ZN5Caver12ProgramState6ResumeEi
 */
void sre_ProgramState_Resume(void* self, int stackIndex) {
    lua_State* L = PS_GET(self, PS_LUA_STATE, lua_State*);

    pthread_mutex_lock(&g_lua_mutex);

    /* Keep the console's target state fresh: Resume fires regularly during
     * gameplay, so a live game-ready state is always available to the console
     * even after scene transitions (where Execute may not fire again). Only
     * states exposing the game API are captured. */
    if (sre_state_is_valid_game_state(L)) {
        g_sre_last_lua_state = L;
    }

    /* Lazy Mini.* injection for coroutine states */
    extern void sre_mini_ensure_injected(lua_State* L);
    sre_mini_ensure_injected(L);
    
    /* Clear suspended flag */
    PS_SET(self, PS_IS_SUSPENDED, int, 0);
    
    /* Save Lua VM state for recovery */
    CallInfo* saved_ci = L->ci;
    StkId saved_top = L->top;
    StkId saved_base = L->base;
    unsigned short saved_nCcalls = L->nCcalls;

    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        int result = g_lua_resume(L, stackIndex);
        if (result != LUA_YIELD) PS_SET(self, PS_COMPLETED, char, 1);
        pthread_mutex_unlock(&g_lua_mutex);
        return;
    }
    
    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        recovery_pop(my_depth);
        pthread_mutex_unlock(&g_lua_mutex);
        PS_SET(self, PS_COMPLETED, char, 1);

        /* ─── Lua VM State Recovery ─────────────────────────────────── */
        luaF_close(L, saved_top);
        L->ci = saved_ci;
        L->top = saved_top;
        L->base = saved_base;
        L->nCcalls = saved_nCcalls;

        return;
    }
    
    int result = g_lua_resume(L, stackIndex);
    recovery_pop(my_depth);
    
    if (result != LUA_YIELD) {
        PS_SET(self, PS_COMPLETED, char, 1);
    }
    pthread_mutex_unlock(&g_lua_mutex);
}

/* ========== ProgramState::Update replacement ==========
 * Symbol: _ZN5Caver12ProgramState6UpdateEf
 *
 * Accurately replicates the Ghidra decompilation of ProgramState::Update:
 *
 *   ARM64 v1.4.12 ProgramState::Update address from nm: 0x4c15fc.
 *
 *   if ((this[0x51] != 0) || (this[0x52] != 0)) {          // condition1 || paused
 *       if (this[0x20] != NULL)                             // has SceneObject?
 *           scaledDelta = SceneObject::updateSpeedMultiplier(this[0x20]) * dt;
 *       speedScaling = *(float*)(this + 0x54);
 *
 *       if (this[0x48] == 1) {                              // isSuspended?
 *           this[0x4c] -= scaledDelta * speedScaling;       // tick timer
 *           if (this[0x4c] < 0) {
 *               this[0x48] = 0;                             // clear suspended
 *               result = lua_resume(L, 0);
 *               if (result != 1) this[0x53] = 1;           // mark completed
 *           }
 *       }
 *       // iterate child list (original code handles this)
 *   }
 *
 * KEY CORRECTNESS FIX:
 *   The relay stub calls the ORIGINAL Update for child iteration.
 *   If we leave PS_IS_SUSPENDED=1 when the timer is still counting down,
 *   the original ALSO ticks the timer → double-speed timers (bolts fire
 *   2x fast, sleep() durations halved). Fix: always clear PS_IS_SUSPENDED
 *   before calling original so the original's inner timer branch is skipped
 *   for *this* state. Children hit our hook recursively — correct behavior.
 */
typedef void (*pfn_orig_Update)(void* self, float deltaTime);
pfn_orig_Update g_orig_ProgramState_Update = 0;

void sre_ProgramState_Update(void* self, float deltaTime) {
    g_sre_ps_ticks++;
    /* L is needed throughout this function (timer countdown, lua_resume).
     * Declare it unconditionally here. */
    lua_State* L = PS_GET(self, PS_LUA_STATE, lua_State*);

    /* Capture lua_State and service the Lua console every frame */
    if (L) {
        g_sre_last_lua_state = L;
        if (__builtin_expect(g_lua_console_pending, 0)) {
            g_lua_console_pending = 0;
            sre_run_console(L);
        }
    }

    /* DEADLOCK GUARD: while scene loading is in progress, do NOT call lua_resume.
     * Scene::FinishLoad runs inside the updateApplication call chain. If any
     * SceneObject Lua script throws during FinishLoad, sre_cxa_throw longjmps
     * out of a pthread_mutex_lock we may be holding here. Skipping lua_resume
     * entirely while loading keeps the mutex clean. Children still iterate;
     * their timers count down but no coroutines fire until load completes. */
    extern volatile int g_sre_scene_loading;
    if (g_sre_scene_loading) {
        /* Still need to call child iteration (so children get registered and
         * cleaned up), but skip the lua_resume block above. Jump straight to
         * child propagation at the bottom. */
        goto child_update;
    }

    /* Ghidra line 332: outer guard — skip entire body if both flags are zero.
     * (Constructor sets condition1=1 so active states always enter.) */
    char condition1 = PS_GET(self, PS_CONDITION1, char);
    char paused     = PS_GET(self, PS_PAUSED,     char);

    if (condition1 || paused) {
        /* --- Apply SceneObject speed multiplier (Ghidra lines 333-336) --- */
        float scaledDelta = deltaTime;
        void* sceneObj = PS_GET(self, PS_SCENE_OBJECT, void*);
        if (sceneObj != 0 && g_getSpeedMultiplier != 0) {
            scaledDelta = g_getSpeedMultiplier(sceneObj) * deltaTime;
        }

        /* Ghidra line 337: speedScaling factor stored at +0x54 */
        float speedScaling = PS_GET(self, PS_SPEED_SCALING, float);

        /* Ghidra lines 339-350: timer countdown + coroutine resume */
        int isSuspended = PS_GET(self, PS_IS_SUSPENDED, int);
        if (isSuspended == 1) {
            g_sre_ps_suspended_seen++;
            float timer = PS_GET(self, PS_SLEEP_TIME, float);
            timer -= scaledDelta * speedScaling;
            PS_SET(self, PS_SLEEP_TIME, float, timer);

            if (timer < 0.0f) {
                /* Timer fired — permanently clear isSuspended and resume.
                 * After lua_resume, the Lua coroutine may call sleep() again
                 * (setting isSuspended=1 with a new timer) or finish
                 * (isSuspended stays 0, completed=1). Both are handled by
                 * the save/restore around the original call below. */
                PS_SET(self, PS_IS_SUSPENDED, int, 0);

                pthread_mutex_lock(&g_lua_mutex);

                /* Save Lua VM state for recovery */
                CallInfo* saved_ci = L->ci;
                StkId saved_top = L->top;
                StkId saved_base = L->base;
                unsigned short saved_nCcalls = L->nCcalls;

                int my_depth = recovery_push(L);
                if (my_depth < 0) {
                    if (g_lua_sethook) g_lua_sethook(L, (lua_Hook)sre_lua_timeout_hook, LUA_MASKCOUNT, 100000);
                    int r = g_lua_resume(L, 0);
                    g_sre_ps_timer_fires++;
                    if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
                    if (r != LUA_YIELD) {
                        if (r != 0) g_sre_ps_resume_errors++;
                        if (r != 0 && g_lua_tolstring && g_lua_gettop) {
                            void* sceneObj = PS_GET(self, PS_SCENE_OBJECT, void*);
                            const char* obj_id = sre_scene_object_identifier((SceneObject*)sceneObj);
                            const char* err_msg = g_lua_gettop(L) > 0 ? lua_tostring(L, -1) : "unknown";
                            fprintf(stderr, "[SRE/Lua] Coroutine ERROR (status %d) for object '%s' (%p): %s\n",
                                    r, obj_id ? obj_id : "<unnamed>", sceneObj, err_msg ? err_msg : "nil");
                        }
                        PS_SET(self, PS_COMPLETED, char, 1);
                    }
                    pthread_mutex_unlock(&g_lua_mutex);
                } else if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
                    recovery_pop(my_depth);
                    if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
                    pthread_mutex_unlock(&g_lua_mutex);
                    void* sceneObj = PS_GET(self, PS_SCENE_OBJECT, void*);
                    const char* obj_id = sre_scene_object_identifier((SceneObject*)sceneObj);
                    fprintf(stderr, "[SRE/Lua] Coroutine PANIC/LONGJMP recovered for object '%s' (%p)\n",
                            obj_id ? obj_id : "<unnamed>", sceneObj);
                    PS_SET(self, PS_COMPLETED, char, 1);

                    /* ─── Lua VM State Recovery ─────────────────────────────── */
                    luaF_close(L, saved_top);
                    L->ci = saved_ci;
                    L->top = saved_top;
                    L->base = saved_base;
                    L->nCcalls = saved_nCcalls;
                } else {
                    if (g_lua_sethook) g_lua_sethook(L, (lua_Hook)sre_lua_timeout_hook, LUA_MASKCOUNT, 100000);
                    int r = g_lua_resume(L, 0);
                    g_sre_ps_timer_fires++;
                    if (g_lua_sethook) g_lua_sethook(L, NULL, 0, 0);
                    recovery_pop(my_depth);
                    if (r != LUA_YIELD) {
                        if (r != 0) g_sre_ps_resume_errors++;
                        if (r != 0 && g_lua_tolstring && g_lua_gettop) {
                            void* sceneObj = PS_GET(self, PS_SCENE_OBJECT, void*);
                            const char* obj_id = sre_scene_object_identifier((SceneObject*)sceneObj);
                            const char* err_msg = g_lua_gettop(L) > 0 ? lua_tostring(L, -1) : "unknown";
                            fprintf(stderr, "[SRE/Lua] Coroutine ERROR (status %d) for object '%s' (%p): %s\n",
                                    r, obj_id ? obj_id : "<unnamed>", sceneObj, err_msg ? err_msg : "nil");
                        }
                        PS_SET(self, PS_COMPLETED, char, 1);
                    }
                    pthread_mutex_unlock(&g_lua_mutex);
                }

            }
            /* If timer still counting: do NOT touch isSuspended here.
             * It stays 1 so next frame we keep counting down.
             * The double-tick suppression is handled below via save/restore. */
        }
    }

    /* Call the original Update relay for child-state iteration + cleanup.
     *
     * KEY: temporarily zero PS_IS_SUSPENDED for *this* state before calling
     * the original, then RESTORE it afterward. This prevents the original
     * from double-ticking our timer (its isSuspended==1 branch is skipped),
     * while preserving the countdown state across frames so bolts/timers fire
     * at the correct time.
     *
     * Performance note: the sre_setjmp / recovery_push / recovery_pop wrap
     * that previously existed here has been intentionally removed. Because we
     * set isSuspended = 0 above, the original's coroutine-resume branch is
     * never entered for *this* state, so it cannot call lua_resume and cannot
     * panic/longjmp. Wrapping it with setjmp was therefore dead overhead on
     * every frame. The lua_resume setjmp inside the isSuspended==1 block
     * above (which IS a real call site) is retained unchanged. */
    if (g_orig_ProgramState_Update != 0) {
        /* Sanitize child ProgramState linked list at self + 0x10 before calling original.
         * Prevents ARM64 NoExecuteFault at 0x20202020202020 when a completed state node
         * or shared_ptr control block is corrupted by uninitialized/space-filled heap memory. */
        void** head = (void**)((char*)self + 0x10);
        if (head && *head) {
            void* node = *head;
            int depth = 0;
            int bad = 0;
            while (node != (void*)head && depth < 256) {
                uint64_t node_addr = (uint64_t)node;
                if (node_addr < 0x10000 || node_addr >= 0x0000800000000000ULL) { bad = 1; break; }
                void* child_ps = *(void**)((char*)node + 0x10);
                uint64_t child_addr = (uint64_t)child_ps;
                if (child_ps && (child_addr < 0x10000 || child_addr >= 0x0000800000000000ULL)) { bad = 1; break; }
                if (child_ps) {
                    void* shared_ctrl = *(void**)((char*)child_ps + 0x20);
                    uint64_t ctrl_addr = (uint64_t)shared_ctrl;
                    if (shared_ctrl) {
                        if (ctrl_addr < 0x10000 || ctrl_addr >= 0x0000800000000000ULL) {
                            *(void**)((char*)child_ps + 0x20) = NULL;
                        } else {
                            uint64_t vtable_ptr = *(uint64_t*)shared_ctrl;
                            if (!sre_is_valid_vtable_ptr(vtable_ptr)) {
                                *(void**)((char*)child_ps + 0x20) = NULL;
                            }
                        }
                    }
                }
                node = *(void**)node;
                depth++;
            }
            if (bad || depth >= 256) {
                fprintf(stderr, "[SRE/ProgramState] Corrupted child list node detected in %p — repairing sentinel\n", self);
                *head = (void*)head;
                *((void**)((char*)self + 0x18)) = (void*)head;
            }
        }

        int saved_suspended = PS_GET(self, PS_IS_SUSPENDED, int);
        PS_SET(self, PS_IS_SUSPENDED, int, 0);  /* suppress original's timer for *this* */

        /* TIME-SLICE GUARD: if this frame's coroutine budget is exhausted,
         * skip child-state iteration entirely. The children will be resumed
         * on the next frame. This prevents SDL/X11 starvation when hundreds
         * of coroutines are active simultaneously (e.g. large mod scripts). */
        if (!sre_frame_budget_check()) {
            /* RECOVERY WRAPPER: wrap child iteration in setjmp so that any
             * longjmp that escapes from a child coroutine error (sre_cxa_throw)
             * does NOT escape with the mutex still locked.
             *
             * Without this, the sequence:
             *   child ProgramState::Update
             *     → lua_resume (mutex locked)
             *       → Lua error → sre_luaD_throw → sre_cxa_throw → longjmp
             *         → longjmp escapes past pthread_mutex_unlock
             *           → mutex left locked FOREVER → game hangs
             *
             * With this wrapper, the longjmp lands HERE. We force-unlock the
             * mutex and continue — the child that threw is marked completed. */
            if (L != NULL) {
                int child_depth = recovery_push(L);
                if (child_depth >= 0 && sre_setjmp(g_sre_recovery_stack[child_depth].buf) != 0) {
                    /* longjmp fired from inside child iteration */
                    recovery_pop(child_depth);
                    fprintf(stderr, "[SRE/ProgramState] Child exception caught; continuing.\n");
                    /* Don't restore suspended — fall through to restore below */
                } else {
                    g_orig_ProgramState_Update(self, deltaTime);
                    if (child_depth >= 0) recovery_pop(child_depth);
                }
            } else {
                /* No Lua state — just call directly (scene objects without scripts) */
                g_orig_ProgramState_Update(self, deltaTime);
            }
        }
        /* else: budget expired — skip child iteration for this frame. */

child_update_done:
        /* Recovery-stack invariant: depth here equals the depth on entry. The
         * child-iteration block's recovery_push is always matched by a
         * recovery_pop on both its setjmp-fired and normal-completion paths
         * (and the isSuspended block earlier is likewise balanced), so no
         * push is outstanding at this label. */
        /* Restore: the original only iterates/cleans children, it never
         * modifies isSuspended for *this* when we passed isSuspended=0.
         * So restoring is always safe and correct. */
        PS_SET(self, PS_IS_SUSPENDED, int, saved_suspended);
    }
    return;

child_update:
    /* Recovery-stack invariant: depth here equals the depth on entry. This
     * fast path is reached via `goto child_update` taken BEFORE any
     * recovery_push, so nothing is outstanding to pop. */
    /* Scene loading fast path: skip lua_resume but still propagate to children
     * so the state tree stays consistent. No mutex taken here. */
    if (g_orig_ProgramState_Update != 0) {
        int saved_suspended = PS_GET(self, PS_IS_SUSPENDED, int);
        PS_SET(self, PS_IS_SUSPENDED, int, 0);
        g_orig_ProgramState_Update(self, deltaTime);
        PS_SET(self, PS_IS_SUSPENDED, int, saved_suspended);
    }
}


/* ========== updateApplication hook ==========
 * Symbol: Java_com_touchfoo_swordigo_Native_updateApplication
 * Address: 0x1478ccc (== updateApp in the game loop)
 *
 * Ghidra:
 *   void Java_..._updateApplication(void) {
 *       if (DAT_007f3c20 != NULL)
 *           (**(code**)(*DAT_007f3c20 + 0x68))();   // vtable → ProgramState::Update
 *   }
 *
 * We hook this outer JNI frame to:
 *   1. Service any pending Lua console commands before the frame
 *   2. Call-through to original so the game ticks normally
 *
 * The actual timer/coroutine logic is handled inside sre_ProgramState_Update
 * which is called from within the original's vtable dispatch. */
typedef void (*pfn_orig_updateApp)(void* env, void* obj);
pfn_orig_updateApp g_orig_updateApplication = 0;
volatile uint64_t g_sre_update_application_ticks = 0;

volatile uint64_t g_sre_console_service_entries = 0;  /* diag */
volatile uint64_t g_sre_console_pending_seen     = 0;  /* diag */
volatile uint64_t g_sre_console_runs             = 0;  /* diag */
volatile uint64_t g_sre_console_direct_runs      = 0;  /* diag: serviced via host direct-call */

/* ── Shared console servicer ────────────────────────────────────────────────
 * Services a single pending console command on a valid, game-ready lua_State.
 * Returns 1 if a command was actually executed this call, 0 otherwise.
 *
 * This is the ONE place the console mailbox is drained. It is called from two
 * sites with identical semantics:
 *   1. sre_console_run_pending()  — invoked SYNCHRONOUSLY by the host right
 *      after updateApp returns (same thread, VM idle) so a command runs in the
 *      SAME frame it was submitted → zero frame-latency round-trip.
 *   2. sre_updateApplication()    — the per-frame fallback, so a command still
 *      runs even if the host direct-call path is unavailable (e.g. no valid
 *      state yet at submit time, or mid scene-load).
 *
 * Whichever fires first clears g_lua_console_pending, so the other is a no-op —
 * the command can never run twice. All the existing safety guards are reused:
 * validity probe, stale-state drop, live-state re-lock, and Mini injection. */
static int sre_console_service_pending(void) {
    if (!g_lua_console_pending) return 0;

    if (g_sre_last_lua_state) {
        g_sre_console_service_entries++;
        if (!sre_state_is_valid_game_state(g_sre_last_lua_state)) {
            /* Stale/wrong state (destroyed or child/UI) — drop it so we never
             * pcall on freed memory; a future Execute re-captures a good one.
             * The pending command stays queued (pending flag untouched). */
            g_sre_last_lua_state = NULL;
        } else {
            extern void sre_mini_ensure_injected(lua_State* L);
            sre_mini_ensure_injected(g_sre_last_lua_state);

            if (g_lua_console_pending) {
                g_sre_console_pending_seen++;
                g_lua_console_pending = 0;
                g_sre_console_runs++;
                sre_run_console(g_sre_last_lua_state);
                return 1;
            }
        }
    }
    /* Lost our target (state destroyed)? Re-lock onto the newest live
     * game-ready state from the creation registry — the game keeps the
     * current scene's main state alive across the swap, so the console
     * keeps working instead of hanging on a pending command forever. */
    if (!g_sre_last_lua_state && g_lua_console_pending) {
        lua_State* fresh = sre_find_live_game_state();
        if (fresh) {
            g_sre_last_lua_state = fresh;
            extern void sre_mini_ensure_injected(lua_State* L);
            sre_mini_ensure_injected(fresh);
            g_sre_console_service_entries++;
            g_sre_console_pending_seen++;
            g_lua_console_pending = 0;
            g_sre_console_runs++;
            sre_run_console(fresh);
            return 1;
        }
    }
    return 0;
}

void sre_updateApplication(void* env, void* obj) {
    g_sre_update_application_ticks++;
    /* 1. Service Lua console & Mini injection BEFORE the frame tick.
     *
     * This runs INSIDE the guest updateApp frame — the only context where the
     * JNI/host bridge is fully set up, so any engine Lua the command triggers
     * (which may call back through JniBridge64::call_handler into host GL) is
     * safe. An earlier attempt to service it from a SEPARATE host-initiated
     * emulator->call (right after updateApp returned) SIGSEGV'd for exactly
     * that reason: the console command's Lua reached a bridge call with no
     * valid frame/bridge context. Servicing here, at the very top of the frame,
     * already runs the command with zero added intra-frame latency and full
     * bridge safety — so this hook is the single, correct servicer.
     *
     * The per-command validation cost was removed separately (the stdlib
     * re-open fix), so this is cheap when nothing is pending. */
    sre_console_service_pending();

    /* 2. Compute delta time from host wall clock.
     *    We maintain our own dt here so we can apply g_sre_game_speed and
     *    cap dt to avoid physics explosions after lag spikes (e.g. alt-tab). */
    static uint64_t s_last_frame_ns = 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        float dt = 0.016666667f;  /* Default: 60 Hz */
        if (s_last_frame_ns != 0) {
            dt = (float)(now_ns - s_last_frame_ns) / 1e9f;
            /* Clamp: minimum 1ms, maximum 100ms (avoids physics explosion after stall) */
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.1f)   dt = 0.1f;
        }
        s_last_frame_ns = now_ns;

        /* 3. Dispatch PC-safe frame tick (sre_frame_loop.c) */
        extern void sre_frame_update(void* env, void* obj, float dt);
        sre_frame_update(env, obj, dt);

        /* 4. Per-frame Z-walk poll (W/A/S/D → Throndigo parity). Runs in the
         *    same pcall-safe context as the console; no-op unless the z-walk
         *    was injected. */
        extern void sre_mini_zwalk_poll(void);
        sre_mini_zwalk_poll();
    }
}



/* ========== handleTouchEvent hook ==========
 * Symbol: Java_com_touchfoo_swordigo_Native_handleTouchEvent
 * Offset: 0x478f84
 *
 * Wraps the guest touch handler with exception recovery so that
 * any script exceptions thrown by button click handlers do not
 * result in unrecovered longjmps (which corrupt JIT states and hang).
 */
typedef void (*pfn_orig_handleTouchEvent)(void* env, void* obj, int action, int id, double time, float x, float y, float oldX, float oldY, int tapCount);
pfn_orig_handleTouchEvent g_orig_handleTouchEvent = 0;

void sre_handleTouchEvent(void* env, void* obj, int action, int id, double time, float x, float y, float oldX, float oldY, int tapCount) {
    if (g_orig_handleTouchEvent) {
        lua_State* L = g_sre_last_lua_state;
        if (L != NULL) {
            /* Save Lua VM state for recovery */
            CallInfo* saved_ci = L->ci;
            StkId saved_top = L->top;
            StkId saved_base = L->base;
            unsigned short saved_nCcalls = L->nCcalls;

            int d = recovery_push(L);
            if (d < 0) {
                g_orig_handleTouchEvent(env, obj, action, id, time, x, y, oldX, oldY, tapCount);
            } else if (sre_setjmp(g_sre_recovery_stack[d].buf) != 0) {
                recovery_pop(d);
                sre_log_lua_error("handleTouchEvent", "Exception caught in handleTouchEvent!");

                /* ─── Lua VM State Recovery ─────────────────────────────────── */
                luaF_close(L, saved_top);
                L->ci = saved_ci;
                L->top = saved_top;
                L->base = saved_base;
                L->nCcalls = saved_nCcalls;
            } else {
                g_orig_handleTouchEvent(env, obj, action, id, time, x, y, oldX, oldY, tapCount);
                recovery_pop(d);
            }
        } else {
            g_orig_handleTouchEvent(env, obj, action, id, time, x, y, oldX, oldY, tapCount);
        }
    }
}

void sre_ProgramState_destructor(void* self) {
    /* Evict the closed lua_State from the injection cache so that if the
     * guest allocator recycles this pointer for a new state, the new state
     * gets fully initialized by sre_mini_ensure_injected instead of being
     * silently skipped as "already injected".
     *
     * Clear g_sre_last_lua_state for EVERY destroyed ProgramState (root or
     * child): a stale pointer here would make the console pcall on freed
     * memory → GC corruption → remarkupvals spin → console death. */
    lua_State* L = *(lua_State**)self;
    if (L != NULL) {
        extern void sre_mini_remove_injected(lua_State* L);
        sre_mini_remove_injected(L);
        if (L == g_sre_last_lua_state) {
            g_sre_last_lua_state = NULL;
        }
        /* Drop destroyed states from the creation registry so the console
         * never re-locks onto freed memory. */
        sre_state_evict(L);
    }

    if (g_orig_ProgramState_destructor) {
        g_orig_ProgramState_destructor(self);
    }
}
