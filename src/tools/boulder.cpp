#include "tools/boulder.h"
#include <sstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <algorithm>
#include <cstdint>

/*
 * Boulder GroundMesh Generator C++ Port
 * 
 * Missing Features (documented as requested):
 * - Integration with a live Blender instance/script over localhost (requires networking/socket server).
 * - Noise displacement filter ("HorizNoise") to displace vertices procedurally (left as 0.0).
 */

namespace boulder {

struct Vertex {
    double x, y, z;
    Vector3 normal;
    double u, v;
};

// Top Segment triangle indices templates
static const int topIndicesLeft[4][3] = {
    {0, 4, 5},
    {0, 3, 4},
    {0, 1, 3},
    {1, 2, 3}
};

static const int topIndicesMiddle[6][3] = {
    {6, 10, 11},
    {7, 6, 11},
    {7, 11, 12},
    {8, 7, 12},
    {8, 12, 13},
    {9, 8, 13}
};

static const int topIndicesRight[4][3] = {
    {14, 19, 18},
    {14, 18, 17},
    {14, 17, 15},
    {17, 16, 15}
};

static const double textureSpaceFactor = 1.0 / 250.0;
static const double textureSpaceXOffset = 0.5;
static const double textureSpaceYOffset = 0.5;

static const size_t triSize = 6;
static const size_t vertexSize = 32;

static double distance(Vector2 v1, Vector2 v2) {
    double dx = v2.x - v1.x;
    double dy = v2.y - v1.y;
    return std::sqrt(dx*dx + dy*dy);
}

static Vector2 normalize_v2(Vector2 v) {
    double len = std::sqrt(v.x*v.x + v.y*v.y);
    if (len == 0.0) return {0.0, 0.0};
    return {v.x / len, v.y / len};
}

static Vector3 surface_normal(Vector3 a, Vector3 b, Vector3 c) {
    Vector3 u = {b.x - a.x, b.y - a.y, b.z - a.z};
    Vector3 v = {c.x - a.x, c.y - a.y, c.z - a.z};
    return {
        (u.y * v.z) - (u.z * v.y),
        (u.z * v.x) - (u.x * v.z),
        (u.x * v.y) - (u.y * v.x)
    };
}

static Vector3 normalize_v3(Vector3 v) {
    double mag = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (mag == 0.0) return {0.0, 0.0, 0.0};
    return {v.x / mag, v.y / mag, v.z / mag};
}

static double tex(double f) {
    return (f * textureSpaceFactor) + 0.5;
}

static std::string quote_bytes(const std::vector<uint8_t>& s) {
    const char* hexChars = "0123456789abcdef";
    std::string out;
    for (uint8_t uc : s) {
        if (uc == '"') out += "\\\"";
        else if (uc == '\\') out += "\\\\";
        else if (uc == '\t') out += "\\t";
        else if (uc == '\n') out += "\\n";
        else if (uc == '\r') out += "\\r";
        else if (uc >= 0x20 && uc < 0x7f) out += uc;
        else {
            out += "\\x";
            out += hexChars[uc >> 4];
            out += hexChars[uc & 0xf];
        }
    }
    return out;
}

static void append_ushort(std::vector<uint8_t>& bytes, int val) {
    uint8_t lower = val & 0xff;
    uint8_t higher = (val >> 8) & 0xff;
    bytes.push_back(lower);
    bytes.push_back(higher);
}

static void append_float(std::vector<uint8_t>& bytes, double val) {
    float fval = static_cast<float>(val);
    uint8_t fbytes[4];
    std::memcpy(fbytes, &fval, 4);
    bytes.insert(bytes.end(), fbytes, fbytes + 4);
}

static double edge_angle(PolygonPoint a, PolygonPoint b) {
    double radians = std::atan2(b.y - a.y, b.x - a.x);
    double degrees = radians * (180.0 / M_PI);
    if (degrees < 0.0) degrees += 360.0;
    return degrees;
}

static bool is_top_segment(const GroundMesh& gm, int i) {
    int l = gm.polygon.size();
    if (l == 0) return false;
    int idx1 = (i + l) % l;
    int idx2 = (i + 1 + l) % l;
    double angle = edge_angle(gm.polygon[idx1], gm.polygon[idx2]);
    return std::abs(angle - 180.0) < gm.top_angle;
}

static Vector2 edge_normal(PolygonPoint a, PolygonPoint b) {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    return normalize_v2({dy, -dx});
}

static Vector2 vertex_normal(const std::vector<PolygonPoint>& polygon, int i) {
    int n = polygon.size();
    auto prev = polygon[(i - 1 + n) % n];
    auto curr = polygon[i];
    auto next = polygon[(i + 1) % n];

    auto n1 = edge_normal(prev, curr);
    auto n2 = edge_normal(curr, next);

    Vector2 avg = {n1.x + n2.x, n1.y + n2.y}; // Summing normal components (addition correction)
    return normalize_v2(avg);
}

static std::vector<Vertex> get_top_vertices(double left, double right, double leftHeight, double rightHeight, double minDepth, double maxDepth, double uOffset) {
    leftHeight += 0.05;
    rightHeight += 0.05;
    
    Vector3 upN = normalize_v3(surface_normal(
        {left, leftHeight, minDepth},
        {left, leftHeight, maxDepth},
        {right, rightHeight, maxDepth}
    ));
    Vector3 downN = normalize_v3(surface_normal(
        {right, rightHeight, maxDepth},
        {left, leftHeight, maxDepth},
        {left, leftHeight, minDepth}
    ));
    
    Vector2 leftPoint = {left, leftHeight};
    Vector2 rightPoint = {right, rightHeight};
    double width = distance(leftPoint, rightPoint);
    double frontHeight1 = distance({maxDepth, 0.0}, {maxDepth + 5.0, -10.0});
    double frontHeight2 = distance({maxDepth + 5.0, -10.0}, {maxDepth, -25.0});
    
    return {
        {right, rightHeight, minDepth, upN, tex(uOffset), tex(minDepth)},
        {right, rightHeight, maxDepth, upN, tex(uOffset), tex(maxDepth)},
        {right, rightHeight - 10.0, maxDepth + 5.0, {0, 0, 1}, tex(uOffset - 10.0), tex(maxDepth + 5.0)},
        {right, rightHeight - 25.0, maxDepth, downN, tex(uOffset - 25.0), tex(maxDepth)},
        {right, rightHeight - 25.0, minDepth, downN, tex(uOffset - 25.0), tex(minDepth)},
        {right, rightHeight - 10.0, minDepth - 5.0, {0, 0, 1}, tex(uOffset - 10.0), tex(minDepth - 5.0)},
        {right, rightHeight, minDepth, upN, tex(uOffset), tex(minDepth)},
        {right, rightHeight, maxDepth, upN, tex(uOffset), tex(maxDepth)},
        {right, rightHeight - 10.0, maxDepth + 5.0, {0, 0, 1}, tex(uOffset), tex(maxDepth + frontHeight1)},
        {right, rightHeight - 25.0, maxDepth, downN, tex(uOffset), tex(maxDepth + frontHeight1 + frontHeight2)},
        {left, leftHeight, minDepth, upN, tex(uOffset + width), tex(minDepth)},
        {left, leftHeight, maxDepth, upN, tex(uOffset + width), tex(maxDepth)},
        {left, leftHeight - 10.0, maxDepth + 5.0, {0, 0, 1}, tex(uOffset + width), tex(maxDepth + frontHeight1)},
        {left, leftHeight - 25.0, maxDepth, downN, tex(uOffset + width), tex(maxDepth + frontHeight1 + frontHeight2)},
        {left, leftHeight, minDepth, upN, tex(uOffset + width), tex(minDepth)},
        {left, leftHeight, maxDepth, upN, tex(uOffset + width), tex(maxDepth)},
        {left, leftHeight - 10.0, maxDepth + 5.0, {0, 0, 1}, tex(uOffset + width + 10.0), tex(maxDepth + 5.0)},
        {left, leftHeight - 25.0, maxDepth, downN, tex(uOffset + width + 25.0), tex(maxDepth)},
        {left, leftHeight - 25.0, minDepth, downN, tex(uOffset + width + 25.0), tex(minDepth)},
        {left, leftHeight - 10.0, minDepth - 5.0, {0, 0, 1}, tex(uOffset + width + 10.0), tex(minDepth - 5.0)}
    };
}

static void generate_top_mesh(const GroundMesh& gm, std::vector<uint8_t>& vertexBits, std::vector<uint8_t>& indexBits) {
    int indexOffset = 0;
    double uOffset = 0.0;
    
    for (size_t i = 0; i < gm.polygon.size(); ++i) {
        if (!is_top_segment(gm, i)) continue;
        
        auto curr = gm.polygon[i];
        auto next = gm.polygon[(i + 1) % gm.polygon.size()];
        double left = next.x;
        double right = curr.x;
        if (!is_top_segment(gm, i + 1)) left -= 3.0;
        if (!is_top_segment(gm, i - 1)) right += 3.0;
        
        auto vertices = get_top_vertices(left, right, next.y, curr.y, gm.min_depth, gm.max_depth, uOffset);
        for (const auto& v : vertices) {
            append_float(vertexBits, v.x);
            append_float(vertexBits, v.y);
            append_float(vertexBits, v.z);
            append_float(vertexBits, v.normal.x);
            append_float(vertexBits, v.normal.y);
            append_float(vertexBits, v.normal.z);
            append_float(vertexBits, v.u);
            append_float(vertexBits, v.v);
        }
        
        if (!is_top_segment(gm, i + 1)) {
            for (const auto& tri : topIndicesLeft) {
                append_ushort(indexBits, tri[0] + indexOffset);
                append_ushort(indexBits, tri[1] + indexOffset);
                append_ushort(indexBits, tri[2] + indexOffset);
            }
        }
        for (const auto& tri : topIndicesMiddle) {
            append_ushort(indexBits, tri[0] + indexOffset);
            append_ushort(indexBits, tri[1] + indexOffset);
            append_ushort(indexBits, tri[2] + indexOffset);
        }
        if (!is_top_segment(gm, i - 1)) {
            for (const auto& tri : topIndicesRight) {
                append_ushort(indexBits, tri[0] + indexOffset);
                append_ushort(indexBits, tri[1] + indexOffset);
                append_ushort(indexBits, tri[2] + indexOffset);
            }
        }
        
        indexOffset += 20;
        if (is_top_segment(gm, i + 1)) {
            uOffset += distance({gm.polygon[i].x, gm.polygon[i].y}, {gm.polygon[(i+1)%gm.polygon.size()].x, gm.polygon[(i+1)%gm.polygon.size()].y});
        } else {
            uOffset = 0.0;
        }
    }
}

static void generate_side_mesh(const GroundMesh& gm, std::vector<uint8_t>& vertexBits, std::vector<uint8_t>& indexBits) {
    PolygonPoint prevVertex;
    double totalDistance = 0.5;
    
    for (size_t i = 0; i < gm.polygon.size(); ++i) {
        auto vertex = gm.polygon[i];
        if (i != 0) {
            totalDistance += distance({vertex.x, vertex.y}, {prevVertex.x, prevVertex.y}) * textureSpaceFactor;
        }
        prevVertex = vertex;
        auto normal = vertex_normal(gm.polygon, i);
        
        double depths[2] = {gm.min_depth + 5.0, gm.max_depth - 5.0};
        for (double d : depths) {
            append_float(vertexBits, vertex.x);
            append_float(vertexBits, vertex.y);
            append_float(vertexBits, d);
            append_float(vertexBits, normal.x);
            append_float(vertexBits, normal.y);
            append_float(vertexBits, 0.0);
            append_float(vertexBits, totalDistance);
            append_float(vertexBits, d * textureSpaceFactor + 0.5);
        }
    }
    
    auto lastPoint = gm.polygon[0];
    auto lastNormal = vertex_normal(gm.polygon, 0);
    totalDistance += distance({lastPoint.x, lastPoint.y}, {prevVertex.x, prevVertex.y}) * textureSpaceFactor;
    
    double depths[2] = {gm.min_depth + 5.0, gm.max_depth - 5.0};
    for (double d : depths) {
        append_float(vertexBits, lastPoint.x);
        append_float(vertexBits, lastPoint.y);
        append_float(vertexBits, d);
        append_float(vertexBits, lastNormal.x);
        append_float(vertexBits, lastNormal.y);
        append_float(vertexBits, 0.0);
        append_float(vertexBits, totalDistance);
        append_float(vertexBits, d * textureSpaceFactor + 0.5);
    }
    
    for (size_t v = 0; v < gm.polygon.size(); ++v) {
        auto curr = gm.polygon[v];
        auto next = gm.polygon[(v + 1) % gm.polygon.size()];
        double angle = edge_angle(curr, next);
        
        if (gm.generate_top && std::abs(angle - 180.0) < gm.top_angle) {
            continue;
        }
        
        int i = v * 2;
        append_ushort(indexBits, i);
        append_ushort(indexBits, i + 2);
        append_ushort(indexBits, i + 3);
        
        append_ushort(indexBits, i);
        append_ushort(indexBits, i + 3); // Fix index mapping order from sidemesh.go
        append_ushort(indexBits, i - 1 + 2); // Matches `i-1` and `i+2` lower logic
    }
}

static double get_cross(PolygonPoint a, PolygonPoint b, PolygonPoint c) {
    return (a.x - c.x)*(b.y - c.y) - (a.y - c.y)*(b.x - c.x);
}

static bool is_point_within(PolygonPoint a, PolygonPoint b, PolygonPoint c, PolygonPoint target) {
    if (get_cross(a, b, target) < 0.0) return false;
    if (get_cross(b, c, target) < 0.0) return false;
    if (get_cross(c, a, target) < 0.0) return false;
    return true;
}

static bool is_an_ear(int a, int b, int c, const std::vector<PolygonPoint>& v) {
    if (get_cross(v[a], v[b], v[c]) < 0.0) return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != static_cast<size_t>(a) && i != static_cast<size_t>(b) && i != static_cast<size_t>(c)) {
            if (is_point_within(v[a], v[b], v[c], v[i])) return false;
        }
    }
    return true;
}

