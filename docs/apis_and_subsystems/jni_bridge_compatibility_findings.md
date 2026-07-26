# Agent 1 JNI and Compatibility Findings

## 2026-06-14 - Preservation Snapshot

This file is append-only. It preserves findings for JNI, Android compatibility, startup order, Lua registration clues, and POD/PowerVR loading.

## JNI requirements from Vita loader

The Vita loader builds fake `JavaVM` and `JNIEnv` tables before calling exported JNI functions from `libswordigo.so`.

Evidence:

- `fake_vm` is initialized at `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1703`.
- `fake_env` is initialized at `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1709`.
- The fake `JNIEnv` table entries are assigned at `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1711` through `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1752`.

Required or stubbed JNI functions visible in the Vita table:

- VM: `GetEnv`, `GetJavaVM`
- Class/object refs: `FindClass`, `NewGlobalRef`, `DeleteGlobalRef`, `DeleteLocalRef`, `NewObjectV`, `GetObjectClass`
- Method lookup/calls: `GetMethodID`, `GetStaticMethodID`, `CallObjectMethodV`, `CallBooleanMethodV`, `CallIntMethodV`, `CallLongMethodV`, `CallVoidMethodV`, `CallStaticObjectMethodV`, `CallStaticBooleanMethodV`, `CallStaticIntMethodV`, `CallStaticLongMethodV`, `CallStaticFloatMethodV`, `CallStaticVoidMethodV`
- Field lookup/access: `GetFieldID`, `GetBooleanField`, `GetIntField`, `GetFloatField`, `GetStaticFieldID`, `GetStaticObjectField`
- Strings: `NewStringUTF`, `GetStringUTFLength`, `GetStringUTFChars`, `ReleaseStringUTFChars`, `GetStringUTFRegion`
- Arrays: `GetArrayLength`, `GetObjectArrayElement`, `GetIntArrayElements`, `ReleaseIntArrayElements`, `SetIntArrayRegion`
- Frames/native registration: `PushLocalFrame`, `PopLocalFrame`, `RegisterNatives`
- Exception path: `ThrowNew` is stubbed to `ret0`

Vita maps Java method names to local IDs in `name_to_method_ids`:

- `<init>`
- `isAgeKnown`
- `getPlatformConsentState`
- `loadFile`
- `play`
- `pause`
- `stop`
- `setLooping`
- `setVolume`
- `reportAchievementProgress`

Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1334` through `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1345`.

Callback behavior implemented by Vita:

- `CallBooleanMethodV(LOAD_FILE)` calls `load_music(args[0])` and returns true. Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1501`.
- `CallVoidMethodV(PAUSE/PLAY/SET_LOOPING/SET_VOLUME/STOP)` routes to `pause_music`, `play_music`, `set_music_loop`, `set_music_volume`, and `stop_music`. Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1526`.
- `CallStaticVoidMethodV(REPORT_ACHIEVEMENT_PROGRESS)` routes to `unlock_achievement`. Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1414`.
- `CallStaticBooleanMethodV(IS_AGE_KNOWN/GET_PLATFORM_CONSENT_STATE)` returns true. Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1424`.

## Native startup order

Vita direct-call order:

1. Load `ux0:data/swordigo/libswordigo.so`
2. `so_relocate`
3. `so_resolve`
4. `patch_game`
5. `so_flush_caches`
6. `so_initialize`
7. Build fake VM/JNI tables
8. Resolve exported JNI function pointers via `so_symbol`
9. `setFilesDir`
10. `setCacheDir`
11. `setAssetManager`
12. `googleSignInCompleted` and `handleApplicationLaunch` in Vita source only; local `reference/lib/armeabi-v7a/libswordigo.so` does not export these
13. GL state setup
14. `initMusicPlayer`
15. `setupNativeInterface`
16. `setupApplication`
17. `setApplicationViewSize`
18. `applicationDidBecomeActive`
19. Main loop calls `updateApplication(deltaSecond)` then `drawApplication()`

Evidence:

- Load/relocate/resolve/init: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1693` through `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1701`.
- JNI symbol resolution: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1754` through `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1766`.
- Lifecycle calls: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1779` through `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1817`.
- Frame loop calls: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1894`.

Android Java order for local reference:

1. `MainActivity` loads `openal-soft` and `swordigo`
2. `MainActivity.onCreate`: `setFilesDir`, `setCacheDir`, `setAssetManager`
3. `GameRenderer.onSurfaceCreated`: construct `MusicPlayer`, `setupNativeInterface`, `setupApplication`
4. `GameRenderer.onSurfaceChanged`: `setApplicationViewSize`
5. `GameRenderer.onDrawFrame`: fixed-step `updateApplication`, then `drawApplication`

Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:273`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:43`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:46`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:30`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:33`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:52`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:69`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:76`.

## Android compatibility layer requirements

Native imports from local `reference/lib/armeabi-v7a/libswordigo.so` include:

- AssetManager: `AAssetManager_fromJava`, `AAssetManager_open`, `AAsset_close`, `AAsset_getLength`, `AAsset_openFileDescriptor`, `AAsset_read`
- EGL/GLES: `eglGetProcAddress`, `glActiveTexture`, `glBindBuffer`, `glBindTexture`, `glBlendFunc`, `glBufferData`, `glBufferSubData`, `glClear`, `glClearColor`, `glClearDepthf`, `glClearStencil`, `glColor4f`, `glColor4ub`, `glColorMask`, `glColorPointer`, `glCompressedTexImage2D`, `glCullFace`, `glDeleteBuffers`, `glDeleteTextures`, `glDepthFunc`, `glDepthMask`, `glDisable`, `glDisableClientState`, `glDrawArrays`, `glDrawElements`, `glEnable`, `glEnableClientState`, `glFlush`, `glGenBuffers`, `glGenTextures`, `glGetError`, `glGetIntegerv`, `glGetString`, `glLightModelfv`, `glLightf`, `glLightfv`, `glLoadIdentity`, `glLoadMatrixf`, `glMaterialfv`, `glMatrixMode`, `glNormalPointer`, `glPixelStorei`, `glPopMatrix`, `glPushMatrix`, `glScalef`, `glScissor`, `glStencilFunc`, `glStencilOp`, `glTexCoordPointer`, `glTexEnvfv`, `glTexEnvi`, `glTexImage2D`, `glTexParameterf`, `glTexParameteri`, `glTranslatef`, `glVertexPointer`, `glViewport`
- OpenAL: `alBufferData`, `alDeleteBuffers`, `alDeleteSources`, `alDistanceModel`, `alGenBuffers`, `alGenSources`, `alGetError`, `alGetSourcef`, `alGetSourcei`, `alListener3f`, `alListenerf`, `alListenerfv`, `alSource3f`, `alSourcePause`, `alSourcePlay`, `alSourceRewind`, `alSourceStop`, `alSourcef`, `alSourcei`, `alcCloseDevice`, `alcCreateContext`, `alcDestroyContext`, `alcGetCurrentContext`, `alcMakeContextCurrent`, `alcOpenDevice`, `alcProcessContext`, `alcResume`, `alcSuspend`, `alcSuspendContext`
- libc/Bionic/filesystem/threading/zlib: `open`, `read`, `write`, `close`, `fstat`, `stat`, `opendir`, `readdir`, `closedir`, `lseek`, `mkdir`, `remove`, `rename`, `pthread_*`, `gz*`, math/string/memory functions

Evidence: `readelf -Ws reference/lib/armeabi-v7a/libswordigo.so | awk '$7=="UND" {print $8}' | sort -u`.

Vita asset handling maps Android assets to files:

- `AAssetManager_open` opens `ux0:data/swordigo/assets/<name>`.
- `AAsset_openFileDescriptor` seeks and returns the same file descriptor.
- `AAsset_getLength`, `AAsset_read`, `AAsset_seek`, and `AAsset_close` are simple file-descriptor wrappers.

Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:824` through `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:860`.

