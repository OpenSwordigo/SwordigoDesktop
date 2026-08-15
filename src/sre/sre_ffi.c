/* ============================================================
 * sre_ffi.c — Native FFI Lua module for SRE
 * ============================================================
 * Provides _G.ffi with direct ARM64 native call dispatch,
 * memory peek/poke, struct access, and function binding.
 *
 * Because SRE IS the ARM64 native process, we can call Swordigo
 * functions directly by casting function pointer addresses from
 * g_swordigo_base + offset. No JNI, no intermediate ABI layer.
 *
 * API (from Lua):
 *
 *   ffi.call(addr, ret_type, arg_types, ...)
 *     addr      : number  — absolute guest VA
 *     ret_type  : string  — "void","i32","i64","f32","f64","ptr","str"
 *                           or 1-char codes: 'v','i','l','f','d','p','s'
 *     arg_types : string  — e.g. "iif" = int,int,float
 *     ...                 — Lua values matching arg_types
 *   Returns: typed return value (or nothing for void)
 *
 *   local fn = ffi.bind(addr, ret_type, arg_types) → callable closure
 *
 *   ffi.peek8(addr)  ffi.peek16  ffi.peek32  ffi.peek64  ffi.peekf
 *   ffi.peekstr(addr)            — dereference char** → pushstring
 *   ffi.poke8(addr,v) ffi.poke16 ffi.poke32 ffi.poke64 ffi.pokef
 *
 *   ffi.readf32(base,off)  ffi.readf64  ffi.readi32  ffi.readi64
 *   ffi.writef32(base,off,v)   ffi.writei32(base,off,v)
 *
 *   ffi.offset(base, ...)  → base + sum(offsets)
 *   ffi.deref(addr)        → *(uint64_t*)addr   (follow pointer)
 *   ffi.null()             → 0
 *   ffi.base()             → g_swordigo_base
 *   ffi.at(offset)         → g_swordigo_base + offset
 *   ffi.memcpy(dst,src,n)
 *   ffi.memset(dst,byte,n)
 *
 * Type string chars:
 *   v=void  b=bool  c=i8  h=i16  i=i32  l=i64
 *   f=f32   d=f64   p=ptr s=cstr
 * ============================================================ */

#include "sre.h"
#include "sre_lua.h"
#include "sre_caver.h"

/* g_swordigo_base — base address of libswordigo.so in guest space */
extern uint64_t g_swordigo_base;

/* =========================================================================
 * Type system
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

static SreFfiType ffi_type_from_char(char c) {
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
    if (!s[1]) return ffi_type_from_char(s[0]);
    /* Multi-char keywords */
    const char* kws[] = { "void","bool","i8","i16","i32","i64","f32","f64","ptr","str" };
    SreFfiType  kwtypes[] = { FFI_T_VOID,FFI_T_BOOL,FFI_T_I8,FFI_T_I16,FFI_T_I32,
                               FFI_T_I64,FFI_T_F32,FFI_T_F64,FFI_T_PTR,FFI_T_STR };
    for (int i = 0; i < 10; i++) {
        const char* a = kws[i]; const char* b = s;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) return kwtypes[i];
    }
    return FFI_T_VOID;
}

/* =========================================================================
 * Argument packing
 * ========================================================================= */
typedef struct {
    SreFfiType arg_types[SRE_FFI_MAX_ARGS];
    int        n_args;
    uint64_t   iargs[8];   /* integer/ptr args (ARM64 x0–x7) */
    double     fargs[8];   /* float args (ARM64 d0–d7) */
    int        n_iargs;
    int        n_fargs;
} SreFfiCall;

