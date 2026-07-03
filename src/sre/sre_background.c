/* sre_background.c — Custom background rendering for libsre.so
 *
 * We OWN BackgroundComponent::Draw now! Instead of the engine's
 * baked-texture renderer, we re-implement it ourselves with
 * full control over the rendering pipeline.
 *
 * From decompiled BackgroundComponent::Draw:
 *   1. Get camera Z position (param_2 + 0x18) and forward direction
 *   2. Calculate projection: background plane at z = -11000
 *   3. Scale sprite to fill screen based on FOV
 *   4. RenderingContext::SetMatrix(ctx, &matrix)
 *   5. Sprite::Draw(this->sprite, ctx)
 *
 * Our version:
 *   - Same core rendering (we keep the original sky sprites)
 *   - But we can add: color tinting, brightness, custom transforms
 *   - And we capture data for the host (biome detection, etc.)
 *
 * Layout (ARM64 v1.4.12):
 *   BackgroundComponent + 0x70: float[2] — direction/scale
 *   BackgroundComponent + 0x78: float   — Z scale
 *   BackgroundComponent + 0x80: Sprite* — the sky texture sprite
 *   Camera + 0x18: float — Z position
 *   Camera + 0xf0: float — aspect ratio  
 *   Camera + 0xf4: float — field of view
 *   Sprite + 0x10: float — width
 *   Sprite + 0x14: float — height
 *   RotatingBG + 0x90: float — rotation angle
 */

#include "sre.h"

/* ========== Engine function pointers for rendering ========== */
typedef void (*pfn_SetMatrix)(void* ctx, void* matrix);
typedef void (*pfn_SetColor)(void* ctx, void* color);
typedef void (*pfn_SpriteDraw)(void* sprite, void* ctx);
typedef void (*pfn_glDepthMask)(int flag);  /* GL_TRUE=1, GL_FALSE=0 */

pfn_SetMatrix   g_RenderingContext_SetMatrix = 0;
pfn_SetColor    g_RenderingContext_SetColor = 0;
pfn_SpriteDraw  g_Sprite_Draw = 0;
pfn_glDepthMask g_glDepthMask = 0;

/* Background state — readable by host for FBO shader integration */
int     g_sre_bg_mode = 0;          /* 0=our renderer (default), 1=NOP/black */
int     g_sre_bg_draw_count = 0;    /* Draws per frame (host resets each frame) */
sre_u64 g_sre_bg_sprite_ptr = 0;    /* Last Sprite* seen */

/* === Color control (host writes, SRE reads) === */
float   g_sre_bg_tint_r = 1.0f;    /* Color tint RGBA */
float   g_sre_bg_tint_g = 1.0f;
float   g_sre_bg_tint_b = 1.0f;
float   g_sre_bg_tint_a = 1.0f;
float   g_sre_bg_brightness = 1.0f; /* Overall brightness multiplier */

/* === Position control (host writes, SRE reads) === */
float   g_sre_bg_parallax_x = 0.1f; /* Extra parallax offset */
float   g_sre_bg_parallax_y = 0.32f; /* Tuned sky height */

/* === Data extraction (SRE writes each frame, host reads for shaders) === */
float   g_sre_bg_sprite_w = 0.0f;  /* Sprite texture width (pixels) */
float   g_sre_bg_sprite_h = 0.0f;  /* Sprite texture height (pixels) */
float   g_sre_bg_cam_z = 0.0f;     /* Camera Z position */
float   g_sre_bg_cam_fov = 0.0f;   /* Camera FOV (radians) */
float   g_sre_bg_cam_aspect = 0.0f;/* Camera aspect ratio */
float   g_sre_bg_scale = 0.0f;     /* Computed uniform scale */
float   g_sre_bg_depth = 0.0f;     /* Distance to BG plane (cam_z + 11000) */
int     g_sre_bg_frame = 0;        /* Frame counter (for host sync) */

/* Init function — host calls this with engine function addresses */
typedef struct {
    sre_u64 setMatrix;
    sre_u64 setColor;
    sre_u64 spriteDraw;
    sre_u64 glDepthMask;  /* PLT entry: 0x1fb820 */
} SreBgAddrs;

void sre_init_background(SreBgAddrs* addrs) {
    g_RenderingContext_SetMatrix = (pfn_SetMatrix)addrs->setMatrix;
    g_RenderingContext_SetColor  = (pfn_SetColor)addrs->setColor;
    g_Sprite_Draw                = (pfn_SpriteDraw)addrs->spriteDraw;
    g_glDepthMask                = (pfn_glDepthMask)addrs->glDepthMask;
}

/* ========== Matrix helpers (no libc!) ========== */

static void mat4_identity(float* m) {
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
}

static void mat4_mul(const float* a, const float* b, float* out) {
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            out[i * 4 + j] = sum;
        }
    }
}

