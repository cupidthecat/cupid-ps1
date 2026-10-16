/* cupid-ps1 generated R3000A programs for interpreter/cache execution checks. */
#include "tests/test_harness.h"
#include "common/error.h"
#include "core/memory/bus.h"
#include "core/cpu/cpu_code_cache.h"
#include "core/cpu/cpu_core.h"
#include "core/cpu/cpu_core_private.h"
#include "core/cpu/cpu_execution_stats.h"
#include "core/config/settings.h"
#include "core/scheduler/timing_event.h"

typedef struct {
  cpu_state_t                    checkpoints[4];
  global_ticks_t                 global_ticks[4];
  global_ticks_t                 event_ticks[4];
  u8*                            ram;
  u32                            ram_size;
  cpu_cached_interpreter_stats_t stats;
  cpu_execution_stats_t          execution;
} execution_result_t;

static u32 imm(u32 op, u32 rs, u32 rt, s32 value)
{
  return (op << 26) | (rs << 21) | (rt << 16) | ((u32)value & 0xFFFFu);
}

static void stop_execution(void* user, tick_count_t ticks, tick_count_t late)
{
  (void)user;
  (void)ticks;
  (void)late;
  timing_events_cancel_running_event();
  cpu_exit_execution();
}

static execution_result_t* run_program(cpu_execution_mode_t mode, u32 start, const u32* words, size_t count, bool split)
{
  execution_result_t* result = calloc(1, sizeof(*result));
  EXPECT_NOT_NULL(result);
  memset(&g_cpu_state, 0, sizeof(g_cpu_state));
  settings_init(&g_settings);
  g_settings.cpu_execution_mode = mode;
  g_settings.cpu_fastmem_mode   = CPU_FASTMEM_MODE_DISABLED;
  g_settings.gpu_pgxp_enable    = false;
  timing_events_initialize();
  cpu_execution_stats_initialize(true);

  Error error;
  Error_init(&error);
  EXPECT_TRUE(bus_allocate_memory(false, &error));
  bus_initialize();
  bus_reset();
  memset(bus_get_bios_pointer(), 0, BUS_BIOS_SIZE);
  for (size_t i = 0; i < count; i++) {
    const u32 address     = (start + (u32)i * 4u) & CPU_KSEG_MASK;
    u8*       destination = (address >= BUS_BIOS_BASE && address < BUS_BIOS_BASE + BUS_BIOS_SIZE)
                                ? bus_get_bios_pointer() + (address & BUS_BIOS_MASK)
                                : bus_get_unprotected_ram_pointer() + (address & bus_get_ram_mask());
    memcpy(destination, &words[i], sizeof(words[i]));
  }
  /* Exception handler: capture Cause/EPC after their MFC0 load delays. */
  const u32 handler[] = {0x401A6800u, 0, 0x401B7000u, 0, 0x1000FFFFu, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80u, handler, sizeof(handler));

  cpu_initialize();
  cpu_reset();
  EXPECT_TRUE(cpu_code_cache_process_startup(&error));
  cpu_code_cache_reset();
  cpu_set_pc(start);
  Error_destroy(&error);

  const tick_count_t slices[] = {7, 7, 7, 1000};
  const u32          phases   = split ? 4u : 1u;
  for (u32 phase = 0; phase < phases; phase++) {
    timing_event_t     event;
    const tick_count_t duration = split ? slices[phase] : 1000;
    timing_event_init(&event, "test-stop", 9, duration, duration, stop_execution, NULL);
    timing_event_activate(&event);
    cpu_execute();
    timing_event_deactivate(&event);
    timing_event_destroy(&event);
    result->checkpoints[phase]  = g_cpu_state;
    result->global_ticks[phase] = timing_events_get_global_tick_counter();
    result->event_ticks[phase]  = timing_events_get_event_run_tick_counter();
    /* Host mapping addresses are not emulated CPU state. */
    result->checkpoints[phase].memory_handlers = NULL;
    result->checkpoints[phase].fastmem_base    = NULL;
  }
  result->execution = *cpu_execution_stats_get();
  result->stats     = cpu_code_cache_get_cached_interpreter_stats();
  result->ram_size  = bus_get_ram_size();
  result->ram       = malloc(result->ram_size);
  EXPECT_NOT_NULL(result->ram);
  memcpy(result->ram, bus_get_unprotected_ram_pointer(), result->ram_size);

  cpu_code_cache_shutdown();
  cpu_code_cache_process_shutdown();
  cpu_shutdown();
  timing_events_shutdown();
  bus_shutdown();
  bus_release_memory();
  settings_destroy(&g_settings);
  return result;
}

static void release_result(execution_result_t* result)
{
  free(result->ram);
  free(result);
}

