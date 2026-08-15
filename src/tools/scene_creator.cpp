// scene_creator.cpp — Scene Creator implementation
// Evidence base: docs/scenecreator/01–08_*.md (decoded from 118 shipped scenes)
// All component field numbers verified against src/tools/scene_schemas.cpp.
//
// Component payload field numbers (object-level, scene_schemas.cpp):
//   1042  LightComponent           1602  BackgroundComponent
//   4002  PortalComponent          4010  SpawnPointComponent
//   962   ShapeComponent           970   CollisionShapeComponent
//
// LightComponent inner fields (scene_schemas.cpp line 562–569):
//   8  Type    21  Intensity    26  Color{R,G,B,A}    61  Radius
//
// SpawnPointComponent inner fields (scene_schemas.cpp line 929–930):
//   8  FacingDirection    18  SpawnOffset{Vector3}
//
// BackgroundComponent inner field:
//   10  TextureName
//
// CollisionShapeComponent inner fields (docs/scenecreator/04 §3):
//   16 IsGround  24 Collides  32 ReceivesDamage  40 InflictsDamage
//   53 MinDepth  61 MaxDepth  64 SpecialType     88 Enabled
//
// PortalComponent inner fields (docs/scenecreator/04 §2):
//   10 DestinationSceneName  18 SpawnPointName  24 TapToEnter  32 TriggerShapeId
//
// ShapeComponent inner (Rectangle message):
//   field 2 Rectangle{1 X, 2 Y, 3 Width, 4 Height}

#include "tools/scene_creator.h"

