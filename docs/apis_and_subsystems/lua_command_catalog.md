# Lua Command Catalog (Developer Console Reference)

This document catalogs the namespaces, global libraries, and component-specific bindings exposed to the Lua environment in *Swordigo* (version 1.4.6). It serves as a reference for writing custom scripts to trigger systems or query state when injecting Lua directly via `Caver::ProgramState::ExecuteString`.

---

## 1. Player & Progression Namespace (`Character`)

In *Swordigo*, the player's inventory, quests, coins, stats, and flags are exposed via the global `Character` namespace.

| Function Signature | Description | Example |
| :--- | :--- | :--- |
| `Character.NumCoins()` | Returns the player's current coin count (integer). | `local gold = Character.NumCoins()` |
| `Character.SetNumCoins(int coins)` | Sets the player's coin count directly. | `Character.SetNumCoins(9999)` |
| `Character.AddItem(string item_id)` | Grants an item (e.g. `"legendsword"`, `"brasssword"`, `"platearmor"`). | `Character.AddItem("legendsword")` |
| `Character.RemoveItem(string item_id)` | Removes an item from the player's inventory. | `Character.RemoveItem("brasssword")` |
| `Character.HasItem(string item_id)` | Checks if the player possesses a specific item. | `if Character.HasItem("key_yellow") then ...` |
| `Character.AddSkill(string skill_id)` | Unlocks a spell or magic skill (e.g. `"bolt"`, `"bomb"`, `"hookshot"`). | `Character.AddSkill("hookshot")` |
| `Character.HasSkill(string skill_id)` | Checks if a spell or magic skill is unlocked. | `if Character.HasSkill("bolt") then ...` |
| `Character.AddQuest(string quest_id)` | Activates/starts a quest in the log. | `Character.AddQuest("quest03")` |
| `Character.HasQuest(string quest_id)` | Checks if a quest has been started. | `if Character.HasQuest("quest03") then ...` |
| `Character.SetQuestCompleted(string quest_id)` | Marks a quest as finished in the log. | `Character.SetQuestCompleted("quest03")` |
| `Character.IsQuestCompleted(string quest_id)` | Checks if a quest is marked completed. | `if Character.IsQuestCompleted("quest03") then ...` |
| `Character.IsQuestInProgress(string quest_id)` | Checks if a quest is active but incomplete. | `if Character.IsQuestInProgress("quest03") then ...` |
| `Character.AddQuestText(string quest_id, string text)` | Appends text updates to a quest log entry. | `Character.AddQuestText("quest03", "Found the key.")` |
| `Character.AddFlag(string flag_id)` | Sets a global character/progression flag to `true`. | `Character.AddFlag("sword_found")` |
| `Character.HasFlag(string flag_id)` | Checks if a global progression flag is active. | `if Character.HasFlag("sword_found") then ...` |
| `Character.AddSceneFlag(string flag_id)` | Sets a scene-specific flag to `true`. | `Character.AddSceneFlag("door_unlocked")` |
| `Character.HasSceneFlag(string flag_id)` | Checks if a scene-specific flag is active. | `if Character.HasSceneFlag("door_unlocked") then ...` |
| `Character.SetSceneFlag(string flag_id, bool active)` | Explicitly configures a scene-specific flag. | `Character.SetSceneFlag("gate_open", false)` |
| `Character.RegisterTreasure(SceneObject treasure)` | Registers a treasure chest entity in the level. | `Character.RegisterTreasure(self)` |
| `Character.RegisterTreasureCollection(SceneObject chest)` | Registers the looting of a treasure chest. | `Character.RegisterTreasureCollection(self)` |

---

## 2. Audio Namespaces (`SoundLibrary` & `MusicPlayer`)

Sound effects and background music playback are split into two namespaces.

### Sound Effect Library (`SoundLibrary`)
Exposes spatial sound effects mapped to wave files defined in `sounds.sounds`.

*   **`SoundLibrary.PlayEffect(string effect_name)`**
    Plays a specified sound effect.
    *Example:* `SoundLibrary.PlayEffect("coin_get")` or `SoundLibrary.PlayEffect("surprise2")`

### Background Music Manager (`MusicPlayer`)
Exposes streaming background music tracks.

*   **`MusicPlayer.PlayMusic(string track_name)`**
    Triggers a background track (e.g. `"menu"`, `"town"`, `"forest"`, `"action"`, `"boss"`).
    *Example:* `MusicPlayer.PlayMusic("menu")`
*   **`MusicPlayer.FadeIn(float duration)`**
    Fades in the current music track over the specified duration in seconds.
    *Example:* `MusicPlayer.FadeIn(1.5)`
