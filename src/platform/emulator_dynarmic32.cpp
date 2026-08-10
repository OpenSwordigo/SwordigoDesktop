// SPDX-FileCopyrightText: Copyright 2026 SwordigoDesktop Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// ARM32 Dynarmic JIT Backend — emulator_dynarmic32.cpp
//
// AArch32 dynamic recompiler backend built on Dynarmic's A32::Jit. It is a
// drop-in replacement for the Unicorn-based ARM32 `Emulator` class, giving
// ARM32 (armeabi-v7a) instances the same near-native JIT speedup that
// EmulatorDynarmic64 provides for ARM64 instances.
//
// Design mirrors emulator_dynarmic64.cpp:
//   * Bridge region (0xFF000000-0xFF100000)  → HaltExecution(UserDefined1),
//     run() dispatches via JniBridge::call_handler.
//   * Magic LR page (0xE0000000)             → HaltExecution(UserDefined2),
//     run() treats it as a clean function return.
//   * ARM32 hero/scene observer hooks fire from MemoryReadCode when the JIT
//     fetches their function-entry addresses, populating the same globals the
//     Unicorn UC_HOOK_CODE hooks wrote.
//   * Real CAS-backed MemoryWriteExclusive* for LDREX/STREX correctness
//     (boost::shared_ptr refcounts).
//
// Thread-safety: single emulator thread, mirroring the Unicorn backend.

#include "emulator_dynarmic32.h"
#include "jni/jni_bridge.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstddef>

extern uint8_t* g_guest_memory;

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/interface/exclusive_monitor.h"

// ============================================================================
// ARM32 observer-hook globals (defined in emulator.cpp — the Unicorn backend).
// We write to them from the JIT code-fetch path so mod tools / camera overrides
// keep working with the same data they got from UC_HOOK_CODE.
// ============================================================================
extern uint32_t g_game_scene_controller;
extern uint32_t g_hero_obj;
extern uint32_t g_hero_char_ctrl_comp;
extern uint32_t g_hero_health_comp;
extern uint32_t g_hero_mana_comp;
extern float s_vanilla_walk_speed;
extern float s_vanilla_run_speed;
extern float s_vanilla_jump_height;
extern uint32_t g_cam_ctrl_ptr;
extern void cam_capture_controller(uint32_t this_ptr);

// ============================================================================
// ARM32 observer hooks — mirror emulator.cpp hook_camera_ctor /
// hook_game_scene_controller_update / hook_hero_entity_update /
// hook_char_controller_update / hook_health_update / hook_mana_update.
//
// These fire from MemoryReadCode when the JIT compiles a fresh block at the
// function-entry address. That happens at spawn, on scene loads, and whenever
// the JIT cache is invalidated — exactly when the captured object pointers can
// change. (Per-execution re-fires are unnecessary: the pointers only change on
// scene/component re-creation.)
// ============================================================================
static bool try_arm32_observer_hooks(EmulatorDynarmic32* emu, uint32_t vaddr) {
    uint8_t* mem = emu->get_memory_base();
    switch (vaddr) {
    case 0x002e35c4: {  // CameraController ctor
        if (g_cam_ctrl_ptr == 0) {
            uint32_t this_ptr = emu->get_reg(0);
            if (this_ptr != 0) cam_capture_controller(this_ptr);
        }
        return true;
    }
    case 0x00278738: {  // GameSceneController::Update
        uint32_t r0 = emu->get_reg(0);
        if (r0 != 0) {
            g_game_scene_controller = r0;
            g_hero_obj = *(uint32_t*)(mem + r0 + 0xa4);
        }
        return true;
    }
    case 0x001fdf18: {  // HeroEntityComponent::Update
        uint32_t r0 = emu->get_reg(0);
        if (r0 != 0) {
            uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
            uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
            if (owner1 != 0 && owner1 != g_hero_obj) g_hero_obj = owner1;
            else if (owner2 != 0 && owner2 != g_hero_obj) g_hero_obj = owner2;
        }
        return true;
    }
    case 0x001f285c: {  // CharControllerComponent::Update
        uint32_t r0 = emu->get_reg(0);
        if (r0 != 0 && g_hero_obj != 0) {
            uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
            uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
            if (owner1 == g_hero_obj || owner2 == g_hero_obj) {
                if (g_hero_char_ctrl_comp != r0) {
                    g_hero_char_ctrl_comp = r0;
                    s_vanilla_walk_speed = *(float*)(mem + r0 + 0x170);
                    s_vanilla_run_speed  = *(float*)(mem + r0 + 0x178);
                    s_vanilla_jump_height = *(float*)(mem + r0 + 0x164);
                }
            }
        }
        return true;
    }
    case 0x001fcf2c: {  // HealthComponent::Update
        uint32_t r0 = emu->get_reg(0);
        if (r0 != 0 && g_hero_obj != 0) {
            uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
            uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
            if (owner1 == g_hero_obj || owner2 == g_hero_obj) g_hero_health_comp = r0;
        }
        return true;
    }
    case 0x00201d64: {  // ManaComponent::Update
        uint32_t r0 = emu->get_reg(0);
        if (r0 != 0 && g_hero_obj != 0) {
            uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
            uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
            if (owner1 == g_hero_obj || owner2 == g_hero_obj) g_hero_mana_comp = r0;
        }
        return true;
    }
    default:
        return false;
    }
}

