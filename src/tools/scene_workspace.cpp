/* scene_workspace.cpp — implementation of the Ruby scene-editor helper module.
 *
 * Straightforward math/geometry — no UI state, no globals.
 */
#include "scene_workspace.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace swk {

static constexpr float kPi = 3.14159265358979323846f;

// ============================================================================
// Camera / projection math
// ============================================================================

void camera_basis(const av::Camera& cam, float right[3], float up[3], float forward[3]) {
    const float yaw   = cam.yaw * kPi / 180.0f;
    const float pitch = cam.pitch * kPi / 180.0f;
    const float cp = cosf(pitch);
    const float eye[3] = {
        cam.target[0] + cam.distance * cp * sinf(yaw),
        cam.target[1] + cam.distance * sinf(pitch),
        cam.target[2] + cam.distance * cp * cosf(yaw)
    };
    forward[0] = cam.target[0] - eye[0];
    forward[1] = cam.target[1] - eye[1];
    forward[2] = cam.target[2] - eye[2];
    const float fl = std::sqrt(forward[0]*forward[0] + forward[1]*forward[1] + forward[2]*forward[2]);
    if (fl > 0.0f) { forward[0] /= fl; forward[1] /= fl; forward[2] /= fl; }

    right[0] = forward[2];
    right[1] = 0.0f;
    right[2] = -forward[0];
    const float rl = std::sqrt(right[0]*right[0] + right[2]*right[2]);
    if (rl > 0.0f) { right[0] /= rl; right[2] /= rl; }

    up[0] = right[1]*forward[2] - right[2]*forward[1];
    up[1] = right[2]*forward[0] - right[0]*forward[2];
    up[2] = right[0]*forward[1] - right[1]*forward[0];
}

bool world_to_screen(const av::Camera& cam, int w, int h, const ImVec2& viewport_pos,
                     const float world[3], ImVec2& out) {
    float view[16], proj[16], vp[16];
    av::camera_get_view_matrix(cam, view);
    av::camera_get_projection(cam, (h > 0) ? (float)w / (float)h : 1.0f, proj);
    av::mat4_multiply(vp, proj, view);

    const float p[4] = {world[0], world[1], world[2], 1.0f};
    float c[4];
    for (int r = 0; r < 4; ++r)
        c[r] = vp[r]*p[0] + vp[4+r]*p[1] + vp[8+r]*p[2] + vp[12+r]*p[3];
    if (c[3] <= 0.0f) return false;  // behind the camera
    const float ndc_x = c[0] / c[3];
    const float ndc_y = c[1] / c[3];
    if (ndc_x < -1.01f || ndc_x > 1.01f || ndc_y < -1.01f || ndc_y > 1.01f) return false;
    out.x = viewport_pos.x + (ndc_x * 0.5f + 0.5f) * (float)w;
    out.y = viewport_pos.y + (0.5f - ndc_y * 0.5f) * (float)h;
    return true;
}

void screen_ray(const av::Camera& cam, int w, int h, const ImVec2& viewport_pos,
                const ImVec2& screen, float origin[3], float dir[3]) {
    float right[3], up[3], forward[3];
    camera_basis(cam, right, up, forward);
    const float yaw   = cam.yaw * kPi / 180.0f;
    const float pitch = cam.pitch * kPi / 180.0f;
    const float cp = cosf(pitch);
    origin[0] = cam.target[0] + cam.distance * cp * sinf(yaw);
    origin[1] = cam.target[1] + cam.distance * sinf(pitch);
    origin[2] = cam.target[2] + cam.distance * cp * cosf(yaw);

    const float tan_half_fov = tanf(cam.fov * kPi / 360.0f);
    const float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    const float ndc_x = ((screen.x - viewport_pos.x) / std::max(1, w)) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - ((screen.y - viewport_pos.y) / std::max(1, h)) * 2.0f;
    dir[0] = forward[0] + right[0] * ndc_x * tan_half_fov * aspect + up[0] * ndc_y * tan_half_fov;
    dir[1] = forward[1] + right[1] * ndc_x * tan_half_fov * aspect + up[1] * ndc_y * tan_half_fov;
    dir[2] = forward[2] + right[2] * ndc_x * tan_half_fov * aspect + up[2] * ndc_y * tan_half_fov;
    const float len = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (len > 0.0f) { dir[0] /= len; dir[1] /= len; dir[2] /= len; }
}

