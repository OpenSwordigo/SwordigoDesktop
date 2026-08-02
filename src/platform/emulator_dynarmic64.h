#ifndef EMULATOR_DYNARMIC64_H
#define EMULATOR_DYNARMIC64_H

#include "i_emulator_arm64.h"
#include <stdint.h>
#include <vector>
#include <iostream>
#include <memory>

// Forward declare Dynarmic types (avoid including heavy headers in .h)
namespace Dynarmic { namespace A64 { class Jit; } }

// ============================================================================
// EmulatorDynarmic64 — High-performance ARM64 JIT backend using Dynarmic.
//
// Dynarmic is a dynamic recompiler that translates ARM64 → host x86_64 with
// advanced optimizations (block chaining, register allocation, NEON→SSE).
// Expected 5-10x speedup over Unicorn's TCG interpreter.
//
// Bridge mechanism: The bridge trampoline region (0xFF000000+) is filled
// with HLT instructions. When guest code branches there, Dynarmic halts
// with a UserDefined reason, we dispatch to the JNI bridge handler,
// then resume execution.
// ============================================================================

class EmulatorDynarmic64 : public IEmulatorArm64 {
public:
    EmulatorDynarmic64(uint8_t* guest_mem, uint64_t mem_size);
    ~EmulatorDynarmic64() override;

    void set_pc(uint64_t pc) override;
    uint64_t get_pc() override;
    uint64_t get_lr() override;
    void set_lr(uint64_t lr) override;

    void set_reg(int reg, uint64_t value) override;
    uint64_t get_reg(int reg) override;
    void set_tpidr_el0(uint64_t val) override { tpidr_el0_value = val; }

    void set_dreg(int reg, double value) override;
    double get_dreg(int reg) override;
    void set_sreg(int reg, float value) override;
    float get_sreg(int reg) override;

    void run(uint64_t start_pc) override;
    uint64_t call(uint64_t addr, const std::vector<uint64_t>& args) override;

    void set_bridge(JniBridge64* b) override { this->bridge = b; }
    void handle_bridge_call(uint64_t address) override;
    uint64_t get_bridge_base() override;
    uint8_t* get_memory_base() override { return memory; }
    void* get_uc_handle() override { return nullptr; }  // No Unicorn handle

    void record_pc(uint64_t pc) override;
    void print_trace() override;

    const char* engine_name() const override { return "Dynarmic"; }

    void queue_thread(uint64_t start_routine, uint64_t arg) override {
        pending_threads.push_back({start_routine, arg});
        std::cerr << "[Thread64/Dyn] Queued deferred thread func=0x" << std::hex
                  << start_routine << " arg=0x" << arg << std::dec << std::endl;
    }

    void run_pending_threads() override {
        while (!pending_threads.empty()) {
            DeferredThread t = pending_threads.front();
            pending_threads.erase(pending_threads.begin());
            std::cout << "[Thread64/Dyn] Running deferred thread func=0x" << std::hex
                      << t.start_routine << " arg=0x" << t.arg << std::dec << std::endl;
            call(t.start_routine, {t.arg});
            std::cout << "[Thread64/Dyn] Deferred thread completed." << std::endl;
        }
    }

    bool has_pending_threads() const override { return !pending_threads.empty(); }

    void protect_memory(uint64_t addr, uint64_t size, uint32_t perms) override;

    // The Dynarmic Jit instance (public for callbacks to access)
    Dynarmic::A64::Jit* get_jit() { return jit.get(); }

    // ProHook Phase 1: Invalidate the JIT code cache for a guest address range.
    // Called by srehost_impl.cpp without needing the full Dynarmic::A64::Jit type.
    void invalidate_cache_range(uint64_t guest_vaddr, size_t size);

    // Bridge halt flag — set by memory callback when PC enters bridge region
    bool bridge_halt_requested = false;
    uint64_t bridge_halt_address = 0;

    // Function return flag — set when PC hits MAGIC_LR (0xE0000000) HLT
    bool function_returned = false;

    // Quiet mode — suppresses frequent InterpreterFallback log spam.
    // Set to false only for deep debugging.
    bool quiet_mode = true;

private:
    uint8_t* memory;
    uint64_t mem_size;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::vector<uint64_t> last_pcs;

    // TPIDR_EL0 storage (Dynarmic needs a pointer to this)
    uint64_t tpidr_el0_value = 0;
};

#endif
