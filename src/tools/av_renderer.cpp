/* av_renderer.cpp — Modern OpenGL 3.3 renderer implementation
 *
 * Self-contained renderer for the Swordigo Asset Viewer.
 * Uses Lambertian lighting faithful to Swordigo's original model:
 *   lit = emission + ambient*mat_ambient + diffuse*mat_diffuse*max(dot(N,L),0)
 *
 * All matrix math is inline (no GLM dependency).
 * Requires GL_GLEXT_PROTOTYPES for modern GL 3.3 functions on Linux.
 */

// Enable GL extension prototypes BEFORE any GL includes
#define GL_GLEXT_PROTOTYPES 1
#include "platform/gl_inc.h"
#include <GL/glext.h>

#include "av_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace av {

// ============================================================================
// Constants
// ============================================================================

static constexpr float PI = 3.14159265358979323846f;
static constexpr float DEG2RAD = PI / 180.0f;

// Global lighting (warm directional, slight cool ambient — Swordigo-faithful by default)
// Vanilla Swordigo: a warm sun from above, deep dark cave ambient, strong
// top-vs-side distinction so platforms read as lit on top and shaded below.
float g_light_dir[3]   = { 0.35f,  0.85f,  0.39f };  // mostly-down sun
float g_light_color[3] = { 0.95f,  0.86f,  0.70f };  // warm golden (tamed)
// Cool fill from the opposite side of the key — keeps undersides and the
// side of the player away from the key readable instead of dead black.
float g_fill_color[3]  = { 0.30f,  0.38f,  0.55f };
float g_rim_strength   = 0.32f;   // camera-opposed edge light (silhouette pop)
float g_spec_strength  = 0.06f;   // subtle Blinn-Phong sheen on materials
float g_ambient_color[3]     = { 0.19f,   0.20f,   0.25f };  // cool sky fill
float g_ambient_ground_color[3] = { 0.09f, 0.095f, 0.14f };  // dark ground fill
float g_clear_color[3]       = { 0.055f,  0.058f,  0.08f };
// Vendor .scn light scales (renderer-master semantics): the scene's
// `punctual` factor modulates the directional sun, `ambient` the hemisphere
// fill. Both default to 1.0 so non-vendor previews are untouched; the
// .scn loader saves/restores these around the preview.
float g_light_scale   = 1.0f;
float g_ambient_scale = 1.0f;

// ============================================================================
// Shader sources — GLSL 330 core
// ============================================================================

// --- Model shaders (Lambertian + texture) ---

static const char* MODEL_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uNormalMat;

out vec3 vNormal;
out vec3 vWorldPos;
out vec2 vUV;

void main() {
    vNormal   = normalize(uNormalMat * aNorm);
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    vUV       = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* MODEL_FS = R"GLSL(
#version 330 core
in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUV;

uniform sampler2D uTexture;
uniform bool  uHasTexture;
uniform vec3  uLightDir;        // normalized direction TO the light
uniform vec3  uLightColor;      // directional light color
uniform vec3  uAmbient;         // ambient light color (sky / up-facing fill)
uniform vec3  uAmbientGround;   // hemisphere ground fill (bounced ambient)
uniform vec4  uMatColor;        // material base color (when untextured)
uniform float uAlpha;           // overall alpha multiplier
uniform bool  uFlatShade;       // diagnostics: bypass lighting (texture \u00d7 material)
uniform bool  uDebugNormals;    // diagnostics: color = N*0.5+0.5 (orientation check)
uniform bool  uFlipV;           // DCC-style UVs (v=0 top) on bottom-origin textures
uniform vec3  uFillColor;       // cool fill light (opposite side of the key)
uniform float uRimStrength;     // camera-opposed edge light
uniform float uSpecStrength;    // Blinn-Phong sheen amount
uniform vec3  uCamPos;          // camera position (for point lights)
uniform bool  uFogEnabled;
uniform vec3  uFogColor;        // atmospheric depth-darkening tint
uniform float uFogNear;         // distance where fog starts
uniform float uFogFar;          // distance where fog is full
uniform bool  uInlineTonemap;   // false = HDR path: output LINEAR light, the
                                // PostFX HD pass owns tone mapping (no
                                // double-ACES / gamma-on-gamma wash)

#define MAX_POINT_LIGHTS 16
uniform int    uLightCount;
uniform vec3   uLightPos[MAX_POINT_LIGHTS];
uniform vec3   uLightCol[MAX_POINT_LIGHTS];
uniform float  uLightRadius[MAX_POINT_LIGHTS];

#define MAX_DIR_LIGHTS 4
uniform int   uDirLightCount;
uniform vec3  uDirLightDir[MAX_DIR_LIGHTS];
uniform vec3  uDirLightCol[MAX_DIR_LIGHTS];

out vec4 FragColor;

void main() {
    // v = 0 is the BOTTOM of uploaded textures (game convention). DCC
    // importers (FBX/glTF previews) set uFlipV so their v=0-top UVs sample
    // the top of the image instead — fixing the flipped FBX texture bug.
    vec2  sampUV = vec2(vUV.x, uFlipV ? 1.0 - vUV.y : vUV.y);
    vec4  base = uHasTexture ? texture(uTexture, sampUV) : uMatColor;
    vec3  N    = normalize(vNormal);

    // Diagnostics: render world-space normals as color. Inverted / missing
    // normals become obvious (a wall whose normal points INTO the surface
    // shows purple/blue instead of its surface tint) — the "black wood"
    // smoking gun from the lighting bug reports.
    if (uDebugNormals) {
        FragColor = vec4(N * 0.5 + 0.5, 1.0);
        return;
    }
    // Diagnostics: flat texture view — no lighting at all. Separates "the
    // texture itself is dark" from "the lighting equation is wrong".
    if (uFlatShade) {
        FragColor = vec4(base.rgb, base.a * uAlpha);
        return;
    }

    // ── Ambient: hemisphere sky/ground fill with a low floor so nothing
    //    ever crushes to black (the "black wood" / "black hero" symptom),
    //    but shadows still get real depth in the linear pipeline.
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    // Minimum-light floor raised from 0.025 → 0.06: out-of-radius geometry now
    // reads as "in a dim room" instead of crushing to pure black (the "unlit
    // black room with lamps" complaint), while still leaving real shadow
    // contrast for lit areas. This floor applies to EVERY surface the model
    // shader touches (walls, ground meshes, hero), so ambient is guaranteed to
    // reach all of them.
    vec3  amb  = max(mix(uAmbientGround, uAmbient, hemi), vec3(0.06));
    // Textures are authored in sRGB — decode to LINEAR before any lighting
    // math (gamma-correct rendering). This alone recovers most of the lost
    // contrast of the old washed-out look.
    vec3 albedo = pow(base.rgb, vec3(2.2));

    // Epsilon guards normalize() against a vertex sitting exactly at the
    // camera (NaN would poison the rim term → black pixels).
    vec3 V = normalize(uCamPos - vWorldPos + vec3(1e-4));

    // Soft half-Lambert preserves Swordigo's readable silhouettes while every
    // directional component follows one coherent lighting equation. The tight
    // wrap + gamma curve keeps a punchy terminator (real shadow contrast).
    vec3 keyDir = (uDirLightCount == 0) ? normalize(uLightDir)
                                        : normalize(uDirLightDir[0]);
    vec3 keyCol = (uDirLightCount == 0) ? uLightColor : uDirLightCol[0];
    float ndl   = dot(N, keyDir);
    float wrap  = clamp((ndl + 0.12) / 1.12, 0.0, 1.0);
    vec3  key   = keyCol * (pow(wrap, 1.35) * 0.95);

    // ── Fill light: cool light from the opposite side of the key. This is
    //    the classic three-point setup — it gives walls/players shape on the
    //    side the key can't reach and prevents dead-black undersides.
    vec3 fillDir = normalize(-keyDir * 0.55 + vec3(0.0, 0.5, 0.0));
    vec3 fill    = uFillColor * clamp(dot(N, fillDir), 0.0, 1.0) * 0.6;

    // ── Rim light (camera-opposed): brightens the silhouette edge, making
    //    the hero and geometry pop against the background.
    float rim  = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 3.0) * uRimStrength;
    vec3  rimL = keyCol * rim;

    // ── Subtle Blinn-Phong sheen: a soft highlight that gives materials a
    //    polished (but not glossy-plastic) read.
    vec3  H    = normalize(keyDir + V);
    float spec = pow(max(dot(N, H), 0.0), 24.0) * uSpecStrength;

    vec3 lit = albedo * (amb + key + fill + rimL) + keyCol * spec;
    for (int i = 1; i < max(uDirLightCount, 0); ++i) {
        vec3 L = normalize(uDirLightDir[i]);
        float d = clamp((dot(N, L) + 0.14) / 1.14, 0.0, 1.0);
        vec3 H2 = normalize(L + V);
        float s = pow(max(dot(N, H2), 0.0), 24.0) * uSpecStrength;
        lit += albedo * uDirLightCol[i] * d * d + uDirLightCol[i] * s;
    }

    // Point lights (warm torch / fire / SimpleGlow): compact LOCALIZED sources.
    // The inverse-square-ish core concentrates illumination around the emitter
    // and the soft knee transitions to zero just past the influence radius —
    // no giant uniform blob, no hard cutoff ring. Intensity is baked into
    // uLightCol by the host (color * intensity), and N·L keeps surfaces
    // responding by their orientation: floors light up, walls catch the side,
    // away-facing faces stay shaded.
    for (int i = 0; i < uLightCount; ++i) {
        vec3  toL   = uLightPos[i] - vWorldPos;
        float dist  = length(toL);
        float r     = max(uLightRadius[i], 0.001);
        float norm  = dist / r;
        // Radius-faithful falloff. The old curve
        //   window(norm)^2 / (1 + 1.6·norm + 1.8·norm²)
        // was already at ~39% by HALF the authored radius and collapsed to a
        // few percent well before it — that is the "tiny light pool in a black
        // void" bug. A torch whose radius says it reaches N units barely lit a
        // few polygons.
        //
        // New model: a gentle inverse-square core (physically warm near the
        // emitter) MULTIPLIED by a smooth UE4/Karis window that stays wide and
        // only rolls to exactly zero at the authored radius. `atten(0)=1`,
        // `atten(0.5)≈0.6`, `atten(0.85)≈0.2`, `atten(1)=0`. Multiple lights
        // now overlap into a continuous, readable wash instead of isolated
        // hard-edged discs.
        float window = clamp(1.0 - pow(norm, 4.0), 0.0, 1.0);
        window *= window;                                   // soft shoulder
        float invsq = 1.0 / (1.0 + 2.0 * norm * norm);      // gentle core
        float atten = window * mix(1.0, invsq, 0.65);       // blend flat+physical
        // Epsilon keeps normalize() from producing NaN when a vertex sits
        // exactly at the light position (inside the emitter core).
        vec3 L = normalize(toL + vec3(1e-4));
        float diff = clamp((dot(N, L) + 0.08) / 1.08, 0.0, 1.0);
        vec3 Hp = normalize(L + V);
        float pointSpec = pow(max(dot(N, Hp), 0.0), 32.0) * uSpecStrength;
        lit += (albedo * diff + pointSpec) * uLightCol[i] * atten;
    }

    // Atmospheric depth fog: distant geometry darkens toward the cave tint,
    // visually separating background layers like vanilla Swordigo.
    if (uFogEnabled) {
        float camDist = length(vWorldPos - uCamPos);
        float f = smoothstep(uFogNear, uFogFar, camDist);
        // Mix toward a dark fog color modulated by the ambient so fully distant
        // surfaces read as shadowed background rather than hue-preserved.
        vec3 fogged = uFogColor * (uAmbient * 2.0 + 0.02);
        lit = mix(lit, fogged, f * f);
    }

    // Tone map ownership: the HDR path (PostFX HD on) keeps this value LINEAR
    // — exposure + ACES + gamma happen once, in the composite pass. Only the
    // legacy/no-PostFX path tonemaps here (thumbnails, previews).
    if (uInlineTonemap) {
        lit = clamp(lit, 0.0, 1e4);
        lit = (lit * (2.51 * lit + 0.03)) / (lit * (2.43 * lit + 0.59) + 0.14);
    } else {
        lit = clamp(lit, 0.0, 64.0);   // sane HDR ceiling for RGBA8 fallback
    }

    FragColor = vec4(lit, base.a * uAlpha);
}
)GLSL";

// ============================================================================
// PBR shaders — vendored algorithms from zauonlok/renderer (MIT, Zhou Le)
//   · GGX distribution, Smith visibility, Schlick+F90 Fresnel
//   · metallic-roughness + specular-glossiness workflows
//   · tangent-space normal mapping (TBN, tangent.w handedness)
//   · image-based lighting: split-sum (prefiltered cubemap + BRDF LUT)
//   · directional shadow mapping (light-VP compare with N·L-scaled bias)
//   · ACES tone mapping
// See src/render/zauonlok/ for the authoritative C reference sources.
// ============================================================================

static const char* PBR_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aTangent;   // xyz tangent, w handedness
layout(location=4) in vec4 aJoint;      // skinning (JOINTS_0, float) — optional
layout(location=5) in vec4 aWeight;     // skinning (WEIGHTS_0) — optional

uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uNormalMat;
uniform mat4 uLightVPMatrix;
uniform bool uFlipV;
uniform bool uHasTangent;
uniform int  uJointCount;
uniform mat4 uJoints[256];

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vTangent;
out vec3 vBitangent;
out vec2 vUV;
out vec4 vDepthPos;

void main() {
    vec4 pos = vec4(aPos, 1.0);
    vec3 nrm = aNorm;
    if (uJointCount > 0) {
        // 4-bone skinning in model space (vendor: mat4_combine of joint mats)
        mat4 skin = mat4(0.0);
        float js[4] = float[4](aJoint.x, aJoint.y, aJoint.z, aJoint.w);
        float ws[4] = float[4](aWeight.x, aWeight.y, aWeight.z, aWeight.w);
        for (int k = 0; k < 4; ++k) {
            if (js[k] >= 0.0 && js[k] < 256.0 && ws[k] > 0.0)
                skin += uJoints[int(js[k])] * ws[k];
        }
        pos = skin * pos;
        nrm = mat3(skin) * nrm;
    }
    vec4 world = uModel * pos;
    vWorldPos = world.xyz;
    vNormal = normalize(uNormalMat * nrm);
    // Guard against degenerate tangents (a CPU skin update may pass null
    // tangent streams, leaving zeroes that would NaN normalize()).
    if (uHasTangent && dot(aTangent.xyz, aTangent.xyz) > 1e-8) {
        vTangent = normalize(uNormalMat * aTangent.xyz);
        vBitangent = cross(vNormal, vTangent) * aTangent.w;
    } else {
        vTangent = vec3(1.0, 0.0, 0.0);
        vBitangent = vec3(0.0, 0.0, 1.0);
    }
    vUV = vec2(aUV.x, uFlipV ? 1.0 - aUV.y : aUV.y);
    vDepthPos = uLightVPMatrix * world;
    gl_Position = uMVP * pos;
}
)GLSL";

static const char* PBR_FS = R"GLSL(
#version 330 core
in vec3 vWorldPos;
 in vec3 vNormal;
in vec3 vTangent;
in vec3 vBitangent;
in vec2 vUV;
in vec4 vDepthPos;

uniform vec3  uCamPos;
uniform bool  uHasBasecolor;
uniform bool  uHasMetalness;
uniform bool  uHasRoughness;
uniform bool  uHasNormalMap;
uniform bool  uHasOcclusion;
uniform bool  uHasEmission;
uniform bool  uHasSpecular;
uniform bool  uHasGlossiness;
uniform sampler2D uBasecolorTex;   // unit 0
uniform sampler2D uMetalnessTex;   // unit 1
uniform sampler2D uRoughnessTex;   // unit 2
uniform sampler2D uNormalTex;      // unit 3
uniform sampler2D uOcclusionTex;   // unit 4
uniform sampler2D uEmissionTex;    // unit 5
uniform sampler2D uSpecularTex;    // unit 6
uniform sampler2D uGlossinessTex;  // unit 7
uniform samplerCube uEnvMap;       // unit 8 (prefiltered mip chain)
uniform sampler2D uBRDF_LUT;       // unit 9 (split-sum LUT)
uniform sampler2D uShadowMap;      // unit 10 (directional depth)
uniform vec4  uBasecolorFactor;
uniform float uMetalnessFactor;
uniform float uRoughnessFactor;
uniform vec3  uSpecularFactor;
uniform float uGlossinessFactor;
uniform float uOcclusion;
uniform vec3  uEmission;
uniform int   uWorkflow;           // 0 = metallic-roughness, 1 = specular-glossiness
uniform float uAlphaCutoff;        // > 0 enables alpha test
uniform bool  uHasEnv;
uniform float uEnvIntensity;
uniform int   uEnvMaxMip;
uniform bool  uShadowEnabled;
uniform mat4  uLightVPMatrix;

uniform vec3  uLightDir;
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform vec3  uAmbientGround;
#define MAX_POINT_LIGHTS 16
uniform int    uLightCount;
uniform vec3   uLightPos[MAX_POINT_LIGHTS];
uniform vec3   uLightCol[MAX_POINT_LIGHTS];
uniform float  uLightRadius[MAX_POINT_LIGHTS];
#define MAX_DIR_LIGHTS 4
uniform int   uDirLightCount;
uniform vec3  uDirLightDir[MAX_DIR_LIGHTS];
uniform vec3  uDirLightCol[MAX_DIR_LIGHTS];

out vec4 FragColor;

const float PI = 3.14159265359;

// ── vendor get_distribution(): GGX / Trowbridge-Reitz NDF ──
float distribution_ggx(float n_dot_h, float alpha2) {
    float n_dot_h_2 = n_dot_h * n_dot_h;
    float f = n_dot_h_2 * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * f * f);
}

// ── vendor get_visibility(): Smith GGX (folded /4·NdotV·NdotL) ──
float visibility_smith(float n_dot_v, float n_dot_l, float alpha2) {
    float n_dot_v_2 = n_dot_v * n_dot_v;
    float n_dot_l_2 = n_dot_l * n_dot_l;
    float ggx_v = n_dot_l * sqrt(n_dot_v_2 * (1.0 - alpha2) + alpha2);
    float ggx_l = n_dot_v * sqrt(n_dot_l_2 * (1.0 - alpha2) + alpha2);
    return 0.5 / (ggx_v + ggx_l);
}

