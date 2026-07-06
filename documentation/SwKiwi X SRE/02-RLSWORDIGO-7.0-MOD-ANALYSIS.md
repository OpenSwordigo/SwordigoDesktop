# RLSwordigo 7.0 Mod - Deep System Analysis

**Target**: Understanding RLSW's dependencies and critical systems for PC port

**Author**: Research Analysis  
**Date**: July 2026  
**Status**: Research Complete - Do Not Implement Yet

---

## Executive Summary

**RLSwordigo 7.0** is a sophisticated Swordigo mod featuring:
- **Persistent data storage** (inventory, currency, achievements)
- **Accessory system** (Baubles with equip/unequip/leveling)
- **Touch-based UI** (buttons, clickable objects)
- **Physics simulation** (gravity, collision, pull effects)
- **Serialization engine** (save/load with circular references)

**Critical Files**: 8 core modules, ~5,800 lines of Lua

---

## 1. DATA PERSISTENCE SYSTEM

### db.scl - Database Module

**Initialization**:
```lua
db.init()
  ├─ Wait 0.05s for file system
  ├─ Check level (skip menu/hero)
  ├─ Get profile ID via Mini.GetProfileID()
  └─ Load existing DB or create new
```

**Data Structure**:
```lua
DB = {
    Inventory = {},    -- Items owned
    SS = 0,           -- Soul Shards (currency)
    Created = <time>, -- Account creation timestamp
    Music = {},       -- Music settings
    Enchants = {},    -- Enchantment data
    Chests = {},      -- Chest state
    Weapons = {}      -- Weapon inventory
}
```

**Persistence Paths**:
```
Dev Mode:
  /ExternalFiles/data/{pid}.lua
  
Release Mode:
  /Files/Documents/{pid}.lua
  
Legacy Migration:
  /ExternalFiles/{pid}.lua → Migrated on first load
```

**Save Cycle**:
```
Every 0.1 seconds:
  ├─ DB.SS = Character.NumCoins()  [Sync currency]
  └─ write(path, srlz(DB))         [Persist to disk]
```

**Critical Functions**:
```lua
db.load(pid)              → Load player data
db.init()                 → Initialize at startup
read(filename)            → Read file from disk
write(filename, content)  → Write file to disk
isDev()                   → Check dev mode (looks for /ExternalFiles/rldev.lua)
portOldData(pid)          → Migrate legacy format
```

**Boot Integration**:
```lua
db.init()  -- Wait 0.05s for file system
Character.SetNumCoins(DB.SS or 0)  -- Initialize coins from DB
newThread("tickler", SecondTicker) -- Start save loop
```

### srlz.scl - Serialization Engine

**Capabilities**:
- Type handling: numbers, booleans, strings, tables, functions, userdata
- Special numbers: infinity (1/0), -infinity (-1/0), NaN (0/0)
- Circular reference detection and reuse
- Multiline string formatting
- Optional metamtable serialization

**Serialization Options**:
```lua
{
    comments = true,           -- Inline comments
    space = " ",               -- Space separator
    newLine = "\n",            -- Line ending
    indent = "  ",             -- Indentation
    lineLength = 30,           -- Line break threshold
    numFormat = "%.17g",       -- Number precision
    positional = true,         -- Preserve array order
    metatables = true,         -- Serialize metatables
    full = false               -- Preserve circular refs
}
```

**Deserialization**:
```lua
success, value = deserialize(data, opts)
-- Safe mode: Sandboxed environment, blocks __index/__call
-- Unsafe mode: Uses global environment (_G)
```

**Key Functions**:
```lua
Srlz.serialize(data, opts)    → String representation
Srlz.deserialize(data, opts)  → Table from string
sortedPairs(d)                → Ordered iteration
serialTable(d, depth, chain)  → Recursive serialization
```

---

## 2. INPUT SYSTEM

### touch.scl - Touch/Click Factory

**Factory Function**:
```lua
Touch.New(identifier, onClick, onDoubleClick, 
          doubleClickInterval, parent, offset)
  → Returns: Touchable object with event handlers
```

**Object Configuration**:
```lua
obj:setAlwaysActive(true)
obj.onClick = onClick
obj.onDoubleClick = onDoubleClick
obj.doubleClickInterval = doubleClickInterval  -- Default: 0.25s
obj.parent = parent
obj.offset = offset or Vector3.New(0,0,0)
```

### touchable.scl - Touchable Component

**Component Properties**:
```lua
TouchRadius: 30-100 units  -- Hit detection range
OnTouch: event handler     -- Triggered on collision
```

