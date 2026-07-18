/* =====================================================================
 * sre_gui.c — SRE GUI Rendering Subsystem
 * =====================================================================
 * Full access to the Caver engine's GUI stack:
 *   - Immediate-mode drawing (FillRect, DrawRect, sprites)
 *   - Text rendering (FontText)
 *   - View creation (GameInterfaceBuilder → GUILabel, GUIButton, etc.)
 *   - View hierarchy (AddSubview, SetFrame, RemoveFromSuperview)
 *   - Modal presentation (GUIWindow::PresentModalView)
 *   - Texture access (TextureLibrary)
 *   - Font access (FontLibrary)
 *
 * All functions use the engine's own C++ methods via function pointers
 * resolved from nm symbols at init time.
 * ===================================================================== */

#include "sre.h"
#include "sre_gui.h"

/* libc from bridge */
extern void* malloc(size_t size);
extern void  free(void* ptr);
extern void* memcpy(void* dest, const void* src, size_t n);
extern size_t strlen(const char* s);

/* ========== Function pointers from sre_background.c ========== */
extern pfn_SetMatrix   g_RenderingContext_SetMatrix;
extern pfn_SetColor    g_RenderingContext_SetColor;
extern pfn_SpriteDraw  g_Sprite_Draw;

/* CppString constructor from sre_string.c */
extern SreString* sre_CppString_from_char_p(SreString* self, const char* src);
extern void sre_CppString_release(SreString* self);

/* ========== RenderingContext API ========== */
static pfn_SetProjectionMatrix    g_SetProjectionMatrix = 0;
static pfn_FillRect               g_FillRect = 0;
static pfn_DrawRect_RC            g_DrawRect = 0;
static pfn_SetBlendingEnabled     g_SetBlendingEnabled = 0;
static pfn_SetTexturingEnabled    g_SetTexturingEnabled = 0;
static pfn_SetLightingEnabled     g_SetLightingEnabled = 0;
static pfn_SetDepthTestEnabled    g_SetDepthTestEnabled = 0;
static pfn_SetDepthWriteEnabled   g_SetDepthWriteEnabled = 0;
pfn_SetAlpha               g_sre_SetAlpha = 0;  /* non-static: used by sre_gui_native.c */
static pfn_BindTexture            g_BindTexture = 0;
static pfn_DrawArrays             g_DrawArrays = 0;
static pfn_UseProgram             g_UseProgram = 0;
static pfn_SetVertexAttribPointer g_SetVertexAttribPointer = 0;

/* ========== FontText ========== */
pfn_FontText_Draw   g_sre_FontText_Draw = 0;  /* non-static: used by sre_gui_native.c */
pfn_FontText_AddText   g_sre_FontText_AddText = 0;   /* non-static: Phase 2 text interception */
pfn_FontText_Clear     g_sre_FontText_Clear = 0;     /* non-static: Phase 2 text interception */
pfn_FontText_SetColor  g_sre_FontText_SetColor = 0;  /* non-static: Phase 2 text interception */
static pfn_FontText_Translate g_FontText_Translate = 0;
static pfn_FontText_AlignH    g_FontText_AlignH = 0;

/* ========== GUILabel ========== */
static pfn_GUILabel_AddText      g_GUILabel_AddText = 0;
static pfn_GUILabel_AddTextColor g_GUILabel_AddTextColor = 0;
pfn_GUILabel_UpdateText   g_sre_GUILabel_UpdateText = 0;  /* non-static: Phase 2 */

/* ========== GUIButton ========== */
pfn_GUIButton_Ctor       g_sre_GUIButton_Ctor = 0;        /* non-static: Phase 2 */
pfn_GUIButton_SetTitle   g_sre_GUIButton_SetTitle = 0;     /* non-static: Phase 2 */
static pfn_GUIButton_SetImage   g_GUIButton_SetImage = 0;
static pfn_GUIButton_titleLabel g_GUIButton_titleLabel = 0;

/* ========== GUIView ========== */
pfn_GUIView_SetFrame           g_sre_GUIView_SetFrame = 0;     /* non-static: Phase 2 */
pfn_GUIView_AddSubview         g_sre_GUIView_AddSubview = 0;   /* non-static: Phase 2 */
static pfn_GUIView_RemoveFromSuperview g_GUIView_RemoveFromSV = 0;

