# Swordigo OpenSwordigo Research: GroundMesh GLES2 Rendering Pipeline & Vertex Buffers

## 1. OpenGL Hardware Buffer Upload

`GroundMeshComponent` compiles generated vertex and index arrays into native OpenGL Vertex Buffer Objects (VBO) and Element Array Buffer Objects (EBO) during scene loading.

---

## 2. C++ Renderer Implementation

```cpp
#pragma once
#include "caver/graphics/gl_inc.h"
#include "caver/graphics/rendering_context.h"
#include "caver/components/ground_mesh_component.h"

namespace Caver {

class GroundMeshRenderer {
public:
    static void UploadToGPU(GroundMeshComponent& ground_comp) {
        // Upload Surface Mesh
        if (!ground_comp.surface_mesh.positions.empty()) {
            glGenBuffers(1, &ground_comp.surface_mesh.vbo_id);
            glBindBuffer(GL_ARRAY_BUFFER, ground_comp.surface_mesh.vbo_id);
            glBufferData(GL_ARRAY_BUFFER, 
                         ground_comp.surface_mesh.positions.size() * sizeof(float),
                         ground_comp.surface_mesh.positions.data(), 
                         GL_STATIC_DRAW);

            glGenBuffers(1, &ground_comp.surface_mesh.ebo_id);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ground_comp.surface_mesh.ebo_id);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, 
                         ground_comp.surface_mesh.indices.size() * sizeof(uint16_t),
                         ground_comp.surface_mesh.indices.data(), 
                         GL_STATIC_DRAW);
        }
    }

    static void Render(GroundMeshComponent& ground_comp, RenderingContext& ctx) {
        if (ground_comp.transparent) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }

        // Draw Surface and Front Mesh layers
    }
};

} // namespace Caver
```
