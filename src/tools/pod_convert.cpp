#include "tools/pod_convert.h"

#include <zlib.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <cmath>

#include "tools/pod_loader.h"
#include "tools/fbx_import.h"
#include "tools/gltf_glb.h"
#include "tools/pod_writer.h"
#include "stb/stb_image.h"

namespace fs = std::filesystem;

namespace av {

// ─── Game-compatible PVR header ─────────────────────────────────────────────
// The game's runtime texture uploader (PVRTTextureLoadFromPointer in
// libswordigo) ONLY accepts the legacy 52-byte PVR v2-style header: it keys
// off the first u32 == 52 and rejects any other container (including the
// "PVR\3" PVR v3 header) with "failed: not a valid pvr". The real game .pvr
// assets use exactly this layout, so converter output must mirror it:
//
//   +0   u32 header_size   = 52
//   +4   u32 height
//   +8   u32 width
//   +12  u32 mip_count     = 1
//   +16  u32 flags         = 0x36 (ETC1_RGB8_OES) — low byte is the format
//   +20  u32 data_size     = compressed bytes per surface (used as a stride)
//   +24  u32 bits_per_px   = 4  (bits/pixel, ETC1 / size estimation)
//   +28..u32 masks[4]      = 0
//   +44  u32 magic         = 0x21525650 ("PVR!")
//   +48  u32 num_surfaces  = 1
//   +52  ... ETC1/RGBA pixel data
#pragma pack(push, 1)
struct GamePvrHdr {
    uint32_t header_size = 52;
    uint32_t height      = 0;
    uint32_t width       = 0;
    // Stock game .pvr textures use mip_count = 0 (not 1) — match exactly so the
    // game's PVRTTextureLoadFromPointer sees the same header shape as shipped assets.
    uint32_t mip_count   = 0;
    // Stock header has flags = 0x10036: low byte 0x36 = ETC1_RGB8_OES, plus the
    // 0x10000 "twiddle"/mipmap bit the SDK sets. Reproduce it verbatim.
    uint32_t flags       = 0x10036;
    uint32_t data_size   = 0;
    uint32_t bpp_bits    = 4;
    // Stock masks are 0xFFFFFFFF, not 0. Match the shipped header byte-for-byte.
    uint32_t mask[4]     = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
    uint32_t magic       = 0x21525650; // "PVR!"
    uint32_t num_surfaces = 1;
};
#pragma pack(pop)

static const uint32_t kGamePvrFlagsETC1 = 0x36;

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

static bool write_file(const std::string& path, const void* data, size_t size) {
    fs::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (fwrite(data, 1, size, f) == size);
    fclose(f);
    return ok;
}

// ─── Native ETC1 block encoder (mirrors batch_converter.cpp §3 so pod and
//      batch conversions produce identical tex data) ───────────────────────
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

static uint64_t score_subblock(const uint8_t* pixels[8], int br, int bg, int bb,
                               int table_idx, uint8_t selectors[8]) {
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

// High-quality bilinear image resizer for generating HD / custom-resolution textures.
static std::vector<uint8_t> resize_rgba(const uint8_t* src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) {
        return std::vector<uint8_t>(src, src + (size_t)sw * sh * 4);
    }
    std::vector<uint8_t> dst((size_t)dw * dh * 4);
    float x_ratio = (float)sw / (float)dw;
    float y_ratio = (float)sh / (float)dh;

    for (int dy = 0; dy < dh; dy++) {
        float sy = (dy + 0.5f) * y_ratio - 0.5f;
        int y0 = std::clamp((int)std::floor(sy), 0, sh - 1);
        int y1 = std::clamp(y0 + 1, 0, sh - 1);
        float fy = sy - std::floor(sy);
        if (fy < 0.0f) fy = 0.0f;

        for (int dx = 0; dx < dw; dx++) {
            float sx = (dx + 0.5f) * x_ratio - 0.5f;
            int x0 = std::clamp((int)std::floor(sx), 0, sw - 1);
            int x1 = std::clamp(x0 + 1, 0, sw - 1);
            float fx = sx - std::floor(sx);
            if (fx < 0.0f) fx = 0.0f;

            const uint8_t* p00 = src + ((size_t)y0 * sw + x0) * 4;
            const uint8_t* p10 = src + ((size_t)y0 * sw + x1) * 4;
            const uint8_t* p01 = src + ((size_t)y1 * sw + x0) * 4;
            const uint8_t* p11 = src + ((size_t)y1 * sw + x1) * 4;

            uint8_t* out = dst.data() + ((size_t)dy * dw + dx) * 4;
            for (int c = 0; c < 4; c++) {
                float top = (float)p00[c] * (1.0f - fx) + (float)p10[c] * fx;
                float bot = (float)p01[c] * (1.0f - fx) + (float)p11[c] * fx;
                float val = top * (1.0f - fy) + bot * fy;
                out[c] = (uint8_t)std::clamp(std::round(val), 0.0f, 255.0f);
            }
        }
    }
    return dst;
}

// Encode a top-first authored RGBA image into the game's texture containers.
// The vertical flip + premultiplied alpha mirror batch_converter.cpp's import
// path and give every uploaded texture the game's bottom-origin (v = 0 bottom)
// layout.
//
// Alpha handling: ETC1 is RGB-only, so any texture carrying a non-opaque alpha
// channel is encoded as uncompressed RGBA8888 (PVR flags 0x12) instead — matching
// batch_converter.cpp's format selection. Opaque textures use ETC1 (0x10036).
//
//  - out_pvr=true:  legacy 52-byte PVR header + ETC1/RGBA data (.pvr) — the exact
//                   layout the game's PVRTTextureLoadFromPointer parses
//                   (header_size==52, flags=0x10036 ETC1 or 0x12 RGBA8888, "PVR!" magic).
//                   ETC1 uploaded through glCompressedTexImage2D; RGBA through glTexImage2D.
//  - out_pvr=false: bytes for a ".tex.png" — the gzip of the native TEX
//                   container {img_type(1=RGBA8888), w, h} + RGBA payload,
//                   which is what the game/viewer background loaders expect.
static bool encode_texture(const uint8_t* rgba, int w, int h, bool out_pvr, int target_res,
                           std::vector<uint8_t>& tex, std::string* err) {
    std::vector<uint8_t> resized_buf;
    if (target_res > 0 && (w != target_res || h != target_res)) {
        int tw = target_res;
        int th = (w > 0) ? (int)std::round((float)target_res * (float)h / (float)w) : target_res;
        if (th <= 0) th = target_res;
        tw = ((tw + 3) / 4) * 4;
        th = ((th + 3) / 4) * 4;
        resized_buf = resize_rgba(rgba, w, h, tw, th);
        rgba = resized_buf.data();
        w = tw;
        h = th;
    }

    // Detect whether the source carries a meaningful alpha channel. ETC1 cannot
    // store alpha, so we must fall back to uncompressed RGBA for any texture that
    // isn't fully opaque (characters, particles, decals, ...).
    bool has_alpha = false;
    for (int i = 0; i < w * h; i++) {
        uint8_t a = rgba[(size_t)i * 4 + 3];
        if (a != 255) { has_alpha = true; break; }
    }

    std::vector<uint8_t> flipped((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(&flipped[(size_t)y * w * 4], &rgba[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);

    // Only premultiply for the ETC1 (opaque) path. RGBA8888 keeps straight alpha.
    if (!has_alpha) {
        for (int i = 0; i < w * h; i++) {
            uint8_t a = flipped[i * 4 + 3];
            if (a == 0) {
                flipped[i * 4 + 0] = 0;
                flipped[i * 4 + 1] = 0;
                flipped[i * 4 + 2] = 0;
            } else if (a != 255) {
                flipped[i * 4 + 0] = (uint8_t)(((uint32_t)flipped[i * 4 + 0] * a) / 255);
                flipped[i * 4 + 1] = (uint8_t)(((uint32_t)flipped[i * 4 + 1] * a) / 255);
                flipped[i * 4 + 2] = (uint8_t)(((uint32_t)flipped[i * 4 + 2] * a) / 255);
            }
        }
    }

    if (out_pvr) {
        if (has_alpha) {
            // Uncompressed RGBA8888 for alpha-carrying textures. Header flags
            // 0x12 = RGBA8888 in the game's legacy PVR loader (batch_converter.cpp).
            GamePvrHdr hdr;
            hdr.height    = (uint32_t)h;
            hdr.width     = (uint32_t)w;
            hdr.flags     = 0x12;          // RGBA8888
            hdr.bpp_bits  = 32;
            hdr.data_size = (uint32_t)((size_t)w * h * 4);
            std::vector<uint8_t> pvr(sizeof(GamePvrHdr) + (size_t)w * h * 4, 0);
            memcpy(pvr.data(), &hdr, sizeof(GamePvrHdr));
            memcpy(pvr.data() + sizeof(GamePvrHdr), flipped.data(), (size_t)w * h * 4);
            tex = std::move(pvr);
            return true;
        }

        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        size_t etc1_bytes = (size_t)bw * bh * 8;
        GamePvrHdr hdr;
        hdr.height    = (uint32_t)h;
        hdr.width     = (uint32_t)w;
        // flags/mip_count/mask already default to stock ETC1 values (0x10036 / 0 / FFFFFFFF).
        // Only data_size is per-texture.
        hdr.data_size = (uint32_t)etc1_bytes;
        std::vector<uint8_t> pvr(sizeof(GamePvrHdr) + etc1_bytes, 0);
        memcpy(pvr.data(), &hdr, sizeof(GamePvrHdr));

        int pw = bw * 4, ph = bh * 4;
        std::vector<uint8_t> padded((size_t)pw * ph * 4, 0);
        for (int y = 0; y < h; y++)
            memcpy(padded.data() + (size_t)y * pw * 4, flipped.data() + (size_t)y * w * 4, (size_t)w * 4);
        for (int y = 0; y < h; y++)
            for (int x = w; x < pw; x++)
                memcpy(padded.data() + ((size_t)y * pw + x) * 4, padded.data() + ((size_t)y * pw + (w - 1)) * 4, 4);
        for (int y = h; y < ph; y++)
            memcpy(padded.data() + (size_t)y * pw * 4, padded.data() + (size_t)(h - 1) * pw * 4, (size_t)pw * 4);

        uint8_t* out_blocks = pvr.data() + sizeof(GamePvrHdr);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const uint8_t* src = padded.data() + (size_t)(by * 4) * pw * 4 + (size_t)(bx * 4) * 4;
                encode_etc1_block(src, pw * 4, out_blocks + (size_t)(by * bw + bx) * 8);
            }
        tex = std::move(pvr);
        return true;
    }

    // Native TEX container: 12-byte header {type=1 RGBA8888, w, h} + payload,
    // gzipped. This is the loader format for .tex.png (see load_tex_png).
    std::vector<uint8_t> texraw(12 + (size_t)w * h * 4, 0);
    uint32_t hdr3[3] = { 1, (uint32_t)w, (uint32_t)h };
    memcpy(texraw.data(), hdr3, 12);
    memcpy(texraw.data() + 12, flipped.data(), (size_t)w * h * 4);
    if (!gzip_compress(texraw, tex)) {
        if (err) *err = "gzip compression of texture failed";
        return false;
    }
    return true;
}

// ─── Texture resolution (mirrors tools/fbx_import.cpp) ────────────────────
static std::string strip_ext(const std::string& in) {
    std::string base = in;
    size_t nul = base.find('\0');
    if (nul != std::string::npos) base.resize(nul);
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

static std::string find_source_image(const fs::path& fbx_dir, const std::string& tex_name,
                                     std::error_code& ec) {
    std::vector<fs::path> roots = { fbx_dir };
    if (fs::is_directory(fbx_dir, ec))
        for (const char* sub : {"images", "textures", "maps", "Texture", "Textures"}) {
            fs::path p = fbx_dir / sub;
            if (fs::is_directory(p, ec)) roots.push_back(p);
        }
    std::string stem = strip_ext(tex_name);
    static const char* exts[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    for (const auto& root : roots) {
        for (const char* e : exts) {
            fs::path c = root / (stem + e);
            if (fs::is_regular_file(c, ec)) return c.string();
        }
        for (const char* e : exts) {
            fs::path c = root / (stem + ".tex" + e);
            if (fs::is_regular_file(c, ec)) return c.string();
        }
    }
    return {};
}

// ─── Node-transform baking + centering ───────────────────────────────────────
// The glTF→POD path keeps the source scene's node hierarchy (Sketchfab_model
// root with unit-conversion scales, GLTF_SceneRootNode axis rotations, 500+
// skeleton bones, …). The game's PODLoader::CreateModel BAKES every mesh
// node's world matrix into its vertices at load time, so those root
// transforms silently rescale / rotate / even zero-out the geometry in-game
// while the ruby visualiser (which applies node matrices at render time)
// shows something else. Fix: bake every mesh node's world matrix into its
// vertex positions right here, collapse mesh nodes to identity, drop the
// non-mesh hierarchy for static models, and re-center the bounding box at
// the origin so scene-object placement is predictable.
static void bake_and_center_model(PODModel& model, bool center) {
    if (model.meshes.empty()) return;

    // 1) Bake each mesh node's world matrix into its vertex positions and
    //    normals, then set the node transform to identity.
    for (int ni = 0; ni < (int)model.nodes.size(); ++ni) {
        auto& node = model.nodes[ni];
        if (node.object_index < 0 ||
            node.object_index >= (int)model.meshes.size())
            continue;
        float world[16];
        av::get_node_matrix(model, ni, 0.0f, world);

        bool identity = true;
        for (int k = 0; k < 16; ++k) {
            float expect = (k == 0 || k == 5 || k == 10 || k == 15) ? 1.0f : 0.0f;
            if (std::fabs(world[k] - expect) > 1e-6f) { identity = false; break; }
        }
        if (identity) continue;   // nothing to bake

        auto& mesh = model.meshes[node.object_index];
        // Positions.
        for (size_t v = 0; v + 2 < mesh.positions.size(); v += 3) {
            float x = mesh.positions[v], y = mesh.positions[v+1], z = mesh.positions[v+2];
            mesh.positions[v]   = world[0]*x + world[4]*y + world[8]*z  + world[12];
            mesh.positions[v+1] = world[1]*x + world[5]*y + world[9]*z  + world[13];
            mesh.positions[v+2] = world[2]*x + world[6]*y + world[10]*z + world[14];
        }
        // Normals (rotation-only: use upper 3×3, no translation).
        for (size_t v = 0; v + 2 < mesh.normals.size(); v += 3) {
            float x = mesh.normals[v], y = mesh.normals[v+1], z = mesh.normals[v+2];
            float nx = world[0]*x + world[4]*y + world[8]*z;
            float ny = world[1]*x + world[5]*y + world[9]*z;
            float nz = world[2]*x + world[6]*y + world[10]*z;
            float len = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            mesh.normals[v] = nx; mesh.normals[v+1] = ny; mesh.normals[v+2] = nz;
        }
        // Collapse the node to identity so the POD carries no hidden transform.
        for (int k = 0; k < 16; ++k) node.matrix[k] = 0.0f;
        node.matrix[0] = node.matrix[5] = node.matrix[10] = node.matrix[15] = 1.0f;
        node.has_matrix = true;
        node.has_translation = node.has_rotation = node.has_scale = false;
    }

    // 2) Drop non-mesh hierarchy nodes (skeleton bones, glTF root wrappers).
    //    For static models they only poison the bbox and confuse the editor.
    //    Keep the nodes the game needs: mesh nodes first, then any named
    //    CenterPoint. Bone-animated models keep their skeleton.
    bool has_bones = false;
    for (const auto& m : model.meshes)
        if (m.bones_per_vertex > 0) { has_bones = true; break; }

    if (!has_bones) {
        std::vector<PODNode> kept;
        kept.reserve(model.nodes.size());
        for (auto& n : model.nodes) {
            bool is_mesh = n.object_index >= 0 &&
                           n.object_index < (int)model.meshes.size() &&
                           n.object_index < model.num_mesh_nodes;
            bool is_center = (n.name == "CenterPoint");
            if (is_mesh || is_center) {
                n.parent_index = -1;   // flat hierarchy after the strip
                kept.push_back(std::move(n));
            }
        }
        model.nodes = std::move(kept);
        model.num_mesh_nodes = (int)model.nodes.size();
    }

    // 3) Center the bounding box at the origin (predictable scene placement).
    //    Recompute mesh AABBs from the BAKED positions — the loader's cached
    //    AABBs are pre-bake and stale after step 1.
    if (center) {
        float mnx =  1e30f, mny =  1e30f, mnz =  1e30f;
        float mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
        for (auto& m : model.meshes) {
            m.min_x = m.min_y = m.min_z =  1e9f;
            m.max_x = m.max_y = m.max_z = -1e9f;
            for (size_t v = 0; v + 2 < m.positions.size(); v += 3) {
                float px = m.positions[v], py = m.positions[v+1], pz = m.positions[v+2];
                if (px < m.min_x) m.min_x = px; if (px > m.max_x) m.max_x = px;
                if (py < m.min_y) m.min_y = py; if (py > m.max_y) m.max_y = py;
                if (pz < m.min_z) m.min_z = pz; if (pz > m.max_z) m.max_z = pz;
            }
            if (m.positions.empty()) continue;
            mnx = std::min(mnx, m.min_x); mxx = std::max(mxx, m.max_x);
            mny = std::min(mny, m.min_y); mxy = std::max(mxy, m.max_y);
            mnz = std::min(mnz, m.min_z); mxz = std::max(mxz, m.max_z);
        }
        if (mnx <= mxx) {
            float cx = (mnx + mxx) * 0.5f;
            float cy = (mny + mxy) * 0.5f;
            float cz = (mnz + mxz) * 0.5f;
            for (auto& m : model.meshes) {
                for (size_t v = 0; v + 2 < m.positions.size(); v += 3) {
                    m.positions[v]   -= cx;
                    m.positions[v+1] -= cy;
                    m.positions[v+2] -= cz;
                }
                m.min_x -= cx; m.max_x -= cx;
                m.min_y -= cy; m.max_y -= cy;
                m.min_z -= cz; m.max_z -= cz;
            }
        }
    }
}

bool fbx_to_pod(const std::string& fbx_path, const std::string& pod_path,
                const PodConvertOptions& opts,
                std::vector<std::string>* written_textures,
                std::string* err) {
    PODModel model = fbx_load(fbx_path);

    if (model.meshes.empty()) {
        if (err) *err = "no meshes found (unsupported FBX or parse failure)";
        return false;
    }

    // FBX/glTF UVs put v = 0 at the top of the texture; the game samples POD
    // UVs with v = 0 at the bottom, so flip when carrying FBX previews into a
    // game asset. EXCEPTION: ufbx already flips V when it mirrors a left-handed
    // FBX scene (3ds Max / Unity exports) — model.uv_v_flipped is true then, and
    // flipping again would mirror textures upside-down. Mirror the viewer's dcc_uv
    // logic (asset_viewer.cpp).
    if (opts.flip_v && !model.uv_v_flipped) {
        for (auto& m : model.meshes) {
            for (size_t i = 0; i + 1 < m.uvs.size(); i += 2)
                m.uvs[i + 1] = 1.0f - m.uvs[i + 1];
        }
    }

    // Scale positions. FBX authored in centimetres is silently divided by 100 by ufbx
    // (target_unit_meters=1.0), so `opts.unit_scale` lets the user undo that and reach
    // game units (≈ 39.4 units / metre). Combined with the class-level `opts.scale`,
    // effective multiplier = unit_scale * scale.
    float fbx_scale = opts.unit_scale * opts.scale;
    if (fbx_scale != 1.0f && fbx_scale > 0.0f) {
        for (auto& m : model.meshes) {
            for (float& pv : m.positions) pv *= fbx_scale;
        }
    }

    if (opts.convert_textures) {
        fs::path fbx_dir = fs::path(fbx_path).parent_path();
        fs::path pod_dir = fs::path(pod_path).parent_path();
        if (pod_dir.empty()) pod_dir = ".";
        std::error_code ec;
        for (auto& tex_name : model.texture_filenames) {
            if (tex_name.empty()) continue;
            std::string stem = strip_ext(tex_name);
            std::string src = find_source_image(fbx_dir, tex_name, ec);
            if (src.empty()) {
                fprintf(stderr, "  ! texture '%s' not found next to the FBX — skipped\n", tex_name.c_str());
                continue;
            }
            int w = 0, h = 0, comp = 0;
            uint8_t* rgba = stbi_load(src.c_str(), &w, &h, &comp, 4);
            if (!rgba) {
                fprintf(stderr, "  ! texture '%s' failed to decode — skipped\n", src.c_str());
                continue;
            }
            std::vector<uint8_t> tex;
            bool ok = encode_texture(rgba, w, h, opts.output_pvr, opts.pvr_resolution, tex, err);
            stbi_image_free(rgba);
            if (!ok) return false;

            std::string out_name = stem + (opts.output_pvr ? ".pvr" : ".tex.png");
            std::string out_path = (pod_dir / out_name).string();
            if (!opts.overwrite && fs::exists(out_path)) {
                fprintf(stderr, "  ! %s already exists (use --force to overwrite)\n", out_name.c_str());
            } else if (!write_file(out_path, tex.data(), tex.size())) {
                if (err) *err = "cannot write texture file: " + out_path;
                return false;
            } else {
                fprintf(stderr, "  ✓ texture %s  (%dx%d → %.1f kB)\n", out_name.c_str(), w, h,
                        tex.size() / 1024.0);
            }
            tex_name = out_name; // POD references the file we just wrote
            if (written_textures) written_textures->push_back(out_name);
        }
    }

    if (!opts.overwrite && fs::exists(pod_path)) {
        if (err) *err = "output exists (use --force to overwrite): " + pod_path;
        return false;
    }
    // Bake FBX node transforms into vertices and center the model. The FBX
    // importer already bakes geometry_to_world, so the bake is usually a no-op,
    // but centering gives predictable scene placement.
    bake_and_center_model(model, /*center=*/true);
    if (!pod_write(model, pod_path, err)) {
        if (err && err->empty()) *err = "POD serialization failed";
        return false;
    }
    return true;
}

bool glb_to_pod(const std::string& glb_path,
                const std::string& pod_path,
                const PodConvertOptions& opts,
                std::vector<std::string>* written_textures,
                std::vector<std::string>* written_clips,
                std::string* err) {
    PODModel model;
    std::vector<GLTFImageBuffer> embedded_images;
    bool is_glb = (glb_path.size() >= 4 && glb_path.compare(glb_path.size() - 4, 4, ".glb") == 0);
    bool ok = is_glb ? gltf_import_glb(glb_path, model, embedded_images, err, nullptr, opts.scale)
                     : gltf_import_gltf(glb_path, model, embedded_images, err, nullptr, opts.scale);
    if (!ok) return false;

    if (opts.flip_v) {
        for (auto& m : model.meshes) {
            for (size_t i = 0; i + 1 < m.uvs.size(); i += 2)
                m.uvs[i + 1] = 1.0f - m.uvs[i + 1];
        }
    }

    fs::path pod_dir = fs::path(pod_path).parent_path();
    if (pod_dir.empty()) pod_dir = ".";
    std::string pod_stem = strip_ext(fs::path(pod_path).filename().string());

    if (opts.convert_textures) {
        fs::path glb_dir = fs::path(glb_path).parent_path();
        std::error_code ec;

        for (size_t i = 0; i < model.texture_filenames.size(); ++i) {
            std::string& tex_name = model.texture_filenames[i];
            if (tex_name.empty()) continue;
            std::string stem = strip_ext(tex_name);

            int w = 0, h = 0, comp = 0;
            uint8_t* rgba = nullptr;

            // Check if we have an embedded image payload for this texture slot
            if (i < embedded_images.size() && !embedded_images[i].data.empty()) {
                rgba = stbi_load_from_memory(embedded_images[i].data.data(),
                                             (int)embedded_images[i].data.size(),
                                             &w, &h, &comp, 4);
            }

            // Fallback: look on disk
            if (!rgba) {
                std::string src = find_source_image(glb_dir, tex_name, ec);
                if (!src.empty()) {
                    rgba = stbi_load(src.c_str(), &w, &h, &comp, 4);
                }
            }

            if (!rgba) {
                fprintf(stderr, "  ! texture '%s' could not be loaded — skipped\n", tex_name.c_str());
                continue;
            }

            std::vector<uint8_t> tex;
            bool enc_ok = encode_texture(rgba, w, h, opts.output_pvr, opts.pvr_resolution, tex, err);
            stbi_image_free(rgba);
            if (!enc_ok) return false;

            std::string out_name = stem + (opts.output_pvr ? ".pvr" : ".tex.png");
            std::string out_path = (pod_dir / out_name).string();
            if (!opts.overwrite && fs::exists(out_path)) {
                fprintf(stderr, "  ! %s already exists (use --force to overwrite)\n", out_name.c_str());
            } else if (!write_file(out_path, tex.data(), tex.size())) {
                if (err) *err = "cannot write texture file: " + out_path;
                return false;
            } else {
                fprintf(stderr, "  ✓ texture %s  (%dx%d → %.1f kB)\n", out_name.c_str(), w, h,
                        tex.size() / 1024.0);
            }
            tex_name = out_name;
            if (written_textures) written_textures->push_back(out_name);
        }
    }

    if (!opts.overwrite && fs::exists(pod_path)) {
        if (err) *err = "output exists (use --force to overwrite): " + pod_path;
        return false;
    }
    // Bake glTF node transforms into vertices and center the model. CRITICAL:
    // glTF scenes carry root-node unit-conversion scales (Sketchfab 394×) and
    // axis-rotation matrices (GLTF_SceneRootNode Z-up→Y-up). The game's
    // PODLoader bakes these into vertices at load, producing unpredictable
    // in-game sizes. Baking here + collapsing to identity makes the POD
    // self-contained: what you see in the converter output IS what the game
    // renders, and the bbox/center/feet computations are all correct.
    bake_and_center_model(model, /*center=*/true);
    if (!pod_write(model, pod_path, err)) {
        if (err && err->empty()) *err = "POD serialization failed";
        return false;
    }

    // Extract separate animation clip POD files (<pod_stem>_<clip_name>.pod)
    std::vector<std::pair<std::string, PODModel>> clips;
    if (gltf_import_all_clips(glb_path, clips, nullptr, opts.scale)) {
        for (const auto& kv : clips) {
            std::string clip_pod_name = pod_stem + "_" + kv.first + ".POD";
            std::string clip_pod_path = (pod_dir / clip_pod_name).string();
            if (opts.overwrite || !fs::exists(clip_pod_path)) {
                std::string clip_err;
                if (pod_write(kv.second, clip_pod_path, &clip_err)) {
                    fprintf(stderr, "  ✓ clip POD: %s (%d frames @ %.0f fps)\n",
                            clip_pod_name.c_str(), kv.second.num_frames, kv.second.fps);
                    if (written_clips) written_clips->push_back(clip_pod_name);
                }
            }
        }
    }

    return true;
}

// ─── CLI ───────────────────────────────────────────────────────────────────
int pod_convert_cli(int argc, char** argv) {
    PodConvertOptions opts;
    std::string in_path, pod_path;
    bool is_glb = false;

    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-flip") opts.flip_v = false;
        else if (a == "--no-textures") opts.convert_textures = false;
        else if (a == "--tex-png") opts.output_pvr = false;
        else if (a == "--force" || a == "-f") opts.overwrite = true;
        else if ((a == "--scale" || a == "-s") && i + 1 < argc) {
            opts.scale = std::stof(argv[++i]);
        }
        else if ((a == "--unit" || a == "-u") && i + 1 < argc) {
            // FBX unit multiplier. ufbx converts to metres at load, so use this to
            // reach game units. Common: 1.0 (metres), 100.0 (centimetres), 39.37 (metres→inches).
            opts.unit_scale = std::stof(argv[++i]);
        }
        else if (a == "--help" || a == "-h") {
            printf("Usage: bin/ruby --fbx2pod <in.fbx> [out.pod] [options]\n"
                   "       bin/ruby --glb2pod <in.glb/gltf> [out.pod] [options]\n"
                   "  out.pod        defaults to <in>.POD next to the source model\n"
                   "  --scale, -s    uniform scale factor applied to model (e.g. 80.0)\n"
                   "  --unit, -u     FBX unit multiplier (e.g. 100.0 for cm sources, 39.37 for m→inches)\n"
                   "  --no-flip      keep source V coordinates (skip bottom-origin flip)\n"
                   "  --no-textures  do not convert referenced textures\n"
                   "  --tex-png      emit gzipped native .tex.png (backgrounds)\n"
                   "                   instead of the default raw .pvr (game model textures)\n"
                   "  --force, -f    overwrite existing output files\n");
            return 0;
        }
        else if (in_path.empty()) {
            in_path = a;
            std::string low = in_path;
            for (char& c : low) c = (char)tolower((unsigned char)c);
            if (low.rfind(".glb") != std::string::npos || low.rfind(".gltf") != std::string::npos)
                is_glb = true;
        }
        else if (pod_path.empty()) pod_path = a;
        else {
            fprintf(stderr, "unexpected argument: %s\n", a.c_str());
            return 1;
        }
    }
    if (in_path.empty()) {
        fprintf(stderr, "usage: bin/ruby --fbx2pod <in.fbx> [out.pod] [options]\n"
                        "       bin/ruby --glb2pod <in.glb> [out.pod] [options]\n");
        return 1;
    }
    if (pod_path.empty())
        pod_path = (fs::path(in_path).parent_path() /
                    (strip_ext(fs::path(in_path).filename().string()) + ".POD")).string();

    std::vector<std::string> written_tex, written_clips;
    std::string err;
    printf("Converting %s → %s ...\n", in_path.c_str(), pod_path.c_str());
    bool ok = false;
    if (is_glb) {
        ok = glb_to_pod(in_path, pod_path, opts, &written_tex, &written_clips, &err);
    } else {
        ok = fbx_to_pod(in_path, pod_path, opts, &written_tex, &err);
    }

    if (!ok) {
        fprintf(stderr, "Model → POD failed: %s\n", err.empty() ? "unknown error" : err.c_str());
        return 1;
    }
    printf("  ✓ wrote %s (%zu textures, %zu animation clips)\n",
           pod_path.c_str(), written_tex.size(), written_clips.size());
    return 0;
}

} // namespace av