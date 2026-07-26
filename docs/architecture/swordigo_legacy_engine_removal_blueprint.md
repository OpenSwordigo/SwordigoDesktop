# Comprehensive Research Report & Implementation Blueprint: Eliminating Legacy CPU, GPU, & Single-Core Assumptions in Swordigo via SRE

**Document Author:** Antigravity AI  
**Target Path:** `/run/media/quantumcreeper/TVPG/research/Swordigo_Legacy_Removal_Architecture_Plan.md`  
**Decompiled Source Analyzed:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoTools/GhidraDecomp src`  
**SRE Target Project:** `/home/quantumcreeper/SwordigoDesktop`

---

## 1. Executive Overview & Problem Statement

Touch Foo's *Swordigo* was original engineered for early mobile hardware (iOS / Android circa 2011–2014, PowerVR MBX/SGX GPUs, single-core ARM Cortex-A8 CPUs). As a consequence, its underlying C++ engine architecture ("Caver" / "FW Engine") contains hardcoded assumptions about:
1. **Fixed-Function OpenGL ES 1.1 Graphics State**: Heavy reliance on immediate matrix stacks (`glMatrixMode`, `glPushMatrix`, `glLoadMatrixf`), client-side RAM vertex pointer submission (`glVertexPointer`, `glDrawArrays`), and synchronous single-context state binding.
2. **Single-Threaded Main Execution Loop**: All engine subsystems—physics, AI, particle simulations, animation, GUI event handling, and rendering—are executed synchronously on a single CPU thread (`CaverShell`).
3. **Fixed Delta-Time & VSync Coupling**: Frame updates and Box2D physics ticks directly consume variable frame time without sub-step accumulators, causing physics glitches, tunneling, and game-speed acceleration on modern high-refresh-rate displays (120Hz, 144Hz, 240Hz).
4. **Blocking Synchronous Asset & Texture I/O**: Scene loading stalls the main loop while `.POD` models and textures are decompressed and uploaded synchronously to the GPU context.

Through **SRE (Swordigo Runtime Engine)**, we can hook native function symbols in `libswordigo.so`, override legacy methods, inject a modern multi-threaded job system, decouple render ticks from game logic, and upgrade the renderer to a modern VBO/VAO shader-driven pipeline.

---

## 2. Decompiled Code Analysis: Critical Legacy Spots in Ghidra

Below is the precise mapping of legacy engine code locations identified within `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoTools/GhidraDecomp src`:

| Engine Subsystem | Decompiled Source File | Legacy Functions / Symbols | Legacy Assumption / Problem Identified |
| :--- | :--- | :--- | :--- |
| **Main Engine Loop** | `platform/CaverShell.c` | `CaverShell::Update(float dt)` | Executes audio, achievements, UI frame size checks, and scene updates sequentially on a single thread. |
| **Scene Logic & AI** | `game_systems/GameSceneController.c` | `GameSceneController::Update(float dt)` | Sequentially updates hero entity, enemy AI, scene triggers, particle emitters, and camera target in one blocking loop. |
| **GL Context & State** | `render/RenderingContext.c` | `RenderingContext::RenderingContext`, `SetCurrentContext`, `StartBackgroundLoading` | Stores context in global static `DAT_007e9dd0`. Uses `BindBackgroundGLContext`/`UnbindBackgroundGLContext` with blocking `glFlush()`. |
| **Fixed-Function Matrices** | `platform/FWShell.c` | `FWShell::DrawTouches` | Direct calls to `glMatrixMode(GL_PROJECTION)`, `glPushMatrix`, `glLoadMatrixf`, `glLoadIdentity`, `glTranslatef`. |
| **Immediate Mesh Drawing** | `render/Mesh.c`, `render/GroundMeshComponent.c` | `Mesh::Render`, `GroundMeshComponent::Render` | Calls `glVertexPointer`, `glTexCoordPointer`, `glColorPointer` with raw RAM pointers (`&DAT_007e9d40`) per draw call without VBO/VAO caching. |
| **Particle Simulation** | `render/ParticleSystem.c`, `ParticleEmitterComponent.c` | `ParticleSystem::Update(float dt)` | Single-threaded particle physics & matrix transforms calculated sequentially on main CPU core. |
| **Synchronous Model IO** | `render/PODLoader.c`, `CPVRTModelPOD.c` | `PODLoader::LoadPOD` | Synchronous file read, string parsing, and mesh array allocation blocking the main thread during scene transitions. |

---

## 3. Detailed Technical Blueprint: Removing Legacy Assumptions via SRE

### 3.1 Subsystem 1: Modernizing the Renderer (GLES 1.1 Immediate -> VBO/VAO Shader Pipeline)

#### Critical Code Spot: `RenderingContext.c`, `Mesh.c`, `GroundMeshComponent.c`

#### Legacy Assumption:
The engine submits vertex attributes directly from CPU RAM pointers on every frame:
```cpp
// Legacy Ghidra Decomp: FWShell.c / Mesh.c
glVertexPointer(2, GL_FLOAT, 8, &DAT_007e9d40);
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
```

#### SRE Hook & Modernization Strategy:
1. **Hook `RenderingContext::RenderingContext` and `Mesh::Render`**:
   SRE intercepts `Mesh::Render` calls and maintains a dynamic **Vertex Buffer Object (VBO) / Vertex Array Object (VAO) Cache**.
2. **Buffer Upload & Caching**:
   - On first render of a mesh, SRE generates a GPU VBO (`glGenBuffers`) and streams vertex/index data once.
   - Replaces CPU array pointers with bound VBO offsets (`glBindBuffer(GL_ARRAY_BUFFER, vbo_id); glVertexAttribPointer(...)`).
3. **Matrix Stack Emulation via Shader Uniforms**:
   - Intercept `RenderingContext::SetProjectionMatrix` and `SetWorldMatrix`.
   - Store matrices in an SRE `UniformBufferObject` (UBO) or GLSL `mat4 u_MVP` uniform rather than `glMatrixMode`/`glLoadMatrixf`.

```cpp
// [SRE HOOK CONCEPT] Modern VBO/VAO Replacement for Mesh::Render
extern "C" void SRE_Hook_Mesh_Render(CaverMesh* mesh, RenderingContext* ctx) {
    uint32_t vbo_id = SRE_GetOrCreateVBO(mesh);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
    
    // Bind modern shader program & submit attributes
    SRE_Shader_UseProgram(ctx->active_program_id);
    SRE_Shader_SetUniformMat4("u_MVP", ctx->current_mvp_matrix);
    
    glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_SHORT, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
```

---

### 3.2 Subsystem 2: Decoupled Multi-Threaded Engine Loop & Job System

#### Critical Code Spot: `CaverShell.c`, `GameSceneController.c`, `ParticleSystem.c`

#### Legacy Assumption:
All entity logic, physics collision steps, particle emission, camera movement, and rendering are tightly coupled in `CaverShell::Update`:
```cpp
// Legacy Ghidra Decomp: CaverShell::Update
AchievementsManager::Update(achievements_mgr, dt);
AudioSystem::Update(audio_sys, dt);
GameSceneController::Update(game_scene_ctrl, dt); // Entity AI + Physics
GUIApplication::DispatchEvents(gui_app);
// Rendering immediately follows on same thread...
```

#### SRE Multi-Core Job System Architecture:

```
                  ┌────────────────────────────────────────┐
                  │          SRE Main Controller           │
                  └───────────────────┬────────────────────┘
                                      │
              ┌───────────────────────┴───────────────────────┐
              ▼                                               ▼
┌──────────────────────────┐                    ┌──────────────────────────┐
│   Logic / Update Thread  │                    │   Render Driver Thread   │
├──────────────────────────┤                    ├──────────────────────────┤
│ - Box2D Physics Step     │                    │ - Consume Command Queue  │
│ - Entity AI & Traps      │ ──(Render Cmds)──> │ - VBO/VAO State Bind     │
│ - Particle Position Sim  │   (Lock-Free SPSC) │ - Draw Call Execution    │
│ - Camera Trajectory      │                    │ - SwapBuffers (VSync)    │
└──────────────────────────┘                    └──────────────────────────┘
              │
              ├── Worker Thread 1: Parallel Particle Field Simulation (`ParticleSystem::Update`)
              ├── Worker Thread 2: Terrain Mesh Chunk Generation (`GroundMeshGenerator`)
              └── Worker Thread 3: Audio Stream & Event Processing (`AudioSystem::Update`)
```

#### SRE Hook & Modernization Strategy:
1. **Hook `CaverShell::Update`**:
   - Split execution into **Logic Update Phase** and **Render Command Generation Phase**.
2. **Parallelize Heavy Engine Systems**:
   - `ParticleSystem::Update`: Distribute particle array updates across worker threads (`std::for_each(std::execution::par, ...)`).
   - `GroundMeshGenerator::Generate`: Terrain chunk mesh creation offloaded to background threads.
3. **Lock-Free Render Command Queue (SPSC)**:
   - Logic thread writes render draw commands (mesh ID, transform matrix, material ID) into a double-buffered queue.
   - Render thread reads commands and executes OpenGL/Vulkan draw calls independently.

---

### 3.3 Subsystem 3: Variable High-Refresh-Rate Physics & Frame Decoupling

#### Critical Code Spot: `GameSceneController.c` (`Update`), `CaverShell.c`

#### Legacy Assumption:
Delta-time `param_1` is passed raw to entity movement and physics:
```cpp
void Caver::GameSceneController::Update(GameSceneController *this, float param_1) {
    // param_1 (dt) varies with frame rate (e.g. 0.016s for 60Hz, 0.007s for 144Hz)
    // Dynamic dt causes Box2D solver divergence & hero jumping glitches
}
```

#### SRE Timestep Decoupling Blueprint:
Implement a **Deterministic Fixed-Timestep Accumulator** inside SRE's hooked `GameSceneController::Update`:

```cpp
// [SRE HOOK CONCEPT] Fixed Timestep Physics Accumulator (decouples 144Hz/240Hz rendering from 60Hz physics)
static float s_accumulator = 0.0f;
static constexpr float FIXED_TIMESTEP = 1.0f / 60.0f; // Fixed 60Hz physics step

extern "C" void SRE_Hook_GameSceneController_Update(GameSceneController* self, float frame_dt) {
    // Clamp maximum frame_dt to prevent "spiral of death" during lag spikes
    if (frame_dt > 0.1f) frame_dt = 0.1f;
    
    s_accumulator += frame_dt;
    
    // Execute fixed 60Hz game logic & Box2D physics steps
    while (s_accumulator >= FIXED_TIMESTEP) {
        SRE_Orig_GameSceneController_Update(self, FIXED_TIMESTEP);
        s_accumulator -= FIXED_TIMESTEP;
    }
    
    // Alpha ratio for smooth visual interpolation between frames
    float alpha = s_accumulator / FIXED_TIMESTEP;
    SRE_InterpolateEntityTransforms(self, alpha);
}
```

---

### 3.4 Subsystem 4: Multi-Threaded Asynchronous Asset Streaming

#### Critical Code Spot: `PODLoader.c`, `TextureLibrary.c`, `RenderingContext.c`

#### Legacy Assumption:
`RenderingContext::StartBackgroundLoading` uses primitive manual thread context binding (`BindBackgroundGLContext`), which stalls the main thread on `glFlush()` during loading screens.

#### SRE Async Streaming Strategy:
1. **Hook `PODLoader::LoadPOD` and `TextureLibrary::GetTexture`**:
   - Intercept asset load requests when transitioning scenes (e.g. `lowergrove_cave1.scene`).
2. **Background Thread Asset Loader**:
   - Move binary `.POD` parsing and PNG/PVR decoding to a thread pool worker thread.
   - Upload texture data to GPU using modern `glPixelUnpackBuffer` or PBO (Pixel Buffer Objects) asynchronously.

---

## 4. Phased Implementation Roadmap for SwordigoDesktop

```
┌─────────────────────────────────────────────────────────────────────────┐
│ PHASE 1: Hooking Infrastructure & Render Pipeline Modernization         │
├─────────────────────────────────────────────────────────────────────────┤
│ - Install SRE hooks for `Mesh::Render` & `RenderingContext`.            │
│ - Convert raw vertex arrays to dynamic VBO/VAO cache.                  │
│ - Emulate GLES 1.1 matrix stack via GLSL uniforms.                     │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ PHASE 2: Physics Accumulator & Decoupled High-Refresh Support           │
├─────────────────────────────────────────────────────────────────────────┤
│ - Hook `GameSceneController::Update` & `CaverShell::Update`.            │
│ - Inject 60Hz fixed physics accumulator with transform interpolation.  │
│ - Validate smooth 144Hz / 240Hz rendering without physics glitches.     │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ PHASE 3: Multi-Core Job System & Async Asset Streaming                  │
├─────────────────────────────────────────────────────────────────────────┤
│ - Implement SPSC Render Command Queue (Decouple Update & Render).       │
│ - Parallelize particle system (`ParticleSystem::Update`) on worker pool.│
│ - Enable PBO-based multi-threaded texture loading.                      │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 5. Summary of Deliverables & Document Locations

1. **Primary Research Plan Saved**: `/run/media/quantumcreeper/TVPG/research/Swordigo_Legacy_Removal_Architecture_Plan.md`
2. **Workspace Copy Saved**: `/home/quantumcreeper/SwordigoDesktop/research/Swordigo_Legacy_Removal_Architecture_Plan.md`
3. **Ready for SRE Implementation**: Detailed symbol mapping and hook code blueprints established for all 4 critical legacy areas.
