/* cupid-ps1 shared GPU backend record boundaries and byte layout. */
#include "tests/test_harness.h"
#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "core/state/save_state_version.h"
#include "util/state/state_wrapper.h"
#include <stdlib.h>
#include <string.h>

static size_t record_size(void) {
  return sizeof(g_vram) + sizeof(g_gpu_clut);
}
static void seed(void) {
  for (u32 i = 0; i < VRAM_WIDTH * VRAM_HEIGHT; i++)
    g_vram[i] = (u16)(i * 17 + 3);
  for (u32 i = 0; i < GPU_CLUT_SIZE; i++)
    g_gpu_clut[i] = (u16)(0x8000 + i * 61);
}
static u8* save(gpu_backend_t* be) {
  u8* bytes = malloc(record_size());
  EXPECT_TRUE(bytes != NULL);
  seed();
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, record_size(), SAVE_STATE_VERSION);
  EXPECT_TRUE(be->vtable->do_state(be, &state));
  EXPECT_EQ(state_wrapper_get_position(&state), record_size());
  return bytes;
}
static void live_snapshot(u8* bytes) {
  memset(g_vram, 0xA5, sizeof(g_vram));
  memset(g_gpu_clut, 0x5A, sizeof(g_gpu_clut));
  memcpy(bytes, g_vram, sizeof(g_vram));
  memcpy(bytes + sizeof(g_vram), g_gpu_clut, sizeof(g_gpu_clut));
}
static void expect_unchanged(const u8* bytes) {
  EXPECT_EQ(memcmp(bytes, g_vram, sizeof(g_vram)), 0);
  EXPECT_EQ(memcmp(bytes + sizeof(g_vram), g_gpu_clut, sizeof(g_gpu_clut)), 0);
}

TEST(GpuBackendState, wire_layout_and_roundtrip) {
  gpu_backend_t* be = gpu_sw_create();
  EXPECT_TRUE(be != NULL);
  u8* bytes = save(be);
  EXPECT_EQ(memcmp(bytes, g_vram, sizeof(g_vram)), 0);
  EXPECT_EQ(memcmp(bytes + sizeof(g_vram), g_gpu_clut, sizeof(g_gpu_clut)), 0);
  memset(g_vram, 0, sizeof(g_vram));
  memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
  state_wrapper_t state;
  state_wrapper_init_read(&state, bytes, record_size(), SAVE_STATE_VERSION);
  EXPECT_TRUE(be->vtable->do_state(be, &state));
  EXPECT_EQ(state_wrapper_get_position(&state), record_size());
  expect_unchanged(bytes);
  free(bytes);
  gpu_backend_destroy(be);
}
static void truncated(bool clut) {
  gpu_backend_t* be = gpu_sw_create();
  EXPECT_TRUE(be != NULL);
  u8* bytes = save(be);
  u8* live  = malloc(record_size());
  EXPECT_TRUE(live != NULL);
  live_snapshot(live);
  const size_t  vram_cuts[] = {0, 1, sizeof(g_vram) / 2, sizeof(g_vram) - 1};
  const size_t  clut_cuts[] = {sizeof(g_vram), sizeof(g_vram) + 1, record_size() - 1};
  const size_t* cuts        = clut ? clut_cuts : vram_cuts;
  const u32     count       = clut ? 3 : 4;
  for (u32 i = 0; i < count; i++) {
    state_wrapper_t state;
    state_wrapper_init_read(&state, bytes, cuts[i], SAVE_STATE_VERSION);
    EXPECT_FALSE(be->vtable->do_state(be, &state));
    EXPECT_TRUE(state_wrapper_has_error(&state));
    expect_unchanged(live);
  }
  free(live);
  free(bytes);
  gpu_backend_destroy(be);
}
TEST(GpuBackendState, truncated_vram_preserves_live_buffers) {
  truncated(false);
}
TEST(GpuBackendState, truncated_palette_preserves_live_buffers) {
  truncated(true);
}
TEST(GpuBackendState, sticky_error_and_invalid_cursor_preserve_live_buffers) {
  gpu_backend_t* be = gpu_sw_create();
  EXPECT_TRUE(be != NULL);
  u8* bytes = save(be);
  u8* live  = malloc(record_size());
  EXPECT_TRUE(live != NULL);
  live_snapshot(live);
  state_wrapper_t state;
  state_wrapper_init_read(&state, bytes, record_size(), SAVE_STATE_VERSION);
  state.error = true;
  EXPECT_FALSE(be->vtable->do_state(be, &state));
  expect_unchanged(live);
  state_wrapper_init_read(&state, bytes, record_size(), SAVE_STATE_VERSION);
  state_wrapper_set_position(&state, record_size() + 1);
  EXPECT_FALSE(be->vtable->do_state(be, &state));
  EXPECT_TRUE(state_wrapper_has_error(&state));
  expect_unchanged(live);
  free(live);
  free(bytes);
  gpu_backend_destroy(be);
}
TEST(GpuBackendState, short_write_reports_failure_without_mutation) {
  gpu_backend_t* be = gpu_sw_create();
  EXPECT_TRUE(be != NULL);
  u8* bytes = save(be);
  u8* live  = malloc(record_size());
  EXPECT_TRUE(live != NULL);
  live_snapshot(live);
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, record_size() - 1, SAVE_STATE_VERSION);
  EXPECT_FALSE(be->vtable->do_state(be, &state));
  EXPECT_TRUE(state_wrapper_has_error(&state));
  expect_unchanged(live);
  free(live);
  free(bytes);
  gpu_backend_destroy(be);
}
TEST(GpuBackendState, offset_record_and_tail_are_version_independent) {
  gpu_backend_t* be = gpu_sw_create();
  EXPECT_TRUE(be != NULL);
  u8*          bytes  = save(be);
  const size_t prefix = 7, tail = 9, size = prefix + record_size() + tail;
  u8*          container = malloc(size);
  EXPECT_TRUE(container != NULL);
  memset(container, 0xCC, size);
  memcpy(container + prefix, bytes, record_size());
  const u32 versions[] = {85, SAVE_STATE_VERSION};
  for (u32 i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
    memset(g_vram, 0, sizeof(g_vram));
    memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
    state_wrapper_t state;
    state_wrapper_init_read(&state, container, size, versions[i]);
    state_wrapper_set_position(&state, prefix);
    EXPECT_TRUE(be->vtable->do_state(be, &state));
    EXPECT_EQ(state_wrapper_get_position(&state), prefix + record_size());
    expect_unchanged(bytes);
  }
  for (size_t i = 0; i < prefix; i++)
    EXPECT_EQ(container[i], 0xCC);
  for (size_t i = prefix + record_size(); i < size; i++)
    EXPECT_EQ(container[i], 0xCC);
  free(container);
  free(bytes);
  gpu_backend_destroy(be);
}
