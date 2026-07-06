/*
 * sre_lua_libs.c — Lua 5.1 standard library implementations for SRE
 *
 * The vanilla engine only ships base + string Lua libs.
 * SwMini compiles the rest from Lua 5.1 source. We implement
 * the essential functions directly in SRE.
 *
 * Uses software math implementations since we compile with -nostdlib.
 * AArch64 hardware ops used for floor/ceil/sqrt/fabs.
 *
 * STATUS: API complete. RLSW still crashes with UC_ERR_WRITE_PROT at
 *         PC 0x14ee790 during gameplay — likely unpatched text-segment
 *         write triggered by RL's modified game data. See README.md.
 */

#include "sre.h"
#include "sre_lua.h"

extern void sre_log_lua_error(const char* source, const char* err_msg);

/* =========================================================================
 * Additional Lua API function pointers needed by standard libs
 * Types and externs are declared in sre_lua.h.
 * Globals are defined here, initialized by sre_init_lua_ext().
 * ========================================================================= */

/* Global extended function pointers */
pfn_lua_pushvalue      g_lua_pushvalue      = 0;
pfn_lua_remove         g_lua_remove         = 0;
pfn_lua_insert         g_lua_insert         = 0;
pfn_lua_replace        g_lua_replace        = 0;
pfn_lua_checkstack     g_lua_checkstack     = 0;
pfn_lua_rawget         g_lua_rawget         = 0;
pfn_lua_rawset         g_lua_rawset         = 0;
pfn_lua_rawgeti        g_lua_rawgeti        = 0;
pfn_lua_rawseti        g_lua_rawseti        = 0;
pfn_lua_next           g_lua_next           = 0;
pfn_lua_objlen         g_lua_objlen         = 0;
pfn_lua_settable       g_lua_settable       = 0;
pfn_lua_gettable       g_lua_gettable       = 0;
pfn_lua_isnumber       g_lua_isnumber       = 0;
pfn_lua_isstring       g_lua_isstring       = 0;
pfn_lua_tointeger      g_lua_tointeger      = 0;
pfn_lua_pushinteger    g_lua_pushinteger    = 0;
pfn_lua_concat         g_lua_concat         = 0;
pfn_lua_pushlstring    g_lua_pushlstring    = 0;
pfn_lua_setmetatable   g_lua_setmetatable   = 0;

/* Extended init — called from host after sre_init_lua */
typedef struct {
    sre_u64 lua_pushvalue;
    sre_u64 lua_remove;
    sre_u64 lua_insert;
    sre_u64 lua_replace;
    sre_u64 lua_checkstack;
    sre_u64 lua_rawget;
    sre_u64 lua_rawset;
    sre_u64 lua_rawgeti;
    sre_u64 lua_rawseti;
    sre_u64 lua_next;
    sre_u64 lua_objlen;
    sre_u64 lua_settable;
    sre_u64 lua_gettable;
    sre_u64 lua_isnumber;
    sre_u64 lua_isstring;
    sre_u64 lua_tointeger;
    sre_u64 lua_pushinteger;
    sre_u64 lua_concat;
    sre_u64 lua_pushlstring;
    sre_u64 lua_setmetatable;
} SreLuaExtAddrs;

void sre_init_lua_ext(SreLuaExtAddrs* a) {
    g_lua_pushvalue      = (pfn_lua_pushvalue)a->lua_pushvalue;
    g_lua_remove         = (pfn_lua_remove)a->lua_remove;
    g_lua_insert         = (pfn_lua_insert)a->lua_insert;
    g_lua_replace        = (pfn_lua_replace)a->lua_replace;
    g_lua_checkstack     = (pfn_lua_checkstack)a->lua_checkstack;
    g_lua_rawget         = (pfn_lua_rawget)a->lua_rawget;
    g_lua_rawset         = (pfn_lua_rawset)a->lua_rawset;
    g_lua_rawgeti        = (pfn_lua_rawgeti)a->lua_rawgeti;
    g_lua_rawseti        = (pfn_lua_rawseti)a->lua_rawseti;
    g_lua_next           = (pfn_lua_next)a->lua_next;
    g_lua_objlen         = (pfn_lua_objlen)a->lua_objlen;
    g_lua_settable       = (pfn_lua_settable)a->lua_settable;
    g_lua_gettable       = (pfn_lua_gettable)a->lua_gettable;
    g_lua_isnumber       = (pfn_lua_isnumber)a->lua_isnumber;
    g_lua_isstring       = (pfn_lua_isstring)a->lua_isstring;
    g_lua_tointeger      = (pfn_lua_tointeger)a->lua_tointeger;
    g_lua_pushinteger    = (pfn_lua_pushinteger)a->lua_pushinteger;
    g_lua_concat         = (pfn_lua_concat)a->lua_concat;
    g_lua_pushlstring    = (pfn_lua_pushlstring)a->lua_pushlstring;
    g_lua_setmetatable   = (pfn_lua_setmetatable)a->lua_setmetatable;
}


/* =========================================================================
 * External VFS globals from sre_vfs.c — for loadfile/dofile path translation
 * ========================================================================= */
extern volatile int g_sre_vfs_lua_translate_pending;
extern char g_sre_vfs_lua_input_path[];
extern char g_sre_vfs_lua_output_path[];
extern int  g_sre_vfs_active;


/* =========================================================================
 * GCC builtins for math — AArch64 maps these to hardware FPU instructions
 * ========================================================================= */
/* Hardware float ops — these map to single AArch64 instructions */
#define sre_floor(x)  __builtin_floor(x)
#define sre_ceil(x)   __builtin_ceil(x)
#define sre_sqrt(x)   __builtin_sqrt(x)
#define sre_fabs(x)   __builtin_fabs(x)
#define sre_huge      __builtin_huge_val()

/* fmod — a / b remainder (no libm call needed) */
static double sre_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - (double)((int64_t)(x / y)) * y;
}

/* Software sin/cos via Bhaskara approximation (accurate to ~0.001) */
static double sre_sin(double x) {
    /* Normalize to [-pi, pi] */
    while (x > 3.14159265358979323846) x -= 6.28318530717958647692;
    while (x < -3.14159265358979323846) x += 6.28318530717958647692;
    /* Bhaskara I formula: sin(x) ≈ 16x(pi-x) / (5pi² - 4x(pi-x)) */
    double pi = 3.14159265358979323846;
    double num = 16.0 * x * (pi - x);
    double den = 5.0 * pi * pi - 4.0 * x * (pi - x);
    if (den == 0.0) return 0.0;
    return num / den;
}

static double sre_cos(double x) {
    return sre_sin(x + 1.5707963267948966);  /* cos(x) = sin(x + pi/2) */
}

static double sre_tan(double x) {
    double c = sre_cos(x);
    if (c == 0.0) return sre_huge;
    return sre_sin(x) / c;
}

/* atan via polynomial approximation */
static double sre_atan(double x) {
    /* For |x| > 1: atan(x) = pi/2 - atan(1/x) */
    if (x > 1.0) return 1.5707963267948966 - sre_atan(1.0 / x);
    if (x < -1.0) return -1.5707963267948966 - sre_atan(1.0 / x);
    /* Polynomial: atan(x) ≈ x - x³/3 + x⁵/5 - x⁷/7 + x⁹/9 */
    double x2 = x * x;
    double x3 = x2 * x;
    return x - x3/3.0 + x3*x2/5.0 - x3*x2*x2/7.0 + x3*x2*x2*x2/9.0;
}

