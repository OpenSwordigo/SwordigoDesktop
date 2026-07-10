#include "emulator.h"
#include <iostream>
#include <cstring>
#include <unicorn/unicorn.h>

// Forward declarations from game/camera_override.cpp
extern uint32_t g_cam_ctrl_ptr;
extern void cam_capture_controller(uint32_t this_ptr);

static uint32_t g_instruction_count = 0;

static bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
    std::cerr << "[FAULT] Memory UNKNOWN unmapped at 0x" << std::hex << address << " (size: " << size << ") @instr " << std::dec << g_instruction_count << std::endl;
    uint32_t pc, lr, sp;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    std::cerr << "  PC=0x" << std::hex << pc << " LR=0x" << lr << " SP=0x" << sp << std::dec << std::endl;
    return false;
}

// Lightweight global code hook — only counts instructions in debug mode
static void hook_code_count(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    g_instruction_count++;
}

// Bridge-specific hook — only fires for bridge address range (0xFF000000+)
static void hook_bridge(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    Emulator* emu = (Emulator*)user_data;
    emu->handle_bridge_call((uint32_t)address);
}

// Camera capture hook — only fires at the specific CameraController constructor address
static void hook_camera_ctor(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    if (g_cam_ctrl_ptr == 0) {
        uint32_t this_ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &this_ptr);
        if (this_ptr != 0) {
            cam_capture_controller(this_ptr);
        }
    }
}

uint32_t g_game_scene_controller = 0;
uint32_t g_hero_obj = 0;
uint32_t g_hero_char_ctrl_comp = 0;
uint32_t g_hero_health_comp = 0;
uint32_t g_hero_mana_comp = 0;

float s_vanilla_walk_speed = 0.0f;
float s_vanilla_run_speed = 0.0f;
float s_vanilla_jump_height = 0.0f;

// Hook 1: GameSceneController::Update
static void hook_game_scene_controller_update(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    if (r0 != 0) {
        g_game_scene_controller = r0;
        Emulator* emu = (Emulator*)user_data;
        uint8_t* mem = emu->get_memory_base();
        g_hero_obj = *(uint32_t*)(mem + r0 + 0xa4);
    }
}

// Hook 2: HeroEntityComponent::Update
static void hook_hero_entity_update(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    if (r0 != 0) {
        Emulator* emu = (Emulator*)user_data;
        uint8_t* mem = emu->get_memory_base();
        uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
        uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
        if (owner1 != 0 && owner1 != g_hero_obj) {
            g_hero_obj = owner1;
        } else if (owner2 != 0 && owner2 != g_hero_obj) {
            g_hero_obj = owner2;
        }
    }
}

// Hook 3: CharControllerComponent::Update
static void hook_char_controller_update(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    if (r0 != 0 && g_hero_obj != 0) {
        Emulator* emu = (Emulator*)user_data;
        uint8_t* mem = emu->get_memory_base();
        uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
        uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
        if (owner1 == g_hero_obj || owner2 == g_hero_obj) {
            if (g_hero_char_ctrl_comp != r0) {
                g_hero_char_ctrl_comp = r0;
                s_vanilla_walk_speed = *(float*)(mem + r0 + 0x170);
                s_vanilla_run_speed = *(float*)(mem + r0 + 0x178);
                s_vanilla_jump_height = *(float*)(mem + r0 + 0x164);
            }
        }
    }
}

// Hook 4: HealthComponent::Update
static void hook_health_update(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    if (r0 != 0 && g_hero_obj != 0) {
        Emulator* emu = (Emulator*)user_data;
        uint8_t* mem = emu->get_memory_base();
        uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
        uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
        if (owner1 == g_hero_obj || owner2 == g_hero_obj) {
            g_hero_health_comp = r0;
        }
    }
}

// Hook 5: ManaComponent::Update
static void hook_mana_update(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    if (r0 != 0 && g_hero_obj != 0) {
        Emulator* emu = (Emulator*)user_data;
        uint8_t* mem = emu->get_memory_base();
        uint32_t owner1 = *(uint32_t*)(mem + r0 + 0x14);
        uint32_t owner2 = *(uint32_t*)(mem + r0 + 0x18);
        if (owner1 == g_hero_obj || owner2 == g_hero_obj) {
            g_hero_mana_comp = r0;
        }
    }
}


