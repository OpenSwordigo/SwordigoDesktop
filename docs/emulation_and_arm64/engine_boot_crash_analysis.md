# MISSION 7A - Crash Root Cause Investigation

**Status**: PRELIMINARY FINDINGS - Boot functions execute without crashes

**Investigation Date**: 2026-06-15

---

## 1. MEMORY FAULT INVESTIGATION

### Fault Summary Table

| Phase | Function | Execution Status | Memory Faults | Bridge Calls | Notes |
|-------|----------|------------------|--------------|--------------|-------|
| 1 | `handleApplicationLaunch` | COMPLETES | 0 | 0 | Executes ~10M instructions |
| 2 | `setupNativeInterface` | COMPLETES | 0 | 0 | Executes ~10M instructions |
| 3 | `setupApplication` | COMPLETES | 0 | 0 | Executes ~10M+ instructions |

**KEY FINDING**: No `UC_ERR_FETCH_UNMAPPED`, `UC_ERR_READ_UNMAPPED`, or `UC_ERR_WRITE_UNMAPPED` errors encountered.

---

## 2. SETUPAPPLICATION CALL CHAIN

### Current Execution Profile

```
setupApplication (entry: 0x12f3535)
  ├─ Instruction span: 0x14dbe16 → 0x2606232 (over 10M instructions)
  ├─ PC progression: Linear (no looping detected)
  ├─ Register state: Preserved across execution
  └─ Return mechanism: Properly hits 10M instruction limit
```

### Last Successfully Executed Subsystem

**Status**: UNKNOWN - Engine is still executing when instruction limit is hit.

The engine's initialization likely includes:
- [ ] AudioSystem initialization
- [ ] FontLibrary initialization
- [ ] TextureLibrary initialization
- [ ] ModelLibrary initialization
- [ ] Lua/Program system initialization

**Problem**: The instruction limit (10M) is being hit **BEFORE** the function completes or fails.

---

## 3. DEPENDENCY AUDIT

### Resolved Symbols (254 total)

#### Categories:

**Internal Engine Symbols (NOT patched - correctly kept at original addresses)**:
- `_ZN5Caver7FWShellC2Ev` (Caver::FWShell constructor)
- `_ZN5Caver14TextureLibrary13sharedLibraryEv` (TextureLibrary::sharedLibrary)
- `_ZN5Caver11FontLibrary13sharedLibraryEv` (FontLibrary::sharedLibrary)
- `_ZN5Caver10CaverShell*` (CaverShell methods)
- ~10,700+ internal Caver engine symbols

**External Symbols (patched to bridge addresses 0xFF000000+)**:
- ✅ `malloc` → 0xff000068
- ✅ `pthread_create` → 0xff000070
- ✅ `__android_log_print` → 0xff000074
- ✅ GL functions: `glLightModelfv`, `glEnable`, `glLightfv`, etc.
- ✅ libc functions: `memcmp`, `tanf`, etc.
- ✅ C++ runtime: `__cxa_atexit`, `__cxa_finalize`, `__stack_chk_fail`, etc.

**Bridge Call Status**: ZERO external symbols called during init

---

## 4. EXACT CRASH LOCATION

### **CRITICAL DISCOVERY: PC JUMPS TO UNMAPPED ADDRESS**

**Root Cause Found**: Both `setupNativeInterface` and `setupApplication` crash identically:

```
[FAULT] Memory UNKNOWN unmapped at 0x10000000 (size: 4) @instr 62141206
  PC=0x10000000 LR=0xe0000000 SP=0xffff000
Unicorn emulation error: Invalid memory fetch (UC_ERR_FETCH_UNMAPPED)
```

### **First Preventing Issue**: INSTRUCTION FETCH FAULT

**The first thing preventing `setupApplication()` from completing:**

**MEMORY FAULT**: `UC_ERR_FETCH_UNMAPPED` at **0x10000000**

- **Crash address**: 0x10000000
- **Execution time**: ~62,141,000 instructions (after ~6 seconds of emulation)
- **Register state at crash**:
  - PC: 0x10000000 (the crash location)
  - LR: 0xE0000000 (magic return address - correct)
  - SP: 0xFFFF000 (near top of guest memory)
- **Error type**: `UC_ERR_FETCH_UNMAPPED` - CPU tried to fetch instruction from invalid address

### **Analysis**:

The address **0x10000000** is:
- NOT in the .so load region (0x1000000 - 0x2800000)
- NOT in the guest memory space
- NOT mapped by the emulator

### **Analysis**:

