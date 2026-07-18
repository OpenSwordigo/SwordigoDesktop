/* batch_converter.cpp — Ruby Batch Texture Converter (100% Native with Metadata Tracking)
 *
 * EXPORT  (game assets → editable PNG)
 *   .pvr / .tex.png  →  .pvr.png / .tex.png.png
 *   - Automatically extracts original format type, container, dimension details.
 *   - Symmetrically flips the output vertically so exported PNGs are right-side up.
 *   - Appends them to "ruby.sdk" metadata file in the destination folder.
 *
 * IMPORT  (editable PNG → game assets)
 *   .pvr.png / .tex.png.png  →  .pvr / .tex.png
 *   - Symmetrically flips the image vertically to store it as the game expects.
 *   - Performs native premultiplied alpha conversions for correct game blending.
 *   - Looks up the file's original layout configuration in "ruby.sdk" (supports both
 *     relative path and filename-only lookups).
 *   - Natively compiles and compresses to the exact matching game format:
 *     - ETC1 (native block compression)
 *     - PVR uncompressed formats (reconstructs exact channel mappings)
 *     - TEX formats (uncompressed types 1 to 8):
 *       - 1: RGBA_8888 (4 bytes/px)
 *       - 2: RGBA_4444 (2 bytes/px, packed r4g4b4a4)
 *       - 3: RGBA_5551 (2 bytes/px, packed r5g5b5a1)
 *       - 4: RGB_888 (3 bytes/px, raw rgb)
 *       - 5: RGB_565 (2 bytes/px, packed r5g6b5)
 *       - 6: LUMINANCE_8 (1 byte/px, L)
 *       - 7: ALPHA_8 (1 byte/px, A)
 *       - 8: LUMINANCE_ALPHA_88 (2 bytes/px, LA)
 *     - Gzip compression (wrapped dynamically for .tex.png assets)
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "tools/batch_converter.h"
#include "platform/pvr_loader.h"
#include "platform/pvrtc_decoder.h"

#include <zlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <cassert>
#include <SDL3/SDL.h>

// imgui.h must be included OUTSIDE any namespace
#include "imgui.h"

namespace fs = std::filesystem;
namespace batch {

// ═══════════════════════════════════════════════════════════════════════════════
// §1  PVR Headers
// ═══════════════════════════════════════════════════════════════════════════════

#pragma pack(push, 1)
struct PVRv3Hdr {
    uint32_t version      = 0x03525650; // "PVR\3"
    uint32_t flags        = 0;
    uint64_t pixel_format = 0;
    uint32_t color_space  = 0;
    uint32_t channel_type = 0;
    uint32_t height       = 0;
    uint32_t width        = 0;
    uint32_t depth        = 1;
    uint32_t num_surfaces = 1;
    uint32_t num_faces    = 1;
    uint32_t mip_count    = 1;
    uint32_t metadata_sz  = 0;
};

struct PVRv2Hdr {
    uint32_t header_size;
    uint32_t height;
    uint32_t width;
    uint32_t mip_count;
    uint32_t flags;
    uint32_t data_size;
    uint32_t bpp;
    uint32_t mask_r;
    uint32_t mask_g;
    uint32_t mask_b;
    uint32_t magic;
    uint32_t num_surfaces;
};
#pragma pack(pop)

#define PVR3_FMT_ETC1      6ULL
#define PVR3_FMT_RGBA8888  0x0808080861626772ULL

// ═══════════════════════════════════════════════════════════════════════════════
// §2  Gzip inflate & deflate helpers
// ═══════════════════════════════════════════════════════════════════════════════

static bool gzip_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;

    strm.next_in  = const_cast<uint8_t*>(in.data());
    strm.avail_in = (uInt)in.size();

    uint8_t buf[65536];
    do {
        strm.next_out  = buf;
        strm.avail_out = sizeof(buf);
        int ret = deflate(&strm, Z_FINISH);
        if (ret == Z_STREAM_ERROR) { deflateEnd(&strm); return false; }
        size_t produced = sizeof(buf) - strm.avail_out;
        out.insert(out.end(), buf, buf + produced);
    } while (strm.avail_out == 0);

    deflateEnd(&strm);
    return true;
}

static bool gzip_decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;

    strm.next_in  = const_cast<uint8_t*>(in.data());
    strm.avail_in = (uInt)in.size();

    uint8_t buf[65536];
    int ret;
    do {
        strm.next_out  = buf;
        strm.avail_out = sizeof(buf);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return false;
        }
        size_t produced = sizeof(buf) - strm.avail_out;
        out.insert(out.end(), buf, buf + produced);
    } while (ret != Z_STREAM_END && strm.avail_in > 0);

    inflateEnd(&strm);
    return (ret == Z_STREAM_END);
}

// ═══════════════════════════════════════════════════════════════════════════════
// §3  Native ETC1 Block Encoder
// ═══════════════════════════════════════════════════════════════════════════════

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static const int kETC1Modifiers[8][2] = {
    { 2, 8}, { 5, 17}, { 9, 29}, {13, 42},
    {18,56}, {24, 71}, {33, 92}, {47,127}
};

static void quant555(int r, int g, int b, int& r5, int& g5, int& b5) {
    r5 = clamp8(r) >> 3;
    g5 = clamp8(g) >> 3;
    b5 = clamp8(b) >> 3;
}

static void expand555(int r5, int g5, int b5, int& r, int& g, int& b) {
    r = (r5 << 3) | (r5 >> 2);
    g = (g5 << 3) | (g5 >> 2);
    b = (b5 << 3) | (b5 >> 2);
}

static uint64_t score_subblock(const uint8_t* pixels[8], int br, int bg, int bb, int table_idx, uint8_t selectors[8]) {
    uint64_t err = 0;
    const int* mods = kETC1Modifiers[table_idx];
    for (int i = 0; i < 8; i++) {
        int pr = pixels[i][0], pg = pixels[i][1], pb = pixels[i][2];
        uint64_t best = UINT64_MAX;
        uint8_t  sel  = 0;
        for (uint8_t s = 0; s < 4; s++) {
            int sign = (s < 2) ? 1 : -1;
            int mod  = (s & 1) ? mods[1] : mods[0];
            int dr = clamp8(br + sign*mod) - pr;
            int dg = clamp8(bg + sign*mod) - pg;
            int db = clamp8(bb + sign*mod) - pb;
            uint64_t e = (uint64_t)(dr*dr + dg*dg + db*db);
            if (e < best) { best = e; sel = s; }
        }
        err += best;
        selectors[i] = sel;
    }
    return err;
}

static void encode_etc1_block(const uint8_t* src, int src_stride_bytes, uint8_t* dst) {
    const uint8_t* P[4][4];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            P[row][col] = src + row * src_stride_bytes + col * 4;

    auto avg_rgb = [](const uint8_t* pp[8], int& ar, int& ag, int& ab) {
        int sr = 0, sg = 0, sb = 0;
        for (int i = 0; i < 8; i++) { sr += pp[i][0]; sg += pp[i][1]; sb += pp[i][2]; }
        ar = sr / 8; ag = sg / 8; ab = sb / 8;
    };

    uint64_t best_err = UINT64_MAX;
    uint64_t best_block = 0;

    for (int flip = 0; flip < 2; flip++) {
        const uint8_t* ppA[8], *ppB[8];
        if (flip == 0) {
            for (int row = 0; row < 4; row++) {
                ppA[row*2+0] = P[row][0]; ppA[row*2+1] = P[row][1];
                ppB[row*2+0] = P[row][2]; ppB[row*2+1] = P[row][3];
            }
        } else {
            for (int col = 0; col < 4; col++) {
                ppA[col*2+0] = P[0][col]; ppA[col*2+1] = P[1][col];
                ppB[col*2+0] = P[2][col]; ppB[col*2+1] = P[3][col];
            }
        }

        int arA, agA, abA, arB, agB, abB;
        avg_rgb(ppA, arA, agA, abA);
        avg_rgb(ppB, arB, agB, abB);

        int r5A, g5A, b5A, r5B, g5B, b5B;
        quant555(arA, agA, abA, r5A, g5A, b5A);
        quant555(arB, agB, abB, r5B, g5B, b5B);

        int dr = r5B - r5A, dg = g5B - g5A, db = b5B - b5A;
        auto clamp3s = [](int v) { return v < -4 ? -4 : (v > 3 ? 3 : v); };
        dr = clamp3s(dr); dg = clamp3s(dg); db = clamp3s(db);
        int r5Beff = r5A + dr, g5Beff = g5A + dg, b5Beff = b5A + db;
        if (r5Beff < 0 || r5Beff > 31 || g5Beff < 0 || g5Beff > 31 || b5Beff < 0 || b5Beff > 31) continue;

        int brA, bgA, bbA, brB, bgB, bbB;
        expand555(r5A, g5A, b5A, brA, bgA, bbA);
        expand555(r5Beff, g5Beff, b5Beff, brB, bgB, bbB);

        for (int tA = 0; tA < 8; tA++) {
            for (int tB = 0; tB < 8; tB++) {
                uint8_t selA[8], selB[8];
                uint64_t eA = score_subblock(ppA, brA, bgA, bbA, tA, selA);
                uint64_t eB = score_subblock(ppB, brB, bgB, bbB, tB, selB);
                uint64_t total_err = eA + eB;
                if (total_err >= best_err) continue;
                best_err = total_err;

                uint64_t word = 0;
                word |= (uint64_t)(r5A & 31) << 59;
                word |= (uint64_t)((uint8_t)(dr & 7)) << 56;
                word |= (uint64_t)(g5A & 31) << 51;
                word |= (uint64_t)((uint8_t)(dg & 7)) << 48;
                word |= (uint64_t)(b5A & 31) << 43;
                word |= (uint64_t)((uint8_t)(db & 7)) << 40;
                word |= (uint64_t)(tA & 7) << 37;
                word |= (uint64_t)(tB & 7) << 34;
                word |= (1ULL << 33);
                if (flip) word |= (1ULL << 32);

                static const uint8_t SEL2MSB[4] = {0, 1, 0, 1};
                static const uint8_t SEL2LSB[4] = {0, 0, 1, 1};

                auto pixel_index = [flip](int sub, int i) -> int {
                    if (flip == 0) {
                        int row = i / 2;
                        int col = (i % 2) + (sub == 1 ? 2 : 0);
                        return col * 4 + row;
                    } else {
                        int col = i / 2;
                        int row = (i % 2) + (sub == 1 ? 2 : 0);
                        return col * 4 + row;
                    }
                };

                for (int i = 0; i < 8; i++) {
                    int pidx = pixel_index(0, i);
                    uint8_t s = selA[i];
                    word |= (uint64_t)SEL2MSB[s] << (16 + pidx);
                    word |= (uint64_t)SEL2LSB[s] << pidx;
                }
                for (int i = 0; i < 8; i++) {
                    int pidx = pixel_index(1, i);
                    uint8_t s = selB[i];
                    word |= (uint64_t)SEL2MSB[s] << (16 + pidx);
                    word |= (uint64_t)SEL2LSB[s] << pidx;
                }
                best_block = word;
            }
        }
    }

    for (int i = 0; i < 8; i++)
        dst[i] = (uint8_t)(best_block >> (56 - i * 8));
}

// ═══════════════════════════════════════════════════════════════════════════════
// §4  Metadata Extract & Load Hub
// ═══════════════════════════════════════════════════════════════════════════════

static std::string extract_original_metadata(const std::vector<uint8_t>& file_data) {
    if (file_data.size() < 12) return "";

    std::vector<uint8_t> decompressed;
    const uint8_t* active_data = file_data.data();
    size_t active_size = file_data.size();

    if (file_data.size() >= 2 && file_data[0] == 0x1f && file_data[1] == 0x8b) {
        if (gzip_decompress(file_data, decompressed)) {
            active_data = decompressed.data();
            active_size = decompressed.size();
        }
    }

    if (active_size < 12) return "";

    bool is_pvr = false;
    if (active_size >= 52) {
        uint32_t magic_v3 = *(const uint32_t*)active_data;
        uint32_t magic_v2 = *(const uint32_t*)(active_data + 44);
        if (magic_v3 == 0x03525650 || magic_v2 == 0x21525650) {
            is_pvr = true;
        }
    }

    if (is_pvr) {
        int version = 3;
        int w = 0, h = 0;
        int format_type = -1;
        uint32_t gl_format = 0, gl_type = 0;
        int bpp = 0;
        char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        uint8_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;

        const PVRv3Hdr* v3 = (const PVRv3Hdr*)active_data;
        if (v3->version == 0x03525650) {
            version = 3;
            w = v3->width;
            h = v3->height;
            format_type = pvr::ParsePVRv3Format(v3->pixel_format, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        } else {
            const PVRv2Hdr* v2 = (const PVRv2Hdr*)active_data;
            version = 2;
            w = v2->width;
            h = v2->height;
            format_type = pvr::ParsePVRv2Format(v2->flags, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        }

        std::ostringstream ss;
        ss << "PVR;" << version << ";" << format_type << ";" << w << ";" << h << ";"
           << (int)c0 << ";" << (int)c1 << ";" << (int)c2 << ";" << (int)c3 << ";"
           << (int)d0 << ";" << (int)d1 << ";" << (int)d2 << ";" << (int)d3;
        return ss.str();
    } else {
        uint32_t img_type = *(const uint32_t*)(active_data + 0);
        uint32_t w = *(const uint32_t*)(active_data + 4);
        uint32_t h = *(const uint32_t*)(active_data + 8);
        std::ostringstream ss;
        ss << "TEX;" << img_type << ";" << w << ";" << h;
        return ss.str();
    }
}

static std::map<std::string, std::string> load_sdk_metadata(const std::string& src_dir) {
    std::map<std::string, std::string> metadata_map;
    fs::path sdk_path = fs::path(src_dir) / "ruby.sdk";
    std::ifstream infile(sdk_path);
    if (!infile.is_open()) return metadata_map;

    std::string line;
    while (std::getline(infile, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        std::string rel_path = line.substr(0, eq);
        std::string meta = line.substr(eq + 3);
        metadata_map[rel_path] = meta;
    }
    return metadata_map;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5  Utility Helpers
// ═══════════════════════════════════════════════════════════════════════════════

static double wall_ms() {
    using namespace std::chrono;
    return (double)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static std::string fmt_bytes(size_t b) {
    char buf[64];
    if (b < 1024)           snprintf(buf, sizeof(buf), "%zu B", b);
    else if (b < 1024*1024) snprintf(buf, sizeof(buf), "%.1f KB", b/1024.0);
    else                    snprintf(buf, sizeof(buf), "%.2f MB", b/1048576.0);
    return buf;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return sz == 0; }
    out.resize((size_t)sz);
    bool ok = (fread(out.data(), 1, sz, f) == (size_t)sz);
    fclose(f);
    return ok;
}

static bool write_file(const std::string& path, const void* data, size_t size) {
    fs::path p(path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (fwrite(data, 1, size, f) == size);
    fclose(f);
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §6  Export Worker: game assets → PNG (+ ruby.sdk metadata tracking)
// ═══════════════════════════════════════════════════════════════════════════════
static void do_export_file(BatchState& bs,
                            const std::string& src_path,
                            const std::string& dst_path,
                            double job_start_ms)
{
    double t0 = wall_ms();
    FileResult res;
    res.src_path  = src_path;
    res.dst_path  = dst_path;
    std::error_code ec;
    res.src_bytes = fs::file_size(src_path, ec);
    res.src_w = res.src_h = 0;
    res.dst_bytes = 0;
    res.success   = false;

    std::vector<uint8_t> src_data;
    if (!read_file(src_path, src_data)) {
        res.error_msg = "Cannot read source file";
        res.duration_ms = wall_ms() - t0;
        bs.push_log(LogLevel::ERR, "✗ " + fs::path(src_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.results.push_back(res);
        return;
    }

    // Extract format metadata
    std::string meta_str = extract_original_metadata(src_data);
    if (!meta_str.empty()) {
        std::error_code ec2;
        fs::path rel = fs::relative(fs::path(src_path), fs::path(bs.src_dir), ec2);
        std::string rel_str = ec2 ? fs::path(src_path).filename().string() : rel.string();
        std::string key = rel_str + ".png"; // Store original properties keyed by output PNG path
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.metadata_export[key] = meta_str;
    }

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!pvr_decode_to_rgba(src_data.data(), src_data.size(), rgba, w, h)) {
        res.error_msg = "Decode failed (unsupported format or corrupt file)";
        res.duration_ms = wall_ms() - t0;
        bs.push_log(LogLevel::ERR, "✗ " + fs::path(src_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.results.push_back(res);
        return;
    }
    res.src_w = w; res.src_h = h;

    // Symmetrical vertical flip so exported PNG is right-side up
    for (int y = 0; y < h / 2; y++) {
        for (int x = 0; x < w * 4; x++) {
            std::swap(rgba[y * w * 4 + x], rgba[(h - 1 - y) * w * 4 + x]);
        }
    }

    if (!stbi_write_png(dst_path.c_str(), w, h, 4, rgba.data(), w * 4)) {
        res.error_msg = "stbi_write_png failed";
        res.duration_ms = wall_ms() - t0;
        bs.push_log(LogLevel::ERR, "✗ " + fs::path(dst_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.results.push_back(res);
        return;
    }

    res.dst_bytes   = fs::file_size(dst_path, ec);
    res.success     = true;
    res.duration_ms = wall_ms() - t0;

    char info[256];
    snprintf(info, sizeof(info), "  %dx%d  %s → %s  (%.0f ms)", w, h, fmt_bytes(res.src_bytes).c_str(), fmt_bytes(res.dst_bytes).c_str(), res.duration_ms);
    bs.push_log(LogLevel::OK, "✔ " + fs::path(dst_path).filename().string() + info, wall_ms() - job_start_ms);

    std::lock_guard<std::mutex> lk(bs.log_mutex);
    bs.results.push_back(res);
}

// ═══════════════════════════════════════════════════════════════════════════════
// §7  Import Worker: PNG → game asset (native PVR / TEX encoder using ruby.sdk)
// ═══════════════════════════════════════════════════════════════════════════════
static void do_import_file(BatchState& bs,
                            const std::string& src_path,
                            const std::string& dst_path,
                            bool output_texpng,
                            const std::map<std::string, std::string>& metadata_map,
                            double job_start_ms)
{
    double t0 = wall_ms();
    FileResult res;
    res.src_path  = src_path;
    res.dst_path  = dst_path;
    std::error_code size_ec2;
    res.src_bytes = fs::file_size(src_path, size_ec2);
    res.dst_bytes = 0;
    res.success   = false;

    // Resolve original layout info from ruby.sdk
    std::error_code ec_rel;
    fs::path rel = fs::relative(fs::path(src_path), fs::path(bs.src_dir), ec_rel);
    std::string key = ec_rel ? fs::path(src_path).filename().string() : rel.string();

    std::string meta_line;
    bool has_meta = false;
    if (metadata_map.count(key)) {
        meta_line = metadata_map.at(key);
        has_meta = true;
    } else {
        // Fallback: search by filename only to ignore directory prefix mismatches
        std::string filename = fs::path(src_path).filename().string();
        for (auto& entry : metadata_map) {
            if (fs::path(entry.first).filename().string() == filename) {
                meta_line = entry.second;
                has_meta = true;
                break;
            }
        }
    }

    // Decode PNG
    int w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load(src_path.c_str(), &w, &h, &comp, 4);
    if (!rgba) {
        res.error_msg = "PNG decode failed: " + std::string(stbi_failure_reason());
        res.duration_ms = wall_ms() - t0;
        bs.push_log(LogLevel::ERR, "✗ " + fs::path(src_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.results.push_back(res);
        return;
    }
    res.src_w = w; res.src_h = h;

    // Symmetrical vertical flip back to game layout
    for (int y = 0; y < h / 2; y++) {
        for (int x = 0; x < w * 4; x++) {
            std::swap(rgba[y * w * 4 + x], rgba[(h - 1 - y) * w * 4 + x]);
        }
    }

    // Natively perform premultiplied alpha (as expected by game blending)
    for (int i = 0; i < w * h; i++) {
        uint8_t a = rgba[i * 4 + 3];
        if (a == 0) {
            rgba[i * 4 + 0] = 0;
            rgba[i * 4 + 1] = 0;
            rgba[i * 4 + 2] = 0;
        } else if (a != 255) {
            rgba[i * 4 + 0] = (uint8_t)(((uint32_t)rgba[i * 4 + 0] * a) / 255);
            rgba[i * 4 + 1] = (uint8_t)(((uint32_t)rgba[i * 4 + 1] * a) / 255);
            rgba[i * 4 + 2] = (uint8_t)(((uint32_t)rgba[i * 4 + 2] * a) / 255);
        }
    }

    std::vector<uint8_t> pvr_raw;
    bool encode_ok = false;

    // Check configuration
    bool compile_as_tex = output_texpng;
    int tex_img_type = 1; // Default RGBA_8888 for TEX fallback
    bool compile_as_etc = (bs.compress_fmt == CompressFmt::ETC1);
    bool compile_as_pvr_uncompressed = false;
    int pvr_c0 = 'r', pvr_c1 = 'g', pvr_c2 = 'b', pvr_c3 = 'a';
    int pvr_d0 = 8, pvr_d1 = 8, pvr_d2 = 8, pvr_d3 = 8;

    if (has_meta) {
        std::stringstream ss(meta_line);
        std::string container;
        std::getline(ss, container, ';');
        if (container == "TEX") {
            compile_as_tex = true;
            std::string s_type;
            if (std::getline(ss, s_type, ';')) {
                tex_img_type = std::stoi(s_type);
            }
        } else if (container == "PVR") {
            compile_as_tex = false;
            std::string s_ver, s_fmt, s_w, s_h;
            std::getline(ss, s_ver, ';');
            std::getline(ss, s_fmt, ';');
            std::getline(ss, s_w, ';');
            std::getline(ss, s_h, ';');
            int fmt_val = std::stoi(s_fmt);
            compile_as_etc = (fmt_val == 1);
            
            if (fmt_val == 10) { // Uncompressed PVR
                compile_as_pvr_uncompressed = true;
                std::string s_c0, s_c1, s_c2, s_c3, s_d0, s_d1, s_d2, s_d3;
                std::getline(ss, s_c0, ';'); std::getline(ss, s_c1, ';');
                std::getline(ss, s_c2, ';'); std::getline(ss, s_c3, ';');
                std::getline(ss, s_d0, ';'); std::getline(ss, s_d1, ';');
                std::getline(ss, s_d2, ';'); std::getline(ss, s_d3, ';');
                pvr_c0 = std::stoi(s_c0); pvr_c1 = std::stoi(s_c1);
                pvr_c2 = std::stoi(s_c2); pvr_c3 = std::stoi(s_c3);
                pvr_d0 = std::stoi(s_d0); pvr_d1 = std::stoi(s_d1);
                pvr_d2 = std::stoi(s_d2); pvr_d3 = std::stoi(s_d3);
            }
        }
    }

    if (compile_as_tex) {
        // ── Native TEX formats packing (1 to 8) ─────────────────────────────
        uint32_t header[3] = { (uint32_t)tex_img_type, (uint32_t)w, (uint32_t)h };
        
        if (tex_img_type == 1) { // RGBA_8888
            size_t px_bytes = (size_t)w * h * 4;
            pvr_raw.resize(12 + px_bytes);
            memcpy(pvr_raw.data(), header, 12);
            memcpy(pvr_raw.data() + 12, rgba, px_bytes);
            encode_ok = true;
        } 
        else if (tex_img_type == 2) { // RGBA_4444 (2 bytes/px, packed r4g4b4a4)
            pvr_raw.resize(12 + w * h * 2);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                uint8_t r = rgba[i * 4 + 0] >> 4;
                uint8_t g = rgba[i * 4 + 1] >> 4;
                uint8_t b = rgba[i * 4 + 2] >> 4;
                uint8_t a = rgba[i * 4 + 3] >> 4;
                uint16_t val = (r << 12) | (g << 8) | (b << 4) | a;
                dst[i * 2 + 0] = val & 0xFF;
                dst[i * 2 + 1] = val >> 8;
            }
            encode_ok = true;
        }
        else if (tex_img_type == 3) { // RGBA_5551 (2 bytes/px, packed r5g5b5a1)
            pvr_raw.resize(12 + w * h * 2);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                uint8_t r = rgba[i * 4 + 0] >> 3;
                uint8_t g = rgba[i * 4 + 1] >> 3;
                uint8_t b = rgba[i * 4 + 2] >> 3;
                uint8_t a = rgba[i * 4 + 3] >> 7;
                uint16_t val = (r << 11) | (g << 6) | (b << 1) | a;
                dst[i * 2 + 0] = val & 0xFF;
                dst[i * 2 + 1] = val >> 8;
            }
            encode_ok = true;
        }
        else if (tex_img_type == 4) { // RGB_888 (3 bytes/px)
            pvr_raw.resize(12 + w * h * 3);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                dst[i * 3 + 0] = rgba[i * 4 + 0];
                dst[i * 3 + 1] = rgba[i * 4 + 1];
                dst[i * 3 + 2] = rgba[i * 4 + 2];
            }
            encode_ok = true;
        }
        else if (tex_img_type == 5) { // RGB_565 (2 bytes/px, packed r5g6b5)
            pvr_raw.resize(12 + w * h * 2);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                uint8_t r = rgba[i * 4 + 0] >> 3;
                uint8_t g = rgba[i * 4 + 1] >> 2;
                uint8_t b = rgba[i * 4 + 2] >> 3;
                uint16_t val = (r << 11) | (g << 5) | b;
                dst[i * 2 + 0] = val & 0xFF;
                dst[i * 2 + 1] = val >> 8;
            }
            encode_ok = true;
        }
        else if (tex_img_type == 6) { // LUMINANCE_8 (1 byte/px, L)
            pvr_raw.resize(12 + w * h);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                dst[i] = rgba[i * 4 + 0];
            }
            encode_ok = true;
        }
        else if (tex_img_type == 7) { // ALPHA_8 (1 byte/px, A)
            pvr_raw.resize(12 + w * h);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                dst[i] = rgba[i * 4 + 3];
            }
            encode_ok = true;
        }
        else if (tex_img_type == 8) { // LUMINANCE_ALPHA_88 (2 bytes/px, LA)
            pvr_raw.resize(12 + w * h * 2);
            memcpy(pvr_raw.data(), header, 12);
            uint8_t* dst = pvr_raw.data() + 12;
            for (int i = 0; i < w * h; i++) {
                dst[i * 2 + 0] = rgba[i * 4 + 0];
                dst[i * 2 + 1] = rgba[i * 4 + 3];
            }
            encode_ok = true;
        }
    } 
    else if (compile_as_pvr_uncompressed) {
        // ── PVR uncompressed reconstruct mappings ──────────────────────────
        uint64_t pixel_format = ((uint64_t)pvr_d3 << 56) | ((uint64_t)pvr_d2 << 48) |
                                ((uint64_t)pvr_d1 << 40) | ((uint64_t)pvr_d0 << 32) |
                                ((uint64_t)pvr_c3 << 24) | ((uint64_t)pvr_c2 << 16) |
                                ((uint64_t)pvr_c1 << 8)  | (uint64_t)pvr_c0;

        PVRv3Hdr hdr;
        hdr.pixel_format = pixel_format;
        hdr.width        = (uint32_t)w;
        hdr.height       = (uint32_t)h;

        size_t bpp = (pvr_d0 + pvr_d1 + pvr_d2 + pvr_d3 + 7) / 8;
        pvr_raw.resize(sizeof(PVRv3Hdr) + w * h * bpp);
        memcpy(pvr_raw.data(), &hdr, sizeof(PVRv3Hdr));
        uint8_t* dst = pvr_raw.data() + sizeof(PVRv3Hdr);

        if (pvr_c0 == 'r' && pvr_c1 == 'g' && pvr_c2 == 'b' && pvr_c3 == 'a' && pvr_d0 == 8) {
            // RGBA8888
            memcpy(dst, rgba, w * h * 4);
        }
        else if (pvr_c0 == 'b' && pvr_c1 == 'g' && pvr_c2 == 'r' && pvr_c3 == 'a' && pvr_d0 == 8) {
            // BGRA8888
            for (int i = 0; i < w * h; i++) {
                dst[i * 4 + 0] = rgba[i * 4 + 2]; // B
                dst[i * 4 + 1] = rgba[i * 4 + 1]; // G
                dst[i * 4 + 2] = rgba[i * 4 + 0]; // R
                dst[i * 4 + 3] = rgba[i * 4 + 3]; // A
            }
        }
        else if (pvr_c0 == 'l' && pvr_c1 == 'a' && pvr_d0 == 8) {
            // LA88
            for (int i = 0; i < w * h; i++) {
                dst[i * 2 + 0] = rgba[i * 4 + 0]; // L
                dst[i * 2 + 1] = rgba[i * 4 + 3]; // A
            }
        }
        else if (pvr_c0 == 'l' && pvr_d0 == 8) {
            // L8
            for (int i = 0; i < w * h; i++) {
                dst[i] = rgba[i * 4 + 0];
            }
        }
        else if (pvr_c0 == 'a' && pvr_d0 == 8) {
            // A8
            for (int i = 0; i < w * h; i++) {
                dst[i] = rgba[i * 4 + 3];
            }
        }
        else if (pvr_c0 == 'r' && pvr_c1 == 'g' && pvr_c2 == 'b' && pvr_d0 == 5 && pvr_d1 == 6) {
            // RGB565
            for (int i = 0; i < w * h; i++) {
                uint8_t r = rgba[i * 4 + 0] >> 3;
                uint8_t g = rgba[i * 4 + 1] >> 2;
                uint8_t b = rgba[i * 4 + 2] >> 3;
                uint16_t val = (r << 11) | (g << 5) | b;
                dst[i * 2 + 0] = val & 0xFF;
                dst[i * 2 + 1] = val >> 8;
            }
        }
        else if (pvr_c0 == 'r' && pvr_c1 == 'g' && pvr_c2 == 'b' && pvr_c3 == 'a' && pvr_d0 == 4) {
            // RGBA4444
            for (int i = 0; i < w * h; i++) {
                uint8_t r = rgba[i * 4 + 0] >> 4;
                uint8_t g = rgba[i * 4 + 1] >> 4;
                uint8_t b = rgba[i * 4 + 2] >> 4;
                uint8_t a = rgba[i * 4 + 3] >> 4;
                uint16_t val = (r << 12) | (g << 8) | (b << 4) | a;
                dst[i * 2 + 0] = val & 0xFF;
                dst[i * 2 + 1] = val >> 8;
            }
        }
        else if (pvr_c0 == 'r' && pvr_c1 == 'g' && pvr_c2 == 'b' && pvr_c3 == 'a' && pvr_d0 == 5 && pvr_d3 == 1) {
            // RGBA5551
            for (int i = 0; i < w * h; i++) {
                uint8_t r = rgba[i * 4 + 0] >> 3;
                uint8_t g = rgba[i * 4 + 1] >> 3;
                uint8_t b = rgba[i * 4 + 2] >> 3;
                uint8_t a = rgba[i * 4 + 3] >> 7;
                uint16_t val = (r << 11) | (g << 6) | (b << 1) | a;
                dst[i * 2 + 0] = val & 0xFF;
                dst[i * 2 + 1] = val >> 8;
            }
        }
        encode_ok = true;
    }
    else {
        // ── PVR compressed (ETC1) or RGBA8888 fallback ──────────────────────
        if (compile_as_etc) {
            int bw = (w + 3) / 4;
            int bh = (h + 3) / 4;
            size_t etc1_bytes = (size_t)bw * bh * 8;

            PVRv3Hdr hdr;
            hdr.pixel_format = PVR3_FMT_ETC1;
            hdr.width        = (uint32_t)w;
            hdr.height       = (uint32_t)h;

            pvr_raw.resize(sizeof(PVRv3Hdr) + etc1_bytes, 0);
            memcpy(pvr_raw.data(), &hdr, sizeof(PVRv3Hdr));
            uint8_t* out_blocks = pvr_raw.data() + sizeof(PVRv3Hdr);

            int pw = bw * 4, ph = bh * 4;
            std::vector<uint8_t> padded((size_t)pw * ph * 4, 0);
            for (int y = 0; y < h; y++)
                memcpy(padded.data() + y * pw * 4, rgba + y * w * 4, (size_t)w * 4);
            for (int y = 0; y < h; y++)
                for (int x = w; x < pw; x++)
                    memcpy(padded.data() + (y * pw + x) * 4, padded.data() + (y * pw + (w-1)) * 4, 4);
            for (int y = h; y < ph; y++)
                memcpy(padded.data() + y * pw * 4, padded.data() + (h-1) * pw * 4, (size_t)pw * 4);

            for (int by = 0; by < bh && !bs.cancel_flag.load(); by++) {
                for (int bx = 0; bx < bw; bx++) {
                    const uint8_t* block_src = padded.data() + (by * 4) * pw * 4 + (bx * 4) * 4;
                    uint8_t* block_dst = out_blocks + (by * bw + bx) * 8;
                    encode_etc1_block(block_src, pw * 4, block_dst);
                }
            }
            encode_ok = !bs.cancel_flag.load();
        } else {
            PVRv3Hdr hdr;
            hdr.pixel_format = PVR3_FMT_RGBA8888;
            hdr.width        = (uint32_t)w;
            hdr.height       = (uint32_t)h;

            size_t px_bytes = (size_t)w * h * 4;
            pvr_raw.resize(sizeof(PVRv3Hdr) + px_bytes);
            memcpy(pvr_raw.data(), &hdr, sizeof(PVRv3Hdr));
            memcpy(pvr_raw.data() + sizeof(PVRv3Hdr), rgba, px_bytes);
            encode_ok = true;
        }
    }

    stbi_image_free(rgba);

    if (!encode_ok) {
        res.error_msg = "Encoding cancelled or failed";
        res.duration_ms = wall_ms() - t0;
        bs.push_log(LogLevel::ERR, "✗ " + fs::path(dst_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.results.push_back(res);
        return;
    }

    std::vector<uint8_t> final_bytes;
    if (output_texpng) {
        if (!gzip_compress(pvr_raw, final_bytes)) {
            res.error_msg = "gzip compression failed";
            res.duration_ms = wall_ms() - t0;
            bs.push_log(LogLevel::ERR, "✗ " + fs::path(dst_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
            std::lock_guard<std::mutex> lk(bs.log_mutex);
            bs.results.push_back(res);
            return;
        }
    } else {
        final_bytes = std::move(pvr_raw);
    }

    if (!write_file(dst_path, final_bytes.data(), final_bytes.size())) {
        res.error_msg = "Cannot write output file";
        res.duration_ms = wall_ms() - t0;
        bs.push_log(LogLevel::ERR, "✗ " + fs::path(dst_path).filename().string() + " — " + res.error_msg, wall_ms() - job_start_ms);
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.results.push_back(res);
        return;
    }

    res.dst_bytes   = final_bytes.size();
    res.success     = true;
    res.duration_ms = wall_ms() - t0;

    char info[256];
    snprintf(info, sizeof(info), "  %dx%d  %s → %s  (%.0f ms)", w, h, fmt_bytes(res.src_bytes).c_str(), fmt_bytes(res.dst_bytes).c_str(), res.duration_ms);
    bs.push_log(LogLevel::OK, "✔ " + fs::path(dst_path).filename().string() + info, wall_ms() - job_start_ms);

    std::lock_guard<std::mutex> lk(bs.log_mutex);
    bs.results.push_back(res);
}

// ═══════════════════════════════════════════════════════════════════════════════
// §8  Collect files to process
// ═══════════════════════════════════════════════════════════════════════════════
struct ConvTask {
    std::string src_path;
    std::string dst_path;
    bool        output_texpng;
};

static std::vector<ConvTask> collect_tasks(const BatchState& bs) {
    std::vector<ConvTask> tasks;
    std::error_code ec;

    auto add_entry = [&](const fs::path& p) {
        std::string name = p.filename().string();
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);

        std::string dst_name;
        bool out_texpng = false;

        if (bs.mode == Mode::EXPORT_TO_PNG) {
            if (bs.filter_pvr && lower.size() > 4 && lower.substr(lower.size()-4) == ".pvr" && lower.find(".tex.png") == std::string::npos) {
                dst_name = name + ".png";
            } else if (bs.filter_texpng && lower.find(".tex.png") != std::string::npos) {
                dst_name = name + ".png";
            } else {
                return;
            }
        } else {
            if (bs.filter_pvrpng && lower.size() > 8 && lower.substr(lower.size()-8) == ".pvr.png") {
                dst_name = name.substr(0, name.size() - 4);
                out_texpng = false;
            } else if (bs.filter_texppng && lower.size() > 12 && lower.substr(lower.size()-12) == ".tex.png.png") {
                dst_name = name.substr(0, name.size() - 4);
                out_texpng = true;
            } else {
                return;
            }
        }

        fs::path rel;
        std::error_code ec2;
        rel = fs::relative(p.parent_path(), fs::path(bs.src_dir), ec2);
        if (ec2) rel = "";

        fs::path dst = fs::path(bs.dst_dir) / rel / dst_name;
        tasks.push_back({p.string(), dst.string(), out_texpng});
    };

    if (bs.recurse_subdirs) {
        for (auto& entry : fs::recursive_directory_iterator(fs::path(bs.src_dir), fs::directory_options::skip_permission_denied, ec))
            if (entry.is_regular_file(ec)) add_entry(entry.path());
    } else {
        for (auto& entry : fs::directory_iterator(fs::path(bs.src_dir), fs::directory_options::skip_permission_denied, ec))
            if (entry.is_regular_file(ec)) add_entry(entry.path());
    }

    return tasks;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §9  Worker Thread Entry Point
// ═══════════════════════════════════════════════════════════════════════════════
static void worker_thread(BatchState* bsp, double job_start_ms) {
    BatchState& bs = *bsp;

    bs.push_log(LogLevel::INFO, std::string("⊙ Scanning '") + bs.src_dir + "'...", 0.0);

    std::vector<ConvTask> tasks = collect_tasks(bs);
    bs.total_files.store((int)tasks.size());

    if (tasks.empty()) {
        bs.push_log(LogLevel::WARN, "⚠ No matching files found in source directory.", 0.5);
        bs.finished.store(true);
        bs.running.store(false);
        return;
    }

    {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "⊙ Found %d file(s) to convert — output → '%s'", (int)tasks.size(), bs.dst_dir);
        bs.push_log(LogLevel::INFO, hdr, 1.0);
        bs.push_log(LogLevel::INFO, std::string(60, '-'), 1.0);
    }

    // Load original layout properties if importing
    std::map<std::string, std::string> metadata_map;
    if (bs.mode == Mode::IMPORT_TO_GAME) {
        metadata_map = load_sdk_metadata(bs.src_dir);
        if (!metadata_map.empty()) {
            char info_sdk[256];
            snprintf(info_sdk, sizeof(info_sdk), "⊙ Loaded %zu layout mapping rules from ruby.sdk", metadata_map.size());
            bs.push_log(LogLevel::OK, info_sdk, wall_ms() - job_start_ms);
        }
    }

    int idx = 0;
    for (auto& task : tasks) {
        if (bs.cancel_flag.load()) {
            bs.push_log(LogLevel::WARN, "⚠ Cancelled by user.", wall_ms() - job_start_ms);
            break;
        }

        {
            std::lock_guard<std::mutex> lk(bs.log_mutex);
            bs.current_file = fs::path(task.src_path).filename().string();
        }

        char progress_msg[512];
        snprintf(progress_msg, sizeof(progress_msg), "[%d/%d]  %s", idx + 1, (int)tasks.size(), fs::path(task.src_path).filename().string().c_str());
        bs.push_log(LogLevel::INFO, progress_msg, wall_ms() - job_start_ms);

        if (bs.skip_existing && fs::exists(task.dst_path)) {
            bs.push_log(LogLevel::WARN, "  → Skipped (already exists): " + fs::path(task.dst_path).filename().string(), wall_ms() - job_start_ms);
            bs.done_files.fetch_add(1);
            idx++;
            continue;
        }

        if (bs.mode == Mode::EXPORT_TO_PNG) {
            do_export_file(bs, task.src_path, task.dst_path, job_start_ms);
        } else {
            do_import_file(bs, task.src_path, task.dst_path, task.output_texpng, metadata_map, job_start_ms);
        }

        bs.done_files.fetch_add(1);

        {
            std::lock_guard<std::mutex> lk(bs.log_mutex);
            if (!bs.results.empty()) {
                if (bs.results.back().success)
                    bs.ok_count.fetch_add(1);
                else
                    bs.err_count.fetch_add(1);
            }
        }

        idx++;
    }

    // Write ruby.sdk file at target location if we exported assets successfully
    if (bs.mode == Mode::EXPORT_TO_PNG && !bs.metadata_export.empty()) {
        fs::path sdk_out = fs::path(bs.dst_dir) / "ruby.sdk";
        FILE* f_sdk = fopen(sdk_out.string().c_str(), "w");
        if (f_sdk) {
            fprintf(f_sdk, "# Ruby SDK Asset Metadata File\n");
            fprintf(f_sdk, "# Mirroring file properties for precise reconstruction.\n\n");
            for (auto& entry : bs.metadata_export) {
                fprintf(f_sdk, "%s = %s\n", entry.first.c_str(), entry.second.c_str());
            }
            fclose(f_sdk);
            bs.push_log(LogLevel::OK, "✔ Saved metadata index 'ruby.sdk' in output directory.", wall_ms() - job_start_ms);
        }
    }

    double elapsed = wall_ms() - job_start_ms;
    char summary[256];
    snprintf(summary, sizeof(summary), "═══ Done ═══   ✔ %d  ✗ %d  (%.1f s total)", bs.ok_count.load(), bs.err_count.load(), elapsed / 1000.0);
    bs.push_log(bs.err_count.load() > 0 ? LogLevel::WARN : LogLevel::OK, summary, elapsed);

    bs.job_end_wall = elapsed;
    bs.finished.store(true);
    bs.running.store(false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// §10  Public API
// ═══════════════════════════════════════════════════════════════════════════════
void start_batch_job(BatchState& bs, double now_sec) {
    if (bs.running.load()) return;
    if (bs.src_dir[0] == '\0' || bs.dst_dir[0] == '\0') return;

    {
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.log.clear();
        bs.results.clear();
        bs.current_file.clear();
        bs.metadata_export.clear();
    }
    bs.total_files.store(0);
    bs.done_files.store(0);
    bs.ok_count.store(0);
    bs.err_count.store(0);
    bs.cancel_flag.store(false);
    bs.finished.store(false);
    bs.job_start_wall = now_sec;
    bs.job_end_wall   = 0.0;

    bs.running.store(true);
    if (bs.worker.joinable()) bs.worker.join();
    bs.worker = std::thread(worker_thread, &bs, wall_ms());
}

void cancel_batch_job(BatchState& bs) {
    bs.cancel_flag.store(true);
}

void shutdown_batch(BatchState& bs) {
    bs.cancel_flag.store(true);
    if (bs.worker.joinable()) bs.worker.join();
}

// ═══════════════════════════════════════════════════════════════════════════════
// §11  ImGui UI — Professional Batch Converter window
// ═══════════════════════════════════════════════════════════════════════════════

static void draw_dir_row(const char* label, char* buf, size_t bufsz, const char* hint_id) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(140.0f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10.0f);
    ImGui::InputText(hint_id, buf, bufsz);
}

static ImVec4 log_color(LogLevel lv) {
    switch (lv) {
        case LogLevel::OK:   return ImVec4(0.35f, 0.95f, 0.45f, 1.0f);
        case LogLevel::WARN: return ImVec4(1.00f, 0.80f, 0.20f, 1.0f);
        case LogLevel::ERR:  return ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
        default:             return ImVec4(0.75f, 0.80f, 0.90f, 1.0f);
    }
}

void draw_batch_converter(BatchState& bs, double now_sec) {
    ImGui::SetNextWindowSize(ImVec2(820, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    bool p_open = true;
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::BeginPopupModal("  ⚙  Batch Converter — Ruby", &p_open, wflags)) {
        return;
    }
    if (!p_open) {
        ImGui::CloseCurrentPopup();
        bs.open_window = false;
        ImGui::EndPopup();
        return;
    }

    bool running = bs.running.load();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
    ImGui::TextUnformatted("Ruby Batch Texture Converter  |  100% Native — no external tools");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    if (running) ImGui::BeginDisabled();

    ImGui::TextUnformatted("Mode:");
    ImGui::SameLine(140.0f);
    int mode_i = (int)bs.mode;
    ImGui::RadioButton("Export  (Game → PNG)", &mode_i, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Import  (PNG → Game)", &mode_i, 1);
    bs.mode = (Mode)mode_i;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    draw_dir_row("Source Directory", bs.src_dir, sizeof(bs.src_dir), "##src");
    ImGui::Spacing();
    draw_dir_row("Output Directory", bs.dst_dir, sizeof(bs.dst_dir), "##dst");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (bs.mode == Mode::EXPORT_TO_PNG) {
        ImGui::TextUnformatted("Convert:");
        ImGui::SameLine(140.0f);
        ImGui::Checkbox(".pvr##exp",     &bs.filter_pvr);
        ImGui::SameLine();
        ImGui::Checkbox(".tex.png##exp", &bs.filter_texpng);
    } else {
        ImGui::TextUnformatted("Convert:");
        ImGui::SameLine(140.0f);
        ImGui::Checkbox(".pvr.png  → .pvr##imp",         &bs.filter_pvrpng);
        ImGui::SameLine();
        ImGui::Checkbox(".tex.png.png  → .tex.png##imp", &bs.filter_texppng);
    }

    ImGui::Spacing();
    ImGui::Checkbox("Recurse sub-directories##rec", &bs.recurse_subdirs);
    ImGui::SameLine(240.0f);
    ImGui::Checkbox("Skip already existing output files##skip", &bs.skip_existing);

    if (bs.mode == Mode::IMPORT_TO_GAME) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Advanced Fallback Encode Options")) {
            ImGui::TextUnformatted("Compression:");
            ImGui::SameLine(140.0f);
            int cf = (int)bs.compress_fmt;
            ImGui::RadioButton("ETC1 (Mobile/OpenGL ES)", &cf, 0);
            ImGui::SameLine();
            ImGui::RadioButton("PVRTC 4bpp", &cf, 1);
            ImGui::SameLine();
            ImGui::RadioButton("RGBA8888 (Uncompressed)", &cf, 3);
            bs.compress_fmt = (CompressFmt)cf;

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.7f,0.7f,1.0f));
            ImGui::TextWrapped("NOTE: Original layout details (like RGBA4444, RGB565, etc.) stored in 'ruby.sdk' take precedence over these fallback choices.");
            ImGui::PopStyleColor();
        }
    }

    if (running) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 160.0f;
    if (!running) {
        bool src_valid = (bs.src_dir[0] != '\0') && fs::is_directory(bs.src_dir);
        bool dst_valid = (bs.dst_dir[0] != '\0');

        if (!src_valid || !dst_valid) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.50f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.40f, 0.18f, 1.0f));
        if (ImGui::Button("  ▶  Start Conversion", ImVec2(btn_w * 1.3f, 0))) {
            start_batch_job(bs, now_sec);
        }
        ImGui::PopStyleColor(3);
        if (!src_valid || !dst_valid) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            if (!src_valid)
                ImGui::TextUnformatted("⚠ Source directory is invalid or doesn't exist.");
            else
                ImGui::TextUnformatted("⚠ Please specify an output directory.");
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.15f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.20f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.10f, 0.08f, 1.0f));
        if (ImGui::Button("  ■  Cancel", ImVec2(btn_w, 0)))
            cancel_batch_job(bs);
        ImGui::PopStyleColor(3);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    int total = bs.total_files.load();
    int done  = bs.done_files.load();

    if (running || bs.finished.load()) {
        if (running) {
            float spin = fmodf((float)now_sec * 8.0f, 8.0f);
            const char* spinners[] = {"◐","◓","◑","◒","◐","◓","◑","◒"};
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::Text("%s  Processing...", spinners[(int)spin]);
            ImGui::PopStyleColor();
        } else {
            int errs = bs.err_count.load();
            ImGui::PushStyleColor(ImGuiCol_Text, errs > 0 ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.35f, 0.95f, 0.45f, 1.0f));
            ImGui::Text("  ✔  Finished — %d ok, %d error(s)  (%.1f s)", bs.ok_count.load(), errs, bs.job_end_wall / 1000.0);
            ImGui::PopStyleColor();
        }

        float frac = (total > 0) ? (float)done / (float)total : 0.0f;
        char ovr[64]; snprintf(ovr, sizeof(ovr), "%d / %d files", done, total);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.18f, 0.65f, 0.35f, 1.0f));
        ImGui::ProgressBar(frac, ImVec2(-1, 18), ovr);
        ImGui::PopStyleColor();

        if (running) {
            std::string cf;
            { std::lock_guard<std::mutex> lk(bs.log_mutex); cf = bs.current_file; }
            if (!cf.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.50f, 1.0f));
                ImGui::Text("  ▶  %s", cf.c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::Text("  ✔ %d ok    ✗ %d errors    Elapsed: %.1f s", bs.ok_count.load(), bs.err_count.load(), running ? (now_sec - bs.job_start_wall) : bs.job_end_wall / 1000.0);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::TextUnformatted("Conversion Log:");
    ImGui::Separator();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140.0f);
    if (ImGui::SmallButton("Copy Log")) {
        std::string all;
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        for (auto& e : bs.log) all += e.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Log")) {
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        bs.log.clear();
    }

    float log_h = ImGui::GetContentRegionAvail().y - 8.0f;
    if (log_h < 80.0f) log_h = 80.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
    ImGui::BeginChild("##BatchLog", ImVec2(0, log_h), ImGuiChildFlags_Borders);

    {
        std::lock_guard<std::mutex> lk(bs.log_mutex);
        for (auto& entry : bs.log) {
            if (entry.elapsed_ms >= 0.0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.40f, 0.50f, 1.0f));
                ImGui::Text("[%6.1fs]", entry.elapsed_ms / 1000.0);
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, log_color(entry.level));
            ImGui::TextUnformatted(entry.text.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (running || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg

    ImGui::EndPopup();
}

} // namespace batch
