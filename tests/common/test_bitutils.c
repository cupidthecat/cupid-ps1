 /*
 * union tests are skipped (cupid-ps1 uses plain C bitfields).  Tested here:
 * sign/zero extend, count leading/trailing zeros, byte swap, signN. */

#include "tests/test_harness.h"

#include "common/bitutils.h"

TEST(BitUtils, ZeroExtend)
{
  EXPECT_EQ(ZeroExtend16((u8)0x7F), 0x7Fu);
  EXPECT_EQ(ZeroExtend16((u8)0xFF), 0xFFu);
  EXPECT_EQ(ZeroExtend32_u8 ((u8)0xFF),  0xFFu);
  EXPECT_EQ(ZeroExtend32_u16((u16)0xFFFF), 0xFFFFu);
  EXPECT_EQ(ZeroExtend64_u32(0xFFFFFFFFu), 0xFFFFFFFFull);
}

TEST(BitUtils, SignExtend)
{
  EXPECT_EQ(SignExtend16((s8)0x7F), 0x007Fu);
  EXPECT_EQ(SignExtend16((s8)0x80), 0xFF80u);
  EXPECT_EQ(SignExtend16((s8)-1),   0xFFFFu);

  EXPECT_EQ(SignExtend32_s8 ((s8)0x7F), 0x0000007Fu);
  EXPECT_EQ(SignExtend32_s8 ((s8)-1),   0xFFFFFFFFu);
  EXPECT_EQ(SignExtend32_s16((s16)0x8000), 0xFFFF8000u);

  EXPECT_EQ(SignExtend64_s32((s32)-1), 0xFFFFFFFFFFFFFFFFull);
}

TEST(BitUtils, SignExtendN)
{
  /* 5-bit field: 0x1F (-1) -> 0xFFFFFFFF */
  EXPECT_EQ(SignExtendN_u32(0x1Fu, 5), 0xFFFFFFFFu);
  /* 5-bit field: 0x10 (-16) -> 0xFFFFFFF0 */
  EXPECT_EQ(SignExtendN_u32(0x10u, 5), 0xFFFFFFF0u);
  /* 5-bit field: 0x0F (+15) -> 0x0000000F */
  EXPECT_EQ(SignExtendN_u32(0x0Fu, 5), 0x0000000Fu);
  /* 16-bit field at 0x8000 -> 0xFFFF8000 */
  EXPECT_EQ(SignExtendN_u32(0x8000u, 16), 0xFFFF8000u);
}

TEST(BitUtils, BoolConvert)
{
  EXPECT_EQ(BoolToUInt8(true),  1u);
  EXPECT_EQ(BoolToUInt8(false), 0u);
  EXPECT_EQ(BoolToUInt32(true), 1u);
  EXPECT_FLOAT_NEAR(BoolToFloat(true),  1.0f, 1e-9f);
  EXPECT_FLOAT_NEAR(BoolToFloat(false), 0.0f, 1e-9f);
}

TEST(BitUtils, CountZeros)
{
  EXPECT_EQ(CountTrailingZeros_u32(0x00000001u), 0u);
  EXPECT_EQ(CountTrailingZeros_u32(0x00000010u), 4u);
  EXPECT_EQ(CountTrailingZeros_u32(0x80000000u), 31u);

  EXPECT_EQ(CountLeadingZeros_u32(0x80000000u), 0u);
  EXPECT_EQ(CountLeadingZeros_u32(0x00000001u), 31u);
  EXPECT_EQ(CountLeadingZeros_u32(0x00010000u), 15u);
}

TEST(BitUtils, ByteSwap)
{
  EXPECT_EQ(ByteSwap_u16(0x1234u),     0x3412u);
  EXPECT_EQ(ByteSwap_u32(0x12345678u), 0x78563412u);
  EXPECT_EQ(ByteSwap_u64(0x0102030405060708ull), 0x0807060504030201ull);
  EXPECT_EQ(ByteSwap_s16((s16)0x1234), (s16)0x3412);
}