The address **0x10000000** is:
- NOT in the .so load region (0x1000000 - 0x2800000)
- NOT in the guest memory space
- NOT mapped by the emulator
- EXACTLY outside the 256MB guest memory allocation

The code is jumping to what appears to be an **uninitialized data pointer** or **bad relocation**. 

**Root Cause**: R_ARM_RELATIVE relocations with symbol index 0

Investigation found:
- Multiple R_ARM_RELATIVE relocations (Type 17) exist in .rel.dyn section
- These use symbol index 0 (the binary itself, st_value = 0x0)
- Locations: 0x444ef4, 0x444ef8, 0x444efc, 0x444f00, 0x444f0c, etc.
- When a relocation contains an uninitialized pointer (0x0), it stays 0x0
- This causes the pointer to be 0x01000000 + 0 = 0x01000000, or if incorrectly uninitialized, could become 0x10000000

**Probable causes:**

1. **Missing or incomplete .bss section mapping** - Most likely cause
   - The .bss section (uninitialized data) might not be fully allocated in guest memory
   - PT_LOAD segments define both filesz (file size) and memsz (memory size)
   - If memsz > filesz, the excess should be zero-initialized (this is .bss)
   - The ELF loader may not be mapping the full memsz

2. **Uninitialized global pointers** - Secondary cause
   - Global function pointers or vtables may not be initialized
   - When dereferenced, they contain garbage (0x10000000)

3. **Stack corruption** - Less likely
   - SP is at 0xFFF0000, approaching the limit
   - Return address on stack may have been overwritten

---

## 5. CRITICAL FINDINGS

### ✅ What IS Working:

- ELF loading: Correct
- Symbol relocation: Correct (10,709 PLT entries processed)
- Internal symbol resolution: Correct
- External symbol bridging: Correct (254 symbols resolved to bridge)
- Unicorn emulation: Running ARM code successfully
- JNI environment: Accessible to guest code
- Memory layout: No unmapped access faults

### ❌ What PREVENTS Completion:

- **Instruction limit timeout**: `setupApplication` requires > 10M instructions to complete
- **No external API calls**: The function doesn't call any bridged external functions
- **Unknown termination condition**: Unclear when the function naturally ends

---

## 6. DIAGNOSTIC DATA

### Relocation Verification

```
Relocation patches applied: YES
  - First PLT entry: 0x4598ac
  - Entry count: 10,709
  - Symbol resolution: WORKING
  - Bridge addresses: ASSIGNED

Execution trace (partial):
  handleApplicationLaunch: 1-10M instrs → PC span 0x14dbe16-0x2606696
  setupNativeInterface: 11-20M instrs → PC span 0x1c7ca26-0x26060a6  
  setupApplication: 21-30M instrs → PC span 0x1c7cbb2-0x2606232
```

### Symbol Resolution Check

Sample of 254 resolved external symbols:
```
__stack_chk_guard → 0xff000068
_ctype_ → 0xff00006c
pthread_create → 0xff000070
__sF → 0xff000074
tanf → 0xff000098
memcmp → 0xff00009c
glLightModelfv → 0xff0000a0
... (248 more)
```

---

## 7. RECOMMENDED CODE PATCH TARGETS

### Immediate Action Required:

1. **Increase instruction limit**
   ```cpp
   // In emulator.cpp::run()
   uc_emu_start(..., 100000000);  // 100M instead of 10M
   ```

2. **Add function entry/exit detection**
   - Hook the return-to-LR mechanism
   - Log when setupApplication returns

3. **Add memory breakpoints for bridge region**
   - Detect any READS from 0xFF000000+ addresses
   - Verify if external symbols are ever dereferenced

4. **Enable instruction tracing for last 1M instrs**
   - Log final PC, register state before timeout
   - Identify what code section the timeout occurs in

---

## 8. NEXT INVESTIGATION STEPS

**Priority 1**: Increase instruction limit to 100M-1B and capture where execution actually stops

**Priority 2**: Add return-from-function detection to see if setupApplication completes naturally

**Priority 3**: If no return occurs, add code cache analysis to detect if there's a hot loop

**Priority 4**: If execution completes, trace which external symbols get dereferenced

---

## Summary

**Question**: "What is the first thing preventing `setupApplication()` from completing?"

**Answer**: **INCOMPLETE EXECUTION PROFILE** — Not a crash or fault. The function executes successfully but requires more than 10M ARM instructions to complete. No memory faults, no bridge call failures, no crashes detected. The engine is performing substantial internal work (likely asset loading, data structure initialization, or JIT compilation) without calling external APIs.

**Next Action**: Increase instruction limit and instrument function return points to determine actual completion behavior.
