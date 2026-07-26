# Swordigo OpenSwordigo Research: Ground Polygon Geometry & Vertex Polyline Systems

## 1. Ground Polygon Representation

Ground geometry begins with 2D boundary polylines stored in `GroundPolygonComponent`. A ground polygon consists of an ordered sequence of 2D control vertices (`Vector2`) defining the outline of terrain segments, platform tops, or cavern walls.

---

## 2. Polyline Structure and Data Types

```cpp
namespace Caver {

struct GroundPolylineVertex {
    Vector2 position;         // 2D Coordinates (X, Y)
    Vector2 normal;           // Outward edge normal vector
    float edge_length;        // Distance to next vertex
    bool is_corner;           // Sharp corner flag
};

class GroundPolyline {
public:
    std::vector<GroundPolylineVertex> vertices;
    bool is_closed = false;    // Closed loop vs open terrain strip
    bool is_convex = false;    // Optimization flag for triangulation

    void CalculateNormals() {
        size_t count = vertices.size();
        if (count < 2) return;

        for (size_t i = 0; i < count; ++i) {
            size_t next_idx = (i + 1) % count;
            if (!is_closed && next_idx == 0) break;

            Vector2 edge = vertices[next_idx].position - vertices[i].position;
            vertices[i].edge_length = edge.Length();
            
            // Perpendicular outward normal (-Ny, Nx)
            Vector2 perp(-edge.y, edge.x);
            vertices[i].normal = perp.Normalized();
        }
    }
};

} // namespace Caver
```

---

## 3. Depth Boundaries and Friction Parameters

Ground polygons carry depth ranges (`min_depth` and `max_depth`) that govern the 3D extrusion thickness along the Z axis, as well as physical surface friction:

```cpp
namespace Caver {

struct SurfaceProperties {
    float min_depth;     // Extrusion back face Z position
    float max_depth;     // Extrusion front face Z position
    float friction;      // Surface friction coefficient (Ice = 0.05, Normal = 1.0, Mud = 2.5)
    bool unsafe_ground;  // Hazard flag (Spikes, Lava, Poison)

    float GetDepthThickness() const {
        return std::abs(max_depth - min_depth);
    }
};

} // namespace Caver
```