/* ========== GUIWindow ========== */
pfn_GUIWindow_mainWindow   g_sre_GUIWindow_mainWindow = 0;    /* non-static: Phase 2 */
pfn_GUIWindow_PresentModal g_sre_GUIWindow_PresentModal = 0;  /* non-static: Phase 2 */
pfn_GUIWindow_DismissModal g_sre_GUIWindow_DismissModal = 0;  /* non-static: Phase 2 */
static pfn_GUIWindow_Dismiss      g_GUIWindow_Dismiss = 0;

/* ========== GUIAlertView ========== */
pfn_GUIAlertView_SetTitle   g_sre_GUIAlertView_SetTitle = 0;   /* non-static: Phase 2 */
pfn_GUIAlertView_SetMessage g_sre_GUIAlertView_SetMessage = 0; /* non-static: Phase 2 */
pfn_GUIAlertView_AddButton  g_sre_GUIAlertView_AddButton = 0;  /* non-static: Phase 2 */

/* ========== GameInterfaceBuilder ========== */
static pfn_GIB_NormalLabel     g_GIB_NormalLabel = 0;
static pfn_GIB_SmallLabel      g_GIB_SmallLabel = 0;
pfn_GIB_FramedButton    g_sre_GIB_FramedButton = 0;     /* non-static: Phase 2 */
pfn_GIB_MainMenuButton  g_sre_GIB_MainMenuButton = 0;   /* non-static: Phase 2 */
pfn_GIB_AlertView       g_sre_GIB_AlertView = 0;        /* non-static: Phase 2 */
static pfn_GIB_Slider          g_GIB_Slider = 0;
static pfn_GIB_Switch          g_GIB_Switch = 0;

/* ========== TextureLibrary ========== */
static pfn_TextureLibrary_shared  g_TexLib_shared = 0;
static pfn_TextureLibrary_ForName g_TexLib_ForName = 0;

/* ========== FontLibrary ========== */
static pfn_FontLibrary_shared      g_FontLib_shared = 0;
static pfn_FontLibrary_DefaultFont g_FontLib_DefaultFont = 0;
static pfn_FontLibrary_SmallFont   g_FontLib_SmallFont = 0;

/* ========== State ========== */
int   g_sre_gui_initialized = 0;
int   g_sre_gui_overlay_enabled = 0;
float g_sre_gui_screen_w = 960.0f;
float g_sre_gui_screen_h = 540.0f;

/* ========== Init ========== */

