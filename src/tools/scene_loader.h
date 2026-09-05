#pragma once
/* scene_loader.h — Parse Swordigo .scene protobuf files into a scene graph
 *
 * Swordigo scenes are raw protobuf wire-format binary files (no .proto schema).
 *
 * Top-level Scene message fields:
 *   Tag 1 (LEN, repeated) : SceneObject
 *   Tag 2 (LEN, repeated) : ObjectLibrary
 *   Tag 3 (LEN, repeated) : Bounds (Rectangle)
 *   Tag 4 (LEN, repeated) : Group  (SceneObjectGroup)
 *   Tag 5 (LEN, repeated) : OnLoad (Program)
 *
 * SceneObject fields (true wire-format from block_formats.py / decomp.js):
 *   Tag 1  (LEN)    : TemplateName  (string)
 *   Tag 2  (LEN)    : Identifier    (string)
 *   Tag 3  (LEN)    : Component     (repeated nested)
 *   Tag 4  (LEN)    : Position      (Vector2 nested: tag1=X float, tag2=Y float)
 *   Tag 5  (I32)    : Depth         (float  — world Z / parallax layer)
 *   Tag 6  (I32)    : Rotation      (float  — Y-axis rotation, radians)
 *   Tag 7  (I32)    : Scaling       (float  — uniform scale)
 *   Tag 8  (LEN)    : LocalAabb     (Rectangle nested)
 *   Tag 9  (VARINT) : Hidden        (bool)
 *   Tag 10 (LEN)    : OnLoad        (Program nested)
 */

#include <string>
#include <vector>
#include "platform/protobuf_reader.h"
#include "tools/pod_loader.h"
#include "tools/scene_entity.h"
#include "tools/scene_physics.h"
#include "tools/scene_collision.h"

namespace av {

// ============================================================
// A single component attached to a scene object.
// raw_data holds the entire serialized protobuf bytes for the
// component (including type_name/type_id), so we can write it
// back verbatim during scene_save without losing any sub-fields.
// ============================================================
struct SceneComponent {
    std::string type_name;   // e.g. "Light", "MeshRenderer", "Background"
    int         type_id = 0;
    int         payload_field = 0; // nested Component field (Model=101, GroundMesh=111, ...)
    std::string raw_data;    // full protobuf bytes of this component message
};

struct SceneComponentField {
    uint32_t field_number = 0;
    proto::WireType wire_type = proto::WIRE_VARINT;
    std::string name;
    std::string class_name;
    bool is_message = false;
    size_t occurrence = 0;
    uint64_t varint_value = 0;
    double double_value = 0.0;
    float float_value = 0.0f;
    std::string bytes_value;
};

// Forward declaration (full definition below SceneData).
struct SceneGroup;

// ============================================================
// One object in the scene (game object + transform + components)
// ============================================================
struct SceneObject {
    // --- Protobuf fields (tags 1-10) ---
    std::string template_name;       // Tag 1 : TemplateName
    std::string name;                // Tag 2 : Identifier (display name)
    std::vector<SceneComponent> components; // Tag 3 : Component[]
    std::vector<SceneComponent> resolved_components; // local + inherited, never serialized
    float pos_x   = 0.0f;           // Tag 4 : Position.X
    float pos_y   = 0.0f;           // Tag 4 : Position.Y
    float pos_z   = 0.0f;           // Tag 5 : Depth
    float rot_x   = 0.0f;           // (not stored — zero)
    float rot_y   = 0.0f;           // Tag 6 : Rotation
    float rot_z   = 0.0f;           // (not stored — zero)
    float scale_x = 1.0f;           // Tag 7 : Scaling (uniform)
    float scale_y = 1.0f;           // Tag 7 : Scaling (uniform)
    float scale_z = 1.0f;           // Tag 7 : Scaling (uniform)
    float template_scaling = 1.0f;  // inherited library scale, render-only
    std::string local_aabb;          // Tag 8 : LocalAabb raw bytes (Rectangle)
    bool        hidden = false;      // Tag 9 : Hidden
    std::string onload;              // Tag 10: OnLoad raw bytes (Program)

