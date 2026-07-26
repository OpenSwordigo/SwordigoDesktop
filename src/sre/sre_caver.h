/*
 * sre_caver.h — Caver engine component interfaces and helper accessors
 *
 * Ported from SwKiwi caver/ with ARM64 (64-bit) struct offsets.
 * Provides:
 *   - Component interface pointers (resolved at sre_init time)
 *   - SceneObject field accessors
 *   - ProgramState / GameController / SceneController traversal
 *   - SceneObject::ComponentWithInterface helper
 */

#ifndef SRE_CAVER_H
#define SRE_CAVER_H

#include "sre.h"
#include "sre_lua.h"

/* =========================================================================
 * Opaque engine types
 * ========================================================================= */
typedef void SceneObject;
typedef void ProgramState;
typedef void GameController;
typedef void SceneController;
typedef void Component;

/* =========================================================================
 * FloatColor — Caver::FloatColor (4 × float, matching SwKiWi types.h)
 * Used by SwingableWeaponComponent::SetGlowColor
 * ========================================================================= */
typedef struct {
    float R;
    float G;
    float B;
    float A;
} SreFloatColor;

/* =========================================================================
 * ARM64 struct field accessor macros
 * =========================================================================
 * $F(type, ptr, offset) — read field at byte offset
 * $W(type, ptr, offset, val) — write field at byte offset
 */
#define $F(type, ptr, off)       (*(type*)((char*)(ptr) + (off)))
#define $W(type, ptr, off, val)  (*(type*)((char*)(ptr) + (off)) = (val))

/* Double-deref: follow pointer at offset, then read sub-field */
#define $FF(type, ptr, off1, off2) \
    (*(type*)((char*)($F(void*, ptr, off1)) + (off2)))

/* =========================================================================
 * Component interface vtable pointers
 * (initialised by sre_caver_init())
 * ========================================================================= */

