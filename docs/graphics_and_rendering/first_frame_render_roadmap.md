# First Frame Render Checklist

This document details the assets, resource loading order, graphics API functions, and minimal system shims required to render a static main menu frame for *Swordigo* (version 1.4.6).

---

## 1. Startup Asset Inventory

To boot the game to the main menu, the following files must be present in the `assets/resources/` directory:

*   **Scene Files**:
    *   `menu.scene`: Defines the layout, lights, background meshes, camera panning script, and entities for the main menu.
*   **Entity Templates (`.scl`)**:
    *   `game_common.scl`: Base blueprints for general objects, particles, and items.
    *   `monsters.scl`: Contains template definitions for characters (such as the main menu's `darkhero`).
*   **3D Models & Animations (`.POD`)**:
    *   `menu_back.POD`: Mountainous terrain and sky backdrop geometry.
    *   `char_evil.POD`: Mesh geometry for the main menu character model.
    *   `hiro_stand.POD`: Stand-idle animation sequence for the character.
    *   `weapon_glow.POD`: Mesh geometry for the sword model.
    *   `treek.POD` & `tree1.POD`: Environment tree models.
*   **Textures (`.pvr`)**:
    *   `menu_back.pvr`: Skybox and background texture sheet.
    *   `game_common_atlas_2x.pvr`: GUI elements, buttons, frames, and logo sprites.
    *   `swordigo_title_2x.pvr`: The main game title logo.
*   **Texture Mapping & Atlases**:
    *   `game_common_atlas_2x.atlas`: Frame/rectangle coordinates for GUI sprites.
*   **Fonts (`.fnt` & `.pvr`)**:
    *   `font_megalopolis_12.fnt` & `font_megalopolis_12_2x.pvr`: Default font descriptors and textures.
*   **Config Files**:
    *   `options.gopt`: Holds engine preferences and configurations.

---

## 2. First Loaded Assets

During engine initialization (`setupApplication`), the very first assets of each type loaded are:

1.  **First Texture Loaded**: `game_common_atlas_2x.pvr`
    *   *Why*: Loaded during the preloading of `game_common_atlas_2x.atlas` to configure the coordinates of all GUI borders, button shapes, and main menu elements.
2.  **First Model Loaded**: `menu_back.POD`
    *   *Why*: When parsing the first entity (`Background`) in `menu.scene`, the engine instantiates its model component, triggering the load of the mountain/sky geometry file.
3.  **First Font Loaded**: `font_megalopolis_12.fnt` (or `font_megalopolis_10.fnt` if loaded alphabetically)
    *   *Why*: Loaded during the startup of `FontLibrary` to provide UI font definitions for button labels and text overlay rendering.

---

## 3. GLES 1.1 Fixed-Function Requirements

The dynamic imports of `libswordigo.so` verify that it runs exclusively on an **OpenGL ES 1.1 fixed-function pipeline**. Shader compilation, program linking, and uniform updates (GLES 2.0) are **not** used.

Below is the list of GLES 1.1 functions categorized by the engine class that utilizes them:

### TextureLibrary (Texture Allocations & Parameter Configuration)
*   `glGenTextures`: Allocates texture identifiers.
*   `glBindTexture`: Binds a texture to a rendering target (e.g. `GL_TEXTURE_2D`).
*   `glTexImage2D`: Uploads uncompressed texture data to GPU memory.
*   `glCompressedTexImage2D`: Uploads compressed texture formats.
*   `glTexParameteri` & `glTexParameterf`: Sets wrapping (clamp/repeat) and filtering modes (linear/nearest).
*   `glDeleteTextures`: Releases texture identifiers from memory.
*   `glPixelStorei`: Sets pixel storage parameters (e.g. byte alignment).

### FontLibrary / Font (Text Coordinate Arrays)
*   `glBindTexture`: Binds the active font glyph texture sheet.
*   `glEnableClientState` & `glDisableClientState`: Activates array trackers for vertices and texture coordinates (`GL_VERTEX_ARRAY`, `GL_TEXTURE_COORD_ARRAY`).
*   `glVertexPointer`: Specifies the vertex coordinate array for character quads.
*   `glTexCoordPointer`: Specifies the texture mapping array for font glyphs.
*   `glDrawArrays` / `glDrawElements`: Renders the bound arrays as quads.
*   `glEnable(GL_BLEND)` & `glBlendFunc`: Configures alpha blending for text.

### RenderingContext (Matrix Operations & Pipeline State)
*   **Viewport & Clear**: `glViewport`, `glClearColor`, `glClearDepthf`, `glClearStencil`, `glClear`, `glColorMask`, `glDepthMask`.
*   **Matrix Stack**: `glMatrixMode`, `glLoadIdentity`, `glLoadMatrixf`, `glPushMatrix`, `glPopMatrix`, `glTranslatef`, `glScalef`.
*   **Render States**: `glEnable`, `glDisable`, `glBlendFunc`, `glDepthFunc`, `glCullFace`, `glScissor`.
*   **VBO (Vertex Buffer Objects)**: `glGenBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers`.
*   **Client State Arrays**: `glEnableClientState`, `glDisableClientState`, `glVertexPointer`, `glTexCoordPointer`, `glColorPointer`, `glNormalPointer`.
*   **Queries & Errors**: `glGetIntegerv`, `glGetError`, `glGetString`.

---

## 4. Minimum Systems for a Static Title Screen

To render a static title screen with **no audio and no input**, only the following minimal subset of systems is required:

1.  **Host GL Context & Window**:
    *   An active host window (SDL2/GLFW) with a bound OpenGL context supporting compatibility profiles (to execute fixed-function pipeline calls).
2.  **GLES 1.1 Shim**:
    *   Forwarding of GLES 1.1 calls (`glVertexPointer`, `glLoadIdentity`, etc.) to the host OpenGL pipeline.
3.  **JNI Handshake Shims**:
    *   A fake `JNIEnv` and `JavaVM` supporting JNI calls for directory mapping (`setFilesDir`, `setCacheDir`) and asset loading (`setAssetManager`).
4.  **Mocked NDK AssetManager**:
    *   Custom implementations of `AAssetManager_open`, `AAsset_read`, `AAsset_seek`, `AAsset_getLength`, and `AAsset_close` mapping directly to the host filesystem directory `assets/resources/`.
5.  **PVRTC Texture Decoder**:
    *   Since desktop GPUs do not natively support mobile `PVRTC` texture formats, a transcoder must decompress the `.pvr` data to standard `RGBA8888` at load time before sending it to the GPU via `glTexImage2D`.
6.  **Core Engine Classes**:
    *   `Caver::CaverShell`: Allocates the engine state.
    *   `Caver::RenderingContext`: Handles drawing passes and transforms.
    *   `Caver::TextureLibrary`, `Caver::ModelLibrary`, `Caver::FontLibrary`: Caches textures, models, and fonts.
    *   *Note*: The Lua VM is **not** strictly required; stubbing the program execution will freeze animations and panning, but the static background geometry and Main Menu buttons will still render successfully.
