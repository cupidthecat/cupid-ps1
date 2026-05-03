/*
 * Scalar software rasterizer.  Shading / modulation / transparency are
 * runtime branches inside ShadePixel + the per-primitive routines.
 * Single TU; no ISA-specialized variants.
 *
 *  - ShadePixel unconditionally fetches the texture pixel and applies
 *    modulation, then conditionally blends/dithers on cmd flags.  The
 *    runtime branch on `texture_enable` etc. predicts perfectly within
 *    a primitive.
 *  - blargg's 15bpp transparency math (sum / carry / borrow tricks)
 *    handles all four modes by manipulating the RGB5A1 bit pattern as a
 *    single u32.
 *  - Dither matrix is 4x4 (Bayer-like); the LUT gates output to [0,31]
 *    after adding the matrix offset to the 5-bit channel value.
 */

#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_helpers.h"
#include "core/settings.h"
#include "common/assert.h"
#include "common/log.h"
#include "util/cpu_features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>           /* strcasecmp */

LOG_CHANNEL(GPU_SW);

/* VRAM is 1MB (1024 * 512 * 2).  Aligned to 4K so future GPU readback paths
 * can mmap it without reallocation.  */
__attribute__((aligned(4096))) u16 g_vram[VRAM_HEIGHT * VRAM_WIDTH];
u16 g_gpu_clut[GPU_CLUT_SIZE];

u8  gpu_sw_dither_lut[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE][GPU_SW_DITHER_LUT_SIZE];
gpu_drawing_area_t gpu_sw_drawing_area;

/* Vtable populated by gpu_sw_rasterizer_dispatch_init() at startup, then
 * called via the trampolines at the end of this file.  AVX2 entry points are
 * defined in gpu_sw_rasterizer_avx2.c (separate TU compiled with -mavx2).
 * Today they forward to the scalar implementations; the per-TU split is in
 * place so a future GSVector-C port can fill in the SIMD bodies without
 * touching any caller. */
typedef struct {
  void (*draw_rectangle)(const gpu_backend_draw_rectangle_cmd_t*);
  void (*draw_line)(const gpu_backend_draw_cmd_t*,
                    const gpu_backend_line_vertex_t*,
                    const gpu_backend_line_vertex_t*);
  void (*draw_triangle)(const gpu_backend_draw_cmd_t*,
                        const gpu_backend_polygon_vertex_t*,
                        const gpu_backend_polygon_vertex_t*,
                        const gpu_backend_polygon_vertex_t*);
  void (*fill_vram)(u32, u32, u32, u32, u32, bool, u8);
  void (*write_vram)(u32, u32, u32, u32, const u16*, bool, bool);
  void (*copy_vram)(u32, u32, u32, u32, u32, u32, bool, bool);
  const char* name;
} gpu_sw_rasterizer_vtable_t;

/* Forward decls: scalar (defined below; non-static so other TUs link),
 * SIMD (SSE4.1, gpu_sw_rasterizer_simd.c), AVX2 (gpu_sw_rasterizer_avx2.c). */
void gpu_sw_rasterizer_scalar_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_scalar_draw_line     (const gpu_backend_draw_cmd_t*,
                                             const gpu_backend_line_vertex_t*,
                                             const gpu_backend_line_vertex_t*);
void gpu_sw_rasterizer_scalar_draw_triangle (const gpu_backend_draw_cmd_t*,
                                             const gpu_backend_polygon_vertex_t*,
                                             const gpu_backend_polygon_vertex_t*,
                                             const gpu_backend_polygon_vertex_t*);
void gpu_sw_rasterizer_scalar_fill_vram (u32, u32, u32, u32, u32, bool, u8);
void gpu_sw_rasterizer_scalar_write_vram(u32, u32, u32, u32, const u16*, bool, bool);
void gpu_sw_rasterizer_scalar_copy_vram (u32, u32, u32, u32, u32, u32, bool, bool);

void gpu_sw_rasterizer_simd_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_simd_draw_line     (const gpu_backend_draw_cmd_t*,
                                           const gpu_backend_line_vertex_t*,
                                           const gpu_backend_line_vertex_t*);
void gpu_sw_rasterizer_simd_draw_triangle (const gpu_backend_draw_cmd_t*,
                                           const gpu_backend_polygon_vertex_t*,
                                           const gpu_backend_polygon_vertex_t*,
                                           const gpu_backend_polygon_vertex_t*);
void gpu_sw_rasterizer_simd_fill_vram (u32, u32, u32, u32, u32, bool, u8);
void gpu_sw_rasterizer_simd_write_vram(u32, u32, u32, u32, const u16*, bool, bool);
void gpu_sw_rasterizer_simd_copy_vram (u32, u32, u32, u32, u32, u32, bool, bool);

void gpu_sw_rasterizer_avx2_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_avx2_draw_line     (const gpu_backend_draw_cmd_t*,
                                           const gpu_backend_line_vertex_t*,
                                           const gpu_backend_line_vertex_t*);
void gpu_sw_rasterizer_avx2_draw_triangle (const gpu_backend_draw_cmd_t*,
                                           const gpu_backend_polygon_vertex_t*,
                                           const gpu_backend_polygon_vertex_t*,
                                           const gpu_backend_polygon_vertex_t*);
void gpu_sw_rasterizer_avx2_fill_vram (u32, u32, u32, u32, u32, bool, u8);
void gpu_sw_rasterizer_avx2_write_vram(u32, u32, u32, u32, const u16*, bool, bool);
void gpu_sw_rasterizer_avx2_copy_vram (u32, u32, u32, u32, u32, u32, bool, bool);

static const gpu_sw_rasterizer_vtable_t s_scalar_vt = {
  gpu_sw_rasterizer_scalar_draw_rectangle,
  gpu_sw_rasterizer_scalar_draw_line,
  gpu_sw_rasterizer_scalar_draw_triangle,
  gpu_sw_rasterizer_scalar_fill_vram,
  gpu_sw_rasterizer_scalar_write_vram,
  gpu_sw_rasterizer_scalar_copy_vram,
  "SCALAR"
};
/* SIMD vtable: SSE4.1 4-pixel-per-vec rasterizer. VRAM blits forward
 * to scalar (scalar already has a non-wrap fast path; full vectorization
 * lives in the AVX2 vtable). */
static const gpu_sw_rasterizer_vtable_t s_simd_vt = {
  gpu_sw_rasterizer_simd_draw_rectangle,
  gpu_sw_rasterizer_simd_draw_line,
  gpu_sw_rasterizer_simd_draw_triangle,
  gpu_sw_rasterizer_simd_fill_vram,
  gpu_sw_rasterizer_simd_write_vram,
  gpu_sw_rasterizer_simd_copy_vram,
  "SIMD"
};
/* AVX2 vtable: SIMD rasterizer (no AVX2 widening yet; deferred) +
 * AVX2 256-bit VRAM blits from gpu_sw_rasterizer_avx2.c. */
