/* pvr_loader.cpp — PVR texture file loader with ETC1 decoder
 *
 * Implements our own PVR parser + ETC1 decompressor.
 * No external dependencies — pure C++ with GL upload.
 *
 * PVR v3 header:
 *   uint32 version      = 0x03525650 ("PVR\3")
 *   uint32 flags
 *   uint64 pixel_format  (6 = ETC1)
 *   uint32 color_space
 *   uint32 channel_type
 *   uint32 height, width, depth
 *   uint32 num_surfaces, num_faces, mip_count
 *   uint32 metadata_size
 *   [metadata bytes]
 *   [pixel data]
 *
 * PVR v2 header (legacy):
 *   uint32 header_size   = 44
 *   uint32 height, width
 *   uint32 mip_count
 *   uint32 flags         (bit 0x0036 = format mask)
 *   uint32 data_size
 *   uint32 bpp
 *   uint32 mask_r, mask_g, mask_b, mask_a
 *   uint32 magic         = 0x21525650 ("PVR!")
 *   uint32 num_surfaces
 *   [pixel data]
 */

#include "pvr_loader.h"
#include "pvrtc_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <zlib.h>

bool g_pvr_software_decode = true;

/* ===================== ETC1 Decoder =====================
 * Decodes a single 4x4 ETC1 block (8 bytes → 64 RGBA bytes).
 * This is our own implementation, same algorithm as the bridge decoder. */

static inline int etc1_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static const int etc1_modifiers[8][2] = {
    {  2,   8}, {  5,  17}, {  9,  29}, { 13,  42},
    { 18,  56}, { 24,  71}, { 33,  92}, { 47, 127}
};

static void decode_etc1_block(const uint8_t* src, uint8_t* dst, int dst_stride) {
    /* Read 64-bit block (big-endian) */
    uint64_t block = 0;
    for (int i = 0; i < 8; i++)
        block = (block << 8) | src[i];
    
    int diff  = (block >> 33) & 1;
    int flip  = (block >> 32) & 1;
    int table1 = (block >> 37) & 7;
    int table2 = (block >> 34) & 7;
    
    int r1, g1, b1, r2, g2, b2;
    
    if (diff == 0) {
        /* Individual mode: two RGB444 colors */
        int rr1 = (block >> 60) & 0xF; r1 = (rr1 << 4) | rr1;
        int rr2 = (block >> 56) & 0xF; r2 = (rr2 << 4) | rr2;
        int gg1 = (block >> 52) & 0xF; g1 = (gg1 << 4) | gg1;
        int gg2 = (block >> 48) & 0xF; g2 = (gg2 << 4) | gg2;
        int bb1 = (block >> 44) & 0xF; b1 = (bb1 << 4) | bb1;
        int bb2 = (block >> 40) & 0xF; b2 = (bb2 << 4) | bb2;
    } else {
        /* Differential mode: RGB555 + RGB333 delta */
        int r = (block >> 59) & 0x1F;
        int dr = (block >> 56) & 0x7; if (dr > 3) dr -= 8;
        int g = (block >> 51) & 0x1F;
        int dg = (block >> 48) & 0x7; if (dg > 3) dg -= 8;
        int b = (block >> 43) & 0x1F;
        int db = (block >> 40) & 0x7; if (db > 3) db -= 8;
        
        r1 = (r << 3) | (r >> 2);
        int r2v = r + dr; r2 = (r2v << 3) | (r2v >> 2);
        g1 = (g << 3) | (g >> 2);
        int g2v = g + dg; g2 = (g2v << 3) | (g2v >> 2);
        b1 = (b << 3) | (b >> 2);
        int b2v = b + db; b2 = (b2v << 3) | (b2v >> 2);
    }
    
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            int pixel_idx = col * 4 + row;
            
            int msb = (block >> (pixel_idx + 16)) & 1;
            int lsb = (block >> pixel_idx) & 1;
            
            int sub;
            if (flip == 0) sub = (col >= 2) ? 1 : 0;
            else           sub = (row >= 2) ? 1 : 0;
            
            int rb = sub ? r2 : r1;
            int gb = sub ? g2 : g1;
            int bb = sub ? b2 : b1;
            int table = sub ? table2 : table1;
            
            int mod = etc1_modifiers[table][lsb];
            if (msb) mod = -mod;
            
            uint8_t* pixel = dst + row * dst_stride + col * 4;
            pixel[0] = etc1_clamp(rb + mod);
            pixel[1] = etc1_clamp(gb + mod);
            pixel[2] = etc1_clamp(bb + mod);
            pixel[3] = 255; /* ETC1 is always opaque */
        }
    }
}

