/* ruby_cli.cpp — native FileRift-compatible command line tool (Ruby CLI).
 *
 * Mirrors FileRift 5.8.5's standalone CLI (decode / recode / both / user /
 * force / build / info) plus Ruby-specific extensions (APK extract/sign/build
 * and a headless batch texture converter). No Python subprocess is used; the
 * decode/recode backend is the native filerift library.
 *
 * Usage:
 *   ruby_cli [mode] [paths...] [options]
 *     -r, --recode [paths]      recode markup -> binary (default re_in)
 *     -d, --decode [paths]      decode binary -> markup (default de_in)
 *     -u, --user                decode de_in/<user_folder>
 *     --both                    recode then decode
 *     -f, --force [paths]       recode with allways_recode
 *     -b, --build [project]     build an APK from a .frproject
 *     -i, --info [query]        info / help
 *     --recode-stdin            recode from stdin
 *     --decode-stdin            decode from stdin
 *     -o, --output PATH         output file or directory
 *     -t, --file-type TYPE      force file type (stdin)
 *     -w, --working-dir DIR     working directory
 *     -n, --no-colour           disable ANSI colours
 *     --compile MODE            never|trigger|all|auto|skip
 *     --snake                   snake-case tags (presentation)
 *     --apksigner PATH          path to Android apksigner.jar
 *
 *   ruby_cli apk extract <apk> <dest_dir>
 *   ruby_cli apk sign <apk> [--apksigner PATH]
 *   ruby_cli apk build <project.frproject> [--apksigner PATH]
 *
 *   ruby_cli batch [--import] <src_dir> <dst_dir> [options]
 */

#include "tools/ruby_cli.h"
#include "tools/filerift.h"
#include "tools/batch_converter.h"
#include "tools/ruby_mcp.h"
#include "tools/scene_creator.h"
#include "tools/map_loader.h"

#include <zlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <regex>
#include <functional>

namespace fs = std::filesystem;

namespace rubycli {

// ─── ANSI colours (mirror config.py defaults) ────────────────────────────────
static const char* C_SUCCESS      = "\033[0m\033[1;32m";
static const char* C_ERROR        = "\033[0m\033[1;31m";
static const char* C_WARNING      = "\033[0m\033[1;33m";
static const char* C_DATA         = "\033[0m\033[1;34m";
static const char* C_PUNCTUATION  = "\033[0m\033[2;37m";
static const char* C_RESET        = "\033[0m";

static void init_colours(const Options& opt) {
    if (opt.no_colour) {
        C_SUCCESS = C_ERROR = C_WARNING = C_DATA = C_PUNCTUATION = C_RESET = "";
    }
}

// ─── File / path helpers ─────────────────────────────────────────────────────

static bool read_bytes(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool write_bytes(const std::string& path, const std::string& data) {
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return f.good();
}

static bool write_text(const std::string& path, const std::string& text) {
    return write_bytes(path, text);
}

// ─── Filetype detection (mirror FileRift: extension after last dot) ──────────

static const std::set<std::string>& supported_types() {
    static const std::set<std::string> types = {
        "fr", "scene", "scl", "gdata", "gopt", "gplayer",
        "gstate", "scmap", "sounds", "fnt", "atlas",
    };
    return types;
}

std::string filetype_for_path(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    size_t dot = lower.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= lower.size()) return "fr";
    std::string ext = lower.substr(dot + 1);
    if (!supported_types().count(ext)) return "";
    return ext;
}

// ─── Single file decode/recode (shared backend) ─────────────────────────────

bool decode_file(const std::string& path, const Options& opt,
                 std::string& markup, std::string& error) {
    (void)opt;
    std::string bytes;
    if (!read_bytes(path, bytes)) { error = "cannot read file"; return false; }
    if (bytes.empty()) { error = "empty file"; return false; }
    std::string filetype = filetype_for_path(path);
    if (filetype.empty()) { error = "unrecognized extension"; return false; }
    try {
        markup = filerift::decode_protobuf(bytes, filetype);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool recode_file(const std::string& path, const Options& opt,
                 std::string& bytes, std::string& error) {
    (void)opt;
    std::string markup;
    if (!read_bytes(path, markup)) { error = "cannot read file"; return false; }
    std::string filetype = filetype_for_path(path);
    if (filetype.empty()) { error = "unrecognized extension"; return false; }
    try {
        bytes = filerift::recode_markup(markup, filetype);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (bytes.empty()) { error = "empty recoded output"; return false; }
    return true;
}

// ─── Directory walk (common backend for full-directory decode/recode) ────────

static void walk_dir(const std::string& dir,
                     std::vector<fs::path>& out,
                     bool recursive) {
    std::error_code ec;
    if (recursive) {
        for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (e.is_regular_file(ec)) out.push_back(e.path());
        }
    } else {
        for (auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (e.is_regular_file(ec)) out.push_back(e.path());
        }
    }
}

int decode_directory(const std::string& in_dir, const std::string& out_dir,
                     const Options& opt) {
    std::vector<fs::path> files;
    walk_dir(in_dir, files, true);
    int handled = 0;
    for (auto& f : files) {
        if (filetype_for_path(f.string()).empty()) continue;
        std::string rel = fs::relative(f, fs::path(in_dir)).string();
        std::string dst = (fs::path(out_dir) / rel).string();
        std::string markup, error;
        if (!decode_file(f.string(), opt, markup, error)) {
            std::printf("%s×%s %s %s(%s)%s\n", C_ERROR, C_RESET,
                        rel.c_str(), C_PUNCTUATION, error.c_str(), C_RESET);
            continue;
        }
        if (!write_text(dst, markup)) {
            std::printf("%s×%s %s %s(cannot write output)%s\n",
                        C_ERROR, C_RESET, rel.c_str(), C_PUNCTUATION, C_RESET);
            continue;
        }
        std::printf("%s✔%s %s %s->%s %s\n", C_SUCCESS, C_RESET, rel.c_str(),
                    C_PUNCTUATION, C_RESET, dst.c_str());
        ++handled;
    }
    return handled;
}

int recode_directory(const std::string& in_dir, const std::string& out_dir,
                     const Options& opt) {
    std::vector<fs::path> files;
    walk_dir(in_dir, files, true);
    int handled = 0;
    for (auto& f : files) {
        if (filetype_for_path(f.string()).empty()) continue;
        std::string rel = fs::relative(f, fs::path(in_dir)).string();
        std::string dst = (fs::path(out_dir) / rel).string();
        std::string bytes, error;
        if (!recode_file(f.string(), opt, bytes, error)) {
            std::printf("%s×%s %s %s(%s)%s\n", C_ERROR, C_RESET,
                        rel.c_str(), C_PUNCTUATION, error.c_str(), C_RESET);
            continue;
        }
        if (!write_bytes(dst, bytes)) {
            std::printf("%s×%s %s %s(cannot write output)%s\n",
                        C_ERROR, C_RESET, rel.c_str(), C_PUNCTUATION, C_RESET);
            continue;
        }
        std::printf("%s✔%s %s %s->%s %s\n", C_SUCCESS, C_RESET, rel.c_str(),
                    C_PUNCTUATION, C_RESET, dst.c_str());
        ++handled;
    }
    return handled;
}

// ════════════════════════════════════════════════════════════════════════════
// Minimal ZIP reader/writer (needed for APK extract + build). Store + deflate.
// ════════════════════════════════════════════════════════════════════════════

namespace zip {

#pragma pack(push, 1)
struct LocalHeader {
    uint32_t sig;        // 0x04034b50
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t name_len;
    uint16_t extra_len;
};
struct CentralHeader {
    uint32_t sig;        // 0x02014b50
    uint16_t version_made;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t name_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint16_t disk_start;
    uint16_t internal_attr;
    uint32_t external_attr;
    uint32_t local_offset;
};
#pragma pack(pop)

struct Entry {
    std::string name;
    uint16_t    method = 0;
    uint32_t    crc32 = 0;
    uint32_t    comp_size = 0;
    uint32_t    uncomp_size = 0;
    uint64_t    data_offset = 0;
    std::string data;   // decompressed payload (when read via read_entry)
};

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static void wr16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xff)); v.push_back((uint8_t)(x >> 8));
}
static void wr32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xff)); v.push_back((uint8_t)((x >> 8) & 0xff));
    v.push_back((uint8_t)((x >> 16) & 0xff)); v.push_back((uint8_t)((x >> 24) & 0xff));
}

