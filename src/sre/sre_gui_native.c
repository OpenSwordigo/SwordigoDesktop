/* sre_gui_native.c — Native GUI rendering for libsre.so
 *
 * Full reimplementation of the Caver GUI rendering stack.
 * Every GUI class's DrawRect is implemented in C — no emulated
 * game code involved. We read the C++ object fields directly
 * and render using engine primitives.
 *
 * Object layouts reverse-engineered from decompiled ARM64 v1.4.12.
 *
 * Call chain:
 *   GUIWindow::DrawRect → GUIView::DrawRect → DrawSubviewRect
 *     → virtual dispatch → GUIButton/GUILabel/GUIFrameView::DrawRect
 *
 * Engine primitives used (via function pointers):
 *   RenderingContext::SetMatrix, SetAlpha
 *   FontText::Draw
 *   GUIRoundedRect::Draw, SetColor
 *   GUITexturedRect::Draw
 */

#include "sre.h"
#include "sre_gui.h"
#include "sre_caver.h"

/* =====================================================================
 * FORWARD DECLARATIONS
 * ===================================================================== */

extern SreString* sre_CppString_from_char_p(SreString* self, const char* src);
extern void sre_CppString_release(SreString* self);

/* From sre_gui.c — FontText functions for text rebuild */
extern pfn_FontText_AddText   g_sre_FontText_AddText;
extern pfn_FontText_Clear     g_sre_FontText_Clear;
extern pfn_FontText_SetColor  g_sre_FontText_SetColor;
extern pfn_GUILabel_UpdateText g_sre_GUILabel_UpdateText;

/* =====================================================================
 * PHASE 2: TEXT OVERRIDE & BUTTON CONTROL
 * =====================================================================
 * THE POWER TABLE. Edit these to control the game's GUI from C.
 *
 * To change button text:  add { "Original", "New Text" } to overrides
 * To hide a button:       add "Button Text" to hidden list
 *
 * Changes happen ONCE on first render — zero per-frame overhead.
 * ===================================================================== */

/* Simple string comparison (freestanding — no libc strcmp) */
static int sre_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ---- TEXT OVERRIDE TABLE ----
 * Change ANY label text in the game. Just add a line here.
 * The replacement happens once, permanently for that label. */
typedef struct {
    const char* original;
    const char* replacement;
} SreTextOverride;

static const SreTextOverride g_text_overrides[] = {
    /* ======== MAIN MENU ======== */
    { "Offers",          "Options"      },  /* repurpose Offers button */

    /* ======== ADD YOUR OVERRIDES BELOW ======== */

    { 0, 0 }  /* sentinel — do not remove */
};

/* ---- BUTTON HIDE LIST ----
 * Buttons with these titles are completely hidden.
 * They won't render and won't take up visual space. */
static const char* g_hidden_buttons[] = {
    "Privacy Policy",    /* useless on desktop */
    0  /* sentinel — do not remove */
};

/* ---- Read a GUILabel's text ----
 * The label's std::string is at offset 0x100 (SreString = char* data).
 * The data pointer points directly to null-terminated text. */
#define GL_TEXT_DATA(l) (*(char**)((char*)(l) + 0x100))

static const char* sre_read_label_text(void* label) {
    if (!label) return "";
    char* data = GL_TEXT_DATA(label);
    if (!data) return "";
    return data;
}

/* Look up text override */
static const char* sre_find_override(const char* text) {
    const SreTextOverride* e = g_text_overrides;
    while (e->original) {
        if (sre_streq(text, e->original))
            return e->replacement;
        e++;
    }
    return 0;
}

/* Check if button should be hidden */
static int sre_should_hide(const char* text) {
    const char** e = g_hidden_buttons;
    while (*e) {
        if (sre_streq(text, *e))
            return 1;
        e++;
    }
    return 0;
}

/* Apply text override to a label — changes the internal string permanently.
 * After this, the label's own text reads as the replacement. */
static void sre_apply_text_override(void* label, const char* newText) {
    SreString* str = (SreString*)((char*)label + 0x100);
    sre_CppString_release(str);
    sre_CppString_from_char_p(str, newText);
    /* Rebuild the FontText so it renders the new text */
    if (g_sre_GUILabel_UpdateText)
        g_sre_GUILabel_UpdateText(label);
}

/* =====================================================================
 * PHASE 2: OPTIONS MENU — Clean Implementation
 * =====================================================================
 * Strategy:
 * 1. Hide "Offers" button via hidden button list (done above)
 * 2. Detect MainMenuView by spotting "Credits" button in DrawRect
 * 3. Walk view hierarchy: button→container→MainMenuView
 * 4. Create "Options" button via GIB::MainMenuButtonWithTitle
 * 5. Add it as subview of the container
 * 6. Poll button state in DrawRect for click detection
 * 7. On click, present a Swordigo-native options dialog
 *
 * MainMenuView Object Layout (key offsets):
 *   +0x058  superview pointer (GUIView base)
 *   +0x108  container GUIView (holds all buttons)
 *   +0x118  Start button shared_ptr
 *   +0x128  Achievements button shared_ptr
 *   +0x148  Offers button shared_ptr
 *   +0x158  Credits button shared_ptr
 *   +0x168  Privacy Policy button shared_ptr
 *   +0x178  button vector (begin/end/capacity)
 * ===================================================================== */

/* From sre_gui.c/h — GUI factory helpers */
extern void sre_gui_present_modal(SreSharedPtr* view, int animated);
extern void sre_gui_create_button(SreSharedPtr* out, const char* title);
extern void sre_gui_create_framed_button(SreSharedPtr* out, const char* title);
extern void sre_gui_set_frame(void* view, float x, float y, float w, float h);
extern void sre_gui_add_subview(void* parent, SreSharedPtr* child);
extern void sre_gui_make_string(SreString* out, const char* text);
extern void sre_gui_free_string(SreString* str);

/* Exposed factory function pointers (used directly for AlertView) */
extern pfn_GIB_AlertView          g_sre_GIB_AlertView;
extern pfn_GUIAlertView_AddButton g_sre_GUIAlertView_AddButton;

/* ---- Options button state ---- */
static void*        g_options_button      = 0;  /* raw ptr to our Options GUIButton */
static SreSharedPtr g_options_shared      = {0, 0};  /* shared_ptr (prevent GC) */
static unsigned int g_options_prev_state  = 0;
static int          g_options_created     = 0;  /* 1 = already created */

/* ---- MainMenuView tracking (for scoped icon hiding) ---- */
static void*        g_main_menu_view_ptr  = 0;  /* detected from "Start" button */

/* Read a GUIView's superview pointer (GUIView +0x58) */
#define GV_SUPERVIEW(v) (*(void**)((char*)(v) + 0x58))

/* ---- Options menu state ---- */
static int g_options_menu_open = 0;  /* 1 = overlay is visible */
static int g_options_mode = 0;       /* 1 = next CreditsVC::LoadView creates Options, not Credits */

/* ---- Menu detection for host-side PostFX auto-disable ----
 * Exported to host via get_symbol_vaddr. The host reads this each frame
 * to decide whether to suppress PostFX rendering.
 *
 * Bit flags:
 *   0x01 = NewMenuView visible (main menu / tab bar)
 *   0x02 = GUIAlertView visible (popup / dialog / death screen)
 *   0x04 = GUISlider visible (settings panel)
 *
 * Cleared to 0 in GUIWindow::DrawRect (fires first each frame as root).
 * Set by child DrawRect hooks during the same frame. */
#define SRE_MENU_MAIN_MENU   0x01
#define SRE_MENU_ALERT       0x02
#define SRE_MENU_SLIDER      0x04
volatile int g_sre_menu_active = 0;

/* Text input state — set by SRE hooks, read by host each frame.
 * 1 = text input active (game called StartTextInputWithDelegate)
 * 0 = inactive (StopTextInputWithDelegate or textInputDidFinish called) */
volatile int g_sre_text_input_active = 0;

/* Hard mode toggle — read by both SRE and host.
 * 0 = normal speed (frame-limited, no double-tick)
 * 1 = hard mode (uncapped FPS, ProgramState double-tick — entities move faster)
 * Toggled by host (future: Options menu). */
volatile int g_sre_hardmode = 0;

/* ---- Toggle the SRE Options menu ---- */
static void sre_open_options_menu(void) {
    g_options_menu_open = !g_options_menu_open;
}

