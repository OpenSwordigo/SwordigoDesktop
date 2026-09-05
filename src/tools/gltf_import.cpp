// gltf_import.cpp — Remastered glTF 2.0 / GLB importer for POD models.
// Uses tiny_gltf_v3 for robust, 100% spec-compliant glTF parsing,
// and maps assets into Swordigo PowerVR POD models and PVR textures.

#include "gltf_glb.h"
#include "pod_loader.h"
#include "tiny_gltf_v3.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace av {
namespace {

// ─── Accessor Unpacking Helper ─────────────────────────────────────────
static bool read_accessor_floats(const tg3_model* model, int32_t acc_idx, std::vector<float>& out) {
    out.clear();
    if (!model || acc_idx < 0 || acc_idx >= (int32_t)model->accessors_count) return false;
    const tg3_accessor* acc = &model->accessors[acc_idx];
    if (acc->count == 0) return true;
    if (acc->buffer_view < 0 || acc->buffer_view >= (int32_t)model->buffer_views_count) return false;
    const tg3_buffer_view* bv = &model->buffer_views[acc->buffer_view];
    if (bv->buffer < 0 || bv->buffer >= (int32_t)model->buffers_count) return false;
    const tg3_buffer* buf = &model->buffers[bv->buffer];
    if (!buf->data.data || buf->data.count == 0) return false;

    int num_comps = 1;
    switch (acc->type) {
        case TG3_TYPE_SCALAR: num_comps = 1; break;
        case TG3_TYPE_VEC2:   num_comps = 2; break;
        case TG3_TYPE_VEC3:   num_comps = 3; break;
        case TG3_TYPE_VEC4:   num_comps = 4; break;
        case TG3_TYPE_MAT2:   num_comps = 4; break;
        case TG3_TYPE_MAT3:   num_comps = 9; break;
        case TG3_TYPE_MAT4:   num_comps = 16; break;
        default: num_comps = 1; break;
    }

    size_t comp_size = 4;
    switch (acc->component_type) {
        case TG3_COMPONENT_TYPE_BYTE:
        case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:  comp_size = 1; break;
        case TG3_COMPONENT_TYPE_SHORT:
        case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: comp_size = 2; break;
        case TG3_COMPONENT_TYPE_INT:
        case TG3_COMPONENT_TYPE_UNSIGNED_INT:
        case TG3_COMPONENT_TYPE_FLOAT:          comp_size = 4; break;
        case TG3_COMPONENT_TYPE_DOUBLE:         comp_size = 8; break;
        default: comp_size = 4; break;
    }

    size_t elem_size = (size_t)num_comps * comp_size;
    size_t stride = (bv->byte_stride > 0) ? bv->byte_stride : elem_size;
    uint64_t start_offset = bv->byte_offset + acc->byte_offset;

    if (start_offset + (acc->count - 1) * stride + elem_size > buf->data.count) {
        return false;
    }

    out.resize(acc->count * num_comps);
    const uint8_t* base_ptr = buf->data.data + start_offset;

    for (uint64_t i = 0; i < acc->count; ++i) {
        const uint8_t* elem_ptr = base_ptr + i * stride;
        float* dst = &out[i * num_comps];

        for (int c = 0; c < num_comps; ++c) {
            const uint8_t* cp = elem_ptr + c * comp_size;
            float val = 0.0f;
            switch (acc->component_type) {
                case TG3_COMPONENT_TYPE_BYTE:
                    val = acc->normalized ? std::max(-1.0f, (float)*(const int8_t*)cp / 127.0f) : (float)*(const int8_t*)cp;
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
                    val = acc->normalized ? (float)*(const uint8_t*)cp / 255.0f : (float)*(const uint8_t*)cp;
                    break;
                case TG3_COMPONENT_TYPE_SHORT:
                    val = acc->normalized ? std::max(-1.0f, (float)*(const int16_t*)cp / 32767.0f) : (float)*(const int16_t*)cp;
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
                    val = acc->normalized ? (float)*(const uint16_t*)cp / 65535.0f : (float)*(const uint16_t*)cp;
                    break;
                case TG3_COMPONENT_TYPE_INT:
                    val = (float)*(const int32_t*)cp;
                    break;
                case TG3_COMPONENT_TYPE_UNSIGNED_INT:
                    val = (float)*(const uint32_t*)cp;
                    break;
                case TG3_COMPONENT_TYPE_FLOAT:
                    val = *(const float*)cp;
                    break;
                case TG3_COMPONENT_TYPE_DOUBLE:
                    val = (float)*(const double*)cp;
                    break;
            }
            dst[c] = val;
        }
    }
    return true;
}

static int find_attribute_accessor(const tg3_primitive* prim, const char* name) {
    if (!prim || !prim->attributes || !name) return -1;
    size_t name_len = std::strlen(name);
    for (uint32_t i = 0; i < prim->attributes_count; ++i) {
        if (prim->attributes[i].key.len == name_len &&
            std::strncmp(prim->attributes[i].key.data, name, name_len) == 0) {
            return prim->attributes[i].value;
        }
    }
    return -1;
}

static int find_uv_accessor(const tg3_model* model, const tg3_primitive* prim, int mat_idx) {
    if (mat_idx >= 0 && mat_idx < (int32_t)model->materials_count) {
        const tg3_material* mat = &model->materials[mat_idx];
        int tc = mat->pbr_metallic_roughness.base_color_texture.tex_coord;
        if (tc > 0) {
            std::string attr = "TEXCOORD_" + std::to_string(tc);
            int acc = find_attribute_accessor(prim, attr.c_str());
            if (acc >= 0) return acc;
        }
    }
    int acc = find_attribute_accessor(prim, "TEXCOORD_0");
    if (acc < 0) acc = find_attribute_accessor(prim, "TEXCOORD");
    if (acc < 0) acc = find_attribute_accessor(prim, "TEXCOORD_1");
    if (acc < 0) acc = find_attribute_accessor(prim, "TEXCOORD_2");
    return acc;
}

static std::string tg3_to_string(const tg3_str& s) {
    if (!s.data || s.len == 0) return "";
    return std::string(s.data, s.len);
}

// ─── Rebuild PODModel from tg3_model ───────────────────────────────────
static bool build_pod_from_tg3(const tg3_model* model,
                               PODModel& out, std::vector<GLTFImageBuffer>& images,
                               GLTFPBRInfo* pbr, std::string* err, float scale = 1.0f) {
    (void)err;
    if (!model) return false;

    std::vector<int> parent_of(model->nodes_count, -1);
    for (uint32_t i = 0; i < model->nodes_count; ++i) {
        const tg3_node* n = &model->nodes[i];
        for (uint32_t c = 0; c < n->children_count; ++c) {
            int32_t child_idx = n->children[c];
            if (child_idx >= 0 && child_idx < (int32_t)model->nodes_count) {
                parent_of[child_idx] = (int)i;
            }
        }
    }

    std::vector<float> accbuf;
    auto load = [&](int32_t acc_idx) -> const std::vector<float>& {
        accbuf.clear();
        if (!read_accessor_floats(model, acc_idx, accbuf)) accbuf.clear();
        return accbuf;
    };

    out.meshes.clear();
    for (uint32_t mi = 0; mi < model->meshes_count; ++mi) {
        const tg3_mesh* gm = &model->meshes[mi];
        for (uint32_t pi = 0; pi < gm->primitives_count; ++pi) {
            const tg3_primitive* p = &gm->primitives[pi];
            PODMesh m;

            int pos_acc = find_attribute_accessor(p, "POSITION");
            const std::vector<float>& pos = load(pos_acc);
            if (!pos.empty()) {
                m.positions = pos;
                m.num_vertices = (int)(pos.size() / 3);
            }

            int nrm_acc = find_attribute_accessor(p, "NORMAL");
            const std::vector<float>& nrm = load(nrm_acc);
            if (!nrm.empty()) m.normals = nrm;

            int uv_acc = find_uv_accessor(model, p, p->material);
            const std::vector<float>& uv = load(uv_acc);
            if (!uv.empty()) m.uvs = uv;

            int tng_acc = find_attribute_accessor(p, "TANGENT");
            const std::vector<float>& tng = load(tng_acc);
            if (!tng.empty()) m.tangents = tng;

            const std::vector<float>& idx = load(p->indices);
            if (!idx.empty()) {
                m.indices.reserve(idx.size());
                for (float x : idx) m.indices.push_back(static_cast<uint32_t>(x));
                m.num_faces = static_cast<int>(m.indices.size() / 3);
            }

            int joints_acc = find_attribute_accessor(p, "JOINTS_0");
            int weights_acc = find_attribute_accessor(p, "WEIGHTS_0");
            const std::vector<float>& joints = load(joints_acc);
            const std::vector<float>& weights = load(weights_acc);

            if (!joints.empty() && m.num_vertices > 0) {
                int jcomps = 4;
                if (joints_acc >= 0 && joints_acc < (int32_t)model->accessors_count) {
                    if (model->accessors[joints_acc].type == TG3_TYPE_VEC4) jcomps = 4;
                    else if (model->accessors[joints_acc].type == TG3_TYPE_VEC3) jcomps = 3;
                    else if (model->accessors[joints_acc].type == TG3_TYPE_VEC2) jcomps = 2;
                    else jcomps = 1;
                }
                int nverts = m.num_vertices;
                int max_inf = 0;
                for (int v = 0; v < nverts; ++v) {
                    int infl = 0;
                    for (int k = 0; k < jcomps; ++k) {
                        size_t i = (size_t)v * jcomps + k;
                        float w = (weights.size() > i) ? weights[i] : 0.0f;
                        if (w > 0.0f) infl = k + 1;
                    }
                    max_inf = std::max(max_inf, infl);
                }
                max_inf = std::clamp(max_inf, 1, 4);
                m.bones_per_vertex = max_inf;
                m.bone_indices.assign((size_t)nverts * max_inf, 0.0f);
                m.bone_weights.assign((size_t)nverts * max_inf, 0.0f);
                for (int v = 0; v < nverts; ++v) {
                    for (int k = 0; k < max_inf; ++k) {
                        size_t src = (size_t)v * jcomps + k;
                        size_t dst = (size_t)v * max_inf + k;
                        m.bone_indices[dst] = (joints.size() > src) ? joints[src] : 0.0f;
                        m.bone_weights[dst] = (weights.size() > src) ? weights[src] : 0.0f;
                    }
                }
                m.has_bone_batches = true;
                m.bone_batches.count = 1;
                m.bone_batches.offsets = {0};
                m.bone_batches.counts = {1};
                m.bone_batches.max_bones = 1;
            }

            if (scale != 1.0f && scale > 0.0f) {
                for (float& pv : m.positions) pv *= scale;
            }

            m.num_vertices = m.num_vertices ? m.num_vertices : (int)(m.positions.size() / 3);
            if (m.num_vertices == 0 && !m.positions.empty()) m.num_vertices = (int)(m.positions.size() / 3);
            if (m.num_faces == 0) m.num_faces = m.indices.empty()
                ? (m.num_vertices > 0 ? m.num_vertices / 3 : 0)
                : (int)(m.indices.size() / 3);

            float m_minx =  1e30f, m_miny =  1e30f, m_minz =  1e30f;
            float m_maxx = -1e30f, m_maxy = -1e30f, m_maxz = -1e30f;
            for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
                float px = m.positions[i + 0];
                float py = m.positions[i + 1];
                float pz = m.positions[i + 2];
                m_minx = std::min(m_minx, px); m_maxx = std::max(m_maxx, px);
                m_miny = std::min(m_miny, py); m_maxy = std::max(m_maxy, py);
                m_minz = std::min(m_minz, pz); m_maxz = std::max(m_maxz, pz);
            }
            if (m.num_vertices > 0) {
                m.min_x = m_minx; m.max_x = m_maxx;
                m.min_y = m_miny; m.max_y = m_maxy;
                m.min_z = m_minz; m.max_z = m_maxz;
            }

            out.meshes.push_back(std::move(m));
        }
    }

