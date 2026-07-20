#ifndef JNI_BRIDGE_ARM64_H
#define JNI_BRIDGE_ARM64_H

#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

// Magic base for host-native function bridges (ARM64 uses separate address space)
#define BRIDGE_BASE_64 0xFF000000ULL

typedef void (*BridgeHandler64)(void* emu);

struct BridgeFunction64 {
    std::string name;
    uint64_t address;
    BridgeHandler64 handler;
    uint64_t call_count = 0;
};

// --- Frame Statistics (shared with ARM32) ---
// Reuse FrameStats from jni_bridge.h — it's arch-independent
#include "jni_bridge.h"

class JniBridge64 {
public:
    JniBridge64();
    
    uint64_t get_address(const std::string& name);
    std::string get_name(uint64_t address);
    uint64_t lookup_proc_address(const std::string& name);
    
    void register_handler(const std::string& name, BridgeHandler64 handler);
    void call_handler(uint64_t address, void* emu);

    // Initialize standard GLES/AL/Bionic bridges for ARM64
    void init_standard_bridges();

private:
    std::map<std::string, uint64_t> name_to_addr;
    std::map<uint64_t, BridgeFunction64> addr_to_func;
    uint64_t next_addr;
};

// Handle management (shared between ARM32 and ARM64)
extern uint32_t register_pointer(void* ptr);
extern void* get_pointer(uint32_t handle);
extern void release_pointer(void* ptr);

// Draw call batcher — init at GL context ready, flush at frame end
void draw_batcher_init();
void draw_batcher_flush();
void draw_batcher_print_stats();

#endif
