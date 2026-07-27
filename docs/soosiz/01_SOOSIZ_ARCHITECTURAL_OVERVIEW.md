# 01: Soosiz HD Architectural Overview (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/01_SOOSIZ_ARCHITECTURAL_OVERVIEW.md`  
> **Status:** Remastered Direct Binary Loading Architecture  
> **Target Executable:** `/home/quantumcreeper/SwordigoDesktop/OpenSoozis/SoosizHD` (Mach-O ARMv7 32-bit Executable)

---

## 1. Direct Binary Execution Architecture

The integration strategy for **Soosiz HD** into **OpenSwordigo Desktop** centers on **Direct Mach-O Binary Loading**. Rather than recompiling decompiled source code, OpenSwordigo's loader subsystem (`src/loader/`) will load and execute the original, unmodified 32-bit ARMv7 executable binary **`SoosizHD`**.

```
       +-------------------------------------------------------+
       |             OpenSwordigo Desktop Engine               |
       |                   (x86_64 / ARM64)                    |
       +-------------------------------------------------------+
       | Mach-O Memory Loader & Segment Allocator (src/loader/) |
       |  - Maps __TEXT (0x1000) & __DATA (0xC8000) into RAM    |
       +-------------------------------------------------------+
       | Dynarmic ARM32 JIT Execution Engine                   |
       |  - Executes ARMv7 assembly instructions natively      |
       +-------------------------------------------------------+
       | Dynamic Symbol Interceptor & Dylib Bridge             |
       |  - Hooks imports (libobjc, Foundation, OpenGLES, OpenAL)|
       +-------------------------------------------------------+
       | Unmodified SoosizHD Mach-O Binary Execution Target   |
       +-------------------------------------------------------+
```

---

## 2. Mach-O Binary Segment & Section Map

Binary analysis of `SoosizHD` reveals the exact memory map:

### 1. `__TEXT` Segment (`vmaddr: 0x00001000`, `vmsize: 0x000C7000`, `fileoff: 0x0`):
- `__TEXT.__text` (`0x2100` - `0xB0BD0`): Main executable ARM code (715 KB). Contains start routine, ObjC method implementations, C++ math kernels.
- `__TEXT.__cstring` (`0xB0BD0` - `0xC71C4`): ASCII string constants (91 KB).
- `__TEXT.__stub_helper` (`0xC753C` - `0xC7D58`): Dyld symbol resolution stub helper.
- `__TEXT.__symbolstub1` (`0xC7D58` - `0xC8000`): Import symbol stubs.

### 2. `__DATA` Segment (`vmaddr: 0x000C8000`, `vmsize: 0x00026000`, `fileoff: 0xC7000`):
- `__DATA.__lazy_symbol`: Lazy binding symbol pointers.
- `__DATA.__nl_symbol_ptr`: Non-lazy binding symbol pointers.
- `__DATA.__objc_classlist` (`0xEC3E0`): Array of Objective-C class pointers (267 classes).
- `__DATA.__objc_selrefs` (`0xCEFAC`): Objective-C selector reference pointers (1,517 selectors).
- `__DATA.__objc_const` (`0xD0EF8`): Objective-C method descriptions, ivar offsets, class structures.

### 3. `__LINKEDIT` Segment (`vmaddr: 0x000EE000`, `vmsize: 0x000B6000`, `fileoff: 0xEC000`):
- Symbol table (`LC_SYMTAB`), dynamic symbol table (`LC_DYSYMTAB`), export trie, and relocation entries.

---

## 3. Linked Dynamic Framework Dependencies

`SoosizHD` imports 14 system dynamic libraries. The loader resolves these symbols to native OpenSwordigo C/C++ implementations:

1. `/usr/lib/libobjc.A.dylib` $\rightarrow$ Mapped to `src/soosiz/objc_shim.c`.
2. `/System/Library/Frameworks/Foundation.framework` $\rightarrow$ Mapped to Foundation C stubs (`NSString`, `NSUserDefaults`).
3. `/System/Library/Frameworks/OpenGLES.framework` $\rightarrow$ Mapped to OpenSwordigo SRE OpenGL context (`src/sre/sre_init.c`).
4. `/System/Library/Frameworks/OpenAL.framework` $\rightarrow$ Mapped to OpenSwordigo SRE audio engine (`src/sre/sre_music.c`).
5. `/usr/lib/libsqlite3.dylib` $\rightarrow$ Mapped to native desktop `sqlite3`.
6. `/System/Library/Frameworks/UIKit.framework` $\rightarrow$ Mapped to SDL2 windowing and input event pump (`src/main.cpp`).

---

## 4. Key Architectural Advantages of Direct Binary Loading

1. **Guaranteed 100% Code Fidelity:** Running the untouched `SoosizHD` binary eliminates subtle logic bugs or missing code that might occur in decompilation exports.
2. **Deterministic Behavior:** Binary memory layouts, ivar offsets, and struct packings are preserved exactly as compiled by Touch Foo.
3. **High Performance:** Dynarmic ARM32 JIT translates ARMv7 instructions to native host x86_64 / ARM64 assembly with near-native speed.
