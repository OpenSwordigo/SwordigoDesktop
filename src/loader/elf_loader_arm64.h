#ifndef ELF_LOADER_ARM64_H
#define ELF_LOADER_ARM64_H

#include <stdint.h>
#include <vector>
#include <string>
#include <map>

// 64-bit ELF structures for AArch64
#include "elf_types_arm64.h"

class JniBridge64;

// ARM64 module — mirrors so_module but with 64-bit types
struct so_module_arm64 {
    uintptr_t base_addr;
    size_t mem_size;
    
    uintptr_t text_base;
    size_t text_size;
    // Guest-VA byte range of the .dynstr STRTAB section (never executable).
    // Used by the emulator RenderGuard to reject a stray branch/return into
    // the string table. 0 if not found.
    uintptr_t dynstr_vaddr;
    size_t dynstr_size;
    
    std::vector<uintptr_t> data_bases;
    std::vector<size_t> data_sizes;

    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    Elf64_Shdr *shdr;

    Elf64_Dyn  *dynamic;
    Elf64_Sym  *dynsym;
    Elf64_Rela *reladyn;   // ARM64 uses RELA (with addend), not REL
    Elf64_Rela *relaplt;

    uint32_t *hash;
    char *dynstr;

    int num_dynsym;
    int num_reladyn;
    int num_relaplt;
    
    uint64_t init_array_vaddr = 0;
    uint64_t init_array_size = 0;
    
    std::string soname;
};

struct so_default_dynlib_arm64 {
    std::string symbol;
    uint64_t func; // Bridge trampoline address (64-bit)
};

struct FastCRuntime64 {
    uint64_t memcmp_vaddr = 0;
    uint64_t memcpy_vaddr = 0;
    uint64_t memset_vaddr = 0;
    uint64_t strlen_vaddr = 0;
    uint64_t strcmp_vaddr = 0;
};
extern FastCRuntime64 g_fast_c_runtime_64;

class ElfLoaderArm64 {
public:
    ElfLoaderArm64(uint8_t* guest_mem_base, uint64_t guest_mem_size);
    
    int load(so_module_arm64* mod, const std::string& filename, uint64_t load_addr);
    int relocate(so_module_arm64* mod);
    int resolve(so_module_arm64* mod, const std::vector<so_default_dynlib_arm64>& imports);
    int resolve_all_to_bridge(so_module_arm64* mod, JniBridge64* bridge, uint64_t globals_base = 0);
    
    uint64_t get_symbol_vaddr(so_module_arm64* mod, const std::string& name);

private:
    uint8_t* guest_base;
    uint64_t guest_limit;
    
    uint32_t so_hash(const uint8_t *name);
    int find_symbol_index(so_module_arm64 *mod, const char *symbol);
};

#endif