    out.nodes.clear();
    std::vector<int> pod_mesh_of_gltf_mesh(model->meshes_count, -1);
    {
        int cursor = 0;
        for (uint32_t m = 0; m < model->meshes_count; ++m) {
            pod_mesh_of_gltf_mesh[m] = cursor;
            cursor += (int)model->meshes[m].primitives_count;
        }
    }

    std::vector<int> mesh_gltf_nodes;
    std::vector<int> other_gltf_nodes;
    for (uint32_t i = 0; i < model->nodes_count; ++i) {
        if (model->nodes[i].mesh >= 0 && model->nodes[i].mesh < (int32_t)model->meshes_count) {
            mesh_gltf_nodes.push_back(static_cast<int>(i));
        } else {
            other_gltf_nodes.push_back(static_cast<int>(i));
        }
    }

    std::vector<int> gltf_to_pod_node(model->nodes_count, -1);
    for (size_t k = 0; k < mesh_gltf_nodes.size(); ++k) {
        gltf_to_pod_node[mesh_gltf_nodes[k]] = static_cast<int>(k);
    }
    for (size_t k = 0; k < other_gltf_nodes.size(); ++k) {
        gltf_to_pod_node[other_gltf_nodes[k]] = static_cast<int>(mesh_gltf_nodes.size() + k);
    }

