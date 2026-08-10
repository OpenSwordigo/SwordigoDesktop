#include "platform/binary_selector.h"
#include <cstdint>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <algorithm>
#include <map>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <zlib.h>

namespace fs = std::filesystem;

static std::string safe_instance_name(const std::string& name) {
    std::string result;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_') result += static_cast<char>(c);
        else if (std::isspace(c) && !result.empty() && result.back() != '-') result += '-';
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result.empty() ? "Imported-APK" : result;
}

// ============================================================================
// Bundled miniature ZIP extractor (no external `unzip`/`7z` dependency).
//
// Used by APK import. Reads only what an APK needs: the `assets/*` tree and
// native libraries `lib/<abi>/*.so`, decoding stored (method 0) and deflate
// (method 8) entries with zlib. Central-directory driven, with the local file
// header located so the data offset is always correct.
// ============================================================================

namespace {

inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool want_apk_entry(const std::string& name) {
    if (name.empty() || name.back() == '/') return false; // directories
    if (name.rfind("assets/", 0) == 0) return true;
    if (name.rfind("lib/", 0) == 0) {
        // lib/<abi>/<lib>*.so
        const size_t second = name.find('/', 4);
        if (second != std::string::npos) {
            const std::string abi = name.substr(4, second - 4);
            if ((abi == "armeabi-v7a" || abi == "arm64-v8a") &&
                name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0)
                return true;
        }
    }
    return false;
}

bool inflate_entry(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out) {
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

} // namespace

// Extract assets/* and lib/<abi>/*.so from `apk_path` into `dest`. Returns
// false (with *err set) on any error. Safe against Zip-bomb-ish sizes.
static bool extract_apk_zip(const std::string& apk_path, const fs::path& dest, std::string* err) {
    using u8 = uint8_t;
    std::ifstream f(apk_path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open APK"; return false; }

    f.seekg(0, std::ios::end);
    const std::streamoff file_size = f.tellg();
    if (file_size < 22) { if (err) *err = "file too small to be a ZIP"; return false; }

    // Locate End Of Central Directory within the last 64 KiB + 22 bytes.
    const std::streamoff win_len = std::min<std::streamoff>(file_size, 22 + 65535);
    f.seekg(file_size - win_len);
    std::vector<u8> tail((size_t)win_len);
    if (win_len > 0) f.read(reinterpret_cast<char*>(tail.data()), win_len);

    std::streamoff eocd = -1;
    for (std::streamoff i = win_len - 22; i >= 0; --i) {
        if (tail[(size_t)i] == 0x50 && tail[(size_t)i + 1] == 0x4b &&
            tail[(size_t)i + 2] == 0x05 && tail[(size_t)i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { if (err) *err = "ZIP end-of-central-directory not found"; return false; }

    const u8* e = tail.data() + eocd;
    const uint32_t entry_count = le16(e + 10);
    const uint32_t cd_offset   = le32(e + 16);

    f.seekg(cd_offset);
    for (uint32_t n = 0; n < entry_count; ++n) {
        u8 hdr[46];
        f.read(reinterpret_cast<char*>(hdr), 46);
        if (!f || le32(hdr) != 0x02014b50) { if (err) *err = "bad central-directory entry"; return false; }
        const uint16_t method    = le16(hdr + 10);
        const uint32_t comp_size = le32(hdr + 20);
        const uint32_t uncomp    = le32(hdr + 24);
        const uint16_t name_len  = le16(hdr + 28);
        const uint16_t extra_len = le16(hdr + 30);
        const uint32_t local_off = le32(hdr + 42);
        std::string name((size_t)name_len, '\0');
        if (name_len) f.read(&name[0], name_len);
        if (f.gcount() != name_len) { if (err) *err = "truncated central-directory name"; return false; }
        f.seekg(extra_len, std::ios::cur); // skip central extra
        f.seekg(std::streamoff(le16(hdr + 32)), std::ios::cur); // skip central comment

        if (!want_apk_entry(name)) continue;
        if (uncomp > (512u << 20)) { if (err) *err = "entry too large"; return false; }

        // Locate compressed payload: local header + 30 + name + extra.
        f.seekg(local_off);
        u8 lh[30];
        f.read(reinterpret_cast<char*>(lh), 30);
        if (f.gcount() != 30 || le32(lh) != 0x04034b50) { if (err) *err = "bad local file header"; return false; }
        const std::streamoff data_off = local_off + 30 + le16(lh + 26) + le16(lh + 28);

        std::vector<uint8_t> in((size_t)comp_size);
        if (comp_size) {
            f.seekg(data_off);
            f.read(reinterpret_cast<char*>(in.data()), comp_size);
            if (f.gcount() != comp_size) { if (err) *err = "truncated entry data"; return false; }
        }

        std::vector<uint8_t> out((size_t)uncomp);
        if (method == 0) {          // stored
            if (comp_size != uncomp) { if (err) *err = "size mismatch"; return false; }
            out = in;
        } else if (method == 8) {   // deflate
            if (!inflate_entry(in.data(), in.size(), out)) { if (err) *err = "inflate failed"; return false; }
        } else {
            if (err) *err = "unsupported compression method";
            return false;
        }

        // Write under dest, preventing traversal.
        std::string cleaned;
        for (char c : name) cleaned += (c == '\\') ? '/' : c;
        while (cleaned.rfind("../", 0) == 0) cleaned.erase(0, 3);
        const fs::path target = dest / fs::path(cleaned);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream w(target, std::ios::binary | std::ios::trunc);
        if (!w) { if (err) *err = "cannot write " + target.string(); return false; }
        w.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
    }
    return true;
}

// ============================================================================
// Minimal SHA-256 implementation (no OpenSSL dependency)
// ============================================================================

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
    uint32_t e=state[4], f=state[5], g=state[6], h=state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

std::string BinarySelector::compute_sha256(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return "";

    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint8_t block[64];
    uint64_t total_bits = 0;

    while (file) {
        file.read((char*)block, 64);
        size_t bytes_read = file.gcount();
        total_bits += bytes_read * 8;

        if (bytes_read == 64) {
            sha256_transform(state, block);
        } else {
            block[bytes_read] = 0x80;
            for (size_t i = bytes_read + 1; i < 64; i++) block[i] = 0;

            if (bytes_read >= 56) {
                sha256_transform(state, block);
                memset(block, 0, 64);
            }

            for (int i = 7; i >= 0; i--)
                block[56 + (7 - i)] = (uint8_t)(total_bits >> (i * 8));

            sha256_transform(state, block);
        }
    }

    std::ostringstream oss;
    for (int i = 0; i < 8; i++)
        oss << std::hex << std::setw(8) << std::setfill('0') << state[i];
    return oss.str();
}

// ============================================================================
// Known binary hashes
// ============================================================================

const std::map<std::string, std::pair<std::string, BinaryStatus>> BinarySelector::KNOWN_HASHES = {
    // Vanilla ARM32
    {"cee15dd2730746269ce5db97d150371ebbad1f41371c6a728f1bb7d045632138",
        {"1.4.6", BinaryStatus::TESTED}},
    {"08d49dd6f7f8639a4c59f290ff2bb79254accf710f530bf53c2fce1659191c9e",
        {"1.4.12", BinaryStatus::TESTED}},
    // RLSwordigo ARM32
    {"a7c00ff6f3ed0d5b3221158d6e214bba03288c1e6782be3dc2c736ae80eb19df",
        {"6.6-rl", BinaryStatus::TESTED}},
};

// ============================================================================
// Constructor
// ============================================================================

BinarySelector::BinarySelector() : default_binary("engine/v1.4.12/armeabi-v7a/libswordigo.so") {}

// ============================================================================
// Scan engine/ directory structure
//
//   engine/
//     v1.4.6/
//       armeabi-v7a/
//         libswordigo.so
//       arm64-v8a/
//         libswordigo.so
//     v1.4.12/
//       ...
//     rl-v6.1/
//       armeabi-v7a/
//         libswordigo.so
//         libmini.so
//         libGlossHook.so
// ============================================================================

void BinarySelector::scan_engine_directory(const std::string& engine_path) {
    binaries.clear();

    if (!fs::exists(engine_path) || !fs::is_directory(engine_path)) {
        std::cerr << "[BinSel] engine/ directory not found: " << engine_path << std::endl;
        return;
    }

    // Iterate version directories (v1.4.6, v1.4.12, rl-v6.1, etc.)
    for (const auto& ver_entry : fs::directory_iterator(engine_path)) {
        if (!ver_entry.is_directory()) continue;
        std::string ver_dir = ver_entry.path().filename().string();
        scan_version_dir(engine_path, ver_dir);
    }

    // Sort: default first, then by status (TESTED > TESTING > UNKNOWN), then by version
    std::sort(binaries.begin(), binaries.end(), [](const BinaryInfo& a, const BinaryInfo& b) {
        if (a.is_default != b.is_default) return a.is_default;
        if (a.status != b.status) return (int)a.status < (int)b.status;
        // ARM32 before ARM64 within same version
        if (a.version_dir == b.version_dir && a.arch != b.arch)
            return a.arch == BinaryArch::ARM32;
        return a.version_dir > b.version_dir; // Newer versions first
    });

    std::cout << "[BinSel] Found " << binaries.size() << " game binaries in engine/" << std::endl;
    for (const auto& b : binaries) {
        std::cout << "  " << b.label << " → " << b.filepath << std::endl;
    }
}

void BinarySelector::scan_version_dir(const std::string& engine_path, const std::string& version_dir) {
    std::string ver_path = engine_path + "/" + version_dir;

    // Check for armeabi-v7a/ and arm64-v8a/ subdirectories
    std::string arm32_path = ver_path + "/armeabi-v7a";
    std::string arm64_path = ver_path + "/arm64-v8a";

    if (fs::exists(arm32_path) && fs::is_directory(arm32_path)) {
        scan_arch_dir(arm32_path, version_dir, BinaryArch::ARM32);
    }
    if (fs::exists(arm64_path) && fs::is_directory(arm64_path)) {
        scan_arch_dir(arm64_path, version_dir, BinaryArch::ARM64);
    }
}

void BinarySelector::scan_arch_dir(const std::string& arch_path, const std::string& version_dir, BinaryArch arch) {

    // Find the main libswordigo.so
    std::string main_so = arch_path + "/libswordigo.so";
    if (!fs::exists(main_so)) {
        // No game binary in this arch dir
        return;
    }

    BinaryInfo info;
    info.filename = "libswordigo.so";
    info.filepath = main_so;
    info.version_dir = version_dir;
    info.arch = arch;
    info.file_size = fs::file_size(main_so);
    info.is_default = (info.filepath == default_binary);

    // Determine game type from version dir name
    // - "rl-*" directories → RLSwordigo mod, uses rl_assets
    // - "sw3d"  → 3D camera mod, uses vanilla assets
    // - "v*"    → vanilla Swordigo, uses vanilla assets
    bool is_rl = (version_dir.find("rl-") == 0);
    bool is_vanilla = (version_dir[0] == 'v');  // v1.4.6, v1.4.12, v1.1
    bool is_mod = !is_vanilla;                   // Any non-vanilla has companion .so files

    if (is_rl) {
        info.game_type = "RLSwordigo";
        info.assets_dir = "rl_assets";
    } else {
        info.game_type = "Swordigo";
        info.assets_dir = "assets";
    }
    
    // Scan for companion .so files in ALL mod directories (rl-, sw3d, etc.)
    if (is_mod) {
        for (const auto& entry : fs::directory_iterator(arch_path)) {
            if (!entry.is_regular_file()) continue;
            std::string dep_name = entry.path().filename().string();
            // Skip the main game binary and non-.so files
            if (dep_name != "libswordigo.so" && dep_name.length() > 3 &&
                dep_name.substr(dep_name.length() - 3) == ".so") {
                info.dependencies.push_back(dep_name);
                info.dep_paths.push_back(entry.path().string());
            }
        }
    }

    // If libsre.so is present in this directory, auto-strip libmini.so and libGlossHook.so
    // from dependencies (SRE replaces their functionality)
    if (fs::exists(arch_path + "/libsre.so")) {
        info.dependencies.erase(
            std::remove_if(info.dependencies.begin(), info.dependencies.end(),
                [](const std::string& d) {
                    return d == "libmini.so" || d == "libGlossHook.so" || d == "libkiwi.so";
                }), info.dependencies.end());
        info.dep_paths.erase(
            std::remove_if(info.dep_paths.begin(), info.dep_paths.end(),
                [](const std::string& p) {
                    return p.find("libmini.so") != std::string::npos ||
                           p.find("libGlossHook.so") != std::string::npos ||
                           p.find("libkiwi.so") != std::string::npos;
                }), info.dep_paths.end());
        std::cout << "[BinSel] SRE detected in " << arch_path << ", stripped modloader deps" << std::endl;
    }

    // Compute hash
    std::cout << "[BinSel] Hashing " << version_dir << "/" << arch_string(arch) 
              << "/libswordigo.so..." << std::flush;
    info.sha256 = compute_sha256(main_so);
    std::cout << " " << info.sha256.substr(0, 16) << "..." << std::endl;

    // Look up in known hashes
    auto it = KNOWN_HASHES.find(info.sha256);
    if (it != KNOWN_HASHES.end()) {
        info.version = it->second.first;
        info.status = it->second.second;
    } else {
        // Extract version from directory name
        std::string ver = version_dir;
        if (ver.substr(0, 1) == "v") {
            ver = ver.substr(1);  // v1.4.6 → 1.4.6
        } else if (ver.substr(0, 3) == "rl-") {
            ver = ver.substr(3);  // rl-v6.6 → v6.6
            if (ver.substr(0, 1) == "v") ver = ver.substr(1);  // → 6.6
            ver += "-rl";  // → 6.6-rl
        }
        // sw3d stays as "sw3d"
        info.version = ver;
        info.status = BinaryStatus::UNKNOWN;
    }

    // Build display label
    const char* status_str = (info.status == BinaryStatus::TESTED) ? "Stable" :
                             (info.status == BinaryStatus::TESTING) ? "Testing" : "Unknown";
    std::string arch_badge = std::string("[") + arch_string(arch) + "]";
    
    if (is_rl) {
        info.label = "[RL] v" + info.version + " " + arch_badge + " (" + status_str + ")";
    } else if (version_dir == "sw3d") {
        info.label = "[3D] " + arch_badge + " (" + status_str + ")";
    } else {
        info.label = "v" + info.version + " " + arch_badge + " (" + status_str + ")";
    }
    
    // Mark latest tested vanilla version
    if (!is_rl && info.status == BinaryStatus::TESTED && info.version == "1.4.12") {
        info.label += " [Latest]";
    }

    binaries.push_back(info);
}

// ============================================================================
// Legacy flat directory scan (backward compat — scans project root for *.so)
// ============================================================================

void BinarySelector::scan_directory(const std::string& dir_path) {
    // Check if engine/ directory exists — if so, use the new method
    std::string engine_path = dir_path + "/engine";
    if (fs::exists(engine_path) && fs::is_directory(engine_path)) {
        scan_engine_directory(engine_path);
        return;
    }

    // Fallback: legacy flat scan (for backward compat)
    std::cerr << "[BinSel] No engine/ directory found, falling back to legacy scan" << std::endl;
    
    binaries.clear();
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) return;

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();

        if (name.find("libswordigo") == 0 && name.length() > 3 &&
            name.substr(name.length() - 3) == ".so") {
            
            if (name.find("64") != std::string::npos) continue; // Skip arm64 in legacy mode

            BinaryInfo info;
            info.filename = name;
            info.filepath = entry.path().string();
            info.arch = BinaryArch::ARM32;
            info.file_size = fs::file_size(entry.path());
            info.game_type = "Swordigo";
            info.assets_dir = "assets";
            info.is_default = false;

            info.sha256 = compute_sha256(info.filepath);
            auto it = KNOWN_HASHES.find(info.sha256);
            if (it != KNOWN_HASHES.end()) {
                info.version = it->second.first;
                info.status = it->second.second;
            } else {
                info.version = "Unknown";
                info.status = BinaryStatus::UNKNOWN;
            }

            const char* status_str = (info.status == BinaryStatus::TESTED) ? "Stable" :
                                     (info.status == BinaryStatus::TESTING) ? "Testing" : "Unknown";
            info.label = "v" + info.version + " [ARM32] (" + status_str + ")";
            binaries.push_back(info);
        }
    }

    std::cout << "[BinSel] Legacy scan: found " << binaries.size() << " binaries" << std::endl;
}

// ============================================================================
// JSON Registry
// ============================================================================

void BinarySelector::load_registry(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f) {
        std::cout << "[BinSel] No registry found at " << json_path << " (will create on save)" << std::endl;
        return;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto find_value = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos = content.find("\"", pos + search.length() + 1);
        if (pos == std::string::npos) return "";
        size_t end = content.find("\"", pos + 1);
        if (end == std::string::npos) return "";
        return content.substr(pos + 1, end - pos - 1);
    };

    std::string def = find_value("default");
    if (!def.empty()) {
        default_binary = def;
        std::cout << "[BinSel] Default binary: " << default_binary << std::endl;
    }

    for (auto& b : binaries) {
        b.is_default = (b.filepath == default_binary);
    }
}

void BinarySelector::save_registry(const std::string& json_path) {
    std::ofstream f(json_path);
    if (!f) {
        std::cerr << "[BinSel] Cannot write registry to " << json_path << std::endl;
        return;
    }

    f << "{\n";
    f << "  \"default\": \"" << default_binary << "\",\n";
    f << "  \"known_binaries\": {\n";
    for (size_t i = 0; i < binaries.size(); i++) {
        const auto& b = binaries[i];
        const char* status_str = (b.status == BinaryStatus::TESTED) ? "tested" :
                                 (b.status == BinaryStatus::TESTING) ? "testing" : "unknown";
        f << "    \"" << b.sha256 << "\": {\n";
        f << "      \"filepath\": \"" << b.filepath << "\",\n";
        f << "      \"version\": \"" << b.version << "\",\n";
        f << "      \"arch\": \"" << arch_string(b.arch) << "\",\n";
        f << "      \"game_type\": \"" << b.game_type << "\",\n";
        f << "      \"status\": \"" << status_str << "\",\n";
        f << "      \"label\": \"" << b.label << "\"\n";
        f << "    }" << (i + 1 < binaries.size() ? "," : "") << "\n";
    }
    f << "  }\n";
    f << "}\n";

    std::cout << "[BinSel] Registry saved to " << json_path << std::endl;
}

// ============================================================================
// Selection helpers
// ============================================================================

void BinarySelector::set_default(const std::string& filepath) {
    default_binary = filepath;
    for (auto& b : binaries) {
        b.is_default = (b.filepath == filepath);
    }
    
    // Save to ~/.config/swordigo-desktop/default_instance.txt
    std::string user_config_dir;
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config) {
        user_config_dir = std::string(xdg_config) + "/swordigo-desktop";
    } else {
        const char* home = getenv("HOME");
        user_config_dir = std::string(home ? home : ".") + "/.config/swordigo-desktop";
    }
    try {
        fs::create_directories(user_config_dir);
        std::string default_txt_path = user_config_dir + "/default_instance.txt";
        std::ofstream df(default_txt_path);
        if (df) {
            std::string rel_filepath = filepath;
            size_t pos = filepath.find("engine/");
            if (pos != std::string::npos) {
                rel_filepath = filepath.substr(pos);
            }
            df << rel_filepath << std::endl;
        }
    } catch (...) {}
}

const BinaryInfo* BinarySelector::get_loaded_info() const {
    for (const auto& b : binaries) {
        if (b.filepath == loaded_binary) return &b;
    }
    return nullptr;
}

void BinarySelector::set_loaded(const std::string& filepath) {
    loaded_binary = filepath;
}

void BinarySelector::strip_sre_conflicts_from_last() {
    if (binaries.empty()) return;

    auto& b = binaries.back();
    b.dependencies.erase(
        std::remove_if(b.dependencies.begin(), b.dependencies.end(), [](const std::string& d) {
            return d == "libmini.so" || d == "libGlossHook.so" || d == "libkiwi.so";
        }), b.dependencies.end());
    b.dep_paths.erase(
        std::remove_if(b.dep_paths.begin(), b.dep_paths.end(), [](const std::string& p) {
            return p.find("libmini.so") != std::string::npos ||
                   p.find("libGlossHook.so") != std::string::npos ||
                   p.find("libkiwi.so") != std::string::npos;
        }), b.dep_paths.end());
}

// ============================================================================
// Add a custom binary instance from a .so file
// ============================================================================

bool BinarySelector::add_custom_instance(const std::string& so_filepath, const std::string& name, const std::string& assets_dir) {
    // Validate source file exists
    if (!fs::exists(so_filepath) || !fs::is_regular_file(so_filepath)) {
        std::cerr << "[BinSel] Custom .so not found: " << so_filepath << std::endl;
        return false;
    }

    // Read ELF header to detect architecture
    // Byte 4 (e_ident[EI_CLASS]): 1 = ELFCLASS32 (ARM32), 2 = ELFCLASS64 (ARM64)
    std::ifstream elf_file(so_filepath, std::ios::binary);
    if (!elf_file) {
        std::cerr << "[BinSel] Cannot open .so file: " << so_filepath << std::endl;
        return false;
    }

    uint8_t ei_ident[5];
    elf_file.read(reinterpret_cast<char*>(ei_ident), 5);
    if (elf_file.gcount() < 5) {
        std::cerr << "[BinSel] File too small to be an ELF: " << so_filepath << std::endl;
        return false;
    }

    // Verify ELF magic: 0x7f 'E' 'L' 'F'
    if (ei_ident[0] != 0x7f || ei_ident[1] != 'E' || ei_ident[2] != 'L' || ei_ident[3] != 'F') {
        std::cerr << "[BinSel] Not a valid ELF file: " << so_filepath << std::endl;
        return false;
    }

    BinaryArch arch;
    std::string arch_dir;
    if (ei_ident[4] == 1) {
        arch = BinaryArch::ARM32;
        arch_dir = "armeabi-v7a";
    } else if (ei_ident[4] == 2) {
        arch = BinaryArch::ARM64;
        arch_dir = "arm64-v8a";
    } else {
        std::cerr << "[BinSel] Unknown ELF class: " << (int)ei_ident[4] << std::endl;
        return false;
    }
    elf_file.close();

    // Create directory: <data_dir>/engine/custom-NAME/<arch>/
    // Derive data dir from so_filepath (which is absolute, e.g. .../engine/v1.4.12/arm64-v8a/libswordigo.so)
    std::string version_dir = "custom-" + name;
    std::string rel_dest = "engine/" + version_dir + "/" + arch_dir;
    std::string rel_dest_so = rel_dest + "/libswordigo.so";
    
    // Extract base data directory from absolute so_filepath
    // so_filepath = "<data_dir>/engine/<version>/<arch>/libswordigo.so"
    std::string base_data_dir;
    {
        fs::path sp(so_filepath);
        // Walk up to find the "engine" component
        fs::path parent = sp.parent_path(); // <data_dir>/engine/<version>/<arch>
        while (!parent.empty() && parent.filename() != "engine") {
            parent = parent.parent_path();
        }
        if (parent.filename() == "engine") {
            base_data_dir = parent.parent_path().string(); // <data_dir>
        }
    }
    if (base_data_dir.empty()) {
        // Fallback: use parent 4 levels up from so_filepath
        base_data_dir = fs::path(so_filepath).parent_path().parent_path().parent_path().parent_path().string();
    }
    
    // Store data_dir so save_instance_ini() can use it
    const_cast<BinarySelector*>(this)->set_data_dir(base_data_dir);
    
    std::string dest_dir = base_data_dir + "/" + rel_dest;
    std::string dest_path = base_data_dir + "/" + rel_dest_so;

    try {
        fs::create_directories(dest_dir);
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[BinSel] Failed to create directory " << dest_dir << ": " << e.what() << std::endl;
        return false;
    }

    // Copy the .so file
    try {
        fs::copy_file(so_filepath, dest_path, fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[BinSel] Failed to copy .so to " << dest_path << ": " << e.what() << std::endl;
        return false;
    }

    std::cout << "[BinSel] Copied custom binary to " << dest_path << std::endl;

    // Compute SHA256 of the copied file
    std::string sha = compute_sha256(dest_path);
    if (sha.empty()) {
        std::cerr << "[BinSel] Failed to compute SHA256 for " << dest_path << std::endl;
        return false;
    }

    // Build BinaryInfo
    BinaryInfo info;
    info.filename = "libswordigo.so";
    info.filepath = rel_dest_so;
    info.version_dir = version_dir;
    info.arch = arch;
    info.file_size = fs::file_size(dest_path);
    info.sha256 = sha;
    info.is_default = false;

    // Determine game type from assets_dir
    if (assets_dir == "rl_assets") {
        info.game_type = "RLSwordigo";
    } else {
        info.game_type = "Swordigo";
    }
    info.assets_dir = assets_dir;
    if (info.assets_dir.empty()) {
        info.assets_dir = "assets";
    }

    // Look up hash in known hashes for status
    auto it = KNOWN_HASHES.find(info.sha256);
    if (it != KNOWN_HASHES.end()) {
        info.version = it->second.first;
        info.status = it->second.second;
    } else {
        info.version = name;
        info.status = BinaryStatus::UNKNOWN;
    }

    // Build display label
    const char* status_str = (info.status == BinaryStatus::TESTED) ? "Stable" :
                             (info.status == BinaryStatus::TESTING) ? "Testing" : "Unknown";
    std::string arch_badge = std::string("[") + arch_string(arch) + "]";

    if (info.game_type == "RLSwordigo") {
        info.label = "[RL] [Custom] " + name + " " + arch_badge + " (" + status_str + ")";
    } else {
        info.label = "[Custom] " + name + " " + arch_badge + " (" + status_str + ")";
    }

    binaries.push_back(info);
    save_instance_ini(info);

    std::cout << "[BinSel] Added custom instance: " << info.label << " → " << info.filepath << std::endl;
    return true;
}

bool BinarySelector::import_apk_instance(const std::string& apk_path, const std::string& name,
                                         std::string* error_message) {
    auto fail = [&](const std::string& message) {
        if (error_message) *error_message = message;
        std::cerr << "[BinSel/APK] " << message << std::endl;
        return false;
    };
    if (!fs::is_regular_file(apk_path)) return fail("APK file does not exist");
    if (data_dir.empty()) return fail("Launcher data directory is not configured");

    const std::string slug = safe_instance_name(name);
    const std::string version_dir = "apk-" + slug;
    const std::string assets_dir_name = "inst-" + slug;
    const fs::path temp = fs::path(data_dir) / (".apk-import-" + slug);
    const fs::path engine_dest = fs::path(data_dir) / "engine" / version_dir;
    const fs::path assets_dest = fs::path(data_dir) / assets_dir_name;

    try {
        fs::remove_all(temp);
        fs::create_directories(temp);
        // APKs also contain large DEX/resource tables that Swordigo Desktop
        // never consumes. Extract only game assets and native libraries with
        // the bundled ZIP reader (no external unzip/7z required).
        std::string zip_err;
        if (!extract_apk_zip(apk_path, temp, &zip_err)) {
            fs::remove_all(temp);
            return fail(zip_err.empty() ? "Failed to extract APK (invalid or unsupported archive)" : zip_err);
        }
        const fs::path extracted_assets = temp / "assets";
        if (!fs::is_directory(extracted_assets / "resources")) {
            fs::remove_all(temp);
            return fail("APK has no assets/resources directory");
        }
        fs::remove_all(extracted_assets / "dexopt");

        fs::remove_all(engine_dest);

        size_t imported = 0;
        const fs::path lib_root = temp / "lib";
        for (const auto& abi : {std::string("armeabi-v7a"), std::string("arm64-v8a")}) {
            const fs::path source = lib_root / abi;
            if (!fs::is_regular_file(source / "libswordigo.so")) continue;
            const BinaryArch architecture = abi == "arm64-v8a" ? BinaryArch::ARM64 : BinaryArch::ARM32;
            const fs::path destination = engine_dest / abi;
            fs::create_directories(destination);
            for (const auto& library : fs::directory_iterator(source)) {
                if (!library.is_regular_file() || library.path().extension() != ".so") continue;
                fs::copy_file(library.path(), destination / library.path().filename(),
                              fs::copy_options::overwrite_existing);
            }
            const fs::path registered = destination / "libswordigo.so";
            if (!fs::is_regular_file(registered)) return fail("APK ABI is missing libswordigo.so");
            BinaryInfo info;
            info.filename = "libswordigo.so";
            info.filepath = (fs::path("engine") / version_dir / abi / info.filename).string();
            info.version_dir = version_dir;
            info.arch = architecture;
            info.file_size = fs::file_size(registered);
            info.sha256 = compute_sha256(registered.string());
            info.is_default = false;
            info.game_type = "Swordigo";
            info.assets_dir = assets_dir_name;
            info.label = "[APK] " + name + " [" + arch_string(info.arch) + "]";
            info.status = BinaryStatus::UNKNOWN;
            for (const auto& library : fs::directory_iterator(destination)) {
                if (library.path().filename() == "libswordigo.so") continue;
                info.dependencies.push_back(library.path().filename().string());
                info.dep_paths.push_back(library.path().string());
            }
            binaries.push_back(std::move(info));
            save_instance_ini(info);
            ++imported;
        }
        if (imported == 0) {
            fs::remove_all(temp);
            return fail("APK contains no lib/armeabi-v7a or lib/arm64-v8a libswordigo.so");
        }
        // Commit assets only after at least one ABI has registered correctly.
        fs::remove_all(assets_dest);
        fs::rename(extracted_assets, assets_dest);
        for (auto& info : binaries)
            if (info.version_dir == version_dir) save_instance_ini(info);
        fs::remove_all(temp);
        std::cout << "[BinSel/APK] Imported " << imported << " ABI(s), assets -> " << assets_dest << std::endl;
        return true;
    } catch (const std::exception& error) {
        std::error_code ignored;
        fs::remove_all(temp, ignored);
        return fail(error.what());
    }
}

// ============================================================================
// JSON Manifest System — Hand-rolled JSON (no external dependency)
// ============================================================================

// Serialize a single BinaryInfo to JSON object string
std::string BinarySelector::instance_to_json(const BinaryInfo& b, int indent) {
    std::string pad(indent, ' ');
    std::string pad2(indent + 2, ' ');
    std::ostringstream ss;
    
    ss << pad << "{\n";
    ss << pad2 << "\"name\": \"" << b.label << "\",\n";
    ss << pad2 << "\"filename\": \"" << b.filename << "\",\n";
    ss << pad2 << "\"filepath\": \"" << b.filepath << "\",\n";
    ss << pad2 << "\"sha256\": \"" << b.sha256 << "\",\n";
    ss << pad2 << "\"version\": \"" << b.version << "\",\n";
    ss << pad2 << "\"version_dir\": \"" << b.version_dir << "\",\n";
    ss << pad2 << "\"arch\": \"" << arch_string(b.arch) << "\",\n";
    ss << pad2 << "\"file_size\": " << b.file_size << ",\n";
    ss << pad2 << "\"is_default\": " << (b.is_default ? "true" : "false") << ",\n";
    
    const char* st = (b.status == BinaryStatus::TESTED) ? "tested" :
                     (b.status == BinaryStatus::TESTING) ? "testing" : "unknown";
    ss << pad2 << "\"status\": \"" << st << "\",\n";
    ss << pad2 << "\"game_type\": \"" << b.game_type << "\",\n";
    ss << pad2 << "\"assets_dir\": \"" << b.assets_dir << "\",\n";
    ss << pad2 << "\"icon_path\": \"" << b.icon_path << "\",\n";
    
    // Dependencies array
    ss << pad2 << "\"dependencies\": [";
    for (size_t i = 0; i < b.dependencies.size(); i++) {
        if (i > 0) ss << ", ";
        ss << "\"" << b.dependencies[i] << "\"";
    }
    ss << "],\n";
    
    // Dep paths array
    ss << pad2 << "\"dep_paths\": [";
    for (size_t i = 0; i < b.dep_paths.size(); i++) {
        if (i > 0) ss << ", ";
        ss << "\"" << b.dep_paths[i] << "\"";
    }
    ss << "]\n";
    
    ss << pad << "}";
    return ss.str();
}

// Simple JSON string value extractor — finds "key": "value" in a JSON block
static std::string json_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\": \"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Simple JSON integer extractor — finds "key": 12345
static size_t json_int(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\": ";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    // Skip non-digits
    while (pos < json.size() && json[pos] == ' ') pos++;
    size_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return val;
}

// Simple JSON bool extractor
static bool json_bool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\": ";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    return json.substr(pos, 4) == "true";
}

// Extract a simple string array: "key": ["a", "b"]
static std::vector<std::string> json_str_array(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\": [";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return result;
    pos += needle.size();
    auto end = json.find(']', pos);
    if (end == std::string::npos) return result;
    std::string arr = json.substr(pos, end - pos);
    // Parse quoted strings
    size_t p = 0;
    while (p < arr.size()) {
        auto q1 = arr.find('"', p);
        if (q1 == std::string::npos) break;
        auto q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        p = q2 + 1;
    }
    return result;
}

// Parse a single JSON object block into BinaryInfo
BinaryInfo BinarySelector::parse_instance_json(const std::string& json) {
    BinaryInfo b;
    b.label = json_str(json, "name");
    b.filename = json_str(json, "filename");
    b.filepath = json_str(json, "filepath");
    b.sha256 = json_str(json, "sha256");
    b.version = json_str(json, "version");
    b.version_dir = json_str(json, "version_dir");
    b.file_size = json_int(json, "file_size");
    b.is_default = json_bool(json, "is_default");
    b.game_type = json_str(json, "game_type");
    b.assets_dir = json_str(json, "assets_dir");
    if (b.assets_dir.empty()) {
        b.assets_dir = "assets";
    }
    b.icon_path = json_str(json, "icon_path");
    b.dependencies = json_str_array(json, "dependencies");
    b.dep_paths = json_str_array(json, "dep_paths");
    
    std::string arch_s = json_str(json, "arch");
    b.arch = (arch_s == "ARM64") ? BinaryArch::ARM64 : BinaryArch::ARM32;
    
    std::string status_s = json_str(json, "status");
    b.status = (status_s == "tested") ? BinaryStatus::TESTED :
               (status_s == "testing") ? BinaryStatus::TESTING : BinaryStatus::UNKNOWN;
    
    return b;
}

// Split a JSON array of objects into individual object blocks
static std::vector<std::string> split_json_objects(const std::string& content) {
    std::vector<std::string> objects;
    int depth = 0;
    size_t start = 0;
    bool in_array = false;
    
    for (size_t i = 0; i < content.size(); i++) {
        char c = content[i];
        if (!in_array && c == '[') { in_array = true; continue; }
        if (!in_array) continue;
        
        if (c == '{') {
            if (depth == 0) start = i;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                objects.push_back(content.substr(start, i - start + 1));
            }
        }
    }
    return objects;
}

