/* cupid-ps1 rectangle texture traversal regressions. */
#include "tests/test_harness.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "core/gpu/gpu_backend.h"
#include <string.h>

void gpu_sw_rasterizer_scalar_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_simd_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);
void gpu_sw_rasterizer_avx2_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t*);

typedef void (*draw_rectangle_t)(const gpu_backend_draw_rectangle_cmd_t*);
static const draw_rectangle_t drawers[] = {gpu_sw_rasterizer_scalar_draw_rectangle,
                                           gpu_sw_rasterizer_simd_draw_rectangle,
                                           gpu_sw_rasterizer_avx2_draw_rectangle};
static u16                    before[VRAM_WIDTH * VRAM_HEIGHT], expected[VRAM_WIDTH * VRAM_HEIGHT];

static u16 sample_color(u32 index) {
  return (u16)(1u + index % 31u + ((index * 7u % 32u) << 5) + ((index * 11u % 32u) << 10) + ((index & 1u) << 15));
}

static u32 sample_index(u32 u, u32 v, u32 depth) {
  return depth == 0 ? (u + 3u * v) % 15u + 1u : (u + 3u * v) % 255u + 1u;
}

static void prepare_texture(u32 depth) {
  gpu_sw_rasterizer_init();
  for (u32 i = 0; i < VRAM_WIDTH * VRAM_HEIGHT; i++)
    g_vram[i] = sample_color(i);
  for (u32 i = 0; i < 256; i++)
    g_gpu_clut[i] = sample_color(i);
  for (u32 v = 0; v < 256; v++) {
    for (u32 u = 0; u < 256; u++) {
      const u32 index = sample_index(u, v, depth);
      u16*      word  = &g_vram[(256u + v) * VRAM_WIDTH + 640u + (depth == 0 ? u / 4 : depth == 1 ? u / 2 : u)];
      if (depth < 2) {
        const u32 shift = depth == 0 ? (u % 4) * 4 : (u % 2) * 8;
        const u32 mask  = depth == 0 ? 15u : 255u;
        *word           = (u16)((*word & ~(mask << shift)) | (index << shift));
      } else {
        *word = sample_color(index);
      }
    }
  }
  gpu_sw_drawing_area = (gpu_drawing_area_t){0, 0, VRAM_WIDTH - 1, VRAM_HEIGHT - 1};
  memcpy(before, g_vram, sizeof(before));
}

static gpu_backend_draw_rectangle_cmd_t rectangle(u32 depth, u32 flips, u32 uv) {
  gpu_backend_draw_rectangle_cmd_t cmd = {0};
  cmd.base.texture_enable              = true;
  cmd.base.raw_texture_enable          = true;
  cmd.base.draw_mode.bits              = (u16)(10u | 16u | (depth << 7) | (flips << 12));
  cmd.base.window.and_x = cmd.base.window.and_y = 255;
  cmd.x                                         = 32;
  cmd.y                                         = 24;
  cmd.width                                     = 17;
  cmd.height                                    = 5;
  cmd.texcoord                                  = (u16)(uv | uv << 8);
  cmd.color                                     = 0x80A060;
  return cmd;
}

TEST(GpuRectangleFlip, raw_coordinates_all_depths_and_paths) {
  const u32 origins[]    = {0, 1, 254, 255};
  const u32 widths[]     = {1, 3, 8, 17};
  u32       observations = 0, failures = 0;
  for (u32 depth = 0; depth < 4; depth++) {
    prepare_texture(depth);
    for (u32 flip = 0; flip < 4; flip++)
      for (u32 origin = 0; origin < 4; origin++)
        for (u32 width = 0; width < 4; width++) {
          gpu_backend_draw_rectangle_cmd_t cmd = rectangle(depth, flip, origins[origin]);
          cmd.width                            = (u16)widths[width];
          memcpy(expected, before, sizeof(expected));
          /* Explicit coordinate sequences cover even/odd initial U and byte wrap. */
          for (u32 y = 0; y < cmd.height; y++)
            for (u32 x = 0; x < cmd.width; x++) {
              const u8 u = (u8)((flip & 1) ? ((origins[origin] | 1u) + 256u - x) : origins[origin] + x);
              const u8 v = (u8)((flip & 2) ? (origins[origin] + 256u - y) : origins[origin] + y);
              expected[((u32)cmd.y + y) * VRAM_WIDTH + (u32)cmd.x + x] = sample_color(sample_index(u, v, depth));
            }
          for (u32 path = 0; path < 3; path++) {
            memcpy(g_vram, before, sizeof(before));
            drawers[path](&cmd);
            const bool equal = memcmp(g_vram, expected, sizeof(expected)) == 0;
            if (!equal && failures < 8)
              printf("rectangle flip depth=%u flip=%u uv=%u width=%u path=%u\n", depth, flip, origins[origin],
                     cmd.width, path);
            failures += !equal;
            observations++;
          }
        }
  }
  printf("rectangle flip observations=%u failures=%u\n", observations, failures);
  EXPECT_EQ(observations, 768);
  EXPECT_EQ(failures, 0);
}

