# ============================================================================
# SwordigoDeps.cmake — external dependency discovery. Required on all platforms
# unless noted. FFmpeg stays local/static; vorbisfile + mpg123 are pkg-config
# only on non-Windows (Windows builds link their own copies or skip audio).
# ============================================================================

include(GNUInstallDirs)
if (NOT WIN32)
    find_package(PkgConfig REQUIRED)
endif()
find_package(OpenGL REQUIRED)
if (WIN32)
    # GLEW supplies the modern (1.2+) OpenGL entry points that opengl32.lib does
    # not export; every host component links it via swordigo_target().
    find_package(glew CONFIG REQUIRED)
    set(SWORDIGO_GLEW_TARGET GLEW::GLEW)
endif()
find_package(ZLIB REQUIRED)
find_package(OpenAL REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan QUIET)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG QUIET)
if (TARGET SDL3_image::SDL3_image-shared)
    set(SDL3_IMAGE_LINK SDL3_image::SDL3_image-shared)
elseif (TARGET SDL3_image::SDL3_image-static)
    set(SDL3_IMAGE_LINK SDL3_image::SDL3_image-static)
elseif (TARGET SDL3_image::SDL3_image)
    set(SDL3_IMAGE_LINK SDL3_image::SDL3_image)
else()
    pkg_check_modules(SDL3_IMAGE REQUIRED SDL3_image)
    set(SDL3_IMAGE_LINK ${SDL3_IMAGE_LIBRARIES} ${SDL3_IMAGE_LDFLAGS})
endif()

if (NOT WIN32)
    pkg_check_modules(VORBISFILE REQUIRED vorbisfile)
    pkg_check_modules(MPG123 REQUIRED libmpg123)
else()
    # Windows: resolve audio backends through vcpkg's CONFIG packages so the
    # headers (vorbis/vorbisfile.h, mpg123.h) and import libs are wired in.
    find_package(Vorbis CONFIG REQUIRED)
    set(VORBISFILE_LIBRARIES Vorbis::vorbisfile Vorbis::vorbis)
    find_package(MPG123 CONFIG REQUIRED)
    set(MPG123_LIBRARIES MPG123::libmpg123)
endif()

# Unicorn is optional and runtime-loaded (src/platform/unicorn_dyn.cpp).
find_library(UNICORN_LIBRARY NAMES unicorn)