static std::vector<uint8_t> make_tri(PolygonPoint a, PolygonPoint b, PolygonPoint c, const GroundMesh& gm) {
    std::vector<uint8_t> bits;
    PolygonPoint verts[3] = {a, b, c};
    for (const auto& vertex : verts) {
        append_float(bits, vertex.x);
        append_float(bits, vertex.y);
        append_float(bits, gm.max_depth - 5.0);
        append_float(bits, 0.0);
        append_float(bits, 0.0);
        append_float(bits, 1.0);
        
        double u = (vertex.x * textureSpaceFactor) + textureSpaceXOffset;
        double v = (vertex.y * textureSpaceFactor) + textureSpaceYOffset;
        append_float(bits, u);
        append_float(bits, v);
    }
    return bits;
}

static std::vector<uint8_t> generate_face_mesh(GroundMesh face) {
    std::vector<uint8_t> bits;
    if (face.polygon.size() < 3) return {};
    
    while (face.polygon.size() > 3) {
        bool found_ear = false;
        for (size_t i = 0; i < face.polygon.size() - 2; ++i) {
            if (is_an_ear(i, i + 1, i + 2, face.polygon)) {
                auto tri_bits = make_tri(face.polygon[i], face.polygon[i+1], face.polygon[i+2], face);
                bits.insert(bits.end(), tri_bits.begin(), tri_bits.end());
                face.polygon.erase(face.polygon.begin() + i + 1);
                found_ear = true;
                break;
            }
        }
        if (!found_ear) return bits;
    }
    
    auto tri_bits = make_tri(face.polygon[0], face.polygon[1], face.polygon[2], face);
    bits.insert(bits.end(), tri_bits.begin(), tri_bits.end());
    return bits;
}

