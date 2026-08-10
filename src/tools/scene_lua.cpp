/* scene_lua.cpp — SCL Lua AI host implementation.
 *
 * Runs the REAL plaintext Lua AI programs embedded in Swordigo .scl/.scene
 * files (Program components) inside a Lua 5.1 VM — the exact scripts the
 * shipped game executes. Each keep-active AI loop (`while true … Program.Wait`)
 * becomes a coroutine resumed by the player clock; one-shot event programs
 * (OnHurt / OnKill / OnAttack / OnCollide …) are kept for Program.Execute().
 *
 * The Lua API mirrors Caver's Program/EntityController/Entity/PhysicsObject/
 * KeyframeAnimation/AnimationController/CollisionShape/Math/SoundLibrary/
 * Vector3 surface as used by the extracted scripts (see .agents/npm.md and
 * OpenSwordigo/resources/). Lua 5.1 symbols come from libfilerift.so, which
 * already embeds the same src/sre/lua sources — we only need the headers.
 */
#include "scene_lua.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "tools/pod_loader.h"
#include "tools/scene_terrain.h"
#include "platform/protobuf_reader.h"

// This tree's lua.h lacks the usual extern "C" guard, so wrap it manually:
// otherwise every lua_* call gets C++-mangled and never links against the
// C ABI exported by libfilerift.so.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace sl {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr const char* kMtProxy = "sl.proxy";
constexpr const char* kMtVec3  = "sl.vec3";

// ── host structures ───────────────────────────────────────────────────────
struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

// Entity proxy userdata: points at a PlayObject (or the hero).
struct Proxy {
    sp::Player* p = nullptr;
    int object_index = -1;   // index into p->objects (hero: -1)
    bool hero = false;
};

// One running coroutine (a keep-active AI program).
struct Script {
    std::string source;
    int thread = LUA_NOREF;    // coroutine ref in the registry
    double wait_until = -1.0;  // resume when p->clock >= wait_until
    bool keep_active = true;
    bool restart = false;
    bool finished = false;
};

struct ObjLua {
    int object_index = -1;
    std::vector<Script> scripts;          // running AI coroutines
    std::vector<std::string> event_sources; // one-shot programs (Execute)
};

struct Ctx {
    lua_State* L = nullptr;
    sp::Player* p = nullptr;
    std::vector<ObjLua> objs;
    int cur_obj = -1;    // object currently resuming (for SetKeepActive)
    int cur_script = -1;
};

Ctx* ctx_of(sp::Player& p) { return (Ctx*)p.lua_ctx; }

// One VM is active at a time in the editor — keep a global pointer so the
// static Scene.* globals (Scene.Find("hero") …) can reach the host without
// a self proxy argument.
static Ctx* g_active = nullptr;

ObjLua* find_obj(Ctx* c, int object_index) {
    for (auto& o : c->objs)
        if (o.object_index == object_index) return &o;
    return nullptr;
}

// ── userdata helpers ──────────────────────────────────────────────────────
bool is_vec3(lua_State* L, int idx) {
    if (!lua_isuserdata(L, idx)) return false;
    if (!lua_getmetatable(L, idx)) return false;
    luaL_getmetatable(L, kMtVec3);
    const bool eq = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return eq;
}

Vec3* to_vec3(lua_State* L, int idx) {
    return (Vec3*)lua_touserdata(L, idx);
}

void push_vec3(lua_State* L, float x, float y, float z) {
    Vec3* v = (Vec3*)lua_newuserdata(L, sizeof(Vec3));
    v->x = x; v->y = y; v->z = z;
    luaL_getmetatable(L, kMtVec3);
    lua_setmetatable(L, -2);
}

void push_proxy(lua_State* L, sp::Player* p, int object_index, bool hero) {
    Proxy* pr = (Proxy*)lua_newuserdata(L, sizeof(Proxy));
    pr->p = p; pr->object_index = object_index; pr->hero = hero;
    luaL_getmetatable(L, kMtProxy);
    lua_setmetatable(L, -2);
}

sp::PlayObject* proxy_obj(lua_State* L, int idx) {
    Proxy* pr = (Proxy*)lua_touserdata(L, idx);
    if (!pr || pr->hero || !pr->p) return nullptr;
    if (pr->object_index < 0 || pr->object_index >= (int)pr->p->objects.size())
        return nullptr;
    return &pr->p->objects[pr->object_index];
}

bool proxy_position(lua_State* L, int idx, float out[3]) {
    Proxy* pr = (Proxy*)lua_touserdata(L, idx);
    if (!pr) return false;
    if (pr->hero) {
        out[0] = pr->p->hiro.pos[0];
        out[1] = pr->p->hiro.pos[1];
        out[2] = pr->p->hiro.pos[2];
        return true;
    }
    sp::PlayObject* po = proxy_obj(L, idx);
    if (!po) return false;
    out[0] = po->pos[0]; out[1] = po->pos[1]; out[2] = po->pos[2];
    return true;
}

// Nearest living entity (monster or hero) to a point, skipping one object.
struct Nearest {
    int index = -1;
    float dist = 1e18f;
    bool hero = false;
};

Nearest nearest_entity(sp::Player& p, int skip_index, float sx, float sy) {
    Nearest n;
    if (p.hiro.active) {
        const float dx = sx - p.hiro.pos[0], dy = sy - p.hiro.pos[1];
        n.dist = std::sqrt(dx * dx + dy * dy);
        n.hero = true;
    }
    for (auto& po : p.objects) {
        if (po.dead || po.index == skip_index) continue;
        if (!po.lua_driven && po.kind == sp::AiKind::None) continue;
        const float dx = sx - po.pos[0], dy = sy - po.pos[1];
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < n.dist) { n.index = po.index; n.dist = d; n.hero = false; }
    }
    return n;
}

// ── Program ───────────────────────────────────────────────────────────────
int l_ProgramWait(lua_State* L) {
    // Tolerate nil/non-numeric waits (some scripts pass the result of a
    // call that may return nil); treat as a tiny tick so they don't error.
    const double t = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : 0.01;
    lua_pushnumber(L, t);
    return lua_yield(L, 1);
}

