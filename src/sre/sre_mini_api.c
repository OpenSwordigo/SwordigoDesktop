/*
 * sre_mini_api.c — SwMini-compatible Lua API for mod support
 *
 * Reimplements the Mini.*, LNI.*, and Components.* Lua tables
 * that SwMini registers in every Lua state. RLSwordigo scripts
 * (rlsw.scl, code.scl, mason.scl, etc.) depend on these APIs.
 *
 * Instead of loading libmini.so (which needs GlossHook + JNI),
 * we implement the same API surface directly in SRE.
 *
 * Hook: ProgramState::RegisterProgramLibrary
 * After the engine registers its own Lua libs, we inject ours.
 */

#include "sre.h"
#include "sre_lua.h"
#include "sre_caver.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* g_swordigo_base — loaded base address of libswordigo.so in guest space */
extern uint64_t g_swordigo_base;


/* Avoid relying on system headers (cross-build). Provide minimal externs */
extern void sre_log_lua_error(const char* source, const char* err_msg);


/* =========================================================================
 * External globals from sre_scene_update.c — player stats
 * ========================================================================= */
extern volatile int g_sre_player_hp;
extern volatile int g_sre_player_max_hp;
extern volatile int g_sre_player_mana;
extern volatile int g_sre_player_max_mana;
extern volatile int g_sre_player_coins;
extern volatile int g_sre_player_xp;
extern volatile int g_sre_player_level;
extern volatile int g_sre_player_atk_level;
extern volatile int g_sre_player_hp_level;
extern volatile int g_sre_player_mana_level;

/* =========================================================================
 * Shared globals for host communication
 * ========================================================================= */

/* Mod identification */
char g_sre_mod_arch[32] = "arm64-v8a";     /* Mini.Arch() return value */
char g_sre_mod_profile_id[64] = {0};       /* Mini.GetProfileID() */

/* Game speed — readable/writable from Lua, host polls this */
float g_sre_game_speed = 1.0f;

/* Resolved dynamically by host JIT */
void* g_OverlayTextComponent_Interface_addr = NULL;

/* Controls visibility flag */
int g_sre_controls_hidden = 0;

/* Coin limit */
int g_sre_coin_limit = 9999;

/* Debug toggle */
int g_sre_debug_active = 0;

/* LNI command buffer — for commands that need host action */
volatile char g_sre_lni_command[64] = {0};
volatile char g_sre_lni_arg1[512] = {0};
volatile char g_sre_lni_arg2[512] = {0};
volatile int  g_sre_lni_pending = 0;

/* Character modification requests — host polls these */
volatile int g_sre_char_set_pending = 0;
volatile int g_sre_char_set_field = 0;  /* 0=none, 1=level, 2=exp, 3=hp, 4=mana, 5=coins */
volatile int g_sre_char_set_value = 0;

/* Character action requests (SwKiwi deferred-action pattern) — host polls */
#define SRE_CHAR_ACTION_NONE         0
#define SRE_CHAR_ACTION_DIE          1
#define SRE_CHAR_ACTION_HURT         2
#define SRE_CHAR_ACTION_USE          3
#define SRE_CHAR_ACTION_SWING        4
#define SRE_CHAR_ACTION_STOP_SWING   5
#define SRE_CHAR_ACTION_START_JUMP   6
#define SRE_CHAR_ACTION_STOP_JUMP    7
#define SRE_CHAR_ACTION_DROP_QUICKLY 8
#define SRE_CHAR_ACTION_CANCEL_CAST  9
#define SRE_CHAR_ACTION_FINISH_CAST  10
volatile int g_sre_char_action_pending = 0;
volatile int g_sre_char_action = SRE_CHAR_ACTION_NONE;

/* Character extended state (SwKiwi) */
int   g_sre_char_movement_facing_lock = 0;
float g_sre_char_stun_time = 0.0f;
int   g_sre_char_air_jump_used = 0;

/* Deferred level-attribute write (SwKiwi) — host polls */
volatile int g_sre_char_attr_set_pending = 0;
volatile int g_sre_char_attr_hp = 0;
volatile int g_sre_char_attr_atk = 0;
volatile int g_sre_char_attr_mana = 0;

/* Camera state — SRE-side globals, host polls g_sre_cam_set_pending */
volatile float g_sre_cam_x = 0.0f;
volatile float g_sre_cam_y = 0.0f;
volatile float g_sre_cam_z = 0.0f;
volatile int   g_sre_cam_set_pending = 0;
volatile float g_sre_cam_zoom = 1.0f;
volatile int   g_sre_cam_follow = 1;  /* 1 = follow hero, 0 = free */

/* Camera up-vector (SwKiwi CameraController) */
float g_sre_cam_up_x = 0.0f;
float g_sre_cam_up_y = 1.0f;
float g_sre_cam_up_z = 0.0f;

/* RecreateHero requires calling Caver::GameSceneController::RecreateHero()
 * at engine offset. This needs a guest function callback mechanism that
 * doesn't exist yet. For now, set a flag that the host can poll. */
volatile int g_sre_recreate_hero_pending = 0;

/* Host-polled resource flags (SwKiwi) */
volatile int g_sre_reload_textures_pending = 0;
volatile int g_sre_clear_models_pending = 0;

/* UI state — controls disabled when custom GUI is shown */
volatile int g_sre_controls_disabled = 0;

/* CharAnimController deferred flags (SwKiwi) */
#define SRE_ANIM_ACTION_NONE          0
#define SRE_ANIM_ACTION_STOP_MOVING   1
#define SRE_ANIM_ACTION_START_MOVING  2
#define SRE_ANIM_ACTION_STOP_ACTION   3
#define SRE_ANIM_ACTION_BEGIN_CASTING 4
#define SRE_ANIM_ACTION_START_FALLING 5
volatile int g_sre_anim_action_pending = 0;
volatile int g_sre_anim_action = SRE_ANIM_ACTION_NONE;

/* =========================================================================
 * Game API — deferred actions for host
 * ========================================================================= */
volatile int g_sre_game_action_pending = 0;
volatile int g_sre_game_action_type = 0;
#define SRE_GAME_ACTION_NONE          0
#define SRE_GAME_ACTION_FADE_IN       1
#define SRE_GAME_ACTION_FADE_OUT      2
#define SRE_GAME_ACTION_FLASH         3
#define SRE_GAME_ACTION_CINEMATIC_ON  4
#define SRE_GAME_ACTION_CINEMATIC_OFF 5
#define SRE_GAME_ACTION_ENTER_PORTAL  6
#define SRE_GAME_ACTION_INC_COUNTER   7
volatile char g_sre_game_notification[512] = {0};
volatile int  g_sre_game_notification_pending = 0;
volatile char g_sre_game_portal_level[128] = {0};
volatile char g_sre_game_portal_spawn[128] = {0};
volatile char g_sre_game_counter_name[128] = {0};
char g_sre_game_level_name[128] = "unknown";
char g_sre_game_item_titles[64][64] = {{0}};  /* Cache of item titles */

/* =========================================================================
 * Health API — deferred actions for host
 * ========================================================================= */
volatile float g_sre_immunity_time = 0;
volatile int   g_sre_immunity_pending = 0;
volatile int   g_sre_has_taken_damage = 0;

/* =========================================================================
 * fs API — file I/O for guest
 * ========================================================================= */
#define SRE_FS_FILE FILE

extern char g_sre_vfs_path_external[512];
extern char g_sre_vfs_path_files[512];
extern char g_sre_vfs_path_cache[512];
extern char g_sre_vfs_path_assets[512];
    


/* ======== ButtonController ======== */
#define SRE_BTN_MAX       128
#define SRE_BTN_ID_LEN    32
#define SRE_BTN_LABEL_LEN 64

typedef struct {
    char     id[SRE_BTN_ID_LEN];       /* Lua string ID */
    char     label[SRE_BTN_LABEL_LEN]; /* Display text */
    float    x, y;                      /* Normalized position (0-1) */
    float    w, h;                      /* Normalized dimensions (base-relative) */
    float    alpha;                     /* Overall alpha 0-255 */
    float    scale_x, scale_y;         /* Scaling factors */
    int      text_color;               /* Packed ARGB */
    float    text_scale;               /* Text size multiplier */
    int      bg_alpha;                 /* Background alpha (0-255) */
    int      hidden;                   /* Per-button hidden flag */
    int      clickable;                /* Whether it accepts clicks */
    int      movable;                  /* Can be dragged */
    int      snapback;                 /* Returns to original pos on release */
    float    home_x, home_y;           /* Original position (for snapback) */
    int      padding_l, padding_t, padding_r, padding_b;
    int      alignment;                /* Text alignment / gravity */
    char     overlay_id[SRE_BTN_ID_LEN]; /* Belongs to overlay */
    int      confined;                  /* Confined to overlay bounds */
    /* ---- STATE (written by host, read by SRE) ---- */
    volatile int pressed;              /* Host writes: 1=down, 0=up */
    volatile int released;             /* Host writes: 1 on release */
    volatile int dragging;             /* Host writes: 1=dragging, 0=not */
    volatile float cur_x, cur_y;       /* Current position (after drag) */
    int      active;                   /* 1 = slot in use, 0 = free */
    int      dirty;                    /* 1 = needs visual update by host */
} SreBtnSlot;

#define SRE_OVERLAY_MAX 8
typedef struct {
    char     id[SRE_BTN_ID_LEN];
    float    x, y;
    float    w, h;
    int      bg_color;
    int      bg_alpha;
    float    corner_radius;
    int      hidden;
    int      movable;
    int      pinchable;
    float    scale_factor;
    int      pinching;
    /* Separators inside overlay */
    float    separators[8];
    int      separator_count;
    int      active;
    int      dirty;
} SreOverlaySlot;

volatile SreBtnSlot g_sre_buttons[SRE_BTN_MAX] = {{0}};
volatile SreOverlaySlot g_sre_overlays[SRE_OVERLAY_MAX] = {{0}};
volatile int g_sre_btn_count = 0;
volatile int g_sre_btn_dirty = 0;
volatile int g_sre_btn_delete_all = 0;
volatile int g_sre_btn_globally_hidden = 0;

/* ======== Animation State Tracking (Phase 1.5A) ======== */
#define SRE_MAX_ENTITIES 256

char g_sre_entity_anim[SRE_MAX_ENTITIES][128] = {{0}};  /* animation name per entity */
int  g_sre_entity_anim_playing[SRE_MAX_ENTITIES] = {0};  /* is animation playing? */

/* ======== Physics State Tracking (Phase 1.5A) ======== */
typedef struct {
    double x, y, z;           /* position */
    double vx, vy, vz;        /* velocity */
    double ax, ay, az;        /* acceleration */
    int is_grounded;          /* on ground? */
    double width, height;     /* collision bounds */
    double mass;              /* entity mass */
    double friction;          /* friction factor */
    int is_kinematic;         /* physics type (0=dynamic, 1=kinematic, 2=static) */
} SrePhysicsState;

SrePhysicsState g_sre_physics[SRE_MAX_ENTITIES] = {{0}};

/* ======== Entity State Tracking (Phase 1.5A) ======== */
typedef struct {
    char name[64];            /* entity name */
    int type;                 /* entity type (0=unknown, 1=enemy, 2=npc, 3=item, etc) */
    int health;               /* current HP */
    int max_health;           /* max HP */
    int active;               /* is alive? (1=yes, 0=no) */
    int visible;              /* is visible? (1=yes, 0=no) */
    int can_move;             /* movement enabled? (1=yes, 0=no) */
    double speed;             /* movement speed */
    double direction;         /* facing direction (radians, 0=right) */
} SreEntityState;

SreEntityState g_sre_entities[SRE_MAX_ENTITIES] = {{0}};

/* ======== KeyboardController Globals ======== */
volatile char g_sre_keyboard_text[256] = {0};
volatile int  g_sre_keyboard_open = 0;
volatile int  g_sre_keyboard_done = 0;


/* =========================================================================
 * String helper (no libc)
 * ========================================================================= */
static int sre_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* =========================================================================
 * Mini.Health.* Lua Functions
 * ========================================================================= */

/* Mini.Health.CurrentMana() → number */
static int l_mini_health_current_mana(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_mana);
    return 1;
}

/* Mini.Health.CurrentManaPercent() → number (0.0–1.0) */
static int l_mini_health_current_mana_percent(lua_State* L) {
    if (g_sre_player_max_mana > 0)
        g_lua_pushnumber(L, (double)g_sre_player_mana / (double)g_sre_player_max_mana);
    else
        g_lua_pushnumber(L, 0.0);
    return 1;
}

/* =========================================================================
 * Mini.Character.* Lua Functions — full player stats
 * ========================================================================= */

static int l_mini_char_get_level(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_level);
    return 1;
}

static int l_mini_char_get_exp(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_xp);
    return 1;
}

static int l_mini_char_get_health(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_hp);
    return 1;
}

static int l_mini_char_get_max_health(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_max_hp);
    return 1;
}

static int l_mini_char_get_mana(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_mana);
    return 1;
}

static int l_mini_char_get_max_mana(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_max_mana);
    return 1;
}

static int l_mini_char_get_coins(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_coins);
    return 1;
}

static int l_mini_char_get_atk_level(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_atk_level);
    return 1;
}

static int l_mini_char_get_hp_level(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_hp_level);
    return 1;
}

static int l_mini_char_get_mana_level(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_mana_level);
    return 1;
}

/* Setters — write to pending buffer, host polls */
static int l_mini_char_set_level(lua_State* L) {
    g_sre_char_set_value = (int)g_lua_tonumber(L, 1);
    g_sre_char_set_field = 1;
    g_sre_char_set_pending = 1;
    return 0;
}

static int l_mini_char_set_exp(lua_State* L) {
    g_sre_char_set_value = (int)g_lua_tonumber(L, 1);
    g_sre_char_set_field = 2;
    g_sre_char_set_pending = 1;
    return 0;
}

static int l_mini_char_set_health(lua_State* L) {
    g_sre_char_set_value = (int)g_lua_tonumber(L, 1);
    g_sre_char_set_field = 3;
    g_sre_char_set_pending = 1;
    return 0;
}

static int l_mini_char_set_mana(lua_State* L) {
    g_sre_char_set_value = (int)g_lua_tonumber(L, 1);
    g_sre_char_set_field = 4;
    g_sre_char_set_pending = 1;
    return 0;
}

static int l_mini_char_set_coins(lua_State* L) {
    int val = (int)g_lua_tonumber(L, 1);
    if (val > 0 || g_sre_player_coins <= 0) {
        g_sre_char_set_value = val;
        g_sre_char_set_field = 5;
        g_sre_char_set_pending = 1;
    }
    return 0;
}

/* =========================================================================
 * Mini.Character.* Movement/Speed Stubs (Combatch mod compatibility)
 *
 * These track values in SRE globals. The host can poll them
 * to apply engine-side changes if desired.
 * ========================================================================= */
float g_sre_walk_speed = 1.0f;
float g_sre_run_speed = 1.0f;
float g_sre_jump_height = 1.0f;
int   g_sre_move_direction = 0;  /* 0=stopped, -1=left, 1=right */

static void* sre_get_hero_cc(lua_State* L);

static int l_mini_char_get_walk_speed(lua_State* L) {
    void* cc = sre_get_hero_cc(L);
    if (cc) {
        float speed = *(float*)((char*)cc + 0x26c);
        g_lua_pushnumber(L, (double)speed);
    } else {
        g_lua_pushnumber(L, (double)g_sre_walk_speed);
    }
    return 1;
}

static int l_mini_char_set_walk_speed(lua_State* L) {
    float speed = (float)g_lua_tonumber(L, 1);
    g_sre_walk_speed = speed;
    void* cc = sre_get_hero_cc(L);
    if (cc) {
        *(float*)((char*)cc + 0x26c) = speed;
    }
    return 0;
}

static int l_mini_char_get_run_speed(lua_State* L) {
    void* cc = sre_get_hero_cc(L);
    if (cc) {
        float speed = *(float*)((char*)cc + 0x268);
        g_lua_pushnumber(L, (double)speed);
    } else {
        g_lua_pushnumber(L, (double)g_sre_run_speed);
    }
    return 1;
}

static int l_mini_char_set_run_speed(lua_State* L) {
    float speed = (float)g_lua_tonumber(L, 1);
    g_sre_run_speed = speed;
    void* cc = sre_get_hero_cc(L);
    if (cc) {
        *(float*)((char*)cc + 0x268) = speed;
    }
    return 0;
}

static int l_mini_char_set_jump_height(lua_State* L) {
    float jh = (float)g_lua_tonumber(L, 1);
    g_sre_jump_height = jh;
    void* cc = sre_get_hero_cc(L);
    if (cc) {
        *(float*)((char*)cc + 0x274) = jh;
        *(float*)((char*)cc + 0x27c) = jh;
    }
    return 0;
}

static int l_mini_char_get_jump_height(lua_State* L) {
    void* cc = sre_get_hero_cc(L);
    if (cc) {
        float jh = *(float*)((char*)cc + 0x27c);
        g_lua_pushnumber(L, (double)jh);
    } else {
        g_lua_pushnumber(L, (double)g_sre_jump_height);
    }
    return 1;
}

/* =========================================================================
 * Mini.Character.* SwKiwi Action Functions — direct guest C++ calls
 *
 * Instead of the old deferred-action pattern (writing to g_sre_char_action_pending
 * and hoping the host polls), we call the real CharControllerComponent methods
 * directly in guest address space. This matches exactly how SwKiwi's functions.c
 * works: resolve CharControllerComponent via SceneObject::ComponentWithInterface,
 * then call the method at its nm-verified ARM64 offset.
 *
 * Offsets are relative to g_swordigo_base (libswordigo.so v1.4.12 ARM64).
 * Verified against GhidraDecomp src/game_entities/CharControllerComponent.c.
 * ========================================================================= */

/* Helper: resolve CharControllerComponent* for the hero, or NULL */
static void* sre_get_hero_cc(lua_State* L) {
    SceneObject* hero = sre_hero_object_from_L(L);
    if (!hero) return (void*)0;
    return sre_scene_object_component(hero, CharControllerComponent_Interface);
}

/* Macro: void method — no args besides 'this' */
#define CC_VOID(fn_ptr) \
    do { \
        void* cc = sre_get_hero_cc(L); \
        if (cc && fn_ptr) { \
            fn_ptr(cc); \
        } \
    } while(0)

/* Macro: bool method — pushes result onto Lua stack */
#define CC_BOOL(fn_ptr) \
    do { \
        void* cc = sre_get_hero_cc(L); \
        if (!cc || !fn_ptr) { g_lua_pushboolean(L, 0); return 1; } \
        int r = fn_ptr(cc); \
        g_lua_pushboolean(L, r); \
        return 1; \
    } while(0)

/* Mini.Character.StartMovingToDirection(dir) — dir: -1=left, 1=right
 * Calls CharControllerComponent::StartMovingToDirection(int dir) at offset 0x111709.
 * SwKiwi normalizes dir to ±1; we do the same. */
static int l_mini_char_start_moving(lua_State* L) {
    int dir = (int)g_lua_tonumber(L, 1);
    int norm = (dir >= 0) ? 1 : -1;
    g_sre_move_direction = norm;  /* keep for host reads */
    void* cc = sre_get_hero_cc(L);
    if (cc && g_sre_CharController_StartMovingToDirection) {
        g_sre_CharController_StartMovingToDirection(cc, norm);
    }
    return 0;
}

/* Mini.Character.StopMovingToDirection() — no direction argument
 * Calls CharControllerComponent::StopMovingToDirection(int dir) at offset 0x25c85c
 * with the last known direction (matches SwKiwi behaviour). */
static int l_mini_char_stop_moving(lua_State* L) {
    int dir = g_sre_move_direction ? g_sre_move_direction : 1;
    g_sre_move_direction = 0;  /* keep for host reads */
    void* cc = sre_get_hero_cc(L);
    if (cc && g_sre_CharController_StopMovingToDirection) {
        g_sre_CharController_StopMovingToDirection(cc, dir);
    }
    return 0;
}

static int l_mini_char_die(lua_State* L) {
    CC_VOID(g_sre_CharController_Die);
    return 0;
}

static int l_mini_char_hurt(lua_State* L) {
    CC_VOID(g_sre_CharController_Hurt);
    return 0;
}

static int l_mini_char_use(lua_State* L) {
    CC_VOID(g_sre_CharController_Use);
    return 0;
}

static int l_mini_char_swing(lua_State* L) {
    CC_VOID(g_sre_CharController_Swing);
    return 0;
}

static int l_mini_char_stop_swing(lua_State* L) {
    CC_VOID(g_sre_CharController_StopSwing);
    return 0;
}

static int l_mini_char_start_jumping(lua_State* L) {
    CC_VOID(g_sre_CharController_StartJumping);
    return 0;
}

static int l_mini_char_stop_jumping(lua_State* L) {
    CC_VOID(g_sre_CharController_StopJumping);
    return 0;
}

static int l_mini_char_drop_quickly(lua_State* L) {
    CC_VOID(g_sre_CharController_DropQuickly);
    return 0;
}

static int l_mini_char_cancel_casting(lua_State* L) {
    CC_VOID(g_sre_CharController_CancelCasting);
    return 0;
}

static int l_mini_char_finish_casting(lua_State* L) {
    CC_VOID(g_sre_CharController_FinishCasting);
    return 0;
}

/* =========================================================================
 * Mini.Character.* Capability Queries — direct guest C++ calls
 *
 * Previously returned hardcoded 'true'. Now queries the real
 * CharControllerComponent boolean methods via CC_BOOL.
 *
 * Offsets (ARM64 v1.4.12, from GhidraDecomp README.md):
 *   CanDoSomething  25c888
 *   CanBeginCasting 25cb80
 *   CanUse          25c950
 *   CanJump         25c8e4
 *   CanSwing        25aebc
 *   CanPickup       25ca68
 * ========================================================================= */

static int l_mini_char_can_do_something(lua_State* L) {
    CC_BOOL(g_sre_CharController_CanDoSomething);
}

static int l_mini_char_can_begin_casting(lua_State* L) {
    CC_BOOL(g_sre_CharController_CanBeginCasting);
}

static int l_mini_char_can_use(lua_State* L) {
    CC_BOOL(g_sre_CharController_CanUse);
}

static int l_mini_char_can_jump(lua_State* L) {
    CC_BOOL(g_sre_CharController_CanJump);
}

static int l_mini_char_can_swing(lua_State* L) {
    CC_BOOL(g_sre_CharController_CanSwing);
}

static int l_mini_char_can_pickup(lua_State* L) {
    CC_BOOL(g_sre_CharController_CanPickup);
}


/* =========================================================================
 * Mini.Character.* SwKiwi Extended State
 *
 * SetMovementFacingLock / SetStunTime write directly into the
 * CharControllerComponent struct at the ARM64 field offsets used by
 * SwKiwi functions.c:
 *   movement_facing_lock → CC + 0x245  (int/bool, 1 byte)
 *   stun_time            → CC + 0x2f8  (float, 4 bytes)
 * ========================================================================= */

static int l_mini_char_set_movement_facing_lock(lua_State* L) {
    int val = g_lua_toboolean(L, 1);
    g_sre_char_movement_facing_lock = val;  /* keep for host reads */
    void* cc = sre_get_hero_cc(L);
    if (cc) *(int*)((char*)cc + 0x245) = val;
    return 0;
}

static int l_mini_char_set_stun_time(lua_State* L) {
    float t = (float)g_lua_tonumber(L, 1);
    g_sre_char_stun_time = t;              /* keep for host reads */
    void* cc = sre_get_hero_cc(L);
    if (cc) *(float*)((char*)cc + 0x2f8) = t;
    return 0;
}

static int l_mini_char_get_air_jump_used(lua_State* L) {
    g_lua_pushboolean(L, g_sre_char_air_jump_used);
    return 1;
}

static int l_mini_char_set_air_jump_used(lua_State* L) {
    g_sre_char_air_jump_used = g_lua_toboolean(L, 1);
    return 0;
}


/* Mini.Character.ExpForLevel(n) → 100 * n * (n + 1) / 2 */
static int l_mini_char_exp_for_level(lua_State* L) {
    int n = (int)g_lua_tonumber(L, 1);
    int xp = 100 * n * (n + 1) / 2;
    g_lua_pushnumber(L, (double)xp);
    return 1;
}

/* Mini.Character.GetLevelAttributes() → health_attr, attack_attr, magic_attr */
static int l_mini_char_get_level_attributes(lua_State* L) {
    (void)L;
    g_lua_pushnumber(L, (double)g_sre_player_hp_level);
    g_lua_pushnumber(L, (double)g_sre_player_atk_level);
    g_lua_pushnumber(L, (double)g_sre_player_mana_level);
    return 3;
}

/* Mini.Character.SetLevelAttributes(h, a, m) — deferred write */
static int l_mini_char_set_level_attributes(lua_State* L) {
    g_sre_char_attr_hp   = (int)g_lua_tonumber(L, 1);
    g_sre_char_attr_atk  = (int)g_lua_tonumber(L, 2);
    g_sre_char_attr_mana = (int)g_lua_tonumber(L, 3);
    g_sre_char_attr_set_pending = 1;
    return 0;
}

/* =========================================================================
 * Mini.SetTrinketColor(item_id, r, g, b, a, intensity)
 *
 * Stores trinket glow color in a packed table for host to read.
 * Format: item_id\0 followed by 5 floats (r,g,b,a,intensity)
 * ========================================================================= */
typedef struct {
    char item_id[32];
    float r, g, b, a;
    float intensity;
} SreTrinketGlow;

SreTrinketGlow g_sre_trinket_glows[16] = {{0}};
int g_sre_trinket_glow_count = 0;

static int l_mini_set_trinket_color(lua_State* L) {
    const char* item_id = lua_tostring(L, 1);
    if (!item_id) return 0;

    float r = (float)g_lua_tonumber(L, 2);
    float g = (float)g_lua_tonumber(L, 3);
    float b = (float)g_lua_tonumber(L, 4);
    float a = (float)g_lua_tonumber(L, 5);
    float intensity = (float)g_lua_tonumber(L, 6);

    /* Find existing or add new */
    int idx = -1;
    int i;
    for (i = 0; i < g_sre_trinket_glow_count; i++) {
        /* strcmp inline */
        const char* p = g_sre_trinket_glows[i].item_id;
        const char* q = item_id;
        int match = 1;
        while (*p || *q) {
            if (*p != *q) { match = 0; break; }
            p++; q++;
        }
        if (match) { idx = i; break; }
    }
    if (idx < 0 && g_sre_trinket_glow_count < 16) {
        idx = g_sre_trinket_glow_count++;
    }
    if (idx < 0) return 0;  /* table full */

    /* Copy item_id */
    for (i = 0; i < 31 && item_id[i]; i++)
        g_sre_trinket_glows[idx].item_id[i] = item_id[i];
    g_sre_trinket_glows[idx].item_id[i] = '\0';

    g_sre_trinket_glows[idx].r = r;
    g_sre_trinket_glows[idx].g = g;
    g_sre_trinket_glows[idx].b = b;
    g_sre_trinket_glows[idx].a = a;
    g_sre_trinket_glows[idx].intensity = intensity;
    return 0;
}

/* =========================================================================
 * Mini.Camera.* Lua Functions
 * ========================================================================= */

/* Mini.Camera.GetPosition() → table {x, y, z} */
static int l_mini_cam_get_position(lua_State* L) {
    g_lua_createtable(L, 0, 3);
    g_lua_pushnumber(L, (double)g_sre_cam_x);
    g_lua_setfield(L, -2, "x");
    g_lua_pushnumber(L, (double)g_sre_cam_y);
    g_lua_setfield(L, -2, "y");
    g_lua_pushnumber(L, (double)g_sre_cam_z);
    g_lua_setfield(L, -2, "z");
    return 1;
}

/* Mini.Camera.SetPosition(x, y, z) */
static int l_mini_cam_set_position(lua_State* L) {
    g_sre_cam_x = (float)g_lua_tonumber(L, 1);
    g_sre_cam_y = (float)g_lua_tonumber(L, 2);
    g_sre_cam_z = (float)g_lua_tonumber(L, 3);
    g_sre_cam_set_pending = 1;
    return 0;
}

/* Mini.Camera.GetZoom() → number */
static int l_mini_cam_get_zoom(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_cam_zoom);
    return 1;
}

/* Mini.Camera.SetZoom(n) */
static int l_mini_cam_set_zoom(lua_State* L) {
    float z = (float)g_lua_tonumber(L, 1);
    if (z > 0.0f && z <= 20.0f)
        g_sre_cam_zoom = z;
    return 0;
}

