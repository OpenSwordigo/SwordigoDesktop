/* ruby_mcp.cpp — Model Context Protocol (MCP) server for the Ruby SDK
 *
 * 100% native C++17, zero new dependencies. Contains:
 *   §1  Minimal JSON engine (parser + compact/pretty writers)
 *   §2  Asset helpers (read, type detect, gzip, PNG/PVR/TEX headers, POD walk)
 *   §3  Tool implementations over existing modules:
 *         filerift  → scl_decode, scene_decode, search_scl
 *         av (scene_loader) → scene_objects, scene_summary, scene_programs,
 *                             scene_templates, scene_libraries
 *         av (pod_loader)   → pod_info, pod_blocks
 *         pvr_loader        → texture_info
 *   §4  MCP/JSON-RPC 2.0 session state machine (initialize, tools, resources,
 *       prompts, ping, notifications)
 *   §5  stdio server entry (RunStdioServer) + HandleLine for embedding
 *
 * Transport: MCP stdio — one JSON message per line on stdout, requests on
 * stdin. Compatible with Claude Desktop / Cline / Continue `command` configs.
 */

#include "tools/ruby_mcp.h"

#include "tools/filerift.h"
#include "tools/scene_loader.h"   // av:: scene graph (pulls entity/physics/collision/pod)
#include "platform/pvr_loader.h"  // pvr_decode_to_rgba (texture_info decode)
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <thread>
#include <unistd.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <cerrno>
#endif
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <functional>

namespace fs = std::filesystem;

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════════
// §1  Minimal JSON engine
// ═══════════════════════════════════════════════════════════════════════════

struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type t = NUL;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;   // ordered

    static Json Null()  { return Json{}; }
    static Json Bool(bool v)  { Json j; j.t = BOOL; j.b = v; return j; }
    static Json Num(double v) { Json j; j.t = NUM;  j.n = v; return j; }
    static Json Str(const std::string& v) { Json j; j.t = STR; j.s = v; return j; }
    static Json Arr() { Json j; j.t = ARR; return j; }
    static Json Obj() { Json j; j.t = OBJ; return j; }

    Json& set(const std::string& k, Json v) {
        for (auto& kv : obj) if (kv.first == k) { kv.second = std::move(v); return *this; }
        obj.emplace_back(k, std::move(v));
        return *this;
    }
    Json& set(const std::string& k, const std::string& v) { return set(k, Str(v)); }
    Json& set(const std::string& k, const char* v)        { return set(k, Str(v ? v : "")); }
    Json& set(const std::string& k, double v)             { return set(k, Num(v)); }
    Json& set(const std::string& k, int v)                { return set(k, Num((double)v)); }
    Json& set(const std::string& k, bool v)               { return set(k, Bool(v)); }
    void push(Json v) { arr.push_back(std::move(v)); }

    const Json* get(const std::string& k) const {
        if (t != OBJ) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool has(const std::string& k) const { return get(k) != nullptr; }
    std::string str(const std::string& k, const std::string& dflt = "") const {
        const Json* v = get(k);
        return (v && v->t == STR) ? v->s : dflt;
    }
    double num(const std::string& k, double dflt = 0.0) const {
        const Json* v = get(k);
        return (v && v->t == NUM) ? v->n : dflt;
    }
    bool boolean(const std::string& k, bool dflt = false) const {
        const Json* v = get(k);
        return (v && v->t == BOOL) ? v->b : dflt;
    }
    int as_int(double dflt = 0) const { return (t == NUM) ? (int)n : (int)dflt; }
    std::string as_str() const {
        if (t == STR) return s;
        if (t == NUL) return "null";
        return dump();
    }
    bool is_str() const { return t == STR; }
    bool is_null() const { return t == NUL; }

    // ── Writers ──
    // Escape a raw byte string into a valid UTF-8 JSON string literal:
    //   - ASCII printable chars pass through
    //   - control chars (< 0x20) become \u00XX
    //   - valid multi-byte UTF-8 sequences pass through untouched
    //   - invalid/bare high bytes become U+FFFD (keeps every message
    //     strictly valid UTF-8 — required by the MCP stdio transport)
    static std::string esc(const std::string& in) {
        std::string o;
        o.reserve(in.size() + 8);
        static const char* HEX = "0123456789abcdef";
        size_t i = 0;
        while (i < in.size()) {
            unsigned char c = (unsigned char)in[i];
            int len = 1;
            uint32_t cp = c;
            if (c >= 0xC2 && c <= 0xDF)      { len = 2; cp = c & 0x1F; }
            else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
            else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
            bool valid = len > 1 && i + (size_t)len <= in.size();
            if (valid) {
                for (int k = 1; k < len; ++k) {
                    unsigned char cc = (unsigned char)in[i + k];
                    if ((cc & 0xC0) != 0x80) { valid = false; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if (valid) {
                    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                        (len == 4 && cp < 0x10000)) valid = false;   // overlong
                    if (cp >= 0xD800 && cp <= 0xDFFF) valid = false; // surrogate
                    if (cp > 0x10FFFF) valid = false;
                }
            }
            if (valid) {
                for (int k = 0; k < len; ++k) o += in[i + k];
                i += (size_t)len;
                continue;
            }
            if (c == '"') o += "\\\"";
            else if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else if (c == '\b') o += "\\b";
            else if (c == '\f') o += "\\f";
            else if (c < 0x20) {
                o += "\\u00";
                o += HEX[c >> 4];
                o += HEX[c & 15];
            } else if (c < 0x80) {
                o += (char)c;            // printable ASCII passes through
            } else {
                o += "\xEF\xBF\xBD";   // invalid high byte → U+FFFD
            }
            ++i;
        }
        return o;
    }

    std::string dump() const {   // compact, single line (required by MCP stdio)
        switch (t) {
            case NUL:  return "null";
            case BOOL: return b ? "true" : "false";
            case NUM: {
                char buf[40];
                if (n == (double)(long long)n && std::fabs(n) < 1e15)
                    snprintf(buf, sizeof buf, "%lld", (long long)n);
                else
                    snprintf(buf, sizeof buf, "%.9g", n);
                return buf;
            }
            case STR:  return "\"" + esc(s) + "\"";
            case ARR: {
                std::string o = "[";
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (i) o += ",";
                    o += arr[i].dump();
                }
                return o + "]";
            }
            case OBJ: {
                std::string o = "{";
                for (size_t i = 0; i < obj.size(); ++i) {
                    if (i) o += ",";
                    o += "\"" + esc(obj[i].first) + "\":" + obj[i].second.dump();
                }
                return o + "}";
            }
        }
        return "null";
    }

    std::string pretty(int indent = 0) const {   // human-readable, for consoles
        const std::string pad((size_t)indent, ' ');
        switch (t) {
            case NUL: return "null";
            case BOOL: return b ? "true" : "false";
            case NUM: { Json c = *this; return c.dump(); }
            case STR: return "\"" + esc(s) + "\"";
            case ARR: {
                if (arr.empty()) return "[]";
                std::string o = "[\n";
                for (size_t i = 0; i < arr.size(); ++i) {
                    o += pad + "  " + arr[i].pretty(indent + 2);
                    if (i + 1 < arr.size()) o += ",";
                    o += "\n";
                }
                return o + pad + "]";
            }
            case OBJ: {
                if (obj.empty()) return "{}";
                std::string o = "{\n";
                for (size_t i = 0; i < obj.size(); ++i) {
                    o += pad + "  \"" + esc(obj[i].first) + "\": " +
                         obj[i].second.pretty(indent + 2);
                    if (i + 1 < obj.size()) o += ",";
                    o += "\n";
                }
                return o + pad + "}";
            }
        }
        return "null";
    }

    // ── Parser ──
    struct Parser {
        const std::string& in;
        size_t p = 0;
        int depth = 0;
        static constexpr int kMaxDepth = 256;
        bool ok = true;
        std::string err;
        explicit Parser(const std::string& s) : in(s) {}

        void ws() { while (p < in.size() && (in[p] == ' ' || in[p] == '\t' || in[p] == '\n' || in[p] == '\r')) ++p; }
        bool fail(const std::string& m) { ok = false; err = m; return false; }

        bool parse(Json& out) { ws(); bool r = value(out); ws(); if (r && p != in.size()) return fail("trailing characters"); return r; }

        bool value(Json& out) {
            ws();
            if (p >= in.size()) return fail("unexpected end");
            if (depth > kMaxDepth) return fail("nesting too deep");
            char c = in[p];
            if (c == '{') { ++depth; bool ok2 = object(out); --depth; return ok2; }
            if (c == '[') { ++depth; bool ok2 = array(out); --depth; return ok2; }
            if (c == '"') { std::string s; if (!str(s)) return false; out = Json::Str(s); return true; }
            if (c == 't') { if (in.compare(p, 4, "true") == 0) { p += 4; out = Json::Bool(true); return true; } return fail("invalid literal"); }
            if (c == 'f') { if (in.compare(p, 5, "false") == 0) { p += 5; out = Json::Bool(false); return true; } return fail("invalid literal"); }
            if (c == 'n') { if (in.compare(p, 4, "null") == 0) { p += 4; out = Json::Null(); return true; } return fail("invalid literal"); }
            return number(out);
        }

        bool str(std::string& out) {
            if (p >= in.size() || in[p] != '"') return fail("expected string");
            ++p;
            out.clear();
            while (p < in.size()) {
                unsigned char c = (unsigned char)in[p];
                if (c == '"') { ++p; return true; }
                if (c == '\\') {
                    ++p;
                    if (p >= in.size()) return fail("bad escape");
                    char e = in[p++];
                    switch (e) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u': {
                            if (p + 4 > in.size()) return fail("bad \\u");
                            unsigned cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = in[p + i];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                                else return fail("bad \\u hex");
                            }
                            p += 4;
                            // encode cp as UTF-8 (surrogates collapsed to U+FFFD)
                            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= in.size() && in[p] == '\\' && in[p+1] == 'u') {
                                unsigned lo = 0;
                                for (int i = 0; i < 4; ++i) {
                                    char h = in[p + 2 + i]; lo <<= 4;
                                    if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                    else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                                    else { lo = 0xFFFF; break; }
                                }
                                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                                else cp = 0xFFFD;
                            } else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else if (cp < 0x10000) {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xF0 | (cp >> 18));
                                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default: return fail("bad escape char");
                    }
                } else {
                    out += (char)c;
                    ++p;
                }
            }
            return fail("unterminated string");
        }

        bool number(Json& out) {
            size_t start = p;
            if (p < in.size() && (in[p] == '-' || in[p] == '+')) ++p;
            bool any = false;
            while (p < in.size() && in[p] >= '0' && in[p] <= '9') { ++p; any = true; }
            if (p < in.size() && in[p] == '.') {
                ++p;
                while (p < in.size() && in[p] >= '0' && in[p] <= '9') { ++p; any = true; }
            }
            if (p < in.size() && (in[p] == 'e' || in[p] == 'E')) {
                ++p;
                if (p < in.size() && (in[p] == '+' || in[p] == '-')) ++p;
                while (p < in.size() && in[p] >= '0' && in[p] <= '9') ++p;
            }
            if (!any) return fail("invalid number");
            std::string lit = in.substr(start, p - start);
            char* endp = nullptr;
            double v = strtod(lit.c_str(), &endp);
            if (endp == lit.c_str()) return fail("invalid number");
            out = Json::Num(v);
            return true;
        }

        bool object(Json& out) {
            ++p; // {
            out = Json::Obj();
            ws();
            if (p < in.size() && in[p] == '}') { ++p; return true; }
            while (true) {
                ws();
                std::string key;
                if (!str(key)) return fail("object key");
                ws();
                if (p >= in.size() || in[p] != ':') return fail("expected ':'");
                ++p;
                Json val;
                if (!value(val)) return false;
                out.set(key, std::move(val));
                ws();
                if (p >= in.size()) return fail("unterminated object");
                if (in[p] == ',') { ++p; continue; }
                if (in[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }

        bool array(Json& out) {
            ++p; // [
            out = Json::Arr();
            ws();
            if (p < in.size() && in[p] == ']') { ++p; return true; }
            while (true) {
                Json val;
                if (!value(val)) return false;
                out.push(std::move(val));
                ws();
                if (p >= in.size()) return fail("unterminated array");
                if (in[p] == ',') { ++p; continue; }
                if (in[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
    };

    static bool parse(const std::string& text, Json& out, std::string& err) {
        Parser ps(text);
        if (!ps.parse(out)) { err = ps.err; return false; }
        return true;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// §2  Asset helpers
// ═══════════════════════════════════════════════════════════════════════════

static std::string to_lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool ends_with_ci(const std::string& s, const std::string& suf) {
    std::string a = to_lower(s), b = to_lower(suf);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}
static bool read_bytes(const std::string& path, std::string& out, size_t cap = 0) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (cap && out.size() > cap) out.resize(cap);
    return true;
}
static std::string human_size(uintmax_t sz) {
    char buf[64];
    if (sz < 1024) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)sz);
    else if (sz < 1024ULL*1024) snprintf(buf, sizeof buf, "%.1f KB", (double)sz/1024.0);
    else snprintf(buf, sizeof buf, "%.2f MB", (double)sz/1048576.0);
    return buf;
}
static std::string file_size_str(const fs::path& p) {
    std::error_code ec;
    return human_size(fs::file_size(p, ec));
}

// gzip container (0x1f 0x8b) → inflated bytes
static bool gzip_inflate(const std::string& in, std::string& out) {
    if (in.size() < 2 || (unsigned char)in[0] != 0x1f || (unsigned char)in[1] != 0x8b) return false;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    char buf[65536];
    int ret;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof buf;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) { inflateEnd(&zs); return false; }
        out.append(buf, sizeof buf - zs.avail_out);
    } while (ret != Z_STREAM_END && zs.avail_in > 0);
    inflateEnd(&zs);
    return ret == Z_STREAM_END;
}

// File-type detection (magic + extension)
static std::string detect_type(const std::string& path, const std::string* bytes = nullptr) {
    std::string l = to_lower(path);
    if (ends_with_ci(path, ".scene")) return "scene";
    if (ends_with_ci(path, ".scl"))   return "scl";
    if (ends_with_ci(path, ".pod"))   return "pod";
    if (ends_with_ci(path, ".pvr"))   return "pvr";
    if (ends_with_ci(path, ".tex"))   return "tex";
    if (ends_with_ci(path, ".png"))   return "png";
    if (ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg")) return "jpeg";
    if (ends_with_ci(path, ".wav") || ends_with_ci(path, ".ogg") || ends_with_ci(path, ".mp3")) return "audio";
    if (ends_with_ci(path, ".lua") || ends_with_ci(path, ".txt") || ends_with_ci(path, ".json") ||
        ends_with_ci(path, ".md")  || ends_with_ci(path, ".frproject")) return "text";
    if (bytes) {
        if (bytes->size() >= 4 && (unsigned char)(*bytes)[0] == 0x89 && (unsigned char)(*bytes)[1] == 'P') return "png";
        if (bytes->size() >= 4 && (unsigned char)(*bytes)[0] == 0x1f && (unsigned char)(*bytes)[1] == 0x8b) return "gzip";
    }
    return "unknown";
}

// Decode helpers (never throw)
static float printable_ratio(const std::string& s) {
    if (s.empty()) return 0.0f;
    size_t printable = 0;
    for (unsigned char c : s)
        if (c == '\n' || c == '\t' || c == '\r' || c >= 0x20) ++printable;
    return (float)printable / (float)s.size();
}
static std::string try_decode_scl(const std::string& bytes) {
    std::string proto, script, lua;
    try { proto = filerift::decode_protobuf(bytes, "scl"); } catch (...) {}
    if (proto.empty()) try { proto = filerift::decode_protobuf(bytes, "script"); } catch (...) {}
    try { lua = filerift::extract_lua_generic(bytes); } catch (...) {}
    // Prefer the most readable representation: FileRift markup if it is
    // actually text, otherwise the generic Lua/string extractor.
    script = proto;
    if (printable_ratio(lua) > printable_ratio(script) + 0.25f) script = lua;
    return script;
}
static std::string try_decode_scene(const std::string& bytes) {
    std::string text;
    try { text = filerift::decode_protobuf(bytes, "scene"); } catch (...) { text.clear(); }
    return text;
}
static std::string try_decode_any(const std::string& path, const std::string& bytes) {
    std::string t = detect_type(path, &bytes);
    if (t == "scene") return try_decode_scene(bytes);
    if (t == "scl")   return try_decode_scl(bytes);
    return "";
}

// RAII guard: while a tool runs, C-level stdout writes (scene_loader debug
// logs, filerift notices, …) are redirected to stderr so the MCP stdio
// protocol channel stays 100% clean JSON-RPC.
struct StdoutGuard {
    int saved = -1;
    StdoutGuard() {
        saved = dup(STDOUT_FILENO);
        if (saved >= 0) dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    ~StdoutGuard() {
        if (saved >= 0) {
            // Library debug writes are typically buffered in userspace; they
            // must be flushed while fd 1 STILL points at stderr, otherwise
            // they would land on the protocol channel after fd 1 is restored.
            fflush(stdout);
            std::cout.flush();
            dup2(saved, STDOUT_FILENO);
            close(saved);
        }
    }
};

// Convert binary bytes to a printable string (for searching non-decodable files)
static std::string printable(const std::string& in, size_t cap = 1 << 22) {
    std::string o;
    o.reserve(std::min(in.size(), cap));
    for (unsigned char c : in) {
        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) o += (char)c;
        else o += ' ';
        if (o.size() >= cap) break;
    }
    return o;
}

static size_t count_lines(const std::string& s) {
    if (s.empty()) return 0;
    size_t n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

static size_t count_matches(const std::string& hay, const std::string& q, bool ci) {
    if (q.empty()) return 0;
    std::string h = hay, qq = q;
    if (ci) { h = to_lower(hay); qq = to_lower(q); }
    size_t c = 0, pos = 0;
    while ((pos = h.find(qq, pos)) != std::string::npos) { ++c; pos += qq.size(); }
    return c;
}
static std::vector<std::string> find_snippets(const std::string& hay, const std::string& q,
                                              bool ci, int max_snippets, size_t ctx) {
    std::vector<std::string> out;
    if (q.empty()) return out;
    std::string h = hay, qq = q;
    if (ci) { h = to_lower(hay); qq = to_lower(q); }
    size_t pos = 0;
    while ((size_t)out.size() < (size_t)max_snippets && (pos = h.find(qq, pos)) != std::string::npos) {
        size_t start = pos > ctx ? pos - ctx : 0;
        size_t len = std::min(h.size() - start, qq.size() + ctx * 2);
        std::string sn = hay.substr(start, len);
        // single-line collapse
        std::string line;
        for (char c : sn) {
            if (c == '\n') line += "\\n";
            else line += c;
        }
        out.push_back(line);
        pos += qq.size();
    }
    return out;
}

// Aligned-safe little-endian reads (memcpy — this project targets ARM too).
static uint32_t rd_u32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t rd_u64(const char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
static bool g_is_pvr(const std::string& b) {
    if (b.size() >= 52) {
        uint32_t v3 = rd_u32(b.data());
        uint32_t v2 = rd_u32(b.data() + 44);
        if (v3 == 0x03525650 || v2 == 0x21525650) return true;
    }
    return false;
}
// Return {container, w, h, extra} for pvr/tex/png
static Json texture_header(const std::string& path, const std::string& raw) {
    Json out = Json::Obj();
    std::string b = raw;
    bool gz = b.size() >= 2 && (unsigned char)b[0] == 0x1f && (unsigned char)b[1] == 0x8b;
    if (gz) {
        std::string dec;
        if (gzip_inflate(raw, dec)) b = dec;
    }
    out.set("gzip", gz);
    if (g_is_pvr(b)) {
        uint32_t magic = rd_u32(b.data());
        if (magic == 0x03525650) {
            uint32_t h = rd_u32(b.data() + 20);
            uint32_t w = rd_u32(b.data() + 24);
            uint64_t pf = rd_u64(b.data() + 8);
            out.set("container", "pvr");
            out.set("pvr_version", 3);
            out.set("width", (int)w);
            out.set("height", (int)h);
            char fmt[24];
            if (pf == 6ULL) snprintf(fmt, sizeof fmt, "ETC1");
            else snprintf(fmt, sizeof fmt, "0x%016llx", (unsigned long long)pf);
            out.set("pixel_format", fmt);
        } else {
            uint32_t h = rd_u32(b.data() + 4);
            uint32_t w = rd_u32(b.data() + 8);
            out.set("container", "pvr");
            out.set("pvr_version", 2);
            out.set("width", (int)w);
            out.set("height", (int)h);
        }
    } else if (b.size() >= 12) {
        uint32_t type = rd_u32(b.data());
        uint32_t w = rd_u32(b.data() + 4);
        uint32_t h = rd_u32(b.data() + 8);
        if (type >= 1 && type <= 8) {
            out.set("container", "tex");
            out.set("img_type", (int)type);
            out.set("width", (int)w);
            out.set("height", (int)h);
        }
    }
    if (!out.has("width") && b.size() >= 24 && (unsigned char)b[0] == 0x89 && (unsigned char)b[1] == 'P') {
        uint32_t w = ((uint32_t)(unsigned char)b[16] << 24) | ((uint32_t)(unsigned char)b[17] << 16) |
                     ((uint32_t)(unsigned char)b[18] << 8) | (uint32_t)(unsigned char)b[19];
        uint32_t h = ((uint32_t)(unsigned char)b[20] << 24) | ((uint32_t)(unsigned char)b[21] << 16) |
                     ((uint32_t)(unsigned char)b[22] << 8) | (uint32_t)(unsigned char)b[23];
        out.set("container", "png");
        out.set("width", (int)w);
        out.set("height", (int)h);
    }
    if (!out.has("container")) out.set("container", "unknown");
    return out;
}

// POD chunk walk — returns {id:name} histogram.
// Swordigo uses PowerVR's numeric binary POD: the file starts directly with
// tag/len pairs (no header); tags are the SDK block ids (1000..9003) and the
// high bit (0x80000000) marks a block's end tag.
static const std::map<int, const char*>& pod_block_names() {
    static const std::map<int, const char*> names = {
        {1000,"FormatVersion"},{1001,"Scene"},
        {2004,"SceneNumMeshes"},{2005,"SceneNumNodes"},{2006,"SceneNumMeshNodes"},
        {2007,"SceneNumTextures"},{2008,"SceneNumMaterials"},{2009,"SceneNumFrames"},
        {2017,"SceneFPS"},{2012,"SceneMesh"},{2013,"SceneNode"},{2014,"SceneTexture"},{2015,"SceneMaterial"},
        {3000,"MaterialName"},{3001,"MaterialDiffuseTextureIndex"},{3002,"MaterialOpacity"},{3004,"MaterialDiffuse"},
        {4000,"TextureFilename"},
        {5000,"NodeIndex"},{5001,"NodeName"},{5002,"NodeMaterialIndex"},{5003,"NodeParentIndex"},
        {5004,"NodePosition"},{5005,"NodeRotation"},{5006,"NodeScale"},
        {5007,"NodeAnimPosition"},{5008,"NodeAnimRotation"},{5009,"NodeAnimScale"},
        {5010,"NodeMatrix"},{5011,"NodeAnimMatrix"},{5012,"NodeAnimFlags"},
        {5013,"NodeAnimPositionIndex"},{5014,"NodeAnimRotationIndex"},{5015,"NodeAnimScaleIndex"},{5016,"NodeAnimMatrixIndex"},
        {6000,"MeshNumVertices"},{6001,"MeshNumFaces"},{6002,"MeshNumUVWChannels"},{6003,"MeshVertexIndexList"},
        {6004,"MeshStripLengthList"},{6005,"MeshNumStrips"},{6006,"MeshVertexList"},{6007,"MeshNormalList"},
        {6008,"MeshTangentList"},{6009,"MeshBinormalList"},{6010,"MeshUVWList"},{6011,"MeshVertexColourList"},
        {6012,"MeshBoneIndexList"},{6013,"MeshBoneWeightList"},{6014,"MeshInterleavedDataList"},
        {6015,"MeshBoneBatchIndexList"},{6016,"MeshNumBoneIndicesPerBatch"},{6017,"MeshBoneOffsetPerBatch"},
        {6018,"MeshMaxNumBonesPerBatch"},{6019,"MeshNumBoneBatches"},{6020,"MeshUnpackMatrix"},
        {6021,"MeshType"},{6022,"MeshAdjacencyIndexList"},
        {9000,"BlockDataType"},{9001,"BlockNumComponents"},{9002,"BlockStride"},{9003,"BlockData"},
    };
    return names;
}
static Json pod_block_histogram(const std::string& bytes) {
    Json blocks = Json::Arr();
    if (bytes.size() < 8) return blocks;
    size_t off = 0;
    std::map<int, int> hist;
    while (off + 8 <= bytes.size()) {
        uint32_t tag = rd_u32(bytes.data() + off);
        uint32_t len = rd_u32(bytes.data() + off + 4);
        if (off + 8 + (size_t)len > bytes.size()) break;
        int id = (int)(tag & 0x7FFFFFFFu);   // strip the end-tag mask bit
        hist[id]++;
        off += 8 + len;
    }
    for (const auto& kv : hist) {
        Json b = Json::Obj();
        b.set("id", kv.first);
        auto it = pod_block_names().find(kv.first);
        b.set("name", it != pod_block_names().end() ? it->second : "?");
        b.set("count", kv.second);
        blocks.push(std::move(b));
    }
    return blocks;
}

// ═══════════════════════════════════════════════════════════════════════════
// §3  Tool registry + implementations
// ═══════════════════════════════════════════════════════════════════════════

using ToolFn = Json (*)(const Json& args, std::string& err);
struct Tool {
    const char* name;
    const char* desc;
    Json schema;
    ToolFn fn;
};

static const size_t kMaxToolText = 8 << 20; // 8 MiB cap on decoded text

static Json tool_scl_decode(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::string bytes;
    if (!read_bytes(path, bytes)) { err = "cannot read file: " + path; return Json::Null(); }
    std::string text = try_decode_scl(bytes);
    bool truncated = false;
    if (text.size() > kMaxToolText) { text.resize(kMaxToolText); truncated = true; }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("bytes", (int)(long long)bytes.size());
    r.set("filetype", detect_type(path, &bytes));
    r.set("decoded_bytes", (int)(long long)text.size());
    r.set("truncated", truncated);
    r.set("text", text);
    return r;
}

static Json tool_scene_decode(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::string bytes;
    if (!read_bytes(path, bytes)) { err = "cannot read file: " + path; return Json::Null(); }
    std::string text = try_decode_scene(bytes);
    if (text.empty()) { err = "scene decode produced no text (not a .scene file?)"; return Json::Null(); }
    bool truncated = false;
    if (text.size() > kMaxToolText) { text.resize(kMaxToolText); truncated = true; }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("bytes", (int)(long long)bytes.size());
    r.set("truncated", truncated);
    r.set("text", text);
    return r;
}

static Json obj_json(const av::SceneObject& o) {
    Json j = Json::Obj();
    j.set("name", o.name);
    j.set("template_name", o.template_name);
    Json pos = Json::Arr();
    pos.push(Json::Num(o.pos_x)); pos.push(Json::Num(o.pos_y)); pos.push(Json::Num(o.pos_z));
    j.set("pos", std::move(pos));
    j.set("depth", o.pos_z);
    j.set("rotation_y", o.rot_y);
    j.set("scale", o.scale_x);
    j.set("hidden", o.hidden);
    if (!o.mesh_name.empty()) j.set("mesh", o.mesh_name);
    if (!o.texture_name.empty()) j.set("texture", o.texture_name);
    if (!o.background_name.empty()) j.set("background", o.background_name);
    if (o.is_spawn_point) {
        j.set("is_spawn_point", true);
        j.set("spawn_facing", o.spawn_facing);
    }
    if (o.is_portal) {
        j.set("is_portal", true);
        j.set("portal_destination", o.portal_destination);
    }
    av::EntityData ed = av::entity_parse(o);
    if (ed.is_entity) {
        Json e = Json::Obj();
        e.set("is_hero", ed.is_hero);
        e.set("is_monster", ed.is_monster);
        e.set("facing_direction", ed.entity.facing_direction);
        e.set("physics_enabled", ed.entity.physics_enabled);
        if (!ed.controller.entity_id.empty()) e.set("entity_id", ed.controller.entity_id);
        if (!ed.controller.animation_controller.empty()) e.set("animation_controller", ed.controller.animation_controller);
        if (ed.monster.gives_experience) e.set("gives_experience", true);
        e.set("death_anim", ed.monster.default_death_anim);
        j.set("entity", std::move(e));
    }
    av::AnimBindings ab = av::anim_bindings(o);
    if (ab.present) {
        Json an = Json::Obj();
        an.set("pod", ab.pod);
        an.set("repeating", ab.repeating);
        an.set("speed_multiplier", ab.speed_multiplier);
        if (ab.walk_speed != 0.0f) an.set("walk_speed", ab.walk_speed);
        if (ab.run_speed != 0.0f)  an.set("run_speed", ab.run_speed);
        if (ab.jump_speed != 0.0f) an.set("jump_speed", ab.jump_speed);
        j.set("animation", std::move(an));
    }
    av::PhysicsData pd = av::physics_parse(o);
    if (pd.enabled || pd.is_platform) {
        Json p = Json::Obj();
        p.set("enabled", pd.enabled);
        p.set("gravity", pd.gravity);
        p.set("elasticity", pd.elasticity);
        p.set("is_platform", pd.is_platform);
        if (pd.is_platform) { p.set("mass", pd.platform_mass); p.set("spring", pd.platform_spring); }
        j.set("physics", std::move(p));
    }
    av::CollisionData cd = av::collision_parse(o);
    if (!cd.shapes.empty() || !cd.ground_polygons.empty()) {
        Json c = Json::Obj();
        c.set("shapes", (int)cd.shapes.size());
        c.set("ground_polygons", (int)cd.ground_polygons.size());
        for (const auto& s : cd.shapes) {
            if (s.is_ground) c.set("is_ground", true);
            if (!s.collides) c.set("collides", false);
            if (s.receives_damage) c.set("receives_damage", true);
            if (s.inflicts_damage) c.set("inflicts_damage", true);
            if (s.unsafe_ground) c.set("unsafe_ground", true);
            if (s.bone_controlled) { c.set("bone_controlled", true); c.set("bone_controller", s.bone_controller_id); }
        }
        j.set("collision", std::move(c));
    }
    return j;
}

static Json tool_scene_objects(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    av::SceneData sc = av::scene_load(path);
    if (sc.objects.empty() && sc.object_count == 0) { err = "scene load produced no objects (bad path?)"; return Json::Null(); }
    bool with_components = a.boolean("include_components", true);
    const size_t kMaxObjects = 2000;
    bool truncated = sc.objects.size() > kMaxObjects;
    Json objs = Json::Arr();
    size_t n = std::min(sc.objects.size(), kMaxObjects);
    for (size_t oi = 0; oi < n; ++oi) {
        const auto& o = sc.objects[oi];
        Json j = obj_json(o);
        if (with_components) {
            Json comps = Json::Arr();
            for (const auto& c : o.resolved_components.empty() ? o.components : o.resolved_components) {
                Json cj = Json::Obj();
                cj.set("type", c.type_name);
                cj.set("type_id", c.type_id);
                comps.push(std::move(cj));
            }
            j.set("components", std::move(comps));
        }
        objs.push(std::move(j));
    }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("object_count", (int)sc.objects.size());
    r.set("truncated", truncated);
    r.set("objects", std::move(objs));
    return r;
}

static Json tool_scene_summary(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    av::SceneData sc = av::scene_load(path);
    if (sc.objects.empty() && sc.object_count == 0) { err = "scene load produced no objects (bad path?)"; return Json::Null(); }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("objects", (int)sc.objects.size());
    r.set("object_libraries", (int)sc.object_libraries.size());
    r.set("bounds", (int)sc.bounds.size());
    r.set("groups", (int)sc.groups.size());
    r.set("onload_scripts", (int)sc.onload_scripts.size());
    r.set("external_libraries", (int)sc.external_libraries.size());
    r.set("missing_libraries", (int)sc.missing_libraries.size());
    r.set("entities", (int)sc.entities.size());
    r.set("physics_objects", (int)sc.physics_objects.size());
    r.set("collisions", (int)sc.collisions.size());
    r.set("waters", (int)sc.waters.size());
    r.set("lights", (int)sc.lights.size());
    r.set("shadows", (int)sc.shadows.size());
    r.set("overlays", (int)sc.overlays.size());
    Json mn = Json::Arr(); mn.push(Json::Num(sc.bounds_min[0])); mn.push(Json::Num(sc.bounds_min[1])); mn.push(Json::Num(sc.bounds_min[2]));
    Json mx = Json::Arr(); mx.push(Json::Num(sc.bounds_max[0])); mx.push(Json::Num(sc.bounds_max[1])); mx.push(Json::Num(sc.bounds_max[2]));
    r.set("bounds_min", std::move(mn));
    r.set("bounds_max", std::move(mx));
    Json libs = Json::Arr();
    for (const auto& p : sc.imported_library_paths) libs.push(Json::Str(p));
    r.set("external_library_paths", std::move(libs));
    Json miss = Json::Arr();
    for (const auto& m : sc.missing_libraries) miss.push(Json::Str(m));
    r.set("missing_library_paths", std::move(miss));
    for (const auto& o : sc.objects) {
        if (o.is_spawn_point) {
            r.set("spawn_point_object", o.name);
            Json sp = Json::Arr();
            sp.push(Json::Num(o.pos_x)); sp.push(Json::Num(o.pos_y)); sp.push(Json::Num(o.pos_z));
            r.set("spawn_pos", std::move(sp));
            break;
        }
    }
    return r;
}

static Json tool_scene_programs(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    av::SceneData sc = av::scene_load(path);
    if (sc.objects.empty() && sc.object_count == 0) { err = "scene load produced no objects (bad path?)"; return Json::Null(); }
    Json progs = Json::Arr();
    for (const auto& o : sc.objects) {
        if (o.onload.empty()) continue;
        std::string src;
        try { src = av::scene_program_source(o.onload); } catch (...) { src = "<decode failed>"; }
        Json p = Json::Obj();
        p.set("object", o.name);
        p.set("template", o.template_name);
        p.set("source", src);
        progs.push(std::move(p));
    }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("program_count", (int)progs.arr.size());
    r.set("programs", std::move(progs));
    return r;
}

static Json tool_scene_templates(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    av::SceneData sc = av::scene_load(path);
    if (sc.objects.empty() && sc.object_count == 0) { err = "scene load produced no objects (bad path?)"; return Json::Null(); }
    auto tpls = av::scene_list_templates(sc);
    Json out = Json::Arr();
    for (const auto& t : tpls) {
        Json tj = Json::Obj();
        tj.set("name", t.name);
        tj.set("scaling", t.scaling);
        Json comps = Json::Arr();
        for (const auto& c : t.component_types) comps.push(Json::Str(c));
        tj.set("components", std::move(comps));
        out.push(std::move(tj));
    }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("template_count", (int)out.arr.size());
    r.set("templates", std::move(out));
    return r;
}

static Json tool_scene_libraries(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    av::SceneData sc = av::scene_load(path);
    if (sc.objects.empty() && sc.object_count == 0) { err = "scene load produced no objects (bad path?)"; return Json::Null(); }
    Json libs = Json::Arr();
    for (size_t i = 0; i < sc.external_libraries.size(); ++i) {
        Json lj = Json::Obj();
        lj.set("name", i < sc.imported_library_names.size() ? sc.imported_library_names[i] : "");
        lj.set("path", i < sc.imported_library_paths.size() ? sc.imported_library_paths[i] : "");
        lj.set("bytes", (int)(long long)sc.external_libraries[i].size());
        libs.push(std::move(lj));
    }
    Json miss = Json::Arr();
    for (const auto& m : sc.missing_libraries) miss.push(Json::Str(m));
    Json r = Json::Obj();
    r.set("path", path);
    r.set("library_count", (int)sc.external_libraries.size());
    r.set("libraries", std::move(libs));
    r.set("missing", std::move(miss));
    return r;
}

static Json tool_search(const Json& a, std::string& err) {
    std::string dir = a.str("dir");
    std::string query = a.str("query");
    if (dir.empty()) { err = "missing 'dir'"; return Json::Null(); }
    if (query.empty()) { err = "missing 'query'"; return Json::Null(); }
    bool recursive = a.boolean("recursive", true);
    bool ci = a.boolean("case_sensitive", false) == false;
    int max_results = (int)a.num("max_results", 100);
    int max_snips = (int)a.num("snippets_per_file", 3);
    size_t ctx = (size_t)a.num("context", 120);
    std::string ext_filter = to_lower(a.str("extensions"));
    std::set<std::string> exts;
    if (!ext_filter.empty()) {
        std::stringstream ss(ext_filter);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = to_lower(item);
            if (!item.empty() && item[0] != '.') item = "." + item;
            if (!item.empty()) exts.insert(item);
        }
    }
    std::error_code ec;
    std::vector<std::pair<size_t, Json>> hits;
    auto scan = [&](auto& self, const fs::path& p) -> void {
        if ((int)hits.size() >= max_results * 4) return;
        std::error_code lec;
        if (fs::is_directory(p, lec)) {
            if (!recursive) return;
            for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, lec)) {
                std::string nm = e.path().filename().string();
                if (!nm.empty() && nm[0] == '.') continue;
                if (fs::is_directory(e.path(), lec)) self(self, e.path());
                else if (fs::is_regular_file(e.path(), lec)) {
                    if (!exts.empty() && !exts.count(to_lower(e.path().extension().string()))) continue;
                    std::error_code sec;
                    if (fs::file_size(e.path(), sec) > (64u << 20)) continue;
                    std::string bytes;
                    if (!read_bytes(e.path().string(), bytes)) continue;
                    std::string text = try_decode_any(e.path().string(), bytes);
                    std::string hay = !text.empty() ? text : printable(bytes);
                    size_t c = count_matches(hay, query, ci);
                    if (c == 0) continue;
                    Json fj = Json::Obj();
                    fj.set("path", e.path().string());
                    fj.set("matches", (int)c);
                    fj.set("decoded", !text.empty());
                    Json snips = Json::Arr();
                    for (auto& sn : find_snippets(hay, query, ci, max_snips, ctx))
                        snips.push(Json::Str(sn));
                    fj.set("snippets", std::move(snips));
                    hits.emplace_back(c, std::move(fj));
                }
            }
        } else if (fs::is_regular_file(p, lec)) {
            std::string bytes;
            if (!read_bytes(p.string(), bytes)) return;
            std::string text = try_decode_any(p.string(), bytes);
            std::string hay = !text.empty() ? text : printable(bytes);
            size_t c = count_matches(hay, query, ci);
            if (c == 0) return;
            Json fj = Json::Obj();
            fj.set("path", p.string());
            fj.set("matches", (int)c);
            fj.set("decoded", !text.empty());
            Json snips = Json::Arr();
            for (auto& sn : find_snippets(hay, query, ci, max_snips, ctx))
                snips.push(Json::Str(sn));
            fj.set("snippets", std::move(snips));
            hits.emplace_back(c, std::move(fj));
        }
    };
    scan(scan, fs::path(dir));
    std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    if ((int)hits.size() > max_results) hits.resize(max_results);
    Json out = Json::Arr();
    size_t total = 0;
    for (auto& h : hits) { total += h.first; out.push(std::move(h.second)); }
    Json r = Json::Obj();
    r.set("dir", dir);
    r.set("query", query);
    r.set("files_matched", (int)out.arr.size());
    r.set("total_matches", (int)(long long)total);
    r.set("results", std::move(out));
    return r;
}

static Json tool_search_scl(const Json& a, std::string& err) {
    std::string path = a.str("path");
    std::string query = a.str("query");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    if (query.empty()) { err = "missing 'query'"; return Json::Null(); }
    std::string bytes;
    if (!read_bytes(path, bytes)) { err = "cannot read file: " + path; return Json::Null(); }
    std::string text = try_decode_scl(bytes);
    if (text.empty()) text = printable(bytes);
    bool ci = !a.boolean("case_sensitive", false);
    int max_snips = (int)a.num("max_snippets", 20);
    size_t ctx = (size_t)a.num("context", 140);
    auto snips = find_snippets(text, query, ci, max_snips, ctx);
    Json s = Json::Arr();
    for (auto& sn : snips) s.push(Json::Str(sn));
    Json r = Json::Obj();
    r.set("path", path);
    r.set("query", query);
    r.set("matches", (int)count_matches(text, query, ci));
    r.set("decoded", !try_decode_scl(bytes).empty());
    r.set("snippets", std::move(s));
    return r;
}

static Json tool_file_info(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::error_code ec;
    if (!fs::exists(path, ec)) { err = "file does not exist: " + path; return Json::Null(); }
    std::string bytes;
    read_bytes(path, bytes);
    Json r = Json::Obj();
    r.set("path", path);
    r.set("size", file_size_str(path));
    r.set("bytes", (int)(long long)bytes.size());
    r.set("extension", fs::path(path).extension().string());
    r.set("type", detect_type(path, &bytes));
    std::string t = detect_type(path, &bytes);
    if (t == "pod") {
        av::PODModel m = av::pod_load(path);
        r.set("meshes", (int)m.meshes.size());
        r.set("nodes", (int)m.nodes.size());
        r.set("frames", m.num_frames);
        r.set("fps", m.fps);
    } else if (t == "pvr" || t == "tex" || t == "png") {
        Json h = texture_header(path, bytes);
        for (const auto& kv : h.obj) r.set(kv.first, kv.second);
    } else if (t == "scene") {
        av::SceneData sc = av::scene_load(path);
        r.set("objects", (int)sc.objects.size());
        r.set("external_libraries", (int)sc.external_libraries.size());
    } else if (t == "scl") {
        std::string text = try_decode_scl(bytes);
        r.set("decoded_bytes", (int)(long long)text.size());
    }
    return r;
}

static Json tool_pod_info(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    av::PODModel m = av::pod_load(path);
    if (m.meshes.empty() && m.nodes.empty()) { err = "pod_load failed for: " + path; return Json::Null(); }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("version", m.version);
    r.set("frames", m.num_frames);
    r.set("fps", m.fps);
    r.set("mesh_nodes", m.num_mesh_nodes);
    r.set("total_vertices", m.total_vertices);
    r.set("total_faces", m.total_faces);
    r.set("radius", m.radius);
    Json c = Json::Arr();
    c.push(Json::Num(m.center_x)); c.push(Json::Num(m.center_y)); c.push(Json::Num(m.center_z));
    r.set("center", std::move(c));
    Json bb = Json::Arr();
    bb.push(Json::Num(m.min_x)); bb.push(Json::Num(m.min_y)); bb.push(Json::Num(m.min_z));
    bb.push(Json::Num(m.max_x)); bb.push(Json::Num(m.max_y)); bb.push(Json::Num(m.max_z));
    r.set("aabb_min_max", std::move(bb));
    Json meshes = Json::Arr();
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const auto& me = m.meshes[i];
        Json mj = Json::Obj();
        mj.set("index", (int)i);
        mj.set("vertices", me.num_vertices);
        mj.set("faces", me.num_faces);
        mj.set("bones_per_vertex", me.bones_per_vertex);
        if (me.has_unpack_matrix) mj.set("unpack_matrix", true);
        meshes.push(std::move(mj));
    }
    r.set("meshes", std::move(meshes));
    Json nodes = Json::Arr();
    for (const auto& nd : m.nodes) {
        Json nj = Json::Obj();
        nj.set("name", nd.name);
        nj.set("parent", nd.parent_index);
        nj.set("object", nd.object_index);
        nodes.push(std::move(nj));
    }
    r.set("nodes", std::move(nodes));
    Json mats = Json::Arr();
    for (const auto& mt : m.materials) {
        Json mj = Json::Obj();
        mj.set("name", mt.name);
        mj.set("diffuse_texture_index", mt.diffuse_texture_index);
        mats.push(std::move(mj));
    }
    r.set("materials", std::move(mats));
    Json texs = Json::Arr();
    for (const auto& tx : m.texture_filenames) texs.push(Json::Str(tx));
    r.set("textures", std::move(texs));
    return r;
}

static Json tool_pod_blocks(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::string bytes;
    if (!read_bytes(path, bytes)) { err = "cannot read file: " + path; return Json::Null(); }
    Json r = Json::Obj();
    r.set("path", path);
    r.set("blocks", pod_block_histogram(bytes));
    return r;
}

static Json tool_list_files(const Json& a, std::string& err) {
    std::string dir = a.str("dir");
    if (dir.empty()) { err = "missing 'dir'"; return Json::Null(); }
    bool recursive = a.boolean("recursive", true);
    std::string pattern = to_lower(a.str("pattern"));
    int max = (int)a.num("max", 500);
    std::string root = fs::path(dir).string();
    std::error_code ec;
    Json files = Json::Arr();
    auto scan = [&](auto& self, const fs::path& p) -> void {
        if ((int)files.arr.size() >= max) return;
        std::error_code lec;
        if (fs::is_directory(p, lec)) {
            for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, lec)) {
                if ((int)files.arr.size() >= max) return;
                std::string nm = e.path().filename().string();
                if (!nm.empty() && nm[0] == '.') continue;
                if (fs::is_directory(e.path(), lec)) { if (recursive) self(self, e.path()); }
                else if (fs::is_regular_file(e.path(), lec)) {
                    std::string rel = fs::relative(e.path(), fs::path(dir), ec).string();
                    if (!pattern.empty() && to_lower(rel).find(pattern) == std::string::npos) continue;
                    Json fj = Json::Obj();
                    fj.set("path", rel);
                    fj.set("size", file_size_str(e.path()));
                    fj.set("type", detect_type(e.path().string()));
                    files.push(std::move(fj));
                }
            }
        }
    };
    scan(scan, fs::path(dir));
    Json r = Json::Obj();
    r.set("dir", dir);
    r.set("file_count", (int)files.arr.size());
    r.set("files", std::move(files));
    return r;
}