void sre_init_gui(SreGuiAddrs* addrs) {
    /* RenderingContext */
    g_SetProjectionMatrix    = (pfn_SetProjectionMatrix)addrs->SetProjectionMatrix;
    g_FillRect               = (pfn_FillRect)addrs->FillRect;
    g_DrawRect               = (pfn_DrawRect_RC)addrs->DrawRect;
    g_SetBlendingEnabled     = (pfn_SetBlendingEnabled)addrs->SetBlendingEnabled;
    g_SetTexturingEnabled    = (pfn_SetTexturingEnabled)addrs->SetTexturingEnabled;
    g_SetLightingEnabled     = (pfn_SetLightingEnabled)addrs->SetLightingEnabled;
    g_SetDepthTestEnabled    = (pfn_SetDepthTestEnabled)addrs->SetDepthTestEnabled;
    g_SetDepthWriteEnabled   = (pfn_SetDepthWriteEnabled)addrs->SetDepthWriteEnabled;
    g_sre_SetAlpha           = (pfn_SetAlpha)addrs->SetAlpha;
    g_BindTexture            = (pfn_BindTexture)addrs->BindTexture;
    g_DrawArrays             = (pfn_DrawArrays)addrs->DrawArrays;
    g_UseProgram             = (pfn_UseProgram)addrs->UseProgram;
    g_SetVertexAttribPointer = (pfn_SetVertexAttribPointer)addrs->SetVertexAttribPointer;

    /* FontText */
    g_sre_FontText_Draw  = (pfn_FontText_Draw)addrs->FontText_Draw;
    g_sre_FontText_AddText = (pfn_FontText_AddText)addrs->FontText_AddText;
    g_sre_FontText_Clear   = (pfn_FontText_Clear)addrs->FontText_Clear;
    g_sre_FontText_SetColor = (pfn_FontText_SetColor)addrs->FontText_SetColor;
    g_FontText_Translate = (pfn_FontText_Translate)addrs->FontText_Translate;
    g_FontText_AlignH    = (pfn_FontText_AlignH)addrs->FontText_AlignH;

    /* GUILabel */
    g_GUILabel_AddText      = (pfn_GUILabel_AddText)addrs->GUILabel_AddText;
    g_GUILabel_AddTextColor = (pfn_GUILabel_AddTextColor)addrs->GUILabel_AddTextColor;
    g_sre_GUILabel_UpdateText = (pfn_GUILabel_UpdateText)addrs->GUILabel_UpdateText;

    /* GUIButton */
    g_sre_GUIButton_Ctor   = (pfn_GUIButton_Ctor)addrs->GUIButton_Ctor;
    g_sre_GUIButton_SetTitle = (pfn_GUIButton_SetTitle)addrs->GUIButton_SetTitle;
    g_GUIButton_SetImage   = (pfn_GUIButton_SetImage)addrs->GUIButton_SetImage;
    g_GUIButton_titleLabel = (pfn_GUIButton_titleLabel)addrs->GUIButton_titleLabel;

    /* GUIView */
    g_sre_GUIView_SetFrame  = (pfn_GUIView_SetFrame)addrs->GUIView_SetFrame;
    g_sre_GUIView_AddSubview = (pfn_GUIView_AddSubview)addrs->GUIView_AddSubview;
    g_GUIView_RemoveFromSV  = (pfn_GUIView_RemoveFromSuperview)addrs->GUIView_RemoveFromSV;

    /* GUIWindow */
    g_sre_GUIWindow_mainWindow   = (pfn_GUIWindow_mainWindow)addrs->GUIWindow_mainWindow;
    g_sre_GUIWindow_PresentModal = (pfn_GUIWindow_PresentModal)addrs->GUIWindow_PresentModal;
    g_sre_GUIWindow_DismissModal = (pfn_GUIWindow_DismissModal)addrs->GUIWindow_DismissModal;
    g_GUIWindow_Dismiss      = (pfn_GUIWindow_Dismiss)addrs->GUIWindow_Dismiss;

    /* GUIAlertView */
    g_sre_GUIAlertView_SetTitle   = (pfn_GUIAlertView_SetTitle)addrs->GUIAlertView_SetTitle;
    g_sre_GUIAlertView_SetMessage = (pfn_GUIAlertView_SetMessage)addrs->GUIAlertView_SetMessage;
    g_sre_GUIAlertView_AddButton  = (pfn_GUIAlertView_AddButton)addrs->GUIAlertView_AddButton;

    /* GameInterfaceBuilder */
    g_GIB_NormalLabel    = (pfn_GIB_NormalLabel)addrs->GIB_NormalLabel;
    g_GIB_SmallLabel     = (pfn_GIB_SmallLabel)addrs->GIB_SmallLabel;
    g_sre_GIB_FramedButton   = (pfn_GIB_FramedButton)addrs->GIB_FramedButton;
    g_sre_GIB_MainMenuButton = (pfn_GIB_MainMenuButton)addrs->GIB_MainMenuButton;
    g_sre_GIB_AlertView      = (pfn_GIB_AlertView)addrs->GIB_AlertView;
    g_GIB_Slider         = (pfn_GIB_Slider)addrs->GIB_Slider;
    g_GIB_Switch         = (pfn_GIB_Switch)addrs->GIB_Switch;

    /* TextureLibrary */
    g_TexLib_shared  = (pfn_TextureLibrary_shared)addrs->TexLib_shared;
    g_TexLib_ForName = (pfn_TextureLibrary_ForName)addrs->TexLib_ForName;

    /* FontLibrary */
    g_FontLib_shared      = (pfn_FontLibrary_shared)addrs->FontLib_shared;
    g_FontLib_DefaultFont = (pfn_FontLibrary_DefaultFont)addrs->FontLib_DefaultFont;
    g_FontLib_SmallFont   = (pfn_FontLibrary_SmallFont)addrs->FontLib_SmallFont;

    g_sre_gui_initialized = 1;
}

