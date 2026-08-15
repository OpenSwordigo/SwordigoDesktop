// ============================================================================
// mod_manager.cpp — Raijin-compatible mod manager for the Swordfare launcher.
//
// Implements full parity with Raijin's lawncher ModManager.java + ModStoreScreen:
//   - properties.toml parsing ([mod] section, key="value" pairs)
//   - zip install (icon.png + properties.toml + resources/) into mods/<id>/
//   - legacy mod.json folders still listed (backwards compatible)
//   - live store parsing: top-level JSON array with camelCase keys
//     (id/name/author/version/description/icon/downloadUrl)
//   - HTTPS via dlopen'd libcurl (TLS/redirects/gzip native); raw-socket
//     http:// fallback when libcurl is unavailable
// ============================================================================
#include "platform/mod_manager.h"
#include "platform/mod_catalog_embedded.h"
#include "platform/data_path.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#endif

#ifdef _WIN32
// Winsock needs WSAStartup; helper ensures it happens exactly once.
static bool ensure_winsock() {
    static bool ok = false;
    static bool tried = false;
    if (!tried) {
        WSADATA wsa;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        tried = true;
    }
    return ok;
}
#endif

namespace fs = std::filesystem;

namespace modman {

// ============================================================================
// Small JSON helpers (flat "key": value extraction, no external dependency)
// ============================================================================
namespace {

std::string json_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

long long json_i64(const std::string& json, const std::string& key, long long dflt = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return dflt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return dflt;
    char* endp = nullptr;
    long long v = strtoll(json.c_str() + pos, &endp, 10);
    return endp == json.c_str() + pos ? dflt : v;
}

double json_dbl(const std::string& json, const std::string& key, double dflt = 0.0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return dflt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return dflt;
    char* endp = nullptr;
    double v = strtod(json.c_str() + pos, &endp);
    return endp == json.c_str() + pos ? dflt : v;
}

bool json_bool(const std::string& json, const std::string& key, bool dflt = false) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return dflt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return dflt;
    return json.compare(pos, 4, "true") == 0;
}

// Extract an array of strings for a key: "key": ["a","b","c"]
std::vector<std::string> json_str_array(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return out;
    size_t end = json.find(']', pos);
    if (end == std::string::npos) return out;
    size_t p = pos + 1;
    while (p < end) {
        p = json.find('"', p);
        if (p == std::string::npos || p >= end) break;
        size_t e = json.find('"', p + 1);
        if (e == std::string::npos || e > end) break;
        out.push_back(json.substr(p + 1, e - p - 1));
        p = e + 1;
    }
    return out;
}

// Find the substring of one JSON object at array index idx of `key`.
std::string json_object_at(const std::string& json, const std::string& key, int idx) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return "";
    // Walk to the idx-th '{'
    int depth = 0;
    int seen = -1;
    bool in_str = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') { if (++depth == 1) { if (++seen == idx) { size_t start = i; 
            int d2 = 0; size_t end = start;
            for (size_t j = start; j < json.size(); ++j) {
                char cc = json[j];
                if (cc == '"') { 
                    size_t k = j + 1;
                    while (k < json.size() && json[k] != '"') { if (json[k] == '\\') ++k; ++k; }
                    j = k; continue; 
                }
                if (cc == '{') ++d2;
                else if (cc == '}') { if (--d2 == 0) { end = j; break; } }
            }
            return json.substr(start, end - start + 1);
        } } }
        else if (c == '}') { --depth; }
    }
    return "";
}

// Count JSON objects in array `key`. Tracks nested array depth so inner
// empty arrays (e.g. "screenshots": []) don't terminate the scan early.
int json_array_count(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return 0;
    int count = 0;
    int depth = 0;
    bool in_str = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '[') ++depth;
        else if (c == ']') { if (--depth == 0) break; }
        else if (c == '{' && depth == 1) ++count;
    }
    return count;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

std::string sanitize_id(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') out += c;
        else out += '_';
    }
    return out;
}

}  // namespace

