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

static void parse_scene_waters(SceneData& scene);

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

static void parse_single_mesh(SceneObject& obj, const std::string& bytes, int src_field) {
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

    // Remember the original serialized MeshData so a dirty mesh can be
    // re-emitted with preserved sub-fields (material, etc.).  Stored after the
    // validation checks below so the parallel vectors stay in sync with the
    // parsed ground_meshes list.

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
    obj.ground_mesh_raw.push_back(std::move(bytes));
    obj.ground_mesh_fields.push_back(src_field);
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
            for (const auto& mesh : front_meshes) parse_single_mesh(obj, mesh, 8);
            for (const auto& mesh : surface_meshes) parse_single_mesh(obj, mesh, 9);
            for (const auto& mesh : base_meshes) parse_single_mesh(obj, mesh, 6);
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
    obj.is_spawn_point = false;
    // When the object's ground meshes were edited in the SDK, keep the parsed
    // (modified) mesh data instead of re-deriving it from the original raw
    // component bytes — scene_refresh() is called during interactive edits.
    const bool preserve_meshes = obj.ground_meshes_dirty && !obj.ground_meshes.empty();
    if (!preserve_meshes) {
        obj.ground_meshes.clear();
        obj.ground_mesh_textures.clear();
        obj.ground_mesh_raw.clear();
        obj.ground_mesh_fields.clear();
    }
    const auto& components = obj.resolved_components.empty() ? obj.components : obj.resolved_components;
    for (const auto& comp : components) {
        const std::string schema_name = component_schema_name(comp);
        if (schema_name == "GroundMeshComponent" && !preserve_meshes)
            parse_ground_mesh_component(obj, comp);

        if (comp.type_name == "SpawnPoint")
            obj.is_spawn_point = true;

        if (comp.type_name == "MeshRenderer" || comp.type_name == "SkinnedMeshRenderer")
            scan_for_asset_refs(comp.raw_data, obj.mesh_name, obj.texture_name);

        // Ruby's schema stores ModelComponent.Name as field 1 without a .pod
        // suffix. Resolve this form as well as the older filename form.
        if (schema_name == "ModelComponent" && obj.mesh_name.empty())
            obj.mesh_name = component_string_field(comp, 1);

        if (comp.type_name == "Background") {
            // BackgroundComponent stores TextureName in payload field 1 as a
            // bare stem without extension (e.g. "graveyardback", matching
            // graveyardback_2x.tex.png). scan_for_asset_refs only catches
            // extensioned names, so read the schema field first.
            obj.background_name = component_string_field(comp, 1);
            if (obj.background_name.empty()) {
                std::string dummy_tex;
                scan_for_asset_refs(comp.raw_data, obj.background_name, dummy_tex);
            }
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

// ObjectLibrary.ImportedLibrary (tag 3, repeated string): names of external
// .scl files whose templates must be merged for object template resolution.
static std::vector<std::string> parse_imported_library_names(const std::string& bytes) {
    std::vector<std::string> names;
    try {
        proto::Reader library(bytes);
        proto::Field field;
        while (library.read_field(field)) {
            if (field.field_number == 3 && field.wire_type == proto::WIRE_LEN)
                names.emplace_back(field.bytes_val.data(), field.bytes_val.size());
        }
    } catch (...) {}
    return names;
}

// Resolve ImportedLibrary references into external_libraries by loading each
// <name>.scl from the scene directory / resources tree. Cached per load call;
// called only from scene_load so interactive scene_refresh stays cheap.
static void load_external_libraries(SceneData& scene) {
    if (scene.filepath.empty()) return;
    const fs::path scene_dir = fs::path(scene.filepath).parent_path();
    const char* home = getenv("HOME");
    const fs::path data_res = home
        ? fs::path(home) / ".local/share/swordigo-desktop/assets/resources"
        : fs::path();
    const fs::path local_res = fs::path("assets") / "resources";

    std::vector<fs::path> roots = {
        scene_dir,
        scene_dir / "resources",
        scene_dir.parent_path(),
        scene_dir.parent_path() / "resources",
        data_res,
        local_res
    };

    std::vector<std::string> wanted;
    for (const auto& library : scene.object_libraries) {
        for (auto& name : parse_imported_library_names(library))
            if (!name.empty()) wanted.push_back(std::move(name));
    }

    for (const auto& name : wanted) {
        const std::string filename = name + ".scl";
        fs::path found;
        for (const auto& root : roots) {
            std::error_code ec;
            fs::path candidate = root / filename;
            if (fs::is_regular_file(candidate, ec)) { found = candidate; break; }
        }
        if (found.empty()) {
            scene.missing_libraries.push_back(name);
            std::cerr << "[scene_loader] warning: imported library '" << name
                      << "' not found (" << filename << ")\n";
            continue;
        }
        std::ifstream in(found, std::ios::binary);
        if (!in) continue;
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) scene.external_libraries.push_back(std::move(bytes));
    }
}

static void resolve_scene_templates(SceneData& scene) {
    std::unordered_map<std::string, SceneTemplate> templates;
    for (const auto& library : scene.object_libraries) {
        for (auto& item : parse_object_library(library))
            templates[item.name] = std::move(item);
    }
    // Merge templates from external .scl libraries (ImportedLibrary refs).
    for (const auto& library : scene.external_libraries) {
        for (auto& item : parse_object_library(library)) {
            if (templates.find(item.name) == templates.end())
                templates[item.name] = std::move(item);
        }
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
// GroundMesh re-encoding (vertex editing round-trip)
//
// GroundMesh component payload field number (Component schema: 890 >> 3).
// ============================================================
static constexpr uint32_t kGroundMeshPayload = 111;

// Re-encode one MeshData message from the parsed PODMesh vertex/index data.
// Field 1 (num_vertices), field 50 (interleaved pos/normal/uv stream) and
// field 51 (uint16 index stream) are rewritten; every other sub-field
// (material/texture mapping, metadata) is preserved verbatim from the
// original serialized message.
static std::string reencode_mesh_data(const std::string& original, const PODMesh& pm) {
    // Interleaved stream: pos(3) + normal(3) + uv(2) = 32 bytes per vertex.
    std::string vertex_data;
    vertex_data.reserve(static_cast<size_t>(pm.num_vertices) * 32);
    static const float kUp[3]    = {0.0f, 1.0f, 0.0f};
    static const float kZero[2]  = {0.0f, 0.0f};
    for (int i = 0; i < pm.num_vertices; ++i) {
        const float* p = &pm.positions[static_cast<size_t>(i) * 3];
        const bool has_n = static_cast<size_t>(i) * 3 + 2 < pm.normals.size();
        const bool has_t = static_cast<size_t>(i) * 2 + 1 < pm.uvs.size();
        const float* n = has_n ? &pm.normals[static_cast<size_t>(i) * 3] : kUp;
        const float* t = has_t ? &pm.uvs[static_cast<size_t>(i) * 2] : kZero;
        vertex_data.append(reinterpret_cast<const char*>(p), 12);
        vertex_data.append(reinterpret_cast<const char*>(n), 12);
        vertex_data.append(reinterpret_cast<const char*>(t), 8);
    }

    std::string index_data;
    index_data.reserve(pm.indices.size() * 2);
    for (uint32_t idx : pm.indices) {
        uint16_t v = static_cast<uint16_t>(idx);
        index_data.append(reinterpret_cast<const char*>(&v), 2);
    }

    proto::Writer w;
    bool wrote_vertices = false;
    bool wrote_indices  = false;
    bool wrote_count    = false;
    try {
        proto::Reader reader(original);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_VARINT) {
                w.write_varint_field(1, static_cast<uint64_t>(pm.num_vertices));
                wrote_count = true;
            } else if (f.field_number == 50 && f.wire_type == proto::WIRE_LEN) {
                w.write_bytes_field(50, vertex_data);
                wrote_vertices = true;
            } else if (f.field_number == 51 && f.wire_type == proto::WIRE_LEN) {
                w.write_bytes_field(51, index_data);
                wrote_indices = true;
            } else {
                w.write_field(f);
            }
        }
    } catch (...) {
        std::cerr << "[scene_loader] warning: cannot re-encode dirty GroundMesh "
                     "(malformed source); edit will not persist\n";
        return original; // malformed original — refuse to touch it
    }
    // Append anything the original lacked (protobuf ordering is insignificant).
    if (!wrote_count)    w.write_varint_field(1, static_cast<uint64_t>(pm.num_vertices));
    if (!wrote_vertices) w.write_bytes_field(50, vertex_data);
    if (!wrote_indices)  w.write_bytes_field(51, index_data);
    return w.to_string();
}

// Rebuild the GroundMeshComponent payload message of a dirty object from its
// parsed ground_meshes list, preserving all other component sub-fields and
// matching each serialized child (fields 6/8/9) to the correct parsed mesh.
static std::string rebuild_ground_mesh_component(const SceneObject& obj,
                                                 const SceneComponent& component) {
    const int payload = component_payload_field(component);
    if (payload == 0) return component.raw_data;

    // Per-child-field cursors into the parsed mesh list (parse order: front 8,
    // surface 9, mesh 6 — but children may be interleaved in the wire data).
    std::vector<size_t> field_cursor(64, 0);
    std::vector<std::vector<size_t>> field_to_meshes(64);
    for (size_t i = 0; i < obj.ground_meshes.size(); ++i) {
        const int fld = (i < obj.ground_mesh_fields.size()) ? obj.ground_mesh_fields[i] : 0;
        if (fld >= 0 && fld < 64) field_to_meshes[fld].push_back(i);
    }

    proto::Writer w;
    try {
        proto::Reader reader(component.raw_data);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == static_cast<uint32_t>(payload) && f.wire_type == proto::WIRE_LEN) {
                proto::Reader gm(f.bytes_val);
                proto::Field g;
                proto::Writer gw;
                while (gm.read_field(g)) {
                    const bool is_mesh_child = (g.field_number == 6 || g.field_number == 8 ||
                                                g.field_number == 9) && g.wire_type == proto::WIRE_LEN;
                    if (!is_mesh_child) {
                        gw.write_field(g);
                        continue;
                    }
                    const size_t fld = g.field_number;
                    size_t pos = field_cursor[fld]++;
                    if (pos < field_to_meshes[fld].size()) {
                        const size_t mi = field_to_meshes[fld][pos];
                        const std::string orig = (mi < obj.ground_mesh_raw.size())
                            ? obj.ground_mesh_raw[mi] : g.bytes_val;
                        gw.write_bytes_field(g.field_number,
                                             reencode_mesh_data(orig, obj.ground_meshes[mi]));
                    } else {
                        gw.write_field(g);
                    }
                }
                w.write_bytes_field(payload, gw.to_string());
            } else {
                w.write_field(f);
            }
        }
    } catch (...) {
        return component.raw_data;
    }
    return w.to_string();
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

    // Tag 3: Component[] — write raw bytes verbatim (preserves all sub-fields).
    // Dirty GroundMesh components are re-encoded from the edited mesh data.
    for (const auto& comp : obj.components) {
        std::string data = comp.raw_data;
        if (obj.ground_meshes_dirty && !obj.ground_meshes.empty() &&
            component_payload_field(comp) == static_cast<int>(kGroundMeshPayload)) {
            data = rebuild_ground_mesh_component(obj, comp);
        }
        w.write_bytes_field(3, data);
    }

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
    // Re-derive fluid sheets so SceneWater::object_index stays valid across
    // object add/delete/move edits (parse_scene_waters is cheap — it only
    // scans for WaterMesh components).
    parse_scene_waters(scene);
}

