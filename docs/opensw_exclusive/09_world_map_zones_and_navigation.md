# Swordigo OpenSwordigo Research: World Map Network, Zones & Navigation Guides

## 1. World Map Graph & Zone Definitions

The world of Swordigo is partitioned into interconnected `MapZone` regions containing `MapNode` locations (towns, waypoints, dungeons, boss arenas).

```cpp
namespace Caver {

enum class MapNodeType : uint32_t {
    Default  = 0,
    Town     = 1,
    Waypoint = 2,
    Boss     = 3
};

enum class PortalDirection : uint32_t {
    East      = 1,
    NorthEast = 2,
    North     = 3,
    NorthWest = 4,
    West      = 5,
    SouthWest = 6,
    South     = 7,
    SouthEast = 8
};

struct MapNode_Portal {
    std::string destination_name;     // Tag 0x0A
    PortalDirection direction;        // Tag 0x10
    PortalDirection pass_direction;   // Tag 0x18
    bool ignore_in_positioning = false;// Tag 0x20
};

struct MapNode {
    std::string level_name;           // Tag 0x12
    Vector2 map_position{ 0.0f, 0.0f };// Tag 0x0A
    MapNodeType type = MapNodeType::Default; // Tag 0x20
    bool hidden = false;              // Tag 0x28
    uint32_t experience_level = 1;    // Tag 0x30
    bool has_portal = false;          // Tag 0x40
    uint32_t num_treasures = 0;       // Tag 0x48
    std::string title;                // Tag 0x52
    std::string music_track;          // Tag 0x3A
    MapNode_Portal portal_connection; // Tag 0x1A
};

struct MapZone {
    std::string zone_id;              // Tag 0x0A ("Name")
    std::string title;                // Tag 0x12
    uint32_t experience_level = 1;    // Tag 0x20
    std::string default_music;        // Tag 0x2A
    std::vector<MapNode> nodes;       // Tag 0x1A
};

} // namespace Caver
```

---

## 2. Compass & Guide Target System (`GuideTarget`)

Compass direction arrows and quest marker indicators resolve active travel targets toward level destinations.

```cpp
namespace Caver {

enum class GuideTargetType : uint32_t {
    QuestGet = 1,
    Quest    = 2,
    Spell    = 3,
    Key      = 4
};

struct GuideTarget {
    std::string target_id;            // Tag 0x0C ("Name")
    GuideTargetType type;             // Tag 0x08
    std::string level_name;           // Tag 0x1A
    std::string object_identifier;    // Tag 0x22
    std::string carry_object_id;      // Tag 0x2A
    bool show_only_after_load = false;// Tag 0x30
};

} // namespace Caver
```
