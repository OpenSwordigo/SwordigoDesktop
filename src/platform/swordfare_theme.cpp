#include "platform/swordfare_theme.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <cfloat>
#include <cmath>

namespace sf_theme {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Multiply the alpha channel of a packed ImU32 color by `a` (0..1).
ImU32 WithAlphaMul(ImU32 col, float a) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
    c.w *= a;
    return ImGui::ColorConvertFloat4ToU32(c);
}

// Brighten an RGB color toward white by `amount` (0..1), preserving alpha.
ImU32 Brighten(ImU32 col, float amount) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
    c.x += (1.0f - c.x) * amount;
    c.y += (1.0f - c.y) * amount;
    c.z += (1.0f - c.z) * amount;
    return ImGui::ColorConvertFloat4ToU32(c);
}

} // namespace

// ---------------------------------------------------------------------------
// Color lerp
// ---------------------------------------------------------------------------
ImU32 Lerp(ImU32 a, ImU32 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    ImVec4 r = ImLerp(ca, cb, t);
    return ImGui::ColorConvertFloat4ToU32(r);
}

// ---------------------------------------------------------------------------
// Animation helpers
// ---------------------------------------------------------------------------
float Pulse(float speed) {
    const float t = static_cast<float>(ImGui::GetTime());
    return 0.5f + 0.5f * std::sin(t * speed * 2.0f * IM_PI);
}

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------
void ApplyTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();

    // Base on the dark style, then override everything below.
    ImGui::StyleColorsDark(&style);

    // Shape / layout.
    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 10.0f;
    style.FrameRounding     = 8.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 10.0f;
    style.GrabRounding      = 8.0f;
    style.TabRounding       = 8.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize  = 1.0f;
    style.PopupBorderSize  = 1.0f;
    style.FrameBorderSize  = 1.0f;
    style.TabBorderSize    = 0.0f;

    style.WindowPadding    = ImVec2(16.0f, 16.0f);
    style.FramePadding     = ImVec2(14.0f, 8.0f);
    style.CellPadding      = ImVec2(8.0f, 6.0f);
    style.ItemSpacing      = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing    = 22.0f;
    style.ScrollbarSize    = 12.0f;
    style.GrabMinSize      = 10.0f;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign  = ImVec2(0.5f, 0.5f);

    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.Alpha = 1.0f;

    // Colors.
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = Vec_Text;
    c[ImGuiCol_TextDisabled]          = Vec_TextMuted;
    c[ImGuiCol_WindowBg]              = Vec_Bg;
    c[ImGuiCol_ChildBg]               = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.071f, 0.086f, 0.110f, 0.980f);
    c[ImGuiCol_Border]                = Vec_PanelBorder;
    c[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.106f, 0.129f, 0.161f, 0.700f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.145f, 0.176f, 0.216f, 0.850f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.176f, 0.212f, 0.259f, 0.950f);
    c[ImGuiCol_TitleBg]               = Vec_Bg;
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.086f, 0.106f, 0.133f, 1.000f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.055f, 0.067f, 0.086f, 0.750f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.071f, 0.086f, 0.110f, 1.000f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.212f, 0.247f, 0.294f, 0.800f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.286f, 0.325f, 0.376f, 0.900f);
    c[ImGuiCol_ScrollbarGrabActive]   = Vec_Accent;
    c[ImGuiCol_CheckMark]             = Vec_AccentAlt;
    c[ImGuiCol_SliderGrab]            = Vec_Accent;
    c[ImGuiCol_SliderGrabActive]      = Vec_AccentAlt;
    c[ImGuiCol_Button]                = ImVec4(0.145f, 0.176f, 0.216f, 0.800f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.192f, 0.231f, 0.282f, 0.950f);
    c[ImGuiCol_ButtonActive]          = Vec_Accent;
    c[ImGuiCol_Header]                = ImVec4(0.424f, 0.361f, 0.906f, 0.320f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.424f, 0.361f, 0.906f, 0.500f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.424f, 0.361f, 0.906f, 0.750f);
    c[ImGuiCol_Separator]             = Vec_PanelBorder;
    c[ImGuiCol_SeparatorHovered]      = Vec_AccentDim;
    c[ImGuiCol_SeparatorActive]       = Vec_Accent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.424f, 0.361f, 0.906f, 0.250f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.424f, 0.361f, 0.906f, 0.550f);
    c[ImGuiCol_ResizeGripActive]      = Vec_Accent;
    c[ImGuiCol_Tab]                   = ImVec4(0.086f, 0.106f, 0.133f, 1.000f);
    c[ImGuiCol_TabHovered]            = Vec_AccentDim;
    c[ImGuiCol_TabActive]             = ImVec4(0.145f, 0.176f, 0.216f, 1.000f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.071f, 0.086f, 0.110f, 1.000f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.106f, 0.129f, 0.161f, 1.000f);
    c[ImGuiCol_PlotLines]             = Vec_AccentAlt;
    c[ImGuiCol_PlotLinesHovered]      = Vec_Accent;
    c[ImGuiCol_PlotHistogram]         = Vec_Accent;
    c[ImGuiCol_PlotHistogramHovered]  = Vec_AccentAlt;
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.086f, 0.106f, 0.133f, 1.000f);
    c[ImGuiCol_TableBorderStrong]     = Vec_PanelBorder;
    c[ImGuiCol_TableBorderLight]      = ImVec4(1.0f, 1.0f, 1.0f, 0.080f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.0f, 1.0f, 1.0f, 0.030f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.424f, 0.361f, 0.906f, 0.400f);
    c[ImGuiCol_DragDropTarget]        = Vec_AccentAlt;
    c[ImGuiCol_NavHighlight]          = Vec_Accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.700f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.055f, 0.067f, 0.086f, 0.600f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.020f, 0.031f, 0.043f, 0.700f);

    // Scale everything (fonts are handled by the caller / atlas).
    if (scale > 0.0f && scale != 1.0f)
        style.ScaleAllSizes(scale);
    else
        style.ScaleAllSizes(1.0f);
}

