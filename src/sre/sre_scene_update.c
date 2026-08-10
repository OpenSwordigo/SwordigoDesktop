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

/* =========================================================================
 * Scene Loading State — guards coroutine execution during level transitions
 *
 * During Scene::FinishLoad / SceneLoadingView::InitWithGameState, hundreds
 * of SceneObjects run FinishLoad which starts Lua scripts. Any Lua error
 * during this phase causes sre_cxa_throw → longjmp which can escape a
 * pthread_mutex_lock that a parent ProgramState::Update holds, leaving
 * the mutex permanently locked → game deadlocks on next frame.
 *
 * Fix: set g_sre_scene_loading=1 while loading. sre_ProgramState_Update
 * checks this and skips lua_resume entirely when set, preventing the
 * mutex from ever being taken during scene load.
 * ========================================================================= */
volatile int g_sre_scene_loading = 0;  /* 1 = scene load in progress */
/* Watchdog suppressor: when set > 0, sre_GameSceneView_Update skips its HUD
 * data export for that many frames. The scene-load watchdog (main.cpp) sets
 * this to 2 whenever it force-clears g_sre_scene_loading so that we don't
 * spam stale game-state reads during the frame where the new scene is activating. */
volatile int g_sre_suppress_hud_frames = 0;

/* Extern for force-unlocking the Lua mutex after a scene load completes.
 * The mutex may have been left locked by a longjmp escape during loading.
 * We always unlock once after FinishLoad regardless of lock count. */
extern int pthread_mutex_unlock(void *mutex);
extern void *g_lua_mutex_ptr;

/* Swordigo base address & Lua recovery externs */
extern uint64_t g_swordigo_base;
extern lua_State* g_sre_last_lua_state;
/* recovery_push / recovery_pop / g_sre_recovery_stack / sre_setjmp:
 * declared in sre_lua.h and defined (non-static) in sre_lua.c.
 * Using sre_lua.h instead of bare externs ensures the linker resolves these
 * symbols within libsre.so rather than falling back to the bridge PLT stub. */
#include "sre_lua.h"
typedef struct { unsigned long buf[22]; } SreJmpBufRec;
extern SreJmpBufRec g_sre_recovery_stack[];
extern int sre_setjmp(void* env);

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

/* Hook: Pass through original RenderingContext API mode */
void* sre_RenderingContext_C1(void* self, int api_mode) {
    typedef void* (*fn_Ctor)(void*, int);
    if (g_orig_RenderingContext_C1) {
        return ((fn_Ctor)g_orig_RenderingContext_C1)(self, api_mode);
    }
    return self;
}

/* Scene state flags */
volatile int g_sre_gui_scene_active = 0;
volatile uint64_t g_sre_gui_scene_view_ptr = 0;
void* g_sre_gamesceneview_ptr = NULL;

/* ── Per-frame world-update drive guard ──────────────────────────────────────
 * The original update pipeline ticks the simulation through
 *   GameSceneView::Update → GameSceneController::Update → Scene::Update
 * (see sre_frame_loop.c header). Our GameSceneView reimplementation mirrors
 * only the HUD half, so Scene::Update — which owns the per-object component
 * updates, physics/collision and the DEFERRED DELETION loop that removes
 * objects after their timed destruction window — was never ticked in SRE
 * builds. We drive GameSceneController::Update once per frame from the view
 * update, and use Scene's frame counter (Scene+0x310, incremented at the top
 * of Scene::Update) to detect whether the original chain already ticked the
 * scene this frame, so we never double-step physics. */
static uint64_t s_sre_last_driven_scene  = 0;
static int      s_sre_last_driven_frame  = -1;
/* Total number of frames we actually drove GameSceneController::Update.
 * Exported to the host and printed with the frame diagnostics so we can
 * verify the world-simulation drive is active without spamming stderr. */
volatile uint64_t g_sre_world_drive_count = 0;

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

/* Valid vtable ranges for libswordigo.so guest objects (see sre_gui_nav.c).
 * Used to stop scene-object destruction from branching into .dynstr or freed
 * heap memory when a refcount block has been partially reclaimed. */
static int sp_vtable_is_valid(uint64_t vtable) {
    if (!vtable || (vtable & 7) != 0) return 0;
    if (vtable >= 0x16b6a80ULL && vtable < 0x16DC000ULL) return 1; /* .data.rel.ro */
    if (vtable >= 0x1583480ULL && vtable < 0x15A2000ULL) return 1; /* .rodata */
    /* SRE relay-cave vtables (TrampolineMgr arena 0x3000000–0x3100000) */
    if (vtable >= 0x3000000ULL && vtable < 0x3100000ULL) return 1;
    return 0;
}

static int sp_code_is_valid(uint64_t fn) {
    if (!fn || (fn & 3) != 0) return 0;
    if (fn >= 0x1203e90ULL && fn < 0x1584000ULL) return 1;      /* .text */
    if (fn >= 0x3000000ULL && fn < 0x3100000ULL) return 1;      /* relay arena */
    if (fn >= 0x2000000ULL && fn < 0x2300000ULL) return 1;      /* libsre.so guest */
    return 0;
}

