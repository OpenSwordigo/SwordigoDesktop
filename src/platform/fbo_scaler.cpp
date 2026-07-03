#define GL_GLEXT_PROTOTYPES
#include "fbo_scaler.h"
#include "platform/gl_inc.h"
#include <GL/glext.h>
#include "pvr_loader.h"
#include <iostream>
#include <cstring>
#include <cmath>

// ============================================================
//  FBO + Multi-Pass Post-Processing Pipeline
// ============================================================

static GLuint g_fbo       = 0;
static GLuint g_fbo_tex   = 0;
static GLuint g_fbo_depth_tex = 0;  // Depth as TEXTURE (not renderbuffer!) for sampling
static int    g_game_w    = 960;
static int    g_game_h    = 544;
static bool   g_fbo_ok    = false;

// Intermediate FBOs
static GLuint g_postfx_fbo  = 0, g_postfx_tex  = 0;  // Color PostFX output / composite
static GLuint g_postfx_fbo_b = 0, g_postfx_tex_b = 0; // Second full-res FBO for ping-pong
static GLuint g_half_fbo_a  = 0, g_half_tex_a  = 0;  // Half-res ping
static GLuint g_half_fbo_b  = 0, g_half_tex_b  = 0;  // Half-res pong
static GLuint g_bloom_fbo_a = 0, g_bloom_tex_a = 0;  // Bloom ping (quarter-res)
static GLuint g_bloom_fbo_b = 0, g_bloom_tex_b = 0;  // Bloom pong (quarter-res)
static int    g_half_w = 0, g_half_h = 0;
static int    g_quarter_w = 0, g_quarter_h = 0;
static float  g_postfx_time = 0.0f;

// Background data from SRE — host writes these each frame
float g_bg_depth = 11000.0f;       // Distance to background plane
float g_bg_scale = 1.0f;           // Background sprite scale
float g_bg_ambient_r = 0.6f;       // Sky ambient color (warm golden default)
float g_bg_ambient_g = 0.5f;
float g_bg_ambient_b = 0.4f;

// Portal effect data from SRE — host writes these each frame
float g_portal_active = 0.0f;          // 1.0 when portal is being drawn
float g_portal_world_x = 0.0f;        // Portal world position (from Matrix4)
float g_portal_world_y = 0.0f;
float g_portal_world_z = 0.0f;
float g_portal_color_r = 0.5f;        // Portal color from game level data
float g_portal_color_g = 0.2f;
float g_portal_color_b = 0.8f;
float g_portal_intensity = 0.8f;      // Glow strength
float g_portal_speed = 1.0f;          // Animation speed from component
float g_portal_vp_matrix[16] = {      // MV matrix from Draw call for positioning
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
};

// GUI detection — set by GL bridge (glOrthof/glFrustumf), read by PostFX pipeline
// When g_frame_has_ortho > 0 && g_frame_has_perspective == 0, game is in full-screen menu
int g_frame_has_ortho = 0;
int g_frame_has_perspective = 0;

// Shader programs
static GLuint g_prog_sharp   = 0;
static GLuint g_prog_nearest = 0;
static GLuint g_prog_crt     = 0;
static GLuint g_prog_postfx  = 0;
static GLuint g_prog_godrays = 0;
static GLuint g_prog_ssao    = 0;
static GLuint g_prog_blur    = 0;
static GLuint g_prog_composite = 0;
static GLuint g_prog_bloom_extract = 0;
static GLuint g_prog_fsr     = 0;
static GLuint g_prog_portal  = 0;   // Portal quad renderer

// Portal texture (loaded from PVR)
static GLuint g_portal_tex    = 0;   // Portal swirl texture (portal_effect_2x.pvr)
static bool   g_portal_tex_loaded = false;

// Fullscreen quad VAO/VBO (Optimization 2)
static GLuint g_fsq_vao = 0, g_fsq_vbo = 0;

// ======================= CACHED UNIFORM LOCATIONS =======================
// (Optimization 1) — eliminates ~81 glGetUniformLocation calls per frame

// g_prog_ssao uniforms
static GLint loc_ssao_depth = -1;
static GLint loc_ssao_tex_size = -1;
static GLint loc_ssao_radius = -1;
static GLint loc_ssao_intensity = -1;
static GLint loc_ssao_time = -1;
static GLint loc_ssao_near_plane = -1;
static GLint loc_ssao_far_plane = -1;

// g_prog_blur uniforms
static GLint loc_blur_tex = -1;
static GLint loc_blur_direction = -1;

// g_prog_godrays uniforms
static GLint loc_godrays_tex = -1;
static GLint loc_godrays_depth = -1;
static GLint loc_godrays_sun_pos = -1;
static GLint loc_godrays_intensity = -1;
static GLint loc_godrays_decay = -1;
static GLint loc_godrays_density = -1;
static GLint loc_godrays_exposure = -1;

// g_prog_bloom_extract uniforms
static GLint loc_bloom_extract_tex = -1;
static GLint loc_bloom_extract_threshold = -1;

// g_prog_composite uniforms
static GLint loc_comp_scene = -1;
static GLint loc_comp_ao = -1;
static GLint loc_comp_godrays = -1;
static GLint loc_comp_bloom = -1;
static GLint loc_comp_depth = -1;
static GLint loc_comp_ao_enabled = -1;
static GLint loc_comp_gr_enabled = -1;
static GLint loc_comp_bloom_enabled = -1;
static GLint loc_comp_bloom_intensity = -1;
static GLint loc_comp_gr_tint_r = -1;
static GLint loc_comp_gr_tint_g = -1;
static GLint loc_comp_gr_tint_b = -1;
static GLint loc_comp_shadow_enabled = -1;
static GLint loc_comp_shadow_intensity = -1;
static GLint loc_comp_shadow_softness = -1;
static GLint loc_comp_shadow_light_dir = -1;
static GLint loc_comp_shadow_near = -1;
static GLint loc_comp_shadow_far = -1;
static GLint loc_comp_shadow_lift = -1;
static GLint loc_comp_bg_depth = -1;
static GLint loc_comp_bg_scale = -1;
static GLint loc_comp_ambient_r = -1;
static GLint loc_comp_ambient_g = -1;
static GLint loc_comp_ambient_b = -1;

// g_prog_postfx uniforms
static GLint loc_postfx_tex = -1;
static GLint loc_postfx_depth_tex = -1;
static GLint loc_postfx_tex_size = -1;
static GLint loc_postfx_time = -1;
static GLint loc_postfx_vignette = -1;
static GLint loc_postfx_grain = -1;
static GLint loc_postfx_ca_offset = -1;
static GLint loc_postfx_saturation = -1;
static GLint loc_postfx_contrast = -1;
static GLint loc_postfx_brightness = -1;
static GLint loc_postfx_warmth = -1;
static GLint loc_postfx_sharpen = -1;
static GLint loc_postfx_outline_enabled = -1;
static GLint loc_postfx_outline_thickness = -1;
static GLint loc_postfx_outline_intensity = -1;
static GLint loc_postfx_outline_depth_threshold = -1;
static GLint loc_postfx_outline_near = -1;
static GLint loc_postfx_outline_far = -1;

// g_prog_portal uniforms
static GLint loc_portal_portal_tex = -1;
static GLint loc_portal_mv_matrix = -1;
static GLint loc_portal_aspect = -1;
static GLint loc_portal_color = -1;
static GLint loc_portal_time = -1;
static GLint loc_portal_speed = -1;
static GLint loc_portal_alpha = -1;

// ======================= SHADER SOURCES =======================

static const char* VERT_SRC = R"GLSL(
#version 330
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)GLSL";

// ----- Portal Quad Vertex Shader -----
// Uses the game's model-view matrix (from PortalEffectComponent::Draw)
// combined with a perspective projection (FOV=20°, near=50, far=20000)
// to correctly position the portal quad in screen space.
static const char* VERT_PORTAL = R"GLSL(
#version 330
layout(location = 0) in vec2 a_pos;   // -1..1 quad corners
layout(location = 1) in vec2 a_uv;    // 0..1
uniform mat4 u_mv_matrix;             // model-view matrix from game Draw call
uniform float u_aspect;               // viewport aspect ratio (w/h)
out vec2 v_uv;

void main() {
    // Portal quad in LOCAL space — sized to fill the entire stone archway.
    vec3 local_pos = vec3(a_pos.x * 55.0, a_pos.y * 75.0, 0.0);
    
    // Transform to view space using the computed Model-View matrix
    // (VIEW * WorldTranslation, confirmed by Scene::Draw decompilation)
    vec4 view_pos = u_mv_matrix * vec4(local_pos, 1.0);
    
    // Apply perspective projection (matching the game's camera)
    // FOV = 0.34906584 rad ≈ 20°, near = 50, far = 20000
    float fov = 0.34906584;
    float near = 50.0;
    float far = 20000.0;
    float f = 1.0 / tan(fov * 0.5);
    
    // Perspective projection matrix elements
    float p00 = f / u_aspect;   // X scale
    float p11 = f;              // Y scale  
    float p22 = -(far + near) / (far - near);
    float p23 = -2.0 * far * near / (far - near);
    
    gl_Position = vec4(
        view_pos.x * p00,
        view_pos.y * p11,
        view_pos.z * p22 + p23,
        -view_pos.z    // perspective divide (camera looks down -Z)
    );
    
    v_uv = a_uv;
}
)GLSL";

// ----- Portal Quad Fragment Shader -----
// Samples the portal texture with animated UV swirl.
static const char* FRAG_PORTAL = R"GLSL(
#version 330
uniform sampler2D u_portal_tex;
uniform vec3  u_color;         // portal tint color
uniform float u_time;          // animation time
uniform float u_speed;         // animation speed from game data
uniform float u_alpha;         // overall portal alpha/intensity

in vec2 v_uv;
out vec4 FragColor;

