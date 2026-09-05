# ============================================================================
# SwordigoOptions.cmake — all user-facing build options and cache paths.
# ============================================================================

option(SWORDIGO_USE_DYNARMIC "Build and link the Dynarmic ARM64 JIT" ON)
option(SWORDIGO_USE_FFMPEG "Link static FFmpeg (MP4 background videos)" ON)
option(SWORDIGO_BUILD_SRE "Build the ARM64 guest libsre.so" ON)
option(SWORDIGO_BUILD_RUBY "Build the asset viewer" ON)
option(SWORDIGO_BUILD_RUBY_GG "Build ruby_gg (Xpera native editor frontend)" ON)
option(SWORDIGO_STATIC "Build components as static libraries (single-PE app; avoids cross-DLL globals)" OFF)
# Opt-in: strip symbols from our own executables/libraries at link time for a
# smaller distributable. OFF by default so normal builds keep debug symbols.
option(SWORDIGO_STRIP_RELEASE "Strip symbols from Swordigo binaries (smaller, for distribution)" OFF)
set(SWORDIGO_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/src/tools/ffmpeg/build" CACHE PATH "Local static FFmpeg prefix")
set(SWORDIGO_DYNARMIC_ROOT "${CMAKE_SOURCE_DIR}/deps/dynarmic/build" CACHE PATH "Dynarmic build directory")
