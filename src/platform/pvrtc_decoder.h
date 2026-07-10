#pragma once
#include <stdint.h>

namespace pvr {
uint32_t PVRTDecompressPVRTC(const void* compressedData, uint32_t do2bitMode, uint32_t xDim, uint32_t yDim, uint8_t* outResultImage);
} // namespace pvr