static void ffi_fill_args(lua_State* L, SreFfiCall* c,
                           const char* types_str, int base_idx)
{
    c->n_args = 0; c->n_iargs = 0; c->n_fargs = 0;
    if (!types_str) return;

    for (int i = 0; types_str[i] && c->n_args < SRE_FFI_MAX_ARGS; i++) {
        SreFfiType t = ffi_type_from_char(types_str[i]);
        c->arg_types[c->n_args++] = t;
        int li = base_idx + i;

        switch (t) {
            case FFI_T_VOID: break;
            case FFI_T_BOOL:
                if (c->n_iargs < 8)
                    c->iargs[c->n_iargs++] = g_lua_toboolean ? (uint64_t)g_lua_toboolean(L, li) : 0;
                break;
            case FFI_T_I8: case FFI_T_I16: case FFI_T_I32: case FFI_T_I64:
                if (c->n_iargs < 8)
                    c->iargs[c->n_iargs++] = (uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L, li) : 0.0);
                break;
            case FFI_T_F32:
                if (c->n_fargs < 8)
                    c->fargs[c->n_fargs++] = (double)(float)(g_lua_tonumber ? g_lua_tonumber(L, li) : 0.0);
                break;
            case FFI_T_F64:
                if (c->n_fargs < 8)
                    c->fargs[c->n_fargs++] = (g_lua_tonumber ? g_lua_tonumber(L, li) : 0.0);
                break;
            case FFI_T_PTR: {
                if (c->n_iargs < 8) {
                    int lt = g_lua_type ? g_lua_type(L, li) : 0;
                    if (lt == LUA_TLIGHTUSERDATA)
                        c->iargs[c->n_iargs++] = (uint64_t)(uintptr_t)(g_lua_touserdata ? g_lua_touserdata(L, li) : (void*)0);
                    else
                        c->iargs[c->n_iargs++] = (uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L, li) : 0.0);
                }
                break;
            }
            case FFI_T_STR:
                if (c->n_iargs < 8) {
                    const char* sv = g_lua_tolstring ? g_lua_tolstring(L, li, 0) : (const char*)0;
                    c->iargs[c->n_iargs++] = (uint64_t)(uintptr_t)sv;
                }
                break;
        }
    }
}

/* =========================================================================
 * ARM64 call dispatch
 *
 * ARM64 EABI: integer args in x0–x7, float args in d0–d7.
 * We emit typed function pointer casts so the compiler puts args in the
 * right registers. Covers all common Swordigo API call shapes.
 * ========================================================================= */
typedef uint64_t (*pfn_i0)(void);
typedef uint64_t (*pfn_i1)(uint64_t);
typedef uint64_t (*pfn_i2)(uint64_t,uint64_t);
typedef uint64_t (*pfn_i3)(uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_i4)(uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_i5)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_i6)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_i7)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
typedef uint64_t (*pfn_i8)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);

/* Mixed int+float */
typedef uint64_t (*pfn_f0v)(double);
typedef uint64_t (*pfn_f1v)(uint64_t,double);
typedef uint64_t (*pfn_f2v)(uint64_t,double,double);
typedef uint64_t (*pfn_if1)(uint64_t,uint64_t,double);
typedef uint64_t (*pfn_if2)(uint64_t,uint64_t,double,double);
typedef uint64_t (*pfn_iif1)(uint64_t,uint64_t,uint64_t,double);
typedef uint64_t (*pfn_iif2)(uint64_t,uint64_t,uint64_t,double,double);
typedef uint64_t (*pfn_iiif1)(uint64_t,uint64_t,uint64_t,uint64_t,double);

/* Float return */
typedef double (*pfn_dr0)(void);
typedef double (*pfn_dr1)(uint64_t);
typedef double (*pfn_dr2)(uint64_t,uint64_t);
typedef double (*pfn_drf1)(double);
typedef double (*pfn_drif1)(uint64_t,double);
typedef double (*pfn_driif1)(uint64_t,uint64_t,double);