Emulator::Emulator(uint8_t* guest_mem, uint32_t mem_size) 
    : memory(guest_mem), size(mem_size), bridge(nullptr), uc(nullptr) {
    
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, (uc_engine**)&uc);
    if (err) {
        std::cerr << "Failed on uc_open() with error returned: " << err << std::endl;
        return;
    }

    err = uc_mem_map_ptr((uc_engine*)uc, 0, size, UC_PROT_ALL, memory);
    if (err) {
        std::cerr << "Failed on uc_mem_map_ptr() with error returned: " << err << std::endl;
        return;
    }
    
    err = uc_mem_map((uc_engine*)uc, 0xFF000000, 0x1000000, UC_PROT_ALL);
    if (err) {
        std::cerr << "Failed on uc_mem_map() bridge with error: " << err << std::endl;
    }

    // Fill bridge with 'bx lr' in Thumb (0x4770)
    uint16_t bx_lr = 0x4770;
    for (uint32_t addr = 0xFF000000; addr < 0xFF100000; addr += 2) {
        uc_mem_write((uc_engine*)uc, addr, &bx_lr, 2);
    }

    // Enable VFP
    uint32_t cp15;
    uc_reg_read((uc_engine*)uc, UC_ARM_REG_C1_C0_2, &cp15);
    cp15 |= (0xf << 20);
    uc_reg_write((uc_engine*)uc, UC_ARM_REG_C1_C0_2, &cp15);
    uint32_t fpexc = 0x40000000;
    uc_reg_write((uc_engine*)uc, UC_ARM_REG_FPEXC, &fpexc);

    // Configure FPSCR to match Android's default VFP settings:
    // Bit 24: FZ=1 (Flush denormals to zero — critical! Without this,
    //         near-zero values produce denormals instead of 0, causing
    //         divisions to produce infinity → NaN cascades in matrix math)
    // Bit 25: DN=1 (Default NaN — produce standard 0x7FC00000 for any NaN result)
    uint32_t fpscr = (1 << 24) | (1 << 25);  // FZ | DN
    uc_reg_write((uc_engine*)uc, UC_ARM_REG_FPSCR, &fpscr);

    // Zero-initialize all VFP/NEON registers (D0-D15 covers S0-S31)
    // Unicorn may leave these as garbage/NaN. Game code that reads a
    // VFP register before writing it would produce NaN cascades.
    {
        uint64_t zero = 0;
        int dregs[] = {
            UC_ARM_REG_D0,  UC_ARM_REG_D1,  UC_ARM_REG_D2,  UC_ARM_REG_D3,
            UC_ARM_REG_D4,  UC_ARM_REG_D5,  UC_ARM_REG_D6,  UC_ARM_REG_D7,
            UC_ARM_REG_D8,  UC_ARM_REG_D9,  UC_ARM_REG_D10, UC_ARM_REG_D11,
            UC_ARM_REG_D12, UC_ARM_REG_D13, UC_ARM_REG_D14, UC_ARM_REG_D15,
            UC_ARM_REG_D16, UC_ARM_REG_D17, UC_ARM_REG_D18, UC_ARM_REG_D19,
            UC_ARM_REG_D20, UC_ARM_REG_D21, UC_ARM_REG_D22, UC_ARM_REG_D23,
            UC_ARM_REG_D24, UC_ARM_REG_D25, UC_ARM_REG_D26, UC_ARM_REG_D27,
            UC_ARM_REG_D28, UC_ARM_REG_D29, UC_ARM_REG_D30, UC_ARM_REG_D31,
        };
        for (int reg : dregs) {
            uc_reg_write((uc_engine*)uc, reg, &zero);
        }
    }

    uint32_t stack_base = size - 0x1000; 
    set_reg(13, stack_base); 

    err = uc_mem_map((uc_engine*)uc, 0xE0000000, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
    if (err) {
        std::cerr << "Failed to map magic LR page: " << err << std::endl;
    }

    // Hook 1: Bridge calls — only for bridge address range (HUGE perf win vs global hook)
    uc_hook bridge_hook;
    uc_hook_add((uc_engine*)uc, &bridge_hook, UC_HOOK_CODE, (void*)hook_bridge, this,
                0xFF000000, 0xFF100000);

    // Hook 2: Camera controller capture — only fires at the constructor address
    uc_hook cam_hook;
    uc_hook_add((uc_engine*)uc, &cam_hook, UC_HOOK_CODE, (void*)hook_camera_ctor, this,
                0x002e35c4, 0x002e35c6);

    // Hook 2b: GameSceneController::Update
    uc_hook scene_ctrl_hook;
    uc_hook_add((uc_engine*)uc, &scene_ctrl_hook, UC_HOOK_CODE, (void*)hook_game_scene_controller_update, this,
                0x00278738, 0x0027873a);

    // Hook 2c: HeroEntityComponent::Update
    uc_hook hero_ent_hook;
    uc_hook_add((uc_engine*)uc, &hero_ent_hook, UC_HOOK_CODE, (void*)hook_hero_entity_update, this,
                0x001fdf18, 0x001fdf1a);

    // Hook 2d: CharControllerComponent::Update
    uc_hook char_ctrl_hook;
    uc_hook_add((uc_engine*)uc, &char_ctrl_hook, UC_HOOK_CODE, (void*)hook_char_controller_update, this,
                0x001f285c, 0x001f285e);

    // Hook 2e: HealthComponent::Update
    uc_hook health_hook;
    uc_hook_add((uc_engine*)uc, &health_hook, UC_HOOK_CODE, (void*)hook_health_update, this,
                0x001fcf2c, 0x001fcf2e);

    // Hook 2f: ManaComponent::Update
    uc_hook mana_hook;
    uc_hook_add((uc_engine*)uc, &mana_hook, UC_HOOK_CODE, (void*)hook_mana_update, this,
                0x00201d64, 0x00201d66);

    // Hook 3: Unmapped memory
    uc_hook mem_hook;
    uc_hook_add((uc_engine*)uc, &mem_hook, UC_HOOK_MEM_UNMAPPED, (void*)hook_mem_unmapped, this, 1, 0);

    // Hook 4: NaN matrix tracer — traces inside the ARM-mode function that
    // produces NaN. The BLX at 0x122228c targets an ARM-mode function.
    // We hook a wide range to catch the target function and trace VFP.
    static bool nan_traced = false;
    static uint32_t pc_history[32] = {};
    static int pc_idx = 0;
    static auto nan_check = +[](uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
        if (nan_traced) return;
        
        pc_history[pc_idx % 32] = (uint32_t)address;
        pc_idx++;
        
        // Only check S0 for speed (NaN always appears in S0 first)
        uint32_t s0_raw;
        uc_reg_read(uc, UC_ARM_REG_S0, &s0_raw);
        if (s0_raw != 0x7fc00000) return;
        
        nan_traced = true;
        uint32_t pc, lr, cpsr;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        bool thumb = (cpsr >> 5) & 1;
        
        std::cerr << "[NaN-TRACE] First NaN in S0 at PC=0x" << std::hex << pc
                  << " LR=0x" << lr << " mode=" << (thumb ? "THUMB" : "ARM")
                  << std::dec << std::endl;
        
        // Dump last 32 PCs
        std::cerr << "  PC history (oldest first):" << std::endl << "  ";
        int start = (pc_idx >= 32) ? pc_idx - 32 : 0;
        for (int i = start; i < pc_idx; i++) {
            std::cerr << std::hex << pc_history[i % 32] << " ";
            if ((i - start + 1) % 10 == 0) std::cerr << std::endl << "  ";
        }
        std::cerr << std::dec << std::endl;
        
        // Dump VFP S0-S15
        for (int i = 0; i < 16; i++) {
            uint32_t sv;
            uc_reg_read(uc, UC_ARM_REG_S0 + i, &sv);
            float fv;
            memcpy(&fv, &sv, 4);
            std::cerr << "  S" << i << "=0x" << std::hex << sv << std::dec
                      << " (" << fv << ")" << std::endl;
        }
        
        // Dump the instruction that produced NaN
        Emulator* emu = (Emulator*)user_data;
        uint8_t* memory = emu->get_memory_base();
        uint32_t prev_pc = pc_history[(pc_idx - 2) % 32];
        std::cerr << "  Instruction at prev PC=0x" << std::hex << prev_pc << ": ";
        for (int i = 0; i < 8; i++) {
            std::cerr << std::hex << (int)memory[prev_pc + i] << " ";
        }
        std::cerr << std::dec << std::endl;
        
        // Also dump R0-R3 for context
        for (int r = 0; r < 4; r++) {
            uint32_t rv;
            uc_reg_read(uc, UC_ARM_REG_R0 + r, &rv);
            std::cerr << "  R" << r << "=0x" << std::hex << rv << std::dec << std::endl;
        }
    };
    
    // Hook the Thumb caller region
    uc_hook nan_hook1;
    uc_hook_add((uc_engine*)uc, &nan_hook1, UC_HOOK_CODE, (void*)nan_check,
                this, 0x1222000, 0x1222400);
    
    // Hook a wide ARM-mode target range (covers likely BLX targets)
    uc_hook nan_hook2;
    uc_hook_add((uc_engine*)uc, &nan_hook2, UC_HOOK_CODE, (void*)nan_check,
                this, 0x11A0000, 0x11B0000);
}

