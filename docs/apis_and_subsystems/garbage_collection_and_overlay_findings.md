# Swordigo PC Port — GC Step & Overlay Memory Corruption Audit (Phase 2.5)
**Date:** July 18, 2026

## 1. Lua GC Step Loop (`luaC_step`)
By resolving SRE symbols, we mapped the guest active program counter `0x2020fd4` to `luaC_step + 0x4c` (inside the SRE linked Lua engine):
```
0000000000020f88 t luaC_step
```
* **Why it happens:**
  - `luaC_step` is the incremental garbage collector step in Lua 5.1.
  - During `AchievementsManager` initialization or save loading, Lua scripts (likely mod hooks) execute heavy memory allocations, which recursively trigger Lua GC sweeps.
  - Because incremental GC execution requires high JIT cycle counts inside the Dynarmic emulator, the 30-second wall-clock limit is breached, forcing the JIT watchdog to abort the frame. This corrupts CPU/stack state and triggers subsequent `NoExecuteFault` crashes.

---

## 2. GameOverlayView Corruption Bug (Disappearing HUD & Buttons)
We traced the exact mechanism behind the touch controls and HUD buttons disappearing:
* **The Bug:**
  In [sre_mini_api.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/sre/sre_mini_api.c#L2040), `l_mini_set_controls_hidden` manually overwrites offset `0xe4` on the `overlay` pointer:
  ```c
  *(char*)((char*)overlay + 0xe4) = (char)hidden;
  ```
* **The Impact:**
  - `GameOverlayView` inherits from `GUIView`. The offset `0xe4` is the base class `hidden` status flag for the entire `GUIView`.
  - Overwriting this flag to `1` hides the **entire overlay** (including the Health bar, Mana bar, Coin bar, and all buttons).
  - The native function `GameOverlayView::SetControlsHidden` (at offset `0x2adb2d`) does *not* hide the overlay base class itself; instead, it hides the child controls (like the joystick) and handles button touch cancellation (`GUIButton::CancelPress`) safely.
  - Manually forcing `overlay + 0xe4` to `1` breaks the engine state and permanently hides the entire HUD view, preventing any buttons from being drawn or clicked.

### Solution:
We must modify `l_mini_set_controls_hidden` to remove the direct write to `overlay + 0xe4`. We should exclusively rely on the native `GameOverlayView::SetControlsHidden` API to toggle touch controls visibility safely.
