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
        std::string top_texture = "graveyard_grass_2x";
        std::string bottom_texture = "graveyard_ground_2x";
    };

    // Parses a .gmesh file content and generates FileRift-compatible GroundMesh markup.
    // Returns empty string on failure.
    std::string generate_ground_mesh(const std::string& gmesh_content);

} // namespace boulder
