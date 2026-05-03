/*
 * SSE4.1 SIMD body of the SW rasterizer.
 *
 * Mirrors duckstation gpu_sw_rasterizer.inl SIMD path with PIXELS_PER_VEC=4
 * (GSVector4i, 4xs32 lanes). The .inl's #ifdef GSVECTOR_HAS_256 path
 * (GSVector8i / 8 lanes) is what the AVX2 namespace uses upstream, gated
 * off in upstream production (`#if 0` per gpu_sw_rasterizer.cpp:111).
 *
 * Compiled with -msse4.1 per Makefile per-TU rule. Runtime dispatch picks
 * this vtable when cpu_has_sse41() is true (which is every modern x86_64).
 *
 * Output is bit-identical to the scalar path; enforced by frame-dump
 * comparison.
 */

#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_helpers.h"
#include "core/gpu_types.h"
#include "core/gpu_backend.h"
#include "common/gsvector.h"
#include "common/intrin.h"

#include <stdalign.h>
#include <stdbool.h>
#include <string.h>

/* External symbols from gpu_sw_rasterizer.c (scalar TU). Reused: VRAM, CLUT,
 * dither LUT, drawing area state. */
/* gpu_sw_dither_lut declared in gpu_sw_rasterizer.h; SIMD path doesn't use
 * it (vector dither matrix is built per-row in VECTOR_DITHER_MATRIX). */
extern gpu_drawing_area_t gpu_sw_drawing_area;

/* Forward decls for scalar fallbacks. SIMD vtable's draw_line forwards
 * to scalar (matches upstream's "TODO: Vectorize line draw"); fill/write/
 * copy_vram forward to scalar; the AVX2 .c file has its own vectorized
 * versions for the AVX2 vtable, but the SIMD vtable doesn't add a 128-bit
 * vector path for those (would be marginal vs the existing scalar
 * `(x+w) <= VRAM_WIDTH` fast path). */
void gpu_sw_rasterizer_scalar_draw_line     (const gpu_backend_draw_cmd_t*,
                                             const gpu_backend_line_vertex_t*,
                                             const gpu_backend_line_vertex_t*);
void gpu_sw_rasterizer_scalar_fill_vram (u32, u32, u32, u32, u32, bool, u8);
void gpu_sw_rasterizer_scalar_write_vram(u32, u32, u32, u32, const u16*, bool, bool);
void gpu_sw_rasterizer_scalar_copy_vram (u32, u32, u32, u32, u32, u32, bool, bool);

/* Public SIMD vtable entries. */
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

/* ---------------------------------------------------------------------------
 *  PIXELS_PER_VEC + lane-count constants for GSVector4i path.
 * --------------------------------------------------------------------------- */

enum { PIXELS_PER_VEC = 4 };

/* Per-lane literals: SPAN_OFFSET = (0,1,2,3); SPAN_WIDTH = (1,2,3,4). */
static inline GSVector4i span_offset_vec(void) { return gsv4i_setr32(0, 1, 2, 3); }
static inline GSVector4i span_width_vec (void) { return gsv4i_setr32(1, 2, 3, 4); }
static inline GSVector4i ppv_vec        (void) { return gsv4i_set32(PIXELS_PER_VEC); }

/* VECTOR_DITHER_MATRIX[row][16] = D(r,0)D(r,0)D(r,1)D(r,1)D(r,2)D(r,2)D(r,3)D(r,3)
 *                                 repeated, so a 128-bit load at offset (col%4)*2
 *                                 yields 8 s16 dither offsets aligned to RG/BA.
 * Built at TU init; saves a runtime spread when dither is enabled. */
alignas(16) static s16 VECTOR_DITHER_MATRIX[4][16];

__attribute__((constructor))
static void init_vector_dither_matrix(void)
{
  for (int r = 0; r < 4; r++) {
    s16 v0 = (s16)DITHER_MATRIX[r][0];
    s16 v1 = (s16)DITHER_MATRIX[r][1];
    s16 v2 = (s16)DITHER_MATRIX[r][2];
    s16 v3 = (s16)DITHER_MATRIX[r][3];
    s16 row[16] = { v0,v0,v1,v1,v2,v2,v3,v3, v0,v0,v1,v1,v2,v2,v3,v3 };
    memcpy(VECTOR_DITHER_MATRIX[r], row, sizeof(row));
  }
}

/* ---------------------------------------------------------------------------
 *  GatherVector / GatherCLUTVector
 *
 *  Per-lane VRAM/CLUT loads. SSE4.1 has no scatter/gather, so extract each
 *  index, scalar-load the u16, insert16 it back. This is what upstream's
 *  GSVector4i path does too. see .inl:375.
 * --------------------------------------------------------------------------- */

static inline GSVector4i gather_vector(GSVector4i coord_x, GSVector4i coord_y)
{
  /* offset = y * VRAM_WIDTH + x  (VRAM_WIDTH = 1024, so y << 10) */
  const GSVector4i offsets = gsv4i_add32(gsv4i_sll32(coord_y, 10), coord_x);

  GSVector4i pixels = gsv4i_zext32((u32)g_vram[(u32)gsv4i_extract32(offsets, 0)]);
  pixels = gsv4i_insert16(pixels, g_vram[(u32)gsv4i_extract32(offsets, 1)], 2);
  pixels = gsv4i_insert16(pixels, g_vram[(u32)gsv4i_extract32(offsets, 2)], 4);
  pixels = gsv4i_insert16(pixels, g_vram[(u32)gsv4i_extract32(offsets, 3)], 6);
  return pixels;
}

/* Without _mm_srlv_epi32 (AVX2-only), SSE4.1 fallback extracts to scalar.
 * Branch on AVX2 presence at compile time keeps both paths warm. */
