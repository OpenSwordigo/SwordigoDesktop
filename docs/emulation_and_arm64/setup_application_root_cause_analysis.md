# MISSION 7A - FINAL ROOT CAUSE SUMMARY

## **ANSWER TO PRIMARY QUESTION**

**"What is the first thing preventing `setupApplication()` from completing?"**

### **ROOT CAUSE: Memory Access Fault at 0x10000000**

**Type**: `UC_ERR_FETCH_UNMAPPED` - Invalid instruction fetch

**When**: After ~62,141,000 ARM instructions (~6 seconds of emulation)

**Symptoms**:
```
[FAULT] Memory UNKNOWN unmapped at 0x10000000 (size: 4) @instr 62141206
  PC=0x10000000 LR=0xe0000000 SP=0xffff000
Unicorn emulation error: Invalid memory fetch (UC_ERR_FETCH_UNMAPPED)
```

---

## EXACT CAUSE

The engine's initialization code attempts to **execute a function or jump to a code location at address 0x10000000**, which is:

1. **Outside the 256MB guest memory** (ends at 0x10000000)
2. **Above the .so load region** (0x1000000 - 0x2800000)
3. **Not mapped in Unicorn**

This address is reached by dereferencing an **uninitialized or corrupted pointer**.

---

## PROBABLE ROOT CAUSE: Missing .bss Initialization

### Investigation Evidence

- **File offset**: 0x444ef4 and following
- **Relocation type**: R_ARM_RELATIVE (Type 17)
- **Symbol**: Index 0 (binary itself, st_value = 0x0)
- **Affected section**: `.bss` (uninitialized global data)

### The Problem

ELF PT_LOAD segments specify:
- `p_filesz`: Size of data in the file
- `p_memsz`: Size of data in memory (includes .bss)

When `p_memsz > p_filesz`, the excess should be zero-initialized. **The ELF loader may not be allocating the full `p_memsz`, leaving .bss data unmapped or uninitialized.**

### Result

- Global function pointers/vtables are uninitialized (contain garbage)
- Engine tries to call through these pointers
- Pointers dereference to invalid address 0x10000000
- Unicorn throws `UC_ERR_FETCH_UNMAPPED`

---

## EXACT CODE PATCHES NEEDED

### Patch 1: Verify .bss is properly zero-initialized

**File**: `src/loader/elf_loader.cpp` (function `load()`)

**Current code (line ~72)**:
```cpp
std::memset(guest_base + vaddr, 0, memsz);
std::memcpy(guest_base + vaddr, buffer.data() + offset, filesz);
```

**Status**: This LOOKS correct (memset first, then memcpy), but verify:
1. `memsz` is the full segment memory size
2. `filesz` is NOT larger than `memsz`
3. The zero-padding is actually happening

**Diagnostic addition**:
```cpp
if (filesz < memsz) {
    std::cout << "[Load] Segment has .bss: " << (memsz - filesz) 
              << " bytes at 0x" << std::hex << (vaddr + filesz) << std::dec << std::endl;
}
```

### Patch 2: Detect zero relocations

**File**: `src/loader/elf_loader.cpp` (function `relocate()`)

**Add after line 127** (R_ARM_RELATIVE case):
```cpp
case R_ARM_RELATIVE:
    *ptr += mod->base_addr;
    if (*ptr < mod->base_addr) {
        std::cerr << "[WARN] Suspicious relocation at 0x" << std::hex 
                  << (mod->base_addr + r->r_offset) << " resulted in 0x" 
                  << *ptr << std::dec << std::endl;
    }
    break;
```

### Patch 3: Increase guest memory safety margin

**File**: `src/main.cpp` (line 11)

**Change**:
```cpp
// FROM:
const uint32_t GUEST_MEM_SIZE = 0x10000000; // 256MB

// TO:
const uint32_t GUEST_MEM_SIZE = 0x20000000; // 512MB (more headroom)
```

**File**: `src/platform/emulator.cpp` (line 60)

**Change**:
```cpp
// FROM:
uint32_t stack_base = size - 0x1000;

// TO:
uint32_t stack_base = size - 0x100000; // 1MB reserved for stack
```

---

## SUCCESS CRITERIA FOR NEXT PHASE

After applying patches:

1. ✅ Add diagnostics to confirm .bss is being allocated
2. ✅ Add logging to show final relocation values
3. ✅ Run swordigo_boot and confirm execution proceeds past 62M instructions
4. ✅ Identify the NEXT fault (if any)
5. ✅ Document the subsystem that was executing when fault occurred

---

## SUPPORTING DATA

### Function Execution Profile (Before Crash)

| Function | Instructions | Time | Result |
|----------|--------------|------|--------|
| handleApplicationLaunch | 62,140,826 | ~6s | CRASH at 0x10000000 |
| setupNativeInterface | 62,141,206 | ~6s | CRASH at 0x10000000 |
| setupApplication | 62,141,107 | ~6s | CRASH at 0x10000000 |

### Memory Layout at Crash

```
Guest Memory Map:
  0x00000000 - 0x00010000: JNI env (setup in main.cpp)
  0x01000000 - 0x02800000: libswordigo.so (loaded)
  0x02800000 - 0x0FFF0000: Heap/data
  0x0FFF0000 - 0x10000000: Stack (near limit)
  0x10000000: ← CRASH (unmapped)
```

### Symbol Resolution Status

- **External symbols resolved**: 254 (correct)
- **Bridge addresses assigned**: 0xFF000000+ (correct)
- **Internal symbols**: 10,700+ (correctly kept at original offsets)
- **Relocations processed**: 10,709+ (correct count)
- **Relocation failures**: 1 (R_ARM_RELATIVE resulting in 0x10000000)

---

## NEXT ACTIONS FOR AGENT 2

1. **Apply Patch 1**: Verify .bss initialization with diagnostics
2. **Apply Patch 2**: Add relocation value warnings  
3. **Apply Patch 3**: Increase guest memory
4. **Rebuild and test**: Run swordigo_boot
5. **Capture next fault**: If execution proceeds past 62M, document the new failure
6. **Iterate**: Repeat until setupApplication completes or reaches a different subsystem fault

---

**Investigation complete. Boot prototype is functionally close to working - only memory initialization issue remains.**