static Json tool_texture_info(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::string bytes;
    if (!read_bytes(path, bytes)) { err = "cannot read file: " + path; return Json::Null(); }
    Json r = texture_header(path, bytes);
    r.set("path", path);
    r.set("bytes", (int)(long long)bytes.size());
    if (!r.has("width")) {
        err = "not a supported texture (pvr/tex/png): " + path;
        return Json::Null();
    }
    if (a.boolean("decode", false)) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = pvr_decode_to_rgba((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h);
        r.set("decoded", ok);
        if (ok) {
            r.set("decoded_width", w);
            r.set("decoded_height", h);
            r.set("decoded_pixels", (int)(long long)rgba.size() / 4);
        } else {
            r.set("decode_error", "pvr_decode_to_rgba failed (unsupported format)");
        }
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════
// General file-system utilities (raw views for agents — text sources, ls, glob)
// ═══════════════════════════════════════════════════════════════════════════

// Read a byte range of a file (offset..offset+n) without loading the whole thing.
static bool read_bytes_range(const std::string& path, size_t off, size_t n, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)off);
    if (!f) return false;
    std::string buf(n, '\0');
    if (n) {
        f.read(&buf[0], (std::streamsize)n);
        buf.resize((size_t)f.gcount());
    }
    out = std::move(buf);
    return true;
}

