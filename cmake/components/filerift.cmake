# ============================================================================
# Component: filerift — file format library + host Lua (Lua 5.x core).
# ============================================================================

set(LUA_SRCS lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib ldblib liolib lmathlib loslib lstrlib ltablib loadlib linit)
set(HOST_LUA_SRCS)
foreach(name IN LISTS LUA_SRCS)
    list(APPEND HOST_LUA_SRCS ${SRC_DIR}/sre/lua/src/${name}.c)
endforeach()

swordigo_library(filerift
    ${SRC_DIR}/tools/filerift.cpp
    ${SRC_DIR}/tools/boulder.cpp
    ${SRC_DIR}/tools/scene_creator.cpp
    ${HOST_LUA_SRCS})
if (MSVC)
    target_compile_definitions(filerift PRIVATE "LUAI_FUNC=extern __declspec(dllexport)")
else()
    target_compile_definitions(filerift PRIVATE "LUAI_FUNC=extern __attribute__((visibility(\"default\")))")
endif()
target_link_libraries(filerift PRIVATE swcore swfmt swpod ZLIB::ZLIB ${SWORDIGO_LIBM})
