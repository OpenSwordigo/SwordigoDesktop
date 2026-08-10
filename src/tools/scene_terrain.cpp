/* scene_terrain.cpp — terrain heightfield implementation. See scene_terrain.h. */
#include "tools/scene_terrain.h"

#include "tools/scene_workspace.h"   // swk::object_world_matrix (renderer transform)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Transform a mesh-local vertex into world space with the exact matrix the
// visualizer uses (swk::object_world_matrix: T * Rz(rot) * S where S includes
// scale_x/y/z × template_scaling).  Guessing a simplified pos+scale*scale_x
// transform here produced heights thousands of units off (mobs "in the air").
static void obj_local_to_world(const av::SceneObject& obj, const float v[3],
                               float out[3]) {
    float m[16];
    swk::object_world_matrix(obj, m);
    out[0] = m[0] * v[0] + m[4] * v[1] + m[8]  * v[2] + m[12];
    out[1] = m[1] * v[0] + m[5] * v[1] + m[9]  * v[2] + m[13];
    out[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14];
}

namespace av {

namespace {

// Insert a surface height into a cell's ascending list, deduping near-equal
// values (>= 24 units apart are distinct levels: floor, bridge, ceiling).
static void insert_surface(TerrainGrid& g, size_t cell, float y) {
    float* list = &g.surf[cell * TerrainGrid::kMaxSurfaces];
    int n = g.cnt[cell];
    for (int i = 0; i < n; ++i)
        if (std::fabsf(list[i] - y) < 24.0f) {
            if (y > list[i]) list[i] = y;   // keep the top of near-equal level
            return;
        }
    if (n >= TerrainGrid::kMaxSurfaces)
        return;   // full — keep the kMaxSurfaces most common levels

    list[n] = y;
    ++g.cnt[cell];
    std::sort(list, list + n);
}

// Insert an UNDERSIDE / ceiling height (down-facing surface) into the cell's
// ascending ceiling list. Same dedup rule as insert_surface but keeps the
// LOWEST of near-equal levels (the true bottom plane of a mesh body).
static void insert_ceiling(TerrainGrid& g, size_t cell, float y) {
    float* list = &g.ceil[cell * TerrainGrid::kMaxSurfaces];
    int n = g.ceil_cnt[cell];
    for (int i = 0; i < n; ++i)
        if (std::fabsf(list[i] - y) < 24.0f) {
            if (y < list[i]) list[i] = y;   // keep the lowest of near-equal
            return;
        }
    if (n >= TerrainGrid::kMaxSurfaces)
        return;
    list[n] = y;
    ++g.ceil_cnt[cell];
    std::sort(list, list + n);
}

// Rasterize one triangle (world-space verts): for every cell whose center
// projects inside the triangle on the X-Z plane, insert the surface Y
// (barycentric interpolation → real slopes). Up-facing triangles go into the
// walkable surface list; down-facing triangles (undersides) go into the
// ceiling list so a rising head bumps the true bottom plane of a mesh.
static void rasterize_triangle(TerrainGrid& g, const float a[3], const float b[3],
                               const float c[3]) {
    // Face orientation: only UP-facing (normal.y > 0.3) are walkable ground;
    // DOWN-facing (normal.y < -0.3) are underside ceilings.
    float nx = (b[1]-a[1])*(c[2]-a[2]) - (b[2]-a[2])*(c[1]-a[1]);
    float ny = (b[2]-a[2])*(c[0]-a[0]) - (b[0]-a[0])*(c[2]-a[2]);
    float nz = (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]);
    const float nl = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (nl < 1e-6f) return;
    const bool is_ceiling = (ny / nl) < -0.3f;
    if (!is_ceiling && (ny / nl) < 0.3f) return;   // vertical side — neither

    const float xmin = std::min({a[0], b[0], c[0]});
    const float xmax = std::max({a[0], b[0], c[0]});
    const float zmin = std::min({a[2], b[2], c[2]});
    const float zmax = std::max({a[2], b[2], c[2]});

    int c0 = (int)std::floor((xmin - g.min_x) / g.cell);
    int c1 = (int)std::floor((xmax - g.min_x) / g.cell);
    int r0 = (int)std::floor((zmin - g.min_z) / g.cell);
    int r1 = (int)std::floor((zmax - g.min_z) / g.cell);
    c0 = std::max(0, c0); c1 = std::min(g.cols - 1, c1);
    r0 = std::max(0, r0); r1 = std::min(g.rows - 1, r1);
    if (c0 > c1 || r0 > r1) return;

    const float denom = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (std::fabs(denom) < 1e-9f) return;

