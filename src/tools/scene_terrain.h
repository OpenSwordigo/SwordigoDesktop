#pragma once
/* scene_terrain.h — Swordigo terrain heightfield sampler.
 *
 * The scene player needs entities to STAND on the real ground, not on a
 * hardcoded spawn height. This module bakes a coarse heightfield from:
 *
 *   1. GroundMesh meshes — every object's ground_meshes (PODMesh) triangles
 *      are rasterized into a grid. Because Swordigo scenes are multi-level
 *      (floors, bridges and platforms stack in the same XZ cell), each cell
 *      stores up to kMaxSurfaces distinct surface heights instead of a single
 *      max.  Only UP-FACING triangles count as walkable ground — wall sides
 *      and undersides must never read as "floor" (that glued mobs to the
 *      tops of walls, hundreds of units above the real floor).
 *
 *   2. GroundPolygonComponent profiles — the game's 2.5D walkable surfaces.
 *      A ground polygon is a flat shape in the X-Y (side-view) plane with a
 *      depth (Z) range; the walkable height at a given X is the polygon's
 *      TOP edge. These give precise platform/slope surfaces that the low-res
 *      mesh raster alone may miss.
 *
 * Sampling: entities rest on the surface closest to their current feet Y
 * (within a step tolerance) — exactly how the real engine resolves standing
 * on a floor while a bridge passes overhead in the same cell.
 *
 * Built once per play session in player_begin; sampled every tick.
 */
#ifndef SCENE_TERRAIN_H_
#define SCENE_TERRAIN_H_

#include <vector>
#include "tools/scene_loader.h"

namespace av {

struct TerrainGrid {
    static constexpr int kMaxSurfaces = 4;   // surfaces stacked per cell
    float min_x = 0.0f, min_z = 0.0f;
    float cell = 8.0f;             // world units per cell
    int   cols = 0, rows = 0;
    // Walkable surface TOPS (up-facing triangles + ground-polygon tops),
    // ascending per cell. This is what entities stand on.
    std::vector<float>  surf;      // cols*rows*kMaxSurfaces (ascending per cell)
    std::vector<uint8_t> cnt;      // cols*rows : number of stored surfaces
    // Underside / CEILING surfaces (down-facing triangles + ground-polygon
    // bottoms), ascending per cell. This is what a rising head bumps.
    // Stored separately so a thick mesh's underside bumps the head at the
    // true bottom plane instead of tunnelling through to the top.
    std::vector<float>  ceil;      // cols*rows*kMaxSurfaces (ascending per cell)
    std::vector<uint8_t> ceil_cnt; // cols*rows : number of stored ceilings
};

/// Build a heightfield from every ground mesh + ground polygon in the scene.
void terrain_build(const SceneData& scene, TerrainGrid& grid);

/// Highest walkable surface at (x, z). Returns false when no terrain covers
/// the point (caller should fall back to the spawn height).
bool terrain_height_at(const TerrainGrid& grid, float x, float z, float& h);

/// Height of the walkable surface closest to the reference feet height
/// @p ref_y at (x, z), ignoring surfaces more than @p step above ref_y
/// (an entity on the ground floor must not snap up onto a passing bridge).
/// Returns false when no surface is within reach (free fall / no ground).
bool terrain_height_near(const TerrainGrid& grid, float x, float z,
                         float ref_y, float step, float& h);

/// Height of the LOWEST underside at-or-above @p ref_y within @p step — the
/// ceiling under which an entity is rising. Undersides are stored from
/// down-facing ground-mesh triangles and ground-polygon bottom edges, so a
/// head rising under a platform bumps at its true BOTTOM plane (previously
/// only the surface top was stored and the head tunnelled through the whole
/// mesh before bumping at the top). Returns false when nothing is in reach.
bool terrain_ceiling_near(const TerrainGrid& grid, float x, float z,
                          float ref_y, float step, float& h);

} // namespace av

#endif // SCENE_TERRAIN_H_
