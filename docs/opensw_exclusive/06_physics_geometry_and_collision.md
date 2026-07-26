# Swordigo OpenSwordigo Research: Geometry Primitives, Collision Solvers & Physics Objects

## 1. Geometric Primitives Schema Mappings

The physics and spatial query systems in OpenSwordigo operate on native geometric primitives decoded from binary Protobuf tags:

| Primitive | Protobuf Tag Structure | Description |
| :--- | :--- | :--- |
| `Vector2` | `0x0D` (X), `0x15` (Y) | 2D float point or vector component |
| `Vector3` | `0x0D` (X), `0x15` (Y), `0x1D` (Z) | 3D float point or spatial vector |
| `Rectangle` | `0x0D` (X), `0x15` (Y), `0x1D` (Width), `0x25` (Height) | 2D Axis-Aligned Bounding Box (AABB) |
| `Box` | `0x0D` (X), `0x15` (Y), `0x1D` (Z), `0x25` (W), `0x2D` (H), `0x35` (D) | 3D Axis-Aligned Bounding Box |
| `Circle` | `0x0A` (Center Vector2), `0x15` (Radius) | 2D Bounding Circle |
| `Polygon` | `0x0A` (Repeated Vertices), `0x10` (Convex), `0x18` (Closed) | Polyline ground segments & static collision geometry |

---

## 2. C++ Geometry Math Core (`caver_math.h`)

```cpp
#pragma once
#include <cmath>
#include <vector>
#include <algorithm>

namespace Caver {

struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;

    Vector2() = default;
    Vector2(float _x, float _y) : x(_x), y(_y) {}

    Vector2 operator+(const Vector2& v) const { return { x + v.x, y + v.y }; }
    Vector2 operator-(const Vector2& v) const { return { x - v.x, y - v.y }; }
    Vector2 operator*(float s) const { return { x * s, y * s }; }
    Vector2 operator/(float s) const { return { x / s, y / s }; }

    float Dot(const Vector2& v) const { return x * v.x + y * v.y; }
    float Cross(const Vector2& v) const { return x * v.y - y * v.x; }
    float LengthSq() const { return x * x + y * y; }
    float Length() const { return std::sqrt(LengthSq()); }

    Vector2 Normalized() const {
        float len = Length();
        return len > 0.0f ? *this / len : Vector2(0.0f, 0.0f);
    }
};

struct Rectangle {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    Rectangle() = default;
    Rectangle(float _x, float _y, float _w, float _h) : x(_x), y(_y), width(_w), height(_h) {}

    bool Intersects(const Rectangle& r) const {
        return (x < r.x + r.width && x + width > r.x &&
                y < r.y + r.height && y + height > r.y);
    }

    bool Contains(const Vector2& p) const {
        return (p.x >= x && p.x <= x + width && p.y >= y && p.y <= y + height);
    }
};

struct Polygon {
    std::vector<Vector2> vertices;
    bool is_convex = false;
    bool is_closed = true;
};

} // namespace Caver
```

---

## 3. Physics Object Component (`Caver::PhysicsObjectComponent`)

The `PhysicsObjectComponent` binds rigid body dynamics (velocity, mass, friction, bounce restitution) and collision hulls to entities.

```cpp
#include "caver/scene/component.h"
#include "caver/math/caver_math.h"

namespace Caver {

enum class BodyType {
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2
};

class PhysicsObjectComponent : public Component {
public:
    BodyType body_type = BodyType::Dynamic;
    float mass = 1.0f;
    float friction = 0.2f;
    float restitution = 0.0f; // Bounciness

    Vector2 velocity{ 0.0f, 0.0f };
    Vector2 force_accumulator{ 0.0f, 0.0f };
    Rectangle collision_box{ -0.5f, 0.0f, 1.0f, 2.0f };

    bool is_grounded = false;

    const char* GetComponentType() const override { return "PhysicsObjectComponent"; }

    void ApplyForce(const Vector2& force) {
        if (body_type == BodyType::Dynamic) {
            force_accumulator = force_accumulator + force;
        }
    }

    void OnUpdate(float dt) override {
        if (body_type != BodyType::Dynamic || !m_owner) return;

        // Gravity integration
        Vector2 gravity(0.0f, -9.81f * 2.0f);
        Vector2 acceleration = gravity + (force_accumulator / mass);

        velocity = velocity + (acceleration * dt);
        m_owner->position = m_owner->position + (velocity * dt);

        force_accumulator = Vector2(0.0f, 0.0f);
    }
};

} // namespace Caver
```

---

## 4. Ground Polyline & Terrain Raycast Queries

Collision resolution against static ground meshes tests character raycasts against ground polylines (`Polygon`).

```cpp
namespace Caver {

struct RaycastResult {
    bool hit = false;
    float distance = 0.0f;
    Vector2 point{ 0.0f, 0.0f };
    Vector2 normal{ 0.0f, 0.0f };
};

class TerrainCollisionSolver {
public:
    static RaycastResult RaycastGroundSegment(
        const Vector2& ray_origin,
        const Vector2& ray_dir,
        float max_dist,
        const Polygon& ground_poly
    ) {
        RaycastResult best_result;
        best_result.distance = max_dist;

        for (size_t i = 0; i < ground_poly.vertices.size() - 1; ++i) {
            Vector2 p1 = ground_poly.vertices[i];
            Vector2 p2 = ground_poly.vertices[i + 1];

            // Line segment raycast intersection algorithm
        }
        return best_result;
    }
};

} // namespace Caver
```
