# Swordigo Desktop Host Integration & Host FBO Rendering Pipeline

## 1. System Overview

In **Native Host Mode**, OpenSwordigo (`libopensw_core.so`) runs directly inside the **Swordigo Desktop** process. The Desktop host provides native OpenGL 3.3 context management, Framebuffer Object (FBO) allocation, and VFS file streaming from memory archives.

---

## 2. Host Framebuffer Object (FBO) Render Pass Architecture

Rather than creating separate windows or contexts, the host application creates an offscreen Framebuffer Object (FBO) and passes the FBO handle to `opensw_core_render_to_fbo(ctx, host_fbo_id)`. `libopensw_render.so` renders the 3D scene directly into the host's FBO texture, allowing the host application to display the game in a native viewport, split-screen editor, or post-processing pipeline.

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Swordigo Desktop Host Application                    │
│                                                                        │
│  ┌────────────────────────┐  Render Call  ┌─────────────────────────┐  │
│  │ Create Host FBO        ├──────────────►│ libopensw_render.so     │  │
│  │ (Texture Color/Depth)  │               │ ───                     │  │
│  └───────────▲────────────┘               │ • Bind host_fbo_id      │  │
│              │                            │ • Set Viewport (W x H)  │  │
│              │ FBO Rendered Texture       │ • Render 3D Scene       │  │
│              └────────────────────────────┤ • Restore GL Context    │  │
│                                           └─────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

### Host FBO Pass Implementation Protocol

```cpp
// Host FBO Initialization
GLuint hostFBO, colorTex, depthRBO;
glGenFramebuffers(1, &hostFBO);
glBindFramebuffer(GL_FRAMEBUFFER, hostFBO);

// Color Texture Attachment
glGenTextures(1, &colorTex);
glBindTexture(GL_TEXTURE_2D, colorTex);
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1920, 1080, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

// Depth Buffer Attachment
glGenRenderbuffers(1, &depthRBO);
glBindRenderbuffer(GL_RENDERBUFFER, depthRBO);
glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1920, 1080);
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRBO);

glBindFramebuffer(GL_FRAMEBUFFER, 0);

// Host Execution Loop
while (running) {
    // 1. Tick engine logic
    opensw_core_tick(engine_ctx, delta_time);

    // 2. Render engine scene directly into host FBO
    opensw_core_render_to_fbo(engine_ctx, hostFBO);

    // 3. Host renders colorTex to screen / UI viewport
    Host_RenderViewport(colorTex);
}
```

---

## 3. Host Zero-Copy VFS Infrastructure

The host VFS streams `.scene`, `.template`, and `.PVR` binary assets directly from host memory (`assets.tar.xz`) into `libopensw_core.so` without temporary disk allocations.

```cpp
namespace caver {

class NativeVFSBridge {
public:
    static NativeVFSBridge& instance() {
        static NativeVFSBridge inst;
        return inst;
    }

    void setup(OpenSW_VFSReadFn read_fn, OpenSW_VFSFreeFn free_fn, void* user_data) {
        read_fn_ = read_fn;
        free_fn_ = free_fn;
        user_data_ = user_data;
    }

    std::vector<uint8_t> fetchAsset(const std::string& path) {
        if (!read_fn_) return {};
        uint8_t* ptr = nullptr;
        size_t sz = 0;
        if (read_fn_(path.c_str(), &ptr, &sz, user_data_) && ptr && sz > 0) {
            std::vector<uint8_t> data(ptr, ptr + sz);
            if (free_fn_) free_fn_(ptr, user_data_);
            return data;
        }
        return {};
    }

private:
    NativeVFSBridge() = default;
    OpenSW_VFSReadFn read_fn_{nullptr};
    OpenSW_VFSFreeFn free_fn_{nullptr};
    void* user_data_{nullptr};
};

} // namespace caver
```
