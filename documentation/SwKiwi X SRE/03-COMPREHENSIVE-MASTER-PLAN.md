# SwKiwi X SRE - Comprehensive Integration Master Plan

**Target**: Port SwKiwi (SwMini) modloader and RLSwordigo 7.0 to SRE PC platform

**Status**: RESEARCH COMPLETE - DETAILED MASTER PLAN READY FOR IMPLEMENTATION

**Created**: July 2026  
**Scope**: Full system design for unauthenticated SwKiwi API on PC desktop

---

## PROJECT OVERVIEW

### Goals
1. **Enable SwKiwi mod compatibility** on PC port via SRE
2. **Support RLSwordigo 7.0** mod on desktop platform
3. **Implement persistent mod data storage** (save/load)
4. **Create custom UI/buttons API** for mods
5. **Support bauble/accessory system** seamlessly

### Success Criteria
- RLSwordigo 7.0 loads and runs with all features intact
- Mod data persists across sessions
- UI buttons and touchables work identically to Android
- All baubles, inventory, and progression systems functional
- Mini.* and LNI.* APIs available to mods

---

## PHASE 1: CORE API IMPLEMENTATION (WEEKS 1-2)

### 1.1 Mini API Layer (C in SRE)

**Files to create/modify**:
- `src/sre/sre_mini_api.c` (existing - EXPAND)
- `src/sre/sre_mini_api.h` (new header)

**Functions to implement**:

```c
// Architecture info
const char* sre_mini_arch() 
  → Return "x86_64" (or "i686" on 32-bit)

// Profile management
const char* sre_mini_get_profile_id()
  → Return current save slot UUID or "default"

// Controls visibility
void sre_mini_set_controls_hidden(int hidden)
  → Show/hide UI overlay (toggle Cinematic Mode)
  → Store in: g_sre_controls_hidden

// Game speed
float sre_mini_get_speed()
  → Return g_sre_game_speed

void sre_mini_set_speed(float speed)
  → Set g_sre_game_speed
  → Clamp: 0.1 to 5.0
  → Affects all physics and timers via SRE hooks

// Character recreation
void sre_mini_recreate_hero()
  → Trigger hero respawn with model change
  → Set: g_sre_recreate_hero_pending = 1
  → Host polls this flag

// Coin limit
void sre_mini_set_coin_limit(int limit)
  → Set g_sre_coin_limit
  → Clamp: 1 to 65535
  → Must be re-applied on each scene change

// Weapon/trinket glow
void sre_mini_set_weapon_color(float r, float g, float b, float a)
  → Store in g_sre_weapon_color_*
  → Applied on next character update

// Mana queries
int sre_mini_get_current_mana()
  → Return g_sre_player_mana

int sre_mini_get_current_mana_percent()
  → Return (g_sre_player_mana * 100) / g_sre_player_max_mana
```

**Lua bindings** (register in ProgramState::RegisterProgramLibrary hook):
```lua
Mini = {
    Arch = sre_mini_arch,
    GetProfileID = sre_mini_get_profile_id,
    SetControlsHidden = sre_mini_set_controls_hidden,
    RecreateHero = sre_mini_recreate_hero,
    SetCoinLimit = sre_mini_set_coin_limit,
    Health = {
        CurrentMana = sre_mini_get_current_mana,
        CurrentManaPercent = sre_mini_get_current_mana_percent
    }
}
```

### 1.2 LNI System (Lua-Native Interface)

**Files to create**:
- `src/sre/sre_lni.c` (new)
- `src/sre/sre_lni.h` (new header)

**Architecture**:
```c
// Type system for LNI (replaces JNI complexity)
typedef enum {
    LNI_VOID,
    LNI_BOOLEAN,
    LNI_DOUBLE,
    LNI_STRING
} lni_type_t;

// Function descriptor
typedef struct {
    const char* name;           // "getSpeed", "openUrl", etc.
    void (*func_ptr)(lua_State*);  // C function pointer
    lni_type_t return_type;
    int param_count;
    lni_type_t param_types[8];
} lni_function_t;
```

**Exposed functions** (C implementations):
```c
int lni_open_url(lua_State* L)
  // lua_tostring(L, 1) → URL
  // Open in system browser (platform-specific)
  
int lni_copy_to_clipboard(lua_State* L)
  // lua_tostring(L, 1) → Text
  // Copy to clipboard (platform-specific)

int lni_get_speed(lua_State* L)
  // lua_pushnumber(L, g_sre_game_speed)
  
int lni_set_speed(lua_State* L)
  // float speed = lua_tonumber(L, 1)
  // g_sre_game_speed = speed
  
int lni_quit(lua_State* L)
  // Exit game (platform-specific)
```

