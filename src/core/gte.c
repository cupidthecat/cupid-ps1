/*
 * GTE math kernels.  The interpreter walks the 6-bit command field from
 * each COP2 instruction word and dispatches to one of the per-op kernels
 * below, all of which operate on the global gte_registers_t.
 *
 * Fixed-point conventions used throughout:
 *   - 16-bit IR registers, 32-bit MAC accumulators on the 0..3 lanes.
 *   - MAC0 saturates at 32 bits signed (MAC0_MIN_VALUE..MAC0_MAX_VALUE);
 *     MAC1..MAC3 saturate at 44 bits signed and the high bits get OR'd
 *     into the FLAG.error bit (see gte_check_mac_overflow_*).
 *   - The shift parameter ("sf" in the instruction) is either 0 or 12 -
 *     12 means "result is in 20.12 fixed point, shift right 12 to extract".
 *   - The lm parameter ("limit-min", instruction bit 10) clamps IR1..IR3
 *     at 0 instead of -0x8000.
 *   - UNRDivide implements the PSX's odd 17-bit unsigned reciprocal
 *     divide; the table below is sourced verbatim from the architectural
 *     description (Nocash psx-spx) and is required for bit-exact output.
 */

#include "gte.h"

#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "settings.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/types.h"

#include <string.h>

/* Architectural clamp limits. */
#define MAC0_MIN_VALUE     (-((s64)1 << 31))
#define MAC0_MAX_VALUE     (((s64)1 << 31) - 1)
#define MAC123_MIN_VALUE   (-((s64)1 << 43))
#define MAC123_MAX_VALUE   (((s64)1 << 43) - 1)
#define IR0_MIN_VALUE      ((s32)0x0000)
#define IR0_MAX_VALUE      ((s32)0x1000)
#define IR123_MIN_VALUE    (-((s32)1 << 15))
#define IR123_MAX_VALUE    (((s32)1 << 15) - 1)

