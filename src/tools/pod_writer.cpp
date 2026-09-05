// pod_writer.cpp — PowerVR POD model serializer implementation
// Mirrors the chunk/tag grammar read by pod_loader.cpp (PODLoader.as).

#include "pod_writer.h"
#include "pod_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace av {

// ─── Tag Constants (must match pod_loader.cpp) ─────────────────────────
namespace {
constexpr uint32_t kEndTagMask = 0x80000000u;

constexpr uint32_t eFormatVersion               = 1000;
constexpr uint32_t eScene                       = 1001;

constexpr uint32_t eSceneClearColour            = 2000;
constexpr uint32_t eSceneAmbientColour          = 2001;
constexpr uint32_t eSceneNumCameras             = 2002;
constexpr uint32_t eSceneNumLights              = 2003;
constexpr uint32_t eSceneNumMeshes              = 2004;
constexpr uint32_t eSceneNumNodes               = 2005;
constexpr uint32_t eSceneNumMeshNodes           = 2006;
constexpr uint32_t eSceneNumTextures            = 2007;
constexpr uint32_t eSceneNumMaterials           = 2008;
constexpr uint32_t eSceneNumFrames              = 2009;
constexpr uint32_t eSceneMesh                   = 2012;
constexpr uint32_t eSceneNode                   = 2013;
constexpr uint32_t eSceneTexture                = 2014;
constexpr uint32_t eSceneMaterial               = 2015;
constexpr uint32_t eSceneFlags                  = 2016;
constexpr uint32_t eSceneFPS                    = 2017;

constexpr uint32_t eMaterialName                 = 3000;
constexpr uint32_t eMaterialDiffuseTextureIndex  = 3001;
constexpr uint32_t eMaterialOpacity              = 3002;
constexpr uint32_t eMaterialAmbient              = 3003;
constexpr uint32_t eMaterialDiffuse              = 3004;
constexpr uint32_t eMaterialSpecular             = 3005;
constexpr uint32_t eMaterialShininess            = 3006;

constexpr uint32_t eTextureFilename              = 4000;

constexpr uint32_t eNodeIndex                    = 5000;
constexpr uint32_t eNodeName                     = 5001;
constexpr uint32_t eNodeMaterialIndex            = 5002;
constexpr uint32_t eNodeParentIndex              = 5003;
constexpr uint32_t eNodePosition                 = 5004;
constexpr uint32_t eNodeRotation                 = 5005;
constexpr uint32_t eNodeScale                    = 5006;
constexpr uint32_t eNodeAnimationPosition        = 5007;
constexpr uint32_t eNodeAnimationRotation        = 5008;
constexpr uint32_t eNodeAnimationScale           = 5009;
constexpr uint32_t eNodeMatrix                   = 5010;
constexpr uint32_t eNodeAnimationMatrix           = 5011;
constexpr uint32_t eNodeAnimationFlags            = 5012;
constexpr uint32_t eNodeAnimationPositionIndex   = 5013;
constexpr uint32_t eNodeAnimationRotationIndex   = 5014;
constexpr uint32_t eNodeAnimationScaleIndex      = 5015;
constexpr uint32_t eNodeAnimationMatrixIndex     = 5016;

constexpr uint32_t eMeshNumVertices              = 6000;
constexpr uint32_t eMeshNumFaces                 = 6001;
constexpr uint32_t eMeshNumUVWChannels            = 6002;
constexpr uint32_t eMeshVertexIndexList          = 6003;
constexpr uint32_t eMeshVertexList               = 6006;
constexpr uint32_t eMeshNormalList               = 6007;
constexpr uint32_t eMeshUVWList                  = 6010;
constexpr uint32_t eMeshBoneIndexList            = 6012;
constexpr uint32_t eMeshBoneWeightList            = 6013;
constexpr uint32_t eMeshBoneBatchIndexList       = 6015;
constexpr uint32_t eMeshNumBoneIndicesPerBatch   = 6016;
constexpr uint32_t eMeshBoneOffsetPerBatch       = 6017;
constexpr uint32_t eMeshMaxNumBonesPerBatch      = 6018;
constexpr uint32_t eMeshNumBoneBatches           = 6019;

constexpr uint32_t eBlockDataType                = 9000;
constexpr uint32_t eBlockNumComponents           = 9001;
constexpr uint32_t eBlockStride                  = 9002;
constexpr uint32_t eBlockData                    = 9003;
} // namespace

