/* cupid-ps1 complete polyline submission, color carry and staging bounds. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/cpu/cpu_pgxp.h"

static u32  segments, submissions;
static bool shaded_vertices;

static u32 point(u32 i)
{
  return (32u + (i % 32u) * 3u) | ((24u + (i / 32u % 100u) * 3u) << 16);
}

static u32 color(u32 i)
{
  if (!shaded_vertices) i = 0;
  return ((i * 13u + 17u) & 255u) | (((i * 7u + 31u) & 255u) << 8) | (((i * 3u + 61u) & 255u) << 16);
}

static void check_vertex(s32 x, s32 y, u32 rgb, u32 index)
{
  EXPECT_EQ(x, point(index) & 0xFFFFu);
  EXPECT_EQ(y, point(index) >> 16);
  EXPECT_EQ(rgb, color(index));
}

static void capture_lines(gpu_backend_t* backend, const gpu_backend_draw_line_cmd_t* command,
                          const gpu_backend_line_vertex_t* vertices)
{
  (void)backend;
  EXPECT_TRUE(command->base.num_vertices <= 2048);
  EXPECT_EQ(command->base.num_vertices % 2, 0);
  for (u32 i = 0; i < command->base.num_vertices; i += 2) {
    check_vertex(vertices[i].x, vertices[i].y, vertices[i].color, segments);
    check_vertex(vertices[i + 1].x, vertices[i + 1].y, vertices[i + 1].color, segments + 1);
    segments++;
  }
  submissions++;
}

static void capture_precise_lines(gpu_backend_t* backend, const gpu_backend_draw_precise_line_cmd_t* command,
                                  const gpu_backend_precise_line_vertex_t* vertices)
{
  (void)backend;
  EXPECT_TRUE(command->base.num_vertices <= 2048);
  EXPECT_EQ(command->base.num_vertices % 2, 0);
  for (u32 i = 0; i < command->base.num_vertices; i += 2) {
    check_vertex(vertices[i].native_x, vertices[i].native_y, vertices[i].color, segments);
    check_vertex(vertices[i + 1].native_x, vertices[i + 1].native_y, vertices[i + 1].color, segments + 1);
    EXPECT_EQ(vertices[i].w, 1);
    EXPECT_EQ(vertices[i + 1].w, 1);
    segments++;
  }
  submissions++;
}

static void restore_pending(void)
{
  const size_t capacity = 2 * 1024 * 1024;
  u8*          before   = malloc(capacity);
  u8*          after    = malloc(capacity);
  EXPECT_NOT_NULL(before);
  EXPECT_NOT_NULL(after);
  state_wrapper_t state;
  state_wrapper_init_write(&state, before, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  gpu_write_register(4, 0);
  state_wrapper_init_read(&state, before, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  state_wrapper_init_write(&state, after, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(memcmp(before, after, size), 0);
  free(before);
  free(after);
}

static void write_vertex_word(u32 index, u32 value)
{
  if (index % 3 == 0) {
    gpu_dma_write(0x1000 + index * 4, value);
    gpu_end_dma_write();
  } else {
    gpu_write_register(0, value);
  }
}

static void check_batches(bool precise, bool shaded, bool restore)
{
  const u32 counts[] = {2, 3, 513, 1025, 2048, 2049, 4096, 4097, 9000};
  for (u32 sample = 0; sample < sizeof(counts) / sizeof(counts[0]); sample++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    fixture.capture.draw_line         = capture_lines;
    fixture.capture.draw_precise_line = capture_precise_lines;
    g_settings.gpu_pgxp_enable        = precise;
    g_settings.gpu_pgxp_vertex_cache  = false;
    if (precise) pgxp_initialize();
    shaded_vertices = shaded;
    segments = submissions = 0;
    gpu_write_register(4, 0x04000002);
    gpu_write_register(0, (shaded ? 0x58000000u : 0x48000000u) | color(0));
    gpu_write_register(0, point(0));
    for (u32 i = 1; i < counts[sample]; i++) {
      if (shaded) write_vertex_word(i * 2, color(i));
      if (restore && i == counts[sample] - 1) restore_pending();
      write_vertex_word(i * 2 + 1, point(i));
    }
    gpu_write_register(0, 0x50005000);
    run_slice(2000);
    printf("polyline submission precise=%u shaded=%u points=%u segments=%u batches=%u\n", precise, shaded,
           counts[sample], segments, submissions);
    EXPECT_EQ(segments, counts[sample] - 1);
    EXPECT_TRUE(submissions > 0);
    /* The terminator must leave the following packet available to the parser. */
    gpu_write_register(0, 0x1F000000);
    run_slice(2000);
    EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    if (precise) pgxp_shutdown();
    end_display(&fixture);
  }
}

TEST(GpuPolylineBatches, flat)
{
  check_batches(false, false, false);
}
TEST(GpuPolylineBatches, shaded)
{
  check_batches(false, true, false);
}
TEST(GpuPolylineBatches, precise_flat)
{
  check_batches(true, false, false);
}
TEST(GpuPolylineBatches, precise_shaded)
{
  check_batches(true, true, false);
}
TEST(GpuPolylineBatches, restored_flat)
{
  check_batches(false, false, true);
  check_batches(true, false, true);
}
TEST(GpuPolylineBatches, restored_shaded)
{
  check_batches(false, true, true);
  check_batches(true, true, true);
}
