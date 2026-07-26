# SwordigoDesktop vs Swordigo Master's Curse (SWMC) Compatibility Audit

This document is an accidental compatibility audit of SwordigoDesktop vs Swordigo Master's Curse (SWMC).

The big picture: SWMC boots and is partially playable, but SwordigoDesktop/SRE still lacks several Kiwi/libmini compatibility paths and Android filesystem semantics that the mod expects. You tested Florennum boss, Overseer, Fire boss, and Icy. Overseer and Shadow apparently have no special modifications, so the useful failures are mainly camera behavior, model tinting, resources, gamepass hooks, and virtual filesystem routing.

---

## 1. Dev Conversation Summary

SWMC currently runs far enough on SwordigoDesktop to load scenes and bosses, which is a pretty huge compatibility milestone considering mod support was never part of the original SwordigoDesktop plan 💀.

The main discovered issues are:

* **CameraController compatibility is incomplete for mods**:
  SwordigoDesktop already has camera-controller functionality and keyboard mapping. Render-distance problems were also fixed so CameraController mode can load the full world. However, these APIs are not yet exposed/mapped to mod Lua, so SWMC camera angles and camera shake sequences do not execute correctly.
* **The "THINK FAST" behavior needs verification**:
  Redstell asked whether the attack speeds up the game. This was not conclusively verified in the conversation.
* **ModelComponent RGBA modification is broken or incomplete**:
  One Fire boss is supposed to become green through an RGBA override roughly equivalent to r=0, g=1, b=0. The boss remains incorrectly colored, suggesting missing or incorrect ModelComponent color handling.
* **_G.Game initialization/order is causing Lua problems**:
  The green ground mesh appearing in the menu is reportedly related to `_G.Game`. This matches the separate `hiro.scl` issue you found: scripts may execute in contexts such as `hero.scene` where `_G.Game` does not exist, causing early returns or incorrect initialization.
* **Android res/ compatibility is missing**:
  SWMC uses Kiwi ButtonController with custom button sprites from Android resources. SwordigoDesktop does not technically have an APK `res/` directory, package name, classes.dex, or normal Android resource subsystem.
* **The text font is incorrect**:
  Redstell suspects SWMC obtains an OTF font through `res/`. This likely belongs to the same Android resource compatibility problem.
* **Gamepass/IPA hooks need per-mod control**:
  SWMC should be able to disable SwordigoDesktop's automatic compass and other gamepass-like unlock hooks. The proposed solution is a configuration option in `instance.ini` or possibly mod metadata such as `mini.toml`.
* **Lua terminal needs optional restriction for mods**:
  The Lua terminal currently remains enabled. A mod can request it to be disabled to prevent easy access to GameData, items, and databases. This is not strong anti-tamper security because Linux users control their own machine, but it is useful as a normal mod policy feature.
* **libmini compatibility is being emulated by libsre**:
  SwordigoDesktop does not support official libmini. Instead, `libsre.so` attempts to present compatible behavior to mods. Compatibility for cstrings, `mini.toml`, and `achievements.toml` is still being tested.
* **LuaSocket exists but may not yet be exposed to mods**:
  SRE contains Lua, LuaSocket, LuaFS, and TOML-C libraries, but not all are necessarily mapped into the mod environment.
* **Android storage directories are currently incorrectly unified**:
  `/ExternalFiles/`, `/Files/`, `/ExternalCache/`, and `/Cache/` must retain separate virtual namespaces. If a mod creates the same path in `/Files/` and `/ExternalFiles/`, merging both into one Linux directory changes game behavior.
* **Per-instance filesystem isolation is planned**:
  Current storage is partially combined. The final layout should isolate storage per instance/mod.
* **Mod identification cannot safely rely on modified assets**:
  Every ARM64 1.12.4 instance is effectively SRE-modded. If a user manually copies mod assets into a vanilla assets directory, the engine cannot reliably infer which mod is running. Launcher-declared mod identity or explicit metadata is the practical solution.
* **Anti-tampering is low priority**:
  Some safety and mod-policy mechanisms may be added later, but current priority is fixing broken compatibility systems.

---

## 2. README and TODO

### TODO Checklist
- [ ] Expose CameraController APIs to mod Lua (shakes, custom angles).
- [ ] Investigate and implement ModelComponent RGBA color override checks.
- [ ] Verify `_G.Game` instantiation timing and order across SCL scripts.
- [ ] Implement simulated Android `res/` folder layout and asset mapping for OTF fonts and custom button textures.
- [ ] Add `instance.ini` configuration switches to disable gamepass/unlock hooks per instance.
- [ ] Add option to disable the Lua terminal on a per-mod policy basis.
- [ ] Finalize `libmini` API emulation (TOML, achievements configuration).
- [ ] Separate Android storage paths (`Files`, `ExternalFiles`, `Cache`) into distinct virtual directories.
- [ ] Implement robust launcher/mod-declared metadata detection instead of asset inspection.