static double sre_atan2(double y, double x) {
    double pi = 3.14159265358979323846;
    if (x > 0) return sre_atan(y / x);
    if (x < 0 && y >= 0) return sre_atan(y / x) + pi;
    if (x < 0 && y < 0) return sre_atan(y / x) - pi;
    if (x == 0 && y > 0) return pi / 2.0;
    if (x == 0 && y < 0) return -pi / 2.0;
    return 0.0;
}

static double sre_asin(double x) {
    if (x >= 1.0) return 1.5707963267948966;
    if (x <= -1.0) return -1.5707963267948966;
    return sre_atan(x / sre_sqrt(1.0 - x * x));
}

static double sre_acos(double x) {
    return 1.5707963267948966 - sre_asin(x);
}

/* exp via Taylor series (13 terms — good for |x| < 20) */
static double sre_exp(double x) {
    if (x > 700.0) return sre_huge;
    if (x < -700.0) return 0.0;
    /* Range reduction: exp(x) = exp(n) * exp(f) where x = n + f */
    double result = 1.0;
    double term = 1.0;
    int i;
    for (i = 1; i <= 20; i++) {
        term *= x / (double)i;
        result += term;
        if (term < 1e-15 && term > -1e-15) break;
    }
    return result;
}

/* log via series: ln(x) = 2 * atanh((x-1)/(x+1)) */
static double sre_log(double x) {
    if (x <= 0.0) return -sre_huge;
    if (x == 1.0) return 0.0;
    /* Reduce to [0.5, 2] range */
    int exp_part = 0;
    while (x >= 2.0) { x *= 0.5; exp_part++; }
    while (x < 0.5) { x *= 2.0; exp_part--; }
    /* Series: ln(x) = 2 * sum((z^(2k+1))/(2k+1)) where z = (x-1)/(x+1) */
    double z = (x - 1.0) / (x + 1.0);
    double z2 = z * z;
    double sum = z;
    double term = z;
    int i;
    for (i = 1; i <= 15; i++) {
        term *= z2;
        sum += term / (double)(2 * i + 1);
    }
    return 2.0 * sum + (double)exp_part * 0.6931471805599453;  /* ln(2) */
}

static double sre_log10(double x) {
    return sre_log(x) * 0.4342944819032518;  /* 1/ln(10) */
}

/* pow via exp(y * ln(x)) */
static double sre_pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;
    if (x == 1.0) return 1.0;
    /* Integer power fast path */
    if (y == (double)(int64_t)y && y > 0 && y <= 64) {
        double result = 1.0;
        int n = (int)y;
        double base = x;
        while (n > 0) {
            if (n & 1) result *= base;
            base *= base;
            n >>= 1;
        }
        return result;
    }
    if (x < 0.0) return -sre_exp(y * sre_log(-x));
    return sre_exp(y * sre_log(x));
}

#define PI 3.14159265358979323846
#define RADIANS_PER_DEGREE (PI/180.0)

/* Simple xorshift64 PRNG for math.random */
static uint64_t rng_state = 0x123456789ABCDEF0ULL;

static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}


/* =========================================================================
 * math library
 * ========================================================================= */

static int math_abs(lua_State* L)   { g_lua_pushnumber(L, sre_fabs(g_lua_tonumber(L, 1))); return 1; }
static int math_floor(lua_State* L) { g_lua_pushnumber(L, sre_floor(g_lua_tonumber(L, 1))); return 1; }
static int math_ceil(lua_State* L)  { g_lua_pushnumber(L, sre_ceil(g_lua_tonumber(L, 1))); return 1; }
static int math_sqrt(lua_State* L)  { g_lua_pushnumber(L, sre_sqrt(g_lua_tonumber(L, 1))); return 1; }
static int math_sin(lua_State* L)   { g_lua_pushnumber(L, sre_sin(g_lua_tonumber(L, 1))); return 1; }
static int math_cos(lua_State* L)   { g_lua_pushnumber(L, sre_cos(g_lua_tonumber(L, 1))); return 1; }
static int math_tan(lua_State* L)   { g_lua_pushnumber(L, sre_tan(g_lua_tonumber(L, 1))); return 1; }
static int math_asin(lua_State* L)  { g_lua_pushnumber(L, sre_asin(g_lua_tonumber(L, 1))); return 1; }
static int math_acos(lua_State* L)  { g_lua_pushnumber(L, sre_acos(g_lua_tonumber(L, 1))); return 1; }
static int math_atan(lua_State* L)  { g_lua_pushnumber(L, sre_atan(g_lua_tonumber(L, 1))); return 1; }
static int math_log(lua_State* L)   { g_lua_pushnumber(L, sre_log(g_lua_tonumber(L, 1))); return 1; }
static int math_log10(lua_State* L) { g_lua_pushnumber(L, sre_log10(g_lua_tonumber(L, 1))); return 1; }
static int math_exp(lua_State* L)   { g_lua_pushnumber(L, sre_exp(g_lua_tonumber(L, 1))); return 1; }

static int math_atan2(lua_State* L) {
    g_lua_pushnumber(L, sre_atan2(g_lua_tonumber(L, 1), g_lua_tonumber(L, 2)));
    return 1;
}

static int math_pow(lua_State* L) {
    g_lua_pushnumber(L, sre_pow(g_lua_tonumber(L, 1), g_lua_tonumber(L, 2)));
    return 1;
}

static int math_fmod(lua_State* L) {
    g_lua_pushnumber(L, sre_fmod(g_lua_tonumber(L, 1), g_lua_tonumber(L, 2)));
    return 1;
}

static int math_deg(lua_State* L) {
    g_lua_pushnumber(L, g_lua_tonumber(L, 1) / RADIANS_PER_DEGREE);
    return 1;
}

static int math_rad(lua_State* L) {
    g_lua_pushnumber(L, g_lua_tonumber(L, 1) * RADIANS_PER_DEGREE);
    return 1;
}

static int math_max(lua_State* L) {
    int n = g_lua_gettop(L);
    if (n < 1) { g_lua_pushnumber(L, 0); return 1; }
    double m = g_lua_tonumber(L, 1);
    int i;
    for (i = 2; i <= n; i++) {
        double v = g_lua_tonumber(L, i);
        if (v > m) m = v;
    }
    g_lua_pushnumber(L, m);
    return 1;
}

static int math_min(lua_State* L) {
    int n = g_lua_gettop(L);
    if (n < 1) { g_lua_pushnumber(L, 0); return 1; }
    double m = g_lua_tonumber(L, 1);
    int i;
    for (i = 2; i <= n; i++) {
        double v = g_lua_tonumber(L, i);
        if (v < m) m = v;
    }
    g_lua_pushnumber(L, m);
    return 1;
}

static int math_random(lua_State* L) {
    int n = g_lua_gettop(L);
    uint64_t r = xorshift64();
    if (n == 0) {
        /* math.random() -> [0,1) */
        double d = (double)(r >> 11) * (1.0 / 9007199254740992.0);
        g_lua_pushnumber(L, d);
    } else if (n == 1) {
        /* math.random(n) -> [1,n] */
        int64_t upper = (int64_t)g_lua_tonumber(L, 1);
        if (upper < 1) upper = 1;
        g_lua_pushnumber(L, (double)(1 + (int64_t)(r % (uint64_t)upper)));
    } else {
        /* math.random(m, n) -> [m,n] */
        int64_t lower = (int64_t)g_lua_tonumber(L, 1);
        int64_t upper = (int64_t)g_lua_tonumber(L, 2);
        if (upper < lower) { int64_t t = lower; lower = upper; upper = t; }
        uint64_t range = (uint64_t)(upper - lower + 1);
        if (range == 0) range = 1;
        g_lua_pushnumber(L, (double)(lower + (int64_t)(r % range)));
    }
    return 1;
}