bool ray_plane_y(const float origin[3], const float dir[3], float plane_y, float out[3]) {
    if (std::fabs(dir[1]) < 1e-6f) return false;
    const float t = (plane_y - origin[1]) / dir[1];
    out[0] = origin[0] + dir[0] * t;
    out[1] = plane_y;
    out[2] = origin[2] + dir[2] * t;
    return true;
}

// ============================================================================
// Scene-object / mesh geometry
// ============================================================================

void object_world_matrix(const av::SceneObject& obj, float out[16]) {
    float T[16], R[16], S[16], temp[16];
    av::mat4_translate(T, obj.pos_x, obj.pos_y, obj.pos_z);
    av::mat4_rotate_z(R, obj.rot_y * 180.0f / kPi);
    av::mat4_identity(S);
    S[0]  = obj.scale_x * obj.template_scaling;
    S[5]  = obj.scale_y * obj.template_scaling;
    S[10] = obj.scale_z * obj.template_scaling;
    av::mat4_multiply(temp, T, R);
    av::mat4_multiply(out, temp, S);
}

void recompute_ground_mesh_geometry(av::PODMesh& pm) {
    if ((size_t)pm.num_vertices * 3 <= pm.normals.size() && pm.num_vertices > 0) {
        std::fill(pm.normals.begin(), pm.normals.begin() + (size_t)pm.num_vertices * 3, 0.0f);
        for (size_t f = 0; f + 2 < pm.indices.size(); f += 3) {
            const uint32_t i0 = pm.indices[f], i1 = pm.indices[f+1], i2 = pm.indices[f+2];
            if (i0 >= (uint32_t)pm.num_vertices || i1 >= (uint32_t)pm.num_vertices ||
                i2 >= (uint32_t)pm.num_vertices) continue;
            const float* a = &pm.positions[i0*3];
            const float* b = &pm.positions[i1*3];
            const float* c = &pm.positions[i2*3];
            const float ux = b[0]-a[0], uy = b[1]-a[1], uz = b[2]-a[2];
            const float vx = c[0]-a[0], vy = c[1]-a[1], vz = c[2]-a[2];
            float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
            const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            pm.normals[i0*3] += nx; pm.normals[i0*3+1] += ny; pm.normals[i0*3+2] += nz;
            pm.normals[i1*3] += nx; pm.normals[i1*3+1] += ny; pm.normals[i1*3+2] += nz;
            pm.normals[i2*3] += nx; pm.normals[i2*3+1] += ny; pm.normals[i2*3+2] += nz;
        }
        for (int i = 0; i < pm.num_vertices; ++i) {
            float* n = &pm.normals[i*3];
            const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
            else n[1] = 1.0f;
        }
    }
    pm.min_x = pm.min_y = pm.min_z = 1e9f;
    pm.max_x = pm.max_y = pm.max_z = -1e9f;
    for (int i = 0; i < pm.num_vertices; ++i) {
        const float x = pm.positions[i*3], y = pm.positions[i*3+1], z = pm.positions[i*3+2];
        pm.min_x = std::min(pm.min_x, x); pm.max_x = std::max(pm.max_x, x);
        pm.min_y = std::min(pm.min_y, y); pm.max_y = std::max(pm.max_y, y);
        pm.min_z = std::min(pm.min_z, z); pm.max_z = std::max(pm.max_z, z);
    }
}

float snap_value(float value, float step) {
    if (step <= 0.0f) return value;
    return std::roundf(value / step) * step;
}

bool ground_mesh_delete_vertex(av::PODMesh& pm, int vertex_idx) {
    // No minimum-vertex floor: the editor has full undo, and edge-collapse
    // legitimately removes two endpoints in a row.  Only the real sanity
    // checks (range + consistent buffer sizes) remain.
    if (pm.num_vertices <= 0 || vertex_idx < 0 || vertex_idx >= pm.num_vertices) return false;
    if ((size_t)pm.num_vertices * 3 > pm.positions.size()) return false; // malformed mesh
    const int v = vertex_idx;
    pm.positions.erase(pm.positions.begin() + v * 3, pm.positions.begin() + v * 3 + 3);
    if ((size_t)pm.num_vertices * 3 <= pm.normals.size())
        pm.normals.erase(pm.normals.begin() + v * 3, pm.normals.begin() + v * 3 + 3);
    if ((size_t)pm.num_vertices * 2 <= pm.uvs.size())
        pm.uvs.erase(pm.uvs.begin() + v * 2, pm.uvs.begin() + v * 2 + 2);
    --pm.num_vertices;
    std::vector<uint32_t> new_indices;
    new_indices.reserve(pm.indices.size());
    for (size_t i = 0; i + 2 < pm.indices.size(); i += 3) {
        uint32_t a = pm.indices[i], b = pm.indices[i+1], c = pm.indices[i+2];
        if (a == (uint32_t)v || b == (uint32_t)v || c == (uint32_t)v) continue;
        if (a > (uint32_t)v) --a;
        if (b > (uint32_t)v) --b;
        if (c > (uint32_t)v) --c;
        new_indices.push_back(a); new_indices.push_back(b); new_indices.push_back(c);
    }
    pm.indices = std::move(new_indices);
    pm.num_faces = static_cast<int>(pm.indices.size() / 3);
    return true;
}

