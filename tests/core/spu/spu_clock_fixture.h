/* cupid-ps1 component clock advancement through the shared scheduler. */
#ifndef CUPID_TESTS_SPU_CLOCK_FIXTURE_H
#define CUPID_TESTS_SPU_CLOCK_FIXTURE_H

#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/spu/spu.h"

static void advance_spu_clock(tick_count_t ticks)
{
  if (ticks <= 0) {
    spu_execute(ticks);
    return;
  }
  cpu_add_pending_ticks(ticks);
  timing_events_run_events();
  /* A status read also exposes progress between automatic deadlines. */
  (void)spu_read_register(0x1AE);
}

#endif
