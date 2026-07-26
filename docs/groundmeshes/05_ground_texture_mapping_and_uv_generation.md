# Swordigo OpenSwordigo Research: TextureMappingComponent & Procedural UV Tiling

## 1. Procedural UV Coordinate Generation

`TextureMappingComponent` controls how 2D textures tile continuously across procedurally generated ground polygons without stretching or distortion.

---

## 2. UV Mapping Engine

```cpp
namespace Caver {

class GroundUVGenerator {
public:
    static void ProjectPlanarUVs(
        Mesh& mesh,
        const TextureMappingComponent& mapping,
        bool is_front_wall
    ) {
        size_t vertex_count = mesh.positions.size() / 3;
        mesh.uvs.resize(vertex_count * 2);

        float tile_scale = mapping.scale > 0.0f ? (1.0f / mapping.scale) : 1.0f;

        for (size_t i = 0; i < vertex_count; ++i) {
            float x = mesh.positions[i * 3 + 0];
            float y = mesh.positions[i * 3 + 1];
            float z = mesh.positions[i * 3 + 2];

            if (is_front_wall) {
                // Front cliff wall planar projection (X-Y plane)
                mesh.uvs[i * 2 + 0] = (x + mapping.offset.x) * tile_scale;
                mesh.uvs[i * 2 + 1] = (y + mapping.offset.y) * tile_scale;
            } else {
                // Top surface planar projection along polyline length
                mesh.uvs[i * 2 + 0] = (x + mapping.offset.x) * tile_scale;
                mesh.uvs[i * 2 + 1] = (z + mapping.offset.y) * tile_scale;
            }
        }
    }
};

} // namespace Caver
```

---

## 3. Boulder Engine Extractive Research: Texture Mapping Scale & Offsets

The reverse-engineered Boulder engine specifies global texture space constants used when projecting planar UV coordinates for front cliff faces and side ground meshes:

```cpp
namespace Caver {

constexpr float TEXTURE_SPACE_FACTOR  = 0.004f; // 1.0 / 250.0 Scale
constexpr float TEXTURE_SPACE_XOFFSET = 0.5f;   // Horizontal UV center offset
constexpr float TEXTURE_SPACE_YOFFSET = 0.5f;   // Vertical UV center offset

inline Vector2 CalculateFrontFaceUV(float x, float y) {
    return Vector2(
        (x * TEXTURE_SPACE_FACTOR) + TEXTURE_SPACE_XOFFSET,
        (y * TEXTURE_SPACE_FACTOR) + TEXTURE_SPACE_YOFFSET
    );
}

inline Vector2 CalculateSideMeshUV(float distance_along_polyline, float depth) {
    return Vector2(
        distance_along_polyline * TEXTURE_SPACE_FACTOR,
        (depth * TEXTURE_SPACE_FACTOR) + TEXTURE_SPACE_YOFFSET
    );
}

} // namespace Caver
```
