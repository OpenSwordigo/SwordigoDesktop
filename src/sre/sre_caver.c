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
pfn_SceneObject_InitWithTemplate        g_SceneObject_InitWithTemplate        = 0;
pfn_ProgramState_FromLuaState          g_ProgramState_FromLuaState          = 0;
pfn_CreateHeroObjectAt                 g_sre_CreateHeroObjectAt              = 0;

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

/* Camera controller shared variables & function pointers */
int g_sre_cam_active = 0;
float g_sre_cam_off_x = 0.0f;
float g_sre_cam_off_y = 0.0f;
float g_sre_cam_off_z = 0.0f;
float g_sre_cam_aspect = 1.777778f;
int g_sre_cam_pov_mode = 0;
float g_sre_cam_pov_facing = 1.0f;
void (*g_Camera_SetPerspectiveProjection)(void* camera, float fov, float aspect, float near, float far) = 0;
void (*g_orig_CameraController_Update)(void* self, float dt) = 0;
void (*g_orig_SceneGrid_UpdateVisibleAreasWithCamera)(void* self, void* camera) = 0;

/* SwingableWeaponComponent glow function pointers
 * Addresses from GhidraDecomp (ARM64, libswordigo_v1.4.12.so, base 0x0):
 *   SetGlowIntensity  0x00228c49
 *   SetGlowColor      0x00228d01
 */
pfn_SwingableWeapon_SetGlowColor     g_SwingableWeapon_SetGlowColor     = 0;
pfn_SwingableWeapon_SetGlowIntensity g_SwingableWeapon_SetGlowIntensity = 0;

/* ModelComponent::setModelName — Mini.SetModelName */
void (*g_sre_ModelComponent_setModelName)(void* component, SreString* name) = 0;

/* EntityControllerComponent::SetMoveSpeed — Mini.SetObjectSpeed */
void (*g_sre_EntityControllerComponent_SetMoveSpeed)(void* component, float speed) = 0;

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
    /* All component interfaces and helper pointers are dynamically resolved
     * and injected by the host loader in main.cpp before sre_init is called.
     * We no longer use hardcoded offsets here to prevent stack crashes. */
    (void)swordigo_base;
}

