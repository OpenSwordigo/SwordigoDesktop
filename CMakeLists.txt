cmake_minimum_required(VERSION 3.16)
project(SwordigoDesktop VERSION 4.5.0 LANGUAGES C CXX ASM)

# Build directly into the source tree's distribution folder — no separate
# `cmake --install` step needed. Executables land in <dist>/, shared libs in
# <dist>/libs/.
#
# The dist folder is `bin` for native (Linux) builds and `binw` for Windows
# builds (incl. MinGW cross-compiles) so Windows .exe/.dll artifacts never land
# in — or clobber — the Linux `bin/` tree. Override with -DSWORDIGO_DIST_DIR=...
if (NOT DEFINED SWORDIGO_DIST_DIR)
    if (WIN32)
        set(SWORDIGO_DIST_DIR "binw")
    else()
        set(SWORDIGO_DIST_DIR "bin")
    endif()
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}/libs")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}/libs")

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Modular build. Each component lives in cmake/components/*.cmake and defines
# its own targets; shared platform/option/helper logic is in cmake/*.cmake.
# ---------------------------------------------------------------------------

include(cmake/SwordigoOptions.cmake)     # options + cache paths
include(cmake/SwordigoPlatform.cmake)    # includes, defines, platform flags
include(cmake/SwordigoDeps.cmake)        # external dependencies
include(cmake/SwordigoHelpers.cmake)     # swordigo_target()/swordigo_library()

# Component order matters: libraries before the executables that consume them.
include(cmake/components/swcore.cmake)
include(cmake/components/swgui.cmake)
include(cmake/components/swfmt_swpod.cmake)
include(cmake/components/filerift.cmake)
include(cmake/components/swgfx.cmake)
include(cmake/components/swemu.cmake)
include(cmake/components/swordfare.cmake)
include(cmake/components/swordfare_boot.cmake)
include(cmake/components/ruby.cmake)
include(cmake/components/ruby_cli.cmake)
include(cmake/components/ruby_gg.cmake)

# Guest + third-party build steps and install rules.
include(cmake/components/sre.cmake)
include(cmake/components/misc.cmake)

if (BUILD_TESTING)
    add_executable(trampoline_mgr_test
        tests/trampoline_mgr_test.cpp
        ${SRC_DIR}/platform/arm64_reloc.cpp)
    target_include_directories(trampoline_mgr_test PRIVATE ${SRC_DIR})
    add_test(NAME trampoline_mgr_test COMMAND trampoline_mgr_test)

    add_executable(sre_hud_coin_regression_test
        tests/sre_hud_coin_regression_test.cpp)
    target_compile_definitions(sre_hud_coin_regression_test PRIVATE
        SRE_SOURCE_DIR="${SRC_DIR}/sre")
    add_test(NAME sre_hud_coin_regression_test COMMAND sre_hud_coin_regression_test)

    add_executable(filerift_smoke tests/filerift_smoke.cpp)
    target_include_directories(filerift_smoke PRIVATE ${SRC_DIR})
    target_link_libraries(filerift_smoke PRIVATE filerift swpod OpenGL::GL)
    add_test(NAME filerift_smoke COMMAND filerift_smoke)

    if (SWORDIGO_BUILD_RUBY)
        add_executable(styx_smoke tests/styx_smoke.cpp ${SRC_DIR}/tools/intellij.cpp)
        swordigo_target(styx_smoke)
        target_link_libraries(styx_smoke PRIVATE swgui)
        add_test(NAME styx_smoke COMMAND styx_smoke)
    endif()
endif()
