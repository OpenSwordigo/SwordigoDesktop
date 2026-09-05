# ============================================================================
# Component: sre13 — ARM64 guest libsre13.so, cross-compiled with aarch64-linux-gnu.
# Produces bin/libs/libsre13.so directly for Swordigo 1.4.13.
# ============================================================================

if (SWORDIGO_BUILD_SRE)
    find_program(AARCH64_CC aarch64-linux-gnu-gcc)
    if (AARCH64_CC)
        set(SRE13_CORE_SRCS sre13_init sre13_lua sre13_safety sre13_scene sre13_audio sre13_ui)
        set(SRE13_SRCS)
        foreach(name IN LISTS LUA_SRCS)
            list(APPEND SRE13_SRCS ${SRC_DIR}/sre/lua/src/${name}.c)
        endforeach()
        foreach(name IN LISTS SRE13_CORE_SRCS)
            list(APPEND SRE13_SRCS ${SRC_DIR}/sre13/${name}.c)
        endforeach()
        list(APPEND SRE13_SRCS ${SRC_DIR}/sre/sre_setjmp.S)
        
        set(SRE13_OBJECTS)
        foreach(source IN LISTS SRE13_SRCS)
            file(RELATIVE_PATH relative "${SRC_DIR}" "${source}")
            string(REPLACE "/" "_" object_name "${relative}")
            string(REGEX REPLACE "\\.(c|S)$" ".o" object_name "${object_name}")
            set(object "${CMAKE_BINARY_DIR}/sre13/${object_name}")
            add_custom_command(OUTPUT "${object}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/sre13"
                COMMAND ${AARCH64_CC} -shared -fPIC -O2 -nostdlib -fno-builtin -fno-stack-protector -I${SRC_DIR}/sre/include -I${SRC_DIR}/sre13 -I${SRC_DIR}/sre/lua/src -c "${source}" -o "${object}"
                DEPENDS "${source}" VERBATIM)
            list(APPEND SRE13_OBJECTS "${object}")
        endforeach()
        add_custom_command(OUTPUT "${CMAKE_SOURCE_DIR}/bin/libs/libsre13.so"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/bin/libs"
            COMMAND ${AARCH64_CC} -shared -fPIC -nostdlib -o "${CMAKE_SOURCE_DIR}/bin/libs/libsre13.so" ${SRE13_OBJECTS}
            DEPENDS ${SRE13_OBJECTS} VERBATIM)
        add_custom_target(sre13 ALL DEPENDS "${CMAKE_SOURCE_DIR}/bin/libs/libsre13.so")
    endif()
endif()