**Lua bindings**:
```lua
LNI = {
    openUrl = lni_open_url,
    copyToClipboard = lni_copy_to_clipboard,
    getSpeed = lni_get_speed,
    setSpeed = lni_set_speed,
    quit = lni_quit,
    
    -- PascalCase aliases
    OpenUrl = lni_open_url,
    CopyToClipboard = lni_copy_to_clipboard,
    GetSpeed = lni_get_speed,
    SetSpeed = lni_set_speed,
    Quit = lni_quit
}
```

### 1.3 Configuration System (mini.toml Parser)

**Files to create**:
- `src/sre/sre_config.c` (new)
- `src/sre/sre_config.h` (new header)

**Functionality**:
```c
// TOML parser for mini.toml
typedef struct {
    char mod_name[256];
    char mod_version[64];
    char mod_authors[512];    // CSV
    char mod_readme[4096];    // HTML
    
    int coin_limit;           // Default 999
    float engine_speed;       // Default 1.0
    int show_google_button;   // Boolean
    
    // Armor models: map item_id → model_name
    map_t armor_models;
    
    // Armor attributes: map item_id → damage_reduction (0.0-1.0)
    map_t armor_attributes;
} sre_config_t;

// Load configuration
int sre_config_load_toml(const char* toml_file, sre_config_t* out)
  // Parse mini.toml
  // Return 0 on success
```

**Search paths** (for mini.toml):
1. `./mods/active/mini.toml` (per-profile)
2. `./mods/mini.toml` (global)
3. Built-in defaults

---

## PHASE 2: DATA PERSISTENCE LAYER (WEEKS 2-3)

### 2.1 Virtual Filesystem (MiniPath Translation)

**Files to create/modify**:
- `src/sre/sre_vfs.c` (new - replaces impl_files/)
- `src/sre/sre_vfs.h` (new header)

**Path translation**:
```c
// Translate MiniPath to PC path
// /Files/Documents/{profile}/ → ~/.swordigo/{profile}/
// /ExternalFiles/resources/ → ~/Documents/Swordigo/resources/
// /Cache/ → ~/.swordigo/cache/
// resources/ → Search hierarchy (see below)

char* sre_vfs_translate_path(const char* minipath, const char* profile_id)
  → Returns: Real filesystem path

// Resource search order
int sre_vfs_find_resource(const char* name, const char* profile_id, 
                          char* out_path, size_t out_len)
  // Search order:
  // 1. ~/.swordigo/{profile_id}/resources/{name}
  // 2. ~/.swordigo/resources/{name}
  // 3. ~/Documents/Swordigo/resources/{name}
  // 4. ./resources/{name} (fallback)
  // Return 0 if found, set out_path
```

**Directory structure** (PC):
```
~/.swordigo/                    # App data root
  ├── default/                  # Default profile
  │   ├── rlsw.lua              # RLSW data
  │   ├── Documents/            # Save files
  │   │   └── {pid}.lua         # Player data (serialized DB)
  │   └── resources/            # Override resources
  ├── resources/                # Global overrides
  ├── cache/                    # Temporary files
  └── mini.toml                 # Global configuration

~/Documents/Swordigo/          # User-visible location
  ├── resources/                # External resources
  └── mods/                     # User mods
```

**Critical functions** (wrapper `fopen`, `fwrite`, etc.):
```c
FILE* sre_vfs_fopen(const char* path, const char* mode)
  // Translate path, delegate to fopen()

size_t sre_vfs_fread(void* buf, size_t size, size_t nmemb, FILE* fp)
  // Direct pass-through to fread()

size_t sre_vfs_fwrite(const void* buf, size_t size, size_t nmemb, FILE* fp)
  // Direct pass-through to fwrite()

int sre_vfs_directory_exists(const char* path)
  // Check + create if needed
```

### 2.2 Serialization Library Integration

**Files to create/modify**:
- `src/sre/sre_srlz.c` (Lua serialization bindings)
- `src/sre/sre_srlz.h` (header)

**Approach**: Don't re-implement Srlz.scl—instead, load it as a Lua library
- Load rlsw decompiled srlz.scl at startup
- OR re-implement key functions:

```c
// Simpler version (handles RLSWordigo needs):
int sre_lua_serialize(lua_State* L)
  // ARG: table on stack
  // Return: Lua string (Srlz format)
  // Handles: tables, numbers, strings, functions
  // Circular refs: Error or reuse

int sre_lua_deserialize(lua_State* L)
  // ARG: string on stack
  // Return: table
  // Safe: Sandboxed environment (no file access)

// Lua bindings
Srlz = {
    serialize = sre_lua_serialize,
    deserialize = sre_lua_deserialize
}
```

### 2.3 Database Module (db.scl Adaptation)

**Files to create/modify**:
- `src/lua/db.lua` (Pure Lua implementation - MODIFY decompiled db.scl)

**Key changes for PC**:
```lua
-- db.lua adapted for SRE/PC

local db_path_fmt = "/Files/Documents/%s.lua"  -- Will be translated via VFS

function db.init()
    Program.Wait(0.05)  -- Frame sync
    
    -- Get current level
    local level = Game.CurrentLevelName()
    if level == "menu" or level == "hero" then
        return
    end
    
    -- Get profile ID (fallback "default")
    local pid = Mini.GetProfileID() or "default"
    
    -- Load existing DB
    DB = db.load(pid) or {
        Inventory = {},
        SS = 0,
        Created = os.time(),
        Music = {},
        Enchants = {},
        Chests = {},
        Weapons = {}
    }
    
    -- Initialize coins
    Character.SetNumCoins(DB.SS or 0)
    
    -- Start save loop (every 0.1s)
    local save_thread = function()
        while true do
            Program.Wait(0.1)
            
            -- Sync currency
            DB.SS = Character.NumCoins()
            
            -- Write to disk
            local path = db_path_fmt:format(pid)
            db.write(path, srlz_encode(DB))
        end
    end
    
    newThread("db_saver", save_thread)
end

function db.load(pid)
    local path = db_path_fmt:format(pid)
    local content = db.read(path)
    if not content then return nil end
    
    -- Use Srlz to deserialize
    local success, data = Srlz.deserialize(content, {safe=true})
    return success and data or nil
end

-- File I/O (translated via VFS)
function db.read(filepath)
    local file = io.open(filepath, "r")
    if not file then return nil end
    
    local content = file:read("*a")
    file:close()
    return content
end

function db.write(filepath, content)
    local file = io.open(filepath, "w")
    if not file then return end
    
    file:write(content)
    file:close()
end
```

---

## PHASE 3: UI/BUTTONS SYSTEM (WEEKS 3-4)

### 3.1 Touch/Button Infrastructure

**Files to create/modify**:
- `src/sre/sre_ui.c` (Button state management)
- `src/sre/sre_ui.h` (UI header)

**UI State** (in SRE globals):
```c
// Button registry
typedef struct {
    uint32_t id;
    float x, y;                 // Screen position
    float radius;               // Touch radius
    void (*on_touch)(uint32_t);  // Callback (in Lua)
    int active;                 // Is active
} sre_button_t;

#define MAX_BUTTONS 256
static sre_button_t g_sre_buttons[MAX_BUTTONS];
static int g_sre_button_count = 0;

// Clear buttons on scene change
void sre_ui_clear_buttons()
  → g_sre_button_count = 0
  → memset(g_sre_buttons, 0, sizeof(...))
```

**Lua bindings** (implemented in Lua, not C):
```lua
-- touch.scl adapted for SRE
Touch = {}

function Touch.New(id, onClick, onDoubleClick, interval, parent, offset)
    -- Create touchable object
    local obj = Scene.CreateObject("touchable")
    obj.id = id
    obj.onClick = onClick
    obj.onDoubleClick = onDoubleClick
    obj.doubleClickInterval = interval or 0.25
    obj.parent = parent
    obj.offset = offset or Vector3.New(0,0,0)
    obj.clickCount = 0
    obj.clickTimer = 0
    
    return obj
end

-- touchable.scl adapted
local this = ...

while true do
    Program.Wait(0.0001)  -- Every frame
    
    -- Sync position with parent
    if this.parent then
        local new_pos = this.parent:position() + (this.offset or Vector3.New(0,0,0))
        this:setPosition(new_pos)
    end
    
    -- Click detection loop
    if this.clickCount > 0 then
        this.clickTimer = this.clickTimer + 0.0001
        
        if this.clickTimer >= (this.doubleClickInterval or 0.25) then
            if this.clickCount == 1 and this.onClick then
                this.onClick(this)
            elseif this.clickCount >= 2 and this.onDoubleClick then
                this.onDoubleClick(this)
            end
            this.clickCount = 0
            this.clickTimer = 0
        end
    end
end
```

