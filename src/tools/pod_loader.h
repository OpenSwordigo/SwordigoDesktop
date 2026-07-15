#pragma once
// pod_loader.h — PowerVR POD model parser (chunk-based tag-length-data format)
//
// Based on the PowerVR SDK Flash loader (EPODIdentifiers.as / PODLoader.as)
// Reference: powervr-flash-sdk-master/src/com/powervr/pod/
//
// Standalone reusable module. No OpenGL or ImGui dependencies.
// Parses .pod files used by Swordigo into flat vertex/index arrays
// ready for GPU upload.

#include <string>
#include <vector>
#include <cstdint>

namespace av {

// ─── Per-mesh data ───────────────────────────────────────────────────
struct PODMesh {
    std::vector<float>    positions;   // flat xyz, 3 floats per vertex
    std::vector<float>    normals;     // flat xyz, 3 floats per vertex
    std::vector<float>    uvs;         // flat uv,  2 floats per vertex
    std::vector<uint16_t> indices;     // triangle indices

    int num_vertices = 0;
    int num_faces    = 0;

    // Per-mesh axis-aligned bounding box
    float min_x =  1e9f, min_y =  1e9f, min_z =  1e9f;
    float max_x = -1e9f, max_y = -1e9f, max_z = -1e9f;
};

// ─── Material data ───────────────────────────────────────────────────
struct PODMaterial {
    std::string name;
    int diffuse_texture_index = -1; // index into texture_filenames
};

// ─── Hierarchical node data ──────────────────────────────────────────
struct PODNode {
    std::string name;
    int object_index   = -1; // index in meshes (if >= 0 and < num_mesh_nodes it is a mesh)
    int parent_index   = -1; // index in nodes (-1 if root)
    int material_index = -1; // index into materials

    // Static (deprecated) transform fields — used when no anim data present
    bool has_matrix = false;
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    bool has_translation = false;
    float translation[3] = {0,0,0};

    bool has_rotation = false;
    float rotation[4] = {0,0,0,1}; // xyzw quaternion

    bool has_scale = false;
    float scale[3] = {1,1,1};

    // Animation keyframes (if present) — full arrays, one entry per frame
    std::vector<float> anim_translation; // size: 3 * num_frames
    std::vector<float> anim_rotation;    // size: 4 * num_frames
    std::vector<float> anim_scale;       // size: 7 * num_frames (xyz + quat — SDK stores 7)
    std::vector<float> anim_matrix;      // size: 16 * num_frames

    // Optional index arrays (sparse animation)
    std::vector<uint32_t> anim_translation_idx;
    std::vector<uint32_t> anim_rotation_idx;
    std::vector<uint32_t> anim_scale_idx;
    std::vector<uint32_t> anim_matrix_idx;

    uint32_t anim_flags = 0;
};

// ─── Whole-model aggregate ───────────────────────────────────────────
struct PODModel {
    std::vector<PODMesh>     meshes;
    std::vector<PODNode>     nodes;
    std::vector<PODMaterial> materials;
    std::vector<std::string> texture_filenames; // indexed by material.diffuse_texture_index

    std::string version;
    int num_frames     = 0;
    int num_mesh_nodes = 0; // first num_mesh_nodes nodes are mesh nodes

    // Bounding sphere (computed from union of all mesh AABBs)
    float center_x = 0, center_y = 0, center_z = 0;
    float radius   = 1.0f;

    // Totals across all meshes
    int total_vertices = 0;
    int total_faces    = 0;

    // Global AABB
    float min_x =  1e9f, min_y =  1e9f, min_z =  1e9f;
    float max_x = -1e9f, max_y = -1e9f, max_z = -1e9f;
};

// Load a POD model from a file path. Returns an empty model on failure.
PODModel pod_load(const std::string& path);

// Parse a POD model from an in-memory buffer.
PODModel pod_parse(const uint8_t* data, size_t size);

// Construct absolute world matrix for a node at a given frame.
void get_node_matrix(const PODModel& model, int node_idx, int frame, float* mOut);

} // namespace av
