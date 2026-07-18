/*
 * sre_string.c — Non-atomic CppString replacement
 *
 * Replaces GNU libstdc++ COW std::string functions that use atomic
 * LDAXR/STLXR for reference counting. In our single-threaded Unicorn
 * emulator, atomics cause spin loops. These replacements use simple
 * non-atomic operations.
 *
 * Functions replaced:
 *   CppString_from_char_p  (0x566bb8 in v1.4.12)
 *   CppString_append_impl  (0x567254 in v1.4.12)
 *   CppString_assign       (0x56918c in v1.4.12)
 *   CppString_release      (0x565220 in v1.4.12)
 */

#include "sre.h"

/* These are resolved from libc by the bridge */
extern void* malloc(size_t size);
extern void  free(void* ptr);
extern void* memcpy(void* dest, const void* src, size_t n);
extern void* memset(void* s, int c, size_t n);
extern size_t strlen(const char* s);
extern void* realloc(void* ptr, size_t size);

/* Global: pointer to the empty string sentinel in libswordigo.so's BSS */
static char* g_empty_sentinel = 0;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Allocate a new string with given capacity */
static SreStringRep* sre_string_alloc(uint64_t capacity) {
    /* Allocate: header + data + NUL terminator */
    SreStringRep* rep = (SreStringRep*)malloc(sizeof(SreStringRep) + capacity + 1);
    if (!rep) return 0;

    rep->length   = 0;
    rep->capacity = capacity;
    rep->refcount = 0;  /* 0 = one reference, not shared */
    rep->_pad     = 0;

    /* NUL-terminate */
    char* data = SRE_DATA(rep);
    data[0] = '\0';

    return rep;
}

/* Check if a data pointer is the empty sentinel (refcount -1, never free) */
static int sre_is_empty_sentinel(const char* data) {
    if (!data) return 1;
    if (g_empty_sentinel && data == g_empty_sentinel) return 1;
    /* Also check refcount == -1 as fallback */
    SreStringRep* rep = SRE_REP(data);
    return (rep->refcount == -1);
}

/* Release a reference — non-atomic */
static void sre_rep_release(char* data) {
    if (sre_is_empty_sentinel(data)) return;

    SreStringRep* rep = SRE_REP(data);

    /* Decrement refcount. Convention:
     *   was 0 (sole owner) → -1 → free
     *   was >0 (shared)    → >=0 → don't free
     *   was -1 (static)    → should never happen (caught above)
     */
    int old = rep->refcount;
    rep->refcount = old - 1;

    if (old <= 0) {
        /* We were the last reference, free the block */
        free(rep);
    }
}

/* Grab (increment) a reference — non-atomic */
static void sre_rep_grab(char* data) {
    if (sre_is_empty_sentinel(data)) return;

    SreStringRep* rep = SRE_REP(data);
    /* Only grab if not static (-1) */
    if (rep->refcount >= 0) {
        rep->refcount++;
    }
}

/* Clone a shared string (COW: make a private copy) */
static char* sre_string_clone(const char* old_data, uint64_t len) {
    SreStringRep* new_rep = sre_string_alloc(len);
    if (!new_rep) return (char*)old_data;

    char* new_data = SRE_DATA(new_rep);
    memcpy(new_data, old_data, len);
    new_data[len] = '\0';
    new_rep->length = len;

    return new_data;
}

/* Ensure the string has exclusive ownership (refcount == 0) and enough capacity.
 * Returns pointer to writable data. May reallocate. */
static char* sre_string_mutate(SreString* self, uint64_t needed_capacity) {
    char* data = self->data;

    if (sre_is_empty_sentinel(data)) {
        /* Empty sentinel → allocate new */
        SreStringRep* rep = sre_string_alloc(needed_capacity);
        if (!rep) return data;
        self->data = SRE_DATA(rep);
        return self->data;
    }

    SreStringRep* rep = SRE_REP(data);

    if (rep->refcount > 0) {
        /* Shared — make a private copy */
        char* new_data = sre_string_clone(data, rep->length);
        sre_rep_release(data);  /* release our reference to the shared copy */
        self->data = new_data;
        data = new_data;
        rep = SRE_REP(data);
    }

    /* Now we have exclusive ownership. Check capacity. */
    if (rep->capacity < needed_capacity) {
        /* Need more space — realloc */
        uint64_t new_cap = needed_capacity;
        /* Growth factor: at least double */
        if (new_cap < rep->capacity * 2) {
            new_cap = rep->capacity * 2;
        }
        SreStringRep* new_rep = (SreStringRep*)realloc(rep, sizeof(SreStringRep) + new_cap + 1);
        if (!new_rep) return data;
        new_rep->capacity = new_cap;
        self->data = SRE_DATA(new_rep);
        data = self->data;
    }

    return data;
}