/* ---- Draw the Options menu overlay ----
 * Called during GUIWindow::DrawRect when the menu is open.
 * Uses immediate-mode rendering with the engine's own RenderingContext.
 * RGBA colors are int 0-255 per sre_gui.h.
 *
 * Swordigo color palette (from screenshot analysis):
 *   Panel bg:     RGB(18, 30, 50) — deep navy
 *   Panel border: RGB(90, 120, 170) — steel blue
 *   Title bar:    RGB(25, 42, 68) — dark slate
 *   Button bg:    RGB(30, 50, 80) — midnight blue
 *   Button hover: RGB(40, 65, 105) — brighter blue
 *   Button border:RGB(70, 100, 150) — lighter steel
 *   Accent:       RGB(200, 170, 80) — gold (Swordigo signature)
 */

static void sre_draw_options_overlay(void* ctx, float screenW, float screenH) {
    if (!g_options_menu_open) return;

    sre_gui_begin_overlay(ctx, screenW, screenH);

    /* ---- Dim background ---- */
    sre_gui_fill_rect(ctx, 0, 0, screenW, screenH, 0, 0, 0, 180);

    /* ---- Panel dimensions ---- */
    float panelW = 420.0f, panelH = 340.0f;
    float panelX = (screenW - panelW) / 2.0f;
    float panelY = (screenH - panelH) / 2.0f;

    /* ---- Panel shadow (offset behind panel) ---- */
    sre_gui_fill_rect(ctx, panelX + 4, panelY + 4, panelW, panelH, 0, 0, 0, 120);

    /* ---- Panel background ---- */
    sre_gui_fill_rect(ctx, panelX, panelY, panelW, panelH, 18, 30, 50, 245);

    /* ---- Panel border (outer) ---- */
    sre_gui_draw_rect(ctx, panelX, panelY, panelW, panelH, 90, 120, 170, 200);

    /* ---- Panel inner border (subtle) ---- */
    sre_gui_draw_rect(ctx, panelX + 2, panelY + 2,
                      panelW - 4, panelH - 4, 50, 70, 110, 100);

    /* ---- Title bar ---- */
    float titleH = 44.0f;
    sre_gui_fill_rect(ctx, panelX + 3, panelY + 3,
                      panelW - 6, titleH, 25, 42, 68, 255);

    /* ---- Gold accent line under title ---- */
    sre_gui_fill_rect(ctx, panelX + 3, panelY + titleH + 2,
                      panelW - 6, 2.0f, 200, 170, 80, 180);

    /* ---- Back button area (top-left, gold arrow style) ---- */
    float backW = 50.0f, backH = 34.0f;
    float backX = panelX + 8, backY = panelY + 8;
    sre_gui_fill_rect(ctx, backX, backY, backW, backH, 30, 50, 80, 220);
    sre_gui_draw_rect(ctx, backX, backY, backW, backH, 200, 170, 80, 200);

    /* Arrow shape: ◀ (left-pointing using 3 small rects) */
    float arrowCX = backX + backW / 2.0f;
    float arrowCY = backY + backH / 2.0f;
    sre_gui_fill_rect(ctx, arrowCX - 8, arrowCY - 1, 16, 3, 200, 170, 80, 255);
    sre_gui_fill_rect(ctx, arrowCX - 8, arrowCY - 5, 3, 11, 200, 170, 80, 255);
    sre_gui_fill_rect(ctx, arrowCX - 11, arrowCY - 3, 3, 7, 200, 170, 80, 255);

    /* ---- Button layout ---- */
    float btnW = 340.0f, btnH = 44.0f;
    float btnX = panelX + (panelW - btnW) / 2.0f;
    float startY = panelY + titleH + 20.0f;
    float btnGap = 14.0f;

    /* Button 1: Save Editor */
    float b1Y = startY;
    sre_gui_fill_rect(ctx, btnX, b1Y, btnW, btnH, 30, 50, 80, 230);
    sre_gui_draw_rect(ctx, btnX, b1Y, btnW, btnH, 70, 100, 150, 180);
    /* Gold dot indicator */
    sre_gui_fill_rect(ctx, btnX + 14, b1Y + btnH/2 - 3, 6, 6, 200, 170, 80, 255);

    /* Button 2: Audio Settings (placeholder) */
    float b2Y = startY + (btnH + btnGap);
    sre_gui_fill_rect(ctx, btnX, b2Y, btnW, btnH, 30, 50, 80, 230);
    sre_gui_draw_rect(ctx, btnX, b2Y, btnW, btnH, 70, 100, 150, 180);
    sre_gui_fill_rect(ctx, btnX + 14, b2Y + btnH/2 - 3, 6, 6, 200, 170, 80, 255);

    /* Button 3: Post-FX Settings (placeholder) */
    float b3Y = startY + 2 * (btnH + btnGap);
    sre_gui_fill_rect(ctx, btnX, b3Y, btnW, btnH, 30, 50, 80, 230);
    sre_gui_draw_rect(ctx, btnX, b3Y, btnW, btnH, 70, 100, 150, 180);
    sre_gui_fill_rect(ctx, btnX + 14, b3Y + btnH/2 - 3, 6, 6, 200, 170, 80, 255);

    /* ---- Close area at bottom ---- */
    float closeY = startY + 3 * (btnH + btnGap) + 10.0f;
    float closeW = 120.0f, closeH = 36.0f;
    float closeX = panelX + (panelW - closeW) / 2.0f;
    sre_gui_fill_rect(ctx, closeX, closeY, closeW, closeH, 80, 30, 30, 200);
    sre_gui_draw_rect(ctx, closeX, closeY, closeW, closeH, 150, 60, 60, 180);

    /* ---- Version text area (bottom of panel) ---- */
    float verY = panelY + panelH - 18.0f;
    sre_gui_fill_rect(ctx, panelX + panelW/2 - 60, verY,
                      120, 12, 50, 70, 110, 80);

    sre_gui_end_overlay(ctx);
}

/* ---- Create the Options button and add to MainMenuView ---- 
 * Called ONCE when we first detect the "Credits" button in DrawRect.
 * We walk up the view hierarchy to find the container and add our button. */
static void sre_create_options_button(void* credits_button) {
    if (g_options_created) return;
    g_options_created = 1;

    /* Walk up: Credits button → superview = container (MainMenuView+0x108) */
    void* container = GV_SUPERVIEW(credits_button);
    if (!container) return;

    /* Create "Options" button via GIB::MainMenuButtonWithTitle */
    sre_gui_create_button(&g_options_shared, "Options");
    if (!g_options_shared.ptr) {
        g_options_created = 0;  /* retry next frame */
        return;
    }
    g_options_button = (void*)g_options_shared.ptr;

    /* Read Credits button frame for positioning reference */
    float credits_x = *(float*)((char*)credits_button + 0x74);
    float credits_y = *(float*)((char*)credits_button + 0x78);
    float credits_w = *(float*)((char*)credits_button + 0x7C);
    float credits_h = *(float*)((char*)credits_button + 0x80);

    /* Position Options below Credits with same spacing */
    sre_gui_set_frame(g_options_button, credits_x, credits_y - credits_h - 3.0f,
                      credits_w, credits_h);

    /* Add as subview of the container */
    sre_gui_add_subview(container, &g_options_shared);
}

/* ---- Check Options button click (press→release transition) ---- */
static void sre_check_options_click(void* self) {
    unsigned int cur = *(unsigned int*)((char*)self + 0x120);
    if ((g_options_prev_state & 1) && !(cur & 1)) {
        sre_open_options_menu();
    }
    g_options_prev_state = cur;
}


/* =====================================================================
 * GUIView OBJECT LAYOUT (base class for all GUI elements)
 *
 * Offset | Field
 * -------|------
 *  0x00  | vtable*
 *  0x20  | subviews linked list (next ptr; sentinel = &this+0x20)
 *  0x28  | subviews linked list (prev ptr)
 *  0x58  | parent GUIView*
 *  0x60  | animations linked list head
 *  0x84  | frame.x (float)
 *  0x88  | frame.y (float)
 *  0x8C  | frame.w (float)
 *  0x90  | frame.h (float)
 *  0xA4  | local transform (Matrix4 = 64 bytes)
 *  0xE4  | isHidden (byte)
 *  0xE8  | clipsToBounds (byte)
 * ===================================================================== */

