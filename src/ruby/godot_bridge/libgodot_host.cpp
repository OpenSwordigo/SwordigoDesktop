// ============================================================================
// libgodot_host.cpp — Ruby ↔ Godot bridge (lifecycle only)
//
// Loads libgodot.so and resolves lifecycle functions.
// Ruby owns the main loop. Editor shell is in GDScript (shim project).
// ============================================================================

#include "libgodot_host.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <linux/limits.h>

namespace ruby { namespace godot {
extern "C" int swordigo_load_pod(const char*);
extern "C" int swordigo_load_scene(const char*);

using FnSetup    = int  (*)(int argc, char** argv);
using FnSetup2   = int  (*)();
using FnStart    = int  (*)();
using FnIteration = bool (*)();
using FnCleanup  = void (*)();

static void*       s_lib_handle = nullptr;
static FnSetup     s_fn_setup    = nullptr;
static FnSetup2    s_fn_setup2   = nullptr;
static FnStart     s_fn_start    = nullptr;
static FnIteration s_fn_iter     = nullptr;
static FnCleanup   s_fn_cleanup  = nullptr;

static bool s_engine_started = false;

bool godot_host_init(const GodotHostConfig& /*config*/)
{
    // Find executable directory for library search
    char exe_dir[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char* last = strrchr(exe_dir, '/');
        if (last) *last = '\0';
    } else {
        strcpy(exe_dir, ".");
    }

    std::string exe_lib = std::string(exe_dir) + "/libs/libgodot.so";
    std::string cwd_lib = "bin/libs/libgodot.so";
    std::string direct  = "./libgodot.so";

    const char* paths[] = { exe_lib.c_str(), cwd_lib.c_str(), direct.c_str(), nullptr };

    for (const char** p = paths; *p; ++p) {
        s_lib_handle = dlopen(*p, RTLD_NOW | RTLD_GLOBAL);
        if (s_lib_handle) {
            fprintf(stderr, "[ruby_gg] Loaded libgodot from: %s\n", *p);
            break;
        }
    }

    if (!s_lib_handle) {
        fprintf(stderr, "[ruby_gg] ERROR: Could not load libgodot.so: %s\n", dlerror());
        return false;
    }

    s_fn_setup   = reinterpret_cast<FnSetup>(dlsym(s_lib_handle, "ruby_gg_setup"));
    s_fn_setup2  = reinterpret_cast<FnSetup2>(dlsym(s_lib_handle, "ruby_gg_setup2"));
    s_fn_start   = reinterpret_cast<FnStart>(dlsym(s_lib_handle, "ruby_gg_start"));
    s_fn_iter    = reinterpret_cast<FnIteration>(dlsym(s_lib_handle, "ruby_gg_iteration"));
    s_fn_cleanup = reinterpret_cast<FnCleanup>(dlsym(s_lib_handle, "ruby_gg_cleanup"));

    if (!s_fn_setup || !s_fn_setup2 || !s_fn_start || !s_fn_iter || !s_fn_cleanup) {
        fprintf(stderr, "[ruby_gg] ERROR: Missing lifecycle symbols\n");
        return false;
    }

    fprintf(stderr, "[ruby_gg] libgodot.so loaded\n");
    return true;
}

std::vector<const char*> build_argv(const GodotHostConfig& config)
{
    std::vector<const char*> argv;
    argv.push_back("ruby_gg");
    argv.push_back("--path");
    argv.push_back(config.shim_project_path.c_str());
    for (const auto& arg : config.extra_args) {
        argv.push_back(arg.c_str());
    }
    return argv;
}

GodotInstanceHandle godot_host_create_and_start(const GodotHostConfig& config)
{
    GodotInstanceHandle handle;
    if (!s_fn_setup) return handle;

    auto argv_ptrs = build_argv(config);
    int argc = static_cast<int>(argv_ptrs.size());
    std::vector<char*> argv_mutable;
    for (int i = 0; i < argc; ++i)
        argv_mutable.push_back(const_cast<char*>(argv_ptrs[i]));

    fprintf(stderr, "[ruby_gg] Main::setup(%d args)...\n", argc);
    if (s_fn_setup(argc, argv_mutable.data()) != 0) return handle;

    fprintf(stderr, "[ruby_gg] Main::setup2()...\n");
    if (s_fn_setup2() != 0) return handle;

    fprintf(stderr, "[ruby_gg] Main::start()...\n");
    if (s_fn_start() != 0) return handle;

    s_engine_started = true;
    handle.godot_instance = reinterpret_cast<void*>(1);
    handle.started = true;
    fprintf(stderr, "[ruby_gg] Engine started!\n");
    // Wire the Swordigo backend to the RubyBackend singleton in Godot
    auto s_fn_register = reinterpret_cast<void (*)(void*, void*)>(
        dlsym(s_lib_handle, "ruby_gg_register_backend"));
    if (s_fn_register) {
        fprintf(stderr, "[ruby_gg] Registering Swordigo backend...\n");
        s_fn_register((void*)swordigo_load_pod, (void*)swordigo_load_scene);
        fprintf(stderr, "[ruby_gg] Backend registered!\n");
    } else {
        fprintf(stderr, "[ruby_gg] WARNING: ruby_gg_register_backend not found\n");
    }
    return handle;
}

bool godot_host_iteration(GodotInstanceHandle& handle)
{
    if (!s_engine_started || !s_fn_iter) return false;
    return s_fn_iter();
}

void godot_host_shutdown(GodotInstanceHandle& handle)
{
    if (!s_engine_started) return;
    fprintf(stderr, "[ruby_gg] Shutting down...\n");
    if (s_fn_cleanup) s_fn_cleanup();
    s_engine_started = false;
    handle.godot_instance = nullptr;
    handle.started = false;
}

}} // namespace ruby::godot