/* =========================================================================
 * String Replacement Table
 * =========================================================================
 * EDIT THIS TABLE to change ANY text in the game.
 * Every string the game creates goes through sre_CppString_from_char_p.
 * Just add { "original", "replacement" } entries and rebuild.
 *
 * Examples:
 *   { "Start",        "Play"          }   — rename menu button
 *   { "Offers",       "Settings"      }   — repurpose Offers button
 *   { "Credits",      "About"         }   — rename Credits
 *   { "Privacy Policy","SRE v1.0"     }   — replace footer text
 * =========================================================================*/

static const char* g_sre_string_replacements[][2] = {
    /* ---- Main Menu Buttons ---- */
    /* { "Start",          "Singleplayer" }, */
    /* { "Achievements",   "Online"       }, */
    /* { "Offers",         "Settings"     }, */
    /* { "Credits",        "About"        }, */
    /* { "Privacy Policy", "SRE v1.0"     }, */

    { 0, 0 }  /* sentinel — do NOT remove */
};

/* =========================================================================
 * Dynamic CString Replacement Table
 * =========================================================================
 * The host populates this table from config files (e.g. cstrings.toml).
 * Format: packed key-value pairs separated by null bytes.
 *
 * Layout: "original1\0replacement1\0original2\0replacement2\0\0"
 * The double-null at the end marks the end of entries.
 *
 * Host writes this via sre_cstring_add_replacement() or directly to
 * g_sre_cstring_table before the game starts creating strings.
 * ========================================================================= */

/* Dynamic replacement table — host populates this */
char g_sre_cstring_table[8192] = {0};  /* Packed key\0value\0key\0value\0\0 */
int  g_sre_cstring_count = 0;          /* Number of active replacements */

/*
 * sre_cstring_add — Add a dynamic string replacement
 * Called by the host to add entries from cstrings.toml / config.
 *
 * @param original     The original string to match
 * @param replacement  The replacement string
 * @return 1 on success, 0 if table full
 */
int sre_cstring_add(const char* original, const char* replacement) {
    if (!original || !replacement) return 0;

    /* Find the end of the current table */
    int pos = 0;
    int entries = 0;
    while (pos < 8190 && entries < g_sre_cstring_count) {
        /* Skip original */
        while (pos < 8190 && g_sre_cstring_table[pos]) pos++;
        pos++; /* skip null */
        /* Skip replacement */
        while (pos < 8190 && g_sre_cstring_table[pos]) pos++;
        pos++; /* skip null */
        entries++;
    }

    /* Copy original */
    int orig_len = 0;
    while (original[orig_len]) orig_len++;
    if (pos + orig_len + 1 >= 8190) return 0;
    for (int i = 0; i <= orig_len; i++)
        g_sre_cstring_table[pos + i] = original[i];
    pos += orig_len + 1;

    /* Copy replacement */
    int repl_len = 0;
    while (replacement[repl_len]) repl_len++;
    if (pos + repl_len + 1 >= 8190) return 0;
    for (int i = 0; i <= repl_len; i++)
        g_sre_cstring_table[pos + i] = replacement[i];
    pos += repl_len + 1;

    /* Terminate */
    g_sre_cstring_table[pos] = '\0';
    g_sre_cstring_count++;

    return 1;
}

/* Forward declaration — sre_strcmp defined below */
static int sre_strcmp(const char* a, const char* b);