static int math_randomseed(lua_State* L) {
    rng_state = (uint64_t)g_lua_tonumber(L, 1);
    if (rng_state == 0) rng_state = 1;
    return 0;
}

/* luaL_Reg-compatible struct (matches Lua 5.1 layout) */
typedef struct { const char* name; int (*func)(lua_State*); } SreLuaReg;

static const SreLuaReg mathlib[] = {
    {"abs",        math_abs},
    {"acos",       math_acos},
    {"asin",       math_asin},
    {"atan",       math_atan},
    {"atan2",      math_atan2},
    {"ceil",       math_ceil},
    {"cos",        math_cos},
    {"deg",        math_deg},
    {"exp",        math_exp},
    {"floor",      math_floor},
    {"fmod",       math_fmod},
    {"log",        math_log},
    {"log10",      math_log10},
    {"max",        math_max},
    {"min",        math_min},
    {"pow",        math_pow},
    {"rad",        math_rad},
    {"random",     math_random},
    {"randomseed", math_randomseed},
    {"sin",        math_sin},
    {"sqrt",       math_sqrt},
    {"tan",        math_tan},
    {NULL, NULL}
};


/* =========================================================================
 * table library
 * ========================================================================= */

static int table_insert(lua_State* L) {
    int n = g_lua_gettop(L);
    if (!g_lua_objlen || !g_lua_rawgeti || !g_lua_rawseti) return 0;

    int tlen = (int)g_lua_objlen(L, 1);

    if (n == 2) {
        /* table.insert(t, val) — append at end */
        g_lua_rawseti(L, 1, tlen + 1);
    } else if (n >= 3) {
        /* table.insert(t, pos, val) */
        int pos = (int)g_lua_tonumber(L, 2);
        int i;
        /* Shift elements up */
        for (i = tlen; i >= pos; i--) {
            g_lua_rawgeti(L, 1, i);
            g_lua_rawseti(L, 1, i + 1);
        }
        /* Insert value at pos */
        if (g_lua_pushvalue) g_lua_pushvalue(L, 3);
        g_lua_rawseti(L, 1, pos);
    }
    return 0;
}

static int table_remove(lua_State* L) {
    if (!g_lua_objlen || !g_lua_rawgeti || !g_lua_rawseti) return 0;

    int tlen = (int)g_lua_objlen(L, 1);
    int pos = (g_lua_gettop(L) >= 2) ? (int)g_lua_tonumber(L, 2) : tlen;

    /* Get the removed value to return */
    g_lua_rawgeti(L, 1, pos);

    /* Shift elements down */
    int i;
    for (i = pos; i < tlen; i++) {
        g_lua_rawgeti(L, 1, i + 1);
        g_lua_rawseti(L, 1, i);
    }

    /* Remove last element */
    g_lua_pushnil(L);
    g_lua_rawseti(L, 1, tlen);

    return 1;
}

static int table_getn(lua_State* L) {
    if (!g_lua_objlen) { g_lua_pushnumber(L, 0); return 1; }
    g_lua_pushnumber(L, (double)g_lua_objlen(L, 1));
    return 1;
}

static int table_maxn(lua_State* L) {
    if (!g_lua_next) { g_lua_pushnumber(L, 0); return 1; }
    double max = 0;
    g_lua_pushnil(L);
    while (g_lua_next(L, 1) != 0) {
        lua_pop(L, 1);  /* pop value, keep key */
        if (g_lua_type(L, -1) == LUA_TNUMBER) {
            double v = g_lua_tonumber(L, -1);
            if (v > max) max = v;
        }
    }
    g_lua_pushnumber(L, max);
    return 1;
}

static int table_concat(lua_State* L) {
    if (!g_lua_objlen || !g_lua_rawgeti || !g_lua_concat) {
        g_lua_pushstring(L, "");
        return 1;
    }

    const char* sep = "";
    size_t seplen = 0;
    if (g_lua_gettop(L) >= 2 && g_lua_type(L, 2) == LUA_TSTRING) {
        sep = g_lua_tolstring(L, 2, &seplen);
    }

    int tlen = (int)g_lua_objlen(L, 1);
    int start = 1, end = tlen;
    if (g_lua_gettop(L) >= 3) start = (int)g_lua_tonumber(L, 3);
    if (g_lua_gettop(L) >= 4) end = (int)g_lua_tonumber(L, 4);

    if (start > end) {
        g_lua_pushstring(L, "");
        return 1;
    }

    /* Push all elements with separators, then concat */
    int count = 0;
    int i;
    for (i = start; i <= end; i++) {
        if (i > start && seplen > 0) {
            g_lua_pushstring(L, sep);
            count++;
        }
        g_lua_rawgeti(L, 1, i);
        count++;
    }
    if (count > 1) {
        g_lua_concat(L, count);
    } else if (count == 0) {
        g_lua_pushstring(L, "");
    }
    return 1;
}

/* table.sort — insertion sort (adequate for mod script data sizes) */
static int table_sort(lua_State* L) {
    if (!g_lua_objlen || !g_lua_rawgeti || !g_lua_rawseti) return 0;

    int n = (int)g_lua_objlen(L, 1);
    int has_cmp = (g_lua_gettop(L) >= 2 && g_lua_type(L, 2) == LUA_TFUNCTION);

    int i, j;
    for (i = 2; i <= n; i++) {
        g_lua_rawgeti(L, 1, i);  /* push key = t[i] */
        j = i - 1;
        while (j >= 1) {
            g_lua_rawgeti(L, 1, j);  /* push t[j] */
            int less;
            if (has_cmp) {
                if (g_lua_pushvalue) g_lua_pushvalue(L, 2);  /* push cmp */
                if (g_lua_pushvalue) g_lua_pushvalue(L, -3);  /* push key */
                if (g_lua_pushvalue) g_lua_pushvalue(L, -3);  /* push t[j] */
                g_lua_pcall(L, 2, 1, 0);
                less = g_lua_toboolean(L, -1);
                lua_pop(L, 1);  /* pop result */
            } else {
                double a = g_lua_tonumber(L, -2);  /* key */
                double b = g_lua_tonumber(L, -1);  /* t[j] */
                less = (a < b);
            }
            if (!less) {
                lua_pop(L, 1);  /* pop t[j] */
                break;
            }
            /* t[j+1] = t[j] */
            g_lua_rawseti(L, 1, j + 1);
            j--;
        }
        /* t[j+1] = key */
        g_lua_rawseti(L, 1, j + 1);
    }
    return 0;
}

static int table_unpack(lua_State* L) {
    if (!g_lua_objlen || !g_lua_rawgeti) return 0;

    int start = 1;
    int end = (int)g_lua_objlen(L, 1);
    if (g_lua_gettop(L) >= 2) start = (int)g_lua_tonumber(L, 2);
    if (g_lua_gettop(L) >= 3) end = (int)g_lua_tonumber(L, 3);

    int count = end - start + 1;
    if (count <= 0) return 0;
    if (count > 200) count = 200;  /* safety limit */

    int i;
    for (i = start; i <= start + count - 1; i++) {
        g_lua_rawgeti(L, 1, i);
    }
    return count;
}

static const SreLuaReg tablib[] = {
    {"concat",  table_concat},
    {"getn",    table_getn},
    {"insert",  table_insert},
    {"maxn",    table_maxn},
    {"remove",  table_remove},
    {"sort",    table_sort},
    {NULL, NULL}
};


