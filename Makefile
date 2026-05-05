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

CFLAGS   := $(CSTD) $(WARN) $(OPT) $(DEFS) $(INC) $(SDL_CFLAGS) $(LZMA_CFLAGS_PKG) -fno-strict-aliasing -fvisibility=hidden
GLAD_CFLAGS := $(CSTD) $(GLAD_WARN) $(OPT) $(DEFS) $(INC) -fno-strict-aliasing -fvisibility=hidden
# Vendored third-party C (libchdr, lzma): relax warnings; we don't own the code.
DEP_CFLAGS  := -std=gnu11 -w $(OPT) -D_GNU_SOURCE -Idep/libchdr/include -Idep/lzma/include $(ZSTD_CFLAGS) -fno-strict-aliasing -fvisibility=hidden
LZMA_CFLAGS := $(DEP_CFLAGS) -DZ7_ST
# Vendored SoundTouch (audio time-stretch / resample, LGPL).  Linked at
# build/cupid-ps1's $ORIGIN-relative path so the binary runs without
# LD_LIBRARY_PATH (build/cupid-ps1 -> ../dep/soundtouch/lib/libsoundtouch.so.2).
LDFLAGS  := -Ldep/soundtouch/lib -Wl,-rpath,'$$ORIGIN/../dep/soundtouch/lib'
LDLIBS   := -lm -lpthread -lz $(ZSTD_LIBS) $(LZMA_LIBS_PKG) -ldl -lGL -lEGL -lX11 -ludev $(SDL_LIBS) -lsoundtouch

BUILD    := build
SRCDIR   := src

