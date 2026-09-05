#include "tools/fbx_import.h"

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// FBX import module backed by ufbx (https://github.com/bqqqqqq/ufbx, MIT).
//
// ufbx does all the actual FBX work: binary/ASCII container parsing, node the
// hierarchy, geometry, materials, textures, coordinate-system conversion and
// skinning. This file only adapts the resulting ufbx_scene into the viewer's
// PODModel so .fbx files can be previewed by the existing renderer.
//
// All ufbx code lives in src/tools/ufbx/ufbx.c — see that folder's LICENSE.

namespace av {
namespace {

namespace fs = std::filesystem;

// Vertex dedup: same position + same uv + same normal -> one vertex.
struct MeshBuilder {
    std::vector<float> pos, nrm, uv;
    std::vector<uint32_t> idx;
    std::unordered_map<uint64_t, uint32_t> lut;

    uint32_t slot(int64_t p_key, int64_t u_key, int32_t n_key) {
        uint64_t key = ((uint64_t)(uint32_t)p_key << 40) |
                       ((uint64_t)(uint32_t)u_key << 24) |
                       ((uint64_t)(uint32_t)n_key & 0xFFFFFF);
        auto it = lut.find(key);
        if (it != lut.end()) return it->second;
        uint32_t s = (uint32_t)pos.size() / 3;
        lut[key] = s;
        return s;
    }
};

// Resolve a material's referenced texture to an existing file next to the fbx.
//
// The returned name is ALWAYS relative to `fbx_dir` (e.g. "body.png" or
// "images/body.png") so that the asset viewer can join it back onto the fbx's
// own folder and hit the exact file ufbx referenced — including textures that
// live in an "images"/"textures" subfolder. Returning bare basenames (the old
// behaviour) made the viewer fall through to unrelated .tex.png/.pvr game
// containers, which use a flipped (bottom-origin) storage and therefore mapped
// wrong on FBX previews (whose UVs are DCC-style, v = 0 at the top).
static std::string resolve_texture(const fs::path& fbx_dir, const std::string& tex, int mat_index) {
    auto base_name = [](const std::string& p) -> std::string {
        size_t slash = p.find_last_of("/\\");
        std::string b = slash == std::string::npos ? p : p.substr(slash + 1);
        size_t nul = b.find('\0');
        return nul == std::string::npos ? b : b.substr(0, nul);
    };
    std::string base = base_name(tex);

    // Candidate paths to probe: the fbx directory, then common subfolders
    // ("images", "textures", "maps"). FBX exports usually emit
    // "images/<name>.png"; the game SDK copies them to the fbx's own folder.
    std::vector<fs::path> search_roots = { fbx_dir };
    std::error_code ec;
    if (fs::is_directory(fbx_dir, ec)) {
        for (const char* sub : {"images", "textures", "maps", "Texture", "Textures"}) {
            fs::path p = fbx_dir / sub;
            if (fs::is_directory(p, ec)) search_roots.push_back(p);
        }
    }

    // Convert an absolute found path back into a name relative to fbx_dir.
    // This is what the viewer joins onto model_dir, so subfolders survive.
    auto relative_name = [&](const fs::path& abs) -> std::string {
        std::error_code ec2;
        fs::path rel = fs::relative(abs, fbx_dir, ec2);
        if (!ec2) {
            std::string s = rel.generic_string();
            if (!s.empty()) return s;
        }
        return base_name(abs.string());
    };

    auto exists = [&](const std::string& f, fs::path& out) {
        for (const auto& root : search_roots) {
            fs::path c = root / f;
            if (fs::is_regular_file(c, ec)) { out = c; return true; }
        }
        return false;
    };

    if (!base.empty()) {
        fs::path found;
        if (exists(base, found)) return relative_name(found);
        size_t dot = base.find('.');
        std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
        static const char* exts[] = {"jpg", "jpeg", "png", "tga", "bmp", "pvr", "tex", "dds"};
        for (const char* e : exts) {
            std::string f = stem + "." + e;
            if (exists(f, found)) return relative_name(found);
        }
        std::string f = stem + ".tex.png";
        if (exists(f, found)) return relative_name(found);
    }
    // Fallback: search the fbx directory for a "material_<N>" image.
    if (fs::is_directory(fbx_dir, ec)) {
        std::string want = "material_" + std::to_string(mat_index) + ".";
        for (const auto& ent : fs::directory_iterator(fbx_dir, ec)) {
            std::string nm = ent.path().filename().string();
            std::string low = nm;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            if (low.find(want) == std::string::npos) continue;
            static const char* iext[] = {".jpg", ".jpeg", ".png", ".tga", ".bmp", ".pvr", ".tex"};
            for (const char* e : iext)
                if (low.rfind(e) == low.size() - std::strlen(e)) return relative_name(ent.path());
        }
    }
    return base;
}

// ufbx strings are not guaranteed NUL-terminated; copy up to `length`.
static std::string ustr(const ufbx_string& s) {
    if (!s.data || s.length == 0) return {};
    return std::string(s.data, s.length);
}

// Diffuse texture filename for a material (FBX maps first, then PBR).
static std::string material_diffuse_file(const ufbx_material* mat) {
    if (!mat) return {};
    const ufbx_texture* tex = mat->fbx.diffuse_color.texture;
    if (!tex) tex = mat->pbr.base_color.texture;
    if (!tex) return {};
    std::string f = ustr(tex->relative_filename);
    if (f.empty()) f = ustr(tex->filename);
    if (f.empty()) f = ustr(tex->absolute_filename);
    return f;
}

} // namespace

// ─── public API ───────────────────────────────────────────────────────
PODModel fbx_parse(const std::vector<uint8_t>& data, const std::string& hint_path) {
    PODModel model;
    if (data.empty()) return model;

    ufbx_load_opts opts = {};
    // Target a Y-up, right-handed coordinate system (POD convention) and let
    // ufbx bake the conversion into node transforms and generated normals.
    // Skinning/embedded content are kept; animation is skipped (static preview).
    opts.target_axes           = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters    = 1.0;
    opts.ignore_animation      = true;
    opts.generate_missing_normals = true;
    if (!hint_path.empty()) {
        opts.filename.data   = hint_path.c_str();
        opts.filename.length = hint_path.size();
    }

    ufbx_error error = {};
    ufbx_scene* scene = ufbx_load_memory(data.data(), data.size(), &opts, &error);
    if (!scene) return model;

    // When the source file's handedness differs from our right-handed-y-up
    // target (e.g. 3ds Max / Unity left-handed exports), ufbx mirrors the
    // scene and flips UV V so textures keep their orientation. The viewer
    // needs to know this: for those files the UVs are already v = 0 at the
    // bottom (matching our bottom-origin uploads) and the shader must NOT
    // flip them again.
    model.uv_v_flipped =
        (scene->metadata.handedness_conversion_axis != UFBX_MIRROR_AXIS_NONE);

    fs::path fbx_dir;
    if (!hint_path.empty()) {
        fs::path p = hint_path;
        fbx_dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
    }

    // Global material registry, deduped by ufbx material pointer.
    std::map<const ufbx_material*, int> mat_registry;

    // Walk all nodes; bake each node's world transform into its vertices, split
    // geometry by material so every emitted PODMesh maps to one texture.
    auto process_node = [&](const ufbx_node* node) {
        if (!node || !node->mesh || !node->visible) return;
        const ufbx_mesh* mesh = node->mesh;
        if (mesh->num_faces == 0) return;

        const ufbx_matrix& m = node->geometry_to_world;

        // Per-sub-mesh builders, keyed by ufbx material list index.
        std::map<uint32_t, MeshBuilder> builders;

        for (size_t f = 0; f < mesh->faces.count; ++f) {
            const ufbx_face& face = mesh->faces.data[f];
            if (face.num_indices < 3) continue;

            // Material index of this face (falls back to 0 when not authored).
            uint32_t mat_idx = 0;
            if (mesh->face_material.count > f && f < mesh->face_material.count)
                mat_idx = mesh->face_material.data[f];

            // Fan-triangulate the polygon: (0,i,i+1).
            for (uint32_t k = 0; k + 2 < face.num_indices; ++k) {
                uint32_t corners[3] = {
                    face.index_begin,
                    face.index_begin + k + 1,
                    face.index_begin + k + 2,
                };
                MeshBuilder& mb = builders[mat_idx];
                for (int c = 0; c < 3; ++c) {
                    uint32_t ci = corners[c];
                    if (ci >= mesh->vertex_position.indices.count) continue;
                    uint32_t pi = mesh->vertex_position.indices.data[ci];
                    if (pi >= mesh->vertex_position.values.count) continue;
                    ufbx_vec3 p = mesh->vertex_position.values.data[pi];

                    ufbx_vec3 n = {0, 0, 1};
                    int64_t n_key = 0;
                    if (mesh->vertex_normal.exists &&
                        ci < mesh->vertex_normal.indices.count) {
                        uint32_t ni = mesh->vertex_normal.indices.data[ci];
                        if (ni < mesh->vertex_normal.values.count) {
                            n = mesh->vertex_normal.values.data[ni];
                            n_key = (int32_t)ni;
                        }
                    }

                    float u = 0, v = 0;
                    int64_t u_key = -1;
                    if (mesh->vertex_uv.exists &&
                        ci < mesh->vertex_uv.indices.count) {
                        uint32_t ui = mesh->vertex_uv.indices.data[ci];
                        if (ui < mesh->vertex_uv.values.count) {
                            ufbx_vec2 uv = mesh->vertex_uv.values.data[ui];
                            u = (float)uv.x;
                            v = (float)uv.y;
                            // FBX UVs use the glTF/DCC convention: v = 0 is the TOP
                            // of the texture. Keep them as-is so previewing an FBX
                            // with a plain (top-first) image matches the source
                            // program; the FBX→POD converter flips v when writing a
                            // game POD (whose convention is v = 0 at the bottom).
                            u_key = (int32_t)ui;
                        }
                    }

                    // Bake the node's world transform (Y-up, unit meters) into
                    // the vertex and normal.
                    ufbx_vec3 wp = ufbx_transform_position(&m, p);
                    ufbx_vec3 wn = ufbx_transform_direction(&m, n);
                    float nl = std::sqrt((float)(wn.x * wn.x + wn.y * wn.y + wn.z * wn.z));
                    if (nl > 1e-12f) { wn.x /= nl; wn.y /= nl; wn.z /= nl; }

                    uint32_t s = mb.slot(pi, u_key, (int32_t)n_key);
                    if (s == mb.pos.size() / 3) {
                        mb.pos.push_back((float)wp.x);
                        mb.pos.push_back((float)wp.y);
                        mb.pos.push_back((float)wp.z);
                        mb.nrm.push_back((float)wn.x);
                        mb.nrm.push_back((float)wn.y);
                        mb.nrm.push_back((float)wn.z);
                        mb.uv.push_back(u);
                        mb.uv.push_back(v);
                    }
                    mb.idx.push_back(s);
                }
            }
        }

        if (builders.empty()) return;

        // Materials the node uses: prefer the per-instance list from the node
        // (may differ per mesh instance), else the mesh's default materials.
        for (auto& kv : builders) {
            const ufbx_material* mat = nullptr;
            if (node->materials.count > 0 && kv.first < node->materials.count)
                mat = node->materials.data[kv.first];
            else if (kv.first < mesh->materials.count)
                mat = mesh->materials.data[kv.first];

            int global_mat = -1;
            auto it = mat_registry.find(mat);
            if (it != mat_registry.end()) global_mat = it->second;
            else {
                global_mat = (int)model.materials.size();
                mat_registry[mat] = global_mat;
                PODMaterial m;
                m.name = mat && !ustr(mat->name).empty() ? ustr(mat->name)
                                                         : "material_" + std::to_string(kv.first);
                std::string resolved = mat ? resolve_texture(fbx_dir, material_diffuse_file(mat), (int)kv.first) : "";
                m.diffuse_texture_index = -1;
                if (!resolved.empty()) {
                    int ti = (int)model.texture_filenames.size();
                    model.texture_filenames.push_back(resolved);
                    m.diffuse_texture_index = ti;
                }
                model.materials.push_back(std::move(m));
            }

            MeshBuilder& mb = kv.second;
            PODMesh mesh_out;
            mesh_out.positions = std::move(mb.pos);
            mesh_out.normals   = std::move(mb.nrm);
            mesh_out.uvs       = std::move(mb.uv);
            mesh_out.indices   = std::move(mb.idx);
            mesh_out.num_vertices = (int)(mesh_out.positions.size() / 3);
            mesh_out.num_faces    = (int)(mesh_out.indices.size() / 3);
            model.meshes.push_back(std::move(mesh_out));

            PODNode pn;
            pn.name = "mesh";
            pn.object_index   = (int)model.meshes.size() - 1;
            pn.material_index = global_mat;
            pn.has_matrix     = true;
            for (int i = 0; i < 16; ++i) pn.matrix[i] = 0.0f;
            pn.matrix[0] = pn.matrix[5] = pn.matrix[10] = pn.matrix[15] = 1.0f;
            model.nodes.push_back(std::move(pn));
        }
    };

    // Walk the scene graph starting from the root node.
    std::vector<const ufbx_node*> stack;
    stack.push_back(scene->root_node);
    while (!stack.empty()) {
        const ufbx_node* node = stack.back();
        stack.pop_back();
        if (!node) continue;
        process_node(node);
        for (size_t i = 0; i < node->children.count; ++i)
            stack.push_back(node->children.data[i]);
    }

    ufbx_free_scene(scene);

    if (model.meshes.empty()) return model;

    // Compute accurate per-mesh and whole-model bounding box without squashing
    float minx =  1e30f, miny =  1e30f, minz =  1e30f;
    float maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
    model.total_vertices = 0;
    model.total_faces    = 0;

    for (auto& m : model.meshes) {
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
        m.min_x = m_minx; m.max_x = m_maxx;
        m.min_y = m_miny; m.max_y = m_maxy;
        m.min_z = m_minz; m.max_z = m_maxz;

        minx = std::min(minx, m_minx); maxx = std::max(maxx, m_maxx);
        miny = std::min(miny, m_miny); maxy = std::max(maxy, m_maxy);
        minz = std::min(minz, m_minz); maxz = std::max(maxz, m_maxz);

        model.total_vertices += m.num_vertices;
        model.total_faces    += m.num_faces;
    }

    model.min_x = minx; model.max_x = maxx;
    model.min_y = miny; model.max_y = maxy;
    model.min_z = minz; model.max_z = maxz;
    model.center_x = (minx + maxx) * 0.5f;
    model.center_y = (miny + maxy) * 0.5f;
    model.center_z = (minz + maxz) * 0.5f;

    float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
    model.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (model.radius < 1.0f) model.radius = 1.0f;

    model.num_mesh_nodes = (int)model.nodes.size();
    model.num_frames = 0; // static FBX import; animation not decoded yet
    return model;
}

PODModel fbx_load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return fbx_parse(data, path);
}

} // namespace av