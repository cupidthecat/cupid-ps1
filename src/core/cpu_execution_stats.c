#include "cpu_execution_stats.h"

static bool                  s_enabled;
static cpu_execution_stats_t s_stats;

void cpu_execution_stats_initialize(bool enabled)
{
  s_enabled = enabled;
  s_stats   = (cpu_execution_stats_t){0};
}

bool cpu_execution_stats_is_enabled(void)
{
  return s_enabled;
}

cpu_execution_stats_t* cpu_execution_stats_get(void)
{
  return &s_stats;
}
