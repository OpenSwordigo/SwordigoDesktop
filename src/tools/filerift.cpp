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

/*
 * FileRift C++ Port
 * 
 * Missing Features (documented as requested):
 * - APK unpacking, modification, and rebuilding/signing (requires Java/jarsigner/zipalign integration).
 * - File manifests/checksum comparison to skip unmodified files (requires global database state).
 * - Detailed Lua compiler checks using host luac binary (uses direct extraction/in-memory compiler instead).
 * - CLI configuration profiles config.py support.
 */

namespace filerift {

struct FieldSchema {
    uint32_t field_number;
    proto::WireType wire_type;
    std::string name;
    std::string classname;
};

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

    // 3. Try global fallbacks
    auto it_all = schemas.find("All");
    if (it_all != schemas.end()) {
        for (const auto& fs : it_all->second) {
            if (fs.field_number == field_number) return &fs;
        }
    }
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

    // 3. Try global fallbacks
    auto it_all = schemas.find("All");
    if (it_all != schemas.end()) {
        for (const auto& fs : it_all->second) {
            if (fs.name == name) return &fs;
        }
    }
    return nullptr;
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

static void decode_message(const std::string& bytes, const std::string& classname, int indent, std::stringstream& out) {
    std::string tabs(indent * 4, ' ');
    try {
        proto::Reader reader(bytes);
        proto::Field f;
        while (reader.read_field(f)) {
            const FieldSchema* fs = find_field(classname, f.field_number);
            std::string tagname;
            std::string sub_classname;
            if (fs) {
                tagname = fs->name;
                sub_classname = fs->classname;
            } else {
                tagname = "Tag_" + std::to_string(f.field_number);
            }

            out << tabs << tagname;

            if (f.wire_type == proto::WIRE_LEN) {
                if (!sub_classname.empty()) {
                    out << "{\n";
                    decode_message(f.bytes_val, sub_classname, indent + 1, out);
                    out << tabs << "}\n";
                } else if (tagname == "String") {
                    out << " : $\n" << f.bytes_val << "\n" << tabs << "$end\n";
                } else {
                    out << " : " << escape_bytes(f.bytes_val) << "\n";
                }
            } else if (f.wire_type == proto::WIRE_VARINT) {
                out << " : " << f.varint_val << "\n";
            } else if (f.wire_type == proto::WIRE_I32) {
                out << " : " << f.float_val << "\n";
            } else if (f.wire_type == proto::WIRE_I64) {
                out << " : " << f.double_val << "\n";
            }
        }
    } catch (const std::exception& e) {
        out << tabs << "# [Decode Error: " << e.what() << "]\n";
    }
}

std::string decode_protobuf(const std::string& bytes, const std::string& filetype) {
    std::string root_class = "Scene";
    if (filetype == "scl") root_class = "ObjectLibrary";
    else if (filetype == "gdata") root_class = "GameData";
    else if (filetype == "gopt") root_class = "GameOptions";
    else if (filetype == "gplayer") root_class = "PlayerProfile";
    else if (filetype == "gstate") root_class = "GameState";
    else if (filetype == "scmap") root_class = "Map";
    else if (filetype == "sounds") root_class = "SoundLibrary";
    else if (filetype == "fnt") root_class = "Font";
    else if (filetype == "atlas") root_class = "Texture";

    std::stringstream out;
    out << "# FileRift decoded Swordigo file type: " << filetype << "\n\n";
    decode_message(bytes, root_class, 0, out);
    return out.str();
}

static std::vector<std::string> lex(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_s_quote = false;
    bool in_d_quote = false;
    bool in_lua = false;
    bool in_comment = false;
    
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        
        if (in_comment) {
            if (c == '\n') {
                in_comment = false;
                tokens.push_back("<newline>");
            }
            continue;
        }
        
        if (in_lua) {
            if (i + 4 <= text.size() && text.substr(i, 4) == "$end") {
                tokens.push_back(token);
                token.clear();
                tokens.push_back("$end");
                in_lua = false;
                i += 3;
            } else {
                token += c;
            }
            continue;
        }
        
        if (in_s_quote) {
            if (c == '\'' && (i == 0 || text[i-1] != '\\')) {
                in_s_quote = false;
                tokens.push_back("'" + token + "'");
                token.clear();
            } else {
                token += c;
            }
            continue;
        }
        
        if (in_d_quote) {
            if (c == '"' && (i == 0 || text[i-1] != '\\')) {
                in_d_quote = false;
                tokens.push_back("\"" + token + "\"");
                token.clear();
            } else {
                token += c;
            }
            continue;
        }
        
        if (c == '#') {
            in_comment = true;
            continue;
        }
        
        if (c == '$') {
            in_lua = true;
            continue;
        }
        
        if (c == '\'' && !in_d_quote) {
            in_s_quote = true;
            continue;
        }
        
        if (c == '"' && !in_s_quote) {
            in_d_quote = true;
            continue;
        }
        
        if (c == '{' || c == '}' || c == ':') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
            tokens.push_back(std::string(1, c));
            continue;
        }
        
