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

#include "sre.h"
#include "sre_lua.h"

/* ========== Lua API Function Pointers ========== */

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
pfn_lua_pushlightuserdata g_lua_pushlightuserdata = 0;
pfn_lua_error       g_lua_error = 0;

/* Pointer to SceneObject::updateSpeedMultiplier (resolved by host) */
typedef float (*pfn_getSpeedMultiplier)(void* sceneObject);
pfn_getSpeedMultiplier g_getSpeedMultiplier = 0;

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
    sre_u64 getSpeedMultiplier;
} SreLuaAddrs;

extern void sre_mini_ensure_injected(lua_State* L);
extern void sre_log_lua_error(const char* source, const char* err_msg);
extern lua_State* g_sre_last_lua_state;

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
    g_lua_pushlightuserdata = (pfn_lua_pushlightuserdata)addrs->lua_pushlightuserdata;
    g_lua_error       = (pfn_lua_error)addrs->lua_error;
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

/* Push a recovery entry. Returns the depth index, or -1 if stack full. */
static int recovery_push(lua_State* L) {
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

/* Pop recovery stack back to a given depth */
static void recovery_pop(int depth) {
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

    /* Lazy Mini.* injection — injects on first call per lua_State */
    extern void sre_mini_ensure_injected(lua_State* L);
    sre_mini_ensure_injected(L);
    
    /* Save stack top for recovery */
    int saved_top = 0;
    if (g_lua_gettop) {
        saved_top = g_lua_gettop(L);
    }
    
    /* Push recovery entry */
    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        /* Stack full — fall through to raw pcall without recovery */
        int result = g_lua_pcall(L, nargs, nresults, 0);
        if (result != 0 && g_lua_settop) g_lua_settop(L, -2);
        return;
    }
    
    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        /* Caught C++ exception via longjmp from sre_cxa_throw!
         * errorJmp was already restored by sre_cxa_throw. */
        recovery_pop(my_depth);
        g_lua_call_safe_errors++;
        /* Note: error message not available via longjmp path */
        g_sre_lua_error_count++;
        
        /* Restore Lua stack to pre-call state (BUG 3 FIX: guard against underflow) */
        int new_top = saved_top - (nargs + 1);
        if (g_lua_settop && new_top >= 0) {
            g_lua_settop(L, new_top);
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
            g_lua_settop(L, -2);
        }
        if (nresults != -1 && g_lua_pushnil) {
            int i;
            for (i = 0; i < nresults; i++) {
                g_lua_pushnil(L);
            }
        }
    }
}

/* ========== sre_lua_resume_safe — PROTECTED lua_resume wrapper ==========
 *
 * Unlike sre_lua_call_safe (which replaces lua_call entirely with pcall),
 * this function wraps the ORIGINAL lua_resume with setjmp recovery.
 *
 * The host patches all BL instructions in libswordigo.so that target
 * lua_resume to instead target this function. The original lua_resume
 * function bytes are NOT modified — g_lua_resume still works.
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

int sre_lua_resume_safe(lua_State* L, int narg) {
    if (!g_lua_resume) {
        /* Should never happen — lua_resume not resolved */
        return 2;  /* LUA_ERRRUN */
    }

    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        /* Recovery stack full — call without protection (fallback) */
        return g_lua_resume(L, narg);
    }

    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        /* Caught C++ exception via longjmp from sre_cxa_throw!
         * The coroutine threw a Lua error during resume.
         * errorJmp was already restored by sre_cxa_throw. */
        recovery_pop(my_depth);
        g_lua_resume_safe_errors++;
        g_sre_resume_err_count++;
        /* Try to get the error message from the Lua stack */
        capture_lua_error(L);
        return 2;  /* LUA_ERRRUN */
    }

    int result = g_lua_resume(L, narg);
    recovery_pop(my_depth);

    /* Also capture errors from normal lua_resume return (non-exception path) */
    if (result >= 2) {
        g_sre_resume_err_count++;
        capture_lua_error(L);
    }

    return result;
}
/* ========== Lua Console — in-engine Lua code execution ==========
 * 
 * The host writes a Lua code string to g_lua_console_buf[] and
 * sets g_lua_console_pending = 1. On the next ProgramState::Execute,
 * SRE runs the code in the engine's lua_State.
 *
 * Results/errors are written to g_lua_console_result[].
 *
 * These globals are at known addresses that the host reads/writes
 * directly in guest memory via get_symbol_vaddr().
 */
#define CONSOLE_BUF_SIZE 4096
char g_lua_console_buf[CONSOLE_BUF_SIZE];     /* Lua code to execute */
char g_lua_console_result[CONSOLE_BUF_SIZE];  /* Output/error message */
int  g_lua_console_pending = 0;               /* 1 = host wants to run code */
int  g_lua_console_status = 0;                /* 0=idle, 1=success, 2=error */

/* Last captured lua_State — for host inspection */
lua_State* g_sre_last_lua_state = 0;

