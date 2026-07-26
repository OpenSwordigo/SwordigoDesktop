# SDL2 → SDL3 Migration Analysis for SwordigoDesktop

## Executive Summary

SwordigoDesktop is in an **unusually good position** for SDL3 migration. The two hardest parts of any SDL2→SDL3 port — audio system rewrite and SDL_ttf adaptation — **don't apply to this project at all**:

- **Audio**: 100% OpenAL (no SDL_mixer, no SDL audio callbacks)
- **Text rendering**: Custom 8×8 bitmap font via raw OpenGL (no SDL_ttf)

This means the migration is primarily **mechanical renaming** of event constants, gamepad functions, and window creation signatures — most of which can be automated with SDL's official migration scripts.

> [!TIP]
> **Bottom line**: Migration is estimated at **~2-4 hours of work** across 7 files. Risk is LOW. Benefits are real but not urgent.

---

## Current SDL2 Usage Audit

### Files That Touch SDL2

| File | SDL2 APIs Used | Complexity |
|------|---------------|------------|
| [display.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/display.cpp) | `SDL_Init`, `SDL_CreateWindow`, `SDL_GL_*`, `SDL_DestroyWindow`, `SDL_GL_SwapWindow`, `IMG_Load`, event polling | **MEDIUM** |
| [main.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/main.cpp) | `SDL_Init(GAMECONTROLLER)`, `SDL_GameController*`, `SDL_PollEvent`, `SDL_GetTicks`, `SDL_SetWindowFullscreen`, all input events | **HIGH** (largest file) |
| [launcher.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/launcher.cpp) | `SDL_Init`, `SDL_CreateWindow`, `SDL_GL_*`, `IMG_Init/Load/Quit`, `SDL_Delay`, event polling | **MEDIUM** |
| [input_config.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/input_config.cpp) | `SDL_SCANCODE_*`, `SDL_CONTROLLER_BUTTON_*` | **LOW** (just constants) |
| [display.h](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/display.h) | `#include <SDL2/SDL.h>` | **TRIVIAL** |
| [vulkan_backend.h](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/vulkan_backend.h) | `#include <SDL2/SDL.h>`, `<SDL2/SDL_vulkan.h>` | **TRIVIAL** |
| [vulkan_backend.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/vulkan_backend.cpp) | `SDL_Vulkan_GetInstanceExtensions`, `SDL_Vulkan_CreateSurface`, `SDL_Vulkan_GetDrawableSize` | **LOW** |

### What We **Don't** Use (Lucky Breaks)

| SDL2 Feature | Used? | SDL3 Impact |
|---|---|---|
| SDL_mixer | ❌ No | ✅ The **hardest** SDL3 migration task — doesn't apply |
| SDL_ttf | ❌ No | ✅ No font API migration needed |
| SDL Audio callbacks | ❌ No | ✅ The entirely revamped audio system — doesn't apply |
| SDL Renderer (2D) | ❌ No | ✅ No `SDL_RenderCopy` → `SDL_RenderTexture` changes |
| SDL Threading | ❌ No | ✅ No mutex/thread API changes |

### What We **Do** Use

| Feature | File Count | Migration Effort |
|---|---|---|
| Window creation (`SDL_CreateWindow`) | 3 calls | Signature change: remove x,y params |
| GL context (`SDL_GL_*`) | 2 files | Mostly unchanged, `GetDrawableSize` removed |
| Event handling (`SDL_PollEvent` + constants) | 3 files | ~40 event constants to rename |
| Gamepad API (`SDL_GameController*`) | 2 files | All functions renamed to `SDL_Gamepad*` |
| Timer (`SDL_GetTicks`) | 1 file | Return type `Uint32` → `Uint64` |
| Image loading (`IMG_Load`) | 2 files | Minor: `SDL_FreeSurface` → `SDL_DestroySurface` |
| Vulkan surface | 1 file | `SDL_Vulkan_GetDrawableSize` removed |
| Fullscreen toggle | 1 file | `SDL_WINDOW_FULLSCREEN_DESKTOP` removed |

---

## SDL3 Benefits for SwordigoDesktop

### Directly Useful Benefits

