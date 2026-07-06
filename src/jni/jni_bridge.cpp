#include "jni_bridge.h"
#include "platform/emulator.h"
#include "platform/io_thread.h"
#include "platform/data_path.h"
#include "android/asset_manager.h"
#include "platform/video_background.h"
#include <iostream>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <sys/time.h>
#include <filesystem>
namespace fs = std::filesystem;
#include <vorbis/vorbisfile.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"
#include <GL/glext.h>
#include <AL/al.h>
#include <AL/alc.h>

#ifdef VULKAN_BACKEND
#include "platform/vulkan_backend.h"
extern GraphicsAPI g_graphics_api;
extern VulkanBackend g_vk_backend;
#endif

// When true, GL bridge functions call real OpenGL instead of no-ops
bool g_display_active = false;
std::string g_save_dir = "./save";  // Default; overwritten by main.cpp at startup
int g_death_detected_countdown = 0;  // Set when gameover music loads, counted down in game loop
bool g_text_input_active = false;   // Set by JNI bridge when game requests text input
std::string g_text_input_buffer;    // Current text buffer for text input mode
bool g_text_input_pending_result = false; // Set by zenity thread when dialog closes


// --- Frame Statistics ---
FrameStats g_frame_stats;

// --- Handle Management (shared between ARM32 and ARM64 bridges) ---
std::unordered_map<uint32_t, void*> g_handle_to_ptr;
std::unordered_map<void*, uint32_t> g_ptr_to_handle;
uint32_t g_next_handle = 0x88880001;
std::mutex g_handles_mutex;

uint32_t register_pointer(void* ptr) {
    if (!ptr) return 0;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    if (g_ptr_to_handle.count(ptr)) return g_ptr_to_handle[ptr];
    uint32_t handle = g_next_handle++;
    g_handle_to_ptr[handle] = ptr;
    g_ptr_to_handle[ptr] = handle;
    return handle;
}

void* get_pointer(uint32_t handle) {
    if (handle == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    if (g_handle_to_ptr.count(handle)) return g_handle_to_ptr[handle];
    return (void*)(uintptr_t)handle; 
}

void release_pointer(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    if (g_ptr_to_handle.count(ptr)) {
        uint32_t handle = g_ptr_to_handle[ptr];
        g_ptr_to_handle.erase(ptr);
        g_handle_to_ptr.erase(handle);
    }
}

// --- Soft-Float Register Conversion Helpers ---
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

// --- JniBridge Methods ---
JniBridge::JniBridge() : next_addr(BRIDGE_BASE) {}

uint32_t JniBridge::get_address(const std::string& name) {
    if (name_to_addr.count(name)) return name_to_addr[name];
    
    uint32_t addr = next_addr;
    next_addr += 4;
    
    name_to_addr[name] = addr;
    addr_to_func[addr] = {name, addr, nullptr};
    
    return addr;
}

std::string JniBridge::get_name(uint32_t address) {
    if (addr_to_func.count(address)) return addr_to_func[address].name;
    return "UnknownBridge";
}

uint32_t JniBridge::lookup_proc_address(const std::string& name) {
    auto it = name_to_addr.find(name);
    if (it == name_to_addr.end()) return 0;
    uint32_t addr = it->second;
    auto fit = addr_to_func.find(addr);
    if (fit == addr_to_func.end() || !fit->second.handler) return 0;
    return addr;
}

void JniBridge::register_handler(const std::string& name, BridgeHandler handler) {
    uint32_t addr = get_address(name);
    addr_to_func[addr].handler = handler;
}

void JniBridge::call_handler(uint32_t address, void* emu_ptr) {
    if (addr_to_func.count(address)) {
        BridgeFunction& func = addr_to_func[address];
        Emulator* emu = (Emulator*)emu_ptr;
        // Only log when not in quiet mode, and skip trivial calls
        if (!emu->quiet_mode) {
            static const std::unordered_map<std::string, bool> quiet_funcs = {
                {"memcpy",1},{"memset",1},{"memmove",1},{"memcmp",1},{"memchr",1},
                {"strlen",1},{"malloc",1},{"calloc",1},{"realloc",1},{"free",1},
                {"__aeabi_memclr",1},{"__aeabi_memclr4",1},{"__aeabi_memclr8",1},
                {"__aeabi_memset",1},{"__aeabi_memset4",1},{"__aeabi_memset8",1},
                {"__aeabi_memcpy",1},{"__aeabi_memcpy4",1},{"__aeabi_memcpy8",1},
                {"__aeabi_memmove",1},{"__aeabi_memmove4",1},{"__aeabi_memmove8",1},
                {"__aeabi_uidiv",1},{"__aeabi_idiv",1},{"__aeabi_uidivmod",1},
                {"__aeabi_idivmod",1},{"__aeabi_ldivmod",1},{"__aeabi_uldivmod",1},
                {"strchr",1},{"strrchr",1},{"strcpy",1},{"strncpy",1},
                {"strcmp",1},{"strncmp",1},{"strcat",1},{"strstr",1},
                {"cosf",1},{"sinf",1},{"roundf",1},{"floorf",1},{"ceilf",1},
                {"sqrtf",1},{"atan2f",1},{"powf",1},{"sincosf",1},
                {"strtol",1},{"strtoul",1},{"atoi",1},{"atof",1},
                {"wctob",1},{"btowc",1},{"__ctype_get_mb_cur_max",1},
                {"pthread_mutex_init",1},{"pthread_mutex_lock",1},{"pthread_mutex_unlock",1},
                {"pthread_mutex_destroy",1},{"pthread_cond_init",1},{"pthread_cond_signal",1},
                {"pthread_cond_wait",1},{"pthread_cond_destroy",1},{"pthread_cond_broadcast",1},
            };
            if (!quiet_funcs.count(func.name) || func.name == "GetMethodID") {
                std::cout << "[Bridge] Call: " << func.name << std::endl;
            }
        }
        if (func.handler) {
            func.handler(emu_ptr);
        } else {
            static std::unordered_set<std::string> warned_funcs;
            if (!warned_funcs.count(func.name)) {
                std::cout << "[WARNING] Call to UNIMPLEMENTED bridge function: " << func.name << std::endl;
                warned_funcs.insert(func.name);
            }
        }
    } else {
        // Completely unregistered bridge address — this function was imported
        // by libswordigo but has no handler. It silently returns via bx lr.
        // Log it so we can find missing bridges (e.g. timer_create, setitimer).
        Emulator* emu = (Emulator*)emu_ptr;
        static std::unordered_set<uint32_t> warned_addrs;
        if (!warned_addrs.count(address)) {
            uint32_t lr = emu->get_reg(14);
            std::cerr << "[Bridge] UNREGISTERED call at 0x" << std::hex << address
                      << " (caller LR=0x" << lr << ")" << std::dec << std::endl;
            warned_addrs.insert(address);
        }
    }
}



// --- Memory Allocator Bridges ---
struct DeferredBlock {
    uint32_t addr;
    uint32_t size;
    uint64_t alloc_index;
};
static uint32_t g_guest_heap_ptr = 0x20000000; // Start heap at 512MB
static std::unordered_map<uint32_t, uint32_t> g_guest_allocs;
static std::vector<DeferredBlock> g_deferred_free_list;
static uint64_t g_alloc_counter = 0;
static std::mutex g_heap_mutex;

static uint32_t host_malloc_locked(uint32_t size) {
    size = (size + 7) & ~7;
    // Only recycle if heap size exceeds 580MB (fast path O(1) bump allocator under 580MB)
    if (g_guest_heap_ptr - 0x20000000 > 580ULL * 1024 * 1024) {
        size_t search_limit = std::min(g_deferred_free_list.size(), (size_t)50);
        for (size_t i = 0; i < search_limit; i++) {
            const auto& db = g_deferred_free_list[i];
            // Chronological list: if the oldest elements aren't ready yet, none are!
            if (g_alloc_counter - db.alloc_index <= 15000) {
                break; // Stop searching completely
            }
            if (db.size >= size && db.size <= size + 128) {
                uint32_t addr = db.addr;
                g_guest_allocs[addr] = size;
                g_deferred_free_list.erase(g_deferred_free_list.begin() + i);
                g_alloc_counter++;
                return addr;
            }
        }
    }
    uint32_t addr = g_guest_heap_ptr;
    g_guest_heap_ptr += size;
    g_guest_allocs[addr] = size;
    g_alloc_counter++;
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
        g_deferred_free_list.push_back(db);
        g_guest_allocs.erase(ptr);
    }
}

void bridge_malloc(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t size = emu->get_reg(0);
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    uint32_t addr = host_malloc_locked(size);
    emu->set_reg(0, addr);
}

void bridge_calloc(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t num = emu->get_reg(0);
    uint32_t size = emu->get_reg(1);
    uint32_t total = num * size;
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    uint32_t addr = host_malloc_locked(total);
    std::memset(emu->get_memory_base() + addr, 0, total);
    emu->set_reg(0, addr);
}

void bridge_realloc(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t ptr = emu->get_reg(0);
    uint32_t size = emu->get_reg(1);
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    if (ptr == 0) {
        uint32_t addr = host_malloc_locked(size);
        emu->set_reg(0, addr);
        return;
    }
    if (size == 0) {
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
            emu->set_reg(0, ptr);
            return;
        }
        
        if (ptr + aligned_old_size == g_guest_heap_ptr) {
            g_guest_heap_ptr = ptr + aligned_new_size;
            g_guest_allocs[ptr] = size;
            emu->set_reg(0, ptr);
            return;
        }
        
        uint32_t addr = host_malloc_locked(size);
        uint32_t copy_size = (old_size < size) ? old_size : size;
        if (copy_size > 0) {
            std::memcpy(emu->get_memory_base() + addr, emu->get_memory_base() + ptr, copy_size);
        }
        host_free_locked(ptr);
        emu->set_reg(0, addr);
    } else {
        uint32_t addr = host_malloc_locked(size);
        if (size > 0) {
            std::memcpy(emu->get_memory_base() + addr, emu->get_memory_base() + ptr, size);
        }
        emu->set_reg(0, addr);
    }
}

void bridge_free(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t ptr = emu->get_reg(0);
    if (ptr) {
        std::lock_guard<std::mutex> lock(g_heap_mutex);
        host_free_locked(ptr);
    }
}

// --- Standard C Memory & String Bridges ---
void bridge_memcpy(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[MEM] memcpy(dest=0x" << std::hex << dest << ", src=0x" << src << ", n=" << std::dec << n << ")" << std::endl;
    }
    if (n > 0) {
        std::memmove(emu->get_memory_base() + dest, emu->get_memory_base() + src, n);
    }
    emu->set_reg(0, dest);
}

void bridge_memset(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t c = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (n > 0) {
        std::memset(emu->get_memory_base() + dest, c, n);
    }
    emu->set_reg(0, dest);
}

void bridge_memmove(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[MEM] memmove(dest=0x" << std::hex << dest << ", src=0x" << src << ", n=" << std::dec << n << ")" << std::endl;
    }
    if (n > 0) {
        std::memmove(emu->get_memory_base() + dest, emu->get_memory_base() + src, n);
    }
    emu->set_reg(0, dest);
}

void bridge_strlen(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t str = emu->get_reg(0);
    const char* s = (const char*)(emu->get_memory_base() + str);
    if (!emu->quiet_mode) {
        std::cout << "[STR] strlen(str=0x" << std::hex << str << ") -> \"" << s << "\"" << std::dec << std::endl;
    }
    emu->set_reg(0, std::strlen(s));
}