/* -- basic/physics -- */
extern void *GlowComponent_Interface;
extern void *ManaComponent_Interface;
extern void *LightComponent_Interface;
extern void *ModelComponent_Interface;
extern void *ShapeComponent_Interface;
extern void *SkillComponent_Interface;
extern void *SpellComponent_Interface;
extern void *SwingComponent_Interface;
extern void *AttackComponent_Interface;
extern void *DamageComponent_Interface;
extern void *EntityComponent_Interface;
extern void *HealthComponent_Interface;
extern void *PortalComponent_Interface;
extern void *ShadowComponent_Interface;
extern void *SpriteComponent_Interface;
extern void *OverlayComponent_Interface;
extern void *ProgramComponent_Interface;
extern void *ShatterComponent_Interface;
extern void *ItemDropComponent_Interface;
extern void *ParticleComponent_Interface;
extern void *AnimationComponent_Interface;
extern void *MagicBoltComponent_Interface;
extern void *MagicBombComponent_Interface;
extern void *TouchableComponent_Interface;
extern void *TransformComponent_Interface;
extern void *WaterMeshComponent_Interface;
extern void *BackgroundComponent_Interface;
extern void *EntityInfoComponent_Interface;
extern void *FireBreathComponent_Interface;
extern void *GroundMeshComponent_Interface;
extern void *HeroEntityComponent_Interface;
extern void *PropertiesComponent_Interface;
extern void *SimpleGlowComponent_Interface;
extern void *SpawnPointComponent_Interface;
extern void *TextBubbleComponent_Interface;
extern void *WeaponGlowComponent_Interface;
extern void *FireEmitterComponent_Interface;
extern void *OverlayTextComponent_Interface;
extern void *SoundEffectComponent_Interface;
extern void *WeaponTrailComponent_Interface;
extern void *EntityActionComponent_Interface;
extern void *PortalEffectComponent_Interface;
extern void *ShadowVolumeComponent_Interface;
extern void *UtilityShapeComponent_Interface;
extern void *GroundPolygonComponent_Interface;
extern void *HookshotTrailComponent_Interface;
extern void *MagicHookshotComponent_Interface;
extern void *MonsterEntityComponent_Interface;
extern void *ParticleFieldComponent_Interface;
extern void *PhysicsObjectComponent_Interface;
extern void *BlendAnimationComponent_Interface;
extern void *BushControllerComponent_Interface;
extern void *CharControllerComponent_Interface;
extern void *CollisionShapeComponent_Interface;
extern void *DimensionSpellComponent_Interface;
extern void *DoorControllerComponent_Interface;
extern void *MagicExplosionComponent_Interface;
extern void *MagicSpellCastComponent_Interface;
extern void *ObjectModifierComponent_Interface;
extern void *ParticleObjectComponent_Interface;
extern void *TextureMappingComponent_Interface;
extern void *BreakableObjectComponent_Interface;
extern void *CollectableItemComponent_Interface;
extern void *DimensionObjectComponent_Interface;
extern void *OrbitControllerComponent_Interface;
extern void *ParticleEmitterComponent_Interface;
extern void *PhysicsPlatformComponent_Interface;
extern void *PressureTriggerComponent_Interface;
extern void *SwingableWeaponComponent_Interface;
extern void *EntityControllerComponent_Interface;
extern void *KeyframeAnimationComponent_Interface;
extern void *MonsterControllerComponent_Interface;
extern void *CharAnimControllerComponent_Interface;
extern void *ElevatorControllerComponent_Interface;
extern void *OverlayTargetArrowComponent_Interface;
extern void *RotatingBackgroundComponent_Interface;
extern void *AnimationControllerComponent_Interface;
extern void *GroundMeshGeneratorComponent_Interface;
extern void *TransformControllerComponent_Interface;
extern void *BatMonsterControllerComponent_Interface;
extern void *MagicParticleEmitterComponent_Interface;
extern void *ObjectLinkControllerComponent_Interface;
extern void *ProjectileControllerComponent_Interface;
extern void *MonsterDeathControllerComponent_Interface;
extern void *SkellyMonsterControllerComponent_Interface;
extern void *StaticMonsterControllerComponent_Interface;
extern void *GenericMonsterControllerComponent_Interface;
extern void *LeapingMonsterControllerComponent_Interface;
extern void *ModelTransformControllerComponent_Interface;
extern void *WalkingMonsterControllerComponent_Interface;
extern void *BouncingMonsterControllerComponent_Interface;
extern void *ChargingMonsterControllerComponent_Interface;
extern void *ShootingMonsterControllerComponent_Interface;
extern void *SnappingMonsterControllerComponent_Interface;
extern void *SwingableWeaponControllerComponent_Interface;
extern void *ProjectileMonsterControllerComponent_Interface;
extern void *BoneControlledCollisionShapeComponent_Interface;

/* =========================================================================
 * SceneObject::ComponentWithInterface function pointer
 * Symbol: _ZNK5Caver11SceneObject22ComponentWithInterfaceEl
 * ========================================================================= */
typedef void* (*pfn_SceneObject_ComponentWithInterface)(const SceneObject*, void*);
extern pfn_SceneObject_ComponentWithInterface g_SceneObject_ComponentWithInterface;

/* Query a component from a SceneObject by interface pointer */
static inline void* sre_scene_object_component(const SceneObject* obj, void* iface) {
    if (!obj || !iface || !g_SceneObject_ComponentWithInterface) return (void*)0;
    return g_SceneObject_ComponentWithInterface(obj, iface);
}

/* =========================================================================
 * ProgramState::FromLuaState function pointer
 * Symbol: _ZN5Caver12ProgramState12FromLuaStateEP9lua_State
 * ========================================================================= */
typedef void* (*pfn_ProgramState_FromLuaState)(lua_State*);
extern pfn_ProgramState_FromLuaState g_ProgramState_FromLuaState;

// GameOverViewDidContinue
typedef void (*pfn_GameOverVC_DidContinue)(void*, int);
extern pfn_GameOverVC_DidContinue g_sre_GameOverVC_DidContinue;