// Non-printable bytes → '.', newlines/tabs kept (best-effort binary view).
static std::string printable_dots(const std::string& in, size_t cap) {
    std::string o;
    o.reserve(std::min(in.size(), cap));
    for (unsigned char c : in) {
        if (c == '\n' || c == '\t' || c == '\r' || (c >= 0x20 && c < 0x7f)) o += (char)c;
        else o += '.';
        if (o.size() >= cap) break;
    }
    return o;
}

// Strict-ASCII printable ratio (0x20..0x7e + \n\r\t). Unlike printable_ratio,
// high bytes (0x80+) count as binary — that's what raw-view detection needs.
static float ascii_ratio(const std::string& s) {
    if (s.empty()) return 0.0f;
    size_t n = 0;
    for (unsigned char c : s)
        if (c == '\n' || c == '\t' || c == '\r' || (c >= 0x20 && c < 0x7f)) ++n;
    return (float)n / (float)s.size();
}

// Classic hex dump: "00000000  48 65 6c 6c 6f ... |Hello|" rows of 16 bytes.
static std::string hex_dump(const std::string& b, size_t base_off, size_t max_rows) {
    std::string o;
    size_t i = 0, rows = 0;
    while (i < b.size()) {
        if (max_rows && rows >= max_rows) break;
        size_t end = std::min(i + 16, b.size());
        char buf[64];
        snprintf(buf, sizeof buf, "%08zx  ", base_off + i);
        o += buf;
        for (size_t j = i; j < end; ++j) {
            snprintf(buf, sizeof buf, "%02x ", (unsigned char)b[j]);
            o += buf;
        }
        for (size_t j = end; j < i + 16; ++j) o += "   ";
        o += " |";
        for (size_t j = i; j < end; ++j) {
            unsigned char c = (unsigned char)b[j];
            o += (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        o += "|\n";
        i = end;
        ++rows;
    }
    return o;
}

// fnmatch-style glob: '*' (any run), '?' (single char). No path separators.
static bool glob_match(const std::string& pat, const std::string& str) {
    const char* p = pat.c_str();
    const char* s = str.c_str();
    while (*p) {
        if (*p == '*') {
            while (*p == '*') ++p;
            if (!*p) return true;
            for (const char* t = s;; ++t) {
                if (glob_match(std::string(p), std::string(t))) return true;
                if (!*t) break;
            }
            return false;
        }
        if (!*s) return false;
        if (*p == '?' || *p == *s) { ++p; ++s; continue; }
        return false;
    }
    return *s == 0;
}

static Json tool_read_file(const Json& a, std::string& err) {
    std::string path = a.str("path");
    if (path.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { err = "not a regular file: " + path; return Json::Null(); }
    uintmax_t total = fs::file_size(path, ec);

    size_t offset = (size_t)a.num("offset", 0);
    size_t max_bytes = (size_t)a.num("max_bytes", 262144);
    if (max_bytes == 0 || max_bytes > (8u << 20)) max_bytes = 8u << 20;
    size_t max_lines = (size_t)a.num("max_lines", 200);
    bool hex = a.boolean("hex", false);
    if (offset > total) offset = (size_t)total;

    std::string bytes;
    if (!read_bytes_range(path, offset, max_bytes, bytes)) {
        err = "cannot read file: " + path;
        return Json::Null();
    }
    bool binary = ascii_ratio(bytes) < 0.5f;
    bool byte_trunc = offset + bytes.size() < (size_t)total;
    bool line_trunc = false;

    std::string text;
    if (hex) {
        text = hex_dump(bytes, offset, max_lines ? max_lines : 0);
        if (!text.empty() && text.back() == '\n') text.pop_back();
        line_trunc = max_lines && (bytes.size() + 15) / 16 > max_lines;
    } else if (binary) {
        text = printable_dots(bytes, bytes.size());
        if (max_lines) {
            size_t nl = count_lines(text);
            if (nl > max_lines) {
                size_t pos = 0;
                for (size_t k = 0; k < max_lines; ++k) pos = text.find('\n', pos) + 1;
                text.resize(pos);
                line_trunc = true;
            }
        }
    } else {
        text = bytes;
        if (max_lines) {
            size_t nl = count_lines(text);
            if (nl > max_lines) {
                size_t pos = 0;
                for (size_t k = 0; k < max_lines; ++k) pos = text.find('\n', pos) + 1;
                text.resize(pos);   // keep the trailing newline of the last kept line
                line_trunc = true;
            }
        }
    }

    Json r = Json::Obj();
    r.set("path", path);
    r.set("offset", (int)(long long)offset);
    r.set("bytes", (int)(long long)bytes.size());
    r.set("total_bytes", (int)(long long)total);
    r.set("binary", binary);
    r.set("hex", hex);
    r.set("lines", (int)(long long)count_lines(text));
    r.set("truncated", byte_trunc || line_trunc);
    if (byte_trunc) r.set("truncated_by_bytes", true);
    if (line_trunc) r.set("truncated_by_lines", true);
    if (byte_trunc) r.set("remaining_bytes", (int)(long long)(total - offset - bytes.size()));
    r.set("text", text);
    return r;
}

static Json tool_list_dir(const Json& a, std::string& err) {
    std::string dir = a.str("path");
    if (dir.empty()) dir = a.str("dir");
    if (dir.empty()) { err = "missing 'path'"; return Json::Null(); }
    bool recursive = a.boolean("recursive", false);
    std::string pattern = to_lower(a.str("pattern"));
    int max = (int)a.num("max", 500);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { err = "not a directory: " + dir; return Json::Null(); }

    struct Entry { std::string name; std::string rel; bool is_dir; uintmax_t size; };
    std::vector<Entry> entries;
    auto scan = [&](auto& self, const fs::path& p, const std::string& rel) -> void {
        if ((int)entries.size() >= max) return;
        std::error_code lec;
        for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, lec)) {
            if ((int)entries.size() >= max) return;
            std::string nm = e.path().filename().string();
            bool isd = fs::is_directory(e.path(), lec);
            std::string r = rel.empty() ? nm : rel + "/" + nm;
            // Always descend into directories so deeper matches are found,
            // but the pattern only filters what gets listed.
            if (isd && recursive) self(self, e.path(), r);
            if (!pattern.empty() && to_lower(nm).find(pattern) == std::string::npos) continue;
            uintmax_t sz = 0;
            if (!isd) sz = fs::file_size(e.path(), lec);
            entries.push_back({nm, r, isd, sz});
        }
    };
    scan(scan, fs::path(dir), "");

    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& x, const Entry& y) {
            if (x.is_dir != y.is_dir) return x.is_dir;
            return x.name < y.name;
        });
    if ((int)entries.size() > max) entries.resize(max);

    Json out = Json::Arr();
    for (const auto& e : entries) {
        Json j = Json::Obj();
        j.set("name", e.name);
        j.set("path", e.rel);
        j.set("type", e.is_dir ? "dir" : "file");
        if (!e.is_dir) {
            j.set("size", human_size(e.size));
            j.set("bytes", (int)(long long)e.size);
            j.set("extension", fs::path(e.name).extension().string());
        }
        out.push(std::move(j));
    }
    Json r = Json::Obj();
    r.set("path", dir);
    r.set("recursive", recursive);
    r.set("entry_count", (int)out.arr.size());
    r.set("truncated", out.arr.size() < entries.size() || entries.size() == (size_t)max && !entries.empty());
    r.set("entries", std::move(out));
    return r;
}

