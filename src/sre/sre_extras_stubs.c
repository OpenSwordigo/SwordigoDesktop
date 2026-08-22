/*
 * sre_extras_stubs.c — stub Lua API for the optional libsre-extras addon.
 *
 * When libsre-extras.so is NOT loaded (or not built), SRE still exposes the
 * same Lua surface — Mini.MemoryAddress, Mini.GetAddress/Dlsym/Malloc/
 * GetComponentAddress, and ffi.call_sig — as safe stubs so mods written
 * against the extras API keep running:
 *   - reads return 0 / nil
 *   - writes are no-ops
 *   - Dlsym / GetComponentAddress / ffi.call_sig return nil (and log once)
 *   - Malloc / GetAddress still work (pure memory ops)
 *
 * When the host DOES load libsre-extras.so, it writes the extras'
 * miniLL_open_memory guest address into g_sre_extras_miniLL_open_memory and
 * sre_register_mini_api calls the real implementation instead of this file.
 */

#include "sre.h"
#include "sre_lua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Set by the host to the guest address of the extras' miniLL_open_memory()
 * when libsre-extras.so is present. 0 => use stubs. */
void* g_sre_extras_miniLL_open_memory = 0;

#define STUB_MT "Mini.MemoryAddress"

static int stub_log_once(void) {
    static int s_logged = 0;
    if (!s_logged) {
        s_logged = 1;
        printf("[SRE/Extras] libsre-extras.so not loaded — using stub memory/ffi API.\n");
    }
    return 1;
}

/* =========================================================================
 * Stub MemoryAddress userdata — read/write methods are safe no-ops.
 * ========================================================================= */
typedef struct {
    void* ptr;
} StubAddr;

static StubAddr* stub_check_addr(lua_State* L, int i) {
    if (!g_lua_isuserdata || !g_lua_touserdata || !g_lua_getmetatable ||
        !g_lua_getfield || !g_lua_rawequal || !g_lua_settop || !g_lua_type)
        return NULL;
    if (!g_lua_isuserdata(L, i)) return NULL;
    void* ud = g_lua_touserdata(L, i);
    if (g_lua_getmetatable(L, i)) {
        g_lua_getfield(L, LUA_REGISTRYINDEX, STUB_MT);
        int eq = g_lua_rawequal(L, -1, -2);
        g_lua_settop(L, -2);
        if (!eq) return NULL;
    } else {
        return NULL;
    }
    return (StubAddr*)ud;
}

static StubAddr* stub_push_addr(lua_State* L, void* ptr) {
    if (!g_lua_newuserdata || !g_lua_getfield || !g_lua_setmetatable) return NULL;
    StubAddr* addr = (StubAddr*)g_lua_newuserdata(L, sizeof(*addr));
    if (!addr) return NULL;
    addr->ptr = ptr;
    g_lua_getfield(L, LUA_REGISTRYINDEX, STUB_MT);
    g_lua_setmetatable(L, -2);
    return addr;
}

/* read* -> nil (unknown), write* -> no-op */
#define STUB_READ(name) \
    static int stub_read##name(lua_State* L) { (void)L; g_lua_pushnil(L); return 1; }
#define STUB_WRITE(name) \
    static int stub_write##name(lua_State* L) { (void)L; return 0; }
#define STUB_RW(name) STUB_READ(name) STUB_WRITE(name)

STUB_RW(Bool)
STUB_RW(Int8)  STUB_RW(Int16)  STUB_RW(Int32)  STUB_RW(Int64)
STUB_RW(UInt8) STUB_RW(UInt16) STUB_RW(UInt32) STUB_RW(UInt64)
STUB_RW(Float) STUB_RW(Double)
STUB_RW(Pointer) STUB_RW(CString) STUB_RW(CppString) STUB_RW(Vector3)

/* call — Raijin signature FFI: unavailable without the extras. */
static int stub_call(lua_State* L) {
    stub_log_once();
    g_lua_pushnil(L);
    return 1;
}

static int stub_free(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    if (addr && addr->ptr) {
        free(addr->ptr);
        addr->ptr = NULL;
    }
    return 0;
}

static int stub_add(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    if (!addr) { g_lua_pushnil(L); return 1; }
    stub_push_addr(L, (char*)addr->ptr + (ptrdiff_t)(int64_t)g_lua_tonumber(L, 2));
    return 1;
}

