# ============================================================================
# Component: swcore — platform foundation (paths, IO thread, embedded assets,
# OS shims). Everything else depends on this.
# ============================================================================

swordigo_library(swcore
    ${SRC_DIR}/platform/data_path.cpp
    ${SRC_DIR}/platform/io_thread.cpp
    ${SRC_DIR}/platform/embedded_assets.cpp
    ${SRC_DIR}/platform/os_external.cpp
    ${SRC_DIR}/android/log.c
    ${SRC_DIR}/wincompat/pthread_mutex_shim.c)
target_link_libraries(swcore PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
# embedded_assets.cpp is a ~60MB generated TU of const byte arrays — no -O3 needed
if (MSVC)
    set_source_files_properties(${SRC_DIR}/platform/embedded_assets.cpp PROPERTIES COMPILE_OPTIONS "/Od")
else()
    set_source_files_properties(${SRC_DIR}/platform/embedded_assets.cpp PROPERTIES COMPILE_OPTIONS "-O0")
endif()