static const gpu_sw_rasterizer_vtable_t s_avx2_vt = {
  gpu_sw_rasterizer_simd_draw_rectangle,
  gpu_sw_rasterizer_simd_draw_line,
  gpu_sw_rasterizer_simd_draw_triangle,
  gpu_sw_rasterizer_avx2_fill_vram,
  gpu_sw_rasterizer_avx2_write_vram,
  gpu_sw_rasterizer_avx2_copy_vram,
  "AVX2"
};
static const gpu_sw_rasterizer_vtable_t* g_vt = &s_scalar_vt;

void gpu_sw_rasterizer_init(void)
{
  for (u32 i = 0; i < DITHER_MATRIX_SIZE; i++) {
    for (u32 j = 0; j < DITHER_MATRIX_SIZE; j++) {
      for (u32 value = 0; value < GPU_SW_DITHER_LUT_SIZE; value++) {
        const s32 d = ((s32)value + DITHER_MATRIX[i][j]) >> 3;
        gpu_sw_dither_lut[i][j][value] = (u8)((d < 0) ? 0 : ((d > 31) ? 31 : d));
      }
    }
  }
  gpu_sw_rasterizer_dispatch_init();
}

void gpu_sw_rasterizer_dispatch_init(void)
{
  /* Idempotent: re-runs are cheap and just re-pick.  Honors:
   *   1. CUPID_GPU_SW_ISA env var ("AVX2", "SIMD", "SCALAR"); override.
   *   2. g_settings.gpu_sw_use_isa  (same values, "" = auto).
   *   3. Otherwise auto: AVX2 > SIMD > SCALAR per host capability.
   * AVX2 vtable inherits SIMD per-pixel ops + adds 256-bit VRAM blits.
   * SIMD vtable provides 4-pixel-wide SSE4.1 rasterizer. */
  cpu_features_init();

  const char* override = getenv("CUPID_GPU_SW_ISA");
  const char* setting  = g_settings.gpu_sw_use_isa;
  const char* req      = (override && *override) ? override
                       : (setting && *setting)   ? setting
                       : "";

  const gpu_sw_rasterizer_vtable_t* picked;
  if (strcasecmp(req, "SCALAR") == 0) {
    picked = &s_scalar_vt;
  } else if (strcasecmp(req, "SIMD") == 0) {
    picked = cpu_has_sse41() ? &s_simd_vt : &s_scalar_vt;
  } else if (strcasecmp(req, "AVX2") == 0) {
    picked = cpu_has_avx2() ? &s_avx2_vt
           : cpu_has_sse41() ? &s_simd_vt : &s_scalar_vt;
  } else {
    picked = cpu_has_avx2() ? &s_avx2_vt
           : cpu_has_sse41() ? &s_simd_vt : &s_scalar_vt;
  }

  if (picked != g_vt) {
    g_vt = picked;
    INFO_LOG("[gpu_sw] selected impl=%s (host=%s, requested='%s')",
             g_vt->name, cpu_features_isa_name(), req);
  } else {
    INFO_LOG("[gpu_sw] selected impl=%s (host=%s)",
             g_vt->name, cpu_features_isa_name());
  }
}

void gpu_sw_rasterizer_update_clut(gpu_texture_palette_reg_t reg, bool clut_is_8bit)
{
  const u32 base_y = gpu_texture_palette_reg_get_y_base(reg);
  const u32 start_x = gpu_texture_palette_reg_get_x_base(reg);
  const u16* src_row = &g_vram[base_y * VRAM_WIDTH];

  if (!clut_is_8bit) {
    /* 4-bit CLUT: 16 entries, can't wrap (16 <= 1024 minus any start_x within row). */
    memcpy(g_gpu_clut, &src_row[start_x], sizeof(u16) * 16);
  } else {
    /* 8-bit CLUT: 256 entries, may wrap around the row. */
    if ((start_x + 256) > VRAM_WIDTH) {
      const u32 end = VRAM_WIDTH - start_x;
      const u32 start = 256u - end;
      memcpy(g_gpu_clut, &src_row[start_x], sizeof(u16) * end);
      memcpy(g_gpu_clut + end, src_row, sizeof(u16) * start);
    } else {
      memcpy(g_gpu_clut, &src_row[start_x], sizeof(u16) * 256);
    }
  }
}

ALWAYS_INLINE static u16 get_pixel(u32 x, u32 y)
{
  return g_vram[VRAM_WIDTH * y + x];
}
ALWAYS_INLINE static void set_pixel(u32 x, u32 y, u16 v)
{
  g_vram[VRAM_WIDTH * y + x] = v;
}

