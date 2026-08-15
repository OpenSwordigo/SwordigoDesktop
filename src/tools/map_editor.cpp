// =============================================================================
// map_editor.cpp — Ruby world-map editor (see map_editor.h).
// =============================================================================
#include "tools/map_editor.h"

#include "platform/embedded_assets.h"   // embedded_asset / asset_decode_image (libswcore)
#include "platform/gl_inc.h"
#include "platform/IconsFontAwesome6.h"
#include "platform/data_path.h"         // get_vfs_save_dir()
#include "tools/filerift.h"             // decode_protobuf (PlayerProfile schema)#include "imgui.h"
#include "imgui_internal.h"   // ImRect (label collision avoidance)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <filesystem>

namespace fs = std::filesystem;

namespace mapedit {

// Forward declarations for helpers defined later in this file but used by
// map_editor_open(), hit_test_node() and the toolbar (graph-mode support).
static void node_xy(const MapEditorState& st, const MapNodeData& n, float& x, float& y);
static void node_pos(const MapEditorState& st, const MapNodeData& n, float& x, float& y);
static void rebuild_graph_layout(MapEditorState& st);
// Forward declaration: the "View / Options" popup that now hosts the display
// toggles that used to crowd the toolbar row (see draw_toolbar).
static void draw_view_options_popup(MapEditorState& st);

// ── Embedded texture cache ──────────────────────────────────────────────────
struct TexInfo { GLuint tex = 0; int w = 0, h = 0; };
static std::map<std::string, TexInfo> g_tex_cache;

// stb_image decodes rows top-down; GL textures are bottom-up, so flip the
// rows so ImGui renders the sprite upright (same convention as load_texture_file).
static void flip_pixels_vertical(unsigned char* px, int w, int h) {
    const int stride = w * 4;
    std::vector<unsigned char> row((size_t)stride);
    for (int y = 0; y < h / 2; ++y) {
        unsigned char* a = px + (size_t)y * stride;
        unsigned char* b = px + (size_t)(h - 1 - y) * stride;
        std::memcpy(row.data(), a, (size_t)stride);
        std::memcpy(a, b, (size_t)stride);
        std::memcpy(b, row.data(), (size_t)stride);
    }
}

static GLuint load_embedded_texture(const std::string& name,
                                    int* out_w = nullptr, int* out_h = nullptr) {
    auto it = g_tex_cache.find(name);
    if (it != g_tex_cache.end()) {
        if (out_w) *out_w = it->second.w;
        if (out_h) *out_h = it->second.h;
        return it->second.tex;
    }
    const unsigned char* data = nullptr;
    size_t size = 0;
    if (!embedded_asset(name.c_str(), &data, &size) || !data || size == 0) {
        g_tex_cache[name] = TexInfo{};
        return 0;
    }
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    if (!asset_decode_image(data, size, &px, &w, &h) || !px || w <= 0 || h <= 0) {
        g_tex_cache[name] = TexInfo{};
        return 0;
    }
    if (getenv("RUBY_MAP_DEBUG"))
        fprintf(stderr, "[MapDebug] texture '%s' %dx%d\n", name.c_str(), w, h);
    flip_pixels_vertical(px, w, h);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    asset_image_free(px);
    g_tex_cache[name] = { tex, w, h };
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

// ── Layout persistence (sidecar .swmap, editor-only) ────────────────────────
// Simple line format:
//   node <level_name> <x> <y> [manual]
//   bg   <zone_name> <asset>
namespace {
const char* kSwmapHeader = "# Swordigo map editor layout v1";
}

static std::string sidecar_for(const std::string& map_path) {
    if (map_path.empty()) return std::string();
    return map_path + ".swmap";
}

bool map_editor_save_sidecar(MapEditorState& st, std::string* error) {
    if (st.sidecar_path.empty()) return true;
    std::ofstream f(st.sidecar_path, std::ios::trunc);
    if (!f) { if (error) *error = "cannot write sidecar"; return false; }
    f << kSwmapHeader << "\n";
    for (const auto& z : st.map.zones)
        for (const auto& n : z.nodes) {
            auto it = st.layout.find(n.level_name);
            if (it != st.layout.end() && it->second.manual)
                f << "node " << n.level_name << " " << it->second.lx << " "
                  << it->second.ly << " 1\n";
        }
    for (const auto& [zone, bg] : st.zone_bg_override)
        f << "bg " << zone << " " << bg << "\n";
    if (!st.current_node.empty())
        f << "hero " << st.current_node << "\n";
    f << "zoom " << st.zoom << "\n";
    f << "cam " << st.cam_x << " " << st.cam_y << "\n";
    return true;
}

static void map_editor_load_sidecar(MapEditorState& st) {
    st.sidecar_path = sidecar_for(st.map.filepath);
    st.layout.clear();
    st.zone_bg_override.clear();
    std::ifstream f(st.sidecar_path);
    if (!f) return;
    std::string ln;
    while (std::getline(f, ln)) {
        std::istringstream ss(ln);
        std::string tag;
        ss >> tag;
        if (tag == "node") {
            std::string name;
            float x, y;
            int man = 0;
            if (ss >> name >> x >> y >> man) {
                LayoutEntry e;
                e.lx = x; e.ly = y; e.manual = (man != 0);
                st.layout[name] = e;
            }
        } else if (tag == "bg") {
            std::string zone, bg;
            if (ss >> zone >> bg) st.zone_bg_override[zone] = bg;
        } else if (tag == "hero") {
            std::string level;
            if (ss >> level) st.current_node = level;
        } else if (tag == "zoom") {
            float z;
            if (ss >> z && z > 0.01f && z < 1000.f) st.zoom = z;
        } else if (tag == "cam") {
            float x, y;
            if (ss >> x >> y) { st.cam_x = x; st.cam_y = y; }
        }
    }
}

// ── Save-file integration ───────────────────────────────────────────────────
// Reads a Swordigo .gplayer save (via filerift's PlayerProfile schema) and
// extracts the current map node + player level — the two facts the editor
// needs to place the hero marker and drive XP gates.
static bool save_extract(const std::string& path, std::string* map_node,
                         int* level, std::string* name, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open save"; return false; }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string markup;
    try {
        markup = filerift::decode_protobuf(bytes, "gplayer");
    } catch (const std::exception& e) {
        if (err) *err = std::string("decode failed: ") + e.what();
        return false;
    }
    if (markup.empty()) { if (err) *err = "empty decode"; return false; }
    // PlayerProfile{  Name : 'x'  ExperienceLevel : 5  GameState{ ... CurrentMapNodeName : 'level' ... } }
    auto grab_str = [&](const std::string& key) -> std::string {
        std::string pat = key + " : ";
        size_t p = markup.find(pat);
        if (p == std::string::npos) return std::string();
        p += pat.size();
        size_t q = markup.find('\n', p);
        std::string v = markup.substr(p, (q == std::string::npos ? markup.size() : q) - p);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\'' || v.front() == '\r'))
            v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\'' || v.back() == '\r'))
            v.pop_back();
        return v;
    };
    auto grab_int = [&](const std::string& key) -> int {
        std::string v = grab_str(key);
        if (v.empty()) return 0;
        try { return std::stoi(v); } catch (...) { return 0; }
    };
    if (map_node) *map_node = grab_str("CurrentMapNodeName");
    if (level) *level = grab_int("ExperienceLevel");
    if (name) *name = grab_str("Name");
    return true;
}

// Find the most-recently-modified .gplayer in the VFS save Documents dir.
static std::string latest_save_path() {
    std::string dir = get_vfs_save_dir();
    std::string best;
    fs::file_time_type best_t{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (e.path().extension() != ".gplayer") continue;
        auto t = e.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (best.empty() || t > best_t) { best = e.path().string(); best_t = t; }
    }
    return best;
}

// Load hero marker + player level from the most recent save file.
bool map_editor_load_from_save(MapEditorState& st, std::string* err) {
    std::string path = latest_save_path();
    if (path.empty()) { if (err) *err = "no .gplayer save found in " + get_vfs_save_dir(); return false; }
    std::string node, name;
    int level = 0;
    if (!save_extract(path, &node, &level, &name, err)) return false;
    if (node.empty() && level <= 0) { if (err) *err = "save has no map node or level"; return false; }
    // Only accept nodes that actually exist on this map.
    if (!node.empty() && st.map.node_index.count(node))
        st.current_node = node;
    if (level > 0) {
        st.player_level = level;
        st.save_level = level;
    }
    st.save_file = path;
    return true;
}

