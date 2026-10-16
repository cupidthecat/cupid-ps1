/*
 * Runtime CPU-feature detection for ISA-specialized code paths.
 *
 * Idempotent.  Safe to call cpu_features_init() multiple times; the first
 * call probes via cpuid + xgetbv and caches results, later calls are no-ops.
 *
 * On non-x86_64 builds every cpu_has_* returns false and isa_name is
 * "SCALAR".
 */

#ifndef CUPID_UTIL_CPU_FEATURES_H
#define CUPID_UTIL_CPU_FEATURES_H

#include <stdbool.h>

void        cpu_features_init(void);
bool        cpu_has_sse2(void);
bool        cpu_has_sse41(void);
bool        cpu_has_avx(void);
bool        cpu_has_avx2(void);
const char* cpu_features_isa_name(void);

#endif /* CUPID_UTIL_CPU_FEATURES_H */
