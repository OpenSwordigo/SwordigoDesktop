// ============================================================
//  gl_render_state.h
//  Live GL state extracted from the game's own GL calls via the
//  ARM64 JNI bridge.  Written by bridge hooks every frame;
//  consumed by fbo_scaler.cpp at composite / PostFX time.
//
//  This is the "handshake" layer that gives the FBO post-processing
//  pipeline accurate, per-frame knowledge of what the game engine is
//  actually doing — light positions, material colors, matrices, and
//  the current vertex/model color — so shaders can go beyond static
//  PostFX presets and react to the actual scene state.
// ============================================================
#pragma once

// ── Per-light state (GL_LIGHT0..GL_LIGHT7) ─────────────────────────────────
struct GuestLightState {
    float position[4];  // GL_POSITION (world space, w=0 directional, w=1 point)
    float ambient[4];   // GL_AMBIENT
    float diffuse[4];   // GL_DIFFUSE
    float specular[4];  // GL_SPECULAR
    bool  enabled;      // set by glEnable/glDisable(GL_LIGHT0+i)
};

// ── Material state (last glMaterialfv call) ────────────────────────────────
struct GuestMaterialState {
    float ambient[4];
    float diffuse[4];   // ← most important: drives model tint / boss color
    float specular[4];
    float emission[4];
    float shininess;
};

#ifdef __cplusplus
extern "C" {
#endif

// Eight shadow lights matching GL_LIGHT0..GL_LIGHT7
extern GuestLightState g_frame_lights[8];

// GL_LIGHT_MODEL_AMBIENT — global ambient (from glLightModelfv)
extern float g_frame_light_model_ambient[4];

// Current material properties
extern GuestMaterialState g_frame_material;

// Current GL matrix shadows (column-major, same layout as OpenGL)
extern float g_current_modelview[16];
extern float g_current_projection[16];

// Current RGBA vertex/model color (normalised 0..1)
extern float g_current_color[4];

// Host-side cached hero position (synchronized from guest memory each frame)
#if defined(_MSC_VER)
// MSVC names namespace-scope array objects with a C (undecorated) symbol;
// the definition in jni_bridge_arm64.cpp emits the same plain name, so the
// declaration must use C linkage on Windows to match.
extern "C" float g_host_hero_pos[3];
#else
extern float g_host_hero_pos[3];
#endif

#ifdef __cplusplus
}
#endif
