# Binary Selector + GUI Click Fix + Version Tracking

## Overview

1. **Boot-time binary selector** — scan for `libswordigo*.so`, show GUI to pick which to load
2. **Fix GUI click detection** — clicks are offset from visual elements (investigating now)
3. **SHA256-based version tracking** — JSON config stores known binary hashes, versions, test status
4. **F3 debug overlay** — show loaded binary name and Swordigo version
5. **Settings: change binary** — switch mid-game with save/restart prompt
6. **Settings: change graphics API** — same restart prompt for Vulkan switch

---

## Known Binary Hashes

| SHA256 | Version | File | Status |
|--------|---------|------|--------|
| `cee15dd2730746...` | v1.4.6 | libswordigo.so | ✅ Tested |
| `08d49dd6f7f863...` | v1.4.12 | libswordigo_test.so | 🧪 Testing |

---

## Proposed Changes

### 1. Binary Registry JSON

#### [NEW] `swordigo_binaries.json` (in user data dir `~/.local/share/swordigo-desktop/`)

```json
{
  "default": "libswordigo.so",
  "known_binaries": {
    "cee15dd2730746269ce5db97d150371ebbad1f41371c6a728f1bb7d045632138": {
      "filename": "libswordigo.so",
      "version": "1.4.6",
      "status": "tested",
      "label": "v1.4.6 (Stable)"
    },
    "08d49dd6f7f8639a4c59f290ff2bb79254accf710f530bf53c2fce1659191c9e": {
      "filename": "libswordigo_test.so",
      "version": "1.4.12",
      "status": "testing",
      "label": "v1.4.12 (Testing)"
    }
  }
}
```

---

### 2. Boot-time Binary Selector GUI

At startup, before `load_and_boot()`:
- Scan project root for `libswordigo*.so` files
- Compute SHA256 of each
- Look up in JSON registry (auto-add unknown ones as "unknown version")
- Show a GUI panel (same OpenGL style as our menus):

```
╔══════════════════════════════════════════════╗
║  🎮 SELECT GAME BINARY                      ║
╠══════════════════════════════════════════════╣
║                                              ║
║  ● libswordigo.so                            ║
║    v1.4.6 · TESTED · 4.4 MB                 ║
║    [Set as Default]                          ║
║                                              ║
║  ○ libswordigo_test.so                       ║
║    v1.4.12 · TESTING · 4.4 MB               ║
║    [Set as Default]                          ║
║                                              ║
║  ○ libswordigo_patched.so                    ║
║    Unknown Version · 4.4 MB                  ║
║                                              ║
║         [Launch Selected]  [Use Default]     ║
╚══════════════════════════════════════════════╝
```

- Rendered with our existing OpenGL GUI system (same style as mod menu)
- Default binary auto-launches after 5 seconds if no input
- "Set as Default" writes to JSON
- Only shown when multiple binaries exist

---

### 3. GUI Click Fix

> [!WARNING]
> Currently investigating — a subagent is auditing every render vs click coordinate. The issue is likely a Y-offset mismatch in the mod panel and settings panel row calculations. Fix will be applied to ALL panels.

---

### 4. F3 Debug Overlay Update

Add to the existing debug overlay (rendered in `main.cpp`):
```
Binary: libswordigo.so (v1.4.6, TESTED)
SHA256: cee15dd2...
```

---

### 5. Settings: Binary & Graphics Switching

Add to Settings panel:
```
── Engine ──
Game Binary    < v1.4.6 (Stable) >     ← clicking arrows cycles through detected binaries
Graphics API   < OpenGL >               ← OpenGL / Vulkan
```

When either is changed, show a restart prompt:
```
╔══════════════════════════════════╗
║  ⚠️ ENGINE RESTART REQUIRED     ║
║                                  ║
║  Changing binary/graphics API    ║
║  requires restarting the engine. ║
║                                  ║
║  Save your game first!           ║
║                                  ║
║    [Restart Now]  [Cancel]       ║
╚══════════════════════════════════╝
```

---

## Files to Modify

### [NEW] `src/platform/binary_selector.h` + `.cpp`
- SHA256 computation (using OpenSSL or manual implementation)
- JSON read/write for binary registry
- Binary scanning (find `libswordigo*.so` in data paths)
- Boot selector GUI rendering and click handling

### [MODIFY] `src/platform/gui.cpp`
- Fix click detection Y-offset bug across all panels
- Add Engine section to Settings panel (binary + graphics switching)
- Add restart prompt modal

### [MODIFY] `src/platform/gui.h`
- New GuiAction enums for binary/engine switching
- Restart prompt state

### [MODIFY] `src/main.cpp`
- Call binary selector before `load_and_boot()`
- Add binary info to F3 debug overlay
- Handle restart actions

---

## Verification Plan

### Manual Testing
- Place multiple `libswordigo*.so` files → verify selector appears at boot
- Click each option → verify correct binary loads
- Set default → verify it auto-loads on next launch
- Check F3 overlay → verify binary info shown
- Change binary in Settings → verify restart prompt
- Test GUI click accuracy on ALL panels after fix