void sre_BackgroundComponent_Draw(void* self, void* ctx, void* camera) {
    g_sre_bg_draw_count++;
    
    /* Mode 1: NOP (black sky for testing) */
    if (g_sre_bg_mode == 1) return;
    
    /* Get the sprite pointer */
    void* sprite = *(void**)((char*)self + 0x80);
    g_sre_bg_sprite_ptr = (sre_u64)sprite;
    
    if (!sprite || !g_RenderingContext_SetMatrix || !g_Sprite_Draw) {
        g_sre_bg_draw_count = -1;
        return;
    }
    
    /* Read camera data */
    float cam_x    = *(float*)((char*)camera + 0x10);
    float cam_y    = *(float*)((char*)camera + 0x14);
    float cam_z    = *(float*)((char*)camera + 0x18);
    float cam_fov  = *(float*)((char*)camera + 0xf4);  /* radians */
    float cam_aspect = *(float*)((char*)camera + 0xf0);

    /* Synchronize global camera coordinates */
    extern volatile float g_sre_cam_x;
    extern volatile float g_sre_cam_y;
    extern volatile float g_sre_cam_z;
    g_sre_cam_x = cam_x;
    g_sre_cam_y = cam_y;
    g_sre_cam_z = cam_z;
    
    /* Read sprite dimensions */
    float spr_w = *(float*)((char*)sprite + 0x10);
    float spr_h = *(float*)((char*)sprite + 0x14);
    
    /* Depth to background plane at z = -11000 */
    float depth = cam_z + 11000.0f;
    if (depth <= 0.0f) return;  /* Camera past background plane */
    
    /* tanf approximation (no libc!) 
     * fov is in radians, typical ~1.0 (57 deg)
     * tan(x) ≈ x + x³/3 + 2x⁵/15  (good for |x| < 1.2) */
    float hf = cam_fov * 0.5f;
    float hf2 = hf * hf;
    float fov_tan = hf + (hf * hf2) * 0.3333f + (hf * hf2 * hf2) * 0.1333f;
    
    /* Scale: how many "world units per pixel" at the background distance */
    float pixel_scale = 0.5f / (depth * fov_tan);
    float scale = pixel_scale * 20.0f * (1024.0f / spr_w) * 1.1f;
    
    /* === Export data for host FBO shaders === */
    g_sre_bg_sprite_w = spr_w;
    g_sre_bg_sprite_h = spr_h;
    g_sre_bg_cam_z = cam_z;
    g_sre_bg_cam_fov = cam_fov;
    g_sre_bg_cam_aspect = cam_aspect;
    g_sre_bg_scale = scale;
    g_sre_bg_depth = depth;
    g_sre_bg_frame++;
    
    /* Apply parallax offset — these are set by host or defaults */
    float offset_x = g_sre_bg_parallax_x;
    float offset_y = g_sre_bg_parallax_y;
    
    /* Loose clamp — just prevent completely off-screen */
    float y_limit = spr_h * scale * 0.5f;
    float x_limit = spr_w * scale * 0.5f;
    
    if (offset_x > x_limit) offset_x = x_limit;
    if (offset_x < -x_limit) offset_x = -x_limit;
    if (offset_y > y_limit) offset_y = y_limit;
    if (offset_y < -y_limit) offset_y = -y_limit;
    
    /* Build COLUMN-MAJOR matrix (OpenGL convention):
     * From decompiled: local_b8 = 0x3f8000003f7d70a4 = {0.99f, 1.0f}
     * This maps to m[14]=0.99 (Z translation), m[15]=1.0 (W)
     * The 0.99 Z pushes the sprite to the back of the depth range.
     */
    float matrix[16] = {
        scale,  0.0f,   0.0f, 0.0f,     /* column 0 */
        0.0f,   scale,  0.0f, 0.0f,     /* column 1 */
        0.0f,   0.0f,   1.0f, 0.0f,     /* column 2 */
        offset_x, offset_y, 0.99f, 1.0f /* column 3: Z=0.99 from original */
    };
    
    /* Apply color tint if brightness changed */
    if (g_RenderingContext_SetColor && g_sre_bg_brightness != 1.0f) {
        float color[4] = {
            g_sre_bg_tint_r * g_sre_bg_brightness,
            g_sre_bg_tint_g * g_sre_bg_brightness,
            g_sre_bg_tint_b * g_sre_bg_brightness,
            g_sre_bg_tint_a
        };
        g_RenderingContext_SetColor(ctx, color);
    }
    
    /* Disable depth writes — background must NOT occlude the 3D scene */
    if (g_glDepthMask) g_glDepthMask(0);  /* GL_FALSE */
    
    g_RenderingContext_SetMatrix(ctx, matrix);
    g_Sprite_Draw(sprite, ctx);
    
    /* Re-enable depth writes for subsequent draws */
    if (g_glDepthMask) g_glDepthMask(1);  /* GL_TRUE */
}

/* ========== RotatingBackgroundComponent::Draw ==========
 * Symbol (const): _ZNK5Caver27RotatingBackgroundComponent4DrawEPNS_16RenderingContextEPNS_6CameraE
 * Offset: 0x2b6760
 *
 * Similar to BackgroundComponent but adds Z-rotation from this + 0x90
 */
void sre_RotatingBackgroundComponent_Draw(void* self, void* ctx, void* camera) {
    if (g_sre_bg_mode == 1) return;
    
    /* For now, delegate to the same logic as BackgroundComponent
     * TODO: Add rotation from this + 0x90 */
    sre_BackgroundComponent_Draw(self, ctx, camera);
}

/* ========== RotatingBackgroundComponent::Update ==========
 * Symbol: _ZN5Caver27RotatingBackgroundComponent6UpdateEf
 * Offset: 0x2b66f8
 *
 * Original: updates rotation angle at this + 0x90
 * We keep the rotation going (or customize the speed)
 */
void sre_RotatingBackgroundComponent_Update(void* self, float dt) {
    /* Update the rotation angle (original behavior) */
    float* angle = (float*)((char*)self + 0x90);
    *angle += dt * 0.02f;  /* Slow cloud rotation */
}
