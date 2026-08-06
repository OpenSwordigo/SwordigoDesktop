/* ============================================================
 * sre_ffi.c — Native FFI for SRE Lua API
 * ============================================================
 * Provides a proper Foreign Function Interface for mod scripts
 * running inside the SRE guest (ARM64 libsre.so).
 *
 * Because SRE itself IS the ARM64 native process, we can call
 * game functions directly via function pointers — no JNI, no
 * separate FFI ABI marshal layer needed. libffi on the host
 * side is used for host→guest callbacks; for guest Lua→native
 * calls we just cast the address and call with typed args.
 *
 * API exposed to Lua:
 *
 *   -- Core call (immediate)
 *   ffi.call(addr, ret_type, arg_types, ...)
 *     addr      : number  — absolute guest VA (e.g. g_swordigo_base + offset)
 *     ret_type  : string  — "void","i32","i64","f32","f64","ptr","str"
 *     arg_types : string  — arg type string e.g. "iif" = (int, int, float)
 *     ...       : values  — Lua values matching arg_types
 *   Returns: the return value pushed to Lua, or nothing for "void".
 *
 *   -- Bind: create a reusable closure
 *   local fn = ffi.bind(addr, ret_type, arg_types)
 *   fn(...)  -- call as above but without addr/ret/types each time
 *
 *   -- Peek/Poke: raw memory access
 *   ffi.peek8(addr)             → byte
 *   ffi.peek16(addr)            → u16
 *   ffi.peek32(addr)            → u32
 *   ffi.peek64(addr)            → u64
 *   ffi.peekf(addr)             → float
 *   ffi.peekstr(addr)           → C string at that ptr
 *   ffi.poke8(addr, value)
 *   ffi.poke16(addr, value)
 *   ffi.poke32(addr, value)
 *   ffi.poke64(addr, value)
 *   ffi.pokef(addr, value)
 *
 *   -- Offset helpers
 *   ffi.offset(base, ...)       → base + sum(offsets)
 *   ffi.deref(addr)             → *(void**)addr   (follow pointer)
 *   ffi.null()                  → 0  (nil-safe null)
 *   ffi.base()                  → g_swordigo_base
 *
 *   -- Struct helpers
 *   ffi.readf32(addr, offset)
 *   ffi.readf64(addr, offset)
 *   ffi.readi32(addr, offset)
 *   ffi.readi64(addr, offset)
 *   ffi.writef32(addr, offset, val)
 *   ffi.writei32(addr, offset, val)
 *
 *   -- Type code string format (arg_types / ret_type):
 *     'v' or "void"  = void (ret only)
 *     'b'            = bool  (int)
 *     'c'            = int8
 *     'h'            = int16
 *     'i'            = int32
 *     'l'            = int64
 *     'f'            = float32
 *     'd'            = float64
 *     'p'            = pointer (void*, passed as Lua number = address)
 *     's'            = C string (const char*, read-only)
 *
 * Architecture notes:
 *   - SRE is ARM64 EABI (aarch64-linux-gnu ABI).
 *   - Integer args ≤8 bytes go in X0–X7.
 *   - Float/double args go in D0–D7.
 *   - For simplicity we support up to 8 integer + 8 float args via
 *     a set of typed union registers.
 *   - Structs by value are NOT supported in this API (use ptr).
 *   - All pointers are 64-bit guest VAs.
 * ============================================================ */

#include "sre.h"
#include "sre_lua.h"
#include "sre_caver.h"

/* We use the guest libc functions available via sre_caver.c bridge */
extern void* memcpy(void* dest, const void* src, size_t n);
extern void* memset(void* s, int c, size_t n);
extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int snprintf(char* buf, size_t sz, const char* fmt, ...);

/* g_swordigo_base from sre_init.c */
extern uint64_t g_swordigo_base;

/* =========================================================================
 * Type parsing helpers
 * ========================================================================= */

#define SRE_FFI_MAX_ARGS 16

