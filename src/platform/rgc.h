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
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>

// Resource Types tracked by RGC
enum class RgcResourceType {
    UNKNOWN = 0,
    MEMORY,
    TEXTURE,
    PVR_IMAGE,
    POD_MODEL,
    SCENE_OBJECT,
    SHADER,
    LUA_OBJECT,
    LUA_STATE,
    AUDIO,
    FRAMEBUFFER,
    RENDER_TARGET,
    OPENGL_OBJECT,
    FILE_BUFFER,
    NETWORK_BUFFER,
    TEMPORARY_BUFFER,
    VFS_HANDLE,
    COUNT
};

// Return human-readable name of a resource type
const char* rgc_resource_type_name(RgcResourceType type);

// Metadata for a guest memory allocation
struct RgcAllocInfo {
    uint64_t address;
    size_t size;
    uint64_t timestamp; // Microseconds since boot
    RgcResourceType type;
    std::string subsystem;
    uint64_t thread_id; // Host thread representation
    std::string reason;
    uint64_t callsite;  // Link register (LR) caller address
    std::string resolved_callsite; // Resolved function/symbol if available
};

// Metadata for a tracked high-level resource
struct RgcResourceNode {
    uint64_t address;
    RgcResourceType type;
    std::string name;
    int ref_count;
    uint64_t creation_time;
    std::unordered_set<std::string> referencing_subsystems;
    std::vector<uint64_t> dependencies; // Dependent resource addresses
    bool marked_for_cleanup;
};

// Circular log entry for debug overlay display
struct RgcLogEntry {
    uint64_t timestamp; // ms since boot
    std::string message;
};

// Thread-safe Job Queue for background work
struct RgcCleanupJob {
    uint64_t address;
    RgcResourceType type;
    std::string name;
};

// Redstell Garbage Collector subsystem coordinator
class RedstellGC {
public:
    static RedstellGC& instance();

    // Subsystem Lifecycle
    void init();
    void shutdown();

    // Memory Tracking API
    void track_alloc(uint64_t addr, size_t size, uint64_t lr, const char* reason = nullptr, RgcResourceType type = RgcResourceType::MEMORY);
    void track_free(uint64_t addr, uint64_t lr = 0);
    bool is_tracked(uint64_t addr);

    // Resource reference tracking API
    void track_resource(uint64_t addr, RgcResourceType type, const char* name);
    void add_resource_ref(uint64_t addr, const char* subsystem = "Engine");
    void release_resource_ref(uint64_t addr);
    void register_dependency(uint64_t parent_addr, uint64_t child_addr);

    // VFS / File buffer API
    void track_file_open(uint64_t handle, const char* path);
    void track_file_close(uint64_t handle);
    std::string get_file_path(uint64_t handle);

    // OpenGL interception API
    void track_gl_gen_texture(uint32_t tex_id);
    void track_gl_delete_texture(uint32_t tex_id);
    void track_gl_tex_image(uint32_t tex_id, int width, int height, int internal_format, const char* asset_name, const void* pixels, size_t pixels_size);
    uint32_t get_redirected_texture(uint32_t tex_id);

    // OpenAL audio tracking API
    void track_al_gen_source(uint32_t source_id);
    void track_al_delete_source(uint32_t source_id);
    void track_al_gen_buffer(uint32_t buffer_id);
    void track_al_delete_buffer(uint32_t buffer_id);

    // Lua state API
    void register_lua_state(void* L);
    void unregister_lua_state(void* L);

    void process_main_thread_deletions();
    void flush_thread_local_queues();
    void process_event_queue();
    void generate_allocation_report();

    // Diagnostics & Overlay Logging
    void log(const char* fmt, ...);
    std::vector<RgcLogEntry> get_logs();
    
    // Predictive Parallel Decompression (PPD) API
    struct PPDTexture {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        int format_type = -1;
        uint32_t gl_format = 0;
        uint32_t gl_type = 0;
        int caver_format = 1;
        bool success = false;
    };
    void ppd_preload_texture(const std::string& path);
    bool ppd_get_texture(const std::string& path, PPDTexture& out_tex);

    // Statistics queries
    size_t get_current_ram() const { return m_current_ram.load(); }
    size_t get_peak_ram() const { return m_peak_ram.load(); }
    size_t get_largest_allocation() const { return m_largest_alloc.load(); }
    float get_fragmentation_estimate() const;
    void get_allocation_histogram(uint64_t* out_buckets) const;

    uint64_t get_alloc_rate() const { return m_alloc_rate.load(); }
    uint64_t get_free_rate() const { return m_free_rate.load(); }
    uint64_t get_resource_count() const;
    uint32_t get_texture_count() const { return m_texture_count.load(); }
    uint32_t get_pod_count() const { return m_pod_count.load(); }
    uint32_t get_lua_object_count() const { return m_lua_object_count.load(); }
    uint32_t get_opengl_count() const { return m_opengl_count.load(); }
    uint32_t get_open_file_count() const { return m_open_file_count.load(); }
    uint32_t get_openal_count() const { return m_openal_count.load(); }

