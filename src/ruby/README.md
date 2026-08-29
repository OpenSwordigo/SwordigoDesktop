# Ruby / Swordigo Studio (`ruby_gg`) — Godot-Powered Frontend

> **Role & Philosophy:** Godot is used as the **native UI and rendering substrate** (a modernized, full-featured replacement for ImGui), while **Ruby remains the application and authority**.

---

## 1. Project Concept

In previous versions, Ruby / Swordigo Studio used:
- **ImGui** for UI windows, menus, property trees, docking
- **Custom OpenGL 3.3** render loops for 3D model/scene viewports, wireframe, grid, textures
- **SDL3** for window management and input handling

In `ruby_gg`:
- **Ruby owns the application, data models, state, and Swordigo formats (`src/tools/*`).**
- **Godot (`libgodot.so` / `godot.dll`) replaces ImGui + custom OpenGL rendering.**
- Godot acts as an advanced GUI, 3D viewport, gizmo, text editor, and scene rendering backend.
- We do **not** convert Swordigo data into Godot game projects (`.tscn` / `.gd`). Swordigo formats (`.scene`, `.POD`, `.PVR`, `.scl`, `.fnt`, `.scmap`) are parsed and serialized entirely by Ruby's native backend.

```
                    ruby_gg (Executable)
                             │
            ┌────────────────┴────────────────┐
            ▼                                 ▼
   Ruby / Swordigo Core                 Godot Substrate
     (`src/tools/*`)                     (`libgodot`)
  ─────────────────────              ───────────────────
  • POD 3D Models                    • Window & Viewport
  • PVR / Tex Textures               • UI Controls & Docking
  • .scene Entity Hierarchy          • 3D Rendering & Lighting
  • Collision / Physics / Terrain    • Gizmos & Selection
  • Lua Scripting / Schemas          • Text Editor & Inspector
  • CLI / MCP Tools & Exporters      • Event & Input System
```

---

## 2. Directory Architecture

```
src/
├── ruby/
│   ├── README.md                 # This architecture & technical reference
│   ├── ruby_gg.cpp               # Main application entry point & UI controller
│   ├── editor/
│   │   ├── ruby_editor_shell.h   # Editor shell components & widget hierarchy
│   │   └── ruby_editor_shell.cpp # C++ UI creation & layout logic
│   └── godot_bridge/
│       ├── libgodot_host.h       # Godot library lifecycle host definitions
│       └── libgodot_host.cpp     # Dynamic loader & symbol resolver for libgodot
│
└── tools/                        # Authoritative Swordigo backend (UNTOUCHED / SHARED)
    ├── pod_loader.* / pod_writer.*
    ├── scene_loader.* / scene_entity.* / scene_creator.* / scene_collision.*
    ├── pvr / texture decoders
    ├── gltf_* / fbx_* / obj_*
    ├── filerift.* / batch_converter.*
    └── ruby_cli.* / ruby_mcp.*
```

---

## 3. How Godot is Hosted

### A. Dynamic Initialization (`godot_bridge/libgodot_host.cpp`)
`ruby_gg` loads `libgodot.so` via `dlopen` or links directly against it. The engine lifecycle is controlled through exported C entry points:
- `ruby_gg_setup(argc, argv)` — Initializes core Godot configuration.
- `ruby_gg_setup2()` — Completes subsystem initialization.
- `ruby_gg_start()` — Initializes SceneTree, Window, and Viewports.
- `ruby_gg_iteration()` — Steps 1 frame (Ruby drives the frame loop).
- `ruby_gg_cleanup()` — Shuts down the engine gracefully.

### B. Minimal Shim Project
Godot's editor/engine requires a valid project directory to start. At boot, `ruby_gg` ensures a tiny shim project exists in the user workspace (`.godot_shim/project.godot`). This is purely internal runtime scaffolding so Godot initializes cleanly; no game assets or Swordigo files reside here.

### C. Direct C++ Widget Construction
UI is constructed programmatically using Godot's C++ `Control` hierarchy (`MarginContainer`, `HSplitContainer`, `Tree`, `SubViewportContainer`, `MenuBar`, `TabContainer`, `TextEdit`, etc.) with dark styling and custom signal callbacks, avoiding unnecessary GDScript layers.

---

## 4. Asset Pipeline & Data Flow

```
[ Swordigo Files (.POD / .PVR / .scene) ]
                   │
                   ▼  (src/tools/* loader)
       [ Native C++ Data Structs ]
      (av::PODModel, SceneEntity, etc.)
                   │
                   ▼  (ruby_gg bridge conversion)
       [ Godot Visual Instances ]
   (ArrayMesh, StandardMaterial3D, Node3D)
                   │
                   ▼
  [ Godot SubViewport & Inspector Rendering ]
```

1. **Models (`.POD` / `.OBJ` / `.FBX`):** Parsed by `pod_loader` into vertex arrays, then fed to `ArrayMesh` via `add_surface_from_arrays` inside a `MeshInstance3D`.
2. **Textures (`.PVR` / `.PNG`):** Decoded by `pvr_loader` / `stb_image` and uploaded to `ImageTexture` / `StandardMaterial3D`.
3. **Scenes (`.scene`):** Parsed by `scene_loader`, populated as an interactive hierarchy in the Godot Viewport + Scene Tree.
4. **Mutations / Edits:** User manipulations in the 3D viewport or Inspector update the underlying Ruby `SceneEntity` state. Serializing back to `.scene` is handled by `scene_loader`.

---

## 5. Parity Goals with Legacy ImGui Edition

| Feature Area | Legacy (ImGui + Custom GL) | `ruby_gg` (Godot Frontend) | Status |
|---|---|---|---|
| **Window & Frame Loop** | SDL3 + OpenGL 3.3 Context | Godot Substrate Window (`SceneTree`) | ✅ Working |
| **Theme / Styling** | ImGui Custom Dark Theme | Godot Flat Dark Theme | ✅ Working |
| **File Browser** | ImGui Custom File Tree | Godot `Tree` + Color Classification | ✅ Working |
| **3D POD Model Viewer** | Custom Orbit Camera + GL VBOs | Godot `SubViewport` + `Camera3D` + `ArrayMesh` | ✅ Working |
| **Grid & Axes** | Custom GL Line Renderer | Godot `ImmediateMesh` Lines | ✅ Working |
| **Log / Output Console** | ImGui Scrolling Text Box | Godot `TextEdit` Output Panel | ✅ Working |
| **Menu Bar Actions** | ImGui Menus | Godot `MenuBar` + Signal Handlers | 🔄 In Progress |
| **Interactive Viewport** | Mouse drag / pan / zoom in window | SubViewport input capture & orbit/pan/zoom | 🔄 In Progress |
| **PVR / Texture Preview** | Custom 2D Texture Viewer | 2D/3D Texture Inspector & Preview | ⏳ Planned |
| **Scene Entity Hierarchy** | ImGui Collapsing Headers | Interactive `Tree` + Node selection | ⏳ Planned |
| **Transform Gizmos** | Custom Math / ImGuizmo | Godot 3D Transform Gizmos | ⏳ Planned |
| **Scene Text / SCL Editor** | ImGui Text Editor | Godot `TextEdit` with Syntax Highlighting | ⏳ Planned |
| **Audio WAV / OGG Player**| Custom OpenAL Thread | Integrated Audio Player | ⏳ Planned |

---

## 6. Critical Engineering Guidelines

1. **NEVER modify or break `src/tools/asset_viewer.cpp` or the existing ImGui implementation.** Both versions must coexist independently.
2. **Do not create normal Godot game projects or depend on `.gd` scripting.** Keep the architecture native C++.
3. **Ruby remains the authority.** Godot is the viewport and UI engine; all file formats and business logic belong to `src/tools/`.
