/* cupid-ps1 instruction-cache tests using generated RAM programs. */
#include "tests/core/cpu_execution_fixture.h"

static const u32 cached_loop[] = {0x24080001, 0x1000FFFE, 0};

static void replace_ram_instruction(u32 address, u32 instruction)
{
  const u32 physical = address & bus_get_ram_mask();
  memcpy(bus_get_unprotected_ram_pointer() + physical, &instruction, sizeof(instruction));
  cpu_code_cache_invalidate_blocks_with_page_index(bus_get_ram_code_page_index(physical));
}

static void start_cached_loop(cpu_execution_mode_t mode, u32 entry)
{
  start_cpu(mode, entry, cached_loop, sizeof(cached_loop) / sizeof(cached_loop[0]));
  g_settings.cpu_recompiler_icache = true;
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
}

TEST(NativeInstructionCache, instruction_cache_is_enabled_by_default)
{
  settings_t settings;
  settings_init(&settings);
  EXPECT_TRUE(settings.cpu_recompiler_icache);
  settings_destroy(&settings);
}

TEST(NativeInstructionCache, ram_write_keeps_cached_code_until_explicit_invalidation)
{
  const u32 entry = 0x80010000;
  start_cached_loop(CPU_EXECUTION_MODE_RECOMPILER, entry);
  replace_ram_instruction(entry, 0x24080002);
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  cpu_clear_icache();
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 2);
  finish_cpu();
}

TEST(NativeInstructionCache, native_fill_retains_instruction_words)
{
  const u32 entry = 0x80010000;
  start_cached_loop(CPU_EXECUTION_MODE_RECOMPILER, entry);
  const u32 offset = cpu_get_icache_line(entry) * CPU_ICACHE_WORDS_PER_LINE;
  for (u32 word = 0; word < 3; word++)
    EXPECT_EQ(g_cpu_state.icache_data[offset + word], cached_loop[word]);
  finish_cpu();
}

TEST(NativeInstructionCache, partial_line_fill_keeps_prior_words_invalid)
{
  const u32 entry = 0x80010004;
  start_cached_loop(CPU_EXECUTION_MODE_RECOMPILER, entry);
  EXPECT_EQ(g_cpu_state.icache_tags[cpu_get_icache_line(entry)], cpu_get_icache_tag_for_address(entry) | 1u);
  const u32 offset = cpu_get_icache_line(entry) * CPU_ICACHE_WORDS_PER_LINE;
  EXPECT_EQ(g_cpu_state.icache_data[offset], 0);
  for (u32 word = 0; word < 3; word++)
    EXPECT_EQ(g_cpu_state.icache_data[offset + word + 1], cached_loop[word]);
  finish_cpu();
}

static void stale_code_crosses_mode_change(cpu_execution_mode_t from, cpu_execution_mode_t to)
{
  const u32 entry = 0x80010000;
  start_cached_loop(from, entry);
  replace_ram_instruction(entry, 0x24080002);
  g_settings.cpu_execution_mode = to;
  run_slice(to == CPU_EXECUTION_MODE_RECOMPILER ? 100 : 1);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  if (to == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  finish_cpu();
}

TEST(NativeInstructionCache, interpreter_to_native_keeps_cached_code)
{
  stale_code_crosses_mode_change(CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_RECOMPILER);
}

TEST(NativeInstructionCache, cached_to_native_keeps_cached_code)
{
  stale_code_crosses_mode_change(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_EXECUTION_MODE_RECOMPILER);
}

TEST(NativeInstructionCache, native_to_interpreter_keeps_cached_code)
{
  stale_code_crosses_mode_change(CPU_EXECUTION_MODE_RECOMPILER, CPU_EXECUTION_MODE_INTERPRETER);
}

TEST(NativeInstructionCache, native_to_cached_keeps_cached_code)
{
  stale_code_crosses_mode_change(CPU_EXECUTION_MODE_RECOMPILER, CPU_EXECUTION_MODE_CACHED_INTERPRETER);
}

TEST(NativeInstructionCache, uncached_execution_observes_ram_writes)
{
  const u32 entry = 0xA0010000;
  start_cached_loop(CPU_EXECUTION_MODE_RECOMPILER, entry);
  replace_ram_instruction(entry, 0x24080002);
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 2);
  finish_cpu();
}

static void store_before_later_line(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking,
                                    bool pending_load)
{
  u32 words[20] = {0x3C08A001, 0x40097800, 0, 0x01094021, 0x2508FFFE, 0xAD0A0040};
  words[6]      = 0x26310001;
  if (pending_load) {
    words[15] = 0x8E8B0000;
    words[17] = 0x25720001;
    words[18] = 0x1000FFFF;
  } else {
    words[17] = 0x1000FFFF;
  }
  words[16] = 0x24100001;
  start_cpu_with_fastmem(mode, fastmem, 0x80010000, words, 20);
  g_settings.cpu_recompiler_icache        = true;
  g_settings.cpu_recompiler_block_linking = linking;
  g_cpu_state.regs.r[10]                  = pending_load ? 0x25700002 : 0x24100002;
  g_cpu_state.regs.r[20]                  = 0xA0030000;
  const u32 loaded                        = 42;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &loaded, sizeof(loaded));
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[16], 2);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  if (pending_load) EXPECT_EQ(g_cpu_state.regs.r[18], 43);
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
  finish_cpu();
}

