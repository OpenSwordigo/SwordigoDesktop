// ============================================================================
// ruby_editor_shell.cpp — Swordigo Studio editor shell implementation
//
// Builds the complete editor layout using Godot's native Control nodes.
// No GDScript — pure C++ GDExtension.
// ============================================================================

#include "ruby_editor_shell.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/menu_bar.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_split_container.hpp>
#include <godot_cpp/variant/color.hpp>

using namespace godot;

// ============================================================================
// Registration
// ============================================================================

void RubyEditorShell::_bind_methods() {
    ClassDB::bind_method(D_METHOD("_build_menu_bar"), &RubyEditorShell::_build_menu_bar);
    ClassDB::bind_method(D_METHOD("_build_toolbar"), &RubyEditorShell::_build_toolbar);
    ClassDB::bind_method(D_METHOD("_build_left_panel"), &RubyEditorShell::_build_left_panel);
    ClassDB::bind_method(D_METHOD("_build_center_panel"), &RubyEditorShell::_build_center_panel);
    ClassDB::bind_method(D_METHOD("_build_right_panel"), &RubyEditorShell::_build_right_panel);
    ClassDB::bind_method(D_METHOD("_build_bottom_panel"), &RubyEditorShell::_build_bottom_panel);
    ClassDB::bind_method(D_METHOD("_build_status_bar"), &RubyEditorShell::_build_status_bar);
    ClassDB::bind_method(D_METHOD("_open_file", "path"), &RubyEditorShell::_open_file);
    ClassDB::bind_method(D_METHOD("_refresh_file_browser"), &RubyEditorShell::_refresh_file_browser);
}

RubyEditorShell::RubyEditorShell() {
}

RubyEditorShell::~RubyEditorShell() {
}

// ============================================================================
// _ready — Build the entire editor layout
// ============================================================================

void RubyEditorShell::_ready() {
    UtilityFunctions::print("[RubyEditor] Building editor shell...");

    // Fill the entire parent
    set_anchors_preset(Control::PRESET_FULL_RECT);
    set_v_size_flags(Control::SIZE_EXPAND_FILL);
    set_h_size_flags(Control::SIZE_EXPAND_FILL);

    // ---- Main vertical layout: menu → toolbar → content → status ----
    main_vbox = memnew(VBoxContainer);
    main_vbox->set_anchors_preset(Control::PRESET_FULL_RECT);
    main_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    main_vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    add_child(main_vbox);

    // 1. Menu Bar
    _build_menu_bar();

    // 2. Toolbar
    _build_toolbar();

    // 3. Main content area (HSplit: left panel | center+right)
    main_hsplit = memnew(HSplitContainer);
    main_hsplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    main_hsplit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    main_hsplit->set_split_offset(250);
    main_vbox->add_child(main_hsplit);

    // Left panel (File Browser)
    _build_left_panel();

    // Center + Right split
    center_vsplit = memnew(VSplitContainer);
    center_vsplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    center_vsplit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    main_hsplit->add_child(center_vsplit);

    // 4. Center panel (3D Viewport)
    _build_center_panel();

    // 5. Bottom panel (logs/output) — below viewport
    _build_bottom_panel();

    // 6. Right panel (Inspector)
    _build_right_panel();

    // 7. Status Bar
    _build_status_bar();

    UtilityFunctions::print("[RubyEditor] Editor shell ready.");
}

// ============================================================================
// _process — Per-frame updates
// ============================================================================

void RubyEditorShell::_process(double /*delta*/) {
    // Future: update panels based on state changes
}

// ============================================================================
// Menu Bar
// ============================================================================

