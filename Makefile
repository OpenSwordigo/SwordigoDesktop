## Swordigo Desktop — Modular Makefile
##
## Output layout:
##   bin/swordfare             ← Main desktop engine (renamed from swordigo_boot)
##   bin/ruby                  ← Asset browser / previewer
##   bin/libs/libswcore.so     ← Platform utilities
##   bin/libs/libswgui.so      ← Dear ImGui + SDL3/GL/Vulkan backends
##   bin/libs/libswfmt.so      ← PVR texture + PVRTC decoder
##   bin/libs/libfilerift.so   ← Filerift engine + Host Lua 5.1
##   bin/libs/libswgfx.so      ← FBO scaler, Vulkan backend, video, SRT overlay
##   bin/libs/libswemu.so      ← ARM32/ARM64 ELF loader, emulators, JNI bridge
##   bin/libs/libswordfare.so  ← Swordfare overlay, launcher UI, save editor, mods
##   libsre.so                 ← Guest ARM64 SRE (root, loaded by ELF loader)
##
## Usage:
##   make -j$(nproc)           # Full build
##   make libsre.so            # Guest SRE only
##   make bin/ruby             # Asset viewer only
##   make install-sre          # Deploy libsre.so to engine cache
##   make clean                # Remove build/ and bin/

# ============================================================================
# Toolchains
# ============================================================================
CXX        := g++
CC         := gcc
AARCH64_CC  := aarch64-linux-gnu-gcc
AARCH64_CXX := $(shell command -v aarch64-linux-gnu-g++ 2>/dev/null || echo "aarch64-linux-gnu-gcc -x c++")

# ============================================================================
# Directories
# ============================================================================
BIN_DIR := bin
LIB_DIR := bin/libs
BUILD   := build

# ============================================================================
# Common compiler flags
# ============================================================================
COMMON_CXXFLAGS := -std=c++17 -g -O3 \
    -Isrc -Isrc/imgui -Isrc/sre/lua/src -Iinclude -Isrc/stb \
    -DVULKAN_BACKEND -DVK_NO_PROTOTYPES \
    -MMD -MP

COMMON_CFLAGS := -g -O3 \
    -Isrc -Isrc/sre/lua/src -Iinclude \
    -MMD -MP

# ============================================================================
# pkg-config queries
# ============================================================================
SDL3_CFLAGS  := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS    := $(shell pkg-config --libs   sdl3 2>/dev/null)
SDL3I_CFLAGS := $(shell pkg-config --cflags SDL3_image 2>/dev/null || \
                        pkg-config --cflags sdl3-image 2>/dev/null)
SDL3I_LIBS   := $(shell pkg-config --libs   SDL3_image 2>/dev/null || \
                        pkg-config --libs   sdl3-image 2>/dev/null || echo "-lSDL3_image")
VORB_CFLAGS  := $(shell pkg-config --cflags vorbisfile  2>/dev/null)
VORB_LIBS    := $(shell pkg-config --libs   vorbisfile  2>/dev/null)
MP3_CFLAGS   := $(shell pkg-config --cflags libmpg123   2>/dev/null)
MP3_LIBS     := $(shell pkg-config --libs   libmpg123   2>/dev/null)
ZLIB_LIBS    := $(shell pkg-config --libs   zlib 2>/dev/null || echo "-lz")
OAL_LIBS     := $(shell pkg-config --libs   openal 2>/dev/null || echo "-lopenal")

ALL_CXXFLAGS := $(COMMON_CXXFLAGS) $(SDL3_CFLAGS) $(SDL3I_CFLAGS) $(VORB_CFLAGS) $(MP3_CFLAGS)
ALL_CFLAGS   := $(COMMON_CFLAGS)   $(SDL3_CFLAGS)

# -fPIC for shared objects (host side)
SO_CXXFLAGS := $(ALL_CXXFLAGS) -fPIC
SO_CFLAGS   := $(ALL_CFLAGS)   -fPIC

# RPATH baked in: libs resolve each other from bin/libs/ and system install paths at runtime
RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN/libs:$$ORIGIN:$$ORIGIN/../libs:$$ORIGIN/../share/swordigo-desktop/libs:/usr/share/swordigo-desktop/libs:/usr/share/swordigo-desktop'

