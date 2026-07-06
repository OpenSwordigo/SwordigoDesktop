# Implementation Challenges & Solutions

**Purpose**: Technical deep-dive on specific problems and recommended solutions

**Status**: Research Complete - Detailed Solutions Provided

---

## Challenge 1: Hook System Adaptation (GlossHook → SRE)

### Problem
SwKiwi uses **GlossHook** (Android function interception via ARM instruction patching). SRE has its own hooking system using Unicorn emulator + trampoline stubs. How to map SwKiwi's hooks to SRE?

### Analysis
- **GlossHook approach**: Insert ARM64 instructions at function entry, jump to hook
- **SRE approach**: Hijack function calls at a higher level (loader + trampolines)
- **Key difference**: GlossHook modifies binary at runtime; SRE patches at load time

### Recommended Solution

**Use SRE's existing hooking, but reorganize it**:

1. **Identify function signatures** from SwKiwi headers
2. **Map to SRE equivalents** (already exist in sre_gui.c, sre_scene_update.c, etc.)
3. **Extend SRE hooks** to call Lua APIs at the right points

**Example**:
```c
// Instead of GlossHook's "Scene_Create" hook,
// use SRE's existing scene lifecycle:

// sre_scene_update.c (existing)
void sre_on_scene_loaded(scene_ptr scene)
{
    // This is already called when scene loads
    // Just inject Lua API registration here:
    if (g_scene_needs_lua_init) {
        sre_lua_register_mini(g_lua_state);
        sre_lua_register_lni(g_lua_state);
    }
}
```

**Cost**: Low (2-3 days) - reuse existing SRE infrastructure

---

## Challenge 2: Virtual Filesystem on PC

### Problem
- Android has `/ExternalFiles/`, `/Files/`, `/Cache/` paths
- PC has different filesystem layout (home, documents, app data)
- How to translate MiniPaths transparently?

### Analysis
**Android paths**:
- `/Files/Documents/{pid}.lua` → `/data/user/0/<pkg>/files/Documents/{pid}.lua`
- `/ExternalFiles/resources/` → `/sdcard/Android/data/<pkg>/files/resources/`

**PC equivalents**:
- `/Files/Documents/` → `~/.swordigo/` (Linux/Mac) or `%APPDATA%/Swordigo/` (Windows)
- `/ExternalFiles/` → `~/Documents/Swordigo/` or `%USERPROFILE%/Documents/Swordigo/`

### Recommended Solution

**Create platform-agnostic translation layer**:

```c
// sre_vfs.c

#ifdef _WIN32
  #define VFS_APP_DATA    "%APPDATA%/Swordigo"
  #define VFS_DOCUMENTS   "%USERPROFILE%/Documents/Swordigo"
#elif __APPLE__
  #define VFS_APP_DATA    "~/Library/Application Support/Swordigo"
  #define VFS_DOCUMENTS   "~/Documents/Swordigo"
#else  // Linux
  #define VFS_APP_DATA    "~/.swordigo"
  #define VFS_DOCUMENTS   "~/Documents/Swordigo"
#endif

char* sre_vfs_translate_path(const char* minipath)
{
    if (strncmp(minipath, "/Files/", 7) == 0) {
        // /Files/Documents/... → VFS_APP_DATA/Documents/...
        const char* subpath = minipath + 7;
        return format("%s/%s", VFS_APP_DATA, subpath);
    }
    else if (strncmp(minipath, "/ExternalFiles/", 15) == 0) {
        // /ExternalFiles/... → VFS_DOCUMENTS/...
        const char* subpath = minipath + 15;
        return format("%s/%s", VFS_DOCUMENTS, subpath);
    }
    else if (strncmp(minipath, "/Cache/", 7) == 0) {
        // /Cache/... → VFS_APP_DATA/cache/...
        const char* subpath = minipath + 7;
        return format("%s/cache/%s", VFS_APP_DATA, subpath);
    }
    
    return minipath;  // Fallback
}

// Auto-create directories if needed
int sre_vfs_ensure_path(const char* minipath)
{
    char* real_path = sre_vfs_translate_path(minipath);
    
    // Extract directory
    char* dir = dirname(real_path);
    
    // Expand ~ to home directory
    char expanded[PATH_MAX];
    sre_vfs_expand_home(dir, expanded);
    
    // Create directory tree
    return sre_mkdir_recursive(expanded);
}
```

**Cost**: Low-Medium (3-4 days) - straightforward path mapping

---

## Challenge 3: Character API Implementation