int l_ProgramPrint(lua_State* L) {
    const int n = lua_gettop(L);
    fprintf(stderr, "[Lua]");
    for (int i = 1; i <= n; ++i) {
        if (const char* s = lua_tostring(L, i)) fprintf(stderr, " %s", s);
        else if (lua_isnumber(L, i)) fprintf(stderr, " %g", lua_tonumber(L, i));
        else if (lua_isnil(L, i)) fprintf(stderr, " nil");
    }
    fprintf(stderr, "\n");
    return 0;
}

int l_ProgramSetKeepActive(lua_State* L) {
    Ctx* c = nullptr;
    if (lua_isuserdata(L, 1)) {
        Proxy* pr = (Proxy*)lua_touserdata(L, 1);
        if (pr && pr->p) c = ctx_of(*pr->p);
    }
    const bool keep = lua_toboolean(L, 2);
    if (c && c->cur_obj >= 0 && c->cur_obj < (int)c->objs.size()) {
        ObjLua& o = c->objs[c->cur_obj];
        if (c->cur_script >= 0 && c->cur_script < (int)o.scripts.size())
            o.scripts[c->cur_script].keep_active = keep;
    }
    return 0;
}

int l_ProgramExecute(lua_State* L) {
    Proxy* pr = lua_isuserdata(L, 1) ? (Proxy*)lua_touserdata(L, 1) : nullptr;
    const char* name = luaL_checkstring(L, 2);
    if (!pr || !pr->p) return 0;
    Ctx* c = ctx_of(*pr->p);
    if (!c || !c->L) return 0;
    std::string src;
    ObjLua* ol = find_obj(c, pr->object_index);
    if (ol) {
        for (const auto& e : ol->event_sources)
            if (e.find(name) != std::string::npos) { src = e; break; }
    }
    if (src.empty()) return 0;
    // Run the named program once as a one-shot coroutine with (self, ...).
    lua_State* t = lua_newthread(c->L);
    const int ref = luaL_ref(c->L, LUA_REGISTRYINDEX);
    if (luaL_loadbuffer(t, src.c_str(), src.size(), "scl:execute") == 0) {
        push_proxy(t, pr->p, pr->object_index, pr->hero);
        const int n = lua_gettop(L);
        for (int i = 3; i <= n; ++i) { lua_pushvalue(L, i); lua_xmove(L, t, 1); }
        lua_resume(t, 1 + (n > 2 ? n - 2 : 0));
    }
    luaL_unref(c->L, LUA_REGISTRYINDEX, ref);
    return 0;
}

// ── EntityController ──────────────────────────────────────────────────────
int l_EC_Target(lua_State* L) {
    Proxy* pr = lua_isuserdata(L, 1) ? (Proxy*)lua_touserdata(L, 1) : nullptr;
    if (!pr || !pr->p) { lua_pushnil(L); return 1; }
    float s[3];
    if (!proxy_position(L, 1, s)) { lua_pushnil(L); return 1; }
    const int skip = pr->hero ? -2 : pr->object_index;
    Nearest n = nearest_entity(*pr->p, skip, s[0], s[1]);
    if (n.hero) { push_proxy(L, pr->p, -1, true); return 1; }
    if (n.index >= 0) { push_proxy(L, pr->p, n.index, false); return 1; }
    lua_pushnil(L);
    return 1;
}

int l_EC_IsIdle(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushboolean(L, po && po->action_timer <= 0.0f);
    return 1;
}

int l_EC_PerformAction(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) {
        po->action_id = (int)luaL_checknumber(L, 2);
        po->action_timer = 1.1f;   // attack/action window
        po->moving = false;
    }
    return 0;
}

static void set_facing(sp::PlayObject* po, lua_State* L, int idx) {
    float dir = 1.0f;
    if (lua_isnumber(L, idx)) {
        dir = lua_tonumber(L, idx) < 0.0f ? -1.0f : 1.0f;
    } else if (is_vec3(L, idx)) {
        Vec3* v = to_vec3(L, idx);
        if (v && v->x < 0.0f) dir = -1.0f;
    }
    if (po) {
        po->dir = dir;
        po->rot = (dir < 0.0f) ? kPi : 0.0f;
    }
}

int l_EC_SetFacingDirection(lua_State* L) {
    set_facing(proxy_obj(L, 1), L, 2);
    return 0;
}

int l_EC_SetMoveDirection(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    const float speed = (float)luaL_optnumber(L, 3, 0.0);
    if (po) {
        float dir = 1.0f;
        if (lua_isnumber(L, 2)) dir = lua_tonumber(L, 2) < 0.0f ? -1.0f : 1.0f;
        else if (is_vec3(L, 2)) { Vec3* v = to_vec3(L, 2); if (v && v->x < 0.0f) dir = -1.0f; }
        po->dir = dir;
        po->move_speed = speed;
        po->vel[0] = dir * speed;
        po->vel[1] = 0.0f;
        po->moving = speed > 0.01f;
        po->rot = (dir < 0.0f) ? kPi : 0.0f;
    }
    return 0;
}

int l_EC_SetMoveSpeed(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    const float s = (float)luaL_checknumber(L, 2);
    if (po) {
        po->move_speed = s;
        if (std::fabsf(po->dir) > 0.01f) po->vel[0] = po->dir * s;
        po->moving = s > 0.01f;
    }
    return 0;
}

int l_EC_SetMovementBehavior(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->behavior = luaL_checkstring(L, 2);
    return 0;
}

int l_EC_SetAcceleration(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    const float a = (float)luaL_checknumber(L, 2);
    const bool instant = lua_toboolean(L, 3);
    if (po) {
        po->accel = a;
        if (instant && std::fabsf(po->dir) > 0.01f) po->vel[0] += po->dir * a;
    }
    return 0;
}

int l_EC_SetMoveAnimation(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->move_anim_id = (int)luaL_checknumber(L, 2);
    return 0;
}

