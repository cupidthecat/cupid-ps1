/* cupid-ps1 native memory-access state regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void check_consecutive_stores(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 isolated = 0; isolated < 2; isolated++) {
    const u32 words[] = {0xAD090000, 0xAD090004, 0x26100001, 0x1000FFFF, 0};
    start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, 5);
    g_settings.cpu_recompiler_block_linking = linking;
    g_cpu_state.cop0_regs.sr.Isc            = isolated;
    cpu_update_memory_pointers();
    g_cpu_state.regs.r[8] = 0x80031000;
    g_cpu_state.regs.r[9] = 0x11223344;
    const u32 canary[]    = {0xAABBCCDD, 0x55667788};
    memcpy(bus_get_unprotected_ram_pointer() + 0x31000, canary, sizeof(canary));
    run_slice(1000);
    u32 actual[2];
    memcpy(actual, bus_get_unprotected_ram_pointer() + 0x31000, sizeof(actual));
    EXPECT_EQ(actual[0], isolated ? canary[0] : 0x11223344);
    EXPECT_EQ(actual[1], isolated ? canary[1] : 0x11223344);
    if (isolated) {
      const u32 offset = cpu_get_icache_line(0x80031000) * CPU_ICACHE_WORDS_PER_LINE;
      EXPECT_EQ(g_cpu_state.icache_data[offset], 0x11223344);
      EXPECT_EQ(g_cpu_state.icache_data[offset + 1], 0x11223344);
      if (fastmem != CPU_FASTMEM_MODE_DISABLED) {
        EXPECT_TRUE(!cpu_code_cache_has_previously_faulted_on_pc(0xA0010000));
        EXPECT_TRUE(!cpu_code_cache_has_previously_faulted_on_pc(0xA0010004));
      }
    }
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.sr.Isc, isolated);
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
      EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
    }
    if (fastmem != CPU_FASTMEM_MODE_DISABLED) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    finish_cpu();
  }
}

TEST(NativeMemoryIsolation, consecutive_stores_interpreter)
{
  check_consecutive_stores(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativeMemoryIsolation, consecutive_stores_cached)
{
  check_consecutive_stores(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativeMemoryIsolation, consecutive_stores_native)
{
  check_consecutive_stores(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativeMemoryIsolation, consecutive_stores_mmap)
{
  check_consecutive_stores(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);
}
TEST(NativeMemoryIsolation, consecutive_stores_unlinked)
{
  check_consecutive_stores(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);
}

TEST(NativeMemoryIsolation, consecutive_stores_lut)
{
  check_consecutive_stores(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true);
}
