/* Generated programs exercise CPU dispatcher changes without BIOS or disc inputs. */
#include "tests/core/cpu/cpu_execution_fixture.h"

TEST(CpuExecutionTransition, cached_to_native_executes_native_blocks)
{
  const u32 words[] = {0x25080001, 0x1000FFFE, 0}; /* addiu t0,t0,1; branch to start; nop */
  start_cpu(CPU_EXECUTION_MODE_CACHED_INTERPRETER, 0xA0010000, words, 3);
  run_slice(100);
  EXPECT_TRUE(cpu_code_cache_get_cached_interpreter_stats().blocks_executed > 0);
  const u32 before              = g_cpu_state.regs.r[8];
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_RECOMPILER;
  run_slice(1000);
  EXPECT_TRUE(g_cpu_state.regs.r[8] > before);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  EXPECT_FALSE(g_cpu_state.using_interpreter);
  finish_cpu();
}

static void native_to_shared(cpu_execution_mode_t destination, bool restore_state)
{
  /* The prefix increments s2 once. A stale interpreter prefetch would re-enter it. */
  const u32 words[] = {0x3C08A003, 0x26520001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 4);
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[18], 1);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  g_settings.cpu_execution_mode = destination;
  if (restore_state) {
    u8              buffer[16384];
    state_wrapper_t writer, reader;
    state_wrapper_init_write(&writer, buffer, sizeof(buffer), SAVE_STATE_VERSION);
    EXPECT_TRUE(cpu_do_state(&writer));
    EXPECT_FALSE(state_wrapper_has_error(&writer));
    state_wrapper_init_read(&reader, buffer, state_wrapper_get_position(&writer), SAVE_STATE_VERSION);
    EXPECT_TRUE(cpu_do_state(&reader));
    EXPECT_FALSE(state_wrapper_has_error(&reader));
  }
  const u64 before = cpu_execution_stats_get()->interpreted_instructions;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[18], 1);
  EXPECT_TRUE(cpu_execution_stats_get()->interpreted_instructions > before);
  EXPECT_TRUE(g_cpu_state.using_interpreter);
  if (destination == CPU_EXECUTION_MODE_CACHED_INTERPRETER)
    EXPECT_TRUE(cpu_code_cache_get_cached_interpreter_stats().blocks_executed > 0);
  finish_cpu();
}

TEST(CpuExecutionTransition, native_to_interpreter_rebuilds_prefetch)
{
  native_to_shared(CPU_EXECUTION_MODE_INTERPRETER, false);
}

TEST(CpuExecutionTransition, native_to_cached_rebuilds_prefetch)
{
  native_to_shared(CPU_EXECUTION_MODE_CACHED_INTERPRETER, false);
}

TEST(CpuExecutionTransition, native_cpu_state_restores_into_interpreter)
{
  native_to_shared(CPU_EXECUTION_MODE_INTERPRETER, true);
}

static void branch_delay_to_native(cpu_execution_mode_t initial)
{
  const u32 words[] = {0x10000003, 0x25080001, 0x240A0001, 0, 0x24091234, 0x1000FFFF, 0};
  start_cpu(initial, 0xA0010000, words, 7);
  run_slice(1);
  EXPECT_TRUE(g_cpu_state.next_instruction_is_branch_delay_slot);
  EXPECT_EQ(g_cpu_state.pc, 0xA0010004);
  EXPECT_EQ(g_cpu_state.regs.r[8], 0);
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_RECOMPILER;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  EXPECT_EQ(g_cpu_state.regs.r[9], 0x1234);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  finish_cpu();
}

TEST(CpuExecutionTransition, interpreter_branch_delay_finishes_before_native_dispatch)
{
  branch_delay_to_native(CPU_EXECUTION_MODE_INTERPRETER);
}

TEST(CpuExecutionTransition, cached_branch_delay_finishes_before_native_dispatch)
{
  branch_delay_to_native(CPU_EXECUTION_MODE_CACHED_INTERPRETER);
}

TEST(CpuExecutionTransition, shared_pipeline_switch_preserves_instruction_cache_data)
{
  const u32 words[] = {0x24080001, 0x1000FFFE, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x80010000, words, 3);
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  const u32 changed = 0x24080002;
  memcpy(bus_get_unprotected_ram_pointer() + 0x10000, &changed, sizeof(changed));
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_CACHED_INTERPRETER;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  finish_cpu();
}

TEST(CpuExecutionTransition, pending_load_crosses_interpreter_to_native_switch)
{
  const u32 words[] = {0x3C08A003, 0x24091234, 0xAD090000, 0x8D0A0000, 0x254B0001, 0x254C0001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 8);
  for (u32 step = 0; step < 4; step++)
    run_slice(1);
  EXPECT_EQ(g_cpu_state.pc, 0xA0010010);
  EXPECT_EQ(g_cpu_state.load_delay_reg, CPU_REG_T2);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0);
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_RECOMPILER;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[11], 1);
  EXPECT_EQ(g_cpu_state.regs.r[12], 0x1235);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  finish_cpu();
}