    uint32_t get_cleanup_queue_size();
    uint32_t get_worker_threads_count() const { return 4; } // Hardcoded pool count
    uint32_t get_pending_resources_count();
    uint64_t get_optimizations_performed() const { return m_optimizations_performed.load(); }
    double get_last_cleanup_duration() const { return m_last_cleanup_duration.load(); }

private:
    RedstellGC();
    ~RedstellGC();

    // Callsite symbol resolution helper
    std::string resolve_symbol(uint64_t address);

    // Background Thread entries
    void gc_thread_func();
    void opt_thread_func();
    void stats_thread_func();
    void worker_thread_func(int id);

    // Allocations tracker
    std::unordered_map<uint64_t, RgcAllocInfo> m_allocs;
    mutable std::shared_mutex m_allocs_mutex;

    // High level resource lifetime registry
    std::unordered_map<uint64_t, RgcResourceNode> m_resources;
    mutable std::shared_mutex m_resources_mutex;

    // VFS handle registry
    std::unordered_map<uint64_t, std::string> m_file_handles;
    mutable std::shared_mutex m_files_mutex;

    // OpenGL texture metadata & duplicate merge map
    struct TextureMetadata {
        uint32_t tex_id;
        int width;
        int height;
        int internal_format;
        std::string asset_name;
        uint32_t pixel_hash;
        bool is_duplicate;
        uint32_t redirect_target;
    };
    std::unordered_map<uint32_t, TextureMetadata> m_textures;
    std::unordered_map<uint32_t, uint32_t> m_texture_redirects;
    mutable std::shared_mutex m_textures_mutex;

    // OpenAL registries
    std::unordered_set<uint32_t> m_al_sources;
    std::unordered_set<uint32_t> m_al_buffers;
    mutable std::shared_mutex m_openal_mutex;

    // Lua registries
    std::unordered_set<void*> m_lua_states;
    mutable std::shared_mutex m_lua_mutex;

    // Main thread queued deletions
    std::vector<uint32_t> m_queued_gl_texture_deletions;
    std::mutex m_deletions_mutex;

    // Job queue for asynchronous finalizers
    std::queue<RgcCleanupJob> m_cleanup_queue;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;

    // Asynchronous Alloc/Free Double Buffer Queue
public:
    struct RgcEvent {
        enum Type { ALLOC, FREE };
        Type type;
        uint64_t addr;
        size_t size;
        uint64_t lr;
        RgcResourceType r_type;
        const char* reason;
    };
private:
    std::vector<RgcEvent> m_write_queue;
    std::mutex m_queue_swap_mutex;

    // Logs buffer
    std::vector<RgcLogEntry> m_log_buffer;
    mutable std::mutex m_log_mutex;
    static constexpr size_t MAX_LOG_SIZE = 100;

    // Metrics & Performance statistics counters
    std::atomic<bool> m_running;
    std::atomic<size_t> m_current_ram;
    std::atomic<size_t> m_peak_ram;
    std::atomic<size_t> m_largest_alloc;
    std::atomic<uint64_t> m_alloc_buckets[10]; // Histogram buckets

    std::atomic<uint64_t> m_alloc_counter;
    std::atomic<uint64_t> m_free_counter;
    std::atomic<uint64_t> m_alloc_rate;
    std::atomic<uint64_t> m_free_rate;

    std::atomic<uint32_t> m_texture_count;
    std::atomic<uint32_t> m_pod_count;
    std::atomic<uint32_t> m_lua_object_count;
    std::atomic<uint32_t> m_opengl_count;
    std::atomic<uint32_t> m_open_file_count;
    std::atomic<uint32_t> m_openal_count;

    std::atomic<uint64_t> m_optimizations_performed;
    std::atomic<double> m_last_cleanup_duration;

    // Background Threads
    std::thread m_gc_thread;
    std::thread m_opt_thread;
    std::thread m_stats_thread;
    std::vector<std::thread> m_workers;

    // PPD Cache Registry
    std::unordered_map<std::string, std::shared_future<PPDTexture>> m_ppd_cache;
    mutable std::mutex m_ppd_mutex;

    // Time baseline
    std::chrono::steady_clock::time_point m_boot_time;

    // Rendering path performance optimization & thread check
    std::atomic<bool> m_has_redirects;
    std::thread::id m_main_thread_id;
};

#ifdef __cplusplus
extern "C" {
#endif

void rgc_track_file_open(uint64_t handle, const char* path);
void rgc_track_file_close(uint64_t handle);

#ifdef __cplusplus
}
#endif