    std::vector<bool> is_joint(model->nodes_count, false);
    for (uint32_t si = 0; si < model->skins_count; ++si) {
        const tg3_skin* skin = &model->skins[si];
        for (uint32_t j = 0; j < skin->joints_count; ++j) {
            int32_t ji = skin->joints[j];
            if (ji >= 0 && ji < (int32_t)is_joint.size()) is_joint[ji] = true;
        }
    }

    std::vector<int> node_mesh_begin(model->nodes_count, -1);
    std::vector<int> node_mesh_end(model->nodes_count, -1);
    for (uint32_t n = 0; n < model->nodes_count; ++n) {
        if (model->nodes[n].mesh >= 0 && model->nodes[n].mesh < (int32_t)model->meshes_count) {
            int m = model->nodes[n].mesh;
            node_mesh_begin[n] = pod_mesh_of_gltf_mesh[m];
            node_mesh_end[n] = pod_mesh_of_gltf_mesh[m] + (int)model->meshes[m].primitives_count;
        }
    }

    out.nodes.resize(model->nodes_count);
    for (uint32_t i = 0; i < model->nodes_count; ++i) {
        int pod_idx = gltf_to_pod_node[i];
        const tg3_node* gn = &model->nodes[i];
        PODNode n;
        std::string name = tg3_to_string(gn->name);
        for (char& c : name) {
            if (c == ':' || c == '/' || c == '\\') c = '_';
        }
        if (is_joint[i]) {
            if (name.rfind("Bone", 0) != 0 &&
                name.rfind("Control", 0) != 0 &&
                name != "CenterPoint") {
                if (name.empty()) name = "Bone_" + std::to_string(i);
                else name = "Bone_" + name;
            }
        }
        n.name = name;
        int orig_parent = parent_of[i];
        n.parent_index = (orig_parent >= 0 && orig_parent < (int)gltf_to_pod_node.size())
                         ? gltf_to_pod_node[orig_parent] : -1;

        if (gn->mesh >= 0 && gn->mesh < (int32_t)model->meshes_count) {
            n.object_index = pod_mesh_of_gltf_mesh[gn->mesh];
            if (model->meshes[gn->mesh].primitives_count > 0)
                n.material_index = model->meshes[gn->mesh].primitives[0].material;
        } else {
            n.object_index = -1;
            n.material_index = -1;
        }

        if (gn->has_matrix) {
            n.has_matrix = true;
            for (int k = 0; k < 16; ++k) n.matrix[k] = (float)gn->matrix[k];
            // Apply opts.scale to world-space position only (translation column).
            // Do NOT scale the upper 3x3: that would square the scale for any
            // root node carrying an authored scale (unit-convert, assemblies).
            if (scale != 1.0f && orig_parent == -1) {
                n.matrix[12] *= scale;
                n.matrix[13] *= scale;
                n.matrix[14] *= scale;
            }
        } else {
            n.has_translation = true;
            n.translation[0] = (float)gn->translation[0];
            n.translation[1] = (float)gn->translation[1];
            n.translation[2] = (float)gn->translation[2];

            n.has_rotation = true;
            n.rotation[0] = (float)gn->rotation[0];
            n.rotation[1] = (float)gn->rotation[1];
            n.rotation[2] = (float)gn->rotation[2];
            n.rotation[3] = (float)gn->rotation[3];

            n.has_scale = true;
            n.scale[0] = (float)gn->scale[0];
            n.scale[1] = (float)gn->scale[1];
            n.scale[2] = (float)gn->scale[2];

            // Scale world-space translation only. The node's own scale channel
            // must stay authored (it scales children); scaling it here would
            // double-apply opts.scale to the whole subtree.
            if (scale != 1.0f && orig_parent == -1) {
                n.translation[0] *= scale;
                n.translation[1] *= scale;
                n.translation[2] *= scale;
            }
        }
        out.nodes[pod_idx] = std::move(n);
    }