/* ===================== ETC1 Full Decode ===================== */
static bool decode_etc1_data(const uint8_t* src, int width, int height, 
                              std::vector<uint8_t>& rgba_out) {
    int block_w = (width + 3) / 4;
    int block_h = (height + 3) / 4;
    
    rgba_out.resize(width * height * 4, 255);
    
    for (int by = 0; by < block_h; by++) {
        for (int bx = 0; bx < block_w; bx++) {
            uint8_t block_rgba[4 * 4 * 4]; /* 4x4 pixels × RGBA */
            decode_etc1_block(src + (by * block_w + bx) * 8, block_rgba, 4 * 4);
            
            for (int row = 0; row < 4 && (by * 4 + row) < height; row++) {
                for (int col = 0; col < 4 && (bx * 4 + col) < width; col++) {
                    int dst_x = bx * 4 + col;
                    int dst_y = by * 4 + row;
                    memcpy(&rgba_out[(dst_y * width + dst_x) * 4],
                           &block_rgba[row * 16 + col * 4], 4);
                }
            }
        }
    }
    
    std::cout << "[PVR] Decoded ETC1 " << width << "x" << height 
              << " (" << (block_w * block_h) << " blocks)" << std::endl;
    return true;
}

/* ===================== PVR Header Structures ===================== */

#pragma pack(push, 1)

/* PVR v3 header (52 bytes) */
struct PVRv3Header {
    uint32_t version;       /* 0x03525650 = "PVR\3" */
    uint32_t flags;
    uint64_t pixel_format;  /* Lower 32 bits: 0=PVRTC2, 1=PVRTC4, 6=ETC1, ... */
    uint32_t color_space;
    uint32_t channel_type;
    uint32_t height;
    uint32_t width;
    uint32_t depth;
    uint32_t num_surfaces;
    uint32_t num_faces;
    uint32_t mip_count;
    uint32_t metadata_size;
};

/* PVR v2 header (44 bytes) */
struct PVRv2Header {
    uint32_t header_size;   /* Always 44 */
    uint32_t height;
    uint32_t width;
    uint32_t mip_count;
    uint32_t flags;
    uint32_t data_size;
    uint32_t bpp;
    uint32_t mask_r;
    uint32_t mask_g;
    uint32_t mask_b;
    uint32_t mask_a;
    uint32_t magic;         /* 0x21525650 = "PVR!" */
    uint32_t num_surfaces;
};

#pragma pack(pop)

/* PVR v3 pixel format constants */
#define PVR3_PIXEL_FORMAT_ETC1  6
#define PVR3_VERSION            0x03525650

/* PVR v2 format flags */
#define PVR2_MAGIC              0x21525650
#define PVR2_FORMAT_ETC1        0x0036

/* ===================== Main Loader ===================== */

static bool decompress_gzip_buffer(const std::vector<uint8_t>& in_buf, std::vector<uint8_t>& out_buf) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) return false;
    
    strm.next_in = const_cast<uint8_t*>(in_buf.data());
    strm.avail_in = in_buf.size();
    
    uint8_t temp[16384];
    int ret;
    do {
        strm.next_out = temp;
        strm.avail_out = sizeof(temp);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return false;
        }
        size_t decoded = sizeof(temp) - strm.avail_out;
        if (decoded > 0) {
            out_buf.insert(out_buf.end(), temp, temp + decoded);
        }
    } while (ret != Z_STREAM_END && strm.avail_in > 0);
    
    inflateEnd(&strm);
    return (ret == Z_STREAM_END);
}

