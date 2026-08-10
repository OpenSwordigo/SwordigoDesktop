/*
 * ============================================================================
 *  Redstell GC — Intelligent Resource Garbage Collector & Management System
 * ============================================================================
 *  Tracks and manages guest and host resource allocations (OpenGL textures,
 *  PVR graphics data, POD models, scene structures, Lua states/closures,
 *  and system allocations) to dynamically prevent memory leaks and coordinate
 *  safe reclamation.
 * ============================================================================
 */
#include "rgc.h"
#include "loader/elf_loader_arm64.h"
#include "loader/elf_loader.h"
#include "platform/gl_inc.h"
#include <stdarg.h>
#include <stdio.h>
#include <cstring>

extern "C" void rgc_reclaim_guest_memory(uint64_t addr);

// Link external modules from main.cpp for callsite resolution
extern so_module g_main_mod;
extern so_module_arm64 g_main_mod_64;
extern so_module_arm64 g_sre_mod;

const char* rgc_resource_type_name(RgcResourceType type) {
    switch (type) {
        case RgcResourceType::MEMORY: return "Memory";
        case RgcResourceType::TEXTURE: return "Texture";
        case RgcResourceType::PVR_IMAGE: return "PVR Image";
        case RgcResourceType::POD_MODEL: return "POD Model";
        case RgcResourceType::SCENE_OBJECT: return "Scene Object";
        case RgcResourceType::SHADER: return "Shader";
        case RgcResourceType::LUA_OBJECT: return "Lua Object";
        case RgcResourceType::LUA_STATE: return "Lua State";
        case RgcResourceType::AUDIO: return "Audio";
        case RgcResourceType::FRAMEBUFFER: return "Framebuffer";
        case RgcResourceType::RENDER_TARGET: return "Render Target";
        case RgcResourceType::OPENGL_OBJECT: return "OpenGL Object";
        case RgcResourceType::FILE_BUFFER: return "File Buffer";
        case RgcResourceType::NETWORK_BUFFER: return "Network Buffer";
        case RgcResourceType::TEMPORARY_BUFFER: return "Temporary Buffer";
        case RgcResourceType::VFS_HANDLE: return "VFS Handle";
        default: return "Unknown";
    }
}

RedstellGC& RedstellGC::instance() {
    static RedstellGC inst;
    return inst;
}

RedstellGC::RedstellGC() 
    : m_running(false)
    , m_current_ram(0)
    , m_peak_ram(0)
    , m_largest_alloc(0)
    , m_alloc_counter(0)
    , m_free_counter(0)
    , m_alloc_rate(0)
    , m_free_rate(0)
    , m_texture_count(0)
    , m_pod_count(0)
    , m_lua_object_count(0)
    , m_opengl_count(0)
    , m_open_file_count(0)
    , m_openal_count(0)
    , m_optimizations_performed(0)
    , m_last_cleanup_duration(0.0)
    , m_has_redirects(false)
{
    m_boot_time = std::chrono::steady_clock::now();
    memset((void*)m_alloc_buckets, 0, sizeof(m_alloc_buckets));
}

RedstellGC::~RedstellGC() {
    shutdown();
}

void RedstellGC::init() {
    if (m_running) return;
    m_running = true;
    m_main_thread_id = std::this_thread::get_id();

    log("[Redstell GC] Initializing...");

    // Start background threads
    m_gc_thread = std::thread(&RedstellGC::gc_thread_func, this);
    m_opt_thread = std::thread(&RedstellGC::opt_thread_func, this);
    m_stats_thread = std::thread(&RedstellGC::stats_thread_func, this);

    // Start worker pool (4 workers)
    for (int i = 0; i < 4; i++) {
        m_workers.push_back(std::thread(&RedstellGC::worker_thread_func, this, i));
    }

    log("[Redstell GC] Worker threads started.");
}

void RedstellGC::shutdown() {
    if (!m_running) return;
    m_running = false;

    log("[Redstell GC] Shutting down worker threads...");
    m_queue_cv.notify_all();

    if (m_gc_thread.joinable()) m_gc_thread.join();
    if (m_opt_thread.joinable()) m_opt_thread.join();
    if (m_stats_thread.joinable()) m_stats_thread.join();

    for (auto& worker : m_workers) {
        if (worker.joinable()) worker.join();
    }
    m_workers.clear();

    log("[Redstell GC] Shutdown complete.");
}