# ============================================================================
# Dynarmic JIT (static archives)
# ============================================================================
DYNARMIC_DIR   := deps/dynarmic
DYNARMIC_BUILD := $(DYNARMIC_DIR)/build

ALL_CXXFLAGS += -DUSE_DYNARMIC -I$(DYNARMIC_DIR)/src
SO_CXXFLAGS  += -DUSE_DYNARMIC -I$(DYNARMIC_DIR)/src

DYNARMIC_STATIC_LIBS := \
    -L$(DYNARMIC_BUILD)/src/dynarmic       -ldynarmic \
    -L$(DYNARMIC_BUILD)/externals/mcl/src  -lmcl \
    -L$(DYNARMIC_BUILD)/externals/fmt      -lfmt \
    -L$(DYNARMIC_BUILD)/externals/zydis          -lZydis \
    -L$(DYNARMIC_BUILD)/externals/zydis/zycore   -lZycore

# ============================================================================
# Local Static FFmpeg
# ============================================================================
FFMPEG_DIR   := src/tools/ffmpeg
FFMPEG_BUILD := $(FFMPEG_DIR)/build

ifneq ($(MAKECMDGOALS),ffmpeg-build)
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(FFMPEG_BUILD)/lib/libavcodec.a),)
$(error Local static FFmpeg not found in $(FFMPEG_BUILD)/lib/. Run 'make ffmpeg-build' first.)
endif
endif
endif

ALL_CXXFLAGS += -I$(FFMPEG_BUILD)/include
SO_CXXFLAGS  += -I$(FFMPEG_BUILD)/include

FFMPEG_STATIC_LIBS := \
    -L$(FFMPEG_BUILD)/lib -lavformat -lavcodec -lswscale -lavutil

# ============================================================================
# ── libswcore.so ── Platform utilities
#    data_path, io_thread, android/log
#    Dependency of every other .so
# ============================================================================
CORE_CXX_SRCS := \
    src/platform/data_path.cpp \
    src/platform/io_thread.cpp

CORE_C_SRCS := \
    src/android/log.c

CORE_CXX_OBJS := $(patsubst src/%.cpp, $(BUILD)/core/%.o, $(CORE_CXX_SRCS))
CORE_C_OBJS   := $(patsubst src/%.c,   $(BUILD)/core/%.o, $(CORE_C_SRCS))
CORE_OBJS     := $(CORE_CXX_OBJS) $(CORE_C_OBJS)
CORE_DEPS     := $(CORE_OBJS:.o=.d)

$(BUILD)/core/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/core]  $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(BUILD)/core/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/core]   $<"
	@$(CC) $(SO_CFLAGS) -c $< -o $@

$(LIB_DIR)/libswcore.so: $(CORE_OBJS)
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) -o $@ $^ -lpthread -ldl

# ============================================================================
# ── libswgui.so ── Dear ImGui + SDL3/GL/Vulkan backends
#    Depends on: libswcore
# ============================================================================
GUI_SRCS := \
    src/imgui/imgui.cpp \
    src/imgui/imgui_draw.cpp \
    src/imgui/imgui_tables.cpp \
    src/imgui/imgui_widgets.cpp \
    src/imgui/backends/imgui_impl_sdl3.cpp \
    src/imgui/backends/imgui_impl_opengl3.cpp \
    src/imgui/backends/imgui_impl_vulkan.cpp

GUI_OBJS := $(patsubst src/%.cpp, $(BUILD)/gui/%.o, $(GUI_SRCS))
GUI_DEPS := $(GUI_OBJS:.o=.d)

$(BUILD)/gui/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/gui]   $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(LIB_DIR)/libswgui.so: $(GUI_OBJS) | $(LIB_DIR)/libswcore.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) \
	    -Wl,--allow-shlib-undefined \
	    -o $@ $^ \
	    -L$(LIB_DIR) -lswcore \
	    $(SDL3_LIBS) -lGL -lvulkan

# ============================================================================
# ── libswfmt.so ── PVR texture loader + PVRTC decoder
#    Depends on: libswcore
# ============================================================================
FORMATS_SRCS := \
    src/platform/pvr_loader.cpp \
    src/platform/pvrtc_decoder.cpp

FORMATS_OBJS := $(patsubst src/%.cpp, $(BUILD)/formats/%.o, $(FORMATS_SRCS))
FORMATS_DEPS := $(FORMATS_OBJS:.o=.d)