// ── vendor get_fresnel(): Schlick with F90 (max component * 50) ──
vec3 fresnel_schlick(float v_dot_h, vec3 fresnel0) {
    float f = pow(1.0 - v_dot_h, 5.0);
    float f90 = clamp(max(max(fresnel0.r, fresnel0.g), fresnel0.b) * 50.0, 0.0, 1.0);
    return fresnel0 + (vec3(f90) - fresnel0) * f;
}

// ── vendor get_ibl_shade(): split-sum diffuse + specular ──
vec3 ibl_shade(vec3 N, vec3 V, vec3 diffuse_color, vec3 specular_color,
               float roughness, float occlusion) {
    vec3 diffuse_light = texture(uEnvMap, N).rgb;
    vec3 diffuse_shade = diffuse_light * diffuse_color * occlusion;

    float n_dot_v = clamp(dot(N, V), 0.0, 1.0);
    vec2 lut = texture(uBRDF_LUT, vec2(n_dot_v, roughness)).rg;
    vec3 R = reflect(-V, N);
    float lod = roughness * float(uEnvMaxMip);
    vec3 specular_light = textureLod(uEnvMap, R, lod).rgb;
    vec3 specular_shade = specular_light * (specular_color * lut.x + lut.y);
    return diffuse_shade + specular_shade;
}

// ── vendor get_normal_dir(): tangent-space normal mapping ──
vec3 get_normal_dir(vec3 normal_dir) {
    if (uHasNormalMap) {
        vec3 tn = texture(uNormalTex, vUV).xyz * 2.0 - 1.0;
        return normalize(vTangent * tn.x + vBitangent * tn.y + normal_dir * tn.z);
    }
    return normal_dir;
}

// ── vendor is_in_shadow(): depth compare with N·L-scaled bias ──
bool is_in_shadow(vec3 depth_pos, float n_dot_l) {
    vec2 uv = depth_pos.xy * 0.5 + 0.5;
    float d = depth_pos.z * 0.5 + 0.5;
    float bias = max(0.05 * (1.0 - n_dot_l), 0.005);
    float closest = texture(uShadowMap, uv).r;
    return (d - bias) > closest;
}

struct Material {
    vec3 diffuse;
    vec3 specular;
    float alpha;
    float roughness;
    float occlusion;
    vec3 emission;
};

// ── vendor get_pbrm_material() / get_pbrs_material() ──
Material decode_material() {
    vec3 basecolor = uBasecolorFactor.rgb;
    float alpha = uBasecolorFactor.a;
    if (uHasBasecolor) {
        vec4 s = texture(uBasecolorTex, vUV);
        basecolor *= s.rgb;
        alpha *= s.a;
    }
    float metalness = uMetalnessFactor;
    if (uHasMetalness) metalness *= texture(uMetalnessTex, vUV).r;
    float roughness = uRoughnessFactor;
    if (uHasRoughness) roughness *= texture(uRoughnessTex, vUV).r;

    Material m;
    m.alpha = alpha;
    m.roughness = clamp(roughness, 0.045, 1.0);
    m.occlusion = 1.0;
    m.emission = uEmission;
    if (uHasOcclusion) m.occlusion = texture(uOcclusionTex, vUV).r;
    if (uHasEmission) m.emission = texture(uEmissionTex, vUV).rgb;

    if (uWorkflow == 0) {
        // metallic-roughness: diffuse = base·(1−F0)·(1−metalness), spec = lerp(F0, base, metalness)
        vec3 F0 = vec3(0.04);
        m.diffuse = basecolor * (1.0 - 0.04) * (1.0 - metalness);
        m.specular = mix(F0, basecolor, metalness);
    } else {
        // specular-glossiness: diffuse = base·(1−max(spec)), roughness = 1−glossiness
        vec3 spec = uSpecularFactor;
        if (uHasSpecular) spec *= texture(uSpecularTex, vUV).rgb;
        float glossiness = uGlossinessFactor;
        if (uHasGlossiness) glossiness *= texture(uGlossinessTex, vUV).r;
        float maxc = max(max(spec.r, spec.g), spec.b);
        m.diffuse = basecolor * (1.0 - maxc);
        m.specular = spec;
        m.roughness = clamp(1.0 - glossiness, 0.045, 1.0);
    }
    return m;
}

void main() {
    Material m = decode_material();
    if (uAlphaCutoff > 0.0 && m.alpha < uAlphaCutoff) discard;

    vec3 N = get_normal_dir(normalize(vNormal));
    vec3 V = normalize(uCamPos - vWorldPos + vec3(1e-4));
    float n_dot_v = max(dot(N, V), 0.0);
    float alpha2 = m.roughness * m.roughness;

    // Environment (IBL split-sum) + emission first.
    vec3 color = m.emission;
    if (uHasEnv) color += ibl_shade(N, V, m.diffuse, m.specular, m.roughness, m.occlusion) * uEnvIntensity;

    // Key directional light (fallback to editor preview light).
    vec3 keyDir = (uDirLightCount == 0) ? normalize(uLightDir) : normalize(uDirLightDir[0]);
    vec3 keyCol = (uDirLightCount == 0) ? uLightColor : uDirLightCol[0];
    float n_dot_l = max(dot(N, keyDir), 0.0);
    if (n_dot_l > 0.0) {
        vec3 H = normalize(keyDir + V);
        float D = distribution_ggx(max(dot(N, H), 0.0), alpha2);
        float G = visibility_smith(n_dot_v, n_dot_l, alpha2);
        vec3 F = fresnel_schlick(max(dot(V, H), 0.0), m.specular);
        float shadow = (uShadowEnabled && is_in_shadow(vDepthPos.xyz / vDepthPos.w, n_dot_l)) ? 0.0 : 1.0;
        color += (m.diffuse / PI + D * G * F) * keyCol * n_dot_l * shadow;
    }

    // Extra directional lights.
    for (int i = 1; i < uDirLightCount; ++i) {
        vec3 L = normalize(uDirLightDir[i]);
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) continue;
        vec3 H = normalize(L + V);
        float D = distribution_ggx(max(dot(N, H), 0.0), alpha2);
        float G = visibility_smith(n_dot_v, ndl, alpha2);
        vec3 F = fresnel_schlick(max(dot(V, H), 0.0), m.specular);
        color += (m.diffuse / PI + D * G * F) * uDirLightCol[i] * ndl;
    }

    // Point lights (torch/fire — compact localized sources).
    for (int i = 0; i < uLightCount; ++i) {
        vec3 toL = uLightPos[i] - vWorldPos;
        float dist = length(toL);
        float r = max(uLightRadius[i], 0.001);
        float norm = dist / r;
        // Radius-faithful falloff (see MODEL_FS for the full rationale): a
        // gentle inverse-square core blended with a smooth window that only
        // reaches zero at the authored radius, so torch/lava light spreads and
        // overlaps the way the original game does instead of dying instantly.
        float window = clamp(1.0 - pow(norm, 4.0), 0.0, 1.0);
        window *= window;
        float invsq = 1.0 / (1.0 + 2.0 * norm * norm);
        float atten = window * mix(1.0, invsq, 0.65);
        vec3 L = normalize(toL + vec3(1e-4));
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) continue;
        vec3 H = normalize(L + V);
        float D = distribution_ggx(max(dot(N, H), 0.0), alpha2);
        float G = visibility_smith(n_dot_v, ndl, alpha2);
        vec3 F = fresnel_schlick(max(dot(V, H), 0.0), m.specular);
        color += (m.diffuse / PI + D * G * F) * uLightCol[i] * ndl * atten;
    }

    // Hemisphere ambient (sky/ground fill) — same feel + same raised
    // minimum-light floor (0.02 → 0.06) as the model shader, so PBR/vendor
    // geometry out of any light's radius also reads as "dim room", not black.
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 amb = max(mix(uAmbientGround, uAmbient, hemi), vec3(0.06));
    color += m.diffuse * amb * m.occlusion;

    // ACES filmic tone mapping (Krzysztof Narkowicz; vendor README reference).
    color = clamp(color, 0.0, 1e4);
    color = (color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14);

    FragColor = vec4(color, m.alpha);
}
)GLSL";

// ── Directional shadow-map depth pass ──
static const char* SHADOW_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;

uniform mat4 uLightVP;
uniform mat4 uModel;

void main() {
    gl_Position = uLightVP * uModel * vec4(aPos, 1.0);
}
)GLSL";

static const char* SHADOW_FS = R"GLSL(
#version 330 core
out vec4 FragColor;
void main() {
    FragColor = vec4(1.0);
}
)GLSL";

// --- Grid shaders (XZ plane with fade-to-transparent) ---

static const char* GRID_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec3 vWorldPos;

void main() {
    vWorldPos   = vec3(uModel * vec4(aPos, 1.0));
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* GRID_FS = R"GLSL(
#version 330 core
in vec3 vWorldPos;

uniform float uGridSize;  // fade outer radius (world units)
uniform vec4  uGridColor; // base grid color (rgb) + global alpha
uniform float uScale;     // world units per screen pixel at the grid plane

out vec4 FragColor;

// Distance (world units) from a coordinate to its nearest grid line of a
// given spacing. 1-2-5 adaptive spacing keeps the grid crisp at any zoom.
float distToLine(float pos, float step)
{
    return abs(abs(pos) - round(pos / step) * step);
}

void main()
{
    vec2 g = vWorldPos.xz;
    float dist = length(g);
    float fade = 1.0 - smoothstep(uGridSize * 0.55, uGridSize, dist);

    // Target ~80px between parallel lines, snapped to a clean 1-2-5 step.
    float s = max(uScale, 1e-4);
    float target = s * 80.0;
    float decade = pow(10.0, floor(log(target) / log(10.0)));
    float norm = target / decade;
    float major = (norm < 2.0 ? 2.0 : norm < 5.0 ? 5.0 : 10.0) * decade;
    float minor = major / 5.0;

    // Per-pixel anti-aliased line alpha (1.0px minor, ~1.7px major).
    float pxMinor = min(distToLine(g.x, minor), distToLine(g.y, minor)) / s;
    float pxMajor = min(distToLine(g.x, major), distToLine(g.y, major)) / s;
    float minorA  = 1.0 - smoothstep(0.55, 1.35, pxMinor);
    float majorA  = 1.0 - smoothstep(0.55, 1.75, pxMajor);
    float line    = max(majorA, minorA * 0.45);

    // Axis emphasis: the zero lines are drawn thicker and brighter.
    float axX = 1.0 - smoothstep(0.6, 2.0, abs(g.x) / s);
    float axZ = 1.0 - smoothstep(0.6, 2.0, abs(g.y) / s);
    float axis = max(axX, axZ);

    vec3 col = uGridColor.rgb;
    vec3 axisCol = min(vec3(1.0), uGridColor.rgb * 1.35 + vec3(0.06));
    col = mix(col, axisCol, axis * 0.9);

    float alpha = uGridColor.a * max(line, axis) * fade;
    FragColor = vec4(col, alpha);
}
)GLSL";

// --- Background quad shaders (unlit, textured, depth-write off) ---
// Matches the reference editor's background handling: a MeshBasicMaterial-like
// textured plane drawn first, never occluding the level.

static const char* BG_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* BG_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    FragColor = texture(uTexture, vUV);
}
)GLSL";

// --- Glow sprite shaders (additive billboards for torch/fire bloom) ---
// A radial soft falloff sprite; alpha is the sprite mask and brightness is
// multiplied by uColor so values above 1.0 feed the bloom bright-pass.
static const char* GLOW_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* GLOW_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform vec3 uColor;

out vec4 FragColor;

void main() {
    // Radial falloff: bright core, soft edge.
    vec2  c = vUV - 0.5;
    float d = length(c) * 2.0;
    float falloff = 1.0 - smoothstep(0.15, 1.0, d);
    FragColor = vec4(uColor * falloff, falloff);
}
)GLSL";

// --- Animated procedural fire shaders (FBO-based flame billboard) ---
// A small camera-facing quad on which an upward-flowing turbulent flame is
// generated PROCEDURALLY (layered value-noise scrolling up + horizontal sway),
// masked to a teardrop flame silhouette and colored through a fire gradient
// (dark red → orange → yellow → pale tip). Output is additive with soft alpha
// edges so it emits into the scene (bloom bright-pass) instead of sitting flat.
// The same VS as the glow sprite is reused; only the FS differs.
static const char* FIRE_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform vec3  uColor;    // torch tint (baked color*intensity from host)
uniform float uTime;     // seconds, drives the upward flow + flicker
uniform float uFlicker;  // 0..1 current flame brightness (synced to the light)

out vec4 FragColor;

// Cheap hash-based value noise (no texture dependency — instances freely).
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// Fractal turbulence flowing upward over time.
float fbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * vnoise(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

void main() {
    // Centered coords: x in [-1,1], y 0 (base) .. 1 (tip).
    vec2 uv = vUV;
    float x = (uv.x - 0.5) * 2.0;
    float y = uv.y;

    // Upward-scrolling turbulence + gentle horizontal sway near the tip.
    float flow = uTime * 1.6;
    float n = fbm(vec2(uv.x * 3.0 + sin(uTime * 2.3) * 0.15,
                       uv.y * 2.2 - flow));
    // Teardrop flame mask: narrow at the top, widest low-mid, soft edges.
    float width = mix(0.85, 0.06, smoothstep(0.15, 1.0, y));
    float body  = 1.0 - smoothstep(0.0, width, abs(x) + (n - 0.5) * 0.55 * y);
    // Vertical envelope: fade in from the base, taper out at the tip.
    float vert  = smoothstep(0.0, 0.12, y) * (1.0 - smoothstep(0.55, 1.0, y));
    float flame = clamp(body * vert, 0.0, 1.0);
    // Turbulent inner detail so it never looks like a solid blob.
    flame *= 0.65 + 0.35 * fbm(vec2(uv.x * 5.0, uv.y * 4.0 - flow * 1.3));

    if (flame < 0.02) discard;

    // Fire gradient: hot white/yellow core low-center → orange → red tips.
    float heat = clamp(flame * (1.0 - y * 0.7) * 1.6, 0.0, 1.0);
    vec3 cool = uColor * 0.7;                     // deep red-ish base tint
    vec3 warm = uColor * 1.4;                      // orange body (over-driven)
    vec3 hot  = mix(uColor, vec3(1.0, 0.95, 0.78), 0.8) * 2.2; // white-hot core
    vec3 col  = mix(cool, warm, smoothstep(0.15, 0.55, heat));
    col       = mix(col, hot, smoothstep(0.6, 1.0, heat));

    // Flicker modulates overall brightness (synced to the point light).
    float bright = flame * (0.85 + 0.35 * uFlicker);
    // Emit additively in HDR range. Fire is an EMITTER: the color must land
    // well above 1.0 so it stays visibly bright after ACES tonemapping (when
    // PostFX/HDR is on) AND drives the bloom bright-pass. The old ~1.4 gain
    // collapsed to a dim smear once the tonemapper compressed it — this ~4x
    // HDR gain keeps a fierce, glowing flame in both the LDR and HDR paths.
    FragColor = vec4(col * bright * 4.0, flame);
}
)GLSL";

// Water / fluid sheet shader (unlit, tinted, animated UV)
static const char* WATER_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* WATER_FS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform sampler2D uTexture;
uniform bool     uHasTex;
uniform vec4     uTint;      // RGBA material tint (front or surface color)
uniform float    uScroll;    // UV scroll phase (drives texture flow)

out vec4 FragColor;

void main() {
    vec2 uv = vUV + vec2(uScroll, uScroll * 0.35);
    vec4 tex = uHasTex ? texture(uTexture, uv) : vec4(1.0);
    vec3 col = tex.rgb * uTint.rgb;
    float a = uTint.a * (uHasTex ? tex.a : 1.0);
    // Soft surface sheen: brighten toward the top of the sheet.
    col += vec3(0.06) * (1.0 - vUV.y);
    FragColor = vec4(col, a);
}
)GLSL";

// Portal effect shader (camera-facing animated swirl) — editor-viewport
// preview of PortalEffectComponent. Procedural: a rotating radial swirl with
// a bright rim, tinted by the component Color and animated by its Speed. No
// external texture required (mirrors fbo_scaler's portal quad, but driven by
// editor scene data + the editor camera instead of SRE frame globals).
static const char* PORTAL_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* PORTAL_FS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform vec3  uColor;    // PortalEffectComponent.Color
uniform float uTime;     // animation clock (seconds)
uniform float uSpeed;    // PortalEffectComponent.Speed magnitude
out vec4 FragColor;
void main() {
    vec2  c = vUV - 0.5;
    float d = length(c) * 2.0;           // 0 center → 1 edge
    if (d > 1.0) discard;
    float ang = atan(c.y, c.x);
    // Swirl: angular + radial phase animated over time by Speed.
    float sw = sin(ang * 5.0 - uTime * uSpeed * 3.0 + d * 8.0) * 0.5 + 0.5;
    // Bright core, soft outer edge, animated swirl band in between.
    float core = 1.0 - smoothstep(0.0, 0.55, d);
    float rim  = smoothstep(0.55, 1.0, d) * (1.0 - smoothstep(0.92, 1.0, d));
    float band = sw * (0.35 + 0.65 * rim);
    float intensity = clamp(core + band, 0.0, 1.5);
    vec3  col = uColor * (0.6 + 0.8 * intensity);
    float alpha = clamp(core * 0.9 + band * 0.8, 0.0, 1.0);
    FragColor = vec4(col, alpha);
}
)GLSL";

// ============================================================================
// Internal state
// ============================================================================

static GLuint s_model_prog = 0;
static GLuint s_grid_prog  = 0;

// Glow sprite program + geometry (camera-facing billboard)
static GLuint s_glow_prog = 0;
static GLuint s_glow_vao = 0, s_glow_vbo = 0, s_glow_ebo = 0;

// Portal effect program (reuses the glow VAO — same pos+UV unit quad)
static GLuint s_portal_prog = 0;
static GLint  s_portal_loc_mvp = -1, s_portal_loc_color = -1,
              s_portal_loc_time = -1, s_portal_loc_speed = -1;
static GLint  s_glow_loc_mvp = -1, s_glow_loc_color = -1;

// Animated fire program (reuses the glow VAO — same pos+UV unit quad)
static GLuint s_fire_prog = 0;
static GLint  s_fire_loc_mvp = -1, s_fire_loc_color = -1,
              s_fire_loc_time = -1, s_fire_loc_flicker = -1;