// ---------------------------------------------------------------------------
// Draw-list primitives
// ---------------------------------------------------------------------------
void GlassPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                ImU32 fill, ImU32 border) {
    if (!dl) return;
    if (fill == 0) fill = Col_Panel;
    if (border == 0) border = Col_PanelBorder;

    // Base frosted fill: a soft vertical gradient makes it read as glass.
    const ImU32 fillTop = Brighten(fill, 0.06f);
    const ImU32 fillBot = fill;
    dl->AddRectFilledMultiColor(min, max, fillTop, fillTop, fillBot, fillBot);

    // Subtle 1px top-highlight line, inset slightly from the corners.
    const float inset = rounding * 0.5f;
    const ImU32 highlight = WithAlphaMul(0xFFFFFFFF, 0.14f);
    dl->AddLine(ImVec2(min.x + inset, min.y + 1.0f),
                ImVec2(max.x - inset, min.y + 1.0f), highlight, 1.0f);

    // Soft border.
    dl->AddRect(min, max, border, rounding, 0, 1.0f);
}

void VerticalGradient(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 top,
                      ImU32 bottom, float rounding) {
    if (!dl) return;
    if (rounding <= 0.0f) {
        dl->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
        return;
    }

    // AddRectFilledMultiColor does not support rounding, so clip a rounded
    // rect and paint the gradient inside it.
    dl->PushClipRect(min, max, true);
    dl->AddRectFilled(min, max, top, rounding); // rounded silhouette
    dl->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
    dl->PopClipRect();
}

void AccentBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float t) {
    if (!dl) return;
    t = t - std::floor(t); // wrap to 0..1

    // Shimmer: shift the blend point across the bar based on t.
    const float phase = 0.5f + 0.5f * std::sin(t * 2.0f * IM_PI);
    const ImU32 left  = Col_Accent;
    const ImU32 right = Col_AccentAlt;
    const ImU32 mid   = Lerp(left, right, phase);

    const float xmid = ImLerp(min.x, max.x, 0.5f);
    // Two-segment gradient meeting at a moving-tint midpoint.
    dl->AddRectFilledMultiColor(min, ImVec2(xmid, max.y), left, mid, mid, left);
    dl->AddRectFilledMultiColor(ImVec2(xmid, min.y), max, mid, right, right, mid);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
namespace {

// Resolve the final button size: honor an explicit size, otherwise auto-size
// from the label like ImGui::Button (CalcTextSize + FramePadding * 2).
ImVec2 ResolveButtonSize(const char* label, const ImVec2& size) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 out = size;
    if (out.x <= 0.0f) out.x = labelSize.x + style.FramePadding.x * 2.0f;
    if (out.y <= 0.0f) out.y = labelSize.y + style.FramePadding.y * 2.0f;
    return out;
}

} // namespace

bool GhostButton(const char* label, const ImVec2& size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 btnSize = ResolveButtonSize(label, size);

    // InvisibleButton handles layout, ID, and interaction like a real button.
    const bool clicked = ImGui::InvisibleButton(label, btnSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();

    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + btnSize.x, pos.y + btnSize.y);
    const float rounding = ImGui::GetStyle().FrameRounding;

    ImDrawList* dl = window->DrawList;

    // Flat by default; accent-tinted fill on hover/active.
    if (held) {
        dl->AddRectFilled(min, max, WithAlphaMul(Col_Accent, 0.28f), rounding);
        dl->AddRect(min, max, WithAlphaMul(Col_Accent, 0.55f), rounding, 0, 1.0f);
    } else if (hovered) {
        dl->AddRectFilled(min, max, WithAlphaMul(Col_Accent, 0.16f), rounding);
        dl->AddRect(min, max, WithAlphaMul(Col_Accent, 0.35f), rounding, 0, 1.0f);
    }

    // Centered label.
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 textPos(
        min.x + (btnSize.x - labelSize.x) * 0.5f,
        min.y + (btnSize.y - labelSize.y) * 0.5f);
    const ImU32 textCol = hovered ? Col_Text : Col_TextDim;
    dl->AddText(textPos, textCol, label);

    return clicked;
}

