/* cupid-ps1 MDEC record bounds, failed-load events and legacy scale layout. */
#include "tests/core/mdec/mdec_decode_fixture.h"

enum {
  RECORD_IN          = sizeof(u32) + 2 * sizeof(u8),
  RECORD_OUT         = RECORD_IN + 3 * sizeof(u32) + 512 * sizeof(u16),
  RECORD_STATE       = RECORD_OUT + 3 * sizeof(u32) + 192 * sizeof(u32),
  RECORD_REMAINING   = RECORD_STATE + sizeof(u8),
  RECORD_SCALE       = RECORD_REMAINING + sizeof(u32) + 128 * sizeof(u8),
  RECORD_BLOCK       = RECORD_SCALE + 64 * sizeof(s16) + 6 * 64 * sizeof(s16),
  RECORD_COEFFICIENT = RECORD_BLOCK + sizeof(u32),
  RECORD_QSCALE      = RECORD_COEFFICIENT + sizeof(u32),
};

static size_t save_mdec_record(u8* bytes, size_t capacity, u32 version) {
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, capacity, version);
  EXPECT_TRUE(mdec_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  return state_wrapper_get_position(&sw);
}

TEST(MdecState, invalid_fields_do_not_resume_copy_out) {
  const struct {
    size_t offset;
    u32    value, width;
  } cases[] = {
      {RECORD_IN, 512, 4},      {RECORD_IN + 4, 512, 4},     {RECORD_IN + 8, 513, 4}, {RECORD_OUT, 192, 4},
      {RECORD_OUT + 4, 192, 4}, {RECORD_OUT + 8, 193, 4},    {RECORD_STATE, 6, 1},    {RECORD_REMAINING, 0x20001, 4},
      {RECORD_BLOCK, 7, 4},     {RECORD_COEFFICIENT, 65, 4}, {RECORD_QSCALE, 64, 2},  {RECORD_STATE, 0, 1},
  };
  u32 failures = 0;
  for (u32 field = 0; field < countof(cases); field++) {
    begin_decode(false);
    mdec_write_register(0, 0x28000001);
    mdec_write_register(0, 0xFE000000);
    timing_event_t* event = find_mdec_copy_event();
    u8              bytes[8192];
    const size_t    size = save_mdec_record(bytes, sizeof(bytes), SAVE_STATE_VERSION);
    timing_event_deactivate(event);
    for (u32 byte = 0; byte < cases[field].width; byte++)
      bytes[cases[field].offset + byte] = (u8)(cases[field].value >> (8 * byte));
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, bytes, size, SAVE_STATE_VERSION);
    const bool accepted = mdec_do_state(&sw);
    failures += accepted || !state_wrapper_has_error(&sw) || timing_event_is_active(event);
    mdec_reset();
    end_decode();
  }
  EXPECT_EQ(failures, 0);
}

TEST(MdecState, truncated_reads_cancel_copy_out) {
  u32 failures = 0;
  for (u32 legacy = 0; legacy < 2; legacy++) {
    begin_decode(false);
    mdec_write_register(0, 0x28000001);
    mdec_write_register(0, 0xFE000000);
    u8           modern[8192], bytes[8192];
    const size_t modern_size = save_mdec_record(modern, sizeof(modern), SAVE_STATE_VERSION);
    const u32    version     = legacy ? 65 : SAVE_STATE_VERSION;
    const size_t size        = save_mdec_record(bytes, sizeof(bytes), version);
    const size_t lengths[]   = {0, RECORD_IN + 8, RECORD_SCALE + 1, size - 1};
    for (u32 index = 0; index < countof(lengths); index++) {
      state_wrapper_t sw;
      state_wrapper_init_read(&sw, modern, modern_size, SAVE_STATE_VERSION);
      EXPECT_TRUE(mdec_do_state(&sw));
      timing_event_t* event = find_mdec_copy_event();
      state_wrapper_init_read(&sw, bytes, lengths[index], version);
      const bool accepted = mdec_do_state(&sw);
      failures += accepted || !state_wrapper_has_error(&sw) || timing_event_is_active(event);
    }
    mdec_reset();
    end_decode();
  }
  EXPECT_EQ(failures, 0);
}

TEST(MdecState, legacy_matrix_write_preserves_state_and_round_trips) {
  begin_decode(false);
  mdec_write_register(0, 0x60000000);
  for (u32 word = 0; word < 32; word++)
    mdec_write_register(0, (0x0123u + word * 257u) | ((0xF321u - word * 127u) << 16));
  const coefficient_snapshot_t expected = read_coefficients();
  u8                           modern[8192], zero_legacy[8192], encoded[8192];
  const size_t                 size = save_mdec_record(modern, sizeof(modern), SAVE_STATE_VERSION);
  memcpy(zero_legacy, modern, size);
  memset(zero_legacy + RECORD_SCALE, 0, 64 * sizeof(u16));
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, zero_legacy, size, 65);
  EXPECT_TRUE(mdec_do_state(&sw));
  state_wrapper_init_read(&sw, modern, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(mdec_do_state(&sw));
  const size_t                 legacy_size = save_mdec_record(encoded, sizeof(encoded), 65);
  const coefficient_snapshot_t after_write = read_coefficients();
  mdec_reset();
  state_wrapper_init_read(&sw, encoded, legacy_size, 65);
  EXPECT_TRUE(mdec_do_state(&sw));
  const coefficient_snapshot_t restored = read_coefficients();
  end_decode();
  EXPECT_EQ(memcmp(expected.scale, after_write.scale, sizeof(expected.scale)), 0);
  EXPECT_EQ(memcmp(expected.scale, restored.scale, sizeof(expected.scale)), 0);
}