void main() {
    // Center UV for polar coordinate swirl
    vec2 centered = v_uv - 0.5;
    float dist = length(centered);
    float angle = atan(centered.y, centered.x);
    
    // Swirl: rotate UVs based on distance from center + time
    float swirl_amount = (1.0 - dist * 2.0) * 2.0;  // stronger swirl at center
    float t = u_time * u_speed;
    angle += swirl_amount * sin(t * 0.7) + t * 0.5;
    
    // Radial pull toward center
    float warped_dist = dist + sin(dist * 8.0 - t * 2.0) * 0.03;
    
    // Convert back to UV
    vec2 swirled = vec2(cos(angle), sin(angle)) * warped_dist + 0.5;
    
    // Sample portal texture with swirled UVs
    vec3 tex = texture(u_portal_tex, swirled).rgb;
    
    // Second layer: slower rotation for depth
    float angle2 = atan(centered.y, centered.x) - t * 0.3 + 1.57;
    vec2 swirled2 = vec2(cos(angle2), sin(angle2)) * warped_dist * 1.1 + 0.5;
    vec3 tex2 = texture(u_portal_tex, swirled2).rgb;
    
    // Blend layers
    vec3 portal = mix(tex, tex2, 0.4);
    
    // Apply portal color tint
    portal *= u_color;
    
    // Archway-shaped mask — wide ellipse matching the stone portal frame
    // Wider horizontally (1.0) than vertically (0.72) to match the arch shape
    vec2 ellipse = centered * vec2(1.0, 0.72);
    float eDist = length(ellipse);
    float edge_fade = smoothstep(0.50, 0.38, eDist);  // fills to the edges
    
    // Subtle center glow
    float center_glow = smoothstep(0.3, 0.0, eDist) * 0.3;
    portal += u_color * center_glow;
    
    // Pulsing intensity
    float pulse = 0.9 + 0.1 * sin(t * 2.5);
    
    FragColor = vec4(portal * pulse, edge_fade * u_alpha);
}
)GLSL";

static const char* FRAG_SHARP_BILINEAR = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform vec2 u_out_size;
in vec2 v_uv;
out vec4 FragColor;
void main() {
    vec2 scale    = u_out_size / u_tex_size;
    vec2 texel    = v_uv * u_tex_size;
    vec2 texel_f  = fract(texel);
    vec2 texel_i  = floor(texel) + 0.5;
    vec2 frange   = clamp(texel_f / fwidth(texel), 0.0, 1.0);
    vec2 uv_sharp = (texel_i + frange - 0.5) / u_tex_size;
    FragColor     = texture(u_tex, uv_sharp);
}
)GLSL";

static const char* FRAG_NEAREST = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform vec2 u_out_size;
in vec2 v_uv;
out vec4 FragColor;
void main() {
    vec2 uv = (floor(v_uv * u_tex_size) + 0.5) / u_tex_size;
    FragColor = texture(u_tex, uv);
}
)GLSL";

static const char* FRAG_CRT = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform vec2 u_out_size;
in vec2 v_uv;
out vec4 FragColor;
vec2 barrel(vec2 uv) {
    vec2 c = uv - 0.5;
    float r2 = dot(c, c);
    return uv + c * r2 * 0.04;
}
void main() {
    vec2 uv = barrel(v_uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0); return;
    }
    vec4 col = texture(u_tex, uv);
    float scanline = sin(uv.y * u_tex_size.y * 3.14159) * 0.5 + 0.5;
    col.rgb *= 0.75 + 0.25 * scanline;
    vec2 vig = uv * (1.0 - uv);
    col.rgb *= clamp(vig.x * vig.y * 15.0, 0.0, 1.0);
    FragColor = col;
}
)GLSL";

// ----- Tier 1 PostFX (color effects in single pass) -----
static const char* FRAG_POSTFX = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform sampler2D u_depth_tex;  // depth buffer for outlines
uniform vec2 u_tex_size;
uniform float u_time;
uniform float u_vignette, u_grain, u_ca_offset;
uniform float u_saturation, u_contrast, u_brightness, u_warmth, u_sharpen;
// Outline uniforms
uniform float u_outline_enabled;
uniform float u_outline_thickness;
uniform float u_outline_intensity;
uniform float u_outline_depth_threshold;
uniform float u_outline_near;
uniform float u_outline_far;

in vec2 v_uv;
out vec4 FragColor;

float rand(vec2 co) {
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

float lin_depth(float d) {
    float n = u_outline_near;
    float f = u_outline_far;
    return (2.0 * n) / (f + n - d * (f - n));
}

float detect_edge() {
    vec2 px = u_outline_thickness / u_tex_size;
    
    // Sample depth in a 3x3 cross pattern (Sobel-like)
    float d_c = lin_depth(texture(u_depth_tex, v_uv).r);
    float d_l = lin_depth(texture(u_depth_tex, v_uv + vec2(-px.x, 0)).r);
    float d_r = lin_depth(texture(u_depth_tex, v_uv + vec2( px.x, 0)).r);
    float d_u = lin_depth(texture(u_depth_tex, v_uv + vec2(0,  px.y)).r);
    float d_d = lin_depth(texture(u_depth_tex, v_uv + vec2(0, -px.y)).r);
    
    // Depth gradient (Sobel approximation)
    float edge_h = abs(d_l - d_r);
    float edge_v = abs(d_u - d_d);
    float depth_edge = max(edge_h, edge_v);
    
    // Also detect color edges (for objects at same depth with different textures)
    vec3 c_c = texture(u_tex, v_uv).rgb;
    vec3 c_l = texture(u_tex, v_uv + vec2(-px.x, 0)).rgb;
    vec3 c_r = texture(u_tex, v_uv + vec2( px.x, 0)).rgb;
    vec3 c_u = texture(u_tex, v_uv + vec2(0,  px.y)).rgb;
    vec3 c_d = texture(u_tex, v_uv + vec2(0, -px.y)).rgb;
    
    float color_edge_h = length(c_l - c_r);
    float color_edge_v = length(c_u - c_d);
    float color_edge = max(color_edge_h, color_edge_v);
    
    // Combine: depth edges are primary, color edges are secondary
    float edge = smoothstep(u_outline_depth_threshold, u_outline_depth_threshold * 3.0, depth_edge);
    edge = max(edge, smoothstep(0.3, 0.6, color_edge) * 0.5);
    
    // Skip background (far depth = no outline on sky)
    if (d_c > 0.95) edge = 0.0;
    
    return edge;
}

void main() {
    vec2 uv = v_uv;
    vec2 px = 1.0 / u_tex_size;
    vec4 col;
    if (u_ca_offset > 0.0) {
        vec2 dir = (uv - 0.5) * u_ca_offset;
        col.r = texture(u_tex, uv - dir).r;
        col.g = texture(u_tex, uv).g;
        col.b = texture(u_tex, uv + dir).b;
        col.a = 1.0;
    } else {
        col = texture(u_tex, uv);
    }
    if (u_sharpen > 0.0) {
        vec4 blur = (texture(u_tex, uv + vec2(-px.x,0)) + texture(u_tex, uv + vec2(px.x,0)) +
                     texture(u_tex, uv + vec2(0,-px.y)) + texture(u_tex, uv + vec2(0,px.y))) * 0.25;
        col.rgb += (col.rgb - blur.rgb) * u_sharpen;
    }
    col.rgb += u_brightness;
    col.rgb = (col.rgb - 0.5) * u_contrast + 0.5;
    float lum = dot(col.rgb, vec3(0.2126, 0.7152, 0.0722));
    col.rgb = mix(vec3(lum), col.rgb, u_saturation);
    if (u_warmth != 0.0) { col.r += u_warmth * 0.05; col.b -= u_warmth * 0.05; }
    if (u_grain > 0.0) { col.rgb += (rand(uv + fract(u_time)) * 2.0 - 1.0) * u_grain; }
    if (u_vignette > 0.0) {
        vec2 vig = uv * (1.0 - uv);
        col.rgb *= pow(clamp(vig.x * vig.y * 15.0, 0.0, 1.0), u_vignette);
    }
    
    // Edge outlines — color-aware: darkened shade of surrounding color
    // Green areas → dark green outlines, brown → dark brown, blue → dark blue
    if (u_outline_enabled > 0.5) {
        float edge = detect_edge();
        if (edge > 0.01) {
            // Sample average color from neighbors for the outline tint
            vec2 opx = u_outline_thickness / u_tex_size;
            vec3 avg_color = (
                texture(u_tex, v_uv + vec2(-opx.x, 0)).rgb +
                texture(u_tex, v_uv + vec2( opx.x, 0)).rgb +
                texture(u_tex, v_uv + vec2(0, -opx.y)).rgb +
                texture(u_tex, v_uv + vec2(0,  opx.y)).rgb
            ) * 0.25;
            
            // Outline = darkened + slightly more saturated version of local color
            // This makes outlines feel like natural contour shadows
            float avg_luma = dot(avg_color, vec3(0.2126, 0.7152, 0.0722));
            vec3 outline_color = mix(vec3(avg_luma), avg_color, 1.3); // boost saturation 30%
            outline_color *= 0.3;  // darken to ~30% brightness
            
            col.rgb = mix(col.rgb, outline_color, edge * u_outline_intensity);
        }
    }
    
    FragColor = vec4(clamp(col.rgb, 0.0, 1.0), 1.0);
}
)GLSL";

// ----- God Rays (screen-space radial blur from sun position) -----
static const char* FRAG_GODRAYS = R"GLSL(
#version 330
uniform sampler2D u_tex;      // scene color
uniform sampler2D u_depth;    // depth texture
uniform vec2 u_sun_pos;       // sun position in UV space
uniform float u_intensity;    // overall intensity
uniform float u_decay;        // per-sample decay
uniform float u_density;      // sample spacing
uniform float u_exposure;     // final exposure multiply
in vec2 v_uv;
out vec4 FragColor;

