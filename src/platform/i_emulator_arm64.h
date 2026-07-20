#ifndef I_EMULATOR_ARM64_H
#define I_EMULATOR_ARM64_H
#pragma once
/* ============================================================================
 * IEmulatorArm64 — Abstract interface for ARM64 CPU emulation backends.
 *
 * Implementations:
 *   - EmulatorUnicorn64  (Unicorn/QEMU TCG — stable, slower)
 *   - EmulatorDynarmic64 (Dynarmic JIT — fast, experimental)
 *
 * The JNI bridge layer (JniBridge64) and main game loop interact ONLY
 * through this interface, making the backend swappable at compile time
 * or runtime.
 * ============================================================================
 */

#include <stdint.h>
#include <vector>
#include <string>

// Forward declarations
class JniBridge64;

class IEmulatorArm64 {
public:
    virtual ~IEmulatorArm64() = default;

    // --- Program counter ---
    virtual void set_pc(uint64_t pc) = 0;
    virtual uint64_t get_pc() = 0;
    virtual uint64_t get_lr() = 0;
    virtual void set_lr(uint64_t lr) = 0;

    // --- Integer registers X0-X30, SP ---
    virtual void set_reg(int reg, uint64_t value) = 0;
    virtual uint64_t get_reg(int reg) = 0;

    // --- SIMD/FP registers ---
    virtual void set_dreg(int reg, double value) = 0;
    virtual double get_dreg(int reg) = 0;
    virtual void set_sreg(int reg, float value) = 0;
    virtual float get_sreg(int reg) = 0;

    // --- Execution ---
    virtual void run(uint64_t start_pc) = 0;
    virtual uint64_t call(uint64_t addr, const std::vector<uint64_t>& args) = 0;

    // --- Bridge ---
    virtual void set_bridge(JniBridge64* bridge) = 0;
    virtual void handle_bridge_call(uint64_t address) = 0;
    virtual uint64_t get_bridge_base() = 0;

    // --- Memory ---
    virtual uint8_t* get_memory_base() = 0;
    virtual void* get_uc_handle() = 0;  // Returns nullptr for non-Unicorn backends
    virtual void protect_memory(uint64_t addr, uint64_t size, uint32_t perms) = 0;

    // --- Thread management ---
    struct DeferredThread {
        uint64_t start_routine;
        uint64_t arg;
    };
    virtual void queue_thread(uint64_t start_routine, uint64_t arg) = 0;
    virtual void run_pending_threads() = 0;
    virtual bool has_pending_threads() const = 0;

    // --- Debugging ---
    virtual void record_pc(uint64_t pc) = 0;
    virtual void print_trace() = 0;

    // --- Engine info ---
    virtual const char* engine_name() const = 0;  // "Unicorn" or "Dynarmic"

    // --- Public state (accessed by bridge handlers) ---
    JniBridge64* bridge = nullptr;
    bool fault_flag = false;
    virtual bool has_faulted() const { return fault_flag; }
    virtual void set_faulted(bool f) { fault_flag = f; }
    bool quiet_mode = false;
    uint64_t redirect_pc = 0;
    std::vector<DeferredThread> pending_threads;
};

#endif
