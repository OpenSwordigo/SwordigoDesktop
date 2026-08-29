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
# ImGui's Vulkan backend is compiled with VK_NO_PROTOTYPES (dynamic loader),
# so it only needs the Vulkan *headers* at build time — the loader is resolved
# at runtime and must NOT be hard-linked. Link the full loader only when the
# Vulkan package is actually found (e.g. native Linux); otherwise fall back to
# just the headers target (vcpkg's Vulkan::Headers) so MinGW/Windows builds
# that ship only vulkan-headers still compile without an -lvulkan link error.
if (Vulkan_FOUND)
    target_link_libraries(swgui PRIVATE Vulkan::Vulkan)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(swgui PRIVATE Vulkan::Headers)
endif()
