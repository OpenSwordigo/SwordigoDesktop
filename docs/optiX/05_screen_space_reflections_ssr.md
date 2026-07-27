# OptiX Technical Specification 05: Screen Space Reflections (SSR) & Real-Time Reflection Shaders

## 1. Executive Overview

This specification details the technical design and GLSL implementation of **Screen Space Reflections (SSR)** for Swordigo Desktop.

In Ghidra decompilation `WaterMeshComponent.c` and `BackgroundComponent.c`, water surfaces and metallic sword blades in Swordigo rely on static 2D scrolling textures or basic planar reflection maps. SRE OptiX introduces a real-time ray-marched SSR pass that samples the G-Buffer depth and normal textures to calculate dynamic reflections for water bodies, ice, armor, and glossy floors.

---

## 2. SSR Ray Marching Algorithm

The SSR post-processing pass operates on the G-Buffer color attachment and depth texture. For each glossy pixel, a ray is cast from the fragment position in view space along the reflection vector $\mathbf{R}$:

$$\mathbf{R} = \text{reflect}(\mathbf{V}, \mathbf{N})$$

Where:
- $\mathbf{V}$ is the normalized view direction vector ($\text{normalize}(\mathbf{P}_{\text{view}})$).
- $\mathbf{N}$ is the view-space normal sampled from G-Buffer Attachment 1.

```
Camera (Origin)
   \
    \ View Vector V
     \          Reflection Vector R
      v         /
  +------------*-----------------------------------------------+  G-Buffer Depth
  | Surface    \                                               |  Surface
  |             \ Ray March Steps: P_t = P_0 + t * R           |
  |              v  v  v  v  v (Sample Depth Buffer)           |
  +------------------------------------------------------------+
```

---

## 3. High-Performance GLSL SSR Shader (`ssr_pass.frag`)

```glsl
#version 330 core

out vec4 FragColor;
in vec2 v_TexCoord;

uniform sampler2D u_SceneColor;
uniform sampler2D gNormalMetallic;
uniform sampler2D gDepthMap;

uniform mat4 u_ProjectionMatrix;
uniform mat4 u_InverseProjectionMatrix;

const int MAX_STEPS = 32;
const float RAY_STEP = 0.08;
const float DISTANCE_BIAS = 0.03;

vec3 ReconstructViewPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = u_InverseProjectionMatrix * ndc;
    return viewPos.xyz / viewPos.w;
}

vec2 ProjectViewPos(vec3 viewPos) {
    vec4 clipPos = u_ProjectionMatrix * vec4(viewPos, 1.0);
    vec3 ndc = clipPos.xyz / clipPos.w;
    return ndc.xy * 0.5 + 0.5;
}

void main() {
    float depth = texture(gDepthMap, v_TexCoord).r;
    if (depth >= 1.0) {
        FragColor = texture(u_SceneColor, v_TexCoord);
        return;
    }

    vec4 normMetal = texture(gNormalMetallic, v_TexCoord);
    vec3 N = normalize(normMetal.rgb * 2.0 - 1.0);
    float metallic = normMetal.a;

    // Skip non-reflective surfaces
    if (metallic < 0.1) {
        FragColor = texture(u_SceneColor, v_TexCoord);
        return;
    }

    vec3 viewPos = ReconstructViewPos(v_TexCoord, depth);
    vec3 V = normalize(viewPos);
    vec3 R = reflect(V, N);

    // Ray marching loop
    vec3 rayPos = viewPos;
    vec2 hitUV = vec2(0.0);
    bool hitFound = false;

    for (int i = 0; i < MAX_STEPS; i++) {
        rayPos += R * RAY_STEP;
        vec2 sampleUV = ProjectViewPos(rayPos);

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            break; // Ray exited screen space
        }

        float sampleDepth = texture(gDepthMap, sampleUV).r;
        vec3 sampledPos = ReconstructViewPos(sampleUV, sampleDepth);

        float depthDiff = rayPos.z - sampledPos.z;
        if (depthDiff > 0.0 && depthDiff < DISTANCE_BIAS) {
            hitUV = sampleUV;
            hitFound = true;
            break;
        }
    }

    vec4 baseColor = texture(u_SceneColor, v_TexCoord);
    if (hitFound) {
        vec3 reflectedColor = texture(u_SceneColor, hitUV).rgb;
        // Edge fading factor
        float edgeFade = 1.0 - smoothstep(0.8, 1.0, max(abs(hitUV.x - 0.5), abs(hitUV.y - 0.5)) * 2.0);
        FragColor = vec4(mix(baseColor.rgb, reflectedColor, metallic * edgeFade), 1.0);
    } else {
        FragColor = baseColor;
    }
}
```

---

## 4. `WaterMeshComponent` Interception Blueprint

In `WaterMeshComponent.c` (`Caver::WaterMeshComponent::Render`), water surface mesh rendering is intercepted via SRE trampoline `sre_WaterMeshComponent_Render`:

1. **G-Buffer Tagging**: Sets `gNormalMetallic.a` (metallic/glossiness) to `0.85` for water surface vertices.
2. **Normal Map Distortion**: Blends dynamic normal maps generated from wave equations into the G-Buffer normal attachment:
   $$\mathbf{N}_{\text{water}} = \text{normalize}\left(\mathbf{N}_{\text{base}} + \mathbf{N}_{\text{wave1}}(UV + t) + \mathbf{N}_{\text{wave2}}(UV - t)\right)$$
3. **SSR Post-Pass Execution**: The water surface automatically receives real-time screen-space reflections of characters, monsters, spell effects, and cavern ceilings.
