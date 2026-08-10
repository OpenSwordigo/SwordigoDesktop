#ifndef EMULATOR_DYNARMIC32_H
#define EMULATOR_DYNARMIC32_H

#include "emulator.h"
#include <stdint.h>
#include <vector>
#include <memory>

// Forward declare Dynarmic types (avoid including heavy headers in .h)
namespace Dynarmic { namespace A32 { class Jit; } }

// ============================================================================
// EmulatorDynarmic32 — ARM32 (AArch32) JIT backend using Dynarmic's A32::Jit.
//
// Drop-in replacement for the ARM32 `Emulator` (Unicorn/TCG) class. It inherits
// from Emulator so that JniBridge handlers (which cast the emu pointer back to
// Emulator*) keep working — all CPU-facing methods are virtual and overridden.
//
// Bridge mechanism (mirrors EmulatorDynarmic64):
//   * The bridge region (0xFF000000-0xFF100000) and the magic function-return
//     page (0xE0000000) are intercepted in the MemoryReadCode callback. When the
//     JIT fetches an instruction there we call HaltExecution with a user-defined
//     reason, then run() dispatches the bridge handler / detects the return.
//   * The ARM32 hero/scene observer hooks (camera ctor, GameSceneController/
//     HeroEntity/CharController/Health/Mana Update) fire on JIT code fetch of
//     their function-entry addresses, populating the same globals the Unicorn
//     backend's UC_HOOK_CODE hooks wrote. They re-fire whenever a fresh block is
//     compiled (scene loads, cache invalidation), which is exactly when the
//     captured object pointers change.
// ============================================================================
class EmulatorDynarmic32 : public Emulator {
public:
    EmulatorDynarmic32(uint8_t* guest_mem, uint32_t mem_size);
    ~EmulatorDynarmic32() override;

    void set_pc(uint32_t pc) override;
    uint32_t get_pc() override;
    uint32_t get_lr() override;
    void set_reg(int reg, uint32_t value) override;
    uint32_t get_reg(int reg) override;
    float get_vfp_reg(int reg) override;

    void run(uint32_t start_pc) override;
    uint32_t call(uint32_t addr, const std::vector<uint32_t>& args) override;

    void handle_bridge_call(uint32_t address) override;
    uint32_t get_bridge_base() override { return 0xFF000000; }
    uint8_t* get_memory_base() override { return memory; }
    void* get_uc_handle() override { return nullptr; }  // No Unicorn handle

    void record_pc(uint32_t pc) override;
    void print_trace() override;

    const char* engine_name() const { return "Dynarmic32"; }

    // The Dynarmic Jit instance (public for callbacks to access)
    Dynarmic::A32::Jit* get_jit() { return jit.get(); }

    // Bridge halt state — set by the memory callback, consumed by run()
    bool bridge_halt_requested = false;
    uint32_t bridge_halt_address = 0;
    bool function_returned = false;

    // Redirect target for the current halt (mirrors IEmulatorArm64::redirect_pc)
    uint32_t redirect_pc = 0;

private:
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::vector<uint32_t> last_pcs;

    // Current CPSR (mode + T bit). Mirrors what Unicorn would report so the
    // guest sees the same initial state (SVC mode, IRQ/FIQ masked, ARM state).
    uint32_t cpsr_value = 0xD3;
};

#endif