int l_EC_DefaultMoveSpeed(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushnumber(L, po ? (double)po->default_move_speed : 60.0);
    return 1;
}

int l_EC_IsActionCancelled(lua_State* L) { lua_pushboolean(L, 0); return 1; }

int l_EC_StartSwing(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) { po->action_id = 0; po->action_timer = 0.6f; }
    return 0;
}

int l_EC_Acceleration(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushnumber(L, po ? (double)po->accel : 0.0);
    return 1;
}

// ── Entity ────────────────────────────────────────────────────────────────
int l_E_SetPhysicsEnabled(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->physics_enabled = lua_toboolean(L, 2);
    return 0;
}

int l_E_IsOnGround(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushboolean(L, po && po->grounded);
    return 1;
}

int l_E_GetFacingDirection(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushnumber(L, po ? (double)po->dir : 1.0);
    return 1;
}

int l_E_SetFacingDirection(lua_State* L) {
    set_facing(proxy_obj(L, 1), L, 2);
    return 0;
}

// ── PhysicsObject ─────────────────────────────────────────────────────────
int l_PO_SetGravityDirection(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po && is_vec3(L, 2)) {
        Vec3* v = to_vec3(L, 2);
        const float len = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
        if (len < 1e-5f) return 0;
        const float cur = std::sqrt(po->gravity[0] * po->gravity[0] +
                                    po->gravity[1] * po->gravity[1] +
                                    po->gravity[2] * po->gravity[2]);
        po->gravity[0] = v->x / len * cur;
        po->gravity[1] = v->y / len * cur;
        po->gravity[2] = v->z / len * cur;
    }
    return 0;
}

int l_PO_SetGravityMagnitude(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    const float m = (float)luaL_checknumber(L, 2);
    if (po) {
        const float len = std::sqrt(po->gravity[0] * po->gravity[0] +
                                    po->gravity[1] * po->gravity[1] +
                                    po->gravity[2] * po->gravity[2]);
        if (len < 1e-6f) {
            po->gravity[0] = 0.0f; po->gravity[1] = -m; po->gravity[2] = 0.0f;
        } else {
            const float k = m / len;
            po->gravity[0] *= k; po->gravity[1] *= k; po->gravity[2] *= k;
        }
    }
    return 0;
}

int l_PO_SetDecelerationForce(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->decel = (float)luaL_checknumber(L, 2);
    return 0;
}

// ── KeyframeAnimation ─────────────────────────────────────────────────────
int l_KA_SetCurrentTime(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->frame = (float)luaL_checknumber(L, 3);
    return 0;
}

int l_KA_SetRunning(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->anim_running = lua_toboolean(L, 3);
    return 0;
}

int l_KA_TimeToCompletion(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushnumber(L, po ? (double)po->anim_frames / 30.0 : 1.0);
    return 1;
}

int l_KA_TimeToFrame(lua_State* L) {
    const float frame = (float)luaL_checknumber(L, 3);
    lua_pushnumber(L, (double)frame / 30.0);
    return 1;
}

// ── AnimationController ───────────────────────────────────────────────────
int l_AC_BlendToAnimation(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) { po->anim_id = (int)luaL_checknumber(L, 2); po->frame = 0.0f; }
    return 0;
}

// ── CollisionShape ────────────────────────────────────────────────────────
int l_CS_SetEnabled(lua_State* L) {
    if (getenv("RUBY_LUA_DEBUG"))
        fprintf(stderr, "[Lua] CollisionShape.SetEnabled(%s, %d)\n",
                luaL_optstring(L, 2, "?"), (int)lua_toboolean(L, 3));
    return 0;   // visual-only for now (no runtime collision solvers)
}

// ── Math / Sound ──────────────────────────────────────────────────────────
int l_Math_RandomFloat(lua_State* L) {
    const float a = (float)luaL_checknumber(L, 1);
    const float b = (float)luaL_checknumber(L, 2);
    lua_pushnumber(L, a + (b - a) * ((float)rand() / (float)RAND_MAX));
    return 1;
}

int l_Math_RandomInt(lua_State* L) {
    const int a = (int)luaL_checknumber(L, 1);
    const int b = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, a + rand() % (b - a + 1));
    return 1;
}

int l_Math_Abs(lua_State* L) {
    lua_pushnumber(L, std::fabs(luaL_checknumber(L, 1)));
    return 1;
}

