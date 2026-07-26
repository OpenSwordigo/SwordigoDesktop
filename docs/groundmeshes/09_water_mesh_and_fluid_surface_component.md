# Swordigo OpenSwordigo Research: WaterMeshComponent & Fluid Surface Geometry

## 1. Water Mesh System Overview

Water bodies, rivers, and acid/lava pools are procedurally generated using `WaterMeshComponent`, which extends ground polyline bounds into animated fluid surface quads.

---

## 2. WaterMeshComponent Schema and C++ Definition

```cpp
#pragma once
#include "caver/scene/component.h"
#include "caver/math/caver_math.h"

namespace Caver {

class WaterMeshComponent : public Component {
public:
    uint32_t bounds_shape_id = 0;        // Tag 0x08: Target ShapeComponent ID
    uint32_t texture_mapping_id = 0;     // Tag 10: Linked TextureMappingComponent ID
    
    FloatColor surface_color{ 0.2f, 0.5f, 0.9f, 0.7f }; // Tag 22: Water surface color tint
    FloatColor front_color{ 0.1f, 0.3f, 0.7f, 0.9f };   // Tag 1A: Underwater front face tint

    float wave_amplitude = 0.15f;
    float wave_frequency = 2.0f;
    float wave_speed = 3.0f;

    const char* GetComponentType() const override { return "WaterMeshComponent"; }

    void OnUpdate(float dt) override {
        m_time_accumulator += dt;
    }

    void GetWaveUVOffset(Vector2& out_offset) const {
        out_offset.x = std::sin(m_time_accumulator * wave_speed) * wave_amplitude;
        out_offset.y = std::cos(m_time_accumulator * wave_speed * 0.8f) * wave_amplitude;
    }

private:
    float m_time_accumulator = 0.0f;
};

} // namespace Caver
```
