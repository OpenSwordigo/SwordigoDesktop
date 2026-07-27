# OptiX Technical Specification 04: Deferred Rendering Pipeline & G-Buffer PBR Shader Injection

## 1. Executive Overview

This specification details the design of a **Deferred PBR (Physically Based Rendering) Shader Pipeline** for Swordigo Desktop.

The original Touch Foo Caver Engine uses forward rendering with basic ambient/diffuse Blinn-Phong lighting (`LightComponent.c`, `BasicRenderingPrograms.c`). To support modern visual effects—such as dynamic metallic/roughness material response, physically accurate specular highlights, and Screen Space Reflections (SSR)—SRE OptiX intercepts mesh draw calls to render into a multi-target **G-Buffer Framebuffer**.

---

## 2. G-Buffer Layout & Attachment Architecture

The G-Buffer consists of 4 render target attachments created at display resolution:

$$\text{G-Buffer Size} = \text{Screen Width} \times \text{Screen Height} \times 128 \text{ bits/pixel}$$

```
+-------------------------------------------------------------------------------+
| G-Buffer Framebuffer (FBO)                                                   |
|                                                                               |
| Attachment 0 (GL_COLOR_ATTACHMENT0) : RGBA8   [Albedo.rgb  |  Roughness.a]     |
| Attachment 1 (GL_COLOR_ATTACHMENT1) : RGBA16F [Normal.xyz  |  Metallic.a]      |
| Attachment 2 (GL_COLOR_ATTACHMENT2) : RGBA8   [Emission.rgb |  AO.a]            |
| Attachment 3 (GL_DEPTH24_STENCIL8)  : Depth   [Hardware Depth Buffer]         |
+-------------------------------------------------------------------------------+
```

---

## 3. Shader Injection Specification

### 3.1 Geometry Pass Fragment Shader (`gbuffer_pass.frag`)

During mesh rendering, SRE OptiX hooks `MeshMaterial::Bind` (`MeshMaterial.c`) and `ModelComponent::Render` (`ModelComponent.c`), overriding active shaders to emit G-Buffer properties:

```glsl
#version 330 core

layout (location = 0) out vec4 gAlbedoRoughness;
layout (location = 1) out vec4 gNormalMetallic;
layout (location = 2) out vec4 gEmissionAO;

in vec2 v_TexCoord;
in vec3 v_Normal;
in vec3 v_FragPos;

uniform sampler2D u_AlbedoMap;
uniform sampler2D u_NormalMap;
uniform sampler2D u_MetallicRoughnessMap;

uniform float u_RoughnessOverride;
uniform float u_MetallicOverride;

void main() {
    // Sample diffuse texture
    vec4 albedoSample = texture(u_AlbedoMap, v_TexCoord);
    if (albedoSample.a < 0.1) discard; // Alpha cutout

    // Sample or compute normals
    vec3 N = normalize(v_Normal);
    
    // Read PBR parameters or fallback to material overrides
    vec2 mrSample = texture(u_MetallicRoughnessMap, v_TexCoord).bg;
    float roughness = (mrSample.x > 0.0) ? mrSample.x : u_RoughnessOverride;
    float metallic  = (mrSample.y > 0.0) ? mrSample.y : u_MetallicOverride;

    gAlbedoRoughness = vec4(albedoSample.rgb, roughness);
    gNormalMetallic  = vec4(N * 0.5 + 0.5, metallic); // Pack normal to [0, 1]
    gEmissionAO      = vec4(0.0, 0.0, 0.0, 1.0);     // Ambient occlusion
}
```

### 3.2 Deferred Lighting Pass Fragment Shader (`deferred_pbr_lighting.frag`)

The lighting pass evaluates Cook-Torrance BRDF for all active point and directional lights:

$$f_{r} = \frac{D \cdot F \cdot G}{4 (\mathbf{\omega_o} \cdot \mathbf{N})(\mathbf{\omega_i} \cdot \mathbf{N})}$$

Where:
- $D$ is the Trowbridge-Reitz GGX Normal Distribution Function.
- $F$ is the Fresnel-Schlick approximation ($F_0 + (1 - F_0)(1 - (\mathbf{H} \cdot \mathbf{V}))^5$).
- $G$ is the Smith Geometric Shadowing Function.

```glsl
#version 330 core

out vec4 FragColor;
in vec2 v_TexCoord;

uniform sampler2D gAlbedoRoughness;
uniform sampler2D gNormalMetallic;
uniform sampler2D gDepthMap;

uniform vec3 u_CameraPos;
uniform vec3 u_SunDirection;
uniform vec3 u_SunColor;

const float PI = 3.14159265359;

// GGX Normal Distribution Function
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return num / (PI * denom * denom);
}

// Fresnel Schlick Equation
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec4 albedoRough = texture(gAlbedoRoughness, v_TexCoord);
    vec4 normMetal   = texture(gNormalMetallic, v_TexCoord);

    vec3 albedo    = albedoRough.rgb;
    float roughness = albedoRough.a;
    vec3 N         = normalize(normMetal.rgb * 2.0 - 1.0);
    float metallic  = normMetal.a;

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    
    // Directional sun light calculation
    vec3 L = normalize(-u_SunDirection);
    vec3 V = normalize(u_CameraPos - vec3(v_TexCoord, 0.0)); // World pos reconstructed from depth
    vec3 H = normalize(V + L);

    float NDF = DistributionGGX(N, H, roughness);
    vec3 F   = FresnelSchlick(max(dot(H, V), 0.0), F0);
    
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    
    float NdotL = max(dot(N, L), 0.0);
    vec3 specular = (NDF * F) / max(0.001, 4.0 * max(dot(N, V), 0.0) * NdotL);
    
    vec3 ambient = vec3(0.15) * albedo;
    vec3 color = ambient + (kD * albedo / PI + specular) * u_SunColor * NdotL;

    FragColor = vec4(color, 1.0);
}
```

---

## 4. Material Texture Mapping Conventions

OptiX automatically resolves optional PBR texture maps by checking the Virtual File System (VFS) for companion filenames when a diffuse texture is loaded (`TextureLibrary::TextureForName` in `TextureLibrary.c`):

| Diffuse Base Path | Automatically Injected Normal Map | Automatically Injected PBR Map |
| :--- | :--- | :--- |
| `resources/textures/stone_wall.png` | `resources/textures/stone_wall_n.png` | `resources/textures/stone_wall_mr.png` |
| `resources/textures/hero_body.png` | `resources/textures/hero_body_n.png` | `resources/textures/hero_body_mr.png` |
| `resources/textures/water_caustics.png` | `resources/textures/water_caustics_n.png` | `resources/textures/water_caustics_mr.png` |
