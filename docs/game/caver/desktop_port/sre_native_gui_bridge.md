# Caver Native GUI Bridge & Resolution Scaling Documentation

## 1. System Overview & Purpose

The Native GUI Bridge (`src/sre/sre_gui_native.c`, `sre_gui.c`) in `SwordigoDesktop` bridges mobile touch overlay GUI views (`GUIWindow`, `GUIView`, `GUIButton`) to desktop PC mouse pointer clicks, keyboard navigation, and variable aspect ratio monitor displays.

This document details logical coordinate mapping ($960 \times 640$ native design coordinate system), aspect ratio pillarboxing/letterboxing, high-DPI font scaling factors, and touch overlay auto-hiding for the C++ PC rewrite.

---

## 2. Namespace & Architecture

```
SwordigoDesktop::GUI
 ├── SRE_GUINative (Master Native GUI Bridge & Coordinate Converter)
 ├── AspectRatioEngine (Pillarbox / Letterbox Viewport Scaling Calculator)
 └── InputTranslator (Mouse & Gamepad Focus to GUIResponder Event Translator)
```

---

## 3. Coordinate Translation & Viewport Calculation

Mobile Swordigo was authored around a native design coordinate resolution of $960 \times 640$ ($3:2$ aspect ratio). On desktop PC displays ($16:9$, $16:10$, $21:9$, $32:9$), `SRE_GUINative` projects logical UI coordinates to target window screen coordinates:

```mermaid
flowchart TD
    A[Window Dimensions: W_win x H_win] --> B[Calculate Target Aspect Ratio: R_win = W_win / H_win]
    B --> C{R_win == Design Aspect Ratio (1.5)?}
    C -->|Yes| D[Direct Scale: ScaleFactor = W_win / 960.0]
    C -->|No| E[Calculate Letterbox / Pillarbox Offsets]
    E --> F[Compute Viewport Rect: X_offset, Y_offset, W_view, H_view]
    F --> G[Map Mouse Click (X_mouse, Y_mouse) -> Logical (X_log, Y_log)]
    G --> H[Dispatch Event to GUIWindow::HitTest(X_log, Y_log)]
```

### Mathematical Coordinate Transformation Formula
Given desktop mouse position $(X_{\text{mouse}}, Y_{\text{mouse}})$ and viewport rect $(X_{\text{offset}}, Y_{\text{offset}}, W_{\text{view}}, H_{\text{view}})$:

$$X_{\text{logical}} = \frac{X_{\text{mouse}} - X_{\text{offset}}}{W_{\text{view}}} \cdot 960.0$$

$$Y_{\text{logical}} = \frac{Y_{\text{mouse}} - Y_{\text{offset}}}{H_{\text{view}}} \cdot 640.0$$

---

## 4. Desktop Resolution Scaling Profiles

| Screen Resolution Name | Display Pixels | Scale Factor ($\times$) | Aspect Ratio | Layout Strategy |
| :--- | :--- | :--- | :--- | :--- |
| **720p HD** | $1280 \times 720$ | $1.125\times$ | $16:9$ | Pillarbox sides ($64\text{px}$ left/right). |
| **1080p Full HD** | $1920 \times 1080$ | $1.6875\times$ | $16:9$ | Pillarbox sides ($96\text{px}$ left/right). |
| **1440p QHD** | $2560 \times 1440$ | $2.25\times$ | $16:9$ | Pillarbox sides ($128\text{px}$ left/right). |
| **4K Ultra HD** | $3840 \times 2160$ | $3.375\times$ | $16:9$ | Pillarbox sides ($192\text{px}$ left/right). |
| **Ultrawide** | $3440 \times 1440$ | $2.25\times$ | $21:9$ | Expanded HUD edges / Pillarbox center. |

---

## 5. Reverse Engineering & Tools Integration Notes

- **SwKiWi Integration**: SwKiWi's `Mini.SetControlsHidden(true)` is automatically executed by `SRE_GUINative` on desktop PC builds when physical mouse or gamepad input is detected.
- **Native SDK Reference**: Replaces iOS/Android touch driver calls with desktop mouse and keyboard event dispatches.

---

## 6. PC Port (`swd`) Implementation Strategy

1. **Ultrawide HUD Anchoring**: Anchor HUD elements (Health Bar, Mana Bar, Coin Counter) to the outer screen corners of ultrawide displays ($21:9$/$32:9$) rather than forcing pillarbox constraints on the HUD layer.
2. **Sub-Pixel Cursor Smoothing**: Render desktop mouse cursors at native display resolution ($60\text{Hz} \to 240\text{Hz}$) to eliminate UI cursor lag.
