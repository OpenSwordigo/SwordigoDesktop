#include "elf_loader.h"
#include "../jni/jni_bridge.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

ElfLoader::ElfLoader(uint8_t* guest_mem_base, uint32_t guest_mem_size) 
    : guest_base(guest_mem_base), guest_limit(guest_mem_size) {}

int ElfLoader::resolve_all_to_bridge(so_module* mod, JniBridge* bridge, uint32_t globals_base) {
    int sym_count = 0;
    uint32_t next_global = globals_base;

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
                
                // Strip @version if present
                std::string sname(name);
                size_t at_pos = sname.find('@');
                if (at_pos != std::string::npos) {
                    sname = sname.substr(0, at_pos);
                }
                
                // Identify if it's a data symbol or a function symbol
                bool is_data = (sname == "__stack_chk_guard" || sname == "_ctype_" || sname == "__sF");
                
                if (is_data && globals_base != 0) {
                    *ptr = next_global;
                    next_global += 8; // Advance global pointer
                    // std::cout << "[Resolve] Global " << name << " -> 0x" << std::hex << *ptr << std::dec << std::endl;
                } else {
                    uint32_t bridge_addr = bridge->get_address(sname.c_str());
                    if (bridge_addr != 0) {
                        *ptr = bridge_addr;
                        if (sname == "sinf" || sname == "cosf" || sname == "atof" || sname == "gzread") {
                            std::cout << "[Resolve] " << name << " (stripped: " << sname << ") -> 0x" << std::hex << bridge_addr << std::dec << std::endl;
                        }
                    } else {
                        std::cerr << "[Resolve] WARNING: No bridge for " << name << std::endl;
                    }
                }
                sym_count++;
            }
        }
    };

    process_rel(mod->reldyn, mod->num_reldyn);
    process_rel(mod->relplt, mod->num_relplt);
    
    std::cout << "[Resolve] Total external symbols resolved: " << sym_count << std::endl;

    return 0;
}

