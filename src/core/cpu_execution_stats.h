/* Optional execution counters. Counts describe executed paths, not hardware accuracy. */
#ifndef CUPID_CORE_CPU_EXECUTION_STATS_H
#define CUPID_CORE_CPU_EXECUTION_STATS_H

#include "common/types.h"

typedef struct {
  u64 interpreted_instructions;
  u64 native_blocks_entered;
  u64 native_blocks_completed;
} cpu_execution_stats_t;

/* Enable before compiling blocks. Resetting counts does not change generated code. */
void                   cpu_execution_stats_initialize(bool enabled);
bool                   cpu_execution_stats_is_enabled(void);
cpu_execution_stats_t* cpu_execution_stats_get(void);

#endif
