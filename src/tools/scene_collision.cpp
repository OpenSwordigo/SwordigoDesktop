/* scene_collision.cpp — Swordigo collision subsystem parser
 *
 * Parses ShapeComponent, CollisionShapeComponent,
 * BoneControlledCollisionShapeComponent, and GroundPolygonComponent
 * from SceneObject component lists.
 *
 * Component payload field numbers (schema_tag >> 3):
 *   ShapeComponent                       : tag 962,  field = 962  >> 3 = 120
 *   CollisionShapeComponent              : tag 970,  field = 970  >> 3 = 121
 *   BoneControlledCollisionShapeComponent: tag 994,  field = 994  >> 3 = 124
 *   GroundPolygonComponent               : tag 882,  field = 882  >> 3 = 110
 *
 * Protobuf wire types used:
 *   WIRE_I32    (5) : float (fixed32) — Rectangle X/Y/W/H, Circle Radius,
 *                     MinDepth, MaxDepth, Friction
 *   WIRE_VARINT (0) : bool/int — IsGround, Collides, Enabled, SpecialType, etc.
 *   WIRE_LEN    (2) : nested message / string
 *
 * Inner message layouts (from scene_schemas.cpp):
 *   Rectangle: field 13=X(I32), field 21=Y(I32), field 29=W(I32), field 37=H(I32)
 *   Circle:    field 10=Center(LEN/Vector2), field 21=Radius(I32)
 *   Vector2:   field 1=X(I32), field 2=Y(I32)
 *   Polygon:   field 10=Point(LEN/Vector2, repeated)
 *   CollisionShapeComponent payload: field 16=IsGround, 24=Collides, 32=ReceivesDamage,
 *       40=InflictsDamage, 53=MinDepth, 61=MaxDepth, 64=SpecialType, 88=Enabled,
 *       109=Friction, 112=UnsafeGround
 *   BoneControlledCollisionShapeComponent payload: field 16=CollisionShapeId(LEN),
 *       field 24=BoneControllerComponentId(LEN)
 *   GroundPolygonComponent payload: field 10=Vertex(LEN/Vector2, repeated),
 *       field 18=Polygon(LEN nested), field 24=Collides(VARINT),
 *       field 37=MinDepth(I32), field 45=MaxDepth(I32), field 61=Friction(I32),
 *       field 64=UnsafeGround(VARINT)
 */

#include "tools/scene_loader.h"
#include "tools/scene_collision.h"
#include "platform/protobuf_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace av {

// ─── Payload field numbers ────────────────────────────────────────────────
static constexpr int PAYLOAD_SHAPE_COMPONENT     = 120;  // 962  >> 3
static constexpr int PAYLOAD_COLLISION_SHAPE     = 121;  // 970  >> 3
static constexpr int PAYLOAD_BONE_CTRL_COLL      = 124;  // 994  >> 3
static constexpr int PAYLOAD_GROUND_POLYGON      = 110;  // 882  >> 3

// ─── Type name strings ────────────────────────────────────────────────────
// SCENE files store SHORT stems ("Shape", "CollisionShape", "GroundPolygon")
// while library templates and the schema use the "...Component" suffix.
// Match either spelling.
static constexpr const char* TYPE_SHAPE_COMPONENT    = "Shape";
static constexpr const char* TYPE_COLLISION_SHAPE    = "CollisionShape";
static constexpr const char* TYPE_BONE_CTRL_COLL     = "BoneControlledCollisionShape";
static constexpr const char* TYPE_GROUND_POLYGON     = "GroundPolygon";

// Match a component type name against a schema stem (e.g. "CollisionShape"),
// accepting the short scene spelling, the "...Component" suffix spelling,
// and trailing NUL / whitespace from protobuf padding.
static bool type_matches(const std::string& tn, const char* stem) {
    const size_t n = std::strlen(stem);
    if (tn.size() < n) return false;
    if (tn.compare(0, n, stem) != 0) return false;
    const char* rest = tn.c_str() + n;
    if (*rest == '\0') return true;
    if (std::strncmp(rest, "Component", 9) == 0) rest += 9;
    for (; *rest; ++rest) {
        const char c = *rest;
        if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return false;
    }
    return true;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

// Read the payload bytes for a given payload field_number from a component.
static std::string read_payload(const SceneComponent& comp, int expected_field) {
    try {
        proto::Reader reader(comp.raw_data);
        proto::Field f;
        while (reader.read_field(f)) {
            if (static_cast<int>(f.field_number) == expected_field &&
                f.wire_type == proto::WIRE_LEN)
                return f.bytes_val;
        }
    } catch (...) {}
    if (comp.payload_field > 0 && comp.payload_field != expected_field) {
        try {
            proto::Reader reader(comp.raw_data);
            proto::Field f;
            while (reader.read_field(f)) {
                if (f.field_number == static_cast<uint32_t>(comp.payload_field) &&
                    f.wire_type == proto::WIRE_LEN)
                    return f.bytes_val;
            }
        } catch (...) {}
    }
    return {};
}

// Parse a Vector2 nested message → {x, y}.
static bool read_vector2(const std::string& bytes, float& x, float& y) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        bool got_x = false, got_y = false;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_I32) { x = f.float_val; got_x = true; }
            if (f.field_number == 2 && f.wire_type == proto::WIRE_I32) { y = f.float_val; got_y = true; }
        }
        return got_x || got_y;
    } catch (...) {}
    return false;
}

