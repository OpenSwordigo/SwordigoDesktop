/* ============================================================
 * sre_scene_update.c — GameSceneView::Update reimplementation
 * ============================================================
 * Complete reimplementation of Caver::GameSceneView::Update(float).
 * Calls every sub-function the original does, with non-atomic
 * shared_ptr refcounting where needed.
 *
 * Recovered from TVPG snapshot — this was the OLD sre_gui.c before
 * it was replaced by the Phase 2 GUI rendering stack.
 *
 * Function addresses from: nm -D libswordigo.so (v1.4.12 ARM64)
 * Pointer offsets from: Ghidra decompilation of GameSceneView::Update
 * ============================================================ */

#include "sre.h"
#include "sre_caver.h"

/* Swordigo base address — defined in sre_init.c */
extern uint64_t g_swordigo_base;

/* =========================================================================
 * sre_GUIView_Update — safe no-op
 * =========================================================================
 * GUIView::Update iterates child animations using boost::shared_ptr refcounts
 * with STXR/LDXR spin loops. It also dereferences animation list pointers
 * that may point into the files_dir string area (0x20010), causing a jump
 * into string data and a ReservedValue exception.
 * Safe to skip: gameplay, rendering, and input are all unaffected.
 */
void sre_GUIView_Update(void* self, float deltaTime) {
    (void)self; (void)deltaTime;
    /* Intentional no-op: animation system bypassed */
}

/* =========================================================================
 * sre_AchievementsManager_Update — safe no-op
 * =========================================================================
 * Contains STXR/LDXR loops for boost::shared_ptr notification queue.
 * Desktop has no achievement backend — safe to skip entirely.
 */
void sre_AchievementsManager_Update(void* self, float deltaTime) {
    (void)self; (void)deltaTime;
    /* Intentional no-op: no achievement backend on desktop */
}


/* =========================================================================
 * Function pointer types — matching ARM64 ABI calling convention
 * ========================================================================= */

/* Simple setters: self in X0, int/bool in X1 (W1) */
typedef void (*fn_void_self_int)(void* self, int val);
typedef void (*fn_void_self_bool)(void* self, int val);  /* bool passed as int in W1 */
typedef void (*fn_void_self_float)(void* self, float val);

/* Query functions: self in X0, return in W0 */
typedef int  (*fn_int_self)(void* self);

/* CanCastSkill: self in X0, shared_ptr ref in X1 */
typedef int  (*fn_int_self_ptr)(void* self, void* shared_ptr_ref);

/* =========================================================================
 * Function offsets — from nm (v1.4.12 ARM64)
 * ========================================================================= */

/* HealthBar */
#define OFF_HealthBar_SetMaxHealth       0x3d9b74
#define OFF_HealthBar_SetCurrentHealth   0x3d9b9c

/* ManaBar */
#define OFF_ManaBar_SetMaxMana           0x3df0a4
#define OFF_ManaBar_SetCurrentMana       0x3df178

/* CoinBar */
#define OFF_CoinBar_SetCurrentCoins      0x3c3acc

/* GameOverlayView */
#define OFF_GameOverlayView_SetControlsHidden      0x3d5394
#define OFF_GameOverlayView_SetShowsUseButton      0x3d56ac
#define OFF_GameOverlayView_SetSkillButtonDisabled  0x3d57c4

/* GUIEffect */
#define OFF_GUIEffect_FadeOut            0x4a6f54
#define OFF_GUIEffect_FadeIn             0x4a709c
#define OFF_GUIEffect_Update             0x4a6f88

/* GUIView */
#define OFF_GUIView_Update               0x49e33c

/* CharControllerComponent */
#define OFF_CharController_CanUse        0x25c950
#define OFF_CharController_CanPickup     0x25ca68

/* GameSceneController */
#define OFF_GameSceneController_CanCastSkill  0x34c2fc

/* GameSceneView (own methods we call on self) */
#define OFF_GameSceneView_HideCinematicSkipButton  0x34def4

/* =========================================================================
 * Helper: compute function pointer from offset
 * ========================================================================= */
