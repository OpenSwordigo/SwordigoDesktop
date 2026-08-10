// pod_loader.cpp — PowerVR POD model parser implementation
// Reference: com/powervr/pod/PODLoader.as & EPODIdentifiers.as

#include "pod_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <filesystem>

namespace av {

static void finalize_model_bounds(PODModel& model);
static PODFileLoader g_pod_file_loader = nullptr;

void set_pod_file_loader(PODFileLoader loader) {
    g_pod_file_loader = loader;
}

// ─── Tag Constants from SDK ──────────────────────────────────────────
static constexpr uint32_t kEndTagMask = 0x80000000u;

// Identifiers
static constexpr uint32_t eFormatVersion               = 1000;
static constexpr uint32_t eScene                        = 1001;

// Scene parameters
static constexpr uint32_t eSceneNumMeshes               = 2004;
static constexpr uint32_t eSceneNumNodes                = 2005;
static constexpr uint32_t eSceneNumMeshNodes            = 2006;
static constexpr uint32_t eSceneNumTextures             = 2007;
static constexpr uint32_t eSceneNumMaterials            = 2008;
static constexpr uint32_t eSceneNumFrames               = 2009;
static constexpr uint32_t eSceneFPS                     = 2017;
static constexpr uint32_t eSceneMesh                    = 2012;
static constexpr uint32_t eSceneNode                    = 2013;
static constexpr uint32_t eSceneTexture                 = 2014;
static constexpr uint32_t eSceneMaterial                = 2015;

// Material properties
static constexpr uint32_t eMaterialName                 = 3000;
static constexpr uint32_t eMaterialDiffuseTextureIndex  = 3001;
static constexpr uint32_t eMaterialOpacity              = 3002;
static constexpr uint32_t eMaterialDiffuse              = 3004;

// Texture properties
static constexpr uint32_t eTextureFilename              = 4000;

// Node properties
static constexpr uint32_t eNodeIndex                    = 5000;
static constexpr uint32_t eNodeName                     = 5001;
static constexpr uint32_t eNodeMaterialIndex            = 5002;
static constexpr uint32_t eNodeParentIndex              = 5003;
static constexpr uint32_t eNodePosition                 = 5004;
static constexpr uint32_t eNodeRotation                 = 5005;
static constexpr uint32_t eNodeScale                    = 5006;
static constexpr uint32_t eNodeAnimationPosition        = 5007;
static constexpr uint32_t eNodeAnimationRotation        = 5008;
static constexpr uint32_t eNodeAnimationScale           = 5009;
static constexpr uint32_t eNodeMatrix                   = 5010;
static constexpr uint32_t eNodeAnimationMatrix           = 5011;
static constexpr uint32_t eNodeAnimationFlags            = 5012;
static constexpr uint32_t eNodeAnimationPositionIndex   = 5013;
static constexpr uint32_t eNodeAnimationRotationIndex   = 5014;
static constexpr uint32_t eNodeAnimationScaleIndex      = 5015;
static constexpr uint32_t eNodeAnimationMatrixIndex     = 5016;

// Mesh properties
// (Block-ID table cross-checked against blenderift's jPOD.py — the PowerVR
// POD spec from Imagination's Developer Technology Team — see
// docs/BLENDERIFT_INTEGRATION.md for the full reference.)
static constexpr uint32_t eMeshNumVertices              = 6000;
static constexpr uint32_t eMeshNumFaces                 = 6001;
static constexpr uint32_t eMeshNumUVWChannels            = 6002;
static constexpr uint32_t eMeshVertexIndexList          = 6003;
static constexpr uint32_t eMeshStripLengthList          = 6004;  // 'il' list of strip lengths (strips only)
static constexpr uint32_t eMeshNumStrips                = 6005;  // >0 => faces stored as triangle strips
static constexpr uint32_t eMeshVertexList               = 6006;
static constexpr uint32_t eMeshNormalList               = 6007;
static constexpr uint32_t eMeshTangentList              = 6008;
static constexpr uint32_t eMeshBinormalList             = 6009;
static constexpr uint32_t eMeshUVWList                  = 6010;
static constexpr uint32_t eMeshVertexColourList         = 6011;
static constexpr uint32_t eMeshBoneIndexList            = 6012;
static constexpr uint32_t eMeshBoneWeightList            = 6013;
static constexpr uint32_t eMeshInteravedDataList        = 6014;
static constexpr uint32_t eMeshBoneBatchIndexList       = 6015;
static constexpr uint32_t eMeshNumBoneIndicesPerBatch   = 6016;
static constexpr uint32_t eMeshBoneOffsetPerBatch       = 6017;
static constexpr uint32_t eMeshMaxNumBonesPerBatch      = 6018;
static constexpr uint32_t eMeshNumBoneBatches           = 6019;
static constexpr uint32_t eMeshUnpackMatrix             = 6020;  // 'f' 16 floats, unpacks packed vertex data
static constexpr uint32_t eMeshType                     = 6021;  // 0 = triangles (list/strip), 1 = quads, 2 = lines
static constexpr uint32_t eMeshAdjacencyIndexList       = 6022;  // 'il' 6 per face, optional

// Vertex Block fields
static constexpr uint32_t eBlockDataType                = 9000;
static constexpr uint32_t eBlockNumComponents           = 9001;
static constexpr uint32_t eBlockStride                  = 9002;
static constexpr uint32_t eBlockData                    = 9003;

// Vertex data types (DataType 9000 values, from jPOD.py / PowerVR SDK)
// 1 Float, 2 Int, 3 UnsignedShort, 4 RGBA, 5 ARGB, 6 D3DCOLOR, 7 UBYTE4,
// 8 DEC3N, 9 Fixed16_16, 10 UnsignedByte, 11 Short, 12 ShortNorm,
// 13 Byte, 14 ByteNorm, 15 UnsignedByteNorm, 16 UnsignedShortNorm,
// 17 UnsignedInt, 18 ABGR, 19 HalfFloat.

// Helper to safely read values
static uint32_t read_u32(const uint8_t* data, size_t size, size_t& off) {
    if (off + 4 > size) { off = size; return 0; }
    uint32_t val;
    std::memcpy(&val, data + off, 4);
    off += 4;
    return val;
}

static float read_float(const uint8_t* data, size_t size, size_t& off) {
    if (off + 4 > size) { off = size; return 0.0f; }
    float val;
    std::memcpy(&val, data + off, 4);
    off += 4;
    return val;
}

static std::vector<float> read_float_array(const uint8_t* data, size_t len, size_t& off) {
    std::vector<float> val(len / 4);
    if (len > 0) {
        std::memcpy(val.data(), data + off, len);
        off += len;
    }
    return val;
}

static std::vector<uint32_t> read_u32_array(const uint8_t* data, size_t len, size_t& off) {
    std::vector<uint32_t> val(len / 4);
    if (len > 0) {
        std::memcpy(val.data(), data + off, len);
        off += len;
    }
    return val;
}

// Data Element structure mirroring SDK's Block elements
struct DataElement {
    uint32_t type = 0;
    uint32_t num_components = 0;
    uint32_t stride = 0;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
};

// Parse a vertex block container (e.g. vertices, normals, uvs)
static void parse_vertex_block(const uint8_t* data, size_t size, size_t& off, uint32_t block_id, DataElement& out) {
    uint32_t end_tag = block_id | kEndTagMask;
    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == end_tag) return;