    out.num_mesh_nodes = static_cast<int>(mesh_gltf_nodes.size());

    for (uint32_t i = 0; i < model->nodes_count; ++i) {
        if (model->nodes[i].skin < 0 || model->nodes[i].skin >= (int32_t)model->skins_count) continue;
        const tg3_skin* skin = &model->skins[model->nodes[i].skin];
        if (skin->joints_count == 0) continue;
        int mesh_begin = node_mesh_begin[i];
        int mesh_end = node_mesh_end[i];
        if (mesh_begin < 0) continue;
        for (int mi = mesh_begin; mi < mesh_end; ++mi) {
            PODMesh& m = out.meshes[mi];
            if (m.bones_per_vertex <= 0) continue;
            std::vector<uint32_t> remapped_joints;
            remapped_joints.reserve(skin->joints_count);
            for (uint32_t j = 0; j < skin->joints_count; ++j) {
                int32_t ji = skin->joints[j];
                int rj = (ji >= 0 && ji < (int)gltf_to_pod_node.size()) ? gltf_to_pod_node[ji] : ji;
                remapped_joints.push_back(static_cast<uint32_t>(rj));
            }
            m.bone_batches.indices = std::move(remapped_joints);
            m.bone_batches.counts = {static_cast<uint32_t>(m.bone_batches.indices.size())};
            m.bone_batches.offsets = {0};
            m.bone_batches.count = 1;
            m.bone_batches.max_bones = static_cast<int>(m.bone_batches.indices.size());
            m.has_bone_batches = true;
        }
    }

    images.clear();
    std::vector<std::string> loaded_img_names(model->images_count);
    for (uint32_t img_idx = 0; img_idx < model->images_count; ++img_idx) {
        const tg3_image* img = &model->images[img_idx];
        std::string name = tg3_to_string(img->name);
        if (name.empty()) {
            std::string uri = tg3_to_string(img->uri);
            if (!uri.empty()) {
                size_t slash = uri.find_last_of("/\\");
                name = (slash != std::string::npos) ? uri.substr(slash + 1) : uri;
            }
        }
        if (name.empty()) name = "texture" + std::to_string(img_idx) + ".png";
        loaded_img_names[img_idx] = name;

        GLTFImageBuffer ib;
        ib.mime = tg3_to_string(img->mime_type);
        if (img->image.data && img->image.count > 0) {
            ib.data.assign(img->image.data, img->image.data + img->image.count);
        } else if (img->buffer_view >= 0 && img->buffer_view < (int32_t)model->buffer_views_count) {
            const tg3_buffer_view* bv = &model->buffer_views[img->buffer_view];
            if (bv->buffer >= 0 && bv->buffer < (int32_t)model->buffers_count) {
                const tg3_buffer* buf = &model->buffers[bv->buffer];
                if (buf->data.data && bv->byte_offset + bv->byte_length <= buf->data.count) {
                    ib.data.assign(buf->data.data + bv->byte_offset,
                                   buf->data.data + bv->byte_offset + bv->byte_length);
                }
            }
        }
        images.push_back(std::move(ib));
    }

