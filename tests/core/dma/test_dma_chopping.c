/* cupid-ps1 chopped manual DMA progress, pause isolation and saved phases. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/dma/dma.h"

enum { ADDRESS = 0x50000, CONTROL = 0x20 + 8, BLOCK = 0x20 + 4, BASE = 0x20 };

static void begin_chop(display_fixture_t* fixture, u32 count, u32 dma_window, u32 cpu_window) {
  begin_display(fixture);
  dma_initialize();
  gpu_write_register(4, 0x04000002);
  u32 words[128];
  for (u32 i = 0; i < countof(words); i++)
    words[i] = 0x1F000000;
  memcpy(bus_get_unprotected_ram_pointer() + ADDRESS, words, sizeof(words));
  dma_write_register(BASE, ADDRESS);
  dma_write_register(BLOCK, count);
  dma_write_register(0x70, 8 << 8);
  dma_write_register(CONTROL, 0x11000101 | dma_window << 16 | cpu_window << 20);
}

static void end_chop(display_fixture_t* fixture) {
  dma_shutdown();
  end_display(fixture);
}

static void advance_chop(u32 ticks) {
  cpu_add_pending_ticks(ticks);
  timing_events_run_events();
}

static timing_event_t* chop_event(void) {
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next)
    if (event->name_len == 12 && !memcmp(event->name_data, "DMA Chop GPU", 12)) return event;
  return NULL;
}

TEST(DmaChopping, first_burst_is_immediate_and_each_cpu_window_is_retained) {
  for (u32 window = 0; window < 8; window++) {
    display_fixture_t fixture;
    begin_chop(&fixture, 12, 2, window);
    EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    EXPECT_EQ(dma_read_register(BASE), ADDRESS + 16);
    EXPECT_EQ(dma_read_register(BLOCK), 8);
    EXPECT_TRUE(dma_read_register(CONTROL) & (1u << 24));
    EXPECT_FALSE(dma_read_register(CONTROL) & (1u << 28));
    EXPECT_NOT_NULL(chop_event());
    EXPECT_EQ(timing_event_get_ticks_until_next_execution(chop_event()), 1 << window);
    dma_write_register(0x74, 0x00840000);
    EXPECT_EQ(dma_read_register(0x74), 0x00840000);
    if (window) {
      advance_chop((1 << window) - 1);
      EXPECT_EQ(dma_read_register(BLOCK), 8);
    }
    advance_chop(1);
    EXPECT_EQ(dma_read_register(BASE), ADDRESS + 32);
    EXPECT_EQ(dma_read_register(BLOCK), 4);
    EXPECT_EQ(dma_read_register(0x74), 0x00840000);
    advance_chop(1 << window);
    EXPECT_EQ(dma_read_register(BASE), ADDRESS + 48);
    EXPECT_EQ(dma_read_register(BLOCK), 0);
    EXPECT_FALSE(dma_read_register(CONTROL) & (1u << 24));
    EXPECT_NULL(chop_event());
    EXPECT_EQ(dma_read_register(0x74), 0x84840000);
    end_chop(&fixture);
  }
}

TEST(DmaChopping, request_edges_and_another_channel_cannot_skip_cpu_window) {
  display_fixture_t fixture;
  begin_chop(&fixture, 8, 0, 7);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  dma_set_request(DMA_CHANNEL_GPU, false);
  dma_set_request(DMA_CHANNEL_GPU, true);
  dma_write_register(0x60, 0x60000);
  dma_write_register(0x64, 1);
  dma_write_register(0x68, 0x11000000);
  dma_write_register(0x70, (8 << 8) | (8 << 24));
  EXPECT_EQ(dma_read_register(0x68), 2);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  const tick_count_t remaining = 128 - bus_get_dma_ram_tick_count(1);
  EXPECT_EQ(timing_event_get_ticks_until_next_execution(chop_event()), remaining);
  advance_chop(remaining - 1);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  advance_chop(1);
  EXPECT_EQ(dma_read_register(BLOCK), 6);
  end_chop(&fixture);
}

TEST(DmaChopping, legacy_records_do_not_replay_or_invent_a_pause) {
  display_fixture_t fixture;
  begin_chop(&fixture, 8, 0, 7);
  u8              saved[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), 94);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  EXPECT_EQ(size, 103);
  advance_chop(128);
  EXPECT_EQ(dma_read_register(BLOCK), 6);
  state_wrapper_init_read(&state, saved, size, 94);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(dma_read_register(BASE), ADDRESS + 4);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  EXPECT_NULL(chop_event());
  end_chop(&fixture);
}

TEST(DmaChopping, rejects_invalid_pause_records_and_truncation) {
  display_fixture_t fixture;
  begin_chop(&fixture, 8, 0, 7);
  u8              saved[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  EXPECT_EQ(size, 131);
  const s32 invalid[] = {-1, 129};
  for (u32 i = 0; i < countof(invalid); i++) {
    dma_reset();
    memcpy(saved + 111, &invalid[i], 4);
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    EXPECT_FALSE(dma_do_state(&state));
    EXPECT_NULL(chop_event());
  }
  dma_reset();
  const s32 valid = 128;
  memcpy(saved + 111, &valid, 4);
  state_wrapper_init_read(&state, saved, size - 1, SAVE_STATE_VERSION);
  EXPECT_FALSE(dma_do_state(&state));
  EXPECT_TRUE(state_wrapper_has_error(&state));
  end_chop(&fixture);
}

TEST(DmaChopping, gpu_reads_consume_one_burst_without_losing_later_words) {
  display_fixture_t fixture;
  begin_chop(&fixture, 1, 0, 0);
  dma_reset();
  const u32 pixels[] = {0x12345678, 0x23456789, 0x3456789A, 0x45671234};
  gpu_write_register(4, 0x04000003);
  gpu_write_register(0, 0xA0000000);
  gpu_write_register(0, 0);
  gpu_write_register(0, 0x00010008);
  for (u32 i = 0; i < 4; i++)
    gpu_write_register(0, pixels[i]);
  gpu_write_register(0, 0xC0000000);
  gpu_write_register(0, 0);
  gpu_write_register(0, 0x00010008);
  const u32 address = 0x60000;
  memset(bus_get_unprotected_ram_pointer() + address, 0, sizeof(pixels));
  dma_write_register(BASE, address);
  dma_write_register(BLOCK, 0x12340004);
  dma_write_register(0x70, 8 << 8);
  dma_write_register(CONTROL, 0x11700100);
  for (u32 i = 0; i < 4; i++) {
    u32 actual[4];
    memcpy(actual, bus_get_unprotected_ram_pointer() + address, sizeof(actual));
    for (u32 j = 0; j < 4; j++)
      EXPECT_EQ(actual[j], j <= i ? pixels[j] : 0);
    EXPECT_EQ(dma_read_register(BASE), address + 4 * (i + 1));
    EXPECT_EQ(dma_read_register(BLOCK), 0x12340000 | (3 - i));
    if (i < 3) advance_chop(128);
  }
  EXPECT_FALSE(dma_read_register(CONTROL) & (1u << 24));
  EXPECT_NULL(chop_event());
  end_chop(&fixture);
}

TEST(DmaChopping, an_expired_pause_waits_for_a_new_device_request) {
  display_fixture_t fixture;
  begin_chop(&fixture, 8, 0, 7);
  gpu_write_register(4, 0x04000000);
  advance_chop(128);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  EXPECT_NULL(chop_event());
  EXPECT_TRUE(dma_read_register(CONTROL) & (1u << 24));
  advance_chop(512);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  gpu_write_register(4, 0x04000002);
  EXPECT_EQ(dma_read_register(BLOCK), 6);
  EXPECT_NOT_NULL(chop_event());
  EXPECT_EQ(timing_event_get_ticks_until_next_execution(chop_event()), 128);
  end_chop(&fixture);
}

TEST(DmaChopping, accumulated_cpu_work_matches_single_tick_service) {
  for (u32 coarse = 0; coarse < 2; coarse++) {
    display_fixture_t fixture;
    begin_chop(&fixture, 64, 0, 7);
    if (coarse)
      advance_chop(1024);
    else
      for (u32 i = 0; i < 1024; i++)
        advance_chop(1);
    EXPECT_EQ(dma_read_register(BASE), ADDRESS + 9 * 4);
    EXPECT_EQ(dma_read_register(BLOCK), 64 - 9);
    EXPECT_NOT_NULL(chop_event());
    EXPECT_EQ(timing_event_get_ticks_until_next_execution(chop_event()), 128);
    end_chop(&fixture);
  }
}

TEST(DmaChopping, stop_and_reset_cancel_pending_bursts) {
  display_fixture_t fixture;
  begin_chop(&fixture, 8, 0, 7);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  const u32 completed = dma_dbg_transfer_counts()[2];
  dma_write_register(CONTROL, 0);
  EXPECT_NULL(chop_event());
  advance_chop(1024);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  EXPECT_EQ(dma_dbg_transfer_counts()[2], completed);
  dma_reset();
  EXPECT_EQ(dma_read_register(BLOCK), 0);
  EXPECT_NULL(chop_event());
  end_chop(&fixture);
}

TEST(DmaChopping, zero_word_count_starts_a_full_65536_word_transfer) {
  display_fixture_t fixture;
  begin_chop(&fixture, 0x12340000, 7, 7);
  EXPECT_EQ(dma_read_register(BASE), ADDRESS + 512);
  EXPECT_EQ(dma_read_register(BLOCK), 0x1234FF80);
  EXPECT_TRUE(dma_read_register(CONTROL) & (1u << 24));
  end_chop(&fixture);
}

TEST(DmaChopping, a_fault_retains_only_the_successful_words) {
  display_fixture_t fixture;
  begin_chop(&fixture, 1, 0, 0);
  const u32 word = 0x1F000000;
  memcpy(bus_get_unprotected_ram_pointer() + 0x1FFFFC, &word, 4);
  dma_write_register(BASE, 0x7FFFFC);
  dma_write_register(BLOCK, 4);
  dma_write_register(CONTROL, 0x11020101);
  EXPECT_EQ(dma_read_register(BASE), 0x800000);
  EXPECT_EQ(dma_read_register(BLOCK), 3);
  EXPECT_EQ(dma_read_register(0x74), 0x80008000);
  EXPECT_FALSE(dma_read_register(CONTROL) & (1u << 24));
  EXPECT_NULL(chop_event());
  end_chop(&fixture);
}

TEST(DmaChopping, reset_restore_retains_remaining_window_without_replaying_prefix) {
  display_fixture_t fixture;
  begin_chop(&fixture, 8, 0, 7);
  advance_chop(50);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  u8              saved[256], check[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  dma_reset();
  state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(dma_read_register(BASE), ADDRESS + 4);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  EXPECT_NOT_NULL(chop_event());
  EXPECT_EQ(timing_event_get_ticks_until_next_execution(chop_event()), 78);
  state_wrapper_init_write(&state, check, sizeof(check), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(memcmp(saved, check, size), 0);
  advance_chop(77);
  EXPECT_EQ(dma_read_register(BLOCK), 7);
  advance_chop(1);
  EXPECT_EQ(dma_read_register(BLOCK), 6);
  end_chop(&fixture);
}
