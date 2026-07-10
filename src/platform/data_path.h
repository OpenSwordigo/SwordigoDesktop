#pragma once
#include <string>

// Resolve a relative path to the game data directory.
// Priority: 
//   1. ~/.local/share/swordigo-desktop/ (user data — writable, Minecraft-style)
//   2. SWORDIGO_DATA_DIR env variable
//   3. ./ (development mode)
//   4. /usr/share/swordigo/ (system install — read-only)
std::string get_data_path(const std::string& relative_path);

// Get the user data directory (~/.local/share/swordigo-desktop/)
// Creates it if it doesn't exist.
std::string get_user_data_dir();

// Get the system install directory (/usr/share/swordigo/)
// Returns empty string if not found.
std::string get_system_data_dir();

// First-run setup: copies game data from system install to user data dir.
// Returns true if data was copied, false if already exists or no system install.
bool ensure_user_data();

#ifdef __cplusplus
void set_active_mod_name(const std::string& name);
void set_active_profile_id(const std::string& id);
#endif

#ifdef __cplusplus
extern "C" {
#endif
char* get_user_data_dir_c(void);
bool resolve_vfs_path(const char* original_path, char* out_resolved_path, int max_len);
#ifdef __cplusplus
}
#endif
