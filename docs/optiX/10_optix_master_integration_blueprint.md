# OptiX Technical Specification 10: Master Engine Architecture & Integration Roadmap

## 1. Executive Overview

This master specification synthesizes the 9 precedingOptiX research documents into a unified, phase-by-phase engineering blueprint for the **SRE OptiX Framework** inside Swordigo Desktop.

OptiX transforms the emulated mobile Touch Foo Caver Engine into a high-performance, modern desktop engine by combining:
1. **Yuzu-Style CPU Acceleration**: Fastmem memory mapping and unsafe Dynarmic A64 JIT optimizations.
2. **Multi-Threaded Rendering Architecture**: Lock-free SPSC command queues decoupling simulation from host render submission.
3. **Modern VBO/VAO Geometry Pipeline**: Zero-copy persistent coherent buffers and instanced drawing.
4. **Deferred PBR & High-Fidelity Shaders**: Multi-attachment G-Buffer, Cook-Torrance BRDF, real-time Screen Space Reflections (SSR), and Cascaded Shadow Maps (CSM).

---

## 2. OptiX System Architecture Blueprint

```
+---------------------------------------------------------------------------------------+
|  Host Process (SwordigoDesktop x86_64)                                                |
|                                                                                       |
|  +-----------------------------------+     +---------------------------------------+  |
|  |  Dynarmic A64 JIT Engine          |     |  Multi-Threaded Worker Pool           |  |
|  |  - Fastmem Direct Pointer (mmap)  |     |  - Worker 1: Particle SIMD Updates    |  |
|  |  - Unsafe FMA & Inaccurate NaN    |     |  - Worker 2: Heightmap Mesh Builder   |  |
|  |  - 512MB Code Cache Allocation    |     |  - Worker 3: Async Shader Compiler    |  |
|  +-----------------------------------+     +---------------------------------------+  |
|                    |                                           |                      |
|                    +--------------------+----------------------+                      |
|                                         | Lock-Free SPSC Ring Buffer                  |
|                                         v                                             |
|  +---------------------------------------------------------------------------------+  |
|  |  OptiX Renderer Core (Host OpenGL 3.3 Core / Vulkan)                           |  |
|  |                                                                                 |  |
|  |  Phase 1: G-Buffer Geometry Pass ----------> Output 4 Color/Depth Attachments  |  |
|  |  Phase 2: Cascaded Shadow Depth Pass ------> 4-Layer Directional Texture Array   |  |
|  |  Phase 3: Deferred Cook-Torrance PBR ------> Evaluate GGX Specular & Ambient    |  |
|  |  Phase 4: Screen Space Reflection Pass ---> Ray-March G-Buffer Color/Normal     |  |
|  |  Phase 5: FBO Viewport Scaler -------------> AMD FSR 1.0 / Sharp Bilinear 1080p   |  |
|  +---------------------------------------------------------------------------------+  |
+---------------------------------------------------------------------------------------+
```

---

## 3. Phase-by-Phase Integration Roadmap

### Phase 1: JIT & Memory Foundation (Specifications 01 & 08)
- Configure `fastmem_pointer` in `emulator_dynarmic64.cpp` to map guest memory 1:1.
- Register `SIGSEGV` fault recovery handler in `main.cpp`.
- Enable Dynarmic unsafe flags (`Unsafe_UnfuseFMA`, `Unsafe_InaccurateNaN`, `Unsafe_IgnoreGlobalMonitor`).
- Allocate persistent coherent VBO ring buffer (16MB) with `GL_MAP_PERSISTENT_BIT`.

### Phase 2: Lock-Free Queues & Geometry Modernization (Specifications 02 & 03)
- Implement `LockFreeSPSCQueue` in `io_thread.cpp`.
- Intercept `RenderingContext::DrawArrays` and `Mesh::Render` to route raw CPU arrays to the persistent VBO.
- Enable geometry instancing (`glDrawElementsInstanced`) for particle emitters.
- Offload `ParticleSystem::Update` and `GroundMeshGenerator::GenerateMesh` to background worker threads.

### Phase 3: Graphics Hooking & Deferred G-Buffer (Specifications 04 & 06)
- Install 16-byte ARM64 SRE trampolines for 10 core graphics functions in `sre_init.c`.
- Construct 4-attachment G-Buffer FBO (`gAlbedoRoughness`, `gNormalMetallic`, `gEmissionAO`, Depth).
- Intercept `MeshMaterial::Bind` to inject Cook-Torrance PBR roughness and metallic parameters into shader uniforms.
- Implement deferred lighting pass in `fbo_scaler.cpp`.

### Phase 4: SSR, CSM & Async Shader Compiler (Specifications 05, 07 & 09)
- Hook `WaterMeshComponent::Render` and `BackgroundComponent::Render` to tag reflective surfaces in G-Buffer.
- Implement ray-marched Screen Space Reflection (SSR) shader pass.
- Replace legacy CPU stencil shadow volumes with 4-cascade Cascaded Shadow Maps (CSM) and 16-sample PCF.
- Enable `AsyncShaderCompiler` background worker pool for zero-stutter material compilation.

---

## 4. Master Specification Index

| Doc # | Specification File | Key Focus Area |
| :--- | :--- | :--- |
| **01** | [`01_yuzu_fastmem_and_dynarmic_opt.md`](01_yuzu_fastmem_and_dynarmic_opt.md) | Dynarmic Fastmem & Signal Fault Handling |
| **02** | [`02_multithreaded_job_system_queue.md`](02_multithreaded_job_system_queue.md) | Lock-Free SPSC Command Buffer Queue |
| **03** | [`03_gles1_to_modern_vbo_vao_batching.md`](03_gles1_to_modern_vbo_vao_batching.md) | Fixed-Function GLES 1.1 VBO/VAO Modernization |
| **04** | [`04_pbr_pipeline_gbuffer_injection.md`](04_pbr_pipeline_gbuffer_injection.md) | Deferred G-Buffer & Cook-Torrance PBR |
| **05** | [`05_screen_space_reflections_ssr.md`](05_screen_space_reflections_ssr.md) | Real-Time Screen Space Reflections (SSR) |
| **06** | [`06_graphic_hooks_material_interception.md`](06_graphic_hooks_material_interception.md) | Complete Engine Graphics Hooking Address Inventory |
| **07** | [`07_dynamic_shadow_maps_cascaded_shadows.md`](07_dynamic_shadow_maps_cascaded_shadows.md) | 4-Cascade CSM & PCF Soft Shadows |
| **08** | [`08_memory_layout_zero_copy_buffers.md`](08_memory_layout_zero_copy_buffers.md) | Persistent Coherent Zero-Copy Mapping |
| **09** | [`09_async_shader_pipeline_compiler.md`](09_async_shader_pipeline_compiler.md) | Background Async Shader Compilation Pool |
| **10** | [`10_optix_master_integration_blueprint.md`](10_optix_master_integration_blueprint.md) | Master System Architecture & Roadmap |
