/* cupid-ps1 native LUT execution regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/gte/gte.h"
#include "util/platform/page_fault_handler.h"

static void check_lut_widths(bool linking)
{
  const u32 aliases[] = {0,          0x00200000, 0x00400000, 0x00600000, 0x80000000, 0x80200000,
                         0x80400000, 0x80600000, 0xA0000000, 0xA0200000, 0xA0400000, 0xA0600000};
  for (u32 constant = 0; constant < 2; constant++) {
    for (u32 i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
      const u32 address = aliases[i] + 0x30000;
      u32       words[16];
      u32       count = 0;
      if (constant) {
        words[count++] = 0x3C080000 | (address >> 16);
        words[count++] = 0x35080000 | (address & 0xFFFF);
      }
      const u32 accesses[] = {0xA1090000, 0xA5090002, 0xAD090004, 0x810A0000, 0x910B0000, 0x850C0002,
                              0x950D0002, 0x8D0E0004, 0,          0x26100001, 0x1000FFFF, 0};
      memcpy(words + count, accesses, sizeof(accesses));
      count += sizeof(accesses) / sizeof(accesses[0]);
      start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, count);
      g_settings.cpu_recompiler_block_linking     = linking;
      g_settings.cpu_recompiler_memory_exceptions = false;
      g_cpu_state.regs.r[8]                       = address;
      g_cpu_state.regs.r[9]                       = 0x89ABCDEF;
      memset(bus_get_unprotected_ram_pointer() + 0x30000, 0x55, 8);
      EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
      run_slice(1000);
      EXPECT_EQ(g_cpu_state.regs.r[10], 0xFFFFFFEF);
      EXPECT_EQ(g_cpu_state.regs.r[11], 0xEF);
      EXPECT_EQ(g_cpu_state.regs.r[12], 0xFFFFCDEF);
      EXPECT_EQ(g_cpu_state.regs.r[13], 0xCDEF);
      EXPECT_EQ(g_cpu_state.regs.r[14], 0x89ABCDEF);
      EXPECT_EQ(g_cpu_state.regs.r[16], 1);
      u32 memory[2];
      memcpy(memory, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(memory));
      EXPECT_EQ(memory[0], 0xCDEF55EF);
      EXPECT_EQ(memory[1], 0x89ABCDEF);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
      EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 5);
      EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 3);
      finish_cpu();
    }
  }
}

TEST(NativeLut, widths_and_signs_through_all_ram_aliases)
{
  check_lut_widths(true);
}

TEST(NativeLut, unlinked_widths_and_signs)
{
  check_lut_widths(false);
}

TEST(NativeLut, mmio_recovery_does_not_count_as_a_direct_access)
{
  const u32 words[] = {0x8DE80000, 0, 0xAD090000, 0x8D0A0000, 0, 0x26100001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 8);
  dma_initialize();
  dma_reset();
  g_settings.cpu_recompiler_memory_exceptions = false;
  g_cpu_state.regs.r[15]                      = 0xA0030000;
  g_cpu_state.regs.r[9]                       = 0x01234567;
  const u32 pointer                           = 0x1F8010F0;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &pointer, sizeof(pointer));
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0x01234567);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  EXPECT_TRUE(cpu_code_cache_has_previously_faulted_on_pc(0xA0010008));
  EXPECT_TRUE(cpu_code_cache_has_previously_faulted_on_pc(0xA001000C));
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 1);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 0);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  finish_cpu();
}

static void check_lut_code_write(bool cached)
{
  const u32 entry   = cached ? 0x80010000 : 0xA0010000;
  const u32 words[] = {0xAD090008, 0, 0x24100001, 0x26310001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, entry, words, 6);
  g_settings.cpu_recompiler_memory_exceptions = false;
  g_settings.cpu_recompiler_icache            = true;
  g_cpu_state.regs.r[8]                       = 0xA0410000;
  g_cpu_state.regs.r[9]                       = 0x24100002;
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[16], cached ? 1 : 2);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  u32 word;
  memcpy(&word, bus_get_unprotected_ram_pointer() + 0x10008, sizeof(word));
  EXPECT_EQ(word, 0x24100002);
  EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(entry));
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 1);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  finish_cpu();
}

TEST(NativeLut, cached_code_write_preserves_instruction_cache)
{
  check_lut_code_write(true);
}

TEST(NativeLut, uncached_code_write_observes_replacement)
{
  check_lut_code_write(false);
}

TEST(NativeLut, fault_classifier_rejects_unrelated_addresses_and_reads)
{
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 2);
  run_slice(1000);
  const u32 page = bus_get_ram_code_page_index(0x10000);
  u8*       ram  = bus_get_ram_pointer();
  EXPECT_TRUE(bus_is_ram_code_page(page));
  EXPECT_EQ(cpu_code_cache_handle_fastmem_exception(NULL, ram + 0x10000, false), PAGE_FAULT_HANDLER_EXECUTE_NEXT);
  EXPECT_TRUE(bus_is_ram_code_page(page));
  EXPECT_EQ(cpu_code_cache_handle_fastmem_exception(NULL, bus_get_fastmem_base(false), true),
            PAGE_FAULT_HANDLER_EXECUTE_NEXT);
  EXPECT_EQ(cpu_code_cache_handle_fastmem_exception(NULL, ram + 0x30000, true), PAGE_FAULT_HANDLER_EXECUTE_NEXT);
  EXPECT_EQ(cpu_code_cache_handle_fastmem_exception(NULL, ram + bus_get_ram_size(), true),
            PAGE_FAULT_HANDLER_EXECUTE_NEXT);
  EXPECT_TRUE(bus_is_ram_code_page(page));
  /* A host-side write also invalidates canonical code pages, including during isolation. */
  g_cpu_state.cop0_regs.sr.Isc = 1;
  EXPECT_EQ(cpu_code_cache_handle_fastmem_exception(NULL, ram + 0x10000, true), PAGE_FAULT_HANDLER_CONTINUE_EXECUTION);
  EXPECT_FALSE(bus_is_ram_code_page(page));
  finish_cpu();
}

