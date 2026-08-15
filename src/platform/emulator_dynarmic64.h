#ifndef EMULATOR_DYNARMIC64_H
#define EMULATOR_DYNARMIC64_H

#include "i_emulator_arm64.h"
#include <stdint.h>
#include <vector>
#include <iostream>
#include <memory>
#include <cstring>

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

    // Clear a previously-latched fault so guest execution can be retried on a
    // later frame. Also clears Dynarmic's halt reason and the recoverable-fault
    // bookkeeping so a fresh call() starts clean.
    void clear_faulted() override;

    void set_bridge(JniBridge64* b) override { this->bridge = b; }
    void handle_bridge_call(uint64_t address) override;
    uint64_t get_bridge_base() override;
    uint8_t* get_memory_base() override { return memory; }
    uint64_t get_memory_size() override { return mem_size; }
    void* get_uc_handle() override { return nullptr; }  // No Unicorn handle

    void record_pc(uint64_t pc) override;
    void print_trace() override;

    const char* engine_name() const override { return "Dynarmic"; }

    void queue_thread(uint64_t start_routine, uint64_t arg) override {
        pending_threads.push_back({start_routine, arg});
        std::cerr << "[Thread64/Dyn] Queued deferred thread func=0x" << std::hex
                  << start_routine << " arg=0x" << arg << std::dec << std::endl;
    }

    void run_pending_threads() override;

    bool has_pending_threads() const override { return !pending_threads.empty(); }

    void configure_recovery_context(uint64_t depth_addr,
                                    uint64_t stack_addr,
                                    uint32_t stack_bytes) override;

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

    // Recoverable-fault policy state (see run() MemoryAbort branch).
    // consecutive_mem_faults: number of guest memory faults seen back-to-back
    //   without an intervening clean MAGIC_LR return. Reset to 0 on a clean
    //   return; once it reaches a hard threshold we permanently poison the
    //   emulator via set_faulted(true).
    // last_call_faulted: set true when the CURRENT run() call aborted due to a
    //   recoverable memory fault (as opposed to permanent poison). Cleared at
    //   the top of every run() invocation.
    int consecutive_mem_faults = 0;
    bool last_call_faulted = false;

private:
    uint8_t* memory;
    uint64_t mem_size;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::vector<uint64_t> last_pcs;

    // TPIDR_EL0 storage (Dynarmic needs a pointer to this)
    uint64_t tpidr_el0_value = 0;

    // Guest addresses of libsre's process-global setjmp recovery state.
    // Deferred guest threads are independent CPU contexts, so this state is
    // snapshotted and cleared while each one runs.
    uint64_t recovery_depth_addr = 0;
    uint64_t recovery_stack_addr = 0;
    uint32_t recovery_stack_bytes = 0;
};

#endif