$(BUILD)/formats/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/fmt]   $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(LIB_DIR)/libswfmt.so: $(FORMATS_OBJS) | $(LIB_DIR)/libswcore.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) -o $@ $^ \
	    -L$(LIB_DIR) -lswcore \
	    $(ZLIB_LIBS)

POD_SRCS := src/tools/pod_loader.cpp src/tools/scene_loader.cpp src/tools/scene_schemas.cpp
POD_OBJS := $(patsubst src/%.cpp, $(BUILD)/pod/%.o, $(POD_SRCS))
POD_DEPS := $(POD_OBJS:.o=.d)

$(BUILD)/pod/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/pod]   $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(LIB_DIR)/libswpod.so: $(POD_OBJS) | $(LIB_DIR)/libswcore.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) -o $@ $^ -L$(LIB_DIR) -lswcore

# ============================================================================
# ── libfilerift.so ── Filerift engine + Host Lua 5.1
#    filerift.cpp, scene_schemas.cpp, boulder.cpp + 28 Lua source files.
#    Depends on: libswcore, libswfmt
# ============================================================================
FILERIFT_CXX_SRCS := \
    src/tools/filerift.cpp \
    src/tools/scene_schemas.cpp \
    src/tools/boulder.cpp

HOST_LUA_C_SRCS := \
    src/sre/lua/src/lapi.c     src/sre/lua/src/lcode.c    src/sre/lua/src/ldebug.c \
    src/sre/lua/src/ldo.c      src/sre/lua/src/ldump.c    src/sre/lua/src/lfunc.c \
    src/sre/lua/src/lgc.c      src/sre/lua/src/llex.c     src/sre/lua/src/lmem.c \
    src/sre/lua/src/lobject.c  src/sre/lua/src/lopcodes.c src/sre/lua/src/lparser.c \
    src/sre/lua/src/lstate.c   src/sre/lua/src/lstring.c  src/sre/lua/src/ltable.c \
    src/sre/lua/src/ltm.c      src/sre/lua/src/lundump.c  src/sre/lua/src/lvm.c \
    src/sre/lua/src/lzio.c     src/sre/lua/src/lauxlib.c  src/sre/lua/src/lbaselib.c \
    src/sre/lua/src/ldblib.c   src/sre/lua/src/liolib.c   src/sre/lua/src/lmathlib.c \
    src/sre/lua/src/loslib.c   src/sre/lua/src/lstrlib.c  src/sre/lua/src/ltablib.c \
    src/sre/lua/src/loadlib.c  src/sre/lua/src/linit.c

FILERIFT_CXX_OBJS := $(patsubst src/%.cpp,          $(BUILD)/filerift/%.o,    $(FILERIFT_CXX_SRCS))
FILERIFT_LUA_OBJS := $(patsubst src/sre/lua/src/%.c,$(BUILD)/filerift/lua/%.o,$(HOST_LUA_C_SRCS))
FILERIFT_OBJS     := $(FILERIFT_CXX_OBJS) $(FILERIFT_LUA_OBJS)
FILERIFT_DEPS     := $(FILERIFT_OBJS:.o=.d)

$(BUILD)/filerift/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/frf]   $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(BUILD)/filerift/lua/%.o: src/sre/lua/src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/lua]    $<"
	@$(CC) $(SO_CFLAGS) -DLUAI_FUNC='extern __attribute__((visibility("default")))' -c $< -o $@

$(LIB_DIR)/libfilerift.so: $(FILERIFT_OBJS) | \
    $(LIB_DIR)/libswcore.so $(LIB_DIR)/libswfmt.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) -o $@ $^ \
	    -L$(LIB_DIR) -lswcore -lswfmt \
	    $(ZLIB_LIBS) -lm

# ============================================================================
# ── libswgfx.so ── Graphics & post-processing pipeline
#    FBO scaler, Vulkan backend, video background, SRT overlay.
#    Depends on: libswcore, libswgui
# ============================================================================
GFX_SRCS := \
    src/platform/fbo_scaler.cpp \
    src/platform/vulkan_backend.cpp \
    src/platform/video_background.cpp \
    src/platform/srt_overlay.cpp

GFX_OBJS := $(patsubst src/%.cpp, $(BUILD)/gfx/%.o, $(GFX_SRCS))
GFX_DEPS := $(GFX_OBJS:.o=.d)