void RedstellGC::log(const char* fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    // Print to standard console
    printf("%s\n", message);

    // Push into thread-safe circular buffer
    std::lock_guard<std::mutex> lock(m_log_mutex);
    uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_boot_time).count();
    
    m_log_buffer.push_back({ts, message});
    if (m_log_buffer.size() > MAX_LOG_SIZE) {
        m_log_buffer.erase(m_log_buffer.begin());
    }
}

std::vector<RgcLogEntry> RedstellGC::get_logs() {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    return m_log_buffer;
}

std::string RedstellGC::resolve_symbol(uint64_t address) {
    if (address == 0) return "Unknown";
    
    static std::unordered_map<uint64_t, std::string> s_symbol_cache;
    static std::mutex s_cache_mutex;
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_symbol_cache.find(address);
        if (it != s_symbol_cache.end()) return it->second;
    }

    std::string result = "";
    
    // 1. Check in libsre.so (ARM64)
    if (g_sre_mod.base_addr != 0 && address >= g_sre_mod.base_addr && address < g_sre_mod.base_addr + g_sre_mod.mem_size) {
        uint64_t offset = address - g_sre_mod.base_addr;
        uint64_t best_offset = 0;
        std::string best_name = "";
        for (int i = 0; i < g_sre_mod.num_dynsym; i++) {
            auto& sym = g_sre_mod.dynsym[i];
            if (sym.st_value > 0 && sym.st_value <= offset) {
                if (sym.st_value > best_offset) {
                    best_offset = sym.st_value;
                    best_name = g_sre_mod.dynstr + sym.st_name;
                }
            }
        }
        if (!best_name.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s+0x%lx", best_name.c_str(), offset - best_offset);
            result = std::string("libsre.so:") + buf;
        } else {
            result = "libsre.so";
        }
    }
    // 2. Check in libswordigo.so (ARM64)
    else if (g_main_mod_64.base_addr != 0 && address >= g_main_mod_64.base_addr && address < g_main_mod_64.base_addr + g_main_mod_64.mem_size) {
        uint64_t offset = address - g_main_mod_64.base_addr;
        uint64_t best_offset = 0;
        std::string best_name = "";
        for (int i = 0; i < g_main_mod_64.num_dynsym; i++) {
            auto& sym = g_main_mod_64.dynsym[i];
            if (sym.st_value > 0 && sym.st_value <= offset) {
                if (sym.st_value > best_offset) {
                    best_offset = sym.st_value;
                    best_name = g_main_mod_64.dynstr + sym.st_name;
                }
            }
        }
        if (!best_name.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s+0x%lx", best_name.c_str(), offset - best_offset);
            result = std::string("libswordigo.so:") + buf;
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "libswordigo.so+0x%lx", offset);
            result = buf;
        }
    }
    // 3. Check in libswordigo.so (ARM32)
    else if (g_main_mod.base_addr != 0 && address >= g_main_mod.base_addr && address < g_main_mod.base_addr + g_main_mod.mem_size) {
        uint64_t offset = address - g_main_mod.base_addr;
        uint64_t best_offset = 0;
        std::string best_name = "";
        for (int i = 0; i < g_main_mod.num_dynsym; i++) {
            auto& sym = g_main_mod.dynsym[i];
            if (sym.st_value > 0 && sym.st_value <= offset) {
                if (sym.st_value > best_offset) {
                    best_offset = sym.st_value;
                    best_name = g_main_mod.dynstr + sym.st_name;
                }
            }
        }
        if (!best_name.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s+0x%lx", best_name.c_str(), (unsigned long)(offset - best_offset));
            result = std::string("libswordigo.so:") + buf;
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "libswordigo.so+0x%lx", (unsigned long)offset);
            result = buf;
        }
    }

    if (result.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)address);
        result = buf;
    }

    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        s_symbol_cache[address] = result;
    }
    return result;
}

thread_local std::vector<RedstellGC::RgcEvent> tl_write_queue;
thread_local uint64_t tl_last_flush_us = 0;  // monotonic us of the last auto-flush

extern bool g_advanced_redstell_opts;

// Bounds the thread-local event queue even when the host frame loop is not
// running (e.g. the main thread is stuck inside the guest emulator during a
// long scene load). Without this, every guest malloc/calloc/realloc pushes an
// RgcEvent and the queue grows without bound — the RL mod's allocation pattern
// grew it past 384 MB (an "insane" memory leak) until the OOM killer fired.
void RedstellGC::auto_flush_tl_queue() {
    if (tl_write_queue.empty()) return;
    // Cheap size bound first (no clock call in the hot path).
    if (tl_write_queue.size() >= 4096) {
        flush_thread_local_queues();
        return;
    }
    // Only pay for the clock when we actually need the time comparison.
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - tl_last_flush_us > 50000) {
        flush_thread_local_queues();
        tl_last_flush_us = now;
    }
}