void bridge_memcmp(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t str1 = emu->get_reg(0);
    uint32_t str2 = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    int res = 0;
    if (n > 0) {
        res = std::memcmp(emu->get_memory_base() + str1, emu->get_memory_base() + str2, n);
    }
    emu->set_reg(0, res);
}

// --- EABI Memory Helpers ---
void bridge_aeabi_memclr(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t n = emu->get_reg(1);
    if (n > 0) {
        std::memset(emu->get_memory_base() + dest, 0, n);
    }
}

void bridge_aeabi_memset(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t n = emu->get_reg(1);
    uint32_t c = emu->get_reg(2);
    if (n > 0) {
        std::memset(emu->get_memory_base() + dest, c, n);
    }
}

void bridge_aeabi_memcpy(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (n > 0) {
        std::memmove(emu->get_memory_base() + dest, emu->get_memory_base() + src, n);
    }
}

void bridge_aeabi_memmove(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    if (n > 0) {
        std::memmove(emu->get_memory_base() + dest, emu->get_memory_base() + src, n);
    }
}

// --- EABI Division Helpers ---
void bridge_aeabi_uidiv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t num = emu->get_reg(0);
    uint32_t den = emu->get_reg(1);
    emu->set_reg(0, (den == 0) ? 0 : (num / den));
}

void bridge_aeabi_idiv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    int32_t num = (int32_t)emu->get_reg(0);
    int32_t den = (int32_t)emu->get_reg(1);
    emu->set_reg(0, (den == 0) ? 0 : (num / den));
}

void bridge_aeabi_uidivmod(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t num = emu->get_reg(0);
    uint32_t den = emu->get_reg(1);
    if (den == 0) {
        emu->set_reg(0, 0);
        emu->set_reg(1, 0);
    } else {
        emu->set_reg(0, num / den);
        emu->set_reg(1, num % den);
    }
}

void bridge_aeabi_idivmod(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    int32_t num = (int32_t)emu->get_reg(0);
    int32_t den = (int32_t)emu->get_reg(1);
    if (den == 0) {
        emu->set_reg(0, 0);
        emu->set_reg(1, 0);
    } else {
        emu->set_reg(0, num / den);
        emu->set_reg(1, num % den);
    }
}

void bridge_aeabi_ldivmod(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint64_t num_low = emu->get_reg(0);
    uint64_t num_high = emu->get_reg(1);
    uint64_t den_low = emu->get_reg(2);
    uint64_t den_high = emu->get_reg(3);
    int64_t num = (int64_t)((num_high << 32) | num_low);
    int64_t den = (int64_t)((den_high << 32) | den_low);
    if (den == 0) {
        emu->set_reg(0, 0); emu->set_reg(1, 0);
        emu->set_reg(2, 0); emu->set_reg(3, 0);
    } else {
        int64_t quot = num / den;
        int64_t rem = num % den;
        emu->set_reg(0, (uint32_t)(quot & 0xFFFFFFFF));
        emu->set_reg(1, (uint32_t)((quot >> 32) & 0xFFFFFFFF));
        emu->set_reg(2, (uint32_t)(rem & 0xFFFFFFFF));
        emu->set_reg(3, (uint32_t)((rem >> 32) & 0xFFFFFFFF));
    }
}

void bridge_aeabi_uldivmod(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint64_t num_low = emu->get_reg(0);
    uint64_t num_high = emu->get_reg(1);
    uint64_t den_low = emu->get_reg(2);
    uint64_t den_high = emu->get_reg(3);
    uint64_t num = ((num_high << 32) | num_low);
    uint64_t den = ((den_high << 32) | den_low);
    if (den == 0) {
        emu->set_reg(0, 0); emu->set_reg(1, 0);
        emu->set_reg(2, 0); emu->set_reg(3, 0);
    } else {
        uint64_t quot = num / den;
        uint64_t rem = num % den;
        emu->set_reg(0, (uint32_t)(quot & 0xFFFFFFFF));
        emu->set_reg(1, (uint32_t)((quot >> 32) & 0xFFFFFFFF));
        emu->set_reg(2, (uint32_t)(rem & 0xFFFFFFFF));
        emu->set_reg(3, (uint32_t)((rem >> 32) & 0xFFFFFFFF));
    }
}