// ── Open / save / reload ────────────────────────────────────────────────────
bool map_editor_open(MapEditorState& st, const std::string& path, std::string* error) {
    if (!map_load(path, st.map, error)) {
        st.loaded = false;
        return false;
    }
    map_validate(st.map);
    st.loaded = true;
    st.dirty = false;
    st.error.clear();
    st.sel_zone = st.sel_node = st.sel_portal = -1;
    st.path_target_zone = st.path_target_node = -1;
    st.portal_src_zone = st.portal_src_node = -1;
    st.tool = MapTool::Select;
    st.dragging = false;
    map_editor_load_sidecar(st);
    map_editor_fit_view(st);
    // If the sidecar had no manual layout, run auto-layout first so the
    // graph is immediately readable.
    bool any_manual = false;
    for (const auto& [k, e] : st.layout)
        if (e.manual) any_manual = true;
    if (!any_manual) {
        st.layout.clear();
        map_auto_layout(st.map);
        for (auto& z : st.map.zones)
            for (auto& n : z.nodes) st.layout[n.level_name] = { n.lx, n.ly, false };
    } else {
        // apply manual layout onto the typed overlay
        for (auto& z : st.map.zones)
            for (auto& n : z.nodes) {
                auto it = st.layout.find(n.level_name);
                if (it != st.layout.end()) {
                    n.lx = it->second.lx;
                    n.ly = it->second.ly;
                    n.manual = it->second.manual;
                }
            }
    }
    rebuild_graph_layout(st);
    // Default view: zoomed INTO the world, not fitting all 110 nodes.
    // Center on the hero/current node when known, else the first town.
    if (st.current_node.empty() && !st.map.zones.empty()) {
        for (const auto& z : st.map.zones)
            for (const auto& n : z.nodes)
                if (n.type == 1 && st.current_node.empty()) st.current_node = n.level_name;
        if (st.current_node.empty() && !st.map.zones.empty() && !st.map.zones[0].nodes.empty())
            st.current_node = st.map.zones[0].nodes[0].level_name;
    }
    auto itn = st.map.node_index.find(st.current_node);
    if (itn != st.map.node_index.end()) {
        const MapNodeData& nn = st.map.zones[itn->second.first].nodes[itn->second.second];
        float nx = 0.f, ny = 0.f;
        node_xy(st, nn, nx, ny);
        st.cam_x = nx;
        st.cam_y = ny;
        st.zoom = std::clamp(1.35f, st.zoom_min, st.zoom_max);
    }
    return true;
}

bool map_editor_reload(MapEditorState& st, std::string* error) {
    if (st.map.filepath.empty()) { if (error) *error = "no map loaded"; return false; }
    return map_editor_open(st, st.map.filepath, error);
}

bool map_editor_save(MapEditorState& st, std::string* error) {
    if (st.map.filepath.empty()) { if (error) *error = "no map loaded"; return false; }
    // sync typed overlay (manual layout) → generic tree when writing positions
    if (st.write_positions) {
        for (auto& z : st.map.zones)
            for (auto& n : z.nodes) {
                auto it = st.layout.find(n.level_name);
                if (it != st.layout.end()) {
                    n.lx = it->second.lx;
                    n.ly = it->second.ly;
                    n.manual = it->second.manual;
                }
            }
        map_inject_positions(st.map);
    }
    if (!map_save(st.map.filepath, st.map, error)) return false;
    map_editor_save_sidecar(st, nullptr);
    st.dirty = false;
    return true;
}

// ── Camera ──────────────────────────────────────────────────────────────────
void map_editor_fit_view(MapEditorState& st) {
    if (st.map.zones.empty()) { st.cam_x = st.cam_y = 0.f; st.zoom = 1.f; return; }
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    bool any = false;
    for (const auto& z : st.map.zones)
        for (const auto& n : z.nodes) {
            float lx, ly;
            if (st.graph_mode) {
                auto gi = st.graph_layout.find(n.level_name);
                lx = (gi != st.graph_layout.end()) ? gi->second.first : n.lx;
                ly = (gi != st.graph_layout.end()) ? gi->second.second : n.ly;
            } else {
                auto it = st.layout.find(n.level_name);
                lx = (it != st.layout.end()) ? it->second.lx : n.lx;
                ly = (it != st.layout.end()) ? it->second.ly : n.ly;
            }
            minx = std::min(minx, lx); maxx = std::max(maxx, lx);
            miny = std::min(miny, ly); maxy = std::max(maxy, ly);
            any = true;
        }
    if (!any) return;
    st.cam_x = (minx + maxx) * 0.5f;
    st.cam_y = (miny + maxy) * 0.5f;
    float bw = std::max(400.f, maxx - minx);
    float bh = std::max(400.f, maxy - miny);
    // Fit is an *overview* utility: it should not be the default viewing
    // scale. Compute a fit zoom but cap it so it never shrinks nodes to dust.
    st.zoom = std::clamp(520.f / std::max(bw, bh), st.zoom_min, 2.6f);
}

// ── World → screen ──────────────────────────────────────────────────────────
namespace {
struct Canvas {
    ImVec2 origin;   // canvas top-left in screen space
    ImVec2 size;
};

inline ImVec2 world_to_screen(const MapEditorState& st, const Canvas& c, float wx, float wy) {
    return ImVec2(c.origin.x + c.size.x * 0.5f + (wx - st.cam_x) * st.zoom,
                  c.origin.y + c.size.y * 0.5f + (wy - st.cam_y) * st.zoom);
}

inline ImVec2 screen_to_world(const MapEditorState& st, const Canvas& c, ImVec2 s) {
    return ImVec2(st.cam_x + (s.x - (c.origin.x + c.size.x * 0.5f)) / st.zoom,
                  st.cam_y + (s.y - (c.origin.y + c.size.y * 0.5f)) / st.zoom);
}

// The same sprites the game's Caver::MapView loads for the world map
// (from the decompiled constructor): ui_map_node for plain nodes,
// ui_map_icon_town for towns, ui_map_icon_boss for boss chambers.
const char* node_sprite_name(int type) {
    switch (type) {
        case 1:  return "icons/game/ui_map_icon_town.png";
        case 3:  return "icons/game/ui_map_icon_boss.png";
        default: return "icons/game/ui_map_node.png";
    }
}

// Overlay badges (drawn above the node sprite), same set as the game.
const char* locked_badge_name()   { return "icons/game/ui_map_icon_locked.png"; }
const char* treasure_badge_name() { return "icons/game/ui_map_icon_treasure.png"; }
const char* portal_badge_name()   { return "icons/game/ui_map_icon_portal.png"; }
const char* hero_marker_name()    { return "icons/game/ui_map_marker_hero.png"; }
const char* selected_marker_name(){ return "icons/game/ui_map_marker_selected.png"; }
const char* guide_edge_name()     { return "icons/game/ui_map_guide_edge.png"; }
const char* path_dot_name()       { return "icons/game/ui_map_path.png"; }

ImU32 node_label_color(int type) {
    switch (type) {
        case 1:  return IM_COL32(255, 214, 102, 255);
        case 2:  return IM_COL32(126, 217, 255, 255);
        case 3:  return IM_COL32(255, 96, 96, 255);
        default: return IM_COL32(224, 224, 224, 255);
    }
}

const char* type_name(int t) {
    switch (t) {
        case 0: return "Plain";
        case 1: return "Town";
        case 2: return "Waypoint";
        case 3: return "Boss";
        default: return "?";
    }
}

// dir 1=E, 2=NE, ... 8=SE (compass octant) → soft colour
ImU32 dir_color(int dir) {
    static const ImU32 kC[9] = {
        IM_COL32(140, 150, 170, 200),
        IM_COL32(110, 190, 255, 210), IM_COL32(150, 220, 255, 210),
        IM_COL32(120, 255, 180, 210), IM_COL32(255, 235, 130, 210),
        IM_COL32(255, 170, 110, 210), IM_COL32(255, 130, 170, 210),
        IM_COL32(190, 140, 255, 210), IM_COL32(130, 160, 255, 210),
    };
    return (dir >= 1 && dir <= 8) ? kC[dir] : kC[0];
}

// ── Layout helpers ──────────────────────────────────────────────────────────
// Get layout entry (sidecar is authoritative), else fall back to overlay.
const LayoutEntry* layout_of(const MapEditorState& st, const std::string& level) {
    auto it = st.layout.find(level);
    return (it != st.layout.end()) ? &it->second : nullptr;
}

void set_layout(MapEditorState& st, const std::string& level, float x, float y, bool manual) {
    auto it = st.layout.find(level);
    if (it == st.layout.end()) {
        st.layout[level] = { x, y, manual };
    } else {
        it->second.lx = x; it->second.ly = y; it->second.manual = manual;
    }
    st.dirty = true;
}

} // namespace

// ── Node editing (tree mutations) ───────────────────────────────────────────
static MkBlock* zone_block(MapData& m, int zi) {
    if (zi < 0 || zi >= (int)m.root.size()) return nullptr;
    if (m.root[zi].type != "Zone") return nullptr;
    return &m.root[zi];
}

static MkField* node_field(MkBlock* zb, int ni) {
    if (!zb) return nullptr;
    auto nodes = mk_msgs(*zb, "Node");
    if (ni < 0 || ni >= (int)nodes.size()) return nullptr;
    return nodes[ni];
}

static void mark_dirty(MapEditorState& st) {
    st.dirty = true;
    map_rebuild(st.map);      // refresh typed overlay from tree
    map_validate(st.map);
}