/* ========== Matrix helpers ========== */

static void mat4_ortho(float* m, float left, float right,
                        float bottom, float top,
                        float near_val, float far_val) {
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = 2.0f / (right - left);
    m[5]  = 2.0f / (top - bottom);
    m[10] = -2.0f / (far_val - near_val);
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[14] = -(far_val + near_val) / (far_val - near_val);
    m[15] = 1.0f;
}

static void mat4_identity(float* m) {
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
}

/* ========== CppString helpers ========== */

void sre_gui_make_string(SreString* out, const char* text) {
    sre_CppString_from_char_p(out, text);
}

void sre_gui_free_string(SreString* str) {
    sre_CppString_release(str);
}

/* ========== 2D Overlay Projection ========== */

void sre_gui_begin_overlay(void* ctx, float screenW, float screenH) {
    if (!g_SetProjectionMatrix || !g_RenderingContext_SetMatrix) return;

    float ortho[16];
    mat4_ortho(ortho, 0.0f, screenW, screenH, 0.0f, -1.0f, 1.0f);
    g_SetProjectionMatrix(ctx, ortho);

    float ident[16];
    mat4_identity(ident);
    g_RenderingContext_SetMatrix(ctx, ident);

    if (g_SetBlendingEnabled)  g_SetBlendingEnabled(ctx, 1);
    if (g_SetTexturingEnabled) g_SetTexturingEnabled(ctx, 0);
    if (g_SetLightingEnabled)  g_SetLightingEnabled(ctx, 0);
    if (g_SetDepthTestEnabled) g_SetDepthTestEnabled(ctx, 0);
    if (g_UseProgram) g_UseProgram(ctx, 0);
}

void sre_gui_end_overlay(void* ctx) {
    (void)ctx;
}

/* ========== Immediate-Mode Drawing ========== */

void sre_gui_fill_rect(void* ctx, float x, float y, float w, float h,
                        int r, int g, int b, int a) {
    if (!g_FillRect) return;
    SreRect rect = { x, y, w, h };
    SreColor color = { (float)r/255.0f, (float)g/255.0f,
                        (float)b/255.0f, (float)a/255.0f };
    g_FillRect(ctx, &rect, &color, color.a);
}

void sre_gui_draw_rect(void* ctx, float x, float y, float w, float h,
                        int r, int g, int b, int a) {
    if (!g_DrawRect) return;
    SreRect rect = { x, y, w, h };
    SreColor color = { (float)r/255.0f, (float)g/255.0f,
                        (float)b/255.0f, (float)a/255.0f };
    g_DrawRect(ctx, &rect, &color, color.a);
}

void sre_gui_draw_sprite(void* ctx, void* sprite, float x, float y) {
    if (!g_Sprite_Draw || !g_RenderingContext_SetMatrix || !sprite) return;
    float mat[16];
    mat4_identity(mat);
    mat[12] = x;
    mat[13] = y;
    g_RenderingContext_SetMatrix(ctx, mat);
    if (g_SetTexturingEnabled) g_SetTexturingEnabled(ctx, 1);
    g_Sprite_Draw(sprite, ctx);
    mat4_identity(mat);
    g_RenderingContext_SetMatrix(ctx, mat);
}

/* ========== Texture Access ========== */

void* sre_gui_get_texture(const char* name) {
    if (!g_TexLib_shared || !g_TexLib_ForName) return 0;
    void* lib = g_TexLib_shared();
    if (!lib) return 0;
    
    SreString str;
    sre_gui_make_string(&str, name);
    
    /* TextureForName returns intrusive_ptr<Texture> via hidden first arg */
    sre_u64 result[2] = {0, 0};  /* intrusive_ptr is just a raw pointer */
    g_TexLib_ForName(result, lib, &str, 0);
    
    sre_gui_free_string(&str);
    return (void*)result[0];
}

/* ========== GameInterfaceBuilder Wrappers ========== */

