/* cupid-ps1 upload dimensions, odd padding and pending data restoration. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"

enum { UPLOAD_WIDTH = 1023, UPLOAD_HEIGHT = 511, UPLOAD_WORDS = (UPLOAD_WIDTH * UPLOAD_HEIGHT + 1) / 2 };

static void upload_header(u32 size) {
  gpu_write_register(0, 0xBFFFFFFF); /* All CPU-to-VRAM command aliases share the transfer model. */
  gpu_write_register(0, 1022 | 510u << 16);
  gpu_write_register(0, size);
}

static void upload_data(u32 first, u32 end) {
  for (u32 i = first; i < end; i++)
    gpu_write_register(0, i + 1 == UPLOAD_WORDS ? 0x1F005678 : 0x12345678);
}

static void check_upload(void) {
  u32 differences = 0;
  for (u32 y = 0; y < VRAM_HEIGHT; y++) {
    for (u32 x = 0; x < VRAM_WIDTH; x++) {
      const u32 dx = (x - 1022u) & VRAM_WIDTH_MASK;
      const u32 dy = (y - 510u) & VRAM_HEIGHT_MASK;
      const u16 expected =
          dx < UPLOAD_WIDTH && dy < UPLOAD_HEIGHT ? ((dy * UPLOAD_WIDTH + dx) & 1u ? 0x1234 : 0x5678) : 0x2222;
      differences += g_vram[y * VRAM_WIDTH + x] != expected;
    }
  }
  EXPECT_EQ(differences, 0);
  EXPECT_FALSE(gpu_read_register(4) & (1u << 24));
  gpu_write_register(0, 0x1F000000);
  run_slice(2000);
  EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
}

static void check_size(u32 size, bool dma) {
  display_fixture_t fixture;
  begin_display(&fixture);
  for (u32 pixel = 0; pixel < VRAM_WIDTH * VRAM_HEIGHT; pixel++)
    g_vram[pixel] = 0x2222;
  upload_header(size);
  if (dma) {
    for (u32 i = 0; i < UPLOAD_WORDS; i++) {
      gpu_dma_write(0x10000 + i * 4, i + 1 == UPLOAD_WORDS ? 0x1F005678 : 0x12345678);
      gpu_end_dma_write();
    }
  } else {
    upload_data(0, UPLOAD_WORDS);
  }
  run_slice(2000);
  check_upload();
  end_display(&fixture);
}

TEST(GpuVramUpload, all_ones_dimensions_and_odd_padding) {
  check_size(0xFFFFFFFF, false);
}

TEST(GpuVramUpload, ignored_size_bits_controls) {
  const u32 sizes[] = {0x01FF03FF, 0xFFFF03FF, 0x01FFFFFF};
  for (u32 i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    check_size(sizes[i], false);
  }
}

TEST(GpuVramUpload, dma_preserves_all_ones_upload_data) {
  check_size(0xFFFFFFFF, true);
}

TEST(GpuVramUpload, restore_after_reset_preserves_large_pending_upload) {
  display_fixture_t fixture;
  begin_display(&fixture);
  for (u32 pixel = 0; pixel < VRAM_WIDTH * VRAM_HEIGHT; pixel++)
    g_vram[pixel] = 0x2222;
  upload_header(0xFFFFFFFF);
  upload_data(0, 514);
  const size_t capacity = 4 * 1024 * 1024;
  u8*          bytes    = malloc(capacity);
  EXPECT_NOT_NULL(bytes);
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t size = state_wrapper_get_position(&state);
  gpu_write_register(4, 0);
  memset(g_vram, 0, sizeof(g_vram));
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  upload_data(514, UPLOAD_WORDS);
  run_slice(2000);
  check_upload();
  free(bytes);
  end_display(&fixture);
}
