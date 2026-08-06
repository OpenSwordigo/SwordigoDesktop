// gltf_export.cpp — glTF 2.0 GLB exporter for POD models.
// Produces a self-contained .glb: JSON chunk + binary chunk, with vertex
// buffers, indices, skin joint/weight data, animations and embedded PNG
// textures. Coordinate systems match (both Y-up) so geometry passes through.
//
// Skinning mapping (reproduces av::skin_mesh's exact transform):
//   glTF mesh node transform = identity
//   inverseBindMatrix[bone]  = invBindWorld[bone] * bindWorld[meshNode]
//   joints                   = the POD node indices used by the mesh

#include "gltf_glb.h"
#include "pod_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "stb/stb_image_write.h"

namespace av {
namespace {

// ─── JSON writer ───────────────────────────────────────────────────────
class Json {
public:
    void key(const std::string& k) { sep(); s_ += "\"" + escape(k) + "\":"; }
    void begin_obj() { sep(); s_ += "{"; }
    void end_obj() { s_ += "}"; }
    void begin_arr() { sep(); s_ += "["; }
    void end_arr() { s_ += "]"; }
    void num(double v) { sep(); char b[64]; std::snprintf(b, sizeof(b), "%g", v); s_ += b; }
    void num(int v) { sep(); s_ += std::to_string(v); }
    void str(const std::string& v) { sep(); s_ += "\"" + escape(v) + "\""; }
    void boolean(bool v) { sep(); s_ += v ? "true" : "false"; }
    void raw(const std::string& v) { sep(); s_ += v; }
    std::string str() const { return s_; }

private:
    void sep() {
        if (s_.empty()) return;
        char last = s_.back();
        if (last == '{' || last == '[' || last == ',' || last == ':') return;
        s_ += ",";
    }
    static std::string escape(const std::string& in) {
        std::string out;
        for (char c : in) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if ((unsigned char)c < 0x20) {
                char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c); out += b;
            } else out += c;
        }
        return out;
    }
    std::string s_;
};

// ─── Matrix helpers ────────────────────────────────────────────────────
void mat_mul(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            tmp[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
    std::memcpy(out, tmp, sizeof(tmp));
}

bool mat_inverse(const float in[16], float out[16]) {
    float a[16]; std::memcpy(a, in, sizeof(a));
    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(out, id, sizeof(id));
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row)
            if (std::fabs(a[col * 4 + row]) > std::fabs(a[col * 4 + pivot])) pivot = row;
        if (std::fabs(a[col * 4 + pivot]) < 1e-9f) return false;
        if (pivot != col)
            for (int c = 0; c < 4; ++c) { std::swap(a[c * 4 + col], a[c * 4 + pivot]); std::swap(out[c * 4 + col], out[c * 4 + pivot]); }
        float s = a[col * 4 + col];
        for (int c = 0; c < 4; ++c) { a[c * 4 + col] /= s; out[c * 4 + col] /= s; }
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            float f = a[col * 4 + row];
            for (int c = 0; c < 4; ++c) { a[c * 4 + row] -= f * a[c * 4 + col]; out[c * 4 + row] -= f * out[c * 4 + col]; }
        }
    }
    return true;
}

