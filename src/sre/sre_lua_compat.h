#ifndef SRE_LUA_COMPAT_H
#define SRE_LUA_COMPAT_H

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "sre_lua.h"

/* Redirections for core Lua API functions */
#define lua_pcall g_lua_pcall
#define lua_resume g_lua_resume
#define lua_settop g_lua_settop
#define lua_gettop g_lua_gettop
#define lua_tolstring g_lua_tolstring
#define lua_call g_lua_call
#define lua_pushstring g_lua_pushstring
#define lua_pushcclosure g_lua_pushcclosure
#define lua_setfield g_lua_setfield
#define lua_getfield g_lua_getfield
#define lua_createtable g_lua_createtable
#define lua_pushnumber g_lua_pushnumber
#define lua_pushboolean g_lua_pushboolean
#define lua_pushnil g_lua_pushnil
#define lua_tonumber g_lua_tonumber
#define lua_toboolean g_lua_toboolean
#define lua_type g_lua_type
#define lua_touserdata g_lua_touserdata
#define lua_pushlightuserdata g_lua_pushlightuserdata
#define lua_error g_lua_error

/* Extended Lua API redirections */
#define lua_pushvalue g_lua_pushvalue
#define lua_remove g_lua_remove
#define lua_insert g_lua_insert
#define lua_replace g_lua_replace
#define lua_checkstack g_lua_checkstack
#define lua_rawget g_lua_rawget
#define lua_rawset g_lua_rawset
#define lua_rawgeti g_lua_rawgeti
#define lua_rawseti g_lua_rawseti
#define lua_next g_lua_next
#define lua_objlen g_lua_objlen
#define lua_settable g_lua_settable
#define lua_gettable g_lua_gettable
#define lua_isnumber g_lua_isnumber
#define lua_isstring g_lua_isstring
#define lua_tointeger g_lua_tointeger
#define lua_pushinteger g_lua_pushinteger
#define lua_concat g_lua_concat
#define lua_pushlstring g_lua_pushlstring
#define lua_setmetatable g_lua_setmetatable
#define lua_close g_lua_close
#define lua_dump g_lua_dump
#define lua_atpanic g_lua_atpanic
#define lua_getmetatable g_lua_getmetatable
#define lua_rawequal g_lua_rawequal
#define lua_equal g_lua_equal
#define lua_lessthan g_lua_lessthan
#define lua_isuserdata g_lua_isuserdata

/* Avoid duplicate macro warnings for standard helper macros */
#undef lua_tostring
#define lua_tostring(L, idx) g_lua_tolstring(L, (idx), (sre_size_t*)0)

#undef lua_pop
#define lua_pop(L, n)        g_lua_settop(L, -(n)-1)

#endif /* __ASSEMBLER__ */

#endif /* SRE_LUA_COMPAT_H */
