/* cupid-ps1 GPU information query selection and read-latch ownership. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"

enum {
  SEED   = 0x12345,
  WINDOW = 0x54321,
  TOP    = 0x12 | (0x34 << 10),
  BOTTOM = 0x312 | (0x134 << 10),
  OFFSET = 0x7FD | (0x7FB << 11)
};

static void drain_information_setup(void) {
  for (;;) {
    timing_event_t* command = NULL;
    for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next)
      if (event->name_len == 16 && !memcmp(event->name_data, "GPU Command Tick", 16)) command = event;
    if (!command) return;
    cpu_add_pending_ticks(timing_event_get_ticks_until_next_execution(command));
    timing_events_run_events();
  }
}

static void prepare_information(void) {
  gpu_write_register(0, 0xE2000000 | SEED);
  drain_information_setup();
  gpu_write_register(4, 0x10000002);
  EXPECT_EQ(gpu_read_register(0), SEED);
  gpu_write_register(0, 0xE2000000 | WINDOW);
  gpu_write_register(0, 0xE3000000 | TOP);
  gpu_write_register(0, 0xE4000000 | BOTTOM);
  gpu_write_register(0, 0xE5000000 | OFFSET);
  drain_information_setup();
}

TEST(GpuInformation, all_command_mirrors_decode_four_index_bits) {
  display_fixture_t fixture;
  begin_display(&fixture);
  u32 mismatches = 0;
  for (u32 command = 0x10; command <= 0x1F; command++) {
    for (u32 index = 0; index < 32; index++) {
      prepare_information();
      const u32 values[16] = {SEED, SEED, WINDOW, TOP,  BOTTOM, OFFSET, SEED, 2,
                              0,    SEED, SEED,   SEED, SEED,   SEED,   SEED, SEED};
      gpu_write_register(4, (command << 24) | 0x765400 | index);
      mismatches += gpu_read_register(0) != values[index & 15];
    }
  }
  EXPECT_EQ(mismatches, 0);
  end_display(&fixture);
}

TEST(GpuInformation, register_changes_do_not_implicitly_update_the_read_latch) {
  display_fixture_t fixture;
  begin_display(&fixture);
  prepare_information();
  EXPECT_EQ(gpu_read_register(0), SEED);
  EXPECT_EQ(gpu_read_register(0), SEED);
  gpu_write_register(4, 0x10000002);
  EXPECT_EQ(gpu_read_register(0), WINDOW);
  gpu_write_register(0, 0xE2000000);
  EXPECT_EQ(gpu_read_register(0), WINDOW);
  end_display(&fixture);
}

TEST(GpuInformation, version_and_zero_queries_need_no_vram_read_request) {
  display_fixture_t fixture;
  begin_display(&fixture);
  prepare_information();
  gpu_write_register(4, 0x04000003);
  const u32 status = gpu_read_register(4);
  EXPECT_FALSE(status & ((1u << 25) | (1u << 27)));
  gpu_write_register(4, 0x10000007);
  EXPECT_EQ(gpu_read_register(0), 2);
  EXPECT_EQ(gpu_read_register(0), 2);
  gpu_write_register(4, 0x10000008);
  EXPECT_EQ(gpu_read_register(0), 0);
  EXPECT_EQ(gpu_read_register(4), status);
  end_display(&fixture);
}

TEST(GpuInformation, restored_latch_survives_unknown_queries_and_register_changes) {
  display_fixture_t fixture;
  begin_display(&fixture);
  prepare_information();
  gpu_write_register(4, 0x10000007);
  EXPECT_EQ(gpu_read_register(0), 2);
  u8* saved = malloc(2 * 1024 * 1024);
  EXPECT_NOT_NULL(saved);
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, 2 * 1024 * 1024, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  gpu_write_register(4, 0x10000002);
  EXPECT_EQ(gpu_read_register(0), WINDOW);
  state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  gpu_write_register(0, 0xE2000000);
  gpu_write_register(4, 0x1000000A);
  EXPECT_EQ(gpu_read_register(0), 2);
  free(saved);
  end_display(&fixture);
}
