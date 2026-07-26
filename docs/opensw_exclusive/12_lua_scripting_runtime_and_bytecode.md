# Swordigo OpenSwordigo Research: Lua Scripting Runtime, Bytecode Execution & Engine Bindings

## 1. Program Script Container Schema (`Program`)

Interactive entity behaviors, level triggers, cutscenes, and quest dialogue conditions are stored as binary Lua programs or bytecode chunks inside `Program` messages.

```cpp
namespace Caver {

struct Program {
    std::string name;                     // Tag 0x1A
    std::string lua_script_text;          // Tag 0x0A ("String")
    std::vector<uint8_t> compiled_bytes;  // Tag 0x0C ("Bytes")
};

} // namespace Caver
```

---

## 2. C++ Lua 5.1 Host Integration Architecture

OpenSwordigo embeds Lua 5.1 along with **LuaFileSystem** (`lfs`) and **LuaSocket** to execute entity scripts within the C++ runtime.

```cpp
#pragma once
#include <string>
#include <iostream>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace Caver {

class ScriptEngine {
public:
    ScriptEngine() {
        m_L = luaL_newstate();
        luaL_openlibs(m_L);
        RegisterEngineBindings();
    }

    ~ScriptEngine() {
        if (m_L) lua_close(m_L);
    }

    bool ExecuteScript(const Program& program) {
        if (!program.compiled_bytes.empty()) {
            if (luaL_loadbuffer(m_L, reinterpret_cast<const char*>(program.compiled_bytes.data()), 
                                program.compiled_bytes.size(), program.name.c_str()) != 0) {
                std::cerr << "[ScriptEngine] Bytecode load error: " << lua_tostring(m_L, -1) << "\n";
                lua_pop(m_L, 1);
                return false;
            }
        } else if (!program.lua_script_text.empty()) {
            if (luaL_loadstring(m_L, program.lua_script_text.c_str()) != 0) {
                std::cerr << "[ScriptEngine] Script load error: " << lua_tostring(m_L, -1) << "\n";
                lua_pop(m_L, 1);
                return false;
            }
        } else {
            return true;
        }

        if (lua_pcall(m_L, 0, 0, 0) != 0) {
            std::cerr << "[ScriptEngine] Execution error: " << lua_tostring(m_L, -1) << "\n";
            lua_pop(m_L, 1);
            return false;
        }
        return true;
    }

private:
    lua_State* m_L = nullptr;

    void RegisterEngineBindings() {
        // Expose Caver C++ classes (SceneObject, Audio, Quests) to Lua environment
    }
};

} // namespace Caver
```
