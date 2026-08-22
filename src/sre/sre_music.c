/* =====================================================================
 * sre_music.c — SRE Native MusicPlayer
 * =====================================================================
 * Replaces Caver::MusicPlayer entirely. The original uses boost::shared_ptr
 * and C++ exceptions (try/catch) for playlist management — all of which
 * break under Unicorn emulation.
 *
 * Architecture:
 *   Engine calls MusicPlayer methods (PlayMusicWithName, FadeIn, etc.)
 *     → SRE intercepts → writes commands to g_sre_music_* globals
 *     → Host reads globals each frame → executes via OpenAL
 *
 * This eliminates ALL C++ exceptions from the music subsystem.
 *
 * Hook addresses (ARM64 v1.4.12):
 *   PlayMusicWithName    = 0x4811a0
 *   FadeIn               = 0x4814a8
 *   FadeOut              = 0x4815d8
 *   Update               = 0x482090
 *   SetVolume            = 0x482064
 *   SetLooping           = 0x48206c
 *   SetEnabled           = 0x481e88
 *   SetSuspended         = 0x481fc0
 * =====================================================================
 */

/* No libc in guest SRE code */

/* ========== Music Command Interface ==========
 * SRE writes these globals. Host reads them each frame.
 * The host clears _pending flags after processing. */

/* Load command: SRE writes name + sets pending flag */
char g_sre_music_load_name[256] = {0};  /* Music name to load (e.g., "wastelands") */
int  g_sre_music_load_pending = 0;      /* 1 = host should load this file */
int  g_sre_music_load_restart = 0;      /* 1 = restart even if same track */

/* Playback commands */
int  g_sre_music_play_pending = 0;      /* 1 = host should play */
int  g_sre_music_pause_pending = 0;     /* 1 = host should pause */
int  g_sre_music_stop_pending = 0;      /* 1 = host should stop */

/* Volume/looping — host reads these continuously */
float g_sre_music_volume = 1.0f;        /* Current effective volume */
int   g_sre_music_volume_dirty = 0;     /* 1 = volume changed, host should apply */
int   g_sre_music_looping = 1;          /* Loop flag */
int   g_sre_music_looping_dirty = 0;    /* 1 = looping changed */

/* ========== LNI Command Interface ========== */
extern volatile char g_sre_lni_command[64];
extern volatile char g_sre_lni_arg1[512];
extern volatile char g_sre_lni_arg2[512];
extern volatile int  g_sre_lni_pending;

/* ========== Internal State ========== */
float g_sre_music_master_volume = 1.0f;  /* Engine-set master volume */
static float s_fade_volume = 1.0f;       /* Current fade multiplier (0..1) */
static float s_fade_target = 1.0f;       /* Fade destination */
static float s_fade_speed = 0.0f;        /* Fade rate per second */
static int   s_enabled = 1;              /* Music enabled by engine */
static int   s_suspended = 0;            /* Music suspended (app backgrounded) */
static int   s_playing = 0;              /* Track is loaded and playing */

/* Current music name — for duplicate detection */
static char s_current_name[256] = {0};

/* ========== Helper: string copy ========== */
static void sre_strcpy(char* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static int sre_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int sre_strncmp(const char* s1, const char* s2, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return s1[i] - s2[i];
        if (s1[i] == '\0') break;
    }
    return 0;
}

/* ========== Pending track — for fade transitions ========== */
static char s_pending_name[256] = {0};
static int  s_has_pending = 0;
static int  s_pending_restart = 0;

/* Fade speed for automatic transitions (matches original: 1.0 / 1.5 ≈ 0.667) */
#define FADE_TRANSITION_SPEED 0.667f

/* ========== AudioSystem::SetMusicVolume ==========
 * Original: Caver::AudioSystem::SetMusicVolume(float)
 * nm offset: 0x47f5f0
 *
 * ARM64 ABI:
 *   X0 = this (AudioSystem*)
 *   S0 = float volume (0.0 to 1.0)
 *
 * Called by the engine when the music volume slider changes.
 * The original calls MusicPlayer::SetVolume → MusicPlayerJNI::SetVolume
 * → broken JNI bridge. We bypass all of that and route directly to
 * our OpenAL music system via the command interface.
 */
void sre_AudioSystem_SetMusicVolume(void* self, float vol) {
    /* Clamp to valid range */
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    /* Update internal state for fade calculations */
    g_sre_music_master_volume = vol;

    /* Write to command interface — host picks this up and applies via OpenAL */
    g_sre_music_volume = vol * s_fade_volume;
    g_sre_music_volume_dirty = 1;
}

