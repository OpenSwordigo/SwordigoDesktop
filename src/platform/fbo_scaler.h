// ============================================================
//  SwordigoDesktop — FBO + GLSL Rendering Pipeline
//  Game renders to FBO, then through optional post-processing
//  passes (SSAO, God Rays, Color FX), then upscaled to window.
// ============================================================

#pragma once
#include <cstdint>

// Upscale filter modes
enum class FBOScale : int {
    SHARP_BILINEAR = 0,
    NEAREST        = 1,
    CRT_SCANLINE   = 2,
    FSR            = 3,   // AMD FidelityFX Super Resolution 1.0 (EASU)
};

// Post-processing effects state
struct PostFXState {
    bool enabled = false;
    
    // --- Tier 1: Color effects ---
    bool vignette = false;
    float vignette_intensity = 0.3f;
    
    bool film_grain = false;
    float grain_intensity = 0.06f;
    
    bool chromatic_aberration = false;
    float ca_offset = 0.002f;
    
    bool color_adjust = false;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float brightness = 0.0f;
    float warmth = 0.0f;
    
    bool sharpen = false;
    float sharpen_strength = 0.4f;
    
    // --- Tier 2: Advanced effects ---
    bool god_rays = false;
    float god_rays_intensity = 0.4f;    // 0.0-1.0
    float god_rays_decay = 0.96f;       // per-sample falloff
    float sun_x = 0.5f;                 // sun position in UV (0-1)
    float sun_y = 0.85f;                // top of screen
    
    bool ssao = false;
    float ssao_radius = 0.02f;          // sample radius in UV
    float ssao_intensity = 1.2f;        // darkening strength
    
    bool volumetric_light = false;
    float volumetric_intensity = 0.3f;
    
    bool bloom = false;
    float bloom_threshold = 0.7f;       // brightness threshold for bloom extraction
    float bloom_intensity = 0.35f;      // additive bloom strength
    
    bool shadows = false;
    float shadow_intensity = 0.5f;       // shadow darkness (0=none, 1=pitch black)
    float shadow_softness = 0.003f;      // blur radius for soft shadow edges
    float shadow_light_x = 0.3f;         // light direction X (+right, normalized)
    float shadow_light_y = -0.8f;        // light direction Y (-down, normalized)
    float shadow_sun_z = 0.15f;          // virtual sun depth component for 3D shadow angle
                                         // >0 = player/camera side (shadows angle toward player)
                                         // 0  = purely 2D screen-space shadows
                                         // <0 = background side (shadows angle away from player)
    
    bool outlines = false;               // depth-based edge outlines (cel-shading style)
    float outline_thickness = 1.0f;      // pixel width of outline (1.0 = 1px, 2.0 = thicker)
    float outline_intensity = 0.7f;      // outline opacity (0-1)
    float outline_depth_threshold = 0.002f; // depth difference to detect an edge
    
    // Preset name (for display)
    const char* preset_name = "Low";
};

enum class PostFXPreset : int {
    OFF = 0,
    SW_PLUS,        // Swordigo Plus — enhanced atmospheric with god rays
    ATMOSPHERIC,    // SSAO + volumetric + vignette
    ETHEREAL,       // God rays + warm glow
    CINEMATIC,
    RETRO,
    FANTASY,
    NOIR,
    CUSTOM,
    COUNT
};

void postfx_apply_preset(PostFXState& state, PostFXPreset preset);
const char* postfx_preset_name(PostFXPreset p);

bool postfx_load_custom_json(PostFXState& state);
void postfx_save_default_json();

bool fbo_init(int game_w, int game_h);
void fbo_destroy();
void fbo_begin_game();
void fbo_end_game_and_blit(int win_w, int win_h, FBOScale mode = FBOScale::SHARP_BILINEAR, const PostFXState* postfx = nullptr);
unsigned int fbo_get_texture();
unsigned int fbo_get_fbo();
bool fbo_is_active();

// Render portal effect directly into the current framebuffer (for non-PostFX / vanilla mode).
// Call AFTER drawApplication() and BEFORE GUI overlay.
// Works with or without the FBO pipeline active.
void fbo_draw_portal_vanilla(int viewport_w, int viewport_h);
