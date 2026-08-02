/*
 * sre_host_abi.h — Guest-Side ProHook Phase 1 ABI Stubs
 *
 * This header provides inline helper macros for ARM64 guest code (libsre.so)
 * to call SREHost_* services via SVC #0x5352 without linking to any host libs.
 *
 * Usage:
 *   uint64_t sym_addr = srehost_get_symbol("_ZN5Caver19GameSceneController18CreateHeroObjectAtERKNS_7Vector3Eib");
 *   void* orig = NULL;
 *   SREHost_HookHandle h = srehost_install_hook(sym_addr, (void*)my_hook, &orig, 0);
 *
 * Compatibility:
 *   - Requires Dynarmic backend with SVC #0x5352 gateway (ProHook Phase 1).
 *   - Falls back gracefully on Unicorn (SVC is silently skipped, X0 = unchanged).
 *   - Does NOT replace or conflict with sre_init.c legacy hooks.
 */

#ifndef SRE_HOST_ABI_H
#define SRE_HOST_ABI_H

#include <stdint.h>

/* SVC number identifying SRE gateway calls */
#define SRE_SVC_NUMBER 0x5352

/* Syscall IDs (must match srehost_abi.h) */
#define SRE_SVC_INSTALL_HOOK     0x01
#define SRE_SVC_REMOVE_HOOK      0x02
#define SRE_SVC_GET_SYMBOL       0x03
#define SRE_SVC_INVALIDATE_RANGE 0x04
#define SRE_SVC_LOG              0x05
#define SRE_SVC_PROFILE_BEGIN    0x06
#define SRE_SVC_PROFILE_END      0x07
#define SRE_SVC_GET_HOST_BLOCK   0x08
/* Tier-2 PC hooks — Dynarmic-native, zero guest writes */
#define SRE_SVC_INSTALL_PC_HOOK  0x09
#define SRE_SVC_REMOVE_PC_HOOK   0x0A

typedef void* SREHost_HookHandle;

/*
 * Emit a host ABI SVC call.
 * ARM64 inline asm: load X8 with syscall_id, then emit SVC #0x5352.
 * Result is returned in X0 (captured as uint64_t).
 *
 * NOTE: This is a compile-time helper only used by ARM64 libsre.so.
 *       The x86_64 host never compiles this file with __aarch64__ defined.
 */
#ifdef __aarch64__

static inline uint64_t srehost_syscall0(uint64_t id) {
    register uint64_t x8 __asm__("x8") = id;
    register uint64_t x0 __asm__("x0");
    __asm__ volatile("svc #0x5352" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline uint64_t srehost_syscall2(uint64_t id, uint64_t a0, uint64_t a1) {
    register uint64_t x8 __asm__("x8") = id;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0x5352"
                     : "+r"(x0), "+r"(x1)
                     : "r"(x8)
                     : "memory");
    return x0;
}

static inline uint64_t srehost_syscall4(uint64_t id,
                                        uint64_t a0, uint64_t a1,
                                        uint64_t a2, uint64_t a3) {
    register uint64_t x8 __asm__("x8") = id;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0x5352"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "r"(x8)
                     : "memory");
    return x0;
}

/* -------------------------------------------------------------------------
 * High-level wrappers
 * ---------------------------------------------------------------------- */

/**
 * Install a guest function hook via the host trampoline patcher.
 * @param target_vaddr  Guest virtual address of function to hook
 * @param proxy_func    Guest address of replacement function
 * @param orig_func_out Guest address of void* to receive original fn ptr
 * @param backend       0=AUTO, 1=OFFSET, 2=TRAMPOLINE, 3=TRANSLATION
 * @return opaque handle (0 on failure)
 */
static inline SREHost_HookHandle srehost_install_hook(
    uint64_t target_vaddr,
    void*    proxy_func,
    void**   orig_func_out,
    int      backend
) {
    return (SREHost_HookHandle)srehost_syscall4(
        SRE_SVC_INSTALL_HOOK,
        target_vaddr,
        (uint64_t)proxy_func,
        (uint64_t)orig_func_out,
        (uint64_t)(unsigned int)backend
    );
}

/**
 * Remove an installed hook by handle.
 */
static inline int srehost_remove_hook(SREHost_HookHandle handle) {
    return (int)srehost_syscall2(SRE_SVC_REMOVE_HOOK, (uint64_t)handle, 0);
}

/**
 * Resolve a mangled C++ symbol name to a guest virtual address.
 */
static inline uint64_t srehost_get_symbol(const char* symbol_name) {
    return srehost_syscall2(SRE_SVC_GET_SYMBOL, (uint64_t)symbol_name, 0);
}

/**
 * Invalidate a range of guest addresses in the Dynarmic JIT cache.
 * Must be called after patching guest opcodes in memory.
 */
static inline void srehost_invalidate_range(uint64_t vaddr, uint64_t size) {
    srehost_syscall2(SRE_SVC_INVALIDATE_RANGE, vaddr, size);
}

/**
 * Log a message through the host logging system.
 */
static inline void srehost_log(int level, const char* tag, const char* msg) {
    srehost_syscall4(SRE_SVC_LOG, (uint64_t)(unsigned int)level,
                     (uint64_t)tag, (uint64_t)msg, 0);
}

/**
 * Begin a named profiling region.
 */
static inline void srehost_profile_begin(const char* name) {
    srehost_syscall2(SRE_SVC_PROFILE_BEGIN, (uint64_t)name, 0);
}

/**
 * End a named profiling region.
 */
static inline void srehost_profile_end(const char* name) {
    srehost_syscall2(SRE_SVC_PROFILE_END, (uint64_t)name, 0);
}

#else  /* !__aarch64__ -- x86_64 host stubs (no-op for host compilation) */

static inline SREHost_HookHandle srehost_install_hook(uint64_t t, void* p, void** o, int b) {
    (void)t; (void)p; (void)o; (void)b; return 0;
}
static inline int     srehost_remove_hook(SREHost_HookHandle h) { (void)h; return 0; }
static inline uint64_t srehost_get_symbol(const char* s) { (void)s; return 0; }
static inline void    srehost_invalidate_range(uint64_t v, uint64_t sz) { (void)v; (void)sz; }
static inline void    srehost_log(int l, const char* t, const char* m) { (void)l;(void)t;(void)m; }
static inline void    srehost_profile_begin(const char* n) { (void)n; }
static inline void    srehost_profile_end(const char* n) { (void)n; }

#endif /* __aarch64__ */

#endif /* SRE_HOST_ABI_H */
