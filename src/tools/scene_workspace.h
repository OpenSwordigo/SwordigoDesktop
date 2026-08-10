/* scene_workspace.h — Pure helpers for the Ruby scene editor viewport.
 *
 * Self-contained math + geometry + editing primitives used by the scene
 * visualizer (asset_viewer.cpp).  No ViewerState, no globals: every function
 * takes the data it needs as parameters, so the module is easy to reason
 * about and unit-test independently of the UI.
 */
#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <map>
#include <string>

#include "av_renderer.h"
#include "scene_loader.h"
#include "imgui.h"

namespace swk {

// ============================================================================
// Camera / projection math
// ============================================================================

/// Camera local axes (right / up / forward) from yaw-pitch orbit state.
void camera_basis(const av::Camera& cam, float right[3], float up[3], float forward[3]);

/// Project a world-space point to viewport pixels.  Returns false when the
/// point is behind the camera or outside the NDC window.
bool world_to_screen(const av::Camera& cam, int w, int h, const ImVec2& viewport_pos,
                     const float world[3], ImVec2& out);

/// Unproject a screen point into a world-space ray (origin + normalized dir).
void screen_ray(const av::Camera& cam, int w, int h, const ImVec2& viewport_pos,
                const ImVec2& screen, float origin[3], float dir[3]);

/// Ray / horizontal-plane intersection (false when parallel).
bool ray_plane_y(const float origin[3], const float dir[3], float plane_y, float out[3]);

// ============================================================================
// Scene-object / mesh geometry
// ============================================================================

/// Build the object world matrix (T * Rz(rot) * S) — mirrors the renderer.
void object_world_matrix(const av::SceneObject& obj, float out[16]);

/// Render matrix = object_world_matrix + the ModelComponent's baked Y-rotation
/// (main.js addModel rotation.y). Use for draw passes; keep object_world_matrix
/// for gizmo/picking so editing stays stable.
void object_render_matrix(const av::SceneObject& obj, float out[16]);

/// Flat-shade normals + recompute the AABB of an edited ground mesh.
void recompute_ground_mesh_geometry(av::PODMesh& pm);

/// Snap a value to the nearest grid step.
float snap_value(float value, float step);

/// Delete a vertex from a ground mesh and remap indices (dropping its
/// triangles).  Returns false when the mesh would become degenerate.
bool ground_mesh_delete_vertex(av::PODMesh& pm, int vertex_idx);

/// Remove triangle @p tri (0-based) — erases its three indices.  Returns
/// false when the triangle index is out of range.
bool ground_mesh_delete_triangle(av::PODMesh& pm, int tri);

/// Insert a new vertex at the centroid of triangle @p tri, splitting it into
/// three triangles.  The new vertex inherits the averaged normal/uv.  Returns
/// the new vertex index, or -1 when the triangle index is invalid.
int ground_mesh_subdivide_triangle(av::PODMesh& pm, int tri);

/// Insert a new vertex at the midpoint of the edge (@p v0, @p v1), splitting
/// every triangle that uses the edge into two (winding preserved).  Returns
/// the new vertex index, or -1 when the edge is not part of any triangle.
int ground_mesh_split_edge(av::PODMesh& pm, int v0, int v1);

/// Screen-space pick helpers for the ground-mesh editor.  Both transform the
/// mesh by @p obj_mat (the object world matrix) before projecting.
///
/// Pick the triangle under the cursor (front-most wins).  Returns a 0-based
/// triangle index or -1.
int pick_ground_mesh_triangle(const av::PODMesh& pm, const float obj_mat[16],
                              const av::Camera& cam, int w, int h,
                              const ImVec2& viewport_pos, const ImVec2& mouse);

/// Pick the edge under the cursor.  Returns true and fills @p out_v0/@p out_v1
/// when an edge within the pick threshold is found.
bool pick_ground_mesh_edge(const av::PODMesh& pm, const float obj_mat[16],
                           const av::Camera& cam, int w, int h,
                           const ImVec2& viewport_pos, const ImVec2& mouse,
                           int& out_v0, int& out_v1);

// ============================================================================
// Mesh normals
// ============================================================================

/// Compute smooth (area-weighted, normalized) vertex normals from positions +
/// indices. When @p indices is empty the vertex stream is treated as a plain
/// triangle list (3 vertices per triangle).
///
/// Fills @p out_normals with 3 floats per vertex and returns the number of
/// vertices processed (0 on empty/invalid input). Callers upload these when a
/// mesh carries no baked normals — otherwise the renderer lights every
/// surface as Y-up, which is what makes stone read flat and walls lose their
/// form.
int compute_smooth_normals(const std::vector<float>& positions,
                           const std::vector<uint32_t>& indices,
                           std::vector<float>& out_normals);

// ============================================================================
// Object picking (screen-space, touch friendly)
// ============================================================================

/// Pick the scene object under the mouse.
///
/// Objects are projected to the screen; an object is a candidate when the
/// mouse is within its projected bounds radius (at least 12 px so proxies and
/// distant objects stay clickable).  The closest candidate along the view ray
/// wins.  @p model_cache maps POD names to loaded models (may be null).
int pick_scene_object(const std::vector<av::SceneObject>& objects,
                      const std::map<std::string, av::PODModel>* model_cache,
                      bool show_hidden, const av::Camera& cam, int w, int h,
                      const ImVec2& viewport_pos, const ImVec2& mouse);

} // namespace swk