void RubyEditorShell::_build_menu_bar() {
    menu_bar = memnew(MenuBar);
    main_vbox->add_child(menu_bar);

    // File menu
    PopupMenu* file_menu = memnew(PopupMenu);
    file_menu->set_name("File");
    file_menu->add_item("Open File...", 0);
    file_menu->add_item("Open Folder...", 1);
    file_menu->add_separator();
    file_menu->add_item("Save", 10);
    file_menu->add_item("Save As...", 11);
    file_menu->add_separator();
    file_menu->add_item("Exit", 20);
    menu_bar->add_child(file_menu);

    // Edit menu
    PopupMenu* edit_menu = memnew(PopupMenu);
    edit_menu->set_name("Edit");
    edit_menu->add_item("Undo", 30);
    edit_menu->add_item("Redo", 31);
    edit_menu->add_separator();
    edit_menu->add_item("Preferences...", 40);
    menu_bar->add_child(edit_menu);

    // View menu
    PopupMenu* view_menu = memnew(PopupMenu);
    view_menu->set_name("View");
    view_menu->add_item("File Browser", 50);
    view_menu->add_item("Inspector", 51);
    view_menu->add_item("Output Log", 52);
    menu_bar->add_child(view_menu);

    // Tools menu
    PopupMenu* tools_menu = memnew(PopupMenu);
    tools_menu->set_name("Tools");
    tools_menu->add_item("Batch Convert...", 60);
    tools_menu->add_item("Scene Generator...", 61);
    tools_menu->add_item("MCP Console...", 62);
    menu_bar->add_child(tools_menu);

    // Help menu
    PopupMenu* help_menu = memnew(PopupMenu);
    help_menu->set_name("Help");
    help_menu->add_item("About Swordigo Studio", 70);
    menu_bar->add_child(help_menu);
}

// ============================================================================
// Toolbar
// ============================================================================

void RubyEditorShell::_build_toolbar() {
    toolbar = memnew(HBoxContainer);
    toolbar->set_custom_minimum_size(Size2(0, 32));
    main_vbox->add_child(toolbar);

    // Navigation buttons
    Button* btn_back = memnew(Button);
    btn_back->set_text("<");
    btn_back->set_custom_minimum_size(Size2(28, 28));
    toolbar->add_child(btn_back);

    Button* btn_forward = memnew(Button);
    btn_forward->set_text(">");
    btn_forward->set_custom_minimum_size(Size2(28, 28));
    toolbar->add_child(btn_forward);

    Button* btn_up = memnew(Button);
    btn_up->set_text("^");
    btn_up->set_custom_minimum_size(Size2(28, 28));
    toolbar->add_child(btn_up);

    // Breadcrumb path
    LineEdit* breadcrumb = memnew(LineEdit);
    breadcrumb->set_editable(false);
    breadcrumb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    breadcrumb->set_text("/");
    toolbar->add_child(breadcrumb);

    // Separator
    toolbar->add_child(memnew(VSeparator));

    // View mode buttons
    Button* btn_3d = memnew(Button);
    btn_3d->set_text("3D");
    btn_3d->set_custom_minimum_size(Size2(40, 28));
    toolbar->add_child(btn_3d);

    Button* btn_2d = memnew(Button);
    btn_2d->set_text("2D");
    btn_2d->set_custom_minimum_size(Size2(40, 28));
    toolbar->add_child(btn_2d);

    Button* btn_text = memnew(Button);
    btn_text->set_text("Text");
    btn_text->set_custom_minimum_size(Size2(40, 28));
    toolbar->add_child(btn_text);
}

// ============================================================================
// Left Panel — File Browser
// ============================================================================

void RubyEditorShell::_build_left_panel() {
    left_vsplit = memnew(VSplitContainer);
    left_vsplit->set_custom_minimum_size(Size2(220, 0));
    left_vsplit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    left_vsplit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    main_hsplit->add_child(left_vsplit);

    VBoxContainer* left_vbox = memnew(VBoxContainer);
    left_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    left_vsplit->add_child(left_vbox);

    // Search bar
    file_search = memnew(LineEdit);
    file_search->set_placeholder("Search files...");
    file_search->set_clear_button_enabled(true);
    left_vbox->add_child(file_search);

    // File tree
    file_tree = memnew(Tree);
    file_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    file_tree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    file_tree->set_hide_root(true);
    left_vbox->add_child(file_tree);

    // Populate with workspace files
    _refresh_file_browser();
}

// ============================================================================
// Center Panel — 3D Viewport
// ============================================================================

