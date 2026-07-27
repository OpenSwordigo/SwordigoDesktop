# 11: Boot Loop Taxonomy & Diagnostics (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/11_BOOT_LOOP_TAXONOMY_AND_CRASH_DIAGNOSTICS.md`  
> **Status:** Remastered Binary Diagnostics & Crash Recovery Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & Binary Crash Protection

When executing the native **`SoosizHD`** binary inside a Dynarmic ARM32 JIT environment, crash protection must capture unresolved dyld symbol calls, invalid memory reads/writes outside mapped segments, and unhandled system traps.

---

## 2. Failure Taxonomy & Recovery Matrix

| Failure Category | Failure Cause | Automated Recovery Strategy |
| :--- | :--- | :--- |
| **1. Unresolved Dylib Symbol** | JIT hits unhooked symbol stub in `__TEXT.__symbolstub1`. | Symbol resolver returns dummy stub returning `0` in `R0` and logs warning to `soosiz_boot.log`. |
| **2. Unhandled ObjC Selector** | Binary invokes missing selector on Cocoa/GameCenter class. | `objc_msgSend` trap logs missing selector name and returns `NULL` without crashing JIT. |
| **3. Invalid Memory Address** | Binary attempts read/write outside mapped `__TEXT` or `__DATA`. | Dynarmic `MemoryRead32` callback catches out-of-bounds access, allocates dynamic zero-page, and logs warning. |
| **4. Thread Lock / Deadlock** | Binary spawns `NSThread` waiting on main thread event. | `NSThread` creation intercepted; update loop merged into single-threaded main loop pump. |

---

## 3. Automated Diagnostics Logger (`soosiz_boot.log`)

OpenSwordigo logs all Mach-O loader, symbol interceptor, and JIT events to:  
`~/.config/openswordigo/soosiz_boot.log`

```
[00:00:00.001] [INFO] OpenSwordigo Mach-O Loader Initialized.
[00:00:00.005] [INFO] Opening Binary: /home/quantumcreeper/SwordigoDesktop/OpenSoozis/SoosizHD
[00:00:00.010] [INFO] Mach-O Header Verified: Magic=0xFEEDFACE, CPU=ARMv7 (32-bit).
[00:00:00.020] [INFO] Mapped Segment __TEXT: 0x1000 - 0xC8000 (RX)
[00:00:00.025] [INFO] Mapped Segment __DATA: 0xC8000 - 0xEE000 (RW)
[00:00:00.040] [INFO] Bound 14 System Dylibs (UIKit, Foundation, OpenGLES, OpenAL, SQLite).
[00:00:00.080] [INFO] Dynarmic ARM32 JIT Started. PC=0x2100.
[00:00:00.120] [INFO] Intercepted UIApplicationMain -> SoosizAppDelegate Initialized Successfully.
```