/* Accessor macros for reading GUIView fields from raw pointer */
#define GV_VTABLE(v)       (*(void***)(v))
#define GV_SUBVIEW_HEAD(v) ((char*)(v) + 0x20)
#define GV_FRAME_X(v)      (*(float*)((char*)(v) + 0x84))
#define GV_FRAME_Y(v)      (*(float*)((char*)(v) + 0x88))
#define GV_FRAME_W(v)      (*(float*)((char*)(v) + 0x8C))
#define GV_FRAME_H(v)      (*(float*)((char*)(v) + 0x90))
#define GV_TRANSFORM(v)    ((float*)((char*)(v) + 0xA4))
#define GV_HIDDEN(v)       (*(unsigned char*)((char*)(v) + 0xE4))
#define GV_CLIPS(v)        (*(unsigned char*)((char*)(v) + 0xE8))
#define GV_PARENT(v)       (*(void**)((char*)(v) + 0x58))

/* Linked list node for subviews:
 *   node+0x00 = next node*
 *   node+0x08 = prev node*
 *   node+0x10 = GUIView* (the actual child view)
 */
#define LISTNODE_NEXT(n)   (*(void**)(n))
#define LISTNODE_CHILD(n)  (*(void**)((char*)(n) + 0x10))

/* =====================================================================
 * GUILabel OBJECT LAYOUT (extends GUIView)
 *
 *  0xF0  | Font* (shared_ptr raw)
 *  0x100 | text string ptr (std::string)
 *  0x108 | text color (uint32 RGBA packed)
 *  0x120 | FontText* (the renderable text object)
 *  0x128 | FontText refcount*
 *  0x130 | h_align (int)
 *  0x134 | v_align (int)
 *  0x138 | word_wrap (byte)
 * ===================================================================== */

#define GL_FONTTEXT(l)     (*(void**)((char*)(l) + 0x120))
#define GL_COLOR(l)        (*(unsigned int*)((char*)(l) + 0x108))

/* =====================================================================
 * GUIButton OBJECT LAYOUT (extends GUIView)
 *
 *  0x120 | state flags (uint32) — bit0=pressed, bit1=highlighted/dimmed
 *  0x124 | skip-draw flag (byte)
 *  0x128 | normal frame GUIRoundedRect*
 *  0x130 | highlighted frame GUIRoundedRect*
 *  0x138 | background GUIRoundedRect*
 *  0x140 | title GUILabel* (child view)
 *  0x148 | normal image GUITexturedRect*
 *  0x150 | highlighted image GUITexturedRect*
 *  0x158 | image transform (Matrix4, 64 bytes)
 *  0x1B0 | image offset x (float)
 *  0x1B4 | image offset y (float)
 *  0x1B8 | background color (4 bytes RGBA)
 *  0x1BC | tint color R (byte)
 *  0x1BD | tint color G (byte)
 *  0x1BE | tint color B (byte)
 *  0x1BF | tint color A (byte)
 * ===================================================================== */

#define GB_STATE(b)        (*(unsigned int*)((char*)(b) + 0x120))
#define GB_SKIPDRAW(b)     (*(unsigned char*)((char*)(b) + 0x124))
#define GB_FRAME_NORMAL(b) (*(void**)((char*)(b) + 0x128))
#define GB_FRAME_HILITE(b) (*(void**)((char*)(b) + 0x130))
#define GB_BG_RECT(b)      (*(void**)((char*)(b) + 0x138))
#define GB_TITLE_LABEL(b)  (*(void**)((char*)(b) + 0x140))
#define GB_IMG_NORMAL(b)   (*(void**)((char*)(b) + 0x148))
#define GB_IMG_HILITE(b)   (*(void**)((char*)(b) + 0x150))
#define GB_IMG_XFORM(b)    ((float*)((char*)(b) + 0x158))
#define GB_IMG_OFS_X(b)    (*(float*)((char*)(b) + 0x1B0))
#define GB_IMG_OFS_Y(b)    (*(float*)((char*)(b) + 0x1B4))
#define GB_TINT_R(b)       (*(unsigned char*)((char*)(b) + 0x1BC))
#define GB_TINT_G(b)       (*(unsigned char*)((char*)(b) + 0x1BD))
#define GB_TINT_B(b)       (*(unsigned char*)((char*)(b) + 0x1BE))
#define GB_TINT_A(b)       (*(unsigned char*)((char*)(b) + 0x1BF))

/* =====================================================================
 * GUIFrameView OBJECT LAYOUT (extends GUIView)
 *
 *  0xE9  | frame color (uint32 RGBA)
 *  0xF0  | textured rect GUITexturedRect*
 *  0xF8  | rounded rect GUIRoundedRect*
 *  0x108 | pivot flag (byte)
 * ===================================================================== */

#define GFV_TEX_RECT(f)   (*(void**)((char*)(f) + 0xF0))
#define GFV_RND_RECT(f)   (*(void**)((char*)(f) + 0xF8))
#define GFV_PIVOT(f)       (*(unsigned char*)((char*)(f) + 0x108))

/* =====================================================================
 * ENGINE FUNCTION POINTERS
 * Set during init from sre_gui.c globals.
 * ===================================================================== */

/* From sre_background.c */
extern pfn_SetMatrix   g_RenderingContext_SetMatrix;
extern pfn_SetColor    g_RenderingContext_SetColor;
extern pfn_SpriteDraw  g_Sprite_Draw;

/* From sre_gui.c (made non-static for native GUI access) */
extern pfn_SetAlpha        g_sre_SetAlpha;
extern pfn_FontText_Draw   g_sre_FontText_Draw;

/* Additional engine functions needed for native GUI.
 * These are passed from the host or resolved from nm. */

/* GUIRoundedRect — draws 9-patch rounded backgrounds */
typedef void (*pfn_GUIRoundedRect_Draw)(void* self, void* ctx);
/* SetColor takes Caver::Color BY VALUE (4 bytes packed RGBA in w1 register) */
typedef void (*pfn_GUIRoundedRect_SetColor)(void* self, unsigned int colorPacked);

/* GUITexturedRect — draws textured quads */
typedef void (*pfn_GUITexturedRect_Draw)(void* self, void* ctx);

/* GL functions — from PLT */
typedef void (*pfn_glEnable)(unsigned int cap);
typedef void (*pfn_glDisable)(unsigned int cap);
typedef void (*pfn_glScissor)(int x, int y, int w, int h);

/* Global pointers — set by sre_init_gui_native */
pfn_GUIRoundedRect_Draw     g_GUIRoundedRect_Draw = 0;
pfn_GUIRoundedRect_SetColor g_GUIRoundedRect_SetColor = 0;
pfn_GUITexturedRect_Draw    g_GUITexturedRect_Draw = 0;
pfn_glEnable                g_glEnable = 0;
pfn_glDisable               g_glDisable = 0;
pfn_glScissor               g_glScissor = 0;

/* Init struct for additional addresses */
typedef struct {
    sre_u64 GUIRoundedRect_Draw;     /* nm needed */
    sre_u64 GUIRoundedRect_SetColor; /* nm needed */
    sre_u64 GUITexturedRect_Draw;    /* nm needed */
    sre_u64 glEnable;                /* PLT */
    sre_u64 glDisable;               /* PLT */
    sre_u64 glScissor;               /* PLT */
} SreGuiNativeAddrs;

void sre_init_gui_native(SreGuiNativeAddrs* addrs) {
    g_GUIRoundedRect_Draw     = (pfn_GUIRoundedRect_Draw)addrs->GUIRoundedRect_Draw;
    g_GUIRoundedRect_SetColor = (pfn_GUIRoundedRect_SetColor)addrs->GUIRoundedRect_SetColor;
    g_GUITexturedRect_Draw    = (pfn_GUITexturedRect_Draw)addrs->GUITexturedRect_Draw;
    g_glEnable                = (pfn_glEnable)addrs->glEnable;
    g_glDisable               = (pfn_glDisable)addrs->glDisable;
    g_glScissor               = (pfn_glScissor)addrs->glScissor;
}

/* =====================================================================
 * MATRIX MATH (no libc — freestanding)
 * ===================================================================== */

static void mat4_identity_gui(float* m) {
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
}

static void mat4_mul_gui(const float* a, const float* b, float* out) {
    float tmp[16];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++)
                sum += a[i * 4 + k] * b[k * 4 + j];
            tmp[i * 4 + j] = sum;
        }
    }
    for (i = 0; i < 16; i++) out[i] = tmp[i];
}

