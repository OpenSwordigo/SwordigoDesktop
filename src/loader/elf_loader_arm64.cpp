#include "elf_loader_arm64.h"
#include "../jni/jni_bridge_arm64.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

// ============================================================================
// ARM64 ELF Loader
//
// Key differences from ARM32 ELF loader:
//   - Elf64_* structs (different field sizes and order)
//   - RELA relocations (with addend) instead of REL
//   - Section names: .rela.dyn/.rela.plt instead of .rel.dyn/.rel.plt
//   - 64-bit addresses throughout
//   - Different relocation types (R_AARCH64_*)
//   - Elf64_Sym has different field order than Elf32_Sym
//   - .init_array contains 8-byte pointers instead of 4-byte
// ============================================================================

ElfLoaderArm64::ElfLoaderArm64(uint8_t* guest_mem_base, uint64_t guest_mem_size)
    : guest_base(guest_mem_base), guest_limit(guest_mem_size) {}

int ElfLoaderArm64::resolve_all_to_bridge(so_module_arm64* mod, JniBridge64* bridge, uint64_t globals_base) {
    int sym_count = 0;
    int resolved = 0, unresolved = 0;
    uint64_t next_global = globals_base;

    // ARM64 uses RELA (with explicit addend) instead of REL
    auto process_rela = [&](Elf64_Rela* rela, int count, const char* section) {
        for (int i = 0; i < count; i++) {
            Elf64_Rela* r = &rela[i];
            uint64_t sym_idx = ELF64_R_SYM(r->r_info);
            uint64_t type = ELF64_R_TYPE(r->r_info);
            Elf64_Sym* sym = &mod->dynsym[sym_idx];
            // Write 64-bit pointers into the GOT/PLT
            uint64_t* ptr = (uint64_t*)(guest_base + mod->base_addr + r->r_offset);

            if (sym->st_shndx == SHN_UNDEF) {
                if (sym->st_name == 0) continue;
                const char* name = mod->dynstr + sym->st_name;
                if (name[0] == '\0') continue;
                
                // Strip @version if present
                std::string sname(name);
                size_t at_pos = sname.find('@');
                if (at_pos != std::string::npos) {
                    sname = sname.substr(0, at_pos);
                }
                
                // Known data symbols → map to guest globals area
                bool is_data = (sname == "__stack_chk_guard" || sname == "_ctype_" || sname == "__sF");
                
                if (is_data && globals_base != 0) {
                    *ptr = next_global;
                    next_global += 16; // 64-bit aligned globals
                } else {
                    // ARM64: __aeabi_* helpers don't exist — skip silently
                    if (sname.substr(0, 7) == "__aeabi") {
                        continue;
                    }
                    
                    uint64_t bridge_addr = bridge->get_address(sname.c_str());
                    if (bridge_addr != 0) {
                        *ptr = (uint64_t)bridge_addr;
                        resolved++;
                        // Debug: log first few resolutions
                        if (resolved <= 5) {
                            std::cout << "[Resolve/ARM64] " << section << "[" << i << "] " << sname
                                      << " -> GOT@0x" << std::hex << (mod->base_addr + r->r_offset) 
                                      << " = 0x" << bridge_addr << std::dec << std::endl;
                        }
                    } else {
                        // Auto-stub: register a dynamic stub bridge so the GOT
                        // points to valid bridge code instead of dangling at 0.
                        // The stub logs the call and returns 0.
                        bridge->register_handler(sname, nullptr);
                        uint64_t stub_addr = bridge->get_address(sname.c_str());
                        if (stub_addr != 0) {
                            *ptr = stub_addr;
                        }
                        unresolved++;
                        std::cerr << "[Resolve/ARM64] AUTO-STUB: " << name << " -> 0x" 
                                  << std::hex << stub_addr << std::dec << std::endl;
                    }
                }
                sym_count++;
            }
        }
    };

    process_rela(mod->reladyn, mod->num_reladyn, ".rela.dyn");
    process_rela(mod->relaplt, mod->num_relaplt, ".rela.plt");
    
    std::cout << "[Resolve/ARM64] Total: " << sym_count << " external symbols, " 
              << resolved << " resolved, " << unresolved << " unresolved" << std::endl;

    return 0;
}