static ALWAYS_INLINE s32 gte_clamp_s32(s32 v, s32 lo, s32 hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static ALWAYS_INLINE u32 gte_min_u32(u32 a, u32 b) { return a < b ? a : b; }

/* The GTE register file lives inside g_cpu_state (cpu_core.h:cpu_state_t::
 * gte_regs as a u32[64] for stable JIT offsets).  REGS aliases that storage
 * via a type pun so the GTE math kernels see the same memory the recompiler
 * MTC2/LWC2/CFC2 hooks write to.  Without this alias, recomp would write to
 * cpu_state.gte_regs while gte_execute() reads a separate static g_gte_regs
 * that never gets the MTC2/LWC2 inputs; silent divergence from interp. */
#define REGS (*((gte_registers_t*)&g_cpu_state.gte_regs))

static gte_aspect_ratio_t s_aspect_ratio = GTE_ASPECT_RATIO_NONE;
static u32 s_custom_aspect_ratio_numerator = 0u;
static u32 s_custom_aspect_ratio_denominator = 0u;
static u32 s_dbg_gte_op_counts[64];

/* Counts leading bits the way the LZCR register expects: invert the
 * input first if its sign bit is set so that LZCR reports leading 1s for
 * negative inputs (and leading 0s for positive). */
static ALWAYS_INLINE u32 gte_count_leading_bits(u32 value)
{
  if (value & UINT32_C(0x80000000))
    value ^= UINT32_C(0xFFFFFFFF);
  return (value == 0u) ? 32u : (u32)CountLeadingZeros_u32(value);
}

/* OR'd error bit: any of the high saturation bits being set forces
 * FLAG.error to 1. */
static ALWAYS_INLINE void gte_flag_update_error(void)
{
  REGS.FLAG.error = ((REGS.FLAG.bits & UINT32_C(0x7F87E000)) != 0u) ? 1u : 0u;
}

static ALWAYS_INLINE void gte_flag_clear(void) { REGS.FLAG.bits = 0u; }

/* Per-lane MAC saturation flag setters.  The C++ source used
 * CheckMACOverflow<index>(value) with a non-type template parameter; we
 * fold that down to per-N functions because the interpreter dispatches
 * each lane explicitly. */
#define DEFINE_CHECK_MAC_OVERFLOW(N, lo, hi, under_field, over_field)                \
  static ALWAYS_INLINE void gte_check_mac_overflow_##N(s64 value)                    \
  {                                                                                  \
    if (value < (lo))                                                                \
      REGS.FLAG.under_field = 1u;                                                    \
    else if (value > (hi))                                                           \
      REGS.FLAG.over_field = 1u;                                                     \
  }

DEFINE_CHECK_MAC_OVERFLOW(0, MAC0_MIN_VALUE, MAC0_MAX_VALUE, mac0_underflow, mac0_overflow)
DEFINE_CHECK_MAC_OVERFLOW(1, MAC123_MIN_VALUE, MAC123_MAX_VALUE, mac1_underflow, mac1_overflow)
DEFINE_CHECK_MAC_OVERFLOW(2, MAC123_MIN_VALUE, MAC123_MAX_VALUE, mac2_underflow, mac2_overflow)
DEFINE_CHECK_MAC_OVERFLOW(3, MAC123_MIN_VALUE, MAC123_MAX_VALUE, mac3_underflow, mac3_overflow)

#undef DEFINE_CHECK_MAC_OVERFLOW

/* Chained-add sign extension for partial MAC sums.  The hardware folds
 * MAC0 partial results to 31-bit and MAC1/2/3 to 44-bit; those are the
 * widths at which the FLAG overflow bits are actually defined.  Using 32
 * or 45 here would silently let one extra bit through and miscompute the
 * sign of the next add in the chain. */
static ALWAYS_INLINE s64 gte_sign_extend_mac0(s64 v)
{
  gte_check_mac_overflow_0(v);
  return (s64)SignExtendN_u64((u64)v, 31);
}
static ALWAYS_INLINE s64 gte_sign_extend_mac1(s64 v)
{
  gte_check_mac_overflow_1(v);
  return (s64)SignExtendN_u64((u64)v, 44);
}
static ALWAYS_INLINE s64 gte_sign_extend_mac2(s64 v)
{
  gte_check_mac_overflow_2(v);
  return (s64)SignExtendN_u64((u64)v, 44);
}
static ALWAYS_INLINE s64 gte_sign_extend_mac3(s64 v)
{
  gte_check_mac_overflow_3(v);
  return (s64)SignExtendN_u64((u64)v, 44);
}

/* MAC store: shift then write into the dr32[24+N] slot.  Shift comes
 * after overflow checking so the saturation bits reflect pre-shift
 * magnitude, matching hardware. */
static ALWAYS_INLINE void gte_truncate_set_mac0(s64 value, u8 shift)
{
  gte_check_mac_overflow_0(value);
  value >>= shift;
  REGS.dr32[24] = (u32)(u64)value;
}
static ALWAYS_INLINE void gte_truncate_set_mac1(s64 value, u8 shift)
{
  gte_check_mac_overflow_1(value);
  value >>= shift;
  REGS.dr32[25] = (u32)(u64)value;
}
static ALWAYS_INLINE void gte_truncate_set_mac2(s64 value, u8 shift)
{
  gte_check_mac_overflow_2(value);
  value >>= shift;
  REGS.dr32[26] = (u32)(u64)value;
}
static ALWAYS_INLINE void gte_truncate_set_mac3(s64 value, u8 shift)
{
  gte_check_mac_overflow_3(value);
  value >>= shift;
  REGS.dr32[27] = (u32)(u64)value;
}

/* IR clamp / store.  For IR0 the range is 0..0x1000; for IR1..IR3 the
 * range is -0x8000..0x7FFF (or 0..0x7FFF when lm is set).  In both cases
 * the saturation bit is sticky for the duration of the instruction. */
static ALWAYS_INLINE void gte_truncate_set_ir0(s32 value, bool lm)
{
  const s32 min = lm ? 0 : IR0_MIN_VALUE;
  const s32 max = IR0_MAX_VALUE;
  if (value < min) {
    value = min;
    REGS.FLAG.ir0_saturated = 1u;
  } else if (value > max) {
    value = max;
    REGS.FLAG.ir0_saturated = 1u;
  }
  REGS.dr32[8] = (u32)value;
}
static ALWAYS_INLINE void gte_truncate_set_ir1(s32 value, bool lm)
{
  const s32 min = lm ? 0 : IR123_MIN_VALUE;
  const s32 max = IR123_MAX_VALUE;
  if (value < min) {
    value = min;
    REGS.FLAG.ir1_saturated = 1u;
  } else if (value > max) {
    value = max;
    REGS.FLAG.ir1_saturated = 1u;
  }
  REGS.dr32[9] = (u32)value;
}
static ALWAYS_INLINE void gte_truncate_set_ir2(s32 value, bool lm)
{
  const s32 min = lm ? 0 : IR123_MIN_VALUE;
  const s32 max = IR123_MAX_VALUE;
  if (value < min) {
    value = min;
    REGS.FLAG.ir2_saturated = 1u;
  } else if (value > max) {
    value = max;
    REGS.FLAG.ir2_saturated = 1u;
  }
  REGS.dr32[10] = (u32)value;
}
static ALWAYS_INLINE void gte_truncate_set_ir3(s32 value, bool lm)
{
  const s32 min = lm ? 0 : IR123_MIN_VALUE;
  const s32 max = IR123_MAX_VALUE;
  if (value < min) {
    value = min;
    REGS.FLAG.ir3_saturated = 1u;
  } else if (value > max) {
    value = max;
    REGS.FLAG.ir3_saturated = 1u;
  }
  REGS.dr32[11] = (u32)value;
}

/* Combined MAC + IR store: write MAC, then sign-clip into IR with the
 * lm flag.  Used by every dot-product step in the matrix kernels. */
static ALWAYS_INLINE void gte_truncate_set_mac_ir1(s64 value, u8 shift, bool lm)
{
  gte_check_mac_overflow_1(value);
  value >>= shift;
  const s32 v32 = (s32)value;
  REGS.dr32[25] = (u32)v32;
  gte_truncate_set_ir1(v32, lm);
}
static ALWAYS_INLINE void gte_truncate_set_mac_ir2(s64 value, u8 shift, bool lm)
{
  gte_check_mac_overflow_2(value);
  value >>= shift;
  const s32 v32 = (s32)value;
  REGS.dr32[26] = (u32)v32;
  gte_truncate_set_ir2(v32, lm);
}
static ALWAYS_INLINE void gte_truncate_set_mac_ir3(s64 value, u8 shift, bool lm)
{
  gte_check_mac_overflow_3(value);
  value >>= shift;
  const s32 v32 = (s32)value;
  REGS.dr32[27] = (u32)v32;
  gte_truncate_set_ir3(v32, lm);
}

/* RGB clamp 0..0xFF for each color lane, sticky FLAG bit. */
static ALWAYS_INLINE u32 gte_truncate_rgb_r(s32 v)
{
  if (v < 0 || v > 0xFF) {
    REGS.FLAG.color_r_saturated = 1u;
    return (v < 0) ? 0u : 0xFFu;
  }
  return (u32)v;
}
static ALWAYS_INLINE u32 gte_truncate_rgb_g(s32 v)
{
  if (v < 0 || v > 0xFF) {
    REGS.FLAG.color_g_saturated = 1u;
    return (v < 0) ? 0u : 0xFFu;
  }
  return (u32)v;
}
static ALWAYS_INLINE u32 gte_truncate_rgb_b(s32 v)
{
  if (v < 0 || v > 0xFF) {
    REGS.FLAG.color_b_saturated = 1u;
    return (v < 0) ? 0u : 0xFFu;
  }
  return (u32)v;
}

/* OTZ clamp 0..0xFFFF (sticky on the same FLAG bit as SZ1..SZ3). */
static ALWAYS_INLINE void gte_set_otz(s32 value)
{
  if (value < 0) {
    REGS.FLAG.sz1_otz_saturated = 1u;
    value = 0;
  } else if (value > 0xFFFF) {
    REGS.FLAG.sz1_otz_saturated = 1u;
    value = 0xFFFF;
  }
  REGS.dr32[7] = (u32)value;
}

/* SXY FIFO push: clamp x/y to -1024..1023 and rotate the 3-entry FIFO.
 * Each FIFO slot packs the two halves into one u32, low 16 = x. */
static ALWAYS_INLINE void gte_push_sxy(s32 x, s32 y)
{
  if (x < -1024) { REGS.FLAG.sx2_saturated = 1u; x = -1024; }
  else if (x > 1023) { REGS.FLAG.sx2_saturated = 1u; x = 1023; }

  if (y < -1024) { REGS.FLAG.sy2_saturated = 1u; y = -1024; }
  else if (y > 1023) { REGS.FLAG.sy2_saturated = 1u; y = 1023; }

  REGS.dr32[12] = REGS.dr32[13]; /* SXY0 <- SXY1 */
  REGS.dr32[13] = REGS.dr32[14]; /* SXY1 <- SXY2 */
  REGS.dr32[14] = ((u32)x & 0xFFFFu) | ((u32)y << 16);
}

/* SZ FIFO push: 16-bit unsigned, 4-entry FIFO. */
static ALWAYS_INLINE void gte_push_sz(s32 value)
{
  if (value < 0) {
    REGS.FLAG.sz1_otz_saturated = 1u;
    value = 0;
  } else if (value > 0xFFFF) {
    REGS.FLAG.sz1_otz_saturated = 1u;
    value = 0xFFFF;
  }

  REGS.dr32[16] = REGS.dr32[17]; /* SZ0 <- SZ1 */
  REGS.dr32[17] = REGS.dr32[18]; /* SZ1 <- SZ2 */
  REGS.dr32[18] = REGS.dr32[19]; /* SZ2 <- SZ3 */
  REGS.dr32[19] = (u32)value;    /* SZ3 <- value */
}

/* RGB FIFO push from MAC1..MAC3.  SHR 4 (not /16) is significant: the
 * sign behavior on negative MACs is bit-shift rather than arithmetic
 * divide. */
static ALWAYS_INLINE void gte_push_rgb_from_mac(void)
{
  const u32 r = gte_truncate_rgb_r((s32)((u32)REGS.MAC1 >> 4));
  const u32 g = gte_truncate_rgb_g((s32)((u32)REGS.MAC2 >> 4));
  const u32 b = gte_truncate_rgb_b((s32)((u32)REGS.MAC3 >> 4));
  const u32 c = (u32)REGS.RGBC[3];

  REGS.dr32[20] = REGS.dr32[21];                        /* RGB0 <- RGB1 */
  REGS.dr32[21] = REGS.dr32[22];                        /* RGB1 <- RGB2 */
  REGS.dr32[22] = r | (g << 8) | (b << 16) | (c << 24);
}

static u32 gte_unr_divide(u32 lhs, u32 rhs)
{
  if (rhs * 2u <= lhs) {
    REGS.FLAG.divide_overflow = 1u;
    return 0x1FFFFu;
  }

  const u32 shift = (rhs == 0u) ? 16u : (u32)CountLeadingZeros_u32((u32)(u16)rhs) - 16u;
  /* CountLeadingZeros_u32 of a u16 cast back: subtract 16 to match the
   * CountLeadingZeros<u16>() returns. */
  lhs <<= shift;
  rhs <<= shift;

  static const u8 unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 
    0x00, /* one extra entry for "(d-7FC0h)/80h"=100h */
  };

  const u32 divisor = rhs | 0x8000u;
  const s32 x = (s32)(0x101 + (s32)(u32)unr_table[((divisor & 0x7FFFu) + 0x40u) >> 7]);
  const s32 d = (((s32)(u32)divisor * -x) + 0x80) >> 8;
  const u32 recip = (u32)(((x * (0x20000 + d)) + 0x80) >> 8);

  const u32 result = (u32)((((u64)lhs * (u64)recip) + (u64)0x8000) >> 16);

  return gte_min_u32(0x1FFFFu, result);
}