Emulator::~Emulator() {
    if (uc) uc_close((uc_engine*)uc);
}

void Emulator::set_pc(uint32_t pc) {
    uc_reg_write((uc_engine*)uc, UC_ARM_REG_PC, &pc);
}

void Emulator::set_reg(int reg, uint32_t value) {
    int uc_reg = UC_ARM_REG_R0 + reg;
    if (reg == 13) uc_reg = UC_ARM_REG_SP;
    if (reg == 14) uc_reg = UC_ARM_REG_LR;
    if (reg == 15) uc_reg = UC_ARM_REG_PC;
    uc_reg_write((uc_engine*)uc, uc_reg, &value);
}

uint32_t Emulator::get_reg(int reg) {
    uint32_t val;
    int uc_reg = UC_ARM_REG_R0 + reg;
    if (reg == 13) uc_reg = UC_ARM_REG_SP;
    if (reg == 14) uc_reg = UC_ARM_REG_LR;
    if (reg == 15) uc_reg = UC_ARM_REG_PC;
    uc_reg_read((uc_engine*)uc, uc_reg, &val);
    return val;
}

float Emulator::get_vfp_reg(int reg) {
    union {
        uint32_t i;
        float f;
    } val;
    int uc_reg = UC_ARM_REG_S0 + reg;
    uc_reg_read((uc_engine*)uc, uc_reg, &val.i);
    return val.f;
}

