# GLES Boot Matrix & Symbol Classification

This document maps all 57 OpenGL ES 1.1 and EGL symbols imported by `libswordigo.so` (version 1.4.6), classifies them by execution necessity, and details the strategy of using no-op stubs to bootstrap the emulator.

---

## 1. Symbol Classification Matrix

| GLES Symbol | Category | Purpose | Stub Behavior |
| :--- | :--- | :--- | :--- |
| **`glViewport`** | Required | Configures draw boundaries | Real call |
| **`glClearColor`** | Required | Sets background color | Real call |
| **`glClearDepthf`** | Required | Sets depth clear value | Real call |
| **`glClear`** | Required | Clears active buffers | Real call |
| **`glMatrixMode`** | Required | Switches matrix stack | Real call |
| **`glLoadIdentity`** | Required | Resets matrix transforms | Real call |
| **`glLoadMatrixf`** | Required | Binds custom projection | Real call |
| **`glPushMatrix`** | Required | Saves matrix transform | Real call |
| **`glPopMatrix`** | Required | Restores matrix transform | Real call |
| **`glTranslatef`** | Required | Moves objects | Real call |
| **`glScalef`** | Required | Scales geometry | Real call |
| **`glEnable`** | Required | Enables states (`GL_TEXTURE_2D`, `GL_BLEND`) | Real call |
| **`glDisable`** | Required | Disables states | Real call |
| **`glEnableClientState`** | Required | Activates vertex coordinate arrays | Real call |
| **`glDisableClientState`** | Required | Deactivates vertex arrays | Real call |
| **`glVertexPointer`** | Required | Binds mesh vertex coordinates | Real call |
| **`glTexCoordPointer`** | Required | Binds texture mapping coordinates | Real call |
| **`glGenTextures`** | Required | Allocates texture names | Real call |
| **`glDeleteTextures`** | Required | Deallocates texture names | Real call |
| **`glBindTexture`** | Required | Binds active texture sheets | Real call |
| **`glTexImage2D`** | Required | Uploads uncompressed texture data | Real call |
| **`glCompressedTexImage2D`**| Required | Uploads compressed textures | Real call (decompress to RGBA first) |
| **`glDrawArrays`** | Required | Draws vertex arrays | Real call |
| **`glDrawElements`** | Required | Draws indexed vertices | Real call |
| **`glBlendFunc`** | Required | Configures alpha blending for UI | Real call |
| **`glDepthFunc`** | Required | Sets depth test conditions | Real call |
| **`glDepthMask`** | Required | Controls depth buffer writes | Real call |
| **`glTexParameteri`** | Required | Sets texture filtering wrapping | Real call |
| **`glTexParameterf`** | Required | Sets texture parameter float | Real call |
| **`glPixelStorei`** | Required | Sets texture alignment | Real call |
| `glActiveTexture` | Optional | Texture unit swapping | Stub to NOP |
| `glTexEnvi` | Optional | Combines texture properties | Stub to NOP |
| `glTexEnvfv` | Optional | Combines texture properties float | Stub to NOP |
| `glLightfv` | Optional | Sets up GLES 1.1 lights | Stub to NOP (renders full-bright/unlit) |
| `glLightf` | Optional | Sets up GLES 1.1 lights float | Stub to NOP |
| `glLightModelfv` | Optional | Sets up lighting model | Stub to NOP |
| `glMaterialfv` | Optional | Sets object material properties | Stub to NOP |
| `glColorPointer` | Optional | Mesh color arrays | Stub to NOP |
| `glNormalPointer` | Optional | Mesh normal arrays | Stub to NOP |
| `glColor4f` | Optional | Sets global rendering color | Stub to NOP |
| `glColor4ub` | Optional | Sets global color byte | Stub to NOP |
| `glColorMask` | Optional | Disables writing to color channels | Stub to NOP |
| `glCullFace` | Optional | Disables backface rendering | Stub to NOP |
| `glScissor` | Optional | Limits rendering region | Stub to NOP |
| `glFlush` | Optional | Forces execution queue drain | Stub to NOP |
| `glGenBuffers` | Optional | Allocates VBO buffer IDs | Stub to NOP (engine falls back to standard client arrays) |
| `glBindBuffer` | Optional | Binds active VBOs | Stub to NOP |
| `glBufferData` | Optional | Fills VBO buffers | Stub to NOP |
| `glBufferSubData` | Optional | Updates VBO buffers | Stub to NOP |
| `glDeleteBuffers` | Optional | Frees VBO buffers | Stub to NOP |
| `glClearStencil` | Optional | Configures stencil clear value | Stub to NOP |
| `glStencilFunc` | Optional | Configures stencil test | Stub to NOP |
| `glStencilOp` | Optional | Configures stencil operations | Stub to NOP |
| `eglGetProcAddress` | Optional | Dynamic function resolver | Stub to return `NULL` |
| **`glGetError`** | Debug-only | Queries pipeline error status | Return `0` (`GL_NO_ERROR`) |
| **`glGetString`** | Debug-only | Queries device capabilities | Return string (e.g. `"OpenGL ES 1.1"`) |
| **`glGetIntegerv`** | Debug-only | Queries pipeline properties | Return values (e.g., max texture size = `2048`) |

---

## 2. Minimum Render Set for `menu.scene`

To render the main menu frame successfully, only the **30 Required / Debug-only** symbols highlighted in bold above must be implemented. 

The optional functions (such as lighting, stencils, vertex colors, and VBOs) can be safely shimmed to NOPs. The scene will render unlit and without shadows, but the skybox background mountains (`menu_back.pvr`), character mesh (`char_evil.POD`), and main menu button overlays (`game_common_atlas_2x.pvr`) will draw correctly.

---

## 3. The No-op GLES Stub Strategy

> [!IMPORTANT]
> **Can no-op GLES stubs advance the engine farther before real rendering is needed?**
>
> **Yes. Stubbing the entire GLES dynamic symbol table as no-ops is highly recommended.**
>
> *   *Why*: The native C++ engine (`CaverShell`) handles resource loading, Lua script ticks, and physics updates in RAM, and only pushes drawing commands to the GPU during `drawApplication()`.
> *   *Significance*: If every GLES symbol is resolved to a simple dummy function (returning `0` or doing nothing), the host runner will execute `setupApplication()`, load all files, parse protobuf structures, run the Lua VM state, and loop indefinitely inside the `drawApplication()` loop without crashing.
> *   *Tactical Benefit*: This decouples emulator relocation, JNI handshakes, and crash forensics (Agent 1 & 2's tasks) from GL context configuration. The team can achieve a stable booting and looping state *before* dealing with host GL libraries or texture transcoding.
