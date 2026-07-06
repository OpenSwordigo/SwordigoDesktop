#include "emulator_arm64.h"
#include "platform/binary_selector.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <iomanip>
#include <unicorn/unicorn.h>

// ============================================================================
// ARM64 Emulator — AArch64 mode via Unicorn Engine
//
// Differences from ARM32:
//   - UC_ARCH_ARM64 / UC_MODE_ARM (no Thumb!)
//   - Registers: UC_ARM64_REG_X0..X30, SP, PC, LR
//   - Bridge trampoline: 'ret' = 0xD65F03C0 (4 bytes) vs 'bx lr' = 0x4770
//   - No CPSR Thumb bit manipulation
//   - VFP/NEON enabled by default on AArch64 (no CP15 setup needed)
//   - call() passes up to 8 integer args in X0-X7
// ============================================================================

// Forward declarations from game/camera_override.cpp
extern uint32_t g_cam_ctrl_ptr;
extern void cam_capture_controller(uint32_t this_ptr);

static uint64_t g_instruction_count_64 = 0;

// --- Unicorn hooks ---

static bool hook_mem_unmapped_64(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
    // Auto-map strategy: if the faulting address is in a reasonable range,
    // map a 4KB page and let execution continue. This handles dynamic
    // allocations (malloc/mmap) that land in unmapped regions.
    
    // Align to 4KB page
    uint64_t page_addr = address & ~0xFFFULL;
    uint64_t page_size = 0x1000;
    
    // Safety: only auto-map addresses in reasonable ranges
    // 0x00000000 - 0xEFFFFFFF: guest memory area (heap, stack, BSS)
    // Refuse to map NULL page, bridge region (0xFF000000+), or absurd addresses
    if (page_addr == 0 || page_addr >= 0xEF000000) {
        // For addresses above 32-bit guest space (0xffffffff... range from
        // pointer arithmetic overflow), try to map a zero page so the load
        // reads 0 and execution continues. This handles all bad 64-bit 
        // accesses generically.
        if (address >= 0x100000000ULL) {
            uint64_t fault_page = address & ~0xFFFULL;
            uint32_t low32 = (uint32_t)(address & 0xFFFFFFFFULL);
            uint32_t guest_page = low32 & ~0xFFFU;
            
            // If the lower 32 bits point to valid guest memory, ALIAS the page
            // to the real data instead of mapping zeros. The game's ARM64 pointer
            // arithmetic overflows into 64 bits but the useful address is in the
            // lower 32 bits.
            EmulatorArm64* emu = (EmulatorArm64*)user_data;
            uint8_t* base = emu->get_memory_base();
            
            uc_err map_err;
            if (guest_page < 0xE0000000U && base != nullptr) {
                // Alias: high address → same physical memory as low address
                // CRITICAL: if the aliased page falls in the .text section (code),
                // map it READ-ONLY to prevent accidental code corruption.
                // EXCEPTION: RLSwordigo requires writable text segments for modded game data.
                // The .text segment is at load_addr(0x1000000) to 0x16af698.
                uint32_t prot = UC_PROT_ALL;
                // Check if this is RLSwordigo by looking at the loaded binary info
                extern BinarySelector g_binary_selector;
                const BinaryInfo* binfo = g_binary_selector.get_loaded_info();
                bool is_rlsw = (binfo && binfo->game_type == "RLSwordigo");
                
                if (!is_rlsw && guest_page >= 0x1000000 && guest_page < 0x16b0000) {
                    prot = UC_PROT_READ;  // Code section: read-only alias (vanilla only)
                }
                map_err = uc_mem_map_ptr(uc, fault_page, 0x1000, prot, base + guest_page);
            } else {
                // Guest page is out of range — map zeros as fallback.
                // LIMIT: Too many zero pages = garbage 64-bit pointers → stop.
                static int zero_page_count = 0;
                zero_page_count++;
                if (zero_page_count > 50) {
                    // Too many garbage pages — stop emulation to avoid OOM
                    uint64_t pc;
                    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
                    if (zero_page_count <= 52) {
                        std::cerr << "[ARM64] Zero page limit (50) reached at PC=0x" 
                                  << std::hex << pc << std::dec 
                                  << " — stopping emulation" << std::endl;
                    }
                    uc_emu_stop(uc);
                    return false;
                }
                map_err = uc_mem_map(uc, fault_page, 0x1000, UC_PROT_ALL);
            }
            
            if (map_err == UC_ERR_OK) {
                static int map64_count = 0;
                map64_count++;
                if (map64_count <= 10) {
                    uint64_t pc, lr;
                    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
                    uc_reg_read(uc, UC_ARM64_REG_LR, &lr);
                    if (guest_page < 0xE0000000U) {
                        std::cerr << "[ARM64] ALIAS 0x" << std::hex << fault_page 
                                  << " -> guest 0x" << guest_page
                                  << " (PC=0x" << pc << ")" << std::dec << std::endl;
                    } else {
                        std::cerr << "[ARM64] Mapped zero page at 0x" << std::hex << fault_page 
                                  << " (no valid alias, PC=0x" << pc << ")" << std::dec << std::endl;
                    }
                } else if (map64_count == 11) {
                    std::cerr << "[ARM64] (suppressing further 64-bit map messages)" << std::endl;
                }
                return true; // Retry — load will read 0
            }
            // Map failed — fall through to hard fault
        }
        
        std::cerr << "[FAULT/ARM64] Memory unmapped at 0x" << std::hex << address 
                  << " (size: " << size << ") — NOT recoverable" << std::dec << std::endl;
        uint64_t pc, lr, sp;
        uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
        uc_reg_read(uc, UC_ARM64_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM64_REG_SP, &sp);
        std::cerr << "  PC=0x" << std::hex << pc << " LR=0x" << lr << " SP=0x" << sp << std::dec << std::endl;
        return false; // Stop emulation
    }
    
    // Try to auto-map a larger region (64KB) around the faulting address 
    // to reduce repeated faults from sequential access patterns
    uint64_t region_addr = page_addr & ~0xFFFFULL;  // Align to 64KB
    uint64_t region_size = 0x10000;                   // 64KB
    
    // Clamp to stay below 0xEF000000
    if (region_addr + region_size > 0xEF000000) {
        region_size = 0xEF000000 - region_addr;
    }
    
    uc_err err = uc_mem_map(uc, region_addr, region_size, UC_PROT_ALL);
    if (err == UC_ERR_OK) {
        // Zero-fill the mapped region (clean memory)
        std::vector<uint8_t> zeros(region_size, 0);
        uc_mem_write(uc, region_addr, zeros.data(), region_size);
        
        static int auto_map_count = 0;
        auto_map_count++;
        if (auto_map_count <= 20) {
            std::cout << "[ARM64] Auto-mapped 64KB at 0x" << std::hex << region_addr 
                      << " (fault at 0x" << address << ", type=" << type << ")"
                      << std::dec << std::endl;
        } else if (auto_map_count == 21) {
            std::cout << "[ARM64] (suppressing further auto-map messages)" << std::endl;
        }
        return true; // Continue emulation
    }
    
    // If 64KB failed (already partially mapped?), try just the 4KB page
    err = uc_mem_map(uc, page_addr, page_size, UC_PROT_ALL);
    if (err == UC_ERR_OK) {
        std::vector<uint8_t> zeros(page_size, 0);
        uc_mem_write(uc, page_addr, zeros.data(), page_size);
        return true; // Continue emulation
    }
    
    // Both failed — hard fault
    std::cerr << "[FAULT/ARM64] Memory unmapped at 0x" << std::hex << address 
              << " — auto-map FAILED: " << uc_strerror(err) << std::dec << std::endl;
    uint64_t pc, lr, sp;
    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM64_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM64_REG_SP, &sp);
    std::cerr << "  PC=0x" << std::hex << pc << " LR=0x" << lr << " SP=0x" << sp << std::dec << std::endl;
    return false;
}

