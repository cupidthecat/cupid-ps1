/*
 * gsvector ops smoke + bit-exact checks. Each op is a static-inline rename
 * of the SSE4.1 / AVX2 intrinsic, so these tests verify the wrapper wires
 * the right intrinsic — not algorithmic correctness of the intrinsic itself.
 *
 * Tests group by op family. Where the op is itself an emulation (e.g.
 * gsv4i_runion using min/max/blend), the test asserts the composite
 * behavior, not an alternative implementation.
 */

#include "tests/test_harness.h"

#include "common/gsvector.h"

#include <stdalign.h>
#include <string.h>

/* Helpers: load/extract a vector via aligned u32[4] for inspection. */
static void v4i_unpack(GSVector4i v, u32 out[4])
{
  alignas(16) u32 buf[4];
  gsv4i_store_a(buf, v);
  memcpy(out, buf, sizeof(buf));
}

static void v4i_unpack16(GSVector4i v, u16 out[8])
{
  alignas(16) u16 buf[8];
  gsv4i_store_a(buf, v);
  memcpy(out, buf, sizeof(buf));
}

TEST(GSVector4i, ZeroAndSet)
{
  u32 b[4];
  v4i_unpack(gsv4i_zero(), b);
  EXPECT_EQ(b[0], 0u); EXPECT_EQ(b[1], 0u); EXPECT_EQ(b[2], 0u); EXPECT_EQ(b[3], 0u);

  v4i_unpack(gsv4i_set32(0x1234u), b);
  EXPECT_EQ(b[0], 0x1234u); EXPECT_EQ(b[3], 0x1234u);

  v4i_unpack(gsv4i_setr32(1, 2, 3, 4), b);
  EXPECT_EQ(b[0], 1u); EXPECT_EQ(b[1], 2u); EXPECT_EQ(b[2], 3u); EXPECT_EQ(b[3], 4u);
}

TEST(GSVector4i, AddSub32)
{
  GSVector4i a = gsv4i_setr32(10, 20, 30, 40);
  GSVector4i b = gsv4i_setr32(1, 2, 3, 4);
  u32 r[4];
  v4i_unpack(gsv4i_add32(a, b), r);
  EXPECT_EQ(r[0], 11u); EXPECT_EQ(r[3], 44u);
  v4i_unpack(gsv4i_sub32(a, b), r);
  EXPECT_EQ(r[0], 9u); EXPECT_EQ(r[3], 36u);
}

TEST(GSVector4i, Mul16Lo)
{
  GSVector4i a = gsv4i_setr16(2, 3, 4, 5, 6, 7, 8, 9);
  GSVector4i b = gsv4i_setr16(10, 10, 10, 10, 10, 10, 10, 10);
  u16 r[8];
  v4i_unpack16(gsv4i_mul16l(a, b), r);
  for (int i = 0; i < 8; i++)
    EXPECT_EQ(r[i], (u16)((i + 2) * 10));
}

TEST(GSVector4i, Mul32Lo)
{
  GSVector4i a = gsv4i_setr32(7, 11, 13, 17);
  GSVector4i b = gsv4i_setr32(3, 5, 7, 11);
  u32 r[4];
  v4i_unpack(gsv4i_mul32l(a, b), r);
  EXPECT_EQ(r[0], 21u); EXPECT_EQ(r[1], 55u); EXPECT_EQ(r[2], 91u); EXPECT_EQ(r[3], 187u);
}

TEST(GSVector4i, Shifts32Imm)
{
  GSVector4i a = gsv4i_setr32(1, 2, 4, 8);
  u32 r[4];
  v4i_unpack(gsv4i_sll32(a, 2), r);
  EXPECT_EQ(r[0], 4u); EXPECT_EQ(r[3], 32u);
  v4i_unpack(gsv4i_srl32(a, 1), r);
  EXPECT_EQ(r[0], 0u); EXPECT_EQ(r[3], 4u);
}

TEST(GSVector4i, Sra16Imm)
{
  /* sra16 = arithmetic right shift on 8 lanes of s16. */
  GSVector4i a = gsv4i_setr16((s16)-16, (s16)-8, (s16)-4, (s16)-2,
                              (s16) 16, (s16) 32, (s16) 64, (s16)128);
  u16 r[8];
  v4i_unpack16(gsv4i_sra16(a, 1), r);
  EXPECT_EQ((s16)r[0], (s16)-8);
  EXPECT_EQ((s16)r[3], (s16)-1);
  EXPECT_EQ((s16)r[4], (s16)8);
  EXPECT_EQ((s16)r[7], (s16)64);
}

