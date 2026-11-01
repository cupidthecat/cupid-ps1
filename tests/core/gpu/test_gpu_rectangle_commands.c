/* cupid-ps1 draw-mode latching and rectangle packet regressions. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"

static gpu_backend_draw_rectangle_cmd_t captured_rectangle;
static u32 rectangle_count;

static void capture_rectangle(gpu_backend_t* backend, const gpu_backend_draw_rectangle_cmd_t* command)
{
  (void)backend;
  captured_rectangle = *command;
  rectangle_count++;
}

static void restore_packet(void)
{
  const size_t capacity = 2 * 1024 * 1024;
  u8* bytes = malloc(capacity);
  EXPECT_NOT_NULL(bytes);
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  /* Discard the live FIFO and mode before reading their saved values. */
  gpu_write_register(4, 0);
  gpu_write_register(0, 0xE1000000);
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  free(bytes);
}

static void check_rectangle_commands(bool restore)
{
  u32 observations = 0, failures = 0;
  for (u32 flip = 0; flip < 4; flip++) for (u32 size = 0; size < 4; size++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    fixture.capture.draw_rectangle = capture_rectangle;
    rectangle_count = 0;
    gpu_write_register(0, 0xE3000000);
    gpu_write_register(0, 0xE407FFFF);
    gpu_write_register(0, 0xE1000000 | flip << 12);
    /* A textured triangle updates only its texture-page fields. */
    const u32 polygon[] = {0x25808080, 0, 0, 8, 0x01000008, 8 << 16, 8 << 8};
    for (u32 i = 0; i < sizeof(polygon) / sizeof(polygon[0]); i++) gpu_write_register(0, polygon[i]);
    gpu_write_register(0, 0x65808080 | size << 27);
    gpu_write_register(0, 32 | 24 << 16);
    if (restore) restore_packet();
    gpu_write_register(0, 0x0012FFFE);
    if (size == 0) gpu_write_register(0, 17 | 5 << 16);
    run_slice(2000);
    EXPECT_EQ(rectangle_count, 1);
    const u32 width = size == 0 ? 17 : size == 1 ? 1 : size == 2 ? 8 : 16;
    const u32 height = size == 0 ? 5 : width;
    const bool equal = captured_rectangle.base.draw_mode.bits == (0x0100u | flip << 12) &&
                       captured_rectangle.width == width && captured_rectangle.height == height &&
                       captured_rectangle.texcoord == 0xFFFE && captured_rectangle.x == 32 && captured_rectangle.y == 24;
    if (!equal) printf("rectangle packet restore=%u flip=%u size=%u mode=%04X expected=%04X\n", restore,
                       flip, size, captured_rectangle.base.draw_mode.bits, 0x0100u | flip << 12);
    failures += !equal; observations++;
    gpu_write_register(4, 0);
    gpu_write_register(0, 0x6D808080); gpu_write_register(0, 0); gpu_write_register(0, 0);
    run_slice(2000);
    EXPECT_EQ(rectangle_count, 2);
    EXPECT_EQ(captured_rectangle.base.draw_mode.bits, 0);
    end_display(&fixture);
  }
  printf("rectangle packets restore=%u observations=%u failures=%u\n", restore, observations, failures);
  EXPECT_EQ(observations, 16); EXPECT_EQ(failures, 0);
}

TEST(GpuRectangleCommands, draw_mode_and_fixed_sizes)
{
  check_rectangle_commands(false);
}

TEST(GpuRectangleCommands, restored_incomplete_packets)
{
  check_rectangle_commands(true);
}