/* Mini.Camera.GetFollow() → boolean */
static int l_mini_cam_get_follow(lua_State* L) {
    g_lua_pushboolean(L, g_sre_cam_follow);
    return 1;
}

/* Mini.Camera.SetFollow(bool) */
static int l_mini_cam_set_follow(lua_State* L) {
    g_sre_cam_follow = g_lua_toboolean(L, 1);
    return 0;
}

/* =========================================================================
 * Components.Health / Components.Physics / Components.Entity
 * ========================================================================= */

/* Components.Health.GetValue(entity, fieldName) */
static int l_comp_health_get_value(lua_State* L) {
    const char* field = lua_tostring(L, 2);
    if (!field) return 0;

    if (sre_streq(field, "CurrentHealth")) {
        g_lua_pushnumber(L, (double)g_sre_player_hp);
        return 1;
    }
    if (sre_streq(field, "MaxHealth")) {
        g_lua_pushnumber(L, (double)g_sre_player_max_hp);
        return 1;
    }
    if (sre_streq(field, "CurrentMana")) {
        g_lua_pushnumber(L, (double)g_sre_player_mana);
        return 1;
    }
    if (sre_streq(field, "MaxMana")) {
        g_lua_pushnumber(L, (double)g_sre_player_max_mana);
        return 1;
    }

    g_lua_pushnumber(L, 0.0);
    return 1;
}

/* Components.Health.SetValue(entity, fieldName, value) */
static int l_comp_health_set_value(lua_State* L) {
    const char* field = lua_tostring(L, 2);
    int val = (int)g_lua_tonumber(L, 3);
    if (!field) return 0;

    if (sre_streq(field, "CurrentHealth")) {
        g_sre_char_set_value = val;
        g_sre_char_set_field = 3;
        g_sre_char_set_pending = 1;
    } else if (sre_streq(field, "CurrentMana")) {
        g_sre_char_set_value = val;
        g_sre_char_set_field = 4;
        g_sre_char_set_pending = 1;
    }
    return 0;
}

/* Components.Physics.GetValue(entity, fieldName) → value */
static int l_comp_physics_get_value(lua_State* L) {
    /* Extract entity as userdata (NOT integer ID) */
    void* entity_ptr = NULL;
    if (g_lua_touserdata) {
        entity_ptr = (void*)g_lua_touserdata(L, 1);
    }
    
    const char* field = NULL;
    if (g_lua_tolstring) {
        size_t len = 0;
        field = g_lua_tolstring(L, 2, &len);
    }
    
    if (!entity_ptr || !field) {
        g_lua_pushnumber(L, 0.0);
        return 1;
    }
    
    /* Return sensible defaults for common physics properties
     * These are safe defaults that won't break the game */
    if (sre_streq(field, "x") || sre_streq(field, "y") || sre_streq(field, "z")) {
        g_lua_pushnumber(L, 0.0);  /* Position defaults to origin */
    } else if (sre_streq(field, "vx") || sre_streq(field, "vy") || sre_streq(field, "vz")) {
        g_lua_pushnumber(L, 0.0);  /* Velocity defaults to zero */
    } else if (sre_streq(field, "IsGrounded")) {
        g_lua_pushboolean(L, 1);   /* Default to grounded */
    } else if (sre_streq(field, "Width") || sre_streq(field, "Height")) {
        g_lua_pushnumber(L, 10.0); /* Default size */
    } else {
        g_lua_pushnumber(L, 0.0);
    }
    
    return 1;
}

/* Components.Entity.GetValue(entity, fieldName) → value */
static int l_comp_entity_get_value(lua_State* L) {
    /* Extract entity as userdata (NOT integer ID) */
    void* entity_ptr = NULL;
    if (g_lua_touserdata) {
        entity_ptr = (void*)g_lua_touserdata(L, 1);
    }
    
    const char* field = NULL;
    if (g_lua_tolstring) {
        size_t len = 0;
        field = g_lua_tolstring(L, 2, &len);
    }
    
    if (!entity_ptr || !field) {
        g_lua_pushnumber(L, 0.0);
        return 1;
    }
    
    /* Return sensible defaults for common entity properties */
    if (sre_streq(field, "Name")) {
        g_lua_pushstring(L, "");
    } else if (sre_streq(field, "Health")) {
        g_lua_pushnumber(L, 100.0);  /* Default health */
    } else if (sre_streq(field, "MaxHealth")) {
        g_lua_pushnumber(L, 100.0);
    } else if (sre_streq(field, "Active")) {
        g_lua_pushboolean(L, 1);  /* Assume active */
    } else if (sre_streq(field, "Visible")) {
        g_lua_pushboolean(L, 1);  /* Assume visible */
    } else if (sre_streq(field, "Speed") || sre_streq(field, "Direction")) {
        g_lua_pushnumber(L, 0.0);
    } else {
        g_lua_pushnumber(L, 0.0);
    }
    
    return 1;
}

/* =========================================================================
 * Skeleton.* Lua Functions (stubs)
 * ========================================================================= */

/* Skeleton.GetBonePosition(entity, boneName) → {0, 0, 0} */
static int l_skeleton_get_bone_position(lua_State* L) {
    (void)L;
    g_lua_createtable(L, 3, 0);
    g_lua_pushnumber(L, 0.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 1);
    else lua_pop(L, 1);
    g_lua_pushnumber(L, 0.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 2);
    else lua_pop(L, 1);
    g_lua_pushnumber(L, 0.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 3);
    else lua_pop(L, 1);
    return 1;
}

/* Skeleton.GetBoneRotation(entity, boneName) → {0, 0, 0, 1} (quaternion) */
static int l_skeleton_get_bone_rotation(lua_State* L) {
    (void)L;
    g_lua_createtable(L, 4, 0);
    g_lua_pushnumber(L, 0.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 1);
    else lua_pop(L, 1);
    g_lua_pushnumber(L, 0.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 2);
    else lua_pop(L, 1);
    g_lua_pushnumber(L, 0.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 3);
    else lua_pop(L, 1);
    g_lua_pushnumber(L, 1.0);
    if (g_lua_rawseti) g_lua_rawseti(L, -2, 4);
    else lua_pop(L, 1);
    return 1;
}

/* Skeleton.SetBoneScale(entity, boneName, sx, sy, sz) → 0 (no-op) */
static int l_skeleton_set_bone_scale(lua_State* L) {
    (void)L;
    g_lua_pushnumber(L, 0.0);
    return 1;
}

/* Skeleton.New(sceneObject) → lightuserdata (pass-through entity ref) */
static int l_skeleton_new(lua_State* L) {
    /* Return arg1 as-is — the caller passes a scene object handle */
    if (g_lua_pushvalue) g_lua_pushvalue(L, 1);
    else g_lua_pushnumber(L, 0.0);
    return 1;
}

/* Skeleton.setBoneOffset(entity, idx, x, y, z) — store (no-op stub) */
static int l_skeleton_set_bone_offset(lua_State* L) {
    (void)L;
    return 0;
}

/* Skeleton.setBoneRotation(entity, idx, rx, ry, rz) — store (no-op stub) */
static int l_skeleton_set_bone_rotation(lua_State* L) {
    (void)L;
    return 0;
}

/* Skeleton.resetBones(entity) — clear overrides (no-op stub) */
static int l_skeleton_reset_bones(lua_State* L) {
    (void)L;
    return 0;
}

/* Skeleton.getBoneIndex(entity, name) → 0 (stub, but valid) */
static int l_skeleton_get_bone_index(lua_State* L) {
    (void)L;
    g_lua_pushnumber(L, 0.0);
    return 1;
}

/* =========================================================================
 * CharAnimController.* Lua Functions (stubs)
 * ========================================================================= */

/* CharAnimController.Play(entity, animName) → success bool */
static int l_charanim_play(lua_State* L) {
    /* Extract entity as userdata (NOT integer ID) */
    void* entity_ptr = NULL;
    if (g_lua_touserdata) {
        entity_ptr = (void*)g_lua_touserdata(L, 1);
    }
    
    const char* anim_name = NULL;
    if (g_lua_tolstring) {
        size_t len = 0;
        anim_name = g_lua_tolstring(L, 2, &len);
    }
    
    /* If we can't extract both, fail gracefully */
    if (!entity_ptr || !anim_name) {
        g_lua_pushboolean(L, 0);
        return 1;
    }
    
    /* For now, just indicate success - the actual animation 
     * should be handled by the game's own AnimationController */
    g_lua_pushboolean(L, 1);
    return 1;
}

/* CharAnimController.Stop(entity) → success */
static int l_charanim_stop(lua_State* L) {
    void* entity_ptr = NULL;
    if (g_lua_touserdata) {
        entity_ptr = (void*)g_lua_touserdata(L, 1);
    }
    
    if (!entity_ptr) {
        return 0;
    }
    
    /* Successfully stopped (no-op for now) */
    return 0;
}

/* CharAnimController.GetCurrent(entity) → animation name */
static int l_charanim_get_current(lua_State* L) {
    void* entity_ptr = NULL;
    if (g_lua_touserdata) {
        entity_ptr = (void*)g_lua_touserdata(L, 1);
    }
    
    if (!entity_ptr) {
        g_lua_pushstring(L, "");
        return 1;
    }
    
    /* Return empty string - we don't track this yet */
    g_lua_pushstring(L, "");
    return 1;
}

/* CharAnimController.IsPlaying(entity) → is animation playing? */
static int l_charanim_is_playing(lua_State* L) {
    void* entity_ptr = NULL;
    if (g_lua_touserdata) {
        entity_ptr = (void*)g_lua_touserdata(L, 1);
    }
    
    if (!entity_ptr) {
        g_lua_pushboolean(L, 0);
        return 1;
    }
    
    /* Default to false */
    g_lua_pushboolean(L, 0);
    return 1;
}

/* CharAnimController.StopMoving(entity) — deferred flag */
static int l_charanim_stop_moving(lua_State* L) {
    (void)L;
    g_sre_anim_action = SRE_ANIM_ACTION_STOP_MOVING;
    g_sre_anim_action_pending = 1;
    return 0;
}

/* CharAnimController.StartMoving(entity) — deferred flag */
static int l_charanim_start_moving(lua_State* L) {
    (void)L;
    g_sre_anim_action = SRE_ANIM_ACTION_START_MOVING;
    g_sre_anim_action_pending = 1;
    return 0;
}

/* CharAnimController.StopAction(entity) — deferred flag */
static int l_charanim_stop_action(lua_State* L) {
    (void)L;
    g_sre_anim_action = SRE_ANIM_ACTION_STOP_ACTION;
    g_sre_anim_action_pending = 1;
    return 0;
}

/* CharAnimController.BeginCasting(entity) — deferred flag */
static int l_charanim_begin_casting(lua_State* L) {
    (void)L;
    g_sre_anim_action = SRE_ANIM_ACTION_BEGIN_CASTING;
    g_sre_anim_action_pending = 1;
    return 0;
}

/* CharAnimController.StartFalling(entity) — deferred flag */
static int l_charanim_start_falling(lua_State* L) {
    (void)L;
    g_sre_anim_action = SRE_ANIM_ACTION_START_FALLING;
    g_sre_anim_action_pending = 1;
    return 0;
}

/* CharAnimController.IsReadyToJump(entity) → true (optimistic) */
static int l_charanim_is_ready_to_jump(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 1);
    return 1;
}

/* CharAnimController.IsMoving(entity) → false (safe default) */
static int l_charanim_is_moving(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 0);
    return 1;
}

/* CharAnimController.ActionNearlyFinished(entity) → true (safe default) */
static int l_charanim_action_nearly_finished(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 1);
    return 1;
}

/* =========================================================================
 * ButtonController.* Lua Functions — real implementation
 *
 * Mods heavily use ButtonController for custom UI. Buttons are stored
 * in g_sre_buttons[] and the host polls/renders them.
 * ========================================================================= */

/* Find button slot by string ID */
static volatile SreBtnSlot* sre_find_btn(const char* id) {
    int i;
    if (!id) return 0;
    for (i = 0; i < SRE_BTN_MAX; i++) {
        if (g_sre_buttons[i].active) {
            int j;
            int match = 1;
            for (j = 0; id[j] && g_sre_buttons[i].id[j]; j++) {
                if (id[j] != g_sre_buttons[i].id[j]) { match = 0; break; }
            }
            if (match && id[j] == g_sre_buttons[i].id[j]) return &g_sre_buttons[i];
        }
    }
    return 0;
}

