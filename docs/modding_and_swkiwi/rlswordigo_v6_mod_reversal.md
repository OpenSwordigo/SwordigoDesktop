# RLSwordigo Mod System Analysis

## How Swordigo Mods Work

A mod is just a **replacement asset pack** — flat files in `resources/` that override vanilla. No manifest, no packages. The modloader (SwMini/SwKiwi) hooks the engine's file I/O to check mod dirs first.

## RL Structure
```
rl_assets/
├── mini.properties         # mod.name=RLSwordigo, version=6.1
├── cstrings.properties     # UI string replacements  
├── music/                  # 16 custom .ogg tracks
└── resources/              # FLAT directory — everything here
    ├── *.scene             # Protobuf binary (levels)
    ├── *.scl               # Protobuf binary (embedded Lua 5.1 source)
    ├── *.POD, *.pvr, *.wav # Models, textures, sounds
    ├── gamedata.gdata       # Items/spells/quests
    └── newplayer.gstate     # Default new game state
```

> [!IMPORTANT]
> RL is a **total conversion** (~150MB). It needs the FULL `rl_assets/resources/` directory.

## .scl Scripts = Protobuf with Embedded Lua

Scripts use **vanilla Swordigo Lua API** (`Game.*`, `Scene.*`, `Health.*`, `MusicPlayer.*`) plus SwMini extensions:

| API | Functions | Status |
|-----|-----------|--------|
| `Mini.*` | `Arch()`, `GetProfileID()`, `SetControlsHidden()`, `SetCoinLimit()`, `RecreateHero()` | ✅ Stubbed |
| `Mini.Health.*` | `CurrentMana()`, `CurrentManaPercent()` | ✅ Stubbed |
| `LNI.*` | `copyToClipboard()`, `openUrl()`, `getSpeed()`, `setSpeed()`, `quit()` | ✅ Stubbed |
| `Components.*` | `GetValue()`, `SetValue()`, `IsPresent()` | ✅ Stubbed |
| Std Lua libs | `math`, `table`, `io`, `os`, `debug` | ❌ **MISSING** |

> [!CAUTION]
> **The vanilla engine only ships `base` + `string` Lua libs.** SwMini re-adds `math`, `table`, `debug`, `io`, `os`. RL scripts like `monsters.scl` (260KB!), `mason.scl` (158KB), `code.scl` (72KB) almost certainly use `math.*` and `table.*` heavily. This is likely the #1 cause of Lua crashes.

## The Segfault (UC_ERR_WRITE_PROT at 0x4ee790)

This is NOT a Lua error — it's a **write to protected text memory**. Likely caused by:
1. Exception recovery corrupting the stack after repeated Lua crashes
2. A code path only triggered by RL's modified `gamedata.gdata` or `newplayer.gstate`

## What's Needed for Full RL Support

| Priority | Item | Effort |
|----------|------|--------|
| **P0** | Re-add `math`, `table`, `debug` Lua libs | Medium — need `luaopen_math` etc. addresses |
| **P0** | Fix segfault (investigate 0x4ee790) | Hard — need to trace the crash path |
| **P1** | Implement `Components.*` properly (GetValue/SetValue) | Hard — needs engine object introspection |
| **P2** | Parse `mini.properties` (coin limit, speed) | Easy |
| **P2** | `cstrings.properties` UI string replacement | Easy |
| **P3** | Full LNI implementation | Medium |