/* Look up a string in the dynamic table */
static const char* sre_cstring_lookup(const char* src) {
    if (g_sre_cstring_count == 0) return 0;

    int pos = 0;
    int entries = 0;
    while (pos < 8190 && entries < g_sre_cstring_count) {
        const char* key = &g_sre_cstring_table[pos];
        /* Skip to value */
        while (pos < 8190 && g_sre_cstring_table[pos]) pos++;
        pos++;
        const char* val = &g_sre_cstring_table[pos];
        /* Skip to next entry */
        while (pos < 8190 && g_sre_cstring_table[pos]) pos++;
        pos++;
        entries++;

        /* Compare key with src */
        if (sre_strcmp(src, key) == 0) {
            return val;
        }
    }
    return 0;
}

/* Simple strcmp — we don't have libc strcmp in freestanding */
static int sre_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* =========================================================================
 * Exported replacement functions
 * ========================================================================= */

/*
 * sre_CppString_from_char_p — Replace std::string constructor from char*
 *
 * Original at offset 0x566bb8 (v1.4.12)
 * ARM64 ABI: X0 = this (SreString*), X1 = src (const char*)
 *
 * ALL game strings pass through here. The replacement table above
 * lets you change any text without touching the binary.
 */
SreString* sre_CppString_from_char_p(SreString* self, const char* src) {
    if (!src) {
        /* NULL source → empty string */
        self->data = g_empty_sentinel;
        return self;
    }

    /* Apply string replacement table (static) */
    {
        int i;
        for (i = 0; g_sre_string_replacements[i][0] != 0; i++) {
            if (sre_strcmp(src, g_sre_string_replacements[i][0]) == 0) {
                src = g_sre_string_replacements[i][1];
                break;
            }
        }
    }

    /* Apply dynamic replacements (from cstrings.toml / host config) */
    {
        const char* dyn_repl = sre_cstring_lookup(src);
        if (dyn_repl) {
            src = dyn_repl;
        }
    }

    uint64_t len = strlen(src);
    if (len == 0 && g_empty_sentinel) {
        self->data = g_empty_sentinel;
        return self;
    }

    SreStringRep* rep = sre_string_alloc(len);
    if (!rep) {
        self->data = g_empty_sentinel;
        return self;
    }

    char* data = SRE_DATA(rep);
    memcpy(data, src, len + 1);  /* include NUL */
    rep->length = len;

    self->data = data;
    return self;
}

/*
 * sre_CppString_assign — Replace std::string::assign(char*, len)
 *
 * Original at offset 0x56918c (v1.4.12)
 * ARM64 ABI: X0 = this (SreString*), X1 = src (const char*), X2 = len
 */
SreString* sre_CppString_assign(SreString* self, const char* src, uint64_t len) {
    if (!src || len == 0) {
        /* Release old data */
        sre_rep_release(self->data);
        self->data = g_empty_sentinel;
        return self;
    }

    /* Try to reuse existing buffer */
    char* data = sre_string_mutate(self, len);
    SreStringRep* rep = SRE_REP(data);

    memcpy(data, src, len);
    data[len] = '\0';
    rep->length = len;
    return self;
}

/*
 * sre_CppString_append — Replace std::string::append(char*, len)
 *
 * Original at offset 0x567254 (v1.4.12)
 * ARM64 ABI: X0 = this (SreString*), X1 = src (const char*), X2 = len
 */
SreString* sre_CppString_append(SreString* self, const char* src, uint64_t len) {
    if (!src || len == 0) return self;

    char* data = self->data;
    uint64_t old_len = 0;

    if (!sre_is_empty_sentinel(data)) {
        old_len = SRE_REP(data)->length;
    }

    uint64_t new_len = old_len + len;

    /* Ensure we have exclusive ownership and enough space */
    data = sre_string_mutate(self, new_len);
    SreStringRep* rep = SRE_REP(data);

    /* Append */
    memcpy(data + old_len, src, len);
    data[new_len] = '\0';
    rep->length = new_len;
    return self;
}

/*
 * sre_CppString_release — Replace std::string destructor / release
 *
 * Original at offset 0x565220 (v1.4.12)
 * ARM64 ABI: X0 = this (SreString*)
 *
 * Decrements refcount. If last reference, frees memory.
 * If empty sentinel, does nothing.
 */
void sre_CppString_release(SreString* self) {
    if (!self) return;
    sre_rep_release(self->data);
    self->data = g_empty_sentinel;
}