static Json tool_find_files(const Json& a, std::string& err) {
    std::string dir = a.str("path");
    if (dir.empty()) dir = a.str("dir");
    if (dir.empty()) { err = "missing 'path'"; return Json::Null(); }
    std::string pattern = a.str("pattern");
    if (pattern.empty()) pattern = "*";
    bool ci = !a.boolean("case_sensitive", false);
    bool include_dirs = a.boolean("include_dirs", false);
    int max = (int)a.num("max", 500);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { err = "not a directory: " + dir; return Json::Null(); }

    bool has_wild = pattern.find_first_of("*?") != std::string::npos;
    std::string pat = ci ? to_lower(pattern) : pattern;

    struct Entry { std::string rel; bool is_dir; uintmax_t size; };
    std::vector<Entry> matches;
    auto scan = [&](auto& self, const fs::path& p, const std::string& rel) -> void {
        if ((int)matches.size() >= max) return;
        std::error_code lec;
        for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, lec)) {
            if ((int)matches.size() >= max) return;
            std::string nm = e.path().filename().string();
            if (!nm.empty() && nm[0] == '.' && !fs::is_directory(e.path(), lec)) continue;
            bool isd = fs::is_directory(e.path(), lec);
            if (isd) {
                if (include_dirs) {
                    std::string pn = ci ? to_lower(nm) : nm;
                    bool hit = has_wild ? glob_match(pat, pn) : pn.find(pat) != std::string::npos;
                    if (hit) matches.push_back({rel.empty() ? nm : rel + "/" + nm, true, 0});
                }
                self(self, e.path(), rel.empty() ? nm : rel + "/" + nm);
                continue;
            }
            std::string pn = ci ? to_lower(nm) : nm;
            bool hit = has_wild ? glob_match(pat, pn) : pn.find(pat) != std::string::npos;
            if (!hit) continue;
            matches.push_back({rel.empty() ? nm : rel + "/" + nm, false,
                               fs::file_size(e.path(), lec)});
        }
    };
    scan(scan, fs::path(dir), "");
    std::sort(matches.begin(), matches.end(),
        [](const Entry& x, const Entry& y) { return x.rel < y.rel; });
    if ((int)matches.size() > max) matches.resize(max);

    Json out = Json::Arr();
    for (const auto& m : matches) {
        Json j = Json::Obj();
        j.set("path", m.rel);
        j.set("type", m.is_dir ? "dir" : "file");
        if (!m.is_dir) j.set("bytes", (int)(long long)m.size);
        out.push(std::move(j));
    }
    Json r = Json::Obj();
    r.set("path", dir);
    r.set("pattern", pattern);
    r.set("match_count", (int)out.arr.size());
    r.set("truncated", out.arr.size() < matches.size() || matches.size() == (size_t)max && !matches.empty());
    r.set("matches", std::move(out));
    return r;
}