// ============================================================================
// properties.toml parsing (minimal: flat key="value" pairs inside [mod])
// ============================================================================
namespace {

ModMeta parse_properties_toml(const std::string& text, const std::string& dir_path) {
    ModMeta m;
    m.dir_path = dir_path;
    m.is_toml = true;
    bool in_mod = false;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            in_mod = (line.compare(0, 5, "[mod]", 0, 5) == 0 ||
                      line.compare(0, 6, "[Mod]", 0, 6) == 0 ||
                      line.compare(0, 5, "[MOD]", 0, 5) == 0);
            continue;
        }
        if (!in_mod) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::string val = line.substr(eq + 1);
        val.erase(0, val.find_first_not_of(" \t\""));
        val.erase(val.find_last_not_of(" \t\"") + 1);
        if (key == "id") m.id = val;
        else if (key == "name") m.name = val;
        else if (key == "version") m.version = val;
        else if (key == "author") m.author = val;
        else if (key == "category") m.category = val;
        else if (key == "description") m.description = val;
        else if (key == "screenshots") {
            std::string cur;
            for (char c : val) {
                if (c == ',') {
                    if (!cur.empty()) { m.screenshots.push_back(cur); cur.clear(); }
                } else cur += c;
            }
            if (!cur.empty()) m.screenshots.push_back(cur);
        }
    }
    if (m.name.empty()) m.name = m.id;
    if (m.category.empty()) m.category = "General";
    return m;
}

// Legacy mod.json reading (old folder-based mods) — kept for back-compat.
ModMeta parse_mod_json(const std::string& json, const std::string& dir_path) {
    ModMeta m;
    m.dir_path = dir_path;
    m.is_toml = false;
    m.id          = json_str(json, "id");
    m.name        = json_str(json, "name");
    m.version     = json_str(json, "version");
    m.author      = json_str(json, "author");
    m.description = json_str(json, "description");
    m.type        = json_str(json, "type");
    m.category    = json_str(json, "category");
    if (m.category.empty()) m.category = m.type.empty() ? "General" : m.type;
    if (m.name.empty()) m.name = m.id;
    return m;
}

bool recursive_delete(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

bool recursive_copy(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (fs::is_directory(src)) {
        fs::create_directories(dst, ec);
        if (ec) return false;
        for (const auto& e : fs::directory_iterator(src, ec)) {
            if (ec) return false;
            if (!recursive_copy(e.path(), dst / e.path().filename())) return false;
        }
        return true;
    }
    fs::create_directories(dst.parent_path(), ec);
    if (ec) return false;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// ============================================================================
// Generic ZIP extractor (stored + deflate, central-directory driven)
// ============================================================================
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool zip_inflate(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in));
    zs.avail_in = (uInt)in_len;
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END || (rc == Z_OK && zs.avail_out == 0);
}

struct ZipEntryInfo {
    std::string name;
    uint16_t    method;
    uint32_t    comp_size;
    uint32_t    uncomp;
    uint32_t    local_off;
};