/* =========================================================================
 * os library — real time/date/clock via libc
 * ========================================================================= */

/* libc time/date functions — resolved at load time via the bridge.
 * We don't #include <time.h> because the ARM64 cross-compiler uses -nostdlib.
 * These are resolved at load time just like the FILE* functions above. */


static int os_time(lua_State* L) {
    g_lua_pushnumber(L, (double)time(NULL));
    return 1;
}

static int os_clock(lua_State* L) {
    g_lua_pushnumber(L, (double)clock() / (double)CLOCKS_PER_SEC);
    return 1;
}

static int os_difftime(lua_State* L) {
    g_lua_pushnumber(L, g_lua_tonumber(L, 1) - g_lua_tonumber(L, 2));
    return 1;
}

static int os_date(lua_State* L) {
    const char* fmt = "%c";
    if (g_lua_gettop(L) >= 1 && g_lua_type(L, 1) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 1);
        if (s) fmt = s;
    }

    time_t t;
    if (g_lua_gettop(L) >= 2 && g_lua_type(L, 2) == LUA_TNUMBER) {
        t = (time_t)g_lua_tonumber(L, 2);
    } else {
        t = time(NULL);
    }

    /* Handle "*t" — return table */
    if (fmt[0] == '!' || (fmt[0] == '*' && fmt[1] == 't')) {
        int utc = 0;
        if (fmt[0] == '!') { utc = 1; fmt++; }
        if (fmt[0] == '*' && fmt[1] == 't') {
            struct tm* tm_p = utc ? gmtime(&t) : localtime(&t);
            if (!tm_p) { g_lua_pushnil(L); return 1; }
            g_lua_createtable(L, 0, 9);
            g_lua_pushnumber(L, (double)tm_p->tm_year + 1900);
            g_lua_setfield(L, -2, "year");
            g_lua_pushnumber(L, (double)tm_p->tm_mon + 1);
            g_lua_setfield(L, -2, "month");
            g_lua_pushnumber(L, (double)tm_p->tm_mday);
            g_lua_setfield(L, -2, "day");
            g_lua_pushnumber(L, (double)tm_p->tm_hour);
            g_lua_setfield(L, -2, "hour");
            g_lua_pushnumber(L, (double)tm_p->tm_min);
            g_lua_setfield(L, -2, "min");
            g_lua_pushnumber(L, (double)tm_p->tm_sec);
            g_lua_setfield(L, -2, "sec");
            g_lua_pushnumber(L, (double)tm_p->tm_wday + 1);
            g_lua_setfield(L, -2, "wday");
            g_lua_pushnumber(L, (double)tm_p->tm_yday + 1);
            g_lua_setfield(L, -2, "yday");
            g_lua_pushboolean(L, tm_p->tm_isdst);
            g_lua_setfield(L, -2, "isdst");
            return 1;
        }
    }

    /* Handle "!fmt" — use UTC */
    int utc = 0;
    if (fmt[0] == '!') { utc = 1; fmt++; }
    struct tm* tm_p = utc ? gmtime(&t) : localtime(&t);
    if (!tm_p) { g_lua_pushstring(L, ""); return 1; }

    char buf[256];
    size_t n = strftime(buf, sizeof(buf), fmt, tm_p);
    if (n > 0) {
        g_lua_pushlstring(L, buf, n);
    } else {
        g_lua_pushstring(L, "");
    }
    return 1;
}

static int os_exit(lua_State* L) { (void)L; return 0; }

/* Forward declaration — defined below near io_open */
static const char* sre_minipath_translate(const char* path, char* out, int outlen);

static int os_remove(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "os.remove: filename expected");
        return 2;
    }

    /* VFS path translation */
    const char* real_path = path;
    char vfs_buf[512];
    if (g_sre_vfs_active) {
        int i;
        for (i = 0; i < 511 && path[i]; i++)
            g_sre_vfs_lua_input_path[i] = path[i];
        g_sre_vfs_lua_input_path[i] = '\0';
        g_sre_vfs_lua_translate_pending = 1;
        if (g_sre_vfs_lua_output_path[0]) {
            /* Copy to local buffer since output_path may be reused */
            for (i = 0; i < 511 && g_sre_vfs_lua_output_path[i]; i++)
                vfs_buf[i] = g_sre_vfs_lua_output_path[i];
            vfs_buf[i] = '\0';
            real_path = vfs_buf;
        }
    } else {
        /* Fallback: try MiniPath translation when VFS is not active */
        real_path = sre_minipath_translate(path, vfs_buf, 512);
    }

    if (remove(real_path) == 0) {
        g_lua_pushboolean(L, 1);
        return 1;
    } else {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "cannot remove file");
        return 2;
    }
}

static int os_rename(lua_State* L) {
    const char* old_path = lua_tostring(L, 1);
    const char* new_path = lua_tostring(L, 2);
    if (!old_path || !new_path) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "os.rename: filenames expected");
        return 2;
    }
    char real_old[512];
    char real_new[512];
    old_path = sre_minipath_translate(old_path, real_old, 512);
    new_path = sre_minipath_translate(new_path, real_new, 512);
    if (rename(old_path, new_path) == 0) {
        g_lua_pushboolean(L, 1);
        return 1;
    } else {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "cannot rename file");
        return 2;
    }
}

static const SreLuaReg oslib[] = {
    {"clock",    os_clock},
    {"date",     os_date},
    {"difftime", os_difftime},
    {"exit",     os_exit},
    {"remove",   os_remove},
    {"rename",   os_rename},
    {"time",     os_time},
    {NULL, NULL}
};


/* =========================================================================
 * debug library (minimal stubs)
 * ========================================================================= */

static int debug_getinfo(lua_State* L) {
    g_lua_createtable(L, 0, 4);
    g_lua_pushstring(L, "Lua");
    g_lua_setfield(L, -2, "what");
    g_lua_pushstring(L, "?");
    g_lua_setfield(L, -2, "source");
    g_lua_pushnumber(L, 0);
    g_lua_setfield(L, -2, "currentline");
    g_lua_pushstring(L, "");
    g_lua_setfield(L, -2, "name");
    return 1;
}

static int debug_traceback(lua_State* L) {
    const char* msg = "";
    if (g_lua_gettop(L) >= 1 && g_lua_type(L, 1) == LUA_TSTRING) {
        msg = lua_tostring(L, 1);
    }
    g_lua_pushstring(L, msg ? msg : "traceback unavailable");
    return 1;
}

static int debug_sethook(lua_State* L) { (void)L; return 0; }

static const SreLuaReg dblib[] = {
    {"getinfo",   debug_getinfo},
    {"sethook",   debug_sethook},
    {"traceback", debug_traceback},
    {NULL, NULL}
};


/* =========================================================================
 * io library — working implementation for mod file I/O
 *
 * Mods like Combatch use io.open(path, mode) to save/load custom data.
 * Uses lightuserdata for FILE* handles, with method closures that
 * capture the handle as an upvalue. This avoids needing
 * lua_newuserdata/lua_setmetatable which aren't in our hook table.
 * ========================================================================= */
/* Forward declarations for C stdio/stdlib functions — we don't #include <stdio.h>
 * because the ARM64 cross-compiler sysroot uses -nostdlib.
 * These are resolved at load time via the bridge (bridge_fopen, bridge_malloc, etc.). */


/* Track open files for cleanup (max 32 simultaneous) */
static FILE* g_sre_open_files[32] = {0};
static int g_sre_open_file_count = 0;