static void sp_release(void* pn) {
    if (!pn) return;
    int64_t* use_count = (int64_t*)((char*)pn + 0x08);
    (*use_count)--;
    if (*use_count == 0) {
        uint64_t vtable = *(uint64_t*)pn;

        /* SCENE-OBJECT DESTRUCTION GUARD: never call virtual dispose()/destroy()
         * through a corrupt or stale vtable — that was a recurring source of
         * NoExecuteFault crashes during scene unload (PC landing in .dynstr).
         * If the vtable looks bad, zero the counts and leak the block instead
         * of branching into garbage. */
        if (!sp_vtable_is_valid(vtable)) {
            fprintf(stderr, "[SRE/SceneObject] sp_release: corrupt vtable=0x%llx on refcount block %p — "
                            "skipping dispose/destroy (counts zeroed)\n",
                    (unsigned long long)vtable, pn);
            *use_count = 0;
            *(int64_t*)((char*)pn + 0x10) = 0;
            return;
        }

        /* Call virtual dispose() — vtable[2] (offset 0x10).
         * NOTE: dispose() may free 'pn' or alter its refcount fields.
         * We capture use_count/weak_count BEFORE the call and re-validate after. */
        typedef void (*fn_dispose)(void*);
        fn_dispose dispose = (fn_dispose)(*(uint64_t*)(vtable + 0x10));
        if (sp_code_is_valid((uint64_t)(uintptr_t)dispose)) {
            dispose(pn);
        } else {
            fprintf(stderr, "[SRE/SceneObject] sp_release: dispose fn 0x%llx invalid — skipped\n",
                    (unsigned long long)(uintptr_t)dispose);
        }

        /* After dispose(), re-read weak_count and re-validate vtable before
         * calling destroy(). dispose() may have freed the block, recycled the
         * allocator slot, or zeroed the counts itself. */
        uint64_t vtable2 = *(uint64_t*)pn;
        if (!sp_vtable_is_valid(vtable2)) {
            /* vtable invalidated by dispose() — block was freed, skip destroy() */
            return;
        }

        int64_t* weak_count = (int64_t*)((char*)pn + 0x10);
        (*weak_count)--;
        if (*weak_count == 0) {
            /* Call virtual destroy() — vtable[3] (offset 0x18) */
            typedef void (*fn_destroy)(void*);
            fn_destroy destroy = (fn_destroy)(*(uint64_t*)(vtable2 + 0x18));
            if (sp_code_is_valid((uint64_t)(uintptr_t)destroy)) {
                destroy(pn);
            } else {
                fprintf(stderr, "[SRE/SceneObject] sp_release: destroy fn 0x%llx invalid — skipped\n",
                        (unsigned long long)(uintptr_t)destroy);
            }
        }
    }
}

/* =========================================================================
 * sre_GameSceneView_Update — FULL reimplementation
 * =========================================================================
 * ARM64 ABI: X0 = this, S0 = float deltaTime
 */
