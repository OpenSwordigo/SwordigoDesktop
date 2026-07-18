/* scene_loader.cpp — Parse & serialize Swordigo .scene protobuf files
 *
 * Pipeline (load):
 *   1. Read entire .scene binary into memory
 *   2. Use proto::Reader to iterate top-level fields
 *      Field 1 (Object)        → parse_object  → SceneObject
 *      Field 2 (ObjectLibrary) → raw bytes      → SceneData::object_libraries
 *      Field 3 (Bounds)        → raw bytes      → SceneData::bounds
 *      Field 4 (Group)         → raw bytes      → SceneData::groups
 *      Field 5 (OnLoad)        → raw bytes      → SceneData::onload_scripts
 *      All other tags          → proto::Field   → SceneData::other_fields
 *   3. Compute scene AABB from object positions
 *
 * Pipeline (save):
 *   1. Use proto::Writer to re-emit every SceneObject (tag 1, via serialize_object)
 *   2. Write preserved raw-byte sections verbatim (tags 2-5)
 *   3. Write any unrecognised other_fields verbatim
 *   4. Flush binary to disk
 *
 * True SceneObject wire format (confirmed from block_formats.py + decomp.js):
 *   Tag 1  LEN    TemplateName  string
 *   Tag 2  LEN    Identifier    string
 *   Tag 3  LEN    Component     repeated nested
 *   Tag 4  LEN    Position      Vector2 { tag1=X(I32 float), tag2=Y(I32 float) }
 *   Tag 5  I32    Depth         float   (world Z / parallax layer)
 *   Tag 6  I32    Rotation      float   (Y-axis rotation, radians)
 *   Tag 7  I32    Scaling       float   (uniform scale)
 *   Tag 8  LEN    LocalAabb     Rectangle (raw bytes preserved)
 *   Tag 9  VAR    Hidden        bool
 *   Tag 10 LEN    OnLoad        Program (raw bytes preserved)
 */

#include "scene_loader.h"
#include "platform/protobuf_reader.h"

#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