// Build a JSON-schema object from a compact spec:
//   "name:type:desc,name2:type:desc;required1,required2"
static Json schema_from(const std::string& spec) {
    Json props = Json::Obj();
    Json req = Json::Arr();
    size_t semi = spec.find(';');
    std::string pspec = spec.substr(0, semi);
    std::string rspec = semi == std::string::npos ? "" : spec.substr(semi + 1);
    std::stringstream ps(pspec);
    std::string item;
    while (std::getline(ps, item, ',')) {
        if (item.empty()) continue;
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        std::string name = item.substr(0, c1);
        std::string rest = item.substr(c1 + 1);
        size_t c2 = rest.find(':');
        std::string type = c2 == std::string::npos ? rest : rest.substr(0, c2);
        std::string desc = c2 == std::string::npos ? "" : rest.substr(c2 + 1);
        Json p = Json::Obj();
        p.set("type", type);
        if (!desc.empty()) p.set("description", desc);
        props.set(name, std::move(p));
    }
    std::stringstream rs(rspec);
    while (std::getline(rs, item, ',')) {
        if (!item.empty()) req.push(Json::Str(item));
    }
    Json s = Json::Obj();
    s.set("type", "object");
    s.set("properties", std::move(props));
    if (!req.arr.empty()) s.set("required", std::move(req));
    return s;
}

static std::vector<Tool>& tool_registry() {
    static std::vector<Tool> reg = {
        {"scl_decode",
         "Decode a Swordigo .scl script file to readable markup / Lua source (filerift protobuf decode, with Lua fallback).",
         schema_from("path:string:Path to a .scl script file;path"), tool_scl_decode},
        {"scene_decode",
         "Decode a Swordigo .scene file to full FileRift markup text (all objects, components, libraries).",
         schema_from("path:string:Path to a .scene file;path"), tool_scene_decode},
        {"scene_objects",
         "Parse a .scene into structured JSON: every object's transform, mesh, entity/hero/monster data, animation bindings, physics, collision and component list.",
         schema_from("path:string:Path to a .scene file,include_components:boolean:Include per-object component list (default true);path"), tool_scene_objects},
        {"scene_summary",
         "Counts and global facts about a .scene: objects, libraries, waters, lights, spawn point, bounds, missing external .scl libraries.",
         schema_from("path:string:Path to a .scene file;path"), tool_scene_summary},
        {"scene_programs",
         "Extract the onload Lua program source attached to each scene object.",
         schema_from("path:string:Path to a .scene file;path"), tool_scene_programs},
        {"scene_templates",
         "List the add-object palette of a scene (templates from embedded + external .scl libraries, with component types).",
         schema_from("path:string:Path to a .scene file;path"), tool_scene_templates},
        {"scene_libraries",
         "List the external .scl object libraries a scene references (paths + names) and any that could not be resolved.",
         schema_from("path:string:Path to a .scene file;path"), tool_scene_libraries},
        {"search",
         "Search for a string across an entire folder (decoded-aware: .scl/.scene are searched as decoded text). Returns per-file match counts + context snippets.",
         schema_from("dir:string:Directory to search,query:string:String to find,extensions:string:Comma-separated extension filter (optional, e.g. .scl,.scene),recursive:boolean:Include subdirectories (default true),case_sensitive:boolean:Case-sensitive match (default false),max_results:number:Max files to return (default 100),snippets_per_file:number:Context snippets per file (default 3),context:number:Context chars around each hit (default 120);dir,query"), tool_search},
        {"search_scl",
         "Search for a string inside one .scl file (decoded markup + Lua fallback) with context snippets.",
         schema_from("path:string:Path to a .scl file,query:string:String to find,case_sensitive:boolean:Case-sensitive match (default false),max_snippets:number:Max snippets (default 20),context:number:Context chars around each hit (default 140);path,query"), tool_search_scl},
        {"file_info",
         "Quick facts about any asset: size, detected type, and type-specific details (POD mesh counts, texture dims, scene object count).",
         schema_from("path:string:Path to any asset file;path"), tool_file_info},
        {"pod_info",
         "Structural summary of a .POD model: meshes, nodes, materials, textures, frames, fps, bones, AABB.",
         schema_from("path:string:Path to a .POD model file;path"), tool_pod_info},
        {"pod_blocks",
         "POD block-id histogram for a .POD file (PowerVR chunk tags, e.g. 6006 Mesh, 6014 Interleaved, 6020 MeshUnpackMatrix).",
         schema_from("path:string:Path to a .POD model file;path"), tool_pod_blocks},
        {"list_files",
         "Enumerate assets under a directory (relative paths, sizes, detected types).",
         schema_from("dir:string:Directory to list,pattern:string:Case-insensitive substring filter on relative path,recursive:boolean:Include subdirectories (default true),max:number:Max entries (default 500);dir"), tool_list_files},
        {"texture_info",
         "Texture dimensions + format for .pvr / .tex / .png (header-only, or full decode with decode=true).",
         schema_from("path:string:Path to a .pvr/.tex/.png texture,decode:boolean:Full pixel decode via pvr_decode_to_rgba (default false);path"), tool_texture_info},
        {"read_file",
         "Read a raw file as text (txt/c/cpp/md/json/etc). Supports byte offset pagination, a max-lines cap, hex dump mode, and binary detection.",
         schema_from("path:string:Path to the file to read,offset:number:Byte offset to start from (default 0),max_bytes:number:Max bytes to read (default 262144),max_lines:number:Max lines to return (0=unlimited default 200),hex:boolean:Emit a hex dump instead of text;path"), tool_read_file},
        {"list_dir",
         "List a directory (ls): entries with name, relative path, type (dir/file), size, extension. Optionally recursive, with name substring filter.",
         schema_from("path:string:Directory to list,recursive:boolean:Include subdirectories (default false),pattern:string:Case-insensitive substring filter on entry name,max:number:Max entries (default 500);path"), tool_list_dir},
        {"find_files",
         "Recursively find files by glob/substring name pattern (e.g. *.cpp, main, *.h). Returns relative paths, types and sizes.",
         schema_from("path:string:Directory to search,pattern:string:Glob (e.g. *.cpp) or substring to match against file names (default *),case_sensitive:boolean:Case-sensitive match (default false),include_dirs:boolean:Also list matching directories (default false),max:number:Max matches (default 500);path"), tool_find_files},
    };
    return reg;
}

