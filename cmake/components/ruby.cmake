# ============================================================================
# Component: ruby — the asset viewer tool (bin/ruby).
# ============================================================================

if (SWORDIGO_BUILD_RUBY)
    add_executable(ruby
        ${SRC_DIR}/tools/asset_viewer.cpp
        ${SRC_DIR}/tools/av_renderer.cpp
        ${SRC_DIR}/tools/av_audio.cpp
        ${SRC_DIR}/tools/scene_loader.cpp
        ${SRC_DIR}/tools/scene_workspace.cpp
        ${SRC_DIR}/tools/scene_player.cpp
        ${SRC_DIR}/tools/scene_lua.cpp
        ${SRC_DIR}/tools/scene_terrain.cpp
        ${SRC_DIR}/tools/scene_entity.cpp
        ${SRC_DIR}/tools/scene_collision.cpp
        ${SRC_DIR}/tools/scene_physics.cpp
        ${SRC_DIR}/tools/scene_game.cpp
        ${SRC_DIR}/tools/scene_generator.cpp
        ${SRC_DIR}/tools/intellij.cpp
        ${SRC_DIR}/tools/batch_converter.cpp
        ${SRC_DIR}/tools/gltf_export.cpp
        ${SRC_DIR}/tools/gltf_import.cpp
        ${SRC_DIR}/tools/obj_loader.cpp
        ${SRC_DIR}/tools/ani_loader.cpp
        ${SRC_DIR}/tools/scn_loader.cpp
        ${SRC_DIR}/tools/pod_writer.cpp
        ${SRC_DIR}/tools/fbx_import.cpp
        ${SRC_DIR}/tools/pod_convert.cpp
        ${SRC_DIR}/tools/ruby_mcp.cpp
        ${SRC_DIR}/tools/map_loader.cpp
        ${SRC_DIR}/tools/map_editor.cpp
        ${SRC_DIR}/imgui/Guizmo/src/ImGuizmo.cpp
        ${SRC_DIR}/tools/ufbx/ufbx.c)
    # Output lands in bin/ per the top-level CMakeLists output dirs.
    swordigo_target(ruby)
    target_include_directories(ruby PRIVATE ${SRC_DIR}/tools/ufbx)
    target_link_libraries(ruby PRIVATE
        swcore swgui swfmt swpod filerift SDL3::SDL3 ${SDL3_IMAGE_LINK} OpenGL::GL ZLIB::ZLIB ${SWORDIGO_LIBM} ${SWORDIGO_SOCKLIB})
    if (NOT WIN32)
        target_link_libraries(ruby PRIVATE util)
    endif()
endif()