$(BUILD)/gfx/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/gfx]   $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(LIB_DIR)/libswgfx.so: $(GFX_OBJS) | \
    $(LIB_DIR)/libswcore.so $(LIB_DIR)/libswgui.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) \
	    -Wl,--allow-shlib-undefined \
	    -o $@ $^ \
	    -L$(LIB_DIR) -lswcore -lswgui \
	    $(SDL3_LIBS) -lGL \
	    -lm

# ============================================================================
# ── libswemu.so ── ARM32/ARM64 Emulation core
#    ELF loaders, Unicorn + Dynarmic emulators, JNI bridge, asset_manager.
#    Depends on: libswcore, libswgfx
# ============================================================================
EMU_CXX_SRCS := \
    src/loader/elf_loader.cpp \
    src/loader/elf_loader_arm64.cpp \
    src/platform/emulator.cpp \
    src/platform/emulator_arm64.cpp \
    src/platform/emulator_dynarmic64.cpp \
    src/srehost/srehost_impl.cpp \
    src/jni/jni_bridge.cpp \
    src/jni/jni_bridge_arm64.cpp \
    src/jni/jni_marshaller.cpp \
    src/platform/rgc.cpp

EMU_C_SRCS := \
    src/android/asset_manager.c \
    src/android/asset_manager_arm32.c

EMU_CXX_OBJS := $(patsubst src/%.cpp, $(BUILD)/emu/%.o, $(EMU_CXX_SRCS))
EMU_C_OBJS   := $(patsubst src/%.c,   $(BUILD)/emu/%.o, $(EMU_C_SRCS))
EMU_OBJS     := $(EMU_CXX_OBJS) $(EMU_C_OBJS)
EMU_DEPS     := $(EMU_OBJS:.o=.d)

$(BUILD)/emu/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/emu]   $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(BUILD)/emu/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/emu]    $<"
	@$(CC) $(SO_CFLAGS) -c $< -o $@

$(LIB_DIR)/libswemu.so: $(EMU_OBJS) | \
    $(LIB_DIR)/libswcore.so $(LIB_DIR)/libswgfx.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) \
	    -Wl,--allow-shlib-undefined \
	    -o $@ $^ \
	    -L$(LIB_DIR) -lswcore -lswgfx \
	    -lunicorn -lpthread -ldl -lm

# ============================================================================
# ── libswordfare.so ── Swordfare overlay, launcher UI, save editor, mods
#    Depends on: libswcore, libswgui, libfilerift
# ============================================================================
UI_SRCS := \
    src/platform/swordfare_gui.cpp \
    src/platform/launcher_ui.cpp \
    src/platform/save_editor.cpp \
    src/platform/scl_parser.cpp \
    src/game/mod_tools.cpp \
    src/game/mod_config.cpp \
    src/game/save_editor_logic.cpp \
    src/game/camera_override.cpp

UI_OBJS := $(patsubst src/%.cpp, $(BUILD)/ui/%.o, $(UI_SRCS))
UI_DEPS := $(UI_OBJS:.o=.d)

$(BUILD)/ui/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/ui]    $<"
	@$(CXX) $(SO_CXXFLAGS) -c $< -o $@

$(LIB_DIR)/libswordfare.so: $(UI_OBJS) | \
    $(LIB_DIR)/libswcore.so $(LIB_DIR)/libswgui.so $(LIB_DIR)/libfilerift.so
	@mkdir -p $(LIB_DIR)
	@echo "[LINK/so]   $@"
	@$(CXX) -shared -fPIC $(RPATH_FLAGS) \
	    -Wl,--allow-shlib-undefined \
	    -o $@ $^ \
	    -L$(LIB_DIR) -lswcore -lswgui -lfilerift \
	    $(SDL3_LIBS) -lGL -lpthread

# ============================================================================
# ── All 7 host-side shared libraries ──
# ============================================================================
ALL_HOST_SO := \
    $(LIB_DIR)/libswcore.so \
    $(LIB_DIR)/libswgui.so \
    $(LIB_DIR)/libswfmt.so \
    $(LIB_DIR)/libswpod.so \
    $(LIB_DIR)/libfilerift.so \
    $(LIB_DIR)/libswgfx.so \
    $(LIB_DIR)/libswemu.so \
    $(LIB_DIR)/libswordfare.so

