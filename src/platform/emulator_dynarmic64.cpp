// SPDX-FileCopyrightText: Copyright 2024 SwordigoDesktop Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// ARM64 Dynarmic JIT Backend — emulator_dynarmic64.cpp
// Improvements inspired by the Yuzu Nintendo Switch emulator's Dynarmic integration.
// Key additions:
//   [YUZU-INSPIRED] WFE/WFI/Yield exception interception (softlock prevention)
//   [YUZU-INSPIRED] InstructionCacheOperationRaised (IC IVAU / IC IALLU support)
//   [YUZU-INSPIRED] MemoryWriteExclusive* CAS callbacks (LDXR/STXR correctness)
//   [LEGACY]        Original spinloop/bridge-spin detection is preserved below.

#include "emulator_dynarmic64.h"
#include "platform/binary_selector.h"
#include "jni/jni_bridge_arm64.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <iomanip>

extern "C" const char* sre_resolve_symbol(uint64_t addr);

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include <atomic>    // for MemoryWriteExclusive CAS operations
#include <cstddef>   // for size_t

struct SavedCpuState {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t pstate;
    Dynarmic::A64::Vector vectors[32];
    
    // Member variables for reentrancy
    bool bridge_halt_requested;
    uint64_t bridge_halt_address;
    bool function_returned;
};

// ============================================================================
// Dynarmic ARM64 Backend — Implementation
//
// Uses Dynarmic's A64::Jit for high-performance JIT compilation.
// Memory is accessed via callbacks (MemoryRead*/MemoryWrite*) that
// directly read/write from the shared guest memory buffer.
//
// Bridge calls are detected by checking if the PC enters the bridge
// region (0xFF000000-0xFF100000) after a Run() completes. The bridge
// region is filled with HLT instructions, causing Dynarmic to halt
// via ExceptionRaised(Breakpoint).
// ============================================================================

// --- Memory Callbacks for Dynarmic ---

class SwordigoMemory : public Dynarmic::A64::UserCallbacks {
public:
    SwordigoMemory(EmulatorDynarmic64* emu, uint8_t* mem, uint64_t size)
        : emu(emu), memory(mem), mem_size(size) {}