static Json g_tools() {
    static Json tools = Json::Null();
    if (tools.is_null()) {
        tools = Json::Arr();
        for (const auto& t : tool_registry()) {
            Json tj = Json::Obj();
            tj.set("name", t.name);
            tj.set("description", t.desc);
            tj.set("inputSchema", t.schema);
            tools.push(std::move(tj));
        }
    }
    return tools;
}

// ═══════════════════════════════════════════════════════════════════════════
// §4  MCP / JSON-RPC 2.0 session
// ═══════════════════════════════════════════════════════════════════════════

struct Session {
    std::string protocol_version = "2024-11-05";
    std::string client_name;
    std::string root;
};

static Session g_session;   // single-client stdio session (also used by the GUI tester)

static const char* kServerName = "ruby-mcp";
static const char* kServerVersion = "0.8.0";

static Json err_json(int code, const std::string& msg) {
    Json e = Json::Obj();
    e.set("code", code);
    e.set("message", msg);
    return e;
}

static Json result_message(const Json& id, Json result) {
    Json m = Json::Obj();
    m.set("jsonrpc", "2.0");
    if (!id.is_null()) m.set("id", id);
    m.set("result", std::move(result));
    return m;
}
static Json error_message(const Json& id, int code, const std::string& msg, const Json& data = Json::Null()) {
    Json m = Json::Obj();
    m.set("jsonrpc", "2.0");
    if (!id.is_null()) m.set("id", id);
    Json e = err_json(code, msg);
    if (!data.is_null()) e.set("data", data);
    m.set("error", std::move(e));
    return m;
}

// walk the resource root and build resources/list output
static Json list_resources(const Session& s) {
    Json out = Json::Arr();
    std::error_code ec;
    auto scan = [&](auto& self, const fs::path& p) -> void {
        if (out.arr.size() >= 2000) return;
        std::error_code lec;
        for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, lec)) {
            if (out.arr.size() >= 2000) return;
            std::string nm = e.path().filename().string();
            if (!nm.empty() && nm[0] == '.') continue;
            if (fs::is_directory(e.path(), lec)) { self(self, e.path()); continue; }
            std::string ext = to_lower(e.path().extension().string());
            static const std::set<std::string> kInteresting = {
                ".scene", ".scl", ".pod", ".pvr", ".tex", ".png", ".jpg", ".wav", ".ogg", ".lua", ".txt", ".json"};
            if (!kInteresting.count(ext)) continue;
            std::string rel = fs::relative(e.path(), fs::path(s.root), ec).string();
            Json r = Json::Obj();
            r.set("uri", "file://" + e.path().string());
            r.set("name", rel);
            std::string t = detect_type(e.path().string());
            r.set("mimeType", (t == "scene" || t == "scl" || t == "text") ? "text/plain" : "application/octet-stream");
            out.push(std::move(r));
        }
    };
    if (fs::is_directory(s.root, ec)) scan(scan, fs::path(s.root));
    return out;
}

static std::string handle_request(Session& s, const Json& msg, Json& resp_out) {
    // ── method-based requests / notifications ──
    std::string method = msg.str("method");
    Json id = msg.get("id") ? *msg.get("id") : Json::Null();

    if (method == "initialize") {
        s.protocol_version = msg.str("protocolVersion", s.protocol_version);
        if (const Json* ci = msg.get("clientInfo")) {
            s.client_name = ci->str("name");
        }
        Json caps = Json::Obj();
        Json tools_c = Json::Obj(); tools_c.set("listChanged", false);
        Json res_c  = Json::Obj();  res_c.set("subscribe", false); res_c.set("listChanged", false);
        Json prm_c  = Json::Obj();  prm_c.set("listChanged", false);
        caps.set("tools", std::move(tools_c));
        caps.set("resources", std::move(res_c));
        caps.set("prompts", std::move(prm_c));
        Json info = Json::Obj();
        info.set("name", kServerName);
        info.set("version", kServerVersion);
        Json result = Json::Obj();
        result.set("protocolVersion", "2024-11-05");
        result.set("capabilities", std::move(caps));
        result.set("serverInfo", std::move(info));
        resp_out = result_message(id, std::move(result));
        return "";
    }
    if (method == "notifications/initialized" || method == "notifications/cancelled") return "notify";
    if (method == "ping") { resp_out = result_message(id, Json::Obj()); return ""; }

    if (method == "tools/list") {
        Json result = Json::Obj();
        result.set("tools", g_tools());
        resp_out = result_message(id, std::move(result));
        return "";
    }
    if (method == "tools/call") {
        const Json* par = msg.get("params");
        Json empty = Json::Obj();
        const Json& pm = par ? *par : empty;
        std::string name = pm.str("name");
        const Json* args = pm.get("arguments");
        const Json& argv = args ? *args : empty;
        for (const auto& t : tool_registry()) {
            if (name != t.name) continue;
            std::string terr;
            Json data;
            StdoutGuard guard;   // keep protocol stdout clean
            try {
                data = t.fn(argv, terr);
            } catch (const std::exception& e) {
                terr = std::string("exception: ") + e.what();
            } catch (...) {
                terr = "unknown exception";
            }
            Json result = Json::Obj();
            Json content = Json::Arr();
            Json text = Json::Obj();
            if (terr.empty()) {
                text.set("type", "text");
                text.set("text", data.pretty());
                content.push(std::move(text));
                result.set("content", std::move(content));
                result.set("structuredContent", data);
            } else {
                text.set("type", "text");
                text.set("text", "ERROR: " + terr);
                content.push(std::move(text));
                result.set("content", std::move(content));
                result.set("isError", true);
            }
            resp_out = result_message(id, std::move(result));
            return "";
        }
        resp_out = error_message(id, -32602, "Unknown tool: " + name);
        return "";
    }

    if (method == "resources/list") {
        Json result = Json::Obj();
        result.set("resources", list_resources(s));
        resp_out = result_message(id, std::move(result));
        return "";
    }
    if (method == "resources/read") {
        const Json* par = msg.get("params");
        Json empty = Json::Obj();
        const Json& pm = par ? *par : empty;
        std::string uri = pm.str("uri");
        if (uri.rfind("file://", 0) != 0) {
            resp_out = error_message(id, -32602, "Only file:// URIs are supported");
            return "";
        }
        std::string path = uri.substr(7);
        std::string bytes;
        StdoutGuard guard;
        if (!read_bytes(path, bytes)) {
            resp_out = error_message(id, -32002, "Cannot read resource: " + uri);
            return "";
        }
        Json item = Json::Obj();
        item.set("uri", uri);
        std::string t = detect_type(path, &bytes);
        if (t == "scene") { item.set("mimeType", "text/plain"); item.set("text", try_decode_scene(bytes)); }
        else if (t == "scl") { item.set("mimeType", "text/plain"); item.set("text", try_decode_scl(bytes)); }
        else if (t == "text") { item.set("mimeType", "text/plain"); item.set("text", bytes); }
        else if (t == "pod") {
            item.set("mimeType", "application/json");
            Json argv = Json::Obj();
            argv.set("path", path);
            std::string terr;
            Json pinfo = tool_pod_info(argv, terr);
            item.set("text", terr.empty() ? pinfo.pretty() : ("ERROR: " + terr));
        }
        else {
            Json h = texture_header(path, bytes);
            item.set("mimeType", "application/json");
            h.set("path", path);
            item.set("text", h.pretty());
        }
        Json contents = Json::Arr();
        contents.push(std::move(item));
        Json result = Json::Obj();
        result.set("contents", std::move(contents));
        resp_out = result_message(id, std::move(result));
        return "";
    }
    if (method == "resources/subscribe" || method == "resources/unsubscribe") {
        resp_out = result_message(id, Json::Obj());
        return "";
    }

    if (method == "prompts/list") {
        Json prompts = Json::Arr();
        Json p1 = Json::Obj();
        p1.set("name", "analyze_scene");
        p1.set("description", "Deep-dive a Swordigo .scene: summarize objects, entities, AI, physics, lights and scripts.");
        Json p1a = Json::Arr();
        Json arg = Json::Obj();
        arg.set("name", "path");
        arg.set("description", "Path to a .scene file");
        arg.set("required", true);
        p1a.push(std::move(arg));
        p1.set("arguments", std::move(p1a));
        prompts.push(std::move(p1));
        Json p2 = Json::Obj();
        p2.set("name", "explain_scl");
        p2.set("description", "Explain what a Swordigo .scl object-library / script does, step by step.");
        Json p2a = Json::Arr();
        Json arg2 = Json::Obj();
        arg2.set("name", "path");
        arg2.set("description", "Path to a .scl file");
        arg2.set("required", true);
        p2a.push(std::move(arg2));
        p2.set("arguments", std::move(p2a));
        prompts.push(std::move(p2));
        Json result = Json::Obj();
        result.set("prompts", std::move(prompts));
        resp_out = result_message(id, std::move(result));
        return "";
    }
    if (method == "prompts/get") {
        const Json* par = msg.get("params");
        Json empty = Json::Obj();
        const Json& pm = par ? *par : empty;
        std::string name = pm.str("name");
        const Json* args = pm.get("arguments");
        const Json& argv = args ? *args : empty;
        std::string path = argv.str("path", "");
        std::string text;
        if (name == "analyze_scene") {
            text = "Use scene_summary, scene_objects and scene_programs on '" + path +
                   "' to analyze this Swordigo scene: what objects, entities/AI, physics, lighting and scripts does it contain? ";
        } else if (name == "explain_scl") {
            text = "Decode the .scl file at '" + path + "' with scl_decode and explain what object templates / scripts it defines, and how the game would use them.";
        } else {
            resp_out = error_message(id, -32602, "Unknown prompt: " + name);
            return "";
        }
        Json msg2 = Json::Obj();
        msg2.set("role", "user");
        Json content = Json::Obj();
        content.set("type", "text");
        content.set("text", text);
        msg2.set("content", std::move(content));
        Json messages = Json::Arr();
        messages.push(std::move(msg2));
        Json result = Json::Obj();
        result.set("description", "");
        result.set("messages", std::move(messages));
        resp_out = result_message(id, std::move(result));
        return "";
    }

    if (method == "logging/setLevel" || method == "completion/complete") {
        resp_out = result_message(id, Json::Obj());
        return "";
    }

    // Unknown method. Notifications (no id) must never receive a response.
    if (!msg.has("id")) return "notify";
    resp_out = error_message(id, -32601, "Method not found: " + method);
    return "";
}