int ElfLoader::load(so_module* mod, const std::string& filename, uint32_t load_addr) {
    std::ifstream fs(filename, std::ios::binary | std::ios::ate);
    if (!fs.is_open()) return -1;
    
    size_t size = fs.tellg();
    fs.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(size);
    fs.read((char*)buffer.data(), size);
    
    if (std::memcmp(buffer.data(), ELFMAG, SELFMAG) != 0) return -2;

    mod->ehdr = (Elf32_Ehdr*)new uint8_t[sizeof(Elf32_Ehdr)];
    std::memcpy(mod->ehdr, buffer.data(), sizeof(Elf32_Ehdr));
    
    mod->phdr = (Elf32_Phdr*)new uint8_t[mod->ehdr->e_phnum * sizeof(Elf32_Phdr)];
    std::memcpy(mod->phdr, buffer.data() + mod->ehdr->e_phoff, mod->ehdr->e_phnum * sizeof(Elf32_Phdr));
    
    mod->shdr = (Elf32_Shdr*)new uint8_t[mod->ehdr->e_shnum * sizeof(Elf32_Shdr)];
    std::memcpy(mod->shdr, buffer.data() + mod->ehdr->e_shoff, mod->ehdr->e_shnum * sizeof(Elf32_Shdr));

    char* shstr = (char*)(buffer.data() + mod->shdr[mod->ehdr->e_shstrndx].sh_offset);

    mod->base_addr = load_addr;
    uint32_t max_addr = load_addr;

    for (int i = 0; i < mod->ehdr->e_phnum; i++) {
        if (mod->phdr[i].p_type == PT_LOAD) {
            uint32_t vaddr = load_addr + mod->phdr[i].p_vaddr;
            uint32_t memsz = mod->phdr[i].p_memsz;
            uint32_t filesz = mod->phdr[i].p_filesz;
            uint32_t offset = mod->phdr[i].p_offset;

            if (vaddr + memsz > max_addr) {
                max_addr = vaddr + memsz;
            }

            if (vaddr + memsz > guest_limit) {
                std::cerr << "Segment out of guest memory bounds!" << std::endl;
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
        }
    }
    mod->mem_size = max_addr - load_addr;

    for (int i = 0; i < mod->ehdr->e_shnum; i++) {
        std::string name = shstr + mod->shdr[i].sh_name;
        uint32_t addr = load_addr + mod->shdr[i].sh_addr;
        uint32_t size = mod->shdr[i].sh_size;

        if (name == ".dynamic") {
            mod->dynamic = (Elf32_Dyn*)(guest_base + addr);
        } else if (name == ".dynstr") {
            mod->dynstr = (char*)(guest_base + addr);
        } else if (name == ".dynsym") {
            mod->dynsym = (Elf32_Sym*)(guest_base + addr);
            mod->num_dynsym = size / sizeof(Elf32_Sym);
        } else if (name == ".rel.dyn") {
            mod->reldyn = (Elf32_Rel*)(guest_base + addr);
            mod->num_reldyn = size / sizeof(Elf32_Rel);
        } else if (name == ".rel.plt") {
            mod->relplt = (Elf32_Rel*)(guest_base + addr);
            mod->num_relplt = size / sizeof(Elf32_Rel);
        } else if (name == ".hash") {
            mod->hash = (uint32_t*)(guest_base + addr);
        } else if (name == ".init_array") {
            mod->init_array_vaddr = addr;
            mod->init_array_size = size;
        }
    }

    return 0;
}

int ElfLoader::relocate(so_module* mod) {
    auto process_rel = [&](Elf32_Rel* rel, int count, const char* rel_name) {
        for (int i = 0; i < count; i++) {
            Elf32_Rel* r = &rel[i];
            uint32_t type = ELF32_R_TYPE(r->r_info);
            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            Elf32_Sym* sym = &mod->dynsym[sym_idx];
            uint32_t* ptr = (uint32_t*)(guest_base + mod->base_addr + r->r_offset);
            
            uint32_t target_addr = mod->base_addr + r->r_offset;
            uint32_t before_value = *ptr;

            switch (type) {
                case R_ARM_ABS32:
                    if (sym->st_shndx != SHN_UNDEF) {
                        *ptr += mod->base_addr + sym->st_value;
                    }
                    break;
                case R_ARM_RELATIVE:
                    *ptr += mod->base_addr;
                    break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT:
                    if (sym->st_shndx != SHN_UNDEF) {
                        *ptr = mod->base_addr + sym->st_value;
                    }
                    break;
            }
            
            uint32_t after_value = *ptr;
            
            // Watchpoint logging
            const char* sym_name = (sym->st_name > 0) ? (mod->dynstr + sym->st_name) : "(null)";
            bool is_suspicious = (after_value == 0x100018c || after_value == 0x46af94 || after_value > 0x20000000);
            
            if (is_suspicious) {
                const char* type_name = "UNKNOWN";
                if (type == R_ARM_RELATIVE) type_name = "R_ARM_RELATIVE";
                else if (type == R_ARM_ABS32) type_name = "R_ARM_ABS32";
                else if (type == R_ARM_GLOB_DAT) type_name = "R_ARM_GLOB_DAT";
                else if (type == R_ARM_JUMP_SLOT) type_name = "R_ARM_JUMP_SLOT";
                
                std::cout << "[Reloc] " << rel_name << " #" << i << std::endl;
                std::cout << "  Type: " << type_name << std::endl;
                std::cout << "  Target Address: 0x" << std::hex << target_addr << std::dec << std::endl;
                std::cout << "  Before: 0x" << std::hex << before_value << std::dec << std::endl;
                std::cout << "  After:  0x" << std::hex << after_value << std::dec << std::endl;
                std::cout << "  Symbol: " << sym_name << std::endl;
                if (is_suspicious) {
                    std::cout << "  ⚠⚠⚠ SUSPICIOUS VALUE - POTENTIAL CORRUPTION SOURCE ⚠⚠⚠" << std::endl;
                }
            }
        }
    };

    process_rel(mod->reldyn, mod->num_reldyn, ".rel.dyn");
    process_rel(mod->relplt, mod->num_relplt, ".rel.plt");

    return 0;
}

int ElfLoader::resolve(so_module* mod, const std::vector<so_default_dynlib>& imports) {
    auto process_rel = [&](Elf32_Rel* rel, int count) {
        for (int i = 0; i < count; i++) {
            Elf32_Rel* r = &rel[i];
            uint32_t type = ELF32_R_TYPE(r->r_info);
            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            Elf32_Sym* sym = &mod->dynsym[sym_idx];
            uint32_t* ptr = (uint32_t*)(guest_base + mod->base_addr + r->r_offset);

            if (sym->st_shndx == SHN_UNDEF) {
                if (sym->st_name == 0) continue; // Skip null symbol
                const char* name = mod->dynstr + sym->st_name;
                if (name[0] == '\0') continue; // Skip empty names
                
                bool found = false;
                for (const auto& imp : imports) {
                    if (imp.symbol == name) {
                        *ptr = imp.func;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cerr << "Unresolved symbol: " << name << std::endl;
                }
            }
        }
    };

    process_rel(mod->reldyn, mod->num_reldyn);
    process_rel(mod->relplt, mod->num_relplt);

    return 0;
}

uint32_t ElfLoader::get_symbol_vaddr(so_module* mod, const std::string& name) {
    for (int i = 0; i < mod->num_dynsym; i++) {
        if (mod->dynsym[i].st_name == 0) continue;
        const char* sym_name = mod->dynstr + mod->dynsym[i].st_name;
        if (name == sym_name) {
            return mod->base_addr + mod->dynsym[i].st_value;
        }
    }
    return 0;
}
