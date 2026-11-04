/* cupid-ps1 reset counter latch, queue cancellation and restored idle state. */
#include "tests/core/mdec/mdec_decode_fixture.h"

TEST(MdecReset, power_status_has_zero_word_counter) {
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    const u32 status = mdec_read_register(4);
    end_decode();
    EXPECT_EQ(status, 0x80040000);
  }
}

TEST(MdecReset, zero_counter_survives_control_rewrites) {
  u32 failures = 0;
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    for (u32 control = 0; control < 4; control++) {
      mdec_write_register(0, 0x1E005678);
      mdec_write_register(4, 0x80000000u | control << 29);
      failures += (mdec_read_register(4) & 0xFFFFu) != 0;
      for (u32 next = 0; next < 4; next++) {
        mdec_write_register(4, next << 29);
        failures += (mdec_read_register(4) & 0xFFFFu) != 0;
      }
    }
    end_decode();
  }
  EXPECT_EQ(failures, 0);
}

TEST(MdecReset, pending_copy_and_queued_input_are_cancelled) {
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    mdec_write_register(4, 0x60000000);
    mdec_write_register(0, 0x28000001);
    mdec_write_register(0, 0xFE000000);
    const tick_count_t delay = timing_event_get_ticks_until_next_execution(find_mdec_copy_event());
    EXPECT_TRUE(read_coefficients().pending);
    mdec_write_register(0, 0x12345678);
    EXPECT_EQ((mdec_dbg_fifo_status() >> 11) & 0x7FFu, 2);
    mdec_write_register(4, 0x80000000);
    const u32 reset_status = mdec_read_register(4);
    EXPECT_FALSE(read_coefficients().pending);
    EXPECT_EQ(mdec_dbg_fifo_status() & 0x00FFFFFFu, 0);
    run_slice(delay + 1);
    const u32 after_deadline = mdec_read_register(4);
    end_decode();
    EXPECT_EQ(reset_status, 0x80040000);
    EXPECT_EQ(after_deadline, reset_status);
  }
}

TEST(MdecReset, completed_output_is_discarded) {
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    mdec_write_register(4, 0x60000000);
    mdec_write_register(0, 0x28000001);
    mdec_write_register(0, 0xFE000000);
    const tick_count_t delay = timing_event_get_ticks_until_next_execution(find_mdec_copy_event());
    run_slice(delay + 1);
    EXPECT_EQ(mdec_read_register(0), 0x80808080);
    EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FFu, 15);
    mdec_write_register(4, 0x80000000);
    const u32 status = mdec_read_register(4);
    EXPECT_EQ(mdec_dbg_fifo_status() & 0x00FFFFFFu, 0);
    end_decode();
    EXPECT_EQ(status, 0x80040000);
  }
}

TEST(MdecReset, zero_counter_survives_state_restoration) {
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    mdec_write_register(4, 0x80000000);
    restore_coefficients();
    mdec_write_register(4, 0);
    const u32 status = mdec_read_register(4);
    end_decode();
    EXPECT_EQ(status, 0x80040000);
  }
}
