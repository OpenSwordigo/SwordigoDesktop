# Questions - 2026-06-14

1. Which exact Android Swordigo version does `reference/lib/armeabi-v7a/libswordigo.so` come from? The Vita loader source expects `handleApplicationLaunch` and `googleSignInCompleted`, but the local library does not export those symbols.
2. Is `handleApplicationLaunch` required only for the Vita-targeted game version, or is it optional for the local Android library because Java never calls it?
3. What ARM disassembly tool should be added to the research environment so JNI wrappers can be traced from `Java_com_touchfoo_swordigo_Native_setupApplication` to internal `Caver::*` calls?
4. Which JNI method names are actually requested during `setupNativeInterface()` and `setupApplication()` on the local library? Vita's static method table is evidence for one version, but runtime logging under emulation would confirm the local one.
5. Does reaching engine initialization on Linux require ARM emulation/translation from process start, or can a host launcher use a user-mode ARM emulator around only `libswordigo.so` calls?
6. What exact OpenGL ES profile is required at initialization: GLES 1.1 fixed-function only, or GLES 2.0 paths via `eglGetProcAddress` for shader-related code?
7. Are `.gdata`, `.gstate`, and `.scene` all protobuf-lite messages generated into `libswordigo.so`, and can field names be recovered from symbol strings or generated parser symbols?
8. Which protobuf field actually contains Lua/program script source and bytecode in `.scene` files? 
    - *Update (Agent 2)*: In `Program` components, field `157.2.1` is Lua source and `157.2.2` is bytecode. In trigger-like components, field `121.9.1` is source and `121.9.2` is bytecode. Evidence: `protoc --decode_raw < "assets/resources/plains_woodkeep3.scene"`.
9. Are `.scl` files protobuf template/class collections? 
    - *Update (Agent 2)*: Yes. Field `1` is the collection name, field `2` is a repeated entity class definition. Evidence: `protoc --decode_raw < "assets/resources/monsters.scl"`.
10. What are the complete Lua/Program library bindings exposed by native code, e.g. `Program.Wait`, `EntityController.SetMoveSpeed`, and `EntityController.PerformAction`?
    - *Update (Agent 2)*: Observed additional bindings: `Scene.CreateObject`, `Scene.Find`, `Math.RandomInt`, `CollisionShape.SetEnabled`, `Game.SetCinematicMode`, `DoorController.Close`, `SoundLibrary.PlayEffect`, `Camera.FocusAtPoint`, `MusicPlayer.PlayMusic`.