static inline GSVector4i gather_clut_vector(GSVector4i indices, GSVector4i shifts, u32 mask)
{
#ifdef __AVX2__
  /* When this TU is built with -mavx2 (during AVX2 vtable swap), use SRLV. */
  const GSVector4i offsets = gsv4i_and(gsv4i_srlv32(indices, shifts), gsv4i_set32((s32)mask));
  GSVector4i pixels = gsv4i_zext32((u32)g_gpu_clut[(u32)gsv4i_extract32(offsets, 0)]);
  pixels = gsv4i_insert16(pixels, g_gpu_clut[(u32)gsv4i_extract32(offsets, 1)], 2);
  pixels = gsv4i_insert16(pixels, g_gpu_clut[(u32)gsv4i_extract32(offsets, 2)], 4);
  pixels = gsv4i_insert16(pixels, g_gpu_clut[(u32)gsv4i_extract32(offsets, 3)], 6);
  return pixels;
#else
  alignas(16) s32 ind[4], sh[4];
  gsv4i_store_a(ind, indices);
  gsv4i_store_a(sh,  shifts);
  GSVector4i pixels = gsv4i_zext32((u32)g_gpu_clut[((u32)ind[0] >> sh[0]) & mask]);
  pixels = gsv4i_insert16(pixels, g_gpu_clut[((u32)ind[1] >> sh[1]) & mask], 2);
  pixels = gsv4i_insert16(pixels, g_gpu_clut[((u32)ind[2] >> sh[2]) & mask], 4);
  pixels = gsv4i_insert16(pixels, g_gpu_clut[((u32)ind[3] >> sh[3]) & mask], 6);
  return pixels;
#endif
}

/* ---------------------------------------------------------------------------
 *  LoadVector / StoreVector
 *
 *  Read/write a 4-pixel run of VRAM. Fast path: contiguous 4 u16 = 64-bit
 *  load; widen to u32 lanes. Wrap path: 4 scalar loads + insert16 (matches
 *  .inl:415).
 *
 *  StoreVector packs 4 u32 → 4 u16 via gsv4i_pu32 (unsigned saturation,
 *  which is fine because color lanes never exceed 0xFFFF here; we
 *  already AND'd with 0xFFFF in the blend code).
 * --------------------------------------------------------------------------- */

static inline GSVector4i load_vector(u32 x, u32 y)
{
  if (x <= (VRAM_WIDTH - 4u)) {
    /* loadl is u16x4 in low 64 bits, then widen to u32x4. */
    return gsv4i_u16to32(gsv4i_loadl_u(&g_vram[y * VRAM_WIDTH + x]));
  }
  const u16* line = &g_vram[y * VRAM_WIDTH];
  GSVector4i pixels = gsv4i_zext32((u32)line[x & VRAM_WIDTH_MASK]); x++;
  pixels = gsv4i_insert16(pixels, line[x & VRAM_WIDTH_MASK], 2); x++;
  pixels = gsv4i_insert16(pixels, line[x & VRAM_WIDTH_MASK], 4); x++;
  pixels = gsv4i_insert16(pixels, line[x & VRAM_WIDTH_MASK], 6);
  return pixels;
}

static inline void store_vector(u32 x, u32 y, GSVector4i color)
{
  /* Pack 4 × u32 → 4 × u16 (low half of result). */
  const GSVector4i packed = gsv4i_pu32(color);
  if (x <= (VRAM_WIDTH - 4u)) {
    gsv4i_storel_u(&g_vram[y * VRAM_WIDTH + x], packed);
    return;
  }
  u16* line = &g_vram[y * VRAM_WIDTH];
  line[x & VRAM_WIDTH_MASK] = (u16)gsv4i_extract16(packed, 0); x++;
  line[x & VRAM_WIDTH_MASK] = (u16)gsv4i_extract16(packed, 1); x++;
  line[x & VRAM_WIDTH_MASK] = (u16)gsv4i_extract16(packed, 2); x++;
  line[x & VRAM_WIDTH_MASK] = (u16)gsv4i_extract16(packed, 3);
}

/* ---------------------------------------------------------------------------
 *  RGB5A1 lane packing helpers (.inl:451)
 *
 *  rg = R | R | R | R  (with G in bits 16..)  per lane (32-bit lane = u16-u16)
 *  ba = B | B | B | B  (with A bit in bits 16+)
 *
 *  Used so the per-channel modulation (multiply, dither, sra) can run on
 *  16 lanes of u16 instead of needing 32-bit ops.
 * --------------------------------------------------------------------------- */

static inline void rgb5a1_to_rg_ba(GSVector4i rgb5a1, GSVector4i* rg, GSVector4i* ba)
{
  /* rg lane layout per 32-bit slot:
   *   bits 0..4   = R  (mask 0x1F)
   *   bits 16..20 = G  (rgb5a1 >> 5 & 0x1F, then shifted left to bit 16) */
  GSVector4i rg_v = gsv4i_and(rgb5a1, gsv4i_set32(0x1F));
  rg_v = gsv4i_or(rg_v, gsv4i_sll32(gsv4i_and(rgb5a1, gsv4i_set32(0x3E0)), 11));

  /* ba lane layout per 32-bit slot:
   *   bits 0..4   = B  ((rgb5a1 >> 10) & 0x1F)
   *   bit  16     = A  ((rgb5a1 & 0x8000) << 1) */
  GSVector4i ba_v = gsv4i_and(gsv4i_srl32(rgb5a1, 10), gsv4i_set32(0x1F));
  ba_v = gsv4i_or(ba_v, gsv4i_sll32(gsv4i_and(rgb5a1, gsv4i_set32(0x8000)), 1));

  *rg = rg_v;
  *ba = ba_v;
}

static inline GSVector4i rg_ba_to_rgb5a1(GSVector4i rg, GSVector4i ba)
{
  GSVector4i res = gsv4i_and(rg, gsv4i_set32(0x1F));                                          /* R */
  res = gsv4i_or(res, gsv4i_and(gsv4i_srl32(rg, 11), gsv4i_set32(0x3E0)));                    /* G */
  res = gsv4i_or(res, gsv4i_sll32(gsv4i_and(ba, gsv4i_set32(0x1F)), 10));                     /* B */
  res = gsv4i_or(res, gsv4i_sll32(gsv4i_srl32(ba, 16), 15));                                  /* A bit15 */
  return res;
}

/* ---------------------------------------------------------------------------
 *  PixelVectors: per-cmd preloaded vector constants used inside ShadePixel.
 *  Mirrors .inl:483 PixelVectors.
 * --------------------------------------------------------------------------- */