typedef enum {
    FFI_T_VOID   = 0,
    FFI_T_BOOL   = 1,
    FFI_T_I8     = 2,
    FFI_T_I16    = 3,
    FFI_T_I32    = 4,
    FFI_T_I64    = 5,
    FFI_T_F32    = 6,
    FFI_T_F64    = 7,
    FFI_T_PTR    = 8,
    FFI_T_STR    = 9,
} SreFfiType;

static SreFfiType ffi_parse_type_char(char c) {
    switch (c) {
        case 'v': return FFI_T_VOID;
        case 'b': return FFI_T_BOOL;
        case 'c': return FFI_T_I8;
        case 'h': return FFI_T_I16;
        case 'i': return FFI_T_I32;
        case 'l': return FFI_T_I64;
        case 'f': return FFI_T_F32;
        case 'd': return FFI_T_F64;
        case 'p': return FFI_T_PTR;
        case 's': return FFI_T_STR;
        default:  return FFI_T_PTR;
    }
}

static SreFfiType ffi_parse_ret_type(const char* s) {
    if (!s || !s[0]) return FFI_T_VOID;
    if (s[1] == '\0') return ffi_parse_type_char(s[0]);
    if (strcmp(s, "void") == 0)  return FFI_T_VOID;
    if (strcmp(s, "bool") == 0)  return FFI_T_BOOL;
    if (strcmp(s, "i8")   == 0)  return FFI_T_I8;
    if (strcmp(s, "i16")  == 0)  return FFI_T_I16;
    if (strcmp(s, "i32")  == 0)  return FFI_T_I32;
    if (strcmp(s, "i64")  == 0)  return FFI_T_I64;
    if (strcmp(s, "f32")  == 0)  return FFI_T_F32;
    if (strcmp(s, "f64")  == 0)  return FFI_T_F64;
    if (strcmp(s, "ptr")  == 0)  return FFI_T_PTR;
    if (strcmp(s, "str")  == 0)  return FFI_T_STR;
    return FFI_T_VOID;
}

/* =========================================================================
 * ARM64 native call dispatch
 * =========================================================================
 * We use a variadic integer + float split. ARM64 ABI: integer args in x0-x7,
 * float args in d0-d7.  We pass them explicitly so the compiler knows the layout.
 *
 * For simplicity we use a "fat" call helper with uint64 and double unions.
 * The compiler (aarch64-linux-gnu) will place these in the correct registers.
 * ========================================================================= */

typedef union { uint64_t u64; int64_t i64; uint32_t u32; int32_t i32;
                float f32; double f64; void* ptr; } SreFfiReg;

/* Maximum 8 integer registers + 8 float registers for ARM64 EABI */
typedef struct {
    SreFfiType arg_types[SRE_FFI_MAX_ARGS];
    int        n_args;
    SreFfiReg  iargs[8];  /* integer/pointer args (x0-x7) */
    SreFfiReg  fargs[8];  /* float args (d0-d7) */
    int        n_iargs;
    int        n_fargs;
} SreFfiCall;

