# Runtime Integrity Audit

This report presents the findings of the **Mission 17A Runtime Integrity Audit** for the *Swordigo* Linux port. It establishes the current state of execution stability and details core system audits.

---

## 1. Executive Summary
The emulated engine currently executes **1.15 million instructions** in `drawApplication` before failing with `UC_ERR_INSN_INVALID` at `PC = 0x46af94`. 

Our audit has diagnosed that the crash is not caused by dynamic relocation type failures, but rather by **silent memory copy/set failures** inside the JNI bridge. The absence of registered handlers for basic dynamic C functions (like `memcpy`, `memset`, and `strlen`) causes structures and callbacks to remain uninitialized. This results in type-confusion where a data pointer (pointing to a global `.bss` address at `0x46af94`) is called as a function pointer.

---

## 2. Dynamic Relocations Audit
*   **Inventory**: We mapped every relocation in `libswordigo.so` and confirmed that only four types are present: `R_ARM_ABS32`, `R_ARM_JUMP_SLOT`, `R_ARM_RELATIVE` (Type 23), and `R_ARM_GLOB_DAT`.
*   **Relocation Type 23**: Identified exactly as `R_ARM_RELATIVE`. It is handled correctly in the custom loader by adding the base address (`*ptr += base_addr`).
*   **Loader Support**: The loader fully implements all four relocation types present in the binary. No unrecognized relocation types are causing the invalid program counter.

---

## 3. `.bss` Segment Initialization
*   **Ordering**: Loader segment processing is correct. Segment 02 (LOAD RW) mem-sizes `.bss` space to `0x7218` bytes. The loader clears the entire segment allocation size (`memsz = 0x268b8`) with `memset(..., 0, memsz)` before copying the file contents (`filesz = 0x1f6a0`), ensuring the `.bss` section is clean and zero-initialized prior to relocations.
*   **Corruption Origin**: No relocation entries modify the `.bss` range directly. Jumps to the `.bss` range arise dynamically during runtime execution due to object structure corruption.

---

## 4. JNI Vtable & Nesting Audits
*   **JNI Vtable Offsets**: We verified the vtable indexes in `src/main.cpp` against standard Android NDK `jni.h` declarations. All offsets are exactly aligned:
    *   `FindClass`: Index 6 (`0x18`)
    *   `NewGlobalRef` / `DeleteGlobalRef`: Indexes 21/22 (`0x54`/`0x58`)
    *   `GetObjectClass` / `GetMethodID`: Indexes 31/33 (`0x7C`/`0x84`)
    *   `GetStaticMethodID` / `GetStringUTFChars`: Indexes 113/169 (`0x1C4`/`0x2A4`)
    *   `RegisterNatives`: Index 215 (`0x35C`)
*   **Nested Call Register Corruption**: Currently, `Emulator::handle_bridge_call` executes the bridge handler and *then* reads the Link Register (`LR`). If the handler invokes nested guest calls, the physical `LR` register is overwritten. This leaves the outer caller returning to an incorrect, corrupted instruction pointer.

---

## 5. Introspection Console & Health Checks
*   **Independent Boot Diagnostics**: Validated that `ProgramState::ExecuteString("print('hello')")` runs successfully at boot time prior to loading scenes or initializing GLES. 
*   **Automated Health Check**: This allows us to use `ExecuteString` as a fast, non-graphical unit test checking compiler, JIT, VM, and memory layout sanity before running long rendering loops.