typedef struct {
  GSVector4i clip_left, clip_right;
  GSVector4i mask_and,  mask_or;
  /* texture-only: zero-init when texture_enable=false. */
  GSVector4i tw_and_x,  tw_or_x;
  GSVector4i tw_and_y,  tw_or_y;
  GSVector4i base_x,    base_y;
} pixel_vectors_t;

static inline void pixel_vectors_init(pixel_vectors_t* pv,
                                       const gpu_backend_draw_cmd_t* cmd, bool texture_enable)
{
  pv->clip_left  = gsv4i_set32((s32)gpu_sw_drawing_area.left);
  pv->clip_right = gsv4i_set32((s32)gpu_sw_drawing_area.right);
  pv->mask_and   = gsv4i_set32((s32)gpu_backend_draw_cmd_mask_and(cmd));
  pv->mask_or    = gsv4i_set32((s32)gpu_backend_draw_cmd_mask_or(cmd));
  if (texture_enable) {
    pv->tw_and_x = gsv4i_set32((s32)cmd->window.and_x);
    pv->tw_or_x  = gsv4i_set32((s32)cmd->window.or_x);
    pv->tw_and_y = gsv4i_set32((s32)cmd->window.and_y);
    pv->tw_or_y  = gsv4i_set32((s32)cmd->window.or_y);
    pv->base_x   = gsv4i_set32((s32)gpu_draw_mode_reg_get_texture_page_base_x(cmd->draw_mode));
    pv->base_y   = gsv4i_set32((s32)gpu_draw_mode_reg_get_texture_page_base_y(cmd->draw_mode));
  } else {
    pv->tw_and_x = pv->tw_or_x = pv->tw_and_y = pv->tw_or_y = gsv4i_zero();
    pv->base_x   = pv->base_y  = gsv4i_zero();
  }
}

/* ---------------------------------------------------------------------------
 *  ShadePixel SIMD: 4 pixels at once, runtime modulation/transparency.
 *
 *  Mirrors .inl:524. Branches on modulation_mode + transparency_enable +
 *  texture_mode + transparency_mode are runtime here (vs C++ template
 *  specialization upstream); GCC reliably hoists the constant-per-primitive
 *  branches out of the inner loop via -O2 + ALWAYS_INLINE on the caller.
 *
 *  preserve_mask = ffffffff per lane that should be UNTOUCHED (out of clip,
 *                  past width, or fully-transparent texture color, or
 *                  background mask-bit set).
 *  dither = 8x s16 lane offsets to add to each channel pre-clamp.
 * --------------------------------------------------------------------------- */