static void gte_mul_mat_vec(const s16* M, s16 Vx, s16 Vy, s16 Vz, u8 shift, bool lm)
{
#define M_AT(i, j) M[((i) * 3) + (j)]
  gte_truncate_set_mac_ir1(
    gte_sign_extend_mac1((s64)M_AT(0, 0) * (s64)Vx + (s64)M_AT(0, 1) * (s64)Vy)
      + (s64)M_AT(0, 2) * (s64)Vz,
    shift, lm);
  gte_truncate_set_mac_ir2(
    gte_sign_extend_mac2((s64)M_AT(1, 0) * (s64)Vx + (s64)M_AT(1, 1) * (s64)Vy)
      + (s64)M_AT(1, 2) * (s64)Vz,
    shift, lm);
  gte_truncate_set_mac_ir3(
    gte_sign_extend_mac3((s64)M_AT(2, 0) * (s64)Vx + (s64)M_AT(2, 1) * (s64)Vy)
      + (s64)M_AT(2, 2) * (s64)Vz,
    shift, lm);
#undef M_AT
}

/* Translation-vector form: T<<12 + M*V, with sign-extension after each
 * partial sum to model the hardware's chained 44-bit accumulator. */
static void gte_mul_mat_vec_t(const s16* M, const s32 T[3], s16 Vx, s16 Vy, s16 Vz, u8 shift,
                              bool lm)
{
#define M_AT(i, j) M[((i) * 3) + (j)]
  gte_truncate_set_mac_ir1(
    gte_sign_extend_mac1(gte_sign_extend_mac1(((s64)T[0] << 12) + (s64)M_AT(0, 0) * (s64)Vx)
                         + (s64)M_AT(0, 1) * (s64)Vy)
      + (s64)M_AT(0, 2) * (s64)Vz, 
    shift, lm);
  gte_truncate_set_mac_ir2(
    gte_sign_extend_mac2(gte_sign_extend_mac2(((s64)T[1] << 12) + (s64)M_AT(1, 0) * (s64)Vx)
                         + (s64)M_AT(1, 1) * (s64)Vy)
      + (s64)M_AT(1, 2) * (s64)Vz, 
    shift, lm);
  gte_truncate_set_mac_ir3(
    gte_sign_extend_mac3(gte_sign_extend_mac3(((s64)T[2] << 12) + (s64)M_AT(2, 0) * (s64)Vx)
                         + (s64)M_AT(2, 1) * (s64)Vy)
      + (s64)M_AT(2, 2) * (s64)Vz, 
    shift, lm);
#undef M_AT
}

