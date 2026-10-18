/* cupid-ps1 counter clocks, reset phases and state continuation. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/timers/timers.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/spu/spu.h"
#include "core/system/system.h"

#include <limits.h>

static void begin_timers(void)
{
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
  interrupt_controller_reset();
  timers_initialize();
}

static void end_timers(void)
{
  timers_shutdown();
  finish_cpu();
}

static size_t save_counters(u8* bytes, size_t capacity, u32 version)
{
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, capacity, version);
  EXPECT_TRUE(timers_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  return state_wrapper_get_position(&state);
}

static void load_counters(const u8* bytes, size_t size, u32 version)
{
  state_wrapper_t state;
  state_wrapper_init_read(&state, bytes, size, version);
  EXPECT_TRUE(timers_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
}

TEST(TimerCounter, system_target_readback)
{
  const u32 after_target[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 0, 1};
  u32       failures = 0, observed = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    begin_timers();
    timers_write_register(timer * 16 + 8, 10);
    timers_write_register(timer * 16 + 4, 8);
    bool reached = false;
    for (u32 tick = 0; tick < 32; tick++) {
      timers_add_ticks(timer, 1);
      if (timers_read_register(timer * 16) == 10) {
        reached = true;
        break;
      }
    }
    printf("timer-target-readback timer=%u reached=%u\n", timer, reached);
    failures += !reached;
    if (reached) {
      for (u32 i = 0; i < sizeof(after_target) / sizeof(after_target[0]); i++) {
        timers_add_ticks(timer, 1);
        const u32 value = timers_read_register(timer * 16);
        printf("timer-target-phase timer=%u step=%u actual=%u expected=%u\n", timer, i, value, after_target[i]);
        failures += value != after_target[i];
        observed++;
      }
    }
    end_timers();
  }
  fflush(stdout);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(observed, 45);
}

TEST(TimerCounter, full_16bit_period)
{
  const u32 increments[] = {0xFFFE, 1, 1, 0x10000, 0x10001};
  const u32 expected[]   = {0xFFFE, 0xFFFF, 0, 0, 1};
  u32       failures     = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    begin_timers();
    for (u32 i = 0; i < 5; i++) {
      timers_add_ticks(timer, (tick_count_t)increments[i]);
      const u32 value = timers_dbg_counter(timer), overflow = !!(timers_dbg_mode(timer) & 0x1000);
      printf("timer-wrap timer=%u step=%u actual=%04X expected=%04X overflow=%u expected_overflow=%u\n", timer, i,
             value, expected[i], overflow, i >= 2);
      failures += value != expected[i] || overflow != (i >= 2);
    }
    end_timers();
  }
  fflush(stdout);
  EXPECT_EQ(failures, 0);
}

TEST(TimerCounter, timer2_sync_clocks)
{
  u32 failures = 0;
  for (u32 divided = 0; divided < 2; divided++) {
    for (u32 sync = 0; sync < 4; sync++) {
      begin_timers();
      timers_write_register(0x24, (divided ? 0x200 : 0) | (sync << 1) | 1);
      run_slice(1000);
      const u32  value    = timers_read_register(0x20);
      const bool expected = sync == 1 || sync == 2;
      printf("timer-sync divided=%u sync=%u counter=%u counting=%u expected=%u\n", divided, sync, value, value > 0,
             expected);
      failures += (value > 0) != expected;
      end_timers();
    }
  }
  fflush(stdout);
  EXPECT_EQ(failures, 0);
}

TEST(TimerCounter, reset_clock_state)
{
  u8 cold[512], warm[512];
  begin_timers();
  const size_t size = save_counters(cold, sizeof(cold), SAVE_STATE_VERSION);
  timers_write_register(0x24, 0x200);
  timers_reset();
  EXPECT_EQ(save_counters(warm, sizeof(warm), SAVE_STATE_VERSION), size);
  const bool equal = memcmp(cold, warm, size) == 0;
  end_timers();
  EXPECT_TRUE(equal);
}

TEST(TimerCounter, phase_state_and_legacy)
{
  u8 current[512], legacy[512];
  begin_timers();
  timers_write_register(0x28, 10);
  timers_write_register(0x24, 8);
  bool reached = false;
  for (u32 i = 0; i < 32; i++) {
    timers_add_ticks(2, 1);
    if (timers_read_register(0x20) == 10) {
      reached = true;
      break;
    }
  }
  const size_t current_size = save_counters(current, sizeof(current), 88);
  const size_t legacy_size  = save_counters(legacy, sizeof(legacy), 87);
  timers_add_ticks(2, 4);
  load_counters(current, current_size, 88);
  const u32 restored_target = timers_read_register(0x20);
  timers_add_ticks(2, 1);
  const u32 first_zero = timers_read_register(0x20);
  timers_add_ticks(2, 1);
  const u32 second_zero = timers_read_register(0x20);
  timers_add_ticks(2, 1);
  const u32 next_one = timers_read_register(0x20);
  load_counters(legacy, legacy_size, 87);
  const u32 legacy_zero = timers_read_register(0x20);
  timers_add_ticks(2, 1);
  const u32 legacy_one = timers_read_register(0x20);
  end_timers();
  EXPECT_TRUE(reached);
  EXPECT_EQ(current_size, 62);
  EXPECT_EQ(legacy_size, 59);
  EXPECT_EQ(restored_target, 10);
  EXPECT_EQ(first_zero, 0);
  EXPECT_EQ(second_zero, 0);
  EXPECT_EQ(next_one, 1);
  EXPECT_EQ(legacy_zero, 0);
  EXPECT_EQ(legacy_one, 1);
}

TEST(TimerCounter, irq_deadlines)
{
  u32 observations = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    for (u32 external = 0; external < 2; external++) {
      begin_timers();
      const u32 clock = external ? (timer == 2 ? 0x200 : 0x100) : 0;
      timers_write_register(timer * 16 + 8, 10);
      timers_write_register(timer * 16 + 4, clock | 0x58);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), external ? 10 : 11);
      timers_add_ticks(timer, external ? 4 : 5);
      EXPECT_EQ(timers_dbg_counter(timer), 4);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), 6);
      timers_add_ticks(timer, 6);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), external ? 10 : 12);
      timers_add_ticks(timer, external ? 0 : 2);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), 10);
      timers_write_register(timer * 16 + 4, clock | 0x70);
      timers_write_register(timer * 16, 0xFFFE);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), external ? 2 : 3);
      timers_add_ticks(timer, external ? 2 : 3);
      EXPECT_EQ(timers_dbg_counter(timer), 0);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), 10);
      printf("timer-deadline timer=%u external=%u checks=6\n", timer, external);
      observations += 6;
      end_timers();
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 36);
}

TEST(TimerCounter, irq_pulse_and_toggle)
{
  u32 failures = 0, observations = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    for (u32 external = 0; external < 2; external++) {
      for (u32 toggle = 0; toggle < 2; toggle++) {
        for (u32 repeat = 0; repeat < 2; repeat++) {
          begin_timers();
          const u32 bit   = 1u << (INTERRUPT_CONTROLLER_IRQ_TMR0 + timer);
          const u32 clock = external ? (timer == 2 ? 0x200 : 0x100) : 0;
          const u32 mode  = clock | 0x18 | (toggle ? 0x80 : 0) | (repeat ? 0x40 : 0);
          timers_write_register(timer * 16 + 8, 3);
          timers_write_register(timer * 16 + 4, mode);
          timers_add_ticks(timer, external ? 0 : 1);
          for (u32 event = 0; event < 6; event++) {
            timers_add_ticks(timer, 3);
            const u32          status            = !!(irq_dbg_status_register() & bit);
            const u32          flags             = timers_read_register(timer * 16 + 4);
            const u32          expected_irq      = (repeat || event == 0) && (!toggle || !(event & 1));
            const u32          expected_request  = !toggle || (repeat && (event & 1));
            const tick_count_t expected_deadline = repeat ? (external ? 3 : 5) : INT32_MAX;
            printf("timer-irq timer=%u external=%u toggle=%u repeat=%u event=%u status=%u expected=%u request=%u "
                   "expected_request=%u\n",
                   timer, external, toggle, repeat, event, status, expected_irq, !!(flags & 0x400), expected_request);
            failures += status != expected_irq || !!(flags & 0x400) != expected_request || (flags & 0x1800) != 0x800 ||
                        timers_get_ticks_until_irq(timer) != expected_deadline;
            EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1800, 0);
            interrupt_controller_write_register(0, ~bit);
            timers_add_ticks(timer, external ? 0 : 2);
            observations++;
          }
          end_timers();
        }
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 144);
  EXPECT_EQ(failures, 0);
}

TEST(TimerCounter, zero_and_maximum_targets)
{
  for (u32 timer = 0; timer < 3; timer++) {
    begin_timers();
    const u32 bit = 1u << (INTERRUPT_CONTROLLER_IRQ_TMR0 + timer);
    timers_write_register(timer * 16 + 8, 0);
    timers_write_register(timer * 16 + 4, 0xF8);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x800);
    EXPECT_TRUE(irq_dbg_status_register() & bit);
    interrupt_controller_write_register(0, ~bit);
    timers_add_ticks(timer, 1);
    EXPECT_EQ(timers_dbg_counter(timer), 0);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1800, 0);
    timers_add_ticks(timer, 0xFFFF);
    EXPECT_EQ(timers_dbg_counter(timer), 0xFFFF);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1800, 0);
    timers_add_ticks(timer, 1);
    EXPECT_EQ(timers_dbg_counter(timer), 0);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x1C00);
    EXPECT_FALSE(irq_dbg_status_register() & bit);

    timers_reset();
    interrupt_controller_reset();
    timers_write_register(timer * 16 + 8, 0xFFFF);
    timers_write_register(timer * 16 + 4, 0xF8);
    timers_add_ticks(timer, 0x10000);
    EXPECT_EQ(timers_dbg_counter(timer), 0xFFFF);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x800);
    interrupt_controller_write_register(0, ~bit);
    timers_add_ticks(timer, 1);
    EXPECT_EQ(timers_dbg_counter(timer), 0);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x1400);
    EXPECT_FALSE(irq_dbg_status_register() & bit);
    printf("timer-edge-targets timer=%u zero-and-ffff=checked\n", timer);
    end_timers();
  }
  fflush(stdout);
}

typedef struct {
  u8     bytes[128];
  size_t size;
  u32    irq_edges;
} timer_observation_t;

static timer_observation_t observe_counters(u32 timer, u32 initial_edges)
{
  timer_observation_t result = {0};
  state_wrapper_t     state;
  state_wrapper_init_write(&state, result.bytes, sizeof(result.bytes), SAVE_STATE_VERSION);
  EXPECT_TRUE(timers_do_state(&state));
  EXPECT_TRUE(interrupt_controller_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  result.size      = state_wrapper_get_position(&state);
  result.irq_edges = irq_dbg_set_counts()[INTERRUPT_CONTROLLER_IRQ_TMR0 + timer] - initial_edges;
  return result;
}

TEST(TimerCounter, bulk_matches_single_ticks)
{
  const u32 targets[]    = {0, 1, 10, 0xFFFE, 0xFFFF};
  const u32 modes[]      = {0x58, 0xD8, 0x38, 0xF8};
  const u32 increments[] = {0, 1, 2, 3, 0x10000, 7};
  u32       failures = 0, observations = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    begin_timers();
    for (u32 external = 0; external < 2; external++) {
      for (u32 target = 0; target < 5; target++) {
        for (u32 mode = 0; mode < 4; mode++) {
          timer_observation_t bulk[6];
          for (u32 single = 0; single < 2; single++) {
            timers_reset();
            interrupt_controller_reset();
            const u32 clock         = external ? (timer == 2 ? 0x200 : 0x100) : 0;
            const u32 initial_edges = irq_dbg_set_counts()[INTERRUPT_CONTROLLER_IRQ_TMR0 + timer];
            timers_write_register(timer * 16 + 8, targets[target]);
            timers_write_register(timer * 16 + 4, modes[mode] | clock);
            for (u32 step = 0; step < 6; step++) {
              if (single) {
                for (u32 tick = 0; tick < increments[step]; tick++)
                  timers_add_ticks(timer, 1);
              } else {
                timers_add_ticks(timer, (tick_count_t)increments[step]);
              }
              const timer_observation_t current = observe_counters(timer, initial_edges);
              if (!single) {
                bulk[step] = current;
              } else {
                const bool equal = current.size == bulk[step].size && current.irq_edges == bulk[step].irq_edges &&
                                   memcmp(current.bytes, bulk[step].bytes, current.size) == 0;
                printf("timer-batching timer=%u external=%u target=%u mode=%03X step=%u equal=%u edges=%u\n", timer,
                       external, targets[target], modes[mode], step, equal, current.irq_edges);
                failures += !equal;
                observations++;
              }
            }
          }
        }
      }
    }
    end_timers();
  }
  fflush(stdout);
  EXPECT_EQ(observations, 720);
  EXPECT_EQ(failures, 0);
}

static void advance_system_clock(tick_count_t ticks)
{
  cpu_add_pending_ticks(ticks);
  timing_events_run_events();
}

TEST(TimerCounter, gpu_gate_modes)
{
  const u32 expected[2][4][3] = {{{63, 63, 127}, {63, 127, 64}, {0, 63, 63}, {0, 63, 127}},
                                 {{64, 64, 128}, {64, 128, 64}, {0, 64, 64}, {0, 64, 128}}};
  u32       failures = 0, observations = 0;
  for (u32 timer = 0; timer < 2; timer++) {
    for (u32 external = 0; external < 2; external++) {
      for (u32 sync = 0; sync < 4; sync++) {
        begin_timers();
        timers_write_register(timer * 16 + 8, 0xFFFF);
        timers_write_register(timer * 16 + 4, (external ? 0x100 : 0) | (sync << 1) | 1);
        for (u32 step = 0; step < 3; step++) {
          if (step) timers_set_gate(timer, step == 1);
          if (!external)
            advance_system_clock(64);
          else if (timers_is_using_external_clock(timer))
            timers_add_ticks(timer, 64);
          const u32 actual = timers_read_register(timer * 16);
          printf("timer-gate timer=%u external=%u sync=%u step=%u counter=%u expected=%u\n", timer, external, sync,
                 step, actual, expected[external][sync][step]);
          failures += actual != expected[external][sync][step];
          observations++;
        }
        if (sync == 3) EXPECT_FALSE(timers_is_sync_enabled(timer));
        end_timers();
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 48);
  EXPECT_EQ(failures, 0);
}

static timing_event_t* counter_event(void)
{
  timing_event_t* event = *timing_events_get_head_event_ptr();
  while (event && strncmp(event->name_data, "Timer SysClk Interrupt", event->name_len) != 0)
    event = event->next;
  EXPECT_NOT_NULL(event);
  return event;
}

TEST(TimerCounter, divided_clock_carry_restore)
{
  u8 counters[128], cpu[16384], timing[4096];
  begin_timers();
  /* The CPU fixture leaves the system slice unset. Initialize the clock
   * consumers before testing the timer's actual scheduled deadline. */
  spu_initialize();
  system_update_overclock();
  EXPECT_TRUE(system_get_max_slice_ticks() > 80);
  timers_write_register(0x28, 10);
  timers_write_register(0x24, 0x258);
  EXPECT_EQ(timing_event_get_ticks_until_next_execution(counter_event()), 80);
  advance_system_clock(13);
  EXPECT_EQ(timers_read_register(0x20), 1);
  EXPECT_EQ(timing_event_get_ticks_until_next_execution(counter_event()), 67);
  const size_t    counters_size = save_counters(counters, sizeof(counters), SAVE_STATE_VERSION);
  state_wrapper_t state;
  state_wrapper_init_write(&state, cpu, sizeof(cpu), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&state));
  const size_t cpu_size = state_wrapper_get_position(&state);
  state_wrapper_init_write(&state, timing, sizeof(timing), SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&state));
  const size_t timing_size = state_wrapper_get_position(&state);
  advance_system_clock(67);
  EXPECT_EQ(timers_read_register(0x20), 0);
  EXPECT_TRUE(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_TMR2));
  interrupt_controller_reset();
  state_wrapper_init_read(&state, cpu, cpu_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&state));
  load_counters(counters, counters_size, SAVE_STATE_VERSION);
  state_wrapper_init_read(&state, timing, timing_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&state));
  EXPECT_EQ(timing_event_get_ticks_until_next_execution(counter_event()), 67);
  timers_set_gate(2, true);
  EXPECT_EQ(timers_read_register(0x20), 1);
  advance_system_clock(66);
  EXPECT_EQ(timers_read_register(0x20), 9);
  EXPECT_FALSE(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_TMR2));
  advance_system_clock(1);
  EXPECT_EQ(timers_read_register(0x20), 0);
  EXPECT_TRUE(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_TMR2));
  spu_shutdown();
  end_timers();
}