static std::map<std::string, std::string> parse_ini(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    if (!f) return kv;
    std::string line;
    while (std::getline(f, line)) {
        size_t comment = line.find(';');
        if (comment != std::string::npos) line = line.substr(0, comment);
        comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty() || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        val.erase(0, val.find_first_not_of(" \t\r\n"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);
        
        if (val.length() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.length() - 2);
        }
        
        kv[key] = val;
    }
    return kv;
}

static std::vector<std::string> split_comma_string(const std::string& str) {
    std::vector<std::string> res;
    if (str.empty()) return res;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t\r\n"));
        token.erase(token.find_last_not_of(" \t\r\n") + 1);
        if (!token.empty()) {
            res.push_back(token);
        }
    }
    return res;
}

void BinarySelector::load_manifest(const std::string& manifest_path) {
    fs::path engine_dir = fs::path(manifest_path).parent_path();
    if (!fs::exists(engine_dir) || !fs::is_directory(engine_dir)) {
        std::cerr << "[BinSel] Engine directory not found: " << engine_dir << std::endl;
        return;
    }

    std::string user_config_dir;
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config) {
        user_config_dir = std::string(xdg_config) + "/swordigo-desktop";
    } else {
        const char* home = getenv("HOME");
        user_config_dir = std::string(home ? home : ".") + "/.config/swordigo-desktop";
    }
    std::string default_txt_path = user_config_dir + "/default_instance.txt";
    std::string saved_default_filepath;
    if (fs::exists(default_txt_path)) {
        std::ifstream df(default_txt_path);
        if (df) {
            std::getline(df, saved_default_filepath);
            saved_default_filepath.erase(0, saved_default_filepath.find_first_not_of(" \t\r\n"));
            saved_default_filepath.erase(saved_default_filepath.find_last_not_of(" \t\r\n") + 1);
        }
    }

    std::string base_str = engine_dir.parent_path().string();
    if (!base_str.empty() && base_str.back() != '/') base_str += "/";

    int loaded = 0;
    
    // Scan exactly 2 levels deep: engine/<instance>/<arch>/instance.ini
    for (const auto& inst_entry : fs::directory_iterator(engine_dir)) {
        if (!inst_entry.is_directory()) continue;
        
        for (const auto& arch_entry : fs::directory_iterator(inst_entry.path())) {
            if (!arch_entry.is_directory()) continue;
            
            fs::path ini_path = arch_entry.path() / "instance.ini";
            if (!fs::exists(ini_path) || !fs::is_regular_file(ini_path)) continue;
            
            std::string ini_str = ini_path.string();
            fs::path arch_path = arch_entry.path();
            
            auto kv = parse_ini(ini_str);
            if (kv.empty()) continue;
            
            // Check schema version compatibility
            std::string schema_str = kv["schema_version"];
            if (!schema_str.empty()) {
                try {
                    int schema_ver = std::stoi(schema_str);
                    if (schema_ver > 1) {
                        std::cerr << "[BinSel] Skipped " << ini_str << " (unsupported schema version: " << schema_str << ")" << std::endl;
                        continue;
                    }
                } catch (...) {
                    // Treat as legacy/invalid but check if we can load it
                }
            }
            
            BinaryInfo b;
            b.filename = kv["filename"].empty() ? "libswordigo.so" : kv["filename"];
            
            std::string filepath = kv["filepath"];
            if (!filepath.empty() && filepath[0] != '/') {
                b.filepath = base_str + filepath;
            } else {
                b.filepath = filepath;
            }
            
            if (b.filepath.empty()) {
                fs::path so_path = arch_path / b.filename;
                b.filepath = so_path.string();
            }

            b.sha256 = kv["sha256"];
            b.version = kv["version"];
            b.label = kv["name"];
            b.version_dir = kv["version_dir"];
            
            std::string arch_str = kv["arch"];
            if (arch_str == "ARM64" || arch_str == "arm64-v8a") {
                b.arch = BinaryArch::ARM64;
            } else {
                b.arch = BinaryArch::ARM32;
            }

            std::string fs_str = kv["file_size"];
            b.file_size = fs_str.empty() ? 0 : std::stoull(fs_str);
            
            b.is_default = (kv["is_default"] == "true");
            if (!saved_default_filepath.empty() && (filepath == saved_default_filepath || b.filepath == saved_default_filepath || (base_str + filepath) == saved_default_filepath)) {
                b.is_default = true;
            }
            if (b.is_default) {
                default_binary = b.filepath;
            }

            std::string status_str = kv["status"];
            if (status_str == "tested") {
                b.status = BinaryStatus::TESTED;
            } else if (status_str == "testing") {
                b.status = BinaryStatus::TESTING;
            } else {
                b.status = BinaryStatus::UNKNOWN;
            }

            b.game_type = kv["game_type"];
            b.assets_dir = kv["assets_dir"];
            if (b.assets_dir.empty()) {
                b.assets_dir = "assets";
            }
            b.icon_path = kv["icon_path"];

            b.dependencies = split_comma_string(kv["dependencies"]);
            b.dep_paths = split_comma_string(kv["dep_paths"]);

            for (auto& dp : b.dep_paths) {
                if (!dp.empty() && dp[0] != '/') {
                    dp = base_str + dp;
                }
            }

            // Safety check 1: ARM32 instances cannot carry libsre.so
            if (b.arch == BinaryArch::ARM32) {
                b.dependencies.erase(
                    std::remove_if(b.dependencies.begin(), b.dependencies.end(),
                        [](const std::string& d) {
                            return d == "libsre.so";
                        }), b.dependencies.end());
                b.dep_paths.erase(
                    std::remove_if(b.dep_paths.begin(), b.dep_paths.end(),
                        [](const std::string& p) {
                            return p.find("libsre.so") != std::string::npos;
                        }), b.dep_paths.end());
            }

            // Safety check 2: if an instance has libsre.so, remove conflicting modloaders
            bool has_sre = false;
            for (const auto& d : b.dependencies) {
                if (d == "libsre.so") { has_sre = true; break; }
            }
            if (has_sre) {
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

            if (b.dependencies.empty()) {
                for (const auto& f_entry : fs::directory_iterator(arch_path)) {
                    if (!f_entry.is_regular_file()) continue;
                    std::string fn = f_entry.path().filename().string();
                    if (fn != b.filename && fn.length() > 3 && fn.substr(fn.length() - 3) == ".so") {
                        b.dependencies.push_back(fn);
                        b.dep_paths.push_back(f_entry.path().string());
                    }
                }
                
                if (fs::exists(arch_path / "libsre.so")) {
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
            }

            if (b.sha256.empty() && fs::exists(b.filepath)) {
                b.sha256 = compute_sha256(b.filepath);
            }
            if (b.file_size == 0 && fs::exists(b.filepath)) {
                b.file_size = fs::file_size(b.filepath);
            }

            // Check for duplicates
            bool dup = false;
            for (const auto& existing : binaries) {
                if (existing.filepath == b.filepath) { dup = true; break; }
            }
            if (dup) continue;

            binaries.push_back(b);
            loaded++;
        }
    }
    
    std::cout << "[BinSel] Loaded " << loaded << " instances from local instance.ini files" << std::endl;
}

void BinarySelector::load_user_instances(const std::string& json_path) {
    // NO-OP: Handled dynamically by scanning directory in load_manifest
}

void BinarySelector::reload_instances() {
    if (data_dir.empty()) return;
    binaries.clear();
    scan_engine_directory((fs::path(data_dir) / "engine").string());
}

void BinarySelector::save_user_instances(const std::string& json_path) const {
    for (const auto& b : binaries) {
        save_instance_ini(b);
    }
}

void BinarySelector::save_instance_ini(const BinaryInfo& b) const {
    if (b.filepath.empty()) return;
    
    // Build absolute path: data_dir + filepath
    std::string abs_so_path = b.filepath;
    if (!data_dir.empty() && !fs::path(b.filepath).is_absolute()) {
        abs_so_path = data_dir + "/" + b.filepath;
    }
    
    fs::path so_path(abs_so_path);
    fs::path dir_path = so_path.parent_path();
    if (!fs::exists(dir_path)) {
        try {
            fs::create_directories(dir_path);
        } catch (const std::exception& e) {
            std::cerr << "[BinSel] Failed to create directory " << dir_path << ": " << e.what() << std::endl;
            return;
        }
    }
    
    fs::path ini_path = dir_path / "instance.ini";
    std::ofstream f(ini_path);
    if (!f) {
        std::cerr << "[BinSel] Cannot write instance.ini to: " << ini_path << std::endl;
        return;
    }
    
    f << "[instance]\n";
    f << "schema_version = 1\n";
    f << "name = " << b.label << "\n";
    f << "filename = " << b.filename << "\n";
    
    std::string rel_filepath = b.filepath;
    size_t pos = rel_filepath.find("engine/");
    if (pos != std::string::npos) {
        rel_filepath = rel_filepath.substr(pos);
    }
    f << "filepath = " << rel_filepath << "\n";
    f << "sha256 = " << b.sha256 << "\n";
    f << "version = " << b.version << "\n";
    f << "version_dir = " << b.version_dir << "\n";
    f << "arch = " << arch_string(b.arch) << "\n";
    f << "file_size = " << b.file_size << "\n";
    f << "is_default = " << (b.is_default ? "true" : "false") << "\n";
    
    const char* status_str = (b.status == BinaryStatus::TESTED) ? "tested" :
                             (b.status == BinaryStatus::TESTING) ? "testing" : "unknown";
    f << "status = " << status_str << "\n";
    f << "game_type = " << b.game_type << "\n";
    f << "assets_dir = " << b.assets_dir << "\n";
    f << "icon_path = " << b.icon_path << "\n";
    
    f << "dependencies = ";
    for (size_t i = 0; i < b.dependencies.size(); i++) {
        f << b.dependencies[i];
        if (i + 1 < b.dependencies.size()) f << ", ";
    }
    f << "\n";
    
    f << "dep_paths = ";
    for (size_t i = 0; i < b.dep_paths.size(); i++) {
        std::string dp = b.dep_paths[i];
        size_t d_pos = dp.find("engine/");
        if (d_pos != std::string::npos) {
            f << dp.substr(d_pos);
        } else {
            f << dp;
        }
        if (i + 1 < b.dep_paths.size()) f << ", ";
    }
    f << "\n";
}

// ── generate_manifest: Scan engine/ dir and write manifest.json ──
// This is called by the packaging scriptlet, NOT at runtime!
void BinarySelector::generate_manifest(const std::string& engine_path, const std::string& output_path) {
    // Clear and do a fresh scan
    binaries.clear();
    scan_engine_directory(engine_path);
    
    if (binaries.empty()) {
        std::cerr << "[BinSel] No binaries found in " << engine_path << " — no manifest written." << std::endl;
        return;
    }
    
    // Write the manifest
    std::ofstream f(output_path);
    if (!f) {
        std::cerr << "[BinSel] Cannot write manifest to: " << output_path << std::endl;
        return;
    }
    
    f << "{\n";
    f << "  \"default\": \"" << default_binary << "\",\n";
    f << "  \"instances\": [\n";
    for (size_t i = 0; i < binaries.size(); i++) {
        f << instance_to_json(binaries[i], 4);
        if (i + 1 < binaries.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
    
    std::cout << "[BinSel] Generated manifest with " << binaries.size()
              << " instances → " << output_path << std::endl;
}
