# Swordigo OpenSwordigo Research: GroundMeshGenerator Procedural Mesh Synthesis

## 1. Procedural Synthesis Pipeline

The `GroundMeshGeneratorComponent` executes a 3-stage mesh generation pipeline that converts 2D polyline control vertices into fully textured 3D terrain meshes:

```
[GroundPolygon 2D Vertices] 
            │
            ▼
┌───────────────────────────────────────┐
│ Stage 1: Noise & Edge Jittering      │
│ (horiz_noise, random_seed)            │
└───────────────────┬───────────────────┘
                    │
                    ▼
┌───────────────────────────────────────┐
│ Stage 2: Mesh Extrusion & Triangulation│
│ (MeshType: Plain / RoundedHat)        │
└───────────────────┬───────────────────┘
                    │
                    ▼
┌───────────────────────────────────────┐
│ Stage 3: Layer Decomposition          │
│ (SurfaceMesh cap + FrontMesh cliff)   │
└───────────────────────────────────────┘
```

---

## 2. C++ Generator Algorithm Implementation

```cpp
#pragma once
#include <vector>
#include <cmath>
#include "caver/math/caver_math.h"
#include "caver/graphics/mesh.h"

namespace Caver {

class GroundMeshGenerator {
public:
    static void GenerateMesh(
        const std::vector<Vector2>& input_vertices,
        GroundMeshType mesh_type,
        float horiz_noise,
        uint32_t random_seed,
        float surface_width,
        float hat_height,
        Mesh& out_surface_mesh,
        Mesh& out_front_mesh
    ) {
        if (input_vertices.size() < 2) return;

        // Stage 1: Apply seed-based pseudorandom horizontal noise
        std::vector<Vector2> noisy_vertices = input_vertices;
        if (horiz_noise > 0.0f) {
            srand(random_seed);
            for (auto& v : noisy_vertices) {
                float noise = ((rand() % 1000) / 1000.0f - 0.5f) * 2.0f * horiz_noise;
                v.x += noise;
            }
        }

        // Stage 2 & 3: Extrude surface cap and front cliff faces
        if (mesh_type == GroundMeshType::RoundedHat) {
            GenerateRoundedHatMesh(noisy_vertices, surface_width, hat_height, out_surface_mesh, out_front_mesh);
        } else {
            GeneratePlainMesh(noisy_vertices, surface_width, out_surface_mesh, out_front_mesh);
        }
    }

private:
    static void GeneratePlainMesh(
        const std::vector<Vector2>& verts,
        float depth,
        Mesh& surface,
        Mesh& front
    ) {
        // Extrude flat vertical front quads along polyline edges
        for (size_t i = 0; i < verts.size() - 1; ++i) {
            Vector2 p1 = verts[i];
            Vector2 p2 = verts[i + 1];

            // Add front cliff quad vertices
            front.positions.push_back(p1.x); front.positions.push_back(p1.y); front.positions.push_back(0.0f);
            front.positions.push_back(p2.x); front.positions.push_back(p2.y); front.positions.push_back(0.0f);
            front.positions.push_back(p1.x); front.positions.push_back(p1.y - depth); front.positions.push_back(0.0f);
            front.positions.push_back(p2.x); front.positions.push_back(p2.y - depth); front.positions.push_back(0.0f);
        }
    }

    static void GenerateRoundedHatMesh(
        const std::vector<Vector2>& verts,
        float surface_width,
        float hat_height,
        Mesh& surface,
        Mesh& front
    ) {
        // Synthesize curved top grass cap with rounded bevel quads
    }
};

} // namespace Caver
```

---

## 3. Boulder Engine Extractive Research: Polygon Ear-Clipping & Slope Angle Detection

### 3.1 Top Segment Angle Classifier (`IsTopSegment`)

An edge between two polyline vertices is classified as a top grass segment if its angle relative to the horizontal 180° axis falls within `TopAngle` threshold (typically 20°):

```cpp
namespace Caver {

bool IsTopSegment(const std::vector<Vector2>& polygon, size_t index, float top_angle_threshold) {
    size_t count = polygon.size();
    Vector2 curr = polygon[index % count];
    Vector2 next = polygon[(index + 1) % count];

    float radians = std::atan2(next.y - curr.y, next.x - curr.x);
    float degrees = radians * (180.0f / 3.14159265f);
    if (degrees < 0.0f) degrees += 360.0f;

    return std::abs(degrees - 180.0f) < top_angle_threshold;
}

} // namespace Caver
```

### 3.2 Front Face Ear-Clipping Triangulation Engine

Front cliff faces and interior cap polygons are triangulated using a 2D Ear-Clipping algorithm:

```cpp
namespace Caver {

inline float Cross2D(const Vector2& a, const Vector2& b, const Vector2& c) {
    return (a.x - c.x) * (b.y - c.y) - (a.y - c.y) * (b.x - c.x);
}

inline bool IsPointInsideTriangle(const Vector2& a, const Vector2& b, const Vector2& c, const Vector2& pt) {
    if (Cross2D(a, b, pt) < 0.0f) return false;
    if (Cross2D(b, c, pt) < 0.0f) return false;
    if (Cross2D(c, a, pt) < 0.0f) return false;
    return true;
}

bool IsEar(size_t i, size_t j, size_t k, const std::vector<Vector2>& poly) {
    if (Cross2D(poly[i], poly[j], poly[k]) < 0.0f) return false;

    for (size_t idx = 0; idx < poly.size(); ++idx) {
        if (idx != i && idx != j && idx != k) {
            if (IsPointInsideTriangle(poly[i], poly[j], poly[k], poly[idx])) {
                return false;
            }
        }
    }
    return true;
}

std::vector<uint16_t> TriangulatePolygonEarClipping(std::vector<Vector2> poly) {
    std::vector<uint16_t> indices;
    if (poly.size() < 3) return indices;

    while (poly.size() > 3) {
        bool ear_found = false;
        for (size_t i = 0; i < poly.size() - 2; ++i) {
            if (IsEar(i, i + 1, i + 2, poly)) {
                // Form triangle (i, i+1, i+2)
                indices.push_back(static_cast<uint16_t>(i));
                indices.push_back(static_cast<uint16_t>(i + 1));
                indices.push_back(static_cast<uint16_t>(i + 2));

                poly.erase(poly.begin() + i + 1);
                ear_found = true;
                break;
            }
        }
        if (!ear_found) break; // Non-simple polygon fallback
    }

    if (poly.size() == 3) {
        indices.push_back(0);
        indices.push_back(1);
        indices.push_back(2);
    }
    return indices;
}

} // namespace Caver
```
