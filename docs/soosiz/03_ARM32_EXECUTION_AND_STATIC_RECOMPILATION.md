# 03: Mach-O Loading & Dynarmic ARM32 Execution Architecture

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/03_ARM32_EXECUTION_AND_STATIC_RECOMPILATION.md`  
> **Status:** Remastered Mach-O Loader & ARM32 JIT Specification  
> **Target Executable:** `SoosizHD` (Mach-O ARMv7 32-bit Binary)

---

## 1. Overview: Direct Mach-O Binary Execution Model

Instead of recompiling source code, OpenSwordigo Desktop uses a **Mach-O Binary Loader & ARM32 Execution Engine** built into `src/loader/`.

This subsystem loads the raw `SoosizHD` Mach-O binary into host virtual memory, resolves dynamic framework symbols to native host C functions, and uses **Dynarmic ARM32 JIT** to execute ARMv7 instructions at native host CPU speeds.

---

## 2. Mach-O Loader Step-by-Step Pipeline

```
  Step 1: Open & Parse Mach-O Header (0xFEEDFACE)
        |
  Step 2: Allocate Segment Memory Pages (__TEXT, __DATA)
        |
  Step 3: Read Segment Payloads from SoosizHD into RAM
        |
  Step 4: Set Memory Protection (mprotect: RX for __TEXT, RW for __DATA)
        |
  Step 5: Parse Dyld Symbol Stubs & Perform Import Relocations
        |
  Step 6: Initialize Dynarmic ARM32 JIT (PC = 0x2100)
```

### Detailed Loader Tasks:
1. **Header Validation:** Verify magic `0xFEEDFACE`, `CPU_TYPE_ARM` (12), `CPU_SUBTYPE_ARM_V7` (9).
2. **Segment Mapping:**
   - Map `__PAGEZERO` (`0x0 - 0x1000`): Guard page (`PROT_NONE`).
   - Map `__TEXT` (`0x1000 - 0xC8000`): Code segment (`PROT_READ | PROT_EXEC`).
   - Map `__DATA` (`0xC8000 - 0xEE000`): Data segment (`PROT_READ | PROT_WRITE`).
3. **Symbol Table Parsing:** Parse `LC_SYMTAB` and `LC_DYSYMTAB` to locate non-lazy and lazy symbol pointers (`__DATA.__nl_symbol_ptr` and `__DATA.__lazy_symbol`).

---

## 3. Dynarmic ARM32 JIT Engine Integration

**Dynarmic** is an ultra-fast, modern ARM32/ARM64 JIT translator library written in C++. It translates ARMv7 assembly instructions into host x86_64 or ARM64 machine instructions on the fly.

### Dynarmic Execution Context in OpenSwordigo (`src/loader/arch_detect.h`):

```cpp
#include "dynarmic/frontend/A32/a32_location.h"
#include "dynarmic/interface/A32/a32.h"

class SoosizArm32UserCallbacks : public Dynarmic::A32::UserCallbacks {
public:
    uint32_t MemoryRead32(uint32_t vaddr) override {
        return *reinterpret_block_ptr<uint32_t*>(vaddr);
    }
    void MemoryWrite32(uint32_t vaddr, uint32_t value) override {
        *reinterpret_block_ptr<uint32_t*>(vaddr) = value;
    }
    // Intercept SVC / Syscall or Host Trampolines
    void CallSVC(uint32_t swi) override {
        Soosiz_HandleArm32Syscall(swi);
    }
};

void Soosiz_LaunchBinaryJIT() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = new SoosizArm32UserCallbacks();
    
    Dynarmic::A32::Jit jit(config);
    jit.Regs()[15] = 0x2100; // Set PC to start()
    jit.Regs()[13] = 0x700FF000; // Set SP
    
    // Execute JIT loop
    jit.Run();
}
```

---

## 4. Symbol Trampoline & Call Forwarding

When `SoosizHD` binary calls external dylib functions (e.g. `objc_msgSend`, `glDrawArrays`, `sqlite3_open`), the execution hits a symbol stub in `__TEXT.__symbolstub1`.

Our loader replaces the stub target address with an **ARM32 SVC (Supervisor Call) Trampoline**:

```arm
// ARM32 Trampoline written into symbol stub by loader
SVC #0xFE   // Triggers Dynarmic CallSVC callback
NOP
```

When Dynarmic encounters `SVC #0xFE`:
1. Reads `R0`, `R1`, `R2`, `R3` register values from Dynarmic CPU context.
2. Identifies target function ID (e.g. `glDrawArrays`).
3. Executes host C function (`sre_glDrawArrays(R0, R1, R2)`).
4. Writes return value back to `R0` register and resumes ARM32 execution seamlessly!
