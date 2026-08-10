// ani_loader.cpp — .ani parser + evaluator (ported from
// zauonlok/renderer core/skeleton.c, MIT, Zhou Le).
//
// Joint matrices follow the reference exactly:
//   transform = mat4_from_trs(T, R, S)
//   if parent >= 0: transform = parent.transform * transform   (parents are
//                   stored before children, so array order is evaluation order)
//   joint_matrix = transform * inverse_bind
//   normal_matrix = inverse_transpose(mat3(joint_matrix))
// Time wraps with fmod(frame_time, max_time).

#include "ani_loader.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace av {
namespace {

// ── small math (column-major 4x4) ─────────────────────────────────────
inline void m4_mul(float out[16], const float a[16], const float b[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
    }
}

inline void m4_from_trs(float out[16], const float t[3], const float q[4], const float s[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, xy = x * y2, xz = x * z2;
    const float yy = y * y2, yz = y * z2, zz = z * z2;
    const float wx = w * x2, wy = w * y2, wz = w * z2;

    // R (column-major), scaled by S
    const float m[16] = {
        (1 - (yy + zz)) * s[0], (xy + wz) * s[0],      (xz - wy) * s[0],      0,
        (xy - wz) * s[1],       (1 - (xx + zz)) * s[1], (yz + wx) * s[1],     0,
        (xz + wy) * s[2],       (yz - wx) * s[2],      (1 - (xx + yy)) * s[2], 0,
        t[0],                   t[1],                   t[2],                  1
    };
    for (int i = 0; i < 16; ++i) out[i] = m[i];
}

inline float q_dot(const float a[4], const float b[4]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

// Standard quaternion slerp (vendor quat_slerp).
inline void q_slerp(float out[4], const float a[4], const float b[4], float t) {
    float cb[4] = {b[0], b[1], b[2], b[3]};
    float cos_omega = q_dot(a, cb);
    if (cos_omega < 0.0f) {
        cos_omega = -cos_omega;
        cb[0] = -cb[0]; cb[1] = -cb[1]; cb[2] = -cb[2]; cb[3] = -cb[3];
    }
    if (cos_omega > 0.9995f) {   // nearly parallel → nlerp
        for (int i = 0; i < 4; ++i) out[i] = a[i] + t * (cb[i] - a[i]);
        float n = std::sqrt(q_dot(out, out));
        if (n > 1e-8f) for (int i = 0; i < 4; ++i) out[i] /= n;
        return;
    }
    const float omega = std::acos(cos_omega);
    const float sin_omega = std::sin(omega);
    if (sin_omega < 1e-8f) { for (int i = 0; i < 4; ++i) out[i] = a[i]; return; }
    const float s0 = std::sin((1.0f - t) * omega) / sin_omega;
    const float s1 = std::sin(t * omega) / sin_omega;
    for (int i = 0; i < 4; ++i) out[i] = a[i] * s0 + cb[i] * s1;
}

// Keyframe sampling (clamp at ends, lerp between) — vendor get_translation /
// get_rotation / get_scale.
bool sample3(const std::vector<float>& times, const std::vector<float>& vals,
             float t, float def[3], float out[3]) {
    const size_t nk = times.size();
    if (nk == 0) { out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; return false; }
    if (t <= times[0]) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; return true; }
    if (t >= times[nk - 1]) {
        size_t b = (nk - 1) * 3;
        out[0] = vals[b]; out[1] = vals[b + 1]; out[2] = vals[b + 2];
        return true;
    }
    for (size_t i = 0; i + 1 < nk; ++i) {
        if (t >= times[i] && t < times[i + 1]) {
            const float k = (t - times[i]) / (times[i + 1] - times[i]);
            size_t b0 = i * 3, b1 = (i + 1) * 3;
            out[0] = vals[b0] + k * (vals[b1] - vals[b0]);
            out[1] = vals[b0 + 1] + k * (vals[b1 + 1] - vals[b0 + 1]);
            out[2] = vals[b0 + 2] + k * (vals[b1 + 2] - vals[b0 + 2]);
            return true;
        }
    }
    out[0] = def[0]; out[1] = def[1]; out[2] = def[2];
    return false;
}

void sample_quat(const std::vector<float>& times, const std::vector<float>& vals,
                 float t, float out[4]) {
    static const float kIdent[4] = {0, 0, 0, 1};
    const size_t nk = times.size();
    if (nk == 0) { out[0] = kIdent[0]; out[1] = kIdent[1]; out[2] = kIdent[2]; out[3] = kIdent[3]; return; }
    if (t <= times[0]) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3]; return; }
    if (t >= times[nk - 1]) {
        size_t b = (nk - 1) * 4;
        out[0] = vals[b]; out[1] = vals[b + 1]; out[2] = vals[b + 2]; out[3] = vals[b + 3];
        return;
    }
    for (size_t i = 0; i + 1 < nk; ++i) {
        if (t >= times[i] && t < times[i + 1]) {
            const float k = (t - times[i]) / (times[i + 1] - times[i]);
            q_slerp(out, &vals[i * 4], &vals[(i + 1) * 4], k);
            return;
        }
    }
    out[0] = kIdent[0]; out[1] = kIdent[1]; out[2] = kIdent[2]; out[3] = kIdent[3];
}