void mat_decompose(const float m[16], float t[3], float q[4], float s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    s[0] = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    s[1] = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    s[2] = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    if (s[0] < 1e-8f) s[0] = 1.0f; if (s[1] < 1e-8f) s[1] = 1.0f; if (s[2] < 1e-8f) s[2] = 1.0f;
    float r[9] = {m[0]/s[0], m[1]/s[0], m[2]/s[0],
                  m[4]/s[1], m[5]/s[1], m[6]/s[1],
                  m[8]/s[2], m[9]/s[2], m[10]/s[2]};
    float trace = r[0] + r[5] + r[10];
    if (trace > 0.0f) {
        float S = std::sqrt(trace + 1.0f) * 2.0f;
        q[0] = (r[7] - r[6]) / S; q[1] = (r[2] - r[8]) / S; q[2] = (r[3] - r[1]) / S; q[3] = 0.25f * S;
    } else if (r[0] > r[5] && r[0] > r[10]) {
        float S = std::sqrt(1.0f + r[0] - r[5] - r[10]) * 2.0f;
        q[0] = 0.25f * S; q[1] = (r[3] + r[1]) / S; q[2] = (r[2] + r[8]) / S; q[3] = (r[7] - r[6]) / S;
    } else if (r[5] > r[10]) {
        float S = std::sqrt(1.0f + r[5] - r[0] - r[10]) * 2.0f;
        q[0] = (r[3] + r[1]) / S; q[1] = 0.25f * S; q[2] = (r[7] + r[6]) / S; q[3] = (r[2] - r[8]) / S;
    } else {
        float S = std::sqrt(1.0f + r[10] - r[0] - r[5]) * 2.0f;
        q[0] = (r[2] + r[8]) / S; q[1] = (r[7] + r[6]) / S; q[2] = 0.25f * S; q[3] = (r[3] - r[1]) / S;
    }
    float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-8f) { q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n; }
    if (n == 0.0f) { q[3] = 1.0f; }
}

// ─── Static node transform from POD (frame-0 animation streams) ─────────
void pod_static_transform(const PODNode& n, bool& has_trs, float t[3], float q[4], float s[3],
                          bool& has_mat, float m[16]) {
    has_trs = false;
    has_mat = false;
    if (n.has_matrix) {
        has_mat = true;
        std::memcpy(m, n.matrix, sizeof(float) * 16);
        return;
    }
    // POD stores even static transforms as 1-frame animation streams.
    if (!n.anim_translation.empty()) { t[0]=n.anim_translation[0]; t[1]=n.anim_translation[1]; t[2]=n.anim_translation[2]; has_trs = true; }
    else if (n.has_translation)      { t[0]=n.translation[0]; t[1]=n.translation[1]; t[2]=n.translation[2]; has_trs = true; }
    if (!n.anim_rotation.empty()) { q[0]=n.anim_rotation[0]; q[1]=n.anim_rotation[1]; q[2]=n.anim_rotation[2]; q[3]=n.anim_rotation[3]; has_trs = true; }
    else if (n.has_rotation)      { q[0]=n.rotation[0]; q[1]=n.rotation[1]; q[2]=n.rotation[2]; q[3]=n.rotation[3]; has_trs = true; }
    if (!n.anim_scale.empty()) { s[0]=n.anim_scale[0]; s[1]=n.anim_scale[1]; s[2]=n.anim_scale[2]; has_trs = true; }
    else if (n.has_scale)      { s[0]=n.scale[0]; s[1]=n.scale[1]; s[2]=n.scale[2]; has_trs = true; }
}

// ─── Binary assembly ───────────────────────────────────────────────────
struct Bin {
    std::vector<uint8_t> data;
    size_t align(size_t n) { while (data.size() % n != 0) data.push_back(0); return data.size(); }
    size_t push(const void* p, size_t n) {
        size_t off = align(4);
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
        return off;
    }
};

struct Accessor {
    int component_type = 5126; // 5123 ushort, 5125 uint, 5126 float
    std::string type = "VEC3";
    int count = 0;
    bool normalized = false;
    bool indices = false;
    std::vector<uint8_t> payload;
};

struct Primitive {
    int material = -1;
    int pos = -1, nrm = -1, uv = -1, joints = -1, weights = -1, idx = -1;
};

struct AnimData {
    int node = -1;
    int time_acc = -1;
    int pos_acc = -1, rot_acc = -1, scl_acc = -1;
};

} // namespace

