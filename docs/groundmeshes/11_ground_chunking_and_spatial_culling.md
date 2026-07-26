# Swordigo OpenSwordigo Research: Spatial Chunking, AABB Bounds & View Frustum Culling

## 1. Spatial Partitioning Architecture

Large level maps divide continuous ground terrain into spatial chunks. Each `GroundMeshComponent` computes a local Axis-Aligned Bounding Box (`LocalAABB`), allowing the engine to frustum-cull invisible terrain sections before issuing GPU draw calls.

---

## 2. Spatial Culling Implementation

```cpp
#pragma once
#include <vector>
#include "caver/math/caver_math.h"
#include "caver/components/ground_mesh_component.h"

namespace Caver {

struct ViewFrustum {
    Rectangle camera_viewport;

    bool Intersects(const Rectangle& bounds) const {
        return camera_viewport.Intersects(bounds);
    }
};

class GroundTerrainChunkManager {
public:
    void RegisterGroundChunk(GroundMeshComponent* ground_comp) {
        if (ground_comp) {
            m_chunks.push_back(ground_comp);
        }
    }

    void RenderVisibleChunks(RenderingContext& ctx, const ViewFrustum& frustum) {
        uint32_t rendered_count = 0;
        uint32_t culled_count = 0;

        for (auto* chunk : m_chunks) {
            if (!chunk || !chunk->GetOwner() || chunk->GetOwner()->hidden) continue;

            // Calculate world AABB
            Vector2 pos = chunk->GetOwner()->position;
            Rectangle world_aabb(
                pos.x + chunk->local_aabb.x,
                pos.y + chunk->local_aabb.y,
                chunk->local_aabb.width,
                chunk->local_aabb.height
            );

            // Frustum Culling Test
            if (frustum.Intersects(world_aabb)) {
                // Issue GL Draw Calls
                rendered_count++;
            } else {
                culled_count++;
            }
        }
    }

private:
    std::vector<GroundMeshComponent*> m_chunks;
};

} // namespace Caver
```