**Behavior Loop** (every 0.0001s):
```lua
1. Position Sync (if has parent & offset):
   self:setPosition(self.parent:position() + self.offset)

2. Click Detection:
   if clickCount > 0 then
       clickTimer += 0.01
       if clickTimer >= doubleClickInterval then
           if clickCount == 1 then self.onClick(self)
           elseif clickCount >= 2 then self.onDoubleClick(self)
           clickCount = 0
```

**State Management**:
```lua
clickCount: integer        -- Clicks in current window
clickTimer: float          -- Time since first click
parent: SceneObject        -- Parent reference
offset: Vector3            -- Position offset
```

---

## 3. BAUBLE/ACCESSORY SYSTEM

### baubles.scl - Core Bauble Module

**Data Structure**:
```lua
Bauble = {}  -- Module namespace

baubles = {
    {
        name = "Fiery",
        typeof = "Pendant",     -- Type: Pendant|Ring|Charm
        rarity = "B",           -- Rarity: F,C,B,A,S
        itemvar = "bauble_fiery",
        levelvar = "bauble_fiery_",
        max_level = 1,
        info = {...},           -- Description array
        object = SceneObject    -- Reference
    },
    -- 7 more baubles...
}

max_wear = {
    ["Pendant"] = 1,  -- Max 1 pendant
    ["Ring"] = 2,     -- Max 2 rings
    ["Charm"] = 1     -- Max 1 charm
}

wearing = {
    ["Pendant"] = 0,
    ["Ring"] = 0,
    ["Charm"] = 0
}
```

**Bauble Types & Effects**:

| Name | Type | Rarity | Max Lvl | Effect |
|------|------|--------|---------|--------|
| Fiery | Pendant | B | 1 | Lava protection |
| Freaky | Pendant | S | ? | Anomaly immunity |
| Magic Ring | Ring | A | 3 | Eases magic, mana orbs |
| Vitality Ring | Ring | B | 3 | Health every 14s |
| Magnet Ring | Ring | C | 10 | Increased pull |
| Radioactive Ring | Ring | A | ? | Poison passive |
| Infinity Charm | Charm | S | ? | Blessing/curse |
| Feather Charm | Charm | F | ? | Reduced gravity |

**API Functions**:
```lua
Bauble.HideAll()                      → Hide all bauble objects
Bauble.GetLevel(bauble)               → Current level (0 to max)
Bauble.IncLevel(bauble)               → Increment level if < max
Bauble.Equip(itemvar)                 → Equip if space available
Bauble.Unequip(itemvar)               → Unequip bauble
Bauble.IsWearing(name)                → (bool, object)
Bauble.Find(name)                     → bauble_table by name
Bauble.Has(name)                      → bool (has item owned)
```

**State Persistence**:
```lua
Ownership: Character.HasItem(bauble.itemvar)
Levels: Character.HasFlag(bauble.levelvar..N) for N=1 to max_level
Equipped: Implicit from "wearing" inventory checks
```

**Equip Constraints**:
```lua
if wearing[typeof] < max_wear[typeof] then
    Bauble.Equip(itemvar)
    wearing[typeof]++
```

### baublelib.scl - Bauble Library

**Key Functions**:
```lua
ShowTextBubblez(identifier, position, handleTouches, 
                textArray, maxWidth)
  → Display multi-bubble dialog sequence
  → Returns: Final bubble object

BLoad(name)
  → Create bauble object: Scene.CreateObject("b_"..name)
  → Set: alwaysActive = true
```

**Bootstrap** (on "baubleroom" level):
1. Load: b_funcload object
2. For each bauble with Character.HasFlag(name):
   - Load: "feather", "vitality", "magik", "molten"

---

## 4. MAIN INITIALIZATION & PHYSICS

### rlsw.scl - Bootstrap & Physics

**Entity Initialization**:
```lua
self.friendly = self.friendly or "neutral"  -- Faction
self.exper = self.exper or 700              -- Experience pool
```

**Physics Helpers**:
```lua
ObjectHasPhysics(obj)
  → Returns: PhysicsObject.IsEnabled(obj)
  → Uses: CollisionShape.NewThread() for thread-safe check

getvectoclose(obj1, obj2)
  → Normalized direction vector

GetBHPull(blackhole, obj)
  → Calculate pull strength (distance-based)

IsWithin(obj, position, radius)
  → Sphere collision check
```

**Collision Behavior**:
```lua
if ObjectHasPhysics(obj) and not obj.pulled then
    if obj.friendly != "neutral" then  -- Only damage hostiles
        velocity = direction * pull_strength * (PI - 1.5)
        obj:setVelocity(velocity)
        obj.pulled = true
```