    // --- Memory reads ---
    std::uint8_t MemoryRead8(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr < mem_size) return memory[vaddr];
        return 0;
    }

    std::uint16_t MemoryRead16(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr + 1 < mem_size) {
            std::uint16_t val;
            std::memcpy(&val, memory + vaddr, 2);
            return val;
        }
        return 0;
    }

    std::uint32_t MemoryRead32(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr + 3 < mem_size) {
            std::uint32_t val;
            std::memcpy(&val, memory + vaddr, 4);
            return val;
        }
        return 0;
    }

    std::uint64_t MemoryRead64(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr + 7 < mem_size) {
            std::uint64_t val;
            std::memcpy(&val, memory + vaddr, 8);
            return val;
        }
        return 0;
    }

    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr vaddr) override {
        Dynarmic::A64::Vector val = {0, 0};
        if (vaddr + 15 < mem_size) {
            std::memcpy(&val, memory + vaddr, 16);
        }
        return val;
    }

    // --- Memory writes ---
    void MemoryWrite8(Dynarmic::A64::VAddr vaddr, std::uint8_t value) override {
        if (vaddr < mem_size) memory[vaddr] = value;
    }

    void MemoryWrite16(Dynarmic::A64::VAddr vaddr, std::uint16_t value) override {
        if (vaddr + 1 < mem_size) std::memcpy(memory + vaddr, &value, 2);
    }

    void MemoryWrite32(Dynarmic::A64::VAddr vaddr, std::uint32_t value) override {
        if (vaddr + 3 < mem_size) std::memcpy(memory + vaddr, &value, 4);
    }

    void MemoryWrite64(Dynarmic::A64::VAddr vaddr, std::uint64_t value) override {
        if (vaddr + 7 < mem_size) std::memcpy(memory + vaddr, &value, 8);
    }

    void MemoryWrite128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value) override {
        if (vaddr + 15 < mem_size) std::memcpy(memory + vaddr, &value, 16);
    }

    // =========================================================================
    // [YUZU-INSPIRED] MemoryWriteExclusive* — Proper CAS-based exclusive writes
    // =========================================================================
    // Yuzu reference: core/arm/dynarmic/arm_dynarmic_64.cpp, WriteExclusive*
    // These are needed for correct LDXR/STXR (Load/Store Exclusive) emulation.
    // Without these, guest spinlocks and mutexes can corrupt shared state.
    // We implement them via std::atomic CAS on the guest memory buffer.
    // =========================================================================

    bool MemoryWriteExclusive8(Dynarmic::A64::VAddr vaddr, std::uint8_t value, std::uint8_t expected) override {
        if (vaddr >= mem_size) return false;
        auto* ptr = reinterpret_cast<std::atomic<std::uint8_t>*>(memory + vaddr);
        return ptr->compare_exchange_weak(expected, value, std::memory_order_seq_cst);
    }

    bool MemoryWriteExclusive16(Dynarmic::A64::VAddr vaddr, std::uint16_t value, std::uint16_t expected) override {
        if (vaddr + 1 >= mem_size) return false;
        auto* ptr = reinterpret_cast<std::atomic<std::uint16_t>*>(memory + vaddr);
        return ptr->compare_exchange_weak(expected, value, std::memory_order_seq_cst);
    }

    bool MemoryWriteExclusive32(Dynarmic::A64::VAddr vaddr, std::uint32_t value, std::uint32_t expected) override {
        if (vaddr + 3 >= mem_size) return false;
        auto* ptr = reinterpret_cast<std::atomic<std::uint32_t>*>(memory + vaddr);
        return ptr->compare_exchange_weak(expected, value, std::memory_order_seq_cst);
    }

    bool MemoryWriteExclusive64(Dynarmic::A64::VAddr vaddr, std::uint64_t value, std::uint64_t expected) override {
        if (vaddr + 7 >= mem_size) return false;
        auto* ptr = reinterpret_cast<std::atomic<std::uint64_t>*>(memory + vaddr);
        return ptr->compare_exchange_weak(expected, value, std::memory_order_seq_cst);
    }

    // =========================================================================
    // [YUZU-INSPIRED] InstructionCacheOperationRaised
    // =========================================================================
    // Yuzu reference: core/arm/dynarmic/arm_dynarmic_64.cpp, InstructionCacheOperationRaised
    // Handles IC IVAU (invalidate by VA) and IC IALLU (invalidate all) instructions.
    // Without this, any self-modifying guest code or JIT-generated code will
    // crash with a stale code cache hit. HLT the JIT after invalidation.
    // =========================================================================
    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op,
                                         std::uint64_t value) override {
        static constexpr std::uint64_t ICACHE_LINE_SIZE = 64;
        switch (op) {
        case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
            // Invalidate a single cache line aligned to 64 bytes
            const std::uint64_t cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
            emu->get_jit()->InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
            break;
        }
        case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
        case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
            emu->get_jit()->ClearCache();
            break;
        default:
            // Unknown cache op — silently ignore rather than crash
            break;
        }
        // Halt and re-enter so the JIT picks up the freshly invalidated blocks
        emu->get_jit()->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
    }

    // Read-only optimization: code section is read-only
    // EXCEPTION: RLSwordigo requires writable text segments for modded game data
    bool IsReadOnlyMemory(Dynarmic::A64::VAddr vaddr) override {
        // Check if this is RLSwordigo by looking at the loaded binary info
        extern BinarySelector g_binary_selector;
        const BinaryInfo* binfo = g_binary_selector.get_loaded_info();
        bool is_rlsw = (binfo && binfo->game_type == "RLSwordigo");
        
        if (!is_rlsw) {
            // libswordigo.so .text: 0x1000000 - 0x16B0000
            // libsre.so .text: 0x2000000 - 0x2010000
            if (vaddr >= 0x1000000 && vaddr < 0x16B0000) return true;
            if (vaddr >= 0x2000000 && vaddr < 0x2010000) return true;
        }
        return false;
    }

    // --- Code fetch ---
    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A64::VAddr vaddr) override {
        // Bridge region: MUST check first — return HLT instruction to trigger
        // ExceptionRaised(Breakpoint) for bridge dispatch
        if (vaddr >= 0xFF000000 && vaddr < 0xFF100000) {
            return 0xD4400000;  // HLT #0
        }
        // Magic LR page: return HLT — triggers ExceptionRaised(Breakpoint)
        // which sets function_returned flag and halts. Using RET here would
        // create an infinite loop (RET→X30=0xE0000000→RET→...)
        if (vaddr >= 0xE0000000 && vaddr < 0xE0001000) {
            return 0xD4400000;  // HLT #0
        }
        // Regular memory
        if (vaddr + 3 < mem_size) {
            std::uint32_t val;
            std::memcpy(&val, memory + vaddr, 4);
            return val;
        }
        return std::nullopt;  // Unmapped — triggers NoExecuteFault
    }

    // --- Exception handling ---
    void InterpreterFallback(Dynarmic::A64::VAddr pc, size_t num_instructions) override {
        // Check for MAGIC_LR region — function return
        if (pc >= 0xE0000000 && pc < 0xE0100000) {
            emu->function_returned = true;
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        // Check for bridge region — bridge call
        if (pc >= 0xFF000000 && pc < 0xFF100000) {
            emu->bridge_halt_requested = true;
            emu->bridge_halt_address = pc;
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        // [YUZU-INSPIRED] Log unhandled interpreter fallbacks like Yuzu does.
        // Yuzu reference: arm_dynarmic_64.cpp InterpreterFallback — logs + PrefetchAbort.
        // We skip past the instruction (can't abort since we don't have OS kernel),
        // but we log it at a reduced rate to avoid log spam.
        {
            static int fallback_log_count = 0;
            if (!emu->quiet_mode && fallback_log_count < 50) {
                fallback_log_count++;
                std::cerr << "[Dynarmic] InterpreterFallback at 0x" << std::hex << pc
                          << " for " << std::dec << num_instructions
                          << " instr (raw=0x" << std::hex;
                if (pc + 3 < mem_size)
                    std::cerr << *(std::uint32_t*)(memory + pc);
                else
                    std::cerr << "????";
                std::cerr << std::dec << ") — skipping" << std::endl;
            }
        }
        emu->get_jit()->SetPC(pc + num_instructions * 4);
    }

    void CallSVC(std::uint32_t swi) override {
        // SVC in our guest code — skip and continue
        if (!emu->quiet_mode) {
            std::cerr << "[Dynarmic] SVC #" << swi << " at PC=0x" << std::hex
                      << emu->get_jit()->GetPC() << std::dec << " — skipping" << std::endl;
        }
    }

    void ExceptionRaised(Dynarmic::A64::VAddr pc, Dynarmic::A64::Exception exception) override {
        using Exception = Dynarmic::A64::Exception;

        // =====================================================================
        // [YUZU-INSPIRED] Synchronisation / power-management exception interception.
        // =====================================================================
        // Yuzu reference: arm_dynarmic_64.cpp ExceptionRaised, cases:
        //   WaitForInterrupt / WaitForEvent / SendEvent / SendEventLocal / Yield
        // These ARM64 instructions (WFI, WFE, SEV, SEVL, YIELD) are frequently
        // used by guest spinlocks, condition variables, and the OS scheduler.
        // Without handling them the JIT will fall through to the default branch
        // which prints a critical error and aborts, causing a hard softlock.
        //
        // Strategy: when we see a WFE/WFI/Yield — run any pending deferred
        // threads immediately so they can write the flag the main loop is waiting
        // for. This turns what was a true softlock into a cooperative yield.
        // =====================================================================
        if (exception == Exception::WaitForInterrupt ||
            exception == Exception::WaitForEvent     ||
            exception == Exception::SendEvent        ||
            exception == Exception::SendEventLocal   ||
            exception == Exception::Yield) {
            // Run pending threads inline so they can release locks / write flags
            if (!emu->pending_threads.empty()) {
                static int wfe_unblock_count = 0;
                wfe_unblock_count++;
                if (wfe_unblock_count <= 10 || wfe_unblock_count % 100 == 0) {
                    std::cerr << "[Dynarmic/WFE] Cooperative yield at 0x" << std::hex << pc
                              << " (exc=" << static_cast<int>(exception) << std::dec
                              << ") — running " << emu->pending_threads.size()
                              << " pending thread(s)" << std::endl;
                }
                emu->run_pending_threads();
            }
            // Always return — let the JIT continue after the WFE/WFI instruction.
            // Returning without halting means Dynarmic advances PC past the instruction.
            return;
        }

        if (exception == Exception::Breakpoint) {
            // Magic LR — guest function returned normally
            if (pc >= 0xE0000000 && pc < 0xE0001000) {
                emu->function_returned = true;
                emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            // HLT instruction hit — this is our bridge dispatch mechanism
            // Check if PC is in bridge region
            if (pc >= 0xFF000000 && pc < 0xFF100000) {
                emu->bridge_halt_requested = true;
                emu->bridge_halt_address = pc;
                emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
                return;
            }
            // BRK in SRE (C++ exception recovery)
            if (pc >= 0x2000000 && pc < 0x2100000) {
                emu->bridge_halt_requested = true;
                emu->bridge_halt_address = pc;
                emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            // Unknown breakpoint — log and halt
            std::cerr << "[Dynarmic] Unknown breakpoint at 0x" << std::hex << pc << std::dec << std::endl;
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }

        if (exception == Exception::NoExecuteFault) {
            std::cerr << "[Dynarmic] NoExecuteFault at 0x" << std::hex << pc << std::dec << std::endl;
            std::cerr << "=== REGISTERS ===" << std::endl;
            for (int r = 0; r < 31; r++) {
                uint64_t val = emu->get_reg(r);
                const char* sym = sre_resolve_symbol(val);
                std::cerr << "  X" << r << " = 0x" << std::hex << val;
                if (sym) std::cerr << " [" << sym << "]";
                std::cerr << std::dec << std::endl;
            }
            std::cerr << "  SP = 0x" << std::hex << emu->get_jit()->GetSP() << std::dec << std::endl;
            std::cerr << "  PC = 0x" << std::hex << pc << std::dec << std::endl;
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
            return;
        }

        // All other exceptions — log and halt to prevent hangs
        std::cerr << "[Dynarmic] Exception at 0x" << std::hex << pc
                  << " type=" << static_cast<int>(exception) << std::dec << " — halting" << std::endl;
        std::cerr << "=== REGISTERS ===" << std::endl;
        for (int r = 0; r < 31; r++) {
            uint64_t val = emu->get_reg(r);
            const char* sym = sre_resolve_symbol(val);
            std::cerr << "  X" << r << " = 0x" << std::hex << val;
            if (sym) std::cerr << " [" << sym << "]";
            std::cerr << std::dec << std::endl;
        }
        std::cerr << "  SP = 0x" << std::hex << emu->get_jit()->GetSP() << std::dec << std::endl;
        std::cerr << "  PC = 0x" << std::hex << pc << std::dec << std::endl;

        uint8_t* mem = emu->get_memory_base();
        uint64_t active_state_ptr = *(uint64_t*)(mem + 0x16e9c20);
        std::cerr << "=== CAUGHT ACTIVE STATE ===" << std::endl;
        std::cerr << "  DAT_007e9c20 (0x16e9c20) = 0x" << std::hex << active_state_ptr << std::dec << std::endl;
        if (active_state_ptr && active_state_ptr < 0xE0000000) {
            uint64_t vtable = *(uint64_t*)(mem + active_state_ptr);
            std::cerr << "  Object vtable pointer = 0x" << std::hex << vtable << std::dec << std::endl;
            if (vtable && vtable < 0xE0000000) {
                for (int i = 0; i < 20; i++) {
                    uint64_t entry = *(uint64_t*)(mem + vtable + i * 8);
                    const char* sym = sre_resolve_symbol(entry);
                    std::cerr << "    vtable[0x" << std::hex << i * 8 << "] = 0x" << entry;
                    if (sym) std::cerr << " [" << sym << "]";
                    std::cerr << std::dec << std::endl;
                }
            }
        }

        std::cerr << "=== CODE AT PC ===" << std::endl;
        for (int i = -4; i < 4; i++) {
            uint64_t target_pc = pc + i * 4;
            if (target_pc < 0xE0000000) {
                uint32_t insn = *(uint32_t*)(mem + target_pc);
                std::cerr << "  " << (i == 0 ? "=> " : "   ") << "0x" << std::hex << target_pc << ": 0x" << insn << std::dec << std::endl;
            }
        }

        std::cerr << "=== GUEST STACK ===" << std::endl;
        uint64_t sp = emu->get_jit()->GetSP();
        for (int i = 0; i < 20; i++) {
            uint64_t target_sp = sp + i * 8;
            if (target_sp < 0xE0000000) {
                uint64_t val = *(uint64_t*)(mem + target_sp);
                const char* sym = sre_resolve_symbol(val);
                std::cerr << "  SP+0x" << std::hex << i * 8 << " (0x" << target_sp << ") = 0x" << val;
                if (sym) std::cerr << " [" << sym << "]";
                std::cerr << std::dec << std::endl;
            }
        }
        emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    // --- Tick management (instruction budget) ---
    void AddTicks(std::uint64_t ticks) override {
        ticks_elapsed += ticks;
        total_ticks += ticks;
        add_ticks_calls++;
    }

    std::uint64_t GetTicksRemaining() override {
        get_ticks_calls++;
        if (ticks_elapsed >= tick_budget) return 0;
        return tick_budget - ticks_elapsed;
    }

    std::uint64_t GetCNTPCT() override {
        return total_ticks;
    }

    // Reset tick counter for a new run
    void ResetTicks(uint64_t budget) {
        ticks_elapsed = 0;
        tick_budget = budget;
    }

    uint64_t GetTicksElapsed() const { return ticks_elapsed; }

private:
    EmulatorDynarmic64* emu;
    uint8_t* memory;
    uint64_t mem_size;
    uint64_t ticks_elapsed = 0;
    uint64_t tick_budget = 1000000000ULL;  // 1B ticks default
    uint64_t total_ticks = 0;
    uint64_t add_ticks_calls = 0;
    uint64_t get_ticks_calls = 0;
};

// ============================================================================
// EmulatorDynarmic64 Implementation
// ============================================================================

static SwordigoMemory* g_dyn_memory = nullptr;

EmulatorDynarmic64::EmulatorDynarmic64(uint8_t* guest_mem, uint64_t size)
    : memory(guest_mem), mem_size(size) {

    // Create memory callbacks
    g_dyn_memory = new SwordigoMemory(this, memory, mem_size);

    // Configure Dynarmic
    Dynarmic::A64::UserConfig config;
    config.callbacks = g_dyn_memory;

    // Enable BlockLinking optimization for maximum emulation performance.
    config.optimizations = Dynarmic::all_safe_optimizations;

    // Enable fastmem optimization since we mapped the entire 4GB virtual address space
    // and filled the bridge and magic return regions with valid HLT instructions.
    config.fastmem_pointer = (uintptr_t)memory;
    config.fastmem_address_space_bits = 32;

    // TPIDR_EL0 storage
    config.tpidr_el0 = &tpidr_el0_value;

    // Code cache: 128MB (maximum for good performance)
    config.code_cache_size = 128 * 1024 * 1024;

    // Use wall clock for counter (we don't need cycle-accurate timing)
    config.wall_clock_cntpct = true;

    // ExclusiveMonitor: required for LDXR/STXR instructions.
    // Without it, Dynarmic asserts on any exclusive memory operation.
    // We use 1 processor since this is single-threaded.
    static Dynarmic::ExclusiveMonitor exclusive_monitor(1);
    config.global_monitor = &exclusive_monitor;
    config.processor_id = 0;

    // Create the JIT
    jit = std::make_unique<Dynarmic::A64::Jit>(config);

    // Set stack pointer (same as Unicorn: near top of guest memory)
    uint64_t stack_base = size - 0x1000;
    jit->SetSP(stack_base);

    std::cout << "[Dynarmic] JIT initialized (callback memory, 128MB code cache)"
              << ", stack at 0x" << std::hex << stack_base << std::dec << std::endl;
}

EmulatorDynarmic64::~EmulatorDynarmic64() {
    jit.reset();
    delete g_dyn_memory;
    g_dyn_memory = nullptr;
}

// --- Register access ---

void EmulatorDynarmic64::set_pc(uint64_t pc) { jit->SetPC(pc); }
uint64_t EmulatorDynarmic64::get_pc() { return jit->GetPC(); }

uint64_t EmulatorDynarmic64::get_lr() { return jit->GetRegister(30); }
void EmulatorDynarmic64::set_lr(uint64_t lr) { jit->SetRegister(30, lr); }

void EmulatorDynarmic64::set_reg(int reg, uint64_t value) {
    if (reg <= 28)       jit->SetRegister(reg, value);
    else if (reg == 29)  jit->SetRegister(29, value);  // FP
    else if (reg == 30)  jit->SetRegister(30, value);  // LR
    else                 jit->SetSP(value);              // SP
}

uint64_t EmulatorDynarmic64::get_reg(int reg) {
    if (reg <= 28)       return jit->GetRegister(reg);
    else if (reg == 29)  return jit->GetRegister(29);
    else if (reg == 30)  return jit->GetRegister(30);
    else                 return jit->GetSP();
}

void EmulatorDynarmic64::set_dreg(int reg, double value) {
    Dynarmic::A64::Vector vec = jit->GetVector(reg);
    std::memcpy(&vec[0], &value, sizeof(double));
    jit->SetVector(reg, vec);
}

double EmulatorDynarmic64::get_dreg(int reg) {
    Dynarmic::A64::Vector vec = jit->GetVector(reg);
    double val;
    std::memcpy(&val, &vec[0], sizeof(double));
    return val;
}

void EmulatorDynarmic64::set_sreg(int reg, float value) {
    Dynarmic::A64::Vector vec = jit->GetVector(reg);
    std::memcpy(&vec[0], &value, sizeof(float));
    jit->SetVector(reg, vec);
}

float EmulatorDynarmic64::get_sreg(int reg) {
    Dynarmic::A64::Vector vec = jit->GetVector(reg);
    float val;
    std::memcpy(&val, &vec[0], sizeof(float));
    return val;
}

// --- Execution ---

void EmulatorDynarmic64::run(uint64_t start_pc) {
    static const uint64_t MAGIC_LR = 0xE0000000;
    static const uint64_t TICK_BUDGET = 10000000ULL;
    static const int MAX_CHUNKS = 50000;  // Was 500 — too low for boot functions

    // Guard: reject calls into string/data area (0x10000–0x80000).
    // The files_dir string sits at 0x20000; branching there means a corrupted
    // function pointer was passed to call(). Log it and bail out immediately
    // so we can see which address is bad.
    if (start_pc >= 0x10000 && start_pc < 0x80000) {
        static int bad_pc_count = 0;
        if (bad_pc_count < 10) {
            std::cerr << "[BAD-PC] call() to 0x" << std::hex << start_pc
                      << " — likely a corrupted function pointer! (call #"
                      << std::dec << ++bad_pc_count << ")" << std::endl;
            // Print registers to help identify the variable
            std::cerr << "  X0=0x" << std::hex << jit->GetRegister(0)
                      << " X1=0x" << jit->GetRegister(1)
                      << " X2=0x" << jit->GetRegister(2)
                      << " SP=0x" << jit->GetSP() << std::dec << std::endl;
        }
        function_returned = true;
        return;
    }

    // Set LR to magic sentinel (function return detection)
    jit->SetRegister(30, MAGIC_LR);
    jit->SetPC(start_pc);

    // Save entry SP for stack leak prevention
    uint64_t entry_sp = jit->GetSP();

    auto t0 = std::chrono::steady_clock::now();

    // Debug: log first few calls
    static int call_count = 0;
    call_count++;
    bool verbose = (call_count <= 20);

    // (Step trace removed — sre_init verified working)

    uint64_t curr_pc = start_pc;
    int chunk = 0;
    int same_pc_count = 0;
    uint64_t last_chunk_pc = 0;
    int bridge_calls = 0;

    // =========================================================================
    // [YUZU-INSPIRED] Bridge/PC spinloop detection — NEW + LEGACY both present.
    // =========================================================================
    // NEW:    WFE/WFI/Yield exceptions are handled inside ExceptionRaised() above,
    //         which runs pending threads cooperatively without needing PC counting.
    // LEGACY: The bridge-spin counter (same_bridge_count) and PC-repetition counter
    //         (same_pc_count) below are preserved as a belt-and-suspenders fallback
    //         for spinloops that do NOT use ARM sync instructions.
    // =========================================================================

    /* LEGACY_SPINLOOP_DETECTION_START
     * Original bridge-spin tracker variables.
     * Still active — these handle non-WFE spinloops (e.g. tight assembly busy-waits
     * that check a memory flag without issuing any synchronisation instruction).
     */
    uint64_t last_bridge_addr = 0;
    int same_bridge_count = 0;
    static int total_spinloop_logs = 0;  // Suppress after too many
    /* LEGACY_SPINLOOP_DETECTION_END */

    // Wall clock time limit per function call
    auto wall_start = std::chrono::steady_clock::now();
    
    // RLSwordigo has some slow functions that need more time
    // Check if we're running RLSW and increase limit accordingly
    extern BinarySelector g_binary_selector;
    const BinaryInfo* binfo = g_binary_selector.get_loaded_info();
    bool is_rlsw = (binfo && binfo->game_type == "RLSwordigo");
    int wall_limit_ms = is_rlsw ? 120000 : 30000;  // 2 min for RLSW, 30s for vanilla

    for (chunk = 0; chunk < MAX_CHUNKS; chunk++) {
        // Wall clock safety check
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - wall_start).count();
        if (elapsed_ms > wall_limit_ms) {
            std::cerr << "[Dynarmic] Wall clock limit (" << wall_limit_ms
                      << "ms) hit for 0x" << std::hex << start_pc << std::dec
                      << " — force-returning" << std::endl;
            jit->SetPC(MAGIC_LR);
            jit->SetSP(entry_sp);
            break;
        }

        // Reset tick budget
        g_dyn_memory->ResetTicks(TICK_BUDGET);

        // Clear any previous halt — also clear CacheInvalidation so JIT resumes
        // after an IC IVAU / IC IALLU issued by InstructionCacheOperationRaised.
        jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        jit->ClearHalt(Dynarmic::HaltReason::UserDefined2);
        jit->ClearHalt(Dynarmic::HaltReason::CacheInvalidation); // [YUZU-INSPIRED]
        bridge_halt_requested = false;
        function_returned = false;

        /*
        if (verbose && chunk == 0) {
            std::cerr << "[Dynarmic/dbg] About to call jit->Run() PC=0x"
                      << std::hex << jit->GetPC() << " LR=0x" << jit->GetRegister(30)
                      << " SP=0x" << jit->GetSP() << std::dec << std::endl;
        }
        */

        // Run!
        Dynarmic::HaltReason hr = jit->Run();
        curr_pc = jit->GetPC();

        /*
        if (verbose && chunk < 5) {
            std::cerr << "[Dynarmic/dbg] chunk=" << chunk
                      << " PC=0x" << std::hex << curr_pc
                      << " hr=" << static_cast<uint32_t>(hr)
                      << std::dec << " bridge_halt=" << bridge_halt_requested
                      << " ticks=" << g_dyn_memory->GetTicksElapsed() << std::endl;
        }
        */

        // [YUZU-INSPIRED] Handle CacheInvalidation halt — JIT needs to re-enter
        // after flushing invalidated blocks. Don't count this as a chunk.
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::CacheInvalidation)) {
            chunk--;
            continue;
        }

        // Check for bridge call (UserDefined1)
        if (bridge_halt_requested && Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1)) {
            bridge_calls++;

            /* LEGACY_SPINLOOP_DETECTION_START
             * Bridge-spin counter: tracks consecutive halts at the same bridge
             * address. This catches busy-wait loops that call a bridge stub
             * (e.g. clock_gettime, yield shim) in a tight loop without using
             * ARM WFE/WFI, which would be caught by ExceptionRaised instead.
             *
             * Don't track return_pc — callers can alternate between two sites
             * (e.g. mutex_lock and mutex_unlock) creating an undetectable pattern.
             */
            if (bridge_halt_address == last_bridge_addr) {
                same_bridge_count++;
            } else {
                same_bridge_count = 0;
                last_bridge_addr = bridge_halt_address;
            }

            // ── Scene Transition Deadlock Unblock ─────────────────────────────
            // During level transitions, updateApplication spins on a bridge
            // (typically clock_gettime or a yield stub) while waiting for the
            // background scene-load thread to complete and set a "loaded" flag.
            // That thread is queued in pending_threads but can't run because we
            // are still inside this run() call.
            //
            // Fix: As soon as we detect the bridge spin AND pending threads exist,
            // run them inline right here. The nested call() is safe because
            // call() saves/restores all JIT registers around jit->Run().
            if (same_bridge_count >= 4 && !pending_threads.empty()) {
                static int thread_unblock_count = 0;
                thread_unblock_count++;
                if (thread_unblock_count <= 10) {
                    std::cerr << "[Thread64/Dyn] LEGACY UNBLOCKING spin at bridge 0x"
                              << std::hex << bridge_halt_address
                              << " (same_bridge_count=" << std::dec << same_bridge_count
                              << " bridge_calls=" << bridge_calls
                              << " pending=" << pending_threads.size() << ")" << std::endl;
                }
                // Handle the current bridge call first so the JIT state is clean
                handle_bridge_call(bridge_halt_address);
                bridge_halt_requested = false;
                curr_pc = jit->GetPC();
                if (curr_pc == MAGIC_LR) break;

                // Now run all pending threads inline
                run_pending_threads();

                // Reset spin counter so we detect the NEXT spin wave too
                same_bridge_count = 0;
                chunk--;
                continue;
            }

            if (same_bridge_count >= 50) {
                // Spinlock on same bridge with no pending threads —
                // wall clock limit is the real safeguard; don't count against MAX_CHUNKS
                handle_bridge_call(bridge_halt_address);
                bridge_halt_requested = false;
                curr_pc = jit->GetPC();
                if (curr_pc == MAGIC_LR) break;
                chunk--;
                continue;
            }
            /* LEGACY_SPINLOOP_DETECTION_END */

            handle_bridge_call(bridge_halt_address);
            bridge_halt_requested = false;

            curr_pc = jit->GetPC();
            if (curr_pc == MAGIC_LR) break;

            // Normal bridge call — don't count as chunk
            chunk--;
            continue;
        }

        // Check for function return (UserDefined2 + function_returned flag)
        if (function_returned && Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
            /*
            if (verbose) {
                std::cerr << "[Dynarmic/dbg] Function returned cleanly (bridge_calls=" << bridge_calls << ")" << std::endl;
            }
            */
            break;
        }

        // Check for SRE exception (UserDefined2 without function_returned)
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
            std::cerr << "[Dynarmic] Halt (UserDefined2) at 0x" << std::hex << curr_pc
                      << " — force returning" << std::dec << std::endl;
            jit->SetPC(MAGIC_LR);
            jit->SetSP(entry_sp);
            break;
        }

        // Check for MemoryAbort (e.g. NoExecuteFault or unmapped memory)
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::MemoryAbort)) {
            std::cerr << "[Dynarmic] MemoryAbort halt at PC=0x" << std::hex << curr_pc << std::dec << " — stopping execution" << std::endl;
            set_faulted(true);
            jit->SetPC(MAGIC_LR);
            jit->SetSP(entry_sp);
            break;
        }

        // Check for StepLimit / Tick budget expiration (normal JIT yield).
        // If the JIT stops due to tick exhaustion without hitting a bridge,
        // a memory fault, or a return, the guest is likely spinning on a
        // memory flag in a tight guest assembly loop.
        // We must run any pending threads now so they can execute and write the flag.
        if (!Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1) &&
            !Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2) &&
            !Dynarmic::Has(hr, Dynarmic::HaltReason::MemoryAbort)) 
        {
            if (!pending_threads.empty()) {
                std::cerr << "[Thread64/Dyn] TICK EXHAUSTED inside guest spin — executing "
                          << pending_threads.size() << " pending threads to unblock it" << std::endl;
                run_pending_threads();
            }
        }

        // Check if function completed
        if (curr_pc == MAGIC_LR) break;

        /* LEGACY_SPINLOOP_DETECTION_START
         * PC-repetition spin detector: if the PC doesn't advance across tick
         * budget boundaries, the guest is stuck in a tight loop that neither
         * uses WFE/WFI (caught by ExceptionRaised) nor calls a bridge stub
         * (caught by bridge_spin counter above). Force a return as last resort.
         * The [YUZU-INSPIRED] WFE path above should handle most real cases;
         * this is the final belt-and-suspenders guard.
         */
        if (curr_pc == last_chunk_pc) {
            same_pc_count++;
            if (same_pc_count >= 3) {
                std::cerr << "[Dynarmic] LEGACY Spin loop: PC=0x" << std::hex << curr_pc
                          << std::dec << " unchanged for " << same_pc_count
                          << " chunks — force-returning" << std::endl;
                jit->SetPC(MAGIC_LR);
                jit->SetSP(entry_sp);
                break;
            }
        } else {
            same_pc_count = 0;
        }
        last_chunk_pc = curr_pc;
        /* LEGACY_SPINLOOP_DETECTION_END */

        // Log heavy functions
        if (chunk == 1) {
            std::cerr << "[Dynarmic] Heavy function 0x" << std::hex << start_pc
                      << " — chunk " << std::dec << (chunk + 1) << "/" << MAX_CHUNKS
                      << " (PC=0x" << std::hex << curr_pc << ")" << std::dec << std::endl;
        }
    }

    // If loop exited by exhausting MAX_CHUNKS, force a clean return
    if (chunk >= MAX_CHUNKS) {
        std::cerr << "[Dynarmic] MAX_CHUNKS (" << MAX_CHUNKS << ") exhausted for 0x"
                  << std::hex << start_pc << std::dec
                  << " (bridge_calls=" << bridge_calls << ") — force-returning" << std::endl;
        jit->SetPC(MAGIC_LR);
        jit->SetSP(entry_sp);
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Log slow calls (>50ms)
    if (ms > 50.0) {
        static int slow_count = 0;
        slow_count++;
        if (slow_count <= 30) {
            std::cerr << "[PERF/Dynarmic] SLOW call at 0x" << std::hex << start_pc << std::dec
                      << " took " << (int)ms << "ms (bridge_calls=" << bridge_calls << ")" << std::endl;
        }
    }

    if (!quiet_mode) {
        uint64_t final_pc = jit->GetPC();
        std::cout << "[Dynarmic] Function stopped at PC=0x" << std::hex << final_pc << std::dec << std::endl;
    }
}