// TextBubbleComponent
typedef void (*pfn_TextBubble_SetHandleTouches)(void*, int);
typedef int (*pfn_TextBubble_IsTextFinishedShowing)(void*);
typedef void (*pfn_TextBubble_ShowText)(void*, SreString*, float);
extern pfn_TextBubble_SetHandleTouches g_sre_TextBubble_SetHandleTouches;
extern pfn_TextBubble_IsTextFinishedShowing g_sre_TextBubble_IsTextFinishedShowing;
extern pfn_TextBubble_ShowText g_sre_TextBubble_ShowText;

// HealthBar
typedef void (*pfn_HealthBar_SetMaxHealth)(void*, int);
typedef void (*pfn_HealthBar_SetCurrentHealth)(void*, int);
extern pfn_HealthBar_SetMaxHealth g_sre_HealthBar_SetMaxHealth;
extern pfn_HealthBar_SetCurrentHealth g_sre_HealthBar_SetCurrentHealth;

// ManaBar
typedef void (*pfn_ManaBar_SetMaxMana)(void*, int);
typedef void (*pfn_ManaBar_SetCurrentMana)(void*, int);
extern pfn_ManaBar_SetMaxMana g_sre_ManaBar_SetMaxMana;
extern pfn_ManaBar_SetCurrentMana g_sre_ManaBar_SetCurrentMana;

// CoinBar
typedef void (*pfn_CoinBar_SetCurrentCoins)(void*, int);
extern pfn_CoinBar_SetCurrentCoins g_sre_CoinBar_SetCurrentCoins;

// GameOverlayView
typedef void (*pfn_GameOverlayView_SetControlsHidden)(void*, int);
typedef void (*pfn_GameOverlayView_SetShowsUseButton)(void*, int);
typedef void (*pfn_GameOverlayView_SetSkillButtonDisabled)(void*, int);
extern pfn_GameOverlayView_SetControlsHidden g_sre_GameOverlayView_SetControlsHidden;
extern pfn_GameOverlayView_SetShowsUseButton g_sre_GameOverlayView_SetShowsUseButton;
extern pfn_GameOverlayView_SetSkillButtonDisabled g_sre_GameOverlayView_SetSkillButtonDisabled;

// GUIEffect
typedef void (*pfn_GUIEffect_FadeOut)(void*, float);
typedef void (*pfn_GUIEffect_FadeIn)(void*, float);
typedef void (*pfn_GUIEffect_Update)(void*, float);
extern pfn_GUIEffect_FadeOut g_sre_GUIEffect_FadeOut;
extern pfn_GUIEffect_FadeIn g_sre_GUIEffect_FadeIn;
extern pfn_GUIEffect_Update g_sre_GUIEffect_Update;

// GUIView
typedef void (*pfn_GUIView_Update)(void*, float);
extern pfn_GUIView_Update g_sre_GUIView_Update;

// CharControllerComponent
typedef int (*pfn_CharController_CanUse)(void*);
typedef int (*pfn_CharController_CanPickup)(void*);
typedef void (*pfn_CharController_StartMovingToDirection)(void*, int);
typedef void (*pfn_CharController_StopMovingToDirection)(void*, int);
typedef void (*pfn_CharController_StartJumping)(void*);
typedef void (*pfn_CharController_StopJumping)(void*);
typedef int (*pfn_CharController_CanJump)(void*);
typedef int (*pfn_CharController_CanDoSomething)(void*);
typedef int (*pfn_CharController_CanBeginCasting)(void*);
typedef void (*pfn_CharController_Use)(void*);
typedef void (*pfn_CharController_Die)(void*);
typedef void (*pfn_CharController_Hurt)(void*);
typedef void (*pfn_CharController_Swing)(void*);
typedef void (*pfn_CharController_StopSwing)(void*);
typedef void (*pfn_CharController_DropQuickly)(void*);
typedef void (*pfn_CharController_CancelCasting)(void*);
typedef void (*pfn_CharController_FinishCasting)(void*);
typedef int (*pfn_CharController_CanSwing)(void*);

