/*
 * uses BitField<> unions; here we keep the raw `bits` word and provide inline
 * accessor functions, matching the convention established in util/cd_image.h.
 */

#ifndef CUPID_CORE_GPU_TYPES_H
#define CUPID_CORE_GPU_TYPES_H

#include "common/types.h"

enum {
  VRAM_WIDTH       = 1024,
  VRAM_HEIGHT      = 512,
  VRAM_SIZE        = VRAM_WIDTH * VRAM_HEIGHT * (s32)sizeof(u16),
  VRAM_WIDTH_MASK  = VRAM_WIDTH - 1,
  VRAM_HEIGHT_MASK = VRAM_HEIGHT - 1,

  TEXTURE_PAGE_WIDTH  = 256,
  TEXTURE_PAGE_HEIGHT = 256,
  GPU_CLUT_SIZE       = 256,

  /* Interlaced PAL modes can exceed VRAM_HEIGHT (up to 576). */
  GPU_MAX_DISPLAY_WIDTH  = 720,
  GPU_MAX_DISPLAY_HEIGHT = 576,

  DITHER_MATRIX_SIZE = 4,

  VRAM_PAGE_WIDTH   = 64,
  VRAM_PAGE_HEIGHT  = 256,
  VRAM_PAGES_WIDE   = VRAM_WIDTH / VRAM_PAGE_WIDTH,
  VRAM_PAGES_HIGH   = VRAM_HEIGHT / VRAM_PAGE_HEIGHT,
  VRAM_PAGE_X_MASK  = 0xf,  /* 16 pages wide */
  VRAM_PAGE_Y_MASK  = 0x10, /* 2 pages high */
  NUM_VRAM_PAGES    = VRAM_PAGES_WIDE * VRAM_PAGES_HIGH,
};

enum {
  MAX_PRIMITIVE_WIDTH  = 1024,
  MAX_PRIMITIVE_HEIGHT = 512,
};

typedef enum : u8 {
  GPU_DMA_DIRECTION_OFF           = 0,
  GPU_DMA_DIRECTION_FIFO          = 1,
  GPU_DMA_DIRECTION_CPU_TO_GP0    = 2,
  GPU_DMA_DIRECTION_GPUREAD_TO_CPU = 3,
} gpu_dma_direction_t;

typedef enum : u8 {
  GPU_PRIMITIVE_RESERVED  = 0,
  GPU_PRIMITIVE_POLYGON   = 1,
  GPU_PRIMITIVE_LINE      = 2,
  GPU_PRIMITIVE_RECTANGLE = 3,
} gpu_primitive_t;

typedef enum : u8 {
  GPU_DRAW_RECTANGLE_SIZE_VARIABLE = 0,
  GPU_DRAW_RECTANGLE_SIZE_R1X1     = 1,
  GPU_DRAW_RECTANGLE_SIZE_R8X8     = 2,
  GPU_DRAW_RECTANGLE_SIZE_R16X16   = 3,
} gpu_draw_rectangle_size_t;

typedef enum : u8 {
  GPU_TEXTURE_MODE_PALETTE_4BIT          = 0,
  GPU_TEXTURE_MODE_PALETTE_8BIT          = 1,
  GPU_TEXTURE_MODE_DIRECT_16BIT          = 2,
  GPU_TEXTURE_MODE_RESERVED_DIRECT_16BIT = 3, /* unused */
} gpu_texture_mode_t;

typedef enum : u8 {
  GPU_TRANSPARENCY_MODE_HALF_BG_PLUS_HALF_FG     = 0,
  GPU_TRANSPARENCY_MODE_BG_PLUS_FG               = 1,
  GPU_TRANSPARENCY_MODE_BG_MINUS_FG              = 2,
  GPU_TRANSPARENCY_MODE_BG_PLUS_QUARTER_FG       = 3,
  GPU_TRANSPARENCY_MODE_DISABLED                 = 4, /* not a register value */
} gpu_transparency_mode_t;

