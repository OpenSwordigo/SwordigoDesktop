/* batch_converter.h — Ruby Batch Texture Converter
 *
 * Provides a professional, threaded batch conversion pipeline:
 *   - PVR / tex.png  →  .pvr.png / .tex.png.png  (Export to editable PNG)
 *   - .pvr.png / .tex.png.png  →  PVR / tex.png  (Import back as game assets)
 *
 * Design:
 *   - Conversion runs on a dedicated std::thread to keep the UI at 60fps
 *   - Progress, per-file status, timing, and error messages are reported via
 *     an atomic-/mutex-protected log vector that the UI reads each frame.
 *   - Export: use pvr_decode_to_rgba() then stbi_write_png().
 *   - Import: call PVRTexToolCLI (external binary) for compression, then
 *     optional gzip-wrap for .tex.png output.
 */

#pragma once
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

namespace batch {

// ─── Log entry ───────────────────────────────────────────────────────────────
enum class LogLevel { INFO, OK, WARN, ERR };

struct LogEntry {
    LogLevel    level;
    std::string text;
    double      elapsed_ms; // wall time from job start, or -1
};

// ─── Per-file result ──────────────────────────────────────────────────────────
struct FileResult {
    std::string src_path;
    std::string dst_path;
    bool        success;
    std::string error_msg;
    double      duration_ms;
    int         src_w, src_h;   // source texture dimensions (if known)
    size_t      src_bytes;      // input file size in bytes
    size_t      dst_bytes;      // output file size in bytes
};

// ─── Conversion mode ──────────────────────────────────────────────────────────
enum class Mode { EXPORT_TO_PNG = 0, IMPORT_TO_GAME };

// ─── Compression format (for import mode) ─────────────────────────────────────
enum class CompressFmt { ETC1 = 0, PVRTC_4BPP, PVRTC_2BPP, RGBA8888 };

// ─── Batch Converter State ────────────────────────────────────────────────────
struct BatchState {
    // ── UI-editable fields (only touched when thread NOT running) ──
    char    src_dir[4096]  = {};
    char    dst_dir[4096]  = {};
    Mode    mode           = Mode::EXPORT_TO_PNG;

    // Export format filter checkboxes
    bool    filter_pvr     = true;
    bool    filter_texpng  = true;
    bool    filter_pvrpng  = true;  // for import mode: select .pvr.png files
    bool    filter_texppng = true;  // for import mode: select .tex.png.png files

    // Import advanced options
    CompressFmt compress_fmt = CompressFmt::ETC1;
    char    pvrtextool_path[4096] =
        "/run/media/quantumcreeper/TVPG/linuxFiles/FEDORA_DOWNS/"
        "PVRTexTool_linux@57c209a94ba7ae6524b15b779f167b48ed360d07/"
        "CLI/Linux_x86_64/PVRTexToolCLI";

    bool    recurse_subdirs = true;
    bool    skip_existing   = false;

    // ── Modal open flag ──
    bool    open_window = false;
    bool    show_advanced = false;

    // ── Thread state (atomic, safe to read from UI thread) ──
    std::atomic<bool>   running {false};
    std::atomic<int>    total_files {0};
    std::atomic<int>    done_files  {0};
    std::atomic<int>    ok_count    {0};
    std::atomic<int>    err_count   {0};
    std::atomic<bool>   cancel_flag {false};
    std::atomic<bool>   finished    {false};  // set true when thread exits naturally

    // Current file being processed (updated from worker thread)
    std::mutex              log_mutex;
    std::vector<LogEntry>   log;           // all log entries
    std::vector<FileResult> results;       // per-file results
    std::string             current_file;  // basename of file being processed NOW
    std::map<std::string, std::string> metadata_export; // rel_path -> metadata string

    double job_start_wall = 0.0;           // SDL_GetTicks64 at job start / 1000.0
    double job_end_wall   = 0.0;

    std::thread worker;

    // Helper: push a log entry (thread-safe)
    void push_log(LogLevel lv, const std::string& msg, double elapsed_ms = -1.0) {
        std::lock_guard<std::mutex> lk(log_mutex);
        log.push_back({lv, msg, elapsed_ms});
    }
};

// ─── Public API ───────────────────────────────────────────────────────────────

/* Called once per frame from the main render loop.
 * Opens / draws the Batch Converter modal window.
 * Pass the global SDL_GetTicks64() / 1000.0 as `now_sec`. */
void draw_batch_converter(BatchState& bs, double now_sec);

/* Start an export (game formats → PNG) or import (PNG → game formats) job.
 * Spawns a background std::thread.  No-op if already running. */
void start_batch_job(BatchState& bs, double now_sec);

/* Request cancellation of a running job (sets cancel_flag). */
void cancel_batch_job(BatchState& bs);

/* Join the worker thread (called on app exit). */
void shutdown_batch(BatchState& bs);

} // namespace batch