static void hook_bridge_64(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    EmulatorArm64* emu = (EmulatorArm64*)user_data;
    emu->handle_bridge_call(address);
}

// TODO: Camera hook address for ARM64 binary — needs to be found via symbol
// static void hook_camera_ctor_64(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) { ... }

// --- Constructor ---

EmulatorArm64::EmulatorArm64(uint8_t* guest_mem, uint64_t mem_size)
    : memory(guest_mem), size(mem_size), uc(nullptr) {
    
    // Open Unicorn in AArch64 mode (no Thumb, no mode switching!)
    uc_err err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, (uc_engine**)&uc);
    if (err) {
        std::cerr << "[ARM64] Failed on uc_open(): " << uc_strerror(err) << std::endl;
        return;
    }

    // Map guest memory (same flat mapping as ARM32)
    err = uc_mem_map_ptr((uc_engine*)uc, 0, size, UC_PROT_ALL, memory);
    if (err) {
        std::cerr << "[ARM64] Failed on uc_mem_map_ptr(): " << uc_strerror(err) << std::endl;
        return;
    }
    
    // Map bridge trampoline region (same range as ARM32: 0xFF000000)
    err = uc_mem_map((uc_engine*)uc, 0xFF000000, 0x1000000, UC_PROT_ALL);
    if (err) {
        std::cerr << "[ARM64] Failed to map bridge region: " << uc_strerror(err) << std::endl;
    }

    // Fill bridge region with 'ret' instructions (ARM64: 0xD65F03C0)
    // Unlike ARM32's 2-byte 'bx lr', ARM64 'ret' is 4 bytes, aligned to 4
    uint32_t ret_insn = 0xD65F03C0;
    for (uint64_t addr = 0xFF000000; addr < 0xFF100000; addr += 4) {
        uc_mem_write((uc_engine*)uc, addr, &ret_insn, 4);
    }

    // No VFP/CP15 setup needed — AArch64 has NEON/FP enabled by default!

    // Set stack pointer (same strategy as ARM32)
    uint64_t stack_base = (uint64_t)(size - 0x1000);
    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_SP, &stack_base);

    // Map magic LR page (return detection)
    err = uc_mem_map((uc_engine*)uc, 0xE0000000, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
    if (err) {
        std::cerr << "[ARM64] Failed to map magic LR page: " << uc_strerror(err) << std::endl;
    }
    // Write a 'ret' at magic LR so Unicorn stops cleanly
    uc_mem_write((uc_engine*)uc, 0xE0000000, &ret_insn, 4);

    // Hook 1: Bridge calls (0xFF000000 range)
    uc_hook bridge_hook;
    uc_hook_add((uc_engine*)uc, &bridge_hook, UC_HOOK_CODE, (void*)hook_bridge_64, this,
                0xFF000000, 0xFF100000);

    // Hook 2: Unmapped memory
    uc_hook mem_hook;
    uc_hook_add((uc_engine*)uc, &mem_hook, UC_HOOK_MEM_UNMAPPED, (void*)hook_mem_unmapped_64, this, 1, 0);
    
    // TODO: Hook for camera controller capture — address needs to come from ARM64 symbol table
    // (ARM32 uses hardcoded 0x002e35c4 — this WILL be different in ARM64 binary)

    std::cout << "[ARM64] Emulator initialized, stack at 0x" << std::hex << stack_base << std::dec << std::endl;
}

