#pragma once
/*
 * scene_generator_v2_3d.h — Swordigo "v2-3d" full-3D procedural world generator.
 *
 * Ground-up Minecraft-style terrain for the Swordigo scene format, built on
 * measured engine + vanilla-scene evidence (see scene_generator_v2_3d.cpp
 * header comment). v2-3d synthesizes ONE continuous 2D heightfield H(x, z) —
 * X is the walk direction, Z is true world depth — and tiles it into GROUND
 * ROWS on both sides of the hero plane:
 *
 * THE X-AXIS SEAM SYSTEM, RECREATED ON THE Z AXIS
 *   v1/v2 slice a heightfield into strips that SHARE their boundary sample
 *   (strip p's s1 == strip p+1's s0 → identical edge vertices → seamless X).
 *   v2-3d recreates that system along Z: rows tile the depth axis on a
 *   uniform grid — row k is centred at z = ±2k·row_band with extrusion band
 *   ±row_band, so row k spans [(2k−1)·B, (2k+1)·B] and adjacent rows SHARE
 *   their boundary depth plane exactly, the same way adjacent strips share a
 *   boundary sample in X. Block-quantized column heights make the Z-steps
 *   clean voxel terraces: the whole world is one continuous extruded volume
 *   (a Minecraft chunk cross-section), not floating parallax layers.
 *
 *   • gameplay row  z = 0, band ±row_band — walkable slice the hero runs on
 *     (hero plane 0 ∈ [−B, B] → collides; every other row's band excludes 0).
 *   • depth rows    z = −2k·B, k = 1..depth_rows (default 15 → z = −2700):
 *     the world rolling away behind the hero plane, perspective-shrunk.
 *   • front rows    z = +2j·B, j = 1..front_rows (default 5 → z = +900):
 *     the excavated front face of the world — terraces descending toward the
 *     camera, tops perspective-capped below the gameplay floor so the hero
 *     stays visible (classic side-scroller foreground framing).
 *
 * EVIDENCE (engine + vanilla)
 *   True perspective renderer: FOV 20°, camera at z = 2835.6 → apparent size
 *   ∝ 2835.6/(2835.6 − z); GroundMesh tops are horizontal quads extruded
 *   across [min_depth, max_depth] (boulder.cpp). Vanilla: gameplay bands
 *   ±70..±100, SurfaceWidth 130–190, far ground meshes at z ≈ −1958, near-
 *   flat walkable tops (median Y-variation 28–44), decoration density
 *   2.5–3.7 objects / 1000u.
 *
 * CAMERA: v1 semantics exactly — v2's camera_follow_y_shape emission is known
 * buggy, so v2-3d emits NO camera shapes and derives the root Bounds as v1
 * does: plain AABB of the walkable geometry + fixed padding.
 *
 * v1/v2 files are untouched; this generator only links their public builders.
 */
#include "tools/scene_generator.h"
#include <string>
#include <vector>
#include <cstdint>

namespace sgen {
namespace v2_3d {

// ── v2-3d world options ──────────────────────────────────────────────────────
struct TerrainOptions3D : public TerrainOptions {
    // ── Z grid (the X-seam system recreated in depth) ────────────────────────
    int   depth_rows        = 15;     // rows BEHIND the hero plane (z < 0)
    int   front_rows        = 5;      // rows IN FRONT of the hero plane (z > 0)
    float row_band          = 90.0f;  // half extrusion per row; rows tile Z at
                                      // 2·row_band (adjacent rows share the
                                      // boundary plane — the Z seam)

    // ── Terrain style ─────────────────────────────────────────────────────────
    // Minecraft is the STYLE (a full procedural world on both sides of the
    // hero plane), not the geometry: the default world is SMOOTH — gentle
    // low-level elevations and depressions, Gaussian-smoothed. Blocky voxel
    // staircases are opt-in for those who want them.
    float block_size        = 44.0f;  // (blocky mode) block column size
    bool  blocky            = false;  // false = smooth rolling world
    int   min_run_cols      = 5;      // (blocky mode) flat run per plateau
    int   max_cliff_blocks  = 3;      // (blocky mode) max cliff before benches
    float smoothness        = 1.0f;   // (smooth mode) smoothing pass multiplier

    // ── Caves / ravines ──────────────────────────────────────────────────────
    bool  add_caves         = true;   // winding carved canyons
    float cave_depth        = 150.0f; // canyon floor drop (≤ hero jump!)
    float cave_width        = 0.06f;  // |noise| threshold — canyon width

    // ── Floating sky islands ─────────────────────────────────────────────────
    bool  sky_islands       = true;   // blocky floating islands (depth rows)
    float island_density    = 0.42f;  // island-noise threshold (lower = rarer)

    // ── Scatter (vanilla-calibrated; combined ≈3.5 decos / 1000u) ────────────
    bool  scatter_far_trees = true;   // forest the depth rows (perspective comp.)
};

// Generate a full v2-3d Minecraft-style world as a complete .scene.
Result generate_biome_scene_v2_3d(const TerrainOptions3D& opt);

} // namespace v2_3d
} // namespace sgen