*   **`MusicPlayer.FadeOut(float duration)`**
    Fades out the current music track.
    *Example:* `MusicPlayer.FadeOut(2.0)`

---

## 3. Save Game System (`SaveGame`)

The game does not expose a direct `SaveGame()` Lua API. Instead, saving is implicitly handled by the engine's C++ layers when specific events occur in Lua, or can be called directly under native emulation.

### Implicit Saving in Lua
*   **`Game.EnterPortal(string scene_name, string spawn_point)`**
    Triggers a transition to a new scene. During the transition, the engine automatically serializes the active character profile and progress into `player.gstate` on disk.
    *Example:* `Game.EnterPortal("town_herohouse", "door1")`

### Native/Emulated Debug Call
Under host emulation (Unicorn/GDB), a debugger can force a game save by directly invoking the engine's C++ save routine:
*   **Target Address (1.4.6)**: `0027c1a5` (`Caver::GameViewController::SaveGameState(bool)`)
*   **Register Setup**:
    *   `R0` = Address of `GameViewController` instance
    *   `R1` = `1` (Boolean flag to force save immediately)

---

## 4. Scene & Entity Management (`Scene`)

Exposes queries and instantiation functions for the active level map.

*   **`Scene.Find(string entity_name)`**
    Locates an active object in the scene by its identifier. Returns a `SceneObject` reference (or `nil` if not found).
    *Example:* `local hero = Scene.Find("hero")`
*   **`Scene.CreateObject(string template_name, string instance_name, SceneObject parent)`**
    Spawns a new entity based on its `.scl` blueprint template, attaching it to a parent object if specified.
    *Example:* `local fireball = Scene.CreateObject("p3_fireball", "fireball_1", self)`
*   **`Scene.AddObject(SceneObject obj)`**
    Adds a dynamically created or cloned object to the scene structure.
    *Example:* `Scene.AddObject(cloned_block)`
*   **`Scene.OverrideLights(Vector3 color, float intensity)`**
    Forces custom ambient lighting properties on the scene.
    *Example:* `Scene.OverrideLights(Vector3.New(1.0, 0.5, 0.5), 1.2)`
*   **`Scene.ResetLights()`**
    Restores default ambient and diffuse lights configured for the scene.
    *Example:* `Scene.ResetLights()`
*   **`Scene.SetGroupHidden(string group_name, bool hidden)`**
    Hides or reveals a collection of objects sharing a group tag.
    *Example:* `Scene.SetGroupHidden("secret_walls", true)`
*   **`Scene.SetPaused(bool paused)`**
    Pauses or resumes update loops for all game entities in the scene.
    *Example:* `Scene.SetPaused(true)`

---

## 5. Camera Control Namespace (`Camera`)

Controls rendering camera alignment, focal targets, panning animations, and shake.

*   **`Camera.FocusAtShape(SceneObject obj)`**
    Aligns the camera's focus target coordinates on a specific entity.
    *Example:* `Camera.FocusAtShape(self)`
*   **`Camera.JumpToFocus()`**
    Snaps the camera position instantly to the current focal point, bypassing interpolation.
    *Example:* `Camera.JumpToFocus()`
*   **`Camera.FollowShape(SceneObject obj)`**
    Configures the camera to dynamically track the movement of a specific entity.
    *Example:* `Camera.FollowShape(hero)`
*   **`Camera.FollowObject(SceneObject obj)`**
    Identical tracking behavior for generic objects.
    *Example:* `Camera.FollowObject(npc)`
*   **`Camera.FocusAtPoint(Vector3 coords)`**
    Pans the camera focus to fixed static coordinates.
    *Example:* `Camera.FocusAtPoint(Vector3.New(100, 50, 0))`
*   **`Camera.ResetFocus()`**
    Clears current targets and restores default camera tracking on the player (`"hero"`).
    *Example:* `Camera.ResetFocus()`
*   **`Camera.Rumble(float intensity, float duration)`**
    Triggers screen-shake with custom power and length.
    *Example:* `Camera.Rumble(5.0, 0.5)`
*   **`Camera.IsPointVisible(Vector3 point)`**
    Returns `true` if a coordinate is within the current viewport boundary.
    *Example:* `if Camera.IsPointVisible(self:position()) then ...`

---

## 6. SceneObject Methods & Component Bindings

In *Swordigo*'s ECS architecture, references to entities (`SceneObject`) are passed to Lua. You can manipulate them using object-oriented methods or static component wrappers.

### Core Object Methods (`SceneObject`)
Methods are invoked directly on object references using the `:` syntax:

