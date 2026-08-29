# ============================================================================
# SwordigoPlatform.cmake — platform-agnostic build defaults + the platform
# branches that used to be hard-coded GNU/Linux flags in the top-level file.
# Every Linux-only compiler/linker flag is gated here so Windows/MSVC builds
# get sensible equivalents.
# ============================================================================

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

# --- Common include dirs (platform-independent) -----------------------------
set(COMMON_INCLUDES
    "${SRC_DIR}" "${SRC_DIR}/imgui" "${SRC_DIR}/sre/lua/src"
    "${SRC_DIR}/sre/toml-c" "${SRC_DIR}/sre/lua/luasocket/src"
    "${SRC_DIR}/sre/raknet" "${INCLUDE_DIR}" "${SWORDIGO_FFMPEG_ROOT}/include")
if (SWORDIGO_USE_DYNARMIC)
    list(APPEND COMMON_INCLUDES "${CMAKE_SOURCE_DIR}/deps/dynarmic/src")
endif()
if (WIN32)
    # Windows MSVC POSIX shims (dirent.h, posix.h). Linux untouched.
    list(APPEND COMMON_INCLUDES "${SRC_DIR}/wincompat")
endif()

# --- Common compile definitions --------------------------------------------
set(COMMON_DEFINITIONS VULKAN_BACKEND VK_NO_PROTOTYPES)
if (NOT WIN32)
    # GNU/Linux feature-test macros. Do not define on Windows/MSVC.
    list(APPEND COMMON_DEFINITIONS _GNU_SOURCE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
else()
    # MSVC needs this for <math.h> to expose M_PI etc.
    list(APPEND COMMON_DEFINITIONS _USE_MATH_DEFINES)
endif()
if (SWORDIGO_USE_DYNARMIC)
    list(APPEND COMMON_DEFINITIONS USE_DYNARMIC SWORDIGO_HAS_DYNARMIC)
endif()

# --- Runtime search paths ---------------------------------------------------
set(SWORDIGO_RPATH "$ORIGIN/libs;$ORIGIN;$ORIGIN/../libs;$ORIGIN/../share/swordigo-desktop/libs;/usr/share/swordigo-desktop/libs;/usr/share/swordigo-desktop")
set(FFMPEG_LIB_DIR "${SWORDIGO_FFMPEG_ROOT}/lib")

# --- libm ----------------------------------------------------------------
# MSVC has no separate math library; GNU/Clang link -lm explicitly.
if (WIN32)
    set(SWORDIGO_LIBM "")
else()
    set(SWORDIGO_LIBM m)
endif()

# --- Socket library -------------------------------------------------------
# Winsock 2 API lives in ws2_32.lib on Windows; empty elsewhere.
if (WIN32)
    set(SWORDIGO_SOCKLIB ws2_32)
else()
    set(SWORDIGO_SOCKLIB "")
endif()

# --- Compiler options -------------------------------------------------------
# GNU/Clang (Linux + MinGW) and MSVC use different optimization flags.
set(SWORDIGO_COMPILE_OPTS -g -O3 -fno-strict-aliasing)
set(SWORDIGO_COMPILE_OPTS_RELEASE "")
if (MSVC)
    set(SWORDIGO_COMPILE_OPTS /O2)
    set(SWORDIGO_COMPILE_OPTS_RELEASE /DNDEBUG)
endif()

# --- Optional symbol stripping (opt-in, for distribution) -------------------
# When SWORDIGO_STRIP_RELEASE is ON, strip our own exes/libs at link time.
# GNU/Clang/MinGW use the linker's -s flag; MSVC strips via /DEBUG:NONE. This
# only touches Swordigo's targets — third-party DLLs are stripped separately at
# packaging time. Default is OFF, so ordinary builds keep their debug symbols.
if (SWORDIGO_STRIP_RELEASE)
    if (MSVC)
        add_link_options(/DEBUG:NONE)
    else()
        add_link_options(-s)
    endif()
endif()
