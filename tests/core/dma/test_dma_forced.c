/* cupid-ps1 manual force-start, pause ownership and saved continuation. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/dma/dma.h"

enum { ADDRESS = 0x60000, BUSY = 1u << 24, TRIGGER = 1u << 28, PAUSE = 1u << 29 };

static void begin_forced(display_fixture_t* fixture, u32 control) {
  begin_display(fixture);
  dma_initialize();
  gpu_write_register(4, 0x04000003);
  gpu_write_register(0, 0xE2000002);
  gpu_write_register(4, 0x10000002);
  EXPECT_EQ(gpu_read_register(0), 2);
  EXPECT_FALSE(gpu_read_register(4) & (1u << 25));
  EXPECT_EQ(dma_dbg_request(DMA_CHANNEL_GPU), 0);
  memset(bus_get_unprotected_ram_pointer() + ADDRESS, 0x55, 16);
  dma_write_register(0x20, ADDRESS);
  dma_write_register(0x24, 4);
  dma_write_register(0x74, 0x00840000);
  dma_write_register(0x70, 8u << 8);
  dma_write_register(0x28, control);
}

static void end_forced(display_fixture_t* fixture) {
  dma_shutdown();
  end_display(fixture);
}

static u32 ram_word(u32 index) {
  u32 value;
  memcpy(&value, bus_get_unprotected_ram_pointer() + ADDRESS + index * 4, 4);
  return value;
}

static void advance_forced(u32 ticks) {
  cpu_add_pending_ticks(ticks);
  timing_events_run_events();
}

TEST(DmaForced, absent_request_waits_until_force_start) {
  display_fixture_t fixture;
  begin_forced(&fixture, BUSY);
  advance_forced(1024);
  EXPECT_EQ(ram_word(0), 0x55555555);
  EXPECT_EQ(dma_read_register(0x28), BUSY);
  dma_write_register(0x28, BUSY | TRIGGER);
  for (u32 i = 0; i < 4; i++)
    EXPECT_EQ(ram_word(i), 2);
  EXPECT_EQ(dma_read_register(0x28), 0);
  EXPECT_EQ(dma_read_register(0x20), ADDRESS);
  EXPECT_EQ(dma_read_register(0x24), 4);
  EXPECT_EQ(dma_read_register(0x74), 0x84840000);
  end_forced(&fixture);
}

TEST(DmaForced, initial_pause_retains_trigger_until_release) {
  display_fixture_t fixture;
  begin_forced(&fixture, BUSY | TRIGGER | PAUSE);
  advance_forced(1024);
  EXPECT_EQ(ram_word(0), 0x55555555);
  EXPECT_EQ(dma_read_register(0x28), BUSY | TRIGGER | PAUSE);
  EXPECT_EQ(dma_read_register(0x74), 0x00840000);
  dma_write_register(0x28, BUSY | TRIGGER);
  EXPECT_EQ(ram_word(3), 2);
  EXPECT_EQ(dma_read_register(0x28), 0);
  end_forced(&fixture);
}

TEST(DmaForced, chopped_force_survives_trigger_clear_without_request) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  EXPECT_EQ(ram_word(0), 2);
  EXPECT_EQ(ram_word(1), 0x55555555);
  EXPECT_FALSE(dma_read_register(0x28) & TRIGGER);
  for (u32 i = 1; i < 4; i++) {
    advance_forced(128);
    EXPECT_EQ(ram_word(i), 2);
    EXPECT_EQ(dma_read_register(0x24), 3 - i);
  }
  EXPECT_EQ(dma_read_register(0x20), ADDRESS + 16);
  EXPECT_FALSE(dma_read_register(0x28) & BUSY);
  EXPECT_EQ(dma_read_register(0x74), 0x84840000);
  end_forced(&fixture);
}

TEST(DmaForced, expired_chop_pause_and_force_origin_survive_restore) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  dma_write_register(0x28, dma_read_register(0x28) | PAUSE);
  advance_forced(1024);
  EXPECT_EQ(ram_word(1), 0x55555555);
  EXPECT_EQ(dma_read_register(0x24), 3);
  u8              saved[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  EXPECT_EQ(size, 132);
  EXPECT_EQ(saved[131], 1u << DMA_CHANNEL_GPU);
  dma_reset();
  state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(ram_word(1), 0x55555555);
  dma_write_register(0x28, dma_read_register(0x28) & ~PAUSE);
  EXPECT_EQ(ram_word(1), 2);
  advance_forced(256);
  EXPECT_EQ(ram_word(3), 2);
  EXPECT_FALSE(dma_read_register(0x28) & BUSY);
  end_forced(&fixture);
}

TEST(DmaForced, device_started_burst_ignores_force_pause) {
  display_fixture_t fixture;
  begin_forced(&fixture, BUSY | PAUSE);
  dma_set_request(DMA_CHANNEL_GPU, true);
  EXPECT_EQ(ram_word(3), 2);
  EXPECT_EQ(dma_read_register(0x28), PAUSE);
  EXPECT_EQ(dma_read_register(0x74), 0x84840000);
  end_forced(&fixture);
}

TEST(DmaForced, stopping_discards_force_origin) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  EXPECT_EQ(ram_word(0), 2);
  dma_write_register(0x28, 0);
  dma_write_register(0x28, BUSY);
  advance_forced(1024);
  EXPECT_EQ(ram_word(1), 0x55555555);
  EXPECT_EQ(dma_read_register(0x28), BUSY);
  end_forced(&fixture);
}

TEST(DmaForced, reset_and_bus_error_discard_force_origin) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  dma_reset();
  dma_write_register(0x20, ADDRESS + 4);
  dma_write_register(0x24, 3);
  dma_write_register(0x70, 8u << 8);
  dma_write_register(0x28, BUSY);
  advance_forced(1024);
  EXPECT_EQ(ram_word(1), 0x55555555);
  dma_write_register(0x28, 0);
  dma_write_register(0x20, bus_get_ram_mapped_size() - 4);
  dma_write_register(0x24, 2);
  dma_write_register(0x28, 0x11700100);
  advance_forced(128);
  EXPECT_FALSE(dma_read_register(0x28) & BUSY);
  EXPECT_TRUE(dma_read_register(0x74) & (1u << 15));
  dma_write_register(0x20, ADDRESS + 4);
  dma_write_register(0x24, 3);
  dma_write_register(0x28, BUSY);
  advance_forced(1024);
  EXPECT_EQ(ram_word(1), 0x55555555);
  end_forced(&fixture);
}

TEST(DmaForced, restored_unexpired_pause_retains_forced_origin_after_request_edges) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  dma_set_request(DMA_CHANNEL_GPU, true);
  dma_set_request(DMA_CHANNEL_GPU, false);
  advance_forced(32);
  u8              saved[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  dma_reset();
  state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  advance_forced(95);
  EXPECT_EQ(ram_word(1), 0x55555555);
  advance_forced(1);
  EXPECT_EQ(ram_word(1), 2);
  end_forced(&fixture);
}

TEST(DmaForced, legacy95_preserves_phase_without_inventing_force_origin) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  u8              saved[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), 95);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  EXPECT_EQ(size, 131);
  dma_reset();
  state_wrapper_init_read(&state, saved, size, 95);
  EXPECT_TRUE(dma_do_state(&state));
  advance_forced(1024);
  EXPECT_EQ(ram_word(1), 0x55555555);
  EXPECT_EQ(dma_read_register(0x24), 3);
  dma_set_request(DMA_CHANNEL_GPU, true);
  EXPECT_EQ(ram_word(1), 2);
  end_forced(&fixture);
}

TEST(DmaForced, rejects_invalid_origin_masks_and_missing_mask_byte) {
  display_fixture_t fixture;
  begin_forced(&fixture, 0x11700100);
  u8              saved[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  for (u32 invalid = 0x40; invalid <= 0x80; invalid <<= 1) {
    dma_reset();
    saved[131] = invalid;
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    EXPECT_FALSE(dma_do_state(&state));
  }
  saved[131] = 1u << DMA_CHANNEL_GPU;
  dma_reset();
  state_wrapper_init_read(&state, saved, size - 1, SAVE_STATE_VERSION);
  EXPECT_FALSE(dma_do_state(&state));
  EXPECT_TRUE(state_wrapper_has_error(&state));
  const u32 invalid_controls[] = {0, BUSY | 0x200};
  const s32 no_pause           = 0;
  memcpy(saved + 111, &no_pause, 4);
  for (u32 i = 0; i < countof(invalid_controls); i++) {
    dma_reset();
    memcpy(saved + 38, &invalid_controls[i], 4);
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    EXPECT_FALSE(dma_do_state(&state));
  }
  end_forced(&fixture);
}
