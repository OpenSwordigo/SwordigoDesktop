// ARM64 port of jni_bridge.cpp — auto-generated
// Uses IEmulatorArm64 interface with AAPCS64 calling convention:
//   - Integer args in X0-X7, float args in S0-S7, double args in D0-D7
//   - Float/int register numbering is INDEPENDENT
//   - No __aeabi_* helpers (ARM64 has native div instructions)
//   - Backend-agnostic: works with Unicorn or Dynarmic

// Must be defined before any GL headers are pulled in (even transitively)
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif

#include "jni_bridge.h"
#include "jni_bridge_arm64.h"

extern "C" uint64_t sre_resolve_address(const char* symbol);
extern "C" const char* sre_resolve_symbol(uint64_t addr);

#include "jni/gl_render_state.h"  // GL state handshake — bridge writes, FBO reads
#include "platform/i_emulator_arm64.h"
#include "platform/emulator_arm64.h"  // For backward compat — EmulatorArm64 inherits IEmulatorArm64
#include "platform/io_thread.h"
#include "platform/data_path.h"
#include "android/asset_manager.h"
#include "platform/video_background.h"
#include "platform/pvrtc_decoder.h"
#include "platform/pvr_loader.h"
#include "platform/gl_inc.h"
#include <GL/glext.h>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <unordered_map>
#include <deque>
#include <unordered_set>
#include <map>
#include <sstream>
#include <mutex>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <sys/time.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <strings.h>
#include "platform/draw_batcher.h"
#include <filesystem>
namespace fs = std::filesystem;
#include <vorbis/vorbisfile.h>
#include <mpg123.h>
#include <algorithm>
#include <time.h>
#include <unistd.h>
#include <thread>
#include "platform/rgc.h"
#include "platform/fbo_scaler.h"
#include "loader/elf_loader_arm64.h"

extern so_module_arm64 g_sre_mod;
extern so_module_arm64 g_main_mod_64;
extern uint8_t* g_guest_memory;
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include "platform/gl_inc.h"
#include <GL/glext.h>
#include <AL/al.h>
#include <AL/alc.h>

#ifdef VULKAN_BACKEND
#include "platform/vulkan_backend.h"
extern GraphicsAPI g_graphics_api;
extern VulkanBackend g_vk_backend;
#endif

// These globals are defined in jni_bridge.cpp — use extern here
extern bool g_display_active;
extern std::string g_save_dir;
extern bool g_text_input_active;
extern std::string g_text_input_buffer;

// ============================================================================
// ARM64 va_list helper
// AArch64 va_list is a 32-byte struct (NOT a flat pointer like ARM32):
//   offset 0:  __stack    (8 bytes) — pointer to stack overflow area
//   offset 8:  __gr_top   (8 bytes) — top of GP register save area
//   offset 16: __vr_top   (8 bytes) — top of FP/SIMD register save area
//   offset 24: __gr_offs  (4 bytes) — offset from __gr_top (negative = regs remain)
//   offset 28: __vr_offs  (4 bytes) — offset from __vr_top (negative = regs remain)
// ============================================================================
struct Arm64VaList {
    uint8_t* memory;
    uint64_t stack;
    uint64_t gr_top;
    uint64_t vr_top;
    int32_t gr_offs;
    int32_t vr_offs;

    Arm64VaList(uint8_t* mem, uint32_t va_list_ptr) : memory(mem) {
        stack   = *(uint64_t*)(mem + va_list_ptr);
        gr_top  = *(uint64_t*)(mem + va_list_ptr + 8);
        vr_top  = *(uint64_t*)(mem + va_list_ptr + 16);
        gr_offs = *(int32_t*)(mem + va_list_ptr + 24);
        vr_offs = *(int32_t*)(mem + va_list_ptr + 28);
    }

    // Read next general-purpose (integer/pointer) argument
    uint64_t next_gp() {
        uint64_t val;
        if (gr_offs < 0) {
            val = *(uint64_t*)(memory + (uint32_t)(gr_top + gr_offs));
            gr_offs += 8;
        } else {
            val = *(uint64_t*)(memory + (uint32_t)stack);
            stack += 8;
        }
        return val;
    }

    // Read next floating-point argument
    double next_fp() {
        double val;
        if (vr_offs < 0) {
            // FP regs in save area are 16 bytes each (SIMD reg size)
            val = *(double*)(memory + (uint32_t)(vr_top + vr_offs));
            vr_offs += 16;
        } else {
            val = *(double*)(memory + (uint32_t)stack);
            stack += 8;
        }
        return val;
    }
};

// ============================================================================
// JniBridge64 Class Methods
// ============================================================================
JniBridge64::JniBridge64() : next_addr(BRIDGE_BASE_64) {}

uint64_t JniBridge64::get_address(const std::string& name) {
    if (name_to_addr.count(name)) return name_to_addr[name];
    
    uint64_t addr = next_addr;
    next_addr += 8;  // 8-byte aligned for ARM64
    
    name_to_addr[name] = addr;
    addr_to_func[addr] = {name, addr, nullptr};
    
    return addr;
}

std::string JniBridge64::get_name(uint64_t address) {
    if (addr_to_func.count(address)) return addr_to_func[address].name;
    return "UnknownBridge64";
}

uint64_t JniBridge64::lookup_proc_address(const std::string& name) {
    auto it = name_to_addr.find(name);
    if (it == name_to_addr.end()) return 0;
    uint64_t addr = it->second;
    auto fit = addr_to_func.find(addr);
    if (fit == addr_to_func.end() || !fit->second.handler) return 0;
    return addr;
}

void JniBridge64::register_handler(const std::string& name, BridgeHandler64 handler) {
    uint64_t addr = get_address(name);
    addr_to_func[addr].handler = handler;
}

void JniBridge64::call_handler(uint64_t address, void* emu_ptr) {
    if (addr_to_func.count(address)) {
        BridgeFunction64& func = addr_to_func[address];
        if (func.handler) {
            func.handler(emu_ptr);
        } else {
            // ALWAYS log unhandled calls — critical for debugging crashes
            std::cerr << "[Bridge64] !! UNHANDLED: " << func.name << std::endl;
            // Return 0 instead of leaving garbage in X0
            IEmulatorArm64* emu2 = (IEmulatorArm64*)emu_ptr;
            emu2->set_reg(0, 0);
        }
    } else {
        std::cerr << "[Bridge64] !! Unknown address: 0x" << std::hex << address << std::dec << std::endl;
        IEmulatorArm64* emu2 = (IEmulatorArm64*)emu_ptr;
        emu2->set_reg(0, 0);
    }
}
// These are defined in jni_bridge.cpp — shared between ARM32 and ARM64
extern int g_death_detected_countdown;
extern FrameStats g_frame_stats;
extern std::vector<uint8_t> g_snapshot_data;

// Handle management is defined in jni_bridge.cpp but used by bridge functions here
extern std::unordered_map<uint32_t, void*> g_handle_to_ptr;
extern std::unordered_map<void*, uint32_t> g_ptr_to_handle;
extern uint32_t g_next_handle;
extern std::mutex g_handles_mutex;

// Handle management is shared (defined in jni_bridge.cpp)
// register_pointer, get_pointer, release_pointer — declared in jni_bridge.h

// Soft-float helpers (not needed for ARM64 but some bridge code may reference them)
static float uint_to_float(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

static uint32_t float_to_uint(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}


// --- Memory Allocator Bridges ---
struct DeferredBlock {
    uint32_t addr;
    uint32_t size;
    uint64_t alloc_index;
};
static uint32_t g_guest_heap_ptr = 0x20000000; // Start heap at 512MB
static std::unordered_map<uint32_t, uint32_t> g_guest_allocs;
static std::unordered_set<uint32_t> g_tracked_allocs;
static std::vector<DeferredBlock> g_deferred_free_list_vector; // Original TVPG container
static std::deque<DeferredBlock> g_deferred_free_list;
static uint64_t g_alloc_counter = 0;
static std::mutex g_heap_mutex;

// Fastbins: O(1) allocator for small blocks (size <= 1024, in multiples of 8 bytes)
// 128 buckets: index = (size - 1) / 8. Free blocks form a singly linked list in guest memory.
static uint32_t g_fastbins[128] = {0};

// Dual-index free list: O(log n) best-fit with boundary coalescing (for size > 1024).
// g_free_by_addr: addr -> size (for O(log n) coalescing via neighbour lookup)
// g_free_by_size: size -> addr (multimap for O(log n) best-fit search)
static std::map<uint32_t, uint32_t>       g_free_by_addr; // addr → size
static std::multimap<uint32_t, uint32_t>  g_free_by_size; // size → addr

static void add_free_block_locked(uint32_t addr, uint32_t size) {
    if (size == 0) return;

    // Fast path: size <= 1024 goes to fastbins in O(1) without coalescing
    if (size <= 1024) {
        uint32_t idx = (size - 1) >> 3; // (size - 1) / 8
        if (idx < 128) {
            *(uint32_t*)(g_guest_memory + addr) = g_fastbins[idx];
            g_fastbins[idx] = addr;
            return;
        }
    }

    uint32_t new_addr = addr;
    uint32_t new_size = size;

    // Coalesce with the immediately following block (addr + size == next.addr)
    auto next_it = g_free_by_addr.find(new_addr + new_size);
    if (next_it != g_free_by_addr.end()) {
        // Remove from size index
        auto sr = g_free_by_size.equal_range(next_it->second);
        for (auto it = sr.first; it != sr.second; ++it) {
            if (it->second == next_it->first) { g_free_by_size.erase(it); break; }
        }
        new_size += next_it->second;
        g_free_by_addr.erase(next_it);
    }

    // Coalesce with the immediately preceding block (prev.addr + prev.size == addr)
    auto after_it = g_free_by_addr.upper_bound(new_addr);
    if (after_it != g_free_by_addr.begin()) {
        auto prev_it = std::prev(after_it);
        if (prev_it->first + prev_it->second == new_addr) {
            // Remove from size index
            auto sr = g_free_by_size.equal_range(prev_it->second);
            for (auto it = sr.first; it != sr.second; ++it) {
                if (it->second == prev_it->first) { g_free_by_size.erase(it); break; }
            }
            prev_it->second += new_size; // Expand in addr map
            g_free_by_size.emplace(prev_it->second, prev_it->first); // Re-insert size index
            return;
        }
    }

    // No coalescing — insert fresh entry in both indexes
    g_free_by_addr[new_addr] = new_size;
    g_free_by_size.emplace(new_size, new_addr);
}

static uint32_t find_free_block_locked(uint32_t size, uint32_t& found_size) {
    // Fast path: try to pop from fastbin in O(1)
    if (size <= 1024) {
        uint32_t idx = (size - 1) >> 3; // (size - 1) / 8
        if (idx < 128 && g_fastbins[idx] != 0) {
            uint32_t addr = g_fastbins[idx];
            g_fastbins[idx] = *(uint32_t*)(g_guest_memory + addr);
            found_size = (idx + 1) << 3; // Aligned size
            return addr;
        }
    }

    // lower_bound gives us the smallest block >= requested size in O(log n)
    auto it = g_free_by_size.lower_bound(size);
    if (it == g_free_by_size.end()) return 0;

    uint32_t addr      = it->second;
    uint32_t blk_size  = it->first;

    // Remove from both indexes
    g_free_by_size.erase(it);
    g_free_by_addr.erase(addr);

    found_size = blk_size;
    return addr;
}



extern bool g_advanced_redstell_opts;
extern uint64_t g_sre_profile_addr;
extern uint64_t g_sre_vfs_profile_addr;

static uint32_t host_malloc_locked(uint32_t size) {
    size = (size + 7) & ~7;
    if (size == 0) size = 8;
    
    // === TVPG Original Config Allocator ===
    if (!g_advanced_redstell_opts) {
        // Drain deferred list (oldest-first, stop at first not-yet-safe block)
        while (!g_deferred_free_list_vector.empty()) {
            const auto& db = g_deferred_free_list_vector.front();
            if (g_alloc_counter - db.alloc_index > 5000) {
                add_free_block_locked(db.addr, db.size);
                g_deferred_free_list_vector.erase(g_deferred_free_list_vector.begin());
            } else {
                break;
            }
        }
        uint32_t found_size = 0;
        uint32_t addr = find_free_block_locked(size, found_size);
        if (addr != 0) {
            if (found_size >= size + 16) {
                add_free_block_locked(addr + size, found_size - size);
                found_size = size;
            }
            g_guest_allocs[addr] = found_size;
            g_alloc_counter++;
            if (addr >= 0x10000) memset(g_guest_memory + addr, 0, found_size);
            return addr;
        }
        addr = g_guest_heap_ptr;
        g_guest_heap_ptr += size;
        g_guest_allocs[addr] = size;
        g_alloc_counter++;
        if (addr >= 0x10000) memset(g_guest_memory + addr, 0, size);
        return addr;
    }

    // === SRE Config Allocator — drain at 500-alloc safety margin ===
    while (!g_deferred_free_list.empty()) {
        const auto& db = g_deferred_free_list.front();
        if (g_alloc_counter - db.alloc_index > 500) {
            add_free_block_locked(db.addr, db.size);
            g_deferred_free_list.pop_front();
        } else {
            break;
        }
    }
    
    uint32_t found_size = 0;
    uint32_t addr = find_free_block_locked(size, found_size);
    if (addr != 0) {
        if (found_size >= size + 16) {
            uint32_t remainder_addr = addr + size;
            uint32_t remainder_size = found_size - size;
            add_free_block_locked(remainder_addr, remainder_size);
            found_size = size;
        }
        g_guest_allocs[addr] = found_size;
        g_alloc_counter++;
        if (addr >= 0x10000) memset(g_guest_memory + addr, 0, found_size);
        return addr;
    }
    
    addr = g_guest_heap_ptr;
    g_guest_heap_ptr += size;
    g_guest_allocs[addr] = size;
    g_alloc_counter++;
    if (addr >= 0x10000) memset(g_guest_memory + addr, 0, size);
    return addr;
}


static void host_free_locked(uint32_t ptr) {
    if (ptr == 0) return;
    if (g_guest_allocs.count(ptr)) {
        uint32_t size = g_guest_allocs[ptr];
        DeferredBlock db;
        db.addr = ptr;
        db.size = (size + 7) & ~7;
        db.alloc_index = g_alloc_counter;
        
        // === TVPG Original Config Free ===
        if (!g_advanced_redstell_opts) {
            g_deferred_free_list_vector.push_back(db);
            g_guest_allocs.erase(ptr);
            return;
        }

        // === SRE Config Free ===
        g_deferred_free_list.push_back(db);
        g_guest_allocs.erase(ptr);
    }
}

extern "C" void rgc_reclaim_guest_memory(uint64_t addr) {
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    host_free_locked((uint32_t)addr);
}

extern "C" uint32_t get_guest_heap_size_64() {
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    return g_guest_heap_ptr - 0x20000000;
}

static void bridge_malloc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t size = emu->get_reg(0);
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    uint32_t addr = host_malloc_locked(size);

    uint64_t lr = emu->get_reg(30);
    bool should_track = false;
    if (g_sre_mod.base_addr != 0 && lr >= g_sre_mod.base_addr && lr < g_sre_mod.base_addr + g_sre_mod.mem_size) {
        should_track = true;
    } else if (g_main_mod_64.base_addr != 0 && lr >= g_main_mod_64.base_addr && lr < g_main_mod_64.base_addr + g_main_mod_64.mem_size) {
        should_track = true;
    }

    if (should_track) {
        RedstellGC::instance().track_alloc(addr, size, lr, "malloc");
        g_tracked_allocs.insert(addr);
    }

    emu->set_reg(0, addr);
}

static void bridge_calloc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t num = emu->get_reg(0);
    uint32_t size = emu->get_reg(1);
    uint32_t total = num * size;
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    uint32_t addr = host_malloc_locked(total);
    std::memset(emu->get_memory_base() + addr, 0, total);

    uint64_t lr = emu->get_reg(30);
    bool should_track = false;
    if (g_sre_mod.base_addr != 0 && lr >= g_sre_mod.base_addr && lr < g_sre_mod.base_addr + g_sre_mod.mem_size) {
        should_track = true;
    } else if (g_main_mod_64.base_addr != 0 && lr >= g_main_mod_64.base_addr && lr < g_main_mod_64.base_addr + g_main_mod_64.mem_size) {
        should_track = true;
    }

    if (should_track) {
        RedstellGC::instance().track_alloc(addr, total, lr, "calloc");
        g_tracked_allocs.insert(addr);
    }

    emu->set_reg(0, addr);
}

static void bridge_realloc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t ptr = emu->get_reg(0);
    uint32_t size = emu->get_reg(1);
    std::lock_guard<std::mutex> lock(g_heap_mutex);

    // --- TVPG Vanilla Realloc ---
    if (!g_advanced_redstell_opts) {
        uint64_t lr = emu->get_reg(30);
        bool should_track = false;
        if (g_sre_mod.base_addr != 0 && lr >= g_sre_mod.base_addr && lr < g_sre_mod.base_addr + g_sre_mod.mem_size) {
            should_track = true;
        } else if (g_main_mod_64.base_addr != 0 && lr >= g_main_mod_64.base_addr && lr < g_main_mod_64.base_addr + g_main_mod_64.mem_size) {
            should_track = true;
        }
        bool was_tracked = (ptr != 0 && g_tracked_allocs.count(ptr) > 0);

        if (ptr == 0) {
            uint32_t addr = host_malloc_locked(size);
            if (should_track) {
                RedstellGC::instance().track_alloc(addr, size, lr, "realloc(new)");
                g_tracked_allocs.insert(addr);
            }
            emu->set_reg(0, addr);
            return;
        }
        if (size == 0) {
            if (was_tracked) {
                RedstellGC::instance().track_free(ptr, lr);
                g_tracked_allocs.erase(ptr);
            }
            host_free_locked(ptr);
            emu->set_reg(0, 0);
            return;
        }
        
        if (g_guest_allocs.count(ptr)) {
            uint32_t old_size = g_guest_allocs[ptr];
            uint32_t addr = host_malloc_locked(size);
            uint32_t copy_size = (old_size < size) ? old_size : size;
            if (copy_size > 0) {
                std::memcpy(emu->get_memory_base() + addr, emu->get_memory_base() + ptr, copy_size);
            }
            if (was_tracked) {
                RedstellGC::instance().track_free(ptr, lr);
                g_tracked_allocs.erase(ptr);
            }
            host_free_locked(ptr);
            if (should_track) {
                RedstellGC::instance().track_alloc(addr, size, lr, "realloc(move)");
                g_tracked_allocs.insert(addr);
            }
            emu->set_reg(0, addr);
        } else {
            uint32_t addr = host_malloc_locked(size);
            if (size > 0) {
                std::memcpy(emu->get_memory_base() + addr, emu->get_memory_base() + ptr, size);
            }
            if (should_track) {
                RedstellGC::instance().track_alloc(addr, size, lr, "realloc(fallback)");
                g_tracked_allocs.insert(addr);
            }
            emu->set_reg(0, addr);
        }
        return;
    }

    // --- SRE Tracked Realloc ---
    uint64_t lr = emu->get_reg(30);
    bool should_track = false;
    if (g_sre_mod.base_addr != 0 && lr >= g_sre_mod.base_addr && lr < g_sre_mod.base_addr + g_sre_mod.mem_size) {
        should_track = true;
    } else if (g_main_mod_64.base_addr != 0 && lr >= g_main_mod_64.base_addr && lr < g_main_mod_64.base_addr + g_main_mod_64.mem_size) {
        should_track = true;
    }
    bool was_tracked = (ptr != 0 && g_tracked_allocs.count(ptr) > 0);

    if (ptr == 0) {
        uint32_t addr = host_malloc_locked(size);
        if (should_track) {
            RedstellGC::instance().track_alloc(addr, size, lr, "realloc(new)");
            g_tracked_allocs.insert(addr);
        }
        emu->set_reg(0, addr);
        return;
    }
    if (size == 0) {
        if (was_tracked) {
            RedstellGC::instance().track_free(ptr, lr);
            g_tracked_allocs.erase(ptr);
        }
        host_free_locked(ptr);
        emu->set_reg(0, 0);
        return;
    }
    
    if (g_guest_allocs.count(ptr)) {
        uint32_t old_size = g_guest_allocs[ptr];
        uint32_t aligned_old_size = (old_size + 7) & ~7;
        uint32_t aligned_new_size = (size + 7) & ~7;
        
        if (aligned_new_size <= aligned_old_size) {
            g_guest_allocs[ptr] = size;
            if (was_tracked) {
                RedstellGC::instance().track_free(ptr, lr);
                g_tracked_allocs.erase(ptr);
            }
            if (should_track) {
                RedstellGC::instance().track_alloc(ptr, size, lr, "realloc(inplace)");
                g_tracked_allocs.insert(ptr);
            }
            emu->set_reg(0, ptr);
            return;
        }
        
        if (ptr + aligned_old_size == g_guest_heap_ptr) {
            g_guest_heap_ptr = ptr + aligned_new_size;
            g_guest_allocs[ptr] = size;
            if (was_tracked) {
                RedstellGC::instance().track_free(ptr, lr);
                g_tracked_allocs.erase(ptr);
            }
            if (should_track) {
                RedstellGC::instance().track_alloc(ptr, size, lr, "realloc(extend)");
                g_tracked_allocs.insert(ptr);
            }
            emu->set_reg(0, ptr);
            return;
        }
        
        uint32_t addr = host_malloc_locked(size);
        uint32_t copy_size = (old_size < size) ? old_size : size;
        if (copy_size > 0) {
            std::memcpy(emu->get_memory_base() + addr, emu->get_memory_base() + ptr, copy_size);
        }
        if (was_tracked) {
            RedstellGC::instance().track_free(ptr, lr);
            g_tracked_allocs.erase(ptr);
        }
        host_free_locked(ptr);
        if (should_track) {
            RedstellGC::instance().track_alloc(addr, size, lr, "realloc(move)");
            g_tracked_allocs.insert(addr);
        }
        emu->set_reg(0, addr);
    } else {
        uint32_t addr = host_malloc_locked(size);
        if (size > 0) {
            std::memcpy(emu->get_memory_base() + addr, emu->get_memory_base() + ptr, size);
        }
        if (should_track) {
            RedstellGC::instance().track_alloc(addr, size, lr, "realloc(fallback)");
            g_tracked_allocs.insert(addr);
        }
        emu->set_reg(0, addr);
    }
}

static void bridge_free(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t ptr = emu->get_reg(0);
    if (ptr) {
        std::lock_guard<std::mutex> lock(g_heap_mutex);
        
        uint64_t lr = emu->get_reg(30);
        if (g_tracked_allocs.erase(ptr)) {
            RedstellGC::instance().track_free(ptr, lr);
        }

        host_free_locked(ptr);
    }
}

static void bridge_getenv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t name_addr = emu->get_reg(0);
    if (name_addr == 0) {
        emu->set_reg(0, 0);
        return;
    }
    const char* name = (const char*)(emu->get_memory_base() + name_addr);
    
    // Static cache to avoid leaking guest memory on repeated lookups
    static std::unordered_map<std::string, uint32_t> env_cache;
    auto it = env_cache.find(name);
    if (it != env_cache.end()) {
        emu->set_reg(0, it->second);
        return;
    }
    
    const char* value = std::getenv(name);
    if (value == nullptr) {
        env_cache[name] = 0;
        emu->set_reg(0, 0);
        return;
    }
    
    uint32_t len = std::strlen(value);
    uint32_t addr = g_guest_heap_ptr;
    g_guest_heap_ptr += (len + 1 + 7) & ~7;
    
    std::memcpy(emu->get_memory_base() + addr, value, len + 1);
    env_cache[name] = addr;
    
    emu->set_reg(0, addr);
}

static void bridge_signal(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // signal(int sig, void (*func)(int))
    // Stub: return success/SIG_DFL (0)
    emu->set_reg(0, 0);
}


// --- Standard C Memory & String Bridges ---
static void bridge_memcpy(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);

    if (n > 0) {
        std::memmove(emu->get_memory_base() + dest, emu->get_memory_base() + src, n);
    }
    emu->set_reg(0, dest);
}

static void bridge_memset(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t c = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (n > 0) {
        std::memset(emu->get_memory_base() + dest, c, n);
    }
    emu->set_reg(0, dest);
}

static void bridge_memmove(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);

    if (n > 0) {
        std::memmove(emu->get_memory_base() + dest, emu->get_memory_base() + src, n);
    }
    emu->set_reg(0, dest);
}

static void bridge_strlen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t str = emu->get_reg(0);
    const char* s = (const char*)(emu->get_memory_base() + str);
    emu->set_reg(0, std::strlen(s));
}

static void bridge_memcmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t str1 = emu->get_reg(0);
    uint64_t str2 = emu->get_reg(1);
    uint64_t n = emu->get_reg(2);
    int res = 0;
    if (n > 0 && str1 && str2) {
        res = std::memcmp(emu->get_memory_base() + str1, emu->get_memory_base() + str2, n);
    }
    emu->set_reg(0, (uint64_t)(int64_t)res);
}

static void bridge_strnlen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t str = emu->get_reg(0);
    uint64_t maxlen = emu->get_reg(1);
    if (!str) { emu->set_reg(0, 0); return; }
    const char* s = (const char*)(emu->get_memory_base() + str);
    size_t len = 0;
    while (len < maxlen && s[len] != '\0') len++;
    emu->set_reg(0, len);
}

static void bridge_strdup(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t str = emu->get_reg(0);
    if (!str) { emu->set_reg(0, 0); return; }
    const char* s = (const char*)(emu->get_memory_base() + str);
    size_t len = std::strlen(s);
    uint32_t addr = host_malloc_locked((uint32_t)(len + 1));
    std::memcpy(emu->get_memory_base() + addr, s, len + 1);
    emu->set_reg(0, addr);
}

static void bridge_strndup(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t str = emu->get_reg(0);
    uint64_t n = emu->get_reg(1);
    if (!str) { emu->set_reg(0, 0); return; }
    const char* s = (const char*)(emu->get_memory_base() + str);
    size_t len = 0;
    while (len < n && s[len] != '\0') len++;
    uint32_t addr = host_malloc_locked((uint32_t)(len + 1));
    std::memcpy(emu->get_memory_base() + addr, s, len);
    *(char*)(emu->get_memory_base() + addr + len) = '\0';
    emu->set_reg(0, addr);
}

static void bridge_strcasecmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t str1 = emu->get_reg(0);
    uint64_t str2 = emu->get_reg(1);
    if (!str1 || !str2) { emu->set_reg(0, 0); return; }
    const char* s1 = (const char*)(emu->get_memory_base() + str1);
    const char* s2 = (const char*)(emu->get_memory_base() + str2);
    int res = strcasecmp(s1, s2);
    emu->set_reg(0, (uint64_t)(int64_t)res);
}

static void bridge_strncasecmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t str1 = emu->get_reg(0);
    uint64_t str2 = emu->get_reg(1);
    uint64_t n = emu->get_reg(2);
    if (!str1 || !str2) { emu->set_reg(0, 0); return; }
    const char* s1 = (const char*)(emu->get_memory_base() + str1);
    const char* s2 = (const char*)(emu->get_memory_base() + str2);
    int res = strncasecmp(s1, s2, n);
    emu->set_reg(0, (uint64_t)(int64_t)res);
}



// --- Soft-Float Math Bridges ---
static void bridge_cosf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::cos(x));
}

static void bridge_sinf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::sin(x));
}

static void bridge_atan2f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float y = emu->get_sreg(0);
    float x = emu->get_sreg(1);
    emu->set_sreg(0, std::atan2(y, x));
}

static void bridge_powf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    float y = emu->get_sreg(1);
    emu->set_sreg(0, std::pow(x, y));
}

static void bridge_pow(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    double base = emu->get_dreg(0);
    double exp_val = emu->get_dreg(1);
    emu->set_dreg(0, std::pow(base, exp_val));
}

static void bridge_sin(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::sin(emu->get_dreg(0)));
}

static void bridge_cos(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::cos(emu->get_dreg(0)));
}

static void bridge_acos(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::acos(emu->get_dreg(0)));
}

static void bridge_round(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::round(emu->get_dreg(0)));
}

static void bridge_tan(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::tan(emu->get_dreg(0)));
}

// --- Double-precision math (critical for Lua) ---
static void bridge_floor(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::floor(emu->get_dreg(0)));
}

static void bridge_ceil(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::ceil(emu->get_dreg(0)));
}

static void bridge_sqrt(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::sqrt(emu->get_dreg(0)));
}

static void bridge_fmod(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::fmod(emu->get_dreg(0), emu->get_dreg(1)));
}

static void bridge_fabs(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::fabs(emu->get_dreg(0)));
}

static void bridge_abs(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int64_t val = (int64_t)emu->get_reg(0);
    emu->set_reg(0, (uint64_t)std::abs(val));
}

static void bridge_log(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::log(emu->get_dreg(0)));
}

static void bridge_log10(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::log10(emu->get_dreg(0)));
}

static void bridge_log2(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::log2(emu->get_dreg(0)));
}

static void bridge_exp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::exp(emu->get_dreg(0)));
}

static void bridge_ldexp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    double x = emu->get_dreg(0);
    int n = (int)emu->get_reg(0);  // AAPCS64: int arg in W0 (separate from FP)
    emu->set_dreg(0, std::ldexp(x, n));
}

static void bridge_frexp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    double x = emu->get_dreg(0);
    uint32_t exp_ptr = emu->get_reg(0);  // int* exp in X0
    int exp_val;
    double result = std::frexp(x, &exp_val);
    if (exp_ptr) *(int*)(memory + exp_ptr) = exp_val;
    emu->set_dreg(0, result);
}

static void bridge_modf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    double x = emu->get_dreg(0);
    uint32_t iptr = emu->get_reg(0);  // double* iptr in X0
    double intpart;
    double result = std::modf(x, &intpart);
    if (iptr) *(double*)(memory + iptr) = intpart;
    emu->set_dreg(0, result);
}

static void bridge_asin(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::asin(emu->get_dreg(0)));
}

static void bridge_atan(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::atan(emu->get_dreg(0)));
}

static void bridge_atan2(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_dreg(0, std::atan2(emu->get_dreg(0), emu->get_dreg(1)));
}

static void bridge_tanf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_sreg(0, std::tan(emu->get_sreg(0)));
}

static void bridge_roundf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_sreg(0, std::round(emu->get_sreg(0)));
}

static void bridge_floorf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_sreg(0, std::floor(emu->get_sreg(0)));
}

static void bridge_ceilf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_sreg(0, std::ceil(emu->get_sreg(0)));
}

static void bridge_sqrtf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_sreg(0, std::sqrt(emu->get_sreg(0)));
}

static void bridge_acosf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::acos(x));
}

static void bridge_asinf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::asin(x));
}

static void bridge_atanf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::atan(x));
}

static void bridge_fmodf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    float y = emu->get_sreg(1);
    emu->set_sreg(0, std::fmod(x, y));
}

static void bridge_expf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::exp(x));
}

static void bridge_logf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::log(x));
}

static void bridge_log10f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    float x = emu->get_sreg(0);
    emu->set_sreg(0, std::log10(x));
}

static void bridge_sincosf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // ARM64: float arg in S0, output pointers in X0, X1 (AAPCS64: int/ptr and float regs are independent)
    float x = emu->get_sreg(0);
    uint32_t sin_ptr = (uint32_t)emu->get_reg(0);
    uint32_t cos_ptr = (uint32_t)emu->get_reg(1);
    float s = std::sin(x);
    float c = std::cos(x);
    uint8_t* memory = emu->get_memory_base();
    memcpy(memory + sin_ptr, &s, 4);
    memcpy(memory + cos_ptr, &c, 4);
}

// --- JNI Standard Bridges ---
static void bridge_FindClass(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(1);
    const char* name = (const char*)(memory + name_ptr);
    std::cout << "[JNI] FindClass: " << name << std::endl;
    emu->set_reg(0, 0x12340001);
}

static void bridge_GetMethodID(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(2);
    const char* name = (const char*)(memory + name_ptr);
    // Return unique IDs per method name (matching Vita approach)
    uint32_t id;
    if (strcmp(name, "getPlatformConsentState") == 0) id = 0x13180001;
    else if (strcmp(name, "isAgeKnown") == 0) id = 0x13170001;
    else if (strcmp(name, "enteredAge") == 0) id = 0x13170002;
    else if (strcmp(name, "isAgeOfConsent") == 0) id = 0x13170003;
    else if (strcmp(name, "hasPrivacyConsent") == 0) id = 0x13170004;
    else if (strcmp(name, "isExplicitPrivacyConsent") == 0) id = 0x13170005;
    else if (strcmp(name, "getAnalyticsId") == 0) id = 0x13170006;
    else if (strcmp(name, "receivedPrivacyConsent") == 0) id = 0x13170007;
    else if (strcmp(name, "loadFile") == 0) id = 0x13190001;
    else if (strcmp(name, "play") == 0) id = 0x13200001;
    else if (strcmp(name, "pause") == 0) id = 0x13210001;
    else if (strcmp(name, "stop") == 0) id = 0x13220001;
    else if (strcmp(name, "setLooping") == 0) id = 0x13230001;
    else if (strcmp(name, "setVolume") == 0) id = 0x13240001;
    else if (strcmp(name, "loadSnapshot") == 0) id = 0x13250001;
    else if (strcmp(name, "saveSnapshot") == 0) id = 0x13250002;
    else if (strcmp(name, "deleteSnapshot") == 0) id = 0x13250003;
    else if (strcmp(name, "isGoogleGameServicesAvailable") == 0) id = 0x13260001;
    else if (strcmp(name, "startAdsAndAnalytics") == 0) id = 0x13270001;
    // SharedPreferences bridge
    else if (strcmp(name, "getBooleanFromSP") == 0) id = 0x13280001;
    else if (strcmp(name, "saveBooleanInSP") == 0) id = 0x13280002;
    else if (strcmp(name, "getIntFromSP") == 0) id = 0x13280003;
    else if (strcmp(name, "saveIntInSP") == 0) id = 0x13280004;
    else if (strcmp(name, "getLongFromSP") == 0) id = 0x13280005;
    else if (strcmp(name, "saveLongInSP") == 0) id = 0x13280006;
    else if (strcmp(name, "<init>") == 0) id = 0x13000001;
    else if (strcmp(name, "startTextInput") == 0) id = 0x13290001;
    else if (strcmp(name, "stopTextInput") == 0) id = 0x13290002;
    else id = 0x56780001;
    if (!emu->quiet_mode && id != 0x56780001) {
        std::cout << "[JNI] GetMethodID: " << name << " -> 0x" << std::hex << id << std::dec << std::endl;
    }
    emu->set_reg(0, id);
}

static void bridge_RegisterNatives(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_NewGlobalRef(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t obj = emu->get_reg(1);
    emu->set_reg(0, obj);
}

static std::unordered_map<uint32_t, std::string> g_jstrings;
static uint32_t g_next_jstring = 0x99990001;

static void bridge_NewStringUTF(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(1);
    
    std::string str = "";
    if (str_ptr != 0) {
        str = (const char*)(memory + str_ptr);
    }
    
    uint32_t handle = g_next_jstring++;
    g_jstrings[handle] = str;
    
    emu->set_reg(0, handle);
}

static void bridge_GetStringUTFChars(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t jstr = emu->get_reg(1);
    
    if (jstr >= 0x20000 && jstr < 0x21000) {
        emu->set_reg(0, jstr);
    } else if (g_jstrings.count(jstr) > 0) {
        std::string str = g_jstrings[jstr];
        uint32_t addr = g_guest_heap_ptr;
        g_guest_heap_ptr += (str.length() + 8) & ~7;
        std::strcpy((char*)(memory + addr), str.c_str());
        emu->set_reg(0, addr);
    } else if (jstr > 0x1000 && jstr < 0x40000000) {
        // Treat as direct C-string pointer in guest memory
        emu->set_reg(0, jstr);
    } else {
        uint32_t addr = 0x30000;
        strcpy((char*)(memory + addr), "dummy_jni_string");
        emu->set_reg(0, addr);
    }
}

static void bridge_AAssetManager_fromJava(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0xAA000001ULL);
}

static void bridge_AAssetManager_open(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t filename_ptr = emu->get_reg(1);
    const char* filename = (const char*)(memory + filename_ptr);
    g_frame_stats.asset_opens++;
    

    // Mission 13: Asset proof detection
    std::string fname(filename);
    if (fname.find("swordigo_title") != std::string::npos && !g_frame_stats.title_texture_loaded) {
        g_frame_stats.title_texture_loaded = true;
        std::cout << "[ASSET PROOF] ✓ Title texture loaded: " << filename << std::endl;
    }
    if (fname == "resources/menu.scene" && !g_frame_stats.menu_scene_loaded) {
        g_frame_stats.menu_scene_loaded = true;
        std::cout << "[ASSET PROOF] ✓ Menu scene loaded: " << filename << std::endl;
    }
    if (fname.find("menu_back") != std::string::npos && !g_frame_stats.menu_back_loaded) {
        g_frame_stats.menu_back_loaded = true;
        std::cout << "[ASSET PROOF] ✓ Menu background loaded: " << filename << std::endl;
    }
    if (fname.find("game_common_atlas") != std::string::npos && !g_frame_stats.common_atlas_loaded) {
        g_frame_stats.common_atlas_loaded = true;
        std::cout << "[ASSET PROOF] ✓ Common atlas loaded: " << filename << std::endl;
    }
    
    // Music ducking trigger: when 0item.wav is loaded, duck the background music
    // This is the SFX played for boss kills and XP sack events.
    extern int g_music_duck_trigger;
    if (fname.find("0item.wav") != std::string::npos) {
        g_music_duck_trigger = 1;
    }
    
    void* asset = AAssetManager_open(NULL, filename, emu->get_reg(2));
    if (!asset) {
        std::cerr << "[ASSET] Open FAILED: " << filename << std::endl;
    }
    emu->set_reg(0, register_pointer(asset));
}



static void bridge_AAsset_read(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    void* buf = (void*)(memory + emu->get_reg(1));
    uint32_t count = emu->get_reg(2);
    int read = AAsset_read(asset, buf, count);
    if (read < 0) {
        std::cerr << "[Asset] READ FAILED: " << (asset ? asset->name : "NULL") << std::endl;
    }
    emu->set_reg(0, (uint64_t)(int64_t)read);
}

static void bridge_AAsset_close(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    AAsset_close(asset);
    release_pointer(asset);
}

static void bridge_AAsset_getLength(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    emu->set_reg(0, (uint64_t)(int64_t)AAsset_getLength(asset));
}

static void bridge_AAsset_openFileDescriptor(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    off_t* outStart = (off_t*)(memory + emu->get_reg(1));
    off_t* outLength = (off_t*)(memory + emu->get_reg(2));
    int fd = AAsset_openFileDescriptor(asset, outStart, outLength);
    if (!emu->quiet_mode) {
        std::cout << "[Asset] openFileDescriptor: " << (asset ? asset->name : "NULL") << " -> fd=" << fd << std::endl;
    }
    emu->set_reg(0, (uint64_t)(int64_t)fd);
}

// --- RESTORED JNI ENVIRONMENT SHIMS ---
static void bridge_GetJavaVM(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t vm_ptr_ptr = emu->get_reg(1);
    *(uint64_t*)(memory + vm_ptr_ptr) = 0x11000ULL; // Fake VM pointer (64-bit!)
    emu->set_reg(0, 0); // JNI_OK
}

static void bridge_GetEnv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t env_ptr_ptr = emu->get_reg(2);
    *(uint64_t*)(memory + env_ptr_ptr) = 0x10000ULL; // Fake Env pointer (64-bit!)
    emu->set_reg(0, 0); // JNI_OK
}

static void bridge_ThrowNew(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_PushLocalFrame(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_PopLocalFrame(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_DeleteLocalRef(void* emu_ptr) {
}

static void bridge_DeleteGlobalRef(void* emu_ptr) {
}

static void bridge_NewObjectV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0x43434343);
}

static void bridge_CallObjectMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t obj = emu->get_reg(1);
    uint32_t mid = emu->get_reg(2);

    // The engine calls getPlatformConsentState which returns a consent object.
    // If we return NULL, engine retries every frame. Return a valid fake object.
    if (mid == 0x13180001) {
        static int count = 0;
        count++;
        if (count <= 3) std::cout << "[JNI] CallObjectMethodV(getPlatformConsentState) -> returning fake consent obj" << std::endl;
        // Return a non-null fake consent object so engine doesn't retry
        emu->set_reg(0, 0x56565656);
    } else {
        emu->set_reg(0, 0x34343434);
    }
}

static bool g_music_looping = false;
static float g_music_volume = 1.0f;
static ALuint g_music_source = 0;
static ALuint g_music_buffer = 0;

static int g_saved_age = 25; // Pre-set age to bypass age gate entirely

// ---------------------------------------------------------------
// SharedPreferences — persistent key-value store backed by prefs.ini
// Mirrors Android SharedPreferences used by the native engine for:
//   knownAge, privacyConsent, explicitConsent, foreground timing, etc.
// ---------------------------------------------------------------
static std::unordered_map<std::string, std::string> g_prefs;
static bool g_prefs_loaded = false;

static void prefs_load() {
    if (g_prefs_loaded) return;
    g_prefs_loaded = true;
    // Sensible defaults — ensures consent/age gate never blocks gameplay
    g_prefs["knownAge"]             = "25";
    g_prefs["privacyConsent"]       = "true";
    g_prefs["explicitConsent"]      = "true";
    g_prefs["ageConsent"]           = "true";
    g_prefs["totalForegroundTime"]  = "999999";
    g_prefs["foregroundTimeForReviewFlow"] = "0";
    g_prefs["delayToReviewFlow"]    = "999999";
    // Load overrides from disk
    std::string path = g_save_dir + "/prefs.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq);
        std::string v = s.substr(eq + 1);
        if (!v.empty() && v.back() == '\n') v.pop_back();
        g_prefs[k] = v;
    }
    fclose(f);
    std::cout << "[Prefs] Loaded " << g_prefs.size() << " preferences from " << path << std::endl;
}

static void prefs_save() {
    std::string path = g_save_dir + "/prefs.ini";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { std::cerr << "[Prefs] Cannot write " << path << std::endl; return; }
    for (auto& kv : g_prefs) {
        fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(f);
}

static std::string pref_get(const std::string& key, const std::string& def = "0") {
    prefs_load();
    auto it = g_prefs.find(key);
    return (it != g_prefs.end()) ? it->second : def;
}

static void pref_set(const std::string& key, const std::string& val) {
    prefs_load();
    g_prefs[key] = val;
    prefs_save();
    std::cout << "[Prefs] Set " << key << " = " << val << std::endl;
}

// Resolve a jstring handle (from g_jstrings map) or a raw guest C-string pointer
// ── Per-slot snapshot naming ──────────────────────────────────────────────
// Each save slot is identified by the snapshot "name" that the guest passes to
// saveSnapshot/loadSnapshot/deleteSnapshot. Before this change the name was
// ignored and every slot collapsed into a single snapshot.bin, so a newly
// created save slot lost its identity and never appeared again on the save
// screen. Keep everything under g_save_dir so old paths keep working.
static std::string snapshot_sanitize_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0' || (unsigned char)c < 0x20)
            out += '_';
        else
            out += c;
    }
    if (out.empty()) return "snapshot";
    if (out.size() > 120) out = out.substr(0, 120);
    return out;
}

static std::string snapshot_path_for_name(const std::string& name) {
    return g_save_dir + "/snapshots/" + snapshot_sanitize_name(name) + ".bin";
}

static std::string snapshot_documents_path_for_name(const std::string& name) {
    return g_save_dir + "/Documents/" + snapshot_sanitize_name(name) + ".gplayer";
}

static std::string resolve_jstring(uint32_t handle, uint8_t* memory) {
    auto it = g_jstrings.find(handle);
    if (it != g_jstrings.end()) return it->second;
    // Fallback: treat as direct C-string pointer in guest memory
    if (handle > 0x1000 && handle < 0x40000000)
        return std::string((const char*)(memory + handle));
    return "";
}
extern int g_snapshot_load_pending_count;
extern bool g_snapshot_has_data; 

static bool load_wav_to_buffer(const std::string& path, ALuint buffer) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    
    char chunk_id[4];
    uint32_t chunk_size;
    
    // Read RIFF header
    if (fread(chunk_id, 1, 4, f) != 4 ||
        fread(&chunk_size, 4, 1, f) != 1 ||
        fread(chunk_id, 1, 4, f) != 4 || // Format: should be "WAVE"
        std::memcmp(chunk_id, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }
    
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    std::vector<uint8_t> data;
    
    while (fread(chunk_id, 1, 4, f) == 4) {
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            fread(&audio_format, 2, 1, f);
            fread(&channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            fseek(f, 6, SEEK_CUR); // skip byte_rate, block_align
            fread(&bits_per_sample, 2, 1, f);
            if (chunk_size > 16) {
                fseek(f, chunk_size - 16, SEEK_CUR);
            }
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            data.resize(chunk_size);
            fread(data.data(), 1, chunk_size, f);
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    fclose(f);
    
    if (data.empty()) return false;
    
    ALenum al_format = AL_FORMAT_MONO8;
    if (channels == 1) {
        al_format = (bits_per_sample == 16) ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8;
    } else {
        al_format = (bits_per_sample == 16) ? AL_FORMAT_STEREO16 : AL_FORMAT_STEREO8;
    }
    
    alBufferData(buffer, al_format, data.data(), data.size(), sample_rate);
    return alGetError() == AL_NO_ERROR;
}

// Static MPG123 initialization (called once at first MP3 load)
static bool g_mpg123_initialized = false;
static void ensure_mpg123_init() {
    if (!g_mpg123_initialized) {
        int err = mpg123_init();
        if (err == MPG123_OK) {
            g_mpg123_initialized = true;
        } else {
            std::cerr << "[SRE-Music] WARNING: mpg123_init failed: " << mpg123_plain_strerror(err) << std::endl;
        }
    }
}

// Load MP3 file → decode via libmpg123 → fill OpenAL buffer
// Direct library-based decoding (no temporary files or shell commands)
static bool load_mp3_to_buffer(const std::string& path, ALuint buffer) {
    // Ensure mpg123 is initialized
    ensure_mpg123_init();
    if (!g_mpg123_initialized) {
        return false;
    }

    // Create decoder handle
    int err = 0;
    mpg123_handle* mh = mpg123_new(nullptr, &err);
    if (!mh) {
        std::cerr << "[SRE-Music] Failed to create mpg123 handle: " << mpg123_plain_strerror(err) << std::endl;
        return false;
    }

    // Open the MP3 file
    err = mpg123_open(mh, path.c_str());
    if (err != MPG123_OK) {
        std::cerr << "[SRE-Music] Failed to open MP3: " << path << " - " << mpg123_strerror(mh) << std::endl;
        mpg123_delete(mh);
        return false;
    }

    // Get audio format info
    long rate = 0;
    int channels = 0, encoding = 0;
    err = mpg123_getformat(mh, &rate, &channels, &encoding);
    if (err != MPG123_OK) {
        std::cerr << "[SRE-Music] Failed to get MP3 format: " << mpg123_strerror(mh) << std::endl;
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }

    // Determine OpenAL format
    ALenum al_format;
    if (channels == 1) {
        al_format = AL_FORMAT_MONO16;
    } else if (channels == 2) {
        al_format = AL_FORMAT_STEREO16;
    } else {
        std::cerr << "[SRE-Music] Unsupported channel count: " << channels << std::endl;
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }

    // Allocate buffer for decoded PCM data
    std::vector<unsigned char> pcm_data;
    unsigned char decode_buf[8192];
    size_t bytes_read = 0;

    // Decode entire MP3 to PCM
    while (mpg123_read(mh, decode_buf, sizeof(decode_buf), &bytes_read) == MPG123_OK && bytes_read > 0) {
        pcm_data.insert(pcm_data.end(), decode_buf, decode_buf + bytes_read);
    }

    if (pcm_data.empty()) {
        std::cerr << "[SRE-Music] Failed to decode MP3 data: " << path << std::endl;
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }

    // Upload to OpenAL buffer
    alBufferData(buffer, al_format, pcm_data.data(), (ALsizei)pcm_data.size(), (ALsizei)rate);
    ALenum al_err = alGetError();
    
    // Clean up
    mpg123_close(mh);
    mpg123_delete(mh);

    if (al_err != AL_NO_ERROR) {
        std::cerr << "[SRE-Music] OpenAL buffer error: " << al_err << std::endl;
        return false;
    }

    std::cerr << "[SRE-Music] Loaded MP3: " << path << " (" << channels << "ch @ " << rate << "Hz)" << std::endl;
    return true;
}

// Load OGG Vorbis file → decode to PCM → fill OpenAL buffer
static bool load_ogg_to_buffer(const std::string& path, ALuint buffer) {
    OggVorbis_File vf;
    if (ov_fopen(path.c_str(), &vf) != 0) return false;

    vorbis_info* info = ov_info(&vf, -1);
    if (!info) { ov_clear(&vf); return false; }

    ALenum format = (info->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    ALsizei freq = info->rate;

    // Decode entire OGG to PCM
    std::vector<char> pcm;
    char buf[8192];
    int section = 0;
    long bytes;
    while ((bytes = ov_read(&vf, buf, sizeof(buf), 0, 2, 1, &section)) > 0) {
        pcm.insert(pcm.end(), buf, buf + bytes);
    }
    ov_clear(&vf);

    if (pcm.empty()) return false;

    alBufferData(buffer, format, pcm.data(), (ALsizei)pcm.size(), freq);
    return alGetError() == AL_NO_ERROR;
}

static void bridge_CallBooleanMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t obj = emu->get_reg(1);
    uint32_t method_id = emu->get_reg(2);
    uint32_t va_list_ptr = emu->get_reg(3);
    Arm64VaList va(memory, va_list_ptr);
    
    uint32_t res = 0;
    if (method_id == 0x13190001) { // loadFile
        uint32_t jstr = (uint32_t)va.next_gp();
        std::string fname = "";
        if (g_jstrings.count(jstr) > 0) {
            fname = g_jstrings[jstr];
        }
        
        std::cout << "[JNI] MusicPlayer.loadFile(\"" << fname << "\")" << std::endl;
        
        // Death detection: flag when gameover music is loaded
        if (fname.find("gameover") != std::string::npos) {
            g_death_detected_countdown = 180;  // ~3 seconds at 60fps, then auto-restart
            std::cout << "[Fix] Death detected (gameover music loaded) — auto-restart in ~3s" << std::endl;
        }
        
        if (g_music_source != 0) {
            alSourceStop(g_music_source);
            alSourcei(g_music_source, AL_BUFFER, 0);
        }
        // Build music path — get_data_path handles all install locations
        // (AppImage, RPM /usr/share/swordigo/, dev ./, etc.)
        extern std::string g_assets_dir;
        std::string music_dir = get_data_path(g_assets_dir + "/resources/music");
        // RL mod stores music at rl_assets/music/ (not resources/music/)
        std::string music_dir_alt = get_data_path(g_assets_dir + "/music");
        
        // Build filename — replace dashes with underscores
        std::string safe_fname = fname;
        size_t pos;
        while ((pos = safe_fname.find('-')) != std::string::npos) {
            safe_fname.replace(pos, 1, "_");
        }
        
        // Try OGG: prefixed (vanilla), plain (RL in resources/music), alt dir (RL in music/)
        std::string ogg_prefixed = music_dir + "/music_" + safe_fname + ".ogg";
        std::string ogg_plain    = music_dir + "/" + safe_fname + ".ogg";
        std::string ogg_alt      = music_dir_alt + "/" + safe_fname + ".ogg";
        std::string ogg_alt_pre  = music_dir_alt + "/music_" + safe_fname + ".ogg";
        std::string wav_prefixed = music_dir + "/music_" + safe_fname + ".wav";
        
        std::cout << "  -> Trying OGG: " << ogg_prefixed << std::endl;
        
        if (g_music_buffer == 0) {
            alGenBuffers(1, &g_music_buffer);
        }
        
        bool loaded = load_ogg_to_buffer(ogg_prefixed, g_music_buffer);
        if (!loaded) {
            std::cout << "  -> Trying OGG (no prefix): " << ogg_plain << std::endl;
            loaded = load_ogg_to_buffer(ogg_plain, g_music_buffer);
        }
        if (!loaded) {
            std::cout << "  -> Trying OGG (alt dir): " << ogg_alt << std::endl;
            loaded = load_ogg_to_buffer(ogg_alt, g_music_buffer);
        }
        if (!loaded) {
            std::cout << "  -> Trying OGG (alt dir prefixed): " << ogg_alt_pre << std::endl;
            loaded = load_ogg_to_buffer(ogg_alt_pre, g_music_buffer);
        }
        if (!loaded) {
            std::cout << "  -> Trying WAV: " << wav_prefixed << std::endl;
            loaded = load_wav_to_buffer(wav_prefixed, g_music_buffer);
        }
        if (loaded) {
            if (g_music_source == 0) {
                alGenSources(1, &g_music_source);
            }
            alSourcei(g_music_source, AL_BUFFER, g_music_buffer);
            alSourcei(g_music_source, AL_LOOPING, g_music_looping ? AL_TRUE : AL_FALSE);
            alSourcef(g_music_source, AL_GAIN, g_music_volume);
            res = 1;
        } else {
            std::cerr << "  ⚠ Failed to load music: " << safe_fname << std::endl;
            res = 0;
        }
    } else {
        res = 0; // return 0 like Vita port
    }
    
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallBooleanMethodV(mid=0x" << std::hex << method_id << ") -> " << res << std::dec << std::endl;
    }
    emu->set_reg(0, res);
}

static void bridge_CallIntMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t obj = emu->get_reg(1);
    uint32_t method_id = emu->get_reg(2);
    uint32_t res = 0;
    if (method_id == 0x13180001) { // getPlatformConsentState
        res = 0; // Return 0 (unknown) to trigger consent/age check flow in-game
    } else {
        res = 0;
    }
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallIntMethodV(mid=0x" << std::hex << method_id << ") -> " << res << std::dec << std::endl;
    }
    emu->set_reg(0, res);
}

static void bridge_CallLongMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // ARM64: 64-bit return value fits in single X0
    emu->set_reg(0, 0);
}

static void bridge_CallVoidMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t obj = emu->get_reg(1);
    uint32_t method_id = emu->get_reg(2);
    uint32_t va_list_ptr = emu->get_reg(3);
    Arm64VaList va(memory, va_list_ptr);
    
    if (method_id == 0x13200001) { // play
        std::cout << "[Music] play()" << std::endl;
        if (g_music_source != 0) {
            alSourcePlay(g_music_source);
        }
    } else if (method_id == 0x13210001) { // pause
        std::cout << "[Music] pause()" << std::endl;
        if (g_music_source != 0) {
            alSourcePause(g_music_source);
        }
    } else if (method_id == 0x13220001) { // stop
        std::cout << "[Music] stop()" << std::endl;
        if (g_music_source != 0) {
            alSourceStop(g_music_source);
        }
    } else if (method_id == 0x13230001) { // setLooping
        uint32_t looping = (uint32_t)va.next_gp();
        g_music_looping = (looping != 0);
        if (g_music_source != 0) {
            alSourcei(g_music_source, AL_LOOPING, g_music_looping ? AL_TRUE : AL_FALSE);
        }
    } else if (method_id == 0x13240001) { // setVolume
        // ARM64: double in va_list — use FP register slot
        double vol_d = va.next_fp();
        g_music_volume = (float)vol_d;
        
        // Clamp: prevent total silence (game uses vol=0 during transitions)
        if (g_music_volume < 0.05f && g_music_volume >= 0.0f) {
            g_music_volume = 0.05f;
        }
        if (g_music_volume > 1.0f) g_music_volume = 1.0f;
        
        std::cout << "[Music] setVolume(" << g_music_volume << ") raw_double=" << vol_d << std::endl;
        if (g_music_source != 0) {
            alSourcef(g_music_source, AL_GAIN, g_music_volume);
        }
    } else if (method_id == 0x13190001) { // loadFile
        // Music loadFile: load a new background music track
        uint32_t va_list_ptr = emu->get_reg(3);
        Arm64VaList va2(memory, va_list_ptr);
        uint32_t str_ref = (uint32_t)va2.next_gp();
        std::string filename;
        if (g_jstrings.count(str_ref)) {
            filename = g_jstrings[str_ref];
        } else if (str_ref > 0 && str_ref < 0xE0000000) {
            filename = (const char*)(memory + str_ref);
        }
        std::cout << "[Music] loadFile(\"" << filename << "\") — STUBBED" << std::endl;
    } else {
        // Log unknown method IDs — critical for debugging loading issues
        std::cerr << "[JNI] !! UNHANDLED CallVoidMethodV(obj=0x" << std::hex << obj 
                  << ", mid=0x" << method_id << ")" << std::dec << std::endl;
    }
}

static void bridge_GetObjectClass(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0x44444444);
}

static void bridge_GetFieldID(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_GetBooleanField(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 1);
}

static void bridge_GetIntField(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_GetFloatField(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // ARM64: float return in S0
    emu->set_sreg(0, 0.0f);
}

static void bridge_CallStaticObjectMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    uint32_t res = 0;
    if (mid == 0x13170006) { // getAnalyticsId
        g_jstrings[0x99990000] = ""; // Ensure empty string handle is valid
        res = 0x99990000;
    } else {
        res = 0;
    }
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticObjectMethodV(mid=0x" << std::hex << mid << ") -> 0x" << res << std::dec << std::endl;
    }
    emu->set_reg(0, res);
}

static void bridge_CallStaticBooleanMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    uint32_t res = 0;
    if (mid == 0x13170001) { // isAgeKnown
        res = 1; // Always return true — bypass age gate entirely
    } else if (mid == 0x13170003) { // isAgeOfConsent
        res = 1; // Return true — user is old enough
    } else if (mid == 0x13170004) { // hasPrivacyConsent
        res = 1; // Always return true
    } else if (mid == 0x13170005) { // isExplicitPrivacyConsent
        res = 1; // Always return true
    } else if (mid == 0x13180001) { // getPlatformConsentState
        res = 3; // Return 3 — OBTAINED
    } else if (mid == 0x13260001) { // isGoogleGameServicesAvailable
        res = 0; // Return false — GGS sign-in flow blocks forever on desktop.
                 // Returning 1 triggers HandleGoogleSignInCompleted(true) which
                 // waits for Android Sign-In UI/network that never completes.
                 // Snapshots are handled locally; GGS is not needed.
    } else if (mid == 0x13280001) { // getBooleanFromSP(String key)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_ptr);
        uint32_t key_h = (uint32_t)va.next_gp();
        std::string key = resolve_jstring(key_h, memory);
        std::string val = pref_get(key, "false");
        res = (val == "true" || val == "1") ? 1 : 0;
        if (!emu->quiet_mode)
            std::cout << "[Prefs] getBooleanFromSP(" << key << ") -> " << res << std::endl;
    } else {
        res = 0;
    }
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticBooleanMethodV(mid=0x" << std::hex << mid << ") -> " << res << std::dec << std::endl;
    }
    emu->set_reg(0, res);
}

static void bridge_CallStaticIntMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    int res = 0;
    if (mid == 0x13180001) { // getPlatformConsentState — return 3 (OBTAINED)
        res = 3;
    } else if (mid == 0x13280003) { // getIntFromSP(String key)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_ptr);
        uint32_t key_h = (uint32_t)va.next_gp();
        std::string key = resolve_jstring(key_h, memory);
        std::string val = pref_get(key, "0");
        try { res = std::stoi(val); } catch (...) { res = 0; }
        if (!emu->quiet_mode)
            std::cout << "[Prefs] getIntFromSP(" << key << ") -> " << res << std::endl;
    } else if (mid == 0x13170002) { // enteredAge — forward to getIntFromSP("knownAge")
        res = std::stoi(pref_get("knownAge", "25"));
    }
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticIntMethodV(mid=0x" << std::hex << mid << ") -> " << std::dec << res << std::endl;
    }
    emu->set_reg(0, res);
}

static void bridge_CallStaticLongMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mid = (uint32_t)emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticLongMethodV(mid=0x" << std::hex << mid << ") -> 0" << std::dec << std::endl;
    }
    // ARM64: 64-bit return value fits in single X0
    emu->set_reg(0, 0);
}

static void bridge_CallStaticFloatMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mid = (uint32_t)emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticFloatMethodV(mid=0x" << std::hex << mid << ") -> 0" << std::dec << std::endl;
    }
    // ARM64: float return in S0
    emu->set_sreg(0, 0.0f);
}

static void bridge_CallStaticVoidMethodV(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    if (!emu->quiet_mode && mid != 0x56780001) {
        std::cout << "[JNI] CallStaticVoidMethodV(mid=0x" << std::hex << mid << ")" << std::dec << std::endl;
    }

    if (mid == 0x13170002) { // enteredAge
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_list_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_list_ptr);
        int age = (int)va.next_gp();
        g_saved_age = age;
        std::cout << "[JNI] enteredAge(" << age << ") -> Saved!" << std::endl;
    } else if (mid == 0x13170007) { // receivedPrivacyConsent
        std::cout << "[JNI] receivedPrivacyConsent() -> Stubbed!" << std::endl;
    } else if (mid == 0x13250001) { // loadSnapshot
        // Resolve the snapshot NAME (save slot) — it was ignored before, which made
        // every slot map to the single snapshot.bin and broke new-save-slot creation.
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_list_ptr = emu->get_reg(3);
        Arm64VaList va_load(memory, va_list_ptr);
        uint32_t name_ref_load = (uint32_t)va_load.next_gp();
        std::string snap_name = resolve_jstring(name_ref_load, memory);
        std::string snap_path = snapshot_path_for_name(snap_name);
        std::string snap_path_legacy = g_save_dir + "/snapshot.bin";
        std::string snap_path_doc = snapshot_documents_path_for_name(snap_name);
        std::string actual_path = snap_path;
        if (!fs::exists(actual_path) && !snap_name.empty() && fs::exists(snap_path_doc))
            actual_path = snap_path_doc;           // .gplayer mirror written by saveSnapshot
        if (!fs::exists(actual_path)) actual_path = snap_path_legacy; // legacy boot resume
        std::cout << "[SAVE] loadSnapshot(name=\"" << snap_name << "\") -> Posting async load for " << actual_path << std::endl;
        io_thread_post_load(actual_path, [](bool ok, std::vector<uint8_t> data) {
            g_snapshot_load_pending_count++;
            g_snapshot_has_data = ok;
            if (ok) {
                g_snapshot_data = std::move(data);
                std::cout << "[SAVE] Loaded " << g_snapshot_data.size() << " bytes via async thread" << std::endl;
            } else {
                g_snapshot_data.clear();
                std::cout << "[SAVE] Async load completed (no save data or error)" << std::endl;
            }
        });
    } else if (mid == 0x13250002) { // saveSnapshot
        // Extract save data from JNI args: saveSnapshot(String name, byte[] data)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_list_ptr = emu->get_reg(3);
        Arm64VaList va2(memory, va_list_ptr);
        uint32_t name_ref = (uint32_t)va2.next_gp();
        uint32_t data_ref = (uint32_t)va2.next_gp();
        std::string snap_name = resolve_jstring(name_ref, memory);
        std::cout << "[SAVE] saveSnapshot(name=\"" << snap_name << "\""
                  << ", data=0x" << std::hex << data_ref << ")" << std::dec << std::endl;
        
        if (data_ref != 0) {
            uint32_t array_len = *(uint32_t*)(memory + data_ref);
            uint8_t* array_data = memory + data_ref + 4;
            
            if (array_len > 0 && array_len < 0x1000000) {
                std::vector<uint8_t> data_to_save(array_data, array_data + array_len);
                // Persist the slot under its NAME (not just the shared snapshot.bin) so
                // a newly created save slot keeps its identity and survives a menu return.
                std::string snap_path = snapshot_path_for_name(snap_name);
                io_thread_post_save(snap_path, data_to_save);
                if (!snap_name.empty()) {
                    // Mirror into Documents/<name>.gplayer (same PlayerProfile protobuf format)
                    // so the in-game save screen, which scans Documents/.gplayer, sees the slot.
                    std::string doc_path = snapshot_documents_path_for_name(snap_name);
                    try { fs::create_directories(fs::path(doc_path).parent_path()); } catch (...) {}
                    std::vector<uint8_t> doc_copy = data_to_save;
                    io_thread_post_save(doc_path, std::move(doc_copy));
                }
                // Keep legacy mirror for boot auto-resume compatibility.
                std::string snap_legacy = g_save_dir + "/snapshot.bin";
                std::vector<uint8_t> legacy_copy = data_to_save;
                io_thread_post_save(snap_legacy, std::move(legacy_copy));
                std::cout << "[SAVE] Wrote " << array_len << " bytes asynchronously via IO thread" << std::endl;
            }
        }
    } else if (mid == 0x13250003) { // deleteSnapshot
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_list_ptr = emu->get_reg(3);
        Arm64VaList va3(memory, va_list_ptr);
        uint32_t name_ref_del = (uint32_t)va3.next_gp();
        std::string snap_name_del = resolve_jstring(name_ref_del, memory);
        std::string snap_path_del = snapshot_path_for_name(snap_name_del);
        if (snap_name_del.empty()) {
            remove((g_save_dir + "/snapshot.bin").c_str());
        } else {
            remove(snap_path_del.c_str());
            remove(snapshot_documents_path_for_name(snap_name_del).c_str());
        }
        std::cout << "[SAVE] Deleted snapshot \"" << snap_name_del << "\"" << std::endl;
    } else if (mid == 0x13270001) { // startAdsAndAnalytics
        std::cout << "[JNI] startAdsAndAnalytics() -> Stubbed!" << std::endl;
    } else if (mid == 0x13280002) { // saveBooleanInSP(String key, boolean val)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_ptr);
        uint32_t key_h = (uint32_t)va.next_gp();
        uint32_t bval  = (uint32_t)va.next_gp();
        std::string key = resolve_jstring(key_h, memory);
        pref_set(key, bval ? "true" : "false");
    } else if (mid == 0x13280004) { // saveIntInSP(String key, int val)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_ptr);
        uint32_t key_h = (uint32_t)va.next_gp();
        int ival       = (int)va.next_gp();
        std::string key = resolve_jstring(key_h, memory);
        pref_set(key, std::to_string(ival));
    } else if (mid == 0x13280006) { // saveLongInSP(String key, long val)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_ptr);
        uint32_t key_h = (uint32_t)va.next_gp();
        // ARM64: long is a single 8-byte value in one register slot
        int64_t lval = (int64_t)va.next_gp();
        std::string key = resolve_jstring(key_h, memory);
        pref_set(key, std::to_string(lval));
    } else if (mid == 0x13290001) { // startTextInput(String initialText)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        Arm64VaList va(memory, va_ptr);
        uint32_t str_h = (uint32_t)va.next_gp();
        std::string initial = resolve_jstring(str_h, memory);
        if (initial == "dummy_jni_string") initial = "";
        
        g_text_input_buffer = initial;
        g_text_input_active = true;
        std::cout << "[TextInput] startTextInput(\"" << initial 
                  << "\") — type on keyboard, Enter to confirm, Escape to cancel" << std::endl;
    } else if (mid == 0x13290002) { // stopTextInput
        g_text_input_active = false;
        g_text_input_buffer.clear();
        std::cout << "[TextInput] stopTextInput()" << std::endl;
    }
}

static void bridge_GetStaticFieldID(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_GetStaticObjectField(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_GetStringUTFLength(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t jstr = emu->get_reg(1);
    const char* str = (const char*)(memory + jstr);
    emu->set_reg(0, std::strlen(str));
}

static void bridge_ReleaseStringUTFChars(void* emu_ptr) {
}

static void bridge_GetArrayLength(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t array = emu->get_reg(1);
    if (array) {
        uint8_t* mem = emu->get_memory_base();
        uint32_t len = *(uint32_t*)(mem + array);
        emu->set_reg(0, len);
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_GetObjectArrayElement(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_NewIntArray(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

// Bump allocator for JNI byte arrays in guest memory
// Arrays are short-lived (created, filled, passed to saveSnapshot/etc, then discarded)
static uint64_t g_byte_array_alloc_ptr_64 = 0x48000000; // High address region for arrays

static void bridge_NewByteArray(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t length = emu->get_reg(1); // X1 = size
    
    if (length == 0 || length > 0x1000000) { // Sanity check (max 16MB)
        std::cout << "[JNI] NewByteArray(" << length << ") -> 0 (invalid size)" << std::endl;
        emu->set_reg(0, 0);
        return;
    }

    // Layout: [uint32_t length][byte data...] — matches GetByteArrayElements expectation
    uint64_t array_addr = g_byte_array_alloc_ptr_64;
    uint8_t* memory = emu->get_memory_base();
    *(uint32_t*)(memory + array_addr) = (uint32_t)length;
    // Zero-initialize the data region
    memset(memory + array_addr + 4, 0, length);
    // Bump allocator forward (align to 8 bytes)
    g_byte_array_alloc_ptr_64 += (4 + length + 7) & ~7ULL;
    // Wrap around if we go too far (reuse space)
    if (g_byte_array_alloc_ptr_64 > 0x4A000000) {
        g_byte_array_alloc_ptr_64 = 0x48000000;
    }

    std::cout << "[JNI] NewByteArray(" << length << ") -> 0x" << std::hex << array_addr << std::dec << std::endl;
    emu->set_reg(0, array_addr);
}

// SetByteArrayRegion(JNIEnv*, jbyteArray array, jsize start, jsize len, const jbyte* buf)
// Copies len bytes from buf into array[start..start+len]
static void bridge_SetByteArrayRegion(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t array = emu->get_reg(1);  // X1 = array
    int32_t start  = (int32_t)emu->get_reg(2);  // X2 = start offset
    int32_t len    = (int32_t)emu->get_reg(3);   // X3 = length
    uint64_t buf   = emu->get_reg(4);  // X4 = source buffer

    if (array && buf && len > 0) {
        uint8_t* memory = emu->get_memory_base();
        // Array layout: [uint32_t length][byte data...]
        // Data starts at array + 4
        uint8_t* dst = memory + array + 4 + start;
        uint8_t* src = memory + buf;
        memcpy(dst, src, len);
        if (!emu->quiet_mode) {
            std::cout << "[JNI] SetByteArrayRegion(array=0x" << std::hex << array
                      << ", start=" << std::dec << start << ", len=" << len
                      << ", buf=0x" << std::hex << buf << ")" << std::dec << std::endl;
        }
    }
}

static void bridge_GetByteArrayElements(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t array = emu->get_reg(1);
    if (array) {
        // Array layout: [uint32_t length][byte data...] — return pointer past length
        uint64_t data_ptr = array + 4;
        std::cout << "[JNI] GetByteArrayElements(array=0x" << std::hex << array
                  << ") -> 0x" << data_ptr << std::dec << std::endl;
        emu->set_reg(0, data_ptr);
    } else {
        std::cout << "[JNI] GetByteArrayElements(null) -> 0" << std::endl;
        emu->set_reg(0, 0);
    }
}

static void bridge_ReleaseByteArrayElements(void* emu_ptr) {
    // No-op — bump allocator, nothing to release
}

static void bridge_GetIntArrayElements(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_ReleaseIntArrayElements(void* emu_ptr) {
}

static void bridge_SetIntArrayRegion(void* emu_ptr) {
}

static void bridge_GetStringUTFRegion(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t jstr = (uint32_t)emu->get_reg(1);
    uint32_t start = (uint32_t)emu->get_reg(2);
    uint32_t len = (uint32_t)emu->get_reg(3);
    // ARM64: 5th arg in X4
    uint32_t buf = (uint32_t)emu->get_reg(4);
    const char* str = (const char*)(memory + jstr);
    std::memcpy(memory + buf, str + start, len);
    memory[buf + len] = '\0';
}

// --- Additional C String Bridges ---
static void bridge_strchr(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    int c = (int)(emu->get_reg(1) & 0xFF);
    const char* s = (const char*)(memory + str);
    const char* result = std::strchr(s, c);
    if (result) {
        emu->set_reg(0, str + (uint32_t)(result - s));
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_strrchr(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    int c = (int)(emu->get_reg(1) & 0xFF);
    const char* s = (const char*)(memory + str);
    const char* result = std::strrchr(s, c);
    if (result) {
        emu->set_reg(0, str + (uint32_t)(result - s));
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_strcpy(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    std::strcpy((char*)(memory + dest), (const char*)(memory + src));
    emu->set_reg(0, dest);
}

static void bridge_strncpy(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    std::strncpy((char*)(memory + dest), (const char*)(memory + src), n);
    emu->set_reg(0, dest);
}

static void bridge_strcmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t s1 = emu->get_reg(0);
    uint64_t s2 = emu->get_reg(1);
    if (!s1 || !s2) {
        emu->set_reg(0, 0);
        return;
    }
    int result = std::strcmp((const char*)(memory + s1), (const char*)(memory + s2));
    emu->set_reg(0, (uint64_t)(int64_t)result);
}

static void bridge_strncmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t s1 = emu->get_reg(0);
    uint64_t s2 = emu->get_reg(1);
    uint64_t n = emu->get_reg(2);
    if (!s1 || !s2 || !n) {
        emu->set_reg(0, 0);
        return;
    }
    int result = std::strncmp((const char*)(memory + s1), (const char*)(memory + s2), n);
    emu->set_reg(0, (uint64_t)(int64_t)result);
}

static void bridge_strcat(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    std::strcat((char*)(memory + dest), (const char*)(memory + src));
    emu->set_reg(0, dest);
}

static void bridge_strstr(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t haystack = emu->get_reg(0);
    uint32_t needle = emu->get_reg(1);
    const char* h = (const char*)(memory + haystack);
    const char* n = (const char*)(memory + needle);
    const char* result = std::strstr(h, n);
    if (result) {
        emu->set_reg(0, haystack + (uint32_t)(result - h));
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_memchr_impl(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t ptr = emu->get_reg(0);
    int c = (int)(emu->get_reg(1) & 0xFF);
    uint32_t n = emu->get_reg(2);
    if (ptr == 0) {
        emu->set_reg(0, 0);
        return;
    }
    const void* result = std::memchr(memory + ptr, c, n);
    if (result) {
        emu->set_reg(0, ptr + (uint32_t)((const uint8_t*)result - (memory + ptr)));
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_strtol(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    int base = (int)emu->get_reg(2);
    char* endptr = nullptr;
    long result = std::strtol((const char*)(memory + str), &endptr, base);
    if (endptr_ptr && endptr) {
        uint64_t offset = (uint64_t)(endptr - (char*)(memory + str));
        *(uint64_t*)(memory + endptr_ptr) = (uint64_t)(str + offset);  // ARM64: 64-bit pointers!
    }
    emu->set_reg(0, (uint32_t)result);
}

static void bridge_strtoul(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    int base = (int)emu->get_reg(2);
    char* endptr = nullptr;
    unsigned long result = std::strtoul((const char*)(memory + str), &endptr, base);
    if (endptr_ptr && endptr) {
        uint64_t offset = (uint64_t)(endptr - (char*)(memory + str));
        *(uint64_t*)(memory + endptr_ptr) = (uint64_t)(str + offset);  // ARM64: 64-bit pointers!
    }
    emu->set_reg(0, (uint32_t)result);
}

static void bridge_atoi(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    emu->set_reg(0, (uint32_t)std::atoi((const char*)(memory + str)));
}

static void bridge_atof(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = (uint32_t)emu->get_reg(0);
    const char* str = (const char*)(memory + str_ptr);
    double res = std::atof(str);
    // atof returns double; ARM64 returns in D0
    emu->set_dreg(0, res);
}

// --- Wide Character / CType Bridges ---
#include <cwchar>
static void bridge_wctob(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    wint_t wc = (wint_t)emu->get_reg(0);
    emu->set_reg(0, (uint32_t)std::wctob(wc));
}

static void bridge_btowc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, (uint32_t)std::btowc(c));
}

static void bridge_ctype_cur_max(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 1); // Return 1 for MB_CUR_MAX (simple ASCII/UTF-8 single byte for now)
}

// --- ctype character classification bridges ---
static void bridge_iscntrl(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, iscntrl(c) ? 1 : 0);
}

static void bridge_isprint(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isprint(c) ? 1 : 0);
}

static void bridge_isgraph(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isgraph(c) ? 1 : 0);
}

static void bridge_ispunct(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, ispunct(c) ? 1 : 0);
}

static void bridge_isxdigit(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isxdigit(c) ? 1 : 0);
}

static void bridge_isblank(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isblank(c) ? 1 : 0);
}

static void bridge_isalpha(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isalpha(c) ? 1 : 0);
}

static void bridge_isdigit(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isdigit(c) ? 1 : 0);
}

static void bridge_isalnum(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isalnum(c) ? 1 : 0);
}

static void bridge_isspace(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isspace(c) ? 1 : 0);
}

static void bridge_isupper(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, isupper(c) ? 1 : 0);
}

static void bridge_islower(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, islower(c) ? 1 : 0);
}

static void bridge_toupper(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, (uint64_t)toupper(c));
}

static void bridge_tolower(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, (uint64_t)tolower(c));
}

// --- wide character bridges ---
#include <wctype.h>
static void bridge_wctype(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(0);
    const char* name = (const char*)(memory + name_ptr);
    wctype_t result = wctype(name);
    emu->set_reg(0, (uint64_t)result);
}

static void bridge_iswctype(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    wint_t wc = (wint_t)emu->get_reg(0);
    wctype_t desc = (wctype_t)emu->get_reg(1);
    emu->set_reg(0, iswctype(wc, desc) ? 1 : 0);
}

static void bridge_towupper(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    wint_t wc = (wint_t)emu->get_reg(0);
    emu->set_reg(0, (uint64_t)towupper(wc));
}

static void bridge_towlower(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    wint_t wc = (wint_t)emu->get_reg(0);
    emu->set_reg(0, (uint64_t)towlower(wc));
}

// --- dl_iterate_phdr: needed for C++ exception unwinding ---
// Return 0 = success, tells the unwinder "no more shared objects"
static void bridge_dl_iterate_phdr(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

// --- __cxa_throw / __cxa_begin_catch / __cxa_end_catch ---
// Stub C++ exception handling to prevent abort
static void bridge_cxa_throw(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    std::cerr << "[Bridge64] C++ exception thrown (stubbed)" << std::endl;
    emu->set_reg(0, 0);
}

static void bridge_cxa_begin_catch(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, emu->get_reg(0)); // return exception object as-is
}

static void bridge_cxa_end_catch(void* emu_ptr) {
    // no-op
}

// --- setjmp/longjmp for Lua error handling ---
#include <csetjmp>

// setjmp: save callee-saved registers + SP + LR into guest jmp_buf
// ARM64 jmp_buf layout (Android/bionic):
//   [0]:  sigflag/mask
//   [8]:  X19
//   [16]: X20
//   ...
//   [88]: X29 (FP)
//   [96]: X30 (LR) — return address for setjmp
//   [104]: SP
//   [112]: D8 ... D15 (8 × 8 = 64 bytes)
// Total ~176 bytes. We use a simplified layout: save 13 GP regs + SP + 8 SIMD = 22 × 8 = 176 bytes.
static void bridge_setjmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t buf_ptr = emu->get_reg(0);  // jmp_buf pointer
    uint64_t* buf = (uint64_t*)(memory + buf_ptr);
    
    // Save callee-saved GP registers: X19-X29 (11 regs), LR (X30), SP
    buf[0] = 0;  // sigflag (unused)
    for (int i = 19; i <= 29; i++) {
        buf[i - 19 + 1] = emu->get_reg(i);  // buf[1] = X19, buf[2] = X20, ..., buf[11] = X29
    }
    buf[12] = emu->get_lr();    // LR (return address — where to resume on longjmp)
    buf[13] = emu->get_reg(31); // SP
    
    // Save callee-saved SIMD registers: D8-D15
    for (int i = 8; i <= 15; i++) {
        uint64_t dreg;
        // Read the double register as raw bits
        double d = emu->get_dreg(i);
        memcpy(&dreg, &d, sizeof(dreg));
        buf[14 + (i - 8)] = dreg;  // buf[14] = D8, ..., buf[21] = D15
    }
    
    // setjmp returns 0 on first call
    emu->set_reg(0, 0);
}

// longjmp: restore saved registers and jump back to setjmp return point
static void bridge_longjmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t buf_ptr = emu->get_reg(0);  // jmp_buf pointer
    int val = (int)emu->get_reg(1);      // return value
    if (val == 0) val = 1;               // longjmp must return non-zero
    
    uint64_t* buf = (uint64_t*)(memory + buf_ptr);
    
    // Restore callee-saved GP registers
    for (int i = 19; i <= 29; i++) {
        emu->set_reg(i, buf[i - 19 + 1]);
    }
    uint64_t saved_lr = buf[12];
    uint64_t saved_sp = buf[13];
    
    // Restore SP
    emu->set_reg(31, saved_sp);
    
    // Restore callee-saved SIMD registers: D8-D15
    for (int i = 8; i <= 15; i++) {
        double d;
        uint64_t dreg = buf[14 + (i - 8)];
        memcpy(&d, &dreg, sizeof(d));
        emu->set_dreg(i, d);
    }
    
    // Set return value (setjmp will appear to return this value)
    emu->set_reg(0, (uint64_t)val);
    
    // Use redirect_pc so handle_bridge_call doesn't overwrite our PC!
    // set_pc() alone gets clobbered by handle_bridge_call's "set_pc(lr)" afterward.
    emu->redirect_pc = saved_lr;
    
    std::cerr << "[Bridge64] longjmp -> resuming at 0x" << std::hex << saved_lr 
              << " with val=" << std::dec << val << std::endl;
}

// --- strerror ---
static void bridge_strerror(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // Return a pointer to a static "unknown error" string in guest memory
    static uint32_t err_addr = 0;
    if (err_addr == 0) {
        err_addr = g_guest_heap_ptr;
        g_guest_heap_ptr += 32;
        const char* msg = "unknown error";
        memcpy(emu->get_memory_base() + err_addr, msg, strlen(msg) + 1);
    }
    emu->set_reg(0, err_addr);
}

// --- sscanf (simplified) ---
static void bridge_sscanf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    uint32_t fmt_ptr = emu->get_reg(1);
    const char* str = (const char*)(memory + str_ptr);
    const char* fmt = (const char*)(memory + fmt_ptr);
    // Simple case: "%d" reading single int
    if (strcmp(fmt, "%d") == 0 || strcmp(fmt, "%i") == 0) {
        uint32_t out_ptr = emu->get_reg(2);
        int val = 0;
        int result = sscanf(str, fmt, &val);
        if (result > 0) {
            *(int*)(memory + out_ptr) = val;
        }
        emu->set_reg(0, result);
    } else if (strcmp(fmt, "%f") == 0) {
        uint32_t out_ptr = emu->get_reg(2);
        float val = 0;
        int result = sscanf(str, fmt, &val);
        if (result > 0) {
            *(float*)(memory + out_ptr) = val;
        }
        emu->set_reg(0, result);
    } else {
        // Unsupported format
        emu->set_reg(0, 0);
    }
}

// --- sprintf / snprintf / vsprintf / fprintf / printf ---
// These are CRITICAL for Lua's internal error messages and string formatting

static std::string format_guest_string(IEmulatorArm64* emu, const char* fmt, int start_arg_idx) {
    uint8_t* memory = emu->get_memory_base();
    std::string res;
    int reg_idx = start_arg_idx;
    int dreg_idx = 0;
    int stack_offset = 0;
    
    auto get_next_arg = [&]() -> uint64_t {
        if (reg_idx < 8) {
            return emu->get_reg(reg_idx++);
        } else {
            uint64_t sp = emu->get_reg(31); // SP
            uint64_t val = *(uint64_t*)(memory + sp + stack_offset);
            stack_offset += 8;
            return val;
        }
    };
    
    auto get_next_dreg = [&]() -> double {
        if (dreg_idx < 8) {
            return emu->get_dreg(dreg_idx++);
        }
        return 0.0;
    };

    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1) != '\0') {
            fmt++;
            char spec = *fmt;
            std::string width_prec = "%";
            while ((spec >= '0' && spec <= '9') || spec == '.' || spec == '-' || spec == '+') {
                width_prec += spec;
                fmt++;
                spec = *fmt;
            }
            width_prec += spec;
            
            if (spec == 's') {
                uint64_t ptr = get_next_arg();
                if (ptr) {
                    res += (const char*)(memory + ptr);
                } else {
                    res += "(null)";
                }
            } else if (spec == 'd' || spec == 'i') {
                int64_t val = (int64_t)(int32_t)get_next_arg();
                char buf[64];
                snprintf(buf, sizeof(buf), width_prec.c_str(), val);
                res += buf;
            } else if (spec == 'u') {
                uint64_t val = (uint32_t)get_next_arg();
                char buf[64];
                snprintf(buf, sizeof(buf), width_prec.c_str(), val);
                res += buf;
            } else if (spec == 'x' || spec == 'X') {
                uint64_t val = get_next_arg();
                char buf[64];
                snprintf(buf, sizeof(buf), width_prec.c_str(), val);
                res += buf;
            } else if (spec == 'f' || spec == 'g' || spec == 'G' || spec == 'e' || spec == 'E') {
                double val = get_next_dreg();
                char buf[64];
                snprintf(buf, sizeof(buf), width_prec.c_str(), val);
                res += buf;
            } else if (spec == '%') {
                res += '%';
            } else {
                res += '%';
                res += spec;
            }
        } else {
            res += *fmt;
        }
        fmt++;
    }
    return res;
}

static void bridge_sprintf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = emu->get_reg(0);
    uint32_t fmt_ptr = emu->get_reg(1);
    char* dst = (char*)(memory + dst_ptr);
    const char* fmt = (const char*)(memory + fmt_ptr);
    
    std::string formatted = format_guest_string(emu, fmt, 2);
    int n = sprintf(dst, "%s", formatted.c_str());
    emu->set_reg(0, n);
}

static void bridge_snprintf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = emu->get_reg(0);
    uint64_t maxlen = emu->get_reg(1);
    uint32_t fmt_ptr = emu->get_reg(2);
    char* dst = (char*)(memory + dst_ptr);
    const char* fmt = (const char*)(memory + fmt_ptr);
    
    std::string formatted = format_guest_string(emu, fmt, 3);
    int n = snprintf(dst, maxlen, "%s", formatted.c_str());
    emu->set_reg(0, n);
}

static void bridge_fprintf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t fmt_ptr = emu->get_reg(1);
    const char* fmt = (const char*)(memory + fmt_ptr);
    std::string formatted = format_guest_string(emu, fmt, 2);
    std::cerr << "[fprintf] " << formatted;
    emu->set_reg(0, formatted.length());
}

static void bridge_printf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t fmt_ptr = emu->get_reg(0);
    const char* fmt = (const char*)(memory + fmt_ptr);
    std::string formatted = format_guest_string(emu, fmt, 1);
    std::cout << "[printf] " << formatted;
    emu->set_reg(0, formatted.length());
}

static void bridge_fputs(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    const char* str = (const char*)(memory + str_ptr);
    std::cerr << "[GUEST fputs] " << str;
    emu->set_reg(0, 0); // success
}

static void bridge_fputc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, c); // return the char
}

static void bridge_puts(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    const char* str = (const char*)(memory + str_ptr);
    std::cout << str << std::endl;
    emu->set_reg(0, 0);
}

static void bridge_getc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, (uint64_t)-1); // EOF
}

static void bridge_ungetc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, emu->get_reg(0)); // return the char
}

static void bridge_feof(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // not EOF
}

static void bridge_ferror(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // no error
}

static void bridge_fileno(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 1); // return stdout fd
}

static void bridge_setvbuf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // success
}

// __sF: stdio FILE* array. On Android/bionic, __sF is an array of 3 FILE structs
// for stdin, stdout, stderr. We allocate fake FILE structs in guest memory.
static uint32_t g_sF_addr = 0;
static void bridge__sF(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_sF_addr == 0) {
        g_sF_addr = g_guest_heap_ptr;
        g_guest_heap_ptr += 512; // 3 fake FILE structs
        memset(emu->get_memory_base() + g_sF_addr, 0, 512);
    }
    emu->set_reg(0, g_sF_addr);
}

// __errno: return pointer to errno in guest memory
static uint32_t g_errno_addr = 0;
static void bridge__errno(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_errno_addr == 0) {
        g_errno_addr = g_guest_heap_ptr;
        g_guest_heap_ptr += 8;
        *(int*)(emu->get_memory_base() + g_errno_addr) = 0;
    }
    emu->set_reg(0, g_errno_addr);
}

// __stack_chk_fail: stack canary check. Just log and continue.
static void bridge__stack_chk_fail(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    std::cerr << "[WARN] __stack_chk_fail at PC=0x" << std::hex << emu->get_pc() << std::dec << std::endl;
    // Don't abort — just continue
}

static void bridge_setlocale(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // Return a pointer to "C" locale string in guest memory
    static uint32_t locale_addr = 0;
    if (locale_addr == 0) {
        locale_addr = g_guest_heap_ptr;
        g_guest_heap_ptr += 8;
        memcpy(emu->get_memory_base() + locale_addr, "C\0", 2);
    }
    emu->set_reg(0, locale_addr);
}

static void bridge_rand(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, rand());
}

static void bridge_clock(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, (uint64_t)clock());
}

static void bridge_strtold(void* emu_ptr) {
    // strtold same as strtod for our purposes
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    const char* str = (const char*)(memory + str_ptr);
    char* endp = nullptr;
    double result = strtod(str, &endp);
    if (endptr_ptr != 0 && endp != nullptr) {
        uint64_t offset = (uint64_t)(endp - (char*)memory);
        *(uint64_t*)(memory + endptr_ptr) = offset;
    }
    emu->set_dreg(0, result);
}

static void bridge_remove(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t path_ptr = emu->get_reg(0);
    const char* path = (const char*)(memory + path_ptr);
    
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    }
    
    emu->set_reg(0, remove(resolved_path.c_str()));
}

static void bridge_vsprintf(void* emu_ptr) {
    // vsprintf with va_list is complex; just stub it
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = emu->get_reg(0);
    uint32_t fmt_ptr = emu->get_reg(1);
    const char* fmt = (const char*)(memory + fmt_ptr);
    strcpy((char*)(memory + dst_ptr), fmt);
    emu->set_reg(0, strlen(fmt));
}

// --- strtod / strtof ---
static void bridge_strtod(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    const char* str = (const char*)(memory + str_ptr);
    char* endp = nullptr;
    double result = strtod(str, &endp);
    if (endptr_ptr != 0 && endp != nullptr) {
        uint64_t offset = (uint64_t)(endp - (char*)memory);
        *(uint64_t*)(memory + endptr_ptr) = offset;  // ARM64: 64-bit pointers!
    }
    // ARM64 returns double in D0
    emu->set_dreg(0, result);
}

static void bridge_strtof(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    const char* str = (const char*)(memory + str_ptr);
    char* endp = nullptr;
    float result = strtof(str, &endp);
    if (endptr_ptr != 0 && endp != nullptr) {
        uint64_t offset = (uint64_t)(endp - (char*)memory);
        *(uint64_t*)(memory + endptr_ptr) = offset;  // ARM64: 64-bit pointers!
    }
    emu->set_sreg(0, result);
}

// --- pthread TLS (Thread-Local Storage) ---
static std::unordered_map<uint32_t, uint64_t> g_tls_values;
static uint32_t g_next_tls_key = 1;

static void bridge_pthread_key_create(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t key_ptr = emu->get_reg(0);
    // Allocate a TLS key
    uint32_t key = g_next_tls_key++;
    *(uint32_t*)(memory + key_ptr) = key;
    g_tls_values[key] = 0;
    emu->set_reg(0, 0); // success
}

static void bridge_pthread_key_delete(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t key = emu->get_reg(0);
    g_tls_values.erase(key);
    emu->set_reg(0, 0);
}

static void bridge_pthread_getspecific(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t key = emu->get_reg(0);
    uint64_t val = g_tls_values.count(key) ? g_tls_values[key] : 0;
    emu->set_reg(0, val);
}

static void bridge_pthread_setspecific(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t key = emu->get_reg(0);
    uint64_t val = emu->get_reg(1);
    g_tls_values[key] = val;
    emu->set_reg(0, 0);
}

// --- nanosleep ---
static void bridge_nanosleep(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t req_ptr = emu->get_reg(0);
    
    if (req_ptr) {
        int64_t tv_sec = *(int64_t*)(memory + req_ptr);
        int64_t tv_nsec = *(int64_t*)(memory + req_ptr + 8);
        
        // Cap sleep to 100ms max to avoid blocking the game
        long total_ns = tv_sec * 1000000000L + tv_nsec;
        if (total_ns > 100000000L) total_ns = 100000000L; // 100ms max
        
        struct timespec ts;
        ts.tv_sec = total_ns / 1000000000L;
        ts.tv_nsec = total_ns % 1000000000L;
        nanosleep(&ts, nullptr);
    }
    emu->set_reg(0, 0);
}

// --- sched_yield ---
static void bridge_sched_yield(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    std::this_thread::yield();
    emu->set_reg(0, 0);
}

#include <zlib.h>

// --- File I/O Bridges ---
static std::unordered_map<uint32_t, FILE*> g_file_handles;
static std::unordered_map<uint32_t, gzFile> g_gz_handles;
static uint32_t g_next_file_handle = 0x70000001;

// PVR-as-gzip: when gzdopen is called on a PVR file fd, we decode the PVR
// pixels immediately and serve them from this buffer during gzread calls.
struct PVRGzBuffer {
    std::vector<uint8_t> pixels;  // decoded RGBA pixels
    uint32_t offset;              // read cursor
    int width, height;
};
static std::unordered_map<uint32_t, PVRGzBuffer> g_pvr_gz_handles;

// Forward declarations — full definitions appear later in this file
// (HostPVRv3Header and HostPVRv2Header are defined around bridge_PVRTTextureLoadFromPVRBuffer)
static void decode_etc1_block(const uint8_t* src, uint8_t* dst, int dst_stride);

// PVR header structs needed by try_decode_pvr_from_fd (duplicated here for
// forward use; the canonical #pragma pack definitions come later)
#pragma pack(push, 1)
struct HostPVRv3Header {
    uint32_t version; uint32_t flags; uint64_t pixel_format;
    uint32_t color_space; uint32_t channel_type;
    uint32_t height; uint32_t width; uint32_t depth;
    uint32_t num_surfaces; uint32_t num_faces;
    uint32_t mip_count; uint32_t metadata_size;
};
struct HostPVRv2Header {
    uint32_t header_size; uint32_t height; uint32_t width;
    uint32_t mip_count; uint32_t flags; uint32_t data_size;
    uint32_t bpp; uint32_t mask_r; uint32_t mask_g;
    uint32_t mask_b; uint32_t mask_a; uint32_t magic; uint32_t num_surfaces;
};
#pragma pack(pop)


static void bridge_lseek(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int fd = emu->get_reg(0);
    off_t offset = (off_t)emu->get_reg(1);
    int whence = emu->get_reg(2);
    
    // Check if it's one of our asset fds or standard handles
    if (g_file_handles.count(fd)) {
        int res = fseek(g_file_handles[fd], offset, whence);
        emu->set_reg(0, res == 0 ? ftell(g_file_handles[fd]) : -1);
    } else {
        off_t res = lseek(fd, offset, whence);
        emu->set_reg(0, (uint32_t)res);
    }
}

static bool decompress_gzip_buffer(const std::vector<uint8_t>& in_buf, std::vector<uint8_t>& out_buf) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) return false;
    
    strm.next_in = const_cast<uint8_t*>(in_buf.data());
    strm.avail_in = in_buf.size();
    
    uint8_t temp[16384];
    int ret;
    do {
        strm.next_out = temp;
        strm.avail_out = sizeof(temp);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return false;
        }
        size_t decoded = sizeof(temp) - strm.avail_out;
        if (decoded > 0) {
            out_buf.insert(out_buf.end(), temp, temp + decoded);
        }
    } while (ret != Z_STREAM_END && strm.avail_in > 0);
    
    inflateEnd(&strm);
    return (ret == Z_STREAM_END);
}

static bool parse_pvr_uncompressed_format(uint64_t pixel_format, GLenum& gl_format, GLenum& gl_type, int& bpp, int& caver_format) {
    uint32_t fmt_high = pixel_format >> 32;
    if (fmt_high == 0) {
        return false; // Compressed format
    }
    
    char c1 = (char)(pixel_format & 0xFF);
    char c2 = (char)((pixel_format >> 8) & 0xFF);
    char c3 = (char)((pixel_format >> 16) & 0xFF);
    char c4 = (char)((pixel_format >> 24) & 0xFF);
    
    int b1 = (int)((pixel_format >> 32) & 0xFF);
    int b2 = (int)((pixel_format >> 40) & 0xFF);
    int b3 = (int)((pixel_format >> 48) & 0xFF);
    int b4 = (int)((pixel_format >> 56) & 0xFF);
    
    bpp = (b1 + b2 + b3 + b4) / 8;
    gl_type = GL_UNSIGNED_BYTE;
    
    if (c1 == 'r' && c2 == 'g' && c3 == 'b' && c4 == 'a' && b1 == 8 && b2 == 8 && b3 == 8 && b4 == 8) {
        gl_format = GL_RGBA; caver_format = 1; return true;
    }
    if (c1 == 'b' && c2 == 'g' && c3 == 'r' && c4 == 'a' && b1 == 8 && b2 == 8 && b3 == 8 && b4 == 8) {
        gl_format = GL_BGRA; caver_format = 1; return true;
    }
    if (c1 == 'l' && c2 == 'a' && b1 == 8 && b2 == 8) {
        gl_format = GL_LUMINANCE_ALPHA; caver_format = 4; return true;
    }
    if (c1 == 'l' && b1 == 8) {
        gl_format = GL_LUMINANCE; caver_format = 6; return true;
    }
    if (c1 == 'a' && b1 == 8) {
        gl_format = GL_ALPHA; caver_format = 7; return true;
    }
    if (c1 == 'r' && c2 == 'g' && c3 == 'b' && b1 == 5 && b2 == 6 && b3 == 5) {
        gl_format = GL_RGB; gl_type = GL_UNSIGNED_SHORT_5_6_5; bpp = 2; caver_format = 5; return true;
    }
    if (c1 == 'r' && c2 == 'g' && c3 == 'b' && c4 == 'a' && b1 == 4 && b2 == 4 && b3 == 4 && b4 == 4) {
        gl_format = GL_RGBA; gl_type = GL_UNSIGNED_SHORT_4_4_4_4; bpp = 2; caver_format = 2; return true;
    }
    if (c1 == 'r' && c2 == 'g' && c3 == 'b' && c4 == 'a' && b1 == 5 && b2 == 5 && b3 == 5 && b4 == 1) {
        gl_format = GL_RGBA; gl_type = GL_UNSIGNED_SHORT_5_5_5_1; bpp = 2; caver_format = 3; return true;
    }
    
    // Fallback
    gl_format = GL_RGBA; bpp = 4; caver_format = 1;
    return true;
}

static bool parse_legacy_pvr_uncompressed_format(uint32_t flags, GLenum& gl_format, GLenum& gl_type, int& bpp, int& caver_format) {
    uint32_t fmt = flags & 0xFF;
    gl_type = GL_UNSIGNED_BYTE;
    
    switch (fmt) {
        case 0x12: // OGL_RGBA_8888
        case 0x91: // VG_sRGBA_8888
        case 0x92: // VG_sRGBA_8888_PRE
        case 0x98: // VG_lRGBA_8888
        case 0x99: // VG_lRGBA_8888_PRE
            gl_format = GL_RGBA; bpp = 4; caver_format = 1; return true;
            
        case 0x9c: // VG_sXRGB_8888
        case 0x9d: // VG_sARGB_8888
        case 0xa1: // VG_lXRGB_8888
        case 0xa2: // VG_lARGB_8888
            gl_format = GL_RGBA; bpp = 4; caver_format = 1; return true;
            
        case 0xa4: // VG_sBGRX_8888
        case 0xa5: // VG_sBGRA_8888
        case 0xab: // VG_lBGRX_8888
        case 0xac: // VG_lBGRA_8888
            gl_format = GL_BGRA; bpp = 4; caver_format = 1; return true;
            
        case 0xb4: // VG_sABGR_8888
        case 0xb9: // VG_lABGR_8888
            gl_format = GL_RGBA; bpp = 4; caver_format = 1; return true;
            
        case 0x93: // VG_sRGB_565
        case 0xa7: // VG_sBGR_565
        case 0x15: // OGL_RGB_565
            gl_format = GL_RGB; gl_type = GL_UNSIGNED_SHORT_5_6_5; bpp = 2; caver_format = 5; return true;
            
        case 0x95: // VG_sRGBA_4444
        case 0xaa: // VG_sBGRA_4444
        case 0x14: // OGL_RGBA_4444
            gl_format = GL_RGBA; gl_type = GL_UNSIGNED_SHORT_4_4_4_4; bpp = 2; caver_format = 2; return true;
            
        case 0x94: // VG_sRGBA_5551
        case 0xa9: // VG_sBGRA_5551
        case 0x13: // OGL_RGBA_5551
            gl_format = GL_RGBA; gl_type = GL_UNSIGNED_SHORT_5_5_5_1; bpp = 2; caver_format = 3; return true;
            
        case 0x96: // VG_sL_8
        case 0x9a: // VG_lL_8
        case 0x16: // OGL_I_8 (Intensity 8)
            gl_format = GL_LUMINANCE; bpp = 1; caver_format = 6; return true;
            
        case 0x9b: // VG_A_8
        case 0x17: // OGL_A_8 (Alpha 8)
            gl_format = GL_ALPHA; bpp = 1; caver_format = 7; return true;
            
        default:
            return false; // Compressed format or unknown
    }
}

/* The old cache key used only the file size and the first 64 bytes.  Many
 * Swordigo textures share identical PVR headers, so that key aliases unrelated
 * textures and produces random sprites/atlas glyphs after a scene transition.
 * Include the complete input in the key and protect the map because asset
 * loads can be initiated by more than one guest/host path. */
static std::unordered_map<uint64_t, PVRGzBuffer> g_pvr_decomp_cache;
static std::mutex g_pvr_decomp_cache_mutex;

static uint64_t pvr_cache_key(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    hash ^= (uint64_t)size + 0x9e3779b97f4a7c15ULL;
    hash *= 1099511628211ULL;
    return hash;
}

static bool try_decode_pvr_from_fd(int real_fd, PVRGzBuffer& out) {
    // Save position, read data, restore
    off_t orig = lseek(real_fd, 0, SEEK_CUR);
    lseek(real_fd, 0, SEEK_END);
    off_t sz = lseek(real_fd, 0, SEEK_CUR);
    lseek(real_fd, 0, SEEK_SET);
    if (sz < 12) { lseek(real_fd, orig, SEEK_SET); return false; }
    std::vector<uint8_t> buf(sz);
    if (read(real_fd, buf.data(), sz) != sz) { lseek(real_fd, orig, SEEK_SET); return false; }
    lseek(real_fd, orig, SEEK_SET);

    uint64_t hash = pvr_cache_key(buf.data(), buf.size());
    {
        std::lock_guard<std::mutex> lock(g_pvr_decomp_cache_mutex);
        auto it = g_pvr_decomp_cache.find(hash);
        if (it != g_pvr_decomp_cache.end()) {
            out = it->second;
            out.offset = 0; // reset read position
            return true;
        }
    }

    std::vector<uint8_t> decompressed;
    const uint8_t* data = buf.data();
    size_t data_sz = sz;

    if (buf.size() >= 2 && buf[0] == 0x1f && buf[1] == 0x8b) {
        if (decompress_gzip_buffer(buf, decompressed)) {
            data = decompressed.data();
            data_sz = decompressed.size();
        } else {
            return false; // corrupt gzip
        }
    }

    int width = 0, height = 0;
    const uint8_t* pixel_data = nullptr;
    int format_type = -1;

    uint32_t gl_format = 0x1908; // GL_RGBA
    uint32_t gl_type = 0x1401; // GL_UNSIGNED_BYTE
    int bpp = 4;
    char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    uint8_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    int caver_format = 1;

    // PVR v3: magic 0x03525650 ('PVR\x03' LE)
    const HostPVRv3Header* v3 = (const HostPVRv3Header*)data;
    if (data_sz >= sizeof(HostPVRv3Header) && v3->version == 0x03525650) {
        width = v3->width; height = v3->height;
        size_t data_offset = sizeof(HostPVRv3Header) + (size_t)v3->metadata_size;
        if (data_offset > data_sz) return false;
        pixel_data = data + data_offset;
        format_type = pvr::ParsePVRv3Format(v3->pixel_format, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
    } else {
        // PVR v2: magic 0x21525650 ('PVR!')
        const HostPVRv2Header* v2 = (const HostPVRv2Header*)data;
        if (data_sz >= sizeof(HostPVRv2Header) && (v2->magic == 0x21525650 || v2->header_size == 44)) {
            width = v2->width; height = v2->height;
            if (v2->header_size < sizeof(HostPVRv2Header) || v2->header_size > data_sz)
                return false;
            pixel_data = data + v2->header_size;
            format_type = pvr::ParsePVRv2Format(v2->flags, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        } else {
            return false;  // not a PVR
        }
    }

    if (format_type < 0) return false;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    if (!pixel_data || pixel_data >= data + data_sz) return false;

    // Map format_type to caver_format
    if (format_type == 1) { // ETC1
        caver_format = 1;
    } else if (format_type == 2 || format_type == 3) { // PVRTC
        caver_format = 1;
    } else if (format_type >= 4 && format_type <= 6) { // DXT
        caver_format = 1;
    } else if (format_type == 10) { // Uncompressed
        if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 8 && d1 == 8 && d2 == 8 && d3 == 8) caver_format = 1;
        else if (c0 == 'b' && c1 == 'g' && c2 == 'r' && c3 == 'a' && d0 == 8 && d1 == 8 && d2 == 8 && d3 == 8) caver_format = 1;
        else if (c0 == 'l' && c1 == 'a' && d0 == 8 && d1 == 8) caver_format = 4;
        else if (c0 == 'l' && d0 == 8) caver_format = 6;
        else if (c0 == 'a' && d0 == 8) caver_format = 7;
        else if (c0 == 'r' && c1 == 'g' && c2 == 'b' && d0 == 5 && d1 == 6 && d2 == 5) caver_format = 5;
        else if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 4 && d1 == 4 && d2 == 4 && d3 == 4) caver_format = 2;
        else if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 5 && d1 == 5 && d2 == 5 && d3 == 1) caver_format = 3;
        else caver_format = 1;
    }

    std::vector<uint8_t> rgba;
    if (format_type == 10) { // Uncompressed: assign directly
        size_t required = (size_t)width * (size_t)height * (size_t)bpp;
        if (required > data_sz - (size_t)(pixel_data - data)) return false;
        rgba.assign(pixel_data, pixel_data + required);
        if (gl_format == 0x80E1) { // GL_BGRA -> swap to RGBA
            for (size_t i = 0; i < rgba.size(); i += 4) {
                uint8_t b = rgba[i];
                rgba[i] = rgba[i+2];
                rgba[i+2] = b;
            }
            caver_format = 1;
        }
    } else {
        // Decode to RGBA8888
        rgba.assign(width * height * 4, 0xFF);
        bool decode_success = false;
        if (format_type == 1) { // ETC1
            pvr::PVRTDecompressETC(pixel_data, width, height, rgba.data(), 6);
            decode_success = true;
        } else if (format_type == 2 || format_type == 3) { // PVRTC
            uint32_t do2bitMode = (format_type == 2) ? 1 : 0;
            pvr::PVRTDecompressPVRTC(pixel_data, do2bitMode, width, height, rgba.data());
            decode_success = true;
        } else if (format_type >= 4 && format_type <= 6) { // DXT
            uint32_t dxt_fmt = (format_type == 4) ? 1 : ((format_type == 5) ? 3 : 5);
            pvr::PVRTDecompressDXT(pixel_data, width, height, rgba.data(), dxt_fmt);
            decode_success = true;
        }
        if (!decode_success) return false;
    }

    out.pixels.resize(12 + rgba.size());
    uint32_t hdr[3] = { (uint32_t)caver_format, (uint32_t)width, (uint32_t)height };
    memcpy(out.pixels.data(),       hdr,       12);
    memcpy(out.pixels.data() + 12,  rgba.data(), rgba.size());
    out.offset = 0;
    out.width  = width;
    out.height = height;

    {
        std::lock_guard<std::mutex> lock(g_pvr_decomp_cache_mutex);
        g_pvr_decomp_cache.emplace(hash, out);
    }

    extern IEmulatorArm64* g_emulator_64;
    if (g_emulator_64 && !g_emulator_64->quiet_mode) {
        std::cerr << "[SRE-PVR] gzdopen: decoded PVR " << width << "x" << height
                  << " format=" << caver_format << " -> TEX stream (cached)" << std::endl;
    }
    return true;
}

static void bridge_gzdopen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int fd = emu->get_reg(0);
    // mode_ptr is get_reg(1) — we don't need to log it

    int real_fd = fd;
    if (g_file_handles.count(fd)) {
        real_fd = fileno(g_file_handles[fd]);
    }

    if (real_fd <= 0) {
        std::cerr << "[ZLIB] gzdopen: invalid fd " << real_fd << std::endl;
        emu->set_reg(0, 0);
        return;
    }

    // Try to decode as PVR first
    PVRGzBuffer pvr_buf;
    if (try_decode_pvr_from_fd(real_fd, pvr_buf)) {
        uint32_t handle = g_next_file_handle++;
        g_pvr_gz_handles[handle] = std::move(pvr_buf);
        emu->set_reg(0, handle);
        return;
    }

    // Not a PVR — open as real gzip
    int new_fd = dup(real_fd);
    uint32_t mode_ptr = emu->get_reg(1);
    const char* mode = (const char*)(memory + mode_ptr);
    gzFile gz = gzdopen(new_fd, mode);
    if (gz) {
        uint32_t handle = g_next_file_handle++;
        g_gz_handles[handle] = gz;
        emu->set_reg(0, handle);
    } else {
        if (new_fd >= 0) close(new_fd);
        std::cerr << "[ZLIB] gzdopen: gzdopen() failed on fd " << real_fd << std::endl;
        emu->set_reg(0, 0);
    }
}

static void bridge_gzread(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t handle = emu->get_reg(0);
    uint32_t buf = emu->get_reg(1);
    uint32_t len = emu->get_reg(2);

    // PVR decoded buffer path
    if (g_pvr_gz_handles.count(handle)) {
        PVRGzBuffer& pvr = g_pvr_gz_handles[handle];
        uint32_t avail = (uint32_t)pvr.pixels.size() - pvr.offset;
        uint32_t to_copy = (len < avail) ? len : avail;
        if (to_copy > 0) {
            memcpy(memory + buf, pvr.pixels.data() + pvr.offset, to_copy);
            pvr.offset += to_copy;
        }
        emu->set_reg(0, (uint32_t)to_copy);
        return;
    }

    // Real gzip path
    if (g_gz_handles.count(handle)) {
        int read = gzread(g_gz_handles[handle], memory + buf, len);
        if (read < 0) {
            int errnum;
            const char* errmsg = gzerror(g_gz_handles[handle], &errnum);
            std::cerr << "[ZLIB] gzread ERROR: " << errmsg << " (" << errnum << ")" << std::endl;
        }
        emu->set_reg(0, (uint32_t)read);
        return;
    }

    std::cerr << "[ZLIB] gzread: INVALID handle=0x" << std::hex << handle << std::dec << std::endl;
    emu->set_reg(0, (uint32_t)-1);
}

static void bridge_gzclose(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (g_pvr_gz_handles.count(handle)) {
        g_pvr_gz_handles.erase(handle);
        emu->set_reg(0, 0);
    } else if (g_gz_handles.count(handle)) {
        gzclose(g_gz_handles[handle]);
        g_gz_handles.erase(handle);
        emu->set_reg(0, 0);
    } else {
        emu->set_reg(0, (uint32_t)-1);
    }
}
static std::unordered_map<uint32_t, std::string> g_buffer_to_path;
static std::mutex g_buffer_to_path_mutex;

extern "C" bool resolve_vfs_path(const char* original_path, char* out_resolved_path, int max_len);

static void bridge_fopen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t path_ptr = emu->get_reg(0);
    uint32_t mode_ptr = emu->get_reg(1);
    const char* path = (const char*)(memory + path_ptr);
    const char* mode = (const char*)(memory + mode_ptr);

    // Resolve virtual paths using VFS translation first
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    } else if (path[0] != '/' && (strncmp(path, "resources/", 10) == 0 || strncmp(path, "assets/", 7) == 0)) {
        // Resolve relative asset paths to the absolute base directory
        resolved_path = std::string(get_assets_base_path()) + "/" + path;
    }

    bool is_write = (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
    if (is_write) {
        try {
            fs::create_directories(fs::path(resolved_path).parent_path());
        } catch (...) {
            // Ignore failure
        }
    }
    
    FILE* f = fopen(resolved_path.c_str(), mode);
    
    /* Fallback: if a file in external/ (e.g. external/saves, external/main.lua) does not exist,
     * auto-create an empty file so mod scripts don't loop endlessly waiting for it. */
    if (!f && resolved_path.find("/external/") != std::string::npos) {
        FILE* create_f = fopen(resolved_path.c_str(), "w+b");
        if (create_f) {
            fclose(create_f);
            f = fopen(resolved_path.c_str(), mode);
        }
    }

    if (f) {
        uint32_t handle = g_next_file_handle++;
        g_file_handles[handle] = f;
        RedstellGC::instance().track_file_open(handle, resolved_path.c_str());
        
        // PPD: Kick off asynchronous pre-decompression if it's a PVR texture
        if (resolved_path.find(".pvr") != std::string::npos) {
            RedstellGC::instance().ppd_preload_texture(resolved_path);
        }
        
        emu->set_reg(0, handle);
    } else {
        emu->set_reg(0, 0);
        if (!emu->quiet_mode) {
            printf("[AssetMgr/fopen] Failed to open: %s -> %s (mode=%s)\n", path, resolved_path.c_str(), mode);
        }
    }
}


static void bridge_fclose(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (g_file_handles.count(handle)) {
        RedstellGC::instance().track_file_close(handle);
        fflush(g_file_handles[handle]);  // Ensure data written to disk before close
        fclose(g_file_handles[handle]);
        g_file_handles.erase(handle);
        emu->set_reg(0, 0);
    } else {
        emu->set_reg(0, -1);
    }
}

static void bridge_fflush(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (handle == 0) {
        fflush(NULL); // flush all streams
        emu->set_reg(0, 0);
    } else if (g_file_handles.count(handle)) {
        emu->set_reg(0, fflush(g_file_handles[handle]));
    } else {
        emu->set_reg(0, -1);
    }
}

static void bridge_fread(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf = emu->get_reg(0);
    uint32_t elem_size = emu->get_reg(1);
    uint32_t count = emu->get_reg(2);
    uint32_t handle = emu->get_reg(3);
    if (g_file_handles.count(handle)) {
        size_t read = fread(memory + buf, elem_size, count, g_file_handles[handle]);
        
        // PPD: Associate buffer address with the texture file path
        std::string path = RedstellGC::instance().get_file_path(handle);
        if (!path.empty() && path.find(".pvr") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_buffer_to_path_mutex);
            g_buffer_to_path[buf] = path;
        }
        
        emu->set_reg(0, (uint32_t)read);
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_fwrite(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf = emu->get_reg(0);
    uint32_t elem_size = emu->get_reg(1);
    uint32_t count = emu->get_reg(2);
    uint32_t handle = emu->get_reg(3);
    if (g_file_handles.count(handle)) {
        size_t written = fwrite(memory + buf, elem_size, count, g_file_handles[handle]);
        emu->set_reg(0, (uint32_t)written);
        // std::cout << "[File] fwrite(handle=" << handle << ", size=" << elem_size 
        //           << ", count=" << count << ") -> wrote " << written << std::endl;
    } else {
        // Unknown handle (likely stderr/stdout guest pointers) — pretend success
        // to prevent cascading abort() calls
        emu->set_reg(0, count);
    }
}

static void bridge_fseek(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    int32_t offset = (int32_t)emu->get_reg(1);
    int whence = (int)emu->get_reg(2);
    if (g_file_handles.count(handle)) {
        emu->set_reg(0, (uint32_t)fseek(g_file_handles[handle], offset, whence));
    } else {
        emu->set_reg(0, (uint32_t)-1);
    }
}

static void bridge_ftell(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (g_file_handles.count(handle)) {
        emu->set_reg(0, (uint32_t)ftell(g_file_handles[handle]));
    } else {
        emu->set_reg(0, (uint32_t)-1);
    }
}

// fgets(char* s, int size, FILE* stream) → char* (or NULL)
static void bridge_fgets(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf_ptr = emu->get_reg(0);
    int size = (int)emu->get_reg(1);
    uint32_t handle = emu->get_reg(2);
    if (g_file_handles.count(handle) && buf_ptr != 0 && size > 0) {
        char* result = fgets((char*)(memory + buf_ptr), size, g_file_handles[handle]);
        emu->set_reg(0, result ? buf_ptr : 0);
    } else {
        emu->set_reg(0, 0);
    }
}

// fscanf(FILE* stream, const char* fmt, ...) — only supports "%lf" for SRE io.read("*n")
static void bridge_fscanf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t handle = emu->get_reg(0);
    uint32_t fmt_ptr = emu->get_reg(1);
    const char* fmt = (const char*)(memory + fmt_ptr);
    
    if (g_file_handles.count(handle) && fmt) {
        // We only support "%lf" (read double) which is what SRE's io uses
        if (strstr(fmt, "%lf") || strstr(fmt, "%f")) {
            uint32_t out_ptr = emu->get_reg(2);
            double val = 0.0;
            int result = fscanf(g_file_handles[handle], "%lf", &val);
            if (out_ptr != 0) {
                *(double*)(memory + out_ptr) = val;
            }
            emu->set_reg(0, (uint32_t)result);
        } else if (strstr(fmt, "%d") || strstr(fmt, "%i")) {
            uint32_t out_ptr = emu->get_reg(2);
            int val = 0;
            int result = fscanf(g_file_handles[handle], "%d", &val);
            if (out_ptr != 0) {
                *(int*)(memory + out_ptr) = val;
            }
            emu->set_reg(0, (uint32_t)result);
        } else if (strstr(fmt, "%s")) {
            uint32_t out_ptr = emu->get_reg(2);
            if (out_ptr != 0) {
                int result = fscanf(g_file_handles[handle], "%255s", (char*)(memory + out_ptr));
                emu->set_reg(0, (uint32_t)result);
            } else {
                emu->set_reg(0, 0);
            }
        } else {
            // Unsupported format — return 0 items read
            std::cout << "[File] fscanf: unsupported format \"" << fmt << "\"" << std::endl;
            emu->set_reg(0, 0);
        }
    } else {
        emu->set_reg(0, 0);
    }
}

// --- Directory / System Bridges ---
static void bridge_mkdir(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t path_ptr = emu->get_reg(0);
    uint64_t mode = emu->get_reg(1);
    const char* path = (const char*)(memory + path_ptr);
    if (!path || !*path) {
        emu->set_reg(0, 0);
        return;
    }
    
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    }

    // Fast path: cached created directory set eliminates repeated syscalls & log spam
    static std::unordered_set<std::string> s_created_dirs;
    if (s_created_dirs.find(resolved_path) != s_created_dirs.end()) {
        emu->set_reg(0, 0);
        return;
    }

    int result = mkdir(resolved_path.c_str(), (mode_t)mode);
    if (result == 0 || errno == EEXIST) {
        s_created_dirs.insert(resolved_path);
        result = 0;
    } else {
        std::cout << "[File] mkdir FAILED: \"" << resolved_path << "\": " << strerror(errno) << std::endl;
    }
    emu->set_reg(0, (uint64_t)(int64_t)result);
}

static void bridge_rename(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* old_path = (const char*)(memory + emu->get_reg(0));
    const char* new_path = (const char*)(memory + emu->get_reg(1));
    
    std::string resolved_old = old_path;
    std::string resolved_new = new_path;
    char resolved[512];
    if (resolve_vfs_path(old_path, resolved, sizeof(resolved))) {
        resolved_old = resolved;
    }
    if (resolve_vfs_path(new_path, resolved, sizeof(resolved))) {
        resolved_new = resolved;
    }

    // Ensure destination parent directory exists
    size_t last_slash = resolved_new.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string parent_dir = resolved_new.substr(0, last_slash);
        try {
            fs::create_directories(parent_dir);
        } catch (...) {}
    }

    int result = rename(resolved_old.c_str(), resolved_new.c_str());
    if (result != 0) {
        // Fallback: Copy and remove (cross-device/permissions fallback)
        try {
            if (fs::exists(resolved_old)) {
                fs::copy_file(resolved_old, resolved_new, fs::copy_options::overwrite_existing);
                fs::remove(resolved_old);
                result = 0; // Success!
                std::cout << "[File] rename fallback (copy+remove) succeeded for " << old_path << " -> " << new_path << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[File] rename fallback FAILED: " << e.what() << std::endl;
        }
    }

    if (result != 0) {
        std::cerr << "[File] rename(\"" << old_path << "\" -> \"" << new_path << "\") FAILED: " << strerror(errno) << std::endl;
    } else {
        std::cout << "[File] rename(\"" << old_path << "\" -> \"" << new_path << "\") OK" << std::endl;
        
        // Dynamically update SRE profile ID upon successful creation of a new save slot
        if (resolved_new.find(".gplayer") != std::string::npos && resolved_new.find("/save/Documents/") != std::string::npos) {
            try {
                fs::path p(resolved_new);
                std::string profile_id = p.stem().string();
                if (!profile_id.empty() && profile_id.find("_b") == std::string::npos) { // ignore backup saves
                    set_active_profile_id(profile_id);
                    uint8_t* memory = emu->get_memory_base();
                    if (g_sre_profile_addr) {
                        std::strncpy((char*)(memory + g_sre_profile_addr), profile_id.c_str(), 63);
                        ((char*)(memory + g_sre_profile_addr))[63] = '\0';
                    }
                    if (g_sre_vfs_profile_addr) {
                        std::strncpy((char*)(memory + g_sre_vfs_profile_addr), profile_id.c_str(), 63);
                        ((char*)(memory + g_sre_vfs_profile_addr))[63] = '\0';
                    }
                    std::cout << "[SRE/Rename] Dynamically updated active profile ID to: " << profile_id << std::endl;
                }
            } catch (const std::exception& e) {
                // ignore
            }
        }
    }
    emu->set_reg(0, (uint32_t)result);
}


static void bridge_access(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* path = (const char*)(memory + emu->get_reg(0));
    int mode = (int)emu->get_reg(1);
    
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    }

    int result = access(resolved_path.c_str(), mode);
    emu->set_reg(0, (uint32_t)result);
}

static void bridge_unlink(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* path = (const char*)(memory + emu->get_reg(0));
    
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    }

    int result = unlink(resolved_path.c_str());
    emu->set_reg(0, (uint32_t)result);
}

static int g_abort_count = 0;
static uint64_t g_last_abort_lr = 0;
static int g_same_lr_count = 0;
static int g_abort_reentry_depth = 0;  /* re-entrancy guard: how deep we are in abort recovery */
static uint64_t g_abort_last_target = 0;  /* last PC we redirected to — detect redirect loops */
static int g_abort_same_target_count = 0;

/* Validate a return address for use as an abort-recovery redirect target.
 * Must be 4-byte aligned, in .text [0x1000000, 0x3000000) not in bridge/cave.
 * This prevents redirecting into destructors that call abort() again. */
static bool abort_lr_valid(uint64_t lr) {
    return ((lr & 3) == 0) &&
           lr >= 0x1000000ULL &&
           lr < 0x3000000ULL;
}

static void bridge_abort(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    g_abort_count++;
    g_abort_reentry_depth++;

    uint64_t lr = emu->get_lr();
    uint64_t sp = emu->get_reg(31);

    // Detect abort retry loop: same LR means same callsite retrying
    if (lr == g_last_abort_lr) {
        g_same_lr_count++;
    } else {
        g_same_lr_count = 1;
        g_last_abort_lr = lr;
    }

    // ── RE-ENTRANCY GUARD ─────────────────────────────────────────────────
    // If abort fires WHILE we are already handling a previous abort, the
    // frame-walker landed in code that called abort() again (destructor loop,
    // e.g. ObjectLibrary::~ObjectLibrary → vtable dispatch → abort()).
    // Do NOT walk frames again — that will only deepen the loop.
    // Instead, redirect straight to MAGIC_LR and let the host recover.
    if (g_abort_reentry_depth > 1 || g_same_lr_count > 2) {
        std::cerr << "[SRE/Abort-Recovery] Re-entrant abort #" << g_abort_reentry_depth
                  << " (same_lr=" << g_same_lr_count << ") — escaping to MAGIC_LR\n";
        g_abort_reentry_depth = 0;
        g_same_lr_count = 0;
        emu->redirect_pc = 0xE0000000ULL;  // MAGIC_LR — host detects and recovers
        emu->set_reg(0, 0);
        return;
    }

    if (g_abort_count <= 3) {
        std::cerr << "[WARN] abort() #" << g_abort_count
                  << " PC=0x" << std::hex << emu->get_pc()
                  << " LR=0x" << lr
                  << " SP=0x" << sp << std::dec << "\n";

        // Dump registers for debugging
        std::cerr << "  X0=0x" << std::hex << emu->get_reg(0)
                  << " X1=0x" << emu->get_reg(1)
                  << " X19=0x" << emu->get_reg(19)
                  << " X20=0x" << emu->get_reg(20) << std::dec << "\n";

        // If called from the parser assert (LR near 0x15812e4), dump the object
        uint64_t x19 = emu->get_reg(19);
        if (x19 > 0x1000 && x19 < 0xF0000000) {
            uint64_t val792 = *(uint64_t*)(memory + (uint32_t)x19 + 792);
            uint64_t val832 = *(uint64_t*)(memory + (uint32_t)x19 + 832);
            std::cerr << "  object[792]=0x" << std::hex << val792
                      << " object[832]=0x" << val832 << std::dec << "\n";
            if (val792 > 0x1000000 && val792 < 0x2000000) {
                uint32_t code1 = *(uint32_t*)(memory + (uint32_t)val792);
                uint32_t code2 = *(uint32_t*)(memory + (uint32_t)val792 + 4);
                std::cerr << "  code@[792]: 0x" << std::hex << code1
                          << " 0x" << code2 << std::dec << "\n";
            }
        }
    }

    // ABORT RECOVERY: Walk up to 2 stack frames looking for a valid return address.
    // Critically: validate that the target is 4-byte aligned and NOT in a region
    // that will call abort() itself (i.e. not a destructor body).
    uint64_t fp = emu->get_reg(29);  // current frame pointer
    if (fp > 0x1000 && fp < 0xF0000000) {
        uint64_t fp1 = *(uint64_t*)(memory + (uint32_t)fp);
        uint64_t lr1 = *(uint64_t*)(memory + (uint32_t)fp + 8);
        uint64_t fp2 = 0, lr2 = 0;
        if (fp1 > 0x1000 && fp1 < 0xF0000000) {
            fp2 = *(uint64_t*)(memory + (uint32_t)fp1);
            lr2 = *(uint64_t*)(memory + (uint32_t)fp1 + 8);
        }

        // Prefer lr2 (2 frames up) — it's further from the abort callsite
        if (abort_lr_valid(lr2)) {
            // Extra guard: if we already redirected here and got another abort,
            // the target itself is bad — try lr1 instead
            if (lr2 == g_abort_last_target) {
                g_abort_same_target_count++;
                if (g_abort_same_target_count > 1) {
                    std::cerr << "[SRE/Abort-Recovery] Target 0x" << std::hex << lr2
                              << " triggered abort again — escaping to MAGIC_LR\n" << std::dec;
                    g_abort_reentry_depth = 0;
                    g_abort_same_target_count = 0;
                    emu->redirect_pc = 0xE0000000ULL;
                    emu->set_reg(0, 0);
                    return;
                }
            } else {
                g_abort_same_target_count = 1;
                g_abort_last_target = lr2;
            }
            std::cerr << "[SRE/Abort-Recovery] Intercepted abort() — unwinding 2 frames to 0x"
                      << std::hex << lr2 << std::dec << "\n";
            emu->set_reg(29, fp2);
            emu->set_reg(31, fp1 + 16);
            emu->set_reg(0, 0);
            emu->redirect_pc = lr2;
            g_abort_reentry_depth = 0;
            return;
        } else if (abort_lr_valid(lr1)) {
            if (lr1 == g_abort_last_target) {
                g_abort_same_target_count++;
                if (g_abort_same_target_count > 1) {
                    std::cerr << "[SRE/Abort-Recovery] Target 0x" << std::hex << lr1
                              << " triggered abort again — escaping to MAGIC_LR\n" << std::dec;
                    g_abort_reentry_depth = 0;
                    g_abort_same_target_count = 0;
                    emu->redirect_pc = 0xE0000000ULL;
                    emu->set_reg(0, 0);
                    return;
                }
            } else {
                g_abort_same_target_count = 1;
                g_abort_last_target = lr1;
            }
            std::cerr << "[SRE/Abort-Recovery] Intercepted abort() — unwinding 1 frame to 0x"
                      << std::hex << lr1 << std::dec << "\n";
            emu->set_reg(29, fp1);
            emu->set_reg(31, fp + 16);
            emu->set_reg(0, 0);
            emu->redirect_pc = lr1;
            g_abort_reentry_depth = 0;
            return;
        }
    }

    // Fallback: redirect directly to LR or MAGIC_LR
    if (abort_lr_valid(lr)) {
        std::cerr << "[SRE/Abort-Recovery] Intercepted abort() — returning to LR=0x"
                  << std::hex << lr << std::dec << "\n";
        emu->redirect_pc = lr;
        emu->set_reg(0, 0);
    } else {
        std::cerr << "[SRE/Abort-Recovery] Intercepted abort() — escaping to MAGIC_LR\n";
        emu->redirect_pc = 0xE0000000ULL;  // MAGIC_LR
    }
    g_abort_reentry_depth = 0;
}


static void bridge_localtime(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t timer_ptr = emu->get_reg(0);
    time_t timer;
    if (timer_ptr) timer = (time_t)*(uint64_t*)(memory + timer_ptr); // ARM64: time_t is 64-bit
    else timer = time(NULL);
    
    struct tm* t = localtime(&timer);
    // Use a fixed area in guest memory to return the struct tm
    uint32_t tm_guest_ptr = 0x42000;
    uint32_t* tm_guest = (uint32_t*)(memory + tm_guest_ptr);
    tm_guest[0] = t->tm_sec;
    tm_guest[1] = t->tm_min;
    tm_guest[2] = t->tm_hour;
    tm_guest[3] = t->tm_mday;
    tm_guest[4] = t->tm_mon;
    tm_guest[5] = t->tm_year;
    tm_guest[6] = t->tm_wday;
    tm_guest[7] = t->tm_yday;
    tm_guest[8] = t->tm_isdst;
    
    emu->set_reg(0, tm_guest_ptr);
}

// localtime_r(const time_t* timer, struct tm* result) → struct tm*
// Thread-safe version — writes to caller-supplied buffer (X1)
static void bridge_localtime_r(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t timer_ptr = emu->get_reg(0);
    uint64_t result_ptr = emu->get_reg(1);
    
    time_t timer;
    if (timer_ptr) timer = (time_t)*(int64_t*)(memory + timer_ptr);
    else timer = time(NULL);
    
    struct tm t;
    localtime_r(&timer, &t);
    
    if (result_ptr) {
        // ARM64 struct tm: 9 ints (each 4 bytes) + padding
        int32_t* tm_guest = (int32_t*)(memory + result_ptr);
        tm_guest[0] = t.tm_sec;
        tm_guest[1] = t.tm_min;
        tm_guest[2] = t.tm_hour;
        tm_guest[3] = t.tm_mday;
        tm_guest[4] = t.tm_mon;
        tm_guest[5] = t.tm_year;
        tm_guest[6] = t.tm_wday;
        tm_guest[7] = t.tm_yday;
        tm_guest[8] = t.tm_isdst;
        // Android's struct tm also has tm_gmtoff (long) and tm_zone (char*)
        // at offsets 9*4=36 and 36+8=44 — zero them for safety
        *(int64_t*)(tm_guest + 9) = t.tm_gmtoff;
        *(uint64_t*)(tm_guest + 11) = 0; // tm_zone = NULL
    }
    
    emu->set_reg(0, result_ptr); // returns pointer to result
}


static void bridge_lrand48(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    long int res = lrand48();
    emu->set_reg(0, (uint32_t)res);
}


static void bridge_exidx(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t pc = emu->get_reg(0);
    uint32_t pcount_ptr = emu->get_reg(1);
    
    // Mission 15: Return EXIDX for libswordigo.so
    // .ARM.exidx at 0x00410ff8, size 0x19790 (from readelf)
    // Base address is 0x1000000
    uint32_t exidx_start = 0x1000000 + 0x410ff8;
    uint32_t exidx_count = 0x19790 / 8;
    
    if (pcount_ptr) {
        *(uint32_t*)(memory + pcount_ptr) = exidx_count;
    }
    emu->set_reg(0, exidx_start);
}

static void bridge_stat(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t path_ptr = emu->get_reg(0);
    uint32_t stat_buf_ptr = emu->get_reg(1);
    const char* path = (const char*)(memory + path_ptr);
    
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    }

#ifdef _WIN32
    struct __stat64 st;
    int result = _stat64(resolved_path.c_str(), &st);
#else
    struct stat st;
    int result = stat(resolved_path.c_str(), &st);
#endif

    if (result == 0 && stat_buf_ptr != 0) {
        // Android Bionic ARM32 struct stat layout (exactly 104 bytes)
        *(uint64_t*)(memory + stat_buf_ptr + 0) = st.st_dev;     // st_dev (8 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 8) = 0;             // __pad0 (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 12) = st.st_ino;    // __st_ino (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 16) = st.st_mode;   // st_mode (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 20) = st.st_nlink;  // st_nlink (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 24) = st.st_uid;    // st_uid (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 28) = st.st_gid;    // st_gid (4 bytes)
        *(uint64_t*)(memory + stat_buf_ptr + 32) = st.st_rdev;   // st_rdev (8 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 40) = 0;            // __pad3 (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 44) = 0;            // padding before st_size (4 bytes)
        *(uint64_t*)(memory + stat_buf_ptr + 48) = st.st_size;   // st_size (8-byte aligned, 8 bytes)
#ifdef _WIN32
        *(uint32_t*)(memory + stat_buf_ptr + 56) = 4096;         // st_blksize (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 60) = 0;            // padding before st_blocks (4 bytes)
        *(uint64_t*)(memory + stat_buf_ptr + 64) = (st.st_size + 511) / 512; // st_blocks (8-byte aligned, 8 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 72) = (uint32_t)st.st_atime;
        *(uint32_t*)(memory + stat_buf_ptr + 76) = 0;
        *(uint32_t*)(memory + stat_buf_ptr + 80) = (uint32_t)st.st_mtime;
        *(uint32_t*)(memory + stat_buf_ptr + 84) = 0;
        *(uint32_t*)(memory + stat_buf_ptr + 88) = (uint32_t)st.st_ctime;
        *(uint32_t*)(memory + stat_buf_ptr + 92) = 0;
#else
        *(uint32_t*)(memory + stat_buf_ptr + 56) = st.st_blksize;// st_blksize (4 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 60) = 0;            // padding before st_blocks (4 bytes)
        *(uint64_t*)(memory + stat_buf_ptr + 64) = st.st_blocks; // st_blocks (8-byte aligned, 8 bytes)
        *(uint32_t*)(memory + stat_buf_ptr + 72) = st.st_atim.tv_sec;
        *(uint32_t*)(memory + stat_buf_ptr + 76) = st.st_atim.tv_nsec;
        *(uint32_t*)(memory + stat_buf_ptr + 80) = st.st_mtim.tv_sec;
        *(uint32_t*)(memory + stat_buf_ptr + 84) = st.st_mtim.tv_nsec;
        *(uint32_t*)(memory + stat_buf_ptr + 88) = st.st_ctim.tv_sec;
        *(uint32_t*)(memory + stat_buf_ptr + 92) = st.st_ctim.tv_nsec;
#endif
        *(uint64_t*)(memory + stat_buf_ptr + 96) = st.st_ino;    // st_ino (8-byte aligned, 8 bytes)
    }
    
    if (!emu->quiet_mode) {
        std::cout << "[File] stat(\"" << path << "\" -> \"" << resolved_path << "\") -> " << result 
                  << " (mode: 0x" << std::hex << st.st_mode << std::dec 
                  << ", size: " << st.st_size << ")" << std::endl;
    }
    emu->set_reg(0, (uint32_t)result);
}

struct GuestDir {
    std::vector<std::pair<std::string, uint8_t>> entries;
    size_t index = 0;
    uint32_t guest_dirent_addr;
};

static void bridge_opendir(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(0);
    
    if (name_ptr == 0) {
        emu->set_reg(0, 0); // null
        return;
    }
    
    const char* path = (const char*)(memory + name_ptr);
    if (!emu->quiet_mode) {
        std::cout << "[File] opendir(\"" << path << "\")" << std::endl;
    }
    
    std::string resolved_path = path;
    char resolved[512];
    if (resolve_vfs_path(path, resolved, sizeof(resolved))) {
        resolved_path = resolved;
    }

    if (!fs::exists(resolved_path) || !fs::is_directory(resolved_path)) {
        emu->set_reg(0, 0); // null
        return;
    }
    
    GuestDir* gd = new GuestDir();
    gd->guest_dirent_addr = g_guest_heap_ptr;
    g_guest_heap_ptr += (280 + 7) & ~7; // allocate 280 bytes in guest heap for dirent struct
    
    for (const auto& entry : fs::directory_iterator(resolved_path)) {
        std::string name = entry.path().filename().string();
        uint8_t type = 8; // DT_REG default
        if (entry.is_directory()) {
            type = 4; // DT_DIR
        }
        gd->entries.push_back({name, type});
    }
    
    uint32_t handle = g_next_handle++;
    g_handle_to_ptr[handle] = gd;
    
    emu->set_reg(0, handle);
}

static void bridge_readdir(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t handle = emu->get_reg(0);
    
    if (g_handle_to_ptr.count(handle) == 0) {
        emu->set_reg(0, 0); // null
        return;
    }
    
    GuestDir* gd = (GuestDir*)g_handle_to_ptr[handle];
    if (gd->index >= gd->entries.size()) {
        emu->set_reg(0, 0); // null
        return;
    }
    
    const auto& de = gd->entries[gd->index++];
    uint32_t addr = gd->guest_dirent_addr;
    *(uint64_t*)(memory + addr + 0) = gd->index; // dummy inode
    *(uint64_t*)(memory + addr + 8) = gd->index; // dummy offset
    *(uint16_t*)(memory + addr + 16) = 280;      // reclen
    *(uint8_t*)(memory + addr + 18) = de.second;  // type
    std::strncpy((char*)(memory + addr + 19), de.first.c_str(), 256);
    
    emu->set_reg(0, addr);
}

static void bridge_closedir(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    
    if (g_handle_to_ptr.count(handle) == 0) {
        emu->set_reg(0, -1); // error
        return;
    }
    
    GuestDir* gd = (GuestDir*)g_handle_to_ptr[handle];
    delete gd;
    g_handle_to_ptr.erase(handle);
    
    emu->set_reg(0, 0);
}

static void bridge_android_log_print(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t tag_ptr = emu->get_reg(1);
    uint32_t fmt_ptr = emu->get_reg(2);
    const char* tag = (const char*)(memory + tag_ptr);
    const char* fmt = (const char*)(memory + fmt_ptr);
    std::cout << "[ALOG] " << tag << ": " << fmt << std::endl;
    emu->set_reg(0, 0);
}

// --- OpenAL Stubs (Now Real wrappers) ---
static ALCdevice* g_alc_device = nullptr;
static ALCcontext* g_alc_context = nullptr;

static void bridge_alcOpenDevice(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_alc_device = alcOpenDevice(nullptr);
    emu->set_reg(0, g_alc_device ? 1 : 0);
}

static void bridge_alcCreateContext(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_alc_context = alcCreateContext(g_alc_device, nullptr);
    emu->set_reg(0, g_alc_context ? 2 : 0);
}

static void bridge_alcMakeContextCurrent(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    ALCboolean res = alcMakeContextCurrent(g_alc_context);
    emu->set_reg(0, res ? 1 : 0);
}

static void bridge_alGetError(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, alGetError());
}

static void bridge_alcSuspendContext(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_alc_context) alcSuspendContext(g_alc_context);
}

static void bridge_alcProcessContext(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_alc_context) alcProcessContext(g_alc_context);
}

static void bridge_alcResume(void* emu_ptr) {
    // alcResume is an Android extension (not standard OpenAL)
    // Just make context current again as a substitute
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_alc_context) alcMakeContextCurrent(g_alc_context);
}

static void bridge_alcSuspend(void* emu_ptr) {
    // alcSuspend is an Android extension — no-op
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    (void)emu;
}

static void bridge_alcDestroyContext(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_alc_context) {
        alcDestroyContext(g_alc_context);
        g_alc_context = nullptr;
    }
}

static void bridge_alcCloseDevice(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_alc_device) {
        alcCloseDevice(g_alc_device);
        g_alc_device = nullptr;
    }
}

static void bridge_alGenSources(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t sources_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_sources(n);
    alGenSources(n, host_sources.data());
    for (uint32_t i = 0; i < n; i++) {
        RedstellGC::instance().track_al_gen_source(host_sources[i]);
    }
    std::memcpy(memory + sources_ptr, host_sources.data(), n * sizeof(ALuint));
}

static void bridge_alGenBuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t buffers_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_buffers(n);
    alGenBuffers(n, host_buffers.data());
    for (uint32_t i = 0; i < n; i++) {
        RedstellGC::instance().track_al_gen_buffer(host_buffers[i]);
    }
    std::memcpy(memory + buffers_ptr, host_buffers.data(), n * sizeof(ALuint));
}

static void bridge_alSourcePlay(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourcePlay(source);
}

static void bridge_alSourceStop(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourceStop(source);
}

static void bridge_alSourcePause(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourcePause(source);
}

static void bridge_alSourcei(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t value = emu->get_reg(2);
    alSourcei(source, param, value);
}

static void bridge_alSourcef(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = (uint32_t)emu->get_reg(0);
    uint32_t param = (uint32_t)emu->get_reg(1);
    // ARM64 AAPCS64: float arg in S0 (independent of integer regs)
    float val_f = emu->get_sreg(0);
    alSourcef(source, param, val_f);
}

static void bridge_alSource3f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = (uint32_t)emu->get_reg(0);
    uint32_t param = (uint32_t)emu->get_reg(1);
    // ARM64 AAPCS64: float args in S0, S1, S2 (independent of integer regs)
    float v1 = emu->get_sreg(0);
    float v2 = emu->get_sreg(1);
    float v3 = emu->get_sreg(2);
    alSource3f(source, param, v1, v2, v3);
}

static void bridge_alBufferData(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buffer = (uint32_t)emu->get_reg(0);
    uint32_t format = (uint32_t)emu->get_reg(1);
    uint32_t data_ptr = (uint32_t)emu->get_reg(2);
    uint32_t size = (uint32_t)emu->get_reg(3);
    // ARM64: 5th arg in X4 (8 integer regs available)
    uint32_t freq = (uint32_t)emu->get_reg(4);
    
    alBufferData(buffer, format, memory + data_ptr, size, freq);
}

static void bridge_alSourceQueueBuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t nb = emu->get_reg(1);
    uint32_t buffers_ptr = emu->get_reg(2);
    
    std::vector<ALuint> host_buffers(nb);
    std::memcpy(host_buffers.data(), memory + buffers_ptr, nb * sizeof(ALuint));
    alSourceQueueBuffers(source, nb, host_buffers.data());
}

static void bridge_alSourceUnqueueBuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t nb = emu->get_reg(1);
    uint32_t buffers_ptr = emu->get_reg(2);
    
    std::vector<ALuint> host_buffers(nb);
    alSourceUnqueueBuffers(source, nb, host_buffers.data());
    std::memcpy(memory + buffers_ptr, host_buffers.data(), nb * sizeof(ALuint));
}

static void bridge_alGetSourcei(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t value_ptr = emu->get_reg(2);
    
    ALint host_val = 0;
    alGetSourcei(source, param, &host_val);
    *(ALint*)(memory + value_ptr) = host_val;
}

static void bridge_alDeleteSources(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t sources_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_sources(n);
    std::memcpy(host_sources.data(), memory + sources_ptr, n * sizeof(ALuint));
    for (uint32_t i = 0; i < n; i++) {
        RedstellGC::instance().track_al_delete_source(host_sources[i]);
    }
    alDeleteSources(n, host_sources.data());
}

static void bridge_alDeleteBuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t buffers_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_buffers(n);
    std::memcpy(host_buffers.data(), memory + buffers_ptr, n * sizeof(ALuint));
    for (uint32_t i = 0; i < n; i++) {
        RedstellGC::instance().track_al_delete_buffer(host_buffers[i]);
    }
    alDeleteBuffers(n, host_buffers.data());
}

static void bridge_alListenerf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t param = (uint32_t)emu->get_reg(0);
    // ARM64 AAPCS64: float arg in S0
    float val_f = emu->get_sreg(0);
    alListenerf(param, val_f);
}

static void bridge_alListener3f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t param = (uint32_t)emu->get_reg(0);
    // ARM64 AAPCS64: float args in S0, S1, S2
    float v1 = emu->get_sreg(0);
    float v2 = emu->get_sreg(1);
    float v3 = emu->get_sreg(2);
    alListener3f(param, v1, v2, v3);
}

static void bridge_alDistanceModel(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t distanceModel = emu->get_reg(0);
    alDistanceModel(distanceModel);
}

// --- Missing OpenAL bridges ---

static void bridge_alGetSourcef(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t value_ptr = emu->get_reg(2);
    ALfloat val = 0.0f;
    alGetSourcef(source, param, &val);
    if (value_ptr) *(float*)(memory + value_ptr) = val;
}

static void bridge_alListenerfv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t param = emu->get_reg(0);
    uint32_t values_ptr = emu->get_reg(1);
    // Most common: AL_ORIENTATION = 6 floats, AL_POSITION/VELOCITY = 3 floats
    float host_values[6];
    int count = (param == AL_ORIENTATION) ? 6 : 3;
    memcpy(host_values, memory + values_ptr, count * sizeof(float));
    alListenerfv(param, host_values);
}

static void bridge_alSourceRewind(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourceRewind(source);
}

static void bridge_alcGetCurrentContext(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // Return a non-zero fake handle if we have a context
    emu->set_reg(0, g_alc_context ? 1 : 0);
}

// --- Missing libc/Android bridges ---

static void bridge_exit(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int code = (int)emu->get_reg(0);
    std::cerr << "[Bridge] exit(" << code << ") called — ignored (non-fatal)" << std::endl;
    /* Don't actually exit. luaD_throw will return to caller.
     * The corrupted ProgramState will be cleaned up by the game loop. */
}

static void bridge_read(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int fd = emu->get_reg(0);
    uint64_t buf_g = emu->get_reg(1);
    size_t count = emu->get_reg(2);
    void* buf = buf_g ? (void*)(emu->get_memory_base() + buf_g) : nullptr;
    ssize_t ret = read(fd, buf, count);
    emu->set_reg(0, ret);
}

static void bridge_write(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int fd = emu->get_reg(0);
    uint64_t buf_g = emu->get_reg(1);
    size_t count = emu->get_reg(2);
    const void* buf = buf_g ? (const void*)(emu->get_memory_base() + buf_g) : nullptr;
    ssize_t ret = write(fd, buf, count);
    emu->set_reg(0, ret);
}

static void bridge_writev(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int fd = emu->get_reg(0);
    uint64_t iov_g = emu->get_reg(1);
    int iovcnt = emu->get_reg(2);
    
    if (iov_g && iovcnt > 0) {
        struct iovec* host_iov = (struct iovec*)alloca(sizeof(struct iovec) * iovcnt);
        for (int i = 0; i < iovcnt; i++) {
            uint64_t guest_iov_base = *(uint64_t*)(memory + iov_g + i * 16);
            uint64_t guest_iov_len = *(uint64_t*)(memory + iov_g + i * 16 + 8);
            host_iov[i].iov_base = guest_iov_base ? (void*)(memory + guest_iov_base) : nullptr;
            host_iov[i].iov_len = guest_iov_len;
        }
        ssize_t ret = writev(fd, host_iov, iovcnt);
        emu->set_reg(0, ret);
    } else {
        emu->set_reg(0, (uint64_t)-1);
    }
}

static void bridge_fstat(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int fd = emu->get_reg(0);
    uint64_t statbuf_g = emu->get_reg(1);
    
    struct stat host_stat;
    int ret = fstat(fd, &host_stat);
    if (ret == 0 && statbuf_g) {
        memset(memory + statbuf_g, 0, 128);
        *(uint64_t*)(memory + statbuf_g + 0) = host_stat.st_dev;
        *(uint64_t*)(memory + statbuf_g + 8) = host_stat.st_ino;
        *(uint32_t*)(memory + statbuf_g + 16) = host_stat.st_mode;
        *(uint32_t*)(memory + statbuf_g + 20) = host_stat.st_nlink;
        *(uint32_t*)(memory + statbuf_g + 24) = host_stat.st_uid;
        *(uint32_t*)(memory + statbuf_g + 28) = host_stat.st_gid;
        *(uint64_t*)(memory + statbuf_g + 32) = host_stat.st_rdev;
        *(uint64_t*)(memory + statbuf_g + 48) = host_stat.st_size;
        *(uint32_t*)(memory + statbuf_g + 56) = host_stat.st_blksize;
        *(uint64_t*)(memory + statbuf_g + 64) = host_stat.st_blocks;
        *(uint64_t*)(memory + statbuf_g + 72) = host_stat.st_atim.tv_sec;
        *(uint64_t*)(memory + statbuf_g + 80) = host_stat.st_atim.tv_nsec;
        *(uint64_t*)(memory + statbuf_g + 88) = host_stat.st_mtim.tv_sec;
        *(uint64_t*)(memory + statbuf_g + 96) = host_stat.st_mtim.tv_nsec;
        *(uint64_t*)(memory + statbuf_g + 104) = host_stat.st_ctim.tv_sec;
        *(uint64_t*)(memory + statbuf_g + 112) = host_stat.st_ctim.tv_nsec;
    }
    emu->set_reg(0, ret);
}

static void bridge_poll(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t fds_g = emu->get_reg(0);
    nfds_t nfds = emu->get_reg(1);
    int timeout = emu->get_reg(2);
    struct pollfd* fds = fds_g ? (struct pollfd*)(memory + fds_g) : nullptr;
    int ret = poll(fds, nfds, timeout);
    emu->set_reg(0, ret);
}

static void bridge_ioctl(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int fd = emu->get_reg(0);
    uint64_t request = emu->get_reg(1);
    uint64_t argp_g = emu->get_reg(2);
    void* argp = argp_g ? (void*)(memory + argp_g) : nullptr;
    int ret = ioctl(fd, request, argp);
    emu->set_reg(0, ret);
}

static void bridge_perror(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    if (str_ptr) {
        std::cerr << "[Guest] perror: " << (char*)(memory + str_ptr) << std::endl;
    }
}

static void bridge_syscall(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t sysno = emu->get_reg(0);
    std::cout << "[Bridge] syscall(" << sysno << ") stubbed" << std::endl;
    emu->set_reg(0, (uint64_t)-1);
}

// Android _ctype_ table — provides character classification for ctype.h
static const unsigned short g_android_ctype_table[256] = {
    // Control chars 0x00-0x1F
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20, 0x20,0x01,0x01,0x01,0x01,0x01,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20, 0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    // Space, punct, digits
    0x08,0x10,0x10,0x10,0x10,0x10,0x10,0x10, 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04, 0x04,0x04,0x10,0x10,0x10,0x10,0x10,0x10,
    // @, A-Z
    0x10,0x41,0x41,0x41,0x41,0x41,0x41,0x01, 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, 0x01,0x01,0x01,0x10,0x10,0x10,0x10,0x10,
    // `, a-z
    0x10,0x42,0x42,0x42,0x42,0x42,0x42,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x10,0x10,0x10,0x10,0x20,
    // 0x80-0xFF (high bytes)
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
};

static void bridge_ctype_(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // Write the ctype table to a known guest address and return pointer to it
    static uint32_t ctype_addr = 0x50000000;
    static bool written = false;
    if (!written) {
        memcpy(emu->get_memory_base() + ctype_addr, g_android_ctype_table, sizeof(g_android_ctype_table));
        written = true;
    }
    emu->set_reg(0, ctype_addr);
}

// String functions
static void bridge_strftime(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf_ptr = emu->get_reg(0);
    uint32_t maxsize = emu->get_reg(1);
    uint32_t fmt_ptr = emu->get_reg(2);
    uint32_t tm_ptr = emu->get_reg(3);
    // Simple stub — write empty string
    if (buf_ptr && maxsize > 0) {
        memory[buf_ptr] = '\0';
    }
    emu->set_reg(0, 0);
}

static void bridge_strncat(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest_ptr = emu->get_reg(0);
    uint32_t src_ptr = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    strncat((char*)(memory + dest_ptr), (const char*)(memory + src_ptr), n);
    emu->set_reg(0, dest_ptr);
}

static void bridge_fdopen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // stub — return NULL
}

static void bridge_freopen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // stub — return NULL
}

// --- Wide character bridges ---

static void bridge_wcslen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    // Android wchar_t is 4 bytes
    size_t len = 0;
    uint32_t* ws = (uint32_t*)(memory + str_ptr);
    while (ws[len]) len++;
    emu->set_reg(0, len);
}

static void bridge_wmemcpy(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    memcpy(memory + dst, memory + src, n * 4); // wchar_t = 4 bytes
    emu->set_reg(0, dst);
}

static void bridge_wmemmove(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    memmove(memory + dst, memory + src, n * 4);
    emu->set_reg(0, dst);
}

static void bridge_wmemset(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst = emu->get_reg(0);
    uint32_t wc = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    uint32_t* d = (uint32_t*)(memory + dst);
    for (uint32_t i = 0; i < n; i++) d[i] = wc;
    emu->set_reg(0, dst);
}

static void bridge_wmemcmp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s1 = emu->get_reg(0);
    uint32_t s2 = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    int res = memcmp(memory + s1, memory + s2, n * 4);
    emu->set_reg(0, (uint64_t)(int64_t)res);
}

static void bridge_wmemchr(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s = emu->get_reg(0);
    uint32_t wc = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    uint32_t* ws = (uint32_t*)(memory + s);
    uint32_t result = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ws[i] == wc) { result = s + i * 4; break; }
    }
    emu->set_reg(0, result);
}

static void bridge_mbrtowc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t pwc = emu->get_reg(0);
    uint32_t s = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (s == 0 || n == 0) { emu->set_reg(0, 0); return; }
    uint8_t c = memory[s];
    if (pwc) *(uint32_t*)(memory + pwc) = (uint32_t)c;
    emu->set_reg(0, c ? 1 : 0);
}

static void bridge_wcrtomb(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s = emu->get_reg(0);
    uint32_t wc = emu->get_reg(1);
    if (s) memory[s] = (uint8_t)(wc & 0xFF);
    emu->set_reg(0, 1);
}

static void bridge_getwc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, (uint64_t)-1); // WEOF
}

static void bridge_putwc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t wc = emu->get_reg(0);
    emu->set_reg(0, wc); // success
}

static void bridge_ungetwc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, (uint64_t)-1); // WEOF — can't unget
}

// String comparison/search
static void bridge_strcoll(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s1 = emu->get_reg(0);
    uint32_t s2 = emu->get_reg(1);
    int res = strcmp((const char*)(memory + s1), (const char*)(memory + s2));
    emu->set_reg(0, (uint64_t)(int64_t)res);
}

static void bridge_strcspn(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s = emu->get_reg(0);
    uint32_t reject = emu->get_reg(1);
    size_t res = strcspn((const char*)(memory + s), (const char*)(memory + reject));
    emu->set_reg(0, res);
}

static void bridge_strpbrk(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s = emu->get_reg(0);
    uint32_t accept = emu->get_reg(1);
    const char* res = strpbrk((const char*)(memory + s), (const char*)(memory + accept));
    emu->set_reg(0, res ? (uint64_t)(res - (const char*)memory) : 0);
}

static void bridge_strxfrm(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    // Simple implementation: just copy
    size_t len = strlen((const char*)(memory + src));
    if (n > 0) strncpy((char*)(memory + dst), (const char*)(memory + src), n);
    emu->set_reg(0, len);
}

// Wide string stubs
static void bridge_wcscoll(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // equal
}
static void bridge_wcsxfrm(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}
static void bridge_wcsftime(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_gzwrite(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t len = emu->get_reg(2);
    emu->set_reg(0, len); // pretend we wrote everything
}

static void bridge_al_noop(void* emu_ptr) {
    // Generic no-op
}

// --- GLES Real Bridge Functions ---
// When g_display_active, these call real host OpenGL.
// Guest memory pointers are translated via (memory + guest_offset).

// Diagnostic: log GL calls on first 3 frames to debug black screen
extern int g_gl_diag_frame;
extern bool g_gl_diag_enabled;
#define GL_DIAG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[GL F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define TEX_LOG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[TEX F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define VERTEX_LOG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[VERTEX F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define MATRIX_LOG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[MATRIX F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define EGL_LOG(...) do { printf("[EGL] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)

static std::unordered_set<uint32_t> g_matrix_dumped_ptrs;

static void log_matrix_once(IEmulatorArm64* emu, uint32_t ptr, const GLfloat* m) {
    if (!g_gl_diag_enabled || g_gl_diag_frame >= 3) return;
    if (g_matrix_dumped_ptrs.count(ptr)) return;
    g_matrix_dumped_ptrs.insert(ptr);
    uint8_t* memory = emu->get_memory_base();
    MATRIX_LOG("glLoadMatrixf ptr=0x%x host=%p sp=0x%x", ptr, (void*)(memory + ptr), emu->get_reg(31));
    MATRIX_LOG("  row0 [%.4f %.4f %.4f %.4f]", m[0], m[1], m[2], m[3]);
    MATRIX_LOG("  row1 [%.4f %.4f %.4f %.4f]", m[4], m[5], m[6], m[7]);
    MATRIX_LOG("  row2 [%.4f %.4f %.4f %.4f]", m[8], m[9], m[10], m[11]);
    MATRIX_LOG("  row3 [%.4f %.4f %.4f %.4f]", m[12], m[13], m[14], m[15]);
}

static void log_vertex_sample(IEmulatorArm64* emu, uint32_t ptr, uint32_t size, uint32_t type, uint32_t stride, const char* label) {
    if (!g_gl_diag_enabled || g_gl_diag_frame >= 3) return;
    VERTEX_LOG("%s ptr=0x%x size=%u stride=%d type=0x%x", label, ptr, size, stride, type);
    if (ptr == 0) {
        VERTEX_LOG("  -> NULL pointer (no vertex data)");
        return;
    }
    if (type != GL_FLOAT) {
        VERTEX_LOG("  -> non-float type, skipping float dump");
        return;
    }
    uint8_t* memory = emu->get_memory_base();
    const float* v = (const float*)(memory + ptr);
    if (size >= 3) {
        VERTEX_LOG("  v0=(%f,%f,%f)", v[0], v[1], v[2]);
        const float* v1 = (stride > 0 && stride != (uint32_t)(size * 4))
            ? (const float*)((const uint8_t*)v + stride)
            : v + size;
        VERTEX_LOG("  v1=(%f,%f,%f)", v1[0], v1[1], v1[2]);
    } else if (size == 2) {
        VERTEX_LOG("  v0=(%f,%f)", v[0], v[1]);
        const float* v1 = (stride > 0 && stride != 8)
            ? (const float*)((const uint8_t*)v + stride)
            : v + 2;
        VERTEX_LOG("  v1=(%f,%f)", v1[0], v1[1]);
    }
}

static void bridge_gl_noop(void* emu_ptr) {
    g_frame_stats.state_changes++;
}

// --- Fog (water color, atmosphere, depth haze) ---
static void bridge_glFogf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum pname = (GLenum)emu->get_reg(0);
    // ARM64 AAPCS64: float arg in S0
    float param = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.fogf(pname, param);
        return;
    }
#endif
    if (g_display_active) glFogf(pname, param);
}

static void bridge_glFogfv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum pname = (GLenum)emu->get_reg(0);
    uint32_t params_ptr = emu->get_reg(1);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (params_ptr != 0) {
            float params[4];
            memcpy(params, memory + params_ptr, 16);
            g_vk_backend.fogfv(pname, params);
        }
        return;
    }
#endif
    if (g_display_active && params_ptr != 0) {
        float params[4];
        memcpy(params, memory + params_ptr, 16);
        glFogfv(pname, params);
    }
}

static void bridge_glFogi(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum pname = (GLenum)emu->get_reg(0);
    GLint param = (GLint)emu->get_reg(1);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.fogi(pname, param);
        return;
    }
#endif
    if (g_display_active) glFogi(pname, param);
}
// --- Lighting ---
static void bridge_glLightf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum light = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    // ARM64 AAPCS64: float arg in S0
    float param = emu->get_sreg(0);
    
    static int lf_diag = 0;
    if (lf_diag < 10) {
        std::cout << "[GL64-LIGHT] glLightf(light=0x" << std::hex << light 
                  << ", pname=0x" << pname << std::dec << ", param=" << param << ")" << std::endl;
        lf_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.lightf(light, pname, param);
        return;
    }
#endif
    if (g_display_active) glLightf(light, pname, param);
}

static void bridge_glLightfv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum light = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t params_ptr = emu->get_reg(2);
    
    static int lfv_diag = 0;
    if (lfv_diag < 10 && params_ptr != 0) {
        float p[4];
        memcpy(p, memory + params_ptr, 16);
        std::cout << "[GL64-LIGHT] glLightfv(light=0x" << std::hex << light 
                  << ", pname=0x" << pname << std::dec 
                  << ", vals=[" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << "])" << std::endl;
        lfv_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (params_ptr != 0) {
            float params[4];
            memcpy(params, memory + params_ptr, 16);
            g_vk_backend.lightfv(light, pname, params);
        }
        return;
    }
#endif
    if (g_display_active && params_ptr != 0) {
        float params[4];
        memcpy(params, memory + params_ptr, 16);
        glLightfv(light, pname, params);

        // ── Handshake: capture light state for FBO shaders ────────────────
        int li = (int)(light - GL_LIGHT0);
        if (li >= 0 && li < 8) {
            switch (pname) {
                case GL_POSITION: memcpy(g_frame_lights[li].position, params, 16); break;
                case GL_AMBIENT:  memcpy(g_frame_lights[li].ambient,  params, 16); break;
                case GL_DIFFUSE:  memcpy(g_frame_lights[li].diffuse,  params, 16); break;
                case GL_SPECULAR: memcpy(g_frame_lights[li].specular, params, 16); break;
                default: break;
            }
        }
    }
}

static void bridge_glLightModelf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum pname = (GLenum)emu->get_reg(0);
    // ARM64 AAPCS64: float arg in S0
    float param = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.light_modelf(pname, param);
        return;
    }
#endif
    if (g_display_active) glLightModelf(pname, param);
}

static void bridge_glLightModelfv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum pname = (GLenum)emu->get_reg(0);
    uint32_t params_ptr = emu->get_reg(1);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (params_ptr != 0) {
            float params[4];
            memcpy(params, memory + params_ptr, 16);
            g_vk_backend.light_modelfv(pname, params);
        }
        return;
    }
#endif
    if (g_display_active && params_ptr != 0) {
        float params[4];
        memcpy(params, memory + params_ptr, 16);
        glLightModelfv(pname, params);

        // ── Handshake: capture global ambient for FBO shaders ─────────────
        if (pname == GL_LIGHT_MODEL_AMBIENT)
            memcpy(g_frame_light_model_ambient, params, 16);
    }
}

// --- Materials ---
static void bridge_glMaterialf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum face = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    // ARM64 AAPCS64: float arg in S0
    float param = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.materialf(face, pname, param);
        return;
    }
#endif
    if (g_display_active) glMaterialf(face, pname, param);
}

static void bridge_glMaterialfv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum face = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t params_ptr = emu->get_reg(2);
    
    static int mat_diag = 0;
    if (mat_diag < 15 && params_ptr != 0) {
        float p[4];
        memcpy(p, memory + params_ptr, 16);
        std::cout << "[GL64-MAT] glMaterialfv(face=0x" << std::hex << face 
                  << ", pname=0x" << pname << std::dec 
                  << ", ptr=0x" << std::hex << params_ptr << std::dec
                  << ", vals=[" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << "])" << std::endl;
        mat_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (params_ptr != 0) {
            float params[4];
            memcpy(params, memory + params_ptr, 16);
            g_vk_backend.materialfv(face, pname, params);
        }
        return;
    }
#endif
    if (g_display_active && params_ptr != 0) {
        float params[4];
        memcpy(params, memory + params_ptr, 16);
        glMaterialfv(face, pname, params);

        // ── Handshake: capture material state for FBO shaders ─────────────
        // We track front-face materials only (Swordigo uses GL_FRONT).
        if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
            switch (pname) {
                case GL_AMBIENT:  memcpy(g_frame_material.ambient,  params, 16); break;
                case GL_DIFFUSE:  memcpy(g_frame_material.diffuse,  params, 16); break;
                case GL_SPECULAR: memcpy(g_frame_material.specular, params, 16); break;
                case GL_EMISSION: memcpy(g_frame_material.emission, params, 16); break;
                default: break;
            }
        }
    }
}

// --- Vertex Color ---
static void bridge_glColor4ub(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLubyte r = (GLubyte)emu->get_reg(0);
    GLubyte g = (GLubyte)emu->get_reg(1);
    GLubyte b = (GLubyte)emu->get_reg(2);
    GLubyte a = (GLubyte)emu->get_reg(3);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.color4ub((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
        return;
    }
#endif
    if (g_display_active) glColor4ub(r, g, b, a);
    // ── Handshake: track current color (normalised) ───────────────────────
    g_current_color[0] = r / 255.0f; g_current_color[1] = g / 255.0f;
    g_current_color[2] = b / 255.0f; g_current_color[3] = a / 255.0f;
}

// --- Texture Environment ---
static void bridge_glTexEnvi(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum target = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    GLint param = (GLint)emu->get_reg(2);
    
    (void)target; (void)pname; (void)param;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.tex_envi(target, pname, param);
        return;
    }
#endif
    if (g_display_active) glTexEnvi(target, pname, param);
}

static void bridge_glTexEnvfv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum target = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t params_ptr = emu->get_reg(2);
    
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (params_ptr != 0) {
            float params[4];
            memcpy(params, memory + params_ptr, 16);
            g_vk_backend.tex_envfv(target, pname, params);
        }
        return;
    }
#endif
    if (g_display_active && params_ptr != 0) {
        float params[4];
        memcpy(params, memory + params_ptr, 16);
        glTexEnvfv(target, pname, params);
    }
}

// --- Misc rendering ---
static void bridge_glCullFace(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum mode = (GLenum)emu->get_reg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.cull_face(mode);
        return;
    }
#endif
    if (g_display_active) glCullFace(mode);
}

static void bridge_glHint(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum target = (GLenum)emu->get_reg(0);
    GLenum mode = (GLenum)emu->get_reg(1);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.hint(target, mode);
        return;
    }
#endif
    if (g_display_active) glHint(target, mode);
}

static void bridge_glPointSize(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // ARM64 AAPCS64: float arg in S0
    float size = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.point_size(size);
        return;
    }
#endif
    if (g_display_active) glPointSize(size);
}

// --- Stencil (needed for shadows/water reflections) ---
static void bridge_glStencilFunc(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum func = (GLenum)emu->get_reg(0);
    GLint ref = (GLint)emu->get_reg(1);
    GLuint mask = (GLuint)emu->get_reg(2);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.stencil_func(func, ref, mask);
        return;
    }
#endif
    if (g_display_active) glStencilFunc(func, ref, mask);
}

static void bridge_glStencilMask(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint mask = (GLuint)emu->get_reg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.stencil_mask(mask);
        return;
    }
#endif
    if (g_display_active) glStencilMask(mask);
}

static void bridge_glStencilOp(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum sfail = (GLenum)emu->get_reg(0);
    GLenum dpfail = (GLenum)emu->get_reg(1);
    GLenum dppass = (GLenum)emu->get_reg(2);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.stencil_op(sfail, dpfail, dppass);
        return;
    }
#endif
    if (g_display_active) glStencilOp(sfail, dpfail, dppass);
}

static void bridge_glClearStencil(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLint s = (GLint)emu->get_reg(0);
    if (g_display_active) glClearStencil(s);
}


// --- Phase 1: Clear/Viewport ---

static void bridge_gl_clear(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.clear_calls++;
    uint64_t mask = emu->get_reg(0);
    GL_DIAG("glClear(0x%x)", mask);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.clear(mask);
        return;
    }
#endif
    if (g_display_active) {
        if (g_void_fill_color.active && (mask & GL_COLOR_BUFFER_BIT)) {
            float current_clear_color[4];
            glGetFloatv(GL_COLOR_CLEAR_VALUE, current_clear_color);
            if (current_clear_color[0] < 0.05f && current_clear_color[1] < 0.05f && current_clear_color[2] < 0.05f) {
                glClearColor(g_void_fill_color.r, g_void_fill_color.g, g_void_fill_color.b, 1.0f);
            }
        }
        glClear(mask);
    }
}

static void bridge_gl_clear_color(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    // ARM64 AAPCS64: all-float args in S0-S3
    float fr = emu->get_sreg(0), fg = emu->get_sreg(1);
    float fb = emu->get_sreg(2), fa = emu->get_sreg(3);
    
    if (g_void_fill_color.active && fr < 0.01f && fg < 0.01f && fb < 0.01f) {
        fr = g_void_fill_color.r;
        fg = g_void_fill_color.g;
        fb = g_void_fill_color.b;
    }

    static int cc_diag = 0;
    if (cc_diag < 10) {
        std::cout << "[GL64-DIAG] glClearColor(" << fr << ", " << fg << ", " << fb << ", " << fa << ")" << std::endl;
        cc_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.clear_color(fr, fg, fb, fa);
        return;
    }
#endif
    if (g_display_active) {
        GL_DIAG("glClearColor(%f, %f, %f, %f)", fr, fg, fb, fa);
        glClearColor(fr, fg, fb, fa);
    }
}

// Global window dimensions — updated by main.cpp when window resizes or goes fullscreen
extern int g_win_w;
extern int g_win_h;
extern int g_draw_w;
extern int g_draw_h;

static void bridge_gl_viewport(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.viewport_calls++;
    
    static int vp_diag = 0;
    if (vp_diag < 10) {
        std::cout << "[GL64-DIAG] glViewport(" << emu->get_reg(0) << ", " << emu->get_reg(1) 
                  << ", " << emu->get_reg(2) << ", " << emu->get_reg(3) << ")" << std::endl;
        vp_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.viewport(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
        return;
    }
#endif
    if (g_display_active) {
        // Pass viewport through unchanged — the FBO captures at game resolution,
        // fbo_end_game_and_blit() handles all scaling to window via GLSL shaders.
        glViewport(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
    }
}

// Forward declarations for draw batcher (defined later with GL state)
static DrawBatcher g_batcher;
static bool g_batching_enabled = false; // Disabled: causes tearing — re-enable after fix
static BatchState g_current_batch_state = {};

// Track current matrix mode so we can apply correct fallback for zero matrices
static GLenum g_current_matrix_mode = GL_MODELVIEW;
static bool g_current_projection_is_ortho = false;

static void bridge_gl_matrix_mode(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    g_current_matrix_mode = (GLenum)emu->get_reg(0);
    GL_DIAG("glMatrixMode(0x%x) %s", emu->get_reg(0), emu->get_reg(0)==0x1701?"PROJECTION":emu->get_reg(0)==0x1700?"MODELVIEW":"OTHER");
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.matrix_mode(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) {
        glMatrixMode(emu->get_reg(0));
    }
}

static void bridge_gl_load_identity(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GL_DIAG("glLoadIdentity()");
    g_frame_stats.matrix_ops++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.load_identity();
        return;
    }
#endif
    if (g_display_active) glLoadIdentity();

    // ── Handshake: reset matrix shadow to identity ─────────────────────────
    static const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (g_current_matrix_mode == GL_MODELVIEW)
        memcpy(g_current_modelview,  kIdentity, 64);
    else if (g_current_matrix_mode == GL_PROJECTION) {
        memcpy(g_current_projection, kIdentity, 64);
        g_current_projection_is_ortho = true;
    }
}

static void bridge_gl_load_matrixf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    uint8_t* memory = emu->get_memory_base();
    uint32_t ptr = emu->get_reg(0);
    const GLfloat* m = (const GLfloat*)(memory + ptr);
    
    bool has_bad = false;
    for (int i = 0; i < 16; i++) {
        if (std::isnan(m[i]) || std::isinf(m[i])) {
            has_bad = true;
            break;
        }
    }
    
    bool is_zero = true;
    for (int i = 0; i < 16; i++) {
        if (m[i] != 0.0f) {
            is_zero = false;
            break;
        }
    }
    
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (is_zero || has_bad) {
            // Use identity as fallback for bad matrices
            float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            g_vk_backend.load_matrixf(identity);
        } else {
            g_vk_backend.load_matrixf(m);
        }
        return;
    }
#endif

    // Silently fix zero matrices - they happen during projection setup before
    // the engine's tanf-based projection is fully computed.
    // Log only NaN/Inf (real bugs), not zero (expected during boot).
    if (is_zero) {
        static uint32_t zero_matrix_count = 0;
        if ((++zero_matrix_count <= 3) || (zero_matrix_count % 10000 == 0)) {
            uint32_t lr = emu->get_lr();
            std::cout << "[MATRIX] Zero matrix #" << zero_matrix_count 
                      << " (using fallback) LR=0x" << std::hex << lr << std::dec << std::endl;
        }
        if (g_display_active) {
            // For projection: use ortho matching game viewport; for modelview: identity
            if (g_current_matrix_mode == GL_PROJECTION) {
                glOrtho(0, g_draw_w, g_draw_h, 0, -1, 1);  // Match actual game resolution
                g_current_projection_is_ortho = true;
            } else {
                glLoadIdentity();
            }
        } else {
            GLfloat identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            memcpy((void*)m, identity, 64);
        }
        return;
    }
    
    if (has_bad) {
        uint32_t lr = emu->get_lr();
        static uint32_t nan_count = 0;
        if ((++nan_count <= 3) || (nan_count % 1000 == 0)) {
            std::cout << "[MATRIX] NaN/Inf #" << nan_count << " at LR=0x" << std::hex << lr << std::dec
                      << " [" << m[0] << " " << m[1] << " / " << m[4] << " " << m[5] << "]" << std::endl;
        }
        if (g_display_active) {
            if (g_current_matrix_mode == GL_PROJECTION) {
                glOrtho(0, g_draw_w, g_draw_h, 0, -1, 1);
                g_current_projection_is_ortho = true;
            } else {
                glLoadIdentity();
            }
        }
        return;
    }

    log_matrix_once(emu, ptr, m);
    GL_DIAG("glLoadMatrixf(@0x%x) [%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / ...]", ptr, m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7]);
    
    // Capture matrices for draw batcher (CPU-side transform)
    if (g_batching_enabled) {
        if (g_current_matrix_mode == GL_MODELVIEW) {
            g_batcher.flush(); // Matrix changed → flush pending batch
            g_batcher.set_modelview(m);
        } else if (g_current_matrix_mode == GL_PROJECTION) {
            g_batcher.flush();
            g_batcher.set_projection(m);
        }
    }
    
    if (g_display_active) {
        glLoadMatrixf(m);
    }

    // ── Handshake: shadow the matrix for FBO shaders ──────────────────────
    // We only capture after the GL call so skipped (zero/NaN) matrices are
    // not reflected in the shadow (those code paths returned early above).
    if (g_current_matrix_mode == GL_MODELVIEW)
        memcpy(g_current_modelview,  m, 64);
    else if (g_current_matrix_mode == GL_PROJECTION) {
        memcpy(g_current_projection, m, 64);
        g_current_projection_is_ortho = (m[15] == 1.0f);
    }
}

static void bridge_gl_mult_matrixf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    uint8_t* memory = emu->get_memory_base();
    uint32_t ptr = emu->get_reg(0);
    const GLfloat* m = (const GLfloat*)(memory + ptr);
    
    bool has_bad = false;
    for (int i = 0; i < 16; i++) {
        if (std::isnan(m[i]) || std::isinf(m[i])) {
            has_bad = true;
            break;
        }
    }
    
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (!has_bad) {
            g_vk_backend.mult_matrixf(m);
        }
        return;
    }
#endif

    if (has_bad || (g_gl_diag_enabled && g_gl_diag_frame < 3)) {
        std::cout << "[MATRIX] glMultMatrixf" << (has_bad ? " BAD!" : "") << " at LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl;
        std::cout << "  [" << m[0] << " " << m[4] << " " << m[8] << " " << m[12] << " / "
                  << m[1] << " " << m[5] << " " << m[9] << " " << m[13] << " / "
                  << m[2] << " " << m[6] << " " << m[10] << " " << m[14] << " / "
                  << m[3] << " " << m[7] << " " << m[11] << " " << m[15] << "]" << std::endl;
    }

    if (has_bad) {
        return;
    }

    if (g_display_active) {
        glMultMatrixf(m);
        
        // Shadow matrix updates
        if (g_current_matrix_mode == GL_MODELVIEW) {
            glGetFloatv(GL_MODELVIEW_MATRIX, g_current_modelview);
        } else if (g_current_matrix_mode == GL_PROJECTION) {
            glGetFloatv(GL_PROJECTION_MATRIX, g_current_projection);
            g_current_projection_is_ortho = (g_current_projection[15] == 1.0f);
        }
    }
}

static int g_gl_matrix_stack_depth = 0;

static void bridge_gl_push_matrix(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_gl_matrix_stack_depth++;
    GL_DIAG("glPushMatrix() depth=%d", g_gl_matrix_stack_depth);
    g_frame_stats.matrix_ops++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.push_matrix();
        return;
    }
#endif
    if (g_display_active) glPushMatrix();
}

static void bridge_gl_pop_matrix(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_gl_matrix_stack_depth--;
    GL_DIAG("glPopMatrix() depth=%d", g_gl_matrix_stack_depth);
    g_frame_stats.matrix_ops++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.pop_matrix();
        return;
    }
#endif
    if (g_display_active) {
        glPopMatrix();
        
        // Shadow matrix updates
        if (g_current_matrix_mode == GL_MODELVIEW) {
            glGetFloatv(GL_MODELVIEW_MATRIX, g_current_modelview);
        } else if (g_current_matrix_mode == GL_PROJECTION) {
            glGetFloatv(GL_PROJECTION_MATRIX, g_current_projection);
            g_current_projection_is_ortho = (g_current_projection[15] == 1.0f);
        }
    }
}

static void bridge_gl_orthof(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    // ARM64 AAPCS64: all 6 float args in S0-S5
    float l = emu->get_sreg(0), r_ = emu->get_sreg(1);
    float b = emu->get_sreg(2), t = emu->get_sreg(3);
    float n = emu->get_sreg(4), f = emu->get_sreg(5);
    
    /* Track ortho calls for GUI detection — if only ortho and no frustum,
     * the game is showing a full-screen menu (map, inventory, etc.) */
    extern int g_frame_has_ortho;
    g_frame_has_ortho++;
    
    static int ortho_diag = 0;
    if (ortho_diag < 10) {
        std::cout << "[GL64-DIAG] glOrthof(l=" << l << ", r=" << r_ << ", b=" << b 
                  << ", t=" << t << ", n=" << n << ", f=" << f << ")" << std::endl;
        ortho_diag++;
    }
    GL_DIAG("glOrthof(l=%f, r=%f, b=%f, t=%f, n=%f, f=%f)", l, r_, b, t, n, f);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.orthof(l, r_, b, t, n, f);
        return;
    }
#endif
    if (l == r_ || b == t || n == f) {
        std::cout << "[GL] glOrthof with zero range detected! l=" << l << " r=" << r_ << " b=" << b << " t=" << t << " n=" << n << " f=" << f << std::endl;
    }
    if (g_display_active) {
        glOrtho(l, r_, b, t, n, f);
    }
    g_current_projection_is_ortho = true;
}


static void bridge_gl_translatef(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    // ARM64 AAPCS64: all-float args in S0-S2
    float x = emu->get_sreg(0), y = emu->get_sreg(1), z = emu->get_sreg(2);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.translatef(x, y, z);
        return;
    }
#endif
    if (g_display_active) {
        glTranslatef(x, y, z);
    }
}

static void bridge_gl_rotatef(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    // ARM64 AAPCS64: all-float args in S0-S3
    float angle = emu->get_sreg(0), x = emu->get_sreg(1);
    float y = emu->get_sreg(2), z = emu->get_sreg(3);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.rotatef(angle, x, y, z);
        return;
    }
#endif
    if (g_display_active) {
        glRotatef(angle, x, y, z);
    }
}

static void bridge_gl_scalef(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    // ARM64 AAPCS64: all-float args in S0-S2
    float x = emu->get_sreg(0), y = emu->get_sreg(1), z = emu->get_sreg(2);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.scalef(x, y, z);
        return;
    }
#endif
    if (g_display_active) {
        glScalef(x, y, z);
    }
}

static void bridge_gl_frustumf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.matrix_ops++;
    // ARM64 AAPCS64: all 6 float args in S0-S5
    float l = emu->get_sreg(0), r_ = emu->get_sreg(1);
    float b = emu->get_sreg(2), t = emu->get_sreg(3);
    float n = emu->get_sreg(4), f = emu->get_sreg(5);
    
    /* Track frustum calls for GUI detection — if frustum is called,
     * the game is rendering the 3D scene (not just a full-screen menu) */
    extern int g_frame_has_perspective;
    g_frame_has_perspective++;
    
    static int frust_diag = 0;
    if (frust_diag < 10) {
        std::cout << "[GL64-DIAG] glFrustumf(l=" << l << ", r=" << r_ << ", b=" << b 
                  << ", t=" << t << ", n=" << n << ", f=" << f << ")" << std::endl;
        frust_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.frustumf(l, r_, b, t, n, f);
        return;
    }
#endif
    if (g_display_active) {
        glFrustum(l, r_, b, t, n, f);
    }
    g_current_projection_is_ortho = false;
}

// --- Phase 3: Geometry Submission ---

// Current GL state for debugging
struct {
    uint32_t vptr_addr;
    GLint vptr_size;
    GLenum vptr_type;
    GLsizei vptr_stride;
    
    uint32_t tptr_addr;
    GLint tptr_size;
    GLenum tptr_type;
    GLsizei tptr_stride;

    uint32_t nptr_addr;
    GLenum nptr_type;
    GLsizei nptr_stride;
} g_gl_state;

// ── GL Render-State Handshake ──────────────────────────────────────────────
// These globals are written by the bridge hooks below every frame and
// consumed by fbo_scaler.cpp during the composite / PostFX pass.
// They give the PostFX pipeline accurate, live knowledge of what the game
// engine is actually telling OpenGL — light positions, material colors,
// current transform matrices, and the active vertex color — so post-
// processing effects can be light-aware and scene-reactive rather than
// relying purely on static per-preset settings.

GuestLightState g_frame_lights[8] = {};
float g_frame_light_model_ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};

GuestMaterialState g_frame_material = {
    {0.2f, 0.2f, 0.2f, 1.0f},   // ambient
    {0.8f, 0.8f, 0.8f, 1.0f},   // diffuse
    {0.0f, 0.0f, 0.0f, 1.0f},   // specular
    {0.0f, 0.0f, 0.0f, 1.0f},   // emission
    0.0f                         // shininess
};

// Column-major (OpenGL convention) matrix shadows
float g_current_modelview[16]  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
float g_current_projection[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// Normalised RGBA vertex / model color — last value set by glColor4f/glColor4ub
float g_current_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

// Host-side cached hero position (synchronized from guest memory each frame)
float g_host_hero_pos[3] = {0.0f, 0.0f, 0.0f};


// Color pointer tracking for draw batcher
static bool g_color_array_enabled = false;
static uint32_t g_color_ptr = 0;
static GLsizei g_color_stride = 0;
extern bool g_gl_force_identity;
extern bool g_gl_hide_hud;
static GLuint g_controls_atlas_tex_id = 0;    // Original atlas texture ID
static GLuint g_controls_hidden_tex = 0;       // Modified copy with controls zeroed out

// From asset_manager.c — diagnostic only. Asset opens and GL uploads are not
// one-to-one, so this must not be used as authoritative upload ownership.
extern "C" { extern char g_last_opened_asset[256]; }

// Control regions in 1024x1024 pixel space (parsed from ui_game_atlas_2x.atlas)
// Format: {x, y, w, h} — these are the touch button sprite regions to zero out
struct ControlRect { int x, y, w, h; };
static const ControlRect CONTROL_RECTS[] = {
    {197, 751,  56,  56},  // ui_controls_button
    {140, 751,  56,  56},  // ui_controls_button_pressed
    {483, 518,  82,  80},  // ui_controls_jump
    {838, 802,  96,  50},  // ui_controls_left
    {741, 805,  96,  50},  // ui_controls_right
    {417, 751, 104,  62},  // ui_controls_swing
    {166, 613,  74,  68},  // ui_controls_handle
};
static const int NUM_CONTROL_RECTS = sizeof(CONTROL_RECTS) / sizeof(CONTROL_RECTS[0]);

// Create a modified copy of the controls atlas with control regions zeroed out
static void create_hidden_controls_atlas(GLenum target, GLint level, GLint ifmt,
                                          int w, int h, GLint border,
                                          GLenum fmt, GLenum type, const void* pixels) {
    if (!pixels || w < 512 || h < 512) return;
    
    // Determine bytes per pixel
    int bpp = 4;  // Assume RGBA
    if (fmt == GL_RGB) bpp = 3;
    
    // Copy the pixel data
    size_t data_size = (size_t)w * h * bpp;
    uint8_t* modified = (uint8_t*)malloc(data_size);
    if (!modified) return;
    memcpy(modified, pixels, data_size);
    
    // Zero out control regions (with 2px padding for safety)
    for (int r = 0; r < NUM_CONTROL_RECTS; r++) {
        int rx = CONTROL_RECTS[r].x - 2;
        int ry = CONTROL_RECTS[r].y - 2;
        int rw = CONTROL_RECTS[r].w + 4;
        int rh = CONTROL_RECTS[r].h + 4;
        
        // Clamp to texture bounds
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
        
        // Zero out each row of this rectangle
        for (int row = ry; row < ry + rh; row++) {
            memset(modified + (row * w + rx) * bpp, 0, rw * bpp);
        }
    }
    
    // Create a new GL texture with the modified data
    glGenTextures(1, &g_controls_hidden_tex);
    glBindTexture(GL_TEXTURE_2D, g_controls_hidden_tex);
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, modified);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Rebind the original texture (the caller expects it still bound)
    glBindTexture(GL_TEXTURE_2D, g_controls_atlas_tex_id);
    
    free(modified);
    printf("[HUD] Created controls-hidden atlas copy (id=%u) — %d control regions zeroed\n",
           g_controls_hidden_tex, NUM_CONTROL_RECTS);
}

static void bridge_gl_draw_arrays(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mode = emu->get_reg(0);
    uint32_t first = emu->get_reg(1);
    uint32_t count = emu->get_reg(2);
    g_frame_stats.draw_calls++;
    g_frame_stats.vertices_submitted += count;
    
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.draw_arrays(mode, first, count, emu->get_memory_base());
        return;
    }
#endif

    if (g_gl_diag_enabled && g_gl_diag_frame < 3) {
        printf("[GL F%d] glDrawArrays(mode=0x%x, first=%d, count=%d)\n", g_gl_diag_frame, mode, first, count);
        uint8_t* memory = emu->get_memory_base();
        if (g_gl_state.vptr_addr && g_gl_state.vptr_type == GL_FLOAT) {
            const float* v = (const float*)(memory + g_gl_state.vptr_addr + first * g_gl_state.vptr_size * 4);
            printf("  -> Verts: [%.2f %.2f %.2f / %.2f %.2f %.2f / ...]\n", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
    }

    if (!g_display_active) return;

    // Try draw call batching
    if (g_batching_enabled && g_batcher.initialized && 
        g_gl_state.vptr_addr && g_gl_state.vptr_type == GL_FLOAT) {
        
        // Update batch state (blend is tracked via glEnable/glBlendFunc hooks)
        uint8_t* memory = emu->get_memory_base();
        
        bool batched = g_batcher.try_batch(
            g_current_batch_state, memory,
            g_gl_state.vptr_addr, g_gl_state.vptr_size, g_gl_state.vptr_stride,
            g_gl_state.tptr_addr, g_gl_state.tptr_size, g_gl_state.tptr_stride,
            g_color_ptr, g_color_stride, g_color_array_enabled,
            mode, first, count);
        
        if (batched) return; // Successfully batched — will be drawn later
        // Fall through to direct draw if can't batch
    }

    // Direct draw (fallback).
    extern PostFXState g_postfx;
    bool do_stencil = fbo_is_active() && g_postfx.enabled;

    if (do_stencil) {
        bool is_ui = g_current_projection_is_ortho;
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        if (is_ui) {
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        } else {
            glStencilFunc(GL_ALWAYS, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        }
    }

    glDrawArrays(mode, first, count);

    if (do_stencil) {
        glDisable(GL_STENCIL_TEST);
    }
}

static void bridge_gl_draw_elements(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t mode = emu->get_reg(0);
    uint32_t count = emu->get_reg(1);
    uint32_t type = emu->get_reg(2);
    uint32_t indices_ptr = emu->get_reg(3);
    g_frame_stats.draw_calls++;
    g_frame_stats.vertices_submitted += count;
    
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.draw_elements(mode, count, type, indices_ptr, emu->get_memory_base());
        return;
    }
#endif

    // ===== COMPREHENSIVE GL STATE DUMP on first 3 draw calls =====
    static int draw_diag_count = 0;
    if (draw_diag_count < 3 && g_display_active) {
        std::cout << "\n========== [GL64-STATE] Draw call #" << draw_diag_count << " ==========" << std::endl;
        std::cout << "  mode=0x" << std::hex << mode << " count=" << std::dec << count 
                  << " type=0x" << std::hex << type << " indices=0x" << indices_ptr << std::dec << std::endl;
        
        // Current bound texture
        GLint bound_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex);
        std::cout << "  Bound texture: " << bound_tex << std::endl;
        
        // Enabled states
        std::cout << "  GL_TEXTURE_2D: " << (glIsEnabled(GL_TEXTURE_2D) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_DEPTH_TEST: " << (glIsEnabled(GL_DEPTH_TEST) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_BLEND: " << (glIsEnabled(GL_BLEND) ? "ON" : "OFF") << std::endl;

        std::cout << "  GL_CULL_FACE: " << (glIsEnabled(GL_CULL_FACE) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_LIGHTING: " << (glIsEnabled(GL_LIGHTING) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_ALPHA_TEST: " << (glIsEnabled(GL_ALPHA_TEST) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_SCISSOR_TEST: " << (glIsEnabled(GL_SCISSOR_TEST) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_COLOR_ARRAY: " << (glIsEnabled(GL_COLOR_ARRAY) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_VERTEX_ARRAY: " << (glIsEnabled(GL_VERTEX_ARRAY) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_TEXTURE_COORD_ARRAY: " << (glIsEnabled(GL_TEXTURE_COORD_ARRAY) ? "ON" : "OFF") << std::endl;
        std::cout << "  GL_NORMAL_ARRAY: " << (glIsEnabled(GL_NORMAL_ARRAY) ? "ON" : "OFF") << std::endl;
        
        // Texture environment mode
        GLint tex_env_mode;
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &tex_env_mode);
        std::cout << "  TEX_ENV_MODE: 0x" << std::hex << tex_env_mode << std::dec 
                  << " (" << (tex_env_mode == 0x2100 ? "MODULATE" : 
                             tex_env_mode == 0x1E01 ? "REPLACE" :
                             tex_env_mode == 0x0104 ? "ADD" :
                             tex_env_mode == 0x2101 ? "DECAL" : 
                             tex_env_mode == 0x8570 ? "COMBINE" : "?") << ")" << std::endl;
        
        // GL_COLOR_MATERIAL
        std::cout << "  GL_COLOR_MATERIAL: " << (glIsEnabled(GL_COLOR_MATERIAL) ? "ON" : "OFF") << std::endl;
        
        // Color mask
        GLboolean cm[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        std::cout << "  ColorMask: R=" << (int)cm[0] << " G=" << (int)cm[1] 
                  << " B=" << (int)cm[2] << " A=" << (int)cm[3] << std::endl;
        
        // Viewport
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        std::cout << "  Viewport: " << vp[0] << "," << vp[1] << " " << vp[2] << "x" << vp[3] << std::endl;
        
        // Current color
        GLfloat cc[4];
        glGetFloatv(GL_CURRENT_COLOR, cc);
        std::cout << "  Current color: " << cc[0] << ", " << cc[1] << ", " << cc[2] << ", " << cc[3] << std::endl;
        
        // Matrix mode
        GLint mm;
        glGetIntegerv(GL_MATRIX_MODE, &mm);
        std::cout << "  Matrix mode: 0x" << std::hex << mm << std::dec << std::endl;
        
        // Projection matrix (first row to see if it's identity/zero/valid)
        GLfloat proj[16];
        glGetFloatv(GL_PROJECTION_MATRIX, proj);
        std::cout << "  Proj matrix row0: " << proj[0] << " " << proj[4] << " " << proj[8] << " " << proj[12] << std::endl;
        std::cout << "  Proj matrix row1: " << proj[1] << " " << proj[5] << " " << proj[9] << " " << proj[13] << std::endl;
        std::cout << "  Proj matrix row2: " << proj[2] << " " << proj[6] << " " << proj[10] << " " << proj[14] << std::endl;
        std::cout << "  Proj matrix row3: " << proj[3] << " " << proj[7] << " " << proj[11] << " " << proj[15] << std::endl;
        
        // Modelview matrix (first row)
        GLfloat mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        std::cout << "  MV matrix row0: " << mv[0] << " " << mv[4] << " " << mv[8] << " " << mv[12] << std::endl;
        std::cout << "  MV matrix row1: " << mv[1] << " " << mv[5] << " " << mv[9] << " " << mv[13] << std::endl;
        std::cout << "  MV matrix row2: " << mv[2] << " " << mv[6] << " " << mv[10] << " " << mv[14] << std::endl;
        std::cout << "  MV matrix row3: " << mv[3] << " " << mv[7] << " " << mv[11] << " " << mv[15] << std::endl;
        
        // Vertex data sample
        uint8_t* memory = emu->get_memory_base();
        if (g_gl_state.vptr_addr && g_gl_state.vptr_type == GL_FLOAT) {
            const float* v = (const float*)(memory + g_gl_state.vptr_addr);
            std::cout << "  VPtr @ 0x" << std::hex << g_gl_state.vptr_addr << std::dec 
                      << " size=" << g_gl_state.vptr_size << " stride=" << g_gl_state.vptr_stride << std::endl;
            std::cout << "  First verts: [" << v[0] << " " << v[1] << " " << v[2] << "] [" 
                      << v[3] << " " << v[4] << " " << v[5] << "]" << std::endl;
        }
        
        // Bound FBO (should be our FBO, not 0)
        GLint fbo;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        std::cout << "  Bound FBO: " << fbo << std::endl;
        
        // Which lights are enabled?
        for (int i = 0; i < 8; i++) {
            if (glIsEnabled(GL_LIGHT0 + i)) {
                std::cout << "  GL_LIGHT" << i << ": ENABLED" << std::endl;
                // Query this light's properties
                GLfloat pos[4], amb[4], dif[4];
                glGetLightfv(GL_LIGHT0 + i, GL_POSITION, pos);
                glGetLightfv(GL_LIGHT0 + i, GL_AMBIENT, amb);
                glGetLightfv(GL_LIGHT0 + i, GL_DIFFUSE, dif);
                std::cout << "    Position: " << pos[0] << ", " << pos[1] << ", " << pos[2] << ", " << pos[3] << std::endl;
                std::cout << "    Ambient: " << amb[0] << ", " << amb[1] << ", " << amb[2] << ", " << amb[3] << std::endl;
                std::cout << "    Diffuse: " << dif[0] << ", " << dif[1] << ", " << dif[2] << ", " << dif[3] << std::endl;
            }
        }
        if (!glIsEnabled(GL_LIGHT0) && !glIsEnabled(GL_LIGHT1)) {
            std::cout << "  *** NO LIGHTS ENABLED! This explains black rendering! ***" << std::endl;
        }
        
        // Material properties
        GLfloat mat_amb[4], mat_dif[4];
        glGetMaterialfv(GL_FRONT, GL_AMBIENT, mat_amb);
        glGetMaterialfv(GL_FRONT, GL_DIFFUSE, mat_dif);
        std::cout << "  Material ambient: " << mat_amb[0] << ", " << mat_amb[1] << ", " << mat_amb[2] << ", " << mat_amb[3] << std::endl;
        std::cout << "  Material diffuse: " << mat_dif[0] << ", " << mat_dif[1] << ", " << mat_dif[2] << ", " << mat_dif[3] << std::endl;
        
        // GL errors
        GLenum err = glGetError();
        std::cout << "  GL error: " << err << std::endl;
        std::cout << "=========================================\n" << std::endl;
        draw_diag_count++;
    }

    if (g_gl_diag_enabled && g_gl_diag_frame < 3) {
        printf("[GL F%d] glDrawElements(mode=0x%x, count=%d, type=0x%x, indices=@0x%x)\n", g_gl_diag_frame, mode, count, type, indices_ptr);
        uint8_t* memory = emu->get_memory_base();
        if (g_gl_state.vptr_addr && g_gl_state.vptr_type == GL_FLOAT) {
            const float* v = (const float*)(memory + g_gl_state.vptr_addr);
            printf("  -> Verts: [%.2f %.2f %.2f / %.2f %.2f %.2f / ...]\n", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
    }

    if (g_display_active) {
        extern PostFXState g_postfx;
        bool do_stencil = fbo_is_active() && g_postfx.enabled;

        if (do_stencil) {
            bool is_ui = g_current_projection_is_ortho;
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            if (is_ui) {
                glStencilFunc(GL_ALWAYS, 1, 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            } else {
                glStencilFunc(GL_ALWAYS, 0, 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            }
        }

        uint8_t* memory = emu->get_memory_base();
        glDrawElements(mode, count, type, (const void*)(memory + indices_ptr));

        if (do_stencil) {
            glDisable(GL_STENCIL_TEST);
        }
    }
}


// glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
static void bridge_gl_vertex_pointer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.vertex_pointer_calls++;
    
    uint32_t size = emu->get_reg(0);
    uint32_t type = emu->get_reg(1);
    uint32_t stride = emu->get_reg(2);
    uint32_t ptr = emu->get_reg(3);
    
    g_gl_state.vptr_addr = ptr;
    g_gl_state.vptr_size = size;
    g_gl_state.vptr_type = type;
    g_gl_state.vptr_stride = stride;

#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.vertex_pointer(size, type, stride, ptr);
        return;
    }
#endif

    log_vertex_sample(emu, ptr, size, type, stride, "glVertexPointer");
    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        GL_DIAG("glVertexPointer(size=%d, type=0x%x, stride=%d, ptr=0x%x -> host=%p)", size, type, stride, ptr, (void*)(memory + ptr));
        glVertexPointer(size, type, stride, (const void*)(memory + ptr));
    }
}


static void bridge_gl_texcoord_pointer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.texcoord_pointer_calls++;
    
    uint32_t size = emu->get_reg(0);
    uint32_t type = emu->get_reg(1);
    uint32_t stride = emu->get_reg(2);
    uint32_t ptr = emu->get_reg(3);
    
    g_gl_state.tptr_addr = ptr;
    g_gl_state.tptr_size = size;
    g_gl_state.tptr_type = type;
    g_gl_state.tptr_stride = stride;

#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.texcoord_pointer(size, type, stride, ptr);
        return;
    }
#endif

    log_vertex_sample(emu, ptr, size, type, stride, "glTexCoordPointer");
    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        GL_DIAG("glTexCoordPointer(size=%d, type=0x%x, stride=%d, ptr=0x%x -> host=%p)", size, type, stride, ptr, (void*)(memory + ptr));
        glTexCoordPointer(size, type, stride, (const void*)(memory + ptr));
    }
}

static void bridge_gl_color_pointer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.color_pointer_calls++;
    uint32_t size = emu->get_reg(0);
    uint32_t type = emu->get_reg(1);
    uint32_t stride = emu->get_reg(2);
    uint32_t ptr = emu->get_reg(3);
    g_color_ptr = ptr;
    g_color_stride = stride;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.color_pointer(size, type, stride, ptr);
        return;
    }
#endif
    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        GL_DIAG("glColorPointer(size=%d, type=0x%x, stride=%d, ptr=0x%x -> host=%p)", size, type, stride, ptr, (void*)(memory + ptr));
        glColorPointer(size, type, stride, (const void*)(memory + ptr));
    }
}

static void bridge_gl_normal_pointer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    
    uint32_t type = emu->get_reg(0);
    uint32_t stride = emu->get_reg(1);
    uint32_t ptr = emu->get_reg(2);
    
    g_gl_state.nptr_addr = ptr;
    g_gl_state.nptr_type = type;
    g_gl_state.nptr_stride = stride;

#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.normal_pointer(type, stride, ptr);
        return;
    }
#endif

    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        glNormalPointer(type, stride, (const void*)(memory + ptr));
    }
}

static void bridge_gl_enable_client_state(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glEnableClientState(0x%x) %s", emu->get_reg(0), emu->get_reg(0)==0x8074?"VERTEX_ARRAY":emu->get_reg(0)==0x8078?"TEXTURE_COORD_ARRAY":"OTHER");
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.enable_client_state(emu->get_reg(0));
        return;
    }
#endif
    GLenum arr = emu->get_reg(0);
    if (g_batching_enabled && arr == GL_COLOR_ARRAY) g_color_array_enabled = true;
    if (g_display_active) glEnableClientState(arr);
}

static void bridge_gl_disable_client_state(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glDisableClientState(0x%x)", emu->get_reg(0));
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.disable_client_state(emu->get_reg(0));
        return;
    }
#endif
    GLenum arr = emu->get_reg(0);
    if (g_batching_enabled && arr == GL_COLOR_ARRAY) g_color_array_enabled = false;
    if (g_display_active) glDisableClientState(arr);
}

// --- Phase 4: Textures ---

static std::unordered_map<uint32_t, bool> g_seen_textures;

extern bool g_gl_force_white;

static void bridge_gl_bind_texture(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t target = emu->get_reg(0);
    uint32_t tex_id = emu->get_reg(1);
    tex_id = RedstellGC::instance().get_redirected_texture(tex_id);
    g_frame_stats.texture_binds++;
    g_frame_stats.last_bound_texture = tex_id;
    if (tex_id && !g_seen_textures.count(tex_id)) {
        g_seen_textures[tex_id] = true;
        g_frame_stats.unique_textures_bound++;
    }
    GL_DIAG("glBindTexture(target=0x%x, id=%d)", target, tex_id);
    TEX_LOG("glBindTexture(target=0x%x, id=%u)", target, tex_id);
    
    // Track texture for draw batcher
    if (g_batching_enabled && target == GL_TEXTURE_2D) {
        if (g_current_batch_state.texture_id != tex_id) {
            g_batcher.flush(); // Texture changed → flush pending batch
        }
        g_current_batch_state.texture_id = tex_id;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.bind_texture(target, tex_id);
        return;
    }
#endif
    if (g_display_active) {
        // Controls hiding: swap the controls atlas with our modified copy
        // that has only the touch button regions zeroed out.
        // Health bars, mana, coins, settings icons etc. stay visible.
        if (g_gl_hide_hud && tex_id != 0 && g_controls_hidden_tex
            && g_controls_atlas_tex_id != 0 && tex_id == g_controls_atlas_tex_id) {
            glBindTexture(target, g_controls_hidden_tex);
            if (g_gl_force_white) {
                glEnable(GL_TEXTURE_2D);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            }
            return;
        }
        // Video background: upload next video frame before rendering if registered
        if (tex_id != 0) {
            VideoBackground::update_texture_maybe(tex_id);
        }
        glBindTexture(target, tex_id);
        if (g_gl_force_white) {
            if (tex_id == 0) {
                glDisable(GL_TEXTURE_2D);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            } else {
                glEnable(GL_TEXTURE_2D);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
    }
}

// glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels)
// R0=target, R1=level, R2=internalformat, R3=width, stack: height, border, format, type, pixels
static void bridge_gl_tex_image_2d(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.tex_uploads++;
    uint8_t* memory = emu->get_memory_base();
    // ARM64: 9 args. X0-X7 hold first 8, 9th on stack
    uint32_t target = (uint32_t)emu->get_reg(0);
    uint32_t level = (uint32_t)emu->get_reg(1);
    uint32_t internalformat = (uint32_t)emu->get_reg(2);
    uint32_t width = (uint32_t)emu->get_reg(3);
    uint32_t height = (uint32_t)emu->get_reg(4);
    uint32_t border = (uint32_t)emu->get_reg(5);
    uint32_t format = (uint32_t)emu->get_reg(6);
    uint32_t type = (uint32_t)emu->get_reg(7);
    // 9th arg on stack
    // ARM64: SP is register 31, NOT 13 (13 is ARM32's SP)
    uint64_t sp = emu->get_reg(31);
    uint32_t pixels_ptr = (uint32_t)*(uint64_t*)(memory + sp); // ARM64: 8-byte stack slots
    TEX_LOG("glTexImage2D(%ux%u level=%u ifmt=0x%x fmt=0x%x type=0x%x pixels=0x%x)",
            width, height, level, internalformat, format, type, pixels_ptr);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        const void* pixels = pixels_ptr ? (const void*)(memory + pixels_ptr) : nullptr;
        g_vk_backend.tex_image_2d(target, level, internalformat, width, height, border, format, type, pixels);
        return;
    }
#endif
    if (g_display_active) {
        const void* pixels = pixels_ptr ? (const void*)(memory + pixels_ptr) : nullptr;
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        
        size_t bpp = (format == GL_RGB) ? 3 : ((format == GL_ALPHA) ? 1 : 4);
        RedstellGC::instance().track_gl_tex_image(
            g_frame_stats.last_bound_texture, width, height, internalformat,
            g_last_opened_asset, pixels, width * height * bpp
        );

        /* Tag the controls atlas only once. g_last_opened_asset is global and
         * can be stale when deferred texture uploads occur; repeatedly tagging
         * would make an unrelated mob/item texture become the HUD replacement
         * and corrupt the screen when a keyboard action toggles the HUD. */
        if (g_controls_atlas_tex_id == 0 && level == 0 && pixels &&
            width == 1024 && height == 1024 &&
            strstr(g_last_opened_asset, "ui_game_atlas_2x") != nullptr &&
            strstr(g_last_opened_asset, "ui_game2") == nullptr) {
            g_controls_atlas_tex_id = g_frame_stats.last_bound_texture;
            printf("[HUD] Tagged controls atlas: texture ID=%u (from %s, %ux%u)\n",
                   g_controls_atlas_tex_id, g_last_opened_asset, width, height);
            // Create the modified copy with control regions zeroed out
            create_hidden_controls_atlas(target, level, internalformat,
                                          width, height, border, format, type, pixels);
        }
        // Video background: register texture if a .mp4 video version exists
        if (g_last_opened_asset[0] != '\0') {
            VideoBackground::register_texture_maybe(
                g_frame_stats.last_bound_texture, g_last_opened_asset, width, height, pixels, format, type);
        }
    }
}

static void bridge_gl_tex_sub_image_2d(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.tex_uploads++;
    uint8_t* memory = emu->get_memory_base();
    // ARM64: 9 args. X0-X7 hold first 8, 9th on stack
    uint32_t target = (uint32_t)emu->get_reg(0);
    uint32_t level = (uint32_t)emu->get_reg(1);
    uint32_t xoff = (uint32_t)emu->get_reg(2);
    uint32_t yoff = (uint32_t)emu->get_reg(3);
    uint32_t width = (uint32_t)emu->get_reg(4);
    uint32_t height = (uint32_t)emu->get_reg(5);
    uint32_t format = (uint32_t)emu->get_reg(6);
    uint32_t type = (uint32_t)emu->get_reg(7);
    // 9th arg on stack
    // ARM64: SP is register 31, NOT 13 (13 is ARM32's SP)
    uint64_t sp = emu->get_reg(31);
    uint32_t pixels_ptr = (uint32_t)*(uint64_t*)(memory + sp); // ARM64: 8-byte stack slots
    TEX_LOG("glTexSubImage2D(%ux%u @%u,%u level=%u fmt=0x%x type=0x%x pixels=0x%x)",
            width, height, xoff, yoff, level, format, type, pixels_ptr);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        const void* pixels = pixels_ptr ? (const void*)(memory + pixels_ptr) : nullptr;
        g_vk_backend.tex_sub_image_2d(target, level, xoff, yoff, width, height, format, type, pixels);
        return;
    }
#endif
    if (g_display_active) {
        const void* pixels = pixels_ptr ? (const void*)(memory + pixels_ptr) : nullptr;
        glTexSubImage2D(target, level, xoff, yoff, width, height, format, type, pixels);
    }
}

// ========================== ETC1 Software Decoder ==========================
static inline int etc1_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static const int etc1_modifiers[8][2] = {
    {  2,   8}, {  5,  17}, {  9,  29}, { 13,  42},
    { 18,  56}, { 24,  71}, { 33,  92}, { 47, 127}
};

static void decode_etc1_block(const uint8_t* src, uint8_t* dst, int dst_stride) {
    // Read 64-bit block (big-endian)
    uint64_t block = 0;
    for (int i = 0; i < 8; i++)
        block = (block << 8) | src[i];
    
    int diff  = (block >> 33) & 1;
    int flip  = (block >> 32) & 1;
    int table1 = (block >> 37) & 7;
    int table2 = (block >> 34) & 7;
    
    int r1, g1, b1, r2, g2, b2;
    
    if (diff == 0) {
        // Individual mode: two RGB444 colors
        int rr1 = (block >> 60) & 0xF; r1 = (rr1 << 4) | rr1;
        int rr2 = (block >> 56) & 0xF; r2 = (rr2 << 4) | rr2;
        int gg1 = (block >> 52) & 0xF; g1 = (gg1 << 4) | gg1;
        int gg2 = (block >> 48) & 0xF; g2 = (gg2 << 4) | gg2;
        int bb1 = (block >> 44) & 0xF; b1 = (bb1 << 4) | bb1;
        int bb2 = (block >> 40) & 0xF; b2 = (bb2 << 4) | bb2;
    } else {
        // Differential mode: RGB555 + RGB333 delta
        int r = (block >> 59) & 0x1F;
        int dr = (block >> 56) & 0x7; if (dr > 3) dr -= 8;
        int g = (block >> 51) & 0x1F;
        int dg = (block >> 48) & 0x7; if (dg > 3) dg -= 8;
        int b = (block >> 43) & 0x1F;
        int db = (block >> 40) & 0x7; if (db > 3) db -= 8;
        
        r1 = (r << 3) | (r >> 2);
        int r2v = r + dr; r2 = (r2v << 3) | (r2v >> 2);
        g1 = (g << 3) | (g >> 2);
        int g2v = g + dg; g2 = (g2v << 3) | (g2v >> 2);
        b1 = (b << 3) | (b >> 2);
        int b2v = b + db; b2 = (b2v << 3) | (b2v >> 2);
    }
    
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            int pixel_idx = col * 4 + row;
            
            int msb = (block >> (pixel_idx + 16)) & 1;
            int lsb = (block >> pixel_idx) & 1;
            
            // Determine sub-block
            int sub;
            if (flip == 0) sub = (col >= 2) ? 1 : 0;
            else           sub = (row >= 2) ? 1 : 0;
            
            int rb = sub ? r2 : r1;
            int gb = sub ? g2 : g1;
            int bb = sub ? b2 : b1;
            int table = sub ? table2 : table1;
            
            int mod = etc1_modifiers[table][lsb];
            if (msb) mod = -mod;
            
            uint8_t* pixel = dst + row * dst_stride + col * 4;
            pixel[0] = etc1_clamp(rb + mod);
            pixel[1] = etc1_clamp(gb + mod);
            pixel[2] = etc1_clamp(bb + mod);
            pixel[3] = 255; // ETC1 is always opaque
        }
    }
}

static void bridge_gl_compressed_tex_image_2d(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.tex_uploads++;
    uint8_t* memory = emu->get_memory_base();
    // ARM64: 8 args all fit in X0-X7
    uint32_t target = (uint32_t)emu->get_reg(0);
    uint32_t level = (uint32_t)emu->get_reg(1);
    uint32_t internalformat = (uint32_t)emu->get_reg(2);
    uint32_t width = (uint32_t)emu->get_reg(3);
    uint32_t height = (uint32_t)emu->get_reg(4);
    uint32_t border = (uint32_t)emu->get_reg(5);
    uint32_t image_size = (uint32_t)emu->get_reg(6);
    uint32_t data_ptr = (uint32_t)emu->get_reg(7);
    
    // ETC1_RGB8_OES = 0x8D64
    if (!g_display_active || data_ptr == 0) return;
    
    if (internalformat == 0x8D64) {
        // Decode ETC1 to RGBA8888
        const uint8_t* src = memory + data_ptr;
        uint32_t block_w = (width + 3) / 4;
        uint32_t block_h = (height + 3) / 4;
        
        std::vector<uint8_t> rgba(width * height * 4, 255);
        
        for (uint32_t by = 0; by < block_h; by++) {
            for (uint32_t bx = 0; bx < block_w; bx++) {
                uint8_t block_rgba[4 * 4 * 4]; // 4x4 pixels * RGBA
                decode_etc1_block(src + (by * block_w + bx) * 8, block_rgba, 4 * 4);
                
                // Copy decoded block to output (clamp to image bounds)
                for (int row = 0; row < 4 && (by * 4 + row) < height; row++) {
                    for (int col = 0; col < 4 && (bx * 4 + col) < width; col++) {
                        int dst_x = bx * 4 + col;
                        int dst_y = by * 4 + row;
                        memcpy(&rgba[(dst_y * width + dst_x) * 4],
                               &block_rgba[row * 16 + col * 4], 4);
                    }
                }
            }
        }
        
#ifdef VULKAN_BACKEND
        if (g_graphics_api == GraphicsAPI::VULKAN) {
            g_vk_backend.tex_image_2d(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else
#endif
        if (g_display_active) {
            glTexImage2D(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        }
        
        RedstellGC::instance().track_gl_tex_image(
            g_frame_stats.last_bound_texture, width, height, GL_RGBA,
            g_last_opened_asset, rgba.data(), rgba.size()
        );
        std::cout << "[ETC1] Decoded " << width << "x" << height << " texture (" 
                  << (block_w * block_h) << " blocks)" << std::endl;
    } else if (internalformat == 0x8C00 || internalformat == 0x8C01 ||
               internalformat == 0x8C02 || internalformat == 0x8C03) {
        // Decode PVRTC to RGBA8888
        const uint8_t* src = memory + data_ptr;
        uint32_t do2bitMode = (internalformat == 0x8C01 || internalformat == 0x8C03) ? 1 : 0;
        
        std::vector<uint8_t> rgba(width * height * 4, 255);
        pvr::PVRTDecompressPVRTC(src, do2bitMode, width, height, rgba.data());
        
#ifdef VULKAN_BACKEND
        if (g_graphics_api == GraphicsAPI::VULKAN) {
            g_vk_backend.tex_image_2d(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else
#endif
        if (g_display_active) {
            glTexImage2D(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        }
        
        RedstellGC::instance().track_gl_tex_image(
            g_frame_stats.last_bound_texture, width, height, GL_RGBA,
            g_last_opened_asset, rgba.data(), rgba.size()
        );
        std::cout << "[PVRTC] Decoded " << width << "x" << height << " texture (format=0x" 
                  << std::hex << internalformat << std::dec << ")" << std::endl;
    } else {
        std::cout << "[GL] glCompressedTexImage2D: unknown format 0x" 
                  << std::hex << internalformat << std::dec << " — skipped" << std::endl;
    }
}


static void bridge_PVRTTextureLoadFromPVRBuffer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    
    uint32_t file_data_ptr = (uint32_t)emu->get_reg(0);
    uint32_t file_size = (uint32_t)emu->get_reg(1);
    uint32_t out_tex_name_ptr = (uint32_t)emu->get_reg(2);
    uint64_t image_ptr = emu->get_reg(3);      // X3 = Image* temp
    uint32_t out_height_ptr = (uint32_t)emu->get_reg(6); // X6 = height out (pTVar11) -> writes width to HEIGHT field
    uint32_t out_width_ptr = (uint32_t)emu->get_reg(7);  // X7 = width out  (pTVar10) -> writes height to WIDTH field
    
    if (file_data_ptr == 0) {
        std::cerr << "[SRE-PVR] Error: file_data_ptr is NULL" << std::endl;
        return;
    }
    
    RedstellGC::PPDTexture ppd_tex;
    bool cache_hit = false;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_buffer_to_path_mutex);
        if (g_buffer_to_path.count(file_data_ptr)) {
            path = g_buffer_to_path[file_data_ptr];
            g_buffer_to_path.erase(file_data_ptr);
        }
    }
    if (!path.empty()) {
        cache_hit = RedstellGC::instance().ppd_get_texture(path, ppd_tex);
    }
    
    int width = 0, height = 0;
    const uint8_t* pixel_data = nullptr;
    int format_type = -1;
    uint32_t gl_format = GL_RGBA;
    uint32_t gl_type = GL_UNSIGNED_BYTE;
    int bpp = 4;
    char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    uint8_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    int caver_format = 1;
    const uint8_t* upload_pixels = nullptr;
    std::vector<uint8_t> rgba;

    if (cache_hit) {
        width = ppd_tex.width;
        height = ppd_tex.height;
        gl_format = ppd_tex.gl_format;
        gl_type = ppd_tex.gl_type;
        caver_format = ppd_tex.caver_format;
        upload_pixels = ppd_tex.rgba.data();
    } else {
        const uint8_t* file_data = memory + file_data_ptr;
        const HostPVRv3Header* v3 = (const HostPVRv3Header*)file_data;
        if (v3->version == 0x03525650) {
            width = v3->width;
            height = v3->height;
            pixel_data = file_data + sizeof(HostPVRv3Header) + v3->metadata_size;
            format_type = pvr::ParsePVRv3Format(v3->pixel_format, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        } else {
            const HostPVRv2Header* v2 = (const HostPVRv2Header*)file_data;
            if (v2->magic == 0x21525650 || v2->header_size == 44) {
                width = v2->width;
                height = v2->height;
                pixel_data = file_data + v2->header_size;
                format_type = pvr::ParsePVRv2Format(v2->flags, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
            } else {
                std::cerr << "[SRE-PVR] Unknown PVR header magic 0x" << std::hex << v3->version << std::dec << std::endl;
                return;
            }
        }
        
        if (format_type < 0) {
            std::cerr << "[SRE-PVR] Unsupported format decoding " << width << "x" << height << std::endl;
            return;
        }
        
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            std::cerr << "[SRE-PVR] Invalid PVR dimensions " << width << "x" << height << std::endl;
            return;
        }
        
        if (format_type == 1) caver_format = 1;
        else if (format_type == 2 || format_type == 3) caver_format = 1;
        else if (format_type >= 4 && format_type <= 6) caver_format = 1;
        else if (format_type == 10) {
            if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 8 && d1 == 8 && d2 == 8 && d3 == 8) caver_format = 1;
            else if (c0 == 'b' && c1 == 'g' && c2 == 'r' && c3 == 'a' && d0 == 8 && d1 == 8 && d2 == 8 && d3 == 8) caver_format = 1;
            else if (c0 == 'l' && c1 == 'a' && d0 == 8 && d1 == 8) caver_format = 4;
            else if (c0 == 'l' && d0 == 8) caver_format = 6;
            else if (c0 == 'a' && d0 == 8) caver_format = 7;
            else if (c0 == 'r' && c1 == 'g' && c2 == 'b' && d0 == 5 && d1 == 6 && d2 == 5) caver_format = 5;
            else if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 4 && d1 == 4 && d2 == 4 && d3 == 4) caver_format = 2;
            else if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 5 && d1 == 5 && d2 == 5 && d3 == 1) caver_format = 3;
            else caver_format = 1;
        }

        if (format_type == 10) { // Uncompressed
            rgba.assign(pixel_data, pixel_data + width * height * bpp);
            upload_pixels = rgba.data();
            if (gl_format == GL_BGRA) { // BGRA -> swap to RGBA
                for (size_t i = 0; i < rgba.size(); i += 4) {
                    uint8_t b = rgba[i];
                    rgba[i] = rgba[i+2];
                    rgba[i+2] = b;
                }
                gl_format = GL_RGBA;
                caver_format = 1;
            }
        } else {
            // Decode to RGBA8888
            rgba.assign(width * height * 4, 255);
            bool decode_success = false;
            if (format_type == 1) { // ETC1
                pvr::PVRTDecompressETC(pixel_data, width, height, rgba.data(), 6);
                decode_success = true;
            } else if (format_type == 2 || format_type == 3) { // PVRTC
                uint32_t do2bitMode = (format_type == 2) ? 1 : 0;
                pvr::PVRTDecompressPVRTC(pixel_data, do2bitMode, width, height, rgba.data());
                decode_success = true;
            } else if (format_type >= 4 && format_type <= 6) { // DXT
                uint32_t dxt_fmt = (format_type == 4) ? 1 : ((format_type == 5) ? 3 : 5);
                pvr::PVRTDecompressDXT(pixel_data, width, height, rgba.data(), dxt_fmt);
                decode_success = true;
            }
            
            if (!decode_success) {
                std::cerr << "[SRE-PVR] Decoding failed for format_type: " << format_type << std::endl;
                return;
            }
            upload_pixels = rgba.data();
            gl_format = GL_RGBA;
            gl_type = GL_UNSIGNED_BYTE;
        }
    }
    
    // Generate texture ID if not already generated
    uint32_t tex_name = 0;
    if (out_tex_name_ptr) {
        tex_name = *(uint32_t*)(memory + out_tex_name_ptr);
    }
    if (tex_name == 0) {
        GLuint gl_tex;
        glGenTextures(1, &gl_tex);
        tex_name = gl_tex;
        if (out_tex_name_ptr) {
            *(uint32_t*)(memory + out_tex_name_ptr) = tex_name;
        }
    }

    if (!cache_hit && !g_pvr_software_decode) {
        const uint8_t* file_data = memory + file_data_ptr;
        if (pvr_upload_compressed_to_tex(file_data, file_size, tex_name, width, height)) {
            caver_format = 1;
            if (out_height_ptr) {
                *(uint32_t*)(memory + out_height_ptr) = (uint32_t)width;
            }
            if (out_width_ptr) {
                *(uint32_t*)(memory + out_width_ptr) = (uint32_t)height;
            }
            if (image_ptr) {
                *(int32_t*)(memory + image_ptr + 4) = (int32_t)height;
                *(int32_t*)(memory + image_ptr + 8) = (int32_t)width;
                *(int32_t*)(memory + image_ptr + 12) = (int32_t)caver_format;
            }
            if (!emu->quiet_mode) {
                std::cerr << "[SRE-PVR] Uploaded compressed " << width << "x" << height
                          << " format=" << caver_format << " -> GL tex " << tex_name << std::endl;
            }
            emu->set_reg(0, 0);
            return;
        }
    }
    
    glBindTexture(GL_TEXTURE_2D, tex_name);
    
    glTexImage2D(GL_TEXTURE_2D, 0, gl_format, width, height, 0, gl_format, gl_type, upload_pixels);
    bool is_npot = ((width & (width - 1)) != 0) || ((height & (height - 1)) != 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, is_npot ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, is_npot ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    // Write swapped dimensions to Texture struct (X6=height_ptr gets width, X7=width_ptr gets height)
    // to match guest engine's compiled scaling calculation and struct layout expectations.
    if (out_height_ptr) {  /* X6 = height field in Texture struct gets width */
        *(uint32_t*)(memory + out_height_ptr) = (uint32_t)width;
    }
    if (out_width_ptr) {   /* X7 = width field in Texture struct gets height */
        *(uint32_t*)(memory + out_width_ptr) = (uint32_t)height;
    }
    
    // Populate Image temp struct at X3 (so local scaling factors are calculated correctly by guest)
    if (image_ptr) {
        *(int32_t*)(memory + image_ptr + 4) = (int32_t)height; // local_7c (Image width gets height)
        *(int32_t*)(memory + image_ptr + 8) = (int32_t)width;  // iStack_78 (Image height gets width)
        *(int32_t*)(memory + image_ptr + 12) = (int32_t)caver_format; // format: caver_format
    }

    if (!emu->quiet_mode) {
        std::cerr << "[SRE-PVR] Uploaded " << width << "x" << height
                  << " format=" << caver_format << " -> GL tex " << tex_name << std::endl;
    }

    /* Return 0 = success so the engine proceeds with UV setup */
    emu->set_reg(0, 0);
}

static void bridge_gl_tex_parameteri(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.tex_parameteri(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2));
        return;
    }
#endif
    if (g_display_active) {
        glTexParameteri(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2));
    }
}

static void bridge_gl_tex_parameterf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    // ARM64 AAPCS64: float arg in S0
    float val = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.tex_parameterf(emu->get_reg(0), emu->get_reg(1), val);
        return;
    }
#endif
    if (g_display_active) {
        glTexParameterf(emu->get_reg(0), emu->get_reg(1), val);
    }
}

// --- Phase 5: State & Blending ---

static void bridge_gl_enable(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    GLenum cap = (GLenum)emu->get_reg(0);
    GL_DIAG("glEnable(0x%x)", cap);
    
    // Log light-related enables (GL_LIGHT0=0x4000 .. GL_LIGHT7=0x4007, GL_LIGHTING=0x0B50)
    static int en_diag = 0;
    if (en_diag < 20 && (cap >= 0x4000 && cap <= 0x4007 || cap == 0x0B50)) {
        std::cout << "[GL64-LIGHT] glEnable(0x" << std::hex << cap << std::dec << ") = "
                  << (cap == 0x0B50 ? "GL_LIGHTING" : "GL_LIGHT") << (cap >= 0x4000 ? (int)(cap - 0x4000) : -1) << std::endl;
        en_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.enable(cap);
        return;
    }
#endif
    // Track blend/alpha state for batcher
    if (g_batching_enabled) {
        if (cap == GL_BLEND) {
            if (!g_current_batch_state.blend_enabled) g_batcher.flush();
            g_current_batch_state.blend_enabled = true;
        } else if (cap == GL_ALPHA_TEST) {
            if (!g_current_batch_state.alpha_test_enabled) g_batcher.flush();
            g_current_batch_state.alpha_test_enabled = true;
        }
    }
    if (g_display_active) glEnable(cap);

    // ── Handshake: track per-light enable ────────────────────────────────
    if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7)
        g_frame_lights[cap - GL_LIGHT0].enabled = true;
}

static void bridge_gl_disable(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glDisable(0x%x)", emu->get_reg(0));
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.disable(emu->get_reg(0));
        return;
    }
#endif
    GLenum cap = emu->get_reg(0);
    if (g_batching_enabled) {
        if (cap == GL_BLEND) {
            if (g_current_batch_state.blend_enabled) g_batcher.flush();
            g_current_batch_state.blend_enabled = false;
        } else if (cap == GL_ALPHA_TEST) {
            if (g_current_batch_state.alpha_test_enabled) g_batcher.flush();
            g_current_batch_state.alpha_test_enabled = false;
        }
    }
    if (g_display_active) glDisable(cap);

    // ── Handshake: track per-light disable ───────────────────────────────
    if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7)
        g_frame_lights[cap - GL_LIGHT0].enabled = false;
}

static void bridge_gl_blend_func(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.blend_func(emu->get_reg(0), emu->get_reg(1));
        return;
    }
#endif
    GLenum src = emu->get_reg(0), dst = emu->get_reg(1);
    if (g_batching_enabled) {
        if (g_current_batch_state.blend_src != src || g_current_batch_state.blend_dst != dst) {
            g_batcher.flush();
        }
        g_current_batch_state.blend_src = src;
        g_current_batch_state.blend_dst = dst;
    }
    if (g_display_active) glBlendFunc(src, dst);
}

static void bridge_gl_depth_func(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.depth_func(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glDepthFunc(emu->get_reg(0));
}

static void bridge_gl_depth_mask(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.depth_mask(emu->get_reg(0) != 0);
        return;
    }
#endif
    if (g_display_active) glDepthMask(emu->get_reg(0));
}

static void bridge_gl_color4f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    // ARM64 AAPCS64: all-float args in S0-S3
    float r = emu->get_sreg(0), g = emu->get_sreg(1);
    float b = emu->get_sreg(2), a = emu->get_sreg(3);
    
    static int c4f_diag = 0;
    if (c4f_diag < 10) {
        std::cout << "[GL64-DIAG] glColor4f(" << r << ", " << g << ", " << b << ", " << a << ")" << std::endl;
        c4f_diag++;
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.color4f(r, g, b, a);
        return;
    }
#endif
    if (g_display_active) {
        GL_DIAG("glColor4f(%f, %f, %f, %f)", r, g, b, a);
        glColor4f(r, g, b, a);
    }
    // ── Handshake: track current color ────────────────────────────────────
    g_current_color[0] = r; g_current_color[1] = g;
    g_current_color[2] = b; g_current_color[3] = a;
}

static void bridge_gl_scissor(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.scissor(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
        return;
    }
#endif
    if (g_display_active) glScissor(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
}

static void bridge_gl_color_mask(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.color_mask(emu->get_reg(0) != 0, emu->get_reg(1) != 0, emu->get_reg(2) != 0, emu->get_reg(3) != 0);
        return;
    }
#endif
    if (g_display_active) glColorMask(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
}

static void bridge_gl_pixel_storei(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.pixel_storei(emu->get_reg(0), emu->get_reg(1));
        return;
    }
#endif
    if (g_display_active) glPixelStorei(emu->get_reg(0), emu->get_reg(1));
}

// Player position guest addresses for GLES2 lighting (defined in main.cpp)
extern uint64_t g_sre_hero_pos_x_addr;
extern uint64_t g_sre_hero_pos_y_addr;
extern uint64_t g_sre_hero_pos_z_addr;

// UPGRADED SHADERS FOR GLES2 LIGHTING
// NOTE: These are desktop GLSL 1.20 compatible — no ES precision qualifiers.
// The GLES ES 'precision mediump float;' and similar qualifiers are syntax errors
// on desktop OpenGL 2.1 Compatibility Profile (Mesa/Linux drivers).
// We define them away as empty macros and always prepend the version header.
const char* UPGRADED_VERT_SHADER = 
    "#version 120\n"
    "#define lowp\n"
    "#define mediump\n"
    "#define highp\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_texCoord;\n"
    "varying vec3 v_world_pos;\n"
    "uniform mat4 mvpMatrix;\n"
    "void main() {\n"
    "    v_color = gl_Color;\n"
    "    v_texCoord = gl_MultiTexCoord0.xy;\n"
    "    v_world_pos = gl_Vertex.xyz;\n"
    "    gl_Position = mvpMatrix * gl_Vertex;\n"
    "}\n";

const char* UPGRADED_VERT_SHADER_COLOR = 
    "#version 120\n"
    "#define lowp\n"
    "#define mediump\n"
    "#define highp\n"
    "varying vec4 v_color;\n"
    "varying vec3 v_world_pos;\n"
    "uniform mat4 mvpMatrix;\n"
    "void main() {\n"
    "    v_color = gl_Color;\n"
    "    v_world_pos = gl_Vertex.xyz;\n"
    "    gl_Position = mvpMatrix * gl_Vertex;\n"
    "}\n";

const char* UPGRADED_FRAG_SHADER_TEXTURED = 
    "#version 120\n"
    "#define lowp\n"
    "#define mediump\n"
    "#define highp\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D s_texture;\n"
    "void main() {\n"
    "    vec4 tex = texture2D(s_texture, v_texCoord);\n"
    // Multiply texture by the vertex color (glColor4f / glColorPointer data).
    // The game drives tanning, damage tints, and boss colors through vertex
    // and material diffuse colors — we must NOT clobber them with a fake
    // point-light model that multiplies by uninitialized uniforms (= black).
    "    gl_FragColor = tex * v_color;\n"
    "}\n";


const char* UPGRADED_FRAG_SHADER_COLOR = 
    "#version 120\n"
    "#define lowp\n"
    "#define mediump\n"
    "#define highp\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    // Pass vertex color through directly — same rationale as TEXTURED variant.
    "    gl_FragColor = v_color;\n"
    "}\n";


// Helper: sanitize a guest GLES2 shader source for desktop GLSL 1.20 compatibility.
// Strips 'precision X float;' / 'precision X int;' etc. (syntax errors on desktop)
// and prepends '#version 120' plus compatibility macro defines.
static std::string sanitize_gles2_shader(const std::string& src) {
    static const char* DESKTOP_COMPAT_HEADER =
        "#version 120\n"
        "#define lowp\n"
        "#define mediump\n"
        "#define highp\n";
    std::string out;
    out.reserve(src.size() + 128);
    // Tokenize lines, dropping 'precision ...' declarations
    std::istringstream stream(src);
    std::string line;
    bool has_version = false;
    while (std::getline(stream, line)) {
        // Detect existing #version directive — skip it, we'll prepend our own
        std::string trimmed = line;
        size_t first = trimmed.find_first_not_of(" \t");
        if (first != std::string::npos) trimmed = trimmed.substr(first);
        if (trimmed.substr(0, 8) == "#version") {
            has_version = true;
            continue; // replace with our own
        }
        // Drop 'precision mediump float;' style lines
        if (trimmed.substr(0, 9) == "precision") continue;
        out += line;
        out += '\n';
    }
    return std::string(DESKTOP_COMPAT_HEADER) + out;
}

static void bridge_glCreateShader(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum type = (GLenum)emu->get_reg(0);
    GLuint shader = 0;
    if (g_display_active) {
        shader = glCreateShader(type);
    }
    emu->set_reg(0, shader);
}

// Per-shader sanitized source storage (lives until the next glShaderSource on that shader)
static std::unordered_map<GLuint, std::string> g_sanitized_sources;

static void bridge_glShaderSource(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t shader = emu->get_reg(0);
    uint32_t count = emu->get_reg(1);
    uint64_t string_ptr = emu->get_reg(2);
    uint64_t length_ptr = emu->get_reg(3);

    std::vector<const GLchar*> sources;
    std::vector<GLint> lengths;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t str_addr = *(uint64_t*)(memory + string_ptr + i * 8);
        sources.push_back((const GLchar*)(memory + str_addr));
        if (length_ptr != 0) {
            lengths.push_back(*(GLint*)(memory + length_ptr + i * 4));
        }
    }

    if (count > 0 && sources[0] != nullptr) {
        std::string src;
        if (length_ptr != 0 && lengths[0] > 0) {
            src = std::string(sources[0], lengths[0]);
        } else {
            src = sources[0];
        }

        // Upgrade known GLES 2.0 patterns to validated desktop GLSL so the shader
        // pipeline produces correct output on the host OpenGL driver.
        // For unknown patterns, fall back to sanitize_gles2_shader (#version 120 +
        // precision stripping) — same behaviour as the original bridge code.
        if (src.find("attribute vec4 position;") != std::string::npos) {
            if (src.find("attribute vec2 texCoord;") != std::string::npos) {
                sources[0] = UPGRADED_VERT_SHADER;
                if (length_ptr != 0) lengths[0] = (GLint)strlen(UPGRADED_VERT_SHADER);
            } else {
                sources[0] = UPGRADED_VERT_SHADER_COLOR;
                if (length_ptr != 0) lengths[0] = (GLint)strlen(UPGRADED_VERT_SHADER_COLOR);
            }
        } else if (src.find("texture2D") != std::string::npos) {
            sources[0] = UPGRADED_FRAG_SHADER_TEXTURED;
            if (length_ptr != 0) lengths[0] = (GLint)strlen(UPGRADED_FRAG_SHADER_TEXTURED);
        } else if (src.find("gl_FragColor =") != std::string::npos) {
            sources[0] = UPGRADED_FRAG_SHADER_COLOR;
            if (length_ptr != 0) lengths[0] = (GLint)strlen(UPGRADED_FRAG_SHADER_COLOR);
        } else if (!src.empty()) {
            // Unknown guest shader — sanitize for desktop GLSL compatibility
            std::string sanitized = sanitize_gles2_shader(src);
            g_sanitized_sources[shader] = std::move(sanitized);
            sources[0] = g_sanitized_sources[shader].c_str();
            if (length_ptr != 0) lengths[0] = (GLint)g_sanitized_sources[shader].size();
        }
    }

    if (g_display_active) {
        glShaderSource(shader, count, sources.data(), length_ptr ? lengths.data() : NULL);
    }
}

static void bridge_glCompileShader(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint shader = (GLuint)emu->get_reg(0);
    if (g_display_active) {
        glCompileShader(shader);
        // Diagnostics: log compile errors so they are visible in stdout
        GLint status = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status) {
            GLint len = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
            std::string log(len + 1, '\0');
            glGetShaderInfoLog(shader, len, nullptr, &log[0]);
            printf("[GLES2-ERR] glCompileShader(%u) FAILED:\n%s\n", shader, log.c_str());
            fflush(stdout);
        } else {
            printf("[GLES2] glCompileShader(%u) OK\n", shader);
            fflush(stdout);
        }
    }
}

static void bridge_glCreateProgram(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint program = 0;
    if (g_display_active) {
        program = glCreateProgram();
    }
    emu->set_reg(0, program);
}

static void bridge_glAttachShader(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint program = (GLuint)emu->get_reg(0);
    GLuint shader = (GLuint)emu->get_reg(1);
    if (g_display_active) {
        glAttachShader(program, shader);
    }
}

static void bridge_glLinkProgram(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint program = (GLuint)emu->get_reg(0);
    if (g_display_active) {
        glLinkProgram(program);
        // Diagnostics: log link errors so they are visible in stdout
        GLint status = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (!status) {
            GLint len = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
            std::string log(len + 1, '\0');
            glGetProgramInfoLog(program, len, nullptr, &log[0]);
            printf("[GLES2-ERR] glLinkProgram(%u) FAILED:\n%s\n", program, log.c_str());
            fflush(stdout);
        } else {
            printf("[GLES2] glLinkProgram(%u) OK\n", program);
            fflush(stdout);
        }
    }
}

static void bridge_glUseProgram(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint program = (GLuint)emu->get_reg(0);
    g_frame_stats.state_changes++;
    if (g_display_active) {
        glUseProgram(program);
        if (program != 0) {
            GLint u_light_pos_loc = glGetUniformLocation(program, "u_light_pos");
            if (u_light_pos_loc != -1) {
                // Fetch dynamic player coordinates & ambient colors
                extern uint64_t g_sre_hero_pos_x_addr;
                extern uint64_t g_sre_hero_pos_y_addr;
                extern uint64_t g_sre_hero_pos_z_addr;
                extern float g_bg_ambient_r, g_bg_ambient_g, g_bg_ambient_b;

                uint8_t* memory = emu->get_memory_base();
                float hx = 0.0f, hy = 0.0f, hz = 0.0f;
                if (g_sre_hero_pos_x_addr != 0) {
                    hx = *(float*)(memory + g_sre_hero_pos_x_addr);
                    hy = *(float*)(memory + g_sre_hero_pos_y_addr);
                    hz = *(float*)(memory + g_sre_hero_pos_z_addr);
                }

                glUniform3f(u_light_pos_loc, hx, hy + 40.0f, hz);

                GLint u_light_color_loc = glGetUniformLocation(program, "u_light_color");
                if (u_light_color_loc != -1) {
                    glUniform3f(u_light_color_loc, 1.0f, 0.85f, 0.6f);
                }

                GLint u_light_radius_loc = glGetUniformLocation(program, "u_light_radius");
                if (u_light_radius_loc != -1) {
                    glUniform1f(u_light_radius_loc, 250.0f);
                }

                GLint u_ambient_color_loc = glGetUniformLocation(program, "u_ambient_color");
                if (u_ambient_color_loc != -1) {
                    glUniform3f(u_ambient_color_loc, g_bg_ambient_r, g_bg_ambient_g, g_bg_ambient_b);
                }
            }
        }
    }
}

static void bridge_glGetUniformLocation(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint program = (GLuint)emu->get_reg(0);
    uint64_t name_ptr = emu->get_reg(1);
    const char* name = (const char*)(memory + name_ptr);
    GLint loc = -1;
    if (g_display_active) {
        loc = glGetUniformLocation(program, name);
    }
    emu->set_reg(0, loc);
}

static void bridge_glUniformMatrix4fv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLint location = (GLint)emu->get_reg(0);
    GLsizei count = (GLsizei)emu->get_reg(1);
    GLboolean transpose = (GLboolean)emu->get_reg(2);
    uint64_t value_ptr = emu->get_reg(3);
    if (g_display_active && value_ptr != 0) {
        glUniformMatrix4fv(location, count, transpose, (const GLfloat*)(memory + value_ptr));
    }
}

static void bridge_glUniform4fv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLint location = (GLint)emu->get_reg(0);
    GLsizei count = (GLsizei)emu->get_reg(1);
    uint64_t value_ptr = emu->get_reg(2);
    if (g_display_active && value_ptr != 0) {
        glUniform4fv(location, count, (const GLfloat*)(memory + value_ptr));
    }
}

static void bridge_glUniform1f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLint location = (GLint)emu->get_reg(0);
    float v0 = emu->get_sreg(0);
    if (g_display_active) {
        glUniform1f(location, v0);
    }
}

static void bridge_glUniform2f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLint location = (GLint)emu->get_reg(0);
    float v0 = emu->get_sreg(0);
    float v1 = emu->get_sreg(1);
    if (g_display_active) {
        glUniform2f(location, v0, v1);
    }
}

static void bridge_glUniform3f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLint location = (GLint)emu->get_reg(0);
    float v0 = emu->get_sreg(0);
    float v1 = emu->get_sreg(1);
    float v2 = emu->get_sreg(2);
    if (g_display_active) {
        glUniform3f(location, v0, v1, v2);
    }
}

static void bridge_glUniform4f(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLint location = (GLint)emu->get_reg(0);
    float v0 = emu->get_sreg(0);
    float v1 = emu->get_sreg(1);
    float v2 = emu->get_sreg(2);
    float v3 = emu->get_sreg(3);
    if (g_display_active) {
        glUniform4f(location, v0, v1, v2, v3);
    }
}

static void bridge_glUniform1i(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLint location = (GLint)emu->get_reg(0);
    GLint v0 = (GLint)emu->get_reg(1);
    if (g_display_active) {
        glUniform1i(location, v0);
    }
}

static void bridge_glVertexAttribPointer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint index = (GLuint)emu->get_reg(0);
    GLint size = (GLint)emu->get_reg(1);
    GLenum type = (GLenum)emu->get_reg(2);
    GLboolean normalized = (GLboolean)emu->get_reg(3);
    GLsizei stride = (GLsizei)emu->get_reg(4);
    uint64_t pointer_ptr = emu->get_reg(5);
    if (g_display_active) {
        GLint bound_vbo = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound_vbo);
        const void* ptr = (bound_vbo != 0) ? (const void*)pointer_ptr : (const void*)(memory + pointer_ptr);
        glVertexAttribPointer(index, size, type, normalized, stride, ptr);
    }
}

static void bridge_glEnableVertexAttribArray(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint index = (GLuint)emu->get_reg(0);
    if (g_display_active) {
        glEnableVertexAttribArray(index);
    }
}

static void bridge_glDisableVertexAttribArray(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint index = (GLuint)emu->get_reg(0);
    if (g_display_active) {
        glDisableVertexAttribArray(index);
    }
}

static void bridge_glGetShaderiv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint shader = (GLuint)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint64_t params_ptr = emu->get_reg(2);
    if (g_display_active && params_ptr != 0) {
        glGetShaderiv(shader, pname, (GLint*)(memory + params_ptr));
    }
}

static void bridge_glGetShaderInfoLog(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint shader = (GLuint)emu->get_reg(0);
    GLsizei maxLength = (GLsizei)emu->get_reg(1);
    uint64_t length_ptr = emu->get_reg(2);
    uint64_t infoLog_ptr = emu->get_reg(3);
    if (g_display_active) {
        glGetShaderInfoLog(shader, maxLength, length_ptr ? (GLsizei*)(memory + length_ptr) : NULL, (GLchar*)(memory + infoLog_ptr));
    }
}

static void bridge_glGetProgramiv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint program = (GLuint)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint64_t params_ptr = emu->get_reg(2);
    if (g_display_active && params_ptr != 0) {
        glGetProgramiv(program, pname, (GLint*)(memory + params_ptr));
    }
}

static void bridge_glGetProgramInfoLog(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint program = (GLuint)emu->get_reg(0);
    GLsizei maxLength = (GLsizei)emu->get_reg(1);
    uint64_t length_ptr = emu->get_reg(2);
    uint64_t infoLog_ptr = emu->get_reg(3);
    if (g_display_active) {
        glGetProgramInfoLog(program, maxLength, length_ptr ? (GLsizei*)(memory + length_ptr) : NULL, (GLchar*)(memory + infoLog_ptr));
    }
}

static void bridge_glBindAttribLocation(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint program = (GLuint)emu->get_reg(0);
    GLuint index = (GLuint)emu->get_reg(1);
    uint64_t name_ptr = emu->get_reg(2);
    if (g_display_active && name_ptr != 0) {
        glBindAttribLocation(program, index, (const GLchar*)(memory + name_ptr));
    }
}

static void bridge_glGetAttribLocation(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLuint program = (GLuint)emu->get_reg(0);
    uint64_t name_ptr = emu->get_reg(1);
    GLint loc = -1;
    if (g_display_active && name_ptr != 0) {
        loc = glGetAttribLocation(program, (const GLchar*)(memory + name_ptr));
    }
    emu->set_reg(0, loc);
}

static void bridge_glDeleteShader(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint shader = (GLuint)emu->get_reg(0);
    if (g_display_active) glDeleteShader(shader);
}

static void bridge_glDeleteProgram(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint program = (GLuint)emu->get_reg(0);
    if (g_display_active) glDeleteProgram(program);
}

static void bridge_glDetachShader(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLuint program = (GLuint)emu->get_reg(0);
    GLuint shader = (GLuint)emu->get_reg(1);
    if (g_display_active) glDetachShader(program, shader);
}

static void bridge_gl_buffer_sub_data(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum target = (GLenum)emu->get_reg(0);
    GLintptr offset = (GLintptr)emu->get_reg(1);
    GLsizeiptr size = (GLsizeiptr)emu->get_reg(2);
    uint64_t data_ptr = emu->get_reg(3);
    if (g_display_active && data_ptr != 0) {
        glBufferSubData(target, offset, size, (const void*)(memory + data_ptr));
    }
}

static void bridge_gl_alpha_func(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    // ARM64 AAPCS64: float arg in S0
    float ref = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.alpha_func(emu->get_reg(0), ref);
        return;
    }
#endif
    if (g_display_active) {
        glAlphaFunc(emu->get_reg(0), ref);
    }
}

static void bridge_gl_shade_model(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.shade_model(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glShadeModel(emu->get_reg(0));
}

static void bridge_gl_clear_depthf(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    // ARM64 AAPCS64: float arg in S0
    float d = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.clear_depthf(d);
        return;
    }
#endif
    if (g_display_active) {
        glClearDepth(d);
    }
}

static void bridge_gl_line_width(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    // ARM64 AAPCS64: float arg in S0
    float w = emu->get_sreg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.line_width(w);
        return;
    }
#endif
    if (g_display_active) {
        glLineWidth(w);
    }
}

static void bridge_gl_active_texture(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.active_texture(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glActiveTexture(emu->get_reg(0));
}

static void bridge_gl_client_active_texture(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.client_active_texture(emu->get_reg(0));
        return;
    }
#endif
    // On desktop GL, glClientActiveTexture is a separate function
    if (g_display_active) glClientActiveTexture(emu->get_reg(0));
}

// --- Queries & Gen ---

// eglGetProcAddress(const char* procname) -> void* func
static void bridge_eglGetProcAddress(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(0);
    const char* procname = name_ptr ? (const char*)(memory + name_ptr) : "";
    printf("[EGL] eglGetProcAddress(\"%s\")\n", procname);
    fflush(stdout);
    uint32_t addr = 0;
    if (emu->bridge && procname[0]) {
        addr = emu->bridge->lookup_proc_address(procname);
    }
    if (addr) {
        printf("[EGL]   -> bridge 0x%x (%s)\n", addr, emu->bridge->get_name(addr).c_str());
        fflush(stdout);
    } else {
        // Log all EGL requests even if no handler, to see what's missing
        static std::unordered_set<std::string> warned_egl;
        if (!warned_egl.count(procname)) {
            printf("[EGL]   -> NULL (no bridge handler)\n");
            fflush(stdout);
            warned_egl.insert(procname);
        }
    }
    emu->set_reg(0, addr);
}

static void bridge_glGetError(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_display_active) {
        emu->set_reg(0, glGetError());
    } else {
        emu->set_reg(0, 0); // GL_NO_ERROR
    }
}

static void bridge_glGenTextures(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t textures_ptr = emu->get_reg(1);
    GL_DIAG("glGenTextures(n=%d)", n);
    TEX_LOG("glGenTextures(n=%u out=0x%x)", n, textures_ptr);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        if (n > 64) n = 64;
        uint32_t textures[64];
        g_vk_backend.gen_textures(n, textures);
        for (uint32_t i = 0; i < n; i++) {
            *(uint32_t*)(memory + textures_ptr + i * 4) = textures[i];
        }
        return;
    }
#endif
    if (g_display_active) {
        // Generate real GL texture IDs
        GLuint textures[64];
        if (n > 64) n = 64;
        glGenTextures(n, textures);
        for (uint32_t i = 0; i < n; i++) {
            *(uint32_t*)(memory + textures_ptr + i * 4) = textures[i];
            GL_DIAG("  -> id[%d] = %d", i, textures[i]);
            TEX_LOG("  -> id[%u] = %u", i, textures[i]);
            RedstellGC::instance().track_gl_gen_texture(textures[i]);
        }
    } else {
        static uint32_t next_tex = 1;
        for (uint32_t i = 0; i < n && i < 64; i++) {
            *(uint32_t*)(memory + textures_ptr + i * 4) = next_tex++;
            GL_DIAG("  -> id[%d] = %d", i, next_tex - 1);
            TEX_LOG("  -> id[%u] = %u (stub)", i, next_tex - 1);
            RedstellGC::instance().track_gl_gen_texture(next_tex - 1);
        }
    }
}

static void bridge_glGenBuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t buffers_ptr = emu->get_reg(1);
    if (g_display_active) {
        GLuint buffers[64];
        if (n > 64) n = 64;
        glGenBuffers(n, buffers);
        for (uint32_t i = 0; i < n; i++) {
            *(uint32_t*)(memory + buffers_ptr + i * 4) = buffers[i];
        }
    } else {
        static uint32_t next_buf = 0x2000;
        for (uint32_t i = 0; i < n && i < 64; i++) {
            *(uint32_t*)(memory + buffers_ptr + i * 4) = next_buf++;
        }
    }
}

static void bridge_glGetIntegerv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t pname = emu->get_reg(0);
    uint32_t params_ptr = emu->get_reg(1);
    if (g_display_active) {
        glGetIntegerv(pname, (GLint*)(memory + params_ptr));
        GLint* vals = (GLint*)(memory + params_ptr);
        GL_DIAG("glGetIntegerv(pname=0x%x) -> [%d, %d, %d, %d]", pname, vals[0], vals[1], vals[2], vals[3]);
    } else {
        *(uint32_t*)(memory + params_ptr) = 2048;
    }
}

static void bridge_glGetFloatv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t pname = emu->get_reg(0);
    uint32_t params_ptr = emu->get_reg(1);
    if (g_display_active) {
        glGetFloatv(pname, (GLfloat*)(memory + params_ptr));
        GLfloat* vals = (GLfloat*)(memory + params_ptr);
        GL_DIAG("glGetFloatv(pname=0x%x) -> [%.3f, %.3f, %.3f, %.3f]", pname, vals[0], vals[1], vals[2], vals[3]);
    } else {
        // Return identity matrix for matrix queries, zero otherwise
        if (pname == 0x0BA6 || pname == 0x0BA7 || pname == 0x0C56) {
            // GL_MODELVIEW_MATRIX, GL_PROJECTION_MATRIX, GL_TEXTURE_MATRIX
            GLfloat identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            memcpy(memory + params_ptr, identity, 64);
        } else {
            *(float*)(memory + params_ptr) = 0.0f;
        }
    }
}

static void bridge_glGetString(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    static bool initialized = false;
    uint8_t* memory = emu->get_memory_base();
    if (!initialized) {
        strcpy((char*)(memory + 0x40000), "OpenGL ES 2.0 (Swordigo Desktop)");
        strcpy((char*)(memory + 0x40100), "Swordigo Desktop Emulator");
        // Advertise ETC1 and PVRTC support — we have full software decoders.
        strcpy((char*)(memory + 0x40200), "GL_OES_compressed_ETC1_RGB8_texture GL_OES_compressed_ETC1_RGB8_sub_texture GL_OES_texture_npot GL_IMG_texture_format_BGRA8888 GL_IMG_texture_compression_pvrtc");
        initialized = true;
    }
    uint32_t name = emu->get_reg(0);
    if (g_gl_diag_frame < 3) std::cout << "[GL] glGetString(0x" << std::hex << name << std::dec << ")" << std::endl;
    switch (name) {
        case 0x1F00: emu->set_reg(0, 0x40100); break; // GL_VENDOR
        case 0x1F01: emu->set_reg(0, 0x40100); break; // GL_RENDERER
        case 0x1F02: emu->set_reg(0, 0x40000); break; // GL_VERSION
        case 0x1F03: emu->set_reg(0, 0x40200); break; // GL_EXTENSIONS
        default: emu->set_reg(0, 0x40000); break;
    }
}

// --- Misc GL stubs (things that need GL but are less critical) ---

static void bridge_gl_delete_textures(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t textures_ptr = emu->get_reg(1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tid = *(uint32_t*)(memory + textures_ptr + i * 4);
        RedstellGC::instance().track_gl_delete_texture(tid);
    }
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.delete_textures(n, (const uint32_t*)(memory + textures_ptr));
        return;
    }
#endif
    if (g_display_active) {
        glDeleteTextures(n, (const GLuint*)(memory + textures_ptr));
    }
}

static void bridge_gl_delete_buffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        uint32_t n = emu->get_reg(0);
        uint32_t buffers_ptr = emu->get_reg(1);
        glDeleteBuffers(n, (const GLuint*)(memory + buffers_ptr));
    }
}

static void bridge_gl_bind_buffer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    if (g_display_active) glBindBuffer(emu->get_reg(0), emu->get_reg(1));
}

static void bridge_gl_buffer_data(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    g_frame_stats.state_changes++;
    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        uint32_t target = emu->get_reg(0);
        uint32_t size = emu->get_reg(1);
        uint32_t data_ptr = emu->get_reg(2);
        uint32_t usage = emu->get_reg(3);
        const void* data = data_ptr ? (const void*)(memory + data_ptr) : nullptr;
        glBufferData(target, size, data, usage);
    }
}

static void bridge_gl_flush(void* emu_ptr) {
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.flush();
        return;
    }
#endif
    if (g_display_active) glFlush();
}

static void bridge_gl_finish(void* emu_ptr) {
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.finish();
        return;
    }
#endif
    if (g_display_active) glFinish();
}

// ========== FBO Bridge — Proper implementation for game effects ==========
// The game uses FBOs for: portal effects, drift effects, post-processing.
// We need to track guest FBO IDs and map them to host FBOs, while being
// careful not to conflict with the host's own FBO (from fbo_scaler).

// Track the host FBO that fbo_scaler is using, so we can restore it
extern unsigned int fbo_get_fbo();  // from fbo_scaler.cpp

static void bridge_gl_gen_framebuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLsizei n = (GLsizei)emu->get_reg(0);
    uint32_t ids_ptr = (uint32_t)emu->get_reg(1);
    
    if (!g_display_active || n <= 0 || n > 16) return;
    
    GLuint host_ids[16];
    glGenFramebuffers(n, host_ids);
    
    // Write IDs back to guest memory
    uint32_t* guest_ids = (uint32_t*)(memory + ids_ptr);
    for (int i = 0; i < n; i++) {
        guest_ids[i] = host_ids[i];
    }
    
    std::cout << "[FBO] glGenFramebuffers(n=" << n << ") -> ";
    for (int i = 0; i < n; i++) std::cout << host_ids[i] << " ";
    std::cout << std::endl;
}

static void bridge_gl_bind_framebuffer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum target = (GLenum)emu->get_reg(0);
    GLuint framebuffer = (GLuint)emu->get_reg(1);
    
    if (!g_display_active) return;
    
    if (framebuffer == 0) {
        // Game unbinding FBO → bind back to host's game FBO (not FBO 0!)
        glBindFramebuffer(target, fbo_get_fbo());
    } else {
        glBindFramebuffer(target, framebuffer);
    }
}

static void bridge_gl_delete_framebuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLsizei n = (GLsizei)emu->get_reg(0);
    uint32_t ids_ptr = (uint32_t)emu->get_reg(1);
    
    if (!g_display_active || n <= 0 || n > 16) return;
    
    GLuint host_ids[16];
    uint32_t* guest_ids = (uint32_t*)(memory + ids_ptr);
    for (int i = 0; i < n; i++) host_ids[i] = guest_ids[i];
    
    glDeleteFramebuffers(n, host_ids);
}

static void bridge_gl_framebuffer_texture2d(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    GLenum target = (GLenum)emu->get_reg(0);
    GLenum attachment = (GLenum)emu->get_reg(1);
    GLenum textarget = (GLenum)emu->get_reg(2);
    GLuint texture = (GLuint)emu->get_reg(3);
    GLint level = (GLint)emu->get_reg(4);
    
    if (!g_display_active) return;
    
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

static void bridge_gl_framebuffer_status(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0x8CD5); // GL_FRAMEBUFFER_COMPLETE — always
}

// ========== Renderbuffer Bridge — Required for FBO depth/stencil ==========

static void bridge_gl_gen_renderbuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLsizei n = (GLsizei)emu->get_reg(0);
    uint32_t ids_ptr = (uint32_t)emu->get_reg(1);
    if (!g_display_active || n <= 0 || n > 16) return;
    GLuint host_ids[16];
    glGenRenderbuffers(n, host_ids);
    uint32_t* guest_ids = (uint32_t*)(memory + ids_ptr);
    for (int i = 0; i < n; i++) guest_ids[i] = host_ids[i];
}

static void bridge_gl_bind_renderbuffer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (!g_display_active) return;
    glBindRenderbuffer((GLenum)emu->get_reg(0), (GLuint)emu->get_reg(1));
}

static void bridge_gl_renderbuffer_storage(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (!g_display_active) return;
    glRenderbufferStorage((GLenum)emu->get_reg(0), (GLenum)emu->get_reg(1),
                          (GLsizei)emu->get_reg(2), (GLsizei)emu->get_reg(3));
}

static void bridge_gl_framebuffer_renderbuffer(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    if (!g_display_active) return;
    glFramebufferRenderbuffer((GLenum)emu->get_reg(0), (GLenum)emu->get_reg(1),
                              (GLenum)emu->get_reg(2), (GLuint)emu->get_reg(3));
}

static void bridge_gl_delete_renderbuffers(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLsizei n = (GLsizei)emu->get_reg(0);
    uint32_t ids_ptr = (uint32_t)emu->get_reg(1);
    if (!g_display_active || n <= 0 || n > 16) return;
    GLuint host_ids[16];
    uint32_t* guest_ids = (uint32_t*)(memory + ids_ptr);
    for (int i = 0; i < n; i++) host_ids[i] = guest_ids[i];
    glDeleteRenderbuffers(n, host_ids);
}

static void bridge_matrix4_mul(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const float* a = (const float*)(memory + emu->get_reg(0));
    const float* b = (const float*)(memory + emu->get_reg(1));
    float* out = (float*)(memory + emu->get_reg(2));

    float res[16];
    for (int i = 0; i < 4; i++) { // column
        for (int j = 0; j < 4; j++) { // row
            float sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + j] * b[i * 4 + k];
            }
            res[i * 4 + j] = sum;
        }
    }
    
    bool bad_a = false, bad_b = false, bad_res = false;
    for (int i = 0; i < 16; i++) {
        if (std::isnan(a[i]) || std::isinf(a[i])) bad_a = true;
        if (std::isnan(b[i]) || std::isinf(b[i])) bad_b = true;
        if (std::isnan(res[i]) || std::isinf(res[i])) bad_res = true;
    }

    if (bad_res && !bad_a && !bad_b) {
        std::cout << "[MATRIX] Matrix4Mul CREATED NaNs! LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl;
    }
    
    memcpy(out, res, 64);
}

// -------------------------------------------------------------------------
// Matrix4::Ortho spy — intercepts the engine's own ortho projection setup.
// ARM calling convention: r0=this(Matrix4*), r1=left, r2=right, r3=bottom,
// [sp+0]=top, [sp+4]=near, [sp+8]=far (float args via integer regs/stack).
// We log the params and compute the matrix natively so it's always correct.
// -------------------------------------------------------------------------
static void bridge_matrix4_ortho(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = (uint32_t)emu->get_reg(0);  // Matrix4* this (X0)
    // ARM64 AAPCS64: float args in S0-S5
    float left   = emu->get_sreg(0);
    float right  = emu->get_sreg(1);
    float bottom = emu->get_sreg(2);
    float top    = emu->get_sreg(3);
    float near_  = emu->get_sreg(4);
    float far_   = emu->get_sreg(5);

    static int ortho_count = 0;
    ortho_count++;
    std::cout << "[PROJ] Matrix4::Ortho #" << ortho_count
              << " l=" << left << " r=" << right
              << " b=" << bottom << " t=" << top
              << " n=" << near_ << " f=" << far_ << std::endl;

    // Compute standard OpenGL ortho matrix in column-major layout
    // to write into guest memory at dst_ptr
    if (dst_ptr && dst_ptr < (uint32_t)(0xC0000000)) {
        float tx = -(right + left) / (right - left);
        float ty = -(top + bottom) / (top - bottom);
        float tz = -(far_ + near_) / (far_ - near_);
        float sx =  2.0f / (right - left);
        float sy =  2.0f / (top - bottom);
        float sz = -2.0f / (far_ - near_);
        // Column-major layout (OpenGL convention)
        float m[16] = {
            sx,   0.0f, 0.0f, 0.0f,
            0.0f, sy,   0.0f, 0.0f,
            0.0f, 0.0f, sz,   0.0f,
            tx,   ty,   tz,   1.0f
        };
        memcpy(memory + dst_ptr, m, 64);
    }
    // Return this (r0 unchanged)
}

// -------------------------------------------------------------------------
// Matrix4::PerspectiveFov spy — logs FOV params for 3D projection.
// ARM: r0=this(Matrix4*), r1=fov(deg), r2=aspect, r3=near, [sp]=far
// -------------------------------------------------------------------------
static void bridge_matrix4_perspective(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = (uint32_t)emu->get_reg(0);  // Matrix4* this (X0)
    // ARM64 AAPCS64: float args in S0-S3
    float fov    = emu->get_sreg(0);  // in degrees
    float aspect = emu->get_sreg(1);
    float near_  = emu->get_sreg(2);
    float far_   = emu->get_sreg(3);

    static int persp_count = 0;
    persp_count++;
    std::cout << "[PROJ] Matrix4::PerspectiveFov #" << persp_count
              << " fov=" << fov << " aspect=" << aspect
              << " near=" << near_ << " far=" << far_ << std::endl;

    // Compute standard perspective matrix
    if (dst_ptr && dst_ptr < (uint32_t)(0xC0000000)) {
        float rad = fov * (3.14159265f / 180.0f);
        float f = 1.0f / std::tan(rad / 2.0f);
        float range = near_ - far_;
        float m[16] = {
            f / aspect, 0.0f, 0.0f,                         0.0f,
            0.0f,       f,    0.0f,                         0.0f,
            0.0f,       0.0f, (near_ + far_) / range,       -1.0f,
            0.0f,       0.0f, (2.0f * near_ * far_) / range, 0.0f
        };
        memcpy(memory + dst_ptr, m, 64);
    }
}


static const uint32_t GUEST_ERRNO_ADDR = 0x41000; // Reserved area
static void bridge_errno(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, GUEST_ERRNO_ADDR);
}

static void bridge_google_blocking(void* emu_ptr) {
    // __google_potentially_blocking_region_begin/end - no-op
}

static void bridge_cxa_atexit(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}


static void bridge_stack_chk_fail(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t lr = emu->get_lr();
    uint64_t sp = emu->get_reg(31);
    std::cerr << "[SRE/Recovery] __stack_chk_fail called at PC=0x" << std::hex << emu->get_pc()
              << " — unwinding to LR=0x" << lr << " (SP=0x" << sp << ")" << std::dec << std::endl;

    if (lr >= 0x1000000ULL && lr < 0x3000000ULL) {
        emu->set_pc(lr);
        emu->set_reg(0, 0);
    } else {
        emu->set_pc(0xE0000000ULL);  // MAGIC_LR
    }
}


static void bridge_cxa_guard_acquire(void* emu_ptr) {
    // __cxa_guard_acquire(int64_t* guard_object)
    // Returns 1 if initialization is needed, 0 if already done
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t guard_ptr = (uint32_t)emu->get_reg(0);
    if (guard_ptr && guard_ptr < 0xE0000000) {
        uint8_t guard_byte = memory[guard_ptr];
        if (guard_byte != 0) {
            emu->set_reg(0, 0); // already initialized
            return;
        }
    }
    emu->set_reg(0, 1); // needs initialization
}

static void bridge_cxa_guard_release(void* emu_ptr) {
    // __cxa_guard_release(int64_t* guard_object) — mark as initialized
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t guard_ptr = (uint32_t)emu->get_reg(0);
    if (guard_ptr && guard_ptr < 0xE0000000) {
        memory[guard_ptr] = 1; // mark initialized
    }
}

static void bridge_cxa_guard_abort(void* emu_ptr) {
    // __cxa_guard_abort — just return (no-op in single-threaded)
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    (void)emu;
}

static void bridge_pthread_mutex(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t mutex_ptr = emu->get_reg(0);
    // Write state=0 (unlocked) to the guest mutex structure.
    // Android bionic's pthread_mutex_t has the state word at offset 0.
    // This is critical for single-threaded emulation: without modifying
    // guest memory, the caller's retry loop sees the old state and spins
    // forever because no second thread will ever change it.
    if (mutex_ptr != 0 && mutex_ptr < 0xE0000000) {
        *(uint32_t*)(memory + mutex_ptr) = 0;
    }
    emu->set_reg(0, 0); // success
}

static void bridge_pthread_once(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t once_control_ptr = emu->get_reg(0);
    uint32_t init_routine = emu->get_reg(1);

    if (once_control_ptr == 0) {
        emu->set_reg(0, -1); // EINVAL
        return;
    }

    uint32_t once_val = *(uint32_t*)(memory + once_control_ptr);
    if (once_val != 2) { // not initialized
        *(uint32_t*)(memory + once_control_ptr) = 2;
        if (init_routine != 0) {
            // Known issue: init_routine hangs at Windcliffe Campsite on ARM64
            // because it internally spawns threads that deadlock in
            // single-threaded emulation. Use ARM32 for full game playthrough.
            emu->redirect_pc = init_routine;
        }
    }
    emu->set_reg(0, 0); // success
}

static void bridge_pthread_cond(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // pthread_cond_wait is an explicit synchronization point: the calling thread
    // is yielding control and waiting for another thread to signal it. In our
    // single-threaded emulation model, this is exactly the point where we must
    // run any pending threads (spawned via pthread_create) so that they can
    // complete their work and set whatever flag/condition the caller is waiting on.
    //
    // We also handle pthread_cond_signal/broadcast/init/destroy here as no-ops
    // since our threading model runs threads to completion inline.
    if (emu->has_pending_threads()) {
        std::cerr << "[Thread64] pthread_cond_wait — running " 
                  << "pending threads to resolve synchronization" << std::endl;
        emu->run_pending_threads();
    }
    emu->set_reg(0, 0); // success
}

static void bridge_pthread_create(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    
    // pthread_create(pthread_t* thread, const pthread_attr_t* attr, 
    //                void* (*start_routine)(void*), void* arg)
    // ARM64 ABI: X0=thread, X1=attr, X2=start_routine, X3=arg
    uint64_t thread_ptr = emu->get_reg(0);
    uint64_t attr = emu->get_reg(1);
    uint64_t start_routine = emu->get_reg(2);
    uint64_t arg = emu->get_reg(3);
    
    // Write a fake thread ID if thread_ptr is valid
    if (thread_ptr && thread_ptr < 0xE0000000) {
        static uint64_t next_tid = 0x1001;
        *(uint64_t*)(memory + thread_ptr) = next_tid++;
    }
    
    // Queue thread for deferred execution.
    // The entity worker thread populates a linked list that entity processing
    // needs. We run it via queue_thread + run_pending_threads() which is called
    // between emulator call() invocations (avoids nested uc_emu_start).
    if (start_routine != 0) {
        std::cerr << "[Thread64] pthread_create: QUEUED thread func 0x" 
                  << std::hex << start_routine << " arg=0x" << arg 
                  << std::dec << std::endl;
        emu->queue_thread(start_routine, arg);
    }
    
    emu->set_reg(0, 0); // success
}

static void bridge_pthread_self(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 1); // fake thread id
}

static void bridge_pthread_join(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    // Threads run inline, so by the time join is called, they're already done
    std::cout << "[Bridge] pthread_join (no-op — threads run inline)" << std::endl;
    emu->set_reg(0, 0); // success
}

static void bridge_pthread_detach(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0); // success
}


static void bridge_usleep(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint64_t usec = emu->get_reg(0);
    // Cap at 100ms
    if (usec > 100000) usec = 100000;
    if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = usec / 1000000;
        ts.tv_nsec = (usec % 1000000) * 1000;
        nanosleep(&ts, nullptr);
    }
    emu->set_reg(0, 0);
}

static void bridge_time(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t t_ptr = emu->get_reg(0);
    time_t t = time(nullptr);
    if (t_ptr) {
        *(uint64_t*)(memory + t_ptr) = (uint64_t)t; // ARM64: time_t is 64-bit
    }
    emu->set_reg(0, (uint64_t)t);
}

static uint64_t g_guest_virtual_time_ns = 0;

static void init_virtual_clock() {
    if (g_guest_virtual_time_ns == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_guest_virtual_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }
}

extern "C" void advance_guest_virtual_clock(float dt) {
    init_virtual_clock();
    g_guest_virtual_time_ns += (uint64_t)(dt * 1000000000.0f);
}

static void bridge_clock_gettime(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t clock_id = emu->get_reg(0);
    uint32_t tp_ptr = emu->get_reg(1);
    
    uint64_t ns;
    if (clock_id == 0 || clock_id == 5) { // CLOCK_REALTIME or CLOCK_REALTIME_COARSE
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    } else {
        // CLOCK_MONOTONIC etc. - use smooth virtual clock
        init_virtual_clock();
        ns = g_guest_virtual_time_ns;
    }
    
    if (tp_ptr) {
        // ARM64 struct timespec: { int64_t tv_sec; int64_t tv_nsec; }
        *(int64_t*)(memory + tp_ptr)     = (int64_t)(ns / 1000000000ULL);
        *(int64_t*)(memory + tp_ptr + 8) = (int64_t)(ns % 1000000000ULL);
    }
    emu->set_reg(0, 0);
}

static void bridge_gettimeofday(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t tv_ptr = emu->get_reg(0);
    
    if (tv_ptr) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        // ARM64 struct timeval: { int64_t tv_sec; int64_t tv_usec; }
        *(int64_t*)(memory + tv_ptr)     = (int64_t)tv.tv_sec;
        *(int64_t*)(memory + tv_ptr + 8) = (int64_t)tv.tv_usec;
    }
    emu->set_reg(0, 0);
}

static void bridge_socket(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int domain = emu->get_reg(0);
    int type = emu->get_reg(1);
    int protocol = emu->get_reg(2);
    int ret = socket(domain, type, protocol);
    emu->set_reg(0, ret);
}

static void bridge_connect(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t addr_g = emu->get_reg(1);
    socklen_t addrlen = emu->get_reg(2);
    const struct sockaddr* addr = addr_g ? (const struct sockaddr*)(emu->get_memory_base() + addr_g) : nullptr;
    int ret = connect(sockfd, addr, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_bind(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t addr_g = emu->get_reg(1);
    socklen_t addrlen = emu->get_reg(2);
    const struct sockaddr* addr = addr_g ? (const struct sockaddr*)(emu->get_memory_base() + addr_g) : nullptr;
    int ret = bind(sockfd, addr, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_listen(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    int backlog = emu->get_reg(1);
    int ret = listen(sockfd, backlog);
    emu->set_reg(0, ret);
}

static void bridge_accept(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t addr_g = emu->get_reg(1);
    uint64_t addrlen_g = emu->get_reg(2);
    struct sockaddr* addr = addr_g ? (struct sockaddr*)(emu->get_memory_base() + addr_g) : nullptr;
    socklen_t* addrlen = addrlen_g ? (socklen_t*)(emu->get_memory_base() + addrlen_g) : nullptr;
    int ret = accept(sockfd, addr, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_send(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t buf_g = emu->get_reg(1);
    size_t len = emu->get_reg(2);
    int flags = emu->get_reg(3);
    const void* buf = buf_g ? (const void*)(emu->get_memory_base() + buf_g) : nullptr;
    ssize_t ret = send(sockfd, buf, len, flags);
    emu->set_reg(0, ret);
}

static void bridge_recv(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t buf_g = emu->get_reg(1);
    size_t len = emu->get_reg(2);
    int flags = emu->get_reg(3);
    void* buf = buf_g ? (void*)(emu->get_memory_base() + buf_g) : nullptr;
    ssize_t ret = recv(sockfd, buf, len, flags);
    emu->set_reg(0, ret);
}

static void bridge_sendto(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t buf_g = emu->get_reg(1);
    size_t len = emu->get_reg(2);
    int flags = emu->get_reg(3);
    uint64_t dest_g = emu->get_reg(4);
    socklen_t addrlen = emu->get_reg(5);
    const void* buf = buf_g ? (const void*)(emu->get_memory_base() + buf_g) : nullptr;
    const struct sockaddr* dest = dest_g ? (const struct sockaddr*)(emu->get_memory_base() + dest_g) : nullptr;
    ssize_t ret = sendto(sockfd, buf, len, flags, dest, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_recvfrom(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t buf_g = emu->get_reg(1);
    size_t len = emu->get_reg(2);
    int flags = emu->get_reg(3);
    uint64_t src_g = emu->get_reg(4);
    uint64_t addrlen_g = emu->get_reg(5);
    void* buf = buf_g ? (void*)(emu->get_memory_base() + buf_g) : nullptr;
    struct sockaddr* src = src_g ? (struct sockaddr*)(emu->get_memory_base() + src_g) : nullptr;
    socklen_t* addrlen = addrlen_g ? (socklen_t*)(emu->get_memory_base() + addrlen_g) : nullptr;
    ssize_t ret = recvfrom(sockfd, buf, len, flags, src, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_setsockopt(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    int level = emu->get_reg(1);
    int optname = emu->get_reg(2);
    uint64_t optval_g = emu->get_reg(3);
    socklen_t optlen = emu->get_reg(4);
    const void* optval = optval_g ? (const void*)(emu->get_memory_base() + optval_g) : nullptr;
    int ret = setsockopt(sockfd, level, optname, optval, optlen);
    emu->set_reg(0, ret);
}

static void bridge_getsockopt(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    int level = emu->get_reg(1);
    int optname = emu->get_reg(2);
    uint64_t optval_g = emu->get_reg(3);
    uint64_t optlen_g = emu->get_reg(4);
    void* optval = optval_g ? (void*)(emu->get_memory_base() + optval_g) : nullptr;
    socklen_t* optlen = optlen_g ? (socklen_t*)(emu->get_memory_base() + optlen_g) : nullptr;
    int ret = getsockopt(sockfd, level, optname, optval, optlen);
    emu->set_reg(0, ret);
}

static void bridge_getsockname(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t addr_g = emu->get_reg(1);
    uint64_t addrlen_g = emu->get_reg(2);
    struct sockaddr* addr = addr_g ? (struct sockaddr*)(emu->get_memory_base() + addr_g) : nullptr;
    socklen_t* addrlen = addrlen_g ? (socklen_t*)(emu->get_memory_base() + addrlen_g) : nullptr;
    int ret = getsockname(sockfd, addr, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_getpeername(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    uint64_t addr_g = emu->get_reg(1);
    uint64_t addrlen_g = emu->get_reg(2);
    struct sockaddr* addr = addr_g ? (struct sockaddr*)(emu->get_memory_base() + addr_g) : nullptr;
    socklen_t* addrlen = addrlen_g ? (socklen_t*)(emu->get_memory_base() + addrlen_g) : nullptr;
    int ret = getpeername(sockfd, addr, addrlen);
    emu->set_reg(0, ret);
}

static void bridge_shutdown(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int sockfd = emu->get_reg(0);
    int how = emu->get_reg(1);
    int ret = shutdown(sockfd, how);
    emu->set_reg(0, ret);
}

static void bridge_fcntl(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int fd = emu->get_reg(0);
    int cmd = emu->get_reg(1);
    uint64_t arg = emu->get_reg(2);
    int ret = fcntl(fd, cmd, arg);
    emu->set_reg(0, ret);
}

static void bridge_select(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int nfds = emu->get_reg(0);
    uint64_t r_g = emu->get_reg(1);
    uint64_t w_g = emu->get_reg(2);
    uint64_t e_g = emu->get_reg(3);
    uint64_t t_g = emu->get_reg(4);
    fd_set* readfds = r_g ? (fd_set*)(emu->get_memory_base() + r_g) : nullptr;
    fd_set* writefds = w_g ? (fd_set*)(emu->get_memory_base() + w_g) : nullptr;
    fd_set* exceptfds = e_g ? (fd_set*)(emu->get_memory_base() + e_g) : nullptr;
    struct timeval* timeout = t_g ? (struct timeval*)(emu->get_memory_base() + t_g) : nullptr;
    int ret = select(nfds, readfds, writefds, exceptfds, timeout);
    emu->set_reg(0, ret);
}

static void bridge_close(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int fd = emu->get_reg(0);
    int ret = close(fd);
    emu->set_reg(0, ret);
}

static void bridge_gethostbyname(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t name_g = emu->get_reg(0);
    if (!name_g) {
        emu->set_reg(0, 0);
        return;
    }
    const char* name = (const char*)(memory + name_g);
    struct hostent* he = gethostbyname(name);
    if (!he) {
        emu->set_reg(0, 0);
        return;
    }
    uint64_t base = 0x5D000;
    *(uint64_t*)(memory + base) = base + 0x100;
    *(uint64_t*)(memory + base + 8) = base + 0x180;
    *(uint64_t*)(memory + base + 0x180) = 0;
    *(int32_t*)(memory + base + 16) = he->h_addrtype;
    *(int32_t*)(memory + base + 20) = he->h_length;
    *(uint64_t*)(memory + base + 24) = base + 0x190;
    *(uint64_t*)(memory + base + 0x190) = base + 0x1A0;
    *(uint64_t*)(memory + base + 0x198) = 0;
    if (he->h_name) {
        strncpy((char*)(memory + base + 0x100), he->h_name, 127);
        memory[base + 0x100 + 127] = 0;
    } else {
        *(char*)(memory + base + 0x100) = 0;
    }
    if (he->h_addr_list[0]) {
        memcpy(memory + base + 0x1A0, he->h_addr_list[0], he->h_length);
    }
    emu->set_reg(0, base);
}

static void bridge_getaddrinfo(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t node_g = emu->get_reg(0);
    uint64_t service_g = emu->get_reg(1);
    uint64_t hints_g = emu->get_reg(2);
    uint64_t res_g = emu->get_reg(3);
    const char* node = node_g ? (const char*)(memory + node_g) : nullptr;
    const char* service = service_g ? (const char*)(memory + service_g) : nullptr;
    const struct addrinfo* hints = hints_g ? (const struct addrinfo*)(memory + hints_g) : nullptr;
    struct addrinfo* host_res = nullptr;
    int ret = getaddrinfo(node, service, hints, &host_res);
    if (ret != 0) {
        emu->set_reg(0, ret);
        return;
    }
    if (res_g && host_res) {
        uint64_t base = 0x5E000;
        *(int32_t*)(memory + base + 0) = host_res->ai_flags;
        *(int32_t*)(memory + base + 4) = host_res->ai_family;
        *(int32_t*)(memory + base + 8) = host_res->ai_socktype;
        *(int32_t*)(memory + base + 12) = host_res->ai_protocol;
        *(uint32_t*)(memory + base + 16) = host_res->ai_addrlen;
        *(uint64_t*)(memory + base + 24) = base + 0x100;
        memcpy(memory + base + 0x100, host_res->ai_addr, host_res->ai_addrlen);
        if (host_res->ai_canonname) {
            *(uint64_t*)(memory + base + 32) = base + 0x200;
            strncpy((char*)(memory + base + 0x200), host_res->ai_canonname, 127);
            memory[base + 0x200 + 127] = 0;
        } else {
            *(uint64_t*)(memory + base + 32) = 0;
        }
        *(uint64_t*)(memory + base + 40) = 0;
        *(uint64_t*)(memory + res_g) = base;
        freeaddrinfo(host_res);
    }
    emu->set_reg(0, 0);
}

static void bridge_freeaddrinfo(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    emu->set_reg(0, 0);
}

static void bridge_gai_strerror(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    int errcode = emu->get_reg(0);
    const char* err = gai_strerror(errcode);
    uint64_t base = 0x5E300;
    uint8_t* memory = emu->get_memory_base();
    if (err) {
        strncpy((char*)(memory + base), err, 127);
        memory[base + 127] = 0;
    } else {
        *(char*)(memory + base) = 0;
    }
    emu->set_reg(0, base);
}

static void bridge_htons(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint16_t val = emu->get_reg(0);
    emu->set_reg(0, htons(val));
}
static void bridge_htonl(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t val = emu->get_reg(0);
    emu->set_reg(0, htonl(val));
}
static void bridge_ntohs(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint16_t val = emu->get_reg(0);
    emu->set_reg(0, ntohs(val));
}
static void bridge_ntohl(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint32_t val = emu->get_reg(0);
    emu->set_reg(0, ntohl(val));
}

static void bridge_inet_ntoa(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    struct in_addr in;
    in.s_addr = emu->get_reg(0);
    char* res = inet_ntoa(in);
    uint64_t base = 0x5E400;
    uint8_t* memory = emu->get_memory_base();
    if (res) {
        strncpy((char*)(memory + base), res, 31);
        memory[base + 31] = 0;
    } else {
        *(char*)(memory + base) = 0;
    }
    emu->set_reg(0, base);
}

static void bridge_inet_pton(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int af = emu->get_reg(0);
    uint64_t src_g = emu->get_reg(1);
    uint64_t dst_g = emu->get_reg(2);
    const char* src = src_g ? (const char*)(memory + src_g) : nullptr;
    void* dst = dst_g ? (void*)(memory + dst_g) : nullptr;
    int ret = inet_pton(af, src, dst);
    emu->set_reg(0, ret);
}

static void bridge_inet_ntop(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int af = emu->get_reg(0);
    uint64_t src_g = emu->get_reg(1);
    uint64_t dst_g = emu->get_reg(2);
    socklen_t size = emu->get_reg(3);
    const void* src = src_g ? (const void*)(memory + src_g) : nullptr;
    char* dst = dst_g ? (char*)(memory + dst_g) : nullptr;
    const char* ret = inet_ntop(af, src, dst, size);
    emu->set_reg(0, ret ? dst_g : 0);
}

static void bridge_inet_addr(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t cp_g = emu->get_reg(0);
    const char* cp = cp_g ? (const char*)(memory + cp_g) : nullptr;
    in_addr_t ret = inet_addr(cp);
    emu->set_reg(0, ret);
}

static void bridge_sre_resolve_address(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t sym_g = emu->get_reg(0);
    const char* sym = sym_g ? (const char*)(memory + sym_g) : nullptr;
    
    if (sym) {
        uint64_t addr = sre_resolve_address(sym);
        emu->set_reg(0, addr);
    } else {
        emu->set_reg(0, 0);
    }
}

static void bridge_sre_resolve_symbol(void* emu_ptr) {
    IEmulatorArm64* emu = (IEmulatorArm64*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint64_t addr = emu->get_reg(0);
    uint64_t out_buf_g = emu->get_reg(1);
    uint32_t max_len = (uint32_t)emu->get_reg(2);
    
    char* out_buf = out_buf_g ? (char*)(memory + out_buf_g) : nullptr;
    if (!out_buf || max_len == 0) return;
    
    const char* sym = sre_resolve_symbol(addr);
    if (sym) {
        strncpy(out_buf, sym, max_len - 1);
        out_buf[max_len - 1] = '\0';
    } else {
        out_buf[0] = '\0';
    }
}


void JniBridge64::init_standard_bridges() {
    register_handler("sre_resolve_address", bridge_sre_resolve_address);
    register_handler("sre_resolve_symbol", bridge_sre_resolve_symbol);

    register_handler("malloc", bridge_malloc);
    register_handler("calloc", bridge_calloc);
    register_handler("realloc", bridge_realloc);
    register_handler("free", bridge_free);
    register_handler("getenv", bridge_getenv);
    register_handler("signal", bridge_signal);

    register_handler("memcpy", bridge_memcpy);
    register_handler("memset", bridge_memset);
    register_handler("memmove", bridge_memmove);
    register_handler("strlen", bridge_strlen);
    register_handler("strnlen", bridge_strnlen);
    register_handler("strdup", bridge_strdup);
    register_handler("strndup", bridge_strndup);
    register_handler("strcasecmp", bridge_strcasecmp);
    register_handler("strncasecmp", bridge_strncasecmp);
    register_handler("memcmp", bridge_memcmp);
    register_handler("memchr", bridge_memchr_impl);

    register_handler("socket", bridge_socket);
    register_handler("connect", bridge_connect);
    register_handler("send", bridge_send);
    register_handler("recv", bridge_recv);
    register_handler("bind", bridge_bind);
    register_handler("listen", bridge_listen);
    register_handler("accept", bridge_accept);
    register_handler("setsockopt", bridge_setsockopt);
    register_handler("getsockopt", bridge_getsockopt);
    register_handler("fcntl", bridge_fcntl);

    register_handler("strchr", bridge_strchr);
    register_handler("strrchr", bridge_strrchr);
    register_handler("strcpy", bridge_strcpy);
    register_handler("strncpy", bridge_strncpy);
    register_handler("strcmp", bridge_strcmp);
    register_handler("strncmp", bridge_strncmp);
    register_handler("strcat", bridge_strcat);
    register_handler("strncat", bridge_strncat);
    register_handler("strcspn", bridge_strcspn);
    register_handler("strstr", bridge_strstr);
    register_handler("strtol", bridge_strtol);
    register_handler("strtoul", bridge_strtoul);
    register_handler("atoi", bridge_atoi);
    register_handler("atof", bridge_atof);
    register_handler("wctob", bridge_wctob);
    register_handler("btowc", bridge_btowc);
    register_handler("__ctype_get_mb_cur_max", bridge_ctype_cur_max);

    // ctype character classification
    register_handler("iscntrl", bridge_iscntrl);
    register_handler("isprint", bridge_isprint);
    register_handler("isgraph", bridge_isgraph);
    register_handler("ispunct", bridge_ispunct);
    register_handler("isxdigit", bridge_isxdigit);
    register_handler("isblank", bridge_isblank);
    register_handler("isalpha", bridge_isalpha);
    register_handler("isdigit", bridge_isdigit);
    register_handler("isalnum", bridge_isalnum);
    register_handler("isspace", bridge_isspace);
    register_handler("isupper", bridge_isupper);
    register_handler("islower", bridge_islower);
    register_handler("toupper", bridge_toupper);
    register_handler("tolower", bridge_tolower);

    // wide character
    register_handler("wctype", bridge_wctype);
    register_handler("iswctype", bridge_iswctype);
    register_handler("towupper", bridge_towupper);
    register_handler("towlower", bridge_towlower);

    // C++ exception handling stubs
    register_handler("dl_iterate_phdr", bridge_dl_iterate_phdr);
    register_handler("__cxa_throw", bridge_cxa_throw);
    register_handler("__cxa_begin_catch", bridge_cxa_begin_catch);
    register_handler("__cxa_end_catch", bridge_cxa_end_catch);

    // setjmp/longjmp (Lua error handling)
    register_handler("setjmp", bridge_setjmp);
    register_handler("_setjmp", bridge_setjmp);
    register_handler("sigsetjmp", bridge_setjmp);
    register_handler("longjmp", bridge_longjmp);
    register_handler("_longjmp", bridge_longjmp);
    register_handler("siglongjmp", bridge_longjmp);

    // misc libc
    register_handler("strerror", bridge_strerror);
    register_handler("sscanf", bridge_sscanf);
    register_handler("strtod", bridge_strtod);
    register_handler("strtof", bridge_strtof);
    register_handler("strtold", bridge_strtold);
    register_handler("sprintf", bridge_sprintf);
    register_handler("snprintf", bridge_snprintf);
    register_handler("vsprintf", bridge_vsprintf);
    register_handler("fprintf", bridge_fprintf);
    register_handler("printf", bridge_printf);
    register_handler("fputs", bridge_fputs);
    register_handler("fputc", bridge_fputc);
    register_handler("putc", bridge_fputc);
    register_handler("puts", bridge_puts);
    register_handler("getc", bridge_getc);
    register_handler("ungetc", bridge_ungetc);
    register_handler("feof", bridge_feof);
    register_handler("ferror", bridge_ferror);
    register_handler("fileno", bridge_fileno);
    register_handler("setvbuf", bridge_setvbuf);
    register_handler("__sF", bridge__sF);
    register_handler("__errno", bridge__errno);
    register_handler("__stack_chk_fail", bridge__stack_chk_fail);
    register_handler("setlocale", bridge_setlocale);
    register_handler("rand", bridge_rand);
    register_handler("clock", bridge_clock);
    register_handler("remove", bridge_remove);
    register_handler("nanosleep", bridge_nanosleep);
    register_handler("sched_yield", bridge_sched_yield);

    // pthread TLS
    register_handler("pthread_key_create", bridge_pthread_key_create);
    register_handler("pthread_key_delete", bridge_pthread_key_delete);
    register_handler("pthread_getspecific", bridge_pthread_getspecific);
    register_handler("pthread_setspecific", bridge_pthread_setspecific);

    register_handler("cosf", bridge_cosf);
    register_handler("sinf", bridge_sinf);
    register_handler("acosf", bridge_acosf);
    register_handler("asinf", bridge_asinf);
    register_handler("atanf", bridge_atanf);
    register_handler("atan2f", bridge_atan2f);
    register_handler("tanf", bridge_tanf);
    register_handler("tan", bridge_tan);
    register_handler("sqrtf", bridge_sqrtf);
    register_handler("floorf", bridge_floorf);
    register_handler("ceilf", bridge_ceilf);
    register_handler("fmodf", bridge_fmodf);
    register_handler("roundf", bridge_roundf);
    register_handler("round", bridge_round);
    register_handler("sin", bridge_sin);
    register_handler("cos", bridge_cos);
    register_handler("acos", bridge_acos);
    register_handler("floor", bridge_floor);
    register_handler("ceil", bridge_ceil);
    register_handler("sqrt", bridge_sqrt);
    register_handler("fmod", bridge_fmod);
    register_handler("fabs", bridge_fabs);
    register_handler("abs", bridge_abs);
    register_handler("log", bridge_log);
    register_handler("log10", bridge_log10);
    register_handler("log2", bridge_log2);
    register_handler("exp", bridge_exp);
    register_handler("ldexp", bridge_ldexp);
    register_handler("frexp", bridge_frexp);
    register_handler("modf", bridge_modf);
    register_handler("asin", bridge_asin);
    register_handler("atan", bridge_atan);
    register_handler("atan2", bridge_atan2);

    register_handler("powf", bridge_powf);
    register_handler("pow", bridge_pow);
    register_handler("sincosf", bridge_sincosf);

    // File I/O
    register_handler("fopen", bridge_fopen);
    register_handler("fclose", bridge_fclose);
    register_handler("fread", bridge_fread);
    register_handler("fwrite", bridge_fwrite);
    register_handler("fseek", bridge_fseek);
    register_handler("ftell", bridge_ftell);
    register_handler("fflush", bridge_fflush);
    register_handler("fgets", bridge_fgets);
    register_handler("fscanf", bridge_fscanf);
    register_handler("lseek", bridge_lseek);
    register_handler("gzdopen", bridge_gzdopen);
    register_handler("gzread", bridge_gzread);
    register_handler("gzclose", bridge_gzclose);

    // Directory / system
    register_handler("mkdir", bridge_mkdir);
    register_handler("stat", bridge_stat);
    register_handler("opendir", bridge_opendir);
    register_handler("readdir", bridge_readdir);
    register_handler("closedir", bridge_closedir);
    register_handler("rename", bridge_rename);
    register_handler("remove", bridge_remove);
    register_handler("access", bridge_access);
    register_handler("unlink", bridge_unlink);
    register_handler("C_Matrix4Mul", bridge_matrix4_mul);
    register_handler("_Z12C_Matrix4MulPKfS0_Pf", bridge_matrix4_mul);

    // Intercept engine's internal matrix projection functions so they always
    // produce correct results regardless of VFP emulation accuracy.
    register_handler("_ZN5Caver7Matrix45OrthoEffffff",        bridge_matrix4_ortho);
    register_handler("_ZN5Caver7Matrix414PerspectiveFovEffff", bridge_matrix4_perspective);


    // Printf / logging
    register_handler("printf", bridge_printf);
    register_handler("fputs", bridge_fputs);
    register_handler("snprintf", bridge_snprintf);
    register_handler("sprintf", bridge_sprintf);
    register_handler("__android_log_print", bridge_android_log_print);
    register_handler("__android_log_write", bridge_android_log_print);
    register_handler("__android_log_vprint", bridge_android_log_print);

    // Misc libc
    register_handler("abort", bridge_abort);
    register_handler("__errno", bridge_errno);
    register_handler("__cxa_atexit", bridge_cxa_atexit);
    register_handler("__cxa_finalize", bridge_cxa_atexit);
    register_handler("__stack_chk_fail", bridge_stack_chk_fail);
    register_handler("__cxa_guard_acquire", bridge_cxa_guard_acquire);
    register_handler("__cxa_guard_release", bridge_cxa_guard_release);
    register_handler("__cxa_guard_abort", bridge_cxa_guard_abort);
    register_handler("__google_potentially_blocking_region_begin", bridge_google_blocking);
    register_handler("__google_potentially_blocking_region_end", bridge_google_blocking);
    register_handler("time", bridge_time);
    register_handler("clock", bridge_clock);
    register_handler("lrand48", bridge_lrand48);
    register_handler("localtime", bridge_localtime);
    register_handler("localtime_r", bridge_localtime_r);
    register_handler("clock_gettime", bridge_clock_gettime);
    register_handler("gettimeofday", bridge_gettimeofday);
    register_handler("__gnu_Unwind_Find_exidx", bridge_exidx);

    // pthreads
    register_handler("pthread_mutex_init", bridge_pthread_mutex);
    register_handler("pthread_mutex_lock", bridge_pthread_mutex);
    register_handler("pthread_mutex_unlock", bridge_pthread_mutex);
    register_handler("pthread_mutex_destroy", bridge_pthread_mutex);
    register_handler("pthread_cond_init", bridge_pthread_cond);
    register_handler("pthread_cond_signal", bridge_pthread_cond);
    register_handler("pthread_cond_broadcast", bridge_pthread_cond);
    register_handler("pthread_cond_wait", bridge_pthread_cond);
    register_handler("pthread_cond_destroy", bridge_pthread_cond);
    register_handler("pthread_create", bridge_pthread_create);
    register_handler("pthread_self", bridge_pthread_self);
    register_handler("pthread_once", bridge_pthread_once);
    register_handler("pthread_join", bridge_pthread_join);
    register_handler("pthread_detach", bridge_pthread_detach);

    // Sleep / yield
    register_handler("nanosleep", bridge_nanosleep);
    register_handler("sched_yield", bridge_sched_yield);
    register_handler("usleep", bridge_usleep);

    // JNI
    register_handler("FindClass", bridge_FindClass);
    register_handler("GetMethodID", bridge_GetMethodID);
    register_handler("GetStaticMethodID", bridge_GetMethodID); 
    register_handler("RegisterNatives", bridge_RegisterNatives);
    register_handler("NewGlobalRef", bridge_NewGlobalRef);
    register_handler("NewStringUTF", bridge_NewStringUTF);
    register_handler("GetStringUTFChars", bridge_GetStringUTFChars);
    
    // Asset Manager
    register_handler("AAssetManager_fromJava", bridge_AAssetManager_fromJava);
    register_handler("AAssetManager_open", bridge_AAssetManager_open);
    register_handler("AAsset_read", bridge_AAsset_read);
    register_handler("AAsset_close", bridge_AAsset_close);
    register_handler("AAsset_getLength", bridge_AAsset_getLength);
    register_handler("AAsset_openFileDescriptor", bridge_AAsset_openFileDescriptor);

    // JNI Env
    register_handler("ThrowNew", bridge_ThrowNew);
    register_handler("PushLocalFrame", bridge_PushLocalFrame);
    register_handler("PopLocalFrame", bridge_PopLocalFrame);
    register_handler("DeleteLocalRef", bridge_DeleteLocalRef);
    register_handler("DeleteGlobalRef", bridge_DeleteGlobalRef);
    register_handler("NewObjectV", bridge_NewObjectV);
    register_handler("GetObjectClass", bridge_GetObjectClass);
    register_handler("CallObjectMethodV", bridge_CallObjectMethodV);
    register_handler("CallBooleanMethodV", bridge_CallBooleanMethodV);
    register_handler("CallIntMethodV", bridge_CallIntMethodV);
    register_handler("CallLongMethodV", bridge_CallLongMethodV);
    register_handler("CallVoidMethodV", bridge_CallVoidMethodV);
    register_handler("GetFieldID", bridge_GetFieldID);
    register_handler("GetBooleanField", bridge_GetBooleanField);
    register_handler("GetIntField", bridge_GetIntField);
    register_handler("GetFloatField", bridge_GetFloatField);
    register_handler("CallStaticObjectMethodV", bridge_CallStaticObjectMethodV);
    register_handler("CallStaticBooleanMethodV", bridge_CallStaticBooleanMethodV);
    register_handler("CallStaticIntMethodV", bridge_CallStaticIntMethodV);
    register_handler("CallStaticLongMethodV", bridge_CallStaticLongMethodV);
    register_handler("CallStaticFloatMethodV", bridge_CallStaticFloatMethodV);
    register_handler("CallStaticVoidMethodV", bridge_CallStaticVoidMethodV);
    register_handler("GetStaticFieldID", bridge_GetStaticFieldID);
    register_handler("GetStaticObjectField", bridge_GetStaticObjectField);
    register_handler("GetStringUTFLength", bridge_GetStringUTFLength);
    register_handler("ReleaseStringUTFChars", bridge_ReleaseStringUTFChars);
    register_handler("GetArrayLength", bridge_GetArrayLength);
    register_handler("GetObjectArrayElement", bridge_GetObjectArrayElement);
    register_handler("NewIntArray", bridge_NewIntArray);
    register_handler("NewByteArray", bridge_NewByteArray);
    register_handler("GetByteArrayElements", bridge_GetByteArrayElements);
    register_handler("ReleaseByteArrayElements", bridge_ReleaseByteArrayElements);
    register_handler("SetByteArrayRegion", bridge_SetByteArrayRegion);
    register_handler("GetIntArrayElements", bridge_GetIntArrayElements);
    register_handler("ReleaseIntArrayElements", bridge_ReleaseIntArrayElements);
    register_handler("SetIntArrayRegion", bridge_SetIntArrayRegion);
    register_handler("GetJavaVM", bridge_GetJavaVM);
    register_handler("GetEnv", bridge_GetEnv);
    register_handler("GetStringUTFRegion", bridge_GetStringUTFRegion);

    // OpenAL
    register_handler("alcOpenDevice", bridge_alcOpenDevice);
    register_handler("alcCreateContext", bridge_alcCreateContext);
    register_handler("alcMakeContextCurrent", bridge_alcMakeContextCurrent);
    register_handler("alGetError", bridge_alGetError);
    register_handler("alGenSources", bridge_alGenSources);
    register_handler("alGenBuffers", bridge_alGenBuffers);
    register_handler("alSourcePlay", bridge_alSourcePlay);
    register_handler("alSourceStop", bridge_alSourceStop);
    register_handler("alSourcePause", bridge_alSourcePause);
    register_handler("alSourcei", bridge_alSourcei);
    register_handler("alSourcef", bridge_alSourcef);
    register_handler("alSource3f", bridge_alSource3f);
    register_handler("alBufferData", bridge_alBufferData);
    register_handler("alSourceQueueBuffers", bridge_alSourceQueueBuffers);
    register_handler("alSourceUnqueueBuffers", bridge_alSourceUnqueueBuffers);
    register_handler("alGetSourcei", bridge_alGetSourcei);
    register_handler("alDeleteSources", bridge_alDeleteSources);
    register_handler("alDeleteBuffers", bridge_alDeleteBuffers);
    register_handler("alListenerf", bridge_alListenerf);
    register_handler("alListener3f", bridge_alListener3f);
    register_handler("alDistanceModel", bridge_alDistanceModel);
    register_handler("alcSuspendContext", bridge_alcSuspendContext);
    register_handler("alcProcessContext", bridge_alcProcessContext);
    register_handler("alcResume", bridge_alcResume);
    register_handler("alcSuspend", bridge_alcSuspend);
    register_handler("alcDestroyContext", bridge_alcDestroyContext);
    register_handler("alcCloseDevice", bridge_alcCloseDevice);
    register_handler("alGetSourcef", bridge_alGetSourcef);
    register_handler("alListenerfv", bridge_alListenerfv);
    register_handler("alSourceRewind", bridge_alSourceRewind);
    register_handler("alcGetCurrentContext", bridge_alcGetCurrentContext);

    // libc / Android
    register_handler("exit", bridge_exit);
    register_handler("read", bridge_read);
    register_handler("write", bridge_write);
    register_handler("writev", bridge_writev);
    register_handler("fstat", bridge_fstat);
    register_handler("poll", bridge_poll);
    register_handler("ioctl", bridge_ioctl);
    register_handler("perror", bridge_perror);
    register_handler("syscall", bridge_syscall);
    register_handler("_ctype_", bridge_ctype_);
    register_handler("strftime", bridge_strftime);
    register_handler("strncat", bridge_strncat);
    register_handler("fdopen", bridge_fdopen);
    register_handler("freopen", bridge_freopen);

    // Wide character bridges
    register_handler("wcslen", bridge_wcslen);
    register_handler("wmemcpy", bridge_wmemcpy);
    register_handler("wmemmove", bridge_wmemmove);
    register_handler("wmemset", bridge_wmemset);
    register_handler("wmemcmp", bridge_wmemcmp);
    register_handler("wmemchr", bridge_wmemchr);
    register_handler("mbrtowc", bridge_mbrtowc);
    register_handler("wcrtomb", bridge_wcrtomb);
    register_handler("getwc", bridge_getwc);
    register_handler("putwc", bridge_putwc);
    register_handler("ungetwc", bridge_ungetwc);
    register_handler("wcscoll", bridge_wcscoll);
    register_handler("wcsxfrm", bridge_wcsxfrm);
    register_handler("wcsftime", bridge_wcsftime);

    // String comparison/search
    register_handler("strcoll", bridge_strcoll);
    register_handler("strcspn", bridge_strcspn);
    register_handler("strpbrk", bridge_strpbrk);
    register_handler("strxfrm", bridge_strxfrm);

    // Compression
    register_handler("gzwrite", bridge_gzwrite);

    // GLES real bridge functions
    register_handler("glViewport", bridge_gl_viewport);
    register_handler("glClear", bridge_gl_clear);
    register_handler("glClearColor", bridge_gl_clear_color);
    register_handler("glEnable", bridge_gl_enable);
    register_handler("glDisable", bridge_gl_disable);
    register_handler("glBlendFunc", bridge_gl_blend_func);
    register_handler("glDrawArrays", bridge_gl_draw_arrays);
    register_handler("glDrawElements", bridge_gl_draw_elements);
    register_handler("glBindTexture", bridge_gl_bind_texture);
    register_handler("glTexImage2D", bridge_gl_tex_image_2d);
    register_handler("glTexSubImage2D", bridge_gl_tex_sub_image_2d);
    register_handler("glTexParameteri", bridge_gl_tex_parameteri);
    register_handler("glTexParameterf", bridge_gl_tex_parameterf);
    register_handler("glCompressedTexImage2D", bridge_gl_compressed_tex_image_2d);
    register_handler("sre_PVRTTextureLoadFromPVRBuffer", bridge_PVRTTextureLoadFromPVRBuffer);
    register_handler("glVertexPointer", bridge_gl_vertex_pointer);
    register_handler("glTexCoordPointer", bridge_gl_texcoord_pointer);
    register_handler("glColorPointer", bridge_gl_color_pointer);
    register_handler("glNormalPointer", bridge_gl_normal_pointer);
    register_handler("glEnableClientState", bridge_gl_enable_client_state);
    register_handler("glDisableClientState", bridge_gl_disable_client_state);
    register_handler("glMatrixMode", bridge_gl_matrix_mode);
    register_handler("glLoadIdentity", bridge_gl_load_identity);
    register_handler("glLoadMatrixf", bridge_gl_load_matrixf);
    register_handler("glMultMatrixf", bridge_gl_mult_matrixf);
    register_handler("glPushMatrix", bridge_gl_push_matrix);
    register_handler("glPopMatrix", bridge_gl_pop_matrix);
    register_handler("glOrthof", bridge_gl_orthof);
    register_handler("glFrustumf", bridge_gl_frustumf);
    register_handler("glTranslatef", bridge_gl_translatef);
    register_handler("glRotatef", bridge_gl_rotatef);
    register_handler("glScalef", bridge_gl_scalef);
    register_handler("glColor4f", bridge_gl_color4f);
    register_handler("glDepthFunc", bridge_gl_depth_func);
    register_handler("glDepthMask", bridge_gl_depth_mask);
    register_handler("glClearDepthf", bridge_gl_clear_depthf);
    register_handler("glLineWidth", bridge_gl_line_width);
    register_handler("glScissor", bridge_gl_scissor);
    register_handler("glColorMask", bridge_gl_color_mask);
    register_handler("glStencilFunc", bridge_glStencilFunc);
    register_handler("glStencilMask", bridge_glStencilMask);
    register_handler("glStencilOp", bridge_glStencilOp);
    register_handler("glActiveTexture", bridge_gl_active_texture);
    register_handler("glClientActiveTexture", bridge_gl_client_active_texture);
    register_handler("glPixelStorei", bridge_gl_pixel_storei);
    register_handler("glVertexAttribPointer", bridge_glVertexAttribPointer);
    register_handler("glEnableVertexAttribArray", bridge_glEnableVertexAttribArray);
    register_handler("glDisableVertexAttribArray", bridge_glDisableVertexAttribArray);
    register_handler("glUseProgram", bridge_glUseProgram);
    register_handler("glBindBuffer", bridge_gl_bind_buffer);
    register_handler("glBufferData", bridge_gl_buffer_data);
    register_handler("glBufferSubData", bridge_gl_buffer_sub_data);

    // GLES 2.0 Shader APIs
    register_handler("glCreateShader", bridge_glCreateShader);
    register_handler("glShaderSource", bridge_glShaderSource);
    register_handler("glCompileShader", bridge_glCompileShader);
    register_handler("glCreateProgram", bridge_glCreateProgram);
    register_handler("glAttachShader", bridge_glAttachShader);
    register_handler("glLinkProgram", bridge_glLinkProgram);
    register_handler("glGetUniformLocation", bridge_glGetUniformLocation);
    register_handler("glUniformMatrix4fv", bridge_glUniformMatrix4fv);
    register_handler("glUniform4fv", bridge_glUniform4fv);
    register_handler("glUniform1f", bridge_glUniform1f);
    register_handler("glUniform2f", bridge_glUniform2f);
    register_handler("glUniform3f", bridge_glUniform3f);
    register_handler("glUniform4f", bridge_glUniform4f);
    register_handler("glUniform1i", bridge_glUniform1i);
    register_handler("glGetShaderiv", bridge_glGetShaderiv);
    register_handler("glGetShaderInfoLog", bridge_glGetShaderInfoLog);
    register_handler("glGetProgramiv", bridge_glGetProgramiv);
    register_handler("glGetProgramInfoLog", bridge_glGetProgramInfoLog);
    register_handler("glBindAttribLocation", bridge_glBindAttribLocation);
    register_handler("glGetAttribLocation", bridge_glGetAttribLocation);
    register_handler("glDeleteShader", bridge_glDeleteShader);
    register_handler("glDeleteProgram", bridge_glDeleteProgram);
    register_handler("glDetachShader", bridge_glDetachShader);
    register_handler("glDeleteTextures", bridge_gl_delete_textures);
    register_handler("glDeleteBuffers", bridge_gl_delete_buffers);
    register_handler("glBindFramebuffer", bridge_gl_bind_framebuffer);
    register_handler("glGenFramebuffers", bridge_gl_gen_framebuffers);
    register_handler("glDeleteFramebuffers", bridge_gl_delete_framebuffers);
    register_handler("glFramebufferTexture2D", bridge_gl_framebuffer_texture2d);
    register_handler("glCheckFramebufferStatus", bridge_gl_framebuffer_status);
    register_handler("glGenTextures", bridge_glGenTextures);
    register_handler("glGenBuffers", bridge_glGenBuffers);
    register_handler("glGetError", bridge_glGetError);
    register_handler("glGetIntegerv", bridge_glGetIntegerv);
    register_handler("glGetFloatv", bridge_glGetFloatv);
    register_handler("glGetString", bridge_glGetString);
    register_handler("glAlphaFunc", bridge_gl_alpha_func);
    register_handler("glShadeModel", bridge_gl_shade_model);
    register_handler("glFogf", bridge_glFogf);
    register_handler("glFogfv", bridge_glFogfv);
    register_handler("glFogi", bridge_glFogi);
    register_handler("glMaterialf", bridge_glMaterialf);
    register_handler("glMaterialfv", bridge_glMaterialfv);
    register_handler("glLightf", bridge_glLightf);
    register_handler("glLightfv", bridge_glLightfv);
    register_handler("glLightModelf", bridge_glLightModelf);
    register_handler("glLightModelfv", bridge_glLightModelfv);
    register_handler("glPointSize", bridge_glPointSize);
    register_handler("glCullFace", bridge_glCullFace);
    register_handler("glHint", bridge_glHint);
    register_handler("glFlush", bridge_gl_flush);
    register_handler("glFinish", bridge_gl_finish);
    register_handler("glReadPixels", bridge_gl_noop);
    register_handler("glClearStencil", bridge_glClearStencil);
    register_handler("glColor4ub", bridge_glColor4ub);
    register_handler("glTexEnvi", bridge_glTexEnvi);
    register_handler("glTexEnvfv", bridge_glTexEnvfv);
    register_handler("glTexParameterf", bridge_gl_tex_parameterf);

    // EGL
    register_handler("eglGetDisplay", bridge_gl_noop);
    register_handler("eglSwapBuffers", bridge_gl_noop);
    register_handler("eglGetProcAddress", bridge_eglGetProcAddress);

    register_handler("glGenRenderbuffers", bridge_gl_gen_renderbuffers);
    register_handler("glBindRenderbuffer", bridge_gl_bind_renderbuffer);
    register_handler("glRenderbufferStorage", bridge_gl_renderbuffer_storage);
    register_handler("glFramebufferRenderbuffer", bridge_gl_framebuffer_renderbuffer);
    register_handler("glDeleteRenderbuffers", bridge_gl_delete_renderbuffers);

    // Luasocket support
    register_handler("socket", bridge_socket);
    register_handler("connect", bridge_connect);
    register_handler("bind", bridge_bind);
    register_handler("listen", bridge_listen);
    register_handler("accept", bridge_accept);
    register_handler("send", bridge_send);
    register_handler("recv", bridge_recv);
    register_handler("sendto", bridge_sendto);
    register_handler("recvfrom", bridge_recvfrom);
    register_handler("setsockopt", bridge_setsockopt);
    register_handler("getsockopt", bridge_getsockopt);
    register_handler("getsockname", bridge_getsockname);
    register_handler("getpeername", bridge_getpeername);
    register_handler("shutdown", bridge_shutdown);
    register_handler("fcntl", bridge_fcntl);
    register_handler("select", bridge_select);
    register_handler("close", bridge_close);
    register_handler("gethostbyname", bridge_gethostbyname);
    register_handler("getaddrinfo", bridge_getaddrinfo);
    register_handler("freeaddrinfo", bridge_freeaddrinfo);
    register_handler("gai_strerror", bridge_gai_strerror);
    register_handler("htons", bridge_htons);
    register_handler("htonl", bridge_htonl);
    register_handler("ntohs", bridge_ntohs);
    register_handler("ntohl", bridge_ntohl);
    register_handler("inet_ntoa", bridge_inet_ntoa);
    register_handler("inet_pton", bridge_inet_pton);
    register_handler("inet_ntop", bridge_inet_ntop);
    register_handler("inet_addr", bridge_inet_addr);
}

// ======================= DRAW BATCHER API =======================

void draw_batcher_init() {
    g_batcher.init();
    std::cout << "[Batcher] Draw call batcher initialized (VBO=" << g_batcher.vbo 
              << ", max_verts=" << BATCH_MAX_VERTS << ")" << std::endl;
}

void draw_batcher_flush() {
    if (g_batcher.initialized) {
        g_batcher.flush();
    }
}

void draw_batcher_print_stats() {
    if (!g_batcher.initialized) return;
    if (g_batcher.total_input_draws > 0) {
        std::cout << "[Batcher] Input draws=" << g_batcher.total_input_draws
                  << " -> Batched flushes=" << g_batcher.flushed_draws
                  << " (reduction: " << g_batcher.total_input_draws << " -> " << g_batcher.flushed_draws << ")"
                  << std::endl;
    }
    g_batcher.reset_stats();
}

/* =====================================================================
 * SRE Music Host API — called by main.cpp to execute music commands
 * from the SRE guest. Full control over the music subsystem.
 * ===================================================================== */

static std::string g_sre_music_current_track = "";  /* What's currently loaded */

bool sre_music_host_load(const std::string& name) {
    extern std::string g_assets_dir;
    
    /* ===== Playlist Name → Track Filename mapping =====
     * The engine calls PlayMusicWithName("outdoors_light") but the actual
     * file is music_squire_new2.ogg. This table maps playlist→track.
     * Easy to edit — just change the right column!
     * Same track can serve multiple playlists (Swordigo reuses music). */
    static const struct { const char* playlist; const char* track; } music_map[] = {
        { "menu",            "1_hero2"         },  /* Main menu / title screen */
        { "title",           "1_hero2"         },  /* Title screen (alias) */
        { "house",           "1_plaintest2"    },  /* House interior */
        { "outdoors_light",  "squire_new2"     },  /* Plains / outdoors light */
        { "outdoors_dark",   "1_hero2"         },  /* Dark outdoors, adventure */
        { "dungeon1",        "1_dung73"        },  /* Dungeons */
        { "cave",            "2cave2"          },  /* Caves */
        { "forest",          "2cave2"          },  /* Forest areas */
        { "boss",            "1_boss23"        },  /* Boss fights */
        { "bosskill",        "momentofwonder"  },  /* After killing boss — fades in slowly */
        { "gameover",        "gameover"        },  /* Death screen */
        { "heartbeat",       "heartbeat"       },  /* Tension / danger */
        { "momentofwonder",  "momentofwonder"  },  /* Discovery / portals */
        { "squire",          "squire_new2"     },  /* Town / shop */
        { NULL, NULL }
    };
    
    // Resolve playlist name → track name
    std::string track = name;  // fallback: use playlist name as-is
    for (int i = 0; music_map[i].playlist; i++) {
        if (name == music_map[i].playlist) {
            track = music_map[i].track;
            break;
        }
    }
    
    std::cout << "[SRE-Music] Loading: \"" << name << "\" → track \"" << track << "\"" << std::endl;
    
    // Death detection
    if (name.find("gameover") != std::string::npos) {
        extern int g_death_detected_countdown;
        g_death_detected_countdown = 180;
        std::cout << "[SRE-Music] Death detected (gameover music)" << std::endl;
    }
    
    // Stop current playback
    if (g_music_source != 0) {
        alSourceStop(g_music_source);
        alSourcei(g_music_source, AL_BUFFER, 0);
    }
    
    // === MOD MUSIC: Check mod directories first ===
    // Search by BOTH the original playlist name AND the resolved track name,
    // because mod files are typically named after playlists (boss.mp3, squire.mp3)
    // while vanilla files use track names (1_boss23.ogg, squire_new2.ogg).
    std::string mod_music_path;
    {
        std::string mods_dir = get_user_data_dir() + "/mods";
        if (std::filesystem::exists(mods_dir)) {
            for (auto& mod_entry : std::filesystem::directory_iterator(mods_dir)) {
                if (!mod_entry.is_directory()) continue;
                // Skip disabled mods (dot-prefixed folders)
                std::string dname = mod_entry.path().filename().string();
                if (!dname.empty() && dname[0] == '.') continue;
                std::string mod_music_dir = mod_entry.path().string() + "/assets/music";
                if (!std::filesystem::exists(mod_music_dir)) continue;
                
                // Try playlist name first (what mods typically use), then track name
                std::string try_names[] = {
                    mod_music_dir + "/" + name + ".mp3",
                    mod_music_dir + "/" + name + ".ogg",
                    mod_music_dir + "/" + name + ".wav",
                    mod_music_dir + "/" + track + ".mp3",
                    mod_music_dir + "/" + track + ".ogg",
                    mod_music_dir + "/" + track + ".wav",
                    mod_music_dir + "/music_" + name + ".mp3",
                    mod_music_dir + "/music_" + track + ".mp3",
                };
                for (auto& try_path : try_names) {
                    if (std::filesystem::exists(try_path)) {
                        mod_music_path = try_path;
                        std::cout << "[SRE-Music] MOD override: " << try_path << std::endl;
                        break;
                    }
                }
                if (!mod_music_path.empty()) break;
            }
        }
    }

    // Build paths using resolved track name
    std::string music_dir = get_data_path(g_assets_dir + "/resources/music");
    std::string music_dir_alt = get_data_path(g_assets_dir + "/music");
    std::string res_raw = get_data_path("res/raw");
    
    // Sanitize: replace dashes with underscores
    std::string safe = track;
    for (size_t i = 0; i < safe.size(); i++) {
        if (safe[i] == '-') safe[i] = '_';
    }
    
    // === SMARTER PATH SEARCH ORDER ===
    // Try each location with ALL formats before moving to next location
    // This ensures we find MP3 files in game-specific directories (e.g., rl_assets/music)
    // instead of skipping to res/raw too early
    
    std::vector<std::string> paths;
    
    // 1. Mod overrides (already checked above, highest priority)
    if (!mod_music_path.empty()) {
        paths.push_back(mod_music_path);
    }
    
    // 2. Game-specific music directory (e.g., assets/resources/music or rl_assets/music)
    //    Try ALL formats here before moving on
    const std::string formats[] = {".mp3", ".ogg", ".wav"};
    const std::string prefixes[] = {"music_", ""};
    
    // Try primary music directory with all formats and name variants
    for (const auto& fmt : formats) {
        for (const auto& pfx : prefixes) {
            paths.push_back(music_dir + "/" + pfx + safe + fmt);
        }
    }
    
    // Try alternate music directory with all formats and name variants
    for (const auto& fmt : formats) {
        for (const auto& pfx : prefixes) {
            paths.push_back(music_dir_alt + "/" + pfx + safe + fmt);
        }
    }
    
    // 3. Fallback: res/raw/ (vanilla location, lowest priority)
    for (const auto& fmt : formats) {
        for (const auto& pfx : prefixes) {
            paths.push_back(res_raw + "/" + pfx + safe + fmt);
        }
    }
    
    // Remove duplicates while preserving order
    std::vector<std::string> unique_paths;
    for (const auto& p : paths) {
        if (std::find(unique_paths.begin(), unique_paths.end(), p) == unique_paths.end()) {
            unique_paths.push_back(p);
        }
    }
    paths = unique_paths;
    
    if (g_music_buffer == 0) {
        alGenBuffers(1, &g_music_buffer);
    }
    
    bool loaded = false;
    for (size_t i = 0; i < paths.size() && !loaded; i++) {
        // Check if file exists on disk first to prevent libmpg123 error noise
        std::ifstream check_file(paths[i]);
        if (!check_file.good()) continue;
        check_file.close();

        std::cout << "[SRE-Music]   try[" << i << "]: " << paths[i] << std::endl;
        // Detect format by extension
        std::string ext = paths[i].substr(paths[i].rfind('.'));
        if (ext == ".mp3") {
            loaded = load_mp3_to_buffer(paths[i], g_music_buffer);
        } else if (ext == ".wav") {
            loaded = load_wav_to_buffer(paths[i], g_music_buffer);
        } else {
            loaded = load_ogg_to_buffer(paths[i], g_music_buffer);
        }
        if (loaded) {
            std::cout << "[SRE-Music] ✓ Loaded: " << paths[i] << std::endl;
        }
    }
    
    if (loaded) {
        if (g_music_source == 0) {
            alGenSources(1, &g_music_source);
        }
        alSourcei(g_music_source, AL_BUFFER, g_music_buffer);
        alSourcei(g_music_source, AL_LOOPING, g_music_looping ? AL_TRUE : AL_FALSE);
        alSourcef(g_music_source, AL_GAIN, g_music_volume);
        alSourcePlay(g_music_source);
        g_sre_music_current_track = name;
        std::cout << "[SRE-Music] Playing: \"" << name << "\"" << std::endl;
        return true;
    } else {
        std::cerr << "[SRE-Music] ⚠ Failed to load: \"" << name << "\"" << std::endl;
        g_sre_music_current_track = "";
        return false;
    }
}

void sre_music_host_play() {
    if (g_music_source != 0) {
        alSourcePlay(g_music_source);
    }
}

void sre_music_host_pause() {
    if (g_music_source != 0) {
        alSourcePause(g_music_source);
    }
}

void sre_music_host_stop() {
    if (g_music_source != 0) {
        alSourceStop(g_music_source);
    }
}

void sre_music_host_set_volume(float vol) {
    g_music_volume = vol;
    if (g_music_volume < 0.0f) g_music_volume = 0.0f;
    if (g_music_volume > 1.0f) g_music_volume = 1.0f;
    if (g_music_source != 0) {
        alSourcef(g_music_source, AL_GAIN, g_music_volume);
    }
}

void sre_music_host_set_looping(bool loop) {
    g_music_looping = loop;
    if (g_music_source != 0) {
        alSourcei(g_music_source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    }
}

const std::string& sre_music_host_get_track() {
    return g_sre_music_current_track;
}

float sre_music_host_get_volume() {
    return g_music_volume;
}

bool sre_music_host_is_playing() {
    if (g_music_source == 0) return false;
    ALint state;
    alGetSourcei(g_music_source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}
