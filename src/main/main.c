/* ============================================================================
 * DEAD LEGACY PROTOTYPE — NOT COMPILED.
 *
 * This file is an obsolete ARMv7 boot prototype (it uses the old ARM32
 * so_module / so_file_load API and prints "CRITICAL BLOCKER: ARMv7 execution
 * not possible"). It defines its OWN int main() which collides with the real
 * host entry point in src/main.cpp. It is not referenced by any CMake target;
 * the whole body is disabled with #if 0 so it can never be compiled in.
 * Kept on disk for historical reference only — superseded by src/main.cpp.
 * ============================================================================ */
#if 0
#include <stdio.h>
#include <stdlib.h>
#include "../loader/loader.h"
#include "../jni/jni_shim.h"
#include "../android/log.h"
#include "../android/asset_manager.h"

int main() {
    printf("Swordigo Linux Boot Prototype\n");
    printf("------------------------------\n");

    // Initialize Shims
    asset_manager_init("assets/resources");
    
    JNIEnv env;
    jni_init_env(&env);
    
    JavaVM vm;
    jni_init_vm(&vm);

    // Setup Dynamic Library Table (Imports for libswordigo.so)
    so_default_dynlib default_libs[] = {
        {"__android_log_print", (uintptr_t)__android_log_print},
        {"AAssetManager_fromJava", (uintptr_t)AAssetManager_fromJava},
        {"AAssetManager_open", (uintptr_t)AAssetManager_open},
        {"AAsset_read", (uintptr_t)AAsset_read},
        {"AAsset_close", (uintptr_t)AAsset_close},
        {"AAsset_getLength", (uintptr_t)AAsset_getLength},
        // Add more as needed
    };

    // Load Library
    so_module mod;
    const char* so_path = "reference/lib/armeabi-v7a/libswordigo.so";
    printf("Loading %s...\n", so_path);
    if (so_file_load(&mod, so_path, 0) < 0) {
        fprintf(stderr, "Failed to load library!\n");
        return 1;
    }

    printf("Relocating...\n");
    so_relocate(&mod);

    printf("Resolving imports...\n");
    so_resolve(&mod, default_libs, sizeof(default_libs));

    printf("Initializing...\n");
    so_initialize(&mod);

    // Locate JNI Exports
    void (*setFilesDir)(void *, void *, jstring) = (void *)so_symbol(&mod, "Java_com_touchfoo_swordigo_Native_setFilesDir");
    void (*setupNativeInterface)(void *, void *) = (void *)so_symbol(&mod, "Java_com_touchfoo_swordigo_Native_setupNativeInterface");
    void (*setupApplication)(void *, void *) = (void *)so_symbol(&mod, "Java_com_touchfoo_swordigo_Native_setupApplication");

    if (setFilesDir) printf("Found setFilesDir at %p\n", setFilesDir);
    if (setupNativeInterface) printf("Found setupNativeInterface at %p\n", setupNativeInterface);
    if (setupApplication) printf("Found setupApplication at %p\n", setupApplication);

    printf("\nStatus: Library loaded and symbols resolved.\n");
    printf("CRITICAL BLOCKER: ARMv7 execution is not possible on x86_64 without an emulator.\n");
    printf("Next step requires integrating Unicorn or QEMU user-mode.\n");

    return 0;
}
#endif /* dead legacy ARMv7 prototype — superseded by src/main.cpp */