int l_Math_Min(lua_State* L) {
    lua_pushnumber(L, std::min(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

int l_Math_Max(lua_State* L) {
    lua_pushnumber(L, std::max(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

int l_SL_PlayEffect(lua_State* L) {
    if (getenv("RUBY_LUA_DEBUG"))
        fprintf(stderr, "[Lua] SoundLibrary.PlayEffect(%s)\n",
                luaL_optstring(L, 1, "?"));
    return 0;
}

// ── DirectionToTargetFromPosition(a, b) → normalize(a − b) ───────────────
int l_DirectionToTarget(lua_State* L) {
    if (!is_vec3(L, 1) || !is_vec3(L, 2)) { lua_pushnil(L); return 1; }
    Vec3* a = to_vec3(L, 1); Vec3* b = to_vec3(L, 2);
    const float dx = a->x - b->x, dy = a->y - b->y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-5f) push_vec3(L, 1.0f, 0.0f, 0.0f);
    else push_vec3(L, dx / len, dy / len, 0.0f);
    return 1;
}

// ── Vector3 ───────────────────────────────────────────────────────────────
int l_Vector3New(lua_State* L) {
    const float x = (float)luaL_checknumber(L, 1);
    const float y = (float)luaL_checknumber(L, 2);
    const float z = lua_gettop(L) >= 3 ? (float)luaL_checknumber(L, 3) : 0.0f;
    push_vec3(L, x, y, z);
    return 1;
}

int l_Vector3FromAngle(lua_State* L) {
    const float a = (float)luaL_checknumber(L, 1);
    push_vec3(L, std::cos(a), std::sin(a), 0.0f);
    return 1;
}

int l_V3_x(lua_State* L)   { lua_pushnumber(L, to_vec3(L, 1)->x); return 1; }
int l_V3_y(lua_State* L)   { lua_pushnumber(L, to_vec3(L, 1)->y); return 1; }
int l_V3_z(lua_State* L)   { lua_pushnumber(L, to_vec3(L, 1)->z); return 1; }

int l_V3_length(lua_State* L) {
    Vec3* v = to_vec3(L, 1);
    lua_pushnumber(L, std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z));
    return 1;
}

int l_V3_normalized(lua_State* L) {
    Vec3* v = to_vec3(L, 1);
    const float len = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len < 1e-6f) push_vec3(L, 1.0f, 0.0f, 0.0f);
    else push_vec3(L, v->x / len, v->y / len, v->z / len);
    return 1;
}

int l_V3_add(lua_State* L) {
    Vec3* a = to_vec3(L, 1); Vec3* b = to_vec3(L, 2);
    push_vec3(L, a->x + b->x, a->y + b->y, a->z + b->z); return 1;
}

int l_V3_sub(lua_State* L) {
    Vec3* a = to_vec3(L, 1); Vec3* b = to_vec3(L, 2);
    push_vec3(L, a->x - b->x, a->y - b->y, a->z - b->z); return 1;
}

int l_V3_mul(lua_State* L) {
    if (is_vec3(L, 1) && lua_isnumber(L, 2)) {
        Vec3* a = to_vec3(L, 1); const float k = (float)lua_tonumber(L, 2);
        push_vec3(L, a->x * k, a->y * k, a->z * k);
    } else {
        const float k = (float)lua_tonumber(L, 1); Vec3* b = to_vec3(L, 2);
        push_vec3(L, b->x * k, b->y * k, b->z * k);
    }
    return 1;
}

int l_V3_div(lua_State* L) {
    Vec3* a = to_vec3(L, 1); const float k = (float)luaL_checknumber(L, 2);
    push_vec3(L, k == 0.0f ? a->x : a->x / k,
              k == 0.0f ? a->y : a->y / k,
              k == 0.0f ? a->z : a->z / k);
    return 1;
}

int l_V3_unm(lua_State* L) {
    Vec3* a = to_vec3(L, 1);
    push_vec3(L, -a->x, -a->y, -a->z); return 1;
}

int l_V3_eq(lua_State* L) {
    Vec3* a = to_vec3(L, 1); Vec3* b = to_vec3(L, 2);
    lua_pushboolean(L, a && b &&
                      std::fabs(a->x - b->x) < 1e-4f &&
                      std::fabs(a->y - b->y) < 1e-4f &&
                      std::fabs(a->z - b->z) < 1e-4f);
    return 1;
}

int l_V3_tostring(lua_State* L) {
    Vec3* a = to_vec3(L, 1);
    char buf[128];
    snprintf(buf, sizeof buf, "(%g, %g, %g)", a->x, a->y, a->z);
    lua_pushstring(L, buf);
    return 1;
}

// ── self / target proxy methods ───────────────────────────────────────────
int l_P_position(lua_State* L) {
    float p[3] = {0.0f, 0.0f, 0.0f};
    if (!proxy_position(L, 1, p)) { lua_pushnil(L); return 1; }
    push_vec3(L, p[0], p[1], p[2]);
    return 1;
}

int l_P_setPosition(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) {
        if (is_vec3(L, 2)) {
            Vec3* v = to_vec3(L, 2);
            po->pos[0] = v->x; po->pos[1] = v->y; po->pos[2] = v->z;
        } else {
            po->pos[0] = (float)luaL_checknumber(L, 2);
            po->pos[1] = (float)luaL_checknumber(L, 3);
            po->pos[2] = lua_gettop(L) >= 4 ? (float)luaL_checknumber(L, 4) : 0.0f;
        }
    }
    return 0;
}

int l_P_setVelocity(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po && is_vec3(L, 2)) {
        Vec3* v = to_vec3(L, 2);
        po->vel[0] = v->x; po->vel[1] = v->y; po->vel[2] = v->z;
    }
    return 0;
}

int l_P_identifier(lua_State* L) {
    Proxy* pr = (Proxy*)lua_touserdata(L, 1);
    if (pr && pr->hero) { lua_pushstring(L, "hero"); return 1; }
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushstring(L, po ? po->name.c_str() : "");
    return 1;
}

int l_P_setAlwaysActive(lua_State* L) {
    // Scripts mark themselves always-active so the scene keeps them running
    // even off-screen. Our scheduler runs everything already — no-op.
    (void)lua_toboolean(L, 2);
    return 0;
}

int l_P_setActive(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po && !lua_toboolean(L, 2)) po->dead = true;   // deactivate → despawn
    return 0;
}

int l_P_rotation(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    lua_pushnumber(L, po ? (double)po->rot : 0.0);
    return 1;
}

int l_P_setRotation(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po && lua_isnumber(L, 2)) po->rot = (float)lua_tonumber(L, 2);
    return 0;
}

