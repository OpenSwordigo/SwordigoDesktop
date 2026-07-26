# Swordigo OpenSwordigo Research: Sound Library, SFX Intervals & Music Playlists

## 1. Sound Library & SFX Engine (`SoundEffect` & `SoundLibrary`)

Sound effects (sword slashes, enemy grunts, spell impacts, coin pickups) enforce minimum playback intervals to prevent audio clipping during rapid event triggers.

```cpp
namespace Caver {

struct SoundEffect {
    std::string effect_id;        // Tag 0x0A ("Name")
    std::string resource_name;    // Tag 0x0C ("ResourceName", .wav / .caf / .ogg)
    float volume = 1.0f;          // Tag 0x1D
    float min_play_interval = 0.05f; // Tag 0x19 (Seconds between triggers)
    float last_played_time = 0.0f;
};

class SoundEngine {
public:
    static SoundEngine& Instance() {
        static SoundEngine instance;
        return instance;
    }

    void PlaySound(const std::string& effect_id, float current_time) {
        auto it = m_effects.find(effect_id);
        if (it != m_effects.end()) {
            SoundEffect& sfx = it->second;
            if (current_time - sfx.last_played_time >= sfx.min_play_interval) {
                sfx.last_played_time = current_time;
                // Issue hardware audio buffer playback command
            }
        }
    }

private:
    std::unordered_map<std::string, SoundEffect> m_effects;
};

} // namespace Caver
```

---

## 2. Music Playlist System (`MusicPlaylist` & `MusicTrack`)

Background music streams loop track resources tied to active world map zones.

```cpp
namespace Caver {

struct MusicTrack {
    std::string resource_name;   // Tag 0x0A (.mp3 / .ogg)
    float volume = 1.0f;         // Tag 0x0F
};

struct MusicPlaylist {
    std::string playlist_name;   // Tag 0x0A
    std::vector<MusicTrack> tracks; // Tag 0x0C
};

} // namespace Caver
```