// Water / fluid sheet program + dynamic geometry (rebuilt per frame)
static GLuint s_water_prog = 0;
static GLuint s_water_vao = 0, s_water_vbo = 0, s_water_ebo = 0;
static GLint  s_water_loc_mvp = -1, s_water_loc_tex = -1;
static GLint  s_water_loc_has_tex = -1, s_water_loc_tint = -1;
static GLint  s_water_loc_scroll = -1;

// Background quad program + geometry (unlit textured plane, depth-write off)
static GLuint s_bg_prog = 0;
static GLuint s_bg_vao = 0, s_bg_vbo = 0, s_bg_ebo = 0;
static GLint  s_bg_loc_mvp = -1, s_bg_loc_tex = -1;

// Model shader uniform locations
static GLint s_loc_mvp        = -1;
static GLint s_loc_model      = -1;
static GLint s_loc_normalmat  = -1;
static GLint s_loc_texture    = -1;
static GLint s_loc_has_tex    = -1;
static GLint s_loc_light_dir  = -1;
static GLint s_loc_light_col  = -1;
static GLint s_loc_ambient    = -1;
static GLint s_loc_amb_ground = -1;
static GLint s_loc_mat_color  = -1;
static GLint s_loc_alpha      = -1;
static GLint s_loc_cam_pos    = -1;
static GLint s_loc_light_cnt  = -1;
static GLint s_loc_light_pos  = -1;
static GLint s_loc_light_cols = -1;
static GLint s_loc_light_rad  = -1;
static GLint s_loc_dir_cnt    = -1;
static GLint s_loc_dir_dir    = -1;
static GLint s_loc_dir_col    = -1;
static GLint s_loc_fog_en     = -1;
static GLint s_loc_fog_col    = -1;
static GLint s_loc_fog_near   = -1;
static GLint s_loc_fog_far    = -1;
static GLint s_loc_flat_shade = -1;
static GLint s_loc_dbg_normals = -1;
static GLint s_loc_fill_col  = -1;
static GLint s_loc_rim_str   = -1;
static GLint s_loc_spec_str  = -1;
static GLint s_loc_flip_v    = -1;
static GLint s_loc_inline_tm = -1;

// HDR path switch (see set_inline_tonemap): when false, render_mesh uploads
// linear-space boosted lights and the mesh shader outputs linear light.
static bool s_inline_tonemap = true;

// ── PBR program state (vendored algorithms, see src/render/zauonlok) ──
struct PBRProg {
    GLuint prog = 0;
    GLint mvp = -1, model = -1, nmat = -1, light_vp = -1, flip_v = -1, has_tan = -1,
          joint_cnt = -1, joints = -1, cam_pos = -1,
          base_f = -1, metal_f = -1, rough_f = -1, spec_f = -1, gloss_f = -1,
          occ = -1, emiss = -1, workflow = -1, alpha_cutoff = -1,
          has_base = -1, has_metal = -1, has_rough = -1, has_nmap = -1,
          has_occ = -1, has_emiss = -1, has_spec = -1, has_gloss = -1,
          env_has = -1, env_int = -1, env_mip = -1, shadow_en = -1,
          light_dir = -1, light_col = -1, ambient = -1, amb_ground = -1,
          light_cnt = -1, light_pos = -1, light_cols = -1, light_rad = -1,
          dir_cnt = -1, dir_dir = -1, dir_col = -1;
} s_pbr;

// Directional shadow-map depth pass
static GLuint s_shadow_prog = 0;
static GLint  s_shadow_loc_vp = -1, s_shadow_loc_model = -1;

// PBR runtime state
static float  s_pbr_joints[256 * 16];  // column-major joint matrices (GPU skinning)
static int    s_pbr_joint_count = 0;
static GLuint s_pbr_env_cubemap = 0;   // prefiltered radiance cubemap (mip chain)
static float  s_pbr_env_intensity = 1.0f;
static GLuint s_pbr_brdf_lut = 0;      // split-sum BRDF LUT (generated at init)
static float  s_shadow_light_vp[16];   // light proj * view (column-major)
static bool   s_shadow_enabled = false;
static GLuint s_shadow_depth_tex = 0;
static GLint  s_shadow_saved_viewport[4] = {0, 0, 0, 0};
static GLuint s_shadow_saved_fbo = 0;
static bool   s_shadow_saved_blend = false;
static bool   s_shadow_saved_scissor = false;

// Depth fog state (set per-frame; disabled by default so model previews stay clear)
static bool   s_fog_enabled = false;
static float  s_fog_color[3] = {0.05f, 0.06f, 0.09f};  // dark cave tint
static float  s_fog_near = 400.0f;
static float  s_fog_far  = 1400.0f;

// Viewport diagnostics (see set_view_flags)
static bool   s_flat_shade = false;
static bool   s_debug_normals = false;

// Scene point lights uploaded per-frame (positions in world space).
static constexpr int MAX_DIR_LIGHTS = 4;   // must match GLSL MAX_DIR_LIGHTS
static float s_point_light_pos[16][3]  = {{0}};
static float s_point_light_col[16][3]  = {{0}};
static float s_point_light_radius[16]  = {0};
static int   s_point_light_count = 0;

// Scene directional lights uploaded per-frame (up to MAX_DIR_LIGHTS).
static float s_dir_light_dir[MAX_DIR_LIGHTS][3] = {{0}};
static float s_dir_light_col[MAX_DIR_LIGHTS][3] = {{0}};
static int   s_dir_light_count = 0;

// Grid shader uniform locations
static GLint s_loc_grid_mvp   = -1;
static GLint s_loc_grid_model = -1;
static GLint s_loc_grid_size  = -1;
static GLint s_loc_grid_color = -1;
static GLint s_loc_grid_scale = -1;

// Viewport height (px) + projection vertical focal factor f = 1/tan(fov/2),
// used to convert world units to screen pixels on the grid plane.
static float s_grid_viewport_h = 1.0f;
static float s_grid_proj_f     = 1.0f;

// Grid geometry (a simple XZ quad — large enough, faded at edges)
static GLuint s_grid_vao = 0;
static GLuint s_grid_vbo = 0;

// Currently active view/proj matrices (set by begin_3d)
static float s_view[16];
static float s_cam_eye[3] = {0, 0, 0};   // camera eye, set by begin_3d (point lights)
static float s_proj[16];
static float s_vp[16];   // view * proj combined

// Saved viewport for end_3d restore
static GLint s_saved_viewport[4] = {0, 0, 0, 0};
static GLuint s_saved_fbo = 0;

// Depth textures for FBOs (sampleable — the PostFX depth-of-field pass reads
// them). We maintain a map-free approach: store one depth texture per FBO via
// a small lookup (asset viewer typically has 1–2 FBOs).
struct FBORecord { GLuint fbo; GLuint depth_tex; };
static std::vector<FBORecord> s_fbo_records;

static GLuint find_depth_for_fbo(GLuint fbo) {
    for (auto& r : s_fbo_records)
        if (r.fbo == fbo) return r.depth_tex;
    return 0;
}

// ============================================================================
// Mat4 math — column-major, right-handed
// ============================================================================

void mat4_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_multiply(float out[16], const float a[16], const float b[16]) {
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_translate(float out[16], float tx, float ty, float tz) {
    mat4_identity(out);
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
}

void mat4_rotate_x(float out[16], float angle_deg) {
    mat4_identity(out);
    float rad = angle_deg * DEG2RAD;
    float c = cosf(rad), s = sinf(rad);
    out[5]  =  c;  out[6]  = s;
    out[9]  = -s;  out[10] = c;
}

void mat4_rotate_y(float out[16], float angle_deg) {
    mat4_identity(out);
    float rad = angle_deg * DEG2RAD;
    float c = cosf(rad), s = sinf(rad);
    out[0]  =  c;  out[2]  = -s;
    out[8]  =  s;  out[10] =  c;
}

void mat4_rotate_z(float out[16], float angle_deg) {
    mat4_identity(out);
    float rad = angle_deg * DEG2RAD;
    float c = cosf(rad), s = sinf(rad);
    out[0]  =  c;  out[1]  =  s;
    out[4]  = -s;  out[5]  =  c;
}

void mat4_perspective(float out[16], float fov_deg, float aspect,
                      float near_p, float far_p) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fov_deg * DEG2RAD * 0.5f);
    out[0]  = f / aspect;
    out[5]  = f;
    out[10] = (far_p + near_p) / (near_p - far_p);
    out[11] = -1.0f;
    out[14] = (2.0f * far_p * near_p) / (near_p - far_p);
}

void mat4_ortho(float out[16], float left, float right, float bottom,
                float top, float near_p, float far_p) {
    // Column-major, right-handed, mirrors THREE.Matrix4.makeOrthographic.
    memset(out, 0, 16 * sizeof(float));
    const float rl = (right - left);
    const float tb = (top - bottom);
    const float fn = (far_p - near_p);
    out[0]  = 2.0f / rl;
    out[5]  = 2.0f / tb;
    out[10] = -2.0f / fn;
    out[12] = -(right + left) / rl;
    out[13] = -(top + bottom) / tb;
    out[14] = -(far_p + near_p) / fn;
    out[15] = 1.0f;
}

void mat4_look_at(float out[16],
                  float ex, float ey, float ez,
                  float cx, float cy, float cz,
                  float ux, float uy, float uz) {
    // Forward = normalize(center - eye)
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    if (flen > 1e-8f) { fx /= flen; fy /= flen; fz /= flen; }

    // Side = normalize(forward × up)
    float sx = fy*uz - fz*uy;
    float sy = fz*ux - fx*uz;
    float sz = fx*uy - fy*ux;
    float slen = sqrtf(sx*sx + sy*sy + sz*sz);
    if (slen > 1e-8f) { sx /= slen; sy /= slen; sz /= slen; }

    // Recompute up = side × forward
    float rx = sy*fz - sz*fy;
    float ry = sz*fx - sx*fz;
    float rz = sx*fy - sy*fx;

    // Column-major layout
    mat4_identity(out);
    out[0] =  sx;  out[4] =  sy;  out[8]  =  sz;
    out[1] =  rx;  out[5] =  ry;  out[9]  =  rz;
    out[2] = -fx;  out[6] = -fy;  out[10] = -fz;
    out[12] = -(sx*ex + sy*ey + sz*ez);
    out[13] = -(rx*ex + ry*ey + rz*ez);
    out[14] =  (fx*ex + fy*ey + fz*ez);
}

/// Extract the upper-left 3×3 from a column-major 4×4 and compute its
/// inverse-transpose for correct normal transformation.
/// Output is 9 floats in column-major order (what glUniformMatrix3fv expects).
void mat4_normal_matrix(float out[9], const float m[16]) {
    // Extract 3x3 (column-major)
    float a00 = m[0], a01 = m[4], a02 = m[8];
    float a10 = m[1], a11 = m[5], a12 = m[9];
    float a20 = m[2], a21 = m[6], a22 = m[10];

    // Cofactors
    float c00 =  (a11*a22 - a12*a21);
    float c01 = -(a10*a22 - a12*a20);
    float c02 =  (a10*a21 - a11*a20);
    float c10 = -(a01*a22 - a02*a21);
    float c11 =  (a00*a22 - a02*a20);
    float c12 = -(a00*a21 - a01*a20);
    float c20 =  (a01*a12 - a02*a11);
    float c21 = -(a00*a12 - a02*a10);
    float c22 =  (a00*a11 - a01*a10);

    float det = a00*c00 + a01*c01 + a02*c02;
    if (fabsf(det) < 1e-12f) {
        // Degenerate — fall back to identity
        memset(out, 0, 9 * sizeof(float));
        out[0] = out[4] = out[8] = 1.0f;
        return;
    }

    float inv_det = 1.0f / det;

    // Inverse-transpose = cofactor / det  (already transposed by cofactor layout)
    // Output column-major: col 0 = row 0 of cofactor matrix / det, etc.
    out[0] = c00 * inv_det;  out[3] = c01 * inv_det;  out[6] = c02 * inv_det;
    out[1] = c10 * inv_det;  out[4] = c11 * inv_det;  out[7] = c12 * inv_det;
    out[2] = c20 * inv_det;  out[5] = c21 * inv_det;  out[8] = c22 * inv_det;
}

void camera_get_view_matrix(const Camera& cam, float out[16]) {
    // Spherical coordinates → eye position
    float yaw_rad   = cam.yaw   * DEG2RAD;
    float pitch_rad = cam.pitch * DEG2RAD;

    float cos_p = cosf(pitch_rad);
    float eye_x = cam.target[0] + cam.distance * cos_p * sinf(yaw_rad);
    float eye_y = cam.target[1] + cam.distance * sinf(pitch_rad);
    float eye_z = cam.target[2] + cam.distance * cos_p * cosf(yaw_rad);

    mat4_look_at(out,
                 eye_x, eye_y, eye_z,
                 cam.target[0], cam.target[1], cam.target[2],
                 0.0f, 1.0f, 0.0f);
}

void camera_get_projection(const Camera& cam, float aspect, float out[16]) {
    if (cam.orthographic) {
        // Match the on-screen scale of the perspective view at the target
        // plane: half-height = distance * tan(fov/2), divided by ortho_zoom
        // so mouse-wheel distance AND the ortho zoom control both frame the
        // scene consistently (parity with THREE.OrthographicCamera).
        const float zoom = (cam.ortho_zoom > 1e-4f) ? cam.ortho_zoom : 1.0f;
        float half_h = cam.distance * tanf(cam.fov * DEG2RAD * 0.5f) / zoom;
        if (half_h < 1e-4f) half_h = 1e-4f;
        const float half_w = half_h * aspect;
        mat4_ortho(out, -half_w, half_w, -half_h, half_h,
                   cam.near_plane, cam.far_plane);
        return;
    }
    mat4_perspective(out, cam.fov, aspect, cam.near_plane, cam.far_plane);
}

bool mat4_inverse(float out[16], const float m[16]) {
    // Cofactor expansion with partial-pivot-free determinant check.
    const float a00 = m[0], a01 = m[4], a02 = m[8],  a03 = m[12];
    const float a10 = m[1], a11 = m[5], a12 = m[9],  a13 = m[13];
    const float a20 = m[2], a21 = m[6], a22 = m[10], a23 = m[14];
    const float a30 = m[3], a31 = m[7], a32 = m[11], a33 = m[15];

    const float b00 = a00*a11 - a01*a10;
    const float b01 = a00*a12 - a02*a10;
    const float b02 = a00*a13 - a03*a10;
    const float b03 = a01*a12 - a02*a11;
    const float b04 = a01*a13 - a03*a11;
    const float b05 = a02*a13 - a03*a12;
    const float b06 = a20*a31 - a21*a30;
    const float b07 = a20*a32 - a22*a30;
    const float b08 = a20*a33 - a23*a30;
    const float b09 = a21*a32 - a22*a31;
    const float b10 = a21*a33 - a23*a31;
    const float b11 = a22*a33 - a23*a32;

    const float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    if (det == 0.0f) return false;
    const float inv = 1.0f / det;

    // The cofactor block below yields the inverse in a transposed (row-major)
    // arrangement.  Compute it into a local, then transpose into the standard
    // column-major layout so that mat4_multiply(out, m) == identity.
    float t[16];
    t[0]  =  (a11*b11 - a12*b10 + a13*b09) * inv;
    t[1]  = -(a01*b11 - a02*b10 + a03*b09) * inv;
    t[2]  =  (a31*b05 - a32*b04 + a33*b03) * inv;
    t[3]  = -(a21*b05 - a22*b04 + a23*b03) * inv;
    t[4]  = -(a10*b11 - a12*b08 + a13*b07) * inv;
    t[5]  =  (a00*b11 - a02*b08 + a03*b07) * inv;
    t[6]  = -(a30*b05 - a32*b02 + a33*b01) * inv;
    t[7]  =  (a20*b05 - a22*b02 + a23*b01) * inv;
    t[8]  =  (a10*b10 - a11*b08 + a13*b06) * inv;
    t[9]  = -(a00*b10 - a01*b08 + a03*b06) * inv;
    t[10] =  (a30*b04 - a31*b02 + a33*b00) * inv;
    t[11] = -(a20*b04 - a21*b02 + a23*b00) * inv;
    t[12] = -(a10*b09 - a11*b07 + a12*b06) * inv;
    t[13] =  (a00*b09 - a01*b07 + a02*b06) * inv;
    t[14] = -(a30*b03 - a31*b01 + a32*b00) * inv;
    t[15] =  (a20*b03 - a21*b01 + a22*b00) * inv;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out[c * 4 + r] = t[r * 4 + c];
    return true;
}

// ============================================================================
// Internal helpers — shader compilation
// ============================================================================

static void dump_shader_source(const char* label, const char* src) {
    fprintf(stderr, "----- [av_renderer] %s source -----\n", label);
    int lineno = 1;
    size_t start = 0;
    for (size_t i = 0; ; ++i) {
        if (src[i] == '\n' || src[i] == '\0') {
            fprintf(stderr, "%4d | ", lineno);
            fwrite(src + start, 1, i - start, stderr);
            fputc('\n', stderr);
            ++lineno;
            if (src[i] == '\0') break;
            start = i + 1;
        }
    }
}

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        fprintf(stderr, "[av_renderer] Shader compile error:\n%s\n", log);
        dump_shader_source(type == GL_VERTEX_SHADER ? "vertex" : "fragment", src);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[av_renderer] Program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static GLuint build_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = link_program(vs, fs);

    // Shaders are ref-counted; safe to delete after linking
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// ============================================================================
// PBR helpers — split-sum BRDF LUT + procedural default environment
// ============================================================================

