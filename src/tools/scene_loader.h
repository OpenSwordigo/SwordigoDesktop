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
    std::string raw_data;    // full protobuf bytes of this component message
};

// ============================================================
// One object in the scene (game object + transform + components)
// ============================================================
struct SceneObject {
    // --- Protobuf fields (tags 1-10) ---
    std::string template_name;       // Tag 1 : TemplateName
    std::string name;                // Tag 2 : Identifier (display name)
    std::vector<SceneComponent> components; // Tag 3 : Component[]
    float pos_x   = 0.0f;           // Tag 4 : Position.X
    float pos_y   = 0.0f;           // Tag 4 : Position.Y
    float pos_z   = 0.0f;           // Tag 5 : Depth
    float rot_x   = 0.0f;           // (not stored — zero)
    float rot_y   = 0.0f;           // Tag 6 : Rotation
    float rot_z   = 0.0f;           // (not stored — zero)
    float scale_x = 1.0f;           // Tag 7 : Scaling (uniform)
    float scale_y = 1.0f;           // Tag 7 : Scaling (uniform)
    float scale_z = 1.0f;           // Tag 7 : Scaling (uniform)
    std::string local_aabb;          // Tag 8 : LocalAabb raw bytes (Rectangle)
    bool        hidden = false;      // Tag 9 : Hidden
    std::string onload;              // Tag 10: OnLoad raw bytes (Program)

    // --- References extracted from component data (post-parse helpers) ---
    std::string mesh_name;           // .POD model name if MeshRenderer found
    std::string texture_name;        // texture name if found in component data
    std::string background_name;     // background model if Background component

    // --- Embedded GroundMesh meshes and textures ---
    std::vector<PODMesh>       ground_meshes;
    std::vector<std::string>   ground_mesh_textures;
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

    // Any unrecognised top-level fields — preserved verbatim for forward compat
    std::vector<proto::Field> other_fields;

    // Axis-aligned bounding box computed from object positions
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
};

// Load and parse a .scene file.  Returns an empty SceneData on failure.
SceneData scene_load(const std::string& path);

// Serialize and write a SceneData back to disk.
// All objects are re-serialised via proto::Writer; all preserved raw-byte
// sections (libraries, bounds, groups, onload_scripts, other_fields) are
// written verbatim so that nothing is lost on a round-trip.
void scene_save(const std::string& path, const SceneData& scene);

} // namespace av
