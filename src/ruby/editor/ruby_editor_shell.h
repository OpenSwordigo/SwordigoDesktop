#pragma once
// ============================================================================
// ruby_editor_shell.h — Swordigo Studio editor shell (Godot Control-based)
//
// Builds the editor layout programmatically using Godot's native Control nodes:
//   - MenuBar + Toolbar
//   - File Browser (left panel)
//   - 3D Viewport (center)
//   - Properties/Inspector (right panel)
//   - Bottom Panel (logs, output)
//   - Status Bar
//
// Registered as a GDExtension class so it boots automatically when the engine starts.
// ============================================================================

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

class RubyEditorShell : public Control {
    GDCLASS(RubyEditorShell, Control);

protected:
    static void _bind_methods();

public:
    RubyEditorShell();
    ~RubyEditorShell();

    void _ready() override;
    void _process(double delta) override;

    // ---- Panel builders (called once during _ready) ----
    void _build_menu_bar();
    void _build_toolbar();
    void _build_left_panel();      // File Browser
    void _build_center_panel();    // 3D Viewport
    void _build_right_panel();     // Properties/Inspector
    void _build_bottom_panel();    // Logs/Output
    void _build_status_bar();

    // ---- Panel update callbacks (called every frame) ----
    void _update_file_browser();
    void _update_viewport();
    void _update_inspector();
    void _update_status_bar();

    // ---- File operations ----
    void _open_file(const String& path);
    void _refresh_file_browser();

private:
    // Layout containers
    VBoxContainer* main_vbox = nullptr;
    HSplitContainer* main_hsplit = nullptr;
    VSplitContainer* center_vsplit = nullptr;

    // Menu
    MenuBar* menu_bar = nullptr;

    // Toolbar
    HBoxContainer* toolbar = nullptr;

    // Left panel: File Browser
    VSplitContainer* left_vsplit = nullptr;
    Tree* file_tree = nullptr;
    LineEdit* file_search = nullptr;

    // Center: Viewport
    SubViewportContainer* viewport_container = nullptr;
    SubViewport* viewport = nullptr;
    Camera3D* editor_camera = nullptr;

    // Right panel: Inspector
    Tree* inspector_tree = nullptr;
    Label* inspector_title = nullptr;

    // Bottom panel
    RichTextLabel* output_log = nullptr;
    TabContainer* bottom_tabs = nullptr;

    // Status bar
    HBoxContainer* status_bar = nullptr;
    Label* status_left = nullptr;
    Label* status_right = nullptr;

    // State
    String current_workspace;
    String current_file;
    bool needs_update = false;
};

} // namespace godot