bool pvr_decode_to_rgba(const uint8_t* file_data, size_t file_size, std::vector<uint8_t>& rgba_out, int& width, int& height) {
    if (file_size == 0 || !file_data) return false;

    std::vector<uint8_t> decompressed;
    const uint8_t* active_data = file_data;
    size_t active_size = file_size;

    if (file_size >= 2 && file_data[0] == 0x1f && file_data[1] == 0x8b) {
        std::vector<uint8_t> input_wrap(file_data, file_data + file_size);
        if (!decompress_gzip_buffer(input_wrap, decompressed)) {
            std::cerr << "[PVR-Decoder] Gzip decompression failed" << std::endl;
            return false;
        }
        active_data = decompressed.data();
        active_size = decompressed.size();
    }

    if (active_size < 12) {
        std::cerr << "[PVR-Decoder] Active buffer too small: " << active_size << " bytes" << std::endl;
        return false;
    }

    bool is_pvr = false;
    if (active_size >= 52) {
        uint32_t magic_v3 = *(const uint32_t*)active_data;
        uint32_t magic_v2 = *(const uint32_t*)(active_data + 44);
        if (magic_v3 == PVR3_VERSION || magic_v2 == PVR2_MAGIC) {
            is_pvr = true;
        }
    }

    if (is_pvr) {
        int w = 0, h = 0;
        const uint8_t* pixel_data = nullptr;
        int format_type = -1;
        uint32_t gl_format = 0x1908;
        uint32_t gl_type = 0x1401;
        int bpp = 4;
        char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        uint8_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;

        const PVRv3Header* v3 = (const PVRv3Header*)active_data;
        if (v3->version == PVR3_VERSION) {
            w = v3->width;
            h = v3->height;
            pixel_data = active_data + sizeof(PVRv3Header) + v3->metadata_size;
            format_type = pvr::ParsePVRv3Format(v3->pixel_format, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        } else {
            const PVRv2Header* v2 = (const PVRv2Header*)active_data;
            w = v2->width;
            h = v2->height;
            pixel_data = active_data + v2->header_size;
            format_type = pvr::ParsePVRv2Format(v2->flags, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        }

        if (format_type < 0 || w <= 0 || h <= 0) {
            std::cerr << "[PVR-Decoder] Unsupported PVR format_type or invalid dimensions" << std::endl;
            return false;
        }

        width = w;
        height = h;
        rgba_out.resize(width * height * 4, 255);
        bool decode_success = false;

        if (format_type == 1) { // ETC1
            pvr::PVRTDecompressETC(pixel_data, width, height, rgba_out.data(), 6);
            decode_success = true;
        } else if (format_type == 2 || format_type == 3) { // PVRTC
            uint32_t do2bitMode = (format_type == 2) ? 1 : 0;
            pvr::PVRTDecompressPVRTC(pixel_data, do2bitMode, width, height, rgba_out.data());
            decode_success = true;
        } else if (format_type >= 4 && format_type <= 6) { // DXT
            uint32_t dxt_fmt = (format_type == 4) ? 1 : ((format_type == 5) ? 3 : 5);
            pvr::PVRTDecompressDXT(pixel_data, width, height, rgba_out.data(), dxt_fmt);
            decode_success = true;
        } else if (format_type == 10) { // Uncompressed
            decode_success = pvr::PVRTDecodeUncompressed(pixel_data, width, height, c0, c1, c2, c3, d0, d1, d2, d3, rgba_out.data());
        }

        return decode_success;
    } else {
        uint32_t img_type = *(const uint32_t*)(active_data + 0);
        uint32_t w = *(const uint32_t*)(active_data + 4);
        uint32_t h = *(const uint32_t*)(active_data + 8);

        if (w == 0 || h == 0 || w > 8192 || h > 8192 || img_type < 1 || img_type > 8) {
            std::cerr << "[PVR-Decoder] Invalid .tex dimensions or format: " << w << "x" << h << ", type=" << img_type << std::endl;
            return false;
        }

        width = (int)w;
        height = (int)h;
        size_t num_pixels = (size_t)width * (size_t)height;
        rgba_out.resize(num_pixels * 4, 255);

        const uint8_t* payload = active_data + 12;
        size_t payload_size = active_size - 12;

        if (img_type == 1) { // RGBA_8888
            if (payload_size < num_pixels * 4) return false;
            std::memcpy(rgba_out.data(), payload, num_pixels * 4);
        }
        else if (img_type == 2) { // RGBA_4444 (2 bytes/pixel)
            if (payload_size < num_pixels * 2) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                uint16_t d = payload[u * 2] | (payload[u * 2 + 1] << 8);
                rgba_out[u * 4 + 0] = ((d >> 12) & 15) * 17;
                rgba_out[u * 4 + 1] = ((d >> 8)  & 15) * 17;
                rgba_out[u * 4 + 2] = ((d >> 4)  & 15) * 17;
                rgba_out[u * 4 + 3] = (d & 15) * 17;
            }
        }
        else if (img_type == 3) { // RGBA_5551 (2 bytes/pixel)
            if (payload_size < num_pixels * 2) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                uint16_t d = payload[u * 2] | (payload[u * 2 + 1] << 8);
                rgba_out[u * 4 + 0] = ((d >> 11) & 31) * 255 / 31;
                rgba_out[u * 4 + 1] = ((d >> 6)  & 31) * 255 / 31;
                rgba_out[u * 4 + 2] = ((d >> 1)  & 31) * 255 / 31;
                rgba_out[u * 4 + 3] = (d & 1) * 255;
            }
        }
        else if (img_type == 4) { // RGB_888 (3 bytes/pixel)
            if (payload_size < num_pixels * 3) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                rgba_out[u * 4 + 0] = payload[u * 3 + 0];
                rgba_out[u * 4 + 1] = payload[u * 3 + 1];
                rgba_out[u * 4 + 2] = payload[u * 3 + 2];
                rgba_out[u * 4 + 3] = 255;
            }
        }
        else if (img_type == 5) { // RGB_565 (2 bytes/pixel)
            if (payload_size < num_pixels * 2) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                uint16_t d = payload[u * 2] | (payload[u * 2 + 1] << 8);
                rgba_out[u * 4 + 0] = ((d >> 11) & 31) * 255 / 31;
                rgba_out[u * 4 + 1] = ((d >> 5)  & 63) * 255 / 63;
                rgba_out[u * 4 + 2] = (d & 31) * 255 / 31;
                rgba_out[u * 4 + 3] = 255;
            }
        }
        else if (img_type == 6) { // LUMINANCE_8 (1 byte/pixel)
            if (payload_size < num_pixels) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                uint8_t val = payload[u];
                rgba_out[u * 4 + 0] = val;
                rgba_out[u * 4 + 1] = val;
                rgba_out[u * 4 + 2] = val;
                rgba_out[u * 4 + 3] = 255;
            }
        }
        else if (img_type == 7) { // ALPHA_8 (1 byte/pixel)
            if (payload_size < num_pixels) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                rgba_out[u * 4 + 0] = 255;
                rgba_out[u * 4 + 1] = 255;
                rgba_out[u * 4 + 2] = 255;
                rgba_out[u * 4 + 3] = payload[u];
            }
        }
        else if (img_type == 8) { // LUMINANCE_ALPHA_88 (2 bytes/pixel)
            if (payload_size < num_pixels * 2) return false;
            for (size_t u = 0; u < num_pixels; ++u) {
                uint8_t lum = payload[u * 2 + 0];
                uint8_t alpha = payload[u * 2 + 1];
                rgba_out[u * 4 + 0] = lum;
                rgba_out[u * 4 + 1] = lum;
                rgba_out[u * 4 + 2] = lum;
                rgba_out[u * 4 + 3] = alpha;
            }
        }

        return true;
    }
}

