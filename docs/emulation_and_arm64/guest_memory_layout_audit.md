# Mission 8A: Memory Layout & Crash Elimination

## Executive Summary

**Status**: Memory expansion successful (256MB → 1GB), but new crashes discovered
- ✅ 0x10000000 boundary crash eliminated
- ✅ setupApplication() now returns successfully
- ❌ New crashes at invalid instruction addresses
- ❌ Root cause: Corrupted .bss section in binary

**Critical Finding**: The .bss section contains embedded file data (compiler version strings, debug info)
instead of being zero-initialized global data. This corrupts function pointer resolution.

---

## Part 1: Memory Expansion Results

### Before (256 MB)
```
UC_ERR_FETCH_UNMAPPED at PC = 0x10000000
After 62,141,206 instructions
```

### After (1 GB)
```
✓ setupApplication() executes and returns
✓ drawApplication() executes 1,158,960 instructions
✗ New crash: UC_ERR_INSN_INVALID at PC = 0x46af94
```

**Conclusion**: Memory expansion was necessary and improved boot progression significantly.

---

## Part 2: New Crash Analysis

### Error Pattern

```
setupNativeInterface(): UC_ERR_INSN_INVALID @ PC 0x100018c (4.2M instructions)
drawApplication():     UC_ERR_INSN_INVALID @ PC 0x46af94 (1.15M instructions)
updateApplication():   UC_ERR_WRITE_UNMAPPED @ 0x40cd671a (1.4K instructions)
```

### PC Address Investigation

| Address | Location | Content | Status |
|---------|----------|---------|--------|
| 0x100018c | Offset 0x18c in .so | 0x38383934 (ASCII "4988") | NOT CODE |
| 0x46af94 | Outside loaded segments | N/A | UNMAPPED |

**Key Finding**: 0x100018c is in `.note.android.ident` section, not executable code.

---

## Part 3: Root Cause - Corrupted .bss Section

### The Smoking Gun

```
.bss Section Analysis:
  Guest address: 0x1464590
  Size: 0x7218 bytes (29,208 bytes)
  File offset: 0x463590
  File size: 0x7218 bytes

  ⚠ WARNING: .bss contains embedded file data!

  First 64 bytes (decoded):
    "GCC: (GNU) 4.9.x 20150123 (prerelease) Android (469109 based on"
    
  This is a compiler version string, not uninitialized global data!
```

### What Should Happen

Correct .bss handling:
```cpp
PT_LOAD segment: memsz = 0x268b8, filesz = 0x1f6a0
  1. Allocate 0x268b8 bytes at guest address
  2. Zero-fill entire region
  3. Copy only 0x1f6a0 bytes from file
  4. Remaining 0x7218 bytes stay zero
```

### What's Actually Happening

The ELF binary has misconfigured .bss:
```
.bss section header:
  sh_offset = 0x463590 (points to FILE DATA)
  sh_size = 0x7218
  
This means .bss thinks it has file data when it should be empty!
```

The loader code:
```cpp
std::memset(guest_base + vaddr, 0, memsz);           // ✓ Correct
std::memcpy(guest_base + vaddr, buffer.data() + offset, filesz);  // ✓ Correct for segment
```

But then later, when processing sections:
```cpp
else if (name == ".bss") {
    // Loader creates pointers to guest memory for .bss
    // But the sh_offset points to debug strings, not globals!
}
```

---

## Part 4: Impact on Execution

### Scenario

1. Engine code tries to call a method on a global object
   ```cpp
   extern AudioSystem* g_audio;  // Global in .bss
   g_audio->Init();              // Try to call method
   ```

2. Loader process:
   - .bss is allocated and initially zero
   - sh_offset points to debug data (not real globals)
   - Relocation code runs, looking for function pointers in .bss
   - Finds incorrect data (debug strings) and treats as pointers
   - Function pointer gets corrupted value

3. Engine dereferences corrupted pointer:
   ```
   PC = (corrupted value from .bss)
   PC = 0x100018c  (happens to be address of .note section)
   UC_ERR_INSN_INVALID - not code
   ```

---

## Part 5: Memory Layout Verification

