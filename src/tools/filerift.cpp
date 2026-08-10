#include "tools/filerift.h"
#include "tools/scene_schemas.h"
#include "platform/protobuf_reader.h"
#include <sstream>
#include <iostream>
#include <map>
#include <vector>
#include <cmath>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

/*
 * FileRift C++ Port - Stage 1 - MrSinup
 * 
 * Missing Features (these are not needed for us now, i guess!? ):
 * - APK unpacking, modification, and rebuilding/signing (requires Java/jarsigner/zipalign integration).
 * - File manifests/checksum comparison to skip unmodified files (requires global database state).
 * - Detailed Lua compiler checks using host luac binary (uses direct extraction/in-memory compiler instead).
 * - CLI configuration profiles config.py support.
 */

extern "C" const char* sre_vfs_resolve_path(const char* filename, char* buf) {
    if (!filename || !buf) return filename;
    std::strcpy(buf, filename);
    return buf;
}

namespace filerift {

struct FieldSchema {
    uint32_t field_number;
    proto::WireType wire_type;
    std::string name;
    std::string classname;
};

static int lua_writer_cb(lua_State* L, const void* p, size_t sz, void* ud) {
    std::string* out = static_cast<std::string*>(ud);
    out->append(static_cast<const char*>(p), sz);
    return 0;
}

std::string compile_lua_to_bytecode(const std::string& source, const std::string& name = "script") {
    lua_State* L = luaL_newstate();
    if (!L) {
        std::cerr << "Failed to create Lua state for compilation" << std::endl;
        return "";
    }
    
    luaL_openlibs(L);
    
    int status = luaL_loadbuffer(L, source.c_str(), source.size(), name.c_str());
    if (status != 0) {
        std::string err = lua_tostring(L, -1);
        std::cerr << "Lua compile error: " << err << std::endl;
        lua_close(L);
        return "";
    }
    
    std::string bytecode;
    status = lua_dump(L, lua_writer_cb, &bytecode);
    lua_close(L);
    
    if (status != 0) {
        std::cerr << "Lua dump failed" << std::endl;
        return "";
    }
    
    return bytecode;
}

static std::map<std::string, std::vector<FieldSchema>> schemas;
static bool schemas_initialized = false;

static void init_schemas() {
    if (schemas_initialized) return;

    auto add = [](const std::string& msg, uint32_t f, proto::WireType w, const std::string& name, const std::string& cls) {
        schemas[msg].push_back({f, w, name, cls});
    };

    // --- "All" global fallbacks ---
    add("All", 1, proto::WIRE_LEN, "Object", "SceneObject");
    add("All", 2, proto::WIRE_LEN, "ObjectLibrary", "ObjectLibrary");
    add("All", 3, proto::WIRE_LEN, "Bounds", "Rectangle");
    add("All", 4, proto::WIRE_LEN, "Group", "SceneObjectGroup");
    add("All", 5, proto::WIRE_LEN, "OnLoad", "Program");
    add("All", 1, proto::WIRE_LEN, "Name", "");
    add("All", 2, proto::WIRE_LEN, "Template", "ObjectTemplate");
    add("All", 3, proto::WIRE_LEN, "ImportedLibrary", "");
    add("All", 4, proto::WIRE_LEN, "Texture", "Texture");
    add("All", 5, proto::WIRE_LEN, "Program", "Program");
    add("All", 1, proto::WIRE_LEN, "Item", "Item");
    add("All", 2, proto::WIRE_LEN, "Skill", "Skill");
    add("All", 3, proto::WIRE_LEN, "Quest", "Quest");
    add("All", 4, proto::WIRE_LEN, "EntityClass", "EntityClass");
    add("All", 5, proto::WIRE_LEN, "GuideTarget", "GuideTarget");
    add("All", 1, proto::WIRE_LEN, "Playlist", "MusicPlaylist");
    add("All", 2, proto::WIRE_VARINT, "MusicEnabled", "");
    add("All", 3, proto::WIRE_VARINT, "SoundEnabled", "");
    add("All", 4, proto::WIRE_I32, "MusicVolume", "");
    add("All", 5, proto::WIRE_I32, "SoundVolume", "");
    add("All", 6, proto::WIRE_LEN, "PhoneControlsLayout", "GUIViewLayout");
    add("All", 7, proto::WIRE_LEN, "PadControlsLayout", "GUIViewLayout");
    add("All", 8, proto::WIRE_VARINT, "GuideUnlocked", "");
    add("All", 9, proto::WIRE_VARINT, "CoinDoublerUnlocked", "");
    add("All", 10, proto::WIRE_VARINT, "NoAdsUnlocked", "");
    add("All", 2, proto::WIRE_VARINT, "ExperienceLevel", "");
    add("All", 3, proto::WIRE_I64, "TimePlayed", "");
    add("All", 4, proto::WIRE_LEN, "GameState", "GameState");
    add("All", 5, proto::WIRE_LEN, "EquippedWeaponName", "");
    add("All", 6, proto::WIRE_LEN, "EquippedArmorName", "");
    add("All", 7, proto::WIRE_LEN, "WeaponTrinketName", "");
    add("All", 8, proto::WIRE_LEN, "ArmorTrinketName", "");
    add("All", 9, proto::WIRE_LEN, "CurrentLevelTitle", "");
    add("All", 10, proto::WIRE_LEN, "LastPlayedTime", "DateTime");
    add("All", 11, proto::WIRE_I32, "PercentCompleted", "");
    add("All", 12, proto::WIRE_LEN, "Counter", "PlayerProfile_Counter");
    add("All", 13, proto::WIRE_VARINT, "CheatEnabled", "");
    add("All", 14, proto::WIRE_LEN, "Identifier", "");
    add("All", 1, proto::WIRE_LEN, "CharacterState", "CharacterState");
    add("All", 2, proto::WIRE_LEN, "LevelState", "LevelState");
    add("All", 3, proto::WIRE_LEN, "CurrentLevel", "");
    add("All", 4, proto::WIRE_LEN, "CurrentSpawnPoint", "");
    add("All", 5, proto::WIRE_LEN, "CurrentMapNodeName", "");
    add("All", 7, proto::WIRE_LEN, "QuestState", "QuestState");
    add("All", 8, proto::WIRE_LEN, "Properties", "StateProperties");
    add("All", 9, proto::WIRE_LEN, "SelectedMenuTab", "");
    add("All", 10, proto::WIRE_LEN, "CarriedObjectTemplate", "");
    add("All", 11, proto::WIRE_LEN, "CarriedObjectIdentifier", "");
    add("All", 12, proto::WIRE_LEN, "QuestText", "QuestText");
    add("All", 13, proto::WIRE_LEN, "PreviousPortalLevel", "");
    add("All", 14, proto::WIRE_VARINT, "MenuButtonFlashing", "");
    add("All", 15, proto::WIRE_VARINT, "SkillToggleButtonFlashing", "");
    add("All", 16, proto::WIRE_LEN, "FlashingItemName", "");
    add("All", 17, proto::WIRE_LEN, "FlashingSkillName", "");
    add("All", 18, proto::WIRE_VARINT, "GuideEnabled", "");
    add("All", 19, proto::WIRE_VARINT, "GuideToggled", "");
    add("All", 20, proto::WIRE_VARINT, "CoinDoublerEnabled", "");
    add("All", 21, proto::WIRE_VARINT, "CoinDoublerToggled", "");
    add("All", 2, proto::WIRE_LEN, "Zone", "MapZone");
    add("All", 1, proto::WIRE_LEN, "Effect", "SoundEffect");
    add("All", 3, proto::WIRE_LEN, "Glyph", "Font_Glyph");
    add("All", 4, proto::WIRE_LEN, "Kerning", "");
    add("All", 5, proto::WIRE_VARINT, "Height", "");
    add("All", 6, proto::WIRE_LEN, "BoundingBox", "Rectangle");
    add("All", 2, proto::WIRE_VARINT, "PixelFormat", "");
    add("All", 3, proto::WIRE_LEN, "Subtexture", "Texture_Subtexture");
    add("All", 4, proto::WIRE_VARINT, "ImageType", "");
    add("All", 5, proto::WIRE_LEN, "ConversionInfo", "Texture_ConversionInfo");

    // --- Message specific schemas ---
    add("Scene", 1, proto::WIRE_LEN, "Object", "SceneObject");
    add("Scene", 2, proto::WIRE_LEN, "ObjectLibrary", "ObjectLibrary");
    add("Scene", 3, proto::WIRE_LEN, "Bounds", "Rectangle");
    add("Scene", 4, proto::WIRE_LEN, "Group", "SceneObjectGroup");
    add("Scene", 5, proto::WIRE_LEN, "OnLoad", "Program");

    add("ObjectLibrary", 1, proto::WIRE_LEN, "Name", "");
    add("ObjectLibrary", 2, proto::WIRE_LEN, "Template", "ObjectTemplate");
    add("ObjectLibrary", 3, proto::WIRE_LEN, "ImportedLibrary", "");
    add("ObjectLibrary", 4, proto::WIRE_LEN, "Texture", "Texture");
    add("ObjectLibrary", 5, proto::WIRE_LEN, "Program", "Program");

    add("GameData", 1, proto::WIRE_LEN, "Item", "Item");
    add("GameData", 2, proto::WIRE_LEN, "Skill", "Skill");
    add("GameData", 3, proto::WIRE_LEN, "Quest", "Quest");
    add("GameData", 4, proto::WIRE_LEN, "EntityClass", "EntityClass");
    add("GameData", 5, proto::WIRE_LEN, "GuideTarget", "GuideTarget");

    add("GameOptions", 1, proto::WIRE_LEN, "Playlist", "MusicPlaylist");
    add("GameOptions", 2, proto::WIRE_VARINT, "MusicEnabled", "");
    add("GameOptions", 3, proto::WIRE_VARINT, "SoundEnabled", "");
    add("GameOptions", 4, proto::WIRE_I32, "MusicVolume", "");
    add("GameOptions", 5, proto::WIRE_I32, "SoundVolume", "");
    add("GameOptions", 6, proto::WIRE_LEN, "PhoneControlsLayout", "GUIViewLayout");
    add("GameOptions", 7, proto::WIRE_LEN, "PadControlsLayout", "GUIViewLayout");
    add("GameOptions", 8, proto::WIRE_VARINT, "GuideUnlocked", "");
    add("GameOptions", 9, proto::WIRE_VARINT, "CoinDoublerUnlocked", "");
    add("GameOptions", 10, proto::WIRE_VARINT, "NoAdsUnlocked", "");

    add("PlayerProfile", 1, proto::WIRE_LEN, "Name", "");
    add("PlayerProfile", 2, proto::WIRE_VARINT, "ExperienceLevel", "");
    add("PlayerProfile", 3, proto::WIRE_I64, "TimePlayed", "");
    add("PlayerProfile", 4, proto::WIRE_LEN, "GameState", "GameState");
    add("PlayerProfile", 5, proto::WIRE_LEN, "EquippedWeaponName", "");
    add("PlayerProfile", 6, proto::WIRE_LEN, "EquippedArmorName", "");
    add("PlayerProfile", 7, proto::WIRE_LEN, "WeaponTrinketName", "");
    add("PlayerProfile", 8, proto::WIRE_LEN, "ArmorTrinketName", "");
    add("PlayerProfile", 9, proto::WIRE_LEN, "CurrentLevelTitle", "");
    add("PlayerProfile", 10, proto::WIRE_LEN, "LastPlayedTime", "DateTime");
    add("PlayerProfile", 11, proto::WIRE_I32, "PercentCompleted", "");
    add("PlayerProfile", 12, proto::WIRE_LEN, "Counter", "PlayerProfile_Counter");
    add("PlayerProfile", 13, proto::WIRE_VARINT, "CheatEnabled", "");
    add("PlayerProfile", 14, proto::WIRE_LEN, "Identifier", "");

    add("GameState", 1, proto::WIRE_LEN, "CharacterState", "CharacterState");
    add("GameState", 2, proto::WIRE_LEN, "LevelState", "LevelState");
    add("GameState", 3, proto::WIRE_LEN, "CurrentLevel", "");
    add("GameState", 4, proto::WIRE_LEN, "CurrentSpawnPoint", "");
    add("GameState", 5, proto::WIRE_LEN, "CurrentMapNodeName", "");
    add("GameState", 7, proto::WIRE_LEN, "QuestState", "QuestState");
    add("GameState", 8, proto::WIRE_LEN, "Properties", "StateProperties");
    add("GameState", 9, proto::WIRE_LEN, "SelectedMenuTab", "");
    add("GameState", 10, proto::WIRE_LEN, "CarriedObjectTemplate", "");
    add("GameState", 11, proto::WIRE_LEN, "CarriedObjectIdentifier", "");
    add("GameState", 12, proto::WIRE_LEN, "QuestText", "QuestText");
    add("GameState", 13, proto::WIRE_LEN, "PreviousPortalLevel", "");
    add("GameState", 14, proto::WIRE_VARINT, "MenuButtonFlashing", "");
    add("GameState", 15, proto::WIRE_VARINT, "SkillToggleButtonFlashing", "");
    add("GameState", 16, proto::WIRE_LEN, "FlashingItemName", "");
    add("GameState", 17, proto::WIRE_LEN, "FlashingSkillName", "");
    add("GameState", 18, proto::WIRE_VARINT, "GuideEnabled", "");
    add("GameState", 19, proto::WIRE_VARINT, "GuideToggled", "");
    add("GameState", 20, proto::WIRE_VARINT, "CoinDoublerEnabled", "");
    add("GameState", 21, proto::WIRE_VARINT, "CoinDoublerToggled", "");

    add("Map", 2, proto::WIRE_LEN, "Zone", "MapZone");

    add("SoundLibrary", 1, proto::WIRE_LEN, "Effect", "SoundEffect");

    add("Font", 1, proto::WIRE_LEN, "Name", "");
    add("Font", 2, proto::WIRE_LEN, "Texture", "Texture");
    add("Font", 3, proto::WIRE_LEN, "Glyph", "Font_Glyph");
    add("Font", 4, proto::WIRE_LEN, "Kerning", "");
    add("Font", 5, proto::WIRE_VARINT, "Height", "");
    add("Font", 6, proto::WIRE_LEN, "BoundingBox", "Rectangle");

    add("Texture", 1, proto::WIRE_LEN, "Name", "");
    add("Texture", 2, proto::WIRE_VARINT, "PixelFormat", "");
    add("Texture", 3, proto::WIRE_LEN, "Subtexture", "Texture_Subtexture");
    add("Texture", 4, proto::WIRE_VARINT, "ImageType", "");
    add("Texture", 5, proto::WIRE_LEN, "ConversionInfo", "Texture_ConversionInfo");

    add("SceneObject", 1, proto::WIRE_LEN, "TemplateName", "");
    add("SceneObject", 2, proto::WIRE_LEN, "Identifier", "");
    add("SceneObject", 3, proto::WIRE_LEN, "Component", "Component");
    add("SceneObject", 4, proto::WIRE_LEN, "Position", "Vector2");
    add("SceneObject", 5, proto::WIRE_I32, "Depth", "");
    add("SceneObject", 6, proto::WIRE_I32, "Rotation", "");
    add("SceneObject", 7, proto::WIRE_I32, "Scaling", "");
    add("SceneObject", 8, proto::WIRE_LEN, "LocalAabb", "Rectangle");
    add("SceneObject", 9, proto::WIRE_VARINT, "Hidden", "");
    add("SceneObject", 10, proto::WIRE_LEN, "OnLoad", "Program");

    add("ObjectTemplate", 1, proto::WIRE_LEN, "Object", "SceneObject");
    add("ObjectTemplate", 2, proto::WIRE_I32, "Scaling", "");

    add("SceneObjectGroup", 1, proto::WIRE_LEN, "Identifier", "");
    add("SceneObjectGroup", 2, proto::WIRE_LEN, "ObjectIdentifier", "");
    add("SceneObjectGroup", 3, proto::WIRE_VARINT, "Hidden", "");
    add("SceneObjectGroup", 4, proto::WIRE_LEN, "OnLoad", "Program");
    add("SceneObjectGroup", 5, proto::WIRE_VARINT, "CanBecomeActive", "");
    add("SceneObjectGroup", 6, proto::WIRE_VARINT, "Locked", "");

    add("Item", 1, proto::WIRE_VARINT, "Type", "");
    add("Item", 2, proto::WIRE_LEN, "Name", "");
    add("Item", 3, proto::WIRE_LEN, "Title", "");
    add("Item", 4, proto::WIRE_LEN, "ShortDescription", "");
    add("Item", 5, proto::WIRE_LEN, "Description", "");
    add("Item", 6, proto::WIRE_VARINT, "Unique", "");
    add("Item", 7, proto::WIRE_VARINT, "MinDamage", "");
    add("Item", 8, proto::WIRE_VARINT, "MaxDamage", "");
    add("Item", 9, proto::WIRE_VARINT, "Level", "");

    add("Skill", 1, proto::WIRE_LEN, "Name", "");
    add("Skill", 2, proto::WIRE_LEN, "Title", "");
    add("Skill", 3, proto::WIRE_LEN, "Description", "");
    add("Skill", 4, proto::WIRE_VARINT, "ManaCost", "");
    add("Skill", 5, proto::WIRE_VARINT, "MinDamage", "");
    add("Skill", 6, proto::WIRE_VARINT, "MaxDamage", "");

    add("Quest", 1, proto::WIRE_LEN, "Name", "");
    add("Quest", 2, proto::WIRE_LEN, "Title", "");
    add("Quest", 3, proto::WIRE_LEN, "FollowUpQuest", "");
    add("Quest", 4, proto::WIRE_LEN, "MapLocation", "");

    add("EntityClass", 1, proto::WIRE_LEN, "Name", "");
    add("EntityClass", 2, proto::WIRE_LEN, "Title", "");
    add("EntityClass", 3, proto::WIRE_VARINT, "LevelHidden", "");
    add("EntityClass", 4, proto::WIRE_VARINT, "Freezable", "");
    add("EntityClass", 5, proto::WIRE_VARINT, "Stunnable", "");
    add("EntityClass", 6, proto::WIRE_VARINT, "Grabbable", "");
    add("EntityClass", 7, proto::WIRE_I32, "MagicResistance", "");
    add("EntityClass", 8, proto::WIRE_I32, "PhysicalResistance", "");

    add("GuideTarget", 1, proto::WIRE_VARINT, "Type", "");
    add("GuideTarget", 2, proto::WIRE_LEN, "Name", "");
    add("GuideTarget", 3, proto::WIRE_LEN, "LevelName", "");
    add("GuideTarget", 4, proto::WIRE_LEN, "ObjectIdentifier", "");
    add("GuideTarget", 5, proto::WIRE_LEN, "CarryObjectIdentifier", "");
    add("GuideTarget", 6, proto::WIRE_VARINT, "ShowOnlyAfterSceneLoad", "");
    add("GuideTarget", 7, proto::WIRE_LEN, "PortalHint", "GuideTarget_LevelObject");

    add("GuideTarget_LevelObject", 1, proto::WIRE_LEN, "LevelName", "");
    add("GuideTarget_LevelObject", 2, proto::WIRE_LEN, "ObjectIdentifier", "");

    add("MusicPlaylist", 1, proto::WIRE_LEN, "Name", "");
    add("MusicPlaylist", 2, proto::WIRE_LEN, "Track", "MusicTrack");

    add("MusicTrack", 1, proto::WIRE_LEN, "ResourceName", "");
    add("MusicTrack", 2, proto::WIRE_I32, "Volume", "");

    add("GUIViewLayout", 1, proto::WIRE_LEN, "Identifier", "");
    add("GUIViewLayout", 2, proto::WIRE_LEN, "Subview", "GUIViewLayout");
    add("GUIViewLayout", 3, proto::WIRE_LEN, "Margins", "GUIMargins");

    add("GUIMargins", 1, proto::WIRE_I32, "Left", "");
    add("GUIMargins", 2, proto::WIRE_I32, "Right", "");
    add("GUIMargins", 3, proto::WIRE_I32, "Bottom", "");
    add("GUIMargins", 4, proto::WIRE_I32, "Top", "");

    add("CharacterState", 2, proto::WIRE_VARINT, "CurrentHealth", "");
    add("CharacterState", 4, proto::WIRE_VARINT, "CurrentMana", "");
    add("CharacterState", 5, proto::WIRE_VARINT, "CurrentCoins", "");
    add("CharacterState", 6, proto::WIRE_VARINT, "ExperiencePoints", "");
    add("CharacterState", 7, proto::WIRE_VARINT, "ExperienceLevel", "");
    add("CharacterState", 11, proto::WIRE_LEN, "Item", "CharacterState_ItemState");
    add("CharacterState", 12, proto::WIRE_LEN, "EquippedWeapon", "");
    add("CharacterState", 13, proto::WIRE_LEN, "EquippedArmor", "");
    add("CharacterState", 15, proto::WIRE_LEN, "Skill", "");
    add("CharacterState", 16, proto::WIRE_LEN, "CurrentSkill", "");
    add("CharacterState", 17, proto::WIRE_LEN, "WeaponTrinket", "");
    add("CharacterState", 18, proto::WIRE_LEN, "ArmorTrinket", "");
    add("CharacterState", 19, proto::WIRE_LEN, "SkillTrinket", "");
    add("CharacterState", 20, proto::WIRE_VARINT, "HealthAttribute", "");
    add("CharacterState", 21, proto::WIRE_VARINT, "AttackAttribute", "");
    add("CharacterState", 22, proto::WIRE_VARINT, "MagicAttribute", "");

    add("CharacterState_ItemState", 1, proto::WIRE_LEN, "Name", "");
    add("CharacterState_ItemState", 2, proto::WIRE_VARINT, "Count", "");

    add("LevelState", 1, proto::WIRE_LEN, "LevelName", "");
    add("LevelState", 2, proto::WIRE_VARINT, "Visited", "");
    add("LevelState", 3, proto::WIRE_LEN, "Properties", "StateProperties");
    add("LevelState", 4, proto::WIRE_VARINT, "NumTreasures", "");
    add("LevelState", 5, proto::WIRE_VARINT, "TreasuresFound", "");

    add("StateProperties", 1, proto::WIRE_LEN, "Flag", "");

    add("QuestState", 1, proto::WIRE_LEN, "QuestName", "");
    add("QuestState", 2, proto::WIRE_VARINT, "Completed", "");

    add("QuestText", 1, proto::WIRE_LEN, "QuestName", "");
    add("QuestText", 2, proto::WIRE_LEN, "Line", "");

    add("PlayerProfile_Counter", 1, proto::WIRE_LEN, "Name", "");
    add("PlayerProfile_Counter", 2, proto::WIRE_VARINT, "Value", "");

    add("MapZone", 1, proto::WIRE_LEN, "Name", "");
    add("MapZone", 2, proto::WIRE_LEN, "Title", "");
    add("MapZone", 3, proto::WIRE_LEN, "Node", "MapNode");
    add("MapZone", 4, proto::WIRE_VARINT, "ExperienceLevel", "");
    add("MapZone", 5, proto::WIRE_LEN, "Music", "");

    add("MapNode", 1, proto::WIRE_LEN, "Position", "Vector2");
    add("MapNode", 2, proto::WIRE_LEN, "LevelName", "");
    add("MapNode", 3, proto::WIRE_LEN, "Portal", "MapNode_Portal");
    add("MapNode", 4, proto::WIRE_VARINT, "Type", "");
    add("MapNode", 5, proto::WIRE_VARINT, "Hidden", "");
    add("MapNode", 6, proto::WIRE_VARINT, "ExperienceLevel", "");
    add("MapNode", 7, proto::WIRE_LEN, "Music", "");
    add("MapNode", 8, proto::WIRE_VARINT, "HasPortal", "");
    add("MapNode", 9, proto::WIRE_VARINT, "NumTreasures", "");
    add("MapNode", 10, proto::WIRE_LEN, "Title", "");
    add("MapNode", 11, proto::WIRE_VARINT, "IgnoreInStatistics", "");

    add("MapNode_Portal", 1, proto::WIRE_LEN, "DestinationName", "");
    add("MapNode_Portal", 2, proto::WIRE_VARINT, "Direction", "");
    add("MapNode_Portal", 3, proto::WIRE_VARINT, "PassDirection", "");
    add("MapNode_Portal", 4, proto::WIRE_VARINT, "IgnoreInNodePositioning", "");

    add("SoundEffect", 1, proto::WIRE_LEN, "Name", "");
    add("SoundEffect", 2, proto::WIRE_LEN, "ResourceName", "");
    add("SoundEffect", 3, proto::WIRE_I32, "Volume", "");
    add("SoundEffect", 4, proto::WIRE_I32, "MinPlayInterval", "");

    add("Font_Glyph", 1, proto::WIRE_VARINT, "CharCode", "");
    add("Font_Glyph", 2, proto::WIRE_LEN, "DrawBounds", "Rectangle");
    add("Font_Glyph", 3, proto::WIRE_VARINT, "HorizAdvance", "");
    add("Font_Glyph", 4, proto::WIRE_LEN, "TextureBounds", "Rectangle");

    add("Texture_Subtexture", 1, proto::WIRE_LEN, "Name", "");
    add("Texture_Subtexture", 2, proto::WIRE_LEN, "Bounds", "Rectangle");
    add("Texture_Subtexture", 3, proto::WIRE_I32, "Resolution", "");

    add("Texture_ConversionInfo", 1, proto::WIRE_VARINT, "Width", "");
    add("Texture_ConversionInfo", 2, proto::WIRE_VARINT, "Height", "");
    add("Texture_ConversionInfo", 5, proto::WIRE_VARINT, "PixelFormat", "");
    add("Texture_ConversionInfo", 6, proto::WIRE_VARINT, "ImageType", "");

    add("Program", 1, proto::WIRE_LEN, "String", "");
    add("Program", 2, proto::WIRE_LEN, "Bytes", "");
    add("Program", 3, proto::WIRE_LEN, "Name", "");

    add("Vector2", 1, proto::WIRE_I32, "X", "");
    add("Vector2", 2, proto::WIRE_I32, "Y", "");

    add("Vector3", 1, proto::WIRE_I32, "X", "");
    add("Vector3", 2, proto::WIRE_I32, "Y", "");
    add("Vector3", 3, proto::WIRE_I32, "Z", "");

    add("Circle", 1, proto::WIRE_LEN, "Center", "Vector2");
    add("Circle", 2, proto::WIRE_I32, "Radius", "");

    add("Rectangle", 1, proto::WIRE_I32, "X", "");
    add("Rectangle", 2, proto::WIRE_I32, "Y", "");
    add("Rectangle", 3, proto::WIRE_I32, "Width", "");
    add("Rectangle", 4, proto::WIRE_I32, "Height", "");

    add("MeshData", 1, proto::WIRE_VARINT, "ValueType", "");
    add("MeshData", 2, proto::WIRE_VARINT, "ValuesPerVertex", "");
    add("MeshData", 3, proto::WIRE_VARINT, "Stride", "");
    add("MeshData", 4, proto::WIRE_VARINT, "DataOffset", "");

    add("Box", 1, proto::WIRE_I32, "X", "");
    add("Box", 2, proto::WIRE_I32, "Y", "");
    add("Box", 3, proto::WIRE_I32, "Z", "");
    add("Box", 4, proto::WIRE_I32, "Width", "");
    add("Box", 5, proto::WIRE_I32, "Height", "");
    add("Box", 6, proto::WIRE_I32, "Depth", "");

    add("Polygon", 1, proto::WIRE_LEN, "Vertex", "Vector2");
    add("Polygon", 2, proto::WIRE_VARINT, "Convex", "");
    add("Polygon", 3, proto::WIRE_VARINT, "Closed", "");

    add("FloatColor", 1, proto::WIRE_I32, "R", "");
    add("FloatColor", 2, proto::WIRE_I32, "G", "");
    add("FloatColor", 3, proto::WIRE_I32, "B", "");
    add("FloatColor", 4, proto::WIRE_I32, "A", "");

    add("Mesh", 1, proto::WIRE_VARINT, "NumVertices", "");
    add("Mesh", 2, proto::WIRE_VARINT, "NumFaces", "");
    add("Mesh", 3, proto::WIRE_LEN, "Indices", "MeshData");
    add("Mesh", 4, proto::WIRE_LEN, "Vertices", "MeshData");
    add("Mesh", 5, proto::WIRE_LEN, "Normals", "MeshData");
    add("Mesh", 6, proto::WIRE_LEN, "TexCoordSet", "MeshData");
    add("Mesh", 7, proto::WIRE_LEN, "VertexColors", "MeshData");
    add("Mesh", 8, proto::WIRE_LEN, "BoneIndices", "MeshData");
    add("Mesh", 9, proto::WIRE_LEN, "BoneWeights", "MeshData");
    add("Mesh", 10, proto::WIRE_LEN, "Material", "MeshMaterial");
    add("Mesh", 11, proto::WIRE_LEN, "BoundingBox", "Box");
    add("Mesh", 50, proto::WIRE_LEN, "VertexData", "");
    add("Mesh", 51, proto::WIRE_LEN, "IndexData", "");

    add("MeshMaterial", 1, proto::WIRE_LEN, "AmbientColor", "FloatColor");
    add("MeshMaterial", 2, proto::WIRE_LEN, "DiffuseColor", "FloatColor");
    add("MeshMaterial", 3, proto::WIRE_LEN, "SpecularColor", "FloatColor");
    add("MeshMaterial", 4, proto::WIRE_I32, "Shininess", "");
    add("MeshMaterial", 5, proto::WIRE_LEN, "Texture", "Texture");

    add("DateTime", 1, proto::WIRE_I64, "SecondsSinceReferenceDate", "");

    add("Component", 1, proto::WIRE_LEN, "ClassName", "");
    add("Component", 2, proto::WIRE_VARINT, "Identifier", "");
    add("Component", 3, proto::WIRE_LEN, "Label", "");
    add("Component", 4, proto::WIRE_VARINT, "ParentComponentIdentifier", "");
    add("Component", 100, proto::WIRE_LEN, "SpriteComponent", "SpriteComponent");
    add("Component", 101, proto::WIRE_LEN, "ModelComponent", "ModelComponent");
    add("Component", 102, proto::WIRE_LEN, "KeyframeAnimationComponent", "KeyframeAnimationComponent");
    add("Component", 103, proto::WIRE_LEN, "BlendAnimationComponent", "BlendAnimationComponent");
    add("Component", 104, proto::WIRE_LEN, "ModelTransformControllerComponent", "ModelTransformControllerComponent");
    add("Component", 110, proto::WIRE_LEN, "GroundPolygonComponent", "GroundPolygonComponent");
    add("Component", 111, proto::WIRE_LEN, "GroundMeshComponent", "GroundMeshComponent");
    add("Component", 112, proto::WIRE_LEN, "GroundMeshGeneratorComponent", "GroundMeshGeneratorComponent");
    add("Component", 113, proto::WIRE_LEN, "TextureMappingComponent", "TextureMappingComponent");
    add("Component", 114, proto::WIRE_LEN, "WaterMeshComponent", "WaterMeshComponent");
    add("Component", 120, proto::WIRE_LEN, "ShapeComponent", "ShapeComponent");
    add("Component", 121, proto::WIRE_LEN, "CollisionShapeComponent", "CollisionShapeComponent");
    add("Component", 122, proto::WIRE_LEN, "DamageComponent", "DamageComponent");
    add("Component", 123, proto::WIRE_LEN, "HealthComponent", "HealthComponent");
    add("Component", 124, proto::WIRE_LEN, "BoneControlledCollisionShapeComponent", "BoneControlledCollisionShapeComponent");
    add("Component", 125, proto::WIRE_LEN, "ObjectLinkControllerComponent", "ObjectLinkControllerComponent");
    add("Component", 130, proto::WIRE_LEN, "LightComponent", "LightComponent");
    add("Component", 131, proto::WIRE_LEN, "ShadowComponent", "ShadowComponent");
    add("Component", 140, proto::WIRE_LEN, "SoundEffectComponent", "SoundEffectComponent");
    add("Component", 149, proto::WIRE_LEN, "AnimationControllerComponent", "AnimationControllerComponent");
    add("Component", 150, proto::WIRE_LEN, "CharAnimControllerComponent", "CharAnimControllerComponent");
    add("Component", 151, proto::WIRE_LEN, "CharControllerComponent", "CharControllerComponent");
    add("Component", 152, proto::WIRE_LEN, "EntityComponent", "EntityComponent");
    add("Component", 153, proto::WIRE_LEN, "BushControllerComponent", "BushControllerComponent");
    add("Component", 154, proto::WIRE_LEN, "ElevatorControllerComponent", "ElevatorControllerComponent");
    add("Component", 155, proto::WIRE_LEN, "PressureTriggerComponent", "PressureTriggerComponent");
    add("Component", 156, proto::WIRE_LEN, "DoorControllerComponent", "DoorControllerComponent");
    add("Component", 157, proto::WIRE_LEN, "ProgramComponent", "ProgramComponent");
    add("Component", 158, proto::WIRE_LEN, "MonsterEntityComponent", "MonsterEntityComponent");
    add("Component", 159, proto::WIRE_LEN, "PhysicsObjectComponent", "PhysicsObjectComponent");
    add("Component", 160, proto::WIRE_LEN, "BreakableObjectComponent", "BreakableObjectComponent");
    add("Component", 161, proto::WIRE_LEN, "EntityControllerComponent", "EntityControllerComponent");
    add("Component", 162, proto::WIRE_LEN, "EntityActionComponent", "EntityActionComponent");
    add("Component", 163, proto::WIRE_LEN, "PhysicsPlatformComponent", "PhysicsPlatformComponent");
    add("Component", 164, proto::WIRE_LEN, "EntityInfoComponent", "EntityInfoComponent");
    add("Component", 165, proto::WIRE_LEN, "HeroEntityComponent", "HeroEntityComponent");
    add("Component", 200, proto::WIRE_LEN, "BackgroundComponent", "BackgroundComponent");
    add("Component", 210, proto::WIRE_LEN, "PropertiesComponent", "PropertiesComponent");
    add("Component", 250, proto::WIRE_LEN, "ParticleEmitterComponent", "ParticleEmitterComponent");
    add("Component", 251, proto::WIRE_LEN, "ParticleComponent", "ParticleComponent");
    add("Component", 253, proto::WIRE_LEN, "FireEmitterComponent", "FireEmitterComponent");
    add("Component", 254, proto::WIRE_LEN, "SimpleGlowComponent", "SimpleGlowComponent");
    add("Component", 255, proto::WIRE_LEN, "ParticleObjectComponent", "ParticleObjectComponent");
    add("Component", 256, proto::WIRE_LEN, "OrbitControllerComponent", "OrbitControllerComponent");
    add("Component", 302, proto::WIRE_LEN, "MonsterControllerComponent", "MonsterControllerComponent");
    add("Component", 303, proto::WIRE_LEN, "WalkingMonsterControllerComponent", "WalkingMonsterControllerComponent");
    add("Component", 304, proto::WIRE_LEN, "ChargingMonsterControllerComponent", "ChargingMonsterControllerComponent");
    add("Component", 305, proto::WIRE_LEN, "SnappingMonsterControllerComponent", "SnappingMonsterControllerComponent");
    add("Component", 306, proto::WIRE_LEN, "AttackComponent", "AttackComponent");
    add("Component", 307, proto::WIRE_LEN, "LeapingMonsterControllerComponent", "LeapingMonsterControllerComponent");
    add("Component", 308, proto::WIRE_LEN, "SkellyMonsterControllerComponent", "SkellyMonsterControllerComponent");
    add("Component", 309, proto::WIRE_LEN, "StaticMonsterControllerComponent", "StaticMonsterControllerComponent");
    add("Component", 310, proto::WIRE_LEN, "ShootingMonsterControllerComponent", "ShootingMonsterControllerComponent");
    add("Component", 311, proto::WIRE_LEN, "BatMonsterControllerComponent", "BatMonsterControllerComponent");
    add("Component", 312, proto::WIRE_LEN, "BouncingMonsterControllerComponent", "BouncingMonsterControllerComponent");
    add("Component", 313, proto::WIRE_LEN, "MonsterDeathControllerComponent", "MonsterDeathControllerComponent");
    add("Component", 314, proto::WIRE_LEN, "GenericMonsterControllerComponent", "GenericMonsterControllerComponent");
    add("Component", 400, proto::WIRE_LEN, "SwingableWeaponComponent", "SwingableWeaponComponent");
    add("Component", 401, proto::WIRE_LEN, "SwingableWeaponControllerComponent", "SwingableWeaponControllerComponent");
    add("Component", 402, proto::WIRE_LEN, "SwingComponent", "SwingComponent");
    add("Component", 403, proto::WIRE_LEN, "WeaponGlowComponent", "WeaponGlowComponent");
    add("Component", 404, proto::WIRE_LEN, "WeaponTrailComponent", "WeaponTrailComponent");
    add("Component", 500, proto::WIRE_LEN, "PortalComponent", "PortalComponent");
    add("Component", 501, proto::WIRE_LEN, "SpawnPointComponent", "SpawnPointComponent");
    add("Component", 502, proto::WIRE_LEN, "CollectableItemComponent", "CollectableItemComponent");
    add("Component", 504, proto::WIRE_LEN, "TouchableComponent", "TouchableComponent");
    add("Component", 505, proto::WIRE_LEN, "ItemDropComponent", "ItemDropComponent");
    add("Component", 506, proto::WIRE_LEN, "OverlayTextComponent", "OverlayTextComponent");
    add("Component", 507, proto::WIRE_LEN, "PortalEffectComponent", "PortalEffectComponent");
    add("Component", 550, proto::WIRE_LEN, "MagicBoltComponent", "MagicBoltComponent");
    add("Component", 551, proto::WIRE_LEN, "MagicExplosionComponent", "MagicExplosionComponent");
    add("Component", 552, proto::WIRE_LEN, "SkillComponent", "SkillComponent");
    add("Component", 553, proto::WIRE_LEN, "MagicSpellCastComponent", "MagicSpellCastComponent");
    add("Component", 554, proto::WIRE_LEN, "FireBreathComponent", "FireBreathComponent");
    add("Component", 555, proto::WIRE_LEN, "ProjectileControllerComponent", "ProjectileControllerComponent");
    add("Component", 556, proto::WIRE_LEN, "MagicBombComponent", "MagicBombComponent");
    add("Component", 557, proto::WIRE_LEN, "MagicHookshotComponent", "MagicHookshotComponent");
    add("Component", 558, proto::WIRE_LEN, "SpellComponent", "SpellComponent");

    add("SpriteComponent", 1, proto::WIRE_LEN, "TextureName", "");

    add("ModelComponent", 1, proto::WIRE_LEN, "Name", "");
    add("ModelComponent", 2, proto::WIRE_I32, "YRotation", "");
    add("ModelComponent", 3, proto::WIRE_I32, "EmissionFactor", "");
    add("ModelComponent", 4, proto::WIRE_I32, "XRotation", "");
    add("ModelComponent", 5, proto::WIRE_LEN, "ShatterColor", "FloatColor");
    add("ModelComponent", 6, proto::WIRE_LEN, "Origin", "Vector3");
    add("ModelComponent", 7, proto::WIRE_VARINT, "Transparent", "");
    add("ModelComponent", 8, proto::WIRE_LEN, "DiffuseColor", "FloatColor");

    add("KeyframeAnimationComponent", 1, proto::WIRE_VARINT, "ModelId", "");
    add("KeyframeAnimationComponent", 2, proto::WIRE_LEN, "Name", "");
    add("KeyframeAnimationComponent", 3, proto::WIRE_VARINT, "Repeating", "");
    add("KeyframeAnimationComponent", 4, proto::WIRE_I32, "SpeedMultiplier", "");
    add("KeyframeAnimationComponent", 5, proto::WIRE_VARINT, "Running", "");

    add("BlendAnimationComponent", 1, proto::WIRE_VARINT, "Animation1Id", "");
    add("BlendAnimationComponent", 2, proto::WIRE_VARINT, "Animation2Id", "");
    add("BlendAnimationComponent", 3, proto::WIRE_I32, "BlendTime", "");
    add("BlendAnimationComponent", 4, proto::WIRE_I32, "ReverseBlendTime", "");

    add("ModelTransformControllerComponent", 1, proto::WIRE_VARINT, "ModelId", "");
    add("ModelTransformControllerComponent", 2, proto::WIRE_LEN, "Origin", "Vector3");
    add("ModelTransformControllerComponent", 3, proto::WIRE_LEN, "RotationAxis", "Vector3");
    add("ModelTransformControllerComponent", 4, proto::WIRE_I32, "RotationAngle", "");
    add("ModelTransformControllerComponent", 5, proto::WIRE_I32, "RotationSpeed", "");

    add("GroundPolygonComponent", 1, proto::WIRE_LEN, "Vertex", "Vector2");
    add("GroundPolygonComponent", 2, proto::WIRE_LEN, "Polygon", "Polygon");
    add("GroundPolygonComponent", 3, proto::WIRE_VARINT, "Collides", "");
    add("GroundPolygonComponent", 4, proto::WIRE_I32, "MinDepth", "");
    add("GroundPolygonComponent", 5, proto::WIRE_I32, "MaxDepth", "");
    add("GroundPolygonComponent", 6, proto::WIRE_LEN, "OnCollide", "Program");
    add("GroundPolygonComponent", 7, proto::WIRE_I32, "Friction", "");
    add("GroundPolygonComponent", 8, proto::WIRE_VARINT, "UnsafeGround", "");

    add("GroundMeshComponent", 1, proto::WIRE_LEN, "VertexData", "");
    add("GroundMeshComponent", 2, proto::WIRE_LEN, "Indices", "");
    add("GroundMeshComponent", 3, proto::WIRE_LEN, "Mesh", "Mesh");
    add("GroundMeshComponent", 4, proto::WIRE_LEN, "LocalAabb", "Rectangle");
    add("GroundMeshComponent", 5, proto::WIRE_LEN, "SurfaceMesh", "Mesh");
    add("GroundMeshComponent", 6, proto::WIRE_LEN, "FrontMesh", "Mesh");
    add("GroundMeshComponent", 7, proto::WIRE_LEN, "Color", "FloatColor");
    add("GroundMeshComponent", 8, proto::WIRE_VARINT, "Transparent", "");

    add("GroundMeshGeneratorComponent", 1, proto::WIRE_VARINT, "GroundPolygonId", "");
    add("GroundMeshGeneratorComponent", 2, proto::WIRE_VARINT, "TargetMeshId", "");
    add("GroundMeshGeneratorComponent", 3, proto::WIRE_VARINT, "FrontTextureMappingId", "");
    add("GroundMeshGeneratorComponent", 4, proto::WIRE_VARINT, "SurfaceTextureMappingId", "");
    add("GroundMeshGeneratorComponent", 5, proto::WIRE_VARINT, "RandomSeed", "");
    add("GroundMeshGeneratorComponent", 6, proto::WIRE_I32, "HorizNoise", "");
    add("GroundMeshGeneratorComponent", 7, proto::WIRE_VARINT, "MeshType", "");
    add("GroundMeshGeneratorComponent", 8, proto::WIRE_I32, "SurfaceWidth", "");
    add("GroundMeshGeneratorComponent", 9, proto::WIRE_I32, "HatHeight", "");
    add("GroundMeshGeneratorComponent", 10, proto::WIRE_I32, "HatWidthOffset1", "");
    add("GroundMeshGeneratorComponent", 11, proto::WIRE_I32, "HatWidthOffset2", "");

    add("TextureMappingComponent", 1, proto::WIRE_LEN, "TextureName", "");
    add("TextureMappingComponent", 2, proto::WIRE_I32, "Scale", "");
    add("TextureMappingComponent", 3, proto::WIRE_LEN, "Offset", "Vector2");

    add("WaterMeshComponent", 1, proto::WIRE_VARINT, "BoundsShapeId", "");
    add("WaterMeshComponent", 2, proto::WIRE_VARINT, "TextureMappingId", "");
    add("WaterMeshComponent", 3, proto::WIRE_LEN, "FrontColor", "FloatColor");
    add("WaterMeshComponent", 4, proto::WIRE_LEN, "SurfaceColor", "FloatColor");

    add("ShapeComponent", 1, proto::WIRE_LEN, "Rectangle", "Rectangle");
    add("ShapeComponent", 2, proto::WIRE_LEN, "Circle", "Circle");
    add("ShapeComponent", 3, proto::WIRE_LEN, "Polygon", "Polygon");

    add("CollisionShapeComponent", 2, proto::WIRE_VARINT, "IsGround", "");
    add("CollisionShapeComponent", 3, proto::WIRE_VARINT, "Collides", "");
    add("CollisionShapeComponent", 4, proto::WIRE_VARINT, "ReceivesDamage", "");
    add("CollisionShapeComponent", 5, proto::WIRE_VARINT, "InflictsDamage", "");
    add("CollisionShapeComponent", 6, proto::WIRE_I32, "MinDepth", "");
    add("CollisionShapeComponent", 7, proto::WIRE_I32, "MaxDepth", "");
    add("CollisionShapeComponent", 8, proto::WIRE_VARINT, "SpecialType", "");
    add("CollisionShapeComponent", 9, proto::WIRE_LEN, "OnCollide", "Program");
    add("CollisionShapeComponent", 10, proto::WIRE_LEN, "OnCollisionEnd", "Program");
    add("CollisionShapeComponent", 11, proto::WIRE_VARINT, "Enabled", "");
    add("CollisionShapeComponent", 12, proto::WIRE_LEN, "OnReceiveDamage", "Program");
    add("CollisionShapeComponent", 13, proto::WIRE_I32, "Friction", "");
    add("CollisionShapeComponent", 14, proto::WIRE_VARINT, "UnsafeGround", "");

    add("DamageComponent", 1, proto::WIRE_VARINT, "MinDamage", "");
    add("DamageComponent", 2, proto::WIRE_VARINT, "DamageType", "");
    add("DamageComponent", 3, proto::WIRE_VARINT, "SpecialDamageType", "");
    add("DamageComponent", 4, proto::WIRE_VARINT, "StandAlone", "");
    add("DamageComponent", 5, proto::WIRE_VARINT, "MaxDamage", "");
    add("DamageComponent", 6, proto::WIRE_I32, "PhysicalDamageFactor", "");
    add("DamageComponent", 7, proto::WIRE_I32, "MagicDamageFactor", "");
    add("DamageComponent", 8, proto::WIRE_VARINT, "IgnoreTargetImmunity", "");
    add("DamageComponent", 9, proto::WIRE_VARINT, "CanBeBlocked", "");

    add("HealthComponent", 1, proto::WIRE_VARINT, "MaxHealth", "");
    add("HealthComponent", 2, proto::WIRE_VARINT, "HEALTH_TYPE", "");
    add("HealthComponent", 3, proto::WIRE_LEN, "BarOffset", "Vector3");

    add("BoneControlledCollisionShapeComponent", 1, proto::WIRE_VARINT, "ControllingModelId", "");
    add("BoneControlledCollisionShapeComponent", 2, proto::WIRE_LEN, "ControllingBoneName", "");

    add("ObjectLinkControllerComponent", 1, proto::WIRE_LEN, "TargetObjectIdentifier", "");
    add("ObjectLinkControllerComponent", 2, proto::WIRE_LEN, "TargetBoneIdentifier", "");
    add("ObjectLinkControllerComponent", 3, proto::WIRE_LEN, "LocalOffset", "Vector3");
    add("ObjectLinkControllerComponent", 4, proto::WIRE_LEN, "WorldOffset", "Vector3");
    add("ObjectLinkControllerComponent", 5, proto::WIRE_LEN, "LocalRotation", "Vector3");

    add("LightComponent", 1, proto::WIRE_VARINT, "Type", "");
    add("LightComponent", 2, proto::WIRE_I32, "Intensity", "");
    add("LightComponent", 3, proto::WIRE_LEN, "Color", "FloatColor");
    add("LightComponent", 4, proto::WIRE_I32, "LinearAttenuation", "");
    add("LightComponent", 5, proto::WIRE_I32, "QuadraticAttenuation", "");
    add("LightComponent", 6, proto::WIRE_LEN, "Offset", "Vector3");
    add("LightComponent", 7, proto::WIRE_I32, "Radius", "");

    add("ShadowComponent", 1, proto::WIRE_I32, "WidthRadius", "");
    add("ShadowComponent", 2, proto::WIRE_I32, "DepthRadius", "");
    add("ShadowComponent", 3, proto::WIRE_LEN, "Offset", "Vector3");

    add("SoundEffectComponent", 1, proto::WIRE_LEN, "Name", "");
    add("SoundEffectComponent", 2, proto::WIRE_I32, "Delay", "");
    add("SoundEffectComponent", 3, proto::WIRE_I32, "Volume", "");

    add("AnimationControllerComponent", 1, proto::WIRE_VARINT, "ModelId", "");
    add("AnimationControllerComponent", 2, proto::WIRE_VARINT, "DefaultAnimationId", "");
    add("AnimationControllerComponent", 3, proto::WIRE_VARINT, "SelfUpdate", "");

    add("CharAnimControllerComponent", 4, proto::WIRE_VARINT, "StandAnimationId", "");
    add("CharAnimControllerComponent", 5, proto::WIRE_VARINT, "WalkAnimationId", "");
    add("CharAnimControllerComponent", 6, proto::WIRE_VARINT, "JumpAnimationId", "");
    add("CharAnimControllerComponent", 7, proto::WIRE_VARINT, "FallAnimationId", "");
    add("CharAnimControllerComponent", 8, proto::WIRE_VARINT, "CastAnimationId", "");
    add("CharAnimControllerComponent", 9, proto::WIRE_VARINT, "AirJumpAnimationId", "");

    add("CharControllerComponent", 1, proto::WIRE_VARINT, "DefaultAnimationControllerId", "");
    add("CharControllerComponent", 2, proto::WIRE_VARINT, "RightWeaponControllerId", "");
    add("CharControllerComponent", 3, proto::WIRE_I32, "NormalRunSpeed", "");
    add("CharControllerComponent", 4, proto::WIRE_I32, "JumpSpeed", "");
    add("CharControllerComponent", 5, proto::WIRE_I32, "NormalMaxJumpTime", "");
    add("CharControllerComponent", 6, proto::WIRE_VARINT, "LeftWeaponControllerId", "");
    add("CharControllerComponent", 7, proto::WIRE_VARINT, "EntityId", "");
    add("CharControllerComponent", 8, proto::WIRE_VARINT, "SwingComponentId", "");
    add("CharControllerComponent", 9, proto::WIRE_VARINT, "LiftAnimationControllerId", "");
    add("CharControllerComponent", 10, proto::WIRE_VARINT, "LiftAnimationId", "");
    add("CharControllerComponent", 11, proto::WIRE_VARINT, "DropAnimationId", "");
    add("CharControllerComponent", 12, proto::WIRE_VARINT, "ThrowAnimationId", "");
    add("CharControllerComponent", 13, proto::WIRE_VARINT, "HurtAnimationId", "");
    add("CharControllerComponent", 14, proto::WIRE_VARINT, "DieAnimationId", "");
    add("CharControllerComponent", 15, proto::WIRE_VARINT, "PushAnimationId", "");
    add("CharControllerComponent", 16, proto::WIRE_I32, "FastRunSpeed", "");
    add("CharControllerComponent", 17, proto::WIRE_I32, "FastMaxJumpTime", "");
    add("CharControllerComponent", 18, proto::WIRE_VARINT, "JumpSoundId", "");
    add("CharControllerComponent", 19, proto::WIRE_VARINT, "AirJumpSoundId", "");
    add("CharControllerComponent", 20, proto::WIRE_VARINT, "JumpLandSoundId", "");

    add("EntityComponent", 1, proto::WIRE_VARINT, "FacingDirection", "");
    add("EntityComponent", 2, proto::WIRE_VARINT, "PhysicsEnabled", "");

    add("BushControllerComponent", 1, proto::WIRE_VARINT, "WobbleAnimationId", "");
    add("BushControllerComponent", 2, proto::WIRE_VARINT, "WobbleSoundId", "");
    add("BushControllerComponent", 3, proto::WIRE_VARINT, "CutSoundId", "");

    add("ElevatorControllerComponent", 1, proto::WIRE_VARINT, "ElevationShapeId", "");
    add("ElevatorControllerComponent", 2, proto::WIRE_VARINT, "Mode", "");

    add("PressureTriggerComponent", 1, proto::WIRE_I32, "MaxHeightOffset", "");
    add("PressureTriggerComponent", 2, proto::WIRE_LEN, "OnPress", "Program");
    add("PressureTriggerComponent", 3, proto::WIRE_LEN, "OnRelease", "Program");
    add("PressureTriggerComponent", 4, proto::WIRE_VARINT, "StayPressed", "");

    add("DoorControllerComponent", 1, proto::WIRE_VARINT, "AnimationControllerId", "");
    add("DoorControllerComponent", 2, proto::WIRE_VARINT, "AnimationId", "");
    add("DoorControllerComponent", 4, proto::WIRE_VARINT, "Open", "");
    add("DoorControllerComponent", 5, proto::WIRE_VARINT, "CloseSoundId", "");
    add("DoorControllerComponent", 6, proto::WIRE_VARINT, "OpenSoundId", "");

    add("ProgramComponent", 1, proto::WIRE_VARINT, "ExecuteOnce", "");
    add("ProgramComponent", 2, proto::WIRE_LEN, "Program", "Program");
    add("ProgramComponent", 3, proto::WIRE_VARINT, "Enabled", "");
    add("ProgramComponent", 4, proto::WIRE_VARINT, "Trigger", "");

    add("MonsterEntityComponent", 1, proto::WIRE_LEN, "OnKill", "Program");
    add("MonsterEntityComponent", 2, proto::WIRE_LEN, "OnHurt", "Program");
    add("MonsterEntityComponent", 3, proto::WIRE_VARINT, "GivesExperience", "");
    add("MonsterEntityComponent", 4, proto::WIRE_VARINT, "DefaultDeathAnimation", "");

    add("PhysicsObjectComponent", 1, proto::WIRE_VARINT, "PhysicsEnabled", "");
    add("PhysicsObjectComponent", 2, proto::WIRE_LEN, "GravityDirection", "Vector2");
    add("PhysicsObjectComponent", 3, proto::WIRE_I32, "GravityMagnitude", "");
    add("PhysicsObjectComponent", 4, proto::WIRE_I32, "GroundDeceleration", "");
    add("PhysicsObjectComponent", 5, proto::WIRE_I32, "AirDeceleration", "");
    add("PhysicsObjectComponent", 6, proto::WIRE_I32, "MaxSpeed", "");
    add("PhysicsObjectComponent", 7, proto::WIRE_VARINT, "AllowRotation", "");
    add("PhysicsObjectComponent", 8, proto::WIRE_I32, "Elasticity", "");

    add("BreakableObjectComponent", 1, proto::WIRE_VARINT, "BreaksOnImpact", "");
    add("BreakableObjectComponent", 2, proto::WIRE_VARINT, "NumHitsToBreak", "");
    add("BreakableObjectComponent", 3, proto::WIRE_VARINT, "RequiredDamageType", "");
    add("BreakableObjectComponent", 4, proto::WIRE_LEN, "OnBreak", "Program");

    add("EntityControllerComponent", 1, proto::WIRE_VARINT, "EntityId", "");
    add("EntityControllerComponent", 2, proto::WIRE_VARINT, "AnimationControllerId", "");
    add("EntityControllerComponent", 3, proto::WIRE_VARINT, "DefaultMoveAnimationId", "");
    add("EntityControllerComponent", 4, proto::WIRE_VARINT, "RoamAreaId", "");
    add("EntityControllerComponent", 5, proto::WIRE_I32, "DefaultMoveSpeed", "");
    add("EntityControllerComponent", 6, proto::WIRE_I32, "DefaultAcceleration", "");
    add("EntityControllerComponent", 7, proto::WIRE_I32, "TargetingDistance", "");
    add("EntityControllerComponent", 8, proto::WIRE_VARINT, "MovementBehavior", "");

    add("EntityActionComponent", 1, proto::WIRE_LEN, "OnActivate", "Program");

    add("PhysicsPlatformComponent", 1, proto::WIRE_I32, "Mass", "");
    add("PhysicsPlatformComponent", 2, proto::WIRE_I32, "SpringForce", "");
    add("PhysicsPlatformComponent", 3, proto::WIRE_I32, "DecelerationForce", "");
    add("PhysicsPlatformComponent", 4, proto::WIRE_I32, "MinSpeed", "");

    add("EntityInfoComponent", 1, proto::WIRE_LEN, "EntityClass", "");

    add("HeroEntityComponent", 1, proto::WIRE_LEN, "OnItemGet", "Program");

    add("BackgroundComponent", 1, proto::WIRE_LEN, "TextureName", "");

    add("PropertiesComponent", 1, proto::WIRE_LEN, "OnLoad", "Program");

    add("ParticleEmitter", 1, proto::WIRE_VARINT, "Type", "");
    add("ParticleEmitter", 2, proto::WIRE_LEN, "BaseColor", "FloatColor");
    add("ParticleEmitter", 3, proto::WIRE_I32, "Parameter", "");
    add("ParticleEmitter", 4, proto::WIRE_I32, "HueVariance", "");
    add("ParticleEmitter", 5, proto::WIRE_I32, "SaturationVariance", "");
    add("ParticleEmitter", 6, proto::WIRE_I32, "LightnessVariance", "");
    add("ParticleEmitter", 7, proto::WIRE_LEN, "OriginOffset", "Vector3");

    add("ParticleEmitterComponent", 2, proto::WIRE_VARINT, "ParticleId", "");
    add("ParticleEmitterComponent", 3, proto::WIRE_VARINT, "ModelBindingId", "");
    add("ParticleEmitterComponent", 4, proto::WIRE_VARINT, "MaxParticles", "");
    add("ParticleEmitterComponent", 5, proto::WIRE_VARINT, "ParentEmitterId", "");
    add("ParticleEmitterComponent", 6, proto::WIRE_VARINT, "DestroyWhenFinished", "");
    add("ParticleEmitterComponent", 7, proto::WIRE_LEN, "Emitter", "ParticleEmitter");
    add("ParticleEmitterComponent", 8, proto::WIRE_VARINT, "LocalSystem", "");
    add("ParticleEmitterComponent", 9, proto::WIRE_LEN, "Gravity", "Vector3");
    add("ParticleEmitterComponent", 10, proto::WIRE_LEN, "Rotation", "Vector3");

    add("ParticleComponent", 1, proto::WIRE_LEN, "TextureName", "");
    add("ParticleComponent", 2, proto::WIRE_I32, "Size", "");

    add("FireEmitterComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("FireEmitterComponent", 2, proto::WIRE_LEN, "Origin", "Vector3");
    add("FireEmitterComponent", 3, proto::WIRE_VARINT, "LightId", "");
    add("FireEmitterComponent", 5, proto::WIRE_LEN, "Color", "FloatColor");
    add("FireEmitterComponent", 6, proto::WIRE_I32, "ParticleInterval", "");
    add("FireEmitterComponent", 7, proto::WIRE_I32, "ParticleMaxAge", "");
    add("FireEmitterComponent", 8, proto::WIRE_LEN, "ParticleSpread", "Vector3");
    add("FireEmitterComponent", 9, proto::WIRE_LEN, "Origin3", "Vector3");

    add("SimpleGlowComponent", 1, proto::WIRE_LEN, "Color", "FloatColor");
    add("SimpleGlowComponent", 2, proto::WIRE_I32, "Size", "");
    add("SimpleGlowComponent", 3, proto::WIRE_VARINT, "NumSegments", "");
    add("SimpleGlowComponent", 4, proto::WIRE_I32, "Depth", "");
    add("SimpleGlowComponent", 5, proto::WIRE_I32, "PulseAmount", "");
    add("SimpleGlowComponent", 6, proto::WIRE_I32, "PulseTime", "");
    add("SimpleGlowComponent", 7, proto::WIRE_LEN, "Offset", "Vector2");

    add("ParticleObjectComponent", 1, proto::WIRE_VARINT, "ModelId", "");

    add("OrbitControllerComponent", 1, proto::WIRE_LEN, "RotationAxis", "Vector3");
    add("OrbitControllerComponent", 2, proto::WIRE_I32, "RotationSpeed", "");
    add("OrbitControllerComponent", 3, proto::WIRE_I32, "OrbitDistance", "");

    add("MonsterControllerComponent", 1, proto::WIRE_I32, "WalkSpeed", "");
    add("MonsterControllerComponent", 2, proto::WIRE_VARINT, "AnimationControllerId", "");
    add("MonsterControllerComponent", 3, proto::WIRE_VARINT, "EntityId", "");
    add("MonsterControllerComponent", 4, proto::WIRE_VARINT, "RoamAreaId", "");

    add("WalkingMonsterControllerComponent", 1, proto::WIRE_VARINT, "WalkAnimationId", "");

    add("ChargingMonsterControllerComponent", 1, proto::WIRE_VARINT, "WalkAnimationId", "");
    add("ChargingMonsterControllerComponent", 2, proto::WIRE_VARINT, "ChargeAnimationId", "");
    add("ChargingMonsterControllerComponent", 3, proto::WIRE_VARINT, "RunAnimationId", "");
    add("ChargingMonsterControllerComponent", 4, proto::WIRE_I32, "RunSpeed", "");
    add("ChargingMonsterControllerComponent", 5, proto::WIRE_I32, "RunAcceleration", "");

    add("SnappingMonsterControllerComponent", 1, proto::WIRE_VARINT, "StandAnimationId", "");
    add("SnappingMonsterControllerComponent", 2, proto::WIRE_VARINT, "AttackAnimationId", "");
    add("SnappingMonsterControllerComponent", 3, proto::WIRE_VARINT, "BlendAnimationId", "");
    add("SnappingMonsterControllerComponent", 4, proto::WIRE_VARINT, "AttackAreaId", "");
    add("SnappingMonsterControllerComponent", 5, proto::WIRE_VARINT, "AttackSoundId", "");

    add("AttackComponent", 1, proto::WIRE_VARINT, "AnimationId", "");
    add("AttackComponent", 2, proto::WIRE_VARINT, "CollisionShapeId", "");
    add("AttackComponent", 3, proto::WIRE_VARINT, "AttackAreaId", "");
    add("AttackComponent", 4, proto::WIRE_VARINT, "SoundEffectId", "");
    add("AttackComponent", 5, proto::WIRE_I32, "AttackInterval", "");
    add("AttackComponent", 6, proto::WIRE_I32, "AttackDuration", "");
    add("AttackComponent", 7, proto::WIRE_I32, "DamageStartTime", "");
    add("AttackComponent", 8, proto::WIRE_I32, "DamageEndTime", "");
    add("AttackComponent", 9, proto::WIRE_I32, "AnimationStartBlendTime", "");
    add("AttackComponent", 10, proto::WIRE_I32, "AnimationEndBlendTime", "");
    add("AttackComponent", 11, proto::WIRE_LEN, "OnAttack", "Program");
    add("AttackComponent", 12, proto::WIRE_I32, "DamageStartTime2", "");
    add("AttackComponent", 13, proto::WIRE_I32, "DamageEndTime2", "");

    add("LeapingMonsterControllerComponent", 1, proto::WIRE_VARINT, "WalkAnimationId", "");
    add("LeapingMonsterControllerComponent", 2, proto::WIRE_VARINT, "LeapAttackId", "");

    add("SkellyMonsterControllerComponent", 1, proto::WIRE_VARINT, "CharControllerId", "");
    add("SkellyMonsterControllerComponent", 2, proto::WIRE_VARINT, "AttackAreaId", "");

    add("StaticMonsterControllerComponent", 1, proto::WIRE_VARINT, "AnimationId", "");
    add("StaticMonsterControllerComponent", 2, proto::WIRE_VARINT, "SoundId", "");

    add("ShootingMonsterControllerComponent", 1, proto::WIRE_VARINT, "WalkAnimationId", "");
    add("ShootingMonsterControllerComponent", 2, proto::WIRE_VARINT, "ShootAnimationId", "");

    add("BatMonsterControllerComponent", 1, proto::WIRE_VARINT, "FlyAnimationId", "");
    add("BatMonsterControllerComponent", 2, proto::WIRE_VARINT, "FlapSoundId", "");

    add("BouncingMonsterControllerComponent", 1, proto::WIRE_VARINT, "JumpAnimationId", "");
    add("BouncingMonsterControllerComponent", 2, proto::WIRE_VARINT, "FallAnimationId", "");
    add("BouncingMonsterControllerComponent", 3, proto::WIRE_I32, "JumpAngle", "");
    add("BouncingMonsterControllerComponent", 4, proto::WIRE_I32, "JumpSpeed", "");

    add("MonsterDeathControllerComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");

    add("GenericMonsterControllerComponent", 1, proto::WIRE_VARINT, "WalkAnimationId", "");

    add("SwingableWeaponComponent", 1, proto::WIRE_VARINT, "ModelId", "");
    add("SwingableWeaponComponent", 2, proto::WIRE_VARINT, "TrailId", "");
    add("SwingableWeaponComponent", 4, proto::WIRE_VARINT, "ImpactParticleEmitterId", "");
    add("SwingableWeaponComponent", 5, proto::WIRE_VARINT, "SwingSoundId", "");
    add("SwingableWeaponComponent", 6, proto::WIRE_VARINT, "DamageImpactSoundId", "");
    add("SwingableWeaponComponent", 7, proto::WIRE_VARINT, "GlowTrailId", "");
    add("SwingableWeaponComponent", 8, proto::WIRE_VARINT, "CollisionShapeId", "");
    add("SwingableWeaponComponent", 9, proto::WIRE_VARINT, "GlowId", "");
    add("SwingableWeaponComponent", 10, proto::WIRE_I32, "BaseLength", "");
    add("SwingableWeaponComponent", 11, proto::WIRE_I32, "GlowLength", "");
    add("SwingableWeaponComponent", 12, proto::WIRE_I32, "GlowIntensity", "");
    add("SwingableWeaponComponent", 13, proto::WIRE_I32, "Width", "");
    add("SwingableWeaponComponent", 14, proto::WIRE_LEN, "GlowColor", "FloatColor");

    add("SwingableWeaponControllerComponent", 1, proto::WIRE_VARINT, "ControllingModelId", "");
    add("SwingableWeaponControllerComponent", 2, proto::WIRE_LEN, "ControllingBoneName", "");
    add("SwingableWeaponControllerComponent", 3, proto::WIRE_LEN, "WeaponTemplateName", "");

    add("SwingComponent", 1, proto::WIRE_VARINT, "AnimationId", "");
    add("SwingComponent", 2, proto::WIRE_VARINT, "SwingLeftWeapon", "");
    add("SwingComponent", 3, proto::WIRE_VARINT, "SwingRightWeapon", "");
    add("SwingComponent", 6, proto::WIRE_I32, "StartFrame", "");
    add("SwingComponent", 7, proto::WIRE_I32, "EndFrame", "");

    add("WeaponGlowComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("WeaponGlowComponent", 2, proto::WIRE_LEN, "Color", "FloatColor");
    add("WeaponGlowComponent", 3, proto::WIRE_LEN, "ParticleColor", "FloatColor");
    add("WeaponGlowComponent", 4, proto::WIRE_I32, "Width", "");

    add("WeaponTrailComponent", 1, proto::WIRE_LEN, "Color", "FloatColor");

    add("PortalComponent", 1, proto::WIRE_LEN, "DestinationSceneName", "");
    add("PortalComponent", 2, proto::WIRE_LEN, "SpawnPointName", "");
    add("PortalComponent", 3, proto::WIRE_VARINT, "TapToEnter", "");
    add("PortalComponent", 4, proto::WIRE_VARINT, "TriggerShapeId", "");

    add("SpawnPointComponent", 1, proto::WIRE_VARINT, "FacingDirection", "");
    add("SpawnPointComponent", 2, proto::WIRE_LEN, "SpawnOffset", "Vector3");

    add("CollectableItemComponent", 1, proto::WIRE_VARINT, "Type", "");
    add("CollectableItemComponent", 2, proto::WIRE_VARINT, "Value", "");
    add("CollectableItemComponent", 3, proto::WIRE_LEN, "OnCollect", "Program");
    add("CollectableItemComponent", 4, proto::WIRE_LEN, "Identifier", "");
    add("CollectableItemComponent", 5, proto::WIRE_LEN, "ItemName", "");
    add("CollectableItemComponent", 6, proto::WIRE_VARINT, "RequiresPickup", "");

    add("TouchableComponent", 1, proto::WIRE_I32, "TouchRadius", "");
    add("TouchableComponent", 2, proto::WIRE_LEN, "OnTouch", "Program");

    add("ItemDropComponent_ItemDropEntry", 1, proto::WIRE_LEN, "TemplateName", "");
    add("ItemDropComponent_ItemDropEntry", 2, proto::WIRE_LEN, "ItemIdentifier", "");
    add("ItemDropComponent_ItemDropEntry", 3, proto::WIRE_I32, "DropChance", "");
    add("ItemDropComponent_ItemDropEntry", 4, proto::WIRE_VARINT, "MinCount", "");
    add("ItemDropComponent_ItemDropEntry", 5, proto::WIRE_VARINT, "MaxCount", "");

    add("ItemDropComponent", 1, proto::WIRE_LEN, "ItemName", "");
    add("ItemDropComponent", 2, proto::WIRE_LEN, "ItemIdentifier", "");
    add("ItemDropComponent", 3, proto::WIRE_LEN, "DropEntry", "ItemDropComponent_ItemDropEntry");
    add("ItemDropComponent", 4, proto::WIRE_VARINT, "CanDropMultipleItems", "");
    add("ItemDropComponent", 5, proto::WIRE_VARINT, "CanDropDefaultItems", "");

    add("OverlayTextComponent", 1, proto::WIRE_LEN, "Text", "");
    add("OverlayTextComponent", 2, proto::WIRE_LEN, "TextOffset", "Vector2");
    add("OverlayTextComponent", 3, proto::WIRE_LEN, "SpriteName", "");
    add("OverlayTextComponent", 4, proto::WIRE_LEN, "SpriteOffset", "Vector2");

    add("PortalEffectComponent", 1, proto::WIRE_VARINT, "PolygonId", "");
    add("PortalEffectComponent", 2, proto::WIRE_VARINT, "TextureMappingId", "");
    add("PortalEffectComponent", 3, proto::WIRE_LEN, "Color", "FloatColor");
    add("PortalEffectComponent", 4, proto::WIRE_LEN, "Speed", "Vector3");

    add("MagicBoltComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("MagicBoltComponent", 2, proto::WIRE_VARINT, "SwooshSoundId", "");
    add("MagicBoltComponent", 3, proto::WIRE_VARINT, "HitSoundId", "");
    add("MagicBoltComponent", 4, proto::WIRE_LEN, "Color", "FloatColor");
    add("MagicBoltComponent", 5, proto::WIRE_I32, "Speed", "");

    add("MagicExplosionComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("MagicExplosionComponent", 2, proto::WIRE_VARINT, "SoundId", "");
    add("MagicExplosionComponent", 3, proto::WIRE_LEN, "Color", "FloatColor");
    add("MagicExplosionComponent", 4, proto::WIRE_I32, "Radius", "");
    add("MagicExplosionComponent", 5, proto::WIRE_I32, "Duration", "");

    add("SkillComponent", 1, proto::WIRE_VARINT, "CastFinishAnimationId", "");
    add("SkillComponent", 2, proto::WIRE_LEN, "Origin", "Vector2");
    add("SkillComponent", 3, proto::WIRE_LEN, "CastObjectTemplateName", "");

    add("MagicSpellCastComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("MagicSpellCastComponent", 2, proto::WIRE_VARINT, "SoundEffectId", "");

    add("FireBreathComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("FireBreathComponent", 2, proto::WIRE_VARINT, "SwooshSoundId", "");
    add("FireBreathComponent", 3, proto::WIRE_LEN, "Color", "FloatColor");

    add("ProjectileControllerComponent", 1, proto::WIRE_VARINT, "AlignObjectRotation", "");
    add("ProjectileControllerComponent", 2, proto::WIRE_VARINT, "BreakOnGroundCollision", "");

    add("MagicBombComponent", 1, proto::WIRE_LEN, "Color", "FloatColor");

    add("MagicHookshotComponent", 1, proto::WIRE_VARINT, "ParticleEmitterId", "");
    add("MagicHookshotComponent", 2, proto::WIRE_VARINT, "SwooshSoundId", "");
    add("MagicHookshotComponent", 3, proto::WIRE_VARINT, "HitSoundId", "");
    add("MagicHookshotComponent", 4, proto::WIRE_LEN, "Color", "FloatColor");
    add("MagicHookshotComponent", 5, proto::WIRE_VARINT, "GroundHitSoundId", "");

    add("SpellComponent", 1, proto::WIRE_LEN, "OnCast", "Program");

    schemas_initialized = true;
}

static FieldSchema g_temp_find_field_result;
static FieldSchema g_temp_find_field_by_name_result;

static const FieldSchema* find_field(const std::string& parent_msg_type, uint32_t field_number) {
    // 1. Try av::g_schemas lookup (contains perfect auto-generated scene and component schemas)
    auto it_av = av::g_schemas.find(parent_msg_type);
    if (it_av != av::g_schemas.end()) {
        for (const auto& pair : it_av->second.fields) {
            uint32_t tag = pair.first;
            uint32_t f_num = tag >> 3;
            if (f_num == field_number) {
                g_temp_find_field_result.field_number = f_num;
                g_temp_find_field_result.wire_type = static_cast<proto::WireType>(tag & 7);
                g_temp_find_field_result.name = pair.second.name;
                g_temp_find_field_result.classname = pair.second.class_name;
                return &g_temp_find_field_result;
            }
        }
    }

    // 2. Try manual schemas lookup
    init_schemas();
    auto it = schemas.find(parent_msg_type);
    if (it != schemas.end()) {
        for (const auto& fs : it->second) {
            if (fs.field_number == field_number) return &fs;
        }
    }

    // "All" is a real FileRift root type, not a fallback schema. Field
    // numbers overlap heavily between Swordigo messages, so consulting it for
    // another class can silently rename and reinterpret unknown fields.
    return nullptr;
}

static const FieldSchema* find_field_by_name(const std::string& parent_msg_type, const std::string& name) {
    // 1. Try av::g_schemas lookup
    auto it_av = av::g_schemas.find(parent_msg_type);
    if (it_av != av::g_schemas.end()) {
        for (const auto& pair : it_av->second.fields) {
            if (pair.second.name == name) {
                uint32_t tag = pair.first;
                g_temp_find_field_by_name_result.field_number = tag >> 3;
                g_temp_find_field_by_name_result.wire_type = static_cast<proto::WireType>(tag & 7);
                g_temp_find_field_by_name_result.name = pair.second.name;
                g_temp_find_field_by_name_result.classname = pair.second.class_name;
                return &g_temp_find_field_by_name_result;
            }
        }
    }

    // 2. Try manual schemas lookup
    init_schemas();
    auto it = schemas.find(parent_msg_type);
    if (it != schemas.end()) {
        for (const auto& fs : it->second) {
            if (fs.name == name) return &fs;
        }
    }

    return nullptr;
}

static std::string root_class_for_filetype(const std::string& filetype) {
    if (filetype == "fr") return "All";
    if (filetype == "scene") return "Scene";
    if (filetype == "scl") return "ObjectLibrary";
    if (filetype == "gdata") return "GameData";
    if (filetype == "gopt") return "GameOptions";
    if (filetype == "gplayer") return "PlayerProfile";
    if (filetype == "gstate") return "GameState";
    if (filetype == "scmap") return "Map";
    if (filetype == "sounds") return "SoundLibrary";
    if (filetype == "fnt") return "Font";
    if (filetype == "atlas") return "Texture";
    throw std::invalid_argument("unsupported FileRift file type: " + filetype);
}

static std::string escape_bytes(const std::string& s) {
    std::string out;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\'') out += "\\'";
        else if (uc == '\\') out += "\\\\";
        else if (uc == '\n') out += "\\n";
        else if (uc == '\r') out += "\\r";
        else if (uc == '\t') out += "\\t";
        else if (uc >= 32 && uc < 127) out += c;
        else {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\x%02x", uc);
            out += buf;
        }
    }
    return "'" + out + "'";
}

static std::string unescape_bytes(const std::string& s) {
    if (s.size() < 2 || (s.front() != '\'' && s.front() != '"')) return s;
    std::string inner = s.substr(1, s.size() - 2);
    std::string out;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\\' && i + 1 < inner.size()) {
            char next = inner[i + 1];
            if (next == 'n') { out += '\n'; ++i; }
            else if (next == 'r') { out += '\r'; ++i; }
            else if (next == 't') { out += '\t'; ++i; }
            else if (next == '\'' || next == '"' || next == '\\') { out += next; ++i; }
            else if (next == 'x' && i + 3 < inner.size()) {
                std::string hex_str = inner.substr(i + 2, 2);
                char* end;
                long val = strtol(hex_str.c_str(), &end, 16);
                out += static_cast<char>(val);
                i += 3;
            } else {
                out += '\\';
            }
        } else {
            out += inner[i];
        }
    }
    return out;
}

static std::string unknown_tag_name(uint32_t field_number, proto::WireType wire_type) {
    const char* suffix = "unknown";
    if (wire_type == proto::WIRE_VARINT) suffix = "varint";
    else if (wire_type == proto::WIRE_I64) suffix = "i64";
    else if (wire_type == proto::WIRE_LEN) suffix = "len";
    else if (wire_type == proto::WIRE_I32) suffix = "i32";
    return "Tag_" + std::to_string(field_number) + "_" + suffix;
}

static void decode_message(const std::string& bytes, const std::string& classname, int indent, std::stringstream& out) {
    std::string tabs(indent * 4, ' ');
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        bool preserve_compile = false;
        std::string preserved_degrees;
        while (reader.read_field(f)) {
            // FileRift preservation records live outside the game schemas.
            if (f.field_number == 513 && f.wire_type == proto::WIRE_LEN) {
                out << tabs << f.bytes_val << "\n";
                continue;
            }
            if (f.field_number == 514 && f.wire_type == proto::WIRE_VARINT) {
                preserve_compile = true;
                continue;
            }
            if (f.field_number == 515 && f.wire_type == proto::WIRE_LEN) {
                preserved_degrees = f.bytes_val;
                continue;
            }

            const FieldSchema* fs = find_field(classname, f.field_number);
            std::string tagname;
            std::string sub_classname;
            if (fs) {
                tagname = fs->name;
                sub_classname = fs->classname;
            } else {
                tagname = unknown_tag_name(f.field_number, f.wire_type);
            }

            out << tabs << tagname;

            if (f.wire_type == proto::WIRE_LEN) {
                if (!sub_classname.empty()) {
                    out << "{\n";
                    decode_message(f.bytes_val, sub_classname, indent + 1, out);
                    out << tabs << "}\n";
                } else if (preserve_compile) {
                    out << " : @compile\n";
                } else if (tagname == "String") {
                    std::string body = f.bytes_val;
                    if (!body.empty() && body.front() == '\n') body.erase(body.begin());
                    out << " : $\n" << body << "\n$end\n";
                } else {
                    out << " : " << escape_bytes(f.bytes_val) << "\n";
                }
            } else if (f.wire_type == proto::WIRE_VARINT) {
                out << " : " << f.varint_val << "\n";
            } else if (f.wire_type == proto::WIRE_I32) {
                if (!preserved_degrees.empty()) out << " : " << preserved_degrees << "d\n";
                else out << " : " << std::setprecision(std::numeric_limits<float>::max_digits10) << f.float_val << "\n";
            } else if (f.wire_type == proto::WIRE_I64) {
                if (!preserved_degrees.empty()) out << " : " << preserved_degrees << "d\n";
                else out << " : " << std::setprecision(std::numeric_limits<double>::max_digits10) << f.double_val << "\n";
            }
            preserve_compile = false;
            preserved_degrees.clear();
        }
    } catch (const std::exception& e) {
        out << tabs << "# [Decode Error: " << e.what() << "]\n";
    }
}

std::string decode_protobuf(const std::string& bytes, const std::string& filetype) {
    const std::string root_class = root_class_for_filetype(filetype);

    std::stringstream out;
    out << "## FileRift decoded Swordigo file type: " << filetype << "\n\n";
    decode_message(bytes, root_class, 0, out);
    return out.str();
}

struct MarkupToken {
    std::string text;
    size_t line = 1;
};

static std::vector<MarkupToken> lex(const std::string& text) {
    std::vector<MarkupToken> tokens;
    std::string token;
    size_t token_line = 1;
    size_t line = 1;
    bool in_s_quote = false;
    bool in_d_quote = false;
    bool in_lua = false;
    bool in_comment = false;

    auto push = [&](std::string value, size_t at_line) {
        if (!value.empty()) tokens.push_back({std::move(value), at_line});
    };
    auto flush = [&]() {
        if (!token.empty()) {
            push(token, token_line);
            token.clear();
        }
    };

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        if (in_comment) {
            if (c == '\n') {
                in_comment = false;
                ++line;
            }
            continue;
        }

        if (in_lua) {
            if (i + 4 <= text.size() && text.substr(i, 4) == "$end") {
                push(token, token_line);
                token.clear();
                push("$end", line);
                in_lua = false;
                i += 3;
            } else {
                token += c;
                if (c == '\n') ++line;
            }
            continue;
        }

        if (in_s_quote) {
            token += c;
            size_t slash_count = 0;
            for (size_t j = i; j > 0 && text[j - 1] == '\\'; --j) ++slash_count;
            if (c == '\'' && (slash_count & 1u) == 0) {
                in_s_quote = false;
                push(token, token_line);
                token.clear();
            }
            if (c == '\n') ++line;
            continue;
        }

        if (in_d_quote) {
            token += c;
            size_t slash_count = 0;
            for (size_t j = i; j > 0 && text[j - 1] == '\\'; --j) ++slash_count;
            if (c == '"' && (slash_count & 1u) == 0) {
                in_d_quote = false;
                push(token, token_line);
                token.clear();
            }
            if (c == '\n') ++line;
            continue;
        }

        const bool dash_comment = c == '-' && i + 1 < text.size() && text[i + 1] == '-';
        const bool slash_comment = c == '/' && i + 1 < text.size() && text[i + 1] == '/';
        if (c == '#' || dash_comment || slash_comment) {
            flush();
            const size_t marker_size = (dash_comment || slash_comment) ? 2 : 1;
            const size_t comment_start = i;
            const size_t content_start = i + marker_size;
            const size_t end = text.find('\n', content_start);
            const bool explicitly_unpreserved = content_start < text.size() && text[content_start] == c;
            if (!explicitly_unpreserved)
                push("@@comment:" + text.substr(comment_start, (end == std::string::npos ? text.size() : end) - comment_start), line);
            if (end == std::string::npos) break;
            i = end;
            ++line;
            continue;
        }

        if (c == '$') {
            flush();
            push("$", line);
            in_lua = true;
            token_line = line;
            continue;
        }

        if (c == '\'') {
            flush();
            in_s_quote = true;
            token = "'";
            token_line = line;
            continue;
        }
        if (c == '"') {
            flush();
            in_d_quote = true;
            token = "\"";
            token_line = line;
            continue;
        }

        if (c == '{' || c == '}' || c == ':' || c == '=') {
            flush();
            push(std::string(1, c), line);
            continue;
        }
        if (c == ',' || c == ';') {
            flush();
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
            if (c == '\n') ++line;
            continue;
        }
        if (token.empty()) token_line = line;
        token += c;
    }
    if (in_s_quote || in_d_quote) throw std::runtime_error("FileRift line " + std::to_string(token_line) + ": unterminated string");
    if (in_lua) throw std::runtime_error("FileRift line " + std::to_string(token_line) + ": missing $end");
    flush();
    return tokens;
}

static bool parse_unknown_tag(const std::string& name, uint32_t& field_number, proto::WireType& wire_type) {
    if (name.rfind("Tag_", 0) != 0) return false;
    const size_t suffix = name.find('_', 4);
    const std::string number = name.substr(4, suffix == std::string::npos ? std::string::npos : suffix - 4);
    if (number.empty() || !std::all_of(number.begin(), number.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return false;
    unsigned long parsed = 0;
    try {
        parsed = std::stoul(number);
    } catch (...) {
        return false;
    }
    if (parsed == 0 || parsed > 0x1fffffffu) return false;
    field_number = static_cast<uint32_t>(parsed);
    if (suffix == std::string::npos) return true;
    const std::string type = name.substr(suffix + 1);
    if (type == "varint") wire_type = proto::WIRE_VARINT;
    else if (type == "i64") wire_type = proto::WIRE_I64;
    else if (type == "len") wire_type = proto::WIRE_LEN;
    else if (type == "i32") wire_type = proto::WIRE_I32;
    else return false;
    return true;
}

static double parse_real(const MarkupToken& token) {
    std::string value = token.text;
    bool degrees = !value.empty() && value.back() == 'd';
    if (degrees) value.pop_back();
    size_t consumed = 0;
    double result;
    try { result = std::stod(value, &consumed); }
    catch (...) { throw std::runtime_error("FileRift line " + std::to_string(token.line) + ": expected number, got '" + token.text + "'"); }
    if (consumed != value.size() || !std::isfinite(result))
        throw std::runtime_error("FileRift line " + std::to_string(token.line) + ": invalid number '" + token.text + "'");
    return degrees ? result * (3.14159265358979323846 / 180.0) : result;
}

static uint64_t parse_integer(const MarkupToken& token) {
    if (token.text.empty() || !std::all_of(token.text.begin(), token.text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        throw std::runtime_error("FileRift line " + std::to_string(token.line) + ": expected non-negative integer, got '" + token.text + "'");
    size_t consumed = 0;
    try {
        const uint64_t value = std::stoull(token.text, &consumed);
        if (consumed == token.text.size()) return value;
    } catch (...) {}
    throw std::runtime_error("FileRift line " + std::to_string(token.line) + ": integer out of range");
}

static std::string recode_message(const std::vector<MarkupToken>& tokens, size_t& idx, const std::string& classname, std::string& last_chunk) {
    proto::Writer writer;

    while (idx < tokens.size()) {
        const MarkupToken tag_token = tokens[idx];
        const std::string& t = tag_token.text;
        if (t == "}") {
            break;
        }
        if (t.rfind("@@comment:", 0) == 0) {
            writer.write_bytes_field(513, t.substr(10));
            ++idx;
            continue;
        }
        if (t == ":" || t == "=" || t == "{")
            throw std::runtime_error("FileRift line " + std::to_string(tag_token.line) + ": expected tag, got '" + t + "'");
        if (t == "@stop") break;
        if (t == "@line") { ++idx; continue; }

        std::string tagname = t;
        idx++;
        if (idx < tokens.size() && (tokens[idx].text == ":" || tokens[idx].text == "=")) {
            idx++;
        }
        if (idx >= tokens.size())
            throw std::runtime_error("FileRift line " + std::to_string(tag_token.line) + ": missing value for " + tagname);

        const MarkupToken val_token = tokens[idx];
        const std::string& val_tok = val_token.text;
        uint32_t f_num = 0;
        proto::WireType w_type = proto::WIRE_LEN;
        std::string sub_classname;

        if (!parse_unknown_tag(tagname, f_num, w_type)) {
            const FieldSchema* fs = find_field_by_name(classname, tagname);
            if (fs) {
                f_num = fs->field_number;
                w_type = fs->wire_type;
                sub_classname = fs->classname;
            }
        }
        if (f_num == 0)
            throw std::runtime_error("FileRift line " + std::to_string(tag_token.line) + ": tag '" + tagname + "' is not valid in " + classname);

        if (val_tok == "{") {
            if (w_type != proto::WIRE_LEN || sub_classname.empty())
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": tag '" + tagname + "' is not a message");
            idx++; // skip '{'
            std::string sub_bytes = recode_message(tokens, idx, sub_classname, last_chunk);
            if (idx < tokens.size() && tokens[idx].text == "}") {
                idx++; // skip '}'
            } else throw std::runtime_error("FileRift line " + std::to_string(tag_token.line) + ": missing closing brace for " + tagname);
            writer.write_bytes_field(f_num, sub_bytes);
        } else if (!val_tok.empty() && (val_tok.front() == '\'' || val_tok.front() == '"')) {
            if (w_type != proto::WIRE_LEN)
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": tag '" + tagname + "' does not accept a string");
            std::string unescaped = unescape_bytes(val_tok);
            writer.write_bytes_field(f_num, unescaped);
            idx++;
        } else if (val_tok == "$") {
            if (w_type != proto::WIRE_LEN)
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": Lua chunk requires a length-delimited field");
            idx++; // skip '$'
            if (idx >= tokens.size() || tokens[idx].text == "$end") last_chunk.clear();
            else { last_chunk = tokens[idx].text; ++idx; }
            if (idx >= tokens.size() || tokens[idx].text != "$end")
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": missing $end");
            ++idx;
            // normalize: the '$end' terminator is emitted on its own line by
            // the decoder, so drop the single trailing '\n' that precedes it
            // (mirrors reference recode lexeme[:-6]). Keeps decode->recode
            // round-trips at a fixed point instead of growing a newline each
            // cycle.
            if (!last_chunk.empty() && last_chunk.back() == '\n')
                last_chunk.pop_back();
            writer.write_bytes_field(f_num, last_chunk);
        } else if (val_tok == "@compile" || val_tok == "@comp") {
            if (w_type != proto::WIRE_LEN)
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": @compile requires a bytes field");
            if (last_chunk.empty())
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": @compile has no preceding Lua chunk");
            const std::string bytecode = compile_lua_to_bytecode(last_chunk);
            if (bytecode.empty())
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": Lua compilation failed");
            writer.write_varint_field(514, 0);
            writer.write_bytes_field(f_num, bytecode);
            ++idx;
        } else {
            const bool degrees = !val_tok.empty() && val_tok.back() == 'd' &&
                                 (w_type == proto::WIRE_I32 || w_type == proto::WIRE_I64);
            if (degrees) writer.write_bytes_field(515, val_tok.substr(0, val_tok.size() - 1));
            if (w_type == proto::WIRE_VARINT) {
                writer.write_varint_field(f_num, parse_integer(val_token));
            } else if (w_type == proto::WIRE_I32) {
                writer.write_float_field(f_num, static_cast<float>(parse_real(val_token)));
            } else if (w_type == proto::WIRE_I64) {
                writer.write_double_field(f_num, parse_real(val_token));
            } else {
                throw std::runtime_error("FileRift line " + std::to_string(val_token.line) + ": expected quoted string for " + tagname);
            }
            idx++;
        }
    }
    
    return writer.to_string();
}