float rand(vec2 co) {
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    // Direction from this pixel toward the sun
    vec2 delta = v_uv - u_sun_pos;
    float dist = length(delta);
    
    // Anisotropic scattering: brighter when looking closer to the sun
    float scattering = 1.0 / (1.0 + dist * 2.0);
    
    // Sun corona/glow: radial glow centered on the sun
    float sun_glow = smoothstep(0.25, 0.0, dist) * 0.4;
    
    delta = delta / max(dist, 0.001) * (1.0 / 64.0) * u_density;
    
    // Dither starting offset to eliminate banding artifacts
    float dither = rand(v_uv);
    vec2 uv = v_uv + delta * dither;
    
    float illumination_decay = 1.0;
    vec3 god_ray = vec3(0.0);
    
    // March toward the sun, accumulating light
    for (int i = 0; i < 64; i++) {
        uv -= delta;
        // Clamp to valid UV range
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
        
        vec3 sample_col = texture(u_tex, uv).rgb;
        float depth = texture(u_depth, uv).r;
        
        // Depth masking — only sky/background (far pixels) can emit light
        // Depth near 1.0 = far/sky, near 0.0 = close (foreground occluder)
        float depth_mask = smoothstep(0.95, 1.0, depth);
        
        // Brightness threshold for the light source
        float bright = max(max(sample_col.r, sample_col.g), sample_col.b);
        float brightness_mask = smoothstep(0.2, 0.7, bright);
        
        // Combined emission: only the sky/sun region emits rays. 
        // Close objects (depth <= 0.95) block the light (emission = 0.0).
        float emission = depth_mask * brightness_mask;
        
        god_ray += sample_col * emission * illumination_decay;
        illumination_decay *= u_decay;
    }
    
    god_ray *= u_exposure / 64.0;
    
    // Add sun corona and apply intensity & scattering
    vec3 final_ray = god_ray * u_intensity * (1.0 + scattering * 0.5) + vec3(sun_glow * u_intensity);
    FragColor = vec4(clamp(final_ray, 0.0, 1.0), 1.0);
}
)GLSL";

// ----- SSAO (Screen Space Ambient Occlusion) -----
static const char* FRAG_SSAO = R"GLSL(
#version 330
uniform sampler2D u_depth;    // depth texture
uniform vec2 u_tex_size;      // resolution
uniform float u_radius;       // sample radius in UV
uniform float u_intensity;    // AO strength
uniform float u_time;         // for noise
uniform float u_near_plane;   // camera near plane (game uses 50.0)
uniform float u_far_plane;    // camera far plane  (game uses 20000.0)
in vec2 v_uv;
out vec4 FragColor;

float rand(vec2 co) {
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

float linearize_depth(float d) {
    return (2.0 * u_near_plane) / (u_far_plane + u_near_plane - d * (u_far_plane - u_near_plane));
}

void main() {
    float depth = texture(u_depth, v_uv).r;
    float center_z = linearize_depth(depth);
    
    // Skip sky/far pixels
    if (depth > 0.999) {
        FragColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    
    float occlusion = 0.0;
    int samples = 16;
    float angle_step = 6.2831853 / float(samples);
    
    // Rotate sample pattern per pixel for noise
    float noise_angle = rand(v_uv + fract(u_time * 0.1)) * 6.2831853;
    
    for (int i = 0; i < 16; i++) {
        float angle = float(i) * angle_step + noise_angle;
        // Vary radius per sample
        float r = u_radius * (0.3 + 0.7 * rand(vec2(float(i) * 0.1, v_uv.x)));
        vec2 offset = vec2(cos(angle), sin(angle)) * r;
        
        // Aspect ratio correction
        offset.x *= u_tex_size.y / u_tex_size.x;
        
        vec2 sample_uv = v_uv + offset;
        float sample_depth = texture(u_depth, sample_uv).r;
        float sample_z = linearize_depth(sample_depth);
        
        // If sample is closer to the camera than center (sample_z < center_z), it blocks light
        float occluded = step(sample_z, center_z - 0.001);
        
        // Range check: check if neighboring sample is within depth radius to prevent false halos
        float range_check = smoothstep(u_radius * 2.0, 0.0, abs(center_z - sample_z));
        
        occlusion += occluded * range_check;
    }
    
    float ao = 1.0 - (occlusion / float(samples)) * u_intensity;
    ao = clamp(ao, 0.0, 1.0);
    
    FragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";

// ----- Gaussian Blur (separable, for SSAO smoothing) -----
static const char* FRAG_BLUR = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform vec2 u_direction;   // (1/w, 0) for horizontal, (0, 1/h) for vertical
in vec2 v_uv;
out vec4 FragColor;
void main() {
    vec4 col = vec4(0.0);
    // 9-tap Gaussian kernel
    col += texture(u_tex, v_uv - 4.0 * u_direction) * 0.0162;
    col += texture(u_tex, v_uv - 3.0 * u_direction) * 0.0540;
    col += texture(u_tex, v_uv - 2.0 * u_direction) * 0.1216;
    col += texture(u_tex, v_uv - 1.0 * u_direction) * 0.1945;
    col += texture(u_tex, v_uv)                      * 0.2270;
    col += texture(u_tex, v_uv + 1.0 * u_direction) * 0.1945;
    col += texture(u_tex, v_uv + 2.0 * u_direction) * 0.1216;
    col += texture(u_tex, v_uv + 3.0 * u_direction) * 0.0540;
    col += texture(u_tex, v_uv + 4.0 * u_direction) * 0.0162;
    FragColor = col;
}
)GLSL";

// ----- Bloom: extract bright areas -----
static const char* FRAG_BLOOM_EXTRACT = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform float u_threshold;
in vec2 v_uv;
out vec4 FragColor;
void main() {
    vec3 col = texture(u_tex, v_uv).rgb;
    float brightness = dot(col, vec3(0.2126, 0.7152, 0.0722));
    // Soft knee extraction — smoother than hard cutoff
    float knee = max(0.0, brightness - u_threshold) / max(brightness, 0.001);
    FragColor = vec4(col * knee, 1.0);
}
)GLSL";

// ----- Composite: scene + AO + god rays + bloom + shadows -----
static const char* FRAG_COMPOSITE = R"GLSL(
#version 330
uniform sampler2D u_scene;      // original scene
uniform sampler2D u_ao;         // SSAO result (grayscale)
uniform sampler2D u_godrays;    // god rays (additive light)
uniform sampler2D u_bloom;      // bloom result (additive glow)
uniform sampler2D u_depth;      // depth buffer for shadows
uniform float u_ao_enabled;     // 0 or 1
uniform float u_gr_enabled;     // 0 or 1
uniform float u_bloom_enabled;  // 0 or 1
uniform float u_bloom_intensity; // bloom strength
uniform float u_gr_tint_r, u_gr_tint_g, u_gr_tint_b;  // god ray color tint

// === Shadows ===
uniform float u_shadow_enabled;  // 0 or 1
uniform float u_shadow_intensity;
uniform float u_shadow_softness;
uniform vec2  u_shadow_light_dir;  // light direction (normalized, in UV space)
uniform float u_shadow_near;
uniform float u_shadow_far;

// === Background-driven atmosphere (from SRE) ===
uniform float u_bg_depth;       // distance to background plane
uniform float u_bg_scale;       // background scale (larger = closer sky)
uniform float u_shadow_lift;    // how much to brighten shadows (0.0-0.5)
uniform float u_ambient_r, u_ambient_g, u_ambient_b;  // sky ambient color

in vec2 v_uv;
out vec4 FragColor;

float linearize_depth(float d) {
    float n = u_shadow_near;
    float f = u_shadow_far;
    return (2.0 * n) / (f + n - d * (f - n));
}

float compute_shadow() {
    // Screen-space directional shadow via depth ray marching
    // March FROM this pixel TOWARD the light source.
    // If any sample along the ray has CLOSER depth, we're in shadow.
    
    float my_depth = linearize_depth(texture(u_depth, v_uv).r);
    
    // Skip far background pixels (no shadow on sky/backgrounds)
    if (my_depth > 0.95) return 1.0;
    
    float shadow = 1.0;
    vec2 step_dir = u_shadow_light_dir * u_shadow_softness;
    
    // March 12 steps toward the light
    for (int i = 1; i <= 12; i++) {
        vec2 sample_uv = v_uv + step_dir * float(i);
        
        // Bounds check
        if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || 
            sample_uv.y < 0.0 || sample_uv.y > 1.0) break;
        
        float sample_depth = linearize_depth(texture(u_depth, sample_uv).r);
        
        // If the sample is CLOSER to the camera than us, it's an occluder
        float depth_diff = my_depth - sample_depth;
        
        // Only count as shadow if occluder is close in depth (not a distant wall)
        // and actually closer than us
        if (depth_diff > 0.001 && depth_diff < 0.15) {
            // Closer samples = harder shadow, distant samples = softer
            float falloff = 1.0 - float(i) / 12.0;
            shadow = min(shadow, 1.0 - u_shadow_intensity * falloff);
        }
    }
    
    return shadow;
}

void main() {
    vec3 scene = texture(u_scene, v_uv).rgb;
    
    // Apply SSAO with shadow lift — don't let AO darken too much
    if (u_ao_enabled > 0.5) {
        float ao = texture(u_ao, v_uv).r;
        ao = mix(ao, 1.0, u_shadow_lift);
        scene *= ao;
    }
    
    // Apply screen-space shadows
    if (u_shadow_enabled > 0.5) {
        float shadow = compute_shadow();
        scene *= shadow;
    }
    
    // Ambient sky color influence — subtly tint shadows with sky color
    float luma = dot(scene, vec3(0.2126, 0.7152, 0.0722));
    float shadow_mask = smoothstep(0.4, 0.1, luma);  // 1.0 in dark areas
    vec3 ambient = vec3(u_ambient_r, u_ambient_g, u_ambient_b);
    scene += ambient * shadow_mask * 0.08;  // subtle fill light
    
    // Apply God Rays (additive with tint)
    if (u_gr_enabled > 0.5) {
        vec3 rays = texture(u_godrays, v_uv).rgb;
        vec3 tint = vec3(u_gr_tint_r, u_gr_tint_g, u_gr_tint_b);
        scene += rays * tint;
    }
    
    // Apply Bloom (additive glow)
    if (u_bloom_enabled > 0.5) {
        vec3 bloom = texture(u_bloom, v_uv).rgb;
        scene += bloom * u_bloom_intensity;
    }
    
    // --- Tone mapping (Extended Reinhard) ---
    float Lwhite = 2.5;
    scene = scene * (1.0 + scene / (Lwhite * Lwhite)) / (1.0 + scene);
    
    // --- Saturation boost ---
    float luma2 = dot(scene, vec3(0.2126, 0.7152, 0.0722));
    scene = mix(vec3(luma2), scene, 1.15);
    
    // --- Contrast micro-curve ---
    scene = smoothstep(0.0, 1.0, scene);
    
    FragColor = vec4(clamp(scene, 0.0, 1.0), 1.0);
}
)GLSL";

