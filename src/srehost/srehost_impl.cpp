/*
 * srehost_impl.cpp — SRE Host Runtime Implementation (ProHook Phase 1)
 *
 * Implements the SREHost_* host ABI functions called from Dynarmic's
 * CallSVC handler when guest ARM64 code emits SVC #0x5352.
 *
 * This module bridges the ARM64 guest world to x86_64 host services:
 *   - Hook installation via existing trampoline patcher (no new deps)
 *   - Symbol resolution via the existing ELF symbol table
 *   - JIT cache invalidation via Dynarmic's InvalidateCacheRange
 *   - Logging and profiling services
 *
 * COMPATIBILITY GUARANTEE:
 *   The existing sre_init.c hook table and all legacy offset hooks
 *   are completely untouched. SREHost_InstallHook routes through the
 *   same patcher infrastructure as the legacy system.
 */

#include "srehost_abi.h"
#include "platform/os_external.h"
#include <iostream>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <setjmp.h>   // for jmp_buf / longjmp — used by render-guard recovery

// ============================================================================
// Host-side sre_longjmp / sre_setjmp
//
// The guest-side sre_longjmp is an ARM64 asm function inside libsre.so and
// cannot be called from x86_64 host code. This host wrapper forwards to the
// standard C library longjmp/setjmp, satisfying the extern "C" symbol that
// emulator_dynarmic64.cpp declares for the render-guard recovery path.
//
// The jmp_buf must be a host jmp_buf (standard size, x86_64 ABI). The render
// guard APIs (sre_render_guard_begin / sre_render_guard_end) in sre_init.c
// store a void* to the caller's jmp_buf; this host-side longjmp casts it back.
// ============================================================================
extern "C" void sre_longjmp(void* buf, int val) {
    longjmp(*static_cast<jmp_buf*>(buf), val);
}

// ============================================================================
// External dependencies from the host runtime
// ============================================================================

// Guest memory buffer (from main.cpp)
extern uint8_t* g_guest_memory;

// EmulatorDynarmic64 forward declaration for JIT cache invalidation
#include "../platform/emulator_dynarmic64.h"

// The registered Dynarmic emulator instance (set by SREHost_RegisterEmulator)
static EmulatorDynarmic64* s_dynarmic_emu = nullptr;

/**
 * Register the Dynarmic emulator for JIT cache invalidation.
 * Called from main.cpp after creating EmulatorDynarmic64.
 */
void SREHost_RegisterEmulator(EmulatorDynarmic64* emu) {
    s_dynarmic_emu = emu;
}

// ============================================================================
// sre_host_patch_guest — Phase 2: real ARM64 hook via TrampolineMgr
//
// Installs a direct-branch trampoline at target_vaddr:
//   B  replacement_vaddr   (4-byte ARM64 B instruction)
//
// The relay cave (64 bytes from TrampolineMgr arena) holds:
//   [original first instruction, relocated]
//   B  target_vaddr + 4       (return-jump for call-through)
//
// *orig_out is set to the relay cave guest VA so callers can call-through.
// Returns 0 on success, -1 on failure.
//
// Not weak — this definition replaces the Phase 1 weak stub from
// srehost_impl.cpp at link time (only one strong definition allowed).
// ============================================================================

#include "../platform/trampoline_mgr.h"

// Guest memory base — set in main.cpp, same pointer used by TrampolineMgr
extern uint8_t* g_guest_memory;

