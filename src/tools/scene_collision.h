#pragma once
/* scene_collision.h — Swordigo collision subsystem data structures
 *
 * Faithfully reconstructed from:
 *   - IDA decompilations:
 *       Caver::CollisionShapeComponent @ 0x1FA400 (LoadFromProtobufMessage)
 *       Caver::BoneControlledCollisionShapeComponent
 *       Caver::CollisionShape (SetUpdatedShape, UpdateWorldAABB)
 *       Caver::IsCollisionNormalValidForPolygonLineSegment @ 0x1FD560
 *   - scene_schemas.cpp Component schema field numbers:
 *       {962,  "ShapeComponent"}                     → payload field_number = 120
 *       {970,  "CollisionShapeComponent"}             → payload field_number = 121
 *       {994,  "BoneControlledCollisionShapeComponent"} → payload field_number = 124
 *       {882,  "GroundPolygonComponent"}              → payload field_number = 110
 *   - ShapeComponent schema: field 10 = Rectangle, field 18 = Circle, field 26 = Polygon
 *   - Rectangle schema: field 13=X, field 21=Y, field 29=Width, field 37=Height (I32/float)
 *   - Circle schema: field 10 = Center (Vector2), field 21 = Radius (I32/float)
 *   - CollisionShapeComponent schema:
 *       field 16 = IsGround (VARINT/bool), field 24 = Collides (VARINT/bool)
 *       field 32 = ReceivesDamage (VARINT), field 40 = InflictsDamage (VARINT)
 *       field 53 = MinDepth (I32/float), field 61 = MaxDepth (I32/float)
 *       field 64 = SpecialType (VARINT), field 88 = Enabled (VARINT/bool)
 *       field 109 = Friction (I32/float), field 112 = UnsafeGround (VARINT)
 *   - GroundPolygonComponent schema:
 *       field 10 = Vertex (Vector2, repeated), field 18 = Polygon (nested)
 *       field 24 = Collides (VARINT), field 37 = MinDepth (I32), field 45 = MaxDepth (I32)
 *       field 61 = Friction (I32), field 64 = UnsafeGround (VARINT)
 *   - BoneControlledCollisionShapeComponent schema:
 *       field 16 = CollisionShapeId, field 24 = BoneControllerComponentId
 */

#include <string>
#include <vector>

namespace av {
struct SceneObject;   // forward decl — full definition lives in scene_loader.h
struct SceneData;     // forward decl — full definition lives in scene_loader.h

// ─── Collision shape geometry types ──────────────────────────────────────
enum CollisionShapeType {
    COLL_NONE    = 0,
    COLL_RECT    = 1,  // ShapeComponent field 10 (Rectangle)
    COLL_CIRCLE  = 2,  // ShapeComponent field 18 (Circle)
    COLL_POLYGON = 3,  // ShapeComponent field 26 (Polygon) or GroundPolygonComponent
};

// ─── One collision shape (geometry + behavior flags) ─────────────────────
struct CollisionShapeData {
    CollisionShapeType type = COLL_NONE;

    // COLL_RECT: bounding rectangle in object-local space
    float rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // x, y, width, height

    // COLL_CIRCLE
    float circle_center[2] = {0.0f, 0.0f};  // cx, cy in object-local space
    float circle_radius    = 0.0f;

    // COLL_POLYGON: flat {x0,y0, x1,y1, ...} CCW winding (object-local)
    std::vector<float> polygon_points;

    // CollisionShapeComponent behavior flags
    bool  is_ground        = false;  // field 16 : walkable ground surface
    bool  collides         = true;   // field 24 : solid collision
    bool  receives_damage  = false;  // field 32 : can take hits (hitbox)
    bool  inflicts_damage  = false;  // field 40 : deals hits (hurtbox / weapon)
    bool  enabled          = true;   // field 88 : runtime enable switch
    bool  unsafe_ground    = false;  // field 112: kills entity on contact
    float min_depth        = -1e9f;  // field 53 : Z-range min (depth clipping)
    float max_depth        =  1e9f;  // field 61 : Z-range max
    float friction         = 0.0f;   // field 109: surface friction coefficient
    int   special_type     = 0;      // field 64 : SpecialType enum

    // BoneControlledCollisionShapeComponent: shape tracks a skeleton bone
    bool        bone_controlled    = false;
    std::string bone_controller_id;  // BoneControllerComponentId (string)
    std::string collision_shape_id; // CollisionShapeId (string)
};

// ─── All collision shapes on one SceneObject ─────────────────────────────
struct CollisionData {
    std::vector<CollisionShapeData> shapes;

