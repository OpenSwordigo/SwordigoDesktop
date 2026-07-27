# 02: Boot Path & Lifecycle Mapping (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/02_BOOT_PATH_AND_LIFECYCLE_MAPPING.md`  
> **Status:** Remastered Direct Binary Boot Execution Specification

---

## 1. Mach-O Binary Entry Point & Initial Register State

When OpenSwordigo's Mach-O loader launches **`SoosizHD`**:

1. **Memory Allocation:** Allocate virtual memory pages for `__TEXT` (`0x1000`), `__DATA` (`0xC8000`), and stack (`0x70000000` - 1 MB).
2. **Copy Segments:** Read raw segment data from `SoosizHD` binary file into target memory addresses.
3. **Register Context Setup:**
   - `PC` (Program Counter) = `0x00002100` (Address of `start` function).
   - `SP` (Stack Pointer) = `0x700FF000` (Top of allocated stack).
   - `R0` = `argc` (1).
   - `R1` = `argv` (Pointer to string array `{"SoosizHD", NULL}`).
4. **Start JIT Execution:** Pass register state to Dynarmic ARM32 JIT engine.

---

## 2. Binary Startup Routine & Execution Flow

```
Binary Memory Offset                Execution Step
--------------------                --------------
0x00002100  start()                 Entry point routine. Initializes environment.
     |
0x0000212C  main()                  Allocates NSAutoreleasePool; calls UIApplicationMain.
     |
[Symbol Intercept]  UIApplicationMain()  Intercepted by Loader Harness.
     |
0x0000323C  SoosizAppDelegate       - application:didFinishLaunchingWithOptions:
     |                              - Allocates ApplicationController instance.
     |
0x00004ED4  ApplicationController   - viewDidLoad -> Allocates ApplicationView (EAGLView).
     |                              - Prepares control overlays (controlOverlay_normal.pb).
     |
0x000051EC  updateThreadMain:       - Spawns physics/game logic update loop.
     |
0x00002474  EAGLView drawView       - CADisplayLink / Timer calls render pass.
```

---

## 3. `UIApplicationMain` Symbol Trap & Harness Hook

In `SoosizHD`, `main()` calls `UIApplicationMain(argc, argv, nil, nil)`. In a standard iOS environment, `UIApplicationMain` blocks forever running the iOS Cocoa event loop.

On OpenSwordigo Desktop, our dylib symbol resolver hooks the `UIApplicationMain` import symbol stub in `__DATA.__nl_symbol_ptr`:

```cpp
// Mach-O Symbol Trap Hook in OpenSwordigo Desktop
int Hook_UIApplicationMain(int argc, char *argv[], void *principalClassName, void *delegateClassName) {
    printf("[Soosiz Harness] Intercepted UIApplicationMain. Initializing Desktop Harness...\n");

    // 1. Manually instantiate SoosizAppDelegate class via ObjC Shim
    id appDelegate = objc_msgSend((id)objc_getClass("SoosizAppDelegate"), sel_registerName("alloc"));
    appDelegate = objc_msgSend(appDelegate, sel_registerName("init"));

    // 2. Trigger didFinishLaunching
    objc_msgSend(appDelegate, sel_registerName("application:didFinishLaunchingWithOptions:"), NULL, NULL);

    // 3. Hand control over to OpenSwordigo SRE Frame Loop
    while (!sre_should_quit()) {
        sre_poll_events();
        
        // Execute 1 tick of game logic in binary
        Soosiz_StepBinaryLogic(1.0f / 60.0f);
        
        // Execute 1 frame of rendering in binary
        Soosiz_RenderBinaryFrame();
        
        sre_swap_buffers();
    }

    return 0;
}
```

---

## 4. Single-Threaded Desktop Synchronization

The original iOS binary spawns a separate `updateThreadMain:` background thread via `NSThread`.

To ensure 100% thread safety on desktop GPU drivers:
- The loader harness traps `NSThread detachNewThreadSelector:toTarget:withObject:` and queues `updateThreadMain:` to run deterministically on the main thread pump right before `drawView` is invoked.
- Eliminates race conditions, mutex contention, and frame tearing on desktop Linux.