// ── View / Options popup ─────────────────────────────────────────────────────
// All the *display* toggles that used to crowd the toolbar row now live here,
// behind one "View / Options" button, so the toolbar reads clean while every
// feature stays reachable. Opened via ImGui::OpenPopup from draw_toolbar.
static void draw_view_options_popup(MapEditorState& st) {
    if (!ImGui::BeginPopup("MapViewOptions")) return;

    // ── View mode: Map (geographic world) vs Graph (debug auto-layout) ──────
    // — Auto Layout must never clobber the game's geography.
    ImGui::TextDisabled("View mode");
    bool graph_on = st.graph_mode;
    if (!graph_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.40f, 0.60f, 1.f));
    if (ImGui::Button(ICON_FA_MAP " Map")) {
        if (st.graph_mode) { st.graph_mode = false; st.status = "Map mode — geographic world positions"; }
    }
    if (!graph_on) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (graph_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.40f, 0.60f, 1.f));
    if (ImGui::Button(ICON_FA_OBJECT_GROUP " Graph")) {
        if (!st.graph_mode) { st.graph_mode = true; rebuild_graph_layout(st); st.status = "Graph mode — debug auto-layout"; }
    }
    if (graph_on) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Auto Layout")) {
        // Auto-layout is a *debug* utility: it switches to Graph mode and
        // only touches the graph-only layout, never the game's geography.
        st.graph_mode = true;
        rebuild_graph_layout(st);
        st.status = "Graph mode: auto-layout (vanilla recursion port)";
    }

    ImGui::Separator();
    ImGui::TextDisabled("Display");
    ImGui::Checkbox("Zone Background", &st.show_zone_bg);
    ImGui::Checkbox("Grid", &st.show_grid);
    ImGui::Checkbox("Labels", &st.show_labels);
    ImGui::Checkbox("Portals", &st.show_portals);
    ImGui::Checkbox("Zone Plates", &st.show_title_plates);
    ImGui::Checkbox("Node Overlays", &st.show_positions);
    ImGui::Checkbox("Validation Panel", &st.show_validation);

    ImGui::Separator();
    ImGui::TextDisabled("File");
    ImGui::Checkbox("Write Positions to .scmap", &st.write_positions);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write manual node positions (Position{X,Y}) into the .scmap.\n"
                          "Vanilla computes positions itself; keep off for byte-exact vanilla files.");

    ImGui::Separator();
    ImGui::TextDisabled("Player / Hero");
    ImGui::SetNextItemWidth(120.f);
    ImGui::DragInt("Level", &st.player_level, 0.25f, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Player level — nodes requiring a higher experience\n"
                          "level show the locked badge, matching the in-game XP gate");
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Load Level From Save")) {
        std::string err;
        if (map_editor_load_from_save(st, &err))
            st.status = "Loaded hero from save: " + st.save_file.substr(st.save_file.find_last_of("/\\") + 1)
                        + "  (level " + std::to_string(st.player_level) + ")";
        else
            st.status = "From Save failed: " + err;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Read the most recent .gplayer save\n"
                          "to auto-place the hero marker and set player level");

    // Hero (current location) marker picker
    std::string hero_preview = st.current_node.empty()
        ? std::string("(none)")
        : st.current_node;
    ImGui::SetNextItemWidth(200.f);
    if (ImGui::BeginCombo("Hero Node", hero_preview.c_str())) {
        if (ImGui::Selectable("(none)", st.current_node.empty())) {
            st.current_node.clear();
            st.status = "Hero marker removed";
        }
        for (const auto& [level, idx] : st.map.node_index) {
            std::string label = level;
            const MapNodeData& n = st.map.zones[idx.first].nodes[idx.second];
            if (!n.title.empty()) label += " — " + n.title;
            if (ImGui::Selectable(label.c_str(), st.current_node == level)) {
                st.current_node = level;
                st.status = "Hero marker at '" + level + "'";
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where the hero marker sits on the map\n"
                          "(mirrors GameState.CurrentMapNodeName; persisted to sidecar)");

    ImGui::EndPopup();
}

// ── Toolbar ─────────────────────────────────────────────────────────────────
// Compact: essential ACTIONS inline (Reload, Save, the 4 tools, Fit); all the
// many display TOGGLES moved into the "View / Options" popup above.
static void draw_toolbar(MapEditorState& st) {
    if (ImGui::Button(ICON_FA_CLOCK_ROTATE_LEFT " Reload") && st.loaded) {
        if (st.dirty) {
            st.confirm_reload = true;
        } else {
            std::string err;
            if (!map_editor_reload(st, &err)) st.status = "Reload failed: " + err;
            else st.status = "Reloaded from disk";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save") && st.loaded) {
        std::string err;
        if (!map_editor_save(st, &err)) st.status = "Save failed: " + err;
        else st.status = "Saved (byte-exact)";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Tools (the four core interaction modes)
    const char* tool_names[] = {
        ICON_FA_HAND_POINTER " Select",
        ICON_FA_PLUS " Node",
        ICON_FA_LINK " Portal",
        ICON_FA_ARROWS_ROTATE " Pan",
    };
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        bool active = ((int)st.tool == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.8f, 1.f));
        if (ImGui::Button(tool_names[i])) st.tool = (MapTool)i;
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LOCATION_DOT " Fit")) map_editor_fit_view(st);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Everything else (view mode, display toggles, player level, hero) lives
    // in this single tidy popup so the toolbar stays uncluttered.
    if (ImGui::Button(ICON_FA_SLIDERS " View / Options"))
        ImGui::OpenPopup("MapViewOptions");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("View mode, display toggles, player level & hero marker");
    draw_view_options_popup(st);

    // Status
    if (!st.status.empty()) {
        ImGui::SameLine(0.f, 16.f);
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.f, 1.f), "%s", st.status.c_str());
        if (st.status_timer > 0.f) {
            st.status_timer -= ImGui::GetIO().DeltaTime;
            if (st.status_timer <= 0.f) st.status.clear();
        } else {
            st.status_timer = 4.f;
        }
    }
    if (st.dirty) {
        ImGui::SameLine(0.f, 16.f);
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), ICON_FA_CIRCLE_EXCLAMATION " unsaved");
    }
    ImGui::Separator();
}

// Draw a sprite with rotation around its centre (for guide-edge arrows).
static void draw_image_rotated(ImDrawList* dl, ImTextureID tex, ImVec2 center,
                               float half_w, float half_h, float angle_deg, ImU32 col) {
    float a = angle_deg * 3.14159265f / 180.f;
    float ca = std::cos(a), sa = std::sin(a);
    ImVec2 p0(center.x - half_w, center.y - half_h);
    ImVec2 p1(center.x + half_w, center.y - half_h);
    ImVec2 p2(center.x + half_w, center.y + half_h);
    ImVec2 p3(center.x - half_w, center.y + half_h);
    auto rot = [&](ImVec2 p) {
        float dx = p.x - center.x, dy = p.y - center.y;
        return ImVec2(center.x + dx * ca - dy * sa, center.y + dx * sa + dy * ca);
    };
    dl->AddImageQuad(tex, rot(p0), rot(p1), rot(p2), rot(p3),
                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), col);
}

// ── Canvas ──────────────────────────────────────────────────────────────────
static int hit_test_node(const MapEditorState& st, const Canvas& c, ImVec2 mouse) {
    float best_d = 1e9f;
    int best = -1;
    int idx = 0;
    for (const auto& z : st.map.zones)
        for (const auto& n : z.nodes) {
            float lx = 0.f, ly = 0.f;
            node_pos(st, n, lx, ly);
            ImVec2 p = world_to_screen(st, c, lx, ly);
            float d = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
            float r = std::max(10.f, 18.f * st.zoom);
            if (d < r * r && d < best_d) { best_d = d; best = idx; }
            ++idx;
        }
    return best;
}

static void node_xy(const MapEditorState& st, const MapNodeData& n, float& x, float& y) {
    const LayoutEntry* e = layout_of(st, n.level_name);
    x = e ? e->lx : n.lx;
    y = e ? e->ly : n.ly;
}

// Node position honoring the current view mode: graph mode uses a fresh
// auto-layout (debug), map mode uses the geographic/persisted layout.
static void node_pos(const MapEditorState& st, const MapNodeData& n, float& x, float& y) {
    if (st.graph_mode) {
        auto it = st.graph_layout.find(n.level_name);
        if (it != st.graph_layout.end()) { x = it->second.first; y = it->second.second; return; }
    }
    node_xy(st, n, x, y);
}

// (Re)compute the graph-mode auto layout into graph_layout without touching
// the geographic layout in st.layout.
static void rebuild_graph_layout(MapEditorState& st) {
    st.graph_layout.clear();
    MapData tmp = st.map;                 // copy so auto-layout doesn't clobber positions
    map_auto_layout(tmp);
    for (const auto& z : tmp.zones)
        for (const auto& n : z.nodes)
            st.graph_layout[n.level_name] = { n.lx, n.ly };
}