### 3.2 Input Handling

**Files to create/modify**:
- Hook: `Scene_OnInput` (when player clicks)
  
**Click detection** (in scene update hook):
```c
// SRE hook: Scene_OnInput(x, y)
void sre_scene_on_input(float x, float y)
{
    // Check all active buttons
    for (int i = 0; i < g_sre_button_count; i++) {
        sre_button_t* btn = &g_sre_buttons[i];
        
        // Distance check
        float dx = x - btn->x;
        float dy = y - btn->y;
        float dist = sqrt(dx*dx + dy*dy);
        
        if (dist <= btn->radius && btn->active) {
            // Register click with Lua
            // Lua will handle debouncing/double-click logic
            btn->on_touch(btn->id);  // Call Lua callback
        }
    }
}
```

---

## PHASE 4: BAUBLE/ACCESSORY SYSTEM (WEEKS 4-5)

### 4.1 Bauble Module (Pure Lua)

**Files to create/modify**:
- `src/lua/baubles.lua` (ADAPT decompiled baubles.scl)
- `src/lua/baublelib.lua` (ADAPT baublelib.scl)

**Implementation** (no C changes needed—all in Lua):
```lua
-- baubles.lua

Bauble = {}

-- Bauble database
local baubles = {
    {name="Fiery", typeof="Pendant", max_level=1, itemvar="bauble_fiery", ...},
    {name="Magic Ring", typeof="Ring", max_level=3, itemvar="bauble_magic", ...},
    -- ... more baubles
}

local max_wear = {Pendant=1, Ring=2, Charm=1}
local wearing = {Pendant=0, Ring=0, Charm=0}

-- API functions
function Bauble.IsWearing(name)
    for i, bauble in pairs(baubles) do
        if bauble.name == name then
            return Character.HasItem(bauble.itemvar), bauble
        end
    end
    return false, nil
end

function Bauble.GetLevel(bauble)
    local levelvar = bauble.levelvar or bauble.itemvar .. "_"
    local level = 0
    for i = 1, bauble.max_level or 1 do
        if Character.HasFlag(levelvar .. i) then
            level = i
        end
    end
    return level
end

function Bauble.Equip(itemvar)
    -- Find bauble
    local bauble = nil
    for i, b in pairs(baubles) do
        if b.itemvar == itemvar then
            bauble = b
            break
        end
    end
    
    if not bauble then return end
    
    -- Check wear limit
    if wearing[bauble.typeof] < (max_wear[bauble.typeof] or 1) then
        Character.SetFlag("worn_" .. itemvar)
        wearing[bauble.typeof] = wearing[bauble.typeof] + 1
        
        -- Spawn scene object
        local obj = Scene.CreateObject("b_" .. bauble.name:lower())
        bauble.object = obj
    end
end

function Bauble.Unequip(itemvar)
    -- Implementation...
end

-- More functions...
```

### 4.2 Character API Integration

**Files to modify**:
- `src/sre/sre_scene_update.c` (Character state)

**Required Character functions** (Lua bindings):
```c
// Flag system (persistent across sessions via DB)
int character_has_flag(lua_State* L)
  → lua_tostring(L, 1) → flag name
  → Check: DB.Flags[flag_name]
  → Return: boolean

int character_set_flag(lua_State* L)
  → lua_tostring(L, 1) → flag name
  → Set: DB.Flags[flag_name] = true
  → Persist on next save

// Item system
int character_has_item(lua_State* L)
  → lua_tostring(L, 1) → item name
  → Check: Character.HasFlag(item_name)
  → Return: boolean

int character_add_item(lua_State* L)
  → lua_tostring(L, 1) → item name
  → Character.SetFlag(item_name)
```

---

## PHASE 5: INTEGRATION & TESTING (WEEKS 5-6)

### 5.1 Hook Installation

**Files to modify**:
- `src/sre/sre_init.c` (Add new hooks)