void sre_GameSceneView_Update(void* self, float deltaTime) {
    if (!self) return;
    char* this_ = (char*)self;
    
    /* Store scene view pointer for host */
    g_sre_gui_scene_view_ptr = (uint64_t)self;
    g_sre_gamesceneview_ptr = self;
    g_sre_gui_scene_active = 1;

    /* Watchdog suppressor: skip game-state reads for N frames after a forced
     * scene-load flag clear (avoids spamming stale HUD data during transition). */
    if (g_sre_suppress_hud_frames > 0) {
        g_sre_suppress_hud_frames--;
        goto do_effects;
    }

    /* ---- Read core pointers ---- */
    uint64_t overlay_ptr = *(uint64_t*)(this_ + 0x100);  /* GameOverlayView* */
    uint64_t ctrl_ptr    = *(uint64_t*)(this_ + 0xF0);   /* GameSceneController* */
    
    if (overlay_ptr == 0 || ctrl_ptr == 0) goto do_effects;
    
    uint64_t gamestate = *(uint64_t*)(ctrl_ptr + 0x08);   /* GameState* */
    if (gamestate == 0) goto do_effects;
    
    /* Export gamestate pointer for host-side modding */
    g_sre_gamestate_ptr = (uint64_t)gamestate;

    /* Poll current scene name from GameState (offset +0x158 contains std::string current_level) */
    {
        extern char g_sre_current_scene_name[128];
        char* scene_name_ptr = *(char**)(gamestate + 0x158);
        if (scene_name_ptr && (uint64_t)scene_name_ptr > 0x1000) { /* basic validity check */
            if (scene_name_ptr[0] != '\0') {
                /* sre_streq is safer than strcmp here, but let's just do a manual check or use strncmp */
                extern int strncmp(const char*, const char*, size_t);
                if (strncmp(g_sre_current_scene_name, scene_name_ptr, 127) != 0) {
                    extern int snprintf(char *str, size_t size, const char *format, ...);
                    snprintf(g_sre_current_scene_name, 128, "%s", scene_name_ptr);
                }
            }
        }
    }

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
    /* Respect g_sre_controls_hidden set by mods, and hide during cinematics/dialog (combat_flag) */
    {
        extern int g_sre_controls_hidden;
        int combat_flag = (int)(uint8_t)this_[0x198];
        void* overlay = (void*)*(uint64_t*)(this_ + 0x100);
        int hide = (g_sre_controls_hidden || combat_flag != 0) ? 1 : 0;
        if (g_sre_GameOverlayView_SetControlsHidden) g_sre_GameOverlayView_SetControlsHidden(overlay, hide);

        if (overlay) {
            uint64_t spell_picker = *(uint64_t*)((char*)overlay + 0x1B8);
            uint64_t settings_btn = *(uint64_t*)((char*)overlay + 0x1C8);
            uint64_t mana_bar = *(uint64_t*)((char*)overlay + 0x1F8);

            // Diagnostic log removed to fix SRE-HUD spam

            // Force the settings button to remain visible
            if (settings_btn) {
                *(uint8_t*)(settings_btn + 0xE4) = 0; // isHidden = false
            }

            // Force the spell picker button to remain visible if player has spells
            if (spell_picker) {
                int has_spells = 0;
                if (gamestate) {
                    has_spells = (int)((uint64_t)(*(long *)(gamestate + 0x58) - *(long *)(gamestate + 0x50)) >> 4) > 0;
                }
                *(uint8_t*)(spell_picker + 0xE4) = has_spells ? 0 : 1;
            }

            // Force the mana bar to remain visible if the player has at least one spell
            if (mana_bar) {
                int has_spells = 0;
                if (gamestate) {
                    has_spells = (int)((uint64_t)(*(long *)(gamestate + 0x58) - *(long *)(gamestate + 0x50)) >> 4) > 0;
                }
                *(uint8_t*)(mana_bar + 0xE4) = has_spells ? 0 : 1;
            }
        }
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

    /* ---- 6.5. GAME WORLD UPDATE — tick the engine simulation ---- */
    /* The original chain reaches Scene::Update (object updates, physics,
     * collision, and the DEFERRED DELETION loop that removes objects after
     * their timed destruction window) via GameSceneController::Update. The
     * SRE frame driver never dispatched it, so timed object destruction and
     * world simulation were dead. Drive it here, guarded by Scene's frame
     * counter (+0x310) so we never double-step if the original chain also
     * ticks the scene this frame. GameSceneController::Update is the entry
     * that also handles hero spawn, camera and enemy targeting (nm 0x349d84). */
    {
        extern volatile int g_sre_scene_loading;
        extern uint64_t g_swordigo_base;
        if (!g_sre_scene_loading && ctrl_ptr != 0) {
            void* scene = *(void**)((char*)ctrl_ptr + 0x20);  /* GameSceneController+0x20 = Scene* */
            if (scene && (uint64_t)scene > 0x10000) {
                uint64_t scene_vt = *(uint64_t*)scene;
                if (scene_vt && (scene_vt & 7) == 0) {
                    int frame = *(int*)((char*)scene + 0x310);
                    if ((uint64_t)scene != s_sre_last_driven_scene ||
                        frame == s_sre_last_driven_frame) {
                        g_sre_world_drive_count++;
                        typedef void (*pfn_gsc_update)(void*, float);
                        ((pfn_gsc_update)(g_swordigo_base + 0x349d84))((void*)ctrl_ptr, deltaTime);
                        frame = *(int*)((char*)scene + 0x310);
                    }
                    s_sre_last_driven_scene = (uint64_t)scene;
                    s_sre_last_driven_frame = frame;
                }
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

/* =========================================================================
 * Scene Loading Pipeline Safety Wrappers & Error Recovery
 * ========================================================================= */

typedef void (*pfn_orig_SceneLoadingView_InitWithGameState)(void* self, void* state_ptr, void* map_node_ptr);
pfn_orig_SceneLoadingView_InitWithGameState g_orig_SceneLoadingView_InitWithGameState = 0;

volatile void* g_sre_last_slv = NULL;

void sre_SceneLoadingView_InitWithGameState(void* self, void* state_ptr, void* map_node_ptr) {
    fprintf(stderr, "[SRE/Scene] SceneLoadingView::InitWithGameState starting (view=%p)...\n", self);

    /* CRITICAL: raise scene loading flag BEFORE calling original.
     * This suppresses all lua_resume calls in ProgramState::Update for the
     * entire duration of scene loading, preventing mutex-under-longjmp deadlock. */
    g_sre_scene_loading = 1;
    g_sre_last_slv = self;

    /* New scene is loading: forget the previously-driven scene so the world-
     * update drive re-engages even if the fresh scene is allocated at the
     * same address as the old one (which would otherwise match the stale
     * last-driven guard state and silently skip the drive). */
    s_sre_last_driven_scene = 0;
    s_sre_last_driven_frame = -1;

    if (g_orig_SceneLoadingView_InitWithGameState) {
        g_orig_SceneLoadingView_InitWithGameState(self, state_ptr, map_node_ptr);
    }
    fprintf(stderr, "[SRE/Scene] SceneLoadingView::InitWithGameState finished successfully.\n");
}

typedef void (*pfn_orig_GameSceneController_InitWithScene)(void* self, void* scene_ptr);
pfn_orig_GameSceneController_InitWithScene g_orig_GameSceneController_InitWithScene = 0;

void sre_GameSceneController_InitWithScene(void* self, void* scene_ptr) {
    fprintf(stderr, "[SRE/Scene] GameSceneController::InitWithScene starting (ctrl=%p, scene=%p)...\n", self, scene_ptr);
    if (g_orig_GameSceneController_InitWithScene) {
        g_orig_GameSceneController_InitWithScene(self, scene_ptr);
    }
    fprintf(stderr, "[SRE/Scene] GameSceneController::InitWithScene finished successfully.\n");
}

typedef void (*pfn_orig_Scene_FinishLoad)(void* self);
pfn_orig_Scene_FinishLoad g_orig_Scene_FinishLoad = 0;

void sre_Scene_FinishLoad(void* self) {
    if (!self) return;
    fprintf(stderr, "[SRE/Scene] Scene::FinishLoad starting (scene=%p)...\n", self);

    /* CRITICAL: raise scene loading flag BEFORE calling original.
     * This suppresses all lua_resume calls in ProgramState::Update for the
     * entire duration of scene loading, preventing mutex-under-longjmp deadlock. */
    g_sre_scene_loading = 1;

    /* NOTE: Scene's object iteration uses a doubly-linked list, NOT a begin/end vector.
     * Layout (from Ghidra):
     *   this+0x28  = ProgramState embedded object (NOT a vector!)
     *   this+0xb8  = SceneObject linked-list sentinel
     *   this+0xC8  = SceneObject linked-list head (this+200)
     *   this+0xe8  = SceneObjectGroup linked-list sentinel
     *   this+0xf8  = SceneObjectGroup linked-list head
     * DO NOT sanitize any of these — they are traversed by the original FinishLoad. */

    if (g_orig_Scene_FinishLoad) {
        lua_State* L = g_sre_last_lua_state;
        if (L != NULL) {
            int my_depth = recovery_push(L);
            if (my_depth >= 0 && sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
                recovery_pop(my_depth);
                fprintf(stderr, "[SRE/Scene] Recovered exception during Scene::FinishLoad! Scene load continuing.\n");
                /* MUTEX SAFETY: force unlock — a longjmp may have escaped a locked section */
                if (g_lua_mutex_ptr) pthread_mutex_unlock(g_lua_mutex_ptr);
                goto finish_load_done;
            }
            g_orig_Scene_FinishLoad(self);
            if (my_depth >= 0) recovery_pop(my_depth);
        } else {
            g_orig_Scene_FinishLoad(self);
        }
    }

finish_load_done:
    /* Scene load is complete. ProgramState owns its mutex; this wrapper never
     * locks it, so unlocking here would violate mutex ownership and is undefined. */
    g_sre_scene_loading = 0;
    fprintf(stderr, "[SRE/Scene] Scene::FinishLoad finished. Loading flag cleared.\n");
}

typedef void (*pfn_orig_SceneObject_FinishLoad)(void* self);
pfn_orig_SceneObject_FinishLoad g_orig_SceneObject_FinishLoad = 0;

void sre_SceneObject_FinishLoad(void* self) {
    if (!self) return;

    /* Guard the component vector (this+0xC0=begin, this+0xC8=end) before calling the
     * original. IDA decomp shows FinishLoad iterates from end→begin calling vtable[9].
     * If begin/end are corrupted (too far apart), the loop runs forever → freeze.
     *
     * Safe bounds: a single SceneObject should never have more than 256 components.
     * If the distance exceeds this, zero the end pointer so the loop body is skipped. */
    {
        uint64_t* begin_p = (uint64_t*)((char*)self + 0xC0);
        uint64_t* end_p   = (uint64_t*)((char*)self + 0xC8);
        uint64_t begin_v  = *begin_p;
        uint64_t end_v    = *end_p;
        size_t count = 0;
        /* Check if pointers look valid: both non-null, end >= begin, difference <= 256 ptrs */
        if (begin_v && end_v && end_v >= begin_v) {
            count = (size_t)((end_v - begin_v) / sizeof(uint64_t));
            if (count > 256) {
                fprintf(stderr, "[SRE/SceneObject] FinishLoad: corrupt component count=%zu (obj=%p) — clamping to 0\n",
                        count, self);
                /* Clamp: set end = begin so the loop body is skipped entirely */
                *end_p = begin_v;
                count = 0;
            }
        } else if (end_v && !begin_v) {
            /* end non-null but begin null — clear end too */
            fprintf(stderr, "[SRE/SceneObject] FinishLoad: null begin, non-null end=%llx (obj=%p) — clearing end\n",
                    (unsigned long long)end_v, self);
            *end_p = 0;
            count = 0;
        }

        /* ── Per-component vtable safety (Purplemoor Crypt crash fix) ──────────────
         * Crash: PC=0x10771ac = g_swordigo_base+0x77168 (.dynstr section).
         * Occurs when a corrupted component pointer (e.g. unaligned 0x17cc002)
         * or a component with a invalid parent/vtable is passed to FinishLoad.
         * When FinishLoad calls vtable[9], Dynarmic executes string bytes as ARM64
         * instructions → UnallocatedEncoding (type=0) which bypasses setjmp.
         *
         * Fix: Strict 5-point validation on every component before calling original:
         * 1. 8-byte pointer alignment ((comp & 7) == 0)
         * 2. Valid vtable location (.data.rel.ro / .rodata [0x583480, 0x6e8000))
         * 3. 4-byte fn9 instruction alignment ((fn9 & 3) == 0)
         * 4. Valid fn9 code location (.text [0x1f33d0, 0x583480))
         * 5. Parent SceneObject pointer validation (at comp+0x28 if non-null)
         * Invalid components are zeroed out so original's loop skips them safely. */
        if (count > 0 && begin_v && g_swordigo_base) {
            const uint64_t text_lo = g_swordigo_base + 0x1f33d0ULL; /* .plt start */
            const uint64_t text_hi = g_swordigo_base + 0x583480ULL; /* .rodata start */
            const uint64_t ro_lo   = g_swordigo_base + 0x583480ULL; /* .rodata start */
            const uint64_t ro_hi   = g_swordigo_base + 0x6e8000ULL; /* .bss start */

            uint64_t* arr = (uint64_t*)begin_v;
            for (size_t ci = 0; ci < count; ci++) {
                uint64_t comp = arr[ci];
                if (!comp) continue;

                /* 1. Alignment check: C++ component pointers MUST be 8-byte aligned */
                if ((comp & 7) != 0 || comp < 0x10000ULL || comp >= 0x800000000000ULL) {
                    fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                            "comp[%zu]=0x%llx is misaligned or invalid range — zeroing\n",
                            ci, (unsigned long long)comp);
                    arr[ci] = 0;
                    continue;
                }

                /* 2. Read vtable pointer */
                uint64_t vtable = *(uint64_t*)comp;
                if (!vtable || (vtable & 7) != 0) {
                    fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                            "comp[%zu]=%p vtable=0x%llx is null or misaligned — zeroing\n",
                            ci, (void*)comp, (unsigned long long)vtable);
                    arr[ci] = 0;
                    continue;
                }

                /* Vtable must be in .data.rel.ro / .rodata, libsre, or trampoline caves */
                bool vtable_ok = (vtable >= ro_lo && vtable < ro_hi) ||
                                 (vtable >= 0x2000000ULL && vtable < 0x2300000ULL) ||
                                 (vtable >= 0x3000000ULL && vtable < 0x3100000ULL);
                if (!vtable_ok) {
                    fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                            "comp[%zu]=%p vtable=0x%llx not in .rodata range — zeroing\n",
                            ci, (void*)comp, (unsigned long long)vtable);
                    arr[ci] = 0;
                    continue;
                }

                /* 3 & 4. Validate vtable[9] — FinishLoad call target */
                uint64_t fn9 = ((uint64_t*)vtable)[9];
                if (!fn9 || (fn9 & 3) != 0) {
                    fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                            "comp[%zu]=%p vtable[9]=0x%llx is null or misaligned — zeroing\n",
                            ci, (void*)comp, (unsigned long long)fn9);
                    arr[ci] = 0;
                    continue;
                }

                bool fn9_ok = (fn9 >= text_lo && fn9 < text_hi) ||
                              (fn9 >= 0x2000000ULL && fn9 < 0x2300000ULL) ||
                              (fn9 >= 0x3000000ULL && fn9 < 0x3100000ULL);
                if (!fn9_ok) {
                    fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                            "comp[%zu]=%p vtable[9]=0x%llx not in .text range — zeroing\n",
                            ci, (void*)comp, (unsigned long long)fn9);
                    arr[ci] = 0;
                    continue;
                }

                /* 5. Check parent pointer at comp+0x28 (accessed by BackgroundComponent::Prepare) */
                uint64_t parent = *(uint64_t*)(comp + 0x28);
                if (parent) {
                    if ((parent & 7) != 0 || parent < 0x10000ULL || parent >= 0x800000000000ULL) {
                        fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                                "comp[%zu]=%p parent=0x%llx misaligned — zeroing\n",
                                ci, (void*)comp, (unsigned long long)parent);
                        arr[ci] = 0;
                        continue;
                    }
                    uint64_t parent_vt = *(uint64_t*)parent;
                    if (!parent_vt || (parent_vt & 7) != 0 ||
                        !((parent_vt >= ro_lo && parent_vt < ro_hi) ||
                          (parent_vt >= 0x2000000ULL && parent_vt < 0x2300000ULL) ||
                          (parent_vt >= 0x3000000ULL && parent_vt < 0x3100000ULL))) {
                        fprintf(stderr, "[SRE/SceneObject] FinishLoad guard: "
                                "comp[%zu]=%p parent=0x%llx has bad vtable=0x%llx — zeroing\n",
                                ci, (void*)comp, (unsigned long long)parent, (unsigned long long)parent_vt);
                        arr[ci] = 0;
                        continue;
                    }
                }
            }
        }
    }

    if (g_orig_SceneObject_FinishLoad) {
        lua_State* L = g_sre_last_lua_state;
        if (L != NULL) {
            int my_depth = recovery_push(L);
            if (my_depth >= 0 && sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
                recovery_pop(my_depth);
                fprintf(stderr, "[SRE/SceneObject] Recovered exception in SceneObject::FinishLoad (obj=%p)! Safely skipped object.\n", self);
                if (g_lua_mutex_ptr) pthread_mutex_unlock(g_lua_mutex_ptr);
                return;
            }
            g_orig_SceneObject_FinishLoad(self);
            if (my_depth >= 0) recovery_pop(my_depth);
        } else {
            g_orig_SceneObject_FinishLoad(self);
        }
    }
}

