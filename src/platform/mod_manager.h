// ============================================================================
// mod_manager.h — Raijin-compatible mod manager for the Swordfare launcher.
//
// Full parity with Raijin's lawncher mod API (permission granted):
//   zip layout:  icon.png  properties.toml  resources/
//   properties.toml:  [mod]  id= name= version= author= category= description= screenshots=
//   on-disk:     mods/<id>/icon.png  mods/<id>/properties.toml  mods/<id>/resources/...
//
// The host-side VFS (data_path.cpp 5-level hierarchy) already resolves
// mods/<mod>/resources/<path>, so installing a Raijin zip "just works" —
// no game-side changes needed.
// ============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace modman {

// ----------------------------------------------------------------------------
// Installed-mod metadata (superset of both properties.toml and legacy mod.json)
// ----------------------------------------------------------------------------
struct ModMeta {
    std::string id;            // sanitized dir name == mod id
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string category;      // properties.toml category (default "General")
    std::string type;          // legacy mod.json "type" field
    std::vector<std::string> screenshots;
    std::string dir_path;      // absolute path of the mod folder
    std::string icon_path;     // absolute path to icon.png if present
    bool        enabled = true;
    bool        is_toml  = false;  // true if properties.toml based (Raijin), false if mod.json legacy
};

// List installed mods under mods_dir. Directories starting with '.' are
// reported as disabled. Both properties.toml (primary) and mod.json (legacy)
// are understood. Sorted by name.
std::vector<ModMeta> list_mods(const std::string& mods_dir);

// Install a Raijin-format mod zip into mods_dir. Validates properties.toml +
// [mod] id, sanitizes the id, extracts resources/ + icon.png + properties.toml
// into mods/<id>/. On success returns true and fills *out (dir_path set).
// On failure returns false and sets *err.
bool install_mod_zip(const std::string& zip_path, const std::string& mods_dir,
                     ModMeta* out, std::string* err);

// Delete a mod's whole directory tree. Returns false if it couldn't be fully removed.
bool delete_mod(const ModMeta& mod);

// Toggle a mod's enabled state by renaming the folder to/from a leading dot.
bool set_mod_enabled(const std::string& dir_path, bool enabled);

// ----------------------------------------------------------------------------
// Store catalog (Raijin store.json format)
// ----------------------------------------------------------------------------
struct StoreMod {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string long_description;
    std::string category = "General";
    std::string download_url;   // camelCase "downloadUrl" in the live Raijin store
    std::string icon_url;       // "icon" — remote PNG URL in the live Raijin store
    long long   price_cents = 0;
    std::string currency = "USD";
    long long   size_bytes = 0;
    long long   installs = 0;
    double      rating = 0;
    bool        featured = false;
    std::vector<std::string> screenshots;
    std::vector<std::string> tags;
};

// Parse a store document. Accepts BOTH formats:
//   1. The live Raijin store — a top-level JSON array of mod objects with
//      camelCase keys: id/name/author/version/description/icon/downloadUrl.
//   2. The legacy demo catalog object: {"version":N,"categories":[...],
//      "mods":[...]} with snake_case keys (download_url).
// Malformed entries are skipped. Returns the parsed list.
std::vector<StoreMod> parse_catalog(const std::string& json);

// True if the document looks like the live Raijin store format (top-level array).
bool is_raijin_store_format(const std::string& json);

// Determine which catalog entries are already installed (id matches a dir in mods_dir).
std::vector<bool> catalog_installed_mask(const std::vector<StoreMod>& mods,
                                         const std::string& mods_dir);

// ----------------------------------------------------------------------------
// HTTP client. Supports http:// AND https:// by dlopen'ing libcurl at runtime
// (libcurl handles TLS, redirects, gzip and HTTP/1.1 transparently; no build
// time dependency, and falls back to a tiny raw-socket client for http:// when
// libcurl is unavailable). Streams response body to *out with progress.
// ----------------------------------------------------------------------------
bool http_get(const std::string& url, std::string* out,
              void (*progress)(void* user, long long done, long long total) = nullptr,
              void* progress_user = nullptr,
              long timeout_ms = 30000,
              std::string* err = nullptr);

// Download a URL to a local file (streams, reports progress). Supports https://.
bool http_download(const std::string& url, const std::string& dest_path,
                   void (*progress)(void* user, long long done, long long total) = nullptr,
                   void* progress_user = nullptr,
                   long timeout_ms = 60000,
                   std::string* err = nullptr);

// ----------------------------------------------------------------------------
// Embedded demo catalog (generated from Raijin's catalog.json)
// ----------------------------------------------------------------------------
const std::string& demo_catalog_json();

}  // namespace modman
