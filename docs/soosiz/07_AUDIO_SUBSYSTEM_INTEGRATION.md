# 07: Audio Subsystem Interceptor (Remastered)

> **Location:** `/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDesktop/docs/soosiz/07_AUDIO_SUBSYSTEM_INTEGRATION.md`  
> **Status:** Remastered OpenAL & Audio Driver Interceptor Specification  
> **Target Binary:** `SoosizHD` (Mach-O ARMv7 Binary)

---

## 1. Overview & OpenAL Direct Binding

In **`SoosizHD`**, sound playback is handled by OpenAL (for sound effects) and `AVAudioPlayer` (for background music streams). Both APIs are imported via dyld symbol stubs in `__TEXT.__symbolstub1`.

OpenSwordigo's audio subsystem (**`src/sre/sre_music.c`**) already maintains an active OpenAL context and FMOD/SDL_mixer audio device. We intercept OpenAL and AudioToolbox symbol stubs in the binary and bind them directly to `sre_music.c`.

---

## 2. OpenAL Symbol Trap Map

```cpp
// Mach-O OpenAL Symbol Interceptors in src/sre/sre_music.c
ALCdevice* Hook_alcOpenDevice(const char* devicename) {
    // Return OpenSwordigo's global active OpenAL device
    return sre_music_get_openal_device();
}

void Hook_alBufferData(ALuint buffer, ALenum format, const ALvoid* data, ALsizei size, ALsizei freq) {
    // Forward PCM sound sample buffer directly to host OpenAL driver
    alBufferData(buffer, format, data, size, freq);
}

void Hook_alSourcePlay(ALuint source) {
    // Execute OpenAL sound effect trigger
    alSourcePlay(source);
}
```

---

## 3. Background Music Interception (`MusicPlayer`)

`SoosizHD` contains the `MusicPlayer` class for driving MP3 music tracks (`Breaktime.mp3`, `Conflicted.mp3`, etc.).

When the JIT executes `- [MusicPlayer playMusic:repeated:]`:
1. `objc_msgSend` trap extracts track string argument (e.g. `"Breaktime"`).
2. Forwards track path to `sre_music_play_stream("/.../SoosizHD_assets/Breaktime.mp3", SRE_MUSIC_LOOP)`.
3. OpenSwordigo streams the MP3 audio background track with volume control and smooth fading.

---

## 4. Key Takeaways

- **Zero Latency:** Direct OpenAL passthrough provides zero-latency sound effects for jumping, coin pickups, and enemy hits.
- **Minimal Code Effort:** Requires only ~120 lines of C symbol wrappers in `sre_music.c`.
