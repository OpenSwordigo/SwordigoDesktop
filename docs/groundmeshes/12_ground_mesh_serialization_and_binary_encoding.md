# Swordigo OpenSwordigo Research: GroundMesh Protobuf Binary Encoding & Storage

## 1. Protobuf Tag Encoding Reference Table

The table below lists all protobuf binary field tags for `GroundPolygonComponent`, `GroundMeshComponent`, `GroundMeshGeneratorComponent`, and `TextureMappingComponent`:

| Message Class | Field Tag | Wire Type | Field Name | Data Description |
| :--- | :--- | :--- | :--- | :--- |
| `GroundPolygonComponent` | `0x0A` | LengthDelimited | `Vertex` | Repeated `Vector2` polyline vertices |
| | `12` | LengthDelimited | `Polygon` | Embedded `Polygon` message |
| | `18` | Varint | `Collides` | Collision boolean toggle |
| | `25` | Bit32 | `MinDepth` | Minimum Z depth floating point value |
| | `2D` | Bit32 | `MaxDepth` | Maximum Z depth floating point value |
| | `32` | LengthDelimited | `OnCollide` | Embedded Lua script `Program` |
| | `3D` | Bit32 | `Friction` | Surface friction scalar |
| | `40` | Varint | `UnsafeGround` | Unsafe respawn boundary toggle |
| `GroundMeshComponent` | `22` | LengthDelimited | `VertexData` | Raw binary vertex byte buffer |
| | `2A` | LengthDelimited | `Indices` | Raw binary index byte buffer |
| | `32` | LengthDelimited | `Mesh` | Master `Mesh` structure |
| | `3A` | LengthDelimited | `LocalAABB` | Bounds `Rectangle` |
| | `42` | LengthDelimited | `SurfaceMesh` | Surface cap `Mesh` |
| | `4A` | LengthDelimited | `FrontMesh` | Front cliff face `Mesh` |
| | `52` | LengthDelimited | `Color` | `FloatColor` RGBA tint |
| | `58` | Varint | `Transparent` | Alpha blending flag |
| `GroundMeshGeneratorComponent` | `08` | Varint | `GroundPolygonId` | Input `GroundPolygonComponent` ID |
| | `10` | Varint | `TargetMeshId` | Output `GroundMeshComponent` ID |
| | `18` | Varint | `FrontTextureMappingId` | Front cliff UV mapping ID |
| | `20` | Varint | `SurfaceTextureMappingId`| Top surface UV mapping ID |
| | `28` | Varint | `RandomSeed` | Pseudorandom noise seed |
| | `35` | Bit32 | `HorizNoise` | Edge jitter noise amplitude |
| | `38` | Varint | `MeshType` | Style enum (`0:PLAIN`, `1:ROUNDED_HAT`) |
| | `45` | Bit32 | `SurfaceWidth` | Top cap surface thickness |
| | `4D` | Bit32 | `HatHeight` | Bevel height |
| `TextureMappingComponent` | `0x0A` | LengthDelimited | `TextureName` | Diffuse texture asset string |
| | `15` | Bit32 | `Scale` | Tiling scale multiplier |
| | `1A` | LengthDelimited | `Offset` | Origin offset `Vector2` |

---

## 2. C++ Binary Stream Encoder Implementation

```cpp
#pragma once
#include <vector>
#include <cstdint>
#include "caver/components/ground_polygon_component.h"

namespace Caver {

class GroundMeshSerializer {
public:
    static std::vector<uint8_t> SerializePolygonComponent(const GroundPolygonComponent& poly) {
        std::vector<uint8_t> buffer;
        // Encode Protobuf tags and varints into binary output stream
        return buffer;
    }
};

} // namespace Caver
```

---

## 3. Boulder Engine Extractive Research: Text Protobuf Template Encoding

In text-based scene file representations (FileRift format), ground meshes, collision shapes, generators, and texture mappings are encoded as human-readable Protobuf block definitions:

```protobuf
// Extracted Ground Mesh Text Protobuf Block Template
Component {
    ClassName : 'GroundPolygon'
    Identifier : 980
    GroundPolygonComponent {
        Polygon {
            Vertex { X: -2.60 Y: -2.15 }
            Vertex { X: -2.28 Y: -1.31 }
            Vertex { X: -1.45 Y: -2.25 }
            Convex : 0
            Closed : 1
        }
        Collides : 1
        MinDepth : -45.0
        MaxDepth : 45.0
    }
}

Component {
    ClassName : 'GroundMesh'
    Identifier : 981
    GroundMeshComponent {
        LocalAabb { X: -2.60 Y: -2.89 Z: -50.0 Width: 3.15 Height: 1.58 Depth: 100.0 }
        SurfaceMesh {
            NumVertices : 20
            NumFaces : 6
            Indices { ValueType : 4 ValuesPerVertex : 1 Stride : 2 DataOffset : 0 }
            Vertices { ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 0 }
            Normals { ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 12 }
            TexCoordSet { ValueType : 7 ValuesPerVertex : 2 Stride : 32 DataOffset : 24 }
            Material {
                AmbientColor { R : 1.0 G : 1.0 B : 1.0 A : 1.0 }
                DiffuseColor { R : 1.0 G : 1.0 B : 1.0 A : 1.0 }
                SpecularColor { R : 1.0 G : 1.0 B : 1.0 A : 1.0 }
                Shininess : 0.0
                Texture { Name : 'graveyard_grass_2x' PixelFormat : 1 ImageType : 2 }
            }
            BoundingBox { X: -2.60 Y: -2.89 Z: -50.0 Width: 3.15 Height: 1.58 Depth: 100.0 }
            VertexData : "\x00\x00\x00..."
            IndexData : "\x00\x00\x01..."
        }
        FrontMesh {
            NumVertices : 3
            NumFaces : 1
            Vertices { ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 0 }
            Normals { ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 12 }
            TexCoordSet { ValueType : 7 ValuesPerVertex : 2 Stride : 32 DataOffset : 24 }
            Material {
                Texture { Name : 'graveyard_ground_2x' PixelFormat : 1 ImageType : 2 }
            }
            BoundingBox { X: -2.60 Y: -2.89 Z: -50.0 Width: 3.15 Height: 1.58 Depth: 100.0 }
            VertexData : "\x00\x00\x00..."
        }
        Color { R : 1.0 G : 1.0 B : 1.0 A : 1.0 }
    }
}

Component {
    ClassName : 'GroundMeshGenerator'
    Identifier : 982
    GroundMeshGeneratorComponent {
        GroundPolygonId : 980
        TargetMeshId : 981
        FrontTextureMappingId : 985
        SurfaceTextureMappingId : 984
        RandomSeed : 1291618994
        HorizNoise : 0.0
        MeshType : 1
        SurfaceWidth : 80.0
        HatHeight : 25.0
        HatWidthOffset1 : 5.0
        HatWidthOffset2 : 5.0
    }
}

Component {
    ClassName : 'CollisionShape'
    Identifier : 983
    ParentComponentIdentifier : 980
    ShapeComponent {
        Polygon {
            Vertex { X: -2.60 Y: -2.15 }
            Vertex { X: -2.28 Y: -1.31 }
            Vertex { X: -1.45 Y: -2.25 }
            Convex : 0
            Closed : 1
        }
    }
    CollisionShapeComponent {
        IsGround : 1
        MinDepth : -45.0
        MaxDepth : 45.0
        Enabled : 1
    }
}
```