static int sre_track_file(FILE* fp) {
    int i;
    for (i = 0; i < 32; i++) {
        if (!g_sre_open_files[i]) {
            g_sre_open_files[i] = fp;
            if (i >= g_sre_open_file_count) g_sre_open_file_count = i + 1;
            return 1;
        }
    }
    return 0;
}

static void sre_untrack_file(FILE* fp) {
    int i;
    for (i = 0; i < 32; i++) {
        if (g_sre_open_files[i] == fp) {
            g_sre_open_files[i] = NULL;
            break;
        }
    }
}

static FILE* sre_get_fp(lua_State* L, int idx) {
    if (!g_lua_touserdata) return NULL;
    return (FILE*)g_lua_touserdata(L, idx);
}

/* file:close() — upvalue 1 is the FILE* lightuserdata */
static int filemethod_close(lua_State* L) {
    FILE* fp = (FILE*)g_lua_touserdata(L, 1);
    if (!fp) {
        /* Try upvalue for method calls on the table */
        fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    }
    if (fp) {
        sre_untrack_file(fp);
        fclose(fp);
    }
    return 0;
}

/* file:read(fmt) */
static int filemethod_read(lua_State* L) {
    FILE* fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (!fp) { g_lua_pushnil(L); return 1; }

    const char* fmt = "*l";
    int count = 0;

    if (g_lua_gettop(L) >= 2) {
        if (g_lua_type(L, 2) == LUA_TNUMBER) {
            count = (int)g_lua_tonumber(L, 2);
        } else {
            const char* s = lua_tostring(L, 2);
            if (s) fmt = s;
        }
    }

    if (count > 0) {
        char* buf = (char*)malloc(count + 1);
        if (!buf) { g_lua_pushnil(L); return 1; }
        size_t n = fread(buf, 1, count, fp);
        if (n > 0) g_lua_pushlstring(L, buf, n);
        else g_lua_pushnil(L);
        free(buf);
        return 1;
    }

    if (fmt[0] == '*' && fmt[1] == 'a') {
        long pos = ftell(fp);
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, pos, SEEK_SET);
        long remaining = size - pos;
        if (remaining <= 0) { g_lua_pushstring(L, ""); return 1; }
        char* buf = (char*)malloc(remaining + 1);
        if (!buf) { g_lua_pushnil(L); return 1; }
        size_t n = fread(buf, 1, remaining, fp);
        buf[n] = '\0';
        g_lua_pushlstring(L, buf, n);
        free(buf);
        return 1;
    }

    if (fmt[0] == '*' && fmt[1] == 'n') {
        double val;
        if (fscanf(fp, "%lf", &val) == 1) g_lua_pushnumber(L, val);
        else g_lua_pushnil(L);
        return 1;
    }

    /* Default: read line */
    {
        char line[4096];
        if (fgets(line, sizeof(line), fp)) {
            size_t len = 0;
            while (line[len]) len++;
            if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
            if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
            g_lua_pushlstring(L, line, len);
        } else {
            g_lua_pushnil(L);
        }
        return 1;
    }
}

/* file:write(str, ...) */
static int filemethod_write(lua_State* L) {
    FILE* fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (!fp) { g_lua_pushnil(L); return 1; }

    int nargs = g_lua_gettop(L);
    int i;
    for (i = 2; i <= nargs; i++) {
        size_t len;
        const char* s = g_lua_tolstring(L, i, &len);
        if (s) fwrite(s, 1, len, fp);
    }
    g_lua_pushboolean(L, 1);
    return 1;
}

/* file:seek(whence, offset) */
static int filemethod_seek(lua_State* L) {
    FILE* fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (!fp) { g_lua_pushnil(L); return 1; }

    int whence = SEEK_CUR;
    long offset = 0;

    if (g_lua_gettop(L) >= 2) {
        const char* w = lua_tostring(L, 2);
        if (w) {
            if (w[0] == 's') whence = SEEK_SET;
            else if (w[0] == 'e') whence = SEEK_END;
        }
    }
    if (g_lua_gettop(L) >= 3) offset = (long)g_lua_tonumber(L, 3);

    fseek(fp, offset, whence);
    g_lua_pushnumber(L, (double)ftell(fp));
    return 1;
}

/* file:flush() */
static int filemethod_flush(lua_State* L) {
    FILE* fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (fp) fflush(fp);
    return 0;
}

/* file:lines() iterator */
static int filemethod_lines_iter(lua_State* L) {
    FILE* fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (!fp) { g_lua_pushnil(L); return 1; }
    char line[4096];
    if (fgets(line, sizeof(line), fp)) {
        size_t len = 0;
        while (line[len]) len++;
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
        g_lua_pushlstring(L, line, len);
        return 1;
    }
    g_lua_pushnil(L);
    return 1;
}

static int filemethod_lines(lua_State* L) {
    /* Get the fp from upvalue and create iterator with same fp */
    FILE* fp = (FILE*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (fp && g_lua_pushlightuserdata) {
        g_lua_pushlightuserdata(L, fp);
        g_lua_pushcclosure(L, filemethod_lines_iter, 1);
        return 1;
    }
    g_lua_pushnil(L);
    return 1;
}

/* MiniPath translation for io.open — translates Android virtual paths to desktop paths.
 * Uses g_sre_vfs_path_* globals from sre_vfs.c if set, otherwise falls back to
 * hardcoded ~/.local/share/swordigo-desktop/ paths. */
extern char g_sre_vfs_path_external[512];
extern char g_sre_vfs_path_files[512];
extern char g_sre_vfs_path_cache[512];
extern char g_sre_vfs_path_assets[512];

static int sre_str_starts_with(const char* s, const char* prefix) {
    int i;
    for (i = 0; prefix[i]; i++)
        if (s[i] != prefix[i]) return 0;
    return 1;
}

static const char* sre_minipath_translate(const char* path, char* out, int outlen) {
    if (!path || path[0] != '/') return path; /* Not a MiniPath */
    
    const char* base = 0;
    const char* rest = 0;
    
    if (sre_str_starts_with(path, "/ExternalFiles/")) {
        rest = path + 15;
        base = g_sre_vfs_path_external[0] ? g_sre_vfs_path_external : 0;
    } else if (sre_str_starts_with(path, "/Files/")) {
        rest = path + 7;
        base = g_sre_vfs_path_files[0] ? g_sre_vfs_path_files : 0;
    } else if (sre_str_starts_with(path, "/Cache/")) {
        rest = path + 7;
        base = g_sre_vfs_path_cache[0] ? g_sre_vfs_path_cache : 0;
    } else if (sre_str_starts_with(path, "/ExternalCache/")) {
        rest = path + 15;
        base = g_sre_vfs_path_cache[0] ? g_sre_vfs_path_cache : 0;
    } else if (sre_str_starts_with(path, "/Assets/")) {
        rest = path + 8;
        base = g_sre_vfs_path_assets[0] ? g_sre_vfs_path_assets : 0;
    } else {
        return path; /* Not a recognized MiniPath */
    }
    
    if (!base) return path; /* VFS path not set by host — pass through */
    
    /* Build: base + "/" + rest */
    {

        int i = 0, j;
        for (j = 0; i < outlen - 1 && base[j]; j++)
            out[i++] = base[j];
        /* Ensure trailing slash */
        if (i > 0 && out[i-1] != '/')
            out[i++] = '/';
        for (j = 0; i < outlen - 1 && rest[j]; j++)
            out[i++] = rest[j];
        out[i] = '\0';
        return out;
    }
    
    return path;
}

/* io.open(filename, mode) → file table or nil, errmsg */
static int io_open(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    const char* mode = "r";
    if (g_lua_gettop(L) >= 2) {
        const char* m = lua_tostring(L, 2);
        if (m) mode = m;
    }

    if (!path || !g_lua_pushlightuserdata) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "io.open: invalid path or missing APIs");
        return 2;
    }

    /* Translate MiniPaths (/ExternalFiles/, /Files/, /Cache/) to desktop paths */
    char minipath_buf[512];
    path = sre_minipath_translate(path, minipath_buf, 512);

    FILE* fp = fopen(path, mode);
    if (!fp) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "cannot open file");
        return 2;
    }

    sre_track_file(fp);

    /* Create a table to represent the file handle:
     * { close=fn, read=fn, write=fn, seek=fn, flush=fn, lines=fn, _fp=lightuserdata }
     * Each method closure captures fp as upvalue(1) via lightuserdata.
     */
    g_lua_createtable(L, 0, 8);

    /* Store the FILE* as lightuserdata in each closure's upvalue */
    g_lua_pushlightuserdata(L, fp);
    g_lua_pushcclosure(L, filemethod_close, 1);
    g_lua_setfield(L, -2, "close");

    g_lua_pushlightuserdata(L, fp);
    g_lua_pushcclosure(L, filemethod_read, 1);
    g_lua_setfield(L, -2, "read");

    g_lua_pushlightuserdata(L, fp);
    g_lua_pushcclosure(L, filemethod_write, 1);
    g_lua_setfield(L, -2, "write");

    g_lua_pushlightuserdata(L, fp);
    g_lua_pushcclosure(L, filemethod_seek, 1);
    g_lua_setfield(L, -2, "seek");

    g_lua_pushlightuserdata(L, fp);
    g_lua_pushcclosure(L, filemethod_flush, 1);
    g_lua_setfield(L, -2, "flush");

    g_lua_pushlightuserdata(L, fp);
    g_lua_pushcclosure(L, filemethod_lines, 1);
    g_lua_setfield(L, -2, "lines");

    return 1;  /* return the table */
}

