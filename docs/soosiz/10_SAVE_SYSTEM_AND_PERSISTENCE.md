# 10: Save System & Persistence Interceptor (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/10_SAVE_SYSTEM_AND_PERSISTENCE.md`  
> **Status:** Remastered Persistence Interceptor Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & Persistence Interception

The **`SoosizHD`** binary persists data using two APIs:
1. **`NSUserDefaults` (Plist Storage):** Audio volume options, keybind settings, selected world.
2. **`database.sqlite` (SQLite Database):** Level progress, Buddy medal counts, high scores, coin totals.

Our loader's dyld symbol resolver traps these API calls in the binary and routes persistent data to standard desktop Linux paths:  
`~/.config/openswordigo/soosiz/`

---

## 2. SQLite Symbol Redirection (`libsqlite3.dylib`)

When `SoosizHD` calls `sqlite3_open("database.sqlite", &db)`:

```cpp
// Mach-O SQLite Symbol Hook
int Hook_sqlite3_open(const char* filename, sqlite3** ppDb) {
    char targetPath[512];
    snprintf(targetPath, sizeof(targetPath), "%s/.config/openswordigo/soosiz/database.sqlite", getenv("HOME"));

    // Ensure save directory exists
    mkdir_recursive("~/.config/openswordigo/soosiz/");

    // Copy template database.sqlite from SoosizHD_assets if missing
    if (access(targetPath, F_OK) != 0) {
        CopyAssetFile("database.sqlite", targetPath);
    }

    printf("[SQLite Hook] Redirected database connection to: %s\n", targetPath);
    return sqlite3_open(targetPath, ppDb);
}
```

---

## 3. `NSUserDefaults` Interception

When `SoosizHD` calls `-[NSUserDefaults standardUserDefaults]`:
- Trapped by `objc_msgSend` shim.
- Getter/setter methods (`integerForKey:`, `setInteger:forKey:`, `synchronize`) read and write to `~/.config/openswordigo/soosiz/soosiz_settings.toml` via `src/sre/sre_config.c`.