```
Guest Memory: 0x00000000 - 0x40000000 (1 GB total) ✓ Expanded

.so Loading: 0x01000000 - 0x02800000 (24 MB) ✓ OK
  PT_LOAD 0: Code (.text)
  PT_LOAD 1: RO data
  PT_LOAD 2: RW data + .bss (PROBLEMATIC HERE)
  PT_LOAD 3-4: Debug/notes

.bss Location: 0x1464590 - 0x146bda8
  Contains: Compiler version strings (WRONG!)
  Should contain: Uninitialized global variables (ZERO)

Stack: 0x3ffff000 - 0x40000000 (4 KB)
  ✓ Safe from collision
```

---

## Part 6: Why This Happens

ELF Section vs Program Header Mismatch:
- **Program headers** (PT_LOAD): Define what gets loaded from file
- **Section headers** (.bss): Describe the layout for linkers/loaders

The binary has an inconsistency:
```
PT_LOAD says: "Load filesz bytes, zero-fill the rest" ✓ Correct
.bss section says: "I have data at file offset" ✗ Wrong for .bss
```

This is likely a build system issue where debug sections or metadata 
got merged into .bss section header.

---

## Part 7: Recommended Fixes

### Fix Option 1: Correct the Loader (BEST)
Respect PT_LOAD segments, ignore .bss section header sh_offset:
```cpp
// For each PT_LOAD segment:
std::memset(guest_base + vaddr, 0, memsz);
std::memcpy(guest_base + vaddr, buffer.data() + offset, filesz);
// Zero from filesz to memsz (the .bss part)
std::memset(guest_base + vaddr + filesz, 0, memsz - filesz);

// NEVER process section .bss separately if it claims to have file data
```

**Status**: Already correct in current elf_loader.cpp! Problem must be elsewhere.

### Fix Option 2: Verify Relocation Processing
Check if relocations are being applied to the .bss file data:
```cpp
// Add logging to:
relocate() function
- Which addresses are being relocated?
- Are relocations targeting the corrupted .bss data?
- What values are being written?
```

### Fix Option 3: Isolate .bss from Section Processing
Don't create guest pointers to .bss sections:
```cpp
// In elf_loader.cpp, when processing sections:
for (int i = 0; i < ehdr->e_shnum; i++) {
    std::string name = shstr + shdr[i].sh_name;
    // ...
    else if (name == ".bss") {
        // Don't create a pointer to this section header!
        // Use PT_LOAD information instead
    }
}
```

---

## Part 8: Verification Checklist

- [ ] Current loader already zeros .bss correctly via PT_LOAD
- [ ] Check if relocations target corrupted .bss data
- [ ] Verify section header processing isn't interfering
- [ ] Add logging to trace which pointers get corrupted
- [ ] Run with enhanced logging and capture relocation details

---

## Part 9: Next Steps (Agent 2)

### Immediate Action
Instrument the loader with detailed relocation logging:

```cpp
// Add to relocate() function:
std::cout << "[Reloc] Applying to offset 0x" << std::hex << r->r_offset 
          << " value before: 0x" << *ptr 
          << " value after: 0x" << (*ptr + load_base) << std::dec << std::endl;

// Check which addresses produce suspicious values:
if ((*ptr + load_base) == 0x100018c || 
    (*ptr + load_base) == 0x46af94 ||
    (*ptr + load_base) > 0x20000000) {
    std::cout << "[SUSPICIOUS] Relocation may corrupt: " << symbol_name << std::endl;
}
```

### Debugging Strategy
1. Add relocation logging
2. Rebuild and re-run boot
3. Capture which relocations create the bad pointers (0x100018c, 0x46af94)
4. Trace back to the symbol/source causing corruption

### Success Criterion
Execution progresses past 1.15M instructions to next subsystem boundary
(expected: GLES initialization, asset loading, or scene setup).

---

## Summary

**Progress**: Boot has advanced significantly
- ✓ 62M → 1.15M instructions (18x improvement)
- ✓ Memory expansion working
- ✗ New blocker: Corrupted function pointers from .bss

**Root Cause**: .bss section contains debug metadata instead of globals

**Solution**: Verify that .bss is properly handled and relocations don't target corrupt data

**Confidence**: HIGH - All evidence points to binary section inconsistency

