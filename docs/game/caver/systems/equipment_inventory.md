# Caver Equipment & Inventory System Documentation

## 1. System Overview & Purpose

The Equipment and Inventory system (`Caver::HeroEquipmentManager`, `Caver::Item`, `Caver::ItemDropComponent`, `Caver::InventoryItemPanel`) governs player weapons, armor, equippable trinkets, health potions, soul coin drops, and item pickup mechanics.

This document details item data schemas, stat modifier calculations, drop tables, and inventory grid logic for the C++ PC rewrite.

---

## 2. Namespace & Class Hierarchy (`Caver::*`)

```
Caver::Item (Base Item Definition Node)
 ├── Caver::WeaponItem (Melee Weapons)
 ├── Caver::ArmorItem (Defensive Armor)
 ├── Caver::TrinketItem (Equippable Stat Buff Rings / Amulets)
 └── Caver::ConsumableItem (Health Potions & Mana Elixirs)

Caver::HeroEquipmentManager (Active Equipment State Controller)
Caver::ItemDropComponent (Enemy & Chest Loot Generator)
Caver::InventoryItemPanel (Inventory GUI Screen Panel)
```

---

## 3. Equipment Categories & Stat Formulas

### 1. Weapons Catalog & Base Damage Values

| Weapon Name | Weapon ID | Base Physical Damage | Special Attributes |
| :--- | :--- | :--- | :--- |
| **Brass Sword** | `sword_brass` | $10$ | Basic starter sword. |
| **Broadsword** | `sword_broad` | $18$ | Increased attack arc width. |
| **Master Sword** | `sword_master` | $32$ | Emits magic energy wave at full health. |
| **Mage Blade** | `sword_mage` | $65$ | Converts $25\%$ of physical damage to magic element. |

### 2. Stat Modifier Formulas
Final player damage per melee swing is computed dynamically by `HeroEquipmentManager`:
$$\text{Damage}_{\text{final}} = (\text{Damage}_{\text{weapon}} + \text{Level}_{\text{attack}} \times 4) \times (1.0 + \text{Bonus}_{\text{trinket\_attack}})$$

---

## 4. Drop Tables & Item Drop Logic (`ItemDropComponent`)

Enemies and breakable bushes/chests instantiate an `ItemDropComponent` with custom drop probability entries:

```mermaid
flowchart TD
    A[Enemy Slain / Bush Broken Event] --> B[ItemDropComponent::TriggerDrop]
    B --> C[Generate Random Roll Float [0.0, 1.0]]
    C --> D{Check Drop Table Probability}
    D -->|Roll <= Coin Prob| E[Spawn Soul Coin Entity]
    D -->|Roll <= Heart Prob| F[Spawn Health Heart Entity]
    D -->|Roll <= Mana Prob| G[Spawn Mana Orb Entity]
    D -->|Roll > Threshold| H[No Drop Spawned]
    E & F & G --> I[Apply Outward Physics Ejection Velocity Vector]
```

---

## 5. Reverse Engineering & Tools Integration Notes

- **FileRift Asset Reference**: Item 3D models (`.POD`) and icon textures parsed via FileRift match item IDs defined in `HeroEquipmentManager`.
- **SwKiWi API Modding**: SwKiWi exposes `HeroEquipmentManager::RegisterCustomItem`, allowing mods to add custom swords, armor skins, and powerful custom trinkets.

---

## 6. PC Port (`swd`) Implementation Strategy

1. **Structured Item Database**: Store all item definitions, sprite coordinates, models, and stat multipliers in a clean `items.json` database.
2. **Equip / Unequip Delegate Hooks**: Fire event notifications (`OnEquipmentChanged`) when items are equipped to automatically refresh player model mesh attachments and stat displays.