| Benefit | Impact | Details |
|---|---|---|
| **64-bit `SDL_GetTicks()`** | 🟢 | No more 49-day Uint32 overflow. Cleaner timing code. |
| **Better HiDPI** | 🟢 | Windows are DPI-aware by default. Better on 4K displays. |
| **Improved Wayland** | 🟢 | SDL3 has much better Wayland support on Linux. |
| **Modern Gamepad API** | 🟢 | Cleaner API (`SDL_Gamepad*`), better device database. |
| **Window events as top-level** | 🟢 | No more `SDL_WINDOWEVENT` + sub-event checking — cleaner code. |
| **Bool return types** | 🟢 | `SDL_Init()` returns `bool` — more readable error checks. |
| **Nanosecond timers** | 🟡 | `SDL_GetTicksNS()` and `SDL_DelayPrecise()` for frame timing. |
| **Async I/O** | 🟡 | `io_uring`/`IoRing` support — could help asset loading. |

### Future-Proofing Benefits

| Benefit | Impact | Details |
|---|---|---|
| **SDL_GPU API** | 🔮 | Modern Vulkan/Metal/D3D12 abstraction. Could replace your raw Vulkan backend someday. |
| **HDR rendering** | 🔮 | Native HDR surface support — relevant for your post-processing pipeline. |
| **SDL2 is EOL** | ⚠️ | SDL2 will stop receiving fixes. SDL3 is the future. |

### Not Relevant to Us

| Feature | Why Not |
|---|---|
| New audio stream system | We use OpenAL directly |
| SDL_GPU for rendering | We use raw OpenGL/Vulkan |
| Pen/tablet input | Not a drawing app |
| Camera API | Not used |

---

## Detailed Per-File Migration Plan

### Phase 1: Headers & Build System

#### [MODIFY] [CMakeLists.txt](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/CMakeLists.txt)

```diff
-find_package(SDL2 REQUIRED)
+find_package(SDL3 REQUIRED)

-pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
+find_package(SDL3_image REQUIRED)

-target_link_libraries(swordigo_boot SDL2::SDL2 ...)
+target_link_libraries(swordigo_boot SDL3::SDL3 ...)

-find_library(SDL2_IMAGE_LIB NAMES SDL2_image REQUIRED)
+find_library(SDL3_IMAGE_LIB NAMES SDL3_image REQUIRED)
```

#### All headers (7 files)

```diff
-#include <SDL2/SDL.h>
+#include <SDL3/SDL.h>

-#include <SDL2/SDL_image.h>
+#include <SDL3_image/SDL_image.h>

-#include <SDL2/SDL_vulkan.h>
+#include <SDL3/SDL_vulkan.h>
```

---

### Phase 2: Window & Init (display.cpp, launcher.cpp)

#### [MODIFY] [display.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/display.cpp)

```diff
 // Init check (bool return instead of int)
-if (SDL_Init(SDL_INIT_VIDEO) < 0) {
+if (!SDL_Init(SDL_INIT_VIDEO)) {

 // Window creation (x,y params removed)
-window = SDL_CreateWindow(
-    "Swordigo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
-    w, h,
-    SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
-);
+window = SDL_CreateWindow(
+    "Swordigo", w, h,
+    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
+);
+// Note: SDL_WINDOW_SHOWN removed — windows are shown by default in SDL3

 // Vulkan window similarly
-SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
+SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE

 // Surface cleanup
-SDL_FreeSurface(icon);
+SDL_DestroySurface(icon);
```

#### [MODIFY] [launcher.cpp](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/launcher.cpp)

```diff
-if (SDL_Init(SDL_INIT_VIDEO) < 0) {
+if (!SDL_Init(SDL_INIT_VIDEO)) {

-SDL_Window* win = SDL_CreateWindow("Swordigo",
-    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
-    w, h,
-    SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN);
+SDL_Window* win = SDL_CreateWindow("Swordigo", w, h,
+    SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS);

-IMG_Init(IMG_INIT_PNG);
+IMG_Init(IMG_INIT_PNG);  // Still works, signature unchanged
```

---

### Phase 3: Events (main.cpp, display.cpp, launcher.cpp)

