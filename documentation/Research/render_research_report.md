# Swordigo Rendering Engine Architecture & Hook Analysis

This report documents a deep research into the Swordigo (Caver engine) rendering code paths based on Ghidra decompilation of the ARM64 binary (`libswordigo_v1.4.12.so`), comparing the OpenGL ES 1.1 and OpenGL ES 2.0 pipelines.

---

## 1. Executive Summary & Root Cause

The core rendering defects on the ARM64 port (missing text, misplaced HUD elements, broken shadows, and vanilla effects) stem from a **fundamentally broken and incomplete GLES 2.0 (shader-based) implementation in the Caver game engine itself**.

* **The SRE Hook**: The SRE mod loader installs a hook on `RenderingContext::RenderingContext` (`0x2fc03c`) that forces GLES 2.0 mode (`api_mode = 1`).
* **The Engine's GLES2 Flaws**: In the Caver engine binary, setting `api_mode = 1` disables all fixed-function pipelines but fails to replace them with working GLES2 shader commands:
  * **No Matrix Uploads**: Matrix updates (like projection matrices) set a dirty flag `this[0x80] = 1` but do not make any `glUniformMatrix4fv` calls to upload matrices to the shaders.
  * **No Vertex Attribute Binding**: Vertex pointer setups (positions, texture coordinates, and colors) in `VertexArrayObject::BindArrays` are gated with `if (*(int*)param_1 == 0)` (meaning they only execute in GLES 1.1 mode). In GLES 2.0 mode, the engine does not call `glVertexAttribPointer` or `glEnableVertexAttribArray`, leaving the GPU drawing with empty/invalid vertex data.
  * **Disabled Fixed-Function States**: Critical drawing states (like `glEnable(GL_TEXTURE_2D)`) are conditionally bypassed in GLES2 mode.

Because our desktop host runs a standard OpenGL context that natively supports the fixed-function pipeline, **forcing GLES 1.1 mode (`api_mode = 0`)** resolves these rendering issues by executing the engine's fully functional fixed-function pipeline path.

---

## 2. GLES1 vs GLES2 Code Path Comparison

Below is the exact decompiled logic from key rendering files showing how GLES2 skips matrix uploads and vertex binding.

### A. Matrix Setup (`src/render/RenderingContext.c` - Line 249)
In `RenderingContext::SetProjectionMatrix`, the engine completely skips uploading the matrix in GLES2 mode:

```c
  /* If api_mode == 0 (GLES 1.1) */
  if (*(int *)this == 0) {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(param_1);
    glMatrixMode(GL_MODELVIEW);
    return;
  }
  
  /* If api_mode == 1 (GLES 2.0) */
  this[0x80] = (RenderingContext)0x1; // Just sets a dirty flag, no glUniform call!
  return;
```

### B. Vertex & Attribute Binding (`src/scene/VertexArrayObject.c` - Line 262, 363)
In GLES 1.1, vertex attributes are bound via standard pointers. In GLES 2.0, the engine skips them completely:

```c
void Caver::VertexArrayObject::SetVertexAttribArrayEnabled(RenderingContext *param_1, uint param_2, bool param_3) {
  if (*(int *)param_1 != 0) {
    return; // Bypasses GLES1 client states, but NEVER calls GLES2 glEnableVertexAttribArray!
  }
  if (param_3) {
    glEnableClientState(param_2);
    return;
  }
  glDisableClientState(param_2);
}
```

Similarly, in `BindArrays`:
```c
        /* Gated: Only sets pointers in GLES1 mode! */
        if ((*(int *)param_1 == 0) && (*puVar4 - 0x8074 < 5)) {
          switch(*puVar4) {
          case 0x8074:
            glVertexPointer(...);
            break;
          case 0x8076:
            glColorPointer(...);
            break;
          case 0x8078:
            glTexCoordPointer(...);
            break;
          }
        }
```

---

## 3. Recommended Fix

To restore full rendering compatibility on Desktop (including texts, fonts, and shadows), the game must be configured to run in **GLES 1.1 mode**. This can be achieved by updating the `sre_RenderingContext_C1` hook in `src/sre/sre_scene_update.c` to pass `0` instead of `1`:

```diff
- void sre_RenderingContext_C1(void* self, int api_mode) {
-     // Call the original constructor through relay stub forcing GLES 2.0 (api_mode = 1)
-     typedef void (*fn_Ctor)(void*, int);
-     ((fn_Ctor)g_orig_RenderingContext_C1)(self, 1);
- }
+ void sre_RenderingContext_C1(void* self, int api_mode) {
+     // Force GLES 1.1 mode (api_mode = 0) to use the working fixed-function pipeline
+     typedef void (*fn_Ctor)(void*, int);
+     ((fn_Ctor)g_orig_RenderingContext_C1)(self, 0);
+ }
```
