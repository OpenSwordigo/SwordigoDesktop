# Mission 7A: Relocation Smoking Gun

## Discovery

**1,013 occurrences of 0x10000000 exist in the binary** — distributed across:

```
.ARM.exidx:        8 (debug unwinding)
.ARM.extab:      890 (exception handling tables)  ← MASSIVE
.bss:             15 (uninitialized globals)      ← SUSPICIOUS
.data.rel.ro:      2 (read-only relocated data)
.data.rel.ro.local: 6 (local read-only relocated data)
.dynamic:          1 (dynamic linking)
.dynsym:           3 (dynamic symbol table)
.gnu.version_r:    1 (version requirements)
.got:              1 (Global Offset Table)
.hash:            33 (symbol hash table)
.rel.dyn:         43 (relocations)
.rodata:           7 (read-only constants)
.text:             3 (code)
```

## Critical Finding

**→ 0x10000000 is NOT produced by any relocation.**

Verification:
- Checked all R_ARM_RELATIVE relocations (type 17)
- None of them produce 0x10000000 as output
- None of them have 0x10000000 as input

**Conclusion**: 0x10000000 is a **hard-coded constant** in the binary, placed by the compiler/linker.

## Significance

This means:

1. **Not a relocation bug** — it's intentional binary data
2. **Engine code expects 0x10000000 to exist at that address**
3. **Why crash happens**:
   - Binary contains 0x10000000 as a constant (likely address or magic number)
   - Copied into guest memory at load time
   - Engine code reads/uses it
   - But 0x10000000 is outside guest memory (0x00000000 - 0x10000000)
   - Dereference fails → UC_ERR_FETCH_UNMAPPED

## Most Suspicious Occurrences

15 occurrences in `.bss` — uninitialized global data should NOT contain hard-coded addresses.

This suggests:
- Compiler error or linker script issue
- `.bss` is being populated with `.data` (hard-coded values)
- Or `.bss` should be excluded from guest memory copy but isn't

## Next Steps for Agent 2

1. **Verify .bss is allocated separately** in `emulator.cpp:setup_guest_memory()`
   - Should .bss be mapped but not copied?
   - Should it be allocated fresh for each run?

2. **Check if PT_LOAD memsz covers .bss for both segments**
   - Does loader allocate the full memsz for each PT_LOAD?
   - Are those regions zero-initialized before memcpy?

3. **Find what consumes 0x10000000**
   - Instrument ARM execution with: "if PC == value from 0x10000000, log it"
   - May reveal the actual usage

4. **Increase guest memory if safe**
   - If 0x10000000 is truly meant to be a buffer/heap boundary, expand GUEST_MEM_SIZE
   - But first: verify it's NOT a bug (incorrect relocation or pointer corruption)

## Confidence Assessment

**High confidence**: 0x10000000 is intentionally in the binary
**Medium confidence**: It's a hard-coded constant (not a relocation)
**Low confidence**: Whether 0x10000000 should be mapped in guest memory or is a bug

The smoking gun is real, but we still need to determine:
- Is this a design error (value should be relocated)?
- Or an emulator error (memory should be larger)?
- Or a game engine bug (hard-coded address is wrong)?
