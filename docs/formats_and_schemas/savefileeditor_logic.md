# Swordigo Save Editor Research

## Save File Locations
- **Desktop Port:** The save files are typically stored in:
  - Linux: `$XDG_DATA_HOME/swordigo-desktop/save/Documents/` (usually `~/.local/share/swordigo-desktop/save/Documents/`)
  - Windows: `%LOCALAPPDATA%/swordigo-desktop/save/Documents/`
- **Format:** Protobuf message.
- **Filename:** A UUID with `.gplayer` extension (e.g., `e578c008-465e-43ab-bad6-4a56ea7ea34c.gplayer`).
- **Legacy/Backup:** Sometimes found as `snapshot.bin` in the `save/` root.

## Save File Structure (Protobuf Mapping)

### Top-level Structure (`PlayerProfile`)
| Field | Type | Name (Likely) | Description |
|---|---|---|---|
| 1 | String | `name` | Profile name (often empty/unused) |
| 2 | Varint | `level` | Player experience level |
| 3 | Fixed64 | `lastPlayed` | Timestamp (double/fixed64) |
| 4 | Message | `gameState` | The main game state data |
| 9 | String | `levelTitle` | Current level/area name (e.g., "Cairnwood Village") |
| 14 | String | `identifier` | Profile UUID |

### `GameState` Structure (Top-level Field 4)
| Field | Type | Name | Description |
|---|---|---|---|
| 1 | Message | `charState` | Player attributes and inventory |
| 2 | Message | `sceneStates` | Repeated states for each visited scene |
| 3 | String | `currentScene` | ID of the current scene (e.g., `town_herohouse`) |
| 4 | String | `spawnPoint` | ID of the spawn point in the scene |
| 9 | String | `menuState` | Current UI/Map state |

### `CharacterState` Structure (GameState Field 1)
| Field | Type | Name | Description |
|---|---|---|---|
| 2 | Varint | `health` | Current health |
| 4 | Varint | `coins` | Current Soul Shards |
| 5 | Varint | `maxHealth` | Maximum health |
| 11 | Message | `items` | Repeated submessages containing item IDs (e.g., `legendsword`) |
| 15 | String | `spells` | Repeated spell IDs (e.g., `bolt`, `bomb`) |
| 16 | String | `activeSpell` | Currently selected spell |
| 17 | String | `activeTrinket` | Currently selected trinket |

## Implementation Plan

### 1. Research & Tooling
- Use `protoc --decode_raw` to verify the structure of any given save file.
- Since we don't have the official `.proto`, we will use a "dynamic" approach or a minimal `.proto` file that matches our findings.

### 2. Backend (Python)
- **Library:** `protobuf` (using `google.protobuf.reflection` or just raw stream manipulation).
- **Core Logic:**
  - `load_save(path)`: Decodes the binary data into a Python object/dictionary.
  - `save_save(path, data)`: Encodes the data back to binary.
  - `modify_field(data, field_path, value)`: Helper to update specific fields.

### 3. GUI (Python + Tkinter/Custom)
- **File Browser:** Select the `.gplayer` file.
- **Main Editor Tabs:**
  - **Stats:** Health, Coins, Level.
  - **Inventory:** List of items and spells with "Add/Remove" buttons.
  - **Location:** Dropdown or text field for Scene and Spawn Point.
- **Safety:** Automatically create a `.bak` backup before saving changes.

## References
- `research/savefile.md`: General save system overview.
- `research/protobuf_schema.md`: Protobuf mapping research.
- `save/Documents/*.gplayer`: Live save samples.
