#include "tools/boulder.h"
#include "platform/protobuf_reader.h"
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

// Round hat — a smooth dome standing on the surface (the polygon's top edge).
// Ported structure from the game's GroundMeshGenerator::InsertRoundHatVertices
// / InsertCapForRoundHat (Caver @0x2BA45C / @0x2BA724): a profile ring of
// vertices around the hat footprint plus cap faces closing the dome. The
// cross-section is a half-ellipse (base radius r, height h) and the ridge
// spans the full Min..Max depth like the top surface, so it reads as a
// rounded bump on the ground mesh in-game.
static void generate_hat_mesh(const GroundMesh& gm, const Hat& hat,
                              std::vector<uint8_t>& vertexBits,
                              std::vector<uint8_t>& indexBits) {
    const int N = 18;                     // cross-section segments
    double base = gm.polygon.empty() ? 0.0 : gm.polygon[0].y;
    for (const auto& p : gm.polygon) base = std::max(base, p.y);
    base += 0.05;   // lift off the top surface plane (anti z-fighting)
    const double r = std::max(1.0, hat.radius);
    const double h = std::max(1.0, hat.height);
    const double cx = hat.x;
    // Guard against an inverted Min/Max depth range (the editor lets the user
    // drag Min above Max) — a reversed range flips the dome inside-out.
    double z0 = gm.min_depth, z1 = gm.max_depth;
    if (z0 > z1) std::swap(z0, z1);

    const double pi = 3.14159265358979323846;
    auto add_v = [&](double x, double y, double z, const Vector3& n) {
        append_float(vertexBits, x);
        append_float(vertexBits, y);
        append_float(vertexBits, z);
        append_float(vertexBits, n.x);
        append_float(vertexBits, n.y);
        append_float(vertexBits, n.z);
        append_float(vertexBits, tex(x));
        append_float(vertexBits, tex(y));
    };

    // Profile arc: left base -> apex -> right base with outward normals.
    std::vector<int> ring0(N + 1), ring1(N + 1);
    for (int t = 0; t <= N; ++t) {
        const double phi = pi * (double)t / (double)N;
        const double px = cx + r * std::cos(phi);
        const double py = base + h * std::sin(phi);
        const Vector3 n = normalize_v3({std::cos(phi), std::sin(phi), 0.0});
        ring0[t] = (int)(vertexBits.size() / vertexSize);
        add_v(px, py, z0, n);             // back depth
        ring1[t] = (int)(vertexBits.size() / vertexSize);
        add_v(px, py, z1, n);             // front depth
    }
    // Ridge sheet (same winding convention as the top surface mesh so the
    // outward face keeps the correct orientation).
    for (int t = 0; t < N; ++t) {
        const int a = ring0[t],  b = ring0[t + 1];
        const int c = ring1[t + 1], d = ring1[t];
        append_ushort(indexBits, a); append_ushort(indexBits, b); append_ushort(indexBits, c);
        append_ushort(indexBits, a); append_ushort(indexBits, c); append_ushort(indexBits, d);
    }
    // End caps: fans from the bottom-center close the dome at both depths.
    const int cb = (int)(vertexBits.size() / vertexSize);
    add_v(cx, base, z0, {0.0, 0.0, -1.0});
    for (int t = 0; t < N; ++t) {
        append_ushort(indexBits, cb); append_ushort(indexBits, ring0[t + 1]); append_ushort(indexBits, ring0[t]);
    }
    const int cf = (int)(vertexBits.size() / vertexSize);
    add_v(cx, base, z1, {0.0, 0.0, 1.0});
    for (int t = 0; t < N; ++t) {
        append_ushort(indexBits, cf); append_ushort(indexBits, ring1[t]); append_ushort(indexBits, ring1[t + 1]);
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
    bool in_hat = false;
    
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
        if (in_hat) {
            if (line == "]") {
                in_hat = false;
                continue;
            }
            std::stringstream line_ss(line);
            double x, y, r, h;
            if (line_ss >> x >> y >> r >> h) {
                gm.hats.push_back({x, y, r, h});
            }
            continue;
        }
        
        if (line.rfind("Hat[", 0) == 0 || line.rfind("Hat [", 0) == 0) {
            in_hat = true;
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
        } else if (key == "Z") {
            line_ss >> gm.z;
        }
    }
    
    return gm;
}