bool ground_mesh_delete_triangle(av::PODMesh& pm, int tri) {
    const size_t f = static_cast<size_t>(tri) * 3;
    if (f + 2 >= pm.indices.size()) return false;
    pm.indices.erase(pm.indices.begin() + static_cast<std::ptrdiff_t>(f),
                     pm.indices.begin() + static_cast<std::ptrdiff_t>(f) + 3);
    pm.num_faces = static_cast<int>(pm.indices.size() / 3);
    return true;
}

int ground_mesh_subdivide_triangle(av::PODMesh& pm, int tri) {
    // The scene format serializes indices as uint16; refuse to grow a mesh
    // past the cap so a burst of insert ops can never corrupt a saved scene.
    if (pm.num_vertices >= 65535) return -1;
    const size_t f = static_cast<size_t>(tri) * 3;
    if (f + 2 >= pm.indices.size()) return -1;
    const uint32_t a = pm.indices[f], b = pm.indices[f+1], c = pm.indices[f+2];
    if (a >= (uint32_t)pm.num_vertices || b >= (uint32_t)pm.num_vertices ||
        c >= (uint32_t)pm.num_vertices || (size_t)pm.num_vertices * 3 > pm.positions.size())
        return -1;

    const int n = pm.num_vertices;  // index of the new centroid vertex
    pm.positions.push_back((pm.positions[a*3]   + pm.positions[b*3]   + pm.positions[c*3])   / 3.0f);
    pm.positions.push_back((pm.positions[a*3+1] + pm.positions[b*3+1] + pm.positions[c*3+1]) / 3.0f);
    pm.positions.push_back((pm.positions[a*3+2] + pm.positions[b*3+2] + pm.positions[c*3+2]) / 3.0f);
    if ((size_t)pm.num_vertices * 3 <= pm.normals.size()) {
        pm.normals.push_back((pm.normals[a*3]   + pm.normals[b*3]   + pm.normals[c*3])   / 3.0f);
        pm.normals.push_back((pm.normals[a*3+1] + pm.normals[b*3+1] + pm.normals[c*3+1]) / 3.0f);
        pm.normals.push_back((pm.normals[a*3+2] + pm.normals[b*3+2] + pm.normals[c*3+2]) / 3.0f);
    }
    if ((size_t)pm.num_vertices * 2 <= pm.uvs.size()) {
        pm.uvs.push_back((pm.uvs[a*2]   + pm.uvs[b*2]   + pm.uvs[c*2])   / 3.0f);
        pm.uvs.push_back((pm.uvs[a*2+1] + pm.uvs[b*2+1] + pm.uvs[c*2+1]) / 3.0f);
    }
    ++pm.num_vertices;

    // Replace (a,b,c) with (a,b,n), (b,c,n), (c,a,n) — winding preserved.
    pm.indices[f] = a; pm.indices[f+1] = b; pm.indices[f+2] = (uint32_t)n;
    pm.indices.push_back(b); pm.indices.push_back(c); pm.indices.push_back((uint32_t)n);
    pm.indices.push_back(c); pm.indices.push_back(a); pm.indices.push_back((uint32_t)n);
    pm.num_faces = static_cast<int>(pm.indices.size() / 3);
    return n;
}

