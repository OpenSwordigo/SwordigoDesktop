/*
 * sre_caver.c — Caver engine component interface resolution
 *
 * Resolves all 60+ component interface vtable pointers from libswordigo.so
 * using the g_swordigo_base offset + nm-verified symbol addresses.
 *
 * Each Component::Interface() call returns the singleton interface vtable
 * pointer for that component type. We call them once at init and store
 * the result so Lua mods can call ComponentWithInterface() cheaply.
 *
 * Symbol format: _ZN5Caver<len><Name>9InterfaceEv
 */

#include "sre_caver.h"
#include "sre.h"

/* =========================================================================
 * Component interface globals
 * ========================================================================= */
void *GlowComponent_Interface;
void *ManaComponent_Interface;
void *LightComponent_Interface;
void *ModelComponent_Interface;
void *ShapeComponent_Interface;
void *SkillComponent_Interface;
void *SpellComponent_Interface;
void *SwingComponent_Interface;
void *AttackComponent_Interface;
void *DamageComponent_Interface;
void *EntityComponent_Interface;
void *HealthComponent_Interface;
void *PortalComponent_Interface;
void *ShadowComponent_Interface;
void *SpriteComponent_Interface;
void *OverlayComponent_Interface;
void *ProgramComponent_Interface;
void *ShatterComponent_Interface;
void *ItemDropComponent_Interface;
void *ParticleComponent_Interface;
void *AnimationComponent_Interface;
void *MagicBoltComponent_Interface;
void *MagicBombComponent_Interface;
void *TouchableComponent_Interface;
void *TransformComponent_Interface;
void *WaterMeshComponent_Interface;
void *BackgroundComponent_Interface;
void *EntityInfoComponent_Interface;
void *FireBreathComponent_Interface;
void *GroundMeshComponent_Interface;
void *HeroEntityComponent_Interface;
void *PropertiesComponent_Interface;
void *SimpleGlowComponent_Interface;
void *SpawnPointComponent_Interface;
void *TextBubbleComponent_Interface;
void *WeaponGlowComponent_Interface;
void *FireEmitterComponent_Interface;
void *OverlayTextComponent_Interface;
void *SoundEffectComponent_Interface;
void *WeaponTrailComponent_Interface;
void *EntityActionComponent_Interface;
void *PortalEffectComponent_Interface;
void *ShadowVolumeComponent_Interface;
void *UtilityShapeComponent_Interface;
void *GroundPolygonComponent_Interface;
void *HookshotTrailComponent_Interface;
void *MagicHookshotComponent_Interface;
void *MonsterEntityComponent_Interface;
void *ParticleFieldComponent_Interface;
void *PhysicsObjectComponent_Interface;
void *BlendAnimationComponent_Interface;
void *BushControllerComponent_Interface;
void *CharControllerComponent_Interface;
void *CollisionShapeComponent_Interface;
void *DimensionSpellComponent_Interface;
void *DoorControllerComponent_Interface;
void *MagicExplosionComponent_Interface;
void *MagicSpellCastComponent_Interface;
void *ObjectModifierComponent_Interface;
void *ParticleObjectComponent_Interface;
void *TextureMappingComponent_Interface;
void *BreakableObjectComponent_Interface;
void *CollectableItemComponent_Interface;
void *DimensionObjectComponent_Interface;
void *OrbitControllerComponent_Interface;
void *ParticleEmitterComponent_Interface;
void *PhysicsPlatformComponent_Interface;
void *PressureTriggerComponent_Interface;
void *SwingableWeaponComponent_Interface;
void *EntityControllerComponent_Interface;
void *KeyframeAnimationComponent_Interface;
void *MonsterControllerComponent_Interface;
void *CharAnimControllerComponent_Interface;
void *ElevatorControllerComponent_Interface;
void *OverlayTargetArrowComponent_Interface;
void *RotatingBackgroundComponent_Interface;
void *AnimationControllerComponent_Interface;
void *GroundMeshGeneratorComponent_Interface;
void *TransformControllerComponent_Interface;
void *BatMonsterControllerComponent_Interface;
void *MagicParticleEmitterComponent_Interface;
void *ObjectLinkControllerComponent_Interface;
void *ProjectileControllerComponent_Interface;
void *MonsterDeathControllerComponent_Interface;
void *SkellyMonsterControllerComponent_Interface;
void *StaticMonsterControllerComponent_Interface;
void *GenericMonsterControllerComponent_Interface;
void *LeapingMonsterControllerComponent_Interface;
void *ModelTransformControllerComponent_Interface;
void *WalkingMonsterControllerComponent_Interface;
void *BouncingMonsterControllerComponent_Interface;
void *ChargingMonsterControllerComponent_Interface;
void *ShootingMonsterControllerComponent_Interface;
void *SnappingMonsterControllerComponent_Interface;
void *SwingableWeaponControllerComponent_Interface;
void *ProjectileMonsterControllerComponent_Interface;
void *BoneControlledCollisionShapeComponent_Interface;

