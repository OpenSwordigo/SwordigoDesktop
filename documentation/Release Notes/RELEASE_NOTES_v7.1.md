# Swordigo Desktop v7.1 — Release Notes
<img width="929" height="247" alt="swordigo_desktop_text" src="https://github.com/user-attachments/assets/35a7e21c-41a3-45ae-85c0-73928260a303" />

**Release Date:** June 25, 2026  
**Codename:** *hot-fix*  
**Tag:** `v7.1`  

---

## 🆕 What's New

This is a hotfix release focusing on resolving critical crashes and usability issues introduced in v7.0 when playing user-imported custom modded instances (specifically with `RLSwordigo` game type).

### 🛠️ Custom Instance Compatibility Fixes

- **SRE Compatibility in Custom Directories**: Fixed a bug where `libsre.so` loading was bypassed for custom instances. The runtime now checks for `custom-` prefixes in the library directory path. This ensures that the required guest-side hooks are successfully loaded, preventing atomic spin-loop hangs and empty `.POD` crashes.
- **Automatic SRE Dependency Registration**: The launcher now automatically copies and registers `libsre.so` dependency paths when importing a custom instance with SRE enabled.
- **Config Persistence Merging**: Restructured the duplicate check logic in the launcher's metadata scanner. Settings loaded from `instances.json` (such as `game_type` and `assets_dir`) now correctly merge and override the filesystem-scanned attributes instead of being discarded on launcher restart.

### 📚 Documentation Cleanup

- **Removed ARM64 Limitations**: Officially marked the ARM64 emulation target as having no major known limitations.
- Removed references to:
  - **Bolt/timer misbehavior**: Projectile firing rates and boss timing alignment have been fully stabilized.
  - **Text input crash**: Intercepted by the SRE input thunk layer, allowing safe character/label transitions.

---

*Powered by the Swordigo Runtime (SRT) v7.1*
