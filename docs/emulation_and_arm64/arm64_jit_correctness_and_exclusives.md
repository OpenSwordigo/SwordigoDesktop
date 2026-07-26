# ARM64 JIT Execution Correctness, Exclusive Locks & Scene Destruction Analysis

## Executive Summary
This document logs critical discoveries regarding ARM64 execution correctness, `LDXR`/`STXR` exclusive memory monitor handling, `boost::shared_ptr` destructor performance, and memory crash diagnostics for `libswordigo v1.4.12` running on SRE.

---

## 1. Scene Destruction & `LDXR`/`STXR` Exclusive Monitor Analysis

### 1.1 The Scene Loading & Destructor Chain
In `libswordigo.so`, scene transitions (`GameSceneView::~GameSceneView()`, `SceneLoadingView::Update`) destroy dozens of `boost::shared_ptr` references sequentially.
```
GameSceneView::~GameSceneView() @ 0x0044dbfc
    Releases 8+ boost::shared_ptr members sequentially
    Each: LDXR/STXR loop to decrement atomic refcount
```

### 1.2 STXR -> STR Patcher Optimization
Under single-threaded host CPU emulation, true multi-core memory contention on exclusive monitors (`LDXR`/`STXR`) is absent. 
- **Legacy Issue**: `Dynarmic` `MemoryWriteExclusive*` callbacks were adding atomic CAS barrier overhead per reference count decrement across 1000s of objects during scene unloading.
- **Optimization**: Applying the `STXR -> STR` patcher directly converts exclusive store instructions into direct stores in guest code (`0xC8000000 -> 0xF9000000`), converting costly CAS loops into single atomic instructions.

---

## 2. Analysis of `NoExecuteFault` Crashes

### 2.1 Failure Signature
```
[Dynarmic] NoExecuteFault at 0xffffa3f3aa1303e0
  X0  = 0x97ffa181aa1303e0  <- corrupt upper bits
  X1  = 0x67754220776f6e53  <- "Snow w Bu" (ASCII texture string)
  X17 = 0x2019824            (lua_pushlstring)
  X30 = 0x2023294            (inside Lua runtime)
  PC  = 0xffffa3f3aa1303e0  <- invalid guest PC target
```

### 2.2 Root Cause Mechanics
1. **ASCII Buffer Overwrites**: String data (`"Snow w Bu"`) overwriting adjacent C++ vtables or object pointers.
2. **Use-After-Free in Lua Userdata**: Lua script calling a C++ member function on a destroyed C++ entity object whose raw memory address was held in a Lua lightuserdata wrapper.

---

## 3. Lua Garbage Collector Hitching (`SRE-GC`)
Large circular table dependencies in game scripts can trigger `GCSpropagate` traversal limits.
- **Diagnostic**: Lower the Lua GC traversal cycle threshold in `sre_lua_libs.c` to recover from cyclic references without 500M+ iteration hitches.