void RedstellGC::track_alloc(uint64_t addr, size_t size, uint64_t lr, const char* reason, RgcResourceType type) {
    if (g_sre_mod.base_addr == 0 || addr == 0) return;
    tl_write_queue.push_back({RgcEvent::ALLOC, addr, size, lr, type, reason});
    if (std::this_thread::get_id() != m_main_thread_id) {
        flush_thread_local_queues();
    } else {
        auto_flush_tl_queue();
    }
}

void RedstellGC::track_free(uint64_t addr, uint64_t lr) {
    if (g_sre_mod.base_addr == 0 || addr == 0) return;
    tl_write_queue.push_back({RgcEvent::FREE, addr, 0, lr, RgcResourceType::MEMORY, nullptr});
    if (std::this_thread::get_id() != m_main_thread_id) {
        flush_thread_local_queues();
    } else {
        auto_flush_tl_queue();
    }
}

bool RedstellGC::is_tracked(uint64_t addr) {
    if (g_sre_mod.base_addr == 0) return false;
    std::shared_lock<std::shared_mutex> lock(m_allocs_mutex);
    return m_allocs.count(addr) > 0;
}

void RedstellGC::flush_thread_local_queues() {
    if (tl_write_queue.empty()) return;
    std::lock_guard<std::mutex> lock(m_queue_swap_mutex);
    m_write_queue.insert(m_write_queue.end(), tl_write_queue.begin(), tl_write_queue.end());
    tl_write_queue.clear();
}

void RedstellGC::process_event_queue() {
    std::vector<RgcEvent> local_queue;
    {
        std::lock_guard<std::mutex> lock(m_queue_swap_mutex);
        if (m_write_queue.empty()) return;
        local_queue = std::move(m_write_queue);
        m_write_queue.clear();
    }

    std::unique_lock<std::shared_mutex> lock(m_allocs_mutex);
    for (const auto& ev : local_queue) {
        if (ev.type == RgcEvent::ALLOC) {
            if (m_allocs.count(ev.addr)) {
                log("[Redstell GC] Warning: Zombie allocation detected at 0x%lx (Size %zu was not freed)", ev.addr, m_allocs[ev.addr].size);
            }

            RgcAllocInfo info;
            info.address = ev.addr;
            info.size = ev.size;
            info.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_boot_time).count();
            info.type = ev.r_type;
            info.thread_id = 0;
            info.reason = ev.reason ? ev.reason : "Generic Allocation";
            info.callsite = ev.lr;
            info.resolved_callsite = "";

            if (g_sre_mod.base_addr != 0 && ev.lr >= g_sre_mod.base_addr && ev.lr < g_sre_mod.base_addr + g_sre_mod.mem_size) {
                info.subsystem = "SRE Runtime";
                if (ev.reason) {
                    std::string r_str = ev.reason;
                    if (r_str.find("Lua") != std::string::npos) {
                        info.subsystem = "SRE Lua";
                        info.type = RgcResourceType::LUA_OBJECT;
                    } else if (r_str.find("VFS") != std::string::npos || r_str.find("AndroidAsset") != std::string::npos) {
                        info.subsystem = "SRE VFS";
                        info.type = RgcResourceType::FILE_BUFFER;
                    }
                }
            } else {
                info.subsystem = "Game Engine";
            }

            m_allocs[ev.addr] = info;
            m_alloc_counter++;
            
            m_current_ram += ev.size;
            if (m_current_ram.load() > m_peak_ram.load()) {
                m_peak_ram.store(m_current_ram.load());
            }
            if (ev.size > m_largest_alloc.load()) {
                m_largest_alloc.store(ev.size);
            }
            
            int bucket = 0;
            size_t s = ev.size;
            while (s > 1024 && bucket < 9) {
                s /= 1024;
                bucket++;
            }
            m_alloc_buckets[bucket]++;

        } else if (ev.type == RgcEvent::FREE) {
            auto it = m_allocs.find(ev.addr);
            if (it != m_allocs.end()) {
                m_current_ram -= it->second.size;
                m_allocs.erase(it);
                m_free_counter++;
            } else {
                std::string symbol = resolve_symbol(ev.lr);
                log("[Redstell GC] Warning: Invalid or Double free attempt at 0x%lx (caller: 0x%lx %s)", ev.addr, ev.lr, symbol.c_str());
            }
        }
    }
}

