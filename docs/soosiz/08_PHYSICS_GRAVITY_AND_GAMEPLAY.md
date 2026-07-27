# 08: Physics & Planet Gravity Execution (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/08_PHYSICS_GRAVITY_AND_GAMEPLAY.md`  
> **Status:** Remastered Physics Execution Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Direct Binary Physics Execution

Because we execute **`SoosizHD`** binary instructions directly via Dynarmic ARM32 JIT, **all 360-degree planet surface gravity calculations, collision responses, entity updates, and momentum mechanics are executed by the original compiled ARM32 binary code!**

This guarantees 100% authentic gameplay physics without physics discrepancies, float rounding differences, or collision glitches.

---

## 2. Dynamic Update Step Driven by OpenSwordigo

In `SoosizHD`, physics updates are executed by `-[ApplicationController updateThreadMain:]` at 60 Hz:

```
  OpenSwordigo Main Loop (sre_frame_loop.c)
        |
        v
  Soosiz_StepBinaryLogic(delta_time = 1/60.0f)
        |
        v
  Dynarmic ARM32 JIT -> Executes updateThreadMain: in SoosizHD binary
        |
        +---> Evaluates active planet radial gravity field: g = -r_hat * g_mag
        +---> Integrates hero position & tangential velocity
        +---> Performs circle-to-polygon collision detection
        +---> Updates enemy AI, coin collection, and particle nodes
```

---

## 3. Core Physics Vectors Managed by Binary Code

- **Radial Gravity Vector:** $\vec{g} = -\hat{n} \cdot g_{strength}$ calculated relative to active planet center.
- **Surface Orientation Angle:** $\theta = \operatorname{atan2}(n_y, n_x) - \pi/2$ updated every tick.
- **Dynamic Planet Attraction Transfer:** Evaluated natively across all `.pb` planet nodes in level memory.
- **Trampoline & Bouncy Surface Elasticity:** Integrated directly in JIT.