/* MVMVA with cv=2 (FC translation) is documented as buggy: only the
 * first column is added to T<<12, and IR is set from that partial dot
 * (without lm), then MAC/IR is overwritten by the remaining two
 * columns.  We faithfully reproduce that behavior. */
static void gte_mul_mat_vec_buggy(const s16* M, const s32 T[3], s16 Vx, s16 Vy, s16 Vz, u8 shift,
                                  bool lm)
{
#define M_AT(i, j) M[((i) * 3) + (j)]
  /* lane 1 */
  {
    const s64 partial = gte_sign_extend_mac1(
      gte_sign_extend_mac1(((s64)T[0] << 12) + (s64)M_AT(0, 0) * (s64)Vx));
    gte_truncate_set_ir1((s32)(partial >> shift), false);
    gte_truncate_set_mac_ir1(
      gte_sign_extend_mac1((s64)M_AT(0, 1) * (s64)Vy) + (s64)M_AT(0, 2) * (s64)Vz, shift, lm);
  }
  /* lane 2 */
  {
    const s64 partial = gte_sign_extend_mac2(
      gte_sign_extend_mac2(((s64)T[1] << 12) + (s64)M_AT(1, 0) * (s64)Vx));
    gte_truncate_set_ir2((s32)(partial >> shift), false);
    gte_truncate_set_mac_ir2(
      gte_sign_extend_mac2((s64)M_AT(1, 1) * (s64)Vy) + (s64)M_AT(1, 2) * (s64)Vz, shift, lm);
  }
  /* lane 3 */
  {
    const s64 partial = gte_sign_extend_mac3(
      gte_sign_extend_mac3(((s64)T[2] << 12) + (s64)M_AT(2, 0) * (s64)Vx));
    gte_truncate_set_ir3((s32)(partial >> shift), false);
    gte_truncate_set_mac_ir3(
      gte_sign_extend_mac3((s64)M_AT(2, 1) * (s64)Vy) + (s64)M_AT(2, 2) * (s64)Vz, shift, lm);
  }
#undef M_AT
}

static ALWAYS_INLINE u8 gte_inst_shift(u32 inst) { return ((inst >> 19) & 1u) ? 12u : 0u; }
static ALWAYS_INLINE bool gte_inst_lm(u32 inst) { return ((inst >> 10) & 1u) != 0u; }
static ALWAYS_INLINE u8 gte_inst_command(u32 inst) { return (u8)(inst & 0x3Fu); }
static ALWAYS_INLINE u8 gte_inst_mvmva_mat(u32 inst) { return (u8)((inst >> 17) & 0x3u); }
static ALWAYS_INLINE u8 gte_inst_mvmva_vec(u32 inst) { return (u8)((inst >> 15) & 0x3u); }
static ALWAYS_INLINE u8 gte_inst_mvmva_tvec(u32 inst) { return (u8)((inst >> 13) & 0x3u); }

/* IR = (FC<<12) - MAC, then MAC = (IR*IR0 + MAC), all clipped/shifted. */
static void gte_interpolate_color(s64 in_MAC1, s64 in_MAC2, s64 in_MAC3, u8 shift, bool lm)
{
  gte_truncate_set_mac_ir1(((s64)REGS.FC[0] << 12) - in_MAC1, shift, false);
  gte_truncate_set_mac_ir2(((s64)REGS.FC[1] << 12) - in_MAC2, shift, false);
  gte_truncate_set_mac_ir3(((s64)REGS.FC[2] << 12) - in_MAC3, shift, false);

  gte_truncate_set_mac_ir1((s64)((s32)REGS.IR1 * (s32)REGS.IR0) + in_MAC1, shift, lm);
  gte_truncate_set_mac_ir2((s64)((s32)REGS.IR2 * (s32)REGS.IR0) + in_MAC2, shift, lm);
  gte_truncate_set_mac_ir3((s64)((s32)REGS.IR3 * (s32)REGS.IR0) + in_MAC3, shift, lm);
}

/* Apply the rotation+translation to V[3], then perspective-divide via
 * UNR, push results to the SXY/SZ FIFOs.  When 'last' is true (RTPS, or
 * the 3rd vertex of RTPT) we additionally compute the depth-cued IR0. */
static void gte_rtps(const s16 V[3], u8 shift, bool lm, bool last)
{
#define DOT3(i)                                                                                    \
  (gte_sign_extend_mac##i(                                                                         \
     gte_sign_extend_mac##i(((s64)REGS.TR[(i) - 1] << 12)                                          \
                            + (s64)REGS.RT[(i) - 1][0] * (s64)V[0])                                \
     + (s64)REGS.RT[(i) - 1][1] * (s64)V[1])                                                       \
   + (s64)REGS.RT[(i) - 1][2] * (s64)V[2]) 

  const s64 x = DOT3(1);
  const s64 y = DOT3(2);
  const s64 z = DOT3(3);
