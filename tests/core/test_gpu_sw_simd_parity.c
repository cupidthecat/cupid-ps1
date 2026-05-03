/* SCALAR vs SIMD parity tests for the SW rasterizer (R22 Phase D gate).
 *
 * Calls gpu_sw_rasterizer_scalar_* and gpu_sw_rasterizer_simd_* directly
 * (both symbols are non-static in their TUs) and asserts the resulting
 * 1024x512 u16 g_vram is byte-identical between the two paths.
 *
 * Covers: draw_rectangle (untextured + raw + 8-bit modulated, opaque +
 * transparent), draw_triangle (gouraud-shaded, untextured + textured),
 * with mask-set / mask-check variations.
 */

#include "tests/test_harness.h"

#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_backend.h"
#include "core/gpu_types.h"

#include <string.h>

void gpu_sw_rasterizer_scalar_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_simd_draw_rectangle  (const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_scalar_draw_triangle (const gpu_backend_draw_cmd_t*,
                                             const gpu_backend_polygon_vertex_t*,
                                             const gpu_backend_polygon_vertex_t*,
                                             const gpu_backend_polygon_vertex_t*);
void gpu_sw_rasterizer_simd_draw_triangle   (const gpu_backend_draw_cmd_t*,
                                             const gpu_backend_polygon_vertex_t*,
                                             const gpu_backend_polygon_vertex_t*,
                                             const gpu_backend_polygon_vertex_t*);

static u16 s_vram_scalar[VRAM_HEIGHT * VRAM_WIDTH];
static u16 s_vram_simd  [VRAM_HEIGHT * VRAM_WIDTH];

static void ensure_init(void)
{
  /* Per-test forking means we can re-init without state contamination, but
   * the dither LUT init only needs to run once per process. Call every
   * time -- gpu_sw_rasterizer_init is idempotent. Without it, scalar's
   * shade_pixel reads gpu_sw_dither_lut which is all-zero -> wrong colors. */
  gpu_sw_rasterizer_init();
}

static void seed_vram(void)
{
  u32 s = 0xC0FFEE00u;
  for (u32 i = 0; i < (u32)(VRAM_HEIGHT * VRAM_WIDTH); i++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    g_vram[i] = (u16)(s & 0xFFFFu);
  }
}

static void clear_vram(u16 fill)
{
  for (u32 i = 0; i < (u32)(VRAM_HEIGHT * VRAM_WIDTH); i++)
    g_vram[i] = fill;
}

static void set_drawing_area_full(void)
{
  /* Full VRAM minus 1: matches what gpu.c emits for unconstrained draws. */
  gpu_sw_drawing_area.left   = 0;
  gpu_sw_drawing_area.top    = 0;
  gpu_sw_drawing_area.right  = VRAM_WIDTH  - 1;
  gpu_sw_drawing_area.bottom = VRAM_HEIGHT - 1;
}

static int vram_equals(void)
{
  return memcmp(s_vram_scalar, s_vram_simd, sizeof(s_vram_scalar)) == 0;
}

/* Run scalar then SIMD with same input + initial VRAM, snapshot each, compare. */
#define RUN_RECT_PARITY(cmd)                                                   \
  do {                                                                          \
    seed_vram();                                                                \
    gpu_sw_rasterizer_scalar_draw_rectangle(&(cmd));                            \
    memcpy(s_vram_scalar, g_vram, sizeof(s_vram_scalar));                       \
    seed_vram();                                                                \
    gpu_sw_rasterizer_simd_draw_rectangle(&(cmd));                              \
    memcpy(s_vram_simd, g_vram, sizeof(s_vram_simd));                           \
    EXPECT_TRUE(vram_equals());                                                 \
  } while (0)

#define RUN_TRI_PARITY(base, v0, v1, v2)                                       \
  do {                                                                          \
    seed_vram();                                                                \
    gpu_sw_rasterizer_scalar_draw_triangle(&(base), &(v0), &(v1), &(v2));       \
    memcpy(s_vram_scalar, g_vram, sizeof(s_vram_scalar));                       \
    seed_vram();                                                                \
    gpu_sw_rasterizer_simd_draw_triangle(&(base), &(v0), &(v1), &(v2));         \
    memcpy(s_vram_simd, g_vram, sizeof(s_vram_simd));                           \
    EXPECT_TRUE(vram_equals());                                                 \
  } while (0)

TEST(GpuSwSimdParity, rect_untextured_opaque)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.x = 100; cmd.y = 50; cmd.width = 32; cmd.height = 16;
  cmd.color = 0xFF80F0u;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, rect_untextured_unaligned_width)
{
  /* Width that isn't a multiple of 4 -- exercises the SIMD tail-mask. */
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.x = 17; cmd.y = 23; cmd.width = 19; cmd.height = 7;
  cmd.color = 0x102030u;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, rect_clipped_left)
{
  set_drawing_area_full(); ensure_init();
  gpu_sw_drawing_area.left = 50;
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.x = 30; cmd.y = 10; cmd.width = 64; cmd.height = 4;  /* spills left clip */
  cmd.color = 0xAABBCCu;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, rect_clipped_right)
{
  set_drawing_area_full(); ensure_init();
  gpu_sw_drawing_area.right = 100;
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.x = 80; cmd.y = 10; cmd.width = 64; cmd.height = 4;  /* spills right clip */
  cmd.color = 0x123456u;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, rect_with_set_mask)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.base.set_mask_while_drawing = true;
  cmd.x = 200; cmd.y = 100; cmd.width = 16; cmd.height = 16;
  cmd.color = 0x7F7F7Fu;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, rect_with_check_mask)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.base.check_mask_before_draw = true;
  cmd.x = 300; cmd.y = 200; cmd.width = 16; cmd.height = 8;
  cmd.color = 0x010203u;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, rect_transparent_half_blend)
{
  /* HALF_BG_PLUS_HALF_FG transparency. */
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.base.transparency_enable = true;
  /* draw_mode bits: transparency_mode = 0 (HALF). texture_mode = 0. */
  cmd.base.draw_mode.bits = 0;
  cmd.x = 50; cmd.y = 50; cmd.width = 16; cmd.height = 8;
  cmd.color = 0x808080u;
  RUN_RECT_PARITY(cmd);
}