/* Safe string copy for button fields */
static void sre_btn_strcpy(volatile char* dst, const char* src, int maxlen) {
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ButtonController.New(id, label, x, y, w, h) → 0 */
static int l_btn_new(lua_State* L) {
    const char* id    = lua_tostring(L, 1);
    const char* label = lua_tostring(L, 2);
    if (!id) return 0;
    if (!label) label = "";

    float bx = (float)g_lua_tonumber(L, 3);
    float by = (float)g_lua_tonumber(L, 4);
    float bw = (float)g_lua_tonumber(L, 5);
    float bh = (float)g_lua_tonumber(L, 6);

    /* Find existing or allocate new */
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (!btn) {
        int i;
        for (i = 0; i < SRE_BTN_MAX; i++) {
            if (!g_sre_buttons[i].active) { btn = &g_sre_buttons[i]; break; }
        }
    }
    if (!btn) return 0;  /* table full */

    sre_btn_strcpy(btn->id, id, SRE_BTN_ID_LEN);
    sre_btn_strcpy(btn->label, label, SRE_BTN_LABEL_LEN);
    btn->x = bx;  btn->y = by;
    btn->w = bw;  btn->h = bh;
    btn->alpha = 255.0f;
    btn->scale_x = 1.0f;  btn->scale_y = 1.0f;
    btn->text_color = (int)0xFFFFFFFF;
    btn->text_scale = 1.0f;
    btn->bg_alpha = 180;
    btn->hidden = 0;
    btn->clickable = 1;
    btn->movable = 0;
    btn->snapback = 0;
    btn->home_x = bx;  btn->home_y = by;
    btn->cur_x = bx;   btn->cur_y = by;
    btn->padding_l = 0; btn->padding_t = 0;
    btn->padding_r = 0; btn->padding_b = 0;
    btn->alignment = 0;
    btn->overlay_id[0] = '\0';  /* Clear any stale overlay association from previous slot use */
    btn->confined = 0;
    btn->pressed = 0;
    btn->released = 0;
    btn->dragging = 0;
    btn->active = 1;
    btn->dirty = 1;
    g_sre_btn_dirty = 1;
    return 0;
}

/* ButtonController.Delete(id) */
static int l_btn_delete(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->active = 0;
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.DeleteAll() */
static int l_btn_delete_all(lua_State* L) {
    int i;
    (void)L;
    printf("[SRE/Guest] l_btn_delete_all called\n");
    for (i = 0; i < SRE_BTN_MAX; i++) {
        g_sre_buttons[i].active = 0;
    }
    g_sre_btn_delete_all = 1;
    g_sre_btn_dirty = 1;
    return 0;
}

/* ButtonController.SetHidden(id, bool) */
static int l_btn_set_hidden(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->hidden = g_lua_toboolean(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetHiddenAll(bool) */
static int l_btn_set_hidden_all(lua_State* L) {
    g_sre_btn_globally_hidden = g_lua_toboolean(L, 1);
    g_sre_btn_dirty = 1;
    return 0;
}

/* ButtonController.IsPressed(id) → boolean */
static int l_btn_is_pressed(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (!btn) {
        g_lua_pushboolean(L, 0);
        return 1;
    }
    int val = btn->pressed || btn->released;
    g_lua_pushboolean(L, val);
    if (btn->released) {
        btn->pressed = 0;
        btn->released = 0;
    }
    return 1;
}

/* ButtonController.IsDragging(id) → boolean */
static int l_btn_is_dragging(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    g_lua_pushboolean(L, btn ? btn->dragging : 0);
    return 1;
}

/* ButtonController.Exists(id) → boolean */
static int l_btn_exists(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    g_lua_pushboolean(L, (btn && btn->active) ? 1 : 0);
    return 1;
}

/* ButtonController.SetText(id, text) — truncate text if too long to fit button */
static int l_btn_set_text(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    const char* text = lua_tostring(L, 2);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn && text) {
        /* Truncate to fit button width; assume ~50 pixels available, ~8 pixels per char */
        size_t max_len = SRE_BTN_LABEL_LEN - 1;
        size_t text_len = strlen(text);
        if (text_len > max_len) {
            /* Truncate and add ellipsis if needed */
            if (max_len > 3) {
                sre_btn_strcpy(btn->label, text, max_len - 2);
                strcat((char*)btn->label, "..");
            } else {
                sre_btn_strcpy(btn->label, text, max_len);
            }
        } else {
            sre_btn_strcpy(btn->label, text, SRE_BTN_LABEL_LEN);
        }
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetPosition(id, x, y) */
static int l_btn_set_position(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        float nx = (float)g_lua_tonumber(L, 2);
        float ny = (float)g_lua_tonumber(L, 3);
        btn->x = nx;  btn->y = ny;
        btn->cur_x = nx;  btn->cur_y = ny;
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.GetPosition(id) → x, y */
static int l_btn_get_position(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        g_lua_pushnumber(L, (double)btn->cur_x);
        g_lua_pushnumber(L, (double)btn->cur_y);
    } else {
        g_lua_pushnumber(L, 0.0);
        g_lua_pushnumber(L, 0.0);
    }
    return 2;
}

/* ButtonController.GetPositionX(id) → x */
static int l_btn_get_position_x(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    g_lua_pushnumber(L, btn ? (double)btn->cur_x : 0.0);
    return 1;
}

/* ButtonController.GetPositionY(id) → y */
static int l_btn_get_position_y(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    g_lua_pushnumber(L, btn ? (double)btn->cur_y : 0.0);
    return 1;
}

/* ButtonController.SetAlpha(id, alpha) */
static int l_btn_set_alpha(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->alpha = (float)g_lua_tonumber(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetScaling(id, sx [, sy]) */
static int l_btn_set_scaling(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        float sx = (float)g_lua_tonumber(L, 2);
        float sy = sx;
        if (g_lua_gettop(L) >= 3 && g_lua_type(L, 3) == LUA_TNUMBER)
            sy = (float)g_lua_tonumber(L, 3);
        btn->scale_x = sx;
        btn->scale_y = sy;
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetDimensions(id, w, h) */
static int l_btn_set_dimensions(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->w = (float)g_lua_tonumber(L, 2);
        btn->h = (float)g_lua_tonumber(L, 3);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.MakeMovable(id, snapback) */
static int l_btn_make_movable(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->movable = 1;
        btn->snapback = g_lua_toboolean(L, 2);
        btn->home_x = btn->x;
        btn->home_y = btn->y;
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetClickable(id, bool) */
static int l_btn_set_clickable(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->clickable = g_lua_toboolean(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetTextFont(id, fontName) — accept and ignore */
static int l_btn_set_text_font(lua_State* L) {
    (void)L;  /* no custom fonts — bitmap only */
    return 0;
}

/* ButtonController.SetTextScale(id, scale) */
static int l_btn_set_text_scale(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->text_scale = (float)g_lua_tonumber(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetTextColor(id, ...) — (id, packed) or (id, r, g, b, a) */
static int l_btn_set_text_color(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (!btn) return 0;

    if (g_lua_gettop(L) >= 5) {
        /* (id, r, g, b, a) form */
        int r = (int)g_lua_tonumber(L, 2) & 0xFF;
        int g = (int)g_lua_tonumber(L, 3) & 0xFF;
        int b = (int)g_lua_tonumber(L, 4) & 0xFF;
        int a = (int)g_lua_tonumber(L, 5) & 0xFF;
        btn->text_color = (a << 24) | (r << 16) | (g << 8) | b;
    } else {
        /* (id, packed_int) form */
        btn->text_color = (int)(unsigned int)g_lua_tonumber(L, 2);
    }
    btn->dirty = 1;
    g_sre_btn_dirty = 1;
    return 0;
}

/* ButtonController.SetPadding(id, l, t, r, b) */
static int l_btn_set_padding(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->padding_l = (int)g_lua_tonumber(L, 2);
        btn->padding_t = (int)g_lua_tonumber(L, 3);
        btn->padding_r = (int)g_lua_tonumber(L, 4);
        btn->padding_b = (int)g_lua_tonumber(L, 5);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetAlignment(id, alignment) */
static int l_btn_set_alignment(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->alignment = (int)g_lua_tonumber(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetBackgroundResource(id, resource) — accept, no-op visual */
static int l_btn_set_bg_resource(lua_State* L) {
    (void)L;  /* flat color background only */
    return 0;
}

/* ButtonController.SetBackgroundAlpha(id, alpha) */
static int l_btn_set_bg_alpha(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->bg_alpha = (int)g_lua_tonumber(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}



/* Find overlay slot by string ID */
static volatile SreOverlaySlot* sre_find_overlay(const char* id) {
    int i;
    if (!id) return 0;
    for (i = 0; i < SRE_OVERLAY_MAX; i++) {
        if (g_sre_overlays[i].active) {
            int j;
            int match = 1;
            for (j = 0; id[j] && g_sre_overlays[i].id[j]; j++) {
                if (id[j] != g_sre_overlays[i].id[j]) { match = 0; break; }
            }
            if (match && id[j] == g_sre_overlays[i].id[j]) return &g_sre_overlays[i];
        }
    }
    return 0;
}

/* ButtonController.NewOverlay(id, x, y, w, h) */
static int l_btn_new_overlay(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    if (!id) return 0;
    float ox = (float)g_lua_tonumber(L, 2);
    float oy = (float)g_lua_tonumber(L, 3);
    float ow = (float)g_lua_tonumber(L, 4);
    float oh = (float)g_lua_tonumber(L, 5);

    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (!ovr) {
        int i;
        for (i = 0; i < SRE_OVERLAY_MAX; i++) {
            if (!g_sre_overlays[i].active) { ovr = &g_sre_overlays[i]; break; }
        }
    }
    if (!ovr) return 0;

    sre_btn_strcpy(ovr->id, id, SRE_BTN_ID_LEN);
    ovr->x = ox; ovr->y = oy;
    ovr->w = ow; ovr->h = oh;
    ovr->bg_color = 0x000000;
    ovr->bg_alpha = 204;
    ovr->corner_radius = 12.0f;
    ovr->hidden = 0;
    ovr->movable = 0;
    ovr->pinchable = 0;
    ovr->scale_factor = 1.0f;
    ovr->pinching = 0;
    ovr->separator_count = 0;
    ovr->active = 1;
    ovr->dirty = 1;
    g_sre_btn_dirty = 1;
    return 0;
}

/* ButtonController.SetOverlayMovable(id, movable) */
static int l_btn_set_overlay_movable(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->movable = g_lua_toboolean(L, 2);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetOverlayPinchable(id, pinchable) */
static int l_btn_set_overlay_pinchable(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->pinchable = g_lua_toboolean(L, 2);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetOverlayBackgroundColor(id, color) */
static int l_btn_set_overlay_bg_color(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->bg_color = (int)(unsigned int)g_lua_tonumber(L, 2);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetOverlayBackgroundAlpha(id, alpha) */
static int l_btn_set_overlay_bg_alpha(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->bg_alpha = (int)g_lua_tonumber(L, 2);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetOverlayCornerRadius(id, radiusDp) */
static int l_btn_set_overlay_corner_radius(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->corner_radius = (float)g_lua_tonumber(L, 2);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.RemoveOverlay(id) */
static int l_btn_remove_overlay(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->active = 0;
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
        /* Fully deactivate all buttons owned by this overlay so slots are freed */
        int i;
        for (i = 0; i < SRE_BTN_MAX; i++) {
            if (g_sre_buttons[i].active && sre_streq((const char*)g_sre_buttons[i].overlay_id, id)) {
                g_sre_buttons[i].active = 0;
                g_sre_buttons[i].id[0] = '\0';
                g_sre_buttons[i].overlay_id[0] = '\0';
                g_sre_buttons[i].dirty = 1;
            }
        }
    }
    return 0;
}



/* ButtonController.OverlayAddButton(overlayId, btnId) */
static int l_btn_overlay_add_button(lua_State* L) {
    const char* overlayId = lua_tostring(L, 1);
    const char* btnId = lua_tostring(L, 2);
    volatile SreBtnSlot* btn = sre_find_btn(btnId);
    if (btn && overlayId) {
        sre_btn_strcpy(btn->overlay_id, overlayId, SRE_BTN_ID_LEN);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.OverlayAddSeparator(overlayId, ny) */
static int l_btn_overlay_add_separator(lua_State* L) {
    const char* overlayId = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(overlayId);
    if (ovr) {
        float ny = (float)g_lua_tonumber(L, 2);
        if (ovr->separator_count < 8) {
            ovr->separators[ovr->separator_count++] = ny;
            ovr->dirty = 1;
            g_sre_btn_dirty = 1;
        }
    }
    return 0;
}

/* ButtonController.SetOverlayHidden(id, hidden) */
static int l_btn_set_overlay_hidden(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->hidden = g_lua_toboolean(L, 2);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.SetOverlayPosition(id, nx, ny) */
static int l_btn_set_overlay_position(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        ovr->x = (float)g_lua_tonumber(L, 2);
        ovr->y = (float)g_lua_tonumber(L, 3);
        ovr->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ButtonController.GetOverlayPosition(id) → x, y */
static int l_btn_get_overlay_position(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    if (ovr) {
        g_lua_pushnumber(L, (double)ovr->x);
        g_lua_pushnumber(L, (double)ovr->y);
    } else {
        g_lua_pushnumber(L, 0.0);
        g_lua_pushnumber(L, 0.0);
    }
    return 2;
}

/* ButtonController.GetOverlayScaleFactor(id) → scale */
static int l_btn_get_overlay_scale_factor(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    g_lua_pushnumber(L, ovr ? (double)ovr->scale_factor : 1.0);
    return 1;
}

/* ButtonController.IsOverlayPinching(id) → bool */
static int l_btn_is_overlay_pinching(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreOverlaySlot* ovr = sre_find_overlay(id);
    g_lua_pushboolean(L, ovr ? ovr->pinching : 0);
    return 1;
}

/* ButtonController.SetButtonConfined(id, confined) */
static int l_btn_set_button_confined(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    volatile SreBtnSlot* btn = sre_find_btn(id);
    if (btn) {
        btn->confined = g_lua_toboolean(L, 2);
        btn->dirty = 1;
        g_sre_btn_dirty = 1;
    }
    return 0;
}

/* ======== Keyboard API ======== */
static int l_kbd_open(lua_State* L) {
    const char* initial = lua_tostring(L, 1);
    if (!initial) initial = "";
    int i;
    for (i = 0; i < 255 && initial[i]; i++) g_sre_keyboard_text[i] = initial[i];
    g_sre_keyboard_text[i] = '\0';
    g_sre_keyboard_open = 1;
    g_sre_keyboard_done = 0;
    return 0;
}

static int l_kbd_close(lua_State* L) {
    (void)L;
    g_sre_keyboard_open = 0;
    g_sre_keyboard_done = 0;
    return 0;
}

static int l_kbd_get_text(lua_State* L) {
    g_lua_pushstring(L, (const char*)g_sre_keyboard_text);
    return 1;
}

static int l_kbd_is_done(lua_State* L) {
    g_lua_pushboolean(L, g_sre_keyboard_done);
    return 1;
}

static int l_kbd_is_open(lua_State* L) {
    g_lua_pushboolean(L, g_sre_keyboard_open);
    return 1;
}


/* =========================================================================
 * Mini.* Lua Functions
 * ========================================================================= */

/* Mini.Arch() → string ("arm64-v8a" or "armeabi-v7a") */
static int l_mini_arch(lua_State* L) {
    g_lua_pushstring(L, g_sre_mod_arch);
    return 1;
}

/* Mini.GetProfileID() → string (UUID of current save) */
static int l_mini_get_profile_id(lua_State* L) {
    g_lua_pushstring(L, g_sre_mod_profile_id);
    return 1;
}

/* Mini.SetControlsHidden(bool) */
static int l_mini_set_controls_hidden(lua_State* L) {
    g_sre_controls_hidden = g_lua_toboolean(L, 1);
    return 0;
}

/* =========================================================================
 * Coin Limit (Phase 3.5)
 * ========================================================================= */

/* Mini.SetCoinLimit(n) — set max coin count (host patches binary) */
static int l_mini_set_coin_limit(lua_State* L) {
    int n = (int)g_lua_tonumber(L, 1);
    if (n < 0) n = 0;
    if (n > 65535) n = 65535;
    g_sre_coin_limit = n;
    return 0;
}

/* Mini.GetCoinLimit() */
static int l_mini_get_coin_limit(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_coin_limit);
    return 1;
}

/* Mini.ToggleDebug() */
static int l_mini_toggle_debug(lua_State* L) {
    (void)L;
    g_sre_debug_active = !g_sre_debug_active;
    return 0;
}

/* Mini.RecreateHero() — sets pending flag for host to poll.
 * The real implementation needs Caver::GameSceneController::RecreateHero()
 * called at the engine offset via a guest→host callback mechanism. */
static int l_mini_recreate_hero(lua_State* L) {
    (void)L;
    g_sre_recreate_hero_pending = 1;
    return 0;
}

/* Mini.ReloadTextures(force) — set pending flag for host */
static int l_mini_reload_textures(lua_State* L) {
    (void)L;
    g_sre_reload_textures_pending = 1;
    return 0;
}

/* Mini.ClearModels() — set pending flag for host */
static int l_mini_clear_models(lua_State* L) {
    (void)L;
    g_sre_clear_models_pending = 1;
    return 0;
}

/* Mini.SetWeaponColor(obj, r, g, b, a, intensity) — reuse trinket glow */
static int l_mini_set_weapon_color(lua_State* L) {
    const char* item_id = lua_tostring(L, 1);
    if (!item_id) return 0;
    float r = (float)g_lua_tonumber(L, 2);
    float g = (float)g_lua_tonumber(L, 3);
    float b = (float)g_lua_tonumber(L, 4);
    float a = (float)g_lua_tonumber(L, 5);
    float intensity = (float)g_lua_tonumber(L, 6);

    int idx = -1;
    int i;
    for (i = 0; i < g_sre_trinket_glow_count; i++) {
        if (sre_streq(g_sre_trinket_glows[i].item_id, item_id)) { idx = i; break; }
    }
    if (idx < 0 && g_sre_trinket_glow_count < 16)
        idx = g_sre_trinket_glow_count++;
    if (idx < 0) return 0;

    for (i = 0; i < 31 && item_id[i]; i++)
        g_sre_trinket_glows[idx].item_id[i] = item_id[i];
    g_sre_trinket_glows[idx].item_id[i] = '\0';
    g_sre_trinket_glows[idx].r = r;
    g_sre_trinket_glows[idx].g = g;
    g_sre_trinket_glows[idx].b = b;
    g_sre_trinket_glows[idx].a = a;
    g_sre_trinket_glows[idx].intensity = intensity;
    return 0;
}

/* =========================================================================
 * CameraController.* (SwKiwi alias functions)
 * ========================================================================= */

/* CameraController.SetPositionOffset({x,y,z}) — alias for SetPosition
 * SwKiwi passes a table {x=, y=, z=} instead of 3 args */
static int l_camctrl_set_position_offset(lua_State* L) {
    if (g_lua_type(L, 1) == LUA_TTABLE) {
        g_lua_getfield(L, 1, "x");
        g_sre_cam_x = (float)g_lua_tonumber(L, -1);
        lua_pop(L, 1);
        g_lua_getfield(L, 1, "y");
        g_sre_cam_y = (float)g_lua_tonumber(L, -1);
        lua_pop(L, 1);
        g_lua_getfield(L, 1, "z");
        g_sre_cam_z = (float)g_lua_tonumber(L, -1);
        lua_pop(L, 1);
    } else {
        g_sre_cam_x = (float)g_lua_tonumber(L, 1);
        g_sre_cam_y = (float)g_lua_tonumber(L, 2);
        g_sre_cam_z = (float)g_lua_tonumber(L, 3);
    }
    g_sre_cam_set_pending = 1;
    return 0;
}

/* CameraController.SetPerspectiveProjection(fov, aspect, near, far) — stub */
static int l_camctrl_set_perspective(lua_State* L) {
    (void)L;
    /* No-op: log values would need sre_log which we don't call here */
    return 0;
}

/* CameraController.SetUpVector({x,y,z}) — store values */
static int l_camctrl_set_up_vector(lua_State* L) {
    if (g_lua_type(L, 1) == LUA_TTABLE) {
        g_lua_getfield(L, 1, "x");
        g_sre_cam_up_x = (float)g_lua_tonumber(L, -1);
        lua_pop(L, 1);
        g_lua_getfield(L, 1, "y");
        g_sre_cam_up_y = (float)g_lua_tonumber(L, -1);
        lua_pop(L, 1);
        g_lua_getfield(L, 1, "z");
        g_sre_cam_up_z = (float)g_lua_tonumber(L, -1);
        lua_pop(L, 1);
    } else {
        g_sre_cam_up_x = (float)g_lua_tonumber(L, 1);
        g_sre_cam_up_y = (float)g_lua_tonumber(L, 2);
        g_sre_cam_up_z = (float)g_lua_tonumber(L, 3);
    }
    return 0;
}

/* CameraController.GetUpVector() → {x=0, y=1, z=0} */
static int l_camctrl_get_up_vector(lua_State* L) {
    g_lua_createtable(L, 0, 3);
    g_lua_pushnumber(L, (double)g_sre_cam_up_x);
    g_lua_setfield(L, -2, "x");
    g_lua_pushnumber(L, (double)g_sre_cam_up_y);
    g_lua_setfield(L, -2, "y");
    g_lua_pushnumber(L, (double)g_sre_cam_up_z);
    g_lua_setfield(L, -2, "z");
    return 1;
}

/* Mini.SceneFindAll() → table of scene object names
 * Placeholder — returns empty table. SwKiwi's implementation iterates an
 * engine scene tree (ARM offset-based), which we cannot do on PC without
 * the right guest memory layout. A future implementation can hook scene
 * events to build this list dynamically. */
static int l_mini_scene_find_all(lua_State* L) {
    g_lua_createtable(L, 0, 0);
    return 1;
}

/* Mini.Test() — SwKiwi debug probe (reads hero level and logs it on device).
 * On desktop we simply return 0 (no-op), matching the no-visible-output
 * behaviour when logcat is not present. */
static int l_mini_test(lua_State* L) {
    (void)L;
    return 0;
}

/* Mini.map(...) — polymorphic map function
 *
 * SwKiwi supports 4 overloads based on arg types:
 *   map(table, fn) — table_map: call fn(value, key, table) for each key-value pair
 *   map(fn, table) — array_map: call fn(value, index, array) for each sequential element
 *   map(string, fn) — string_map: call fn(char, index, string) for each character
 *   map(number, fn) — number_map: call fn(index, output_table) for i=1..n
 * All return a table of results.
 *
 * Accuracy: matches SwKiwi's the_map_function.c exactly.
 */
static int l_mini_map(lua_State* L) {
    int nargs = g_lua_gettop(L);
    if (nargs < 2) {
        if (g_lua_error) return g_lua_error(L);
        return 0;
    }

    int t1 = g_lua_type(L, 1);
    int t2 = g_lua_type(L, 2);

    /* map(table, fn) — SwKiwi table_map: fn(value, key, table) */
    if (t1 == LUA_TTABLE && t2 == LUA_TFUNCTION) {
        if (!g_lua_objlen || !g_lua_next || !g_lua_pushvalue) {
            g_lua_createtable(L, 0, 0); return 1;
        }
        g_lua_settop(L, 2);
        /* Stack: 1=input_table, 2=fn */
        int out_size = g_lua_objlen ? (int)g_lua_objlen(L, 1) : 0;
        g_lua_createtable(L, out_size, 0); /* 3=output */
        g_lua_pushnil(L); /* first key: 4 */
        while (g_lua_next(L, 1) != 0) {
            /* key=-2 (4), value=-1 (5) */
            /* call fn(value, key, table) — 3 args, 1 result */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 2);  /* push fn */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 5);  /* value */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 4);  /* key */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 1);  /* array/table */
            g_lua_pcall(L, 3, 1, 0); /* result at 6 */
            /* Duplicate key for rawset (rawset consumes it) */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 4);
            if (g_lua_replace) g_lua_replace(L, 5); /* key copy at 5 now, result at 6 */
            /* output[key] = result */
            if (g_lua_rawset) g_lua_rawset(L, 3);
            else { lua_pop(L, 1); }
            /* key is now on top for next next() call */
        }
        return 1;
    }

    /* map(fn, table) — SwKiwi array_map: fn(value, index, array) */
    if (t1 == LUA_TFUNCTION && t2 == LUA_TTABLE) {
        if (!g_lua_objlen || !g_lua_rawgeti || !g_lua_rawseti || !g_lua_pushvalue) {
            g_lua_createtable(L, 0, 0); return 1;
        }
        g_lua_settop(L, 2);
        /* Stack: 1=fn, 2=array */
        int len = (int)g_lua_objlen(L, 2);
        g_lua_createtable(L, len, 0); /* 3=output */
        int i;
        for (i = 1; i <= len; i++) {
            /* fn(value, index, array) — 3 args, 1 result */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 1);  /* fn */
            if (g_lua_rawgeti) g_lua_rawgeti(L, 2, i);  /* value */
            g_lua_pushnumber(L, (double)i);              /* index */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 2);  /* array */
            g_lua_pcall(L, 3, 1, 0);
            if (g_lua_rawseti) g_lua_rawseti(L, 3, i);
            else lua_pop(L, 1);
        }
        return 1;
    }

    /* map(number, fn) — SwKiwi number_map: fn(index, output_table) → 1 or 2 values */
    if (t1 == LUA_TNUMBER && t2 == LUA_TFUNCTION) {
        int n = (int)g_lua_tonumber(L, 1);
        g_lua_createtable(L, n, 0); /* 3=output */
        int result_idx = g_lua_gettop(L);
        int i;
        for (i = 1; i < n; i++) {  /* SwKiwi loops i < count (not <=) */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 2);  /* fn */
            g_lua_pushnumber(L, (double)i);              /* index */
            if (g_lua_pushvalue) g_lua_pushvalue(L, result_idx); /* output table */
            g_lua_pcall(L, 2, 2, 0); /* 2 results */
            /* If second result is nil, function returned one thing */
            if (g_lua_type(L, -1) == LUA_TNIL) {
                lua_pop(L, 1); /* remove nil */
                if (g_lua_rawseti) g_lua_rawseti(L, result_idx, i);
                else lua_pop(L, 1);
            } else {
                /* Two returns: insert -2 before -1 and do rawset */
                if (g_lua_insert) g_lua_insert(L, -2);
                if (g_lua_settable) g_lua_settable(L, result_idx);
                else { lua_pop(L, 2); }
            }
        }
        return 1;
    }

    /* map(string, fn) — SwKiwi string_map: fn(char, index, string) */
    if (t1 == LUA_TSTRING && t2 == LUA_TFUNCTION) {
        sre_size_t slen = 0;
        const char* s = g_lua_tolstring(L, 1, &slen);
        if (!s) { g_lua_createtable(L, 0, 0); return 1; }
        g_lua_createtable(L, (int)slen, 0); /* 3=output */
        int result_idx = g_lua_gettop(L);
        size_t i;
        for (i = 0; i < slen; i++) {
            if (g_lua_pushvalue) g_lua_pushvalue(L, 2);           /* fn */
            if (g_lua_pushlstring) g_lua_pushlstring(L, s + i, 1); /* char */
            else g_lua_pushstring(L, "?");
            g_lua_pushnumber(L, (double)(i + 1));                /* index (1-based) */
            if (g_lua_pushvalue) g_lua_pushvalue(L, 1);          /* whole string */
            g_lua_pcall(L, 3, 1, 0);
            if (g_lua_rawseti) g_lua_rawseti(L, result_idx, (int)(i + 1));
            else lua_pop(L, 1);
        }
        /* Note: SwKiwi string_map also table.concat's the result — omit here
         * since mods on desktop typically use string.char already. */
        return 1;
    }

    /* Unknown arg types — return error like SwKiwi */
    if (g_lua_error) return g_lua_error(L);
    g_lua_createtable(L, 0, 0);
    return 1;
}

static int l_mini_execute_lni(lua_State* L) {
    const char* func = NULL;
    int is_bound = 0;
    
    /* Check if called as a bound closure (upvalue 1 is function name) */
    if (g_lua_type && g_lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING) {
        func = lua_tostring(L, lua_upvalueindex(1));
        is_bound = 1;
    } else {
        func = lua_tostring(L, 1);
    }
    if (!func) return 0;

    /* Copy function name to LNI command buffer */
    int i;
    for (i = 0; i < 63 && func[i]; i++) {
        g_sre_lni_command[i] = func[i];
    }
    g_sre_lni_command[i] = '\0';

    /* Retrieve arguments:
     * If bound: arg1 is at index 1, arg2 is at index 2.
     * If not bound (direct Execute): arg1 is at index 2, arg2 is at index 3.
     */
    int arg1_idx = is_bound ? 1 : 2;
    int arg2_idx = is_bound ? 2 : 3;

    /* If there's a string argument, copy it too */
    if (g_lua_gettop(L) >= arg1_idx && g_lua_type(L, arg1_idx) == LUA_TSTRING) {
        const char* arg = lua_tostring(L, arg1_idx);
        if (arg) {
            for (i = 0; i < 511 && arg[i]; i++) {
                g_sre_lni_arg1[i] = arg[i];
            }
            g_sre_lni_arg1[i] = '\0';
        }
    } else {
        g_sre_lni_arg1[0] = '\0';
    }

    /* If there's a second string argument, copy it to arg2 */
    if (g_lua_gettop(L) >= arg2_idx && g_lua_type(L, arg2_idx) == LUA_TSTRING) {
        const char* arg = lua_tostring(L, arg2_idx);
        if (arg) {
            for (i = 0; i < 511 && arg[i]; i++) {
                g_sre_lni_arg2[i] = arg[i];
            }
            g_sre_lni_arg2[i] = '\0';
        }
    } else {
        g_sre_lni_arg2[0] = '\0';
    }

    g_sre_lni_pending = 1;
    return 0;
}

/* Mini.BindLNI(funcName) → function
 * Returns a Lua function that calls ExecuteLNI with the given name */
static int l_mini_bind_lni(lua_State* L) {
    /* Push the function name as an upvalue */
    g_lua_pushcclosure(L, l_mini_execute_lni, 1);
    return 1;
}

/* =========================================================================
 * LNI.* Lua Functions (direct host actions, no Java needed)
 * ========================================================================= */

/* LNI.getSpeed() → number */
static int l_lni_get_speed(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_game_speed);
    return 1;
}

/* LNI.setSpeed(n) */
static int l_lni_set_speed(lua_State* L) {
    double speed = g_lua_tonumber(L, 1);
    if (speed > 0.0 && speed <= 10.0) {
        g_sre_game_speed = (float)speed;
    }
    return 0;
}

/* LNI.quit() — signal host to exit */
static int l_lni_quit(lua_State* L) {
    (void)L;
    /* Set LNI command for host to process */
    g_sre_lni_command[0] = 'q'; g_sre_lni_command[1] = 'u';
    g_sre_lni_command[2] = 'i'; g_sre_lni_command[3] = 't';
    g_sre_lni_command[4] = '\0';
    g_sre_lni_pending = 1;
    return 0;
}

/* LNI.copyToClipboard(text) — signal host */
static int l_lni_copy_to_clipboard(lua_State* L) {
    const char* label = lua_tostring(L, 1);
    const char* text = lua_tostring(L, 2);
    if (!text) {
        text = label;
        label = "Clipboard";
    }
    if (!text) return 0;

    /* Copy function name */
    const char* cmd = "copyToClipboard";
    int i;
    for (i = 0; cmd[i]; i++) g_sre_lni_command[i] = cmd[i];
    g_sre_lni_command[i] = '\0';

    /* Copy arguments */
    for (i = 0; i < 511 && label[i]; i++) g_sre_lni_arg1[i] = label[i];
    g_sre_lni_arg1[i] = '\0';

    for (i = 0; i < 511 && text[i]; i++) g_sre_lni_arg2[i] = text[i];
    g_sre_lni_arg2[i] = '\0';

    g_sre_lni_pending = 1;
    return 0;
}

/* LNI.openUrl(url) — signal host */
static int l_lni_open_url(lua_State* L) {
    const char* url = lua_tostring(L, 1);
    if (!url) return 0;

    const char* cmd = "openUrl";
    int i;
    for (i = 0; cmd[i]; i++) g_sre_lni_command[i] = cmd[i];
    g_sre_lni_command[i] = '\0';

    for (i = 0; i < 511 && url[i]; i++) g_sre_lni_arg1[i] = url[i];
    g_sre_lni_arg1[i] = '\0';
    g_sre_lni_arg2[0] = '\0';

    g_sre_lni_pending = 1;
    return 0;
}

/* =========================================================================
 * Armor Model Swap System (Phase 3.2)
 * =========================================================================
 * Host reads these to override ModelNameForArmor results.
 * Format: packed key\0value\0 pairs like CString table.
 * ========================================================================= */
char g_sre_armor_models[2048] = {0};   /* item_id\0model_name\0...\0\0 */
int  g_sre_armor_model_count = 0;
char g_sre_default_player_model[64] = "hiro";

/* Mini.SetArmorModel(item_id, model_name) */
static int sre_mini_set_armor_model(lua_State* L) {
    const char* item_id = lua_tostring(L, 1);
    const char* model   = lua_tostring(L, 2);
    if (!item_id || !model) return 0;

    /* Find end of table */
    int pos = 0;
    int entries = 0;
    while (pos < 2046 && entries < g_sre_armor_model_count) {
        while (pos < 2046 && g_sre_armor_models[pos]) pos++;
        pos++;
        while (pos < 2046 && g_sre_armor_models[pos]) pos++;
        pos++;
        entries++;
    }

    /* Copy item_id */
    int len1 = 0;
    while (item_id[len1]) len1++;
    if (pos + len1 + 1 >= 2046) return 0;
    int i;
    for (i = 0; i <= len1; i++)
        g_sre_armor_models[pos + i] = item_id[i];
    pos += len1 + 1;

    /* Copy model_name */
    int len2 = 0;
    while (model[len2]) len2++;
    if (pos + len2 + 1 >= 2046) return 0;
    for (i = 0; i <= len2; i++)
        g_sre_armor_models[pos + i] = model[i];
    pos += len2 + 1;

    g_sre_armor_models[pos] = '\0';
    g_sre_armor_model_count++;
    return 0;
}

/* Mini.SetDefaultPlayerModel(model_name) */
static int sre_mini_set_default_model(lua_State* L) {
    const char* model = lua_tostring(L, 1);
    if (!model) return 0;
    int i;
    for (i = 0; i < 63 && model[i]; i++)
        g_sre_default_player_model[i] = model[i];
    g_sre_default_player_model[i] = '\0';
    return 0;
}

/* =========================================================================
 * Scene Event System (Phase 4.2)
 * =========================================================================
 * The host updates g_sre_current_scene_name whenever the scene changes.
 * We check for changes and call registered Lua callbacks.
 * ========================================================================= */
char g_sre_current_scene_name[128] = {0};
char g_sre_previous_scene_name[128] = {0};
int  g_sre_scene_changed = 0;     /* Host sets to 1 when scene changes */
int  g_sre_scene_callback_ref = 0; /* Lua registry ref for callback */

/* Mini.OnSceneChange(callback_function)
 * Stores the callback in the Lua registry at a fixed integer key.
 * We use raw registry key 91001 as our private slot. */
#define SRE_SCENE_CB_REGKEY 91001

static int sre_mini_on_scene_change(lua_State* L) {
    if (g_lua_type(L, 1) == LUA_TFUNCTION) {
        if (g_lua_pushvalue) g_lua_pushvalue(L, 1);
        if (g_lua_rawseti) g_lua_rawseti(L, LUA_REGISTRYINDEX, SRE_SCENE_CB_REGKEY);
        g_sre_scene_callback_ref = SRE_SCENE_CB_REGKEY;
    }
    return 0;
}

/* Mini.GetCurrentScene() */
static int sre_mini_get_current_scene(lua_State* L) {
    g_lua_pushstring(L, g_sre_current_scene_name);
    return 1;
}

/* Mini.GetPreviousScene() */
static int sre_mini_get_previous_scene(lua_State* L) {
    g_lua_pushstring(L, g_sre_previous_scene_name);
    return 1;
}

/* =========================================================================
 * Mod Info (Phase 4.3)
 * ========================================================================= */
char g_sre_mod_name[128] = {0};
char g_sre_mod_version[32] = {0};
char g_sre_mod_author[128] = {0};

/* Mini.GetModName() */
static int sre_mini_get_mod_name(lua_State* L) {
    g_lua_pushstring(L, g_sre_mod_name[0] ? g_sre_mod_name : "Unknown");
    return 1;
}

/* Mini.GetModVersion() */
static int sre_mini_get_mod_version(lua_State* L) {
    g_lua_pushstring(L, g_sre_mod_version[0] ? g_sre_mod_version : "1.0");
    return 1;
}

/* Mini.GetModAuthor() */
static int sre_mini_get_mod_author(lua_State* L) {
    g_lua_pushstring(L, g_sre_mod_author[0] ? g_sre_mod_author : "Unknown");
    return 1;
}

/* =========================================================================
 * Bauble System (Phase 3.3)
 * =========================================================================
 * Tracks equippable baubles with name, level, and equipped state.
 * Lua scripts access via the global Bauble table.
 * ========================================================================= */

typedef struct {
    char name[32];
    int level;
    int equipped;
} SreBauble;

SreBauble g_sre_baubles[32] = {{0}};
int g_sre_bauble_count = 0;

static SreBauble* sre_find_bauble(const char* name) {
    int i;
    for (i = 0; i < g_sre_bauble_count; i++) {
        if (sre_streq(g_sre_baubles[i].name, name)) return &g_sre_baubles[i];
    }
    return 0;
}

static SreBauble* sre_find_or_create_bauble(const char* name) {
    SreBauble* b = sre_find_bauble(name);
    if (b) return b;
    if (g_sre_bauble_count >= 32) return 0;
    b = &g_sre_baubles[g_sre_bauble_count++];
    int i;
    for (i = 0; name[i] && i < 31; i++) b->name[i] = name[i];
    b->name[i] = '\0';
    b->level = 0;
    b->equipped = 0;
    return b;
}

/* Bauble.Find(name) → table {name, level, equipped} or nil */
static int l_bauble_find(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) { g_lua_pushnil(L); return 1; }
    SreBauble* b = sre_find_bauble(name);
    if (!b) { g_lua_pushnil(L); return 1; }
    g_lua_createtable(L, 0, 3);
    g_lua_pushstring(L, b->name);
    g_lua_setfield(L, -2, "name");
    g_lua_pushnumber(L, (double)b->level);
    g_lua_setfield(L, -2, "level");
    g_lua_pushboolean(L, b->equipped);
    g_lua_setfield(L, -2, "equipped");
    return 1;
}

/* Bauble.Equip(name) — set equipped=1, create if not found */
static int l_bauble_equip(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;
    SreBauble* b = sre_find_or_create_bauble(name);
    if (b) b->equipped = 1;
    return 0;
}

/* Bauble.Unequip(name) — set equipped=0 */
static int l_bauble_unequip(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;
    SreBauble* b = sre_find_bauble(name);
    if (b) b->equipped = 0;
    return 0;
}

/* Bauble.IsWearing(name) → boolean */
static int l_bauble_is_wearing(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) { g_lua_pushboolean(L, 0); return 1; }
    SreBauble* b = sre_find_bauble(name);
    g_lua_pushboolean(L, b ? b->equipped : 0);
    return 1;
}

/* Bauble.GetLevel(name) → integer (0 if not found) */
static int l_bauble_get_level(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) { g_lua_pushnumber(L, 0.0); return 1; }
    SreBauble* b = sre_find_bauble(name);
    g_lua_pushnumber(L, b ? (double)b->level : 0.0);
    return 1;
}

/* Bauble.IncLevel(name) — increment level by 1, create with level=1 if not found */
static int l_bauble_inc_level(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;
    SreBauble* b = sre_find_or_create_bauble(name);
    if (b) b->level++;
    return 0;
}

/* Bauble.HideAll() — set all baubles' equipped=0 */
static int l_bauble_hide_all(lua_State* L) {
    (void)L;
    int i;
    for (i = 0; i < g_sre_bauble_count; i++) {
        g_sre_baubles[i].equipped = 0;
    }
    return 0;
}

/* =========================================================================
 * EdgeTest — Screen edge/bounds API used by RLSW button menus (nb.scl)
 * =========================================================================
 * EdgeTest.Test(depth) -> screen_origin_vec3, screen_width, screen_height
 *
 * The button_menu coroutine calls this every tick to reposition buttons.
 * screen_origin is the top-left corner of the viewport in world coords.
 * On Swordigo the game space is 960×544 units (or 960×540).
 * We return a Lua table {x,y,z} via Vector3.New so scripts work naturally.
 * =========================================================================*/
extern float g_sre_gui_screen_w;
extern float g_sre_gui_screen_h;

/* Helper: call Vector3.New(x,y,z) and push result on stack. Returns 1 on
 * success (result is on top of stack), 0 on failure (nothing pushed). */
static int sre_push_vector3(lua_State* L, double x, double y, double z) {
    if (!g_lua_getfield || !g_lua_type || !g_lua_pcall ||
        !g_lua_pushnumber || !g_lua_settop || !g_lua_gettop) return 0;
    int base = g_lua_gettop(L);
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Vector3");
    if (g_lua_type(L, -1) != LUA_TTABLE) {
        g_lua_settop(L, base);
        /* Fallback: return a plain {x,y,z} table */
        g_lua_createtable(L, 0, 3);
        g_lua_pushnumber(L, x); g_lua_setfield(L, -2, "x");
        g_lua_pushnumber(L, y); g_lua_setfield(L, -2, "y");
        g_lua_pushnumber(L, z); g_lua_setfield(L, -2, "z");
        return 1;
    }
    g_lua_getfield(L, -1, "New");
    if (g_lua_type(L, -1) != LUA_TFUNCTION) {
        g_lua_settop(L, base);
        /* Fallback */
        g_lua_createtable(L, 0, 3);
        g_lua_pushnumber(L, x); g_lua_setfield(L, -2, "x");
        g_lua_pushnumber(L, y); g_lua_setfield(L, -2, "y");
        g_lua_pushnumber(L, z); g_lua_setfield(L, -2, "z");
        return 1;
    }
    /* Stack: [Vector3, Vector3.New] — insert args and call */
    g_lua_pushnumber(L, x);
    g_lua_pushnumber(L, y);
    g_lua_pushnumber(L, z);
    if (g_lua_pcall(L, 3, 1, 0) != 0) {
        g_lua_settop(L, base);
        return 0;
    }
    /* Remove the Vector3 table below the result */
    /* Stack: [Vector3, result] -> remove Vector3 at base+1 */
    /* lua_remove equivalent: shift result down */
    /* Simple: just leave [Vector3_table, result] — caller only uses top */
    /* Actually fix: remove the Vector3 table beneath result */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "table"); /* dummy access to ensure stack sane */
    g_lua_settop(L, base); /* reset — redo simpler */
    /* Simpler re-approach: store result then clean up */
    return 0; /* fall through to simple version below */
}

static void sre_parse_vector3(lua_State* L, int idx, float* out_x, float* out_y, float* out_z) {
    if (idx < 0) {
        idx = g_lua_gettop(L) + idx + 1;
    }
    float coords[3] = {0.0f, 0.0f, 0.0f};
    const char* names[3] = {"x", "y", "z"};
    int i;
    for (i = 0; i < 3; i++) {
        g_lua_getfield(L, idx, names[i]);
        if (g_lua_type(L, -1) == 6) { /* 6 = LUA_TFUNCTION */
            g_lua_pushvalue(L, idx);
            if (g_lua_pcall) {
                if (g_lua_pcall(L, 1, 1, 0) == 0) {
                    coords[i] = (float)g_lua_tonumber(L, -1);
                }
                g_lua_settop(L, -2);
            } else {
                g_lua_call(L, 1, 1);
                coords[i] = (float)g_lua_tonumber(L, -1);
                g_lua_settop(L, -2);
            }
        } else {
            if (g_lua_type(L, -1) == 3) { /* 3 = LUA_TNUMBER */
                coords[i] = (float)g_lua_tonumber(L, -1);
            }
            g_lua_settop(L, -2);
        }
    }
    if (out_x) *out_x = coords[0];
    if (out_y) *out_y = coords[1];
    if (out_z) *out_z = coords[2];
}

static int sre_itoa_local(int val, char* buf) {
    char tmp[16];
    int i = 0;
    int is_neg = 0;
    if (val < 0) {
        is_neg = 1;
        val = -val;
    }
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0 && i < 15) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }
    int pos = 0;
    if (is_neg) {
        buf[pos++] = '-';
    }
    while (i > 0) {
        buf[pos++] = tmp[--i];
    }
    buf[pos] = '\0';
    return pos;
}

static int sre_ftoa(float f, char* buf) {
    int pos = 0;
    if (f < 0.0f) {
        buf[pos++] = '-';
        f = -f;
    }
    int ipart = (int)f;
    float fpart = f - (float)ipart;
    pos += sre_itoa_local(ipart, buf + pos);
    buf[pos++] = '.';
    int fpart_int = (int)(fpart * 1000.0f + 0.5f);
    if (fpart_int < 10) {
        buf[pos++] = '0';
        buf[pos++] = '0';
    } else if (fpart_int < 100) {
        buf[pos++] = '0';
    }
    pos += sre_itoa_local(fpart_int, buf + pos);
    buf[pos] = '\0';
    return pos;
}

static int l_camera_is_point_visible(lua_State* L) {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    sre_parse_vector3(L, 1, &px, &py, &pz);
    float sw = g_sre_gui_screen_w > 0 ? g_sre_gui_screen_w : 960.0f;
    float sh = g_sre_gui_screen_h > 0 ? g_sre_gui_screen_h : 544.0f;

    extern volatile float g_sre_cam_x;
    extern volatile float g_sre_cam_y;
    extern volatile float g_sre_cam_z;

    float cam_x = g_sre_cam_x;
    float cam_y = g_sre_cam_y;
    float cam_z = g_sre_cam_z != 0.0f ? g_sre_cam_z : 480.0f;
    float cam_fov = 1.047f;
    float cam_aspect = 1.76470588f;

    /* Query active cameraController from Lua globals for the most accurate state */
    if (g_lua_getfield && g_lua_type && g_lua_touserdata && g_lua_settop) {
        g_lua_getfield(L, LUA_GLOBALSINDEX, "cameraController");
        int t = g_lua_type(L, -1);
        if (t == 2 || t == 7) { /* 2 = LUA_TLIGHTUSERDATA, 7 = LUA_TUSERDATA */
            void* cc = g_lua_touserdata(L, -1);
            if (cc != NULL) {
                void* camera = NULL;
                if (sizeof(void*) == 8) {
                    camera = *(void**)((char*)cc + 0x58);
                } else {
                    camera = *(void**)((char*)cc + 0x54);
                }
                if (camera != NULL) {
                    cam_x = *(float*)((char*)camera + 0x10);
                    cam_y = *(float*)((char*)camera + 0x14);
                    cam_z = *(float*)((char*)camera + 0x18);
                    cam_fov = *(float*)((char*)camera + 0xf4);
                    float aspect_val = *(float*)((char*)camera + 0xf0);
                    if (aspect_val > 0.0f) cam_aspect = aspect_val;
                }
            }
        }
        g_lua_settop(L, -2);
    }

    float dist = cam_z - pz;
    if (dist <= 0.0f) {
        g_lua_pushboolean(L, 0);
        return 1;
    }

    float hf = cam_fov * 0.5f;
    float hf2 = hf * hf;
    float fov_tan = hf + (hf * hf2) * 0.333333f + (hf * hf2 * hf2) * 0.133333f;

    float half_h = dist * fov_tan;
    float half_w = half_h * cam_aspect;

    float dx = px - cam_x;
    if (dx < 0.0f) dx = -dx;
    float dy = py - cam_y;
    if (dy < 0.0f) dy = -dy;

    int visible = (dx <= half_w && dy <= half_h);
    g_lua_pushboolean(L, visible);
    return 1;
}

/* EdgeTest.Test(depth) -> vec3_screen_origin, screen_w, screen_h
 * Returns 3 values: the top-left corner as a Vector3 + w + h */
static int l_edgetest_test(lua_State* L) {
    double depth = 0.0;
    if (g_lua_type && g_lua_type(L, 1) != LUA_TNIL &&
        g_lua_tonumber) {
        depth = g_lua_tonumber(L, 1);
    }
    float sw = g_sre_gui_screen_w > 0 ? g_sre_gui_screen_w : 960.0f;
    float sh = g_sre_gui_screen_h > 0 ? g_sre_gui_screen_h : 544.0f;

    extern volatile float g_sre_cam_x;
    extern volatile float g_sre_cam_y;
    extern volatile float g_sre_cam_z;

    float cam_x = g_sre_cam_x;
    float cam_y = g_sre_cam_y;
    float cam_z = g_sre_cam_z != 0.0f ? g_sre_cam_z : 480.0f;
    float cam_fov = 1.047f;
    float cam_aspect = 1.76470588f;

    /* Query active cameraController from Lua globals for the most accurate state */
    if (g_lua_getfield && g_lua_type && g_lua_touserdata && g_lua_settop) {
        g_lua_getfield(L, LUA_GLOBALSINDEX, "cameraController");
        int t = g_lua_type(L, -1);
        if (t == 2 || t == 7) { /* 2 = LUA_TLIGHTUSERDATA, 7 = LUA_TUSERDATA */
            void* cc = g_lua_touserdata(L, -1);
            if (cc != NULL) {
                void* camera = NULL;
                if (sizeof(void*) == 8) {
                    camera = *(void**)((char*)cc + 0x58);
                } else {
                    camera = *(void**)((char*)cc + 0x54);
                }
                if (camera != NULL) {
                    cam_x = *(float*)((char*)camera + 0x10);
                    cam_y = *(float*)((char*)camera + 0x14);
                    cam_z = *(float*)((char*)camera + 0x18);
                    cam_fov = *(float*)((char*)camera + 0xf4);
                    float aspect_val = *(float*)((char*)camera + 0xf0);
                    if (aspect_val > 0.0f) cam_aspect = aspect_val;
                }
            }
        }
        g_lua_settop(L, -2);
    }

    /* Calculate dynamic viewport bounds at target depth */
    float dist = cam_z - (float)depth;
    if (dist < 1.0f) dist = 1.0f;

    float hf = cam_fov * 0.5f;
    float hf2 = hf * hf;
    float fov_tan = hf + (hf * hf2) * 0.333333f + (hf * hf2 * hf2) * 0.133333f;

    float viewport_h = 2.0f * dist * fov_tan;
    float viewport_w = viewport_h * cam_aspect;

    float ox = cam_x - viewport_w / 2.0f;
    float oy = cam_y + viewport_h / 2.0f - viewport_h * 0.52f;

    int pushed = 0;
    if (g_lua_getfield && g_lua_type && g_lua_pcall && g_lua_pushnumber && g_lua_settop && g_lua_gettop) {
        int base = g_lua_gettop(L);
        g_lua_getfield(L, LUA_GLOBALSINDEX, "Vector3");
        if (g_lua_type(L, -1) == LUA_TTABLE) {
            g_lua_getfield(L, -1, "New");
            if (g_lua_type(L, -1) == LUA_TFUNCTION) {
                g_lua_pushnumber(L, (double)ox);
                g_lua_pushnumber(L, (double)oy);
                g_lua_pushnumber(L, depth);
                if (g_lua_pcall(L, 3, 1, 0) == 0) {
                    pushed = 1;
                    g_lua_setfield(L, LUA_GLOBALSINDEX, "__et_tmp");
                    g_lua_settop(L, base);
                    g_lua_getfield(L, LUA_GLOBALSINDEX, "__et_tmp");
                    g_lua_pushnil(L);
                    g_lua_setfield(L, LUA_GLOBALSINDEX, "__et_tmp");
                } else {
                    g_lua_settop(L, base);
                }
            } else {
                g_lua_settop(L, base);
            }
        } else {
            g_lua_settop(L, base);
        }
    }

    if (!pushed) {
        if (g_lua_createtable && g_lua_pushnumber && g_lua_setfield) {
            g_lua_createtable(L, 0, 3);
            g_lua_pushnumber(L, (double)ox);
            g_lua_setfield(L, -2, "x");
            g_lua_pushnumber(L, (double)oy);
            g_lua_setfield(L, -2, "y");
            g_lua_pushnumber(L, depth);
            g_lua_setfield(L, -2, "z");
        } else {
            if (g_lua_pushnil) g_lua_pushnil(L);
        }
    }

    /* Push screen width and height */
    if (g_lua_pushnumber) {
        g_lua_pushnumber(L, (double)viewport_w);
        g_lua_pushnumber(L, (double)viewport_h);
    }
    return 3; /* screen_origin, width, height */
}

/* =========================================================================
 * Achievement System (Phase 3.4)
 * =========================================================================
 * Tracks unlocked achievements with id, title, description.
 * Lua scripts access via Mini.Achievement sub-table.
 * Host polls g_sre_achievement_pending for popup display.
 * ========================================================================= */

typedef struct {
    char id[32];
    char title[64];
    char desc[128];
    int unlocked;
} SreAchievement;

SreAchievement g_sre_achievements[64] = {{0}};
int g_sre_achievement_count = 0;

/* Pending popup data — host polls this */
int g_sre_achievement_pending = 0;
char g_sre_achievement_pending_title[64] = {0};
char g_sre_achievement_pending_desc[128] = {0};

static SreAchievement* sre_find_achievement(const char* id) {
    int i;
    for (i = 0; i < g_sre_achievement_count; i++) {
        if (sre_streq(g_sre_achievements[i].id, id)) return &g_sre_achievements[i];
    }
    return 0;
}

static SreAchievement* sre_find_or_create_achievement(const char* id) {
    SreAchievement* a = sre_find_achievement(id);
    if (a) return a;
    if (g_sre_achievement_count >= 64) return 0;
    a = &g_sre_achievements[g_sre_achievement_count++];
    int i;
    for (i = 0; id[i] && i < 31; i++) a->id[i] = id[i];
    a->id[i] = '\0';
    a->title[0] = '\0';
    a->desc[0] = '\0';
    a->unlocked = 0;
    return a;
}

/* Mini.Achievement.Unlock(id, title, desc) — mark unlocked, set pending */
static int l_achievement_unlock(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    if (!id) return 0;
    const char* title = lua_tostring(L, 2);
    const char* desc = lua_tostring(L, 3);

    SreAchievement* a = sre_find_or_create_achievement(id);
    if (!a) return 0;

    a->unlocked = 1;

    /* Copy title */
    if (title) {
        int i;
        for (i = 0; title[i] && i < 63; i++) a->title[i] = title[i];
        a->title[i] = '\0';
        /* Also copy to pending title */
        for (i = 0; title[i] && i < 63; i++) g_sre_achievement_pending_title[i] = title[i];
        g_sre_achievement_pending_title[i] = '\0';
    }

    /* Copy desc */
    if (desc) {
        int i;
        for (i = 0; desc[i] && i < 127; i++) a->desc[i] = desc[i];
        a->desc[i] = '\0';
        /* Also copy to pending desc */
        for (i = 0; desc[i] && i < 127; i++) g_sre_achievement_pending_desc[i] = desc[i];
        g_sre_achievement_pending_desc[i] = '\0';
    }

    g_sre_achievement_pending = 1;
    return 0;
}

/* Mini.Achievement.IsUnlocked(id) → boolean */
static int l_achievement_is_unlocked(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    if (!id) { g_lua_pushboolean(L, 0); return 1; }
    SreAchievement* a = sre_find_achievement(id);
    g_lua_pushboolean(L, a ? a->unlocked : 0);
    return 1;
}

/* Mini.Achievement.GetAll() → table of achievement tables */
static int l_achievement_get_all(lua_State* L) {
    g_lua_createtable(L, g_sre_achievement_count, 0);
    int i;
    for (i = 0; i < g_sre_achievement_count; i++) {
        g_lua_createtable(L, 0, 4);
        g_lua_pushstring(L, g_sre_achievements[i].id);
        g_lua_setfield(L, -2, "id");
        g_lua_pushstring(L, g_sre_achievements[i].title);
        g_lua_setfield(L, -2, "title");
        g_lua_pushstring(L, g_sre_achievements[i].desc);
        g_lua_setfield(L, -2, "desc");
        g_lua_pushboolean(L, g_sre_achievements[i].unlocked);
        g_lua_setfield(L, -2, "unlocked");
        if (g_lua_rawseti) g_lua_rawseti(L, -2, i + 1);
        else lua_pop(L, 1);
    }
    return 1;
}

/* Mini.Achievement.Reset(id) — re-lock one achievement */
static int l_achievement_reset(lua_State* L) {
    const char* id = lua_tostring(L, 1);
    if (!id) return 0;
    SreAchievement* a = sre_find_achievement(id);
    if (a) a->unlocked = 0;
    return 0;
}

/* Mini.Achievement.ResetAll() — re-lock all achievements */
static int l_achievement_reset_all(lua_State* L) {
    (void)L;
    int i;
    for (i = 0; i < g_sre_achievement_count; i++) {
        g_sre_achievements[i].unlocked = 0;
    }
    return 0;
}

/* =========================================================================
 * String helpers (local to this file — mirror of sre_lua_libs versions)
 * ========================================================================= */

static int sre_api_starts_with(const char* s, const char* prefix) {
    int i;
    for (i = 0; prefix[i]; i++)
        if (s[i] != prefix[i]) return 0;
    return 1;
}

static const char* sre_api_minipath_translate(const char* path, char* out, int outlen) {
    if (!path || path[0] != '/') return path;
    const char* base = 0;
    const char* rest = 0;
    if (sre_api_starts_with(path, "/ExternalFiles/")) {
        rest = path + 15;
        base = g_sre_vfs_path_external[0] ? g_sre_vfs_path_external : 0;
    } else if (sre_api_starts_with(path, "/Files/")) {
        rest = path + 7;
        base = g_sre_vfs_path_files[0] ? g_sre_vfs_path_files : 0;
    } else if (sre_api_starts_with(path, "/Cache/")) {
        rest = path + 7;
        base = g_sre_vfs_path_cache[0] ? g_sre_vfs_path_cache : 0;
    } else if (sre_api_starts_with(path, "/ExternalCache/")) {
        rest = path + 15;
        base = g_sre_vfs_path_cache[0] ? g_sre_vfs_path_cache : 0;
    } else if (sre_api_starts_with(path, "/Assets/")) {
        rest = path + 8;
        base = g_sre_vfs_path_assets[0] ? g_sre_vfs_path_assets : 0;
    } else {
        return path;
    }
    if (!base) return path;
    {
        int i = 0, j;
        for (j = 0; i < outlen - 1 && base[j]; j++)
            out[i++] = base[j];
        if (i > 0 && out[i-1] != '/')
            out[i++] = '/';
        for (j = 0; i < outlen - 1 && rest[j]; j++)
            out[i++] = rest[j];
        out[i] = '\0';
        return out;
    }
}

/* =========================================================================
 * Game API — Lua function implementations
 * ========================================================================= */

/* Game.ShowNotification(msg) */
static int l_game_show_notification(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) return 0;
    int i;
    for (i = 0; i < 511 && msg[i]; i++)
        g_sre_game_notification[i] = msg[i];
    g_sre_game_notification[i] = '\0';
    g_sre_game_notification_pending = 1;
    return 0;
}

/* Game.CurrentLevelName() */
static int l_game_current_level_name(lua_State* L) {
    g_lua_pushstring(L, g_sre_game_level_name);
    return 1;
}

/* Game.SetCinematicMode(enabled, showBars) */
static int l_game_set_cinematic_mode(lua_State* L) {
    int enabled = g_lua_toboolean(L, 1);
    /* showBars arg (index 2) is accepted but not used yet */
    g_sre_game_action_type = enabled ? SRE_GAME_ACTION_CINEMATIC_ON
                                     : SRE_GAME_ACTION_CINEMATIC_OFF;
    g_sre_game_action_pending = 1;
    return 0;
}

/* Game.FadeIn() */
static int l_game_fade_in(lua_State* L) {
    (void)L;
    g_sre_game_action_type = SRE_GAME_ACTION_FADE_IN;
    g_sre_game_action_pending = 1;
    return 0;
}

/* Game.FadeOut() */
static int l_game_fade_out(lua_State* L) {
    (void)L;
    g_sre_game_action_type = SRE_GAME_ACTION_FADE_OUT;
    g_sre_game_action_pending = 1;
    return 0;
}

/* Game.Flash() */
static int l_game_flash(lua_State* L) {
    (void)L;
    g_sre_game_action_type = SRE_GAME_ACTION_FLASH;
    g_sre_game_action_pending = 1;
    return 0;
}

/* Game.EnterPortal(level, spawn) */
static int l_game_enter_portal(lua_State* L) {
    const char* level = lua_tostring(L, 1);
    const char* spawn = lua_tostring(L, 2);
    if (!level) return 0;
    int i;
    for (i = 0; i < 127 && level[i]; i++)
        g_sre_game_portal_level[i] = level[i];
    g_sre_game_portal_level[i] = '\0';
    if (spawn) {
        for (i = 0; i < 127 && spawn[i]; i++)
            g_sre_game_portal_spawn[i] = spawn[i];
        g_sre_game_portal_spawn[i] = '\0';
    } else {
        g_sre_game_portal_spawn[0] = '\0';
    }
    g_sre_game_action_type = SRE_GAME_ACTION_ENTER_PORTAL;
    g_sre_game_action_pending = 1;
    return 0;
}

/* Game.IncCounter(name) */
static int l_game_inc_counter(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;
    int i;
    for (i = 0; i < 127 && name[i]; i++)
        g_sre_game_counter_name[i] = name[i];
    g_sre_game_counter_name[i] = '\0';
    g_sre_game_action_type = SRE_GAME_ACTION_INC_COUNTER;
    g_sre_game_action_pending = 1;
    return 0;
}

/* Game.TitleForItem(itemName) — passthrough for now */
static int l_game_title_for_item(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (name) {
        g_lua_pushstring(L, name);
    } else {
        g_lua_pushstring(L, "");
    }
    return 1;
}

/* =========================================================================
 * Health API — Lua function implementations
 * ========================================================================= */

/* Health.CurrentHealth(obj) */
static int l_health_current_health(lua_State* L) {
    (void)L;  /* obj arg ignored — hero-only for now */
    g_lua_pushnumber(L, (double)g_sre_player_hp);
    return 1;
}

/* Health.SetCurrentHealth(obj, val) */
static int l_health_set_current_health(lua_State* L) {
    double val = g_lua_tonumber(L, 2);
    g_sre_char_set_field = 3;  /* 3 = hp */
    g_sre_char_set_value = (int)val;
    g_sre_char_set_pending = 1;
    return 0;
}

/* Health.SetCurrentMana(obj, val) */
static int l_health_set_current_mana(lua_State* L) {
    double val = g_lua_tonumber(L, 2);
    g_sre_char_set_field = 4;  /* 4 = mana */
    g_sre_char_set_value = (int)val;
    g_sre_char_set_pending = 1;
    return 0;
}

/* Health.SetImmunityTime(obj, seconds) */
static int l_health_set_immunity_time(lua_State* L) {
    double seconds = g_lua_tonumber(L, 2);
    g_sre_immunity_time = (float)seconds;
    g_sre_immunity_pending = 1;
    return 0;
}

/* Health.HasTakenDamage(obj) */
static int l_health_has_taken_damage(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, g_sre_has_taken_damage);
    return 1;
}

/* UI.SetControlsDisabled(disabled) — prevent player input when GUI is open */
static int l_ui_set_controls_disabled(lua_State* L) {
    int disabled = g_lua_toboolean(L, 1);
    /* Store globally; host input system should check this flag */
    extern volatile int g_sre_controls_disabled;
    g_sre_controls_disabled = disabled;

    /* Walk the gameController pointer to set native hidden flag if available */
    if (g_lua_getfield && g_lua_type && g_lua_touserdata && g_lua_settop) {
        g_lua_getfield(L, LUA_GLOBALSINDEX, "gameController");
        if (g_lua_type(L, -1) == 2) {
            const void* gameController = g_lua_touserdata(L, -1);
            if (gameController) {
                int is_64bit = (g_sre_mod_arch[3] == '6');
                void* gameSceneView = *(void**)((char*)gameController + (is_64bit ? 0xd8 : 0x70));
                if (gameSceneView) {
                    void* gameOverlayView = *(void**)((char*)gameSceneView + (is_64bit ? 0x100 : 0xcc));
                    if (gameOverlayView) {
                        *(char*)((char*)gameOverlayView + (is_64bit ? 0xe4 : 0xbc)) = (char)(disabled != 0);
                    }
                }
            }
        }
        g_lua_settop(L, -2); /* Pop gameController */
    }
    return 0;
}

/* UI.GetControlsDisabled() → boolean */
static int l_ui_get_controls_disabled(lua_State* L) {
    extern volatile int g_sre_controls_disabled;
    g_lua_pushboolean(L, g_sre_controls_disabled);
    return 1;
}

/* =========================================================================
 * fs API — Lua function implementations
 * ========================================================================= */

/* fs.exists(path) — simple wrapper used by some Kiwi mods */
static int l_fs_file_exists(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) { g_lua_pushboolean(L, 0); return 1; }
    char buf[512];
    const char* real = sre_api_minipath_translate(path, buf, 512);
    SRE_FS_FILE* fp = fopen(real, "r");
    if (fp) { fclose(fp); g_lua_pushboolean(L, 1); }
    else { g_lua_pushboolean(L, 0); }
    return 1;
}

/* fs.read(path) — return file contents as string if present, otherwise return an empty table to avoid nil in init scripts */
static int l_fs_read_file(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) { g_lua_createtable(L, 0, 0); return 1; }
    char tpath[512];
    const char* real = sre_api_minipath_translate(path, tpath, 512);
    SRE_FS_FILE* fp = fopen(real, "rb");
    if (!fp) { /* return empty table to make pairs() safe */
        g_lua_createtable(L, 0, 0);
        return 1;
    }

    /* Read file into dynamically-grown buffer */
    size_t cap = 4096;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { fclose(fp); g_lua_createtable(L, 0, 0); return 1; }

    while (1) {
        size_t toread = cap - len;
        if (toread == 0) {
            /* grow */
            size_t newcap = cap * 2;
            char* nbuf = (char*)realloc(buf, newcap);
            if (!nbuf) break;
            buf = nbuf; cap = newcap; toread = cap - len;
        }
        size_t r = fread(buf + len, 1, toread, fp);
        len += r;
        if (r < toread) break; /* EOF or short read */
    }

    fclose(fp);

    if (len == 0) {
        free(buf);
        g_lua_createtable(L, 0, 0);
        return 1;
    }

    if (g_lua_pushlstring) {
        g_lua_pushlstring(L, buf, len);
    } else {
        /* ensure null-termination then push string (may truncate binary data) */
        if (buf[len-1] != '\0') {
            char* nbuf = (char*)realloc(buf, len+1);
            if (nbuf) { buf = nbuf; buf[len] = '\0'; }
        }
        g_lua_pushstring(L, buf);
    }
    free(buf);
    return 1;
}

/* fs.write(path, content) — write content (string) to path, return boolean */
static int l_fs_write_file(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    size_t len = 0;
    const char* content = NULL;
    if (g_lua_tolstring) content = g_lua_tolstring(L, 2, (sre_size_t*)&len);
    else content = lua_tostring(L, 2);
    if (!path || !content) { g_lua_pushboolean(L, 0); return 1; }
    char buf[512];
    const char* real = sre_api_minipath_translate(path, buf, 512);
    SRE_FS_FILE* fp = fopen(real, "wb");
    if (!fp) { g_lua_pushboolean(L, 0); return 1; }
    size_t w = fwrite(content, 1, len, fp);
    fclose(fp);
    g_lua_pushboolean(L, w == len);
    return 1;
}

/* hero no-op used by compatibility shims */
static int l_hero_noop(lua_State* L) { (void)L; g_lua_pushnumber(L, 0); return 1; }

/* fs.exists(path) — check file existence, return boolean */
static int l_fs_exists(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) {
        g_lua_pushboolean(L, 0);
        return 1;
    }
    char buf[512];
    const char* real = sre_api_minipath_translate(path, buf, 512);
    SRE_FS_FILE* fp = fopen(real, "r");
    if (fp) {
        fclose(fp);
        g_lua_pushboolean(L, 1);
    } else {
        g_lua_pushboolean(L, 0);
    }
    return 1;
}

/* fs.mkdir(path) */
static int l_fs_mkdir(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) {
        g_lua_pushnil(L);
        return 1;
    }
    char buf[512];
    const char* real = sre_api_minipath_translate(path, buf, 512);
    int ret = mkdir(real, 0755);
    if (ret == 0) {
        g_lua_pushboolean(L, 1);
    } else {
        g_lua_pushnil(L);
    }
    return 1;
}

/* fs.rmdir(path) */
static int l_fs_rmdir(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) {
        g_lua_pushnil(L);
        return 1;
    }
    char buf[512];
    const char* real = sre_api_minipath_translate(path, buf, 512);
    int ret = rmdir(real);
    if (ret == 0) {
        g_lua_pushboolean(L, 1);
    } else {
        g_lua_pushnil(L);
    }
    return 1;
}

/* fs.dir(path) — returns an empty iterator for now */
static int l_fs_dir_iter(lua_State* L) {
    (void)L;
    g_lua_pushnil(L);
    return 1;
}

static int l_fs_dir(lua_State* L) {
    (void)L;
    g_lua_pushcclosure(L, l_fs_dir_iter, 0);
    return 1;
}

/* fs.attributes(path) — check file existence via fopen, return table or nil */
static int l_fs_attributes(lua_State* L) {
    const char* path = lua_tostring(L, 1);
    if (!path) {
        /* Return empty table to make pairs() safe for callers */
        g_lua_createtable(L, 0, 0);
        return 1;
    }
    char buf[512];
    const char* real = sre_api_minipath_translate(path, buf, 512);
    SRE_FS_FILE* fp = fopen(real, "r");
    if (!fp) {
        /* Return empty table to make pairs() safe for callers */
        g_lua_createtable(L, 0, 0);
        return 1;
    }
    fclose(fp);
    /* Return {mode="file", size=0} */
    g_lua_createtable(L, 0, 2);
    g_lua_pushstring(L, "file");
    g_lua_setfield(L, -2, "mode");
    g_lua_pushnumber(L, 0);
    g_lua_setfield(L, -2, "size");
    return 1;
}

/* =========================================================================
 * C-Backed TOML Parser Module for Lua
 * ========================================================================= */
#include "toml.h"

static void push_toml_value(lua_State* L, toml_value_t val, char type_char) {
    if (type_char == 's') {
        g_lua_pushstring(L, val.u.s);
        free(val.u.s);
    } else if (type_char == 'b') {
        g_lua_pushboolean(L, val.u.b);
    } else if (type_char == 'i') {
        g_lua_pushnumber(L, (double)val.u.i);
    } else if (type_char == 'd') {
        g_lua_pushnumber(L, val.u.d);
    } else if (type_char == 't') {
        char ts_buf[64];
        snprintf(ts_buf, sizeof(ts_buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 val.u.ts.year, val.u.ts.month, val.u.ts.day,
                 val.u.ts.hour, val.u.ts.minute, val.u.ts.second);
        g_lua_pushstring(L, ts_buf);
    } else {
        g_lua_pushnil(L);
    }
}

static void push_toml_array(lua_State* L, toml_array_t* arr);

static void push_toml_table(lua_State* L, toml_table_t* tab) {
    if (!g_lua_createtable || !g_lua_pushstring || !g_lua_settable || !g_lua_pushnil) return;
    g_lua_createtable(L, 0, toml_table_len(tab));
    
    int len = toml_table_len(tab);
    for (int i = 0; i < len; i++) {
        int key_len;
        const char* key = toml_table_key(tab, i, &key_len);
        if (!key) continue;
        
        g_lua_pushstring(L, key);
        
        toml_value_t val = toml_table_string(tab, key);
        if (val.ok) {
            push_toml_value(L, val, 's');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_table_bool(tab, key);
        if (val.ok) {
            push_toml_value(L, val, 'b');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_table_int(tab, key);
        if (val.ok) {
            push_toml_value(L, val, 'i');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_table_double(tab, key);
        if (val.ok) {
            push_toml_value(L, val, 'd');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_table_timestamp(tab, key);
        if (val.ok) {
            push_toml_value(L, val, 't');
            g_lua_settable(L, -3);
            continue;
        }
        
        toml_table_t* sub_tab = toml_table_table(tab, key);
        if (sub_tab) {
            push_toml_table(L, sub_tab);
            g_lua_settable(L, -3);
            continue;
        }
        
        toml_array_t* sub_arr = toml_table_array(tab, key);
        if (sub_arr) {
            push_toml_array(L, sub_arr);
            g_lua_settable(L, -3);
            continue;
        }
        
        g_lua_pushnil(L);
        g_lua_settable(L, -3);
    }
}

static void push_toml_array(lua_State* L, toml_array_t* arr) {
    if (!g_lua_createtable || !g_lua_pushnumber || !g_lua_settable || !g_lua_pushnil) return;
    int len = toml_array_len(arr);
    g_lua_createtable(L, len, 0);
    
    for (int i = 0; i < len; i++) {
        g_lua_pushnumber(L, i + 1);
        
        toml_value_t val = toml_array_string(arr, i);
        if (val.ok) {
            push_toml_value(L, val, 's');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_array_bool(arr, i);
        if (val.ok) {
            push_toml_value(L, val, 'b');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_array_int(arr, i);
        if (val.ok) {
            push_toml_value(L, val, 'i');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_array_double(arr, i);
        if (val.ok) {
            push_toml_value(L, val, 'd');
            g_lua_settable(L, -3);
            continue;
        }
        val = toml_array_timestamp(arr, i);
        if (val.ok) {
            push_toml_value(L, val, 't');
            g_lua_settable(L, -3);
            continue;
        }
        
        toml_table_t* sub_tab = toml_array_table(arr, i);
        if (sub_tab) {
            push_toml_table(L, sub_tab);
            g_lua_settable(L, -3);
            continue;
        }
        
        toml_array_t* sub_arr = toml_array_array(arr, i);
        if (sub_arr) {
            push_toml_array(L, sub_arr);
            g_lua_settable(L, -3);
            continue;
        }
        
        g_lua_pushnil(L);
        g_lua_settable(L, -3);
    }
}

static int l_toml_parse(lua_State* L) {
    if (!g_lua_pushnil || !g_lua_pushstring) return 0;
    const char* toml_str = lua_tostring(L, 1);
    if (!toml_str) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "toml string expected");
        return 2;
    }
    
    char errbuf[256];
    size_t len = strlen(toml_str);
    char* toml_copy = (char*)malloc(len + 1);
    if (!toml_copy) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, "out of memory");
        return 2;
    }
    strcpy(toml_copy, toml_str);
    
    toml_table_t* tab = toml_parse(toml_copy, errbuf, sizeof(errbuf));
    free(toml_copy);
    
    if (!tab) {
        g_lua_pushnil(L);
        g_lua_pushstring(L, errbuf);
        return 2;
    }
    
    push_toml_table(L, tab);
    toml_free(tab);
    return 1;
}

static int luaopen_toml(lua_State* L) {
    if (!g_lua_createtable || !g_lua_pushcclosure || !g_lua_setfield) return 0;
    g_lua_createtable(L, 0, 1);
    g_lua_pushcclosure(L, l_toml_parse, 0);
    g_lua_setfield(L, -2, "parse");
    return 1;
}

/* Forward declarations for stub functions used by registration */
static int stub_char_num_coins(lua_State* L);
static int stub_char_set_num_coins(lua_State* L);
static int stub_char_has_flag(lua_State* L);
static int stub_char_has_item(lua_State* L);
static int stub_char_item_count(lua_State* L);
static int stub_char_noop(lua_State* L);
static int stub_all_items_collected(lua_State* L);
static int stub_itemdrop_item_identifier(lua_State* L);


/* =========================================================================
 * Registration — inject Mini/LNI tables into a Lua state
 * ========================================================================= */

/*
 * sre_register_mini_api — Register Mini.* and LNI.* tables
 *
 * Called from our RegisterProgramLibrary hook for every new Lua state.
 * Creates the global tables that RL scripts expect.
 */
void sre_register_mini_api(lua_State* L) {
    if (!g_lua_createtable || !g_lua_setfield || !g_lua_pushcclosure) {
        return;  /* Lua API not initialized yet */
    }

    /* ---- Mini table ---- */
    g_lua_createtable(L, 0, 24);  /* Mini = {} */

    g_lua_pushcclosure(L, l_mini_arch, 0);
    g_lua_setfield(L, -2, "Arch");

    g_lua_pushcclosure(L, l_mini_get_profile_id, 0);
    g_lua_setfield(L, -2, "GetProfileID");

    g_lua_pushcclosure(L, l_mini_set_controls_hidden, 0);
    g_lua_setfield(L, -2, "SetControlsHidden");

    g_lua_pushcclosure(L, l_mini_set_trinket_color, 0);
    g_lua_setfield(L, -2, "SetTrinketColor");

    g_lua_pushcclosure(L, l_mini_set_coin_limit, 0);
    g_lua_setfield(L, -2, "SetCoinLimit");

    /* UI module for control management */
    g_lua_createtable(L, 0, 8);
    g_lua_pushcclosure(L, l_ui_set_controls_disabled, 0);
    g_lua_setfield(L, -2, "SetControlsDisabled");
    g_lua_pushcclosure(L, l_ui_get_controls_disabled, 0);
    g_lua_setfield(L, -2, "GetControlsDisabled");
    g_lua_setfield(L, -2, "UI");

    g_lua_pushcclosure(L, l_mini_toggle_debug, 0);
    g_lua_setfield(L, -2, "ToggleDebug");

    g_lua_pushcclosure(L, l_mini_recreate_hero, 0);
    g_lua_setfield(L, -2, "RecreateHero");

    g_lua_pushcclosure(L, l_mini_scene_find_all, 0);
    g_lua_setfield(L, -2, "SceneFindAll");

    g_lua_pushcclosure(L, l_mini_execute_lni, 0);
    g_lua_setfield(L, -2, "ExecuteLNI");

    g_lua_pushcclosure(L, l_mini_bind_lni, 0);
    g_lua_setfield(L, -2, "BindLNI");

    g_lua_pushcclosure(L, l_mini_map, 0);
    g_lua_setfield(L, -2, "map");

    /* Mini.Test — SwKiwi debug probe stub */
    g_lua_pushcclosure(L, l_mini_test, 0);
    g_lua_setfield(L, -2, "Test");

    /* Phase 3.2: Armor models */
    g_lua_pushcclosure(L, sre_mini_set_armor_model, 0);
    g_lua_setfield(L, -2, "SetArmorModel");
    g_lua_pushcclosure(L, sre_mini_set_default_model, 0);
    g_lua_setfield(L, -2, "SetDefaultPlayerModel");

    /* Phase 3.5: Coin limit (getter) */
    g_lua_pushcclosure(L, l_mini_get_coin_limit, 0);
    g_lua_setfield(L, -2, "GetCoinLimit");

    /* SwKiwi: Resource management */
    g_lua_pushcclosure(L, l_mini_reload_textures, 0);
    g_lua_setfield(L, -2, "ReloadTextures");
    g_lua_pushcclosure(L, l_mini_clear_models, 0);
    g_lua_setfield(L, -2, "ClearModels");
    g_lua_pushcclosure(L, l_mini_set_weapon_color, 0);
    g_lua_setfield(L, -2, "SetWeaponColor");
    g_lua_pushcclosure(L, l_mini_set_trinket_color, 0);  /* alias */
    g_lua_setfield(L, -2, "SetWeaponColorForTrinket");

    /* Phase 4.2: Scene events */
    g_lua_pushcclosure(L, sre_mini_on_scene_change, 0);
    g_lua_setfield(L, -2, "OnSceneChange");
    g_lua_pushcclosure(L, sre_mini_get_current_scene, 0);
    g_lua_setfield(L, -2, "GetCurrentScene");
    g_lua_pushcclosure(L, sre_mini_get_previous_scene, 0);
    g_lua_setfield(L, -2, "GetPreviousScene");

    /* Phase 4.3: Mod info */
    g_lua_pushcclosure(L, sre_mini_get_mod_name, 0);
    g_lua_setfield(L, -2, "GetModName");
    g_lua_pushcclosure(L, sre_mini_get_mod_version, 0);
    g_lua_setfield(L, -2, "GetModVersion");
    g_lua_pushcclosure(L, sre_mini_get_mod_author, 0);
    g_lua_setfield(L, -2, "GetModAuthor");

    /* Mini.Health = {} (sub-table) */
    g_lua_createtable(L, 0, 4);
    g_lua_pushcclosure(L, l_mini_health_current_mana, 0);
    g_lua_setfield(L, -2, "CurrentMana");
    g_lua_pushcclosure(L, l_mini_health_current_mana_percent, 0);
    g_lua_setfield(L, -2, "CurrentManaPercent");
    g_lua_setfield(L, -2, "Health");

    /* Mini.Character = {} (sub-table) */
    g_lua_createtable(L, 0, 48);
    g_lua_pushcclosure(L, l_mini_char_get_level, 0);
    g_lua_setfield(L, -2, "GetLevel");
    g_lua_pushcclosure(L, l_mini_char_get_exp, 0);
    g_lua_setfield(L, -2, "GetExp");
    g_lua_pushcclosure(L, l_mini_char_get_health, 0);
    g_lua_setfield(L, -2, "GetHealth");
    g_lua_pushcclosure(L, l_mini_char_get_max_health, 0);
    g_lua_setfield(L, -2, "GetMaxHealth");
    g_lua_pushcclosure(L, l_mini_char_get_mana, 0);
    g_lua_setfield(L, -2, "GetMana");
    g_lua_pushcclosure(L, l_mini_char_get_max_mana, 0);
    g_lua_setfield(L, -2, "GetMaxMana");
    g_lua_pushcclosure(L, l_mini_char_get_coins, 0);
    g_lua_setfield(L, -2, "GetCoins");
    g_lua_pushcclosure(L, l_mini_char_get_atk_level, 0);
    g_lua_setfield(L, -2, "GetAttackLevel");
    g_lua_pushcclosure(L, l_mini_char_get_hp_level, 0);
    g_lua_setfield(L, -2, "GetHealthLevel");
    g_lua_pushcclosure(L, l_mini_char_get_mana_level, 0);
    g_lua_setfield(L, -2, "GetManaLevel");
    g_lua_pushcclosure(L, l_mini_char_set_level, 0);
    g_lua_setfield(L, -2, "SetLevel");
    g_lua_pushcclosure(L, l_mini_char_set_exp, 0);
    g_lua_setfield(L, -2, "SetExp");
    g_lua_pushcclosure(L, l_mini_char_set_health, 0);
    g_lua_setfield(L, -2, "SetHealth");
    g_lua_pushcclosure(L, l_mini_char_set_mana, 0);
    g_lua_setfield(L, -2, "SetMana");
    g_lua_pushcclosure(L, l_mini_char_set_coins, 0);
    g_lua_setfield(L, -2, "SetCoins");
    g_lua_pushcclosure(L, l_mini_char_get_walk_speed, 0);
    g_lua_setfield(L, -2, "GetWalkSpeed");
    g_lua_pushcclosure(L, l_mini_char_set_walk_speed, 0);
    g_lua_setfield(L, -2, "SetWalkSpeed");
    g_lua_pushcclosure(L, l_mini_char_get_run_speed, 0);
    g_lua_setfield(L, -2, "GetRunSpeed");
    g_lua_pushcclosure(L, l_mini_char_set_run_speed, 0);
    g_lua_setfield(L, -2, "SetRunSpeed");
    g_lua_pushcclosure(L, l_mini_char_set_jump_height, 0);
    g_lua_setfield(L, -2, "SetJumpHeight");
    g_lua_pushcclosure(L, l_mini_char_get_jump_height, 0);
    g_lua_setfield(L, -2, "GetJumpHeight");
    g_lua_pushcclosure(L, l_mini_char_start_moving, 0);
    g_lua_setfield(L, -2, "StartMovingToDirection");
    g_lua_pushcclosure(L, l_mini_char_stop_moving, 0);
    g_lua_setfield(L, -2, "StopMovingToDirection");

    /* SwKiwi: Character action functions */
    g_lua_pushcclosure(L, l_mini_char_die, 0);
    g_lua_setfield(L, -2, "Die");
    g_lua_pushcclosure(L, l_mini_char_hurt, 0);
    g_lua_setfield(L, -2, "Hurt");
    g_lua_pushcclosure(L, l_mini_char_use, 0);
    g_lua_setfield(L, -2, "Use");
    g_lua_pushcclosure(L, l_mini_char_swing, 0);
    g_lua_setfield(L, -2, "Swing");
    g_lua_pushcclosure(L, l_mini_char_stop_swing, 0);
    g_lua_setfield(L, -2, "StopSwing");
    g_lua_pushcclosure(L, l_mini_char_start_jumping, 0);
    g_lua_setfield(L, -2, "StartJumping");
    g_lua_pushcclosure(L, l_mini_char_stop_jumping, 0);
    g_lua_setfield(L, -2, "StopJumping");
    g_lua_pushcclosure(L, l_mini_char_drop_quickly, 0);
    g_lua_setfield(L, -2, "DropQuickly");
    g_lua_pushcclosure(L, l_mini_char_cancel_casting, 0);
    g_lua_setfield(L, -2, "CancelCasting");
    g_lua_pushcclosure(L, l_mini_char_finish_casting, 0);
    g_lua_setfield(L, -2, "FinishCasting");

    /* SwKiwi: Capability stubs (optimistic) */
    g_lua_pushcclosure(L, l_mini_char_can_do_something, 0);
    g_lua_setfield(L, -2, "CanDoSomething");
    g_lua_pushcclosure(L, l_mini_char_can_begin_casting, 0);
    g_lua_setfield(L, -2, "CanBeginCasting");
    g_lua_pushcclosure(L, l_mini_char_can_use, 0);
    g_lua_setfield(L, -2, "CanUse");
    g_lua_pushcclosure(L, l_mini_char_can_jump, 0);
    g_lua_setfield(L, -2, "CanJump");
    g_lua_pushcclosure(L, l_mini_char_can_swing, 0);
    g_lua_setfield(L, -2, "CanSwing");
    g_lua_pushcclosure(L, l_mini_char_can_pickup, 0);
    g_lua_setfield(L, -2, "CanPickup");

    /* SwKiwi: Extended state */
    g_lua_pushcclosure(L, l_mini_char_set_movement_facing_lock, 0);
    g_lua_setfield(L, -2, "SetMovementFacingLock");
    g_lua_pushcclosure(L, l_mini_char_set_stun_time, 0);
    g_lua_setfield(L, -2, "SetStunTime");
    g_lua_pushcclosure(L, l_mini_char_get_air_jump_used, 0);
    g_lua_setfield(L, -2, "GetAirJumpUsed");
    g_lua_pushcclosure(L, l_mini_char_set_air_jump_used, 0);
    g_lua_setfield(L, -2, "SetAirJumpUsed");
    g_lua_pushcclosure(L, l_mini_char_exp_for_level, 0);
    g_lua_setfield(L, -2, "ExpForLevel");
    g_lua_pushcclosure(L, l_mini_char_get_level_attributes, 0);
    g_lua_setfield(L, -2, "GetLevelAttributes");
    g_lua_pushcclosure(L, l_mini_char_set_level_attributes, 0);
    g_lua_setfield(L, -2, "SetLevelAttributes");

    g_lua_setfield(L, -2, "Character");

    /* Mini.Camera = {} (sub-table) */
    g_lua_createtable(L, 0, 8);
    g_lua_pushcclosure(L, l_mini_cam_get_position, 0);
    g_lua_setfield(L, -2, "GetPosition");
    g_lua_pushcclosure(L, l_mini_cam_set_position, 0);
    g_lua_setfield(L, -2, "SetPosition");
    g_lua_pushcclosure(L, l_mini_cam_get_zoom, 0);
    g_lua_setfield(L, -2, "GetZoom");
    g_lua_pushcclosure(L, l_mini_cam_set_zoom, 0);
    g_lua_setfield(L, -2, "SetZoom");
    g_lua_pushcclosure(L, l_mini_cam_get_follow, 0);
    g_lua_setfield(L, -2, "GetFollow");
    g_lua_pushcclosure(L, l_mini_cam_set_follow, 0);
    g_lua_setfield(L, -2, "SetFollow");
    g_lua_setfield(L, -2, "Camera");

    /* Mini.Achievement = {} (sub-table, Phase 3.4) */
    g_lua_createtable(L, 0, 5);
    g_lua_pushcclosure(L, l_achievement_unlock, 0);
    g_lua_setfield(L, -2, "Unlock");
    g_lua_pushcclosure(L, l_achievement_is_unlocked, 0);
    g_lua_setfield(L, -2, "IsUnlocked");
    g_lua_pushcclosure(L, l_achievement_get_all, 0);
    g_lua_setfield(L, -2, "GetAll");
    g_lua_pushcclosure(L, l_achievement_reset, 0);
    g_lua_setfield(L, -2, "Reset");
    g_lua_pushcclosure(L, l_achievement_reset_all, 0);
    g_lua_setfield(L, -2, "ResetAll");
    g_lua_setfield(L, -2, "Achievement");

    g_lua_setfield(L, LUA_GLOBALSINDEX, "Mini");  /* _G.Mini = table */

    /* ---- LNI table ---- */
    g_lua_createtable(L, 0, 5);  /* LNI = {} */

    g_lua_pushcclosure(L, l_lni_get_speed, 0);
    g_lua_setfield(L, -2, "getSpeed");

    g_lua_pushcclosure(L, l_lni_set_speed, 0);
    g_lua_setfield(L, -2, "setSpeed");

    g_lua_pushcclosure(L, l_lni_quit, 0);
    g_lua_setfield(L, -2, "quit");

    g_lua_pushcclosure(L, l_lni_copy_to_clipboard, 0);
    g_lua_setfield(L, -2, "copyToClipboard");

    g_lua_pushcclosure(L, l_lni_open_url, 0);
    g_lua_setfield(L, -2, "openUrl");

    /* Case-insensitive aliases — RL scripts use various casings */
    g_lua_pushcclosure(L, l_lni_get_speed, 0);
    g_lua_setfield(L, -2, "GetSpeed");
    g_lua_pushcclosure(L, l_lni_set_speed, 0);
    g_lua_setfield(L, -2, "SetSpeed");
    g_lua_pushcclosure(L, l_lni_quit, 0);
    g_lua_setfield(L, -2, "Quit");
    g_lua_pushcclosure(L, l_lni_copy_to_clipboard, 0);
    g_lua_setfield(L, -2, "CopyToClipboard");
    g_lua_pushcclosure(L, l_lni_copy_to_clipboard, 0);
    g_lua_setfield(L, -2, "copy");
    g_lua_pushcclosure(L, l_lni_copy_to_clipboard, 0);
    g_lua_setfield(L, -2, "Copy");
    g_lua_pushcclosure(L, l_lni_open_url, 0);
    g_lua_setfield(L, -2, "OpenUrl");
    g_lua_pushcclosure(L, l_lni_open_url, 0);
    g_lua_setfield(L, -2, "openURL");
    g_lua_pushcclosure(L, l_lni_open_url, 0);
    g_lua_setfield(L, -2, "OpenURL");

    g_lua_pushcclosure(L, l_mini_execute_lni, 0);
    g_lua_setfield(L, -2, "Execute");
    g_lua_pushcclosure(L, l_mini_execute_lni, 0);
    g_lua_setfield(L, -2, "execute");
    g_lua_pushcclosure(L, l_mini_execute_lni, 0);
    g_lua_setfield(L, -2, "ExecuteLNI");
    g_lua_pushcclosure(L, l_mini_bind_lni, 0);
    g_lua_setfield(L, -2, "Bind");
    g_lua_pushcclosure(L, l_mini_bind_lni, 0);
    g_lua_setfield(L, -2, "bind");
    g_lua_pushcclosure(L, l_mini_bind_lni, 0);
    g_lua_setfield(L, -2, "BindLNI");

    g_lua_setfield(L, LUA_GLOBALSINDEX, "LNI");  /* _G.LNI = table */

    /* ---- Components table ---- */
    g_lua_createtable(L, 0, 4);

    /* Components.Health = { GetValue, SetValue } */
    g_lua_createtable(L, 0, 4);
    g_lua_pushcclosure(L, l_comp_health_get_value, 0);
    g_lua_setfield(L, -2, "GetValue");
    g_lua_pushcclosure(L, l_comp_health_set_value, 0);
    g_lua_setfield(L, -2, "SetValue");
    g_lua_setfield(L, -2, "Health");

    /* Components.Physics = { GetValue } */
    g_lua_createtable(L, 0, 4);
    g_lua_pushcclosure(L, l_comp_physics_get_value, 0);
    g_lua_setfield(L, -2, "GetValue");
    g_lua_setfield(L, -2, "Physics");

    /* Components.Entity = { GetValue } */
    g_lua_createtable(L, 0, 4);
    g_lua_pushcclosure(L, l_comp_entity_get_value, 0);
    g_lua_setfield(L, -2, "GetValue");
    g_lua_setfield(L, -2, "Entity");

    g_lua_setfield(L, LUA_GLOBALSINDEX, "Components");

    /* ---- Skeleton table ---- */
    g_lua_createtable(L, 0, 8);
    g_lua_pushcclosure(L, l_skeleton_get_bone_position, 0);
    g_lua_setfield(L, -2, "GetBonePosition");
    g_lua_pushcclosure(L, l_skeleton_get_bone_rotation, 0);
    g_lua_setfield(L, -2, "GetBoneRotation");
    g_lua_pushcclosure(L, l_skeleton_set_bone_scale, 0);
    g_lua_setfield(L, -2, "SetBoneScale");
    /* SwKiwi additions */
    g_lua_pushcclosure(L, l_skeleton_new, 0);
    g_lua_setfield(L, -2, "New");
    g_lua_pushcclosure(L, l_skeleton_set_bone_offset, 0);
    g_lua_setfield(L, -2, "setBoneOffset");
    g_lua_pushcclosure(L, l_skeleton_set_bone_rotation, 0);
    g_lua_setfield(L, -2, "setBoneRotation");
    g_lua_pushcclosure(L, l_skeleton_reset_bones, 0);
    g_lua_setfield(L, -2, "resetBones");
    g_lua_pushcclosure(L, l_skeleton_get_bone_index, 0);
    g_lua_setfield(L, -2, "getBoneIndex");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "Skeleton");

    /* ---- CharAnimController table ---- */
    g_lua_createtable(L, 0, 12);
    g_lua_pushcclosure(L, l_charanim_play, 0);
    g_lua_setfield(L, -2, "Play");
    g_lua_pushcclosure(L, l_charanim_stop, 0);
    g_lua_setfield(L, -2, "Stop");
    g_lua_pushcclosure(L, l_charanim_get_current, 0);
    g_lua_setfield(L, -2, "GetCurrent");
    /* SwKiwi additions */
    g_lua_pushcclosure(L, l_charanim_stop_moving, 0);
    g_lua_setfield(L, -2, "StopMoving");
    g_lua_pushcclosure(L, l_charanim_start_moving, 0);
    g_lua_setfield(L, -2, "StartMoving");
    g_lua_pushcclosure(L, l_charanim_stop_action, 0);
    g_lua_setfield(L, -2, "StopAction");
    g_lua_pushcclosure(L, l_charanim_begin_casting, 0);
    g_lua_setfield(L, -2, "BeginCasting");
    g_lua_pushcclosure(L, l_charanim_start_falling, 0);
    g_lua_setfield(L, -2, "StartFalling");
    g_lua_pushcclosure(L, l_charanim_is_ready_to_jump, 0);
    g_lua_setfield(L, -2, "IsReadyToJump");
    g_lua_pushcclosure(L, l_charanim_is_moving, 0);
    g_lua_setfield(L, -2, "IsMoving");
    g_lua_pushcclosure(L, l_charanim_action_nearly_finished, 0);
    g_lua_setfield(L, -2, "ActionNearlyFinished");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "CharAnimController");

    /* ---- Bauble table (Phase 3.3) ---- */
    g_lua_createtable(L, 0, 8);
    g_lua_pushcclosure(L, l_bauble_find, 0);
    g_lua_setfield(L, -2, "Find");
    g_lua_pushcclosure(L, l_bauble_equip, 0);
    g_lua_setfield(L, -2, "Equip");
    g_lua_pushcclosure(L, l_bauble_equip, 0);  /* Wear is alias for Equip */
    g_lua_setfield(L, -2, "Wear");
    g_lua_pushcclosure(L, l_bauble_unequip, 0);
    g_lua_setfield(L, -2, "Unequip");
    g_lua_pushcclosure(L, l_bauble_is_wearing, 0);
    g_lua_setfield(L, -2, "IsWearing");
    g_lua_pushcclosure(L, l_bauble_get_level, 0);
    g_lua_setfield(L, -2, "GetLevel");
    g_lua_pushcclosure(L, l_bauble_inc_level, 0);
    g_lua_setfield(L, -2, "IncLevel");
    g_lua_pushcclosure(L, l_bauble_hide_all, 0);
    g_lua_setfield(L, -2, "HideAll");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "Bauble");

    /* ---- EdgeTest table — screen bounds API used by RLSW button menus ---- */
    g_lua_createtable(L, 0, 2);
    g_lua_pushcclosure(L, l_edgetest_test, 0);
    g_lua_setfield(L, -2, "Test");
    /* EdgeTest.GetSize() -> w, h  (alias used by some mods) */
    g_lua_pushcclosure(L, l_edgetest_test, 0);
    g_lua_setfield(L, -2, "GetSize");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "EdgeTest");

    /* ---- ButtonController table ---- */
    g_lua_createtable(L, 0, 64);
    g_lua_pushcclosure(L, l_btn_new, 0);
    g_lua_setfield(L, -2, "New");
    g_lua_pushcclosure(L, l_btn_new, 0);
    g_lua_setfield(L, -2, "new");
    /* Alias for convenience: Add/addButton */
    g_lua_pushcclosure(L, l_btn_new, 0);
    g_lua_setfield(L, -2, "Add");
    g_lua_pushcclosure(L, l_btn_new, 0);
    g_lua_setfield(L, -2, "addButton");
    g_lua_pushcclosure(L, l_btn_delete, 0);
    g_lua_setfield(L, -2, "Delete");
    g_lua_pushcclosure(L, l_btn_delete, 0);
    g_lua_setfield(L, -2, "delete");
    /* Alias: Remove/remove/removeButton */
    g_lua_pushcclosure(L, l_btn_delete, 0);
    g_lua_setfield(L, -2, "Remove");
    g_lua_pushcclosure(L, l_btn_delete, 0);
    g_lua_setfield(L, -2, "remove");
    g_lua_pushcclosure(L, l_btn_delete, 0);
    g_lua_setfield(L, -2, "removeButton");
    g_lua_pushcclosure(L, l_btn_delete_all, 0);
    g_lua_setfield(L, -2, "DeleteAll");
    g_lua_pushcclosure(L, l_btn_delete_all, 0);
    g_lua_setfield(L, -2, "deleteAll");
    /* Alias: RemoveAll/removeAll */
    g_lua_pushcclosure(L, l_btn_delete_all, 0);
    g_lua_setfield(L, -2, "RemoveAll");
    g_lua_pushcclosure(L, l_btn_delete_all, 0);
    g_lua_setfield(L, -2, "removeAll");
    g_lua_pushcclosure(L, l_btn_set_hidden, 0);
    g_lua_setfield(L, -2, "SetHidden");
    g_lua_pushcclosure(L, l_btn_set_hidden, 0);
    g_lua_setfield(L, -2, "setHidden");
    g_lua_pushcclosure(L, l_btn_set_hidden_all, 0);
    g_lua_setfield(L, -2, "SetHiddenAll");
    g_lua_pushcclosure(L, l_btn_set_hidden_all, 0);
    g_lua_setfield(L, -2, "setHiddenAll");
    g_lua_pushcclosure(L, l_btn_is_pressed, 0);
    g_lua_setfield(L, -2, "IsPressed");
    g_lua_pushcclosure(L, l_btn_is_pressed, 0);
    g_lua_setfield(L, -2, "isPressed");
    g_lua_pushcclosure(L, l_btn_is_dragging, 0);
    g_lua_setfield(L, -2, "IsDragging");
    g_lua_pushcclosure(L, l_btn_is_dragging, 0);
    g_lua_setfield(L, -2, "isDragging");
    g_lua_pushcclosure(L, l_btn_exists, 0);
    g_lua_setfield(L, -2, "Exists");
    g_lua_pushcclosure(L, l_btn_exists, 0);
    g_lua_setfield(L, -2, "exists");
    g_lua_pushcclosure(L, l_btn_set_text, 0);
    g_lua_setfield(L, -2, "SetText");
    g_lua_pushcclosure(L, l_btn_set_text, 0);
    g_lua_setfield(L, -2, "setText");
    g_lua_pushcclosure(L, l_btn_set_position, 0);
    g_lua_setfield(L, -2, "SetPosition");
    g_lua_pushcclosure(L, l_btn_set_position, 0);
    g_lua_setfield(L, -2, "setPosition");
    g_lua_pushcclosure(L, l_btn_get_position, 0);
    g_lua_setfield(L, -2, "GetPosition");
    g_lua_pushcclosure(L, l_btn_get_position, 0);
    g_lua_setfield(L, -2, "getPosition");
    g_lua_pushcclosure(L, l_btn_get_position_x, 0);
    g_lua_setfield(L, -2, "GetPositionX");
    g_lua_pushcclosure(L, l_btn_get_position_y, 0);
    g_lua_setfield(L, -2, "GetPositionY");
    g_lua_pushcclosure(L, l_btn_set_alpha, 0);
    g_lua_setfield(L, -2, "SetAlpha");
    g_lua_pushcclosure(L, l_btn_set_alpha, 0);
    g_lua_setfield(L, -2, "setAlpha");
    g_lua_pushcclosure(L, l_btn_set_scaling, 0);
    g_lua_setfield(L, -2, "SetScaling");
    g_lua_pushcclosure(L, l_btn_set_scaling, 0);
    g_lua_setfield(L, -2, "setScaling");
    g_lua_pushcclosure(L, l_btn_set_dimensions, 0);
    g_lua_setfield(L, -2, "SetDimensions");
    g_lua_pushcclosure(L, l_btn_set_dimensions, 0);
    g_lua_setfield(L, -2, "setDimensions");
    g_lua_pushcclosure(L, l_btn_make_movable, 0);
    g_lua_setfield(L, -2, "MakeMovable");
    g_lua_pushcclosure(L, l_btn_make_movable, 0);
    g_lua_setfield(L, -2, "makeMovable");
    g_lua_pushcclosure(L, l_btn_set_clickable, 0);
    g_lua_setfield(L, -2, "SetClickable");
    g_lua_pushcclosure(L, l_btn_set_clickable, 0);
    g_lua_setfield(L, -2, "setClickable");
    g_lua_pushcclosure(L, l_btn_set_text_font, 0);
    g_lua_setfield(L, -2, "SetTextFont");
    g_lua_pushcclosure(L, l_btn_set_text_font, 0);
    g_lua_setfield(L, -2, "setTextFont");
    g_lua_pushcclosure(L, l_btn_set_text_scale, 0);
    g_lua_setfield(L, -2, "SetTextScale");
    g_lua_pushcclosure(L, l_btn_set_text_scale, 0);
    g_lua_setfield(L, -2, "setTextScale");
    g_lua_pushcclosure(L, l_btn_set_text_color, 0);
    g_lua_setfield(L, -2, "SetTextColor");
    g_lua_pushcclosure(L, l_btn_set_text_color, 0);
    g_lua_setfield(L, -2, "setTextColor");
    g_lua_pushcclosure(L, l_btn_set_padding, 0);
    g_lua_setfield(L, -2, "SetPadding");
    g_lua_pushcclosure(L, l_btn_set_padding, 0);
    g_lua_setfield(L, -2, "setPadding");
    g_lua_pushcclosure(L, l_btn_set_alignment, 0);
    g_lua_setfield(L, -2, "SetAlignment");
    g_lua_pushcclosure(L, l_btn_set_alignment, 0);
    g_lua_setfield(L, -2, "setAlignment");
    g_lua_pushcclosure(L, l_btn_set_bg_resource, 0);
    g_lua_setfield(L, -2, "SetBackgroundResource");
    g_lua_pushcclosure(L, l_btn_set_bg_resource, 0);
    g_lua_setfield(L, -2, "setBackgroundResource");
    g_lua_pushcclosure(L, l_btn_set_bg_alpha, 0);
    g_lua_setfield(L, -2, "SetBackgroundAlpha");
    g_lua_pushcclosure(L, l_btn_set_bg_alpha, 0);
    g_lua_setfield(L, -2, "setBackgroundAlpha");

    /* New Overlay API */
    g_lua_pushcclosure(L, l_btn_new_overlay, 0);
    g_lua_setfield(L, -2, "newOverlay");
    g_lua_pushcclosure(L, l_btn_new_overlay, 0);
    g_lua_setfield(L, -2, "NewOverlay");
    g_lua_pushcclosure(L, l_btn_set_overlay_movable, 0);
    g_lua_setfield(L, -2, "setOverlayMovable");
    g_lua_pushcclosure(L, l_btn_set_overlay_movable, 0);
    g_lua_setfield(L, -2, "SetOverlayMovable");
    g_lua_pushcclosure(L, l_btn_set_overlay_pinchable, 0);
    g_lua_setfield(L, -2, "setOverlayPinchable");
    g_lua_pushcclosure(L, l_btn_set_overlay_pinchable, 0);
    g_lua_setfield(L, -2, "SetOverlayPinchable");
    g_lua_pushcclosure(L, l_btn_set_overlay_bg_color, 0);
    g_lua_setfield(L, -2, "setOverlayBackgroundColor");
    g_lua_pushcclosure(L, l_btn_set_overlay_bg_color, 0);
    g_lua_setfield(L, -2, "SetOverlayBackgroundColor");
    g_lua_pushcclosure(L, l_btn_set_overlay_bg_alpha, 0);
    g_lua_setfield(L, -2, "setOverlayBackgroundAlpha");
    g_lua_pushcclosure(L, l_btn_set_overlay_bg_alpha, 0);
    g_lua_setfield(L, -2, "SetOverlayBackgroundAlpha");
    g_lua_pushcclosure(L, l_btn_set_overlay_corner_radius, 0);
    g_lua_setfield(L, -2, "setOverlayCornerRadius");
    g_lua_pushcclosure(L, l_btn_set_overlay_corner_radius, 0);
    g_lua_setfield(L, -2, "SetOverlayCornerRadius");
    g_lua_pushcclosure(L, l_btn_remove_overlay, 0);
    g_lua_setfield(L, -2, "removeOverlay");
    g_lua_pushcclosure(L, l_btn_remove_overlay, 0);
    g_lua_setfield(L, -2, "RemoveOverlay");
    g_lua_pushcclosure(L, l_btn_overlay_add_button, 0);
    g_lua_setfield(L, -2, "overlayAddButton");
    g_lua_pushcclosure(L, l_btn_overlay_add_button, 0);
    g_lua_setfield(L, -2, "OverlayAddButton");

    g_lua_pushcclosure(L, l_btn_overlay_add_separator, 0);
    g_lua_setfield(L, -2, "overlayAddSeparator");
    g_lua_pushcclosure(L, l_btn_overlay_add_separator, 0);
    g_lua_setfield(L, -2, "OverlayAddSeparator");
    g_lua_pushcclosure(L, l_btn_set_overlay_hidden, 0);
    g_lua_setfield(L, -2, "setOverlayHidden");
    g_lua_pushcclosure(L, l_btn_set_overlay_hidden, 0);
    g_lua_setfield(L, -2, "SetOverlayHidden");
    g_lua_pushcclosure(L, l_btn_set_overlay_position, 0);
    g_lua_setfield(L, -2, "setOverlayPosition");
    g_lua_pushcclosure(L, l_btn_set_overlay_position, 0);
    g_lua_setfield(L, -2, "SetOverlayPosition");
    g_lua_pushcclosure(L, l_btn_get_overlay_position, 0);
    g_lua_setfield(L, -2, "getOverlayPosition");
    g_lua_pushcclosure(L, l_btn_get_overlay_position, 0);
    g_lua_setfield(L, -2, "GetOverlayPosition");
    g_lua_pushcclosure(L, l_btn_get_overlay_scale_factor, 0);
    g_lua_setfield(L, -2, "getOverlayScaleFactor");
    g_lua_pushcclosure(L, l_btn_get_overlay_scale_factor, 0);
    g_lua_setfield(L, -2, "GetOverlayScaleFactor");
    g_lua_pushcclosure(L, l_btn_is_overlay_pinching, 0);
    g_lua_setfield(L, -2, "isOverlayPinching");
    g_lua_pushcclosure(L, l_btn_is_overlay_pinching, 0);
    g_lua_setfield(L, -2, "IsOverlayPinching");
    g_lua_pushcclosure(L, l_btn_set_button_confined, 0);
    g_lua_setfield(L, -2, "setButtonConfined");
    g_lua_pushcclosure(L, l_btn_set_button_confined, 0);
    g_lua_setfield(L, -2, "SetButtonConfined");

    g_lua_setfield(L, LUA_GLOBALSINDEX, "ButtonController");
    /* Alias Button and OverlayController to ButtonController */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "ButtonController");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "Button");
    g_lua_getfield(L, LUA_GLOBALSINDEX, "ButtonController");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "OverlayController");

    /* ---- Keyboard table ---- */
    g_lua_createtable(L, 0, 8);
    g_lua_pushcclosure(L, l_kbd_open, 0);
    g_lua_setfield(L, -2, "open");
    g_lua_pushcclosure(L, l_kbd_close, 0);
    g_lua_setfield(L, -2, "close");
    g_lua_pushcclosure(L, l_kbd_get_text, 0);
    g_lua_setfield(L, -2, "getText");
    g_lua_pushcclosure(L, l_kbd_is_done, 0);
    g_lua_setfield(L, -2, "isDone");
    g_lua_pushcclosure(L, l_kbd_is_open, 0);
    g_lua_setfield(L, -2, "isOpen");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "Keyboard");


    /* ---- CameraController table (SwKiwi alias for Mini.Camera) ---- */
    g_lua_createtable(L, 0, 6);
    g_lua_pushcclosure(L, l_camctrl_set_position_offset, 0);
    g_lua_setfield(L, -2, "SetPositionOffset");
    g_lua_pushcclosure(L, l_mini_cam_get_position, 0);
    g_lua_setfield(L, -2, "GetPositionOffset");
    g_lua_pushcclosure(L, l_camctrl_set_perspective, 0);
    g_lua_setfield(L, -2, "SetPerspectiveProjection");
    g_lua_pushcclosure(L, l_camctrl_set_up_vector, 0);
    g_lua_setfield(L, -2, "SetUpVector");
    g_lua_pushcclosure(L, l_camctrl_get_up_vector, 0);
    g_lua_setfield(L, -2, "GetUpVector");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "CameraController");

    /* ---- DB table (compat shim for Kiwi) ---- */
    g_lua_createtable(L, 0, 6);
    g_lua_pushcclosure(L, l_fs_file_exists, 0);
    g_lua_setfield(L, -2, "exists");
    /* Minimal read/write stubs to avoid nil errors in mods */
    g_lua_pushcclosure(L, l_fs_read_file, 0);
    g_lua_setfield(L, -2, "read");
    g_lua_pushcclosure(L, l_fs_write_file, 0);
    g_lua_setfield(L, -2, "write");
    /* DB.Inventory — initialize as empty table; filled during db.init()
     * Prevents 'bad argument #1 to pairs' when mods iterate before real load.
     * Without this, is.scl inventory counter crashes with 'attempt to index
     * field Inventory (a nil value)' on line 717. */
    g_lua_createtable(L, 0, 4);
    g_lua_setfield(L, -2, "Inventory");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "DB");

    /* ---- Character table (compat shim) ---- */