// ============================================================================
// Memory callbacks for Dynarmic A32
// ============================================================================
class SwordigoMemory32 : public Dynarmic::A32::UserCallbacks {
public:
    SwordigoMemory32(EmulatorDynarmic32* emu, uint8_t* mem, uint32_t size)
        : emu(emu), memory(mem), mem_size(size) {}

    void HandleMemoryFault(Dynarmic::A32::VAddr vaddr, const char* op) {
        // NULL-pointer or tiny reads: return 0 silently so guest C++ NULL
        // checks evaluate to false (mirrors the ARM64 backend).
        if (vaddr < 0x1000u) return;
        static int fault_count = 0;
        if (fault_count++ < 10)
            std::cerr << "[Dynarmic32/Fault] Unmapped " << op << " at 0x"
                      << std::hex << vaddr << std::dec << std::endl;
        emu->get_jit()->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    }

    // --- Memory reads ---
    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override {
        if (vaddr < mem_size) return memory[vaddr];
        HandleMemoryFault(vaddr, "MemoryRead8");
        return 0;
    }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override {
        if (vaddr + 1 < mem_size) {
            std::uint16_t v;
            std::memcpy(&v, memory + vaddr, 2);
            return v;
        }
        HandleMemoryFault(vaddr, "MemoryRead16");
        return 0;
    }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override {
        if (vaddr + 3 < mem_size) {
            std::uint32_t v;
            std::memcpy(&v, memory + vaddr, 4);
            return v;
        }
        HandleMemoryFault(vaddr, "MemoryRead32");
        return 0;
    }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override {
        if (vaddr + 7 < mem_size) {
            std::uint64_t v;
            std::memcpy(&v, memory + vaddr, 8);
            return v;
        }
        HandleMemoryFault(vaddr, "MemoryRead64");
        return 0;
    }

    // --- Memory writes ---
    void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override {
        if (vaddr < mem_size) memory[vaddr] = value;
        else HandleMemoryFault(vaddr, "MemoryWrite8");
    }
    void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override {
        if (vaddr + 1 < mem_size) std::memcpy(memory + vaddr, &value, 2);
        else HandleMemoryFault(vaddr, "MemoryWrite16");
    }
    void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override {
        if (vaddr + 3 < mem_size) std::memcpy(memory + vaddr, &value, 4);
        else HandleMemoryFault(vaddr, "MemoryWrite32");
    }
    void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override {
        if (vaddr + 7 < mem_size) std::memcpy(memory + vaddr, &value, 8);
        else HandleMemoryFault(vaddr, "MemoryWrite64");
    }