#define FN(type, offset)  ((type)(g_swordigo_base + (offset)))

/* =========================================================================
 * GameSceneView object layout (from decompiled source)
 * =========================================================================
 *   +0x00   vtable pointer (GUIView base)
 *   +0xF0   shared_ptr<GameSceneController>  → px at +0xF0, pn at +0xF8
 *   +0x100  GameOverlayView* (raw ptr from shared_ptr px)
 *   +0x110  DebugInfoOverlay* (raw ptr from shared_ptr px)
 *   +0x120  NotificationView* (shared_ptr: px=+0x120, pn=+0x128)
 *   +0x130  ItemInfoPopupView* (shared_ptr: px=+0x130, pn=+0x138)
 *   +0x148  CinematicSkipButton* (GUIButton/GUIView*)
 *   +0x158  byte: cinematic skip button visible flag
 *   +0x15C  float: cinematic skip button timer
 *   +0x160  GUIEffect* (screen effect 2)
 *   +0x170  GUIEffect* (damage flash — red overlay)
 *   +0x188  GUIEffect* (cinematic bars)
 *   +0x198  byte: wireframe/combat flag  
 *   +0x1A2  byte: wireframe render flag
 *
 * GameSceneController layout:
 *   +0x08   GameState* (from shared_ptr px)
 *   +0x20   something → +0x20 = enemy count related
 *   +0xE0   CharControllerComponent* (player)
 *
 * GameState layout (CharacterState fields — protobuf order):
 *   +0x68   current Skill shared_ptr px
 *   +0x70   current Skill shared_ptr pn (shared_count*)
 *   +0xA8   currentHP (int)            — proto field 2
 *   +0xAC   currentMana (int)          — proto field 3
 *   +0xB0   coins (int)               — proto field 4
 *   +0xB4   experiencePoints (int)     — proto field 5
 *   +0xB8   experienceLevel (int)      — proto field 6
 *   +0xBC   healthAttribute (int)      — proto field 15
 *   +0xC0   attackAttribute (int)      — proto field 16
 *   +0xC4   magicAttribute (int)       — proto field 17
 *   +0x1CA  coinDoublerShift (byte)
 *
 * GameOverlayView layout:
 *   +0x1E8  HealthBar* (shared_ptr px)
 *   +0x1F8  ManaBar* (shared_ptr px)
 *   +0x238  CoinBar* (shared_ptr px)
 */

/* ========== Player Stats — Exported to Host ========== */
volatile int g_sre_player_hp = 0;
volatile int g_sre_player_max_hp = 0;
volatile int g_sre_player_mana = 0;
volatile int g_sre_player_max_mana = 0;
volatile int g_sre_player_coins = 0;
volatile int g_sre_player_hp_level = 0;
volatile int g_sre_player_mana_level = 0;
volatile int g_sre_player_xp = 0;
volatile int g_sre_player_level = 0;
volatile int g_sre_player_atk_level = 0;

/* Player Position — Exported to Host for GLES2 Lighting */
volatile float g_sre_hero_pos_x = 0.0f;
volatile float g_sre_hero_pos_y = 0.0f;
volatile float g_sre_hero_pos_z = 0.0f;

/* Relay pointer to original RenderingContext constructor */
uint64_t g_orig_RenderingContext_C1 = 0;

/* Hook: Force GLES 2.0 API Mode */
void sre_RenderingContext_C1(void* self, int api_mode) {
    // Call the original constructor through relay stub forcing GLES 2.0 (api_mode = 1)
    typedef void (*fn_Ctor)(void*, int);
    ((fn_Ctor)g_orig_RenderingContext_C1)(self, 1);
}

/* Scene state flags */
volatile int g_sre_gui_scene_active = 0;
volatile uint64_t g_sre_gui_scene_view_ptr = 0;

/* GameState pointer — exported so host can directly read/write game state.
 * This is a GUEST pointer (offset from guest memory base).
 * Host reads as: *(int*)(g_guest_memory + gamestate_ptr + OFFSET) */