# Optional native OpenSwordigo bridge. The engine is built independently in
# OpenSwordigo and loaded at runtime by --openswordigo; it is not linked into
# the normal Swordfare binary.
OPENSWORDIGO_LIB := OpenSwordigo/build/bin/libopenswordigo.so

# ============================================================================
# ── bin/swordfare ── Main desktop engine executable
#    (renamed from swordigo_boot)
# ============================================================================
BOOT_SRCS := \
    src/main.cpp \
    src/platform/display.cpp \
    src/platform/openswordigo_host.cpp \
    src/platform/gui.cpp \
    src/platform/input_config.cpp \
    src/platform/binary_selector.cpp

BOOT_OBJS := $(patsubst src/%.cpp, $(BUILD)/main/%.o, $(BOOT_SRCS))
BOOT_DEPS := $(BOOT_OBJS:.o=.d)

$(BUILD)/main/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/boot]  $<"
	@$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

$(BIN_DIR)/swordfare: $(BOOT_OBJS) | $(ALL_HOST_SO)
	@mkdir -p $(BIN_DIR)
	@echo "[LINK] $@"
	@$(CXX) -rdynamic $(RPATH_FLAGS) -o $@ $^ \
	    -L$(LIB_DIR) \
	    -lswcore -lswgui -lswfmt -lswpod -lfilerift \
	    -lswgfx -lswemu -lswordfare \
	    -Wl,--whole-archive \
	    $(DYNARMIC_STATIC_LIBS) \
	    $(FFMPEG_STATIC_LIBS) \
	    -Wl,--no-whole-archive \
	    $(SDL3_LIBS) $(SDL3I_LIBS) $(VORB_LIBS) $(MP3_LIBS) \
	    $(ZLIB_LIBS) $(OAL_LIBS) \
	    -lGL -lpthread -lm -ldl
	@echo "[OK]   bin/swordfare  —  run with: ./run_swordigo.sh"

openswordigo:
	@$(MAKE) $(LIB_DIR)/libswpod.so
	@cmake -S OpenSwordigo -B OpenSwordigo/build -DCMAKE_BUILD_TYPE=Release
	@cmake --build OpenSwordigo/build -j$(nproc) --target openswordigo_swordfare
	@mkdir -p $(LIB_DIR)
	@cp $(OPENSWORDIGO_LIB) $(LIB_DIR)/libopenswordigo.so
	@echo "[OK]   $(LIB_DIR)/libopenswordigo.so"

# ============================================================================
# ── bin/ruby ── Standalone asset browser / previewer
#    Links against core + gui + fmt + filerift only.
# ============================================================================
RUBY_SRCS := \
    src/tools/asset_viewer.cpp \
    src/tools/av_renderer.cpp \
    src/tools/av_audio.cpp \
    src/tools/scene_loader.cpp \
    src/tools/intellij.cpp \
    src/tools/batch_converter.cpp

RUBY_OBJS := $(patsubst src/%.cpp, $(BUILD)/ruby/%.o, $(RUBY_SRCS))
RUBY_DEPS := $(RUBY_OBJS:.o=.d)

$(BUILD)/ruby/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX/ruby]  $<"
	@$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

$(BIN_DIR)/ruby: $(RUBY_OBJS) | \
    $(LIB_DIR)/libswcore.so \
    $(LIB_DIR)/libswgui.so \
    $(LIB_DIR)/libswfmt.so \
    $(LIB_DIR)/libswpod.so \
    $(LIB_DIR)/libfilerift.so
	@mkdir -p $(BIN_DIR)
	@echo "[LINK] $@"
	@$(CXX) $(RPATH_FLAGS) -o $@ $^ \
	    -L$(LIB_DIR) \
	    -lswcore -lswgui -lswfmt -lswpod -lfilerift \
	    $(SDL3_LIBS) $(SDL3I_LIBS) $(ZLIB_LIBS) \
	    -lGL -lutil -lm
	@echo "[AV]   bin/ruby  —  run with: ./bin/ruby"

