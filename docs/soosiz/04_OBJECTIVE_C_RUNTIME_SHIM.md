# 04: Objective-C Runtime & Dylib Interceptor (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/04_OBJECTIVE_C_RUNTIME_SHIM.md`  
> **Status:** Remastered ObjC Interceptor & Dylib Symbol Bridge Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & Objective-C Binary Structure

The `SoosizHD` binary contains pre-compiled Objective-C class structures, vtables, and selector references in its `__DATA` segment:

- **`__DATA.__objc_classlist` (`0xEC3E0`):** Array of 267 Objective-C class pointers defined within the binary (`SoosizAppDelegate`, `ApplicationController`, `EAGLView`, `MusicPlayer`, `Scene`, etc.).
- **`__DATA.__objc_selrefs` (`0xCEFAC`):** Array of 1,517 selector string pointers.
- **`__DATA.__objc_const` (`0xD0EF8`):** Read-only class method tables, ivar offset values, and protocol definitions.

---

## 2. Intercepting `objc_msgSend` in ARM32 JIT

In ARM32 AAPCS calling convention:
- **`R0`:** Pointer to receiver object (`self`).
- **`R1`:** Pointer to selector string (`SEL`).
- **`R2`, `R3`, Stack:** Method arguments.

When `SoosizHD` calls `objc_msgSend`, Dynarmic traps the call and invokes our C bridge:

```cpp
// Dynarmic ARM32 Trampoline for objc_msgSend
uint32_t Bridge_objc_msgSend(uint32_t self_ptr, uint32_t sel_ptr, uint32_t r2, uint32_t r3) {
    if (!self_ptr || !sel_ptr) return 0;

    const char* sel_name = reinterpret_cast<const char*>(sel_ptr);
    
    // Read object class pointer (isa) from first 4 bytes of instance struct
    uint32_t isa = *reinterpret_cast<uint32_t*>(self_ptr);
    const char* class_name = GetObjCClassName(isa);

    // 1. Look up selector in binary's embedded class method table
    uint32_t method_fn_ptr = LookupMethodInClass(isa, sel_name);
    if (method_fn_ptr != 0) {
        // Execute decompiled ARM32 method function directly in JIT
        return ExecuteArm32Function(method_fn_ptr, self_ptr, sel_ptr, r2, r3);
    }

    // 2. Look up selector in Host Foundation C Stubs (NSAutoreleasePool, NSString, etc.)
    uint32_t stub_result = ExecuteFoundationHostStub(class_name, sel_name, self_ptr, r2, r3);
    return stub_result;
}
```

---

## 3. Foundation & System Dylib Bridge Map

| System Dylib | Intercepted Import Symbols | Host C/C++ Implementation in OpenSwordigo |
| :--- | :--- | :--- |
| **`libobjc.A.dylib`** | `objc_msgSend`, `objc_getClass`, `sel_registerName` | Mapped to `Bridge_objc_msgSend` and class reflection tables. |
| **`Foundation`** | `NSAutoreleasePool`, `NSString`, `NSUserDefaults` | Mapped to `sre_config.c` and standard C string handling. |
| **`UIKit`** | `UIApplicationMain`, `UIWindow`, `UIScreen` | Mapped to OpenSwordigo SDL2 windowing harness in `src/main.cpp`. |
| **`OpenGLES`** | `EAGLContext`, `glDrawArrays`, `glBindTexture` | Mapped to SRE OpenGL 2.1 desktop context in `src/sre/sre_init.c`. |
| **`OpenAL`** | `alcOpenDevice`, `alBufferData`, `alSourcePlay` | Mapped to SRE OpenAL audio channels in `src/sre/sre_music.c`. |
| **`libsqlite3`** | `sqlite3_open`, `sqlite3_exec`, `sqlite3_step` | Mapped to native desktop `sqlite3` library (`-lsqlite3`). |

---

## 4. Class Reflection & Ivar Offset Integrity

Because we run the original `SoosizHD` binary code directly:
1. All class ivar offsets defined in `__DATA.__objc_const` are used **unmodified** by the ARM32 instructions.
2. No struct packing issues or pointer offset drift can occur!
3. Object instances allocated in JIT memory retain exact binary layouts matching Touch Foo's original iOS build.