/* =========================================================================
 * Function pointer globals
 * ========================================================================= */
pfn_SceneObject_ComponentWithInterface g_SceneObject_ComponentWithInterface = 0;
pfn_ProgramState_FromLuaState          g_ProgramState_FromLuaState          = 0;

pfn_GameOverVC_DidContinue g_sre_GameOverVC_DidContinue = 0;

pfn_TextBubble_SetHandleTouches g_sre_TextBubble_SetHandleTouches = 0;
pfn_TextBubble_IsTextFinishedShowing g_sre_TextBubble_IsTextFinishedShowing = 0;
pfn_TextBubble_ShowText g_sre_TextBubble_ShowText = 0;

pfn_HealthBar_SetMaxHealth g_sre_HealthBar_SetMaxHealth = 0;
pfn_HealthBar_SetCurrentHealth g_sre_HealthBar_SetCurrentHealth = 0;

pfn_ManaBar_SetMaxMana g_sre_ManaBar_SetMaxMana = 0;
pfn_ManaBar_SetCurrentMana g_sre_ManaBar_SetCurrentMana = 0;

pfn_CoinBar_SetCurrentCoins g_sre_CoinBar_SetCurrentCoins = 0;

pfn_GameOverlayView_SetControlsHidden g_sre_GameOverlayView_SetControlsHidden = 0;
pfn_GameOverlayView_SetShowsUseButton g_sre_GameOverlayView_SetShowsUseButton = 0;
pfn_GameOverlayView_SetSkillButtonDisabled g_sre_GameOverlayView_SetSkillButtonDisabled = 0;

pfn_GUIEffect_FadeOut g_sre_GUIEffect_FadeOut = 0;
pfn_GUIEffect_FadeIn g_sre_GUIEffect_FadeIn = 0;
pfn_GUIEffect_Update g_sre_GUIEffect_Update = 0;

pfn_GUIView_Update g_sre_GUIView_Update = 0;

pfn_CharController_CanUse g_sre_CharController_CanUse = 0;
pfn_CharController_CanPickup g_sre_CharController_CanPickup = 0;
pfn_CharController_StartMovingToDirection g_sre_CharController_StartMovingToDirection = 0;
pfn_CharController_StopMovingToDirection g_sre_CharController_StopMovingToDirection = 0;
pfn_CharController_StartJumping g_sre_CharController_StartJumping = 0;
pfn_CharController_StopJumping g_sre_CharController_StopJumping = 0;
pfn_CharController_CanJump g_sre_CharController_CanJump = 0;
pfn_CharController_CanDoSomething g_sre_CharController_CanDoSomething = 0;
pfn_CharController_CanBeginCasting g_sre_CharController_CanBeginCasting = 0;
pfn_CharController_Use g_sre_CharController_Use = 0;
pfn_CharController_Die g_sre_CharController_Die = 0;
pfn_CharController_Hurt g_sre_CharController_Hurt = 0;
pfn_CharController_Swing g_sre_CharController_Swing = 0;
pfn_CharController_StopSwing g_sre_CharController_StopSwing = 0;
pfn_CharController_DropQuickly g_sre_CharController_DropQuickly = 0;
pfn_CharController_CancelCasting g_sre_CharController_CancelCasting = 0;
pfn_CharController_FinishCasting g_sre_CharController_FinishCasting = 0;
pfn_CharController_CanSwing g_sre_CharController_CanSwing = 0;

pfn_GameSceneController_CanCastSkill g_sre_GameSceneController_CanCastSkill = 0;

pfn_GameSceneView_HideCinematicSkipButton g_sre_GameSceneView_HideCinematicSkipButton = 0;