**Pull Radius** (based on object scale):
```lua
if scaling < 16 then
    radius = 150 units
else
    radius = 200 units
```

**State Properties**:
```lua
friendly: "neutral" | "player" | "enemy"
pulled: boolean (prevent stacking)
isBauble: boolean (immune to pull)
dimension: special property
scaling: size multiplier
```

---

## 5. EXTERNAL API DEPENDENCIES

### Character API (Called from RLSW)
```lua
Character.NumCoins()          → Get current Soul Shards
Character.SetNumCoins(amount) → Set currency
Character.HasItem(itemvar)    → Check item owned
Character.HasFlag(flag)       → Check flag status
Character.SetFlag(flag)       → Set flag (persistent)
Character.AddFlag(flag)       → Same as SetFlag
```

### Game API
```lua
Game.CurrentLevelName()       → Get scene name
Game.ShowNotification(text)   → Display message
Game.IncCounter(name)         → Increment achievement counter
```

### Scene API
```lua
Scene.CreateObject(name)      → Spawn object
Scene.Find(name)              → Get object by ID
Scene.OverrideLights()        → Custom lighting
```

### Physics API
```lua
PhysicsObject.IsEnabled(obj)  → Check physics active
Entity.SetPhysicsEnabled(bool)→ Toggle physics
Entity.destroy()              → Delete entity
```

### Component API
```lua
obj:setPosition(Vector3)      → Set location
obj:position()                → Get location
obj:setScaling(float)         → Set size
obj:scaling()                 → Get size
obj:setAlwaysActive(bool)     → Keep active even off-screen
obj:setHidden(bool)           → Visibility
obj:setVelocity(Vector3)      → Set movement speed
```

### Touchable/Input API
```lua
Touchable.SetTouchRadius(obj, radius)
Touchable.SetTouchHandlingEnabled(obj, bool)
TextBubble.IsTextFinished(obj) → Check animation complete
```

### Program/Threading
```lua
Program.Wait(seconds)         → Sleep frame
CollisionShape.NewThread(fn, ...) → Thread-safe call
```

### Mini/LNI API
```lua
Mini.GetProfileID()           → Current save slot UUID
LNI functions (system calls)  → See SwKiwi analysis
```

---

## 6. BOOT SEQUENCE

### Startup Flow
```
Game loads level
  ↓
rlsw.scl OnLoad triggered
  ├─ db.init()  -- Wait 0.05s, load DB
  ├─ Character.SetNumCoins(DB.SS)  -- Init coins
  ├─ newThread("tickler", SecondTicker) -- Save loop
  ├─ newThread("obstacles", loadf)
  └─ newThread("achievements", RLAchievements)
  ↓
SecondTicker loop starts
  └─ Every 1.0s: DB.Time = (DB.Time or 0) + 1
  ↓
Achievement loop starts
  └─ Every 1.0s: Check if all baubles equipped → IncCounter("pancaketime")
  ↓
Game runs
```

### Save Loop
```
db.init() → Every 0.1s:
  ├─ Current coin count = Character.NumCoins()
  ├─ If different from DB.SS:
  │  └─ DB.SS = Character.NumCoins()
  │     write(path, srlz(DB))  -- Persist
  └─ Repeat
```

---

## 7. CRITICAL INTEGRATION POINTS

### Required RLSW Hooks
1. **Level Load**: When scene creates, db.init() must run
2. **Currency Sync**: Character coin changes must update DB.SS
3. **Flag Storage**: Character flags must persist in DB
4. **Bauble Creation**: Scene.CreateObject("b_*") must work
5. **Touch Input**: Click detection must trigger OnTouch events
6. **File System**: Read/write to /Files/Documents/{pid}.lua

### Required Character API
- GetNumCoins() / SetNumCoins()
- HasItem() / HasFlag() / SetFlag()

### Required Scene API
- CreateObject(name)
- Find(name)

### Required Physics
- PhysicsObject.IsEnabled()
- SetVelocity()
- Collision detection

---

## 8. DATA FLOW DIAGRAM