        switch (tag) {
            case eBlockDataType:
                if (len >= 4) out.type = read_u32(data, size, off);
                else off += len;
                break;
            case eBlockNumComponents:
                if (len >= 4) out.num_components = read_u32(data, size, off);
                else off += len;
                break;
            case eBlockStride:
                if (len >= 4) out.stride = read_u32(data, size, off);
                else off += len;
                break;
            case eBlockData:
                out.payload = data + off;
                out.payload_size = len;
                off += len;
                break;
            default:
                off += len;
                break;
        }
    }
}

// Convert diverse vertex components (interleaved or standalone) to floats
static std::vector<float> unpack_vertex_data(
    const uint8_t* interleaved_payload, size_t interleaved_size,
    const DataElement& de, int num_vertices, int num_components) 
{
    std::vector<float> result(num_vertices * num_components, 0.0f);
    if (num_vertices <= 0 || num_components <= 0) return result;

    const uint8_t* src_ptr = nullptr;
    const uint8_t* limit_ptr = nullptr;
    size_t stride = de.stride;

    if (interleaved_payload != nullptr) {
        // Interleaved: payload is a 4-byte offset into interleaved data block
        uint32_t offset = 0;
        if (de.payload && de.payload_size >= 4) {
            std::memcpy(&offset, de.payload, 4);
        }
        if (offset >= interleaved_size) return result;
        src_ptr = interleaved_payload + offset;
        limit_ptr = interleaved_payload + interleaved_size;
    } else {
        // Non-interleaved: payload contains the actual elements directly
        src_ptr = de.payload;
        limit_ptr = de.payload ? (de.payload + de.payload_size) : nullptr;
    }

    if (!src_ptr || !limit_ptr) return result;

    uint32_t type = de.type;
    size_t comp_size = 4;
    if (type == 1) comp_size = 4; // float
    else if (type == 2 || type == 17) comp_size = 4; // int / uint
    else if (type == 3 || type == 11 || type == 12 || type == 16) comp_size = 2; // short / ushort
    else if (type == 10 || type == 13 || type == 14 || type == 15) comp_size = 1; // byte / ubyte

    // libswordigo_arm32.c Is(): stride defaults to blockNumComponents * compSize when 0
    uint32_t block_components = (de.num_components > 0) ? de.num_components : (uint32_t)num_components;
    if (stride == 0) {
        stride = block_components * comp_size;
    }
    int read_components = std::min((int)block_components, num_components);

    for (int i = 0; i < num_vertices; ++i) {
        const uint8_t* vert_ptr = src_ptr + i * stride;
        if (vert_ptr + stride > limit_ptr) break;

        for (int c = 0; c < read_components; ++c) {
            const uint8_t* comp_ptr = vert_ptr + c * comp_size;
            if (comp_ptr + comp_size > limit_ptr) continue;

            float val = 0.0f;
            if (type == 1) { // Float
                std::memcpy(&val, comp_ptr, 4);
            } else if (type == 3) { // Unsigned Short
                uint16_t v; std::memcpy(&v, comp_ptr, 2);
                val = static_cast<float>(v);
            } else if (type == 16) { // Unsigned Short Normalized
                uint16_t v; std::memcpy(&v, comp_ptr, 2);
                val = static_cast<float>(v) / 65535.0f;
            } else if (type == 11) { // Signed Short
                int16_t v; std::memcpy(&v, comp_ptr, 2);
                val = static_cast<float>(v);
            } else if (type == 12) { // Signed Short Normalized
                int16_t v; std::memcpy(&v, comp_ptr, 2);
                val = static_cast<float>(v) / 32767.0f;
            } else if (type == 10) { // Unsigned Byte
                val = static_cast<float>(comp_ptr[0]);
            } else if (type == 15) { // Unsigned Byte Normalized
                val = static_cast<float>(comp_ptr[0]) / 255.0f;
            } else if (type == 13) { // Signed Byte
                val = static_cast<float>(static_cast<int8_t>(comp_ptr[0]));
            } else if (type == 14) { // Signed Byte Normalized
                val = static_cast<float>(static_cast<int8_t>(comp_ptr[0])) / 127.0f;
            } else if (type == 2) { // Signed Int
                int32_t v; std::memcpy(&v, comp_ptr, 4);
                val = static_cast<float>(v);
            } else if (type == 17) { // Unsigned Int
                uint32_t v; std::memcpy(&v, comp_ptr, 4);
                val = static_cast<float>(v);
            }
            result[i * num_components + c] = val;
        }
    }
    return result;
}

// Parse indices (Face Index List)
// Matches libswordigo_arm32.c yS: indices are decoded by dataType and widened into a
// Uint32Array of size numFaces*verts_per_face (never truncated to 16-bit).
// Triangle meshes use 3; quad meshes (MeshType 6021 == 1) use 4 and are
// expanded to triangles here. All stock Swordigo meshes are triangles.
static void parse_indices(const DataElement& de, int num_faces, int verts_per_face, PODMesh& mesh) {
    if (!de.payload || de.payload_size == 0) return;
    const int index_count = num_faces * verts_per_face;
    mesh.indices.resize(index_count);

    size_t comp_size = 4;
    if (de.type == 3 || de.type == 11 || de.type == 12 || de.type == 16) comp_size = 2; // short
    else if (de.type == 7 || de.type == 10 || de.type == 13 || de.type == 14 || de.type == 15) comp_size = 1; // byte

    for (int i = 0; i < index_count; i++) {
        if ((size_t)(i + 1) * comp_size > de.payload_size) break;
        uint32_t val = 0;
        if (comp_size == 4) {
            if (de.type == 2) { int32_t v; std::memcpy(&v, de.payload + i * 4, 4); val = (uint32_t)v; }
            else std::memcpy(&val, de.payload + i * 4, 4);
        } else if (comp_size == 2) {
            uint16_t v; std::memcpy(&v, de.payload + i * 2, 2);
            val = v;
        } else {
            val = de.payload[i];
        }
        mesh.indices[i] = val;
    }
    if (verts_per_face == 4) {
        std::vector<uint32_t> tris;
        tris.reserve((size_t)num_faces * 6);
        for (int f = 0; f < num_faces; ++f) {
            const uint32_t* q = &mesh.indices[(size_t)f * 4];
            tris.push_back(q[0]); tris.push_back(q[1]); tris.push_back(q[2]);
            tris.push_back(q[0]); tris.push_back(q[2]); tris.push_back(q[3]);
        }
        mesh.indices.swap(tris);
    }
}