| Method Signature | Return Type | Description | Example |
| :--- | :--- | :--- | :--- |
| `obj:identifier()` | `string` | Returns the unique string identifier. | `if self:identifier() == "hero" then` |
| `obj:position()` | `Vector3` | Gets the current coordinate vector. | `local pos = self:position()` |
| `obj:setPosition(Vector3 coords)` | `void` | Teleports the object to coordinates. | `self:setPosition(Vector3.New(0,0,0))` |
| `obj:velocity()` | `Vector3` | Gets the current movement speed vector. | `local vel = self:velocity()` |
| `obj:setVelocity(Vector3 speed)` | `void` | Applies physical velocity. | `self:setVelocity(Vector3.New(200, 0, 0))` |
| `obj:rotation()` | `float` | Gets rotation angle (in degrees). | `local angle = self:rotation()` |
| `obj:setRotation(float angle)` | `void` | Rotates the object. | `self:setRotation(180.0)` |
| `obj:scaling()` | `Vector3` | Gets scale multiplier vector. | `local size = self:scaling()` |
| `obj:setScaling(float scale)` | `void` | Resizes the object uniformly. | `self:setScaling(2.0)` |
| `obj:setHidden(bool hidden)` | `void` | Toggles rendering visibility. | `self:setHidden(true)` |
| `obj:setAlwaysActive(bool active)` | `void` | Forces script updates outside screen. | `self:setAlwaysActive(true)` |
| `obj:clone()` | `SceneObject` | Spawns an exact duplicate of the object. | `local copy = self:clone()` |
| `obj:destroy()` | `void` | Deletes the entity from the world. | `self:destroy()` |
| `obj:isAlive()` | `bool` | Checks if the entity is not destroyed. | `if self:isAlive() then` |

---

### Component Libraries (Static Wrappers)
To manipulate components attached to an entity, static library calls are used, passing the object as the first parameter (the `this` context):

#### Entity Physics & Movement (`EntityController` & `PhysicsObject`)
*   `EntityController.SetMoveSpeed(obj, speed)`: Configures default translation rate.
*   `EntityController.SetMoveDirection(obj, direction)`: Moves left (`-1`), stops (`0`), or right (`1`).
*   `EntityController.SetFacingDirection(obj, direction)`: Orients mesh left (`-1`) or right (`1`).
*   `EntityController.SetMovementBehavior(obj, behavior)`: Configures behavior (e.g. `"fight"`, `"run"`, `"idle"`).
*   `EntityController.StartSwing(obj, damage_type, duration)`: Triggers combat swing animation.
*   `PhysicsObject.SetEnabled(obj, enabled)`: Toggles physical forces, gravity, and collision.
*   `PhysicsObject.SetGravityMagnitude(obj, magnitude)`: Alters gravitational acceleration force.

#### Collisions & Anchors (`CollisionShape` & `ObjectLinkController`)
*   `CollisionShape.SetEnabled(obj, shape_id, enabled)`: Enables/disables a collision box.
*   `CollisionShape.DisableAll(obj)`: Clears all physics barriers for the object.
*   `ObjectLinkController.LinkToBone(child, parent, bone_name)`: Anchors a model bone (e.g., attaching sword to right hand).

#### Visual & Spatial Panning (`TransformController` & `Light`)
*   `TransformController.TranslateTo(obj, Vector3 target, float duration)`: Smoothly pans coordinates.
*   `TransformController.TranslateBy(obj, Vector3 offset, float duration)`: Moves relative to current position.
*   `TransformController.ScaleTo(obj, float size, float duration)`: Shrinks or grows over time.
*   `Light.SetOverlayIntensity(obj, float intensity)`: Controls lighting component strength.

#### Mechanics & HP (`Health` & `Portal`)
*   `Health.SetCurrentHealth(obj, int health)`: Restores or damages HP.
*   `Health.MaxHealth(obj)`: Returns maximum configured health.
*   `Portal.Activate(obj)` / `Portal.Deactivate(obj)`: Toggles portal warp collision.

---

## 7. Execution Proof of Concept Script

This script can be fed into `Caver::ProgramState::ExecuteString` under emulation to verify arbitrary Lua injection. It executes without requiring full scenes or assets loaded.

```lua
-- 1. Output verification to stdout (captured by host hook)
Program.Print("ExecuteString Console Hook Active!")

-- 2. Play a test background music theme (music JNI layer handles this)
MusicPlayer.PlayMusic("menu")

-- 3. Teleport player character if present in the scene
local player = Scene.Find("hero")
if player ~= nil then
    -- Teleport player upwards
    player:setPosition(Vector3.New(player:position():x(), player:position():y() + 200, 0))
    -- Play visual flash effect
    Game.Flash()
    Program.Print("Player teleported successfully.")
else
    Program.Print("Active scene holds no 'hero' entity.")
end
```
