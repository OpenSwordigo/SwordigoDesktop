# ============================================================================
# Component: xpera — Native retained-mode persistent GUI framework
# ============================================================================

set(XPERA_SOURCES
    ${SRC_DIR}/xpera/native/core/object.cpp
    ${SRC_DIR}/xpera/native/rendering/font.cpp
    ${SRC_DIR}/xpera/native/rendering/draw_list.cpp
    ${SRC_DIR}/xpera/native/rendering/renderer_opengl.cpp
    ${SRC_DIR}/xpera/native/theme/style_box.cpp
    ${SRC_DIR}/xpera/native/theme/theme.cpp
    ${SRC_DIR}/xpera/native/ui/widget.cpp
    ${SRC_DIR}/xpera/native/ui/ui_context.cpp
    ${SRC_DIR}/xpera/native/widgets/panel.cpp
    ${SRC_DIR}/xpera/native/widgets/label.cpp
    ${SRC_DIR}/xpera/native/widgets/button.cpp
    ${SRC_DIR}/xpera/native/widgets/check_box.cpp
    ${SRC_DIR}/xpera/native/widgets/slider.cpp
    ${SRC_DIR}/xpera/native/widgets/number_scrubber.cpp
    ${SRC_DIR}/xpera/native/widgets/progress_bar.cpp
    ${SRC_DIR}/xpera/native/widgets/line_edit.cpp
    ${SRC_DIR}/xpera/native/widgets/text_edit.cpp
    ${SRC_DIR}/xpera/native/widgets/containers.cpp
    ${SRC_DIR}/xpera/native/widgets/split_container.cpp
    ${SRC_DIR}/xpera/native/widgets/tree.cpp
    ${SRC_DIR}/xpera/native/widgets/tab_container.cpp
)

swordigo_library(xpera ${XPERA_SOURCES})
target_include_directories(xpera PUBLIC ${SRC_DIR})
target_link_libraries(xpera PRIVATE OpenGL::GL ${SWORDIGO_LIBM})

if (BUILD_TESTING)
    add_executable(xpera_demo tests/xpera_demo.cpp)
    target_include_directories(xpera_demo PRIVATE ${SRC_DIR})
    target_link_libraries(xpera_demo PRIVATE xpera OpenGL::GL ${SWORDIGO_LIBM})
    add_test(NAME xpera_demo COMMAND xpera_demo)
endif()

add_executable(gui_test
    ${SRC_DIR}/tools/gui_test.cpp
    ${SRC_DIR}/xpera/compat/godot_bridge.cpp
)
target_include_directories(gui_test PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/xpera/compat
    ${SRC_DIR}/xpera/native
)
target_link_libraries(gui_test PRIVATE xpera SDL3::SDL3 OpenGL::GL ${SWORDIGO_LIBM})

if (BUILD_TESTING)
    add_test(NAME gui_test COMMAND gui_test --headless)
endif()


