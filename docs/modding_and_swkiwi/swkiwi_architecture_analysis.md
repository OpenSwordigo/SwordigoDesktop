# SwKiwi (SwMini) Modloader Architecture Analysis

> Comprehensive reverse engineering analysis of the **SwKiwi (SwMini)** Android modloader framework for Swordigo.

---

## 1. Executive Summary & Architecture
SwKiwi is a multi-layered modding system built with ~190+ source files across C/C++ and Java:
1. **Java Layer**: Android application bridge, file system resolution, configuration.
2. **JNI / C Bridge**: Native interface, Lua state management.
3. **C/C++ Core**: Function hooking (GlossHook), engine interception, custom API bindings (`Mini.*`, `LNI.*`).

```
Initialization Flow:
JNI_OnLoad() -> MainActivity.onCreate() -> LibraryManager.loadMini() -> NativeBridge.midLoad() -> NativeBridge.lateLoad()
```

---

## 2. Dynamic Function Hooking System (GlossHook)

SwKiwi utilizes **GlossHook** for symbol-based or offset-based function redirection:
```c
// Example GlossHook Definition in SwKiwi
DL_HOOK_SYMBOL(Scene_Create, "_ZN5Caver5SceneC1Ev", void, (void *Scene) {
    orig_Scene_Create(Scene); // Execute original engine function
    SwKiwi_OnSceneCreated(Scene);
})
```

---

## 3. Core Subsystems

### 3.1 Virtual Filesystem (VFS) Path Translation
SwKiwi intercepts asset loads to enforce a resource search priority:
1. `/ExternalFiles/resources/<profile_id>/`
2. `/Files/resources/<profile_id>/`
3. `/ExternalFiles/resources/`
4. `/Files/resources/`
5. `/Assets/resources/` (Vanilla APK fallback)

### 3.2 Lua-Native Interface (LNI System)
Provides C/Lua bindings without standard JNI overhead:
- `LNI.openUrl(url)`: Open web browser.
- `LNI.copyToClipboard(text)`: Clipboard string operations.
- `LNI.getSpeed()` / `LNI.setSpeed(val)`: Game engine time dilation factor.
- `LNI.quit()`: Graceful game termination.

### 3.3 Lua API Surface (`Mini.*`)
- `Mini.Arch()`: Returns `"arm64-v8a"` or `"x86_64"`.
- `Mini.GetProfileID()`: Current save slot UUID.
- `Mini.SetControlsHidden(bool)`: Show/hide UI HUD overlay.
- `Mini.RecreateHero()`: Respawn hero entity with new skin mesh.
- `Mini.SetCoinLimit(limit)`: Maximum Soul Shards / coin capacity (up to 65535).
- `Mini.Health.CurrentMana()` & `Mini.Health.CurrentManaPercent()`: Player mana queries.

---

## 4. Configuration Schema (`mini.toml`)

Mod configuration parameters are parsed from `mini.toml`:
```toml
[mod_overlay]
name = "RLSwordigo 7.0"
authors = ["ItsJustSomeDude"]
version = "7.0"

[options]
coin_limit = 65535
engine_speed = 1.0

[armor_models]
"platearmor" = "hiro_plated"

[armor_attributes]
"platearmor" = 0.5  # 50% damage reduction
```