// Compute single mesh AABB bounding box
static void compute_mesh_aabb(PODMesh& mesh) {
    if (mesh.positions.empty()) return;
    mesh.min_x = mesh.min_y = mesh.min_z = 1e9f;
    mesh.max_x = mesh.max_y = mesh.max_z = -1e9f;
    for (size_t i = 0; i < mesh.positions.size() / 3; ++i) {
        float x = mesh.positions[i * 3 + 0];
        float y = mesh.positions[i * 3 + 1];
        float z = mesh.positions[i * 3 + 2];
        mesh.min_x = std::min(mesh.min_x, x);
        mesh.min_y = std::min(mesh.min_y, y);
        mesh.min_z = std::min(mesh.min_z, z);
        mesh.max_x = std::max(mesh.max_x, x);
        mesh.max_y = std::max(mesh.max_y, y);
        mesh.max_z = std::max(mesh.max_z, z);
    }
}

// MeshUnpackMatrix (6020) is how PowerVR-packed exports undo their packing.
// Real Swordigo PODs carry an identity here (verified across the asset
// library); non-identity values must be applied so Blender/GLB round-trips
// see unpacked vertices.
static bool unpack_matrix_is_identity_or_degenerate(const float m[16]) {
    const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool ident = true, zero = true;
    for (int i = 0; i < 16; ++i) {
        if (m[i] != id[i]) ident = false;
        if (m[i] != 0.0f)  zero = false;
    }
    return ident || zero;
}

// Apply the column-major 4x4 unpack matrix to positions (w=1) and normals
// (w=0, rotation part only).
static void apply_unpack_matrix(float* positions, size_t pos_count,
                                float* normals, size_t nrm_count,
                                const float m[16]) {
    for (size_t i = 0; i + 2 < pos_count; i += 3) {
        const float x = positions[i], y = positions[i + 1], z = positions[i + 2];
        positions[i]     = m[0] * x + m[4] * y + m[8]  * z + m[12];
        positions[i + 1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
        positions[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }
    for (size_t i = 0; i + 2 < nrm_count; i += 3) {
        const float x = normals[i], y = normals[i + 1], z = normals[i + 2];
        normals[i]     = m[0] * x + m[4] * y + m[8]  * z;
        normals[i + 1] = m[1] * x + m[5] * y + m[9]  * z;
        normals[i + 2] = m[2] * x + m[6] * y + m[10] * z;
    }
}

// readMeshBlock Implementation
static PODMesh readMeshBlock(const uint8_t* data, size_t size, size_t& off) {
    PODMesh mesh;
    uint32_t end_tag = eSceneMesh | kEndTagMask;

    const uint8_t* interleaved_payload = nullptr;
    size_t interleaved_size = 0;

    DataElement idx_element;
    DataElement pos_element;
    DataElement nrm_element;
    DataElement uv_element; // first UV channel
    DataElement bone_idx_element;
    DataElement bone_wgt_element;

    std::vector<uint32_t> bone_batch_indices;
    std::vector<uint32_t> bone_batch_counts;
    std::vector<uint32_t> bone_batch_offsets;
    uint32_t max_bones_per_batch = 0;
    uint32_t num_bone_batches = 0;

    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == end_tag) break;

        switch (tag) {
            case eMeshNumVertices:
                mesh.num_vertices = static_cast<int>(read_u32(data, size, off));
                break;
            case eMeshNumFaces:
                mesh.num_faces = static_cast<int>(read_u32(data, size, off));
                break;
            case eMeshInteravedDataList:
                interleaved_payload = data + off;
                interleaved_size = len;
                off += len;
                break;
            case eMeshVertexIndexList:
                parse_vertex_block(data, size, off, tag, idx_element);
                break;
            case eMeshVertexList:
                parse_vertex_block(data, size, off, tag, pos_element);
                break;
            case eMeshNormalList:
                parse_vertex_block(data, size, off, tag, nrm_element);
                break;
            case eMeshUVWList: {
                DataElement current_uv;
                parse_vertex_block(data, size, off, tag, current_uv);
                if (uv_element.payload == nullptr) {
                     uv_element = current_uv; // Store the first UV channel
                }
                break;
            }
            case eMeshBoneIndexList:
                parse_vertex_block(data, size, off, tag, bone_idx_element);
                break;
            case eMeshBoneWeightList:
                parse_vertex_block(data, size, off, tag, bone_wgt_element);
                break;
            case eMeshBoneBatchIndexList:
                bone_batch_indices = read_u32_array(data, len, off);
                break;
            case eMeshNumBoneIndicesPerBatch:
                bone_batch_counts = read_u32_array(data, len, off);
                break;
            case eMeshBoneOffsetPerBatch:
                bone_batch_offsets = read_u32_array(data, len, off);
                break;
            case eMeshMaxNumBonesPerBatch:
                max_bones_per_batch = read_u32(data, size, off);
                break;
            case eMeshNumBoneBatches:
                num_bone_batches = read_u32(data, size, off);
                break;
            case eMeshUnpackMatrix:
                if (len >= 16 * 4) {
                    std::memcpy(mesh.unpack_matrix, data + off, 16 * 4);
                    mesh.has_unpack_matrix = true;
                }
                off += len;
                break;
            case eMeshType:
                if (len >= 4) {
                    mesh.mesh_type = static_cast<int>(read_u32(data, size, off));
                    if (len > 4) off += len - 4;   // keep the block walker aligned
                } else {
                    off += len;
                }
                break;
            default:
                off += len;
                break;
        }
    }

    // Now convert and unpack elements. Quads (mesh_type 1) expand to tris.
    parse_indices(idx_element, mesh.num_faces, mesh.mesh_type == 1 ? 4 : 3, mesh);

    mesh.positions = unpack_vertex_data(interleaved_payload, interleaved_size, pos_element, mesh.num_vertices, 3);
    mesh.normals   = unpack_vertex_data(interleaved_payload, interleaved_size, nrm_element, mesh.num_vertices, 3);
    mesh.uvs       = unpack_vertex_data(interleaved_payload, interleaved_size, uv_element,  mesh.num_vertices, 2);

    mesh.bones_per_vertex = bone_idx_element.num_components;
    if (mesh.bones_per_vertex > 0) {
        mesh.bone_indices = unpack_vertex_data(interleaved_payload, interleaved_size, bone_idx_element, mesh.num_vertices, mesh.bones_per_vertex);
        mesh.bone_weights = unpack_vertex_data(interleaved_payload, interleaved_size, bone_wgt_element, mesh.num_vertices, mesh.bones_per_vertex);
    }

    if (num_bone_batches > 0) {
        mesh.has_bone_batches = true;
        mesh.bone_batches.indices = std::move(bone_batch_indices);
        mesh.bone_batches.counts = std::move(bone_batch_counts);
        mesh.bone_batches.offsets = std::move(bone_batch_offsets);
        mesh.bone_batches.max_bones = max_bones_per_batch;
        mesh.bone_batches.count = num_bone_batches;
    }

    if (mesh.has_unpack_matrix &&
        !unpack_matrix_is_identity_or_degenerate(mesh.unpack_matrix)) {
        apply_unpack_matrix(mesh.positions.data(), mesh.positions.size(),
                            mesh.normals.data(), mesh.normals.size(),
                            mesh.unpack_matrix);
    }

    compute_mesh_aabb(mesh);

    return mesh;
}