// --- Soft-Float Math Bridges ---
void bridge_cosf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::cos(x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] cosf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_sinf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::sin(x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] sinf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_atan2f(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float y = uint_to_float(emu->get_reg(0));
    float x = uint_to_float(emu->get_reg(1));
    float res = std::atan2(y, x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] atan2f(" << y << ", " << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_powf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float y = uint_to_float(emu->get_reg(1));
    float res = std::pow(x, y);
    if (!emu->quiet_mode) {
        std::cout << "[Math] powf(" << x << ", " << y << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_pow(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    // ARM softfp ABI: double in R0:R1 (base), R2:R3 (exponent)
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t r2 = emu->get_reg(2), r3 = emu->get_reg(3);
    double base, exp_val;
    uint64_t base_i = ((uint64_t)r1 << 32) | r0;
    uint64_t exp_i  = ((uint64_t)r3 << 32) | r2;
    memcpy(&base, &base_i, 8);
    memcpy(&exp_val, &exp_i, 8);
    double res = std::pow(base, exp_val);
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

// Double-precision math bridges (ARM softfp: double in R0:R1, result in R0:R1)
// These are needed for 3D billboard/rotation calculations on spinning items

void bridge_sin(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    double res = std::sin(x);
    if (std::isnan(res)) {
        static int c = 0; if (++c <= 3)
            std::cerr << "[Math-NaN] sin(" << x << ") LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl;
    }
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

void bridge_cos(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    double res = std::cos(x);
    if (std::isnan(res)) {
        static int c = 0; if (++c <= 3)
            std::cerr << "[Math-NaN] cos(" << x << ") LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl;
    }
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

void bridge_acos(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    double res = std::acos(x);
    if (std::isnan(res)) {
        static int c = 0; if (++c <= 3)
            std::cerr << "[Math-NaN] acos(" << x << ") -> NaN! LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl;
    }
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

void bridge_round(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    double res = std::round(x);
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

void bridge_tan(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    // Android ARM uses softfp for doubles: R0, R1
    uint32_t r0 = emu->get_reg(0);
    uint32_t r1 = emu->get_reg(1);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    double res = std::tan(x);
    if (!emu->quiet_mode || std::isnan(res)) {
        std::cout << "[Math] tan(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

// --- Missing double-precision math bridges (critical for Lua) ---
// Lua uses double for ALL numbers. Without these, gate timers, trigger
// distances, and state transitions return garbage — causing the ARM32
// gate/obstacle progression bug.

// Helper macro: single-arg double function (softfp: R0:R1 -> R0:R1)
#define BRIDGE_DOUBLE_1ARG(fname, cfunc) \
void bridge_##fname(void* emu_ptr) { \
    Emulator* emu = (Emulator*)emu_ptr; \
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1); \
    double x; \
    uint64_t x_i = ((uint64_t)r1 << 32) | r0; \
    memcpy(&x, &x_i, 8); \
    double res = cfunc(x); \
    if (std::isnan(res) || std::isinf(res)) { \
        static int nan_count_##fname = 0; \
        if (++nan_count_##fname <= 5) { \
            std::cerr << "[Math-NaN] " << #fname << "(" << x << ") -> " << res \
                      << " LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl; \
        } \
    } \
    uint64_t res_i; \
    memcpy(&res_i, &res, 8); \
    emu->set_reg(0, (uint32_t)res_i); \
    emu->set_reg(1, (uint32_t)(res_i >> 32)); \
}

// Helper macro: two-arg double function (softfp: R0:R1, R2:R3 -> R0:R1)
#define BRIDGE_DOUBLE_2ARG(fname, cfunc) \
void bridge_##fname(void* emu_ptr) { \
    Emulator* emu = (Emulator*)emu_ptr; \
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1); \
    uint32_t r2 = emu->get_reg(2), r3 = emu->get_reg(3); \
    double a, b; \
    uint64_t a_i = ((uint64_t)r1 << 32) | r0; \
    uint64_t b_i = ((uint64_t)r3 << 32) | r2; \
    memcpy(&a, &a_i, 8); \
    memcpy(&b, &b_i, 8); \
    double res = cfunc(a, b); \
    if (std::isnan(res) || std::isinf(res)) { \
        static int nan_count_##fname = 0; \
        if (++nan_count_##fname <= 5) { \
            std::cerr << "[Math-NaN] " << #fname << "(" << a << ", " << b << ") -> " << res \
                      << " LR=0x" << std::hex << emu->get_lr() << std::dec << std::endl; \
        } \
    } \
    uint64_t res_i; \
    memcpy(&res_i, &res, 8); \
    emu->set_reg(0, (uint32_t)res_i); \
    emu->set_reg(1, (uint32_t)(res_i >> 32)); \
}

BRIDGE_DOUBLE_1ARG(floor_d, std::floor)
BRIDGE_DOUBLE_1ARG(ceil_d, std::ceil)
BRIDGE_DOUBLE_1ARG(sqrt_d, std::sqrt)
BRIDGE_DOUBLE_1ARG(fabs_d, std::fabs)
BRIDGE_DOUBLE_1ARG(log_d, std::log)
BRIDGE_DOUBLE_1ARG(log10_d, std::log10)
BRIDGE_DOUBLE_1ARG(log2_d, std::log2)
BRIDGE_DOUBLE_1ARG(exp_d, std::exp)
BRIDGE_DOUBLE_1ARG(asin_d, std::asin)
BRIDGE_DOUBLE_1ARG(atan_d, std::atan)

BRIDGE_DOUBLE_2ARG(fmod_d, std::fmod)
BRIDGE_DOUBLE_2ARG(atan2_d, std::atan2)
BRIDGE_DOUBLE_2ARG(ldexp_d, std::ldexp)

// frexp: double frexp(double x, int* exp) — writes int via pointer in R2
void bridge_frexp_d(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t exp_ptr = emu->get_reg(2);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    int exp_val;
    double res = std::frexp(x, &exp_val);
    if (exp_ptr) *(int32_t*)(memory + exp_ptr) = exp_val;
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

// modf: double modf(double x, double* iptr) — writes integer part via pointer in R2
void bridge_modf_d(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t iptr = emu->get_reg(2);
    double x;
    uint64_t x_i = ((uint64_t)r1 << 32) | r0;
    memcpy(&x, &x_i, 8);
    double int_part;
    double res = std::modf(x, &int_part);
    if (iptr) {
        uint64_t ip_i;
        memcpy(&ip_i, &int_part, 8);
        *(uint64_t*)(memory + iptr) = ip_i;
    }
    uint64_t res_i;
    memcpy(&res_i, &res, 8);
    emu->set_reg(0, (uint32_t)res_i);
    emu->set_reg(1, (uint32_t)(res_i >> 32));
}

void bridge_tanf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::tan(x);
    if (!emu->quiet_mode || std::isnan(res)) {
        std::cout << "[Math] tanf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_roundf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::round(x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] roundf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_floorf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::floor(x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] floorf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_ceilf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::ceil(x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] ceilf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}

void bridge_sqrtf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float res = std::sqrt(x);
    if (!emu->quiet_mode) {
        std::cout << "[Math] sqrtf(" << x << ") -> " << res << (std::isnan(res) ? " (NaN!)" : "") << std::endl;
    }
    emu->set_reg(0, float_to_uint(res));
}


void bridge_acosf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    emu->set_reg(0, float_to_uint(std::acos(x)));
}

void bridge_asinf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    emu->set_reg(0, float_to_uint(std::asin(x)));
}

void bridge_atanf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    emu->set_reg(0, float_to_uint(std::atan(x)));
}

void bridge_fmodf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    float y = uint_to_float(emu->get_reg(1));
    emu->set_reg(0, float_to_uint(std::fmod(x, y)));
}

void bridge_expf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    emu->set_reg(0, float_to_uint(std::exp(x)));
}

void bridge_logf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    emu->set_reg(0, float_to_uint(std::log(x)));
}

void bridge_log10f(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    emu->set_reg(0, float_to_uint(std::log10(x)));
}

void bridge_sincosf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    float x = uint_to_float(emu->get_reg(0));
    uint32_t sin_ptr = emu->get_reg(1);
    uint32_t cos_ptr = emu->get_reg(2);
    float s = std::sin(x);
    float c = std::cos(x);
    uint8_t* memory = emu->get_memory_base();
    *(uint32_t*)(memory + sin_ptr) = float_to_uint(s);
    *(uint32_t*)(memory + cos_ptr) = float_to_uint(c);
}

// --- JNI Standard Bridges ---
void bridge_FindClass(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(1);
    const char* name = (const char*)(memory + name_ptr);
    std::cout << "[JNI] FindClass: " << name << std::endl;
    emu->set_reg(0, 0x12340001);
}

void bridge_GetMethodID(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
    if (!emu->quiet_mode) {
        std::cout << "[JNI] GetMethodID: " << name << " -> 0x" << std::hex << id << std::dec << std::endl;
    }
    emu->set_reg(0, id);
}

void bridge_RegisterNatives(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_NewGlobalRef(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t obj = emu->get_reg(1);
    emu->set_reg(0, obj);
}

static std::unordered_map<uint32_t, std::string> g_jstrings;
static uint32_t g_next_jstring = 0x99990001;

void bridge_NewStringUTF(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_GetStringUTFChars(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
        // (used by text input system at 0x3F000000)
        emu->set_reg(0, jstr);
    } else {
        uint32_t addr = 0x30000;
        strcpy((char*)(memory + addr), "dummy_jni_string");
        emu->set_reg(0, addr);
    }
}

void bridge_AAssetManager_fromJava(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    void* mgr = AAssetManager_fromJava(NULL, NULL);
    emu->set_reg(0, register_pointer(mgr));
}

void bridge_AAssetManager_open(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    AAssetManager* mgr = (AAssetManager*)get_pointer(emu->get_reg(0));
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
    
    void* asset = AAssetManager_open(mgr, filename, emu->get_reg(2));
    std::cout << "[ASSET] Open " << filename << (asset ? " -> SUCCESS" : " -> FAILED") << std::endl;
    emu->set_reg(0, register_pointer(asset));
}



void bridge_AAsset_read(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    void* buf = (void*)(memory + emu->get_reg(1));
    uint32_t count = emu->get_reg(2);
    int read = AAsset_read(asset, buf, count);
    if (read > 0 && !emu->quiet_mode) {
        std::cout << "[Asset] READ: " << (asset ? asset->name : "NULL") << " (" << read << " bytes) to 0x" << std::hex << emu->get_reg(1) << std::dec << std::endl;
    }
    emu->set_reg(0, read);
}

void bridge_AAsset_close(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    AAsset_close(asset);
    release_pointer(asset);
}

void bridge_AAsset_getLength(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    emu->set_reg(0, (uint32_t)AAsset_getLength(asset));
}

void bridge_AAsset_openFileDescriptor(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    AAsset* asset = (AAsset*)get_pointer(emu->get_reg(0));
    off_t* outStart = (off_t*)(memory + emu->get_reg(1));
    off_t* outLength = (off_t*)(memory + emu->get_reg(2));
    int fd = AAsset_openFileDescriptor(asset, outStart, outLength);
    if (!emu->quiet_mode) {
        std::cout << "[Asset] openFileDescriptor: " << (asset ? asset->name : "NULL") << " -> fd=" << fd << std::endl;
    }
    emu->set_reg(0, fd);
}

// --- RESTORED JNI ENVIRONMENT SHIMS ---
void bridge_GetJavaVM(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t vm_ptr_ptr = emu->get_reg(1);
    *(uint32_t*)(memory + vm_ptr_ptr) = 0x11000; // Fake VM pointer
    emu->set_reg(0, 0); // JNI_OK
}

void bridge_GetEnv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t env_ptr_ptr = emu->get_reg(2);
    *(uint32_t*)(memory + env_ptr_ptr) = 0x10000; // Fake Env pointer
    emu->set_reg(0, 0); // JNI_OK
}

void bridge_ThrowNew(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_PushLocalFrame(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_PopLocalFrame(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_DeleteLocalRef(void* emu_ptr) {
}

void bridge_DeleteGlobalRef(void* emu_ptr) {
}

void bridge_NewObjectV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0x43434343);
}

void bridge_CallObjectMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
static std::string resolve_jstring(uint32_t handle, uint8_t* memory) {
    auto it = g_jstrings.find(handle);
    if (it != g_jstrings.end()) return it->second;
    // Fallback: treat as direct C-string pointer in guest memory
    if (handle > 0x1000 && handle < 0x40000000)
        return std::string((const char*)(memory + handle));
    return "";
}
int g_snapshot_load_pending_count = 0;
std::vector<uint8_t> g_snapshot_data; // Loaded save data to pass back via snapshotLoaded
bool g_snapshot_has_data = false; // Whether we have save data to return

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

void bridge_CallBooleanMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t obj = emu->get_reg(1);
    uint32_t method_id = emu->get_reg(2);
    uint32_t va_list_ptr = emu->get_reg(3);
    
    uint32_t res = 0;
    if (method_id == 0x13190001) { // loadFile
        uint32_t jstr = *(uint32_t*)(memory + va_list_ptr);
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

void bridge_CallIntMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_CallLongMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
    emu->set_reg(1, 0);
}

void bridge_CallVoidMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t obj = emu->get_reg(1);
    uint32_t method_id = emu->get_reg(2);
    uint32_t va_list_ptr = emu->get_reg(3);
    
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
        uint32_t looping = *(uint32_t*)(memory + va_list_ptr);
        g_music_looping = (looping != 0);
        if (g_music_source != 0) {
            alSourcei(g_music_source, AL_LOOPING, g_music_looping ? AL_TRUE : AL_FALSE);
        }
    } else if (method_id == 0x13240001) { // setVolume
        // ARM EABI: double in va_list must be 8-byte aligned
        // Align va_list_ptr up to next 8-byte boundary for the double
        uint32_t aligned_ptr = (va_list_ptr + 7) & ~7u;
        double vol_d;
        memcpy(&vol_d, memory + aligned_ptr, sizeof(double));
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
    }
}

void bridge_GetObjectClass(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0x44444444);
}

void bridge_GetFieldID(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_GetBooleanField(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 1);
}

void bridge_GetIntField(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_GetFloatField(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_CallStaticObjectMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_CallStaticBooleanMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
        res = 1; // Return true — we handle snapshots locally
    } else if (mid == 0x13280001) { // getBooleanFromSP(String key)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        uint32_t key_h  = *(uint32_t*)(memory + va_ptr);
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

void bridge_CallStaticIntMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    int res = 0;
    if (mid == 0x13180001) { // getPlatformConsentState — return 3 (OBTAINED)
        res = 3;
    } else if (mid == 0x13280003) { // getIntFromSP(String key)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        uint32_t key_h  = *(uint32_t*)(memory + va_ptr);
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

void bridge_CallStaticLongMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticLongMethodV(mid=0x" << std::hex << mid << ") -> 0" << std::dec << std::endl;
    }
    emu->set_reg(0, 0);
    emu->set_reg(1, 0);
}

void bridge_CallStaticFloatMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticFloatMethodV(mid=0x" << std::hex << mid << ") -> 0" << std::dec << std::endl;
    }
    emu->set_reg(0, 0);
}

