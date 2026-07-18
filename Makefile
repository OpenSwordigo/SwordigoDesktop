## Swordigo Desktop - Makefile
## Usage: make -j$(nproc) && ./swordigo_boot

CXX     := g++
CC      := gcc
CXXFLAGS := -std=c++17 -g -O3 -Isrc -Isrc/imgui -Isrc/sre/lua/src -Iinclude -Isrc/stb -MMD -MP
CFLAGS   := -g -O3 -Isrc -Isrc/sre/lua/src -Iinclude -MMD -MP

# pkg-config queries
SDL3_CFLAGS  := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS    := $(shell pkg-config --libs sdl3 2>/dev/null)
SDL3I_CFLAGS := $(shell pkg-config --cflags SDL3_image 2>/dev/null || pkg-config --cflags sdl3-image 2>/dev/null)
SDL3I_LIBS   := $(shell pkg-config --libs SDL3_image 2>/dev/null || pkg-config --libs sdl3-image 2>/dev/null || echo "-lSDL3_image")
VORB_CFLAGS  := $(shell pkg-config --cflags vorbisfile 2>/dev/null)
VORB_LIBS    := $(shell pkg-config --libs vorbisfile 2>/dev/null)
MP3_CFLAGS   := $(shell pkg-config --cflags libmpg123 2>/dev/null)
MP3_LIBS     := $(shell pkg-config --libs libmpg123 2>/dev/null)
ZLIB_LIBS    := $(shell pkg-config --libs zlib 2>/dev/null || echo "-lz")
OAL_LIBS     := $(shell pkg-config --libs openal 2>/dev/null || echo "-lopenal")

ALL_CXXFLAGS := $(CXXFLAGS) $(SDL3_CFLAGS) $(SDL3I_CFLAGS) $(VORB_CFLAGS) $(MP3_CFLAGS)
ALL_CFLAGS   := $(CFLAGS) $(SDL3_CFLAGS)

LIBS := $(SDL3_LIBS) $(SDL3I_LIBS) $(VORB_LIBS) $(MP3_LIBS) $(ZLIB_LIBS) $(OAL_LIBS) \
        -lunicorn -lGL -lpthread -lm -ldl

# ========== Dynarmic JIT backend (optional) ==========
# Build with: make DYNARMIC=1
# Requires: deps/dynarmic/ (git clone https://github.com/lioncash/dynarmic)
# First run: make dynarmic-build  (builds the static library)
DYNARMIC_DIR := deps/dynarmic
DYNARMIC_BUILD := $(DYNARMIC_DIR)/build
ALL_CXXFLAGS += -DUSE_DYNARMIC -I$(DYNARMIC_DIR)/src
LIBS += -L$(DYNARMIC_BUILD)/src/dynarmic -ldynarmic \
        -L$(DYNARMIC_BUILD)/externals/mcl/src -lmcl \
        -L$(DYNARMIC_BUILD)/externals/fmt -lfmt \
        -L$(DYNARMIC_BUILD)/externals/zydis -lZydis \
        -L$(DYNARMIC_BUILD)/externals/zydis/zycore -lZycore

# Source files
CXX_SRCS := \
    src/main.cpp \
    src/jni/jni_marshaller.cpp \
    src/platform/display.cpp \
    src/loader/elf_loader.cpp \
    src/loader/elf_loader_arm64.cpp \
    src/jni/jni_bridge.cpp \
    src/jni/jni_bridge_arm64.cpp \
    src/platform/emulator.cpp \
    src/platform/emulator_arm64.cpp \
    src/platform/gui.cpp \
    src/platform/input_config.cpp \
    src/game/camera_override.cpp \
    src/game/mod_tools.cpp \
    src/game/save_editor_logic.cpp \
    src/game/mod_config.cpp \
    src/platform/fbo_scaler.cpp \
    src/platform/pvr_loader.cpp \
    src/platform/io_thread.cpp \
    src/platform/data_path.cpp \
    src/platform/binary_selector.cpp \
    src/platform/launcher_ui.cpp \
    src/platform/save_editor.cpp \
    src/platform/scl_parser.cpp \
    src/tools/filerift.cpp \
    src/tools/scene_schemas.cpp \
    src/tools/boulder.cpp \
    src/platform/srt_overlay.cpp \
    src/platform/video_background.cpp \
    src/platform/swordfare_gui.cpp \
    src/platform/rgc.cpp \
    src/platform/pvrtc_decoder.cpp \
    src/imgui/imgui.cpp \
    src/imgui/imgui_draw.cpp \
    src/imgui/imgui_tables.cpp \
    src/imgui/imgui_widgets.cpp \
    src/imgui/backends/imgui_impl_sdl3.cpp \
    src/imgui/backends/imgui_impl_opengl3.cpp

