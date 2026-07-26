# Swordigo OpenSwordigo Research: GroundMesh Editor Gizmos & Interactive Vertex Control

## 1. GroundMesh Editor Tool Architecture

Level designer tooling in OpenSwordigo provides interactive 2D polyline manipulation, real-time procedural mesh regeneration, vertex snapping, and editor gizmos for ground mesh creation.

---

## 2. Editor Data Structures & Control Handles

```cpp
#pragma once
#include <vector>
#include "caver/math/caver_math.h"
#include "caver/components/ground_polygon_component.h"
#include "caver/components/ground_mesh_generator_component.h"

namespace Caver {

enum class EditorGizmoType {
    None,
    VertexHandle,  // Circle handle for editing polyline vertex position
    EdgeAddHandle, // Inter-vertex handle for inserting new polyline points
    DepthHandle    // Slider handle for modifying min/max Z depth
};

struct SelectedVertexInfo {
    GroundPolygonComponent* target_polygon = nullptr;
    int vertex_index = -1;
    bool is_selected = false;
};

class GroundMeshEditorController {
public:
    void SelectVertex(GroundPolygonComponent* poly, int index) {
        m_selection.target_polygon = poly;
        m_selection.vertex_index = index;
        m_selection.is_selected = (poly != nullptr && index >= 0);
    }

    void DragSelectedVertex(const Vector2& new_position, bool snap_to_grid, float grid_size) {
        if (!m_selection.is_selected || !m_selection.target_polygon) return;

        Vector2 final_pos = new_position;
        if (snap_to_grid && grid_size > 0.0f) {
            final_pos.x = std::round(new_position.x / grid_size) * grid_size;
            final_pos.y = std::round(new_position.y / grid_size) * grid_size;
        }

        m_selection.target_polygon->vertices[m_selection.vertex_index] = final_pos;
        TriggerMeshRegeneration();
    }

    void InsertVertexAfter(int index) {
        if (!m_selection.target_polygon || index < 0) return;
        auto& verts = m_selection.target_polygon->vertices;
        if (index >= static_cast<int>(verts.size()) - 1) return;

        Vector2 mid_point = (verts[index] + verts[index + 1]) * 0.5f;
        verts.insert(verts.begin() + index + 1, mid_point);
        TriggerMeshRegeneration();
    }

    void DeleteSelectedVertex() {
        if (!m_selection.is_selected || !m_selection.target_polygon) return;
        auto& verts = m_selection.target_polygon->vertices;
        if (verts.size() <= 2) return; // Maintain minimum 2 points

        verts.erase(verts.begin() + m_selection.vertex_index);
        m_selection.is_selected = false;
        TriggerMeshRegeneration();
    }

private:
    SelectedVertexInfo m_selection;

    void TriggerMeshRegeneration() {
        // Re-execute GroundMeshGenerator::GenerateMesh in real-time
    }
};

} // namespace Caver
```

---

## 3. Boulder Engine Extractive Research: 3D Modeling Software Polyline Export

Level geometry can be authored inside 3D modeling tools (such as Blender) and exported directly into ground mesh polyline data blocks. The extraction pipeline queries face vertex coordinates in counter-clockwise order:

```python
# 3D Editor Vertex Extractor (Blender Python Script Bridge)
# Extracts XY vertex coordinates from selected 3D face loops in counter-clockwise order

import bpy
import bmesh

obj = bpy.context.edit_object
me = obj.data
bm = bmesh.from_edit_mesh(me)

faces = [f for f in bm.faces if f.select]
if len(faces) > 0:
    face = faces[0]
    result_text = "Vertex[\n"
    for v in face.verts:
        world_co = obj.matrix_world @ v.co
        result_text += f"    {world_co.x:.2f} {world_co.y:.2f}\n"
    result_text += "]\n"

    # Write formatted polyline block to editor text buffer
    text_block = bpy.data.texts.new(name="boulder_vertices.gmesh")
    text_block.write(result_text)
```
