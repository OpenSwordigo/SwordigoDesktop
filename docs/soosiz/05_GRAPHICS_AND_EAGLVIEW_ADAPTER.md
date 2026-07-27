# 05: Graphics Pipeline & EAGLView Interceptor (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/05_GRAPHICS_AND_EAGLVIEW_ADAPTER.md`  
> **Status:** Remastered Rendering Subsystem & GLES Interceptor Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & GLES Call Interception

In the `SoosizHD` binary, graphics rendering is executed by `-[EAGLView drawView]` and `-[ApplicationView renderBegin]`. The binary issues standard **OpenGL ES 1.1** calls via import stubs in `__TEXT.__symbolstub1`.

Our Mach-O loader intercepts these GLES symbol stubs and routes them directly to OpenSwordigo's desktop OpenGL 2.1 / ES 2.0 context managed by **`src/sre/sre_init.c`**.

---

## 2. GLES 1.1 Symbol Intercept Map

| SoosizHD Imported Symbol | Action / Interception Target | OpenSwordigo Desktop Engine Mapping |
| :--- | :--- | :--- |
| **`EAGLContext setCurrentContext:`** | Intercept | No-op (Context managed by SDL2 GL context in `sre_init.c`). |
| **`glBindFramebufferOES`** | Intercept | Bound to default SDL2 main window Framebuffer (ID `0`). |
| **`glMatrixMode`** | Forward | Mapped directly to desktop `glMatrixMode`. |
| **`glOrthof`** | Forward | Mapped to desktop `glOrtho` or matrix uniform update. |
| **`glTranslatef` / `glRotatef`** | Forward | Mapped to desktop `glTranslatef` / `glRotatef`. |
| **`glVertexPointer`** | Forward | Mapped to desktop `glVertexPointer` or VBO array pointer. |
| **`glTexCoordPointer`** | Forward | Mapped to desktop `glTexCoordPointer`. |
| **`glDrawArrays`** | Forward | Mapped directly to desktop `glDrawArrays`. |

---

## 3. 360° Planet View Transformation Execution

When `SoosizHD` binary executes `ApplicationView_renderBegin`:

1. The ARM32 code calculates the dynamic camera orientation angle $\theta$ based on the hero's position on the current planet.
2. The binary executes ARM instructions for `glRotatef(\theta, 0, 0, 1)` and `glTranslatef(-cameraX, -cameraY, 0)`.
3. The symbol interceptor forwards these rotation calls directly to OpenSwordigo's OpenGL matrix stack.
4. Result: 100% authentic 360-degree planet gravity camera rotation rendered natively at 60–144 FPS!

---

## 4. Framebuffer Swap & Display Synchronization

In `SoosizHD`, frame presentation is triggered by `-[EAGLContext presentRenderbuffer:]`.

Our symbol trap hooks `presentRenderbuffer`:

```cpp
// Mach-O GLES Presentation Hook
bool Hook_EAGLContext_presentRenderbuffer(id self, SEL _cmd, uint32_t target) {
    // Render ImGui Debug Launcher Overlay (if active)
    sre_gui_render_overlay();
    
    // Swap SDL2 OpenGL Display Buffers
    sre_swap_buffers();
    return true;
}
```