EmulatorArm64::~EmulatorArm64() {
    if (uc) uc_close((uc_engine*)uc);
}

void EmulatorArm64::protect_memory(uint64_t addr, uint64_t size, uint32_t perms) {
    uc_err err = uc_mem_protect((uc_engine*)uc, addr, size, perms);
    if (err) {
        std::cerr << "[ARM64] uc_mem_protect(0x" << std::hex << addr 
                  << ", 0x" << size << ", " << std::dec << perms 
                  << ") failed: " << uc_strerror(err) << std::endl;
    } else {
        std::cout << "[ARM64] Protected 0x" << std::hex << addr 
                  << "-0x" << (addr + size) << " perms=" << std::dec << perms << std::endl;
    }
}

// --- Register access ---

void EmulatorArm64::set_pc(uint64_t pc) {
    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &pc);
}

uint64_t EmulatorArm64::get_pc() {
    uint64_t val;
    uc_reg_read((uc_engine*)uc, UC_ARM64_REG_PC, &val);
    return val;
}

uint64_t EmulatorArm64::get_lr() {
    uint64_t val;
    uc_reg_read((uc_engine*)uc, UC_ARM64_REG_LR, &val);
    return val;
}

void EmulatorArm64::set_reg(int reg, uint64_t value) {
    // X0-X28 map directly, X29=FP, X30=LR
    int uc_reg;
    if (reg <= 28)      uc_reg = UC_ARM64_REG_X0 + reg;
    else if (reg == 29) uc_reg = UC_ARM64_REG_FP;
    else if (reg == 30) uc_reg = UC_ARM64_REG_LR;
    else                uc_reg = UC_ARM64_REG_SP;
    uc_reg_write((uc_engine*)uc, uc_reg, &value);
}