// ----- FSR 1.0 EASU: Edge-Adaptive Spatial Upscaling (simplified) -----
// Based on AMD FidelityFX Super Resolution 1.0
// 12-tap filter with edge detection for sharp, artifact-free upscaling
static const char* FRAG_FSR = R"GLSL(
#version 330
uniform sampler2D u_tex;
uniform vec2 u_tex_size;   // input texture resolution
uniform vec2 u_out_size;   // output resolution
in vec2 v_uv;
out vec4 FragColor;

// Compute Lanczos2 weight
float lanczos2(float x) {
    if (abs(x) < 1e-6) return 1.0;
    if (abs(x) >= 2.0) return 0.0;
    float pi_x = 3.14159265 * x;
    return sin(pi_x) * sin(pi_x * 0.5) / (pi_x * pi_x * 0.5);
}

void main() {
    vec2 pixel = v_uv * u_tex_size;
    vec2 frac_pos = fract(pixel) - 0.5;
    vec2 pixel_center = floor(pixel) + 0.5;
    vec2 texel = 1.0 / u_tex_size;
    
    // Sample 4x4 neighborhood (16 taps, but we use 12 inner ones)
    vec3 samples[4][4];
    for (int y = -1; y <= 2; y++) {
        for (int x = -1; x <= 2; x++) {
            vec2 uv = (pixel_center + vec2(x, y)) * texel;
            samples[y+1][x+1] = texture(u_tex, uv).rgb;
        }
    }
    
    // Edge detection: compute gradients
    vec3 dx = (samples[1][2] - samples[1][0]) + (samples[2][2] - samples[2][0]);
    vec3 dy = (samples[2][1] - samples[0][1]) + (samples[2][2] - samples[0][2]);
    float edge_h = dot(abs(dx), vec3(0.333));
    float edge_v = dot(abs(dy), vec3(0.333));
    
    // Directional sharpness: sharpen along edges, smooth across them
    float edge_strength = min(edge_h + edge_v, 1.0);
    float sharpness = mix(0.5, 1.0, edge_strength); // More sharpness at edges
    
    // Lanczos2 filter with directional weighting
    vec3 color = vec3(0.0);
    float total_weight = 0.0;
    
    for (int y = -1; y <= 2; y++) {
        for (int x = -1; x <= 2; x++) {
            vec2 d = frac_pos - vec2(x, y);
            
            // Directional scaling: stretch kernel along edge direction
            float w;
            if (edge_h > edge_v * 1.5) {
                // Horizontal edge: sharpen vertically
                w = lanczos2(d.x) * lanczos2(d.y * sharpness);
            } else if (edge_v > edge_h * 1.5) {
                // Vertical edge: sharpen horizontally
                w = lanczos2(d.x * sharpness) * lanczos2(d.y);
            } else {
                // No strong edge: isotropic
                w = lanczos2(d.x) * lanczos2(d.y);
            }
            
            color += samples[y+1][x+1] * w;
            total_weight += w;
        }
    }
    
    color /= max(total_weight, 0.001);
    
    // Clamp to prevent ringing artifacts
    vec3 min_col = min(min(samples[1][1], samples[1][2]), min(samples[2][1], samples[2][2]));
    vec3 max_col = max(max(samples[1][1], samples[1][2]), max(samples[2][1], samples[2][2]));
    color = clamp(color, min_col, max_col);
    
    // Subtle RCAS (sharpening) pass — integrated
    vec3 center = samples[1][1];
    vec3 n = samples[0][1], s = samples[2][1], e = samples[1][2], w2 = samples[1][0];
    vec3 sharp = center * 5.0 - n - s - e - w2;
    color = mix(color, clamp(sharp, min_col, max_col), 0.15);
    
    FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)GLSL";

// ======================= SHADER HELPERS =======================

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(sh, 1024, nullptr, buf);
        std::cerr << "[FBO] Shader compile error: " << buf << std::endl;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char* vert, const char* frag) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag);
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetProgramInfoLog(prog, 1024, nullptr, buf);
        std::cerr << "[FBO] Program link error: " << buf << std::endl;
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static GLuint get_program(FBOScale mode) {
    switch (mode) {
        case FBOScale::NEAREST:
            if (!g_prog_nearest) g_prog_nearest = link_program(VERT_SRC, FRAG_NEAREST);
            return g_prog_nearest;
        case FBOScale::CRT_SCANLINE:
            if (!g_prog_crt) g_prog_crt = link_program(VERT_SRC, FRAG_CRT);
            return g_prog_crt;
        case FBOScale::FSR:
            if (!g_prog_fsr) g_prog_fsr = link_program(VERT_SRC, FRAG_FSR);
            return g_prog_fsr;
        default:
            if (!g_prog_sharp) g_prog_sharp = link_program(VERT_SRC, FRAG_SHARP_BILINEAR);
            return g_prog_sharp;
    }
}

// Fullscreen quad data (uploaded to VBO during fbo_init)
static const float FSQ[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
};

static void draw_fsq() {
    glBindVertexArray(g_fsq_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);  // MUST unbind — game uses GL1.x client-side vertex arrays
}

// Helper: create an FBO with color texture
static bool create_fbo(GLuint& fbo, GLuint& tex, int w, int h) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return status == GL_FRAMEBUFFER_COMPLETE;
}

// ======================= PUBLIC API =======================