#undef DOT3

  gte_truncate_set_mac1(x, shift);
  gte_truncate_set_mac2(y, shift);
  gte_truncate_set_mac3(z, shift);
  gte_truncate_set_ir1(REGS.MAC1, lm);
  gte_truncate_set_ir2(REGS.MAC2, lm);

  /* IR3 saturation flag is set off the raw MAC3 (pre-store-clamp) when
   * sf=0; the IR3 register itself holds the clamp of MAC3>>12. */
  gte_truncate_set_ir3((s32)(z >> 12), false);
  REGS.dr32[11] = (u32)gte_clamp_s32(REGS.MAC3, lm ? 0 : IR123_MIN_VALUE, IR123_MAX_VALUE);

  gte_push_sz((s32)(z >> 12));

  /* Perspective division then projection. */
  const s64 result = (s64)(u64)gte_unr_divide(REGS.H, REGS.SZ3);

  s64 Sx;
  switch (s_aspect_ratio) {
    case GTE_ASPECT_RATIO_R16_9:
      Sx = (((result * (s64)REGS.IR1) * (s64)3) / (s64)4) + (s64)REGS.OFX;
      break;
    case GTE_ASPECT_RATIO_R19_9:
      Sx = (((result * (s64)REGS.IR1) * (s64)12) / (s64)19) + (s64)REGS.OFX;
      break;
    case GTE_ASPECT_RATIO_R20_9:
      Sx = (((result * (s64)REGS.IR1) * (s64)3) / (s64)5) + (s64)REGS.OFX;
      break;
    case GTE_ASPECT_RATIO_CUSTOM:
      Sx = (((result * (s64)REGS.IR1) * (s64)s_custom_aspect_ratio_numerator)
            / (s64)s_custom_aspect_ratio_denominator)
           + (s64)REGS.OFX;
      break;
    case GTE_ASPECT_RATIO_NONE:
    case GTE_ASPECT_RATIO_COUNT:
    default:
      Sx = result * (s64)REGS.IR1 + (s64)REGS.OFX;
      break;
  }

  const s64 Sy = result * (s64)REGS.IR2 + (s64)REGS.OFY;
  gte_check_mac_overflow_0(Sx);
  gte_check_mac_overflow_0(Sy);
  gte_push_sxy((s32)(Sx >> 16), (s32)(Sy >> 16));

  if (g_settings.gpu_pgxp_enable) {
    /* PGXP: feed pre-truncation float coords (Sx/Sy as 16.16 fixed; MAC3
     * as a Q12 fixed-point depth) into the precision shadow.  packed_sxy
     * matches the SXY2 word the FIFO push just stored. */
    const u32 packed_sxy = REGS.dr32[14];
    pgxp_gte_rtps((float)Sx / 65536.0f,
                  (float)Sy / 65536.0f,
                  (float)REGS.MAC3 / 4096.0f, 
                  packed_sxy);
  }

  if (last) {
    /* MAC0 = result*DQA + DQB; IR0 = MAC0/0x1000  (depth cueing 0..0x1000). */
    const s64 Sz = result * (s64)REGS.DQA + (s64)REGS.DQB;
    gte_truncate_set_mac0(Sz, 0);
    gte_truncate_set_ir0((s32)(Sz >> 12), true);
  }
}

/* NCS: just lighting, no color modulation, no FC interpolation. */
static void gte_ncs(const s16 V[3], u8 shift, bool lm)
{
  gte_mul_mat_vec(&REGS.LLM[0][0], V[0], V[1], V[2], shift, lm);
  gte_mul_mat_vec_t(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);
  gte_push_rgb_from_mac();
}