typedef enum : u8 {
  GPU_INTERLACED_DISPLAY_MODE_NONE,
  GPU_INTERLACED_DISPLAY_MODE_INTERLEAVED_FIELDS,
  GPU_INTERLACED_DISPLAY_MODE_SEPARATE_FIELDS,
} gpu_interlaced_display_mode_t;

typedef enum : u8 {
  GP1_COMMAND_RESET_GPU                  = 0x00,
  GP1_COMMAND_CLEAR_FIFO                 = 0x01,
  GP1_COMMAND_ACKNOWLEDGE_INTERRUPT      = 0x02,
  GP1_COMMAND_SET_DISPLAY_DISABLE        = 0x03,
  GP1_COMMAND_SET_DMA_DIRECTION          = 0x04,
  GP1_COMMAND_SET_DISPLAY_START_ADDRESS  = 0x05,
  GP1_COMMAND_SET_HORIZONTAL_DISPLAY_RANGE = 0x06,
  GP1_COMMAND_SET_VERTICAL_DISPLAY_RANGE = 0x07,
  GP1_COMMAND_SET_DISPLAY_MODE           = 0x08,
  GP1_COMMAND_SET_ALLOW_TEXTURE_DISABLE  = 0x09,
} gp1_command_t;

/* Drawing area: inclusive bounds, not exclusive. */
typedef struct {
  u32 left;
  u32 top;
  u32 right;
  u32 bottom;
} gpu_drawing_area_t;

typedef struct {
  s32 x;
  s32 y;
} gpu_drawing_offset_t;

typedef union {
  u32 bits;
} gpu_render_command_t;

ALWAYS_INLINE u32 gpu_render_command_color_for_first_vertex(gpu_render_command_t c)
{
  return c.bits & 0x00FFFFFFu;
}
ALWAYS_INLINE bool gpu_render_command_raw_texture_enable(gpu_render_command_t c)
{
  return (c.bits & (1u << 24)) != 0;
}
ALWAYS_INLINE bool gpu_render_command_transparency_enable(gpu_render_command_t c)
{
  return (c.bits & (1u << 25)) != 0;
}
ALWAYS_INLINE bool gpu_render_command_texture_enable(gpu_render_command_t c)
{
  return (c.bits & (1u << 26)) != 0;
}
ALWAYS_INLINE gpu_draw_rectangle_size_t gpu_render_command_rectangle_size(gpu_render_command_t c)
{
  return (gpu_draw_rectangle_size_t)((c.bits >> 27) & 0x3u);
}
ALWAYS_INLINE bool gpu_render_command_quad_polygon(gpu_render_command_t c)
{
  return (c.bits & (1u << 27)) != 0;
}
ALWAYS_INLINE bool gpu_render_command_polyline(gpu_render_command_t c)
{
  return (c.bits & (1u << 27)) != 0;
}
ALWAYS_INLINE bool gpu_render_command_shading_enable(gpu_render_command_t c)
{
  return (c.bits & (1u << 28)) != 0;
}
ALWAYS_INLINE gpu_primitive_t gpu_render_command_primitive(gpu_render_command_t c)
{
  return (gpu_primitive_t)((c.bits >> 29) & 0x3u);
}

ALWAYS_INLINE bool gpu_render_command_is_texturing_enabled(gpu_render_command_t c)
{
  return gpu_render_command_primitive(c) != GPU_PRIMITIVE_LINE
           ? gpu_render_command_texture_enable(c)
           : false;
}

ALWAYS_INLINE bool gpu_render_command_is_dithering_enabled(gpu_render_command_t c)
{
  switch (gpu_render_command_primitive(c)) {
    case GPU_PRIMITIVE_POLYGON:
      return gpu_render_command_shading_enable(c)
        || (gpu_render_command_texture_enable(c) && !gpu_render_command_raw_texture_enable(c));
    case GPU_PRIMITIVE_LINE:
      return true;
    case GPU_PRIMITIVE_RECTANGLE:
    case GPU_PRIMITIVE_RESERVED:
    default:
      return false;
  }
}

