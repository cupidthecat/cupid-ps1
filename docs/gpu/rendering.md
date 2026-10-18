# Rendering

Two backends, picked by `--gpu-renderer` or settings INI. Backend abstraction: `src/core/gpu/gpu_backend.h`.

GPU command resets and interrupt acknowledgement are described in
[GPU reset and interrupts](reset-and-irqs.md).

## Software renderer (`gpu_sw`)

Source: `src/core/gpu/software/gpu_sw.c`, `src/core/gpu/software/gpu_sw_rasterizer.c`. Default rendering path.

- GP0/GP1 command processing: polygons, rectangles, lines, VRAM fill/copy.
- Dithering, true-colour modes.
- 4-bit / 8-bit / 16-bit texture sampling with palette lookup.
- All four transparency modes (HALF, ADD, SUB, QUARTER).
- 24-bit FMV path with optional chroma smoothing.
- PGXP subpixel correction when enabled.
- Runtime-dispatched rasterizer vtable: SCALAR, SIMD (SSE4.1, 4-pixel-wide), AVX2 (256-bit VRAM blits over SIMD per-pixel ops). Picked by `cpu_features_init()` at startup; overridable via `CUPID_GPU_SW_ISA` env var or `gpu_sw_use_isa` setting (`SCALAR` / `SIMD` / `AVX2`). Sources: `gpu_sw_rasterizer_simd.c`, `gpu_sw_rasterizer_avx2.c`.

## Hardware OpenGL renderer (`gpu_hw`)

Source: `src/core/gpu/hardware/gpu_hw.c`, `src/core/gpu/hardware/gpu_hw_shadergen.c`, `src/core/gpu/hardware/gpu_hw_texture_cache.c`. **Work in progress**. Known gaps remain in fill/copy paths, transparency edge cases, and resolution-scale corner cases; the software path is the shipping default for that reason. Opt in with `--gpu-renderer opengl`. If GL device or backend init fails at startup, the frontend logs `[gpu] HW OpenGL setup failed; falling back to software.` and continues on the SW renderer.

- OpenGL 3.3+ via EGL on X11 (`src/util/graphics/opengl_context_egl_xlib.c`).
- Internal resolution scale 1×..16× (`--gpu-resolution-scale`).
- Texture cache with PNG-based replacement-pack support (`gpu_hw_texture_cache.c` plus `tc_image_loader.c` → vendored `dep/stb/stb_image.h`). Replacement directory walked lazily; entries decoded on demand and cached.
- PGXP subpixel correction.
- All four transparency modes via shader-driven blend states; dual-source blending used when supported.
- Per-batch dithering and true-color toggles.
- Eager batch-pipeline compilation at startup; unused filter rows are skipped.
- VRAM readback via PBO with synchronous `glReadPixels` fallback.

`make gl-smoke` builds an EGL/X11 context check. A successful run completes 120
buffer swaps. Setup or swap failures return a nonzero status. Automated tests
check success, failed swaps and cleanup with controlled window and context
operations; running the standalone binary requires an X11 display.

Automated rasterizer checks cover specific operations, fixed-point arithmetic and selected SIMD comparisons. Complete command coverage, display timing and gameplay rendering remain unverified. See [testing limits](../development/testing.md).