### Problem
RLSW uses Character.* functions that are hardcoded in vanilla engine:
- `Character.NumCoins()` - Get Soul Shards
- `Character.SetNumCoins()` - Set currency
- `Character.HasFlag()` - Check persistent flag
- `Character.SetFlag()` - Set persistent flag
- `Character.HasItem()` - Check inventory

These are C/C++ functions in libswordigo.so. How to intercept and make them work with our DB system?

### Analysis
**Two approaches**:
1. **Hook the originals**: Intercept C++ calls, redirect to Lua DB system
2. **Wrap them**: Leave C++ intact, layer Lua DB on top

**SwKiwi's approach**: Hooks the originals, completely replaces them with Lua wrappers

**Risk**: Existing code might depend on C++ behavior (performance, atomicity, etc.)

### Recommended Solution

**Hybrid approach - Minimal hooking**:

```c
// Hook only the critical functions
// Let them store in BOTH C++ AND Lua DB

volatile int g_character_coins = 0;  // Mirror in SRE global

// Hook: Character::NumCoins()
DL_HOOK_SYMBOL(Character_NumCoins, "...", int, (void *self))
{
    int coins = orig_Character_NumCoins(self);
    g_character_coins = coins;  // Mirror to SRE
    return coins;
}

// Hook: Character::SetNumCoins(int amount)
DL_HOOK_SYMBOL(Character_SetNumCoins, "...", void, (void *self, int amount))
{
    orig_Character_SetNumCoins(self, amount);
    g_character_coins = amount;  // Mirror to SRE
}

// Lua API: Read from mirror
int sre_lua_character_num_coins(lua_State* L)
{
    lua_pushinteger(L, g_character_coins);
    return 1;
}

// Lua API: Write to mirror + database
int sre_lua_character_set_num_coins(lua_State* L)
{
    int amount = lua_tointeger(L, 1);
    g_character_coins = amount;
    
    // Also call C++
    character_set_coins_native(amount);
    
    // Persist in DB
    DB.SS = amount;
    
    return 0;
}
```

**Lua bindings**:
```lua
Character = {
    NumCoins = sre_lua_character_num_coins,
    SetNumCoins = sre_lua_character_set_num_coins,
    HasFlag = sre_lua_character_has_flag,
    SetFlag = sre_lua_character_set_flag,
    HasItem = sre_lua_character_has_item,
}
```

**Cost**: Medium (5-6 days) - need to reverse-engineer all signatures

---

## Challenge 4: Serialization Format

### Problem
RLSW uses custom Lua serialization (Srlz.scl), not JSON or binary. Format includes:
- Circular reference handling
- Multiline strings
- Special numbers (NaN, infinity)
- Metadata (comments, formatting)

Where do we get the serialization code for PC port?

### Analysis
**Option 1**: Copy decompiled Srlz.scl → Load as Lua module
**Option 2**: Re-implement in C for performance
**Option 3**: Use simpler format (JSON), break compatibility

**SwKiwi's approach**: Uses full Lua implementation (works perfectly)

### Recommended Solution

**Option 1 - Load decompiled code**:

```lua
-- src/lua/srlz.lua
-- Copy the decompiled Srlz.scl code here

Srlz = {}

function Srlz.serialize(data, options)
    -- Full implementation from rlsw decompiled code
    -- Handles: circular refs, formatting, special numbers
end

function Srlz.deserialize(data, options)
    -- Full implementation
    -- Safe mode: Sandboxed environment
end

-- db.lua uses it:
local serialized = Srlz.serialize(DB)
file:write(serialized)

-- Load:
local data = Srlz.deserialize(file_content)
```

**Fallback option** (if performance issues):
- Implement `serialize_simple()` in C
- Only handles simple tables, numbers, strings
- For critical data only

**Cost**: Low (1-2 days) - decompiled code ready to use

---

## Challenge 5: Profile Management

### Problem
SwKiwi supports multiple save profiles, each with its own:
- Database file (`{pid}.lua`)
- Flags and inventory
- Resources override directory

How to implement multi-profile on PC?

### Analysis
**Android**: Profile ID via Mini.GetProfileID() from game engine

**PC Port**: No native game profile system. Need to:
1. Track "current profile" somewhere
2. Allow user to switch profiles
3. Persist profile preference

### Recommended Solution

**Profile manager**:

```c
// sre_profile.c

typedef struct {
    char id[64];            // "default", "profile1", etc.
    char name[256];         // Display name
    time_t created;         // Creation timestamp
    time_t last_played;     // Last play time
} sre_profile_t;

#define MAX_PROFILES 32
static sre_profile_t g_profiles[MAX_PROFILES];
static int g_profile_count = 0;
static int g_current_profile = 0;

// Load profile list from ~/.swordigo/profiles.toml
int sre_profile_load_list()
{
    FILE* f = fopen("~/.swordigo/profiles.toml", "r");
    // Parse TOML, populate g_profiles[]
}

// Set active profile
void sre_profile_set_current(int index)
{
    g_current_profile = index;
    // Update DB path, resource path, etc.
}

// Lua API
const char* sre_lua_get_profile_id(lua_State* L)
{
    lua_pushstring(L, g_profiles[g_current_profile].id);
    return 1;
}
```

**Lua access**:
```lua
function Mini.GetProfileID()
    return sre_lua_get_profile_id()
end
```

**Cost**: Medium (4-5 days) - need UI for profile selection

---

## Challenge 6: UI Button Implementation

### Problem
RLSW expects:
- `Scene.CreateObject("touchable")` → Spawn button object
- `obj:setPosition(x, y)` → Position on screen
- `OnTouch` event handler → Called on click
- Raycast-like detection with `TouchRadius`

But SRE runs game at 30-60 FPS on emulated ARM. How to make UI responsive without lag?

### Analysis
**Problem**: 
- Touch events from host (OS-level clicks)
- Need to raycast against scene objects (expensive)
- Frame rate varies, UI must stay responsive

**SwKiwi's approach**:
- Native touch event handler
- DirectX/OpenGL raycast from cursor
- Called at OS event time, not game time

### Recommended Solution

**Event-driven approach** (instead of per-frame polling):

```c
// sre_input.c

// Register touch callback at OS level
void sre_on_mouse_click(float screen_x, float screen_y)
{
    // Convert screen coords to game world coords
    float world_x, world_y;
    sre_screen_to_world(screen_x, screen_y, &world_x, &world_y);
    
    // Find touched objects
    for (int i = 0; i < active_objects; i++) {
        if (is_within_radius(world_x, world_y, obj[i])) {
            // Trigger OnTouch event in Lua
            sre_lua_call_ontouchevent(L, obj[i], world_x, world_y);
        }
    }
}

// Lua bindings
int sre_lua_set_touch_radius(lua_State* L)
{
    // obj.SetTouchRadius(30)
    // Store radius in object
}

int sre_lua_on_touch_callback(lua_State* L)
{
    // obj.OnTouch = function(...) end
    // Store callback
}
```

**Cost**: Medium (5-6 days) - coordinate system transforms + testing

---

## Challenge 7: Performance Optimization

### Problem
Running Lua mods on emulated ARM might be slower than native Android. Concerns:
- Save/load latency (disk I/O)
- Serialization overhead
- Physics simulation
- Lua GC pauses

### Analysis
**Expected overhead**:
- Serialization: ~10-50ms per save (10KB-1MB DB)
- Deserialization: ~5-20ms per load
- Physics simulation: Same as vanilla (no change)
- Lua execution: Same (interpreting same code)

**Likely bottlenecks**:
1. Disk I/O (file writes block game)
2. Lua garbage collection (rare, but can stutter)
3. Large table serialization (if DB grows >10MB)

### Recommended Solution

**Async I/O**:

```lua
-- db.lua (async save)

local save_queue = {}
local save_thread = nil

function db.init()
    -- ... existing code ...
    
    -- Start async save thread
    save_thread = newThread("db_async_save", function()
        while true do
            Program.Wait(0.1)
            
            if #save_queue > 0 then
                local task = table.remove(save_queue, 1)
                sre_async_write_file(task.path, task.content)
            end
        end
    end)
end

-- Queue save (non-blocking)
function db.queue_save()
    local path = db_path_fmt:format(current_pid)
    local content = Srlz.serialize(DB)
    
    table.insert(save_queue, {
        path = path,
        content = content
    })
end
```

**Optimization checklist**:
- [ ] Profile save time: `print("Save took " .. (time() - start) .. "ms")`
- [ ] Monitor GC pauses: Check `collectgarbage("count")`
- [ ] Cache serialized strings: Don't re-serialize unchanged data
- [ ] Compress large DBs: Use gzip for files >5MB

**Cost**: Low-Medium (3-4 days) - profiling + optimization

---

## Challenge 8: Cross-Platform Compatibility

### Problem
Mods might have platform-specific code or assumptions:
- Paths use `/` (Unix) vs `\` (Windows)
- Filenames case-sensitive on Linux, not on Windows
- Graphics APIs differ (OpenGL vs Direct3D)

### Analysis
**RLSW specifically**:
- Uses Lua paths (already cross-platform)
- Uses MiniPaths (abstracted)
- No graphics API calls (uses engine)

**Risk**: Low - RLSW probably already portable

### Recommended Solution

**Ensure consistent paths**:

```c
// sre_path.c