static int stub_sub(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    if (!addr) { g_lua_pushnil(L); return 1; }
    stub_push_addr(L, (char*)addr->ptr - (ptrdiff_t)(int64_t)g_lua_tonumber(L, 2));
    return 1;
}

static int stub_eq(lua_State* L) {
    StubAddr* a = stub_check_addr(L, 1);
    StubAddr* b = stub_check_addr(L, 2);
    g_lua_pushboolean(L, (a && b) && a->ptr == b->ptr);
    return 1;
}

static int stub_tostring(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    char buf[32];
    snprintf(buf, sizeof(buf), "%p", addr ? addr->ptr : NULL);
    g_lua_pushstring(L, buf);
    return 1;
}

static int stub_offset(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    if (!addr) { g_lua_pushnil(L); return 1; }
    stub_push_addr(L, (char*)addr->ptr + (ptrdiff_t)(int64_t)g_lua_tonumber(L, 2));
    return 1;
}

static int stub_getAddress(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    g_lua_pushlightuserdata(L, addr ? addr->ptr : NULL);
    return 1;
}

static int stub_isNull(lua_State* L) {
    StubAddr* addr = stub_check_addr(L, 1);
    g_lua_pushboolean(L, !addr || addr->ptr == NULL);
    return 1;
}

static int stub_gc(lua_State* L) { (void)L; return 0; }

/* =========================================================================
 * Mini.* library stubs
 * ========================================================================= */
static int stub_get_address(lua_State* L) {
    if (!g_lua_topointer) { g_lua_pushnil(L); return 1; }
    const void* ptr = g_lua_topointer(L, 1);
    if (!ptr) { g_lua_pushnil(L); return 1; }
    stub_push_addr(L, (void*)ptr);
    return 1;
}

static int stub_malloc(lua_State* L) {
    size_t size = (size_t)(int64_t)g_lua_tonumber(L, 1);
    void* ptr = NULL;
    if (size > 0) {
        ptr = malloc(size);
        if (ptr) memset(ptr, 0, size);
    }
    stub_push_addr(L, ptr);
    return 1;
}

static int stub_dlsym(lua_State* L) {
    stub_log_once();
    g_lua_pushnil(L);
    return 1;
}

static int stub_get_component_address(lua_State* L) {
    stub_log_once();
    g_lua_pushnil(L);
    return 1;
}

/* ffi.call_sig(addr, sig, ...) — unavailable without the extras. */
static int stub_ffi_call_sig(lua_State* L) {
    stub_log_once();
    g_lua_pushnil(L);
    return 1;
}

/* =========================================================================
 * Stub _G.ffi — registered UNCONDITIONALLY by SRE core (FFI is now a
 * closed-source feature living in libsre-extras.so). When the extras are
 * present, sre_ffi_register_lua() (in the extras module) overwrites this
 * table with the real libffi-backed one.
 *
 * Pure-math / introspection functions work for real (no dispatch, no
 * memory dereference beyond a plain deref). Everything that needs libffi,
 * allocation, mprotect, or dlopen is a safe stub: reads -> nil, writes ->
 * no-op, dispatch -> nil, dangerous ops -> no-op/false.
 * ========================================================================= */

extern uint64_t g_swordigo_base;

static uint64_t sffi_addr(lua_State* L, int idx) {
    return (uint64_t)(int64_t)g_lua_tonumber(L, idx);
}