/* =============================================================================
 * sre_SceneObjectGroup_FinishLoad — SceneObjectGroup::FinishLoad safety wrap
 * =============================================================================
 * IDA at nm 0x475498:
 *   Caver::SceneObjectGroup::FinishLoad reads this->program (offset +40)
 *   and calls ProgramState::Execute on it immediately during scene loading.
 *   If the attached Lua script throws (error, stack overflow, bad access),
 *   the original calls ProgramPanic → abort. This hook intercepts via setjmp
 *   so the error is recoverable.
 *
 * Unlike SceneObject::FinishLoad (which iterates a component vector), the
 * SceneObjectGroup variant does NOT have a loop that can run forever — the
 * only danger is an unrecovered Lua exception from Execute(). So we do NOT
 * apply a component bounds guard here; only the setjmp wrapper is needed.
 * ============================================================================= */
typedef void (*pfn_orig_SceneObjectGroup_FinishLoad)(void* self);
pfn_orig_SceneObjectGroup_FinishLoad g_orig_SceneObjectGroup_FinishLoad = 0;

void sre_SceneObjectGroup_FinishLoad(void* self) {
    if (!self) return;
    if (g_orig_SceneObjectGroup_FinishLoad) {
        lua_State* L = g_sre_last_lua_state;
        if (L != NULL) {
            int my_depth = recovery_push(L);
            if (my_depth >= 0 && sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
                recovery_pop(my_depth);
                fprintf(stderr, "[SRE/SceneGroup] Recovered exception in SceneObjectGroup::FinishLoad (obj=%p)! "
                        "Skipped group script execution safely.\n", self);
                return;
            }
            g_orig_SceneObjectGroup_FinishLoad(self);
            if (my_depth >= 0) recovery_pop(my_depth);
        } else {
            g_orig_SceneObjectGroup_FinishLoad(self);
        }
    }
}