volatile uint64_t g_sre_gamestate_ptr = 0;
volatile uint64_t g_sre_hero_obj = 0;
volatile uint64_t g_sre_hero_health_comp = 0;
volatile uint64_t g_sre_hero_mana_comp = 0;

/* =========================================================================
 * Non-atomic shared_ptr refcount helpers
 * =========================================================================
 * boost::shared_ptr uses LDAXR/STLXR for refcounting which spins in Unicorn.
 * We do it non-atomically (single-threaded anyway).
 *
 * shared_count (sp_counted_base) layout:
 *   +0x00  vtable
 *   +0x08  use_count (long)
 *   +0x10  weak_count (long)
 */
static void sp_addref(void* pn) {
    if (pn) {
        int64_t* use_count = (int64_t*)((char*)pn + 0x08);
        (*use_count)++;
    }
}

static void sp_release(void* pn) {
    if (!pn) return;
    int64_t* use_count = (int64_t*)((char*)pn + 0x08);
    (*use_count)--;
    if (*use_count == 0) {
        /* Call virtual dispose() — vtable[2] (offset 0x10) */
        uint64_t vtable = *(uint64_t*)pn;
        typedef void (*fn_dispose)(void*);
        fn_dispose dispose = (fn_dispose)(*(uint64_t*)(vtable + 0x10));
        dispose(pn);
        /* Decrement weak_count */
        int64_t* weak_count = (int64_t*)((char*)pn + 0x10);
        (*weak_count)--;
        if (*weak_count == 0) {
            /* Call virtual destroy() — vtable[3] (offset 0x18) */
            typedef void (*fn_destroy)(void*);
            fn_destroy destroy = (fn_destroy)(*(uint64_t*)(vtable + 0x18));
            destroy(pn);
        }
    }
}

/* =========================================================================
 * sre_GameSceneView_Update — FULL reimplementation
 * =========================================================================
 * ARM64 ABI: X0 = this, S0 = float deltaTime
 */
