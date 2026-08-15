// =============================================================================
// map_loader.h — .scmap world-map data model for the Ruby map editor.
//
// The .scmap is the Swordigo overworld *travel graph* (zones → nodes →
// portals), stored as a protobuf (Map schema).  We decode it to the FileRift
// markup text via filerift, parse that into an ordered generic tree (preserving
// field order, presence AND unknown fields), and serialize the tree back so
// `recode_markup` reproduces byte-identical output for unmodified data.
//
// Node positions are NOT stored in vanilla .scmap files (the engine computes
// them from portal directions at load time — step 54.0 units along 8 compass
// octants, see Caver::Map::RecursivelySetNodePositions).  The editor therefore
// keeps layout in memory (auto-layout port + drag), and only writes Position
// fields into the file when the user opts in ("write positions").
// =============================================================================
#pragma once

#include <string>
#include <vector>
#include <map>
#include <utility>

namespace mapedit {

// ── Generic ordered markup tree ─────────────────────────────────────────────
// Mirrors the FileRift decoded text exactly: a block is "Zone{ ... }" and each
// field is either a scalar ("Name : 'town'") or a nested message ("Node{...}")
// whose children are again fields.
struct MkField {
    enum Kind { Scalar, Message };
    Kind  kind = Scalar;
    std::string key;                 // field name (e.g. "Name", "Node")
    std::string scalar;              // raw scalar text incl. quotes ("'town'", "1")
    std::vector<MkField> children;   // for Message
};

struct MkBlock {
    std::string type;                // "Zone" / "Node" / "Portal" / ...
    std::vector<MkField> fields;     // in original order
};

// ── Typed overlay ───────────────────────────────────────────────────────────
struct MapPortal {
    std::string destination_name;
    int direction = 0;               // 1..8 compass octant (0 = unset)
    int pass_direction = 0;
    bool ignore_in_node_positioning = false;
};

struct MapNodeData {
    std::string level_name;
    std::string title;
    int type = 0;                    // 0 plain, 1 town, 2 waypoint, 3 boss
    bool hidden = false;
    int experience_level = 0;
    std::string music;
    bool has_portal = false;
    int num_treasures = 0;
    bool ignore_in_statistics = false;
    std::vector<MapPortal> portals;
    // Editor layout (NOT serialized unless write_positions)
    float lx = 0.f, ly = 0.f;        // layout position (editor space)
    bool  manual = false;            // user-dragged (auto-layout keeps these)
};

struct MapZoneData {
    std::string name;
    std::string title;
    int experience_level = 0;
    std::string music;
    std::vector<MapNodeData> nodes;
};

// ── Whole map ───────────────────────────────────────────────────────────────
struct MapData {
    std::string filepath;            // .scmap path (empty = not loaded)
    std::vector<MkBlock> root;       // top-level blocks (Zone + any unknowns)
    std::vector<MapZoneData> zones;  // typed overlay (kept in sync on load)
    bool dirty = false;
    bool write_positions = false;    // persist manual node positions into .scmap
    std::vector<std::string> issues; // validation results
    std::map<std::string, std::pair<int,int>> node_index; // level → (zone,node)
};

// ── Load / save ─────────────────────────────────────────────────────────────
bool map_load(const std::string& path, MapData& out, std::string* error);
bool map_save(const std::string& path, const MapData& in, std::string* error);
// Serialize the generic tree back to FileRift markup (byte-faithful).
std::string map_to_markup(const MapData& in);
// Parse decoded markup text (from filerift::decode_protobuf) into the tree.
bool map_parse_markup(const std::string& markup, MapData& out, std::string* error);
// Rebuild the typed zone/node overlay + node index from the generic tree.
void map_rebuild(MapData& m);
// Inject manual node positions into the generic tree as Position{X,Y} messages
// (only the nodes flagged .manual).  Called before map_save when
// write_positions is enabled.
void map_inject_positions(MapData& m);

// ── Editor helpers ──────────────────────────────────────────────────────────
// Port of Caver::Map::RecursivelySetNodePositions: BFS from each zone's first
// node, placing each portal destination at parent + FromAngle(dir)*54 (rounded),
// skipping portals flagged IgnoreInNodePositioning.  Manual nodes keep their
// positions.  Returns count of nodes laid out.
int map_auto_layout(MapData& m);
// BFS path from node A to node B across portals (level names). Empty if none.
std::vector<std::string> map_find_path(const MapData& m,
                                       const std::string& from,
                                       const std::string& to);
// Validation: orphan portals, missing levels, dup names, untitled hubs.
void map_validate(MapData& m);
// Default zone background asset for a zone (by name convention).
std::string zone_background_name(const std::string& zone_name);

// ── Tree access helpers ─────────────────────────────────────────────────────
std::string mk_get_str(const MkBlock& b, const std::string& key);
int         mk_get_int(const MkBlock& b, const std::string& key, int def);
bool        mk_has(const MkBlock& b, const std::string& key);
void        mk_set_str(MkBlock& b, const std::string& key, const std::string& value);
void        mk_set_int(MkBlock& b, const std::string& key, int value);
void        mk_remove(MkBlock& b, const std::string& key);
// First message child field with this key (nullptr if none).
MkField*    mk_msg(MkBlock& b, const std::string& key);
// All message child fields with this key (e.g. all "Node" blocks of a zone).
std::vector<MkField*> mk_msgs(MkBlock& b, const std::string& key);
// Add a new empty message child (returns the new field).
MkField*    mk_add_msg(MkBlock& b, const std::string& key);
// Message variants: operate on a message field's children list directly.
MkField*    mk_add_msg(MkField& msg, const std::string& key);
std::vector<MkField*> mk_msgs(MkField& msg, const std::string& key);

// Scalar get/set operating directly on a message child's children list.
std::string mk_msg_get_str(const MkField& msg, const std::string& key);
int         mk_msg_get_int(const MkField& msg, const std::string& key, int def);
void        mk_msg_set_str(MkField& msg, const std::string& key, const std::string& value);
void        mk_msg_set_int(MkField& msg, const std::string& key, int value);
void        mk_msg_remove(MkField& msg, const std::string& key);

} // namespace mapedit