typedef void (*pfn_orig_ReadPODModelFromFile)(void* pod_model, void* filename_cppstr);
pfn_orig_ReadPODModelFromFile g_orig_ReadPODModelFromFile = 0;

void sre_ReadPODModelFromFile(void* pod_model, void* filename_cppstr) {
    if (g_orig_ReadPODModelFromFile) {
        lua_State* L = g_sre_last_lua_state;
        if (L != NULL) {
            int my_depth = recovery_push(L);
            if (my_depth >= 0 && sre_setjmp(g_sre_recovery_stack[my_depth].buf) != 0) {
                recovery_pop(my_depth);
                fprintf(stderr, "[SRE/POD] Exception recovered during ReadPODModelFromFile! Bypassed invalid POD mesh.\n");
                return;
            }
            g_orig_ReadPODModelFromFile(pod_model, filename_cppstr);
            if (my_depth >= 0) recovery_pop(my_depth);
        } else {
            g_orig_ReadPODModelFromFile(pod_model, filename_cppstr);
        }
    }
}

typedef int (*pfn_orig_CPVRTModelPOD_ReadFromMemory)(void* pod_model, const char* pData, size_t nSize, const char* pszExpOpt, size_t nExpOptSize, const char* pszHistory, size_t nHistorySize);
pfn_orig_CPVRTModelPOD_ReadFromMemory g_orig_CPVRTModelPOD_ReadFromMemory = 0;

