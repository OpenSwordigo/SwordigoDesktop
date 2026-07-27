# 06: Asset Pipeline & VFS File I/O Interceptor (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/06_ASSET_PIPELINE_AND_PB_FORMAT.md`  
> **Status:** Remastered VFS Redirection & Asset Hook Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & Major Simplification

Because we execute the original **`SoosizHD`** binary directly using Dynarmic ARM32 JIT, **we do NOT need to reverse-engineer or write a custom `.pb` binary parser from scratch!**

The binary contains Touch Foo's original, battle-tested `.pb` parsing routines compiled inside `__TEXT.__text`. All that is required is intercepting C file I/O calls (`fopen`, `open`, `stat`, `access`) and redirecting asset path requests to the `SoosizHD_assets` directory managed by OpenSwordigo's **`sre_vfs.c`**.

---

## 2. File I/O Intercept Map (`src/sre/sre_vfs.c`)

When `SoosizHD` binary loads levels (`bnw_level1.pb`), textures (`hero_atlas1.png`), fonts (`font_markerFeltLarge.pb`), or audio (`jump.caf`):

```cpp
// Mach-O C File I/O Interceptor Hook
FILE* Hook_fopen(const char* filename, const char* mode) {
    char resolvedPath[512];
    
    // Normalize relative iOS path ("bnw_level1.pb" -> "/.../SoosizHD_assets/bnw_level1.pb")
    if (filename[0] != '/') {
        snprintf(resolvedPath, sizeof(resolvedPath), "%s/%s", 
                 sre_vfs_get_asset_dir(), filename);
    } else {
        strncpy(resolvedPath, filename, sizeof(resolvedPath));
    }

    printf("[VFS Hook] Opening Asset: %s\n", resolvedPath);
    return fopen(resolvedPath, mode);
}
```

---

## 3. Asset Package Mapping Summary

The binary seamlessly loads all original asset files via our VFS hook:

| Asset Category | Extension | Target Directory | Binary Processing Routine |
| :--- | :---: | :--- | :--- |
| **Level Geometry & Templates** | `.pb` | `SoosizHD_assets/` | Parsed natively by `SoosizHD` binary routines in JIT. |
| **World Theme Descriptors** | `.desc` | `SoosizHD_assets/` | Read natively by `SoosizHD` theme loader. |
| **Texture Atlases** | `.png` / `.jpg` | `SoosizHD_assets/` | Decoded by binary's image loader & bound to GL textures. |
| **Sound Effects & Music** | `.caf` / `.mp3` | `SoosizHD_assets/` | Streamed via OpenAL / AudioToolbox symbol hooks. |
| **SQLite Database** | `.sqlite` | `~/.config/openswordigo/soosiz/` | Handled by intercepted `sqlite3_open` calls. |

---

## 4. Key Takeaways & Advantages

1. **Zero Data Discrepancies:** Native binary parsing guarantees 100% precision for level geometry, entity spawner positions, and collision polygon boundaries.
2. **Zero Maintenance Burden:** Future level mods or asset edits in `.pb` format will automatically work without updating parser code.