#include "tools/boulder.h"
#include "tools/scene_loader.h"
#include "platform/protobuf_reader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace scenecreate {
namespace {

// ============================================================================
// Low-level helpers
// ============================================================================

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last  = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool valid_identifier(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool write_file(const fs::path& path, const std::string& data, std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Cannot write " + path.string(); return false; }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) { error = "Failed while writing " + path.string(); return false; }
    return true;
}

// ============================================================================
// Component builders
// Each make_component wraps a fully-encoded payload into an av::SceneComponent
// with type_name / type_id / payload_field / raw_data set.
//
// The raw_data is a serialized proto::Writer wrapping:
//   field 1 = ClassName (string)
//   field 2 = Identifier (varint)
//   [optional] field = payload sub-message
//
// payload_field is the *object-level* field number from scene_schemas.cpp
// e.g. 4010 for SpawnPointComponent, 1042 for LightComponent, etc.
// ============================================================================

av::SceneComponent make_component(const std::string& class_name,
                                  int identifier,
                                  uint32_t payload_field,
                                  const proto::Writer& payload) {
    proto::Writer wrapper;
    wrapper.write_string_field(1, class_name);
    wrapper.write_varint_field(2, static_cast<uint64_t>(identifier));
    wrapper.write_nested_field(payload_field, payload);

    av::SceneComponent comp;
    comp.type_name     = class_name;
    comp.type_id       = identifier;
    comp.payload_field = static_cast<int>(payload_field);
    comp.raw_data      = wrapper.to_string();
    return comp;
}

// Like above but also writes ParentComponentIdentifier (field 3 on the wrapper)
av::SceneComponent make_child_component(const std::string& class_name,
                                        int identifier,
                                        int parent_id,
                                        uint32_t payload_field,
                                        const proto::Writer& payload) {
    proto::Writer wrapper;
    wrapper.write_string_field(1, class_name);
    wrapper.write_varint_field(2, static_cast<uint64_t>(identifier));
    wrapper.write_varint_field(3, static_cast<uint64_t>(parent_id));
    wrapper.write_nested_field(payload_field, payload);

    av::SceneComponent comp;
    comp.type_name     = class_name;
    comp.type_id       = identifier;
    comp.payload_field = static_cast<int>(payload_field);
    comp.raw_data      = wrapper.to_string();
    return comp;
}

// ============================================================================
// Bounds builder (docs/scenecreator/02 §1.2)
// The Bounds sub-message has four fixed32 float fields: X, Y, Width, Height.
// Stored as raw bytes in scene.bounds[].
// ============================================================================
std::string make_bounds(float x, float y, float width, float height) {
    proto::Writer rect;
    rect.write_float_field(1, x);
    rect.write_float_field(2, y);
    rect.write_float_field(3, width);
    rect.write_float_field(4, height);
    return rect.to_string();
}

// Derive bounds from a ground AABB + margin (docs/scenecreator/02 §1.4)
std::string make_bounds_from_platform(float plat_w, float plat_h) {
    constexpr float margin = 200.0f;
    const float x = -plat_w * 0.5f - margin;
    const float y = -plat_h       - margin;
    const float w =  plat_w       + 2.0f * margin;
    const float h =  plat_h * 4.0f + 2.0f * margin;  // generous vertical room
    return make_bounds(x, y, w, h);
}

// Per-template canonical bounds (docs/scenecreator/02 §1.3 + 06 §3)
std::string make_template_bounds(SceneTemplate t, float plat_w, float plat_h) {
    switch (t) {
    case SceneTemplate::Outdoor:
        return make_bounds(-3500.0f, -500.0f, 5500.0f, 2000.0f);
    case SceneTemplate::Standard:
        return make_bounds(-3500.0f, -1000.0f, 5500.0f, 2500.0f);
    case SceneTemplate::Indoor:
        return make_bounds(-450.0f, -300.0f, 1750.0f, 1000.0f);
    case SceneTemplate::Dungeon:
        return make_bounds(-2900.0f, -1200.0f, 4000.0f, 2200.0f);
    case SceneTemplate::BossArena:
        return make_bounds(-3500.0f, -500.0f, 7000.0f, 3000.0f);
    case SceneTemplate::Portal:
        return make_bounds(-1600.0f, -500.0f, 3200.0f, 1500.0f);
    case SceneTemplate::Menu:
        return make_bounds(-3500.0f, -500.0f, 5500.0f, 2000.0f);
    case SceneTemplate::Minimal:
    default:
        return make_bounds_from_platform(plat_w, plat_h);
    }
}

// ============================================================================
// spawn_default builder (docs/scenecreator/04 §1)
// SpawnPointComponent payload field = 501
//   field 1 = FacingDirection (varint)
//   field 2 = SpawnOffset (Vector3 nested: fields 1,2,3 = X,Y,Z floats)
// LocalAabb matches observed: X:-30 Y:-30 W:60 H:60
// ============================================================================
static std::string make_local_aabb(float x, float y, float w, float h) {
    proto::Writer aabb;
    aabb.write_float_field(1, x);
    aabb.write_float_field(2, y);
    aabb.write_float_field(3, w);
    aabb.write_float_field(4, h);
    return aabb.to_string();
}

av::SceneObject make_spawn(const Options& options, const std::string& name = "spawn_default") {
    proto::Writer offset;
    offset.write_float_field(1, 0.0f);  // X
    offset.write_float_field(2, 0.0f);  // Y
    offset.write_float_field(3, 0.0f);  // Z

    proto::Writer payload;
    // field 1 = FacingDirection (varint, signed)
    payload.write_varint_field(1, static_cast<uint64_t>(
        static_cast<int64_t>(options.spawn_facing)));
    // field 2 = SpawnOffset (Vector3 nested)
    payload.write_nested_field(2, offset);

    av::SceneObject obj;
    obj.name    = name;
    obj.pos_x   = options.spawn_x;
    obj.pos_y   = options.spawn_y;
    obj.pos_z   = 0.0f;
    obj.rot_y   = 0.0f;
    obj.scale_x = obj.scale_y = obj.scale_z = 1.0f;
    obj.is_spawn_point  = true;
    obj.spawn_facing    = options.spawn_facing;
    obj.local_aabb      = make_local_aabb(-30.0f, -30.0f, 60.0f, 60.0f);
    // payload field 501 = SpawnPointComponent
    obj.components.push_back(make_component("SpawnPoint", 101, 501, payload));
    return obj;
}

// ============================================================================
// DirectionalLight triple (docs/scenecreator/03 §4.1)
// Three LightComponent (payload field 130) with IDs 101, 103, 105.
//   LightComponent inner fields:
//     1 = Type (varint)
//     2 = Intensity (float)
//     3 = Color (FloatColor nested: 1 R, 2 G, 3 B, 4 A)
// Depth = 620.097656 (observed in every scene).
// ============================================================================
static proto::Writer make_float_color(float r, float g, float b, float a) {
    proto::Writer c;
    c.write_float_field(1, r);
    c.write_float_field(2, g);
    c.write_float_field(3, b);
    c.write_float_field(4, a);
    return c;
}

static av::SceneComponent make_light_component(int comp_id, const LightParams& lp) {
    proto::Writer payload;
    payload.write_varint_field(1, static_cast<uint64_t>(lp.type));
    payload.write_float_field(2, lp.intensity);
    payload.write_nested_field(3, make_float_color(lp.r, lp.g, lp.b, lp.a));
    return make_component("Light", comp_id, 130, payload);
}

av::SceneObject make_directional_light(const Options& options) {
    av::SceneObject obj;
    obj.name    = "DirectionalLight";
    obj.pos_x   = 0.0f;
    obj.pos_y   = 0.0f;
    obj.pos_z   = 620.097656f;  // canonical observed depth
    obj.rot_y   = 0.0f;
    obj.scale_x = obj.scale_y = obj.scale_z = 1.0f;
    obj.local_aabb = make_local_aabb(-30.0f, -30.0f, 60.0f, 60.0f);
    // Canonical triple: Key(101) / Ambient(103) / Shadow-fill(105)
    obj.components.push_back(make_light_component(101, options.key_light));
    obj.components.push_back(make_light_component(103, options.ambient));
    obj.components.push_back(make_light_component(105, options.shadow_fill));
    return obj;
}

// ============================================================================
// Background object (docs/scenecreator/05 §2 BackgroundComponent)
//
// CRITICAL FIX: every shipped scene's Background object carries TWO components
// (verified byte-for-byte against forest_part1/grass_part1/grove_part1):
//
//   Component 1  ClassName 'Model' (id 1)  ModelComponent (payload field 101)
//   Component 2  ClassName 'Background' (id 101)  BackgroundComponent (field 200)
//
// The Model component is REQUIRED. In libswordigo, BackgroundComponent::Draw
// does NOT own a drawable — it renders the fullscreen sky quad through the
// object's Model/Sprite. Emitting only the BackgroundComponent (as we did
// before) leaves the game with nothing to draw: the background renders BLACK /
// disappears and the whole scene looks too dark. The Ruby SDK viewer is lenient
// (it draws the texture from the BackgroundComponent alone), so the bug was
// invisible in-editor but broke in the real game.
//
// The Model component carries NO model name (Name/field 1 absent, exactly like
// vanilla) and a WHITE DiffuseColor (a missing/zero color defaults to alpha 0
// and would render the object invisible).
//
//   BackgroundComponent payload field = 200 (raw tag 0xC2 0x0C)
//   ModelComponent      payload field = 101 (raw tag 0xAA 0x06)
//   field 1 = TextureName (BackgroundComponent) / Name (ModelComponent)
// Depth = 1.72 (observed across all scenes).
// ============================================================================
av::SceneObject make_background(const Options& options) {
    // --- Component 1: Model (the renderable hook the sky quad draws through) ---
    // Matches vanilla exactly: no Name field, white DiffuseColor.
    proto::Writer model_pay;
    model_pay.write_float_field(2, 0.0f);                       // YRotation
    model_pay.write_float_field(3, 0.0f);                       // EmissionFactor
    model_pay.write_float_field(4, 0.0f);                       // XRotation
    model_pay.write_nested_field(5, make_float_color(0, 0, 0, 1)); // ShatterColor
    proto::Writer origin;
    origin.write_float_field(1, 0.0f);
    origin.write_float_field(2, 0.0f);
    origin.write_float_field(3, 0.0f);
    model_pay.write_nested_field(6, origin);                    // Origin
    model_pay.write_varint_field(7, 0);                         // Transparent
    model_pay.write_nested_field(8, make_float_color(1, 1, 1, 1)); // DiffuseColor (white)

    // --- Component 2: Background (the sky texture name) ---
    proto::Writer payload;
    payload.write_string_field(1, resource_stem(options.background));

    av::SceneObject obj;
    obj.name             = "Background";
    obj.background_name  = resource_stem(options.background);
    obj.pos_x            = 0.0f;
    obj.pos_y            = 0.0f;
    obj.pos_z            = 1.72f;   // canonical background depth
    obj.rot_y            = 0.0f;
    obj.scale_x          = obj.scale_y = obj.scale_z = 1.0f;
    // Model FIRST (payload field 101), then Background (payload field 200) —
    // component order matches the shipped scenes.
    obj.components.push_back(make_component("Model", 1, 101, model_pay));
    obj.components.push_back(make_component("Background", 101, 200, payload));
    return obj;
}

// ============================================================================
// Optional base Model object
// ============================================================================
av::SceneObject make_model(const Options& options) {
    proto::Writer payload;
    payload.write_string_field(1, resource_stem(options.base_mesh)); // Name
    payload.write_float_field(2, 0.0f);             // YRotation
    payload.write_float_field(3, 0.0f);             // EmissionFactor
    payload.write_float_field(4, 0.0f);             // XRotation
    payload.write_nested_field(5, make_float_color(0, 0, 0, 1));   // ShatterColor
    proto::Writer origin;                           // Origin (Vector3)
    origin.write_float_field(1, 0.0f);
    origin.write_float_field(2, 0.0f);
    origin.write_float_field(3, 0.0f);
    payload.write_nested_field(6, origin);
    payload.write_varint_field(7, 0);               // Transparent
    // DiffuseColor MUST be white: a missing color defaults to alpha 0 in the
    // real game, rendering the model fully invisible.
    payload.write_nested_field(8, make_float_color(1, 1, 1, 1));   // DiffuseColor

    av::SceneObject obj;
    obj.name      = "base_mesh";
    obj.mesh_name = resource_stem(options.base_mesh);
    obj.rot_y     = 0.0f;
    obj.scale_x   = obj.scale_y = obj.scale_z = 1.0f;
    obj.components.push_back(make_component("Model", 101, 101, payload));
    return obj;
}

// ============================================================================
// Portal object (docs/scenecreator/04 §2 + template §4)
// Three components:
//   Portal         (id 101, payload field 500)
//   CollisionShape (id 102, payload field 120 Rectangle + child 121 props)
//   SpawnPoint     (id 105, payload field 501)
//
// PortalComponent inner fields:
//   1 DestinationSceneName  2 SpawnPointName  3 TapToEnter  4 TriggerShapeId
//
// CollisionShapeComponent inner fields:
//   6 MinDepth  7 MaxDepth  8 SpecialType=2  11 Enabled=1
//
// ShapeComponent inner: field 1 = Rectangle sub-msg {1 X, 2 Y, 3 W, 4 H}
// ============================================================================
av::SceneObject make_portal(const PortalParams& pp) {
    // Portal component
    {
        // prepared below
    }
    proto::Writer portal_pay;
    portal_pay.write_string_field(1, pp.destination);
    portal_pay.write_string_field(2, pp.spawn_name);
    portal_pay.write_varint_field(3, pp.tap_to_enter ? 1ULL : 0ULL);
    portal_pay.write_varint_field(4, 102ULL);  // TriggerShapeId → CollisionShape id 102

    // CollisionShape: shape Rectangle + collision properties
    proto::Writer rect;
    rect.write_float_field(1, pp.rect_x);
    rect.write_float_field(2, pp.rect_y);
    rect.write_float_field(3, pp.rect_w);
    rect.write_float_field(4, pp.rect_h);
    proto::Writer shape_pay;
    shape_pay.write_nested_field(1, rect);  // field 1 = Rectangle inside ShapeComponent

    proto::Writer coll_pay;
    coll_pay.write_float_field(6, pp.min_depth);    // MinDepth
    coll_pay.write_float_field(7, pp.max_depth);    // MaxDepth
    coll_pay.write_varint_field(8, 2ULL);           // SpecialType = 2 (portal zone)
    coll_pay.write_varint_field(11, 1ULL);          // Enabled

    // SpawnPoint component (the return-spawn when coming back)
    proto::Writer sp_offset;
    sp_offset.write_float_field(1, 0.0f);
    sp_offset.write_float_field(2, 0.0f);
    sp_offset.write_float_field(3, 0.0f);
    proto::Writer sp_pay;
    sp_pay.write_varint_field(1, static_cast<uint64_t>(
        static_cast<int64_t>(pp.facing)));
    sp_pay.write_nested_field(2, sp_offset);

    // Object name = "spawn_from_<destination>" (docs/scenecreator/04 §2)
    const std::string obj_name = "spawn_from_" + pp.destination;

    av::SceneObject obj;
    obj.name    = obj_name;
    obj.pos_x   = pp.x;
    obj.pos_y   = pp.y;
    obj.pos_z   = 0.0f;
    obj.rot_y   = 0.0f;
    obj.scale_x = obj.scale_y = obj.scale_z = 1.0f;
    obj.local_aabb = make_local_aabb(-30.0f, -30.0f, 60.0f, 60.0f);

    // Component 101: Portal (payload field 500)
    obj.components.push_back(make_component("Portal", 101, 500, portal_pay));
    // Component 102: ONE CollisionShape component carrying ShapeComponent
    // (field 120 Rectangle) AND CollisionShapeComponent (field 121 props)
    // inline — real scenes + boulder keep both payloads in the same message,
    // and the game reads them from the component the Portal's TriggerShapeId
    // references (a split parent/child structure would drop the props).
    proto::Writer cs_wrap;
    cs_wrap.write_string_field(1, "CollisionShape");
    cs_wrap.write_varint_field(2, 102);
    cs_wrap.write_nested_field(120, shape_pay);
    cs_wrap.write_nested_field(121, coll_pay);
    av::SceneComponent cs_comp;
    cs_comp.type_name     = "CollisionShape";
    cs_comp.type_id       = 102;
    cs_comp.payload_field = 120;
    cs_comp.raw_data      = cs_wrap.to_string();
    obj.components.push_back(std::move(cs_comp));
    // Component 105: SpawnPoint (return spawn, payload field 501)
    obj.components.push_back(make_component("SpawnPoint", 105, 501, sp_pay));

    return obj;
}

// ============================================================================
// ObjectLibrary namespace wrapper (minimal blob with the scene's namespace)
// ============================================================================
std::string make_namespace_library(const std::string& scene_namespace) {
    proto::Writer lib;
    lib.write_string_field(1, scene_namespace);
    return lib.to_string();
}

// ============================================================================
// Manifest writer (.swscene sidecar — editor metadata only)
// ============================================================================
static std::string quote_manifest(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static const char* template_id_str(SceneTemplate t) {
    switch (t) {
    case SceneTemplate::Minimal:   return "Minimal";
    case SceneTemplate::Standard:  return "Standard";
    case SceneTemplate::Outdoor:   return "Outdoor";
    case SceneTemplate::Indoor:    return "Indoor";
    case SceneTemplate::Dungeon:   return "Dungeon";
    case SceneTemplate::BossArena: return "BossArena";
    case SceneTemplate::Portal:    return "Portal";
    case SceneTemplate::Menu:      return "Menu";
    }
    return "Standard";
}

static bool write_manifest(const fs::path& path, const Options& options,
                           const Result& result, std::string& error) {
    std::ostringstream text;
    text << "# Ruby Scene Creator project v2\n";
    text << "scene = "     << quote_manifest(fs::path(result.scene_path).filename().string()) << "\n";
    text << "level_name = " << quote_manifest(options.level_name) << "\n";
    text << "namespace = "  << quote_manifest(options.scene_namespace) << "\n";
    text << "template = "   << quote_manifest(template_id_str(options.scene_template)) << "\n";
    text << "base_mesh = "  << quote_manifest(resource_stem(options.base_mesh)) << "\n";
    text << "background = " << quote_manifest(resource_stem(options.background)) << "\n";
    text << "ground_top_texture = "  << quote_manifest(resource_stem(options.ground_top_texture)) << "\n";
    text << "ground_side_texture = " << quote_manifest(resource_stem(options.ground_side_texture)) << "\n";
    text << "map_link_enabled = "    << (options.link_to_map ? "true" : "false") << "\n";
    text << "map_path = "  << quote_manifest(options.link_to_map ? options.map_path : "") << "\n";
    text << std::fixed << std::setprecision(3);
    text << "platform_size = [" << options.platform_width  << ", "
                                << options.platform_height << ", "
                                << options.platform_depth  << "]\n";
    text << "world_spawn = [" << options.spawn_x << ", "
                              << options.spawn_y << ", "
                              << options.spawn_z << "]\n";
    text << "spawn_facing = " << options.spawn_facing << "\n";
    return write_file(path, text.str(), error);
}

} // namespace (anonymous)

// ============================================================================
// Public helpers
// ============================================================================

std::string resource_stem(const std::string& value) {
    std::string clean = trim(value);
    if (clean.empty()) return {};
    fs::path path(clean);
    std::string name  = path.filename().string();
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    const char* exts[] = {".pod", ".png", ".pvr", ".tex", ".jpg", ".jpeg"};
    bool removed = true;
    while (removed) {
        removed = false;
        for (const char* ext : exts) {
            const size_t len = std::char_traits<char>::length(ext);
            if (lower.size() > len &&
                lower.compare(lower.size() - len, len, ext) == 0) {
                name.resize(name.size() - len);
                lower.resize(lower.size() - len);
                removed = true;
                break;
            }
        }
    }
    return name;
}

const char* template_label(SceneTemplate t) {
    switch (t) {
    case SceneTemplate::Minimal:   return "Minimal — bare starter (ground + spawn + light)";
    case SceneTemplate::Standard:  return "Standard — outdoor story scene (default)";
    case SceneTemplate::Outdoor:   return "Outdoor — wide open world (large bounds)";
    case SceneTemplate::Indoor:    return "Indoor — house / shop (compact bounds + point lights)";
    case SceneTemplate::Dungeon:   return "Dungeon — cave / jail / ice (medium bounds, cave bg)";
    case SceneTemplate::BossArena: return "Boss Arena — arena scale + portal exits";
    case SceneTemplate::Portal:    return "Portal Hub — transition / hub (portal objects)";
    case SceneTemplate::Menu:      return "Menu Scene — attract / idle (menu.scene pattern)";
    }
    return "Standard";
}

const char* template_default_background(SceneTemplate t) {
    switch (t) {
    case SceneTemplate::Outdoor:  return "grasslandsbackground_day";
    case SceneTemplate::Standard: return "grasslandsbackground_day";
    case SceneTemplate::Indoor:   return "townbackground";
    case SceneTemplate::Dungeon:  return "cavesbackground2";
    case SceneTemplate::BossArena:return "fire_background";
    case SceneTemplate::Portal:   return "graveyardback";
    case SceneTemplate::Menu:     return "townbackground";
    case SceneTemplate::Minimal:
    default:                      return "grasslandsbackground_day";
    }
}

// ============================================================================
// create() — main entry point
// ============================================================================
bool create(const Options& input, Result& result, std::string& error) {
    Options options = input;
    options.level_name      = trim(options.level_name);
    options.scene_namespace = trim(options.scene_namespace);
    options.output_path     = trim(options.output_path);
    options.map_path        = trim(options.map_path);
    options.spawn_facing    = options.spawn_facing < 0 ? -1 : 1;

    if (!valid_identifier(options.level_name)) {
        error = "Level name must contain only letters, numbers, '_' or '-'";
        return false;
    }
    if (!valid_identifier(options.scene_namespace)) {
        error = "Scene namespace must contain only letters, numbers, '_' or '-'";
        return false;
    }
    if (!(options.platform_width  >= 32.0f  && options.platform_width  <= 10000.0f) ||
        !(options.platform_height >= 8.0f   && options.platform_height <= 5000.0f)  ||
        !(options.platform_depth  >= 8.0f   && options.platform_depth  <= 5000.0f)) {
        error = "Platform dimensions are outside the supported range";
        return false;
    }

    // Resolve output path → .scene file
    fs::path destination = options.output_path.empty()
        ? fs::path(options.level_name + ".scene")
        : fs::path(options.output_path);
    if (destination.has_extension()) {
        if (destination.extension() != ".scene") {
            error = "Output file must use the .scene extension";
            return false;
        }
    } else {
        destination /= options.level_name + ".scene";
    }
    destination = fs::absolute(destination).lexically_normal();

    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) { error = "Cannot create output directory: " + ec.message(); return false; }
    if (fs::exists(destination, ec)) {
        error = "Scene already exists: " + destination.string();
        return false;
    }

    // ── 1. Generate the starter ground platform via boulder ──────────────────
    boulder::GroundMesh ground;
    const double half_w = options.platform_width  * 0.5;
    const double half_h = options.platform_height * 0.5;
    ground.polygon = {
        {-half_w, -half_h},
        { half_w, -half_h},
        { half_w,  half_h},
        {-half_w,  half_h},
    };
    ground.min_depth    = -options.platform_depth * 0.5;
    ground.max_depth    =  options.platform_depth * 0.5;
    ground.z            = 0.0;
    ground.top_texture  = resource_stem(options.ground_top_texture);
    ground.bottom_texture = resource_stem(options.ground_side_texture);
    if (ground.top_texture.empty())    ground.top_texture    = "fire_grass";
    if (ground.bottom_texture.empty()) ground.bottom_texture = "graveyard_ground";

    const std::string generated = boulder::generate_ground_mesh_object(
        boulder::serialize_swdm(ground), "world_base", 0.0);
    if (generated.empty()) {
        error = "Ground-mesh generator rejected the starter platform";
        return false;
    }

    // Parse the boulder-generated ground object via a temp file
    const fs::path parse_path = destination.string() + ".ruby-create-source.tmp";
    if (!write_file(parse_path, generated, error)) return false;
    av::SceneData scene = av::scene_load(parse_path.string());
    fs::remove(parse_path, ec);
    if (scene.objects.empty()) {
        error = "Generated starter platform could not be parsed";
        return false;
    }

    scene.filename = destination.filename().string();
    scene.filepath = destination.string();

    // ── 2. Background (REQUIRED — docs/scenecreator/06 §1) ──────────────────
    // Fill in the template default background if the user left it blank.
    if (resource_stem(options.background).empty()) {
        options.background = template_default_background(options.scene_template);
    }
    scene.objects.push_back(make_background(options));

    // ── 3. DirectionalLight canonical triple (REQUIRED — docs/03 §4.1) ──────
    scene.objects.push_back(make_directional_light(options));

    // ── 4. spawn_default (REQUIRED — docs/04 §1) ────────────────────────────
    scene.objects.push_back(make_spawn(options));

    // ── 5. Optional base mesh model ──────────────────────────────────────────
    if (!resource_stem(options.base_mesh).empty())
        scene.objects.push_back(make_model(options));

    // ── 6. Portals (docs/04 §2) ─────────────────────────────────────────────
    for (const auto& pp : options.portals)
        scene.objects.push_back(make_portal(pp));

    // ── 7. ObjectLibrary namespace tag ──────────────────────────────────────
    scene.object_libraries.push_back(make_namespace_library(options.scene_namespace));

    // ── 8. Scene Bounds (REQUIRED — docs/02 §1) ─────────────────────────────
    if (options.bounds_w > 0.0f && options.bounds_h > 0.0f) {
        // User-supplied override
        scene.bounds.push_back(
            make_bounds(options.bounds_x, options.bounds_y,
                        options.bounds_w, options.bounds_h));
    } else {
        // Template-derived canonical bounds
        scene.bounds.push_back(
            make_template_bounds(options.scene_template,
                                 options.platform_width,
                                 options.platform_height));
    }

    av::scene_refresh(scene);

    // ── 9. Write .scene ──────────────────────────────────────────────────────
    if (!av::scene_save(destination.string(), scene, &error)) return false;

    result.scene_path    = destination.string();
    result.manifest_path = destination.replace_extension(".swscene").string();
    result.object_count  = static_cast<int>(scene.objects.size());

    if (!write_manifest(result.manifest_path, options, result, error)) {
        std::error_code rem_ec;
        fs::remove(result.scene_path, rem_ec);
        return false;
    }
    return true;
}

} // namespace scenecreate
