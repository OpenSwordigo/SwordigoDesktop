# OptiX Technical Specification 06: Engine Graphics Function Hooking & Material Interception Inventory

## 1. Executive Overview

This specification provides the comprehensive reverse-engineered function signature inventory and address map required to gain full programmatic control over Touch Foo's rendering pipeline.

By placing 16-byte ARM64 SRE trampolines at key rendering dispatch entries isolated in Ghidra and IDA decompilation (`RenderingContext.c`, `MeshMaterial.c`, `ModelComponent.c`, `Camera.c`, `PointLightManager.c`), SRE OptiX intercepts material binding, geometry rendering, light buffer updates, and camera transformation matrices.

---

## 2. Complete Graphics Function Hooking Address Inventory

The addresses below correspond to `libswordigo_v1.4.12.so` (ARM64 `libswordigo.so` base `0x1000000`):

| SRE Hook Target Symbol | Virtual Address | Original Function Signature | Interception Purpose |
| :--- | :--- | :--- | :--- |
| `RenderingContext::SetProjectionMatrix` | `0x002fc103` | `void SetProjectionMatrix(RenderingContext* this, Matrix4x4* mat)` | Captures active 3D camera projection matrix for GLSL uniforms and SSAO/SSR depth reconstruction. |
| `RenderingContext::SetViewport` | `0x002fc103` | `void SetViewport(RenderingContext* this, Rectangle* rect)` | Overrides viewport bounds for G-Buffer render targets and high-DPI scaling. |
| `MeshMaterial::Bind` | `0x00332810` | `void Bind(MeshMaterial* this, RenderingContext* ctx)` | Intercepts material properties (ambient, diffuse, specular), injecting PBR roughness/metallic parameters. |
| `ModelComponent::Render` | `0x0032f410` | `void Render(ModelComponent* this, RenderingContext* ctx)` | Captures 3D character and monster model instances, injecting G-Buffer pass shaders. |
| `Mesh::Render` | `0x003261a4` | `void Render(Mesh* this, RenderingContext* ctx)` | Converts client-side CPU vertex arrays into persistent VBO/VAO draw calls. |
| `WaterMeshComponent::Render` | `0x00344d28` | `void Render(WaterMeshComponent* this, RenderingContext* ctx)` | Overrides 2D water plane rendering with real-time Screen Space Reflections (SSR). |
| `PointLightManager::UpdateLights` | `0x00339900` | `void UpdateLights(PointLightManager* this, float dt)` | Captures dynamic point light position, radius, and color arrays into uniform buffer blocks. |
| `Camera::GetViewMatrix` | `0x002e1b40` | `Matrix4x4* GetViewMatrix(Camera* this)` | Reconstructs view space transformation matrix for view-space normal and SSAO passes. |
| `TextureLibrary::TextureForName` | `0x004cc494` | `Texture* TextureForName(TextureLibrary* this, String* name, bool mipmap)` | Resolves companion PBR normal/roughness textures from VFS on texture load. |
| `ShadowVolumeComponent::Render` | `0x0033d5c0` | `void Render(ShadowVolumeComponent* this, RenderingContext* ctx)` | Disables legacy stencil shadow volume passes in favor of Cascaded Shadow Maps (CSM). |

---

## 3. Detailed Hook Implementation Code Examples

### 3.1 `MeshMaterial::Bind` Interception & PBR Parameter Injection

```cpp
typedef void (*orig_MeshMaterial_Bind_t)(void* material_this, void* ctx);
static orig_MeshMaterial_Bind_t g_orig_MeshMaterial_Bind = nullptr;

// SRE OptiX Trampoline Target
extern "C" void sre_MeshMaterial_Bind(void* material_this, void* ctx) {
    // Execute original binding logic to resolve diffuse texture binding
    if (g_orig_MeshMaterial_Bind) {
        g_orig_MeshMaterial_Bind(material_this, ctx);
    }

    // Inspect material struct fields (offset +0x18: specular exponent, +0x24: diffuse color)
    float specular_exponent = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(material_this) + 0x18);
    
    // Map legacy specular exponent to roughness parameter:
    // Roughness = sqrt(2 / (Specular Exponent + 2))
    float roughness = std::sqrt(2.0f / (specular_exponent + 2.0f));
    roughness = std::clamp(roughness, 0.05f, 1.0f);

    // Pass PBR uniform overrides to OptiX G-Buffer Shader Program
    GLint loc_roughness = glGetUniformLocation(g_optix_gbuffer_shader, "u_RoughnessOverride");
    if (loc_roughness != -1) {
        glUniform1f(loc_roughness, roughness);
    }
}
```

### 3.2 `PointLightManager::UpdateLights` Interception

```cpp
struct PointLightData {
    float pos[3];
    float radius;
    float color[3];
    float intensity;
};

// SRE OptiX Point Light Trampoline
extern "C" void sre_PointLightManager_UpdateLights(void* light_mgr, float dt) {
    // Read light vector from PointLightManager struct (offset +0x20: FastVector of PointLights)
    uintptr_t vec_ptr = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(light_mgr) + 0x20);
    uint32_t light_count = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(light_mgr) + 0x28);

    PointLightData light_buffer[16];
    uint32_t active_lights = std::min(light_count, 16U);

    for (uint32_t i = 0; i < active_lights; ++i) {
        uintptr_t light_obj = *reinterpret_cast<uintptr_t*>(vec_ptr + i * sizeof(uintptr_t));
        if (light_obj) {
            float* pos = reinterpret_cast<float*>(light_obj + 0x10);
            float* col = reinterpret_cast<float*>(light_obj + 0x20);
            float radius = *reinterpret_cast<float*>(light_obj + 0x30);

            std::memcpy(light_buffer[i].pos, pos, sizeof(float) * 3);
            std::memcpy(light_buffer[i].color, col, sizeof(float) * 3);
            light_buffer[i].radius = radius;
            light_buffer[i].intensity = 1.0f;
        }
    }

    // Upload light data to Uniform Buffer Object (UBO) for deferred lighting pass
    glBindBuffer(GL_UNIFORM_BUFFER, g_optix_light_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, active_lights * sizeof(PointLightData), light_buffer);
}
```

---

## 4. Hook Installation Sequence in `sre_init.c`

```c
void sre_init_optix_graphics_hooks(uintptr_t base_addr) {
    sre_install_trampoline(base_addr + 0x00332810, (uintptr_t)sre_MeshMaterial_Bind, (uintptr_t*)&g_orig_MeshMaterial_Bind);
    sre_install_trampoline(base_addr + 0x0032f410, (uintptr_t)sre_ModelComponent_Render, (uintptr_t*)&g_orig_ModelComponent_Render);
    sre_install_trampoline(base_addr + 0x00344d28, (uintptr_t)sre_WaterMeshComponent_Render, (uintptr_t*)&g_orig_WaterMeshComponent_Render);
    sre_install_trampoline(base_addr + 0x00339900, (uintptr_t)sre_PointLightManager_UpdateLights, (uintptr_t*)&g_orig_PointLightManager_UpdateLights);
}
```