    // --- References extracted from component data (post-parse helpers) ---
    std::string mesh_name;           // .POD model name if MeshRenderer found
    std::string texture_name;        // texture name if found in component data
    std::string background_name;     // background model if Background component
    // ModelComponent payload: field 2 = baked Y-rotation (radians) that the
    // editor (main.js addModel) applies as rotation.y on top of the scene
    // object's own Rotation. e.g. castle_lockdoor = 4.7124 (270°), chair = 1.57.
    // Ignoring it makes doors/objects face the viewer instead of their mesh side.
    float       model_y_rotation = 0.0f;
    bool        has_model_y_rotation = false;
    bool        is_spawn_point = false;  // SpawnPoint component (camera port)
    int         spawn_facing = 1;        // SpawnPointComponent.FacingDirection
    float       spawn_offset[3] = {0.0f, 0.0f, 0.0f};
    bool        has_model_transform = false;
    float       model_transform_origin[3] = {0.0f, 0.0f, 0.0f};
    float       model_transform_axis[3] = {0.0f, 1.0f, 0.0f};
    float       model_transform_angle = 0.0f;
    float       model_transform_speed = 0.0f;
                bool        is_portal = false;
    std::string portal_destination;
    std::string portal_spawn_point;
    bool        portal_tap_to_enter = false;
    // CameraComponent: an in-game camera object (focus target / camera marker).
    // Flagged defensively — real Swordigo scenes mostly invoke the camera from
    // Lua, but a CameraComponent may be present in some scene graphs.
    bool        is_camera = false;
    // DimensionObject component: the object only appears in-game while the
    // Dimension Rift powerup is active (obj5#5 in lowergrove_part1, etc).
    // Ruby ghosts it (very faint + transparent) unless the rift is toggled on.
    bool        is_dimension_object = false;

    // True when this object carries no renderable geometry — its components are
    // purely non-visual (Light, Portal, CollisionShape, SpawnPoint, controllers,
    // AI, triggers). Such objects resolve fine but have nothing to draw, so the
    // editor shows a tiny neutral marker instead of a "missing model" dot.
    bool is_non_visual() const {
        if (!mesh_name.empty() || !background_name.empty() || !ground_meshes.empty())
            return false;
        if (components.empty() && resolved_components.empty())
            return false;
        for (const auto& c : resolved_components.empty() ? components : resolved_components) {
            const std::string& t = c.type_name;
            if (t.find("MeshRenderer") != std::string::npos ||
                t.find("ModelComponent") != std::string::npos ||
                t.find("GroundMesh") != std::string::npos ||
                t.find("Sprite") != std::string::npos ||
                t.find("Particle") != std::string::npos ||
                t.find("FireEmitter") != std::string::npos ||
                t.find("WaterMesh") != std::string::npos ||
                t.find("Skeleton") != std::string::npos)
                return false;
        }
        return true;
    }

    // --- Embedded GroundMesh meshes and textures ---
    std::vector<PODMesh>       ground_meshes;
    std::vector<std::string>   ground_mesh_textures;

    // --- GroundPolygon walkable surface (from GroundPolygonComponent) ---
    // Populated by scene_loader from GroundPolygonComponent data.
    // Flat {x0,y0, x1,y1, ...} pairs in object-local space, CCW winding.
    // Matches Caver::GroundPolygonComponent vertex list exactly.
    std::vector<float> ground_polygon_points;  // flat xy pairs
    bool               ground_polygon_collides    = true;
    bool               ground_polygon_unsafe      = false;
    float              ground_polygon_friction     = 0.0f;
    float              ground_polygon_min_depth    = -1e9f;
    float              ground_polygon_max_depth    =  1e9f;

    // --- GroundMesh round-trip state (vertex editing support) ---
    // ground_mesh_raw[i]   = original serialized MeshData message for mesh i
    //                        (field 1 num_verts, field 10 material, field 50
    //                        vertex stream, field 51 index stream, ...).
    // ground_mesh_fields[i] = the GroundMeshComponent child field number the
    //                        mesh was read from (6=Mesh, 8=FrontMesh, 9=SurfaceMesh).
    // ground_meshes_dirty   = true once the parsed vertex data was modified;
    //                        scene_save then re-serializes the GroundMesh
    //                        component from ground_meshes instead of writing
    //                        the original raw_data verbatim.
    std::vector<std::string>   ground_mesh_raw;
    std::vector<int>           ground_mesh_fields;
    bool                       ground_meshes_dirty = false;
};

// ============================================================
// The full scene — all objects plus preserved top-level sections
// ============================================================
struct SceneData {
    std::string filename;            // basename of the .scene file
    std::string filepath;            // full path, set during scene_load
    int         object_count = 0;

    // Object list (tag 1 repeated)
    std::vector<SceneObject> objects;

    // Preserved raw bytes for all other top-level sections
    std::vector<std::string> object_libraries;  // tag 2: ObjectLibrary[]
    std::vector<std::string> bounds;            // tag 3: Bounds (Rectangle)[]
    std::vector<std::string> groups;            // tag 4: Group (SceneObjectGroup)[]
    std::vector<std::string> onload_scripts;    // tag 5: OnLoad (Program)[]

    // External .scl object libraries referenced by ObjectLibrary.ImportedLibrary
    // (tag 3). Loaded from disk during scene_load for template resolution only —
    // never serialized back into the scene file.
    std::vector<std::string> external_libraries;

