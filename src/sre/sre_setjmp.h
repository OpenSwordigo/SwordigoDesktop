/* sre_setjmp.h — Minimal setjmp/longjmp + recovery stack for libsre.so */
#ifndef SRE_SETJMP_H
#define SRE_SETJMP_H

/* jmp_buf: 176 bytes. ARM64 state occupies 168 bytes; the final slot pads
 * each recovery entry to a 16-byte-friendly size. */
typedef unsigned long long sre_jmp_buf[22];

/* The compiler must preserve the post-setjmp control path and locals because
 * sre_longjmp can make this function return a second time. */
extern int sre_setjmp(sre_jmp_buf buf) __attribute__((returns_twice));

/* Jump back to setjmp point with return value val */
extern void sre_longjmp(sre_jmp_buf buf, int val) __attribute__((noreturn));

/* ========== Recovery Stack ==========
 * Handles nested lua_call → pcall → lua_call → pcall chains.
 * Each entry on the stack has its own jmp_buf and saved errorJmp. */

#define SRE_MAX_RECOVERY 16

typedef struct {
    sre_jmp_buf buf;
    void*       saved_errorJmp;  /* L->errorJmp before pcall */
    void*       lua_state;       /* L pointer for this level */
} sre_recovery_entry;

extern sre_recovery_entry g_sre_recovery_stack[SRE_MAX_RECOVERY];
extern int g_sre_recovery_depth;
extern const unsigned int g_sre_recovery_stack_bytes;

/* Offset of errorJmp in lua_State (ARM64 Lua 5.1) */
#define LUA_ERRORJMP_OFFSET 0xa8

#endif
