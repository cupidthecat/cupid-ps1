/* cupid-ps1 polyline state bounds and genuine version-93 GPU records. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "tests/core/gpu/fixtures/polyline_v93.h"
#include <zlib.h>

enum { STATE_CAPACITY = 2 * 1024 * 1024 };

static size_t save_gpu(u8* bytes) {
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, STATE_CAPACITY, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  return state_wrapper_get_position(&state);
}

static bool load_gpu(const u8* bytes, size_t size, u32 version) {
  state_wrapper_t state;
  state_wrapper_init_read(&state, bytes, size, version);
  const bool result = gpu_do_state(&state);
  EXPECT_EQ(result, !state_wrapper_has_error(&state));
  if (result) EXPECT_EQ(state_wrapper_get_position(&state), size);
  return result;
}

static void put_word(u8* bytes, size_t offset, u32 value) {
  memcpy(bytes + offset, &value, sizeof(value));
}

static u32 get_word(const u8* bytes, size_t offset) {
  u32 value;
  memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

static void begin_pending(void) {
  const u32 words[] = {0x480000FF, 32 | 24 << 16, 40 | 24 << 16, 40 | 32 << 16};
  for (u32 i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    gpu_write_register(0, words[i]);
}

static void check_legacy(bool shaded, bool pending) {
  display_fixture_t fixture;
  begin_display(&fixture);
  begin_pending();
  const u8*    compressed = shaded ? (pending ? polyline_v93_shaded_pending : polyline_v93_shaded_idle)
                                   : (pending ? polyline_v93_flat_pending : polyline_v93_flat_idle);
  const size_t compressed_size =
      shaded ? (pending ? sizeof(polyline_v93_shaded_pending) : sizeof(polyline_v93_shaded_idle))
             : (pending ? sizeof(polyline_v93_flat_pending) : sizeof(polyline_v93_flat_idle));
  const u32 expected_size = shaded ? (pending ? polyline_v93_shaded_pending_size : polyline_v93_shaded_idle_size)
                                   : (pending ? polyline_v93_flat_pending_size : polyline_v93_flat_idle_size);
  u8*       bytes         = malloc(STATE_CAPACITY);
  EXPECT_NOT_NULL(bytes);
  uLongf size = STATE_CAPACITY;
  EXPECT_EQ(uncompress(bytes, &size, compressed, compressed_size), Z_OK);
  EXPECT_EQ(size, expected_size);
  EXPECT_EQ(load_gpu(bytes, size, 93), !pending);
  if (!pending) EXPECT_EQ(g_vram[24 * VRAM_WIDTH + 32], 31);
  /* Neither a successful old idle load nor a rejected old packet reuses live vertices. */
  const size_t current_size = save_gpu(bytes);
  EXPECT_EQ(get_word(bytes, current_size - sizeof(g_vram) - 4), 0);
  free(bytes);
  end_display(&fixture);
}

TEST(GpuPolylineState, legacy_flat_idle) {
  check_legacy(false, false);
}
TEST(GpuPolylineState, legacy_shaded_idle) {
  check_legacy(true, false);
}
TEST(GpuPolylineState, legacy_flat_pending_is_unrecoverable) {
  check_legacy(false, true);
}
TEST(GpuPolylineState, legacy_shaded_pending_is_unrecoverable) {
  check_legacy(true, true);
}

TEST(GpuPolylineState, malformed_queue_counts) {
  for (u32 field = 0; field < 3; field++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    u8* bytes = malloc(STATE_CAPACITY);
    EXPECT_NOT_NULL(bytes);
    const size_t size           = save_gpu(bytes);
    const size_t polyline_count = size - sizeof(g_vram) - 4;
    /* Empty FIFO and blit payloads immediately precede the appended polyline count. */
    const size_t offset = polyline_count - (field == 0 ? 0 : field == 1 ? 12 : 16);
    EXPECT_EQ(get_word(bytes, offset), 0);
    put_word(bytes, offset, UINT32_MAX);
    begin_pending();
    EXPECT_FALSE(load_gpu(bytes, size, SAVE_STATE_VERSION));
    const size_t clean_size = save_gpu(bytes);
    EXPECT_EQ(get_word(bytes, clean_size - sizeof(g_vram) - 4), 0);
    free(bytes);
    end_display(&fixture);
  }
}

