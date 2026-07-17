/*
** $Id: lauxlib.c,v 1.158 2006/01/16 12:42:21 roberto Exp $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/


#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* This file uses only the official API of Lua.
** Any function declared here could be written as an application function.
*/

#define lauxlib_c
#define LUA_LIB

#include "lua.h"

#include "lauxlib.h"


#define FREELIST_REF	0	/* free list of references */


/* convert a stack index to positive */
#define abs_index(L, i)		((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : \
					lua_gettop(L) + (i) + 1)


/*
** {======================================================
** Error-report functions
** =======================================================
*/


LUALIB_API int luaL_argerror (lua_State *L, int narg, const char *extramsg) {
  lua_Debug ar;
  if (!lua_getstack(L, 0, &ar))  /* no stack frame? */
    return luaL_error(L, "bad argument #%d (%s)", narg, extramsg);
  lua_getinfo(L, "n", &ar);
  if (strcmp(ar.namewhat, "method") == 0) {
    narg--;  /* do not count `self' */
    if (narg == 0)  /* error is in the self argument itself? */
      return luaL_error(L, "calling " LUA_QS " on bad self (%s)",
                           ar.name, extramsg);
  }
  if (ar.name == NULL)
    ar.name = "?";
  return luaL_error(L, "bad argument #%d to " LUA_QS " (%s)",
                        narg, ar.name, extramsg);
}


LUALIB_API int luaL_typerror (lua_State *L, int narg, const char *tname) {
  const char *msg = lua_pushfstring(L, "%s expected, got %s",
                                    tname, luaL_typename(L, narg));
  return luaL_argerror(L, narg, msg);
}


static void tag_error (lua_State *L, int narg, int tag) {
  luaL_typerror(L, narg, lua_typename(L, tag));
}


LUALIB_API void luaL_where (lua_State *L, int level) {
  lua_Debug ar;
  if (lua_getstack(L, level, &ar)) {  /* check function at level */
    lua_getinfo(L, "Sl", &ar);  /* get info about it */
    if (ar.currentline > 0) {  /* is there info? */
      lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
      return;
    }
  }
  lua_pushliteral(L, "");  /* else, no information available... */
}


LUALIB_API int luaL_error (lua_State *L, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  luaL_where(L, 1);
  lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_concat(L, 2);
  return lua_error(L);
}

/* }====================================================== */


LUALIB_API int luaL_checkoption (lua_State *L, int narg, const char *def,
                                 const char *const lst[]) {
  const char *name = (def) ? luaL_optstring(L, narg, def) :
                             luaL_checkstring(L, narg);
  int i;
  for (i=0; lst[i]; i++)
    if (strcmp(lst[i], name) == 0)
      return i;
  return luaL_argerror(L, narg,
                       lua_pushfstring(L, "invalid option " LUA_QS, name));
}


LUALIB_API int luaL_newmetatable (lua_State *L, const char *tname) {
  lua_getfield(L, LUA_REGISTRYINDEX, tname);  /* get registry.name */
  if (!lua_isnil(L, -1))  /* name already in use? */
    return 0;  /* leave previous value on top, but return 0 */
  lua_pop(L, 1);
  lua_newtable(L);  /* create metatable */
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, tname);  /* registry.name = metatable */
  return 1;
}


LUALIB_API void *luaL_checkudata (lua_State *L, int ud, const char *tname) {
  void *p = lua_touserdata(L, ud);
  lua_getfield(L, LUA_REGISTRYINDEX, tname);  /* get correct metatable */
  if (p == NULL || !lua_getmetatable(L, ud) || !lua_rawequal(L, -1, -2))
    luaL_typerror(L, ud, tname);
  lua_pop(L, 2);  /* remove both metatables */
  return p;
}


LUALIB_API void luaL_checkstack (lua_State *L, int space, const char *mes) {
  if (!lua_checkstack(L, space))
    luaL_error(L, "stack overflow (%s)", mes);
}


LUALIB_API void luaL_checktype (lua_State *L, int narg, int t) {
  if (lua_type(L, narg) != t)
    tag_error(L, narg, t);
}


LUALIB_API void luaL_checkany (lua_State *L, int narg) {
  if (lua_type(L, narg) == LUA_TNONE)
    luaL_argerror(L, narg, "value expected");
}


LUALIB_API const char *luaL_checklstring (lua_State *L, int narg, size_t *len) {
  const char *s = lua_tolstring(L, narg, len);
  if (!s) tag_error(L, narg, LUA_TSTRING);
  return s;
}