extern "C" int sre_host_patch_guest(
    uint64_t guest_addr,
    void*    replacement_func,
    void**   orig_out
) {
    if (!guest_addr || !replacement_func) {
        fprintf(stderr, "[SREHost] sre_host_patch_guest: null argument (addr=0x%llx, rep=%p)\n",
                (unsigned long long)guest_addr, replacement_func);
        return -1;
    }

    TrampolineMgr& mgr = TrampolineMgr::instance();

    // replacement_func is a guest virtual address (cast from pointer-sized int by SVC dispatch)
    uint64_t replacement_vaddr = (uint64_t)(uintptr_t)replacement_func;

    // Generate a unique hook name for the TrampolineMgr registry
    char hook_name[64];
    snprintf(hook_name, sizeof(hook_name), "ProHook_0x%llx", (unsigned long long)guest_addr);

    // Install hook: writes B <replacement> at guest_addr, relay cave for call-through
    // g_orig_guest_addr = 0 (we write it manually below via orig_out)
    bool ok = mgr.install_hook(hook_name, guest_addr, replacement_vaddr,
                               /*g_orig_guest_addr=*/0, /*insns_to_save=*/1);
    if (!ok) {
        fprintf(stderr, "[SREHost] sre_host_patch_guest: TrampolineMgr::install_hook failed for 0x%llx\n",
                (unsigned long long)guest_addr);
        return -1;
    }

    // Return the relay cave address for call-through
    if (orig_out) {
        // The most recently added entry is the one we just installed
        // TrampolineMgr stores it; retrieve via next_cave_addr - SLOT_SIZE
        uint64_t cave_vaddr = mgr.next_cave_addr() - TrampolineMgr::SLOT_SIZE;
        *orig_out = (void*)(uintptr_t)cave_vaddr;
    }

    // Flush the JIT cache for the patched range
    if (s_dynarmic_emu) {
        s_dynarmic_emu->invalidate_cache_range(guest_addr, 4);
    }

    fprintf(stdout, "[SREHost] sre_host_patch_guest: hooked 0x%llx -> 0x%llx\n",
            (unsigned long long)guest_addr, (unsigned long long)replacement_vaddr);
    return 0;
}

// ============================================================================
// Forward declaration (definition provided by elf_loader_arm64.cpp; MSVC
// also compiles the stub out below so only the real symbol remains).
extern "C" uint64_t elf_lookup_symbol(const char* name);

// elf_lookup_symbol weak stub — overridden by elf_loader_arm64.cpp's
// real implementation which searches g_main_mod_64 and g_sre_mod.
// This stub is the Phase 1 fallback for builds that don't link the loader.
// On MSVC there is no weak-symbol semantics, so the stub is skipped entirely:
// the real loader is always linked, and emitting both would be a duplicate
// symbol. Linux keeps the weak stub so it degrades gracefully if absent.
// ============================================================================
#if !defined(_WIN32)
extern "C" SWORDIGO_WEAK uint64_t elf_lookup_symbol(const char* name) {
    (void)name;
    fprintf(stderr, "[SREHost/Stub] elf_lookup_symbol(\"%s\"): weak stub — loader not linked\n",
            name ? name : "(null)");
    return 0;
}
#endif



// ============================================================================
// Hook Registry (for SREHost_RemoveHook support)
// ============================================================================

struct HookRecord {
    uint64_t target_vaddr;
    void*    saved_opcodes;   // 16 bytes of original instructions
    size_t   patch_size;
};

static std::unordered_map<void*, HookRecord> s_hook_registry;
static std::mutex s_hook_mutex;

// ============================================================================
// Profiling state
// ============================================================================

struct ProfileRegion {
    std::chrono::high_resolution_clock::time_point start;
    uint64_t total_ns;
    uint32_t calls;
};

static std::unordered_map<std::string, ProfileRegion> s_profile_regions;
static std::mutex s_profile_mutex;

// ============================================================================
// SREHost_InstallHook
// ============================================================================

SREHost_HookHandle SREHost_InstallHook(
    uint64_t            target_vaddr,
    void*               proxy_func,
    void**              orig_func_out,
    SREHost_HookBackend backend
) {
    (void)backend;  // Phase 1: AUTO and TRAMPOLINE both use the same patcher

    if (!target_vaddr || !proxy_func) {
        std::cerr << "[SREHost] InstallHook: invalid arguments (vaddr=0x"
                  << std::hex << target_vaddr << ", proxy=" << proxy_func << ")\n" << std::dec;
        return nullptr;
    }

    // Use the existing host-side trampoline patcher
    void* orig = nullptr;
    int ret = sre_host_patch_guest(target_vaddr, proxy_func, &orig);
    if (ret != 0) {
        std::cerr << "[SREHost] InstallHook: patcher failed for 0x"
                  << std::hex << target_vaddr << std::dec << " (ret=" << ret << ")\n";
        return nullptr;
    }

    if (orig_func_out) *orig_func_out = orig;

    // Save hook record for later removal
    HookRecord record;
    record.target_vaddr = target_vaddr;
    record.saved_opcodes = nullptr;  // Patcher owns this
    record.patch_size = 16;

    auto* handle = new HookRecord(record);
    {
        std::lock_guard<std::mutex> lock(s_hook_mutex);
        s_hook_registry[handle] = record;
    }

    std::cout << "[SREHost] Hook installed at 0x" << std::hex << target_vaddr
              << " -> " << proxy_func << std::dec << "\n";

    // Phase 2: Invalidate Dynarmic JIT cache for the patched range
    if (s_dynarmic_emu) {
        s_dynarmic_emu->invalidate_cache_range(target_vaddr, 16);
    }

    return static_cast<SREHost_HookHandle>(handle);
}