int ElfLoaderArm64::load(so_module_arm64* mod, const std::string& filename, uint64_t load_addr) {
    std::ifstream fs(filename, std::ios::binary | std::ios::ate);
    if (!fs.is_open()) return -1;
    
    size_t filesize = fs.tellg();
    fs.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(filesize);
    fs.read((char*)buffer.data(), filesize);
    
    // Validate ELF magic
    if (std::memcmp(buffer.data(), ELFMAG, SELFMAG) != 0) return -2;
    
    // Validate it's a 64-bit ELF
    if (buffer[4] != ELFCLASS64) {
        std::cerr << "[ElfLoader/ARM64] Not a 64-bit ELF (class=" << (int)buffer[4] << ")" << std::endl;
        return -2;
    }
    
    // Validate it's AArch64
    Elf64_Ehdr* raw_ehdr = (Elf64_Ehdr*)buffer.data();
    if (raw_ehdr->e_machine != EM_AARCH64) {
        std::cerr << "[ElfLoader/ARM64] Not AArch64 (machine=" << raw_ehdr->e_machine << ")" << std::endl;
        return -2;
    }

    // Copy ELF header
    mod->ehdr = (Elf64_Ehdr*)new uint8_t[sizeof(Elf64_Ehdr)];
    std::memcpy(mod->ehdr, buffer.data(), sizeof(Elf64_Ehdr));
    
    // Copy program headers
    mod->phdr = (Elf64_Phdr*)new uint8_t[mod->ehdr->e_phnum * sizeof(Elf64_Phdr)];
    std::memcpy(mod->phdr, buffer.data() + mod->ehdr->e_phoff, mod->ehdr->e_phnum * sizeof(Elf64_Phdr));
    
    // Copy section headers
    mod->shdr = (Elf64_Shdr*)new uint8_t[mod->ehdr->e_shnum * sizeof(Elf64_Shdr)];
    std::memcpy(mod->shdr, buffer.data() + mod->ehdr->e_shoff, mod->ehdr->e_shnum * sizeof(Elf64_Shdr));

    // Section header string table
    char* shstr = (char*)(buffer.data() + mod->shdr[mod->ehdr->e_shstrndx].sh_offset);

    mod->base_addr = load_addr;
    uint64_t max_addr = load_addr;

    // Load PT_LOAD segments into guest memory
    for (int i = 0; i < mod->ehdr->e_phnum; i++) {
        if (mod->phdr[i].p_type == PT_LOAD) {
            uint64_t vaddr = load_addr + mod->phdr[i].p_vaddr;
            uint64_t memsz = mod->phdr[i].p_memsz;
            uint64_t filesz = mod->phdr[i].p_filesz;
            uint64_t offset = mod->phdr[i].p_offset;

            if (vaddr + memsz > max_addr) {
                max_addr = vaddr + memsz;
            }

            if (vaddr + memsz > guest_limit) {
                std::cerr << "[ElfLoader/ARM64] Segment out of guest memory bounds!" << std::endl;
                return -3;
            }

            std::memset(guest_base + vaddr, 0, memsz);
            std::memcpy(guest_base + vaddr, buffer.data() + offset, filesz);

            if ((mod->phdr[i].p_flags & PF_X)) {
                mod->text_base = vaddr;
                mod->text_size = memsz;
            } else {
                mod->data_bases.push_back(vaddr);
                mod->data_sizes.push_back(memsz);
            }
            
            std::cout << "[ElfLoader/ARM64] Loaded segment at 0x" << std::hex << vaddr
                      << " (" << std::dec << filesz << "/" << memsz << " bytes)"
                      << ((mod->phdr[i].p_flags & PF_X) ? " [TEXT]" : " [DATA]") << std::endl;
        }
    }
    mod->mem_size = max_addr - load_addr;

    // Parse sections — note: .rela.dyn/.rela.plt instead of .rel.dyn/.rel.plt
    for (int i = 0; i < mod->ehdr->e_shnum; i++) {
        std::string name = shstr + mod->shdr[i].sh_name;
        uint64_t addr = load_addr + mod->shdr[i].sh_addr;
        uint64_t size = mod->shdr[i].sh_size;

        if (name == ".dynamic") {
            mod->dynamic = (Elf64_Dyn*)(guest_base + addr);
        } else if (name == ".dynstr") {
            mod->dynstr = (char*)(guest_base + addr);
        } else if (name == ".dynsym") {
            mod->dynsym = (Elf64_Sym*)(guest_base + addr);
            mod->num_dynsym = size / sizeof(Elf64_Sym);
        } else if (name == ".rela.dyn") {
            // ARM64 uses RELA (with addend), not REL!
            mod->reladyn = (Elf64_Rela*)(guest_base + addr);
            mod->num_reladyn = size / sizeof(Elf64_Rela);
        } else if (name == ".rela.plt") {
            mod->relaplt = (Elf64_Rela*)(guest_base + addr);
            mod->num_relaplt = size / sizeof(Elf64_Rela);
        } else if (name == ".hash" || name == ".gnu.hash") {
            mod->hash = (uint32_t*)(guest_base + addr);
        } else if (name == ".init_array") {
            mod->init_array_vaddr = addr;
            mod->init_array_size = size;
            // Note: .init_array entries are 8-byte pointers on ARM64 (vs 4-byte on ARM32)
        }
    }
    
    std::cout << "[ElfLoader/ARM64] Loaded " << filename 
              << " at 0x" << std::hex << load_addr << std::dec
              << " (" << mod->num_dynsym << " symbols, "
              << mod->num_reladyn << " rela.dyn, "
              << mod->num_relaplt << " rela.plt)" << std::endl;

    return 0;
}