static execution_result_t* compare_program(u32 start, const u32* words, size_t count, bool split)
{
  execution_result_t* interpreter = run_program(CPU_EXECUTION_MODE_INTERPRETER, start, words, count, split);
  execution_result_t* cached      = run_program(CPU_EXECUTION_MODE_CACHED_INTERPRETER, start, words, count, split);
  EXPECT_EQ(interpreter->stats.blocks_executed, 0);
  EXPECT_TRUE(cached->stats.blocks_created > 0);
  EXPECT_TRUE(cached->stats.blocks_executed > 0);
  EXPECT_TRUE(cached->stats.block_hits > 0);
  EXPECT_TRUE(cached->stats.instructions_executed > 0);
  const u8* expected = (const u8*)interpreter->checkpoints;
  const u8* actual   = (const u8*)cached->checkpoints;
  for (size_t i = 0; i < sizeof(cached->checkpoints); i++) {
    if (expected[i] != actual[i])
      test_fail(__FILE__, __LINE__, "CPU checkpoint byte %zu differs: interp=%u cached=%u", i, expected[i], actual[i]);
  }
  EXPECT_EQ(interpreter->ram_size, cached->ram_size);
  EXPECT_EQ(memcmp(interpreter->global_ticks, cached->global_ticks, sizeof(cached->global_ticks)), 0);
  EXPECT_EQ(memcmp(interpreter->event_ticks, cached->event_ticks, sizeof(cached->event_ticks)), 0);
  EXPECT_EQ(memcmp(interpreter->ram, cached->ram, cached->ram_size), 0);
  release_result(interpreter);
  return cached;
}

TEST(CachedInterpreter, loops_reuse_blocks_and_keep_native_ticks)
{
  const u32 words[] = {
      imm(9, 8, 8, 1), imm(10, 8, 9, 4), imm(5, 9, 0, -3), imm(9, 10, 10, 1), imm(9, 0, 11, 0x1234), 0x1000FFFFu, 0,
  };
  execution_result_t* result = compare_program(0xA0010000u, words, sizeof(words) / sizeof(*words), false);
  EXPECT_EQ(result->checkpoints[0].regs.r[8], 4);
  EXPECT_EQ(result->checkpoints[0].regs.r[10], 4);
  EXPECT_EQ(result->checkpoints[0].regs.r[11], 0x1234);
  release_result(result);
}

TEST(CachedInterpreter, event_slices_preserve_partially_executed_blocks)
{
  const u32 words[] = {
      imm(9, 0, 8, 1), imm(9, 8, 9, 2), imm(9, 9, 10, 3), imm(9, 10, 11, 4), imm(9, 11, 12, 5), 0x1000FFFFu, 0,
  };
  execution_result_t* result = compare_program(0x80010000u, words, sizeof(words) / sizeof(*words), true);
  EXPECT_EQ(result->checkpoints[3].regs.r[12], 15);
  release_result(result);
}

TEST(CachedInterpreter, load_delay_and_destination_overwrite)
{
  const u32 words[] = {
      imm(15, 0, 8, 0xA003), imm(9, 0, 9, 0x1234), imm(43, 8, 9, 0),
      imm(35, 8, 10, 0),     imm(9, 10, 11, 1),    imm(9, 10, 12, 1),
      imm(35, 8, 10, 0),     imm(9, 0, 10, 7),     0,
      imm(9, 10, 13, 0),     0x1000FFFFu,          0,
  };
  execution_result_t* result = compare_program(0xA0010000u, words, sizeof(words) / sizeof(*words), false);
  EXPECT_EQ(result->checkpoints[0].regs.r[11], 1);
  EXPECT_EQ(result->checkpoints[0].regs.r[12], 0x1235);
  EXPECT_EQ(result->checkpoints[0].regs.r[13], 7);
  release_result(result);
}

TEST(CachedInterpreter, load_in_branch_delay_crosses_block_boundary)
{
  const u32 words[] = {
      imm(15, 0, 8, 0xA003), imm(9, 0, 9, 42),  imm(43, 8, 9, 0),  imm(4, 0, 0, 2), imm(35, 8, 10, 0),
      imm(9, 0, 11, 99),     imm(9, 10, 12, 1), imm(9, 10, 13, 1), 0x1000FFFFu,     0,
  };
  execution_result_t* result = compare_program(0xA0010000u, words, sizeof(words) / sizeof(*words), false);
  EXPECT_EQ(result->checkpoints[0].regs.r[11], 0);
  EXPECT_EQ(result->checkpoints[0].regs.r[12], 1);
  EXPECT_EQ(result->checkpoints[0].regs.r[13], 43);
  release_result(result);
}

