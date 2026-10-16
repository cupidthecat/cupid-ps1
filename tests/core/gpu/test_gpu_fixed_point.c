/* cupid-ps1: signed coordinates and wrapping fixed-point attribute regressions. */
#include "tests/test_harness.h"

#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"

void gpu_sw_rasterizer_scalar_draw_line(const gpu_backend_draw_cmd_t*, const gpu_backend_line_vertex_t*,
                                        const gpu_backend_line_vertex_t*);
void gpu_sw_rasterizer_scalar_draw_triangle(const gpu_backend_draw_cmd_t*, const gpu_backend_polygon_vertex_t*,
                                            const gpu_backend_polygon_vertex_t*, const gpu_backend_polygon_vertex_t*);
void gpu_sw_rasterizer_simd_draw_triangle(const gpu_backend_draw_cmd_t*, const gpu_backend_polygon_vertex_t*,
                                          const gpu_backend_polygon_vertex_t*, const gpu_backend_polygon_vertex_t*);

static u16 s_expected[VRAM_WIDTH * VRAM_HEIGHT];

static void reset_drawing(void)
{
  gpu_sw_rasterizer_init();
  gpu_sw_drawing_area.left = gpu_sw_drawing_area.top = 0;
  gpu_sw_drawing_area.right                          = VRAM_WIDTH - 1;
  gpu_sw_drawing_area.bottom                         = VRAM_HEIGHT - 1;
  for (u32 i = 0; i < VRAM_WIDTH * VRAM_HEIGHT; i++)
    g_vram[i] = 0x8000;
}

TEST(GpuFixedPoint, clipped_line_with_negative_slopes_and_colors)
{
  reset_drawing();
  gpu_backend_draw_cmd_t cmd        = {0};
  cmd.shading_enable                = true;
  const gpu_backend_line_vertex_t a = {.x = -2, .y = 4, .color = 0x00FF80F0};
  const gpu_backend_line_vertex_t b = {.x = 6, .y = 0, .color = 0x00102030};
  gpu_sw_rasterizer_scalar_draw_line(&cmd, &a, &b);
  /* RGB (48, 32, 16) at the final endpoint becomes RGB555 (6, 4, 2). */
  EXPECT_EQ(0x0886, g_vram[6]);
  EXPECT_EQ(0x8000, g_vram[7 * VRAM_WIDTH + 7]);
  memcpy(s_expected, g_vram, sizeof(s_expected));
  reset_drawing();
  gpu_sw_rasterizer_scalar_draw_line(&cmd, &b, &a);
  EXPECT_EQ(0, memcmp(s_expected, g_vram, sizeof(s_expected)));
}

TEST(GpuFixedPoint, negative_triangle_coordinates_all_vertex_orders)
{
  const gpu_backend_polygon_vertex_t vertices[] = {
      {.x = -4, .y = -4, .color = 0x00FFFFFF},
      {.x = 8, .y = -4, .color = 0x00FFFFFF},
      {.x = -4, .y = 8, .color = 0x00FFFFFF},
  };
  const u8               orders[][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  gpu_backend_draw_cmd_t cmd         = {0};
  for (u32 i = 0; i < 6; i++) {
    reset_drawing();
    gpu_sw_rasterizer_scalar_draw_triangle(&cmd, &vertices[orders[i][0]], &vertices[orders[i][1]],
                                           &vertices[orders[i][2]]);
    EXPECT_EQ(0x7FFF, g_vram[0]);
    EXPECT_EQ(0x8000, g_vram[7 * VRAM_WIDTH + 7]);
    if (i == 0)
      memcpy(s_expected, g_vram, sizeof(s_expected));
    else
      EXPECT_EQ(0, memcmp(s_expected, g_vram, sizeof(s_expected)));
    reset_drawing();
    gpu_sw_rasterizer_simd_draw_triangle(&cmd, &vertices[orders[i][0]], &vertices[orders[i][1]],
                                         &vertices[orders[i][2]]);
    EXPECT_EQ(0, memcmp(s_expected, g_vram, sizeof(s_expected)));
  }
}

TEST(GpuFixedPoint, clipped_triangle_wraps_attribute_offsets)
{
  const gpu_backend_polygon_vertex_t a   = {.x = -32, .y = -16, .color = 0x000000FF};
  const gpu_backend_polygon_vertex_t b   = {.x = 128, .y = 8, .color = 0x0000FF00};
  const gpu_backend_polygon_vertex_t c   = {.x = 16, .y = 96, .color = 0x00FF0000};
  gpu_backend_draw_cmd_t             cmd = {0};
  cmd.shading_enable                     = true;
  reset_drawing();
  gpu_sw_rasterizer_scalar_draw_triangle(&cmd, &a, &b, &c);
  EXPECT_NE(0x8000, g_vram[16 * VRAM_WIDTH + 16]);
  memcpy(s_expected, g_vram, sizeof(s_expected));
  reset_drawing();
  gpu_sw_rasterizer_simd_draw_triangle(&cmd, &a, &b, &c);
  EXPECT_EQ(0, memcmp(s_expected, g_vram, sizeof(s_expected)));
}