// readNodeBlock Implementation
static PODNode readNodeBlock(const uint8_t* data, size_t size, size_t& off) {
    PODNode node;
    uint32_t end_tag = eSceneNode | kEndTagMask;

    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == end_tag) break;

        switch (tag) {
            case eNodeIndex:
                node.object_index = static_cast<int>(read_u32(data, size, off));
                break;
            case eNodeName:
                if (len > 0) {
                    node.name.assign(reinterpret_cast<const char*>(data + off), strnlen(reinterpret_cast<const char*>(data + off), len));
                }
                off += len;
                break;
            case eNodeMaterialIndex:
                node.material_index = static_cast<int>(read_u32(data, size, off));
                break;
            case eNodeParentIndex:
                node.parent_index = static_cast<int>(read_u32(data, size, off));
                break;
            case eNodePosition: // static translation
                if (len >= 12) {
                    node.translation[0] = read_float(data, size, off);
                    node.translation[1] = read_float(data, size, off);
                    node.translation[2] = read_float(data, size, off);
                    node.has_translation = true;
                    off += (len - 12);
                } else off += len;
                break;
            case eNodeRotation: // static rotation
                if (len >= 16) {
                    node.rotation[0] = read_float(data, size, off);
                    node.rotation[1] = read_float(data, size, off);
                    node.rotation[2] = read_float(data, size, off);
                    node.rotation[3] = read_float(data, size, off);
                    node.has_rotation = true;
                    off += (len - 16);
                } else off += len;
                break;
            case eNodeScale: // static scale
                if (len >= 12) {
                    node.scale[0] = read_float(data, size, off);
                    node.scale[1] = read_float(data, size, off);
                    node.scale[2] = read_float(data, size, off);
                    node.has_scale = true;
                    off += (len - 12);
                } else off += len;
                break;
            case eNodeMatrix: // static matrix
                if (len >= 64) {
                    for (int i = 0; i < 16; i++) node.matrix[i] = read_float(data, size, off);
                    node.has_matrix = true;
                    off += (len - 64);
                } else off += len;
                break;
            case eNodeAnimationPosition:
                node.anim_translation = read_float_array(data, len, off);
                break;
            case eNodeAnimationRotation:
                node.anim_rotation = read_float_array(data, len, off);
                break;
            case eNodeAnimationScale:
                node.anim_scale = read_float_array(data, len, off);
                break;
            case eNodeAnimationMatrix:
                node.anim_matrix = read_float_array(data, len, off);
                break;
            case eNodeAnimationFlags:
                node.anim_flags = read_u32(data, size, off);
                break;
            case eNodeAnimationPositionIndex:
                node.anim_translation_idx = read_u32_array(data, len, off);
                break;
            case eNodeAnimationRotationIndex:
                node.anim_rotation_idx = read_u32_array(data, len, off);
                break;
            case eNodeAnimationScaleIndex:
                node.anim_scale_idx = read_u32_array(data, len, off);
                break;
            case eNodeAnimationMatrixIndex:
                node.anim_matrix_idx = read_u32_array(data, len, off);
                break;
            default:
                off += len;
                break;
        }
    }

    return node;
}

// readTextureBlock Implementation
static std::string readTextureBlock(const uint8_t* data, size_t size, size_t& off) {
    std::string name;
    uint32_t end_tag = eSceneTexture | kEndTagMask;
    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == end_tag) break;

        if (tag == eTextureFilename) {
            if (len > 0) {
                name.assign(reinterpret_cast<const char*>(data + off), strnlen(reinterpret_cast<const char*>(data + off), len));
            }
            off += len;
        } else {
            off += len;
        }
    }
    return name;
}

// readMaterialBlock Implementation
static PODMaterial readMaterialBlock(const uint8_t* data, size_t size, size_t& off) {
    PODMaterial mat;
    uint32_t end_tag = eSceneMaterial | kEndTagMask;
    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == end_tag) break;

        switch (tag) {
            case eMaterialName:
                if (len > 0) {
                    mat.name.assign(reinterpret_cast<const char*>(data + off), strnlen(reinterpret_cast<const char*>(data + off), len));
                }
                off += len;
                break;
            case eMaterialDiffuseTextureIndex:
                mat.diffuse_texture_index = static_cast<int>(read_u32(data, size, off));
                break;
            case eMaterialOpacity:
                if (len >= 4) mat.opacity = read_float(data, size, off);
                else off += len;
                break;
            case eMaterialDiffuse:
                if (len >= 12) {
                    mat.diffuse[0] = read_float(data, size, off);
                    mat.diffuse[1] = read_float(data, size, off);
                    mat.diffuse[2] = read_float(data, size, off);
                    off += (len - 12);
                } else off += len;
                break;
            default:
                off += len;
                break;
        }
    }
    return mat;
}

// readSceneBlock Implementation
static void readSceneBlock(const uint8_t* data, size_t size, size_t& off, PODModel& model) {
    uint32_t end_tag = eScene | kEndTagMask;
    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == end_tag) break;

        switch (tag) {
            case eSceneNumMeshes:
                if (len >= 4) {
                    uint32_t count = read_u32(data, size, off);
                    model.meshes.reserve(count);
                } else off += len;
                break;
            case eSceneNumNodes:
                if (len >= 4) {
                    uint32_t count = read_u32(data, size, off);
                    model.nodes.reserve(count);
                } else off += len;
                break;
            case eSceneNumMeshNodes:
                if (len >= 4) {
                    model.num_mesh_nodes = static_cast<int>(read_u32(data, size, off));
                } else off += len;
                break;
            case eSceneNumTextures:
                if (len >= 4) {
                    model.texture_filenames.reserve(read_u32(data, size, off));
                } else off += len;
                break;
            case eSceneNumMaterials:
                if (len >= 4) {
                    model.materials.reserve(read_u32(data, size, off));
                } else off += len;
                break;
            case eSceneNumFrames:
                if (len >= 4) {
                    model.num_frames = static_cast<int>(read_u32(data, size, off));
                } else off += len;
                break;
            case eSceneFPS:
                if (len >= 4) {
                    model.fps = static_cast<float>(read_u32(data, size, off));
                    if (model.fps <= 0.0f) model.fps = 30.0f;
                } else off += len;
                break;
            case eSceneMesh:
                model.meshes.push_back(readMeshBlock(data, size, off));
                break;
            case eSceneNode:
                model.nodes.push_back(readNodeBlock(data, size, off));
                break;
            case eSceneTexture:
                model.texture_filenames.push_back(readTextureBlock(data, size, off));
                break;
            case eSceneMaterial:
                model.materials.push_back(readMaterialBlock(data, size, off));
                break;
            default:
                off += len;
                break;
        }
    }
}

