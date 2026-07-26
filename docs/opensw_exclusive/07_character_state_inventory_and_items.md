# Swordigo OpenSwordigo Research: Character State, Inventory System & Item Database

## 1. Character State System Schema

Player stats, inventory items, equipment slots, experience levels, and stat attributes are serialized within the binary `CharacterState` Protobuf structure.

```cpp
namespace Caver {

enum class ItemType : uint32_t {
    Consumable = 1,
    Weapon     = 2,
    Armor      = 3,
    Trinket    = 4,
    QuestItem  = 5
};

struct ItemState {
    std::string name;          // Tag 0x0A
    uint32_t count = 0;        // Tag 0x10
};

struct CharacterState {
    uint32_t current_health   = 100; // Tag 0x10
    uint32_t current_mana     = 50;  // Tag 0x20
    uint32_t current_coins    = 0;   // Tag 0x28
    uint32_t experience_pts   = 0;   // Tag 0x30
    uint32_t experience_lvl   = 1;   // Tag 0x38

    // Equipment Slots
    std::string equipped_weapon;     // Tag 0x62
    std::string equipped_armor;      // Tag 0x6A
    std::string current_skill;       // Tag 0x82
    std::string weapon_trinket;      // Tag 0x8A
    std::string armor_trinket;       // Tag 0x92
    std::string skill_trinket;       // Tag 0x9A

    // Attribute Upgrades
    uint32_t health_attribute  = 0;   // Tag 0xA0
    uint32_t attack_attribute  = 0;   // Tag 0xA8
    uint32_t magic_attribute   = 0;   // Tag 0xB0

    std::vector<ItemState> inventory_items; // Tag 0x5A
};

} // namespace Caver
```

---

## 2. Master Item Database Schema (`Item`)

Static item definitions map item attributes, damage ranges, titles, descriptions, and required character levels.

```cpp
#pragma once
#include <string>
#include <unordered_map>

namespace Caver {

struct ItemDefinition {
    std::string item_id;            // Tag 0x0A ("Name")
    ItemType type;                  // Tag 0x08
    std::string title;              // Tag 0x1A
    std::string short_description;  // Tag 0x22
    std::string long_description;   // Tag 0x2A
    bool is_unique = false;         // Tag 0x30
    uint32_t min_damage = 0;        // Tag 0x38
    uint32_t max_damage = 0;        // Tag 0x40
    uint32_t required_level = 1;    // Tag 0x48
};

class ItemDatabase {
public:
    static ItemDatabase& Instance() {
        static ItemDatabase db;
        return db;
    }

    void RegisterItem(const ItemDefinition& def) {
        m_items[def.item_id] = def;
    }

    const ItemDefinition* GetItem(const std::string& item_id) const {
        auto it = m_items.find(item_id);
        if (it != m_items.end()) return &it->second;
        return nullptr;
    }

private:
    std::unordered_map<std::string, ItemDefinition> m_items;
};

} // namespace Caver
```

---

## 3. Combat Damage & Stat Calculator Engine

```cpp
namespace Caver {

class CombatCalculator {
public:
    static uint32_t CalculateMeleeDamage(
        const CharacterState& state,
        const ItemDefinition* weapon
    ) {
        uint32_t base_damage = 5; // Unarmed damage
        if (weapon && weapon->type == ItemType::Weapon) {
            base_damage = weapon->min_damage + 
                (rand() % (weapon->max_damage - weapon->min_damage + 1));
        }

        // Apply stat attribute multiplier
        float multiplier = 1.0f + (state.attack_attribute * 0.15f);
        return static_cast<uint32_t>(base_damage * multiplier);
    }
};

} // namespace Caver
```
