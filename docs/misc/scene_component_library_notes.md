# Scene Component Library (SCL) Script Engine Research Notes

> Analysis of SCL script loading, `Caver::Program` / `Caver::ProgramState` coroutine execution, and protobuf message parsing.

---

## 1. Engine Scripting Pipeline Architecture

### 1.1 SCL Representation & Protobuf Schema (`Caver::Program`)
In the native C++ engine, scripts are represented by `Caver::Program` mapping to a Protobuf schema:
- **Field 1 (Name / String)**: Script identifier.
- **Field 2 (Source / String)**: Raw Lua source code (text format).
- **Field 3 (CompiledCode / String)**: Serialized binary Lua bytecode.

`Program::LoadIntoState` loads compiled bytecode into the `lua_State*` via `luaL_loadbuffer(L, code, len, "program")`. If only text source is present, `Program::LoadFromProtobufMessage` dynamically compiles it at load time.

### 1.2 Hierarchical Script Scheduler (`Caver::ProgramState`)
- **Root State**: Master `lua_State*` initialized during `Scene` boot with built-in libraries (`Scene`, `Character`, `Item`, `System`, `Sound`, `Music`).
- **Child Threads**: Spawns coroutine threads (`lua_newthread(parent_L)`) anchored to `LUA_REGISTRYINDEX`.
- **Coroutine Update Loop**: `ProgramState::Update(float dt)` updates timers (`Wait(sec)`), decrements `dt * speedMultiplier`, and calls `lua_resume(L, 0)` when timers reach 0.

### 1.3 Event Callbacks
- **Action Triggers (`EntityActionComponent::Perform`)**: Triggered when interacting with objects. Spawns a child `ProgramState` and runs `lua_pcall` with 1 argument (`self`).
- **Collision Triggers (`CollisionShapeComponent::Perform`)**: Physics collision callback running `onCollision(self, other, flag)` (`lua_pcall` with 3 arguments).

---

## 2. Generic Schema-Independent SCL Extractor
To bypass tag-bound parsing limitations when reading raw `.scl` protobuf files, SRE implements a wire-2 payload scanner:
1. Scan protobuf buffer for wire-2 length-delimited payloads (`wire == 2`).
2. If payload starts with `\033Lua` (`0x1B 0x4C 0x75 0x61`), extract as binary bytecode.
3. If payload is ASCII text matching Lua keywords (`function`, `local`), compile directly as Lua source text.