int sre_CPVRTModelPOD_ReadFromMemory(void* pod_model, const char* pData, size_t nSize, const char* pszExpOpt, size_t nExpOptSize, const char* pszHistory, size_t nHistorySize) {
    if (nSize < 40) {
        fprintf(stderr, "[SRE/POD] Safety Hook: Bypassing CPVRTModelPOD::ReadFromMemory due to small/dummy buffer size (%zu bytes)\n", nSize);
        return 0; // return false, parsing failed
    }
    if (g_orig_CPVRTModelPOD_ReadFromMemory) {
        return g_orig_CPVRTModelPOD_ReadFromMemory(pod_model, pData, nSize, pszExpOpt, nExpOptSize, pszHistory, nHistorySize);
    }
    return 0;
}

/* =============================================================================
 * sre_GameData_Clear — GameData::Clear crash guard
 * =============================================================================
 * CRASH (from live dump + IDA decomp at nm 0x2e6d60):
 *
 * GameData::Clear iterates 5 repeated protobuf fields calling vtable[0x20]
 * (the virtual Clear()) on each element:
 *
 *   for (v2=0; v2 < *(int*)(this+24); v2++) {
 *     v3 = *(QWORD*)(*(QWORD*)(this+16) + 8*v2);
 *     (*(code**)(*(long*)v3 + 32))(v3);   // vtable[0x20]
 *   }
 *   ... repeated for +80, +136, +192, +248 ...
 *
 * When *(int*)(this+24) == 0x1077 (4215, corrupt), the loop walks 4215
 * garbage pointers. One has vtable = .dynstr data → PC = 0x10771ac → crash.
 *
 * FIX: Cap each count field to 0 if it exceeds 512 (impossible for valid save).
 *
 * Field layout (from IDA decomp of Clear + proto field numbers via nm R symbols):
 *   this+24 / this+16   = item         (kItemFieldNumber)
 *   this+80 / this+72   = skill        (kSkillFieldNumber)
 *   this+136 / this+128 = quest        (kQuestFieldNumber)
 *   this+192 / this+184 = entity_class (kEntityClassFieldNumber)
 *   this+248 / this+240 = guide_target (kGuideTargetFieldNumber)
 *
 * nm -D libswordigo.so v1.4.12 arm64-v8a:  GameData::Clear = 0x2e6d60
 * =============================================================================
 */
uint64_t g_orig_GameData_Clear = 0;

#define GAMEDATA_MAX_VALID_COUNT  512

static void gamedata_cap_count(char* base, int offset) {
    int* p = (int*)(base + offset);
    if (*p < 0 || *p > GAMEDATA_MAX_VALID_COUNT) {
        fprintf(stderr, "[SRE/GameData] Clear: corrupt count at +%d = %d — clamped to 0\n",
                offset, *p);
        *p = 0;
    }
}

void sre_GameData_Clear(void* this_) {
    if (!this_) goto call_orig;
    {
        char* t = (char*)this_;
        gamedata_cap_count(t,  24);
        gamedata_cap_count(t,  80);
        gamedata_cap_count(t, 136);
        gamedata_cap_count(t, 192);
        gamedata_cap_count(t, 248);
    }
call_orig:
    if (g_orig_GameData_Clear)
        ((void(*)(void*))g_orig_GameData_Clear)(this_);
    else if (g_swordigo_base)
        ((void(*)(void*))(g_swordigo_base + 0x2e6d60))(this_);
}

/* =============================================================================
 * sre_SceneObject_ComponentWithInterface — Component VTable Safety Guard
 * =============================================================================
 * Prevents Dynarmic Halt exceptions when modded / corrupted SceneObjects contain
 * invalid component pointers or corrupted vtables.
 * Address: 0x47462c (v1.4.12 ARM64)
 * =============================================================================
 */
extern int sre_is_valid_vtable_ptr(uint64_t vtable);

uint64_t sre_SceneObject_ComponentWithInterface(void* self, int64_t interface_id) {
    if (!self) return 0;

    uint64_t* begin = *(uint64_t**)((char*)self + 0xC0);  /* this+24 (64-bit) */
    uint64_t* end   = *(uint64_t**)((char*)self + 0xC8);  /* this+25 (64-bit) */

    if (!begin || !end || begin >= end) return 0;

    /* Safety sanity check on vector bounds pointer distance */
    size_t count = (size_t)(end - begin);
    if (count > 256) return 0;  /* max valid components per object */

    for (size_t i = 0; i < count; i++) {
        uint64_t comp_addr = begin[i];
        if (!comp_addr || comp_addr < 0x10000 || comp_addr >= 0x0000800000000000ULL) continue;

        /* Validate vtable pointer */
        uint64_t vtable = *(uint64_t*)comp_addr;
        if (!sre_is_valid_vtable_ptr(vtable)) continue;

        /* Validate HasInterface function pointer at vtable[20] (offset +160) */
        uint64_t fn_has_interface = *(uint64_t*)(vtable + 160);
        if (!fn_has_interface || fn_has_interface < (g_swordigo_base + 0x100000) ||
            fn_has_interface >= (g_swordigo_base + 0x700000)) continue;

        typedef int (*pfn_HasInterface)(uint64_t self, int64_t id);
        pfn_HasInterface fn = (pfn_HasInterface)fn_has_interface;

        if (fn(comp_addr, interface_id) & 1) {
            return comp_addr;
        }
    }
    return 0;
}

/* =============================================================================
 * sre_Proto_SceneObject_Clear — Proto::SceneObject::Clear crash guard
 * =============================================================================
 * CRASH (IDA decomp at nm 0x2fc030, confirmed in live log LR=0x12fe290):
 *
 *   Crash path: Scene::LoadFromFile (nm 0x463780)
 *     -> Proto::Scene::~Scene()   -> ObjectLibrary::Clear()
 *     -> ObjectTemplate::Clear()  -> Proto::SceneObject::Clear()
 *     -> do { v4 = *(QWORD*)(array + 8*i);
 *            (*(*v4 + 32))(v4);   // vtable[+32] = Clear()
 *       } while (i < *(int*)(this+40));
 *
 * Root cause: this+40 (Component count) is corrupted — allocator "next free"
 * link written to freed SceneObject memory -> large integer -> OOB walk ->
 * element vtable = ARM64 STP instruction bytes -> PC = 0x47bfda9034ff4.
 *
 * FIX: Cap this+40 to 0 if > MAX_PROTO_COMPONENT_COUNT before calling original.
 * Also null-guard the array pointer at this+32 (belt-and-suspenders).
 *
 * nm v1.4.12 arm64-v8a: 0x2fc030 (impl)   0x1fa880 (PLT thunk)
 * We hook the IMPL so callers through both paths are intercepted.
 * =============================================================================
 */