/* ========== PlayMusicWithName ==========
 * Original: Caver::MusicPlayer::PlayMusicWithName(const std::string&, bool)
 * nm offset: 0x4811a0
 *
 * ARM64 ABI:
 *   X0 = this (MusicPlayer*)
 *   X1 = const std::string& name (pointer to SreString)
 *   X2 = bool restart (0 or 1)
 *
 * Behavior (matching original decompilation):
 *   If already playing same track → skip
 *   If currently playing something else → fadeout, queue new track as pending
 *   If nothing playing → load immediately with fadein
 */
void sre_PlayMusicWithName(void* self, void* name_ref, int restart) {
    if (!name_ref) return;

    /* Read the std::string data pointer */
    char* data = *(char**)name_ref;
    if (!data) return;

    /* The engine uses GNU COW std::string, whose object contains only the data
     * pointer. Do not read name_ref+8 as a libc++ length: it belongs to the
     * next object and can make us consume arbitrary track-name bytes. */
    sre_u64 len = 0;
    while (data[len] && len < 255) len++;
    if (len == 0) return;

    /* Build name string */
    char new_name[256];
    sre_u64 i;
    for (i = 0; i < len && i < 255; i++) {
        new_name[i] = data[i];
    }
    new_name[i] = 0;

    /* Intercept LNI commands */
    if (new_name[0] == 'L' && new_name[1] == 'N' && new_name[2] == 'I' && new_name[3] == ':') {
        const char* cmd = new_name + 4;
        if (sre_strncmp(cmd, "openURL(", 8) == 0 || sre_strncmp(cmd, "openUrl(", 8) == 0) {
            const char* start = cmd;
            while (*start && *start != '\"' && *start != '\'') start++;
            if (*start) {
                start++;
                const char* end = start;
                while (*end && *end != '\"' && *end != '\'') end++;
                if (*end) {
                    int url_len = end - start;
                    if (url_len < 511) {
                        int j;
                        for (j = 0; j < url_len; j++) g_sre_lni_arg1[j] = start[j];
                        g_sre_lni_arg1[url_len] = '\0';
                        sre_strcpy((char*)g_sre_lni_command, "openUrl", 64);
                        g_sre_lni_pending = 1;
                    }
                }
            }
        }
        else if (sre_strncmp(cmd, "SetSpeed(", 9) == 0 || sre_strncmp(cmd, "setSpeed(", 9) == 0) {
            const char* start = cmd + 9;
            int arg_len = 0;
            while (start[arg_len] && start[arg_len] != ')') arg_len++;
            if (arg_len < 511) {
                int j;
                for (j = 0; j < arg_len; j++) g_sre_lni_arg1[j] = start[j];
                g_sre_lni_arg1[arg_len] = '\0';
                sre_strcpy((char*)g_sre_lni_command, "setSpeed", 64);
                g_sre_lni_pending = 1;
            }
        }
        else if (sre_strncmp(cmd, "copyToClipboard(", 16) == 0) {
            const char* s1 = cmd;
            while (*s1 && *s1 != '\"' && *s1 != '\'') s1++;
            if (*s1) {
                s1++;
                const char* e1 = s1;
                while (*e1 && *e1 != '\"' && *e1 != '\'') e1++;
                if (*e1) {
                    const char* s2 = e1 + 1;
                    while (*s2 && *s2 != '\"' && *s2 != '\'') s2++;
                    if (*s2) {
                        s2++;
                        const char* e2 = s2;
                        while (*e2 && *e2 != '\"' && *e2 != '\'') e2++;
                        if (*e2) {
                            int l1 = e1 - s1;
                            int l2 = e2 - s2;
                            if (l1 < 511 && l2 < 511) {
                                int j;
                                for (j = 0; j < l1; j++) g_sre_lni_arg1[j] = s1[j];
                                g_sre_lni_arg1[l1] = '\0';
                                for (j = 0; j < l2; j++) g_sre_lni_arg2[j] = s2[j];
                                g_sre_lni_arg2[l2] = '\0';
                                sre_strcpy((char*)g_sre_lni_command, "copyToClipboard", 64);
                                g_sre_lni_pending = 1;
                            }
                        }
                    }
                }
            }
        }
        else if (sre_strncmp(cmd, "quit()", 6) == 0 || sre_strncmp(cmd, "Quit()", 6) == 0) {
            sre_strcpy((char*)g_sre_lni_command, "quit", 64);
            g_sre_lni_pending = 1;
        }
        return;
    }

    /* === MOD API: Check for music replacement === */
    extern const char* sre_mod_get_music_replacement(const char*);
    const char* mod_repl = sre_mod_get_music_replacement(new_name);
    if (mod_repl) {
        /* Replace track name with mod's replacement */
        sre_u64 j;
        for (j = 0; mod_repl[j] && j < 255; j++) {
            new_name[j] = mod_repl[j];
        }
        new_name[j] = 0;
        len = j;
    }
    
    
    /* Check if same track already playing OR already pending (skip reload) */
    if (!restart) {
        /* Compare against currently playing track */
        if (s_playing) {
            int same = 1;
            sre_u64 j;
            for (j = 0; j < len; j++) {
                if (new_name[j] != s_current_name[j]) { same = 0; break; }
            }
            if (same && s_current_name[len] == 0) {
                return;
            }
        }
        
        /* Compare against pending track (already queued for transition) */
        if (s_has_pending) {
            int same = 1;
            sre_u64 j;
            for (j = 0; j < len; j++) {
                if (new_name[j] != s_pending_name[j]) { same = 0; break; }
            }
            if (same && s_pending_name[len] == 0) {
                return;
            }
        }
    }
    
    /* Store as pending track */
    for (i = 0; new_name[i] && i < 255; i++) {
        s_pending_name[i] = new_name[i];
    }
    s_pending_name[i] = 0;
    s_pending_restart = restart;
    s_has_pending = 1;
    
    if (s_playing && s_fade_volume > 0.01f) {
        /* Currently playing — start fadeout, Update will handle transition */
        s_fade_target = 0.0f;
        s_fade_speed = FADE_TRANSITION_SPEED;
    } else {
        /* Nothing playing — load immediately with fadein */
        for (i = 0; s_pending_name[i] && i < 255; i++) {
            g_sre_music_load_name[i] = s_pending_name[i];
            s_current_name[i] = s_pending_name[i];
        }
        g_sre_music_load_name[i] = 0;
        s_current_name[i] = 0;
        
        g_sre_music_load_restart = restart;
        g_sre_music_load_pending = 1;
        s_has_pending = 0;
        s_playing = 1;
        
        /* Start fadein */
        s_fade_volume = 0.0f;
        s_fade_target = 1.0f;
        s_fade_speed = FADE_TRANSITION_SPEED;
        
        g_sre_music_volume = 0.0f;
        g_sre_music_volume_dirty = 1;
    }
}

