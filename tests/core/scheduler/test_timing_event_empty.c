#include "tests/core/cpu/cpu_execution_fixture.h"

static void begin_empty_events(void)
{
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
  EXPECT_FALSE(cpu_has_pending_interrupt());
}

static void restore_empty_events(void)
{
  u8              before[256], after[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, before, sizeof(before), SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t size = state_wrapper_get_position(&state);
  EXPECT_EQ(size, 20);
  state_wrapper_init_read(&state, before, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  state_wrapper_init_write(&state, after, sizeof(after), SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(memcmp(before, after, size), 0);
}

TEST(TimingEventEmpty, idle_downcount)
{
  begin_empty_events();
  timing_events_update_cpu_downcount();
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  EXPECT_EQ(timing_events_get_global_tick_counter(), 0);
  printf("timing-empty stage=idle downcount=%u\n", g_cpu_state.downcount);
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, empty_state_restore)
{
  begin_empty_events();
  restore_empty_events();
  EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  printf("timing-empty stage=restore downcount=%u\n", g_cpu_state.downcount);
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, pending_interrupt)
{
  begin_empty_events();
  g_cpu_state.cop0_regs.sr.bits |= 0x101;
  g_cpu_state.cop0_regs.cause.bits |= 0x100;
  EXPECT_TRUE(cpu_has_pending_interrupt());
  timing_events_update_cpu_downcount();
  EXPECT_EQ(g_cpu_state.downcount, 0);
  g_cpu_state.cop0_regs.sr.bits &= ~0x101u;
  g_cpu_state.cop0_regs.cause.bits &= ~0x100u;
  timing_events_update_cpu_downcount();
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  printf("timing-empty stage=irq irq_downcount=0 cleared_downcount=%u\n", g_cpu_state.downcount);
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, commit_no_events)
{
  begin_empty_events();
  const u32 pending_before = cpu_get_pending_ticks();
  cpu_add_pending_ticks(17);
  timing_events_run_events();
  EXPECT_EQ(cpu_get_pending_ticks(), 0);
  EXPECT_EQ(timing_events_get_global_tick_counter(), pending_before + 17u);
  EXPECT_EQ(timing_events_get_event_run_tick_counter(), pending_before + 17u);
  timing_events_commit_leftover_ticks();
  EXPECT_EQ(timing_events_get_global_tick_counter(), pending_before + 17u);
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  printf("timing-empty stage=commit ticks=%llu pending=%u\n",
         (unsigned long long)timing_events_get_global_tick_counter(), cpu_get_pending_ticks());
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, pending_interrupt_dispatch)
{
  begin_empty_events();
  const u32 before_pc      = g_cpu_state.pc;
  const u32 pending_before = cpu_get_pending_ticks();
  g_cpu_state.cop0_regs.sr.bits |= 0x101;
  g_cpu_state.cop0_regs.cause.bits |= 0x100;
  EXPECT_TRUE(cpu_has_pending_interrupt());
  g_cpu_state.cop0_regs.sr.bits |= 1u << 22;
  cpu_add_pending_ticks(7);
  timing_events_run_events();
  EXPECT_FALSE(cpu_has_pending_interrupt());
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, before_pc);
  EXPECT_EQ(g_cpu_state.pc, 0xBFC00180);
  EXPECT_EQ((g_cpu_state.cop0_regs.cause.bits >> 2) & 31, 0);
  EXPECT_EQ(timing_events_get_global_tick_counter(), pending_before + 7u);
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  printf("timing-empty stage=irq-dispatch epc=%08X pc=%08X ticks=%llu\n", g_cpu_state.cop0_regs.EPC, g_cpu_state.pc,
         (unsigned long long)timing_events_get_global_tick_counter());
  fflush(stdout);
  finish_cpu();
}

typedef struct {
  timing_event_t* event;
  u32             calls;
  global_ticks_t  when;
  tick_count_t    ticks;
  bool            exit_cpu;
} empty_callback_t;

static void finish_last_event(void* user, tick_count_t ticks, tick_count_t late)
{
  empty_callback_t* result = user;
  result->calls++;
  result->when  = timing_events_get_global_tick_counter();
  result->ticks = ticks;
  EXPECT_EQ(late, 0);
  timing_event_deactivate(result->event);
  if (result->exit_cpu) stop_cpu(NULL, ticks, late);
}

static void check_last_event(bool restore)
{
  begin_empty_events();
  if (restore) restore_empty_events();
  timing_event_t   event;
  empty_callback_t result = {.event = &event, .exit_cpu = true};
  timing_event_init(&event, "last-observer", 13, 23, 23, finish_last_event, &result);
  const u32            pending_before = cpu_get_pending_ticks();
  const global_ticks_t expected_when  = timing_events_get_global_tick_counter() + pending_before + 23;
  timing_event_activate(&event);
  EXPECT_EQ(g_cpu_state.downcount, pending_before + 23u);
  cpu_execute();
  EXPECT_EQ(result.calls, 1);
  EXPECT_EQ(result.when, expected_when);
  EXPECT_EQ(result.ticks, 23);
  EXPECT_FALSE(timing_event_is_active(&event));
  EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
  /* CPU exit jumps out of dispatch; refresh its countdown before resuming. */
  timing_events_update_cpu_downcount();
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  EXPECT_TRUE(cpu_execution_stats_get()->interpreted_instructions > 0);
  printf("timing-empty stage=last-callback restored=%u calls=%u when=%llu ticks=%d\n", restore, result.calls,
         (unsigned long long)result.when, result.ticks);
  timing_event_destroy(&event);
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, first_event_after_restore)
{
  check_last_event(true);
}
TEST(TimingEventEmpty, last_event_callback)
{
  begin_empty_events();
  const u32            pending_before = cpu_get_pending_ticks();
  const global_ticks_t expected_when  = timing_events_get_global_tick_counter() + pending_before + 23;
  timing_event_t       event;
  empty_callback_t     result = {.event = &event};
  timing_event_init(&event, "last-observer", 13, 23, 23, finish_last_event, &result);
  timing_event_activate(&event);
  cpu_add_pending_ticks(23);
  timing_events_run_events();
  EXPECT_EQ(result.calls, 1);
  EXPECT_EQ(result.when, expected_when);
  EXPECT_EQ(result.ticks, 23);
  EXPECT_FALSE(timing_event_is_active(&event));
  EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  EXPECT_EQ(timing_events_get_global_tick_counter(), expected_when);
  printf("timing-empty stage=returned-callback calls=%u when=%llu ticks=%d\n", result.calls,
         (unsigned long long)result.when, result.ticks);
  timing_event_destroy(&event);
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, deactivate_last_event)
{
  begin_empty_events();
  timing_event_t   event;
  empty_callback_t result = {.event = &event};
  timing_event_init(&event, "last-observer", 13, 23, 23, finish_last_event, &result);
  const u32 pending_before = cpu_get_pending_ticks();
  timing_event_activate(&event);
  EXPECT_EQ(g_cpu_state.downcount, pending_before + 23u);
  timing_event_deactivate(&event);
  EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  timing_event_schedule(&event, 17);
  EXPECT_EQ(g_cpu_state.downcount, pending_before + 17u);
  timing_event_deactivate(&event);
  EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
  printf("timing-empty stage=deactivate first=%u rescheduled=%u idle=%u\n", pending_before + 23u, pending_before + 17u,
         g_cpu_state.downcount);
  timing_event_destroy(&event);
  fflush(stdout);
  finish_cpu();
}

TEST(TimingEventEmpty, legacy_empty_restore)
{
  const u32 versions[] = {42, 43, 70};
  for (u32 index = 0; index < 3; index++) {
    begin_empty_events();
    u8              bytes[32];
    state_wrapper_t state;
    state_wrapper_init_write(&state, bytes, sizeof(bytes), versions[index]);
    u32 global = 13, count = 0, old_last_event = 0;
    state_wrapper_do_u32(&state, &global);
    state_wrapper_do_u32(&state, &count);
    if (versions[index] < 43) state_wrapper_do_u32(&state, &old_last_event);
    EXPECT_FALSE(state_wrapper_has_error(&state));
    const size_t size = state_wrapper_get_position(&state);
    state_wrapper_init_read(&state, bytes, size, versions[index]);
    EXPECT_TRUE(timing_events_do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), size);
    EXPECT_EQ(timing_events_get_global_tick_counter(), 13);
    EXPECT_EQ(timing_events_get_event_run_tick_counter(), 13);
    EXPECT_EQ(g_cpu_state.downcount, UINT32_MAX);
    EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
    printf("timing-empty stage=legacy version=%u ticks=13 downcount=%u\n", versions[index], g_cpu_state.downcount);
    fflush(stdout);
    finish_cpu();
  }
}