uint64_t g_orig_Proto_SceneObject_Clear = 0;

#define MAX_PROTO_COMPONENT_COUNT 512

void sre_Proto_SceneObject_Clear(void* this_) {
    if (this_) {
        char* t = (char*)this_;
        int* comp_count = (int*)(t + 40);
        if (*comp_count < 0 || *comp_count > MAX_PROTO_COMPONENT_COUNT) {
            fprintf(stderr,
                    "[SRE/Proto] SceneObject::Clear: corrupt component count "
                    "at +40 = %d (obj=%p) — clamped to 0\n",
                    *comp_count, this_);
            *comp_count = 0;
        }
        /* Belt-and-suspenders: if array pointer at +32 is outside guest heap,
         * null both pointer and count so original's loop guard can't fire. */
        uint64_t* arr_ptr = (uint64_t*)(t + 32);
        if (*arr_ptr != 0 &&
            (*arr_ptr < 0x10000000ULL || *arr_ptr > 0xE0000000ULL)) {
            fprintf(stderr,
                    "[SRE/Proto] SceneObject::Clear: corrupt array ptr "
                    "+32 = 0x%lx (obj=%p) — zeroed\n",
                    (unsigned long)*arr_ptr, this_);
            *arr_ptr = 0;
            *comp_count = 0;
        }
    }
    if (g_orig_Proto_SceneObject_Clear)
        ((void(*)(void*))g_orig_Proto_SceneObject_Clear)(this_);
    else if (g_swordigo_base)
        ((void(*)(void*))(g_swordigo_base + 0x2fc030))(this_);
}

/* =============================================================================
 * sre_Proto_ObjectLibrary_Clear — Proto::ObjectLibrary::Clear crash guard
 * =============================================================================
 * Sits ONE level above SceneObject::Clear in the destructor chain.
 * IDA decomp at nm 0x2fd374: iterates THREE repeated-field vtable loops:
 *
 *   loop1: array=this+24, count=this+32  (ObjectTemplate list, calls Clear)
 *   loop3: array=this+136, count=this+144 (unknown, vtable[+32])
 *   loop4: array=this+192, count=this+200 (unknown, vtable[+32])
 *
 * (Loop2 at this+80/+88 uses sub_568CB8 = string release — no vtable, safe.)
 *
 * Same corrupt-count risk. We cap all three before calling through.
 *
 * nm v1.4.12 arm64-v8a: 0x2fd374 (impl)
 * =============================================================================
 */
uint64_t g_orig_Proto_ObjectLibrary_Clear = 0;

#define MAX_PROTO_OBJECTLIB_COUNT 1024

static void objlib_cap_count(char* base, int offset, const char* tag) {
    int* p = (int*)(base + offset);
    if (*p < 0 || *p > MAX_PROTO_OBJECTLIB_COUNT) {
        fprintf(stderr,
                "[SRE/Proto] ObjectLibrary::Clear: corrupt %s count at +%d = %d "
                "— clamped to 0\n", tag, offset, *p);
        *p = 0;
    }
}

void sre_Proto_ObjectLibrary_Clear(void* this_) {
    if (this_) {
        char* t = (char*)this_;
        objlib_cap_count(t,  32, "ObjectTemplate");
        objlib_cap_count(t, 144, "field3");
        objlib_cap_count(t, 200, "field4");
    }
    if (g_orig_Proto_ObjectLibrary_Clear)
        ((void(*)(void*))g_orig_Proto_ObjectLibrary_Clear)(this_);
    else if (g_swordigo_base)
        ((void(*)(void*))(g_swordigo_base + 0x2fd374))(this_);
}

/* =============================================================================
 * Proto::SceneObject::~SceneObject — teardown lifetime guard
 * =============================================================================
 * IDA v1.4.12 ARM64 (0x2fe23c) shows the destructor iterating:
 *   component array: this + 0x20
 *   component count: this + 0x2c
 * and calling each component's vtable[1] destructor.  During a scene swap a
 * deferred guest free can leave the array/count pair pointing at reclaimed
 * memory.  The native destructor has no bounds or vtable validation, so a
 * stale component can branch into .dynstr and halt Dynarmic.
 */
uint64_t g_orig_Proto_SceneObject_Destroy = 0;

