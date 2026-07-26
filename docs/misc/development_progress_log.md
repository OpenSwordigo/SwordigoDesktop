# Progress Log

## 2026-06-14 - Agent 1

- Read `prompt/Phrase 1/main.md` and `prompt/Phrase 1/preFeed.md` before investigation.
- Read shared research files; they were empty at the start of the Agent 1 pass.
- Mapped Vita loader boot sequence from `so_file_load` through relocation/import resolution, fake JNI setup, direct JNI lifecycle calls, and the frame loop.
- Mapped Android Java startup sequence from `System.loadLibrary` through `setFilesDir`, `setCacheDir`, `setAssetManager`, `setupNativeInterface`, `setupApplication`, `setApplicationViewSize`, `updateApplication`, and `drawApplication`.
- Confirmed local `libswordigo.so` is ARM ELF32 and cannot be `dlopen`ed natively on x86_64 Linux.
- Confirmed local native exports include the core startup/frame JNI functions but do not include `handleApplicationLaunch` or `googleSignInCompleted`; documented version mismatch risk with the Vita loader source.
- Cataloged native imports for Android asset APIs, GLES/EGL, OpenAL, logging, libc, pthread, and zlib.
- Ran a small asset-format experiment with `file`, `xxd`, and `protoc --decode_raw` on `.gdata`, `.gstate`, and `.scene`; documented results in `research/experiments/experiment_001.md`.
- Attempted ARM disassembly with host `objdump`; it failed because this environment lacks an ARM-capable objdump.

## 2026-06-14 - Repair after Agent 2 overwrite

- Restored evidence-based `findings.md`, `protobuf_schema.md`, `hypotheses.md`, `questions.md`, `progress_log.md`, and `agent_messages.txt` after Agent 2 replaced them with speculative summaries.
- Preserved verified Agent 2 direction only as cautious Lua/program scripting notes: `assets/resources/plains_woodkeep3.scene` contains readable Lua source and `LuaQ` chunks, and native symbols include `Caver::Program*` and related components.
- Reclassified unverified claims about field `158`, `.scl` schema, item type mappings beyond sampled values, quests, and world triggers as open questions until backed by decode evidence.

## 2026-06-14 - Agent 2 (Asset Evidence)

- Re-verified asset findings with command evidence to supplement the research baseline.
- Confirmed `.scl` files as protobuf entity template collections via `protoc --decode_raw < "assets/resources/monsters.scl"`.
- Identified specific protobuf fields for Lua source/bytecode in `.scene` files via `protoc --decode_raw < "assets/resources/plains_woodkeep3.scene"`.
- Documented quest and trigger structures in `gamedata.gdata` message types 3 and 5.

## 2026-06-14 - Agent 2 (Mission 2: Linux Loader & Native Analysis)
- Analyzed Vita loader source to design a Linux-compatible ARMv7 loader.
- Mapped native engine symbols (`Caver::*`) from `libswordigo.so`.
- Investigated `.POD` asset pipeline, confirming separate animation files and external `.pvr` textures.
- Produced `research/linux_loader_design.md`, `research/native_symbol_map.md`, and `research/pod_pipeline.md`.
- Determined the minimum viable JNI and Android API set to reach `setupApplication()`.

## 2026-06-14 - Agent 2 (Mission 5: Version Alignment)
- Compared Swordigo 1.1 and 1.4.6 native libraries and Java source.
- Identified that 1.4.6 adds 13 JNI exports, including `handleApplicationLaunch`.
- Confirmed that the Vita port reference aligns perfectly with 1.4.6's startup sequence.
- Mapped major engine subsystems in 1.4.6 (`Caver` namespace).
- Recommended 1.4.6 as the primary development target for the Linux port.

## 2026-06-14 - Agent 2 (Mission 6: Boot Prototype Execution)
- Integrated **Unicorn Engine** for ARMv7 emulation on x86_64 host.
- Implemented **Magic LR Stop Condition** and memory-mapped bridge addresses.
- Developed `resolve_all_to_bridge` to automatically stub all external library dependencies.
- **SUCCESS**: Executed `handleApplicationLaunch` and `setupNativeInterface` successfully.
- Currently debugging `setupApplication` memory fetch and JNI return logic.

## 2026-06-14 - Agent 2 (Mission 7: Breaking through setupApplication)
- Overhauled JNI bridge with functional callbacks and a guest heap allocator.
- Replaced root library with version 1.4.6 and verified symbols.
- **SUCCESS (Tier 2)**: Reached and returned from `setupApplication()`.
- **SUCCESS (Tier 4)**: Reached and executed over 1 million instructions in `drawApplication()`.
- Identified memory pressure (> 1GB) and uninitialized pointers as the next hurdles.