**Required hooks**:
```c
// Scene lifecycle
DL_HOOK_SYMBOL(Scene_Create, "_ZN5Caver5SceneC1Ev", ...) {
    // Scene created
    sre_ui_clear_buttons();
    orig_Scene_Create(...);
}

DL_HOOK_SYMBOL(Scene_Destroy, "_ZN5Caver5SceneD1Ev", ...) {
    // Scene destroyed
    sre_ui_clear_buttons();
    orig_Scene_Destroy(...);
}

// Lua library registration
DL_HOOK_SYMBOL(RegisterProgramLibrary, ..., ...) {
    orig_RegisterProgramLibrary(...);
    
    // Inject our APIs
    sre_lua_register_mini(L);
    sre_lua_register_lni(L);
    sre_lua_register_srlz(L);
}

// Game speed (affects physics + timers)
// Hook: Every frame update checks g_sre_game_speed
```

### 5.2 Test Plan

**Unit Tests**:
1. Mini API functions
2. LNI function calls
3. VFS path translation
4. Serialization/deserialization
5. Character flag system

**Integration Tests**:
1. RLSW mod loading
2. Save/load cycle
3. Bauble equip/unequip
4. UI button clicks
5. Physics simulation

**E2E Tests**:
1. Complete game session with RLSW
2. Multiple profiles
3. Cross-session persistence
4. Performance (no stutters, etc.)

---

## PHASE 6: RLSWORDIGO 7.0 PORT (WEEK 6)

### 6.1 Mod Loader Integration

**Files to create**:
- `mods/rlswordigo_7.0/main.lua` (Entry point)
- `mods/rlswordigo_7.0/mini.toml` (Configuration)

**Bootstrap**:
```lua
-- Load RLSW at game start
function load_rlswordigo()
    require("rlsw")  -- Load mod files
end

-- Hook into scene load
Scene.OnLoad = load_rlswordigo
```

### 6.2 RLSW Module Loading

**Required modules** (already decompiled in reference):
```
rlsw/
├── db.lua              # Data persistence
├── srlz.lua            # Serialization
├── baubles.lua         # Bauble system
├── touch.lua           # Input handling
├── touchable.lua       # Touchable objects
├── items.lua           # Item system
├── code.lua            # Main gameplay
└── random.lua          # RNG utilities
```

---

## DETAILED HOOK POINTS FOR SRE

### Critical Hooks (MUST IMPLEMENT)

| Hook | Location | Purpose | Type |
|------|----------|---------|------|
| `Scene_Create` | libswordigo.so | Init level | Symbol/Offset |
| `Scene_Destroy` | libswordigo.so | Cleanup level | Symbol/Offset |
| `RegisterProgramLibrary` | libswordigo.so | Inject Lua APIs | Symbol/Offset |
| `Character_SetNumCoins` | libswordigo.so | Currency update | Symbol/Offset |
| `Character_NumCoins` | libswordigo.so | Query currency | Symbol/Offset |
| `Character_HasFlag` | libswordigo.so | Check flag | Symbol/Offset |
| `Character_SetFlag` | libswordigo.so | Set flag | Symbol/Offset |
| `Game_CurrentLevelName` | libswordigo.so | Get scene name | Symbol/Offset |
| `Scene_CreateObject` | libswordigo.so | Spawn entity | Symbol/Offset |
| `Scene_Find` | libswordigo.so | Find entity | Symbol/Offset |

### Optional Hooks (NICE TO HAVE)

| Hook | Purpose | Performance Impact |
|------|---------|-------------------|
| `lua_pcall` | Profile Lua execution | Low |
| `Model_Load` | Armor model swapping | Medium |
| `Damage_Calculate` | Armor reduction | Medium |
| `Input_OnClick` | Button clicks | None (in input thread) |

---

## RECOMMENDED SOLUTION SUMMARY

### Architecture Decision Matrix

| Component | Recommended | Rationale |
|-----------|-------------|-----------|
| Mini API | C in SRE | Direct access to game state, fast |
| LNI System | C wrapper for Lua | Avoid JNI complexity |
| VFS | Translation layer in C | Minimal overhead |
| Serialization | Load Lua module | Reuse decompiled code |
| Bauble System | Pure Lua | No engine changes needed |
| UI/Buttons | Lua with C support | Flexibility + performance |
| Data Persistence | File I/O + Serialization | Simple, proven |
| Config | TOML parser in C | One-time load |

### Technology Stack

