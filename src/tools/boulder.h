#pragma once
#include <string>
#include <vector>

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

    struct GroundMesh {
        std::vector<PolygonPoint> polygon;
        double min_depth = -45.0;
        double max_depth = 45.0;
        double top_angle = 20.0;
        bool generate_top = true;
        double z = 40.0;                     // constant Z (world depth) for the whole mesh
        std::string top_texture = "fire_grass";
        std::string bottom_texture = "graveyard_ground";
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
                                            double depth);

} // namespace boulder
