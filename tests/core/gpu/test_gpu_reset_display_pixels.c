/* cupid-ps1 software scanout after GPU reset. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"

typedef struct {
  u32  width, height;
  u32* pixels;
} reset_frame_t;

static reset_frame_t read_reset_frame(void)
{
  gpu_update_display(false);
  reset_frame_t frame;
  const u32*    pixels = gpu_sw_get_display_buffer(&frame.width, &frame.height);
  EXPECT_NOT_NULL(pixels);
  EXPECT_TRUE(frame.width > 0 && frame.width <= 1024);
  EXPECT_TRUE(frame.height > 0 && frame.height <= 1024);
  const size_t bytes = (size_t)frame.width * frame.height * sizeof(u32);
  frame.pixels       = malloc(bytes);
  EXPECT_NOT_NULL(frame.pixels);
  memcpy(frame.pixels, pixels, bytes);
  u32 colored = 0;
  for (u32 i = 0; i < frame.width * frame.height; i++)
    colored += (frame.pixels[i] & 0xFFFFFFu) != 0;
  EXPECT_TRUE(colored > 0);
  return frame;
}

static u32 count_frame_differences(const reset_frame_t* a, const reset_frame_t* b)
{
  EXPECT_EQ(a->width, b->width);
  EXPECT_EQ(a->height, b->height);
  u32 different = 0;
  for (u32 i = 0; i < a->width * a->height; i++)
    different += a->pixels[i] != b->pixels[i];
  return different;
}

static void save_reset_frame(const reset_frame_t* frame, bool restore, const char* name)
{
  const char* directory = getenv("CUPID_TEST_GPU_RESET_FRAMES");
  if (!directory || !*directory) return;
  char      path[1024];
  const int length = snprintf(path, sizeof(path), "%s/%s-%s.ppm", directory, restore ? "restored" : "fresh", name);
  EXPECT_TRUE(length > 0 && (size_t)length < sizeof(path));
  FILE* stream = fopen(path, "wb");
  EXPECT_NOT_NULL(stream);
  EXPECT_TRUE(fprintf(stream, "P6\n%u %u\n255\n", frame->width, frame->height) > 0);
  for (u32 i = 0; i < frame->width * frame->height; i++) {
    const u8 pixel[] = {(u8)frame->pixels[i], (u8)(frame->pixels[i] >> 8), (u8)(frame->pixels[i] >> 16)};
    EXPECT_EQ(fwrite(pixel, sizeof(pixel), 1, stream), 1);
  }
  EXPECT_EQ(fclose(stream), 0);
}

static void restore_pixel_display(void)
{
  u8           cpu[16384], irq[16], counters[128], timing[4096];
  const size_t capacity = 2 * 1024 * 1024;
  u8*          gpu      = malloc(capacity);
  EXPECT_NOT_NULL(gpu);
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity;
    size_t size;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {interrupt_controller_do_state, irq, sizeof(irq), 0},
                    {gpu_do_state, gpu, capacity, 0},
                    {timers_do_state, counters, sizeof(counters), 0},
                    {timing_events_do_state, timing, sizeof(timing), 0}};
  state_wrapper_t state;
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].size = state_wrapper_get_position(&state);
  }
  gpu_write_register(4, 0x05012340);
  gpu_write_register(4, 0x08000001);
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
  free(gpu);
}

static void check_reset_pixels(bool restore)
{
  const u16 addresses[][2] = {{0, 0}, {512, 0}, {0, 256}, {512, 256}};
  const u16 colors[]       = {0x001F, 0x03E0, 0x7C00, 0x7FFF};
  u16*      vram           = malloc(1024 * 512 * sizeof(u16));
  EXPECT_NOT_NULL(vram);
  for (u32 y = 0; y < 512; y++)
    for (u32 x = 0; x < 1024; x++)
      vram[y * 1024 + x] = colors[(x / 512) + 2 * (y / 256)];
  u32 failures = 0, observations = 0;
  for (u32 alias = 0; alias < 4; alias++) {
    for (u32 pal = 0; pal < 2; pal++) {
      for (u32 address = 0; address < 4; address++) {
        display_fixture_t fixture;
        begin_display(&fixture);
        /* This test uses the software backend's real scanout callback. */
        fixture.backend->vtable                    = fixture.original;
        const gpu_backend_update_vram_cmd_t update = {0, 0, 1024, 512, false, false};
        gpu_backend_update_vram(&update, vram);
        gpu_write_register(4, 0x03000000);
        const reset_frame_t cold = read_reset_frame();
        gpu_write_register(4, 0x08000000 | (pal ? 8u : 0u));
        const reset_frame_t base = read_reset_frame();
        const u16           x = addresses[address][0], y = addresses[address][1];
        gpu_write_register(4, 0x05000000 | x | ((u32)y << 10));
        const reset_frame_t prior = read_reset_frame();
        EXPECT_EQ(count_frame_differences(&base, &prior) != 0, address != 0);
        if (restore) {
          restore_pixel_display();
          const reset_frame_t restored = read_reset_frame();
          EXPECT_EQ(count_frame_differences(&prior, &restored), 0);
          free(restored.pixels);
        }
        gpu_write_register(4, alias << 30);
        gpu_update_display(false);
        u32 disabled_width, disabled_height;
        EXPECT_NOT_NULL(gpu_sw_get_display_buffer(&disabled_width, &disabled_height));
        EXPECT_EQ(disabled_width, 0);
        EXPECT_EQ(disabled_height, 0);
        gpu_write_register(4, 0x03000000);
        const reset_frame_t after     = read_reset_frame();
        const u32           different = count_frame_differences(&cold, &after);
        printf("gpu-reset-display-pixels restore=%u alias=%u pal=%u address=%u,%u size=%ux%u different=%u "
               "expected_different=0\n",
               restore, alias, pal, x, y, after.width, after.height, different);
        failures += different != 0;
        observations++;
        if (alias == 0 && pal == 0 && address == 1) {
          save_reset_frame(&cold, restore, "cold");
          save_reset_frame(&prior, restore, "prior");
          save_reset_frame(&after, restore, "after");
        }
        free(cold.pixels);
        free(base.pixels);
        free(prior.pixels);
        free(after.pixels);
        end_display(&fixture);
      }
    }
  }
  free(vram);
  fflush(stdout);
  EXPECT_EQ(observations, 32);
  EXPECT_EQ(failures, 0);
}

TEST(GpuResetDisplayPixels, fresh)
{
  check_reset_pixels(false);
}
TEST(GpuResetDisplayPixels, restored)
{
  check_reset_pixels(true);
}
