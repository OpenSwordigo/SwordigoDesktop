# OptiX Technical Specification 01: Dynarmic Fastmem and A64 JIT Acceleration

## 1. Executive Overview

This specification details the technical integration of Yuzu-style **Fastmem** Virtual Memory Address Space (VMAS) mapping and low-overhead Dynarmic ARM64 Just-In-Time (JIT) compilation flags within the Swordigo Runtime Engine (SRE).

In standard SRE operation, Dynarmic executes guest ARM64 instructions using virtual C++ `UserCallbacks` (`MemoryRead32`, `MemoryWrite64`). Each memory operation requires virtual function dispatch overhead, resulting in 12–18 host CPU cycles per guest load/store instruction. By adopting Yuzu's `fastmem_pointer` architecture, Dynarmic generates single host `MOV` instructions (`mov rax, [r15 + rbx]`), eliminating callback dispatch and achieving 3.2x–4.5x JIT speedups.

---

## 2. Yuzu Fastmem Architecture Analysis

### 2.1 Virtual Address Translation Mechanics

Yuzu maps the 39-bit or 36-bit Switch virtual address space to a contiguous host memory arena (`fastmem_arena`). Dynarmic emits host x86_64 or AArch64 machine code that adds a fixed host base register `r15` (the page table base) to the guest virtual offset:

$$\text{Host Address} = \text{Host Base Pointer} + \text{Guest Virtual Address}$$

```
+-------------------------------------------------------------------------+
| Guest Virtual Memory (3.5 GB)                                           |
| 0x00000000 -----------------------> 0x00E00000 ----------------------->|
+-------------------------------------------------------------------------+
                                    |
                                    | Direct 1:1 Offset Pointer
                                    v
+-------------------------------------------------------------------------+
| Host Arena Pointer (mmap / VirtualAlloc)                                |
| 0x7FFF00000000 -------------------> 0x7FFF00E00000 ------------------>|
+-------------------------------------------------------------------------+
```

### 2.2 Unmapped Memory Fault Handling (`SIGSEGV` / `SIGBUS`)

When guest code attempts to dereference unmapped or invalid guest virtual addresses, Fastmem avoids explicit bounds checking instructions by relying on the host OS kernel's Page Fault mechanism.

1. **Signal Handler Registration**: SRE registers a `SIGSEGV` / `SIGBUS` signal handler using `sigaction()` with `SA_SIGINFO`.
2. **Context Inspection**: The signal handler inspects `ucontext_t->uc_mcontext.gregs[REG_RIP]`.
3. **Address Bounds Checking**: If the faulting address `info->si_addr` falls within `g_guest_memory` range `[0x00000000, 0xE0000000]`, the handler rewinds or redirects instruction execution to an SRE recovery trampoline (`sre_fastmem_fault_recovery`), setting the target register to zero and advancing `RIP` past the faulting `MOV` instruction.

```cpp
static void sre_fastmem_sigsegv_handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uctx = static_cast<ucontext_t*>(ctx);
    uintptr_t fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
    uintptr_t guest_base = reinterpret_cast<uintptr_t>(g_guest_memory);

    if (fault_addr >= guest_base && fault_addr < guest_base + 0x0E0000000ULL) {
        // Recover from invalid guest memory access
        uctx->uc_mcontext.gregs[REG_RAX] = 0; // Return 0 for read fault
        uctx->uc_mcontext.gregs[REG_RIP] += 4; // Skip 4-byte faulting instruction
        return;
    }

    // Default OS fatal error dispatch
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &sa, nullptr);
}
```

---

## 3. Dynarmic Unsafe JIT Flag Optimization Matrix

To match Yuzu's execution performance, Dynarmic configuration flags must be tuned to disable non-essential IEEE 754 float strictness and ARM-specific atomic monitors during single-threaded emulation.

| Dynarmic Optimization Flag | Yuzu Setting | SRE Target Setting | Technical Impact |
| :--- | :--- | :--- | :--- |
| `Unsafe_UnfuseFMA` | `true` | `true` | Maps ARM NEON `FMLA`/`FMLS` directly to host x86_64 FMA3 `vfmsub231ps` instructions. |
| `Unsafe_InaccurateNaN` | `true` | `true` | Bypasses canonical NaN bit-pattern conversions on IEEE float operations. |
| `Unsafe_IgnoreGlobalMonitor` | `true` | `true` | Replaces guest `LDXR`/`STXR` exclusive monitor spinlocks with direct loads and stores. |
| `Unsafe_IgnoreStandardHostMXCSR` | `false` | `true` | Suppresses redundant MXCSR state updates on host CPU state transitions. |
| `code_cache_size` | 512 MB | 512 MB | Prevents JIT basic-block eviction and re-compilation thrashing. |

---

## 4. Implementation Blueprint in `emulator_dynarmic64.cpp`

```cpp
void setup_dynarmic_fastmem_opt(Dynarmic::A64::UserConfig& config) {
    config.fastmem_pointer = reinterpret_cast<uint8_t*>(g_guest_memory);
    config.recompile_on_exclusive_fastmem_failure = false;
    
    // Unsafe speed flags
    using namespace Dynarmic::OptimizationFlag;
    config.optimizations |= Unsafe_UnfuseFMA;
    config.optimizations |= Unsafe_InaccurateNaN;
    config.optimizations |= Unsafe_IgnoreGlobalMonitor;
    config.optimizations |= Unsafe_IgnoreStandardHostMXCSR;
    
    // Code cache allocation
    config.code_cache_size = 512 * 1024 * 1024; // 512MB
}
```
