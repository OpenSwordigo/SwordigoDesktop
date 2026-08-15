// =============================================================================
// map_loader.cpp — .scmap world-map data model (see map_loader.h).
// =============================================================================
#include "tools/map_loader.h"
#include "tools/filerift.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include <queue>
#include <algorithm>
#include <set>

namespace mapedit {

// ── Scalar helpers ──────────────────────────────────────────────────────────
static std::string unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        std::string out;
        out.reserve(s.size() - 2);
        bool esc = false;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            char c = s[i];
            if (esc) { out += c; esc = false; }
            else if (c == '\\') esc = true;
            else out += c;
        }
        return out;
    }
    return s;
}

static std::string quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (char c : s) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    out += '\'';
    return out;
}

std::string mk_get_str(const MkBlock& b, const std::string& key) {
    for (const auto& f : b.fields)
        if (f.kind == MkField::Scalar && f.key == key) return unquote(f.scalar);
    return std::string();
}

int mk_get_int(const MkBlock& b, const std::string& key, int def) {
    for (const auto& f : b.fields)
        if (f.kind == MkField::Scalar && f.key == key)
            return std::atoi(f.scalar.c_str());
    return def;
}

bool mk_has(const MkBlock& b, const std::string& key) {
    for (const auto& f : b.fields)
        if (f.kind == MkField::Scalar && f.key == key) return true;
    return false;
}

void mk_set_str(MkBlock& b, const std::string& key, const std::string& value) {
    for (auto& f : b.fields) {
        if (f.kind == MkField::Scalar && f.key == key) { f.scalar = quote(value); return; }
    }
    MkField f; f.kind = MkField::Scalar; f.key = key; f.scalar = quote(value);
    b.fields.push_back(std::move(f));
}

void mk_set_int(MkBlock& b, const std::string& key, int value) {
    for (auto& f : b.fields) {
        if (f.kind == MkField::Scalar && f.key == key) {
            f.scalar = std::to_string(value); return;
        }
    }
    MkField f; f.kind = MkField::Scalar; f.key = key; f.scalar = std::to_string(value);
    b.fields.push_back(std::move(f));
}

void mk_remove(MkBlock& b, const std::string& key) {
    b.fields.erase(
        std::remove_if(b.fields.begin(), b.fields.end(),
                       [&](const MkField& f) { return f.key == key; }),
        b.fields.end());
}

MkField* mk_msg(MkBlock& b, const std::string& key) {
    for (auto& f : b.fields)
        if (f.kind == MkField::Message && f.key == key) return &f;
    return nullptr;
}

std::vector<MkField*> mk_msgs(MkBlock& b, const std::string& key) {
    std::vector<MkField*> out;
    for (auto& f : b.fields)
        if (f.kind == MkField::Message && f.key == key) out.push_back(&f);
    return out;
}

MkField* mk_add_msg(MkBlock& b, const std::string& key) {
    MkField f;
    f.kind = MkField::Message;
    f.key = key;
    b.fields.push_back(std::move(f));
    return &b.fields.back();
}

// Message variants (operate on a message field's own children list).
MkField* mk_add_msg(MkField& msg, const std::string& key) {
    MkField f;
    f.kind = MkField::Message;
    f.key = key;
    msg.children.push_back(std::move(f));
    return &msg.children.back();
}

std::vector<MkField*> mk_msgs(MkField& msg, const std::string& key) {
    std::vector<MkField*> out;
    for (auto& f : msg.children)
        if (f.kind == MkField::Message && f.key == key) out.push_back(&f);
    return out;
}

// ── Message-child scalar helpers ────────────────────────────────────────────
std::string mk_msg_get_str(const MkField& msg, const std::string& key) {
    for (const auto& c : msg.children)
        if (c.kind == MkField::Scalar && c.key == key) return unquote(c.scalar);
    return std::string();
}