static void mat4_post_translate(float* m, float tx, float ty, float tz) {
    /* M = M * T where T is a translation matrix.
     * Only affects column 3: m[i*4+3] += m[i*4+0]*tx + m[i*4+1]*ty + m[i*4+2]*tz */
    int i;
    for (i = 0; i < 4; i++) {
        m[i * 4 + 3] += m[i * 4 + 0] * tx + m[i * 4 + 1] * ty + m[i * 4 + 2] * tz;
    }
}

/* =====================================================================
 * RECTANGLE MATH
 * ===================================================================== */

static int rect_intersect(const float* a, const float* b, float* out) {
    /* a, b, out = {x, y, w, h} */
    float ax2 = a[0] + a[2], ay2 = a[1] + a[3];
    float bx2 = b[0] + b[2], by2 = b[1] + b[3];
    float x1 = (a[0] > b[0]) ? a[0] : b[0];
    float y1 = (a[1] > b[1]) ? a[1] : b[1];
    float x2 = (ax2 < bx2) ? ax2 : bx2;
    float y2 = (ay2 < by2) ? ay2 : by2;
    out[0] = x1;
    out[1] = y1;
    out[2] = x2 - x1;
    out[3] = y2 - y1;
    return (out[2] > 0.0f && out[3] > 0.0f);
}

static float fabsf_sre(float x) { return x < 0.0f ? -x : x; }

/* =====================================================================
 * NATIVE DrawRect IMPLEMENTATIONS
 *
 * Relay pointers (g_orig_*) are defined in sre_gui.c.
 * We declare them here as extern so we can use them as fallbacks.
 * ===================================================================== */

extern sre_u64 g_orig_GUIWindow_DrawRect;
extern sre_u64 g_orig_GUIView_DrawRect;
extern sre_u64 g_orig_GUIButton_DrawRect;
extern sre_u64 g_orig_GUILabel_DrawRect;
extern sre_u64 g_orig_GUIFrameView_DrawRect;
extern sre_u64 g_orig_GUIAlertView_DrawRect;
extern sre_u64 g_orig_GUISlider_DrawRect;
extern sre_u64 g_orig_NewMenuView_DrawRect;

/* 8-param DrawRect — ARM64 passes extra data through x4-x7 that
 * GUIView::DrawRect needs for child iteration/transforms/animations.
 * We MUST capture and forward all 8 register params. */
typedef void (*pfn_DrawRect_N)(void* self, void* ctx, void* rect, void* mat,
                                void* p4, void* p5, void* p6, void* p7);

/* Call the ORIGINAL GUIView::DrawRect via relay.
 * Forwards all 8 register params so the engine handles
 * child iteration, transforms, and animations correctly. */
