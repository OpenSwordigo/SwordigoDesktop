# ============================================================================
# toolchain-mingw64.cmake — MinGW-w64 cross toolchain for building Windows x64
# binaries on a Fedora Linux host. Chainloaded by vcpkg via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE.
# ============================================================================

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Search roots: the MinGW sysroot (for system libs/headers) and the vcpkg
# installed prefix for the x64-mingw-dynamic triplet.
set(CMAKE_FIND_ROOT_PATH
    /usr/x86_64-w64-mingw32
    /run/media/quantumcreeper/TVPG/vcpkg/installed/x64-mingw-dynamic)

# Find host programs on the host; find libraries/headers/packages in the
# target roots (and also allow the host for CONFIG packages vcpkg provides).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