/* ========== FadeIn ==========
 * Original: Caver::MusicPlayer::FadeIn(float duration)
 * nm offset: 0x4814a8
 *
 * Decompiled: this+0x20 = 1, this+0x24 = 1.0/duration
 */
void sre_MusicPlayer_FadeIn(void* self, float duration) {
    s_fade_target = 1.0f;
    if (duration > 0.01f) {
        s_fade_speed = 1.0f / duration;
    } else {
        s_fade_volume = 1.0f;
        s_fade_speed = 0.0f;
        g_sre_music_volume = g_sre_music_master_volume;
        g_sre_music_volume_dirty = 1;
    }
}

/* ========== FadeOut ==========
 * Original: Caver::MusicPlayer::FadeOut(float duration)
 * nm offset: 0x4815d8
 *
 * Decompiled: this+0x20 = -1, this+0x24 = 1.0/duration
 */
void sre_MusicPlayer_FadeOut(void* self, float duration) {
    s_fade_target = 0.0f;
    if (duration > 0.01f) {
        s_fade_speed = 1.0f / duration;
    } else {
        s_fade_volume = 0.0f;
        s_fade_speed = 0.0f;
        g_sre_music_volume = 0.0f;
        g_sre_music_volume_dirty = 1;
    }
}

/* ========== Update ==========
 * Original: Caver::MusicPlayer::Update(float deltaTime)
 * nm offset: 0x482090
 *
 * Called every frame. Handles:
 *   1. Sync master volume from MusicPlayer object (this+4)
 *      (SetVolume can't be hooked — trampoline collision)
 *   2. Apply fade transitions
 *   3. When fadeout completes: load pending track + start fadein
 */