std::string recode_markup(const std::string& text, const std::string& filetype) {
    const std::string root_class = root_class_for_filetype(filetype);
    const std::vector<MarkupToken> tokens = lex(text);
    size_t idx = 0;
    std::string last_chunk;
    const std::string result = recode_message(tokens, idx, root_class, last_chunk);
    if (idx < tokens.size() && tokens[idx].text == "}")
        throw std::runtime_error("FileRift line " + std::to_string(tokens[idx].line) + ": unmatched closing brace");
    return result;
}

std::string extract_lua_generic(const std::string& bytes) {
    if (bytes.size() >= 4 && bytes[0] == '\x1b' && bytes[1] == 'L' && bytes[2] == 'u' && bytes[3] == 'a') {
        return bytes;
    }
    
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            if (f.wire_type == proto::WIRE_LEN) {
                if (f.bytes_val.size() >= 4 && f.bytes_val[0] == '\x1b' && f.bytes_val[1] == 'L' && f.bytes_val[2] == 'u' && f.bytes_val[3] == 'a') {
                    return f.bytes_val;
                }
                if (f.bytes_val.find("function ") != std::string::npos || 
                    f.bytes_val.find("local ") != std::string::npos ||
                    f.bytes_val.find("end") != std::string::npos ||
                    f.bytes_val.find("--") != std::string::npos) {
                    return f.bytes_val;
                }
                
                std::string res = extract_lua_generic(f.bytes_val);
                if (!res.empty()) return res;
            }
        }
    } catch (...) {}
    
    return "";
}

} // namespace filerift