GroundMesh parse_ground_mesh(const std::string& content) {
    return parse_gmesh(content);
}

std::string serialize_swdm(const GroundMesh& gm) {
    std::stringstream ss;
    ss << "// Swordigo Desktop Mesh (.swdm)\n";
    ss << "Z " << gm.z << "\n";
    ss << "MinDepth " << gm.min_depth << "\n";
    ss << "MaxDepth " << gm.max_depth << "\n";
    ss << "TopAngle " << gm.top_angle << "\n";
    ss << "GenerateTop " << (gm.generate_top ? "true" : "false") << "\n";
    ss << "TopTexture \"" << gm.top_texture << "\"\n";
    ss << "BottomTexture \"" << gm.bottom_texture << "\"\n";
    ss << "Hat[\n";
    for (const auto& h : gm.hats)
        ss << h.x << " " << h.y << " " << h.radius << " " << h.height << "\n";
    ss << "]\n";
    ss << "Vertex[\n";
    for (const auto& v : gm.polygon)
        ss << v.x << " " << v.y << "\n";
    ss << "]\n";
    return ss.str();
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

// ============================================================================
// Direct protobuf builder — builds a complete GroundMesh scene object as
// binary Scene bytes without the lossy markup->recode text round-trip.
// ============================================================================

namespace {

// Helper writers matching the Swordigo protobuf schemas.
static proto::Writer make_vector2(double x, double y) {
    proto::Writer w;
    w.write_float_field(1, static_cast<float>(x));
    w.write_float_field(2, static_cast<float>(y));
    return w;
}

static proto::Writer make_rectangle(double x, double y, double width, double height) {
    proto::Writer w;
    w.write_float_field(1, static_cast<float>(x));
    w.write_float_field(2, static_cast<float>(y));
    w.write_float_field(3, static_cast<float>(width));
    w.write_float_field(4, static_cast<float>(height));
    return w;
}

static proto::Writer make_float_color(float r, float g, float b, float a) {
    proto::Writer w;
    w.write_float_field(1, r);
    w.write_float_field(2, g);
    w.write_float_field(3, b);
    w.write_float_field(4, a);
    return w;
}

// MeshMaterial{ Texture{ Name, PixelFormat=1, ImageType=1 } } (field 5 of material)
static proto::Writer make_mesh_material(const std::string& texture_name) {
    proto::Writer tex;
    tex.write_string_field(1, texture_name);
    tex.write_varint_field(2, 1);   // PixelFormat (1 = RGBA8888)
    tex.write_varint_field(4, 1);   // ImageType (1 = PNG, matches real scene data)
    proto::Writer mat;
    mat.write_nested_field(1, make_float_color(1, 1, 1, 1)); // AmbientColor
    mat.write_nested_field(2, make_float_color(1, 1, 1, 1)); // DiffuseColor
    mat.write_nested_field(3, make_float_color(1, 1, 1, 1)); // SpecularColor
    mat.write_float_field(4, 0.0f); // Shininess
    mat.write_nested_field(5, tex); // Texture
    return mat;
}

// MeshData{ ValueType, ValuesPerVertex, Stride, DataOffset } — the layout
// descriptors the game uses to decode the interleaved VertexData stream.
// (Real scenes: 4=uint16 for indices, 7=float32 for pos/nrm/uv.)
static proto::Writer make_mesh_data(int value_type, int values_per_vertex,
                                    int stride, int data_offset) {
    proto::Writer w;
    w.write_varint_field(1, static_cast<uint64_t>(value_type));
    w.write_varint_field(2, static_cast<uint64_t>(values_per_vertex));
    w.write_varint_field(3, static_cast<uint64_t>(stride));
    w.write_varint_field(4, static_cast<uint64_t>(data_offset));
    return w;
}

// Box{ X, Y, Z, Width, Height, Depth } (6 float fields, matches scene_schemas).
static proto::Writer make_box(float x, float y, float z,
                              float width, float height, float depth) {
    proto::Writer w;
    w.write_float_field(1, x);
    w.write_float_field(2, y);
    w.write_float_field(3, z);
    w.write_float_field(4, width);
    w.write_float_field(5, height);
    w.write_float_field(6, depth);
    return w;
}

// Axis-aligned bounds of the interleaved vertex stream (pos = first 3 floats).
static void vertex_bounds(const std::vector<uint8_t>& vertex_bits,
                          float* mn, float* mx) {
    mn[0] = mn[1] = mn[2] = 1e9f;
    mx[0] = mx[1] = mx[2] = -1e9f;
    const int n = static_cast<int>(vertex_bits.size() / vertexSize);
    for (int i = 0; i < n; ++i) {
        const float* p = reinterpret_cast<const float*>(vertex_bits.data() + i * vertexSize);
        for (int c = 0; c < 3; ++c) {
            if (p[c] < mn[c]) mn[c] = p[c];
            if (p[c] > mx[c]) mx[c] = p[c];
        }
    }
    if (n == 0) { mn[0] = mn[1] = mn[2] = 0.0f; mx[0] = mx[1] = mx[2] = 0.0f; }
}

// Mesh{ NumVertices, NumFaces, Indices/Vertices/Normals/TexCoordSet (MeshData
// layout descriptors — REQUIRED by the game, without them the mesh is invisible
// while collision still works), Material, BoundingBox, VertexData(50), IndexData(51) }
static proto::Writer make_mesh(const std::vector<uint8_t>& vertex_bits,
                               const std::vector<uint8_t>& index_bits,
                               const std::string& texture_name) {
    proto::Writer w;
    const int num_vertices = static_cast<int>(vertex_bits.size() / vertexSize);
    // Non-indexed meshes (FrontMesh) encode one triangle per 3 vertices.
    const int num_faces = !index_bits.empty()
        ? static_cast<int>(index_bits.size() / triSize)
        : static_cast<int>(vertex_bits.size() / (3 * vertexSize));
    w.write_varint_field(1, static_cast<uint64_t>(num_vertices));
    w.write_varint_field(2, static_cast<uint64_t>(num_faces));
    // Interleaved layout: pos(3f) @0, normal(3f) @12, uv(2f) @24, stride 32.
    if (!index_bits.empty())
        w.write_nested_field(3, make_mesh_data(4, 1, 2, 0));    // Indices (uint16)
    w.write_nested_field(4, make_mesh_data(7, 3, 32, 0));       // Vertices
    w.write_nested_field(5, make_mesh_data(7, 3, 32, 12));      // Normals
    w.write_nested_field(6, make_mesh_data(7, 2, 32, 24));      // TexCoordSet
    w.write_nested_field(10, make_mesh_material(texture_name));
    float mn[3], mx[3];
    vertex_bounds(vertex_bits, mn, mx);
    w.write_nested_field(11, make_box(mn[0], mn[1], mn[2],
                                      mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]));
    w.write_bytes_field(50, std::string(vertex_bits.begin(), vertex_bits.end()));
    if (!index_bits.empty())
        w.write_bytes_field(51, std::string(index_bits.begin(), index_bits.end()));
    return w;
}

} // namespace

