# Unmapped Memory & Silent Failures in Dynarmic

I have investigated the `Exception at 0xf00010 type=1` crash. The findings are groundbreaking and explain the bizarre control-flow corruption.

## 1. The Crash Trace
At the time of the crash:
- `X30` (Link Register) is `0x14643c0` (`Caver::Scene::FinishLoad()`).
- `X21` is `0x8000000042700000`. This is not a pointer! It is exactly the 64-bit combination of two 32-bit floats: `-0.0f` and `60.0f`.
- The instruction immediately before the return address (`0x14643bc`) is `blr x8`, which calls a virtual destructor.
- `blr x8` jumped to `0xf00010`, which happens to be an ASCII string in memory (`"/home/quantumcreeper/.local/share..."`).
- The CPU tried to execute the string `"eper"` (`0x72657065`), which is an invalid ARM64 instruction, triggering `Exception::UnallocatedInstruction` (type 1).

## 2. The True Root Cause: Silent Unmapped Memory Reads
How did `blr x8` jump to `0xf00010` when `X21` was a float?
Look at `emulator_dynarmic64.cpp`:
```cpp
    std::uint64_t MemoryRead64(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr + 7 < mem_size) return ...;
        return 0; // <--- SILENT FAILURE
    }
```
Dynarmic's memory callbacks do not throw a `MemoryAbort` when the guest reads unmapped memory! Instead, they **silently return 0**.

Here is exactly what happened:
1. `X21` was corrupted by a float array (e.g., a bad union/cast or use-after-free), becoming `0x8000000042700000`.
2. `ldr x8, [x21]` attempted to read from `0x8000000042700000`. Because this is out of bounds, `MemoryRead64` silently returned `0`.
3. `x8` became `0`.
4. `ldr x8, [x8, #8]` attempted to read the destructor address from `vtable[1]`. Since `x8` was `0`, it read from address `0x8`.
5. Due to a previous null-pointer write somewhere in the guest, address `0x8` contained `0xf00010`.
6. `blr x8` jumped to `0xf00010`.

## 3. The Fix
We must modify the `SwordigoMemory` callbacks in `emulator_dynarmic64.cpp` to **throw `Dynarmic::HaltReason::MemoryAbort`** (or invoke a fault handler) instead of silently returning `0` or ignoring writes. 

By making unmapped memory accesses fatal, the emulator will crash *exactly* at the instruction that dereferences the corrupted pointer (e.g., `ldr x8, [x21]`), rather than executing a chain of zero-reads that culminate in jumping into ASCII strings!