/* Our guest-side CameraController::Update hook function */
void sre_CameraController_Update(void* self, float dt) {
    if (g_orig_CameraController_Update) {
        g_orig_CameraController_Update(self, dt);
    }

    static int s_was_active = 0;
    static float s_vanilla_fov = 0.34906584f;
    static float s_vanilla_aspect = 1.0f;
    static float s_vanilla_near = 50.0f;
    static float s_vanilla_far = 20000.0f;
    static float s_vanilla_offset_x = 0.0f;
    static float s_vanilla_offset_y = 0.0f;
    static float s_vanilla_offset_z = 1000.0f;
    static float s_vanilla_qx = 0.0f;
    static float s_vanilla_qy = 0.0f;
    static float s_vanilla_qz = 0.0f;
    static float s_vanilla_qw = 1.0f;

    if (g_sre_cam_active) {
        void* camera = *(void**)((char*)self + 0x58);
        if (camera) {
            // Save vanilla camera parameters on transition to active
            if (!s_was_active) {
                s_vanilla_fov = *(float*)((char*)camera + 0xf4);
                s_vanilla_aspect = *(float*)((char*)camera + 0xf0);
                s_vanilla_near = *(float*)((char*)camera + 0xf8);
                s_vanilla_far = *(float*)((char*)camera + 0xfc);
                s_vanilla_offset_x = *(float*)((char*)self + 0x04);
                s_vanilla_offset_y = *(float*)((char*)self + 0x08);
                s_vanilla_offset_z = *(float*)((char*)self + 0x0c);
                s_vanilla_qx = *(float*)((char*)camera + 0x1c);
                s_vanilla_qy = *(float*)((char*)camera + 0x20);
                s_vanilla_qz = *(float*)((char*)camera + 0x24);
                s_vanilla_qw = *(float*)((char*)camera + 0x28);
            }
            s_was_active = 1;

            if (g_sre_cam_pov_mode) {
                extern volatile float g_sre_hero_pos_x;
                extern volatile float g_sre_hero_pos_y;
                extern volatile float g_sre_hero_pos_z;

                float cam_x = g_sre_hero_pos_x;
                float cam_y = g_sre_hero_pos_y + 75.0f; // Eye level height
                float cam_z = g_sre_hero_pos_z;

                *(float*)((char*)camera + 0x10) = cam_x;
                *(float*)((char*)camera + 0x14) = cam_y;
                *(float*)((char*)camera + 0x18) = cam_z;

                // Rotate around Y axis to face left/right
                float qx = 0.0f;
                float qy = (g_sre_cam_pov_facing > 0.0f) ? -0.70710678f : 0.70710678f;
                float qz = 0.0f;
                float qw = 0.70710678f;

                *(float*)((char*)camera + 0x1c) = qx;
                *(float*)((char*)camera + 0x20) = qy;
                *(float*)((char*)camera + 0x24) = qz;
                *(float*)((char*)camera + 0x28) = qw;

                if (g_Camera_SetPerspectiveProjection) {
                    // First-person perspective FOV (approx 70 degrees)
                    g_Camera_SetPerspectiveProjection(camera, 1.22173f, g_sre_cam_aspect, 50.0f, 20000.0f);
                }
            } else {
                float zoom = 1.0f + g_sre_cam_off_z / 600.0f;
                if (zoom < 0.15f) zoom = 0.15f;
                if (zoom > 5.0f) zoom = 5.0f;

                float offset_x = zoom * g_sre_cam_off_x;
                float offset_y = zoom * (300.0f + g_sre_cam_off_y);
                float offset_z = zoom * 600.0f;

                *(float*)((char*)self + 0x04) = offset_x;
                *(float*)((char*)self + 0x08) = offset_y;
                *(float*)((char*)self + 0x0c) = offset_z;

                *(float*)((char*)self + 0x48) = 0.0f; // up_x
                *(float*)((char*)self + 0x4c) = 1.0f; // up_y
                *(float*)((char*)self + 0x50) = 0.0f; // up_z

                if (g_Camera_SetPerspectiveProjection) {
                    // 45 degrees FOV = 0.78539816f rad
                    g_Camera_SetPerspectiveProjection(camera, 0.78539816f, g_sre_cam_aspect, 50.0f, 20000.0f);
                }
            }
        }
    } else if (s_was_active) {
        s_was_active = 0;
        void* camera = *(void**)((char*)self + 0x58);
        if (camera) {
            // Restore vanilla level camera offsets
            *(float*)((char*)self + 0x04) = s_vanilla_offset_x;
            *(float*)((char*)self + 0x08) = s_vanilla_offset_y;
            *(float*)((char*)self + 0x0c) = s_vanilla_offset_z;

            *(float*)((char*)self + 0x48) = 0.0f;
            *(float*)((char*)self + 0x4c) = 1.0f;
            *(float*)((char*)self + 0x50) = 0.0f;

            // Restore vanilla camera orientation
            *(float*)((char*)camera + 0x1c) = s_vanilla_qx;
            *(float*)((char*)camera + 0x20) = s_vanilla_qy;
            *(float*)((char*)camera + 0x24) = s_vanilla_qz;
            *(float*)((char*)camera + 0x28) = s_vanilla_qw;

            // Restore the exact vanilla projection parameters saved before the override
            if (g_Camera_SetPerspectiveProjection) {
                g_Camera_SetPerspectiveProjection(camera, s_vanilla_fov, s_vanilla_aspect, s_vanilla_near, s_vanilla_far);
            }
        }
    }
}