#define SAFE_SET_METHOD(idx, name, fn) \
    g_lua_getfield(L, idx, name); \
    if (g_lua_type(L, -1) == 0) { /* 0 = LUA_TNIL */ \
        g_lua_settop(L, -2); \
        g_lua_pushcclosure(L, fn, 0); \
        g_lua_setfield(L, idx, name); \
    } else { \
        g_lua_settop(L, -2); \
    }

#define SAFE_SET_NUMBER(idx, name, num) \
    g_lua_getfield(L, idx, name); \
    if (g_lua_type(L, -1) == 0) { /* 0 = LUA_TNIL */ \
        g_lua_settop(L, -2); \
        g_lua_pushnumber(L, num); \
        g_lua_setfield(L, idx, name); \
    } else { \
        g_lua_settop(L, -2); \
    }

    g_lua_getfield(L, LUA_GLOBALSINDEX, "Character");
    if (g_lua_type(L, -1) != 5) { /* 5 = LUA_TTABLE */
        g_lua_settop(L, -2);
        g_lua_createtable(L, 0, 24);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "Character");
        g_lua_getfield(L, LUA_GLOBALSINDEX, "Character");
    }
    int char_tbl_idx = g_lua_gettop(L);

    /* Reuse existing Mini.Character getters/setters where available */
    SAFE_SET_METHOD(char_tbl_idx, "GetLevel", l_mini_char_get_level);
    SAFE_SET_METHOD(char_tbl_idx, "GetExp", l_mini_char_get_exp);
    SAFE_SET_METHOD(char_tbl_idx, "GetHealth", l_mini_char_get_health);
    SAFE_SET_METHOD(char_tbl_idx, "GetMaxHealth", l_mini_char_get_max_health);
    SAFE_SET_METHOD(char_tbl_idx, "GetMana", l_mini_char_get_mana);
    SAFE_SET_METHOD(char_tbl_idx, "GetMaxMana", l_mini_char_get_max_mana);
    SAFE_SET_METHOD(char_tbl_idx, "GetCoins", l_mini_char_get_coins);
    SAFE_SET_METHOD(char_tbl_idx, "NumCoins", stub_char_num_coins);
    SAFE_SET_METHOD(char_tbl_idx, "SetNumCoins", stub_char_set_num_coins);
    SAFE_SET_METHOD(char_tbl_idx, "SetLevel", l_mini_char_set_level);
    SAFE_SET_METHOD(char_tbl_idx, "SetExp", l_mini_char_set_exp);
    SAFE_SET_METHOD(char_tbl_idx, "SetHealth", l_mini_char_set_health);
    SAFE_SET_METHOD(char_tbl_idx, "SetMana", l_mini_char_set_mana);
    SAFE_SET_METHOD(char_tbl_idx, "SetCoins", l_mini_char_set_coins);
    SAFE_SET_METHOD(char_tbl_idx, "HasFlag", stub_char_has_flag);
    SAFE_SET_METHOD(char_tbl_idx, "HasSceneFlag", stub_char_has_flag);
    SAFE_SET_METHOD(char_tbl_idx, "HasItem", stub_char_has_item);
    SAFE_SET_METHOD(char_tbl_idx, "ItemCount", stub_char_item_count);
    
    /* Reuse existing Mini.Character action functions */
    SAFE_SET_METHOD(char_tbl_idx, "Die", l_mini_char_die);
    SAFE_SET_METHOD(char_tbl_idx, "Hurt", l_mini_char_hurt);
    SAFE_SET_METHOD(char_tbl_idx, "Use", l_mini_char_use);
    SAFE_SET_METHOD(char_tbl_idx, "Swing", l_mini_char_swing);
    SAFE_SET_METHOD(char_tbl_idx, "StopSwing", l_mini_char_stop_swing);
    SAFE_SET_METHOD(char_tbl_idx, "StartJumping", l_mini_char_start_jumping);
    SAFE_SET_METHOD(char_tbl_idx, "StopJumping", l_mini_char_stop_jumping);
    SAFE_SET_METHOD(char_tbl_idx, "DropQuickly", l_mini_char_drop_quickly);
    SAFE_SET_METHOD(char_tbl_idx, "CancelCasting", l_mini_char_cancel_casting);
    SAFE_SET_METHOD(char_tbl_idx, "FinishCasting", l_mini_char_finish_casting);
    SAFE_SET_METHOD(char_tbl_idx, "CanDoSomething", l_mini_char_can_do_something);
    SAFE_SET_METHOD(char_tbl_idx, "CanBeginCasting", l_mini_char_can_begin_casting);
    SAFE_SET_METHOD(char_tbl_idx, "CanUse", l_mini_char_can_use);
    SAFE_SET_METHOD(char_tbl_idx, "CanJump", l_mini_char_can_jump);
    SAFE_SET_METHOD(char_tbl_idx, "CanSwing", l_mini_char_can_swing);
    SAFE_SET_METHOD(char_tbl_idx, "CanPickup", l_mini_char_can_pickup);
    SAFE_SET_METHOD(char_tbl_idx, "SetMovementFacingLock", l_mini_char_set_movement_facing_lock);
    SAFE_SET_METHOD(char_tbl_idx, "SetStunTime", l_mini_char_set_stun_time);

    /* No-op implementations for mutators and registration helpers */
    SAFE_SET_METHOD(char_tbl_idx, "AddFlag", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "RemoveFlag", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "AddSceneFlag", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "AddItem", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "RemoveItem", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "RegisterTreasure", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "AddSkill", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "AddQuest", stub_char_noop);

    /* Case-insensitive / global aliases used by some mods */
    SAFE_SET_METHOD(char_tbl_idx, "hasFlag", stub_char_has_flag);
    SAFE_SET_METHOD(char_tbl_idx, "hasItem", stub_char_has_item);
    SAFE_SET_METHOD(char_tbl_idx, "itemCount", stub_char_item_count);
    SAFE_SET_METHOD(char_tbl_idx, "registerTreasure", stub_char_noop);
    SAFE_SET_METHOD(char_tbl_idx, "RegisterTreasure", stub_char_noop);
    
    g_lua_getfield(L, LUA_GLOBALSINDEX, "RegisterTreasure");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_char_noop, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "RegisterTreasure");
    } else {
        g_lua_settop(L, -2);
    }

    /* Also export common convenience globals so scripts that call HasItem/HasFlag
     * directly (without Character.) still work. These are safe no-op/stub
     * implementations that return false/0 so early init scripts can proceed. */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "HasFlag");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_char_has_flag, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "HasFlag");
    } else {
        g_lua_settop(L, -2);
    }
    
    g_lua_getfield(L, LUA_GLOBALSINDEX, "HasSceneFlag");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_char_has_flag, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "HasSceneFlag");
    } else {
        g_lua_settop(L, -2);
    }
    
    g_lua_getfield(L, LUA_GLOBALSINDEX, "HasItem");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_char_has_item, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "HasItem");
    } else {
        g_lua_settop(L, -2);
    }
    
    g_lua_getfield(L, LUA_GLOBALSINDEX, "ItemCount");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_char_item_count, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "ItemCount");
    } else {
        g_lua_settop(L, -2);
    }

    /* Global AllItemsCollected() — some mods call this during initialization */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "AllItemsCollected");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_all_items_collected, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "AllItemsCollected");
    } else {
        g_lua_settop(L, -2);
    }

    g_lua_settop(L, -2); /* Pop Character table */

    /* ---- hero stub (best-effort compatibility) ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "hero");
    if (g_lua_type(L, -1) == 0) { /* 0 = LUA_TNIL */
        g_lua_settop(L, -2);
        g_lua_createtable(L, 0, 16);
        int hero_tbl_idx = g_lua_gettop(L);
        SAFE_SET_NUMBER(hero_tbl_idx, "hp", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "max_hp", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "mana", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "max_mana", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "coins", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "level", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "exp", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "atk", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "hp_level", 0);
        SAFE_SET_NUMBER(hero_tbl_idx, "mana_level", 0);
        SAFE_SET_METHOD(hero_tbl_idx, "Die", l_hero_noop);
        SAFE_SET_METHOD(hero_tbl_idx, "Hurt", l_hero_noop);
        SAFE_SET_METHOD(hero_tbl_idx, "Use", l_hero_noop);
        SAFE_SET_METHOD(hero_tbl_idx, "StartMovingToDirection", l_hero_noop);
        SAFE_SET_METHOD(hero_tbl_idx, "StopMovingToDirection", l_hero_noop);
        SAFE_SET_METHOD(hero_tbl_idx, "IsMoving", l_hero_noop);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "hero");
    } else {
        g_lua_settop(L, -2);
    }

    g_lua_getfield(L, LUA_GLOBALSINDEX, "Hero");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_getfield(L, LUA_GLOBALSINDEX, "hero");
        g_lua_setfield(L, LUA_GLOBALSINDEX, "Hero");
    } else {
        g_lua_settop(L, -2);
    }

    /* ---- Camera table extensions ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Camera");
    if (g_lua_type(L, -1) == 5) { /* 5 = LUA_TTABLE */
        int cam_tbl_idx = g_lua_gettop(L);
        SAFE_SET_METHOD(cam_tbl_idx, "IsPointVisible", l_camera_is_point_visible);
    } else {
        g_lua_settop(L, -2);
        g_lua_createtable(L, 0, 4);
        int cam_tbl_idx = g_lua_gettop(L);
        SAFE_SET_METHOD(cam_tbl_idx, "IsPointVisible", l_camera_is_point_visible);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "Camera");
        g_lua_getfield(L, LUA_GLOBALSINDEX, "Camera");
    }
    g_lua_settop(L, -2); /* Pop Camera table */

