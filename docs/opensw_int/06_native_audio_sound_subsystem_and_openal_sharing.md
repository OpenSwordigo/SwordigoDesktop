# Native Audio Subsystem Integration (`libopensw_audio.so`) & Device Sharing

## 1. Executive Overview

This document specifies the native audio subsystem integration between **Swordigo Desktop** and `libopensw_audio.so`. Because both the host and the OpenSwordigo engine run as native x86_64 binaries in the same process space, audio device context sharing is accomplished directly through shared OpenAL / SDL_mixer hardware contexts.

---

## 2. Shared OpenAL / SDL_mixer Device Context Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Swordigo Desktop Host Application                    │
│                                                                        │
│  ┌────────────────────────┐  Pass AL Context  ┌─────────────────────┐  │
│  │ OpenAL / SDL_Audio     ├──────────────────►│ libopensw_audio.so  │  │
│  │ Primary Device Context │                   │ ───                 │  │
│  └────────────────────────┘                   │ • 3D Sound Buffers  │  │
│                                               │ • BGM Playlist      │  │
│                                               │ • Listener Pos/Vel  │  │
│                                               └─────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

### Shared Audio Device Initialization Protocol

```cpp
#ifndef OPENSW_AUDIO_H
#define OPENSW_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void*  al_device;    // Pointer to ALCdevice (or NULL for host default)
    void*  al_context;   // Pointer to ALCcontext
    float  master_volume;
    float  sfx_volume;
    float  bgm_volume;
} OpenSW_AudioConfig;

// C-ABI Audio Entry Points
OPENSW_API bool opensw_audio_init(OpenSW_ContextHandle ctx, const OpenSW_AudioConfig* config);
OPENSW_API void opensw_audio_shutdown(OpenSW_ContextHandle ctx);
OPENSW_API void opensw_audio_update_listener(OpenSW_ContextHandle ctx, float x, float y, float z, float vx, float vy, float vz);
OPENSW_API void opensw_audio_play_sfx(OpenSW_ContextHandle ctx, const char* sfx_name, float x, float y, float pitch);
OPENSW_API void opensw_audio_play_bgm(OpenSW_ContextHandle ctx, const char* track_name, bool loop);

#ifdef __cplusplus
}
#endif

#endif // OPENSW_AUDIO_H
```

---

## 3. Spatial 3D Sound & Background Music Streaming

1. **Spatial 3D Audio**: `opensw_audio_update_listener` syncs the OpenAL listener position and velocity with the active gameplay camera every frame.
2. **BGM Playlist Manager**: Streamed tracks are decoded from host asset VFS buffers via native C++ vorbis / pcm decoders without blocking the main render thread.