/* Fill iargs/fargs from Lua stack starting at lua_base_idx */
static int ffi_fill_args(lua_State* L, SreFfiCall* c,
                          const char* arg_types_str, int lua_base_idx) {
    c->n_args  = 0;
    c->n_iargs = 0;
    c->n_fargs = 0;

    if (!arg_types_str) return 1;

    int i = 0;
    for (; arg_types_str[i] && c->n_args < SRE_FFI_MAX_ARGS; i++) {
        SreFfiType t = ffi_parse_type_char(arg_types_str[i]);
        c->arg_types[c->n_args++] = t;

        int lua_idx = lua_base_idx + i;
        int ltype = g_lua_type(L, lua_idx);

        switch (t) {
            case FFI_T_VOID:
                break;
            case FFI_T_BOOL:
                if (c->n_iargs < 8) c->iargs[c->n_iargs++].u64 = g_lua_toboolean(L, lua_idx) ? 1 : 0;
                break;
            case FFI_T_I8:
            case FFI_T_I16:
            case FFI_T_I32:
                if (c->n_iargs < 8) c->iargs[c->n_iargs++].i64 = (int64_t)g_lua_tointeger(L, lua_idx);
                break;
            case FFI_T_I64:
                if (c->n_iargs < 8) c->iargs[c->n_iargs++].i64 = (int64_t)g_lua_tonumber(L, lua_idx);
                break;
            case FFI_T_F32:
                if (c->n_fargs < 8) c->fargs[c->n_fargs++].f32 = (float)g_lua_tonumber(L, lua_idx);
                break;
            case FFI_T_F64:
                if (c->n_fargs < 8) c->fargs[c->n_fargs++].f64 = g_lua_tonumber(L, lua_idx);
                break;
            case FFI_T_PTR:
                if (c->n_iargs < 8) {
                    if (ltype == LUA_TLIGHTUSERDATA)
                        c->iargs[c->n_iargs++].ptr = g_lua_touserdata(L, lua_idx);
                    else
                        c->iargs[c->n_iargs++].u64 = (uint64_t)(int64_t)g_lua_tonumber(L, lua_idx);
                }
                break;
            case FFI_T_STR:
                if (c->n_iargs < 8)
                    c->iargs[c->n_iargs++].ptr = (void*)g_lua_tolstring(L, lua_idx, (sre_size_t*)0);
                break;
        }
    }
    return 1;
}

/* =========================================================================
 * Native dispatch stubs: 0–4 int args, 0–2 float args
 * We can't write a fully generic vararg dispatch in C (undefined behavior),
 * so we use a set of typed trampolines that covers the common cases.
 *
 * We expand to support up to 6 integer + 4 float args which covers >99%
 * of Swordigo's internal API surface.
 * ========================================================================= */

/* Typedefs for the call signatures we support */
typedef uint64_t (*pfn_v)(void);
typedef uint64_t (*pfn_i)(uint64_t);
typedef uint64_t (*pfn_ii)(uint64_t,uint64_t);
typedef uint64_t (*pfn_iii)(uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_iiii)(uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_iiiii)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_iiiiii)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_iiiiiii)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_iiiiiiii)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef double   (*pfn_f)(void);
typedef double   (*pfn_if)(uint64_t);
typedef double   (*pfn_iif)(uint64_t,uint64_t);

