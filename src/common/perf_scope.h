/*
 * perf-tool registration for JIT'd code.  Linux only; no-ops unless built
 * with ProfileWithPerf or ProfileWithPerfJitDump.  Consumers are the
 * recompiler.
 */

#ifndef CUPID_COMMON_PERF_SCOPE_H
#define CUPID_COMMON_PERF_SCOPE_H

#include "common/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char* prefix;
} perf_scope_t;

static inline void perf_scope_init(perf_scope_t* ps, const char* prefix)
{
  ps->prefix = prefix;
}

static inline bool perf_scope_has_prefix(const perf_scope_t* ps)
{
  return (ps->prefix && ps->prefix[0]);
}

void perf_scope_register     (const perf_scope_t* ps, const void* ptr, size_t size, const char* symbol);
void perf_scope_register_pc  (const perf_scope_t* ps, const void* ptr, size_t size, u32 pc);
void perf_scope_register_key (const perf_scope_t* ps, const void* ptr, size_t size, const char* prefix, u64 key);

#ifdef __cplusplus
}
#endif

#endif