TEST(NativeInstructionCache, interpreter_store_precedes_later_line_fill)
{
  store_before_later_line(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, false);
}

TEST(NativeInstructionCache, native_store_precedes_later_line_fill)
{
  store_before_later_line(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, false);
}

TEST(NativeInstructionCache, mmap_store_precedes_later_line_fill)
{
  store_before_later_line(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true, false);
}

TEST(NativeInstructionCache, unlinked_store_precedes_later_line_fill)
{
  store_before_later_line(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false, false);
}

TEST(NativeInstructionCache, changed_line_keeps_pending_load)
{
  store_before_later_line(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, true);
}

static void changed_delay_slot(bool taken, bool trap, bool interrupt_before_slot)
{
  u32 words[24] = {0x3C08A001, 0x40097800, 0, 0x01094021, 0x2508FFFE, 0xAD0A0040, 0x26310001};
  words[15]     = 0x11800002;
  words[16]     = 0x24100001;
  words[17]     = 0x24130063;
  words[18]     = 0x24120003;
  words[19]     = 0x1000FFFF;
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x80010000, words, 24);
  const u32 handler[] = {0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_settings.cpu_recompiler_icache = true;
  g_cpu_state.cop0_regs.sr.BEV     = 0;
  g_cpu_state.regs.r[10]           = trap ? 0x4A000030 : 0x24100002;
  g_cpu_state.regs.r[12]           = taken ? 0 : 1;
  if (interrupt_before_slot) {
    run_slice(1);
    EXPECT_EQ(g_cpu_state.pc, 0x80010040);
    EXPECT_TRUE(g_cpu_state.next_instruction_is_branch_delay_slot);
  }
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  if (trap) {
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, taken);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x8001003C);
    EXPECT_EQ(g_cpu_state.cop0_regs.TAR, taken ? 0x80010048 : 0x80010044);
  } else {
    EXPECT_EQ(g_cpu_state.regs.r[16], 2);
    EXPECT_EQ(g_cpu_state.regs.r[18], 3);
    EXPECT_EQ(g_cpu_state.regs.r[19], taken ? 0 : 99);
  }
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
  EXPECT_TRUE(cpu_execution_stats_get()->interpreted_instructions > 0);
  finish_cpu();
}

TEST(NativeInstructionCache, changed_delay_slot_keeps_branch_outcome)
{
  changed_delay_slot(false, false, false);
  changed_delay_slot(true, false, false);
}

TEST(NativeInstructionCache, changed_delay_slot_exception_keeps_branch_context)
{
  changed_delay_slot(false, true, false);
  changed_delay_slot(true, true, false);
}

TEST(NativeInstructionCache, changed_delay_slot_resumes_after_event)
{
  changed_delay_slot(true, false, true);
}

TEST(NativeInstructionCache, restored_cpu_cache_keeps_stale_instruction_words)
{
  const u32 entry = 0x80010000;
  start_cached_loop(CPU_EXECUTION_MODE_RECOMPILER, entry);
  u8              buffer[16384];
  state_wrapper_t state;
  state_wrapper_init_write(&state, buffer, sizeof(buffer), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t size = state_wrapper_get_position(&state);
  replace_ram_instruction(entry, 0x24080002);
  cpu_clear_icache();
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 2);
  state_wrapper_init_read(&state, buffer, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  cpu_code_cache_reset();
  const u64 before = cpu_execution_stats_get()->native_blocks_completed;
  run_slice(100);
  EXPECT_EQ(g_cpu_state.regs.r[8], 1);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > before);
  finish_cpu();
}

static void changed_line_keeps_busy_deadline(bool gte)
{
  u32 words[24] = {0x3C08A001, 0x40097800, 0, 0x01094021, 0x2508FFFE, 0xAD0A0040};
  words[14]     = gte ? 0x4A000030 : 0x018D001A;
  words[16]     = 0x24100001;
  words[17]     = gte ? 0x480CC000 : 0x00009012;
  words[18]     = 0x1000FFFF;
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x80010000, words, 24);
  g_settings.cpu_recompiler_icache = true;
  g_cpu_state.cop0_regs.sr.CE2     = 1;
  g_cpu_state.regs.r[10]           = 0x24100002;
  g_cpu_state.regs.r[12]           = 12345;
  g_cpu_state.regs.r[13]           = 7;
  run_slice(1);
  EXPECT_EQ(g_cpu_state.pc, 0x80010040);
  EXPECT_TRUE((gte ? g_cpu_state.gte_completion_tick : g_cpu_state.muldiv_completion_tick) > g_cpu_state.pending_ticks);
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[16], 2);
  if (gte)
    EXPECT_EQ(g_cpu_state.regs.r[12], 0);
  else
    EXPECT_EQ(g_cpu_state.regs.r[18], 1763);
  finish_cpu();
}

TEST(NativeInstructionCache, changed_line_keeps_outstanding_divide_deadline)
{
  changed_line_keeps_busy_deadline(false);
}

TEST(NativeInstructionCache, changed_line_keeps_outstanding_gte_deadline)
{
  changed_line_keeps_busy_deadline(true);
}