void RedstellGC::track_resource(uint64_t addr, RgcResourceType type, const char* name) {
    if (g_sre_mod.base_addr == 0 || addr == 0) return;

    std::unique_lock<std::shared_mutex> lock(m_resources_mutex);
    
    RgcResourceNode node;
    node.address = addr;
    node.type = type;
    node.name = name ? name : "Resource";
    node.ref_count = 1;
    node.creation_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_boot_time).count();
    node.marked_for_cleanup = false;
    
    m_resources[addr] = node;

    if (type == RgcResourceType::TEXTURE) m_texture_count++;
    else if (type == RgcResourceType::POD_MODEL) m_pod_count++;
    else if (type == RgcResourceType::LUA_OBJECT) m_lua_object_count++;
    else if (type == RgcResourceType::OPENGL_OBJECT) m_opengl_count++;
    else if (type == RgcResourceType::AUDIO) m_openal_count++;
}

void RedstellGC::add_resource_ref(uint64_t addr, const char* subsystem) {
    if (addr == 0) return;

    std::unique_lock<std::shared_mutex> lock(m_resources_mutex);
    auto it = m_resources.find(addr);
    if (it != m_resources.end()) {
        it->second.ref_count++;
        it->second.referencing_subsystems.insert(subsystem);
    }
}

void RedstellGC::release_resource_ref(uint64_t addr) {
    if (addr == 0) return;

    std::unique_lock<std::shared_mutex> lock(m_resources_mutex);
    auto it = m_resources.find(addr);
    if (it != m_resources.end()) {
        it->second.ref_count--;
        if (it->second.ref_count <= 0 && !it->second.marked_for_cleanup) {
            it->second.marked_for_cleanup = true;
            
            // Queue async cleanup
            std::lock_guard<std::mutex> qlock(m_queue_mutex);
            m_cleanup_queue.push({it->second.address, it->second.type, it->second.name});
            m_queue_cv.notify_one();
        }
    }
}

void RedstellGC::register_dependency(uint64_t parent_addr, uint64_t child_addr) {
    std::unique_lock<std::shared_mutex> lock(m_resources_mutex);
    auto it = m_resources.find(parent_addr);
    if (it != m_resources.end()) {
        it->second.dependencies.push_back(child_addr);
        add_resource_ref(child_addr, "Dependency Graph");
    }
}

void RedstellGC::track_file_open(uint64_t handle, const char* path) {
    if (g_sre_mod.base_addr == 0 || handle == 0 || !path) return;
    std::unique_lock<std::shared_mutex> lock(m_files_mutex);
    m_file_handles[handle] = path;
    m_open_file_count++;
}

void RedstellGC::track_file_close(uint64_t handle) {
    if (g_sre_mod.base_addr == 0 || handle == 0) return;
    std::unique_lock<std::shared_mutex> lock(m_files_mutex);
    auto it = m_file_handles.find(handle);
    if (it != m_file_handles.end()) {
        m_file_handles.erase(it);
        if (m_open_file_count > 0) m_open_file_count--;
    }
}

std::string RedstellGC::get_file_path(uint64_t handle) {
    std::shared_lock<std::shared_mutex> lock(m_files_mutex);
    auto it = m_file_handles.find(handle);
    if (it != m_file_handles.end()) return it->second;
    return "";
}

void RedstellGC::track_gl_gen_texture(uint32_t tex_id) {
    if (g_sre_mod.base_addr == 0 || tex_id == 0) return;
    track_resource((uint64_t)tex_id, RgcResourceType::TEXTURE, "GL Texture");
    m_opengl_count++;
}

void RedstellGC::track_gl_delete_texture(uint32_t tex_id) {
    if (g_sre_mod.base_addr == 0 || tex_id == 0) return;
    
    std::unique_lock<std::shared_mutex> lock(m_textures_mutex);
    auto it = m_textures.find(tex_id);
    if (it != m_textures.end()) {
        m_textures.erase(it);
        m_texture_redirects.erase(tex_id);
        if (m_texture_redirects.empty()) {
            m_has_redirects.store(false, std::memory_order_relaxed);
        }
    }
    
    release_resource_ref((uint64_t)tex_id);
    if (m_texture_count > 0) m_texture_count--;
    if (m_opengl_count > 0) m_opengl_count--;
}

// Simple pixel hash function (Fowler-Noll-Vo / FNV-1a)
static uint32_t calculate_pixels_hash(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    const uint8_t* ptr = (const uint8_t*)data;
    uint32_t hash = 2166136261U;
    // Hash up to 1MB to avoid stalling the main thread on massive textures
    size_t limit = size < 1024 * 1024 ? size : 1024 * 1024;
    for (size_t i = 0; i < limit; i++) {
        hash ^= ptr[i];
        hash *= 16777619U;
    }
    return hash;
}

