cmake_minimum_required(VERSION 3.16)
project(SwordigoDesktop VERSION 4.5.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SWORDIGO_USE_DYNARMIC "Build and link the Dynarmic ARM64 JIT" ON)
option(SWORDIGO_BUILD_SRE "Build the ARM64 guest libsre.so" ON)
option(SWORDIGO_BUILD_RUBY "Build the asset viewer" ON)
option(SWORDIGO_BUILD_OPENSWORDIGO "Build the optional native OpenSwordigo runtime" OFF)
set(SWORDIGO_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/src/tools/ffmpeg/build" CACHE PATH "Local static FFmpeg prefix")
set(SWORDIGO_DYNARMIC_ROOT "${CMAKE_SOURCE_DIR}/deps/dynarmic/build" CACHE PATH "Dynarmic build directory")

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenAL REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan QUIET)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG QUIET)
if (TARGET SDL3_image::SDL3_image)
    set(SDL3_IMAGE_LINK SDL3_image::SDL3_image)
else()
    pkg_check_modules(SDL3_IMAGE REQUIRED SDL3_image)
    set(SDL3_IMAGE_LINK ${SDL3_IMAGE_LIBRARIES} ${SDL3_IMAGE_LDFLAGS})
endif()
pkg_check_modules(VORBISFILE REQUIRED vorbisfile)
pkg_check_modules(MPG123 REQUIRED libmpg123)
find_library(UNICORN_LIBRARY NAMES unicorn REQUIRED)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
set(COMMON_INCLUDES
    "${SRC_DIR}" "${SRC_DIR}/imgui" "${SRC_DIR}/sre/lua/src"
    "${SRC_DIR}/sre/toml-c" "${SRC_DIR}/sre/lua/luasocket/src"
    "${SRC_DIR}/sre/raknet" "${INCLUDE_DIR}" "${SWORDIGO_FFMPEG_ROOT}/include")
if (SWORDIGO_USE_DYNARMIC)
    list(APPEND COMMON_INCLUDES "${CMAKE_SOURCE_DIR}/deps/dynarmic/src")
endif()
set(COMMON_DEFINITIONS VULKAN_BACKEND VK_NO_PROTOTYPES _GNU_SOURCE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
if (SWORDIGO_USE_DYNARMIC)
    list(APPEND COMMON_DEFINITIONS USE_DYNARMIC SWORDIGO_HAS_DYNARMIC)
endif()
set(SWORDIGO_RPATH "$ORIGIN/libs;$ORIGIN;$ORIGIN/../libs;$ORIGIN/../share/swordigo-desktop/libs;/usr/share/swordigo-desktop/libs;/usr/share/swordigo-desktop")
set(FFMPEG_LIB_DIR "${SWORDIGO_FFMPEG_ROOT}/lib")

function(swordigo_target target)
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDES} ${SDL3_IMAGE_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE ${COMMON_DEFINITIONS})
    target_compile_options(${target} PRIVATE -g -O3 -fno-strict-aliasing)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}/bin/libs" INSTALL_RPATH "${SWORDIGO_RPATH}")
endfunction()
function(swordigo_library target)
    add_library(${target} SHARED ${ARGN})
    set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/libs")
    swordigo_target(${target})
endfunction()