static void draw_canvas(MapEditorState& st) {
    Canvas c;
    c.origin = ImGui::GetCursorScreenPos();
    c.size = ImGui::GetContentRegionAvail();
    if (c.size.x < 10.f || c.size.y < 10.f) return;

    ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsWindowHovered();
    st.hovered = hovered;

    // ── Pan (MMB or Pan tool) ───────────────────────────────────────────────
    if (hovered &&
        ((io.MouseDown[1]) || (st.tool == MapTool::Pan && io.MouseDown[0]))) {
        st.cam_x -= io.MouseDelta.x / st.zoom;
        st.cam_y -= io.MouseDelta.y / st.zoom;
    }

    // ── Zoom (wheel anchored at cursor, keyboard +/-/0) ────────────────────
    auto apply_zoom = [&](float factor) {
        ImVec2 before = screen_to_world(st, c, io.MousePos);
        st.zoom = std::clamp(st.zoom * factor, st.zoom_min, st.zoom_max);
        ImVec2 after = screen_to_world(st, c, io.MousePos);
        st.cam_x += before.x - after.x;
        st.cam_y += before.y - after.y;
    };
    if (hovered && io.MouseWheel != 0.f) apply_zoom(std::exp(io.MouseWheel * 0.12f));
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
        apply_zoom(1.25f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
        apply_zoom(0.8f);
    if (ImGui::IsKeyPressed(ImGuiKey_KeypadMultiply))
        map_editor_fit_view(st);

    // Zoom-slider overlay geometry (Swordigo-style, top-right). Computed
    // early so pointer input over it is excluded from canvas picking.
    const ImVec2 zo_origin(c.origin.x + c.size.x - 40.f, c.origin.y + 14.f);
    const float zo_w = 26.f, zo_h = 150.f;
    // Hover rect must cover the whole column: +, slider, − (ends at +202),
    // and the % readout — extend past the − button so clicks there never
    // leak through to canvas picking.
    bool over_zoom_ui = ImGui::IsMouseHoveringRect(
        zo_origin, ImVec2(zo_origin.x + zo_w + 10.f, zo_origin.y + zo_h + 58.f));

    // ── Dark-parchment canvas ──────────────────────────────────────────────
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(c.origin, ImVec2(c.origin.x + c.size.x, c.origin.y + c.size.y),
                      IM_COL32(12, 15, 24, 255));
    // Subtle gradient — mimics the game's map-screen blue-grey parchment
    dl->AddRectFilledMultiColor(c.origin, ImVec2(c.origin.x + c.size.x, c.origin.y + c.size.y * 0.2f),
        IM_COL32(20, 26, 42, 80), IM_COL32(20, 26, 42, 80),
        IM_COL32(20, 26, 42, 0), IM_COL32(20, 26, 42, 0));

    // ── Zone background art ─────────────────────────────────────────────────
    if (st.show_zone_bg && !st.map.zones.empty()) {
        for (size_t zi = 0; zi < st.map.zones.size(); ++zi) {
            const MapZoneData& z = st.map.zones[zi];
            if (z.nodes.empty()) continue;
            std::string bg = "pic0.png";
            auto ov = st.zone_bg_override.find(z.name);
            if (ov != st.zone_bg_override.end()) bg = ov->second;
            else bg = zone_background_name(z.name);
            int tw0 = 0, th0 = 0;
            GLuint tex = load_embedded_texture(bg, &tw0, &th0);
            if (!tex) continue;
            // zone bbox in world space
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            for (const auto& n : z.nodes) {
                const LayoutEntry* e = layout_of(st, n.level_name);
                float lx = e ? e->lx : n.lx;
                float ly = e ? e->ly : n.ly;
                minx = std::min(minx, lx); maxx = std::max(maxx, lx);
                miny = std::min(miny, ly); maxy = std::max(maxy, ly);
            }
            float bw = std::max(60.f, (maxx - minx) + 160.f);
            float bh = std::max(60.f, (maxy - miny) + 160.f);
            // contain-fit the texture into the bbox preserving aspect ratio
            int tw = 0, th = 0;
            tex = load_embedded_texture(bg, &tw, &th);
            if (!tex || tw <= 0 || th <= 0) continue;
            float ar = (float)tw / (float)th;
            if (bw / bh > ar) bw = bh * ar; else bh = bw / ar;
            ImVec2 p0 = world_to_screen(st, c, (minx + maxx) * 0.5f - bw * 0.5f,
                                        (miny + maxy) * 0.5f - bh * 0.5f);
            ImVec2 p1 = world_to_screen(st, c, (minx + maxx) * 0.5f + bw * 0.5f,
                                        (miny + maxy) * 0.5f + bh * 0.5f);
            // clamp to canvas
            ImVec2 p0c = ImVec2(std::max(c.origin.x, p0.x), std::max(c.origin.y, p0.y));
            ImVec2 p1c = ImVec2(std::min(c.origin.x + c.size.x, p1.x),
                                std::min(c.origin.y + c.size.y, p1.y));
            if (p1c.x <= p0c.x || p1c.y <= p0c.y) continue;
            float alpha = ((int)zi == st.sel_zone) ? 0.62f : 0.42f;
            if (st.sel_zone < 0) alpha = 0.50f;
            dl->AddImage((ImTextureID)(intptr_t)tex, p0c, p1c, ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, (int)(255 * alpha)));
            // soft vignette so nodes stay readable over bright art
            dl->AddRectFilledMultiColor(p0c, p1c,
                IM_COL32(8, 9, 12, 90), IM_COL32(8, 9, 12, 90),
                IM_COL32(8, 9, 12, 90), IM_COL32(8, 9, 12, 90));
            // zone name plate — parchment-style pill
            if (st.show_title_plates && st.show_labels && p1c.x > p0c.x) {
                std::string ztitle = z.title.empty() ? z.name : z.title;
                ImVec2 pad(12.f, 6.f);
                ImVec2 ts = ImGui::CalcTextSize(ztitle.c_str());
                ImVec2 pill_p0(p0c.x + pad.x, p0c.y + pad.y);
                ImVec2 pill_p1(p0c.x + pad.x + ts.x + pad.x,
                               p0c.y + pad.y + ts.y + pad.y);
                dl->AddRectFilled(pill_p0, pill_p1, IM_COL32(0, 0, 0, 140), 6.f);
                dl->AddRect(pill_p0, pill_p1, IM_COL32(100, 120, 160, 160), 6.f);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            ImVec2(pill_p0.x + pad.x, pill_p0.y + pad.y),
                            IM_COL32(255, 214, 102, 220), ztitle.c_str());
            }
        }
    }

    // ── Grid ────────────────────────────────────────────────────────────────
    if (st.show_grid) {
        const float step = 54.f * st.zoom; // the vanilla layout step
        if (step > 6.f) {
            ImU32 minor = IM_COL32(40, 44, 56, 140);
            ImU32 major = IM_COL32(60, 66, 84, 150);
            float wx0 = screen_to_world(st, c, ImVec2(c.origin.x, 0)).x;
            float wx1 = screen_to_world(st, c, ImVec2(c.origin.x + c.size.x, 0)).x;
            float wy0 = screen_to_world(st, c, ImVec2(0, c.origin.y)).y;
            float wy1 = screen_to_world(st, c, ImVec2(0, c.origin.y + c.size.y)).y;
            for (int gx = (int)std::floor(wx0 / 54.f); gx <= (int)std::ceil(wx1 / 54.f); ++gx) {
                float wx = gx * 54.f;
                ImVec2 p = world_to_screen(st, c, wx, 0);
                bool maj = (gx % 5 == 0);
                dl->AddLine(ImVec2(p.x, c.origin.y), ImVec2(p.x, c.origin.y + c.size.y),
                            maj ? major : minor);
            }
            for (int gy = (int)std::floor(wy0 / 54.f); gy <= (int)std::ceil(wy1 / 54.f); ++gy) {
                float wy = gy * 54.f;
                ImVec2 p = world_to_screen(st, c, 0, wy);
                bool maj = (gy % 5 == 0);
                dl->AddLine(ImVec2(c.origin.x, p.y), ImVec2(c.origin.x + c.size.x, p.y),
                            maj ? major : minor);
            }
        }
    }

    // ── Portals (edges) ─────────────────────────────────────────────────────
    std::vector<std::string> path; // BFS highlight
    if (st.sel_zone >= 0 && st.sel_node >= 0 && st.path_target_zone >= 0 &&
        st.path_target_node >= 0) {
        const MapNodeData& a = st.map.zones[st.sel_zone].nodes[st.sel_node];
        const MapNodeData& b = st.map.zones[st.path_target_zone].nodes[st.path_target_node];
        path = map_find_path(st.map, a.level_name, b.level_name);
    }
    std::set<std::string> path_set(path.begin(), path.end());
    std::set<std::pair<std::string,std::string>> path_edges;

    if (st.show_portals) {
        for (const auto& z : st.map.zones)
            for (const auto& n : z.nodes) {
                const LayoutEntry* e = layout_of(st, n.level_name);
                float ax = e ? e->lx : n.lx;
                float ay = e ? e->ly : n.ly;
                for (const auto& p : n.portals) {
                    auto it = st.map.node_index.find(p.destination_name);
                    ImVec2 a = world_to_screen(st, c, ax, ay);
                    if (it == st.map.node_index.end()) {
                        // missing target: red dashed stub
                        ImVec2 b(a.x + 40.f, a.y - 20.f);
                        dl->AddLine(a, b, IM_COL32(255, 80, 80, 180), 1.5f);
                        continue;
                    }
                    auto [dz, dn] = it->second;
                    const MapNodeData& d = st.map.zones[dz].nodes[dn];
                    const LayoutEntry* de = layout_of(st, d.level_name);
                    float bx = de ? de->lx : d.lx;
                    float by = de ? de->ly : d.ly;
                    ImVec2 b = world_to_screen(st, c, bx, by);
                    bool on_path = path_set.count(n.level_name) && path_set.count(p.destination_name);
                    bool edge_path = path_edges.count({n.level_name, p.destination_name});
                    bool hl = on_path || edge_path;
                    // Portal connections stay *subtle*: thin, low-prominence
                    // gentle curves (seebro.md §5). Only the BFS-highlighted
                    // route is bright.
                    ImU32 col = hl ? IM_COL32(255, 220, 90, 240)
                                   : dir_color(p.direction);
                    float th = hl ? 2.6f : 1.0f;
                    float pdx = b.x - a.x, pdy = b.y - a.y;
                    float plen = std::sqrt(pdx * pdx + pdy * pdy);
                    ImVec2 bend_mid(0.f, 0.f);
                    if (plen > 1.f) {
                        float bend = std::min(30.f, plen * 0.12f);
                        bend_mid = ImVec2((a.x + b.x) * 0.5f + (-pdy / plen) * bend,
                                          (a.y + b.y) * 0.5f + (pdx / plen) * bend);
                        dl->AddBezierQuadratic(a, bend_mid, b, col, th);
                    } else {
                        dl->AddLine(a, b, col, th);
                    }
                    // guide-edge arrow sprite in the middle of the path
                    // (the same ui_map_guide_edge the game draws on travel paths)
                    GLuint ge = load_embedded_texture(guide_edge_name());
                    if (ge) {
                        float dx = b.x - a.x, dy = b.y - a.y;
                        float len = std::sqrt(dx * dx + dy * dy);
                        if (len > 30.f) {
                            float ang = std::atan2(dy, dx) * 180.f / 3.14159265f;
                            float s = (on_path || edge_path) ? 10.f : 7.f;
                            // Place the arrow on the bezier itself (quadratic
                            // at t=0.5 = 0.25a + 0.5*control + 0.25b).
                            ImVec2 ctr = ImVec2(a.x * 0.25f + bend_mid.x * 0.5f + b.x * 0.25f,
                                                a.y * 0.25f + bend_mid.y * 0.5f + b.y * 0.25f);
                            draw_image_rotated(dl, (ImTextureID)(intptr_t)ge,
                                               ctr, s, s * 0.55f, ang, col | 0xFF000000);
                        }
                    }
                    // path dot steps on the highlighted route
                    if (on_path || edge_path) {
                        GLuint pd = load_embedded_texture(path_dot_name());
                        if (pd) {
                            float dx = b.x - a.x, dy = b.y - a.y;
                            float len = std::sqrt(dx * dx + dy * dy);
                            int steps = (int)(len / 26.f);
                            for (int s = 1; s < steps; ++s) {
                                float t = (float)s / (float)steps;
                                dl->AddImage((ImTextureID)(intptr_t)pd,
                                             ImVec2(a.x + dx * t - 5.f, a.y + dy * t - 5.f),
                                             ImVec2(a.x + dx * t + 5.f, a.y + dy * t + 5.f),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 230, 120, 235));
                            }
                        }
                    }
                    // arrowhead
                    ImVec2 dirv = ImVec2(b.x - a.x, b.y - a.y);
                    float len = std::sqrt(dirv.x * dirv.x + dirv.y * dirv.y);
                    if (len > 12.f) {
                        dirv.x /= len; dirv.y /= len;
                        ImVec2 perp(-dirv.y, dirv.x);
                        float asz = 5.f + 2.f * st.zoom;
                        ImVec2 tip = ImVec2(b.x - dirv.x * 6.f, b.y - dirv.y * 6.f);
                        dl->AddTriangleFilled(tip,
                                              ImVec2(tip.x - dirv.x * asz + perp.x * asz * 0.6f,
                                                     tip.y - dirv.y * asz + perp.y * asz * 0.6f),
                                              ImVec2(tip.x - dirv.x * asz - perp.x * asz * 0.6f,
                                                     tip.y - dirv.y * asz - perp.y * asz * 0.6f),
                                              col);
                    }
                }
            }
    }

    // ── Nodes ───────────────────────────────────────────────────────────────
    int idx = 0;
    const int sel_flat = (st.sel_zone >= 0 && st.sel_node >= 0)
        ? (int)st.map.zones[st.sel_zone].nodes.size() * 0 + st.sel_node : -1;
    (void)sel_flat;
    std::vector<ImRect> label_rects;   // zoom-dependent label collision avoidance
    for (const auto& z : st.map.zones)
        for (const auto& n : z.nodes) {
            const LayoutEntry* e = layout_of(st, n.level_name);
            float lx = e ? e->lx : n.lx;
            float ly = e ? e->ly : n.ly;
            ImVec2 p = world_to_screen(st, c, lx, ly);
            bool selected = (st.sel_zone >= 0 && st.sel_node >= 0 &&
                             &st.map.zones[st.sel_zone].nodes[st.sel_node] == &n);
            bool target = (st.path_target_zone >= 0 && st.path_target_node >= 0 &&
                           &st.map.zones[st.path_target_zone].nodes[st.path_target_node] == &n);

            float size = std::clamp(36.f * st.zoom, 26.f, 88.f);
            GLuint tex = load_embedded_texture(node_sprite_name(n.type));
            if (n.hidden) size *= 0.8f;
            if (selected || target) size += 8.f;

            // drop shadow
            dl->AddCircleFilled(ImVec2(p.x + 3.f, p.y + 4.f), size * 0.5f + 4.f,
                                IM_COL32(0, 0, 0, 170), 28);

            // selected ring sprite (ui_map_marker_selected) UNDER the node
            if (selected || target) {
                GLuint ring = load_embedded_texture(selected_marker_name());
                if (ring) {
                    float rs = size * 0.72f;
                    dl->AddImage((ImTextureID)(intptr_t)ring,
                                 ImVec2(p.x - rs, p.y - rs), ImVec2(p.x + rs, p.y + rs),
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 selected ? IM_COL32(255, 255, 255, 255)
                                          : IM_COL32(255, 220, 90, 220));
                } else {
                    if (selected)
                        dl->AddCircle(p, size * 0.5f + 4.f, IM_COL32(255, 255, 255, 240), 28, 3.f);
                    else if (target)
                        dl->AddCircle(p, size * 0.5f + 4.f, IM_COL32(255, 220, 90, 240), 28, 2.5f);
                }
            }

            if (st.graph_mode) {
                // Graph/debug mode: the Swordigo map look — a large golden
                // hexagonal node body with the type sprite centered inside.
                float hr = size * 0.5f;
                dl->AddNgonFilled(p, hr, IM_COL32(214, 168, 66, 255), 6);
                dl->AddNgonFilled(p, hr - 4.f, IM_COL32(238, 198, 106, 255), 6);
                dl->AddNgon(p, hr, IM_COL32(92, 62, 18, 255), 6, 2.5f);
                if (tex) {
                    float ts = hr * 0.72f;
                    ImU32 tint = n.hidden ? IM_COL32(255, 255, 255, 110)
                                          : IM_COL32(255, 255, 255, 255);
                    dl->AddImage((ImTextureID)(intptr_t)tex,
                                 ImVec2(p.x - ts, p.y - ts), ImVec2(p.x + ts, p.y + ts),
                                 ImVec2(0, 0), ImVec2(1, 1), tint);
                }
            } else if (tex) {
                ImU32 tint = IM_COL32(255, 255, 255, 255);
                if (n.hidden) tint = IM_COL32(255, 255, 255, 95);
                dl->AddImage((ImTextureID)(intptr_t)tex,
                             ImVec2(p.x - size * 0.5f, p.y - size * 0.5f),
                             ImVec2(p.x + size * 0.5f, p.y + size * 0.5f),
                             ImVec2(0, 0), ImVec2(1, 1), tint);
            } else {
                dl->AddCircleFilled(p, size * 0.5f, node_label_color(n.type), 24);
                dl->AddCircle(p, size * 0.5f, IM_COL32(0, 0, 0, 160), 24, 2.f);
            }

            // ── overlay badges (same sprites as the game) ──────────────────
            auto badge = [&](const char* nm, float bx, float by, float bs) {
                GLuint bt = load_embedded_texture(nm);
                if (bt)
                    dl->AddImage((ImTextureID)(intptr_t)bt,
                                 ImVec2(p.x + bx - bs * 0.5f, p.y + by - bs * 0.5f),
                                 ImVec2(p.x + bx + bs * 0.5f, p.y + by + bs * 0.5f),
                                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 235));
            };
            bool locked = n.experience_level > 0 && n.experience_level > st.player_level;
            // XP-gated → locked badge (bottom-left corner)
            if (locked && !n.hidden)
                badge(locked_badge_name(), -size * 0.5f, size * 0.5f, size * 0.42f);
            // treasure node → gold badge (top-right corner)
            if (n.num_treasures > 0)
                badge(treasure_badge_name(), size * 0.5f, -size * 0.5f, size * 0.40f);
            // has portal → portal badge (top-left corner)
            if (n.has_portal)
                badge(portal_badge_name(), -size * 0.5f, -size * 0.5f, size * 0.36f);

            // label — zoom-dependent with collision avoidance (seebro.md §4):
            // far zoom shows only selected/hero nodes; labels fade in and are
            // dropped when they would overlap an already-drawn label.
            if (st.show_labels) {
                bool sel_cur = selected || target || (n.level_name == st.current_node);
                // Labels fade in between zoom 0.45 and 0.65 (no hard pop-in).
                float zoom_fade = std::clamp((st.zoom - 0.45f) / 0.20f, 0.f, 1.f);
                if (sel_cur || zoom_fade > 0.f) {
                    std::string label = n.title.empty() ? n.level_name : n.title;
                    if (locked) label += " [Lv " + std::to_string(n.experience_level) + "]";
                    if (n.hidden) label += " (hidden)";
                    ImU32 lc = node_label_color(n.type);
                    if (n.hidden) lc = IM_COL32(150, 150, 160, 200);
                    if (locked) lc = IM_COL32(160, 100, 100, 200);
                    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    float label_alpha = n.hidden ? 0.4f : (locked ? 0.6f : 1.f);
                    if (!sel_cur) label_alpha *= (0.35f + 0.65f * zoom_fade);
                    ImRect r(p.x - ts.x * 0.5f - 4.f, p.y + size * 0.6f + 2.f,
                             p.x + ts.x * 0.5f + 4.f, p.y + size * 0.6f + 6.f + ts.y);
                    bool collide = false;
                    for (const ImRect& o : label_rects)
                        if (r.Overlaps(o)) { collide = true; break; }
                    if (!collide) {
                        label_rects.push_back(r);
                        dl->AddText(ImGui::GetFont(), 12.f,
                                    ImVec2(p.x - ts.x * 0.5f, p.y + size * 0.6f + 4.f),
                                    lc | ((int)(label_alpha * 255.f) & 0xFF), label.c_str());
                    }
                }
            }
            // Dim locked nodes (draw a translucent black overlay)
            if (locked && tex) {
                dl->AddRectFilled(ImVec2(p.x - size * 0.5f, p.y - size * 0.5f),
                                  ImVec2(p.x + size * 0.5f, p.y + size * 0.5f),
                                  IM_COL32(0, 0, 0, 110), 6.f);
            }
            ++idx;
        }

    // ── Hero marker (current location) ─────────────────────────────────────
    // Drawn on top of everything, pulsing, at the "current" map node — the
    // same ui_map_marker_hero the game shows on the world map.
    if (st.show_labels && !st.current_node.empty()) {
        for (const auto& z : st.map.zones)
            for (const auto& n : z.nodes) {
                if (n.level_name != st.current_node) continue;
                const LayoutEntry* e = layout_of(st, n.level_name);
                ImVec2 p = world_to_screen(st, c, e ? e->lx : n.lx, e ? e->ly : n.ly);
                GLuint hm = load_embedded_texture(hero_marker_name());
                st.hero_pulse += ImGui::GetIO().DeltaTime * 3.f;
                float bob = std::sin(st.hero_pulse) * 3.f;
                float hs = 20.f + 6.f * std::sin(st.hero_pulse * 0.7f);
                // soft glow
                dl->AddCircleFilled(ImVec2(p.x, p.y - hs * 0.5f + bob), hs * 0.75f,
                                    IM_COL32(255, 200, 60, 60), 20);
                if (hm) {
                    dl->AddImage((ImTextureID)(intptr_t)hm,
                                 ImVec2(p.x - hs * 0.5f, p.y - hs + bob),
                                 ImVec2(p.x + hs * 0.5f, p.y + bob),
                                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
                } else {
                    dl->AddTriangleFilled(p, ImVec2(p.x - 9.f, p.y - hs + bob),
                                          ImVec2(p.x + 9.f, p.y - hs + bob),
                                          IM_COL32(255, 190, 60, 255));
                }
            }
    }

    // ── Portal creation preview (source highlighted) ────────────────────────
    if (st.tool == MapTool::AddPortal && st.portal_src_zone >= 0 && st.portal_src_node >= 0) {
        const MapNodeData& s = st.map.zones[st.portal_src_zone].nodes[st.portal_src_node];
        const LayoutEntry* e = layout_of(st, s.level_name);
        ImVec2 sp = world_to_screen(st, c, e ? e->lx : s.lx, e ? e->ly : s.ly);
        dl->AddCircle(sp, 22.f * std::clamp(st.zoom, 0.5f, 2.f), IM_COL32(255, 200, 80, 255), 24, 2.f);
        dl->AddLine(sp, io.MousePos, IM_COL32(255, 200, 80, 140), 1.2f);
    }

    // ── Mouse interaction ───────────────────────────────────────────────────
    if (hovered && !over_zoom_ui) {
        // Node hit test
        int hit = hit_test_node(st, c, io.MousePos);

        // LMB press: select / start drag / add node / add portal
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && st.tool != MapTool::Pan) {
            if (st.tool == MapTool::AddNode) {
                // add node at click point
                ImVec2 w = screen_to_world(st, c, io.MousePos);
                int zi = (st.sel_zone >= 0) ? st.sel_zone : 0;
                if (zi < (int)st.map.zones.size() && !st.map.zones.empty()) {
                    std::string base = "new_level";
                    std::string name = base;
                    int k = 1;
                    while (st.map.node_index.count(name)) name = base + std::to_string(k++);
                    MkBlock* zb = zone_block(st.map, zi);
                    if (zb) {
                        MkField* nf = mk_add_msg(*zb, "Node");
                        mk_msg_set_str(*nf, "LevelName", name);
                        mark_dirty(st);
                        // find the new node in the overlay
                        st.sel_zone = zi;
                        st.sel_node = (int)st.map.zones[zi].nodes.size() - 1;
                        set_layout(st, name, w.x, w.y, true);
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Added node '%s'", name.c_str());
                        st.status = buf;
                    }
                }
            } else if (st.tool == MapTool::AddPortal) {
                if (hit >= 0) {
                    int zi = -1, ni = -1;
                    int idx2 = 0;
                    for (size_t zz = 0; zz < st.map.zones.size(); ++zz)
                        for (size_t nn = 0; nn < st.map.zones[zz].nodes.size(); ++nn) {
                            if (idx2 == hit) { zi = (int)zz; ni = (int)nn; }
                            ++idx2;
                        }
                    if (st.portal_src_zone < 0) {
                        st.portal_src_zone = zi; st.portal_src_node = ni;
                        st.status = "Click the destination node";
                    } else if (zi != st.portal_src_zone || ni != st.portal_src_node) {
                        MapZoneData& z = st.map.zones[st.portal_src_zone];
                        MapNodeData& n = z.nodes[st.portal_src_node];
                        MkBlock* zb = zone_block(st.map, st.portal_src_zone);
                        MkField* nf = node_field(zb, st.portal_src_node);
                        if (nf) {
                            bool exists = false;
                            for (const auto& p : n.portals)
                                if (p.destination_name == st.map.zones[zi].nodes[ni].level_name)
                                    exists = true;
                            if (!exists) {
                                MkField* pf = mk_add_msg(*nf, "Portal");
                                mk_msg_set_str(*pf, "DestinationName",
                                               st.map.zones[zi].nodes[ni].level_name);
                                mark_dirty(st);
                                st.status = "Portal added";
                            } else {
                                st.status = "Portal already exists";
                            }
                        }
                        st.portal_src_zone = st.portal_src_node = -1;
                    }
                }
            } else { // Select
                if (hit >= 0) {
                    ImVec2 w = screen_to_world(st, c, io.MousePos);
                    int idx2 = 0;
                    for (size_t zz = 0; zz < st.map.zones.size(); ++zz)
                        for (size_t nn = 0; nn < st.map.zones[zz].nodes.size(); ++nn) {
                            if (idx2 == hit) {
                                // double-click → open the level scene
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                    st.open_scene_request = true;
                                    st.open_scene_level = st.map.zones[zz].nodes[nn].level_name;
                                }
                                st.sel_zone = (int)zz;
                                st.sel_node = (int)nn;
                                st.sel_portal = -1;
                                st.dragging = true;
                                st.drag_zone = (int)zz;
                                st.drag_node = (int)nn;
                                const LayoutEntry* e = layout_of(st, st.map.zones[zz].nodes[nn].level_name);
                                float lx = e ? e->lx : st.map.zones[zz].nodes[nn].lx;
                                float ly = e ? e->ly : st.map.zones[zz].nodes[nn].ly;
                                st.drag_off_x = lx - w.x;
                                st.drag_off_y = ly - w.y;
                            }
                            ++idx2;
                        }
                } else {
                    st.sel_zone = st.sel_node = st.sel_portal = -1;
                }
            }
        }

        // Drag node (Select tool)
        if (st.dragging && st.tool == MapTool::Select && io.MouseDown[0]) {
            ImVec2 w = screen_to_world(st, c, io.MousePos);
            if (st.drag_zone >= 0 && st.drag_node >= 0 &&
                st.drag_zone < (int)st.map.zones.size() &&
                st.drag_node < (int)st.map.zones[st.drag_zone].nodes.size()) {
                MapNodeData& n = st.map.zones[st.drag_zone].nodes[st.drag_node];
                set_layout(st, n.level_name, w.x + st.drag_off_x, w.y + st.drag_off_y, true);
            }
        }
        if (!io.MouseDown[0]) st.dragging = false;

        // RMB: context menu
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("MapCtx");
        }
        if (ImGui::BeginPopup("MapCtx")) {
            ImVec2 w = screen_to_world(st, c, io.MousePos);
            if (hit >= 0) {
                // resolve flat index
                int idx2 = 0;
                for (size_t zz = 0; zz < st.map.zones.size(); ++zz)
                    for (size_t nn = 0; nn < st.map.zones[zz].nodes.size(); ++nn) {
                        if (idx2 == hit) {
                            if (ImGui::MenuItem(ICON_FA_MAP_LOCATION_DOT " Open Level Scene"))
                                { st.open_scene_request = true; st.open_scene_level = st.map.zones[zz].nodes[nn].level_name; }
                            if (ImGui::MenuItem(ICON_FA_LINK " Portal From Here"))
                                { st.tool = MapTool::AddPortal; st.portal_src_zone = (int)zz; st.portal_src_node = (int)nn; }
                            if (ImGui::MenuItem(ICON_FA_LOCATION_DOT " Set Path Target"))
                                { st.path_target_zone = (int)zz; st.path_target_node = (int)nn; }
                            if (ImGui::MenuItem(ICON_FA_MAP_LOCATION_DOT " Set as Current Location")) {
                                st.current_node = st.map.zones[zz].nodes[nn].level_name;
                                st.status = "Hero marker placed at '" + st.current_node + "'";
                            }
                            if (ImGui::MenuItem(ICON_FA_TRASH " Delete Node")) {
                                MkBlock* zb = zone_block(st.map, (int)zz);
                                if (zb) {
                                    auto nodes = mk_msgs(*zb, "Node");
                                    if ((int)nodes.size() > (int)nn) {
                                        zb->fields.erase(zb->fields.begin() +
                                            (int)(nodes[nn] - &zb->fields[0]));
                                    }
                                    mark_dirty(st);
                                    st.sel_zone = st.sel_node = -1;
                                }
                            }
                            ImGui::Separator();
                            if (ImGui::MenuItem(ICON_FA_PLUS " Add Node Here")) {
                                MkBlock* zb = zone_block(st.map, (int)zz);
                                if (zb) {
                                    std::string base = "new_level";
                                    std::string name = base;
                                    int k = 1;
                                    while (st.map.node_index.count(name)) name = base + std::to_string(k++);
                                    MkField* nf = mk_add_msg(*zb, "Node");
                                    mk_msg_set_str(*nf, "LevelName", name);
                                    mark_dirty(st);
                                    st.sel_zone = (int)zz;
                                    st.sel_node = (int)st.map.zones[zz].nodes.size() - 1;
                                    set_layout(st, name, w.x, w.y, true);
                                }
                            }
                        }
                        ++idx2;
                    }
            } else {
                if (ImGui::MenuItem(ICON_FA_PLUS " Add Zone")) {
                    MkBlock zb;
                    zb.type = "Zone";
                    std::string base = "new_zone";
                    std::string name = base;
                    int k = 1;
                    auto has_zone = [&](const std::string& nm) {
                        for (const auto& z : st.map.zones) if (z.name == nm) return true;
                        return false;
                    };
                    while (has_zone(name)) name = base + std::to_string(k++);
                    mk_set_str(zb, "Name", name);
                    st.map.root.push_back(std::move(zb));
                    mark_dirty(st);
                    st.sel_zone = (int)st.map.zones.size() - 1;
                    st.sel_node = -1;
                    st.status = "Added zone '" + name + "'";
                }
            }
            ImGui::EndPopup();
        }
    }

    // ── Path summary ────────────────────────────────────────────────────────
    if (!path.empty()) {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(c.origin.x + 10.f, c.origin.y + c.size.y - 22.f),
            IM_COL32(255, 220, 90, 220),
            ("Path: " + path.front() + " → ... → " + path.back() +
             "  (" + std::to_string((int)path.size()) + " stops)").c_str());
    }

    // Empty state
    if (st.map.zones.empty() && st.loaded) {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(c.origin.x + 20.f, c.origin.y + 70.f),
            IM_COL32(160, 170, 190, 220),
            "Empty map — right-click to add a zone, or double-click a node to open its scene.");
    }

    // ── Swordigo-style zoom controls overlay (seebro.md §7) ─────────────────
    if (st.show_zoom_ui) {
        // Compass (top-left)
        ImVec2 comp(c.origin.x + 30.f, c.origin.y + 30.f);
        dl->AddCircleFilled(comp, 20.f, IM_COL32(10, 14, 24, 210), 32);
        dl->AddCircle(comp, 20.f, IM_COL32(120, 130, 160, 150), 32, 1.4f);
        dl->AddTriangleFilled(ImVec2(comp.x, comp.y - 7.f),
                              ImVec2(comp.x - 5.f, comp.y + 7.f),
                              ImVec2(comp.x + 5.f, comp.y + 7.f),
                              IM_COL32(230, 180, 60, 255));
        dl->AddText(ImGui::GetFont(), 12.f,
                    ImVec2(comp.x - 5.f, comp.y - 30.f), IM_COL32(230, 180, 60, 255), "N");
        dl->AddText(ImGui::GetFont(), 11.f,
                    ImVec2(comp.x + 13.f, comp.y + 7.f), IM_COL32(150, 160, 190, 190), "E");
        dl->AddText(ImGui::GetFont(), 11.f,
                    ImVec2(comp.x - 19.f, comp.y + 7.f), IM_COL32(150, 160, 190, 190), "W");

        // Zoom slider column (top-right): +, slider, -, % readout
        ImGui::SetCursorScreenPos(zo_origin);
        ImGui::InvisibleButton("##zplus", ImVec2(zo_w, 22.f));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) apply_zoom(1.25f);
        ImVec2 pb = ImGui::GetItemRectMin();
        dl->AddRectFilled(pb, ImVec2(pb.x + zo_w, pb.y + 22.f), IM_COL32(30, 38, 58, 235), 4.f);
        dl->AddLine(ImVec2(pb.x + zo_w * 0.5f - 5.f, pb.y + 11.f),
                    ImVec2(pb.x + zo_w * 0.5f + 5.f, pb.y + 11.f), IM_COL32(220, 230, 250, 255), 2.f);
        dl->AddLine(ImVec2(pb.x + zo_w * 0.5f, pb.y + 6.f),
                    ImVec2(pb.x + zo_w * 0.5f, pb.y + 16.f), IM_COL32(220, 230, 250, 255), 2.f);

        ImGui::SetCursorScreenPos(ImVec2(zo_origin.x, zo_origin.y + 26.f));
        ImGui::VSliderFloat("##zslider", ImVec2(zo_w, zo_h), &st.zoom,
                            st.zoom_min, st.zoom_max, "");

        ImGui::SetCursorScreenPos(ImVec2(zo_origin.x, zo_origin.y + 26.f + zo_h + 4.f));
        ImGui::InvisibleButton("##zminus", ImVec2(zo_w, 22.f));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) apply_zoom(0.8f);
        ImVec2 mb = ImGui::GetItemRectMin();
        dl->AddRectFilled(mb, ImVec2(mb.x + zo_w, mb.y + 22.f), IM_COL32(30, 38, 58, 235), 4.f);
        dl->AddLine(ImVec2(mb.x + zo_w * 0.5f - 5.f, mb.y + 11.f),
                    ImVec2(mb.x + zo_w * 0.5f + 5.f, mb.y + 11.f), IM_COL32(220, 230, 250, 255), 2.f);

        char zbuf[24];
        snprintf(zbuf, sizeof(zbuf), "%d%%", (int)(st.zoom * 100.f));
        ImVec2 zs = ImGui::CalcTextSize(zbuf);
        dl->AddText(ImGui::GetFont(), 12.f,
                    ImVec2(zo_origin.x + zo_w * 0.5f - zs.x * 0.5f,
                           zo_origin.y + zo_h + 30.f),
                    IM_COL32(200, 210, 235, 235), zbuf);
    }

    ImGui::SetCursorScreenPos(ImVec2(c.origin.x, c.origin.y));
    ImGui::InvisibleButton("map_canvas", c.size);
}

