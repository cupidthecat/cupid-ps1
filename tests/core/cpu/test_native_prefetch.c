/* Native pipeline serialization and instruction-boundary regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void restore_native_prefetch(u32 version, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 entry   = 0xA0010000;
  const u32 words[] = {0x26100001, 0x26310001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, fastmem, entry, words, 4);
  g_settings.cpu_recompiler_block_linking = linking;
  run_slice(100);
  EXPECT_FALSE(g_cpu_state.using_interpreter);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  g_cpu_state.regs.r[16] = 0;
  g_cpu_state.regs.r[17] = 0;
  cpu_set_pc(entry);
  EXPECT_EQ(g_cpu_state.next_instruction.bits, words[0]);
  g_cpu_state.load_delay_reg         = CPU_REG_T2;
  g_cpu_state.load_delay_value       = 42;
  g_cpu_state.gte_completion_tick    = g_cpu_state.pending_ticks + 17;
  g_cpu_state.muldiv_completion_tick = g_cpu_state.pending_ticks + 29;
  const u32       gte_deadline       = g_cpu_state.gte_completion_tick;
  const u32       divide_deadline    = g_cpu_state.muldiv_completion_tick;
  u8              buffer[16384];
  state_wrapper_t state;
  state_wrapper_init_write(&state, buffer, sizeof(buffer), version);
  EXPECT_TRUE(cpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t size = state_wrapper_get_position(&state);

  /* RAM changes after the saved fetch. New states must retain the latch;
   * old native streams require a fetch from their saved execution PC. */
  const u32 replacement = 0x26100002;
  memcpy(bus_get_unprotected_ram_pointer() + 0x10000, &replacement, sizeof(replacement));
  cpu_code_cache_reset();
  cpu_set_pc(entry + 8);
  g_cpu_state.load_delay_reg         = CPU_REG_COUNT;
  g_cpu_state.gte_completion_tick    = 0;
  g_cpu_state.muldiv_completion_tick = 0;
  state_wrapper_init_read(&state, buffer, size, version);
  EXPECT_TRUE(cpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(g_cpu_state.pc, entry);
  EXPECT_EQ(g_cpu_state.load_delay_reg, CPU_REG_T2);
  EXPECT_EQ(g_cpu_state.load_delay_value, 42);
  EXPECT_EQ(g_cpu_state.gte_completion_tick, gte_deadline);
  EXPECT_EQ(g_cpu_state.muldiv_completion_tick, divide_deadline);
  const u64 before = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[16], version >= 87 ? 1 : 2);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  EXPECT_EQ(g_cpu_state.regs.r[10], 42);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= before + 10);
  if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
  finish_cpu();
}

#define PREFETCH_STATE_CASES(name, version)                             \
  TEST(NativePrefetch, name##_slow)                                     \
  {                                                                     \
    restore_native_prefetch(version, CPU_FASTMEM_MODE_DISABLED, true);  \
  }                                                                     \
  TEST(NativePrefetch, name##_mmap)                                     \
  {                                                                     \
    restore_native_prefetch(version, CPU_FASTMEM_MODE_MMAP, true);      \
  }                                                                     \
  TEST(NativePrefetch, name##_unlinked)                                 \
  {                                                                     \
    restore_native_prefetch(version, CPU_FASTMEM_MODE_DISABLED, false); \
  }

PREFETCH_STATE_CASES(version84_rebuilds_legacy_native_fetch, 84)
PREFETCH_STATE_CASES(version85_rebuilds_legacy_native_fetch, 85)
PREFETCH_STATE_CASES(version86_rebuilds_legacy_native_fetch, 86)
PREFETCH_STATE_CASES(version87_retains_fetched_word, 87)

static void long_native_block(cpu_fastmem_mode_t fastmem, bool linking)
{
  /* A full RAM page exercises near/far guard storage and page-boundary exits. */
  u32 words[1026];
  for (u32 i = 0; i < 1024; i++)
    words[i] = 0x26100001;
  words[1024] = 0x1000FFFF;
  words[1025] = 0;
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, fastmem, 0xA0010000, words, 1026);
  g_settings.cpu_recompiler_block_linking = linking;
  run_slice(20000);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1024);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
  finish_cpu();
}

TEST(NativePrefetch, long_block_slow)
{
  long_native_block(CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativePrefetch, long_block_mmap)
{
  long_native_block(CPU_FASTMEM_MODE_MMAP, true);
}
TEST(NativePrefetch, long_block_unlinked)
{
  long_native_block(CPU_FASTMEM_MODE_DISABLED, false);
}