/* NCCS: lighting + RGBC modulation. */
static void gte_nccs(const s16 V[3], u8 shift, bool lm)
{
  gte_mul_mat_vec(&REGS.LLM[0][0], V[0], V[1], V[2], shift, lm);
  gte_mul_mat_vec_t(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  gte_truncate_set_mac_ir1((s64)((s32)(u32)REGS.RGBC[0] * (s32)REGS.IR1) << 4, shift, lm);
  gte_truncate_set_mac_ir2((s64)((s32)(u32)REGS.RGBC[1] * (s32)REGS.IR2) << 4, shift, lm);
  gte_truncate_set_mac_ir3((s64)((s32)(u32)REGS.RGBC[2] * (s32)REGS.IR3) << 4, shift, lm);

  gte_push_rgb_from_mac();
}

/* NCDS: lighting + RGBC modulation + depth-cue interpolation against FC. */
static void gte_ncds(const s16 V[3], u8 shift, bool lm)
{
  gte_mul_mat_vec(&REGS.LLM[0][0], V[0], V[1], V[2], shift, lm);
  gte_mul_mat_vec_t(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  const s32 in_MAC1 = ((s32)(u32)REGS.RGBC[0] * (s32)REGS.IR1) << 4;
  const s32 in_MAC2 = ((s32)(u32)REGS.RGBC[1] * (s32)REGS.IR2) << 4;
  const s32 in_MAC3 = ((s32)(u32)REGS.RGBC[2] * (s32)REGS.IR3) << 4;

  gte_interpolate_color(in_MAC1, in_MAC2, in_MAC3, shift, lm);
  gte_push_rgb_from_mac();
}

 /* DPCS body: feed a packed RGB triplet (input register's R/G/B bytes)
 * through the FC interpolation path. */
static void gte_dpcs(const u8 color[3], u8 shift, bool lm)
{
  gte_truncate_set_mac1(((s64)(u64)color[0] << 16), 0);
  gte_truncate_set_mac2(((s64)(u64)color[1] << 16), 0);
  gte_truncate_set_mac3(((s64)(u64)color[2] << 16), 0);

  gte_interpolate_color(REGS.MAC1, REGS.MAC2, REGS.MAC3, shift, lm);
  gte_push_rgb_from_mac();
}

static void gte_execute_mvmva(u32 inst)
{
  gte_flag_clear();

  const u8 mat = gte_inst_mvmva_mat(inst);
  const u8 vec = gte_inst_mvmva_vec(inst);
  const u8 tvec = gte_inst_mvmva_tvec(inst);

  /* Matrix selector: 0=RT, 1=LLM, 2=LCM, 3="buggy" (synthesized). */
  const s16* M = NULL;
  switch (mat) {
    case 0: M = &REGS.RT[0][0]; break;
    case 1: M = &REGS.LLM[0][0]; break;
    case 2: M = &REGS.LCM[0][0]; break;
    case 3: M = NULL; break;
    default: break;
  }

  /* Vector selector: 0=V0, 1=V1, 2=V2, 3=IR. */
  s16 Vx, Vy, Vz;
  switch (vec) {
    case 0: Vx = REGS.V0[0]; Vy = REGS.V0[1]; Vz = REGS.V0[2]; break;
    case 1: Vx = REGS.V1[0]; Vy = REGS.V1[1]; Vz = REGS.V1[2]; break;
    case 2: Vx = REGS.V2[0]; Vy = REGS.V2[1]; Vz = REGS.V2[2]; break;
    case 3:
    default:
      Vx = REGS.IR1; Vy = REGS.IR2; Vz = REGS.IR3; break;
  }

  /* Translation selector: 0=TR, 1=BK, 2=FC (buggy), 3=zero. */
  static const s32 zero_T[3] = {0, 0, 0};
  const s32* T = zero_T;
  switch (tvec) {
    case 0: T = REGS.TR; break;
    case 1: T = REGS.BK; break;
    case 2: T = REGS.FC; break;
    case 3:
    default: T = zero_T; break;
  }

  s16 buggy_M[9];
  if (M == NULL) {
    buggy_M[0 * 3 + 0] = (s16)-(s32)((u32)REGS.RGBC[0] << 4);
    buggy_M[0 * 3 + 1] = (s16)((u32)REGS.RGBC[0] << 4);
    buggy_M[0 * 3 + 2] = REGS.IR0;
    buggy_M[1 * 3 + 0] = REGS.RT[0][2];
    buggy_M[1 * 3 + 1] = REGS.RT[0][2];
    buggy_M[1 * 3 + 2] = REGS.RT[0][2];
    buggy_M[2 * 3 + 0] = REGS.RT[1][1];
    buggy_M[2 * 3 + 1] = REGS.RT[1][1];
    buggy_M[2 * 3 + 2] = REGS.RT[1][1];
    M = buggy_M;
  }

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);
  if (tvec != 2)
    gte_mul_mat_vec_t(M, T, Vx, Vy, Vz, shift, lm);
  else
    gte_mul_mat_vec_buggy(M, T, Vx, Vy, Vz, shift, lm);

  gte_flag_update_error();
}

static void gte_execute_sqr(u32 inst)
{
  gte_flag_clear();

  /* IR1..IR3 are 16-bit signed; squaring them fits in 32 bits, so we
   * can use 32-bit multiply for speed. */
  const u8 shift = gte_inst_shift(inst);
  REGS.MAC1 = ((s32)REGS.IR1 * (s32)REGS.IR1) >> shift;
  REGS.MAC2 = ((s32)REGS.IR2 * (s32)REGS.IR2) >> shift;
  REGS.MAC3 = ((s32)REGS.IR3 * (s32)REGS.IR3) >> shift;

  const bool lm = gte_inst_lm(inst);
  gte_truncate_set_ir1(REGS.MAC1, lm);
  gte_truncate_set_ir2(REGS.MAC2, lm);
  gte_truncate_set_ir3(REGS.MAC3, lm);

  gte_flag_update_error();
}

static void gte_execute_op(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);
  const s32 D1 = (s32)REGS.RT[0][0];
  const s32 D2 = (s32)REGS.RT[1][1];
  const s32 D3 = (s32)REGS.RT[2][2];
  const s32 IR1 = (s32)REGS.IR1;
  const s32 IR2 = (s32)REGS.IR2;
  const s32 IR3 = (s32)REGS.IR3;

  gte_truncate_set_mac_ir1((s64)(IR3 * D2) - (s64)(IR2 * D3), shift, lm);
  gte_truncate_set_mac_ir2((s64)(IR1 * D3) - (s64)(IR3 * D1), shift, lm);
  gte_truncate_set_mac_ir3((s64)(IR2 * D1) - (s64)(IR1 * D2), shift, lm);

  gte_flag_update_error();
}

static void gte_execute_rtps(u32 inst)
{
  gte_flag_clear();
  gte_rtps(REGS.V0, gte_inst_shift(inst), gte_inst_lm(inst), true);
  gte_flag_update_error();
}

static void gte_execute_rtpt(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_rtps(REGS.V0, shift, lm, false);
  gte_rtps(REGS.V1, shift, lm, false);
  gte_rtps(REGS.V2, shift, lm, true);

  gte_flag_update_error();
}

static void gte_execute_nclip(u32 inst)
{
  /* MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1 */
  (void)inst;
  gte_flag_clear();

  if (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling) {
    const u32 sxy0 = REGS.dr32[12];
    const u32 sxy1 = REGS.dr32[13];
    const u32 sxy2 = REGS.dr32[14];
    if (pgxp_gte_has_precise_vertices(sxy0, sxy1, sxy2)) {
      REGS.dr32[24] = (u32)(s32)pgxp_gte_nclip();
      return;
    }
  }

  gte_truncate_set_mac0(
    (s64)REGS.SXY0[0] * (s64)REGS.SXY1[1] + (s64)REGS.SXY1[0] * (s64)REGS.SXY2[1]
      + (s64)REGS.SXY2[0] * (s64)REGS.SXY0[1] - (s64)REGS.SXY0[0] * (s64)REGS.SXY2[1]
      - (s64)REGS.SXY1[0] * (s64)REGS.SXY0[1] - (s64)REGS.SXY2[0] * (s64)REGS.SXY1[1], 
    0);

  gte_flag_update_error();
}

static void gte_execute_avsz3(u32 inst)
{
  (void)inst;
  gte_flag_clear();

  const s64 result = (s64)REGS.ZSF3 * (s32)((u32)REGS.SZ1 + (u32)REGS.SZ2 + (u32)REGS.SZ3);
  gte_truncate_set_mac0(result, 0);
  gte_set_otz((s32)(result >> 12));

  gte_flag_update_error();
}

static void gte_execute_avsz4(u32 inst)
{
  (void)inst;
  gte_flag_clear();

  const s64 result = (s64)REGS.ZSF4
                    
                     * (s32)((u32)REGS.SZ0 + (u32)REGS.SZ1 + (u32)REGS.SZ2 + (u32)REGS.SZ3);
                     
  gte_truncate_set_mac0(result, 0);
  gte_set_otz((s32)(result >> 12));

  gte_flag_update_error();
}