static void shade_pixel(const gpu_backend_draw_cmd_t* cmd,
                        gpu_sw_tex_modulation_t modulation_mode,
                        bool transparency_enable,
                        u32 x, u32 y,
                        u8 color_r, u8 color_g, u8 color_b,
                        u8 texcoord_x, u8 texcoord_y)
{
  const bool texture_enable = (modulation_mode != GPU_SW_TEX_MODULATION_DISABLED);
  u16 color;

  if (texture_enable) {
    /* Texture window: tx = (tx & and_x) | or_x. */
    texcoord_x = (texcoord_x & cmd->window.and_x) | cmd->window.or_x;
    texcoord_y = (texcoord_y & cmd->window.and_y) | cmd->window.or_y;

    u16 texture_color;
    const u32 page_x = gpu_draw_mode_reg_get_texture_page_base_x(cmd->draw_mode);
    const u32 page_y = gpu_draw_mode_reg_get_texture_page_base_y(cmd->draw_mode);

    switch (gpu_draw_mode_reg_texture_mode(cmd->draw_mode)) {
      case GPU_TEXTURE_MODE_PALETTE_4BIT: {
        const u16 palette_value = get_pixel((page_x + (texcoord_x / 4u)) % VRAM_WIDTH,
                                            (page_y + texcoord_y) % VRAM_HEIGHT);
        const u32 palette_index = (palette_value >> ((texcoord_x % 4u) * 4u)) & 0x0Fu;
        texture_color = g_gpu_clut[palette_index];
        break;
      }
      case GPU_TEXTURE_MODE_PALETTE_8BIT: {
        const u16 palette_value = get_pixel((page_x + (texcoord_x / 2u)) % VRAM_WIDTH,
                                            (page_y + texcoord_y) % VRAM_HEIGHT);
        const u32 palette_index = (palette_value >> ((texcoord_x % 2u) * 8u)) & 0xFFu;
        texture_color = g_gpu_clut[palette_index];
        break;
      }
      default:
        texture_color = get_pixel((page_x + texcoord_x) % VRAM_WIDTH,
                                  (page_y + texcoord_y) % VRAM_HEIGHT);
        break;
    }

    if (texture_color == 0)
      return; /* fully-transparent texel: drop the write entirely */

    if (modulation_mode == GPU_SW_TEX_MODULATION_NO_MODULATION) {
      color = texture_color;
    } else {
      const bool dithering_enable = cmd->dither_enable;
      const u32 dither_y = dithering_enable ? (y & 3u) : 2u;
      const u32 dither_x = dithering_enable ? (x & 3u) : 3u;

      if (modulation_mode == GPU_SW_TEX_MODULATION_8BIT) {
        color =
          ((u16)gpu_sw_dither_lut[dither_y][dither_x][((u16)(texture_color & 0x1Fu) * (u16)color_r) >> 4] << 0) |
          ((u16)gpu_sw_dither_lut[dither_y][dither_x][((u16)((texture_color >> 5) & 0x1Fu) * (u16)color_g) >> 4] << 5) |
          ((u16)gpu_sw_dither_lut[dither_y][dither_x][((u16)((texture_color >> 10) & 0x1Fu) * (u16)color_b) >> 4] << 10) | 
          (texture_color & 0x8000u);
      } else {
        /* 5-bit modulation: input color is downsampled to 5-bit first. */
        color =
          ((u16)gpu_sw_dither_lut[dither_y][dither_x][((u16)(texture_color & 0x1Fu) * (u16)(color_r >> 3)) >> 1] << 0) |
          ((u16)gpu_sw_dither_lut[dither_y][dither_x][((u16)((texture_color >> 5) & 0x1Fu) * (u16)(color_g >> 3)) >> 1] << 5) |
          ((u16)gpu_sw_dither_lut[dither_y][dither_x][((u16)((texture_color >> 10) & 0x1Fu) * (u16)(color_b >> 3)) >> 1] << 10) | 
          (texture_color & 0x8000u);
      }
    }
  } else {
    const bool dithering_enable = cmd->dither_enable;
    const u32 dither_y = dithering_enable ? (y & 3u) : 2u;
    const u32 dither_x = dithering_enable ? (x & 3u) : 3u;

    /* Untextured semitransparent polys don't physically set bit 15 in the
     * texture, but they're still treated as transparent; that's what the
     * 0x8000 sentinel below encodes. */
    color = ((u16)gpu_sw_dither_lut[dither_y][dither_x][color_r] << 0) |
            ((u16)gpu_sw_dither_lut[dither_y][dither_x][color_g] << 5) |
            ((u16)gpu_sw_dither_lut[dither_y][dither_x][color_b] << 10) | 
            (transparency_enable ? 0x8000u : 0u);
  }

  const u16 bg_color = get_pixel(x, y);
  if (transparency_enable) {
    if ((color & 0x8000u) || !texture_enable) {
      /* blargg's efficient 15bpp blend math.  Each operation works on the
       * full RGB555 word at once, using carry/borrow patterns to keep
       * channels independent. */
      u32 bg_bits = bg_color;
      u32 fg_bits = color;
      switch (gpu_draw_mode_reg_transparency_mode(cmd->draw_mode)) {
        case GPU_TRANSPARENCY_MODE_HALF_BG_PLUS_HALF_FG:
          bg_bits |= 0x8000u;
          color = (u16)(((fg_bits + bg_bits) - ((fg_bits ^ bg_bits) & 0x0421u)) >> 1);
          break;
        case GPU_TRANSPARENCY_MODE_BG_PLUS_FG: {
          bg_bits &= ~0x8000u;
          const u32 sum = fg_bits + bg_bits;
          const u32 carry = (sum - ((fg_bits ^ bg_bits) & 0x8421u)) & 0x8420u;
          color = (u16)((sum - carry) | (carry - (carry >> 5)));
          break;
        }
        case GPU_TRANSPARENCY_MODE_BG_MINUS_FG: {
          bg_bits |= 0x8000u;
          fg_bits &= ~0x8000u;
          const u32 diff = bg_bits - fg_bits + 0x108420u;
          const u32 borrow = (diff - ((bg_bits ^ fg_bits) & 0x108420u)) & 0x108420u;
          color = (u16)((diff - borrow) & (borrow - (borrow >> 5)));
          break;
        }
        case GPU_TRANSPARENCY_MODE_BG_PLUS_QUARTER_FG: {
          bg_bits &= ~0x8000u;
          fg_bits = ((fg_bits >> 2) & 0x1CE7u) | 0x8000u;
          const u32 sum = fg_bits + bg_bits;
          const u32 carry = (sum - ((fg_bits ^ bg_bits) & 0x8421u)) & 0x8420u;
          color = (u16)((sum - carry) | (carry - (carry >> 5)));
          break;
        }
        default:
          break;
      }
      if (!texture_enable)
        color &= ~0x8000u;
    }
  }

  const u16 mask_and = gpu_backend_draw_cmd_mask_and(cmd);
  if ((bg_color & mask_and) != 0)
    return;

  set_pixel(x, y, color | gpu_backend_draw_cmd_mask_or(cmd));
}

void gpu_sw_rasterizer_scalar_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* cmd)
{
  const gpu_backend_draw_cmd_t* base = &cmd->base;
  const s32 origin_x = cmd->x;
  const s32 origin_y = cmd->y;
  const u8  r = (u8)cmd->color;
  const u8  g = (u8)(cmd->color >> 8);
  const u8  b = (u8)(cmd->color >> 16);
  const u8  origin_tx = (u8)cmd->texcoord;
  const u8  origin_ty = (u8)(cmd->texcoord >> 8);

  const gpu_sw_tex_modulation_t mod =
     base->texture_enable
      ? (base->raw_texture_enable ? GPU_SW_TEX_MODULATION_NO_MODULATION : GPU_SW_TEX_MODULATION_8BIT) 
      : GPU_SW_TEX_MODULATION_DISABLED;

  for (u32 offset_y = 0; offset_y < cmd->height; offset_y++) {
    const s32 y = origin_y + (s32)offset_y;
    if (y < (s32)gpu_sw_drawing_area.top || y > (s32)gpu_sw_drawing_area.bottom)
      continue;
    if (base->interlaced_rendering &&
        base->active_line_lsb == (((u32)y) & 1u))
      continue;

    const u32 draw_y = (u32)y & VRAM_HEIGHT_MASK;
    const u8 ty = (u8)(origin_ty + offset_y);

    for (u32 offset_x = 0; offset_x < cmd->width; offset_x++) {
      const s32 x = origin_x + (s32)offset_x;
      if (x < (s32)gpu_sw_drawing_area.left || x > (s32)gpu_sw_drawing_area.right)
        continue;
      const u8 tx = (u8)(origin_tx + offset_x);
      shade_pixel(base, mod, base->transparency_enable, (u32)x, draw_y, r, g, b, tx, ty);
    }
  }
}