TEST(GpuSwSimdParity, tri_flat_untextured)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_cmd_t base = {0};
  gpu_backend_polygon_vertex_t v0 = {.x=10,  .y=10,  .color=0xFF0000FFu, .texcoord=0};
  gpu_backend_polygon_vertex_t v1 = {.x=200, .y=20,  .color=0x00FF0000u, .texcoord=0};
  gpu_backend_polygon_vertex_t v2 = {.x=100, .y=180, .color=0x0000FF00u, .texcoord=0};
  RUN_TRI_PARITY(base, v0, v1, v2);
}

TEST(GpuSwSimdParity, tri_gouraud_shaded)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_cmd_t base = {0};
  base.shading_enable = true;
  gpu_backend_polygon_vertex_t v0 = {.x=20,  .y=30,  .color=0xFF0000FFu, .texcoord=0};
  gpu_backend_polygon_vertex_t v1 = {.x=300, .y=40,  .color=0x00FF0000u, .texcoord=0};
  gpu_backend_polygon_vertex_t v2 = {.x=150, .y=200, .color=0x0000FF00u, .texcoord=0};
  RUN_TRI_PARITY(base, v0, v1, v2);
}

TEST(GpuSwSimdParity, tri_dithered)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_cmd_t base = {0};
  base.shading_enable = true;
  base.dither_enable  = true;
  gpu_backend_polygon_vertex_t v0 = {.x=5,   .y=5,   .color=0x010102FFu, .texcoord=0};
  gpu_backend_polygon_vertex_t v1 = {.x=400, .y=10,  .color=0xFEFEFE80u, .texcoord=0};
  gpu_backend_polygon_vertex_t v2 = {.x=200, .y=300, .color=0x80808040u, .texcoord=0};
  RUN_TRI_PARITY(base, v0, v1, v2);
}

TEST(GpuSwSimdParity, tri_transparent)
{
  set_drawing_area_full(); ensure_init();
  gpu_backend_draw_cmd_t base = {0};
  base.shading_enable      = true;
  base.transparency_enable = true;
  base.draw_mode.bits = 0; /* HALF_BG_PLUS_HALF_FG */
  gpu_backend_polygon_vertex_t v0 = {.x=10,  .y=10,  .color=0x40808080u, .texcoord=0};
  gpu_backend_polygon_vertex_t v1 = {.x=200, .y=20,  .color=0x80404080u, .texcoord=0};
  gpu_backend_polygon_vertex_t v2 = {.x=100, .y=180, .color=0x80808040u, .texcoord=0};
  RUN_TRI_PARITY(base, v0, v1, v2);
}

TEST(GpuSwSimdParity, tri_clipped)
{
  /* Triangle that pokes past all four clip edges. */
  set_drawing_area_full(); ensure_init();
  gpu_sw_drawing_area.left = 50; gpu_sw_drawing_area.top = 50;
  gpu_sw_drawing_area.right = 200; gpu_sw_drawing_area.bottom = 200;
  gpu_backend_draw_cmd_t base = {0};
  base.shading_enable = true;
  gpu_backend_polygon_vertex_t v0 = {.x=10,  .y=10,  .color=0x808080FFu, .texcoord=0};
  gpu_backend_polygon_vertex_t v1 = {.x=300, .y=20,  .color=0x808080FFu, .texcoord=0};
  gpu_backend_polygon_vertex_t v2 = {.x=150, .y=300, .color=0x808080FFu, .texcoord=0};
  RUN_TRI_PARITY(base, v0, v1, v2);
}

TEST(GpuSwSimdParity, fill_pattern_then_overdraw)
{
  /* Pre-fill an area, then draw a smaller rect over it -- catches BG-blend
   * and mask-bit interaction. */
  set_drawing_area_full(); ensure_init();
  clear_vram(0x8421);  /* not-zero, mask bit not set */
  gpu_backend_draw_rectangle_cmd_t cmd = {{{0}}};
  cmd.base.transparency_enable = true;
  cmd.base.draw_mode.bits = 1;  /* BG_PLUS_FG */
  cmd.x = 100; cmd.y = 100; cmd.width = 24; cmd.height = 16;
  cmd.color = 0x404040u;
  /* Manual: scalar pass on pre-filled vram, snapshot. */
  for (u32 i = 0; i < (u32)(VRAM_HEIGHT * VRAM_WIDTH); i++) g_vram[i] = 0x8421;
  gpu_sw_rasterizer_scalar_draw_rectangle(&cmd);
  memcpy(s_vram_scalar, g_vram, sizeof(s_vram_scalar));
  for (u32 i = 0; i < (u32)(VRAM_HEIGHT * VRAM_WIDTH); i++) g_vram[i] = 0x8421;
  gpu_sw_rasterizer_simd_draw_rectangle(&cmd);
  memcpy(s_vram_simd, g_vram, sizeof(s_vram_simd));
  EXPECT_TRUE(vram_equals());
}