static void gte_execute_ncs(u32 inst)
{
  gte_flag_clear();
  gte_ncs(REGS.V0, gte_inst_shift(inst), gte_inst_lm(inst));
  gte_flag_update_error();
}

static void gte_execute_nct(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_ncs(REGS.V0, shift, lm);
  gte_ncs(REGS.V1, shift, lm);
  gte_ncs(REGS.V2, shift, lm);

  gte_flag_update_error();
}

static void gte_execute_nccs(u32 inst)
{
  gte_flag_clear();
  gte_nccs(REGS.V0, gte_inst_shift(inst), gte_inst_lm(inst));
  gte_flag_update_error();
}

static void gte_execute_ncct(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_nccs(REGS.V0, shift, lm);
  gte_nccs(REGS.V1, shift, lm);
  gte_nccs(REGS.V2, shift, lm);

  gte_flag_update_error();
}

static void gte_execute_ncds(u32 inst)
{
  gte_flag_clear();
  gte_ncds(REGS.V0, gte_inst_shift(inst), gte_inst_lm(inst));
  gte_flag_update_error();
}

static void gte_execute_ncdt(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_ncds(REGS.V0, shift, lm);
  gte_ncds(REGS.V1, shift, lm);
  gte_ncds(REGS.V2, shift, lm);

  gte_flag_update_error();
}

