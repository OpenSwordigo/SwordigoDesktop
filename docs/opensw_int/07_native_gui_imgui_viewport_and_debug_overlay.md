# Native ImGui Host Viewport & Debug Overlay Infrastructure

## 1. Executive Overview

This document specifies the integration between **Swordigo Desktop's** host UI framework (Dear ImGui) and OpenSwordigo's native FBO rendering pipeline.

---

## 2. Host Viewport & Live Scene Inspector Architecture

Because OpenSwordigo renders offscreen into a host-allocated Framebuffer Object (FBO) texture (`colorTex`), Swordigo Desktop renders the game inside an ImGui viewport window. This unlocks native developer tools:

- Live 3D Scene Graph Object Hierarchy Tree.
- Component Property Inspector (Position, Scale, Rotation, Depth).
- Ground Mesh Control Point Polygon Gizmo Editor.
- Real-time Texture & Shader Uniform Tweaker.

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Swordigo Desktop Host UI (Dear ImGui)                │
│                                                                        │
│  ┌───────────────────────┐  ┌───────────────────────────────────────┐  │
│  │ Scene Graph Tree      │  │ ImGui::Image(colorTex, ViewportSize)  │  │
│  │ ───                   │  │ ───                                   │  │
│  │ • SceneObject #149    │  │ [Live OpenSwordigo 3D Viewport Render]│  │
│  │   ├─ GroundMesh       │  │                                       │  │
│  │   └─ ModelComponent   │  │                                       │  │
│  └───────────────────────┘  └───────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 3. ImGui Viewport Docking Integration Code Schema

```cpp
void SwordigoDesktopHost::renderEditorUI(GLuint opensw_fbo_texture) {
    ImGui::Begin("OpenSwordigo 3D Viewport", nullptr, ImGuiWindowFlags_NoScrollbar);

    // Get current ImGui window dimensions
    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    // Resize OpenSwordigo rendering context if viewport resized
    if (viewportSize.x != current_vp_w || viewportSize.y != current_vp_h) {
        current_vp_w = (uint32_t)viewportSize.x;
        current_vp_h = (uint32_t)viewportSize.y;
        opensw_render_set_fbo_size(engine_ctx, current_vp_w, current_vp_h);
    }

    // Render offscreen OpenSwordigo FBO texture inside ImGui viewport window
    ImGui::Image((void*)(intptr_t)opensw_fbo_texture, viewportSize, ImVec2(0, 1), ImVec2(1, 0));

    ImGui::End();
}
```