namespace av {

// ============================================================
// Helpers
// ============================================================

static bool is_printable(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

static bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    auto it  = s.rbegin();
    auto sit = suffix.rbegin();
    for (; sit != suffix.rend(); ++it, ++sit) {
        if (std::tolower(static_cast<unsigned char>(*it)) !=
            std::tolower(static_cast<unsigned char>(*sit)))
            return false;
    }
    return true;
}

// Scan raw component bytes for .pod / .pvr / .png asset references
static void scan_for_asset_refs(const std::string& raw,
                                std::string& out_mesh,
                                std::string& out_texture) {
    try {
        proto::Reader sub(raw);
        proto::Field  f;
        while (sub.read_field(f)) {
            if (f.wire_type != proto::WIRE_LEN) continue;
            const std::string& v = f.bytes_val;
            if (!is_printable(v)) continue;
            if (ends_with_ci(v, ".pod") && out_mesh.empty())
                out_mesh = v;
            if ((ends_with_ci(v, ".pvr") || ends_with_ci(v, ".png")) && out_texture.empty())
                out_texture = v;
        }
    } catch (...) {}
}

// ============================================================
// Component parse (raw_data stores entire message bytes so we
// can write it back verbatim on scene_save)
// ============================================================
static SceneComponent parse_component(const std::string& bytes) {
    SceneComponent comp;
    comp.raw_data = bytes;  // preserve verbatim for round-trip

    try {
        proto::Reader reader(bytes);
        proto::Field  f;
        while (reader.read_field(f)) {
            switch (f.field_number) {
                case 1:
                    if (f.wire_type == proto::WIRE_LEN && is_printable(f.bytes_val))
                        comp.type_name = f.bytes_val;
                    break;
                case 2:
                    if (f.wire_type == proto::WIRE_VARINT)
                        comp.type_id = static_cast<int>(f.varint_val);
                    break;
                default:
                    break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[scene_loader] warning: component parse error: " << e.what() << "\n";
    }
    return comp;
}

// ============================================================
// GroundMesh parsing (unchanged from previous implementation)
// ============================================================
struct MeshData_t {
    int data_type  = 0;
    int components = 0;
    int offset     = 0;
    int stride     = 0;
};

static MeshData_t parse_mesh_data(const std::string& bytes) {
    MeshData_t md;
    proto::Reader reader(bytes);
    proto::Field f;
    while (reader.read_field(f)) {
        if      (f.field_number == 1 && f.wire_type == proto::WIRE_VARINT) md.data_type  = static_cast<int>(f.varint_val) + 0x13ff;
        else if (f.field_number == 2 && f.wire_type == proto::WIRE_VARINT) md.components = static_cast<int>(f.varint_val);
        else if (f.field_number == 3 && f.wire_type == proto::WIRE_VARINT) md.offset     = static_cast<int>(f.varint_val);
        else if (f.field_number == 4 && f.wire_type == proto::WIRE_VARINT) md.stride     = static_cast<int>(f.varint_val);
    }
    return md;
}

static std::string parse_mesh_texture_name(const std::string& bytes) {
    proto::Reader reader(bytes);
    proto::Field f;
    while (reader.read_field(f))
        if (f.field_number == 1 && f.wire_type == proto::WIRE_LEN) return f.bytes_val;
    return "";
}

static std::string parse_mesh_material(const std::string& bytes) {
    proto::Reader reader(bytes);
    proto::Field f;
    while (reader.read_field(f))
        if (f.field_number == 5 && f.wire_type == proto::WIRE_LEN) return parse_mesh_texture_name(f.bytes_val);
    return "";
}

static void parse_single_mesh(SceneObject& obj, const std::string& bytes) {
    proto::Reader reader(bytes);
    proto::Field f;

    int index_count = 0;
    MeshData_t indices_meta, positions_meta, normals_meta;
    std::vector<MeshData_t> texcoords_meta;
    std::string texture_name, vertex_data, index_data;
    bool has_indices_meta = false, has_positions_meta = false, has_normals_meta = false;

    while (reader.read_field(f)) {
        if      (f.field_number == 2  && f.wire_type == proto::WIRE_VARINT) { index_count = static_cast<int>(f.varint_val); }
        else if (f.field_number == 3  && f.wire_type == proto::WIRE_LEN)    { indices_meta   = parse_mesh_data(f.bytes_val); has_indices_meta   = true; }
        else if (f.field_number == 4  && f.wire_type == proto::WIRE_LEN)    { positions_meta = parse_mesh_data(f.bytes_val); has_positions_meta = true; }
        else if (f.field_number == 5  && f.wire_type == proto::WIRE_LEN)    { normals_meta   = parse_mesh_data(f.bytes_val); has_normals_meta   = true; }
        else if (f.field_number == 6  && f.wire_type == proto::WIRE_LEN)    { texcoords_meta.push_back(parse_mesh_data(f.bytes_val)); }
        else if (f.field_number == 16 && f.wire_type == proto::WIRE_LEN)    { texture_name   = parse_mesh_material(f.bytes_val); }
        else if (f.field_number == 50 && f.wire_type == proto::WIRE_LEN)    { vertex_data    = f.bytes_val; }
        else if (f.field_number == 51 && f.wire_type == proto::WIRE_LEN)    { index_data     = f.bytes_val; }
    }

    if (!has_positions_meta || vertex_data.empty()) return;

    PODMesh pm;
    int v_stride = positions_meta.stride > 0 ? positions_meta.stride : positions_meta.components * 4;
    int num_vertices = (int)(vertex_data.size() - positions_meta.offset) / v_stride;
    if (num_vertices <= 0) return;
    pm.num_vertices = num_vertices;

    pm.positions.resize(num_vertices * 3, 0.0f);
    for (int i = 0; i < num_vertices; ++i) {
        const char* ptr = vertex_data.data() + positions_meta.offset + i * v_stride;
        if (positions_meta.components >= 3) {
            pm.positions[i*3+0] = *(const float*)(ptr);
            pm.positions[i*3+1] = *(const float*)(ptr+4);
            pm.positions[i*3+2] = *(const float*)(ptr+8);
        } else if (positions_meta.components == 2) {
            pm.positions[i*3+0] = *(const float*)(ptr);
            pm.positions[i*3+1] = *(const float*)(ptr+4);
            pm.positions[i*3+2] = 0.0f;
        }
    }

    if (has_normals_meta && normals_meta.stride > 0) {
        pm.normals.resize(num_vertices * 3, 0.0f);
        for (int i = 0; i < num_vertices; ++i) {
            const char* ptr = vertex_data.data() + normals_meta.offset + i * normals_meta.stride;
            if (normals_meta.components >= 3) {
                pm.normals[i*3+0] = *(const float*)(ptr);
                pm.normals[i*3+1] = *(const float*)(ptr+4);
                pm.normals[i*3+2] = *(const float*)(ptr+8);
            }
        }
    }

    if (!texcoords_meta.empty() && texcoords_meta[0].stride > 0) {
        const auto& uv = texcoords_meta[0];
        pm.uvs.resize(num_vertices * 2, 0.0f);
        for (int i = 0; i < num_vertices; ++i) {
            const char* ptr = vertex_data.data() + uv.offset + i * uv.stride;
            if (uv.components >= 2) {
                pm.uvs[i*2+0] = *(const float*)(ptr);
                pm.uvs[i*2+1] = *(const float*)(ptr+4);
            }
        }
    }

    if (has_indices_meta && !index_data.empty() && index_count > 0) {
        pm.indices.resize(index_count);
        if (indices_meta.data_type == 0x1403) {
            for (int i = 0; i < index_count; ++i)
                if (indices_meta.offset + i*2+2 <= (int)index_data.size())
                    pm.indices[i] = *(const uint16_t*)(index_data.data() + indices_meta.offset + i*2);
        } else if (indices_meta.data_type == 0x1401) {
            for (int i = 0; i < index_count; ++i)
                if (indices_meta.offset + i < (int)index_data.size())
                    pm.indices[i] = *(const uint8_t*)(index_data.data() + indices_meta.offset + i);
        } else if (indices_meta.data_type == 0x1405) {
            for (int i = 0; i < index_count; ++i)
                if (indices_meta.offset + i*4+4 <= (int)index_data.size())
                    pm.indices[i] = (uint16_t)(*(const uint32_t*)(index_data.data() + indices_meta.offset + i*4));
        }
        pm.num_faces = index_count / 3;
    }

    if (!pm.positions.empty()) {
        pm.min_x = pm.min_y = pm.min_z = 1e9f;
        pm.max_x = pm.max_y = pm.max_z = -1e9f;
        for (int i = 0; i < pm.num_vertices; ++i) {
            float x = pm.positions[i*3+0], y = pm.positions[i*3+1], z = pm.positions[i*3+2];
            pm.min_x = std::min(pm.min_x, x); pm.max_x = std::max(pm.max_x, x);
            pm.min_y = std::min(pm.min_y, y); pm.max_y = std::max(pm.max_y, y);
            pm.min_z = std::min(pm.min_z, z); pm.max_z = std::max(pm.max_z, z);
        }
    }

    obj.ground_meshes.push_back(std::move(pm));
    obj.ground_mesh_textures.push_back(texture_name);
}

static void parse_ground_mesh_component(SceneObject& obj, const std::string& component_bytes) {
    proto::Reader reader(component_bytes);
    proto::Field f;
    while (reader.read_field(f)) {
        if (f.field_number == 111 && f.wire_type == proto::WIRE_LEN) {
            proto::Reader gm_reader(f.bytes_val);
            proto::Field gm_f;
            while (gm_reader.read_field(gm_f)) {
                if (gm_f.field_number == 8 && gm_f.wire_type == proto::WIRE_LEN)
                    parse_single_mesh(obj, gm_f.bytes_val);
                else if (gm_f.field_number == 9 && gm_f.wire_type == proto::WIRE_LEN)
                    parse_single_mesh(obj, gm_f.bytes_val);
            }
        }
    }
}

// ============================================================
// Parse a single SceneObject from its nested protobuf bytes.
// Uses the TRUE wire format confirmed from block_formats.py:
//   Tag 1 LEN  TemplateName
//   Tag 2 LEN  Identifier (name)
//   Tag 3 LEN  Component  (repeated)
//   Tag 4 LEN  Position   (Vector2: tag1=X I32 float, tag2=Y I32 float)
//   Tag 5 I32  Depth      (float — world Z)
//   Tag 6 I32  Rotation   (float — Y-axis, radians)
//   Tag 7 I32  Scaling    (float — uniform scale)
//   Tag 8 LEN  LocalAabb  (Rectangle raw bytes, preserved)
//   Tag 9 VAR  Hidden     (bool)
//   Tag 10 LEN OnLoad     (Program raw bytes, preserved)
// ============================================================
static SceneObject parse_object(const std::string& bytes) {
    SceneObject obj;

    try {
        proto::Reader reader(bytes);
        proto::Field  f;
        while (reader.read_field(f)) {
            switch (f.field_number) {

                case 1: // TemplateName
                    if (f.wire_type == proto::WIRE_LEN)
                        obj.template_name = f.bytes_val;
                    break;

                case 2: // Identifier / name
                    if (f.wire_type == proto::WIRE_LEN && is_printable(f.bytes_val))
                        obj.name = f.bytes_val;
                    break;

                case 3: // Component (repeated)
                    if (f.wire_type == proto::WIRE_LEN)
                        obj.components.push_back(parse_component(f.bytes_val));
                    break;

                case 4: // Position (nested Vector2)
                    if (f.wire_type == proto::WIRE_LEN) {
                        proto::Reader pos_r(f.bytes_val);
                        proto::Field  pos_f;
                        while (pos_r.read_field(pos_f)) {
                            if (pos_f.field_number == 1 && pos_f.wire_type == proto::WIRE_I32)
                                obj.pos_x = pos_f.float_val;
                            else if (pos_f.field_number == 2 && pos_f.wire_type == proto::WIRE_I32)
                                obj.pos_y = pos_f.float_val;
                        }
                    }
                    break;

                case 5: // Depth (world Z)
                    if (f.wire_type == proto::WIRE_I32)
                        obj.pos_z = f.float_val;
                    break;

                case 6: // Rotation (Y-axis)
                    if (f.wire_type == proto::WIRE_I32) {
                        obj.rot_y = f.float_val;
                        obj.rot_x = obj.rot_z = 0.0f;
                    }
                    break;

                case 7: // Scaling (uniform)
                    if (f.wire_type == proto::WIRE_I32) {
                        obj.scale_x = f.float_val;
                        obj.scale_y = f.float_val;
                        obj.scale_z = f.float_val;
                    }
                    break;

                case 8: // LocalAabb (Rectangle, preserve raw bytes)
                    if (f.wire_type == proto::WIRE_LEN)
                        obj.local_aabb = f.bytes_val;
                    break;

                case 9: // Hidden (bool)
                    if (f.wire_type == proto::WIRE_VARINT)
                        obj.hidden = f.as_bool();
                    break;

                case 10: // OnLoad (Program, preserve raw bytes)
                    if (f.wire_type == proto::WIRE_LEN)
                        obj.onload = f.bytes_val;
                    break;

                default:
                    break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[scene_loader] warning: object parse error: " << e.what() << "\n";
    }

    // Post-process: scan components for asset references
    for (const auto& comp : obj.components) {
        if (comp.type_name == "GroundMeshComponent")
            parse_ground_mesh_component(obj, comp.raw_data);

        if (comp.type_name == "MeshRenderer" || comp.type_name == "SkinnedMeshRenderer")
            scan_for_asset_refs(comp.raw_data, obj.mesh_name, obj.texture_name);

        if (comp.type_name == "Background") {
            std::string dummy_tex;
            scan_for_asset_refs(comp.raw_data, obj.background_name, dummy_tex);
            if (obj.texture_name.empty() && !dummy_tex.empty())
                obj.texture_name = dummy_tex;
        }

        if (obj.mesh_name.empty())
            scan_for_asset_refs(comp.raw_data, obj.mesh_name, obj.texture_name);
    }

    return obj;
}

// ============================================================
// Serialize a SceneObject back to protobuf binary bytes.
// Mirrors parse_object exactly: every field is written in the
// same tag order so file diffs are minimal.
// ============================================================
static std::string serialize_object(const SceneObject& obj) {
    proto::Writer w;

    // Tag 1: TemplateName
    if (!obj.template_name.empty())
        w.write_string_field(1, obj.template_name);

    // Tag 2: Identifier
    if (!obj.name.empty())
        w.write_string_field(2, obj.name);

    // Tag 3: Component[] — write raw bytes verbatim (preserves all sub-fields)
    for (const auto& comp : obj.components)
        w.write_bytes_field(3, comp.raw_data);

    // Tag 4: Position (Vector2 nested message)
    {
        proto::Writer pos;
        pos.write_float_field(1, obj.pos_x);
        pos.write_float_field(2, obj.pos_y);
        w.write_nested_field(4, pos);
    }

    // Tag 5: Depth
    w.write_float_field(5, obj.pos_z);

    // Tag 6: Rotation
    w.write_float_field(6, obj.rot_y);

    // Tag 7: Scaling (uniform — use scale_x)
    w.write_float_field(7, obj.scale_x);

    // Tag 8: LocalAabb (preserved raw bytes)
    if (!obj.local_aabb.empty())
        w.write_bytes_field(8, obj.local_aabb);

    // Tag 9: Hidden
    w.write_varint_field(9, obj.hidden ? 1ULL : 0ULL);

    // Tag 10: OnLoad (preserved raw bytes)
    if (!obj.onload.empty())
        w.write_bytes_field(10, obj.onload);

    return w.to_string();
}

// ============================================================
// Compute AABB from all object positions
// ============================================================
static void compute_bounds(SceneData& scene) {
    if (scene.objects.empty()) return;

    float min_x = scene.objects[0].pos_x;
    float min_y = scene.objects[0].pos_y;
    float min_z = scene.objects[0].pos_z;
    float max_x = min_x, max_y = min_y, max_z = min_z;

    for (const auto& obj : scene.objects) {
        min_x = std::min(min_x, obj.pos_x);
        min_y = std::min(min_y, obj.pos_y);
        min_z = std::min(min_z, obj.pos_z);
        max_x = std::max(max_x, obj.pos_x);
        max_y = std::max(max_y, obj.pos_y);
        max_z = std::max(max_z, obj.pos_z);

        for (const auto& gm : obj.ground_meshes) {
            if (gm.num_vertices > 0) {
                min_x = std::min(min_x, obj.pos_x + gm.min_x * obj.scale_x);
                min_y = std::min(min_y, obj.pos_y + gm.min_y * obj.scale_y);
                min_z = std::min(min_z, obj.pos_z + gm.min_z * obj.scale_z);
                max_x = std::max(max_x, obj.pos_x + gm.max_x * obj.scale_x);
                max_y = std::max(max_y, obj.pos_y + gm.max_y * obj.scale_y);
                max_z = std::max(max_z, obj.pos_z + gm.max_z * obj.scale_z);
            }
        }
    }

    scene.bounds_min[0] = min_x;
    scene.bounds_min[1] = min_y;
    scene.bounds_min[2] = min_z;
    scene.bounds_max[0] = max_x;
    scene.bounds_max[1] = max_y;
    scene.bounds_max[2] = max_z;
}

// ============================================================
// Public API — Load
// ============================================================
SceneData scene_load(const std::string& path) {
    SceneData scene;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[scene_loader] error: cannot open " << path << "\n";
        return scene;
    }
    const auto size = file.tellg();
    if (size <= 0) {
        std::cerr << "[scene_loader] error: empty file " << path << "\n";
        return scene;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(buf.data()), size);
    file.close();

    scene.filename = fs::path(path).filename().string();
    scene.filepath = path;

    try {
        proto::Reader reader(buf.data(), buf.size());
        proto::Field  f;

        while (reader.read_field(f)) {
            switch (f.field_number) {
                case 1: // SceneObject
                    if (f.wire_type == proto::WIRE_LEN)
                        scene.objects.push_back(parse_object(f.bytes_val));
                    break;
                case 2: // ObjectLibrary (raw bytes preserved)
                    if (f.wire_type == proto::WIRE_LEN)
                        scene.object_libraries.push_back(f.bytes_val);
                    break;
                case 3: // Bounds / Rectangle (raw bytes preserved)
                    if (f.wire_type == proto::WIRE_LEN)
                        scene.bounds.push_back(f.bytes_val);
                    break;
                case 4: // Group / SceneObjectGroup (raw bytes preserved)
                    if (f.wire_type == proto::WIRE_LEN)
                        scene.groups.push_back(f.bytes_val);
                    break;
                case 5: // OnLoad / Program (raw bytes preserved)
                    if (f.wire_type == proto::WIRE_LEN)
                        scene.onload_scripts.push_back(f.bytes_val);
                    break;
                default: // Unknown future tags — preserve verbatim
                    scene.other_fields.push_back(f);
                    break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[scene_loader] error: top-level parse failed: " << e.what() << "\n";
    }

    scene.object_count = static_cast<int>(scene.objects.size());
    compute_bounds(scene);

    std::cout << "[scene_loader] loaded " << scene.filename
              << ": " << scene.object_count << " objects"
              << ", " << scene.object_libraries.size() << " libraries"
              << ", " << scene.bounds.size() << " bounds"
              << ", " << scene.groups.size() << " groups"
              << ", " << scene.onload_scripts.size() << " scene-onload scripts\n";

    return scene;
}

// ============================================================
// Public API — Save
// Serialises the full scene back to binary protobuf.
// Field order matches the load order: objects first, then
// libraries, bounds, groups, onload scripts, then any unknown.
// ============================================================
void scene_save(const std::string& path, const SceneData& scene) {
    proto::Writer w;

    // Tag 1: SceneObject[] — re-serialise every object
    for (const auto& obj : scene.objects)
        w.write_bytes_field(1, serialize_object(obj));

    // Tag 2: ObjectLibrary[] — verbatim round-trip
    for (const auto& lib : scene.object_libraries)
        w.write_bytes_field(2, lib);

    // Tag 3: Bounds[] — verbatim round-trip
    for (const auto& b : scene.bounds)
        w.write_bytes_field(3, b);

    // Tag 4: Group[] — verbatim round-trip
    for (const auto& grp : scene.groups)
        w.write_bytes_field(4, grp);

    // Tag 5: OnLoad[] — verbatim round-trip
    for (const auto& scr : scene.onload_scripts)
        w.write_bytes_field(5, scr);

    // Unknown tags — verbatim round-trip for forward compatibility
    for (const auto& f : scene.other_fields)
        w.write_field(f);

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[scene_loader] error: cannot write " << path << "\n";
        return;
    }
    const std::string data = w.to_string();
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();

    std::cout << "[scene_loader] saved " << fs::path(path).filename().string()
              << " (" << data.size() << " bytes, " << scene.objects.size() << " objects)\n";
}

} // namespace av