ALWAYS_INLINE static void shade_pixel_simd(const pixel_vectors_t* RESTRICT pv,
                                            gpu_sw_tex_modulation_t modulation_mode,
                                            bool transparency_enable,
                                            gpu_texture_mode_t texture_mode,
                                            gpu_transparency_mode_t transparency_mode,
                                            bool mask_bit_test,
                                            u32 start_x, u32 y,
                                            GSVector4i vertex_color_rg,
                                            GSVector4i vertex_color_ba,
                                            GSVector4i texcoord_x,
                                            GSVector4i texcoord_y,
                                            GSVector4i preserve_mask,
                                            GSVector4i dither)
{
  const bool texture_enable = (modulation_mode != GPU_SW_TEX_MODULATION_DISABLED);

  const GSVector4i coord_mask_x = gsv4i_set32(VRAM_WIDTH_MASK);
  const GSVector4i coord_mask_y = gsv4i_set32(VRAM_HEIGHT_MASK);

  GSVector4i color;

  if (texture_enable) {
    /* Texture window. */
    texcoord_x = gsv4i_or(gsv4i_and(texcoord_x, pv->tw_and_x), pv->tw_or_x);
    texcoord_y = gsv4i_or(gsv4i_and(texcoord_y, pv->tw_and_y), pv->tw_or_y);
    texcoord_y = gsv4i_and(gsv4i_add32(pv->base_y, texcoord_y), coord_mask_y);

    GSVector4i texture_color;
    switch (texture_mode) {
      case GPU_TEXTURE_MODE_PALETTE_4BIT: {
        GSVector4i load_tx = gsv4i_srl32(texcoord_x, 2);
        load_tx = gsv4i_and(gsv4i_add32(pv->base_x, load_tx), coord_mask_x);
        const GSVector4i shift = gsv4i_sll32(gsv4i_and(texcoord_x, gsv4i_set32(3)), 2);
        const GSVector4i pal   = gather_vector(load_tx, texcoord_y);
        texture_color = gather_clut_vector(pal, shift, 0x0Fu);
        break;
      }
      case GPU_TEXTURE_MODE_PALETTE_8BIT: {
        GSVector4i load_tx = gsv4i_srl32(texcoord_x, 1);
        load_tx = gsv4i_and(gsv4i_add32(pv->base_x, load_tx), coord_mask_x);
        const GSVector4i shift = gsv4i_sll32(gsv4i_and(texcoord_x, gsv4i_set32(1)), 3);
        const GSVector4i pal   = gather_vector(load_tx, texcoord_y);
        texture_color = gather_clut_vector(pal, shift, 0xFFu);
        break;
      }
      default: {
        texcoord_x = gsv4i_and(gsv4i_add32(pv->base_x, texcoord_x), coord_mask_x);
        texture_color = gather_vector(texcoord_x, texcoord_y);
        break;
      }
    }

    /* Early-out if all 4 texels are 0 (fully-transparent). */
    const GSVector4i tex_transparent_mask = gsv4i_eq32(texture_color, gsv4i_zero());
    if (gsv4i_alltrue(tex_transparent_mask))
      return;
    preserve_mask = gsv4i_or(preserve_mask, tex_transparent_mask);

    if (modulation_mode == GPU_SW_TEX_MODULATION_NO_MODULATION) {
      color = texture_color;
    } else {
      GSVector4i trg, tba;
      rgb5a1_to_rg_ba(texture_color, &trg, &tba);

      GSVector4i rg, ba;
      if (modulation_mode == GPU_SW_TEX_MODULATION_8BIT) {
        rg = gsv4i_mul16l(trg, vertex_color_rg);
        ba = gsv4i_mul16l(tba, vertex_color_ba);
        /* Convert to 5-bit: >> 4, +dither, max(0), >> 3. */
        rg = gsv4i_sra16(gsv4i_max_s16(gsv4i_add16(gsv4i_sra16(rg, 4), dither), gsv4i_zero()), 3);
        ba = gsv4i_sra16(gsv4i_max_s16(gsv4i_add16(gsv4i_sra16(ba, 4), dither), gsv4i_zero()), 3);
      } else {
        /* 5-bit modulate: vertex first downsampled to 5-bit. */
        rg = gsv4i_mul16l(trg, gsv4i_sra16(vertex_color_rg, 3));
        ba = gsv4i_mul16l(tba, gsv4i_sra16(vertex_color_ba, 3));
        rg = gsv4i_sra16(gsv4i_max_s16(gsv4i_add16(gsv4i_sra16(rg, 1), dither), gsv4i_zero()), 3);
        ba = gsv4i_sra16(gsv4i_max_s16(gsv4i_add16(gsv4i_sra16(ba, 1), dither), gsv4i_zero()), 3);
      }

      /* Bit15 (alpha) passes through from texture: blend even u16 lanes
       * (rg) untouched, odd u16 lanes (ba) replaced from tba. */
      ba = gsv4i_blend16(ba, tba, 0xAA);

      /* Clamp to 5-bit. */
      const GSVector4i clamp31 = gsv4i_set16((s16)0x1F);
      rg = gsv4i_min_u16(rg, clamp31);
      ba = gsv4i_min_u16(ba, clamp31);

      color = rg_ba_to_rgb5a1(rg, ba);
    }
  } else {
    /* Untextured: vertex color + dither + clamp. */
    GSVector4i rg = gsv4i_sra16(gsv4i_max_s16(gsv4i_add16(vertex_color_rg, dither), gsv4i_zero()), 3);
    GSVector4i ba = gsv4i_sra16(gsv4i_max_s16(gsv4i_add16(vertex_color_ba, dither), gsv4i_zero()), 3);
    /* Use 32-bit clamp on ba so the alpha lane stays 0. */
    rg = gsv4i_min_u16(rg, gsv4i_set16((s16)0x1F));
    ba = gsv4i_min_u16(ba, gsv4i_set32(0x1F));
    color = rg_ba_to_rgb5a1(rg, ba);
  }

  /* Direct-store fast path: no mask test, no transparency, no preserve. */
  if (!mask_bit_test && !transparency_enable && gsv4i_allfalse(preserve_mask)) {
    color = gsv4i_or(color, pv->mask_or);
  } else {
    GSVector4i bg_color = load_vector(start_x, y);

    if (transparency_enable) {
      /* transparent_mask: ffff per lane that's transparent (texture-only:
       * texel had bit15 set; untextured: all lanes treated as transparent
       * because the rasterizer added 0x8000 to color sentinel pre-blend).
       * Computed from color's MSB after we OR'd 0x8000 above. */
      GSVector4i transparent_mask = gsv4i_zero();
      if (texture_enable) {
        transparent_mask = gsv4i_sra16(color, 15);
      }

      GSVector4i blended;
      switch (transparency_mode) {
        case GPU_TRANSPARENCY_MODE_HALF_BG_PLUS_HALF_FG: {
          const GSVector4i fg_bits = gsv4i_or(color, gsv4i_set32(0x8000));
          const GSVector4i bg_bits = gsv4i_or(bg_color, gsv4i_set32(0x8000));
          const GSVector4i res =
            gsv4i_srl32(gsv4i_sub32(gsv4i_add32(fg_bits, bg_bits),
                                    gsv4i_and(gsv4i_xor(fg_bits, bg_bits), gsv4i_set32(0x0421))), 1);
          blended = gsv4i_and(res, gsv4i_set32(0xFFFF));
          break;
        }
        case GPU_TRANSPARENCY_MODE_BG_PLUS_FG: {
          const GSVector4i fg_bits = gsv4i_or(color, gsv4i_set32(0x8000));
          const GSVector4i bg_bits = gsv4i_and(bg_color, gsv4i_set32(0x7FFF));
          const GSVector4i sum = gsv4i_add32(fg_bits, bg_bits);
          const GSVector4i carry =
            gsv4i_and(gsv4i_sub32(sum, gsv4i_and(gsv4i_xor(fg_bits, bg_bits), gsv4i_set32(0x8421))),
                      gsv4i_set32(0x8420));
          const GSVector4i res = gsv4i_or(gsv4i_sub32(sum, carry),
                                           gsv4i_sub32(carry, gsv4i_srl32(carry, 5)));
          blended = gsv4i_and(res, gsv4i_set32(0xFFFF));
          break;
        }
        case GPU_TRANSPARENCY_MODE_BG_MINUS_FG: {
          const GSVector4i bg_bits = gsv4i_or(bg_color, gsv4i_set32(0x8000));
          const GSVector4i fg_bits = gsv4i_and(color, gsv4i_set32(0x7FFF));
          const GSVector4i diff = gsv4i_add32(gsv4i_sub32(bg_bits, fg_bits), gsv4i_set32(0x108420));
          const GSVector4i borrow =
            gsv4i_and(gsv4i_sub32(diff, gsv4i_and(gsv4i_xor(bg_bits, fg_bits), gsv4i_set32(0x108420))),
                      gsv4i_set32(0x108420));
          const GSVector4i res = gsv4i_and(gsv4i_sub32(diff, borrow),
                                            gsv4i_sub32(borrow, gsv4i_srl32(borrow, 5)));
          blended = gsv4i_and(res, gsv4i_set32(0xFFFF));
          break;
        }
        case GPU_TRANSPARENCY_MODE_BG_PLUS_QUARTER_FG:
        default: {
          const GSVector4i bg_bits = gsv4i_and(bg_color, gsv4i_set32(0x7FFF));
          const GSVector4i fg_bits =
            gsv4i_or(gsv4i_and(gsv4i_srl32(gsv4i_or(color, gsv4i_set32(0x8000)), 2),
                               gsv4i_set32(0x1CE7)),
                     gsv4i_set32(0x8000));
          const GSVector4i sum = gsv4i_add32(fg_bits, bg_bits);
          const GSVector4i carry =
            gsv4i_and(gsv4i_sub32(sum, gsv4i_and(gsv4i_xor(fg_bits, bg_bits), gsv4i_set32(0x8421))),
                      gsv4i_set32(0x8420));
          const GSVector4i res = gsv4i_or(gsv4i_sub32(sum, carry),
                                           gsv4i_sub32(carry, gsv4i_srl32(carry, 5)));
          blended = gsv4i_and(res, gsv4i_set32(0xFFFF));
          break;
        }
      }

      if (texture_enable) {
        /* Texel transparency varies per-lane; blend using texture bit15 mask. */
        color = gsv4i_blend8(color, blended, transparent_mask);
      } else {
        /* All lanes blended; clear the sentinel bit. */
        color = gsv4i_and(blended, gsv4i_set32(0x7FFF));
      }
    }

    /* mask_bits_set: ffff per lane where bg already has mask bit. */
    GSVector4i mask_bits_set = gsv4i_and(bg_color, pv->mask_and);
    mask_bits_set = gsv4i_sra16(mask_bits_set, 15);
    preserve_mask = gsv4i_or(preserve_mask, mask_bits_set);

    bg_color = gsv4i_and(bg_color, preserve_mask);
    color    = gsv4i_andnot(preserve_mask, gsv4i_or(color, pv->mask_or));
    color    = gsv4i_or(color, bg_color);
  }

  store_vector(start_x, y, color);
}

