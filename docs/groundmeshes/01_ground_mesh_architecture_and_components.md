# Swordigo OpenSwordigo Research: GroundMesh Architecture & Component Hierarchy

## 1. System Overview

In the **Caver Engine**, terrain and environment geometry are constructed using a specialized component-driven ground mesh architecture. Rather than placing monolithic static 3D models for all level terrain, the engine proceduralizes level floors, cliffs, platforms, and slopes via interconnected components attached to `SceneObject` nodes.

---

## 2. Ground Mesh Component Taxonomy

The ground mesh system consists of four primary C++ component classes that work in tandem:

```
                          +-------------------------------+
                          |     Caver::SceneObject        |
                          +---------------+---------------+
                                          |
        +---------------------------------+---------------------------------+
        |                                 |                                 |
+-------v-----------------------+ +-------v-----------------------+ +-------v-----------------------+
|    GroundPolygonComponent     | | GroundMeshGeneratorComponent  | |      GroundMeshComponent      |
|  - Defines polyline boundary  | |  - Procedural mesh parameters | |  - Renderable mesh VBO/EBO  |
|  - Stores physics collision   | |  - Hat & slope algorithms     | |  - Surface & Front Mesh data|
+-------------------------------+ +-------------------------------+ +-------------------------------+
                                                  |
                                  +---------------+---------------+
                                  |    TextureMappingComponent    |
                                  |  - Tiling scale & UV offset   |
                                  +-------------------------------+
```

---

## 3. C++ Component Interface Definitions

### 3.1 `Caver::GroundPolygonComponent`

Stores the raw 2D polyline vertices, collision flags, depth boundaries, surface friction, and collision callbacks.

```cpp
#pragma once
#include <vector>
#include <string>
#include "caver/scene/component.h"
#include "caver/math/caver_math.h"
#include "caver/resource/program.h"

namespace Caver {

class GroundPolygonComponent : public Component {
public:
    Polygon polygon;                 // Field Tag 12: 2D vertex polyline
    std::vector<Vector2> vertices;   // Field Tag 0x0A: Raw vertex list
    bool collides = true;            // Field Tag 18: Collision flag
    bool unsafe_ground = false;      // Field Tag 40: Hazard ground (spikes/lava)
    float min_depth = -10.0f;        // Field Tag 25: Minimum z-depth boundary
    float max_depth = 10.0f;         // Field Tag 2D: Maximum z-depth boundary
    float friction = 1.0f;           // Field Tag 3D: Surface friction multiplier
    Program on_collide_program;      // Field Tag 32: Lua collision script

    const char* GetComponentType() const override { return "GroundPolygonComponent"; }
};

} // namespace Caver
```

### 3.2 `Caver::GroundMeshGeneratorComponent`

Contains configuration parameters used by the engine to procedurally synthesize 3D geometry from a `GroundPolygonComponent`.

```cpp
#pragma once
#include "caver/scene/component.h"

namespace Caver {

enum class GroundMeshType : uint32_t {
    Plain      = 0, // Flat extruded block terrain
    RoundedHat = 1  // Beveled top grass hat layer with rounded corners
};

class GroundMeshGeneratorComponent : public Component {
public:
    uint32_t ground_polygon_id = 0;        // Tag 0x08: Linked GroundPolygonComponent ID
    uint32_t target_mesh_id = 0;           // Tag 10: Output GroundMeshComponent ID
    uint32_t front_texture_mapping_id = 0; // Tag 18: Cliff front UV mapping ID
    uint32_t surface_texture_mapping_id=0; // Tag 20: Top surface UV mapping ID
    
    GroundMeshType mesh_type = GroundMeshType::Plain; // Tag 38: Mesh style enum
    uint32_t random_seed = 1337;           // Tag 28: Seed for noise generation
    float horiz_noise = 0.0f;              // Tag 35: Horizontal edge jitter noise
    float surface_width = 1.0f;            // Tag 45: Top cap surface thickness
    float hat_height = 0.5f;               // Tag 4D: Rounded hat bevel height
    float hat_width_offset_1 = 0.0f;       // Tag 55: Left bevel offset
    float hat_width_offset_2 = 0.0f;       // Tag 5D: Right bevel offset

    const char* GetComponentType() const override { return "GroundMeshGeneratorComponent"; }
};

} // namespace Caver
```

### 3.3 `Caver::GroundMeshComponent`

Contains the generated mesh buffers, vertex positions, UV coordinates, normals, colors, and local AABB bounds ready for rendering.

```cpp
#pragma once
#include <vector>
#include "caver/scene/component.h"
#include "caver/graphics/mesh.h"
#include "caver/math/caver_math.h"

namespace Caver {

class GroundMeshComponent : public Component {
public:
    Mesh mesh;                       // Tag 32: Combined master mesh
    Mesh surface_mesh;               // Tag 42: Top surface/cap mesh (grass/dirt)
    Mesh front_mesh;                 // Tag 4A: Front cliff/wall face mesh
    
    std::vector<uint8_t> vertex_data;// Tag 22: Raw packed vertex buffer
    std::vector<uint8_t> indices;    // Tag 2A: Raw index buffer
    FloatColor color{ 1, 1, 1, 1 };  // Tag 52: RGBA tint color
    bool transparent = false;        // Tag 58: Alpha blend flag
    Rectangle local_aabb;            // Tag 3A: Axis-aligned bounding box

    const char* GetComponentType() const override { return "GroundMeshComponent"; }
};

} // namespace Caver
```

### 3.4 `Caver::TextureMappingComponent`

Defines UV scale and origin offsets for texture tiling on ground surfaces.

```cpp
#pragma once
#include <string>
#include "caver/scene/component.h"
#include "caver/math/caver_math.h"

namespace Caver {

class TextureMappingComponent : public Component {
public:
    std::string texture_name;        // Tag 0x0A: Diffuse texture asset name
    float scale = 1.0f;              // Tag 15: UV tiling frequency multiplier
    Vector2 offset{ 0.0f, 0.0f };    // Tag 1A: UV texture scroll/origin offset

    const char* GetComponentType() const override { return "TextureMappingComponent"; }
};

} // namespace Caver
```

---

## 4. Boulder Engine Extractive Research: Component Linkage & Default Attributes

The reverse-engineered Boulder mesh generator specifies exact standard component IDs and default generation parameters used across Touch Foo level files:

| Component | Identifier Code | Target Linked Component | Key Parameters & Standard Constants |
| :--- | :--- | :--- | :--- |
| `GroundPolygon` | `980` | None | `MinDepth: -45.0`, `MaxDepth: 45.0`, `Collides: 1`, `Convex: 0`, `Closed: 1` |
| `GroundMesh` | `981` | None | `LocalAabb`, `SurfaceMesh` (Top & Side), `FrontMesh`, `Shininess: 0.0` |
| `GroundMeshGenerator` | `982` | `GroundPolygonId: 980`, `TargetMeshId: 981` | `MeshType: 1` (RoundedHat), `SurfaceWidth: 80.0`, `HatHeight: 25.0`, `HatWidthOffset1/2: 5.0`, `RandomSeed: 1291618994` |
| `CollisionShape` | `983` | `ParentComponentIdentifier: 980` | `IsGround: 1`, `MinDepth: -45.0`, `MaxDepth: 45.0`, `Enabled: 1` |
| `TextureMapping` (Surface)| `984` | Top Texture | `Scale: 250.0`, `Offset: {0, 0}` |
| `TextureMapping` (Front)  | `985` | Bottom/Front Texture | `Scale: 250.0`, `Offset: {0, 0}` |