    // --- Exclusive writes: real CAS for LDREX/STREX (shared_ptr refcounts) ---
    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr vaddr, std::uint8_t value, std::uint8_t expected) override {
        if (vaddr >= mem_size) { HandleMemoryFault(vaddr, "MemoryWriteExclusive8"); return false; }
        auto* p = reinterpret_cast<std::atomic<std::uint8_t>*>(memory + vaddr);
        return p->compare_exchange_strong(expected, value, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr vaddr, std::uint16_t value, std::uint16_t expected) override {
        if (vaddr + 1 >= mem_size) { HandleMemoryFault(vaddr, "MemoryWriteExclusive16"); return false; }
        auto* p = reinterpret_cast<std::atomic<std::uint16_t>*>(memory + vaddr);
        return p->compare_exchange_strong(expected, value, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr vaddr, std::uint32_t value, std::uint32_t expected) override {
        if (vaddr + 3 >= mem_size) { HandleMemoryFault(vaddr, "MemoryWriteExclusive32"); return false; }
        auto* p = reinterpret_cast<std::atomic<std::uint32_t>*>(memory + vaddr);
        return p->compare_exchange_strong(expected, value, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr vaddr, std::uint64_t value, std::uint64_t expected) override {
        if (vaddr + 7 >= mem_size) { HandleMemoryFault(vaddr, "MemoryWriteExclusive64"); return false; }
        auto* p = reinterpret_cast<std::atomic<std::uint64_t>*>(memory + vaddr);
        return p->compare_exchange_strong(expected, value, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Read-only optimization: keep it simple — everything is treated as
    // writable (RLSwordigo-style mods need writable text segments).
    bool IsReadOnlyMemory(Dynarmic::A32::VAddr /*vaddr*/) override { return false; }

    // --- Code fetch ---
    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override {
        // Bridge region + magic LR page: return a BKPT sentinel (mirrors the
        // A64 backend's HLT approach). BKPT raises ExceptionRaised(Breakpoint)
        // when *executed*, so run() gets a clean halt with PC still inside the
        // bridge/LR page — no NOP fall-through past the region boundary.
        if (vaddr >= 0xFF000000u && vaddr < 0xFF100000u ||
            vaddr >= 0xE0000000u && vaddr < 0xE0001000u) {
            // Thumb-16 BKPT #0 = 0xBE00; ARM BKPT #0 = 0xE1200070.
            // (Thumb: 0xBE00BE00 so BOTH halfwords decode as BKPT #0 —
            //  ReadThumbInstruction picks the low/high halfword by alignment.)
            const bool thumb = (emu->get_jit()->Cpsr() & (1u << 5)) != 0;
            return thumb ? 0xBE00BE00u : 0xE1200070u;
        }

        // ARM32 observer hooks (function-entry addresses, mirror Unicorn hooks).
        try_arm32_observer_hooks(emu, vaddr);

        // Regular memory
        if (vaddr + 3 < mem_size) {
            std::uint32_t v;
            std::memcpy(&v, memory + vaddr, 4);
            return v;
        }
        return std::nullopt;  // Unmapped — triggers NoExecuteFault
    }

    // --- Interpreter fallback (JIT couldn't translate an instruction) ---
    void InterpreterFallback(Dynarmic::A32::VAddr pc, size_t num_instructions) override {
        if (pc >= 0xE0000000u && pc < 0xE0001000u) {
            emu->function_returned = true;
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (pc >= 0xFF000000u && pc < 0xFF100000u) {
            emu->bridge_halt_requested = true;
            emu->bridge_halt_address = pc;
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        static int fallback_log_count = 0;
        if (fallback_log_count++ < 50) {
            std::cerr << "[Dynarmic32] InterpreterFallback at 0x" << std::hex << pc
                      << " for " << std::dec << num_instructions << " instr — skipping"
                      << std::endl;
        }
        // Skip past (ARM = 4 bytes, Thumb = 2 bytes per instruction).
        const uint32_t step = (emu->get_jit()->Cpsr() & (1u << 5)) ? 2u : 4u;
        emu->get_jit()->Regs()[15] += step * (uint32_t)num_instructions;
    }

    void CallSVC(std::uint32_t swi) override {
        static int svc_log_count = 0;
        if (svc_log_count++ < 10) {
            std::cerr << "[Dynarmic32] SVC #" << swi << " at PC=0x" << std::hex
                      << emu->get_jit()->Regs()[15] << std::dec << " — skipping"
                      << std::endl;
        }
    }

    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override {
        using Exception = Dynarmic::A32::Exception;
        // BKPT sentinel: bridge call (UserDefined1) or clean function return
        // (UserDefined2) — mirrors the A64 backend's HLT dispatch.
        if (exception == Exception::Breakpoint) {
            if (pc >= 0xFF000000u && pc < 0xFF100000u) {
                emu->bridge_halt_requested = true;
                emu->bridge_halt_address = pc;
                emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            } else if (pc >= 0xE0000000u && pc < 0xE0001000u) {
                emu->function_returned = true;
                emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            } else {
                // Unknown breakpoint — log once, skip past (BKPT = 4 bytes ARM / 2 Thumb).
                static int unknown_bkpt = 0;
                if (unknown_bkpt++ < 5) {
                    std::cerr << "[Dynarmic32] Unknown BKPT at 0x" << std::hex << pc << std::dec << std::endl;
                }
                const bool thumb = (emu->get_jit()->Cpsr() & (1u << 5)) != 0;
                emu->get_jit()->Regs()[15] += thumb ? 2 : 4;
            }
            return;
        }
        // Hint / sync instructions: let the JIT continue past them.
        if (exception == Exception::WaitForInterrupt ||
            exception == Exception::WaitForEvent ||
            exception == Exception::SendEvent ||
            exception == Exception::SendEventLocal ||
            exception == Exception::Yield) {
            return;
        }
        if (exception == Exception::NoExecuteFault) {
            std::cerr << "[Dynarmic32] NoExecuteFault at 0x" << std::hex << pc << std::dec << std::endl;
            std::cerr << "=== REGISTERS ===" << std::endl;
            for (int r = 0; r < 16; r++) {
                std::cerr << "  R" << r << " = 0x" << std::hex << emu->get_reg(r) << std::dec << std::endl;
            }
            emu->get_jit()->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
            return;
        }
        std::cerr << "[Dynarmic32] Exception at 0x" << std::hex << pc
                  << " type=" << std::dec << static_cast<int>(exception) << " — halting" << std::endl;
        emu->get_jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    // --- Tick management (instruction budget) ---
    void AddTicks(std::uint64_t ticks) override {
        ticks_elapsed += ticks;
    }
    std::uint64_t GetTicksRemaining() override {
        if (ticks_elapsed >= tick_budget) return 0;
        return tick_budget - ticks_elapsed;
    }
    void ResetTicks(uint64_t budget) {
        ticks_elapsed = 0;
        tick_budget = budget;
    }

private:
    EmulatorDynarmic32* emu;
    uint8_t* memory;
    uint32_t mem_size;
    uint64_t ticks_elapsed = 0;
    uint64_t tick_budget = 50000000ULL;
};

static SwordigoMemory32* g_dyn_memory32 = nullptr;

// ============================================================================
// EmulatorDynarmic32 Implementation
// ============================================================================
EmulatorDynarmic32::EmulatorDynarmic32(uint8_t* guest_mem, uint32_t mem_size)
    : Emulator(guest_mem, mem_size) {
    // Note: the base ctor may have opened a (never-started) Unicorn handle for
    // bookkeeping; it is intentionally left alone — the base dtor closes it.

    g_dyn_memory32 = new SwordigoMemory32(this, guest_mem, mem_size);

    Dynarmic::A32::UserConfig config;
    config.callbacks = g_dyn_memory32;

    // Dispatch-level optimizations only (BlockLinking / ReturnStackBuffer /
    // FastDispatch). NOTE: this dynarmic snapshot's A32 frontend hits an IR
    // identity-cycle (infinite recursion in Value::IsImmediate) on some Thumb
    // literal-pool code when the IR optimization passes run — GetSetElimination,
    // ConstProp and MiscIROpt call Inst::ReplaceUsesWith(), which can weave two
    // instructions into an Identity self-cycle. Skipping the IR passes costs
    // little (dispatch speed is unchanged) and fully avoids the hang.
    config.optimizations = Dynarmic::OptimizationFlag::BlockLinking
                         | Dynarmic::OptimizationFlag::ReturnStackBuffer
                         | Dynarmic::OptimizationFlag::FastDispatch;

    // Fastmem: the guest buffer covers the low 2GB; 32-bit guest addresses map
    // directly, so point fastmem at the buffer base.
    config.fastmem_pointer = (uintptr_t)guest_mem;

    // Code cache: 256MB (x64 hosts allow up to 2GiB).
    config.code_cache_size = 256 * 1024 * 1024;

    // Wall clock for the counter (we don't need cycle-accurate timing).
    config.wall_clock_cntpct = true;

    // ExclusiveMonitor: required for LDREX/STREX.
    static Dynarmic::ExclusiveMonitor exclusive_monitor(1);
    config.global_monitor = &exclusive_monitor;
    config.processor_id = 0;

    jit = std::make_unique<Dynarmic::A32::Jit>(config);

    // Match the Unicorn backend's initial CPU state:
    //   CPSR = 0xD3 (SVC mode, IRQ/FIQ masked, ARM state)
    //   FPSCR = FZ | DN (flush denormals, default NaN) — critical for matrix math
    //   All VFP/NEON regs zeroed (avoids NaN cascades from garbage)
    //   SP near top of guest memory
    cpsr_value = 0xD3;
    jit->SetCpsr(cpsr_value);
    jit->SetFpscr((1u << 24) | (1u << 25));
    for (auto& ext : jit->ExtRegs()) ext = 0;
    set_reg(13, size - 0x1000);

    std::cout << "[Dynarmic32] JIT initialized (256MB code cache, Fastmem, unsafe FP fast-paths enabled)"
              << ", stack at 0x" << std::hex << (size - 0x1000) << std::dec << std::endl;
}

EmulatorDynarmic32::~EmulatorDynarmic32() {
    jit.reset();
    delete g_dyn_memory32;
    g_dyn_memory32 = nullptr;
}

// --- Register access ---

void EmulatorDynarmic32::set_pc(uint32_t pc) {
    auto& regs = jit->Regs();
    regs[15] = pc & ~1u;
    if (pc & 1) cpsr_value |= (1u << 5);   // Thumb
    else        cpsr_value &= ~(1u << 5);  // ARM
    jit->SetCpsr(cpsr_value);
}

uint32_t EmulatorDynarmic32::get_pc() { return jit->Regs()[15] & ~1u; }
uint32_t EmulatorDynarmic32::get_lr() { return jit->Regs()[14]; }

void EmulatorDynarmic32::set_reg(int reg, uint32_t value) {
    if (reg == 15) { set_pc(value); return; }
    if (reg >= 0 && reg < 15) jit->Regs()[reg] = value;
}

uint32_t EmulatorDynarmic32::get_reg(int reg) {
    if (reg >= 0 && reg < 16) return jit->Regs()[reg];
    return 0;
}

float EmulatorDynarmic32::get_vfp_reg(int reg) {
    if (reg < 0 || reg >= 32) return 0.0f;
    float f;
    std::memcpy(&f, &jit->ExtRegs()[reg], sizeof(float));  // S0-S31 = ExtRegs[0..31]
    return f;
}

// --- Execution ---

void EmulatorDynarmic32::run(uint32_t start_pc) {
    static const uint32_t MAGIC_LR = 0xE0000000;
    static const uint64_t TICK_BUDGET = 50000000ULL;
    static const int MAX_CHUNKS = 50000;

    // Guard: reject calls into string/data area — likely a corrupted function
    // pointer was passed to call(). Mirrors the ARM64 backend.
    if (start_pc >= 0x10000 && start_pc < 0x80000) {
        static int bad_pc_count = 0;
        if (bad_pc_count < 10) {
            std::cerr << "[Dynarmic32/BAD-PC] call() to 0x" << std::hex << start_pc
                      << " — likely a corrupted function pointer!" << std::dec << std::endl;
        }
        function_returned = true;
        return;
    }

    // Set LR to the magic sentinel (function return detection) and enter.
    jit->Regs()[14] = MAGIC_LR;
    set_pc(start_pc);

    uint64_t entry_sp = jit->Regs()[13];

    auto t0 = std::chrono::steady_clock::now();
    auto wall_start = t0;
    auto last_keepalive = t0;

    // Wall-clock safety limit per function invocation (3 minutes; scene-load
    // functions get 10 minutes).
    const int wall_limit_ms = (start_pc == 0x1478ccc || start_pc == 0x1478f84) ? 600000 : 180000;

    uint32_t curr_pc = start_pc;
    int chunk = 0;
    int bridge_calls = 0;

    for (chunk = 0; chunk < MAX_CHUNKS; chunk++) {
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - wall_start).count();
        if (elapsed_ms > wall_limit_ms) {
            std::cerr << "[Dynarmic32] Wall clock limit (" << wall_limit_ms
                      << "ms) hit for 0x" << std::hex << start_pc << std::dec
                      << " — stopping execution" << std::endl;
            break;
        }

        // Window-manager keep-alive so X11/Wayland never sees us hang.
        int keepalive_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keepalive).count();
        if (keepalive_ms >= 15) {
            last_keepalive = now;
            SDL_PumpEvents();
        }

        g_dyn_memory32->ResetTicks(TICK_BUDGET);
        jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        jit->ClearHalt(Dynarmic::HaltReason::UserDefined2);
        bridge_halt_requested = false;
        function_returned = false;

        Dynarmic::HaltReason hr = jit->Run();
        curr_pc = get_pc();

        // Bridge call (UserDefined1)
        if (bridge_halt_requested && Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1)) {
            bridge_calls++;
            handle_bridge_call(bridge_halt_address);
            bridge_halt_requested = false;
            curr_pc = get_pc();
            if (curr_pc == MAGIC_LR) break;
            chunk--;  // bridge calls don't count as chunks
            continue;
        }

        // Clean function return (UserDefined2 + function_returned flag)
        if (function_returned && Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
            break;
        }

        // UserDefined2 without a clean return — force return to LR
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
            uint32_t lr = get_reg(14);
            std::cerr << "[Dynarmic32] Halt (UserDefined2) at 0x" << std::hex << curr_pc
                      << " — force returning to LR=0x" << lr << std::dec << std::endl;
            if (lr >= 0x1000000u && lr < 0x3000000u) {
                set_pc(lr);
                set_reg(0, 0);
                continue;
            }
            break;
        }

        // MemoryAbort (NoExecuteFault / unmapped memory)
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::MemoryAbort)) {
            std::cerr << "[Dynarmic32] MemoryAbort halt at PC=0x" << std::hex << curr_pc
                      << std::dec << " — stopping execution" << std::endl;
            break;
        }

        // Function completed?
        if (curr_pc == MAGIC_LR) break;
    }

    if (chunk >= MAX_CHUNKS) {
        std::cerr << "[Dynarmic32] MAX_CHUNKS (" << MAX_CHUNKS << ") exhausted for 0x"
                  << std::hex << start_pc << std::dec
                  << " (bridge_calls=" << bridge_calls << ") — stopping execution" << std::endl;
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms > 50.0) {
        static int slow_count = 0;
        if (slow_count++ < 30) {
            std::cerr << "[PERF/Dynarmic32] SLOW call at 0x" << std::hex << start_pc
                      << std::dec << " took " << (int)ms << "ms (bridge_calls=" << bridge_calls << ")" << std::endl;
        }
    }
}

uint32_t EmulatorDynarmic32::call(uint32_t addr, const std::vector<uint32_t>& args) {
    // Save the previous CPU state so nested/recursive guest calls restore it.
    const std::array<uint32_t, 16> saved_regs = jit->Regs();
    const std::array<uint32_t, 64> saved_ext  = jit->ExtRegs();
    const uint32_t saved_cpsr  = jit->Cpsr();
    const uint32_t saved_fpscr = jit->Fpscr();
    const bool saved_bh = bridge_halt_requested;
    const uint32_t saved_bh_addr = bridge_halt_address;
    const bool saved_fr = function_returned;
    const uint32_t saved_redirect = redirect_pc;

    // AAPCS: first 4 integer args in R0-R3.
    for (size_t i = 0; i < args.size() && i < 4; ++i) {
        set_reg((int)i, args[i]);
    }

    run(addr);
    uint32_t result = get_reg(0);  // Read return value BEFORE restoring state

    jit->Regs()  = saved_regs;
    jit->ExtRegs() = saved_ext;
    jit->SetCpsr(saved_cpsr);
    jit->SetFpscr(saved_fpscr);
    bridge_halt_requested = saved_bh;
    bridge_halt_address = saved_bh_addr;
    function_returned = saved_fr;
    redirect_pc = saved_redirect;

    return result;
}

// --- Bridge handling ---

void EmulatorDynarmic32::handle_bridge_call(uint32_t address) {
    if (bridge) {
        uint32_t lr = get_reg(14);
        bridge->call_handler(address, (void*)this);

        if (redirect_pc != 0) {
            uint32_t rp = redirect_pc;
            redirect_pc = 0;
            set_pc(rp);
        } else {
            set_pc(lr);  // set_pc() restores the Thumb bit from LR bit 0
        }
    }
}

// --- Debugging ---

void EmulatorDynarmic32::record_pc(uint32_t pc) {
    if (pc >= 0x1000000 && pc < 0x2000000) {
        last_pcs.push_back(pc);
        if (last_pcs.size() > 50) {
            last_pcs.erase(last_pcs.begin());
        }
    }
}

void EmulatorDynarmic32::print_trace() {
    std::cerr << "--- Dynarmic32 PC Trace (Last " << last_pcs.size() << " entries) ---" << std::endl;
    for (size_t i = 0; i < last_pcs.size(); ++i) {
        std::cerr << "  #" << i << ": 0x" << std::hex << last_pcs[i] << std::dec << std::endl;
    }
    std::cerr << "--------------------------------------------" << std::endl;
}
