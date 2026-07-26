# Swordigo OpenSwordigo Research: Dynamic Component System & ComponentFactory

## 1. Component System Architecture

OpenSwordigo implements a dynamic entity-component pattern where `SceneObject` entities hold heterogeneous lists of `Component` instances. Rather than hardcoding component bindings during compilation, component types are registered string identifiers that get dynamically instantiated via `Caver::ComponentFactory` during binary `.scene` stream parsing.

---

## 2. Base Component Interface

```cpp
#pragma once
#include <string>
#include "caver/scene/reference_counted.h"

namespace Caver {

class SceneObject;
class RenderingContext;

class Component : public ReferenceCountedObject {
public:
    Component() = default;
    ~Component() override = default;

    virtual const char* GetComponentType() const = 0;

    virtual void OnLoad() {}
    virtual void OnUpdate(float delta_time) {}
    virtual void OnRender(RenderingContext& ctx) {}

    void SetOwner(SceneObject* owner) { m_owner = owner; }
    SceneObject* GetOwner() const { return m_owner; }

protected:
    SceneObject* m_owner = nullptr;
};

} // namespace Caver
```

---

## 3. Registered Component Taxonomy

The table below lists all native component classes decoded by the Caver Engine binary parser:

| Component Identifier | Description & Primary Function |
| :--- | :--- |
| `BackgroundComponent` | Parallax 2D background quad rendering and texture scrolling |
| `ModelComponent` | 3D PowerVR `.POD` mesh rendering with local transform matrices |
| `GroundMeshComponent` | Terrain collision surface and static ground mesh rendering |
| `GroundPolygonComponent` | Polygon geometry definition for ground segments |
| `PhysicsObjectComponent` | Rigid body physics, mass, collision hulls, and impulse solvers |
| `SpriteComponent` | Animated 2D quad sprite rendering and atlas subtexture mapping |
| `KeyframeAnimationComponent` | Bone keyframe timeline evaluation and matrix animation |
| `ParentComponentIdentifier` | Dynamic parent linkage and hierarchical transform binding |
| `ScriptComponent` | Embedded Lua event handler and script execution bridge |

---

## 4. ComponentFactory C++ Registry Implementation

```cpp
#pragma once
#include <unordered_map>
#include <functional>
#include <string>
#include <memory>
#include <iostream>
#include "caver/scene/component.h"

namespace Caver {

using ComponentCreator = std::function<Component*()>;

class ComponentFactory {
public:
    static ComponentFactory& Instance() {
        static ComponentFactory factory;
        return factory;
    }

    template <typename T>
    void RegisterComponent(const std::string& type_name) {
        m_registry[type_name] = []() -> Component* {
            return new T();
        };
    }

    Component* CreateComponent(const std::string& type_name) {
        auto it = m_registry.find(type_name);
        if (it != m_registry.end()) {
            return it->second();
        }
        std::cerr << "[ComponentFactory] Warning: Unknown component type '" << type_name << "'\n";
        return nullptr;
    }

private:
    ComponentFactory() = default;
    std::unordered_map<std::string, ComponentCreator> m_registry;
};

// Macro helper for auto-registration
#define REGISTER_CAVER_COMPONENT(TypeClass, TypeString) \
    struct Register##TypeClass { \
        Register##TypeClass() { \
            Caver::ComponentFactory::Instance().RegisterComponent<TypeClass>(TypeString); \
        } \
    } g_register_##TypeClass;

} // namespace Caver
```

---

## 5. Sample Component Implementations

### 5.1 Background Component (`Caver::BackgroundComponent`)

```cpp
#include "caver/scene/component.h"
#include "caver/graphics/rendering_context.h"
#include "caver/scene/scene_object.h"

namespace Caver {

class BackgroundComponent : public Component {
public:
    std::string texture_name;
    Vector2 scroll_speed{ 0.0f, 0.0f };
    Vector2 uv_offset{ 0.0f, 0.0f };

    const char* GetComponentType() const override { return "BackgroundComponent"; }

    void OnUpdate(float dt) override {
        uv_offset.x += scroll_speed.x * dt;
        uv_offset.y += scroll_speed.y * dt;
    }

    void OnRender(RenderingContext& ctx) override {
        if (!m_owner || m_owner->hidden) return;

        ctx.BindTextureByName(texture_name);
        ctx.SetUVOffset(uv_offset);
        ctx.DrawFullscreenQuad();
    }
};

} // namespace Caver
```