typedef union {
  u32 bits;
} gp1_set_display_mode_t;

ALWAYS_INLINE u8   gp1_set_display_mode_horizontal_resolution_1(gp1_set_display_mode_t m) { return (u8)(m.bits & 0x3u); }
ALWAYS_INLINE bool gp1_set_display_mode_vertical_resolution(gp1_set_display_mode_t m)     { return (m.bits & (1u << 2)) != 0; }
ALWAYS_INLINE bool gp1_set_display_mode_pal_mode(gp1_set_display_mode_t m)                { return (m.bits & (1u << 3)) != 0; }
ALWAYS_INLINE bool gp1_set_display_mode_display_area_color_depth(gp1_set_display_mode_t m){ return (m.bits & (1u << 4)) != 0; }
ALWAYS_INLINE bool gp1_set_display_mode_vertical_interlace(gp1_set_display_mode_t m)      { return (m.bits & (1u << 5)) != 0; }
ALWAYS_INLINE bool gp1_set_display_mode_horizontal_resolution_2(gp1_set_display_mode_t m) { return (m.bits & (1u << 6)) != 0; }
ALWAYS_INLINE bool gp1_set_display_mode_reverse_flag(gp1_set_display_mode_t m)            { return (m.bits & (1u << 7)) != 0; }

typedef union {
  u32 bits;
} gpustat_t;

ALWAYS_INLINE u8   gpustat_texture_page_x_base(gpustat_t s)          { return (u8)(s.bits & 0xFu); }
ALWAYS_INLINE u8   gpustat_texture_page_y_base(gpustat_t s)          { return (u8)((s.bits >> 4) & 0x1u); }
ALWAYS_INLINE gpu_transparency_mode_t gpustat_semi_transparency_mode(gpustat_t s)
{
  return (gpu_transparency_mode_t)((s.bits >> 5) & 0x3u);
}
ALWAYS_INLINE gpu_texture_mode_t gpustat_texture_color_mode(gpustat_t s)
{
  return (gpu_texture_mode_t)((s.bits >> 7) & 0x3u);
}
ALWAYS_INLINE bool gpustat_dither_enable(gpustat_t s)            { return (s.bits & (1u <<  9)) != 0; }
ALWAYS_INLINE bool gpustat_draw_to_displayed_field(gpustat_t s)  { return (s.bits & (1u << 10)) != 0; }
ALWAYS_INLINE bool gpustat_set_mask_while_drawing(gpustat_t s)   { return (s.bits & (1u << 11)) != 0; }
ALWAYS_INLINE bool gpustat_check_mask_before_draw(gpustat_t s)   { return (s.bits & (1u << 12)) != 0; }
ALWAYS_INLINE u8   gpustat_interlaced_field(gpustat_t s)         { return (u8)((s.bits >> 13) & 0x1u); }
ALWAYS_INLINE bool gpustat_reverse_flag(gpustat_t s)             { return (s.bits & (1u << 14)) != 0; }
ALWAYS_INLINE bool gpustat_texture_disable(gpustat_t s)          { return (s.bits & (1u << 15)) != 0; }
ALWAYS_INLINE u8   gpustat_horizontal_resolution_2(gpustat_t s)  { return (u8)((s.bits >> 16) & 0x1u); }
ALWAYS_INLINE u8   gpustat_horizontal_resolution_1(gpustat_t s)  { return (u8)((s.bits >> 17) & 0x3u); }
ALWAYS_INLINE bool gpustat_vertical_resolution(gpustat_t s)      { return (s.bits & (1u << 19)) != 0; }
ALWAYS_INLINE bool gpustat_pal_mode(gpustat_t s)                 { return (s.bits & (1u << 20)) != 0; }
ALWAYS_INLINE bool gpustat_display_area_color_depth_24(gpustat_t s) { return (s.bits & (1u << 21)) != 0; }
ALWAYS_INLINE bool gpustat_vertical_interlace(gpustat_t s)       { return (s.bits & (1u << 22)) != 0; }
ALWAYS_INLINE bool gpustat_display_disable(gpustat_t s)          { return (s.bits & (1u << 23)) != 0; }
ALWAYS_INLINE bool gpustat_interrupt_request(gpustat_t s)        { return (s.bits & (1u << 24)) != 0; }
ALWAYS_INLINE bool gpustat_dma_data_request(gpustat_t s)         { return (s.bits & (1u << 25)) != 0; }
ALWAYS_INLINE bool gpustat_gpu_idle(gpustat_t s)                 { return (s.bits & (1u << 26)) != 0; }
ALWAYS_INLINE bool gpustat_ready_to_send_vram(gpustat_t s)       { return (s.bits & (1u << 27)) != 0; }
ALWAYS_INLINE bool gpustat_ready_to_receive_dma(gpustat_t s)     { return (s.bits & (1u << 28)) != 0; }
ALWAYS_INLINE gpu_dma_direction_t gpustat_dma_direction(gpustat_t s)
{
  return (gpu_dma_direction_t)((s.bits >> 29) & 0x3u);
}
ALWAYS_INLINE bool gpustat_display_line_lsb(gpustat_t s)         { return (s.bits & (1u << 31)) != 0; }

