// ============================================================================
// launcher_config.h — Persistent launcher configuration (TOML-based).
//
// Stores:
//   - Accent color (RGBA float)
//   - Profile name & avatar path (future)
//   - All settings tabs (audio volumes, postfx, slideshow, etc.)
//   - VFS mod load order (priority stack: top = highest override priority)
//
// File location: <user_config>/launcher.toml
// Format: minimal TOML (sections + key=value, no arrays-of-tables).
// ============================================================================

#pragma once

#include <string>
#include <vector>

struct LauncherConfig {
    // ── Appearance ──────────────────────────────────────────────────────────
    float accent_r = 0.55f, accent_g = 0.30f, accent_b = 0.89f, accent_a = 1.0f;

    // ── Profile (future) ────────────────────────────────────────────────────
    std::string profile_name;       // display name (e.g. "Quantum")
    std::string profile_avatar;     // path to avatar image (future)

    // ── Audio ───────────────────────────────────────────────────────────────
    float master_volume = 1.0f;
    float music_volume  = 0.85f;
    float sfx_volume    = 1.0f;

    // ── Graphics ────────────────────────────────────────────────────────────
    bool  postfx_enabled       = true;
    bool  pvr_software_decode  = false;

    // ── Loading Screen ──────────────────────────────────────────────────────
    bool  loading_slideshow    = false;

    // ── VFS Mod Load Order ──────────────────────────────────────────────────
    //  Top of list = highest priority (overrides everything below).
    //  Empty list = vanilla (no mods). Only IDs here; on-disk disabled
    //  mods (dot-prefixed dirs) are tracked but never in this list.
    //
    //  Example: ["com.td.remastered", "com.swordiforge.reimagined"]
    //    → com.td.remastered resources override swordiforge,
    //    → both override vanilla.
    std::vector<std::string> mod_load_order;
};

// ── Load / Save ────────────────────────────────────────────────────────────
// Returns the full path of the config file.
std::string launcher_config_path();

// Load config from disk. If file doesn't exist, returns defaults.
LauncherConfig launcher_config_load();

// Save config to disk. Creates parent dirs if needed.
bool launcher_config_save(const LauncherConfig& cfg);

// ── Helpers ────────────────────────────────────────────────────────────────
// Sanitize a mod ID for the load order list (remove disabled/dot-prefixed).
bool launcher_config_is_mod_enabled(const LauncherConfig& cfg, const std::string& mod_id);

// Add a mod to the load order (at the top = highest priority).
// Returns true if the mod was added (wasn't already there).
bool launcher_config_enable_mod(LauncherConfig& cfg, const std::string& mod_id);

// Remove a mod from the load order (disables it).
bool launcher_config_disable_mod(LauncherConfig& cfg, const std::string& mod_id);

// Move a mod up or down in the load order. Positive = toward top (higher priority).
bool launcher_config_reorder_mod(LauncherConfig& cfg, const std::string& mod_id, int delta);

// Get the mod ID at a given priority index (0 = highest priority).
// Returns empty string if index is out of range.
const std::string& launcher_config_get_mod_at(const LauncherConfig& cfg, int index);

// Number of mods in the load order.
int launcher_config_mod_count(const LauncherConfig& cfg);
