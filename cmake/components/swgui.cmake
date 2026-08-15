# ============================================================================
# Component: swgui — Dear ImGui core + SDL3/OpenGL/Vulkan backends.
# ============================================================================

swordigo_library(swgui
    ${SRC_DIR}/imgui/imgui.cpp
    ${SRC_DIR}/imgui/imgui_draw.cpp
    ${SRC_DIR}/imgui/imgui_tables.cpp
    ${SRC_DIR}/imgui/imgui_widgets.cpp
    ${SRC_DIR}/imgui/backends/imgui_impl_sdl3.cpp
    ${SRC_DIR}/imgui/backends/imgui_impl_opengl3.cpp
    ${SRC_DIR}/imgui/backends/imgui_impl_vulkan.cpp)
target_link_libraries(swgui PRIVATE swcore SDL3::SDL3 OpenGL::GL)
if (Vulkan_FOUND)
    target_link_libraries(swgui PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(swgui PRIVATE vulkan)
endif()