```
┌─────────────────────────────────────────────────────┐
│                  RLSWORDIGO 7.0 FLOW                │
└─────────────────────────────────────────────────────┘

GAME INITIALIZATION
  ├─ Lua State Created
  ├─ rlsw.scl OnLoad Block Executes
  └─ db.init() → Load DB from disk

DB LOADED
  ├─ DB = deserialize(file_content)
  ├─ Character.SetNumCoins(DB.SS)  [Restore currency]
  └─ Lua globals initialized

GAME RUNNING
  ├─ Every 0.1s: Character.NumCoins() syncs to DB.SS
  ├─ Every save: write(path, srlz(DB))
  └─ User actions: Equip bauble, collect items, etc.

PLAYER ACTION: Equip Bauble
  ├─ Touch.New() onClick callback triggered
  ├─ Bauble.Equip(itemvar) called
  ├─ Character.HasItem(itemvar) checked
  ├─ Scene.CreateObject("b_" + name) spawned
  └─ wearing[typeof]++ updated

BAUBLE EFFECT: Magic Ring Leveling
  ├─ Every 120 frames:
  │  ├─ Check: Bauble.IsWearing("bauble_magnet")
  │  ├─ If level < 10:
  │  │  ├─ Set flag: Character.SetFlag("bauble_magnet_" + level)
  │  │  └─ Bauble.IncLevel("bauble_magnet")
  │  └─ Effect triggered in game
  └─ On next save: DB persisted with new level

SAVE/QUIT
  ├─ DB.SS = Character.NumCoins()
  ├─ write(path, srlz(DB))
  └─ Persist to disk (/Files/Documents/{pid}.lua)

RELOAD
  ├─ rlsw.scl runs again
  ├─ db.init() loads DB
  ├─ Character.SetNumCoins(DB.SS) restores state
  └─ All flags, items restored via flags
```

---

## 9. CRITICAL IMPLEMENTATION NOTES

### #1: Serialization Format
- Lua strings (Srlz format)
- NOT JSON, NOT binary
- Example:
  ```lua
  return {
    Inventory={...},
    SS=1500,
    Created=1234567890,
    Music={},
    Enchants={},
    Chests={},
    Weapons={}
  }
  ```

### #2: Flag System
- Everything stored as flags
- Bauble levels: `bauble_magnet_1`, `bauble_magnet_2`, etc.
- Items: `bauble_fiery`, `bauble_vitality`, etc.
- Custom: Any string key via Character.SetFlag()

### #3: Profile ID
- Obtained from Mini.GetProfileID()
- If unavailable, defaults to "default"
- Changes per save slot

### #4: Thread Safety
- db.init() uses Program.Wait(0.0001) for frame-safe updates
- Physics checks via CollisionShape.NewThread()
- No direct synchronization (single-threaded game)

### #5: File Paths (MiniPath)
- `/ExternalFiles/` maps to SD card
- `/Files/` maps to app-private directory
- Special: `/Files/Documents/` = Documents folder (RLSW uses this)

---

## 10. KEY VULNERABILITIES & EDGE CASES

### Circular Reference Handling
- Serialization MUST handle:
  ```lua
  t = {}
  t.self = t  -- Circular reference
  ```
- Srlz detects and reuses object references

### Multi-reference Objects
- Same table used multiple places:
  ```lua
  common = {data}
  DB = {inv1=common, inv2=common}
  ```
- Srlz preserves single instance

### NaN / Infinity Handling
- Numbers: `1/0` (inf), `-1/0` (-inf), `0/0` (NaN)
- Must serialize specially, not as normal numbers

### Flag Collision Avoidance
- Bauble levels: `bauble_*_{1..10}`
- Must not conflict with global flags
- Recommend namespace prefix per mod

### Performance
- Save loop: 0.1s interval (10 saves/second)
- Serialization overhead: Medium (Lua tables only)
- Disk write: Blocking call (handle carefully)

---

## 11. TEST CASES FOR IMPLEMENTATION

### Unit Test: Persistence
```lua
1. Save: DB.SS = 500, write()
2. Load: read(), assert DB.SS == 500
3. Currency Sync: Character.SetNumCoins(600), assert DB.SS == 600
```

### Integration Test: Bauble System
```lua
1. Equip: Bauble.Equip("bauble_magnet")
2. Check: Character.HasItem("bauble_magnet") == true
3. Level: Bauble.IncLevel("bauble_magnet")
4. Check: Character.HasFlag("bauble_magnet_1") == true
5. Persist: Save/load cycle
6. Verify: Bauble state restored
```

### Edge Case: Multiprofile
```lua
1. Save as Profile A
2. Load as Profile B
3. Load as Profile A again
4. Assert: State matches step 1
```

---

## NEXT STEPS

1. ✅ Analyze RLSW architecture (DONE)
2. ⏳ Design SRE integration points
3. ⏳ Plan persistent data storage architecture
4. ⏳ Design UI/buttons implementation
5. ⏳ Create master implementation plan