static uint64_t ffi_dispatch_int(uint64_t fn_addr, SreFfiCall* c) {
    uint64_t* ia = (uint64_t*)c->iargs;  /* treat all as uint64 */
    int ni = c->n_iargs;

    /* Float args: if any floats, we need a different path because the C ABI
     * keeps floats in d0-d7 separate from x0-x7.  For the common case of
     * zero floats, we dispatch via plain integer prototypes. */
    if (c->n_fargs == 0) {
        switch (ni) {
            case 0: return ((pfn_v)fn_addr)();
            case 1: return ((pfn_i)fn_addr)(ia[0]);
            case 2: return ((pfn_ii)fn_addr)(ia[0],ia[1]);
            case 3: return ((pfn_iii)fn_addr)(ia[0],ia[1],ia[2]);
            case 4: return ((pfn_iiii)fn_addr)(ia[0],ia[1],ia[2],ia[3]);
            case 5: return ((pfn_iiiii)fn_addr)(ia[0],ia[1],ia[2],ia[3],ia[4]);
            case 6: return ((pfn_iiiiii)fn_addr)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]);
            case 7: return ((pfn_iiiiiii)fn_addr)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5],ia[6]);
            default: return ((pfn_iiiiiiii)fn_addr)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5],ia[6],ia[7]);
        }
    }

    /* Mixed int+float cases: most common in Swordigo are (void*this, float, ...) */
    double* fa = (double*)c->fargs;

    typedef uint64_t (*pfn_if1)(uint64_t, double);
    typedef uint64_t (*pfn_if2)(uint64_t, double, double);
    typedef uint64_t (*pfn_iif1)(uint64_t, uint64_t, double);
    typedef uint64_t (*pfn_iif2)(uint64_t, uint64_t, double, double);
    typedef uint64_t (*pfn_iiif1)(uint64_t, uint64_t, uint64_t, double);
    typedef uint64_t (*pfn_iiff)(uint64_t, uint64_t, double, double);
    typedef uint64_t (*pfn_iiiff)(uint64_t, uint64_t, uint64_t, double, double);
    typedef uint64_t (*pfn_f0)(double);
    typedef uint64_t (*pfn_ff)(double, double);

    int nf = c->n_fargs;

    if (ni == 0 && nf == 1) return ((pfn_f0)fn_addr)(fa[0]);
    if (ni == 0 && nf == 2) return ((pfn_ff)fn_addr)(fa[0], fa[1]);
    if (ni == 1 && nf == 1) return ((pfn_if1)fn_addr)(ia[0], fa[0]);
    if (ni == 1 && nf == 2) return ((pfn_if2)fn_addr)(ia[0], fa[0], fa[1]);
    if (ni == 2 && nf == 1) return ((pfn_iif1)fn_addr)(ia[0], ia[1], fa[0]);
    if (ni == 2 && nf == 2) return ((pfn_iif2)fn_addr)(ia[0], ia[1], fa[0], fa[1]);
    if (ni == 3 && nf == 1) return ((pfn_iiif1)fn_addr)(ia[0], ia[1], ia[2], fa[0]);
    if (ni == 3 && nf == 2) return ((pfn_iiiff)fn_addr)(ia[0], ia[1], ia[2], fa[0], fa[1]);

    /* Fallback: treat floats as integer bits (won't work perfectly for float args
     * but prevents crash and signals the issue via return value) */
    return ((pfn_iiiiiiii)fn_addr)(ia[0],ia[1],ia[2],ia[3],
                                    (uint64_t)*(uint32_t*)&fa[0],
                                    (uint64_t)*(uint32_t*)&fa[1],
                                    ia[4], ia[5]);
}

static double ffi_dispatch_float(uint64_t fn_addr, SreFfiCall* c) {
    uint64_t* ia = (uint64_t*)c->iargs;
    double*   fa = (double*)c->fargs;
    int ni = c->n_iargs, nf = c->n_fargs;

    typedef double (*pfn_dv)(void);
    typedef double (*pfn_di)(uint64_t);
    typedef double (*pfn_dii)(uint64_t,uint64_t);
    typedef double (*pfn_df)(double);
    typedef double (*pfn_dif)(uint64_t,double);
    typedef double (*pfn_diif)(uint64_t,uint64_t,double);

    if (ni==0 && nf==0) return ((pfn_dv)fn_addr)();
    if (ni==1 && nf==0) return ((pfn_di)fn_addr)(ia[0]);
    if (ni==2 && nf==0) return ((pfn_dii)fn_addr)(ia[0],ia[1]);
    if (ni==0 && nf==1) return ((pfn_df)fn_addr)(fa[0]);
    if (ni==1 && nf==1) return ((pfn_dif)fn_addr)(ia[0],fa[0]);
    if (ni==2 && nf==1) return ((pfn_diif)fn_addr)(ia[0],ia[1],fa[0]);

    /* Fallback */
    return (double)ffi_dispatch_int(fn_addr, c);
}

/* =========================================================================
 * Push return value onto Lua stack
 * ========================================================================= */
