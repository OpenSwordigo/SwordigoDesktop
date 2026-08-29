# ============================================================================
# SwordigoHelpers.cmake — target factories shared by every component.
# ============================================================================

# Common properties for every target (includes, defines, compile opts, rpath).
function(swordigo_target target)
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDES} ${SDL3_IMAGE_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE ${COMMON_DEFINITIONS})
    if (MSVC)
        target_compile_options(${target} PRIVATE /W3 /permissive- ${SWORDIGO_COMPILE_OPTS})
    else()
        target_compile_options(${target} PRIVATE ${SWORDIGO_COMPILE_OPTS})
    endif()
    if (NOT WIN32)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "$ORIGIN/libs"
            INSTALL_RPATH "${SWORDIGO_RPATH}")
    endif()
    if (WIN32 AND SWORDIGO_GLEW_TARGET)
        target_link_libraries(${target} PRIVATE ${SWORDIGO_GLEW_TARGET})
    endif()
endfunction()

# Component library. Shared by default so components stay modular on Linux;
# with SWORDIGO_STATIC the whole app collapses into one PE per executable,
# which sidesteps cross-DLL globals and Windows DLL symbol pitfalls.
function(swordigo_library target)
    if (SWORDIGO_STATIC)
        add_library(${target} STATIC ${ARGN})
    else()
        add_library(${target} SHARED ${ARGN})
    endif()
    if (WIN32 AND NOT SWORDIGO_STATIC)
        # .dll next to the launcher; .dll.a import lib under <dist>/libs.
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}/libs")
    elseif (NOT WIN32)
        set_target_properties(${target} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}/libs"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${SWORDIGO_DIST_DIR}/libs")
    endif()
    swordigo_target(${target})
endfunction()
