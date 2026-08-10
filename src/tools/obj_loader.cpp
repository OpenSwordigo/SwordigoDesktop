// obj_loader.cpp — Wavefront .obj → PODModel.
//
// The parser is modeled on zauonlok/renderer's core/mesh.c (MIT, Zhou Le —
// vendored reference in src/render/zauonlok/) but generalized: it accepts
// the four standard corner forms, triangulates quads/ngons, and skips
// malformed lines instead of asserting. The `# ext.*` comments are kept
// byte-compatible with the reference exporter.

#include "obj_loader.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

namespace av {
namespace {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Corner { int p = -1, t = -1, n = -1; };

bool file_exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return static_cast<bool>(f);
}

std::string dir_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string stem_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    size_t b = slash == std::string::npos ? 0 : slash + 1;
    if (dot == std::string::npos || dot <= b) return path.substr(b);
    return path.substr(b, dot - b);
}

// ── .mtl reader: newmtl blocks, usemtl selection, Kd + map_Kd ────────
struct MtlEntry {
    float kd[3] = {1, 1, 1};
    std::string map_kd;
    bool saw_kd = false;
};

// map_Kd often carries options before the filename ("map_Kd -s 1 1 1 tex.png")
// — take the last whitespace token as the path.
std::string last_token(const std::string& s) {
    size_t b = s.find_last_of(" \t");
    return b == std::string::npos ? s : s.substr(b + 1);
}

bool read_mtl(const std::string& path, const std::string& want, MtlEntry& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::vector<std::pair<std::string, MtlEntry>> blocks;
    MtlEntry cur;
    std::string cur_name;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("newmtl ", 0) == 0) {
            if (!cur_name.empty()) blocks.emplace_back(cur_name, cur);
            cur = MtlEntry{};
            size_t sp = line.find_first_not_of(" \t", 7);
            cur_name = sp == std::string::npos ? std::string() : line.substr(sp);
        } else if (line.rfind("Kd ", 0) == 0) {
            float r = 0, g = 0, b = 0;
            if (sscanf(line.c_str(), "Kd %f %f %f", &r, &g, &b) == 3) {
                cur.kd[0] = r; cur.kd[1] = g; cur.kd[2] = b;
                cur.saw_kd = true;
            }
        } else if (line.rfind("map_Kd ", 0) == 0) {
            cur.map_kd = last_token(line.substr(7));
        }
    }
    if (!cur_name.empty()) blocks.emplace_back(cur_name, cur);

    const MtlEntry* best = nullptr;
    for (const auto& b : blocks)
        if (b.first == want) { best = &b.second; break; }
    if (!best)
        for (const auto& b : blocks)
            if (!b.second.map_kd.empty() || b.second.saw_kd) { best = &b.second; break; }
    if (!best && !blocks.empty()) best = &blocks[0].second;
    if (best) {
        out = *best;
        return out.saw_kd || !out.map_kd.empty();
    }
    return false;
}

} // namespace