/* ---------------------------------------------------------------------------
 *  draw_rectangle SIMD: 4 pixels horizontally per inner iteration.
 *  Mirrors .inl:738.
 * --------------------------------------------------------------------------- */

void gpu_sw_rasterizer_simd_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* cmd)
{
  const gpu_backend_draw_cmd_t* base = &cmd->base;
  const gpu_sw_tex_modulation_t mod =
    base->texture_enable
      ? (base->raw_texture_enable ? GPU_SW_TEX_MODULATION_NO_MODULATION : GPU_SW_TEX_MODULATION_8BIT)
      : GPU_SW_TEX_MODULATION_DISABLED;
  const bool texture_enable = (mod != GPU_SW_TEX_MODULATION_DISABLED);

  /* RGBA color spread per upstream .inl:746-750.
   *   rgba = R | G | B | A  (4 u8 in lane 0)
   *   xxxxl: lane 0 broadcast over low 4 lanes (still in low 64 bits as u8s).
   *   u8to16: low 8 u8s expand to 8 u16s.
   *   So `rg` ends up: R0 G0 R0 G0 R0 G0 R0 G0  (per-lane pair RG).
   *   `ba` similarly: B0 A0 B0 A0 ...
   * gsv4i_set32((s32)cmd->color) puts color in all 4 u32 lanes; equivalent
   * to the duckstation broadcast128 step then. */
  const GSVector4i rgba = gsv4i_set32((s32)cmd->color);
  const GSVector4i rgp  = gsv4i_xxxxl(rgba);
  const GSVector4i bap  = gsv4i_yyyyl(rgba);
  const GSVector4i rg   = gsv4i_u8to16(rgp);
  const GSVector4i ba   = gsv4i_u8to16(bap);

  GSVector4i texcoord_x = gsv4i_add32(gsv4i_set32((s32)(cmd->texcoord & 0xFF)), span_offset_vec());
  GSVector4i texcoord_y = gsv4i_set32((s32)(cmd->texcoord >> 8));

  pixel_vectors_t pv;
  pixel_vectors_init(&pv, base, texture_enable);

  const u32 width = cmd->width;
  const gpu_transparency_mode_t tmode = gpu_draw_mode_reg_transparency_mode(base->draw_mode);
  const gpu_texture_mode_t      texm  = gpu_draw_mode_reg_texture_mode(base->draw_mode);
  const bool mask_bit_test = base->check_mask_before_draw;
  const s32 origin_x = cmd->x;
  const s32 origin_y = cmd->y;

  for (u32 offset_y = 0; offset_y < cmd->height; offset_y++) {
    const s32 y = origin_y + (s32)offset_y;
    const bool in_y = (y >= (s32)gpu_sw_drawing_area.top &&
                       y <= (s32)gpu_sw_drawing_area.bottom);
    const bool il_skip = base->interlaced_rendering &&
                         base->active_line_lsb == (((u32)y) & 1u);
    if (in_y && !il_skip) {
      const s32 draw_y = (s32)((u32)y & VRAM_HEIGHT_MASK);

      GSVector4i row_texcoord_x = texcoord_x;
      GSVector4i xvec = gsv4i_add32(gsv4i_set32(origin_x), span_offset_vec());
      GSVector4i wvec = gsv4i_sub32(gsv4i_set32((s32)width), span_width_vec());

      for (u32 offset_x = 0; offset_x < width; offset_x += PIXELS_PER_VEC) {
        const s32 x = origin_x + (s32)offset_x;

        /* preserve_mask ffff per lane that's outside [clip_left,clip_right] OR past width. */
        GSVector4i preserve_mask = gsv4i_lt32(wvec, gsv4i_zero());
        preserve_mask = gsv4i_or(preserve_mask, gsv4i_lt32(xvec, pv.clip_left));
        preserve_mask = gsv4i_or(preserve_mask, gsv4i_gt32(xvec, pv.clip_right));
        if (!gsv4i_alltrue(preserve_mask)) {
          shade_pixel_simd(&pv, mod, base->transparency_enable, texm, tmode, mask_bit_test,
                           (u32)x, (u32)draw_y, rg, ba, row_texcoord_x, texcoord_y,
                           preserve_mask, gsv4i_zero());
        }

        xvec = gsv4i_add32(xvec, ppv_vec());
        wvec = gsv4i_sub32(wvec, ppv_vec());
        if (texture_enable)
          row_texcoord_x = gsv4i_and(gsv4i_add32(row_texcoord_x, ppv_vec()), gsv4i_set32(0xFF));
      }
    }
    if (texture_enable)
      texcoord_y = gsv4i_and(gsv4i_add32(texcoord_y, gsv4i_set32(1)), gsv4i_set32(0xFF));
  }
}

