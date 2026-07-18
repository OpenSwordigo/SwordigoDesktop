# PowerVR POD Model Format and Parsing Specification

This document defines the chunk-based structure of the `AB.POD.2.0` model format and outlines the gap between the current C++ loader and the complete format capability.

---

## 1. File Structure and Chunk Format

A POD file is composed of structured chunks in a tree hierarchy.

### Chunk Header Layout
Each chunk starts with an 8-byte header:
- **`tag`** (4 bytes, `uint32` little endian)
- **`length`** (4 bytes, `uint32` little endian)

Following the header, there are `length` bytes of binary data.

### Hierarchical Nesting
- Child chunks are placed sequentially immediately after a parent chunk's data payload.
- The nesting level is closed by a sentinel chunk whose tag is the parent tag with the high bit set:
  $$\text{closing\_tag} = \text{parent\_tag} \mid \text{0x80000000}$$
  Upon hitting this tag, the recursive parser pops up a level.

### Signature Validation
The file is checked by confirming:
- The first 4 bytes equal the tag `ze.version` (`1000`).
- Bytes 8 to 19 parse to the ASCII version string `"AB.POD.2.0"`.

---

## 2. Format Schema Constants

| Tag Name | Tag ID | Description |
| :--- | :--- | :--- |
| `version` | 1000 | File format version signature |
| `scene` | 1001 | Top-level scene container |
| `sceneNumMeshNodes` | 2006 | Number of nodes containing meshes |
| `sceneNumFrames` | 2009 | Total animation frames |
| `sceneMesh` | 2012 | Mesh description container |
| `sceneNode` | 2013 | Scene node hierarchy definition |
| `sceneTexture` | 2014 | Textures list container |
| `sceneMaterial` | 2015 | Material definitions container |
| `sceneFPS` | 2017 | Animation FPS |
| `matName` | 3000 | Material name string |
| `matDiffuseTexIdx` | 3001 | Index of diffuse texture |
| `matOpacity` | 3002 | Opacity value |
| `matDiffuse` | 3004 | Diffuse RGB color vector |
| `texFilename` | 4000 | Filename of texture mapping |
| `nodeIndex` | 5000 | Index of node |
| `nodeName` | 5001 | Name of node |
| `nodeMaterialIndex`| 5002 | Reference material index |
| `nodeParentIndex` | 5003 | Parent node index (-1 if root) |
| `nodePosition` | 5004 | Node base position vector |
| `nodeRotation` | 5005 | Node base rotation quaternion |
| `nodeScale` | 5006 | Node base scale vector |
| `nodeAnimPosition` | 5007 | Position animation track keys |
| `nodeAnimRotation` | 5008 | Rotation animation track keys |
| `nodeAnimScale` | 5009 | Scale animation track keys |
| `nodeMatrix` | 5010 | Node static transform matrix |
| `nodeAnimMatrix` | 5011 | Animated transform matrix keys |
| `nodeAnimFlags` | 5012 | Animation track interpolation flags |
| `meshNumVertices` | 6000 | Vertices count |
| `meshNumFaces` | 6001 | Faces count |
| `meshNumUVWChannels`| 6002 | Number of UV mapping channels |
| `meshVertexIndexList`| 6003 | Index buffer descriptors |
| `meshVertexList` | 6006 | Vertex positions channel |
| `meshNormalList` | 6007 | Vertex normals channel |
| `meshUVWList` | 6010 | Vertex UV mappings channel |
| `meshBoneIndexList` | 6012 | Skeletal bone indices per vertex |
| `meshBoneWeightList`| 6013 | Skeletal bone weights per vertex |
| `meshInterleavedDataList` | 6014 | Interleaved data buffer container |
| `meshBoneBatchIndexList` | 6015 | Bone batch indices |
| `meshNumBoneIndicesPerBatch`| 6016 | Bone count in batch |
| `meshBoneOffsetPerBatch` | 6017 | Offsets of batches |
| `meshMaxNumBonesPerBatch`| 6018 | Limit of bones per batch |
| `meshNumBoneBatches` | 6019 | Total bone batches |
| `blockDataType` | 9000 | Primitive data type enum |
| `blockNumComponents` | 9001 | Vector elements size |
| `blockStride` | 9002 | Byte stride of block |
| `blockData` | 9003 | Raw binary payload |

---

## 3. Data Type Decoding

Each attribute data block (vertices, normals, UVs) contains a `blockDataType` property identifying its binary coding:

- **`1`**: `Float32` (4 bytes)
- **`2`**: `Int32` (4 bytes)
- **`3`**: `Uint16` (2 bytes)
- **`9`**: Fixed-point 16.16 ($\text{val} / 65536$) (4 bytes)
- **`10`**: `Uint8` (1 byte)
- **`11`**: `Int16` (2 bytes)
- **`12`**: Normalized `Int16` ($\text{val} / 32767$) (2 bytes)
- **`13`**: `Int8` (1 byte)
- **`14`**: Normalized `Int8` ($\text{val} / 127$) (1 byte)
- **`15`**: Normalized `Uint8` ($\text{val} / 255$) (1 byte)
- **`16`**: Normalized `Uint16` ($\text{val} / 65535$) (2 bytes)
- **`17`**: `Uint32` (4 bytes)

---

## 4. Gap Analysis: Current C++ Loader vs. Advanced Loader

Our current implementation in `exptsrc/tools/pod_loader.cpp` is a simplified, non-compliant parser designed for static geometry:

### Gaps Identifed
1. **No Node Hierarchy / Skeletal Joints**:
   The current loader ignores node structures (`sceneNode` / 2013). Consequently, skeletal bone linkages (`nodeParentIndex`, local transformation matrices, parent-child joints offsets) are not loaded.
2. **No Animation Keys**:
   Position (`nodeAnimPosition`), rotation (`nodeAnimRotation`), and scale (`nodeAnimScale`) animation tracks are skipped. Rigged mesh animations cannot be played.
3. **No Materials & Mappings**:
   Texture filename lookups (`texFilename` / 4000) and material mappings (`nodeMaterialIndex`, `matDiffuse`, `matOpacity`) are ignored. Mesh elements are rendered using fallback parameters instead of mapped textures and colors.
4. **No Skinned Vertex Buffers**:
   Vertex bone weights (`meshBoneWeightList`) and indices (`meshBoneIndexList`) are skipped during vertex extraction.

### Proposed Improvement Roadmap for C++ Loader
To achieve full parity with advanced asset editors and support rigged characters and maps:
- **Node Tree Construction**: Implement parent-child matrix multiplication to traverse node arrays and generate relative coordinates.
- **Joint Weights Parser**: Extract bones/weights vectors during `deinterleave` / `extract_floats` logic.
- **Material Dictionary**: Parse texture file lists to match material indices with actual asset manager resource bindings.
