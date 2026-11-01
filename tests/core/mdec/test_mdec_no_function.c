/* cupid-ps1 no-function command status, sequencing and output preservation. */
#include "tests/core/mdec/mdec_decode_fixture.h"

static const u32 no_function_commands[] = {0, 4, 5, 6, 7};

TEST(MdecNoFunction, status_fields_and_unbiased_parameter_bits) {
  const u32 counts[] = {0, 1, 2, 0x7FFF, 0xFFFF};
  u32       failures = 0, observations = 0;
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    for (u32 command = 0; command < countof(no_function_commands); command++) {
      for (u32 count = 0; count < countof(counts); count++) {
        for (u32 flags = 0; flags < 16; flags++) {
          mdec_reset();
          mdec_write_register(4, 0x60000000);
          const u32 header   = (no_function_commands[command] << 29) | (flags << 25) | counts[count];
          const u32 expected = 0x90040000u | (flags << 23) | counts[count];
          mdec_write_register(0, header);
          failures += mdec_read_register(4) != expected;
          /* A control rewrite must not turn a latched count into a countdown. */
          mdec_write_register(4, 0x60000000);
          failures += mdec_read_register(4) != expected;
          observations++;
        }
      }
    }
    end_decode();
  }
  EXPECT_EQ(observations, 800);
  EXPECT_EQ(failures, 0);
}

static void check_next_command(bool restored) {
  const u32 counts[] = {0, 1, 0xFFFF};
  u32       failures = 0;
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    for (u32 command = 0; command < countof(no_function_commands); command++) {
      for (u32 count = 0; count < countof(counts); count++) {
        for (u32 table = 0; table < 2; table++) {
          mdec_reset();
          mdec_write_register(0, (no_function_commands[command] << 29) | counts[count]);
          if (restored) restore_coefficients();
          mdec_write_register(0, table ? 0x60000000 : 0x40000000);
          const u32 words = table ? 32 : 16;
          failures += mdec_read_register(4) != (0xA0040000u | (words - 1));
          const u32 value = 1 + command + 5 * count + 15 * old;
          for (u32 word = 0; word < words; word++)
            mdec_write_register(0, value * 0x01010101u);
          failures += mdec_read_register(4) != 0x8004FFFFu;
          const coefficient_snapshot_t snapshot = read_coefficients();
          for (u32 element = 0; element < 64; element++)
            failures += table ? snapshot.scale[element] != (s16)(value * 0x101) : snapshot.iq_y[element] != value;
        }
      }
    }
    end_decode();
  }
  EXPECT_EQ(failures, 0);
}

TEST(MdecNoFunction, following_table_command_is_not_a_parameter) {
  check_next_command(false);
}
TEST(MdecNoFunction, restored_next_command_is_not_a_parameter) {
  check_next_command(true);
}

TEST(MdecNoFunction, queued_dma_headers_preserve_command_boundaries) {
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    u32 words[20] = {0x0000FFFF, 0xE8000001, 0x40000000};
    for (u32 word = 3; word < 19; word++)
      words[word] = 0x01010101;
    words[19] = 0xB6004321;
    mdec_dma_write(words, countof(words));
    EXPECT_EQ(mdec_read_register(4), 0x85844321);
    end_decode();
  }
}

static void check_output_preservation(bool restored, bool dma_output) {
  for (u32 old = 0; old < 2; old++) {
    for (u32 command = 0; command < countof(no_function_commands); command++) {
      begin_decode(old != 0);
      mdec_write_register(4, 0x60000000);
      mdec_write_register(0, 0x28000001);
      mdec_write_register(0, 0xFE000000);
      const tick_count_t delay = timing_event_get_ticks_until_next_execution(find_mdec_copy_event());
      run_slice(delay + 1);
      EXPECT_EQ(mdec_read_register(4) & 0x88000000u, 0x08000000);
      mdec_write_register(0, (no_function_commands[command] << 29) | 0xFFFF);
      EXPECT_EQ(mdec_read_register(4) & 0xA800FFFFu, 0x0800FFFF);
      if (restored) restore_coefficients();
      u32 words[16];
      if (dma_output)
        mdec_dma_read(words, countof(words));
      else
        for (u32 word = 0; word < countof(words); word++)
          words[word] = mdec_read_register(0);
      for (u32 word = 0; word < countof(words); word++)
        EXPECT_EQ(words[word], 0x80808080);
      EXPECT_EQ(mdec_read_register(4) & 0x88000000u, 0x80000000);
      end_decode();
    }
  }
}

TEST(MdecNoFunction, decoded_output_survives_no_function_headers) {
  check_output_preservation(false, false);
}
TEST(MdecNoFunction, restored_output_survives_no_function_headers) {
  check_output_preservation(true, false);
}
TEST(MdecNoFunction, dma_output_survives_no_function_headers) {
  check_output_preservation(false, true);
}
TEST(MdecNoFunction, restored_dma_output_survives_no_function_headers) {
  check_output_preservation(true, true);
}