void bridge_CallStaticVoidMethodV(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t mid = emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[JNI] CallStaticVoidMethodV(mid=0x" << std::hex << mid << ")" << std::dec << std::endl;
    }

    if (mid == 0x13170002) { // enteredAge
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_list_ptr = emu->get_reg(3);
        int age = *(int*)(memory + va_list_ptr);
        g_saved_age = age;
        std::cout << "[JNI] enteredAge(" << age << ") -> Saved!" << std::endl;
    } else if (mid == 0x13170007) { // receivedPrivacyConsent
        std::cout << "[JNI] receivedPrivacyConsent() -> Stubbed!" << std::endl;
    } else if (mid == 0x13250001) { // loadSnapshot
        std::string snap_path = g_save_dir + "/snapshot.bin";
        std::cout << "[SAVE] loadSnapshot() -> Posting async load for " << snap_path << std::endl;
        io_thread_post_load(snap_path, [](bool ok, std::vector<uint8_t> data) {
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
        uint32_t name_ref = *(uint32_t*)(memory + va_list_ptr);
        uint32_t data_ref = *(uint32_t*)(memory + va_list_ptr + 4);
        std::cout << "[SAVE] saveSnapshot(name=0x" << std::hex << name_ref 
                  << ", data=0x" << data_ref << ")" << std::dec << std::endl;
        
        if (data_ref != 0) {
            uint32_t array_len = *(uint32_t*)(memory + data_ref);
            uint8_t* array_data = memory + data_ref + 4;
            
            if (array_len > 0 && array_len < 0x1000000) {
                std::vector<uint8_t> data_to_save(array_data, array_data + array_len);
                std::string snap_path = g_save_dir + "/snapshot.bin";
                io_thread_post_save(snap_path, std::move(data_to_save));
                std::cout << "[SAVE] Wrote " << array_len << " bytes asynchronously via IO thread" << std::endl;
            }
        }
    } else if (mid == 0x13250003) { // deleteSnapshot
        remove((g_save_dir + "/snapshot.bin").c_str());
        std::cout << "[SAVE] Deleted snapshot" << std::endl;
    } else if (mid == 0x13270001) { // startAdsAndAnalytics
        std::cout << "[JNI] startAdsAndAnalytics() -> Stubbed!" << std::endl;
    } else if (mid == 0x13280002) { // saveBooleanInSP(String key, boolean val)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        uint32_t key_h  = *(uint32_t*)(memory + va_ptr);
        uint32_t bval   = *(uint32_t*)(memory + va_ptr + 4);
        std::string key = resolve_jstring(key_h, memory);
        pref_set(key, bval ? "true" : "false");
    } else if (mid == 0x13280004) { // saveIntInSP(String key, int val)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        uint32_t key_h  = *(uint32_t*)(memory + va_ptr);
        int ival        = *(int*)(memory + va_ptr + 4);
        std::string key = resolve_jstring(key_h, memory);
        pref_set(key, std::to_string(ival));
    } else if (mid == 0x13280006) { // saveLongInSP(String key, long val)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        uint32_t key_h  = *(uint32_t*)(memory + va_ptr);
        // Long in ARM soft-float va_list: low word then high word
        uint32_t lo = *(uint32_t*)(memory + va_ptr + 4);
        uint32_t hi = *(uint32_t*)(memory + va_ptr + 8);
        int64_t lval = (int64_t)((uint64_t)hi << 32 | lo);
        std::string key = resolve_jstring(key_h, memory);
        pref_set(key, std::to_string(lval));
    } else if (mid == 0x13290001) { // startTextInput(String initialText)
        uint8_t* memory = emu->get_memory_base();
        uint32_t va_ptr = emu->get_reg(3);
        uint32_t str_h = *(uint32_t*)(memory + va_ptr);
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

void bridge_GetStaticFieldID(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_GetStaticObjectField(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_GetStringUTFLength(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t jstr = emu->get_reg(1);
    const char* str = (const char*)(memory + jstr);
    emu->set_reg(0, std::strlen(str));
}

void bridge_ReleaseStringUTFChars(void* emu_ptr) {
}

void bridge_GetArrayLength(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t array = emu->get_reg(1);
    if (array) {
        uint8_t* mem = emu->get_memory_base();
        uint32_t len = *(uint32_t*)(mem + array);
        emu->set_reg(0, len);
    } else {
        emu->set_reg(0, 0);
    }
}

void bridge_GetObjectArrayElement(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_NewIntArray(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

// Bump allocator for byte arrays — short-lived (created, filled, passed to save, discarded)
static uint32_t g_byte_array_alloc_ptr = 0x30000000; // High region for arrays

void bridge_NewByteArray(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t length = emu->get_reg(1); // R1 = size
    
    if (length == 0 || length > 0x1000000) { // Sanity check (max 16MB)
        std::cout << "[JNI] NewByteArray(" << length << ") -> 0 (invalid size)" << std::endl;
        emu->set_reg(0, 0);
        return;
    }

    // Layout: [uint32_t length][byte data...] — matches GetByteArrayElements
    uint32_t array_addr = g_byte_array_alloc_ptr;
    uint8_t* memory = emu->get_memory_base();
    *(uint32_t*)(memory + array_addr) = length;
    // Zero-initialize the data region
    memset(memory + array_addr + 4, 0, length);
    // Bump allocator forward (align to 4 bytes)
    g_byte_array_alloc_ptr += (4 + length + 3) & ~3U;
    // Wrap around if we go too far
    if (g_byte_array_alloc_ptr > 0x32000000) {
        g_byte_array_alloc_ptr = 0x30000000;
    }

    std::cout << "[JNI] NewByteArray(" << length << ") -> 0x" << std::hex << array_addr << std::dec << std::endl;
    emu->set_reg(0, array_addr);
}

void bridge_GetByteArrayElements(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t array = emu->get_reg(1);
    if (array) {
        // Array layout: [uint32_t length][byte data...] — return pointer past length
        uint32_t data_ptr = array + 4;
        std::cout << "[JNI] GetByteArrayElements(array=0x" << std::hex << array
                  << ") -> 0x" << data_ptr << std::dec << std::endl;
        emu->set_reg(0, data_ptr);
    } else {
        std::cout << "[JNI] GetByteArrayElements(null) -> 0" << std::endl;
        emu->set_reg(0, 0);
    }
}

void bridge_ReleaseByteArrayElements(void* emu_ptr) {
    // No-op — nothing to release
}

// SetByteArrayRegion(JNIEnv*, jbyteArray array, jsize start, jsize len, const jbyte* buf)
void bridge_SetByteArrayRegion(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t array = emu->get_reg(1);   // R1 = array
    uint32_t start = emu->get_reg(2);   // R2 = start offset
    uint32_t len = emu->get_reg(3);     // R3 = length
    // buf is on the stack (5th arg) for ARM32 calling convention
    uint32_t sp = emu->get_reg(13);
    uint32_t buf = *(uint32_t*)(memory + sp);
    
    if (array && buf && len > 0 && len < 0x1000000) {
        // Array layout: [uint32_t length][byte data...]
        uint8_t* dst = memory + array + 4 + start;
        uint8_t* src = memory + buf;
        memcpy(dst, src, len);
        std::cout << "[JNI] SetByteArrayRegion(array=0x" << std::hex << array 
                  << ", start=" << std::dec << start << ", len=" << len << ")" << std::endl;
    }
}

void bridge_GetIntArrayElements(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}

void bridge_ReleaseIntArrayElements(void* emu_ptr) {
}

void bridge_SetIntArrayRegion(void* emu_ptr) {
}

void bridge_GetStringUTFRegion(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t jstr = emu->get_reg(1);
    uint32_t start = emu->get_reg(2);
    uint32_t len = emu->get_reg(3);
    uint32_t sp = emu->get_reg(13);
    uint32_t buf = *(uint32_t*)(memory + sp);
    const char* str = (const char*)(memory + jstr);
    std::memcpy(memory + buf, str + start, len);
    memory[buf + len] = '\0';
}

// --- Additional C String Bridges ---
void bridge_strchr(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_strrchr(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_strcpy(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    std::strcpy((char*)(memory + dest), (const char*)(memory + src));
    emu->set_reg(0, dest);
}

void bridge_strncpy(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    std::strncpy((char*)(memory + dest), (const char*)(memory + src), n);
    emu->set_reg(0, dest);
}

void bridge_strcmp(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s1 = emu->get_reg(0);
    uint32_t s2 = emu->get_reg(1);
    int result = std::strcmp((const char*)(memory + s1), (const char*)(memory + s2));
    emu->set_reg(0, (uint32_t)result);
}

void bridge_strncmp(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s1 = emu->get_reg(0);
    uint32_t s2 = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    int result = std::strncmp((const char*)(memory + s1), (const char*)(memory + s2), n);
    emu->set_reg(0, (uint32_t)result);
}

void bridge_strcat(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    std::strcat((char*)(memory + dest), (const char*)(memory + src));
    emu->set_reg(0, dest);
}

void bridge_strncat(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dest = emu->get_reg(0);
    uint32_t src = emu->get_reg(1);
    uint32_t n = emu->get_reg(2);
    std::strncat((char*)(memory + dest), (const char*)(memory + src), (size_t)n);
    emu->set_reg(0, dest);
}

void bridge_strcspn(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t s = emu->get_reg(0);
    uint32_t reject = emu->get_reg(1);
    size_t result = std::strcspn((const char*)(memory + s), (const char*)(memory + reject));
    emu->set_reg(0, (uint32_t)result);
}

void bridge_strstr(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_memchr_impl(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t ptr = emu->get_reg(0);
    int c = (int)(emu->get_reg(1) & 0xFF);
    uint32_t n = emu->get_reg(2);
    if (!emu->quiet_mode) {
        std::cout << "[MEM] memchr(ptr=0x" << std::hex << ptr << ", c=" << c << ", n=" << std::dec << n << ")" << std::endl;
    }
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

void bridge_strtol(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    int base = (int)emu->get_reg(2);
    char* endptr = nullptr;
    long result = std::strtol((const char*)(memory + str), &endptr, base);
    if (endptr_ptr && endptr) {
        uint32_t offset = (uint32_t)(endptr - (char*)(memory + str));
        *(uint32_t*)(memory + endptr_ptr) = str + offset;
    }
    emu->set_reg(0, (uint32_t)result);
}

void bridge_strtoul(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    uint32_t endptr_ptr = emu->get_reg(1);
    int base = (int)emu->get_reg(2);
    char* endptr = nullptr;
    unsigned long result = std::strtoul((const char*)(memory + str), &endptr, base);
    if (endptr_ptr && endptr) {
        uint32_t offset = (uint32_t)(endptr - (char*)(memory + str));
        *(uint32_t*)(memory + endptr_ptr) = str + offset;
    }
    emu->set_reg(0, (uint32_t)result);
}

void bridge_atoi(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str = emu->get_reg(0);
    emu->set_reg(0, (uint32_t)std::atoi((const char*)(memory + str)));
}

void bridge_atof(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    const char* str = (const char*)(memory + str_ptr);
    float res = (float)std::atof(str);
    if (std::isnan(res)) std::cout << "[Math] atof(\"" << str << "\") -> NaN!" << std::endl;
    if (!emu->quiet_mode) std::cout << "[Math] atof(\"" << str << "\") -> " << res << std::endl;
    emu->set_reg(0, float_to_uint(res));
}

// --- Wide Character / CType Bridges ---
#include <cwchar>
void bridge_wctob(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    wint_t wc = (wint_t)emu->get_reg(0);
    emu->set_reg(0, (uint32_t)std::wctob(wc));
}

void bridge_btowc(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    int c = (int)emu->get_reg(0);
    emu->set_reg(0, (uint32_t)std::btowc(c));
}

void bridge_ctype_cur_max(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 1); // Return 1 for MB_CUR_MAX (simple ASCII/UTF-8 single byte for now)
}

#include <zlib.h>

// --- File I/O Bridges ---
static std::unordered_map<uint32_t, FILE*> g_file_handles;
static std::unordered_map<uint32_t, gzFile> g_gz_handles;
static uint32_t g_next_file_handle = 0x70000001;

void bridge_lseek(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gzdopen(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    int fd = emu->get_reg(0);
    uint32_t mode_ptr = emu->get_reg(1);
    const char* mode = (const char*)(memory + mode_ptr);
    
    std::cout << "[ZLIB] gzdopen(fd=" << fd << ", mode=\"" << mode << "\")";

    int real_fd = fd;
    if (g_file_handles.count(fd)) {
        real_fd = fileno(g_file_handles[fd]);
    }
    
    // Always dup so gzclose doesn't close our original asset/file handle
    int new_fd = dup(real_fd);
    gzFile gz = gzdopen(new_fd, mode);

    if (gz) {
        uint32_t handle = g_next_file_handle++;
        g_gz_handles[handle] = gz;
        std::cout << " -> handle 0x" << std::hex << handle << std::dec << std::endl;
        emu->set_reg(0, handle);
    } else {
        std::cout << " -> FAILED" << std::endl;
        if (new_fd >= 0) close(new_fd);
        emu->set_reg(0, 0);
    }
}

void bridge_gzread(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t handle = emu->get_reg(0);
    uint32_t buf = emu->get_reg(1);
    uint32_t len = emu->get_reg(2);
    
    if (g_gz_handles.count(handle)) {
        int read = gzread(g_gz_handles[handle], memory + buf, len);
        if (!emu->quiet_mode) {
             std::cout << "[ZLIB] gzread(handle=0x" << std::hex << handle << ", len=" << std::dec << len << ") -> " << read << " bytes";
             if (read < 0) {
                 int errnum;
                 const char* errmsg = gzerror(g_gz_handles[handle], &errnum);
                 std::cout << " ERROR: " << errmsg << " (" << errnum << ")";
             }
             std::cout << std::endl;
        }
        emu->set_reg(0, (uint32_t)read);
    } else {
        std::cout << "[ZLIB] gzread(INVALID handle=0x" << std::hex << handle << ")" << std::endl;
        emu->set_reg(0, (uint32_t)-1);
    }
}

void bridge_gzclose(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (g_gz_handles.count(handle)) {
        gzclose(g_gz_handles[handle]);
        g_gz_handles.erase(handle);
        emu->set_reg(0, 0);
    } else {
        emu->set_reg(0, (uint32_t)-1);
    }
}

void bridge_fopen(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t path_ptr = emu->get_reg(0);
    uint32_t mode_ptr = emu->get_reg(1);
    const char* path = (const char*)(memory + path_ptr);
    const char* mode = (const char*)(memory + mode_ptr);
    
    // Always log write-mode opens for save debugging
    bool is_write = (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
    if (is_write) {
        // std::cout << "[File] fopen(\"" << path << "\", \"" << mode << "\") [WRITE]" << std::endl;
        try {
            fs::create_directories(fs::path(path).parent_path());
        } catch (...) {
            // Ignore failure
        }
    } else if (!emu->quiet_mode) {
        // std::cout << "[File] fopen(\"" << path << "\", \"" << mode << "\")" << std::endl;
    }
    
    FILE* f = fopen(path, mode);
    if (f) {
        uint32_t handle = g_next_file_handle++;
        g_file_handles[handle] = f;
        emu->set_reg(0, handle);
        if (is_write) {
            // std::cout << "[File]   -> OK (handle=" << handle << ")" << std::endl;
        }
    } else {
        emu->set_reg(0, 0);
        if (is_write) {
            // std::cout << "[File]   -> FAILED: " << strerror(errno) << " (errno=" << errno << ")" << std::endl;
        }
    }
}

void bridge_fclose(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (g_file_handles.count(handle)) {
        fflush(g_file_handles[handle]);  // Ensure data written to disk before close
        fclose(g_file_handles[handle]);
        g_file_handles.erase(handle);
        emu->set_reg(0, 0);
    } else {
        emu->set_reg(0, -1);
    }
}

void bridge_fflush(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_fread(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf = emu->get_reg(0);
    uint32_t elem_size = emu->get_reg(1);
    uint32_t count = emu->get_reg(2);
    uint32_t handle = emu->get_reg(3);
    if (g_file_handles.count(handle)) {
        size_t read = fread(memory + buf, elem_size, count, g_file_handles[handle]);
        emu->set_reg(0, (uint32_t)read);
    } else {
        emu->set_reg(0, 0);
    }
}

void bridge_fwrite(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
        emu->set_reg(0, 0);
        // std::cout << "[File] fwrite(handle=" << handle << ") -> INVALID HANDLE" << std::endl;
    }
}

void bridge_fseek(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    int32_t offset = (int32_t)emu->get_reg(1);
    int whence = (int)emu->get_reg(2);
    if (g_file_handles.count(handle)) {
        emu->set_reg(0, (uint32_t)fseek(g_file_handles[handle], offset, whence));
    } else {
        emu->set_reg(0, (uint32_t)-1);
    }
}

void bridge_ftell(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t handle = emu->get_reg(0);
    if (g_file_handles.count(handle)) {
        emu->set_reg(0, (uint32_t)ftell(g_file_handles[handle]));
    } else {
        emu->set_reg(0, (uint32_t)-1);
    }
}

// --- Directory / System Bridges ---
void bridge_mkdir(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t path_ptr = emu->get_reg(0);
    uint32_t mode = emu->get_reg(1);
    const char* path = (const char*)(memory + path_ptr);
    // std::cout << "[File] mkdir(\"" << path << "\", " << std::oct << mode << std::dec << ")" << std::endl;
    int result = mkdir(path, (mode_t)mode);
    if (result != 0 && errno == EEXIST) {
        // Directory already exists — treat as success (game does recursive mkdir)
        result = 0;
    }
    if (result != 0) {
        // std::cout << "[File]   mkdir FAILED: " << strerror(errno) << std::endl;
    }
    emu->set_reg(0, (uint32_t)result);
}

void bridge_rename(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* old_path = (const char*)(memory + emu->get_reg(0));
    const char* new_path = (const char*)(memory + emu->get_reg(1));
    int result = rename(old_path, new_path);
    if (result != 0) {
        std::cerr << "[File] rename(\"" << old_path << "\" -> \"" << new_path << "\") FAILED: " << strerror(errno) << std::endl;
    } else {
        std::cout << "[File] rename(\"" << old_path << "\" -> \"" << new_path << "\") OK" << std::endl;
    }
    emu->set_reg(0, (uint32_t)result);
}

void bridge_remove(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* path = (const char*)(memory + emu->get_reg(0));
    int result = remove(path);
    if (result != 0 && errno != ENOENT) {
        std::cerr << "[File] remove(\"" << path << "\") FAILED: " << strerror(errno) << std::endl;
    }
    emu->set_reg(0, (uint32_t)result);
}

void bridge_access(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* path = (const char*)(memory + emu->get_reg(0));
    int mode = (int)emu->get_reg(1);
    int result = access(path, mode);
    emu->set_reg(0, (uint32_t)result);
}

void bridge_unlink(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    const char* path = (const char*)(memory + emu->get_reg(0));
    int result = unlink(path);
    emu->set_reg(0, (uint32_t)result);
}

void bridge_abort(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    std::cerr << "[FATAL] guest called abort() at PC=0x" << std::hex << emu->get_pc() << std::dec << std::endl;
    exit(1);
}

void bridge_localtime(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t timer_ptr = emu->get_reg(0);
    time_t timer;
    if (timer_ptr) timer = (time_t)*(uint32_t*)(memory + timer_ptr);
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

void bridge_clock(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    clock_t res = clock();
    emu->set_reg(0, (uint32_t)res);
}

void bridge_lrand48(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    long int res = lrand48();
    emu->set_reg(0, (uint32_t)res);
}

void bridge_fputs(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t str_ptr = emu->get_reg(0);
    uint32_t handle = emu->get_reg(1);
    const char* s = (const char*)(memory + str_ptr);
    
    // Check if handle matches __sF (stdout/stderr)
    // In our loader, __sF is at globals_base (0x50000)
    // __sF[0] = stdin, __sF[1] = stdout, __sF[2] = stderr
    // Each FILE struct is usually ~84-148 bytes, but we just check the pointer.
    if (handle == 0x50000 + 0 || handle == 0x50000 + 1*128 || handle == 0x50000 + 2*128) {
         std::cout << "[guest log] " << s;
         emu->set_reg(0, 0);
         return;
    }

    if (g_file_handles.count(handle)) {
        int res = fputs(s, g_file_handles[handle]);
        emu->set_reg(0, res >= 0 ? 0 : -1);
    } else {
        // Fallback: if it looks like a low handle, it might be raw fd or someone's stdout
        if (handle < 10) {
            std::cout << "[guest log fd=" << handle << "] " << s;
            emu->set_reg(0, 0);
        } else {
            emu->set_reg(0, -1);
        }
    }
}

void bridge_exidx(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_stat(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t path_ptr = emu->get_reg(0);
    uint32_t stat_buf_ptr = emu->get_reg(1);
    const char* path = (const char*)(memory + path_ptr);
    
#ifdef _WIN32
    struct __stat64 st;
    int result = _stat64(path, &st);
#else
    struct stat st;
    int result = stat(path, &st);
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
        // std::cout << "[File] stat(\"" << path << "\") -> " << result 
        //           << " (mode: 0x" << std::hex << st.st_mode << std::dec 
        //           << ", size: " << st.st_size << ")" << std::endl;
    }
    emu->set_reg(0, (uint32_t)result);
}

struct GuestDir {
    std::vector<std::pair<std::string, uint8_t>> entries;
    size_t index = 0;
    uint32_t guest_dirent_addr;
};

void bridge_opendir(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t name_ptr = emu->get_reg(0);
    
    if (name_ptr == 0) {
        emu->set_reg(0, 0); // null
        return;
    }
    
    const char* path = (const char*)(memory + name_ptr);
    if (!emu->quiet_mode) {
        // std::cout << "[File] opendir(\"" << path << "\")" << std::endl;
    }
    
    if (!fs::exists(path) || !fs::is_directory(path)) {
        emu->set_reg(0, 0); // null
        return;
    }
    
    GuestDir* gd = new GuestDir();
    gd->guest_dirent_addr = g_guest_heap_ptr;
    g_guest_heap_ptr += (280 + 7) & ~7; // allocate 280 bytes in guest heap for dirent struct
    
    for (const auto& entry : fs::directory_iterator(path)) {
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

void bridge_readdir(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_closedir(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

// --- printf / logging ---
void bridge_printf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t fmt_ptr = emu->get_reg(0);
    const char* fmt = (const char*)(memory + fmt_ptr);
    std::cout << "[guest printf] " << fmt;
    emu->set_reg(0, 0);
}

void bridge_android_log_print(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    // int __android_log_print(int prio, const char* tag, const char* fmt, ...)
    uint32_t tag_ptr = emu->get_reg(1);
    uint32_t fmt_ptr = emu->get_reg(2);
    const char* tag = (const char*)(memory + tag_ptr);
    const char* fmt = (const char*)(memory + fmt_ptr);
    std::cout << "[ALOG] " << tag << ": " << fmt << std::endl;
    emu->set_reg(0, 0);
}

void bridge_snprintf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf = emu->get_reg(0);
    uint32_t size = emu->get_reg(1);
    uint32_t fmt_ptr = emu->get_reg(2);
    const char* fmt = (const char*)(memory + fmt_ptr);
    // Simple: just copy the format string as-is (no varargs handling)
    std::strncpy((char*)(memory + buf), fmt, size);
    if (size > 0) memory[buf + size - 1] = '\0';
    emu->set_reg(0, (uint32_t)std::strlen((char*)(memory + buf)));
}

void bridge_sprintf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buf = emu->get_reg(0);
    uint32_t fmt_ptr = emu->get_reg(1);
    const char* fmt = (const char*)(memory + fmt_ptr);
    // Simple: just copy the format string as-is
    std::strcpy((char*)(memory + buf), fmt);
    emu->set_reg(0, (uint32_t)std::strlen((char*)(memory + buf)));
}

// --- OpenAL Stubs (Now Real wrappers) ---
static ALCdevice* g_alc_device = nullptr;
static ALCcontext* g_alc_context = nullptr;

void bridge_alcOpenDevice(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_alc_device = alcOpenDevice(nullptr);
    emu->set_reg(0, g_alc_device ? 1 : 0);
}

void bridge_alcCreateContext(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_alc_context = alcCreateContext(g_alc_device, nullptr);
    emu->set_reg(0, g_alc_context ? 2 : 0);
}

void bridge_alcMakeContextCurrent(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    ALCboolean res = alcMakeContextCurrent(g_alc_context);
    emu->set_reg(0, res ? 1 : 0);
}

void bridge_alGetError(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, alGetError());
}

void bridge_alGenSources(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t sources_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_sources(n);
    alGenSources(n, host_sources.data());
    std::memcpy(memory + sources_ptr, host_sources.data(), n * sizeof(ALuint));
}

void bridge_alGenBuffers(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t buffers_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_buffers(n);
    alGenBuffers(n, host_buffers.data());
    std::memcpy(memory + buffers_ptr, host_buffers.data(), n * sizeof(ALuint));
}

void bridge_alSourcePlay(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourcePlay(source);
}

void bridge_alSourceStop(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourceStop(source);
}

void bridge_alSourcePause(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    alSourcePause(source);
}

void bridge_alSourcei(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t value = emu->get_reg(2);
    alSourcei(source, param, value);
}

void bridge_alSourcef(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t val_i = emu->get_reg(2);
    float val_f;
    std::memcpy(&val_f, &val_i, 4);
    alSourcef(source, param, val_f);
}

void bridge_alSource3f(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t v1_i = emu->get_reg(2);
    uint32_t v2_i = emu->get_reg(3);
    uint32_t v3_i = *(uint32_t*)(memory + emu->get_reg(13)); // SP
    float v1, v2, v3;
    std::memcpy(&v1, &v1_i, 4);
    std::memcpy(&v2, &v2_i, 4);
    std::memcpy(&v3, &v3_i, 4);
    alSource3f(source, param, v1, v2, v3);
}

void bridge_alBufferData(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t buffer = emu->get_reg(0);
    uint32_t format = emu->get_reg(1);
    uint32_t data_ptr = emu->get_reg(2);
    uint32_t size = emu->get_reg(3);
    uint32_t freq = *(uint32_t*)(memory + emu->get_reg(13)); // SP
    
    alBufferData(buffer, format, memory + data_ptr, size, freq);
}

void bridge_alSourceQueueBuffers(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t nb = emu->get_reg(1);
    uint32_t buffers_ptr = emu->get_reg(2);
    
    std::vector<ALuint> host_buffers(nb);
    std::memcpy(host_buffers.data(), memory + buffers_ptr, nb * sizeof(ALuint));
    alSourceQueueBuffers(source, nb, host_buffers.data());
}

void bridge_alSourceUnqueueBuffers(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t nb = emu->get_reg(1);
    uint32_t buffers_ptr = emu->get_reg(2);
    
    std::vector<ALuint> host_buffers(nb);
    alSourceUnqueueBuffers(source, nb, host_buffers.data());
    std::memcpy(memory + buffers_ptr, host_buffers.data(), nb * sizeof(ALuint));
}

void bridge_alGetSourcei(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t source = emu->get_reg(0);
    uint32_t param = emu->get_reg(1);
    uint32_t value_ptr = emu->get_reg(2);
    
    ALint host_val = 0;
    alGetSourcei(source, param, &host_val);
    *(ALint*)(memory + value_ptr) = host_val;
}

void bridge_alDeleteSources(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t sources_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_sources(n);
    std::memcpy(host_sources.data(), memory + sources_ptr, n * sizeof(ALuint));
    alDeleteSources(n, host_sources.data());
}

void bridge_alDeleteBuffers(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t buffers_ptr = emu->get_reg(1);
    
    std::vector<ALuint> host_buffers(n);
    std::memcpy(host_buffers.data(), memory + buffers_ptr, n * sizeof(ALuint));
    alDeleteBuffers(n, host_buffers.data());
}

void bridge_alListenerf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t param = emu->get_reg(0);
    uint32_t val_i = emu->get_reg(1);
    float val_f;
    std::memcpy(&val_f, &val_i, 4);
    alListenerf(param, val_f);
}

void bridge_alListener3f(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t param = emu->get_reg(0);
    uint32_t v1_i = emu->get_reg(1);
    uint32_t v2_i = emu->get_reg(2);
    uint32_t v3_i = emu->get_reg(3);
    float v1, v2, v3;
    std::memcpy(&v1, &v1_i, 4);
    std::memcpy(&v2, &v2_i, 4);
    std::memcpy(&v3, &v3_i, 4);
    alListener3f(param, v1, v2, v3);
}

void bridge_alDistanceModel(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t distanceModel = emu->get_reg(0);
    alDistanceModel(distanceModel);
}

void bridge_al_noop(void* emu_ptr) {
    // Generic no-op
}

// --- GLES Real Bridge Functions ---
// When g_display_active, these call real host OpenGL.
// Guest memory pointers are translated via (memory + guest_offset).

// Diagnostic: log GL calls on first 3 frames to debug black screen
int g_gl_diag_frame = 0;
bool g_gl_diag_enabled = true;
#define GL_DIAG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[GL F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define TEX_LOG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[TEX F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define VERTEX_LOG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[VERTEX F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define MATRIX_LOG(...) do { if (g_gl_diag_enabled && g_gl_diag_frame < 3) { printf("[MATRIX F%d] ", g_gl_diag_frame); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define EGL_LOG(...) do { printf("[EGL] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)

static std::unordered_set<uint32_t> g_matrix_dumped_ptrs;

static void log_matrix_once(Emulator* emu, uint32_t ptr, const GLfloat* m) {
    if (!g_gl_diag_enabled || g_gl_diag_frame >= 3) return;
    if (g_matrix_dumped_ptrs.count(ptr)) return;
    g_matrix_dumped_ptrs.insert(ptr);
    uint8_t* memory = emu->get_memory_base();
    MATRIX_LOG("glLoadMatrixf ptr=0x%x host=%p sp=0x%x", ptr, (void*)(memory + ptr), emu->get_reg(13));
    MATRIX_LOG("  row0 [%.4f %.4f %.4f %.4f]", m[0], m[1], m[2], m[3]);
    MATRIX_LOG("  row1 [%.4f %.4f %.4f %.4f]", m[4], m[5], m[6], m[7]);
    MATRIX_LOG("  row2 [%.4f %.4f %.4f %.4f]", m[8], m[9], m[10], m[11]);
    MATRIX_LOG("  row3 [%.4f %.4f %.4f %.4f]", m[12], m[13], m[14], m[15]);
}

static void log_vertex_sample(Emulator* emu, uint32_t ptr, uint32_t size, uint32_t type, uint32_t stride, const char* label) {
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

void bridge_gl_noop(void* emu_ptr) {
    g_frame_stats.state_changes++;
}

// --- Fog (water color, atmosphere, depth haze) ---
void bridge_glFogf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLenum pname = (GLenum)emu->get_reg(0);
    uint32_t param_bits = emu->get_reg(1);
    float param;
    memcpy(&param, &param_bits, 4);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.fogf(pname, param);
        return;
    }
#endif
    if (g_display_active) glFogf(pname, param);
}

void bridge_glFogfv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glFogi(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
void bridge_glLightf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLenum light = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t param_bits = emu->get_reg(2);
    float param;
    memcpy(&param, &param_bits, 4);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.lightf(light, pname, param);
        return;
    }
#endif
    if (g_display_active) glLightf(light, pname, param);
}

void bridge_glLightfv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum light = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t params_ptr = emu->get_reg(2);
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
    }
}

void bridge_glLightModelf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLenum pname = (GLenum)emu->get_reg(0);
    uint32_t param_bits = emu->get_reg(1);
    float param;
    memcpy(&param, &param_bits, 4);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.light_modelf(pname, param);
        return;
    }
#endif
    if (g_display_active) glLightModelf(pname, param);
}

void bridge_glLightModelfv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
    }
}

// --- Materials ---
void bridge_glMaterialf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLenum face = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t param_bits = emu->get_reg(2);
    float param;
    memcpy(&param, &param_bits, 4);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.materialf(face, pname, param);
        return;
    }
#endif
    if (g_display_active) glMaterialf(face, pname, param);
}

void bridge_glMaterialfv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    GLenum face = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    uint32_t params_ptr = emu->get_reg(2);
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
    }
}

