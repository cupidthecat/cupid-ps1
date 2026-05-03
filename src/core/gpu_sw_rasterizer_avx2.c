/*
 * AVX2-targeted SW rasterizer TU.  Compiled with -mavx2 -mavx -msse4.1
 * -msse2 -mfma per Makefile per-TU rule.
 *
 * Implemented: fill_vram, copy_vram, write_vram with 256-bit
 * vector inner loops on the wide regular path (no row wrap, no clipping
 * mid-row).  Tail-pixel residue and any wrapping/masking case fall
 * through to the scalar path (or a 1-pixel scalar tail) so behavior is
 * bit-identical to gpu_sw_rasterizer_scalar_*.
 *
 * Triangle / line / rectangle still forward to the scalar implementation
 *; those need GSVector-equivalent per-pixel ops (gradient, CLUT, blend)
 * and stay scalar this round, matching upstream duckstation's `#if 0`
 * AVX2 GSVector path.
 */

#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_helpers.h"
#include "core/gpu_types.h"

#include <immintrin.h>
#include <string.h>

extern u16 g_vram[VRAM_HEIGHT * VRAM_WIDTH];

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

/* AVX2 entry-point prototypes; silence -Wmissing-prototypes; the dispatcher
 * in gpu_sw_rasterizer.c references these via its own forward decls. */
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

/* Triangle/line/rectangle: scalar (GSVector-C body deferred). */
void gpu_sw_rasterizer_avx2_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* cmd)
{
  gpu_sw_rasterizer_scalar_draw_rectangle(cmd);
}
void gpu_sw_rasterizer_avx2_draw_line(const gpu_backend_draw_cmd_t* cmd,
                                      const gpu_backend_line_vertex_t* p0,
                                      const gpu_backend_line_vertex_t* p1)
{
  gpu_sw_rasterizer_scalar_draw_line(cmd, p0, p1);
}
void gpu_sw_rasterizer_avx2_draw_triangle(const gpu_backend_draw_cmd_t* cmd,
                                          const gpu_backend_polygon_vertex_t* v0,
                                          const gpu_backend_polygon_vertex_t* v1,
                                          const gpu_backend_polygon_vertex_t* v2)
{
  gpu_sw_rasterizer_scalar_draw_triangle(cmd, v0, v1, v2);
}

/* ---------------------------------------------------------------------------
 *  fill_vram
 *
 *  Wide regular path (the common case): rect fits the row without wrapping
 *  AND we are not interlaced.  Vector inner loop stores 16 u16 per
 *  _mm256_storeu_si256.  Tail (<16) and wrapping/interlaced cases use the
 *  same scalar logic as gpu_sw_rasterizer_scalar_fill_vram.
 * --------------------------------------------------------------------------- */