/* io.close(file) — file is the table, get _fp and close */
static int io_close_fn(lua_State* L) {
    /* The file object is a table with a close method */
    if (g_lua_type(L, 1) == LUA_TTABLE) {
        g_lua_getfield(L, 1, "close");
        if (g_lua_type(L, -1) == LUA_TFUNCTION) {
            g_lua_call(L, 0, 0);
        } else {
            lua_pop(L, 1);
        }
    }
    return 0;
}

static int io_write_global(lua_State* L) { (void)L; return 0; }
static int io_read_global(lua_State* L) { g_lua_pushnil(L); return 1; }

/* io.type(obj) → "file" | "closed file" | nil */
static int io_type(lua_State* L) {
    if (g_lua_type(L, 1) == LUA_TTABLE) {
        /* Check if it has a "close" field (our file indicator) */
        g_lua_getfield(L, 1, "close");
        if (g_lua_type(L, -1) == LUA_TFUNCTION) {
            lua_pop(L, 1);
            g_lua_pushstring(L, "file");
            return 1;
        }
        lua_pop(L, 1);
    }
    g_lua_pushnil(L);
    return 1;
}

static const SreLuaReg iolib[] = {
    {"open",  io_open},
    {"close", io_close_fn},
    {"read",  io_read_global},
    {"write", io_write_global},
    {"type",  io_type},
    {NULL, NULL}
};


/* =========================================================================
 * loadfile/dofile — VFS-aware overrides (Task 2.6)
 *
 * Uses loadstring from Lua globals (same pattern as sre_run_console in
 * sre_lua.c) since we don't have a luaL_loadbuffer function pointer.
 * Flow: read file into buffer → loadstring(contents, chunkname)
 * ========================================================================= */

const char* sre_vfs_resolve_path(const char* path, char* out_buf) {
    if (!g_sre_vfs_active || !path) return path;

    /* Handle MiniPaths first */
    if (path[0] == '/') {
        return sre_minipath_translate(path, out_buf, 512);
    }

    /* Handle resources/ paths using the 5-level search hierarchy */
    if (strncmp(path, "resources/", 10) == 0) {
        extern char g_sre_vfs_data_dir[512];
        extern char g_sre_vfs_mod_name[128];
        extern char g_sre_vfs_profile_id[64];
        extern char g_sre_vfs_path_assets[512];
        const char* subpath = path + 10;
        FILE* f;

        // 1. <data_dir>/mods/<active_mod>/resources/<profile_id>/<subpath>
        if (g_sre_vfs_data_dir[0] && g_sre_vfs_mod_name[0] && g_sre_vfs_profile_id[0]) {
            snprintf(out_buf, 512, "%s/mods/%s/resources/%s/%s", 
                     g_sre_vfs_data_dir, g_sre_vfs_mod_name, g_sre_vfs_profile_id, subpath);
            f = fopen(out_buf, "rb");
            if (f) { fclose(f); return out_buf; }
        }

        // 2. <data_dir>/mods/<active_mod>/resources/<subpath>
        if (g_sre_vfs_data_dir[0] && g_sre_vfs_mod_name[0]) {
            snprintf(out_buf, 512, "%s/mods/%s/resources/%s", 
                     g_sre_vfs_data_dir, g_sre_vfs_mod_name, subpath);
            f = fopen(out_buf, "rb");
            if (f) { fclose(f); return out_buf; }
        }

        // 3. <data_dir>/resources/<profile_id>/<subpath>
        if (g_sre_vfs_data_dir[0] && g_sre_vfs_profile_id[0]) {
            snprintf(out_buf, 512, "%s/resources/%s/%s", 
                     g_sre_vfs_data_dir, g_sre_vfs_profile_id, subpath);
            f = fopen(out_buf, "rb");
            if (f) { fclose(f); return out_buf; }
        }

        // 4. <data_dir>/resources/<subpath>
        if (g_sre_vfs_data_dir[0]) {
            snprintf(out_buf, 512, "%s/resources/%s", 
                     g_sre_vfs_data_dir, subpath);
            f = fopen(out_buf, "rb");
            if (f) { fclose(f); return out_buf; }
        }

        // 5. <assets_dir>/resources/<subpath>
        if (g_sre_vfs_path_assets[0]) {
            snprintf(out_buf, 512, "%s/resources/%s", 
                     g_sre_vfs_path_assets, subpath);
            f = fopen(out_buf, "rb");
            if (f) { fclose(f); return out_buf; }
        }
    }

    return path;
}

/* =========================================================================
 * SCL Protobuf Decoder for loadfile
 * Extracts the Lua source or bytecode from a .scl ObjectLibrary message.
 * ========================================================================= */
static int pb_read_varint(const unsigned char** p, const unsigned char* end, uint64_t* out) {
    uint64_t result = 0;
    int shift = 0;
    while (*p < end) {
        unsigned char b = **p;
        (*p)++;
        result |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = result;
            return 1;
        }
        shift += 7;
        if (shift >= 64) break;
    }
    return 0;
}

