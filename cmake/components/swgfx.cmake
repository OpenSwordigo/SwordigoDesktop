# ============================================================================
# Component: swgfx — rendering (FBO scaler, Vulkan backend, video bg, SRT overlay).
# ============================================================================

swordigo_library(swgfx
    ${SRC_DIR}/platform/fbo_scaler.cpp
    ${SRC_DIR}/platform/vulkan_backend.cpp
    ${SRC_DIR}/platform/video_background.cpp
    ${SRC_DIR}/platform/srt_overlay.cpp)
target_link_libraries(swgfx PRIVATE swcore swgui SDL3::SDL3 OpenGL::GL ${SWORDIGO_LIBM})
if (SWORDIGO_USE_FFMPEG)
    target_compile_definitions(swgfx PRIVATE "SWORDIGO_USE_FFMPEG=1")
else()
    target_compile_definitions(swgfx PRIVATE "SWORDIGO_USE_FFMPEG=0")
endif()
