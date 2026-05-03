/*
 * GSVector. C port of duckstation common/gsvector.h + gsvector_sse.h.
 *
 * Surface limited to what the SW-rasterizer SIMD body needs:
 *   - GSVector4i: 128-bit integer (4xs32 / 8xs16 / 16xs8). SSE4.1 baseline.
 *   - GSVector8i: 256-bit integer (8xs32 / 16xs16). AVX2-gated.
 *
 * Skipped vs upstream:
 *   - Float vectors (GSVector4 / GSVector2 / GSVector2i). Rasterizer SIMD
 *     path is integer-only; will port when first caller appears.
 *   - GSMatrix2x2 / GSMatrix4x4. Unused by SW renderer.
 *   - NEON path. Linux x64 build only.
 *   - nosimd fallback. SSE2 is implicit on every x64 target we ship.
 *
 * Conventions:
 *   - Types are bare intrinsic typedefs (no wrapper struct). C lets us name
 *     ops directly so we skip the C++ operator-overloading dance.
 *   - Value ops (add/sub/mul/min/max/cmp/blend on full vectors) are
 *     `static inline` functions.
 *   - Template-param ops (immediate-encoded shifts, blends, extracts,
 *     inserts) are macros. the underlying intrinsics require compile-time
 *     constants and `static inline` with `int n` lets non-literal callers
 *     compile but then fault inside the intrinsic. Macros keep the constness
 *     contract checkable at the call site.
 *   - andnot follows duckstation convention: `andnot(this, mask) = ~this & mask`
 *     (NOT the `_mm_andnot_si128(a,b) = ~a & b` order. duckstation flips it
 *     so `x.andnot(mask)` reads as "x but only where mask is set").
 *     C wrapper keeps the duckstation order: `gsv4i_andnot(this, mask)`.
 *
 * Naming: `gsv4i_*` (128-bit int), `gsv8i_*` (256-bit int).
 */

#ifndef CUPID_COMMON_GSVECTOR_H
#define CUPID_COMMON_GSVECTOR_H

#include "common/intrin.h"
#include "common/types.h"

#if !defined(CPU_ARCH_SSE)
#  error "gsvector.h requires CPU_ARCH_SSE (x86/x86_64 build)"
#endif

/* SSE4.1 ops are used unconditionally; the build either has -msse4.1 set
 * for the TU or compiles with native -march that includes it. The Makefile
 * gates the SIMD rasterizer TU explicitly. */
#if !defined(__SSE4_1__) && !defined(CPU_ARCH_SSE41)
#  warning "gsvector.h: -msse4.1 not set; some ops will fail to compile"
#endif

typedef __m128i GSVector4i;

#ifdef __AVX2__
typedef __m256i GSVector8i;
#  define GSVECTOR_HAS_256 1
#  define GSVECTOR_HAS_SRLV 1
#endif

/* ============================================================
 * GSVector4i: 128-bit integer (4×s32, 8×s16, 16×s8)
 * ============================================================ */

/* --- Constructors / constants ---------------------------------------- */

ALWAYS_INLINE GSVector4i gsv4i_zero(void)         { return _mm_setzero_si128(); }
ALWAYS_INLINE GSVector4i gsv4i_set32(s32 a)       { return _mm_set1_epi32(a); }
ALWAYS_INLINE GSVector4i gsv4i_set16(s16 a)       { return _mm_set1_epi16(a); }
ALWAYS_INLINE GSVector4i gsv4i_set8(s8 a)         { return _mm_set1_epi8(a); }

/* setr matches duckstation cxpr ordering (lane 0 = first arg). */
ALWAYS_INLINE GSVector4i gsv4i_setr32(s32 a, s32 b, s32 c, s32 d)
{ return _mm_setr_epi32(a, b, c, d); }
ALWAYS_INLINE GSVector4i gsv4i_setr16(s16 s0, s16 s1, s16 s2, s16 s3,
                                      s16 s4, s16 s5, s16 s6, s16 s7)
{ return _mm_setr_epi16(s0, s1, s2, s3, s4, s5, s6, s7); }

/* Single-scalar zero-extend to lane 0 (matches GSVector4i::zext32). */
ALWAYS_INLINE GSVector4i gsv4i_zext32(u32 a)     { return _mm_cvtsi32_si128((s32)a); }

/* --- Memory ---------------------------------------------------------- */