static void orig_GUIView_DrawRect_8(void* self, void* ctx, void* rect, void* mat,
                                     void* p4, void* p5, void* p6, void* p7) {
    if (g_orig_GUIView_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUIView_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

/* =====================================================================
 * COMMENTED OUT: native_GUIView_DrawRect + native_DrawSubviewRect
 * Currently using original engine relay via orig_GUIView_DrawRect_8().
 * Preserved for future native tree-walker reimplementation.
 * =====================================================================

static void native_GUIView_DrawRect(void* self, void* ctx, void* rect, void* mat) {
    char* sentinel = GV_SUBVIEW_HEAD(self);
    char* node = LISTNODE_NEXT(sentinel);

    while (node != sentinel) {
        void* child = LISTNODE_CHILD(node);

        if (!GV_HIDDEN(child)) {
            if (GV_CLIPS(child) && g_glEnable && g_glScissor) {
                g_glEnable(0x0C11);
                float cx = GV_FRAME_X(child);
                float cy = GV_FRAME_Y(child);
                float cw = GV_FRAME_W(child);
                float ch = GV_FRAME_H(child);
                g_glScissor((int)cx, (int)cy, (int)cw, (int)ch);
            }

            native_DrawSubviewRect(self, child, ctx,
                                    (float*)rect, (float*)mat);

            if (GV_CLIPS(child) && g_glDisable) {
                g_glDisable(0x0C11);
            }
        }

        node = LISTNODE_NEXT(node);
    }
}

static void native_DrawSubviewRect(void* parent, void* child, void* ctx,
                                    float* clipRect, float* parentMat) {
    float childMat[16];
    float scaleMat[16];
    float tmpMat[16];
    float childRect[4];
    int i;

    for (i = 0; i < 16; i++) childMat[i] = parentMat[i];

    float pw = GV_FRAME_W(parent);
    float ph = GV_FRAME_H(parent);
    float px = GV_FRAME_X(parent);
    float py = GV_FRAME_Y(parent);

    float cx = GV_FRAME_X(child);
    float cy = GV_FRAME_Y(child);
    float cw = GV_FRAME_W(child);
    float ch = GV_FRAME_H(child);

    float offX = (cx + cw * 0.5f) - (px + pw * 0.5f);
    float offY = (cy + ch * 0.5f) - (py + ph * 0.5f);

    if (fabsf_sre(offX) > 0.001f || fabsf_sre(offY) > 0.001f) {
        float ndcX = (pw > 0.001f) ? (2.0f / pw) * offX : 0.0f;
        float ndcY = (ph > 0.001f) ? (2.0f / ph) * offY : 0.0f;
        mat4_post_translate(childMat, ndcX, ndcY, 0.0f);
    }

    if (fabsf_sre(cw - pw) > 0.001f || fabsf_sre(ch - ph) > 0.001f) {
        float sx = (pw > 0.001f) ? cw / pw : 1.0f;
        float sy = (ph > 0.001f) ? ch / ph : 1.0f;
        mat4_identity_gui(scaleMat);
        scaleMat[0] = sx;
        scaleMat[5] = sy;
        for (i = 0; i < 16; i++) tmpMat[i] = childMat[i];
        mat4_mul_gui(tmpMat, scaleMat, childMat);
    }

    float childFrame[4] = { cx, cy, cw, ch };
    if (!rect_intersect(clipRect, childFrame, childRect)) {
        return;
    }

    void** vtable = GV_VTABLE(child);
    pfn_DrawRect_N drawFunc = (pfn_DrawRect_N)(vtable[0x50 / 8]);
    if (drawFunc) {
        drawFunc(child, ctx, childRect, childMat, 0, 0, 0, 0);
    }
}

 * END COMMENTED OUT SECTION
 * ===================================================================== */

/* =====================================================================
 * GUILabel::DrawRect — native reimplementation
 *
 * 1. Call virtual method vtable[0x128/8] (ApplyTransform)
 * 2. SetMatrix(identity)
 * 3. FontText::Draw(this->fontText, ctx)
 * 4. GUIView::DrawRect(this, ...) — draw children
 * ===================================================================== */
static void native_GUILabel_DrawRect(void* self, void* ctx, void* rect, void* mat,
                                      void* p4, void* p5, void* p6, void* p7) {
    float identity[16];

    /* ---- PHASE 2: Text interception ----
     * Check if this label's text should be overridden.
     * The override modifies the label's internal string permanently,
     * so this check is essentially free after the first frame. */
    const char* text = sre_read_label_text(self);
    const char* override = sre_find_override(text);
    if (override) {
        sre_apply_text_override(self, override);
    }

    /* Step 1: Call ApplyTransform virtual (vtable+0x128) */
    void** vt = GV_VTABLE(self);
    typedef void (*pfn_ApplyTransform)(void*, void*, void*);
    pfn_ApplyTransform applyXform = (pfn_ApplyTransform)(vt[0x128 / 8]);
    if (applyXform) applyXform(self, ctx, mat);

    /* Step 2: Set identity matrix */
    mat4_identity_gui(identity);
    if (g_RenderingContext_SetMatrix) g_RenderingContext_SetMatrix(ctx, identity);

    /* Step 3: Draw text */
    void* fontText = GL_FONTTEXT(self);
    if (fontText && g_sre_FontText_Draw) {
        g_sre_FontText_Draw(fontText, ctx);
    }

    /* Step 4: Draw children via ORIGINAL engine code (all 8 params forwarded) */
    orig_GUIView_DrawRect_8(self, ctx, rect, mat, p4, p5, p6, p7);
}

/* =====================================================================
 * GUIButton::DrawRect — native reimplementation
 *
 * 1. Early out if skip-draw flag set
 * 2. Call vtable[0x128/8] (layout)
 * 3. SetMatrix(identity)
 * 4. Compute tint (dimmed if pressed)
 * 5. Draw background GUIRoundedRect
 * 6. Draw frame GUIRoundedRect (normal or highlighted)
 * 7. Compute image position, SetMatrix, draw image
 * 8. Dim label color if pressed
 * 9. GUIView::DrawRect (draws children incl. label)
 * 10. Restore label color
 * ===================================================================== */
static void native_GUIButton_DrawRect(void* self, void* ctx, void* rect, void* mat,
                                       void* p4, void* p5, void* p6, void* p7) {
    float identity[16];
    float imgMat[16];
    float tmpMat[16];
    unsigned int state;
    unsigned int tintR, tintG, tintB, tintA;
    void* frameRect;

    /* ---- PHASE 2: Button hiding ----
     * Check title label text against the hide list.
     * If matched, set the view's hidden flag so the engine
     * also skips it in future frames.
     * Also detect Options button clicks for our custom menu. */
    void* titleLabel = GB_TITLE_LABEL(self);
    const char* btnText = 0;
    if (titleLabel) {
        btnText = sre_read_label_text(titleLabel);
    }

    /* Determine if this is a "text button" (has a non-empty title) */
    int has_text = (btnText && btnText[0] != '\0');

    if (has_text) {
        /* Text button — check against hide list */
        if (sre_should_hide(btnText)) {
            GV_HIDDEN(self) = 1;  /* hide permanently */
            return;
        }
        /* Detect MainMenuView: when we see "Start", walk up the view
         * hierarchy to find the MainMenuView and store its pointer.
         * Start → superview(container at +0x108) → superview(MainMenuView) */
        if (!g_main_menu_view_ptr && sre_streq(btnText, "Start")) {
            void* container = GV_SUPERVIEW(self);
            if (container) g_main_menu_view_ptr = GV_SUPERVIEW(container);
        }
    } else {
        /* Do not hide icon-only buttons based on g_main_menu_view_ptr. That
         * pointer outlives scene teardown and may be reused by an in-game
         * HUD object, which hides the HUD after a transition or pickup. */
    }

    /* Phase 2: Poll Options button state for click detection */
    if (self == g_options_button && g_options_button != 0) {
        sre_check_options_click(self);
    }

    /* Step 1: early out */
    if (GB_SKIPDRAW(self)) return;

    /* Step 2: call layout virtual */
    void** vt = GV_VTABLE(self);
    typedef void (*pfn_Layout)(void*, void*, void*);
    pfn_Layout layoutFn = (pfn_Layout)(vt[0x128 / 8]);
    if (layoutFn) layoutFn(self, ctx, mat);

    /* Step 3: identity matrix for frame/background drawing */
    mat4_identity_gui(identity);
    if (g_RenderingContext_SetMatrix) g_RenderingContext_SetMatrix(ctx, identity);

    /* Step 4: compute tint color */
    state = GB_STATE(self);
    tintR = GB_TINT_R(self);
    tintG = GB_TINT_G(self);
    tintB = GB_TINT_B(self);
    tintA = GB_TINT_A(self);

    /* If highlighted/pressed (bit 1): dim by 50% */
    if (state & 2) {
        tintR = (unsigned int)((float)tintR * 0.5f);
        tintG = (unsigned int)((float)tintG * 0.5f);
        tintB = (unsigned int)((float)tintB * 0.5f);
        if (tintR > 255) tintR = 255;
        if (tintG > 255) tintG = 255;
        if (tintB > 255) tintB = 255;
    }

    /* Step 5: draw background */
    if (GB_BG_RECT(self) && g_GUIRoundedRect_Draw) {
        g_GUIRoundedRect_Draw(GB_BG_RECT(self), ctx);
    }

    /* Step 6: draw frame (highlighted if pressed + exists, else normal) */
    frameRect = 0;
    if ((state & 1) && GB_FRAME_HILITE(self)) {
        frameRect = GB_FRAME_HILITE(self);
    } else if (GB_FRAME_NORMAL(self)) {
        frameRect = GB_FRAME_NORMAL(self);
    }
    if (frameRect && g_GUIRoundedRect_Draw) {
        /* TODO: SetColor for press-dimming. Skipped for now —
         * the frame already has its original color from creation. */
        g_GUIRoundedRect_Draw(frameRect, ctx);
    }

    /* Step 7: draw center image */
    {
        float centerX = GV_FRAME_X(self) + GV_FRAME_W(self) * 0.5f + GB_IMG_OFS_X(self);
        float centerY = GV_FRAME_Y(self) + GV_FRAME_H(self) * 0.5f + GB_IMG_OFS_Y(self);
        int i;

        /* Build translation matrix (column-major for OpenGL) */
        mat4_identity_gui(imgMat);
        imgMat[12] = centerX;  /* column-major: [3][0] = index 12 */
        imgMat[13] = centerY;  /* column-major: [3][1] = index 13 */
        imgMat[14] = 0.0f;     /* column-major: [3][2] = index 14 */

        /* Multiply with the button's image transform at 0x158 */
        for (i = 0; i < 16; i++) tmpMat[i] = imgMat[i];
        mat4_mul_gui(tmpMat, GB_IMG_XFORM(self), imgMat);

        if (g_RenderingContext_SetMatrix) g_RenderingContext_SetMatrix(ctx, imgMat);

        void* imgRect = 0;
        if ((state & 1) && GB_IMG_HILITE(self)) {
            imgRect = GB_IMG_HILITE(self);
        } else if (GB_IMG_NORMAL(self)) {
            imgRect = GB_IMG_NORMAL(self);
        }
        if (imgRect && g_GUITexturedRect_Draw) {
            g_GUITexturedRect_Draw(imgRect, ctx);
        }
    }

    /* Step 8: dim label if pressed (bit 1) */
    void* label = GB_TITLE_LABEL(self);
    unsigned int origColor = 0;
    int colorChanged = 0;
    if (label && (state & 2)) {
        origColor = GL_COLOR(label);
        unsigned int r = (origColor      ) & 0xFF;
        unsigned int g = (origColor >>  8) & 0xFF;
        unsigned int b = (origColor >> 16) & 0xFF;
        unsigned int a = (origColor >> 24) & 0xFF;
        unsigned int dr = (unsigned int)((float)r * 0.3f) & 0xFF;
        unsigned int dg = (unsigned int)((float)g * 0.3f) & 0xFF;
        unsigned int db = (unsigned int)((float)b * 0.3f) & 0xFF;
        unsigned int dimmed = dr | (dg << 8) | (db << 16) | (a << 24);
        if (dimmed != origColor) {
            /* Clear cached text to force re-render */
            *(sre_u64*)((char*)label + 0x110) = 0;
            *(sre_u64*)((char*)label + 0x118) = 0;
            GL_COLOR(label) = dimmed;
            colorChanged = 1;
            /* Call UpdateText to rebuild FontText with new color.
             * We use the engine's GUILabel::UpdateText via function pointer. */
            /* For now, the label will pick up the color change in its DrawRect. */
        }
    }

    /* Step 9: draw children (including label) via ORIGINAL engine code */
    orig_GUIView_DrawRect_8(self, ctx, rect, mat, p4, p5, p6, p7);

    /* Step 10: restore label color */
    if (label && colorChanged) {
        *(sre_u64*)((char*)label + 0x110) = 0;
        *(sre_u64*)((char*)label + 0x118) = 0;
        GL_COLOR(label) = origColor;
    }
}

/* =====================================================================
 * GUIFrameView::DrawRect — native reimplementation
 *
 * 1. Call vtable[0x128/8] (ApplyTransform)
 * 2. Set identity matrix
 * 3. If pivot flag: build pivot transform matrix
 * 4. Draw textured rect background
 * 5. Draw rounded rect background
 * 6. GUIView::DrawRect (children)
 * ===================================================================== */
static void native_GUIFrameView_DrawRect(void* self, void* ctx, void* rect, void* mat,
                                          void* p4, void* p5, void* p6, void* p7) {
    float identity[16];

    /* Step 1: ApplyTransform */
    void** vt = GV_VTABLE(self);
    typedef void (*pfn_ApplyTransform)(void*, void*, void*);
    pfn_ApplyTransform applyXform = (pfn_ApplyTransform)(vt[0x128 / 8]);
    if (applyXform) applyXform(self, ctx, mat);

    /* Step 2: identity matrix */
    mat4_identity_gui(identity);
    if (g_RenderingContext_SetMatrix) g_RenderingContext_SetMatrix(ctx, identity);

    /* Step 3: pivot transform (if flagged) */
    if (GFV_PIVOT(self)) {
        float pivotMat[16];
        float cx = GV_FRAME_X(self) + GV_FRAME_W(self) * 0.5f;
        float cy = GV_FRAME_Y(self) + GV_FRAME_H(self) * 0.5f;

        /* identity with X flip: m[0] = -1 */
        mat4_identity_gui(pivotMat);
        pivotMat[0] = -1.0f;

        /* Pre-translate to center, post-translate back */
        /* Simplified: translate -> flip -> translate back */
        mat4_post_translate(pivotMat, cx, cy, 0.0f);
        /* Note: the original does PreTranslate then PostTranslate with negatives.
         * This is a pivot around center. Simplified version for now. */

        if (g_RenderingContext_SetMatrix) g_RenderingContext_SetMatrix(ctx, pivotMat);
    }

    /* Step 4: draw textured rect */
    if (GFV_TEX_RECT(self) && g_GUITexturedRect_Draw) {
        g_GUITexturedRect_Draw(GFV_TEX_RECT(self), ctx);
    }

    /* Step 5: draw rounded rect */
    if (GFV_RND_RECT(self) && g_GUIRoundedRect_Draw) {
        g_GUIRoundedRect_Draw(GFV_RND_RECT(self), ctx);
    }

    /* Step 6: draw children via ORIGINAL engine code */
    orig_GUIView_DrawRect_8(self, ctx, rect, mat, p4, p5, p6, p7);
}

/* =====================================================================
 * PUBLIC HOOK FUNCTIONS
 *
 * These are the actual exported symbols that get hooked.
 * They call our native implementations.
 * ===================================================================== */

/* Relay pointers defined in sre_gui.c (for fallback) */

void sre_GUIView_DrawRect(void* self, void* ctx, void* rect, void* mat,
                           void* p4, void* p5, void* p6, void* p7) {
    /* Use original engine code — child iteration is too complex
     * (transforms, animations, clipping) to reimplement safely.
     * We own the leaf classes, not the tree walker. */
    if (g_orig_GUIView_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUIView_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

void sre_GUILabel_DrawRect(void* self, void* ctx, void* rect, void* mat,
                            void* p4, void* p5, void* p6, void* p7) {
    const char* override = sre_find_override(sre_read_label_text(self));
    if (override) sre_apply_text_override(self, override);

    if (g_orig_GUILabel_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUILabel_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

void sre_GUIButton_DrawRect(void* self, void* ctx, void* rect, void* mat,
                              void* p4, void* p5, void* p6, void* p7) {
    void* titleLabel = GB_TITLE_LABEL(self);
    const char* btnText = titleLabel ? sre_read_label_text(titleLabel) : 0;

    if (btnText && btnText[0] != '\0') {
        if (sre_should_hide(btnText)) {
            GV_HIDDEN(self) = 1;
            return;
        }
        if (!g_main_menu_view_ptr && sre_streq(btnText, "Start")) {
            void* container = GV_SUPERVIEW(self);
            if (container) g_main_menu_view_ptr = GV_SUPERVIEW(container);
        }
    }

    if (self == g_options_button && g_options_button != 0)
        sre_check_options_click(self);

    if (g_orig_GUIButton_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUIButton_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

void sre_GUIFrameView_DrawRect(void* self, void* ctx, void* rect, void* mat,
                                 void* p4, void* p5, void* p6, void* p7) {
    if (g_orig_GUIFrameView_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUIFrameView_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

/* These still use relay for now — they have complex logic beyond
 * basic drawing that needs more research. */
void sre_GUIWindow_DrawRect(void* self, void* ctx, void* rect, void* mat,
                              void* p4, void* p5, void* p6, void* p7) {
    if (g_orig_GUIWindow_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUIWindow_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);

    /* Options overlay — DISABLED for v6, Options menu WIP.
     * if (g_options_menu_open && ctx) { ... }
     */
}

void sre_GUIAlertView_DrawRect(void* self, void* ctx, void* rect, void* mat,
                                 void* p4, void* p5, void* p6, void* p7) {
    g_sre_menu_active |= SRE_MENU_ALERT;
    if (g_orig_GUIAlertView_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUIAlertView_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

void sre_GUISlider_DrawRect(void* self, void* ctx, void* rect, void* mat,
                              void* p4, void* p5, void* p6, void* p7) {
    g_sre_menu_active |= SRE_MENU_SLIDER;
    if (g_orig_GUISlider_DrawRect)
        ((pfn_DrawRect_N)g_orig_GUISlider_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

void sre_NewMenuView_DrawRect(void* self, void* ctx, void* rect, void* mat,
                                void* p4, void* p5, void* p6, void* p7) {
    g_sre_menu_active |= SRE_MENU_MAIN_MENU;
    if (g_orig_NewMenuView_DrawRect)
        ((pfn_DrawRect_N)g_orig_NewMenuView_DrawRect)(self, ctx, rect, mat, p4, p5, p6, p7);
}

/* =====================================================================
 * CreditsVC::LoadView — Hijack for Options Menu
 * =====================================================================
 * When g_options_mode is set (by DidOpenShop), this intercepts
 * CreditsViewController::LoadView. We call the original LoadView to get
 * the full VC structure (back button, transitions, etc.), then the
 * CreditsView content will be overlaid by our Options content.
 *
 * ARM64 ABI:
 *   x0 = CreditsViewController* (this)
 * ===================================================================== */

extern sre_u64 g_orig_CreditsVC_LoadView;

void sre_CreditsVC_LoadView(void* self) {
    typedef void (*pfn_loadview)(void* self);

    if (g_options_mode) {
        /* Options mode: call original LoadView to set up the VC structure
         * (back button, view hierarchy, etc.) */
        if (g_orig_CreditsVC_LoadView)
            ((pfn_loadview)g_orig_CreditsVC_LoadView)(self);

        /* Mark that options overlay should render on this CreditsView */
        g_options_menu_open = 1;
        g_options_mode = 0;
    } else {
        /* Normal Credits: call original as-is */
        if (g_orig_CreditsVC_LoadView)
            ((pfn_loadview)g_orig_CreditsVC_LoadView)(self);
    }
}

/* =====================================================================
 * CreditsVC::ButtonPressed — Clear overlay on back button
 * =====================================================================
 * The CreditsVC's back button calls ButtonPressed which triggers a
 * transition back to the main menu. We clear g_options_menu_open so
 * the overlay stops rendering.
 *
 * ARM64 ABI:
 *   x0 = CreditsViewController* (this)
 *   x1 = void* (target)
 *   x2 = GUIEvent* (event)
 * ===================================================================== */

extern sre_u64 g_orig_CreditsVC_ButtonPressed;

void sre_CreditsVC_ButtonPressed(void* self, void* target, void* event) {
    typedef void (*pfn_btn_pressed)(void* self, void* target, void* event);

    /* Clear our overlay flag BEFORE calling original (which transitions away) */
    g_options_menu_open = 0;

    /* Call original — handles the back transition */
    if (g_orig_CreditsVC_ButtonPressed)
        ((pfn_btn_pressed)g_orig_CreditsVC_ButtonPressed)(self, target, event);
}

/* =====================================================================
 * OPTIONS BUTTON CLICK — Hook MainMenuViewDidOpenShop
 * =====================================================================
 * When the user clicks "Options" (renamed from "Offers"), the engine's
 * MainMenuView::ButtonPressed dispatches to the delegate method:
 *   MainMenuViewController::MainMenuViewDidOpenShop(MainMenuView*)
 *
 * We hook this delegate method to open our Options menu instead of
 * the original IAP/shop screen. No relay needed — we fully replace.
 *
 * ARM64 ABI:
 *   x0 = MainMenuViewController* (this)
 *   x1 = MainMenuView* (the view that triggered)
 * ===================================================================== */

void sre_MainMenuVC_DidOpenShop(void* self, void* view) {
    /* v6: Suppress the shop/IAP screen. Options menu is WIP.
     * Do NOT call original — prevents the broken IAP screen from opening.
     * TODO(v7): Present our custom Options VC here. */
}

/* =====================================================================
 * MainMenuView::ButtonPressed — Phase 2 Event Interception
 * =====================================================================
 * Called via boost::bind when ANY button in the MainMenuView is pressed.
 * Dispatch is by POINTER COMPARISON against stored button shared_ptrs:
 *   self+0x118 = Start, self+0x128 = Achievements,
 *   self+0x148 = Offers, self+0x158 = Credits, etc.
 *
 * ARM64 ABI — this is a C++ member function called through boost::bind:
 *   boost::bind(&MainMenuView::ButtonPressed, this, _1, _2)
 * When the boost::function is invoked with (target, sender):
 *   x0 = this        (MainMenuView*, bound by boost::bind)
 *   x1 = target      (MainMenuView*, registered target — same as x0)
 *   x2 = sender      (GUIButton* — the button that was pressed)
 *
 * We intercept:
 *   - Offers button (self+0x148) → BLOCK (prevents IAP screen)
 *   - Our Options button → open Options menu
 *   - All others → relay to original
 * ===================================================================== */

extern sre_u64 g_orig_MainMenuView_ButtonPressed;

/* 3-arg signature matching the ARM64 calling convention */
typedef void (*pfn_ButtonPressed3)(void* self, void* target, void* sender);

void sre_MainMenuView_ButtonPressed(void* self, void* target, void* sender) {
    /* Block the Offers button — read its ptr from MainMenuView+0x148 */
    void* offers_ptr = *(void**)((char*)self + 0x148);
    if (sender == offers_ptr && offers_ptr != 0) {
        /* Offers click blocked — do nothing (button is hidden anyway) */
        return;
    }

    /* Check if sender is our custom Options button */
    if (sender == g_options_button && g_options_button != 0) {
        sre_open_options_menu();
        return;
    }

    /* All other buttons: relay to original */
    if (g_orig_MainMenuView_ButtonPressed)
        ((pfn_ButtonPressed3)g_orig_MainMenuView_ButtonPressed)(self, target, sender);
}

/* =====================================================================
 * DEATH/RESPAWN FIX
 * =====================================================================
 * When the player dies, the game shows a GameOverView. Tapping it calls
 * GameOverViewController::ShowAdMaybe(), which tries to display an
 * interstitial ad before respawning. On desktop there's no ad SDK, so
 * the ad never shows and the player gets permanently stuck.
 *
 * Fix: Hook ShowAdMaybe and directly call GameOverViewDidContinue,
 * which handles the respawn logic.
 *
 * Addresses (v1.4.12 ARM64):
 *   ShowAdMaybe:          0x347efc  (we hook this)
 *   GameOverViewDidContinue: 0x348060  (we call this)
 *
 * ARM64 ABI:
 *   ShowAdMaybe:          x0 = GameOverViewController* (this)
 *   GameOverViewDidContinue: x0 = GameOverViewController* (this)
 *                            x1 = GameOverView* (the view)
 *
 * We pass NULL for the view — the function likely doesn't use it
 * (it accesses the view through the controller's stored reference).
 * ===================================================================== */

extern uint64_t g_swordigo_base;  /* from sre_init.c */

extern pfn_GameOverVC_DidContinue g_sre_GameOverVC_DidContinue;

void sre_GameOverVC_ShowAdMaybe(void* self) {
    /* Skip the ad, go straight to respawn */
    if (g_sre_GameOverVC_DidContinue) {
        g_sre_GameOverVC_DidContinue(self, 0);
    }
}

/* =====================================================================
 * TEXT INPUT — Full SRE Takeover
 * =====================================================================
 * libswordigo's text input subsystem is completely broken under Unicorn:
 *
 *   1. The PRIMARY GUITextFieldImpl vtable (binary 0x7e1490) has corrupt
 *      entries that cause wild jumps to 0x2d6ce4c during drawApplication.
 *
 *   2. The SECONDARY ITextInputDelegate vtable (binary 0x7e1688) also
 *      has unrelocated function pointers.
 *
 * Our strategy: intercept the ENTIRE text input chain in SRE.
 *   - Hook StartTextInputWithDelegate → capture delegate, clear DAT_007f3ca8
 *   - Hook StopTextInputWithDelegate  → clear state
 *   - Hook textInputTextDidChange     → write text directly to fields
 *   - Hook textInputDidFinish         → clear editing state
 *   - Host maps RET-page at 0x2d6ce4c → catches primary vtable crashes
 *   - sre_init patches ITextInputDelegate vtable → no-op handlers
 *
 * GUITextFieldImpl layout (from Ghidra, v1.4.12 ARM64):
 *   +0x000  GUIView base class (primary vtable at +0x000)
 *   +0x140  ITextInputDelegate sub-object (secondary vtable)
 *   +0x148  GUILabel* shared_ptr data (display label)
 *   +0x188  SreString — current text
 *   +0x190  SreString — placeholder text
 *   +0x198  uint8_t — isEditing flag
 *   +0x199  uint8_t — cursor blink toggle
 *   +0x19C  float — blink timer
 *
 * Key addresses (nm -D, v1.4.12 ARM64):
 *   0x4792ac  StartTextInputWithDelegate (we hook this)
 *   0x4793dc  StopTextInputWithDelegate  (we hook this)
 *   0x4790dc  Java_..._textInputTextDidChange (we hook this)
 *   0x479290  Java_..._textInputDidFinish     (we hook this)
 *   0x7f3ca8  ITextInputDelegate* global (BSS — we clear this)
 * ===================================================================== */

/* BSS offset of the engine's global ITextInputDelegate* pointer */
#define TEXTINPUT_DELEGATE_OFFSET  0x6e9cb0

/* GUITextFieldImpl field offsets */
#define TEXTFIELD_BASE_ADJUST      0x140  /* delegate_ptr - this = base */
#define TEXTFIELD_LABEL_OFFSET     0x148  /* GUILabel* shared_ptr data */
#define TEXTFIELD_TEXT_OFFSET      0x188  /* SreString — current text */
#define TEXTFIELD_EDITING_OFFSET   0x198  /* uint8_t — isEditing */

/* GUILabel text offset */
#define GUILABEL_TEXT_OFFSET       0x100  /* SreString — label display text */

char g_sre_text_input_init_val[4096] = {0};

/* ---- OUR private delegate pointer ----
 * We store the delegate here instead of in DAT_007f3ca8.
 * This way the draw cycle's delegate vtable check sees NULL and skips it,
 * while our textInput hooks can still find the GUITextFieldImpl. */
static uint64_t g_sre_text_delegate = 0;

/* ---- StartTextInputWithDelegate (0x4792ac) ----
 * Called when the game opens a text field for editing.
 * Original: stores delegate in DAT_007f3ca8, calls JNI startTextInput.
 *
 * We intercept to:
 *   1. Capture the delegate in our own global
 *   2. Copy the initial text value to our host-visible buffer
 *   3. Clear DAT_007f3ca8 to prevent draw-cycle vtable dispatch
 *
 * Calling convention: X0 = ITextInputDelegate*, X1 = const std::string*
 */
void sre_StartTextInputWithDelegate(void* delegate, void* text) {
    /* Store delegate in OUR global */
    g_sre_text_delegate = (uint64_t)delegate;

    /* Signal the host that text input is now active */
    g_sre_text_input_active = 1;

    /* Copy initial text value to our host-visible buffer */
    g_sre_text_input_init_val[0] = '\0';
    if (text) {
        SreString* str = (SreString*)text;
        if (str && str->data) {
            strncpy(g_sre_text_input_init_val, str->data, sizeof(g_sre_text_input_init_val) - 1);
            g_sre_text_input_init_val[sizeof(g_sre_text_input_init_val) - 1] = '\0';
        }
    }

    /* Clear the engine's global — this is the KEY fix.
     * The draw cycle checks: if (DAT_007f3ca8 != 0) { dispatch through vtable }
     * By clearing it, no delegate vtable dispatch happens during rendering. */
    *(uint64_t*)(g_swordigo_base + TEXTINPUT_DELEGATE_OFFSET) = 0;
}

/* ---- StopTextInputWithDelegate (0x4793dc) ----
 * Called when text editing ends (ResignFirstResponder, StopEditing).
 *
 * Calling convention: X0 = ITextInputDelegate*
 */
void sre_StopTextInputWithDelegate(void* delegate) {
    (void)delegate;

    g_sre_text_delegate = 0;
    g_sre_text_input_active = 0;
    g_sre_text_input_init_val[0] = '\0';
    *(uint64_t*)(g_swordigo_base + TEXTINPUT_DELEGATE_OFFSET) = 0;
}

/* ---- textInputTextDidChange (0x4790dc) ----
 * Called by host when user types a character (SDL_EVENT_TEXT_INPUT).
 *
 * Calling convention: X0=env, X1=cls, X2=jstring (raw UTF-8 pointer)
 *
 * HOW THIS WORKS (verified from Ghidra):
 *   GUITextFieldImpl::TextInputTextDidChange(GUITextFieldImpl* this)
 *     - takes ONE arg: the GUITextFieldImpl* itself
 *     - reads the NEW text from this->text (offset +0x188)
 *     - does visual width validation, then fires SendActionsForControlEvent
 *
 *   So the correct approach is:
 *     1. Release old SreString at tf+0x188
 *     2. Assign new SreString there
 *     3. Call SendValueChangedEvent(tf) at verified offset 0x3161d1
 *        (which fires GUIControl::SendActionsForControlEvent with mask 0x1000)
 *
 *   We do NOT call TextInputTextDidChange directly because it does complex
 *   visual width checking that requires the engine's font metrics — calling
 *   it cross-ABI would crash (as seen: offset 0x316175 is actually
 *   PressureTriggerComponent::ByteSize, a completely unrelated function).
 *
 *   The max-length enforcement is handled by our host-side logic (the host
 *   reads g_sre_text_input_init_val after each call and clamps the buffer).
 */
void sre_textInputTextDidChange(void* env, void* cls, void* jstring_text) {
    (void)env; (void)cls;

    /* Use OUR delegate pointer (engine's is cleared) */
    uint64_t delegate = g_sre_text_delegate;
    if (!delegate) return;

    const char* text = (const char*)jstring_text;
    if (!text) return;

    /* Declare standard functions since they are not in sre.h */
    extern void* malloc(size_t size);
    extern void  free(void* ptr);
    extern size_t strlen(const char* s);
    extern char* strcpy(char* dest, const char* src);

    /* Create the input string std::string (SreString) on the heap */
    size_t in_len = strlen(text);
    SreStringRep* in_rep = (SreStringRep*)malloc(sizeof(SreStringRep) + in_len + 1);
    if (!in_rep) return;
    in_rep->length = in_len;
    in_rep->capacity = in_len;
    in_rep->refcount = 0;
    in_rep->_pad = 0;
    char* in_data = SRE_DATA(in_rep);
    strcpy(in_data, text);

    SreString in_str;
    in_str.data = in_data;

    /* Create the output string std::string (SreString).
     * Set to NULL to be completely safe for both placement new and copy assignment. */
    SreString out_str;
    out_str.data = NULL;

    /* Resolve virtual method TextInputTextDidChange from ITextInputDelegate vtable (offset 0) */
    uint64_t vtable = *(uint64_t*)delegate;
    uint64_t func_ptr = *(uint64_t*)vtable;

    /* CONSERVATIVE SAFETY GUARD (FIX7): the ITextInputDelegate vtable can be the
     * corrupt/unrelocated one (its patch is intentionally disabled), so dispatching
     * blindly here can jump the JIT into non-code (the documented ~0x2d6ce4c wild
     * jump). Validate the vtable pointer AND the resolved function pointer before
     * the blr; if either is bad, skip the dispatch entirely (safe fallback — the
     * text value is still synced to the host globals below). This does NOT enable
     * the disabled ITextInputDelegate vtable patch; it only refuses to branch into
     * garbage. */
    if (func_ptr && sre_is_valid_vtable_ptr(vtable) && sre_is_valid_code_ptr(func_ptr)) {
        register void* r_x0 __asm__("x0") = (void*)delegate;
        register void* r_x1 __asm__("x1") = &in_str;
        register void* r_x8 __asm__("x8") = &out_str;
        
        __asm__ volatile (
            "blr %3"
            : "+r" (r_x0), "+r" (r_x1), "+r" (r_x8)
            : "r" (func_ptr)
            : "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "memory"
        );
    }

    /* Re-sync global buffer → host reads this to track current text
     * (and detects if the engine enforced any length limit). */
    if (out_str.data) {
        strncpy(g_sre_text_input_init_val, out_str.data, sizeof(g_sre_text_input_init_val) - 1);
        g_sre_text_input_init_val[sizeof(g_sre_text_input_init_val) - 1] = '\0';
    } else {
        g_sre_text_input_init_val[0] = '\0';
    }

    /* Release temporary strings to prevent leaks */
    sre_CppString_release(&in_str);
    sre_CppString_release(&out_str);
}

/* ---- textInputDidFinish (0x479290) ----
 * Called by host when user presses Enter or Escape.
 *
 * Calling convention: X0=env, X1=cls
 */
void sre_textInputDidFinish(void* env, void* cls) {
    (void)env; (void)cls;

    uint64_t delegate = g_sre_text_delegate;
    if (!delegate) return;

    /* Call virtual TextInputDidFinish(delegate) dynamically from vtable */
    uint64_t vtable = *(uint64_t*)delegate;
    uint64_t func_ptr = *(uint64_t*)(vtable + 8);
    if (func_ptr) {
        typedef void (*pfn_VirtualTextInputDidFinish)(void* delegate);
        pfn_VirtualTextInputDidFinish func = (pfn_VirtualTextInputDidFinish)func_ptr;
        func((void*)delegate);
    }

    /* Fallback/safeguard in case it didn't clear them */
    g_sre_text_delegate = 0;
    g_sre_text_input_active = 0;
    g_sre_text_input_init_val[0] = '\0';
    *(uint64_t*)(g_swordigo_base + TEXTINPUT_DELEGATE_OFFSET) = 0;
}

/* =====================================================================
 * VTABLE DISPATCH HANDLERS — safety net for ITextInputDelegate
 * =====================================================================
 * sre_init() patches the ITextInputDelegate vtable (binary 0x7e1688)
 * to point to these handlers, as a safety net in case the draw cycle
 * still reaches the delegate vtable despite DAT_007f3ca8 being cleared.
 * ===================================================================== */

void sre_TextInputTextDidChange_vtable(void* out_str, void* delegate, void* text_str) {
    (void)delegate; (void)text_str;
    if (out_str) {
        sre_CppString_from_char_p((SreString*)out_str, "");
    }
}

void sre_TextInputDidFinish_vtable(void* delegate) {
    (void)delegate;
}

extern sre_u64 g_orig_GameOverlayView_SetControlsHidden;
extern int g_sre_controls_hidden;

void sre_GameOverlayView_SetControlsHidden(void* self, int hide) {
    int orig_hide = hide;
    if (g_sre_controls_hidden) {
        orig_hide = 0; // Show HUD elements (which includes touch buttons initially)
    }

    if (g_orig_GameOverlayView_SetControlsHidden) {
        typedef void (*pfn_SetControlsHidden)(void*, int);
        ((pfn_SetControlsHidden)g_orig_GameOverlayView_SetControlsHidden)(self, orig_hide);
    }

    if (g_sre_controls_hidden && self) {
        // Explicitly hide only the movement touch controls
        void* btn_left   = *(void**)((char*)self + 0x140);
        void* btn_right  = *(void**)((char*)self + 0x150);
        void* btn_swing  = *(void**)((char*)self + 0x160);
        void* btn_jump   = *(void**)((char*)self + 0x170);
        void* btn_dpad   = *(void**)((char*)self + 0x180);
        void* btn_spell  = *(void**)((char*)self + 400); // 0x190

        if (btn_left)  *(uint8_t*)((char*)btn_left + 0xE4) = 1;  // isHidden = true
        if (btn_right) *(uint8_t*)((char*)btn_right + 0xE4) = 1; // isHidden = true
        if (btn_swing) *(uint8_t*)((char*)btn_swing + 0xE4) = 1; // isHidden = true
        if (btn_jump)  *(uint8_t*)((char*)btn_jump + 0xE4) = 1;  // isHidden = true
        if (btn_dpad)  *(uint8_t*)((char*)btn_dpad + 0xE4) = 1;  // isHidden = true
        if (btn_spell) *(uint8_t*)((char*)btn_spell + 0xE4) = 1; // isHidden = true
    }
}