bool list_zip_entries(const std::string& zip_path, std::vector<ZipEntryInfo>* entries,
                      std::string* err) {
    std::ifstream f(zip_path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open zip"; return false; }
    f.seekg(0, std::ios::end);
    const std::streamoff file_size = f.tellg();
    if (file_size < 22) { if (err) *err = "file too small to be a ZIP"; return false; }
    const std::streamoff win_len = std::min<std::streamoff>(file_size, 22 + 65535);
    f.seekg(file_size - win_len);
    std::vector<uint8_t> tail((size_t)win_len);
    if (win_len > 0) f.read(reinterpret_cast<char*>(tail.data()), win_len);
    std::streamoff eocd = -1;
    for (std::streamoff i = win_len - 22; i >= 0; --i) {
        if (tail[(size_t)i] == 0x50 && tail[(size_t)i + 1] == 0x4b &&
            tail[(size_t)i + 2] == 0x05 && tail[(size_t)i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { if (err) *err = "ZIP end-of-central-directory not found"; return false; }
    const uint8_t* e = tail.data() + eocd;
    const uint32_t entry_count = le16(e + 10);
    const uint32_t cd_offset   = le32(e + 16);
    if (entry_count > 4096) { if (err) *err = "too many zip entries"; return false; }
    f.seekg(cd_offset);
    entries->clear();
    entries->reserve(entry_count);
    for (uint32_t n = 0; n < entry_count; ++n) {
        uint8_t hdr[46];
        f.read(reinterpret_cast<char*>(hdr), 46);
        if (!f || le32(hdr) != 0x02014b50) { if (err) *err = "bad central-directory entry"; return false; }
        ZipEntryInfo zi;
        zi.method    = le16(hdr + 10);
        zi.comp_size = le32(hdr + 20);
        zi.uncomp    = le32(hdr + 24);
        zi.local_off = le32(hdr + 42);
        const uint16_t name_len  = le16(hdr + 28);
        const uint16_t extra_len = le16(hdr + 30);
        const uint16_t cmt_len   = le16(hdr + 32);
        zi.name.resize(name_len);
        if (name_len) f.read(&zi.name[0], name_len);
        if (f.gcount() != name_len) { if (err) *err = "truncated name"; return false; }
        f.seekg(extra_len + cmt_len, std::ios::cur);
        if (!zi.name.empty() && zi.name.back() == '/') continue;  // directory
        entries->push_back(std::move(zi));
    }
    return true;
}

bool extract_zip_entry(const std::string& zip_path, const ZipEntryInfo& zi,
                       const fs::path& dest, std::string* err) {
    std::ifstream f(zip_path, std::ios::binary);
    if (!f) { if (err) *err = "cannot reopen zip"; return false; }
    if (zi.uncomp > (1u << 30)) { if (err) *err = "entry too large"; return false; }

    f.seekg(zi.local_off);
    uint8_t lh[30];
    f.read(reinterpret_cast<char*>(lh), 30);
    if (f.gcount() != 30 || le32(lh) != 0x04034b50) { if (err) *err = "bad local file header"; return false; }
    const std::streamoff data_off = zi.local_off + 30 + le16(lh + 26) + le16(lh + 28);

    std::vector<uint8_t> in(zi.comp_size);
    if (zi.comp_size) {
        f.seekg(data_off);
        f.read(reinterpret_cast<char*>(in.data()), zi.comp_size);
        if (f.gcount() != zi.comp_size) { if (err) *err = "truncated entry data"; return false; }
    }

    std::vector<uint8_t> out(zi.uncomp);
    if (zi.method == 0) {
        if (zi.comp_size != zi.uncomp) { if (err) *err = "stored size mismatch"; return false; }
        out = in;
    } else if (zi.method == 8) {
        if (!zip_inflate(in.data(), in.size(), out)) { if (err) *err = "inflate failed"; return false; }
    } else {
        if (err) *err = "unsupported compression method";
        return false;
    }

    // Path traversal protection
    std::string cleaned;
    for (char c : zi.name) cleaned += (c == '\\') ? '/' : c;
    while (cleaned.rfind("../", 0) == 0) cleaned.erase(0, 3);
    if (cleaned.find("/../") != std::string::npos) {
        std::string safe;
        std::istringstream parts(cleaned);
        std::string part;
        while (std::getline(parts, part, '/')) {
            if (part == "..") continue;
            safe += part; safe += '/';
        }
        cleaned = safe;
    }
    const fs::path target = dest / fs::path(cleaned);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) { if (err) *err = "cannot create dirs"; return false; }
    std::ofstream w(target, std::ios::binary | std::ios::trunc);
    if (!w) { if (err) *err = "cannot write " + target.string(); return false; }
    w.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
    return (bool)w;
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<ModMeta> list_mods(const std::string& mods_dir) {
    std::vector<ModMeta> mods;
    std::error_code ec;
    if (!fs::exists(mods_dir, ec) || !fs::is_directory(mods_dir, ec)) return mods;
    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        std::string dirname = entry.path().filename().string();
        bool disabled = (!dirname.empty() && dirname[0] == '.');
        std::string real_name = disabled ? dirname.substr(1) : dirname;

        ModMeta m;
        std::string toml = read_file(entry.path().string() + "/properties.toml");
        if (!toml.empty()) {
            m = parse_properties_toml(toml, entry.path().string());
        } else {
            std::string json = read_file(entry.path().string() + "/mod.json");
            if (json.empty()) continue;
            m = parse_mod_json(json, entry.path().string());
        }
        if (m.id.empty()) m.id = real_name;
        m.enabled = !disabled;
        fs::path icon = entry.path() / "icon.png";
        if (fs::exists(icon, ec)) m.icon_path = icon.string();
        mods.push_back(std::move(m));
    }
    std::sort(mods.begin(), mods.end(),
              [](const ModMeta& a, const ModMeta& b) {
                  std::string an = a.name, bn = b.name;
                  std::transform(an.begin(), an.end(), an.begin(), ::tolower);
                  std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
                  return an < bn;
              });
    return mods;
}

bool install_mod_zip(const std::string& zip_path, const std::string& mods_dir,
                     ModMeta* out, std::string* err) {
    std::vector<ZipEntryInfo> entries;
    if (!list_zip_entries(zip_path, &entries, err)) return false;

    // Extract to a staging dir.
    fs::path staging;
#ifndef _WIN32
    char tmpl[] = "/tmp/swordfare_mod_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { if (err) *err = "cannot create staging dir"; return false; }
    staging = dir;
#else
    staging = fs::temp_directory_path() / ("swordfare_mod_" + std::to_string(rand()));
    std::error_code ec2;
    fs::create_directories(staging, ec2);
    if (ec2) { if (err) *err = "cannot create staging dir"; return false; }
#endif

    bool ok = true;
    for (const auto& zi : entries) {
        std::string e;
        if (!extract_zip_entry(zip_path, zi, staging, &e)) {
            if (err) *err = e.empty() ? "extract failed" : e;
            ok = false;
            break;
        }
    }
    if (!ok) { recursive_delete(staging); return false; }

    // Validate properties.toml
    std::string toml = read_file((staging / "properties.toml").string());
    if (toml.empty()) {
        recursive_delete(staging);
        if (err) *err = "missing properties.toml (not a Raijin-format mod zip)";
        return false;
    }
    ModMeta meta = parse_properties_toml(toml, "");
    if (meta.id.empty()) {
        recursive_delete(staging);
        if (err) *err = "properties.toml missing [mod] id";
        return false;
    }

    std::string id = sanitize_id(meta.id);
    if (id.empty()) {
        recursive_delete(staging);
        if (err) *err = "invalid mod id";
        return false;
    }

    // Install into mods/<id>/ — keep resources/ as a real subfolder so the
    // host VFS 5-level hierarchy resolves mods/<id>/resources/<path>.
    fs::path target = fs::path(mods_dir) / id;
    std::error_code ec;
    fs::create_directories(mods_dir, ec);
    recursive_delete(target);
    fs::create_directories(target, ec);

    fs::path staged_resources = staging / "resources";
    if (fs::exists(staged_resources, ec)) {
        if (!recursive_copy(staged_resources, target / "resources")) {
            recursive_delete(staging); recursive_delete(target);
            if (err) *err = "failed to copy resources";
            return false;
        }
    }
    fs::path staged_icon = staging / "icon.png";
    if (fs::exists(staged_icon, ec)) {
        fs::copy_file(staged_icon, target / "icon.png", fs::copy_options::overwrite_existing, ec);
    }
    fs::copy_file(staging / "properties.toml", target / "properties.toml",
                  fs::copy_options::overwrite_existing, ec);

    recursive_delete(staging);

    meta.dir_path = target.string();
    if (!meta.icon_path.empty()) meta.icon_path = (target / "icon.png").string();
    if (out) *out = std::move(meta);
    if (err) err->clear();
    return true;
}

bool delete_mod(const ModMeta& mod) {
    if (mod.dir_path.empty()) return false;
    return recursive_delete(mod.dir_path);
}

bool set_mod_enabled(const std::string& dir_path, bool enabled) {
    fs::path p(dir_path);
    std::string name = p.filename().string();
    std::string want;
    if (!enabled && name[0] != '.') want = "." + name;
    else if (enabled && name[0] == '.') want = name.substr(1);
    if (want.empty()) return true;
    fs::path np = p.parent_path() / want;
    std::error_code ec;
    fs::rename(p, np, ec);
    return !ec;
}

// ============================================================================
// Store catalog
// ============================================================================

namespace {

// Strip trailing commas before } or ] (Raijin's sanitizeJson — GitHub raw
// sometimes has trailing commas). Operates outside strings.
std::string sanitize_json(const std::string& json) {
    std::string out;
    out.reserve(json.size());
    bool in_str = false;
    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            out += c;
            if (c == '\\' && i + 1 < json.size()) { out += json[++i]; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; out += c; continue; }
        if (c == ',') {
            // Peek ahead past whitespace for } or ]
            size_t j = i + 1;
            while (j < json.size() && (json[j] == ' ' || json[j] == '\t' ||
                                       json[j] == '\r' || json[j] == '\n')) ++j;
            if (j < json.size() && (json[j] == '}' || json[j] == ']')) continue;  // drop comma
        }
        out += c;
    }
    return out;
}

// Extract every top-level JSON object of an array document: [ {...}, {...} ]
std::vector<std::string> json_top_array_objects(const std::string& json) {
    std::vector<std::string> out;
    size_t pos = json.find('[');
    if (pos == std::string::npos) return out;
    bool in_str = false;
    int brace = 0;
    size_t start = std::string::npos;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') {
            if (brace == 0) start = i;
            ++brace;
        } else if (c == '}') {
            --brace;
            if (brace == 0 && start != std::string::npos) {
                out.push_back(json.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

// Reads a field from a store entry, accepting camelCase + snake_case spellings.
std::string entry_str(const std::string& o, const std::string& camel,
                      const std::string& snake = "") {
    std::string v = json_str(o, camel);
    if (v.empty() && !snake.empty()) v = json_str(o, snake);
    return v;
}

}  // namespace

bool is_raijin_store_format(const std::string& json) {
    size_t s = json.find_first_not_of(" \t\r\n");
    return s != std::string::npos && json[s] == '[';
}

std::vector<StoreMod> parse_catalog(const std::string& json) {
    std::vector<StoreMod> mods;
    const std::string clean = sanitize_json(json);

    if (is_raijin_store_format(clean)) {
        // --- Live Raijin store: top-level array, camelCase keys ---
        for (const auto& o : json_top_array_objects(clean)) {
            StoreMod m;
            m.id = entry_str(o, "id");
            if (m.id.empty()) continue;
            m.name             = entry_str(o, "name");
            m.author           = entry_str(o, "author");
            m.version          = entry_str(o, "version");
            m.description      = entry_str(o, "description");
            m.long_description = entry_str(o, "long_description");
            m.category         = entry_str(o, "category");
            m.download_url     = entry_str(o, "downloadUrl", "download_url");
            m.icon_url         = entry_str(o, "icon");
            m.currency         = entry_str(o, "currency");
            m.price_cents      = json_i64(o, "price_cents");
            m.size_bytes       = json_i64(o, "size_bytes");
            m.installs         = json_i64(o, "installs");
            m.rating           = json_dbl(o, "rating");
            m.featured         = json_bool(o, "featured");
            m.screenshots      = json_str_array(o, "screenshots");
            m.tags             = json_str_array(o, "tags");
            if (m.name.empty()) m.name = m.id;
            if (m.category.empty()) m.category = "General";
            if (m.currency.empty()) m.currency = "USD";
            mods.push_back(std::move(m));
        }
    } else {
        // --- Legacy demo catalog: {"version":...,"mods":[...]}, snake_case ---
        int count = json_array_count(clean, "mods");
        for (int i = 0; i < count; ++i) {
            std::string o = json_object_at(clean, "mods", i);
            if (o.empty()) continue;
            StoreMod m;
            m.id              = json_str(o, "id");
            if (m.id.empty()) continue;
            m.name            = json_str(o, "name");
            m.author          = json_str(o, "author");
            m.version         = json_str(o, "version");
            m.description     = json_str(o, "description");
            m.long_description= json_str(o, "long_description");
            m.category        = json_str(o, "category");
            m.currency        = json_str(o, "currency");
            m.download_url    = json_str(o, "download_url");
            m.icon_url        = json_str(o, "icon");
            m.price_cents     = json_i64(o, "price_cents");
            m.size_bytes      = json_i64(o, "size_bytes");
            m.installs        = json_i64(o, "installs");
            m.rating          = json_dbl(o, "rating");
            m.featured        = json_bool(o, "featured");
            m.screenshots     = json_str_array(o, "screenshots");
            m.tags            = json_str_array(o, "tags");
            if (m.name.empty()) m.name = m.id;
            if (m.category.empty()) m.category = "General";
            if (m.currency.empty()) m.currency = "USD";
            mods.push_back(std::move(m));
        }
    }
    return mods;
}

std::vector<bool> catalog_installed_mask(const std::vector<StoreMod>& mods,
                                         const std::string& mods_dir) {
    std::vector<bool> mask(mods.size(), false);
    std::vector<ModMeta> installed = list_mods(mods_dir);
    for (size_t i = 0; i < mods.size(); ++i) {
        for (const auto& im : installed) {
            if (im.id == mods[i].id) { mask[i] = true; break; }
        }
    }
    return mask;
}

// ============================================================================
// HTTP client — dlopen'd libcurl when available (HTTPS/TLS/redirects/gzip),
// raw-socket http:// fallback otherwise.
// ============================================================================

namespace {

// ---- runtime libcurl binding (dlopen / LoadLibrary) -------------------------
#ifdef _WIN32
typedef long long swf_curl_off_t;
#else
typedef long swf_curl_off_t;
#endif

struct CurlApi {
    void* h = nullptr;
    int (*global_init)(long) = nullptr;
    void* (*easy_init)(void) = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
};

CurlApi& curl_api() {
    static CurlApi api = [] {
        CurlApi a;
#ifdef _WIN32
        a.h = (void*)LoadLibraryA("libcurl-4.dll");
        if (!a.h) a.h = (void*)LoadLibraryA("libcurl.dll");
        if (a.h) {
            a.global_init  = (int(*)(long))GetProcAddress((HMODULE)a.h, "curl_global_init");
            a.easy_init    = (void*(*)(void))GetProcAddress((HMODULE)a.h, "curl_easy_init");
            a.easy_setopt  = (int(*)(void*, int, ...))GetProcAddress((HMODULE)a.h, "curl_easy_setopt");
            a.easy_perform = (int(*)(void*))GetProcAddress((HMODULE)a.h, "curl_easy_perform");
            a.easy_cleanup = (void(*)(void*))GetProcAddress((HMODULE)a.h, "curl_easy_cleanup");
            a.easy_getinfo = (int(*)(void*, int, ...))GetProcAddress((HMODULE)a.h, "curl_easy_getinfo");
            a.easy_strerror= (const char*(*)(int))GetProcAddress((HMODULE)a.h, "curl_easy_strerror");
        }
#else
        a.h = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
        if (!a.h) a.h = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
        if (a.h) {
            a.global_init  = (int(*)(long))dlsym(a.h, "curl_global_init");
            a.easy_init    = (void*(*)(void))dlsym(a.h, "curl_easy_init");
            a.easy_setopt  = (int(*)(void*, int, ...))dlsym(a.h, "curl_easy_setopt");
            a.easy_perform = (int(*)(void*))dlsym(a.h, "curl_easy_perform");
            a.easy_cleanup = (void(*)(void*))dlsym(a.h, "curl_easy_cleanup");
            a.easy_getinfo = (int(*)(void*, int, ...))dlsym(a.h, "curl_easy_getinfo");
            a.easy_strerror= (const char*(*)(int))dlsym(a.h, "curl_easy_strerror");
        }
#endif
        return a;
    }();
    return api;
}

bool curl_available() {
    CurlApi& a = curl_api();
    return a.h && a.global_init && a.easy_init && a.easy_setopt && a.easy_perform &&
           a.easy_cleanup && a.easy_getinfo;
}

// curl option numeric values — stable ABI (CURLOPTTYPE_* | offset, from curl/curl.h)
enum {
    kCurlOptUrl            = 10002,  // STRINGPOINT
    kCurlOptWriteData      = 10001,  // OBJECTPOINT
    kCurlOptErrorBuffer    = 10010,  // OBJECTPOINT
    kCurlOptUserAgent      = 10018,  // STRINGPOINT
    kCurlOptNoProgress     = 43,     // LONG
    kCurlOptFollowLocation = 52,     // LONG
    kCurlOptSslVerifyPeer  = 64,     // LONG
    kCurlOptMaxRedirects   = 68,     // LONG
    kCurlOptSslVerifyHost  = 81,     // LONG
    kCurlOptTimeoutMs      = 155,    // LONG
    kCurlOptConnectTimeout = 156,    // LONG
    kCurlOptAcceptEncoding = 10102,  // STRINGPOINT
    kCurlOptWriteFunction  = 20011,  // FUNCTIONPOINT
    kCurlOptXferInfoData   = 10057,  // OBJECTPOINT
    kCurlOptXferInfoFn     = 20219,  // FUNCTIONPOINT
    kCurlInfoResponseCode  = 0x200002,
};

struct CurlSink {
    std::string* out;
    void (*progress)(void*, long long, long long);
    void* progress_user;
};

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    CurlSink* sink = (CurlSink*)userdata;
    sink->out->append(ptr, size * nmemb);
    return size * nmemb;
}

int curl_progress_cb(void* userdata, swf_curl_off_t dltotal, swf_curl_off_t dlnow,
                     swf_curl_off_t, swf_curl_off_t) {
    CurlSink* sink = (CurlSink*)userdata;
    if (sink->progress) sink->progress(sink->progress_user, (long long)dlnow, (long long)dltotal);
    return 0;
}

bool http_get_curl(const std::string& url, std::string* out,
                   void (*progress)(void*, long long, long long), void* progress_user,
                   long timeout_ms, std::string* err) {
    CurlApi& a = curl_api();
    if (!curl_available()) { if (err) *err = "libcurl not available"; return false; }
    static bool global_init_done = false;   // refcounted anyway; call exactly once
    if (!global_init_done && a.global_init) { a.global_init(3); global_init_done = true; }
    void* h = a.easy_init();
    if (!h) { if (err) *err = "curl init failed"; return false; }

    CurlSink sink{ out, progress, progress_user };
    char errbuf[256] = {0};

    a.easy_setopt(h, kCurlOptUrl, url.c_str());
    a.easy_setopt(h, kCurlOptWriteFunction, (void*)&curl_write_cb);
    a.easy_setopt(h, kCurlOptWriteData, &sink);
    a.easy_setopt(h, kCurlOptFollowLocation, 1L);
    a.easy_setopt(h, kCurlOptMaxRedirects, 8L);
    a.easy_setopt(h, kCurlOptSslVerifyPeer, 1L);
    a.easy_setopt(h, kCurlOptSslVerifyHost, 2L);
    a.easy_setopt(h, kCurlOptAcceptEncoding, "");       // gzip + deflate auto-decode
    a.easy_setopt(h, kCurlOptUserAgent,
                  "Swordfare/8.0 (Raijin-mod-compatible; +https://github.com/raijinswordigo/requests)");
    a.easy_setopt(h, kCurlOptConnectTimeout, (long)timeout_ms);
    a.easy_setopt(h, kCurlOptTimeoutMs, (long)timeout_ms);
    a.easy_setopt(h, kCurlOptErrorBuffer, errbuf);
    if (progress) {
        a.easy_setopt(h, kCurlOptNoProgress, 0L);
        a.easy_setopt(h, kCurlOptXferInfoFn, (void*)&curl_progress_cb);
        a.easy_setopt(h, kCurlOptXferInfoData, &sink);
    } else {
        a.easy_setopt(h, kCurlOptNoProgress, 1L);
    }

    int rc = a.easy_perform(h);
    long code = 0;
    if (a.easy_getinfo) a.easy_getinfo(h, kCurlInfoResponseCode, &code);
    a.easy_cleanup(h);

    if (rc != 0) {
        if (err) *err = std::string("curl error: ") +
                        (errbuf[0] ? errbuf
                                   : (a.easy_strerror ? a.easy_strerror(rc) : "unknown"));
        return false;
    }
    if (code != 200 && code != 0) {
        if (err) *err = "HTTP " + std::to_string(code);
        return false;
    }
    return true;
}

struct UrlParts {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

bool parse_url(const std::string& url, UrlParts* parts) {
    if (url.compare(0, 7, "http://") != 0) return false;
    size_t rest = 7;
    size_t slash = url.find('/', rest);
    std::string hostport = (slash == std::string::npos) ? url.substr(rest)
                                                        : url.substr(rest, slash - rest);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        parts->host = hostport.substr(0, colon);
        parts->port = hostport.substr(colon + 1);
    } else {
        parts->host = hostport;
    }
    parts->path = (slash == std::string::npos) ? "/" : url.substr(slash);
    return !parts->host.empty();
}

}  // namespace

#ifdef _WIN32
#define SWF_CLOSE(fd) closesocket(fd)
#define SWF_SOCKET_TYPE SOCKET
#define SWF_INVALID_SOCKET INVALID_SOCKET
#else
#define SWF_CLOSE(fd) close(fd)
#define SWF_SOCKET_TYPE int
#define SWF_INVALID_SOCKET (-1)
#endif

// Raw-socket GET — http:// only (used when libcurl is unavailable).
static bool http_get_raw(const std::string& url, std::string* out,
                         void (*progress)(void*, long long, long long), void* progress_user,
                         long timeout_ms, std::string* err) {
#ifdef _WIN32
    if (!ensure_winsock()) { if (err) *err = "Winsock init failed"; return false; }
#endif
    UrlParts parts;
    if (!parse_url(url, &parts)) {
        if (err) *err = "unsupported URL (only http:// supported without libcurl)";
        return false;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int gai = getaddrinfo(parts.host.c_str(), parts.port.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        if (err) *err = std::string("DNS/connect failed: ") + gai_strerror(gai);
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(res, freeaddrinfo);

    SWF_SOCKET_TYPE fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SWF_INVALID_SOCKET) { if (err) *err = "socket failed"; return false; }
#ifdef _WIN32
    DWORD tmo = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    bool ok = false;
    if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        std::string req = "GET " + parts.path + " HTTP/1.1\r\n"
                          "Host: " + parts.host + "\r\n"
                          "User-Agent: Swordfare/8.0 (Raijin-mod-compatible)\r\n"
                          "Connection: close\r\n\r\n";
        if (send(fd, req.data(), (int)req.size(), 0) == (int)req.size()) {
            std::string raw;
            char buf[65536];
            int n;
            while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
                raw.append(buf, (size_t)n);
            }
            if (!raw.empty()) {
                size_t hdr_end = raw.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    std::string headers = raw.substr(0, hdr_end);
                    std::string body = raw.substr(hdr_end + 4);
                    if (headers.find(" 200 ") != std::string::npos ||
                        headers.find("\n200 ") != std::string::npos ||
                        headers.compare(0, 12, "HTTP/1.1 200") == 0 ||
                        headers.compare(0, 12, "HTTP/1.0 200") == 0) {
                        long long total = 0;
                        size_t cl = headers.find("Content-Length:");
                        if (cl != std::string::npos) {
                            total = atoll(headers.c_str() + cl + 15);
                        }
                        if (out) *out = std::move(body);
                        if (progress) progress(progress_user, (long long)body.size(), total);
                        ok = true;
                    } else {
                        if (err) *err = "HTTP error: " + headers.substr(headers.find(' '));
                    }
                } else {
                    if (err) *err = "malformed HTTP response";
                }
            } else {
                if (err) *err = "empty HTTP response (timeout?)";
            }
        } else {
            if (err) *err = "send failed";
        }
    } else {
        if (err) *err = "connection refused/failed";
    }
    SWF_CLOSE(fd);
    return ok;
}

bool http_get(const std::string& url, std::string* out,
              void (*progress)(void*, long long, long long), void* progress_user,
              long timeout_ms, std::string* err) {
    if (curl_available()) return http_get_curl(url, out, progress, progress_user, timeout_ms, err);
    if (url.compare(0, 8, "https://") == 0) {
        if (err) *err = "https requires libcurl, which is not available";
        return false;
    }
    return http_get_raw(url, out, progress, progress_user, timeout_ms, err);
}

bool http_download(const std::string& url, const std::string& dest_path,
                   void (*progress)(void*, long long, long long), void* progress_user,
                   long timeout_ms, std::string* err) {
    std::string body;
    if (!http_get(url, &body, progress, progress_user, timeout_ms, err)) return false;
    if (!write_file(dest_path, body)) {
        if (err) *err = "cannot write " + dest_path;
        return false;
    }
    return true;
}


const std::string& demo_catalog_json() {
    static const std::string s(reinterpret_cast<const char*>(k_demo_catalog_json),
                               k_demo_catalog_json_len);
    return s;
}

}  // namespace modman