TEST(GpuRectangleFlip, clipping_window_blend_mask_and_interlace) {
  u32 observations = 0, failures = 0;
  for (u32 depth = 0; depth < 3; depth++) {
    prepare_texture(depth);
    for (u32 flip = 0; flip < 4; flip++)
      for (u32 options = 0; options < 32; options++) {
        gpu_backend_draw_rectangle_cmd_t cmd = rectangle(depth, flip, (options & 1) ? 254 : 0);
        cmd.base.raw_texture_enable          = (options & 1) != 0;
        cmd.base.transparency_enable         = (options & 2) != 0;
        cmd.base.set_mask_while_drawing      = (options & 4) != 0;
        cmd.base.check_mask_before_draw      = (options & 8) != 0;
        cmd.base.interlaced_rendering        = (options & 16) != 0;
        cmd.base.active_line_lsb             = options & 1;
        cmd.base.draw_mode.bits |= (u16)((options % 4) << 5);
        cmd.base.window.and_x = 0xE7;
        cmd.base.window.or_x  = 8;
        cmd.base.window.and_y = 0xF7;
        cmd.base.window.or_y  = 8;
        gpu_sw_drawing_area   = (gpu_drawing_area_t){35, 25, 46, 27};
        memcpy(g_vram, before, sizeof(before));
        /* Independently shade addressed texels as ordinary unflipped 1x1 rectangles. */
        gpu_backend_draw_rectangle_cmd_t pixel = cmd;
        pixel.base.draw_mode.bits &= (u16)~0x3000u;
        pixel.width = pixel.height = 1;
        for (u32 y = 0; y < cmd.height; y++)
          for (u32 x = 0; x < cmd.width; x++) {
            const u32 origin = (u8)cmd.texcoord;
            const u8  u      = (u8)((flip & 1) ? ((origin | 1u) + 256u - x) : origin + x);
            const u8  v      = (u8)((flip & 2) ? (origin + 256u - y) : origin + y);
            pixel.x          = (s16)(cmd.x + x);
            pixel.y          = (s16)(cmd.y + y);
            pixel.texcoord   = (u16)(u | (u32)v << 8);
            gpu_sw_rasterizer_scalar_draw_rectangle(&pixel);
          }
        memcpy(expected, g_vram, sizeof(expected));
        for (u32 path = 0; path < 3; path++) {
          memcpy(g_vram, before, sizeof(before));
          drawers[path](&cmd);
          failures += memcmp(g_vram, expected, sizeof(expected)) != 0;
          observations++;
        }
      }
  }
  printf("rectangle flip shading observations=%u failures=%u\n", observations, failures);
  EXPECT_EQ(observations, 1152);
  EXPECT_EQ(failures, 0);
}

TEST(GpuRectangleFlip, untextured_bits_do_not_change_pixels) {
  prepare_texture(2);
  for (u32 path = 0; path < 3; path++)
    for (u32 flip = 0; flip < 4; flip++) {
      gpu_backend_draw_rectangle_cmd_t cmd = rectangle(2, flip, 0);
      cmd.base.texture_enable              = false;
      memcpy(g_vram, before, sizeof(before));
      drawers[path](&cmd);
      memcpy(expected, g_vram, sizeof(expected));
      cmd.base.draw_mode.bits &= (u16)~0x3000u;
      memcpy(g_vram, before, sizeof(before));
      drawers[path](&cmd);
      EXPECT_EQ(memcmp(g_vram, expected, sizeof(expected)), 0);
    }
}
