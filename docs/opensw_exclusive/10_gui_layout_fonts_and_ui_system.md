# Swordigo OpenSwordigo Research: GUI Layout Engine, Typography & Font Rendering

## 1. GUI Layout Engine Schema (`GUIViewLayout` & `GUIMargins`)

The touch user interface (HUD, inventory grid, dialogues, action buttons) uses anchored view hierarchies layout out via margins.

```cpp
namespace Caver {

struct GUIMargins {
    float left   = 0.0f; // Tag 0x0D
    float right  = 0.0f; // Tag 0x0F
    float bottom = 0.0f; // Tag 0x1D
    float top    = 0.0f; // Tag 0x25
};

struct GUIViewLayout {
    std::string identifier;            // Tag 0x0A
    GUIMargins margins;                // Tag 0x1A
    std::vector<GUIViewLayout> subviews; // Tag 0x12

    Rectangle CalculateBounds(const Rectangle& parent_screen) const {
        return Rectangle(
            parent_screen.x + margins.left,
            parent_screen.y + margins.top,
            parent_screen.width - margins.left - margins.right,
            parent_screen.height - margins.top - margins.bottom
        );
    }
};

} // namespace Caver
```

---

## 2. Typography & Texture Font Glyph System (`Font` & `Font_Glyph`)

Bitmap font rendering maps character codes to texture coordinates and horizontal advance distances.

```cpp
namespace Caver {

struct Font_Glyph {
    uint32_t char_code = 0;   // Tag 0x08
    Rectangle draw_bounds;    // Tag 0x12 (Render Quad Offset)
    float horiz_advance = 0.0f;// Tag 0x18 (Cursor Advance)
    Rectangle texture_bounds; // Tag 0x22 (Atlas Texture UV Rect)
};

class Font {
public:
    std::string name;          // Tag 0x0A
    float height = 0.0f;       // Tag 0x1C
    Rectangle bounding_box;    // Tag 0x20
    std::vector<uint8_t> kerning_table; // Tag 0x16

    std::unordered_map<uint32_t, Font_Glyph> glyphs; // Tag 0x1A

    void AddGlyph(const Font_Glyph& glyph) {
        glyphs[glyph.char_code] = glyph;
    }

    const Font_Glyph* GetGlyph(uint32_t code) const {
        auto it = glyphs.find(code);
        if (it != glyphs.end()) return &it->second;
        return nullptr;
    }
};

} // namespace Caver
```
