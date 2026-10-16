/* cupid-ps1 CPU execution test fixture. */
#ifndef CUPID_TESTS_CORE_CPU_EXECUTION_FIXTURE_H
#define CUPID_TESTS_CORE_CPU_EXECUTION_FIXTURE_H

#include "tests/test_harness.h"
#include "common/error.h"
#include "core/memory/bus.h"
#include "core/cpu/cpu_code_cache.h"
#include "core/cpu/cpu_core.h"
#include "core/cpu/cpu_core_private.h"
#include "core/cpu/cpu_execution_stats.h"
#include "core/state/save_state_version.h"
#include "core/config/settings.h"
#include "core/scheduler/timing_event.h"
#include "util/state/state_wrapper.h"

#include <unistd.h>

static void stop_cpu(void* user, tick_count_t ticks, tick_count_t late)
{
  (void)user;
  (void)ticks;
  (void)late;
  timing_events_cancel_running_event();
  cpu_exit_execution();
}

static void start_cpu_with_fastmem(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, u32 pc, const u32* words,
                                   size_t count)
{
  alarm(10);
  memset(&g_cpu_state, 0, sizeof(g_cpu_state));
  settings_init(&g_settings);
  g_settings.cpu_execution_mode = mode;
  g_settings.cpu_fastmem_mode   = fastmem;
  g_settings.gpu_pgxp_enable    = false;
  timing_events_initialize();
  cpu_execution_stats_initialize(true);
  Error error;
  Error_init(&error);
  EXPECT_TRUE(bus_allocate_memory(false, &error));
  bus_initialize();
  bus_reset();
  memcpy(bus_get_unprotected_ram_pointer() + (pc & bus_get_ram_mask()), words, count * sizeof(*words));
  cpu_initialize();
  cpu_reset();
  EXPECT_TRUE(cpu_code_cache_process_startup(&error));
  cpu_code_cache_reset();
  cpu_set_pc(pc);
  Error_destroy(&error);
}

static void start_cpu(cpu_execution_mode_t mode, u32 pc, const u32* words, size_t count)
{
  start_cpu_with_fastmem(mode, CPU_FASTMEM_MODE_DISABLED, pc, words, count);
}

static void run_slice(tick_count_t duration)
{
  timing_event_t stop;
  timing_event_init(&stop, "transition-stop", 15, duration, duration, stop_cpu, NULL);
  timing_event_activate(&stop);
  cpu_execute();
  timing_event_deactivate(&stop);
  timing_event_destroy(&stop);
}

static void finish_cpu(void)
{
  cpu_code_cache_shutdown();
  cpu_code_cache_process_shutdown();
  cpu_shutdown();
  timing_events_shutdown();
  bus_shutdown();
  bus_release_memory();
  settings_destroy(&g_settings);
  alarm(0);
}

#endif
