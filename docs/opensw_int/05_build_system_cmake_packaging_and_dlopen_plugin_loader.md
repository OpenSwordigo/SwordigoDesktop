# CMake Build Packaging & Native `dlopen()` Host Loader

## 1. Executive Overview

This document specifies the CMake build configuration for building OpenSwordigo as native x86_64 ELF dynamic shared libraries (`libopensw_*.so`) and implements the host-side `dlopen()` native plugin loader for **Swordigo Desktop**.

---

## 2. CMake Native Packaging Targets

```cmake
cmake_minimum_required(VERSION 3.16)
project(OpenSwordigoNativeModules VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default hidden visibility for clean export control
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Native x86_64 Shared Objects
add_library(opensw_core SHARED
    src/engine/app_runtime.cpp
    src/resource/vfs.cpp
    src/abi/opensw_core_abi.cpp
)

add_library(opensw_render SHARED
    src/graphics/rendering_context.cpp
    src/graphics/pvrtc_decoder.cpp
    src/graphics/pod_loader.cpp
    src/abi/opensw_render_abi.cpp
)

add_library(opensw_scene SHARED
    src/scene/scene.cpp
    src/scene/scene_object.cpp
    src/components/ground_mesh_component.cpp
    src/components/model_component.cpp
    src/abi/opensw_scene_abi.cpp
)

add_library(opensw_lua SHARED
    src/scripting/lua_runtime.cpp
    src/abi/opensw_lua_abi.cpp
)

target_link_libraries(opensw_scene PRIVATE opensw_core opensw_render)
```

---

## 3. Host Native `dlopen()` Plugin Loader (`OpenSWNativePlugin`)

```cpp
#include <dlfcn.h>
#include <iostream>
#include "opensw_abi.h"

class OpenSWNativePlugin {
public:
    using CreateFn = OpenSW_ContextHandle (*)(const OpenSW_NativeConfig*);
    using DestroyFn = void (*)(OpenSW_ContextHandle);
    using TickFn   = void (*)(OpenSW_ContextHandle, float);
    using RenderFBOFn = void (*)(OpenSW_ContextHandle, uint32_t);

    bool load(const char* lib_path) {
        handle_ = dlopen(lib_path, RTLD_LAZY | RTLD_LOCAL);
        if (!handle_) {
            std::cerr << "[NativePlugin] Failed to load " << lib_path << ": " << dlerror() << "\n";
            return false;
        }

        create_fn_    = (CreateFn)    dlsym(handle_, "opensw_core_create");
        destroy_fn_   = (DestroyFn)   dlsym(handle_, "opensw_core_destroy");
        tick_fn_      = (TickFn)      dlsym(handle_, "opensw_core_tick");
        render_fbo_fn_= (RenderFBOFn) dlsym(handle_, "opensw_core_render_to_fbo");

        return create_fn_ && destroy_fn_ && tick_fn_ && render_fbo_fn_;
    }

    void unload() {
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    OpenSW_ContextHandle createInstance(const OpenSW_NativeConfig* config) {
        return create_fn_ ? create_fn_(config) : nullptr;
    }

    void tickInstance(OpenSW_ContextHandle ctx, float dt) {
        if (tick_fn_ && ctx) tick_fn_(ctx, dt);
    }

    void renderInstanceToFBO(OpenSW_ContextHandle ctx, uint32_t fbo_id) {
        if (render_fbo_fn_ && ctx) render_fbo_fn_(ctx, fbo_id);
    }

private:
    void* handle_{nullptr};
    CreateFn    create_fn_{nullptr};
    DestroyFn   destroy_fn_{nullptr};
    TickFn      tick_fn_{nullptr};
    RenderFBOFn render_fbo_fn_{nullptr};
};
```
