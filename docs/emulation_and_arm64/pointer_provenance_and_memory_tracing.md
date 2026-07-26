# Pointer Provenance Trace: 0x46af94 (.bss Execution)

This document traces the memory allocation, relocation, and execution chain that led the emulated engine to attempt instruction execution at `PC = 0x46af94` (inside the guest `.bss` segment).

---

## 1. Physical Location: The Guest `.bss` Segment
*   **Virtual Address**: `0x0046af94` (without load base) / `0x0146af94` (loaded at base `0x1000000`).
*   **Section**: `.bss` (which spans `0x00464590` to `0x0046b7a8` in `libswordigo.so`).
*   **Properties**:
    *   NOBITS (does not occupy space in the ELF file).
    *   No-execute flags (`RW`).
    *   Zero-initialized at load time.
*   **Relocation Audit**: A complete scan of the dynamic relocations (`.rel.dyn` and `.rel.plt`) shows **zero relocations** targeting the `.bss` range. No initializers or code addresses are written to this range by the loader.

---

## 2. The Provenance & Corruption Chain

The execution of code at `0x46af94` is a classic symptom of **uninitialized structure type-confusion** caused by the absence of standard C library shims on the host bridge.

```
+-------------------------------------------------------------+
| 1. Object Initialization (C++ Code)                         |
|    - Engine allocates struct/functor on heap/stack.         |
|    - Tries to copy a data descriptor using memcpy().       |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
| 2. Silent Copy Failure (Bridge Layer)                       |
|    - memcpy() is resolved to bridge address 0xFF000xxx.     |
|    - Since there is no registered handler, it returns       |
|      as a NO-OP without copying any memory.                |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
| 3. Memory Layout Disalignment & Corruption                  |
|    - Struct fields remain uninitialized/misaligned.         |
|    - A data pointer field (holding the address of a static  |
|      global in .bss: 0x146af94) occupies the memory slot    |
|      where a function pointer or vtable was expected.       |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
| 4. Type Confusion Execution (Jump to .bss)                  |
|    - Engine attempts to invoke virtual method / callback.   |
|    - Reads the address 0x146af94 from the corrupted slot    |
|      and executes BLX (Branch and Link Exchange).           |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
| 5. Instruction Fault (PC = 0x46af94)                        |
|    - CPU jumps to 0x146af94 and attempts instruction decode |
|      on zero-filled memory.                                 |
|    - Unicorn faults with UC_ERR_INSN_INVALID.               |
+-------------------------------------------------------------+
```

---

## 3. Resolution Plan
1.  **Register standard C library shims** in the emulator bridge (`memcpy`, `memset`, `memmove`, `strlen`, `calloc`, `realloc`) to ensure proper memory copying and structure initialization.
2.  **Fix Pointer Truncation** (convert host 64-bit descriptors to guest 32-bit handles) to prevent memory address corruption.