void sre_Proto_SceneObject_Destroy(void* this_) {
    if (!this_) return;

    char* base = (char*)this_;
    uint64_t array_addr = *(uint64_t*)(base + 0x20);
    int count = *(int*)(base + 0x2c);
    bool bad = count < 0 || count > MAX_PROTO_COMPONENT_COUNT;

    /* Proto objects and their repeated-field arrays are guest heap objects. */
    if (count > 0 && (!array_addr || (array_addr & 7) != 0 ||
                      array_addr < 0x20000000ULL || array_addr >= 0xe0000000ULL))
        bad = true;

    if (!bad && array_addr && count > 0 && g_swordigo_base) {
        uint64_t* components = (uint64_t*)array_addr;
        const uint64_t text_lo = g_swordigo_base + 0x203e90ULL;
        const uint64_t text_hi = g_swordigo_base + 0x583480ULL;
        const uint64_t ro_lo = g_swordigo_base + 0x583480ULL;
        const uint64_t ro_hi = g_swordigo_base + 0x6e8000ULL;

        for (int i = 0; i < count; ++i) {
            uint64_t component = components[i];
            if (!component || (component & 7) != 0 ||
                component < 0x20000000ULL || component >= 0xe0000000ULL) {
                bad = true;
                break;
            }

            uint64_t vtable = *(uint64_t*)component;
            bool vtable_ok = vtable && (vtable & 7) == 0 &&
                ((vtable >= ro_lo && vtable < ro_hi) ||
                 (vtable >= 0x2000000ULL && vtable < 0x2300000ULL) ||
                 (vtable >= 0x3000000ULL && vtable < 0x3100000ULL));
            if (!vtable_ok) {
                bad = true;
                break;
            }

            uint64_t destructor = ((uint64_t*)vtable)[1];
            bool destructor_ok = destructor && (destructor & 3) == 0 &&
                ((destructor >= text_lo && destructor < text_hi) ||
                 (destructor >= 0x2000000ULL && destructor < 0x2300000ULL) ||
                 (destructor >= 0x3000000ULL && destructor < 0x3100000ULL));
            if (!destructor_ok) {
                bad = true;
                break;
            }
        }
    }

    if (bad) {
        fprintf(stderr, "[SRE/Proto] SceneObject::~SceneObject: invalid component array/count "
                        "array=0x%llx count=%d (obj=%p) — skipping component teardown\n",
                (unsigned long long)array_addr, count, this_);
        *(uint64_t*)(base + 0x20) = 0;
        *(int*)(base + 0x2c) = 0;
    }

    if (g_orig_Proto_SceneObject_Destroy)
        ((void(*)(void*))g_orig_Proto_SceneObject_Destroy)(this_);
    else if (g_swordigo_base)
        ((void(*)(void*))(g_swordigo_base + 0x2fe23c))(this_);
}

/* =============================================================================
 * sre_ComponentOutletBase_Connect — vtable safety guard
 * =============================================================================
 * CRASH (live log, Dynarmic NoExecuteFault):
 *   X17 = 0x1247c48 [ComponentOutletBase::Connect]
 *   X30 = 0x12824d4 [FireBreathComponent::FireBreathComponent]
 *   X8  = 0xf9443508d0002308  ← CORRUPTED vtable ptr
 *   PC  = 0x443508d0002308    ← jumps into kernel-space non-canonical addr
 *
 * IDA decompiled body at nm 0x247c48:
 *   result = (*(*(_QWORD*)this + 8))(this);  // vtable[1] = identifier getter
 *   if (result) {
 *     v5 = *((_QWORD*)a2 + 5);               // a2 = Component*
 *     v7 = *(*(_QWORD*)this + 32);           // vtable[4] = connect fn
 *     (*(*(_QWORD*)this + 8))(this);         // vtable[1] again
 *     SceneObject::ComponentWithIdentifier(v5);
 *     return v7(this, v6);
 *   }
 *   return result;
 *
 * Root cause: when a component is destroyed during level unload, its 'this'
 * pointer carried by the outlet struct still references freed memory.
 * The allocator has written a free-list link word over the vtable slot.
 * Calling vtable[1] launches into garbage memory → NoExecuteFault / PC far.
 *
 * Fix: validate vtable BEFORE any dispatch. If invalid — bail with 0 (same
 * as the "no identifier" early-exit path in the original). This silently
 * skips the binding which is fine: the component being bound no longer
 * exists anyway.
 *
 * nm -D libswordigo.so v1.4.12 arm64-v8a: 0x247c48 (confirmed via swordigo_symbols_demangled.txt)
 * =============================================================================
 */
uint64_t g_orig_ComponentOutletBase_Connect = 0;

uint64_t sre_ComponentOutletBase_Connect(void* this_, void* a2) {
    /* Null-guard 'this' (the outlet object) */
    if (!this_) {
        fprintf(stderr, "[SRE/Outlet] Connect: null this_ — skipped\n");
        return 0;
    }

    /* Guard the outlet object's vtable pointer at *this */
    uint64_t vtable = *(uint64_t*)this_;
    if (!sre_is_valid_vtable_ptr(vtable)) {
        fprintf(stderr,
                "[SRE/Outlet] Connect: corrupt outlet vtable=0x%llx (this=%p) — blocked\n",
                (unsigned long long)vtable, this_);
        return 0;
    }

    /* Validate vtable slot 1 (byte offset +8): the identifier-getter function.
     * Must be within libswordigo.so .text range (0x1203e90 – 0x1584000). */
    uint64_t fn_slot1 = *(uint64_t*)(vtable + 8);
    if (fn_slot1 < (g_swordigo_base + 0x203e90) ||
        fn_slot1 >= (g_swordigo_base + 0x584000)) {
        fprintf(stderr,
                "[SRE/Outlet] Connect: vtable[1]=0x%llx out of .text (this=%p) — blocked\n",
                (unsigned long long)fn_slot1, this_);
        return 0;
    }

    /* Validate vtable slot 4 (byte offset +32): the connect-setter function. */
    uint64_t fn_slot4 = *(uint64_t*)(vtable + 32);
    if (fn_slot4 < (g_swordigo_base + 0x203e90) ||
        fn_slot4 >= (g_swordigo_base + 0x584000)) {
        fprintf(stderr,
                "[SRE/Outlet] Connect: vtable[4]=0x%llx out of .text (this=%p) — blocked\n",
                (unsigned long long)fn_slot4, this_);
        return 0;
    }

    /* Validate Component* a2 if non-null (IDA: *(QWORD*)(a2+5*8) = sceneObject ptr) */
    if (a2) {
        uint64_t a2_addr = (uint64_t)a2;
        if (a2_addr < 0x10000 || a2_addr >= 0x0000800000000000ULL) {
            fprintf(stderr,
                    "[SRE/Outlet] Connect: Component* a2=0x%llx out of heap range — blocked\n",
                    (unsigned long long)a2_addr);
            return 0;
        }
    }

    /* All guards passed — call through to original implementation */
    if (g_orig_ComponentOutletBase_Connect) {
        return ((uint64_t(*)(void*, void*))g_orig_ComponentOutletBase_Connect)(this_, a2);
    }
    return 0;
}