uint64_t EmulatorArm64::get_reg(int reg) {
    uint64_t val;
    int uc_reg;
    if (reg <= 28)      uc_reg = UC_ARM64_REG_X0 + reg;
    else if (reg == 29) uc_reg = UC_ARM64_REG_FP;
    else if (reg == 30) uc_reg = UC_ARM64_REG_LR;
    else                uc_reg = UC_ARM64_REG_SP;
    uc_reg_read((uc_engine*)uc, uc_reg, &val);
    return val;
}

void EmulatorArm64::set_dreg(int reg, double value) {
    // D0-D31 for double-precision floats
    int uc_reg = UC_ARM64_REG_D0 + reg;
    uc_reg_write((uc_engine*)uc, uc_reg, &value);
}

double EmulatorArm64::get_dreg(int reg) {
    double val;
    int uc_reg = UC_ARM64_REG_D0 + reg;
    uc_reg_read((uc_engine*)uc, uc_reg, &val);
    return val;
}

void EmulatorArm64::set_sreg(int reg, float value) {
    // S0-S31 for single-precision floats
    int uc_reg = UC_ARM64_REG_S0 + reg;
    uc_reg_write((uc_engine*)uc, uc_reg, &value);
}

float EmulatorArm64::get_sreg(int reg) {
    float val;
    int uc_reg = UC_ARM64_REG_S0 + reg;
    uc_reg_read((uc_engine*)uc, uc_reg, &val);
    return val;
}

uint64_t EmulatorArm64::get_bridge_base() { return 0xFF000000; }

// --- Execution ---

