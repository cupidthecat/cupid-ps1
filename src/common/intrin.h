/*
 *

 * Drops MSVC, Apple, and RISC-V/LoongArch branches - Linux on x64/ARM only.
 */
#ifndef CUPID_COMMON_INTRIN_H
#define CUPID_COMMON_INTRIN_H

#include "align.h"
#include "types.h"

#if defined(CPU_ARCH_X86) || defined(CPU_ARCH_X64)
#  define CPU_ARCH_SIMD 1
#  define CPU_ARCH_SSE 1
#  include <emmintrin.h>
#  include <immintrin.h>
#  include <smmintrin.h>
#  include <tmmintrin.h>
#  if defined(__AVX2__)
#    define CPU_ARCH_AVX 1
#    define CPU_ARCH_AVX2 1
#    define CPU_ARCH_SSE41 1
#  elif defined(__AVX__)
#    define CPU_ARCH_AVX 1
#    define CPU_ARCH_SSE41 1
#  elif defined(__SSE4_1__)
#    define CPU_ARCH_SSE41 1
#  endif
#elif defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64)
#  define CPU_ARCH_SIMD 1
#  define CPU_ARCH_NEON 1
#  include <arm_neon.h>
#endif

#include <alloca.h>

#if defined(__clang__)
#  define DONT_VECTORIZE_THIS_LOOP _Pragma("clang loop vectorize(disable)")
#elif defined(__GNUC__)
#  define DONT_VECTORIZE_THIS_LOOP _Pragma("GCC novector")
#else
#  define DONT_VECTORIZE_THIS_LOOP
#endif

enum { VECTOR_ALIGNMENT = 16 };

#define VectorAlign(value) AlignUpPow2((value), VECTOR_ALIGNMENT)

/* Pointer-array memset.  Only the void*-width specialization is used by the
 * codebase; collapse the template down to a single function. */
ALWAYS_INLINE_RELEASE void MemsetPtrs(void** ptr, void* value, u32 count)
{
#if defined(CPU_ARCH_SSE)
  enum { PTRS_PER_VECTOR = 16 / sizeof(void*) };
  const u32 aligned   = count / PTRS_PER_VECTOR;
  const u32 remaining = count % PTRS_PER_VECTOR;
  const __m128i sval = _mm_set1_epi64x((long long)(intptr_t)value);
  void** dest = ptr;
  for (u32 i = 0; i < aligned; i++) {
    _mm_store_si128((__m128i*)dest, sval);
    dest += PTRS_PER_VECTOR;
  }
  for (u32 i = 0; i < remaining; i++)
    *(dest++) = value;
#elif defined(CPU_ARCH_NEON) && defined(CPU_ARCH_ARM64)
  enum { PTRS_PER_VECTOR = 16 / sizeof(void*) };
  const u32 aligned   = count / PTRS_PER_VECTOR;
  const u32 remaining = count % PTRS_PER_VECTOR;
  const uint64x2_t sval = vdupq_n_u64((uintptr_t)value);
  void** dest = ptr;
  for (u32 i = 0; i < aligned; i++) {
    vst1q_u64((u64*)dest, sval);
    dest += PTRS_PER_VECTOR;
  }
  for (u32 i = 0; i < remaining; i++)
    *(dest++) = value;
#else
  for (u32 i = 0; i < count; i++)
    ptr[i] = value;
#endif
}

ALWAYS_INLINE void MultiPause(void)
{
#if defined(CPU_ARCH_X86) || defined(CPU_ARCH_X64)
  _mm_pause(); _mm_pause(); _mm_pause(); _mm_pause();
  _mm_pause(); _mm_pause(); _mm_pause(); _mm_pause();
#elif defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_ARM32)
  __asm__ __volatile__("isb"); __asm__ __volatile__("isb");
  __asm__ __volatile__("isb"); __asm__ __volatile__("isb");
  __asm__ __volatile__("isb"); __asm__ __volatile__("isb");
  __asm__ __volatile__("isb"); __asm__ __volatile__("isb");
#endif
}

#endif /* CUPID_COMMON_INTRIN_H */
