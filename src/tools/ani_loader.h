#pragma once
// ani_loader.h — zauonlok/renderer .ani skeletal animation clips.
//
// Format (text, one joint block per bone — see src/render/zauonlok/skeleton.c):
//   joint-size: N
//   time-range: [min, max]
//   joint i:
//       parent-index: p
//       inverse-bind:           4x4 row-major
//       translations k:         k ×  time: t, value: [x, y, z]
//       rotations k:            k ×  time: t, value: [x, y, z, w]   (quat)
//       scales k:               k ×  time: t, value: [x, y, z]
//
// ani_evaluate() is a direct port of skeleton_update_joints(): per-joint TRS
// keyframe lookup (clamp + lerp / quat_slerp), parent-chain compose, then
// final = composed * inverse_bind, with the inverse-transpose 3x3 for
// normals. Time wraps at max_time (fmod). Output joint matrices are
// column-major — ready for av::pbr_set_joint_matrices (GPU 4-bone skinning).
#include <string>
#include <vector>

namespace av {

struct AniJoint {
    int parent = -1;
    float inverse_bind[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // column-major
    std::vector<float> t_times; std::vector<float> t_vals; // 3 per key
    std::vector<float> r_times; std::vector<float> r_vals; // 4 per key (xyzw)
    std::vector<float> s_times; std::vector<float> s_vals; // 3 per key
};

struct AniSkeleton {
    float min_time = 0.0f;
    float max_time = 1.0f;
    std::vector<AniJoint> joints;

    bool valid() const { return !joints.empty() && max_time > min_time; }
};

// Parse an .ani clip. Returns false (with optional err) on failure.
bool ani_load(const std::string& path, AniSkeleton& out, std::string* err = nullptr);

// Evaluate the clip at time t (seconds, wraps at max_time).
// Fills joint_matrices ([16 * joints] column-major) and, when non-null,
// normal_matrices ([9 * joints] row-major inverse-transpose 3x3) and
// world_matrices ([16 * joints] column-major composed transforms BEFORE the
// inverse-bind multiply — their [12..14] translation is each bone's world
// position, useful for skeleton debug overlays).
// Returns false if the skeleton is empty.
bool ani_evaluate(const AniSkeleton& skel, float t,
                  std::vector<float>& joint_matrices,
                  std::vector<float>* normal_matrices = nullptr,
                  std::vector<float>* world_matrices = nullptr);

} // namespace av