void RedstellGC::track_gl_tex_image(uint32_t tex_id, int width, int height, int internal_format, const char* asset_name, const void* pixels, size_t pixels_size) {
    if (g_sre_mod.base_addr == 0 || tex_id == 0) return;

    std::unique_lock<std::shared_mutex> lock(m_textures_mutex);
    TextureMetadata meta;
    meta.tex_id = tex_id;
    meta.width = width;
    meta.height = height;
    meta.internal_format = internal_format;
    meta.asset_name = asset_name ? asset_name : "";
    meta.pixel_hash = 0; // Hashing is disabled to save CPU cycles during mod texture uploads (duplicate merging is off)
    meta.is_duplicate = false;
    meta.redirect_target = tex_id;

    m_textures[tex_id] = meta;
}

uint32_t RedstellGC::get_redirected_texture(uint32_t tex_id) {
    if (g_sre_mod.base_addr == 0) return tex_id;
    if (!m_has_redirects.load(std::memory_order_relaxed)) return tex_id;
    std::shared_lock<std::shared_mutex> lock(m_textures_mutex);
    auto it = m_texture_redirects.find(tex_id);
    if (it != m_texture_redirects.end()) {
        return it->second;
    }
    return tex_id;
}

void RedstellGC::track_al_gen_source(uint32_t source_id) {
    if (source_id == 0) return;
    std::unique_lock<std::shared_mutex> lock(m_openal_mutex);
    m_al_sources.insert(source_id);
    track_resource((uint64_t)source_id, RgcResourceType::AUDIO, "AL Source");
}

void RedstellGC::track_al_delete_source(uint32_t source_id) {
    if (source_id == 0) return;
    std::unique_lock<std::shared_mutex> lock(m_openal_mutex);
    if (m_al_sources.erase(source_id)) {
        release_resource_ref((uint64_t)source_id);
    }
}

void RedstellGC::track_al_gen_buffer(uint32_t buffer_id) {
    if (buffer_id == 0) return;
    std::unique_lock<std::shared_mutex> lock(m_openal_mutex);
    m_al_buffers.insert(buffer_id);
    track_resource((uint64_t)buffer_id, RgcResourceType::AUDIO, "AL Buffer");
}

void RedstellGC::track_al_delete_buffer(uint32_t buffer_id) {
    if (buffer_id == 0) return;
    std::unique_lock<std::shared_mutex> lock(m_openal_mutex);
    if (m_al_buffers.erase(buffer_id)) {
        release_resource_ref((uint64_t)buffer_id);
    }
}

void RedstellGC::register_lua_state(void* L) {
    if (!L) return;
    std::unique_lock<std::shared_mutex> lock(m_lua_mutex);
    m_lua_states.insert(L);
    track_resource((uint64_t)L, RgcResourceType::LUA_STATE, "Lua State");
}

void RedstellGC::unregister_lua_state(void* L) {
    if (!L) return;
    std::unique_lock<std::shared_mutex> lock(m_lua_mutex);
    if (m_lua_states.erase(L)) {
        release_resource_ref((uint64_t)L);
    }
}

void RedstellGC::process_main_thread_deletions() {
    flush_thread_local_queues();

    std::vector<uint32_t> to_delete;
    {
        std::lock_guard<std::mutex> lock(m_deletions_mutex);
        if (m_queued_gl_texture_deletions.empty()) return;
        to_delete = std::move(m_queued_gl_texture_deletions);
        m_queued_gl_texture_deletions.clear();
    }

    // Call OpenGL finalizer safely on main thread
    if (to_delete.size() > 0) {
        log("[Redstell GC] Executing %zu GPU texture deletions on main thread", to_delete.size());
        for (uint32_t tex_id : to_delete) {
#ifndef VULKAN_BACKEND
            GLuint gl_id = (GLuint)tex_id;
            glDeleteTextures(1, &gl_id);
#endif
        }
    }
}

float RedstellGC::get_fragmentation_estimate() const {
    std::shared_lock<std::shared_mutex> lock(m_allocs_mutex);
    if (m_allocs.empty()) return 0.0f;

    uint64_t min_addr = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t max_addr = 0ULL;
    size_t total_alloc_size = 0;

    for (const auto& pair : m_allocs) {
        if (pair.first < min_addr) min_addr = pair.first;
        if (pair.first + pair.second.size > max_addr) max_addr = pair.first + pair.second.size;
        total_alloc_size += pair.second.size;
    }

    uint64_t span = max_addr - min_addr;
    if (span <= 0 || total_alloc_size == 0) return 0.0f;

    float ratio = (float)total_alloc_size / (float)span;
    return 1.0f - ratio;
}