// Parse a Rectangle nested message → rect[4] = {x, y, w, h}.
// Rectangle schema: field 13=X(I32), field 21=Y(I32), field 29=W(I32), field 37=H(I32)
static bool read_rectangle(const std::string& bytes, float rect[4]) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        bool any = false;
        while (reader.read_field(f)) {
            if (f.wire_type != proto::WIRE_I32) continue;
            if      (f.field_number == 1) { rect[0] = f.float_val; any = true; }  // tag 13 >> 3 = 1
            else if (f.field_number == 2) { rect[1] = f.float_val; any = true; }  // tag 21 >> 3 = 2
            else if (f.field_number == 3) { rect[2] = f.float_val; any = true; }  // tag 29 >> 3 = 3
            else if (f.field_number == 4) { rect[3] = f.float_val; any = true; }  // tag 37 >> 3 = 4
        }
        return any;
    } catch (...) {}
    return false;
}

// Parse a Circle nested message → center_x, center_y, radius.
// Circle schema: field 10=Center(LEN/Vector2), field 21=Radius(I32)
static bool read_circle(const std::string& bytes, float cx[2], float& radius) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        bool any = false;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_LEN) {  // tag 10 >> 3 = 1
                read_vector2(f.bytes_val, cx[0], cx[1]);
                any = true;
            } else if (f.field_number == 2 && f.wire_type == proto::WIRE_I32) {  // tag 21 >> 3 = 2
                radius = f.float_val;
                any = true;
            }
        }
        return any;
    } catch (...) {}
    return false;
}

// Parse a Polygon nested message → flat {x0,y0, x1,y1, ...} point list.
// Polygon schema: field 10=Point(LEN/Vector2, repeated) → tag 10 >> 3 = 1
static void read_polygon(const std::string& bytes, std::vector<float>& out) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_LEN) {  // tag 10 >> 3 = 1
                float px = 0.0f, py = 0.0f;
                read_vector2(f.bytes_val, px, py);
                out.push_back(px);
                out.push_back(py);
            }
        }
    } catch (...) {}
}

// Parse a ShapeComponent payload → detect and decode Rect/Circle/Polygon.
// ShapeComponent schema: field 10=Rectangle(LEN), field 18=Circle(LEN), field 26=Polygon(LEN)
// Tags >> 3: field 1 = Rectangle, field 2 = Circle, field 3 = Polygon
static CollisionShapeData parse_shape_payload(const std::string& bytes) {
    CollisionShapeData shape;
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.wire_type != proto::WIRE_LEN) continue;
            if (f.field_number == 1) {  // Rectangle (tag 10 >> 3 = 1)
                if (read_rectangle(f.bytes_val, shape.rect))
                    shape.type = COLL_RECT;
            } else if (f.field_number == 2) {  // Circle (tag 18 >> 3 = 2)
                if (read_circle(f.bytes_val, shape.circle_center, shape.circle_radius))
                    shape.type = COLL_CIRCLE;
            } else if (f.field_number == 3) {  // Polygon (tag 26 >> 3 = 3)
                read_polygon(f.bytes_val, shape.polygon_points);
                if (!shape.polygon_points.empty())
                    shape.type = COLL_POLYGON;
            }
        }
    } catch (...) {}
    return shape;
}

// Parse CollisionShapeComponent payload — applies behavior flags to a shape.
// CollisionShapeComponent schema field_number = tag >> 3:
//   16 >> 3 = 2  : IsGround (VARINT)
//   24 >> 3 = 3  : Collides (VARINT)
//   32 >> 3 = 4  : ReceivesDamage (VARINT)
//   40 >> 3 = 5  : InflictsDamage (VARINT)
//   53 >> 3 = 6  : MinDepth (I32, tag 53 = 6*8+5 = 53)
//   61 >> 3 = 7  : MaxDepth (I32, tag 61 = 7*8+5 = 61)
//   64 >> 3 = 8  : SpecialType (VARINT, tag 64 = 8*8 = 64)
//   88 >> 3 = 11 : Enabled (VARINT, tag 88 = 11*8 = 88)
//   109 >> 3 = 13: Friction (I32, tag 109 = 13*8+5 = 109)
//   112 >> 3 = 14: UnsafeGround (VARINT, tag 112 = 14*8 = 112)
static void parse_collision_shape_flags(const std::string& bytes, CollisionShapeData& shape) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            switch (f.field_number) {
                case 2:  if (f.wire_type == proto::WIRE_VARINT) shape.is_ground       = f.varint_val != 0; break;
                case 3:  if (f.wire_type == proto::WIRE_VARINT) shape.collides         = f.varint_val != 0; break;
                case 4:  if (f.wire_type == proto::WIRE_VARINT) shape.receives_damage  = f.varint_val != 0; break;
                case 5:  if (f.wire_type == proto::WIRE_VARINT) shape.inflicts_damage  = f.varint_val != 0; break;
                case 6:  if (f.wire_type == proto::WIRE_I32)    shape.min_depth        = f.float_val;       break;
                case 7:  if (f.wire_type == proto::WIRE_I32)    shape.max_depth        = f.float_val;       break;
                case 8:  if (f.wire_type == proto::WIRE_VARINT) shape.special_type     = static_cast<int>(f.varint_val); break;
                case 11: if (f.wire_type == proto::WIRE_VARINT) shape.enabled          = f.varint_val != 0; break;
                case 13: if (f.wire_type == proto::WIRE_I32)    shape.friction         = f.float_val;       break;
                case 14: if (f.wire_type == proto::WIRE_VARINT) shape.unsafe_ground    = f.varint_val != 0; break;
                default: break;
            }
        }
    } catch (...) {}
}

