# cupid-ps1 — PlayStation 1 emulator in C11
# Linux-only. SDL2 frontend.

CC      ?= cc
AR      ?= ar

CSTD     := -std=c11
WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
            -Wno-unused-parameter -Wno-unused-function
# glad's generated C is huge and trips strict-prototypes / missing-prototypes; relax for it only.
GLAD_WARN := -Wall -Wno-unused-parameter -Wno-unused-function
OPT      ?= -O2 -g
SANITIZERS ?=
SANITIZER_FLAGS := $(if $(SANITIZERS),-fsanitize=$(SANITIZERS) -fno-omit-frame-pointer,)
DEFS     := -D_GNU_SOURCE -DCUPID_GL_ENABLED $(EXTRA_DEFS)
ifeq ($(CUPID_FMV_DUMP),1)
DEFS     += -DCUPID_FMV_DUMP_BUILD
endif
INC      := -Isrc -Idep/glad/include -Idep/libchdr/include -Idep/lzma/include -Idep/soundtouch/include -Idep/stb

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

# libzstd is required by libchdr for newer CHDv5 dumps + by compress_helpers
# (Zstandard save-state codec).  Hard-fail at config time.
ZSTD_CFLAGS := $(shell pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LIBS   := $(shell pkg-config --libs   libzstd 2>/dev/null)
ifeq ($(strip $(ZSTD_LIBS)),)
$(error libzstd not found via pkg-config — install libzstd-dev / zstd to build CHD support)
endif

# liblzma (xz-utils) for compress_helpers XZ codec.  Hard-fail to keep parity
# with the zstd posture; install xz-devel / liblzma-dev if missing.
LZMA_CFLAGS_PKG := $(shell pkg-config --cflags liblzma 2>/dev/null)
LZMA_LIBS_PKG   := $(shell pkg-config --libs   liblzma 2>/dev/null)
ifeq ($(strip $(LZMA_LIBS_PKG)),)
$(error liblzma not found via pkg-config — install liblzma-dev / xz to build save-state XZ codec)
endif

CFLAGS   := $(CSTD) $(WARN) $(OPT) $(SANITIZER_FLAGS) $(DEFS) $(INC) $(SDL_CFLAGS) $(ZSTD_CFLAGS) $(LZMA_CFLAGS_PKG) -fno-strict-aliasing -fvisibility=hidden -MMD -MP
GLAD_CFLAGS := $(CSTD) $(GLAD_WARN) $(OPT) $(SANITIZER_FLAGS) $(DEFS) $(INC) -fno-strict-aliasing -fvisibility=hidden -MMD -MP
# Vendored third-party C (libchdr, lzma): relax warnings; we don't own the code.
DEP_CFLAGS  := -std=gnu11 -w $(OPT) $(SANITIZER_FLAGS) -D_GNU_SOURCE -Idep/libchdr/include -Idep/lzma/include $(ZSTD_CFLAGS) -fno-strict-aliasing -fvisibility=hidden -MMD -MP
LZMA_CFLAGS := $(DEP_CFLAGS) -DZ7_ST
# Stage the bundled audio library under its ELF SONAME beside each build.
# This works on checkouts that do not preserve symbolic links.
LDFLAGS  += $(SANITIZER_FLAGS) -L$(BUILD)/lib -Wl,-rpath,'$$ORIGIN/lib'
LDLIBS   := -lm -lpthread -lz $(ZSTD_LIBS) $(LZMA_LIBS_PKG) -ldl -lGL -lEGL -lX11 -ludev $(SDL_LIBS) -l:libsoundtouch.so.2

BUILD    ?= build
SRCDIR   := src
SOUNDTOUCH_RUNTIME := $(BUILD)/lib/libsoundtouch.so.2

# Discover nested subsystem files and retain the existing basename order.
recursive_files = $(wildcard $(1)$(2)) $(foreach dir,$(wildcard $(1)*/),$(call recursive_files,$(dir),$(2)))
by_basename = $(foreach name,$(sort $(notdir $(1))),$(filter %/$(name),$(1)))

COMMON_SRC := $(wildcard $(SRCDIR)/common/*.c)
UTIL_SRC   := $(call by_basename,$(call recursive_files,$(SRCDIR)/util/,*.c))
# gpu_commands.c is #include'd by gpu.c (translation-unit-internal); not a
# stand-alone TU, so exclude from the wildcard build list.
CORE_SRC   := $(filter-out $(SRCDIR)/core/gpu/gpu_commands.c,$(call by_basename,$(call recursive_files,$(SRCDIR)/core/,*.c)))
FRONTEND_SRC := $(wildcard $(SRCDIR)/frontend/*.c)

# Vendored glad (OpenGL + EGL function loader) -- generated C, relaxed warnings.
GLAD_SRC := dep/glad/src/gl.c dep/glad/src/egl.c
GLAD_OBJ := $(patsubst dep/%.c,$(BUILD)/dep/%.o,$(GLAD_SRC))

# Vendored libchdr (MAME CHD reader) + lzma (CHDv5 LZMA codec).  zstd is system-provided.
LIBCHDR_SRC := $(wildcard dep/libchdr/src/*.c)
LIBCHDR_OBJ := $(patsubst dep/%.c,$(BUILD)/dep/%.o,$(LIBCHDR_SRC))
LZMA_SRC    := $(wildcard dep/lzma/src/*.c)
LZMA_OBJ    := $(patsubst dep/%.c,$(BUILD)/dep/%.o,$(LZMA_SRC))
DEP_OBJ     := $(GLAD_OBJ) $(LIBCHDR_OBJ) $(LZMA_OBJ)

ALL_SRC  := $(COMMON_SRC) $(UTIL_SRC) $(CORE_SRC) $(FRONTEND_SRC)
ALL_OBJ  := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(ALL_SRC))

# Header-only compile check: each .h is force-included into a tiny .c so we catch
# missing includes / typos early.  Generated stubs land in $(BUILD)/hcheck/.
COMMON_HDR := $(wildcard $(SRCDIR)/common/*.h)
UTIL_HDR   := $(call by_basename,$(call recursive_files,$(SRCDIR)/util/,*.h))
CORE_HDR   := $(call by_basename,$(call recursive_files,$(SRCDIR)/core/,*.h))
FRONTEND_HDR := $(wildcard $(SRCDIR)/frontend/*.h)
ALL_HDR    := $(COMMON_HDR) $(UTIL_HDR) $(CORE_HDR) $(FRONTEND_HDR)
HCHECK_OBJ := $(patsubst $(SRCDIR)/%.h,$(BUILD)/hcheck/%.o,$(ALL_HDR))

TEST_SRC := tests/test_main.c tests/test_host_stubs.c \
            $(wildcard tests/common/*.c) \
            $(filter-out tests/util/graphics/test_gl_context_smoke.c,$(call by_basename,$(call recursive_files,tests/util/,*.c))) \
            $(call by_basename,$(call recursive_files,tests/core/,*.c))
TEST_OBJ := $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))
# Library-side objects the tests link against (everything except the
# binary's main() / SDL frontend / GL backend).  Filter out GL TUs since the
# test binary doesn't link glad (those symbols would be undefined).
TEST_LIB_OBJ := $(filter-out $(BUILD)/frontend/%.o $(BUILD)/util/graphics/opengl_%.o $(BUILD)/util/graphics/gpu_device.o $(BUILD)/util/graphics/shadergen.o $(BUILD)/core/gpu/hardware/gpu_hw_shadergen.o $(BUILD)/core/gpu/hardware/gpu_hw.o $(BUILD)/core/gpu/hardware/gpu_hw_texture_cache.o,$(ALL_OBJ))

.PHONY: all clean hcheck dirs test gl-smoke test-diff recomp-diff-test recomp-diff-ticks-test
all: dirs $(BUILD)/cupid-ps1

hcheck: dirs $(HCHECK_OBJ)

test: dirs $(BUILD)/cupid-ps1-tests
	@$(BUILD)/cupid-ps1-tests $(TEST_FILTER)

# Independent comparisons require explicit local inputs. Evidence stays in a new
# private directory; a timeout, missing progress or unequal state fails.
DIFF_BIOS       ?=
DIFF_GAME       ?=
DIFF_LINES      ?= 50000
DIFF_SECONDS    ?= 60
DIFF_INTERVAL   ?= 60
DIFF_CHECKPOINTS ?= 5
DIFF_MIN_BLOCKS ?= 100
DIFF_FASTMEM    ?= off
DIFF_LINKING    ?= on
DIFF_LEFT       ?= interp
DIFF_RIGHT      ?= cached
DIFF_OUTPUT     ?=
DIFF_HASH_INPUT ?=
DIFF_OPTIONS = --binary "$(abspath $(BUILD)/cupid-ps1)" --bios "$(DIFF_BIOS)" \
               --timeout "$(DIFF_SECONDS)" --minimum-blocks "$(DIFF_MIN_BLOCKS)" \
               --fastmem "$(DIFF_FASTMEM)" $(if $(filter off,$(DIFF_LINKING)),--no-linking) \
               $(if $(DIFF_OUTPUT),--output "$(DIFF_OUTPUT)") \
               $(if $(DIFF_HASH_INPUT),--hash-input "$(DIFF_HASH_INPUT)")

test-diff: dirs $(BUILD)/cupid-ps1
	python3 scripts/compare_execution.py $(DIFF_OPTIONS) --kind trace \
	  --left "$(DIFF_LEFT)" --right "$(DIFF_RIGHT)" --instructions "$(DIFF_LINES)" "$(DIFF_GAME)"

recomp-diff-test: dirs $(BUILD)/cupid-ps1
	python3 scripts/compare_execution.py $(DIFF_OPTIONS) --kind state --right recomp \
	  --interval "$(DIFF_INTERVAL)" --checkpoints "$(DIFF_CHECKPOINTS)" "$(DIFF_GAME)"

# State comparisons already include serialized CPU/scheduler timing and global ticks.
recomp-diff-ticks-test: recomp-diff-test

# GL context verification binary: needs live X11 display; not run by `make test`.
GL_SMOKE_OBJ := $(BUILD)/tests/util/graphics/test_gl_context_smoke.o
GL_SMOKE_LIB_OBJ := $(BUILD)/util/graphics/opengl_context.o $(BUILD)/util/graphics/opengl_context_egl.o \
                    $(BUILD)/util/graphics/opengl_context_egl_xlib.o $(BUILD)/util/platform/window_info.o \
                    $(BUILD)/common/error.o $(BUILD)/common/log.o \
                    $(BUILD)/common/dynamic_library.o $(BUILD)/common/small_string.o \
                    $(BUILD)/common/string_util.o $(BUILD)/common/assert.o $(BUILD)/common/timer.o \
                    $(BUILD)/common/file_system.o $(BUILD)/common/path.o $(BUILD)/common/threading.o \
                    $(BUILD)/common/memmap.o
gl-smoke: dirs $(BUILD)/cupid-ps1-gl-smoke

$(BUILD)/cupid-ps1-gl-smoke: $(GL_SMOKE_OBJ) $(GL_SMOKE_LIB_OBJ) $(GLAD_OBJ) | $(SOUNDTOUCH_RUNTIME)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/cupid-ps1-tests: $(TEST_OBJ) $(TEST_LIB_OBJ) $(LIBCHDR_OBJ) $(LZMA_OBJ) | $(SOUNDTOUCH_RUNTIME)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c $< -o $@

dirs:
	@mkdir -p $(BUILD)/common $(BUILD)/util $(BUILD)/core $(BUILD)/frontend
	@mkdir -p $(BUILD)/hcheck/common $(BUILD)/hcheck/util $(BUILD)/hcheck/core
	@mkdir -p $(BUILD)/tests/common $(BUILD)/tests/util $(BUILD)/tests/core
	@mkdir -p $(BUILD)/dep/glad/src $(BUILD)/dep/libchdr/src $(BUILD)/dep/lzma/src

$(SOUNDTOUCH_RUNTIME): dep/soundtouch/lib/libsoundtouch.so.2.3.3
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/cupid-ps1: $(ALL_OBJ) $(DEP_OBJ) | $(SOUNDTOUCH_RUNTIME)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# AVX2-specialised rasterizer TU.  Compiled with -mavx2 so future GSVector-C
# intrinsics link cleanly; runtime dispatch picks scalar vs AVX2 via
# cpu_features_init().  Per-TU rule overrides the generic pattern below.
$(BUILD)/core/gpu/software/gpu_sw_rasterizer_avx2.o: $(SRCDIR)/core/gpu/software/gpu_sw_rasterizer_avx2.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mavx2 -mavx -msse4.1 -msse2 -mfma -c $< -o $@

# SSE4.1 SIMD rasterizer TU (R22 Phase C). 4-pixel-wide SIMD body.
$(BUILD)/core/gpu/software/gpu_sw_rasterizer_simd.o: $(SRCDIR)/core/gpu/software/gpu_sw_rasterizer_simd.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -msse4.1 -msse2 -c $< -o $@

# GSVector unit tests exercise both SSE4.1 and AVX2 ops; build with -mavx2
# so all intrinsics resolve. Runtime AVX2 paths inside the test are guarded
# by __AVX2__ at compile time.
$(BUILD)/tests/common/test_gsvector.o: tests/common/test_gsvector.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mavx2 -mavx -msse4.1 -msse2 -mfma -I. -c $< -o $@

# Texture-replacement loader is the sole instantiator of stb_image.h.  Compile
# with -w because the vendored single-header lib has many implicit-conversion
# warnings we don't want polluting the regular -Wall -Wextra build.
$(BUILD)/core/gpu/hardware/tc_image_loader.o: $(SRCDIR)/core/gpu/hardware/tc_image_loader.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(SANITIZER_FLAGS) $(DEFS) $(INC) $(SDL_CFLAGS) -fno-strict-aliasing -fvisibility=hidden -MMD -MP -w -c $< -o $@

$(BUILD)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dep/glad/%.o: dep/glad/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GLAD_CFLAGS) -c $< -o $@

$(BUILD)/dep/libchdr/%.o: dep/libchdr/%.c
	@mkdir -p $(dir $@)
	$(CC) $(DEP_CFLAGS) -c $< -o $@

$(BUILD)/dep/lzma/%.o: dep/lzma/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LZMA_CFLAGS) -c $< -o $@

# Compile each header alone via -include trick.
$(BUILD)/hcheck/%.o: $(SRCDIR)/%.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -x c -include $< /dev/null -c -o $@

clean:
	rm -rf $(BUILD)

-include $(ALL_OBJ:.o=.d) $(DEP_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(HCHECK_OBJ:.o=.d) $(GL_SMOKE_OBJ:.o=.d)