int mk_msg_get_int(const MkField& msg, const std::string& key, int def) {
    for (const auto& c : msg.children)
        if (c.kind == MkField::Scalar && c.key == key) return std::atoi(c.scalar.c_str());
    return def;
}

void mk_msg_set_str(MkField& msg, const std::string& key, const std::string& value) {
    for (auto& c : msg.children) {
        if (c.kind == MkField::Scalar && c.key == key) { c.scalar = quote(value); return; }
    }
    MkField c; c.kind = MkField::Scalar; c.key = key; c.scalar = quote(value);
    msg.children.push_back(std::move(c));
}

void mk_msg_set_int(MkField& msg, const std::string& key, int value) {
    for (auto& c : msg.children) {
        if (c.kind == MkField::Scalar && c.key == key) { c.scalar = std::to_string(value); return; }
    }
    MkField c; c.kind = MkField::Scalar; c.key = key; c.scalar = std::to_string(value);
    msg.children.push_back(std::move(c));
}

void mk_msg_remove(MkField& msg, const std::string& key) {
    msg.children.erase(
        std::remove_if(msg.children.begin(), msg.children.end(),
                       [&](const MkField& c) { return c.key == key; }),
        msg.children.end());
}

// ── Markup parser ───────────────────────────────────────────────────────────
// Format produced by filerift::decode_protobuf(bytes, "scmap"):
//   Zone{                <- 4-space indent per nesting level
//       Name : 'town'
//       Node{
//           LevelName : 'town_part1'
//       }
//   }
namespace {

bool is_blank(const std::string& ln) {
    return ln.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool is_close(const std::string& ln) {
    std::string t = ln;
    t.erase(0, t.find_first_not_of(" \t"));
    return !t.empty() && t[0] == '}';
}

// Parse one block body (fields until the matching close brace).  `ln` is the
// current line (the opening "Name{" line has already been consumed).
bool parse_fields(std::vector<std::string>& lines, size_t& i,
                  std::vector<MkField>& out) {
    while (i < lines.size()) {
        std::string ln = lines[i++];
        if (is_blank(ln)) continue;
        if (is_close(ln)) return true;
        // strip leading whitespace
        size_t s = ln.find_first_not_of(" \t");
        std::string t = (s == std::string::npos) ? std::string() : ln.substr(s);
        // Message child:  "Node{"
        size_t brace = t.find('{');
        if (brace != std::string::npos) {
            MkField f;
            f.kind = MkField::Message;
            f.key = t.substr(0, brace);
            if (!parse_fields(lines, i, f.children)) return false;
            out.push_back(std::move(f));
            continue;
        }
        // Scalar: "Name : 'town'"
        size_t colon = t.find(':');
        if (colon != std::string::npos) {
            MkField f;
            f.kind = MkField::Scalar;
            std::string key = t.substr(0, colon);
            size_t ke = key.find_last_not_of(" \t");
            f.key = key.substr(0, ke + 1);
            std::string val = t.substr(colon + 1);
            size_t vs = val.find_first_not_of(" \t");
            f.scalar = (vs == std::string::npos) ? "" : val.substr(vs);
            out.push_back(std::move(f));
        }
    }
    return true;
}

} // namespace

bool map_parse_markup(const std::string& markup, MapData& out, std::string* error) {
    out.root.clear();
    std::vector<std::string> lines;
    {
        std::istringstream ss(markup);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    size_t i = 0;
    while (i < lines.size()) {
        std::string ln = lines[i++];
        if (is_blank(ln)) continue;
        if (is_close(ln)) continue;
        size_t s = ln.find_first_not_of(" \t");
        std::string t = (s == std::string::npos) ? std::string() : ln.substr(s);
        size_t brace = t.find('{');
        if (brace == std::string::npos) continue;
        MkBlock blk;
        blk.type = t.substr(0, brace);
        parse_fields(lines, i, blk.fields);
        out.root.push_back(std::move(blk));
    }
    map_rebuild(out);
    return true;
}

// ── Serializer (byte-faithful to FileRift decode output) ────────────────────
static void write_field(const MkField& f, int depth, std::string& out) {
    std::string pad(depth * 4, ' ');
    if (f.kind == MkField::Scalar) {
        out += pad + f.key + " : " + f.scalar + "\n";
        return;
    }
    out += pad + f.key + "{\n";
    for (const auto& c : f.children) write_field(c, depth + 1, out);
    out += pad + "}\n";
}

std::string map_to_markup(const MapData& in) {
    std::string out;
    for (const auto& blk : in.root) {
        out += blk.type + "{\n";
        for (const auto& f : blk.fields) write_field(f, 1, out);
        out += "}\n\n";
    }
    return out;
}

// ── Typed overlay rebuild ───────────────────────────────────────────────────
void map_rebuild(MapData& m) {
    m.zones.clear();
    m.node_index.clear();
    for (auto& blk : m.root) {
        if (blk.type != "Zone") continue;
        MapZoneData z;
        z.name = mk_get_str(blk, "Name");
        z.title = mk_get_str(blk, "Title");
        z.experience_level = mk_get_int(blk, "ExperienceLevel", 0);
        z.music = mk_get_str(blk, "Music");
        for (MkField* nf : mk_msgs(blk, "Node")) {
            MkBlock nb;
            nb.type = "Node";
            nb.fields = nf->children;
            MapNodeData n;
            n.level_name = mk_get_str(nb, "LevelName");
            n.title = mk_get_str(nb, "Title");
            n.type = mk_get_int(nb, "Type", 0);
            n.hidden = mk_get_int(nb, "Hidden", 0) != 0;
            n.experience_level = mk_get_int(nb, "ExperienceLevel", 0);
            n.music = mk_get_str(nb, "Music");
            n.has_portal = mk_get_int(nb, "HasPortal", 0) != 0;
            n.num_treasures = mk_get_int(nb, "NumTreasures", 0);
            n.ignore_in_statistics = mk_get_int(nb, "IgnoreInStatistics", 0) != 0;
            for (MkField* pf : mk_msgs(nb, "Portal")) {
                MkBlock pb;
                pb.type = "Portal";
                pb.fields = pf->children;
                MapPortal p;
                p.destination_name = mk_get_str(pb, "DestinationName");
                p.direction = mk_get_int(pb, "Direction", 0);
                p.pass_direction = mk_get_int(pb, "PassDirection", 0);
                p.ignore_in_node_positioning =
                    mk_get_int(pb, "IgnoreInNodePositioning", 0) != 0;
                n.portals.push_back(std::move(p));
            }
            if (!n.level_name.empty())
                m.node_index[n.level_name] = { (int)m.zones.size(), (int)z.nodes.size() };
            z.nodes.push_back(std::move(n));
        }
        m.zones.push_back(std::move(z));
    }
}

// ── Load / save ─────────────────────────────────────────────────────────────
bool map_load(const std::string& path, MapData& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot open file"; return false; }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string markup;
    try {
        markup = filerift::decode_protobuf(bytes, "scmap");
    } catch (const std::exception& e) {
        if (error) *error = std::string("decode failed: ") + e.what();
        return false;
    }
    if (!map_parse_markup(markup, out, error)) return false;
    out.filepath = path;
    out.dirty = false;
    return true;
}

bool map_save(const std::string& path, const MapData& in, std::string* error) {
    std::string markup = map_to_markup(in);
    std::string bytes;
    try {
        bytes = filerift::recode_markup(markup, "scmap");
    } catch (const std::exception& e) {
        if (error) *error = std::string("encode failed: ") + e.what();
        return false;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (error) *error = "cannot write file"; return false; }
    f.write(bytes.data(), (std::streamsize)bytes.size());
    if (!f) { if (error) *error = "write failed"; return false; }
    return true;
}

void map_inject_positions(MapData& m) {
    for (auto& blk : m.root) {
        if (blk.type != "Zone") continue;
        int zi = -1;
        for (size_t i = 0; i < m.zones.size(); ++i)
            if (m.zones[i].name == mk_get_str(blk, "Name")) { zi = (int)i; break; }
        if (zi < 0) continue;
        int ni = 0;
        for (MkField* nf : mk_msgs(blk, "Node")) {
            if (ni >= (int)m.zones[zi].nodes.size()) break;
            MapNodeData& nd = m.zones[zi].nodes[ni++];
            if (!nd.manual) continue;
            // Position message child lives directly in nf->children.
            MkField* pos = nullptr;
            for (auto& c : nf->children)
                if (c.kind == MkField::Message && c.key == "Position") { pos = &c; break; }
            if (!pos) {
                nf->children.push_back(MkField());
                pos = &nf->children.back();
                pos->kind = MkField::Message;
                pos->key = "Position";
            }
            // rewrite X/Y scalars inside pos->children
            bool has_x = false, has_y = false;
            for (auto& c : pos->children) {
                if (c.kind == MkField::Scalar && c.key == "X") {
                    c.scalar = std::to_string((int)nd.lx); has_x = true;
                } else if (c.kind == MkField::Scalar && c.key == "Y") {
                    c.scalar = std::to_string((int)nd.ly); has_y = true;
                }
            }
            if (!has_x) {
                MkField c; c.kind = MkField::Scalar; c.key = "X";
                c.scalar = std::to_string((int)nd.lx); pos->children.push_back(c);
            }
            if (!has_y) {
                MkField c; c.kind = MkField::Scalar; c.key = "Y";
                c.scalar = std::to_string((int)nd.ly); pos->children.push_back(c);
            }
        }
    }
}

// ── Auto-layout (RecursivelySetNodePositions port) ─────────────────────────
namespace {
constexpr float kStep = 54.0f;
constexpr float kPi = 3.14159265f;

float octant_angle(int dir) {
    if (dir < 1 || dir > 8) return -1.f;
    return (float)(dir - 1) * kPi * 0.25f; // 1=E, 2=NE, ..., 8=SE
}

int roundi(float v) { return (int)std::floor(v + 0.5f); }

void place_from(MapData& m, int zi, int ni, std::set<std::string>& placed) {
    MapZoneData& z = m.zones[zi];
    MapNodeData& n = z.nodes[ni];
    placed.insert(n.level_name);
    float bx = n.lx, by = n.ly;
    int fan = 0;
    for (const auto& p : n.portals) {
        if (p.ignore_in_node_positioning) continue;
        auto it = m.node_index.find(p.destination_name);
        if (it == m.node_index.end() || placed.count(p.destination_name)) continue;
        float ang = octant_angle(p.direction);
        if (ang < 0.f) {
            // directionless portals fan out below/behind: 135°, 45°, 225°...
            ang = kPi * (0.75f - 0.5f * (float)(fan % 2)) + (float)(fan / 2) * kPi * 0.5f;
            ++fan;
        }
        auto [dz, dn] = it->second;
        MapNodeData& d = m.zones[dz].nodes[dn];
        d.lx = bx + roundi(std::cos(ang) * kStep);
        d.ly = by + roundi(std::sin(ang) * kStep);
        d.manual = false;
        place_from(m, dz, dn, placed);
    }
}
} // namespace

int map_auto_layout(MapData& m) {
    std::set<std::string> placed;
    int laid = 0;
    for (size_t zi = 0; zi < m.zones.size(); ++zi) {
        MapZoneData& z = m.zones[zi];
        if (z.nodes.empty()) continue;
        if (!z.nodes[0].manual) {
            z.nodes[0].lx = 0.f;
            z.nodes[0].ly = (float)zi * 200.f;
        }
        size_t before = placed.size();
        place_from(m, (int)zi, 0, placed);
        laid += (int)(placed.size() - before);
        for (size_t ni = 0; ni < z.nodes.size(); ++ni)
            if (!placed.count(z.nodes[ni].level_name) && !z.nodes[ni].manual) {
                z.nodes[ni].lx = 0.f;
                z.nodes[ni].ly = (float)zi * 200.f + (float)(ni + 1) * 54.f;
                placed.insert(z.nodes[ni].level_name);
                ++laid;
            }
    }
    return laid;
}

// ── Path finding (BFS across portals) ───────────────────────────────────────
std::vector<std::string> map_find_path(const MapData& m, const std::string& from,
                                       const std::string& to) {
    std::vector<std::string> out;
    if (from.empty() || to.empty() || from == to) {
        if (!to.empty()) out.push_back(to);
        return out;
    }
    std::map<std::string, std::string> prev;
    std::queue<std::string> q;
    std::set<std::string> seen;
    q.push(from); seen.insert(from);
    bool found = false;
    while (!q.empty() && !found) {
        std::string cur = q.front(); q.pop();
        auto it = m.node_index.find(cur);
        if (it == m.node_index.end()) continue;
        auto [zi, ni] = it->second;
        for (const auto& p : m.zones[zi].nodes[ni].portals) {
            if (seen.count(p.destination_name)) continue;
            seen.insert(p.destination_name);
            prev[p.destination_name] = cur;
            if (p.destination_name == to) { found = true; break; }
            q.push(p.destination_name);
        }
    }
    if (!found) return out;
    std::string cur = to;
    while (!cur.empty()) {
        out.push_back(cur);
        auto it = prev.find(cur);
        cur = (it != prev.end()) ? it->second : std::string();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ── Validation ──────────────────────────────────────────────────────────────
void map_validate(MapData& m) {
    m.issues.clear();
    std::set<std::string> names;
    for (const auto& z : m.zones)
        for (const auto& n : z.nodes)
            if (!names.insert(n.level_name).second)
                m.issues.push_back("Duplicate node name: '" + n.level_name + "'");
    for (const auto& z : m.zones)
        for (const auto& n : z.nodes) {
            if (n.title.empty() && (n.type == 1 || n.type == 3))
                m.issues.push_back("Node '" + n.level_name + "' has no title (town/boss)");
            for (const auto& p : n.portals)
                if (!names.count(p.destination_name))
                    m.issues.push_back("Node '" + n.level_name + "' portal -> missing level '" +
                                       p.destination_name + "'");
        }
}

// ── Zone background convention ──────────────────────────────────────────────
std::string zone_background_name(const std::string& zone_name) {
    std::string z = zone_name;
    std::transform(z.begin(), z.end(), z.begin(), ::tolower);
    // Village / town zones → bg0 (the pastoral village art)
    if (z == "town" || z == "cairnwood" || z == "village" || z == "florennum" ||
        z == "city" || z == "capital")
        return "bg0.png";
    // Woodland zones → forest_bg
    if (z == "woods" || z == "forest" || z == "cairnwoodforest")
        return "forest_bg.png";
    // Open plains / fields → grasslands_bg
    if (z == "plains" || z == "grasslands" || z == "grass" || z == "fields" ||
        z == "outskirts")
        return "grasslands_bg.png";
    // The keep / castle ruins → pic0
    if (z == "woodkeep" || z == "keep" || z == "castle")
        return "pic0.png";
    // The Evernight grove → grove_bg
    if (z == "grove" || z == "evernight" || z == "evernightforest")
        return "grove_bg.png";
    // Caves / ice / dungeons → caves_bg
    if (z.find("cave") != std::string::npos || z.find("ice") != std::string::npos ||
        z.find("dungeon") != std::string::npos || z.find("mines") != std::string::npos)
        return "caves_bg.png";
    // Graveyard / endgame zones → graveyard_bg
    if (z.find("grave") != std::string::npos || z.find("worldsend") != std::string::npos ||
        z.find("lowergrove") != std::string::npos)
        return "graveyard_bg.png";
    return "pic0.png";
}

} // namespace mapedit