static s32 abs_s32(s32 v) { return v < 0 ? -v : v; }

void gpu_sw_rasterizer_scalar_draw_line(const gpu_backend_draw_cmd_t* cmd,
                                        const gpu_backend_line_vertex_t* p0,
                                        const gpu_backend_line_vertex_t* p1)
{
  const bool shading_enable      = cmd->shading_enable;
  const bool transparency_enable = cmd->transparency_enable;

  /* Fixed-point math: 32 bits for xy, 12 for rgb. */
  enum { XY_SHIFT = 32, RGB_SHIFT = 12 };

  const s32 i_dx = abs_s32(p1->x - p0->x);
  const s32 i_dy = abs_s32(p1->y - p0->y);
  const s32 k = (i_dx > i_dy) ? i_dx : i_dy;
  if (i_dx >= MAX_PRIMITIVE_WIDTH || i_dy >= MAX_PRIMITIVE_HEIGHT)
    return;

  /* Rasterize from leftmost vertex. */
  if (p0->x >= p1->x && k > 0) {
    const gpu_backend_line_vertex_t* tmp = p0;
    p0 = p1;
    p1 = tmp;
  }

  s64 dxdk = 0, dydk = 0;
  s32 drdk = 0, dgdk = 0, dbdk = 0;
  if (k != 0) {
    s64 ddx = (s64)(p1->x - p0->x);
    s64 ddy = (s64)(p1->y - p0->y);
    /* div_xy with rounding: ((delta << SHIFT) +/- (k-1)) / k */
    dxdk = ((ddx << XY_SHIFT) - ((ddx < 0) ? (k - 1) : 0) + ((ddx > 0) ? (k - 1) : 0)) / k;
    dydk = ((ddy << XY_SHIFT) - ((ddy < 0) ? (k - 1) : 0) + ((ddy > 0) ? (k - 1) : 0)) / k;
    if (shading_enable) {
      drdk = (((s32)p1->r - (s32)p0->r) << RGB_SHIFT) / k;
      dgdk = (((s32)p1->g - (s32)p0->g) << RGB_SHIFT) / k;
      dbdk = (((s32)p1->b - (s32)p0->b) << RGB_SHIFT) / k;
    }
  }

  s64 curx = ((s64)p0->x << XY_SHIFT) | (1LL << (XY_SHIFT - 1));
  curx -= 1024;
  s64 cury = ((s64)p0->y << XY_SHIFT) | (1LL << (XY_SHIFT - 1));
  cury -= (dydk < 0) ? 1024 : 0;

  s32 curr = ((s32)p0->r << RGB_SHIFT) | (1 << (RGB_SHIFT - 1));
  s32 curg = ((s32)p0->g << RGB_SHIFT) | (1 << (RGB_SHIFT - 1));
  s32 curb = ((s32)p0->b << RGB_SHIFT) | (1 << (RGB_SHIFT - 1));

  for (s32 i = 0; i <= k; i++) {
    const s32 x = truncate_gpu_vertex_position((s32)(curx >> XY_SHIFT));
    const s32 y = truncate_gpu_vertex_position((s32)(cury >> XY_SHIFT));

    if ((!cmd->interlaced_rendering ||
          cmd->active_line_lsb != (((u32)y) & 1u)) &&
        x >= (s32)gpu_sw_drawing_area.left && x <= (s32)gpu_sw_drawing_area.right && 
        y >= (s32)gpu_sw_drawing_area.top  && y <= (s32)gpu_sw_drawing_area.bottom) {
      const u8 r = shading_enable ? (u8)(curr >> RGB_SHIFT) : p0->r;
      const u8 g = shading_enable ? (u8)(curg >> RGB_SHIFT) : p0->g;
      const u8 b = shading_enable ? (u8)(curb >> RGB_SHIFT) : p0->b;
      shade_pixel(cmd, GPU_SW_TEX_MODULATION_DISABLED, transparency_enable,
                  (u32)x, (u32)y & VRAM_HEIGHT_MASK, r, g, b, 0, 0);
    }

    curx += dxdk;
    cury += dydk;
    if (shading_enable) {
      curr += drdk;
      curg += dgdk;
      curb += dbdk;
    }
  }
}

#define ATTRIB_SHIFT       12
#define ATTRIB_POST_SHIFT  12

typedef struct {
  u32 dudx, dvdx;
  u32 dudy, dvdy;
} uv_steps_t;

typedef struct {
  u32 u, v;
} uv_stepper_t;