    // Imported library name / resolved path for each entry in external_libraries
    // (parallel vectors, e.g. "game_common" + "/path/to/game_common.scl").
    std::vector<std::string> imported_library_names;
    std::vector<std::string> imported_library_paths;

    // Names of imported libraries that could not be found on disk (diagnostics).
    std::vector<std::string> missing_libraries;

    // Parsed view of the raw SceneObjectGroup messages (never serialized).
    std::vector<SceneGroup> parsed_groups;

    // Parsed WaterMesh components (never serialized — render-time fluid).
    // WaterMeshComponent payload (114) -> { BoundsShapeId (shape id),
    // TextureMappingId (tex id), FrontColor (RGBA), SurfaceColor (RGBA) }.
    // The bounds shape (a ShapeComponent payload 120 Rectangle) defines the
    // fluid sheet: x, y, w, h in object-local coordinates. The texture mapping
    // (TextureMapping payload 113 field 1) names the texture (e.g. "water").
    struct SceneWater {
        int    object_index = -1;    // owning SceneObject index
        float  rect[4] = {0, 0, 0, 0};   // x, y, w, h (object-local)
        float  front_color[4] = {0.5f, 0.7f, 1.0f, 0.7f};   // RGBA
        float  surface_color[4] = {0.7f, 0.9f, 1.0f, 0.7f}; // RGBA
        float  tile_size = 64.0f;    // texture tile size (TextureMapping f2)
        float  tex_offset[2] = {0, 0}; // texture offset (TextureMapping f3)
        std::string texture;         // texture name (e.g. "water")
    };
    std::vector<SceneWater> waters;

    // Parsed Light/SimpleGlow components (never serialized — render-time lighting).
    // LightComponent payload: f130 { f1 Type(1=Ambient,2=Dir,3=Point,4=Overlay),
    // f2 Intensity, f3 Color, f6 Offset, f7 Radius }.
    struct SceneLight {
        int     type      = 3;        // 1 Ambient, 2 Directional, 3 Point, 4 Overlay
        float   intensity = 1.0f;
        float   color[3]  = {1.0f, 0.95f, 0.8f};
        float   pos[3]    = {0, 0, 0}; // point position or directional vector
        float   radius    = 300.0f;
        bool    glow      = false;
        bool    flicker   = false;    // fire-linked point light (FireEmitterComponent)
        float   flicker_speed = 8.0f; // flicker rate multiplier
        float   flicker_amount = 0.35f; // intensity variance around base
        int     object_index = -1;    // owning SceneObject index (fire link resolution)
        float   base_intensity = 1.0f; // stored for flicker modulation
    };
    std::vector<SceneLight> lights;

    // Parsed ShadowComponent instances (never serialized — render-time blobs).
    // ShadowComponent payload: f131 { f1 WidthRadius, f2 DepthRadius, f3 Offset }.
    // The shadow is a soft ellipse on the ground plane under the object.
    struct SceneShadow {
        int    object_index = -1;    // owning SceneObject index
        float  width_radius = 50.0f; // X extent (world units after scale)
        float  depth_radius = 25.0f; // Z extent (world units after scale)
        float  pos[3] = {0, 0, 0};   // world-space blob center
        float  rot_y  = 0.0f;        // object rotation so the ellipse tracks it
    };
    std::vector<SceneShadow> shadows;

    // Parsed overlay (type 4) lights — darkness veil seeds. A scene with an
    // overlay light has a darkened backdrop whose brightness is restored
    // around point-light falloff radii. Kept separate from `lights` so the
    // point-light upload path never sees them.
    struct SceneOverlay {
        float intensity = 1.0f;
        float color[3]  = {0.0f, 0.0f, 0.0f}; // darkness tint
        float pos[3]    = {0, 0, 0};          // ambient/overlay origin
    };
    std::vector<SceneOverlay> overlays;

    // Any unrecognised top-level fields — preserved verbatim for forward compat
    std::vector<proto::Field> other_fields;

    // Axis-aligned bounding box computed from object positions
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};

    // ── Parsed subsystem lists (never serialized — runtime/editor use only) ──

    // Entity instances (objects with EntityComponent / Hero / Monster).
    struct SceneEntityEntry {
        int        object_index = -1;
        EntityData entity;
    };
    std::vector<SceneEntityEntry> entities;

    // Physics instances (objects with PhysicsObjectComponent / PhysicsPlatformComponent).
    struct ScenePhysicsEntry {
        int         object_index = -1;
        PhysicsData physics;
    };
    std::vector<ScenePhysicsEntry> physics_objects;

    // Collision shape instances (objects with ShapeComponent / CollisionShapeComponent
    // / BoneControlledCollisionShapeComponent / GroundPolygonComponent).
    struct SceneCollisionEntry {
        int           object_index = -1;
        CollisionData collision;
    };
    std::vector<SceneCollisionEntry> collisions;
};

