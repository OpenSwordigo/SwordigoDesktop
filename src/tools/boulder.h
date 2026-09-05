#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace boulder {

    struct PolygonPoint {
        double x, y;
    };

    struct Vector3 {
        double x, y, z;
    };

    struct Vector2 {
        double x, y;
    };

    // A "round hat" dome (Caver::GroundMeshGenerator::InsertRoundHatVertices /
    // InsertCapForRoundHat): a rounded bump standing on the surface of the
    // ground mesh. The footprint is a circle of radius r centered at (x,y) in
    // sketch space; the dome rises `height` above the polygon's top edge.
    struct Hat {
        double x = 0.0, y = 0.0;
        double radius = 60.0;
        double height = 40.0;
    };

    struct GroundMesh {
        std::vector<PolygonPoint> polygon;
        std::vector<Hat> hats;               // round-hat domes on the surface
        double min_depth = -45.0;
        double max_depth = 45.0;
        double top_angle = 20.0;
        bool generate_top = true;
        double z = 40.0;                     // constant Z (world depth) for the whole mesh
        std::string top_texture = "fire_grass";
        std::string bottom_texture = "graveyard_ground";
        double surface_width = 80.0;
        double hat_height = 25.0;
        double hat_width_offset_1 = 5.0;
        double hat_width_offset_2 = 5.0;
        double texture_scale = 250.0;
        uint32_t random_seed = 1291618994u;
    };

    struct GroundComponentIds {
        int polygon_id = 980;
        int mesh_id = 981;
        int generator_id = 982;
        int collision_id = 983;
        int tm_surface_id = 984;
        int tm_front_id = 985;
    };

    // Parses a .gmesh / .swdm file content and generates FileRift-compatible
    // GroundMesh markup. Returns empty string on failure.
    std::string generate_ground_mesh(const std::string& gmesh_content);

    // Parse .gmesh / .swdm content into a GroundMesh struct (exposed for the editor).
    GroundMesh parse_ground_mesh(const std::string& content);

    // Serialize a GroundMesh to .swdm text (round-trips with parse_ground_mesh).
    std::string serialize_swdm(const GroundMesh& gm);

    // Build a COMPLETE GroundMesh scene object directly as protobuf binary bytes
    // (Scene field 1 = single Object). This bypasses the lossy markup->recode
    // text pipeline that corrupts embedded binary mesh data. The generated object
    // carries GroundPolygon + GroundMesh (SurfaceMesh/FrontMesh) + Generator +
    // TextureMapping components, exactly like boulder's reference markup.
    // Returns empty string on failure (polygon < 3 points, etc).
    std::string generate_ground_mesh_object(const std::string& gmesh_content,
                                            const std::string& identifier,
                                            double depth,
                                            const GroundComponentIds* ids = nullptr);

} // namespace boulder
