/* scene_categories.h — Scene object / component category taxonomy.
 *
 * Pure header — no .cpp. The taxonomy is derived from the component universe
 * of the original game (Caver engine, see OpenSwordigo/arm32/libswordigo_ida32.c
 * GetComponentsWithInterface instantiations + OpenSwordigo/resources/Ruby schema
 * tables): every component type the editor can meet is mapped to one of a
 * handful of professional categories, mirroring how DCC tools (Blender
 * outliner / SMM2 palettes) group scene content.
 *
 * Used by the Ruby Objects panel to group objects/templates and to pick a
 * primary category + icon for a SceneObject or a template component list.
 */
#pragma once

#include <string>
#include <vector>

#include "tools/scene_loader.h"
#include "platform/IconsFontAwesome6.h"

namespace swk {

enum class ObjCategory {
    Enemies,      // monsters, attacks, projectiles
    Entities,     // hero / NPC / character entities
    Items,        // collectables, drops, magic, spells, weapons
    Geometry,     // ground meshes, models, backgrounds, water, sprites
    Effects,      // particles, fire, glows, trails, overlays
    Lighting,     // lights, shadows
    Controllers,  // triggers, doors, elevators, animation/transform controllers
    Audio,        // sound emitters
    Portals,      // scene portals
    Utility,      // spawn points, programs, health, physics, collision
    Other
};

inline const char* obj_category_label(ObjCategory c) {
    switch (c) {
        case ObjCategory::Enemies:     return "Enemies";
        case ObjCategory::Entities:    return "Entities & NPCs";
        case ObjCategory::Items:       return "Items & Collectibles";
        case ObjCategory::Geometry:    return "Geometry & Terrain";
        case ObjCategory::Effects:     return "Effects & Particles";
        case ObjCategory::Lighting:    return "Lighting";
        case ObjCategory::Controllers: return "Triggers & Controllers";
        case ObjCategory::Audio:       return "Audio";
        case ObjCategory::Portals:     return "Portals & Doors";
        case ObjCategory::Utility:     return "Utility & Meta";
        default:                       return "Other";
    }
}

inline const char* obj_category_icon(ObjCategory c) {
    switch (c) {
        case ObjCategory::Enemies:     return ICON_FA_BUG;
        case ObjCategory::Entities:    return ICON_FA_PERSON;
        case ObjCategory::Items:       return ICON_FA_GEM;
        case ObjCategory::Geometry:    return ICON_FA_CUBE;
        case ObjCategory::Effects:     return ICON_FA_WAND_MAGIC_SPARKLES;
        case ObjCategory::Lighting:    return ICON_FA_LIGHTBULB;
        case ObjCategory::Controllers: return ICON_FA_SHUFFLE;
        case ObjCategory::Audio:       return ICON_FA_MUSIC;
        case ObjCategory::Portals:     return ICON_FA_DOOR_OPEN;
        case ObjCategory::Utility:     return ICON_FA_GEAR;
        default:                       return ICON_FA_CIRCLE_INFO;
    }
}

/// Map a single component type name to its category.
inline ObjCategory category_for_component(const std::string& type) {
    // ── Enemies & combat ─────────────────────────────────────────────
    if (type.find("Monster") != std::string::npos ||
        type.find("Attack") != std::string::npos ||
        type.find("Projectile") != std::string::npos ||
        type == "FireBreathComponent" ||
        type == "MagicBoltComponent" || type == "MagicBombComponent" ||
        type == "MagicExplosionComponent" || type == "MagicHookshotComponent" ||
        type == "MagicSpellCastComponent" || type == "WeaponTrailComponent" ||
        type == "SwingableWeaponComponent" || type == "SwingableWeaponControllerComponent")
        return ObjCategory::Enemies;
    // ── Entities / characters ────────────────────────────────────────
    if (type.find("Entity") != std::string::npos ||
        type == "CharControllerComponent" || type == "CharAnimControllerComponent" ||
        type == "HeroEntityComponent")
        return ObjCategory::Entities;
    // ── Items & collectibles ─────────────────────────────────────────
    if (type == "CollectableItemComponent" || type == "ItemDropComponent" ||
        type == "SpellComponent" || type == "SkillComponent" ||
        type == "MagicBoltComponent" || type == "MagicBombComponent" ||
        type == "MagicExplosionComponent" || type == "MagicHookshotComponent" ||
        type == "MagicSpellCastComponent" || type == "SwingComponent" ||
        type == "SwingableWeaponComponent")
        return ObjCategory::Items;
    // ── Geometry / terrain / rendering surfaces ──────────────────────
    if (type.find("GroundMesh") != std::string::npos ||
        type == "ModelComponent" || type == "BackgroundComponent" ||
        type == "WaterMeshComponent" || type == "SpriteComponent" ||
        type == "TextureMappingComponent")
        return ObjCategory::Geometry;
    // ── Effects & particles ──────────────────────────────────────────
    if (type.find("Particle") != std::string::npos ||
        type == "FireEmitterComponent" || type == "SimpleGlowComponent" ||
        type == "WeaponGlowComponent" || type == "PortalEffectComponent" ||
        type == "OverlayTextComponent")
        return ObjCategory::Effects;
    // ── Lighting ─────────────────────────────────────────────────────
    if (type == "LightComponent" || type == "ShadowComponent")
        return ObjCategory::Lighting;
    // ── Controllers & triggers ───────────────────────────────────────
    if (type.find("Controller") != std::string::npos ||
        type == "KeyframeAnimationComponent" || type == "BlendAnimationComponent" ||
        type == "AnimationControllerComponent" ||
        type == "PressureTriggerComponent" || type == "TouchableComponent" ||
        type == "DoorControllerComponent" || type == "ElevatorControllerComponent" ||
        type == "BreakableObjectComponent" || type == "ObjectLinkControllerComponent" ||
        type == "OrbitControllerComponent" || type == "ModelTransformControllerComponent")
        return ObjCategory::Controllers;
    // ── Audio ────────────────────────────────────────────────────────
    if (type == "SoundEffectComponent")
        return ObjCategory::Audio;
    // ── Portals ──────────────────────────────────────────────────────
    if (type == "PortalComponent")
        return ObjCategory::Portals;
    // ── Utility / meta / physics ─────────────────────────────────────
    if (type == "SpawnPointComponent" || type == "ProgramComponent" ||
        type == "PropertiesComponent" || type == "HealthComponent" ||
        type == "DamageComponent" || type == "PhysicsObjectComponent" ||
        type == "PhysicsPlatformComponent" || type == "CollisionShapeComponent" ||
        type == "BoneControlledCollisionShapeComponent" ||
        type == "EntityActionComponent")
        return ObjCategory::Utility;
    return ObjCategory::Other;
}

/// Primary category of a component type-name list (templates, in order).
inline ObjCategory classify_components(const std::vector<std::string>& types) {
    if (types.empty()) return ObjCategory::Other;
    // Enemies dominate; then entities/items; geometry only when nothing
    // behavioural is present — matches how a designer would group the object.
    const int order[11] = {
        (int)ObjCategory::Enemies, (int)ObjCategory::Entities,
        (int)ObjCategory::Items, (int)ObjCategory::Portals,
        (int)ObjCategory::Lighting, (int)ObjCategory::Audio,
        (int)ObjCategory::Effects, (int)ObjCategory::Controllers,
        (int)ObjCategory::Geometry, (int)ObjCategory::Utility,
        (int)ObjCategory::Other
    };
    for (int oc : order)
        for (const auto& t : types)
            if ((int)category_for_component(t) == oc)
                return (ObjCategory)oc;
    return ObjCategory::Other;
}

/// Primary category of a resolved scene object (own + inherited components).
inline ObjCategory classify_object(const av::SceneObject& obj) {
    const auto& comps = obj.resolved_components.empty()
                            ? obj.components : obj.resolved_components;
    std::vector<std::string> types;
    types.reserve(comps.size());
    for (const auto& c : comps) types.push_back(c.type_name);
    if (types.empty() && !obj.mesh_name.empty())
        return ObjCategory::Geometry;
    return classify_components(types);
}

}  // namespace swk
