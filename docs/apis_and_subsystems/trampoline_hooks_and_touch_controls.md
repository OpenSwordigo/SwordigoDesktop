# Swordigo PC Port — Trampoline Mechanics & Control Emulation Audit (Phase 2.7)
**Date:** July 18, 2026

## 1. Trampoline Relocation Mechanics
We analyzed the implementation of [trampoline_mgr.h](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/platform/trampoline_mgr.h#L90-L134) to trace how instruction relocation behaves:
1. **Cave Allocation:** Claims a monotonically growing 64-byte slot in the `0x3000000` arena.
2. **Copy & Relocate:** Calls `copy_and_relocate` to copy the initial $N$ instructions ($N \ge 4$) from the guest target function to the cave, resolving and rewriting relative AArch64 branch offsets (`ADRP`, `B`, `BL`, `CBZ`, etc.) to match the new address.
3. **Relay Return Jump:** Appends an absolute indirect jump template (LDR X16 + BR X16) pointing back to `target_vaddr + (N * 4)` at the end of the cave.
4. **Trampoline Write:** Overwrites the first 16 bytes of the original guest function with a branch template pointing to SRE's replacement function.
5. **Pointer Registration:** Stores the cave address in the guest-side `g_orig_*` function pointer variable.

---

## 2. Why `updateApplication` CBZ Overflows
* **Mechanics:**
  - The JNI `updateApplication` wrapper (Ghidra offset: `0x479aac`) is extremely short (7 instructions / 28 bytes).
  - It contains a `CBZ` (Compare and Branch on Zero) instruction which uses a relative 19-bit signed offset, limiting its branch target range to $\pm 1\,\text{MB}$ from the current PC.
  - SRE's cave arena resides at `0x3000000` (which is ~32 MB away from `libswordigo.so` load base at `0x1000000`). Relocating the `CBZ` instruction into the cave causes its offset to wrap around, directing execution to an invalid address (`0x20010`) and triggering crash loops.
  - **Verdict:** Leaving `updateApplication` unhooked and executing natively is the correct design, as the original wrapper is completely safe.

---

## 3. The Ultimate Touch Controls Resolution
We confirmed that `l_mini_set_controls_hidden` was manually setting `overlay + 0xe4 = hidden`.
* In the `GUIView` hierarchy, offset `0xe4` is the boolean visibility flag of the entire layout. Setting it to `1` hides the **entire overlay** (health, mana, coins, and buttons).
* The native function `GameOverlayView::SetControlsHidden` (at offset `0x2adb2d` in `libswordigo.so`) only hides the touch buttons/joystick itself while keeping the health/mana HUD elements visible.
* **Resolution:** In execution phase, we must remove the manual write to `overlay + 0xe4` in [sre_mini_api.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/sre/sre_mini_api.c#L2040) and exclusively call the native `g_sre_GameOverlayView_SetControlsHidden` function pointer to toggle controls cleanly.
