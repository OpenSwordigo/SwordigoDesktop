# Native Input Event Dispatching & Windowing Bridge

## 1. Executive Overview

This document specifies the native input event dispatching pipeline between the **Swordigo Desktop** host windowing system (SDL2 / Native x86_64 Linux) and `libopensw_core.so`.

---

## 2. Direct C++ Event Translation Architecture

Since the host and engine share the same native process memory space, input translation operates as a zero-copy lock-free event queue.

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Swordigo Desktop Host Application                    │
│                                                                        │
│   Native SDL_Event (SDL_KEYDOWN / SDL_KEYUP / SDL_MOUSEMOTION)        │
│                           │                                            │
│                           ▼                                            │
│         Native Input Translator (SDL2 -> OpenSW_InputEvent)            │
└───────────────────────────┬────────────────────────────────────────────┘
                            │ Direct C-ABI Function Call
                            ▼
┌────────────────────────────────────────────────────────────────────────┐
│                    libopensw_core.so (Native x86_64)                   │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ caver::InputManager Event Queue                                  │  │
│  └────────────────────────┬─────────────────────────────────────────┘  │
│                           │ Direct Vector Transformation               │
│                           ▼                                            │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ Native Player Controller & Collision Raycast Updates             │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 3. C-ABI Event Data Structures (`opensw_input.h`)

```cpp
#ifndef OPENSW_INPUT_H
#define OPENSW_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENSW_EVT_KEY_DOWN,
    OPENSW_EVT_KEY_UP,
    OPENSW_EVT_MOUSE_DOWN,
    OPENSW_EVT_MOUSE_UP,
    OPENSW_EVT_MOUSE_MOVE,
    OPENSW_EVT_GAMEPAD_AXIS
} OpenSW_EventType;

typedef enum {
    OPENSW_VK_NONE = 0,
    OPENSW_VK_MOVE_LEFT,
    OPENSW_VK_MOVE_RIGHT,
    OPENSW_VK_JUMP,
    OPENSW_VK_SWORD,
    OPENSW_VK_MAGIC_BOLT,
    OPENSW_VK_MAGIC_BOMB,
    OPENSW_VK_USE,
    OPENSW_VK_PAUSE
} OpenSW_VirtualKey;

typedef struct {
    OpenSW_EventType type;
    union {
        struct {
            OpenSW_VirtualKey key;
        } key;
        struct {
            float x_pixel;
            float y_pixel;
            int   button;
        } mouse;
        struct {
            int   axis;
            float value;
        } axis;
    } data;
} OpenSW_InputEvent;

// Direct C-ABI Event Injection
OPENSW_API void opensw_input_inject_event(OpenSW_ContextHandle ctx, const OpenSW_InputEvent* evt);

#ifdef __cplusplus
}
#endif

#endif // OPENSW_INPUT_H
```

---

## 4. Host FBO Viewport Input Raycasting

When the user clicks or touches inside the host application's viewport render target:
1. Host translates window pixel coordinates `(mouseX, mouseY)` into normalized FBO coordinates `[0.0, 1.0]`.
2. Engine maps normalized FBO coordinates to 2D scene world positions using active `Camera` orthographic bounds.