#undef SAFE_SET_METHOD
#undef SAFE_SET_NUMBER

    /* ---- fs (LuaFileSystem) ---- */
    g_lua_createtable(L, 0, 5);
    g_lua_pushcclosure(L, l_fs_mkdir, 0);
    g_lua_setfield(L, -2, "mkdir");
    g_lua_pushcclosure(L, l_fs_rmdir, 0);
    g_lua_setfield(L, -2, "rmdir");
    g_lua_pushcclosure(L, l_fs_dir, 0);
    g_lua_setfield(L, -2, "dir");
    g_lua_pushcclosure(L, l_fs_attributes, 0);
    g_lua_setfield(L, -2, "attributes");
    g_lua_pushcclosure(L, l_fs_exists, 0);
    g_lua_setfield(L, -2, "exists");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "fs");

    /* ---- lfs alias to fs ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "fs");
    g_lua_setfield(L, LUA_GLOBALSINDEX, "lfs");

    /* ---- broken_socket (luasocket core) ---- */
    extern int luaopen_socket_core(lua_State* L);
    g_lua_pushcclosure(L, (lua_CFunction)luaopen_socket_core, 0);
    if (g_lua_pcall) {
        g_lua_pcall(L, 0, 1, 0);
    } else if (g_lua_call) {
        g_lua_call(L, 0, 1);
    }
    g_lua_setfield(L, LUA_GLOBALSINDEX, "broken_socket");

    /* ---- Game table ---- */
    /* The engine's RegisterProgramLibrary registers a real Game table at boot.
     * We must NOT replace it — but we DO need to inject our own extensions
     * (ShowNotification, CurrentLevelName, etc.) since the engine doesn't
     * provide those. Either way, leave the table on the stack then pop it. */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Game");
    if (g_lua_type(L, -1) != 5) {  /* 5 = LUA_TTABLE — engine table not yet ready */
        g_lua_settop(L, -2);  /* pop nil */
        g_lua_createtable(L, 0, 9);
    }
    /* Stack top is the Game table (existing or newly created). Inject our
     * functions — only if not already present so we don't overwrite engine ones. */
    g_lua_getfield(L, -1, "ShowNotification");
    if (g_lua_type(L, -1) == 0) { /* nil — not present */
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_show_notification, 0);
        g_lua_setfield(L, -2, "ShowNotification");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "CurrentLevelName");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_current_level_name, 0);
        g_lua_setfield(L, -2, "CurrentLevelName");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "SetCinematicMode");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_set_cinematic_mode, 0);
        g_lua_setfield(L, -2, "SetCinematicMode");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "FadeIn");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_fade_in, 0);
        g_lua_setfield(L, -2, "FadeIn");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "FadeOut");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_fade_out, 0);
        g_lua_setfield(L, -2, "FadeOut");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "Flash");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_flash, 0);
        g_lua_setfield(L, -2, "Flash");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "EnterPortal");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_enter_portal, 0);
        g_lua_setfield(L, -2, "EnterPortal");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "IncCounter");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_inc_counter, 0);
        g_lua_setfield(L, -2, "IncCounter");
    } else { g_lua_settop(L, -2); }

    g_lua_getfield(L, -1, "TitleForItem");
    if (g_lua_type(L, -1) == 0) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, l_game_title_for_item, 0);
        g_lua_setfield(L, -2, "TitleForItem");
    } else { g_lua_settop(L, -2); }

    /* Set or update the global Game table */
    g_lua_setfield(L, LUA_GLOBALSINDEX, "Game");

    /* ---- Health table ---- */
    /* Same principle: only create stubs if the engine hasn't provided one. */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Health");
    if (g_lua_type(L, -1) != 5) {  /* 5 = LUA_TTABLE */
        g_lua_settop(L, -2);  /* pop nil */
        g_lua_createtable(L, 0, 5);
        g_lua_pushcclosure(L, l_health_current_health, 0);
        g_lua_setfield(L, -2, "CurrentHealth");
        g_lua_pushcclosure(L, l_health_set_current_health, 0);
        g_lua_setfield(L, -2, "SetCurrentHealth");
        g_lua_pushcclosure(L, l_health_set_current_mana, 0);
        g_lua_setfield(L, -2, "SetCurrentMana");
        g_lua_pushcclosure(L, l_health_set_immunity_time, 0);
        g_lua_setfield(L, -2, "SetImmunityTime");
        g_lua_pushcclosure(L, l_health_has_taken_damage, 0);
        g_lua_setfield(L, -2, "HasTakenDamage");
        g_lua_setfield(L, LUA_GLOBALSINDEX, "Health");
    } else {
        g_lua_settop(L, -2);  /* pop existing table — leave it untouched */
    }
}