static GroundMesh parse_gmesh(const std::string& content) {
    GroundMesh gm;
    std::stringstream ss(content);
    std::string line;
    bool in_vertex = false;
    
    while (std::getline(ss, line)) {
        size_t comment_pos = line.find("//");
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1, std::string::npos);
        if (line.empty()) continue;
        
        if (in_vertex) {
            if (line == "]") {
                in_vertex = false;
                continue;
            }
            std::stringstream line_ss(line);
            double x, y;
            if (line_ss >> x >> y) {
                gm.polygon.push_back({x, y});
            }
            continue;
        }
        
        if (line.rfind("Vertex[", 0) == 0 || line.rfind("Vertex [", 0) == 0) {
            in_vertex = true;
            continue;
        }
        
        std::stringstream line_ss(line);
        std::string key;
        line_ss >> key;
        
        if (key == "MinDepth") {
            line_ss >> gm.min_depth;
        } else if (key == "MaxDepth") {
            line_ss >> gm.max_depth;
        } else if (key == "TopAngle") {
            line_ss >> gm.top_angle;
        } else if (key == "GenerateTop") {
            std::string val;
            line_ss >> val;
            gm.generate_top = (val == "true");
        } else if (key == "TopTexture") {
            std::string val;
            line_ss >> val;
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            gm.top_texture = val;
        } else if (key == "BottomTexture") {
            std::string val;
            line_ss >> val;
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            gm.bottom_texture = val;
        }
    }
    
    return gm;
}

