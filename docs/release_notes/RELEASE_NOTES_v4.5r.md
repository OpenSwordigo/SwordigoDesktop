# ⚔️ Swordigo Desktop v4.5r — Release Notes

> *June 19, 2026*

---

## What's New in v4.5r (since v4.0r)

### 📝 Save Editor Revamp

The save editor has been completely rebuilt and moved from the in-game overlay to the **launcher window**, providing a much safer and cleaner experience.

| Feature | Details |
|---------|---------|
| **Launcher-integrated** | Save editor now opens from the launcher, not mid-game |
| **Save file browser** | Lists all `.gplayer` saves with name, area, level, progress, and playtime |
| **Editable fields** | Coins, Health, Mana, XP, Equipped Weapon, Key count |
| **Text input** | Type values directly instead of clicking +/- buttons |
| **Round-trip safe** | Uses proper protobuf serializer that preserves ALL save data |
| **Backup system** | Creates `.bak` backup before writing changes |
| **Success/failure feedback** | Clear status messages after applying edits |
| **Keyboard navigation** | TAB to switch fields, ESC to go back, ENTER to deselect |

> **Breaking change**: The in-game save editor (Mods → Save Editor) has been removed. Use the launcher save editor instead.

---

### 🏗️ ARM64 Stability Improvements

| Fix | Details |
|-----|---------|
| **Entity processing** | NOP'd problematic entity setup call at `0x580708` with `MOV X0, XZR` to prevent infinite loops on empty entity lists |
| **Thread handling** | `pthread_create` now discards thread functions consistently (matching ARM32 behavior) |
| **BSS watchpoints** | Added memory watchpoint infrastructure for debugging null pointer issues in entity data pool |

---

### 🔧 Build & Packaging

- Version bumped to **v4.5r** in launcher status bar
- Target: **glibc 2.39** compatibility for next release builds (requires Ubuntu 24.04 build container)

---

## ⚠️ Known Limitations

### ARM64 (arm64-v8a)

| Issue | Status | Workaround |
|-------|--------|------------|
| **Wastelands freeze** | 🔴 Open | Game spinlocks when entering Wastelands — entity data pool pointer (`[X20+0x310]`) is NULL causing infinite loop on zeroed memory. Switch to ARM32 for this region. |
| **Heavy function stalls** | 🟡 Partial | Some entity processing functions (`0x1478ccc`) take 800ms+ due to instruction-level emulation overhead |

**Recommended ARM64 playthrough**: Use ARM64 from start through **Willcliff Campsite**. Switch to ARM32 for regions beyond.

### ARM32 (armeabi-v7a)

| Issue | Status | Workaround |
|-------|--------|------------|
| **Timer-based spikes** | 🔴 Open | Repeating spikes that poke on a timer interval don't activate (touch-based spikes work fine) |
| **Boss gates** | 🔴 Open | Gates that open after defeating bosses don't trigger. Under investigation — likely related to timer/scheduler system. |
| **Threading** | 🟡 Known | `pthread_create` is stubbed — all thread functions are discarded. Some game mechanics may depend on worker threads. |

### General

| Issue | Status | Notes |
|-------|--------|-------|
| **Launcher .deb icons** | 🟡 Cosmetic | Instance icons show placeholder in .deb install — path mismatch for icon assets |
| **PostFX on some GPUs** | 🟡 Known | SSAO/God Rays may not work on Intel iGPUs with limited GLSL support |
| **Frame spikes in dungeons** | 🟡 Known | Occasional 50ms+ frame times in complex dungeon areas |

---

## 🎮 Recommended Play Strategy

For the most complete experience with v4.5r:

1. **Start on ARM64** — better overall compatibility and features
2. **Play through to Willcliff Campsite** on ARM64
3. **Switch to ARM32** for Wastelands and regions where ARM64 freezes
4. **Use Save Editor** (in launcher) to manage your saves when switching architectures
5. **Note**: Timer-based spikes on ARM32 are cosmetic hazards only — you can still progress past them

---

## 📦 Download

| Format | Platform | Notes |
|--------|----------|-------|
| `.deb` | Debian/Ubuntu x86_64 | Requires glibc ≥ 2.39 |
| `.rpm` | Fedora/RHEL x86_64 | |
| Binary | Raw executable | Requires deps: unicorn, SDL3, OpenAL, zlib |

---

## 👥 Credits

See [README.md](README.md) for the full credits list.

---

*Powered by the Swordigo Runtime Environment (SRE) v4.5*