extern pfn_CharController_CanUse g_sre_CharController_CanUse;
extern pfn_CharController_CanPickup g_sre_CharController_CanPickup;
extern pfn_CharController_StartMovingToDirection g_sre_CharController_StartMovingToDirection;
extern pfn_CharController_StopMovingToDirection g_sre_CharController_StopMovingToDirection;
extern pfn_CharController_StartJumping g_sre_CharController_StartJumping;
extern pfn_CharController_StopJumping g_sre_CharController_StopJumping;
extern pfn_CharController_CanJump g_sre_CharController_CanJump;
extern pfn_CharController_CanDoSomething g_sre_CharController_CanDoSomething;
extern pfn_CharController_CanBeginCasting g_sre_CharController_CanBeginCasting;
extern pfn_CharController_Use g_sre_CharController_Use;
extern pfn_CharController_Die g_sre_CharController_Die;
extern pfn_CharController_Hurt g_sre_CharController_Hurt;
extern pfn_CharController_Swing g_sre_CharController_Swing;
extern pfn_CharController_StopSwing g_sre_CharController_StopSwing;
extern pfn_CharController_DropQuickly g_sre_CharController_DropQuickly;
extern pfn_CharController_CancelCasting g_sre_CharController_CancelCasting;
extern pfn_CharController_FinishCasting g_sre_CharController_FinishCasting;
extern pfn_CharController_CanSwing g_sre_CharController_CanSwing;

// GameSceneController
typedef int (*pfn_GameSceneController_CanCastSkill)(void*, void*);
extern pfn_GameSceneController_CanCastSkill g_sre_GameSceneController_CanCastSkill;

// GameSceneView
typedef void (*pfn_GameSceneView_HideCinematicSkipButton)(void*, int);
extern pfn_GameSceneView_HideCinematicSkipButton g_sre_GameSceneView_HideCinematicSkipButton;

/* =========================================================================
 * SwingableWeaponComponent glow helpers
 *
 * SetGlowColor:     _ZN5Caver24SwingableWeaponComponent12SetGlowColorENS_10FloatColorE
 *                   GhidraDecomp ARM64 address: 0x00228d01
 * SetGlowIntensity: _ZN5Caver24SwingableWeaponComponent16SetGlowIntensityEf
 *                   GhidraDecomp ARM64 address: 0x00228c49
 * ========================================================================= */
typedef void (*pfn_SwingableWeapon_SetGlowColor)(void* swc, SreFloatColor* color);
typedef void (*pfn_SwingableWeapon_SetGlowIntensity)(void* swc, float intensity);
extern pfn_SwingableWeapon_SetGlowColor    g_SwingableWeapon_SetGlowColor;
extern pfn_SwingableWeapon_SetGlowIntensity g_SwingableWeapon_SetGlowIntensity;

// CameraController shared globals & hook
extern int g_sre_cam_active;
extern float g_sre_cam_off_x;
extern float g_sre_cam_off_y;
extern float g_sre_cam_off_z;
extern float g_sre_cam_aspect;
extern void (*g_Camera_SetPerspectiveProjection)(void* camera, float fov, float aspect, float near, float far);
extern void (*g_orig_CameraController_Update)(void* self, float dt);
void sre_CameraController_Update(void* self, float dt);

/* =========================================================================
 * SceneObject field accessors  (ARM64 offsets from SwKiwi scene_object.c)
 * =========================================================================
 * Identifier string: SceneObject->0x2c (ptr)->0x50  (CppString* chain)
 * Speed float:       SceneObject->0x20 (ptr)->0x40
 * ========================================================================= */

/* Get identifier as C-string (reads SreString data pointer) */
static inline const char* sre_scene_object_identifier(SceneObject* obj) {
    if (!obj) return (void*)0;
    /* Read SreString object directly at offset 0x50 */
    SreString* str = (SreString*)((char*)obj + 0x50);
    return str->data;
}

static inline float sre_scene_object_get_speed(SceneObject* obj) {
    if (!obj) return 0.0f;
    /* Read float speed directly at offset 0x40 */
    return $F(float, obj, 0x40);
}

static inline void sre_scene_object_set_speed(SceneObject* obj, float speed) {
    if (!obj) return;
    /* Write float speed directly at offset 0x40 */
    $W(float, obj, 0x40, speed);
}