int ground_mesh_split_edge(av::PODMesh& pm, int v0, int v1) {
    // Indices serialize as uint16; guard the vertex cap (see subdivide).
    if (pm.num_vertices >= 65535) return -1;
    if (v0 < 0 || v1 < 0 || v0 >= pm.num_vertices || v1 >= pm.num_vertices || v0 == v1) return -1;
    if ((size_t)pm.num_vertices * 3 > pm.positions.size()) return -1;

    // Collect every triangle that uses the edge in either winding.
    std::vector<size_t> tris;
    for (size_t f = 0; f + 2 < pm.indices.size(); f += 3) {
        const uint32_t a = pm.indices[f], b = pm.indices[f+1], c = pm.indices[f+2];
        const bool ab = (a == (uint32_t)v0 && b == (uint32_t)v1) || (a == (uint32_t)v1 && b == (uint32_t)v0);
        const bool bc = (b == (uint32_t)v0 && c == (uint32_t)v1) || (b == (uint32_t)v1 && c == (uint32_t)v0);
        const bool ca = (c == (uint32_t)v0 && a == (uint32_t)v1) || (c == (uint32_t)v1 && a == (uint32_t)v0);
        if (ab || bc || ca) tris.push_back(f);
    }
    if (tris.empty()) return -1;

    const int n = pm.num_vertices;  // new midpoint vertex
    pm.positions.push_back((pm.positions[v0*3]   + pm.positions[v1*3])   * 0.5f);
    pm.positions.push_back((pm.positions[v0*3+1] + pm.positions[v1*3+1]) * 0.5f);
    pm.positions.push_back((pm.positions[v0*3+2] + pm.positions[v1*3+2]) * 0.5f);
    if ((size_t)pm.num_vertices * 3 <= pm.normals.size()) {
        pm.normals.push_back((pm.normals[v0*3]   + pm.normals[v1*3])   * 0.5f);
        pm.normals.push_back((pm.normals[v0*3+1] + pm.normals[v1*3+1]) * 0.5f);
        pm.normals.push_back((pm.normals[v0*3+2] + pm.normals[v1*3+2]) * 0.5f);
    }
    if ((size_t)pm.num_vertices * 2 <= pm.uvs.size()) {
        pm.uvs.push_back((pm.uvs[v0*2]   + pm.uvs[v1*2])   * 0.5f);
        pm.uvs.push_back((pm.uvs[v0*2+1] + pm.uvs[v1*2+1]) * 0.5f);
    }
    ++pm.num_vertices;

    // Rebuild the index stream, splitting each affected triangle.
    // Triangle (a,b,c) with edge (v0,v1) becomes (a,m,c) + (m,b,c) for the
    // matching pair (a,b) — keep the non-edge vertex as the third index.
    std::vector<uint32_t> out;
    out.reserve(pm.indices.size() + tris.size() * 3);
    size_t tri_cursor = 0;
    for (size_t f = 0; f + 2 < pm.indices.size(); f += 3) {
        if (tri_cursor < tris.size() && tris[tri_cursor] == f) {
            const uint32_t a = pm.indices[f], b = pm.indices[f+1], c = pm.indices[f+2];
            uint32_t e0 = a, e1 = b, other = c;
            const bool bc = (b == (uint32_t)v0 && c == (uint32_t)v1) || (b == (uint32_t)v1 && c == (uint32_t)v0);
            const bool ca = (c == (uint32_t)v0 && a == (uint32_t)v1) || (c == (uint32_t)v1 && a == (uint32_t)v0);
            if (bc)      { e0 = b; e1 = c; other = a; }
            else if (ca) { e0 = c; e1 = a; other = b; }
            out.push_back(e0); out.push_back((uint32_t)n); out.push_back(other);
            out.push_back((uint32_t)n); out.push_back(e1); out.push_back(other);
            ++tri_cursor;
        } else {
            out.push_back(pm.indices[f]);
            out.push_back(pm.indices[f+1]);
            out.push_back(pm.indices[f+2]);
        }
    }
    pm.indices = std::move(out);
    pm.num_faces = static_cast<int>(pm.indices.size() / 3);
    return n;
}

static void local_to_world(const float mat[16], const float p[3], float out[3]) {
    out[0] = mat[0]*p[0] + mat[4]*p[1] + mat[8]*p[2]  + mat[12];
    out[1] = mat[1]*p[0] + mat[5]*p[1] + mat[9]*p[2]  + mat[13];
    out[2] = mat[2]*p[0] + mat[6]*p[1] + mat[10]*p[2] + mat[14];
}