LUALIB_API const char *luaL_optlstring (lua_State *L, int narg,
                                        const char *def, size_t *len) {
  if (lua_isnoneornil(L, narg)) {
    if (len)
      *len = (def ? strlen(def) : 0);
    return def;
  }
  else return luaL_checklstring(L, narg, len);
}


LUALIB_API lua_Number luaL_checknumber (lua_State *L, int narg) {
  lua_Number d = lua_tonumber(L, narg);
  if (d == 0 && !lua_isnumber(L, narg))  /* avoid extra test when d is not 0 */
    tag_error(L, narg, LUA_TNUMBER);
  return d;
}


LUALIB_API lua_Number luaL_optnumber (lua_State *L, int narg, lua_Number def) {
  return luaL_opt(L, luaL_checknumber, narg, def);
}


LUALIB_API lua_Integer luaL_checkinteger (lua_State *L, int narg) {
  lua_Integer d = lua_tointeger(L, narg);
  if (d == 0 && !lua_isnumber(L, narg))  /* avoid extra test when d is not 0 */
    tag_error(L, narg, LUA_TNUMBER);
  return d;
}


LUALIB_API lua_Integer luaL_optinteger (lua_State *L, int narg,
                                                      lua_Integer def) {
  return luaL_opt(L, luaL_checkinteger, narg, def);
}


LUALIB_API int luaL_getmetafield (lua_State *L, int obj, const char *event) {
  if (!lua_getmetatable(L, obj))  /* no metatable? */
    return 0;
  lua_pushstring(L, event);
  lua_rawget(L, -2);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 2);  /* remove metatable and metafield */
    return 0;
  }
  else {
    lua_remove(L, -2);  /* remove only metatable */
    return 1;
  }
}


LUALIB_API int luaL_callmeta (lua_State *L, int obj, const char *event) {
  obj = abs_index(L, obj);
  if (!luaL_getmetafield(L, obj, event))  /* no metafield? */
    return 0;
  lua_pushvalue(L, obj);
  lua_call(L, 1, 1);
  return 1;
}


LUALIB_API void (luaL_register) (lua_State *L, const char *libname,
                                const luaL_Reg *l) {
  luaI_openlib(L, libname, l, 0);
}


static int libsize (const luaL_Reg *l) {
  int size = 0;
  for (; l->name; l++) size++;
  return size;
}


LUALIB_API void luaI_openlib (lua_State *L, const char *libname,
                              const luaL_Reg *l, int nup) {
  if (libname) {
    int size = libsize(l);
    /* check whether lib already exists */
    luaL_findtable(L, LUA_REGISTRYINDEX, "_LOADED", size);
    lua_getfield(L, -1, libname);  /* get _LOADED[libname] */
    if (!lua_istable(L, -1)) {  /* not found? */
      lua_pop(L, 1);  /* remove previous result */
      /* try global variable (and create one if it does not exist) */
      if (luaL_findtable(L, LUA_GLOBALSINDEX, libname, size) != NULL)
        luaL_error(L, "name conflict for module " LUA_QS, libname);
      lua_pushvalue(L, -1);
      lua_setfield(L, -3, libname);  /* _LOADED[libname] = new table */
    }
    lua_remove(L, -2);  /* remove _LOADED table */
    lua_insert(L, -(nup+1));  /* move library table to below upvalues */
  }
  for (; l->name; l++) {
    int i;
    for (i=0; i<nup; i++)  /* copy upvalues to the top */
      lua_pushvalue(L, -nup);
    lua_pushcclosure(L, l->func, nup);
    lua_setfield(L, -(nup+2), l->name);
  }
  lua_pop(L, nup);  /* remove upvalues */
}



/*
** {======================================================
** getn-setn: size for arrays
** =======================================================
*/

#if defined(LUA_COMPAT_GETN)

static int checkint (lua_State *L, int topop) {
  int n = (lua_type(L, -1) == LUA_TNUMBER) ? lua_tointeger(L, -1) : -1;
  lua_pop(L, topop);
  return n;
}


static void getsizes (lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "LUA_SIZES");
  if (lua_isnil(L, -1)) {  /* no `size' table? */
    lua_pop(L, 1);  /* remove nil */
    lua_newtable(L);  /* create it */
    lua_pushvalue(L, -1);  /* `size' will be its own metatable */
    lua_setmetatable(L, -2);
    lua_pushliteral(L, "kv");
    lua_setfield(L, -2, "__mode");  /* metatable(N).__mode = "kv" */
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "LUA_SIZES");  /* store in register */
  }
}