TEST(GSVector4i, LogicalOps)
{
  GSVector4i a = gsv4i_setr32(0xF0F0F0F0u, 0x0F0F0F0Fu, 0xAAAAAAAAu, 0x55555555u);
  GSVector4i b = gsv4i_setr32(0x0F0F0F0Fu, 0xF0F0F0F0u, 0xFFFFFFFFu, 0x00000000u);
  u32 r[4];
  v4i_unpack(gsv4i_and(a, b), r);
  EXPECT_EQ(r[0], 0u); EXPECT_EQ(r[1], 0u); EXPECT_EQ(r[2], 0xAAAAAAAAu); EXPECT_EQ(r[3], 0u);
  v4i_unpack(gsv4i_or(a, b), r);
  EXPECT_EQ(r[0], 0xFFFFFFFFu);
  v4i_unpack(gsv4i_xor(a, b), r);
  EXPECT_EQ(r[0], 0xFFFFFFFFu);
  /* andnot: ~this & mask. */
  v4i_unpack(gsv4i_andnot(a, b), r);
  /* lane 0: ~0xF0F0F0F0 & 0x0F0F0F0F = 0x0F0F0F0F & 0x0F0F0F0F = 0x0F0F0F0F */
  EXPECT_EQ(r[0], 0x0F0F0F0Fu);
}

TEST(GSVector4i, CompareAndTest)
{
  GSVector4i a = gsv4i_setr32(1, 2, 3, 4);
  GSVector4i b = gsv4i_setr32(1, 5, 3, 6);
  u32 r[4];
  v4i_unpack(gsv4i_eq32(a, b), r);
  EXPECT_EQ(r[0], 0xFFFFFFFFu); EXPECT_EQ(r[1], 0u);
  EXPECT_EQ(r[2], 0xFFFFFFFFu); EXPECT_EQ(r[3], 0u);

  v4i_unpack(gsv4i_lt32(a, b), r);
  EXPECT_EQ(r[0], 0u); EXPECT_EQ(r[1], 0xFFFFFFFFu);
  EXPECT_EQ(r[2], 0u); EXPECT_EQ(r[3], 0xFFFFFFFFu);

  EXPECT_TRUE(gsv4i_alltrue(gsv4i_set32((s32)0xFFFFFFFFu)));
  EXPECT_FALSE(gsv4i_alltrue(gsv4i_setr32(-1, -1, -1, 0)));
  EXPECT_TRUE(gsv4i_allfalse(gsv4i_zero()));
  EXPECT_FALSE(gsv4i_allfalse(gsv4i_set32(1)));
}

TEST(GSVector4i, MinMax)
{
  GSVector4i a = gsv4i_setr32(1, -5, 100, -1);
  GSVector4i b = gsv4i_setr32(2, -3, 50, 0);
  u32 r[4];
  v4i_unpack(gsv4i_min_s32(a, b), r);
  EXPECT_EQ((s32)r[0], 1); EXPECT_EQ((s32)r[1], -5); EXPECT_EQ((s32)r[2], 50); EXPECT_EQ((s32)r[3], -1);
  v4i_unpack(gsv4i_max_s32(a, b), r);
  EXPECT_EQ((s32)r[0], 2); EXPECT_EQ((s32)r[1], -3); EXPECT_EQ((s32)r[2], 100); EXPECT_EQ((s32)r[3], 0);

  GSVector4i c = gsv4i_setr16((s16)-5, (s16)10, (s16)0, (s16)20,
                              (s16)15, (s16)-1, (s16)100, (s16)50);
  GSVector4i d = gsv4i_setr16((s16)5, (s16)8, (s16)10, (s16)18,
                              (s16)0, (s16)50, (s16)80, (s16)60);
  u16 r16[8];
  v4i_unpack16(gsv4i_max_s16(c, d), r16);
  EXPECT_EQ((s16)r16[0], (s16)5);
  EXPECT_EQ((s16)r16[7], (s16)60);
  v4i_unpack16(gsv4i_min_u16(c, d), r16);
  EXPECT_EQ(r16[0], 5u);  /* min(-5, 5) as u16: -5 = 0xFFFB, 5 -> 5 */
  EXPECT_EQ(r16[2], 0u);
}