static float point_segment_distance_sq(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float len2 = abx*abx + aby*aby;
    if (len2 < 1e-9f) {
        const float dx = p.x - a.x, dy = p.y - a.y;
        return dx*dx + dy*dy;
    }
    float t = ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2;
    t = std::max(0.0f, std::min(1.0f, t));
    const float qx = a.x + t * abx, qy = a.y + t * aby;
    const float dx = p.x - qx, dy = p.y - qy;
    return dx*dx + dy*dy;
}

int pick_ground_mesh_triangle(const av::PODMesh& pm, const float obj_mat[16],
                              const av::Camera& cam, int w, int h,
                              const ImVec2& viewport_pos, const ImVec2& mouse) {
    int best = -1;
    float best_centroid_d = 1e30f;
    for (size_t f = 0; f + 2 < pm.indices.size(); f += 3) {
        const uint32_t a = pm.indices[f], b = pm.indices[f+1], c = pm.indices[f+2];
        if (a >= (uint32_t)pm.num_vertices || b >= (uint32_t)pm.num_vertices ||
            c >= (uint32_t)pm.num_vertices) continue;
        const float la[3] = {pm.positions[a*3], pm.positions[a*3+1], pm.positions[a*3+2]};
        const float lb[3] = {pm.positions[b*3], pm.positions[b*3+1], pm.positions[b*3+2]};
        const float lc[3] = {pm.positions[c*3], pm.positions[c*3+1], pm.positions[c*3+2]};
        float wa[3], wb[3], wc[3];
        local_to_world(obj_mat, la, wa);
        local_to_world(obj_mat, lb, wb);
        local_to_world(obj_mat, lc, wc);
        ImVec2 sa, sb, sc;
        if (!world_to_screen(cam, w, h, viewport_pos, wa, sa)) continue;
        if (!world_to_screen(cam, w, h, viewport_pos, wb, sb)) continue;
        if (!world_to_screen(cam, w, h, viewport_pos, wc, sc)) continue;
        // Centroid distance first (cheap pre-filter).
        const ImVec2 centroid = ImVec2((sa.x + sb.x + sc.x) / 3.0f, (sa.y + sb.y + sc.y) / 3.0f);
        const float cd = std::hypotf(mouse.x - centroid.x, mouse.y - centroid.y);
        if (cd > 24.0f) continue;
        // Point-in-triangle test (screen space) to require an actual hit.
        const float d1 = (sb.x - sa.x) * (mouse.y - sa.y) - (sb.y - sa.y) * (mouse.x - sa.x);
        const float d2 = (sc.x - sb.x) * (mouse.y - sb.y) - (sc.y - sb.y) * (mouse.x - sb.x);
        const float d3 = (sa.x - sc.x) * (mouse.y - sc.y) - (sa.y - sc.y) * (mouse.x - sc.x);
        const bool has_neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
        const bool has_pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
        if (has_neg && has_pos) continue;  // outside the triangle
        if (cd < best_centroid_d) { best_centroid_d = cd; best = static_cast<int>(f / 3); }
    }
    return best;
}

bool pick_ground_mesh_edge(const av::PODMesh& pm, const float obj_mat[16],
                           const av::Camera& cam, int w, int h,
                           const ImVec2& viewport_pos, const ImVec2& mouse,
                           int& out_v0, int& out_v1) {
    float best_d = 12.0f * 12.0f;  // pick threshold (squared)
    bool found = false;
    for (size_t f = 0; f + 2 < pm.indices.size(); f += 3) {
        const uint32_t a = pm.indices[f], b = pm.indices[f+1], c = pm.indices[f+2];
        if (a >= (uint32_t)pm.num_vertices || b >= (uint32_t)pm.num_vertices ||
            c >= (uint32_t)pm.num_vertices) continue;
        const float la[3] = {pm.positions[a*3], pm.positions[a*3+1], pm.positions[a*3+2]};
        const float lb[3] = {pm.positions[b*3], pm.positions[b*3+1], pm.positions[b*3+2]};
        const float lc[3] = {pm.positions[c*3], pm.positions[c*3+1], pm.positions[c*3+2]};
        float wa[3], wb[3], wc[3];
        local_to_world(obj_mat, la, wa);
        local_to_world(obj_mat, lb, wb);
        local_to_world(obj_mat, lc, wc);
        ImVec2 sa, sb, sc;
        if (!world_to_screen(cam, w, h, viewport_pos, wa, sa)) continue;
        if (!world_to_screen(cam, w, h, viewport_pos, wb, sb)) continue;
        if (!world_to_screen(cam, w, h, viewport_pos, wc, sc)) continue;
        const float dab = point_segment_distance_sq(mouse, sa, sb);
        const float dbc = point_segment_distance_sq(mouse, sb, sc);
        const float dca = point_segment_distance_sq(mouse, sc, sa);
        if (dab < best_d) { best_d = dab; out_v0 = (int)a; out_v1 = (int)b; found = true; }
        if (dbc < best_d) { best_d = dbc; out_v0 = (int)b; out_v1 = (int)c; found = true; }
        if (dca < best_d) { best_d = dca; out_v0 = (int)c; out_v1 = (int)a; found = true; }
    }
    return found;
}