LUALIB_API void luaL_setn (lua_State *L, int t, int n) {
  t = abs_index(L, t);
  lua_pushliteral(L, "n");
  lua_rawget(L, t);
  if (checkint(L, 1) >= 0) {  /* is there a numeric field `n'? */
    lua_pushliteral(L, "n");  /* use it */
    lua_pushinteger(L, n);
    lua_rawset(L, t);
  }
  else {  /* use `sizes' */
    getsizes(L);
    lua_pushvalue(L, t);
    lua_pushinteger(L, n);
    lua_rawset(L, -3);  /* sizes[t] = n */
    lua_pop(L, 1);  /* remove `sizes' */
  }
}


LUALIB_API int luaL_getn (lua_State *L, int t) {
  int n;
  t = abs_index(L, t);
  lua_pushliteral(L, "n");  /* try t.n */
  lua_rawget(L, t);
  if ((n = checkint(L, 1)) >= 0) return n;
  getsizes(L);  /* else try sizes[t] */
  lua_pushvalue(L, t);
  lua_rawget(L, -2);
  if ((n = checkint(L, 2)) >= 0) return n;
  return (int)lua_objlen(L, t);
}

#endif

/* }====================================================== */



LUALIB_API const char *luaL_gsub (lua_State *L, const char *s, const char *p,
                                                               const char *r) {
  const char *wild;
  size_t l = strlen(p);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  while ((wild = strstr(s, p)) != NULL) {
    luaL_addlstring(&b, s, wild - s);  /* push prefix */
    luaL_addstring(&b, r);  /* push replacement in place of pattern */
    s = wild + l;  /* continue after `p' */
  }
  luaL_addstring(&b, s);  /* push last suffix */
  luaL_pushresult(&b);
  return lua_tostring(L, -1);
}


LUALIB_API const char *luaL_findtable (lua_State *L, int idx,
                                       const char *fname, int szhint) {
  const char *e;
  lua_pushvalue(L, idx);
  do {
    e = strchr(fname, '.');
    if (e == NULL) e = fname + strlen(fname);
    lua_pushlstring(L, fname, e - fname);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {  /* no such field? */
      lua_pop(L, 1);  /* remove this nil */
      lua_createtable(L, 0, (*e == '.' ? 1 : szhint)); /* new table for field */
      lua_pushlstring(L, fname, e - fname);
      lua_pushvalue(L, -2);
      lua_settable(L, -4);  /* set new table into field */
    }
    else if (!lua_istable(L, -1)) {  /* field has a non-table value? */
      lua_pop(L, 2);  /* remove table and value */
      return fname;  /* return problematic part of the name */
    }
    lua_remove(L, -2);  /* remove previous table */
    fname = e + 1;
  } while (*e == '.');
  return NULL;
}



/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/


#define bufflen(B)	((B)->p - (B)->buffer)
#define bufffree(B)	((size_t)(LUAL_BUFFERSIZE - bufflen(B)))

#define LIMIT	(LUA_MINSTACK/2)


static int emptybuffer (luaL_Buffer *B) {
  size_t l = bufflen(B);
  if (l == 0) return 0;  /* put nothing on stack */
  else {
    lua_pushlstring(B->L, B->buffer, l);
    B->p = B->buffer;
    B->lvl++;
    return 1;
  }
}


static void adjuststack (luaL_Buffer *B) {
  if (B->lvl > 1) {
    lua_State *L = B->L;
    int toget = 1;  /* number of levels to concat */
    size_t toplen = lua_strlen(L, -1);
    do {
      size_t l = lua_strlen(L, -(toget+1));
      if (B->lvl - toget + 1 >= LIMIT || toplen > l) {
        toplen += l;
        toget++;
      }
      else break;
    } while (toget < B->lvl);
    lua_concat(L, toget);
    B->lvl = B->lvl - toget + 1;
  }
}


LUALIB_API char *luaL_prepbuffer (luaL_Buffer *B) {
  if (emptybuffer(B))
    adjuststack(B);
  return B->buffer;
}


LUALIB_API void luaL_addlstring (luaL_Buffer *B, const char *s, size_t l) {
  while (l--)
    luaL_addchar(B, *s++);
}


LUALIB_API void luaL_addstring (luaL_Buffer *B, const char *s) {
  luaL_addlstring(B, s, strlen(s));
}