swordigo_library(swcore ${SRC_DIR}/platform/data_path.cpp ${SRC_DIR}/platform/io_thread.cpp ${SRC_DIR}/platform/embedded_assets.cpp ${SRC_DIR}/android/log.c)
target_link_libraries(swcore PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
# embedded_assets.cpp is a ~60MB generated TU of const byte arrays — no -O3 needed
set_source_files_properties(${SRC_DIR}/platform/embedded_assets.cpp PROPERTIES COMPILE_OPTIONS "-O0")
swordigo_library(swgui
    ${SRC_DIR}/imgui/imgui.cpp ${SRC_DIR}/imgui/imgui_draw.cpp ${SRC_DIR}/imgui/imgui_tables.cpp ${SRC_DIR}/imgui/imgui_widgets.cpp
    ${SRC_DIR}/imgui/backends/imgui_impl_sdl3.cpp ${SRC_DIR}/imgui/backends/imgui_impl_opengl3.cpp ${SRC_DIR}/imgui/backends/imgui_impl_vulkan.cpp)
target_link_libraries(swgui PRIVATE swcore SDL3::SDL3 OpenGL::GL)
if (Vulkan_FOUND)
    target_link_libraries(swgui PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(swgui PRIVATE vulkan)
endif()
swordigo_library(swfmt ${SRC_DIR}/platform/pvr_loader.cpp ${SRC_DIR}/platform/pvrtc_decoder.cpp)
target_link_libraries(swfmt PRIVATE swcore ZLIB::ZLIB)
swordigo_library(swpod ${SRC_DIR}/tools/pod_loader.cpp ${SRC_DIR}/tools/scene_loader.cpp ${SRC_DIR}/tools/scene_schemas.cpp)
target_link_libraries(swpod PRIVATE swcore)

set(LUA_SRCS lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib ldblib liolib lmathlib loslib lstrlib ltablib loadlib linit)
set(HOST_LUA_SRCS)
foreach(name IN LISTS LUA_SRCS)
    list(APPEND HOST_LUA_SRCS ${SRC_DIR}/sre/lua/src/${name}.c)
endforeach()
# scene_schemas.cpp lives only in swpod: defining av::g_schemas twice caused
# two destructor registrations for one interposed object -> double free at exit.
swordigo_library(filerift ${SRC_DIR}/tools/filerift.cpp ${SRC_DIR}/tools/boulder.cpp ${HOST_LUA_SRCS})
target_compile_definitions(filerift PRIVATE "LUAI_FUNC=extern __attribute__((visibility(\"default\")))")
target_link_libraries(filerift PRIVATE swcore swfmt ZLIB::ZLIB m)
swordigo_library(swgfx ${SRC_DIR}/platform/fbo_scaler.cpp ${SRC_DIR}/platform/vulkan_backend.cpp ${SRC_DIR}/platform/video_background.cpp ${SRC_DIR}/platform/srt_overlay.cpp)
target_link_libraries(swgfx PRIVATE swcore swgui SDL3::SDL3 OpenGL::GL m)
swordigo_library(swemu
    ${SRC_DIR}/loader/elf_loader.cpp ${SRC_DIR}/loader/elf_loader_arm64.cpp ${SRC_DIR}/platform/emulator.cpp ${SRC_DIR}/platform/emulator_arm64.cpp
    ${SRC_DIR}/srehost/srehost_impl.cpp ${SRC_DIR}/jni/jni_bridge.cpp ${SRC_DIR}/jni/jni_bridge_arm64.cpp
    ${SRC_DIR}/jni/jni_marshaller.cpp ${SRC_DIR}/platform/rgc.cpp ${SRC_DIR}/android/asset_manager.c ${SRC_DIR}/android/asset_manager_arm32.c)
target_link_libraries(swemu PRIVATE swcore swgfx ${UNICORN_LIBRARY} Threads::Threads ${CMAKE_DL_LIBS} m)
if (SWORDIGO_USE_DYNARMIC)
    target_sources(swemu PRIVATE ${SRC_DIR}/platform/emulator_dynarmic64.cpp)
endif()
swordigo_library(swordfare ${SRC_DIR}/platform/swordfare_gui.cpp ${SRC_DIR}/platform/launcher_ui.cpp ${SRC_DIR}/platform/save_editor.cpp ${SRC_DIR}/platform/scl_parser.cpp ${SRC_DIR}/game/mod_tools.cpp ${SRC_DIR}/game/mod_config.cpp ${SRC_DIR}/game/save_editor_logic.cpp ${SRC_DIR}/game/camera_override.cpp)
target_link_libraries(swordfare PRIVATE swcore swgui filerift SDL3::SDL3 OpenGL::GL Threads::Threads)

add_executable(swordfare_boot ${SRC_DIR}/main.cpp ${SRC_DIR}/platform/display.cpp ${SRC_DIR}/platform/openswordigo_host.cpp ${SRC_DIR}/platform/gui.cpp ${SRC_DIR}/platform/input_config.cpp ${SRC_DIR}/platform/binary_selector.cpp)
set_target_properties(swordfare_boot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" OUTPUT_NAME swordfare)
swordigo_target(swordfare_boot)
target_link_libraries(swordfare_boot PRIVATE swcore swgui swfmt swpod filerift swgfx swemu swordfare
    -Wl,--whole-archive
    ${FFMPEG_LIB_DIR}/libavformat.a ${FFMPEG_LIB_DIR}/libavcodec.a ${FFMPEG_LIB_DIR}/libswscale.a ${FFMPEG_LIB_DIR}/libavutil.a
    -Wl,--no-whole-archive
    SDL3::SDL3 ${SDL3_IMAGE_LINK} ${VORBISFILE_LIBRARIES} ${MPG123_LIBRARIES} ZLIB::ZLIB OpenAL::OpenAL OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS} m)
if (SWORDIGO_USE_DYNARMIC)
    target_link_libraries(swordfare_boot PRIVATE
        ${SWORDIGO_DYNARMIC_ROOT}/src/dynarmic/libdynarmic.a ${SWORDIGO_DYNARMIC_ROOT}/externals/mcl/src/libmcl.a
        ${SWORDIGO_DYNARMIC_ROOT}/externals/fmt/libfmt.a ${SWORDIGO_DYNARMIC_ROOT}/externals/zydis/libZydis.a ${SWORDIGO_DYNARMIC_ROOT}/externals/zydis/zycore/libZycore.a)
endif()
target_link_options(swordfare_boot PRIVATE -rdynamic -Wl,--allow-shlib-undefined)

if (SWORDIGO_BUILD_RUBY)
    add_executable(ruby ${SRC_DIR}/tools/asset_viewer.cpp ${SRC_DIR}/tools/av_renderer.cpp ${SRC_DIR}/tools/av_audio.cpp ${SRC_DIR}/tools/scene_loader.cpp ${SRC_DIR}/tools/scene_workspace.cpp ${SRC_DIR}/tools/intellij.cpp ${SRC_DIR}/tools/batch_converter.cpp ${SRC_DIR}/tools/pod_writer.cpp ${SRC_DIR}/tools/gltf_export.cpp ${SRC_DIR}/tools/gltf_import.cpp ${SRC_DIR}/imgui/Guizmo/src/ImGuizmo.cpp)
    set_target_properties(ruby PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
    swordigo_target(ruby)
    target_link_libraries(ruby PRIVATE swcore swgui swfmt swpod filerift SDL3::SDL3 ${SDL3_IMAGE_LINK} OpenGL::GL ZLIB::ZLIB m util)
endif()

if (SWORDIGO_BUILD_OPENSWORDIGO)
    add_subdirectory(OpenSwordigo)
endif()

if (SWORDIGO_BUILD_SRE)
    find_program(AARCH64_CC aarch64-linux-gnu-gcc REQUIRED)
    set(SRE_CORE_SRCS sre_init sre_string sre_lua sre_background sre_effects sre_music sre_gui sre_gui_native sre_scene_update sre_frame_loop sre_gui_nav sre_mini_api sre_vfs sre_lua_libs sre_pack_lua sre_raknet_c sre_mod sre_config sre_caver sre_features sre_scene_shifter)
    set(SRE_SOCKET_SRCS auxiliar buffer except inet luasocket mime options select tcp timeout udp usocket)
    set(SRE_SRCS)
    foreach(name IN LISTS LUA_SRCS)
        list(APPEND SRE_SRCS ${SRC_DIR}/sre/lua/src/${name}.c)
    endforeach()
    foreach(name IN LISTS SRE_CORE_SRCS)
        list(APPEND SRE_SRCS ${SRC_DIR}/sre/${name}.c)
    endforeach()
    list(APPEND SRE_SRCS ${SRC_DIR}/sre/sre_setjmp.S ${SRC_DIR}/sre/toml-c/toml.c ${SRC_DIR}/sre/luafilesystem/src/lfs.c)
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

add_custom_target(dynarmic-build
    COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/deps/dynarmic -B ${SWORDIGO_DYNARMIC_ROOT} -DCMAKE_BUILD_TYPE=Release -DDYNARMIC_TESTS=OFF -DDYNARMIC_FRONTENDS=A64 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDYNARMIC_IGNORE_ASSERTS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF -Wno-dev
    COMMAND ${CMAKE_COMMAND} --build ${SWORDIGO_DYNARMIC_ROOT} --target dynarmic -j
    USES_TERMINAL)
add_custom_target(ffmpeg-build COMMAND ${CMAKE_COMMAND} -E chdir ${SRC_DIR}/tools/ffmpeg ./configure --prefix=build --enable-static --disable-shared --disable-all --enable-avformat --enable-avcodec --enable-swscale --enable-decoder=h264 --enable-demuxer=mov --enable-parser=h264 --enable-protocols --enable-protocol=file --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swresample --disable-libdrm --disable-vulkan --disable-hwaccels --disable-network --disable-iconv --disable-bzlib --disable-libxcb --disable-lzma --disable-sdl2 --disable-xlib --disable-zlib COMMAND ${CMAKE_COMMAND} -E chdir ${SRC_DIR}/tools/ffmpeg make -j COMMAND ${CMAKE_COMMAND} -E chdir ${SRC_DIR}/tools/ffmpeg make install USES_TERMINAL)
add_custom_target(install-sre DEPENDS sre COMMAND echo "[SRE] install-sre is deprecated")

install(TARGETS swordfare_boot DESTINATION bin)
if (SWORDIGO_BUILD_RUBY)
    install(TARGETS ruby DESTINATION bin)
endif()
install(TARGETS swcore swgui swfmt swpod filerift swgfx swemu swordfare LIBRARY DESTINATION bin/libs)