// Generate the standard GGX split-sum BRDF LUT (256×256 RG16F in an RGBA8
// texture): channel R accumulates the F0=1 specular scale, channel G the
// (1−F0) bias — the classic Karis 2013 "Real Shading in Unreal Engine 4"
// integration. Sampled by the PBR program at (NdotV, roughness).
static GLuint generate_brdf_lut() {
    const int S = 256;
    const int SAMPLES = 512;   // 512×256×256 ≈ 34M iterations; 1024 gains nothing visually
    std::vector<float> lut(S * S * 2, 0.0f);

    auto radical_inverse_vdc = [](unsigned bits) -> float {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return float(bits) * 2.3283064365386963e-10f;
    };
    auto hammersley = [&](int i) -> std::pair<float, float> {
        return { float(i) / float(SAMPLES), radical_inverse_vdc((unsigned)i) };
    };
    // Importance-sample the GGX NDF; returns H in tangent space (N = +Z).
    auto importance_sample_ggx = [](float xi1, float xi2, float a2) -> std::array<float, 3> {
        float phi = 2.0f * PI * xi1;
        float cos_theta = sqrtf(std::max(0.0f, (1.0f - xi2) / (1.0f + (a2 - 1.0f) * xi2)));
        float sin_theta = sqrtf(std::max(0.0f, 1.0f - cos_theta * cos_theta));
        return { sin_theta * cosf(phi), sin_theta * sinf(phi), cos_theta };
    };

    for (int y = 0; y < S; ++y) {
        float roughness = (y + 0.5f) / S;
        float a = roughness * roughness;
        float a2 = a * a;
        for (int x = 0; x < S; ++x) {
            float n_dot_v = (x + 0.5f) / S;
            float Vz = sqrtf(std::max(0.0f, 1.0f - n_dot_v * n_dot_v));
            float Vx = 1.0f, Vy = 0.0f;
            float A = 0.0f, B = 0.0f;
            for (int i = 0; i < SAMPLES; ++i) {
                auto [u1, u2] = hammersley(i);
                auto H = importance_sample_ggx(u1, u2, a2);
                // Reflect V about H: L = 2·(V·H)·H − V
                float v_dot_h = Vx * H[0] + Vy * H[1] + Vz * H[2];
                float Lx = 2.0f * v_dot_h * H[0] - Vx;
                float Ly = 2.0f * v_dot_h * H[1] - Vy;
                float Lz = 2.0f * v_dot_h * H[2] - Vz;
                float n_dot_l = Lz;
                float n_dot_h = H[2];
                if (n_dot_l <= 0.0f || n_dot_h <= 0.0f) continue;
                // Smith G1 (same GGX alpha2) with the 4·NdotV·NdotL folding.
                float g1v = 2.0f * n_dot_v / (n_dot_v + sqrtf(a2 + (1.0f - a2) * n_dot_v * n_dot_v));
                float g1l = 2.0f * n_dot_l / (n_dot_l + sqrtf(a2 + (1.0f - a2) * n_dot_l * n_dot_l));
                float g_vis = g1v * g1l * v_dot_h / (n_dot_h * n_dot_v);
                float fc = powf(1.0f - v_dot_h, 5.0f);
                A += (1.0f - fc) * g_vis;   // F0=1 response
                B += fc * g_vis;             // F0=0 response
            }
            size_t o = ((size_t)y * S + (size_t)x) * 2;
            lut[o + 0] = A / float(SAMPLES);
            lut[o + 1] = B / float(SAMPLES);
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, S, S, 0, GL_RG, GL_FLOAT, lut.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// Procedural default environment: a soft studio gradient cubemap (warm sky
// above, cool ground below) with mip chains, so image-based lighting is
// visible in PBR previews before a real HDR environment is supplied.
static GLuint create_default_env_cubemap() {
    const int S = 64;
    std::vector<uint8_t> face(S * S * 4);
    GLuint cube = 0;
    glGenTextures(1, &cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    for (int f = 0; f < 6; ++f) {
        GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + f;
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                // Reconstruct a direction for this texel so the gradient is
                // consistent across faces (rough sky/ground/ground-floor).
                float u = (x + 0.5f) / S * 2.0f - 1.0f;
                float v = (y + 0.5f) / S * 2.0f - 1.0f;
                float dx = 0, dy = 0, dz = 0;
                switch (f) {
                    case 0: dx =  1.0f; dy = -v; dz = -u; break;  // +X
                    case 1: dx = -1.0f; dy = -v; dz =  u; break;  // -X
                    case 2: dx =  u; dy =  1.0f; dz =  v; break;  // +Y (up)
                    case 3: dx =  u; dy = -1.0f; dz = -v; break;  // -Y (down)
                    case 4: dx =  u; dy = -v; dz =  1.0f; break;  // +Z
                    default: dx = -u; dy = -v; dz = -1.0f; break; // -Z
                }
                float len = sqrtf(dx * dx + dy * dy + dz * dz);
                if (len > 1e-6f) { dx /= len; dy /= len; dz /= len; }
                float up = std::clamp(dy * 0.5f + 0.5f, 0.0f, 1.0f);
                // sky: pale warm blue · horizon: light cream · ground: muted olive
                float sky_r = 0.55f, sky_g = 0.66f, sky_b = 0.82f;
                float hor_r = 0.95f, hor_g = 0.88f, hor_b = 0.78f;
                float gnd_r = 0.22f, gnd_g = 0.20f, gnd_b = 0.16f;
                float t = powf(up, 0.55f);
                auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
                float r = lerp(lerp(gnd_r, hor_r, up), sky_r, t);
                float g = lerp(lerp(gnd_g, hor_g, up), sky_g, t);
                float b = lerp(lerp(gnd_b, hor_b, up), sky_b, t);
                size_t o = ((size_t)y * S + (size_t)x) * 4;
                face[o + 0] = (uint8_t)(r * 255.0f);
                face[o + 1] = (uint8_t)(g * 255.0f);
                face[o + 2] = (uint8_t)(b * 255.0f);
                face[o + 3] = 255;
            }
        }
        glTexImage2D(target, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, face.data());
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return cube;
}

// ============================================================================
// Grid geometry setup (a single large XZ quad)
// ============================================================================

static void create_grid_geometry() {
    // Unit quad from -1 to +1 on XZ, centered at origin.
    // Actual size is scaled by render_grid's `size` parameter via the model matrix.
    // clang-format off
    static const float verts[] = {
        // x,   y,  z
        -1.0f, 0.0f, -1.0f,
         1.0f, 0.0f, -1.0f,
         1.0f, 0.0f,  1.0f,
        -1.0f, 0.0f,  1.0f,
    };
    static const uint16_t indices[] = { 0, 1, 2,  0, 2, 3 };
    // clang-format on

    glGenVertexArrays(1, &s_grid_vao);
    glBindVertexArray(s_grid_vao);

    glGenBuffers(1, &s_grid_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // location 0 = position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glBindVertexArray(0);
}

// ============================================================================
// Public API — lifecycle
// ============================================================================

bool renderer_init() {
    // --- Model program ---
    s_model_prog = build_program(MODEL_VS, MODEL_FS);
    if (!s_model_prog) {
        fprintf(stderr, "[av_renderer] Failed to build model shader program.\n");
        return false;
    }

    s_loc_mvp       = glGetUniformLocation(s_model_prog, "uMVP");
    s_loc_model     = glGetUniformLocation(s_model_prog, "uModel");
    s_loc_normalmat = glGetUniformLocation(s_model_prog, "uNormalMat");
    s_loc_texture   = glGetUniformLocation(s_model_prog, "uTexture");
    s_loc_has_tex   = glGetUniformLocation(s_model_prog, "uHasTexture");
    s_loc_light_dir = glGetUniformLocation(s_model_prog, "uLightDir");
    s_loc_light_col = glGetUniformLocation(s_model_prog, "uLightColor");
    s_loc_ambient   = glGetUniformLocation(s_model_prog, "uAmbient");
    s_loc_amb_ground= glGetUniformLocation(s_model_prog, "uAmbientGround");
    s_loc_mat_color = glGetUniformLocation(s_model_prog, "uMatColor");
    s_loc_cam_pos   = glGetUniformLocation(s_model_prog, "uCamPos");
    s_loc_light_cnt = glGetUniformLocation(s_model_prog, "uLightCount");
    s_loc_light_pos = glGetUniformLocation(s_model_prog, "uLightPos[0]");
    s_loc_light_cols= glGetUniformLocation(s_model_prog, "uLightCol[0]");
    s_loc_light_rad = glGetUniformLocation(s_model_prog, "uLightRadius[0]");
    s_loc_dir_cnt   = glGetUniformLocation(s_model_prog, "uDirLightCount");
    s_loc_dir_dir   = glGetUniformLocation(s_model_prog, "uDirLightDir[0]");
    s_loc_dir_col   = glGetUniformLocation(s_model_prog, "uDirLightCol[0]");
    s_loc_fog_en    = glGetUniformLocation(s_model_prog, "uFogEnabled");
    s_loc_fog_col   = glGetUniformLocation(s_model_prog, "uFogColor");
    s_loc_fog_near  = glGetUniformLocation(s_model_prog, "uFogNear");
    s_loc_fog_far   = glGetUniformLocation(s_model_prog, "uFogFar");
    s_loc_alpha     = glGetUniformLocation(s_model_prog, "uAlpha");
    s_loc_flat_shade = glGetUniformLocation(s_model_prog, "uFlatShade");
    s_loc_dbg_normals = glGetUniformLocation(s_model_prog, "uDebugNormals");
    s_loc_fill_col  = glGetUniformLocation(s_model_prog, "uFillColor");
    s_loc_rim_str   = glGetUniformLocation(s_model_prog, "uRimStrength");
    s_loc_spec_str  = glGetUniformLocation(s_model_prog, "uSpecStrength");
    s_loc_flip_v    = glGetUniformLocation(s_model_prog, "uFlipV");
    s_loc_inline_tm = glGetUniformLocation(s_model_prog, "uInlineTonemap");

    // --- PBR program (vendored algorithms from zauonlok/renderer, MIT) ---
    // Non-fatal: if the PBR program fails to build, the legacy model path
    // keeps working and pbr_render_mesh() simply no-ops.
    s_pbr.prog = build_program(PBR_VS, PBR_FS);
    if (s_pbr.prog) {
        s_pbr.mvp         = glGetUniformLocation(s_pbr.prog, "uMVP");
        s_pbr.model       = glGetUniformLocation(s_pbr.prog, "uModel");
        s_pbr.nmat        = glGetUniformLocation(s_pbr.prog, "uNormalMat");
        s_pbr.light_vp    = glGetUniformLocation(s_pbr.prog, "uLightVPMatrix");
        s_pbr.flip_v      = glGetUniformLocation(s_pbr.prog, "uFlipV");
        s_pbr.has_tan     = glGetUniformLocation(s_pbr.prog, "uHasTangent");
        s_pbr.joint_cnt   = glGetUniformLocation(s_pbr.prog, "uJointCount");
        s_pbr.joints      = glGetUniformLocation(s_pbr.prog, "uJoints[0]");
        s_pbr.cam_pos     = glGetUniformLocation(s_pbr.prog, "uCamPos");
        s_pbr.base_f      = glGetUniformLocation(s_pbr.prog, "uBasecolorFactor");
        s_pbr.metal_f     = glGetUniformLocation(s_pbr.prog, "uMetalnessFactor");
        s_pbr.rough_f     = glGetUniformLocation(s_pbr.prog, "uRoughnessFactor");
        s_pbr.spec_f      = glGetUniformLocation(s_pbr.prog, "uSpecularFactor");
        s_pbr.gloss_f     = glGetUniformLocation(s_pbr.prog, "uGlossinessFactor");
        s_pbr.occ         = glGetUniformLocation(s_pbr.prog, "uOcclusion");
        s_pbr.emiss       = glGetUniformLocation(s_pbr.prog, "uEmission");
        s_pbr.workflow    = glGetUniformLocation(s_pbr.prog, "uWorkflow");
        s_pbr.alpha_cutoff= glGetUniformLocation(s_pbr.prog, "uAlphaCutoff");
        s_pbr.has_base    = glGetUniformLocation(s_pbr.prog, "uHasBasecolor");
        s_pbr.has_metal   = glGetUniformLocation(s_pbr.prog, "uHasMetalness");
        s_pbr.has_rough   = glGetUniformLocation(s_pbr.prog, "uHasRoughness");
        s_pbr.has_nmap    = glGetUniformLocation(s_pbr.prog, "uHasNormalMap");
        s_pbr.has_occ     = glGetUniformLocation(s_pbr.prog, "uHasOcclusion");
        s_pbr.has_emiss   = glGetUniformLocation(s_pbr.prog, "uHasEmission");
        s_pbr.has_spec    = glGetUniformLocation(s_pbr.prog, "uHasSpecular");
        s_pbr.has_gloss   = glGetUniformLocation(s_pbr.prog, "uHasGlossiness");
        s_pbr.env_has     = glGetUniformLocation(s_pbr.prog, "uHasEnv");
        s_pbr.env_int     = glGetUniformLocation(s_pbr.prog, "uEnvIntensity");
        s_pbr.env_mip     = glGetUniformLocation(s_pbr.prog, "uEnvMaxMip");
        s_pbr.shadow_en   = glGetUniformLocation(s_pbr.prog, "uShadowEnabled");
        s_pbr.light_dir   = glGetUniformLocation(s_pbr.prog, "uLightDir");
        s_pbr.light_col   = glGetUniformLocation(s_pbr.prog, "uLightColor");
        s_pbr.ambient     = glGetUniformLocation(s_pbr.prog, "uAmbient");
        s_pbr.amb_ground  = glGetUniformLocation(s_pbr.prog, "uAmbientGround");
        s_pbr.light_cnt   = glGetUniformLocation(s_pbr.prog, "uLightCount");
        s_pbr.light_pos   = glGetUniformLocation(s_pbr.prog, "uLightPos[0]");
        s_pbr.light_cols  = glGetUniformLocation(s_pbr.prog, "uLightCol[0]");
        s_pbr.light_rad   = glGetUniformLocation(s_pbr.prog, "uLightRadius[0]");
        s_pbr.dir_cnt     = glGetUniformLocation(s_pbr.prog, "uDirLightCount");
        s_pbr.dir_dir     = glGetUniformLocation(s_pbr.prog, "uDirLightDir[0]");
        s_pbr.dir_col     = glGetUniformLocation(s_pbr.prog, "uDirLightCol[0]");

        // Bind texture units once (they never change).
        glUseProgram(s_pbr.prog);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uBasecolorTex"), 0);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uMetalnessTex"), 1);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uRoughnessTex"), 2);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uNormalTex"), 3);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uOcclusionTex"), 4);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uEmissionTex"), 5);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uSpecularTex"), 6);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uGlossinessTex"), 7);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uEnvMap"), 8);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uBRDF_LUT"), 9);
        glUniform1i(glGetUniformLocation(s_pbr.prog, "uShadowMap"), 10);
        glUseProgram(0);

        s_pbr_brdf_lut = generate_brdf_lut();
        s_pbr_env_cubemap = create_default_env_cubemap();
        fprintf(stderr, "[av_renderer] PBR program ready (BRDF LUT=%u, env=%u).\n",
                s_pbr_brdf_lut, s_pbr_env_cubemap);
    } else {
        fprintf(stderr, "[av_renderer] [warn] PBR program failed to build; PBR preview disabled.\n");
    }

    // --- Directional shadow-map depth pass ---
    s_shadow_prog = build_program(SHADOW_VS, SHADOW_FS);
    if (s_shadow_prog) {
        s_shadow_loc_vp    = glGetUniformLocation(s_shadow_prog, "uLightVP");
        s_shadow_loc_model = glGetUniformLocation(s_shadow_prog, "uModel");
    }

    // --- Grid program ---
    // The grid is editor decoration only. If it fails to compile, we log a
    // warning and continue: the model/scene renderer must never be taken down
    // by a broken grid shader. render_grid/render_grid_xy already guard on
    // s_grid_prog being non-zero, so a disabled grid is harmless.
    s_grid_prog = build_program(GRID_VS, GRID_FS);
    if (!s_grid_prog) {
        fprintf(stderr, "[av_renderer] [warn] Grid shader failed to build; grid is disabled (model/scene rendering continues).\n");
    } else {
        s_loc_grid_mvp   = glGetUniformLocation(s_grid_prog, "uMVP");
        s_loc_grid_model = glGetUniformLocation(s_grid_prog, "uModel");
        s_loc_grid_size  = glGetUniformLocation(s_grid_prog, "uGridSize");
        s_loc_grid_color = glGetUniformLocation(s_grid_prog, "uGridColor");
        s_loc_grid_scale = glGetUniformLocation(s_grid_prog, "uScale");

        // --- Grid geometry ---
        create_grid_geometry();
    }

    // --- Background quad geometry (unit quad ±1 on X/Y, UV 0..1) ---
    s_bg_prog = build_program(BG_VS, BG_FS);
    if (s_bg_prog) {
        s_bg_loc_mvp = glGetUniformLocation(s_bg_prog, "uMVP");
        s_bg_loc_tex = glGetUniformLocation(s_bg_prog, "uTexture");

        static const float bg_verts[] = {
            // x,    y,    z,   u,   v
            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 0.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
        };
        static const uint16_t bg_idx[] = { 0, 1, 2, 0, 2, 3 };

        glGenVertexArrays(1, &s_bg_vao);
        glBindVertexArray(s_bg_vao);
        glGenBuffers(1, &s_bg_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_bg_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bg_verts), bg_verts, GL_STATIC_DRAW);
        glGenBuffers(1, &s_bg_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_bg_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(bg_idx), bg_idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[av_renderer] Failed to build background quad program.\n");
    }

    // --- Glow sprite geometry (unit quad ±1 on X/Y, UV 0..1) ---
    s_glow_prog = build_program(GLOW_VS, GLOW_FS);
    if (s_glow_prog) {
        s_glow_loc_mvp   = glGetUniformLocation(s_glow_prog, "uMVP");
        s_glow_loc_color = glGetUniformLocation(s_glow_prog, "uColor");

        static const float glow_verts[] = {
            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 0.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
        };
        static const uint16_t glow_idx[] = { 0, 1, 2, 0, 2, 3 };

        glGenVertexArrays(1, &s_glow_vao);
        glBindVertexArray(s_glow_vao);
        glGenBuffers(1, &s_glow_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_glow_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(glow_verts), glow_verts, GL_STATIC_DRAW);
        glGenBuffers(1, &s_glow_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_glow_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(glow_idx), glow_idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[av_renderer] Failed to build glow sprite program.\n");
    }

    // --- Portal effect program (reuses the glow unit-quad VAO) ---
    s_portal_prog = build_program(PORTAL_VS, PORTAL_FS);
    if (s_portal_prog) {
        s_portal_loc_mvp   = glGetUniformLocation(s_portal_prog, "uMVP");
        s_portal_loc_color = glGetUniformLocation(s_portal_prog, "uColor");
        s_portal_loc_time  = glGetUniformLocation(s_portal_prog, "uTime");
        s_portal_loc_speed = glGetUniformLocation(s_portal_prog, "uSpeed");
    } else {
        fprintf(stderr, "[av_renderer] Failed to build portal effect program.\n");
    }

    // --- Animated fire program (reuses the glow unit-quad VAO + GLOW_VS) ---
    s_fire_prog = build_program(GLOW_VS, FIRE_FS);
    if (s_fire_prog) {
        s_fire_loc_mvp     = glGetUniformLocation(s_fire_prog, "uMVP");
        s_fire_loc_color   = glGetUniformLocation(s_fire_prog, "uColor");
        s_fire_loc_time    = glGetUniformLocation(s_fire_prog, "uTime");
        s_fire_loc_flicker = glGetUniformLocation(s_fire_prog, "uFlicker");
    } else {
        fprintf(stderr, "[av_renderer] Failed to build fire program.\n");
    }

    // --- Water / fluid sheet program + dynamic geometry ---
    // The vertex buffer is re-filled every frame (grid waves), so it is
    // created with GL_DYNAMIC_DRAW in render_water_sheet; here we only set
    // up the attribute layout once.
    s_water_prog = build_program(WATER_VS, WATER_FS);
    if (s_water_prog) {
        s_water_loc_mvp     = glGetUniformLocation(s_water_prog, "uMVP");
        s_water_loc_tex     = glGetUniformLocation(s_water_prog, "uTexture");
        s_water_loc_has_tex = glGetUniformLocation(s_water_prog, "uHasTex");
        s_water_loc_tint    = glGetUniformLocation(s_water_prog, "uTint");
        s_water_loc_scroll  = glGetUniformLocation(s_water_prog, "uScroll");

        glGenVertexArrays(1, &s_water_vao);
        glBindVertexArray(s_water_vao);
        glGenBuffers(1, &s_water_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_water_vbo);
        // Positions (3) + UV (2) = 5 floats; indices are uint16 (<= 65k verts).
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glGenBuffers(1, &s_water_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_water_ebo);
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[av_renderer] Failed to build water sheet program.\n");
    }

    fprintf(stderr, "[av_renderer] Initialized (model prog=%u, grid prog=%u).\n",
            s_model_prog, s_grid_prog);
    return true;
}