// Parse top-level structures in POD
PODModel pod_parse(const uint8_t* data, size_t size) {
    PODModel model;
    size_t off = 0;

    while (off < size) {
        uint32_t tag = read_u32(data, size, off);
        uint32_t len = read_u32(data, size, off);
        if (off + len > size) len = size - off;

        if (tag == eFormatVersion) {
            if (len > 0) {
                model.version.assign(reinterpret_cast<const char*>(data + off), strnlen(reinterpret_cast<const char*>(data + off), len));
            }
            off += len;
        } else if (tag == eScene) {
            readSceneBlock(data, size, off, model);
        } else {
            off += len;
        }
    }

    finalize_model_bounds(model);

    return model;
}

PODModel pod_load(const std::string& path, const std::string& merge_hint) {
    if (g_pod_file_loader) {
        std::vector<uint8_t> data;
        if (g_pod_file_loader(path, data) && !data.empty()) {
            return pod_parse(data.data(), data.size());
        }
        return {};
    }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};

    auto fsize = f.tellg();
    if (fsize <= 0) return {};

    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), fsize);

    if (!f) return {};

    PODModel model = pod_parse(buf.data(), buf.size());

    // Check if this is an animation-only POD (no meshes)
    if (model.meshes.empty()) {
        namespace fs = std::filesystem;
        fs::path anim_path(path);
        fs::path parent_dir = anim_path.parent_path();
        std::string stem = anim_path.stem().string();

        // Strip "_2x" if it exists at the end of the stem
        bool is_2x = false;
        if (stem.size() > 3 && stem.substr(stem.size() - 3) == "_2x") {
            stem = stem.substr(0, stem.size() - 3);
            is_2x = true;
        }

        if (stem.find('_') != std::string::npos) {
            std::vector<fs::path> candidates;
            // Animation names are commonly nested, e.g.
            // boss1_shadowform_spin.POD -> boss1_shadowform.POD. Try the
            // longest prefix first instead of truncating at the first '_'.
            std::string prefix = stem;
            while (prefix.find('_') != std::string::npos) {
                prefix.resize(prefix.rfind('_'));
                if (is_2x) {
                    candidates.push_back(parent_dir / (prefix + "_2x.pod"));
                    candidates.push_back(parent_dir / (prefix + "_2x.POD"));
                }
                candidates.push_back(parent_dir / (prefix + ".pod"));
                candidates.push_back(parent_dir / (prefix + ".POD"));
            }

            fs::path base_path;
            for (const auto& cand : candidates) {
                if (fs::exists(cand)) {
                    base_path = cand;
                    break;
                }
            }

            if (base_path.empty() && fs::exists(parent_dir)) {
                for (const auto& entry : fs::directory_iterator(parent_dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string entry_stem = entry.path().stem().string();
                    std::string entry_ext = entry.path().extension().string();
                    
                    for (auto& c : entry_ext) c = (char)tolower((unsigned char)c);
                    if (entry_ext != ".pod") continue;

                    if (entry_stem.size() > 3 && entry_stem.substr(entry_stem.size() - 3) == "_2x") {
                        entry_stem = entry_stem.substr(0, entry_stem.size() - 3);
                    }

                    if (stem.rfind(entry_stem + "_", 0) == 0 && entry.path() != anim_path) {
                        base_path = entry.path();
                        break;
                    }
                }
            }

            // Fall back to an explicit base-model hint from the caller — the
            // scene object's own base mesh. npc_stand.POD carries only the
            // anim streams; the stripped prefix (npc.POD) doesn't exist, but
            // the prisoner's base rig lives at knight.POD (and snowball_land
            // at shadowblob.POD). Node names are matched, so a wrong hint is
            // harmless (no stream copies).
            if (base_path.empty() && !merge_hint.empty()) {
                for (const fs::path& cand : {parent_dir / (merge_hint + ".POD"),
                                             parent_dir / (merge_hint + ".pod")}) {
                    if (fs::exists(cand)) { base_path = cand; break; }
                }
            }

            if (!base_path.empty()) {
                std::cout << "[POD] Detected animation-only POD. Merging with base model: " << base_path.string() << std::endl;
                PODModel base_model = pod_load(base_path.string());
                if (!base_model.meshes.empty()) {
                    base_model.num_frames = model.num_frames;
                    base_model.fps = model.fps;
                    // Capture the base model's TRUE bind pose first: the mesh
                    // was exported in this pose, and skinning must stay
                    // relative to it even after the anim streams are replaced.
                    // (The base model stores its rest pose as 1-frame anim
                    // streams — frame 0 of THAT model is the bind pose.)
                    for (size_t bi = 0; bi < base_model.nodes.size(); ++bi) {
                        float bind_m[16];
                        get_node_matrix(base_model, static_cast<int>(bi), 0.0f, bind_m);
                        base_model.nodes[bi].has_bind_matrix = true;
                        std::memcpy(base_model.nodes[bi].bind_matrix, bind_m, sizeof(bind_m));
                    }
                    for (auto& base_node : base_model.nodes) {
                        for (const auto& anim_node : model.nodes) {
                            if (base_node.name == anim_node.name) {
                                base_node.anim_translation = anim_node.anim_translation;
                                base_node.anim_rotation = anim_node.anim_rotation;
                                base_node.anim_scale = anim_node.anim_scale;
                                base_node.anim_matrix = anim_node.anim_matrix;
                                base_node.anim_translation_idx = anim_node.anim_translation_idx;
                                base_node.anim_rotation_idx = anim_node.anim_rotation_idx;
                                base_node.anim_scale_idx = anim_node.anim_scale_idx;
                                base_node.anim_matrix_idx = anim_node.anim_matrix_idx;
                                base_node.anim_flags = anim_node.anim_flags;
                                break;
                            }
                        }
                    }
                    finalize_model_bounds(base_model);
                    return base_model;
                }
            }
        }
    }

    return model;
}

// ─── Local Matrix Multiplication and Transformations ──────────────────
static void local_mat4_identity(float m[16]) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void local_mat4_mul(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    std::memcpy(out, tmp, 16 * sizeof(float));
}