/* =========================================================================
 * Helper: call a no-arg function at guest address and return void*
 *
 * Each Component::Interface() function is:
 *   static void* Interface() { return &s_interface; }
 * It's a trivial function: ADRP + ADD + RET, returns a pointer in X0.
 * We just call it via the ARM64 function pointer.
 * ========================================================================= */
typedef void* (*pfn_get_iface)(void);

static void* call_iface(uint64_t addr) {
    if (!addr) return (void*)0;
    pfn_get_iface fn = (pfn_get_iface)addr;
    return fn();
}

/* =========================================================================
 * nm-verified symbol offsets in libswordigo.so v1.4.12 (ARM64)
 *
 * Generated by:
 *   nm -gD libswordigo.so | grep 'Component9InterfaceEv'
 * All offsets are ELF file offsets (loaded_vaddr - 0x1000000).
 * ========================================================================= */
void sre_caver_init(uint64_t swordigo_base) {
#define IFACE(var, off) if (!var) var = call_iface(swordigo_base + (off))

    /* SceneObject::ComponentWithInterface */
    g_SceneObject_ComponentWithInterface =
        (pfn_SceneObject_ComponentWithInterface)(swordigo_base + 0x47447c);

    /* ProgramState::FromLuaState */
    g_ProgramState_FromLuaState =
        (pfn_ProgramState_FromLuaState)(swordigo_base + 0x4c1094);

    /* Component interfaces — offsets from nm -gD libswordigo.so v1.4.12 */
    IFACE(GlowComponent_Interface,                   0x2098d4);
    IFACE(ManaComponent_Interface,                   0x23b394);
    IFACE(LightComponent_Interface,                  0x22e3f0);
    IFACE(ModelComponent_Interface,                  0x23df84);
    IFACE(ShapeComponent_Interface,                  0x27c250);
    IFACE(SkillComponent_Interface,                  0x27f574);
    IFACE(SpellComponent_Interface,                  0x284160);
    IFACE(SwingComponent_Interface,                  0x28c814);
    IFACE(AttackComponent_Interface,                 0x1dfe00);
    IFACE(DamageComponent_Interface,                 0x1f4ce0);
    IFACE(EntityComponent_Interface,                 0x203660);
    IFACE(HealthComponent_Interface,                 0x210c14);
    IFACE(PortalComponent_Interface,                 0x254af4);
    IFACE(ShadowComponent_Interface,                 0x278a78);
    IFACE(SpriteComponent_Interface,                 0x287104);
    IFACE(OverlayComponent_Interface,                0x24c560);
    IFACE(ProgramComponent_Interface,                0x2591e0);
    IFACE(ShatterComponent_Interface,                0x278f18);
    IFACE(ItemDropComponent_Interface,               0x21a374);
    IFACE(ParticleComponent_Interface,               0x24f07c);
    IFACE(AnimationComponent_Interface,              0x1d69e8);
    IFACE(MagicBoltComponent_Interface,              0x233194);
    IFACE(MagicBombComponent_Interface,              0x234bf0);
    IFACE(TouchableComponent_Interface,              0x296bf4);
    IFACE(TransformComponent_Interface,              0x29fdf4);
    IFACE(WaterMeshComponent_Interface,              0x2b3f50);
    IFACE(BackgroundComponent_Interface,             0x1e25f0);
    IFACE(EntityInfoComponent_Interface,             0x205274);
    IFACE(FireBreathComponent_Interface,             0x207ff8);
    IFACE(GroundMeshComponent_Interface,             0x20f9a4);
    IFACE(HeroEntityComponent_Interface,             0x211778);
    IFACE(PropertiesComponent_Interface,             0x25b3f8);
    IFACE(SimpleGlowComponent_Interface,             0x27f490);
    IFACE(SpawnPointComponent_Interface,             0x28430c);
    IFACE(TextBubbleComponent_Interface,             0x293a2c);
    IFACE(WeaponGlowComponent_Interface,             0x2b415c);
    IFACE(FireEmitterComponent_Interface,            0x208274);
    IFACE(OverlayTextComponent_Interface,            0x24ded0);
    IFACE(SoundEffectComponent_Interface,            0x2840f4);
    IFACE(WeaponTrailComponent_Interface,            0x2b558c);
    IFACE(EntityActionComponent_Interface,           0x203f58);
    IFACE(PortalEffectComponent_Interface,           0x254e34);
    IFACE(ShadowVolumeComponent_Interface,           0x278c60);
    IFACE(UtilityShapeComponent_Interface,           0x2a9484);
    IFACE(GroundPolygonComponent_Interface,          0x20fb54);
    IFACE(HookshotTrailComponent_Interface,          0x211d1c);
    IFACE(MagicHookshotComponent_Interface,          0x233c14);
    IFACE(MonsterEntityComponent_Interface,          0x240fd8);
    IFACE(ParticleFieldComponent_Interface,          0x24f6e8);
    IFACE(PhysicsObjectComponent_Interface,          0x252b60);
    IFACE(BlendAnimationComponent_Interface,         0x1e6a1c);
    IFACE(BushControllerComponent_Interface,         0x1eb7f0);
    IFACE(CharControllerComponent_Interface,         0x1ef714);
    IFACE(CollisionShapeComponent_Interface,         0x1f31d0);
    IFACE(DimensionSpellComponent_Interface,         0x2011e4);
    IFACE(DoorControllerComponent_Interface,         0x201cc0);
    IFACE(MagicExplosionComponent_Interface,         0x235b14);
    IFACE(MagicSpellCastComponent_Interface,         0x23bdb8);
    IFACE(ObjectModifierComponent_Interface,         0x24ada4);
    IFACE(ParticleObjectComponent_Interface,         0x24f984);
    IFACE(TextureMappingComponent_Interface,         0x293f4c);
    IFACE(BreakableObjectComponent_Interface,        0x1e9490);
    IFACE(CollectableItemComponent_Interface,        0x1f18bc);
    IFACE(DimensionObjectComponent_Interface,        0x200df4);
    IFACE(OrbitControllerComponent_Interface,        0x24c1f4);
    IFACE(ParticleEmitterComponent_Interface,        0x24f274);
    IFACE(PhysicsPlatformComponent_Interface,        0x252d78);
    IFACE(PressureTriggerComponent_Interface,        0x25901c);
    IFACE(SwingableWeaponComponent_Interface,        0x28c2e0);
    IFACE(EntityControllerComponent_Interface,       0x204568);
    IFACE(KeyframeAnimationComponent_Interface,      0x22a2f4);
    IFACE(MonsterControllerComponent_Interface,      0x240634);
    IFACE(CharAnimControllerComponent_Interface,     0x1ec420);
    IFACE(ElevatorControllerComponent_Interface,     0x202f44);
    IFACE(OverlayTargetArrowComponent_Interface,     0x24da00);
    IFACE(RotatingBackgroundComponent_Interface,     0x272bf0);
    IFACE(AnimationControllerComponent_Interface,    0x1d7ac0);
    IFACE(GroundMeshGeneratorComponent_Interface,    0x20fc60);
    IFACE(TransformControllerComponent_Interface,    0x29f840);
    IFACE(BatMonsterControllerComponent_Interface,   0x1e1af4);
    IFACE(MagicParticleEmitterComponent_Interface,   0x235ef8);
    IFACE(ObjectLinkControllerComponent_Interface,   0x24b1a4);
    IFACE(ProjectileControllerComponent_Interface,   0x25a438);
    IFACE(MonsterDeathControllerComponent_Interface, 0x240dc4);
    IFACE(SkellyMonsterControllerComponent_Interface,0x2800dc);
    IFACE(StaticMonsterControllerComponent_Interface,0x287798);
    IFACE(GenericMonsterControllerComponent_Interface,0x209264);
    IFACE(LeapingMonsterControllerComponent_Interface,0x22e7e8);
    IFACE(ModelTransformControllerComponent_Interface,0x23e3f4);
    IFACE(WalkingMonsterControllerComponent_Interface,0x2b3eb4);
    IFACE(BouncingMonsterControllerComponent_Interface,0x1e8108);
    IFACE(ChargingMonsterControllerComponent_Interface,0x1f0890);
    IFACE(ShootingMonsterControllerComponent_Interface,0x279344);
    IFACE(SnappingMonsterControllerComponent_Interface,0x284088);
    IFACE(SwingableWeaponControllerComponent_Interface,0x28c940);
    IFACE(ProjectileMonsterControllerComponent_Interface,0x25a648);
    IFACE(BoneControlledCollisionShapeComponent_Interface,0x1e7e88);

#undef IFACE
}
