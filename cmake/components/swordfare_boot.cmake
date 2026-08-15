# ============================================================================
# Component: swordfare_boot — the main launcher executable (bin/swordfare).
# Static FFmpeg + all component libraries get linked into this one binary.
# ============================================================================

add_executable(swordfare_boot
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/platform/arm64_reloc.cpp
    ${SRC_DIR}/platform/display.cpp
    ${SRC_DIR}/platform/loading_screen.cpp
    ${SRC_DIR}/platform/crash_dialog.cpp
    ${SRC_DIR}/platform/openswordigo_host.cpp
    ${SRC_DIR}/platform/gui.cpp
    ${SRC_DIR}/platform/input_config.cpp
    ${SRC_DIR}/platform/binary_selector.cpp)
set_target_properties(swordfare_boot PROPERTIES OUTPUT_NAME swordfare)
swordigo_target(swordfare_boot)

target_link_libraries(swordfare_boot PRIVATE
    swcore swgui swfmt swpod filerift swgfx swemu swordfare
    SDL3::SDL3 ${SDL3_IMAGE_LINK} ${VORBISFILE_LIBRARIES} ${MPG123_LIBRARIES}
    ZLIB::ZLIB OpenAL::OpenAL OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS} ${SWORDIGO_LIBM} ${SWORDIGO_SOCKLIB})

# Static FFmpeg: on GNU linkers pull the archives in whole so their symbols are
# visible to the shared component libs; MSVC uses /WHOLEARCHIVE.
# On Windows, SWORDIGO_USE_FFMPEG links the bundled static build; when disabled
# the video-background FFmpeg code is compiled out (see video_background.cpp).
if (SWORDIGO_USE_FFMPEG)
    if (MSVC)
        target_link_options(swordfare_boot PRIVATE
            /WHOLEARCHIVE:${FFMPEG_LIB_DIR}/libavformat.a
            /WHOLEARCHIVE:${FFMPEG_LIB_DIR}/libavcodec.a
            /WHOLEARCHIVE:${FFMPEG_LIB_DIR}/libswscale.a
            /WHOLEARCHIVE:${FFMPEG_LIB_DIR}/libavutil.a)
    else()
        target_link_libraries(swordfare_boot PRIVATE
            -Wl,--whole-archive
            ${FFMPEG_LIB_DIR}/libavformat.a
            ${FFMPEG_LIB_DIR}/libavcodec.a
            ${FFMPEG_LIB_DIR}/libswscale.a
            ${FFMPEG_LIB_DIR}/libavutil.a
            -Wl,--no-whole-archive)
        # GNU-ld only flags. MSVC linker has no rdynamic/allow-shlib-undefined.
        target_link_options(swordfare_boot PRIVATE -rdynamic -Wl,--allow-shlib-undefined)
    endif()
endif()

if (SWORDIGO_USE_DYNARMIC)
    if (MSVC)
        # MSVC: multi-config build produces the libs under a <Config>/ subdir.
        set(_dyn_cfg "$<UPPER_CASE:$<CONFIG>>")
        target_link_libraries(swordfare_boot PRIVATE
            ${SWORDIGO_DYNARMIC_ROOT}/src/dynarmic/$<UPPER_CASE:$<CONFIG>>/dynarmic.lib
            ${SWORDIGO_DYNARMIC_ROOT}/externals/mcl/src/$<UPPER_CASE:$<CONFIG>>/mcl.lib
            ${SWORDIGO_DYNARMIC_ROOT}/externals/fmt/$<UPPER_CASE:$<CONFIG>>/fmt.lib
            ${SWORDIGO_DYNARMIC_ROOT}/externals/zydis/$<UPPER_CASE:$<CONFIG>>/Zydis.lib
            ${SWORDIGO_DYNARMIC_ROOT}/externals/zydis/zycore/$<UPPER_CASE:$<CONFIG>>/Zycore.lib)
    else()
        target_link_libraries(swordfare_boot PRIVATE
            ${SWORDIGO_DYNARMIC_ROOT}/src/dynarmic/libdynarmic.a
            ${SWORDIGO_DYNARMIC_ROOT}/externals/mcl/src/libmcl.a
            ${SWORDIGO_DYNARMIC_ROOT}/externals/fmt/libfmt.a
            ${SWORDIGO_DYNARMIC_ROOT}/externals/zydis/libZydis.a
            ${SWORDIGO_DYNARMIC_ROOT}/externals/zydis/zycore/libZycore.a)
    endif()
endif()