uint64_t EmulatorDynarmic64::call(uint64_t addr, const std::vector<uint64_t>& args) {
    if (!quiet_mode) {
        std::cout << "[Dynarmic] Calling guest function at 0x" << std::hex << addr << std::dec << std::endl;
    }

    // Save previous CPU register state and reentrancy flags
    SavedCpuState state;
    for (int i = 0; i < 31; ++i) {
        state.regs[i] = jit->GetRegister(i);
    }
    state.sp = jit->GetSP();
    state.pc = jit->GetPC();
    state.pstate = jit->GetPstate();
    for (int i = 0; i < 32; ++i) {
        state.vectors[i] = jit->GetVector(i);
    }
    state.bridge_halt_requested = bridge_halt_requested;
    state.bridge_halt_address = bridge_halt_address;
    state.function_returned = function_returned;

    // ARM64 AAPCS: first 8 integer args go in X0-X7
    for (size_t i = 0; i < args.size() && i < 8; ++i) {
        set_reg(i, args[i]);
    }

    run(addr);
    uint64_t result = jit->GetRegister(0); // Read return value BEFORE restoring state

    // Restore previous state
    for (int i = 0; i < 31; ++i) {
        jit->SetRegister(i, state.regs[i]);
    }
    jit->SetSP(state.sp);
    jit->SetPC(state.pc);
    jit->SetPstate(state.pstate);
    for (int i = 0; i < 32; ++i) {
        jit->SetVector(i, state.vectors[i]);
    }
    bridge_halt_requested = state.bridge_halt_requested;
    bridge_halt_address = state.bridge_halt_address;
    function_returned = state.function_returned;

    if (!quiet_mode) {
        std::cout << "[Dynarmic] Function returned with X0=0x" << std::hex << result << std::dec << std::endl;
    }
    return result;
}

