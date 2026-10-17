/* cupid-ps1 cache alias tests using generated RAM programs. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static const u32 alias_loop[] = {0x24080001, 0x01200008, 0, 0};

static void start_alias_loop(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 entry)
{
  start_cpu_with_fastmem(mode, fastmem, entry, alias_loop, 4);
  g_settings.cpu_recompiler_icache        = true;
  g_settings.cpu_recompiler_block_linking = linking;
  g_cpu_state.cache_control.bits          = 0x900;
  g_cpu_state.regs.r[9]                   = entry;
  cpu_clear_icache();
  cpu_set_pc(entry);
  run_slice(200);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
}

static void write_alias_instruction(u32 word)
{
  const u32                   address = 0xA0010000;
  bus_memory_write_handler_t* handlers =
      bus_offset_write_handler_array(bus_get_memory_handlers(false, false), MEMORY_ACCESS_SIZE_WORD);
  handlers[address >> BUS_MEMORY_LUT_PAGE_SHIFT](address, word);
  EXPECT_EQ(*(const u32*)(bus_get_unprotected_ram_pointer() + 0x10000), word);
}

static void expect_alias_progress(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, u64 before)
{
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > before);
  if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
}

static void cross_alias(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 first, u32 second,
                        bool keep_cached)
{
  start_alias_loop(mode, fastmem, linking, first);
  u32       cached[CPU_ICACHE_WORDS_PER_LINE];
  const u32 line = cpu_get_icache_line(first);
  memcpy(cached, g_cpu_state.icache_data + line * CPU_ICACHE_WORDS_PER_LINE, sizeof(cached));
  write_alias_instruction(0x24080002);
  g_cpu_state.regs.r[9] = second;
  u64 before            = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(200);
  EXPECT_EQ(g_cpu_state.regs.r[8], keep_cached ? 1 : 2);
  EXPECT_EQ(g_cpu_state.pc & 0xE0000000u, second & 0xE0000000u);
  expect_alias_progress(mode, fastmem, before);
  if (keep_cached) {
    EXPECT_EQ(memcmp(cached, g_cpu_state.icache_data + line * CPU_ICACHE_WORDS_PER_LINE, sizeof(cached)), 0);
    write_alias_instruction(0x24080003);
    g_cpu_state.regs.r[9] = first;
    run_slice(200);
    EXPECT_EQ(g_cpu_state.regs.r[8], 1);
    cpu_clear_icache();
    before = cpu_execution_stats_get()->native_blocks_completed;
    run_slice(200);
    EXPECT_EQ(g_cpu_state.regs.r[8], 3);
    expect_alias_progress(mode, fastmem, before);
  }
  finish_cpu();
}

#define ALIAS_CASES(name, first, second, keep)                                                                \
  TEST(InstructionCacheAliases, name##_interpreter)                                                           \
  {                                                                                                           \
    cross_alias(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, first, second, keep);        \
  }                                                                                                           \
  TEST(InstructionCacheAliases, name##_cached)                                                                \
  {                                                                                                           \
    cross_alias(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, first, second, keep); \
  }                                                                                                           \
  TEST(InstructionCacheAliases, name##_native)                                                                \
  {                                                                                                           \
    cross_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, first, second, keep);         \
  }                                                                                                           \
  TEST(InstructionCacheAliases, name##_mmap)                                                                  \
  {                                                                                                           \
    cross_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true, first, second, keep);             \
  }                                                                                                           \
  TEST(InstructionCacheAliases, name##_unlinked)                                                              \
  {                                                                                                           \
    cross_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false, first, second, keep);        \
  }

ALIAS_CASES(user_to_kernel_keeps_stale_code, 0x00010000, 0x80010000, true)
ALIAS_CASES(kernel_to_user_keeps_stale_code, 0x80010000, 0x00010000, true)
ALIAS_CASES(same_address_keeps_stale_code, 0x80010000, 0x80010000, true)
ALIAS_CASES(uncached_target_observes_ram, 0x80010000, 0xA0010000, false)
ALIAS_CASES(distinct_physical_mirror_refills, 0x80010000, 0x80210000, false)

static void restore_alias(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 version)
{
  const u32 first = 0x80010000;
  start_alias_loop(mode, fastmem, linking, first);
  const u32 line = cpu_get_icache_line(first);
  /* Older streams stored the cached virtual address in the internal tag. */
  g_cpu_state.icache_tags[line] = first | 8u;
  u8              buffer[16384];
  state_wrapper_t state;
  state_wrapper_init_write(&state, buffer, sizeof(buffer), version);
  EXPECT_TRUE(cpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t size = state_wrapper_get_position(&state);
  write_alias_instruction(0x24080002);
  cpu_clear_icache();
  state_wrapper_init_read(&state, buffer, size, version);
  EXPECT_TRUE(cpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(g_cpu_state.icache_tags[line], (version < 86 ? 0x10000 : first) | 8u);
  for (u32 word = 0; word < CPU_ICACHE_WORDS_PER_LINE; word++)
    EXPECT_EQ(g_cpu_state.icache_data[line * CPU_ICACHE_WORDS_PER_LINE + word], alias_loop[word]);
  cpu_code_cache_reset();
  g_cpu_state.regs.r[9] = 0x10000;
  const u64 before      = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(200);
  EXPECT_EQ(g_cpu_state.regs.r[8], version < 86 ? 1 : 2);
  expect_alias_progress(mode, fastmem, before);
  finish_cpu();
}

#define RESTORE_CASES(name, version)                                                                \
  TEST(InstructionCacheAliases, name##_interpreter)                                                 \
  {                                                                                                 \
    restore_alias(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, version);        \
  }                                                                                                 \
  TEST(InstructionCacheAliases, name##_cached)                                                      \
  {                                                                                                 \
    restore_alias(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, version); \
  }                                                                                                 \
  TEST(InstructionCacheAliases, name##_native)                                                      \
  {                                                                                                 \
    restore_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, version);         \
  }                                                                                                 \
  TEST(InstructionCacheAliases, name##_mmap)                                                        \
  {                                                                                                 \
    restore_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true, version);             \
  }                                                                                                 \
  TEST(InstructionCacheAliases, name##_unlinked)                                                    \
  {                                                                                                 \
    restore_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false, version);        \
  }

RESTORE_CASES(version84_migrates_virtual_tags, 84)
RESTORE_CASES(version85_migrates_virtual_tags, 85)
RESTORE_CASES(version86_preserves_full_tag_comparison, 86)

static void fault_at_alias(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 first, u32 second)
{
  const u32 words[4]   = {0x0000000C, 0, 0, 0};
  const u32 handler[2] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, fastmem, first, words, 4);
  g_settings.cpu_recompiler_icache        = true;
  g_settings.cpu_recompiler_block_linking = linking;
  g_cpu_state.cache_control.bits          = 0x900;
  g_cpu_state.cop0_regs.sr.BEV            = 0;
  g_cpu_state.cop0_regs.BadVaddr          = 0x12345678;
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  cpu_clear_icache();
  cpu_set_pc(first);
  write_alias_instruction(0);
  cpu_set_pc(second);
  const u64 before = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(200);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_SYSCALL);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 0);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, second);
  EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
  expect_alias_progress(mode, fastmem, before);
  finish_cpu();
}

#define FAULT_CASES(name, first, second)                                                                   \
  TEST(InstructionCacheAliases, name##_interpreter)                                                        \
  {                                                                                                        \
    fault_at_alias(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, first, second);        \
  }                                                                                                        \
  TEST(InstructionCacheAliases, name##_cached)                                                             \
  {                                                                                                        \
    fault_at_alias(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, first, second); \
  }                                                                                                        \
  TEST(InstructionCacheAliases, name##_native)                                                             \
  {                                                                                                        \
    fault_at_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, first, second);         \
  }                                                                                                        \
  TEST(InstructionCacheAliases, name##_mmap)                                                               \
  {                                                                                                        \
    fault_at_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true, first, second);             \
  }                                                                                                        \
  TEST(InstructionCacheAliases, name##_unlinked)                                                           \
  {                                                                                                        \
    fault_at_alias(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false, first, second);        \
  }

FAULT_CASES(user_to_kernel_fault_keeps_virtual_epc, 0x00010000, 0x80010000)
FAULT_CASES(kernel_to_user_fault_keeps_virtual_epc, 0x80010000, 0x00010000)