ALWAYS_INLINE static u8 uv_get_u(uv_stepper_t s)
{ return (u8)(s.u >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }
ALWAYS_INLINE static u8 uv_get_v(uv_stepper_t s)
{ return (u8)(s.v >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }

ALWAYS_INLINE static void uv_init(uv_stepper_t* s, u32 u0, u32 v0)
{
  s->u = ((u0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
  s->v = ((v0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
}
ALWAYS_INLINE static void uv_step_x_n(uv_stepper_t* s, const uv_steps_t* st, s32 n)
{
  s->u += (u32)((s32)st->dudx * n);
  s->v += (u32)((s32)st->dvdx * n);
}
ALWAYS_INLINE static void uv_step_x(uv_stepper_t* s, const uv_steps_t* st)
{
  s->u += st->dudx;
  s->v += st->dvdx;
}
ALWAYS_INLINE static void uv_step_y_n(uv_stepper_t* s, const uv_steps_t* st, s32 n)
{
  s->u += (u32)((s32)st->dudy * n);
  s->v += (u32)((s32)st->dvdy * n);
}
ALWAYS_INLINE static void uv_step_y(uv_stepper_t* s, const uv_steps_t* st, bool upside_down)
{
  s->u = upside_down ? (s->u - st->dudy) : (s->u + st->dudy);
  s->v = upside_down ? (s->v - st->dvdy) : (s->v + st->dvdy);
}

typedef struct {
  u32 drdx, dgdx, dbdx;
  u32 drdy, dgdy, dbdy;
} rgb_steps_t;

typedef struct {
  u32 r, g, b;
} rgb_stepper_t;

ALWAYS_INLINE static u8 rgb_get_r(rgb_stepper_t s) { return (u8)(s.r >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }
ALWAYS_INLINE static u8 rgb_get_g(rgb_stepper_t s) { return (u8)(s.g >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }
ALWAYS_INLINE static u8 rgb_get_b(rgb_stepper_t s) { return (u8)(s.b >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }

ALWAYS_INLINE static void rgb_init(rgb_stepper_t* s, u32 r0, u32 g0, u32 b0)
{
  s->r = ((r0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
  s->g = ((g0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
  s->b = ((b0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
}
ALWAYS_INLINE static void rgb_step_x_n(rgb_stepper_t* s, const rgb_steps_t* st, s32 n)
{
  s->r += (u32)((s32)st->drdx * n);
  s->g += (u32)((s32)st->dgdx * n);
  s->b += (u32)((s32)st->dbdx * n);
}
ALWAYS_INLINE static void rgb_step_x(rgb_stepper_t* s, const rgb_steps_t* st)
{
  s->r += st->drdx;
  s->g += st->dgdx;
  s->b += st->dbdx;
}
ALWAYS_INLINE static void rgb_step_y_n(rgb_stepper_t* s, const rgb_steps_t* st, s32 n)
{
  s->r += (u32)((s32)st->drdy * n);
  s->g += (u32)((s32)st->dgdy * n);
  s->b += (u32)((s32)st->dbdy * n);
}
ALWAYS_INLINE static void rgb_step_y(rgb_stepper_t* s, const rgb_steps_t* st, bool upside_down)
{
  s->r = upside_down ? (s->r - st->drdy) : (s->r + st->drdy);
  s->g = upside_down ? (s->g - st->dgdy) : (s->g + st->dgdy);
  s->b = upside_down ? (s->b - st->dbdy) : (s->b + st->dbdy);
}

typedef struct {
  u64 start_x[2];
  u64 step_x[2];
  s32 start_y;
  s32 end_y;
  bool fill_upside_down;
} triangle_part_t;

static void draw_span(const gpu_backend_draw_cmd_t* cmd,
                       gpu_sw_tex_modulation_t mod, bool transparency_enable, bool shading_enable,
                      s32 y, s32 x_start, s32 x_bound,
                      uv_stepper_t uv, const uv_steps_t* uvstep,
                      rgb_stepper_t rgb, const rgb_steps_t* rgbstep) 
{
  const bool texture_enable = (mod != GPU_SW_TEX_MODULATION_DISABLED);
  s32 width = x_bound - x_start;
  s32 current_x = truncate_gpu_vertex_position(x_start);

  if (current_x < (s32)gpu_sw_drawing_area.left) {
    const s32 delta = (s32)gpu_sw_drawing_area.left - current_x;
    x_start += delta;
    current_x += delta;
    width -= delta;
  }
  if ((current_x + width) > ((s32)gpu_sw_drawing_area.right + 1))
    width = (s32)gpu_sw_drawing_area.right + 1 - current_x;
  if (width <= 0)
    return;

  if (texture_enable)  uv_step_x_n(&uv, uvstep, x_start);
  if (shading_enable)  rgb_step_x_n(&rgb, rgbstep, x_start);

  do {
    shade_pixel(cmd, mod, transparency_enable, (u32)current_x, (u32)y,
                rgb_get_r(rgb), rgb_get_g(rgb), rgb_get_b(rgb),
                uv_get_u(uv), uv_get_v(uv));
    current_x++;
    if (texture_enable) uv_step_x(&uv, uvstep);
    if (shading_enable) rgb_step_x(&rgb, rgbstep);
  } while (--width > 0);
}

static void draw_triangle_part(const gpu_backend_draw_cmd_t* cmd,
                               gpu_sw_tex_modulation_t mod, bool transparency_enable, bool shading_enable,
                               const triangle_part_t* tp,
                               const uv_stepper_t* uv0, const uv_steps_t* uvstep,
                               const rgb_stepper_t* rgb0, const rgb_steps_t* rgbstep)
{
  const bool texture_enable = (mod != GPU_SW_TEX_MODULATION_DISABLED);
  const u64 left_x_step = tp->step_x[0];
  const u64 right_x_step = tp->step_x[1];
  const s32 end_y = tp->end_y;
  u64 left_x = tp->start_x[0];
  u64 right_x = tp->start_x[1];
  s32 current_y = tp->start_y;

  if (tp->fill_upside_down) {
    if (current_y <= end_y) return;

    uv_stepper_t luv = *uv0;
    if (texture_enable) uv_step_y_n(&luv, uvstep, current_y);
    rgb_stepper_t lrgb = *rgb0;
    if (shading_enable) rgb_step_y_n(&lrgb, rgbstep, current_y);

    do {
      current_y--;
      left_x  -= left_x_step;
      right_x -= right_x_step;

      const s32 y = truncate_gpu_vertex_position(current_y);
      if (y < (s32)gpu_sw_drawing_area.top) break;

      if (texture_enable) uv_step_y(&luv, uvstep, true);
      if (shading_enable) rgb_step_y(&lrgb, rgbstep, true);

      if (y > (s32)gpu_sw_drawing_area.bottom ||
          (cmd->interlaced_rendering &&
           cmd->active_line_lsb == ((u32)current_y & 1u))) 
        continue;

      const s32 lx = (s32)((u64)left_x  >> 32);
      const s32 rx = (s32)((u64)right_x >> 32);
      draw_span(cmd, mod, transparency_enable, shading_enable,
                y & VRAM_HEIGHT_MASK, lx, rx,
                luv, uvstep, lrgb, rgbstep);
    } while (current_y > end_y);
  } else {
    if (current_y >= end_y) return;

    uv_stepper_t luv = *uv0;
    if (texture_enable) uv_step_y_n(&luv, uvstep, current_y);
    rgb_stepper_t lrgb = *rgb0;
    if (shading_enable) rgb_step_y_n(&lrgb, rgbstep, current_y);

    do {
      const s32 y = truncate_gpu_vertex_position(current_y);
      if (y > (s32)gpu_sw_drawing_area.bottom) break;
      if (y >= (s32)gpu_sw_drawing_area.top &&
          (!cmd->interlaced_rendering ||
           cmd->active_line_lsb != ((u32)current_y & 1u))) {
        const s32 lx = (s32)((u64)left_x  >> 32);
        const s32 rx = (s32)((u64)right_x >> 32);
        draw_span(cmd, mod, transparency_enable, shading_enable,
                  y & VRAM_HEIGHT_MASK, lx, rx,
                  luv, uvstep, lrgb, rgbstep);
      }
      current_y++;
      left_x  += left_x_step;
      right_x += right_x_step;
      if (texture_enable) uv_step_y(&luv, uvstep, false);
      if (shading_enable) rgb_step_y(&lrgb, rgbstep, false);
    } while (current_y < end_y);
  }
}

void gpu_sw_rasterizer_scalar_draw_triangle(const gpu_backend_draw_cmd_t* cmd,
                                            const gpu_backend_polygon_vertex_t* v0,
                                            const gpu_backend_polygon_vertex_t* v1,
                                            const gpu_backend_polygon_vertex_t* v2)
{
  const bool shading_enable      = cmd->shading_enable;
  const bool transparency_enable = cmd->transparency_enable;
  const gpu_sw_tex_modulation_t mod =
     cmd->texture_enable
      ? (cmd->raw_texture_enable ? GPU_SW_TEX_MODULATION_NO_MODULATION : GPU_SW_TEX_MODULATION_8BIT) 
      : GPU_SW_TEX_MODULATION_DISABLED;
  const bool texture_enable = (mod != GPU_SW_TEX_MODULATION_DISABLED);

  /* Sort: v0 = top, v1 = mid, v2 = bottom; tl is the index (0,1,2) of the
   * top-left vertex which is used for attribute base. */
  u32 tl = 0;
  if (v1->x <= v0->x)      tl = (v2->x <= v1->x) ? 4 : 2;
  else if (v2->x < v0->x)  tl = 4;
  else                     tl = 1;

  if (v2->y < v1->y) {
    const gpu_backend_polygon_vertex_t* t = v2; v2 = v1; v1 = t;
    tl = ((tl >> 1) & 0x2) | ((tl << 1) & 0x4) | (tl & 0x1);
  }
  if (v1->y < v0->y) {
    const gpu_backend_polygon_vertex_t* t = v1; v1 = v0; v0 = t;
    tl = ((tl >> 1) & 0x1) | ((tl << 1) & 0x2) | (tl & 0x4);
  }
  if (v2->y < v1->y) {
    const gpu_backend_polygon_vertex_t* t = v2; v2 = v1; v1 = t;
    tl = ((tl >> 1) & 0x2) | ((tl << 1) & 0x4) | (tl & 0x1);
  }

  const gpu_backend_polygon_vertex_t* vertices[3] = { v0, v1, v2 };
  tl >>= 1;

  if (v0->y == v2->y) return; /* degenerate */

  /* Fixed-point edge stepping at 32-bit precision. */
  const s64 base_coord = ((s64)v0->x << 32) + ((1LL << 32) - (1 << 11));
  const s32 dx_v2v0 = v2->x - v0->x;
  const s32 dy_v2v0 = v2->y - v0->y;
  const s64 base_step = (((s64)dx_v2v0 << 32) +
                         ((dx_v2v0 < 0) ? -(s64)(dy_v2v0 - 1) : ((dx_v2v0 > 0) ? (s64)(dy_v2v0 - 1) : 0))) / dy_v2v0;

  s64 bound_coord_us = 0;
  if (v1->y != v0->y) {
    const s32 dx = v1->x - v0->x, dy = v1->y - v0->y;
    bound_coord_us = (((s64)dx << 32) + ((dx < 0) ? -(s64)(dy - 1) : ((dx > 0) ? (s64)(dy - 1) : 0))) / dy;
  }
  s64 bound_coord_ls = 0;
  if (v2->y != v1->y) {
    const s32 dx = v2->x - v1->x, dy = v2->y - v1->y;
    bound_coord_ls = (((s64)dx << 32) + ((dx < 0) ? -(s64)(dy - 1) : ((dx > 0) ? (s64)(dy - 1) : 0))) / dy;
  }

  const u32 vo = (tl != 0) ? 1u : 0u;
  const u32 vp = (tl == 2) ? 3u : 0u;
  const bool right_facing = (v1->y == v0->y) ? (v1->x > v0->x) : (bound_coord_us > base_step);
  const u32 rfi = right_facing ? 1u : 0u;
  const u32 ofi = right_facing ? 0u : 1u;

  triangle_part_t triparts[2] = {0};
  triangle_part_t* tpo = &triparts[vo];
  triangle_part_t* tpp = &triparts[vo ^ 1];

  tpo->start_y = vertices[0 ^ vo]->y;
  tpo->end_y   = vertices[1 ^ vo]->y;
  tpp->start_y = vertices[1 ^ vp]->y;
  tpp->end_y   = vertices[2 ^ vp]->y;
  tpo->start_x[rfi] = ((s64)vertices[0 ^ vo]->x << 32) + ((1LL << 32) - (1 << 11));
  tpo->step_x[rfi]  = (u64)bound_coord_us;
  tpo->start_x[ofi] = (u64)(base_coord + ((s64)(vertices[vo]->y - vertices[0]->y) * base_step));
  tpo->step_x[ofi]  = (u64)base_step;
  tpo->fill_upside_down = (vo != 0);

  tpp->start_x[rfi] = ((s64)vertices[1 ^ vp]->x << 32) + ((1LL << 32) - (1 << 11));
  tpp->step_x[rfi]  = (u64)bound_coord_ls;
  tpp->start_x[ofi] = (u64)(base_coord + ((s64)(vertices[1 ^ vp]->y - vertices[0]->y) * base_step));
  tpp->step_x[ofi]  = (u64)base_step;
  tpp->fill_upside_down = (vp != 0);

  /* Determinant for attribute steps. */
  const s32 det = ((v1->x - v0->x) * (v2->y - v1->y)) - ((v2->x - v1->x) * (v1->y - v0->y));
  if (det == 0) return;

#define ATTRIB_DET_2(ax, ay, bx, by) (((s32)(v1->ax - v0->ax) * (s32)(v2->by - v1->by)) - \
                                       ((s32)(v2->ax - v1->ax) * (s32)(v1->by - v0->by)))
#define ATTRIB_STEP_2(ax, ay, bx, by) ((u32)(ATTRIB_DET_2(ax, ay, bx, by) * (1 << ATTRIB_SHIFT) / det) << ATTRIB_POST_SHIFT)

  uv_steps_t  uvstep  = {0};
  rgb_steps_t rgbstep = {0};
  if (texture_enable) {
    uvstep.dudx = ATTRIB_STEP_2(u, y, u, y);
    uvstep.dvdx = ATTRIB_STEP_2(v, y, v, y);
    uvstep.dudy = ATTRIB_STEP_2(x, u, x, u);
    uvstep.dvdy = ATTRIB_STEP_2(x, v, x, v);
  }
  if (shading_enable) {
    rgbstep.drdx = ATTRIB_STEP_2(r, y, r, y);
    rgbstep.dgdx = ATTRIB_STEP_2(g, y, g, y);
    rgbstep.dbdx = ATTRIB_STEP_2(b, y, b, y);
    rgbstep.drdy = ATTRIB_STEP_2(x, r, x, r);
    rgbstep.dgdy = ATTRIB_STEP_2(x, g, x, g);
    rgbstep.dbdy = ATTRIB_STEP_2(x, b, x, b);
  }

#undef ATTRIB_STEP_2
#undef ATTRIB_DET_2

  uv_stepper_t uv = {0};
  rgb_stepper_t rgb = {0};
  const gpu_backend_polygon_vertex_t* tlv = vertices[tl];

  if (texture_enable) {
    uv_init(&uv, tlv->u, tlv->v);
    uv_step_x_n(&uv, &uvstep, -tlv->x);
    uv_step_y_n(&uv, &uvstep, -tlv->y);
  }
  rgb_init(&rgb, tlv->r, tlv->g, tlv->b);
  if (shading_enable) {
    rgb_step_x_n(&rgb, &rgbstep, -tlv->x);
    rgb_step_y_n(&rgb, &rgbstep, -tlv->y);
  }

  for (u32 i = 0; i < 2; i++) {
    draw_triangle_part(cmd, mod, transparency_enable, shading_enable,
                       &triparts[i], &uv, &uvstep, &rgb, &rgbstep);
  }
}

void gpu_sw_rasterizer_scalar_fill_vram(u32 x, u32 y, u32 width, u32 height, u32 color,
                                        bool interlaced, u8 active_line_lsb)
{
  const u16 color16 = vram_rgba8888_to_rgba5551(color);

  if ((x + width) <= VRAM_WIDTH && !interlaced) {
    for (u32 yoffs = 0; yoffs < height; yoffs++) {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* dst = &g_vram[row * VRAM_WIDTH + x];
      for (u32 i = 0; i < width; i++) dst[i] = color16;
    }
  } else if (interlaced) {
    /* Hardware quirk: interlaced fills break on the first two lines when
     * the offset matches the displayed field.  Skip those rows. */
    const u32 active_field = active_line_lsb;
    for (u32 yoffs = 0; yoffs < height; yoffs++) {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      if ((row & 1u) == active_field) continue;
      u16* row_ptr = &g_vram[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++) {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  } else {
    for (u32 yoffs = 0; yoffs < height; yoffs++) {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* row_ptr = &g_vram[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++) {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
}

void gpu_sw_rasterizer_scalar_write_vram(u32 x, u32 y, u32 width, u32 height, const u16* data,
                                         bool set_mask, bool check_mask)
{
  /* Fast path: contiguous, no masking. */
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !set_mask && !check_mask) {
    const u16* src = data;
    u16* dst = &g_vram[y * VRAM_WIDTH + x];
    for (u32 yoffs = 0; yoffs < height; yoffs++) {
      memcpy(dst, src, width * sizeof(u16));
      src += width;
      dst += VRAM_WIDTH;
    }
    return;
  }

  const u16 mask_and = check_mask ? (u16)0x8000u : (u16)0x0000u;
  const u16 mask_or  = set_mask   ? (u16)0x8000u : (u16)0x0000u;
  const u16* src = data;

  for (u32 row = 0; row < height; row++) {
    u16* dst_row = &g_vram[((y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
    for (u32 col = 0; col < width; col++) {
      u16* p = &dst_row[(x + col) % VRAM_WIDTH];
      if ((*p & mask_and) == 0)
        *p = *src | mask_or;
      src++;
    }
  }
}

void gpu_sw_rasterizer_scalar_copy_vram(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                        u32 width, u32 height, bool set_mask, bool check_mask)
{
  /* Break up oversized copies that would wrap horizontally. */
  if ((src_x + width) > VRAM_WIDTH || (dst_x + width) > VRAM_WIDTH) {
    u32 remaining_rows = height;
    u32 cur_src_y = src_y, cur_dst_y = dst_y;
    while (remaining_rows > 0) {
      const u32 rows_to_copy = (remaining_rows < (VRAM_HEIGHT - cur_src_y) ? remaining_rows : (VRAM_HEIGHT - cur_src_y));
      const u32 rows = (rows_to_copy < (VRAM_HEIGHT - cur_dst_y) ? rows_to_copy : (VRAM_HEIGHT - cur_dst_y));
      u32 remaining_cols = width;
      u32 cur_src_x = src_x, cur_dst_x = dst_x;
      while (remaining_cols > 0) {
        const u32 cols_to_copy = (remaining_cols < (VRAM_WIDTH - cur_src_x) ? remaining_cols : (VRAM_WIDTH - cur_src_x));
        const u32 cols = (cols_to_copy < (VRAM_WIDTH - cur_dst_x) ? cols_to_copy : (VRAM_WIDTH - cur_dst_x));
        gpu_sw_rasterizer_copy_vram(cur_src_x, cur_src_y, cur_dst_x, cur_dst_y, cols, rows, set_mask, check_mask);
        cur_src_x = (cur_src_x + cols) % VRAM_WIDTH;
        cur_dst_x = (cur_dst_x + cols) % VRAM_WIDTH;
        remaining_cols -= cols;
      }
      cur_src_y = (cur_src_y + rows) % VRAM_HEIGHT;
      cur_dst_y = (cur_dst_y + rows) % VRAM_HEIGHT;
      remaining_rows -= rows;
    }
    return;
  }

  const u16 mask_and = check_mask ? (u16)0x8000u : (u16)0x0000u;
  const u16 mask_or  = set_mask   ? (u16)0x8000u : (u16)0x0000u;

  /* Copy reverse when src_x < dst_x: hardware behavior verified on console. */
  if (src_x < dst_x ||
      ((src_x + width - 1) % VRAM_WIDTH) < ((dst_x + width - 1) % VRAM_WIDTH)) {
    for (u32 row = 0; row < height; row++) {
      const u16* src_row = &g_vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row = &g_vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      for (s32 col = (s32)width - 1; col >= 0; col--) {
        const u16 src_pix = src_row[(src_x + (u32)col) % VRAM_WIDTH];
        u16* dst_pix = &dst_row[(dst_x + (u32)col) % VRAM_WIDTH];
        *dst_pix = ((*dst_pix & mask_and) == 0) ? (src_pix | mask_or) : *dst_pix;
      }
    }
  } else {
    for (u32 row = 0; row < height; row++) {
      const u16* src_row = &g_vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row = &g_vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      for (u32 col = 0; col < width; col++) {
        const u16 src_pix = src_row[(src_x + col) % VRAM_WIDTH];
        u16* dst_pix = &dst_row[(dst_x + col) % VRAM_WIDTH];
        *dst_pix = ((*dst_pix & mask_and) == 0) ? (src_pix | mask_or) : *dst_pix;
      }
    }
  }
}

void gpu_apply_chroma_smoothing_24bit(u32* fb, u32 fb_stride,
                                       u32 fmv_x, u32 fmv_y,
                                      u32 fmv_w, u32 fmv_h) 
{
  static u32 scratch[GPU_MAX_DISPLAY_WIDTH * GPU_MAX_DISPLAY_HEIGHT];
  if ((u64)fmv_w * (u64)fmv_h > (u64)(GPU_MAX_DISPLAY_WIDTH * GPU_MAX_DISPLAY_HEIGHT) ||
      fmv_w == 0u || fmv_h == 0u)
    return;

  for (u32 r = 0; r < fmv_h; r++)
    memcpy(&scratch[r * fmv_w], &fb[(fmv_y + r) * fb_stride + fmv_x], fmv_w * sizeof(u32));

  #define AVG2X2_RGB(out_r, out_g, out_b, px0, py0) do {           \
    const u32 _x0 = (px0), _y0 = (py0);                            \
    const u32 _x1 = ((_x0 + 1u) < fmv_w) ? (_x0 + 1u) : _x0;       \
    const u32 _y1 = ((_y0 + 1u) < fmv_h) ? (_y0 + 1u) : _y0;       \
    const u32 _a = scratch[_y0 * fmv_w + _x0];                     \
    const u32 _b = scratch[_y0 * fmv_w + _x1];                     \
    const u32 _c = scratch[_y1 * fmv_w + _x0];                     \
    const u32 _d = scratch[_y1 * fmv_w + _x1];                     \
    out_r = (float)(((_a >>  0) & 0xFFu) + ((_b >>  0) & 0xFFu)    \
                  + ((_c >>  0) & 0xFFu) + ((_d >>  0) & 0xFFu))   \
           \
            * (1.0f / (4.0f * 255.0f));                            \
            \
    out_g = (float)(((_a >>  8) & 0xFFu) + ((_b >>  8) & 0xFFu)    \
                  + ((_c >>  8) & 0xFFu) + ((_d >>  8) & 0xFFu))   \
           \
            * (1.0f / (4.0f * 255.0f));                            \
            \
    out_b = (float)(((_a >> 16) & 0xFFu) + ((_b >> 16) & 0xFFu)    \
                  + ((_c >> 16) & 0xFFu) + ((_d >> 16) & 0xFFu))   \
           \
            * (1.0f / (4.0f * 255.0f));                            \
            \
  } while (0)

  for (u32 y = 0; y < fmv_h; y++) {
    u32* dst_row = &fb[(fmv_y + y) * fb_stride + fmv_x];
    for (u32 x = 0; x < fmv_w; x++) {
      const u32 p = scratch[y * fmv_w + x];
      const float pr = (float)((p >>  0) & 0xFFu) * (1.0f / 255.0f);
      const float pg = (float)((p >>  8) & 0xFFu) * (1.0f / 255.0f);
      const float pb = (float)((p >> 16) & 0xFFu) * (1.0f / 255.0f);
      const float py = 0.299f * pr + 0.587f * pg + 0.114f * pb;

      const s32 base_x = (s32)x - 1;
      const s32 base_y = (s32)y - 1;
      const u32 lx = (base_x < 0) ? 0u : (u32)(base_x & ~1);
      const u32 ly = (base_y < 0) ? 0u : (u32)(base_y & ~1);
      const u32 hx = ((lx + 2u) < fmv_w) ? (lx + 2u) : (fmv_w - 1u);
      const u32 hy = ((ly + 2u) < fmv_h) ? (ly + 2u) : (fmv_h - 1u);
      const float cx = ((float)(base_x & 1)) * 0.5f + 0.25f;
      const float cy = ((float)(base_y & 1)) * 0.5f + 0.25f;

      float p00r, p00g, p00b; AVG2X2_RGB(p00r, p00g, p00b, lx, ly);
      float p01r, p01g, p01b; AVG2X2_RGB(p01r, p01g, p01b, lx, hy);
      float p10r, p10g, p10b; AVG2X2_RGB(p10r, p10g, p10b, hx, ly);
      float p11r, p11g, p11b; AVG2X2_RGB(p11r, p11g, p11b, hx, hy);

      const float a_r = p00r + (p10r - p00r) * cx;
      const float a_g = p00g + (p10g - p00g) * cx;
      const float a_b = p00b + (p10b - p00b) * cx;
      const float b_r = p01r + (p11r - p01r) * cx;
      const float b_g = p01g + (p11g - p01g) * cx;
      const float b_b = p01b + (p11b - p01b) * cx;
      const float sr  = a_r + (b_r - a_r) * cy;
      const float sg  = a_g + (b_g - a_g) * cy;
      const float sb  = a_b + (b_b - a_b) * cy;

      const float su = -0.14713f * sr - 0.28886f * sg + 0.436f   * sb;
      const float sv =  0.615f   * sr - 0.51499f * sg - 0.10001f * sb;

      float r = py + 1.13983f * sv;
      float g = py - 0.39465f * su - 0.58060f * sv;
      float b = py + 2.03211f * su;
      r = (r < 0.0f) ? 0.0f : (r > 1.0f ? 1.0f : r);
      g = (g < 0.0f) ? 0.0f : (g > 1.0f ? 1.0f : g);
      b = (b < 0.0f) ? 0.0f : (b > 1.0f ? 1.0f : b);

      const u32 R = (u32)(r * 255.0f + 0.5f);
      const u32 G = (u32)(g * 255.0f + 0.5f);
      const u32 B = (u32)(b * 255.0f + 0.5f);
      dst_row[x] = R | (G << 8) | (B << 16) | 0xFF000000u;
    }
  }

  #undef AVG2X2_RGB
}

/* Public entry-point trampolines.  Callers (gpu_sw.c, gpu_hw.c, gpu_commands)
 * keep the original symbol names; the per-ISA body lives behind g_vt. */
void gpu_sw_rasterizer_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* cmd)
{
  g_vt->draw_rectangle(cmd);
}
void gpu_sw_rasterizer_draw_line(const gpu_backend_draw_cmd_t* cmd,
                                 const gpu_backend_line_vertex_t* p0,
                                 const gpu_backend_line_vertex_t* p1)
{
  g_vt->draw_line(cmd, p0, p1);
}
void gpu_sw_rasterizer_draw_triangle(const gpu_backend_draw_cmd_t* cmd,
                                     const gpu_backend_polygon_vertex_t* v0,
                                     const gpu_backend_polygon_vertex_t* v1,
                                     const gpu_backend_polygon_vertex_t* v2)
{
  g_vt->draw_triangle(cmd, v0, v1, v2);
}
void gpu_sw_rasterizer_fill_vram(u32 x, u32 y, u32 width, u32 height, u32 color,
                                 bool interlaced, u8 active_line_lsb)
{
  g_vt->fill_vram(x, y, width, height, color, interlaced, active_line_lsb);
}
void gpu_sw_rasterizer_write_vram(u32 x, u32 y, u32 width, u32 height, const u16* data,
                                  bool set_mask, bool check_mask)
{
  g_vt->write_vram(x, y, width, height, data, set_mask, check_mask);
}
void gpu_sw_rasterizer_copy_vram(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                 u32 width, u32 height, bool set_mask, bool check_mask)
{
  g_vt->copy_vram(src_x, src_y, dst_x, dst_y, width, height, set_mask, check_mask);
}
