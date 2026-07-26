# Caver Native C++ Symbol Demangling & API Registry

## 1. System Overview & Purpose

This document provides a demangled C++ symbol registry covering core engine methods, static constructors, virtual table offsets, and C-linkage entry points extracted from Ghidra decompiled binaries (`symbols_sorted.txt`, `libswordigo_v1.4.12.so`).

This registry allows C++ developers and modding tool authors to resolve exact function signatures, argument types, and class member method mappings for the PC rewrite (`swd`).

---

## 2. Demangled Native Symbol Registry Matrix

### 1. Character & Entity Controller Methods (`Caver::CharControllerComponent`)

| Mangled Itanium Symbol | Demangled C++ Signature | Function Responsibility |
| :--- | :--- | :--- |
| `_ZN5Caver23CharControllerComponent6CreateEv` | `Caver::CharControllerComponent::Create()` | Allocates and initializes new character controller instance ($0\text{x}308$ bytes). |
| `_ZN5Caver23CharControllerComponent12StartJumpingEv` | `Caver::CharControllerComponent::StartJumping()` | Initiates jump impulse and sets airborne state. |
| `_ZN5Caver23CharControllerComponent11StopJumpingEv` | `Caver::CharControllerComponent::StopJumping()` | Cuts variable jump height impulse on button release. |
| `_ZN5Caver23CharControllerComponent5SwingEv` | `Caver::CharControllerComponent::Swing()` | Triggers sword slash animation and attack hitbox. |
| `_ZN5Caver23CharControllerComponent9StopSwingEv` | `Caver::CharControllerComponent::StopSwing()` | Cancels active sword slash animation. |
| `_ZN5Caver23CharControllerComponent11DropQuicklyEv` | `Caver::CharControllerComponent::DropQuickly()` | Applies downward fast-fall velocity vector. |
| `_ZN5Caver23CharControllerComponent3DieEv` | `Caver::CharControllerComponent::Die()` | Triggers player death state and game over screen. |
| `_ZN5Caver23CharControllerComponent14CanDoSomethingEv` | `Caver::CharControllerComponent::CanDoSomething()` | Checks if character is not stunned, frozen, or in cutscene. |

---

### 2. Character State & Attributes (`Caver::CharacterState`)

| Mangled Itanium Symbol | Demangled C++ Signature | Function Responsibility |
| :--- | :--- | :--- |
| `_ZN5Caver14CharacterState31ExperiencePointsRequiredForLevelEi` | `Caver::CharacterState::ExperiencePointsRequiredForLevel(int level)` | Computes total XP threshold formula for level. |
| `_ZN5Caver14CharacterState13AddExperienceEi` | `Caver::CharacterState::AddExperience(int xp)` | Adds XP, checks level-up threshold, grants attribute points. |
| `_ZN5Caver14CharacterState14SetHealthLevelEi` | `Caver::CharacterState::SetHealthLevel(int level)` | Updates health attribute level and increases max hearts. |
| `_ZN5Caver14CharacterState14SetAttackLevelEi` | `Caver::CharacterState::SetAttackLevel(int level)` | Updates attack attribute level and sword damage multiplier. |
| `_ZN5Caver14CharacterState13SetMagicLevelEi` | `Caver::CharacterState::SetMagicLevel(int level)` | Updates magic attribute level and spell damage output. |

---

### 3. Component Manager & Factory (`Caver::ComponentManager` / `ComponentFactory`)

| Mangled Itanium Symbol | Demangled C++ Signature | Function Responsibility |
| :--- | :--- | :--- |
| `_ZN5Caver16ComponentManager6UpdateEf` | `Caver::ComponentManager::Update(float deltaTime)` | Iterates and ticks all active attached components. |
| `_ZN5Caver16ComponentFactory15CreateComponentERKSs` | `Caver::ComponentFactory::CreateComponent(const std::string& className)` | Instantiates component class by string name lookup. |
| `_ZN5Caver19ComponentCollection16AddComponentPtrEPNS_9ComponentE` | `Caver::ComponentCollection::AddComponentPtr(Caver::Component* comp)` | Attaches component pointer to entity array wrapper. |

---

### 4. Game View & Scene Controllers (`Caver::GameViewController` / `GameSceneController`)

| Mangled Itanium Symbol | Demangled C++ Signature | Function Responsibility |
| :--- | :--- | :--- |
| `_ZN5Caver18GameViewController13LoadGameStateEv` | `Caver::GameViewController::LoadGameState()` | Deserializes player save profile and loads map scene. |
| `_ZN5Caver19GameSceneController11UpdateSceneEf` | `Caver::GameSceneController::UpdateScene(float dt)` | Executes level scene tick, physics step, and render pass. |
| `_ZN5Caver19GameSceneController14UnloadCurrentEv` | `Caver::GameSceneController::UnloadCurrent()` | Clears scene graph entities and flushes memory pools. |

---

## 3. Reverse Engineering & Tools Integration Notes

- **GlossHook Integration**: GlossHook's `sym_hooks` registry resolves these mangled C++ symbols at runtime using `dlsym()` / ELF symbol table parsing to inject trampoline hooks.
- **SwKiWi API Integration**: SwKiWi binds its C++ Lua wrappers directly to these demangled method pointers.

---

## 4. PC Port (`swd`) Implementation Strategy

1. **Maintain Exact Method Signatures**: Keep identical function signatures in `swd` C++ headers to allow static linking or dynamic API wrapping.
2. **C-Linkage Symbol Export**: Export C-compatible entry points (`extern "C"`) for modding DLL/so plugin extensions.