# ============================================================================
# ── libsre.so ── Guest ARM64 Swordigo Runtime Engine (cross-compiled AArch64)
# ============================================================================
SRE_LUA_SRCS := \
    src/sre/lua/src/lapi.c     src/sre/lua/src/lcode.c    src/sre/lua/src/ldebug.c \
    src/sre/lua/src/ldo.c      src/sre/lua/src/ldump.c    src/sre/lua/src/lfunc.c \
    src/sre/lua/src/lgc.c      src/sre/lua/src/llex.c     src/sre/lua/src/lmem.c \
    src/sre/lua/src/lobject.c  src/sre/lua/src/lopcodes.c src/sre/lua/src/lparser.c \
    src/sre/lua/src/lstate.c   src/sre/lua/src/lstring.c  src/sre/lua/src/ltable.c \
    src/sre/lua/src/ltm.c      src/sre/lua/src/lundump.c  src/sre/lua/src/lvm.c \
    src/sre/lua/src/lzio.c     src/sre/lua/src/lauxlib.c  src/sre/lua/src/lbaselib.c \
    src/sre/lua/src/ldblib.c   src/sre/lua/src/liolib.c   src/sre/lua/src/lmathlib.c \
    src/sre/lua/src/loslib.c   src/sre/lua/src/lstrlib.c  src/sre/lua/src/ltablib.c \
    src/sre/lua/src/loadlib.c  src/sre/lua/src/linit.c

SRE_CORE_SRCS := \
    src/sre/sre_init.c          src/sre/sre_string.c       src/sre/sre_lua.c \
    src/sre/sre_background.c    src/sre/sre_effects.c      src/sre/sre_music.c \
    src/sre/sre_gui.c           src/sre/sre_gui_native.c   src/sre/sre_scene_update.c \
    src/sre/sre_frame_loop.c    src/sre/sre_gui_nav.c      src/sre/sre_setjmp.S \
    src/sre/sre_mini_api.c      src/sre/sre_vfs.c          src/sre/sre_lua_libs.c \
    src/sre/sre_pack_lua.c      src/sre/sre_raknet_c.c     src/sre/sre_mod.c          src/sre/sre_config.c \
    src/sre/sre_caver.c         src/sre/sre_features.c      src/sre/sre_scene_shifter.c \
    src/sre/toml-c/toml.c \
    src/sre/luafilesystem/src/lfs.c \
    src/sre/luasocket/src/auxiliar.c  src/sre/luasocket/src/buffer.c \
    src/sre/luasocket/src/except.c    src/sre/luasocket/src/inet.c \
    src/sre/luasocket/src/io.c        src/sre/luasocket/src/luasocket.c \
    src/sre/luasocket/src/mime.c      src/sre/luasocket/src/options.c \
    src/sre/luasocket/src/select.c    src/sre/luasocket/src/tcp.c \
    src/sre/luasocket/src/timeout.c   src/sre/luasocket/src/udp.c \
    src/sre/luasocket/src/usocket.c

SRE_LUA_CFLAGS := -shared -fPIC -O2 -nostdlib -fno-builtin -fno-stack-protector \
    -Isrc/sre/include -Isrc/sre/lua/src

SRE_SRE_CFLAGS := $(SRE_LUA_CFLAGS) \
    -Isrc/sre/toml-c \
    -Isrc/sre/lua/luasocket/src \
    -Isrc/sre/raknet \
    -include src/sre/sre_lua_compat.h

SRE_LUA_OBJS  := $(patsubst src/sre/%.c, $(BUILD)/sre/%.o, $(SRE_LUA_SRCS))
SRE_CORE_OBJS := $(patsubst src/sre/%.c, $(BUILD)/sre/%.o, \
                 $(patsubst src/sre/%.S,  $(BUILD)/sre/%.o, $(SRE_CORE_SRCS)))

$(BUILD)/sre/lua/src/%.o: src/sre/lua/src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/SRE-LUA] $<"
	@$(AARCH64_CC) $(SRE_LUA_CFLAGS) -c $< -o $@

$(BUILD)/sre/%.o: src/sre/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/SRE]    $<"
	@$(AARCH64_CC) $(SRE_SRE_CFLAGS) -c $< -o $@

$(BUILD)/sre/%.o: src/sre/%.S
	@mkdir -p $(dir $@)
	@echo "[ASM/SRE]   $<"
	@$(AARCH64_CC) $(SRE_SRE_CFLAGS) -c $< -o $@

libsre.so: $(SRE_LUA_OBJS) $(SRE_CORE_OBJS)
	@echo "[SRE]  Linking libsre.so (ARM64 guest)"
	@$(AARCH64_CC) -shared -fPIC -nostdlib -o $@ $^
	@echo "[SRE]  Built libsre.so — loaded by ElfLoader64"