LUALIB_API void luaL_pushresult (luaL_Buffer *B) {
  emptybuffer(B);
  lua_concat(B->L, B->lvl);
  B->lvl = 1;
}


LUALIB_API void luaL_addvalue (luaL_Buffer *B) {
  lua_State *L = B->L;
  size_t vl;
  const char *s = lua_tolstring(L, -1, &vl);
  if (vl <= bufffree(B)) {  /* fit into buffer? */
    memcpy(B->p, s, vl);  /* put it there */
    B->p += vl;
    lua_pop(L, 1);  /* remove from stack */
  }
  else {
    if (emptybuffer(B))
      lua_insert(L, -2);  /* put buffer before new value */
    B->lvl++;  /* add new value into B stack */
    adjuststack(B);
  }
}


LUALIB_API void luaL_buffinit (lua_State *L, luaL_Buffer *B) {
  B->L = L;
  B->p = B->buffer;
  B->lvl = 0;
}

/* }====================================================== */


LUALIB_API int luaL_ref (lua_State *L, int t) {
  int ref;
  t = abs_index(L, t);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);  /* remove from stack */
    return LUA_REFNIL;  /* `nil' has a unique fixed reference */
  }
  lua_rawgeti(L, t, FREELIST_REF);  /* get first free element */
  ref = (int)lua_tointeger(L, -1);  /* ref = t[FREELIST_REF] */
  lua_pop(L, 1);  /* remove it from stack */
  if (ref != 0) {  /* any free element? */
    lua_rawgeti(L, t, ref);  /* remove it from list */
    lua_rawseti(L, t, FREELIST_REF);  /* (t[FREELIST_REF] = t[ref]) */
  }
  else {  /* no free elements */
    ref = (int)lua_objlen(L, t);
    ref++;  /* create new reference */
  }
  lua_rawseti(L, t, ref);
  return ref;
}


LUALIB_API void luaL_unref (lua_State *L, int t, int ref) {
  if (ref >= 0) {
    t = abs_index(L, t);
    lua_rawgeti(L, t, FREELIST_REF);
    lua_rawseti(L, t, ref);  /* t[ref] = t[FREELIST_REF] */
    lua_pushinteger(L, ref);
    lua_rawseti(L, t, FREELIST_REF);  /* t[FREELIST_REF] = ref */
  }
}



/*
** {======================================================
** Load functions
** =======================================================
*/

typedef struct LoadF {
  int extraline;
  FILE *f;
  char buff[LUAL_BUFFERSIZE];
} LoadF;


static const char *getF (lua_State *L, void *ud, size_t *size) {
  LoadF *lf = (LoadF *)ud;
  (void)L;
  if (lf->extraline) {
    lf->extraline = 0;
    *size = 1;
    return "\n";
  }
  if (feof(lf->f)) return NULL;
  *size = fread(lf->buff, 1, LUAL_BUFFERSIZE, lf->f);
  return (*size > 0) ? lf->buff : NULL;
}


static int errfile (lua_State *L, const char *what, int fnameindex) {
  const char *serr = strerror(errno);
  const char *filename = lua_tostring(L, fnameindex) + 1;
  lua_pushfstring(L, "cannot %s %s: %s", what, filename, serr);
  lua_remove(L, fnameindex);
  return LUA_ERRFILE;
}


extern const char* sre_vfs_resolve_path(const char* path, char* out_buf);

LUALIB_API int luaL_loadfile (lua_State *L, const char *filename) {
  int fnameindex = lua_gettop(L) + 1;

  if (filename == NULL) {
    /* stdin: fall back to original streaming approach — no SCL handling needed */
    LoadF lf;
    int status, readstatus;
    lf.extraline = 0;
    lua_pushliteral(L, "=stdin");
    lf.f = stdin;
    status = lua_load(L, getF, &lf, lua_tostring(L, -1));
    readstatus = ferror(lf.f);
    if (readstatus) {
      lua_settop(L, fnameindex);
      return errfile(L, "read", fnameindex);
    }
    lua_remove(L, fnameindex);
    return status;
  }

  /* For named files: read entirely into memory so we can run SCL extraction.
   * luaL_loadbuffer already contains the protobuf + plain-text SCL detector. */
  char vfs_buf[512];
  const char* real_path = sre_vfs_resolve_path(filename, vfs_buf);
  lua_pushfstring(L, "@%s", filename);

  FILE* f = fopen(real_path, "rb");
  if (f == NULL) return errfile(L, "open", fnameindex);

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (fsize <= 0 || fsize > 4 * 1024 * 1024) {
    fclose(f);
    lua_pushfstring(L, "cannot load %s: %s", filename,
                    fsize <= 0 ? "empty file" : "file too large");
    lua_remove(L, fnameindex);
    return LUA_ERRFILE;
  }

  char* buf = (char*)malloc((size_t)fsize + 1);
  if (!buf) {
    fclose(f);
    lua_pushfstring(L, "cannot load %s: out of memory", filename);
    lua_remove(L, fnameindex);
    return LUA_ERRFILE;
  }

  size_t nread = fread(buf, 1, (size_t)fsize, f);
  fclose(f);
  buf[nread] = '\0';

  /* Build chunk name like original (@filename) for error messages */
  char chunkname[520];
  snprintf(chunkname, sizeof(chunkname), "@%s", filename);

  /* luaL_loadbuffer handles SCL binary protobuf + plain-text detection internally */
  int status = luaL_loadbuffer(L, buf, nread, chunkname);
  free(buf);

  lua_remove(L, fnameindex);
  return status;
}


