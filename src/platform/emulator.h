#ifndef EMULATOR_H
#define EMULATOR_H

#include <stdint.h>
#include <string>
#include <vector>
#include "loader/elf_loader.h"
#include "jni/jni_bridge.h"

// ARM32 CPU emulator base.
//
// Methods are virtual so the Dynarmic A32 backend (EmulatorDynarmic32) can be
// swapped in as a drop-in replacement while JniBridge handlers (which cast the
// emu pointer back to Emulator*) keep working through virtual dispatch.
class Emulator {
public:
    Emulator(uint8_t* guest_mem, uint32_t mem_size);
    virtual ~Emulator();

    virtual void set_pc(uint32_t pc);
    virtual uint32_t get_pc();
    virtual uint32_t get_lr();
    virtual void set_reg(int reg, uint32_t value);
    virtual uint32_t get_reg(int reg);
    virtual float get_vfp_reg(int reg);

    virtual void run(uint32_t start_pc);
    virtual uint32_t call(uint32_t addr, const std::vector<uint32_t>& args);

    void set_bridge(JniBridge* bridge) { this->bridge = bridge; }
    virtual void handle_bridge_call(uint32_t address);
    virtual uint32_t get_bridge_base();
    virtual uint8_t* get_memory_base() { return memory; }
    virtual void* get_uc_handle() { return uc; }

    // Public for hooks
    JniBridge* bridge;
    virtual void record_pc(uint32_t pc);
    virtual void print_trace();
    bool quiet_mode = false;  // Suppress per-call logging

protected:
    uint8_t* memory;
    uint32_t size;
    void* uc; // uc_engine* (nullptr for non-Unicorn backends)

private:
    std::vector<uint32_t> last_pcs;
};


#endif