bool fbo_init(int game_w, int game_h) {
    g_game_w = game_w;
    g_game_h = game_h;
    g_half_w = game_w / 2;
    g_half_h = game_h / 2;

    // --- Main game FBO with color texture + depth TEXTURE ---
    glGenTextures(1, &g_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, game_w, game_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Depth as TEXTURE (not renderbuffer!) so we can sample it in shaders
    glGenTextures(1, &g_fbo_depth_tex);
    glBindTexture(GL_TEXTURE_2D, g_fbo_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, game_w, game_h, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Important: allow reading depth component when sampling
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_fbo_depth_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[FBO] Main framebuffer incomplete — status 0x"
                  << std::hex << status << std::dec << std::endl;
        g_fbo_ok = false;
        return false;
    }

    // --- Intermediate FBOs ---
    if (!create_fbo(g_postfx_fbo, g_postfx_tex, game_w, game_h)) {
        std::cerr << "[FBO] PostFX FBO A incomplete" << std::endl;
    }
    if (!create_fbo(g_postfx_fbo_b, g_postfx_tex_b, game_w, game_h)) {
        std::cerr << "[FBO] PostFX FBO B incomplete" << std::endl;
    }
    if (!create_fbo(g_half_fbo_a, g_half_tex_a, g_half_w, g_half_h)) {
        std::cerr << "[FBO] Half-res FBO A incomplete" << std::endl;
    }
    if (!create_fbo(g_half_fbo_b, g_half_tex_b, g_half_w, g_half_h)) {
        std::cerr << "[FBO] Half-res FBO B incomplete" << std::endl;
    }
    g_quarter_w = game_w / 4;
    g_quarter_h = game_h / 4;
    if (!create_fbo(g_bloom_fbo_a, g_bloom_tex_a, g_quarter_w, g_quarter_h)) {
        std::cerr << "[FBO] Bloom FBO A incomplete" << std::endl;
    }
    if (!create_fbo(g_bloom_fbo_b, g_bloom_tex_b, g_quarter_w, g_quarter_h)) {
        std::cerr << "[FBO] Bloom FBO B incomplete" << std::endl;
    }

    // --- Pre-compile shaders ---
    g_prog_sharp     = link_program(VERT_SRC, FRAG_SHARP_BILINEAR);
    g_prog_postfx    = link_program(VERT_SRC, FRAG_POSTFX);
    g_prog_godrays   = link_program(VERT_SRC, FRAG_GODRAYS);
    g_prog_ssao      = link_program(VERT_SRC, FRAG_SSAO);
    g_prog_blur      = link_program(VERT_SRC, FRAG_BLUR);
    g_prog_composite = link_program(VERT_SRC, FRAG_COMPOSITE);
    g_prog_bloom_extract = link_program(VERT_SRC, FRAG_BLOOM_EXTRACT);
    g_prog_portal    = link_program(VERT_PORTAL, FRAG_PORTAL);
    if (g_prog_portal) {
        std::cout << "[FBO] Portal shader compiled OK (program=" << g_prog_portal << ")" << std::endl;
    } else {
        std::cerr << "[FBO] WARNING: Portal shader FAILED to compile — portal effects disabled" << std::endl;
    }

    // --- Cache uniform locations (Optimization 1) ---
    if (g_prog_ssao) {
        loc_ssao_depth      = glGetUniformLocation(g_prog_ssao, "u_depth");
        loc_ssao_tex_size   = glGetUniformLocation(g_prog_ssao, "u_tex_size");
        loc_ssao_radius     = glGetUniformLocation(g_prog_ssao, "u_radius");
        loc_ssao_intensity  = glGetUniformLocation(g_prog_ssao, "u_intensity");
        loc_ssao_time       = glGetUniformLocation(g_prog_ssao, "u_time");
        loc_ssao_near_plane = glGetUniformLocation(g_prog_ssao, "u_near_plane");
        loc_ssao_far_plane  = glGetUniformLocation(g_prog_ssao, "u_far_plane");
    }
    if (g_prog_blur) {
        loc_blur_tex       = glGetUniformLocation(g_prog_blur, "u_tex");
        loc_blur_direction = glGetUniformLocation(g_prog_blur, "u_direction");
    }
    if (g_prog_godrays) {
        loc_godrays_tex       = glGetUniformLocation(g_prog_godrays, "u_tex");
        loc_godrays_depth     = glGetUniformLocation(g_prog_godrays, "u_depth");
        loc_godrays_sun_pos   = glGetUniformLocation(g_prog_godrays, "u_sun_pos");
        loc_godrays_intensity = glGetUniformLocation(g_prog_godrays, "u_intensity");
        loc_godrays_decay     = glGetUniformLocation(g_prog_godrays, "u_decay");
        loc_godrays_density   = glGetUniformLocation(g_prog_godrays, "u_density");
        loc_godrays_exposure  = glGetUniformLocation(g_prog_godrays, "u_exposure");
    }
    if (g_prog_bloom_extract) {
        loc_bloom_extract_tex       = glGetUniformLocation(g_prog_bloom_extract, "u_tex");
        loc_bloom_extract_threshold = glGetUniformLocation(g_prog_bloom_extract, "u_threshold");
    }
    if (g_prog_composite) {
        loc_comp_scene            = glGetUniformLocation(g_prog_composite, "u_scene");
        loc_comp_ao               = glGetUniformLocation(g_prog_composite, "u_ao");
        loc_comp_godrays          = glGetUniformLocation(g_prog_composite, "u_godrays");
        loc_comp_bloom            = glGetUniformLocation(g_prog_composite, "u_bloom");
        loc_comp_depth            = glGetUniformLocation(g_prog_composite, "u_depth");
        loc_comp_ao_enabled       = glGetUniformLocation(g_prog_composite, "u_ao_enabled");
        loc_comp_gr_enabled       = glGetUniformLocation(g_prog_composite, "u_gr_enabled");
        loc_comp_bloom_enabled    = glGetUniformLocation(g_prog_composite, "u_bloom_enabled");
        loc_comp_bloom_intensity  = glGetUniformLocation(g_prog_composite, "u_bloom_intensity");
        loc_comp_gr_tint_r        = glGetUniformLocation(g_prog_composite, "u_gr_tint_r");
        loc_comp_gr_tint_g        = glGetUniformLocation(g_prog_composite, "u_gr_tint_g");
        loc_comp_gr_tint_b        = glGetUniformLocation(g_prog_composite, "u_gr_tint_b");
        loc_comp_shadow_enabled   = glGetUniformLocation(g_prog_composite, "u_shadow_enabled");
        loc_comp_shadow_intensity = glGetUniformLocation(g_prog_composite, "u_shadow_intensity");
        loc_comp_shadow_softness  = glGetUniformLocation(g_prog_composite, "u_shadow_softness");
        loc_comp_shadow_light_dir = glGetUniformLocation(g_prog_composite, "u_shadow_light_dir");
        loc_comp_shadow_near      = glGetUniformLocation(g_prog_composite, "u_shadow_near");
        loc_comp_shadow_far       = glGetUniformLocation(g_prog_composite, "u_shadow_far");
        loc_comp_shadow_lift      = glGetUniformLocation(g_prog_composite, "u_shadow_lift");
        loc_comp_bg_depth         = glGetUniformLocation(g_prog_composite, "u_bg_depth");
        loc_comp_bg_scale         = glGetUniformLocation(g_prog_composite, "u_bg_scale");
        loc_comp_ambient_r        = glGetUniformLocation(g_prog_composite, "u_ambient_r");
        loc_comp_ambient_g        = glGetUniformLocation(g_prog_composite, "u_ambient_g");
        loc_comp_ambient_b        = glGetUniformLocation(g_prog_composite, "u_ambient_b");
    }
    if (g_prog_postfx) {
        loc_postfx_tex                  = glGetUniformLocation(g_prog_postfx, "u_tex");
        loc_postfx_depth_tex            = glGetUniformLocation(g_prog_postfx, "u_depth_tex");
        loc_postfx_tex_size             = glGetUniformLocation(g_prog_postfx, "u_tex_size");
        loc_postfx_time                 = glGetUniformLocation(g_prog_postfx, "u_time");
        loc_postfx_vignette             = glGetUniformLocation(g_prog_postfx, "u_vignette");
        loc_postfx_grain                = glGetUniformLocation(g_prog_postfx, "u_grain");
        loc_postfx_ca_offset            = glGetUniformLocation(g_prog_postfx, "u_ca_offset");
        loc_postfx_saturation           = glGetUniformLocation(g_prog_postfx, "u_saturation");
        loc_postfx_contrast             = glGetUniformLocation(g_prog_postfx, "u_contrast");
        loc_postfx_brightness           = glGetUniformLocation(g_prog_postfx, "u_brightness");
        loc_postfx_warmth               = glGetUniformLocation(g_prog_postfx, "u_warmth");
        loc_postfx_sharpen              = glGetUniformLocation(g_prog_postfx, "u_sharpen");
        loc_postfx_outline_enabled      = glGetUniformLocation(g_prog_postfx, "u_outline_enabled");
        loc_postfx_outline_thickness    = glGetUniformLocation(g_prog_postfx, "u_outline_thickness");
        loc_postfx_outline_intensity    = glGetUniformLocation(g_prog_postfx, "u_outline_intensity");
        loc_postfx_outline_depth_threshold = glGetUniformLocation(g_prog_postfx, "u_outline_depth_threshold");
        loc_postfx_outline_near         = glGetUniformLocation(g_prog_postfx, "u_outline_near");
        loc_postfx_outline_far          = glGetUniformLocation(g_prog_postfx, "u_outline_far");
    }
    if (g_prog_portal) {
        loc_portal_portal_tex = glGetUniformLocation(g_prog_portal, "u_portal_tex");
        loc_portal_mv_matrix  = glGetUniformLocation(g_prog_portal, "u_mv_matrix");
        loc_portal_aspect     = glGetUniformLocation(g_prog_portal, "u_aspect");
        loc_portal_color      = glGetUniformLocation(g_prog_portal, "u_color");
        loc_portal_time       = glGetUniformLocation(g_prog_portal, "u_time");
        loc_portal_speed      = glGetUniformLocation(g_prog_portal, "u_speed");
        loc_portal_alpha      = glGetUniformLocation(g_prog_portal, "u_alpha");
    }

    // --- Create fullscreen quad VAO/VBO (Optimization 2) ---
    glGenVertexArrays(1, &g_fsq_vao);
    glGenBuffers(1, &g_fsq_vbo);
    glBindVertexArray(g_fsq_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_fsq_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(FSQ), FSQ, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    g_fbo_ok = true;
    std::cout << "[FBO] Pipeline ready: " << game_w << "x" << game_h
              << " | half=" << g_half_w << "x" << g_half_h
              << " | quarter=" << g_quarter_w << "x" << g_quarter_h
              << " | depth=texture | SSAO+GodRays+Bloom+PostFX+Portal" << std::endl;
    return true;
}

void fbo_destroy() {
    auto del_fbo = [](GLuint& f) { if (f) { glDeleteFramebuffers(1, &f); f = 0; } };
    auto del_tex = [](GLuint& t) { if (t) { glDeleteTextures(1, &t); t = 0; } };
    auto del_prg = [](GLuint& p) { if (p) { glDeleteProgram(p); p = 0; } };
    del_fbo(g_fbo); del_tex(g_fbo_tex); del_tex(g_fbo_depth_tex);
    del_fbo(g_postfx_fbo); del_tex(g_postfx_tex);
    del_fbo(g_postfx_fbo_b); del_tex(g_postfx_tex_b);
    del_fbo(g_half_fbo_a); del_tex(g_half_tex_a);
    del_fbo(g_half_fbo_b); del_tex(g_half_tex_b);
    del_fbo(g_bloom_fbo_a); del_tex(g_bloom_tex_a);
    del_fbo(g_bloom_fbo_b); del_tex(g_bloom_tex_b);
    del_prg(g_prog_sharp); del_prg(g_prog_nearest); del_prg(g_prog_crt);
    del_prg(g_prog_postfx); del_prg(g_prog_godrays); del_prg(g_prog_ssao);
    del_prg(g_prog_blur); del_prg(g_prog_composite);
    del_prg(g_prog_bloom_extract); del_prg(g_prog_fsr);
    del_prg(g_prog_portal);
    del_tex(g_portal_tex); g_portal_tex_loaded = false;
    // Delete fullscreen quad VAO/VBO (Optimization 2)
    if (g_fsq_vao) { glDeleteVertexArrays(1, &g_fsq_vao); g_fsq_vao = 0; }
    if (g_fsq_vbo) { glDeleteBuffers(1, &g_fsq_vbo); g_fsq_vbo = 0; }
    g_fbo_ok = false;
}

void fbo_begin_game() {
    if (!g_fbo_ok) return;
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_game_w, g_game_h);
    
    static int begin_diag_count = 0;
    if (begin_diag_count < 5) {
        GLenum err = glGetError();
        std::cout << "[FBO-Diag] fbo_begin_game: FBO=" << g_fbo << " size=" << g_game_w << "x" << g_game_h << " err=" << err << std::endl;
        begin_diag_count++;
    }
}

void fbo_end_game_and_blit(int win_w, int win_h, FBOScale mode, const PostFXState* postfx) {
    if (!g_fbo_ok) return;

    static int end_diag_count = 0;
    if (end_diag_count < 5) {
        GLenum err = glGetError();
        std::cout << "[FBO-Diag] fbo_end_game_and_blit: win=" << win_w << "x" << win_h << " mode=" << (int)mode << " err=" << err << std::endl;
        end_diag_count++;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    // Clear to magenta for diagnostic visual visibility (if FBO is empty/transparent, screen turns magenta)
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Save GL state manually (Optimization 3 — replaces glPushAttrib/glPopAttrib)
    GLint old_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    GLboolean saved_depth_test = glIsEnabled(GL_DEPTH_TEST);
    GLboolean saved_blend      = glIsEnabled(GL_BLEND);
    GLboolean saved_cull_face  = glIsEnabled(GL_CULL_FACE);
    GLboolean saved_scissor    = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean saved_stencil    = glIsEnabled(GL_STENCIL_TEST);
    GLboolean saved_alpha_test = glIsEnabled(GL_ALPHA_TEST);
    GLboolean saved_lighting   = glIsEnabled(GL_LIGHTING);
    GLint saved_blend_src = 0, saved_blend_dst = 0;
    glGetIntegerv(GL_BLEND_SRC, &saved_blend_src);
    glGetIntegerv(GL_BLEND_DST, &saved_blend_dst);
    GLint saved_depth_func = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &saved_depth_func);
    GLboolean saved_depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_depth_mask);
    GLint saved_vao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    GLint saved_active_tex = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_tex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_LIGHTING);

    GLuint final_tex = g_fbo_tex;
    g_postfx_time += 0.016f;
    if (g_postfx_time > 10000.0f) g_postfx_time = 0.0f;

    // GUI detection counters — reset each frame (set by GL bridge)
    // NOTE: glFrustumf detection doesn't work because Caver uses glLoadMatrixf
    // for projection, not glFrustumf. Need to hook a game menu function instead.
    // For now, counters are maintained but NOT used for suppression.
    g_frame_has_ortho = 0;
    g_frame_has_perspective = 0;

    bool do_postfx = postfx && postfx->enabled && g_prog_postfx && g_postfx_fbo;
    bool do_ssao   = do_postfx && postfx->ssao && g_prog_ssao && g_prog_blur && g_half_fbo_a;
    bool do_godrays = do_postfx && (postfx->god_rays || postfx->volumetric_light) && g_prog_godrays && g_half_fbo_a;
    bool do_bloom  = do_postfx && postfx->bloom && g_prog_bloom_extract && g_prog_blur && g_bloom_fbo_a;
    bool do_shadows = do_postfx && postfx->shadows && g_fbo_depth_tex;
    bool do_composite = do_ssao || do_godrays || do_bloom || do_shadows;

    GLuint ao_tex = 0;
    GLuint gr_tex = 0;
    GLuint bloom_tex = 0;

    // ======== PASS 1: SSAO (half-res) ========
    if (do_ssao) {
        // Render SSAO to half_fbo_a
        glBindFramebuffer(GL_FRAMEBUFFER, g_half_fbo_a);
        glViewport(0, 0, g_half_w, g_half_h);
        glUseProgram(g_prog_ssao);
        glUniform1i(loc_ssao_depth, 0);
        glUniform2f(loc_ssao_tex_size, (float)g_half_w, (float)g_half_h);
        glUniform1f(loc_ssao_radius, postfx->ssao_radius);
        glUniform1f(loc_ssao_intensity, postfx->ssao_intensity);
        glUniform1f(loc_ssao_time, g_postfx_time);
        glUniform1f(loc_ssao_near_plane, 50.0f);
        glUniform1f(loc_ssao_far_plane, 20000.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_fbo_depth_tex);
        draw_fsq();

        // Blur SSAO horizontally: half_a -> half_b
        glBindFramebuffer(GL_FRAMEBUFFER, g_half_fbo_b);
        glUseProgram(g_prog_blur);
        glUniform1i(loc_blur_tex, 0);
        glUniform2f(loc_blur_direction, 1.0f / g_half_w, 0.0f);
        glBindTexture(GL_TEXTURE_2D, g_half_tex_a);
        draw_fsq();

        // Blur SSAO vertically: half_b -> half_a
        glBindFramebuffer(GL_FRAMEBUFFER, g_half_fbo_a);
        glUniform2f(loc_blur_direction, 0.0f, 1.0f / g_half_h);
        glBindTexture(GL_TEXTURE_2D, g_half_tex_b);
        draw_fsq();

        ao_tex = g_half_tex_a;
    }

    // ======== PASS 2: God Rays (half-res) ========
    if (do_godrays) {
        GLuint target_fbo = do_ssao ? g_half_fbo_b : g_half_fbo_a;
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, g_half_w, g_half_h);
        glUseProgram(g_prog_godrays);
        glUniform1i(loc_godrays_tex, 0);
        glUniform1i(loc_godrays_depth, 1);
        glUniform2f(loc_godrays_sun_pos, postfx->sun_x, postfx->sun_y);
        float intensity = postfx->god_rays ? postfx->god_rays_intensity : postfx->volumetric_intensity;
        glUniform1f(loc_godrays_intensity, intensity);
        glUniform1f(loc_godrays_decay, postfx->god_rays_decay);
        glUniform1f(loc_godrays_density, 1.0f);
        glUniform1f(loc_godrays_exposure, 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_fbo_depth_tex);
        draw_fsq();
        glActiveTexture(GL_TEXTURE0);

        gr_tex = do_ssao ? g_half_tex_b : g_half_tex_a;
    }

    // ======== PASS 2b: Bloom (quarter-res) ========
    if (do_bloom) {
        // Extract bright pixels: scene -> bloom_fbo_a
        glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo_a);
        glViewport(0, 0, g_quarter_w, g_quarter_h);
        glUseProgram(g_prog_bloom_extract);
        glUniform1i(loc_bloom_extract_tex, 0);
        glUniform1f(loc_bloom_extract_threshold, postfx->bloom_threshold);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
        draw_fsq();

        // Blur horizontal: bloom_a -> bloom_b
        glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo_b);
        glUseProgram(g_prog_blur);
        glUniform1i(loc_blur_tex, 0);
        glUniform2f(loc_blur_direction, 1.0f / g_quarter_w, 0.0f);
        glBindTexture(GL_TEXTURE_2D, g_bloom_tex_a);
        draw_fsq();

        // Blur vertical: bloom_b -> bloom_a
        glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo_a);
        glUniform2f(loc_blur_direction, 0.0f, 1.0f / g_quarter_h);
        glBindTexture(GL_TEXTURE_2D, g_bloom_tex_b);
        draw_fsq();

        bloom_tex = g_bloom_tex_a;
    }

    // ======== PASS 2.5: Portal Effect Quad (into game FBO) ========
    // Portal VP matrix diagnostic — print once every 120 frames to understand coords
    {
        static int portal_diag_count = 0;
        if (g_portal_active > 0.5f && portal_diag_count % 120 == 0) {
            std::cout << "[PORTAL-DIAG] world_pos=(" 
                      << g_portal_world_x << "," << g_portal_world_y << "," << g_portal_world_z 
                      << ") color=(" << g_portal_color_r << "," << g_portal_color_g << "," << g_portal_color_b
                      << ")" << std::endl;
            // Show the computed MVP column 3 (should be view-space position)
            std::cout << "[PORTAL-DIAG] MVP last_col=(" 
                      << g_portal_vp_matrix[3] << "," << g_portal_vp_matrix[7] 
                      << "," << g_portal_vp_matrix[11] << "," << g_portal_vp_matrix[15] << ")" << std::endl;
        }
        if (g_portal_active > 0.5f) portal_diag_count++;
    }
    // PORTAL ENABLED — uses model-view matrix from game + our perspective projection
    if (g_portal_active > 0.5f && g_prog_portal) {
        // Load portal texture on first use
        if (!g_portal_tex_loaded) {
            const char* paths[] = {
                "/home/quantumcreeper/.local/share/swordigo-desktop/assets/resources/portal_effect_2x.pvr",
                "assets/resources/portal_effect_2x.pvr",
                nullptr
            };
            for (int i = 0; paths[i]; i++) {
                FILE* f = fopen(paths[i], "rb");
                if (f) { fclose(f); 
                    g_portal_tex = pvr_load_texture(paths[i]);
                    if (g_portal_tex) {
                        std::cout << "[PORTAL] Loaded portal texture from " << paths[i] << std::endl;
                    }
                    break;
                }
            }
            g_portal_tex_loaded = true;
        }
        
        if (g_portal_tex) {
            // Render into the game FBO WITH depth testing — portal renders
            // behind the hero and other scene objects using the game's depth buffer
            glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
            glViewport(0, 0, g_game_w, g_game_h);
            
            // Enable depth testing (read-only) — portal is occluded by closer objects
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_FALSE);  // don't write to depth buffer
            
            // Alpha blending for the portal effect
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            
            glUseProgram(g_prog_portal);
            
            // Bind portal texture
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_portal_tex);
            glUniform1i(loc_portal_portal_tex, 0);
            
            // Pass the model-view matrix from the game's Draw call
            glUniformMatrix4fv(loc_portal_mv_matrix,
                               1, GL_FALSE, g_portal_vp_matrix);
            
            // Aspect ratio for perspective projection
            float aspect = (float)g_game_w / (float)g_game_h;
            glUniform1f(loc_portal_aspect, aspect);
            
            // Portal color and animation
            glUniform3f(loc_portal_color,
                        g_portal_color_r, g_portal_color_g, g_portal_color_b);
            glUniform1f(loc_portal_time, g_postfx_time);
            glUniform1f(loc_portal_speed, g_portal_speed);
            glUniform1f(loc_portal_alpha, g_portal_intensity);
            
            draw_fsq();  // Quad vertices at -1..1, vertex shader transforms via MVP
            
            // Restore GL state
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }

    // ======== PASS 3: Composite (scene * AO + god rays + bloom) at full-res ========
    if (do_composite) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_postfx_fbo);
        glViewport(0, 0, g_game_w, g_game_h);
        glUseProgram(g_prog_composite);
        // Bind textures to different units
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_fbo_tex);  // scene
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ao_tex ? ao_tex : g_fbo_tex);  // AO (or scene as dummy)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gr_tex ? gr_tex : g_fbo_tex);  // god rays (or dummy)
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, bloom_tex ? bloom_tex : g_fbo_tex);  // bloom (or dummy)
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, g_fbo_depth_tex);  // depth for shadows
        glActiveTexture(GL_TEXTURE0);

        glUniform1i(loc_comp_scene, 0);
        glUniform1i(loc_comp_ao, 1);
        glUniform1i(loc_comp_godrays, 2);
        glUniform1i(loc_comp_bloom, 3);
        glUniform1i(loc_comp_depth, 4);
        glUniform1f(loc_comp_ao_enabled, ao_tex ? 1.0f : 0.0f);
        glUniform1f(loc_comp_gr_enabled, gr_tex ? 1.0f : 0.0f);
        glUniform1f(loc_comp_bloom_enabled, bloom_tex ? 1.0f : 0.0f);
        glUniform1f(loc_comp_bloom_intensity, postfx ? postfx->bloom_intensity : 0.0f);
        // Shadow uniforms
        glUniform1f(loc_comp_shadow_enabled, do_shadows ? 1.0f : 0.0f);
        glUniform1f(loc_comp_shadow_intensity, postfx ? postfx->shadow_intensity : 0.0f);
        glUniform1f(loc_comp_shadow_softness, postfx ? postfx->shadow_softness : 0.003f);
        glUniform2f(loc_comp_shadow_light_dir,
                    postfx ? postfx->shadow_light_x : 0.3f,
                    postfx ? postfx->shadow_light_y : -0.8f);
        glUniform1f(loc_comp_shadow_near, 50.0f);
        glUniform1f(loc_comp_shadow_far, 20000.0f);
        // Warm golden tint for god rays
        glUniform1f(loc_comp_gr_tint_r, 1.0f);
        glUniform1f(loc_comp_gr_tint_g, 0.9f);
        glUniform1f(loc_comp_gr_tint_b, 0.7f);
        // Background-aware atmosphere from SRE
        glUniform1f(loc_comp_shadow_lift, 0.15f);  // subtle lift — keep SSAO dark
        glUniform1f(loc_comp_bg_depth, g_bg_depth);
        glUniform1f(loc_comp_bg_scale, g_bg_scale);
        // Warm golden ambient from sky
        glUniform1f(loc_comp_ambient_r, g_bg_ambient_r);
        glUniform1f(loc_comp_ambient_g, g_bg_ambient_g);
        glUniform1f(loc_comp_ambient_b, g_bg_ambient_b);
        draw_fsq();

        final_tex = g_postfx_tex;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, win_w, win_h);
    }

    // ======== PASS 4: Color PostFX (Tier 1 effects) ========
    if (do_postfx) {
        bool need_color_fx = postfx->vignette || postfx->film_grain || postfx->chromatic_aberration ||
                             postfx->color_adjust || postfx->sharpen || postfx->outlines;
        if (need_color_fx) {
            GLuint src_tex = final_tex;
            GLuint dst_fbo;
            if (do_composite) {
                // Composite wrote to g_postfx_fbo (g_postfx_tex).
                // Ping-pong: read from g_postfx_tex, write to g_postfx_fbo_b.
                dst_fbo = g_postfx_fbo_b;
            } else {
                // No composite pass; scene is still in g_fbo_tex.
                // Write color FX to g_postfx_fbo.
                dst_fbo = g_postfx_fbo;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
            glViewport(0, 0, g_game_w, g_game_h);
            glUseProgram(g_prog_postfx);
            glUniform1i(loc_postfx_tex, 0);
            glUniform1i(loc_postfx_depth_tex, 1);
            glUniform2f(loc_postfx_tex_size, (float)g_game_w, (float)g_game_h);
            glUniform1f(loc_postfx_time, g_postfx_time);
            glUniform1f(loc_postfx_vignette, postfx->vignette ? postfx->vignette_intensity : 0.0f);
            glUniform1f(loc_postfx_grain, postfx->film_grain ? postfx->grain_intensity : 0.0f);
            glUniform1f(loc_postfx_ca_offset, postfx->chromatic_aberration ? postfx->ca_offset : 0.0f);
            glUniform1f(loc_postfx_saturation, postfx->color_adjust ? postfx->saturation : 1.0f);
            glUniform1f(loc_postfx_contrast, postfx->color_adjust ? postfx->contrast : 1.0f);
            glUniform1f(loc_postfx_brightness, postfx->color_adjust ? postfx->brightness : 0.0f);
            glUniform1f(loc_postfx_warmth, postfx->color_adjust ? postfx->warmth : 0.0f);
            glUniform1f(loc_postfx_sharpen, postfx->sharpen ? postfx->sharpen_strength : 0.0f);
            // Outline uniforms
            glUniform1f(loc_postfx_outline_enabled, postfx->outlines ? 1.0f : 0.0f);
            glUniform1f(loc_postfx_outline_thickness, postfx->outline_thickness);
            glUniform1f(loc_postfx_outline_intensity, postfx->outline_intensity);
            glUniform1f(loc_postfx_outline_depth_threshold, postfx->outline_depth_threshold);
            glUniform1f(loc_postfx_outline_near, 50.0f);
            glUniform1f(loc_postfx_outline_far, 20000.0f);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src_tex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_fbo_depth_tex);
            glActiveTexture(GL_TEXTURE0);
            draw_fsq();
            // Update final_tex to point at whichever FBO we just wrote to
            final_tex = (dst_fbo == g_postfx_fbo_b) ? g_postfx_tex_b : g_postfx_tex;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, win_w, win_h);
        }
    }

    // ======== PASS 5: Final upscale blit to screen ========
    // Pixel-perfect shortcut
    if (g_game_w == win_w && g_game_h == win_h && final_tex == g_fbo_tex) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, g_game_w, g_game_h, 0, 0, win_w, win_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(old_prog);
        // Restore GL state (Optimization 3)
        if (saved_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (saved_blend)      glEnable(GL_BLEND);      else glDisable(GL_BLEND);
        if (saved_cull_face)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
        if (saved_scissor)    glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (saved_stencil)    glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        if (saved_alpha_test) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
        if (saved_lighting)   glEnable(GL_LIGHTING);   else glDisable(GL_LIGHTING);
        glBlendFunc(saved_blend_src, saved_blend_dst);
        glDepthFunc(saved_depth_func);
        glDepthMask(saved_depth_mask);
        glBindVertexArray(saved_vao);
        glActiveTexture(saved_active_tex);
        glViewport(0, 0, win_w, win_h);
        return;
    }

    GLuint prog = get_program(mode);
    if (!prog) {
        // Fallback blit
        glUseProgram(old_prog);
        // Restore GL state (Optimization 3)
        if (saved_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (saved_blend)      glEnable(GL_BLEND);      else glDisable(GL_BLEND);
        if (saved_cull_face)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
        if (saved_scissor)    glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (saved_stencil)    glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        if (saved_alpha_test) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
        if (saved_lighting)   glEnable(GL_LIGHTING);   else glDisable(GL_LIGHTING);
        glBlendFunc(saved_blend_src, saved_blend_dst);
        glDepthFunc(saved_depth_func);
        glDepthMask(saved_depth_mask);
        glBindVertexArray(saved_vao);
        glActiveTexture(saved_active_tex);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, g_game_w, g_game_h, 0, 0, win_w, win_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        extern int g_sre_viewport_x, g_sre_viewport_y, g_sre_viewport_w, g_sre_viewport_h;
        g_sre_viewport_x = 0;
        g_sre_viewport_y = 0;
        g_sre_viewport_w = win_w;
        g_sre_viewport_h = win_h;
        glViewport(0, 0, win_w, win_h);
        return;
    }

    // Letterbox/pillarbox
    float game_aspect = (float)g_game_w / (float)g_game_h;
    float win_aspect  = (float)win_w / (float)win_h;
    int vx, vy, vw, vh;
    if (win_aspect > game_aspect) {
        vh = win_h; vw = (int)(win_h * game_aspect); vx = (win_w - vw) / 2; vy = 0;
    } else {
        vw = win_w; vh = (int)(win_w / game_aspect); vx = 0; vy = (win_h - vh) / 2;
    }
    extern int g_sre_viewport_x, g_sre_viewport_y, g_sre_viewport_w, g_sre_viewport_h;
    g_sre_viewport_x = vx;
    g_sre_viewport_y = vy;
    g_sre_viewport_w = vw;
    g_sre_viewport_h = vh;
    glViewport(vx, vy, vw, vh);

    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(prog, "u_tex_size"), (float)g_game_w, (float)g_game_h);
    glUniform2f(glGetUniformLocation(prog, "u_out_size"), (float)vw, (float)vh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, final_tex);
    // FSR does its own filtering — use NEAREST to avoid pre-blurring
    GLenum filter = (mode == FBOScale::FSR || mode == FBOScale::NEAREST) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    draw_fsq();

    glUseProgram(old_prog);
    // Restore GL state (Optimization 3)
    if (saved_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (saved_blend)      glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (saved_cull_face)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    if (saved_scissor)    glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (saved_stencil)    glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (saved_alpha_test) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
    if (saved_lighting)   glEnable(GL_LIGHTING);   else glDisable(GL_LIGHTING);
    glBlendFunc(saved_blend_src, saved_blend_dst);
    glDepthFunc(saved_depth_func);
    glDepthMask(saved_depth_mask);
    glBindVertexArray(saved_vao);
    glActiveTexture(saved_active_tex);
    glViewport(0, 0, win_w, win_h);
}