/* Internal: execute pending console command in the given lua_State
 * 
 * Uses Lua's own loadstring() function via pcall — this avoids the
 * setjmp/longjmp crash that happens when luaL_loadbuffer hits syntax errors.
 * 
 * Flow: pcall(loadstring, code) → pcall(result_func)
 */
static void sre_run_console(lua_State* L) {
    if (!g_lua_getfield || !g_lua_pcall || !g_lua_pushstring || !g_lua_tolstring) {
        const char* msg = "ERR: Lua API not resolved";
        int i;
        for (i = 0; msg[i] && i < CONSOLE_BUF_SIZE - 1; i++)
            g_lua_console_result[i] = msg[i];
        g_lua_console_result[i] = 0;
        g_lua_console_status = 2;
        return;
    }
    
    int base_top = g_lua_gettop(L);
    
    /* Step 1: Get loadstring function from globals */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
    
    /* Step 2: Push the code string */
    g_lua_pushstring(L, g_lua_console_buf);
    
    /* Step 3: pcall loadstring(code) → (func, nil) or (nil, errmsg) */
    int r = g_lua_pcall(L, 1, 2, 0);
    if (r != 0) {
        /* pcall itself failed (shouldn't happen) */
        const char* err = lua_tostring(L, -1);
        if (err) {
            int i;
            for (i = 0; err[i] && i < CONSOLE_BUF_SIZE - 1; i++)
                g_lua_console_result[i] = err[i];
            g_lua_console_result[i] = 0;
        }
        g_lua_settop(L, base_top);
        g_lua_console_status = 2;
        return;
    }
    
    /* Step 4: Check if loadstring returned a function or nil+error */
    int type_at_minus2 = g_lua_type(L, -2);
    
    if (type_at_minus2 != LUA_TFUNCTION) {
        /* Syntax error: stack is [nil, errmsg] */
        const char* err = lua_tostring(L, -1);
        if (err) {
            int i;
            for (i = 0; err[i] && i < CONSOLE_BUF_SIZE - 1; i++)
                g_lua_console_result[i] = err[i];
            g_lua_console_result[i] = 0;
        } else {
            g_lua_console_result[0] = '?';
            g_lua_console_result[1] = 0;
        }
        g_lua_settop(L, base_top);
        g_lua_console_status = 2;
        return;
    }
    
    /* Step 5: Pop the nil, keep the function */
    lua_pop(L, 1);  /* remove nil from top, function is now at top */
    
    /* Step 6: pcall the compiled function */
    r = g_lua_pcall(L, 0, 1, 0);
    if (r != 0) {
        /* Runtime error */
        const char* err = lua_tostring(L, -1);
        if (err) {
            int i;
            for (i = 0; err[i] && i < CONSOLE_BUF_SIZE - 1; i++)
                g_lua_console_result[i] = err[i];
            g_lua_console_result[i] = 0;
        }
        g_lua_settop(L, base_top);
        g_lua_console_status = 2;
        return;
    }
    
    /* Step 7: Read result */
    if (g_lua_gettop(L) > base_top && g_lua_type(L, -1) != LUA_TNIL) {
        const char* result = lua_tostring(L, -1);
        if (result) {
            int i;
            for (i = 0; result[i] && i < CONSOLE_BUF_SIZE - 1; i++)
                g_lua_console_result[i] = result[i];
            g_lua_console_result[i] = 0;
        } else {
            g_lua_console_result[0] = 'O';
            g_lua_console_result[1] = 'K';
            g_lua_console_result[2] = 0;
        }
    } else {
        g_lua_console_result[0] = 'O';
        g_lua_console_result[1] = 'K';
        g_lua_console_result[2] = 0;
    }
    g_lua_settop(L, base_top);
    g_lua_console_status = 1;
}

/* ========== ProgramState::Execute replacement ========== 
 * Original: calls lua_call(L, nargs, 0) which aborts on error
 * Ours: calls lua_pcall(L, nargs, 0, 0) which catches errors
 *
 * Symbol: _ZN5Caver12ProgramState7ExecuteEi
 * SwMini: patches/panic.c
 */