# Unconditionally include Dynarmic backend
CXX_SRCS += src/platform/emulator_dynarmic64.cpp

C_SRCS := \
    src/android/asset_manager.c \
    src/android/asset_manager_arm32.c \
    src/android/log.c

# Object files (in build/ dir)
CXX_OBJS := $(patsubst src/%.cpp, build/%.o, $(CXX_SRCS))
C_OBJS   := $(patsubst src/%.c, build/%.o, $(C_SRCS))
# Host-side Lua objects — needed by filerift.cpp (compile_lua_to_bytecode)
# LUA_SRCS is defined later in the file (for ruby/AV); forward-declare the obj list here.
# We reuse the same build/host_lua/ objects so they're built once and shared.
HOST_LUA_C_SRCS := \
    src/sre/lua/src/lapi.c src/sre/lua/src/lcode.c src/sre/lua/src/ldebug.c \
    src/sre/lua/src/ldo.c src/sre/lua/src/ldump.c src/sre/lua/src/lfunc.c \
    src/sre/lua/src/lgc.c src/sre/lua/src/llex.c src/sre/lua/src/lmem.c \
    src/sre/lua/src/lobject.c src/sre/lua/src/lopcodes.c src/sre/lua/src/lparser.c \
    src/sre/lua/src/lstate.c src/sre/lua/src/lstring.c src/sre/lua/src/ltable.c \
    src/sre/lua/src/ltm.c src/sre/lua/src/lundump.c src/sre/lua/src/lvm.c \
    src/sre/lua/src/lzio.c src/sre/lua/src/lauxlib.c src/sre/lua/src/lbaselib.c \
    src/sre/lua/src/ldblib.c src/sre/lua/src/liolib.c src/sre/lua/src/lmathlib.c \
    src/sre/lua/src/loslib.c src/sre/lua/src/lstrlib.c src/sre/lua/src/ltablib.c \
    src/sre/lua/src/loadlib.c src/sre/lua/src/linit.c
HOST_LUA_OBJS := $(patsubst src/sre/lua/src/%.c, build/host_lua/%.o, $(HOST_LUA_C_SRCS))
ALL_OBJS := $(CXX_OBJS) $(C_OBJS) $(HOST_LUA_OBJS)

# Auto-generated dependency files
DEPS := $(ALL_OBJS:.o=.d)

# Default target
all: swordigo_boot libsre.so ruby

swordigo_boot: $(ALL_OBJS)
	@echo "[LINK] $@"
	@$(CXX) -o $@ $^ $(LIBS)

