# ARM Execution Feasibility Study: Swordigo on x86_64 Linux

## 1. Objective
Determine the most practical method to execute the ARMv7 `libswordigo.so` binary on an x86_64 Linux host.

## 2. PS Vita vs. x86_64 Linux Architecture

### PS Vita (The "So-Loader" Approach)
*   **CPU**: ARM Cortex-A9 (ARMv7).
*   **Binary**: ARMv7 (`libswordigo.so`).
*   **Execution**: Native. The instruction sets match, so no CPU emulation is required.
*   **Role of Loader**: The loader acts as a **dynamic re-linker**. It maps the ELF into memory, resolves relocations, and bridges Android-specific calls (JNI, GLES, OpenAL) to PS Vita native APIs.

### x86_64 Linux
*   **CPU**: x86_64.
*   **Binary**: ARMv7 (`libswordigo.so`).
*   **Execution**: **Emulated/Translated**. The CPU cannot execute ARM instructions directly.
*   **Requirement**: A CPU emulation layer (JIT or interpreter) is mandatory in addition to the API bridging.

## 3. Candidate Solutions

| Solution | Type | Advantages | Disadvantages |
| :--- | :--- | :--- | :--- |
| **QEMU User Mode** | Instruction Translation | Robust, handles syscalls, very mature. | High overhead, difficult to bridge host GLES/AL libraries seamlessly. |
| **Unicorn Engine** | Emulation Framework | Lightweight, easy to embed, perfect for "surgical" API hooking. | Slower than QEMU TCG, requires manual ELF loading/relocation logic. |
| **Dynarmic** | High-perf Emulation | Extremely fast (used in Yuzu), cleaner API than Unicorn. | Higher implementation complexity, primarily AArch64 focused (though ARMv7 exists). |
| **Android Emulator / Waydroid** | Full VM / Container | 100% compatibility, handles GLES via host pass-through. | Heavy, not a "native port" experience, requires full Android environment. |
| **Box86/64** | Translation + Wrapping | Near-native speed, handles library bridging (wrapping) perfectly. | **Wrong direction**: Designed for x86 guest on ARM host. No "BoxARM" for x86 exists yet. |

## 4. Analysis of QEMU User Mode

Can an ARM process under `qemu-user`:
*   **dlopen libswordigo.so?** Yes, provided an ARM-compatible `linker` and `libc.so` (Bionic) are available in the guest rootfs.
*   **Resolve JNI exports?** Yes, `dlsym` works as expected.
*   **Call setupNativeInterface()?** Yes.
*   **Call setupApplication()?** Yes.

**The "Bridge" Problem**: If `libswordigo.so` calls `glDrawArrays`, it will try to call an ARM version of `libGLESv1_CM.so`. To get hardware acceleration, we must either:
1.  Use an ARM GLES library that talks to a "virtual GPU" (VirGL) that QEMU forwards to the host.
2.  Use a "Wrapper" (like Box86 does) that intercepts the ARM call and executes the host x86_64 `glDrawArrays`. QEMU does not support this natively without significant hacking.

## 5. Recommendation: Embedded Emulation (Unicorn/Dynarmic)

The most "professional" and "Vita-like" way to build a standalone desktop port is to use an **Embedded Emulation Loader**.

### Design:
1.  **Host App (x86_64)**: Manages windowing (SDL2), OpenGL context, and OpenAL.
2.  **Emulation Core**: Unicorn or Dynarmic.
3.  **Loader Component**:
    *   Loads `libswordigo.so` into the emulator's memory.
    *   Implements a custom `dlopen`/`dlsym` shim.
    *   **Trampolines**: When the ARM code calls an external function, it jumps to a special address that triggers a hook in the host app.
    *   **Marshalling**: The host app reads ARM registers (R0-R3), converts types if necessary, and calls the native x86_64 function.

### Effort Estimate:
*   **Complexity**: High.
*   **Implementation Effort**: 2-4 weeks for a stable core.
*   **Pros**: No Android sysroot required, near-native integration, single binary distribution.

## 6. Simple Proof-of-Concept (PoC)

To verify the feasibility of QEMU-based loading without a full VM:

1.  **Toolchain**: Install `gcc-arm-linux-gnueabihf`.
2.  **Test Loader**:
    ```c
    // loader_arm.c
    #include <dlfcn.h>
    #include <stdio.h>

    int main() {
        void* handle = dlopen("./libswordigo.so", RTLD_NOW);
        if (!handle) {
            printf("Failed to load: %s\n", dlerror());
            return 1;
        }
        printf("Successfully loaded libswordigo.so at %p\n", handle);
        void* sym = dlsym(handle, "Java_com_touchfoo_swordigo_Native_setupNativeInterface");
        printf("Symbol setupNativeInterface found at %p\n", sym);
        return 0;
    }
    ```
3.  **Execution**:
    ```bash
    qemu-arm -L /path/to/android/sysroot ./loader_arm
    ```

## 7. Conclusion
A full ARM VM is **not** necessary, but a CPU translator is. The cleanest approach is an **Embedded Emulation Loader** using Unicorn or Dynarmic, as it allows the game to interact directly with host libraries, mimicking the high-performance bridging seen in the PS Vita port.