void sre_GameSceneView_Update(void* self, float deltaTime) {
    char* this_ = (char*)self;
    
    /* Store scene view pointer for host */
    g_sre_gui_scene_view_ptr = (uint64_t)self;
    g_sre_gui_scene_active = 1;
    
    /* ---- Read core pointers ---- */
    uint64_t overlay_ptr = *(uint64_t*)(this_ + 0x100);  /* GameOverlayView* */
    uint64_t ctrl_ptr    = *(uint64_t*)(this_ + 0xF0);   /* GameSceneController* */
    
    if (overlay_ptr == 0 || ctrl_ptr == 0) goto do_effects;
    
    uint64_t gamestate = *(uint64_t*)(ctrl_ptr + 0x08);   /* GameState* */
    if (gamestate == 0) goto do_effects;
    
    /* Export gamestate pointer for host-side modding */
    g_sre_gamestate_ptr = (uint64_t)gamestate;

    /* Synchronize global camera coordinates using hero position fallback disabled */
    if (ctrl_ptr != 0) {
        void* hero = *(void**)(ctrl_ptr + 0xd8); // Hero SceneObject*
        g_sre_hero_obj = (uint64_t)hero;
        if (hero != 0) {
            g_sre_hero_pos_x = *(float*)((char*)hero + 0x70);
            g_sre_hero_pos_y = *(float*)((char*)hero + 0x74);
            g_sre_hero_pos_z = *(float*)((char*)hero + 0x78);

            if (g_SceneObject_ComponentWithInterface && HealthComponent_Interface) {
                g_sre_hero_health_comp = (uint64_t)g_SceneObject_ComponentWithInterface(hero, HealthComponent_Interface);
            } else {
                g_sre_hero_health_comp = 0;
            }

            if (g_SceneObject_ComponentWithInterface && ManaComponent_Interface) {
                g_sre_hero_mana_comp = (uint64_t)g_SceneObject_ComponentWithInterface(hero, ManaComponent_Interface);
            } else {
                g_sre_hero_mana_comp = 0;
            }
        } else {
            g_sre_hero_health_comp = 0;
            g_sre_hero_mana_comp = 0;
        }
    }
    
    /* ---- 1. HEALTH BAR ---- */
    {
        uint64_t health_bar = *(uint64_t*)(overlay_ptr + 0x1E8); /* HealthBar* */
        if (health_bar != 0) {
            int hp_level = *(int*)(gamestate + 0xBC);
            int max_hp   = hp_level * 2 + 4;
            int cur_hp   = *(int*)(gamestate + 0xA8);
            
            if (g_sre_HealthBar_SetMaxHealth) g_sre_HealthBar_SetMaxHealth((void*)health_bar, max_hp);
            
            /* Damage flash: if HP decreased, flash red */
            int prev_hp = *(int*)((char*)health_bar + 0x110); /* HealthBar::currentHealth */
            if (cur_hp < prev_hp) {
                uint64_t dmg_effect = *(uint64_t*)(this_ + 0x170);
                if (dmg_effect != 0) {
                    /* Set damage flash color to red: RGBA = 0xCC0000CC */
                    *(uint32_t*)((char*)dmg_effect + 0x48) = 0xCC0000CC;
                    if (g_sre_GUIEffect_FadeOut) g_sre_GUIEffect_FadeOut((void*)dmg_effect, 0.0f);
                    if (g_sre_GUIEffect_FadeIn) g_sre_GUIEffect_FadeIn((void*)dmg_effect, 0.6f);
                }
                /* Re-read HP after potential effect calls */
                cur_hp = *(int*)(gamestate + 0xA8);
                health_bar = *(uint64_t*)(*(uint64_t*)(this_ + 0x100) + 0x1E8);
            }
            
            if (g_sre_HealthBar_SetCurrentHealth) g_sre_HealthBar_SetCurrentHealth((void*)health_bar, cur_hp);
            
            /* Export to host */
            g_sre_player_hp       = cur_hp;
            g_sre_player_max_hp   = max_hp;
            g_sre_player_hp_level = hp_level;
            g_sre_player_xp       = *(int*)(gamestate + 0xB4);  /* ExperiencePoints */
            g_sre_player_level    = *(int*)(gamestate + 0xB8);  /* ExperienceLevel */
            g_sre_player_atk_level = *(int*)(gamestate + 0xC0); /* AttackAttribute */
        }
        
        /* Re-read overlay_ptr (may have been used by sub-calls) */
        overlay_ptr = *(uint64_t*)(this_ + 0x100);
    }
    
    /* ---- 2. MANA BAR ---- */
    {
        uint64_t mana_bar = *(uint64_t*)(overlay_ptr + 0x1F8); /* ManaBar* */
        if (mana_bar != 0) {
            int mana_level = *(int*)(gamestate + 0xC4);
            int max_mana   = mana_level * 20 + 10;
            int cur_mana   = *(int*)(gamestate + 0xAC);
            
            if (g_sre_ManaBar_SetMaxMana) g_sre_ManaBar_SetMaxMana((void*)mana_bar, max_mana);
            if (g_sre_ManaBar_SetCurrentMana) {
                g_sre_ManaBar_SetCurrentMana(
                    (void*)*(uint64_t*)(*(uint64_t*)(this_ + 0x100) + 0x1F8), cur_mana);
            }
            
            /* Export to host */
            g_sre_player_mana       = cur_mana;
            g_sre_player_max_mana   = max_mana;
                       /* ---- 2b. SKILL BUTTON (CanCastSkill) ---- */
            /* Read the current skill shared_ptr from GameState */
            uint64_t skill_px = *(uint64_t*)(gamestate + 0x68);
            void*    skill_pn = *(void**)(gamestate + 0x70);
            
            if (skill_px != 0) {
                /* Build a local shared_ptr on the stack for CanCastSkill */
                uint64_t skill2_px = *(uint64_t*)(gamestate + 0x68);
                void*    skill2_pn = *(void**)(gamestate + 0x70);
                sp_addref(skill2_pn);  /* increment use_count (non-atomic) */
                
                /* Local shared_ptr: { px, pn } */
                uint64_t local_sp[2];
                local_sp[0] = skill2_px;
                local_sp[1] = (uint64_t)skill2_pn;
                
                /* Call CanCastSkill */
                void* controller = (void*)*(uint64_t*)(this_ + 0xF0);
                void* game_overlay = (void*)*(uint64_t*)(this_ + 0x100);
                int can_cast = g_sre_GameSceneController_CanCastSkill ? g_sre_GameSceneController_CanCastSkill(
                    controller, (void*)local_sp) : 0;
                
                /* SetSkillButtonDisabled(overlay, !can_cast) */
                if (g_sre_GameOverlayView_SetSkillButtonDisabled) {
                    g_sre_GameOverlayView_SetSkillButtonDisabled(
                        game_overlay, (can_cast & 1) ^ 1);
                }
                
                /* Release local shared_ptr (non-atomic) */
                sp_release(skill2_pn);
            }
        }
    }
    
    /* ---- 3. COIN BAR — smart visibility ---- */
    /* Rules:
     * - ALWAYS visible in shops (music = "house" or "squire")
     * - Shows for 3s when coins change (pickup)
     * - Shows for 2s on world transition or leaving shop
     * - Hidden otherwise (catch-all enforces this)
     *
     * GUIView hidden flag at offset 0xE4 (0 = visible, 1 = hidden).
     * Music playlist name from g_sre_music_load_name (sre_music.c) */
    #define OFF_GUIVIEW_HIDDEN   0xE4
    #define COIN_SHOW_SECONDS    3.0f
    #define COIN_TRANSITION_SHOW 2.0f
    {
        extern char g_sre_music_load_name[256];
        
        static int      prev_coins    = -1;
        static float    coin_timer    = 0.0f;
        static uint64_t prev_coin_bar = 0;
        static int      was_in_shop   = 0;
        
        uint64_t coin_bar = *(uint64_t*)(*(uint64_t*)(this_ + 0x100) + 0x238);
        if (coin_bar != 0) {
            int coins = *(int*)(gamestate + 0xB0);
            if (coins == 0 && g_sre_player_coins > 0) {
                /* Neglect zero-reset of coins, write back last known coins */
                *(int*)(gamestate + 0xB0) = g_sre_player_coins;
                coins = g_sre_player_coins;
            }
            if (g_sre_CoinBar_SetCurrentCoins) g_sre_CoinBar_SetCurrentCoins((void*)coin_bar, coins);
            g_sre_player_coins = coins;
            
            /* Check if we're in a shop area (house/town music) */
            int in_shop = 0;
            {
                char* m = g_sre_music_load_name;
                if (m[0]=='h' && m[1]=='o' && m[2]=='u' && m[3]=='s' && m[4]=='e' && m[5]==0)
                    in_shop = 1;
                if (m[0]=='s' && m[1]=='q' && m[2]=='u' && m[3]=='i' && m[4]=='r' && m[5]=='e')
                    in_shop = 1;
            }
            
            /* Determine visibility */
            if (in_shop) {
                /* In shop — ALWAYS show coins */
                *(uint8_t*)((char*)coin_bar + OFF_GUIVIEW_HIDDEN) = 0;
                coin_timer = 0.0f;
                was_in_shop = 1;
            } else if (was_in_shop) {
                /* Just LEFT the shop — show briefly then hide */
                was_in_shop = 0;
                coin_timer = COIN_TRANSITION_SHOW;
                /* keep visible during transition */
            } else if (coin_bar != prev_coin_bar && prev_coins >= 0) {
                /* New CoinBar (world transition) — show briefly */
                *(uint8_t*)((char*)coin_bar + OFF_GUIVIEW_HIDDEN) = 0;
                coin_timer = COIN_TRANSITION_SHOW;
            } else if (prev_coins >= 0 && coins != prev_coins) {
                /* Coins changed — show! */
                *(uint8_t*)((char*)coin_bar + OFF_GUIVIEW_HIDDEN) = 0;
                coin_timer = COIN_SHOW_SECONDS;
            } else if (coin_timer > 0.0f) {
                /* Timer running — count down */
                coin_timer -= deltaTime;
                if (coin_timer <= 0.0f) {
                    *(uint8_t*)((char*)coin_bar + OFF_GUIVIEW_HIDDEN) = 1;
                    coin_timer = 0.0f;
                }
            } else {
                /* CATCH-ALL: no reason to show → force hide.
                 * This covers: first load, leaving shop after timer,
                 * and any weird edge cases. */
                *(uint8_t*)((char*)coin_bar + OFF_GUIVIEW_HIDDEN) = 1;
            }
            
            prev_coins = coins;
            prev_coin_bar = coin_bar;
        }
    }
    
    /* ---- 4. CONTROLS VISIBILITY ---- */
    /* Original: hide controls if enemy_count >= 1 OR wireframe combat flag set */
    {
        uint64_t scene_data = *(uint64_t*)(ctrl_ptr + 0x20);
        int enemy_count = (scene_data != 0) ? *(int*)(scene_data + 0x20) : 0;
        int combat_flag = (int)(uint8_t)this_[0x198];
        
        void* overlay = (void*)*(uint64_t*)(this_ + 0x100);
        int hide = (enemy_count >= 1 || combat_flag != 0) ? 1 : 0;
        if (g_sre_GameOverlayView_SetControlsHidden) g_sre_GameOverlayView_SetControlsHidden(overlay, hide);
    }
    
    /* ---- 5. USE/PICKUP BUTTON ---- */
    {
        uint64_t char_ctrl = *(uint64_t*)(ctrl_ptr + 0xE0);
        if (char_ctrl != 0) {
            void* overlay = (void*)*(uint64_t*)(this_ + 0x100);
            int can_interact;
            
            int can_pickup = g_sre_CharController_CanPickup ? g_sre_CharController_CanPickup((void*)char_ctrl) : 0;
            if (can_pickup & 1) {
                can_interact = 1;
            } else {
                can_interact = g_sre_CharController_CanUse ? (g_sre_CharController_CanUse((void*)char_ctrl) & 1) : 0;
            }
            
            if (g_sre_GameOverlayView_SetShowsUseButton) g_sre_GameOverlayView_SetShowsUseButton(overlay, can_interact);
        }
    }
    
    /* ---- 6. CINEMATIC SKIP BUTTON TIMER ---- */
    {
        uint8_t skip_visible = (uint8_t)this_[0x158];
        if (skip_visible != 0) {
            float* timer = (float*)(this_ + 0x15C);
            *timer -= deltaTime;
            if (*timer <= 0.0f) {
                /* Call HideCinematicSkipButton(this, true) */
                if (g_sre_GameSceneView_HideCinematicSkipButton) g_sre_GameSceneView_HideCinematicSkipButton(self, 1);
            }
        }
    }
 
do_effects:
    /* ---- 7. GUI EFFECT UPDATES ---- */
    /* Cinematic bars effect */
    {
        uint64_t effect_bars = *(uint64_t*)(this_ + 0x188);
        if (effect_bars != 0) {
            if (g_sre_GUIEffect_Update) g_sre_GUIEffect_Update((void*)effect_bars, deltaTime);
        }
    }
    /* Screen effect 2 */
    {
        uint64_t effect2 = *(uint64_t*)(this_ + 0x160);
        if (effect2 != 0) {
            if (g_sre_GUIEffect_Update) g_sre_GUIEffect_Update((void*)effect2, deltaTime);
        }
    }
    /* Damage flash effect */
    {
        uint64_t dmg_flash = *(uint64_t*)(this_ + 0x170);
        if (dmg_flash != 0) {
            if (g_sre_GUIEffect_Update) g_sre_GUIEffect_Update((void*)dmg_flash, deltaTime);
        }
    }
    if (g_sre_GUIView_Update) g_sre_GUIView_Update(self, deltaTime);
    /* GUIView::Update (animation system) is now hooked as sre_GUIView_Update
     * (safe no-op) — no direct call needed here. */
}
