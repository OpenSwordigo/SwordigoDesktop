# Caver Game State & World Flags Catalog Documentation

## 1. System Overview & Purpose

Swordigo tracks story progression, map transitions, chest status, boss completions, level-up attributes, item acquisitions, and world state alterations through a bitfield and key-value flag register (`Caver::GameData`, `Caver::StateProperties`).

This document provides a catalog of world flags, quest registers, and player state bitfield definitions required for the C++ PC rewrite and native port (`swd`).

---

## 2. Namespace & Flag Storage Architecture (`Caver::*`)

```
Caver::GameData (Master State Register)
 ├── Caver::StateProperties (Dynamic Key-Value Store)
 │    ├── String Keys -> Integer Values (e.g. "player_level", "soul_coins")
 │    └── String Keys -> Boolean Flags (e.g. "boss_gargoyle_cleared")
 └── Caver::BindingValue (State Binding Observer)
```

---

## 3. Catalog of Critical Game Flags

### 1. Story & Quest Progress Registers

| Flag Key Name | Type | Value Range / Meaning | Triggering Event / Location |
| :--- | :--- | :--- | :--- |
| `quest_main_stage` | Integer | `0` to `20` (Main Quest Index) | Incremented on key quest dialog clear. |
| `flag_hero_woke_up` | Boolean | `true` / `false` | Starting village bed cutscene complete. |
| `flag_elder_visited` | Boolean | `true` / `false` | Talked to Elder in Oakvale. |
| `flag_master_sword_obtained`| Boolean | `true` / `false` | Retrieved Master Sword from Master's Grave. |
| `flag_mage_spell_unlocked` | Boolean | `true` / `false` | Learned Magic Bolt spell. |
| `flag_bomb_spell_unlocked` | Boolean | `true` / `false` | Defeated Corruptor in Plains, unlocked Magic Bomb. |
| `flag_hookshot_unlocked` | Boolean | `true` / `false` | Unlocked Magic Hookshot in Snowy Peak. |
| `flag_dimension_unlocked` | Boolean | `true` / `false` | Unlocked Dimension Rift spell. |

### 2. World & Boss Defeat Registers

| Flag Key Name | Type | Value Range / Meaning | Effect on World |
| :--- | :--- | :--- | :--- |
| `boss_beetle_king_dead` | Boolean | `true` / `false` | Opens barrier in Lower Grove Caves. |
| `boss_gargoyle_dead` | Boolean | `true` / `false` | Unlocks portal to Cairn Wood. |
| `boss_shadow_hero_dead` | Boolean | `true` / `false` | Unlocks Mage Blade shard. |
| `boss_world_boss_cleared` | Boolean | `true` / `false` | Triggers game ending sequence. |

### 3. Chest & Collectable State Bitfields

World treasure chests use encoded spatial key strings:
- **Format**: `chest_<map_id>_<entity_id>` (e.g., `chest_oakvale_01`, `chest_cairnwood_cave_04`).
- **Value**: `true` if opened, `false` if unopened.
- **Effect**: Opened chests stay permanently rendered in their open animation frame upon level reload, preventing duplicate item duplication.

### 4. Player Attributes & Stat Flags

| Stat Register Key | Type | Initial Value | Description |
| :--- | :--- | :--- | :--- |
| `stat_health_level` | Integer | `1` | Player Health Attribute Points spent. |
| `stat_attack_level` | Integer | `1` | Player Melee Damage Attribute Points spent. |
| `stat_magic_level` | Integer | `1` | Player Magic Damage Attribute Points spent. |
| `stat_unspent_points`| Integer | `0` | Available Attribute Points upon level up. |
| `player_xp` | Integer | `0` | Current experience points accumulated. |
| `player_xp_next_level`| Integer | `100` | XP required for next level-up threshold. |

---

## 4. Reverse Engineering & Tools Integration Notes

- **Boulder Level Editor**: Boulder reads `chest_<map_id>_<entity_id>` descriptors directly from entity properties to bind chest objects to global save flags.
- **SwKiWi API Integration**: Mod creators can register new flag namespaces (e.g., `mod_swkiwi_custom_flag_01`) via `StateProperties::SetCustomFlag`.

---

## 5. PC Port (`swd`) Implementation Strategy

1. **Type-Safe Flag Register**: Implement a strongly-typed C++ flag dictionary (`std::unordered_map<std::string, std::variant<bool, int32_t, float, std::string>>`).
2. **Observer Pattern for UI**: Bind UI widgets directly to flag updates via callback delegates (`OnFlagChanged("player_xp", updateXpBar)`).