/* ---------------------------------------------------------------------------
 *  TriangleVectors: per-tri vector constants built once before the row loop.
 *  Mirrors .inl:1162.
 * --------------------------------------------------------------------------- */

typedef struct {
  pixel_vectors_t pv;
  /* shading-only fields zero-init when shading_enable=false (cheap). */
  GSVector4i drdx, dgdx, dbdx;
  GSVector4i drdx_0123, dgdx_0123, dbdx_0123;
  /* texture-only similarly. */
  GSVector4i dudx, dvdx;
  GSVector4i dudx_0123, dvdx_0123;
} triangle_vectors_t;

typedef struct { u32 dudx, dvdx, dudy, dvdy; } uv_steps_simd_t;
typedef struct { u32 drdx, dgdx, dbdx, drdy, dgdy, dbdy; } rgb_steps_simd_t;

static inline void triangle_vectors_init(triangle_vectors_t* tv,
                                          const gpu_backend_draw_cmd_t* cmd,
                                          const uv_steps_simd_t* uvstep,
                                          const rgb_steps_simd_t* rgbstep,
                                          bool shading_enable, bool texture_enable)
{
  pixel_vectors_init(&tv->pv, cmd, texture_enable);
  if (shading_enable) {
    tv->drdx = gsv4i_set32((s32)(rgbstep->drdx * PIXELS_PER_VEC));
    tv->dgdx = gsv4i_set32((s32)(rgbstep->dgdx * PIXELS_PER_VEC));
    tv->dbdx = gsv4i_set32((s32)(rgbstep->dbdx * PIXELS_PER_VEC));
    tv->drdx_0123 = gsv4i_mul32l(gsv4i_set32((s32)rgbstep->drdx), span_offset_vec());
    tv->dgdx_0123 = gsv4i_mul32l(gsv4i_set32((s32)rgbstep->dgdx), span_offset_vec());
    tv->dbdx_0123 = gsv4i_mul32l(gsv4i_set32((s32)rgbstep->dbdx), span_offset_vec());
  } else {
    tv->drdx = tv->dgdx = tv->dbdx = gsv4i_zero();
    tv->drdx_0123 = tv->dgdx_0123 = tv->dbdx_0123 = gsv4i_zero();
  }
  if (texture_enable) {
    tv->dudx = gsv4i_set32((s32)(uvstep->dudx * PIXELS_PER_VEC));
    tv->dvdx = gsv4i_set32((s32)(uvstep->dvdx * PIXELS_PER_VEC));
    tv->dudx_0123 = gsv4i_mul32l(gsv4i_set32((s32)uvstep->dudx), span_offset_vec());
    tv->dvdx_0123 = gsv4i_mul32l(gsv4i_set32((s32)uvstep->dvdx), span_offset_vec());
  } else {
    tv->dudx = tv->dvdx = gsv4i_zero();
    tv->dudx_0123 = tv->dvdx_0123 = gsv4i_zero();
  }
}

/* Stepper state (mirrors .inl scalar UVStepper / RGBStepper but used by SIMD path
 * for setup; per-pixel stepping happens in vector form inside draw_span_simd). */
typedef struct { u32 u, v; } uv_stepper_simd_t;
typedef struct { u32 r, g, b; } rgb_stepper_simd_t;

#define ATTRIB_SHIFT       12
#define ATTRIB_POST_SHIFT  12

