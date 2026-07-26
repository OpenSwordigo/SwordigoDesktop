# Caver Skeletal & Keyframe Animation System Documentation

## 1. System Overview & Purpose

The animation system in Swordigo manages character skeletal animation blending, bone hierarchy transformations, mesh keyframing, and UI animation curves (`Caver::CharAnimControllerComponent`, `Caver::BlendAnimationComponent`, `Caver::KeyframeAnimationComponent`, `Caver::Skeleton`, `Caver::SkeletonInstance`, `Caver::AnimBlendNode`).

This document details the skeletal matrix hierarchy, animation blending pipelines, keyframe interpolation, and swingable weapon bone attachments for the C++ PC rewrite.

---

## 2. Namespace & Class Hierarchy (`Caver::*`)

```
Caver::Component
 ├── Caver::AnimationComponent (Base Animation Component)
 │    ├── Caver::KeyframeAnimationComponent (Mesh Vertex Morph / Keyframe Track)
 │    └── Caver::BlendAnimationComponent (Skeletal Pose Blending)
 └── Caver::CharAnimControllerComponent (Player & NPC Character Animation Engine)

Caver::Skeleton (Master Bone Hierarchy Reference)
Caver::SkeletonInstance (Active Entity Bone Transform Instance)
Caver::AnimBlendNode (Cross-Fade Animation Blend State Node)
Caver::AnimKeysNode (Keyframe Rotation Quaternion & Translation Vector Track)
```

---

## 3. Skeletal Bone Hierarchy & Animation Blending

```mermaid
flowchart TD
    A[CharAnimControllerComponent::Update] --> B[Evaluate Current Animation Clip A]
    A --> C[Evaluate Target Animation Clip B]
    B & C --> D[AnimBlendNode::EvaluatePose(BlendWeight t)]
    D --> E[Interpolate Bone Rotations: Slerp(Q_A, Q_B, t)]
    D --> F[Interpolate Bone Translations: Lerp(T_A, T_B, t)]
    E & F --> G[SkeletonInstance::UpdateWorldTransforms]
    G --> H[Multiply Parent Bone Matrix: M_world = M_parent * M_local]
    H --> I[Upload Palette to Shader Uniform Array / Compute Vertex Skinning]
```

---

## 4. Keyframe Interpolation & Bone Attachment

### 1. Quaternion Slerp Interpolation
For bone rotation quaternions $\mathbf{q}_A$ and $\mathbf{q}_B$ at blend time $t \in [0, 1]$:
$$\mathbf{q}(t) = \frac{\sin((1-t)\theta)}{\sin\theta}\mathbf{q}_A + \frac{\sin(t\theta)}{\sin\theta}\mathbf{q}_B, \quad \cos\theta = \mathbf{q}_A \cdot \mathbf{q}_B$$

### 2. Weapon Bone Attachment (`SwingableWeaponControllerComponent`)
Weapons (swords, shields) attach dynamically to designated hand bones in `SkeletonInstance`:
$$M_{\text{weapon\_world}} = M_{\text{hand\_bone\_world}} \cdot M_{\text{weapon\_offset}}$$
This ensures the sword mesh perfectly tracks the character's arm during attack slash animations (`swing_right`, `swing_left`, `jump_slash`).

---

## 5. Reverse Engineering & Tools Integration Notes

- **FileRift Asset Extractor**: FileRift extracts POD skeletal animation tracks, bone index arrays, and binding matrices into standard glTF 2.0 skinning formats.
- **SwKiWi Modding API**: SwKiWi exposes `CharAnimControllerComponent::PlayCustomAnimation`, allowing custom modded character skins to register unique animation clips.

---

## 6. PC Port (`swd`) Implementation Strategy

1. **GPU Skeletal Skinning**: Compute matrix palette skinning directly in vertex shaders (`GLSL u_boneMatrices[64]`) rather than performing CPU vertex skinning calculations.
2. **Animation Event Cues**: Implement frame-accurate animation events (e.g. `OnFootstep`, `OnSwingHitFrame`, `OnSpellReleaseFrame`) triggered at exact normalized clip timestamps.
