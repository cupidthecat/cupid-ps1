/* cupid-ps1 shared system-clock deadlines during GPU gate changes. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/spu/spu.h"
#include "core/system/system.h"
#include "core/timers/timers.h"

static timing_event_t* gate_system_event(void) {
  const char* name = "Timer SysClk Interrupt";
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next)
    if (event->name_len == strlen(name) && memcmp(event->name_data, name, event->name_len) == 0) return event;
  EXPECT_TRUE(false);
  return NULL;
}

static void check_shared_deadlines(bool external) {
  u32 failures = 0, cases = 0;
  for (u32 timer = 0; timer < 2; timer++)
    for (u32 sync = 0; sync < 4; sync++)
      for (u32 edge = 0; edge < 2; edge++)
        for (u32 divided = 0; divided < 2; divided++) {
          const u32 loop[] = {0x1000FFFF, 0};
          start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
          interrupt_controller_reset();
          timers_initialize();
          spu_initialize();
          system_update_overclock();
          EXPECT_TRUE(system_get_max_slice_ticks() > 80);
          timers_write_register(timer * 16 + 4, (external ? 0x100 : 0) | (sync << 1) | 1);
          if (!edge) timers_set_gate(timer, true);
          timers_write_register(0x28, 10);
          timers_write_register(0x24, (divided ? 0x200 : 0) | 0x18);
          timing_event_t*    event  = gate_system_event();
          const tick_count_t period = divided ? 80 : 11;
          EXPECT_EQ(timing_event_get_ticks_until_next_execution(event), period);
          const global_ticks_t deadline = event->next_run_time;
          const tick_count_t   elapsed  = period / 3;
          cpu_add_pending_ticks(elapsed);
          timers_set_gate(timer, edge != 0);
          const global_ticks_t changed = event->next_run_time;
          const u32            bit     = 1u << INTERRUPT_CONTROLLER_IRQ_TMR2;
          cpu_add_pending_ticks(period - elapsed - 1);
          timing_events_run_events();
          const bool early = (irq_dbg_status_register() & bit) != 0;
          cpu_add_pending_ticks(1);
          timing_events_run_events();
          const bool due = (irq_dbg_status_register() & bit) != 0;
          printf("timer-shared-deadline timer=%u external=%u sync=%u edge=%u divided=%u expected=%llu "
                 "actual=%llu early=%u due=%u\n",
                 timer, external, sync, edge, divided, (unsigned long long)deadline, (unsigned long long)changed, early,
                 due);
          failures += changed != deadline || early || !due;
          cases++;
          spu_shutdown();
          timers_shutdown();
          finish_cpu();
        }
  EXPECT_EQ(cases, 32);
  EXPECT_EQ(failures, 0);
}

TEST(TimerGateScheduling, external_gates_keep_other_counter_deadlines) {
  check_shared_deadlines(true);
}

TEST(TimerGateScheduling, system_gates_keep_other_counter_deadlines) {
  check_shared_deadlines(false);
}