# Deploy libsre.so to canonical engine path
install-sre: libsre.so
	@SRE_INSTALLED=0; \
	while IFS= read -r dir; do \
	    if [ -d "$$dir" ]; then \
	        cp libsre.so "$$dir/libsre.so"; \
	        echo "[SRE]  -> $$dir/"; \
	        SRE_INSTALLED=$$((SRE_INSTALLED + 1)); \
	    fi; \
	done < <(find "$(HOME)/.local/share/swordigo-desktop/engine" \
	             -type d -name "arm64-v8a" 2>/dev/null); \
	if [ "$$SRE_INSTALLED" -eq 0 ]; then \
	    mkdir -p "$(HOME)/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a"; \
	    cp libsre.so \
	       "$(HOME)/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/libsre.so"; \
	    echo "[SRE]  -> ~/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/ (fallback)"; \
	fi

# ============================================================================
# Dependency auto-includes
# ============================================================================
ALL_DEPS := \
    $(CORE_DEPS) $(GUI_DEPS) $(FORMATS_DEPS) $(FILERIFT_DEPS) \
    $(GFX_DEPS) $(EMU_DEPS) $(UI_DEPS) $(POD_DEPS) \
    $(BOOT_DEPS) $(RUBY_DEPS)

-include $(ALL_DEPS)

# ============================================================================
# Default & meta targets
# ============================================================================
.DEFAULT_GOAL := all

all: $(ALL_HOST_SO) $(BIN_DIR)/swordfare $(BIN_DIR)/ruby libsre.so
	@echo ""
	@echo "====================================================="
	@echo "  Swordfare Desktop — modular build complete"
	@echo "  Executables : bin/swordfare  bin/ruby"
	@echo "  Libraries   : bin/libs/*.so (7 distinct modules)"
	@echo "  Guest SRE   : libsre.so (ARM64)"
	@echo "  Run game    : ./run_swordigo.sh"
	@echo "====================================================="

clean:
	rm -rf $(BUILD) $(BIN_DIR) libsre.so
	@echo "[clean] Done."

.PHONY: all clean install-sre dynarmic-build dynarmic-clean ffmpeg-build ffmpeg-clean

# ============================================================================
# Dynarmic — build from source
# ============================================================================
dynarmic-build:
	@echo "[DYN]  Building Dynarmic JIT from source..."
	@mkdir -p $(DYNARMIC_BUILD)
	@cd $(DYNARMIC_BUILD) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DDYNARMIC_TESTS=OFF \
		-DDYNARMIC_FRONTENDS=A64 \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-DDYNARMIC_IGNORE_ASSERTS=ON \
		-DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
		-Wno-dev
	@cd $(DYNARMIC_BUILD) && make -j$$(nproc) dynarmic
	@echo "[DYN]  Built. Run: make -j$$(nproc)"

dynarmic-clean:
	rm -rf $(DYNARMIC_BUILD)

# ============================================================================
# FFmpeg — local static build
# ============================================================================
ffmpeg-build:
	@echo "[FFMPEG] Configuring and building local FFmpeg..."
	@mkdir -p $(FFMPEG_BUILD)
	@cd $(FFMPEG_DIR) && ./configure \
		--prefix=build \
		--enable-static \
		--disable-shared \
		--disable-all \
		--enable-avformat \
		--enable-avcodec \
		--enable-swscale \
		--enable-decoder=h264 \
		--enable-demuxer=mov \
		--enable-parser=h264 \
		--enable-protocols \
		--enable-protocol=file \
		--disable-programs \
		--disable-doc \
		--disable-avdevice \
		--disable-avfilter \
		--disable-swresample \
		--disable-libdrm \
		--disable-vulkan \
		--disable-hwaccels \
		--disable-network \
		--disable-iconv \
		--disable-bzlib \
		--disable-libxcb \
		--disable-lzma \
		--disable-sdl2 \
		--disable-xlib \
		--disable-zlib
	@cd $(FFMPEG_DIR) && make -j$$(nproc) && make install
	@echo "[FFMPEG] Build successful!"

ffmpeg-clean:
	@echo "[FFMPEG] Cleaning local FFmpeg build..."
	-@cd $(FFMPEG_DIR) && make distclean
	rm -rf $(FFMPEG_BUILD)