TEST(NativeLut, checked_memory_path_reports_fastmem_disabled)
{
  const u32 words[] = {0xAD090000, 0x8D0A0000, 0, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 5);
  g_settings.cpu_recompiler_memory_exceptions = true;
  g_cpu_state.regs.r[8]                       = 0xA0030000;
  g_cpu_state.regs.r[9]                       = 42;
  EXPECT_FALSE(cpu_code_cache_is_using_fastmem());
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[10], 42);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 0);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 0);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  finish_cpu();
}

TEST(NativeLut, disabled_counters_preserve_execution)
{
  const u32 words[] = {0xAD090000, 0x8D0A0000, 0, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 5);
  cpu_execution_stats_initialize(false);
  g_settings.cpu_recompiler_memory_exceptions = false;
  g_cpu_state.regs.r[8]                       = 0xA0030000;
  g_cpu_state.regs.r[9]                       = 42;
  EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[10], 42);
  EXPECT_EQ(cpu_execution_stats_get()->native_blocks_entered, 0);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 0);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 0);
  finish_cpu();
}

TEST(NativeLut, merges_and_coprocessor_transfers_use_direct_accesses)
{
  const u32 words[] = {0xC9000000, 0xE9000008, 0xC5090004, 0x890A0004, 0x990A0001, 0, 0xA90B0014,
                       0xB90B0011, 0x8D0C0008, 0,          0x26100001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 13);
  g_settings.cpu_recompiler_memory_exceptions = false;
  g_cpu_state.cop0_regs.sr.CE1                = 1;
  g_cpu_state.cop0_regs.sr.CE2                = 1;
  g_cpu_state.regs.r[8]                       = 0xA0030000;
  g_cpu_state.regs.r[9]                       = 0xDEADBEEF;
  g_cpu_state.regs.r[11]                      = 0xAABBCCDD;
  const u32 values[] = {0x44332211, 0x88776655, 0x55555555, 0x55555555, 0x55555555, 0x55555555};
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, values, sizeof(values));
  run_slice(1000);
  EXPECT_EQ(gte_read_register(0), 0x44332211);
  EXPECT_EQ(g_cpu_state.regs.r[9], 0xDEADBEEF);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0x55443322);
  EXPECT_EQ(g_cpu_state.regs.r[12], 0x44332211);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  u32 actual[6];
  memcpy(actual, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(actual));
  EXPECT_EQ(actual[2], 0x44332211);
  EXPECT_EQ(actual[4], 0xBBCCDD55);
  EXPECT_EQ(actual[5], 0x555555AA);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 7);
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 3);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  finish_cpu();
}

