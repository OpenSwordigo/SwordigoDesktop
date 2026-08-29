#pragma once
// ============================================================================
// libgodot_host.h — Thin bridge over Godot's libgodot shared library API.
//
// ruby_gg links against libgodot.so and uses these helpers to:
//   1. Create a Godot editor instance (with a shim project + CLI args)
//   2. Start the editor (Main::setup2 + Main::start)
//   3. Drive frame iteration (Main::iteration)
//   4. Shut down cleanly (Main::cleanup)
//
// The upstream API lives in <libgodot.h> and uses a GDExtension callback;
// these helpers provide a simpler host-side interface for Ruby's use case.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ruby { namespace godot {

// ---- Configuration (set before calling init) --------------------------------
struct GodotHostConfig {
    // Path to the shim project directory (res/godot_shim/)
    std::string shim_project_path;

    // Swordigo workspace root (where .scene/.pvr/.pod files live)
    std::string workspace_path;

    // Additional CLI args forwarded to Godot's Main::setup()
    std::vector<std::string> extra_args;

    // Window title
    std::string window_title = "Swordigo Studio";
};

// ---- Instance handle --------------------------------------------------------
struct GodotInstanceHandle {
    void* godot_instance = nullptr;  // opaque GDExtensionObjectPtr
    bool  started       = false;
};

// ---- API -------------------------------------------------------------------

// Initialize the Godot library.  Must be called once before any other function.
// Returns true on success.
bool godot_host_init(const GodotHostConfig& config);

// Create and start the Godot editor instance.
// Returns a handle; on failure, handle.godot_instance == nullptr.
GodotInstanceHandle godot_host_create_and_start(const GodotHostConfig& config);

// Run one frame of the editor loop.
// Returns true while the editor wants to keep running.
bool godot_host_iteration(GodotInstanceHandle& handle);

// Shut down and destroy the Godot instance.
void godot_host_shutdown(GodotInstanceHandle& handle);

// ---- Helpers ----------------------------------------------------------------

// Build the full CLI argument list from a config.
// Internally prepends ["ruby_gg", "--editor", "--path", shim_project_path, ...]
std::vector<const char*> build_argv(const GodotHostConfig& config);

}} // namespace ruby::godot