TEST(CpuExecutionTransition, pending_load_crosses_native_to_interpreter_switch)
{
  const u32 words[] = {0x3C08A003, 0x2409002A, 0xAD090000, 0x10000002, 0x8D0A0000,
                       0x240B0001, 0x254C0001, 0x254D0001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 10);
  run_to_pc(0xA0010018);
  EXPECT_EQ(g_cpu_state.pc, 0xA0010018);
  EXPECT_EQ(g_cpu_state.load_delay_reg, CPU_REG_T2);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0);
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_INTERPRETER;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[11], 0);
  EXPECT_EQ(g_cpu_state.regs.r[12], 1);
  EXPECT_EQ(g_cpu_state.regs.r[13], 43);
  finish_cpu();
}

typedef struct {
  u8     cpu[16384];
  size_t cpu_size;
  u8     timing[256];
  size_t timing_size;
} saved_execution_t;

static void save_execution(saved_execution_t* saved)
{
  state_wrapper_t writer;
  state_wrapper_init_write(&writer, saved->cpu, sizeof(saved->cpu), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&writer));
  EXPECT_FALSE(state_wrapper_has_error(&writer));
  saved->cpu_size = state_wrapper_get_position(&writer);
  state_wrapper_init_write(&writer, saved->timing, sizeof(saved->timing), SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&writer));
  EXPECT_FALSE(state_wrapper_has_error(&writer));
  saved->timing_size = state_wrapper_get_position(&writer);
}

static void restore_execution(const saved_execution_t* saved)
{
  state_wrapper_t reader;
  state_wrapper_init_read(&reader, saved->cpu, saved->cpu_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&reader));
  EXPECT_FALSE(state_wrapper_has_error(&reader));
  state_wrapper_init_read(&reader, saved->timing, saved->timing_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&reader));
  EXPECT_FALSE(state_wrapper_has_error(&reader));
}

static void compare_continuation_after_restore(bool gte, cpu_execution_mode_t mode)
{
  const u32 divide[]    = {0x0109001A, 0x10000001, 0, 0x00005012, 0x1000FFFF, 0};
  const u32 transform[] = {0x4A000030, 0x10000001, 0, 0x4808C000, 0x1000FFFF, 0};
  start_cpu(mode, 0xA0010000, gte ? transform : divide, 6);
  g_cpu_state.cop0_regs.sr.CE2 = 1;
  g_cpu_state.regs.r[8]        = 12345;
  g_cpu_state.regs.r[9]        = 7;
  run_to_pc(0xA001000C);
  EXPECT_EQ(g_cpu_state.pc, 0xA001000C);
  EXPECT_TRUE((gte ? g_cpu_state.gte_completion_tick : g_cpu_state.muldiv_completion_tick) > g_cpu_state.pending_ticks);
  timing_event_t anchor;
  timing_event_init(&anchor, "continuation-anchor", 19, 100000, 100000, stop_cpu, NULL);
  timing_event_activate(&anchor);
  saved_execution_t before, uninterrupted, restored;
  save_execution(&before);
  run_slice(1);
  if (!gte) EXPECT_EQ(g_cpu_state.regs.r[10], 1763);
  save_execution(&uninterrupted);
  restore_execution(&before);
  run_slice(1);
  save_execution(&restored);
  EXPECT_EQ(uninterrupted.timing_size, restored.timing_size);
  EXPECT_EQ(memcmp(uninterrupted.timing, restored.timing, restored.timing_size), 0);
  EXPECT_EQ(uninterrupted.cpu_size, restored.cpu_size);
  EXPECT_EQ(memcmp(uninterrupted.cpu, restored.cpu, restored.cpu_size), 0);
  timing_event_deactivate(&anchor);
  timing_event_destroy(&anchor);
  finish_cpu();
}

TEST(CpuStateTiming, divide_stall_survives_save_and_restore)
{
  compare_continuation_after_restore(false, CPU_EXECUTION_MODE_INTERPRETER);
}

TEST(CpuStateTiming, gte_stall_survives_save_and_restore)
{
  compare_continuation_after_restore(true, CPU_EXECUTION_MODE_INTERPRETER);
}

static void native_gte_consumer_waits(u32 consumer)
{
  const u32 words[] = {0x4A000030, 0x10000001, 0, consumer, 0, 0x1000FFFF, 0};
  /* Cache hits keep the long GTE stall outstanding at the result-read boundary. */
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x80010000, words, 7);
  g_cpu_state.cop0_regs.sr.CE2 = 1;
  run_to_pc(0x8001000C);
  EXPECT_EQ(g_cpu_state.pc, 0x8001000C);
  const u32 remaining = g_cpu_state.gte_completion_tick;
  EXPECT_TRUE(remaining > 16);
  const global_ticks_t before = timing_events_get_event_run_tick_counter();
  const u64            blocks = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(1);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > blocks);
  EXPECT_TRUE(timing_events_get_event_run_tick_counter() - before >= remaining);
  finish_cpu();
}

