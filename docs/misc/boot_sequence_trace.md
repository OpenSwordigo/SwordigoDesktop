# Boot Sequence - 2026-06-14 - Agent 1

## Android Java path for local reference

1. `MainActivity` static initializer loads `openal-soft`, then `swordigo`.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:273`.
2. `MainActivity.onCreate` sets `Native.mainActivity`, calls `Native.setFilesDir(getFilesDir())`, `Native.setCacheDir(getCacheDir())`, and `Native.setAssetManager(getAssets())`.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:31`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:42`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:46`.
3. `MainActivity.onCreate` creates `GameView`, sets EGL config, preserves context on pause, installs `GameRenderer`, and adds the view.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:49`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MainActivity.java:52`.
4. `GameRenderer.onSurfaceCreated` constructs `MusicPlayer`, calls `Native.setupNativeInterface()`, then `Native.setupApplication()`.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:30`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:32`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:34`.
5. `MusicPlayer` constructor configures Android `MediaPlayer` and calls native `initMusicPlayer()`.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/MusicPlayer.java:20`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MusicPlayer.java:22`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/MusicPlayer.java:27`.
6. `GameRenderer.onSurfaceChanged` calls `Native.setApplicationViewSize(width, height, isPad)`.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:46`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:52`.
7. `GameRenderer.onDrawFrame` calls `Native.updateApplication(0.016666668f)` in fixed steps, then `Native.drawApplication()`.
   Evidence: `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:58`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:69`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:76`.

## Vita loader path

1. Configure clocks and verify Vita prerequisites.
   Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1680`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1684`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1687`.
2. Load `ux0:data/swordigo/libswordigo.so` via `so_file_load`, relocate, resolve imports, patch, flush caches, run init array.
   Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1691`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1695`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1697`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1701`.
3. Build fake `JavaVM` and `JNIEnv` function tables.
   Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1703`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1709`.
4. Resolve JNI exports from `libswordigo.so` with `so_symbol`.
   Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1754`.
5. Initialize platform state: `setFilesDir`, `setCacheDir`, `setAssetManager`, `googleSignInCompleted`, `handleApplicationLaunch`, GL state, music, `setupNativeInterface`, `setupApplication`, `setApplicationViewSize`, `applicationDidBecomeActive`.
   Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1779`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1788`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1791`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1803`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1807`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1817`.
6. Enter main loop: translate Vita touch/controller input to `handleTouchEvent`, compute delta time, clamp large frame deltas, call `updateApplication(deltaSecond)`, `drawApplication()`, and swap buffers.
   Evidence: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1836`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1845`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1890`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1894`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1897`.

## Documented path from `libswordigo.so` to frame functions

`libswordigo.so`
-> exported JNI symbols found by `so_symbol` or Java native binding
-> `Java_com_touchfoo_swordigo_Native_setupNativeInterface`
-> `Java_com_touchfoo_swordigo_Native_setupApplication`
-> `Java_com_touchfoo_swordigo_Native_setApplicationViewSize`
-> per-frame `Java_com_touchfoo_swordigo_Native_updateApplication`
-> per-frame `Java_com_touchfoo_swordigo_Native_drawApplication`

Evidence for local exports: `readelf -Ws reference/lib/armeabi-v7a/libswordigo.so`.
Evidence for Java calls: `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:33`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:34`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:52`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:69`, `reference/decompiled/decompClasses/com.touchfoo.swordigo/GameRenderer.java:76`.
Evidence for Vita direct calls: `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1807`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1810`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1813`, `docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c:1894`.

## Version mismatch note

The Vita loader source calls `handleApplicationLaunch` and `googleSignInCompleted`, but the local `reference/lib/armeabi-v7a/libswordigo.so` does not export those symbols. Do not assume the Vita exact sequence can be invoked unchanged against the local library.
