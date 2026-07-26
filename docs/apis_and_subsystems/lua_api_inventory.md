# Lua API Inventory

This document inventories the global modules, classes, and component-specific interfaces exposed to the Lua runtime in *Swordigo* (version 1.4.6).

---

## 1. Global Engine Libraries & Modules

These namespaces are registered globally during `ProgramState` setup, exposing C++ engine functions to Lua:

### `Scene` (World & Entity Management)
*   *Registration Address*: `002e9201` (`_ZN5Caver5Scene15RegisterLibraryEv`)
*   *Key Functions*:
    *   `Scene.Find(string name)`: Locates an active entity in the level by its string identifier.
    *   `Scene.CreateObject(string template_name)`: Spawns a new entity based on its `.scl` blueprint.

### `Camera` (Rendering Camera Controls)
*   *Registration Address*: `002e3d65` (`_ZN5Caver16CameraController22RegisterProgramLibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `Camera.FocusAtShape(SceneObject self)`: Centers the camera focus target on a specific entity.
    *   `Camera.JumpToFocus()`: Instantly snaps the camera to the active focus coordinates.
    *   `Camera.FollowShape(SceneObject self)`: Configures the camera to track the movement of an entity.
    *   `Camera.FocusAtPoint(Vector3 coordinates)`: Pans camera focus to static coordinates.
    *   `Camera.ResetFocus()`: Restores standard tracking on the player character.

### `MusicPlayer` (Background Audio Themes)
*   *Registration Address*: `002f7d75` (`_ZN5Caver11MusicPlayer22RegisterProgramLibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `MusicPlayer.PlayMusic(string theme_name)`: Triggers background music playback (e.g. `"menu"`, `"town"`).

### `SoundLibrary` (Sound Effects)
*   *Registration Address*: `002f9aa9` (`_ZN5Caver12SoundLibrary22RegisterProgramLibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `SoundLibrary.PlayEffect(string effect_name)`: Plays a spatial sound effect.

### `Program` (Coroutine Yielding)
*   *Registration Address*: `003195f5` (`_ZN5Caver12ProgramState22RegisterProgramLibraryEv`)
*   *Key Functions*:
    *   `Program.Wait(float seconds)`: Suspends script execution (yields the coroutine) for the specified duration.

### `Game` (Cinematics & Engine Commands)
*   *Key Functions*:
    *   `Game.SetCinematicMode(boolean enable)`: Freezes player controls and hides UI layout indicators for cutscenes.

---

## 2. Math & Geometry Modules

These libraries provide vector arithmetic and geometric shapes to Lua scripts:

### `Vector3` (3D Vector Math)
*   *Registration Address*: `00323755` (`_ZN5Caver11ProgramMath22RegisterVector3LibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `Vector3.New(float x, float y, float z)`: Constructs a new 3D vector.
    *   `Vector3.FromAngle(float degrees)`: Computes unit direction vector from an angle.
    *   `Vector3:x()`, `Vector3:y()`, `Vector3:z()`: Read coordinate values.

### `Rectangle` (AABB Box Boundaries)
*   *Registration Address*: `00323e19` (`_ZN5Caver11ProgramMath24RegisterRectangleLibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `Rectangle.New(float x, float y, float w, float h)`: Constructs a bounding box.

### `Math` (Trigonometric & Random Helpers)
*   *Registration Address*: `003240e1` (`_ZN5Caver11ProgramMath19RegisterMathLibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `Math.RandomInt(int min, int max)`: Returns a random integer.

---

## 3. Entity & Component Classes (`self` Bindings)

When a script is run by a `ProgramComponent`, the entity running it is passed as `self` (an instance of `Caver::SceneObject`). Scripts can invoke methods on `self` or components attached to it:

### SceneObject methods
*   *Registration Address*: `002f2a31` (`_ZN5Caver14SceneObjectLib15RegisterLibraryEPNS_12ProgramStateE`)
*   *Key Functions*:
    *   `self:position()`: Returns the entity's current `Vector3` position.
    *   `self:setPosition(Vector3 coordinates)`: Updates the entity's coordinates.

### Component-based library wrappers
The engine exposes individual component methods to Lua under namespaces matching the component class names:

*   **`ObjectLinkController`**:
    *   *Registration Address*: `001da449` (`_ZN5Caver29ObjectLinkControllerComponent15RegisterLibraryEv`)
    *   *Key Functions*: `LinkToBone(SceneObject child, SceneObject parent, string bone_name)`: Attaches a model model to a parent character skeletal bone.
*   **`DoorController`**:
    *   *Registration Address*: `0021f5a5` (`_ZN5Caver23DoorControllerComponent15RegisterLibraryEv`)
    *   *Key Functions*: `DoorController.Close()`: Closes door meshes.
*   **`EntityController`**:
    *   *Registration Address*: `001f9f3d` (`_ZN5Caver25EntityControllerComponent15RegisterLibraryEv`)
    *   *Key Functions*: `EntityController.SetMoveSpeed(float speed)`: Controls entity travel rate.
