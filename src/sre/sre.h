/*
 * sre.h — Swordigo Runtime Engine
 * 
 * Public header for libsre.so — an ARM64 shared library that replaces
 * problematic functions in libswordigo.so with clean, non-atomic,
 * single-threaded implementations.
 *
 * Compiled with: aarch64-linux-gnu-gcc -shared -fPIC -O2
 * Loaded by: SwordigoDesktop ELF loader as a guest dependency
 */

#ifndef SRE_H
#define SRE_H

/* Freestanding type definitions — no libc headers needed */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef signed int         int32_t;
typedef signed long        int64_t;
typedef unsigned long      size_t;
typedef long               ptrdiff_t;

/* Aliases used by other SRE headers */
typedef uint64_t           sre_u64;
typedef uint32_t           sre_u32;
typedef unsigned long      sre_size_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

/* =========================================================================
 * CppString — GNU libstdc++ COW std::string internals
 * =========================================================================
 * The Caver engine uses GNU libstdc++ std::string with Copy-On-Write (COW).
 * The string data is prefixed by a _Rep header:
 *
 *   [_Rep header (24 bytes)][string data (capacity+1 bytes)]
 *                            ^
 *                            std::string::_M_p points here
 *
 * The _Rep::_M_refcount uses atomic LDAXR/STLXR on ARM64, causing spin
 * loops in Unicorn. Our replacement uses simple non-atomic operations.
 *
 * Refcount convention (GNU libstdc++):
 *   -1 = leaked/static (never delete — used by empty string sentinel)
 *    0 = one reference (not shared)
 *   >0 = N+1 references (shared)
 */

/* Matches GNU libstdc++ _Rep layout on AArch64 */
typedef struct {
    uint64_t length;       /* offset 0:  current string length */
    uint64_t capacity;     /* offset 8:  allocated capacity (excl. NUL) */
    int32_t  refcount;     /* offset 16: reference count (see convention above) */
    int32_t  _pad;         /* offset 20: padding for 8-byte alignment */
    /* char data[] follows at offset 24 */
} SreStringRep;

/* Get the _Rep header from a string data pointer */
#define SRE_REP(data_ptr) (((SreStringRep*)(data_ptr)) - 1)

/* Get the string data from a _Rep pointer */
#define SRE_DATA(rep_ptr) ((char*)((rep_ptr) + 1))

/* std::string object is just a pointer to the data */
typedef struct {
    char* data;  /* points to character data after _Rep header */
} SreString;

/* =========================================================================
 * Hook Table — defines which functions to redirect
 * =========================================================================
 * The host reads this table after loading libsre.so and writes trampolines
 * at each target address in libswordigo.so.
 */

typedef struct {
    uint64_t    target_offset;  /* file offset in libswordigo.so */
    const char* symbol_name;    /* symbol name in libsre.so */
    uint64_t    orig_func;      /* relay stub address (set by host after trampoline install) */
} SreHookEntry;

/* =========================================================================
 * Exported functions — called from the host or via trampoline
 * =========================================================================
 */

/* Initialization — called by host after loading */
void sre_init(uint64_t swordigo_base, uint64_t empty_sentinel_bss_offset);

/* CppString replacements — hooked via trampoline */
SreString* sre_CppString_from_char_p(SreString* self, const char* src);
SreString* sre_CppString_assign(SreString* self, const char* src, uint64_t len);
SreString* sre_CppString_append(SreString* self, const char* src, uint64_t len);
void sre_CppString_release(SreString* self);

/* =========================================================================
 * Math types (matching Caver engine)
 * =========================================================================
 */

typedef struct { float x, y; }       SreVec2;
typedef struct { float x, y, z; }    SreVec3;
typedef struct { float r, g, b, a; } SreColor;

#endif /* SRE_H */
