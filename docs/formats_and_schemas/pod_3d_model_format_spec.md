# PowerVR POD 3D Model Format Specification (`.pod`)

> Implementation in [`src/tools/pod_loader.h`](file:///home/quantumcreeper/SwordigoDesktop/src/tools/pod_loader.h) / [`src/tools/pod_loader.cpp`](file:///home/quantumcreeper/SwordigoDesktop/src/tools/pod_loader.cpp).

---

## 1. Overview
POD (PowerVR Object Data) is a **chunk-based tag-length-data (TLD) binary format** developed by Imagination Technologies for the PowerVR SDK. Swordigo uses `.pod` files for all 3D models — characters, terrain meshes, platforms, enemies, and environment geometry.

- **File Extension**: `.pod` / `.POD`
- **Byte Order**: Little-endian throughout
- **Asset Location**: `assets/resources/Models/`

---

## 2. Chunk Structure

Every POD file consists of chunk headers (8 bytes) followed by payload data:
```
┌──────────────────────────────────────────────────┐
│  uint32_t  tag     <- Chunk type identifier       │
│  uint32_t  length  <- Payload size in bytes        │
│  [length bytes]    <- Payload data                 │
└──────────────────────────────────────────────────┘
```

### Container Chunks
Containers group nested child chunks. An open container has `length = 0`. The closing container tag has the high bit set: `close_tag = open_tag | 0x80000000`.

---

## 3. Key Chunk Tags

| Tag (Dec) | Tag (Hex) | Name | Description |
|---|---|---|---|
| `1000` | `0x000003E8` | Version | Null-terminated version string (`AB.POD.2.0`). |
| `2004` | `0x000007D4` | NumMesh | Total mesh count (`uint32_t`). |
| `2009` | `0x000007D9` | NumFrame | Animation frame count (`uint32_t`). |
| `2012` | `0x000007DC` | Mesh Container | Opens a mesh data block. |
| `6000` | `0x00001770` | NumVerts | Vertex count for mesh (`uint32_t`). |
| `6001` | `0x00001771` | NumFaces | Face / triangle count (`uint32_t`). |
| `6006` | `0x00001776` | Interleaved | Opens interleaved vertex buffer container. |
| `6007` | `0x00001777` | Faces | Opens index buffer container. |
| `9003` | `0x0000232B` | DataPayload | Raw binary vertex or index array payload. |

---

## 4. 32-Byte Interleaved Vertex Layout
Swordigo 3D meshes use a **32-byte stride interleaved vertex format**:
```
Byte Offset   Size    Type      Field
───────────   ────    ────      ─────
0             12B     float3    Position (x, y, z)
12            12B     float3    Normal   (nx, ny, nz)
24            8B      float2    Texture UV (u, v)
Total Stride: 32 bytes
```