static uint64_t ffi_dispatch_int(uint64_t fn, const SreFfiCall* c) {
    const uint64_t* ia = c->iargs;
    const double*   fa = c->fargs;
    int ni = c->n_iargs, nf = c->n_fargs;

    if (nf == 0) {
        switch (ni) {
            case 0: return ((pfn_i0)fn)();
            case 1: return ((pfn_i1)fn)(ia[0]);
            case 2: return ((pfn_i2)fn)(ia[0],ia[1]);
            case 3: return ((pfn_i3)fn)(ia[0],ia[1],ia[2]);
            case 4: return ((pfn_i4)fn)(ia[0],ia[1],ia[2],ia[3]);
            case 5: return ((pfn_i5)fn)(ia[0],ia[1],ia[2],ia[3],ia[4]);
            case 6: return ((pfn_i6)fn)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]);
            case 7: return ((pfn_i7)fn)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5],ia[6]);
            default: return ((pfn_i8)fn)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5],ia[6],ia[7]);
        }
    }
    if (ni==0 && nf==1) return ((pfn_f0v)fn)(fa[0]);
    if (ni==1 && nf==1) return ((pfn_f1v)fn)(ia[0],fa[0]);
    if (ni==1 && nf==2) return ((pfn_f2v)fn)(ia[0],fa[0],fa[1]);
    if (ni==2 && nf==1) return ((pfn_if1)fn)(ia[0],ia[1],fa[0]);
    if (ni==2 && nf==2) return ((pfn_if2)fn)(ia[0],ia[1],fa[0],fa[1]);
    if (ni==3 && nf==1) return ((pfn_iif1)fn)(ia[0],ia[1],ia[2],fa[0]);
    if (ni==3 && nf==2) return ((pfn_iif2)fn)(ia[0],ia[1],ia[2],fa[0],fa[1]);
    if (ni==4 && nf==1) return ((pfn_iiif1)fn)(ia[0],ia[1],ia[2],ia[3],fa[0]);

    /* Fallback: all-integer registers (float args won't marshal correctly
     * but at least no crash — user gets a debug warning via return value) */
    return ((pfn_i8)fn)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5],ia[6],ia[7]);
}

static double ffi_dispatch_float(uint64_t fn, const SreFfiCall* c) {
    const uint64_t* ia = c->iargs;
    const double*   fa = c->fargs;
    int ni = c->n_iargs, nf = c->n_fargs;

    if (ni==0 && nf==0) return ((pfn_dr0)fn)();
    if (ni==1 && nf==0) return ((pfn_dr1)fn)(ia[0]);
    if (ni==2 && nf==0) return ((pfn_dr2)fn)(ia[0],ia[1]);
    if (ni==0 && nf==1) return ((pfn_drf1)fn)(fa[0]);
    if (ni==1 && nf==1) return ((pfn_drif1)fn)(ia[0],fa[0]);
    if (ni==2 && nf==1) return ((pfn_driif1)fn)(ia[0],ia[1],fa[0]);

    return (double)ffi_dispatch_int(fn, c);
}

