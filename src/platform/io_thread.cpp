#include "platform/io_thread.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
#include <sys/stat.h>   // for mkdir()

struct IOJob {
    enum Type { SAVE, LOAD };
    Type type;
    std::string path;
    std::vector<uint8_t> data;
    IOLoadCallback load_callback;
};

struct IOResult {
    bool ok;
    std::vector<uint8_t> data;
    IOLoadCallback load_callback;
};

static std::thread g_io_thread;
static std::mutex g_work_mutex;
static std::condition_variable g_work_cv;
static std::queue<IOJob> g_work_queue;
static std::atomic<bool> g_io_running(false);

static std::mutex g_results_mutex;
static std::queue<IOResult> g_results_queue;

static void io_thread_loop() {
    while (g_io_running) {
        IOJob job;
        {
            std::unique_lock<std::mutex> lock(g_work_mutex);
            g_work_cv.wait(lock, [] { return !g_work_queue.empty() || !g_io_running; });
            if (!g_io_running && g_work_queue.empty()) {
                break;
            }
            job = std::move(g_work_queue.front());
            g_work_queue.pop();
        }

        if (job.type == IOJob::SAVE) {
            // Auto-create parent directories so paths like
            // save/snapshots/ss.bin succeed even if the snapshots/ dir
            // was never explicitly created (e.g. first boot).
            {
                std::string dir = job.path;
                auto pos = dir.find_last_of('/');
                if (pos != std::string::npos) {
                    dir = dir.substr(0, pos);
                    // Walk up and create each component
                    for (size_t i = 1; i <= dir.size(); ++i) {
                        if (i == dir.size() || dir[i] == '/') {
                            std::string component = dir.substr(0, i);
                            // mkdir() is idempotent on existing dirs
#ifdef _WIN32
                            ::mkdir(component.c_str());
#else
                            ::mkdir(component.c_str(), 0755);
#endif
                        }
                    }
                }
            }
            FILE* f = fopen(job.path.c_str(), "wb");
            if (f) {
                if (!job.data.empty()) {
                    fwrite(job.data.data(), 1, job.data.size(), f);
                }
                fflush(f);
                fclose(f);
                std::cout << "[IO Async] Successfully wrote " << job.data.size() << " bytes to " << job.path << std::endl;
            } else {
                std::cerr << "[IO Async] Failed to open " << job.path << " for writing" << std::endl;
            }
        } else if (job.type == IOJob::LOAD) {
            IOResult res;
            res.ok = false;
            res.load_callback = job.load_callback;
            
            FILE* f = fopen(job.path.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz >= 0) {
                    res.data.resize(sz);
                    if (sz > 0) {
                        fread(res.data.data(), 1, sz, f);
                    }
                    res.ok = true;
                    std::cout << "[IO Async] Successfully read " << sz << " bytes from " << job.path << std::endl;
                }
                fclose(f);
            } else {
                std::cout << "[IO Async] Failed to open " << job.path << " for reading" << std::endl;
            }

            {
                std::lock_guard<std::mutex> lock(g_results_mutex);
                g_results_queue.push(std::move(res));
            }
        }
    }
}

void io_thread_start() {
    if (g_io_running) return;
    g_io_running = true;
    g_io_thread = std::thread(io_thread_loop);
    std::cout << "[IO Async] Background thread started" << std::endl;
}

void io_thread_stop() {
    if (!g_io_running) return;
    g_io_running = false;
    g_work_cv.notify_all();
    if (g_io_thread.joinable()) {
        g_io_thread.join();
    }
    std::cout << "[IO Async] Background thread stopped" << std::endl;
}

void io_thread_post_save(const std::string& path, std::vector<uint8_t> data) {
    IOJob job;
    job.type = IOJob::SAVE;
    job.path = path;
    job.data = std::move(data);
    {
        std::lock_guard<std::mutex> lock(g_work_mutex);
        g_work_queue.push(std::move(job));
    }
    g_work_cv.notify_one();
}

void io_thread_post_load(const std::string& path, IOLoadCallback callback) {
    IOJob job;
    job.type = IOJob::LOAD;
    job.path = path;
    job.load_callback = callback;
    {
        std::lock_guard<std::mutex> lock(g_work_mutex);
        g_work_queue.push(std::move(job));
    }
    g_work_cv.notify_one();
}

void io_thread_poll() {
    std::queue<IOResult> local_queue;
    {
        std::lock_guard<std::mutex> lock(g_results_mutex);
        std::swap(local_queue, g_results_queue);
    }
    while (!local_queue.empty()) {
        IOResult res = std::move(local_queue.front());
        local_queue.pop();
        if (res.load_callback) {
            res.load_callback(res.ok, std::move(res.data));
        }
    }
}