This is the **largest change by line count**, but entirely mechanical. Every event constant gets an `SDL_EVENT_` prefix:

```diff
 // Core events
-case SDL_QUIT:
+case SDL_EVENT_QUIT:

-case SDL_WINDOWEVENT:
-    if (event.window.event == SDL_WINDOWEVENT_CLOSE) { ... }
-    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { ... }
+case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
+    { ... }
+case SDL_EVENT_WINDOW_RESIZED:
+    { ... }

 // Input events
-case SDL_KEYDOWN:
+case SDL_EVENT_KEY_DOWN:
-case SDL_KEYUP:
+case SDL_EVENT_KEY_UP:
-case SDL_MOUSEMOTION:
+case SDL_EVENT_MOUSE_MOTION:
-case SDL_MOUSEBUTTONDOWN:
+case SDL_EVENT_MOUSE_BUTTON_DOWN:
-case SDL_MOUSEBUTTONUP:
+case SDL_EVENT_MOUSE_BUTTON_UP:
-case SDL_MOUSEWHEEL:
+case SDL_EVENT_MOUSE_WHEEL:
-case SDL_TEXTINPUT:
+case SDL_EVENT_TEXT_INPUT:
-case SDL_FINGERDOWN:
+case SDL_EVENT_FINGER_DOWN:

 // Gamepad events
-case SDL_CONTROLLERDEVICEADDED:
+case SDL_EVENT_GAMEPAD_ADDED:
-case SDL_CONTROLLERDEVICEREMOVED:
+case SDL_EVENT_GAMEPAD_REMOVED:
-case SDL_CONTROLLERAXISMOTION:
+case SDL_EVENT_GAMEPAD_AXIS_MOTION:
-case SDL_CONTROLLERBUTTONDOWN:
+case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
-case SDL_CONTROLLERBUTTONUP:
+case SDL_EVENT_GAMEPAD_BUTTON_UP:
```

> [!IMPORTANT]
> **Window events are now top-level**. The old pattern of checking `event.window.event == SDL_WINDOWEVENT_*` inside a `case SDL_WINDOWEVENT:` block must be replaced with separate top-level `case` statements. This is the one structural change in event handling.

---

### Phase 4: Gamepad API (main.cpp, input_config.cpp)

```diff
 // Types
-SDL_GameController* g_gamepad = nullptr;
+SDL_Gamepad* g_gamepad = nullptr;

 // Init
-SDL_Init(SDL_INIT_GAMECONTROLLER);
+SDL_Init(SDL_INIT_GAMEPAD);

 // Functions
-SDL_IsGameController(i)
+SDL_IsGamepad(i)
-SDL_GameControllerOpen(i)
+SDL_OpenGamepad(i)
-SDL_GameControllerName(g_gamepad)
+SDL_GetGamepadName(g_gamepad)
-SDL_GameControllerClose(g_gamepad)
+SDL_CloseGamepad(g_gamepad)
-SDL_GameControllerGetJoystick(g_gamepad)
+SDL_GetGamepadJoystick(g_gamepad)
-SDL_JoystickInstanceID(...)
+SDL_GetJoystickID(...)

 // Button constants
-SDL_CONTROLLER_BUTTON_DPAD_LEFT
+SDL_GAMEPAD_BUTTON_DPAD_LEFT
-SDL_CONTROLLER_BUTTON_A
+SDL_GAMEPAD_BUTTON_SOUTH
-SDL_CONTROLLER_BUTTON_B
+SDL_GAMEPAD_BUTTON_EAST
-SDL_CONTROLLER_BUTTON_X
+SDL_GAMEPAD_BUTTON_WEST
-SDL_CONTROLLER_BUTTON_Y
+SDL_GAMEPAD_BUTTON_NORTH
-SDL_CONTROLLER_BUTTON_START
+SDL_GAMEPAD_BUTTON_START
-SDL_CONTROLLER_BUTTON_BACK
+SDL_GAMEPAD_BUTTON_BACK
-SDL_CONTROLLER_BUTTON_LEFTSHOULDER
+SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
-SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
+SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER

 // Axis constants
-SDL_CONTROLLER_AXIS_LEFTX
+SDL_GAMEPAD_AXIS_LEFTX
```