uint32_t Emulator::get_pc() {
    uint32_t val;
    uc_reg_read((uc_engine*)uc, UC_ARM_REG_PC, &val);
    return val;
}

uint32_t Emulator::get_lr() {
    uint32_t val;
    uc_reg_read((uc_engine*)uc, UC_ARM_REG_LR, &val);
    return val;
}

uint32_t Emulator::get_bridge_base() { return 0xFF000000; }

void Emulator::run(uint32_t start_pc) {
    uint32_t magic_lr = 0xE0000000;
    set_reg(14, magic_lr);
    
    uint32_t pc = start_pc;
    uc_reg_write((uc_engine*)uc, UC_ARM_REG_PC, &pc);
    
    uint32_t cpsr;
    uc_reg_read((uc_engine*)uc, UC_ARM_REG_CPSR, &cpsr);
    if (start_pc & 1) {
        cpsr |= (1 << 5); 
    } else {
        cpsr &= ~(1 << 5);
    }
    uc_reg_write((uc_engine*)uc, UC_ARM_REG_CPSR, &cpsr);

    uc_err err = uc_emu_start((uc_engine*)uc, (uint64_t)pc, (uint64_t)magic_lr, 0, 0);
    if (err) {
        std::cerr << "Unicorn emulation error: " << uc_strerror(err) << std::endl;
        uint32_t curr_pc;
        uc_reg_read((uc_engine*)uc, UC_ARM_REG_PC, &curr_pc);
        std::cerr << "Failed at PC: 0x" << std::hex << curr_pc << std::dec << std::endl;
        print_trace();
    }
    
    if (!quiet_mode) {
        std::cout << "[Emulator] Function executed " << g_instruction_count << " instructions" << std::endl;
    }
    g_instruction_count = 0;
}

uint32_t Emulator::call(uint32_t addr, const std::vector<uint32_t>& args) {
    if (!quiet_mode) {
        std::cout << "[Emulator] Calling guest function at 0x" << std::hex << addr << std::dec << std::endl;
    }
    for (int i = 0; i < args.size() && i < 4; ++i) {
        set_reg(i, args[i]);
    }

    run(addr);
    uint32_t result = get_reg(0);
    if (!quiet_mode) {
        std::cout << "[Emulator] Function returned with R0=0x" << std::hex << result << std::dec << std::endl;
    }
    return result;
}

void Emulator::handle_bridge_call(uint32_t address) {
    if (bridge) {
        uint32_t lr = get_reg(14);
        bridge->call_handler(address, this);
        
        uint32_t cpsr;
        uc_reg_read((uc_engine*)uc, UC_ARM_REG_CPSR, &cpsr);
        if (lr & 1) {
            cpsr |= (1 << 5); // Thumb
        } else {
            cpsr &= ~(1 << 5); // ARM
        }
        uc_reg_write((uc_engine*)uc, UC_ARM_REG_CPSR, &cpsr);

        set_pc(lr);
    }
}

void Emulator::record_pc(uint32_t pc) {
    if (pc >= 0x1000000 && pc < 0x1500000) {
        last_pcs.push_back(pc);
        if (last_pcs.size() > 50) {
            last_pcs.erase(last_pcs.begin());
        }
    }
}

void Emulator::print_trace() {
    std::cerr << "--- PC Execution Trace (Last " << last_pcs.size() << " instructions) ---" << std::endl;
    for (size_t i = 0; i < last_pcs.size(); ++i) {
        std::cerr << "  #" << i << ": 0x" << std::hex << last_pcs[i] << std::dec << std::endl;
    }
    std::cerr << "---------------------------------------------------" << std::endl;
}