static int sre_scl_extract_lua(const char* buf, size_t size, const char** out_lua, size_t* out_len) {
    const unsigned char* p = (const unsigned char*)buf;
    const unsigned char* end = p + size;
    
    while (p < end) {
        uint64_t key;
        if (!pb_read_varint(&p, end, &key)) return 0;
        int field = key >> 3;
        int wire = key & 7;
        
        if (wire == 2) {
            uint64_t len;
            if (!pb_read_varint(&p, end, &len)) return 0;
            if (p + len > end) return 0;
            
            if (field == 5) { /* Program */
                const unsigned char* prog_p = p;
                const unsigned char* prog_end = p + len;
                while (prog_p < prog_end) {
                    uint64_t pkey;
                    if (!pb_read_varint(&prog_p, prog_end, &pkey)) return 0;
                    int pfield = pkey >> 3;
                    int pwire = pkey & 7;
                    if (pwire == 2) {
                        uint64_t plen;
                        if (!pb_read_varint(&prog_p, prog_end, &plen)) return 0;
                        if (prog_p + plen > prog_end) return 0;
                        if (pfield == 2 || pfield == 3) { /* Source or CompiledCode */
                            *out_lua = (const char*)prog_p;
                            *out_len = plen;
                            return 1;
                        }
                        prog_p += plen;
                    } else if (pwire == 0) {
                        uint64_t dummy;
                        if (!pb_read_varint(&prog_p, prog_end, &dummy)) return 0;
                    } else if (pwire == 1) {
                        prog_p += 8;
                    } else if (pwire == 5) {
                        prog_p += 4;
                    } else {
                        return 0; /* Unknown wire type */
                    }
                }
            }
            p += len;
        } else if (wire == 0) {
            uint64_t dummy;
            if (!pb_read_varint(&p, end, &dummy)) return 0;
        } else if (wire == 1) {
            p += 8;
        } else if (wire == 5) {
            p += 4;
        } else {
            return 0;
        }
    }
    return 0;
}

static void sre_patch_bc_scl(const char* chunk_buf, size_t chunk_len) {
    char* data = (char*)chunk_buf;
    for (size_t i = 0; i + 8 <= chunk_len; i++) {
        if (memcmp(&data[i], "i < 0.2", 7) == 0 && (i + 7 == chunk_len || data[i+7] != '5')) {
            data[i+4] = '2'; // replace '0' with '2' -> "i < 2.2" or "i < 2.0"
        }
        if (memcmp(&data[i], "i < 0.25", 8) == 0) {
            data[i+4] = '2'; // replace '0' with '2' -> "i < 2.25"
        }
    }
    fprintf(stderr, "[SRE-VFS] Patched bc.scl click/drag thresholds to 2.0s in-place!\n");
}

/* loadfile(filename) — reads file, compiles via loadstring, returns chunk.
 * Returns: function on success, or nil + errmsg on failure. */
static int sre_loadfile(lua_State* L) {
    const char* filename = lua_tostring(L, 1);
    if (!filename) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "loadfile: filename expected");
        return 2;
    }

    /* VFS path translation */
    char vfs_buf[512];
    const char* real_path = sre_vfs_resolve_path(filename, vfs_buf);

    /* Read the entire file */
    FILE* fp = fopen(real_path, "r");
    if (!fp) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "cannot open file");
        return 2;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        g_lua_pushnil(L);
        g_lua_pushstring(L, "empty file");
        return 2;
    }
    if (size > 1024 * 1024) {
        fclose(fp);
        g_lua_pushnil(L);
        g_lua_pushstring(L, "file too large");
        return 2;
    }

    char* buf = (char*)malloc(size + 1);
    if (!buf) {
        fclose(fp);
        g_lua_pushnil(L);
        g_lua_pushstring(L, "out of memory");
        return 2;
    }

    size_t nread = fread(buf, 1, size, fp);
    fclose(fp);
    buf[nread] = '\0';

    /* If it's a Protobuf .scl file, extract the Lua Source/CompiledCode from it.
     * Otherwise, fallback and pass the entire buffer (e.g. plain .lua file) to loadstring. */
    const char* chunk_buf = buf;
    size_t chunk_len = nread;
    sre_scl_extract_lua(buf, nread, &chunk_buf, &chunk_len);

    if (filename && strstr(filename, "bc.scl")) {
        sre_patch_bc_scl(chunk_buf, chunk_len);
    }

    /* Use loadstring(contents, chunkname) from Lua globals to compile */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
    if (g_lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        free(buf);
        g_lua_pushnil(L);
        g_lua_pushstring(L, "loadstring not available");
        return 2;
    }

    /* Push file contents and chunk name */
    g_lua_pushlstring(L, chunk_buf, chunk_len);
    free(buf);
    g_lua_pushstring(L, filename);  /* chunk name = "@filename" convention */

    /* pcall(loadstring, contents, chunkname) → func or nil, errmsg */
    int r = g_lua_pcall(L, 2, 2, 0);
    if (r != 0) {
        /* pcall itself errored — stack: [..., errmsg] */
        /* Insert nil before the error message for (nil, errmsg) return */
        g_lua_pushnil(L);
        if (g_lua_insert) g_lua_insert(L, -2);
        return 2;
    }

    /* loadstring returns (func, nil) on success, (nil, errmsg) on failure */
    if (g_lua_type(L, -2) == LUA_TFUNCTION) {
        lua_pop(L, 1);  /* pop the nil, leave function */
        return 1;
    } else {
        /* Compilation failed: (nil, errmsg) already on stack */
        if (g_lua_tolstring) {
            const char* errmsg = g_lua_tolstring(L, -1, NULL);
            if (errmsg) {
                char err_buf[512];
                snprintf(err_buf, sizeof(err_buf), "Compile error in '%s': %s", filename, errmsg);
                sre_log_lua_error("loadfile", err_buf);
            }
        }
        return 2;
    }
}

/* dofile(filename) — loads and executes a Lua file.
 * Uses sre_loadfile logic, then pcall's the resulting chunk. */