static bool local_mat4_inverse(const float in[16], float out[16]) {
    float a[16];
    std::memcpy(a, in, sizeof(a));
    local_mat4_identity(out);
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row)
            if (std::fabs(a[col * 4 + row]) > std::fabs(a[col * 4 + pivot])) pivot = row;
        if (std::fabs(a[col * 4 + pivot]) < 1e-8f) return false;
        if (pivot != col) {
            for (int c = 0; c < 4; ++c) {
                std::swap(a[c * 4 + col], a[c * 4 + pivot]);
                std::swap(out[c * 4 + col], out[c * 4 + pivot]);
            }
        }
        float scale = 1.0f / a[col * 4 + col];
        for (int c = 0; c < 4; ++c) {
            a[c * 4 + col] *= scale;
            out[c * 4 + col] *= scale;
        }
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            float factor = a[col * 4 + row];
            for (int c = 0; c < 4; ++c) {
                a[c * 4 + row] -= factor * a[c * 4 + col];
                out[c * 4 + row] -= factor * out[c * 4 + col];
            }
        }
    }
    return true;
}

static void local_mat4_from_quat(const float q[4], float m[16]) {
    // libswordigo_arm32.c::$c converts POD quaternions into the editor coordinate system
    // by negating xyz while preserving w.
    float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    m[0] = 1.0f - 2.0f * (y * y + z * z);
    m[1] = 2.0f * (x * y + z * w);
    m[2] = 2.0f * (x * z - y * w);
    m[3] = 0.0f;

    m[4] = 2.0f * (x * y - z * w);
    m[5] = 1.0f - 2.0f * (x * x + z * z);
    m[6] = 2.0f * (y * z + x * w);
    m[7] = 0.0f;

    m[8] = 2.0f * (x * z + y * w);
    m[9] = 2.0f * (y * z - x * w);
    m[10] = 1.0f - 2.0f * (x * x + y * y);
    m[11] = 0.0f;

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

// POD animation streams may be compact keyframe arrays accompanied by one
// keyframe index per scene frame.  A missing index array means dense frames.
static int animation_key_index(const std::vector<uint32_t>& indices,
                               size_t value_count, int frame, int components) {
    if (value_count < static_cast<size_t>(components) || components <= 0)
        return -1;
    int frame_index = std::max(frame, 0);
    if (!indices.empty() && frame_index < static_cast<int>(indices.size()))
        frame_index = static_cast<int>(indices[frame_index]);
    const int key_count = static_cast<int>(value_count / components);
    return std::clamp(frame_index, 0, key_count - 1);
}

static void lerp3(float out[3], const float* a, const float* b, float t) {
    for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

static void slerp_quat(float out[4], const float* a, const float* b, float t) {
    float end[4] = {b[0], b[1], b[2], b[3]};
    float dot = a[0]*end[0] + a[1]*end[1] + a[2]*end[2] + a[3]*end[3];
    if (dot < 0.0f) {
        dot = -dot;
        for (float& v : end) v = -v;
    }
    if (dot > 0.9995f) {
        float length_sq = 0.0f;
        for (int i = 0; i < 4; ++i) {
            out[i] = a[i] + (end[i] - a[i]) * t;
            length_sq += out[i] * out[i];
        }
        float inv_length = length_sq > 0.0f ? 1.0f / std::sqrt(length_sq) : 1.0f;
        for (int i = 0; i < 4; ++i) out[i] *= inv_length;
        return;
    }
    float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
    float sin_theta = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / sin_theta;
    float wb = std::sin(t * theta) / sin_theta;
    for (int i = 0; i < 4; ++i) out[i] = a[i] * wa + end[i] * wb;
}

static void get_node_matrix_internal(const PODModel& model, int node_idx, float frame, float* mOut, int depth) {
    if (depth > 64 || node_idx < 0 || node_idx >= (int)model.nodes.size()) {
        std::memset(mOut, 0, 16 * sizeof(float));
        mOut[0] = mOut[5] = mOut[10] = mOut[15] = 1.0f;
        return;
    }

    const auto& node = model.nodes[node_idx];
    const int whole_frame = std::max(0, static_cast<int>(frame));
    const int next_frame = model.num_frames > 0
        ? std::min(whole_frame + 1, model.num_frames - 1) : whole_frame;
    const float fraction = std::clamp(frame - static_cast<float>(whole_frame), 0.0f, 1.0f);

    auto stream_has_animation = [&](uint32_t flag, size_t values, int components) {
        return values >= static_cast<size_t>(components) &&
               ((node.anim_flags & flag) != 0 || node.anim_flags == 0);
    };

    float local[16];
    local_mat4_identity(local);

    const bool has_anim_matrix = stream_has_animation(8u, node.anim_matrix.size(), 16);
    const bool has_anim_translation = stream_has_animation(1u, node.anim_translation.size(), 3);
    const bool has_anim_rotation = stream_has_animation(2u, node.anim_rotation.size(), 4);
    const bool has_anim_scale = stream_has_animation(4u, node.anim_scale.size(), 3);

    if (has_anim_matrix) {
        int key = animation_key_index(node.anim_matrix_idx, node.anim_matrix.size(), whole_frame, 16);
        if (key >= 0) std::memcpy(local, &node.anim_matrix[key * 16], sizeof(local));
    } else if (node.has_matrix) {
        std::memcpy(local, node.matrix, sizeof(local));
    } else {
        float S[16], R[16], T[16];
        local_mat4_identity(S);
        local_mat4_identity(R);
        local_mat4_identity(T);

        float t_val[3] = {node.translation[0], node.translation[1], node.translation[2]};
        if (!node.anim_translation.empty())
            std::memcpy(t_val, node.anim_translation.data(), sizeof(t_val));
        if (has_anim_translation) {
            int key0 = animation_key_index(node.anim_translation_idx,
                                            node.anim_translation.size(), whole_frame, 3);
            int key1 = animation_key_index(node.anim_translation_idx,
                                            node.anim_translation.size(), next_frame, 3);
            if (key0 >= 0 && key1 >= 0)
                lerp3(t_val, &node.anim_translation[key0 * 3],
                      &node.anim_translation[key1 * 3], fraction);
        }
        T[12] = t_val[0];
        T[13] = t_val[1];
        T[14] = t_val[2];

        float r_val[4] = {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
        if (!node.anim_rotation.empty())
            std::memcpy(r_val, node.anim_rotation.data(), sizeof(r_val));
        if (has_anim_rotation) {
            int key0 = animation_key_index(node.anim_rotation_idx,
                                            node.anim_rotation.size(), whole_frame, 4);
            int key1 = animation_key_index(node.anim_rotation_idx,
                                            node.anim_rotation.size(), next_frame, 4);
            if (key0 >= 0 && key1 >= 0)
                slerp_quat(r_val, &node.anim_rotation[key0 * 4],
                          &node.anim_rotation[key1 * 4], fraction);
        }
        local_mat4_from_quat(r_val, R);

        float s_val[3] = {node.scale[0], node.scale[1], node.scale[2]};
        if (node.anim_scale.size() >= 3)
            std::memcpy(s_val, node.anim_scale.data(), sizeof(s_val));
        if (has_anim_scale) {
            int stride = (node.anim_scale.size() % 7 == 0) ? 7 : 3;
            int key0 = animation_key_index(node.anim_scale_idx,
                                            node.anim_scale.size(), whole_frame, stride);
            int key1 = animation_key_index(node.anim_scale_idx,
                                            node.anim_scale.size(), next_frame, stride);
            if (key0 >= 0 && key1 >= 0)
                lerp3(s_val, &node.anim_scale[key0 * stride],
                      &node.anim_scale[key1 * stride], fraction);
        }
        S[0] = s_val[0];
        S[5] = s_val[1];
        S[10] = s_val[2];

        float temp[16];
        local_mat4_mul(T, R, temp);
        local_mat4_mul(temp, S, local);
    }

    if (node.parent_index != -1 && node.parent_index != node_idx) {
        float parent_world[16];
        get_node_matrix_internal(model, node.parent_index, frame, parent_world, depth + 1);
        local_mat4_mul(parent_world, local, mOut);
    } else {
        std::memcpy(mOut, local, sizeof(local));
    }
}

void get_node_matrix(const PODModel& model, int node_idx, float frame, float* mOut) {
    get_node_matrix_internal(model, node_idx, frame, mOut, 0);
}

float pod_feet_offset(const PODModel& model) {
    // Measure the lowest rendered vertex at REST pose, not animation frame 0:
    // for merged animation PODs frame 0 may be mid-stride (legs split, feet
    // higher/lower than standing). Nodes captured a TRUE bind matrix at merge
    // time — use it when present, fall back to frame 0, then apply the
    // center-point shift exactly like the render path (centerOffset * node_m).
    // NOTE: assumes the lowest vertex is the FEET (no trailing weapon tip
    // below them) — true for hiro (sword node bottoms at +9.98 vs feet −35.14).
    float min_y = 0.0f;
    bool  any   = false;
    const float cy = model.has_center_point ? -model.center_point[1] : 0.0f;
    for (int ni = 0; ni < (int)model.nodes.size() && ni < model.num_mesh_nodes; ++ni) {
        const auto& node = model.nodes[ni];
        if (node.object_index < 0 || node.object_index >= (int)model.meshes.size())
            continue;
        const auto& mesh = model.meshes[node.object_index];
        if (mesh.positions.empty()) continue;
        float mm[16];
        if (node.has_bind_matrix)
            std::memcpy(mm, node.bind_matrix, sizeof(mm));
        else
            get_node_matrix(model, ni, 0.0f, mm);
        const float xs[2] = {mesh.min_x, mesh.max_x};
        const float ys[2] = {mesh.min_y, mesh.max_y};
        const float zs[2] = {mesh.min_z, mesh.max_z};
        for (float x : xs)
            for (float y : ys)
                for (float z : zs) {
                    const float oy = mm[1]*x + mm[5]*y + mm[9]*z + mm[13] + cy;
                    min_y = std::min(min_y, oy);
                }
        any = true;
    }
    if (any) return -min_y;
    // Node-less PODs: raw mesh-space bounds.
    for (const auto& mesh : model.meshes) {
        if (mesh.positions.empty()) continue;
        min_y = std::min(min_y, mesh.min_y);
        any = true;
    }
    return any ? -min_y : 0.0f;
}

bool pod_anim_feet(const PODModel& model, std::vector<float>& out) {
    out.clear();
    const float cy = model.has_center_point ? model.center_point[1] : 0.0f;
    const int nf = std::max(model.num_frames, 1);
    for (int f = 0; f < nf; ++f) {
        float min_y = 0.0f;
        bool  any   = false;
        for (int ni = 0; ni < (int)model.nodes.size() && ni < model.num_mesh_nodes; ++ni) {
            const auto& node = model.nodes[ni];
            if (node.object_index < 0 || node.object_index >= (int)model.meshes.size())
                continue;
            const auto& mesh = model.meshes[node.object_index];
            if (mesh.positions.empty()) continue;
            std::vector<float> sp, sn;
            const bool skinned = skin_mesh(model, ni, (float)f, sp, sn);
            const std::vector<float>& src = skinned ? sp : mesh.positions;
            float mm[16];
            get_node_matrix(model, ni, (float)f, mm);
            for (size_t i = 0; i + 2 < src.size(); i += 3) {
                const float oy = mm[1]*src[i] + mm[5]*src[i+1] + mm[9]*src[i+2]
                               + mm[13] - cy;
                min_y = std::min(min_y, oy);
                any   = true;
            }
        }
        out.push_back(any ? -min_y : 0.0f);
    }
    return !out.empty();
}

bool skin_mesh(const PODModel& model, int mesh_node_idx, float frame,
               std::vector<float>& positions, std::vector<float>& normals) {
    if (mesh_node_idx < 0 || mesh_node_idx >= static_cast<int>(model.nodes.size())) return false;
    const auto& mesh_node = model.nodes[mesh_node_idx];
    if (mesh_node.object_index < 0 || mesh_node.object_index >= static_cast<int>(model.meshes.size())) return false;
    const auto& mesh = model.meshes[mesh_node.object_index];
    if (!mesh.bones_per_vertex || mesh.bone_indices.size() < static_cast<size_t>(mesh.num_vertices * mesh.bones_per_vertex)) return false;

    positions.assign(mesh.positions.size(), 0.0f);
    normals.assign(mesh.normals.size(), 0.0f);
    float mesh_world[16];
    get_node_matrix(model, mesh_node_idx, frame, mesh_world);
    float mesh_inverse[16];
    if (!local_mat4_inverse(mesh_world, mesh_inverse)) return false;

    std::vector<float> bind_world(model.nodes.size() * 16), current_world(model.nodes.size() * 16);
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        // Bind = the pose the mesh was exported in. For merged animation
        // models this is the base model's rest pose (captured at merge time),
        // NOT animation frame 0 — using frame 0 there renders the baked
        // T-pose at frame 0 and offsets every animated pose.
        if (model.nodes[i].has_bind_matrix) {
            std::memcpy(&bind_world[i * 16], model.nodes[i].bind_matrix, 16 * sizeof(float));
        } else {
            get_node_matrix(model, static_cast<int>(i), 0.0f, &bind_world[i * 16]);
        }
        get_node_matrix(model, static_cast<int>(i), frame, &current_world[i * 16]);
    }
    std::vector<float> skin_matrices(model.nodes.size() * 16);
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        float inverse_bind[16];
        if (!local_mat4_inverse(&bind_world[i * 16], inverse_bind)) local_mat4_identity(inverse_bind);
        float animated_from_bind[16], animated_model[16];
        local_mat4_mul(&current_world[i * 16], inverse_bind, animated_from_bind);
        local_mat4_mul(animated_from_bind, &bind_world[static_cast<size_t>(mesh_node_idx) * 16], animated_model);
        local_mat4_mul(mesh_inverse, animated_model, &skin_matrices[i * 16]);
    }

    const int influence_count = mesh.bones_per_vertex;
    for (int vertex = 0; vertex < mesh.num_vertices; ++vertex) {
        float weight_sum = 0.0f;
        for (int influence = 0; influence < influence_count; ++influence) {
            const size_t index = static_cast<size_t>(vertex * influence_count + influence);
            int bone = static_cast<int>(mesh.bone_indices[index]);
            if (mesh.has_bone_batches && !mesh.bone_batches.indices.empty() && bone >= 0 &&
                bone < static_cast<int>(mesh.bone_batches.indices.size()))
                bone = static_cast<int>(mesh.bone_batches.indices[bone]);
            if (bone < 0 || bone >= static_cast<int>(model.nodes.size())) continue;
            float weight = mesh.bone_weights.size() > index ? mesh.bone_weights[index] : 0.0f;
            if (weight <= 0.0f) continue;
            weight_sum += weight;
            const float* matrix = &skin_matrices[static_cast<size_t>(bone) * 16];
            const float* point = &mesh.positions[static_cast<size_t>(vertex) * 3];
            float* result = &positions[static_cast<size_t>(vertex) * 3];
            result[0] += (matrix[0]*point[0] + matrix[4]*point[1] + matrix[8]*point[2] + matrix[12]) * weight;
            result[1] += (matrix[1]*point[0] + matrix[5]*point[1] + matrix[9]*point[2] + matrix[13]) * weight;
            result[2] += (matrix[2]*point[0] + matrix[6]*point[1] + matrix[10]*point[2] + matrix[14]) * weight;
            if (!mesh.normals.empty()) {
                const float* normal = &mesh.normals[static_cast<size_t>(vertex) * 3];
                float* normal_result = &normals[static_cast<size_t>(vertex) * 3];
                normal_result[0] += (matrix[0]*normal[0] + matrix[4]*normal[1] + matrix[8]*normal[2]) * weight;
                normal_result[1] += (matrix[1]*normal[0] + matrix[5]*normal[1] + matrix[9]*normal[2]) * weight;
                normal_result[2] += (matrix[2]*normal[0] + matrix[6]*normal[1] + matrix[10]*normal[2]) * weight;
            }
        }
        if (weight_sum <= 0.0f) {
            std::memcpy(&positions[static_cast<size_t>(vertex) * 3], &mesh.positions[static_cast<size_t>(vertex) * 3], 3 * sizeof(float));
            if (!mesh.normals.empty()) std::memcpy(&normals[static_cast<size_t>(vertex) * 3], &mesh.normals[static_cast<size_t>(vertex) * 3], 3 * sizeof(float));
        } else if (!normals.empty()) {
            float* normal = &normals[static_cast<size_t>(vertex) * 3];
            float length = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
            if (length > 1e-8f) { normal[0] /= length; normal[1] /= length; normal[2] /= length; }
        }
    }
    return true;
}

static void transform_point(const float m[16], float x, float y, float z, float out[3]) {
    out[0] = m[0]*x + m[4]*y + m[8]*z + m[12];
    out[1] = m[1]*x + m[5]*y + m[9]*z + m[13];
    out[2] = m[2]*x + m[6]*y + m[10]*z + m[14];
}

static void finalize_model_bounds(PODModel& model) {
    model.total_vertices = model.total_faces = 0;
    model.min_x = model.min_y = model.min_z = 1e9f;
    model.max_x = model.max_y = model.max_z = -1e9f;
    model.has_center_point = false;

    for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
        if (model.nodes[i].name != "CenterPoint") continue;
        float matrix[16];
        get_node_matrix(model, i, 0.0f, matrix);
        model.center_point[0] = matrix[12];
        model.center_point[1] = matrix[13];
        model.center_point[2] = matrix[14];
        model.has_center_point = true;
        break;
    }

    for (const auto& mesh : model.meshes) {
        model.total_vertices += mesh.num_vertices;
        model.total_faces += mesh.num_faces;
    }

    for (int node_index = 0; node_index < model.num_mesh_nodes &&
         node_index < static_cast<int>(model.nodes.size()); ++node_index) {
        const auto& node = model.nodes[node_index];
        if (node.object_index < 0 || node.object_index >= static_cast<int>(model.meshes.size())) continue;
        const auto& mesh = model.meshes[node.object_index];
        if (mesh.positions.empty()) continue;
        float matrix[16];
        get_node_matrix(model, node_index, 0.0f, matrix);
        for (size_t vertex = 0; vertex + 2 < mesh.positions.size(); vertex += 3) {
            float point[3];
            transform_point(matrix, mesh.positions[vertex], mesh.positions[vertex+1],
                            mesh.positions[vertex+2], point);
            if (model.has_center_point) {
                point[0] -= model.center_point[0];
                point[1] -= model.center_point[1];
                point[2] -= model.center_point[2];
            }
            model.min_x = std::min(model.min_x, point[0]); model.max_x = std::max(model.max_x, point[0]);
            model.min_y = std::min(model.min_y, point[1]); model.max_y = std::max(model.max_y, point[1]);
            model.min_z = std::min(model.min_z, point[2]); model.max_z = std::max(model.max_z, point[2]);
        }
    }

    // Node-less PODs are uncommon but valid; preserve their mesh-space bounds.
    if (model.min_x > model.max_x) {
        for (const auto& mesh : model.meshes) {
            if (mesh.positions.empty()) continue;
            model.min_x = std::min(model.min_x, mesh.min_x); model.max_x = std::max(model.max_x, mesh.max_x);
            model.min_y = std::min(model.min_y, mesh.min_y); model.max_y = std::max(model.max_y, mesh.max_y);
            model.min_z = std::min(model.min_z, mesh.min_z); model.max_z = std::max(model.max_z, mesh.max_z);
        }
    }

    if (model.min_x <= model.max_x) {
        model.center_x = (model.min_x + model.max_x) * 0.5f;
        model.center_y = (model.min_y + model.max_y) * 0.5f;
        model.center_z = (model.min_z + model.max_z) * 0.5f;
        float dx = model.max_x - model.min_x, dy = model.max_y - model.min_y, dz = model.max_z - model.min_z;
        model.radius = std::max(0.5f * std::sqrt(dx*dx + dy*dy + dz*dz), 1.0f);
    }
}

} // namespace av