// inverse-transpose of the upper-left 3x3 of a column-major 4x4.
void m3_inverse_transpose(float out[9], const float m[16]) {
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[4], e = m[5], f = m[6];
    const float g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) {   // singular → identity
        out[0] = 1; out[1] = 0; out[2] = 0;
        out[3] = 0; out[4] = 1; out[5] = 0;
        out[6] = 0; out[7] = 0; out[8] = 1;
        return;
    }
    const float inv = 1.0f / det;
    // cofactor matrix (adjugate, no transpose yet)
    const float c00 =  (e * i - f * h) * inv;
    const float c01 = -(d * i - f * g) * inv;
    const float c02 =  (d * h - e * g) * inv;
    const float c10 = -(b * i - c * h) * inv;
    const float c11 =  (a * i - c * g) * inv;
    const float c12 = -(a * h - b * g) * inv;
    const float c20 =  (b * f - c * e) * inv;
    const float c21 = -(a * f - c * d) * inv;
    const float c22 =  (a * e - b * d) * inv;
    // inverse_transpose[i][j] = C[i][j] / det → emit row-major cofactors
    out[0] = c00; out[1] = c01; out[2] = c02;
    out[3] = c10; out[4] = c11; out[5] = c12;
    out[6] = c20; out[7] = c21; out[8] = c22;
}

} // namespace

