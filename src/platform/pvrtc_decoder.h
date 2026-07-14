#pragma once
#include <stdint.h>

namespace pvr {

// Existing PVRTC decompressor
uint32_t PVRTDecompressPVRTC(const void* compressedData, uint32_t do2bitMode, uint32_t xDim, uint32_t yDim, uint8_t* outResultImage);

// Decompresses DXT1 (1), DXT3 (3), DXT5 (5) to RGBA8888
uint32_t PVRTDecompressDXT(const void* srcData, uint32_t width, uint32_t height, uint8_t* dstData, uint32_t format);

// Decompresses ETC1/ETC2 formats to RGBA8888
uint32_t PVRTDecompressETC(const void* srcData, uint32_t width, uint32_t height, uint8_t* dstData, uint32_t format);

// Decodes uncompressed formats to RGBA8888 based on channel names and bit depths
bool PVRTDecodeUncompressed(const void* srcData, uint32_t width, uint32_t height,
                            char c0, char c1, char c2, char c3,
                            uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3,
                            uint8_t* dstData);

// Parses PVR v3 pixel format and returns format type code:
// 1 = ETC1, 2 = PVRTC 2bpp, 3 = PVRTC 4bpp, 4 = DXT1, 5 = DXT3, 6 = DXT5, 10 = uncompressed, -1 = unsupported
int ParsePVRv3Format(uint64_t pixel_format, uint32_t& gl_format, uint32_t& gl_type, int& bpp,
                     char& c0, char& c1, char& c2, char& c3,
                     uint8_t& d0, uint8_t& d1, uint8_t& d2, uint8_t& d3);

// Parses PVR v2 format flags
int ParsePVRv2Format(uint32_t flags, uint32_t& gl_format, uint32_t& gl_type, int& bpp,
                     char& c0, char& c1, char& c2, char& c3,
                     uint8_t& d0, uint8_t& d1, uint8_t& d2, uint8_t& d3);

} // namespace pvr
