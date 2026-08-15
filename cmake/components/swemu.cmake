# ============================================================================
# Component: swemu — the emulation core. Loads the guest ELF, hosts the
# Unicorn (optional) and Dynarmic (default) ARM64 backends, and the JNI-style
# guest bridge.
# ============================================================================

swordigo_library(swemu
    ${SRC_DIR}/loader/elf_loader.cpp
    ${SRC_DIR}/loader/elf_loader_arm64.cpp
    ${SRC_DIR}/platform/emulator.cpp
    ${SRC_DIR}/platform/emulator_arm64.cpp
    ${SRC_DIR}/platform/unicorn_dyn.cpp
    ${SRC_DIR}/srehost/srehost_impl.cpp
    ${SRC_DIR}/jni/jni_bridge.cpp
    ${SRC_DIR}/jni/jni_bridge_arm64.cpp
    # jni_marshaller.cpp removed from the build: it is a dead mockup (hardcoded
    # fake FindClass/GetMethodID ids) that nothing references — real JNI dispatch
    # lives in jni_bridge_arm64.cpp. File left on disk, just not compiled.
    ${SRC_DIR}/platform/rgc.cpp
    ${SRC_DIR}/android/asset_manager.c
    ${SRC_DIR}/android/asset_manager_arm32.c)
target_link_libraries(swemu PRIVATE swcore swgfx Threads::Threads ${CMAKE_DL_LIBS} ${SWORDIGO_LIBM})
if (UNICORN_LIBRARY)
    target_link_libraries(swemu PRIVATE ${UNICORN_LIBRARY})
endif()
if (SWORDIGO_USE_DYNARMIC)
    target_sources(swemu PRIVATE ${SRC_DIR}/platform/emulator_dynarmic64.cpp)
    target_sources(swemu PRIVATE ${SRC_DIR}/platform/emulator_dynarmic32.cpp)
endif()