ALWAYS_INLINE bool gpustat_is_masking_enabled(gpustat_t s)
{
  /* set_mask_while_drawing | check_mask_before_draw */
  const u32 mask = (1u << 11) | (1u << 12);
  return (s.bits & mask) != 0;
}
ALWAYS_INLINE bool gpustat_skip_drawing_to_active_field(gpustat_t s)
{
  const u32 mask   = (1u << 19) | (1u << 22) | (1u << 10);
  const u32 active = (1u << 19) | (1u << 22);
  return (s.bits & mask) == active;
}
ALWAYS_INLINE bool gpustat_in_interleaved_480i_mode(gpustat_t s)
{
  const u32 active = (1u << 19) | (1u << 22);
  return (s.bits & active) == active;
}

typedef union {
  u32 bits;
} gpu_vertex_position_t;

/* Sign-extend an 11-bit field. */
ALWAYS_INLINE s32 gpu_vertex_position_x(gpu_vertex_position_t p)
{
  s32 v = (s32)(p.bits & 0x7FFu);
  return (v & 0x400) ? (v - 0x800) : v;
}
ALWAYS_INLINE s32 gpu_vertex_position_y(gpu_vertex_position_t p)
{
  s32 v = (s32)((p.bits >> 16) & 0x7FFu);
  return (v & 0x400) ? (v - 0x800) : v;
}

#define GPU_DRAW_MODE_REG_MASK                    UINT16_C(0x1FFF)         /* 13 bits */
#define GPU_DRAW_MODE_REG_TEXTURE_MODE_AND_PAGE_MASK UINT16_C(0x019F)
/* Polygon texpage commands only affect bits 0..8, 11. */
#define GPU_DRAW_MODE_REG_POLYGON_TEXPAGE_MASK    UINT16_C(0x09FF)
/* Bits 0..5 latched into GPUSTAT at E1h / polygon draw time. */
#define GPU_DRAW_MODE_REG_GPUSTAT_MASK            UINT32_C(0x7FF)

typedef union {
  u16 bits;
} gpu_draw_mode_reg_t;