// ── Scene ─────────────────────────────────────────────────────────────────
int l_Scene_Find(lua_State* L) {
    // Static call form: Scene.Find("hero"). Name is arg 1. (A stray
    // self-proxy form is tolerated too — name then lives at arg 2.)
    const char* name = nullptr;
    if (lua_isstring(L, 1)) name = lua_tostring(L, 1);
    else name = luaL_optstring(L, 2, "");
    if (!name || !*name) { lua_pushnil(L); return 1; }
    sp::Player* p = nullptr;
    if (lua_isuserdata(L, 1)) {
        Proxy* pr = (Proxy*)lua_touserdata(L, 1);
        if (pr && pr->p) p = pr->p;
    }
    if (!p && g_active) p = g_active->p;
    if (!p) { lua_pushnil(L); return 1; }
    if (strcmp(name, "hero") == 0) {
        push_proxy(L, p, -1, true);
        return 1;
    }
    for (auto& po : p->objects) {
        if (po.name == name && !po.dead) {
            push_proxy(L, p, po.index, false);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int l_Scene_CreateObject(lua_State* L) {
    Proxy* pr = lua_isuserdata(L, 3) ? (Proxy*)lua_touserdata(L, 3) : nullptr;
    const char* id = luaL_optstring(L, 2, "");
    if (getenv("RUBY_LUA_DEBUG"))
        fprintf(stderr, "[Lua] Scene.CreateObject(%s, %s)\n",
                luaL_optstring(L, 1, "?"), id);
    sp::Player* p = pr && pr->p ? pr->p : (g_active ? g_active->p : nullptr);
    if (!p) { lua_pushnil(L); return 1; }
    // Spawn a runtime PlayObject: physics-driven (velocity/gravity), no scene
    // backing yet (invisible to the static visualizer, but a live entity that
    // other scripts can target). Auto-despawns after a few seconds.
    sp::PlayObject po;
    po.index = -1;
    po.runtime = true;
    po.dead = false;
    po.name = id;
    po.lua_driven = true;
    po.lifespan = 4.0f;
    po.physics_enabled = true;
    float pp[3] = {0.0f, 0.0f, 0.0f};
    if (pr && !pr->hero && pr->object_index >= 0 &&
        pr->object_index < (int)p->objects.size()) {
        pp[0] = p->objects[pr->object_index].pos[0];
        pp[1] = p->objects[pr->object_index].pos[1];
        pp[2] = p->objects[pr->object_index].pos[2];
    }
    po.pos[0] = pp[0]; po.pos[1] = pp[1]; po.pos[2] = pp[2];
    po.home[0] = pp[0]; po.home[1] = pp[1]; po.home[2] = pp[2];
    p->objects.push_back(std::move(po));
    push_proxy(L, p, (int)p->objects.size() - 1, false);
    return 1;
}

int l_Scene_RemoveObject(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po) po->dead = true;
    return 0;
}

int l_Scene_AddObject(lua_State* L) { return 0; }
int l_Scene_OverrideLights(lua_State* L) { return 0; }

// ── TransformController / misc globals ────────────────────────────────────
int l_TC_RotateBy(lua_State* L) {
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po && lua_isnumber(L, 2)) po->rot += (float)lua_tonumber(L, 2);
    return 0;
}

int l_TC_TranslateTo(lua_State* L) {
    // TransformController.TranslateTo(self, targetPosition, factor) — the
    // game lerps the object toward the target each call (graveyard boss-cam
    // uses 0.05 per 0.05 s step).
    sp::PlayObject* po = proxy_obj(L, 1);
    if (po && is_vec3(L, 2)) {
        Vec3* v = to_vec3(L, 2);
        const float k = std::min(1.0f, std::max(0.0f, (float)luaL_optnumber(L, 3, 0.1)));
        po->pos[0] += (v->x - po->pos[0]) * k;
        po->pos[1] += (v->y - po->pos[1]) * k;
        po->pos[2] += (v->z - po->pos[2]) * k;
    }
    return 0;
}

int l_ShowTextBubble(lua_State* L) {
    if (getenv("RUBY_LUA_DEBUG"))
        fprintf(stderr, "[Lua] ShowTextBubble(%s) — cosmetic, skipped\n",
                luaL_optstring(L, 1, "?"));
    lua_pushnil(L);
    return 1;
}

// ── API registration ──────────────────────────────────────────────────────
void register_api(lua_State* L) {
    // Vector3 metatable
    luaL_newmetatable(L, kMtVec3);
    lua_pushcfunction(L, l_V3_x);         lua_setfield(L, -2, "x");
    lua_pushcfunction(L, l_V3_y);         lua_setfield(L, -2, "y");
    lua_pushcfunction(L, l_V3_z);         lua_setfield(L, -2, "z");
    lua_pushcfunction(L, l_V3_length);    lua_setfield(L, -2, "length");
    lua_pushcfunction(L, l_V3_normalized); lua_setfield(L, -2, "normalized");
    lua_pushcfunction(L, l_V3_add);       lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, l_V3_sub);       lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, l_V3_mul);       lua_setfield(L, -2, "__mul");
    lua_pushcfunction(L, l_V3_div);       lua_setfield(L, -2, "__div");
    lua_pushcfunction(L, l_V3_unm);       lua_setfield(L, -2, "__unm");
    lua_pushcfunction(L, l_V3_eq);        lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_V3_tostring);  lua_setfield(L, -2, "__tostring");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");       // mt.__index = mt
    lua_pop(L, 1);

    // Entity proxy metatable
    luaL_newmetatable(L, kMtProxy);
    lua_pushcfunction(L, l_P_position);       lua_setfield(L, -2, "position");
    lua_pushcfunction(L, l_P_setPosition);    lua_setfield(L, -2, "setPosition");
    lua_pushcfunction(L, l_P_setVelocity);    lua_setfield(L, -2, "setVelocity");
    lua_pushcfunction(L, l_P_identifier);     lua_setfield(L, -2, "identifier");
    lua_pushcfunction(L, l_P_setAlwaysActive); lua_setfield(L, -2, "setAlwaysActive");
    lua_pushcfunction(L, l_P_setActive);      lua_setfield(L, -2, "setActive");
    lua_pushcfunction(L, l_P_rotation);       lua_setfield(L, -2, "rotation");
    lua_pushcfunction(L, l_P_setRotation);    lua_setfield(L, -2, "setRotation");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Program
    lua_newtable(L);
    lua_pushcfunction(L, l_ProgramWait);         lua_setfield(L, -2, "Wait");
    lua_pushcfunction(L, l_ProgramPrint);        lua_setfield(L, -2, "Print");
    lua_pushcfunction(L, l_ProgramSetKeepActive); lua_setfield(L, -2, "SetKeepActive");
    lua_pushcfunction(L, l_ProgramExecute);      lua_setfield(L, -2, "Execute");
    lua_setglobal(L, "Program");

    // EntityController
    lua_newtable(L);
    lua_pushcfunction(L, l_EC_Target);             lua_setfield(L, -2, "Target");
    lua_pushcfunction(L, l_EC_IsIdle);             lua_setfield(L, -2, "IsIdle");
    lua_pushcfunction(L, l_EC_PerformAction);      lua_setfield(L, -2, "PerformAction");
    lua_pushcfunction(L, l_EC_SetFacingDirection); lua_setfield(L, -2, "SetFacingDirection");
    lua_pushcfunction(L, l_EC_SetMoveDirection);   lua_setfield(L, -2, "SetMoveDirection");
    lua_pushcfunction(L, l_EC_SetMoveSpeed);       lua_setfield(L, -2, "SetMoveSpeed");
    lua_pushcfunction(L, l_EC_SetMovementBehavior); lua_setfield(L, -2, "SetMovementBehavior");
    lua_pushcfunction(L, l_EC_SetAcceleration);    lua_setfield(L, -2, "SetAcceleration");
    lua_pushcfunction(L, l_EC_SetMoveAnimation);   lua_setfield(L, -2, "SetMoveAnimation");
    lua_pushcfunction(L, l_EC_DefaultMoveSpeed);   lua_setfield(L, -2, "DefaultMoveSpeed");
    lua_pushcfunction(L, l_EC_IsActionCancelled);  lua_setfield(L, -2, "IsActionCancelled");
    lua_pushcfunction(L, l_EC_StartSwing);         lua_setfield(L, -2, "StartSwing");
    lua_pushcfunction(L, l_EC_Acceleration);       lua_setfield(L, -2, "Acceleration");
    lua_setglobal(L, "EntityController");

    // Entity
    lua_newtable(L);
    lua_pushcfunction(L, l_E_SetPhysicsEnabled);  lua_setfield(L, -2, "SetPhysicsEnabled");
    lua_pushcfunction(L, l_E_IsOnGround);         lua_setfield(L, -2, "IsOnGround");
    lua_pushcfunction(L, l_E_GetFacingDirection); lua_setfield(L, -2, "GetFacingDirection");
    lua_pushcfunction(L, l_E_SetFacingDirection); lua_setfield(L, -2, "SetFacingDirection");
    lua_setglobal(L, "Entity");

    // PhysicsObject
    lua_newtable(L);
    lua_pushcfunction(L, l_PO_SetGravityDirection);  lua_setfield(L, -2, "SetGravityDirection");
    lua_pushcfunction(L, l_PO_SetGravityMagnitude);  lua_setfield(L, -2, "SetGravityMagnitude");
    lua_pushcfunction(L, l_PO_SetDecelerationForce); lua_setfield(L, -2, "SetDecelerationForce");
    lua_setglobal(L, "PhysicsObject");

    // KeyframeAnimation
    lua_newtable(L);
    lua_pushcfunction(L, l_KA_SetCurrentTime);   lua_setfield(L, -2, "SetCurrentTime");
    lua_pushcfunction(L, l_KA_SetRunning);       lua_setfield(L, -2, "SetRunning");
    lua_pushcfunction(L, l_KA_TimeToCompletion); lua_setfield(L, -2, "TimeToCompletion");
    lua_pushcfunction(L, l_KA_TimeToFrame);      lua_setfield(L, -2, "TimeToFrame");
    lua_setglobal(L, "KeyframeAnimation");

    // AnimationController
    lua_newtable(L);
    lua_pushcfunction(L, l_AC_BlendToAnimation); lua_setfield(L, -2, "BlendToAnimation");
    lua_setglobal(L, "AnimationController");

    // CollisionShape
    lua_newtable(L);
    lua_pushcfunction(L, l_CS_SetEnabled); lua_setfield(L, -2, "SetEnabled");
    lua_setglobal(L, "CollisionShape");

    // Math
    lua_newtable(L);
    lua_pushcfunction(L, l_Math_RandomFloat); lua_setfield(L, -2, "RandomFloat");
    lua_pushcfunction(L, l_Math_RandomInt);   lua_setfield(L, -2, "RandomInt");
    lua_pushcfunction(L, l_Math_Abs);         lua_setfield(L, -2, "Abs");
    lua_pushcfunction(L, l_Math_Min);         lua_setfield(L, -2, "Min");
    lua_pushcfunction(L, l_Math_Max);         lua_setfield(L, -2, "Max");
    lua_setglobal(L, "Math");

    // SoundLibrary
    lua_newtable(L);
    lua_pushcfunction(L, l_SL_PlayEffect); lua_setfield(L, -2, "PlayEffect");
    lua_setglobal(L, "SoundLibrary");

    // Vector3
    lua_newtable(L);
    lua_pushcfunction(L, l_Vector3New);      lua_setfield(L, -2, "New");
    lua_pushcfunction(L, l_Vector3FromAngle); lua_setfield(L, -2, "FromAngle");
    lua_setglobal(L, "Vector3");

    // Scene
    lua_newtable(L);
    lua_pushcfunction(L, l_Scene_Find);         lua_setfield(L, -2, "Find");
    lua_pushcfunction(L, l_Scene_CreateObject); lua_setfield(L, -2, "CreateObject");
    lua_pushcfunction(L, l_Scene_RemoveObject); lua_setfield(L, -2, "RemoveObject");
    lua_pushcfunction(L, l_Scene_AddObject);    lua_setfield(L, -2, "AddObject");
    lua_pushcfunction(L, l_Scene_OverrideLights); lua_setfield(L, -2, "OverrideLights");
    lua_setglobal(L, "Scene");

    // TransformController
    lua_newtable(L);
    lua_pushcfunction(L, l_TC_RotateBy);    lua_setfield(L, -2, "RotateBy");
    lua_pushcfunction(L, l_TC_TranslateTo); lua_setfield(L, -2, "TranslateTo");
    lua_setglobal(L, "TransformController");

    // Helpers
    lua_pushcfunction(L, l_DirectionToTarget);
    lua_setglobal(L, "DirectionToTargetFromPosition");
    lua_pushcfunction(L, l_ShowTextBubble);
    lua_setglobal(L, "ShowTextBubble");
}