// ============================================================================
// Object picking (screen-space, touch friendly)
// ============================================================================

int pick_scene_object(const std::vector<av::SceneObject>& objects,
                      const std::map<std::string, av::PODModel>* model_cache,
                      bool show_hidden, const av::Camera& cam, int w, int h,
                      const ImVec2& viewport_pos, const ImVec2& mouse) {
    int best = -1;
    float best_depth = 1e30f;

    for (int index = 0; index < (int)objects.size(); ++index) {
        const auto& obj = objects[index];
        if (obj.hidden && !show_hidden) continue;

        // World-space center + bounds radius of the object.
        float center[3] = {obj.pos_x, obj.pos_y, obj.pos_z};
        float radius = 0.0f;
        bool has_geometry = false;

        const std::string model_name = obj.mesh_name.empty() ? obj.background_name : obj.mesh_name;
        if (model_cache) {
            const auto it = model_cache->find(model_name);
            if (it != model_cache->end()) {
                const float s = std::abs(obj.scale_x * obj.template_scaling);
                center[0] += it->second.center_x * s;
                center[1] += it->second.center_y * s;
                center[2] += it->second.center_z * s;
                radius = std::max(radius, it->second.radius * s);
                has_geometry = true;
            }
        }
        for (const auto& gm : obj.ground_meshes) {
            const float s = std::abs(obj.scale_x * obj.template_scaling);
            const float cx = (gm.min_x + gm.max_x) * 0.5f * s + obj.pos_x;
            const float cy = (gm.min_y + gm.max_y) * 0.5f * s + obj.pos_y;
            const float cz = (gm.min_z + gm.max_z) * 0.5f * s + obj.pos_z;
            const float dx = (gm.max_x - gm.min_x) * 0.5f * s;
            const float dy = (gm.max_y - gm.min_y) * 0.5f * s;
            const float dz = (gm.max_z - gm.min_z) * 0.5f * s;
            const float r = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (r > radius) { center[0] = cx; center[1] = cy; center[2] = cz; radius = r; }
            has_geometry = true;
        }

        // Project center; proxy-only objects use a comfortable fixed radius.
        ImVec2 sp;
        if (!world_to_screen(cam, w, h, viewport_pos, center, sp)) continue;
        float rad_px = has_geometry ? 12.0f : 16.0f;
        if (radius > 0.0f) {
            float right[3], up[3], fwd[3];
            camera_basis(cam, right, up, fwd);
            const float probe[3] = {center[0] + right[0]*radius, center[1] + right[1]*radius,
                                    center[2] + right[2]*radius};
            ImVec2 sp2;
            if (world_to_screen(cam, w, h, viewport_pos, probe, sp2))
                rad_px = std::max(rad_px, std::hypotf(sp2.x - sp.x, sp2.y - sp.y));
        }
        const float d = std::hypotf(mouse.x - sp.x, mouse.y - sp.y);
        if (d > rad_px) continue;

        // Prefer the candidate nearest the eye (z-ordering).
        float right[3], up[3], fwd[3];
        camera_basis(cam, right, up, fwd);
        const float yaw   = cam.yaw * kPi / 180.0f;
        const float pitch = cam.pitch * kPi / 180.0f;
        const float cp = cosf(pitch);
        const float eye[3] = {
            cam.target[0] + cam.distance * cp * sinf(yaw),
            cam.target[1] + cam.distance * sinf(pitch),
            cam.target[2] + cam.distance * cp * cosf(yaw)
        };
        const float dxc = center[0]-eye[0], dyc = center[1]-eye[1], dzc = center[2]-eye[2];
        const float depth = dxc*dxc + dyc*dyc + dzc*dzc;
        if (depth < best_depth) { best_depth = depth; best = index; }
    }
    return best;
}

} // namespace swk
