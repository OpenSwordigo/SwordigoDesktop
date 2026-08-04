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
#include "scene_schemas.h"
#include "platform/protobuf_reader.h"

#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <system_error>
#include <unordered_set>

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

static std::string first_printable_len_field(const std::string& raw, int field_number) {
    try {
        proto::Reader reader(raw);
        proto::Field field;
        while (reader.read_field(field)) {
            if (field.field_number == field_number && field.wire_type == proto::WIRE_LEN &&
                is_printable(field.bytes_val))
                return field.bytes_val;
        }
    } catch (...) {}
    return {};
}

static int component_payload_field(const SceneComponent& component) {
    if (component.payload_field >= 50) return component.payload_field;
    // main.js::Ot dispatches the first nested message at field >= 50. This is
    // authoritative even when ClassName uses the short form ("Model") while
    // generated schemas use "ModelComponent".
    try {
        proto::Reader reader(component.raw_data);
        proto::Field field;
        while (reader.read_field(field)) {
            if (field.field_number >= 50 && field.wire_type == proto::WIRE_LEN)
                return static_cast<int>(field.field_number);
        }
    } catch (...) {}

    const auto schema = g_schemas.find("Component");
    if (schema == g_schemas.end()) return 0;
    for (const auto& entry : schema->second.fields) {
        if (entry.second.class_name == component.type_name || entry.second.name == component.type_name)
            return static_cast<int>(entry.first >> 3);
    }
    return 0;
}

static std::string component_schema_name(const SceneComponent& component) {
    const int payload = component_payload_field(component);
    const auto schema = g_schemas.find("Component");
    if (schema != g_schemas.end() && payload > 0) {
        const auto field = schema->second.fields.find((static_cast<uint32_t>(payload) << 3) | proto::WIRE_LEN);
        if (field != schema->second.fields.end()) return field->second.class_name;
    }
    return component.type_name;
}

static std::string component_string_field(const SceneComponent& component, int field_number) {
    const int payload_field = component_payload_field(component);
    if (payload_field == 0) return {};
    try {
        proto::Reader wrapper(component.raw_data);
        proto::Field field;
        while (wrapper.read_field(field)) {
            if (field.field_number != payload_field || field.wire_type != proto::WIRE_LEN)
                continue;
            return first_printable_len_field(field.bytes_val, field_number);
        }
    } catch (...) {}
    return {};
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
                    if (f.field_number >= 50 && f.wire_type == proto::WIRE_LEN && comp.payload_field == 0)
                        comp.payload_field = static_cast<int>(f.field_number);
                    break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[scene_loader] warning: component parse error: " << e.what() << "\n";
    }
    return comp;
}

