/* cupid-ps1 polyline packet and pending-command regressions. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"

static void setup_polyline(display_fixture_t* fixture)
{
  begin_display(fixture);
  memset(g_vram, 0, sizeof(g_vram));
  gpu_write_register(0, 0xE3000000);
  gpu_write_register(0, 0xE407FFFF);
  gpu_write_register(0, 0x480000FF);
  gpu_write_register(0, 32 | 24 << 16);
  gpu_write_register(0, 40 | 24 << 16);
  gpu_write_register(0, 40 | 32 << 16);
  run_slice(2000);
}

TEST(GpuPolyline, fresh_control)
{
  display_fixture_t fixture;
  setup_polyline(&fixture);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 31);
  EXPECT_EQ(g_vram[32 * VRAM_WIDTH + 40], 31);
  end_display(&fixture);
}

TEST(GpuPolyline, minimum_two_vertices)
{
  display_fixture_t fixture;
  begin_display(&fixture);
  memset(g_vram, 0, sizeof(g_vram));
  gpu_write_register(0, 0xE3000000);
  gpu_write_register(0, 0xE407FFFF);
  gpu_write_register(0, 0x480000FF);
  gpu_write_register(0, 32 | 24 << 16);
  gpu_write_register(0, 40 | 24 << 16);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  printf("two-vertex polyline first=%04X last=%04X\n", g_vram[24 * VRAM_WIDTH + 32], g_vram[24 * VRAM_WIDTH + 40]);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 31);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 40], 31);
  end_display(&fixture);
}

TEST(GpuPolyline, restored_pending_vertices)
{
  display_fixture_t fixture;
  setup_polyline(&fixture);
  const size_t capacity = 2 * 1024 * 1024;
  u8*          bytes    = malloc(capacity);
  EXPECT_NOT_NULL(bytes);
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  /* Finish the pending primitive so the live buffer no longer supplies it. */
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 31);
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 0);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  printf("pending polyline restored first=%04X last=%04X\n", g_vram[24 * VRAM_WIDTH + 32],
         g_vram[32 * VRAM_WIDTH + 40]);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 31);
  EXPECT_EQ(g_vram[32 * VRAM_WIDTH + 40], 31);
  free(bytes);
  end_display(&fixture);
}

TEST(GpuPolyline, reset_discards_pending_vertices)
{
  display_fixture_t fixture;
  setup_polyline(&fixture);
  gpu_write_register(4, 0);
  gpu_write_register(0, 0xE3000000);
  gpu_write_register(0, 0xE407FFFF);
  gpu_write_register(0, 0x4800FF00);
  gpu_write_register(0, 64 | 24 << 16);
  gpu_write_register(0, 72 | 24 << 16);
  gpu_write_register(0, 72 | 32 << 16);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  printf("reset polyline old=%04X new=%04X\n", g_vram[24 * VRAM_WIDTH + 32], g_vram[24 * VRAM_WIDTH + 64]);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 0);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 64], 0x3E0);
  end_display(&fixture);
}

TEST(GpuPolyline, all_six_hundred_vertices_render)
{
  display_fixture_t fixture;
  begin_display(&fixture);
  memset(g_vram, 0, sizeof(g_vram));
  gpu_write_register(0, 0xE3000000);
  gpu_write_register(0, 0xE407FFFF);
  gpu_write_register(0, 0x480000FF);
  for (u32 i = 0; i < 600; i++)
    gpu_write_register(0, (32 + i % 16) | (24 + i / 16) << 16);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  printf("long polyline first=%04X final=%04X\n", g_vram[24 * VRAM_WIDTH + 32], g_vram[61 * VRAM_WIDTH + 39]);
  EXPECT_EQ(g_vram[61 * VRAM_WIDTH + 39], 31);
  end_display(&fixture);
}

TEST(GpuPolyline, clear_fifo_discards_pending_vertices)
{
  display_fixture_t fixture;
  setup_polyline(&fixture);
  gpu_write_register(4, 0x01000000);
  gpu_write_register(0, 0x4800FF00);
  gpu_write_register(0, 64 | 24 << 16);
  gpu_write_register(0, 72 | 24 << 16);
  gpu_write_register(0, 72 | 32 << 16);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 0);
  EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 64], 0x3E0);
  end_display(&fixture);
}

TEST(GpuPolyline, terminator_pattern_in_required_position)
{
  for (u32 shaded = 0; shaded < 2; shaded++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    memset(g_vram, 0, sizeof(g_vram));
    gpu_write_register(0, 0xE3000000);
    gpu_write_register(0, 0xE407FFFF);
    gpu_write_register(0, (shaded ? 0x58000000 : 0x48000000) | 255);
    gpu_write_register(0, 32 | 24 << 16);
    if (shaded) gpu_write_register(0, 255);
    gpu_write_register(0, 0x50005000); /* Required second position decodes to (0, 0). */
    gpu_write_register(0, 0x50005000); /* First terminator slot. */
    gpu_write_register(0, 0x1F000000);
    run_slice(2000);
    EXPECT_EQ(g_vram[0], 31);
    EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    end_display(&fixture);
  }
}

TEST(GpuPolyline, shaded_position_is_not_a_terminator)
{
  display_fixture_t fixture;
  setup_polyline(&fixture);
  gpu_write_register(4, 0x01000000);
  gpu_write_register(0, 0x580000FF);
  gpu_write_register(0, 32 | 24 << 16);
  gpu_write_register(0, 255);
  gpu_write_register(0, 40 | 24 << 16);
  gpu_write_register(0, 0x0000FF00);
  gpu_write_register(0, 0x50005000); /* Third position, after a complete color word. */
  gpu_write_register(0, 0x50005000); /* Terminator in the next color slot. */
  gpu_write_register(0, 0x1F000000);
  run_slice(2000);
  EXPECT_EQ(g_vram[0], 0x3E0);
  EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
  end_display(&fixture);
}
