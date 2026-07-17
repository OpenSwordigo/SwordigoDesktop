// ImGui-based launcher UI for Swordigo Desktop
// Replaces the old raw-OpenGL launcher with a full Dear ImGui implementation.
// SDL3 + OpenGL 3.3 Core + ImGui v1.91.x

#pragma once

#include "platform/vulkan_backend.h"   // for GraphicsAPI enum
#include "platform/binary_selector.h"  // for BinarySelector, BinaryInfo
#include <string>

struct LaunchConfig {
    GraphicsAPI graphics_api = GraphicsAPI::OPENGL;
    std::string selected_binary = "engine/v1.4.12/armeabi-v7a/libswordigo.so";
    std::string assets_dir = "assets";       // "assets" for vanilla, "rl_assets" for RLSwordigo
    std::string game_type = "Swordigo";      // "Swordigo" or "RLSwordigo"
    bool should_launch = true;  // false if user closed the launcher
    bool use_dynarmic = false;  // false = Unicorn (default), true = Dynarmic JIT
    bool use_sre = true;        // whether to load libsre.so (user choice)
    bool advanced_redstell_opts = false; // Advanced Redstell Optimisations (off by default)
};

// Show the unified launcher window and block until user clicks Launch or closes.
// Creates its own SDL_Window + GL context + ImGui context, cleans up before returning.
// If selector has binaries, they are shown as a list; otherwise that section is hidden.
LaunchConfig show_launcher(BinarySelector& selector);