// Parse BoneControlledCollisionShapeComponent payload.
// Schema: field 16=CollisionShapeId(LEN), field 24=BoneControllerComponentId(LEN)
// Tags >> 3: field 2 = CollisionShapeId, field 3 = BoneControllerComponentId
static void parse_bone_ctrl_coll_payload(const std::string& bytes, CollisionShapeData& shape) {
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 2 && f.wire_type == proto::WIRE_LEN)
                shape.collision_shape_id = f.bytes_val;
            else if (f.field_number == 3 && f.wire_type == proto::WIRE_LEN)
                shape.bone_controller_id = f.bytes_val;
        }
    } catch (...) {}
    shape.bone_controlled = true;
}

// Parse GroundPolygonComponent payload.
// Schema field_number = tag >> 3:
//   10 >> 3 = 1 : Vertex (LEN/Vector2, repeated) — direct vertex list
//   18 >> 3 = 2 : Polygon (LEN nested, with repeated Point field)
//   24 >> 3 = 3 : Collides (VARINT)
//   37 >> 3 = 4 : MinDepth (I32)
//   45 >> 3 = 5 : MaxDepth (I32)
//   61 >> 3 = 7 : Friction (I32, tag 61 = 7*8+5)
//   64 >> 3 = 8 : UnsafeGround (VARINT)
static CollisionData::GroundPolygon parse_ground_polygon_payload(const std::string& bytes) {
    CollisionData::GroundPolygon gp;
    gp.collides = true;
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.field_number == 1 && f.wire_type == proto::WIRE_LEN) {
                // Direct Vertex (Vector2)
                float px = 0.0f, py = 0.0f;
                read_vector2(f.bytes_val, px, py);
                gp.points.push_back(px);
                gp.points.push_back(py);
            } else if (f.field_number == 2 && f.wire_type == proto::WIRE_LEN) {
                // Polygon nested message — read its Point fields
                read_polygon(f.bytes_val, gp.points);
            } else if (f.field_number == 3 && f.wire_type == proto::WIRE_VARINT) {
                gp.collides = f.varint_val != 0;
            } else if (f.field_number == 4 && f.wire_type == proto::WIRE_I32) {
                gp.min_depth = f.float_val;
            } else if (f.field_number == 5 && f.wire_type == proto::WIRE_I32) {
                gp.max_depth = f.float_val;
            } else if (f.field_number == 7 && f.wire_type == proto::WIRE_I32) {
                gp.friction = f.float_val;
            } else if (f.field_number == 8 && f.wire_type == proto::WIRE_VARINT) {
                gp.unsafe_ground = f.varint_val != 0;
            }
        }
    } catch (...) {}
    return gp;
}

CollisionData collision_parse(const SceneObject& obj) {
    CollisionData result;
    const auto& comps = obj.resolved_components.empty() ? obj.components : obj.resolved_components;

    for (const auto& comp : comps) {
        const std::string& tn = comp.type_name;

        if (type_matches(tn, TYPE_SHAPE_COMPONENT)) {
            // ShapeComponent: defines the geometric shape.
            // It is almost always paired with CollisionShapeComponent on the
            // same object — the engine links them via CollisionShapeId bindings.
            // We parse the geometry here; flags are applied by CollisionShapeComponent
            // if found on the same object.
            std::string payload = read_payload(comp, PAYLOAD_SHAPE_COMPONENT);
            if (!payload.empty()) {
                CollisionShapeData shape = parse_shape_payload(payload);
                if (shape.type != COLL_NONE)
                    result.shapes.push_back(std::move(shape));
            }
        }
        else if (type_matches(tn, TYPE_COLLISION_SHAPE)) {
            // CollisionShapeComponent: behavior flags for the shape.
            // If a ShapeComponent was already parsed, apply flags to the last
            // shape. If no ShapeComponent exists yet, create a placeholder
            // (bounds from LocalAabb will be resolved by caller if needed).
            std::string payload = read_payload(comp, PAYLOAD_COLLISION_SHAPE);
            if (!payload.empty()) {
                if (result.shapes.empty()) {
                    // No geometry yet — create a default rect shape so flags
                    // have somewhere to land; caller can update geometry from
                    // the object's LocalAabb.
                    CollisionShapeData shape;
                    shape.type = COLL_RECT;
                    result.shapes.push_back(shape);
                }
                parse_collision_shape_flags(payload, result.shapes.back());
            }
        }
        else if (type_matches(tn, TYPE_BONE_CTRL_COLL)) {
            // BoneControlledCollisionShapeComponent: hitbox follows a bone.
            CollisionShapeData shape;
            shape.type = COLL_RECT;  // geometry resolved at runtime by bone
            std::string payload = read_payload(comp, PAYLOAD_BONE_CTRL_COLL);
            if (!payload.empty())
                parse_bone_ctrl_coll_payload(payload, shape);
            else
                shape.bone_controlled = true;
            result.shapes.push_back(std::move(shape));
        }
        else if (type_matches(tn, TYPE_GROUND_POLYGON)) {
            // GroundPolygonComponent: walkable surface polygon for ground mesh.
            std::string payload = read_payload(comp, PAYLOAD_GROUND_POLYGON);
            if (!payload.empty()) {
                CollisionData::GroundPolygon gp = parse_ground_polygon_payload(payload);
                if (!gp.points.empty())
                    result.ground_polygons.push_back(std::move(gp));
            }
        }
    }

    return result;
}

// ============================================================================
// RUNTIME COLLISION WORLD
// ============================================================================