TEST(GSVector4i, ExtractInsert)
{
  GSVector4i a = gsv4i_setr32(11, 22, 33, 44);
  EXPECT_EQ(gsv4i_extract32(a, 0), 11);
  EXPECT_EQ(gsv4i_extract32(a, 3), 44);
  GSVector4i b = gsv4i_insert32(a, 99, 1);
  u32 r[4];
  v4i_unpack(b, r);
  EXPECT_EQ(r[1], 99u);
  EXPECT_EQ(r[0], 11u);

  GSVector4i c = gsv4i_setr16((s16)1, (s16)2, (s16)3, (s16)4,
                              (s16)5, (s16)6, (s16)7, (s16)8);
  EXPECT_EQ(gsv4i_extract16(c, 0), 1u);
  EXPECT_EQ(gsv4i_extract16(c, 7), 8u);
  GSVector4i d = gsv4i_insert16(c, 0xBEEF, 4);
  u16 r16[8];
  v4i_unpack16(d, r16);
  EXPECT_EQ(r16[4], 0xBEEFu);
}

TEST(GSVector4i, Conversion)
{
  /* u8to16: low 8 u8 lanes -> 8 u16 lanes. Upper 8 u8 ignored. */
  alignas(16) u8 src[16] = { 1, 2, 3, 4, 5, 6, 7, 8,  100, 100, 100, 100, 100, 100, 100, 100 };
  GSVector4i v = gsv4i_load_a(src);
  u16 r16[8];
  v4i_unpack16(gsv4i_u8to16(v), r16);
  for (int i = 0; i < 8; i++) EXPECT_EQ(r16[i], (u16)(i + 1));

  /* u16to32: low 4 u16 lanes -> 4 u32 lanes. */
  GSVector4i w = gsv4i_setr16((s16)10, (s16)20, (s16)30, (s16)40,
                              (s16)0xFFFF, (s16)0xFFFF, (s16)0xFFFF, (s16)0xFFFF);
  u32 r32[4];
  v4i_unpack(gsv4i_u16to32(w), r32);
  EXPECT_EQ(r32[0], 10u); EXPECT_EQ(r32[3], 40u);

  /* zext32: scalar u32 -> lane 0, others zero. */
  v4i_unpack(gsv4i_zext32(0xCAFEBABEu), r32);
  EXPECT_EQ(r32[0], 0xCAFEBABEu);
  EXPECT_EQ(r32[1], 0u); EXPECT_EQ(r32[2], 0u); EXPECT_EQ(r32[3], 0u);

  /* pu32: pack 4×s32 -> 4×u16 with unsigned saturation, upper 4 = 0. */
  GSVector4i p = gsv4i_setr32(0x1234, 0xFFFFF, -1, 100);
  v4i_unpack16(gsv4i_pu32(p), r16);
  EXPECT_EQ(r16[0], 0x1234u);
  EXPECT_EQ(r16[1], 0xFFFFu);  /* saturated */
  EXPECT_EQ(r16[2], 0u);       /* -1 saturates to 0 (unsigned) */
  EXPECT_EQ(r16[3], 100u);
  EXPECT_EQ(r16[4], 0u);       /* upper half = 0 */
}

TEST(GSVector4i, Shuffles)
{
  GSVector4i a = gsv4i_setr16((s16)0xAA, (s16)0xBB, (s16)0xCC, (s16)0xDD,
                              (s16)5, (s16)6, (s16)7, (s16)8);
  u16 r[8];
  /* xxxxl: lane 0 broadcast to lower 4 lanes. */
  v4i_unpack16(gsv4i_xxxxl(a), r);
  EXPECT_EQ(r[0], 0xAAu); EXPECT_EQ(r[1], 0xAAu); EXPECT_EQ(r[2], 0xAAu); EXPECT_EQ(r[3], 0xAAu);
  EXPECT_EQ(r[4], 5u);    /* upper 4 unchanged */
  /* yyyyl: lane 1 broadcast. */
  v4i_unpack16(gsv4i_yyyyl(a), r);
  EXPECT_EQ(r[0], 0xBBu); EXPECT_EQ(r[3], 0xBBu);
  EXPECT_EQ(r[4], 5u);
}

TEST(GSVector4i, Blend16Imm)
{
  GSVector4i a = gsv4i_setr16((s16)1, (s16)2, (s16)3, (s16)4, (s16)5, (s16)6, (s16)7, (s16)8);
  GSVector4i b = gsv4i_setr16((s16)10, (s16)20, (s16)30, (s16)40, (s16)50, (s16)60, (s16)70, (s16)80);
  /* imm=0xAA = 10101010 -> take b for odd lanes, a for even. */
  u16 r[8];
  v4i_unpack16(gsv4i_blend16(a, b, 0xAA), r);
  EXPECT_EQ(r[0], 1u);  EXPECT_EQ(r[1], 20u);
  EXPECT_EQ(r[2], 3u);  EXPECT_EQ(r[3], 40u);
  EXPECT_EQ(r[6], 7u);  EXPECT_EQ(r[7], 80u);
}