bool ani_load(const std::string& path, AniSkeleton& out, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);

    AniSkeleton skel;
    int num_joints = 0;
    size_t i = 0;
    bool have_size = false, have_range = false;

    for (; i < lines.size(); ++i) {
        int nj = 0;
        if (sscanf(lines[i].c_str(), " joint-size: %d", &nj) == 1) {
            num_joints = nj; have_size = true;
            break;
        }
    }
    for (++i; i < lines.size(); ++i) {
        float a = 0, b = 0;
        if (sscanf(lines[i].c_str(), " time-range: [%f, %f]", &a, &b) == 2) {
            skel.min_time = a; skel.max_time = b; have_range = true;
            break;
        }
    }
    if (!have_size || !have_range || num_joints <= 0 || !(skel.max_time > skel.min_time)) {
        if (err) *err = "malformed .ani header in " + path;
        return false;
    }
    if (num_joints > 4096) {   // sanity guard
        if (err) *err = "implausible joint count in " + path;
        return false;
    }

    skel.joints.resize(static_cast<size_t>(num_joints));

    // locate each "joint N:" block in order
    for (int j = 0; j < num_joints; ++j) {
        for (; i < lines.size(); ++i) {
            int idx = -1;
            if (sscanf(lines[i].c_str(), " joint %d:", &idx) == 1 && idx == j) break;
        }
        if (i >= lines.size()) {
            if (err) *err = "missing joint " + std::to_string(j) + " in " + path;
            return false;
        }
        AniJoint& joint = skel.joints[j];

        // parent-index
        for (++i; i < lines.size(); ++i) {
            int p = -999;
            if (sscanf(lines[i].c_str(), " parent-index: %d", &p) == 1) {
                joint.parent = p;
                break;
            }
        }
        // inverse-bind (label line + 4 rows of 4)
        for (++i; i < lines.size(); ++i) {
            if (lines[i].find("inverse-bind:") != std::string::npos) break;
        }
        for (int r = 0; r < 4 && i + 1 < lines.size(); ++r) {
            ++i;
            float v[4];
            if (sscanf(lines[i].c_str(), " %f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4)
                // file rows are row-major (m[r][c]) — transpose into column-major
                for (int c = 0; c < 4; ++c) joint.inverse_bind[c * 4 + r] = v[c];
        }
        // translations k:
        for (++i; i < lines.size(); ++i) {
            int k = -1;
            if (sscanf(lines[i].c_str(), " translations %d:", &k) == 1) {
                joint.t_times.reserve(k > 0 ? (size_t)k : 0);
                joint.t_vals.reserve(k > 0 ? (size_t)k * 3 : 0);
                for (int q = 0; q < k && i + 1 < lines.size(); ++q) {
                    ++i;
                    float tm, x, y, z;
                    if (sscanf(lines[i].c_str(), " time: %f, value: [%f, %f, %f]",
                               &tm, &x, &y, &z) == 4) {
                        joint.t_times.push_back(tm);
                        joint.t_vals.push_back(x); joint.t_vals.push_back(y); joint.t_vals.push_back(z);
                    }
                }
                break;
            }
        }
        // rotations k:
        for (++i; i < lines.size(); ++i) {
            int k = -1;
            if (sscanf(lines[i].c_str(), " rotations %d:", &k) == 1) {
                joint.r_times.reserve(k > 0 ? (size_t)k : 0);
                joint.r_vals.reserve(k > 0 ? (size_t)k * 4 : 0);
                for (int q = 0; q < k && i + 1 < lines.size(); ++q) {
                    ++i;
                    float tm, x, y, z, w;
                    if (sscanf(lines[i].c_str(), " time: %f, value: [%f, %f, %f, %f]",
                               &tm, &x, &y, &z, &w) == 5) {
                        joint.r_times.push_back(tm);
                        joint.r_vals.push_back(x); joint.r_vals.push_back(y);
                        joint.r_vals.push_back(z); joint.r_vals.push_back(w);
                    }
                }
                break;
            }
        }
        // scales k:
        for (++i; i < lines.size(); ++i) {
            int k = -1;
            if (sscanf(lines[i].c_str(), " scales %d:", &k) == 1) {
                joint.s_times.reserve(k > 0 ? (size_t)k : 0);
                joint.s_vals.reserve(k > 0 ? (size_t)k * 3 : 0);
                for (int q = 0; q < k && i + 1 < lines.size(); ++q) {
                    ++i;
                    float tm, x, y, z;
                    if (sscanf(lines[i].c_str(), " time: %f, value: [%f, %f, %f]",
                               &tm, &x, &y, &z) == 4) {
                        joint.s_times.push_back(tm);
                        joint.s_vals.push_back(x); joint.s_vals.push_back(y); joint.s_vals.push_back(z);
                    }
                }
                break;
            }
        }
    }

    out = std::move(skel);
    return true;
}

bool ani_evaluate(const AniSkeleton& skel, float t,
                  std::vector<float>& joint_matrices,
                  std::vector<float>* normal_matrices,
                  std::vector<float>* world_matrices) {
    const size_t n = skel.joints.size();
    if (n == 0) return false;

    const float span = skel.max_time - skel.min_time;
    const float frame_time = (span > 1e-6f)
        ? skel.min_time + std::fmod(t - skel.min_time, span)
        : skel.min_time;

    joint_matrices.resize(n * 16);
    if (normal_matrices) normal_matrices->assign(n * 9, 0.0f);
    if (world_matrices) world_matrices->assign(n * 16, 0.0f);

    std::vector<float> world(n * 16, 0.0f);

    for (size_t i = 0; i < n; ++i) {
        const AniJoint& j = skel.joints[i];

        float tdef[3] = {0, 0, 0};
        float sdef[3] = {1, 1, 1};
        float tr[3], sc[3], rot[4];
        sample3(j.t_times, j.t_vals, frame_time, tdef, tr);
        sample_quat(j.r_times, j.r_vals, frame_time, rot);
        sample3(j.s_times, j.s_vals, frame_time, sdef, sc);

        float local[16];
        m4_from_trs(local, tr, rot, sc);

        float* w = &world[i * 16];
        if (j.parent >= 0 && j.parent < (int)n) {
            const float* pw = &world[static_cast<size_t>(j.parent) * 16];
            m4_mul(w, pw, local);
        } else {
            for (int k = 0; k < 16; ++k) w[k] = local[k];
        }

        float* jm = &joint_matrices[i * 16];
        m4_mul(jm, w, j.inverse_bind);   // final = composed * inverse_bind

        if (world_matrices) {
            std::memcpy(&(*world_matrices)[i * 16], w, sizeof(float) * 16);
        }

        if (normal_matrices) {
            float* nm = &(*normal_matrices)[i * 9];
            m3_inverse_transpose(nm, jm);
        }
    }
    return true;
}

} // namespace av