bool PrimaryButton(const char* label, const ImVec2& size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 btnSize = ResolveButtonSize(label, size);

    const bool clicked = ImGui::InvisibleButton(label, btnSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();

    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + btnSize.x, pos.y + btnSize.y);
    const float rounding = ImGui::GetStyle().FrameRounding;

    ImDrawList* dl = window->DrawList;

    // Accent gradient fill (left -> right), brightened on hover/active.
    float brighten = 0.0f;
    if (held)         brighten = 0.14f;
    else if (hovered) brighten = 0.08f;

    const ImU32 gl = Brighten(Col_Accent, brighten);
    const ImU32 gr = Brighten(Col_AccentAlt, brighten);

    dl->AddRectFilled(min, max, gl, rounding); // rounded base silhouette
    dl->PushClipRect(min, max, true);
    dl->AddRectFilledMultiColor(min, max, gl, gr, gr, gl);
    dl->PopClipRect();

    // Soft top highlight for depth.
    dl->AddLine(ImVec2(min.x + rounding, min.y + 1.0f),
                ImVec2(max.x - rounding, min.y + 1.0f),
                WithAlphaMul(0xFFFFFFFF, 0.22f), 1.0f);

    // Centered white label.
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 textPos(
        min.x + (btnSize.x - labelSize.x) * 0.5f,
        min.y + (btnSize.y - labelSize.y) * 0.5f);
    dl->AddText(textPos, 0xFFFFFFFF, label);

    return clicked;
}

void SectionHeader(const char* text) {
    if (!text) return;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    // Build an uppercase, letter-spaced copy of the label.
    char buf[256];
    int n = 0;
    for (const char* p = text; *p && n < static_cast<int>(sizeof(buf)) - 2; ++p) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - ('a' - 'A'));
        buf[n++] = ch;
        // Insert a thin space between characters for letter-spacing, but not
        // after the final glyph.
        if (p[1] != '\0' && n < static_cast<int>(sizeof(buf)) - 2)
            buf[n++] = ' ';
    }
    buf[n] = '\0';

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pos = window->DC.CursorPos;

    // Small dimmed header text.
    const float fontSize = ImGui::GetFontSize() * 0.82f;
    ImDrawList* dl = window->DrawList;
    dl->AddText(ImGui::GetFont(), fontSize, pos, Col_TextMuted, buf);

    const ImVec2 textSize =
        ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf);

    // Thin accent rule under the header.
    const float ruleY = pos.y + textSize.y + 3.0f;
    const float ruleW = ImMax(textSize.x, 28.0f);
    dl->AddRectFilledMultiColor(
        ImVec2(pos.x, ruleY),
        ImVec2(pos.x + ruleW, ruleY + 2.0f),
        Col_Accent, Col_AccentAlt, Col_AccentAlt, Col_Accent);

    // Advance the ImGui cursor to reserve the space we drew into.
    const ImVec2 total(ImMax(textSize.x, ruleW),
                       textSize.y + 5.0f + style.ItemSpacing.y);
    ImGui::Dummy(total);
}

void Spinner(const char* id, float radius, int thickness, ImU32 color) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID widgetId = window->GetID(id);

    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 sz(radius * 2.0f, (radius + style.FramePadding.y) * 2.0f);
    const ImRect bb(pos, ImVec2(pos.x + sz.x, pos.y + sz.y));
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, widgetId)) return;

    ImDrawList* dl = window->DrawList;
    const ImVec2 center(pos.x + radius, pos.y + radius + style.FramePadding.y);

    // Drive rotation & arc length purely from GetTime() so it animates every
    // frame with no persistent mutable state.
    const float time = static_cast<float>(ImGui::GetTime());
    const int   numSegments = 30;
    const float speed = 8.0f;

    // Animated sweep: the arc grows and shrinks while the whole thing spins.
    const float t = time * speed;
    const float start =
        std::fabs(std::sin(t) * static_cast<float>(numSegments - 5));
    const float aMin = IM_PI * 2.0f * (start) / static_cast<float>(numSegments);
    const float aMax = IM_PI * 2.0f *
                       (static_cast<float>(numSegments) - 3.0f) /
                       static_cast<float>(numSegments);
    const float rot = t;

    dl->PathClear();
    for (int i = 0; i <= numSegments; ++i) {
        const float a =
            aMin + (static_cast<float>(i) / static_cast<float>(numSegments)) *
                       (aMax - aMin);
        dl->PathLineTo(ImVec2(center.x + std::cos(a + rot) * radius,
                              center.y + std::sin(a + rot) * radius));
    }
    dl->PathStroke(color, ImDrawFlags_None, static_cast<float>(thickness));
}

} // namespace sf_theme