// ======================= PRESETS =======================

void postfx_apply_preset(PostFXState& s, PostFXPreset p) {
    // Reset everything
    s = PostFXState{};
    
    switch (p) {
        case PostFXPreset::OFF:
            s.enabled = true;
            s.shadows = true;
            s.shadow_intensity = 0.35f;
            s.shadow_softness = 0.002f;
            s.shadow_light_x = 0.3f;
            s.shadow_light_y = -0.8f;
            s.color_adjust = true;
            s.warmth = 0.08f;
            s.contrast = 1.03f;
            s.saturation = 1.05f;
            s.brightness = 0.02f;
            s.vignette = true;
            s.vignette_intensity = 0.15f;
            s.preset_name = "Low";
            break;
        case PostFXPreset::SW_PLUS:
            s.enabled = true;
            // SSAO — softer shadows (reduced from 1.5 to 0.8 — shadow lift handles the rest)
            s.ssao = true; s.ssao_radius = 0.025f; s.ssao_intensity = 1.2f;
            // God rays — dramatic light shafts from sky
            s.god_rays = true; s.god_rays_intensity = 0.6f; s.god_rays_decay = 0.96f;
            s.sun_x = 0.65f; s.sun_y = 0.88f;
            // Volumetric light — layered atmosphere
            s.volumetric_light = true; s.volumetric_intensity = 0.3f;
            // Warm vibrant color grading — make it pop!
            s.color_adjust = true; s.warmth = 0.2f; s.contrast = 1.05f; 
            s.saturation = 1.12f; s.brightness = 0.03f;  // brighter overall
            // Vignette — subtle cinematic frame
            s.vignette = true; s.vignette_intensity = 0.2f;
            // Sharpening — crisp edges
            s.sharpen = true; s.sharpen_strength = 0.3f;
            // Subtle chromatic aberration — premium lens feel
            s.chromatic_aberration = true; s.ca_offset = 0.0008f;
            // Bloom for glowing highlights
            s.bloom = true; s.bloom_threshold = 0.65f; s.bloom_intensity = 0.25f;
            // Screen-space shadows — light from upper-right
            s.shadows = true; s.shadow_intensity = 0.45f; s.shadow_softness = 0.003f;
            s.shadow_light_x = 0.3f; s.shadow_light_y = -0.8f;
            // Edge outlines — subtle depth definition
            s.outlines = true; s.outline_thickness = 1.0f; s.outline_intensity = 0.5f;
            s.outline_depth_threshold = 0.002f;
            s.preset_name = "Sw+";
            break;
        case PostFXPreset::ATMOSPHERIC:
            s.enabled = true;
            s.ssao = true; s.ssao_radius = 0.025f; s.ssao_intensity = 1.4f;
            s.volumetric_light = true; s.volumetric_intensity = 0.25f;
            s.sun_x = 0.7f; s.sun_y = 0.85f;
            s.vignette = true; s.vignette_intensity = 0.3f;
            s.color_adjust = true; s.contrast = 1.05f;
            s.bloom = true; s.bloom_threshold = 0.7f; s.bloom_intensity = 0.3f;
            s.shadows = true; s.shadow_intensity = 0.5f; s.shadow_softness = 0.004f;
            s.shadow_light_x = 0.4f; s.shadow_light_y = -0.7f;
            s.preset_name = "Atmospheric";
            break;
        case PostFXPreset::ETHEREAL:
            s.enabled = true;
            s.god_rays = true; s.god_rays_intensity = 0.5f; s.god_rays_decay = 0.95f;
            s.sun_x = 0.5f; s.sun_y = 0.9f;
            s.vignette = true; s.vignette_intensity = 0.2f;
            s.color_adjust = true; s.warmth = 0.4f; s.brightness = 0.03f; s.saturation = 1.1f;
            s.bloom = true; s.bloom_threshold = 0.6f; s.bloom_intensity = 0.35f;
            s.preset_name = "Ethereal";
            break;
        case PostFXPreset::CINEMATIC:
            s.enabled = true;
            s.vignette = true; s.vignette_intensity = 0.4f;
            s.film_grain = true; s.grain_intensity = 0.04f;
            s.color_adjust = true; s.warmth = 0.3f; s.contrast = 1.1f; s.saturation = 1.05f;
            s.bloom = true; s.bloom_threshold = 0.75f; s.bloom_intensity = 0.2f;
            s.preset_name = "Cinematic";
            break;
        case PostFXPreset::RETRO:
            s.enabled = true;
            s.film_grain = true; s.grain_intensity = 0.08f;
            s.color_adjust = true; s.saturation = 1.3f; s.contrast = 1.15f;
            s.chromatic_aberration = true; s.ca_offset = 0.003f;
            s.preset_name = "Retro";
            break;
        case PostFXPreset::FANTASY:
            s.enabled = true;
            s.vignette = true; s.vignette_intensity = 0.25f;
            s.color_adjust = true; s.saturation = 1.2f; s.warmth = 0.2f; s.brightness = 0.02f;
            s.sharpen = true; s.sharpen_strength = 0.3f;
            s.bloom = true; s.bloom_threshold = 0.65f; s.bloom_intensity = 0.3f;
            // Bold outlines — storybook illustration style
            s.outlines = true; s.outline_thickness = 1.5f; s.outline_intensity = 0.7f;
            s.outline_depth_threshold = 0.0015f;
            s.preset_name = "Fantasy";
            break;
        case PostFXPreset::NOIR:
            s.enabled = true;
            s.vignette = true; s.vignette_intensity = 0.6f;
            s.film_grain = true; s.grain_intensity = 0.07f;
            s.color_adjust = true; s.saturation = 0.15f; s.contrast = 1.4f; s.brightness = -0.03f;
            s.preset_name = "Noir";
            break;
        case PostFXPreset::CUSTOM:
            s.enabled = true;
            s.preset_name = "Custom";
            break;
        default: break;
    }
}