/* --- real (safe) --- */
static int sffi_offset(lua_State* L) {
    int n = g_lua_gettop(L);
    uint64_t a = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    for (int i = 2; i <= n; i++) a += (uint64_t)(int64_t)g_lua_tonumber(L, i);
    g_lua_pushnumber(L, (double)a);
    return 1;
}
static int sffi_null(lua_State* L) { g_lua_pushnumber(L, 0.0); return 1; }
static int sffi_base(lua_State* L) { g_lua_pushnumber(L, (double)g_swordigo_base); return 1; }
static int sffi_at(lua_State* L) {
    uint64_t off = (uint64_t)(int64_t)g_lua_tonumber(L, 1);
    g_lua_pushnumber(L, (double)(g_swordigo_base + off));
    return 1;
}
static int sffi_addr_math(lua_State* L) {
    uint64_t p = sffi_addr(L, 1);
    int64_t  o = (int64_t)g_lua_tonumber(L, 2);
    g_lua_pushnumber(L, (double)(uint64_t)(p + o));
    return 1;
}
static int sffi_deref(lua_State* L) {
    uint64_t a = sffi_addr(L, 1);
    if (!a) { g_lua_pushnumber(L, 0.0); return 1; }
    g_lua_pushnumber(L, (double)(*(uint64_t*)(uintptr_t)a));
    return 1;
}
static int sffi_typeof(lua_State* L) {
    switch (g_lua_type(L, 1)) {
        case LUA_TNUMBER:        g_lua_pushstring(L, "number");   break;
        case LUA_TSTRING:        g_lua_pushstring(L, "string");   break;
        case LUA_TBOOLEAN:       g_lua_pushstring(L, "bool");     break;
        case LUA_TTABLE:         g_lua_pushstring(L, "table");    break;
        case LUA_TFUNCTION:      g_lua_pushstring(L, "function"); break;
        case LUA_TLIGHTUSERDATA: g_lua_pushstring(L, "ptr");      break;
        default:                 g_lua_pushstring(L, "nil");      break;
    }
    return 1;
}
static int sffi_tonumber(lua_State* L) { g_lua_pushnumber(L, g_lua_tonumber(L, 1)); return 1; }
static int sffi_tobool(lua_State* L)   { g_lua_pushboolean(L, g_lua_toboolean(L, 1)); return 1; }
static int sffi_errno(lua_State* L)    { g_lua_pushnumber(L, 0.0); return 1; }
static int sffi_abi(lua_State* L) {
    const char* q = g_lua_tolstring ? g_lua_tolstring(L, 1, 0) : 0;
    if (!q) {
#if defined(__aarch64__)
        g_lua_pushstring(L, "arm64");
#elif defined(__x86_64__)
        g_lua_pushstring(L, "x86_64");
#else
        g_lua_pushstring(L, "unknown");
#endif
        return 1;
    }
    int r = 0;
    if (!strcmp(q, "64bit")) r = (sizeof(void*) == 8);
    else if (!strcmp(q, "le")) { unsigned short x = 1; r = (*(unsigned char*)&x == 1); }
    else if (!strcmp(q, "be")) { unsigned short x = 1; r = (*(unsigned char*)&x == 0); }
    g_lua_pushboolean(L, r);
    return 1;
}
static int sffi_sizeof(lua_State* L) {
    const char* n = g_lua_tolstring ? g_lua_tolstring(L, 1, 0) : 0;
    if (!n) { g_lua_pushnil(L); return 1; }
    int w = 0;
    if (!strcmp(n,"i8")||!strcmp(n,"u8")||!strcmp(n,"bool")) w = 1;
    else if (!strcmp(n,"i16")||!strcmp(n,"u16")) w = 2;
    else if (!strcmp(n,"i32")||!strcmp(n,"u32")||!strcmp(n,"f32")||!strcmp(n,"float")||!strcmp(n,"int")) w = 4;
    else if (!strcmp(n,"i64")||!strcmp(n,"u64")||!strcmp(n,"f64")||!strcmp(n,"double")||!strcmp(n,"ptr")||!strcmp(n,"pointer")) w = 8;
    else if (!strcmp(n,"Vector2")) w = 8;
    else if (!strcmp(n,"Vector3")) w = 12;
    else if (!strcmp(n,"Quaternion")||!strcmp(n,"FloatColor")||!strcmp(n,"Rectangle")) w = 16;
    else if (!strcmp(n,"Matrix4")) w = 64;
    if (w) g_lua_pushnumber(L, (double)w); else g_lua_pushnil(L);
    return 1;
}

/* --- safe stubs: reads -> nil --- */
static int sffi_nil(lua_State* L)   { g_lua_pushnil(L); return 1; }
/* --- safe stubs: writes / dangerous -> no-op --- */
static int sffi_noop(lua_State* L)  { (void)L; return 0; }
/* --- safe stubs: dangerous -> false --- */
static int sffi_false(lua_State* L) { g_lua_pushboolean(L, 0); return 1; }
/* risky_mode: report always-off in the stub */
static int sffi_risky_mode(lua_State* L) { (void)L; g_lua_pushboolean(L, 0); return 1; }

static void sffi_set(lua_State* L, const char* name, int (*fn)(lua_State*)) {
    g_lua_pushcclosure(L, fn, 0);
    g_lua_setfield(L, -2, name);
}

