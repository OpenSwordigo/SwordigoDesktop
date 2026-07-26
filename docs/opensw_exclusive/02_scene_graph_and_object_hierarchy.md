# Swordigo OpenSwordigo Research: Scene Graph, Entity Hierarchy & Template Management

## 1. Overview of the Caver Scene Graph

The spatial entity system in OpenSwordigo relies on a reference-counted hierarchical scene graph (`Caver::Scene` and `Caver::SceneObject`). Each node represents a physical or logical entity within a 2.5D world, supporting local transformations (translation, 2D rotation, depth scaling, local AABB bounding boxes) and attached dynamic components.

---

## 2. Reference-Counted Object Semantics

Memory lifecycle management across engine nodes is controlled via a custom intrusive reference-counting base class (`Caver::ReferenceCountedObject`).

```cpp
#pragma once
#include <atomic>
#include <cassert>

namespace Caver {

class ReferenceCountedObject {
public:
    ReferenceCountedObject() : m_ref_count(1) {}
    virtual ~ReferenceCountedObject() = default;

    void Retain() const {
        m_ref_count.fetch_add(1, std::memory_order_relaxed);
    }

    void Release() const {
        if (m_ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    int32_t GetRefCount() const {
        return m_ref_count.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<int32_t> m_ref_count;
};

template <typename T>
class RefPtr {
public:
    RefPtr() : m_ptr(nullptr) {}
    RefPtr(T* ptr) : m_ptr(ptr) { if (m_ptr) m_ptr->Retain(); }
    RefPtr(const RefPtr& other) : m_ptr(other.m_ptr) { if (m_ptr) m_ptr->Retain(); }
    ~RefPtr() { if (m_ptr) m_ptr->Release(); }

    RefPtr& operator=(const RefPtr& other) {
        if (m_ptr != other.m_ptr) {
            if (m_ptr) m_ptr->Release();
            m_ptr = other.m_ptr;
            if (m_ptr) m_ptr->Retain();
        }
        return *this;
    }

    T* Get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T* m_ptr;
};

} // namespace Caver
```

---

## 3. SceneObject Schema and Class Definition

Each node in a `.scene` binary file map corresponds to a binary Protobuf tag structure:

| Tag | Field Name | Type / Kind | Description |
| :--- | :--- | :--- | :--- |
| `0x0A` | `Template` | `string` | Identifier of prefab `ObjectTemplate` |
| `0x0C` | `Name` | `string` | Unique entity instance name |
| `0x16` | `Position` | `Vector2` | 2D world position coordinates (X, Y) |
| `0x1A` | `Component` | Embedded Message | Dynamic attached component instance |
| `0x23` | `Rotation` | `float` | Rotation angle in degrees |
| `0x2A` | `LocalAABB` | `Rectangle` | Axis-aligned local bounding box |
| `0x2D` | `Depth` | `float` | Z-layer rendering depth index |
| `0x30` | `Hidden` | `bool` | Visibility rendering toggle flag |
| `0x34` | `OnLoad` | `Program` | Entity initialization script (Lua) |
| `0x3D` | `Scale` | `float` | Uniform scaling factor multiplier |

### C++ Native `SceneObject` Class

```cpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include "caver/math/caver_math.h"
#include "caver/scene/component.h"

namespace Caver {

class SceneObject : public ReferenceCountedObject {
public:
    SceneObject() = default;
    ~SceneObject() override = default;

    // Entity Attributes
    std::string name;
    std::string template_name;
    Vector2 position{ 0.0f, 0.0f };
    float rotation = 0.0f;
    float depth = 0.0f;
    float scale = 1.0f;
    bool hidden = false;
    Rectangle local_aabb{ 0.0f, 0.0f, 0.0f, 0.0f };

    // Hierarchy & Components
    SceneObject* parent = nullptr;
    std::vector<RefPtr<SceneObject>> children;
    std::vector<RefPtr<Component>> components;

    void AddComponent(Component* comp) {
        if (comp) {
            components.push_back(comp);
            comp->SetOwner(this);
        }
    }

    void AddChild(SceneObject* child) {
        if (child) {
            child->parent = this;
            children.push_back(child);
        }
    }

    Matrix4 GetWorldMatrix() const {
        Matrix4 mat = Matrix4::Identity();
        mat = Matrix4::Translate(Vector3(position.x, position.y, depth)) *
              Matrix4::RotateZ(rotation) *
              Matrix4::Scale(Vector3(scale, scale, 1.0f));

        if (parent) {
            return parent->GetWorldMatrix() * mat;
        }
        return mat;
    }
};

} // namespace Caver
```

---

## 4. ObjectTemplate Prefab System

An `ObjectTemplate` defines a reusable blueprint for instantiating entities with pre-configured components and assets across levels.

```cpp
namespace Caver {

struct ObjectTemplate {
    std::string template_id;
    float scaling = 1.0f;
    RefPtr<SceneObject> archetype_object;
};

class TemplateRegistry {
public:
    static TemplateRegistry& Instance() {
        static TemplateRegistry reg;
        return reg;
    }

    void RegisterTemplate(const std::string& name, std::shared_ptr<ObjectTemplate> tmpl) {
        m_templates[name] = tmpl;
    }

    RefPtr<SceneObject> Instantiate(const std::string& name) {
        auto it = m_templates.find(name);
        if (it != m_templates.end() && it->second->archetype_object) {
            // Clone archetype object and attached components
            return it->second->archetype_object;
        }
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<ObjectTemplate>> m_templates;
};

} // namespace Caver
```
