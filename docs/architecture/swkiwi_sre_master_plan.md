# SwKiwi X SRE - Comprehensive Integration Master Plan

**Target**: Port SwKiwi (SwMini) modloader and RLSwordigo 7.0 to SRE PC platform  
**Status**: RESEARCH COMPLETE - DETAILED MASTER PLAN  
**Scope**: Full system design for unauthenticated SwKiwi API on PC desktop

---

## 1. Overview & Goals
1. **Enable SwKiwi mod compatibility** on PC port via SRE.
2. **Support RLSwordigo 7.0** mod on desktop platform.
3. **Implement persistent mod data storage** (save/load).
4. **Create custom UI/buttons API** for mods.
5. **Support bauble/accessory system** seamlessly.

---

## 2. Phase 1: Core API & Interface Layers

### 2.1 Mini API Layer (`sre_mini_api.c`)
Exposes C/Lua bridge functions:
- `Mini.Arch()`: Returns `"x86_64"`.
- `Mini.GetProfileID()`: Return save slot UUID or `"default"`.
- `Mini.SetControlsHidden(bool)`: Toggle HUD overlay / Cinematic mode.
- `Mini.SetSpeed(float)` & `Mini.GetSpeed()`: Game time dilation factor (0.1 to 5.0).
- `Mini.RecreateHero()`: Trigger hero respawn with model/skin change.
- `Mini.SetCoinLimit(int)`: Re-apply maximum wallet capacity (up to 65535).
- `Mini.Health.CurrentMana()` & `Mini.Health.CurrentManaPercent()`: Player mana queries.

### 2.2 LNI System (Lua-Native Interface)
Provides zero-JNI native binding for Lua:
- `LNI.openUrl(url)`: Open system browser.
- `LNI.copyToClipboard(text)`: OS clipboard API integration.
- `LNI.getSpeed()` / `LNI.setSpeed(speed)`: Fast-path speed queries.
- `LNI.quit()`: Graceful game exit.

### 2.3 Configuration Parser (`mini.toml`)
Parses `mini.toml` for mod metadata:
- Mod info: `name`, `version`, `authors`, `readme`.
- Custom parameters: `coin_limit`, `engine_speed`, armor model mappings, and item attribute modifications.

---

## 3. Phase 2: Persistence & Custom UI

### 3.1 Persistence Layer (`sre_persistence.c`)
- Key-Value store mapping `mod_id` + `key` -> `string/number`.
- Saved into JSON/Binary format in user config dir (`~/.local/share/swordigo-desktop/mod_save.json`).

### 3.2 Custom UI & Button API (`sre_gui_native.c`)
- Touchables & GUI buttons injected dynamically into `GameOverlayView`.
- Intercept touch events and dispatch custom Lua callbacks.

---

## 4. Phase 3: Bauble & Modded Armor Engine

### 4.1 Accessory / Bauble System
- Up to 8 concurrent bauble slots (rings, amulets, boots, cloaks).
- Hook entity update loop to evaluate passive modifiers (damage resistance, mana regeneration, movement speed multipliers).

### 4.2 Dynamic Armor Mesh Swapping
- Intercept hero mesh loading to swap `.POD` mesh and texture handles dynamically when equips change.

---

## 5. Verification & Testing Strategy
1. **API Validation**: Automated Lua test scripts verifying `Mini.*` and `LNI.*` return types.
2. **Persistence Audit**: Verify state survival across full game restarts.
3. **RLSwordigo 7.0 Verification**: Test game boot, custom GUI buttons, modded scene loading, and hero skin switches.