# =========================================================================
# libsre.so — Swordigo Runtime Engine (ARM64 cross-compiled)
# =========================================================================
# This is a guest-side library loaded into the Unicorn emulator.
# It replaces problematic functions in libswordigo.so with clean C code.
AARCH64_CC := aarch64-linux-gnu-gcc
SRE_LUA_SRCS := \
    src/sre/lua/src/lapi.c \
    src/sre/lua/src/lcode.c \
    src/sre/lua/src/ldebug.c \
    src/sre/lua/src/ldo.c \
    src/sre/lua/src/ldump.c \
    src/sre/lua/src/lfunc.c \
    src/sre/lua/src/lgc.c \
    src/sre/lua/src/llex.c \
    src/sre/lua/src/lmem.c \
    src/sre/lua/src/lobject.c \
    src/sre/lua/src/lopcodes.c \
    src/sre/lua/src/lparser.c \
    src/sre/lua/src/lstate.c \
    src/sre/lua/src/lstring.c \
    src/sre/lua/src/ltable.c \
    src/sre/lua/src/ltm.c \
    src/sre/lua/src/lundump.c \
    src/sre/lua/src/lvm.c \
    src/sre/lua/src/lzio.c \
    src/sre/lua/src/lauxlib.c \
    src/sre/lua/src/lbaselib.c \
    src/sre/lua/src/ldblib.c \
    src/sre/lua/src/liolib.c \
    src/sre/lua/src/lmathlib.c \
    src/sre/lua/src/loslib.c \
    src/sre/lua/src/lstrlib.c \
    src/sre/lua/src/ltablib.c \
    src/sre/lua/src/loadlib.c \
    src/sre/lua/src/linit.c

SRE_CORE_SRCS := \
    src/sre/sre_init.c \
    src/sre/sre_string.c \
    src/sre/sre_lua.c \
    src/sre/sre_background.c \
    src/sre/sre_effects.c \
    src/sre/sre_music.c \
    src/sre/sre_gui.c \
    src/sre/sre_gui_native.c \
    src/sre/sre_scene_update.c \
    src/sre/sre_setjmp.S \
    src/sre/sre_mini_api.c \
    src/sre/sre_vfs.c \
    src/sre/sre_lua_libs.c \
    src/sre/sre_mod.c \
    src/sre/sre_config.c \
    src/sre/sre_caver.c \
    src/sre/toml-c/toml.c \
    src/sre/luafilesystem/src/lfs.c \
    src/sre/luasocket/src/auxiliar.c \
    src/sre/luasocket/src/buffer.c \
    src/sre/luasocket/src/except.c \
    src/sre/luasocket/src/inet.c \
    src/sre/luasocket/src/io.c \
    src/sre/luasocket/src/luasocket.c \
    src/sre/luasocket/src/mime.c \
    src/sre/luasocket/src/options.c \
    src/sre/luasocket/src/select.c \
    src/sre/luasocket/src/tcp.c \
    src/sre/luasocket/src/timeout.c \
    src/sre/luasocket/src/udp.c \
    src/sre/luasocket/src/usocket.c

SRE_LUA_OBJS := $(patsubst src/sre/%.c, build/sre/%.o, $(SRE_LUA_SRCS))
SRE_CORE_OBJS := $(patsubst src/sre/%.c, build/sre/%.o, $(patsubst src/sre/%.S, build/sre/%.o, $(SRE_CORE_SRCS)))

SRE_LUA_CFLAGS := -shared -fPIC -O2 -nostdlib -fno-builtin -fno-stack-protector \
    -Isrc/sre/include \
    -Isrc/sre/lua/src

SRE_SRE_CFLAGS := $(SRE_LUA_CFLAGS) \
    -Isrc/sre/toml-c \
    -Isrc/sre/lua/luasocket/src \
    -include src/sre/sre_lua_compat.h

build/sre/lua/src/%.o: src/sre/lua/src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/SRE-LUA] $<"
	@$(AARCH64_CC) $(SRE_LUA_CFLAGS) -c $< -o $@

build/sre/%.o: src/sre/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/SRE] $<"
	@$(AARCH64_CC) $(SRE_SRE_CFLAGS) -c $< -o $@

build/sre/%.o: src/sre/%.S
	@mkdir -p $(dir $@)
	@echo "[ASM/SRE] $<"
	@$(AARCH64_CC) $(SRE_SRE_CFLAGS) -c $< -o $@

libsre.so: $(SRE_LUA_OBJS) $(SRE_CORE_OBJS)
	@echo "[SRE]  Linking libsre.so (ARM64)"
	@$(AARCH64_CC) -shared -fPIC -nostdlib -o $@ $^
	@echo "[SRE]  Built libsre.so (ARM64) — ready for emulator loading"