// ============================================================================
// SREHost_RemoveHook
// ============================================================================

bool SREHost_RemoveHook(SREHost_HookHandle handle) {
    if (!handle) return false;

    std::lock_guard<std::mutex> lock(s_hook_mutex);
    auto it = s_hook_registry.find(handle);
    if (it == s_hook_registry.end()) return false;

    // Phase 1: Restoration is through the trampoline manager
    // (full implementation in Phase 3 when we have opcode save/restore)
    std::cout << "[SREHost] RemoveHook at 0x" << std::hex
              << it->second.target_vaddr << std::dec << "\n";

    if (s_dynarmic_emu) {
        s_dynarmic_emu->invalidate_cache_range(it->second.target_vaddr, 16);
    }

    delete static_cast<HookRecord*>(handle);
    s_hook_registry.erase(it);
    return true;
}

// ============================================================================
// SREHost_GetSymbol
// ============================================================================

uint64_t SREHost_GetSymbol(const char* symbol_name) {
    if (!symbol_name) return 0;
    return elf_lookup_symbol(symbol_name);
}

// ============================================================================
// SREHost_InvalidateGuestRange
// ============================================================================

void SREHost_InvalidateGuestRange(uint64_t guest_vaddr, size_t size) {
    if (s_dynarmic_emu) {
        s_dynarmic_emu->invalidate_cache_range(guest_vaddr, size);
    }
}

// ============================================================================
// SREHost_Log
// ============================================================================

void SREHost_Log(int log_level, const char* tag, const char* message) {
    static const char* level_str[] = { "V", "D", "I", "W", "E" };
    const char* lvl = (log_level >= 0 && log_level <= 4) ? level_str[log_level] : "?";
    fprintf(stderr, "[SREHost/%s] %s: %s\n", lvl, tag ? tag : "?", message ? message : "");
}

// ============================================================================
// SREHost_ProfileBegin / ProfileEnd
// ============================================================================

void SREHost_ProfileBegin(const char* region_name) {
    if (!region_name) return;
    std::lock_guard<std::mutex> lock(s_profile_mutex);
    auto& r = s_profile_regions[region_name];
    r.start = std::chrono::high_resolution_clock::now();
}

void SREHost_ProfileEnd(const char* region_name) {
    if (!region_name) return;
    auto now = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(s_profile_mutex);
    auto it = s_profile_regions.find(region_name);
    if (it == s_profile_regions.end()) return;
    auto& r = it->second;
    uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - r.start).count()
    );
    r.total_ns += elapsed_ns;
    r.calls++;
    if (r.calls % 100 == 0) {
        fprintf(stderr, "[SREHost/Profile] %s: avg=%.2f us, calls=%u\n",
                region_name,
                (double)r.total_ns / r.calls / 1000.0,
                r.calls);
    }
}

// ============================================================================
// SREHost_GetHostBlock
//
// LIMITATION: Dynarmic A64 exposes no public API to query the host machine-code
// pointer for a compiled guest block. The public interface (a64.h) provides
// only: Run(), Step(), ClearCache(), InvalidateCacheRange(), HaltExecution(),
// and register accessors.
//
// Getting the JIT'd host block would require either:
//   (a) Patching Dynarmic internals to expose BlockOfCode::GetCodePtr(), or
//   (b) Implementing a custom JIT backend.
//
// For now this always returns nullptr. Callers should test for nullptr and
// fall back to the SVC-based hook path (SREHOST_BACKEND_PATCH_GUEST).
// ============================================================================

void* SREHost_GetHostBlock(uint64_t guest_vaddr) {
    (void)guest_vaddr;
    return nullptr;
}

