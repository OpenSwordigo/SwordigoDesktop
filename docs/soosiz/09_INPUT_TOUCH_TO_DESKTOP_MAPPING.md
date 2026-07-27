# 09: Input System: Touch Interceptor & Controller Mapping (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/09_INPUT_TOUCH_TO_DESKTOP_MAPPING.md`  
> **Status:** Remastered Input Injector & Gamepad Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & Input Injection Strategy

The **`SoosizHD`** binary expects touch inputs via `-[ApplicationController touchesBegan:withEvent:]` and `-[ApplicationController touchesEnded:withEvent:]`.

OpenSwordigo Desktop (`src/main.cpp`) intercepts desktop keyboard (WASD / Arrow keys), mouse, and SDL2 Gamepads, injecting synthesized touch structs or setting touch button flags directly in `ApplicationController` instance memory inside the ARM32 JIT.

---

## 2. Input Injection Architecture

```
  Desktop Keyboard / Gamepad (SDL_Event)
        |
        v
  OpenSwordigo Input Injector (src/soosiz/soosiz_input.cpp)
        |
        v
  Translates Key/Button to Virtual Touch Coordinates (x, y)
        |
        v
  Invokes touchesBegan / touchesEnded in SoosizHD Binary via JIT
```

---

## 3. Keyboard & Gamepad Control Mapping

| Gameplay Action | Keyboard Input | SDL Gamepad Input | Injected Touch Region |
| :--- | :--- | :--- | :--- |
| **Move Left** | `A` / `Left Arrow` | D-Pad Left / Left Stick | `x: 70, y: 610` (Left Arrow Overlay) |
| **Move Right** | `D` / `Right Arrow` | D-Pad Right / Left Stick | `x: 190, y: 610` (Right Arrow Overlay) |
| **Jump** | `Spacebar` / `K` | Button A (South) | `x: 1170, y: 610` (Jump Button Overlay) |
| **Special Ability** | `J` / `Shift` | Button X / Button B | `x: 1015, y: 610` (Special Overlay) |
| **Pause Game** | `Escape` / `P` | Start / Pause Button | `x: 50, y: 50` (Pause Overlay) |

---

## 4. Input Injection Implementation (`src/soosiz/soosiz_input.cpp`)

```cpp
void Soosiz_InjectKeyAsTouch(uint32_t virtualBtnId, bool isPressed) {
    uint32_t appController_ptr = Soosiz_GetApplicationControllerInstance();
    if (!appController_ptr) return;

    float touchX = 0, touchY = 0;
    GetVirtualBtnCoords(virtualBtnId, &touchX, &touchY);

    // Create synthetic UITouch instance in JIT heap
    uint32_t touch_obj = AllocateSyntheticUITouch(touchX, touchY);

    if (isPressed) {
        ExecuteArm32Function(addr_touchesBegan, appController_ptr, sel_touchesBegan, touch_obj, 0);
    } else {
        ExecuteArm32Function(addr_touchesEnded, appController_ptr, sel_touchesEnded, touch_obj, 0);
    }
}
```