#define gsv4i_load_a(p)        _mm_load_si128((const __m128i*)(p))
#define gsv4i_load_u(p)        _mm_loadu_si128((const __m128i*)(p))
#define gsv4i_loadl_a(p)       _mm_loadl_epi64((const __m128i*)(p))
#define gsv4i_loadl_u(p)       _mm_loadl_epi64((const __m128i*)(p))
#define gsv4i_store_a(p, v)    _mm_store_si128((__m128i*)(p), (v))
#define gsv4i_store_u(p, v)    _mm_storeu_si128((__m128i*)(p), (v))
#define gsv4i_storel_a(p, v)   _mm_storel_epi64((__m128i*)(p), (v))
#define gsv4i_storel_u(p, v)   _mm_storel_epi64((__m128i*)(p), (v))

/* --- Logical --------------------------------------------------------- */

ALWAYS_INLINE GSVector4i gsv4i_and(GSVector4i a, GSVector4i b)  { return _mm_and_si128(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_or (GSVector4i a, GSVector4i b)  { return _mm_or_si128 (a, b); }
ALWAYS_INLINE GSVector4i gsv4i_xor(GSVector4i a, GSVector4i b)  { return _mm_xor_si128(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_not(GSVector4i a)
{ return _mm_xor_si128(a, _mm_cmpeq_epi32(_mm_setzero_si128(), _mm_setzero_si128())); }
/* duckstation convention: this.andnot(mask) = ~this & mask */
ALWAYS_INLINE GSVector4i gsv4i_andnot(GSVector4i v, GSVector4i mask)
{ return _mm_andnot_si128(v, mask); }

/* --- Add / Sub ------------------------------------------------------- */

ALWAYS_INLINE GSVector4i gsv4i_add32(GSVector4i a, GSVector4i b) { return _mm_add_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_sub32(GSVector4i a, GSVector4i b) { return _mm_sub_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_add16(GSVector4i a, GSVector4i b) { return _mm_add_epi16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_sub16(GSVector4i a, GSVector4i b) { return _mm_sub_epi16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_add8 (GSVector4i a, GSVector4i b) { return _mm_add_epi8 (a, b); }
ALWAYS_INLINE GSVector4i gsv4i_sub8 (GSVector4i a, GSVector4i b) { return _mm_sub_epi8 (a, b); }

/* --- Mul ------------------------------------------------------------- */

/* Packed s16 × s16 → low 16 of result (matches duckstation .mul16l). */
ALWAYS_INLINE GSVector4i gsv4i_mul16l(GSVector4i a, GSVector4i b)
{ return _mm_mullo_epi16(a, b); }
/* Packed s32 × s32 → low 32 of result (SSE4.1 _mm_mullo_epi32). */
ALWAYS_INLINE GSVector4i gsv4i_mul32l(GSVector4i a, GSVector4i b)
{ return _mm_mullo_epi32(a, b); }

/* --- Shifts (immediate count -> macros) ------------------------------ */

#define gsv4i_sll32(v, n)  _mm_slli_epi32((v), (n))
#define gsv4i_srl32(v, n)  _mm_srli_epi32((v), (n))
#define gsv4i_sra32(v, n)  _mm_srai_epi32((v), (n))
#define gsv4i_sll16(v, n)  _mm_slli_epi16((v), (n))
#define gsv4i_srl16(v, n)  _mm_srli_epi16((v), (n))
#define gsv4i_sra16(v, n)  _mm_srai_epi16((v), (n))
#define gsv4i_sll64(v, n)  _mm_slli_epi64((v), (n))
#define gsv4i_srl64(v, n)  _mm_srli_epi64((v), (n))

/* Variable-shift per-lane (AVX2). For SSE4.1-only path, fall back to
 * scalar extract+shift+insert. The slow path matches duckstation's
 * non-HAS_SRLV branch. used only by GatherCLUTVector. */
#ifdef __AVX2__
#  define gsv4i_srlv32(v, sh)  _mm_srlv_epi32((v), (sh))
#  define gsv4i_sllv32(v, sh)  _mm_sllv_epi32((v), (sh))
#endif

/* --- Compare --------------------------------------------------------- */

/* Equality / sign. Result lanes are all-1s on match else 0. */
ALWAYS_INLINE GSVector4i gsv4i_eq32(GSVector4i a, GSVector4i b) { return _mm_cmpeq_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_eq16(GSVector4i a, GSVector4i b) { return _mm_cmpeq_epi16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_eq8 (GSVector4i a, GSVector4i b) { return _mm_cmpeq_epi8 (a, b); }
ALWAYS_INLINE GSVector4i gsv4i_lt32(GSVector4i a, GSVector4i b) { return _mm_cmplt_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_gt32(GSVector4i a, GSVector4i b) { return _mm_cmpgt_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_lt16(GSVector4i a, GSVector4i b) { return _mm_cmplt_epi16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_gt16(GSVector4i a, GSVector4i b) { return _mm_cmpgt_epi16(a, b); }

/* --- Min / Max (signed/unsigned) ------------------------------------- */

ALWAYS_INLINE GSVector4i gsv4i_max_s32(GSVector4i a, GSVector4i b) { return _mm_max_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_min_s32(GSVector4i a, GSVector4i b) { return _mm_min_epi32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_max_u32(GSVector4i a, GSVector4i b) { return _mm_max_epu32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_min_u32(GSVector4i a, GSVector4i b) { return _mm_min_epu32(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_max_s16(GSVector4i a, GSVector4i b) { return _mm_max_epi16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_min_s16(GSVector4i a, GSVector4i b) { return _mm_min_epi16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_max_u16(GSVector4i a, GSVector4i b) { return _mm_max_epu16(a, b); }
ALWAYS_INLINE GSVector4i gsv4i_min_u16(GSVector4i a, GSVector4i b) { return _mm_min_epu16(a, b); }

/* --- Test -----------------------------------------------------------
 * alltrue: every bit set across the 128-bit vector.
 * allfalse: every bit clear.
 * Use SSE4.1 _mm_test_all_* when available; otherwise compare to const.
 */

ALWAYS_INLINE bool gsv4i_alltrue(GSVector4i v)
{
#ifdef __SSE4_1__
  return (bool)_mm_test_all_ones(v);
#else
  return _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(-1))) == 0xFFFF;
#endif
}
ALWAYS_INLINE bool gsv4i_allfalse(GSVector4i v)
{
#ifdef __SSE4_1__
  return (bool)_mm_test_all_zeros(v, v);
#else
  return _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_setzero_si128())) == 0xFFFF;
#endif
}

/* --- Extract / Insert (immediate index -> macros) -------------------- */

#define gsv4i_extract32(v, i)        _mm_extract_epi32((v), (i))
#define gsv4i_extract16(v, i)        ((u16)(unsigned)_mm_extract_epi16((v), (i)))
#define gsv4i_extract8 (v, i)        ((u8) (unsigned)_mm_extract_epi8 ((v), (i)))
#define gsv4i_insert32(v, val, i)    _mm_insert_epi32((v), (val), (i))
#define gsv4i_insert16(v, val, i)    _mm_insert_epi16((v), (val), (i))
#define gsv4i_insert8 (v, val, i)    _mm_insert_epi8 ((v), (val), (i))

/* --- Conversion / packing ------------------------------------------- */

/* Unpack low half: u8 → u16 (0..7 of input → all 8 lanes as u16). */
ALWAYS_INLINE GSVector4i gsv4i_u8to16(GSVector4i v)
{ return _mm_cvtepu8_epi16(v); }
/* Unpack low half: u16 → u32. */
ALWAYS_INLINE GSVector4i gsv4i_u16to32(GSVector4i v)
{ return _mm_cvtepu16_epi32(v); }
/* Sign-extend variants. */
ALWAYS_INLINE GSVector4i gsv4i_s8to16(GSVector4i v)  { return _mm_cvtepi8_epi16 (v); }
ALWAYS_INLINE GSVector4i gsv4i_s16to32(GSVector4i v) { return _mm_cvtepi16_epi32(v); }

/* Pack 4×s32 → 4×u16 with unsigned saturation. Upper 4 u16 lanes = 0
 * since input has 4 lanes only (matches duckstation .pu32() one-arg form). */
ALWAYS_INLINE GSVector4i gsv4i_pu32(GSVector4i v)
{ return _mm_packus_epi32(v, _mm_setzero_si128()); }
/* Two-arg form: pack a (low 4 u16) | b (high 4 u16). Matches .pu32(b). */
ALWAYS_INLINE GSVector4i gsv4i_pu32_2(GSVector4i a, GSVector4i b)
{ return _mm_packus_epi32(a, b); }

/* --- Shuffles -------------------------------------------------------- */

/* xxxxl: duplicate lane 0 of low 4 u16s into all 4 lanes (matches
 * duckstation GSVector4i::xxxxl()). Implemented via _mm_shufflelo_epi16. */
ALWAYS_INLINE GSVector4i gsv4i_xxxxl(GSVector4i v)
{ return _mm_shufflelo_epi16(v, _MM_SHUFFLE(0, 0, 0, 0)); }
/* yyyyl: same with lane 1. */
ALWAYS_INLINE GSVector4i gsv4i_yyyyl(GSVector4i v)
{ return _mm_shufflelo_epi16(v, _MM_SHUFFLE(1, 1, 1, 1)); }

/* General 32-bit shuffle. mask = _MM_SHUFFLE(d, c, b, a). */
#define gsv4i_shuffle32(v, mask)  _mm_shuffle_epi32((v), (mask))

/* --- Blend ----------------------------------------------------------- */

/* Variable byte blend (SSE4.1). When a mask byte's high bit is set,
 * take the corresponding byte from b, else from a. */
ALWAYS_INLINE GSVector4i gsv4i_blend8(GSVector4i a, GSVector4i b, GSVector4i mask)
{ return _mm_blendv_epi8(a, b, mask); }

/* Immediate 16-bit blend (SSE4.1). imm bit i selects: 0=a, 1=b for lane i. */
#define gsv4i_blend16(a, b, imm)  _mm_blend_epi16((a), (b), (imm))
/* Immediate 32-bit blend (SSE4.1). imm = 4-bit mask, bit i selects lane i. */
#define gsv4i_blend32(a, b, imm)  _mm_blend_epi32((a), (b), (imm))

/* runion: rect union (lanes 0,1 = min; lanes 2,3 = max). Used by
 * upstream's drawing-area helpers. */
ALWAYS_INLINE GSVector4i gsv4i_runion(GSVector4i a, GSVector4i b)
{
  /* lanes 0,1 = min(a,b); lanes 2,3 = max(a,b). blend the two on bits 2,3. */
  return _mm_blend_epi32(_mm_min_epi32(a, b), _mm_max_epi32(a, b), 0xC);
}

/* ============================================================
 * GSVector8i: 256-bit integer (8×s32, 16×s16). AVX2 only
 * ============================================================ */

#ifdef __AVX2__

ALWAYS_INLINE GSVector8i gsv8i_zero(void)         { return _mm256_setzero_si256(); }
ALWAYS_INLINE GSVector8i gsv8i_set32(s32 a)       { return _mm256_set1_epi32(a); }
ALWAYS_INLINE GSVector8i gsv8i_set16(s16 a)       { return _mm256_set1_epi16(a); }
ALWAYS_INLINE GSVector8i gsv8i_setr32(s32 a, s32 b, s32 c, s32 d,
                                      s32 e, s32 f, s32 g, s32 h)
{ return _mm256_setr_epi32(a, b, c, d, e, f, g, h); }
ALWAYS_INLINE GSVector8i gsv8i_setr16(s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7,
                                      s16 s8, s16 s9, s16 s10, s16 s11, s16 s12, s16 s13, s16 s14, s16 s15)
{ return _mm256_setr_epi16(s0, s1, s2, s3, s4, s5, s6, s7,
                            s8, s9, s10, s11, s12, s13, s14, s15); }
ALWAYS_INLINE GSVector8i gsv8i_zext32(u32 a)
{ return _mm256_castsi128_si256(_mm_cvtsi32_si128((s32)a)); }

#define gsv8i_load_a(p)      _mm256_load_si256((const __m256i*)(p))
#define gsv8i_load_u(p)      _mm256_loadu_si256((const __m256i*)(p))
#define gsv8i_store_a(p, v)  _mm256_store_si256((__m256i*)(p), (v))
#define gsv8i_store_u(p, v)  _mm256_storeu_si256((__m256i*)(p), (v))

ALWAYS_INLINE GSVector8i gsv8i_and(GSVector8i a, GSVector8i b) { return _mm256_and_si256(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_or (GSVector8i a, GSVector8i b) { return _mm256_or_si256 (a, b); }
ALWAYS_INLINE GSVector8i gsv8i_xor(GSVector8i a, GSVector8i b) { return _mm256_xor_si256(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_andnot(GSVector8i v, GSVector8i mask)
{ return _mm256_andnot_si256(v, mask); }

ALWAYS_INLINE GSVector8i gsv8i_add32(GSVector8i a, GSVector8i b) { return _mm256_add_epi32(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_sub32(GSVector8i a, GSVector8i b) { return _mm256_sub_epi32(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_add16(GSVector8i a, GSVector8i b) { return _mm256_add_epi16(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_sub16(GSVector8i a, GSVector8i b) { return _mm256_sub_epi16(a, b); }

ALWAYS_INLINE GSVector8i gsv8i_mul16l(GSVector8i a, GSVector8i b) { return _mm256_mullo_epi16(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_mul32l(GSVector8i a, GSVector8i b) { return _mm256_mullo_epi32(a, b); }

#define gsv8i_sll32(v, n)   _mm256_slli_epi32((v), (n))
#define gsv8i_srl32(v, n)   _mm256_srli_epi32((v), (n))
#define gsv8i_sra32(v, n)   _mm256_srai_epi32((v), (n))
#define gsv8i_sll16(v, n)   _mm256_slli_epi16((v), (n))
#define gsv8i_srl16(v, n)   _mm256_srli_epi16((v), (n))
#define gsv8i_sra16(v, n)   _mm256_srai_epi16((v), (n))
#define gsv8i_srlv32(v, sh) _mm256_srlv_epi32((v), (sh))
#define gsv8i_sllv32(v, sh) _mm256_sllv_epi32((v), (sh))

ALWAYS_INLINE GSVector8i gsv8i_eq32(GSVector8i a, GSVector8i b) { return _mm256_cmpeq_epi32(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_eq16(GSVector8i a, GSVector8i b) { return _mm256_cmpeq_epi16(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_gt32(GSVector8i a, GSVector8i b) { return _mm256_cmpgt_epi32(a, b); }
/* AVX2 has no _mm256_cmplt. flip args. */
ALWAYS_INLINE GSVector8i gsv8i_lt32(GSVector8i a, GSVector8i b) { return _mm256_cmpgt_epi32(b, a); }

ALWAYS_INLINE GSVector8i gsv8i_max_s32(GSVector8i a, GSVector8i b) { return _mm256_max_epi32(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_min_s32(GSVector8i a, GSVector8i b) { return _mm256_min_epi32(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_max_s16(GSVector8i a, GSVector8i b) { return _mm256_max_epi16(a, b); }
ALWAYS_INLINE GSVector8i gsv8i_min_u16(GSVector8i a, GSVector8i b) { return _mm256_min_epu16(a, b); }

ALWAYS_INLINE bool gsv8i_alltrue(GSVector8i v)
{ return (bool)_mm256_testc_si256(v, _mm256_cmpeq_epi32(v, v)); }
ALWAYS_INLINE bool gsv8i_allfalse(GSVector8i v)
{ return (bool)_mm256_testz_si256(v, v); }

#define gsv8i_extract32(v, i)      _mm256_extract_epi32((v), (i))
#define gsv8i_extract16(v, i)      ((u16)(unsigned)_mm256_extract_epi16((v), (i)))
/* AVX2 has no _mm256_insert_epi16 in immediate form; emulate by extract-128
 * → insert16 → insert-128. Done via macro to keep call-site compile-time. */
#define gsv8i_insert16(v, val, i)                                                              \
  ((((i) < 8) ?                                                                                 \
    _mm256_inserti128_si256((v),                                                                \
        _mm_insert_epi16(_mm256_extracti128_si256((v), 0), (val), ((i) & 7)), 0)               \
    :                                                                                           \
    _mm256_inserti128_si256((v),                                                                \
        _mm_insert_epi16(_mm256_extracti128_si256((v), 1), (val), ((i) & 7)), 1)))

/* low128 / high128: extract a 128-bit half. */
ALWAYS_INLINE GSVector4i gsv8i_low128 (GSVector8i v) { return _mm256_castsi256_si128(v); }
ALWAYS_INLINE GSVector4i gsv8i_high128(GSVector8i v) { return _mm256_extracti128_si256(v, 1); }

/* u16to32 of low 8 u16 lanes → 8 u32 lanes. Matches GSVector8i::u16to32. */
ALWAYS_INLINE GSVector8i gsv8i_u16to32(GSVector4i v) { return _mm256_cvtepu16_epi32(v); }
/* Broadcast a 128-bit value to both halves. */
ALWAYS_INLINE GSVector8i gsv8i_broadcast128(GSVector4i v)
{ return _mm256_broadcastsi128_si256(v); }
/* Broadcast from memory. */
ALWAYS_INLINE GSVector8i gsv8i_broadcast128_load(const void* p)
{ return _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)p)); }

ALWAYS_INLINE GSVector8i gsv8i_blend8(GSVector8i a, GSVector8i b, GSVector8i mask)
{ return _mm256_blendv_epi8(a, b, mask); }
#define gsv8i_blend16(a, b, imm)  _mm256_blend_epi16((a), (b), (imm))
#define gsv8i_blend32(a, b, imm)  _mm256_blend_epi32((a), (b), (imm))

#endif /* __AVX2__ */

/* ============================================================
 * Shorthand: GSVectorNi maps to the widest available int vec.
 * Keep behind GSV_USE_NI=1 so callers opt in explicitly. The
 * SIMD .inl port uses GSVector4i directly for the SSE4.1 vtable,
 * GSVector8i directly for the AVX2 vtable. no shim needed.
 * ============================================================ */

#endif /* CUPID_COMMON_GSVECTOR_H */