        if (isspace(c)) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
            if (c == '\n') {
                tokens.push_back("<newline>");
            }
            continue;
        }
        
        token += c;
    }
    
    if (!token.empty()) {
        tokens.push_back(token);
    }
    
    return tokens;
}

static std::string recode_message(const std::vector<std::string>& tokens, size_t& idx, const std::string& classname) {
    proto::Writer writer;
    
    while (idx < tokens.size()) {
        const std::string& t = tokens[idx];
        if (t == "}") {
            break;
        }
        if (t == "<newline>" || t == ":" || t == "{") {
            idx++;
            continue;
        }
        
        std::string tagname = t;
        idx++;
        
        if (idx < tokens.size() && tokens[idx] == ":") {
            idx++;
        }
        
        if (idx >= tokens.size()) break;
        
        std::string val_tok = tokens[idx];
        
        uint32_t f_num = 0;
        proto::WireType w_type = proto::WIRE_LEN;
        std::string sub_classname;
        
        if (tagname.rfind("Tag_", 0) == 0) {
            f_num = std::stoul(tagname.substr(4));
            if (val_tok == "{") w_type = proto::WIRE_LEN;
            else if (val_tok.front() == '\'' || val_tok.front() == '"') w_type = proto::WIRE_LEN;
            else if (val_tok.find('.') != std::string::npos) w_type = proto::WIRE_I32;
            else w_type = proto::WIRE_VARINT;
        } else {
            const FieldSchema* fs = find_field_by_name(classname, tagname);
            if (fs) {
                f_num = fs->field_number;
                w_type = fs->wire_type;
                sub_classname = fs->classname;
            }
        }
        
        if (f_num == 0) {
            idx++;
            continue;
        }
        
        if (val_tok == "{") {
            idx++; // skip '{'
            std::string sub_bytes = recode_message(tokens, idx, sub_classname);
            if (idx < tokens.size() && tokens[idx] == "}") {
                idx++; // skip '}'
            }
            writer.write_bytes_field(f_num, sub_bytes);
        } else if (val_tok.front() == '\'' || val_tok.front() == '"') {
            std::string unescaped = unescape_bytes(val_tok);
            writer.write_bytes_field(f_num, unescaped);
            idx++;
        } else if (val_tok == "$") {
            idx++; // skip '$'
            std::string lua_source;
            while (idx < tokens.size() && tokens[idx] != "$end") {
                if (tokens[idx] == "<newline>") lua_source += "\n";
                else lua_source += tokens[idx] + " ";
                idx++;
            }
            if (idx < tokens.size() && tokens[idx] == "$end") {
                idx++; // skip '$end'
            }
            writer.write_bytes_field(1, lua_source);
            writer.write_bytes_field(2, "");
        } else {
            if (w_type == proto::WIRE_VARINT) {
                uint64_t val = std::stoull(val_tok);
                writer.write_varint_field(f_num, val);
            } else if (w_type == proto::WIRE_I32) {
                float val = std::stof(val_tok);
                writer.write_float_field(f_num, val);
            } else if (w_type == proto::WIRE_I64) {
                double val = std::stod(val_tok);
                writer.write_double_field(f_num, val);
            } else {
                writer.write_bytes_field(f_num, val_tok);
            }
            idx++;
        }
    }
    
    return writer.to_string();
}

std::string recode_markup(const std::string& text, const std::string& filetype) {
    std::string root_class = "Scene";
    if (filetype == "scl") root_class = "ObjectLibrary";
    else if (filetype == "gdata") root_class = "GameData";
    else if (filetype == "gopt") root_class = "GameOptions";
    else if (filetype == "gplayer") root_class = "PlayerProfile";
    else if (filetype == "gstate") root_class = "GameState";
    else if (filetype == "scmap") root_class = "Map";
    else if (filetype == "sounds") root_class = "SoundLibrary";
    else if (filetype == "fnt") root_class = "Font";
    else if (filetype == "atlas") root_class = "Texture";

    std::vector<std::string> tokens = lex(text);
    size_t idx = 0;
    return recode_message(tokens, idx, root_class);
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
