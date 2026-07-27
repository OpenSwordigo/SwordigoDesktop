# OpenSwordigo Native x86_64 Shared Library Architecture (`libopensw_*.so`)

## 1. Executive Overview

OpenSwordigo is a cleanroom C++ rewrite of the Swordigo engine compiled natively as x86_64 ELF shared objects (`libopensw_<subsystem>.so`). Because OpenSwordigo runs natively on the host architecture (Linux x86_64), **no CPU emulation (Dynarmic / ARM translation)** or guest JIT recompiler is involved. 

The **Swordigo Desktop** host process loads `libopensw_*.so` as a native plugin subsystem, executing code at full native CPU speeds with zero emulation overhead.

---

## 2. Native Shared Object Hierarchy (No Emulation Layer)

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Swordigo Desktop Native x86_64 Host                  │
│                                                                        │
│  ┌────────────────────────┐  ┌───────────────────┐  ┌───────────────┐  │
│  │ SDL2 Window / GL Context│  │ FBO Framebuffer   │  │ Host VFS Archive│
│  └───────────┬────────────┘  └─────────┬─────────┘  └───────┬───────┘  │
└──────────────┼─────────────────────────┼────────────────────┼──────────┘
               │ Native Direct C++ Calls │ (No Dynarmic / No JIT)
               ▼                         ▼                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                    libopensw_core.so (Native x86_64)                   │
│                                                                        │
│   ┌────────────────────┐   ┌────────────────────┐   ┌───────────────┐  │
│   │ libopensw_render   │   │ libopensw_scene    │   │ libopensw_lua │  │
│   │ ───                │   │ ───                │   │ ───           │  │
│   │ • Direct FBO Pass  │   │ • GroundMesh Engine│   │ • Native Lua  │  │
│   │ • PVRTC/ETC1 Dec   │   │ • POD Model Graph  │   │ • SCL Scripts │  │
│   └────────────────────┘   └────────────────────┘   └───────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

### Module Breakdown

1. **`libopensw_core.so`**:
   - Primary native engine controller.
   - Handles frame ticking, engine lifecycle, and native event loops.
   - Exposes clean C-ABI endpoints (`opensw_core_*`).

2. **`libopensw_render.so`**:
   - OpenGL 3.3 Core rendering pipeline executing offscreen into host-managed Framebuffer Objects (FBOs).
   - Fast SIMD-accelerated PVRTC1 and ETC1 native C++ texture decompressors.
   - Material shader uniforms and transform MVP pipeline.

3. **`libopensw_scene.so`**:
   - Binary Protobuf scene stream decoder (`.scene`, `.template`).
   - Procedural `GroundMeshComponent` synthesis and cliff planar UV projection.
   - PowerVR `.POD` 3D model graph loading and transform calculations.

4. **`libopensw_lua.so`**:
   - Embedded native Lua 5.1 scripting engine.
   - Executes `.scl` level scripts and quest triggers directly on x86_64.

---

## 3. Native C-ABI Interface (`opensw_abi.h`)

```cpp
#ifndef OPENSW_ABI_H
#define OPENSW_ABI_H

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
  #define OPENSW_API __declspec(dllexport)
#else
  #define OPENSW_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct OpenSW_Context_t* OpenSW_ContextHandle;

// Host VFS File Provider
typedef bool (*OpenSW_VFSReadFn)(const char* path, uint8_t** out_buf, size_t* out_size, void* user_data);
typedef void (*OpenSW_VFSFreeFn)(uint8_t* buf, void* user_data);

// Engine Native Config
typedef struct {
    uint32_t fbo_width;
    uint32_t fbo_height;
    uint32_t target_fbo_id; // Host OpenGL Framebuffer Object ID
    void*    user_data;
    OpenSW_VFSReadFn vfs_read_cb;
    OpenSW_VFSFreeFn vfs_free_cb;
} OpenSW_NativeConfig;

// Core Lifecycle Entry Points
OPENSW_API OpenSW_ContextHandle opensw_core_create(const OpenSW_NativeConfig* config);
OPENSW_API void                 opensw_core_destroy(OpenSW_ContextHandle ctx);
OPENSW_API void                 opensw_core_tick(OpenSW_ContextHandle ctx, float dt_seconds);
OPENSW_API void                 opensw_core_render_to_fbo(OpenSW_ContextHandle ctx, uint32_t fbo_id);

#ifdef __cplusplus
}
#endif

#endif // OPENSW_ABI_H
```
