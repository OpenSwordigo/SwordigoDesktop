#pragma once
// obj_loader.h — Wavefront .obj → PODModel (renderer-master compatible)
//
// Tolerant triangulating parser:
//   · v / vt / vn / f with the v, v/vt, v//vn, v/vt/vn corner forms (quads and
//     ngons are fan-triangulated)
//   · optional per-corner extension comments used by zauonlok/renderer's
//     glTF→OBJ exporter (skinned + normal-mapped demo assets):
//         # ext.tangent x y z w     (w = bitangent handedness)
//         # ext.joint  j0 j1 j2 j3
//         # ext.weight w0 w1 w2 w3
//   · optional .mtl (Kd + map_Kd for the first material); falls back to a
//     same-stem tga/png/jpg next to the file.
//
// Output: a single-mesh PODModel (one node, one material). Ext joint/weight
// data lands in PODMesh::bone_indices/bone_weights (4 floats per vertex) but
// bones_per_vertex stays 0 — CPU skinning is not applicable; the PBR GPU
// path (upload_mesh_ex + pbr_set_joint_matrices) consumes the attributes
// directly. UVs are preserved as authored (DCC top-origin); the viewer's
// uFlipV handles orientation at sample time.
#include <string>
#include "pod_loader.h"

namespace av {

bool obj_load(const std::string& path, PODModel& out, std::string* err = nullptr);

} // namespace av