static bool inflate_raw(const std::string& in, std::string& out, size_t expected) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in  = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    out.resize(expected);
    zs.next_out  = (Bytef*)out.data();
    zs.avail_out = (uInt)out.size();
    int rc = inflate(&zs, Z_FINISH);
    bool ok = (rc == Z_STREAM_END || (rc == Z_OK && zs.avail_out == 0));
    if (!ok) out.clear();
    inflateEnd(&zs);
    return ok;
}

static bool deflate_raw(const std::string& in, std::string& out) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    zs.next_in  = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    std::vector<uint8_t> buf(65536);
    int rc;
    do {
        zs.next_out  = buf.data();
        zs.avail_out = (uInt)buf.size();
        rc = deflate(&zs, Z_FINISH);
        size_t got = buf.size() - zs.avail_out;
        if (got) out.append((const char*)buf.data(), got);
    } while (rc == Z_OK);
    deflateEnd(&zs);
    return rc == Z_STREAM_END;
}

// Read all central-directory entries. On success returns true and fills `entries`
// with name + compressed sizes + data_offset (already past the local header).
static bool read_entries(const std::string& zip_path, std::vector<Entry>& entries) {
    std::string buf;
    if (!read_bytes(zip_path, buf)) return false;
    if (buf.size() < 22) return false;

    // Locate end-of-central-directory within last 64 KiB + 22.
    size_t eocd = std::string::npos;
    size_t start = buf.size() >= (22 + 65535) ? buf.size() - (22 + 65535) : 0;
    for (size_t i = buf.size() - 22 + 1; i-- > start;) {
        if ((uint8_t)buf[i] == 0x50 && (uint8_t)buf[i + 1] == 0x4b &&
            (uint8_t)buf[i + 2] == 0x05 && (uint8_t)buf[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return false;

    const uint8_t* e = (const uint8_t*)buf.data() + eocd;
    uint16_t count = rd16(e + 10);
    uint32_t cd_off = rd32(e + 16);

    for (uint16_t n = 0; n < count; ++n) {
        if (cd_off + 46 > buf.size()) return false;
        const uint8_t* c = (const uint8_t*)buf.data() + cd_off;
        if (rd32(c) != 0x02014b50) return false;
        Entry ent;
        ent.method      = rd16(c + 10);
        ent.crc32       = rd32(c + 16);
        ent.comp_size   = rd32(c + 20);
        ent.uncomp_size = rd32(c + 24);
        uint16_t name_len  = rd16(c + 28);
        uint16_t extra_len = rd16(c + 30);
        uint16_t comm_len  = rd16(c + 32);
        uint32_t local_off = rd32(c + 42);
        if (cd_off + 46 + name_len > buf.size()) return false;
        ent.name.assign((const char*)(c + 46), name_len);
        ent.data_offset = local_off;

        // Resolve actual data offset from the local header.
        if (local_off + 30 <= buf.size()) {
            const uint8_t* l = (const uint8_t*)buf.data() + local_off;
            if (rd32(l) == 0x04034b50) {
                uint16_t lname = rd16(l + 26);
                uint16_t lextra = rd16(l + 28);
                ent.data_offset = local_off + 30 + lname + lextra;
            }
        }
        entries.push_back(std::move(ent));
        cd_off += 46 + name_len + extra_len + comm_len;
    }
    return true;
}

// Extract entry `e` payload (raw stored/deflate bytes -> decompressed).
static bool read_entry(const std::string& zip_path, const Entry& e, std::string& out) {
    std::string buf;
    if (!read_bytes(zip_path, buf)) return false;
    if (e.data_offset + e.comp_size > buf.size()) return false;
    std::string raw = buf.substr(e.data_offset, e.comp_size);
    if (e.method == 0) {
        out = raw;
        return out.size() == e.uncomp_size;
    }
    if (e.method == 8) return inflate_raw(raw, out, e.uncomp_size);
    return false;
}

struct OutEntry {
    std::string name;
    std::string data;   // uncompressed
    uint16_t    method; // 0 store, 8 deflate
};

// Write a ZIP archive containing `entries`. Always stores directories implicitly
// via the paths in entry names; caller adds explicit directory entries if needed.
static bool write_archive(const std::string& out_path,
                          const std::vector<OutEntry>& in_entries) {
    std::vector<OutEntry> entries = in_entries;
    std::vector<uint8_t> body;
    std::vector<uint8_t> central;

    uint16_t mtime = 0, mdate = 0;
    std::time_t now = std::time(nullptr);
    std::tm* tmv = std::localtime(&now);
    if (tmv) {
        mtime = (uint16_t)(((uint16_t)tmv->tm_hour << 11) |
                           ((uint16_t)tmv->tm_min << 5) |
                           ((uint16_t)tmv->tm_sec / 2));
        mdate = (uint16_t)((((uint16_t)(tmv->tm_year + 1900) - 1980) << 9) |
                           ((uint16_t)(tmv->tm_mon + 1) << 5) |
                           (uint16_t)tmv->tm_mday);
    }

    uint32_t local_offset = 0;
    for (auto& e : entries) {
        std::string payload;
        uint32_t crc = 0;
        uint32_t comp = 0;
        uint16_t method = e.method;
        if (e.method == 0) {
            payload = e.data;
            comp = (uint32_t)e.data.size();
        } else {
            if (!deflate_raw(e.data, payload)) return false;
            comp = (uint32_t)payload.size();
        }
        crc = (uint32_t)crc32(0L, (const Bytef*)e.data.data(), (uInt)e.data.size());

        LocalHeader lh;
        std::memset(&lh, 0, sizeof(lh));
        lh.sig = 0x04034b50;
        lh.version_needed = 20;
        lh.method = method;
        lh.mod_time = mtime;
        lh.mod_date = mdate;
        lh.crc32 = crc;
        lh.comp_size = comp;
        lh.uncomp_size = (uint32_t)e.data.size();
        lh.name_len = (uint16_t)e.name.size();
        wr32(body, lh.sig);
        wr16(body, lh.version_needed);
        wr16(body, lh.flags);
        wr16(body, lh.method);
        wr16(body, lh.mod_time);
        wr16(body, lh.mod_date);
        wr32(body, lh.crc32);
        wr32(body, lh.comp_size);
        wr32(body, lh.uncomp_size);
        wr16(body, lh.name_len);
        wr16(body, lh.extra_len);
        body.insert(body.end(), e.name.begin(), e.name.end());
        body.insert(body.end(), payload.begin(), payload.end());

        CentralHeader ch;
        std::memset(&ch, 0, sizeof(ch));
        ch.sig = 0x02014b50;
        ch.version_made = 20;
        ch.version_needed = 20;
        ch.method = method;
        ch.mod_time = mtime;
        ch.mod_date = mdate;
        ch.crc32 = crc;
        ch.comp_size = comp;
        ch.uncomp_size = (uint32_t)e.data.size();
        ch.name_len = (uint16_t)e.name.size();
        ch.local_offset = local_offset;
        wr32(central, ch.sig);
        wr16(central, ch.version_made);
        wr16(central, ch.version_needed);
        wr16(central, ch.flags);
        wr16(central, ch.method);
        wr16(central, ch.mod_time);
        wr16(central, ch.mod_date);
        wr32(central, ch.crc32);
        wr32(central, ch.comp_size);
        wr32(central, ch.uncomp_size);
        wr16(central, ch.name_len);
        wr16(central, ch.extra_len);
        wr16(central, ch.comment_len);
        wr16(central, ch.disk_start);
        wr16(central, ch.internal_attr);
        wr32(central, ch.external_attr);
        wr32(central, ch.local_offset);
        central.insert(central.end(), e.name.begin(), e.name.end());

        local_offset += 30 + (uint32_t)e.name.size() + comp;
    }

    std::vector<uint8_t> out;
    out.insert(out.end(), body.begin(), body.end());
    uint32_t cd_off = (uint32_t)body.size();
    out.insert(out.end(), central.begin(), central.end());
    // EOCD
    wr32(out, 0x06054b50);
    wr16(out, 0);
    wr16(out, 0);
    wr16(out, (uint16_t)entries.size());
    wr16(out, (uint16_t)entries.size());
    wr32(out, (uint32_t)central.size());
    wr32(out, cd_off);
    wr16(out, 0);

    return write_bytes(out_path, std::string((char*)out.data(), out.size()));
}

} // namespace zip

// ════════════════════════════════════════════════════════════════════════════
// APK extract / sign / build
// ════════════════════════════════════════════════════════════════════════════

int apk_extract(const std::string& apk_path, const std::string& dest_dir) {
    std::vector<zip::Entry> entries;
    if (!zip::read_entries(apk_path, entries)) {
        std::fprintf(stderr, "apk: cannot read %s (not a valid ZIP)\n", apk_path.c_str());
        return 1;
    }
    std::error_code ec;
    fs::create_directories(dest_dir, ec);

    int count = 0;
    for (auto& e : entries) {
        // Safe path: strip traversal, reject absolute.
        std::string cleaned;
        for (char c : e.name) cleaned += (c == '\\') ? '/' : c;
        while (cleaned.rfind("../", 0) == 0) cleaned.erase(0, 3);
        if (cleaned.empty() || cleaned[0] == '/') continue;
        if (cleaned.back() == '/') { // directory entry
            fs::create_directories(fs::path(dest_dir) / cleaned, ec);
            continue;
        }
        std::string data;
        if (!zip::read_entry(apk_path, e, data)) {
            std::fprintf(stderr, "apk: failed to extract %s\n", e.name.c_str());
            continue;
        }
        fs::path target = fs::path(dest_dir) / cleaned;
        fs::create_directories(target.parent_path(), ec);
        if (write_bytes(target.string(), data)) {
            ++count;
        } else {
            std::fprintf(stderr, "apk: cannot write %s\n", target.string().c_str());
        }
    }
    std::printf("%s✔%s extracted %d file(s) -> %s\n", C_SUCCESS, C_RESET,
                count, dest_dir.c_str());
    return 0;
}

static int run_process(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    if (rc == -1) return 127;
#ifdef WIFEXITED
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
#endif
    return rc;
}

static std::string quote_shell(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

int apk_sign(const std::string& apk_path, const std::string& apksigner) {
    if (apksigner.empty()) {
        std::fprintf(stderr, "apk: no apksigner.jar path (use --apksigner)\n");
        return 1;
    }
    if (!fs::exists(apksigner)) {
        std::fprintf(stderr, "apk: apksigner.jar not found: %s\n", apksigner.c_str());
        return 1;
    }
    if (!fs::exists(apk_path)) {
        std::fprintf(stderr, "apk: file not found: %s\n", apk_path.c_str());
        return 1;
    }

    std::string cmd = "java -jar " + quote_shell(apksigner) + " -a " +
                      quote_shell(apk_path);
    std::printf("%srunning:%s %s\n", C_WARNING, C_RESET, cmd.c_str());
    int rc = run_process(cmd);
    if (rc != 0) {
        std::fprintf(stderr, "apk: signing failed (exit %d)\n", rc);
        return 1;
    }

    // apksigner writes out-aligned-debugSigned.apk next to the target.
    std::string base = apk_path;
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".apk") == 0)
        base = base.substr(0, base.size() - 4);
    std::string signed_path = base + "-aligned-debugSigned.apk";
    std::string idsig = signed_path + ".idsig";
    std::error_code ec;
    if (fs::exists(signed_path, ec)) {
        fs::copy_file(signed_path, apk_path, fs::copy_options::overwrite_existing, ec);
        fs::remove(signed_path, ec);
    }
    fs::remove(idsig, ec);
    std::printf("%s✔%s signed %s\n", C_SUCCESS, C_RESET, apk_path.c_str());
    return 0;
}

// ─── .frproject parsing + build (mirror FileRift build.py) ───────────────────

struct ProjectConfig {
    std::string base_apk;
    std::string out_apk;
    bool sign = true;
    std::string command_prefix;
    std::string command_suffix;
    std::vector<std::pair<std::string, std::string>> add_files;    // src, target
    std::vector<std::pair<std::string, std::string>> recode_files; // src, target
};

static std::string strip_comments(const std::string& content) {
    std::istringstream ss(content);
    std::string line, out;
    while (std::getline(ss, line)) {
        for (const char* mark : {"//", "--", "#"}) {
            size_t p = line.find(mark);
            if (p != std::string::npos) line = line.substr(0, p);
        }
        out += line + "\n";
    }
    return out;
}

static bool parse_project_file(const std::string& content, ProjectConfig& cfg) {
    std::string c = strip_comments(content);

    auto get_quoted = [&](const std::string& key) -> std::string {
        std::regex re(key + "\\s*\"([^\"]+)\"");
        std::smatch m;
        if (std::regex_search(c, m, re)) return m[1].str();
        return "";
    };

    cfg.base_apk = get_quoted("base");
    cfg.out_apk  = get_quoted("out");
    cfg.command_prefix = get_quoted("command_prefix");
    cfg.command_suffix = get_quoted("command_suffix");

    std::regex sign_re("sign\\s*\"(true|false)\"");
    std::smatch m;
    if (std::regex_search(c, m, sign_re)) cfg.sign = (m[1].str() == "true");

    auto parse_section = [&](const std::string& key,
                             std::vector<std::pair<std::string, std::string>>& out) {
        std::regex sec_re(key + "\\s*\\{([^}]*)\\}");
        std::smatch sm;
        if (!std::regex_search(c, sm, sec_re)) return;
        std::string body = sm[1].str();
        std::regex entry_re("\"([^\"]+)\"(?:\\s*>\\s*\"([^\"]+)\")?");
        auto it = std::sregex_iterator(body.begin(), body.end(), entry_re);
        for (auto end = std::sregex_iterator(); it != end; ++it) {
            std::string src = (*it)[1].str();
            std::string tgt = (*it)[2].matched ? (*it)[2].str() : "";
            out.push_back({src, tgt});
        }
    };
    parse_section("add", cfg.add_files);
    parse_section("recode", cfg.recode_files);
    return true;
}

static void glob_append(const fs::path& root, const std::string& pattern,
                        std::vector<fs::path>& out) {
    // No real wildcards: FileRift treats quoted paths as files or recursive dirs.
    fs::path p = pattern;
    if (!p.is_absolute()) p = root / p;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            if (e.is_regular_file(ec)) out.push_back(e.path());
        }
    } else if (fs::is_regular_file(p, ec)) {
        out.push_back(p);
    }
}

