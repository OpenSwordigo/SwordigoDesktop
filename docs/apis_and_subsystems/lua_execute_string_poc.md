# JNI Lua Harness: ExecuteString Proof-of-Concept

This document establishes the exact calling conventions, register assignments, and memory structures required to execute custom Lua scripts via `Caver::ProgramState::ExecuteString` (address `00319991` in guest memory).

---

## 1. Register & Calling Convention Specifications

On 32-bit ARM (EABI), C++ member functions follow the standard ABI calling convention:

*   **`R0`**: Holds the `this` pointer (pointer to the instance of `Caver::ProgramState`).
*   **`R1`**: Holds the first argument pointer (a reference to the `std::string` object, which translates to a pointer `std::string*`).
*   **`LR`** (Link Register): Set to the return safety address (e.g. `0xE0000000`) before jumping to the function.
*   **`PC`** (Program Counter): Set to `0x00319991` (the entry point of `Caver::ProgramState::ExecuteString`).

---

## 2. Instantiating `ProgramState`

Before executing a script, we need a valid `Caver::ProgramState` instance (`R0`).

### Allocation
1.  **Allocate memory**: Allocate a block of 1024 bytes in guest memory (e.g. at `0x03000000`) and zero it out.
2.  **Call Constructor**:
    *   Target symbol: `_ZN5Caver12ProgramStateC1EPS0_` (address `00319531`).
    *   Set `R0 = 0x03000000` (`this` pointer).
    *   Set `R1 = 0` (passing `NULL` as the parent `ProgramState` pointer to create the root state).
    *   Call constructor.
3.  **Result**: The constructor initializes the class and registers a fresh `lua_State*` internally.
4.  **Register Libraries (Optional)**:
    *   If you need engine APIs (like `MusicPlayer`), call:
        `Caver::ProgramState::RegisterProgramLibrary()` (address `003195f5`) with `R0 = 0x03000000`.

---

## 3. String Representation in Memory (`R1`)

Since `ExecuteString` accepts a `std::string const&`, we must manually construct a `std::string` structure in guest memory. The memory layout depends on the STL version used during compilation:

### Option A: Libc++ Layout (Modern Android NDK - SSO & Long Union)
This structure is 12 bytes on 32-bit ARM:

#### 1. Short String Optimization (SSO) (For lengths < 11 bytes, e.g. `"print(1)"` of length 8)
*   **Byte 0**: Stores `length << 1` (with bit 0 being 0 to indicate short string). For length 8: `8 << 1 = 16 = 0x10`.
*   **Bytes 1-8**: String characters (`"print(1)"`).
*   **Bytes 9-11**: Padding bytes (`0x00`).
*   *Memory Dump (12 bytes)*:
    `10 70 72 69 6e 74 28 31 29 00 00 00` (in hex)

#### 2. Long String Layout (For lengths >= 11 bytes, e.g. `"print('hello')"` of length 14)
*   Allocate string payload at a guest address (e.g. `0x04000000`) and write the null-terminated characters `"print('hello')\0"`.
*   Construct the 12-byte string descriptor structure in memory (e.g. at `0x03500000`):
    *   **Offset 0 (4 bytes)**: `Capacity | 1` (capacity = 16, with bit 0 set to 1 indicating long string): `16 | 1 = 17 = 0x00000011`.
    *   **Offset 4 (4 bytes)**: `Size` (length of characters = 14): `0x0000000e`.
    *   **Offset 8 (4 bytes)**: `Data Pointer` (points to the allocated payload): `0x04000000`.

### Option B: GNU STL Layout (Older NDKs)
This structure is a single 4-byte pointer to the string payload. However, the payload is preceded by a 12-byte header:
*   **Payload Address - 12 (4 bytes)**: String Length (e.g. 14).
*   **Payload Address - 8 (4 bytes)**: Allocated Capacity (e.g. 16).
*   **Payload Address - 4 (4 bytes)**: Refcount (usually `1`).
*   **Payload Address (R1)**: Pointer directly to the null-terminated characters in memory.

---

## 4. Proof-of-Concept Execution Plan: `ExecuteString("print('hello')")`

To execute this call in the guest emulator:

```
[1. Allocate memory for ProgramState]
   Pointer: R0 = 0x03000000 (1024 bytes, zeroed)
   
[2. Call ProgramState Constructor]
   R0 = 0x03000000
   R1 = 0x00000000 (NULL parent)
   BLX 0x00319531  --> Initializes lua_State
   
[3. Allocate custom Lua script characters]
   Pointer: 0x04000000
   Write string: "print('hello')\0" (length = 14)
   
[4. Construct std::string structure (R1)]
   Pointer: R1 = 0x03500000 (12 bytes)
   Write structure (libc++ format):
     +0x00: 0x00000011 (Capacity 16 | 1)
     +0x04: 0x0000000e (Length 14)
     +0x08: 0x04000000 (Data Pointer)
     
[5. Execute the custom script]
   R0 = 0x03000000 (ProgramState)
   R1 = 0x03500000 (std::string*)
   LR = 0xE0000000 (Safety return address)
   BLX 0x00319991  --> Runs ExecuteString
```

### Hooking & Verification
When the guest interpreter executes `"print('hello')"`, it calls the standard Lua base library `print` which eventually invokes writing to stdout (standard output). In a host emulator (Unicorn), we can verify this by intercepting calls to `write` or `__android_log_print` and checking the output buffer for `"hello"`.
