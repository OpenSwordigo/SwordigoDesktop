# Swordigo OpenSwordigo Research: CollisionShapeComponent & Ground Physics Terrain

## 1. Ground Collision Shape Parameters (`CollisionShapeComponent`)

Collision detection against ground surfaces is managed via `CollisionShapeComponent` attached to terrain nodes.

---

## 2. C++ Collision Hull Schema

```cpp
namespace Caver {

enum class CollisionSpecialType : uint32_t {
    None         = 0,
    Pickup       = 1,
    Portal       = 2,
    Collectable  = 3,
    Use          = 4,
    BlocksDamage = 5,
    Grabbable    = 6,
    Pushable     = 7
};

class CollisionShapeComponent : public Component {
public:
    bool is_ground = true;               // Tag 10: Ground collision flag
    bool collides = true;                // Tag 18: Active collision toggle
    bool receives_damage = false;        // Tag 20: Destructible flag
    bool inflicts_damage = false;        // Tag 28: Harmful spikes/lava flag
    bool enabled = true;                 // Tag 3A: Component enabled toggle
    bool unsafe_ground = false;          // Tag 46: Unsafe respawn boundary

    float min_depth = -5.0f;             // Tag 23: Min Z collision depth
    float max_depth = 5.0f;              // Tag 2D: Max Z collision depth
    float friction = 1.0f;               // Tag 43: Surface friction multiplier

    CollisionSpecialType special_type = CollisionSpecialType::None; // Tag 28

    Program on_collide_program;          // Tag 2A: OnCollision start Lua script
    Program on_collision_end_program;    // Tag 34: OnCollision end Lua script
    Program on_receive_damage_program;   // Tag 3E: OnDamage script

    const char* GetComponentType() const override { return "CollisionShapeComponent"; }
};

} // namespace Caver
```
