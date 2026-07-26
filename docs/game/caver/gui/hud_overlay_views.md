# Caver HUD & Overlay Views Documentation

## 1. System Overview & Purpose

The HUD and overlay view system provides real-time gameplay feedback to the player. It includes the main HUD overlay (`GameOverlayView`), health heart container bar (`HealthBar`), mana meter (`ManaBar`), soul coin bar (`CoinBar`), active inventory panel (`InventoryItemPanel`), dialog bubbles (`GUIBubbleView`), and alert overlays (`GUIAlertView`).

This document details the view layouts, data bindings, animation transitions, and overlay event dispatches for the C++ PC rewrite.

---

## 2. Namespace & Class Structure (`Caver::*`)

```
Caver::GUIView
 ├── Caver::GameOverlayView (Master Gameplay HUD View Container)
 ├── Caver::HealthBar (Heart Container Display & Damage Anim Container)
 ├── Caver::ManaBar (Mana Bar Container & Recharge Glow Animation)
 ├── Caver::CoinBar (Soul Coin Counter & Pickup Count Ticker)
 ├── Caver::ItemOverlay (Active Spell & Sword Quick-Slot View)
 ├── Caver::InventoryItemPanel (Pause Menu Full Inventory Grid)
 ├── Caver::GUIBubbleView (NPC Text Dialog & Choice Popup Container)
 └── Caver::GUIAlertView (Achievement & Item Pickup Notification View)
```

---

## 3. Gameplay HUD Architecture (`GameOverlayView`)

```
+-------------------------------------------------------------------------+
| [HealthBar: Heart Containers]     [ItemOverlay: Magic]  [Pause Button]  |
| [ManaBar: Blue Meter]             [ItemOverlay: Sword]                  |
| [CoinBar: Coin Icon + Count]                                            |
|                                                                         |
|                                                                         |
|                                                                         |
| [Virtual Joystick / Movement]           [Action: Attack] [Action: Jump] |
|                                         [Spell Button]   [Dimension]    |
+-------------------------------------------------------------------------+
```

### Key UI View Components Specifications:
1. **HealthBar**:
   - Renders player hearts (each heart represents 4 quarter-health increments).
   - Listens to `HealthComponent::OnHealthChanged` events. When damage is taken, heart icons trigger a shake animation and flash red.
2. **ManaBar**:
   - Displays current mana out of total max mana.
   - Smoothly fills back up when mana regenerates over time.
3. **ItemOverlay**:
   - Displays currently equipped sword (e.g. Master Sword, Mage Blade) and active spell (e.g. Magic Bolt, Magic Bomb, Hookshot).
   - Tapping quick-slots opens the `SkillPickerView` or `InventoryView`.
4. **GUIBubbleView**:
   - Renders NPC text speech bubbles anchored directly to NPC spatial coordinates transformed to screen space:
     $$\vec{P}_{\text{screen}} = \text{WorldToScreen}(\vec{P}_{\text{npc\_world}} + (0, \Delta y_{\text{bubble}}, 0))$$

---

## 4. Reverse Engineering & Tools Integration Notes

- **Native SDK Integration**: On desktop PC port builds, virtual joystick elements (`VirtualJoystick`, touch buttons) are hidden, replaced with clean desktop keybind hints.
- **SwKiWi API Modding**: SwKiWi exposes `GameOverlayView::RegisterCustomWidget`, enabling custom UI elements (like speedrun timers, FPS counters, or extra stat bars).

---

## 5. PC Port (`swd`) Implementation Strategy

1. **Auto-Hiding Touch Controls**: Conditionally disable virtual touch overlay buttons when keyboard or controller input is detected.
2. **Smooth Heart & Mana Fill Interpolation**: Animate health heart changes and mana meter fills using lerp transitions rather than instant value jumps.
3. **Resizable Scale Profiles**: Allow players to scale HUD elements ($80\%, 100\%, 120\%$) to suitUltrawide or high-DPI desktop display screens.