TEST(CpuGteTiming, native_cross_block_result_read_waits_for_completion)
{
  native_gte_consumer_waits(0x4808C000);
}

TEST(CpuGteTiming, native_result_read_to_zero_still_waits)
{
  native_gte_consumer_waits(0x4800C000);
}

TEST(CpuGteTiming, native_control_read_to_zero_still_waits)
{
  native_gte_consumer_waits(0x4840F800);
}

TEST(CpuStateTiming, cached_divide_stall_survives_save_and_restore)
{
  compare_continuation_after_restore(false, CPU_EXECUTION_MODE_CACHED_INTERPRETER);
}

TEST(CpuStateTiming, cached_gte_stall_survives_save_and_restore)
{
  compare_continuation_after_restore(true, CPU_EXECUTION_MODE_CACHED_INTERPRETER);
}

TEST(CpuStateTiming, native_divide_stall_survives_save_and_restore)
{
  compare_continuation_after_restore(false, CPU_EXECUTION_MODE_RECOMPILER);
}

TEST(CpuStateTiming, native_gte_stall_survives_save_and_restore)
{
  compare_continuation_after_restore(true, CPU_EXECUTION_MODE_RECOMPILER);
}

typedef struct {
  u32            remaining;
  global_ticks_t global;
  global_ticks_t event;
} divide_deadline_t;

static divide_deadline_t divide_with_independent_gte(u32 operation, u32 padding)
{
  u32 words[64]    = {0};
  words[0]         = 0x0109001A;
  const u32 index  = padding + 1;
  words[index]     = operation;
  words[index + 1] = 0x10000001;
  words[index + 3] = 0x00005012;
  words[index + 4] = 0x1000FFFF;
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, index + 6);
  g_cpu_state.cop0_regs.sr.CE2 = 1;
  g_cpu_state.regs.r[8]        = 12345;
  g_cpu_state.regs.r[9]        = 7;
  run_to_pc(0xA0010000 + (index + 3) * 4);
  EXPECT_EQ(g_cpu_state.pc, 0xA0010000 + (index + 3) * 4);
  const divide_deadline_t result = {g_cpu_state.muldiv_completion_tick, timing_events_get_global_tick_counter(),
                                    timing_events_get_event_run_tick_counter()};
  finish_cpu();
  return result;
}

TEST(CpuGteTiming, native_gte_access_keeps_independent_divide_deadline)
{
  const u32 operations[] = {0x4A000030, 0x4800C000, 0x4840F800};
  const u32 padding[]    = {0, 40};
  for (u32 p = 0; p < 2; p++) {
    const divide_deadline_t baseline = divide_with_independent_gte(0, padding[p]);
    if (p == 0)
      EXPECT_TRUE(baseline.remaining > 0);
    else
      EXPECT_EQ(baseline.remaining, 0);
    for (u32 op = 0; op < 3; op++) {
      const divide_deadline_t actual = divide_with_independent_gte(operations[op], padding[p]);
      EXPECT_EQ(actual.global, baseline.global);
      EXPECT_EQ(actual.event, baseline.event);
      EXPECT_EQ(actual.remaining, baseline.remaining);
    }
  }
}

TEST(CpuExecutionTransition, native_core_restarts_after_shutdown)
{
  const u32 words[] = {0x24081234, 0x1000FFFF, 0};
  for (u32 boot = 0; boot < 2; boot++) {
    start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 3);
    run_slice(100);
    EXPECT_EQ(g_cpu_state.regs.r[8], 0x1234);
    EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
    finish_cpu();
  }
}

static void disabled_gte_instructions_trap(cpu_execution_mode_t mode)
{
  const u32 operations[] = {0x4808C000, 0x4848F800, 0x48880000, 0x48C80000, 0x4800C000, 0x4840F800, 0x4A000030};
  for (u32 delay = 0; delay < 2; delay++) {
    for (size_t op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
      u32 words[] = {operations[op], 0x26100001, 0x1000FFFF, 0, 0};
      if (delay) {
        words[0] = 0x10000002;
        words[1] = operations[op];
        words[2] = 0x26100001;
        words[3] = 0x1000FFFF;
      }
      start_cpu(mode, 0xA0010000, words, 5);
      const u32 handler[] = {0x1000FFFF, 0};
      memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
      g_cpu_state.cop0_regs.sr.BEV = 0;
      g_cpu_state.cop0_regs.sr.CE2 = 0;
      g_cpu_state.regs.r[8]        = 0x1234;
      g_cpu_state.load_delay_reg   = CPU_REG_T1;
      g_cpu_state.load_delay_value = 42;
      const cpu_gte_regs_t before  = g_cpu_state.gte_regs;
      run_to_pc(0x80000080);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, 2);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, delay);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, delay);
      EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0xA0010000);
      if (delay) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, 0xA001000C);
      EXPECT_EQ(g_cpu_state.regs.r[8], 0x1234);
      EXPECT_EQ(g_cpu_state.regs.r[9], 42);
      EXPECT_EQ(g_cpu_state.regs.r[16], 0);
      EXPECT_EQ(g_cpu_state.gte_completion_tick, 0);
      EXPECT_EQ(memcmp(&g_cpu_state.gte_regs, &before, sizeof(before)), 0);
      if (mode == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
      finish_cpu();
    }
  }
}

