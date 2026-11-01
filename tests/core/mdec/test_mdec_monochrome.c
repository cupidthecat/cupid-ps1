/* cupid-ps1 monochrome pixels keep their own byte or nibble when packed. */
#include "tests/core/mdec/mdec_decode_fixture.h"

static void load_legacy_pending_pixels(void)
{
  u8 bytes[8192]; state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, sizeof(bytes), SAVE_STATE_VERSION);
  EXPECT_TRUE(mdec_do_state(&sw));
  const size_t size = state_wrapper_get_position(&sw);
  /* The RGB array precedes the pending-copy flag and two DMA layout bytes in the existing record. */
  const size_t start = size - 256 * sizeof(u32) - 3 * sizeof(u8);
  for (u32 sample = 0; sample < 64; sample++) {
    u32 value; memcpy(&value, bytes + start + sample * sizeof(value), sizeof(value));
    value = (u32)(s32)(s8)(u8)value;
    memcpy(bytes + start + sample * sizeof(value), &value, sizeof(value));
  }
  state_wrapper_init_read(&sw, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(mdec_do_state(&sw)); EXPECT_FALSE(state_wrapper_has_error(&sw));
}

static void check_monochrome(bool signed_output, u32 depth, bool restored, bool legacy)
{
  const s16 dc_values[] = {-512,-448,-384,-320,-256,-192,-128,-64,0,64,128,192,256,320,384,448};
  const s16 byte_values[] = {-512,-448,-256,-128,-64,-32,-4,0,4,32,64,128,256,448,508};
  const s16* values = depth ? byte_values : dc_values;
  const u32 count = depth ? countof(byte_values) : countof(dc_values);
  u32 failures = 0, observations = 0;
  for (u32 old = 0; old < 2; old++) {
    begin_decode(old != 0);
    for (u32 input_dma = 0; input_dma < 2; input_dma++) {
      for (u32 output_dma = 0; output_dma < 2; output_dma++) {
        for (u32 index = 0; index <= count; index++) {
          const bool mixed = index == count;
          const s16 dc = mixed ? 128 : values[index];
          mdec_reset(); mdec_write_register(4, 0x60000000);
          mdec_write_register(0, 0x40000000);
          for (u32 word = 0; word < 16; word++) mdec_write_register(0, 0x01010101);
          mdec_write_register(0, 0x60000000);
          for (u32 word = 0; word < 32; word++)
            mdec_write_register(0, mixed ? 0x80017FFF : 0x7FFF7FFF);
          mdec_write_register(0, 0x20000001 | (depth << 27) | (signed_output ? 0x04000000 : 0));
          const u32 encoded = 0xFE000400u | ((u32)(u16)dc & 0x3FF);
          if (input_dma) mdec_dma_write(&encoded, 1);
          else mdec_write_register(0, encoded);
          const tick_count_t delay = timing_event_get_ticks_until_next_execution(find_mdec_copy_event());
          if (legacy) load_legacy_pending_pixels();
          if (restored) restore_coefficients();
          run_slice(delay + 1);
          /* DC-only input gives a constant block, or a +/-32 checkerboard
           * with the alternating-sign matrix. Nibble levels avoid ties. */
          const s32 luma = dc / 4;
          const u32 byte = (u8)(luma + (signed_output ? 0 : 128));
          const u32 pixel = depth ? byte : byte >> 4;
          const u32 width = depth ? 8 : 4;
          u32 expected = 0;
          for (u32 shift = 0; shift < 32; shift += width) expected |= pixel << shift;
          const coefficient_snapshot_t snapshot = read_coefficients();
          for (u32 sample = 0; sample < 64; sample++) {
            const s32 level = mixed && (((sample / 8) + (sample % 8)) & 1) ? -luma : luma;
            const u32 stored = legacy ? (u32)level : (u8)(level + (signed_output ? 0 : 128));
            failures += snapshot.rgb[sample] != stored;
          }
          const u32 even = depth ? (signed_output ? 0xE020E020u : 0x60A060A0u) :
                                  (signed_output ? 0xE2E2E2E2u : 0x6A6A6A6Au);
          const u32 odd = depth ? (signed_output ? 0x20E020E0u : 0xA060A060u) :
                                 (signed_output ? 0x2E2E2E2Eu : 0xA6A6A6A6u);
          const u32 words = depth ? 16 : 8;
          for (u32 word = 0; word < words; word++) {
            u32 value;
            if (output_dma) mdec_dma_read(&value, 1);
            else value = mdec_read_register(0);
            const u32 word_expected = mixed ? (((word / (depth ? 2u : 1u)) & 1u) ? odd : even) : expected;
            failures += value != word_expected;
            if (word == 0 && restored) restore_coefficients();
          }
          failures += (mdec_read_register(4) & 0x88000000u) != 0x80000000u;
          observations++;
        }
      }
    }
    end_decode();
  }
  EXPECT_EQ(observations, (count + 1) * 8); EXPECT_EQ(failures, 0);
}

TEST(MdecMonochrome, unsigned_bytes) { check_monochrome(false, 1, false, false); }
TEST(MdecMonochrome, restored_unsigned_bytes) { check_monochrome(false, 1, true, false); }
TEST(MdecMonochrome, signed_bytes) { check_monochrome(true, 1, false, false); }
TEST(MdecMonochrome, restored_signed_bytes) { check_monochrome(true, 1, true, false); }
TEST(MdecMonochrome, unsigned_aligned_nibbles) { check_monochrome(false, 0, false, false); }
TEST(MdecMonochrome, restored_unsigned_aligned_nibbles) { check_monochrome(false, 0, true, false); }
TEST(MdecMonochrome, signed_aligned_nibbles) { check_monochrome(true, 0, false, false); }
TEST(MdecMonochrome, restored_signed_aligned_nibbles) { check_monochrome(true, 0, true, false); }
TEST(MdecMonochrome, legacy_pending_bytes) { check_monochrome(true, 1, true, true); }
TEST(MdecMonochrome, legacy_pending_aligned_nibbles) { check_monochrome(true, 0, true, true); }
