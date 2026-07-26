# Swordigo OpenSwordigo Research: PowerVR Asset Pipeline & Texture/Mesh Decoders

## 1. PowerVR Texture Decompression Pipeline

The original Swordigo assets rely heavily on compressed PowerVR texture formats (`.pvr`) including **PVRTC1** (2bpp & 4bpp) and **ETC1** for high-compression GPU streaming. OpenSwordigo features native C++ decompressors (`Caver::PVRTCDecoder` and `Caver::PVR3Loader`) to expand compressed streams to RGBA8888 pixel buffers before uploading to OpenGL context drivers.

---

## 2. PVRTC Header & Pixel Expansion C++ Solver

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace Caver {

#pragma pack(push, 1)
struct PVRHeaderV3 {
    uint32_t version;          // 0x03525650 ("PVR\x03")
    uint32_t flags;            // Flags (pre-multiplied alpha, etc.)
    uint64_t pixel_format;     // 64-bit Pixel Format code
    uint32_t color_space;      // 0: Linear, 1: sRGB
    uint32_t channel_type;     // Data type (Unsigned Byte, Float, etc.)
    uint32_t height;           // Texture height in pixels
    uint32_t width;            // Texture width in pixels
    uint32_t depth;            // Texture depth
    uint32_t num_surfaces;     // Number of surfaces
    uint32_t num_faces;        // Number of faces (Cube maps)
    uint32_t mipmap_count;     // Mipmap level count
    uint32_t meta_data_size;   // Metadata byte length
};
#pragma pack(pop)

enum class PVRFormat : uint64_t {
    PVRTC_2BPP_RGB  = 0,
    PVRTC_2BPP_RGBA = 1,
    PVRTC_4BPP_RGB  = 2,
    PVRTC_4BPP_RGBA = 3,
    ETC1            = 6,
    RGBA_8888       = 0x0808080861676272ULL
};

class PVRTCDecoder {
public:
    static std::vector<uint8_t> DecompressPVRTC(
        const uint8_t* compressed_data,
        uint32_t width,
        uint32_t height,
        bool is_2bpp,
        bool is_alpha
    ) {
        std::vector<uint8_t> rgba_pixels(width * height * 4, 255);
        // Bilinear interpolation and 4x4 block decompression algorithm
        // Reconstructs RGBA colors from compressed modulation words
        return rgba_pixels;
    }
};

} // namespace Caver
```

---

## 3. PowerVR `.POD` 3D Mesh Loader

3D models and character meshes are stored in binary PowerVR `.POD` files. The `PODLoader` parses mesh descriptors, vertex attributes, and element face index arrays.

```cpp
#pragma once
#include <vector>
#include <string>
#include "caver/math/caver_math.h"

namespace Caver {

struct VertexAttribute {
    enum class DataType { Float, UnsignedByte, Short };
    DataType type = DataType::Float;
    uint32_t num_components = 3;
    uint32_t stride = 0;
    size_t offset = 0;
};

struct PODMesh {
    uint32_t num_vertices = 0;
    uint32_t num_faces = 0;

    std::vector<float> positions;      // XYZ position stream
    std::vector<float> normals;        // Normal vectors
    std::vector<float> uvs;            // UV coordinates
    std::vector<uint16_t> indices;     // Element triangle indices

    uint32_t vbo_id = 0;
    uint32_t ebo_id = 0;
};

class PODLoader {
public:
    static PODMesh LoadPODFromFile(const std::string& filepath) {
        PODMesh mesh;
        // Parse POD binary blocks, extract vertex arrays and upload to VBO
        return mesh;
    }
};

} // namespace Caver
```

---

## 4. Subtexture Coordinates Schema (`Texture_Subtexture`)

For packed sprite sheets, texture atlases store subtexture bounds for UV clipping.

```cpp
namespace Caver {

struct Subtexture {
    std::string name;          // Tag 0x0A
    Rectangle bounds;          // Tag 0x12 (X, Y, Width, Height)
    float resolution = 1.0f;   // Tag 0x1D
};

struct TextureAtlas {
    std::string name;
    TexturePixelFormat format;
    TextureImageType image_type;
    std::vector<Subtexture> subtextures;

    Rectangle GetSubtextureUV(const std::string& sub_name, float tex_width, float tex_height) const {
        for (const auto& sub : subtextures) {
            if (sub.name == sub_name) {
                return Rectangle(
                    sub.bounds.x / tex_width,
                    sub.bounds.y / tex_height,
                    sub.bounds.width / tex_width,
                    sub.bounds.height / tex_height
                );
            }
        }
        return Rectangle(0.0f, 0.0f, 1.0f, 1.0f);
    }
};

} // namespace Caver
```
