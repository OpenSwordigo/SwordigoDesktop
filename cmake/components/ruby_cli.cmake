# ============================================================================
# Component: ruby_cli — native FileRift-compatible command line tool
# (bin/ruby_cli). Links the filerift backend + headless batch converter.
# ============================================================================

if (SWORDIGO_BUILD_RUBY)
    add_executable(ruby_cli
        ${SRC_DIR}/tools/ruby_cli.cpp
        ${SRC_DIR}/tools/batch_converter.cpp
        ${SRC_DIR}/tools/ruby_mcp.cpp
        ${SRC_DIR}/tools/map_loader.cpp
        ${SRC_DIR}/tools/scene_workspace.cpp
        ${SRC_DIR}/tools/av_renderer.cpp)
    target_compile_definitions(ruby_cli PRIVATE BATCH_CONVERTER_NO_UI)
    # Output lands in bin/ per the top-level CMakeLists output dirs.
    swordigo_target(ruby_cli)
    target_link_libraries(ruby_cli PRIVATE
        swcore swfmt swpod filerift OpenGL::GL ZLIB::ZLIB ${SWORDIGO_LIBM} ${SWORDIGO_SOCKLIB})
endif()