TEST(CpuCoprocessorException, interpreter_disabled_gte_traps_before_side_effects)
{
  disabled_gte_instructions_trap(CPU_EXECUTION_MODE_INTERPRETER);
}

TEST(CpuCoprocessorException, cached_disabled_gte_traps_before_side_effects)
{
  disabled_gte_instructions_trap(CPU_EXECUTION_MODE_CACHED_INTERPRETER);
}

TEST(CpuCoprocessorException, native_disabled_gte_traps_before_side_effects)
{
  disabled_gte_instructions_trap(CPU_EXECUTION_MODE_RECOMPILER);
}

TEST(CpuCoprocessorException, native_status_write_disables_gte_within_block)
{
  const u32 words[] = {0x40806000, 0x4A000030, 0x26100001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 5);
  const u32 handler[] = {0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.cop0_regs.sr.CE2 = 1;
  run_to_pc(0x80000080);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, 2);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0xA0010004);
  EXPECT_EQ(g_cpu_state.regs.r[16], 0);
  EXPECT_EQ(g_cpu_state.gte_completion_tick, 0);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  finish_cpu();
}

TEST(CpuCoprocessorException, native_gte_delay_trap_retains_branch_outcome)
{
  const u32 operations[] = {0x4808C000, 0x48880000, 0x4A000030};
  for (u32 taken = 0; taken < 2; taken++) {
    for (size_t op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
      const u32 words[] = {0x26310001, 0x11400002, operations[op], 0x26100001, 0x1000FFFF, 0};
      start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 6);
      const u32 handler[] = {0x1000FFFF, 0};
      memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
      g_cpu_state.cop0_regs.sr.BEV = 0;
      g_cpu_state.cop0_regs.sr.CE2 = 0;
      g_cpu_state.regs.r[10]       = taken ? 0 : 1;
      run_to_pc(0x80000080);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, 2);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 1);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, taken);
      EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0xA0010004);
      EXPECT_EQ(g_cpu_state.cop0_regs.TAR, taken ? 0xA0010010 : 0xA001000C);
      EXPECT_EQ(g_cpu_state.regs.r[16], 0);
      EXPECT_EQ(g_cpu_state.regs.r[17], 1);
      finish_cpu();
    }
  }
}

TEST(CpuCoprocessorException, native_disabled_gte_traps_before_busy_wait)
{
  const u32 words[] = {0x4800C000, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 3);
  const u32 handler[] = {0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.cop0_regs.sr.BEV    = 0;
  g_cpu_state.cop0_regs.sr.CE2    = 0;
  g_cpu_state.gte_completion_tick = 5000;
  run_slice(1);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0xA0010000);
  EXPECT_TRUE(timing_events_get_event_run_tick_counter() < 5000);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  finish_cpu();
}

TEST(CpuStateTiming, native_divide_deadline_survives_interpreter_fallback)
{
  /* TLBR is handled by the interpreter's reserved-instruction exception. */
  const u32 words[] = {0x0109001A, 0x42000001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000, words, 4);
  const u32 handler[] = {0x00005812, 0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.cop0_regs.sr.BEV = 0;
  g_cpu_state.regs.r[8]        = 12345;
  g_cpu_state.regs.r[9]        = 7;
  run_to_pc(0x80000080);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_RI);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0xA0010004);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_entered > 0);
  EXPECT_TRUE(cpu_execution_stats_get()->interpreted_instructions > 0);
  EXPECT_TRUE(g_cpu_state.muldiv_completion_tick > g_cpu_state.pending_ticks);
  const u32            remaining = g_cpu_state.muldiv_completion_tick - g_cpu_state.pending_ticks;
  const global_ticks_t before    = timing_events_get_event_run_tick_counter();
  run_slice(1);
  EXPECT_EQ(g_cpu_state.regs.r[11], 1763);
  EXPECT_TRUE(timing_events_get_event_run_tick_counter() - before >= remaining);
  finish_cpu();
}