static inline void uv_init_simd(uv_stepper_simd_t* s, u32 u0, u32 v0)
{
  s->u = ((u0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
  s->v = ((v0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
}
static inline void rgb_init_simd(rgb_stepper_simd_t* s, u32 r0, u32 g0, u32 b0)
{
  s->r = ((r0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
  s->g = ((g0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
  s->b = ((b0 << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT;
}

static inline void uv_step_x_simd(uv_stepper_simd_t* s, const uv_steps_simd_t* st, s32 n)
{
  s->u += (u32)((s32)st->dudx * n);
  s->v += (u32)((s32)st->dvdx * n);
}
static inline void rgb_step_x_simd(rgb_stepper_simd_t* s, const rgb_steps_simd_t* st, s32 n)
{
  s->r += (u32)((s32)st->drdx * n);
  s->g += (u32)((s32)st->dgdx * n);
  s->b += (u32)((s32)st->dbdx * n);
}
static inline void uv_step_y_simd(uv_stepper_simd_t* s, const uv_steps_simd_t* st, s32 n)
{
  s->u += (u32)((s32)st->dudy * n);
  s->v += (u32)((s32)st->dvdy * n);
}
static inline void rgb_step_y_simd(rgb_stepper_simd_t* s, const rgb_steps_simd_t* st, s32 n)
{
  s->r += (u32)((s32)st->drdy * n);
  s->g += (u32)((s32)st->dgdy * n);
  s->b += (u32)((s32)st->dbdy * n);
}
static inline void uv_step_y1_simd(uv_stepper_simd_t* s, const uv_steps_simd_t* st, bool upside_down)
{
  s->u = upside_down ? (s->u - st->dudy) : (s->u + st->dudy);
  s->v = upside_down ? (s->v - st->dvdy) : (s->v + st->dvdy);
}
static inline void rgb_step_y1_simd(rgb_stepper_simd_t* s, const rgb_steps_simd_t* st, bool upside_down)
{
  s->r = upside_down ? (s->r - st->drdy) : (s->r + st->drdy);
  s->g = upside_down ? (s->g - st->dgdy) : (s->g + st->dgdy);
  s->b = upside_down ? (s->b - st->dbdy) : (s->b + st->dbdy);
}

/* ---------------------------------------------------------------------------
 *  draw_span_simd: SIMD inner loop, 4 pixels at a time.
 *  Mirrors .inl:1203.
 * --------------------------------------------------------------------------- */

ALWAYS_INLINE static void draw_span_simd(const gpu_backend_draw_cmd_t* RESTRICT cmd,
                                          gpu_sw_tex_modulation_t mod,
                                          bool transparency_enable, bool shading_enable,
                                          s32 y, s32 x_start, s32 x_bound,
                                          uv_stepper_simd_t uv,
                                          const uv_steps_simd_t* RESTRICT uvstep,
                                          rgb_stepper_simd_t rgb,
                                          const rgb_steps_simd_t* RESTRICT rgbstep,
                                          const triangle_vectors_t* RESTRICT tv)
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

  GSVector4i dr, dg, db;
  if (shading_enable) {
    dr = gsv4i_add32(gsv4i_set32((s32)(rgb.r + rgbstep->drdx * (u32)x_start)), tv->drdx_0123);
    dg = gsv4i_add32(gsv4i_set32((s32)(rgb.g + rgbstep->dgdx * (u32)x_start)), tv->dgdx_0123);
    db = gsv4i_add32(gsv4i_set32((s32)(rgb.b + rgbstep->dbdx * (u32)x_start)), tv->dbdx_0123);
  } else {
    /* Flat: precompute final per-channel constants. */
    dr = gsv4i_set32((s32)(rgb.r >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)));
    dg = gsv4i_set32((s32)((rgb.g >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)) << 16));
    db = gsv4i_set32((s32)(rgb.b >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)));
  }

  GSVector4i du, dv;
  if (texture_enable) {
    du = gsv4i_add32(gsv4i_set32((s32)(uv.u + uvstep->dudx * (u32)x_start)), tv->dudx_0123);
    dv = gsv4i_add32(gsv4i_set32((s32)(uv.v + uvstep->dvdx * (u32)x_start)), tv->dvdx_0123);
  } else {
    du = gsv4i_zero(); dv = gsv4i_zero();
  }

  GSVector4i dither = gsv4i_zero();
  if (cmd->dither_enable) {
    /* Offset (x&3)*2 -> 0/4/8/12 bytes into the row; not 16-aligned, so
     * unaligned load. Cost is negligible on SSE4.1+. */
    dither = gsv4i_load_u(&VECTOR_DITHER_MATRIX[(u32)y & 3][((u32)current_x & 3) * 2]);
  }

  GSVector4i xvec = gsv4i_add32(gsv4i_set32(current_x), span_offset_vec());
  GSVector4i wvec = gsv4i_sub32(gsv4i_set32(width), span_width_vec());

  const gpu_transparency_mode_t tmode = gpu_draw_mode_reg_transparency_mode(cmd->draw_mode);
  const gpu_texture_mode_t      texm  = gpu_draw_mode_reg_texture_mode(cmd->draw_mode);
  const bool mask_bit_test = cmd->check_mask_before_draw;

  for (s32 count = (width + (PIXELS_PER_VEC - 1)) / PIXELS_PER_VEC; count > 0; --count) {
    /* Channel layout post-shift:
     *   r = R | R | R | R   (in low 16 of each 32-bit lane)
     *   g = G shifted to bit 16 of each lane
     *   b = B
     * For flat, dr/dg/db are pre-arranged. */
    const GSVector4i r = shading_enable ? gsv4i_srl32(dr, ATTRIB_SHIFT + ATTRIB_POST_SHIFT) : dr;
    const GSVector4i g = shading_enable
                          ? gsv4i_sll32(gsv4i_srl32(dg, ATTRIB_SHIFT + ATTRIB_POST_SHIFT), 16)
                          : dg;
    const GSVector4i b = shading_enable ? gsv4i_srl32(db, ATTRIB_SHIFT + ATTRIB_POST_SHIFT) : db;
    const GSVector4i u_v = gsv4i_srl32(du, ATTRIB_SHIFT + ATTRIB_POST_SHIFT);
    const GSVector4i v_v = gsv4i_srl32(dv, ATTRIB_SHIFT + ATTRIB_POST_SHIFT);

    /* rg pair: even u16 lanes from r, odd u16 lanes from g (already shifted). */
    const GSVector4i rg = gsv4i_blend16(r, g, 0xAA);

    GSVector4i preserve_mask = gsv4i_lt32(wvec, gsv4i_zero());
    preserve_mask = gsv4i_or(preserve_mask, gsv4i_lt32(xvec, tv->pv.clip_left));
    preserve_mask = gsv4i_or(preserve_mask, gsv4i_gt32(xvec, tv->pv.clip_right));
    if (!gsv4i_alltrue(preserve_mask)) {
      shade_pixel_simd(&tv->pv, mod, transparency_enable, texm, tmode, mask_bit_test,
                       (u32)current_x, (u32)y, rg, b, u_v, v_v, preserve_mask, dither);
    }

    current_x += PIXELS_PER_VEC;
    xvec = gsv4i_add32(xvec, ppv_vec());
    wvec = gsv4i_sub32(wvec, ppv_vec());

    if (shading_enable) {
      dr = gsv4i_add32(dr, tv->drdx);
      dg = gsv4i_add32(dg, tv->dgdx);
      db = gsv4i_add32(db, tv->dbdx);
    }
    if (texture_enable) {
      du = gsv4i_add32(du, tv->dudx);
      dv = gsv4i_add32(dv, tv->dvdx);
    }
  }
}

/* ---------------------------------------------------------------------------
 *  draw_triangle_part_simd: per-half iterator over scanlines.
 *  Mirrors .inl:1314.
 * --------------------------------------------------------------------------- */

typedef struct {
  u64 start_x[2];
  u64 step_x[2];
  s32 start_y;
  s32 end_y;
  bool fill_upside_down;
} triangle_part_simd_t;

static void draw_triangle_part_simd(const gpu_backend_draw_cmd_t* RESTRICT cmd,
                                     gpu_sw_tex_modulation_t mod,
                                     bool transparency_enable, bool shading_enable,
                                     const triangle_part_simd_t* tp,
                                     uv_stepper_simd_t uv,
                                     const uv_steps_simd_t* uvstep,
                                     rgb_stepper_simd_t rgb,
                                     const rgb_steps_simd_t* rgbstep,
                                     const triangle_vectors_t* RESTRICT tv)
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
    if (texture_enable) uv_step_y_simd(&uv, uvstep, current_y);
    if (shading_enable) rgb_step_y_simd(&rgb, rgbstep, current_y);

    do {
      current_y--;
      left_x  -= left_x_step;
      right_x -= right_x_step;

      const s32 y = truncate_gpu_vertex_position(current_y);
      if (y < (s32)gpu_sw_drawing_area.top) break;

      if (texture_enable) uv_step_y1_simd(&uv, uvstep, true);
      if (shading_enable) rgb_step_y1_simd(&rgb, rgbstep, true);

      if (y > (s32)gpu_sw_drawing_area.bottom ||
          (cmd->interlaced_rendering &&
           cmd->active_line_lsb == ((u32)current_y & 1u)))
        continue;

      const s32 lx = (s32)((u64)left_x  >> 32);
      const s32 rx = (s32)((u64)right_x >> 32);
      draw_span_simd(cmd, mod, transparency_enable, shading_enable,
                     y & VRAM_HEIGHT_MASK, lx, rx, uv, uvstep, rgb, rgbstep, tv);
    } while (current_y > end_y);
  } else {
    if (current_y >= end_y) return;
    if (texture_enable) uv_step_y_simd(&uv, uvstep, current_y);
    if (shading_enable) rgb_step_y_simd(&rgb, rgbstep, current_y);

    do {
      const s32 y = truncate_gpu_vertex_position(current_y);
      if (y > (s32)gpu_sw_drawing_area.bottom) break;
      if (y >= (s32)gpu_sw_drawing_area.top &&
          (!cmd->interlaced_rendering ||
           cmd->active_line_lsb != ((u32)current_y & 1u))) {
        const s32 lx = (s32)((u64)left_x  >> 32);
        const s32 rx = (s32)((u64)right_x >> 32);
        draw_span_simd(cmd, mod, transparency_enable, shading_enable,
                       y & VRAM_HEIGHT_MASK, lx, rx, uv, uvstep, rgb, rgbstep, tv);
      }
      current_y++;
      left_x  += left_x_step;
      right_x += right_x_step;
      if (texture_enable) uv_step_y1_simd(&uv, uvstep, false);
      if (shading_enable) rgb_step_y1_simd(&rgb, rgbstep, false);
    } while (current_y < end_y);
  }
}

/* ---------------------------------------------------------------------------
 *  draw_triangle SIMD: vertex sort + edge step + ATTRIB_STEP setup, then
 *  call draw_triangle_part_simd. Sort/edge logic identical to scalar
 *  (.inl:1418); only the per-pixel inner loop is vectorized.
 * --------------------------------------------------------------------------- */

void gpu_sw_rasterizer_simd_draw_triangle(const gpu_backend_draw_cmd_t* cmd,
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

  /* Vertex sort: v0 = top, v1 = mid, v2 = bottom; tl indexes top-left vertex. */
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

  if (v0->y == v2->y) return;

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

  triangle_part_simd_t triparts[2] = {{{0,0},{0,0},0,0,false},{{0,0},{0,0},0,0,false}};
  triangle_part_simd_t* tpo = &triparts[vo];
  triangle_part_simd_t* tpp = &triparts[vo ^ 1];

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

  const s32 det = ((v1->x - v0->x) * (v2->y - v1->y)) - ((v2->x - v1->x) * (v1->y - v0->y));
  if (det == 0) return;

#define ATTRIB_DET_2(ax, ay, bx, by) (((s32)(v1->ax - v0->ax) * (s32)(v2->by - v1->by)) - \
                                       ((s32)(v2->ax - v1->ax) * (s32)(v1->by - v0->by)))
#define ATTRIB_STEP_2(ax, ay, bx, by) ((u32)(ATTRIB_DET_2(ax, ay, bx, by) * (1 << ATTRIB_SHIFT) / det) << ATTRIB_POST_SHIFT)

  uv_steps_simd_t  uvstep  = {0,0,0,0};
  rgb_steps_simd_t rgbstep = {0,0,0,0,0,0};
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

  uv_stepper_simd_t  uv  = {0, 0};
  rgb_stepper_simd_t rgb = {0, 0, 0};
  const gpu_backend_polygon_vertex_t* tlv = vertices[tl];
  if (texture_enable) {
    uv_init_simd(&uv, tlv->u, tlv->v);
    uv_step_x_simd(&uv, &uvstep, -tlv->x);
    uv_step_y_simd(&uv, &uvstep, -tlv->y);
  }
  rgb_init_simd(&rgb, tlv->r, tlv->g, tlv->b);
  if (shading_enable) {
    rgb_step_x_simd(&rgb, &rgbstep, -tlv->x);
    rgb_step_y_simd(&rgb, &rgbstep, -tlv->y);
  }

  triangle_vectors_t tv;
  triangle_vectors_init(&tv, cmd, &uvstep, &rgbstep, shading_enable, texture_enable);

  for (u32 i = 0; i < 2; i++) {
    draw_triangle_part_simd(cmd, mod, transparency_enable, shading_enable,
                             &triparts[i], uv, &uvstep, rgb, &rgbstep, &tv);
  }
}

/* ---------------------------------------------------------------------------
 *  Forwarders: line/fill/write/copy_vram defer to scalar / AVX2 paths.
 *  Line is per-pixel; SIMD wouldn't help. Fill/Write/Copy already vectorized
 *  in the AVX2 TU; the SIMD vtable just borrows scalar (which has its own
 *  in-line fast path for non-wrapping rectangles).
 * --------------------------------------------------------------------------- */

void gpu_sw_rasterizer_simd_draw_line(const gpu_backend_draw_cmd_t* cmd,
                                       const gpu_backend_line_vertex_t* p0,
                                       const gpu_backend_line_vertex_t* p1)
{ gpu_sw_rasterizer_scalar_draw_line(cmd, p0, p1); }

void gpu_sw_rasterizer_simd_fill_vram(u32 x, u32 y, u32 width, u32 height, u32 color,
                                       bool interlaced, u8 active_line_lsb)
{ gpu_sw_rasterizer_scalar_fill_vram(x, y, width, height, color, interlaced, active_line_lsb); }

void gpu_sw_rasterizer_simd_write_vram(u32 x, u32 y, u32 width, u32 height, const u16* data,
                                        bool set_mask, bool check_mask)
{ gpu_sw_rasterizer_scalar_write_vram(x, y, width, height, data, set_mask, check_mask); }

void gpu_sw_rasterizer_simd_copy_vram(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                       u32 width, u32 height, bool set_mask, bool check_mask)
{ gpu_sw_rasterizer_scalar_copy_vram(src_x, src_y, dst_x, dst_y, width, height, set_mask, check_mask); }