// ============================================================
// GroundMesh parsing
// ============================================================

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

    int num_vertices = 0;
    std::string texture_name, vertex_data, index_data;

    while (reader.read_field(f)) {
        if      (f.field_number == 1  && f.wire_type == proto::WIRE_VARINT) { num_vertices = static_cast<int>(f.varint_val); }
        else if (f.field_number == 10 && f.wire_type == proto::WIRE_LEN)    { texture_name   = parse_mesh_material(f.bytes_val); }
        else if (f.field_number == 50 && f.wire_type == proto::WIRE_LEN)    { vertex_data    = f.bytes_val; }
        else if (f.field_number == 51 && f.wire_type == proto::WIRE_LEN)    { index_data     = f.bytes_val; }
    }

    // main.js::addGroundMesh ignores MeshData metadata and treats field 50 as
    // a packed position3/normal3/uv2 stream. Follow it exactly: metadata in
    // shipped scenes is often stale and was producing exploded geometry.
    if (vertex_data.empty()) return;
    if (num_vertices <= 0) num_vertices = static_cast<int>(vertex_data.size() / 32);
    if (num_vertices <= 0 || static_cast<size_t>(num_vertices) * 32 > vertex_data.size()) return;

    PODMesh pm;
    pm.num_vertices = num_vertices;
    pm.positions.resize(num_vertices * 3, 0.0f);
    pm.normals.resize(num_vertices * 3, 0.0f);
    pm.uvs.resize(num_vertices * 2, 0.0f);
    for (int i = 0; i < num_vertices; ++i) {
        const char* ptr = vertex_data.data() + static_cast<size_t>(i) * 32;
        std::memcpy(&pm.positions[i*3], ptr, 12);
        std::memcpy(&pm.normals[i*3], ptr + 12, 12);
        std::memcpy(&pm.uvs[i*2], ptr + 24, 8);
    }

    // main.js always creates Uint16Array from field 51 for GroundMesh.
    const int index_count = static_cast<int>(index_data.size() / 2);
    if (index_count > 0) {
        pm.indices.resize(index_count);
        for (int i = 0; i < index_count; ++i) {
            uint16_t index;
            std::memcpy(&index, index_data.data() + static_cast<size_t>(i) * 2, 2);
            pm.indices[i] = index;
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

static void parse_ground_mesh_component(SceneObject& obj, const SceneComponent& component) {
    const int payload_field = component_payload_field(component);
    if (payload_field == 0) return;
    proto::Reader reader(component.raw_data);
    proto::Field f;
    while (reader.read_field(f)) {
        if (f.field_number == payload_field && f.wire_type == proto::WIRE_LEN) {
            proto::Reader gm_reader(f.bytes_val);
            proto::Field gm_f;
            std::vector<std::string> front_meshes;
            std::vector<std::string> surface_meshes;
            std::vector<std::string> base_meshes;
            while (gm_reader.read_field(gm_f)) {
                if (gm_f.wire_type == proto::WIRE_LEN) {
                    // main.js order: FrontMesh(8), SurfaceMesh(9), Mesh(6).
                    if (gm_f.field_number == 8) front_meshes.push_back(gm_f.bytes_val);
                    else if (gm_f.field_number == 9) surface_meshes.push_back(gm_f.bytes_val);
                    else if (gm_f.field_number == 6) base_meshes.push_back(gm_f.bytes_val);
                }
            }
            for (const auto& mesh : front_meshes) parse_single_mesh(obj, mesh);
            for (const auto& mesh : surface_meshes) parse_single_mesh(obj, mesh);
            for (const auto& mesh : base_meshes) parse_single_mesh(obj, mesh);
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

    obj.resolved_components = obj.components;
    return obj;
}

static void resolve_object_render_data(SceneObject& obj) {
    obj.mesh_name.clear();
    obj.texture_name.clear();
    obj.background_name.clear();
    obj.ground_meshes.clear();
    obj.ground_mesh_textures.clear();
    const auto& components = obj.resolved_components.empty() ? obj.components : obj.resolved_components;
    for (const auto& comp : components) {
        const std::string schema_name = component_schema_name(comp);
        if (schema_name == "GroundMeshComponent")
            parse_ground_mesh_component(obj, comp);

        if (comp.type_name == "MeshRenderer" || comp.type_name == "SkinnedMeshRenderer")
            scan_for_asset_refs(comp.raw_data, obj.mesh_name, obj.texture_name);

        // Ruby's schema stores ModelComponent.Name as field 1 without a .pod
        // suffix. Resolve this form as well as the older filename form.
        if (schema_name == "ModelComponent" && obj.mesh_name.empty())
            obj.mesh_name = component_string_field(comp, 1);

        if (comp.type_name == "Background") {
            std::string dummy_tex;
            scan_for_asset_refs(comp.raw_data, obj.background_name, dummy_tex);
            if (obj.texture_name.empty() && !dummy_tex.empty())
                obj.texture_name = dummy_tex;
        }

        if (obj.mesh_name.empty())
            scan_for_asset_refs(comp.raw_data, obj.mesh_name, obj.texture_name);
    }

}

struct SceneTemplate {
    std::string name;
    float scaling = 1.0f;
    std::vector<SceneComponent> components;
};

static std::vector<SceneTemplate> parse_object_library(const std::string& bytes) {
    std::vector<SceneTemplate> templates;
    try {
        proto::Reader library(bytes);
        proto::Field field;
        while (library.read_field(field)) {
            if (field.field_number != 2 || field.wire_type != proto::WIRE_LEN) continue;
            SceneTemplate item;
            proto::Reader object_template(field.bytes_val);
            proto::Field template_field;
            while (object_template.read_field(template_field)) {
                if (template_field.field_number == 1 && template_field.wire_type == proto::WIRE_LEN) {
                    SceneObject object = parse_object(template_field.bytes_val);
                    item.name = object.name;
                    item.components = std::move(object.components);
                } else if (template_field.field_number == 2 && template_field.wire_type == proto::WIRE_I32) {
                    item.scaling = template_field.float_val;
                }
            }
            if (!item.name.empty()) templates.push_back(std::move(item));
        }
    } catch (...) {}
    return templates;
}

static void resolve_scene_templates(SceneData& scene) {
    std::unordered_map<std::string, SceneTemplate> templates;
    for (const auto& library : scene.object_libraries) {
        for (auto& item : parse_object_library(library))
            templates[item.name] = std::move(item);
    }

    for (auto& object : scene.objects) {
        object.resolved_components = object.components;
        const auto item = templates.find(object.template_name);
        if (item != templates.end()) {
            if (object.components.empty()) {
                object.resolved_components = item->second.components;
            } else {
                std::unordered_set<int> overridden;
                for (const auto& component : object.components) overridden.insert(component.type_id);
                std::vector<SceneComponent> merged;
                for (const auto& component : item->second.components) {
                    if (overridden.find(component.type_id) == overridden.end()) merged.push_back(component);
                }
                merged.insert(merged.end(), object.components.begin(), object.components.end());
                object.resolved_components = std::move(merged);
            }
            object.template_scaling = item->second.scaling;
        }
        resolve_object_render_data(object);
    }
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
    if (scene.objects.empty()) {
        std::fill(std::begin(scene.bounds_min), std::end(scene.bounds_min), 0.0f);
        std::fill(std::begin(scene.bounds_max), std::end(scene.bounds_max), 0.0f);
        return;
    }

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
                min_x = std::min(min_x, obj.pos_x + gm.min_x * obj.scale_x * obj.template_scaling);
                min_y = std::min(min_y, obj.pos_y + gm.min_y * obj.scale_y * obj.template_scaling);
                min_z = std::min(min_z, obj.pos_z + gm.min_z * obj.scale_z * obj.template_scaling);
                max_x = std::max(max_x, obj.pos_x + gm.max_x * obj.scale_x * obj.template_scaling);
                max_y = std::max(max_y, obj.pos_y + gm.max_y * obj.scale_y * obj.template_scaling);
                max_z = std::max(max_z, obj.pos_z + gm.max_z * obj.scale_z * obj.template_scaling);
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

void scene_refresh(SceneData& scene) {
    resolve_scene_templates(scene);
    scene.object_count = static_cast<int>(scene.objects.size());
    compute_bounds(scene);
}

std::string scene_fresh_identifier(const SceneData& scene) {
    std::unordered_set<std::string> names;
    names.reserve(scene.objects.size());
    for (const auto& object : scene.objects)
        names.insert(object.name);

    for (size_t suffix = 1;; ++suffix) {
        const std::string candidate = "obj" + std::to_string(suffix);
        if (names.find(candidate) == names.end())
            return candidate;
    }
}

size_t scene_create_object(SceneData& scene, const std::string& template_name) {
    SceneObject object;
    object.template_name = template_name;
    object.name = scene_fresh_identifier(scene);
    scene.objects.push_back(std::move(object));
    scene_refresh(scene);
    return scene.objects.size() - 1;
}

bool scene_duplicate_object(SceneData& scene, size_t index, size_t* new_index) {
    if (index >= scene.objects.size())
        return false;

    SceneObject duplicate = scene.objects[index];
    duplicate.name = scene_fresh_identifier(scene);
    const auto insertion = scene.objects.begin() + static_cast<std::ptrdiff_t>(index + 1);
    scene.objects.insert(insertion, std::move(duplicate));
    scene_refresh(scene);
    if (new_index)
        *new_index = index + 1;
    return true;
}

size_t scene_paste_object(SceneData& scene, const SceneObject& object) {
    SceneObject pasted = object;
    pasted.name = scene_fresh_identifier(scene);
    scene.objects.push_back(std::move(pasted));
    scene_refresh(scene);
    return scene.objects.size() - 1;
}

bool scene_delete_object(SceneData& scene, size_t index) {
    if (index >= scene.objects.size())
        return false;
    scene.objects.erase(scene.objects.begin() + static_cast<std::ptrdiff_t>(index));
    scene_refresh(scene);
    return true;
}

bool scene_move_object(SceneData& scene, size_t from_index, size_t to_index) {
    if (from_index >= scene.objects.size() || to_index >= scene.objects.size())
        return false;
    if (from_index == to_index)
        return true;

    SceneObject object = std::move(scene.objects[from_index]);
    scene.objects.erase(scene.objects.begin() + static_cast<std::ptrdiff_t>(from_index));
    scene.objects.insert(scene.objects.begin() + static_cast<std::ptrdiff_t>(to_index),
                         std::move(object));
    scene_refresh(scene);
    return true;
}

std::vector<std::string> scene_component_types() {
    std::vector<std::string> types;
    const auto component_schema = g_schemas.find("Component");
    if (component_schema == g_schemas.end()) return types;
    for (const auto& entry : component_schema->second.fields) {
        if (entry.second.is_message && !entry.second.class_name.empty())
            types.push_back(entry.second.class_name);
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

bool scene_add_component(SceneData& scene, size_t object_index,
                         const std::string& type_name, size_t* new_index) {
    if (object_index >= scene.objects.size()) return false;
    SceneObject& object = scene.objects[object_index];

    const auto component_schema = g_schemas.find("Component");
    if (component_schema == g_schemas.end()) return false;
    int payload_field = 0;
    for (const auto& entry : component_schema->second.fields) {
        if (entry.second.is_message && entry.second.class_name == type_name) {
            payload_field = static_cast<int>(entry.first >> 3);
            break;
        }
    }
    if (payload_field == 0) return false;

    int instance_id = 1;
    for (const auto& component : object.components)
        instance_id = std::max(instance_id, component.type_id + 1);

    proto::Writer wrapper;
    wrapper.write_string_field(1, type_name);
    wrapper.write_varint_field(2, static_cast<uint64_t>(instance_id));
    proto::Writer payload;
    wrapper.write_nested_field(payload_field, payload);

    SceneComponent component;
    component.type_name = type_name;
    component.type_id = instance_id;
    component.raw_data = wrapper.to_string();
    object.components.push_back(std::move(component));
    if (new_index) *new_index = object.components.size() - 1;
    return true;
}

bool scene_remove_component(SceneData& scene, size_t object_index, size_t component_index) {
    if (object_index >= scene.objects.size()) return false;
    auto& components = scene.objects[object_index].components;
    if (component_index >= components.size()) return false;
    components.erase(components.begin() + static_cast<std::ptrdiff_t>(component_index));
    return true;
}

bool scene_paste_component(SceneData& scene, size_t object_index,
                           const SceneComponent& source, size_t* new_index) {
    if (object_index >= scene.objects.size()) return false;
    auto& components = scene.objects[object_index].components;
    int instance_id = 1;
    for (const auto& component : components)
        instance_id = std::max(instance_id, component.type_id + 1);

    SceneComponent pasted = source;
    try {
        proto::Reader reader(pasted.raw_data);
        std::vector<proto::Field> fields = reader.read_all();
        for (auto& field : fields) {
            if (field.field_number == 2 && field.wire_type == proto::WIRE_VARINT) {
                field.varint_val = static_cast<uint64_t>(instance_id);
                break;
            }
        }
        proto::Writer writer;
        for (const auto& field : fields) writer.write_field(field);
        pasted.raw_data = writer.to_string();
    } catch (...) {
        return false;
    }
    pasted.type_id = instance_id;
    components.push_back(std::move(pasted));
    if (new_index) *new_index = components.size() - 1;
    return true;
}

std::vector<SceneComponentField> scene_component_fields(const SceneComponent& component) {
    std::vector<SceneComponentField> result;
    const int payload_field = component_payload_field(component);
    if (payload_field == 0) return result;

    const auto schema_it = g_schemas.find(component_schema_name(component));
    std::unordered_map<uint32_t, size_t> occurrences;
    try {
        proto::Reader wrapper(component.raw_data);
        proto::Field wrapper_field;
        while (wrapper.read_field(wrapper_field)) {
            if (wrapper_field.field_number != static_cast<uint32_t>(payload_field) ||
                wrapper_field.wire_type != proto::WIRE_LEN)
                continue;
            proto::Reader payload(wrapper_field.bytes_val);
            proto::Field field;
            while (payload.read_field(field)) {
                SceneComponentField value;
                value.field_number = field.field_number;
                value.wire_type = field.wire_type;
                value.occurrence = occurrences[field.field_number]++;
                value.varint_value = field.varint_val;
                value.double_value = field.double_val;
                value.float_value = field.float_val;
                value.bytes_value = field.bytes_val;
                if (schema_it != g_schemas.end()) {
                    const uint32_t wire_key = (field.field_number << 3) | field.wire_type;
                    const auto schema_field = schema_it->second.fields.find(wire_key);
                    if (schema_field != schema_it->second.fields.end()) {
                        value.name = schema_field->second.name;
                        value.class_name = schema_field->second.class_name;
                        value.is_message = schema_field->second.is_message;
                    }
                }
                if (value.name.empty()) value.name = "Field " + std::to_string(field.field_number);
                result.push_back(std::move(value));
            }
            break;
        }
    } catch (...) {}
    return result;
}

bool scene_set_component_field(SceneComponent& component, const SceneComponentField& value) {
    const int payload_field = component_payload_field(component);
    if (payload_field == 0 || value.is_message) return false;
    try {
        proto::Reader wrapper_reader(component.raw_data);
        std::vector<proto::Field> wrapper_fields = wrapper_reader.read_all();
        bool changed = false;
        for (auto& wrapper_field : wrapper_fields) {
            if (wrapper_field.field_number != static_cast<uint32_t>(payload_field) ||
                wrapper_field.wire_type != proto::WIRE_LEN)
                continue;
            proto::Reader payload_reader(wrapper_field.bytes_val);
            std::vector<proto::Field> payload_fields = payload_reader.read_all();
            size_t occurrence = 0;
            for (auto& field : payload_fields) {
                if (field.field_number != value.field_number) continue;
                if (occurrence++ != value.occurrence) continue;
                if (field.wire_type != value.wire_type) return false;
                field.varint_val = value.varint_value;
                field.double_val = value.double_value;
                field.float_val = value.float_value;
                field.bytes_val = value.bytes_value;
                changed = true;
                break;
            }
            if (!changed) return false;
            proto::Writer payload_writer;
            for (const auto& field : payload_fields) payload_writer.write_field(field);
            wrapper_field.bytes_val = payload_writer.to_string();
            break;
        }
        if (!changed) return false;
        proto::Writer wrapper_writer;
        for (const auto& field : wrapper_fields) wrapper_writer.write_field(field);
        component.raw_data = wrapper_writer.to_string();
        return true;
    } catch (...) {
        return false;
    }
}

std::string scene_program_source(const std::string& program_data) {
    try {
        proto::Reader reader(program_data);
        proto::Field field;
        while (reader.read_field(field)) {
            if (field.field_number == 1 && field.wire_type == proto::WIRE_LEN)
                return field.bytes_val;
        }
    } catch (...) {}
    return {};
}

bool scene_set_program_source(std::string& program_data, const std::string& source) {
    try {
        std::vector<proto::Field> fields;
        if (!program_data.empty()) {
            proto::Reader reader(program_data);
            fields = reader.read_all();
        }
        bool found = false;
        for (auto& field : fields) {
            if (field.field_number == 1 && field.wire_type == proto::WIRE_LEN) {
                field.bytes_val = source;
                found = true;
                break;
            }
        }
        proto::Writer writer;
        if (!found && !source.empty()) writer.write_string_field(1, source);
        for (const auto& field : fields) writer.write_field(field);
        program_data = writer.to_string();
        return true;
    } catch (...) {
        return false;
    }
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

    scene_refresh(scene);

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
std::string scene_serialize(const SceneData& scene) {
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

    return w.to_string();
}

bool scene_save(const std::string& path, const SceneData& scene, std::string* error_message) {
    const std::string data = scene_serialize(scene);

    const fs::path destination(path);
    const fs::path temporary = destination.string() + ".ruby.tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        const std::string error = "cannot write temporary file " + temporary.string();
        if (error_message) *error_message = error;
        std::cerr << "[scene_loader] error: " << error << "\n";
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        const std::string error = "failed while writing temporary file " + temporary.string();
        if (error_message) *error_message = error;
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        std::cerr << "[scene_loader] error: " << error << "\n";
        return false;
    }

    std::error_code rename_error;
    fs::rename(temporary, destination, rename_error);
    if (rename_error) {
        const std::string error = "cannot replace " + destination.string() + ": " + rename_error.message();
        if (error_message) *error_message = error;
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        std::cerr << "[scene_loader] error: " << error << "\n";
        return false;
    }

    std::cout << "[scene_loader] saved " << fs::path(path).filename().string()
              << " (" << data.size() << " bytes, " << scene.objects.size() << " objects)\n";
    return true;
}

} // namespace av