void sre_ProgramState_Execute(void* self, int stackIndex) {
    lua_State* L = PS_GET(self, PS_LUA_STATE, lua_State*);
    
    /* Capture the lua_State for the host and console */
    g_sre_last_lua_state = L;

    /* Lazy Mini.* injection — ensure mod API exists before script runs */
    extern void sre_mini_ensure_injected(lua_State* L);
    sre_mini_ensure_injected(L);
    /* Check for pending console command */
    if (g_lua_console_pending && L) {
        g_lua_console_pending = 0;
        sre_run_console(L);
    }
    
    /* Push recovery entry */
    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        /* Stack full — no recovery, just run */
        g_lua_pcall(L, stackIndex, 0, 0);
        return;
    }
    
    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        /* C++ exception caught — errorJmp restored by sre_cxa_throw */
        recovery_pop(my_depth);
        return;
    }
    
    /* Check if this is a coroutine (field at 0x08 != NULL) */
    void* coroutine = PS_GET(self, PS_COROUTINE, void*);
    
    if (coroutine == 0) {
        /* Not a thread — use pcall for error catching */
        int result = g_lua_pcall(L, stackIndex, 0, 0);
        recovery_pop(my_depth);
        
        if (result == 0) {
            return;
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
}

/* ========== ProgramState::Resume replacement ==========
 * Symbol: _ZN5Caver12ProgramState6ResumeEi
 */
void sre_ProgramState_Resume(void* self, int stackIndex) {
    lua_State* L = PS_GET(self, PS_LUA_STATE, lua_State*);

    /* Lazy Mini.* injection for coroutine states */
    extern void sre_mini_ensure_injected(lua_State* L);
    sre_mini_ensure_injected(L);
    
    /* Clear suspended flag */
    PS_SET(self, PS_IS_SUSPENDED, int, 0);
    
    /* Push recovery entry */
    int my_depth = recovery_push(L);
    if (my_depth < 0) {
        int result = g_lua_resume(L, stackIndex);
        if (result != LUA_YIELD) PS_SET(self, PS_COMPLETED, char, 1);
        return;
    }
    
    if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
        recovery_pop(my_depth);
        PS_SET(self, PS_COMPLETED, char, 1);
        return;
    }
    
    int result = g_lua_resume(L, stackIndex);
    recovery_pop(my_depth);
    
    if (result != LUA_YIELD) {
        PS_SET(self, PS_COMPLETED, char, 1);
    }
}

/* ========== ProgramState::Update replacement ==========
 * Symbol: _ZN5Caver12ProgramState6UpdateEf
 *
 * Accurately replicates the Ghidra decompilation of ProgramState::Update:
 *
 *   Ghidra (libswordigo_v1.4.12.so.c lines 551876–551949 / address 0x3198bd):
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
    /* Capture lua_State and service the Lua console every frame */
    lua_State* L = PS_GET(self, PS_LUA_STATE, lua_State*);
    if (L) {
        g_sre_last_lua_state = L;
        if (__builtin_expect(g_lua_console_pending, 0)) {
            g_lua_console_pending = 0;
            sre_run_console(L);
        }
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

                int my_depth = recovery_push(L);
                if (my_depth < 0) {
                    int r = g_lua_resume(L, 0);
                    if (r != LUA_YIELD) PS_SET(self, PS_COMPLETED, char, 1);
                } else if (sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
                    recovery_pop(my_depth);
                    PS_SET(self, PS_COMPLETED, char, 1);
                } else {
                    int r = g_lua_resume(L, 0);
                    recovery_pop(my_depth);
                    if (r != LUA_YIELD) PS_SET(self, PS_COMPLETED, char, 1);
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
     * Saved after our timer logic above: may be 0 (timer fired, Lua finished)
     * or 1 (timer still counting / Lua re-slept after resume). */
    if (g_orig_ProgramState_Update != 0) {
        int saved_suspended = PS_GET(self, PS_IS_SUSPENDED, int);
        PS_SET(self, PS_IS_SUSPENDED, int, 0);  /* suppress original's timer for *this* */

        lua_State* L2 = PS_GET(self, PS_LUA_STATE, lua_State*);
        if (L2 != NULL) {
            int d = recovery_push(L2);
            if (d < 0) {
                g_orig_ProgramState_Update(self, deltaTime);
            } else if (sre_setjmp(g_sre_recovery_stack[d].buf) != 0) {
                recovery_pop(d);
                PS_SET(self, PS_COMPLETED, char, 1);
                saved_suspended = 0;  /* completed — don't restore suspended */
            } else {
                g_orig_ProgramState_Update(self, deltaTime);
                recovery_pop(d);
            }
        } else {
            g_orig_ProgramState_Update(self, deltaTime);
        }

        /* Restore: the original only iterates/cleans children, it never
         * modifies isSuspended for *this* when we passed isSuspended=0.
         * So restoring is always safe and correct. */
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

void sre_updateApplication(void* env, void* obj) {
    /* Service Lua console before the frame update */
    if (g_lua_console_pending && g_sre_last_lua_state) {
        g_lua_console_pending = 0;
        sre_run_console(g_sre_last_lua_state);
    }
    /* Call-through to original */
    if (g_orig_updateApplication) {
        g_orig_updateApplication(env, obj);
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
            int d = recovery_push(L);
            if (d < 0) {
                g_orig_handleTouchEvent(env, obj, action, id, time, x, y, oldX, oldY, tapCount);
            } else if (sre_setjmp(g_sre_recovery_stack[d].buf) != 0) {
                recovery_pop(d);
                sre_log_lua_error("handleTouchEvent", "Exception caught in handleTouchEvent!");
            } else {
                g_orig_handleTouchEvent(env, obj, action, id, time, x, y, oldX, oldY, tapCount);
                recovery_pop(d);
            }
        } else {
            g_orig_handleTouchEvent(env, obj, action, id, time, x, y, oldX, oldY, tapCount);
        }
    }
}