bool obj_load(const std::string& path, PODModel& out, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }

    std::vector<Vec3> positions, normals;
    std::vector<Vec2> texcoords;
    std::vector<Corner> corners;
    std::vector<Vec4> ext_tangent, ext_joint, ext_weight;
    std::string mtllib;
    std::string usemtl;

    std::string line;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        std::string s = line.substr(b);

        if (s[0] == '#') {
            if (s.rfind("# ext.tangent", 0) == 0) {
                Vec4 v;
                if (sscanf(s.c_str(), "# ext.tangent %f %f %f %f", &v.x, &v.y, &v.z, &v.w) == 4)
                    ext_tangent.push_back(v);
            } else if (s.rfind("# ext.joint", 0) == 0) {
                Vec4 v;
                if (sscanf(s.c_str(), "# ext.joint %f %f %f %f", &v.x, &v.y, &v.z, &v.w) == 4)
                    ext_joint.push_back(v);
            } else if (s.rfind("# ext.weight", 0) == 0) {
                Vec4 v;
                if (sscanf(s.c_str(), "# ext.weight %f %f %f %f", &v.x, &v.y, &v.z, &v.w) == 4)
                    ext_weight.push_back(v);
            }
            continue;
        }

        if (s.rfind("v ", 0) == 0) {
            Vec3 v;
            if (sscanf(s.c_str(), "v %f %f %f", &v.x, &v.y, &v.z) == 3) positions.push_back(v);
        } else if (s.rfind("vt ", 0) == 0) {
            Vec2 v;
            if (sscanf(s.c_str(), "vt %f %f", &v.x, &v.y) == 2) texcoords.push_back(v);
        } else if (s.rfind("vn ", 0) == 0) {
            Vec3 v;
            if (sscanf(s.c_str(), "vn %f %f %f", &v.x, &v.y, &v.z) == 3) normals.push_back(v);
        } else if (s.rfind("f ", 0) == 0) {
            std::vector<Corner> fc;
            const char* p = s.c_str() + 2;
            while (*p) {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p) break;
                Corner c;
                int pi = -1, ti = -1, ni = -1;
                if (sscanf(p, "%d/%d/%d", &pi, &ti, &ni) == 3) {
                    /* full form */
                } else if (sscanf(p, "%d//%d", &pi, &ni) == 2) {
                    ti = -1;
                } else if (sscanf(p, "%d/%d", &pi, &ti) == 2) {
                    ni = -1;
                } else if (sscanf(p, "%d", &pi) == 1) {
                    ti = ni = -1;
                }
                if (pi >= 1) { c.p = pi - 1; c.t = ti - 1; c.n = ni - 1; fc.push_back(c); }
                while (*p && *p != ' ' && *p != '\t') ++p;
            }
            for (size_t k = 1; k + 1 < fc.size(); ++k) {
                corners.push_back(fc[0]);
                corners.push_back(fc[k]);
                corners.push_back(fc[k + 1]);
            }
        } else if (s.rfind("mtllib ", 0) == 0) {
            mtllib = s.substr(7);
            size_t e = mtllib.find_last_not_of(" \t\r");
            if (e != std::string::npos) mtllib = mtllib.substr(0, e + 1);
        } else if (s.rfind("usemtl ", 0) == 0) {
            usemtl = s.substr(7);
            size_t e = usemtl.find_last_not_of(" \t\r");
            if (e != std::string::npos) usemtl = usemtl.substr(0, e + 1);
        }
    }

    if (corners.empty() || positions.empty()) {
        if (err) *err = "no faces or vertices in " + path;
        return false;
    }

    PODModel m;
    PODMesh mesh;
    const int n = static_cast<int>(corners.size());
    const bool have_ext = !ext_joint.empty() || !ext_weight.empty();
    const bool have_tangents = !ext_tangent.empty();
    mesh.positions.reserve(static_cast<size_t>(n) * 3);
    mesh.normals.reserve(static_cast<size_t>(n) * 3);
    mesh.uvs.reserve(static_cast<size_t>(n) * 2);
    if (have_tangents) mesh.tangents.reserve(static_cast<size_t>(n) * 4);
    if (have_ext) {
        mesh.bone_indices.assign(static_cast<size_t>(n) * 4, 0.0f);
        mesh.bone_weights.assign(static_cast<size_t>(n) * 4, 0.0f);
        mesh.bones_per_vertex = 0;   // GPU path only — see header comment
    }

    float minx = 1e9f, miny = 1e9f, minz = 1e9f;
    float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;

    for (int i = 0; i < n; ++i) {
        const Corner& c = corners[i];
        const Vec3& p = positions[c.p >= 0 && c.p < (int)positions.size() ? c.p : 0];
        const Vec3& nr = (c.n >= 0 && c.n < (int)normals.size()) ? normals[c.n] : Vec3{0, 1, 0};
        const Vec2& uv = (c.t >= 0 && c.t < (int)texcoords.size()) ? texcoords[c.t] : Vec2{0, 0};

        mesh.positions.push_back(p.x); mesh.positions.push_back(p.y); mesh.positions.push_back(p.z);
        mesh.normals.push_back(nr.x);  mesh.normals.push_back(nr.y);  mesh.normals.push_back(nr.z);
        mesh.uvs.push_back(uv.x);      mesh.uvs.push_back(uv.y);

        if (have_tangents) {
            const Vec4& t = i < (int)ext_tangent.size() ? ext_tangent[i] : Vec4{0, 0, 0, 0};
            mesh.tangents.push_back(t.x); mesh.tangents.push_back(t.y);
            mesh.tangents.push_back(t.z); mesh.tangents.push_back(t.w);
        }
        if (have_ext) {
            const Vec4& j = i < (int)ext_joint.size() ? ext_joint[i] : Vec4{0, 0, 0, 0};
            const Vec4& w = i < (int)ext_weight.size() ? ext_weight[i] : Vec4{0, 0, 0, 0};
            mesh.bone_indices[i * 4 + 0] = j.x; mesh.bone_indices[i * 4 + 1] = j.y;
            mesh.bone_indices[i * 4 + 2] = j.z; mesh.bone_indices[i * 4 + 3] = j.w;
            mesh.bone_weights[i * 4 + 0] = w.x; mesh.bone_weights[i * 4 + 1] = w.y;
            mesh.bone_weights[i * 4 + 2] = w.z; mesh.bone_weights[i * 4 + 3] = w.w;
        }

        minx = std::min(minx, p.x); miny = std::min(miny, p.y); minz = std::min(minz, p.z);
        maxx = std::max(maxx, p.x); maxy = std::max(maxy, p.y); maxz = std::max(maxz, p.z);
    }

    mesh.num_vertices = n;
    mesh.num_faces = n / 3;
    mesh.indices.resize(static_cast<size_t>(n));
    std::iota(mesh.indices.begin(), mesh.indices.end(), 0u);
    mesh.min_x = minx; mesh.min_y = miny; mesh.min_z = minz;
    mesh.max_x = maxx; mesh.max_y = maxy; mesh.max_z = maxz;
    m.meshes.push_back(std::move(mesh));

    // ── Material: mtl (first Kd/map_Kd) or same-stem texture ───────────
    std::string tex_name;
    float kd[3] = {1, 1, 1};
    if (!mtllib.empty()) {
        std::string mtl_path = dir_of(path);
        if (!mtl_path.empty()) mtl_path += "/";
        MtlEntry info;
        if (read_mtl(mtl_path + mtllib, usemtl, info)) {
            if (info.saw_kd) { kd[0] = info.kd[0]; kd[1] = info.kd[1]; kd[2] = info.kd[2]; }
            if (!info.map_kd.empty()) {
                size_t sp = info.map_kd.find_last_of('/');
                tex_name = sp == std::string::npos ? info.map_kd : info.map_kd.substr(sp + 1);
            }
        }
    }
    if (tex_name.empty()) {
        // Same-stem texture fallback, covering the renderer-master demo naming
        // convention (<mesh>_diffuse.tga — the demo OBJs carry no mtllib).
        std::string stem = stem_of(path);
        std::string dir = dir_of(path);
        static const char* kSuffixes[] = {"", "_diffuse", "_basecolor", "_albedo", "_color", "_diff"};
        static const char* kExts[] = {".tga", ".png", ".jpg", ".jpeg", ".bmp"};
        for (const char* suf : kSuffixes) {
            for (const char* e : kExts) {
                std::string cand = dir.empty() ? stem + suf + e : dir + "/" + stem + suf + e;
                if (file_exists(cand)) { tex_name = stem + suf + e; break; }
            }
            if (!tex_name.empty()) break;
        }
    }

    PODMaterial mat;
    mat.name = "default";
    mat.diffuse[0] = kd[0]; mat.diffuse[1] = kd[1]; mat.diffuse[2] = kd[2];
    if (!tex_name.empty()) {
        m.texture_filenames.push_back(tex_name);
        mat.diffuse_texture_index = 0;
    } else {
        mat.diffuse_texture_index = -1;
    }
    m.materials.push_back(std::move(mat));

    PODNode node;
    node.name = "mesh0";
    node.object_index = 0;
    node.parent_index = -1;
    node.material_index = 0;
    m.nodes.push_back(std::move(node));
    m.num_mesh_nodes = 1;

    m.min_x = minx; m.min_y = miny; m.min_z = minz;
    m.max_x = maxx; m.max_y = maxy; m.max_z = maxz;
    m.center_x = (minx + maxx) * 0.5f;
    m.center_y = (miny + maxy) * 0.5f;
    m.center_z = (minz + maxz) * 0.5f;
    float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
    m.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (m.radius < 1.0f) m.radius = 1.0f;
    m.total_vertices = n;
    m.total_faces = n / 3;
    m.uv_v_flipped = false;   // OBJ UVs are DCC top-origin — viewer flips V

    out = std::move(m);
    return true;
}

} // namespace av
