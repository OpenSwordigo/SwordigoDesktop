# Swordigo PC Port — Advanced Bug Ride & Emulation Audit (Phase 2)
**Date:** July 18, 2026

## 1. Executive Summary
During this advanced research audit against the original ARM64 1.4.12 binary, we mapped out a critical engine hang occurring during gameplay/save transitions. This hang is directly responsible for the **GUI buttons/touch controls disappearing** and the **indefinite Dynarmic `NoExecuteFault` log spam**. 

The failure cascade unfolds as follows:
1. An infinite loop or performance lockup occurs in guest memory block at virtual address `0x1478ccc` (offset `0x478ccc` in `libswordigo.so`).
2. This loop makes hundreds of thousands of JNI/bridge calls (e.g., `215,321` calls in 30 seconds) related to achievement reporting or save state synchronization.
3. The Dynarmic emulator's 30-second wall-clock watchdog safety net fires, force-aborting the function by resetting the program counter (`PC`) to `MAGIC_LR` and the stack pointer (`SP`) to the entry stack.
4. Because the C++ frame was aborted midway, the guest stack and CPU registers are left in a corrupted state. Mutexes remain locked, and subsequent function returns pop garbage sign-extended host pointers into the PC, triggering immediate `NoExecuteFault` loops.
5. Due to the corrupted thread state, the game's rendering pipeline clears the screen but skips normal level and HUD overlay draws (draw count drops from ~113 to 8), causing all controls and game objects to instantly freeze and disappear.

---

## 2. Technical Findings & Cause Analysis

### A. The `0x1478ccc` Watchdog Abort
* **Symptoms:** 
  ```
  [Dynarmic] Heavy function 0x1478ccc — chunk 2/50000 (PC=0x2020fd4)
  [Dynarmic] Wall clock limit (30000ms) hit for 0x1478ccc — force-returning
  [PERF/Dynarmic] SLOW call at 0x1478ccc took 30003ms (bridge_calls=215321)
  ```
* **Analysis:**
  - `0x1478ccc` corresponds to offset `0x478ccc` in the ARM64 `libswordigo.so` binary.
  - The high number of `bridge_calls` shows the guest code is continuously jumping to host-side hooks (likely JNI callbacks or atomic ref-count monitors).
  - The program counter `PC=0x2020fd4` (offset `0x20fd4` inside `libsre.so`) indicates SRE guest-side code is active within this JIT translation block when the timeout occurred.

### B. The `NoExecuteFault` Cascading Crash
* **Symptoms:**
  ```
  [Dynarmic] NoExecuteFault at 0xfffff26c7102c057
  [Dynarmic] NoExecuteFault at 0xfffff26c7102c05b
  ...
  ```
* **Analysis:**
  - In SwordigoDesktop's Dynarmic JIT, `NoExecuteFault` is raised when the JIT engine fails to read code because the virtual address is out of the mapped guest memory bounds.
  - The address `0xfffff26c7102c057` is a sign-extended negative value containing the lower 32-bit offset `0x7102c057`.
  - When the watchdog timer force-returns, the guest CPU resumes execution with a mismatched stack. When the guest code eventually runs a `RET` (using `LDR X30, [SP, #...]`), it loads a corrupted pointer from the stack and jumps to it, resulting in the invalid memory execution loop.

### C. `updateApplication` Wrapper Instability
* **Findings:**
  - The `updateApplication` hook in [sre_lua.c](file:///run/media/quantumcreeper/TVPG/Prenxy%20Packages/SwordigoDesktop/src/sre/sre_lua.c#L980) was commented out to revert to native execution.
  - The trampoline manager encountered issues because the `CBZ` instruction inside the 7-instruction `updateApplication` wrapper has a relative offset limit of $\pm 1\,\text{MB}$. Relocating it to SRE's cave region (which resides 29 MB away) caused the relative offset to wrap around, sending the PC to `0x20010`.

---

## 3. Recommended Actions & Verification Commands
To proceed, we must obtain the exact symbol names mapping to the hanging block (`0x478ccc`) and SRE active PC (`0x20fd4`).

Please run the following commands on the host system to dump and grep the symbol tables:

```bash
# 1. Dump ARM64 libswordigo symbols
nm -C -n "/home/quantumcreeper/SwordigoDesktop/engine/v1.4.12/arm64-v8a/libswordigo.so" > /home/quantumcreeper/SwordigoDesktop/arm64_symbols.txt

# 2. Dump SRE library symbols
nm -C -n "/home/quantumcreeper/SwordigoDesktop/libsre.so" > /home/quantumcreeper/SwordigoDesktop/sre_symbols.txt

# 3. Find symbol mapping for 0x478ccc (AchievementsManager / JNI)
grep -C 15 " 0000000000478" /home/quantumcreeper/SwordigoDesktop/arm64_symbols.txt || grep -i -C 15 "478c" /home/quantumcreeper/SwordigoDesktop/arm64_symbols.txt

# 4. Find symbol mapping for SRE active hook at 0x20fd4
grep -C 15 " 0000000000020" /home/quantumcreeper/SwordigoDesktop/sre_symbols.txt || grep -i -C 15 "20f" /home/quantumcreeper/SwordigoDesktop/sre_symbols.txt
```

Please paste the output of commands 3 and 4 in your response. This will tell us exactly which method is looping so we can implement a safe guest-side hook or stub out the problematic achievement/JNI loop!