/* Our guest-side SceneGrid::UpdateVisibleAreasWithCamera hook function */
void sre_SceneGrid_UpdateVisibleAreasWithCamera(void* self, void* camera) {
    if (g_orig_SceneGrid_UpdateVisibleAreasWithCamera) {
        g_orig_SceneGrid_UpdateVisibleAreasWithCamera(self, camera);
    }

    if (g_sre_cam_active) {
        int layer_count = *(int*)self;
        if (layer_count > 0) {
            char** array = *(char***)((char*)self + 8);
            if (array) {
                for (int i = 0; i < layer_count; i++) {
                    char* layer = array[i * 2]; // boost::shared_ptr is 16 bytes, index by i*2
                    if (layer) {
                        *(float*)(layer + 0x38) = -1000000.0f; // x
                        *(float*)(layer + 0x3c) = -1000000.0f; // y
                        *(float*)(layer + 0x40) = 2000000.0f;  // width
                        *(float*)(layer + 0x44) = 2000000.0f;  // height
                    }
                }
            }
        }
    }
}

/* ========== AudioSystem::EndAudioInterruptionIfNecessary no-op ==========
 * nm -D v1.4.12 arm64-v8a offset: 0x47ef50
 *   _ZN5Caver11AudioSystem31EndAudioInterruptionIfNecessaryEv
 *   (confirmed from crash label: 0x147ef50 [_ZN5Caver11AudioSystem31EndAudioInterruptionIfNecessaryEv])
 *
 * IDA: iOS audio session resumption — calls SetAudioSessionActive(1),
 * alcMakeContextCurrent, alcProcessContext inside a canary-guarded frame.
 * The Dynarmic guest/host context switches for these JNI calls change
 * TPIDR_EL0, causing the canary comparison at function exit to always fail
 * → __stack_chk_fail → recovery loop.
 * On desktop, iOS audio session interruption management is irrelevant.
 * Return 'self' immediately (same as the function's early-out when no
 * interruption is active: `if (!this[128]) return this;`).
 */
void* sre_AudioSystem_EndAudioInterruptionIfNecessary(void* self) {
    return self;
}

/* ========== __stack_chk_fail replacement ==========
 * PLT stub: 0x1F62D0 (IDA-confirmed: EndAudioInterruptionIfNecessary callee list
 *   entry "0x00000000001F62D0 — .__stack_chk_fail")
 * Real symbol: 0x6FE6F8 (__stack_chk_fail in dynamic segment)
 *
 * Why this fires: ARM64 stack canary is read from TPIDR_EL0+40 at function
 * entry and compared again at exit.  JNI bridge calls (alcMakeContextCurrent,
 * SetAudioSessionActive, etc.) inside EndAudioInterruptionIfNecessary switch
 * between guest and host Dynarmic contexts, changing TPIDR_EL0.  The saved
 * canary no longer matches → __stack_chk_fail is called every time.
 *
 * The current SRE recovery "unwinds to LR" via longjmp, which may leave
 * callee-saved registers (X19 = 'this', etc.) in undefined state.
 *
 * This replacement just returns to LR.  Since __stack_chk_fail is called
 * right before the function epilogue:
 *   BL  __stack_chk_fail   ; LR = 0x147eebc
 *   ; 0x147eebc: shared epilogue (gets MusicPlayer, calls SetSuspended, RET)
 * returning to LR drops straight into the epilogue with the full stack frame
 * still intact (callee-saved regs on stack, SP unchanged).  The epilogue
 * restores everything cleanly.  Canary mismatch is silently ignored.
 *
 * NOTE: EndAudioInterruptionIfNecessary is ALSO hooked at 0x47ED50 so it
 * never reaches __stack_chk_fail in the first place.  This hook is
 * belt-and-suspenders for any OTHER function whose canary check fails due
 * to the same TPIDR_EL0 drift.
 */
void sre_stack_chk_fail(void) {
    /* Return to LR — all registers preserved, epilogue runs cleanly. */
}
