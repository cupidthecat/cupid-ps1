/*
 * functions selected through three function-pointer tables (rect/line/tri)
 * indexed by [shading][modulation][transparency].  We collapse all template
 * params into runtime branches inside one rasterizer per primitive (perf
 * loss accepted; correctness first).  Tables remain so call sites read the
 * same way (`get_draw_triangle()(...)`), but they all point at the same
 * function and the modulation/shading/transparency flags come from cmd.
 *
 * Runtime ISA select: gpu_sw_rasterizer_dispatch_init() picks scalar vs AVX2
 * (vtable populated in gpu_sw_rasterizer.c).  AVX2 entry points live in
 * gpu_sw_rasterizer_avx2.c; today they forward to scalar; the per-TU split
 * is the placeholder for a future GSVector-C SIMD body.
 */

#ifndef CUPID_CORE_GPU_SW_RASTERIZER_H
#define CUPID_CORE_GPU_SW_RASTERIZER_H

#include "core/gpu_backend.h"
#include "core/gpu_types.h"
#include "common/types.h"

/* TextureModulationMode.  Used by rectangles to pick the shade path. */
typedef enum {
  GPU_SW_TEX_MODULATION_DISABLED      = 0, /* untextured */
  GPU_SW_TEX_MODULATION_NO_MODULATION = 1, /* "raw texture" */
  GPU_SW_TEX_MODULATION_8BIT          = 2, /* modulate with 8-bit color */
  GPU_SW_TEX_MODULATION_5BIT          = 3, /* modulate with 5-bit color */
} gpu_sw_tex_modulation_t;

 /* Dither LUT: ((color + DITHER_MATRIX[y][x]) >> 3) clamped to [0,31].
 * 512 = next-pow2 covering max input 31*255>>4 = 494. */
#define GPU_SW_DITHER_LUT_SIZE 512

/* The 4x4 Bayer-style dither matrix in gpu_types.h is the source of truth;
 * we precompute the LUT at startup. */
extern u8 gpu_sw_dither_lut[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE][GPU_SW_DITHER_LUT_SIZE];

 /* Drawing area shared with the rasterizer.  Set by gpu_backend's
 * drawing_area_changed callback. */
extern gpu_drawing_area_t gpu_sw_drawing_area;

/* VRAM + CLUT live as globals (1 MB + 256 entries). */
extern u16 g_vram[VRAM_HEIGHT * VRAM_WIDTH];
extern u16 g_gpu_clut[GPU_CLUT_SIZE];

/* Initialise dither LUT + dispatch vtable.  Idempotent. */
void gpu_sw_rasterizer_init(void);

/* Pick scalar vs AVX2 vtable.  Honors CUPID_GPU_SW_ISA env, then
 * g_settings.gpu_sw_use_isa, otherwise auto-detects.  Idempotent.  Called
 * automatically from gpu_sw_rasterizer_init(). */
void gpu_sw_rasterizer_dispatch_init(void);

 /*
 * GenerateChromaSmoothingFragmentShader).  Operates in-place on a
 * rectangular RGBA8 region of `fb` (stride `fb_stride` u32s/row), keeping
 * each pixel's luma and replacing its chroma with a bilerp of four 2x2
 * neighbour averages.  Both SW and HW renderers call this after the
 * 24-bit row decode fills their display_buffer.  Skip in interlaced mode
 * (per-field smoothing not implemented). */
void gpu_apply_chroma_smoothing_24bit(u32* fb, u32 fb_stride,
                                      u32 fmv_x, u32 fmv_y,
                                      u32 fmv_w, u32 fmv_h);

/* Cache the active CLUT into g_gpu_clut. */
void gpu_sw_rasterizer_update_clut(gpu_texture_palette_reg_t reg, bool clut_is_8bit);

/* Primitive entry points; runtime-dispatched on cmd flags. */
void gpu_sw_rasterizer_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* cmd);
void gpu_sw_rasterizer_draw_line     (const gpu_backend_draw_cmd_t* cmd,
                                      const gpu_backend_line_vertex_t* p0,
                                      const gpu_backend_line_vertex_t* p1);
void gpu_sw_rasterizer_draw_triangle (const gpu_backend_draw_cmd_t* cmd,
                                      const gpu_backend_polygon_vertex_t* v0,
                                      const gpu_backend_polygon_vertex_t* v1,
                                      const gpu_backend_polygon_vertex_t* v2);

/* VRAM blits. */
void gpu_sw_rasterizer_fill_vram (u32 x, u32 y, u32 width, u32 height, u32 color,
                                  bool interlaced, u8 active_line_lsb);
void gpu_sw_rasterizer_write_vram(u32 x, u32 y, u32 width, u32 height, const u16* data,
                                  bool set_mask, bool check_mask);
void gpu_sw_rasterizer_copy_vram (u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                  u32 width, u32 height, bool set_mask, bool check_mask);

#endif /* CUPID_CORE_GPU_SW_RASTERIZER_H */
