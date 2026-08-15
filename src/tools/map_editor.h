// =============================================================================
// map_editor.h — Ruby world-map editor (zones → nodes → portals node graph).
//
// A full visual editor for Swordigo .scmap overworld travel maps:
//   • black canvas with pan/zoom + adaptive grid
//   • zone background art (embedded in libswcore via embedded_asset) behind
//     each zone's nodes — the same art the game shows on its world map
//   • node sprites (ui_map_guide_*) with type-aware styling + labels
//   • portal edges with direction colouring, arrowheads, missing-target red
//   • BFS travel-path highlight between two selected nodes
//   • node/zone/portal add-edit-delete (byte-exact .scmap save via map_loader)
//   • inspector for every field of the MapZone/MapNode/MapNode_Portal schema
//   • validation issues panel + scene-linking (double-click node → open .scene)
//   • editor-only layout persistence in a .swmap sidecar next to the .scmap
//     (vanilla game never sees it; positions are NOT written to the file
//     unless the user enables "write positions")
// =============================================================================
#pragma once

#include "tools/map_loader.h"

#include <string>
#include <vector>
#include <map>

namespace mapedit {

// Editor-only node layout keyed by level name (persisted to the sidecar;
// auto-layout output is written here too so drags start from a sane spot).
struct LayoutEntry {
    float lx = 0.f, ly = 0.f;
    bool  manual = false;   // user-dragged position (auto-layout keeps its own)
};

enum class MapTool {
    Select = 0,
    AddNode,
    AddPortal,
    Pan,
};

struct MapEditorState {
    MapData map;
    bool loaded = false;
    bool dirty = false;
    std::string error;

    // Camera (world coords at canvas centre; zoom = px per world unit)
    float cam_x = 0.f, cam_y = 0.f, zoom = 1.f;
    float zoom_min = 0.06f, zoom_max = 14.f;  // zoom slider bounds

    // Selection
    int sel_zone = -1, sel_node = -1, sel_portal = -1;
    int path_target_zone = -1, path_target_node = -1; // second endpoint for path preview

    // Clipboard (Ctrl+C / Ctrl+V) — copied node pasted into the selected zone.
    bool node_clipboard_valid = false;
    mapedit::MapNodeData node_clipboard; // copied node (Ctrl+C), pasted into selected zone (Ctrl+V)

    // Tool + interaction
    MapTool tool = MapTool::Select;
    bool dragging = false;
    int drag_zone = -1, drag_node = -1;
    float drag_off_x = 0.f, drag_off_y = 0.f;
    bool hovered = false;

    // Add-portal pending source (click source node, then dest node)
    int portal_src_zone = -1, portal_src_node = -1;

    // Display toggles
    bool show_zone_bg = true;
    bool show_grid = true;
    bool show_labels = true;
    bool show_portals = true;
    bool show_validation = true;
    bool write_positions = false;
    bool show_title_plates = true;   // parchment-style zone name plates
    bool show_positions = true;      // position/treasure/portal badges over nodes
    bool graph_mode = false;         // graph/debug view (auto-layout) vs geographic map view
    bool show_zoom_ui = true;        // Swordigo-style zoom slider overlay

    // Player progression — drives XP gates (nodes with a higher required
    // experience level show the locked badge, exactly like the in-game map).
    int player_level = 1;

    // Status message (transient)
    std::string status;
    float status_timer = 0.f;

    // Scene-link request — consumed by asset_viewer after draw_map_editor.
    bool open_scene_request = false;
    std::string open_scene_level;
    bool create_scene_request = false;
    std::string create_scene_level;
    bool gen_scene_request = false;
    std::string gen_scene_level;

    // "Current location" — the node the hero marker sits on (editor concept,
    // mirrors GameState.CurrentMapNodeName; persisted to the sidecar).
    std::string current_node;
    float hero_pulse = 0.f;   // hero marker pulse timer
    std::string save_file;    // last save used for "From Save" (for status/tooltip)
    int save_level = 0;       // player level from the save

    // Sidecar (.swmap) path — layout persistence (editor-only).
    std::string sidecar_path;
    // zone name → bg asset override (editor-only, persisted to sidecar)
    std::map<std::string, std::string> zone_bg_override;

    std::map<std::string, LayoutEntry> layout;
    // Graph/debug mode layout (auto-layout output, never written to the file
    // and never mixed into the geographic layout above).
    std::map<std::string, std::pair<float, float>> graph_layout;
    bool confirm_reload = false;   // pending "discard edits?" modal
};

// Open a .scmap (loads map + sidecar layout). Returns false + sets error.
bool map_editor_open(MapEditorState& st, const std::string& path, std::string* error);
// Save the .scmap (byte-exact when nothing changed) + sidecar layout.
bool map_editor_save(MapEditorState& st, std::string* error);
// Save the sidecar layout file only.
bool map_editor_save_sidecar(MapEditorState& st, std::string* error);
// Reload from disk (discards edits).
bool map_editor_reload(MapEditorState& st, std::string* error);
// Read the most recent .gplayer save and set hero marker + player level.
bool map_editor_load_from_save(MapEditorState& st, std::string* error);
// Fit the camera to all nodes.
void map_editor_fit_view(MapEditorState& st);
// Main ImGui frame (toolbar + canvas + inspector). Call from asset_viewer.
void draw_map_editor(MapEditorState& st);

} // namespace mapedit