void sre_MusicPlayer_Update(void* self, float deltaTime) {
    /* === Sync master volume from MusicPlayer object ===
     * SetVolume (0x482064) can't be hooked (8 bytes from SetLooping).
     * Original SetVolume stores volume at this+4. Read it here. */
    float obj_volume = *(float*)((char*)self + 4);
    if (obj_volume >= 0.0f && obj_volume <= 1.0f) {
        static float last_synced_obj_volume = -1.0f;
        if (obj_volume != last_synced_obj_volume) {
            g_sre_music_master_volume = obj_volume;
            last_synced_obj_volume = obj_volume;
        }
    }
    *(float*)((char*)self + 4) = g_sre_music_master_volume;
    
    /* === Sync looping flag from MusicPlayer object ===
     * SetLooping (0x48206c) can't be hooked either (same collision).
     * Original SetLooping stores at this+8. */
    int obj_looping = *(unsigned char*)((char*)self + 8);
    if (obj_looping != g_sre_music_looping) {
        g_sre_music_looping = obj_looping ? 1 : 0;
        g_sre_music_looping_dirty = 1;
    }
    
    /* === Apply fade === */
    if (s_fade_speed > 0.0f) {
        if (s_fade_volume < s_fade_target) {
            /* Fading IN */
            s_fade_volume += s_fade_speed * deltaTime;
            if (s_fade_volume >= s_fade_target) {
                s_fade_volume = s_fade_target;
                s_fade_speed = 0.0f;
            }
        } else if (s_fade_volume > s_fade_target) {
            /* Fading OUT */
            s_fade_volume -= s_fade_speed * deltaTime;
            if (s_fade_volume <= s_fade_target) {
                s_fade_volume = s_fade_target;
                s_fade_speed = 0.0f;
                
                /* Fadeout complete — handle pending or stop */
                if (s_fade_target <= 0.001f) {
                    if (s_has_pending) {
                        /* Load the queued track */
                        sre_u64 i;
                        for (i = 0; s_pending_name[i] && i < 255; i++) {
                            g_sre_music_load_name[i] = s_pending_name[i];
                            s_current_name[i] = s_pending_name[i];
                        }
                        g_sre_music_load_name[i] = 0;
                        s_current_name[i] = 0;
                        
                        g_sre_music_load_restart = s_pending_restart;
                        g_sre_music_load_pending = 1;
                        s_has_pending = 0;
                        s_playing = 1;
                        
                        /* Start fadein for new track */
                        s_fade_volume = 0.0f;
                        s_fade_target = 1.0f;
                        s_fade_speed = FADE_TRANSITION_SPEED;
                    } else {
                        /* No pending — just stop */
                        g_sre_music_stop_pending = 1;
                        s_playing = 0;
                    }
                }
            }
        }
    }
    
    /* === Apply combined volume every frame === */
    float vol = g_sre_music_master_volume * s_fade_volume;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    
    g_sre_music_volume = vol;
    g_sre_music_volume_dirty = 1;
}

/* ========== SetVolume / SetLooping ==========
 * NOT HOOKED — trampoline collision (only 8 bytes apart).
 * Original SetVolume stores at this+4, our Update reads it.
 * Original SetLooping calls JNI bridge directly (works).
 * These stay for reference / future use. */

void sre_MusicPlayer_SetVolume(void* self, float volume) {
    g_sre_music_master_volume = volume;
    float vol = g_sre_music_master_volume * s_fade_volume;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    g_sre_music_volume = vol;
    g_sre_music_volume_dirty = 1;
}

void sre_MusicPlayer_SetLooping(void* self, int looping) {
    g_sre_music_looping = looping ? 1 : 0;
    g_sre_music_looping_dirty = 1;
}

/* ========== SetEnabled ==========
 * Original: Caver::MusicPlayer::SetEnabled(bool)
 * nm offset: 0x481e88
 */
void sre_MusicPlayer_SetEnabled(void* self, int enabled) {
    s_enabled = enabled ? 1 : 0;
    
    if (!s_enabled && s_playing) {
        g_sre_music_stop_pending = 1;
    } else if (s_enabled && s_playing) {
        g_sre_music_play_pending = 1;
    }
}

/* ========== SetSuspended ==========
 * Original: Caver::MusicPlayer::SetSuspended(bool)
 * nm offset: 0x481fc0
 */
void sre_MusicPlayer_SetSuspended(void* self, int suspended) {
    s_suspended = suspended ? 1 : 0;
    
    if (s_suspended && s_playing) {
        g_sre_music_pause_pending = 1;
    } else if (!s_suspended && s_playing && s_enabled) {
        g_sre_music_play_pending = 1;
    }
}

/* ========== No-op stubs ========== */

void sre_MusicPlayer_AddPlaylist(void* self, void* playlist) {
    /* No-op — we don't use playlists */
}

void sre_MusicPlayer_RegisterProgramLibrary(void* self, void* program_state) {
    /* No-op — Lua bindings handled by our ProgramState hooks */
}