ALWAYS_INLINE u8   gpu_draw_mode_reg_texture_page(gpu_draw_mode_reg_t r)        { return (u8)(r.bits & 0x1Fu); }
ALWAYS_INLINE u8   gpu_draw_mode_reg_texture_page_x_base(gpu_draw_mode_reg_t r) { return (u8)(r.bits & 0x0Fu); }
ALWAYS_INLINE u8   gpu_draw_mode_reg_texture_page_y_base(gpu_draw_mode_reg_t r) { return (u8)((r.bits >> 4) & 0x1u); }
ALWAYS_INLINE gpu_transparency_mode_t gpu_draw_mode_reg_transparency_mode(gpu_draw_mode_reg_t r)
{
  return (gpu_transparency_mode_t)((r.bits >> 5) & 0x3u);
}
ALWAYS_INLINE gpu_texture_mode_t gpu_draw_mode_reg_texture_mode(gpu_draw_mode_reg_t r)
{
  return (gpu_texture_mode_t)((r.bits >> 7) & 0x3u);
}
ALWAYS_INLINE bool gpu_draw_mode_reg_dither_enable(gpu_draw_mode_reg_t r)           { return (r.bits & (1u <<  9)) != 0; }
ALWAYS_INLINE bool gpu_draw_mode_reg_draw_to_displayed_field(gpu_draw_mode_reg_t r) { return (r.bits & (1u << 10)) != 0; }
ALWAYS_INLINE bool gpu_draw_mode_reg_texture_disable(gpu_draw_mode_reg_t r)         { return (r.bits & (1u << 11)) != 0; }
ALWAYS_INLINE bool gpu_draw_mode_reg_texture_x_flip(gpu_draw_mode_reg_t r)          { return (r.bits & (1u << 12)) != 0; }
ALWAYS_INLINE bool gpu_draw_mode_reg_texture_y_flip(gpu_draw_mode_reg_t r)          { return (r.bits & (1u << 13)) != 0; }

ALWAYS_INLINE u32 gpu_draw_mode_reg_get_texture_page_base_x(gpu_draw_mode_reg_t r)
{
  return (u32)gpu_draw_mode_reg_texture_page_x_base(r) * 64u;
}
ALWAYS_INLINE u32 gpu_draw_mode_reg_get_texture_page_base_y(gpu_draw_mode_reg_t r)
{
  return (u32)gpu_draw_mode_reg_texture_page_y_base(r) * 256u;
}

/* True if texture mode is palette (Palette4Bit or Palette8Bit), i.e. bit 8 is 0. */
ALWAYS_INLINE bool gpu_draw_mode_reg_is_using_palette(gpu_draw_mode_reg_t r)
{
  return (r.bits & (2u << 7)) == 0;
}

#define GPU_TEXTURE_PALETTE_REG_MASK UINT16_C(0x7FFF)

typedef union {
  u16 bits;
} gpu_texture_palette_reg_t;

ALWAYS_INLINE u16 gpu_texture_palette_reg_x(gpu_texture_palette_reg_t p) { return (u16)(p.bits & 0x3Fu); }
ALWAYS_INLINE u16 gpu_texture_palette_reg_y(gpu_texture_palette_reg_t p) { return (u16)((p.bits >> 6) & 0x1FFu); }
ALWAYS_INLINE u32 gpu_texture_palette_reg_get_x_base(gpu_texture_palette_reg_t p)
{
  return (u32)gpu_texture_palette_reg_x(p) * 16u;
}
ALWAYS_INLINE u32 gpu_texture_palette_reg_get_y_base(gpu_texture_palette_reg_t p)
{
  return (u32)gpu_texture_palette_reg_y(p);
}

typedef union {
  struct {
    u8 and_x;
    u8 and_y;
    u8 or_x;
    u8 or_y;
  };
  u32 bits;
} gpu_texture_window_t;

ALWAYS_INLINE bool gpu_texture_window_equals(gpu_texture_window_t a, gpu_texture_window_t b)
{
  return a.bits == b.bits;
}

/* 4x4 dither matrix (Bayer-like). */
static const s32 DITHER_MATRIX[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE] = {
  {-4, +0, -3, +1},
  {+2, -2, +3, -1},
  {-3, +1, -4, +0},
  {+3, -1, +2, -2},
};

#endif /* CUPID_CORE_GPU_TYPES_H */