TEST(GpuPolylineState, inconsistent_and_truncated_packets) {
  for (u32 sample = 0; sample < 5; sample++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    u8* bytes = malloc(STATE_CAPACITY);
    EXPECT_NOT_NULL(bytes);
    if (sample != 0) begin_pending();
    const size_t size = save_gpu(bytes);
    if (sample == 0) {
      put_word(bytes, size - sizeof(g_vram) - 4, 1); /* Vertices without a pending primitive. */
      EXPECT_FALSE(load_gpu(bytes, size, SAVE_STATE_VERSION));
    } else {
      const size_t count_offset = size - sizeof(g_vram) - 4 - 3 * 8;
      EXPECT_EQ(get_word(bytes, count_offset), 3);
      if (sample == 1) {
        put_word(bytes, count_offset, 0); /* Pending primitive without an endpoint. */
        EXPECT_FALSE(load_gpu(bytes, size, SAVE_STATE_VERSION));
      } else if (sample == 2) {
        EXPECT_FALSE(load_gpu(bytes, count_offset + 4 + 3 * 8 - 1, SAVE_STATE_VERSION));
      } else if (sample == 3) {
        put_word(bytes, count_offset - 4, 0x400000FF); /* A line opcode cannot resume a polyline. */
        EXPECT_FALSE(load_gpu(bytes, size, SAVE_STATE_VERSION));
      } else {
        EXPECT_FALSE(load_gpu(bytes, size - 1, SAVE_STATE_VERSION)); /* Truncated VRAM. */
      }
    }
    free(bytes);
    end_display(&fixture);
  }
}

TEST(GpuPolylineState, dma_source_addresses_survive_clobbered_restore) {
  display_fixture_t fixture;
  begin_display(&fixture);
  memset(g_vram, 0, sizeof(g_vram));
  gpu_write_register(0, 0xE3000000);
  gpu_write_register(0, 0xE407FFFF);
  gpu_write_register(4, 0x04000002);
  const u32 words[] = {0x580000FF, 32 | 24 << 16, 0x00FF0000, 40 | 24 << 16,
                       0x0000FF00}; /* Last color waits for its position. */
  for (u32 i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    gpu_dma_write(0x1000 + 4 * i, words[i]);
    gpu_end_dma_write();
  }
  u8* bytes = malloc(STATE_CAPACITY);
  EXPECT_NOT_NULL(bytes);
  const size_t size         = save_gpu(bytes);
  const size_t count_offset = size - sizeof(g_vram) - 4 - 4 * sizeof(u64);
  EXPECT_EQ(get_word(bytes, count_offset), 4);
  for (u32 i = 0; i < 4; i++) {
    u64 word;
    memcpy(&word, bytes + count_offset + 4 + i * sizeof(word), sizeof(word));
    EXPECT_EQ(word, ((u64)(0x1004 + 4 * i) << 32) | words[i + 1]);
  }
  gpu_write_register(4, 0);
  EXPECT_TRUE(load_gpu(bytes, size, SAVE_STATE_VERSION));
  u8* restored = malloc(STATE_CAPACITY);
  EXPECT_NOT_NULL(restored);
  EXPECT_EQ(save_gpu(restored), size);
  EXPECT_EQ(memcmp(bytes, restored, size), 0);
  gpu_write_register(0, 40 | 32 << 16);
  gpu_write_register(0, 0x50005000);
  run_slice(2000);
  EXPECT_EQ(g_vram[32 * VRAM_WIDTH + 40], 0x3E0);
  free(restored);
  free(bytes);
  end_display(&fixture);
}
