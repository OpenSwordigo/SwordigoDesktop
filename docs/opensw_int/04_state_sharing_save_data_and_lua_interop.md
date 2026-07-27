# Native State Interoperability, Save Files & Lua Bridge

## 1. Executive Overview

Because **Swordigo Desktop** and `libopensw_core.so` execute as native x86_64 code in the same process space, game state sharing and Lua runtime inspection occur via direct C++ memory access.

---

## 2. Direct C++ Memory State Synchronization

```cpp
#ifndef OPENSW_STATE_H
#define OPENSW_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t level;
    uint32_t xp;
    uint32_t gems;
    uint32_t max_health;
    uint32_t current_health;
    uint32_t max_mana;
    uint32_t current_mana;
    float    player_x;
    float    player_y;
    char     active_scene[64];
} OpenSW_PlayerState;

// Direct Native Memory Inspection
OPENSW_API const OpenSW_PlayerState* opensw_state_get_player(OpenSW_ContextHandle ctx);
OPENSW_API void                      opensw_state_set_player(OpenSW_ContextHandle ctx, const OpenSW_PlayerState* state);

#ifdef __cplusplus
}
#endif

#endif // OPENSW_STATE_H
```

---

## 3. Native Lua 5.1 Runtime Integration (`libopensw_lua.so`)

Level scripts (`.scl` bytecode) are loaded and executed natively by `libopensw_lua.so`. The host can inspect global Lua variables, trigger level events, or evaluate Lua expressions directly.

```cpp
namespace caver {

class NativeLuaBridge {
public:
    static NativeLuaBridge& instance() {
        static NativeLuaBridge inst;
        return inst;
    }

    void setLuaState(lua_State* L) { L_ = L; }
    lua_State* getLuaState() const { return L_; }

    void evalString(const char* expr) {
        if (L_) luaL_dostring(L_, expr);
    }

private:
    NativeLuaBridge() = default;
    lua_State* L_{nullptr};
};

} // namespace caver
```