// Convert any path to canonical form
void sre_path_canonicalize(const char* path, char* out)
{
    // Replace \ with / (normalize)
    // Remove redundant ., ..
    // Expand ~
    // Resolve symlinks (on Unix)
}

// Case-insensitive file lookup on Windows
FILE* sre_path_find_file(const char* path)
{
    #ifdef _WIN32
        // Try exact case first
        FILE* f = fopen(path, "r");
        if (f) return f;
        
        // Try case-insensitive lookup
        char lower_path[MAX_PATH];
        strcpy(lower_path, path);
        for (char* p = lower_path; *p; p++) *p = tolower(*p);
        
        // Enumerate directory, find case-insensitive match
        // ...
    #endif
    
    return fopen(path, "r");
}
```

**Cost**: Low (2-3 days) - straightforward path normalization

---

## Challenge 9: Mod Loading Order

### Problem
If multiple mods want to hook the same functions, what's the execution order?
- RLSW expects certain systems to be initialized first
- Other mods might expect RLSW to be available

How to manage dependency chains?

### Analysis
**SwKiwi's approach**: Single modloader, mods loaded sequentially, no multi-mod support (in SwMini)

**RLSW assumption**: It's the only mod (or main mod)

### Recommended Solution

**Simple load order**:

```lua
-- mod_loader.lua

-- Load order (hardcoded for now)
local mod_load_order = {
    "core",           -- Core APIs
    "rlswordigo_7.0", -- Main mod
    "user_mods"       -- User mods (optional)
}

function load_all_mods()
    for i, mod in ipairs(mod_load_order) do
        print("Loading mod: " .. mod)
        dofile("/mods/" .. mod .. "/main.lua")
    end
end

-- Hook: Called when Lua state is ready
Scene.OnLoadMods = function()
    load_all_mods()
end
```

**Future enhancement** (not needed for MVP):
- Dependency declaration: `requires = {"core", "rlsw"}`
- Topological sort to handle arbitrary order

**Cost**: Low (1-2 days) - simple implementation

---

## Challenge 10: Development Workflow

### Problem
Testing mods on PC requires:
1. Modify Lua file
2. Reload game
3. Test in-game

How to speed up iterate? Can we hot-reload?

### Analysis
**Hot-reload challenge**:
- Lua state is already running
- Functions replaced, but old closures still reference old versions
- State might be corrupt (GC issues)

**SwKiwi approach**: No hot-reload (reload entire app)

### Recommended Solution

**Dev mode with file watcher**:

```lua
-- dev_mode.lua (only loaded in dev)

function watch_lua_files()
    local last_mod = {}
    
    while true do
        Program.Wait(1.0)  -- Check every second
        
        for mod_name in ("rlswordigo_7.0", "user_mods") do
            local file = "/mods/" .. mod_name .. "/main.lua"
            local mtime = get_file_mtime(file)
            
            if mtime > (last_mod[mod_name] or 0) then
                print("File changed, reloading: " .. mod_name)
                
                -- Clear old module
                package.loaded[mod_name] = nil
                
                -- Reload
                dofile(file)
                
                last_mod[mod_name] = mtime
            end
        end
    end
end

-- Hook into init
if is_dev_mode() then
    newThread("lua_watcher", watch_lua_files)
end
```

**Enable via environment variable**:
```bash
$ SWORDIGO_DEV=1 ./game
```

**Cost**: Low (2-3 days) - simple file watcher

---

## SUMMARY TABLE

| Challenge | Solution | Effort | Risk | Notes |
|-----------|----------|--------|------|-------|
| Hook system | Reuse SRE infrastructure | 2-3d | Low | Already have hooks |
| Virtual FS | Path translation layer | 3-4d | Low | Straightforward |
| Character API | Hook + dual-store | 5-6d | Med | Need signatures |
| Serialization | Load decompiled code | 1-2d | Low | Ready to use |
| Profiles | Profile manager | 4-5d | Med | Need UI |
| UI Buttons | Event-driven raycast | 5-6d | Med | Coordinate transforms |
| Performance | Async I/O + profiling | 3-4d | Low | Optimize as needed |
| Cross-platform | Path canonicalization | 2-3d | Low | Simple changes |
| Mod loading | Simple load order | 1-2d | Low | MVP enough |
| Dev workflow | File watcher | 2-3d | None | Optional |

**Total Effort**: ~30-35 days (5-6 weeks)
**Total Risk**: Low to Medium
**Recommended**: Implement in phase order, test thoroughly between phases