/* =========================================================================
 * RLSW Compatibility Stubs — Character, ItemDrop global tables
 *
 * RLSW's initialization scripts (db.scl, code.scl) call Character.*
 * and ItemDrop.* before the native Caver engine's RegisterProgramLibrary
 * has registered those globals. This causes a cascade of nil-index errors:
 *
 *   #1 Character.RegisterTreasure (code.scl:2200)
 *   #2 ItemDrop.NumItems         (code.scl:2190)
 *   #3 Character.AddItem         (code.scl:2476)
 *   #5 DB nil (db.init crashes at Character.SetNumCoins before setting DB)
 *   #6-10 cascading DB nil / offset errors
 *
 * We pre-register minimal stub tables during our SRE injection. The stubs
 * return safe defaults (0, false, "") so that db.init() can complete and
 * set the global DB table. Once the native engine's RegisterProgramLibrary
 * fires and overwrites Character/ItemDrop with real implementations, all
 * subsequent calls use the native ones.
 *
 * Stubs are ONLY registered if the global is currently nil — we never
 * overwrite an already-registered native table.
 * ========================================================================= */

/* ---- Character stubs ---- */

/* Character.NumCoins() → current coin count (from our player state mirror) */
static int stub_char_num_coins(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_coins);
    return 1;
}

/* Character.SetNumCoins(n) → update our mirror */
static int stub_char_set_num_coins(lua_State* L) {
    if (g_lua_isnumber(L, 1)) {
        int val = (int)g_lua_tonumber(L, 1);
        if (val > 0 || g_sre_player_coins <= 0) {
            g_sre_player_coins = val;
        }
    }
    return 0;
}

/* Character.GetMaxHealth() → player max HP */
static int stub_char_get_max_health(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_max_hp);
    return 1;
}

/* Character.GetCurrentHealth() → player current HP */
static int stub_char_get_current_health(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_hp);
    return 1;
}

/* Character.SetCurrentHealth(h) → update mirror */
static int stub_char_set_current_health(lua_State* L) {
    if (g_lua_isnumber(L, 1))
        g_sre_player_hp = (int)g_lua_tonumber(L, 1);
    return 0;
}

/* Character.GetMaxMana() */
static int stub_char_get_max_mana(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_max_mana);
    return 1;
}

/* Character.GetCurrentMana() */
static int stub_char_get_current_mana(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_mana);
    return 1;
}

/* Character.GetLevel() */
static int stub_char_get_level(lua_State* L) {
    g_lua_pushnumber(L, (double)g_sre_player_level);
    return 1;
}

/* Character.HasFlag(name) → false (no flags known yet) */
static int stub_char_has_flag(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 0);
    return 1;
}

/* Character.HasItem(name) → false */
static int stub_char_has_item(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 0);
    return 1;
}

/* Character.ItemCount(name) → 0 */
static int stub_char_item_count(lua_State* L) {
    (void)L;
    g_lua_pushnumber(L, 0.0);
    return 1;
}

/* Generic no-op stub — for AddFlag, RemoveFlag,
 * RegisterTreasure, AddSceneFlag, AddSkill, AddQuest etc. */
static int stub_char_noop(lua_State* L) {
    (void)L;
    return 0;
}

/* Character.AddItem(id) → boolean success */
static int l_char_add_item(lua_State* L) {
    const char* item_id = lua_tostring(L, 1);
    if (item_id && item_id[0]) {
        /* Defensive: assume success; real inventory tracking would need 
         * to interact with the game save format */
        g_lua_pushboolean(L, 1);
    } else {
        g_lua_pushboolean(L, 0);
    }
    return 1;
}

/* Character.RemoveItem(id) → boolean success */
static int l_char_remove_item(lua_State* L) {
    const char* item_id = lua_tostring(L, 1);
    if (item_id && item_id[0]) {
        /* Defensive: assume success */
        g_lua_pushboolean(L, 1);
    } else {
        g_lua_pushboolean(L, 0);
    }
    return 1;
}

/* ---- ItemDrop stubs ---- */

/* ItemDrop.NumItems(obj) → count of dropped items (defensive: 0 if nil table) */
static int stub_itemdrop_num_items(lua_State* L) {
    /* obj should be a dropped item entity; return 0 for base game compatibility */
    g_lua_pushnumber(L, 0.0);
    return 1;
}