bool pvr_upload_compressed_to_tex(const uint8_t* file_data, size_t file_size, GLuint tex_name, int& out_w, int& out_h) {
    if (file_size == 0 || !file_data) return false;

    std::vector<uint8_t> decompressed;
    const uint8_t* active_data = file_data;
    size_t active_size = file_size;

    if (file_size >= 2 && file_data[0] == 0x1f && file_data[1] == 0x8b) {
        if (!decompress_gzip_buffer(std::vector<uint8_t>(file_data, file_data + file_size), decompressed)) {
            return false;
        }
        active_data = decompressed.data();
        active_size = decompressed.size();
    }

    if (active_size < 12) return false;

    bool is_pvr = false;
    if (active_size >= 52) {
        uint32_t magic_v3 = *(const uint32_t*)active_data;
        uint32_t magic_v2 = *(const uint32_t*)(active_data + 44);
        if (magic_v3 == PVR3_VERSION || magic_v2 == PVR2_MAGIC) {
            is_pvr = true;
        }
    }

    int w = 0, h = 0;
    const uint8_t* pixel_data = nullptr;
    size_t data_size = 0;
    int format_type = -1;
    uint32_t gl_format = 0x1908;
    uint32_t gl_type = 0x1401;
    int bpp = 4;
    char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    uint8_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;

    if (is_pvr) {
        const PVRv3Header* v3 = (const PVRv3Header*)active_data;
        if (v3->version == PVR3_VERSION) {
            w = v3->width;
            h = v3->height;
            pixel_data = active_data + sizeof(PVRv3Header) + v3->metadata_size;
            data_size = active_size - (sizeof(PVRv3Header) + v3->metadata_size);
            format_type = pvr::ParsePVRv3Format(v3->pixel_format, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        } else {
            const PVRv2Header* v2 = (const PVRv2Header*)active_data;
            w = v2->width;
            h = v2->height;
            pixel_data = active_data + v2->header_size;
            data_size = active_size - v2->header_size;
            format_type = pvr::ParsePVRv2Format(v2->flags, gl_format, gl_type, bpp, c0, c1, c2, c3, d0, d1, d2, d3);
        }
    } else {
        return false;
    }

    if (format_type < 0 || w <= 0 || h <= 0) return false;

    GLenum internal_format = 0;
    if (format_type == 1) { // ETC1
        internal_format = 0x8D64; // GL_ETC1_RGB8_OES
    } else if (format_type == 2) { // PVRTC 2bpp
        internal_format = 0x8C01; // GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG
    } else if (format_type == 3) { // PVRTC 4bpp
        internal_format = 0x8C00; // GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
    } else if (format_type == 4) { // DXT1
        internal_format = 0x83F0; // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
    } else if (format_type == 5) { // DXT3
        internal_format = 0x83F2; // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
    } else if (format_type == 6) { // DXT5
        internal_format = 0x83F3; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
    }

    if (internal_format == 0) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, tex_name);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, (GLsizei)data_size, pixel_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    out_w = w;
    out_h = h;
    return true;
}