TEST(TimerCounter, reject_invalid_phase_state)
{
  u8 valid[128], invalid[128];
  begin_timers();
  const size_t size = save_counters(valid, sizeof(valid), SAVE_STATE_VERSION);
  for (u32 timer = 0; timer < 3; timer++) {
    for (u32 bad = 0; bad < 3; bad++) {
      memcpy(invalid, valid, size);
      if (bad < 2)
        invalid[timer * 18 + 17] = bad ? 255 : 3;
      else
        invalid[timer * 18 + 6] = 1;
      state_wrapper_t state;
      state_wrapper_init_read(&state, invalid, size, SAVE_STATE_VERSION);
      EXPECT_FALSE(timers_do_state(&state));
      EXPECT_TRUE(state_wrapper_has_error(&state));
      load_counters(valid, size, SAVE_STATE_VERSION);
    }
  }
  state_wrapper_t truncated;
  state_wrapper_init_read(&truncated, valid, size - 1, SAVE_STATE_VERSION);
  EXPECT_FALSE(timers_do_state(&truncated));
  EXPECT_TRUE(state_wrapper_has_error(&truncated));
  end_timers();
}

TEST(TimerCounter, write_comparison_edges)
{
  for (u32 timer = 0; timer < 3; timer++) {
    begin_timers();
    const u32 bit = 1u << (INTERRUPT_CONTROLLER_IRQ_TMR0 + timer);
    timers_write_register(timer * 16 + 8, 20);
    timers_write_register(timer * 16 + 4, 0x98);
    timers_write_register(timer * 16, 10);
    timers_write_register(timer * 16 + 8, 10);
    EXPECT_EQ(timers_read_register(timer * 16), 10);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x400);
    EXPECT_FALSE(irq_dbg_status_register() & bit);
    timers_add_ticks(timer, 1);
    EXPECT_EQ(timers_dbg_counter(timer), 10);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1800, 0);
    timers_add_ticks(timer, 1);
    EXPECT_EQ(timers_dbg_counter(timer), 11);
    EXPECT_FALSE(irq_dbg_status_register() & bit);

    timers_write_register(timer * 16 + 4, 0x98);
    timers_add_ticks(timer, 11);
    EXPECT_TRUE(irq_dbg_status_register() & bit);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x800);
    interrupt_controller_write_register(0, ~bit);
    timers_write_register(timer * 16, 0);
    timers_add_ticks(timer, 11);
    EXPECT_FALSE(irq_dbg_status_register() & bit);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x800);
    EXPECT_EQ(timers_get_ticks_until_irq(timer), INT32_MAX);
    timers_write_register(timer * 16 + 4, 0x98);
    EXPECT_EQ(timers_dbg_counter(timer), 0);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x400);
    EXPECT_EQ(timers_get_ticks_until_irq(timer), 11);
    timers_add_ticks(timer, 11);
    EXPECT_TRUE(irq_dbg_status_register() & bit);
    EXPECT_EQ(timers_read_register(timer * 16 + 4) & 0x1C00, 0x800);
    printf("timer-write-edges timer=%u checked\n", timer);
    end_timers();
  }
  fflush(stdout);
}

