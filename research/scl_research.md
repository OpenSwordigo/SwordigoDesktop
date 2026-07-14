# Scene Script Language (SCL) Loading, Execution, and Event System Research

This document details the reverse-engineered architecture of Swordigo's Scene Script Language (SCL) scripting subsystem, how it interacts with the native C++ engine, and the identified gaps in the SRE (Swordigo Remastered Engine) mini-app implementation.

---

## 1. Engine Scripting Pipeline Architecture

### A. SCL Representation and Compilation (`Caver::Program`)
In the native C++ engine, scripts are represented by the `Caver::Program` class, which maps to a Protobuf schema containing the following fields:
* **Field 1 (Name / String)**: The script's identification.
* **Field 2 (Source / String)**: Raw Lua source code (text format).
* **Field 3 (CompiledCode / String)**: Serialized binary Lua bytecode.

When a Level or Component is loaded:
1. If the protobuf contains `CompiledCode` (Field 3), `Program::LoadIntoState` resolves its pointer and length, loading it into the target `lua_State*` using `luaL_loadbuffer(L, code, len, "program")`.
2. If only `Source` (Field 2) is present, `Program::LoadFromProtobufMessage` dynamically compiles it at load time by spawning a temporary Lua state, running `luaL_loadstring`, and serializing the compiled chunk back to the program object using `lua_dump`.

### B. Hierarchical Script Scheduler (`Caver::ProgramState`)
The native engine uses a hierarchical coroutine thread manager (`Caver::ProgramState`):
* **Root State**: Created during `Scene` initialization. It initializes a new master Lua state via `luaL_newstate`, configures panic handlers, and registers the engine's built-in libraries (`Scene`, `Character`, `Item`, `System`, `Sound`, `Music`).
* **Child Threads**: When components run scripts, they call `Scene::NewProgramStateForProgram` to spawn a child thread state:
  ```cpp
  lua_pushlightuserdata(parent_L, child_ProgramState);
  lua_newthread(parent_L);
  lua_settable(parent_L, LUA_REGISTRYINDEX); // Anchor to prevent GC
  ```
* **Coroutine Update Loop**: The native update loop (`ProgramState::Update(float dt)`) handles thread execution, yields, and resumes:
  - If a script yields via `Wait(seconds)` (suspension reason = 1), `Update` decrements its timer by `dt * speedMultiplier`.
  - Once the timer drops below `0.0`, it sets suspension reason to `0` and resumes the script thread: `lua_resume(L, 0)`.
  - It recursively calls `Update` on all child states in its doubly-linked list. Child states that terminate (return status other than `LUA_YIELD` or marked aborted) are deleted and cleaned up.

### C. Event Management and Trigger Links
Game objects link to script execution through specific event callbacks:
* **Action Triggers (`EntityActionComponent::Perform`)**: Activated when a player interacts with an interactive object. It creates a child `ProgramState`, pushes the self entity (`SceneObject`) as a parameter, and runs `ProgramState::Execute(state, 1)` (which runs `lua_pcall` with 1 argument).
* **Collision Triggers (`CollisionShapeComponent::Perform`)**: Activated when physics collision occurs. It spawns a child `ProgramState`, pushes the self entity, the other colliding entity, and a boolean flag, and runs `ProgramState::Execute(state, 3)` (which maps to `lua_pcall` with 3 arguments: `function onCollision(self, other, flag)`).

---

## 2. Identified Flaws in SRE Mini-App

### A. The `_Z17` Symbol Resolution Typo (Silent Failure)
In `src/main.cpp`, the external symbol mappings are split into two tables: `sym_hooks` (for trampoline intercepts) and `lua_ext_syms` (for pointer resolution passed to SRE).
* While corrected in `sym_hooks`, the mapping in `lua_ext_syms` was incorrectly declared as:
  ```cpp
  {"lua_setmetatable", "_Z17lua_setmetatableP9lua_Statei"}
  ```
  instead of:
  ```cpp
  {"lua_setmetatable", "_Z16lua_setmetatableP9lua_Statei"}
  ```
* This causes `sre_init_lua_ext` to receive a NULL pointer for `lua_setmetatable`. Any metatable setting inside SRE's library wrapper silently fails or crashes.

### B. Rigid Protobuf Parsing (`field == 5` Assumption)
SRE hooks standard Lua file operations (`loadfile`, `dofile`) to intercept `.scl` loads. Its internal parser (`sre_scl_extract_lua`) scans the protobuf stream by assuming:
```c
if (field == 5) { /* Program message */ }
```
* **Why it fails**: Tag `5` is only valid when the `Program` is nested inside a `ProgramComponent` extension wrapper.
* **Mismatches**:
  - Raw SCL scripts (like `is.scl` or `bc.scl`) serialized directly by tool chains store `Name`, `Source`, and `CompiledCode` fields directly at the **root level** (fields 1, 2, 3), so they lack tag 5.
  - Programs nested inside `Scene` or other entities (`MonsterEntityComponent`) use completely different field tags.
* When this parsing fails, SRE passes raw binary protobuf data to `loadstring`, causing a compilation syntax error and preventing the script from running.

---

## 3. Generic Schema-Independent Extractor Design

To make SCL loading 100% robust and independent of protobuf layouts, we replace tag-bound checks with a generic wire-2 payload scanner:

1. **Iterate Protobuf Payloads**: Scan the buffer as a series of protobuf tag/value pairs.
2. **Detect Wire-2**: When a length-delimited payload (`wire == 2`) is encountered:
   - Check if the payload starts with the standard Lua precompiled bytecode signature (`\033Lua` or `0x1B 0x4C 0x75 0x61`).
   - If found, extract the exact boundary and load it immediately as bytecode.
   - If the payload is ASCII/UTF-8 text matching common Lua keywords (like `function`, `local`, `end`), treat it as Lua source and compile it.
3. This eliminates dependency on field tags, nesting structures, and message wrappers.
