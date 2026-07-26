# SRE Native Hook API Reference

> Complete reference for all native function hooks in **libsre.so** (Swordigo Runtime Engine) targeting **libswordigo.so v1.4.12 ARM64**.

---

## 1. Hook Registration & Architecture

`libsre.so` is loaded into guest address space at `0x2000000`. At startup, the host writes **16-byte ARM64 trampolines** at each target function offset in `libswordigo.so`, redirecting execution to SRE implementations.

```asm
; 16-byte ARM64 Trampoline Pattern
LDR  X16, [PC, #8]      ; Load 64-bit target address
BR   X16                 ; Branch to SRE replacement
.quad <sre_func_addr>    ; 64-bit absolute address
```

---

## 2. Active Hook Table Inventory (34 Hooks)

| # | Hook Symbol | Category | Description |
|---|---|---|---|
| 1 | `sre_CppString_from_char_p` | CppString | Replaces GNU COW atomic `std::string` constructors with non-atomic refcounts. |
| 2 | `sre_CppString_assign` | CppString | Non-atomic string assignment override. |
| 3 | `sre_CppString_append` | CppString | Non-atomic string concatenation override. |
| 4 | `sre_CppString_release` | CppString | Non-atomic string destructor/release override. |
| 5 | `sre_ProgramState_Execute` | Lua Error | Intercepts `lua_pcall` / `lua_cpcall` execution to wrap in C++ exception boundaries. |
| 6 | `sre_ProgramState_Resume` | Lua Error | Intercepts `lua_resume` execution for coroutines. |
| 7 | `sre_luaD_throw` | Lua Error | Replaces C++ `throw` in Lua panic paths with longjmp. |
| 8 | `sre_ProgramPanic` | Lua Error | Catches Lua runtime panic handlers. |
| 9 | `sre_cxa_throw` | Lua Error | Catches `__cxa_throw` across guest/host boundaries. |
| 10 | `sre_BackgroundComponent_Draw` | Background | Renders modern GL skybox/background quad. |
| 11 | `sre_RotatingBackgroundComponent_Draw` | Background | Rotating background rendering pass override. |
| 12 | `sre_RotatingBackgroundComponent_Update` | Background | Angle accumulator update override. |
| 13-20 | `sre_GUI*_DrawRect` | GUI Stack | Intercepts `GUIWindow`, `GUIView`, `GUIButton`, `GUILabel`, `GUIFrameView`, `GUIAlertView`, `GUISlider`, `NewMenuView` draw calls for custom scaling & mouse input. |
| 21 | `sre_MainMenuVC_DidOpenShop` | Menu/Death | Bypasses Android billing SDK requirement. |
| 22 | `sre_GameOverVC_ShowAdMaybe` | Menu/Death | Bypasses Android interstitial ad SDK call during hero respawn. |
| 23-26 | `sre_*TextInput*` | Text Input | Redirects Android IME keyboard input to host desktop SDL3 keyboard. |
| 27-33 | `sre_MusicPlayer_*` | Audio/Music | Intercepts background music tracks (`PlayMusicWithName`, `FadeIn`, `SetVolume`) and redirects to OpenAL. |
| 34 | `sre_GameSceneView_Update` | Game State | Controls scene update tick rate & entity updates. |

---

## 3. CppString Non-Atomic Replacement
Standard GNU libstdc++ `std::string` refcounting uses `LDAXR`/`STLXR` atomic spin loops, which stall inside single-threaded CPU emulators. SRE replaces the string rep header (`SreStringRep`) refcounting with single-threaded direct integer operations (`refcount--`), eliminating 100% of atomic spinlocks.
