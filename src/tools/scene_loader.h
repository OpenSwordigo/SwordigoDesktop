#pragma once
/* scene_loader.h — Parse Swordigo .scene files into a scene graph
 *
 * Swordigo scenes are stored as protobuf wire-format messages.
 * This module reads them into a lightweight scene graph that the
 * asset viewer can inspect and (eventually) render.
 *
 * Schema (reverse-engineered from v1.4.x ARM64):
 *   Top-level field 1 (repeated, WIRE_LEN) = scene object
 *     Object field 2: name (string)
 *     Object field 3: component (repeated, nested message)
 *       Component field 1: type name (string)
 *       Component field 2: type ID  (varint)
 *     Object fields 4-8: transform floats (pos, rot, scale)
 */

#include <string>
#include <vector>
#include "tools/pod_loader.h"

namespace av {

// ============================================================
// A single component attached to a scene object
// ============================================================
struct SceneComponent {
    std::string type_name;   // e.g. "Light", "MeshRenderer", "Background"
    int         type_id = 0;
    std::string raw_data;    // raw protobuf bytes for future detailed parsing
};

// ============================================================
// One object in the scene (game object + transform + components)
// ============================================================
struct SceneObject {
    std::string name;        // e.g. "DirectionalLight", "darkhero"
    std::vector<SceneComponent> components;

    // Transform (extracted from fields 4-8)
    float pos_x   = 0, pos_y   = 0, pos_z   = 0;
    float rot_x   = 0, rot_y   = 0, rot_z   = 0;
    float scale_x = 1, scale_y = 1, scale_z = 1;

    // References extracted from component data
    std::string mesh_name;       // .POD model name if MeshRenderer found
    std::string texture_name;    // texture name if found in component data
    std::string background_name; // background model if Background component

    // Embedded GroundMesh meshes and textures
    std::vector<PODMesh> ground_meshes;
    std::vector<std::string> ground_mesh_textures;
};

// ============================================================
// The full scene — all objects + metadata
// ============================================================
struct SceneData {
    std::vector<SceneObject> objects;
    std::string filename;        // basename of the .scene file
    int         object_count = 0;

    // Axis-aligned bounding box computed from object positions
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
};

// Load and parse a .scene file.  Returns an empty SceneData on failure.
SceneData scene_load(const std::string& path);

} // namespace av
