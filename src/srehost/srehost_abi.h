/*
 * srehost_abi.h — SRE Host Runtime ABI
 *
 * ProHook Phase 1: Stable C host ABI for guest-to-host communication.
 *
 * Architecture:
 *   Guest ARM64 (libsre.so) calls SREHost_* functions using SVC #0x5352
 *   ("SR" = SRE marker). Dynarmic's CallSVC handler dispatches to
 *   SREHost_Dispatch() in srehost_impl.cpp on the x86_64 host side.
 *
 * Guest call convention (SVC gateway):
 *   X8 = SREHost syscall ID (SRE_SVC_*)
 *   X0-X7 = arguments
 *   X0 = return value after dispatch
 *
 * Compatibility:
 *   - All existing sre_init.c offset hooks are PRESERVED unchanged.
 *   - This system adds a NEW unified hook backend without removing anything.
 *   - Legacy: sre_init.c table -> host patcher (unchanged)
 *   - New: SREHost_InstallHook() -> unified HookManager (routes same patcher)
 */

#ifndef SREHOST_ABI_H
#define SREHOST_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * SVC syscall IDs (written to X8 before SVC #0x5352)
 * ========================================================================= */
#define SRE_SVC_INSTALL_HOOK         0x01  /* SREHost_InstallHook */
#define SRE_SVC_REMOVE_HOOK          0x02  /* SREHost_RemoveHook */
#define SRE_SVC_GET_SYMBOL           0x03  /* SREHost_GetSymbol */
#define SRE_SVC_INVALIDATE_RANGE     0x04  /* SREHost_InvalidateGuestRange */
#define SRE_SVC_LOG                  0x05  /* SREHost_Log */
#define SRE_SVC_PROFILE_BEGIN        0x06  /* SREHost_ProfileBegin */
#define SRE_SVC_PROFILE_END          0x07  /* SREHost_ProfileEnd */
#define SRE_SVC_GET_HOST_BLOCK       0x08  /* SREHost_GetHostBlock */

/* =========================================================================
 * Hook Backend Selector
 * ========================================================================= */
typedef enum SREHost_HookBackend {
    SREHOST_BACKEND_AUTO         = 0,  /* Select best available (default: TRAMPOLINE) */
    SREHOST_BACKEND_OFFSET       = 1,  /* Legacy function pointer swap in hook table */
    SREHOST_BACKEND_TRAMPOLINE   = 2,  /* Inline opcode patch (16-byte LDR+BR) */
    SREHOST_BACKEND_TRANSLATION  = 3,  /* Dynarmic JIT Zero-Patch Hook (Phase 2) */
    SREHOST_BACKEND_IR_INSTR     = 4   /* IR Instrumentation (Phase 3) */
} SREHost_HookBackend;

typedef void* SREHost_HookHandle;

/* =========================================================================
 * Host ABI -- implemented in srehost_impl.cpp, called from emulator
 * ========================================================================= */

/**
 * Install a hook on a guest function at target_vaddr.
 * Works via the existing host-side trampoline patcher (same as sre_init.c).
 * Sets *orig_func_out to the original function pointer for call-through.
 */
SREHost_HookHandle SREHost_InstallHook(
    uint64_t        target_vaddr,
    void*           proxy_func,
    void**          orig_func_out,
    SREHost_HookBackend backend
);

/**
 * Remove a previously installed hook, restoring original opcodes.
 */
bool SREHost_RemoveHook(SREHost_HookHandle handle);

/**
 * Resolve a mangled or demangled symbol name to a guest virtual address.
 */
uint64_t SREHost_GetSymbol(const char* symbol_name);

/**
 * Invalidate a range of guest addresses in the Dynarmic JIT cache.
 * Required after any guest memory modification (opcode patches).
 */
void SREHost_InvalidateGuestRange(uint64_t guest_vaddr, size_t size);

/**
 * Emit a diagnostic log from guest code to host logging system.
 */
void SREHost_Log(int log_level, const char* tag, const char* message);

/**
 * Begin a named profiling region (host-side timer).
 */
void SREHost_ProfileBegin(const char* region_name);

/**
 * End a named profiling region and record elapsed time.
 */
void SREHost_ProfileEnd(const char* region_name);

/**
 * Query the host JIT code pointer for a compiled guest PC.
 * Returns NULL if the block is not currently in the JIT cache.
 */
void* SREHost_GetHostBlock(uint64_t guest_vaddr);

/* =========================================================================
 * Emulator registration -- must be called from main.cpp after
 * creating the EmulatorDynarmic64 instance so the host runtime can
 * perform JIT cache invalidations on hook install/remove.
 * ========================================================================= */
#ifdef __cplusplus
class EmulatorDynarmic64;
void SREHost_RegisterEmulator(EmulatorDynarmic64* emu);
#endif

/* =========================================================================
 * Main SVC dispatcher -- called by Dynarmic's CallSVC with swi == 0x5352
 * X8 = syscall ID, X0-X7 = arguments, returns result in X0 (regs[0])
 * ========================================================================= */
void SREHost_Dispatch(
    uint32_t svc_num,
    uint64_t* regs,           /* regs[0..7] = args in, regs[0] = result out */
    uint64_t  x8_syscall_id   /* syscall selector */
);

#ifdef __cplusplus
}
#endif

#endif /* SREHOST_ABI_H */
