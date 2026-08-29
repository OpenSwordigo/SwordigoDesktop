# ============================================================================
# Component: ruby_gg — Swordigo Studio frontend using Godot as native C++ lib
#
# Architecture:
#   ruby_gg  ->  Godot C++ API (like ImGui/Qt)
#                    |
#               libgodot.so (link-time)
#                    |
#               GUI + Rendering + SceneTree
#
# Swordigo backends (src/tools/) are linked, not copied.
# ImGui-dependent backends excluded (they belong to the old ruby executable).
# ============================================================================

if (SWORDIGO_BUILD_RUBY_GG)
    # ---- Godot source tree (for headers only) ----
    set(GODOT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/godot-4.7.2-stable")
    set(GODOT_LIB_DIR "${CMAKE_SOURCE_DIR}/bin/libs")

    add_executable(ruby_gg
        ${SRC_DIR}/ruby/ruby_gg.cpp
        # ---- Swordigo backends (no ImGui deps) ----
        ${SRC_DIR}/tools/pod_loader.cpp
        ${SRC_DIR}/tools/pod_writer.cpp
        ${SRC_DIR}/tools/pod_convert.cpp
        ${SRC_DIR}/tools/scene_loader.cpp
        ${SRC_DIR}/tools/scene_workspace.cpp
        ${SRC_DIR}/tools/scene_entity.cpp
        ${SRC_DIR}/tools/scene_physics.cpp
        ${SRC_DIR}/tools/scene_collision.cpp
        ${SRC_DIR}/tools/scene_terrain.cpp
        ${SRC_DIR}/tools/scene_lua.cpp
        ${SRC_DIR}/tools/scene_game.cpp
        ${SRC_DIR}/tools/scene_schemas.cpp
        ${SRC_DIR}/tools/scene_creator.cpp
        ${SRC_DIR}/tools/obj_loader.cpp
        ${SRC_DIR}/tools/ani_loader.cpp
        ${SRC_DIR}/tools/scn_loader.cpp
        ${SRC_DIR}/tools/map_loader.cpp
        ${SRC_DIR}/tools/gltf_export.cpp
        ${SRC_DIR}/tools/gltf_import.cpp
        ${SRC_DIR}/tools/fbx_import.cpp
        ${SRC_DIR}/tools/filerift.cpp
        ${SRC_DIR}/tools/intellij.cpp

        ${SRC_DIR}/tools/batch_converter.cpp
        # Scene Generators (V1 / V2 / V3 / V2-3D) — headless, no ImGui
        ${SRC_DIR}/tools/scene_generator.cpp
        ${SRC_DIR}/tools/scene_generator_v2.cpp
        ${SRC_DIR}/tools/scene_generator_v3.cpp
        ${SRC_DIR}/tools/scene_generator_v2_3d.cpp
        ${SRC_DIR}/tools/scene_v3_db.cpp
        ${SRC_DIR}/tools/boulder.cpp
        ${SRC_DIR}/tools/ufbx/ufbx.c
        ${SRC_DIR}/platform/win_dll_dir.cpp
        ${SRC_DIR}/platform/pvr_loader.cpp
        ${SRC_DIR}/platform/pvrtc_decoder.cpp
    )

    # Keep the Godot frontend's headless/editor helpers explicit. Some CMake
    # generators drop sources after a duplicate backend filename while
    # reconfiguring an existing build tree.
    target_sources(ruby_gg PRIVATE
        ${SRC_DIR}/tools/intellij.cpp
        ${SRC_DIR}/tools/scene_generator.cpp
        ${SRC_DIR}/tools/scene_generator_v2.cpp
        ${SRC_DIR}/tools/scene_generator_v3.cpp
        ${SRC_DIR}/tools/scene_generator_v2_3d.cpp
        ${SRC_DIR}/tools/scene_v3_db.cpp
    )


    swordigo_target(ruby_gg)
    target_compile_definitions(ruby_gg PRIVATE SWORDIGO_NO_IMGUI GODOT_ENABLED BATCH_CONVERTER_NO_UI)


    # ---- Include paths ----
    target_include_directories(ruby_gg PRIVATE
        # Godot headers (the whole tree, like Qt includes)
        ${GODOT_SOURCE_DIR}
        ${GODOT_SOURCE_DIR}/platform/linuxbsd
        ${GODOT_SOURCE_DIR}/thirdparty
        # Swordigo backends
        ${SRC_DIR}/ruby
        ${SRC_DIR}/tools
        ${SRC_DIR}/tools/ufbx
        ${SRC_DIR}/stb
        ${SRC_DIR}
    )

    # ---- Link against libgodot.so directly (like linking Qt/ImGui) ----
    target_link_directories(ruby_gg PRIVATE ${GODOT_LIB_DIR})
    target_link_libraries(ruby_gg PRIVATE
        godot  # libgodot.so
        swcore swfmt swpod filerift
        OpenGL::GL ZLIB::ZLIB
        ${SWORDIGO_LIBM} ${SWORDIGO_SOCKLIB}
    )

    # Godot needs SDL3 for display server
    target_link_libraries(ruby_gg PRIVATE SDL3::SDL3)

    if (NOT WIN32)
        target_link_libraries(ruby_gg PRIVATE dl util pthread)
    endif()

    # RPATH so libgodot.so is found at runtime
    set_target_properties(ruby_gg PROPERTIES
        BUILD_RPATH "${GODOT_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/libs"
    )
endif()
