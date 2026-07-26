# Vita Compatibility Matrix

Evaluation of Swordigo versions against the known Vita loader source code.

| Vita Loader Expectation | Swordigo 1.1 | Swordigo 1.4.6 | Notes |
|---|---|---|---|
| **`setFilesDir`** | OK | OK | |
| **`setCacheDir`** | OK | OK | |
| **`setAssetManager`** | OK | OK | |
| **`googleSignInCompleted`** | **FAIL** | OK | Required for boot in Vita `main.c` |
| **`handleApplicationLaunch`** | **FAIL** | OK | Required for boot in Vita `main.c` |
| **`setupNativeInterface`** | OK | OK | |
| **`setupApplication`** | OK | OK | |
| **`reloadContext`** | **FAIL** | OK | Used in 1.4.6 `onSurfaceCreated` |
| **`MusicPlayer_initMusicPlayer`** | OK | OK | |

## Conclusion
Swordigo **1.4.6** is the only version compatible with the logic found in the `swordigo-vita-master` repository. Version 1.1 lacks the necessary state management and initialization callbacks introduced in later Android updates.