void renderer_shutdown() {
    if (s_model_prog) { glDeleteProgram(s_model_prog); s_model_prog = 0; }
    if (s_grid_prog)  { glDeleteProgram(s_grid_prog);  s_grid_prog  = 0; }

    if (s_grid_vao) {
        glDeleteVertexArrays(1, &s_grid_vao);
        s_grid_vao = 0;
    }
    if (s_grid_vbo) {
        glDeleteBuffers(1, &s_grid_vbo);
        s_grid_vbo = 0;
    }

    // Clean up tracked FBO depth textures
    for (auto& r : s_fbo_records) {
        if (r.depth_tex) glDeleteTextures(1, &r.depth_tex);
    }
    s_fbo_records.clear();

    if (s_bg_prog) { glDeleteProgram(s_bg_prog); s_bg_prog = 0; }
    if (s_bg_vao)  { glDeleteVertexArrays(1, &s_bg_vao); s_bg_vao = 0; }
    if (s_bg_vbo)  { glDeleteBuffers(1, &s_bg_vbo); s_bg_vbo = 0; }
    if (s_bg_ebo)  { glDeleteBuffers(1, &s_bg_ebo); s_bg_ebo = 0; }

    if (s_glow_prog) { glDeleteProgram(s_glow_prog); s_glow_prog = 0; }
    if (s_glow_vao)  { glDeleteVertexArrays(1, &s_glow_vao); s_glow_vao = 0; }
    if (s_glow_vbo)  { glDeleteBuffers(1, &s_glow_vbo); s_glow_vbo = 0; }
    if (s_glow_ebo)  { glDeleteBuffers(1, &s_glow_ebo); s_glow_ebo = 0; }
    if (s_portal_prog) { glDeleteProgram(s_portal_prog); s_portal_prog = 0; }
    if (s_fire_prog) { glDeleteProgram(s_fire_prog); s_fire_prog = 0; }

    if (s_water_prog) { glDeleteProgram(s_water_prog); s_water_prog = 0; }
    if (s_water_vao)  { glDeleteVertexArrays(1, &s_water_vao); s_water_vao = 0; }
    if (s_water_vbo)  { glDeleteBuffers(1, &s_water_vbo); s_water_vbo = 0; }
    if (s_water_ebo)  { glDeleteBuffers(1, &s_water_ebo); s_water_ebo = 0; }

    if (s_pbr.prog)        { glDeleteProgram(s_pbr.prog);        s_pbr.prog = 0; }
    if (s_shadow_prog)     { glDeleteProgram(s_shadow_prog);     s_shadow_prog = 0; }
    if (s_pbr_brdf_lut)    { glDeleteTextures(1, &s_pbr_brdf_lut);    s_pbr_brdf_lut = 0; }
    if (s_pbr_env_cubemap) { glDeleteTextures(1, &s_pbr_env_cubemap); s_pbr_env_cubemap = 0; }

    postfx_shutdown();
}

// ============================================================================
// Mesh upload / free
// ============================================================================

GPUMesh upload_mesh(const float* positions, const float* normals, const float* uvs,
                    int num_verts, const uint32_t* indices, int num_indices) {
    return upload_mesh_ex(positions, normals, uvs, nullptr, nullptr, nullptr,
                          num_verts, indices, num_indices, false);
}

GPUMesh upload_mesh_ex(const float* positions, const float* normals, const float* uvs,
                       const float* tangents4, const float* joints4, const float* weights4,
                       int num_verts, const uint32_t* indices, int num_indices,
                       bool flip_uv_v) {
    GPUMesh mesh{};
    if (!positions || num_verts <= 0) return mesh;

    mesh.flip_uv_v = flip_uv_v;
    mesh.has_tangents = (tangents4 != nullptr);
    mesh.has_skinning = (joints4 != nullptr && weights4 != nullptr);

    // Interleaved layouts:
    //   base     [pos3][nrm3][uv2]              = 8  floats
    //   +tangent [pos3][nrm3][uv2][tan4]        = 12 floats
    //   +skin    [pos3][nrm3][uv2][tan4][j4][w4] = 20 floats
    const int stride_floats = 8 + (mesh.has_tangents ? 4 : 0) + (mesh.has_skinning ? 8 : 0);
    mesh.stride_floats = stride_floats;
    std::vector<float> buf(static_cast<size_t>(num_verts) * stride_floats);

    for (int i = 0; i < num_verts; i++) {
        float* dst = &buf[static_cast<size_t>(i) * stride_floats];
        int o = 0;
        dst[o++] = positions[i * 3 + 0];
        dst[o++] = positions[i * 3 + 1];
        dst[o++] = positions[i * 3 + 2];
        if (normals) { dst[o++] = normals[i * 3 + 0]; dst[o++] = normals[i * 3 + 1]; dst[o++] = normals[i * 3 + 2]; }
        else { dst[o++] = 0.0f; dst[o++] = 1.0f; dst[o++] = 0.0f; }
        if (uvs) { dst[o++] = uvs[i * 2 + 0]; dst[o++] = uvs[i * 2 + 1]; }
        else { dst[o++] = 0.0f; dst[o++] = 0.0f; }
        if (mesh.has_tangents) {
            dst[o++] = tangents4[i * 4 + 0];
            dst[o++] = tangents4[i * 4 + 1];
            dst[o++] = tangents4[i * 4 + 2];
            dst[o++] = tangents4[i * 4 + 3];
        }
        if (mesh.has_skinning) {
            for (int k = 0; k < 4; ++k) dst[o++] = joints4[i * 4 + k];
            for (int k = 0; k < 4; ++k) dst[o++] = weights4[i * 4 + k];
        }
    }

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 buf.size() * sizeof(float), buf.data(), GL_STATIC_DRAW);

    const GLsizei stride = stride_floats * sizeof(float);

    // location 0 = aPos (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(0));
    // location 1 = aNorm (vec3)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    // location 2 = aUV (vec2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    // location 3 = aTangent (vec4)
    if (mesh.has_tangents) {
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    }
    // location 4/5 = aJoint / aWeight (vec4)
    if (mesh.has_skinning) {
        const int so = 8 + (mesh.has_tangents ? 4 : 0);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(so * sizeof(float)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)((so + 4) * sizeof(float)));
    }

    // EBO — index buffer (optional)
    if (indices && num_indices > 0) {
        glGenBuffers(1, &mesh.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     num_indices * sizeof(uint32_t), indices, GL_STATIC_DRAW);
        mesh.index_count = num_indices;
    } else {
        mesh.index_count = num_verts;  // non-indexed: draw all verts
    }

    glBindVertexArray(0);
    return mesh;
}

void set_mesh_flip_uv(GPUMesh& mesh, bool flip) {
    mesh.flip_uv_v = flip;
}

