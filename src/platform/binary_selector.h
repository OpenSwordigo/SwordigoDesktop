#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// Global Swordigo ABI version (12 = 1.4.12, 13 = 1.4.13)
extern "C" int swordi_abi;

// Binary version status
enum class BinaryStatus {
    TESTED,     // Known stable
    TESTING,    // User is testing
    UNKNOWN     // Unrecognized hash
};

// CPU architecture
enum class BinaryArch {
    ARM32,      // armeabi-v7a
    ARM64       // arm64-v8a
};

// Info about a detected game binary
struct BinaryInfo {
    std::string filename;       // e.g. "libswordigo.so"
    std::string filepath;       // Full path: "engine/v1.4.12/arm64-v8a/libswordigo.so"
    std::string sha256;         // SHA256 hex string
    std::string version;        // e.g. "1.4.12"
    std::string label;          // e.g. "v1.4.12 [ARM64] (Stable)"
    std::string version_dir;    // e.g. "v1.4.12" or "rl-v6.1"
    BinaryStatus status;
    BinaryArch arch;
    size_t file_size;           // bytes
    bool is_default;
    std::string game_type;      // "Swordigo" or "RLSwordigo"
    std::string assets_dir;     // "assets" or "rl_assets"
    std::string icon_path;      // Custom icon PNG/JPG (empty = use default for game_type)
    std::vector<std::string> dependencies;  // e.g. {"libmini.so", "libGlossHook.so"}
    std::vector<std::string> dep_paths;     // Full paths to dependency .so files
};

// Manages binary detection, hashing, and selection
class BinarySelector {
public:
    BinarySelector();

    // ── JSON Manifest System ──
    // Primary: load from pre-built JSON manifests (no filesystem scanning at boot)
    //   system_manifest = /usr/share/swordigo/engine/manifest.json (shipped with RPM)
    //   user_instances   = ~/.config/swordigo-desktop/instances.json (user-added)
    
    // Load system manifest (read-only, shipped with package)
    void load_manifest(const std::string& manifest_path);
    
    // Load user-added instances (writable, from user config dir)
    void load_user_instances(const std::string& json_path);

    // Rebuild the runtime list from engine/*/*/instance.ini metadata.
    void reload_instances();
    
    // Save user instances (only custom ones, not system ones)
    void save_user_instances(const std::string& json_path) const;
    
    // Set the base data directory for saving instance.ini files
    void set_data_dir(const std::string& dir) { data_dir = dir; }
    std::string get_data_dir() const { return data_dir; }

    // Save a single instance to its local instance.ini file
    void save_instance_ini(const BinaryInfo& b) const;
    
    // Generate manifest.json from current engine/ directory (used by packaging)
    // This does a full scan + hash + writes manifest.json
    void generate_manifest(const std::string& engine_path, const std::string& output_path);

    // ── Legacy Scan (fallback if no manifest exists) ──
    void scan_engine_directory(const std::string& engine_path);
    void scan_directory(const std::string& dir_path);

    // Load/save the binary registry JSON (old format, kept for compat)
    void load_registry(const std::string& json_path);
    void save_registry(const std::string& json_path);

    // Get detected binaries
    const std::vector<BinaryInfo>& get_binaries() const { return binaries; }

    // Get/set default binary
    std::string get_default() const { return default_binary; }
    void set_default(const std::string& filepath);

    // Get info for the currently loaded binary
    const BinaryInfo* get_loaded_info() const;
    void set_loaded(const std::string& filepath);

    // Remove SRE-conflicting dependencies from a newly-created instance.
    void strip_sre_conflicts_from_last();

    // Add a custom binary instance from a .so file
    // Copies the .so into engine/custom-NAME/<arch>/ and registers it
    bool add_custom_instance(const std::string& so_filepath, const std::string& name, const std::string& assets_dir);

    // Import an Android APK as a complete self-contained instance. Extracts
    // game assets and every native ABI, registers each libswordigo.so, copies
    // companion libraries, and writes instance.ini metadata.
    bool import_apk_instance(const std::string& apk_path, const std::string& name,
                             std::string* error_message = nullptr);

    // Remove an instance by index from the in-memory list
    void remove_instance(int index) {
        if (index >= 0 && index < (int)binaries.size()) {
            binaries.erase(binaries.begin() + index);
        }
    }

    // Strip libmini.so and libGlossHook.so from an instance's dependencies
    // (SRE replaces their functionality natively)
    void strip_sre_conflicts(int index) {
        if (index < 0 || index >= (int)binaries.size()) return;
        auto& b = binaries[index];
        b.dependencies.erase(
            std::remove_if(b.dependencies.begin(), b.dependencies.end(),
                [](const std::string& d) {
                    return d == "libmini.so" || d == "libGlossHook.so" || d == "libkiwi.so";
                }), b.dependencies.end());
        b.dep_paths.erase(
            std::remove_if(b.dep_paths.begin(), b.dep_paths.end(),
                [](const std::string& p) {
                    return p.find("libmini.so") != std::string::npos ||
                           p.find("libGlossHook.so") != std::string::npos ||
                           p.find("libkiwi.so") != std::string::npos;
                }), b.dep_paths.end());
    }

    // Should we show the selector? (more than 1 binary found)
    bool should_show_selector() const { return binaries.size() > 1; }

    // Selection state for the boot GUI
    int selected_index = 0;

    // Compute SHA256 of a file
    static std::string compute_sha256(const std::string& filepath);

    // Get arch string for display
    static const char* arch_string(BinaryArch arch) {
        return (arch == BinaryArch::ARM64) ? "ARM64" : "ARM32";
    }

private:
    std::vector<BinaryInfo> binaries;
    std::string default_binary;
    std::string loaded_binary;
    std::string data_dir;  // Base directory for saving instance.ini files

    // Known binary hashes (hardcoded fallbacks)
    static const std::map<std::string, std::pair<std::string, BinaryStatus>> KNOWN_HASHES;

    // Parse a version directory entry (used by scan + manifest generation)
    void scan_version_dir(const std::string& engine_path, const std::string& version_dir);
    void scan_arch_dir(const std::string& arch_path, const std::string& version_dir, BinaryArch arch);
    
    // JSON helpers
    static BinaryInfo parse_instance_json(const std::string& json_block);
    static std::string instance_to_json(const BinaryInfo& info, int indent = 4);
};