    out.materials.clear();
    out.texture_filenames.clear();
    for (uint32_t mi = 0; mi < model->materials_count; ++mi) {
        const tg3_material* mat = &model->materials[mi];
        PODMaterial pm;
        pm.name = tg3_to_string(mat->name);
        pm.diffuse[0] = (float)mat->pbr_metallic_roughness.base_color_factor[0];
        pm.diffuse[1] = (float)mat->pbr_metallic_roughness.base_color_factor[1];
        pm.diffuse[2] = (float)mat->pbr_metallic_roughness.base_color_factor[2];
        pm.opacity    = (float)mat->pbr_metallic_roughness.base_color_factor[3];

        int32_t tex_idx = mat->pbr_metallic_roughness.base_color_texture.index;
        if (tex_idx >= 0 && tex_idx < (int32_t)model->textures_count) {
            int32_t src_img = model->textures[tex_idx].source;
            if (src_img >= 0 && src_img < (int32_t)loaded_img_names.size()) {
                std::string tname = loaded_img_names[src_img];
                auto it = std::find(out.texture_filenames.begin(), out.texture_filenames.end(), tname);
                int ti = (int)(it - out.texture_filenames.begin());
                if (it == out.texture_filenames.end()) {
                    out.texture_filenames.push_back(tname);
                }
                pm.diffuse_texture_index = ti;
            }
        }
        out.materials.push_back(std::move(pm));
    }

    if (pbr) {
        pbr->materials.clear();
        pbr->images.clear();
        pbr->images.reserve(images.size());
        for (const auto& img : images) {
            GLTFPBRInfo::Image pimg;
            pimg.mime = img.mime;
            pimg.data = img.data;
            pbr->images.push_back(std::move(pimg));
        }
        pbr->image_gltf_index.resize(images.size());
        for (size_t k = 0; k < images.size(); ++k) pbr->image_gltf_index[k] = (int)k;

        for (uint32_t mi = 0; mi < model->materials_count; ++mi) {
            const tg3_material* mat = &model->materials[mi];
            GLTFPBRMaterial pm;
            pm.base_color[0] = (float)mat->pbr_metallic_roughness.base_color_factor[0];
            pm.base_color[1] = (float)mat->pbr_metallic_roughness.base_color_factor[1];
            pm.base_color[2] = (float)mat->pbr_metallic_roughness.base_color_factor[2];
            pm.base_color[3] = (float)mat->pbr_metallic_roughness.base_color_factor[3];
            pm.metallic      = (float)mat->pbr_metallic_roughness.metallic_factor;
            pm.roughness     = (float)mat->pbr_metallic_roughness.roughness_factor;
            pm.occlusion     = (float)mat->occlusion_texture.strength;
            pm.emissive[0]   = (float)mat->emissive_factor[0];
            pm.emissive[1]   = (float)mat->emissive_factor[1];
            pm.emissive[2]   = (float)mat->emissive_factor[2];
            pm.normal_scale  = (float)mat->normal_texture.scale;
            pm.alpha_cutoff  = (float)mat->alpha_cutoff;

            std::string am_str = tg3_to_string(mat->alpha_mode);
            if (am_str == "BLEND") pm.alpha_mode = 2;
            else if (am_str == "MASK") pm.alpha_mode = 1;
            else pm.alpha_mode = 0;

            pm.double_sided  = mat->double_sided ? true : false;

            auto tex_to_img = [&](int32_t t_idx) -> int {
                if (t_idx < 0 || t_idx >= (int32_t)model->textures_count) return -1;
                return model->textures[t_idx].source;
            };

            pm.base_tex        = tex_to_img(mat->pbr_metallic_roughness.base_color_texture.index);
            pm.metalrough_tex  = tex_to_img(mat->pbr_metallic_roughness.metallic_roughness_texture.index);
            pm.normal_tex      = tex_to_img(mat->normal_texture.index);
            pm.occl_tex        = tex_to_img(mat->occlusion_texture.index);
            pm.emissive_tex    = tex_to_img(mat->emissive_texture.index);

            pbr->materials.push_back(std::move(pm));
        }
    }

    out.num_frames = 0;
    out.fps = 0.0f;
    out.total_vertices = 0;
    out.total_faces = 0;

    float minx =  1e30f, miny =  1e30f, minz =  1e30f;
    float maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;

    for (auto& mesh : out.meshes) {
        if (mesh.num_vertices == 0) mesh.num_vertices = (int)(mesh.positions.size() / 3);
        if (mesh.num_faces == 0) mesh.num_faces = (int)(mesh.indices.size() / 3);
        out.total_vertices += mesh.num_vertices;
        out.total_faces    += mesh.num_faces;
        if (mesh.num_vertices > 0) {
            minx = std::min(minx, mesh.min_x); maxx = std::max(maxx, mesh.max_x);
            miny = std::min(miny, mesh.min_y); maxy = std::max(maxy, mesh.max_y);
            minz = std::min(minz, mesh.min_z); maxz = std::max(maxz, mesh.max_z);
        }
    }

    if (!out.meshes.empty() && out.total_vertices > 0) {
        out.min_x = minx; out.max_x = maxx;
        out.min_y = miny; out.max_y = maxy;
        out.min_z = minz; out.max_z = maxz;
        out.center_x = (minx + maxx) * 0.5f;
        out.center_y = (miny + maxy) * 0.5f;
        out.center_z = (minz + maxz) * 0.5f;
        float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
        out.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        if (out.radius < 1.0f) out.radius = 1.0f;
    }