int ElfLoaderArm64::relocate(so_module_arm64* mod) {
    auto process_rela = [&](Elf64_Rela* rela, int count, const char* rel_name) {
        for (int i = 0; i < count; i++) {
            Elf64_Rela* r = &rela[i];
            uint64_t type = ELF64_R_TYPE(r->r_info);
            uint64_t sym_idx = ELF64_R_SYM(r->r_info);
            Elf64_Sym* sym = &mod->dynsym[sym_idx];
            // 64-bit pointer writes
            uint64_t* ptr = (uint64_t*)(guest_base + mod->base_addr + r->r_offset);
            
            switch (type) {
                case R_AARCH64_ABS64:
                    if (sym->st_shndx != SHN_UNDEF) {
                        *ptr += mod->base_addr + sym->st_value + r->r_addend;
                    }
                    break;
                case R_AARCH64_RELATIVE:
                    // RELATIVE uses the addend field (not the in-place value like ARM32)
                    *ptr = mod->base_addr + r->r_addend;
                    break;
                case R_AARCH64_GLOB_DAT:
                case R_AARCH64_JUMP_SLOT:
                    if (sym->st_shndx != SHN_UNDEF) {
                        *ptr = mod->base_addr + sym->st_value + r->r_addend;
                    }
                    break;
                default:
                    // Log unknown relocation types for debugging
                    std::cerr << "[Reloc/ARM64] Unknown relocation type " << type 
                              << " at offset 0x" << std::hex << r->r_offset << std::dec << std::endl;
                    break;
            }
        }
    };

    process_rela(mod->reladyn, mod->num_reladyn, ".rela.dyn");
    process_rela(mod->relaplt, mod->num_relaplt, ".rela.plt");

    return 0;
}

int ElfLoaderArm64::resolve(so_module_arm64* mod, const std::vector<so_default_dynlib_arm64>& imports) {
    auto process_rela = [&](Elf64_Rela* rela, int count) {
        for (int i = 0; i < count; i++) {
            Elf64_Rela* r = &rela[i];
            uint64_t sym_idx = ELF64_R_SYM(r->r_info);
            Elf64_Sym* sym = &mod->dynsym[sym_idx];
            uint64_t* ptr = (uint64_t*)(guest_base + mod->base_addr + r->r_offset);

            if (sym->st_shndx == SHN_UNDEF) {
                if (sym->st_name == 0) continue;
                const char* name = mod->dynstr + sym->st_name;
                if (name[0] == '\0') continue;
                
                bool found = false;
                for (const auto& imp : imports) {
                    if (imp.symbol == name) {
                        *ptr = imp.func;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cerr << "[Resolve/ARM64] Unresolved symbol: " << name << std::endl;
                }
            }
        }
    };

    process_rela(mod->reladyn, mod->num_reladyn);
    process_rela(mod->relaplt, mod->num_relaplt);

    return 0;
}

uint64_t ElfLoaderArm64::get_symbol_vaddr(so_module_arm64* mod, const std::string& name) {
    for (int i = 0; i < mod->num_dynsym; i++) {
        if (mod->dynsym[i].st_name == 0) continue;
        const char* sym_name = mod->dynstr + mod->dynsym[i].st_name;
        if (name == sym_name) {
            return mod->base_addr + mod->dynsym[i].st_value;
        }
    }
    return 0;
}