// --- Vertex Color ---
void bridge_glColor4ub(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
}

// --- Texture Environment ---
void bridge_glTexEnvi(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLenum target = (GLenum)emu->get_reg(0);
    GLenum pname = (GLenum)emu->get_reg(1);
    GLint param = (GLint)emu->get_reg(2);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.tex_envi(target, pname, param);
        return;
    }
#endif
    if (g_display_active) glTexEnvi(target, pname, param);
}

void bridge_glTexEnvfv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
void bridge_glCullFace(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLenum mode = (GLenum)emu->get_reg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.cull_face(mode);
        return;
    }
#endif
    if (g_display_active) glCullFace(mode);
}

void bridge_glHint(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glPointSize(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t size_bits = emu->get_reg(0);
    float size;
    memcpy(&size, &size_bits, 4);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.point_size(size);
        return;
    }
#endif
    if (g_display_active) glPointSize(size);
}

// --- Stencil (needed for shadows/water reflections) ---
void bridge_glStencilFunc(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glStencilMask(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLuint mask = (GLuint)emu->get_reg(0);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.stencil_mask(mask);
        return;
    }
#endif
    if (g_display_active) glStencilMask(mask);
}

void bridge_glStencilOp(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glClearStencil(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GLint s = (GLint)emu->get_reg(0);
    if (g_display_active) glClearStencil(s);
}


// --- Phase 1: Clear/Viewport ---

void bridge_gl_clear(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.clear_calls++;
    uint32_t mask = emu->get_reg(0);
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

void bridge_gl_clear_color(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    uint32_t r = emu->get_reg(0), g = emu->get_reg(1);
    uint32_t b = emu->get_reg(2), a = emu->get_reg(3);
    float fr, fg, fb, fa;
    memcpy(&fr, &r, 4); memcpy(&fg, &g, 4);
    memcpy(&fb, &b, 4); memcpy(&fa, &a, 4);
    
    if (g_void_fill_color.active && fr < 0.01f && fg < 0.01f && fb < 0.01f) {
        fr = g_void_fill_color.r;
        fg = g_void_fill_color.g;
        fb = g_void_fill_color.b;
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
int g_win_w = 1920;   // Logical window size (for mouse coordinate mapping)
int g_win_h = 1080;
int g_draw_w = 1920;  // Physical drawable size in pixels (for glViewport / FBO blit)
int g_draw_h = 1080;

void bridge_gl_viewport(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.viewport_calls++;
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

// Track current matrix mode so we can apply correct fallback for zero matrices
static GLenum g_current_matrix_mode = GL_MODELVIEW;

void bridge_gl_matrix_mode(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gl_load_identity(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    GL_DIAG("glLoadIdentity()");
    g_frame_stats.matrix_ops++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.load_identity();
        return;
    }
#endif
    if (g_display_active) glLoadIdentity();
}

void bridge_gl_load_matrixf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
                glOrtho(0, 800, 480, 0, -1, 1);  // Game's 2D UI space
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
        ++nan_count;
        if (nan_count <= 2) {
            uint32_t raw0;
            memcpy(&raw0, &m[0], 4);
            std::cerr << "[MATRIX] NaN/Inf #" << nan_count << " at LR=0x" << std::hex << lr 
                      << " ptr=0x" << ptr << " raw[0]=0x" << raw0 << std::dec << std::endl;
            // Dump raw hex of all 16 floats
            const uint32_t* raw = (const uint32_t*)m;
            std::cerr << "  raw: ";
            for (int i = 0; i < 16; i++) std::cerr << std::hex << raw[i] << " ";
            std::cerr << std::dec << std::endl;
            
            // Dump all VFP S-registers via get_vfp_reg()
            std::cerr << "  VFP: ";
            for (int i = 0; i < 16; i++) {
                float fval = emu->get_vfp_reg(i);
                if (std::isnan(fval)) std::cerr << "S" << i << "=NaN ";
                else if (std::isinf(fval)) std::cerr << "S" << i << "=Inf ";
                else if (fval != 0.0f) std::cerr << "S" << i << "=" << fval << " ";
            }
            std::cerr << std::endl;
            
            // Scan 64 bytes BEFORE the matrix on stack for NaN source data
            std::cerr << "  Stack[-64..0]: ";
            for (int i = -16; i < 0; i++) {
                uint32_t v = *(uint32_t*)(memory + ptr + i * 4);
                float f;
                memcpy(&f, &v, 4);
                if (std::isnan(f)) std::cerr << "[" << (i*4) << "]=NaN ";
                else if (v != 0) std::cerr << "[" << (i*4) << "]=0x" << std::hex << v << std::dec << " ";
            }
            std::cerr << std::endl;
            
            // Dump the 8 instructions BEFORE the BL to glLoadMatrixf
            uint32_t call_site = (lr & ~1) - 4; // BL instruction
            std::cerr << "  Code before call (0x" << std::hex << (call_site - 16) << "):" << std::dec;
            for (int i = -8; i <= 0; i++) {
                uint16_t insn = *(uint16_t*)(memory + call_site + i * 2);
                std::cerr << " " << std::hex << insn;
            }
            std::cerr << std::dec << std::endl;
        } else if (nan_count % 1000 == 0) {
            std::cout << "[MATRIX] NaN/Inf #" << nan_count << " at LR=0x" << std::hex << lr << std::dec
                      << " [" << m[0] << " " << m[1] << " / " << m[4] << " " << m[5] << "]" << std::endl;
        }
        // Instead of replacing with identity (puts objects at origin = invisible),
        // sanitize each NaN/Inf element to the corresponding identity value.
        // This preserves valid translation/position data while fixing the rotation
        // NaN caused by Unicorn VFP bug in the BLX at 0x122228c.
        if (g_display_active) {
            if (g_current_matrix_mode == GL_PROJECTION) {
                glOrtho(0, 800, 480, 0, -1, 1);
            } else {
                // Identity matrix values: 1 at diagonal (0,5,10,15), 0 elsewhere
                static const GLfloat identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                GLfloat fixed[16];
                bool any_valid = false;
                for (int i = 0; i < 16; i++) {
                    if (std::isnan(m[i]) || std::isinf(m[i])) {
                        fixed[i] = identity[i];
                    } else {
                        fixed[i] = m[i];
                        any_valid = true;
                    }
                }
                glLoadMatrixf(fixed);
            }
        }
        return;
    }

    log_matrix_once(emu, ptr, m);
    GL_DIAG("glLoadMatrixf(@0x%x) [%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / ...]", ptr, m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7]);
    if (g_display_active) {
        glLoadMatrixf(m);
    }
}

void bridge_gl_mult_matrixf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

    if (g_display_active) {
        glMultMatrixf(m);
    }
}

static int g_gl_matrix_stack_depth = 0;

void bridge_gl_push_matrix(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gl_pop_matrix(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_gl_matrix_stack_depth--;
    GL_DIAG("glPopMatrix() depth=%d", g_gl_matrix_stack_depth);
    g_frame_stats.matrix_ops++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.pop_matrix();
        return;
    }
#endif
    if (g_display_active) glPopMatrix();
}

void bridge_gl_orthof(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.matrix_ops++;
    // Soft-float: 6 floats in R0-R3 + stack
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t r2 = emu->get_reg(2), r3 = emu->get_reg(3);
    float l, r_, b, t;
    memcpy(&l, &r0, 4); memcpy(&r_, &r1, 4);
    memcpy(&b, &r2, 4); memcpy(&t, &r3, 4);
    // near/far on stack
    uint8_t* memory = emu->get_memory_base();
    uint32_t sp = emu->get_reg(13);
    float n, f;
    memcpy(&n, memory + sp, 4);
    memcpy(&f, memory + sp + 4, 4);
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
}


void bridge_gl_translatef(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.matrix_ops++;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1), r2 = emu->get_reg(2);
    float x, y, z;
    memcpy(&x, &r0, 4); memcpy(&y, &r1, 4); memcpy(&z, &r2, 4);
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

void bridge_gl_rotatef(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.matrix_ops++;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t r2 = emu->get_reg(2), r3 = emu->get_reg(3);
    float angle, x, y, z;
    memcpy(&angle, &r0, 4); memcpy(&x, &r1, 4);
    memcpy(&y, &r2, 4); memcpy(&z, &r3, 4);
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

void bridge_gl_scalef(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.matrix_ops++;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1), r2 = emu->get_reg(2);
    float x, y, z;
    memcpy(&x, &r0, 4); memcpy(&y, &r1, 4); memcpy(&z, &r2, 4);
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

void bridge_gl_frustumf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.matrix_ops++;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t r2 = emu->get_reg(2), r3 = emu->get_reg(3);
    float l, r_, b, t;
    memcpy(&l, &r0, 4); memcpy(&r_, &r1, 4);
    memcpy(&b, &r2, 4); memcpy(&t, &r3, 4);
    uint8_t* memory = emu->get_memory_base();
    uint32_t sp = emu->get_reg(13);
    float n, f;
    memcpy(&n, memory + sp, 4);
    memcpy(&f, memory + sp + 4, 4);
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.frustumf(l, r_, b, t, n, f);
        return;
    }
#endif
    if (g_display_active) {
        glFrustum(l, r_, b, t, n, f);
    }
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

bool g_gl_force_white = false;
bool g_gl_force_identity = false;
bool g_gl_hide_hud = false;  // When true, swap controls atlas with modified copy
static GLuint g_controls_atlas_tex_id = 0;    // Original atlas texture ID
static GLuint g_controls_hidden_tex = 0;       // Modified copy with controls zeroed out

// From asset_manager.c — tracks last opened asset filename
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

void bridge_gl_draw_arrays(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

    if (g_display_active) {
        glDrawArrays(mode, first, count);
    }
}

void bridge_gl_draw_elements(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

    if (g_gl_diag_enabled && g_gl_diag_frame < 3) {
        printf("[GL F%d] glDrawElements(mode=0x%x, count=%d, type=0x%x, indices=@0x%x)\n", g_gl_diag_frame, mode, count, type, indices_ptr);
        uint8_t* memory = emu->get_memory_base();
        if (g_gl_state.vptr_addr && g_gl_state.vptr_type == GL_FLOAT) {
            const float* v = (const float*)(memory + g_gl_state.vptr_addr);
            printf("  -> Verts: [%.2f %.2f %.2f / %.2f %.2f %.2f / ...]\n", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
    }

    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        glDrawElements(mode, count, type, (const void*)(memory + indices_ptr));
    }
}


// glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
void bridge_gl_vertex_pointer(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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


void bridge_gl_texcoord_pointer(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gl_color_pointer(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.color_pointer_calls++;
    uint32_t size = emu->get_reg(0);
    uint32_t type = emu->get_reg(1);
    uint32_t stride = emu->get_reg(2);
    uint32_t ptr = emu->get_reg(3);
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

void bridge_gl_normal_pointer(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gl_enable_client_state(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glEnableClientState(0x%x) %s", emu->get_reg(0), emu->get_reg(0)==0x8074?"VERTEX_ARRAY":emu->get_reg(0)==0x8078?"TEXTURE_COORD_ARRAY":"OTHER");
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.enable_client_state(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glEnableClientState(emu->get_reg(0));
}

void bridge_gl_disable_client_state(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glDisableClientState(0x%x)", emu->get_reg(0));
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.disable_client_state(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glDisableClientState(emu->get_reg(0));
}

// --- Phase 4: Textures ---

static std::unordered_map<uint32_t, bool> g_seen_textures;

extern bool g_gl_force_white;

void bridge_gl_bind_texture(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint32_t target = emu->get_reg(0);
    uint32_t tex_id = emu->get_reg(1);
    g_frame_stats.texture_binds++;
    g_frame_stats.last_bound_texture = tex_id;
    if (tex_id && !g_seen_textures.count(tex_id)) {
        g_seen_textures[tex_id] = true;
        g_frame_stats.unique_textures_bound++;
    }
    GL_DIAG("glBindTexture(target=0x%x, id=%d)", target, tex_id);
    TEX_LOG("glBindTexture(target=0x%x, id=%u)", target, tex_id);
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
void bridge_gl_tex_image_2d(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.tex_uploads++;
    uint8_t* memory = emu->get_memory_base();
    uint32_t target = emu->get_reg(0);
    uint32_t level = emu->get_reg(1);
    uint32_t internalformat = emu->get_reg(2);
    uint32_t width = emu->get_reg(3);
    uint32_t sp = emu->get_reg(13);
    uint32_t height = *(uint32_t*)(memory + sp);
    uint32_t border = *(uint32_t*)(memory + sp + 4);
    uint32_t format = *(uint32_t*)(memory + sp + 8);
    uint32_t type = *(uint32_t*)(memory + sp + 12);
    uint32_t pixels_ptr = *(uint32_t*)(memory + sp + 16);
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
        
        // Tag the controls atlas texture and create a modified copy
        if (width >= 512 && height >= 512 && strstr(g_last_opened_asset, "ui_game_atlas") != nullptr
            && strstr(g_last_opened_asset, "ui_game2") == nullptr) {
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
                g_frame_stats.last_bound_texture, g_last_opened_asset, width, height);
        }
    }
}

void bridge_gl_tex_sub_image_2d(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.tex_uploads++;
    uint8_t* memory = emu->get_memory_base();
    uint32_t target = emu->get_reg(0);
    uint32_t level = emu->get_reg(1);
    uint32_t xoff = emu->get_reg(2);
    uint32_t yoff = emu->get_reg(3);
    uint32_t sp = emu->get_reg(13);
    uint32_t width = *(uint32_t*)(memory + sp);
    uint32_t height = *(uint32_t*)(memory + sp + 4);
    uint32_t format = *(uint32_t*)(memory + sp + 8);
    uint32_t type = *(uint32_t*)(memory + sp + 12);
    uint32_t pixels_ptr = *(uint32_t*)(memory + sp + 16);
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

void bridge_gl_compressed_tex_image_2d(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.tex_uploads++;
    uint8_t* memory = emu->get_memory_base();
    uint32_t target = emu->get_reg(0);
    uint32_t level = emu->get_reg(1);
    uint32_t internalformat = emu->get_reg(2);
    uint32_t width = emu->get_reg(3);
    uint32_t sp = emu->get_reg(13);
    uint32_t height = *(uint32_t*)(memory + sp);
    uint32_t border = *(uint32_t*)(memory + sp + 4);
    uint32_t image_size = *(uint32_t*)(memory + sp + 8);
    uint32_t data_ptr = *(uint32_t*)(memory + sp + 12);
    
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
        
        glTexImage2D(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        std::cout << "[ETC1] Decoded " << width << "x" << height << " texture (" 
                  << (block_w * block_h) << " blocks)" << std::endl;
    } else {
        std::cout << "[GL] glCompressedTexImage2D: unknown format 0x" 
                  << std::hex << internalformat << std::dec << " — skipped" << std::endl;
    }
}

void bridge_gl_tex_parameteri(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gl_tex_parameterf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    uint32_t r2 = emu->get_reg(2);
    float val; memcpy(&val, &r2, 4);
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

void bridge_gl_enable(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glEnable(0x%x)", emu->get_reg(0));
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.enable(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glEnable(emu->get_reg(0));
}

void bridge_gl_disable(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    GL_DIAG("glDisable(0x%x)", emu->get_reg(0));
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.disable(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glDisable(emu->get_reg(0));
}

void bridge_gl_blend_func(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.blend_func(emu->get_reg(0), emu->get_reg(1));
        return;
    }
#endif
    if (g_display_active) glBlendFunc(emu->get_reg(0), emu->get_reg(1));
}

void bridge_gl_depth_func(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.depth_func(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glDepthFunc(emu->get_reg(0));
}

void bridge_gl_depth_mask(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.depth_mask(emu->get_reg(0) != 0);
        return;
    }
#endif
    if (g_display_active) glDepthMask(emu->get_reg(0));
}

void bridge_gl_color4f(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    uint32_t r0 = emu->get_reg(0), r1 = emu->get_reg(1);
    uint32_t r2 = emu->get_reg(2), r3 = emu->get_reg(3);
    float r, g, b, a;
    memcpy(&r, &r0, 4); memcpy(&g, &r1, 4);
    memcpy(&b, &r2, 4); memcpy(&a, &r3, 4);
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
}

void bridge_gl_scissor(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.scissor(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
        return;
    }
#endif
    if (g_display_active) glScissor(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
}

void bridge_gl_color_mask(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.color_mask(emu->get_reg(0) != 0, emu->get_reg(1) != 0, emu->get_reg(2) != 0, emu->get_reg(3) != 0);
        return;
    }
#endif
    if (g_display_active) glColorMask(emu->get_reg(0), emu->get_reg(1), emu->get_reg(2), emu->get_reg(3));
}

void bridge_gl_pixel_storei(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.pixel_storei(emu->get_reg(0), emu->get_reg(1));
        return;
    }
#endif
    if (g_display_active) glPixelStorei(emu->get_reg(0), emu->get_reg(1));
}

void bridge_gl_alpha_func(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    uint32_t r1 = emu->get_reg(1);
    float ref; memcpy(&ref, &r1, 4);
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

void bridge_gl_shade_model(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.shade_model(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glShadeModel(emu->get_reg(0));
}

void bridge_gl_clear_depthf(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    uint32_t r0 = emu->get_reg(0);
    float d; memcpy(&d, &r0, 4);
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

void bridge_gl_line_width(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    uint32_t r0 = emu->get_reg(0);
    float w; memcpy(&w, &r0, 4);
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

void bridge_gl_active_texture(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.active_texture(emu->get_reg(0));
        return;
    }
#endif
    if (g_display_active) glActiveTexture(emu->get_reg(0));
}

void bridge_gl_client_active_texture(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
void bridge_eglGetProcAddress(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glGetError(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    if (g_display_active) {
        emu->set_reg(0, glGetError());
    } else {
        emu->set_reg(0, 0); // GL_NO_ERROR
    }
}

void bridge_glGenTextures(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
        }
    } else {
        static uint32_t next_tex = 1;
        for (uint32_t i = 0; i < n && i < 64; i++) {
            *(uint32_t*)(memory + textures_ptr + i * 4) = next_tex++;
            GL_DIAG("  -> id[%d] = %d", i, next_tex - 1);
            TEX_LOG("  -> id[%u] = %u (stub)", i, next_tex - 1);
        }
    }
}

void bridge_glGenBuffers(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glGetIntegerv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glGetFloatv(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_glGetString(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    static bool initialized = false;
    uint8_t* memory = emu->get_memory_base();
    if (!initialized) {
        strcpy((char*)(memory + 0x40000), "OpenGL ES 2.0 (Swordigo Desktop)");
        strcpy((char*)(memory + 0x40100), "Swordigo Desktop Emulator");
        // Don't advertise ETC1 — forces game to use uncompressed textures (faster, no CPU decode)
        strcpy((char*)(memory + 0x40200), "GL_OES_compressed_ETC1_texture_data GL_OES_texture_npot");
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

void bridge_gl_delete_textures(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t n = emu->get_reg(0);
    uint32_t textures_ptr = emu->get_reg(1);
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

void bridge_gl_delete_buffers(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    if (g_display_active) {
        uint8_t* memory = emu->get_memory_base();
        uint32_t n = emu->get_reg(0);
        uint32_t buffers_ptr = emu->get_reg(1);
        glDeleteBuffers(n, (const GLuint*)(memory + buffers_ptr));
    }
}

void bridge_gl_bind_buffer(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    g_frame_stats.state_changes++;
    if (g_display_active) glBindBuffer(emu->get_reg(0), emu->get_reg(1));
}

void bridge_gl_buffer_data(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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

void bridge_gl_flush(void* emu_ptr) {
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.flush();
        return;
    }
#endif
    if (g_display_active) glFlush();
}

void bridge_gl_finish(void* emu_ptr) {
#ifdef VULKAN_BACKEND
    if (g_graphics_api == GraphicsAPI::VULKAN) {
        g_vk_backend.finish();
        return;
    }
#endif
    if (g_display_active) glFinish();
}

void bridge_gl_framebuffer_status(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0x8CD5); // GL_FRAMEBUFFER_COMPLETE
}


void bridge_matrix4_mul(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
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
void bridge_matrix4_ortho(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = emu->get_reg(0);  // Matrix4* this
    float left   = uint_to_float(emu->get_reg(1));
    float right  = uint_to_float(emu->get_reg(2));
    float bottom = uint_to_float(emu->get_reg(3));
    uint32_t sp  = emu->get_reg(13);
    float top    = uint_to_float(*(uint32_t*)(memory + sp));
    float near_  = uint_to_float(*(uint32_t*)(memory + sp + 4));
    float far_   = uint_to_float(*(uint32_t*)(memory + sp + 8));

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
void bridge_matrix4_perspective(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t dst_ptr = emu->get_reg(0);
    float fov    = uint_to_float(emu->get_reg(1));  // in degrees
    float aspect = uint_to_float(emu->get_reg(2));
    float near_  = uint_to_float(emu->get_reg(3));
    uint32_t sp  = emu->get_reg(13);
    float far_   = uint_to_float(*(uint32_t*)(memory + sp));

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
void bridge_errno(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, GUEST_ERRNO_ADDR);
}

void bridge_google_blocking(void* emu_ptr) {
    // __google_potentially_blocking_region_begin/end - no-op
}

void bridge_cxa_atexit(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0);
}


void bridge_stack_chk_fail(void* emu_ptr) {
    std::cerr << "[FATAL] __stack_chk_fail called!" << std::endl;
}

void bridge_cxa_guard_acquire(void* emu_ptr) {
    // __cxa_guard_acquire(int32_t* guard_object)
    // Returns 1 if initialization is needed, 0 if already done
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t guard_ptr = emu->get_reg(0);
    if (guard_ptr && guard_ptr < 0xE0000000) {
        uint8_t guard_byte = memory[guard_ptr];
        if (guard_byte != 0) {
            emu->set_reg(0, 0); // already initialized
            return;
        }
    }
    emu->set_reg(0, 1); // needs initialization
}

void bridge_cxa_guard_release(void* emu_ptr) {
    // __cxa_guard_release(int32_t* guard_object) — mark as initialized
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t guard_ptr = emu->get_reg(0);
    if (guard_ptr && guard_ptr < 0xE0000000) {
        memory[guard_ptr] = 1; // mark initialized
    }
}

void bridge_cxa_guard_abort(void* emu_ptr) {
    // __cxa_guard_abort — just return
    Emulator* emu = (Emulator*)emu_ptr;
    (void)emu;
}

void bridge_pthread_mutex(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0); // success
}

void bridge_pthread_once(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t once_control_ptr = emu->get_reg(0);
    uint32_t init_routine = emu->get_reg(1);

    if (once_control_ptr == 0) {
        emu->set_reg(0, -1); // EINVAL
        return;
    }

    uint32_t once_val = *(uint32_t*)(memory + once_control_ptr);
    if (once_val != 2) { // not initialized
        // Mark as initialized/completed to avoid recursion/reentry issues
        *(uint32_t*)(memory + once_control_ptr) = 2;
        if (init_routine != 0) {
            std::cout << "[Bridge] pthread_once calling init_routine at 0x" << std::hex << init_routine << std::dec << std::endl;
            emu->call(init_routine, {});
        }
    }
    emu->set_reg(0, 0); // success
}

void bridge_pthread_cond(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 0); // success
}

void bridge_pthread_create(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    // Can't actually create threads in guest - return error
    std::cout << "[Bridge] pthread_create called (stubbed)" << std::endl;
    emu->set_reg(0, 0); // pretend success
}

void bridge_pthread_self(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    emu->set_reg(0, 1); // fake thread id
}

void bridge_time(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t t_ptr = emu->get_reg(0);
    time_t t = time(nullptr);
    if (t_ptr) {
        *(uint32_t*)(memory + t_ptr) = (uint32_t)t;
    }
    emu->set_reg(0, (uint32_t)t);
}

void bridge_clock_gettime(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t tp_ptr = emu->get_reg(1);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (tp_ptr) {
        *(uint32_t*)(memory + tp_ptr) = (uint32_t)ts.tv_sec;
        *(uint32_t*)(memory + tp_ptr + 4) = (uint32_t)ts.tv_nsec;
    }
    emu->set_reg(0, 0);
}

void bridge_gettimeofday(void* emu_ptr) {
    Emulator* emu = (Emulator*)emu_ptr;
    uint8_t* memory = emu->get_memory_base();
    uint32_t tv_ptr = emu->get_reg(0);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv_ptr) {
        *(uint32_t*)(memory + tv_ptr) = (uint32_t)tv.tv_sec;
        *(uint32_t*)(memory + tv_ptr + 4) = (uint32_t)tv.tv_usec;
    }
    emu->set_reg(0, 0);
}

void JniBridge::init_standard_bridges() {
    register_handler("malloc", bridge_malloc);
    register_handler("calloc", bridge_calloc);
    register_handler("realloc", bridge_realloc);
    register_handler("free", bridge_free);

    register_handler("memcpy", bridge_memcpy);
    register_handler("memset", bridge_memset);
    register_handler("memmove", bridge_memmove);
    register_handler("strlen", bridge_strlen);
    register_handler("memcmp", bridge_memcmp);
    register_handler("memchr", bridge_memchr_impl);

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

    register_handler("__aeabi_memclr", bridge_aeabi_memclr);
    register_handler("__aeabi_memclr4", bridge_aeabi_memclr);
    register_handler("__aeabi_memclr8", bridge_aeabi_memclr);
    register_handler("__aeabi_memset", bridge_aeabi_memset);
    register_handler("__aeabi_memset4", bridge_aeabi_memset);
    register_handler("__aeabi_memset8", bridge_aeabi_memset);
    register_handler("__aeabi_memcpy", bridge_aeabi_memcpy);
    register_handler("__aeabi_memcpy4", bridge_aeabi_memcpy);
    register_handler("__aeabi_memcpy8", bridge_aeabi_memcpy);
    register_handler("__aeabi_memmove", bridge_aeabi_memmove);
    register_handler("__aeabi_memmove4", bridge_aeabi_memmove);
    register_handler("__aeabi_memmove8", bridge_aeabi_memmove);

    register_handler("__aeabi_uidiv", bridge_aeabi_uidiv);
    register_handler("__aeabi_idiv", bridge_aeabi_idiv);
    register_handler("__aeabi_uidivmod", bridge_aeabi_uidivmod);
    register_handler("__aeabi_idivmod", bridge_aeabi_idivmod);
    register_handler("__aeabi_ldivmod", bridge_aeabi_ldivmod);
    register_handler("__aeabi_uldivmod", bridge_aeabi_uldivmod);

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

    register_handler("powf", bridge_powf);
    register_handler("pow", bridge_pow);
    register_handler("sincosf", bridge_sincosf);

    // Double-precision math (critical for Lua — Lua uses double for ALL numbers)
    register_handler("floor", bridge_floor_d);
    register_handler("ceil", bridge_ceil_d);
    register_handler("sqrt", bridge_sqrt_d);
    register_handler("fmod", bridge_fmod_d);
    register_handler("fabs", bridge_fabs_d);
    register_handler("log", bridge_log_d);
    register_handler("log10", bridge_log10_d);
    register_handler("log2", bridge_log2_d);
    register_handler("exp", bridge_exp_d);
    register_handler("ldexp", bridge_ldexp_d);
    register_handler("frexp", bridge_frexp_d);
    register_handler("modf", bridge_modf_d);
    register_handler("asin", bridge_asin_d);
    register_handler("atan", bridge_atan_d);
    register_handler("atan2", bridge_atan2_d);

    // File I/O
    register_handler("fopen", bridge_fopen);
    register_handler("fclose", bridge_fclose);
    register_handler("fread", bridge_fread);
    register_handler("fwrite", bridge_fwrite);
    register_handler("fseek", bridge_fseek);
    register_handler("ftell", bridge_ftell);
    register_handler("fflush", bridge_fflush);
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
    register_handler("glVertexAttribPointer", bridge_gl_noop);
    register_handler("glEnableVertexAttribArray", bridge_gl_noop);
    register_handler("glDisableVertexAttribArray", bridge_gl_noop);
    register_handler("glUseProgram", bridge_gl_noop);
    register_handler("glBindBuffer", bridge_gl_bind_buffer);
    register_handler("glBufferData", bridge_gl_buffer_data);
    register_handler("glBufferSubData", bridge_gl_noop);
    register_handler("glDeleteTextures", bridge_gl_delete_textures);
    register_handler("glDeleteBuffers", bridge_gl_delete_buffers);
    register_handler("glBindFramebuffer", bridge_gl_noop);
    register_handler("glGenFramebuffers", bridge_glGenBuffers);
    register_handler("glDeleteFramebuffers", bridge_gl_noop);
    register_handler("glFramebufferTexture2D", bridge_gl_noop);
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
}