TEST(TimerCounter, phase_restore_matrix)
{
  u32 failures = 0, observations = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    for (u32 external = 0; external < 2; external++) {
      for (u32 phase = 0; phase < 4; phase++) {
        begin_timers();
        const u32 clock = external ? (timer == 2 ? 0x200 : 0x100) : 0;
        timers_write_register(timer * 16 + 8, 10);
        timers_write_register(timer * 16 + 4, clock | 8);
        if (phase) timers_add_ticks(timer, (external ? 10 : 11) + phase - 1);
        u8           initial[128], expected[8][128], actual[128];
        const size_t size = save_counters(initial, sizeof(initial), SAVE_STATE_VERSION);
        for (u32 step = 0; step < 8; step++) {
          timers_add_ticks(timer, 1);
          EXPECT_EQ(save_counters(expected[step], sizeof(expected[step]), SAVE_STATE_VERSION), size);
        }
        load_counters(initial, size, SAVE_STATE_VERSION);
        for (u32 step = 0; step < 8; step++) {
          timers_add_ticks(timer, 1);
          EXPECT_EQ(save_counters(actual, sizeof(actual), SAVE_STATE_VERSION), size);
          const bool equal = memcmp(actual, expected[step], size) == 0;
          printf("timer-phase-restore timer=%u external=%u phase=%u step=%u equal=%u\n", timer, external, phase, step,
                 equal);
          failures += !equal;
          observations++;
        }
        end_timers();
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 192);
  EXPECT_EQ(failures, 0);
}
