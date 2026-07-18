# Ground Mesh Format and Procedural Generation

This document details the binary representation and procedural generation algorithms for ground meshes used in the game engine.

---

## 1. Serialization Schema (Protobuf-style format)

The engine stores game scene objects and components using a schema-driven, tag-length-delimited binary serialization format matching Google Protocol Buffers. 

In this format, each field has an encoded tag value:
$$\text{tag} = (\text{field\_number} \ll 3) \mid \text{wire\_type}$$

The table below lists the component field numbers and hexadecimal keys (based on tag values):

| Component / Type | Field Number | Tag Hex Key | Description |
| :--- | :--- | :--- | :--- |
| `GROUND_POLYGON` | 110 | - | Contains the 2D contour vertices |
| `GROUND_MESH` | 111 | - | Renders the generated 3D ground |
| `GROUND_MESH_GENERATOR` | 112 | - | Settings for procedural mesh building |
| `TEXTURE_MAPPING` | 113 | - | Texture asset reference, scale, and offset |

### Submesh / Mesh Message Fields
A submesh (e.g., tag 8 or 9 inside a `GROUND_MESH` component) is serialized as a `Mesh` type message:
- **`NumVertices`** (Field 1, hex key `08`): Total vertices.
- **`VertexData`** (Field 50, hex key `192`): Interleaved binary buffer of vertex attributes (wire type 2: length-delimited).
- **`IndexData`** (Field 51, hex key `19a`): Binary buffer of vertex indices (wire type 2: length-delimited).
- **`Material`** (Field 10, hex key `52`): Nested material configuration.

---

## 2. Vertex Layout

The binary buffer for **`VertexData`** contains interleaved attributes for each vertex. Each vertex has a stride of **32 bytes** (8 floats):

| Attribute | Components | Offset (Bytes) | Data Type | Description |
| :--- | :--- | :--- | :--- | :--- |
| `position` | 3 (X, Y, Z) | 0 | `Float32` | 3D spatial position |
| `normal` | 3 (X, Y, Z) | 12 | `Float32` | Normal vector for lighting |
| `uv` | 2 (U, V) | 24 | `Float32` | Texture coordinates |

The **`IndexData`** is a flat `Uint16Array` containing 3 indices per face (triangle).

---

## 3. Ground Mesh Generator

Instead of loading static files, ground geometry is generated procedurally by the `GroundMeshGenerator` component using a 2D contour definition (`GroundPolygon`).

### Generator Configuration Fields
- **`GroundPolygonId`** (Field 1): ID of the input polygon.
- **`TargetMeshId`** (Field 10): Output `GROUND_MESH` component ID.
- **`FrontTextureMappingId`** (Field 18): Texture mapping reference for the front face.
- **`SurfaceTextureMappingId`** (Field 20): Texture mapping reference for the top/surface face.
- **`RandomSeed`** (Field 28): Random seed for procedural noise.
- **`HorizNoise`** (Field 35): Horizontal noise amplitude applied to vertices.
- **`MeshType`** (Field 38): Mesh profile type:
  - `0`: `PLAIN` (flat top extrusion)
  - `1`: `ROUNDED_HAT` (adds a rounded cap/ledge profile)
- **`SurfaceWidth`** (Field 45): Width of the top surface.
- **`HatHeight`** (Field 9): Vertical thickness of the rounded cap ledge.
- **`HatWidthOffset1`** (Field 11): Ledge overhang offset (front).
- **`HatWidthOffset2`** (Field 12): Ledge overhang offset (back).

---

## 4. Mesh Generation Logic

The procedural generator extrudes a 3D volume along a 2D path:

1. **Polygon Parsing**: Vertices are parsed from the `GroundPolygon` component. Vertices are stored as nested messages where each vertex has `X` (Field 1) and `Y` (Field 2) coordinates.
2. **Noise Application**: Based on `RandomSeed` and `HorizNoise`, horizontal shifts are applied to the vertices to create uneven, organic rock walls.
3. **Extrusion & Profile Sweeping**:
   - For `PLAIN` meshes: It extrudes flat faces along the polygon segments.
   - For `ROUNDED_HAT` meshes: It generates a multi-segment cap profile using the `HatHeight` and `HatWidthOffset` dimensions.
4. **UV Projection mapping**:
   UV coordinates $(u, v)$ are projected based on the `Scale`, `Offset`, and spatial coordinates of each vertex:
   $$u = \frac{x_{\text{world}} - \text{offX}}{\text{scale}} + 0.5$$
   $$v = \frac{y_{\text{world}} - \text{offY}}{\text{scale}} + 0.5$$
5. **Component Assembly**:
   - The generator splits the generated geometry into **front meshes** (tag 9 / 6, front wall) and **surface meshes** (tag 8, top ledge).
   - Old submeshes are cleared from the target `GROUND_MESH` component children, and new submesh messages containing the procedurally generated vertex and index buffers are packed and appended.