void scene_mark_ground_mesh_dirty(SceneData& scene, size_t object_index) {
    if (object_index >= scene.objects.size()) return;
    scene.objects[object_index].ground_meshes_dirty = true;
}

// Rewrite the Material (field 10) sub-message of a serialized MeshData so its
// texture reference (Material.field5 -> Texture.field1) becomes @p name.
// Every other sub-field is preserved verbatim.
static std::string rewrite_mesh_material(const std::string& original,
                                         const std::string& texture_name) {
    // Build the new Texture message: field 1 = resource string.
    proto::Writer tex;
    tex.write_string_field(1, texture_name);
    // Build the new Material message: field 5 = Texture (LEN).
    proto::Writer mat;
    mat.write_bytes_field(5, tex.to_string());

    proto::Writer w;
    try {
        proto::Reader reader(original);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 10 && f.wire_type == proto::WIRE_LEN) {
                w.write_bytes_field(10, mat.to_string());
            } else {
                w.write_field(f);
            }
        }
    } catch (...) {
        return original; // malformed — leave untouched
    }
    return w.to_string();
}

bool scene_set_ground_mesh_texture(SceneObject& obj, size_t mesh_index,
                                   const std::string& texture_name) {
    if (mesh_index >= obj.ground_meshes.size()) return false;
    if (mesh_index >= obj.ground_mesh_textures.size())
        obj.ground_mesh_textures.resize(obj.ground_meshes.size());
    obj.ground_mesh_textures[mesh_index] = texture_name;
    if (mesh_index < obj.ground_mesh_raw.size())
        obj.ground_mesh_raw[mesh_index] = rewrite_mesh_material(obj.ground_mesh_raw[mesh_index],
                                                               texture_name);
    obj.ground_meshes_dirty = true;
    return true;
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
// WaterMesh parsing
//
// WaterMeshComponent payload (114) -> { f1 BoundsShapeId (varint),
// f2 TextureMappingId (varint), f3 FrontColor (FloatColor RGBA),
// f4 SurfaceColor (FloatColor RGBA) }. The bounds shape is a component on
// the same object whose type_id == BoundsShapeId (CollisionShape /
// UtilityShape); its ShapeComponent payload (field 120) field 1 is the
// Rectangle (x, y, w, h) that defines the fluid sheet. The texture mapping
// (payload 113) field 1 names the texture (e.g. "water"), field 2 the tile
// size, field 3 the offset — mirroring main.js PS() / WaterMeshComponent.
// ============================================================

// Read a FloatColor message (fixed32 fields 1-4 = RGBA) from bytes.
static bool parse_float_color(const std::string& bytes, float out[4]) {
    if (bytes.empty()) return false;
    try {
        proto::Reader reader(bytes);
        proto::Field  f;
        int idx = 0;
        while (reader.read_field(f) && idx < 4) {
            if (f.wire_type == proto::WIRE_I32) out[idx++] = f.float_val;
        }
        return idx >= 4;
    } catch (...) {}
    return false;
}

// Extract the Rectangle { x, y, w, h } from a ShapeComponent payload
// (component with type_id == bounds_id). Returns false when missing.
static bool read_shape_rectangle(const SceneObject& obj, int bounds_id, float rect[4]) {
    const auto& comps = obj.resolved_components.empty() ? obj.components : obj.resolved_components;
    for (const auto& comp : comps) {
        if (comp.type_id != bounds_id) continue;
        const int payload = component_payload_field(comp);
        if (payload == 0) continue;
        try {
            proto::Reader wrapper(comp.raw_data);
            proto::Field field;
            while (wrapper.read_field(field)) {
                if (field.field_number != static_cast<uint32_t>(payload) ||
                    field.wire_type != proto::WIRE_LEN)
                    continue;
                proto::Reader shape(field.bytes_val);
                proto::Field sf;
                while (shape.read_field(sf)) {
                    // ShapeComponent field 1 = Rectangle { x, y, w, h }.
                    if (sf.field_number != 1 || sf.wire_type != proto::WIRE_LEN) continue;
                    proto::Reader rc(sf.bytes_val);
                    proto::Field rf;
                    int idx = 0;
                    while (rc.read_field(rf) && idx < 4) {
                        if (rf.wire_type == proto::WIRE_I32) rect[idx++] = rf.float_val;
                    }
                    return idx >= 4;
                }
            }
        } catch (...) {}
    }
    return false;
}

// Extract texture name / tile size / offset from a TextureMapping component
// whose type_id == tex_id. Payload 113: f1 texture name, f2 tile size,
// f3 offset { x, y }.
static void read_texture_mapping(const SceneObject& obj, int tex_id,
                                 std::string& out_name, float& out_tile,
                                 float out_offset[2]) {
    const auto& comps = obj.resolved_components.empty() ? obj.components : obj.resolved_components;
    for (const auto& comp : comps) {
        if (comp.type_id != tex_id) continue;
        const int payload = component_payload_field(comp);
        if (payload == 0) continue;
        try {
            proto::Reader wrapper(comp.raw_data);
            proto::Field field;
            while (wrapper.read_field(field)) {
                if (field.field_number != static_cast<uint32_t>(payload) ||
                    field.wire_type != proto::WIRE_LEN)
                    continue;
                proto::Reader tm(field.bytes_val);
                proto::Field tf;
                while (tm.read_field(tf)) {
                    if (tf.field_number == 1 && tf.wire_type == proto::WIRE_LEN &&
                        is_printable(tf.bytes_val)) {
                        out_name = tf.bytes_val;
                    } else if (tf.field_number == 2 && tf.wire_type == proto::WIRE_I32) {
                        out_tile = tf.float_val;
                    } else if (tf.field_number == 3 && tf.wire_type == proto::WIRE_LEN) {
                        proto::Reader ofs(tf.bytes_val);
                        proto::Field of;
                        int oi = 0;
                        while (ofs.read_field(of) && oi < 2) {
                            if (of.wire_type == proto::WIRE_I32) out_offset[oi++] = of.float_val;
                        }
                    }
                }
                return;
            }
        } catch (...) {}
    }
}

static void parse_scene_waters(SceneData& scene) {
    scene.waters.clear();
    for (size_t oi = 0; oi < scene.objects.size(); ++oi) {
        const auto& obj = scene.objects[oi];
        const auto& comps = obj.resolved_components.empty() ? obj.components : obj.resolved_components;
        for (const auto& comp : comps) {
            if (comp.type_name != "WaterMesh") continue;
            const int payload = component_payload_field(comp);
            if (payload == 0) continue;

            int bounds_id = 0, tex_id = 0;
            float front[4] = {0.5f, 0.7f, 1.0f, 0.7f};
            float surface[4] = {0.7f, 0.9f, 1.0f, 0.7f};
            try {
                proto::Reader wrapper(comp.raw_data);
                proto::Field field;
                while (wrapper.read_field(field)) {
                    if (field.field_number != static_cast<uint32_t>(payload) ||
                        field.wire_type != proto::WIRE_LEN)
                        continue;
                    proto::Reader wm(field.bytes_val);
                    proto::Field wf;
                    while (wm.read_field(wf)) {
                        if (wf.field_number == 1 && wf.wire_type == proto::WIRE_VARINT)
                            bounds_id = (int)wf.varint_val;
                        else if (wf.field_number == 2 && wf.wire_type == proto::WIRE_VARINT)
                            tex_id = (int)wf.varint_val;
                        else if (wf.field_number == 3 && wf.wire_type == proto::WIRE_LEN)
                            parse_float_color(wf.bytes_val, front);
                        else if (wf.field_number == 4 && wf.wire_type == proto::WIRE_LEN)
                            parse_float_color(wf.bytes_val, surface);
                    }
                    break;
                }
            } catch (...) {}
            if (bounds_id <= 0) continue;  // no shape -> cannot place the sheet

            SceneData::SceneWater water;
            water.object_index = (int)oi;
            if (!read_shape_rectangle(obj, bounds_id, water.rect)) continue;
            if (water.rect[2] <= 0.0f || water.rect[3] <= 0.0f) continue;
            memcpy(water.front_color, front, sizeof(front));
            memcpy(water.surface_color, surface, sizeof(surface));
            read_texture_mapping(obj, tex_id, water.texture, water.tile_size, water.tex_offset);
            if (water.tile_size <= 0.0f) water.tile_size = 64.0f;
            scene.waters.push_back(std::move(water));
        }
    }
    if (!scene.waters.empty())
        std::cout << "[scene_loader] " << scene.waters.size() << " water mesh(es)\n";
}

// ============================================================
// Public API — Load
// ============================================================
// Parse the raw SceneObjectGroup messages (top-level scene tag 4).
// Wire fields (from the Ruby reference editor schema):
//   tag  1 (LEN)    : Identifier       (group name)
//   tag  2 (LEN)    : ObjectIdentifier (member object name, repeated)
//   tag  3 (VARINT) : Hidden
//   tag  3 (LEN)    : OnLoad (Program, preserved raw)
//   tag 28 (VARINT) : CanBecomeActive
//   tag 30 (VARINT) : Locked
std::vector<SceneGroup> parse_scene_groups(const std::vector<std::string>& raw_groups) {
    std::vector<SceneGroup> out;
    out.reserve(raw_groups.size());
    for (const auto& raw : raw_groups) {
        SceneGroup g;
        g.raw = raw;
        try {
            proto::Reader r(raw);
            proto::Field f;
            while (r.read_field(f)) {
                switch (f.field_number) {
                    case 1:
                        if (f.wire_type == proto::WIRE_LEN)
                            g.name.assign(f.bytes_val.data(), f.bytes_val.size());
                        break;
                    case 2:
                        if (f.wire_type == proto::WIRE_LEN)
                            g.members.emplace_back(f.bytes_val.data(), f.bytes_val.size());
                        break;
                    case 3:
                        if (f.wire_type == proto::WIRE_VARINT)
                            g.hidden = (f.varint_val != 0);
                        break;
                    case 30:
                        if (f.wire_type == proto::WIRE_VARINT)
                            g.locked = (f.varint_val != 0);
                        break;
                    default:
                        break; // Preserved verbatim in g.raw.
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[scene_loader] warning: group parse failed: " << e.what() << "\n";
        }
        if (g.name.empty()) g.name = "<unnamed group>";
        out.push_back(std::move(g));
    }
    return out;
}

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

    // Resolve ImportedLibrary .scl references before template resolution.
    // scene_refresh also derives render-time fluids (WaterMesh components)
    // via parse_scene_waters, and re-runs it on every later object edit so
    // SceneWater::object_index stays in sync.
    load_external_libraries(scene);
    scene.parsed_groups = parse_scene_groups(scene.groups);
    scene_refresh(scene);

    // Collect render-time lights from Light components (never serialized).
    // LightComponent payload (verified against OpenSwordigo arm32): embedded
    // field 130 -> { f1 Type, f2 Intensity, f3 Color, f6 Offset, f7 Radius }.
    scene.lights.clear();
    for (const auto& obj : scene.objects) {
        for (const auto& comp : obj.components) {
            if (comp.type_name != "Light") continue;
            SceneData::SceneLight light;
            light.pos[0] = obj.pos_x; light.pos[1] = obj.pos_y; light.pos[2] = obj.pos_z;
            try {
                proto::Reader reader(comp.raw_data);
                proto::Field  f;
                while (reader.read_field(f)) {
                    if (f.field_number != 130 || f.wire_type != proto::WIRE_LEN) continue;
                    proto::Reader data(f.bytes_val);
                    proto::Field  d;
                    while (data.read_field(d)) {
                        switch (d.field_number) {
                            case 1: light.type = (int)d.as_int(); break;
                            case 2: light.intensity = d.as_float(); break;
                            case 3: {
                                proto::Reader col(d.bytes_val);
                                proto::Field cf;
                                int ci = 0;
                                while (col.read_field(cf) && ci < 3) {
                                    if (cf.wire_type == proto::WIRE_I32)
                                        light.color[ci++] = cf.as_float();
                                    else if (cf.wire_type == proto::WIRE_I64)
                                        light.color[ci++] = (float)cf.as_double();
                                }
                                break;
                            }
                            case 6: {
                                proto::Reader ofs(d.bytes_val);
                                proto::Field of;
                                float off[3] = {0, 0, 0};
                                int oi = 0;
                                while (ofs.read_field(of) && oi < 3) {
                                    if (of.wire_type == proto::WIRE_I32)
                                        off[oi++] = of.as_float();
                                    else if (of.wire_type == proto::WIRE_I64)
                                        off[oi++] = (float)of.as_double();
                                }
                                light.pos[0] = obj.pos_x + off[0];
                                light.pos[1] = obj.pos_y + off[1];
                                light.pos[2] = obj.pos_z + off[2];
                                break;
                            }
                            case 7: light.radius = d.as_float() * 3.0f; break;
                        }
                    }
                    break;
                }
            } catch (...) {}
            if (light.type == 1) continue;         // Ambient lights are baked into g_ambient
            if (!(light.intensity > 0.0f)) continue;  // also rejects NaN
            // Some components omit the color sub-message; fall back to warm white.
            if (light.color[0] <= 0.0f && light.color[1] <= 0.0f && light.color[2] <= 0.0f) {
                light.color[0] = 1.0f; light.color[1] = 0.92f; light.color[2] = 0.78f;
            }
            scene.lights.push_back(light);
        }
    }
    // SimpleGlow components act as warm point lights (torches, fire, crystals).
    for (const auto& obj : scene.objects) {
        for (const auto& comp : obj.components) {
            if (comp.type_name != "SimpleGlow") continue;
            SceneData::SceneLight light;
            light.type = 4;
            light.intensity = 1.4f;
            light.color[0] = 1.0f; light.color[1] = 0.72f; light.color[2] = 0.42f;
            light.pos[0] = obj.pos_x; light.pos[1] = obj.pos_y; light.pos[2] = obj.pos_z;
            light.radius = 260.0f;
            scene.lights.push_back(light);
        }
    }

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