    for (int r = r0; r <= r1; ++r) {
        const float pz = g.min_z + ((float)r + 0.5f) * g.cell;
        for (int cc = c0; cc <= c1; ++cc) {
            const float px = g.min_x + ((float)cc + 0.5f) * g.cell;
            const float u = ((b[2]-c[2])*(px-c[0]) + (c[0]-b[0])*(pz-c[2])) / denom;
            const float v = ((c[2]-a[2])*(px-c[0]) + (a[0]-b[0])*(pz-c[2])) / denom;
            const float w = 1.0f - u - v;
            if (u < -1e-4f || v < -1e-4f || w < -1e-4f) continue;
            const float y = u * a[1] + v * b[1] + w * c[1];
            const size_t cell = (size_t)r * g.cols + (size_t)cc;
            if (is_ceiling) insert_ceiling(g, cell, y);
            else            insert_surface(g, cell, y);
        }
    }
}

// Top-edge Y of a 2D polygon (side-view profile) at world X. Points are flat
// {x,y,…} in world space. Returns false when X is outside the polygon.
static bool polygon_top_y(const std::vector<float>& pts, float x, float& top) {
    if (pts.size() < 6) return false;   // need at least a triangle
    bool any = false;
    float best = -1e18f;
    const size_t n = pts.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        const float xi = pts[i * 2], yi = pts[i * 2 + 1];
        const float xj = pts[((i + 1) % n) * 2], yj = pts[((i + 1) % n) * 2 + 1];
        if ((xi <= x && xj > x) || (xj <= x && xi > x)) {
            const float y = yi + (x - xi) * (yj - yi) / (xj - xi);
            if (y > best) best = y;
            any = true;
        }
    }
    if (!any) return false;
    top = best;
    return true;
}

// Bottom-edge Y of a 2D polygon profile at world X — the underside a rising
// head bumps. Same crossing test, keeps the LOWEST edge instead of the top.
static bool polygon_bottom_y(const std::vector<float>& pts, float x, float& bot) {
    if (pts.size() < 6) return false;
    bool any = false;
    float best = 1e18f;
    const size_t n = pts.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        const float xi = pts[i * 2], yi = pts[i * 2 + 1];
        const float xj = pts[((i + 1) % n) * 2], yj = pts[((i + 1) % n) * 2 + 1];
        if ((xi <= x && xj > x) || (xj <= x && xi > x)) {
            const float y = yi + (x - xi) * (yj - yi) / (xj - xi);
            if (y < best) best = y;
            any = true;
        }
    }
    if (!any) return false;
    bot = best;
    return true;
}

} // namespace

void terrain_build(const SceneData& scene, TerrainGrid& grid) {
    grid = TerrainGrid{};
    if (scene.objects.empty()) return;

    // Scene bounds → grid extents (pad by one cell).
    const float span_x = scene.bounds_max[0] - scene.bounds_min[0];
    const float span_z = scene.bounds_max[2] - scene.bounds_min[2];
    if (span_x < 1.0f && span_z < 1.0f) return;
    grid.min_x = scene.bounds_min[0] - grid.cell;
    grid.min_z = scene.bounds_min[2] - grid.cell;
    grid.cols = std::max(1, (int)std::ceil((span_x + 2.0f * grid.cell) / grid.cell));
    grid.rows = std::max(1, (int)std::ceil((span_z + 2.0f * grid.cell) / grid.cell));
    grid.surf.assign((size_t)grid.cols * grid.rows * TerrainGrid::kMaxSurfaces, -1e30f);
    grid.cnt.assign((size_t)grid.cols * grid.rows, 0);
    grid.ceil.assign((size_t)grid.cols * grid.rows * TerrainGrid::kMaxSurfaces, -1e30f);
    grid.ceil_cnt.assign((size_t)grid.cols * grid.rows, 0);

    for (const auto& obj : scene.objects) {
        const float ox = obj.pos_x, oy = obj.pos_y, oz = obj.pos_z;
        for (const auto& mesh : obj.ground_meshes) {
            const auto& pos = mesh.positions;
            const auto& idx = mesh.indices;
            if (pos.empty() || idx.size() < 3) continue;
            const size_t ntri = idx.size() / 3;
            for (size_t t = 0; t < ntri; ++t) {
                const uint32_t ia = idx[t * 3], ib = idx[t * 3 + 1], ic = idx[t * 3 + 2];
                if (ia * 3 + 2 >= pos.size() || ib * 3 + 2 >= pos.size() ||
                    ic * 3 + 2 >= pos.size())
                    continue;
                const float va[3] = {pos[ia*3], pos[ia*3+1], pos[ia*3+2]};
                const float vb[3] = {pos[ib*3], pos[ib*3+1], pos[ib*3+2]};
                const float vc[3] = {pos[ic*3], pos[ic*3+1], pos[ic*3+2]};
                float a[3], b[3], c[3];
                obj_local_to_world(obj, va, a);
                obj_local_to_world(obj, vb, b);
                obj_local_to_world(obj, vc, c);
                rasterize_triangle(grid, a, b, c);
            }
        }

        // 2) GroundPolygonComponent: 2.5D walkable profile across its depth
        //    range.  Transform the profile points with the object matrix.
        if (!obj.ground_polygon_points.empty()) {
            std::vector<float> world;
            world.reserve(obj.ground_polygon_points.size());
            for (size_t i = 0; i + 1 < obj.ground_polygon_points.size(); i += 2) {
                const float v[3] = {obj.ground_polygon_points[i],
                                    obj.ground_polygon_points[i + 1], 0.0f};
                float w[3];
                obj_local_to_world(obj, v, w);
                world.push_back(w[0]);
                world.push_back(w[1]);
            }
            // Depth range in world space via the object matrix (scale/rot).
            float z0 = grid.min_z, z1 = grid.min_z + grid.rows * grid.cell;
            if (obj.ground_polygon_min_depth > -1e8f ||
                obj.ground_polygon_max_depth < 1e8f) {
                float v0[3] = {0.0f, 0.0f, obj.ground_polygon_min_depth};
                float v1[3] = {0.0f, 0.0f, obj.ground_polygon_max_depth};
                float w0[3], w1[3];
                obj_local_to_world(obj, v0, w0);
                obj_local_to_world(obj, v1, w1);
                if (obj.ground_polygon_min_depth > -1e8f) z0 = w0[2];
                if (obj.ground_polygon_max_depth < 1e8f) z1 = w1[2];
            }
            if (z0 > z1) std::swap(z0, z1);
            const int r0 = std::max(0, (int)std::floor((z0 - grid.min_z) / grid.cell));
            const int r1 = std::min(grid.rows - 1, (int)std::floor((z1 - grid.min_z) / grid.cell));
            for (int r = r0; r <= r1; ++r) {
                for (int c = 0; c < grid.cols; ++c) {
                    const float x = grid.min_x + ((float)c + 0.5f) * grid.cell;
                    float top = 0.0f, bot = 0.0f;
                    if (polygon_top_y(world, x, top))
                        insert_surface(grid, (size_t)r * grid.cols + (size_t)c, top);
                    // Underside: the polygon's bottom edge is a ceiling for
                    // entities rising under the platform (a 2.5D solid
                    // profile — jumping up beneath it bumps the head).
                    if (polygon_bottom_y(world, x, bot))
                        insert_ceiling(grid, (size_t)r * grid.cols + (size_t)c, bot);
                }
            }
        }
    }
}

