#pragma once

#include "imgui/imgui.h"
#include <cstdint>

// SwordigoDesktop shared GUI design-system.
//
// This module is the single visual foundation consumed by the launcher, the
// loading screen, and the in-game overlays. It is pure Dear ImGui drawing:
// no SDL, no OpenGL, no platform coupling, so it links cleanly into every GUI
// target in the project.
namespace sf_theme {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
// ImU32 tokens are packed 0xAABBGGRR (ImGui's native draw-list color format),
// so they can be handed straight to ImDrawList calls. ImVec4 tokens are the
// normalized RGBA variants for use with ImGui::PushStyleColor / style editing.

// Packed ABGR color literals for the draw list.
inline constexpr ImU32 Col_Bg          = 0xFF16110E; // deep near-black #0E1116
inline constexpr ImU32 Col_Panel       = 0x99221B16; // elevated glass panel #161B22, low alpha
inline constexpr ImU32 Col_PanelBorder = 0x33FFFFFF; // soft light border
inline constexpr ImU32 Col_Accent      = 0xFFE75C6C; // vivid indigo/violet #6C5CE7
inline constexpr ImU32 Col_AccentAlt   = 0xFFEED322; // cyan #22D3EE
inline constexpr ImU32 Col_AccentDim   = 0x66E75C6C; // dim accent, ~40% alpha
inline constexpr ImU32 Col_Text        = 0xFFF3EDE6; // text primary #E6EDF3
inline constexpr ImU32 Col_TextDim     = 0xFFA5988B; // text secondary #8B98A5
inline constexpr ImU32 Col_TextMuted   = 0xFF70655B; // text muted #5B6570
inline constexpr ImU32 Col_Success     = 0xFF5EC522; // success #22C55E
inline constexpr ImU32 Col_Warning     = 0xFF0B9EF5; // warning #F59E0B
inline constexpr ImU32 Col_Danger      = 0xFF4444EF; // danger #EF4444
inline constexpr ImU32 Col_Overlay     = 0xB30A0806; // scrim overlay, ~70% alpha

// Normalized ImVec4 equivalents (RGBA 0..1) for ImGuiStyle work.
inline const ImVec4 Vec_Bg          = ImVec4(0.055f, 0.067f, 0.086f, 1.000f);
inline const ImVec4 Vec_Panel       = ImVec4(0.086f, 0.106f, 0.133f, 0.600f);
inline const ImVec4 Vec_PanelBorder = ImVec4(1.000f, 1.000f, 1.000f, 0.200f);
inline const ImVec4 Vec_Accent      = ImVec4(0.424f, 0.361f, 0.906f, 1.000f); // #6C5CE7
inline const ImVec4 Vec_AccentAlt   = ImVec4(0.133f, 0.827f, 0.933f, 1.000f); // #22D3EE
inline const ImVec4 Vec_AccentDim   = ImVec4(0.424f, 0.361f, 0.906f, 0.400f);
inline const ImVec4 Vec_Text        = ImVec4(0.902f, 0.929f, 0.953f, 1.000f); // #E6EDF3
inline const ImVec4 Vec_TextDim     = ImVec4(0.545f, 0.596f, 0.647f, 1.000f); // #8B98A5
inline const ImVec4 Vec_TextMuted   = ImVec4(0.357f, 0.396f, 0.439f, 1.000f); // #5B6570
inline const ImVec4 Vec_Success     = ImVec4(0.133f, 0.773f, 0.369f, 1.000f); // #22C55E
inline const ImVec4 Vec_Warning     = ImVec4(0.961f, 0.620f, 0.043f, 1.000f); // #F59E0B
inline const ImVec4 Vec_Danger      = ImVec4(0.937f, 0.267f, 0.267f, 1.000f); // #EF4444

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------

// Applies the full ImGuiStyle: rounding, padding, spacing, borders, and the
// entire ImGuiCol_ array derived from the palette above. Ends with
// ScaleAllSizes(scale) so callers can drive DPI/UI scaling in one place.
void ApplyTheme(float scale = 1.0f);

// ---------------------------------------------------------------------------
// Draw-list primitives
// ---------------------------------------------------------------------------

// Frosted glass card: semi-transparent fill, a subtle 1px top-highlight line,
// and a soft border. Passing 0 for fill/border falls back to Col_Panel /
// Col_PanelBorder.
void GlassPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding = 12.0f,
                ImU32 fill = 0, ImU32 border = 0);

// Filled vertical gradient rectangle (top -> bottom).
void VerticalGradient(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 top,
                      ImU32 bottom, float rounding = 0.0f);

// Horizontal accent gradient bar (Col_Accent -> Col_AccentAlt), usable as a
// progress fill. `t` in 0..1 shifts the gradient phase for a subtle shimmer.
void AccentBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float t);

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

// Flat, borderless button with a subtle accent-tinted hover/active fill.
// Behaves like ImGui::Button for layout and ID purposes.
bool GhostButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Filled accent-gradient button with white text and hover brighten.
// Behaves like ImGui::Button for layout and ID purposes.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Uppercase, letter-spaced, dimmed small header with a thin accent rule below.
void SectionHeader(const char* text);

// ---------------------------------------------------------------------------
// Animation helpers
// ---------------------------------------------------------------------------

// Animated arc spinner driven purely by ImGui::GetTime().
void Spinner(const char* id, float radius, int thickness, ImU32 color);

// Returns a 0..1 sine pulse from ImGui::GetTime(), for animating glows.
float Pulse(float speed = 1.0f);

// Per-channel color lerp between two packed ImU32 colors.
ImU32 Lerp(ImU32 a, ImU32 b, float t);

} // namespace sf_theme