void EmulatorArm64::run(uint64_t start_pc) {
    uint64_t magic_lr = 0xE0000000;
    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_LR, &magic_lr);
    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &start_pc);
    
    // Debug: show what code is at the start address
    static int debug_calls = 0;
    if (debug_calls < 20) {
        uint32_t code[4] = {0};
        uc_mem_read((uc_engine*)uc, start_pc, code, 16);
        std::cout << "[ARM64] run() PC=0x" << std::hex << start_pc 
                  << " code: " << code[0] << " " << code[1] << " " << code[2] << " " << code[3]
                  << std::dec << std::endl;
        debug_calls++;
    }
    
    // Measure execution time
    auto t0 = std::chrono::steady_clock::now();
    
    // Save SP so we can restore it on forced return (prevents stack leaks)
    uint64_t entry_sp;
    uc_reg_read((uc_engine*)uc, UC_ARM64_REG_SP, &entry_sp);
    
    // Adaptive instruction budget: run 1B at a time, retry up to 10 times.
    // Scene loading (entering Wastelands) genuinely needs billions of instructions.
    // Each retry checks if the function returned; if not, continues execution.
    static const uint64_t CHUNK = 1000000000ULL;  // 1B per chunk
    static const int MAX_CHUNKS = 30;             // 30B total max (scene loading needs ~10B)
    uc_err err = UC_ERR_OK;
    uint64_t curr_pc = start_pc;
    int chunk = 0;
    int same_pc_count = 0;     // Count consecutive chunks at the SAME PC
    uint64_t last_chunk_pc = 0; // Track PC from previous chunk
    for (chunk = 0; chunk < MAX_CHUNKS; chunk++) {
        err = uc_emu_start((uc_engine*)uc, 
                           chunk == 0 ? start_pc : curr_pc, 
                           magic_lr, 0, CHUNK);
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_PC, &curr_pc);
        if (curr_pc == magic_lr) break;  // Function completed!
        if (err && err != UC_ERR_OK) break;  // Real error
        
        // Detect TRUE infinite loop: same PC for 3 consecutive chunks.
        // Entity processing changes PCs between chunks (making progress).
        // Only a true spin loop stays at the exact same address.
        if (curr_pc == last_chunk_pc) {
            same_pc_count++;
            if (same_pc_count >= 3) {
                std::cerr << "[ARM64] TRUE spin loop: PC=0x" << std::hex << curr_pc 
                          << std::dec << " unchanged for " << same_pc_count 
                          << " chunks — force-returning" << std::endl;
                uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &magic_lr);
                uc_reg_write((uc_engine*)uc, UC_ARM64_REG_SP, &entry_sp);
                return;
            }
        } else {
            same_pc_count = 0;
        }
        last_chunk_pc = curr_pc;
        
        if (chunk > 0) {
            // Only dump full trace on chunk 2 (first time we know it's heavy)
            if (chunk == 1) {
                uint64_t lr, fp, sp;
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_LR, &lr);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X29, &fp);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_SP, &sp);
                
                std::cerr << "[ARM64] ⚠ Heavy function 0x" << std::hex << start_pc 
                          << " — chunk " << std::dec << (chunk + 1) << "/" << MAX_CHUNKS << std::endl;
                std::cerr << "  PC=0x" << std::hex << curr_pc 
                          << "  LR=0x" << lr << "  SP=0x" << sp << std::endl;
                
                // Walk frame pointer chain to dump call stack
                std::cerr << "  --- Guest Call Stack ---" << std::endl;
                uint64_t walk_fp = fp;
                for (int frame = 0; frame < 12 && walk_fp > 0x1000 && walk_fp < 0xE0000000ULL; frame++) {
                    uint64_t saved_fp = 0, saved_lr = 0;
                    uc_mem_read((uc_engine*)uc, walk_fp, &saved_fp, 8);
                    uc_mem_read((uc_engine*)uc, walk_fp + 8, &saved_lr, 8);
                    std::cerr << "  [" << frame << "] LR=0x" << std::hex << saved_lr 
                              << "  FP=0x" << saved_fp << std::dec << std::endl;
                    if (saved_fp <= walk_fp || saved_fp == 0) break;  // stack grows down
                    walk_fp = saved_fp;
                }
                std::cerr << "  ------------------------" << std::endl;
            } else {
                std::cerr << "[ARM64] Heavy function 0x" << std::hex << start_pc 
                          << " — chunk " << std::dec << (chunk + 1) << "/" << MAX_CHUNKS 
                          << " (PC=0x" << std::hex << curr_pc << ")" << std::dec << std::endl;
            }
        }
    }
    
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    
    if (err) {
        if (curr_pc != magic_lr) {
            /* Check if this is our intentional BRK from sre_cxa_throw (inside libsre.so).
             * libsre.so is loaded at 0x2000000, typically < 64KB in size.
             * If so, walk the frame chain to skip past the throwing function
             * and RESUME emulation instead of aborting the entire call. */
            if (curr_pc >= 0x2000000 && curr_pc < 0x2010000) {
                /* Read frame pointer to walk the call stack */
                uint64_t fp;
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X29, &fp);
                
                /* Walk up frames to get past the exception machinery AND Lua internals.
                 * We want to return to ENGINE code, not to mid-Lua state.
                 * 
                 * Code ranges (guest addresses):
                 *   0x1000000-0x14E0000: Engine code (safe to return to)
                 *   0x14E0000-0x1580000: Lua code (SKIP — state is corrupted)
                 *   0x1580000-0x1700000: C++ runtime/exception code (SKIP)
                 *   0x2000000-0x2010000: libsre.so (SKIP)
                 */
                uint64_t target_lr = 0;
                uint64_t target_fp = 0;
                uint64_t target_sp = 0;
                bool found = false;
                
                for (int i = 0; i < 10 && fp >= 0xD0000000 && fp < 0xE0000000; i++) {
                    uint64_t next_fp = 0, next_lr = 0;
                    uc_mem_read((uc_engine*)uc, fp, &next_fp, 8);
                    uc_mem_read((uc_engine*)uc, fp + 8, &next_lr, 8);
                    
                    /* Want: engine code (0x1000000-0x14E0000) — 
                     * NOT Lua, NOT C++ runtime, NOT libsre */
                    if (i >= 2 && next_lr >= 0x1000000 && next_lr < 0x14E0000) {
                        target_lr = next_lr;
                        target_fp = next_fp;
                        target_sp = fp + 16;
                        found = true;
                        break;
                    }
                    fp = next_fp;
                }
                
                if (found) {
                    static int recovery_count = 0;
                    recovery_count++;
                    if (recovery_count <= 3) {
                        std::cerr << "[SRE-Recovery] C++ exception #" << recovery_count
                                  << " — skipping to engine code 0x" 
                                  << std::hex << target_lr << std::dec << std::endl;
                    } else if (recovery_count == 4) {
                        std::cerr << "[SRE-Recovery] Suppressing further recovery logs" << std::endl;
                    }
                    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &target_lr);
                    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_X29, &target_fp);
                    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_SP, &target_sp);
                    uint64_t zero = 0;
                    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_X0, &zero);
                    
                    /* Resume emulation from the recovered address */
                    err = uc_emu_start((uc_engine*)uc, target_lr, magic_lr, 0, CHUNK);
                    uc_reg_read((uc_engine*)uc, UC_ARM64_REG_PC, &curr_pc);
                    
                    if (!err || curr_pc == magic_lr) {
                        goto post_call;
                    }
                    /* Recovery failed — fall through to normal error handling */
                }
            }
            
            std::cerr << "[ARM64] !! Emulation error: " << uc_strerror(err) << std::endl;
            std::cerr << "  Stuck at PC: 0x" << std::hex << curr_pc << std::dec << std::endl;
            std::cerr << "  (started at 0x" << std::hex << start_pc << std::dec 
                      << ", took " << ms << "ms)" << std::endl;
            // Dump registers to help trace the caller
            {
                uint64_t lr, sp, fp, x0, x1, x2, x3;
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_LR, &lr);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_SP, &sp);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X29, &fp);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X0, &x0);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X1, &x1);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X2, &x2);
                uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X3, &x3);
                std::cerr << "  LR=0x" << std::hex << lr
                          << " SP=0x" << sp << " FP=0x" << fp << std::dec << std::endl;
                std::cerr << "  X0=0x" << std::hex << x0 << " X1=0x" << x1
                          << " X2=0x" << x2 << " X3=0x" << x3 << std::dec << std::endl;
            }
            print_trace();
            uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &magic_lr);
            uc_reg_write((uc_engine*)uc, UC_ARM64_REG_SP, &entry_sp);
        }
    } else if (curr_pc != magic_lr) {
        // UC_ERR_OK but didn't reach return address — stuck in spin loop!
        static int limit_hits = 0;
        limit_hits++;
        uint64_t lr, sp, x0, x1, x2, x3;
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_LR, &lr);
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_SP, &sp);
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X0, &x0);
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X1, &x1);
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X2, &x2);
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_X3, &x3);
        
        std::cerr << "======= SPIN LOOP DETECTED =======" << std::endl;
        std::cerr << "[ARM64] Function 0x" << std::hex << start_pc 
                  << " stuck at PC=0x" << curr_pc << std::dec
                  << " after " << (int)ms << "ms (1B instructions)" << std::endl;
        std::cerr << "  LR=0x" << std::hex << lr << " SP=0x" << sp << std::dec << std::endl;
        std::cerr << "  X0=0x" << std::hex << x0 << " X1=0x" << x1 
                  << " X2=0x" << x2 << " X3=0x" << x3 << std::dec << std::endl;
        
        // Dump 24 instructions covering the full loop (branch target to stuck PC + beyond)
        uint32_t code[24] = {0};
        uint64_t dump_start = (curr_pc >= 64) ? curr_pc - 64 : 0;
        uc_mem_read((uc_engine*)uc, dump_start, code, 96);
        std::cerr << "  Full loop code:" << std::endl;
        for (int i = 0; i < 24; i++) {
            std::cerr << "    0x" << std::hex << (dump_start + i*4) 
                      << ": " << std::setw(8) << std::setfill('0') << code[i]
                      << (((dump_start + i*4) == curr_pc) ? " <-- STUCK" : "")
                      << std::dec << std::endl;
        }
        
        // Read memory at X0 to see the data structure
        if (x0 > 0 && x0 < 0xE0000000) {
            uint8_t data[32] = {0};
            uc_mem_read((uc_engine*)uc, x0, data, 32);
            std::cerr << "  Memory at X0 (0x" << std::hex << x0 << "):" << std::dec;
            for (int i = 0; i < 32; i++) {
                if (i % 8 == 0) std::cerr << "\n    ";
                std::cerr << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
            }
            std::cerr << std::dec << std::endl;
        }
        std::cerr << "==================================" << std::endl;
        
        // SMART FIX: Detect the specific linked-list loop with size=0.
        // Read [X0+0] (tag) and [X0+4] (size). If size==0 and tag!=0,
        // write tag=0 to force the CBZ exit, then re-run from the loop top.
        if (x0 > 0 && x0 < 0xE0000000) {
            uint32_t tag = 0, size = 0;
            uc_mem_read((uc_engine*)uc, x0, &tag, 4);
            uc_mem_read((uc_engine*)uc, x0 + 4, &size, 4);
            if (tag != 0 && size == 0) {
                // Patch: write tag=0 as end sentinel
                uint32_t zero = 0;
                uc_mem_write((uc_engine*)uc, x0, &zero, 4);
                std::cerr << "[FIX] Patched tag=0 at X0=0x" << std::hex << x0 
                          << " (was tag=0x" << tag << " size=0) — re-running loop" 
                          << std::dec << std::endl;
                
                // Find the loop top — scan backward from stuck PC for the 
                // CBZ instruction (0x340001e2 pattern: CBZ W2, ...)
                // The loop top is where LDR W2,[X0] is, one instruction before CBZ
                uint32_t nearby[24] = {0};
                uint64_t scan_start = (curr_pc >= 64) ? curr_pc - 64 : 0;
                uc_mem_read((uc_engine*)uc, scan_start, nearby, 96);
                uint64_t loop_top = curr_pc; // fallback
                for (int i = 0; i < 24; i++) {
                    // Look for "LDR W2, [X0]" = 0xb9400002
                    if (nearby[i] == 0xb9400002) {
                        loop_top = scan_start + i * 4;
                        break;
                    }
                }
                // Set PC to loop top so the patched tag=0 triggers CBZ exit
                uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &loop_top);
                std::cerr << "[FIX] Restarting from loop top 0x" << std::hex << loop_top 
                          << std::dec << std::endl;
                // Re-run with a small instruction budget to let the exit path execute
                uc_emu_start((uc_engine*)uc, loop_top, magic_lr, 0, 10000000ULL);
                std::cerr << "[FIX] Function completed after loop fix" << std::endl;
                // Don't set PC to magic_lr — already returned normally
                goto post_call;
            }
        }
        // THREAD FIX: If we're spinning AND there are deferred threads,
        // run them now! The main thread is likely polling a flag that a
        // worker thread would set. Running the workers breaks the deadlock.
        if (has_pending_threads()) {
            std::cerr << "[FIX] Spin loop detected with " << pending_threads.size() 
                      << " pending thread(s) — running them now" << std::endl;
            
            // Save current state
            uint64_t saved_pc = curr_pc;
            
            // Run all pending threads
            run_pending_threads();
            
            // Restore PC to where we were stuck and retry
            uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &saved_pc);
            std::cerr << "[FIX] Threads completed, resuming from PC=0x" 
                      << std::hex << saved_pc << std::dec << std::endl;
            
            // Re-run with fresh instruction budget
            uc_emu_start((uc_engine*)uc, saved_pc, magic_lr, 0, 500000000ULL);
            
            // Check if it completed
            uint64_t new_pc;
            uc_reg_read((uc_engine*)uc, UC_ARM64_REG_PC, &new_pc);
            if (new_pc == magic_lr) {
                std::cerr << "[FIX] Function completed after thread drain!" << std::endl;
                goto post_call;
            }
            std::cerr << "[FIX] Function still stuck at 0x" << std::hex << new_pc 
                      << std::dec << " after thread drain" << std::endl;
        }
        
        // Fallback: abort function — restore SP to prevent stack leak
        uc_reg_write((uc_engine*)uc, UC_ARM64_REG_PC, &magic_lr);
        uc_reg_write((uc_engine*)uc, UC_ARM64_REG_SP, &entry_sp);
    }
    
    // Log slow calls (>50ms)
    if (ms > 50.0) {
        static int slow_count = 0;
        slow_count++;
        if (slow_count <= 30) {
            std::cerr << "[PERF/ARM64] SLOW call at 0x" << std::hex << start_pc << std::dec 
                      << " took " << (int)ms << "ms" << std::endl;
        } else if (slow_count == 31) {
            std::cerr << "[PERF/ARM64] (suppressing further slow call warnings)" << std::endl;
        }
    }
    