void RedstellGC::get_allocation_histogram(uint64_t* out_buckets) const {
    for (int i = 0; i < 10; i++) {
        out_buckets[i] = m_alloc_buckets[i].load();
    }
}

uint64_t RedstellGC::get_resource_count() const {
    std::shared_lock<std::shared_mutex> lock(m_resources_mutex);
    return m_resources.size();
}

uint32_t RedstellGC::get_cleanup_queue_size() {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    return m_cleanup_queue.size();
}

uint32_t RedstellGC::get_pending_resources_count() {
    std::shared_lock<std::shared_mutex> lock(m_resources_mutex);
    uint32_t count = 0;
    for (const auto& pair : m_resources) {
        if (pair.second.ref_count <= 0 && !pair.second.marked_for_cleanup) {
            count++;
        }
    }
    return count;
}

// Dedicated Garbage Collection Thread function
void RedstellGC::gc_thread_func() {
    while (m_running) {
        RgcCleanupJob job;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !m_cleanup_queue.empty() || !m_running;
            });
            if (!m_running) break;
            if (m_cleanup_queue.empty()) continue;
            
            job = m_cleanup_queue.front();
            m_cleanup_queue.pop();
        }

        auto start = std::chrono::steady_clock::now();

        // Perform async resource release
        if (job.type == RgcResourceType::TEXTURE) {
            // Manual texture deletion is disabled to prevent premature deletion of active/wanted
            // character or boss textures. The native game engine manages texture lifecycles.
        } else if (job.type == RgcResourceType::FILE_BUFFER) {
            // Free the CPU file buffer
            track_free(job.address);
        }

        auto end = std::chrono::steady_clock::now();
        m_last_cleanup_duration = std::chrono::duration<double, std::milli>(end - start).count();
    }
}

// Background Optimization Thread function
void RedstellGC::opt_thread_func() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!m_running) break;

        // 1. Merge Duplicate Textures (DISABLED)
        // Disabled because empty or zero-initialized dynamic textures of matching dimensions 
        // (such as the hero and boss skin assets before dynamic pixel upload) generate identical 
        // FNV-1a hashes. This caused incorrect merges, GL deletion of active texture handles,
        // and visual corruption (white, texture-less models). On modern PCs, saving a few MB
        // of VRAM is not worth the risk of visual bugs.
        
        // 2. Scan for, report, and actively reclaim dangling / leaked file handles and memory blocks
        if (g_advanced_redstell_opts) {
            std::vector<uint64_t> leaked_buffers;
            {
                std::shared_lock<std::shared_mutex> alock(m_allocs_mutex);
                uint64_t current_time = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - m_boot_time).count();
                
                for (const auto& pair : m_allocs) {
                    const auto& info = pair.second;
                    // If allocation is older than 5 minutes (300 seconds) and still active
                    if (current_time - info.timestamp > 300ULL * 1000 * 1000) {
                        if (info.type == RgcResourceType::FILE_BUFFER && info.reason.find("AndroidAsset") != std::string::npos) {
                            std::string sym = resolve_symbol(info.callsite);
                            log("[Redstell GC] Reclaiming leaked File Buffer at 0x%lx (Size %zu, Alloc: %s)", 
                                info.address, info.size, sym.c_str());
                            leaked_buffers.push_back(info.address);
                        }
                    }
                }
            }

            for (uint64_t addr : leaked_buffers) {
                rgc_reclaim_guest_memory(addr);
                track_free(addr);
            }
        }
    }
}

