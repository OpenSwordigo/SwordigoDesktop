# ============================================================================
# Components: swfmt (PVR texture decode) and swpod (POD scene loading).
# ============================================================================

swordigo_library(swfmt
    ${SRC_DIR}/platform/pvr_loader.cpp
    ${SRC_DIR}/platform/pvrtc_decoder.cpp)
target_link_libraries(swfmt PRIVATE swcore ZLIB::ZLIB OpenGL::GL)

# scene_schemas.cpp lives only in swpod: defining av::g_schemas twice caused
# two destructor registrations for one interposed object -> double free at exit.
swordigo_library(swpod
    ${SRC_DIR}/tools/pod_loader.cpp
    ${SRC_DIR}/tools/av_renderer.cpp
    ${SRC_DIR}/tools/scene_loader.cpp
    ${SRC_DIR}/tools/scene_schemas.cpp
    ${SRC_DIR}/tools/scene_collision.cpp
    ${SRC_DIR}/tools/scene_entity.cpp
    ${SRC_DIR}/tools/scene_physics.cpp
    ${SRC_DIR}/tools/scene_game.cpp
    ${SRC_DIR}/tools/scene_terrain.cpp
    ${SRC_DIR}/tools/scene_workspace.cpp)
target_link_libraries(swpod PRIVATE swcore SDL3::SDL3 OpenGL::GL OpenAL::OpenAL)