// ═══════════════════════════════════════════════════════════════════════════
// §5  Entry points
// ═══════════════════════════════════════════════════════════════════════════

std::string DefaultRootDir() {
    const char* env = getenv("MCP_ROOT");
    if (env && *env) return env;
    const char* home = getenv("HOME");
    if (home) {
        std::string p = std::string(home) + "/.local/share/swordigo-desktop/assets";
        std::error_code ec;
        if (fs::is_directory(p, ec)) return p;
    }
    return ".";
}

std::string ToolListText() {
    std::string out;
    for (const auto& t : tool_registry()) {
        out += "  " + std::string(t.name) + " — " + t.desc + "\n";
    }
    return out;
}

bool HandleLine(const std::string& line, std::string& out) {
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) trimmed.pop_back();
    if (trimmed.empty()) return false;

    if (g_session.root.empty()) g_session.root = DefaultRootDir();
    Json msg;
    std::string perr;
    if (!Json::parse(trimmed, msg, perr)) {
        out += error_message(Json::Null(), -32700, "Parse error: " + perr).dump();
        return true;
    }

    if (msg.t == Json::ARR) {
        if (msg.arr.empty()) {   // JSON-RPC: empty batch is invalid
            out += error_message(Json::Null(), -32600, "Invalid Request (empty batch)").dump();
            return true;
        }
        for (const auto& m : msg.arr) {
            Json resp;
            std::string tag = handle_request(g_session, m, resp);
            if (tag == "notify") continue;
            if (!out.empty()) out += "\n";
            out += resp.dump();
        }
        return true;
    }
    if (msg.t != Json::OBJ) {
        out += error_message(Json::Null(), -32600, "Invalid Request").dump();
        return true;
    }
    // A client→server response (has id + result/error) — ignore in server role.
    if (!msg.has("method") && msg.has("id")) return true;

    Json resp;
    std::string tag = handle_request(g_session, msg, resp);
    if (tag == "notify") return true;
    out += resp.dump();
    return true;
}

int RunStdioServer(const std::string& root_dir) {
    std::string root = root_dir.empty() ? DefaultRootDir() : root_dir;
    g_session.root = root;
    // Unbuffer C stdout so no userspace buffer can ever flush onto the
    // protocol channel outside of a StdoutGuard window.
    setvbuf(stdout, nullptr, _IONBF, 0);
    fprintf(stderr, "[ruby-mcp] %s v%s — root=%s\n", kServerName, kServerVersion, root.c_str());
    fprintf(stderr, "[ruby-mcp] tools: %zu registered\n", tool_registry().size());
    fflush(stderr);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::string resp;
        if (!HandleLine(line, resp)) continue;
        std::cout << resp << "\n";
        std::cout.flush();
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// §6  HTTP transport — Streamable HTTP (+ legacy SSE channel on GET)
//
// Serves the SAME dispatcher as stdio (HandleLine / g_session) on:
//     POST /mcp          JSON-RPC request(s) → application/json response
//     POST /sse          alias (ChatGPT's example URL used /sse)
//     GET  /mcp          SSE keep-alive channel (server → client), pings
//     OPTIONS /mcp       CORS preflight (for web-based MCP clients)
// Local-only (127.0.0.1), one detached thread per connection.
// ═══════════════════════════════════════════════════════════════════════════
#ifdef _WIN32

int RunHttpServer(int, const std::string&) {
    fprintf(stderr, "[ruby-mcp] HTTP transport needs POSIX sockets — use stdio (mcp) on Windows.\n");
    return 1;
}

#else

static std::string g_session_id;   // echoed in Mcp-Session-Id (informational)

static std::string http_status_line(int code) {
    switch (code) {
        case 200: return "HTTP/1.1 200 OK";
        case 202: return "HTTP/1.1 202 Accepted";
        case 204: return "HTTP/1.1 204 No Content";
        case 400: return "HTTP/1.1 400 Bad Request";
        case 405: return "HTTP/1.1 405 Method Not Allowed";
        default:  return "HTTP/1.1 200 OK";
    }
}

static void http_write(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

// Read one HTTP request: request-line + headers + Content-Length body.
static bool http_read_request(int fd, std::string& method, std::string& path,
                              std::map<std::string, std::string>& headers,
                              std::string& body) {
    std::string head;
    char tmp[4096];
    size_t hlen = std::string::npos;
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = POLLIN;
    while (true) {
        int pr = poll(&pfd, 1, 5000);
        if (pr <= 0) return false;                      // timeout / error
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        head.append(tmp, (size_t)n);
        size_t p1 = head.find("\r\n\r\n");
        size_t p2 = head.find("\n\n");
        if (p1 != std::string::npos) hlen = p1 + 4;
        else if (p2 != std::string::npos) hlen = p2 + 2;
        if (hlen != std::string::npos) break;
        if (head.size() > 64 * 1024) return false;
    }
    std::string raw_head = head.substr(0, hlen);
    body = head.substr(hlen);

    size_t eol = raw_head.find('\n');
    if (eol == std::string::npos) return false;
    std::string reqline = raw_head.substr(0, eol);
    if (!reqline.empty() && reqline.back() == '\r') reqline.pop_back();
    std::istringstream ls(reqline);
    ls >> method >> path;

    std::string line;
    std::istringstream hs(raw_head.substr(eol + 1));
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        for (char& c : k) c = (char)tolower((unsigned char)c);
        headers[k] = v;
    }

    size_t cl = 0;
    auto it = headers.find("content-length");
    if (it != headers.end()) cl = (size_t)std::atoll(it->second.c_str());
    while (body.size() < cl) {
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        body.append(tmp, (size_t)n);
    }
    body.resize(cl);
    return true;
}

static void http_connection(int cfd) {
    std::string method, path, body;
    std::map<std::string, std::string> headers;
    if (!http_read_request(cfd, method, path, headers, body)) {
        ::close(cfd);
        return;
    }

    if (method == "OPTIONS") {   // CORS preflight for web MCP clients
        http_write(cfd,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Mcp-Session-Id, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n\r\n");
        ::close(cfd);
        return;
    }

    if (method == "GET") {
        bool want_sse = headers["accept"].find("text/event-stream") != std::string::npos;
        if (!want_sse) {
            http_write(cfd,
                "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, POST, OPTIONS\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
            ::close(cfd);
            return;
        }
        // SSE channel: announce the endpoint, then hold with heartbeat pings.
        http_write(cfd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n"
            "event: endpoint\r\ndata: /mcp\r\n\r\n");
        struct pollfd pfd;
        pfd.fd = cfd; pfd.events = POLLIN;
        char drain[256];
        while (true) {
            int pr = poll(&pfd, 1, 15000);
            if (pr < 0) break;
            if (pr > 0) {
                ssize_t n = ::recv(cfd, drain, sizeof drain, 0);
                if (n <= 0) break;                       // client gone
            } else {
                http_write(cfd, ": ping\n\n");   // keep-alive heartbeat
            }
        }
        ::close(cfd);
        return;
    }

    if (method != "POST") {
        http_write(cfd,
            "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, POST, OPTIONS\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n");
        ::close(cfd);
        return;
    }

    if (path != "/mcp" && path != "/sse") {
        http_write(cfd,
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        ::close(cfd);
        return;
    }

    // POST: run through the same dispatcher as stdio.
    std::string out;
    HandleLine(body, out);
    if (out.empty()) {          // pure notification → 202 Accepted (no body)
        http_write(cfd,
            "HTTP/1.1 202 Accepted\r\nMcp-Session-Id: " + g_session_id +
            "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        ::close(cfd);
        return;
    }

    // Multiple response lines → one JSON-RPC batch array for the JSON body.
    std::vector<std::string> lines;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    std::string resp_body = lines[0];
    if (lines.size() > 1) {   // multiple responses → JSON-RPC batch array
        resp_body = "[";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) resp_body += ",";
            resp_body += lines[i];
        }
        resp_body += "]";
    }
    char lenbuf[32];
    snprintf(lenbuf, sizeof lenbuf, "%zu", resp_body.size());
    http_write(cfd,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Mcp-Session-Id: " + g_session_id + "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: " + std::string(lenbuf) + "\r\n"
        "Connection: close\r\n\r\n" + resp_body);
    ::close(cfd);
}

int RunHttpServer(int port, const std::string& root_dir) {
    std::string root = root_dir.empty() ? DefaultRootDir() : root_dir;
    g_session.root = root;
    g_session_id = "ruby-" + std::to_string((long long)time(nullptr));

    int lfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        fprintf(stderr, "[ruby-mcp] socket() failed: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // local-only, safe default
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (sockaddr*)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[ruby-mcp] cannot bind 127.0.0.1:%d — %s\n", port, strerror(errno));
        ::close(lfd);
        return 1;
    }
    if (listen(lfd, 8) < 0) {
        fprintf(stderr, "[ruby-mcp] listen() failed: %s\n", strerror(errno));
        ::close(lfd);
        return 1;
    }
    fprintf(stderr, "[ruby-mcp] HTTP server:  http://127.0.0.1:%d/mcp   (root=%s)\n", port, root.c_str());
    fprintf(stderr, "[ruby-mcp] ChatGPT desktop → Settings → MCP Servers → RubySDK →"
                    " URL = http://127.0.0.1:%d/mcp  (No Auth)\n", port);
    fprintf(stderr, "[ruby-mcp] Ctrl-C to stop.\n");
    fflush(stderr);

    while (true) {
        int cfd = (int)accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread([cfd]() { http_connection(cfd); }).detach();
    }
    ::close(lfd);
    return 0;
}

#endif // _WIN32

} // namespace mcp