// ─── Byte emitter ──────────────────────────────────────────────────────
namespace {

class Sink {
public:
    void u32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>(v));
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v >> 16));
        buf_.push_back(static_cast<uint8_t>(v >> 24));
    }
    void f32(float v) { std::memcpy(tmp_, &v, 4); u32(tmp_[0] | (tmp_[1] << 8) | (tmp_[2] << 16) | (tmp_[3] << 24)); }
    void bytes(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + n);
    }
    void floats(const std::vector<float>& v) { if (!v.empty()) bytes(v.data(), v.size() * sizeof(float)); }
    void u32s(const std::vector<uint32_t>& v) { if (!v.empty()) bytes(v.data(), v.size() * sizeof(uint32_t)); }
    void chunk(uint32_t tag, size_t payload_len) {
        u32(tag);
        u32(static_cast<uint32_t>(payload_len));
    }
    void str_field(uint32_t tag, const std::string& s) {
        u32(tag);
        u32(static_cast<uint32_t>(s.size() + 1)); // NUL-terminated like the SDK
        bytes(s.data(), s.size());
        buf_.push_back(0); // single trailing NUL byte
    }
    void end_tag(uint32_t tag) { chunk(tag | kEndTagMask, 0); }
    void begin(uint32_t tag) {
        u32(tag);
        u32(0); // In PowerVR POD, container blocks have length 0
    }
    void finish(uint32_t tag) {
        end_tag(tag);
    }
    const std::vector<uint8_t>& data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
    uint8_t tmp_[4];
};

void write_vertex_block(Sink& out, uint32_t block_id, const std::vector<float>& data, int components) {
    if (data.empty() || components <= 0) return;
    out.begin(block_id);
    out.u32(eBlockDataType);         out.u32(4);        out.u32(1);                    // type: float (1)
    out.u32(eBlockNumComponents);    out.u32(4);        out.u32(static_cast<uint32_t>(components));
    out.u32(eBlockStride);           out.u32(4);        out.u32(static_cast<uint32_t>(components * 4));
    out.u32(eBlockData);             out.u32(static_cast<uint32_t>(data.size() * 4)); out.floats(data);
    out.finish(block_id);
}

void write_bone_index_block(Sink& out, const std::vector<float>& data, int components) {
    if (data.empty() || components <= 0) return;
    out.begin(eMeshBoneIndexList);
    out.u32(eBlockDataType);         out.u32(4);        out.u32(2);                    // type: int (2)
    out.u32(eBlockNumComponents);    out.u32(4);        out.u32(static_cast<uint32_t>(components));
    out.u32(eBlockStride);           out.u32(4);        out.u32(static_cast<uint32_t>(components * 4));
    out.u32(eBlockData);             out.u32(static_cast<uint32_t>(data.size() * 4));
    for (float f : data) {
        out.u32(static_cast<uint32_t>(static_cast<int32_t>(std::lround(f))));
    }
    out.finish(eMeshBoneIndexList);
}

void write_index_block(Sink& out, const std::vector<uint32_t>& indices) {
    if (indices.empty()) return;
    out.begin(eMeshVertexIndexList);
    out.u32(eBlockDataType);         out.u32(4);        out.u32(3);                    // type: unsigned short (3) for GLES2 / Swordigo
    out.u32(eBlockNumComponents);    out.u32(4);        out.u32(1);
    out.u32(eBlockStride);           out.u32(4);        out.u32(2);
    out.u32(eBlockData);             out.u32(static_cast<uint32_t>(indices.size() * 2));
    for (uint32_t idx : indices) {
        uint16_t u16 = static_cast<uint16_t>(idx & 0xFFFF);
        out.bytes(&u16, 2);
    }
    out.finish(eMeshVertexIndexList);
}

