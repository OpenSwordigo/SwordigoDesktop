# Swordigo OpenSwordigo Research: Surface Mesh & Front Mesh Layer Split

## 1. Dual Layer Architecture

To achieve clean visual distinction between top ground surfaces (grass, dirt walkways, stone steps) and vertical cliff walls, `GroundMeshComponent` separates geometry into two distinct mesh layers:

1. **`SurfaceMesh`**: Renders horizontal or sloped top caps with grass/dirt tile textures.
2. **`FrontMesh`**: Renders vertical cliff walls and underground rock faces facing the camera.

---

## 2. Layer Attributes and Material Binding

```cpp
namespace Caver {

struct GroundMeshLayers {
    Mesh surface_mesh;   // Top surface geometry (Grass cap)
    Mesh front_mesh;     // Vertical front wall geometry (Cliff face)

    std::string surface_texture; // Texture mapping for surface cap
    std::string front_texture;   // Texture mapping for front wall

    void Draw(RenderingContext& ctx) {
        // Pass 1: Draw vertical front cliff faces
        if (!front_mesh.indices.empty()) {
            ctx.BindTextureByName(front_texture);
            front_mesh.DrawGL();
        }

        // Pass 2: Draw top surface caps with depth offset
        if (!surface_mesh.indices.empty()) {
            ctx.BindTextureByName(surface_texture);
            surface_mesh.DrawGL();
        }
    }
};

} // namespace Caver
```

---

## 3. Vertex Data Layout (`VertexData` & `Indices`)

Raw binary streams store packed 3D vertex buffers containing position, normal, UV, and vertex color components:

```cpp
namespace Caver {

#pragma pack(push, 1)
struct GroundVertexPacked {
    float position[3]; // X, Y, Z coordinates
    float normal[3];   // Nx, Ny, Nz normal vector
    float uv[2];       // U, V texture coordinates
    uint8_t color[4];  // R, G, B, A vertex color tint
};
#pragma pack(pop)

static_assert(sizeof(GroundVertexPacked) == 36, "GroundVertexPacked must be 36 bytes");

} // namespace Caver
```

---

## 4. Boulder Engine Extractive Research: 32-Byte Vertex Stride & Index Data Layout

The binary layout used in `GroundMeshComponent` for `SurfaceMesh` and `FrontMesh` specifies a 32-byte stride per vertex:

### 4.1 Vertex Attribute Memory Layout (32-Byte Stride)
- **Position (`ValueType: 7` / Float)**: 3 components, Offset `0` (12 Bytes: `X`, `Y`, `Z`)
- **Normal (`ValueType: 7` / Float)**: 3 components, Offset `12` (12 Bytes: `Nx`, `Ny`, `Nz`)
- **TexCoordSet (`ValueType: 7` / Float)**: 2 components, Offset `24` (8 Bytes: `U`, `V`)

```cpp
#pragma once
#include <cstdint>

namespace Caver {

#pragma pack(push, 1)
struct GroundVertex32Byte {
    float x, y, z;      // Bytes 0-11: 3D Position
    float nx, ny, nz;   // Bytes 12-23: Normal Vector
    float u, v;         // Bytes 24-31: Texture UV Coordinates
};
#pragma pack(pop)

static_assert(sizeof(GroundVertex32Byte) == 32, "GroundVertex32Byte must be exactly 32 bytes");

} // namespace Caver
```

### 4.2 Top Segment Vertex Extrusion & Z-Fighting Prevention

To prevent Z-fighting visual artifacts between the top surface cap and front cliff faces, top segment vertices apply a small vertical offset (`+0.05` Y offset):

```cpp
namespace Caver {

// 20 Vertices generated per top segment with specific depth boundaries
void ConstructTopSegmentVertices(
    float left, float right,
    float left_height, float right_height,
    float min_depth, float max_depth,
    float u_offset,
    std::vector<GroundVertex32Byte>& out_vertices
) {
    // Elevate surface height slightly to prevent z-fighting clipping
    left_height += 0.05f;
    right_height += 0.05f;

    // Build 20 packed vertices with up, down, and forward normals...
}

} // namespace Caver
```