TEST(CachedInterpreter, overflow_in_branch_delay_reports_bd_and_epc)
{
  const u32 words[] = {
      imm(15, 0, 8, 0x7FFF), imm(13, 8, 8, 0xFFFF), imm(4, 0, 0, 2), imm(8, 8, 9, 1), 0, 0x1000FFFFu, 0,
  };
  execution_result_t* result = compare_program(0xA0010000u, words, sizeof(words) / sizeof(*words), false);
  EXPECT_EQ(result->checkpoints[0].regs.r[26] & 0x8000007Cu, 0x80000030u);
  EXPECT_EQ(result->checkpoints[0].regs.r[27], 0xA0010008u);
  release_result(result);
}

TEST(CachedInterpreter, misaligned_load_reports_bad_address)
{
  const u32           words[] = {imm(15, 0, 8, 0xA003), imm(35, 8, 9, 1), 0x1000FFFFu, 0};
  execution_result_t* result  = compare_program(0x80010000u, words, sizeof(words) / sizeof(*words), false);
  EXPECT_EQ(result->checkpoints[0].cop0_regs.BadVaddr, 0xA0030001u);
  EXPECT_EQ(result->checkpoints[0].regs.r[26] & 0x7Cu, 0x10u);
  EXPECT_EQ(result->checkpoints[0].regs.r[27], 0x80010004u);
  release_result(result);
}

static void check_modified_code(u32 start, u32 expected_second_value)
{
  u32 words[19] = {
      imm(15, 0, 16, (s32)(start >> 16)),
      imm(13, 16, 16, 0x40),
      0x0200F809u,
      0,
      imm(9, 8, 17, 0),
      imm(15, 0, 18, 0x2408),
      imm(13, 18, 18, 9),
      imm(43, 16, 18, 0),
      0x0200F809u,
      0,
      imm(9, 8, 19, 0),
      0x1000FFFFu,
      0,
  };
  words[16]                  = imm(9, 0, 8, 1);
  words[17]                  = 0x03E00008u;
  words[18]                  = 0;
  execution_result_t* result = compare_program(start, words, 19, false);
  EXPECT_EQ(result->checkpoints[0].regs.r[17], 1);
  EXPECT_EQ(result->checkpoints[0].regs.r[19], expected_second_value);
  u32 stored;
  memcpy(&stored, result->ram + 0x10040u, sizeof(stored));
  EXPECT_EQ(stored, imm(9, 0, 8, 9));
  release_result(result);
}

TEST(CachedInterpreter, uncached_self_modifying_code_invalidates_blocks)
{
  check_modified_code(0xA0010000u, 9);
}

TEST(CachedInterpreter, instruction_cache_keeps_old_code_until_refetched)
{
  check_modified_code(0x80010000u, 1);
}

TEST(CachedInterpreter, branch_delay_wraps_at_end_of_physical_ram)
{
  const u32           words[] = {0x1000FFFFu, 0};
  execution_result_t* result  = compare_program(0xA01FFFFCu, words, 2, false);
  EXPECT_EQ(result->checkpoints[0].cop0_regs.cause.bits, 0);
  release_result(result);
}

TEST(CachedInterpreter, reserved_instruction_fallback_raises_exception)
{
  const u32           words[] = {0xFC000000u, 0x1000FFFFu, 0};
  execution_result_t* result  = compare_program(0xA0010000u, words, 3, false);
  EXPECT_EQ(result->checkpoints[0].regs.r[26] & 0x7Cu, 0x28u);
  EXPECT_EQ(result->checkpoints[0].regs.r[27], 0xA0010000u);
  EXPECT_TRUE(result->stats.fallback_instructions > 0);
  release_result(result);
}

TEST(CachedInterpreter, failed_prefetch_preserves_event_and_exception_boundary)
{
  const u32           words[] = {imm(9, 0, 8, 0x1234)};
  execution_result_t* result  = compare_program(0xBFC7FFFCu, words, 1, true);
  EXPECT_EQ(result->checkpoints[3].cop0_regs.cause.bits & 0x7Cu, 0x18u);
  release_result(result);
}

TEST(ExecutionProgress, native_linked_blocks_and_scheduler_execute)
{
  const u32           words[] = {imm(9, 0, 8, 0x1234), imm(9, 0, 9, 0x5678), 0x1000FFFFu, 0};
  execution_result_t* result =
      run_program(CPU_EXECUTION_MODE_RECOMPILER, 0xA0010000u, words, sizeof(words) / sizeof(*words), false);
  EXPECT_EQ(result->checkpoints[0].regs.r[8], 0x1234);
  EXPECT_EQ(result->checkpoints[0].regs.r[9], 0x5678);
  EXPECT_TRUE(result->execution.native_blocks_entered > 10);
  EXPECT_TRUE(result->execution.native_blocks_completed > 10);
  EXPECT_EQ(result->execution.interpreted_instructions, 0);
  EXPECT_TRUE(result->global_ticks[0] >= 1000);
  /* Event and global counters use distinct commit points. Check progress here;
   * independent checkpoint comparisons retain both timing domains. */
  EXPECT_TRUE(result->event_ticks[0] >= 1000);
  release_result(result);
}