void write_mesh(Sink& out, const PODMesh& m) {
    out.begin(eSceneMesh);
    out.u32(eMeshNumVertices);     out.u32(4); out.u32(static_cast<uint32_t>(m.num_vertices));
    out.u32(eMeshNumFaces);        out.u32(4); out.u32(static_cast<uint32_t>(m.num_faces));
    out.u32(eMeshNumUVWChannels);  out.u32(4); out.u32(static_cast<uint32_t>(m.uvs.empty() ? 0 : 1));

    if (m.has_bone_batches) {
        const auto& bb = m.bone_batches;
        if (bb.max_bones > 0) { out.u32(eMeshMaxNumBonesPerBatch); out.u32(4); out.u32(static_cast<uint32_t>(bb.max_bones)); }
        if (bb.count > 0)     { out.u32(eMeshNumBoneBatches);      out.u32(4); out.u32(static_cast<uint32_t>(bb.count)); }
        if (!bb.indices.empty()) {
            out.u32(eMeshBoneBatchIndexList);
            out.u32(static_cast<uint32_t>(bb.indices.size() * 4));
            out.u32s(bb.indices);
        }
        if (!bb.counts.empty()) {
            out.u32(eMeshNumBoneIndicesPerBatch);
            out.u32(static_cast<uint32_t>(bb.counts.size() * 4));
            out.u32s(bb.counts);
        }
        if (!bb.offsets.empty()) {
            out.u32(eMeshBoneOffsetPerBatch);
            out.u32(static_cast<uint32_t>(bb.offsets.size() * 4));
            out.u32s(bb.offsets);
        }
    }

    write_index_block(out, m.indices);
    write_vertex_block(out, eMeshVertexList, m.positions, 3);
    write_vertex_block(out, eMeshNormalList, m.normals, 3);
    if (!m.uvs.empty()) {
        write_vertex_block(out, eMeshUVWList, m.uvs, 2);
    }
    if (m.bones_per_vertex > 0) {
        write_bone_index_block(out, m.bone_indices, m.bones_per_vertex);
        write_vertex_block(out, eMeshBoneWeightList, m.bone_weights, m.bones_per_vertex);
    }
    out.finish(eSceneMesh);
}

void write_node(Sink& out, const PODNode& n) {
    out.begin(eSceneNode);
    out.u32(eNodeIndex);         out.u32(4); out.u32(static_cast<uint32_t>(n.object_index));
    out.str_field(eNodeName, n.name.empty() ? "Node" : n.name);
    out.u32(eNodeMaterialIndex); out.u32(4); out.u32(static_cast<uint32_t>(n.material_index));
    out.u32(eNodeParentIndex);   out.u32(4); out.u32(static_cast<uint32_t>(n.parent_index));
    out.u32(eNodeAnimationFlags);out.u32(4); out.u32(n.anim_flags);

    if (n.has_translation) { out.u32(eNodePosition); out.u32(12); out.floats({n.translation[0], n.translation[1], n.translation[2]}); }
    if (n.has_rotation)    { out.u32(eNodeRotation); out.u32(16); out.floats({n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]}); }
    if (n.has_scale)       { out.u32(eNodeScale);    out.u32(12); out.floats({n.scale[0], n.scale[1], n.scale[2]}); }
    if (n.has_matrix)      { out.u32(eNodeMatrix);   out.u32(64); out.floats(std::vector<float>(n.matrix, n.matrix + 16)); }

    if (!n.anim_translation.empty())        { out.u32(eNodeAnimationPosition);      out.u32(static_cast<uint32_t>(n.anim_translation.size() * 4)); out.floats(n.anim_translation); }
    if (!n.anim_rotation.empty())           { out.u32(eNodeAnimationRotation);       out.u32(static_cast<uint32_t>(n.anim_rotation.size() * 4));    out.floats(n.anim_rotation); }
    if (!n.anim_scale.empty())              { out.u32(eNodeAnimationScale);          out.u32(static_cast<uint32_t>(n.anim_scale.size() * 4));       out.floats(n.anim_scale); }
    if (!n.anim_matrix.empty())             { out.u32(eNodeAnimationMatrix);         out.u32(static_cast<uint32_t>(n.anim_matrix.size() * 4));      out.floats(n.anim_matrix); }
    if (!n.anim_translation_idx.empty())    { out.u32(eNodeAnimationPositionIndex); out.u32(static_cast<uint32_t>(n.anim_translation_idx.size() * 4)); out.u32s(n.anim_translation_idx); }
    if (!n.anim_rotation_idx.empty())       { out.u32(eNodeAnimationRotationIndex); out.u32(static_cast<uint32_t>(n.anim_rotation_idx.size() * 4));    out.u32s(n.anim_rotation_idx); }
    if (!n.anim_scale_idx.empty())          { out.u32(eNodeAnimationScaleIndex);    out.u32(static_cast<uint32_t>(n.anim_scale_idx.size() * 4));       out.u32s(n.anim_scale_idx); }
    if (!n.anim_matrix_idx.empty())         { out.u32(eNodeAnimationMatrixIndex);   out.u32(static_cast<uint32_t>(n.anim_matrix_idx.size() * 4));      out.u32s(n.anim_matrix_idx); }

    out.finish(eSceneNode);
}