void update_mesh_vertices(const GPUMesh& mesh, const float* positions,
                          const float* normals, const float* uvs, int num_verts) {
    if (!mesh.vbo || !positions || num_verts <= 0) return;
    constexpr int stride_floats = 8;
    std::vector<float> data(static_cast<size_t>(num_verts) * stride_floats);
    for (int i = 0; i < num_verts; ++i) {
        float* dst = &data[static_cast<size_t>(i) * stride_floats];
        std::memcpy(dst, positions + i * 3, 3 * sizeof(float));
        if (normals) std::memcpy(dst + 3, normals + i * 3, 3 * sizeof(float));
        else { dst[3] = 0; dst[4] = 1; dst[5] = 0; }
        if (uvs) std::memcpy(dst + 6, uvs + i * 2, 2 * sizeof(float));
        else { dst[6] = dst[7] = 0; }
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, data.size() * sizeof(float), data.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/// Stride-aware vertex update for PBR-layout meshes (tangent/joint/weight
/// attributes preserved across CPU skinning updates). Pass nullptr for any
/// stream that should keep its previously uploaded values.
void update_mesh_vertices_ex(const GPUMesh& mesh, const float* positions,
                             const float* normals, const float* uvs,
                             const float* tangents4, const float* joints4,
                             const float* weights4, int num_verts) {
    if (!mesh.vbo || !positions || num_verts <= 0) return;
    const int stride_floats = mesh.stride_floats;
    std::vector<float> data(static_cast<size_t>(num_verts) * stride_floats);
    for (int i = 0; i < num_verts; ++i) {
        float* dst = &data[static_cast<size_t>(i) * stride_floats];
        int o = 0;
        dst[o++] = positions[i * 3 + 0];
        dst[o++] = positions[i * 3 + 1];
        dst[o++] = positions[i * 3 + 2];
        if (normals) { dst[o++] = normals[i * 3 + 0]; dst[o++] = normals[i * 3 + 1]; dst[o++] = normals[i * 3 + 2]; }
        else { dst[o++] = 0; dst[o++] = 1; dst[o++] = 0; }
        if (uvs) { dst[o++] = uvs[i * 2 + 0]; dst[o++] = uvs[i * 2 + 1]; }
        else { dst[o++] = 0; dst[o++] = 0; }
        if (mesh.has_tangents) {
            if (tangents4) for (int k = 0; k < 4; ++k) dst[o++] = tangents4[i * 4 + k];
            else o += 4;
        }
        if (mesh.has_skinning) {
            if (joints4)   for (int k = 0; k < 4; ++k) dst[o++] = joints4[i * 4 + k];
            else o += 4;
            if (weights4)  for (int k = 0; k < 4; ++k) dst[o++] = weights4[i * 4 + k];
            else o += 4;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, data.size() * sizeof(float), data.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void free_mesh(GPUMesh& mesh) {
    if (mesh.ebo) { glDeleteBuffers(1, &mesh.ebo); }
    if (mesh.vbo) { glDeleteBuffers(1, &mesh.vbo); }
    if (mesh.vao) { glDeleteVertexArrays(1, &mesh.vao); }
    mesh = GPUMesh{};
}

// ============================================================================
// FBO management
// ============================================================================

unsigned int create_fbo(int width, int height, unsigned int* out_tex) {
    GLuint fbo, tex, depth_tex;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // Color attachment — RGBA8 texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    // Depth attachment — texture (sampleable by the PostFX DOF pass)
    glGenTextures(1, &depth_tex);
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[av_renderer] FBO incomplete: 0x%x\n", status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteTextures(1, &depth_tex);
        if (out_tex) *out_tex = 0;
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Track the depth texture so we can resize/delete later
    s_fbo_records.push_back({fbo, depth_tex});

    if (out_tex) *out_tex = tex;
    return fbo;
}

unsigned int create_fbo_hdr(int width, int height, unsigned int* out_tex) {
    // Same layout as create_fbo but with an RGBA16F color attachment so the
    // scene can live in LINEAR light (values > 1 feed the bloom pass real
    // HDR energy). Falls back to RGBA8 when half-float rendering is
    // unavailable — the pipeline stays correct, just clamps at 1.0.
    GLuint fbo, tex, depth_tex;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    glGenTextures(1, &depth_tex);
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // Half-float render target unsupported — fall back to RGBA8.
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[av_renderer] HDR FBO incomplete: 0x%x\n", status);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            glDeleteTextures(1, &depth_tex);
            if (out_tex) *out_tex = 0;
            return 0;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_fbo_records.push_back({fbo, depth_tex});
    if (out_tex) *out_tex = tex;
    return fbo;
}

static void resize_fbo_internal(unsigned int fbo, int w, int h,
                                unsigned int* tex, GLint internal_format,
                                GLenum format, GLenum type) {
    if (!fbo || !tex || w <= 0 || h <= 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, type, nullptr);
    GLuint depth_tex = find_depth_for_fbo(fbo);
    if (depth_tex) {
        glBindTexture(GL_TEXTURE_2D, depth_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void resize_fbo_hdr(unsigned int fbo, int w, int h, unsigned int* tex) {
    resize_fbo_internal(fbo, w, h, tex, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
}

void resize_fbo(unsigned int fbo, int w, int h, unsigned int* tex) {
    if (!fbo || !tex || w <= 0 || h <= 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // Resize color texture
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Resize depth texture
    GLuint depth_tex = find_depth_for_fbo(fbo);
    if (depth_tex) {
        glBindTexture(GL_TEXTURE_2D, depth_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

unsigned int fbo_depth_texture(unsigned int fbo) {
    return find_depth_for_fbo(fbo);
}

void delete_fbo(unsigned int fbo, unsigned int tex) {
    if (tex) glDeleteTextures(1, &tex);

    // Find and delete associated depth texture
    for (auto it = s_fbo_records.begin(); it != s_fbo_records.end(); ++it) {
        if (it->fbo == fbo) {
            if (it->depth_tex) glDeleteTextures(1, &it->depth_tex);
            s_fbo_records.erase(it);
            break;
        }
    }

    if (fbo) glDeleteFramebuffers(1, &fbo);
}

// ============================================================================
// Rendering
// ============================================================================

void set_point_lights(const float pos[][3], const float col[][3],
                      const float* radius, int count) {
    s_point_light_count = count < 0 ? 0 : (count > 16 ? 16 : count);
    for (int i = 0; i < s_point_light_count; ++i) {
        s_point_light_pos[i][0] = pos[i][0];
        s_point_light_pos[i][1] = pos[i][1];
        s_point_light_pos[i][2] = pos[i][2];
        s_point_light_col[i][0] = col[i][0];
        s_point_light_col[i][1] = col[i][1];
        s_point_light_col[i][2] = col[i][2];
        s_point_light_radius[i] = radius[i];
    }
}

void clear_point_lights() {
    s_point_light_count = 0;
}

void set_directional_lights(const float dir[][3], const float col[][3], int count) {
    s_dir_light_count = count < 0 ? 0 : (count > MAX_DIR_LIGHTS ? MAX_DIR_LIGHTS : count);
    for (int i = 0; i < s_dir_light_count; ++i) {
        s_dir_light_dir[i][0] = dir[i][0];
        s_dir_light_dir[i][1] = dir[i][1];
        s_dir_light_dir[i][2] = dir[i][2];
        s_dir_light_col[i][0] = col[i][0];
        s_dir_light_col[i][1] = col[i][1];
        s_dir_light_col[i][2] = col[i][2];
    }
}

void clear_directional_lights() {
    s_dir_light_count = 0;
}

void set_depth_fog(bool enabled, const float color[3], float near_dist, float far_dist) {
    s_fog_enabled = enabled;
    if (color) {
        s_fog_color[0] = color[0];
        s_fog_color[1] = color[1];
        s_fog_color[2] = color[2];
    }
    s_fog_near = near_dist;
    s_fog_far  = far_dist;
}

void set_view_flags(bool flat_shade, bool debug_normals) {
    s_flat_shade = flat_shade;
    s_debug_normals = debug_normals;
}

void set_inline_tonemap(bool enabled) {
    s_inline_tonemap = enabled;
}

void begin_3d(unsigned int fbo, int w, int h, const Camera& cam) {
    // Every pass starts with the diagnostics OFF — the scene visualizer opts
    // into flat/normal view after its begin_3d, so POD previews, thumbnails
    // and the GM preview can never inherit stale flags. Same for the HDR
    // inline-tonemap switch: passes opt OUT after begin_3d (scene view with
    // PostFX HD); everything else tonemaps inline into LDR by default.
    s_flat_shade = false;
    s_debug_normals = false;
    s_inline_tonemap = true;

    // Save current state for restore in end_3d
    glGetIntegerv(GL_VIEWPORT, s_saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&s_saved_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);

    // Clear with dark background
    glClearColor(g_clear_color[0], g_clear_color[1], g_clear_color[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Standard 3D state
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Swordigo assets contain both winding conventions (POD and embedded
    // GroundMesh). Keep both visible in the editor; this is an inspection and
    // authoring viewport, not a back-face-culled game pass.
    glDisable(GL_CULL_FACE);

    // Compute view and projection matrices
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    camera_get_view_matrix(cam, s_view);
    camera_get_projection(cam, aspect, s_proj);
    mat4_multiply(s_vp, s_proj, s_view);
    s_grid_viewport_h = (float)(h > 0 ? h : 1);
    s_grid_proj_f     = s_proj[5]; // 1 / tan(fov/2)

    // Camera eye position for point-light calculations (inverse view translate).
    s_cam_eye[0] = cam.target[0] + cam.distance * cosf(cam.pitch * 3.14159265358979323846f / 180.0f) * sinf(cam.yaw * 3.14159265358979323846f / 180.0f);
    s_cam_eye[1] = cam.target[1] + cam.distance * sinf(cam.pitch * 3.14159265358979323846f / 180.0f);
    s_cam_eye[2] = cam.target[2] + cam.distance * cosf(cam.pitch * 3.14159265358979323846f / 180.0f) * cosf(cam.yaw * 3.14159265358979323846f / 180.0f);
}

void render_mesh(const GPUMesh& mesh, const float* model_matrix,
                 const float color[4], bool wireframe) {
    if (!mesh.vao || !s_model_prog) return;

    // GL state is global and other passes (contact shadow blobs, glows) can
    // leave a modified blend function behind — pin ours explicitly so an
    // opaque mesh can never be rendered through a multiplicative blend (which
    // turns it black).
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Default model = identity
    float model[16];
    if (model_matrix) {
        memcpy(model, model_matrix, 16 * sizeof(float));
    } else {
        mat4_identity(model);
    }

    // MVP = proj * view * model
    float mvp[16];
    mat4_multiply(mvp, s_vp, model);

    // Normal matrix (inverse-transpose of upper 3x3 of model)
    float nmat[9];
    mat4_normal_matrix(nmat, model);

    // Default material color
    float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (color) { col[0] = color[0]; col[1] = color[1]; col[2] = color[2]; col[3] = color[3]; }

    glUseProgram(s_model_prog);

    glUniformMatrix4fv(s_loc_mvp,       1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_loc_model,     1, GL_FALSE, model);
    glUniformMatrix3fv(s_loc_normalmat, 1, GL_FALSE, nmat);

    glUniform3fv(s_loc_light_dir, 1, g_light_dir);
    // Linear-space boosts for the HDR path: with sRGB→linear albedo (darker
    // mids) and tone mapping owned by PostFX, lights need to carry honest
    // linear energy. The legacy LDR path keeps the old tuned values.
    const float key_boost = s_inline_tonemap ? 1.0f : 2.35f;
    const float amb_boost = s_inline_tonemap ? 1.0f : 1.35f;
    float lcol[3] = { g_light_color[0] * g_light_scale * key_boost,
                      g_light_color[1] * g_light_scale * key_boost,
                      g_light_color[2] * g_light_scale * key_boost };
    float amb[3] = { g_ambient_color[0] * g_ambient_scale * amb_boost,
                     g_ambient_color[1] * g_ambient_scale * amb_boost,
                     g_ambient_color[2] * g_ambient_scale * amb_boost };
    float ambg[3] = { g_ambient_ground_color[0] * g_ambient_scale * amb_boost,
                      g_ambient_ground_color[1] * g_ambient_scale * amb_boost,
                      g_ambient_ground_color[2] * g_ambient_scale * amb_boost };
    glUniform3fv(s_loc_light_col, 1, lcol);
    glUniform3fv(s_loc_fill_col,  1, g_fill_color);
    glUniform1f(s_loc_rim_str,   g_rim_strength);
    glUniform1f(s_loc_spec_str,  g_spec_strength);
    glUniform3fv(s_loc_ambient,   1, amb);
    glUniform3fv(s_loc_amb_ground, 1, ambg);
    glUniform4fv(s_loc_mat_color, 1, col);
    glUniform1f(s_loc_alpha,      col[3]);
    glUniform1i(s_loc_flat_shade, s_flat_shade ? 1 : 0);
    glUniform1i(s_loc_dbg_normals, s_debug_normals ? 1 : 0);
    glUniform1i(s_loc_inline_tm, s_inline_tonemap ? 1 : 0);

    // Directional light array (scene override). When empty the single
    // editor preview directional (g_light_dir/color) above is used.
    glUniform1i(s_loc_dir_cnt, s_dir_light_count);
    if (s_dir_light_count > 0) {
        glUniform3fv(s_loc_dir_dir, s_dir_light_count, &s_dir_light_dir[0][0]);
        float dcols[4][3];
        for (int i = 0; i < s_dir_light_count && i < 4; ++i)
            for (int a = 0; a < 3; ++a)
                dcols[i][a] = s_dir_light_col[(size_t)i][a] * key_boost;
        glUniform3fv(s_loc_dir_col, s_dir_light_count, &dcols[0][0]);
    }

    // Point lights + camera position for local warm illumination.
    glUniform1i(s_loc_light_cnt, s_point_light_count);
    if (s_point_light_count > 0) {
        glUniform3fv(s_loc_light_pos,  s_point_light_count, &s_point_light_pos[0][0]);
        float pcols[16][3];
        for (int i = 0; i < s_point_light_count && i < 16; ++i)
            for (int a = 0; a < 3; ++a)
                pcols[i][a] = s_point_light_col[(size_t)i][a] * key_boost;
        glUniform3fv(s_loc_light_cols, s_point_light_count, &pcols[0][0]);
        glUniform1fv(s_loc_light_rad,  s_point_light_count, s_point_light_radius);
    }
    glUniform3fv(s_loc_cam_pos, 1, s_cam_eye);

    // Depth fog.
    glUniform1i(s_loc_fog_en,  s_fog_enabled ? 1 : 0);
    glUniform3fv(s_loc_fog_col, 1, s_fog_color);
    glUniform1f(s_loc_fog_near, s_fog_near);
    glUniform1f(s_loc_fog_far,  s_fog_far);

    // Texture binding
    bool has_tex = (mesh.texture_id != 0);
    glUniform1i(s_loc_has_tex, has_tex ? 1 : 0);
    if (has_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mesh.texture_id);
        glUniform1i(s_loc_texture, 0);
    }

    // DCC importers (FBX/glTF) sample bottom-origin textures with v=0-top UVs.
    glUniform1i(s_loc_flip_v, mesh.flip_uv_v ? 1 : 0);

    // Draw
    glBindVertexArray(mesh.vao);

    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDisable(GL_CULL_FACE);
    }

    if (mesh.ebo) {
        glDrawElements(GL_TRIANGLES, mesh.index_count,
                       GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.index_count);
    }

    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_CULL_FACE);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

// ============================================================================
// PBR renderer — vendored algorithms from zauonlok/renderer (MIT, Zhou Le)
// Reference sources: src/render/zauonlok/ (pbr_shader.c, maths.c)
// ============================================================================

void pbr_set_joint_matrices(const float* matrices, int count) {
    count = count < 0 ? 0 : (count > 256 ? 256 : count);
    if (matrices && count > 0) {
        std::memcpy(s_pbr_joints, matrices, sizeof(float) * 16 * count);
    }
    s_pbr_joint_count = count;
}

void pbr_set_environment(unsigned int env_cubemap, float intensity) {
    s_pbr_env_cubemap = env_cubemap;
    s_pbr_env_intensity = intensity > 0.0f ? intensity : 0.0f;
}

void pbr_set_env_intensity(float intensity) {
    s_pbr_env_intensity = intensity > 0.0f ? intensity : 0.0f;
}

void pbr_set_shadow(const float light_view[16], const float light_proj[16],
                    unsigned int shadow_depth_tex) {
    // Light clip matrix = light_proj * light_view (column-major).
    mat4_multiply(s_shadow_light_vp, light_proj, light_view);
    s_shadow_depth_tex = shadow_depth_tex;
}

void pbr_enable_shadows(bool on) {
    s_shadow_enabled = on;
}

void pbr_render_mesh(const GPUMesh& mesh, const float* model_matrix,
                     const PBRMaterial& mat, bool wireframe) {
    if (!mesh.vao || !s_pbr.prog) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float model[16];
    if (model_matrix) std::memcpy(model, model_matrix, 16 * sizeof(float));
    else mat4_identity(model);
    float mvp[16], nmat[9];
    mat4_multiply(mvp, s_vp, model);
    mat4_normal_matrix(nmat, model);

    glUseProgram(s_pbr.prog);

    glUniformMatrix4fv(s_pbr.mvp,      1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_pbr.model,    1, GL_FALSE, model);
    glUniformMatrix3fv(s_pbr.nmat,     1, GL_FALSE, nmat);
    glUniformMatrix4fv(s_pbr.light_vp, 1, GL_FALSE, s_shadow_light_vp);
    glUniform1i(s_pbr.flip_v,   mesh.flip_uv_v ? 1 : 0);
    glUniform1i(s_pbr.has_tan,  mesh.has_tangents ? 1 : 0);
    // Only skin meshes that actually carry joint attributes (a stale global
    // count must never make the VS read undefined aJoint/aWeight values).
    glUniform1i(s_pbr.joint_cnt, mesh.has_skinning ? s_pbr_joint_count : 0);
    if (s_pbr_joint_count > 0)
        glUniformMatrix4fv(s_pbr.joints, s_pbr_joint_count, GL_FALSE, s_pbr_joints);
    glUniform3fv(s_pbr.cam_pos, 1, s_cam_eye);

    glUniform4fv(s_pbr.base_f, 1, mat.base_color);
    glUniform1f(s_pbr.metal_f, mat.metalness);
    glUniform1f(s_pbr.rough_f, mat.roughness);
    glUniform3fv(s_pbr.spec_f, 1, mat.specular);
    glUniform1f(s_pbr.gloss_f, mat.glossiness);
    glUniform1f(s_pbr.occ, mat.occlusion);
    glUniform3fv(s_pbr.emiss, 1, mat.emission);
    glUniform1i(s_pbr.workflow, mat.workflow);
    glUniform1f(s_pbr.alpha_cutoff, 0.0f);   // 0 = alpha test disabled (blend instead)

    // Shared light state (same arrays the legacy model path uses).
    glUniform3fv(s_pbr.light_dir, 1, g_light_dir);
    float lcol[3] = { g_light_color[0] * g_light_scale,
                      g_light_color[1] * g_light_scale,
                      g_light_color[2] * g_light_scale };
    float amb[3] = { g_ambient_color[0] * g_ambient_scale,
                     g_ambient_color[1] * g_ambient_scale,
                     g_ambient_color[2] * g_ambient_scale };
    float ambg[3] = { g_ambient_ground_color[0] * g_ambient_scale,
                      g_ambient_ground_color[1] * g_ambient_scale,
                      g_ambient_ground_color[2] * g_ambient_scale };
    glUniform3fv(s_pbr.light_col, 1, lcol);
    glUniform3fv(s_pbr.ambient,   1, amb);
    glUniform3fv(s_pbr.amb_ground, 1, ambg);
    glUniform1i(s_pbr.dir_cnt, s_dir_light_count);
    if (s_dir_light_count > 0) {
        glUniform3fv(s_pbr.dir_dir, s_dir_light_count, &s_dir_light_dir[0][0]);
        glUniform3fv(s_pbr.dir_col, s_dir_light_count, &s_dir_light_col[0][0]);
    }
    glUniform1i(s_pbr.light_cnt, s_point_light_count);
    if (s_point_light_count > 0) {
        glUniform3fv(s_pbr.light_pos,  s_point_light_count, &s_point_light_pos[0][0]);
        glUniform3fv(s_pbr.light_cols, s_point_light_count, &s_point_light_col[0][0]);
        glUniform1fv(s_pbr.light_rad,  s_point_light_count, s_point_light_radius);
    }

    // IBL environment + BRDF LUT + shadow map.
    glUniform1i(s_pbr.env_has, (s_pbr_env_cubemap != 0) ? 1 : 0);
    glUniform1f(s_pbr.env_int, s_pbr_env_intensity);
    glUniform1i(s_pbr.env_mip, 6);   // default env is 64px (LOD 0..6)
    glUniform1i(s_pbr.shadow_en, (s_shadow_enabled && s_shadow_depth_tex) ? 1 : 0);

    // Material textures (units 0..7), env (8), BRDF LUT (9), shadow (10).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mat.basecolor_tex ? mat.basecolor_tex : mesh.texture_id);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, mat.metalness_tex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, mat.roughness_tex);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, mat.normal_tex);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, mat.occlusion_tex);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, mat.emission_tex);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, mat.specular_tex);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, mat.glossiness_tex);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_CUBE_MAP, s_pbr_env_cubemap);
    glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, s_pbr_brdf_lut);
    glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D, s_shadow_depth_tex);
    glActiveTexture(GL_TEXTURE0);

    glUniform1i(s_pbr.has_base,  (mat.basecolor_tex || mesh.texture_id) ? 1 : 0);
    glUniform1i(s_pbr.has_metal, mat.metalness_tex ? 1 : 0);
    glUniform1i(s_pbr.has_rough, mat.roughness_tex ? 1 : 0);
    glUniform1i(s_pbr.has_nmap,  mat.normal_tex ? 1 : 0);
    glUniform1i(s_pbr.has_occ,   mat.occlusion_tex ? 1 : 0);
    glUniform1i(s_pbr.has_emiss, mat.emission_tex ? 1 : 0);
    glUniform1i(s_pbr.has_spec,  mat.specular_tex ? 1 : 0);
    glUniform1i(s_pbr.has_gloss, mat.glossiness_tex ? 1 : 0);

    glBindVertexArray(mesh.vao);
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDisable(GL_CULL_FACE);
    }
    if (mesh.ebo) {
        glDrawElements(GL_TRIANGLES, mesh.index_count, GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.index_count);
    }
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_CULL_FACE);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

// ── Directional shadow-map pass ──

unsigned int create_shadow_fbo(int width, int height, unsigned int* out_depth_tex) {
    GLuint fbo = 0, depth = 0;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteTextures(1, &depth);
        glDeleteFramebuffers(1, &fbo);
        fprintf(stderr, "[av_renderer] shadow FBO incomplete (0x%x).\n", status);
        return 0;
    }
    if (out_depth_tex) *out_depth_tex = depth;
    return fbo;
}

void begin_shadow_pass(unsigned int fbo, int w, int h) {
    if (!s_shadow_prog) return;
    glGetIntegerv(GL_VIEWPORT, s_shadow_saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&s_shadow_saved_fbo);
    s_shadow_saved_blend   = (glIsEnabled(GL_BLEND) != GL_FALSE);
    s_shadow_saved_scissor = (glIsEnabled(GL_SCISSOR_TEST) != GL_FALSE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_shadow_prog);
    glUniformMatrix4fv(s_shadow_loc_vp, 1, GL_FALSE, s_shadow_light_vp);
}

void end_shadow_pass() {
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_shadow_saved_fbo);
    glViewport(s_shadow_saved_viewport[0], s_shadow_saved_viewport[1],
               s_shadow_saved_viewport[2], s_shadow_saved_viewport[3]);
    if (s_shadow_saved_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s_shadow_saved_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

void shadow_render_mesh(const GPUMesh& mesh, const float* model_matrix) {
    if (!mesh.vao || !s_shadow_prog) return;
    float model[16];
    if (model_matrix) std::memcpy(model, model_matrix, 16 * sizeof(float));
    else mat4_identity(model);
    glUniformMatrix4fv(s_shadow_loc_model, 1, GL_FALSE, model);
    glBindVertexArray(mesh.vao);
    if (mesh.ebo) {
        glDrawElements(GL_TRIANGLES, mesh.index_count, GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.index_count);
    }
    glBindVertexArray(0);
}

void render_lines(const float* positions, int vertex_count, const float color[4],
                  const float* model_matrix, float width) {
    if (!positions || vertex_count < 2 || !s_model_prog) return;
    float model[16];
    if (model_matrix) std::memcpy(model, model_matrix, sizeof(model));
    else mat4_identity(model);
    float mvp[16], normal[9];
    mat4_multiply(mvp, s_vp, model);
    mat4_normal_matrix(normal, model);

    std::vector<float> vertices(static_cast<size_t>(vertex_count) * 8, 0.0f);
    for (int i = 0; i < vertex_count; ++i) {
        std::memcpy(&vertices[static_cast<size_t>(i) * 8], positions + i * 3, 3 * sizeof(float));
        vertices[static_cast<size_t>(i) * 8 + 4] = 1.0f;
    }
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(6*sizeof(float)));
    glUseProgram(s_model_prog);
    glUniformMatrix4fv(s_loc_mvp, 1, GL_FALSE, mvp); glUniformMatrix4fv(s_loc_model, 1, GL_FALSE, model);
    glUniformMatrix3fv(s_loc_normalmat, 1, GL_FALSE, normal);
    glUniform1i(s_loc_has_tex, 0); glUniform4fv(s_loc_mat_color, 1, color); glUniform1f(s_loc_alpha, color[3]);
    glUniform3f(s_loc_light_dir, 0, 0, 0); glUniform3f(s_loc_light_col, 0, 0, 0);
    glUniform3f(s_loc_ambient, 1, 1, 1); glUniform3f(s_loc_amb_ground, 1, 1, 1);
    glLineWidth(width); glDrawArrays(GL_LINES, 0, vertex_count); glLineWidth(1.0f);
    glBindVertexArray(0); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao); glUseProgram(0);
}

void render_grid(float size, float y_level) {
    if (!s_grid_vao || !s_grid_prog) return;

    // Model matrix: scale to `size` and translate to y_level
    float scale_m[16], trans_m[16], model[16];
    mat4_identity(scale_m);
    scale_m[0] = size;       // scale X
    scale_m[5] = 1.0f;       // Y unchanged
    scale_m[10] = size;      // scale Z
    mat4_translate(trans_m, 0.0f, y_level, 0.0f);
    mat4_multiply(model, trans_m, scale_m);

    // MVP
    float mvp[16];
    mat4_multiply(mvp, s_vp, model);

    glUseProgram(s_grid_prog);
    glUniformMatrix4fv(s_loc_grid_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_loc_grid_model, 1, GL_FALSE, model);
    glUniform1f(s_loc_grid_size, size);
    glUniform4f(s_loc_grid_color, 0.4f, 0.4f, 0.5f, 0.5f);
    // World-units-per-pixel on the plane: makes line spacing adaptive to zoom.
    const float plane_dist = fabsf(s_cam_eye[1] - y_level);
    const float wpp = (2.0f * plane_dist) / (s_grid_proj_f * s_grid_viewport_h);
    if (s_loc_grid_scale >= 0) glUniform1f(s_loc_grid_scale, wpp);

    // Grid is translucent — disable depth write, keep depth test
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(s_grid_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

void render_background_quad(unsigned int texture_id, const float* model_matrix) {
    if (!s_bg_prog || !s_bg_vao || !texture_id) return;

    float model[16];
    if (model_matrix) {
        memcpy(model, model_matrix, 16 * sizeof(float));
    } else {
        mat4_identity(model);
    }

    float mvp[16];
    mat4_multiply(mvp, s_vp, model);

    glUseProgram(s_bg_prog);
    glUniformMatrix4fv(s_bg_loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(s_bg_loc_tex, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Drawn behind everything: no depth writes, always passes the depth test
    // at the far plane where the level geometry cannot overlap it.
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_bg_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

void render_glow_sprite(const float pos[3], const float color[3], float size) {
    if (!s_glow_prog || !s_glow_vao || size <= 0.0f) return;

    // Camera-facing billboard: build a model matrix from the view basis.
    // View matrix rows give the camera right/up vectors.
    float M[16];
    const float* V = s_view;
    float right[3] = { V[0], V[4], V[8] };   // first column = camera right
    float up[3]    = { V[1], V[5], V[9] };   // second column = camera up

    const float h = size * 0.5f;
    M[0] = right[0]*h; M[4] = up[0]*h; M[8]  = 0.0f; M[12] = pos[0];
    M[1] = right[1]*h; M[5] = up[1]*h; M[9]  = 0.0f; M[13] = pos[1];
    M[2] = right[2]*h; M[6] = up[2]*h; M[10] = 0.0f; M[14] = pos[2];
    M[3] = 0.0f; M[7] = 0.0f; M[11] = 0.0f; M[15] = 1.0f;

    float mvp[16];
    mat4_multiply(mvp, s_vp, M);

    glUseProgram(s_glow_prog);
    glUniformMatrix4fv(s_glow_loc_mvp, 1, GL_FALSE, mvp);
    glUniform3fv(s_glow_loc_color, 1, color);

    // Additive glow that reads the bloom bright-pass.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_glow_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
}

void render_portal_effect(const float pos[3], const float color[3], float size,
                          float speed, float time_sec) {
    if (!s_portal_prog || !s_glow_vao || size <= 0.0f) return;

    // Camera-facing billboard (same construction as render_glow_sprite) so the
    // swirl always faces the editor camera and stays legible in ortho + persp.
    float M[16];
    const float* V = s_view;
    const float right[3] = { V[0], V[4], V[8] };
    const float up[3]    = { V[1], V[5], V[9] };
    const float h = size * 0.5f;
    M[0] = right[0]*h; M[4] = up[0]*h; M[8]  = 0.0f; M[12] = pos[0];
    M[1] = right[1]*h; M[5] = up[1]*h; M[9]  = 0.0f; M[13] = pos[1];
    M[2] = right[2]*h; M[6] = up[2]*h; M[10] = 0.0f; M[14] = pos[2];
    M[3] = 0.0f; M[7] = 0.0f; M[11] = 0.0f; M[15] = 1.0f;

    float mvp[16];
    mat4_multiply(mvp, s_vp, M);

    glUseProgram(s_portal_prog);
    glUniformMatrix4fv(s_portal_loc_mvp, 1, GL_FALSE, mvp);
    glUniform3fv(s_portal_loc_color, 1, color);
    glUniform1f(s_portal_loc_time, time_sec);
    glUniform1f(s_portal_loc_speed, speed > 0.0f ? speed : 1.0f);

    // Additive so it glows and feeds the bloom bright-pass; depth test on
    // (so geometry in front occludes it) but no depth write.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_glow_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
}

void render_water_sheet(const WaterSheetData& ws, const float* model_matrix,
                        float time_sec) {
    if (!s_water_prog || !s_water_vao) return;
    const float x = ws.rect[0], y = ws.rect[1], w = ws.rect[2], h = ws.rect[3];
    if (w <= 0.0f || h <= 0.0f) return;

    // Grid: ~20 world units per column like the reference editor, clamped so
    // huge pools stay cheap.
    int cols = std::max(1, (int)std::floor(w / 20.0f));
    if (cols > 48) cols = 48;
    const float f = w / (float)cols;
    const int m = cols + 1;
    const int num_verts = m * 4;

    // Sheet thickness (front/back faces) — matches the reference PS() water
    // effect which spans local z from +40 (front) to -20 (back).
    const float front_z = 40.0f, back_z = -20.0f;

    std::vector<float> verts(static_cast<size_t>(num_verts) * 5, 0.0f);
    std::vector<uint16_t> idx(static_cast<size_t>(cols) * 12, 0);

    const float tile = ws.tile_size > 0.0f ? ws.tile_size : 64.0f;
    const float ox = ws.tex_offset[0], oy = ws.tex_offset[1];
    const float bottom = y + h;

    for (int O = 0; O < m; ++O) {
        const float N = x + O * f;  // column x position
        const float phase = N / 160.0f + time_sec * 0.2f;
        const float phase2 = N / 320.0f + time_sec * 0.6f;
        const float wave = std::sin(phase * 6.2831853f) * 6.0f +
                           std::sin(phase2 * 6.2831853f) * 6.0f;
        const float bottom_y = bottom + wave;   // undulating surface edge
        const float scroll = std::sin((O / 20.0f + time_sec * 0.5f) * 6.2831853f) * 0.2f;

        const size_t base = static_cast<size_t>(O) * 4 * 5;
        // v0 front-top, v1 front-bottom, v2 back-top, v3 back-bottom
        const float vx[4] = { N, N, N, N };
        const float vy[4] = { y, bottom_y, y + wave * 0.4f, bottom_y };
        const float vz[4] = { front_z, front_z, back_z, back_z };
        for (int D = 0; D < 4; ++D) {
            float* out = &verts[base + static_cast<size_t>(D) * 5];
            out[0] = vx[D]; out[1] = vy[D]; out[2] = vz[D];
            out[3] = (vx[D] - ox) / tile + 0.5f + scroll;
            out[4] = (vy[D] - oy) / tile + 0.5f;
        }
        if (O < cols) {
            const uint16_t n = (uint16_t)(O * 4);
            // front face: (1,0,4) (4,5,1)
            idx[O*12 + 0] = n+1; idx[O*12 + 1] = n;   idx[O*12 + 2] = n+4;
            idx[O*12 + 3] = n+4; idx[O*12 + 4] = n+5; idx[O*12 + 5] = n+1;
            // back face: (3,2,6) (6,7,3)
            idx[O*12 + 6] = n+3; idx[O*12 + 7] = n+2; idx[O*12 + 8] = n+6;
            idx[O*12 + 9] = n+6; idx[O*12 + 10] = n+7; idx[O*12 + 11] = n+3;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_water_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_water_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint16_t)),
                 idx.data(), GL_DYNAMIC_DRAW);

    float mvp[16], ident[16];
    if (model_matrix) mat4_multiply(mvp, s_vp, model_matrix);
    else { mat4_identity(ident); mat4_multiply(mvp, s_vp, ident); }

    glUseProgram(s_water_prog);
    glUniformMatrix4fv(s_water_loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(s_water_loc_tex, 0);
    glUniform1f(s_water_loc_scroll, time_sec * 0.05f);

    const bool has_tex = (ws.texture_id != 0);
    glUniform1i(s_water_loc_has_tex, has_tex ? 1 : 0);
    if (has_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ws.texture_id);
    }

    // Transparent sheet: no depth write, depth test still on (opaque geometry
    // drawn before us). Slight polygon offset keeps it off the ground plane.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glBindVertexArray(s_water_vao);
    // Front face with FrontColor
    glUniform4fv(s_water_loc_tint, 1, ws.front_color);
    glDrawElements(GL_TRIANGLES, cols * 6, GL_UNSIGNED_SHORT, nullptr);
    // Back face with SurfaceColor
    glUniform4fv(s_water_loc_tint, 1, ws.surface_color);
    glDrawElements(GL_TRIANGLES, cols * 6, GL_UNSIGNED_SHORT,
                   (void*)(size_t)(cols * 6 * sizeof(uint16_t)));

    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void render_fire_sprite(const float pos[3], const float color[3], float size,
                        float flicker, float time_sec) {
    if (!s_fire_prog || !s_glow_vao || size <= 0.0f) return;

    // Camera-facing billboard, but the flame stands UPRIGHT (world +Y up)
    // rather than tilting on both axes — a real flame grows vertically. We
    // keep the horizontal axis camera-facing (screen right, flattened onto the
    // ground plane) and force the vertical axis to world up, so torches read
    // correctly from any camera yaw.
    const float* V = s_view;
    float right[3] = { V[0], V[4], V[8] };      // camera right
    right[1] = 0.0f;                            // flatten to ground plane
    float rl = std::sqrt(right[0]*right[0] + right[2]*right[2]);
    if (rl < 1e-4f) { right[0] = 1.0f; right[2] = 0.0f; rl = 1.0f; }
    right[0] /= rl; right[2] /= rl;
    const float up[3] = { 0.0f, 1.0f, 0.0f };   // world up

    const float hw = size * 0.5f;               // half width
    const float ht = size * 1.6f;               // taller than wide (flame)
    float M[16];
    M[0] = right[0]*hw; M[4] = up[0]*ht; M[8]  = 0.0f; M[12] = pos[0];
    M[1] = right[1]*hw; M[5] = up[1]*ht; M[9]  = 0.0f; M[13] = pos[1];
    M[2] = right[2]*hw; M[6] = up[2]*ht; M[10] = 0.0f; M[14] = pos[2];
    M[3] = 0.0f; M[7] = 0.0f; M[11] = 0.0f; M[15] = 1.0f;

    float mvp[16];
    mat4_multiply(mvp, s_vp, M);

    glUseProgram(s_fire_prog);
    glUniformMatrix4fv(s_fire_loc_mvp, 1, GL_FALSE, mvp);
    glUniform3fv(s_fire_loc_color, 1, color);
    if (s_fire_loc_time    >= 0) glUniform1f(s_fire_loc_time, time_sec);
    if (s_fire_loc_flicker >= 0) glUniform1f(s_fire_loc_flicker, flicker);

    // Additive, no depth write — feeds the bloom bright-pass and blends into
    // the scene rather than occluding it.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_glow_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
}

void render_point_light_glows() {
    for (int i = 0; i < s_point_light_count; ++i) {
        float bright[3] = {
            s_point_light_col[i][0] * 1.5f + 0.10f,
            s_point_light_col[i][1] * 1.5f + 0.10f,
            s_point_light_col[i][2] * 1.5f + 0.10f,
        };
        // Compact emitter marker: the sprite size tracks the camera distance
        // (roughly constant on screen) so the light reads as a small billboard
        // AT the source — NOT as a scaled copy of the (large) influence
        // radius. The bright core still feeds the PostFX bloom bright-pass.
        const float dx = s_point_light_pos[i][0] - s_cam_eye[0];
        const float dy = s_point_light_pos[i][1] - s_cam_eye[1];
        const float dz = s_point_light_pos[i][2] - s_cam_eye[2];
        const float cam_dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        const float size = std::min(std::max(cam_dist * 0.045f, 6.0f), 30.0f);
        render_glow_sprite(s_point_light_pos[i], bright, size);
    }
}

void render_light_debug() {
    if (s_point_light_count <= 0) return;
    // Editor-oriented debug overlay: a small emitter axis marker plus the
    // influence-radius ring at every uploaded point light. Separate from the
    // actual illumination — the ring never "is" the light.
    const int kSegs = 48;
    std::vector<float> segs;
    for (int i = 0; i < s_point_light_count; ++i) {
        const float r  = std::max(s_point_light_radius[i], 1.0f);
        const float cx = s_point_light_pos[i][0];
        const float cy = s_point_light_pos[i][1];
        const float cz = s_point_light_pos[i][2];
        segs.clear();
        for (int k = 0; k < kSegs; ++k) {
            const float a0 = (float)k / kSegs * 2.0f * PI;
            const float a1 = (float)(k + 1) / kSegs * 2.0f * PI;
            segs.push_back(cx + std::cos(a0) * r); segs.push_back(cy); segs.push_back(cz + std::sin(a0) * r);
            segs.push_back(cx + std::cos(a1) * r); segs.push_back(cy); segs.push_back(cz + std::sin(a1) * r);
        }
        // Vertical emitter axis: a small cross through the source position.
        segs.push_back(cx); segs.push_back(cy - r * 0.30f); segs.push_back(cz);
        segs.push_back(cx); segs.push_back(cy + r * 0.30f); segs.push_back(cz);
        segs.push_back(cx - r * 0.30f); segs.push_back(cy); segs.push_back(cz);
        segs.push_back(cx + r * 0.30f); segs.push_back(cy); segs.push_back(cz);

        float col[4] = {
            s_point_light_col[i][0] * 0.7f + 0.20f,
            s_point_light_col[i][1] * 0.7f + 0.20f,
            s_point_light_col[i][2] * 0.7f + 0.20f,
            0.45f,
        };
        render_lines(segs.data(), (int)(segs.size() / 3), col, nullptr, 1.0f);
    }
}

void render_shadow_blob(const float pos[3], float width_radius, float depth_radius,
                        float rot_y, const float color[3]) {
    if (!s_glow_prog || !s_glow_vao) return;

    // Ground-aligned ellipse: scale the unit quad by the two radii, then
    // flatten it onto the ground plane (Y up), rotated by the object Y-rot.
    const float wx = width_radius;
    const float wz = depth_radius;
    const float c = std::cos(rot_y), s = std::sin(rot_y);
    float M[16] = {0};
    M[0]  = wx * c;  M[4] = -wz * s;  M[8]  = 0.0f;  M[12] = pos[0];
    M[1]  = 0.0f;    M[5] =  0.0f;    M[9]  = 0.0f;  M[13] = pos[1];
    M[2]  = wx * s;  M[6] =  wz * c;  M[10] = 0.0f;  M[14] = pos[2];
    M[3]  = 0.0f;    M[7] =  0.0f;    M[11] = 0.0f;  M[15] = 1.0f;

    float mvp[16];
    mat4_multiply(mvp, s_vp, M);

    glUseProgram(s_glow_prog);
    glUniformMatrix4fv(s_glow_loc_mvp, 1, GL_FALSE, mvp);
    glUniform3fv(s_glow_loc_color, 1, color);

    glUseProgram(s_glow_prog);
    glUniformMatrix4fv(s_glow_loc_mvp, 1, GL_FALSE, mvp);
    // Color is black; the alpha (falloff) darkens dest via GL_DST_COLOR-free
    // multiply: out = dst * (1 - src_alpha_falloff).
    glUniform3f(s_glow_loc_color, color[0], color[1], color[2]);

    // Soft contact shadow: multiply the ground color by the shadow coverage.
    // out.rgb = src.rgb (0) + dst.rgb * (1 - src.a). Shader alpha = falloff.
    // NOTE: this overrides the global blend function — it MUST be restored,
    // otherwise every later draw (e.g. the Hiro model, whose opaque fragments
    // have alpha 1.0) computes dst*(1-1) = 0 and renders BLACK. That was the
    // "black hero" bug: the shadow blob corrupted the blend right before the
    // character drew.
    GLint prev_src = 0, prev_dst = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_glow_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc((GLenum)prev_src, (GLenum)prev_dst);   // restore caller's blend
    glUseProgram(0);
}

void render_grid_xy(float size, float z_level) {
    if (!s_grid_vao || !s_grid_prog) return;
    float scale_m[16], rotate_m[16], trans_m[16], temp[16], model[16];
    mat4_identity(scale_m);
    scale_m[0] = size;
    scale_m[10] = size;
    mat4_rotate_x(rotate_m, 90.0f);
    mat4_translate(trans_m, 0.0f, 0.0f, z_level);
    mat4_multiply(temp, rotate_m, scale_m);
    mat4_multiply(model, trans_m, temp);

    float mvp[16];
    mat4_multiply(mvp, s_vp, model);
    glUseProgram(s_grid_prog);
    glUniformMatrix4fv(s_loc_grid_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_loc_grid_model, 1, GL_FALSE, model);
    glUniform1f(s_loc_grid_size, size);
    glUniform4f(s_loc_grid_color, 0.35f, 0.42f, 0.55f, 0.5f);
    const float plane_dist = fabsf(s_cam_eye[2] - z_level);
    const float wpp = (2.0f * plane_dist) / (s_grid_proj_f * s_grid_viewport_h);
    if (s_loc_grid_scale >= 0) glUniform1f(s_loc_grid_scale, wpp);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s_grid_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

void end_3d() {
    // Restore previous FBO and viewport
    glBindFramebuffer(GL_FRAMEBUFFER, s_saved_fbo);
    glViewport(s_saved_viewport[0], s_saved_viewport[1],
               s_saved_viewport[2], s_saved_viewport[3]);

    // Reset state to sane defaults for ImGui
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(0);
}

// ============================================================================
// Post-processing pipeline
// ============================================================================
//
//   src ──► [bright-pass] ──► bloomA(half) ──► [blur H] ──► bloomB
//      │                                             └──► [blur V] ──► bloomA
//      └────────────────────────────────────────────────────────┐
//                                                                ▼
//   out  ◄── [composite: DOF + bloom + HD tone map + grade +
//              sharpen + vignette + grain]
//
// Internal FBOs are lazily created/resized; postfx_apply owns no caller state.

// --- Fullscreen-quad shader sources ----------------------------------------

static const char* FX_VS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

static const char* FX_BRIGHT_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;      // 1 / source size
uniform float uThreshold;
void main() {
    vec2 o = uTexel * 0.5;
    vec3 a = texture(uTex, vUV + vec2(-o.x, -o.y)).rgb;
    vec3 b = texture(uTex, vUV + vec2( o.x, -o.y)).rgb;
    vec3 c = texture(uTex, vUV + vec2(-o.x,  o.y)).rgb;
    vec3 d = texture(uTex, vUV + vec2( o.x,  o.y)).rgb;
    vec3 col = (a + b + c + d) * 0.25;
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float bright = smoothstep(uThreshold, uThreshold + 0.6, luma);
    FragColor = vec4(col * bright, 1.0);
}
)GLSL";

static const char* FX_BLUR_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uDir;         // texel-sized blur direction
void main() {
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 acc = texture(uTex, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = uDir * float(i);
        acc += texture(uTex, vUV + off).rgb * w[i];
        acc += texture(uTex, vUV - off).rgb * w[i];
    }
    FragColor = vec4(acc, 1.0);
}
)GLSL";

// Half-res depth-based SSAO: golden-angle spiral taps compare view-space
// distances around the fragment; closer neighbors occlude. Range-checked so
// silhouettes don't self-shadow. Blurred afterwards (FX_BLUR) and mixed into
// the ambient in the composite — contact shadows the direct lights can't give.
static const char* FX_SSAO_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uDepth;
uniform vec2  uTexel;          // 1 / half-res size
uniform float uNear;
uniform float uFar;
uniform float uTanHalfFov;     // tan(fov/2) of the scene camera
uniform float uAspect;         // width / height
uniform float uRadius;         // view-space radius in world units

float linearDepth(vec2 uv) {
    float d = texture(uDepth, uv).r;
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

void main() {
    float d0 = linearDepth(vUV);
    // World radius → screen UV footprint at this depth:
    //   ndc = (worldOffset / depth) / tanHalf ; uv = ndc / 2
    vec2 radiusUV = vec2((uRadius / d0) / (uTanHalfFov * uAspect),
                         (uRadius / d0) /  uTanHalfFov) * 0.5;
    radiusUV = clamp(radiusUV, vec2(0.002), vec2(0.06));

    float occ = 0.0;
    const int TAPS = 12;
    for (int i = 0; i < TAPS; ++i) {
        float ang = (float(i) + 0.5) * 2.39996323;          // golden angle
        float rr  = sqrt((float(i) + 0.5) / float(TAPS));   // spiral radius
        vec2 off = vec2(cos(ang), sin(ang)) * rr * radiusUV * 2.0;
        float ds = linearDepth(clamp(vUV + off, vec2(0.001), vec2(0.999)));
        float diff = d0 - ds;                               // >0: neighbor nearer
        if (diff > 0.02 * d0) {
            float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(diff), 1e-3));
            occ += clamp(diff / (0.35 * uRadius), 0.0, 1.0) * rangeCheck;
        }
    }
    float ao = 1.0 - (occ / float(TAPS)) * 1.6;
    ao = clamp(pow(max(ao, 0.0), 1.3), 0.0, 1.0);
    FragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";

static const char* FX_COMP_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uColor;
uniform sampler2D uBloom;
uniform sampler2D uBloom2;      // quarter-res wide band
uniform sampler2D uDepth;
uniform sampler2D uSsao;
uniform vec2  uTexel;         // 1 / full size
uniform float uExposure;
uniform int   uHdOn;
uniform int   uBloomOn;
uniform float uBloomStrength;
uniform int   uSsaoOn;
uniform float uSsaoStrength;
uniform int   uDofOn;
uniform float uDofFocus;
uniform float uDofScale;
uniform float uNear;
uniform float uFar;
uniform int   uGradeOn;
uniform float uSaturation;
uniform float uContrast;
uniform float uBrightness;
uniform float uWarmth;
uniform int   uSharpenOn;
uniform float uSharpenAmount;
uniform int   uVignetteOn;
uniform float uVignetteStrength;
uniform int   uGrainOn;
uniform float uGrainAmount;
uniform float uTime;

float linearDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 col = texture(uColor, vUV).rgb;

    // ── SSAO: darken ambient-occluded crevices / contacts in LINEAR light,
    //    before tone mapping (so the darkening behaves like lighting). ──
    if (uSsaoOn == 1)
        col *= mix(1.0, texture(uSsao, vUV).r, uSsaoStrength);

    // ── Depth of field: CoC-driven disc blur ──
    if (uDofOn == 1) {
        float depth = linearDepth(texture(uDepth, vUV).r);
        float coc = clamp(abs(depth - uDofFocus) / max(uDofFocus, 0.001), 0.0, 1.0) * uDofScale;
        if (coc > 0.002) {
            // Ring of 12 taps + the center tap, radius clamped so even a large
            // CoC can't blow out into a 400px smear on small previews.
            float radius = min(coc * 60.0, 24.0);
            vec3 acc = col;          // center tap
            float wsum = 1.0;
            for (int i = 0; i < 12; ++i) {
                float ang = 6.2831853 * float(i) / 12.0;
                vec2 off = vec2(cos(ang), sin(ang)) * radius;
                vec2 tc = clamp(vUV + off * uTexel, vec2(0.001), vec2(0.999));
                float w = 1.0 - float(i) / 13.0;
                acc += texture(uColor, tc).rgb * w;
                wsum += w;
            }
            col = acc / wsum;
        }
    }

    // ── Bloom (additive, two bands): the half-res band keeps tight cores,
    //    the quarter-res band spreads a soft wide halo. ──
    if (uBloomOn == 1)
        col += (texture(uBloom,  vUV).rgb * 0.62 +
                texture(uBloom2, vUV).rgb * 0.48) * uBloomStrength;

    // ── HD render: exposure + ACES tone map + gamma ──
    if (uHdOn == 1) {
        col *= uExposure;
        col = aces(col);
        col = pow(col, vec3(1.0 / 2.2));
    }

    // ── Cinematic color grade ──
    if (uGradeOn == 1) {
        float luma = dot(col, vec3(0.299, 0.587, 0.114));
        col = mix(vec3(luma), col, uSaturation);
        col = (col - 0.5) * uContrast + 0.5 + uBrightness;
        col = max(col, vec3(0.0));
        col.r += uWarmth * 0.2;
        col.b -= uWarmth * 0.2;
    }

    // ── Sharpen (unsharp mask) ──
    if (uSharpenOn == 1) {
        vec3 lap = texture(uColor, vUV + vec2( uTexel.x, 0.0)).rgb
                 + texture(uColor, vUV - vec2( uTexel.x, 0.0)).rgb
                 + texture(uColor, vUV + vec2(0.0,  uTexel.y)).rgb
                 + texture(uColor, vUV - vec2(0.0, -uTexel.y)).rgb
                 - 4.0 * col;
        col += lap * uSharpenAmount * 0.35;
    }

    // ── Vignette ──
    if (uVignetteOn == 1) {
        vec2 d = vUV - 0.5;
        float dist2 = dot(d, d);
        col *= 1.0 - uVignetteStrength * smoothstep(0.25, 0.85, dist2 * 2.4);
    }

    // ── Film grain ──
    if (uGrainOn == 1) {
        float g = (hash12(vUV * 1280.0 + vec2(fract(uTime) * 97.0)) - 0.5) * uGrainAmount;
        col += g;
    }

    FragColor = vec4(col, 1.0);
}
)GLSL";

// --- PostFX internal state ---------------------------------------------------

static GLuint s_fx_bright  = 0;
static GLuint s_fx_blur    = 0;
static GLuint s_fx_comp    = 0;
static GLuint s_fx_ssao    = 0;
static GLuint s_fx_vao     = 0;
static GLuint s_fx_vbo     = 0;
static GLuint s_fx_ebo     = 0;
static GLuint s_fx_bloom_a_fbo = 0, s_fx_bloom_a_tex = 0;   // half-res band
static GLuint s_fx_bloom_b_fbo = 0, s_fx_bloom_b_tex = 0;
static GLuint s_fx_bloom_c_fbo = 0, s_fx_bloom_c_tex = 0;   // quarter-res band
static GLuint s_fx_bloom_d_fbo = 0, s_fx_bloom_d_tex = 0;
static GLuint s_fx_ssao_a_fbo = 0, s_fx_ssao_a_tex = 0;     // half-res AO
static GLuint s_fx_ssao_b_fbo = 0, s_fx_ssao_b_tex = 0;
static GLuint s_fx_out_fbo = 0, s_fx_out_tex = 0;
static int    s_fx_w = 0, s_fx_h = 0;

static GLuint fx_make_prog(const char* fs) {
    return build_program(FX_VS, fs);
}

// Create an RGBA8 color-only FBO (no depth needed for ping-pong passes).
static bool fx_make_fbo(int w, int h, GLuint* fbo, GLuint* tex) {
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    const bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!ok) {
        fprintf(stderr, "[av_renderer] PostFX FBO incomplete\n");
        glDeleteFramebuffers(1, fbo); glDeleteTextures(1, tex);
        *fbo = *tex = 0;
        return false;
    }
    return true;
}

static void fx_delete_fbo(GLuint* fbo, GLuint* tex) {
    if (*fbo) { glDeleteFramebuffers(1, fbo); *fbo = 0; }
    if (*tex) { glDeleteTextures(1, tex);     *tex = 0; }
}

static bool fx_ensure(int w, int h) {
    if (w < 1 || h < 1) return false;

    if (!s_fx_comp) {
        s_fx_bright = fx_make_prog(FX_BRIGHT_FS);
        s_fx_blur   = fx_make_prog(FX_BLUR_FS);
        s_fx_comp   = fx_make_prog(FX_COMP_FS);
        s_fx_ssao   = fx_make_prog(FX_SSAO_FS);
        if (!s_fx_comp || !s_fx_bright || !s_fx_blur || !s_fx_ssao) {
            fprintf(stderr, "[av_renderer] PostFX shader build failed\n");
            return false;
        }

        // Fullscreen quad (two triangles)
        static const float verts[] = {
            -1, -1, 0, 0,    1, -1, 1, 0,
             1,  1, 1, 1,   -1,  1, 0, 1,
        };
        static const uint16_t idx[] = { 0, 1, 2,  0, 2, 3 };
        glGenVertexArrays(1, &s_fx_vao);
        glBindVertexArray(s_fx_vao);
        glGenBuffers(1, &s_fx_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_fx_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glGenBuffers(1, &s_fx_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_fx_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
    }

    // (Re)size the internal FBOs if needed
    if (s_fx_out_fbo && (s_fx_w != w || s_fx_h != h)) {
        fx_delete_fbo(&s_fx_out_fbo, &s_fx_out_tex);
        fx_delete_fbo(&s_fx_bloom_a_fbo, &s_fx_bloom_a_tex);
        fx_delete_fbo(&s_fx_bloom_b_fbo, &s_fx_bloom_b_tex);
        fx_delete_fbo(&s_fx_bloom_c_fbo, &s_fx_bloom_c_tex);
        fx_delete_fbo(&s_fx_bloom_d_fbo, &s_fx_bloom_d_tex);
        fx_delete_fbo(&s_fx_ssao_a_fbo, &s_fx_ssao_a_tex);
        fx_delete_fbo(&s_fx_ssao_b_fbo, &s_fx_ssao_b_tex);
    }
    if (!s_fx_out_fbo) {
        const int bw = std::max(1, w / 2),  bh = std::max(1, h / 2);
        const int qw = std::max(1, w / 4),  qh = std::max(1, h / 4);
        if (!fx_make_fbo(w, h, &s_fx_out_fbo, &s_fx_out_tex)) return false;
        if (!fx_make_fbo(bw, bh, &s_fx_bloom_a_fbo, &s_fx_bloom_a_tex)) return false;
        if (!fx_make_fbo(bw, bh, &s_fx_bloom_b_fbo, &s_fx_bloom_b_tex)) return false;
        if (!fx_make_fbo(qw, qh, &s_fx_bloom_c_fbo, &s_fx_bloom_c_tex)) return false;
        if (!fx_make_fbo(qw, qh, &s_fx_bloom_d_fbo, &s_fx_bloom_d_tex)) return false;
        if (!fx_make_fbo(bw, bh, &s_fx_ssao_a_fbo, &s_fx_ssao_a_tex)) return false;
        if (!fx_make_fbo(bw, bh, &s_fx_ssao_b_fbo, &s_fx_ssao_b_tex)) return false;
        s_fx_w = w; s_fx_h = h;
    }
    return true;
}

static void fx_quad() {
    glBindVertexArray(s_fx_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
}

void postfx_shutdown() {
    if (s_fx_comp)   { glDeleteProgram(s_fx_comp);   s_fx_comp = 0; }
    if (s_fx_bright) { glDeleteProgram(s_fx_bright); s_fx_bright = 0; }
    if (s_fx_blur)   { glDeleteProgram(s_fx_blur);   s_fx_blur = 0; }
    if (s_fx_ssao)   { glDeleteProgram(s_fx_ssao);   s_fx_ssao = 0; }
    if (s_fx_vao)    { glDeleteVertexArrays(1, &s_fx_vao); s_fx_vao = 0; }
    if (s_fx_vbo)    { glDeleteBuffers(1, &s_fx_vbo); s_fx_vbo = 0; }
    if (s_fx_ebo)    { glDeleteBuffers(1, &s_fx_ebo); s_fx_ebo = 0; }
    fx_delete_fbo(&s_fx_out_fbo, &s_fx_out_tex);
    fx_delete_fbo(&s_fx_bloom_a_fbo, &s_fx_bloom_a_tex);
    fx_delete_fbo(&s_fx_bloom_b_fbo, &s_fx_bloom_b_tex);
    fx_delete_fbo(&s_fx_bloom_c_fbo, &s_fx_bloom_c_tex);
    fx_delete_fbo(&s_fx_bloom_d_fbo, &s_fx_bloom_d_tex);
    fx_delete_fbo(&s_fx_ssao_a_fbo, &s_fx_ssao_a_tex);
    fx_delete_fbo(&s_fx_ssao_b_fbo, &s_fx_ssao_b_tex);
    s_fx_w = s_fx_h = 0;
}

unsigned int postfx_apply(unsigned int src_tex, unsigned int depth_tex,
                          int w, int h, const PostFXParams& p,
                          float near_plane, float far_plane, float time_sec) {
    if (!p.enabled) return src_tex;
    const bool any = p.hd || p.bloom || p.ssao || p.dof || p.color_grade ||
                     p.sharpen || p.vignette || p.grain;
    if (!any) return src_tex;
    if (!fx_ensure(w, h)) return src_tex;

    // Save caller GL state so ImGui / overlays after the call are unaffected.
    GLint saved_viewport[4], saved_fbo = 0;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    const int bw = std::max(1, w / 2),  bh = std::max(1, h / 2);
    const int qw = std::max(1, w / 4),  qh = std::max(1, h / 4);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // ── SSAO: half-res AO from the depth buffer, then a separable blur ──
    // (s_grid_proj_f = 1/tan(fov/2) captured by this frame's begin_3d.)
    const bool ssao_ok = p.ssao && depth_tex;
    if (ssao_ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_ssao_a_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_ssao);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, depth_tex);
        glUniform1i(glGetUniformLocation(s_fx_ssao, "uDepth"), 0);
        glUniform2f(glGetUniformLocation(s_fx_ssao, "uTexel"), 1.0f / bw, 1.0f / bh);
        glUniform1f(glGetUniformLocation(s_fx_ssao, "uNear"), near_plane);
        glUniform1f(glGetUniformLocation(s_fx_ssao, "uFar"), far_plane);
        glUniform1f(glGetUniformLocation(s_fx_ssao, "uTanHalfFov"),
                    s_grid_proj_f > 0.0f ? 1.0f / s_grid_proj_f : 1.0f);
        glUniform1f(glGetUniformLocation(s_fx_ssao, "uAspect"),
                    h > 0 ? (float)w / (float)h : 1.0f);
        glUniform1f(glGetUniformLocation(s_fx_ssao, "uRadius"), p.ssao_radius);
        fx_quad();

        // Blur H: ssaoA -> ssaoB
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_ssao_b_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_blur);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_ssao_a_tex);
        glUniform1i(glGetUniformLocation(s_fx_blur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 1.0f / bw, 0.0f);
        fx_quad();
        // Blur V: ssaoB -> ssaoA
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_ssao_a_fbo);
        glViewport(0, 0, bw, bh);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_ssao_b_tex);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 0.0f, 1.0f / bh);
        fx_quad();
    }

    // ── Bloom chain: two bands ──
    //   half-res   — bright pass + H/V blur   (tight cores)
    //   quarter-res— downsample + H/V blur    (soft wide halo)
    if (p.bloom) {
        // Bright pass: full-res -> half-res bloomA
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_a_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_bright);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform1i(glGetUniformLocation(s_fx_bright, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_bright, "uTexel"), 1.0f / w, 1.0f / h);
        glUniform1f(glGetUniformLocation(s_fx_bright, "uThreshold"), p.bloom_threshold);
        fx_quad();

        // Blur H: bloomA -> bloomB
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_b_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_blur);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_a_tex);
        glUniform1i(glGetUniformLocation(s_fx_blur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 1.0f / bw, 0.0f);
        fx_quad();

        // Blur V: bloomB -> bloomA
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_a_fbo);
        glViewport(0, 0, bw, bh);
        glUseProgram(s_fx_blur);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_b_tex);
        glUniform1i(glGetUniformLocation(s_fx_blur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 0.0f, 1.0f / bh);
        fx_quad();

        // Wide band: bright pass over the blurred half-res -> quarter bloomC,
        // then H/V blur at quarter res. Deepens the halo without smearing the
        // core band above.
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_c_fbo);
        glViewport(0, 0, qw, qh);
        glUseProgram(s_fx_bright);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_a_tex);
        glUniform1i(glGetUniformLocation(s_fx_bright, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_bright, "uTexel"), 1.0f / bw, 1.0f / bh);
        glUniform1f(glGetUniformLocation(s_fx_bright, "uThreshold"), 0.0f);
        fx_quad();

        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_d_fbo);
        glViewport(0, 0, qw, qh);
        glUseProgram(s_fx_blur);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_c_tex);
        glUniform1i(glGetUniformLocation(s_fx_blur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 1.0f / qw, 0.0f);
        fx_quad();

        glBindFramebuffer(GL_FRAMEBUFFER, s_fx_bloom_c_fbo);
        glViewport(0, 0, qw, qh);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_fx_bloom_d_tex);
        glUniform2f(glGetUniformLocation(s_fx_blur, "uDir"), 0.0f, 1.0f / qh);
        fx_quad();
    }

    // ── Composite ──
    glBindFramebuffer(GL_FRAMEBUFFER, s_fx_out_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(s_fx_comp);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src_tex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, p.bloom ? s_fx_bloom_a_tex : 0);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, depth_tex);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D,
        ssao_ok ? s_fx_ssao_a_tex : s_fx_bloom_a_tex);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D,
        p.bloom ? s_fx_bloom_c_tex : s_fx_bloom_a_tex);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uColor"), 0);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uBloom"), 1);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uDepth"), 2);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uSsao"), 3);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uBloom2"), 4);
    glUniform2f(glGetUniformLocation(s_fx_comp, "uTexel"), 1.0f / w, 1.0f / h);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uExposure"), p.exposure);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uHdOn"), p.hd ? 1 : 0);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uBloomOn"), p.bloom ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uBloomStrength"), p.bloom_strength);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uSsaoOn"), ssao_ok ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uSsaoStrength"), p.ssao_strength);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uDofOn"), (p.dof && depth_tex) ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uDofFocus"), p.dof_focus);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uDofScale"), p.dof_scale);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uNear"), near_plane);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uFar"), far_plane);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uGradeOn"), p.color_grade ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uSaturation"), p.saturation);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uContrast"), p.contrast);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uBrightness"), p.brightness);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uWarmth"), p.warmth);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uSharpenOn"), p.sharpen ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uSharpenAmount"), p.sharpen_amount);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uVignetteOn"), p.vignette ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uVignetteStrength"), p.vignette_strength);
    glUniform1i(glGetUniformLocation(s_fx_comp, "uGrainOn"), p.grain ? 1 : 0);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uGrainAmount"), p.grain_amount);
    glUniform1f(glGetUniformLocation(s_fx_comp, "uTime"), time_sec);
    fx_quad();

    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    return s_fx_out_tex;
}

} // namespace av