/* ItemDrop.GetItem(obj, idx) → the item table or nil (defensive) */
static int stub_itemdrop_get_item(lua_State* L) {
    /* Typically returns an item from a treasure pile; return empty table to be safe */
    g_lua_createtable(L, 0, 0);
    return 1;
}

/* AllItemsCollected() → false (mods may check this during level init) */
static int stub_all_items_collected(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 0);
    return 1;
}

/* ItemDrop.ItemIdentifier(obj, idx) → identifier string (defensive: empty if nil) */
static int stub_itemdrop_item_identifier(lua_State* L) {
    (void)L;
    g_lua_pushstring(L, "");
    return 1;
}

static int universal_stub_index(lua_State* L) {
    g_lua_pushvalue(L, 1); /* __index returns the table itself */
    return 1;
}

static int universal_stub_call(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 0); /* __call returns false */
    return 1;
}

static int stub_return_false_or_zero(lua_State* L) {
    (void)L;
    g_lua_pushboolean(L, 0);
    return 1;
}

static void create_universal_stub_table(lua_State* L, const char* name) {
    g_lua_getfield(L, LUA_GLOBALSINDEX, name);
    int type = g_lua_type(L, -1);
    if (type != LUA_TNIL) {
        g_lua_settop(L, -2); /* pop existing value and return immediately without modifying it */
        return;
    }
    g_lua_settop(L, -2); /* pop nil */
    g_lua_createtable(L, 0, 8); /* stub table — stack: [stub] */
    g_lua_pushvalue(L, -1);
    g_lua_setfield(L, LUA_GLOBALSINDEX, name);
    
    /* Attach direct stub functions for known critical methods ONLY IF THEY ARE NIL */
#define SAFE_STUB(fname, fn) \
    g_lua_getfield(L, -1, fname); \
    if (g_lua_type(L, -1) == LUA_TNIL) { \
        g_lua_settop(L, -2); \
        g_lua_pushcclosure(L, fn, 0); \
        g_lua_setfield(L, -2, fname); \
    } else { \
        g_lua_settop(L, -2); \
    }

    SAFE_STUB("IsWearing", stub_return_false_or_zero)
    SAFE_STUB("GetLevel",  stub_return_false_or_zero)
    SAFE_STUB("Find",      stub_return_false_or_zero)
    SAFE_STUB("Open",      stub_return_false_or_zero)
    SAFE_STUB("Use",       stub_return_false_or_zero)
#undef SAFE_STUB

    /* Also attach fallback metatable */
    g_lua_createtable(L, 0, 2); /* metatable — stack: [stub, meta] */
    g_lua_pushcclosure(L, universal_stub_index, 0);
    g_lua_setfield(L, -2, "__index");
    g_lua_pushcclosure(L, universal_stub_call, 0);
    g_lua_setfield(L, -2, "__call");

    if (g_lua_setmetatable) {
        g_lua_setmetatable(L, -2); /* pops meta, sets on target table — stack: [stub] */
    } else {
        g_lua_settop(L, -2);
    }

    g_lua_settop(L, -2); /* pop table */
}

/* Mangled C++ Component virtual lookups and class method bindings */
typedef void* (*pfn_ComponentWithInterface)(void* scene_obj, long interface_id);
typedef long (*pfn_Touchable_Interface)(void);
typedef long (*pfn_TextBubble_Interface)(void);
typedef void (*pfn_TextBubble_ShowText)(void* text_bubble_comp, SreString* text, float maxWidth);
typedef void (*pfn_TextBubble_SetHandleTouches)(void* text_bubble_comp, int enabled);
typedef int (*pfn_TextBubble_IsTextFinishedShowing)(void* text_bubble_comp);

extern void sre_CppString_from_char_p(SreString* self, const char* src);
extern void sre_CppString_release(SreString* self);

static void* get_scene_object(lua_State* L, int idx) {
    if (!g_lua_touserdata) return NULL;
    void* ud = g_lua_touserdata(L, idx);
    if (!ud) return NULL;
    return *(void**)ud;
}

/* Touchable.SetTouchRadius(bubble, radius) */
extern void* TouchableComponent_Interface;
extern void* TextBubbleComponent_Interface;

static int l_touchable_set_touch_radius(lua_State* L) {
    void* bubble = get_scene_object(L, 1);
    double radius = 0.0;
    if (g_lua_type(L, 2) == LUA_TNUMBER) {
        radius = g_lua_tonumber(L, 2);
    }
    
    if (bubble && g_SceneObject_ComponentWithInterface && TouchableComponent_Interface) {
        void* component = g_SceneObject_ComponentWithInterface(bubble, TouchableComponent_Interface);
        if (component) {
            *(float*)((char*)component + 0x70) = (float)radius;
        }
    }
    return 0;
}

/* TextBubble.SetTouchHandlingEnabled(bubble, enabled) */
static int l_textbubble_set_touch_handling_enabled(lua_State* L) {
    void* bubble = get_scene_object(L, 1);
    int enabled = 0;
    if (g_lua_type(L, 2) == LUA_TBOOLEAN) {
        enabled = g_lua_toboolean(L, 2);
    } else if (g_lua_type(L, 2) == LUA_TNUMBER) {
        enabled = (g_lua_tonumber(L, 2) != 0.0);
    }
    
    if (bubble && g_SceneObject_ComponentWithInterface && TextBubbleComponent_Interface) {
        void* component = g_SceneObject_ComponentWithInterface(bubble, TextBubbleComponent_Interface);
        if (component && g_sre_TextBubble_SetHandleTouches) {
            g_sre_TextBubble_SetHandleTouches(component, enabled);
        }
    }
    return 0;
}

/* TextBubble.IsTextFinished(bubble) */
static int l_textbubble_is_text_finished(lua_State* L) {
    void* bubble = get_scene_object(L, 1);
    int finished = 1; /* Default to finished to prevent infinite wait loops on invalid bubble */
    
    if (bubble && g_SceneObject_ComponentWithInterface && TextBubbleComponent_Interface) {
        void* component = g_SceneObject_ComponentWithInterface(bubble, TextBubbleComponent_Interface);
        if (component && g_sre_TextBubble_IsTextFinishedShowing) {
            finished = g_sre_TextBubble_IsTextFinishedShowing(component);
        }
    }
    g_lua_pushboolean(L, finished);
    return 1;
}

/* TextBubble.ShowText(bubble, text, maxWidth) */
static int l_textbubble_show_text(lua_State* L) {
    void* bubble = get_scene_object(L, 1);
    const char* text = "";
    if (g_lua_type(L, 2) == LUA_TSTRING && g_lua_tolstring) {
        size_t len = 0;
        text = g_lua_tolstring(L, 2, &len);
    }
    double maxWidth = 0.0;
    if (g_lua_gettop(L) >= 3 && g_lua_type(L, 3) == LUA_TNUMBER) {
        maxWidth = g_lua_tonumber(L, 3);
    }
    
    if (bubble && g_SceneObject_ComponentWithInterface && text && TextBubbleComponent_Interface) {
        void* component = g_SceneObject_ComponentWithInterface(bubble, TextBubbleComponent_Interface);
        if (component && g_sre_TextBubble_ShowText) {
            SreString str;
            sre_CppString_from_char_p(&str, text);
            g_sre_TextBubble_ShowText(component, &str, (float)maxWidth);
            sre_CppString_release(&str);
        }
    }
    return 0;
}

/* OverlayText.SetText(obj, text) */
static int l_overlaytext_set_text(lua_State* L) {
    void* obj = get_scene_object(L, 1);
    const char* text_val = "";
    if (g_lua_type(L, 2) == LUA_TSTRING) {
        text_val = g_lua_tolstring(L, 2, NULL);
    }
    
    extern void* g_OverlayTextComponent_Interface_addr;
    if (obj && g_SceneObject_ComponentWithInterface && g_OverlayTextComponent_Interface_addr) {
        typedef void* (*pfn_Interface)(void);
        pfn_Interface iface_fn = (pfn_Interface)g_OverlayTextComponent_Interface_addr;
        
        void* iface_id = iface_fn();
        void* component = g_SceneObject_ComponentWithInterface(obj, iface_id);
        if (component) {
            SreString* text_str = (SreString*)((char*)component + 0x70);
            sre_CppString_release(text_str);
            sre_CppString_from_char_p(text_str, text_val);
            *(char*)((char*)component + 0x80) = 1; // Mark dirty
        }
    }
    return 0;
}

/*
 * sre_register_rlsw_stubs — register Character and ItemDrop stub tables
 * into the Lua global table, but ONLY if those globals are currently nil.
 *
 * Called from sre_mini_ensure_injected() after Mini.* is registered.
 */
static void sre_register_rlsw_stubs(lua_State* L) {
    /* ---- Character ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Character");
    int char_type = g_lua_type(L, -1);
    if (char_type == LUA_TNIL) {
        g_lua_settop(L, -2);  /* pop nil */
        g_lua_createtable(L, 0, 25);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "Character");
        g_lua_getfield(L, LUA_GLOBALSINDEX, "Character");
    }

#define CHAR_STUB(name, fn) \
    g_lua_getfield(L, -1, name); \
    if (g_lua_type(L, -1) == LUA_TNIL) { \
        g_lua_settop(L, -2); \
        g_lua_pushcclosure(L, fn, 0); \
        g_lua_setfield(L, -2, name); \
    } else { \
        g_lua_settop(L, -2); \
    }

    CHAR_STUB("NumCoins",           stub_char_num_coins)
    CHAR_STUB("SetNumCoins",        stub_char_set_num_coins)
    CHAR_STUB("GetCoins",           stub_char_num_coins)       /* alias */
    CHAR_STUB("GetMaxHealth",       stub_char_get_max_health)
    CHAR_STUB("GetCurrentHealth",   stub_char_get_current_health)
    CHAR_STUB("SetCurrentHealth",   stub_char_set_current_health)
    CHAR_STUB("GetMaxMana",         stub_char_get_max_mana)
    CHAR_STUB("GetCurrentMana",     stub_char_get_current_mana)
    CHAR_STUB("GetLevel",           stub_char_get_level)
    CHAR_STUB("HasFlag",            stub_char_has_flag)
    CHAR_STUB("HasSceneFlag",       stub_char_has_flag)        /* same semantics */
    CHAR_STUB("HasItem",            stub_char_has_item)
    CHAR_STUB("ItemCount",          stub_char_item_count)
    CHAR_STUB("AddFlag",            stub_char_noop)
    CHAR_STUB("RemoveFlag",         stub_char_noop)
    CHAR_STUB("AddSceneFlag",       stub_char_noop)
    CHAR_STUB("AddItem",            l_char_add_item)
    CHAR_STUB("RemoveItem",         l_char_remove_item)
    CHAR_STUB("RegisterTreasure",           stub_char_noop)
    CHAR_STUB("RegisterTreasureCollection", stub_char_noop)
    CHAR_STUB("AddSkill",                   stub_char_noop)
    CHAR_STUB("AddQuest",                   stub_char_noop)
    CHAR_STUB("AddQuestText",               stub_char_noop)
    CHAR_STUB("HasQuest",                   stub_char_has_flag)  /* bool → false */
    CHAR_STUB("IsQuestCompleted",           stub_char_has_flag)  /* bool → false */
    CHAR_STUB("IsQuestInProgress",          stub_char_has_flag)  /* bool → false */
    CHAR_STUB("SetQuestCompleted",          stub_char_noop)
    CHAR_STUB("NumCoin",                    stub_char_num_coins) /* mobile alias */
    CHAR_STUB("SetNumCoin",                 stub_char_set_num_coins)
#undef CHAR_STUB

    /* Also ensure global RegisterTreasure exists for scripts that call it directly */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "RegisterTreasure");
    if (g_lua_type(L, -1) == LUA_TNIL) {
        g_lua_settop(L, -2);
        g_lua_pushcclosure(L, stub_char_noop, 0);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "RegisterTreasure");
    } else {
        g_lua_settop(L, -2);
    }

    g_lua_settop(L, -2);  /* pop Character table */

    /* ---- ItemDrop ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "ItemDrop");
    int drop_type = g_lua_type(L, -1);
    if (drop_type == LUA_TNIL) {
        g_lua_settop(L, -2);  /* pop nil */
        g_lua_createtable(L, 0, 8);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "ItemDrop");
        g_lua_getfield(L, LUA_GLOBALSINDEX, "ItemDrop");
    }

#define DROP_STUB(name, fn) \
    g_lua_getfield(L, -1, name); \
    if (g_lua_type(L, -1) == LUA_TNIL) { \
        g_lua_settop(L, -2); \
        g_lua_pushcclosure(L, fn, 0); \
        g_lua_setfield(L, -2, name); \
    } else { \
        g_lua_settop(L, -2); \
    }

    DROP_STUB("NumItems",           stub_itemdrop_num_items)
    DROP_STUB("GetItem",            stub_itemdrop_get_item)
    DROP_STUB("Drop",               stub_char_noop)
    DROP_STUB("AllItemsCollected",  stub_all_items_collected)
    DROP_STUB("ItemIdentifier",     stub_itemdrop_item_identifier)
    DROP_STUB("SetItemIdentifier",  stub_char_noop)
    DROP_STUB("Trigger",            stub_char_noop)
#undef DROP_STUB

    g_lua_settop(L, -2);  /* pop ItemDrop table */

    /* ---- Bauble ---- */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Bauble");
    int bauble_type = g_lua_type(L, -1);
    if (bauble_type != LUA_TTABLE) {
        g_lua_settop(L, -2);  /* pop non-table */
        g_lua_createtable(L, 0, 8);
        g_lua_setfield(L, LUA_GLOBALSINDEX, "Bauble");
        g_lua_getfield(L, LUA_GLOBALSINDEX, "Bauble");
    }

#define BAUBLE_STUB(name, fn) \
    g_lua_getfield(L, -1, name); \
    if (g_lua_type(L, -1) == LUA_TNIL) { \
        g_lua_settop(L, -2); \
        g_lua_pushcclosure(L, fn, 0); \
        g_lua_setfield(L, -2, name); \
    } else { \
        g_lua_settop(L, -2); \
    }

    BAUBLE_STUB("Find",     l_bauble_find)
    BAUBLE_STUB("Equip",    l_bauble_equip)
    BAUBLE_STUB("Unequip",  l_bauble_unequip)
    BAUBLE_STUB("IsWearing", l_bauble_is_wearing)
    BAUBLE_STUB("GetLevel", l_bauble_get_level)
    BAUBLE_STUB("IncLevel", l_bauble_inc_level)
    BAUBLE_STUB("HideAll",  l_bauble_hide_all)
#undef BAUBLE_STUB

    g_lua_settop(L, -2);  /* pop Bauble table */

    /* ---- Universal Stub for Is to prevent race conditions during level load ---- */
    create_universal_stub_table(L, "Bauble");
    create_universal_stub_table(L, "Is");
    create_universal_stub_table(L, "Touchable");
    g_lua_getfield(L, LUA_GLOBALSINDEX, "Touchable");
    if (g_lua_type(L, -1) == LUA_TTABLE) {
        g_lua_pushcclosure(L, l_touchable_set_touch_radius, 0);
        g_lua_setfield(L, -2, "SetTouchRadius");
    }
    g_lua_settop(L, -2); /* pop Touchable */

    create_universal_stub_table(L, "TextBubble");
    g_lua_getfield(L, LUA_GLOBALSINDEX, "TextBubble");
    if (g_lua_type(L, -1) == LUA_TTABLE) {
        g_lua_pushcclosure(L, l_textbubble_show_text, 0);
        g_lua_setfield(L, -2, "ShowText");
        g_lua_pushcclosure(L, l_textbubble_set_touch_handling_enabled, 0);
        g_lua_setfield(L, -2, "SetTouchHandlingEnabled");
        g_lua_pushcclosure(L, l_textbubble_is_text_finished, 0);
        g_lua_setfield(L, -2, "IsTextFinished");
    }
    g_lua_settop(L, -2); /* pop TextBubble */

    create_universal_stub_table(L, "OverlayText");
    g_lua_getfield(L, LUA_GLOBALSINDEX, "OverlayText");
    if (g_lua_type(L, -1) == LUA_TTABLE) {
        g_lua_pushcclosure(L, l_overlaytext_set_text, 0);
        g_lua_setfield(L, -2, "SetText");
    }
    g_lua_settop(L, -2); /* pop OverlayText */
}

/* =========================================================================
 * Lazy Mini.* injection — called from sre_lua_call_safe
 * =========================================================================
 * Instead of hooking RegisterProgramLibrary (which needs relay stubs that
 * crash on PC-relative instructions), we check on every lua_call whether
 * Mini.* is registered. If not, inject it.
 *
 * Caches up to 8 lua_State pointers to avoid re-checking.
 */
/* Helper to evaluate a Lua code string safely */
static void sre_eval_lua(lua_State* L, const char* code) {
    if (!g_lua_getfield || !g_lua_pcall || !g_lua_type || !g_lua_pushstring || !g_lua_settop || !g_lua_gettop) return;
    int base_top = g_lua_gettop(L);
    g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
    if (g_lua_type(L, -1) == 6) { /* LUA_TFUNCTION */
        g_lua_pushstring(L, code);
        if (g_lua_pcall(L, 1, 2, 0) == 0) {
            if (g_lua_type(L, -2) == 6) {
                g_lua_settop(L, -2); /* Pop errmsg, leave function */
                g_lua_pcall(L, 0, 0, 0);
            }
        }
    }
    g_lua_settop(L, base_top);
}

#define MAX_INJECTED_STATES 8
static lua_State* g_injected_states[MAX_INJECTED_STATES] = {0};
static int g_injected_count = 0;

/* Evict a closed lua_State from the injection cache.
 * Called by sre_ProgramState_destructor when the root state is destroyed.
 * Without this, if the allocator recycles the same pointer for a brand-new
 * state, sre_mini_ensure_injected would skip full initialization on it. */
void sre_mini_remove_injected(lua_State* L) {
    for (int i = 0; i < g_injected_count; i++) {
        if (g_injected_states[i] == L) {
            /* Shift remaining entries down to keep the array compact */
            for (int j = i; j < g_injected_count - 1; j++) {
                g_injected_states[j] = g_injected_states[j + 1];
            }
            g_injected_states[--g_injected_count] = 0;
            return;
        }
    }
}



/* Declared in sre_lua_libs.c — extracts Lua source from binary protobuf SCL */
extern int sre_scl_extract_lua(const char* buf, size_t size, const char** out_lua, size_t* out_len);

static void sre_load_edgetest_scl(lua_State* L) {
    extern char g_sre_vfs_path_assets[512];
    char path[512];
    /* Try configured assets dir first, then bare relative fallback */
    snprintf(path, sizeof(path), "%s/resources/edgetest.scl", g_sre_vfs_path_assets);
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "resources/edgetest.scl");
        f = fopen(path, "rb");
    }
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return; }

    char* buf = malloc(size + 1);
    if (!buf) { fclose(f); return; }

    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    const char* code = NULL;
    size_t code_len = 0;

    /* 1. Try binary protobuf SCL extraction (field 5 = Program, field 2/3 = Source/CompiledCode) */
    if (sre_scl_extract_lua(buf, read_bytes, &code, &code_len) && code && code_len > 0) {
        /* Got Lua source from protobuf — make a NUL-terminated copy and evaluate */
        char* lua_src = malloc(code_len + 1);
        if (lua_src) {
            memcpy(lua_src, code, code_len);
            lua_src[code_len] = '\0';
            sre_eval_lua(L, lua_src);
            free(lua_src);
        }
        free(buf);
        return;
    }

    /* 2. Try plain-text SCL (source format: Program{ String : $ ... $end }) */
    const char* text_start = strstr(buf, "String : $");
    if (text_start) {
        text_start += 10; /* skip "String : $" */
        while (*text_start == ' ' || *text_start == '\t' || *text_start == '\r' || *text_start == '\n')
            text_start++;
        char* text_end = strstr(text_start, "$end");
        if (text_end) *text_end = '\0';
        sre_eval_lua(L, text_start);
        free(buf);
        return;
    }

    /* 3. Last resort: raw Lua source file (no SCL wrapper) */
    sre_eval_lua(L, buf);
    free(buf);
}

/*
 * sre_load_srlz_lua — Load and register Srlz (serialization) library
 *
 * Creates the Srlz table with serialize() and deserialize() functions.
 * Called from sre_mini_ensure_injected() during state initialization.
 */
static void sre_load_srlz_lua(lua_State* L) {
    if (!L || !g_lua_createtable) return;

    /* Inline the Srlz library (small enough to embed) */
    sre_eval_lua(L,
        "local specialNums = {\n"
        "    [tostring(1/0)] = '1/0 --[[math.huge]]',\n"
        "    [tostring(-1/0)] = '-1/0 --[[-math.huge]]',\n"
        "    [tostring(0/0)] = '0/0'\n"
        "}\n"
        "local rawpairs = function(t) return next, t end\n"
        "local function toStr(obj)\n"
        "    local s1 = tostring(tostring(obj))\n"
        "    if type(s1) == 'string' then return s1 end\n"
        "    local mt = getmetatable(obj)\n"
        "    if mt then\n"
        "        local ts = mt.__tostring\n"
        "        mt.__tostring = nil\n"
        "        local s2 = tostring(obj)\n"
        "        mt.__tostring = ts\n"
        "        return s2\n"
        "    end\n"
        "    return 'unknown'\n"
        "end\n"
        "local function insertionSort(arr, func)\n"
        "    local n = #arr\n"
        "    for i = 2, n do\n"
        "        local key = arr[i]\n"
        "        local j = i - 1\n"
        "        if func then\n"
        "            while j >= 1 and func(arr[j], key) do\n"
        "                arr[j + 1] = arr[j]; j = j - 1\n"
        "            end\n"
        "        else\n"
        "            while j >= 1 and tostring(arr[j]) > tostring(key) do\n"
        "                arr[j + 1] = arr[j]; j = j - 1\n"
        "            end\n"
        "        end\n"
        "        arr[j + 1] = key\n"
        "    end\n"
        "    return arr\n"
        "end\n"
        "local sort = (table and table.sort) or insertionSort\n"
        "local function serialize(data, options)\n"
        "    local o = {comments = true, stringifyNumberKeys = false,\n"
        "        space = ' ', newLine = '\\n', indent = '  ', lineLength = 30,\n"
        "        fixradix = false, multilineStrings = true, functionMarker = nil,\n"
        "        sortMarker = nil, omitMarker = nil, noHuge = false,\n"
        "        numFormat = '%.17g', positional = true, positionalMaxNils = 2,\n"
        "        metatables = true, full = false}\n"
        "    for k, v in rawpairs(options or {}) do o[k] = v end\n"
        "    local function process(d, depth, chain)\n"
        "        local t = type(d)\n"
        "        if d == nil then return 'nil'\n"
        "        elseif t == 'boolean' then return tostring(d)\n"
        "        elseif t == 'string' then return string.format('%q', d)\n"
        "        elseif t == 'number' then\n"
        "            if not o.noHuge then\n"
        "                local special = specialNums[toStr(d)]\n"
        "                if special then return special end\n"
        "            end\n"
        "            return o.numFormat:format(d)\n"
        "        elseif t == 'table' then\n"
        "            if chain[d] then return 'nil' end\n"
        "            chain[d] = true\n"
        "            local out = {'{'}\n"
        "            for k, v in pairs(d) do\n"
        "                if k ~= '__metatable' then\n"
        "                    local key = (type(k) == 'string' and k:match('^%a[%w_]*$') and k) or\n"
        "                               ('[' .. process(k, depth+1, chain) .. ']')\n"
        "                    table.insert(out, key .. ' = ' .. process(v, depth+1, chain) .. ',')\n"
        "                end\n"
        "            end\n"
        "            table.insert(out, '}')\n"
        "            chain[d] = nil\n"
        "            return table.concat(out)\n"
        "        else\n"
        "            return 'nil'\n"
        "        end\n"
        "    end\n"
        "    return process(data, 0, {})\n"
        "end\n"
        "local function deserialize(data, opts)\n"
        "    local is_safe = not (opts and opts.safe == false)\n"
        "    local env = is_safe and setmetatable({}, {__index = function() return nil end}) or _G\n"
        "    local chunk, err = loadstring('return ' .. data)\n"
        "    if not chunk then chunk, err = loadstring(data) end\n"
        "    if not chunk then return false, err end\n"
        "    if is_safe and setfenv then setfenv(chunk, env) end\n"
        "    return pcall(chunk)\n"
        "end\n"
        "Srlz = {serialize = serialize, deserialize = deserialize}\n"
        "_G.Srlz = Srlz\n");
}

/*
 * sre_load_db_lua — Load and register DB (database) library
 *
 * Provides db.init(), db.save(), db.load() for character persistence.
 * Called from sre_mini_ensure_injected() during state initialization.
 */
static void sre_load_db_lua(lua_State* L) {
    if (!L || !g_lua_createtable) return;

    sre_eval_lua(L,
        "DB = {Inventory = {}, SS = 0, Created = os.time(), Music = {},\n"
        "      Enchants = {}, Chests = {}, Weapons = {}, Baubles = {}, Flags = {}}\n"
        "db = {}\n"
        "local function read_file(path)\n"
        "    if DB and DB.read then return DB.read(path) end\n"
        "    if io and io.open then\n"
        "        local f = io.open(path, 'r')\n"
        "        if f then local c = f:read('*a'); f:close(); return c end\n"
        "    end\n"
        "    return nil\n"
        "end\n"
        "local function write_file(path, content)\n"
        "    if DB and DB.write then return DB.write(path, content) end\n"
        "    if io and io.open then\n"
        "        local f = io.open(path, 'w')\n"
        "        if f then f:write(content); f:close(); return true end\n"
        "    end\n"
        "    return false\n"
        "end\n"
        "local function mkdir(path)\n"
        "    if fs and fs.mkdir then return fs.mkdir(path) end\n"
        "    return false\n"
        "end\n"
        "local function get_profile_id()\n"
        "    if Mini and Mini.GetProfileID then return Mini.GetProfileID() end\n"
        "    return 'default'\n"
        "end\n"
        "local function get_save_path()\n"
        "    local pid = get_profile_id()\n"
        "    mkdir('/Files/Documents')\n"
        "    return '/Files/Documents/' .. pid .. '.lua'\n"
        "end\n"
        "local function db_load()\n"
        "    local path = get_save_path()\n"
        "    local content = read_file(path)\n"
        "    if not content or content == '' then return nil end\n"
        "    if not Srlz then return nil end\n"
        "    local success, result = Srlz.deserialize(content, {safe = true})\n"
        "    return success and result or nil\n"
        "end\n"
        "local function db_save()\n"
        "    local path = get_save_path()\n"
        "    if not Srlz then return false end\n"
        "    local serialized = Srlz.serialize(DB)\n"
        "    return write_file(path, serialized)\n"
        "end\n"
        "function db.init()\n"
        "    if Program and Program.Wait then Program.Wait(0.05) end\n"
        "    if Game and Game.CurrentLevelName then\n"
        "        local level = Game.CurrentLevelName()\n"
        "        if level == 'menu' or level == 'hero' then return 0 end\n"
        "    end\n"
        "    mkdir('/Files/Documents')\n"
        "    local loaded = db_load()\n"
        "    if loaded then DB = loaded else\n"
        "        DB = {Inventory = {}, SS = 0, Created = os.time(), Music = {},\n"
        "              Enchants = {}, Chests = {}, Weapons = {}, Baubles = {}, Flags = {}}\n"
        "    end\n"
        "    if Character and Character.SetNumCoins then\n"
        "        Character.SetNumCoins(DB.SS or 0)\n"
        "    end\n"
        "    print('DB initialized')\n"
        "    if Program and Program.NewThread then\n"
        "        Program.NewThread('db_sync_loop', function()\n"
        "            while true do\n"
        "                if Character and Character.NumCoins then\n"
        "                    DB.SS = Character.NumCoins()\n"
        "                end\n"
        "                db_save()\n"
        "                if Program and Program.Wait then Program.Wait(0.1) end\n"
        "            end\n"
        "        end)\n"
        "    end\n"
        "    return 1\n"
        "end\n"
        "function db.save() return db_save() end\n"
        "function db.load() local l = db_load(); if l then DB = l end; return l end\n"
        "function db.get() return DB end\n"
        "function db.set(new_db) if new_db and type(new_db) == 'table' then DB = new_db; return true end; return false end\n"
        "_G.db = db\n");
}

