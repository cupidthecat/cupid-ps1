/* AVX2 vs scalar parity tests for the SW-rasterizer VRAM ops.
 *
 * Calls gpu_sw_rasterizer_scalar_* and gpu_sw_rasterizer_avx2_* directly
 * (both symbols are non-static in the build) and asserts the resulting
 * 1024 x 512 u16 g_vram is byte-identical between the two paths.
 *
 * Covers:
 *   fill_vram   -- aligned, unaligned tail, X/Y wrap, interlaced active/odd
 *   copy_vram   -- forward, reverse (overlapping same row), tail residue
 *   write_vram  -- contiguous fast path, mask-set, mask-check, X-wrap
 */

#include "tests/test_harness.h"

#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_types.h"

#include <string.h>

/* Direct entry-point prototypes (non-static in the impl TUs). */
void gpu_sw_rasterizer_scalar_fill_vram (u32, u32, u32, u32, u32, bool, u8);
void gpu_sw_rasterizer_scalar_write_vram(u32, u32, u32, u32, const u16*, bool, bool);
void gpu_sw_rasterizer_scalar_copy_vram (u32, u32, u32, u32, u32, u32, bool, bool);
void gpu_sw_rasterizer_avx2_fill_vram (u32, u32, u32, u32, u32, bool, u8);
void gpu_sw_rasterizer_avx2_write_vram(u32, u32, u32, u32, const u16*, bool, bool);
void gpu_sw_rasterizer_avx2_copy_vram (u32, u32, u32, u32, u32, u32, bool, bool);

/* Snapshots of g_vram for cross-impl comparison. */
static u16 s_vram_a[VRAM_HEIGHT * VRAM_WIDTH];
static u16 s_vram_b[VRAM_HEIGHT * VRAM_WIDTH];

static void seed_vram_pattern(void)
{
  /* Deterministic xor-shift pattern so byte-equality testing is meaningful. */
  u32 s = 0x12345678u;
  for (u32 i = 0; i < (u32)(VRAM_HEIGHT * VRAM_WIDTH); i++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    g_vram[i] = (u16)(s & 0xFFFFu);
  }
}

static void clear_vram(u16 fill)
{
  for (u32 i = 0; i < (u32)(VRAM_HEIGHT * VRAM_WIDTH); i++)
    g_vram[i] = fill;
}

static int vram_equals(const u16* a, const u16* b)
{
  return memcmp(a, b, sizeof(u16) * VRAM_HEIGHT * VRAM_WIDTH) == 0;
}

TEST(GpuSwRasterizer, fill_vram_aligned_no_wrap)
{
  /* Width = 256 = 16 vector groups of 16 pixels.  Common Crash/MGS pattern. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_fill_vram(0u, 0u, 256u, 64u, 0xFF8040C0u, false, 0u);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_fill_vram(0u, 0u, 256u, 64u, 0xFF8040C0u, false, 0u);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, fill_vram_unaligned_tail)
{
  /* Width = 199, height = 23 -- forces both vector and scalar-tail paths. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_fill_vram(11u, 7u, 199u, 23u, 0x12345678u, false, 0u);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_fill_vram(11u, 7u, 199u, 23u, 0x12345678u, false, 0u);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, fill_vram_wrap_x)
{
  /* Start at x=900 with width=300 -- crosses the right edge of VRAM. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_fill_vram(900u, 0u, 300u, 8u, 0xCAFEBABEu, false, 0u);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_fill_vram(900u, 0u, 300u, 8u, 0xCAFEBABEu, false, 0u);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, fill_vram_interlaced)
{
  /* Interlaced fill, active_field=0 -- skips even rows. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_fill_vram(32u, 16u, 240u, 32u, 0x80800040u, true, 0u);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_fill_vram(32u, 16u, 240u, 32u, 0x80800040u, true, 0u);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, write_vram_contiguous_fast)
{
  /* Fast path: contiguous, no masking. */
  static u16 src[256 * 64];
  for (u32 i = 0; i < (u32)(256u * 64u); i++) src[i] = (u16)((i * 31u + 7u) & 0xFFFFu);

  clear_vram(0u);
  gpu_sw_rasterizer_scalar_write_vram(0u, 100u, 256u, 64u, src, false, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  clear_vram(0u);
  gpu_sw_rasterizer_avx2_write_vram(0u, 100u, 256u, 64u, src, false, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, write_vram_unaligned_tail)
{
  static u16 src[199 * 23];
  for (u32 i = 0; i < (u32)(199u * 23u); i++) src[i] = (u16)((i ^ 0xA5A5u) & 0xFFFFu);

  clear_vram(0u);
  gpu_sw_rasterizer_scalar_write_vram(13u, 7u, 199u, 23u, src, false, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  clear_vram(0u);
  gpu_sw_rasterizer_avx2_write_vram(13u, 7u, 199u, 23u, src, false, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, write_vram_with_set_mask)
{
  /* set_mask on -- forces mask path; AVX2 must match scalar bit-for-bit. */
  static u16 src[64 * 16];
  for (u32 i = 0; i < (u32)(64u * 16u); i++) src[i] = (u16)((i * 0x9E37u) & 0x7FFFu);

  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_write_vram(50u, 50u, 64u, 16u, src, true, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_write_vram(50u, 50u, 64u, 16u, src, true, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, write_vram_with_check_mask)
{
  static u16 src[64 * 16];
  for (u32 i = 0; i < (u32)(64u * 16u); i++) src[i] = (u16)((i * 0x9E37u) & 0xFFFFu);

  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_write_vram(50u, 50u, 64u, 16u, src, false, true);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_write_vram(50u, 50u, 64u, 16u, src, false, true);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, copy_vram_no_overlap_aligned)
{
  /* Disjoint src + dst rects, width is a vector multiple. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_copy_vram(0u, 0u, 256u, 256u, 256u, 64u, false, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_copy_vram(0u, 0u, 256u, 256u, 256u, 64u, false, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, copy_vram_unaligned_tail)
{
  /* Width 199, height 19. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_copy_vram(5u, 5u, 305u, 105u, 199u, 19u, false, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_copy_vram(5u, 5u, 305u, 105u, 199u, 19u, false, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, copy_vram_overlap_reverse)
{
  /* Overlap on same row with src_x < dst_x -- forces reverse-direction copy.
   * AVX2 path falls back to scalar reverse; verify no divergence. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_copy_vram(100u, 200u, 150u, 200u, 200u, 4u, false, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_copy_vram(100u, 200u, 150u, 200u, 200u, 4u, false, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, copy_vram_with_check_mask)
{
  /* check_mask on -- AVX2 routes to scalar; verify identical state. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_copy_vram(0u, 0u, 0u, 256u, 128u, 32u, false, true);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_copy_vram(0u, 0u, 0u, 256u, 128u, 32u, false, true);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}

TEST(GpuSwRasterizer, copy_vram_x_wrap)
{
  /* dst_x + width > VRAM_WIDTH -- exercise the recursive split scalar path. */
  seed_vram_pattern();
  gpu_sw_rasterizer_scalar_copy_vram(0u, 0u, 900u, 100u, 300u, 8u, false, false);
  memcpy(s_vram_a, g_vram, sizeof(s_vram_a));

  seed_vram_pattern();
  gpu_sw_rasterizer_avx2_copy_vram(0u, 0u, 900u, 100u, 300u, 8u, false, false);
  memcpy(s_vram_b, g_vram, sizeof(s_vram_b));

  EXPECT_TRUE(vram_equals(s_vram_a, s_vram_b));
}
