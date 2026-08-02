/*
 * sre_pack_lua.c
 * Pure C Binary serialization library for Lua (pack/unpack, encode/decode).
 * Supports both pack.encode(fmt, ...) / pack.decode(data, fmt) and pack.unpack(data, fmt).
 */

#include "sre_lua.h"
#include <stdint.h>
#include <string.h>

static void write_u8(uint8_t *buf, uint8_t v)  { buf[0] = v; }
static void write_u16(uint8_t *buf, uint16_t v) { buf[0] = v & 0xFF; buf[1] = (v >> 8) & 0xFF; }
static void write_u32(uint8_t *buf, uint32_t v) {
    buf[0] = v & 0xFF; buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF; buf[3] = (v >> 24) & 0xFF;
}

static uint8_t  read_u8 (const uint8_t *buf) { return buf[0]; }
static uint16_t read_u16(const uint8_t *buf) { return (uint16_t)(buf[0] | (buf[1] << 8)); }
static uint32_t read_u32(const uint8_t *buf) {
    return (uint32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
}

static void write_f32(uint8_t *buf, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    write_u32(buf, bits);
}

static float read_f32(const uint8_t *buf) {
    uint32_t bits = read_u32(buf);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

#define STACK_BUF_SIZE 512

static int l_pack_encode(lua_State *L) {
    if (!g_lua_tolstring || !g_lua_pushlstring) return 0;
    size_t fmt_len = 0;
    const char *fmt = g_lua_tolstring(L, 1, &fmt_len);
    if (!fmt) return 0;

    size_t needed = 0;
    int arg = 2;
    for (size_t i = 0; i < fmt_len; i++) {
        char c = fmt[i];
        if (c == 'b' || c == 'B') { needed += 1; arg++; }
        else if (c == 'h' || c == 'H') { needed += 2; arg++; }
        else if (c == 'i' || c == 'I' || c == 'f') { needed += 4; arg++; }
        else if (c == 'd') { needed += 8; arg++; }
        else if (c == 's') {
            size_t slen = 0;
            g_lua_tolstring(L, arg, &slen);
            needed += 2 + slen;
            arg++;
        }
    }

    uint8_t stack_buf[STACK_BUF_SIZE];
    uint8_t *buf = stack_buf;
    size_t pos = 0;
    arg = 2;
    for (size_t i = 0; i < fmt_len; i++) {
        char c = fmt[i];
        if (c == 'b' || c == 'B') {
            write_u8(buf + pos, (uint8_t)(g_lua_tointeger ? g_lua_tointeger(L, arg) : 0));
            pos += 1; arg++;
        } else if (c == 'h' || c == 'H') {
            write_u16(buf + pos, (uint16_t)(g_lua_tointeger ? g_lua_tointeger(L, arg) : 0));
            pos += 2; arg++;
        } else if (c == 'i' || c == 'I') {
            write_u32(buf + pos, (uint32_t)(g_lua_tointeger ? g_lua_tointeger(L, arg) : 0));
            pos += 4; arg++;
        } else if (c == 'f') {
            write_f32(buf + pos, (float)(g_lua_tonumber ? g_lua_tonumber(L, arg) : 0.0));
            pos += 4; arg++;
        } else if (c == 's') {
            size_t slen = 0;
            const char *str = g_lua_tolstring(L, arg, &slen);
            write_u16(buf + pos, (uint16_t)slen);
            pos += 2;
            if (str && slen > 0) memcpy(buf + pos, str, slen);
            pos += slen; arg++;
        }
    }

    g_lua_pushlstring(L, (const char*)buf, pos);
    return 1;
}

static int l_pack_decode(lua_State *L) {
    if (!g_lua_tolstring) return 0;

    size_t data_len = 0, fmt_len = 0;
    const char *p1 = g_lua_tolstring(L, 1, &data_len);
    const char *p2 = g_lua_tolstring(L, 2, &fmt_len);

    const char *data = p1;
    const char *fmt = p2;

    /* Detect argument order: pack.decode(data, fmt) vs pack.decode(fmt, data) */
    if (p1 && (p1[0] == 'b' || p1[0] == 'f' || p1[0] == 'h' || p1[0] == 'i') && p2 && data_len <= 16) {
        fmt = p1;
        fmt_len = strlen(p1);
        data = p2;
        data_len = strlen(p2);
    }

    if (!data || !fmt) return 0;

    size_t pos = 0;
    int return_count = 0;

    for (size_t i = 0; i < fmt_len && pos < data_len; i++) {
        char c = fmt[i];
        if (c == 'b' || c == 'B') {
            uint8_t val = read_u8((const uint8_t*)data + pos);
            pos += 1;
            if (g_lua_pushinteger) g_lua_pushinteger(L, val);
            return_count++;
        } else if (c == 'h' || c == 'H') {
            uint16_t val = read_u16((const uint8_t*)data + pos);
            pos += 2;
            if (g_lua_pushinteger) g_lua_pushinteger(L, val);
            return_count++;
        } else if (c == 'i' || c == 'I') {
            uint32_t val = read_u32((const uint8_t*)data + pos);
            pos += 4;
            if (g_lua_pushinteger) g_lua_pushinteger(L, val);
            return_count++;
        } else if (c == 'f') {
            float val = read_f32((const uint8_t*)data + pos);
            pos += 4;
            if (g_lua_pushnumber) g_lua_pushnumber(L, (double)val);
            return_count++;
        } else if (c == 's') {
            uint16_t slen = read_u16((const uint8_t*)data + pos);
            pos += 2;
            if (pos + slen <= data_len) {
                if (g_lua_pushlstring) g_lua_pushlstring(L, data + pos, slen);
                pos += slen;
                return_count++;
            }
        }
    }

    return return_count;
}

static const void* pack_lib[] = {
    (const void*)"encode", (const void*)l_pack_encode,
    (const void*)"decode", (const void*)l_pack_decode,
    (const void*)"pack",   (const void*)l_pack_encode,
    (const void*)"unpack", (const void*)l_pack_decode,
    (const void*)0,        (const void*)0
};

void sre_register_pack_lib(lua_State* L) {
    if (!g_luaL_register) return;
    g_luaL_register(L, "pack", (const void*)pack_lib);
    if (g_lua_settop) g_lua_settop(L, -2);
    g_luaL_register(L, "Pack", (const void*)pack_lib);
    if (g_lua_settop) g_lua_settop(L, -2);
}