TEST(GSVector4i, Blend8Variable)
{
  GSVector4i a = gsv4i_set8((s8)0x11);
  GSVector4i b = gsv4i_set8((s8)0x22);
  /* Mask: high bit set in lanes 0,2,4,...; clear in 1,3,... */
  alignas(16) u8 mb[16] = {0x80,0,0x80,0,0x80,0,0x80,0,0x80,0,0x80,0,0x80,0,0x80,0};
  GSVector4i mask = gsv4i_load_a(mb);
  GSVector4i r = gsv4i_blend8(a, b, mask);
  alignas(16) u8 ob[16];
  gsv4i_store_a(ob, r);
  for (int i = 0; i < 16; i++)
    EXPECT_EQ(ob[i], (i % 2 == 0) ? 0x22u : 0x11u);
}

TEST(GSVector4i, LoadStore)
{
  alignas(16) u32 src[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
  alignas(16) u32 dst[4] = { 0 };
  GSVector4i v = gsv4i_load_a(src);
  gsv4i_store_a(dst, v);
  EXPECT_EQ(dst[0], 0x11111111u); EXPECT_EQ(dst[3], 0x44444444u);

  /* Unaligned load/store. */
  u8 buf[32] = { 0 };
  for (int i = 0; i < 16; i++) buf[i + 1] = (u8)(0xA0 + i);
  GSVector4i u = gsv4i_load_u(buf + 1);
  u8 out[16];
  gsv4i_store_u(out, u);
  for (int i = 0; i < 16; i++) EXPECT_EQ(out[i], (u8)(0xA0 + i));

  /* loadl: low 64 bits only; high lanes irrelevant. */
  alignas(16) u16 lsrc[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };
  GSVector4i lo = gsv4i_loadl_a(lsrc);
  alignas(16) u16 ldst[8] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0 };
  gsv4i_storel_a(ldst, lo);
  EXPECT_EQ(ldst[0], 0x1111u); EXPECT_EQ(ldst[3], 0x4444u);
}

#ifdef __AVX2__
TEST(GSVector8i, BasicOps)
{
  GSVector8i a = gsv8i_setr32(1, 2, 3, 4, 5, 6, 7, 8);
  GSVector8i b = gsv8i_set32(10);
  GSVector8i c = gsv8i_add32(a, b);
  alignas(32) u32 r[8];
  gsv8i_store_a(r, c);
  for (int i = 0; i < 8; i++) EXPECT_EQ(r[i], (u32)(i + 11));

  /* low128 / high128 split. */
  GSVector4i lo = gsv8i_low128(a);
  GSVector4i hi = gsv8i_high128(a);
  u32 r4[4];
  v4i_unpack(lo, r4);
  EXPECT_EQ(r4[0], 1u); EXPECT_EQ(r4[3], 4u);
  v4i_unpack(hi, r4);
  EXPECT_EQ(r4[0], 5u); EXPECT_EQ(r4[3], 8u);

  /* broadcast128: replicate 128 bits to both halves. */
  GSVector4i half = gsv4i_setr32(100, 200, 300, 400);
  GSVector8i bcast = gsv8i_broadcast128(half);
  gsv8i_store_a(r, bcast);
  EXPECT_EQ(r[0], 100u); EXPECT_EQ(r[3], 400u);
  EXPECT_EQ(r[4], 100u); EXPECT_EQ(r[7], 400u);

  /* u16to32 of 8 u16 lanes -> 8 u32 lanes. */
  GSVector4i hw = gsv4i_setr16((s16)10, (s16)20, (s16)30, (s16)40,
                               (s16)50, (s16)60, (s16)70, (s16)80);
  GSVector8i wide = gsv8i_u16to32(hw);
  gsv8i_store_a(r, wide);
  for (int i = 0; i < 8; i++) EXPECT_EQ(r[i], (u32)((i + 1) * 10));
}

TEST(GSVector8i, VariableShift)
{
  GSVector8i v  = gsv8i_setr32(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
  GSVector8i sh = gsv8i_setr32(0, 1, 2, 3, 4, 5, 6, 7);
  GSVector8i r  = gsv8i_srlv32(v, sh);
  alignas(32) u32 out[8];
  gsv8i_store_a(out, r);
  for (int i = 0; i < 8; i++) EXPECT_EQ(out[i], (u32)(0xFF >> i));
}
#endif /* __AVX2__ */