// ── program extraction ────────────────────────────────────────────────────
bool looks_like_lua(const std::string& s) {
    static const char* marks[] = {
        "self", "Program", "Entity", "function ", "local ", "while ",
        "Physics", "Animation", "CollisionShape", "SoundLibrary",
        "Keyframe", "Vector3", "PlayEffect", "Controller"};
    for (const char* m : marks)
        if (s.find(m) != std::string::npos) return true;
    return false;
}

bool is_ai_loop(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (const char ch : s)
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') t.push_back(ch);
    return t.find("whiletrue") != std::string::npos ||
           t.find("while(true") != std::string::npos ||
           t.find("while1do") != std::string::npos;
}

void collect_programs(const av::SceneObject& o,
                      std::vector<std::string>& ai_loops,
                      std::vector<std::string>& events) {
    auto push_program_bytes = [&](const std::string& raw) {
        if (raw.size() < 2) return;
        std::string src = av::scene_program_source(raw);
        if (src.empty() || !looks_like_lua(src)) return;
        if (is_ai_loop(src)) ai_loops.push_back(std::move(src));
        else events.push_back(std::move(src));
    };
    const auto& comps = o.resolved_components.empty() ? o.components
                                                      : o.resolved_components;
    for (const auto& c : comps) {
        try {
            const auto fields = av::scene_component_fields(c);
            for (const auto& f : fields) {
                if (f.wire_type != proto::WIRE_LEN || f.bytes_value.empty())
                    continue;
                const bool program_field =
                    f.class_name == "Program" ||
                    f.name.find("Program") != std::string::npos ||
                    (f.is_message && f.bytes_value.size() > 4 &&
                     looks_like_lua(f.bytes_value));
                if (program_field) push_program_bytes(f.bytes_value);
            }
        } catch (...) {}
    }
    if (!o.onload.empty()) push_program_bytes(o.onload);
}