std::string generate_ground_mesh_object(const std::string& gmesh_content,
                                        const std::string& identifier,
                                        double depth) {
    GroundMesh gm = parse_gmesh(gmesh_content);
    if (gm.polygon.size() < 3) return "";

    double left = gm.polygon[0].x, right = gm.polygon[0].x;
    double bottom = gm.polygon[0].y, top = gm.polygon[0].y;
    for (const auto& v : gm.polygon) {
        left = std::min(left, v.x);   right = std::max(right, v.x);
        bottom = std::min(bottom, v.y); top = std::max(top, v.y);
    }

    // ── geometry (identical to the markup path) ──
    std::vector<uint8_t> top_v, top_i;
    if (gm.generate_top) generate_top_mesh(gm, top_v, top_i);
    std::vector<uint8_t> side_v, side_i;
    generate_side_mesh(gm, side_v, side_i);
    std::vector<uint8_t> face_v = generate_face_mesh(gm);

    // ── round-hat domes: one SurfaceMesh per hat (top texture) ──
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> hat_meshes;
    for (const auto& hat : gm.hats) {
        std::vector<uint8_t> hv, hi;
        generate_hat_mesh(gm, hat, hv, hi);
        if (!hv.empty()) hat_meshes.emplace_back(std::move(hv), std::move(hi));
    }

    // ── polygon message (used by GroundPolygon + CollisionShape) ──
    proto::Writer poly;
    for (const auto& v : gm.polygon)
        poly.write_nested_field(1, make_vector2(v.x, v.y)); // Polygon.Vertex
    poly.write_varint_field(2, 0); // Convex
    poly.write_varint_field(3, 1); // Closed

    // ── GroundPolygonComponent ──
    proto::Writer gpc;
    gpc.write_nested_field(2, poly);                 // Polygon
    gpc.write_varint_field(3, 1);                    // Collides
    gpc.write_float_field(4, static_cast<float>(gm.min_depth)); // MinDepth
    gpc.write_float_field(5, static_cast<float>(gm.max_depth)); // MaxDepth
    proto::Writer comp_ground_polygon;
    comp_ground_polygon.write_string_field(1, "GroundPolygon");
    comp_ground_polygon.write_varint_field(2, 980);
    comp_ground_polygon.write_nested_field(110, gpc); // Component.GroundPolygonComponent

    // ── GroundMeshComponent ──
    // Matches the real game layout: SurfaceMesh (field 8, repeated — top + side
    // walls), FrontMesh (field 9, non-indexed), LocalAabb (7), Color (10).
    proto::Writer gmc;
    gmc.write_nested_field(7, make_rectangle(left, bottom, right - left, top - bottom)); // LocalAabb
    if (!top_v.empty())
        gmc.write_nested_field(8, make_mesh(top_v, top_i, gm.top_texture));   // SurfaceMesh (top)
    if (!side_v.empty())
        gmc.write_nested_field(8, make_mesh(side_v, side_i, gm.bottom_texture)); // SurfaceMesh (side walls)
    if (!face_v.empty())
        gmc.write_nested_field(9, make_mesh(face_v, {}, gm.bottom_texture));  // FrontMesh (non-indexed)
    for (const auto& hm : hat_meshes)
        gmc.write_nested_field(8, make_mesh(hm.first, hm.second, gm.top_texture)); // RoundHat mesh
    gmc.write_nested_field(10, make_float_color(1, 1, 1, 1)); // Color
    proto::Writer comp_ground_mesh;
    comp_ground_mesh.write_string_field(1, "GroundMesh");
    comp_ground_mesh.write_varint_field(2, 981);
    comp_ground_mesh.write_nested_field(111, gmc); // Component.GroundMeshComponent

    // ── GroundMeshGeneratorComponent ──
    proto::Writer ggc;
    ggc.write_varint_field(1, 980);  // GroundPolygonId
    ggc.write_varint_field(2, 981);  // TargetMeshId
    ggc.write_varint_field(3, 985);  // FrontTextureMappingId
    ggc.write_varint_field(4, 984);  // SurfaceTextureMappingId
    ggc.write_varint_field(5, 1291618994u); // RandomSeed
    ggc.write_float_field(6, 0.0f);  // HorizNoise
    ggc.write_varint_field(7, 1);    // MeshType
    ggc.write_float_field(8, 80.0f); // SurfaceWidth
    ggc.write_float_field(9, 25.0f); // HatHeight
    ggc.write_float_field(10, 5.0f); // HatWidthOffset1
    ggc.write_float_field(11, 5.0f); // HatWidthOffset2
    proto::Writer comp_generator;
    comp_generator.write_string_field(1, "GroundMeshGenerator");
    comp_generator.write_varint_field(2, 982);
    comp_generator.write_nested_field(112, ggc); // Component.GroundMeshGeneratorComponent

    // ── CollisionShape (ShapeComponent polygon + CollisionShapeComponent) ──
    proto::Writer shape;
    shape.write_nested_field(3, poly); // ShapeComponent.Polygon
    proto::Writer csc;
    csc.write_varint_field(2, 1);                    // IsGround
    csc.write_float_field(6, static_cast<float>(gm.min_depth)); // MinDepth
    csc.write_float_field(7, static_cast<float>(gm.max_depth)); // MaxDepth
    csc.write_varint_field(11, 1);                   // Enabled
    proto::Writer comp_collision;
    comp_collision.write_string_field(1, "CollisionShape");
    comp_collision.write_varint_field(2, 983);
    comp_collision.write_varint_field(4, 980); // ParentComponentIdentifier
    comp_collision.write_nested_field(120, shape); // Component.ShapeComponent
    comp_collision.write_nested_field(121, csc);   // Component.CollisionShapeComponent

    // ── TextureMapping x2 (984 = surface, 985 = front) ──
    proto::Writer tm_surface;
    tm_surface.write_string_field(1, gm.top_texture);
    tm_surface.write_float_field(2, 250.0f);
    tm_surface.write_nested_field(3, make_vector2(0, 0));
    proto::Writer comp_tm_surface;
    comp_tm_surface.write_string_field(1, "TextureMapping");
    comp_tm_surface.write_varint_field(2, 984);
    comp_tm_surface.write_nested_field(113, tm_surface);

    proto::Writer tm_front;
    tm_front.write_string_field(1, gm.bottom_texture);
    tm_front.write_float_field(2, 250.0f);
    tm_front.write_nested_field(3, make_vector2(0, 0));
    proto::Writer comp_tm_front;
    comp_tm_front.write_string_field(1, "TextureMapping");
    comp_tm_front.write_varint_field(2, 985);
    comp_tm_front.write_nested_field(113, tm_front);

    // ── Object ──
    proto::Writer obj;
    obj.write_string_field(1, "SceneObject");              // TemplateName
    obj.write_string_field(2, identifier);                 // Identifier
    obj.write_bytes_field(3, comp_ground_polygon.to_string()); // Component
    obj.write_bytes_field(3, comp_ground_mesh.to_string());
    obj.write_bytes_field(3, comp_generator.to_string());
    obj.write_bytes_field(3, comp_collision.to_string());
    obj.write_bytes_field(3, comp_tm_surface.to_string());
    obj.write_bytes_field(3, comp_tm_front.to_string());
    obj.write_nested_field(4, make_vector2(0, 0));         // Position
    obj.write_float_field(5, static_cast<float>(depth));   // Depth
    obj.write_float_field(6, 0.0f);                        // Rotation
    obj.write_float_field(7, 1.0f);                        // Scaling
    obj.write_nested_field(8, make_rectangle(left, bottom, right - left, top - bottom)); // LocalAabb
    obj.write_varint_field(9, 0);                          // Hidden

    // ── Scene{ Object } ──
    proto::Writer scene;
    scene.write_bytes_field(1, obj.to_string());
    return scene.to_string();
}

} // namespace boulder
