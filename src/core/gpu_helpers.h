/*
 * Pure inline helpers shared between the GPU register emulator and the
 * software rasterizer.  No state, no allocations; header-only.  Scalar
 * only; no SIMD paths.
 */

#ifndef CUPID_CORE_GPU_HELPERS_H
#define CUPID_CORE_GPU_HELPERS_H

#include "core/gpu_types.h"
#include "common/bitutils.h"
#include "common/types.h"

/* RGBA5551 -> RGBA8888 expansion.  527/64 magic from
 * https://stackoverflow.com/a/9069480; best 5->8 bit upscale. */
ALWAYS_INLINE u32 vram_rgba5551_to_rgba8888(u32 color)
{
#define E5TO8(c) ((((c) * 527u) + 23u) >> 6)
  const u32 r = E5TO8(color & 31u);
  const u32 g = E5TO8((color >> 5) & 31u);
  const u32 b = E5TO8((color >> 10) & 31u);
  const u32 a = ((color >> 15) != 0) ? 255u : 0u;
  return r | (g << 8) | (b << 16) | (a << 24);
#undef E5TO8
}

ALWAYS_INLINE u16 vram_rgba8888_to_rgba5551(u32 color)
{
  const u32 r = (color & 0xFFu) >> 3;
  const u32 g = ((color >> 8) & 0xFFu) >> 3;
  const u32 b = ((color >> 16) & 0xFFu) >> 3;
  const u32 a = ((color >> 24) & 0x01u);
  return (u16)(r | (g << 5) | (b << 10) | (a << 15));
}

/* True if the texture mode uses a CLUT (Palette4 / Palette8). */
ALWAYS_INLINE bool gpu_texture_mode_has_palette(gpu_texture_mode_t mode)
{
  return mode < GPU_TEXTURE_MODE_DIRECT_16BIT;
}

ALWAYS_INLINE s32 truncate_gpu_vertex_position(s32 x)
{
  /* Sign-extend bit 10 across the upper bits. */
  const s32 mask = (1 << 11) - 1;
  s32 v = x & mask;
  if (v & (1 << 10))
    v |= ~mask;
  return v;
}

ALWAYS_INLINE u32 vram_page_index(u32 px, u32 py)
{
  return (py * VRAM_PAGES_WIDE) + px;
}
ALWAYS_INLINE u32 vram_coordinate_to_page(u32 x, u32 y)
{
  return vram_page_index(x / VRAM_PAGE_WIDTH, y / VRAM_PAGE_HEIGHT);
}
ALWAYS_INLINE u32 vram_page_start_x(u32 pn)
{
  return (pn % VRAM_PAGES_WIDE) * VRAM_PAGE_WIDTH;
}
ALWAYS_INLINE u32 vram_page_start_y(u32 pn)
{
  return (pn / VRAM_PAGES_WIDE) * VRAM_PAGE_HEIGHT;
}

ALWAYS_INLINE u8 get_texture_mode_shift(gpu_texture_mode_t mode)
{
  return (mode < GPU_TEXTURE_MODE_DIRECT_16BIT) ? (u8)(2 - (u8)mode) : (u8)0;
}
ALWAYS_INLINE u32 apply_texture_mode_shift(gpu_texture_mode_t mode, u32 vram_width)
{
  return vram_width << get_texture_mode_shift(mode);
}
ALWAYS_INLINE u32 texture_page_count_for_mode(gpu_texture_mode_t mode)
{
  return (mode < GPU_TEXTURE_MODE_DIRECT_16BIT) ? (1u + (u32)mode) : 4u;
}
ALWAYS_INLINE u32 texture_page_width_for_mode(gpu_texture_mode_t mode)
{
  return TEXTURE_PAGE_WIDTH >> get_texture_mode_shift(mode);
}
ALWAYS_INLINE bool texture_page_is_wrapping(gpu_texture_mode_t mode, u32 pn)
{
  return (vram_page_start_x(pn) + texture_page_width_for_mode(mode)) > VRAM_WIDTH;
}

ALWAYS_INLINE u32 palette_page_count_for_mode(gpu_texture_mode_t mode)
{
  return (mode == GPU_TEXTURE_MODE_PALETTE_4BIT) ? 1u : 4u;
}
ALWAYS_INLINE u32 palette_page_number(gpu_texture_palette_reg_t reg)
{
  return vram_coordinate_to_page(gpu_texture_palette_reg_get_x_base(reg),
                                 gpu_texture_palette_reg_get_y_base(reg));
}
ALWAYS_INLINE u32 get_palette_width(gpu_texture_mode_t mode)
{
  return (mode == GPU_TEXTURE_MODE_PALETTE_4BIT) ? 16u
         : (mode == GPU_TEXTURE_MODE_PALETTE_8BIT) ? 256u : 0u;
}

#endif /* CUPID_CORE_GPU_HELPERS_H */