void RubyEditorShell::_build_center_panel() {
    viewport_container = memnew(SubViewportContainer);
    viewport_container->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    viewport_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    viewport_container->set_stretch(true);
    center_vsplit->add_child(viewport_container);

    viewport = memnew(SubViewport);
    viewport->set_size(Vector2i(800, 600));
    viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_container->add_child(viewport);

    // Editor camera
    editor_camera = memnew(Camera3D);
    editor_camera->set_position(Vector3(0, 5, 10));
    editor_camera->look_at(Vector3(0, 0, 0));
    editor_camera->set_fov(60);
    viewport->add_child(editor_camera);

    // Placeholder: grid floor
    // TODO (M2b): Add actual Swordigo scene rendering here
    UtilityFunctions::print("[RubyEditor] 3D Viewport created (placeholder camera)");
}

// ============================================================================
// Bottom Panel — Output Log
// ============================================================================

void RubyEditorShell::_build_bottom_panel() {
    bottom_tabs = memnew(TabContainer);
    bottom_tabs->set_custom_minimum_size(Size2(0, 150));
    bottom_tabs->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    center_vsplit->add_child(bottom_tabs);

    // Output tab
    output_log = memnew(RichTextLabel);
    output_log->set_name("Output");
    output_log->set_bbcode_enabled(true);
    output_log->set_scroll_following(true);
    output_log->append_text("[color=gray]Swordigo Studio — Output Log[/color]\n");
    output_log->append_text("[color=green]Ready.[/color]\n");
    bottom_tabs->add_child(output_log);

    // Errors tab
    RichTextLabel* error_log = memnew(RichTextLabel);
    error_log->set_name("Errors");
    error_log->set_bbcode_enabled(true);
    bottom_tabs->add_child(error_log);
}

// ============================================================================
// Right Panel — Inspector
// ============================================================================

void RubyEditorShell::_build_right_panel() {
    VBoxContainer* right_vbox = memnew(VBoxContainer);
    right_vbox->set_custom_minimum_size(Size2(260, 0));
    right_vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    right_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    main_hsplit->add_child(right_vbox);

    // Inspector title
    inspector_title = memnew(Label);
    inspector_title->set_text("Inspector");
    inspector_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    right_vbox->add_child(inspector_title);

    // Separator
    right_vbox->add_child(memnew(HSeparator));

    // Inspector tree
    inspector_tree = memnew(Tree);
    inspector_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    inspector_tree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    right_vbox->add_child(inspector_tree);

    // Populate with placeholder content
    TreeItem* root = inspector_tree->create_item();
    root->set_text(0, "No Selection");

    TreeItem* info = inspector_tree->create_item(root);
    info->set_text(0, "Select an object to inspect");
    info->set_custom_color(0, Color(0.5f, 0.5f, 0.5f));
}

// ============================================================================
// Status Bar
// ============================================================================

void RubyEditorShell::_build_status_bar() {
    status_bar = memnew(HBoxContainer);
    status_bar->set_custom_minimum_size(Vector2(0, 24));
    main_vbox->add_child(status_bar);

    status_left = memnew(Label);
    status_left->set_text("Ready");
    status_left->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    status_bar->add_child(status_left);

    status_bar->add_child(memnew(VSeparator));

    status_right = memnew(Label);
    status_right->set_text("Swordigo Studio v0.1.0 | Godot 4.7");
    status_bar->add_child(status_right);
}

// ============================================================================
// File Operations
// ============================================================================

void RubyEditorShell::_refresh_file_browser() {
    if (!file_tree) return;

    file_tree->clear();
    TreeItem* root = file_tree->create_item();
    root->set_text(0, "workspace");

    // TODO (M2b): Scan Swordigo workspace directory and populate tree
    // For now, show a placeholder
    TreeItem* placeholder = file_tree->create_item(root);
    placeholder->set_text(0, "(workspace not set)");
    placeholder->set_custom_color(0, Color(0.5f, 0.5f, 0.5f));

    UtilityFunctions::print("[RubyEditor] File browser refreshed (placeholder)");
}

void RubyEditorShell::_open_file(const String& path) {
    current_file = path;
    UtilityFunctions::print("[RubyEditor] Opening: ", path);

    if (status_left) {
        status_left->set_text("Opened: " + path);
    }

    // TODO (M2b): Route to appropriate viewer based on file extension
    // .pod/.POD → 3D model viewer
    // .pvr/.PVR → texture viewer
    // .scene    → scene editor
    // .scl      → text viewer
    // .fnt      → font viewer
    // etc.
}