typedef struct LoadS {
  const char *s;
  size_t size;
} LoadS;


static const char *getS (lua_State *L, void *ud, size_t *size) {
  LoadS *ls = (LoadS *)ud;
  (void)L;
  if (ls->size == 0) return NULL;
  *size = ls->size;
  ls->size = 0;
  return ls->s;
}


struct DumpBuffer {
    char* buf;
    size_t size;
    size_t cap;
};

static int writer_cb(lua_State* L, const void* p, size_t sz, void* ud) {
    struct DumpBuffer* db = (struct DumpBuffer*)ud;
    (void)L;
    if (db->size + sz > db->cap) {
        size_t new_cap = db->cap * 2 + sz + 1024;
        char* new_buf = (char*)realloc(db->buf, new_cap);
        if (!new_buf) return 1; /* error */
        db->buf = new_buf;
        db->cap = new_cap;
    }
    memcpy(db->buf + db->size, p, sz);
    db->size += sz;
    return 0;
}

#define ENABLE_SCRIPT_CACHE 0

static unsigned int fnv1a_hash(const char* data, size_t len) {
    unsigned int hash = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)data[i];
        hash *= 16777619U;
    }
    return hash;
}

static int extract_scl_recursive(const unsigned char* p, const unsigned char* end, const char** found_lua, size_t* found_len, int is_inside_program) {
  while (p < end) {
    uint64_t key = 0;
    int shift = 0;
    while (p < end) {
      unsigned char byte = *p++;
      key |= ((uint64_t)(byte & 0x7F)) << shift;
      if (!(byte & 0x80)) break;
      shift += 7;
      if (shift >= 64) return 0;
    }
    int field = (int)(key >> 3);
    int wire  = (int)(key & 7);

    if (wire == 2) {
      uint64_t len = 0;
      shift = 0;
      while (p < end) {
        unsigned char byte = *p++;
        len |= ((uint64_t)(byte & 0x7F)) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift >= 64) return 0;
      }
      if (p + len > end) return 0;

      if (is_inside_program) {
        if (field == 1 || field == 2 || field == 3) {
          *found_lua = (const char*)p;
          *found_len = (size_t)len;
          return 1;
        }
      } else {
        if (field == 5) {
          if (extract_scl_recursive(p, p + len, found_lua, found_len, 1)) {
            return 1;
          }
        } else {
          if (extract_scl_recursive(p, p + len, found_lua, found_len, 0)) {
            return 1;
          }
        }
      }
      p += len;
    } else if (wire == 0) {
      while (p < end && (*p++ & 0x80));
    } else if (wire == 1) {
      p += 8;
    } else if (wire == 5) {
      p += 4;
    } else {
      break;
    }
  }
  return 0;
}