// A scene-level group (SceneObjectGroup) that bunches objects together for
// hide/show and selection. Members reference SceneObject.name (tag 2). The raw
// protobuf message is preserved in `raw` for verbatim round-trip serialization.
struct SceneGroup {
    std::string               name;       // tag 1  Identifier
    std::vector<std::string>  members;    // tag 2  ObjectIdentifier[] (repeated)
    bool                      hidden = false;    // tag 3  Hidden
    bool                      locked  = false;   // tag 30 Locked
    std::string               raw;        // original message bytes (round-trip)
};

// Load and parse a .scene file.  Returns an empty SceneData on failure.
SceneData scene_load(const std::string& path);

// Serialize a scene without touching the filesystem (used to synchronize the
// structured and markup editor views).
std::string scene_serialize(const SceneData& scene);

// Refresh derived object count and bounds after editing the object list.
void scene_refresh(SceneData& scene);

// Mark an object's ground meshes as edited so the next scene_save re-encodes
// the GroundMesh component from the parsed vertex/index data.
void scene_mark_ground_mesh_dirty(SceneData& scene, size_t object_index);

// Swap the texture name of one ground mesh on an object. The material sub-message
// (MeshData field 10) is rewritten in place so the change persists through
// scene_save; ground_mesh_textures[mesh_index] is updated and the object is
// flagged dirty. Returns false if the mesh index is out of range.
bool scene_set_ground_mesh_texture(SceneObject& obj, size_t mesh_index,
                                   const std::string& texture_name);

// Object-list editing helpers. New and duplicated objects receive a unique
// objN identifier, matching Ruby's reference editor behavior.
std::string scene_fresh_identifier(const SceneData& scene);

/// One entry of a scene's embedded ObjectLibrary (.scl) — enough for the
/// editor's add-object palette (main.js `library` parity).
struct SceneTemplateInfo {
    std::string name;                              // template name (SceneObject TemplateName)
    float scaling = 1.0f;                          // template scaling (applied at render)
    std::vector<std::string> component_types;      // component type names, in order
};

/// Enumerate every template from the scene's embedded object libraries and
/// its external (.scl) libraries. Components are NOT resolved here — the
/// caller (editor) can call scene_refresh() after adding an object with this
/// template name to materialize them.
std::vector<SceneTemplateInfo> scene_list_templates(const SceneData& scene);

// Parse the raw SceneObjectGroup messages into SceneGroup structs.
std::vector<SceneGroup> parse_scene_groups(const std::vector<std::string>& raw_groups);
size_t scene_create_object(SceneData& scene, const std::string& template_name = "SceneObject");
bool scene_duplicate_object(SceneData& scene, size_t index, size_t* new_index = nullptr);
size_t scene_paste_object(SceneData& scene, const SceneObject& object);
bool scene_delete_object(SceneData& scene, size_t index);
bool scene_move_object(SceneData& scene, size_t from_index, size_t to_index);

// Component editing helpers. Component payload field numbers are resolved from
// the generated Component schema; instance identifiers remain object-local.
std::vector<std::string> scene_component_types();
bool scene_add_component(SceneData& scene, size_t object_index,
                         const std::string& type_name, size_t* new_index = nullptr);
bool scene_remove_component(SceneData& scene, size_t object_index, size_t component_index);
bool scene_paste_component(SceneData& scene, size_t object_index,
                           const SceneComponent& component, size_t* new_index = nullptr);
std::vector<SceneComponentField> scene_component_fields(const SceneComponent& component);
bool scene_set_component_field(SceneComponent& component, const SceneComponentField& value);

std::string scene_program_source(const std::string& program_data);
bool scene_set_program_source(std::string& program_data, const std::string& source);

// Serialize and atomically write a SceneData back to disk.
// All objects are re-serialised via proto::Writer; all preserved raw-byte
// sections (libraries, bounds, groups, onload_scripts, other_fields) are
// written verbatim so that nothing is lost on a round-trip.
// Returns false and optionally fills error_message when the temporary write or
// final replacement fails; the original scene file remains untouched.
bool scene_save(const std::string& path, const SceneData& scene,
                std::string* error_message = nullptr);

// --- SCL (ObjectLibrary) direct editing and serialization ---
struct SclTemplateEntry {
    std::string name;
    float scaling = 1.0f;
    std::string raw_object_bytes;
    SceneObject object;
};

std::vector<SclTemplateEntry> scl_load_templates(const std::string& scl_bytes);
bool scl_update_template(std::string& scl_bytes, const std::string& template_name, const SceneObject& obj);
bool scl_save_to_file(const std::string& filepath, const std::string& scl_bytes, std::string* error_message = nullptr);

} // namespace av