static bool pvr_upload_compressed(const uint8_t* file_data, size_t file_size, GLuint& out_tex, int& out_w, int& out_h) {
    glGenTextures(1, &out_tex);
    if (pvr_upload_compressed_to_tex(file_data, file_size, out_tex, out_w, out_h)) {
        return true;
    }
    glDeleteTextures(1, &out_tex);
    out_tex = 0;
    return false;
}


GLuint pvr_load_texture(const char* path, int* out_width, int* out_height) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        std::cerr << "[PVR] Failed to open: " << path << std::endl;
        return 0;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(f);
        return 0;
    }
    
    std::vector<uint8_t> file_data(file_size);
    if (fread(file_data.data(), 1, file_size, f) != (size_t)file_size) {
        std::cerr << "[PVR] Read error" << std::endl;
        fclose(f);
        return 0;
    }
    fclose(f);

    int width = 0, height = 0;
    GLuint tex = 0;

    if (!g_pvr_software_decode) {
        if (pvr_upload_compressed(file_data.data(), file_data.size(), tex, width, height)) {
            if (out_width)  *out_width = width;
            if (out_height) *out_height = height;
            std::cout << "[PVR] Uploaded compressed " << width << "x" << height 
                      << " → GL texture " << tex << " (hardware direct)" << std::endl;
            return tex;
        }
    }

    std::vector<uint8_t> rgba;
    if (!pvr_decode_to_rgba(file_data.data(), file_data.size(), rgba, width, height)) {
        std::cerr << "[PVR] Decoding failed for: " << path << std::endl;
        return 0;
    }

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, 
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    if (out_width)  *out_width = width;
    if (out_height) *out_height = height;
    
    std::cout << "[PVR] Uploaded decoded " << width << "x" << height 
              << " → GL texture " << tex << " (software decoded)" << std::endl;
    
    return tex;
}
