/* sre_lua.h — Lua API function pointer types for libsre.so
 *
 * These are resolved by the host from libswordigo.so's symbol table
 * and passed to libsre.so via sre_init_lua().
 *
 * The engine compiles Lua 5.1 as C++, so all symbols have C++ mangling.
 * We store function pointers here and call through them from our
 * ProgramState replacements.
 */

#ifndef SRE_LUA_H
#define SRE_LUA_H

#include "sre.h"

#ifndef lua_h
/* Lua 5.1 constants */
#define LUA_MULTRET     (-1)
#define LUA_YIELD       1
#define LUA_ERRRUN      2
#define LUA_ERRSYNTAX   3
#define LUA_ERRMEM      4
#define LUA_ERRERR      5

/* Lua type constants */
#define LUA_TNIL        0
#define LUA_TBOOLEAN    1
#define LUA_TNUMBER     3
#define LUA_TSTRING     4
#define LUA_TTABLE      5
#define LUA_TFUNCTION   6

/* Pseudo-indices */
#define LUA_GLOBALSINDEX (-10002)
#define LUA_REGISTRYINDEX (-10000)

/* Opaque types */
typedef void lua_State;
#endif

/* Function pointer types for the Lua API functions we need */
typedef int   (*pfn_lua_pcall)(lua_State* L, int nargs, int nresults, int errfunc);
typedef int   (*pfn_lua_resume)(lua_State* L, int narg);
typedef void  (*pfn_lua_settop)(lua_State* L, int idx);
typedef int   (*pfn_lua_gettop)(lua_State* L);
typedef const char* (*pfn_lua_tolstring)(lua_State* L, int idx, sre_size_t* len);
typedef void  (*pfn_lua_call)(lua_State* L, int nargs, int nresults);
typedef void  (*pfn_lua_pushstring)(lua_State* L, const char* s);
typedef void  (*pfn_lua_pushcclosure)(lua_State* L, int (*fn)(lua_State*), int n);
typedef void  (*pfn_lua_setfield)(lua_State* L, int idx, const char* k);
typedef void  (*pfn_lua_getfield)(lua_State* L, int idx, const char* k);
typedef void  (*pfn_lua_createtable)(lua_State* L, int narr, int nrec);
typedef void  (*pfn_lua_pushnumber)(lua_State* L, double n);
typedef void  (*pfn_lua_pushboolean)(lua_State* L, int b);
typedef void  (*pfn_lua_pushnil)(lua_State* L);
typedef double (*pfn_lua_tonumber)(lua_State* L, int idx);
typedef int   (*pfn_lua_toboolean)(lua_State* L, int idx);
typedef int   (*pfn_lua_type)(lua_State* L, int idx);
typedef void  (*pfn_luaL_register)(lua_State* L, const char* libname, const void* l);
typedef void* (*pfn_lua_touserdata)(lua_State* L, int idx);
typedef const void* (*pfn_lua_topointer)(lua_State* L, int idx);
typedef void  (*pfn_lua_pushlightuserdata)(lua_State* L, void* p);
typedef int   (*pfn_lua_error)(lua_State* L);

/* Global Lua function pointers — set by sre_init_lua() */
extern pfn_lua_pcall       g_lua_pcall;
extern pfn_lua_resume      g_lua_resume;
extern pfn_lua_settop      g_lua_settop;
extern pfn_lua_gettop      g_lua_gettop;
extern pfn_lua_tolstring   g_lua_tolstring;
extern pfn_lua_call        g_lua_call;
extern pfn_lua_pushstring  g_lua_pushstring;
extern pfn_lua_pushcclosure g_lua_pushcclosure;
extern pfn_lua_setfield    g_lua_setfield;
extern pfn_lua_getfield    g_lua_getfield;
extern pfn_lua_createtable g_lua_createtable;
extern pfn_lua_pushnumber  g_lua_pushnumber;
extern pfn_lua_pushboolean g_lua_pushboolean;
extern pfn_lua_pushnil     g_lua_pushnil;
extern pfn_lua_tonumber    g_lua_tonumber;
extern pfn_lua_toboolean   g_lua_toboolean;
extern pfn_lua_type        g_lua_type;
extern pfn_luaL_register   g_luaL_register;
extern pfn_lua_touserdata  g_lua_touserdata;
extern pfn_lua_topointer   g_lua_topointer;
extern pfn_lua_pushlightuserdata g_lua_pushlightuserdata;
extern pfn_lua_error       g_lua_error;

