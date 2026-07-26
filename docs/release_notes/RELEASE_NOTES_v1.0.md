# Swordigo Desktop v1.0.0 Release Notes — Full Desktop Experience

> The first full-featured desktop release. Transforms the ARM emulation proof-of-concept into a polished, playable desktop game with native GUI, audio, modding tools, and Linux packaging.

---

## Highlights
- **Native Desktop GUI**: Menu bar system with dropdown menus, settings panel, and About dialog.
- **Music & Audio**: OpenAL-powered WAV music playback with volume control.
- **Performance Boost**: Targeted Unicorn hooks eliminate per-instruction overhead.
- **1920x1080 Native Rendering**: Pixel-perfect output.
- **Keyboard Controls**: WASD movement, arrow key camera, F-key shortcuts.
- **Linux Packaging**: RPM, DEB, and AppImage with bundled game data.

---

## New Features

### GUI & Audio Systems
- Menu bar (`File`, `Emulation`, `Config`, `Settings`, `Help`).
- Modal settings panel with UI scaling (`< 125% >`) and game speed multipliers.
- OpenAL audio pipeline auto-loading music from `assets/resources/music/`.

### SwMini Modloader Integration
- Game speed controls (`+`/`-` keys, `0.25x` to `4.0x`).
- Free Camera controls (Arrow keys fly around scene, `Home` resets).
- Smooth camera mode toggle.

### Controls Quick Reference
| Key | Action |
|---|---|
| WASD | Move character |
| Backspace | Jump |
| Arrow Keys | Free camera |
| Home | Reset camera |
| +/- | Game speed |
| F1 | Toggle GUI |
| F3 | Debug overlay |
| F7 | Typing mode |
| F8 | Pause/Resume |
| F10 | Toggle on-screen controls |
