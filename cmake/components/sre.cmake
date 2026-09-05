# ============================================================================
# Component: sre — ARM64 guest libsre.so, cross-compiled with aarch64-linux-gnu.
# Produces bin/libs/libsre.so directly (the launcher loads it as guest code).
# ============================================================================

if (SWORDIGO_BUILD_SRE)
    find_program(AARCH64_CC aarch64-linux-gnu-gcc)
    if (NOT AARCH64_CC)
        message(WARNING "aarch64-linux-gnu-gcc not found — libsre.so will not be built. "
                        "Install the cross toolchain or disable SWORDIGO_BUILD_SRE.")
    else()
        # NOTE: sre_ffi removed from core — FFI is now a closed-source feature
        # living in libsre-extras.so (src/sre-extras-closed-source/sre_ffi.c).
        # Core only registers the safe stub _G.ffi surface via sre_extras_stubs.
        set(SRE_CORE_SRCS sre_init sre_string sre_lua sre_background sre_effects sre_music sre_gui sre_gui_native sre_scene_update sre_frame_loop sre_gui_nav sre_mini_api sre_vfs sre_lua_libs sre_pack_lua sre_raknet_c sre_mod sre_config sre_caver sre_features sre_scene_shifter sre_profile_panels sre_extras_stubs)
        set(SRE_SOCKET_SRCS auxiliar buffer except inet luasocket mime options select tcp timeout udp usocket)
        set(SRE_SRCS)
        foreach(name IN LISTS LUA_SRCS)
            list(APPEND SRE_SRCS ${SRC_DIR}/sre/lua/src/${name}.c)
        endforeach()
        foreach(name IN LISTS SRE_CORE_SRCS)
            list(APPEND SRE_SRCS ${SRC_DIR}/sre/${name}.c)
        endforeach()
        list(APPEND SRE_SRCS ${SRC_DIR}/sre/sre_setjmp.S ${SRC_DIR}/sre/sre_scene_loading.S ${SRC_DIR}/sre/toml-c/toml.c ${SRC_DIR}/sre/luafilesystem/src/lfs.c)
        foreach(name IN LISTS SRE_SOCKET_SRCS)
            list(APPEND SRE_SRCS ${SRC_DIR}/sre/luasocket/src/${name}.c)
        endforeach()
        set(SRE_OBJECTS)
        foreach(source IN LISTS SRE_SRCS)
            file(RELATIVE_PATH relative "${SRC_DIR}/sre" "${source}")
            string(REPLACE "/" "_" object_name "${relative}")
            string(REGEX REPLACE "\\.(c|S)$" ".o" object_name "${object_name}")
            set(object "${CMAKE_BINARY_DIR}/sre/${object_name}")
            set(extra_flags)
            if (NOT relative MATCHES "^lua/src/")
                list(APPEND extra_flags -I${SRC_DIR}/sre/toml-c -I${SRC_DIR}/sre/luasocket/src -I${SRC_DIR}/sre/raknet -include ${SRC_DIR}/sre/sre_lua_compat.h)
            endif()
            add_custom_command(OUTPUT "${object}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/sre"
                COMMAND ${AARCH64_CC} -shared -fPIC -O2 -nostdlib -fno-builtin -fno-stack-protector -I${SRC_DIR}/sre/include -I${SRC_DIR}/sre/lua/src ${extra_flags} -c "${source}" -o "${object}"
                DEPENDS "${source}" ${SRC_DIR}/sre/sre_lua_compat.h VERBATIM)
            list(APPEND SRE_OBJECTS "${object}")
        endforeach()
        add_custom_command(OUTPUT "${CMAKE_SOURCE_DIR}/bin/libs/libsre.so"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/bin/libs"
            COMMAND ${AARCH64_CC} -shared -fPIC -nostdlib -o "${CMAKE_SOURCE_DIR}/bin/libs/libsre.so" ${SRE_OBJECTS}
            DEPENDS ${SRE_OBJECTS} VERBATIM)
        add_custom_target(sre ALL DEPENDS "${CMAKE_SOURCE_DIR}/bin/libs/libsre.so")
    endif()
endif()
