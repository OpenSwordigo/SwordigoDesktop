# 🔬 SwordigoDesktop — GPU & Threading Architecture Research

## 1. Multi-Threading Analysis

### Current State: Everything is Stubbed

| Primitive | Behavior | Risk |
|-----------|----------|------|
| `pthread_create` | Returns 0, **thread function never called** | Thread work is silently dropped |
| `pthread_mutex_*` | All return 0 (no-ops) | No real synchronization |
| `pthread_cond_wait` | Returns immediately (doesn't block) | Spin loops possible |
| `pthread_cond_signal/broadcast` | No-op | Wakeups silently lost |
| `pthread_self` | Returns fake ID `1` | Only works for single thread |
| `nanosleep` | Returns immediately | Timing-sensitive code breaks |

### What's Properly Implemented
- ✅ `pthread_once` — Uses `redirect_pc` to chain into init_routine (this is what causes the 1-sec pause!)
- ✅ `pthread_key_*` — Real TLS via `g_tls_values` map (works only in single-thread mode)

### What Threads Are Used For
The game works fully with stubbed threads → threads are **optimization only, not correctness**:
1. **Audio** — Already handled natively by host OpenAL
2. **Asset loading** — Already handled by `io_thread.cpp` 
3. **Lazy init** — `pthread_once` (the only critical one, already working)

### Recommended Implementation Path

| Approach | Complexity | Time | Recommended |
|----------|------------|------|-------------|
| **Option D: Inline Execution** — `pthread_create` immediately calls thread func | Very Low | 1-2 hours | ✅ **Start here** |
| **Option C: Cooperative Scheduler** — Time-sliced single-thread | Medium | 1.5-2 weeks | 🔶 If needed |
| **Option A: Real Host Threads** — Separate Unicorn per thread | Very High | 3-4 weeks | ❌ Overkill |

---

## 2. GPU Architecture Analysis

### Current Rendering Pipeline

```
Game (GLES 1.x) → Bridge → Desktop OpenGL → FBO → PostFX Pipeline → Screen
                                                  ↓
                                     SSAO → God Rays → Bloom → Composite → Color FX → Upscale
```

### Vulkan Backend Status: **~80% Complete**

> [!IMPORTANT]
> A 2097-line Vulkan backend already exists! Key gaps: ARM64 integration, compressed textures, swapchain resize.

| Feature | Status |
|---------|--------|
| Instance/device/swapchain | ✅ |
| VMA memory allocator | ✅ |
| Uber-shader with specialization constants | ✅ |
| Pipeline caching | ✅ |
| draw_arrays / draw_elements | ✅ |
| Matrix stack emulation | ✅ |
| Texture management | ✅ |
| ARM64 bridge integration | ❌ |
| Compressed textures (ETC/PVRTC) | ❌ |
| Swapchain resize | ❌ |
| PostFX pipeline | ❌ |

### GPU Modernization Roadmap

#### 🟢 Tier 1: Quick Wins (1-2 weeks each)

| Improvement | Impact | Time |
|-------------|--------|------|
| Upgrade GLSL from `#version 120` → `#version 330` | Unlocks compute, SSBO | 2-3 hours |
| FSR 1.0 spatial upscaling | Sharper output | 3-5 days |
| Draw call batching | -30% CPU overhead | 1-2 weeks |

#### 🟡 Tier 2: Medium (2-4 weeks each)

| Improvement | Impact | Time |
|-------------|--------|------|
| Compute shader PostFX | 20-40% faster effects | 2-3 weeks |
| Vulkan backend completion | Full Vulkan path | 2-3 weeks |
| Async compute for PostFX | Hide PostFX latency | 3 weeks |

#### 🔴 Tier 3: Advanced (4+ weeks)

| Improvement | Impact | Time |
|-------------|--------|------|
| GPU-driven rendering | Minimal CPU overhead | 4+ weeks |
| FSR 2.0 temporal upscaling | Dramatic quality | 6+ weeks |
| Vulkan PostFX pipeline | Full Vulkan-only path | 3-4 weeks |

### Bolt Light Emission — Feasibility

> [!TIP]
> **Yes, we can make bolts emit light!** The approach: detect when the game draws bolt particles (by intercepting `glDrawArrays` calls with bright vertex colors or specific textures), then update the god ray sun position dynamically to follow the bolt, creating a "bolt glow" effect.

**Implementation options:**
1. **PostFX-based**: Track bolt screen position → add a secondary light source in the god rays shader
2. **Vertex color detection**: When `glVertexPointer` + `glColorPointer` have bright values (bolt particles), record position
3. **Additive bloom**: Already partially handled by the new bloom system — bright bolt pixels will naturally glow!

---

## 3. Key Files Reference

| File | Lines | Purpose |
|------|-------|---------|
| [jni_bridge_arm64.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/jni/jni_bridge_arm64.cpp) | 5855 | ARM64 bridge |
| [vulkan_backend.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/vulkan_backend.cpp) | 2097 | Vulkan GLES1 emulator |
| [fbo_scaler.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/fbo_scaler.cpp) | 943 | PostFX pipeline |
| [emulator_arm64.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/emulator_arm64.cpp) | 284 | Unicorn ARM64 |