TEST(NativeLut, guest_isolation_switch_restores_the_normal_table)
{
  for (u32 constant = 0; constant < 2; constant++) {
    const u32 words[] = {constant ? 0x3C0A0001 : 0,
                         0xAD090000,
                         0x408A6000,
                         0xAD0B0000,
                         0x40806000,
                         0xAD0C0004,
                         0x8D0D0000,
                         0x8D0E0004,
                         0,
                         0x26100001,
                         0x1000FFFF,
                         0};
    start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 12);
    g_settings.cpu_recompiler_memory_exceptions = false;
    g_cpu_state.regs.r[8]                       = 0x80030000;
    g_cpu_state.regs.r[9]                       = 0x11223344;
    g_cpu_state.regs.r[10]                      = 0x10000;
    g_cpu_state.regs.r[11]                      = 0x55667788;
    g_cpu_state.regs.r[12]                      = 0x99AABBCC;
    run_slice(1000);
    EXPECT_EQ(g_cpu_state.regs.r[13], 0x11223344);
    EXPECT_EQ(g_cpu_state.regs.r[14], 0x99AABBCC);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.sr.Isc, 0);
    EXPECT_EQ(g_cpu_state.icache_data[cpu_get_icache_line(0x80030000) * CPU_ICACHE_WORDS_PER_LINE], 0x55667788);
    EXPECT_EQ(g_cpu_state.fastmem_base, bus_get_fastmem_base(false));
    EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 2);
    EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 2);
    EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
    finish_cpu();
  }
}

TEST(NativeLut, return_from_exception_preserves_isolation_knowledge)
{
  const u32 words[] = {0x42000010, 0xAD090000, 0x400A6000, 0, 0x26100001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 7);
  g_settings.cpu_recompiler_memory_exceptions = false;
  g_cpu_state.cop0_regs.sr.bits               = 0x40010030;
  cpu_update_memory_pointers();
  g_cpu_state.regs.r[8] = 0x80031000;
  g_cpu_state.regs.r[9] = 0x44556677;
  const u32 canary      = 0x12345678;
  memcpy(bus_get_unprotected_ram_pointer() + 0x31000, &canary, sizeof(canary));
  run_slice(1000);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0x4001003C);
  EXPECT_EQ(g_cpu_state.cop0_regs.sr.bits, 0x4001003C);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  u32 actual;
  memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x31000, sizeof(actual));
  EXPECT_EQ(actual, canary);
  EXPECT_EQ(g_cpu_state.icache_data[cpu_get_icache_line(0x80031000) * CPU_ICACHE_WORDS_PER_LINE], 0x44556677);
  EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(0xA0010004));
  EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 0);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  finish_cpu();
}

TEST(NativeLut, configured_eight_mb_backing_keeps_distinct_native_accesses)
{
  const u32 segments[] = {0, 0x80000000, 0xA0000000};
  const u32 words[]    = {0xAD090000, 0x8D0A0000, 0, 0x26100001, 0x1000FFFF, 0};
  for (u32 chunk = 0; chunk < 4; chunk++) {
    for (u32 segment = 0; segment < 3; segment++) {
      start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 6);
      g_settings.cpu_enable_8mb_ram               = true;
      g_settings.cpu_recompiler_memory_exceptions = false;
      bus_initialize();
      cpu_update_memory_pointers();
      EXPECT_EQ(bus_get_ram_size(), BUS_RAM_8MB_SIZE);
      for (u32 other = 0; other < 4; other++) {
        const u32 canary = 0xAAAA0000 | other;
        memcpy(bus_get_unprotected_ram_pointer() + other * BUS_RAM_2MB_SIZE + 0x30000, &canary, sizeof(canary));
      }
      g_cpu_state.regs.r[8] = segments[segment] + chunk * BUS_RAM_2MB_SIZE + 0x30000;
      g_cpu_state.regs.r[9] = 0x12340000 | chunk;
      run_slice(1000);
      EXPECT_EQ(g_cpu_state.regs.r[10], 0x12340000 | chunk);
      EXPECT_EQ(g_cpu_state.regs.r[16], 1);
      for (u32 other = 0; other < 4; other++) {
        u32 actual;
        memcpy(&actual, bus_get_unprotected_ram_pointer() + other * BUS_RAM_2MB_SIZE + 0x30000, sizeof(actual));
        EXPECT_EQ(actual, other == chunk ? (0x12340000 | chunk) : (0xAAAA0000 | other));
      }
      EXPECT_EQ(cpu_execution_stats_get()->native_lut_loads, 1);
      EXPECT_EQ(cpu_execution_stats_get()->native_lut_stores, 1);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      finish_cpu();
    }
  }
}