post_call:
    if (!quiet_mode) {
        // Read actual PC after execution to see where it stopped
        uint64_t final_pc;
        uc_reg_read((uc_engine*)uc, UC_ARM64_REG_PC, &final_pc);
        std::cout << "[ARM64] Function stopped at PC=0x" << std::hex << final_pc << std::dec << std::endl;
    }
}

uint64_t EmulatorArm64::call(uint64_t addr, const std::vector<uint64_t>& args) {
    if (!quiet_mode) {
        std::cout << "[ARM64] Calling guest function at 0x" << std::hex << addr << std::dec << std::endl;
    }
    
    // ARM64 AAPCS: first 8 integer args go in X0-X7
    // (ARM32 only had R0-R3 = 4 registers, rest on stack)
    for (size_t i = 0; i < args.size() && i < 8; ++i) {
        set_reg(i, args[i]);
    }
    
    // TODO: if args.size() > 8, push remaining to stack (rare for JNI calls)

    run(addr);
    uint64_t result = get_reg(0); // Return in X0
    if (!quiet_mode) {
        std::cout << "[ARM64] Function returned with X0=0x" << std::hex << result << std::dec << std::endl;
    }
    return result;
}

// --- Bridge handling ---

void EmulatorArm64::handle_bridge_call(uint64_t address) {
    if (bridge) {
        uint64_t lr = get_lr();
        bridge->call_handler(address, (void*)this);
        
        // If the handler set redirect_pc, use that instead of lr.
        // This allows bridge handlers to chain into guest functions
        // without nested uc_emu_start (which breaks outer emulation).
        if (redirect_pc != 0) {
            // Set LR to the original return address so the redirected
            // function returns to the right place when done
            set_lr(lr);
            set_pc(redirect_pc);
            redirect_pc = 0;
        } else {
            set_pc(lr);
        }
    }
}

void EmulatorArm64::set_lr(uint64_t lr) {
    uc_reg_write((uc_engine*)uc, UC_ARM64_REG_LR, &lr);
}

// --- Debugging ---

void EmulatorArm64::record_pc(uint64_t pc) {
    if (pc >= 0x1000000 && pc < 0x2000000) {
        last_pcs.push_back(pc);
        if (last_pcs.size() > 50) {
            last_pcs.erase(last_pcs.begin());
        }
    }
}

void EmulatorArm64::print_trace() {
    std::cerr << "--- ARM64 PC Trace (Last " << last_pcs.size() << " instructions) ---" << std::endl;
    for (size_t i = 0; i < last_pcs.size(); ++i) {
        std::cerr << "  #" << i << ": 0x" << std::hex << last_pcs[i] << std::dec << std::endl;
    }
    std::cerr << "---------------------------------------------" << std::endl;
}