namespace {

// Vector2::Transformed / InverseTransformed (Caver::Vector2 ports):
//   T(v) = pos + R(rot) · v · scale        (scale applied along the axes)
//   T⁻¹(v) = R(-rot) · (v - pos) / scale
void v2_transform(float& x, float& y, const float pos[2], float rot, float s) {
    const float c = std::cos(rot), sn = std::sin(rot);
    const float rx = x * c - y * sn;
    const float ry = x * sn + y * c;
    x = pos[0] + rx * s;
    y = pos[1] + ry * s;
}

void v2_inverse_transform(float& x, float& y, const float pos[2], float rot, float s) {
    const float c = std::cos(rot), sn = std::sin(rot);
    float dx = x - pos[0], dy = y - pos[1];
    const float sx = s != 0.0f ? 1.0f / s : 1.0f;
    x = (dx * c + dy * sn) * sx;
    y = (-dx * sn + dy * c) * sx;
}

// True when the object is an animated entity (monster/hero) — its collision
// shapes are dynamic damage volumes and must never become static walls.
// Same taxonomy as scene_game.cpp is_animated_entity().
bool is_animated_entity(const SceneObject& o) {
    const auto& comps = o.resolved_components.empty() ? o.components
                                                      : o.resolved_components;
    for (const auto& c : comps) {
        const std::string& t = c.type_name;
        if (t.find("MonsterDeathController") != std::string::npos)
            continue;
        if (t.find("MonsterController") != std::string::npos ||
            t.find("CharAnimController") != std::string::npos ||
            t.find("CharController") != std::string::npos ||
            t.find("HeroEntity") != std::string::npos)
            return true;
    }
    return false;
}

void push_segment(std::vector<WallSegment>& walls, const WallSegment& s) {
    walls.push_back(s);
}

} // namespace

OrientedRect ob_from_rectangle(const float rect[4]) {
    OrientedRect ob;
    ob.c[0] = rect[0] + rect[2] * 0.5f;
    ob.c[1] = rect[1] + rect[3] * 0.5f;
    ob.u[0] = 1.0f; ob.u[1] = 0.0f;
    ob.u[2] = 0.0f; ob.u[3] = 1.0f;
    ob.he[0] = rect[2] * 0.5f;
    ob.he[1] = rect[3] * 0.5f;
    return ob;
}

OrientedRect ob_from_transformed_rectangle(const float rect[4],
                                           const float pos_a[2], float rot_a,
                                           float scale_a,
                                           const float pos_b[2], float rot_b,
                                           float scale_b) {
    // Rect center expressed in space A, then inverse-transformed into space B.
    OrientedRect ob;
    float cx = rect[0] + rect[2] * 0.5f;
    float cy = rect[1] + rect[3] * 0.5f;
    v2_transform(cx, cy, pos_a, rot_a, scale_a);
    v2_inverse_transform(cx, cy, pos_b, rot_b, scale_b);
    ob.c[0] = cx; ob.c[1] = cy;
    // Basis axis: the rect's +X axis rotated by the relative rotation.
    const float drot = rot_a - rot_b;
    ob.u[0] = std::cos(drot); ob.u[1] = std::sin(drot);
    ob.u[2] = -ob.u[1]; ob.u[3] = ob.u[0];
    // Half extents: rect half-size scaled by (scale_a / scale_b) — the
    // decompiled code keeps scale_a when B is unscaled.
    float k = scale_a;
    if (std::fabs(scale_b - 1.0f) > 0.0001f) k = scale_a / scale_b;
    ob.he[0] = rect[2] * 0.5f * k;
    ob.he[1] = rect[3] * 0.5f * k;
    return ob;
}

void ob_span_on_axis(const OrientedRect& ob, const float axis[2],
                     float& lo, float& hi) {
    const float proj = ob.c[0] * axis[0] + ob.c[1] * axis[1];
    const float r =
        ob.he[0] * std::fabs(ob.u[0] * axis[0] + ob.u[1] * axis[1]) +
        ob.he[1] * std::fabs(ob.u[2] * axis[0] + ob.u[3] * axis[1]);
    lo = proj - r;
    hi = proj + r;
}

bool ob_intersects_on_axis(const OrientedRect& a, const OrientedRect& b,
                           const float axis[2], float& overlap) {
    float la, ha, lb, hb;
    ob_span_on_axis(a, axis, la, ha);
    ob_span_on_axis(b, axis, lb, hb);
    if (lb - ha > 0.001f || la - hb > 0.001f) return false;
    overlap = std::min(ha, hb) - std::max(la, lb);
    return true;
}