static int sre_dofile(lua_State* L) {
    const char* filename = lua_tostring(L, 1);
    if (!filename) {
        g_lua_pushstring(L, "dofile: filename expected");
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    /* VFS path translation */
    char vfs_buf[512];
    const char* real_path = sre_vfs_resolve_path(filename, vfs_buf);

    /* Read the entire file */
    FILE* fp = fopen(real_path, "r");
    if (!fp) {
        g_lua_pushstring(L, "dofile: cannot open file");
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(fp);
        g_lua_pushstring(L, size <= 0 ? "dofile: empty file" : "dofile: file too large");
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    char* buf = (char*)malloc(size + 1);
    if (!buf) {
        fclose(fp);
        g_lua_pushstring(L, "dofile: out of memory");
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    size_t nread = fread(buf, 1, size, fp);
    fclose(fp);
    buf[nread] = '\0';

    /* If it's a Protobuf .scl file, extract the Lua Source/CompiledCode from it. */
    const char* chunk_buf = buf;
    size_t chunk_len = nread;
    sre_scl_extract_lua(buf, nread, &chunk_buf, &chunk_len);

    if (filename && strstr(filename, "bc.scl")) {
        sre_patch_bc_scl(chunk_buf, chunk_len);
    }

    /* Compile via loadstring */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
    if (g_lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        free(buf);
        g_lua_pushstring(L, "dofile: loadstring not available");
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    g_lua_pushlstring(L, chunk_buf, chunk_len);
    free(buf);
    g_lua_pushstring(L, filename);

    int r = g_lua_pcall(L, 2, 2, 0);
    if (r != 0) {
        /* pcall of loadstring itself failed */
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    /* Check if loadstring succeeded */
    if (g_lua_type(L, -2) != LUA_TFUNCTION) {
        /* (nil, errmsg) — push the error message */
        if (g_lua_tolstring) {
            const char* errmsg = g_lua_tolstring(L, -1, NULL);
            if (errmsg) {
                char err_buf[512];
                snprintf(err_buf, sizeof(err_buf), "Compile error in '%s': %s", filename, errmsg);
                sre_log_lua_error("dofile", err_buf);
            }
        }
        if (g_lua_error) return g_lua_error(L);  /* error with errmsg on top */
        return 0;
    }

    /* Pop the nil from loadstring's second return, leaving the function */
    lua_pop(L, 1);

    /* Execute the compiled chunk */
    r = g_lua_pcall(L, 0, LUA_MULTRET, 0);
    if (r != 0) {
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    /* Return all results from the executed chunk */
    return g_lua_gettop(L);
}


/* =========================================================================
 * LuaFileSystem (fs) library (Task 2.7)
 *
 * SWKiwi exposes this as the "fs" global (via luaopen_lfs).
 * Real implementations using POSIX opendir/readdir/stat/mkdir/rmdir.
 * ========================================================================= */



/* fs.dir iterator — reads entries from a DIR* stored as lightuserdata upvalue */
static int lfs_dir_next(lua_State* L) {
    DIR* dp = (DIR*)g_lua_touserdata(L, lua_upvalueindex(1));
    if (!dp) { g_lua_pushnil(L); return 1; }

    struct dirent* ent;
    while ((ent = readdir(dp)) != NULL) {
        /* Skip "." and ".." */
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }
        g_lua_pushstring(L, ent->d_name);
        return 1;
    }

    /* No more entries — close the directory */
    closedir(dp);
    /* Clear the upvalue so we don't double-close */
    if (g_lua_pushlightuserdata) {
        g_lua_pushlightuserdata(L, NULL);
        if (g_lua_replace) g_lua_replace(L, lua_upvalueindex(1));
    }
    g_lua_pushnil(L);
    return 1;
}

/* fs.dir(path) — returns a real directory iterator */
static int lfs_dir(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) path = ".";

    /* VFS path translation */
    char vfs_buf[512];
    const char* real_path = sre_vfs_resolve_path(path, vfs_buf);

    DIR* dp = opendir(real_path);
    if (!dp) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "cannot open directory");
        return 2;
    }

    if (g_lua_pushlightuserdata) {
        g_lua_pushlightuserdata(L, dp);
        g_lua_pushcclosure(L, lfs_dir_next, 1);
        return 1;
    }

    /* Fallback if pushlightuserdata unavailable */
    closedir(dp);
    g_lua_pushcclosure(L, lfs_dir_next, 0);
    return 1;
}

/* fs.attributes(path, aname) — returns real file stat data */
static int lfs_attributes(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) { g_lua_pushnil(L); return 1; }

    /* VFS path translation */
    char vfs_buf[512];
    const char* real_path = sre_vfs_resolve_path(path, vfs_buf);

    struct stat st;
    if (stat(real_path, &st) != 0) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "cannot obtain information from file");
        return 2;
    }

    /* Check if caller wants a single attribute string */
    const char* aname = NULL;
    if (g_lua_gettop(L) >= 2 && g_lua_type(L, 2) == LUA_TSTRING) {
        aname = lua_tostring(L, 2);
    }

    const char* mode_str = S_ISDIR(st.st_mode) ? "directory" : "file";

    if (aname) {
        /* Return single attribute value */
        if (aname[0] == 'm' && aname[1] == 'o') {  /* "mode" */
            g_lua_pushstring(L, mode_str);
        } else if (aname[0] == 's') {  /* "size" */
            g_lua_pushnumber(L, (double)st.st_size);
        } else if (aname[0] == 'm') {  /* "modification" */
            g_lua_pushnumber(L, (double)st.st_mtime_sec);
        } else if (aname[0] == 'a') {  /* "access" */
            g_lua_pushnumber(L, (double)st.st_atime_sec);
        } else if (aname[0] == 'c') {  /* "change" */
            g_lua_pushnumber(L, (double)st.st_ctime_sec);
        } else {
            g_lua_pushnil(L);
        }
        return 1;
    }

    /* Return full attributes table */
    g_lua_createtable(L, 0, 6);
    g_lua_pushstring(L, mode_str);
    g_lua_setfield(L, -2, "mode");
    g_lua_pushnumber(L, (double)st.st_size);
    g_lua_setfield(L, -2, "size");
    g_lua_pushnumber(L, (double)st.st_mtime_sec);
    g_lua_setfield(L, -2, "modification");
    g_lua_pushnumber(L, (double)st.st_atime_sec);
    g_lua_setfield(L, -2, "access");
    g_lua_pushnumber(L, (double)st.st_ctime_sec);
    g_lua_setfield(L, -2, "change");
    g_lua_pushnumber(L, (double)st.st_ino);
    g_lua_setfield(L, -2, "ino");
    return 1;
}

/* fs.mkdir(path) — create directory with 0755 permissions */
static int lfs_mkdir(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) { g_lua_pushnil(L); return 1; }

    char vfs_buf[512];
    const char* real_path = sre_vfs_resolve_path(path, vfs_buf);

    if (mkdir(real_path, 0755) == 0) {
        g_lua_pushboolean(L, 1);
        return 1;
    }
    g_lua_pushnil(L);
    g_lua_pushstring(L, "cannot create directory");
    return 2;
}

/* fs.rmdir(path) — remove empty directory */
static int lfs_rmdir(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) { g_lua_pushnil(L); return 1; }

    char vfs_buf[512];
    const char* real_path = sre_vfs_resolve_path(path, vfs_buf);

    if (rmdir(real_path) == 0) {
        g_lua_pushboolean(L, 1);
        return 1;
    }
    g_lua_pushnil(L);
    g_lua_pushstring(L, "cannot remove directory");
    return 2;
}

/* fs.currentdir() — returns current directory (stub) */
static int lfs_currentdir(lua_State* L) {
    g_lua_pushstring(L, "/Files/");
    return 1;
}

/* fs.lock/unlock — no-ops */
static int lfs_lock(lua_State* L)   { (void)L; return 0; }
static int lfs_unlock(lua_State* L) { (void)L; return 0; }

static const SreLuaReg fslib[] = {
    {"dir",        lfs_dir},
    {"attributes", lfs_attributes},
    {"mkdir",      lfs_mkdir},
    {"rmdir",      lfs_rmdir},
    {"currentdir", lfs_currentdir},
    {"lock",       lfs_lock},
    {"unlock",     lfs_unlock},
    {NULL, NULL}
};


/* =========================================================================
 * Registration — called from sre_mini_ensure_injected
 * ========================================================================= */

void sre_open_std_libs(lua_State* L) {
    if (!g_luaL_register || !g_lua_pushnumber || !g_lua_setfield) return;

    /* 1. Register custom VFS-aware os library */
    g_luaL_register(L, "os", (const void*)oslib);
    lua_pop(L, 1);

    /* 2. Register custom VFS-aware io library */
    g_luaL_register(L, "io", (const void*)iolib);
    lua_pop(L, 1);

    /* 3. Also register unpack as global (Lua 5.1 compat) */
    g_lua_pushcclosure(L, table_unpack, 0);
    g_lua_setfield(L, LUA_GLOBALSINDEX, "unpack");

    /* 4. Register fs (LuaFileSystem) library */
    g_luaL_register(L, "fs", (const void*)fslib);
    lua_pop(L, 1);

    /* 5. Override loadfile/dofile with VFS-aware versions */
    g_lua_pushcclosure(L, sre_loadfile, 0);
    g_lua_setfield(L, LUA_GLOBALSINDEX, "loadfile");

    g_lua_pushcclosure(L, sre_dofile, 0);
    g_lua_setfield(L, LUA_GLOBALSINDEX, "dofile");
}