// ── coroutine lifecycle ───────────────────────────────────────────────────
void handle_result(Ctx* c, Script& s, int r) {
    lua_State* L = c->L;
    if (r == LUA_YIELD) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.thread);
        lua_State* t = lua_tothread(L, -1);
        const double wait = lua_tonumber(t, -1);
        lua_pop(t, 1);
        lua_pop(L, 1);
        s.wait_until = c->p->clock + (wait > 0.0 ? wait : 0.0);
        s.restart = false;
    } else if (r == 0) {
        if (s.keep_active) {
            // AI loop returned (shouldn't for `while true`, but restart).
            s.restart = true;
            s.wait_until = c->p->clock + 0.05;
        } else {
            s.finished = true;
        }
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.thread);
        lua_State* t = lua_tothread(L, -1);
        const char* msg = lua_tostring(t, -1);
        fprintf(stderr, "[Lua] script error: %s\n", msg ? msg : "unknown");
        lua_pop(t, 1);
        lua_pop(L, 1);
        s.finished = true;
    }
}

void start_script(Ctx* c, int object_index, Script& s) {
    lua_State* L = c->L;
    lua_State* t = lua_newthread(L);
    s.thread = luaL_ref(L, LUA_REGISTRYINDEX);
    if (luaL_loadbuffer(t, s.source.c_str(), s.source.size(), "scl:ai") != 0) {
        fprintf(stderr, "[Lua] script compile error: %s\n",
                lua_tostring(t, -1));
        lua_pop(t, 1);
        s.finished = true;
        return;
    }
    push_proxy(t, c->p, object_index, false);
    ObjLua* ol = find_obj(c, object_index);
    c->cur_obj = ol ? (int)(ol - c->objs.data()) : -1;
    c->cur_script = ol ? (int)(&s - ol->scripts.data()) : -1;
    const int r = lua_resume(t, 1);
    handle_result(c, s, r);
    c->cur_obj = -1;
    c->cur_script = -1;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

void lua_begin(sp::Player& p, const av::SceneData& scene) {
    lua_end(p);
    if (scene.objects.empty()) return;

    struct Found {
        int obj_index;
        std::vector<std::string> loops;
        std::vector<std::string> events;
    };
    std::vector<Found> found;
    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        std::vector<std::string> loops, events;
        collect_programs(scene.objects[i], loops, events);
        if (!loops.empty()) found.push_back({i, std::move(loops), std::move(events)});
    }
    if (found.empty()) return;

    Ctx* c = new Ctx;
    c->p = &p;
    c->L = luaL_newstate();
    luaL_openlibs(c->L);
    register_api(c->L);

    namespace fs = std::filesystem;
    size_t script_count = 0;
    for (auto& f : found) {
        auto& po = p.objects[f.obj_index];
        po.lua_driven = true;
        po.default_move_speed = po.chase_speed;  // WalkSpeed-derived
        po.name = scene.objects[f.obj_index].name;
        // Probe the object's POD frame count so KeyframeAnimation timing is
        // accurate (TimeToCompletion = frames / 30 fps).
        const auto& o = scene.objects[f.obj_index];
        const std::string mname = o.mesh_name.empty() ? o.background_name
                                                      : o.mesh_name;
        if (!mname.empty() && !p.hero_dir.empty()) {
            fs::path pod(p.hero_dir + "/" + mname);
            if (!fs::is_regular_file(pod)) pod = fs::path(p.hero_dir + "/" + mname + ".POD");
            if (!fs::is_regular_file(pod)) pod = fs::path(p.hero_dir + "/" + mname + ".pod");
            if (fs::is_regular_file(pod)) {
                av::PODModel m = av::pod_load(pod.string());
                if (m.num_frames > 0) po.anim_frames = m.num_frames;
            }
        }
        ObjLua ol;
        ol.object_index = f.obj_index;
        ol.event_sources = std::move(f.events);
        for (auto& src : f.loops) {
            Script s;
            s.source = std::move(src);
            ol.scripts.push_back(std::move(s));
        }
        script_count += ol.scripts.size();
        c->objs.push_back(std::move(ol));
    }
    p.lua_ctx = c;
    g_active = c;   // must be live BEFORE first resume (scripts call Scene.*)
    for (auto& ol : c->objs)
        for (auto& s : ol.scripts)
            start_script(c, ol.object_index, s);
    if (getenv("RUBY_LUA_DEBUG"))
        fprintf(stderr, "[Lua] host started: %zu objects, %zu AI scripts\n",
                c->objs.size(), script_count);
}

