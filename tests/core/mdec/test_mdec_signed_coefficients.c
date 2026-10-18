#include "tests/core/mdec/mdec_decode_fixture.h"

static void write_coefficients(bool dma, u32 word)
{
  if (dma)
    mdec_dma_write(&word, 1);
  else
    mdec_write_register(0, word);
}

static void check_signed_coefficients(bool ac, bool scaled)
{
  const s16 values[]        = {-512, -1, 0, 1, 511};
  const s16 unscaled[]      = {-16384, -32, 0, 32, 16352};
  const s16 dc_scaled[2][5] = {{-8184, -8, 0, 8, 8168}, {-16384, -4072, 0, 4072, 16383}};
  const s16 ac_scaled[2][5] = {{-1016, -8, 0, -8, 1000}, {-16384, -16384, 0, 16383, 16383}};
  const u8  tables[]        = {1, 255};
  begin_decode(false);
  u32 observations = 0;
  for (u32 table = 0; table < 2; table++) {
    mdec_reset();
    mdec_write_register(0, 0x40000000);
    for (u32 i = 0; i < 16; i++)
      mdec_write_register(0, (u32)tables[table] * 0x01010101u);
    for (u32 index = 0; index < 5; index++) {
      for (u32 restore = 0; restore < 2; restore++) {
        for (u32 dma = 0; dma < 2; dma++) {
          mdec_reset();
          mdec_write_register(0, 0x2800FFFF);
          const u32 qscale = scaled ? (ac && table ? 63 : 1) : 0;
          if (ac) write_coefficients(dma != 0, qscale << 10);
          if (restore) restore_coefficients();
          const u32 word = ((u32)(u16)values[index] & 0x3FFu) | (ac ? 0 : qscale << 10);
          write_coefficients(dma != 0, word);
          const coefficient_snapshot_t result = read_coefficients();
          const s16 expected = scaled ? (ac ? ac_scaled[table][index] : dc_scaled[table][index]) : unscaled[index];
          const u32 position = ac ? 1 : 0;
          u32       wrong    = 0;
          for (u32 i = 0; i < 64; i++)
            wrong += result.blocks[0][i] != (i == position ? expected : 0);
          printf("mdec-signed-coefficient ac=%u scaled=%u qt=%u qscale=%u input=%d restore=%u dma=%u "
                 "value=%d expected=%d wrong=%u status=%08X decoder=%u remaining=%u coefficient=%u\n",
                 ac, scaled, tables[table], qscale, values[index], restore, dma, result.blocks[0][position], expected,
                 wrong, result.status, result.decoder, result.remaining, result.coefficient);
          fflush(stdout);
          EXPECT_EQ(result.block, 0);
          EXPECT_EQ(result.coefficient, ac ? 3 : 1);
          EXPECT_EQ(result.qscale, qscale);
          EXPECT_EQ(result.remaining, 0xFFFFu * 2u - (ac ? 4u : 2u));
          EXPECT_FALSE(result.pending);
          EXPECT_EQ(wrong, 0);
          observations++;
        }
      }
    }
  }
  EXPECT_EQ(observations, 40);
  end_decode();
}

TEST(MdecSigned, dc_unscaled)
{
  check_signed_coefficients(false, false);
}
TEST(MdecSigned, dc_scaled)
{
  check_signed_coefficients(false, true);
}
TEST(MdecSigned, ac_unscaled)
{
  check_signed_coefficients(true, false);
}
TEST(MdecSigned, ac_scaled)
{
  check_signed_coefficients(true, true);
}
