# ============================================================================
# Component: swordfare — the in-game overlay GUI (script editor, mods, launcher).
# ============================================================================

swordigo_library(swordfare
    ${SRC_DIR}/platform/swordfare_theme.cpp
    ${SRC_DIR}/platform/swordfare_gui.cpp
    ${SRC_DIR}/platform/launcher_ui.cpp
    ${SRC_DIR}/platform/save_editor.cpp
    ${SRC_DIR}/platform/scl_parser.cpp
    ${SRC_DIR}/platform/mod_manager.cpp
    ${SRC_DIR}/platform/mod_catalog_embedded.cpp
    ${SRC_DIR}/game/mod_tools.cpp
    ${SRC_DIR}/game/mod_config.cpp
    ${SRC_DIR}/game/save_editor_logic.cpp
    ${SRC_DIR}/game/camera_override.cpp)
target_link_libraries(swordfare PRIVATE swcore swgui filerift SDL3::SDL3 OpenGL::GL Threads::Threads ZLIB::ZLIB)
