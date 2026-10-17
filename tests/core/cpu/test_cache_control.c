/* cupid-ps1 cache-control tests using generated RAM programs. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void control_write(u32 value)
{
  const u32                   address = 0xFFFE0130;
  bus_memory_write_handler_t* handlers =
      bus_offset_write_handler_array(bus_get_memory_handlers(false, false), MEMORY_ACCESS_SIZE_WORD);
  handlers[address >> BUS_MEMORY_LUT_PAGE_SHIFT](address, value);
  EXPECT_EQ(g_cpu_state.cache_control.bits, value);
}

static void code_write(u32 address, u32 value)
{
  address = (address & 0x1FFFFFFFu) | 0xA0000000u;
  bus_memory_write_handler_t* handlers =
      bus_offset_write_handler_array(bus_get_memory_handlers(false, false), MEMORY_ACCESS_SIZE_WORD);
  handlers[address >> BUS_MEMORY_LUT_PAGE_SHIFT](address, value);
}

static void control_progress(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, u64 before)
{
  if (mode == CPU_EXECUTION_MODE_RECOMPILER && cpu_execution_stats_get()->native_blocks_completed < before + 10)
    test_fail(__FILE__, __LINE__, "native progress before=%llu after=%llu interpreted=%llu control=%08X pc=%08X",
              (unsigned long long)before, (unsigned long long)cpu_execution_stats_get()->native_blocks_completed,
              (unsigned long long)cpu_execution_stats_get()->interpreted_instructions, g_cpu_state.cache_control.bits,
              g_cpu_state.pc);
  if (fastmem != CPU_FASTMEM_MODE_DISABLED) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
}

static void control_loop(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 scenario)
{
  const u32 entry    = 0x80010000;
  const u32 words[4] = {0x24080001, 0x01200008, 0, 0};
  start_cpu_with_fastmem(mode, fastmem, entry, words, 4);
  g_settings.cpu_recompiler_icache        = true;
  g_settings.cpu_recompiler_block_linking = linking;
  g_cpu_state.regs.r[9]                   = entry;
  control_write(scenario == 1 ? 0x100 : 0x900);
  cpu_clear_icache();
  cpu_set_pc(entry);
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  if (scenario == 1) {
    for (u32 line = 0; line < CPU_ICACHE_LINES; line++)
      EXPECT_EQ(g_cpu_state.icache_tags[line], CPU_ICACHE_INVALID_BITS);
    for (u32 word = 0; word < CPU_ICACHE_SLOTS; word++)
      EXPECT_EQ(g_cpu_state.icache_data[word], 0);
    code_write(entry, 0x24080002);
    control_write(0x900);
    const u64 before = cpu_execution_stats_get()->native_blocks_completed;
    run_slice(1000);
    EXPECT_EQ(g_cpu_state.regs.r[8], 2);
    control_progress(mode, fastmem, before);
  } else {
    const u32 line = cpu_get_icache_line(entry);
    const u32 tag  = g_cpu_state.icache_tags[line];
    u32       data[CPU_ICACHE_WORDS_PER_LINE];
    memcpy(data, g_cpu_state.icache_data + line * CPU_ICACHE_WORDS_PER_LINE, sizeof(data));
    code_write(entry, 0x24080002);
    control_write(0x100);
    u64 before = cpu_execution_stats_get()->native_blocks_completed;
    run_slice(1000);
    EXPECT_EQ(g_cpu_state.regs.r[8], 2);
    control_progress(mode, fastmem, before);
    EXPECT_EQ(g_cpu_state.icache_tags[line], tag);
    EXPECT_EQ(memcmp(data, g_cpu_state.icache_data + line * CPU_ICACHE_WORDS_PER_LINE, sizeof(data)), 0);
    if (scenario == 2) {
      u8              buffer[16384];
      state_wrapper_t state;
      state_wrapper_init_write(&state, buffer, sizeof(buffer), SAVE_STATE_VERSION);
      EXPECT_TRUE(cpu_do_state(&state));
      EXPECT_FALSE(state_wrapper_has_error(&state));
      const size_t size = state_wrapper_get_position(&state);
      code_write(entry, 0x24080003);
      state_wrapper_init_read(&state, buffer, size, SAVE_STATE_VERSION);
      EXPECT_TRUE(cpu_do_state(&state));
      EXPECT_FALSE(state_wrapper_has_error(&state));
      cpu_code_cache_reset();
      before = cpu_execution_stats_get()->native_blocks_completed;
      run_slice(1000);
      EXPECT_EQ(g_cpu_state.regs.r[8], 3);
      control_progress(mode, fastmem, before);
      EXPECT_EQ(g_cpu_state.cache_control.bits, 0x100);
    }
    control_write(0x900);
    before = cpu_execution_stats_get()->native_blocks_completed;
    run_slice(1000);
    EXPECT_EQ(g_cpu_state.regs.r[8], 1);
    control_progress(mode, fastmem, before);
  }
  finish_cpu();
}

static void guest_control_write(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 scenario)
{
  const bool enable   = scenario == 4 || scenario == 6;
  const bool uncached = scenario >= 5;
  const u32  entry    = uncached ? 0xA0010000 : 0x80010000;
  const u32  words[5] = {0xAD090000, 0x24100001, 0x24110001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, fastmem, entry, words, 5);
  g_settings.cpu_recompiler_icache        = true;
  g_settings.cpu_recompiler_block_linking = linking;
  control_write(0x900);
  cpu_clear_icache();
  cpu_set_pc(entry);
  code_write(entry + 4, 0x24100002);
  code_write(entry + 8, 0x24110002);
  if (enable) control_write(0x100);
  cpu_set_pc(entry);
  g_cpu_state.regs.r[8] = 0xFFFE0130;
  g_cpu_state.regs.r[9] = enable ? 0x900 : 0x100;
  const u64 before      = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[16], enable || uncached ? 2 : 1);
  EXPECT_EQ(g_cpu_state.regs.r[17], enable && !uncached ? 1 : 2);
  EXPECT_EQ(g_cpu_state.cache_control.bits, enable ? 0x900 : 0x100);
  control_progress(mode, fastmem, before);
  finish_cpu();
}

static void cache_control_case(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 scenario)
{
  if (scenario < 3)
    control_loop(mode, fastmem, linking, scenario);
  else
    guest_control_write(mode, fastmem, linking, scenario);
}

#define CONTROL_CASES(name, scenario)                                                                     \
  TEST(CacheControl, name##_interpreter)                                                                  \
  {                                                                                                       \
    cache_control_case(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, scenario);        \
  }                                                                                                       \
  TEST(CacheControl, name##_cached)                                                                       \
  {                                                                                                       \
    cache_control_case(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, scenario); \
  }                                                                                                       \
  TEST(CacheControl, name##_native)                                                                       \
  {                                                                                                       \
    cache_control_case(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, scenario);         \
  }                                                                                                       \
  TEST(CacheControl, name##_lut)                                                                          \
  {                                                                                                       \
    cache_control_case(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true, scenario);              \
  }                                                                                                       \
  TEST(CacheControl, name##_mmap)                                                                         \
  {                                                                                                       \
    cache_control_case(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true, scenario);             \
  }                                                                                                       \
  TEST(CacheControl, name##_unlinked)                                                                     \
  {                                                                                                       \
    cache_control_case(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false, scenario);        \
  }

CONTROL_CASES(disable_bypasses_and_reenable_keeps_words, 0)
CONTROL_CASES(disabled_misses_do_not_fill, 1)
CONTROL_CASES(disabled_state_restores_cache_and_control, 2)
CONTROL_CASES(guest_disable_preserves_only_prefetched_word, 3)
CONTROL_CASES(guest_enable_preserves_only_prefetched_word, 4)
CONTROL_CASES(uncached_guest_disable_keeps_bypassing, 5)
CONTROL_CASES(uncached_guest_enable_keeps_bypassing, 6)