bool gltf_export_glb(const PODModel& model,
                     const std::vector<GLTFTextureImage>& images,
                     const std::string& output_path,
                     std::string* err) {
    std::vector<Accessor> accs;
    auto add_accessor = [&](Accessor a) -> int {
        int idx = static_cast<int>(accs.size());
        accs.push_back(std::move(a));
        return idx;
    };

    // ── Build glTF nodes in POD order ──────────────────────────────────
    // node_transform[i]: 0 none, 1 TRS, 2 matrix
    std::vector<int> node_transform(model.nodes.size(), 0);
    std::vector<float> node_t(model.nodes.size() * 3, 0.0f);
    std::vector<float> node_q(model.nodes.size() * 4, 0.0f);
    std::vector<float> node_s(model.nodes.size() * 3, 1.0f);
    std::vector<float> node_m(model.nodes.size() * 16, 0.0f);
    std::vector<int> node_children(model.nodes.size(), -1); // linked list of children

    // Children: index of first child + siblings. Build with vectors.
    std::vector<std::vector<int>> children(model.nodes.size());

    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        bool has_trs = false, has_mat = false;
        float t[3], q[4], s[3], m[16];
        pod_static_transform(n, has_trs, t, q, s, has_mat, m);
        if (has_trs) {
            node_transform[i] = 1;
            std::memcpy(&node_t[i * 3], t, sizeof(t));
            std::memcpy(&node_q[i * 4], q, sizeof(q));
            std::memcpy(&node_s[i * 3], s, sizeof(s));
        } else if (has_mat) {
            node_transform[i] = 2;
            std::memcpy(&node_m[i * 16], m, sizeof(m));
        }
        if (n.parent_index >= 0 && n.parent_index < (int)children.size())
            children[n.parent_index].push_back(static_cast<int>(i));
    }

    // ── Skins ──────────────────────────────────────────────────────────
    struct SkinInfo { int mesh_node = -1; std::vector<int> joints; int ibm_acc = -1; };
    std::vector<SkinInfo> skins;
    std::vector<int> skin_of_node(model.nodes.size(), -1);
    std::vector<int> mesh_index_of_node(model.nodes.size(), -1);

    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const auto& node = model.nodes[ni];
        if (node.object_index < 0 || node.object_index >= (int)model.meshes.size()) continue;
        const auto& mesh = model.meshes[node.object_index];
        if (mesh.bones_per_vertex <= 0 || mesh.bone_indices.empty()) continue;

        SkinInfo skin;
        skin.mesh_node = static_cast<int>(ni);
        if (mesh.has_bone_batches && !mesh.bone_batches.indices.empty()) {
            // JOINTS_0 holds batch-local indices; skin.joints must cover every
            // value actually used (indices array may be zero-padded to max_bones).
            int max_local = 0;
            for (float b : mesh.bone_indices) max_local = std::max(max_local, (int)b);
            for (int k = 0; k <= max_local && k < (int)mesh.bone_batches.indices.size(); ++k)
                skin.joints.push_back(static_cast<int>(mesh.bone_batches.indices[k]));
        } else {
            int max_bone = 0;
            for (float b : mesh.bone_indices) max_bone = std::max(max_bone, (int)b);
            for (int k = 0; k <= max_bone; ++k) skin.joints.push_back(k);
        }
        // inverseBindMatrix[bone] = invBindWorld[bone] * bindWorld[meshNode]
        float mesh_bind[16], mesh_bind_inv[16];
        get_node_matrix(model, static_cast<int>(ni), 0.0f, mesh_bind);
        if (!mat_inverse(mesh_bind, mesh_bind_inv)) {
            float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            std::memcpy(mesh_bind_inv, id, sizeof(id));
        }
        std::vector<float> ibm;
        ibm.reserve(skin.joints.size() * 16);
        for (int b : skin.joints) {
            float bind_world[16];
            get_node_matrix(model, b, 0.0f, bind_world);
            float bind_inv[16];
            if (!mat_inverse(bind_world, bind_inv)) {
                float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                std::memcpy(bind_inv, id, sizeof(id));
            }
            float prod[16];
            mat_mul(bind_inv, mesh_bind, prod);
            ibm.insert(ibm.end(), prod, prod + 16);
        }
        Accessor a;
        a.component_type = 5126; a.type = "MAT4"; a.count = static_cast<int>(skin.joints.size());
        a.payload.assign(reinterpret_cast<const uint8_t*>(ibm.data()),
                         reinterpret_cast<const uint8_t*>(ibm.data()) + sizeof(float) * ibm.size());
        skin.ibm_acc = add_accessor(std::move(a));
        skin_of_node[ni] = static_cast<int>(skins.size());
        skins.push_back(std::move(skin));
    }

    // ── Meshes & primitives ────────────────────────────────────────────
    std::vector<std::vector<Primitive>> gltf_meshes;
    // glTF mesh index per POD object_index (shared meshes are deduped).
    std::vector<int> mesh_index_of_object(model.meshes.size(), -1);
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const auto& node = model.nodes[ni];
        if (node.object_index < 0 || node.object_index >= (int)model.meshes.size()) continue;
        const auto& mesh = model.meshes[node.object_index];
        if (mesh_index_of_object[node.object_index] >= 0) {
            // node already seen; reference the existing glTF mesh.
            mesh_index_of_node[ni] = mesh_index_of_object[node.object_index];
            continue;
        }

        Primitive p;
        p.material = node.material_index;
        if (!mesh.positions.empty()) {
            Accessor a; a.component_type = 5126; a.type = "VEC3"; a.count = mesh.num_vertices;
            a.payload.assign((const uint8_t*)mesh.positions.data(), (const uint8_t*)mesh.positions.data() + mesh.positions.size() * 4);
            p.pos = add_accessor(std::move(a));
        }
        if (!mesh.normals.empty()) {
            Accessor a; a.component_type = 5126; a.type = "VEC3"; a.count = mesh.num_vertices;
            a.payload.assign((const uint8_t*)mesh.normals.data(), (const uint8_t*)mesh.normals.data() + mesh.normals.size() * 4);
            p.nrm = add_accessor(std::move(a));
        }
        if (!mesh.uvs.empty()) {
            Accessor a; a.component_type = 5126; a.type = "VEC2"; a.count = mesh.num_vertices;
            a.payload.assign((const uint8_t*)mesh.uvs.data(), (const uint8_t*)mesh.uvs.data() + mesh.uvs.size() * 4);
            p.uv = add_accessor(std::move(a));
        }
        if (!mesh.indices.empty()) {
            Accessor a; a.component_type = 5125; a.type = "SCALAR"; a.count = (int)mesh.indices.size(); a.indices = true;
            a.payload.assign((const uint8_t*)mesh.indices.data(), (const uint8_t*)mesh.indices.data() + mesh.indices.size() * 4);
            p.idx = add_accessor(std::move(a));
        }
        if (mesh.bones_per_vertex > 0 && !mesh.bone_indices.empty()) {
            const int n = mesh.num_vertices;
            std::vector<uint16_t> joints4(n * 4, 0);
            std::vector<float> weights4(n * 4, 0.0f);
            for (int v = 0; v < n; ++v) {
                float wsum = 0.0f;
                for (int k = 0; k < mesh.bones_per_vertex && k < 4; ++k) {
                    size_t idx = (size_t)v * mesh.bones_per_vertex + k;
                    joints4[v * 4 + k] = (uint16_t)(int)mesh.bone_indices[idx];
                    float w = mesh.bone_weights.size() > idx ? mesh.bone_weights[idx] : 0.0f;
                    weights4[v * 4 + k] = w; wsum += w;
                }
                if (wsum > 0.0f)
                    for (int k = 0; k < 4; ++k) weights4[v * 4 + k] /= wsum;
            }
            Accessor j; j.component_type = 5123; j.type = "VEC4"; j.count = n;
            j.payload.assign((const uint8_t*)joints4.data(), (const uint8_t*)joints4.data() + joints4.size() * 2);
            p.joints = add_accessor(std::move(j));
            Accessor w; w.component_type = 5126; w.type = "VEC4"; w.count = n;
            w.payload.assign((const uint8_t*)weights4.data(), (const uint8_t*)weights4.data() + weights4.size() * 4);
            p.weights = add_accessor(std::move(w));
        }

        int mesh_index = static_cast<int>(gltf_meshes.size());
        gltf_meshes.push_back({});
        gltf_meshes.back().push_back(p);
        mesh_index_of_node[ni] = mesh_index;
        mesh_index_of_object[node.object_index] = mesh_index;
    }

    // ── Animations ─────────────────────────────────────────────────────
    struct Channel { int node = -1; std::string path; int sampler = -1; };
    struct Sampler { int input = -1; int output = -1; std::string interp = "LINEAR"; };
    std::vector<Channel> channels;
    std::vector<Sampler> samplers;
    const float fps = model.fps > 0.0f ? model.fps : 30.0f;

    auto expand_stream = [&](const std::vector<float>& values, const std::vector<uint32_t>& idx, int stride, int frames) {
        if (values.empty() || frames <= 0) return std::vector<float>();
        std::vector<float> out;
        out.reserve(static_cast<size_t>(frames) * stride);
        int keys = (int)(values.size() / stride);
        for (int f = 0; f < frames; ++f) {
            int key = (f < (int)idx.size()) ? (int)idx[f] : f;
            key = std::clamp(key, 0, keys - 1);
            out.insert(out.end(), values.begin() + key * stride, values.begin() + key * stride + stride);
        }
        return out;
    };

    if (model.num_frames > 0) {
        for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
            const auto& node = model.nodes[ni];
            std::vector<float> posv, rotv, sclv;
            bool hp = false, hr = false, hs = false;
            if (!node.anim_translation.empty()) {
                posv = expand_stream(node.anim_translation, node.anim_translation_idx, 3, model.num_frames);
                hp = posv.size() >= (size_t)model.num_frames * 3;
            }
            if (!node.anim_rotation.empty()) {
                rotv = expand_stream(node.anim_rotation, node.anim_rotation_idx, 4, model.num_frames);
                hr = rotv.size() >= (size_t)model.num_frames * 4;
            }
            if (!node.anim_scale.empty()) {
                int stride = (node.anim_scale.size() % 7 == 0) ? 7 : 3;
                std::vector<float> full = expand_stream(node.anim_scale, node.anim_scale_idx, stride, model.num_frames);
                if (full.size() >= (size_t)model.num_frames * stride) {
                    sclv.resize((size_t)model.num_frames * 3);
                    for (int f = 0; f < model.num_frames; ++f) {
                        sclv[f * 3 + 0] = full[f * stride + 0];
                        sclv[f * 3 + 1] = full[f * stride + 1];
                        sclv[f * 3 + 2] = full[f * stride + 2];
                    }
                    hs = true;
                }
            }
            if (!node.anim_matrix.empty()) {
                std::vector<float> full = expand_stream(node.anim_matrix, node.anim_matrix_idx, 16, model.num_frames);
                if (full.size() >= (size_t)model.num_frames * 16) {
                    posv.resize((size_t)model.num_frames * 3);
                    rotv.resize((size_t)model.num_frames * 4);
                    sclv.resize((size_t)model.num_frames * 3);
                    for (int f = 0; f < model.num_frames; ++f) {
                        float t[3], q[4], s[3];
                        mat_decompose(&full[f * 16], t, q, s);
                        std::memcpy(&posv[f * 3], t, 12);
                        std::memcpy(&rotv[f * 4], q, 16);
                        std::memcpy(&sclv[f * 3], s, 12);
                    }
                    hp = hr = hs = true;
                }
            }
            if (!(hp || hr || hs)) continue;

            Accessor ta; ta.component_type = 5126; ta.type = "SCALAR"; ta.count = model.num_frames;
            {
                std::vector<float> times(model.num_frames);
                for (int f = 0; f < model.num_frames; ++f) times[f] = (float)f / fps;
                ta.payload.assign((const uint8_t*)times.data(), (const uint8_t*)times.data() + times.size() * 4);
            }
            int time_acc = add_accessor(std::move(ta));

            auto add_channel = [&](const std::vector<float>& vals, const std::string& path, const char* type, int comps) {
                Accessor a; a.component_type = 5126; a.type = type; a.count = model.num_frames;
                a.payload.assign((const uint8_t*)vals.data(), (const uint8_t*)vals.data() + vals.size() * 4);
                int out_acc = add_accessor(std::move(a));
                Sampler s; s.input = time_acc; s.output = out_acc; s.interp = "LINEAR";
                int sampler_idx = static_cast<int>(samplers.size());
                samplers.push_back(s);
                channels.push_back({static_cast<int>(ni), path, sampler_idx});
            };
            if (hp) add_channel(posv, "translation", "VEC3", 3);
            if (hr) add_channel(rotv, "rotation", "VEC4", 4);
            if (hs) add_channel(sclv, "scale", "VEC3", 3);
        }
    }

    // ── Place all accessor payloads into the binary buffer ─────────────
    Bin bin;
    std::vector<int> acc_view_offset(accs.size(), 0);
    for (size_t i = 0; i < accs.size(); ++i) {
        size_t off = bin.align(4);
        acc_view_offset[i] = static_cast<int>(off);
        bin.data.insert(bin.data.end(), accs[i].payload.begin(), accs[i].payload.end());
    }

    // ── Embed textures as PNG bufferViews after the accessor views ─────
    // texture index by POD texture filename.
    std::vector<int> gltf_tex_of(model.texture_filenames.size(), -1);
    std::vector<std::vector<uint8_t>> png_blobs;
    std::vector<int> png_view_offset;
    std::vector<std::string> png_names;
    {
        int img_idx = 0;
        for (size_t i = 0; i < model.texture_filenames.size(); ++i) {
            for (const auto& img : images) {
                if (img.name != model.texture_filenames[i] || img.rgba.empty() || img.w <= 0 || img.h <= 0) continue;
                std::vector<uint8_t> png;
                auto cb = [](void* ctx, void* data, int size) {
                    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
                    const uint8_t* p = static_cast<uint8_t*>(data);
                    out->insert(out->end(), p, p + size);
                };
                if (!stbi_write_png_to_func(cb, &png, img.w, img.h, 4, img.rgba.data(), img.w * 4) || png.empty()) break;
                size_t off = bin.align(4);
                png_view_offset.push_back(static_cast<int>(off));
                bin.data.insert(bin.data.end(), png.begin(), png.end());
                png_blobs.push_back(std::move(png));
                png_names.push_back(img.name);
                gltf_tex_of[i] = img_idx++;
                break;
            }
        }
    }

    // ── JSON emission ──────────────────────────────────────────────────
    Json j;
    j.begin_obj();
    j.key("asset");
    j.begin_obj();
    j.key("version"); j.str("2.0");
    j.key("generator"); j.str("Swordigo Ruby POD round-trip");
    j.end_obj();

    if (!gltf_meshes.empty()) {
        j.key("scene"); j.num(0);
        j.key("scenes");
        j.begin_arr();
        j.begin_obj();
        j.key("nodes");
        j.begin_arr();
        for (size_t i = 0; i < model.nodes.size(); ++i)
            if (model.nodes[i].parent_index < 0) { j.num(static_cast<int>(i)); }
        j.end_arr();
        j.end_obj();
        j.end_arr();
    }

    // nodes
    j.key("nodes");
    j.begin_arr();
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        j.begin_obj();
        if (!model.nodes[i].name.empty()) { j.key("name"); j.str(model.nodes[i].name); }
        if (mesh_index_of_node[i] >= 0) { j.key("mesh"); j.num(mesh_index_of_node[i]); }
        if (skin_of_node[i] >= 0) { j.key("skin"); j.num(skin_of_node[i]); }
        if (!children[i].empty()) {
            j.key("children");
            j.begin_arr();
            for (int c : children[i]) j.num(c);
            j.end_arr();
        }
        if (node_transform[i] == 1) {
            j.key("translation"); j.begin_arr(); j.num(node_t[i*3]); j.num(node_t[i*3+1]); j.num(node_t[i*3+2]); j.end_arr();
            j.key("rotation");    j.begin_arr(); j.num(node_q[i*4]); j.num(node_q[i*4+1]); j.num(node_q[i*4+2]); j.num(node_q[i*4+3]); j.end_arr();
            j.key("scale");       j.begin_arr(); j.num(node_s[i*3]); j.num(node_s[i*3+1]); j.num(node_s[i*3+2]); j.end_arr();
        } else if (node_transform[i] == 2) {
            j.key("matrix");
            j.begin_arr();
            for (int r = 0; r < 16; ++r) j.num(node_m[i*16 + r]);
            j.end_arr();
        }
        j.end_obj();
    }
    j.end_arr();

    // meshes
    if (!gltf_meshes.empty()) {
        j.key("meshes");
        j.begin_arr();
        for (const auto& prims : gltf_meshes) {
            j.begin_obj();
            j.key("primitives");
            j.begin_arr();
            for (const auto& p : prims) {
                j.begin_obj();
                if (p.material >= 0 && p.material < (int)model.materials.size()) { j.key("material"); j.num(p.material); }
                if (p.idx >= 0) { j.key("indices"); j.num(p.idx); }
                j.key("attributes");
                j.begin_obj();
                if (p.pos >= 0) { j.key("POSITION"); j.num(p.pos); }
                if (p.nrm >= 0) { j.key("NORMAL"); j.num(p.nrm); }
                if (p.uv >= 0)  { j.key("TEXCOORD_0"); j.num(p.uv); }
                if (p.joints >= 0) { j.key("JOINTS_0"); j.num(p.joints); }
                if (p.weights >= 0) { j.key("WEIGHTS_0"); j.num(p.weights); }
                j.end_obj();
                j.end_obj();
            }
            j.end_arr();
            j.end_obj();
        }
        j.end_arr();
    }

    // accessors, bufferViews, buffer
    j.key("accessors");
    j.begin_arr();
    for (size_t i = 0; i < accs.size(); ++i) {
        j.begin_obj();
        j.key("bufferView"); j.num(static_cast<int>(i));
        j.key("byteOffset"); j.num(0);
        j.key("componentType"); j.num(accs[i].component_type);
        if (accs[i].normalized) { j.key("normalized"); j.boolean(true); }
        j.key("count"); j.num(accs[i].count);
        j.key("type"); j.str(accs[i].type);
        j.end_obj();
    }
    j.end_arr();

    const int num_views = static_cast<int>(accs.size()) + static_cast<int>(png_view_offset.size());
    j.key("bufferViews");
    j.begin_arr();
    for (size_t i = 0; i < accs.size(); ++i) {
        j.begin_obj();
        j.key("buffer"); j.num(0);
        j.key("byteOffset"); j.num(acc_view_offset[i]);
        j.key("byteLength"); j.num(static_cast<int>(accs[i].payload.size()));
        if (accs[i].indices) { j.key("target"); j.num(34963); }
        else                 { j.key("target"); j.num(34962); }
        j.end_obj();
    }
    for (size_t i = 0; i < png_view_offset.size(); ++i) {
        j.begin_obj();
        j.key("buffer"); j.num(0);
        j.key("byteOffset"); j.num(png_view_offset[i]);
        j.key("byteLength"); j.num(static_cast<int>(png_blobs[i].size()));
        j.end_obj();
    }
    j.end_arr();

    j.key("buffers");
    j.begin_arr();
    j.begin_obj();
    j.key("byteLength"); j.num(static_cast<int>(bin.data.size()));
    j.end_obj();
    j.end_arr();

    // images / samplers / textures
    if (!png_blobs.empty()) {
        j.key("images");
        j.begin_arr();
        for (size_t i = 0; i < png_blobs.size(); ++i) {
            j.begin_obj();
            j.key("bufferView"); j.num(static_cast<int>(accs.size() + i));
            j.key("mimeType"); j.str("image/png");
            if (i < png_names.size() && !png_names[i].empty()) { j.key("name"); j.str(png_names[i]); }
            j.end_obj();
        }
        j.end_arr();

        j.key("samplers");
        j.begin_arr();
        j.begin_obj();
        j.key("magFilter"); j.num(9729);
        j.key("minFilter"); j.num(9987);
        j.key("wrapS"); j.num(10497);
        j.key("wrapT"); j.num(10497);
        j.end_obj();
        j.end_arr();

        j.key("textures");
        j.begin_arr();
        for (size_t i = 0; i < png_blobs.size(); ++i) {
            j.begin_obj();
            j.key("sampler"); j.num(0);
            j.key("source"); j.num(static_cast<int>(i));
            j.end_obj();
        }
        j.end_arr();
    }

    // materials
    j.key("materials");
    j.begin_arr();
    for (const auto& mat : model.materials) {
        j.begin_obj();
        if (!mat.name.empty()) { j.key("name"); j.str(mat.name); }
        j.key("pbrMetallicRoughness");
        j.begin_obj();
        if (mat.diffuse_texture_index >= 0 && mat.diffuse_texture_index < (int)gltf_tex_of.size() && gltf_tex_of[mat.diffuse_texture_index] >= 0) {
            j.key("baseColorTexture");
            j.begin_obj();
            j.key("index"); j.num(gltf_tex_of[mat.diffuse_texture_index]);
            j.end_obj();
        }
        j.key("baseColorFactor");
        j.begin_arr();
        j.num(mat.diffuse[0]); j.num(mat.diffuse[1]); j.num(mat.diffuse[2]); j.num(mat.opacity);
        j.end_arr();
        j.key("metallicFactor"); j.num(0.0);
        j.key("roughnessFactor"); j.num(1.0);
        j.end_obj();
        if (mat.opacity < 1.0f) { j.key("alphaMode"); j.str("BLEND"); }
        j.end_obj();
    }
    j.end_arr();

    // skins
    if (!skins.empty()) {
        j.key("skins");
        j.begin_arr();
        for (const auto& skin : skins) {
            j.begin_obj();
            j.key("inverseBindMatrices"); j.num(skin.ibm_acc);
            j.key("skeleton"); j.num(skin.mesh_node);
            j.key("joints");
            j.begin_arr();
            for (int b : skin.joints) j.num(b);
            j.end_arr();
            j.end_obj();
        }
        j.end_arr();
    }

    // animations
    if (!channels.empty()) {
        j.key("animations");
        j.begin_arr();
        j.begin_obj();
        j.key("name"); j.str("Animation");
        j.key("channels");
        j.begin_arr();
        for (const auto& ch : channels) {
            j.begin_obj();
            j.key("sampler"); j.num(ch.sampler);
            j.key("target");
            j.begin_obj();
            j.key("node"); j.num(ch.node);
            j.key("path"); j.str(ch.path);
            j.end_obj();
            j.end_obj();
        }
        j.end_arr();
        j.key("samplers");
        j.begin_arr();
        for (const auto& s : samplers) {
            j.begin_obj();
            j.key("input"); j.num(s.input);
            j.key("output"); j.num(s.output);
            j.key("interpolation"); j.str(s.interp);
            j.end_obj();
        }
        j.end_arr();
        j.end_obj();
        j.end_arr();
    }

    j.end_obj();

    // ── Assemble GLB container ─────────────────────────────────────────
    std::string json = j.str();
    while (json.size() % 4 != 0) json.push_back(' ');
    const uint32_t bin_padded = static_cast<uint32_t>((bin.data.size() + 3u) & ~3u);
    const uint32_t total = static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin_padded);
    std::vector<uint8_t> glb;
    auto put_u32 = [&](uint32_t v) {
        glb.push_back(static_cast<uint8_t>(v));
        glb.push_back(static_cast<uint8_t>(v >> 8));
        glb.push_back(static_cast<uint8_t>(v >> 16));
        glb.push_back(static_cast<uint8_t>(v >> 24));
    };
    glb.push_back('g'); glb.push_back('l'); glb.push_back('T'); glb.push_back('F');
    put_u32(2);
    put_u32(total);
    put_u32(static_cast<uint32_t>(json.size()));
    glb.push_back('J'); glb.push_back('S'); glb.push_back('O'); glb.push_back('N');
    glb.insert(glb.end(), json.begin(), json.end());
    put_u32(bin_padded);
    glb.push_back('B'); glb.push_back('I'); glb.push_back('N'); glb.push_back(0);
    glb.insert(glb.end(), bin.data.begin(), bin.data.end());
    while (glb.size() < total) glb.push_back(0);

    std::ofstream f(output_path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        if (err) *err = "cannot open output file: " + output_path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    if (!f) {
        if (err) *err = "write failed for " + output_path;
        return false;
    }
    return true;
}

} // namespace av