// ============================================================================
// SREHost_Dispatch — called from emulator_dynarmic64.cpp CallSVC handler
//
// Dynarmic delivers:
//   svc_num       = SVC immediate operand (must == 0x5352 for SRE)
//   regs          = pointer to current guest X0..X30 register file
//   x8_syscall_id = guest X8 value (syscall selector)
// ============================================================================

void SREHost_Dispatch(uint32_t svc_num, uint64_t* regs, uint64_t x8_syscall_id) {
    if (svc_num != 0x5352) return;  // Not our SVC — ignore

    switch (x8_syscall_id) {
    case SRE_SVC_INSTALL_HOOK: {
        /*
         * X0 = target_vaddr
         * X1 = proxy_func (guest address of replacement function)
         * X2 = orig_func_out (guest address of void* to write original into)
         * X3 = backend (SREHost_HookBackend)
         * Returns: X0 = opaque handle (64-bit int)
         */
        uint64_t  target_vaddr  = regs[0];
        void*     proxy_func    = (void*)regs[1];
        void**    orig_out      = (void**)( regs[2] ? (g_guest_memory + regs[2]) : nullptr );
        SREHost_HookBackend backend = (SREHost_HookBackend)(int)regs[3];

        SREHost_HookHandle h = SREHost_InstallHook(target_vaddr, proxy_func, orig_out, backend);
        regs[0] = (uint64_t)h;
        break;
    }
    case SRE_SVC_REMOVE_HOOK: {
        /* X0 = handle (opaque ptr returned by INSTALL_HOOK) */
        bool ok = SREHost_RemoveHook((SREHost_HookHandle)regs[0]);
        regs[0] = ok ? 1 : 0;
        break;
    }
    case SRE_SVC_GET_SYMBOL: {
        /*
         * X0 = guest_addr of null-terminated symbol name string
         * Returns: X0 = resolved guest virtual address (0 = not found)
         */
        const char* sym_name = (regs[0] && regs[0] < 0xE0000000ULL)
                                 ? (const char*)(g_guest_memory + regs[0])
                                 : nullptr;
        regs[0] = sym_name ? SREHost_GetSymbol(sym_name) : 0;
        break;
    }
    case SRE_SVC_INVALIDATE_RANGE: {
        /* X0 = guest_vaddr, X1 = size */
        SREHost_InvalidateGuestRange(regs[0], (size_t)regs[1]);
        regs[0] = 0;
        break;
    }
    case SRE_SVC_LOG: {
        /*
         * X0 = log_level
         * X1 = guest_addr of tag string
         * X2 = guest_addr of message string
         */
        int         level   = (int)regs[0];
        const char* tag     = (regs[1] && regs[1] < 0xE0000000ULL)
                                ? (const char*)(g_guest_memory + regs[1]) : "?";
        const char* message = (regs[2] && regs[2] < 0xE0000000ULL)
                                ? (const char*)(g_guest_memory + regs[2]) : "";
        SREHost_Log(level, tag, message);
        regs[0] = 0;
        break;
    }
    case SRE_SVC_PROFILE_BEGIN: {
        /* X0 = guest_addr of region_name string */
        const char* name = (regs[0] && regs[0] < 0xE0000000ULL)
                             ? (const char*)(g_guest_memory + regs[0]) : nullptr;
        if (name) SREHost_ProfileBegin(name);
        regs[0] = 0;
        break;
    }
    case SRE_SVC_PROFILE_END: {
        /* X0 = guest_addr of region_name string */
        const char* name = (regs[0] && regs[0] < 0xE0000000ULL)
                             ? (const char*)(g_guest_memory + regs[0]) : nullptr;
        if (name) SREHost_ProfileEnd(name);
        regs[0] = 0;
        break;
    }
    case SRE_SVC_GET_HOST_BLOCK: {
        /* X0 = guest_vaddr */
        void* blk = SREHost_GetHostBlock(regs[0]);
        regs[0] = (uint64_t)blk;
        break;
    }
    default:
        fprintf(stderr, "[SREHost] Unknown SVC syscall ID 0x%llx\n",
                (unsigned long long)x8_syscall_id);
        regs[0] = (uint64_t)-1;
        break;
    }
}