static void gte_execute_cc(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_mul_mat_vec_t(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  gte_truncate_set_mac_ir1((s64)((s32)(u32)REGS.RGBC[0] * (s32)REGS.IR1) << 4, shift, lm);
  gte_truncate_set_mac_ir2((s64)((s32)(u32)REGS.RGBC[1] * (s32)REGS.IR2) << 4, shift, lm);
  gte_truncate_set_mac_ir3((s64)((s32)(u32)REGS.RGBC[2] * (s32)REGS.IR3) << 4, shift, lm);

  gte_push_rgb_from_mac();

  gte_flag_update_error();
}

static void gte_execute_cdp(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_mul_mat_vec_t(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  const s32 in_MAC1 = ((s32)(u32)REGS.RGBC[0] * (s32)REGS.IR1) << 4;
  const s32 in_MAC2 = ((s32)(u32)REGS.RGBC[1] * (s32)REGS.IR2) << 4;
  const s32 in_MAC3 = ((s32)(u32)REGS.RGBC[2] * (s32)REGS.IR3) << 4;

  gte_interpolate_color(in_MAC1, in_MAC2, in_MAC3, shift, lm);
  gte_push_rgb_from_mac();

  gte_flag_update_error();
}

static void gte_execute_dpcs(u32 inst)
{
  gte_flag_clear();
  gte_dpcs(REGS.RGBC, gte_inst_shift(inst), gte_inst_lm(inst));
  gte_flag_update_error();
}

static void gte_execute_dpct(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  for (u32 i = 0; i < 3u; i++)
    gte_dpcs(REGS.RGB0, shift, lm);

  gte_flag_update_error();
}

static void gte_execute_dcpl(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  const s32 in_MAC1 = ((s32)(u32)REGS.RGBC[0] * (s32)REGS.IR1) << 4;
  const s32 in_MAC2 = ((s32)(u32)REGS.RGBC[1] * (s32)REGS.IR2) << 4;
  const s32 in_MAC3 = ((s32)(u32)REGS.RGBC[2] * (s32)REGS.IR3) << 4;

  gte_interpolate_color(in_MAC1, in_MAC2, in_MAC3, shift, lm);
  gte_push_rgb_from_mac();

  gte_flag_update_error();
}

static void gte_execute_intpl(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  /* MAC := IR << 12, then FC interpolation. */
  gte_interpolate_color((s64)((s32)REGS.IR1 << 12), (s64)((s32)REGS.IR2 << 12),
                        (s64)((s32)REGS.IR3 << 12), shift, lm);
  gte_push_rgb_from_mac();

  gte_flag_update_error();
}

static void gte_execute_gpl(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  /* GPL preserves the prior MAC accumulator (shifted up by sf*12 first). */
  gte_truncate_set_mac_ir1(
    (s64)((s32)REGS.IR1 * (s32)REGS.IR0) + ((s64)REGS.MAC1 << shift), shift, lm);
  gte_truncate_set_mac_ir2(
    (s64)((s32)REGS.IR2 * (s32)REGS.IR0) + ((s64)REGS.MAC2 << shift), shift, lm);
  gte_truncate_set_mac_ir3(
    (s64)((s32)REGS.IR3 * (s32)REGS.IR0) + ((s64)REGS.MAC3 << shift), shift, lm);

  gte_push_rgb_from_mac();

  gte_flag_update_error();
}

static void gte_execute_gpf(u32 inst)
{
  gte_flag_clear();

  const u8 shift = gte_inst_shift(inst);
  const bool lm = gte_inst_lm(inst);

  gte_truncate_set_mac_ir1((s64)((s32)REGS.IR1 * (s32)REGS.IR0), shift, lm);
  gte_truncate_set_mac_ir2((s64)((s32)REGS.IR2 * (s32)REGS.IR0), shift, lm);
  gte_truncate_set_mac_ir3((s64)((s32)REGS.IR3 * (s32)REGS.IR0), shift, lm);

  gte_push_rgb_from_mac();

  gte_flag_update_error();
}

void gte_execute(u32 inst_bits)
{
  const u8 command = gte_inst_command(inst_bits);
  s_dbg_gte_op_counts[command]++;

  switch (command) {
    case 0x01: gte_execute_rtps(inst_bits); break;
    case 0x06: gte_execute_nclip(inst_bits); break;
    case 0x0C: gte_execute_op(inst_bits); break;
    case 0x10: gte_execute_dpcs(inst_bits); break;
    case 0x11: gte_execute_intpl(inst_bits); break;
    case 0x12: gte_execute_mvmva(inst_bits); break;
    case 0x13: gte_execute_ncds(inst_bits); break;
    case 0x14: gte_execute_cdp(inst_bits); break;
    case 0x16: gte_execute_ncdt(inst_bits); break;
    case 0x1B: gte_execute_nccs(inst_bits); break;
    case 0x1C: gte_execute_cc(inst_bits); break;
    case 0x1E: gte_execute_ncs(inst_bits); break;
    case 0x20: gte_execute_nct(inst_bits); break;
    case 0x28: gte_execute_sqr(inst_bits); break;
    case 0x29: gte_execute_dcpl(inst_bits); break;
    case 0x2A: gte_execute_dpct(inst_bits); break;
    case 0x2D: gte_execute_avsz3(inst_bits); break;
    case 0x2E: gte_execute_avsz4(inst_bits); break;
    case 0x30: gte_execute_rtpt(inst_bits); break;
    case 0x3D: gte_execute_gpf(inst_bits); break;
    case 0x3E: gte_execute_gpl(inst_bits); break;
    case 0x3F: gte_execute_ncct(inst_bits); break;
    default:
      Panic("Unhandled GTE instruction");
      break;
  }
}

void gte_execute_instruction(cpu_instruction_t inst)
{
  gte_execute(inst.bits);
}

const u32* gte_dbg_op_counts(void)
{
  return s_dbg_gte_op_counts;
}

s32 gte_get_instruction_cycles(u32 inst_bits)
{
  switch (gte_inst_command(inst_bits)) {
    case 0x01: return 15;
    case 0x06: return 8;
    case 0x0C: return 6;
    case 0x10: return 8;
    case 0x11: return 7;
    case 0x12: return 8;
    case 0x13: return 19;
    case 0x14: return 13;
    case 0x16: return 44;
    case 0x1B: return 17;
    case 0x1C: return 11;
    case 0x1E: return 14;
    case 0x20: return 30;
    case 0x28: return 5;
    case 0x29: return 8;
    case 0x2A: return 17;
    case 0x2D: return 5;
    case 0x2E: return 6;
    case 0x30: return 23;
    case 0x3D: return 5;
    case 0x3E: return 5;
    case 0x3F: return 39;
    default: return 1;
  }
}

u32 gte_read_register(u32 index)
{
  DebugAssert(index < GTE_NUM_REGS);

  switch (index) {
    case 15:
      return REGS.r32[14];

    case 28: /* IRGB */
    case 29:
    {
      const s32 ir1 = REGS.IR1 / 0x80;
      const s32 ir2 = REGS.IR2 / 0x80;
      const s32 ir3 = REGS.IR3 / 0x80;
      const u8 r = (u8)gte_clamp_s32(ir1, 0x00, 0x1F);
      const u8 g = (u8)gte_clamp_s32(ir2, 0x00, 0x1F);
      const u8 b = (u8)gte_clamp_s32(ir3, 0x00, 0x1F);
      return (u32)r | ((u32)g << 5) | ((u32)b << 10);
    }

    default:
      return REGS.r32[index];
  }
}

void gte_write_register(u32 index, u32 value)
{
  switch (index) {
    case 1:  /* V0[z] */
    case 3:  /* V1[z] */
    case 5:  /* V2[z] */
    case 8:  /* IR0 */
    case 9:  /* IR1 */
    case 10: /* IR2 */
    case 11: /* IR3 */
    case 36: /* RT33 */
    case 44: /* L33  */
    case 52: /* LR33 */
    case 58:
    case 59: /* DQA */
    case 61: /* ZSF3 */
    case 62: /* ZSF4 */
      /* Sign-extend the low half into the full 32-bit slot. */
      REGS.r32[index] = (u32)(s32)(s16)(u16)value;
      break;

    case 7:  /* OTZ */
    case 16: /* SZ0 */
    case 17: /* SZ1 */
    case 18: /* SZ2 */
    case 19: /* SZ3 */
      REGS.r32[index] = (u32)(u16)value;
      break;

    case 15:
      REGS.r32[12] = REGS.r32[13]; /* SXY0 <- SXY1 */
      REGS.r32[13] = REGS.r32[14]; /* SXY1 <- SXY2 */
      REGS.r32[14] = value;        /* SXY2 <- SXYP */
      break;

    case 28:
      REGS.IRGB = value & UINT32_C(0x7FFF);
      REGS.r32[9] = (u32)(s32)(s16)(u16)((value & UINT32_C(0x1F)) * UINT32_C(0x80));
      REGS.r32[10] = (u32)(s32)(s16)(u16)(((value >> 5) & UINT32_C(0x1F)) * UINT32_C(0x80));
      REGS.r32[11] = (u32)(s32)(s16)(u16)(((value >> 10) & UINT32_C(0x1F)) * UINT32_C(0x80));
      break;

    case 30:
      REGS.LZCS = (s32)value;
      REGS.LZCR = gte_count_leading_bits(value);
      break;

    case 29:
    case 31:
      break;

    case 63: /* FLAG */
      REGS.FLAG.bits = value & UINT32_C(0x7FFFF000);
      gte_flag_update_error();
      break;

    default:
      REGS.r32[index] = value;
      break;
  }
}

u32* gte_get_register_ptr(u32 index)
{
  DebugAssert(index < GTE_NUM_REGS);
  return &REGS.r32[index];
}

void gte_initialize(void)
{
  memset(&REGS, 0, sizeof(REGS));
  s_aspect_ratio = GTE_ASPECT_RATIO_NONE;
  s_custom_aspect_ratio_numerator = 0u;
  s_custom_aspect_ratio_denominator = 0u;
}

void gte_reset(void)
{
  memset(&REGS, 0, sizeof(REGS));
}

bool gte_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_array(sw, REGS.r32, sizeof(u32), GTE_NUM_REGS);
  return true;
}

void gte_set_aspect_ratio(gte_aspect_ratio_t aspect, u32 custom_numerator,
                          u32 custom_denominator)
{
  s_aspect_ratio = aspect;
  s_custom_aspect_ratio_numerator = custom_numerator;
  s_custom_aspect_ratio_denominator = custom_denominator;
}
