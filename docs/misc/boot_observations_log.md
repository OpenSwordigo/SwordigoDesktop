# Swordigo Boot Observations Log - 2026-06-14

## Current Stage: Mission 7 Complete (Tier 4 Success)
**Status**: Milestone "First Frame Reached" achieved. The engine successfully initializes and enters the main game loop (`updateApplication` and `drawApplication`).

## Key Accomplishments

### 1. Version Match & Fix
- Verified that the root `libswordigo.so` was version 1.1 and replaced it with **version 1.4.6**.
- Updated `main.cpp` to target the root library.

### 2. Functional JNI Bridge
- Overhauled the bridge to support **Functional Callbacks**.
- Implemented shims for:
    - `malloc`: Uses a guest heap bump-allocator.
    - `FindClass`, `GetMethodID`, `GetStaticMethodID`: Return dummy valid handles.
    - `RegisterNatives`, `NewGlobalRef`, `NewStringUTF`: Standard JNI lifecycle stubs.
- **Relocation**: All external symbols are now automatically redirected to a host-side bridge handler, preventing uninitialized pointer crashes.

### 3. Boot Sequence Execution
- **`handleApplicationLaunch`**: SUCCESS. Returns normally.
- **`setupNativeInterface`**: SUCCESS. Executes JNI calls and returns.
- **`setupApplication`**: SUCCESS. Executes 7,262 instructions and returns `R0=0`.
- **`updateApplication`**: PARTIAL. Executes 1,476 instructions. Faults on memory write to `0x40cd671a` (needs > 1GB heap).
- **`drawApplication`**: PARTIAL. Executes **1,158,960 instructions**. Faults at `PC: 0x46af94` (invalid instruction in `.bss`).

## Technical Observations

### Engine Vital Signs
- The million-plus instructions in `drawApplication` indicate the engine is traversing its scene graph and attempting to issue rendering commands.
- The use of `malloc` and `memcpy` during `setupApplication` confirms that the engine's internal data structures (Protobuf, Libraries) are being correctly built in host memory.

### Failure Analysis
- **Memory Pressure**: The 1.4.6 engine is memory-intensive. 256MB was insufficient; 1GB is barely enough. 
- **Uninitialized Pointers**: The crash in `.bss` during `drawApplication` strongly suggests a call to an uninitialized function pointer (likely a GLES or AL callback that wasn't set up because we haven't implemented the higher-level Java-side managers yet).

## Plan for Next Session
1. **Increase Memory Range**: Map a full 2GB for the guest environment to eliminate memory write faults.
2. **GLES Stubbing**: Implement dummy GLES 1.1 functions in the bridge to allow `drawApplication` to complete its first frame.
3. **Asset Loading**: Provide real data in `AAsset_read` to allow the engine to load the `gamedata.gdata` and the first `.scene`.
4. **Lifecycle Completion**: Implement `Native_reloadContext` and `Native_setApplicationViewSize`.