    return true;
}

} // namespace
} // namespace av

namespace av {

bool gltf_import_glb(const std::string& path, PODModel& out,
                     std::vector<GLTFImageBuffer>& images, std::string* err,
                     GLTFPBRInfo* pbr, float scale) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) { if (err) *err = "cannot open: " + path; return false; }
    std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> file(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(file.data()), size);
    if (!f) { if (err) *err = "read failed"; return false; }

    tg3_model model;
    tg3_error_stack errors;
    std::memset(&model, 0, sizeof(model));
    tg3_error_stack_init(&errors);

    tg3_parse_options opts;
    tg3_parse_options_init(&opts);
    opts.strictness = TG3_PERMISSIVE;
    opts.images_as_is = 1;

    std::string base_dir;
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) base_dir = path.substr(0, slash);

    tg3_error_code code = tg3_parse_auto(&model, &errors, file.data(), file.size(),
                                         base_dir.c_str(), (uint32_t)base_dir.size(), &opts);

    if (code != TG3_OK) {
        if (err) {
            if (tg3_errors_count(&errors) > 0) {
                *err = tg3_errors_get(&errors, 0)->message;
            } else {
                *err = "glTF parse error code " + std::to_string(code);
            }
        }
        tg3_error_stack_free(&errors);
        tg3_model_free(&model);
        return false;
    }

    bool ok = build_pod_from_tg3(&model, out, images, pbr, err, scale);
    tg3_error_stack_free(&errors);
    tg3_model_free(&model);
    return ok;
}

bool gltf_import_gltf(const std::string& path, PODModel& out,
                      std::vector<GLTFImageBuffer>& images, std::string* err,
                      GLTFPBRInfo* pbr, float scale) {
    return gltf_import_glb(path, out, images, err, pbr, scale);
}