## Lua/program registration clues

Native symbols confirm an internal Lua-backed program system:

- `Caver::ProgramState::RegisterLibrary`
- `Caver::ProgramState::FromLuaState`
- `Caver::ProgramState::Execute`
- `Caver::Program::LoadFromProtobufMessage`
- `Caver::ProgramComponent::RegisterLibrary`
- `Caver::SceneObjectLib::PushSceneObject`

Evidence: `readelf -Ws reference/lib/armeabi-v7a/libswordigo.so | c++filt`.

Requested Lua library names observed in scene data, but not yet fully traced to registration functions:

- `Program`
- `Scene`
- `Camera`
- `Game`
- `DoorController`
- `SoundLibrary`
- `MusicPlayer`
- `EntityController`

Evidence: `rg -a "Program.Wait|Scene.Find|Scene.CreateObject|Camera.FocusAtPoint|Camera.ResetFocus|Game.SetCinematicMode|DoorController.Close|SoundLibrary.PlayEffect|MusicPlayer.PlayMusic|EntityController.SetMoveSpeed" assets/resources/plains_woodkeep3.scene`.

## POD / PowerVR loading path clues

Native symbols include a substantial PowerVR SDK/POD loading implementation:

- `CPVRTModelPOD::ReadFromFile`
- `CPVRTModelPOD::SavePOD`
- `CPVRTModelPOD::CopyFromMemory`
- `PVRTModelPODToggleFixedPoint`
- `PVRTModelPODToggleInterleaved`
- `PVRTModelPODToggleStrips`
- `PVRTModelPODCopyMesh`
- `PVRTModelPODCopyMaterial`
- `PVRTModelPODCopyTexture`
- `PVRTTextureLoadFromPointer`
- `PVRTTextureLoadFromPVRResourceFile`
- `PVRTTextureLoadFromPVR`
- `CPVRTResourceFile`
- `CPVRTglesExt::LoadExtensions`

Engine-level model symbols include:

- `Caver::ModelLibrary::sharedLibrary`
- `Caver::ModelLibrary::Clear`
- `Caver::ModelComponent::LoadModel`
- `Caver::ModelInstance`
- `Caver::KeyframeAnimationComponent::LoadAnimation`

Evidence: `readelf -Ws reference/lib/armeabi-v7a/libswordigo.so | c++filt | rg 'POD|PVRT|Model|Mesh|Animation|PowerVR'`.