// Memory Statistics Gathering Thread
void RedstellGC::stats_thread_func() {
    uint64_t last_alloc_count = 0;
    uint64_t last_free_count = 0;
    int tick = 0;

    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (!m_running) break;

        process_event_queue();

        tick++;
        if (tick >= 30) { // Recalculate stats approx every 500ms
            tick = 0;
            size_t current = 0;
            uint64_t buckets[10] = {0};

            {
                std::shared_lock<std::shared_mutex> lock(m_allocs_mutex);
                for (const auto& pair : m_allocs) {
                    size_t sz = pair.second.size;
                    current += sz;

                    // Sort into buckets for histogram
                    if (sz < 64) buckets[0]++;
                    else if (sz < 256) buckets[1]++;
                    else if (sz < 1024) buckets[2]++;
                    else if (sz < 4096) buckets[3]++;
                    else if (sz < 16384) buckets[4]++;
                    else if (sz < 65536) buckets[5]++;
                    else if (sz < 262144) buckets[6]++;
                    else if (sz < 1048576) buckets[7]++;
                    else buckets[8]++;
                }
            }

            m_current_ram.store(current);
            
            // Update peaks
            size_t peak = m_peak_ram.load();
            while (current > peak && !m_peak_ram.compare_exchange_weak(peak, current));

            // Copy buckets to atomics
            for (int i = 0; i < 10; i++) {
                m_alloc_buckets[i].store(buckets[i]);
            }

            // Calculate rates
            uint64_t total_allocs = m_alloc_counter.load();
            uint64_t total_frees = m_free_counter.load();

            m_alloc_rate.store((total_allocs - last_alloc_count) * 2); // multiplied by 2 for rates per second
            m_free_rate.store((total_frees - last_free_count) * 2);

            last_alloc_count = total_allocs;
            last_free_count = total_frees;
        }
    }
}

// Asynchronous Asset Cleanup Worker pool function
void RedstellGC::worker_thread_func(int id) {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Idle loop representing asset decoding or CPU-side buffer clearance
    }
}

#include "platform/pvrtc_decoder.h"
#include <fstream>

#pragma pack(push, 1)
struct PPDv3Header {
    uint32_t version; uint32_t flags; uint64_t pixel_format;
    uint32_t color_space; uint32_t channel_type;
    uint32_t height; uint32_t width; uint32_t depth;
    uint32_t num_surfaces; uint32_t num_faces;
    uint32_t mip_count; uint32_t metadata_size;
};
struct PPDv2Header {
    uint32_t header_size; uint32_t height; uint32_t width;
    uint32_t mip_count; uint32_t flags; uint32_t data_size;
    uint32_t bpp; uint32_t mask_r; uint32_t mask_g;
    uint32_t mask_b; uint32_t mask_a; uint32_t magic; uint32_t num_surfaces;
};
#pragma pack(pop)

