# Swordigo OpenSwordigo Research: Quests, Magic Skills & Entity Class Parameters

## 1. Magic Skill System Schema (`Skill`)

Magic spells (Magic Bolt, Fireball, Dimensional Rift, Dragon's Grasp) consume mana and inflict magical damage scaling with the character's `MagicAttribute`.

```cpp
namespace Caver {

struct SkillDefinition {
    std::string skill_id;     // Tag 0x0A ("Name")
    std::string title;        // Tag 0x12
    std::string description;  // Tag 0x1A
    uint32_t mana_cost = 10;  // Tag 0x20
    uint32_t min_damage = 10; // Tag 0x28
    uint32_t max_damage = 20; // Tag 0x30
};

class SkillManager {
public:
    static bool CanCastSkill(const CharacterState& state, const SkillDefinition& skill) {
        return state.current_mana >= skill.mana_cost;
    }

    static uint32_t CalculateMagicDamage(
        const CharacterState& state,
        const SkillDefinition& skill
    ) {
        uint32_t base_damage = skill.min_damage + 
            (rand() % (skill.max_damage - skill.min_damage + 1));

        float multiplier = 1.0f + (state.magic_attribute * 0.20f);
        return static_cast<uint32_t>(base_damage * multiplier);
    }
};

} // namespace Caver
```

---

## 2. Quest Tracking System Schema (`Quest` & `QuestState`)

```cpp
namespace Caver {

struct QuestDefinition {
    std::string quest_id;       // Tag 0x0A ("Name")
    std::string title;          // Tag 0x12
    std::string follow_up_quest;// Tag 0x1A
    std::string map_location;   // Tag 0x22
};

struct QuestState {
    std::string quest_name;     // Tag 0x0A
    bool completed = false;     // Tag 0x10
};

class QuestJournal {
public:
    void CompleteQuest(const std::string& quest_id) {
        m_quest_states[quest_id].completed = true;
    }

    bool IsQuestCompleted(const std::string& quest_id) const {
        auto it = m_quest_states.find(quest_id);
        if (it != m_quest_states.end()) return it->second.completed;
        return false;
    }

private:
    std::unordered_map<std::string, QuestState> m_quest_states;
};

} // namespace Caver
```

---

## 3. Entity Class Combat Resistance Parameters (`EntityClass`)

Enemy entities, bosses, and destructibles inherit base parameters from binary `EntityClass` templates.

```cpp
namespace Caver {

struct EntityClassDefinition {
    std::string class_name;       // Tag 0x0A ("Name")
    std::string title;            // Tag 0x12
    bool level_hidden = false;    // Tag 0x18
    bool freezable = true;        // Tag 0x20
    bool stunnable = true;        // Tag 0x28
    bool grabbable = false;       // Tag 0x30
    float physical_resistance = 0.0f; // Tag 0x45 (0.0 to 1.0)
    float magic_resistance = 0.0f;    // Tag 0x3D (0.0 to 1.0)
};

} // namespace Caver
```