> [!NOTE]
> SDL3 renames A/B/X/Y buttons to positional names (SOUTH/EAST/WEST/NORTH) to be layout-agnostic across Xbox/PlayStation controllers. The old names still work as aliases.

---

### Phase 5: Timers & Miscellaneous (main.cpp)

```diff
 // Timer types
-Uint32 last_ticks = SDL_GetTicks();
+Uint64 last_ticks = SDL_GetTicks();

-Uint32 now_ticks = SDL_GetTicks();
+Uint64 now_ticks = SDL_GetTicks();

-Uint32 fps_last_time = SDL_GetTicks();
+Uint64 fps_last_time = SDL_GetTicks();

 // Fullscreen toggle
-SDL_WINDOW_FULLSCREEN_DESKTOP
+SDL_WINDOW_FULLSCREEN  // SDL3 merged the two modes
```

---

### Phase 6: Vulkan Backend (vulkan_backend.cpp)

```diff
 // GetDrawableSize removed
-SDL_Vulkan_GetDrawableSize(window, &w, &h);
+SDL_GetWindowSizeInPixels(window, &w, &h);

 // Vulkan surface creation — signature changed slightly
-SDL_Vulkan_CreateSurface(window, instance_, &surface_)
+SDL_Vulkan_CreateSurface(window, instance_, NULL, &surface_)
```

---

## Risk Assessment

| Risk | Severity | Mitigation |
|---|---|---|
| SDL3 packages not in distro repos | MEDIUM | Build SDL3 from source, or use sdl2-compat as bridge |
| Subtle behavior changes in event timing | LOW | Thorough testing |
| Vulkan surface API differences | LOW | Small, well-documented changes |
| Gamepad mappings database differences | LOW | SDL3 has an even larger gamepad DB |
| HiDPI coordinate changes | LOW | Test on HiDPI displays |

---

## Alternative: sdl2-compat (Zero-Effort Bridge)

> [!TIP]
> SDL provides `sdl2-compat` — a drop-in binary compatibility layer that lets SDL2 applications run on top of SDL3 **without any source code changes**. You install `sdl2-compat` as your `libSDL2`, and it translates all SDL2 calls to SDL3 internally.

**Pros**: Zero code changes, get SDL3 backend improvements immediately.
**Cons**: Still depends on SDL2 API surface, no access to new SDL3 features, adds an extra translation layer.

This could be a useful **interim step** — run on sdl2-compat first to verify SDL3 backend stability, then do the full migration at your own pace.

---

## Open Questions

> [!IMPORTANT]
> 1. **Priority**: Is this migration something you want to do soon, or is it informational for future planning?
> 2. **SDL3 availability**: Is SDL3 available in your Fedora repos (`dnf search SDL3`), or would you need to build from source?
> 3. **sdl2-compat interest**: Would you want to try sdl2-compat first as a zero-risk test before committing to a full migration?
> 4. **Vulkan backend**: Is the Vulkan backend (`ENABLE_VULKAN`) actively used, or is it experimental? This affects how much effort to put into the Vulkan migration.

---

## Recommendation

**Migration difficulty**: 🟢 **LOW** for this project (estimated 2-4 hours)
**Benefit**: 🟡 **MEDIUM** (better HiDPI, Wayland, 64-bit timers, future-proofing)
**Urgency**: 🟡 **MEDIUM** (SDL2 is no longer actively developed)

The migration is straightforward because the two hardest SDL3 changes (audio and TTF) don't affect SwordigoDesktop at all. The work is ~90% mechanical renaming. I'd recommend it as a "whenever convenient" task, not a blocker.

## Verification Plan

### Build Verification
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Runtime Verification
- Launch via `swordigo_boot` — verify window creation, GL context, icon loading
- Test gamepad hotplug (connect/disconnect controller)
- Test fullscreen toggle (F11)
- Test all keyboard/mouse input
- Verify FPS counter timing (SDL_GetTicks Uint64)
- Verify post-processing pipeline (SSAO, godrays, composite)
- Test launcher GUI