# Install libsre.so alongside libswordigo.so
install-sre: libsre.so
	@mkdir -p $(HOME)/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/
	@cp libsre.so $(HOME)/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/libsre.so
	@echo "[SRE]  Installed to ~/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a/"

# =========================================================================
# ruby — Standalone asset browser/previewer tool
# =========================================================================
# Separate binary with minimal dependencies (no unicorn, no game code).
AV_SRCS := src/tools/asset_viewer.cpp \
    src/tools/filerift.cpp src/tools/scene_schemas.cpp src/tools/boulder.cpp \
    src/tools/pod_loader.cpp src/tools/av_renderer.cpp \
    src/tools/av_audio.cpp src/tools/scene_loader.cpp src/tools/intellij.cpp \
    src/tools/batch_converter.cpp \
    src/platform/pvr_loader.cpp \
    src/platform/data_path.cpp \
    src/platform/pvrtc_decoder.cpp \
    src/imgui/imgui.cpp src/imgui/imgui_draw.cpp \
    src/imgui/imgui_tables.cpp src/imgui/imgui_widgets.cpp \
    src/imgui/backends/imgui_impl_sdl3.cpp src/imgui/backends/imgui_impl_opengl3.cpp

LUA_SRCS := \
    src/sre/lua/src/lapi.c \
    src/sre/lua/src/lcode.c \
    src/sre/lua/src/ldebug.c \
    src/sre/lua/src/ldo.c \
    src/sre/lua/src/ldump.c \
    src/sre/lua/src/lfunc.c \
    src/sre/lua/src/lgc.c \
    src/sre/lua/src/llex.c \
    src/sre/lua/src/lmem.c \
    src/sre/lua/src/lobject.c \
    src/sre/lua/src/lopcodes.c \
    src/sre/lua/src/lparser.c \
    src/sre/lua/src/lstate.c \
    src/sre/lua/src/lstring.c \
    src/sre/lua/src/ltable.c \
    src/sre/lua/src/ltm.c \
    src/sre/lua/src/lundump.c \
    src/sre/lua/src/lvm.c \
    src/sre/lua/src/lzio.c \
    src/sre/lua/src/lauxlib.c \
    src/sre/lua/src/lbaselib.c \
    src/sre/lua/src/ldblib.c \
    src/sre/lua/src/liolib.c \
    src/sre/lua/src/lmathlib.c \
    src/sre/lua/src/loslib.c \
    src/sre/lua/src/lstrlib.c \
    src/sre/lua/src/ltablib.c \
    src/sre/lua/src/loadlib.c \
    src/sre/lua/src/linit.c

AV_OBJS := $(patsubst src/%.cpp, build/%.o, $(AV_SRCS)) $(patsubst src/sre/lua/src/%.c, build/host_lua/%.o, $(LUA_SRCS))
AV_LIBS := $(SDL3_LIBS) $(SDL3I_LIBS) -lGL $(ZLIB_LIBS) -lutil

ruby: $(AV_OBJS)
	@echo "[LINK] $@"
	@$(CXX) -o $@ $^ $(AV_LIBS)
	@echo "[AV]   Built ruby — run with: ./ruby"

# C++ compile rule
build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX]  $<"
	@$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

# Host Lua compile rule
build/host_lua/%.o: src/sre/lua/src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC/HOST-LUA] $<"
	@$(CC) $(ALL_CFLAGS) -c $< -o $@

# C compile rule
build/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]   $<"
	@$(CC) $(ALL_CFLAGS) -c $< -o $@

# Include auto-generated dependency files (if they exist)
-include $(DEPS)

clean:
	rm -rf build swordigo_boot libsre.so ruby

.PHONY: all clean install-sre ruby dynarmic-build dynarmic-clean

# ========== Dynarmic Build from Source ==========
# Run this ONCE before building with DYNARMIC=1
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
	@echo "[DYN]  Dynarmic built successfully!"
	@echo "[DYN]  Now build SwordigoDesktop with: make DYNARMIC=1 swordigo_boot"

dynarmic-clean:
	rm -rf $(DYNARMIC_BUILD)
