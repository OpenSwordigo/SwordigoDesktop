# Mission 8B: Relocation Instrumentation

**Status**: Diagnostic prep (for Agent 2)
**Goal**: Capture relocation corruption in action

## Finding

.bss is correctly zero-initialized by loader ✓

But corrupted pointers appear after relocation.

**Hypothesis**: Relocations are writing bad values.

## What to Instrument

### 1. Relocation Processing
File: `src/loader/elf_loader.cpp`, function `relocate()`

Add logging BEFORE each relocation:
```cpp
int ElfLoader::relocate(so_module* mod) {
    auto process_rel = [&](Elf32_Rel* rel, int count, const char* section_name) {
        for (int i = 0; i < count; i++) {
            Elf32_Rel* r = &rel[i];
            uint32_t type = ELF32_R_TYPE(r->r_info);
            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            Elf32_Sym* sym = &mod->dynsym[sym_idx];
            uint32_t* ptr = (uint32_t*)(guest_base + mod->base_addr + r->r_offset);
            
            // ADD THIS:
            uint32_t before_value = *ptr;
            
            // ... existing relocation code ...
            
            uint32_t after_value = *ptr;  // Capture after
            
            // ADD THIS LOGGING:
            if (i < 50 || after_value > 0x10000000 || after_value == 0x100018c) {
                std::cout << "[Reloc] " << section_name << " #" << i << std::endl;
                std::cout << "  Type: " << type << " Offset: 0x" << std::hex << r->r_offset << std::dec << std::endl;
                std::cout << "  Before: 0x" << std::hex << before_value << std::dec << std::endl;
                std::cout << "  After:  0x" << std::hex << after_value << std::dec << std::endl;
                if (sym->st_name) {
                    std::cout << "  Symbol: " << (mod->dynstr + sym->st_name) << std::endl;
                }
                if (after_value == 0x100018c || after_value > 0x10000000) {
                    std::cout << "  ⚠ SUSPICIOUS VALUE!" << std::endl;
                }
            }
        }
    };

    process_rel(mod->reldyn, mod->num_reldyn, ".rel.dyn");
    process_rel(mod->relplt, mod->num_relplt, ".rel.plt");

    return 0;
}
```

### 2. Symbol Resolution
File: `src/loader/elf_loader.cpp`, function `resolve_all_to_bridge()`

Check what bridge addresses are being assigned:
```cpp
int ElfLoader::resolve_all_to_bridge(so_module* mod, JniBridge* bridge) {
    int sym_count = 0;
    auto process_rel = [&](Elf32_Rel* rel, int count) {
        for (int i = 0; i < count; i++) {
            Elf32_Rel* r = &rel[i];
            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            Elf32_Sym* sym = &mod->dynsym[sym_idx];
            uint32_t* ptr = (uint32_t*)(guest_base + mod->base_addr + r->r_offset);

            if (sym->st_shndx == SHN_UNDEF) {
                if (sym->st_name == 0) continue;
                const char* name = mod->dynstr + sym->st_name;
                if (name[0] == '\0') continue;
                
                uint32_t bridge_addr = bridge->get_address(name);
                *ptr = bridge_addr;
                sym_count++;
                
                // ADD THIS LOGGING:
                if (sym_count <= 30 || bridge_addr > 0xff000100) {
                    std::cout << "[Resolve] " << name << " -> 0x" << std::hex << bridge_addr << std::dec << std::endl;
                    if (bridge_addr > 0xff001000 || bridge_addr == 0) {
                        std::cout << "  ⚠ SUSPICIOUS BRIDGE ADDRESS!" << std::endl;
                    }
                }
            }
        }
    };

    process_rel(mod->reldyn, mod->num_reldyn);
    process_rel(mod->relplt, mod->num_relplt);
    
    std::cout << "[Resolve] Total external symbols resolved: " << sym_count << std::endl;

    return 0;
}
```

## Expected Output

When you rebuild and run with this logging, you should see:

```
[Resolve] Symbol1 -> 0xff000000
[Resolve] Symbol2 -> 0xff000010
...
[Reloc] .rel.dyn #0
  Type: 17 (R_ARM_RELATIVE)
  Offset: 0x464590
  Before: 0x00000000
  After: 0x01464590
...
[Reloc] .rel.dyn #N
  Type: 1 (R_ARM_ABS32)
  Offset: 0x464594
  Before: 0x00000000
  After: 0x100018c
  ⚠ SUSPICIOUS VALUE!
```

The suspicious value tells you which relocation creates the bad pointer!

## Debugging Path

1. Rebuild with logging
2. Run: `./swordigo_boot 2>&1 | tee boot_reloc.log`
3. Search log for "SUSPICIOUS"
4. Identify relocation #N that creates 0x100018c
5. Look at symbol involved
6. Check if relocation calculation is wrong
7. Verify bridge address is correct

## Success Criteria

You find which relocation produces 0x100018c and understand:
- Which symbol
- Which relocation type
- What calculation went wrong

That pins down the exact bug.