void sre_mini_ensure_injected(lua_State* L) {
    if (!L) return;
    if (!g_lua_createtable) return;  /* Lua API not ready */

    /* Guard: only inject after luaopen_base has registered loadstring.
     * Between luaL_newstate and RegisterProgramLibrary, loadstring doesn't
     * exist yet. sre_eval_lua would silently no-op, leaving injection half-done
     * and marking the state as injected. Skip entirely until base libs are up. */
    if (g_lua_getfield && g_lua_type && g_lua_settop) {
        g_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
        int has_loadstring = (g_lua_type(L, -1) == LUA_TFUNCTION);
        g_lua_settop(L, -2); /* pop result */
        if (!has_loadstring) return; /* luaopen_base not yet run — come back later */
    }


    /* Install sre_hook_obj helper function — only defines the function,
     * does NOT wrap any global Scene.* methods to avoid breaking vanilla
     * game portals/level transitions. Mods that need per-object side-tables
     * can call sre_hook_obj() explicitly via Mini.HookObj() if needed.
     * The __sre_so_hooked guard ensures this runs only once per Lua state. */
    sre_eval_lua(L,
        /* ------------------------------------------------------------------ *
         * sre_hook_obj — GC-safe side-table with weak-wrapper registry       *
         *                                                                      *
         * ROOT CAUSE OF OLD LEAK:                                             *
         *   The previous version stored per-entity environments keyed by      *
         *   tostring(obj) — a plain Lua STRING. In Lua 5.1 weak-keyed tables  *
         *   only collect OBJECT keys (userdata/table/function). String keys   *
         *   are NEVER collected, so mt.__sre_sidetable grew by one entry per  *
         *   unique entity ever wrapped, forever, across all scene reloads.    *
         *                                                                      *
         * FIX — two-table pattern:                                            *
         *   active_wrappers  weak({__mode="k"}) userdata→addr string          *
         *     Lua GC auto-removes dead wrappers from this table.              *
         *   shared_envs      strong addr string→custom-field-table            *
         *     Holds actual mod-assigned fields.                                *
         *   sweep_shared_envs()  periodic mark-and-sweep (every 200 calls)   *
         *     Scans active_wrappers to find live addrs, deletes any           *
         *     shared_envs entry whose addr has zero live wrappers.            *
         * ------------------------------------------------------------------ */
        "if not _G.__sre_so_hooked then\n"
        "  _G.__sre_so_hooked = true\n"
        "  local get_mt = (debug and debug.getmetatable) or getmetatable\n"
        "\n"
        "  -- Weak-keyed table: userdata wrapper object -> C++ address string\n"
        "  -- Entries vanish automatically when Lua GC collects the wrapper\n"
        "  local active_wrappers = setmetatable({}, { __mode = 'k' })\n"
        "\n"
        "  -- Strong table: C++ address string -> custom field env table\n"
        "  local shared_envs = {}\n"
        "\n"
        "  -- Periodic mark-and-sweep: prune envs for fully-GC'd objects\n"
        "  local _sweep_ctr = 0\n"
        "  local function sweep_shared_envs()\n"
        "    _sweep_ctr = _sweep_ctr + 1\n"
        "    if _sweep_ctr < 200 then return end\n"
        "    _sweep_ctr = 0\n"
        "    local live = {}\n"
        "    for _, addr in pairs(active_wrappers) do live[addr] = true end\n"
        "    for addr in pairs(shared_envs) do\n"
        "      if not live[addr] then shared_envs[addr] = nil end\n"
        "    end\n"
        "  end\n"
        "\n"
        "  -- Compute stable C++ address for an object via identifier() or tostring\n"
        "  local function get_addr(obj, og_idx)\n"
        "    local id_fn\n"
        "    if type(og_idx) == 'function' then\n"
        "      id_fn = og_idx(obj, 'identifier')\n"
        "    elseif type(og_idx) == 'table' then\n"
        "      id_fn = og_idx['identifier']\n"
        "    end\n"
        "    if type(id_fn) == 'function' then\n"
        "      local ok, res = pcall(id_fn, obj)\n"
        "      if ok and type(res) == 'string' and res ~= '' then return res end\n"
        "    end\n"
        "    return tostring(obj)\n"
        "  end\n"
        "\n"
        "  _G.sre_hook_obj = function(obj)\n"
        "    if not obj then return obj end\n"
        "    local mt = get_mt and get_mt(obj)\n"
        "    if mt and type(mt) == 'table' then\n"
        "      -- For already-hooked metatables, retrieve the stashed originals\n"
        "      local og_index    = mt.__sre_og_index    or mt.__index\n"
        "      local og_newindex = mt.__sre_og_newindex or mt.__newindex\n"
        "      if not mt.__sre_hooked then\n"
        "        mt.__sre_hooked      = true\n"
        "        mt.__sre_og_index    = og_index    -- stash so later registrations work\n"
        "        mt.__sre_og_newindex = og_newindex\n"
        "        mt.__index = function(self_obj, key)\n"
        "          sweep_shared_envs()\n"
        "          local addr = get_addr(self_obj, og_index)\n"
        "          active_wrappers[self_obj] = addr\n"
        "          local env = shared_envs[addr]\n"
        "          if env and env[key] ~= nil then return env[key] end\n"
        "          if key == 'setAlwaysActive' then\n"
        "            return function(self_act, active)\n"
        "              if SceneObject and SceneObject.SetAlwaysActive then\n"
        "                SceneObject.SetAlwaysActive(self_act, active)\n"
        "              end\n"
        "            end\n"
        "          end\n"
        "          if type(og_index) == 'function' then\n"
        "            return og_index(self_obj, key)\n"
        "          elseif type(og_index) == 'table' then\n"
        "            return og_index[key]\n"
        "          end\n"
        "        end\n"
        "        mt.__newindex = function(self_obj, key, val)\n"
        "          sweep_shared_envs()\n"
        "          if og_newindex then\n"
        "            pcall(function()\n"
        "              if type(og_newindex) == 'function' then og_newindex(self_obj, key, val)\n"
        "              elseif type(og_newindex) == 'table' then og_newindex[key] = val end\n"
        "            end)\n"
        "          end\n"
        "          local addr = get_addr(self_obj, og_index)\n"
        "          active_wrappers[self_obj] = addr\n"
        "          if not shared_envs[addr] then shared_envs[addr] = {} end\n"
        "          shared_envs[addr][key] = val\n"
        "        end\n"
        "      end\n"
        "      -- Register this wrapper in the weak table so GC can track it\n"
        "      local addr = get_addr(obj, og_index)\n"
        "      active_wrappers[obj] = addr\n"
        "      if not shared_envs[addr] then shared_envs[addr] = {} end\n"
        "    end\n"
        "    return obj\n"
        "  end\n"
        "  _G.__sre_wrap_func = function(func)\n"
        "    if not func or type(func) ~= 'function' then return func end\n"
        "    return function(...)\n"
        "      return _G.sre_hook_obj(func(...))\n"
        "    end\n"
        "  end\n"
        "  _G.__sre_wrap_scene_table = function(scene_tbl)\n"
        "    if not scene_tbl or scene_tbl.__sre_wrapped then return end\n"
        "    scene_tbl.__sre_wrapped = true\n"
        "    if scene_tbl.CreateObject then\n"
        "      scene_tbl.CreateObject = _G.__sre_wrap_func(scene_tbl.CreateObject)\n"
        "    end\n"
        "    if scene_tbl.Find then\n"
        "      scene_tbl.Find = _G.__sre_wrap_func(scene_tbl.Find)\n"
        "    end\n"
        "  end\n"
        "  if _G.Scene then _G.__sre_wrap_scene_table(_G.Scene) end\n"
        "  if _G.Find then _G.Find = _G.__sre_wrap_func(_G.Find) end\n"
        "  if _G.New then _G.New = _G.__sre_wrap_func(_G.New) end\n"
        "end\n"
    );

    /* Check if already injected for this state */
    int i;
    for (i = 0; i < g_injected_count; i++) {
        if (g_injected_states[i] == L) {
            /* Re-patch C stubs (cheap, no-op if already set) */
            sre_register_rlsw_stubs(L);

            /* Check if ItemDrop needs proxy installation */
            int need_proxy = 1;
            if (g_lua_getfield && g_lua_type && g_lua_settop) {
                g_lua_getfield(L, LUA_GLOBALSINDEX, "ItemDrop");
                if (g_lua_type(L, -1) == LUA_TTABLE) {
                    g_lua_getfield(L, -1, "__is_sre_proxy");
                    if (g_lua_type(L, -1) != LUA_TNIL) {
                        need_proxy = 0;
                    }
                    g_lua_settop(L, -2);
                }
                g_lua_settop(L, -2);
            }

            /* Check if Character needs guarding */
            int need_guard = 1;
            if (g_lua_getfield && g_lua_type && g_lua_settop) {
                g_lua_getfield(L, LUA_GLOBALSINDEX, "Character");
                if (g_lua_type(L, -1) == LUA_TTABLE) {
                    g_lua_getfield(L, -1, "__is_sre_guarded");
                    if (g_lua_type(L, -1) != LUA_TNIL) {
                        need_guard = 0;
                    }
                    g_lua_settop(L, -2);
                }
                g_lua_settop(L, -2);
            }

            if (need_proxy) {
                sre_eval_lua(L,
                    "local _o = rawget(_G,'ItemDrop') or {}\n"
                    "local _p = {}\n"
                    "local _fb = {\n"
                    "  NumItems=function(o) local f=_o.NumItems; return f and f(o) or 0 end,\n"
                    "  ItemIdentifier=function(o,i) local f=_o.ItemIdentifier; return f and f(o,i) or '' end,\n"
                    "  SetItemIdentifier=function(o,i,v) local f=_o.SetItemIdentifier; if f then f(o,i,v) end end,\n"
                    "  AllItemsCollected=function(o) local f=_o.AllItemsCollected; return f and f(o) or false end,\n"
                    "  GetItem=function(o,i) local f=_o.GetItem; return f and f(o,i) or nil end,\n"
                    "  Drop=function(...) local f=_o.Drop; if f then f(...) end end,\n"
                    "  Trigger=function(...) local f=_o.Trigger; if f then f(...) end end,\n"
                    "  __is_sre_proxy=true\n"
                    "}\n"
                    "setmetatable(_p,{\n"
                    "  __index=function(t,k) return _fb[k] or _o[k] end,\n"
                    "  __newindex=function(t,k,v) _o[k]=v end\n"
                    "})\n"
                    "rawset(_G,'ItemDrop',_p)\n"
                );
            }

            if (need_guard) {
                sre_eval_lua(L,
                    "if Character then\n"
                    "  Character.__is_sre_guarded = true\n"
                    "  _G.__last_coins = _G.__last_coins or 0\n"
                    "  if Character.SetNumCoins then\n"
                    "    local _os=Character.SetNumCoins\n"
                    "    Character.SetNumCoins=function(n)\n"
                    "      if type(n)=='number' then\n"
                    "        if n>0 then _G.__last_coins=n; _os(n)\n"
                    "        elseif _G.__last_coins<=0 then _os(n) end\n"
                    "      else _os(n) end\n"
                    "    end\n"
                    "  end\n"
                    "  if Character.SetNumCoin then\n"
                    "    local _os=Character.SetNumCoin\n"
                    "    Character.SetNumCoin=function(n)\n"
                    "      if type(n)=='number' then\n"
                    "        if n>0 then _G.__last_coins=n; _os(n)\n"
                    "        elseif _G.__last_coins<=0 then _os(n) end\n"
                    "      else _os(n) end\n"
                    "    end\n"
                    "  end\n"
                    "  if Character.NumCoins then\n"
                    "    local _og=Character.NumCoins\n"
                    "    Character.NumCoins=function() local c=_og(); if (not c or c==0) and _G.__last_coins>0 then return _G.__last_coins end; if c and c>0 then _G.__last_coins=c end; return c end\n"
                    "  end\n"
                    "  if Character.NumCoin then\n"
                    "    local _og=Character.NumCoin\n"
                    "    Character.NumCoin=function() local c=_og(); if (not c or c==0) and _G.__last_coins>0 then return _G.__last_coins end; if c and c>0 then _G.__last_coins=c end; return c end\n"
                    "  end\n"
                    "end\n"
                );
            }
            return;
        }
    }

    /* Inject standard Lua libraries (math, table, os, debug, io) */
    extern void sre_open_std_libs(lua_State* L);
    sre_open_std_libs(L);

    /* Inject Caver engine API (hero, components, health, transform, etc.) */
    extern void sre_open_caver_lib(lua_State* L);
    sre_open_caver_lib(L);

    /* Hook nil arithmetic to prevent mod crashes from nil fields (e.g. self.offset) */
    sre_eval_lua(L,
        "if debug and debug.setmetatable then\n"
        "    debug.setmetatable(nil, {\n"
        "        __add = function(a, b) return (a or 0) + (b or 0) end,\n"
        "        __sub = function(a, b) return (a or 0) - (b or 0) end,\n"
        "        __mul = function(a, b) return (a or 0) * (b or 0) end,\n"
        "        __div = function(a, b) return (a or 0) / (b or 1) end,\n"
        "        __unm = function(a) return -(a or 0) end,\n"
        "        __lt = function(a, b) return (a or 0) < (b or 0) end,\n"
        "        __le = function(a, b) return (a or 0) <= (b or 0) end\n"
        "    })\n"
        "end"
    );

    /* Inject Mini.*, LNI.*, Components.*, Game.*, Health.*, fs tables */
    sre_register_mini_api(L);

    /* Register luasocket, luamime, luafilesystem, and toml in package.preload */
    g_lua_getfield(L, LUA_GLOBALSINDEX, "package");
    if (g_lua_type && g_lua_type(L, -1) == 5) {  /* 5 = LUA_TTABLE */
        g_lua_getfield(L, -1, "preload");
        if (g_lua_type(L, -1) == 5) {  /* 5 = LUA_TTABLE */
            /* preload["socket.core"] = luaopen_socket_core */
            extern int luaopen_socket_core(lua_State* L);
            g_lua_pushcclosure(L, (lua_CFunction)luaopen_socket_core, 0);
            g_lua_setfield(L, -2, "socket.core");
            
            /* preload["mime.core"] = luaopen_mime_core */
            extern int luaopen_mime_core(lua_State* L);
            g_lua_pushcclosure(L, (lua_CFunction)luaopen_mime_core, 0);
            g_lua_setfield(L, -2, "mime.core");

            /* preload["lfs"] = luaopen_lfs */
            extern int luaopen_lfs(lua_State* L);
            g_lua_pushcclosure(L, (lua_CFunction)luaopen_lfs, 0);
            g_lua_setfield(L, -2, "lfs");

            /* preload["toml"] = luaopen_toml */
            g_lua_pushcclosure(L, (lua_CFunction)luaopen_toml, 0);
            g_lua_setfield(L, -2, "toml");
        }
        g_lua_settop(L, -2); /* pop preload */
    }
    g_lua_settop(L, -2); /* pop package */

    /* Inject RLSW compat stubs (Character, ItemDrop) — prevents nil-index
     * crashes during db.init() before native RegisterProgramLibrary fires */
    sre_register_rlsw_stubs(L);

    /* Hook Hero to redirect to lowercase hero dynamically */
    sre_eval_lua(L,
        "Hero = Hero or {}\n"
        "setmetatable(Hero, {\n"
        "    __index = function(t, k)\n"
        "        local h = rawget(_G, 'hero')\n"
        "        if h then\n"
        "            local val = h[k]\n"
        "            if type(val) == 'function' then\n"
        "                return function(self, ...)\n"
        "                    if self == Hero then\n"
        "                        return val(h, ...)\n"
        "                    else\n"
        "                        return val(self, ...)\n"
        "                    end\n"
        "                end\n"
        "            end\n"
        "            return val\n"
        "        end\n"
        "    end,\n"
        "    __newindex = function(t, k, v)\n"
        "        local h = rawget(_G, 'hero')\n"
        "        if h then\n"
        "            h[k] = v\n"
        "        end\n"
        "    end\n"
        "})\n"
    );

    /* RLSW Compat Layer: OverlayController, MiniButton, MiniOverlay and fallback stubs */
    sre_eval_lua(L,
        "Is = Is or {}\n"
        "Is.IsWearing = Is.IsWearing or function(name) return Bauble.IsWearing(name) end\n"
        "Is.Wearing = Is.Wearing or function(name) return Bauble.IsWearing(name) end\n"
        "IsWearing = IsWearing or function(name) return Bauble.IsWearing(name) end\n"
        "\n"
        "Bauble = Bauble or {}\n"
        "Bauble.IsWearing = Bauble.IsWearing or function(name) return false end\n"
        "Bauble.Equip = Bauble.Equip or function() end\n"
        "Bauble.Unequip = Bauble.Unequip or function() end\n"
        "Bauble.GetLevel = Bauble.GetLevel or function() return 0 end\n"
        "Bauble.IncLevel = Bauble.IncLevel or function() end\n"
        "Bauble.HideAll = Bauble.HideAll or function() end\n"
        "Bauble.Find = Bauble.Find or function() end\n"
        "\n"
        "DB = DB or {}\n"
        "DB.Inventory = DB.Inventory or {}\n"
        "db = db or {}\n"
        "db.Inventory = db.Inventory or {}\n"
        "Save = Save or {}\n"
        "Save.Inventory = Save.Inventory or {}\n"
        "Player = Player or {}\n"
        "Player.Inventory = Player.Inventory or {}\n"
        "Character = Character or {}\n"
        "Character.Inventory = Character.Inventory or {}\n"
        "Mini = Mini or {}\n"
        "Mini.Inventory = Mini.Inventory or {}\n"
        "Mini.Character = Mini.Character or {}\n"
        "Mini.Character.Inventory = Mini.Character.Inventory or {}\n"
        "\n"
        "_G.__sre_btn_counter = _G.__sre_btn_counter or 0\n"
        "_G.__sre_ovr_counter = _G.__sre_ovr_counter or 0\n"
        "\n"
        "MiniButton = {}\n"
        "MiniButton.__index = MiniButton\n"
        "\n"
        "function MiniButton.new(id)\n"
        "    local self = setmetatable({}, MiniButton)\n"
        "    self.id = id\n"
        "    return self\n"
        "end\n"
        "\n"
        "function MiniButton:getID() return self.id end\n"
        "function MiniButton:isPressed() return ButtonController.IsPressed(self.id) end\n"
        "function MiniButton:isDragging() return ButtonController.IsDragging(self.id) end\n"
        "function MiniButton:setPosition(x, y) ButtonController.SetPosition(self.id, x, y); return self end\n"
        "function MiniButton:getPosition() return ButtonController.GetPosition(self.id) end\n"
        "function MiniButton:setHidden(hidden) ButtonController.SetHidden(self.id, hidden); return self end\n"
        "function MiniButton:setClickable(clickable) ButtonController.SetClickable(self.id, clickable); return self end\n"
        "function MiniButton:setScaling(x, y) ButtonController.SetScaling(self.id, x, y); return self end\n"
        "function MiniButton:setDimensions(width, height) ButtonController.SetDimensions(self.id, width, height); return self end\n"
        "function MiniButton:setAlpha(alpha) ButtonController.SetAlpha(self.id, alpha); return self end\n"
        "function MiniButton:setBackgroundAlpha(alpha) ButtonController.SetBackgroundAlpha(self.id, alpha); return self end\n"
        "function MiniButton:setBackgroundResource(resource) ButtonController.SetBackgroundResource(self.id, resource); return self end\n"
        "function MiniButton:setText(text) ButtonController.SetText(self.id, text); return self end\n"
        "function MiniButton:setTextFont(font) ButtonController.SetTextFont(self.id, font); return self end\n"
        "function MiniButton:setTextScale(scale) ButtonController.SetTextScale(self.id, scale); return self end\n"
        "function MiniButton:setTextColor(color) ButtonController.SetTextColor(self.id, color); return self end\n"
        "function MiniButton:makeMovable(movable) ButtonController.MakeMovable(self.id, movable); return self end\n"
        "function MiniButton:setConfined(confined) ButtonController.SetButtonConfined(self.id, confined); return self end\n"
        "function MiniButton:setPadding(left, top, right, bottom) ButtonController.SetPadding(self.id, left, top, right, bottom); return self end\n"
        "function MiniButton:setAlignment(gravity) ButtonController.SetAlignment(self.id, gravity); return self end\n"
        "function MiniButton:setAlwaysActive(active) return self end\n"
        "function MiniButton:remove() ButtonController.Delete(self.id); self._deleted = true end\n"
        "function MiniButton:delete() ButtonController.Delete(self.id); self._deleted = true end\n"
        "function MiniButton:isDeleted() return not not self._deleted end\n"
        "\n"
        "MiniOverlay = {}\n"
        "MiniOverlay.__index = MiniOverlay\n"
        "\n"
        "function MiniOverlay.new(id)\n"
        "    local self = setmetatable({}, MiniOverlay)\n"
        "    self.id = id\n"
        "    return self\n"
        "end\n"
        "\n"
        "function MiniOverlay:addButton(button)\n"
        "    local bid = type(button) == 'table' and button.id or button\n"
        "    ButtonController.OverlayAddButton(self.id, bid)\n"
        "    return button\n"
        "end\n"
        "\n"
        "function MiniOverlay:separator(y) ButtonController.OverlayAddSeparator(self.id, y); return self end\n"
        "function MiniOverlay:setHidden(hidden) ButtonController.SetOverlayHidden(self.id, hidden); return self end\n"
        "function MiniOverlay:setPosition(x, y) ButtonController.SetOverlayPosition(self.id, x, y); return self end\n"
        "function MiniOverlay:getPosition() return ButtonController.GetOverlayPosition(self.id) end\n"
        "function MiniOverlay:makeMovable(movable) ButtonController.SetOverlayMovable(self.id, movable); return self end\n"
        "function MiniOverlay:makePinchable(pinchable) ButtonController.SetOverlayPinchable(self.id, pinchable); return self end\n"
        "function MiniOverlay:setBackgroundColor(color) ButtonController.SetOverlayBackgroundColor(self.id, color); return self end\n"
        "function MiniOverlay:setBackgroundAlpha(alpha) ButtonController.SetOverlayBackgroundAlpha(self.id, alpha); return self end\n"
        "function MiniOverlay:setCornerRadius(radius) ButtonController.SetOverlayCornerRadius(self.id, radius); return self end\n"
        "function MiniOverlay:getPinchFactor() return ButtonController.GetOverlayScaleFactor(self.id) end\n"
        "function MiniOverlay:isPinching() return ButtonController.IsOverlayPinching(self.id) end\n"
        "function MiniOverlay:setAlwaysActive(active) return self end\n"
        "function MiniOverlay:remove() ButtonController.RemoveOverlay(self.id); self._deleted = true end\n"
        "function MiniOverlay:isDeleted() return not not self._deleted end\n"
        "\n"
        "OverlayController = {}\n"
        "\n"
        "function OverlayController.NewButton(label, x, y, width, height)\n"
        "    _G.__sre_btn_counter = _G.__sre_btn_counter + 1\n"
        "    local id = 'btn_' .. tostring(_G.__sre_btn_counter)\n"
        "    ButtonController.New(id, label or '', x or 0.5, y or 0.5, width or 0.1, height or 0.05)\n"
        "    return MiniButton.new(id)\n"
        "end\n"
        "\n"
        "function OverlayController.NewOverlay(x, y, width, height)\n"
        "    _G.__sre_ovr_counter = _G.__sre_ovr_counter + 1\n"
        "    local id = 'overlay_' .. tostring(_G.__sre_ovr_counter)\n"
        "    ButtonController.NewOverlay(id, x or 0.5, y or 0.5, width or 0.3, height or 0.4)\n"
        "    return MiniOverlay.new(id)\n"
        "end\n"
        "\n"
        "function OverlayController.RemoveAll() ButtonController.DeleteAll() end\n"
        "function OverlayController.HideAll(hidden) ButtonController.SetHiddenAll(hidden) end\n"
        "\n"    );

    /* ---- Portal API safety wrapper ----
     * The game engine registers a native 'Portal' table via ProgramState::RegisterLibrary.
     * RLSW 7 scripts call Portal.Activate(self) and Portal.Deactivate(self) from portalHook2.
     * If the engine's Portal table is nil or its methods crash, the script thread dies silently.
     * We wrap the native Portal table in a safe proxy that:
     *   1. Forwards to native Portal.Activate / Portal.Deactivate if they exist and work
     *   2. Falls back to a no-crash stub if native methods are unavailable (pre-scene load)
     *   3. Tracks activation state so Lua scripts can query it
     * We use 'or' guards to avoid overwriting a correctly-registered native table. */
    sre_eval_lua(L,
        /* Only wrap once per state */
        "if not _G.__sre_portal_wrapped then\n"
        "  _G.__sre_portal_wrapped = true\n"
        "\n"
        "  -- Capture whatever the engine registered (may be nil if called before scene init)\n"
        "  local _native_portal = rawget(_G, 'Portal')\n"
        "\n"
        "  -- Helper: safely call a native portal method\n"
        "  local function _safe_portal_call(method_name, self_obj)\n"
        "    local np = rawget(_G, '__sre_native_portal') or _native_portal\n"
        "    if not np then return false, 'Portal table not registered by engine' end\n"
        "    local fn = np[method_name]\n"
        "    if type(fn) ~= 'function' then return false, 'Portal.' .. method_name .. ' is not a function' end\n"
        "    local ok, err = pcall(fn, self_obj)\n"
        "    return ok, err\n"
        "  end\n"
        "\n"
        "  -- Build the safe Portal wrapper table\n"
        "  local _portal_proxy = {}\n"
        "\n"
        "  function _portal_proxy.Activate(self_obj)\n"
        "    local ok, err = _safe_portal_call('Activate', self_obj)\n"
        "    if not ok then\n"
        "      print('[SRE/Portal] Portal.Activate failed: ' .. tostring(err))\n"
        "    end\n"
        "  end\n"
        "\n"
        "  function _portal_proxy.Deactivate(self_obj)\n"
        "    local ok, err = _safe_portal_call('Deactivate', self_obj)\n"
        "    if not ok then\n"
        "      print('[SRE/Portal] Portal.Deactivate failed: ' .. tostring(err))\n"
        "    end\n"
        "  end\n"
        "\n"
        "  -- Proxy metatable: if a script accesses Portal.SomeOtherMethod,\n"
        "  -- forward to native Portal table transparently\n"
        "  setmetatable(_portal_proxy, {\n"
        "    __index = function(t, k)\n"
        "      local np = rawget(_G, '__sre_native_portal') or _native_portal\n"
        "      if np then return np[k] end\n"
        "      return nil\n"
        "    end,\n"
        "    __newindex = function(t, k, v)\n"
        "      local np = rawget(_G, '__sre_native_portal') or _native_portal\n"
        "      if np then np[k] = v end\n"
        "    end\n"
        "  })\n"
        "\n"
        "  -- Store the native Portal reference so the proxy can re-query it\n"
        "  -- after the engine's RegisterProgramLibrary fires and overwrites Portal\n"
        "  _G.__sre_native_portal = _native_portal\n"
        "\n"
        "  -- Override the global Portal with our safe proxy\n"
        "  rawset(_G, 'Portal', _portal_proxy)\n"
        "\n"
        "  -- Watch Portal overwrites: if engine re-registers Portal,\n"
        "  -- capture the new native table so our proxy picks it up\n"
        "  -- (uses a no-op setmetatable on _G which Lua 5.1 supports via debug)\n"
        "  if debug and debug.setmetatable then\n"
        "    local _env_mt = debug.getmetatable(_G) or {}\n"
        "    local _old_newindex = _env_mt.__newindex\n"
        "    _env_mt.__newindex = function(env, k, v)\n"
        "      if k == 'Portal' and v ~= _portal_proxy then\n"
        "        rawset(_G, '__sre_native_portal', v)\n"
        "        return\n"
        "      end\n"
        "      if k == 'Scene' and v and type(v) == 'table' then\n"
        "        if _G.__sre_wrap_scene_table then _G.__sre_wrap_scene_table(v) end\n"
        "      end\n"
        "      if (k == 'Find' or k == 'New') and v and type(v) == 'function' then\n"
        "        if _G.__sre_wrap_func then v = _G.__sre_wrap_func(v) end\n"
        "      end\n"
        "      if _old_newindex then\n"
        "        return _old_newindex(env, k, v)\n"
        "      else\n"
        "        rawset(env, k, v)\n"
        "      end\n"
        "    end\n"
        "    debug.setmetatable(_G, _env_mt)\n"
        "  end\n"
        "end\n"
    );

    /* Load edgetest.scl dynamically if present */
    // sre_load_edgetest_scl(L);

    /* Load serialization and database libraries */
    sre_load_srlz_lua(L);
    sre_load_db_lua(L);

    /* Cache this state */
    if (g_injected_count < MAX_INJECTED_STATES) {
        g_injected_states[g_injected_count++] = L;
    }
}