void lua_tick(sp::Player& p, av::SceneData& scene, float dt) {
    Ctx* c = ctx_of(p);
    if (!c || !c->L) return;
    c->p = &p;
    lua_State* L = c->L;

    // 0) Reap dead runtime entities (Scene.CreateObject / setActive(false)).
    for (size_t i = 0; i < p.objects.size();) {
        auto& po = p.objects[i];
        if (po.runtime) {
            if (po.lifespan > 0.0f) {
                po.lifespan -= dt;
                if (po.lifespan <= 0.0f) po.dead = true;
            }
            if (po.dead) {
                p.objects.erase(p.objects.begin() + (ptrdiff_t)i);
                continue;
            }
        }
        ++i;
    }

    // 1) Resume coroutines whose wait has expired.
    for (auto& ol : c->objs) {
        for (auto& s : ol.scripts) {
            if (s.finished) continue;
            if (s.restart) {
                luaL_unref(L, LUA_REGISTRYINDEX, s.thread);
                s.thread = LUA_NOREF;
                s.restart = false;
                s.wait_until = -1.0;
                start_script(c, ol.object_index, s);
                continue;
            }
            if (s.wait_until > 0.0 && p.clock < s.wait_until) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, s.thread);
            lua_State* t = lua_tothread(L, -1);
            lua_pop(L, 1);
            c->cur_obj = (int)(&ol - c->objs.data());
            c->cur_script = (int)(&s - ol.scripts.data());
            const int r = lua_resume(t, 0);
            handle_result(c, s, r);
            c->cur_obj = -1;
            c->cur_script = -1;
        }
    }

    // 2) Engine-side behavior autopilot + physics for Lua-driven objects.
    for (auto& po : p.objects) {
        if (!po.lua_driven) continue;
        po.action_timer -= dt;
        if (po.action_timer < 0.0f) po.action_timer = 0.0f;

        if (po.behavior == "follow") {
            const Nearest n = nearest_entity(p, po.index, po.pos[0], po.pos[1]);
            if (n.dist < 3000.0f && (n.index >= 0 || n.hero)) {
                const float tx = n.hero ? p.hiro.pos[0] : p.objects[n.index].pos[0];
                const float ty = n.hero ? p.hiro.pos[1] : p.objects[n.index].pos[1];
                const float dx = tx - po.pos[0], dy = ty - po.pos[1];
                const float dl = std::sqrt(dx * dx + dy * dy);
                if (dl > 1e-3f) {
                    const float spd = po.move_speed > 0.01f ? po.move_speed
                                                            : po.default_move_speed;
                    po.vel[0] = dx / dl * spd;
                    po.vel[1] = dy / dl * spd;
                    po.dir = dx < 0.0f ? -1.0f : 1.0f;
                    po.rot = (po.dir < 0.0f) ? kPi : 0.0f;
                    po.moving = true;
                }
            }
        } else if (po.behavior == "patrol") {
            const float xmin = po.home[0] - po.patrol_range;
            const float xmax = po.home[0] + po.patrol_range;
            const float spd = po.move_speed > 0.01f ? po.move_speed
                                                    : po.default_move_speed;
            po.pos[0] += po.dir * spd * dt;
            if (po.pos[0] <= xmin) { po.pos[0] = xmin; po.dir = 1.0f; po.rot = 0.0f; }
            if (po.pos[0] >= xmax) { po.pos[0] = xmax; po.dir = -1.0f; po.rot = kPi; }
            po.vel[0] = po.dir * spd;
            po.moving = true;
        } else if (po.behavior == "freeze") {
            po.vel[0] = 0.0f;
            po.moving = false;
        }

        // Physics integration (gravity vector from PhysicsObject.*).
        if (po.physics_enabled) {
            po.vel[0] += po.gravity[0] * dt;
            po.vel[1] += po.gravity[1] * dt;
            po.vel[2] += po.gravity[2] * dt;
        }
        if (po.decel > 0.0f) {
            const float damp = std::max(0.0f, 1.0f - po.decel * dt);
            po.vel[0] *= damp; po.vel[1] *= damp; po.vel[2] *= damp;
        }
        po.pos[0] += po.vel[0] * dt;
        po.pos[1] += po.vel[1] * dt;
        po.pos[2] += po.vel[2] * dt;
        // Ground = real game-world surface nearest the entity's feet
        // (collider tops + terrain heightfield; fallback: spawn height).
        // Lua-driven walkers/followers stand on hills and slopes like the
        // real game, but never snap up onto a bridge/celing that passes
        // overhead in the same XZ cell.
        float gnd = po.home[1];
        gnd = sg::game_ground_at(p.game_world, po.pos[0], po.pos[2],
                                 po.pos[1], 80.0f, gnd);
        if (po.pos[1] <= gnd && po.vel[1] <= 0.0f) {
            po.pos[1] = gnd;
            po.vel[1] = 0.0f;
            po.grounded = true;
        } else {
            po.grounded = false;
        }

        if (po.index >= 0 && po.index < (int)scene.objects.size()) {
            auto& so = scene.objects[po.index];
            so.pos_x = po.pos[0];
            so.pos_y = po.pos[1];
            so.pos_z = po.pos[2];
            so.rot_y = po.rot;
        }
    }
}

void lua_end(sp::Player& p) {
    Ctx* c = (Ctx*)p.lua_ctx;
    p.lua_ctx = nullptr;
    if (c) {
        if (g_active == c) g_active = nullptr;
        if (c->L) lua_close(c->L);
        delete c;
    }
    for (auto& po : p.objects) {
        po.lua_driven = false;
        po.vel[0] = po.vel[1] = po.vel[2] = 0.0f;
        po.behavior.clear();
        po.action_timer = 0.0f;
        po.anim_running = true;
    }
}

bool lua_active(const sp::Player& p) {
    Ctx* c = (Ctx*)p.lua_ctx;
    return c && c->L != nullptr;
}

} // namespace sl