void sre_extras_stub_register_ffi(lua_State* L) {
    if (!g_lua_createtable || !g_lua_pushcclosure || !g_lua_setfield) return;

    g_lua_createtable(L, 0, 48);

    /* --- pure-math / introspection: work for real --- */
    sffi_set(L, "offset",    sffi_offset);
    sffi_set(L, "null",      sffi_null);
    sffi_set(L, "base",      sffi_base);
    sffi_set(L, "at",        sffi_at);
    sffi_set(L, "addr",      sffi_addr_math);
    sffi_set(L, "deref",     sffi_deref);
    sffi_set(L, "typeof",    sffi_typeof);
    sffi_set(L, "tonumber",  sffi_tonumber);
    sffi_set(L, "tobool",    sffi_tobool);
    sffi_set(L, "errno",     sffi_errno);
    sffi_set(L, "abi",       sffi_abi);
    sffi_set(L, "sizeof",    sffi_sizeof);
    sffi_set(L, "alignment", sffi_sizeof);   /* same width table for simple types */

    /* --- dispatch: nil --- */
    sffi_set(L, "call",  sffi_nil);
    sffi_set(L, "bind",  sffi_nil);
    sffi_set(L, "cast",  sffi_nil);
    sffi_set(L, "new",   sffi_nil);
    sffi_set(L, "alloc", sffi_nil);
    sffi_set(L, "load",  sffi_nil);

    /* --- memory read: nil --- */
    sffi_set(L, "peek8",   sffi_nil);
    sffi_set(L, "peek16",  sffi_nil);
    sffi_set(L, "peek32",  sffi_nil);
    sffi_set(L, "peek64",  sffi_nil);
    sffi_set(L, "peekf",   sffi_nil);
    sffi_set(L, "peekstr", sffi_nil);
    sffi_set(L, "readf32", sffi_nil);
    sffi_set(L, "readf64", sffi_nil);
    sffi_set(L, "readi32", sffi_nil);
    sffi_set(L, "readi64", sffi_nil);
    sffi_set(L, "string",  sffi_nil);
    sffi_set(L, "tostring",sffi_nil);
    sffi_set(L, "search",  sffi_nil);

    /* --- memory write / no-ops --- */
    sffi_set(L, "poke8",    sffi_noop);
    sffi_set(L, "poke16",   sffi_noop);
    sffi_set(L, "poke32",   sffi_noop);
    sffi_set(L, "poke64",   sffi_noop);
    sffi_set(L, "pokef",    sffi_noop);
    sffi_set(L, "writef32", sffi_noop);
    sffi_set(L, "writei32", sffi_noop);
    sffi_set(L, "memcpy",   sffi_noop);
    sffi_set(L, "memset",   sffi_noop);
    sffi_set(L, "copy",     sffi_noop);
    sffi_set(L, "fill",     sffi_noop);
    sffi_set(L, "free",     sffi_noop);
    sffi_set(L, "dump",     sffi_noop);

    /* --- dangerous: gated off, report false --- */
    sffi_set(L, "risky_mode", sffi_risky_mode);
    sffi_set(L, "patch",      sffi_false);
    sffi_set(L, "seal",       sffi_false);
    sffi_set(L, "unseal",     sffi_false);

    g_lua_setfield(L, LUA_GLOBALSINDEX, "ffi");
}

/* =========================================================================
 * Stub implementations for extras mod_fs, mod_button_hooks, mod_saves.
 * These are weak symbols — when libsre-extras.so is loaded, the real
 * implementations override them.
 * ========================================================================= */

/* --- mod_fs.c stubs --- */
void sre_extras_fs_set_mod_dir(const char* dir) { (void)dir; }
void sre_extras_fs_register(lua_State* L) { (void)L; /* no-op: SRE's own fs stays */ }

/* --- mod_saves.c stubs --- */
void sre_extras_saves_init(const char* a, const char* b) { (void)a; (void)b; }
void sre_extras_init_saves(void) { /* no-op */ }
int  sre_extras_saves_active(void) { return 0; }
void* sre_extras_hook_byte_buffer_from_file(void* p, unsigned int* s) { (void)p; (void)s; return NULL; }
int  sre_extras_hook_save_byte_buffer_to_file(void* d, unsigned int s, void* p) { (void)d; (void)s; (void)p; return -1; }
int  sre_extras_hook_file_exists_at_path(void* p) { (void)p; return -1; }
void sre_extras_hook_delete_file_at_path(void* p) { (void)p; }

