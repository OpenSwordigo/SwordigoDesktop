#ifndef EMULATOR_ARM64_H
#define EMULATOR_ARM64_H

#include <stdint.h>
#include <string>
#include <vector>
#include <iostream>
#include "i_emulator_arm64.h"
#include "loader/elf_loader_arm64.h"
#include "jni/jni_bridge_arm64.h"

// ARM64 emulator — Unicorn backend (QEMU TCG)
// Implements IEmulatorArm64 interface for backend-swappable emulation.
//
// Key differences from ARM32 Emulator:
//   - 64-bit registers (X0-X30, SP, PC)
//   - No Thumb mode (single instruction set)
//   - call() passes args in X0-X7 (8 regs!) instead of R0-R3
//   - Dedicated D0-D7 registers for float/double args
//   - Bridge trampoline uses 'ret' (0xD65F03C0) instead of 'bx lr' (0x4770)

class EmulatorArm64 : public IEmulatorArm64 {
public:
    EmulatorArm64(uint8_t* guest_mem, uint64_t mem_size);
    ~EmulatorArm64() override;

    void set_pc(uint64_t pc) override;
    uint64_t get_pc() override;
    uint64_t get_lr() override;
    void set_lr(uint64_t lr) override;
    
    // Integer registers X0-X30
    void set_reg(int reg, uint64_t value) override;
    uint64_t get_reg(int reg) override;
    void set_tpidr_el0(uint64_t val) override;
    
    // SIMD/FP registers D0-D31 (double) and S0-S31 (float)
    void set_dreg(int reg, double value) override;
    double get_dreg(int reg) override;
    void set_sreg(int reg, float value) override;
    float get_sreg(int reg) override;

    // Execute function at addr, returns X0
    void run(uint64_t start_pc) override;
    uint64_t call(uint64_t addr, const std::vector<uint64_t>& args) override;

    void set_bridge(JniBridge64* b) override { this->bridge = b; }
    void handle_bridge_call(uint64_t address) override;
    uint64_t get_bridge_base() override;
    uint8_t* get_memory_base() override { return memory; }
    void* get_uc_handle() override { return uc; }

    // Debugging
    void record_pc(uint64_t pc) override;
    void print_trace() override;
    
    // Engine info
    const char* engine_name() const override { return "Unicorn"; }

    // Thread management
    void queue_thread(uint64_t start_routine, uint64_t arg) override {
        pending_threads.push_back({start_routine, arg});
        std::cerr << "[Thread64] Queued deferred thread func=0x" << std::hex 
                  << start_routine << " arg=0x" << arg << std::dec << std::endl;
    }
    
    void run_pending_threads() override {
        while (!pending_threads.empty()) {
            DeferredThread t = pending_threads.front();
            pending_threads.erase(pending_threads.begin());
            std::cout << "[Thread64] Running deferred thread func=0x" << std::hex 
                      << t.start_routine << " arg=0x" << t.arg << std::dec << std::endl;
            call(t.start_routine, {t.arg});
            std::cout << "[Thread64] Deferred thread completed." << std::endl;
        }
    }
    
    bool has_pending_threads() const override { return !pending_threads.empty(); }
    
    // Change Unicorn memory protection for a region
    void protect_memory(uint64_t addr, uint64_t size, uint32_t perms) override;

private:
    uint8_t* memory;
    uint64_t size;
    void* uc; // uc_engine*
    std::vector<uint64_t> last_pcs;
};

#endif