void write_texture(Sink& out, const std::string& name) {
    out.begin(eSceneTexture);
    if (!name.empty()) out.str_field(eTextureFilename, name);
    out.finish(eSceneTexture);
}

void write_material(Sink& out, const PODMaterial& mat) {
    out.begin(eSceneMaterial);
    out.str_field(eMaterialName, mat.name.empty() ? "Default" : mat.name);
    out.u32(eMaterialDiffuseTextureIndex); out.u32(4); out.u32(static_cast<uint32_t>(mat.diffuse_texture_index));
    out.u32(eMaterialOpacity);            out.u32(4); out.f32(mat.opacity > 0.0f ? mat.opacity : 1.0f);
    out.u32(eMaterialAmbient);            out.u32(12); out.floats({0.2f, 0.2f, 0.2f});
    out.u32(eMaterialDiffuse);            out.u32(12); out.floats({mat.diffuse[0], mat.diffuse[1], mat.diffuse[2]});
    out.u32(eMaterialSpecular);           out.u32(12); out.floats({0.0f, 0.0f, 0.0f});
    out.u32(eMaterialShininess);          out.u32(4);  out.f32(0.0f);
    out.finish(eSceneMaterial);
}

} // namespace

bool pod_write(const PODModel& model, const std::string& path, std::string* err) {
    Sink out;

    // Top-level version block.
    out.u32(eFormatVersion);
    out.u32(11);
    out.bytes("AB.POD.2.0\0", 11);
    out.end_tag(eFormatVersion);

    // Scene block.
    out.begin(eScene);
    out.u32(eSceneClearColour);   out.u32(12); out.floats({0.0f, 0.0f, 0.0f});
    out.u32(eSceneAmbientColour); out.u32(12); out.floats({0.2f, 0.2f, 0.2f});
    out.u32(eSceneNumCameras);    out.u32(4);  out.u32(0);
    out.u32(eSceneNumLights);     out.u32(4);  out.u32(0);
    out.u32(eSceneNumMeshes);     out.u32(4);  out.u32(static_cast<uint32_t>(model.meshes.size()));
    out.u32(eSceneNumNodes);      out.u32(4);  out.u32(static_cast<uint32_t>(model.nodes.size()));
    out.u32(eSceneNumMeshNodes);  out.u32(4);  out.u32(static_cast<uint32_t>(std::max(0, model.num_mesh_nodes)));
    out.u32(eSceneNumTextures);   out.u32(4);  out.u32(static_cast<uint32_t>(model.texture_filenames.size()));
    out.u32(eSceneNumMaterials);  out.u32(4);  out.u32(static_cast<uint32_t>(model.materials.size()));
    out.u32(eSceneNumFrames);     out.u32(4);  out.u32(static_cast<uint32_t>(std::max(0, model.num_frames)));
    out.u32(eSceneFPS);           out.u32(4);  out.u32(static_cast<uint32_t>(model.fps));
    out.u32(eSceneFlags);         out.u32(4);  out.u32(0);

    for (const auto& m : model.materials) write_material(out, m);
    for (const auto& t : model.texture_filenames) write_texture(out, t);
    for (const auto& m : model.meshes)    write_mesh(out, m);
    for (const auto& n : model.nodes)     write_node(out, n);
    out.finish(eScene);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        if (err) *err = "cannot open output file: " + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(out.data().data()), static_cast<std::streamsize>(out.data().size()));
    if (!f) {
        if (err) *err = "write failed for " + path;
        return false;
    }
    return true;
}

} // namespace av