/* =========================================================================
 * Registration — Mini is expected at the TOP of the Lua stack.
 * ========================================================================= */
static void stub_set(lua_State* L, const char* name, int (*fn)(lua_State*)) {
    g_lua_pushcclosure(L, fn, 0);
    g_lua_setfield(L, -2, name);
}

void sre_extras_stub_register_lua(lua_State* L) {
    if (!g_lua_createtable || !g_lua_pushcclosure || !g_lua_setfield ||
        !g_lua_getfield || !g_lua_type || !g_lua_settop) return;

    /* ---- Mini.MemoryAddress metatable (registry) ---- */
    g_lua_getfield(L, LUA_REGISTRYINDEX, STUB_MT);
    if (g_lua_type(L, -1) == LUA_TNIL) {
        g_lua_settop(L, -2);
        g_lua_createtable(L, 0, 32);

        stub_set(L, "__gc",        stub_gc);
        stub_set(L, "__add",       stub_add);
        stub_set(L, "__sub",       stub_sub);
        stub_set(L, "__eq",        stub_eq);
        stub_set(L, "__tostring",  stub_tostring);

        stub_set(L, "offset",      stub_offset);
        stub_set(L, "getAddress",  stub_getAddress);
        stub_set(L, "isNull",      stub_isNull);
        stub_set(L, "free",        stub_free);

        stub_set(L, "readBool",    stub_readBool);
        stub_set(L, "writeBool",   stub_writeBool);
        stub_set(L, "readInt8",    stub_readInt8);
        stub_set(L, "writeInt8",   stub_writeInt8);
        stub_set(L, "readInt16",   stub_readInt16);
        stub_set(L, "writeInt16",  stub_writeInt16);
        stub_set(L, "readInt32",   stub_readInt32);
        stub_set(L, "writeInt32",  stub_writeInt32);
        stub_set(L, "readInt64",   stub_readInt64);
        stub_set(L, "writeInt64",  stub_writeInt64);
        stub_set(L, "readUInt8",   stub_readUInt8);
        stub_set(L, "writeUInt8",  stub_writeUInt8);
        stub_set(L, "readUInt16",  stub_readUInt16);
        stub_set(L, "writeUInt16", stub_writeUInt16);
        stub_set(L, "readUInt32",  stub_readUInt32);
        stub_set(L, "writeUInt32", stub_writeUInt32);
        stub_set(L, "readUInt64",  stub_readUInt64);
        stub_set(L, "writeUInt64", stub_writeUInt64);
        stub_set(L, "readFloat",   stub_readFloat);
        stub_set(L, "writeFloat",  stub_writeFloat);
        stub_set(L, "readDouble",  stub_readDouble);
        stub_set(L, "writeDouble", stub_writeDouble);
        stub_set(L, "readPointer",  stub_readPointer);
        stub_set(L, "writePointer", stub_writePointer);
        stub_set(L, "readCString",  stub_readCString);
        stub_set(L, "writeCString", stub_writeCString);
        stub_set(L, "readCppString",  stub_readCppString);
        stub_set(L, "writeCppString", stub_writeCppString);
        stub_set(L, "readVector3",  stub_readVector3);
        stub_set(L, "writeVector3", stub_writeVector3);
        stub_set(L, "call",        stub_call);

        /* mt.__index = mt */
        g_lua_pushvalue(L, -1);
        g_lua_setfield(L, -2, "__index");

        g_lua_pushvalue(L, -1);
        g_lua_setfield(L, LUA_REGISTRYINDEX, STUB_MT);
        /* Pop the metatable so Mini (below) is the top table again. */
        g_lua_settop(L, -2);
    } else {
        g_lua_settop(L, -2);
    }

    /* ---- Mini.* (table at top) ---- */
    stub_set(L, "GetAddress",           stub_get_address);
    stub_set(L, "Malloc",               stub_malloc);
    stub_set(L, "Dlsym",                stub_dlsym);
    stub_set(L, "GetComponentAddress",  stub_get_component_address);

    /* ---- ffi.call_sig (Raijin signature FFI stub) ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "ffi");
    if (g_lua_type(L, -1) == LUA_TTABLE) {
        stub_set(L, "call_sig", stub_ffi_call_sig);
    }
    g_lua_settop(L, -2);
}
