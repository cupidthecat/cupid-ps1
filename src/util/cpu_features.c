/*
 * x86_64: GCC's <cpuid.h> + a hand-rolled xgetbv (no intrinsic in older GCC
 * without -mxsave).  XCR0 bits 1+2 must both be set before AVX/AVX2 may be
 * reported, otherwise the OS hasn't enabled the YMM register file and any
 * VEX-encoded instruction would #UD.
 *
 * Other arches: every probe returns false; isa_name is "SCALAR".
 */

#include "util/cpu_features.h"

#include <stdbool.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#endif

static bool s_initialised;
static bool s_sse2;
static bool s_sse41;
static bool s_avx;
static bool s_avx2;

#if defined(__x86_64__) || defined(__i386__)
static bool xgetbv_xcr0_avx_enabled(void)
{
  unsigned int eax, edx;
  /* xgetbv with %ecx=0 reads XCR0.  AVX needs bit 1 (SSE state) + bit 2 (AVX
   * state), AVX-512 would also need bits 5/6/7. */
  __asm__ volatile ("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
  (void)edx;
  return (eax & 0x6u) == 0x6u;
}
#endif

void cpu_features_init(void)
{
  if (s_initialised) return;

#if defined(__x86_64__) || defined(__i386__)
  unsigned int max_leaf = __get_cpuid_max(0, NULL);
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

  if (max_leaf >= 1u && __get_cpuid(1u, &eax, &ebx, &ecx, &edx)) {
    s_sse2  = (edx & (1u << 26)) != 0u;          /* CPUID.1:EDX[26] */
    s_sse41 = (ecx & (1u << 19)) != 0u;          /* CPUID.1:ECX[19] */
    const bool xsave_osxsave =
        (ecx & (1u << 26)) != 0u &&              /* CPU supports XSAVE */
        (ecx & (1u << 27)) != 0u;                /* OS enabled XSAVE */
    const bool avx_cpu = (ecx & (1u << 28)) != 0u; /* CPUID.1:ECX[28] */
    s_avx  = avx_cpu && xsave_osxsave && xgetbv_xcr0_avx_enabled();
  }

  if (s_avx && max_leaf >= 7u) {
    /* CPUID.(EAX=7,ECX=0):EBX[5] = AVX2.  Use cpuid_count for ECX=0 leaf. */
    unsigned int e7a = 0, e7b = 0, e7c = 0, e7d = 0;
    if (__get_cpuid_count(7u, 0u, &e7a, &e7b, &e7c, &e7d)) {
      s_avx2 = (e7b & (1u << 5)) != 0u;
    }
  }
#endif

  s_initialised = true;
}

bool cpu_has_sse2 (void) { return s_sse2;  }
bool cpu_has_sse41(void) { return s_sse41; }
bool cpu_has_avx  (void) { return s_avx;   }
bool cpu_has_avx2 (void) { return s_avx2;  }

const char* cpu_features_isa_name(void)
{
  if (s_avx2)  return "AVX2";
  if (s_avx)   return "AVX";
  if (s_sse41) return "SSE4.1";
  if (s_sse2)  return "SSE2";
  return "SCALAR";
}