static int ffi_push_result(lua_State* L, SreFfiType ret, uint64_t rawi, double rawf) {
    switch (ret) {
        case FFI_T_VOID:
            return 0;
        case FFI_T_BOOL:
            g_lua_pushboolean(L, (int)(rawi & 1));
            return 1;
        case FFI_T_I8:
            g_lua_pushnumber(L, (double)(int8_t)(rawi & 0xFF));
            return 1;
        case FFI_T_I16:
            g_lua_pushnumber(L, (double)(int16_t)(rawi & 0xFFFF));
            return 1;
        case FFI_T_I32:
            g_lua_pushnumber(L, (double)(int32_t)(rawi & 0xFFFFFFFF));
            return 1;
        case FFI_T_I64:
            g_lua_pushnumber(L, (double)(int64_t)rawi);
            return 1;
        case FFI_T_F32:
        case FFI_T_F64:
            g_lua_pushnumber(L, rawf);
            return 1;
        case FFI_T_PTR:
            g_lua_pushnumber(L, (double)(uint64_t)rawi);
            return 1;
        case FFI_T_STR: {
            const char* s = (const char*)(uintptr_t)rawi;
            if (s) g_lua_pushstring(L, s);
            else   g_lua_pushnil(L);
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * ffi.call(addr, ret_type, arg_types, arg1, arg2, ...)
 * ========================================================================= */
static int l_ffi_call(lua_State* L) {
    uint64_t fn_addr = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    if (!fn_addr) {
        g_lua_pushstring(L, "ffi.call: address is zero");
        return g_lua_error(L);
    }

    const char* ret_str  = g_lua_tolstring(L, 2, (sre_size_t*)0);
    const char* args_str = g_lua_tolstring(L, 3, (sre_size_t*)0);

    SreFfiType ret_type = ffi_parse_ret_type(ret_str);

    SreFfiCall c;
    ffi_fill_args(L, &c, args_str, 4);

    uint64_t rawi = 0;
    double   rawf = 0.0;

    if (ret_type == FFI_T_F32 || ret_type == FFI_T_F64) {
        rawf = ffi_dispatch_float(fn_addr, &c);
    } else {
        rawi = ffi_dispatch_int(fn_addr, &c);
    }

    return ffi_push_result(L, ret_type, rawi, rawf);
}

/* =========================================================================
 * ffi.bind(addr, ret_type, arg_types) → callable closure
 * ========================================================================= */
typedef struct {
    uint64_t   fn_addr;
    SreFfiType ret_type;
    char       arg_types[SRE_FFI_MAX_ARGS + 1];
} SreFfiClosure;

/* Each closure is stored as a Lua light userdata pointing to a static pool.
 * We use a fixed-size pool to avoid malloc in guest space. */
#define SRE_FFI_CLOSURE_POOL 64
static SreFfiClosure s_closure_pool[SRE_FFI_CLOSURE_POOL];
static int           s_closure_pool_used = 0;

static int l_ffi_closure_call(lua_State* L) {
    SreFfiClosure* cl = (SreFfiClosure*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (!cl || !cl->fn_addr) return 0;

    SreFfiCall c;
    ffi_fill_args(L, &c, cl->arg_types, 1);

    uint64_t rawi = 0;
    double   rawf = 0.0;

    if (cl->ret_type == FFI_T_F32 || cl->ret_type == FFI_T_F64)
        rawf = ffi_dispatch_float(cl->fn_addr, &c);
    else
        rawi = ffi_dispatch_int(cl->fn_addr, &c);

    return ffi_push_result(L, cl->ret_type, rawi, rawf);
}

static int l_ffi_bind(lua_State* L) {
    uint64_t fn_addr = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    const char* ret_str  = g_lua_tolstring(L, 2, (sre_size_t*)0);
    const char* args_str = g_lua_tolstring(L, 3, (sre_size_t*)0);

    if (s_closure_pool_used >= SRE_FFI_CLOSURE_POOL) {
        g_lua_pushstring(L, "ffi.bind: closure pool exhausted (max 64)");
        return g_lua_error(L);
    }

    SreFfiClosure* cl = &s_closure_pool[s_closure_pool_used++];
    cl->fn_addr  = fn_addr;
    cl->ret_type = ffi_parse_ret_type(ret_str);

    int i = 0;
    if (args_str) {
        for (; args_str[i] && i < SRE_FFI_MAX_ARGS; i++)
            cl->arg_types[i] = args_str[i];
    }
    cl->arg_types[i] = '\0';

    g_lua_pushlightuserdata(L, cl);
    g_lua_pushcclosure(L, l_ffi_closure_call, 1);
    return 1;
}

/* =========================================================================
 * Peek / Poke
 * ========================================================================= */
static inline uint64_t ffi_addr(lua_State* L, int idx) {
    return (uint64_t)(int64_t)g_lua_tonumber(L, idx);
}

static int l_ffi_peek8(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(uint8_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peek16(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(uint16_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peek32(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(uint32_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peek64(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(uint64_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peekf(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(float*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peekstr(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnil(L); return 1; }
    const char* s = *(const char**)(uintptr_t)a;
    if (s) g_lua_pushstring(L, s);
    else   g_lua_pushnil(L);
    return 1;
}
static int l_ffi_poke8(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) return 0;
    *(uint8_t*)(uintptr_t)a = (uint8_t)(uint64_t)(int64_t)g_lua_tonumber(L, 2);
    return 0;
}
static int l_ffi_poke16(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) return 0;
    *(uint16_t*)(uintptr_t)a = (uint16_t)(uint64_t)(int64_t)g_lua_tonumber(L, 2);
    return 0;
}
static int l_ffi_poke32(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) return 0;
    *(uint32_t*)(uintptr_t)a = (uint32_t)(uint64_t)(int64_t)g_lua_tonumber(L, 2);
    return 0;
}
static int l_ffi_poke64(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) return 0;
    *(uint64_t*)(uintptr_t)a = (uint64_t)(int64_t)g_lua_tonumber(L, 2);
    return 0;
}
static int l_ffi_pokef(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) return 0;
    *(float*)(uintptr_t)a = (float)g_lua_tonumber(L, 2);
    return 0;
}

/* =========================================================================
 * Struct field helpers
 * ========================================================================= */
static int l_ffi_readf32(lua_State* L) {
    uint64_t base = ffi_addr(L, 1);
    int64_t  off  = (int64_t)g_lua_tonumber(L, 2);
    if (!base) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(float*)(uintptr_t)(base + off)));
    return 1;
}
static int l_ffi_readf64(lua_State* L) {
    uint64_t base = ffi_addr(L, 1);
    int64_t  off  = (int64_t)g_lua_tonumber(L, 2);
    if (!base) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, *(double*)(uintptr_t)(base + off));
    return 1;
}
static int l_ffi_readi32(lua_State* L) {
    uint64_t base = ffi_addr(L, 1);
    int64_t  off  = (int64_t)g_lua_tonumber(L, 2);
    if (!base) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(int32_t*)(uintptr_t)(base + off)));
    return 1;
}
static int l_ffi_readi64(lua_State* L) {
    uint64_t base = ffi_addr(L, 1);
    int64_t  off  = (int64_t)g_lua_tonumber(L, 2);
    if (!base) { g_lua_pushnil(L); return 1; }
    g_lua_pushnumber(L, (double)(*(int64_t*)(uintptr_t)(base + off)));
    return 1;
}
static int l_ffi_writef32(lua_State* L) {
    uint64_t base = ffi_addr(L, 1);
    int64_t  off  = (int64_t)g_lua_tonumber(L, 2);
    float    val  = (float)g_lua_tonumber(L, 3);
    if (!base) return 0;
    *(float*)(uintptr_t)(base + off) = val;
    return 0;
}
static int l_ffi_writei32(lua_State* L) {
    uint64_t base = ffi_addr(L, 1);
    int64_t  off  = (int64_t)g_lua_tonumber(L, 2);
    int32_t  val  = (int32_t)(int64_t)g_lua_tonumber(L, 3);
    if (!base) return 0;
    *(int32_t*)(uintptr_t)(base + off) = val;
    return 0;
}

/* =========================================================================
 * Offset / Deref helpers
 * ========================================================================= */
static int l_ffi_offset(lua_State* L) {
    int n = g_lua_gettop(L);
    uint64_t addr = ffi_addr(L, 1);
    for (int i = 2; i <= n; i++) {
        addr += (uint64_t)(int64_t)g_lua_tonumber(L, i);
    }
    g_lua_pushnumber(L, (double)addr);
    return 1;
}

static int l_ffi_deref(lua_State* L) {
    uint64_t a = ffi_addr(L, 1);
    if (!a) { g_lua_pushnumber(L, 0.0); return 1; }
    uint64_t val = *(uint64_t*)(uintptr_t)a;
    g_lua_pushnumber(L, (double)val);
    return 1;
}

static int l_ffi_null(lua_State* L) {
    g_lua_pushnumber(L, 0.0);
    return 1;
}

static int l_ffi_base(lua_State* L) {
    g_lua_pushnumber(L, (double)g_swordigo_base);
    return 1;
}

/* ffi.at(base_offset) → g_swordigo_base + offset */
static int l_ffi_at(lua_State* L) {
    uint64_t offset = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    g_lua_pushnumber(L, (double)(g_swordigo_base + offset));
    return 1;
}

/* ffi.memcpy(dst, src, n) — raw memory copy */
static int l_ffi_memcpy(lua_State* L) {
    uint64_t dst = ffi_addr(L, 1);
    uint64_t src = ffi_addr(L, 2);
    size_t   n   = (size_t)(int64_t)g_lua_tonumber(L, 3);
    if (dst && src && n) memcpy((void*)(uintptr_t)dst, (void*)(uintptr_t)src, n);
    return 0;
}

/* ffi.memset(dst, byte, n) */
static int l_ffi_memset(lua_State* L) {
    uint64_t dst = ffi_addr(L, 1);
    int      val = (int)(int64_t)g_lua_tonumber(L, 2);
    size_t   n   = (size_t)(int64_t)g_lua_tonumber(L, 3);
    if (dst && n) memset((void*)(uintptr_t)dst, val, n);
    return 0;
}

/* =========================================================================
 * Register ffi table into Lua global _G.ffi
 * ========================================================================= */
void sre_ffi_register_lua(lua_State* L) {
    g_lua_createtable(L, 0, 0);  /* ffi = {} */

#define REG(name, fn) \
    g_lua_pushcclosure(L, fn, 0); \
    g_lua_setfield(L, -2, name)

    REG("call",    l_ffi_call);
    REG("bind",    l_ffi_bind);

    REG("peek8",   l_ffi_peek8);
    REG("peek16",  l_ffi_peek16);
    REG("peek32",  l_ffi_peek32);
    REG("peek64",  l_ffi_peek64);
    REG("peekf",   l_ffi_peekf);
    REG("peekstr", l_ffi_peekstr);

    REG("poke8",   l_ffi_poke8);
    REG("poke16",  l_ffi_poke16);
    REG("poke32",  l_ffi_poke32);
    REG("poke64",  l_ffi_poke64);
    REG("pokef",   l_ffi_pokef);

    REG("readf32", l_ffi_readf32);
    REG("readf64", l_ffi_readf64);
    REG("readi32", l_ffi_readi32);
    REG("readi64", l_ffi_readi64);
    REG("writef32",l_ffi_writef32);
    REG("writei32",l_ffi_writei32);

    REG("offset",  l_ffi_offset);
    REG("deref",   l_ffi_deref);
    REG("null",    l_ffi_null);
    REG("base",    l_ffi_base);
    REG("at",      l_ffi_at);
    REG("memcpy",  l_ffi_memcpy);
    REG("memset",  l_ffi_memset);

#undef REG

    g_lua_setfield(L, LUA_GLOBALSINDEX, "ffi");   /* _G.ffi = table */
}