void RedstellGC::ppd_preload_texture(const std::string& path) {
    if (path.empty() || path.find(".pvr") == std::string::npos) return;
    
    std::lock_guard<std::mutex> lock(m_ppd_mutex);
    if (m_ppd_cache.count(path)) {
        return; // Already preloading
    }
    
    m_ppd_cache[path] = std::async(std::launch::async, [path, this]() -> PPDTexture {
        PPDTexture tex;
        
        std::ifstream f(path, std::ios::binary);
        if (!f) return tex;
        
        f.seekg(0, std::ios::end);
        size_t file_size = f.tellg();
        f.seekg(0, std::ios::beg);
        if (file_size < sizeof(PPDv2Header)) return tex;
        
        std::vector<uint8_t> file_data(file_size);
        f.read((char*)file_data.data(), file_size);
        f.close();
        
        int width = 0, height = 0;
        const uint8_t* pixel_data = nullptr;
        int format_type = -1;
        uint32_t gl_format = GL_RGBA;
        uint32_t gl_type = GL_UNSIGNED_BYTE;
        int bpp = 4;
        char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        uint8_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        int caver_format = 1;
        
        const PPDv3Header* v3 = (const PPDv3Header*)file_data.data();
        if (v3->version == 0x03525650) {
            width = v3->width;
            height = v3->height;
            pixel_data = file_data.data() + sizeof(PPDv3Header) + v3->metadata_size;
            format_type = pvr::ParsePVRv3Format(v3->pixel_format, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        } else {
            const PPDv2Header* v2 = (const PPDv2Header*)file_data.data();
            if (v2->magic == 0x21525650 || v2->header_size == 44) {
                width = v2->width;
                height = v2->height;
                pixel_data = file_data.data() + v2->header_size;
                format_type = pvr::ParsePVRv2Format(v2->flags, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
            } else {
                return tex;
            }
        }
        
        if (format_type < 0 || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            return tex;
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
        
        tex.rgba.assign(width * height * 4, 255);
        bool decode_success = false;
        
        if (format_type == 10) {
            size_t bytes_to_copy = std::min((size_t)(width * height * bpp), file_size - (pixel_data - file_data.data()));
            std::memcpy(tex.rgba.data(), pixel_data, bytes_to_copy);
            if (gl_format == GL_BGRA) {
                for (size_t i = 0; i < tex.rgba.size(); i += 4) {
                    uint8_t b = tex.rgba[i];
                    tex.rgba[i] = tex.rgba[i+2];
                    tex.rgba[i+2] = b;
                }
                gl_format = GL_RGBA;
                caver_format = 1;
            }
            decode_success = true;
        } else {
            if (format_type == 1) {
                pvr::PVRTDecompressETC(pixel_data, width, height, tex.rgba.data(), 6);
                decode_success = true;
            } else if (format_type == 2 || format_type == 3) {
                uint32_t do2bitMode = (format_type == 2) ? 1 : 0;
                pvr::PVRTDecompressPVRTC(pixel_data, do2bitMode, width, height, tex.rgba.data());
                decode_success = true;
            } else if (format_type >= 4 && format_type <= 6) {
                uint32_t dxt_fmt = (format_type == 4) ? 1 : ((format_type == 5) ? 3 : 5);
                pvr::PVRTDecompressDXT(pixel_data, width, height, tex.rgba.data(), dxt_fmt);
                decode_success = true;
            }
        }
        
        if (decode_success) {
            tex.width = width;
            tex.height = height;
            tex.format_type = format_type;
            tex.gl_format = gl_format;
            tex.gl_type = gl_type;
            tex.caver_format = caver_format;
            tex.success = true;
        }
        
        return tex;
    });
}

bool RedstellGC::ppd_get_texture(const std::string& path, PPDTexture& out_tex) {
    std::shared_future<PPDTexture> fut;
    {
        std::lock_guard<std::mutex> lock(m_ppd_mutex);
        if (!m_ppd_cache.count(path)) {
            return false;
        }
        fut = m_ppd_cache[path];
    }
    
    PPDTexture tex = fut.get();
    
    {
        std::lock_guard<std::mutex> lock(m_ppd_mutex);
        m_ppd_cache.erase(path);
    }
    
    if (tex.success) {
        out_tex = std::move(tex);
        return true;
    }
    return false;
}

#include <fstream>
#include <algorithm>

void RedstellGC::generate_allocation_report() {
    log("[Redstell GC] Generating active allocations report...");

    struct GroupInfo {
        size_t count = 0;
        size_t total_size = 0;
        std::string symbol;
    };

    std::unordered_map<std::string, GroupInfo> groups;
    size_t total_tracked_size = 0;
    size_t total_tracked_count = 0;

    {
        std::shared_lock<std::shared_mutex> lock(m_allocs_mutex);
        for (const auto& pair : m_allocs) {
            const auto& info = pair.second;
            std::string symbol = resolve_symbol(info.callsite);
            
            auto& g = groups[symbol];
            g.symbol = symbol;
            g.count++;
            g.total_size += info.size;

            total_tracked_size += info.size;
            total_tracked_count++;
        }
    }

    // Sort groups by total size descending
    std::vector<GroupInfo> sorted_groups;
    for (const auto& pair : groups) {
        sorted_groups.push_back(pair.second);
    }
    std::sort(sorted_groups.begin(), sorted_groups.end(), [](const GroupInfo& a, const GroupInfo& b) {
        return a.total_size > b.total_size;
    });

    // Write to a Markdown file in the game directory
    std::ofstream out("rgc_allocation_report.md");
    if (!out) {
        log("[Redstell GC] Error: Failed to open rgc_allocation_report.md for writing.");
        return;
    }

    out << "# Redstell GC — Active Allocations Report\n\n";
    out << "This report lists all currently active guest allocations tracked by RGC, grouped by their allocation callsites.\n\n";
    out << "## Summary\n\n";
    out << "- **Total Active Allocations**: " << total_tracked_count << "\n";
    out << "- **Total Tracked Memory**: " << (double)total_tracked_size / (1024.0 * 1024.0) << " MB (" << total_tracked_size << " bytes)\n\n";
    
    out << "## Breakdown by Callsite\n\n";
    out << "| Rank | Count | Total Size (MB) | Total Size (Bytes) | Callsite / Allocation Origin |\n";
    out << "| :--- | :--- | :-------------- | :----------------- | :-------------------------- |\n";

    int rank = 1;
    for (const auto& g : sorted_groups) {
        out << "| " << rank++ 
            << " | " << g.count 
            << " | " << (double)g.total_size / (1024.0 * 1024.0) 
            << " | " << g.total_size 
            << " | `" << g.symbol << "` |\n";
    }

    out.close();
    log("[Redstell GC] Saved allocation report to rgc_allocation_report.md (%zu callsites).", sorted_groups.size());
}

extern "C" {

void rgc_track_file_open(uint64_t handle, const char* path) {
    RedstellGC::instance().track_file_open(handle, path);
}

void rgc_track_file_close(uint64_t handle) {
    RedstellGC::instance().track_file_close(handle);
}

}