bool gltf_import_all_clips(const std::string& path,
                           std::vector<std::pair<std::string, PODModel>>& out_clips,
                           std::string* err,
                           float scale) {
    out_clips.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) { if (err) *err = "cannot open: " + path; return false; }
    std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> file(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(file.data()), size);
    if (!f) { if (err) *err = "read failed"; return false; }

    tg3_model model;
    tg3_error_stack errors;
    std::memset(&model, 0, sizeof(model));
    tg3_error_stack_init(&errors);

    tg3_parse_options opts;
    tg3_parse_options_init(&opts);
    opts.strictness = TG3_PERMISSIVE;
    opts.images_as_is = 1;

    std::string base_dir;
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) base_dir = path.substr(0, slash);

    tg3_error_code code = tg3_parse_auto(&model, &errors, file.data(), file.size(),
                                         base_dir.c_str(), (uint32_t)base_dir.size(), &opts);

    if (code != TG3_OK) {
        if (err) {
            if (tg3_errors_count(&errors) > 0) {
                *err = tg3_errors_get(&errors, 0)->message;
            } else {
                *err = "glTF parse error code " + std::to_string(code);
            }
        }
        tg3_error_stack_free(&errors);
        tg3_model_free(&model);
        return false;
    }

    PODModel base_model;
    std::vector<GLTFImageBuffer> imgs;
    if (!build_pod_from_tg3(&model, base_model, imgs, nullptr, err, scale)) {
        tg3_error_stack_free(&errors);
        tg3_model_free(&model);
        return false;
    }

    if (model.animations_count == 0) {
        tg3_error_stack_free(&errors);
        tg3_model_free(&model);
        return true;
    }

    auto sample_channel = [](const std::string& interp, int comps,
                             const std::vector<float>& times,
                             const std::vector<float>& vals,
                             int num_frames, float fps,
                             std::vector<float>& out_dense,
                             float scale = 1.0f) {
        if (times.empty() || vals.empty() || num_frames <= 0 || comps <= 0) return;
        out_dense.assign((size_t)num_frames * comps, 0.0f);

        bool is_cubic = (interp == "CUBICSPLINE" || interp == "CUBIC_SPLINE");
        bool is_step = (interp == "STEP");
        size_t stride = is_cubic ? (size_t)comps * 3 : (size_t)comps;
        size_t val_offset = is_cubic ? (size_t)comps : 0;

        auto get_val = [&](size_t k, int c) -> float {
            size_t idx = k * stride + val_offset + c;
            return (idx < vals.size()) ? vals[idx] : 0.0f;
        };
        auto get_in_tan = [&](size_t k, int c) -> float {
            size_t idx = k * stride + c;
            return (idx < vals.size()) ? vals[idx] : 0.0f;
        };
        auto get_out_tan = [&](size_t k, int c) -> float {
            size_t idx = k * stride + 2 * comps + c;
            return (idx < vals.size()) ? vals[idx] : 0.0f;
        };

        for (int f = 0; f < num_frames; ++f) {
            float t = static_cast<float>(f) / fps;
            float* out_val = &out_dense[(size_t)f * comps];

            if (t <= times.front() || times.size() == 1) {
                for (int c = 0; c < comps; ++c) out_val[c] = get_val(0, c);
            } else if (t >= times.back()) {
                size_t last = times.size() - 1;
                for (int c = 0; c < comps; ++c) out_val[c] = get_val(last, c);
            } else {
                size_t k = 0;
                while (k + 1 < times.size() && times[k + 1] < t) ++k;
                float t0 = times[k];
                float t1 = times[k + 1];
                float dt = t1 - t0;
                float alpha = (dt > 1e-6f) ? std::clamp((t - t0) / dt, 0.0f, 1.0f) : 0.0f;

                if (is_step) {
                    for (int c = 0; c < comps; ++c) out_val[c] = get_val(k, c);
                } else if (is_cubic) {
                    float a2 = alpha * alpha;
                    float a3 = a2 * alpha;
                    float h00 = 2.0f * a3 - 3.0f * a2 + 1.0f;
                    float h10 = a3 - 2.0f * a2 + alpha;
                    float h01 = -2.0f * a3 + 3.0f * a2;
                    float h11 = a3 - a2;

                    for (int c = 0; c < comps; ++c) {
                        float p0 = get_val(k, c);
                        float m0 = get_out_tan(k, c) * dt;
                        float p1 = get_val(k + 1, c);
                        float m1 = get_in_tan(k + 1, c) * dt;
                        out_val[c] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
                    }
                    if (comps == 4) {
                        float len = std::sqrt(out_val[0]*out_val[0] + out_val[1]*out_val[1] +
                                              out_val[2]*out_val[2] + out_val[3]*out_val[3]);
                        if (len > 1e-6f) {
                            for (int c = 0; c < 4; ++c) out_val[c] /= len;
                        } else {
                            out_val[0] = 0; out_val[1] = 0; out_val[2] = 0; out_val[3] = 1;
                        }
                    }
                } else {
                    if (comps == 4) {
                        float q0[4] = {get_val(k, 0), get_val(k, 1), get_val(k, 2), get_val(k, 3)};
                        float q1[4] = {get_val(k+1, 0), get_val(k+1, 1), get_val(k+1, 2), get_val(k+1, 3)};
                        float dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
                        if (dot < 0.0f) {
                            for (int c = 0; c < 4; ++c) q1[c] = -q1[c];
                            dot = -dot;
                        }
                        float s0 = 1.0f - alpha;
                        float s1 = alpha;
                        if (dot < 0.9995f) {
                            float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
                            float sin_theta = std::sin(theta);
                            if (sin_theta > 1e-5f) {
                                s0 = std::sin((1.0f - alpha) * theta) / sin_theta;
                                s1 = std::sin(alpha * theta) / sin_theta;
                            }
                        }
                        for (int c = 0; c < 4; ++c) out_val[c] = s0 * q0[c] + s1 * q1[c];
                        float len = std::sqrt(out_val[0]*out_val[0] + out_val[1]*out_val[1] +
                                              out_val[2]*out_val[2] + out_val[3]*out_val[3]);
                        if (len > 1e-6f) {
                            for (int c = 0; c < 4; ++c) out_val[c] /= len;
                        } else {
                            out_val[0] = 0; out_val[1] = 0; out_val[2] = 0; out_val[3] = 1;
                        }
                    } else {
                        for (int c = 0; c < comps; ++c) {
                            out_val[c] = (1.0f - alpha) * get_val(k, c) + alpha * get_val(k + 1, c);
                        }
                    }
                }
            }

            if (scale != 1.0f && comps == 3) {
                for (int c = 0; c < 3; ++c) out_val[c] *= scale;
            }
        }
    };

    std::vector<int> parent_of(model.nodes_count, -1);
    for (uint32_t i = 0; i < model.nodes_count; ++i) {
        for (uint32_t c = 0; c < model.nodes[i].children_count; ++c) {
            int32_t child_idx = model.nodes[i].children[c];
            if (child_idx >= 0 && child_idx < (int32_t)model.nodes_count)
                parent_of[child_idx] = static_cast<int>(i);
        }
    }

    for (uint32_t a_idx = 0; a_idx < model.animations_count; ++a_idx) {
        const tg3_animation* anim = &model.animations[a_idx];
        std::string clip_name = tg3_to_string(anim->name);
        if (clip_name.empty()) clip_name = "anim_" + std::to_string(a_idx);
        for (char& c : clip_name) {
            if (c == ':' || c == '/' || c == '\\' || c == ' ') c = '_';
        }

        PODModel clip_model = base_model;
        clip_model.meshes.clear();
        clip_model.materials.clear();
        clip_model.texture_filenames.clear();
        for (auto& n : clip_model.nodes) {
            n.object_index = -1;
            n.material_index = -1;
            n.anim_translation.clear();
            n.anim_rotation.clear();
            n.anim_scale.clear();
            n.anim_flags = 0;
        }

        std::vector<float> accbuf;
        auto load_acc = [&](int32_t acc_idx) -> const std::vector<float>& {
            accbuf.clear();
            if (!read_accessor_floats(&model, acc_idx, accbuf)) accbuf.clear();
            return accbuf;
        };

        float max_time = 0.0f;
        for (uint32_t si = 0; si < anim->samplers_count; ++si) {
            int32_t in_acc = anim->samplers[si].input;
            if (in_acc < 0 || in_acc >= (int32_t)model.accessors_count) continue;
            const std::vector<float>& times = load_acc(in_acc);
            if (!times.empty()) max_time = std::max(max_time, times.back());
        }
        clip_model.fps = 30.0f;
        clip_model.num_frames = std::max(1, static_cast<int>(std::lround(max_time * clip_model.fps)) + 1);

        std::vector<int> gltf_to_pod_node(model.nodes_count, -1);
        {
            int mesh_count = 0;
            for (uint32_t i = 0; i < model.nodes_count; ++i) {
                if (model.nodes[i].mesh >= 0 && model.nodes[i].mesh < (int32_t)model.meshes_count) mesh_count++;
            }
            int next_mesh = 0, next_other = 0;
            for (uint32_t i = 0; i < model.nodes_count; ++i) {
                if (model.nodes[i].mesh >= 0 && model.nodes[i].mesh < (int32_t)model.meshes_count) {
                    gltf_to_pod_node[i] = next_mesh++;
                } else {
                    gltf_to_pod_node[i] = mesh_count + (next_other++);
                }
            }
        }

        for (uint32_t ci = 0; ci < anim->channels_count; ++ci) {
            const tg3_animation_channel* ch = &anim->channels[ci];
            if (ch->target.node < 0 || ch->target.node >= (int32_t)model.nodes_count) continue;
            int pod_node_idx = gltf_to_pod_node[ch->target.node];
            if (pod_node_idx < 0 || pod_node_idx >= (int)clip_model.nodes.size()) continue;
            if (ch->sampler < 0 || ch->sampler >= (int32_t)anim->samplers_count) continue;
            const tg3_animation_sampler* sm = &anim->samplers[ch->sampler];

            const std::vector<float>& times_ref = load_acc(sm->input);
            std::vector<float> times(times_ref.begin(), times_ref.end());
            const std::vector<float>& vals_ref = load_acc(sm->output);
            std::vector<float> vals(vals_ref.begin(), vals_ref.end());
            if (times.empty() || vals.empty()) continue;

            std::string path_str = tg3_to_string(ch->target.path);
            int comps = (path_str == "rotation") ? 4 : 3;
            PODNode& node = clip_model.nodes[pod_node_idx];
            float chan_scale = (path_str == "translation" && parent_of[ch->target.node] == -1) ? scale : 1.0f;
            std::string interp_str = tg3_to_string(sm->interpolation);

            if (path_str == "translation") {
                sample_channel(interp_str, comps, times, vals, clip_model.num_frames, clip_model.fps, node.anim_translation, chan_scale);
                node.anim_flags |= 1;
            } else if (path_str == "rotation") {
                sample_channel(interp_str, comps, times, vals, clip_model.num_frames, clip_model.fps, node.anim_rotation, 1.0f);
                node.anim_flags |= 2;
            } else if (path_str == "scale") {
                sample_channel(interp_str, comps, times, vals, clip_model.num_frames, clip_model.fps, node.anim_scale, 1.0f);
                node.anim_flags |= 4;
            }
        }

        for (auto& node : clip_model.nodes) {
            if (node.anim_translation.empty()) {
                node.anim_translation.assign((size_t)clip_model.num_frames * 3, 0.0f);
                for (int f = 0; f < clip_model.num_frames; ++f) {
                    node.anim_translation[f * 3 + 0] = node.translation[0];
                    node.anim_translation[f * 3 + 1] = node.translation[1];
                    node.anim_translation[f * 3 + 2] = node.translation[2];
                }
            }
            if (node.anim_rotation.empty()) {
                node.anim_rotation.assign((size_t)clip_model.num_frames * 4, 0.0f);
                for (int f = 0; f < clip_model.num_frames; ++f) {
                    node.anim_rotation[f * 4 + 0] = node.rotation[0];
                    node.anim_rotation[f * 4 + 1] = node.rotation[1];
                    node.anim_rotation[f * 4 + 2] = node.rotation[2];
                    node.anim_rotation[f * 4 + 3] = (node.rotation[3] == 0.0f && node.rotation[0] == 0.0f) ? 1.0f : node.rotation[3];
                }
            }
            if (node.anim_scale.empty()) {
                node.anim_scale.assign((size_t)clip_model.num_frames * 3, 1.0f);
                for (int f = 0; f < clip_model.num_frames; ++f) {
                    node.anim_scale[f * 3 + 0] = (node.scale[0] != 0.0f) ? node.scale[0] : 1.0f;
                    node.anim_scale[f * 3 + 1] = (node.scale[1] != 0.0f) ? node.scale[1] : 1.0f;
                    node.anim_scale[f * 3 + 2] = (node.scale[2] != 0.0f) ? node.scale[2] : 1.0f;
                }
            }
            if (node.anim_flags == 0) {
                node.anim_flags = 7;
            }
        }

        clip_model.num_mesh_nodes = 0;
        out_clips.emplace_back(clip_name, std::move(clip_model));
    }

    tg3_error_stack_free(&errors);
    tg3_model_free(&model);
    return true;
}

} // namespace av