/* =========================================================================
 * Controller traversal helpers  (ARM64 offsets from SwKiwi program_state.c)
 *
 * gameController (Lua global "gameController") is a lightuserdata.
 * GameController+0xc8 → GameSceneController*
 * GameSceneController+0x8 → Hero SceneObject*
 * ========================================================================= */

/* Get the active GameController* from a Lua state (reads "gameController" global) */
static inline GameController* sre_game_controller_from_L(lua_State* L) {
    if (!L || !g_lua_getfield || !g_lua_touserdata || !g_lua_settop) return (void*)0;
    int top = g_lua_gettop(L);
    g_lua_getfield(L, LUA_GLOBALSINDEX, "gameController");
    void* gc = g_lua_touserdata(L, -1);
    g_lua_settop(L, top);
    return (GameController*)gc;
}

/* Get the active GameSceneController* (GameController+0xc8) */
static inline SceneController* sre_scene_controller_from_gc(GameController* gc) {
    if (!gc) return (void*)0;
    return (SceneController*)$F(void*, gc, 0xc8);
}

static inline SceneController* sre_scene_controller_from_L(lua_State* L) {
    return sre_scene_controller_from_gc(sre_game_controller_from_L(L));
}

/* Get hero SceneObject* from scene controller (SceneController+0x8) */
static inline SceneObject* sre_hero_object_from_sc(SceneController* sc) {
    if (!sc) return (void*)0;
    return (SceneObject*)$F(void*, sc, 0x8);
}

static inline SceneObject* sre_hero_object_from_L(lua_State* L) {
    return sre_hero_object_from_sc(sre_scene_controller_from_L(L));
}

/* =========================================================================
 * HealthComponent field accessors (ARM64)
 * hp = HealthComponent + 0x1c (float)
 * maxHp = HealthComponent + 0x20 (float)
 * ========================================================================= */
static inline float sre_health_get_hp(void* hc) {
    if (!hc) return 0.0f;
    return $F(float, hc, 0x1c);
}
static inline void sre_health_set_hp(void* hc, float hp) {
    if (!hc) return;
    $W(float, hc, 0x1c, hp);
}
static inline float sre_health_get_max_hp(void* hc) {
    if (!hc) return 0.0f;
    return $F(float, hc, 0x20);
}

/* =========================================================================
 * TransformComponent field accessors (ARM64)
 * position translation vector: Column 3 of Local Matrix at TransformComponent + 0xa0
 * ========================================================================= */
static inline void sre_transform_get_position(void* tc, float* x, float* y, float* z) {
    if (!tc) { *x = *y = *z = 0.0f; return; }
    *x = $F(float, tc, 0xa0);
    *y = $F(float, tc, 0xa4);
    *z = $F(float, tc, 0xa8);
}
static inline void sre_transform_set_position(void* tc, float x, float y, float z) {
    if (!tc) return;
    $W(float, tc, 0xa0, x);
    $W(float, tc, 0xa4, y);
    $W(float, tc, 0xa8, z);

    /* Synchronize coordinates with the parent SceneObject to maintain physics/collision consistency */
    void* obj = *(void**)((char*)tc + 0x28); /* Component + 0x28 is parent SceneObject* */
    if (obj) {
        $W(float, obj, 0x70, x);
        $W(float, obj, 0x74, y);
        $W(float, obj, 0x78, z);
    }
}

extern int g_sre_cam_active;
extern float g_sre_cam_off_x;
extern float g_sre_cam_off_y;
extern float g_sre_cam_off_z;
extern float g_sre_cam_aspect;
extern int g_sre_cam_pov_mode;
extern float g_sre_cam_pov_facing;

extern void (*g_orig_CameraController_Update)(void* self, float dt);
extern void (*g_Camera_SetPerspectiveProjection)(void* self, float fov, float aspect, float near, float far);

extern void (*g_orig_SceneGrid_UpdateVisibleAreasWithCamera)(void* self, void* camera);

void sre_caver_init(uint64_t swordigo_base);

#endif /* SRE_CAVER_H */