bool collision_normal_valid_for_polygon(const float n[2],
                                        const std::vector<float>& poly,
                                        int edge) {
    // Faithful port of IsCollisionNormalValidForPolygonLineSegment @0x4AC2A8.
    // The normal must point into the polygon's exterior wedge at @p edge.
    // Valid iff (all strict):
    //   case A (n·e >= 0, normal aligned with the edge):
    //     edge != N-2,  cross(adj, e) > 0,  n·e > 0,  n·adj < 0
    //   case B (n·e < 0, normal opposed to the edge):
    //     edge != 0,    cross(prev, e) > 0, n·prev > 0, n·e < 0
    // The N-2/edge-0 exclusions are the game's closed-loop ground-edge rules:
    // the first and second-to-last edges of a plain closed polygon are the
    // walkable profile edges, never wall-push normals.
    //
    // Exported as public API for other subsystems. The rect/circle resolvers
    // in this file deliberately do NOT gate on it — the reference feeds it
    // OB-axis candidates produced by its corner-flip selection, which the
    // minimum-penetration resolver here replaces (see rect_intersects_polygon).
    const int N = (int)(poly.size() / 2);
    if (N < 2 || edge < 0 || edge >= N) return false;
    const float* p = poly.data();
    const float ex = p[((edge + 1) % N) * 2] - p[edge * 2];
    const float ey = p[((edge + 1) % N) * 2 + 1] - p[edge * 2 + 1];
    const float de = ex * n[0] + ey * n[1];
    float c1, c2, c3;
    if (de >= 0.0f) {
        if (edge == N - 2) return false;
        const float ax = p[((edge + 2) % N) * 2] - p[((edge + 1) % N) * 2];
        const float ay = p[((edge + 2) % N) * 2 + 1] - p[((edge + 1) % N) * 2 + 1];
        c1 = ay * ex - ax * ey;             // cross(adj, e)
        c2 = de;                            // n·e
        c3 = -(n[0] * ax + n[1] * ay);      // −(n·adj)
    } else {
        if (edge == 0) return false;
        const float px = p[edge * 2] - p[(edge - 1) * 2];
        const float py = p[edge * 2 + 1] - p[(edge - 1) * 2 + 1];
        c1 = ey * px - py * ex;             // cross(prev, e)
        c2 = n[0] * px + n[1] * py;         // n·prev
        c3 = -de;                           // −(n·e)
    }
    if (c1 <= 0.0f) return false;
    if (c2 <= 0.0f) return false;
    return c3 > 0.0f;
}

bool rect_intersects_polygon(const float rect[4],
                             const float rpos[2], float rrot, float rscale,
                             const std::vector<float>& poly,
                             const float ppos[2], float prot, float pscale,
                             const float body_center[2],
                             float normal_out[2], float& pen_out) {
    // Port of RectangleIntersectsPolygon @0x4AC5A0. The reference keeps each
    // edge's OUTWARD unit normal fixed and only flips the OBB corner
    // projections; it never flips the normal toward the body (that would
    // produce inward normals that push INTO the polygon when the body is
    // inside, and every such candidate is then rejected by the exterior-wedge
    // validity guard). We test the OBB against each edge segment on that
    // edge's normal, keep the minimum-penetration axis and rotate it back to
    // world space. Like the reference, a body FULLY contained by a convex
    // polygon reports no contact (every edge is separated on its own axis) —
    // the game never lets a body get that deep.
    const int N = (int)(poly.size() / 2);
    if (N < 2) return false;
    // body_center is reserved for the reference's side-preference tiebreak
    // (the distance-based comparison when two edges tie on penetration); the
    // minimum-penetration selection below does not need it.
    (void)body_center;
    OrientedRect ob = ob_from_transformed_rectangle(rect, rpos, rrot, rscale,
                                                    ppos, prot, pscale);
    const float* p = poly.data();
    float best_n[2] = {0, 1};
    float best_pen = 1e30f;
    bool hit = false;
    for (int i = 0; i < N; ++i) {
        const int j = (i + 1) % N;
        const float ex = p[j * 2] - p[i * 2];
        const float ey = p[j * 2 + 1] - p[i * 2 + 1];
        const float el = std::sqrt(ex * ex + ey * ey);
        if (el < 1e-6f) continue;
        // Outward unit normal for a CCW polygon (right-hand perpendicular).
        const float nx = ey / el, ny = -ex / el;
        const float axis[2] = {nx, ny};
        // OBB span and edge-segment span on the axis.
        float lo1, hi1;
        ob_span_on_axis(ob, axis, lo1, hi1);
        const float e0 = p[i * 2] * nx + p[i * 2 + 1] * ny;
        const float e1 = p[j * 2] * nx + p[j * 2 + 1] * ny;
        const float lo2 = std::min(e0, e1), hi2 = std::max(e0, e1);
        // Separated when the edge lies entirely on one side of the OBB span.
        if (lo2 > hi1 + 0.001f || hi2 < lo1 - 0.001f) continue;
        // Penetration = how far the edge sits inside the OBB's near side,
        // measured along the outward normal (the reference's seg_hi − obb_lo
        // output): pushing the body by this amount along +n separates them.
        const float pen = hi2 - lo1;
        if (pen < best_pen) {
            best_pen = pen;
            best_n[0] = nx; best_n[1] = ny;
            hit = true;
        }
    }
    if (!hit) return false;
    // Rotate the normal back into world space (Vector2::Rotate by prot).
    const float c = std::cos(prot), sn = std::sin(prot);
    normal_out[0] = best_n[0] * c - best_n[1] * sn;
    normal_out[1] = best_n[0] * sn + best_n[1] * c;
    pen_out = best_pen * pscale;
    return true;
}

// ============================================================================
// CollisionWorld builder — parses a scene into clean visible walls
// ============================================================================