/* Extended Lua API function pointer types (resolved by sre_init_lua_ext) */
typedef void  (*pfn_lua_pushvalue)(lua_State* L, int idx);
typedef void  (*pfn_lua_remove)(lua_State* L, int idx);
typedef void  (*pfn_lua_insert)(lua_State* L, int idx);
typedef void  (*pfn_lua_replace)(lua_State* L, int idx);
typedef int   (*pfn_lua_checkstack)(lua_State* L, int sz);
typedef void  (*pfn_lua_rawget)(lua_State* L, int idx);
typedef void  (*pfn_lua_rawset)(lua_State* L, int idx);
typedef void  (*pfn_lua_rawgeti)(lua_State* L, int idx, int n);
typedef void  (*pfn_lua_rawseti)(lua_State* L, int idx, int n);
typedef int   (*pfn_lua_next)(lua_State* L, int idx);
typedef size_t(*pfn_lua_objlen)(lua_State* L, int idx);
typedef void  (*pfn_lua_settable)(lua_State* L, int idx);
typedef void  (*pfn_lua_gettable)(lua_State* L, int idx);
typedef int   (*pfn_lua_isnumber)(lua_State* L, int idx);
typedef int   (*pfn_lua_isstring)(lua_State* L, int idx);
typedef int64_t (*pfn_lua_tointeger)(lua_State* L, int idx);
typedef void  (*pfn_lua_pushinteger)(lua_State* L, int64_t n);
typedef void  (*pfn_lua_concat)(lua_State* L, int n);
typedef void  (*pfn_lua_pushlstring)(lua_State* L, const char* s, size_t len);
typedef int   (*pfn_lua_setmetatable)(lua_State* L, int idx);
typedef void  (*pfn_lua_close)(lua_State* L);
typedef int   (*pfn_lua_dump)(lua_State* L, int (*writer)(lua_State*, const void*, size_t, void*), void* data);
typedef int (*(*pfn_lua_atpanic)(lua_State* L, int (*panicf)(lua_State*)))(lua_State*);
typedef int   (*pfn_lua_getmetatable)(lua_State* L, int idx);
typedef int   (*pfn_lua_rawequal)(lua_State* L, int idx1, int idx2);
typedef int   (*pfn_lua_equal)(lua_State* L, int idx1, int idx2);
typedef int   (*pfn_lua_lessthan)(lua_State* L, int idx1, int idx2);
typedef int   (*pfn_lua_isuserdata)(lua_State* L, int idx);

typedef void       (*pfn_lua_xmove)(lua_State* from, lua_State* to, int n);
typedef lua_State* (*pfn_lua_newthread)(lua_State* L);
typedef int        (*pfn_lua_pushthread)(lua_State* L);
struct lua_Debug;
typedef void       (*lua_Hook)(lua_State *L, struct lua_Debug *ar);
typedef int        (*pfn_lua_sethook)(lua_State *L, lua_Hook func, int mask, int count);

/* Extended globals (set by sre_init_lua_ext) */
extern pfn_lua_xmove       g_lua_xmove;
extern pfn_lua_newthread   g_lua_newthread;
extern pfn_lua_pushthread  g_lua_pushthread;
extern pfn_lua_sethook     g_lua_sethook;
extern pfn_lua_pushvalue   g_lua_pushvalue;
extern pfn_lua_remove      g_lua_remove;
extern pfn_lua_insert      g_lua_insert;
extern pfn_lua_replace     g_lua_replace;
extern pfn_lua_checkstack  g_lua_checkstack;
extern pfn_lua_rawget      g_lua_rawget;
extern pfn_lua_rawset      g_lua_rawset;
extern pfn_lua_rawgeti     g_lua_rawgeti;
extern pfn_lua_rawseti     g_lua_rawseti;
extern pfn_lua_next        g_lua_next;
extern pfn_lua_objlen      g_lua_objlen;
extern pfn_lua_settable    g_lua_settable;
extern pfn_lua_gettable    g_lua_gettable;
extern pfn_lua_isnumber    g_lua_isnumber;
extern pfn_lua_isstring    g_lua_isstring;
extern pfn_lua_tointeger   g_lua_tointeger;
extern pfn_lua_pushinteger g_lua_pushinteger;
extern pfn_lua_concat      g_lua_concat;
extern pfn_lua_pushlstring g_lua_pushlstring;
extern pfn_lua_setmetatable g_lua_setmetatable;
extern pfn_lua_close       g_lua_close;
extern pfn_lua_dump        g_lua_dump;
extern pfn_lua_atpanic     g_lua_atpanic;
extern pfn_lua_getmetatable g_lua_getmetatable;
extern pfn_lua_rawequal    g_lua_rawequal;
extern pfn_lua_equal       g_lua_equal;
extern pfn_lua_lessthan    g_lua_lessthan;
extern pfn_lua_isuserdata  g_lua_isuserdata;

#ifndef lua_h
/* Convenience macros */
#define lua_tostring(L, idx) g_lua_tolstring(L, idx, (sre_size_t*)0)
#define lua_pop(L, n)        g_lua_settop(L, -(n)-1)
#define lua_upvalueindex(i)  (LUA_GLOBALSINDEX - (i))
#define lua_xmove            g_lua_xmove
#endif

/* ProgramState layout (ARM64, v1.4.12) */
#define PS_LUA_STATE     0x00  /* lua_State* L */
#define PS_COROUTINE     0x08  /* coroutine pointer */
#define PS_SCENE_OBJECT  0x20  /* SceneObject* */
#define PS_IS_SUSPENDED  0x48  /* int isSuspended */
#define PS_SLEEP_TIME    0x4c  /* float sleepTime */
#define PS_CONDITION1    0x51  /* bool condition1 */
#define PS_PAUSED        0x52  /* bool paused */
#define PS_COMPLETED     0x53  /* bool completed */
#define PS_SPEED_SCALING 0x54  /* float speedScaling */

/* Access ProgramState fields */
#define PS_GET(self, offset, type)  (*(type*)((char*)(self) + (offset)))
#define PS_SET(self, offset, type, val) (*(type*)((char*)(self) + (offset)) = (val))

typedef void (*pfn_ProgramState_destructor)(void* self);
extern pfn_ProgramState_destructor g_orig_ProgramState_destructor;
void sre_ProgramState_destructor(void* self);

void sre_open_swkiwi_libs(lua_State* L);

/* =========================================================================
 * Recovery stack — public API for cross-unit use
 * =========================================================================
 * Defined (non-static) in sre_lua.c; declared here so any SRE compilation
 * unit can call them by including sre_lua.h instead of needing bare externs.
 * Using bare "extern" in sre_scene_update.c caused these to resolve via the
 * bridge PLT stub (→ UNHANDLED → return 0) since they were previously static.
 */
int  recovery_push(lua_State* L);
void recovery_pop(int depth);

#endif /* SRE_LUA_H */