static int ffi_push_result(lua_State* L, SreFfiType ret, uint64_t ri, double rf) {
    switch (ret) {
        case FFI_T_VOID: return 0;
        case FFI_T_BOOL:
            if (g_lua_pushboolean) g_lua_pushboolean(L, (int)(ri & 1));
            return 1;
        case FFI_T_I8:
            if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(int8_t)(ri & 0xFF));
            return 1;
        case FFI_T_I16:
            if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(int16_t)(ri & 0xFFFF));
            return 1;
        case FFI_T_I32:
            if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(int32_t)(ri & 0xFFFFFFFF));
            return 1;
        case FFI_T_I64:
            if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(int64_t)ri);
            return 1;
        case FFI_T_F32:
        case FFI_T_F64:
            if (g_lua_pushnumber) g_lua_pushnumber(L, rf);
            return 1;
        case FFI_T_PTR:
            if (g_lua_pushnumber) g_lua_pushnumber(L, (double)ri);
            return 1;
        case FFI_T_STR: {
            const char* s = (const char*)(uintptr_t)ri;
            if (s) {
                if (g_lua_pushstring) g_lua_pushstring(L, s);
            } else {
                if (g_lua_pushnil) g_lua_pushnil(L);
            }
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * ffi.call(addr, ret_type, arg_types, ...)
 * ========================================================================= */
static int l_ffi_call(lua_State* L) {
    if (!g_lua_tonumber) return 0;
    uint64_t fn_addr = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    if (!fn_addr) return 0;

    const char* ret_str  = lua_tostring(L, 2);
    const char* args_str = lua_tostring(L, 3);

    SreFfiType ret_type = ffi_parse_ret_type(ret_str);

    SreFfiCall c;
    ffi_fill_args(L, &c, args_str, 4);

    uint64_t ri = 0;
    double   rf = 0.0;
    if (ret_type == FFI_T_F32 || ret_type == FFI_T_F64)
        rf = ffi_dispatch_float(fn_addr, &c);
    else
        ri = ffi_dispatch_int(fn_addr, &c);

    return ffi_push_result(L, ret_type, ri, rf);
}

/* =========================================================================
 * ffi.bind(addr, ret_type, arg_types) → closure
 * ========================================================================= */
#define SRE_FFI_CLOSURE_MAX 64
typedef struct {
    uint64_t   fn_addr;
    SreFfiType ret_type;
    char       arg_types[SRE_FFI_MAX_ARGS + 1];
} SreFfiClosure;

static SreFfiClosure s_closures[SRE_FFI_CLOSURE_MAX];
static int           s_closures_used = 0;

static int l_ffi_closure_call(lua_State* L) {
    SreFfiClosure* cl = (SreFfiClosure*)(g_lua_touserdata ? g_lua_touserdata(L, lua_upvalueindex(1)) : (void*)0);
    if (!cl || !cl->fn_addr) return 0;

    SreFfiCall c;
    ffi_fill_args(L, &c, cl->arg_types, 1);

    uint64_t ri = 0; double rf = 0.0;
    if (cl->ret_type == FFI_T_F32 || cl->ret_type == FFI_T_F64)
        rf = ffi_dispatch_float(cl->fn_addr, &c);
    else
        ri = ffi_dispatch_int(cl->fn_addr, &c);

    return ffi_push_result(L, cl->ret_type, ri, rf);
}

static int l_ffi_bind(lua_State* L) {
    if (!g_lua_tonumber) return 0;
    uint64_t fn_addr = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    const char* ret_str  = lua_tostring(L, 2);
    const char* args_str = lua_tostring(L, 3);

    if (s_closures_used >= SRE_FFI_CLOSURE_MAX) return 0;

    SreFfiClosure* cl = &s_closures[s_closures_used++];
    cl->fn_addr  = fn_addr;
    cl->ret_type = ffi_parse_ret_type(ret_str);

    int i = 0;
    if (args_str)
        for (; args_str[i] && i < SRE_FFI_MAX_ARGS; i++)
            cl->arg_types[i] = args_str[i];
    cl->arg_types[i] = '\0';

    /* Push closure userdata and create C closure */
    if (g_lua_pushlightuserdata) g_lua_pushlightuserdata(L, cl);
    if (g_lua_pushcclosure) g_lua_pushcclosure(L, l_ffi_closure_call, 1);
    return 1;
}

/* =========================================================================
 * Peek / Poke helpers
 * ========================================================================= */
static inline uint64_t ffi_lua_addr(lua_State* L, int idx) {
    if (!g_lua_tonumber) return 0;
    return (uint64_t)(int64_t)g_lua_tonumber(L, idx);
}

static int l_ffi_peek8(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(uint8_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peek16(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(uint16_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peek32(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(uint32_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peek64(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(uint64_t*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peekf(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(float*)(uintptr_t)a));
    return 1;
}
static int l_ffi_peekstr(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    const char* s = *(const char**)(uintptr_t)a;
    if (s) { if (g_lua_pushstring) g_lua_pushstring(L, s); }
    else   { if (g_lua_pushnil)    g_lua_pushnil(L); }
    return 1;
}

static int l_ffi_poke8(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1); if (!a) return 0;
    *(uint8_t*)(uintptr_t)a = (uint8_t)(uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    return 0;
}
static int l_ffi_poke16(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1); if (!a) return 0;
    *(uint16_t*)(uintptr_t)a = (uint16_t)(uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    return 0;
}
static int l_ffi_poke32(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1); if (!a) return 0;
    *(uint32_t*)(uintptr_t)a = (uint32_t)(uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    return 0;
}
static int l_ffi_poke64(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1); if (!a) return 0;
    *(uint64_t*)(uintptr_t)a = (uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    return 0;
}
static int l_ffi_pokef(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1); if (!a) return 0;
    *(float*)(uintptr_t)a = (float)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    return 0;
}

/* =========================================================================
 * Struct field helpers
 * ========================================================================= */
static int l_ffi_readf32(lua_State* L) {
    uint64_t b = ffi_lua_addr(L,1);
    int64_t  o = (int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    if (!b) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(float*)(uintptr_t)(b+o)));
    return 1;
}
static int l_ffi_readf64(lua_State* L) {
    uint64_t b = ffi_lua_addr(L,1);
    int64_t  o = (int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    if (!b) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, *(double*)(uintptr_t)(b+o));
    return 1;
}
static int l_ffi_readi32(lua_State* L) {
    uint64_t b = ffi_lua_addr(L,1);
    int64_t  o = (int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    if (!b) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(int32_t*)(uintptr_t)(b+o)));
    return 1;
}
static int l_ffi_readi64(lua_State* L) {
    uint64_t b = ffi_lua_addr(L,1);
    int64_t  o = (int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    if (!b) { if (g_lua_pushnil) g_lua_pushnil(L); return 1; }
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)(*(int64_t*)(uintptr_t)(b+o)));
    return 1;
}
static int l_ffi_writef32(lua_State* L) {
    uint64_t b = ffi_lua_addr(L,1); if (!b) return 0;
    int64_t  o = (int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    float    v = (float)(g_lua_tonumber ? g_lua_tonumber(L,3) : 0.0);
    *(float*)(uintptr_t)(b+o) = v;
    return 0;
}
static int l_ffi_writei32(lua_State* L) {
    uint64_t b = ffi_lua_addr(L,1); if (!b) return 0;
    int64_t  o = (int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    int32_t  v = (int32_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,3) : 0.0);
    *(int32_t*)(uintptr_t)(b+o) = v;
    return 0;
}

/* =========================================================================
 * Offset / Utility helpers
 * ========================================================================= */
static int l_ffi_offset(lua_State* L) {
    if (!g_lua_gettop || !g_lua_tonumber || !g_lua_pushnumber) return 0;
    int n = g_lua_gettop(L);
    uint64_t addr = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    for (int i = 2; i <= n; i++)
        addr += (uint64_t)(int64_t)g_lua_tonumber(L, i);
    g_lua_pushnumber(L, (double)addr);
    return 1;
}

static int l_ffi_deref(lua_State* L) {
    uint64_t a = ffi_lua_addr(L, 1);
    if (!a) { if (g_lua_pushnumber) g_lua_pushnumber(L, 0.0); return 1; }
    uint64_t val = *(uint64_t*)(uintptr_t)a;
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)val);
    return 1;
}

static int l_ffi_null(lua_State* L) {
    (void)L;
    if (g_lua_pushnumber) g_lua_pushnumber(L, 0.0);
    return 1;
}

static int l_ffi_base(lua_State* L) {
    (void)L;
    if (g_lua_pushnumber) g_lua_pushnumber(L, (double)g_swordigo_base);
    return 1;
}

static int l_ffi_at(lua_State* L) {
    if (!g_lua_tonumber || !g_lua_pushnumber) return 0;
    uint64_t offset = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    g_lua_pushnumber(L, (double)(g_swordigo_base + offset));
    return 1;
}

/* ffi.memcpy(dst, src, n) */
static int l_ffi_memcpy(lua_State* L) {
    uint64_t dst = ffi_lua_addr(L,1);
    uint64_t src = ffi_lua_addr(L,2);
    uint64_t n   = (uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,3) : 0.0);
    if (dst && src && n) {
        /* Use volatile char copy to avoid strict-aliasing opt issues */
        volatile char* d = (volatile char*)(uintptr_t)dst;
        const    char* s = (const    char*)(uintptr_t)src;
        for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    }
    return 0;
}

/* ffi.memset(dst, byte, n) */
static int l_ffi_memset(lua_State* L) {
    uint64_t dst = ffi_lua_addr(L,1);
    int      val = (int)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,2) : 0.0);
    uint64_t n   = (uint64_t)(int64_t)(g_lua_tonumber ? g_lua_tonumber(L,3) : 0.0);
    if (dst && n) {
        volatile char* d = (volatile char*)(uintptr_t)dst;
        for (uint64_t i = 0; i < n; i++) d[i] = (char)val;
    }
    return 0;
}