void gpu_sw_rasterizer_avx2_fill_vram(u32 x, u32 y, u32 width, u32 height, u32 color,
                                      bool interlaced, u8 active_line_lsb)
{
  const u16 color16 = vram_rgba8888_to_rgba5551(color);

  if ((x + width) <= VRAM_WIDTH && !interlaced) {
    const __m256i v = _mm256_set1_epi16((short)color16);
    const u32 vec_count = width >> 4;          /* 16 u16 per vector */
    const u32 vec_pixels = vec_count << 4;     /* covered by vector */
    for (u32 yoffs = 0; yoffs < height; yoffs++) {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* dst = &g_vram[row * VRAM_WIDTH + x];
      for (u32 i = 0; i < vec_count; i++) {
        _mm256_storeu_si256((__m256i*)(dst + i * 16u), v);
      }
      for (u32 i = vec_pixels; i < width; i++)
        dst[i] = color16;
    }
  } else if (interlaced) {
    /* Interlaced + non-wrapping fast path can still vectorize per row. */
    const u32 active_field = active_line_lsb;
    const bool nowrap = ((x + width) <= VRAM_WIDTH);
    if (nowrap) {
      const __m256i v = _mm256_set1_epi16((short)color16);
      const u32 vec_count = width >> 4;
      const u32 vec_pixels = vec_count << 4;
      for (u32 yoffs = 0; yoffs < height; yoffs++) {
        const u32 row = (y + yoffs) % VRAM_HEIGHT;
        if ((row & 1u) == active_field) continue;
        u16* dst = &g_vram[row * VRAM_WIDTH + x];
        for (u32 i = 0; i < vec_count; i++)
          _mm256_storeu_si256((__m256i*)(dst + i * 16u), v);
        for (u32 i = vec_pixels; i < width; i++)
          dst[i] = color16;
      }
    } else {
      for (u32 yoffs = 0; yoffs < height; yoffs++) {
        const u32 row = (y + yoffs) % VRAM_HEIGHT;
        if ((row & 1u) == active_field) continue;
        u16* row_ptr = &g_vram[row * VRAM_WIDTH];
        for (u32 xoffs = 0; xoffs < width; xoffs++) {
          const u32 col = (x + xoffs) % VRAM_WIDTH;
          row_ptr[col] = color16;
        }
      }
    }
  } else {
    /* Wrapping, non-interlaced: scalar (matches scalar_fill_vram exactly). */
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

/* ---------------------------------------------------------------------------
 *  write_vram
 *
 *  Fast path mirrors the scalar fast path: contiguous rect, no masking,
 *  no row/col wrap.  Vector inner loop is just streaming load/store of 16
 *  u16s.  All other cases (mask check / mask set / wrapping) drop to the
 *  scalar per-pixel loop; those still need pixel-level mask logic.
 * --------------------------------------------------------------------------- */
void gpu_sw_rasterizer_avx2_write_vram(u32 x, u32 y, u32 width, u32 height, const u16* data,
                                       bool set_mask, bool check_mask)
{
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !set_mask && !check_mask) {
    const u16* src = data;
    u16* dst = &g_vram[y * VRAM_WIDTH + x];
    const u32 vec_count = width >> 4;
    const u32 vec_pixels = vec_count << 4;
    const u32 tail_bytes = (width - vec_pixels) * (u32)sizeof(u16);
    for (u32 yoffs = 0; yoffs < height; yoffs++) {
      for (u32 i = 0; i < vec_count; i++) {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(src + i * 16u));
        _mm256_storeu_si256((__m256i*)(dst + i * 16u), v);
      }
      if (tail_bytes)
        memcpy(dst + vec_pixels, src + vec_pixels, tail_bytes);
      src += width;
      dst += VRAM_WIDTH;
    }
    return;
  }

  /* Mask / wrap path: bit-identical to scalar. */
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

/* ---------------------------------------------------------------------------
 *  copy_vram
 *
 *  Vectorize only the no-wrap, no-mask, non-overlapping forward case:
 *    - both src + dst rects fit within their rows (no horizontal wrap),
 *    - no set_mask / check_mask,
 *    - rows don't wrap vertically,
 *    - and src+dst rects don't overlap-on-same-row in a way that requires
 *      reverse-direction copy.
 *  All other cases drop to the scalar implementation, which already handles
 *  wrap, overlap reverse-copy, and per-pixel mask logic.
 * --------------------------------------------------------------------------- */
void gpu_sw_rasterizer_avx2_copy_vram(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                      u32 width, u32 height, bool set_mask, bool check_mask)
{
  /* Hand off any wrap/oversize cases to scalar; the recursive split logic
   * there ends up dispatching back through gpu_sw_rasterizer_copy_vram so
   * the non-wrap subrects will land here. */
  const bool nowrap = (src_x + width) <= VRAM_WIDTH
                   && (dst_x + width) <= VRAM_WIDTH
                   && (src_y + height) <= VRAM_HEIGHT
                   && (dst_y + height) <= VRAM_HEIGHT;
  if (!nowrap || set_mask || check_mask) {
    gpu_sw_rasterizer_scalar_copy_vram(src_x, src_y, dst_x, dst_y, width, height, set_mask, check_mask);
    return;
  }

  /* Determine reverse-vs-forward direction (matches scalar). */
  const bool reverse = (src_x < dst_x);

  if (reverse) {
    /* Reverse copy: walk rows from right-edge towards left-edge.  For
     * memmove-equivalent semantics on overlapping same-row src/dst we keep
     * this scalar-per-pixel path; unaligned overlapping AVX2 stores would
     * stomp on values we still need to read. */
    for (u32 row = 0; row < height; row++) {
      const u16* src_row = &g_vram[(src_y + row) * VRAM_WIDTH];
      u16* dst_row = &g_vram[(dst_y + row) * VRAM_WIDTH];
      for (s32 col = (s32)width - 1; col >= 0; col--) {
        dst_row[dst_x + (u32)col] = src_row[src_x + (u32)col];
      }
    }
    return;
  }

  /* Forward copy.  If src and dst rows are different we can vectorize freely.
   * If the same row, dst_x <= src_x guarantees forward AVX2 stores read the
   * not-yet-overwritten src values (we always advance left-to-right). */
  const u32 vec_count = width >> 4;
  const u32 vec_pixels = vec_count << 4;
  const u32 tail_bytes = (width - vec_pixels) * (u32)sizeof(u16);
  for (u32 row = 0; row < height; row++) {
    const u16* src_row = &g_vram[(src_y + row) * VRAM_WIDTH + src_x];
    u16* dst_row = &g_vram[(dst_y + row) * VRAM_WIDTH + dst_x];
    for (u32 i = 0; i < vec_count; i++) {
      const __m256i v = _mm256_loadu_si256((const __m256i*)(src_row + i * 16u));
      _mm256_storeu_si256((__m256i*)(dst_row + i * 16u), v);
    }
    if (tail_bytes)
      memmove(dst_row + vec_pixels, src_row + vec_pixels, tail_bytes);
  }
}