void sre_gui_create_label(SreSharedPtr* out, const char* text,
                           int r, int g, int b, int a) {
    if (!g_GIB_NormalLabel) { out->ptr = 0; out->shared_count = 0; return; }
    
    SreString str;
    sre_gui_make_string(&str, text);
    
    SreColor color = { (float)r/255.0f, (float)g/255.0f,
                        (float)b/255.0f, (float)a/255.0f };
    SreColor shadow = { 0.0f, 0.0f, 0.0f, 0.5f };
    
    g_GIB_NormalLabel(out, &str, &color, &shadow);
    
    sre_gui_free_string(&str);
}

void sre_gui_create_button(SreSharedPtr* out, const char* title) {
    if (!g_sre_GIB_MainMenuButton) { out->ptr = 0; out->shared_count = 0; return; }
    
    SreString str;
    sre_gui_make_string(&str, title);
    
    g_sre_GIB_MainMenuButton(out, &str);
    
    sre_gui_free_string(&str);
}

void sre_gui_create_framed_button(SreSharedPtr* out, const char* title) {
    if (!g_sre_GIB_FramedButton) { out->ptr = 0; out->shared_count = 0; return; }
    
    SreString str;
    sre_gui_make_string(&str, title);
    
    g_sre_GIB_FramedButton(out, &str, 1);
    
    sre_gui_free_string(&str);
}

/* ========== GUIView Manipulation ========== */

void sre_gui_set_frame(void* view, float x, float y, float w, float h) {
    if (!g_sre_GUIView_SetFrame || !view) return;
    SreRect rect = { x, y, w, h };
    g_sre_GUIView_SetFrame(view, &rect);
}

void sre_gui_add_subview(void* parent, SreSharedPtr* child) {
    if (!g_sre_GUIView_AddSubview || !parent || !child || !child->ptr) return;
    g_sre_GUIView_AddSubview(parent, child);
}

void sre_gui_remove_from_superview(void* view) {
    if (!g_GUIView_RemoveFromSV || !view) return;
    g_GUIView_RemoveFromSV(view);
}

/* ========== GUIWindow Access ========== */

void* sre_gui_main_window(void) {
    if (!g_sre_GUIWindow_mainWindow) return 0;
    return g_sre_GUIWindow_mainWindow();
}

void sre_gui_present_modal(SreSharedPtr* view, int animated) {
    if (!g_sre_GUIWindow_PresentModal || !g_sre_GUIWindow_mainWindow) return;
    void* win = g_sre_GUIWindow_mainWindow();
    if (!win || !view || !view->ptr) return;
    g_sre_GUIWindow_PresentModal(win, view, animated);
}

void sre_gui_dismiss_modal(void* view) {
    if (!g_sre_GUIWindow_DismissModal || !g_sre_GUIWindow_mainWindow) return;
    void* win = g_sre_GUIWindow_mainWindow();
    if (!win || !view) return;
    g_sre_GUIWindow_DismissModal(win, view);
}

/* =====================================================================
 * GUI RELAY POINTERS
 * =====================================================================
 * Set by host (main.cpp) via table-driven loop.
 * Used by sre_gui_native.c for fallback (GUIWindow, AlertView, etc.)
 * and by native implementations that need to call engine sub-functions.
 *
 * The actual DrawRect implementations are in sre_gui_native.c.
 * ===================================================================== */

sre_u64 g_orig_GUIWindow_DrawRect    = 0;
sre_u64 g_orig_GUIView_DrawRect      = 0;
sre_u64 g_orig_GUIButton_DrawRect    = 0;
sre_u64 g_orig_GUILabel_DrawRect     = 0;
sre_u64 g_orig_GUIFrameView_DrawRect = 0;
sre_u64 g_orig_GUIAlertView_DrawRect = 0;
sre_u64 g_orig_GUISlider_DrawRect    = 0;
sre_u64 g_orig_NewMenuView_DrawRect  = 0;
sre_u64 g_orig_MainMenuView_ButtonPressed = 0;  /* Phase 2: main menu event interception */
sre_u64 g_orig_CreditsVC_LoadView = 0;          /* Options menu: CreditsVC view customization */
sre_u64 g_orig_CreditsVC_ButtonPressed = 0;     /* Options menu: back button detection */