LUALIB_API int luaL_loadbuffer (lua_State *L, const char *buff, size_t size,
                                const char *name) {
#if ENABLE_SCRIPT_CACHE
  /* Only cache non-empty buffers */
  if (buff && size > 0) {
      /* Generate a cache key based on the FNV-1a hash of the buffer content */
      unsigned int hash = fnv1a_hash(buff, size);
      char hash_key[32];
      snprintf(hash_key, sizeof(hash_key), "SRE_BC_%08X", hash);
      
      /* Look up cache in registry: registry["SRE_SCRIPT_CACHE"] */
      lua_pushstring(L, "SRE_SCRIPT_CACHE");
      lua_rawget(L, LUA_REGISTRYINDEX);
      if (lua_isnil(L, -1)) {
          /* Create the cache table if it doesn't exist */
          lua_pop(L, 1); /* pop nil */
          lua_newtable(L);
          lua_pushstring(L, "SRE_SCRIPT_CACHE");
          lua_pushvalue(L, -2);
          lua_rawset(L, LUA_REGISTRYINDEX);
      }
      
      /* Now cache table is at -1. Look up hash_key: cache[hash_key] */
      lua_pushstring(L, hash_key);
      lua_rawget(L, -2);
      if (!lua_isnil(L, -1)) {
          /* Found cached bytecode! Load it using lua_load */
          size_t bc_size;
          const char* bc_data = lua_tolstring(L, -1, &bc_size);
          LoadS ls;
          ls.s = bc_data;
          ls.size = bc_size;
          int status = lua_load(L, getS, &ls, name);
          if (status != 0) {
              fprintf(stderr, "[SRE-LUA-ERROR] Bytecode load failed: %d (name: '%s', msg: %s)\n", 
                      status, name ? name : "NULL", lua_tostring(L, -1));
          }
          
          /* Replace cache_table with the new closure and pop cached_val */
          lua_replace(L, -3);
          lua_pop(L, 1);
          return status;
      }
      
      /* Not found in cache. Pop the nil value */
      lua_pop(L, 1);
      
      /* Compile the buffer */
      LoadS ls;
      ls.s = buff;
      ls.size = size;
      int status = lua_load(L, getS, &ls, name);
      if (status == 0) {
          /* Dump the compiled closure to bytecode and store in cache */
          struct DumpBuffer db;
          db.buf = NULL;
          db.size = 0;
          db.cap = 0;
          if (lua_dump(L, writer_cb, &db) == 0) {
              lua_pushstring(L, hash_key);
              lua_pushlstring(L, db.buf, db.size);
              lua_rawset(L, -4); /* cache[hash_key] = bytecode_string */
          }
          free(db.buf);
      } else {
          fprintf(stderr, "[SRE-LUA-ERROR] Compile failed: %d (name: '%s', msg: %s)\n", 
                  status, name ? name : "NULL", lua_tostring(L, -1));
      }
      
      /* Remove the cache table from stack, leaving only result */
      lua_remove(L, -2);
      return status;
  }
#endif

  const char* actual_buff = buff;
  size_t actual_size = size;

  if (buff && size > 4) {
    unsigned char b0 = (unsigned char)buff[0];
    if (b0 != 0x1B && (b0 == 0x2A || (b0 >= 0x08 && (b0 & 7) <= 5))) {
      const char* found_lua = NULL;
      size_t found_len = 0;
      if (extract_scl_recursive((const unsigned char*)buff, (const unsigned char*)buff + size, &found_lua, &found_len, 0)) {
        if (found_lua && found_len > 0) {
          actual_buff = found_lua;
          actual_size = found_len;
        }
      }
    }

    /* Plain-text SCL format: Program{ String : $ <lua> $end } */
    if (actual_buff == buff) {
      /* Quick marker: look for "String : $" in first 256 bytes */
      size_t scan = size < 256 ? size : 256;
      for (size_t i = 0; i + 10 < scan; i++) {
        if (memcmp(buff + i, "String : $", 10) == 0) {
          const char* start = buff + i + 10;
          while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
          const char* send = strstr(start, "$end");
          if (send) {
            actual_buff = start;
            actual_size = (size_t)(send - start);
          }
          break;
        }
      }
    }
  }

  LoadS ls;
  ls.s = actual_buff;
  ls.size = actual_size;
  int status = lua_load(L, getS, &ls, name);
  return status;
}


LUALIB_API int (luaL_loadstring) (lua_State *L, const char *s) {
  return luaL_loadbuffer(L, s, strlen(s), s);
}



/* }====================================================== */


static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  (void)osize;
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  else
    return realloc(ptr, nsize);
}


static int panic (lua_State *L) {
  (void)L;  /* to avoid warnings */
  fprintf(stderr, "PANIC: unprotected error in call to Lua API (%s)\n",
                   lua_tostring(L, -1));
  return 0;
}


LUALIB_API lua_State *luaL_newstate (void) {
  lua_State *L = lua_newstate(l_alloc, NULL);
  if (L) {
    lua_atpanic(L, &panic);
    /* commented out new capture suspect
    extern lua_State* g_sre_last_lua_state;
    extern void sre_mini_ensure_injected(lua_State* L);
    g_sre_last_lua_state = L;
    */
  }
  return L;
}