// ── Inspector ───────────────────────────────────────────────────────────────
static void draw_inspector(MapEditorState& st) {
    ImGui::BeginChild("MapInspector", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::TextDisabled("Map %s", st.map.filepath.empty() ? "(none)" : st.map.filepath.c_str());
    ImGui::Text("%zu zones  ·  %zu nodes  ·  %zu portals",
                st.map.zones.size(), st.map.node_index.size(),
                (size_t)[&]() {
                    size_t n = 0;
                    for (const auto& z : st.map.zones)
                        for (const auto& nd : z.nodes) n += nd.portals.size();
                    return n;
                }());
    ImGui::Separator();

    // ── Zones ───────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Zones", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int zi = 0; zi < (int)st.map.zones.size(); ++zi) {
            MapZoneData& z = st.map.zones[zi];
            char label[256];
            snprintf(label, sizeof(label), "%s%s%s  [%zu]", z.name.c_str(),
                     z.title.empty() ? "" : " — ", z.title.empty() ? "" : z.title.c_str(),
                     z.nodes.size());
            bool sel = (st.sel_zone == zi);
            if (ImGui::Selectable(label, sel)) {
                st.sel_zone = zi;
                st.sel_node = st.sel_portal = -1;
            }
            if (sel) {
                ImGui::Indent();
                static char buf_name[128], buf_title[256], buf_music[128];
                snprintf(buf_name, sizeof(buf_name), "%s", z.name.c_str());
                snprintf(buf_title, sizeof(buf_title), "%s", z.title.c_str());
                snprintf(buf_music, sizeof(buf_music), "%s", z.music.c_str());
                bool changed = false;
                if (ImGui::InputText("Name", buf_name, sizeof(buf_name))) changed = true;
                if (ImGui::InputText("Title", buf_title, sizeof(buf_title))) changed = true;
                if (ImGui::InputText("Music", buf_music, sizeof(buf_music))) changed = true;
                int xp = z.experience_level;
                if (ImGui::InputInt("Experience Level", &xp)) changed = true;
                // zone bg override
                std::string cur = zone_background_name(z.name);
                auto ov = st.zone_bg_override.find(z.name);
                if (ov != st.zone_bg_override.end()) cur = ov->second;
                static const char* kBgOptions[] = {
                    "auto", "bg0.png", "pic0.png", "caves_bg.png", "forest_bg.png",
                    "grasslands_bg.png", "graveyard_bg.png", "grove_bg.png"
                };
                int bg_idx = 0;
                for (int i = 0; i < 8; ++i)
                    if (cur == kBgOptions[i]) bg_idx = i;
                const char* preview = cur == "auto" ? "auto" : cur.c_str();
                if (ImGui::BeginCombo("Background", preview)) {
                    for (int i = 0; i < 8; ++i) {
                        if (ImGui::Selectable(kBgOptions[i], i == bg_idx)) {
                            if (i == 0) st.zone_bg_override.erase(z.name);
                            else st.zone_bg_override[z.name] = kBgOptions[i];
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (changed) {
                    MkBlock* zb = zone_block(st.map, zi);
                    if (zb) {
                        mk_set_str(*zb, "Name", buf_name);
                        mk_set_str(*zb, "Title", buf_title);
                        mk_set_str(*zb, "Music", buf_music);
                        mk_set_int(*zb, "ExperienceLevel", xp);
                    }
                    mark_dirty(st);
                }
                ImGui::Unindent();
            }
        }
        if (ImGui::Button(ICON_FA_PLUS " Add Zone")) {
            MkBlock zb;
            zb.type = "Zone";
            std::string base = "new_zone";
            std::string name = base;
            int k = 1;
            auto has_zone = [&](const std::string& nm) {
                for (const auto& z : st.map.zones) if (z.name == nm) return true;
                return false;
            };
            while (has_zone(name)) name = base + std::to_string(k++);
            mk_set_str(zb, "Name", name);
            st.map.root.push_back(std::move(zb));
            mark_dirty(st);
            st.sel_zone = (int)st.map.zones.size() - 1;
        }
        ImGui::Separator();
    }

    // ── Selected zone's nodes ───────────────────────────────────────────────
    if (st.sel_zone >= 0 && st.sel_zone < (int)st.map.zones.size()) {
        MapZoneData& z = st.map.zones[st.sel_zone];
        if (ImGui::CollapsingHeader("Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int ni = 0; ni < (int)z.nodes.size(); ++ni) {
                MapNodeData& n = z.nodes[ni];
                char label[256];
                snprintf(label, sizeof(label), "%s%s  [%s]", n.level_name.c_str(),
                         n.title.empty() ? "" : (" — " + n.title).c_str(),
                         type_name(n.type));
                bool sel = (st.sel_node == ni);
                if (ImGui::Selectable(label, sel)) {
                    st.sel_node = ni;
                    st.sel_portal = -1;
                }
                if (sel) {
                    ImGui::Indent();
                    static char buf_lv[256], buf_ti[256], buf_mu[128];
                    snprintf(buf_lv, sizeof(buf_lv), "%s", n.level_name.c_str());
                    snprintf(buf_ti, sizeof(buf_ti), "%s", n.title.c_str());
                    snprintf(buf_mu, sizeof(buf_mu), "%s", n.music.c_str());
                    bool changed = false;
                    if (ImGui::InputText("Level Name", buf_lv, sizeof(buf_lv))) changed = true;
                    if (ImGui::InputText("Title", buf_ti, sizeof(buf_ti))) changed = true;
                    if (ImGui::InputText("Music", buf_mu, sizeof(buf_mu))) changed = true;
                    int type = n.type;
                    if (ImGui::Combo("Type", &type, "Plain\0Town\0Waypoint\0Boss\0")) changed = true;
                    bool hidden = n.hidden;
                    if (ImGui::Checkbox("Hidden", &hidden)) changed = true;
                    int xp = n.experience_level;
                    if (ImGui::InputInt("Experience Level", &xp)) changed = true;
                    int tr = n.num_treasures;
                    if (ImGui::InputInt("Treasures", &tr)) changed = true;
                    bool hp = n.has_portal;
                    if (ImGui::Checkbox("HasPortal", &hp)) changed = true;
                    bool ign = n.ignore_in_statistics;
                    if (ImGui::Checkbox("IgnoreInStatistics", &ign)) changed = true;
                    if (changed) {
                        MkBlock* zb = zone_block(st.map, st.sel_zone);
                        MkField* nf = node_field(zb, ni);
                        if (nf) {
                            std::string old_name = n.level_name;
                            mk_msg_set_str(*nf, "LevelName", buf_lv);
                            mk_msg_set_str(*nf, "Title", buf_ti);
                            mk_msg_set_str(*nf, "Music", buf_mu);
                            mk_msg_set_int(*nf, "Type", type);
                            mk_msg_set_int(*nf, "Hidden", hidden ? 1 : 0);
                            mk_msg_set_int(*nf, "ExperienceLevel", xp);
                            mk_msg_set_int(*nf, "NumTreasures", tr);
                            mk_msg_set_int(*nf, "HasPortal", hp ? 1 : 0);
                            mk_msg_set_int(*nf, "IgnoreInStatistics", ign ? 1 : 0);
                            mark_dirty(st);
                            // keep layout keyed by old name in sync
                            if (old_name != buf_lv) {
                                auto it = st.layout.find(old_name);
                                if (it != st.layout.end()) {
                                    st.layout[buf_lv] = it->second;
                                    st.layout.erase(old_name);
                                }
                            }
                        }
                    }
                    // ── Scene actions for this node (fused Scene Creator /
                    //    Procedural Generator). We only raise a request flag here;
                    //    asset_viewer consumes it after draw_map_editor (same
                    //    pattern as open_scene_request) so map_editor keeps no
                    //    dependency on scene_creator / sgen.
                    ImGui::Spacing();
                    if (ImGui::Button("Open Scene")) {
                        st.open_scene_request = true;
                        st.open_scene_level = n.level_name;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Open this node's .scene (next to the .scmap) in the scene editor");
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS " Create Scene")) {
                        st.create_scene_request = true;
                        st.create_scene_level = n.level_name;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Create a new .scene for this node from a template,\n"
                                          "auto-linked back to this map");
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_MOUNTAIN_SUN " Generate Terrain")) {
                        st.gen_scene_request = true;
                        st.gen_scene_level = n.level_name;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Procedurally generate this node's .scene from a biome preset");
                    ImGui::Unindent();
                }
            }
            ImGui::Separator();
        }

        // ── Selected node's portals ─────────────────────────────────────────
        if (st.sel_node >= 0 && st.sel_node < (int)z.nodes.size()) {
            MapNodeData& n = z.nodes[st.sel_node];
            if (ImGui::CollapsingHeader("Portals", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int pi = 0; pi < (int)n.portals.size(); ++pi) {
                    MapPortal& p = n.portals[pi];
                    char label[256];
                    snprintf(label, sizeof(label), "→ %s%s", p.destination_name.c_str(),
                             p.direction ? (" [dir " + std::to_string(p.direction) + "]").c_str() : "");
                    bool sel = (st.sel_portal == pi);
                    if (ImGui::Selectable(label, sel)) st.sel_portal = pi;
                    if (sel) {
                        ImGui::Indent();
                        MkBlock* zb = zone_block(st.map, st.sel_zone);
                        MkField* nf = node_field(zb, st.sel_node);
                        if (nf) {
                            auto portals = mk_msgs(*nf, "Portal");
                            if (pi < (int)portals.size()) {
                                MkField* pf = portals[pi];
                                // destination combo over all levels
                                const char* preview = p.destination_name.c_str();
                                if (ImGui::BeginCombo("Destination", preview)) {
                                    for (const auto& [name, idx] : st.map.node_index) {
                                        if (ImGui::Selectable(name.c_str(),
                                                              name == p.destination_name)) {
                                            mk_msg_set_str(*pf, "DestinationName", name);
                                            mark_dirty(st);
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                                int dir = p.direction;
                                if (ImGui::Combo("Direction", &dir,
                                                 "None (auto)\0E\0NE\0N\0NW\0W\0SW\0S\0SE\0")) {
                                    mk_msg_set_int(*pf, "Direction", dir);
                                    mark_dirty(st);
                                }
                                int pd = p.pass_direction;
                                if (ImGui::Combo("PassDirection", &pd,
                                                 "None\0E\0NE\0N\0NW\0W\0SW\0S\0SE\0")) {
                                    mk_msg_set_int(*pf, "PassDirection", pd);
                                    mark_dirty(st);
                                }
                                bool ign = p.ignore_in_node_positioning;
                                if (ImGui::Checkbox("IgnoreInNodePositioning", &ign)) {
                                    mk_msg_set_int(*pf, "IgnoreInNodePositioning", ign ? 1 : 0);
                                    mark_dirty(st);
                                }
                                if (ImGui::Button(ICON_FA_TRASH " Remove Portal")) {
                                    nf->children.erase(nf->children.begin() +
                                        (int)(pf - &nf->children[0]));
                                    mark_dirty(st);
                                    st.sel_portal = -1;
                                }
                            }
                        }
                        ImGui::Unindent();
                    }
                }
                if (ImGui::Button(ICON_FA_PLUS " Add Portal")) {
                    // add an empty portal pointing at the first other node
                    MkBlock* zb = zone_block(st.map, st.sel_zone);
                    MkField* nf = node_field(zb, st.sel_node);
                    if (nf) {
                        MkField* pf = mk_add_msg(*nf, "Portal");
                        std::string target;
                        for (const auto& [name, idx] : st.map.node_index) {
                            if (name != n.level_name) { target = name; break; }
                        }
                        if (!target.empty()) mk_msg_set_str(*pf, "DestinationName", target);
                        mark_dirty(st);
                        st.sel_portal = (int)n.portals.size() - 1;
                    }
                }
                ImGui::Separator();
            }
        }

        if (ImGui::Button(ICON_FA_TRASH " Delete Zone")) {
            if (st.sel_zone >= 0 && st.sel_zone < (int)st.map.root.size()) {
                st.map.root.erase(st.map.root.begin() + st.sel_zone);
                mark_dirty(st);
                st.sel_zone = st.sel_node = -1;
            }
        }
    }

    // ── Validation ──────────────────────────────────────────────────────────
    if (st.show_validation) {
        if (ImGui::CollapsingHeader("Issues", st.map.issues.empty()
                ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (st.map.issues.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.f), ICON_FA_CIRCLE_CHECK " No issues");
            } else {
                for (const auto& is : st.map.issues)
                    ImGui::BulletText("%s", is.c_str());
            }
        }
    }

    ImGui::EndChild();
}

// ── Main frame ──────────────────────────────────────────────────────────────
void draw_map_editor(MapEditorState& st) {
    if (!st.loaded) {
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.4f, 1.f),
                           "No map loaded. Open a .scmap from the asset browser.");
        if (!st.error.empty()) ImGui::TextWrapped("%s", st.error.c_str());
        return;
    }

    draw_toolbar(st);

    float inspector_w = std::clamp(360.f, 260.f, 460.f);
    float canvas_w = ImGui::GetContentRegionAvail().x - inspector_w - 8.f;
    if (canvas_w < 200.f) { canvas_w = std::max(200.f, canvas_w); inspector_w = 0.f; }

    ImGui::BeginChild("MapCanvasChild", ImVec2(canvas_w, 0), ImGuiChildFlags_Borders);
    draw_canvas(st);
    ImGui::EndChild();

    if (inspector_w > 0.f) {
        ImGui::SameLine();
        ImGui::BeginChild("MapInspectorHost", ImVec2(inspector_w, 0));
        draw_inspector(st);
        ImGui::EndChild();
    }

    // Discard-edits confirmation before reloading a dirty map.
    if (st.confirm_reload) {
        ImGui::OpenPopup("Discard map edits?");
        st.confirm_reload = false;
    }
    if (ImGui::BeginPopupModal("Discard map edits?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Discard unsaved map edits and reload from disk?");
        ImGui::Spacing();
        if (ImGui::Button("Discard")) {
            std::string err;
            if (!map_editor_reload(st, &err)) st.status = "Reload failed: " + err;
            else st.status = "Reloaded from disk";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace mapedit