int apk_build(const std::string& project_file, const Options& opt) {
    std::string content;
    if (!read_bytes(project_file, content)) {
        std::fprintf(stderr, "build: cannot read project file %s\n", project_file.c_str());
        return 1;
    }
    ProjectConfig cfg;
    parse_project_file(content, cfg);

    if (cfg.base_apk.empty()) {
        std::fprintf(stderr, "build: no base apk specified\n");
        return 1;
    }
    if (cfg.out_apk.empty()) {
        std::fprintf(stderr, "build: no out apk specified\n");
        return 1;
    }

    fs::path wd = opt.working_dir;
    fs::path base_path = cfg.base_apk;
    if (!base_path.is_absolute()) base_path = wd / "projects" / "apks" / base_path;
    if (!fs::exists(base_path)) {
        std::fprintf(stderr, "build: base apk not found: %s\n", base_path.string().c_str());
        return 1;
    }
    fs::path out_path = cfg.out_apk;
    if (!out_path.is_absolute()) out_path = wd / "projects" / "apks" / out_path;

    // Gather add files (root = working_dir/source) and recode files (re_in).
    std::vector<std::pair<std::string, std::string>> add_src, recode_src;
    for (auto& [src, tgt] : cfg.add_files) {
        std::vector<fs::path> matches;
        glob_append(wd / "source", src, matches);
        for (auto& m : matches) {
            std::string target = tgt.empty() ? m.filename().string()
                                             : (fs::path(tgt) / m.filename()).string();
            if (!target.empty() && target[0] != '/') target = "assets/resources/" + target;
            if (!target.empty() && target[0] == '/') target = target.substr(1);
            add_src.push_back({m.string(), target});
        }
    }
    for (auto& [src, tgt] : cfg.recode_files) {
        std::vector<fs::path> matches;
        glob_append(wd / "re_in", src, matches);
        for (auto& m : matches) {
            std::string target = tgt.empty() ? m.filename().string()
                                             : (fs::path(tgt) / m.filename()).string();
            if (!target.empty() && target[0] != '/') target = "assets/resources/" + target;
            if (!target.empty() && target[0] == '/') target = target.substr(1);
            recode_src.push_back({m.string(), target});
        }
    }

    // Validate sources exist.
    for (auto& [s, t] : add_src)    if (!fs::exists(s)) { std::fprintf(stderr, "build: file not found: %s\n", s.c_str()); return 1; }
    for (auto& [s, t] : recode_src) if (!fs::exists(s)) { std::fprintf(stderr, "build: file not found: %s\n", s.c_str()); return 1; }

    // Read base entries.
    std::vector<zip::Entry> base_entries;
    if (!zip::read_entries(base_path.string(), base_entries)) {
        std::fprintf(stderr, "build: cannot read base apk %s\n", base_path.string().c_str());
        return 1;
    }

    std::set<std::string> replaced;
    for (auto& [s, t] : add_src)    replaced.insert(t);
    for (auto& [s, t] : recode_src) replaced.insert(t);

    // Build the output archive.
    std::vector<zip::OutEntry> out_entries;
    for (auto& e : base_entries) {
        if (replaced.count(e.name)) continue;
        std::string data;
        if (!zip::read_entry(base_path.string(), e, data)) continue;
        out_entries.push_back({e.name, data, 8});
    }
    for (auto& [s, t] : add_src) {
        std::string data;
        if (!read_bytes(s, data)) { std::fprintf(stderr, "build: cannot read %s\n", s.c_str()); return 1; }
        out_entries.push_back({t, data, 8});
        std::printf("%sadded:%s %s %s->%s %s\n", C_DATA, C_RESET, s.c_str(),
                    C_PUNCTUATION, C_RESET, t.c_str());
    }
    for (auto& [s, t] : recode_src) {
        std::string bytes, error;
        if (!recode_file(s, opt, bytes, error)) {
            std::fprintf(stderr, "build: recode failed %s (%s)\n", s.c_str(), error.c_str());
            return 1;
        }
        out_entries.push_back({t, bytes, 8});
        std::printf("%srecoded:%s %s %s->%s %s\n", C_DATA, C_RESET, s.c_str(),
                    C_PUNCTUATION, C_RESET, t.c_str());
    }

    std::error_code ec;
    if (fs::exists(out_path, ec)) fs::remove(out_path, ec);
    fs::create_directories(out_path.parent_path(), ec);
    if (!zip::write_archive(out_path.string(), out_entries)) {
        std::fprintf(stderr, "build: cannot write %s\n", out_path.string().c_str());
        return 1;
    }

    if (cfg.sign) {
        std::string apksigner = opt.apksigner_path;
        if (apksigner.empty()) {
            std::fprintf(stderr, "build: signing requested but no apksigner.jar (use --apksigner)\n");
            return 1;
        }
        if (apk_sign(out_path.string(), apksigner) != 0) return 1;
    }

    if (!cfg.command_prefix.empty()) {
        std::string cmd = cfg.command_prefix + " " + quote_shell(out_path.string());
        if (!cfg.command_suffix.empty()) cmd += " " + cfg.command_suffix;
        std::printf("%srunning:%s %s\n", C_WARNING, C_RESET, cmd.c_str());
        run_process(cmd);
    }

    std::printf("%s✔%s built %s\n", C_SUCCESS, C_RESET, out_path.string().c_str());
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// Playable scene creator (shared with Ruby GUI)
// ════════════════════════════════════════════════════════════════════════════

int run_scene_create_command(const std::vector<std::string>& args) {
    if (args.size() < 3 || args[1] != "create") {
        std::fprintf(stderr,
            "usage: ruby_cli scene create <output.scene|directory> [options]\n"
            "  --level NAME              map/runtime level name\n"
            "  --namespace NAME          object-library namespace\n"
            "  --mesh RESOURCE           optional starter POD resource\n"
            "  --background RESOURCE     optional background texture stem\n"
            "  --ground-top RESOURCE     platform top texture\n"
            "  --ground-side RESOURCE    platform side texture\n"
            "  --map PATH                link editor manifest to a .scmap\n"
            "  --width N --height N --depth N\n"
            "  --spawn-x N --spawn-y N --spawn-z N\n"
            "  --facing left|right\n");
        return 2;
    }

    scenecreate::Options options;
    options.output_path = args[2];
    fs::path output(options.output_path);
    options.level_name = output.has_extension()
        ? output.stem().string()
        : "new_level";
    options.scene_namespace = options.level_name;

    auto value_after = [&](size_t& i, const char* flag, std::string& value) -> bool {
        if (i + 1 >= args.size()) {
            std::fprintf(stderr, "scene create: %s requires a value\n", flag);
            return false;
        }
        value = args[++i];
        return true;
    };
    auto float_after = [&](size_t& i, const char* flag, float& value) -> bool {
        std::string text;
        if (!value_after(i, flag, text)) return false;
        try {
            size_t used = 0;
            value = std::stof(text, &used);
            if (used != text.size()) throw std::invalid_argument("trailing data");
        } catch (const std::exception&) {
            std::fprintf(stderr, "scene create: invalid number for %s: %s\n", flag, text.c_str());
            return false;
        }
        return true;
    };

    for (size_t i = 3; i < args.size(); ++i) {
        const std::string& flag = args[i];
        if (flag == "--level") {
            if (!value_after(i, "--level", options.level_name)) return 2;
        } else if (flag == "--namespace") {
            if (!value_after(i, "--namespace", options.scene_namespace)) return 2;
        } else if (flag == "--mesh") {
            if (!value_after(i, "--mesh", options.base_mesh)) return 2;
        } else if (flag == "--background") {
            if (!value_after(i, "--background", options.background)) return 2;
        } else if (flag == "--ground-top") {
            if (!value_after(i, "--ground-top", options.ground_top_texture)) return 2;
        } else if (flag == "--ground-side") {
            if (!value_after(i, "--ground-side", options.ground_side_texture)) return 2;
        } else if (flag == "--map") {
            if (!value_after(i, "--map", options.map_path)) return 2;
            options.link_to_map = true;
        } else if (flag == "--width") {
            if (!float_after(i, "--width", options.platform_width)) return 2;
        } else if (flag == "--height") {
            if (!float_after(i, "--height", options.platform_height)) return 2;
        } else if (flag == "--depth") {
            if (!float_after(i, "--depth", options.platform_depth)) return 2;
        } else if (flag == "--spawn-x") {
            if (!float_after(i, "--spawn-x", options.spawn_x)) return 2;
        } else if (flag == "--spawn-y") {
            if (!float_after(i, "--spawn-y", options.spawn_y)) return 2;
        } else if (flag == "--spawn-z") {
            if (!float_after(i, "--spawn-z", options.spawn_z)) return 2;
        } else if (flag == "--facing") {
            std::string facing;
            if (!value_after(i, "--facing", facing)) return 2;
            if (facing == "left" || facing == "-1") options.spawn_facing = -1;
            else if (facing == "right" || facing == "1") options.spawn_facing = 1;
            else {
                std::fprintf(stderr, "scene create: --facing must be left or right\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "scene create: unknown option '%s'\n", flag.c_str());
            return 2;
        }
    }

    scenecreate::Result result;
    std::string error;
    if (!scenecreate::create(options, result, error)) {
        std::fprintf(stderr, "scene create: %s\n", error.c_str());
        return 1;
    }
    std::printf("Created playable scene: %s\n", result.scene_path.c_str());
    std::printf("Ruby scene manifest: %s\n", result.manifest_path.c_str());
    std::printf("Objects: %d (world_base, spawn_default%s%s)\n",
                result.object_count,
                options.base_mesh.empty() ? "" : ", base_mesh",
                options.background.empty() ? "" : ", scene_background");
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// `ruby_cli map` — .scmap world-map (zones → nodes → portals) SDK surface.
// Exposes the map_loader API (byte-exact decode, validate, travel-path BFS,
// summary) so the whole overworld travel graph is scriptable, mirroring the
// scene subcommand. See docs/scmap_map_editor_feasibility.md.
// ════════════════════════════════════════════════════════════════════════════
static int run_map_command(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::fprintf(stderr,
            "usage: ruby_cli map <summary|decode|validate|path|list-nodes> "
            "<file.scmap> [args]\n");
        return 2;
    }
    const std::string& sub  = args[1];
    const std::string& file = args[2];

    mapedit::MapData m;
    std::string err;
    if (!mapedit::map_load(file, m, &err)) {
        std::fprintf(stderr, "map: %s\n", err.c_str());
        return 2;
    }

    if (sub == "summary") {
        size_t portals = 0;
        for (const auto& z : m.zones)
            for (const auto& n : z.nodes) portals += n.portals.size();
        std::printf("%zu zones, %zu nodes, %zu portals\n",
                    m.zones.size(), m.node_index.size(), portals);
        for (const auto& z : m.zones)
            std::printf("  zone %s%s%s  [%zu nodes]\n", z.name.c_str(),
                        z.title.empty() ? "" : " — ", z.title.c_str(),
                        z.nodes.size());
        return 0;
    }
    if (sub == "decode") {
        std::printf("%s\n", mapedit::map_to_markup(m).c_str());
        return 0;
    }
    if (sub == "validate") {
        mapedit::map_validate(m);
        if (m.issues.empty()) { std::printf("OK: no issues\n"); return 0; }
        for (const auto& is : m.issues) std::printf("ISSUE: %s\n", is.c_str());
        return 1;
    }
    if (sub == "path") {
        if (args.size() < 5) {
            std::fprintf(stderr, "usage: ruby_cli map path <file.scmap> <fromLevel> <toLevel>\n");
            return 2;
        }
        std::vector<std::string> p = mapedit::map_find_path(m, args[3], args[4]);
        if (p.empty()) { std::printf("no path\n"); return 1; }
        for (size_t i = 0; i < p.size(); ++i)
            std::printf("%s%s", i ? " -> " : "", p[i].c_str());
        std::printf("\n");
        return 0;
    }
    if (sub == "list-nodes") {
        for (const auto& z : m.zones)
            for (const auto& n : z.nodes)
                std::printf("%s\t%s\t%s\ttype=%d\n", z.name.c_str(),
                            n.level_name.c_str(), n.title.c_str(), n.type);
        return 0;
    }
    std::fprintf(stderr, "map: unknown subcommand '%s'\n", sub.c_str());
    return 2;
}

// ════════════════════════════════════════════════════════════════════════════
// Batch converter hook
// ════════════════════════════════════════════════════════════════════════════

int run_batch_command(const Options& opt) {
    if (opt.paths.size() < 2) {
        std::fprintf(stderr, "batch: usage: ruby_cli batch [--import] <src_dir> <dst_dir>\n");
        return 2;
    }
    batch::BatchState bs;
    std::strncpy(bs.src_dir, opt.paths[0].c_str(), sizeof(bs.src_dir) - 1);
    std::strncpy(bs.dst_dir, opt.paths[1].c_str(), sizeof(bs.dst_dir) - 1);
    bs.mode = opt.batch_export ? batch::Mode::EXPORT_TO_PNG
                               : batch::Mode::IMPORT_TO_GAME;
    bs.recurse_subdirs = opt.batch_recurse;
    bs.skip_existing   = opt.batch_skip_existing;
    bs.compress_fmt    = (batch::CompressFmt)opt.batch_compress;
    return batch::run_batch_headless(bs);
}

// ════════════════════════════════════════════════════════════════════════════
// Argument parsing (mirror FileRift setup.py get_args)
// ════════════════════════════════════════════════════════════════════════════

static void print_info(const Options& opt) {
    (void)opt;
    std::printf(
        "Ruby CLI  (native FileRift-compatible)\n"
        "--------------------------------------\n"
        "Supported file types:\n");
    for (auto& t : supported_types()) std::printf("    .%s\n", t.c_str());
    std::printf(
        "\nModes:\n"
        "    decode    binary -> markup   (default dir: de_in)\n"
        "    recode    markup -> binary   (default dir: re_in)\n"
        "    user      decode de_in/<user_folder>\n"
        "    both      recode then decode\n"
        "    force     recode, always rewrite\n"
        "    build     build APK from a .frproject\n"
        "\nAPK:\n"
        "    apk extract <apk> <dest>\n"
        "    apk sign <apk> [--apksigner path]\n"
        "    apk build <project> [--apksigner path]\n"
        "\nScene creator:\n"
        "    scene create <output.scene|directory> [--level NAME] [--namespace NAME]\n"
        "                 [--mesh RESOURCE] [--background RESOURCE] [--map FILE.scmap]\n"
        "                 [--width N --height N --depth N] [--facing left|right]\n"
        "\nWorld map (.scmap) SDK:\n"
        "    map summary    <file.scmap>                 zone/node/portal counts + per-zone list\n"
        "    map decode     <file.scmap>                 print FileRift markup (byte-exact)\n"
        "    map validate   <file.scmap>                 report orphan/broken/dup issues\n"
        "    map path       <file.scmap> <from> <to>     BFS travel path between two nodes\n"
        "    map list-nodes <file.scmap>                 tab-separated zone/level/title/type\n"
        "\nBatch texture converter:\n"
        "    batch [--import] <src_dir> <dst_dir>\n"
        "\nMCP (Model Context Protocol server for AI agents):\n"
        "    mcp [DIR]  run the MCP stdio server (JSON-RPC 2.0 over stdin/stdout)\n"
        "    mcp-http [PORT] [DIR]  run the MCP HTTP server for ChatGPT et al.\n"
        "                        (default http://127.0.0.1:8765/mcp, No Auth)\n"
        "\nOptions:\n"
        "    -o, --output PATH      output file or directory\n"
        "    -t, --file-type TYPE   force file type (stdin)\n"
        "    -w, --working-dir DIR  working directory (default .)\n"
        "    -n, --no-colour        disable ANSI colours\n"
        "    --apksigner PATH       path to Android apksigner.jar\n");
}

static bool has_prefix(const char* arg, const char* pre) {
    return std::strncmp(arg, pre, std::strlen(pre)) == 0;
}

int run_cli(int argc, char** argv) {
    Options opt;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    // Subcommand dispatch: scene / apk / batch / mcp.
    if (!args.empty()) {
        if (args[0] == "scene") {
            return run_scene_create_command(args);
        }
        if (args[0] == "map") {
            return run_map_command(args);
        }
        if (args[0] == "mcp") {
            std::string root = (args.size() > 1 && args[1][0] != '-') ? args[1] : "";
            return mcp::RunStdioServer(root);
        }
        if (args[0] == "mcp-http") {
            // ruby_cli mcp-http [port] [dir]   — URL-based transport for ChatGPT
            int port = 8765;
            std::string root;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i][0] == '-') continue;
                bool numeric = !args[i].empty() &&
                    args[i].find_first_not_of("0123456789") == std::string::npos;
                if (numeric) port = atoi(args[i].c_str());
                else if (root.empty()) root = args[i];
            }
            return mcp::RunHttpServer(port, root);
        }
        if (args[0] == "apk" || args[0] == "apks") {
            if (args.size() < 2) { print_info(opt); return 1; }
            std::string op = args[1];
            if (op == "extract") {
                if (args.size() < 4) {
                    std::fprintf(stderr, "usage: ruby_cli apk extract <apk> <dest_dir>\n");
                    return 2;
                }
                return apk_extract(args[2], args[3]);
            }
            if (op == "sign") {
                if (args.size() < 3) {
                    std::fprintf(stderr, "usage: ruby_cli apk sign <apk> [--apksigner path]\n");
                    return 2;
                }
                opt.apksigner_path = "";
                for (size_t i = 3; i < args.size(); ++i) {
                    if (args[i] == "--apksigner" && i + 1 < args.size())
                        opt.apksigner_path = args[++i];
                }
                return apk_sign(args[2], opt.apksigner_path);
            }
            if (op == "build") {
                if (args.size() < 3) {
                    std::fprintf(stderr, "usage: ruby_cli apk build <project.frproject> [--apksigner path]\n");
                    return 2;
                }
                opt.apksigner_path = "";
                for (size_t i = 3; i < args.size(); ++i) {
                    if (args[i] == "--apksigner" && i + 1 < args.size())
                        opt.apksigner_path = args[++i];
                }
                return apk_build(args[2], opt);
            }
            std::fprintf(stderr, "apk: unknown operation '%s' (extract|sign|build)\n", op.c_str());
            return 2;
        }
        if (args[0] == "batch") {
            args.erase(args.begin());
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--import") opt.batch_export = false;
                else if (args[i] == "--no-recurse") opt.batch_recurse = false;
                else if (args[i] == "--skip-existing") opt.batch_skip_existing = true;
                else if (args[i] == "--export") opt.batch_export = true;
                else opt.paths.push_back(args[i]);
            }
            opt.batch = true;
            return run_batch_command(opt);
        }
    }

    // General FileRift-style flag parsing.
    bool explicit_mode = false;
    std::vector<std::string> decode_paths, recode_paths;
    bool force = false, both = false, user = false, info = false, stdin_rec = false, stdin_dec = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-r" || a == "--recode") {
            opt.mode = Mode::RECODE; explicit_mode = true;
            // consume following non-flag args as paths
            while (i + 1 < args.size() && args[i + 1][0] != '-') recode_paths.push_back(args[++i]);
        } else if (a == "-d" || a == "--decode") {
            opt.mode = Mode::DECODE; explicit_mode = true;
            while (i + 1 < args.size() && args[i + 1][0] != '-') decode_paths.push_back(args[++i]);
        } else if (a == "-u" || a == "--user") {
            user = true;
        } else if (a == "--both") {
            both = true;
        } else if (a == "-f" || a == "--force") {
            force = true;
            while (i + 1 < args.size() && args[i + 1][0] != '-') recode_paths.push_back(args[++i]);
        } else if (a == "-b" || a == "--build") {
            opt.mode = Mode::BUILD; explicit_mode = true;
            if (i + 1 < args.size() && args[i + 1][0] != '-') opt.project_name = args[++i];
        } else if (a == "-i" || a == "--info") {
            info = true;
        } else if (a == "-n" || a == "--no-colour") {
            opt.no_colour = true;
        } else if (a == "--snake") {
            opt.style_snake_case = true;
        } else if (a == "--lua-check") {
            opt.lua_checking = true;
        } else if (a == "--no-lua-check") {
            opt.lua_checking = false;
        } else if (a == "--compile") {
            if (i + 1 < args.size()) opt.compile_mode = args[++i];
        } else if (a == "-o" || a == "--output") {
            if (i + 1 < args.size()) opt.output = args[++i];
        } else if (a == "-t" || a == "--file-type") {
            if (i + 1 < args.size()) opt.file_type = args[++i];
        } else if (a == "-w" || a == "--working-dir") {
            if (i + 1 < args.size()) opt.working_dir = args[++i];
        } else if (a == "--recode-stdin") {
            stdin_rec = true;
        } else if (a == "--decode-stdin") {
            stdin_dec = true;
        } else if (a == "--apksigner") {
            if (i + 1 < args.size()) opt.apksigner_path = args[++i];
        } else if (a == "--batch") {
            opt.batch = true; opt.batch_export = true;
        } else if (a == "--import") {
            opt.batch = true; opt.batch_export = false;
        } else if (a == "--help" || a == "-h") {
            print_info(opt); return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        } else {
            opt.paths.push_back(a);
        }
    }

    init_colours(opt);

    if (info) { print_info(opt); return 0; }

    // Stdin modes.
    if (stdin_rec) {
        std::stringstream buf;
        buf << std::cin.rdbuf();
        std::string markup = buf.str();
        std::string filetype = opt.file_type.empty() ? "fr" : opt.file_type;
        if (!supported_types().count(filetype)) {
            std::fprintf(stderr, "unknown file type: %s\n", filetype.c_str());
            return 1;
        }
        try {
            std::string bytes = filerift::recode_markup(markup, filetype);
            std::fwrite(bytes.data(), 1, bytes.size(), stdout);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "recode error: %s\n", e.what());
            return 1;
        }
        return 0;
    }
    if (stdin_dec) {
        std::stringstream buf;
        buf << std::cin.rdbuf();
        std::string bytes = buf.str();
        std::string filetype = opt.file_type.empty() ? "fr" : opt.file_type;
        if (!supported_types().count(filetype)) {
            std::fprintf(stderr, "unknown file type: %s\n", filetype.c_str());
            return 1;
        }
        try {
            std::string markup = filerift::decode_protobuf(bytes, filetype);
            std::fwrite(markup.data(), 1, markup.size(), stdout);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "decode error: %s\n", e.what());
            return 1;
        }
        return 0;
    }

    // Resolve effective mode (mirror FileRift: force/both/user override).
    if (force) { opt.mode = Mode::FORCE; explicit_mode = true; }
    else if (both) { opt.mode = Mode::BOTH; explicit_mode = true; }
    else if (user) { opt.mode = Mode::USER; explicit_mode = true; }

    if (opt.mode == Mode::BUILD) {
        fs::path wd = opt.working_dir;
        fs::path project = opt.project_name;
        if (project.extension().empty()) project += ".frproject";
        if (!project.is_absolute()) project = wd / "projects" / project;
        return apk_build(project.string(), opt);
    }

    // Batch mode via flags.
    if (opt.batch) return run_batch_command(opt);

    if (!explicit_mode) {
        std::printf("Nothing to do. Use -d/--decode, -r/--recode, --both, or --help.\n");
        return 0;
    }

    // Build file lists: positional paths or default dirs.
    fs::path wd = opt.working_dir;
    std::vector<std::string> decode_list = decode_paths;
    std::vector<std::string> recode_list = recode_paths;
    if (!opt.paths.empty()) {
        if (opt.mode == Mode::RECODE || opt.mode == Mode::FORCE)
            recode_list = opt.paths;
        else
            decode_list = opt.paths;
    }

    int decoded = 0, recoded = 0, skipped = 0;

    auto run_decode = [&](const std::string& in_dir) {
        fs::path out_dir = opt.output.empty()
            ? (wd / "de_out")
            : fs::path(opt.output);
        fs::path in = fs::path(in_dir);
        if (!in.is_absolute()) in = wd / in;
        if (!fs::exists(in)) { std::fprintf(stderr, "decode: dir not found: %s\n", in.string().c_str()); return; }
        int n = decode_directory(in.string(), out_dir.string(), opt);
        decoded += n;
    };
    auto run_recode = [&](const std::string& in_dir) {
        fs::path out_dir = opt.output.empty()
            ? (wd / "re_out")
            : fs::path(opt.output);
        fs::path in = fs::path(in_dir);
        if (!in.is_absolute()) in = wd / in;
        if (!fs::exists(in)) { std::fprintf(stderr, "recode: dir not found: %s\n", in.string().c_str()); return; }
        int n = recode_directory(in.string(), out_dir.string(), opt);
        recoded += n;
    };

    switch (opt.mode) {
        case Mode::BOTH:
            if (recode_list.empty()) recode_list.push_back("re_in");
            for (auto& p : recode_list) run_recode(p);
            if (decode_list.empty()) decode_list.push_back("de_in");
            for (auto& p : decode_list) run_decode(p);
            break;
        case Mode::USER:
            run_decode((wd / "de_in" / opt.user_folder).string());
            break;
        case Mode::RECODE:
        case Mode::FORCE:
            if (recode_list.empty()) recode_list.push_back("re_in");
            for (auto& p : recode_list) run_recode(p);
            break;
        case Mode::DECODE:
        default:
            if (decode_list.empty()) decode_list.push_back("de_in");
            for (auto& p : decode_list) run_decode(p);
            break;
    }

    (void)skipped;
    std::string result;
    if (decoded > 0) result += std::string(C_SUCCESS) + "decoded " + C_RESET + std::to_string(decoded) + "  ";
    if (recoded > 0) result += std::string(C_SUCCESS) + "recoded " + C_RESET + std::to_string(recoded) + "  ";
    if (result.empty()) result = "nothing to do";
    std::printf("\n%s\nFile Rift v5.8.6 (native ruby_cli)\n", result.c_str());
    return 0;
}

} // namespace rubycli

std::string g_instance_assets_dir = "assets";

int main(int argc, char** argv) {
    return rubycli::run_cli(argc, argv);
}