const char* postfx_preset_name(PostFXPreset p) {
    switch (p) {
        case PostFXPreset::OFF: return "Low";
        case PostFXPreset::SW_PLUS: return "Sw+";
        case PostFXPreset::ATMOSPHERIC: return "Atmospheric";
        case PostFXPreset::ETHEREAL: return "Ethereal";
        case PostFXPreset::CINEMATIC: return "Cinematic";
        case PostFXPreset::RETRO: return "Retro";
        case PostFXPreset::FANTASY: return "Fantasy";
        case PostFXPreset::NOIR: return "Noir";
        case PostFXPreset::CUSTOM: return "Custom";
        default: return "Unknown";
    }
}

bool fbo_is_active() { return g_fbo_ok; }
unsigned int fbo_get_texture() { return g_fbo_tex; }

// ======== Vanilla Portal Rendering (no FBO pipeline required) ========
// Renders the portal effect directly into whatever framebuffer is currently bound.
// Uses the same shader and MVP matrix approach as the PostFX path.
void fbo_draw_portal_vanilla(int viewport_w, int viewport_h) {
    // Update animation time (g_postfx_time only updates when FBO is active)
    static float vanilla_portal_time = 0.0f;
    vanilla_portal_time += 0.016f;
    if (vanilla_portal_time > 10000.0f) vanilla_portal_time = 0.0f;
    
    if (g_portal_active <= 0.5f) return;
    
    // Compile portal shader on first call if not already done
    if (!g_prog_portal) {
        g_prog_portal = link_program(VERT_PORTAL, FRAG_PORTAL);
        if (g_prog_portal) {
            std::cout << "[PORTAL-VANILLA] Shader compiled OK (program=" << g_prog_portal << ")" << std::endl;
            // Cache uniform locations for lazy-compiled portal shader
            loc_portal_portal_tex = glGetUniformLocation(g_prog_portal, "u_portal_tex");
            loc_portal_mv_matrix  = glGetUniformLocation(g_prog_portal, "u_mv_matrix");
            loc_portal_aspect     = glGetUniformLocation(g_prog_portal, "u_aspect");
            loc_portal_color      = glGetUniformLocation(g_prog_portal, "u_color");
            loc_portal_time       = glGetUniformLocation(g_prog_portal, "u_time");
            loc_portal_speed      = glGetUniformLocation(g_prog_portal, "u_speed");
            loc_portal_alpha      = glGetUniformLocation(g_prog_portal, "u_alpha");
        } else {
            std::cerr << "[PORTAL-VANILLA] Shader FAILED to compile" << std::endl;
            return;
        }
    }
    
    // Load portal texture on first use
    if (!g_portal_tex_loaded) {
        const char* paths[] = {
            "/home/quantumcreeper/.local/share/swordigo-desktop/assets/resources/portal_effect_2x.pvr",
            "assets/resources/portal_effect_2x.pvr",
            nullptr
        };
        for (int i = 0; paths[i]; i++) {
            FILE* f = fopen(paths[i], "rb");
            if (f) { fclose(f);
                g_portal_tex = pvr_load_texture(paths[i]);
                if (g_portal_tex) {
                    std::cout << "[PORTAL-VANILLA] Loaded texture from " << paths[i] << std::endl;
                }
                break;
            }
        }
        g_portal_tex_loaded = true;
    }
    
    if (!g_portal_tex) return;
    
    // Save GL state
    GLboolean blend_was_on;
    glGetBooleanv(GL_BLEND, &blend_was_on);
    GLint old_blend_src, old_blend_dst;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &old_blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &old_blend_dst);
    
    // Set viewport to match the game's rendering area
    glViewport(0, 0, viewport_w, viewport_h);
    
    // Additive blending for the portal glow
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    
    glUseProgram(g_prog_portal);
    
    // Bind portal texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_portal_tex);
    glUniform1i(loc_portal_portal_tex, 0);
    
    // Pass the model-view matrix from the game's Draw call
    glUniformMatrix4fv(loc_portal_mv_matrix,
                       1, GL_FALSE, g_portal_vp_matrix);
    
    // Aspect ratio for perspective projection
    float aspect = (float)viewport_w / (float)viewport_h;
    glUniform1f(loc_portal_aspect, aspect);
    
    // Portal color and animation
    glUniform3f(loc_portal_color,
                g_portal_color_r, g_portal_color_g, g_portal_color_b);
    glUniform1f(loc_portal_time, vanilla_portal_time);
    glUniform1f(loc_portal_speed, g_portal_speed);
    glUniform1f(loc_portal_alpha, g_portal_intensity);
    
    draw_fsq();
    
    // Restore GL state
    if (!blend_was_on) glDisable(GL_BLEND);
    glBlendFunc(old_blend_src, old_blend_dst);
    glUseProgram(0);
}

unsigned int fbo_get_fbo() {
    return g_fbo;
}