COMMON_SRC := $(wildcard $(SRCDIR)/common/*.c)
UTIL_SRC   := $(wildcard $(SRCDIR)/util/*.c)
# gpu_commands.c is #include'd by gpu.c (translation-unit-internal); not a
# stand-alone TU, so exclude from the wildcard build list.
CORE_SRC   := $(filter-out $(SRCDIR)/core/gpu_commands.c,$(wildcard $(SRCDIR)/core/*.c))
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
UTIL_HDR   := $(wildcard $(SRCDIR)/util/*.h)
CORE_HDR   := $(wildcard $(SRCDIR)/core/*.h)
ALL_HDR    := $(COMMON_HDR) $(UTIL_HDR) $(CORE_HDR)
HCHECK_OBJ := $(patsubst $(SRCDIR)/%.h,$(BUILD)/hcheck/%.o,$(ALL_HDR))

TEST_SRC := tests/test_main.c tests/test_host_stubs.c \
            $(wildcard tests/common/*.c) \
            $(filter-out tests/util/test_gl_context_smoke.c,$(wildcard tests/util/*.c)) \
            $(wildcard tests/core/*.c)
TEST_OBJ := $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))
# Library-side objects the tests link against (everything except the
# binary's main() / SDL frontend / GL backend).  Filter out GL TUs since the
# test binary doesn't link glad (those symbols would be undefined).
TEST_LIB_OBJ := $(filter-out $(BUILD)/frontend/%.o $(BUILD)/util/opengl_%.o $(BUILD)/util/gpu_device.o $(BUILD)/util/shadergen.o $(BUILD)/core/gpu_hw_shadergen.o $(BUILD)/core/gpu_hw.o $(BUILD)/core/gpu_hw_texture_cache.o,$(ALL_OBJ))

.PHONY: all clean hcheck dirs test gl-smoke test-diff recomp-diff-test
all: dirs $(BUILD)/cupid-ps1

hcheck: dirs $(HCHECK_OBJ)

test: dirs $(BUILD)/cupid-ps1-tests
	@$(BUILD)/cupid-ps1-tests

# Interpreter determinism / diff-trace harness.
# Records a per-instruction trace of two interpreter boots with
# identical inputs and compares them byte-exactly.  Recomp-side tracing
# requires register-allocator-aware JIT instrumentation and is deferred
# (the hook + vtable slot exist; cpu_recompiler_compile_instruction
# does not currently dispatch it because the JIT call perturbs flag
# and host-reg state in a way that masks real divergences).  Override:
#   make test-diff DIFF_BIOS=path/to/bios DIFF_GAME=path/to/game.bin \
#                  DIFF_LINES=50000 DIFF_SECONDS=10
DIFF_BIOS    ?= ./SCPH1001.BIN
DIFF_GAME    ?= Crash Bandicoot (USA).bin
DIFF_LINES   ?= 50000
DIFF_SECONDS ?= 10

test-diff: dirs $(BUILD)/cupid-ps1
	@echo "=== diff-trace: interpreter run A ($(DIFF_SECONDS)s) ==="
	@SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy CUPID_DIFF_TRACE=/tmp/cupid-ps1-diff-A.txt \
	  timeout $(DIFF_SECONDS) $(BUILD)/cupid-ps1 --cpu-mode interp --bios "$(DIFF_BIOS)" "$(DIFF_GAME)" \
	  >/dev/null 2>&1; true
	@echo "=== diff-trace: interpreter run B ($(DIFF_SECONDS)s) ==="
	@SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy CUPID_DIFF_TRACE=/tmp/cupid-ps1-diff-B.txt \
	  timeout $(DIFF_SECONDS) $(BUILD)/cupid-ps1 --cpu-mode interp --bios "$(DIFF_BIOS)" "$(DIFF_GAME)" \
	  >/dev/null 2>&1; true
	@head -n $(DIFF_LINES) /tmp/cupid-ps1-diff-A.txt > /tmp/cupid-ps1-diff-A.head
	@head -n $(DIFF_LINES) /tmp/cupid-ps1-diff-B.txt > /tmp/cupid-ps1-diff-B.head
	@if cmp -s /tmp/cupid-ps1-diff-A.head /tmp/cupid-ps1-diff-B.head; then \
	  echo "PASS: interpreter is deterministic across $(DIFF_LINES) instructions"; \
	else \
	  echo "FAIL: interpreter diverged across two runs:"; \
	  diff -u /tmp/cupid-ps1-diff-A.head /tmp/cupid-ps1-diff-B.head | head -40; \
	  exit 1; \
	fi

# Per-block recomp/interp register-file diff.  Runs the
# recompiler with CUPID_RECOMP_DIFF=1 for $(DIFF_SECONDS) seconds.  Each
# compiled block snapshots state at prologue, then on epilogue runs the
# interpreter on a scratch copy of the entry state and aborts on a register
# divergence.  Skipped fields: pc/npc, pending_ticks, completion ticks
# (control-flow + timing accumulator artifacts -- see cpu_diff_block.c).
# Override DIFF_BIOS / DIFF_GAME / DIFF_SECONDS as for test-diff.
recomp-diff-test: dirs $(BUILD)/cupid-ps1
	@echo "=== recomp-diff-test: $(DIFF_SECONDS)s recomp + diff (linking=$(if $(DIFF_LINKING),on,off) fastmem=$(if $(DIFF_FASTMEM),on,off)) ==="
	@if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy CUPID_RECOMP_DIFF=1 \
	    $(if $(DIFF_LINKING),CUPID_RECOMP_DIFF_KEEP_LINKING=1) \
	    $(if $(DIFF_FASTMEM),CUPID_RECOMP_DIFF_KEEP_FASTMEM=1) \
	    timeout $(DIFF_SECONDS) $(BUILD)/cupid-ps1 --cpu-mode recomp \
	    --bios "$(DIFF_BIOS)" "$(DIFF_GAME)" >/tmp/cupid-ps1-recomp-diff.log 2>&1; \
	    rc=$$?; [ $$rc -eq 0 ] || [ $$rc -eq 124 ]; then \
	  if grep -q '\[recomp-diff\] divergence' /tmp/cupid-ps1-recomp-diff.log; then \
	    echo "FAIL: register divergence detected:"; \
	    grep -A 4 '\[recomp-diff\]' /tmp/cupid-ps1-recomp-diff.log | head -40; \
	    exit 1; \
	  else \
	    echo "PASS: 0 register-level divergences over $(DIFF_SECONDS)s of recomp."; \
	  fi; \
	else \
	  echo "FAIL: cupid-ps1 exited abnormally (rc=$$rc) under diff mode:"; \
	  tail -40 /tmp/cupid-ps1-recomp-diff.log; \
	  exit 1; \
	fi

# Per-block tick-precise diff.  Same harness as recomp-diff-test
# but adds CUPID_RECOMP_DIFF_TICKS=1 so the per-block compare strict-checks
# pending_ticks delta + IRQ controller status/mask.  Forces both linking
# and fastmem ON so the real production code path is exercised; the bug
# we are hunting (interp/recomp tick drift -> shifted IRQ delivery ->
# kernel TCB corruption -> wild PC) only surfaces under load with the
# full code-cache pipeline live.  abort() at first divergence; the
# emitted log shows block_pc/size/recomp_delta/interp_delta -- the block
# PC is the fix point.
recomp-diff-ticks-test: dirs $(BUILD)/cupid-ps1
	@echo "=== recomp-diff-ticks-test: $(DIFF_SECONDS)s recomp + ticks-strict diff ==="
	@if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy CUPID_RECOMP_DIFF=1 \
	    CUPID_RECOMP_DIFF_TICKS=1 \
	    CUPID_RECOMP_DIFF_KEEP_LINKING=1 CUPID_RECOMP_DIFF_KEEP_FASTMEM=1 \
	    timeout $(DIFF_SECONDS) $(BUILD)/cupid-ps1 --cpu-mode recomp \
	    --bios "$(DIFF_BIOS)" "$(DIFF_GAME)" >/tmp/cupid-ps1-recomp-diff-ticks.log 2>&1; \
	    rc=$$?; [ $$rc -eq 0 ] || [ $$rc -eq 124 ]; then \
	  if grep -q '\[recomp-diff\] divergence' /tmp/cupid-ps1-recomp-diff-ticks.log; then \
	    echo "FAIL: tick/IRQ divergence detected:"; \
	    grep -B 0 -A 8 '\[recomp-diff\]' /tmp/cupid-ps1-recomp-diff-ticks.log | head -80; \
	    exit 1; \
	  else \
	    echo "PASS: 0 tick/IRQ divergences over $(DIFF_SECONDS)s of recomp."; \
	  fi; \
	else \
	  echo "FAIL: cupid-ps1 exited abnormally (rc=$$rc) under ticks-diff mode:"; \
	  tail -40 /tmp/cupid-ps1-recomp-diff-ticks.log; \
	  exit 1; \
	fi

# GL context verification binary: needs live X11 display; not run by `make test`.
GL_SMOKE_OBJ := $(BUILD)/tests/util/test_gl_context_smoke.o
GL_SMOKE_LIB_OBJ := $(BUILD)/util/opengl_context.o $(BUILD)/util/opengl_context_egl.o \
                    $(BUILD)/util/opengl_context_egl_xlib.o $(BUILD)/util/window_info.o \
                    $(BUILD)/common/error.o $(BUILD)/common/log.o \
                    $(BUILD)/common/dynamic_library.o $(BUILD)/common/small_string.o \
                    $(BUILD)/common/string_util.o $(BUILD)/common/assert.o $(BUILD)/common/timer.o \
                    $(BUILD)/common/file_system.o $(BUILD)/common/path.o $(BUILD)/common/threading.o \
                    $(BUILD)/common/memmap.o
gl-smoke: dirs $(BUILD)/cupid-ps1-gl-smoke

$(BUILD)/cupid-ps1-gl-smoke: $(GL_SMOKE_OBJ) $(GL_SMOKE_LIB_OBJ) $(GLAD_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/cupid-ps1-tests: $(TEST_OBJ) $(TEST_LIB_OBJ) $(LIBCHDR_OBJ) $(LZMA_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c $< -o $@

dirs:
	@mkdir -p $(BUILD)/common $(BUILD)/util $(BUILD)/core $(BUILD)/frontend
	@mkdir -p $(BUILD)/hcheck/common $(BUILD)/hcheck/util $(BUILD)/hcheck/core
	@mkdir -p $(BUILD)/tests/common $(BUILD)/tests/util $(BUILD)/tests/core
	@mkdir -p $(BUILD)/dep/glad/src $(BUILD)/dep/libchdr/src $(BUILD)/dep/lzma/src

$(BUILD)/cupid-ps1: $(ALL_OBJ) $(DEP_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# AVX2-specialised rasterizer TU.  Compiled with -mavx2 so future GSVector-C
# intrinsics link cleanly; runtime dispatch picks scalar vs AVX2 via
# cpu_features_init().  Per-TU rule overrides the generic pattern below.
$(BUILD)/core/gpu_sw_rasterizer_avx2.o: $(SRCDIR)/core/gpu_sw_rasterizer_avx2.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mavx2 -mavx -msse4.1 -msse2 -mfma -c $< -o $@

# SSE4.1 SIMD rasterizer TU (R22 Phase C). 4-pixel-wide SIMD body.
$(BUILD)/core/gpu_sw_rasterizer_simd.o: $(SRCDIR)/core/gpu_sw_rasterizer_simd.c
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
$(BUILD)/core/tc_image_loader.o: $(SRCDIR)/core/tc_image_loader.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(DEFS) $(INC) $(SDL_CFLAGS) -fno-strict-aliasing -fvisibility=hidden -w -c $< -o $@

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