std::string generate_ground_mesh(const std::string& gmesh_content) {
    GroundMesh gm = parse_gmesh(gmesh_content);
    if (gm.polygon.size() < 3) return "";
    
    double left = gm.polygon[0].x;
    double right = gm.polygon[0].x;
    double bottom = gm.polygon[0].y;
    double top = gm.polygon[0].y;
    
    std::stringstream poly_stream;
    for (const auto& v : gm.polygon) {
        poly_stream << "                    Vertex{ X : " << v.x << " Y : " << v.y << " }\n";
        left = std::min(left, v.x);
        right = std::max(right, v.x);
        bottom = std::min(bottom, v.y);
        top = std::max(top, v.y);
    }
    
    char aabb_str[256];
    snprintf(aabb_str, sizeof(aabb_str), "X : %f Y : %f Z : -50.0 Width : %f Height : %f Depth : 100.0", left, bottom, right - left, top - bottom);
    char square_str[256];
    snprintf(square_str, sizeof(square_str), "X : %f Y : %f Width : %f Height : %f", left, bottom, right - left, top - bottom);
    
    std::vector<uint8_t> top_v, top_i;
    if (gm.generate_top) {
        generate_top_mesh(gm, top_v, top_i);
    }
    
    std::vector<uint8_t> side_v, side_i;
    generate_side_mesh(gm, side_v, side_i);
    
    std::vector<uint8_t> face_v = generate_face_mesh(gm);
    
    std::string top_mesh_markup = "";
    if (gm.generate_top) {
        char top_desc[512];
        snprintf(top_desc, sizeof(top_desc), 
            "                SurfaceMesh{\n"
            "                    NumVertices : %d\n"
            "                    NumFaces : %d\n"
            "                    Indices{ ValueType : 4 ValuesPerVertex : 1 Stride : 2 DataOffset : 0 }\n"
            "                    Vertices{ ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 0 }\n"
            "                    Normals{ ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 12 }\n"
            "                    TexCoordSet{ ValueType : 7 ValuesPerVertex : 2 Stride : 32 DataOffset : 24 }\n"
            "                    Material{\n"
            "                        AmbientColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
            "                        DiffuseColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
            "                        SpecularColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
            "                        Shininess : 0.0\n"
            "                        Texture{ Name : '%s' PixelFormat : 1 ImageType : 2 }\n"
            "                    }\n"
            "                    BoundingBox{ %s }\n"
            "                    VertexData : '%s'\n"
            "                    IndexData : '%s'\n"
            "                }\n",
            static_cast<int>(top_v.size() / vertexSize),
            static_cast<int>(top_i.size() / triSize),
            gm.top_texture.c_str(),
            aabb_str,
            quote_bytes(top_v).c_str(),
            quote_bytes(top_i).c_str()
        );
        top_mesh_markup = top_desc;
    }
    
    char main_desc[4096];
    snprintf(main_desc, sizeof(main_desc),
        "        Component{\n"
        "            ClassName : 'GroundPolygon'\n"
        "            Identifier : 980\n"
        "            GroundPolygonComponent{\n"
        "                Polygon{\n"
        "%s"
        "                    Convex : 0\n"
        "                    Closed : 1\n"
        "                }\n"
        "                Collides : 1\n"
        "                MinDepth : %f\n"
        "                MaxDepth : %f\n"
        "            }\n"
        "        }\n"
        "        Component{\n"
        "            ClassName : 'GroundMesh'\n"
        "            Identifier : 981\n"
        "            GroundMeshComponent{\n"
        "                LocalAabb{ %s }\n"
        "%s"
        "                // side mesh\n"
        "                SurfaceMesh{\n"
        "                    NumVertices : %d\n"
        "                    NumFaces : %d\n"
        "                    Indices{ ValueType : 4 ValuesPerVertex : 1 Stride : 2 DataOffset : 0 }\n"
        "                    Vertices{ ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 0 }\n"
        "                    Normals{ ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 12 }\n"
        "                    TexCoordSet{ ValueType : 7 ValuesPerVertex : 2 Stride : 32 DataOffset : 24 }\n"
        "                    Material{\n"
        "                        AmbientColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "                        DiffuseColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "                        SpecularColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "                        Shininess : 0.0\n"
        "                        Texture{ Name : '%s' PixelFormat : 1 ImageType : 2 }\n"
        "                    }\n"
        "                    BoundingBox{ %s }\n"
        "                    VertexData : '%s'\n"
        "                    IndexData : '%s'\n"
        "                }\n"
        "                FrontMesh{\n"
        "                    NumVertices : %d\n"
        "                    NumFaces : %d\n"
        "                    Vertices{ ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 0 }\n"
        "                    Normals{ ValueType : 7 ValuesPerVertex : 3 Stride : 32 DataOffset : 12 }\n"
        "                    TexCoordSet{ ValueType : 7 ValuesPerVertex : 2 Stride : 32 DataOffset : 24 }\n"
        "                    Material{\n"
        "                        AmbientColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "                        DiffuseColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "                        SpecularColor{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "                        Shininess : 0.0\n"
        "                        Texture{ Name : '%s' PixelFormat : 1 ImageType : 2 }\n"
        "                    }\n"
        "                    BoundingBox{ %s }\n"
        "                    VertexData : '%s'\n"
        "                }\n"
        "                Color{ R : 1.0 G : 1.0 B : 1.0 A : 1.0 }\n"
        "            }\n"
        "        }\n"
        "        Component{\n"
        "            ClassName : 'GroundMeshGenerator'\n"
        "            Identifier : 982\n"
        "            GroundMeshGeneratorComponent{\n"
        "                GroundPolygonId : 980\n"
        "                TargetMeshId : 981\n"
        "                FrontTextureMappingId : 985\n"
        "                SurfaceTextureMappingId : 984\n"
        "                RandomSeed : 1291618994\n"
        "                HorizNoise : 0.0\n"
        "                MeshType : 1\n"
        "                SurfaceWidth : 80.0\n"
        "                HatHeight : 25.0\n"
        "                HatWidthOffset1 : 5.0\n"
        "                HatWidthOffset2 : 5.0\n"
        "            }\n"
        "        }\n"
        "        Component{\n"
        "            ClassName : 'CollisionShape'\n"
        "            Identifier : 983\n"
        "            ParentComponentIdentifier : 980\n"
        "            ShapeComponent{\n"
        "                Polygon{\n"
        "%s"
        "                    Convex : 0\n"
        "                    Closed : 1\n"
        "                }\n"
        "            }\n"
        "            CollisionShapeComponent{\n"
        "                IsGround : 1\n"
        "                MinDepth : %f\n"
        "                MaxDepth : %f\n"
        "                Enabled : 1\n"
        "            }\n"
        "        }\n"
        "        Component{\n"
        "            ClassName : 'TextureMapping'\n"
        "            Identifier : 984\n"
        "            TextureMappingComponent{\n"
        "                TextureName : '%s'\n"
        "                Scale : 250.0\n"
        "                Offset{ X : 0.0 Y : 0.0 }\n"
        "            }\n"
        "        }\n"
        "        Component{\n"
        "            ClassName : 'TextureMapping'\n"
        "            Identifier : 985\n"
        "            TextureMappingComponent{\n"
        "                TextureName : '%s'\n"
        "                Scale : 250.0\n"
        "                Offset{ X : 0.0 Y : 0.0 }\n"
        "            }\n"
        "        }\n"
        "        LocalAabb{ %s }\n",
        poly_stream.str().c_str(),
        gm.min_depth,
        gm.max_depth,
        square_str,
        top_mesh_markup.c_str(),
        static_cast<int>(side_v.size() / vertexSize),
        static_cast<int>(side_i.size() / triSize),
        gm.bottom_texture.c_str(),
        aabb_str,
        quote_bytes(side_v).c_str(),
        quote_bytes(side_i).c_str(),
        static_cast<int>(face_v.size() / vertexSize),
        static_cast<int>(face_v.size() / (3 * vertexSize)),
        gm.bottom_texture.c_str(),
        aabb_str,
        quote_bytes(face_v).c_str(),
        poly_stream.str().c_str(),
        gm.min_depth,
        gm.max_depth,
        gm.top_texture.c_str(),
        gm.bottom_texture.c_str(),
        square_str
    );
    
    return main_desc;
}

} // namespace boulder
