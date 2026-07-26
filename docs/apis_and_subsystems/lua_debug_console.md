# Swordigo Developer Console Reference

This reference documents the tiers of debugging and state manipulation commands available for execution via the `Caver::ProgramState::ExecuteString` console hook.

---

## Tier 1: Safe Commands & Diagnostics (Low Risk)
These commands execute standard library functions and simple expressions that inspect VM state. They can be invoked safely at boot time, *before* any scenes, models, textures, or GLES render loops are initialized.

### `Program.Print(value)`
Prints diagnostic text to standard output (intercepted by host console hook).
*   **Lua Syntax**: `Program.Print("Console active!")`
*   **Output**: Writes to stdout.

### `Program.Wait(seconds)`
Yields/pauses current script execution thread for the specified duration.
*   **Lua Syntax**: `Program.Wait(2.5)`

### Math & String Manipulation
Executes pure math or string manipulation inside the clean VM context.
*   **Lua Syntax**:
    ```lua
    local result = Math.RandomInt(1, 100)
    Program.Print("Random roll: " .. result)
    ```

---

## Tier 2: World & Entity Manipulation (Medium Risk)
These commands query, move, and spawn objects, configure camera movements, and toggle physics. They require an **active scene** to be loaded; running them at boot time before scene initialization will result in null-pointer dereference crashes in the C++ engine.

### Entity Position & Physics
Teleports objects or disables physics colliders.
*   **Lua Syntax (Teleport player to start)**:
    ```lua
    local hero = Scene.Find("hero")
    if hero then
        hero:setPosition(Vector3.New(100, 200, 0))
        hero:setVelocity(Vector3.New(0, 0, 0))
    end
    ```
*   **Lua Syntax (Disable gravity on target)**:
    ```lua
    local block = Scene.Find("woodblock_1")
    if block then
        PhysicsObject.SetGravityMagnitude(block, 0)
    end
    ```

### Camera Control & Effects
Manipulates focus tracking and screen shake.
*   **Lua Syntax (Shake viewport)**:
    ```lua
    Camera.Rumble(8.0, 1.0) -- Intensity 8.0, 1.0 second duration
    ```
*   **Lua Syntax (Focus camera on another entity)**:
    ```lua
    local target = Scene.Find("villager_elder")
    if target then
        Camera.FocusAtShape(target)
        Camera.FollowShape(target)
    end
    ```

### Spawning & Clones
Spawns new entities from SCL templates.
*   **Lua Syntax (Spawn monster)**:
    ```lua
    -- Spawn a "cavelurker" named "enemy_spawn" at player location
    local hero = Scene.Find("hero")
    if hero then
        local monster = Scene.CreateObject("cavelurker", "enemy_spawn", nil)
        monster:setPosition(hero:position())
    end
    ```

---

## Tier 3: Game Progression & Inventory Manipulation (Medium-High Risk)
These commands directly modify the player's profile data, inventory array, coin count, and quest flags. Altering quest progression flags can break scene triggers if dependencies are satisfied out of order.

### Currency Cheat
Modifies wallet size directly.
*   **Lua Syntax (Set coins)**:
    ```lua
    Character.SetNumCoins(99999)
    ```

### Inventory & Spells Cheat
Unlocks weapons, armor, and abilities.
*   **Lua Syntax (Grant items & spells)**:
    ```lua
    Character.AddItem("legendsword")
    Character.AddItem("magicplate")
    Character.AddSkill("hookshot")
    Character.AddSkill("bomb")
    ```

### Quest Manipulation
Bypasses storyline checks.
*   **Lua Syntax (Skip active quest)**:
    ```lua
    Character.SetQuestCompleted("quest02")
    Character.AddQuest("quest03")
    ```

---

## Tier 4: Dangerous Engine Commands (High Risk)
These commands trigger scene loads, exit application threads, interface with GLES view states, or force profile saves. Misuse will corrupt user profiles or crash the emulator.

### Scene Transitions & Portals
Forces the engine to destroy the active scene, serialize progress, and load another map.
*   **Lua Syntax (Teleport to another level)**:
    ```lua
    -- Warp to Town map at spawn point "start1"
    Game.EnterPortal("town_part1", "start1")
    ```

### Render Controlling & Fade
Dims viewports or fades screen to black.
*   **Lua Syntax (Fade out viewport)**:
    ```lua
    Game.FadeOut(3.0) -- Fade to black over 3 seconds
    ```

### Direct C++ Controller Saving
Forces immediate serialization of progress to disk by calling the C++ controller.
*   **Under Emulation**: Jump execution context to C++ address `0027c1a5` (`Caver::GameViewController::SaveGameState(bool)`) with `R0 = GameViewController_instance` and `R1 = 1`.

---

## Console Integration Hook Example
We can expose a command listener interface in our emulator wrapper that executes strings input from the host shell:

```python
def execute_debug_command(emulator, lua_command_string):
    # 1. Allocate string wrapper in emulator guest memory
    payload_addr = emulator.guest_malloc(len(lua_command_string) + 1)
    emulator.mem_write(payload_addr, lua_command_string.encode('utf-8') + b'\0')
    
    # 2. Allocate std::string descriptor (12 bytes for libc++)
    std_str_addr = emulator.guest_malloc(12)
    # Capacity = 16 | 1 = 17, Size = len, Data Ptr = payload_addr
    emulator.mem_write(std_str_addr, struct.pack("<III", 17, len(lua_command_string), payload_addr))
    
    # 3. Setup registers (R0 = ProgramState pointer, R1 = std::string*)
    emulator.reg_write(UC_ARM_REG_R0, emulator.program_state_address)
    emulator.reg_write(UC_ARM_REG_R1, std_str_addr)
    emulator.reg_write(UC_ARM_REG_LR, 0xE0000000) # safety return
    
    # 4. Branch & Execute
    emulator.emu_start(0x00319991, 0xE0000000) # ExecuteString address
    
    # 5. Clean up string allocations
    emulator.guest_free(payload_addr)
    emulator.guest_free(std_str_addr)
```
Using this pattern, a live REPL (Read-Eval-Print Loop) console can be opened directly in the terminal executing the emulation.