| Layer | Technology | Files |
|-------|-----------|-------|
| **Mod API** | C (SRE) | sre_mini_api.c/h, sre_lni.c/h |
| **Configuration** | TOML | sre_config.c/h (use toml-c library) |
| **Filesystem** | C + Platform APIs | sre_vfs.c/h |
| **Serialization** | Lua | Srlz.lua (decompiled code) |
| **Lua Bindings** | C + Lua | sre_lua_libs.c, Lua modules |
| **Persistence** | File I/O + Lua | db.lua (adapted) |
| **UI System** | Lua + C hooks | touch.lua, touchable.lua |
| **Physics** | Existing SRE | No changes needed |

---

## IMPLEMENTATION ROADMAP

```
Week 1-2: CORE API
  ├─ Mini.* functions (C)
  ├─ LNI.* functions (C)
  ├─ Lua bindings
  └─ Testing

Week 2-3: PERSISTENCE
  ├─ VFS layer (C)
  ├─ Serialization (Lua)
  ├─ db.lua (adapted)
  └─ Save/load testing

Week 3-4: UI SYSTEM
  ├─ touch.lua / touchable.lua
  ├─ Button state management
  ├─ Input handling
  └─ Integration tests

Week 4-5: BAUBLES
  ├─ baubles.lua (adapted)
  ├─ baublelib.lua (adapted)
  ├─ Character API
  └─ System testing

Week 5-6: INTEGRATION
  ├─ Hook installation
  ├─ RLSW module loading
  ├─ End-to-end tests
  └─ Performance tuning

Week 6+: RLSW PORT
  ├─ mod_loader integration
  ├─ RLSW 7.0 modules
  ├─ Full gameplay testing
  └─ Bug fixes & polish
```

---

## RISK MITIGATION

### Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|-----------|
| Hook conflicts | Low | High | Test each hook in isolation |
| Serialization format mismatch | Low | High | Use decompiled code directly |
| Character API variance | Medium | Medium | Reverse-engineer exact signatures |
| VFS path issues | Medium | Low | Comprehensive path testing |
| Performance degradation | Low | Medium | Profile before/after each phase |

### Scope Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|-----------|
| Missing API function | Medium | Low | Mock missing functions in Lua |
| Mod incompatibility | Medium | Medium | Support common API subset first |
| Save format changes | Low | High | Version serialization format |

---

## SUCCESS METRICS

### Must-Have (MVP)
- [ ] RLSwordigo 7.0 loads without crashes
- [ ] Save/load works across sessions
- [ ] Bauble system functional
- [ ] UI buttons respond to clicks
- [ ] All core APIs working

### Should-Have
- [ ] Configuration via mini.toml
- [ ] Multiple save profiles
- [ ] Cross-platform (Windows/Linux/Mac)
- [ ] Performance ≤5% slower than vanilla

### Nice-to-Have
- [ ] Mod package system
- [ ] Mod store integration
- [ ] In-game mod management UI
- [ ] Mod compatibility checker

---

## FILE CHECKLIST

### Files to Create
- [ ] src/sre/sre_mini_api.c/h
- [ ] src/sre/sre_lni.c/h
- [ ] src/sre/sre_config.c/h
- [ ] src/sre/sre_vfs.c/h
- [ ] src/sre/sre_ui.c/h
- [ ] src/sre/sre_srlz.c/h
- [ ] src/lua/db.lua
- [ ] src/lua/srlz.lua
- [ ] src/lua/touch.lua
- [ ] src/lua/touchable.lua
- [ ] src/lua/baubles.lua
- [ ] src/lua/baublelib.lua
- [ ] mods/rlswordigo_7.0/main.lua
- [ ] mods/rlswordigo_7.0/mini.toml

### Files to Modify
- [ ] src/sre/sre_init.c (add hook registration)
- [ ] src/sre/sre_scene_update.c (add character API)
- [ ] src/sre/sre_lua_libs.c (add library registration)
- [ ] src/sre/sre_mini_api.c (expand existing)
- [ ] BUILD.md (document new build steps)

### External Dependencies
- [ ] toml-c library (add to deps/)
- [ ] LuaFileSystem (already in reference/)
- [ ] LuaSocket C portion (already in reference/)

---

## NEXT PHASE: IMPLEMENTATION

Once this master plan is approved:
1. ✅ Research complete (THIS DOCUMENT)
2. ⏳ Detailed implementation specifications for each phase
3. ⏳ Code review checkpoints
4. ⏳ Integration testing procedures
5. ⏳ Performance benchmarking

**Status**: READY FOR IMPLEMENTATION - DO NOT MODIFY WITHOUT APPROVAL