/* ffi.typeof(addr) → type name string for raw pointer (debug aid) */
static int l_ffi_typeof(lua_State* L) {
    /* On ARM64 we can't RTTI-inspect raw addresses, but we can give type info
     * based on Lua value type */
    if (!g_lua_type || !g_lua_pushstring) return 0;
    int t = g_lua_type(L, 1);
    switch (t) {
        case LUA_TNUMBER:       g_lua_pushstring(L, "number"); break;
        case LUA_TSTRING:       g_lua_pushstring(L, "string"); break;
        case LUA_TBOOLEAN:      g_lua_pushstring(L, "bool");   break;
        case LUA_TTABLE:        g_lua_pushstring(L, "table");  break;
        case LUA_TFUNCTION:     g_lua_pushstring(L, "function"); break;
        case LUA_TLIGHTUSERDATA:g_lua_pushstring(L, "ptr");    break;
        default:                g_lua_pushstring(L, "nil");    break;
    }
    return 1;
}

/* =========================================================================
 * Register _G.ffi
 * ========================================================================= */
__attribute__((visibility("hidden"))) void sre_ffi_register_lua(lua_State* L) {
    if (!g_lua_createtable || !g_lua_pushcclosure || !g_lua_setfield) return;

    g_lua_createtable(L, 0, 32);  /* ffi = {} */

#define FFI_REG(name, fn) \
    g_lua_pushcclosure(L, fn, 0); \
    g_lua_setfield(L, -2, name)

    FFI_REG("call",     l_ffi_call);
    FFI_REG("bind",     l_ffi_bind);

    FFI_REG("peek8",    l_ffi_peek8);
    FFI_REG("peek16",   l_ffi_peek16);
    FFI_REG("peek32",   l_ffi_peek32);
    FFI_REG("peek64",   l_ffi_peek64);
    FFI_REG("peekf",    l_ffi_peekf);
    FFI_REG("peekstr",  l_ffi_peekstr);

    FFI_REG("poke8",    l_ffi_poke8);
    FFI_REG("poke16",   l_ffi_poke16);
    FFI_REG("poke32",   l_ffi_poke32);
    FFI_REG("poke64",   l_ffi_poke64);
    FFI_REG("pokef",    l_ffi_pokef);

    FFI_REG("readf32",  l_ffi_readf32);
    FFI_REG("readf64",  l_ffi_readf64);
    FFI_REG("readi32",  l_ffi_readi32);
    FFI_REG("readi64",  l_ffi_readi64);
    FFI_REG("writef32", l_ffi_writef32);
    FFI_REG("writei32", l_ffi_writei32);

    FFI_REG("offset",   l_ffi_offset);
    FFI_REG("deref",    l_ffi_deref);
    FFI_REG("null",     l_ffi_null);
    FFI_REG("base",     l_ffi_base);
    FFI_REG("at",       l_ffi_at);
    FFI_REG("memcpy",   l_ffi_memcpy);
    FFI_REG("memset",   l_ffi_memset);
    FFI_REG("typeof",   l_ffi_typeof);

#undef FFI_REG

    /* Set as global _G.ffi */
    g_lua_setfield(L, LUA_GLOBALSINDEX, "ffi");
}