void collision_build_world(const SceneData& scene, CollisionWorld& out) {
    out.walls.clear();
    out.segment_count = 0;
    out.shape_objects = 0;
    out.min_x = out.min_y = 1e30f;
    out.max_x = out.max_y = -1e30f;

    for (int oi = 0; oi < (int)scene.objects.size(); ++oi) {
        const SceneObject& o = scene.objects[oi];
        if (o.hidden) continue;
        const bool animated = is_animated_entity(o);
        const float sx = o.scale_x > 0.0f ? o.scale_x : 1.0f;
        const float sy = o.scale_y > 0.0f ? o.scale_y : 1.0f;
        const float pos[2] = {o.pos_x, o.pos_y};

        const CollisionData cd = collision_parse(o);
        bool contributed = false;

        // ── ShapeComponent / CollisionShapeComponent geometry ──────────
        for (const auto& s : cd.shapes) {
            if (animated) break;      // dynamic entity hitboxes → skip
            if (!s.enabled) continue;
            const float z_min = o.pos_z + s.min_depth;
            const float z_max = o.pos_z + s.max_depth;
            const bool solid = s.collides;
            const bool ground = s.is_ground;
            const bool unsafe = s.unsafe_ground;
            switch (s.type) {
            case COLL_RECT: {
                // World OBB → 4 edge segments with outward normals.
                OrientedRect ob = ob_from_rectangle(s.rect);
                // scale + rotate + translate (uniform 2D object transform).
                // The OBB center sits at (rect origin + half extents) in
                // object-local space; transform it to world first, then add
                // each half-extent corner offset (previously the corner
                // offsets were added straight to the object origin, shifting
                // every wall by the rect's local origin).
                const float cx = ob.c[0] * sx, cy = ob.c[1] * sy;
                const float c = std::cos(o.rot_y), sn = std::sin(o.rot_y);
                const float ox = pos[0] + c * cx - sn * cy;
                const float oy = pos[1] + sn * cx + c * cy;
                for (int e = 0; e < 4; ++e) {
                    const float hx = ob.he[0], hy = ob.he[1];
                    // local corners of the rect (bottom-left → CCW)
                    float lc[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
                    const float a[2] = {lc[e][0] * sx, lc[e][1] * sy};
                    const float b[2] = {lc[(e + 1) % 4][0] * sx, lc[(e + 1) % 4][1] * sy};
                    float wx[2] = {c * a[0] - sn * a[1], sn * a[0] + c * a[1]};
                    float wy[2] = {c * b[0] - sn * b[1], sn * b[0] + c * b[1]};
                    WallSegment w;
                    w.a[0] = ox + wx[0]; w.a[1] = oy + wx[1];
                    w.b[0] = ox + wy[0]; w.b[1] = oy + wy[1];
                    // outward normal = right-hand perp of CCW edge
                    const float ex = w.b[0] - w.a[0], ey = w.b[1] - w.a[1];
                    const float el = std::sqrt(ex * ex + ey * ey);
                    if (el < 1e-6f) continue;
                    w.nx = ey / el; w.ny = -ex / el;
                    w.z_min = z_min; w.z_max = z_max;
                    w.solid = solid;
                    // Intelligent walkable top: an up-facing edge of a solid
                    // box/polygon is a surface you can stand on (one-way,
                    // blocks only from above), matching the ground-edge
                    // behavior of the game's ground polygons.
                    w.ground = ground || (solid && w.ny > 0.35f);
                    w.one_way = w.ground;
                    w.unsafe = unsafe;
                    w.source_object = oi;
                    push_segment(out.walls, w);
                }
                contributed = true;
                break;
            }
            case COLL_POLYGON: {
                const auto& pts = s.polygon_points;
                const int n = (int)(pts.size() / 2);
                if (n < 2) break;
                for (int i = 0; i < n; ++i) {
                    const int j = (i + 1) % n;
                    WallSegment w;
                    w.a[0] = pos[0] + pts[i * 2] * sx;
                    w.a[1] = pos[1] + pts[i * 2 + 1] * sy;
                    w.b[0] = pos[0] + pts[j * 2] * sx;
                    w.b[1] = pos[1] + pts[j * 2 + 1] * sy;
                    const float ex = w.b[0] - w.a[0], ey = w.b[1] - w.a[1];
                    const float el = std::sqrt(ex * ex + ey * ey);
                    if (el < 1e-6f) continue;
                    w.nx = ey / el; w.ny = -ex / el;   // CCW outward
                    w.z_min = z_min; w.z_max = z_max;
                    w.solid = solid;
                    w.ground = ground || (solid && w.ny > 0.35f);
                    w.one_way = w.ground;
                    w.unsafe = unsafe;
                    w.source_object = oi;
                    push_segment(out.walls, w);
                }
                contributed = true;
                break;
            }
            case COLL_CIRCLE: {
                // 12-gon ring approximation.
                const int k = 12;
                const float r = s.circle_radius * std::max(sx, sy);
                if (r < 1e-4f) break;
                for (int i = 0; i < k; ++i) {
                    const float t0 = (float)i / k * 6.2831853f;
                    const float t1 = (float)(i + 1) / k * 6.2831853f;
                    WallSegment w;
                    w.a[0] = pos[0] + s.circle_center[0] * sx + std::cos(t0) * r;
                    w.a[1] = pos[1] + s.circle_center[1] * sy + std::sin(t0) * r;
                    w.b[0] = pos[0] + s.circle_center[0] * sx + std::cos(t1) * r;
                    w.b[1] = pos[1] + s.circle_center[1] * sy + std::sin(t1) * r;
                    const float ex = w.b[0] - w.a[0], ey = w.b[1] - w.a[1];
                    const float el = std::sqrt(ex * ex + ey * ey);
                    if (el < 1e-6f) continue;
                    w.nx = ey / el; w.ny = -ex / el;
                    w.z_min = z_min; w.z_max = z_max;
                    w.solid = solid; w.ground = false; w.one_way = false;
                    w.unsafe = unsafe;
                    w.source_object = oi;
                    push_segment(out.walls, w);
                }
                contributed = true;
                break;
            }
            default: break;
            }
        }

        // ── GroundPolygonComponent profiles → walkable one-way tops ────
        // Each edge becomes a wall: up-facing edges are walkable ground
        // (one-way: block only from above), side/steep edges stay solid so
        // the ends of the platform are walls, unsafe surfaces hurt.
        if (!animated) {
            const auto& gps = cd.ground_polygons;
            if (!gps.empty() || !o.ground_polygon_points.empty()) {
                const float z_min = o.pos_z + (gps.empty() ? o.ground_polygon_min_depth
                                                           : gps[0].min_depth);
                const float z_max = o.pos_z + (gps.empty() ? o.ground_polygon_max_depth
                                                           : gps[0].max_depth);
                for (const auto& gp : gps) {
                    const auto& pts = gp.points;
                    const int n = (int)(pts.size() / 2);
                    if (n < 2) continue;
                    for (int i = 0; i < n; ++i) {
                        const int j = (i + 1) % n;
                        WallSegment w;
                        w.a[0] = pos[0] + pts[i * 2] * sx;
                        w.a[1] = pos[1] + pts[i * 2 + 1] * sy;
                        w.b[0] = pos[0] + pts[j * 2] * sx;
                        w.b[1] = pos[1] + pts[j * 2 + 1] * sy;
                        const float ex = w.b[0] - w.a[0], ey = w.b[1] - w.a[1];
                        const float el = std::sqrt(ex * ex + ey * ey);
                        if (el < 1e-6f) continue;
                        w.nx = ey / el; w.ny = -ex / el;
                        w.z_min = z_min; w.z_max = z_max;
                        w.solid = gp.collides;
                        w.unsafe = gp.unsafe_ground;
                        // The game treats the first and second-to-last edges
                        // of a closed ground polygon as the walkable profile
                        // (IsCollisionNormalValidForPolygonLineSegment's
                        // ground-edge rule); up-facing edges are walkable
                        // regardless. Side edges stay solid so the ends of
                        // the platform are walls.
                        w.ground = (i == 0 || i == n - 2) ||
                                   (w.ny > 0.35f) || gp.unsafe_ground;
                        w.one_way = w.ground;   // walkable tops block from above
                        w.source_object = oi;
                        push_segment(out.walls, w);
                    }
                    contributed = true;
                }
                // Fallback: scene_loader's direct copy (no parsed gp).
                if (gps.empty() && !o.ground_polygon_points.empty()) {
                    const auto& pts = o.ground_polygon_points;
                    const int n = (int)(pts.size() / 2);
                    for (int i = 0; i < n; ++i) {
                        const int j = (i + 1) % n;
                        WallSegment w;
                        w.a[0] = pos[0] + pts[i * 2] * sx;
                        w.a[1] = pos[1] + pts[i * 2 + 1] * sy;
                        w.b[0] = pos[0] + pts[j * 2] * sx;
                        w.b[1] = pos[1] + pts[j * 2 + 1] * sy;
                        const float ex = w.b[0] - w.a[0], ey = w.b[1] - w.a[1];
                        const float el = std::sqrt(ex * ex + ey * ey);
                        if (el < 1e-6f) continue;
                        w.nx = ey / el; w.ny = -ex / el;
                        w.z_min = z_min; w.z_max = z_max;
                        w.solid = o.ground_polygon_collides;
                        w.unsafe = o.ground_polygon_unsafe;
                        w.ground = (i == 0 || i == n - 2) ||
                                   (w.ny > 0.35f) || o.ground_polygon_unsafe;
                        w.one_way = w.ground;
                        w.source_object = oi;
                        push_segment(out.walls, w);
                    }
                    contributed = true;
                }
            }
        }
        if (contributed) ++out.shape_objects;
    }

    // ── Merge pass: join collinear, same-flag, touching segments so the
    //    visible wall layout stays clean (no per-edge seams). ────────────
    auto same_flags = [](const WallSegment& a, const WallSegment& b) {
        return a.solid == b.solid && a.ground == b.ground &&
               a.unsafe == b.unsafe && a.one_way == b.one_way;
    };
    bool merged = true;
    while (merged && !out.walls.empty()) {
        merged = false;
        std::vector<WallSegment> next;
        next.reserve(out.walls.size());
        for (auto& w : out.walls) {
            bool consumed = false;
            for (auto& k : next) {
                if (!same_flags(w, k)) continue;
                if (std::fabs(k.z_min - w.z_min) > 1.0f ||
                    std::fabs(k.z_max - w.z_max) > 1.0f)
                    continue;
                // Collinear? direction cross + point-on-line distance.
                const float ex1 = w.b[0] - w.a[0], ey1 = w.b[1] - w.a[1];
                const float ex2 = k.b[0] - k.a[0], ey2 = k.b[1] - k.a[1];
                const float l1 = std::sqrt(ex1 * ex1 + ey1 * ey1);
                const float l2 = std::sqrt(ex2 * ex2 + ey2 * ey2);
                if (l1 < 1e-4f || l2 < 1e-4f) continue;
                const float cross = std::fabs(ex1 * ey2 - ey1 * ex2) /
                                    (l1 * l2);
                if (cross > 0.001f) continue;
                // Distance from w.a to the line of k.
                const float dist = std::fabs((k.b[0] - k.a[0]) * (k.a[1] - w.a[1]) -
                                             (k.b[1] - k.a[1]) * (k.a[0] - w.a[0])) /
                                   l2;
                if (dist > 0.5f) continue;
                // Project both onto the dominant axis; merge if touching/overlap.
                const bool hor = std::fabs(ex1) >= std::fabs(ey1);
                float lo1, hi1, lo2, hi2;
                if (hor) {
                    lo1 = std::min(w.a[0], w.b[0]); hi1 = std::max(w.a[0], w.b[0]);
                    lo2 = std::min(k.a[0], k.b[0]); hi2 = std::max(k.a[0], k.b[0]);
                } else {
                    lo1 = std::min(w.a[1], w.b[1]); hi1 = std::max(w.a[1], w.b[1]);
                    lo2 = std::min(k.a[1], k.b[1]); hi2 = std::max(k.a[1], k.b[1]);
                }
                if (lo1 > hi2 + 1.0f || lo2 > hi1 + 1.0f) continue;
                // Merge: union endpoints (outer points win), averaged normal.
                const float nl = std::sqrt(k.nx * k.nx + k.ny * k.ny);
                k.nx = (k.nx + w.nx) * 0.5f / (nl > 0 ? nl : 1.0f);
                k.ny = (k.ny + w.ny) * 0.5f / (nl > 0 ? nl : 1.0f);
                if (hor) {
                    if (lo1 < lo2) { k.a[0] = w.a[0]; k.a[1] = w.a[1]; }
                    if (hi1 > hi2) { k.b[0] = w.b[0]; k.b[1] = w.b[1]; }
                } else {
                    if (lo1 < lo2) { k.a[0] = w.a[0]; k.a[1] = w.a[1]; }
                    if (hi1 > hi2) { k.b[0] = w.b[0]; k.b[1] = w.b[1]; }
                }
                consumed = true;
                merged = true;
                break;
            }
            if (!consumed) next.push_back(w);
        }
        out.walls.swap(next);
    }

    out.segment_count = out.walls.size();
    for (const auto& w : out.walls) {
        out.min_x = std::min(out.min_x, std::min(w.a[0], w.b[0]));
        out.max_x = std::max(out.max_x, std::max(w.a[0], w.b[0]));
        out.min_y = std::min(out.min_y, std::min(w.a[1], w.b[1]));
        out.max_y = std::max(out.max_y, std::max(w.a[1], w.b[1]));
    }
}

// ============================================================================
// Wall queries
// ============================================================================

static bool seg_seg_intersect(const float p1[2], const float p2[2],
                              const float p3[2], const float p4[2],
                              float& t, float& u) {
    const float d1x = p2[0] - p1[0], d1y = p2[1] - p1[1];
    const float d2x = p4[0] - p3[0], d2y = p4[1] - p3[1];
    const float den = d1x * d2y - d1y * d2x;
    if (std::fabs(den) < 1e-9f) return false;
    const float dx = p3[0] - p1[0], dy = p3[1] - p1[1];
    t = (dx * d2y - dy * d2x) / den;
    u = (dx * d1y - dy * d1x) / den;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

bool wall_clamp_circle(const CollisionWorld& w, float& x, float& y, float z,
                       float radius) {
    bool pushed = false;
    for (const auto& s : w.walls) {
        if (!s.solid) continue;
        if (z < s.z_min || z > s.z_max) continue;
        if (s.one_way) continue;   // one-way platforms don't push sideways
        const float ax = s.b[0] - s.a[0], ay = s.b[1] - s.a[1];
        const float ll = ax * ax + ay * ay;
        float tt = 0.0f;
        if (ll > 1e-8f) {
            tt = ((x - s.a[0]) * ax + (y - s.a[1]) * ay) / ll;
            tt = std::clamp(tt, 0.0f, 1.0f);
        }
        const float px = s.a[0] + ax * tt, py = s.a[1] + ay * tt;
        float dx = x - px, dy = y - py;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > radius) continue;
        // Only push when we are on the outward (normal) side of the wall —
        // walls are 2D edges, the interior side is never entered.
        if (d > 1e-5f) {
            const float side = dx * s.nx + dy * s.ny;
            if (side < 0.0f) continue;   // inside the wall's far side — ignore
            dx /= d; dy /= d;
        } else {
            dx = s.nx; dy = s.ny;
        }
        x += dx * (radius - d);
        y += dy * (radius - d);
        pushed = true;
    }
    return pushed;
}

bool wall_raycast(const CollisionWorld& w, float x0, float y0,
                  float x1, float y1, float z,
                  float& t, float normal[2]) {
    const float p1[2] = {x0, y0}, p2[2] = {x1, y1};
    float best_t = 1.0f;
    bool hit = false;
    for (const auto& s : w.walls) {
        if (!s.solid || s.one_way) continue;
        if (z < s.z_min || z > s.z_max) continue;
        const float p3[2] = {s.a[0], s.a[1]}, p4[2] = {s.b[0], s.b[1]};
        float tt, uu;
        if (seg_seg_intersect(p1, p2, p3, p4, tt, uu)) {
            if (tt < best_t) {
                best_t = tt;
                normal[0] = s.nx; normal[1] = s.ny;
                hit = true;
            }
        }
    }
    if (hit) t = best_t;
    return hit;
}

bool wall_contains(const CollisionWorld& w, float x, float y, float z) {
    for (const auto& s : w.walls) {
        if (!s.solid || s.one_way) continue;
        if (z < s.z_min || z > s.z_max) continue;
        const float ax = s.b[0] - s.a[0], ay = s.b[1] - s.a[1];
        const float ll = ax * ax + ay * ay;
        float tt = 0.0f;
        if (ll > 1e-8f)
            tt = std::clamp(((x - s.a[0]) * ax + (y - s.a[1]) * ay) / ll, 0.0f, 1.0f);
        const float px = s.a[0] + ax * tt, py = s.a[1] + ay * tt;
        const float dx = x - px, dy = y - py;
        const float d = std::sqrt(dx * dx + dy * dy);
        // Inside the wall slab on the outward side, close to the line.
        if (d < 2.0f && (dx * s.nx + dy * s.ny) >= 0.0f) return true;
    }
    return false;
}

} // namespace av