static bool cell_at(const TerrainGrid& grid, float x, float z,
                    size_t& cell) {
    if (grid.cols <= 0 || grid.rows <= 0) return false;
    const int c = (int)std::floor((x - grid.min_x) / grid.cell);
    const int r = (int)std::floor((z - grid.min_z) / grid.cell);
    if (c < 0 || c >= grid.cols || r < 0 || r >= grid.rows) return false;
    cell = (size_t)r * grid.cols + (size_t)c;
    return true;
}

bool terrain_height_at(const TerrainGrid& grid, float x, float z, float& h) {
    size_t cell;
    if (!cell_at(grid, x, z, cell)) return false;
    const int n = grid.cnt[cell];
    if (n <= 0) return false;
    const float* list = &grid.surf[cell * TerrainGrid::kMaxSurfaces];
    h = list[n - 1];   // highest stored surface
    return true;
}

bool terrain_height_near(const TerrainGrid& grid, float x, float z,
                         float ref_y, float step, float& h) {
    size_t cell;
    if (!cell_at(grid, x, z, cell)) return false;
    const int n = grid.cnt[cell];
    if (n <= 0) return false;
    const float* list = &grid.surf[cell * TerrainGrid::kMaxSurfaces];
    // Highest surface at-or-below the feet (+step): that is the floor under
    // the entity.  A bridge/ceiling 200+ units up must NOT count.
    for (int i = n - 1; i >= 0; --i) {
        if (list[i] <= ref_y + step) { h = list[i]; return true; }
    }
    // Nothing reachable below feet within step — check for a floor very
    // close above (gentle slopes up), then give up (free fall).
    if (n > 0 && list[0] - ref_y < step * 2.0f) { h = list[0]; return true; }
    return false;
}

bool terrain_ceiling_near(const TerrainGrid& grid, float x, float z,
                          float ref_y, float step, float& h) {
    size_t cell;
    if (!cell_at(grid, x, z, cell)) return false;
    const int n = grid.ceil_cnt[cell];
    if (n <= 0) return false;
    const float* list = &grid.ceil[cell * TerrainGrid::kMaxSurfaces];
    // Undersides are stored ascending per cell: the FIRST at-or-above the
    // reference is the lowest thing above the entity — the ceiling. This is
    // the true BOTTOM plane of a mesh/polygon body (down-facing triangles +
    // polygon bottom edges), so a head rising under a platform bumps at the
    // underside instead of tunnelling through to the surface top.
    for (int i = 0; i < n; ++i) {
        if (list[i] >= ref_y && list[i] <= ref_y + step) { h = list[i]; return true; }
    }
    return false;
}

} // namespace av