    // GroundPolygonComponent: walkable/collidable ground surface polygon(s)
    // stored separately because they describe mesh surface topology, not
    // entity hitbox regions.
    struct GroundPolygon {
        std::vector<float> points;  // flat {x0,y0, x1,y1, ...} object-local
        bool  collides      = true;
        bool  unsafe_ground = false;
        float friction      = 0.0f;
        float min_depth     = -1e9f;
        float max_depth     =  1e9f;
    };
    std::vector<GroundPolygon> ground_polygons;
};

/// Parse all collision shapes and ground polygons from a SceneObject.
/// Returns CollisionData with empty shapes/ground_polygons if none found.
CollisionData collision_parse(const SceneObject& obj);

// ============================================================================
// RUNTIME COLLISION WORLD — the "intelligent wall" subsystem
// ============================================================================
// Reconstructed from Caver::CollisionShape, CollisionShapeComponent,
// GroundPolygonComponent, OrientedRect and the generic shape-vs-polygon
// resolvers (RectangleIntersectsPolygon @0x4AC5A0, CircleIntersectsPolygon
// @0x4ACC90, IsCollisionNormalValidForPolygonLineSegment @0x4AC2A8).
//
// A CollisionWorld is BUILT from a parsed scene (parses scenes → generates
// clean, visible, world-space collision walls) and is what the Scene Player
// maps over the scene while playing: solid walls block, ground polygons are
// walkable surfaces, unsafe walls hurt, and every wall is a drawable line
// segment so the player can SEE the collision layout.

// One world-space wall edge. Swordigo collision is 2.5D: walls live in the
// X-Y (side-view) plane with a Z (depth) range they are active for.
struct WallSegment {
    float  a[2] = {0, 0};        // segment start (world x, y)
    float  b[2] = {0, 0};        // segment end   (world x, y)
    float  z_min = -1e9f, z_max = 1e9f;  // active depth range
    float  nx = 0, ny = 1;       // outward unit normal (up = ground)
    bool   solid = true;         // blocks movement
    bool   ground = false;       // walkable top surface
    bool   unsafe = false;       // kills on touch (lava / spikes)
    bool   one_way = false;      // platform: blocks from above only
    int    source_object = -1;   // owning SceneObject index (debug)
};

struct CollisionWorld {
    std::vector<WallSegment> walls;      // ALL walls (draw + query)
    size_t shape_objects = 0;            // objects that contributed geometry
    size_t segment_count = 0;            // walls after merge/simplify
    float  min_x = 1e30f, max_x = -1e30f;
    float  min_y = 1e30f, max_y = -1e30f;
};

/// Build the world-space wall set from a scene. Intelligent by design:
///   - every CollisionShapeComponent rect → 4 edge segments (OBB), scaled
///     and positioned by the object transform (pos/scale/rotation)
///   - every polygon shape + GroundPolygonComponent edge list → edge
///     segments with outward normals (ground polygons become one-way
///     walkable tops, colliding polygons become solid walls)
///   - circles → 12-gon approximation ring
///   - animated entity hitboxes (BoneControlledCollisionShape on
///     *MonsterController objects) are EXCLUDED — they are dynamic damage
///     volumes, never static walls
///   - collinear adjacent segments with identical flags are merged so the
///     visible wall layout stays clean (no per-triangle seams)
void collision_build_world(const SceneData& scene, CollisionWorld& out);

// ─── Oriented-rect (OBB) SAT primitives — Caver::OrientedRect ports ────
// Layout mirrors the decompiled 32-byte struct: center, 2 basis axes
// (unit, perpendicular), half extents.
struct OrientedRect {
    float c[2]  = {0, 0};   // center
    float u[4]  = {1, 0, 0, 1}; // basis: u0=(u[0],u[1]) u1=(u[2],u[3])
    float he[2] = {0, 0};   // half extents along u0/u1
};

/// OrientedRect::OBFromRectangle — rect {x,y,w,h} → centered OBB (identity
/// basis).
OrientedRect ob_from_rectangle(const float rect[4]);

/// OrientedRect::OBFromTransformedRectangle — rect in space A expressed in
/// space B (both given by position / rotation / scale).
OrientedRect ob_from_transformed_rectangle(const float rect[4],
                                           const float pos_a[2], float rot_a,
                                           float scale_a,
                                           const float pos_b[2], float rot_b,
                                           float scale_b);

/// OrientedRect::SpanOnAxis — projection [lo, hi] of the OBB on an axis.
void ob_span_on_axis(const OrientedRect& ob, const float axis[2],
                     float& lo, float& hi);

/// OrientedRect::IntersectsOrientedRectOnAxis — SAT test on one axis;
/// returns true + the overlap when the two OBBs overlap on it.
bool ob_intersects_on_axis(const OrientedRect& a, const OrientedRect& b,
                           const float axis[2], float& overlap);

/// IsCollisionNormalValidForPolygonLineSegment — true when @p normal points
/// into the polygon's exterior wedge at edge @p edge_idx (concave-corner
/// guard used by the game's resolvers).
bool collision_normal_valid_for_polygon(const float normal[2],
                                        const std::vector<float>& poly,
                                        int edge_idx);

/// RectangleIntersectsPolygon — the game's definitive rect-vs-polygon
/// resolve: returns true when a transformed rectangle overlaps a transformed
/// polygon; on hit writes the world-space collision normal + penetration.
/// @p body_center is the moving body's center used to pick the side.
bool rect_intersects_polygon(const float rect[4],
                             const float rpos[2], float rrot, float rscale,
                             const std::vector<float>& poly,
                             const float ppos[2], float prot, float pscale,
                             const float body_center[2],
                             float normal_out[2], float& pen_out);

// ─── Wall queries (Scene Player physics) ───────────────────────────────
/// Push a circle of @p radius at (x, y) out of every solid wall whose depth
/// range covers @p z. Returns true when any push happened. This is the
/// entity/hero wall clamp (segment distance, not AABB — walls are edges).
bool wall_clamp_circle(const CollisionWorld& w, float& x, float& y, float z,
                       float radius);

/// Raycast a segment (x0,y0)→(x1,y1) against solid walls active at @p z.
/// Returns true + earliest t (0..1) + wall normal on hit.
bool wall_raycast(const CollisionWorld& w, float x0, float y0,
                  float x1, float y1, float z,
                  float& t, float normal[2]);

/// True when a point is inside any solid wall AABB region (rough check).
bool wall_contains(const CollisionWorld& w, float x, float y, float z);

} // namespace av