// --- Bridge handling ---

void EmulatorDynarmic64::handle_bridge_call(uint64_t address) {
    if (bridge) {
        uint64_t lr = get_lr();
        bridge->call_handler(address, (void*)this);

        // Handle redirect (same as Unicorn backend)
        if (redirect_pc != 0) {
            set_lr(lr);
            set_pc(redirect_pc);
            redirect_pc = 0;
        } else {
            set_pc(lr);
        }
    }
}

uint64_t EmulatorDynarmic64::get_bridge_base() { return 0xFF000000; }

void EmulatorDynarmic64::protect_memory(uint64_t addr, uint64_t size, uint32_t perms) {
    // Dynarmic doesn't have per-page memory protection in the same way.
    // If the region contains code, invalidate the code cache for it.
    if (perms & 0x4) {  // UC_PROT_EXEC equivalent
        jit->InvalidateCacheRange(addr, size);
    }
    std::cout << "[Dynarmic] protect_memory(0x" << std::hex << addr
              << ", 0x" << size << ", " << std::dec << perms << ") — cache invalidated" << std::endl;
}

// --- Debugging ---

void EmulatorDynarmic64::record_pc(uint64_t pc) {
    if (pc >= 0x1000000 && pc < 0x2000000) {
        last_pcs.push_back(pc);
        if (last_pcs.size() > 50) {
            last_pcs.erase(last_pcs.begin());
        }
    }
}

void EmulatorDynarmic64::print_trace() {
    std::cerr << "--- Dynarmic PC Trace (Last " << last_pcs.size() << " entries) ---" << std::endl;
    for (size_t i = 0; i < last_pcs.size(); ++i) {
        std::cerr << "  #" << i << ": 0x" << std::hex << last_pcs[i] << std::dec << std::endl;
    }
    std::cerr << "--------------------------------------------" << std::endl;
}
