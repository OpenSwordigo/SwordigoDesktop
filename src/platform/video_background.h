#ifndef VIDEO_BACKGROUND_H
#define VIDEO_BACKGROUND_H

#include <stdint.h>
#include <string>

// Global void-fill colour: set by VideoBackground each frame from sampled edge pixels.
// bridge_gl_clear_color reads this and overrides whatever the game requests.
// Starts as a safe sky-blue so there's never raw black even before any background loads.
struct VoidFillColor {
    float r = 0.17f, g = 0.35f, b = 0.60f;  // default: sky blue
    bool  active = false;                      // set true once a background has been sampled
};
extern VoidFillColor g_void_fill_color;
extern bool g_video_background_enabled;

#ifdef __cplusplus
extern "C" {
#endif
void reset_void_fill_color(void);
#ifdef __cplusplus
}
#endif

namespace VideoBackground {
    void register_texture_maybe(uint32_t tex_id, const std::string& asset_filename, int width, int height);
    void update_texture_maybe(uint32_t tex_id);
    void cleanup();
}

#endif // VIDEO_BACKGROUND_H
