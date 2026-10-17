/* cupid-ps1 ROM control changes with previously decoded host blocks. */
#include "tests/core/memory/device_bus_execution_fixture.h"

static void check_warm_rom_fetch(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 cached = 0; cached < 2; cached++) {
    const u32 base = 0xA0010000, target = cached ? 0x9FC00400 : 0xBFC00400;
    const u32 words[] = {0x24091357, 0xAD490000, 0x8D4B0000, 0, 0xAD8E1010, 0x01000008, 0};
    start_cpu_with_fastmem(mode, fastmem, base, words, sizeof(words) / sizeof(words[0]));
    g_settings.cpu_recompiler_block_linking = linking;
    const u32 rom[]                         = {0x24110024, 0x03E00008, 0};
    memcpy(bus_get_bios_pointer() + 0x400, rom, sizeof(rom));
    for (u32 phase = 0; phase < 4; phase++) {
      /* Keep host blocks throughout. Phase 1 also retains the emulated cache. */
      if (phase >= 2) cpu_clear_icache();
      const bool increment   = phase == 0 || phase == 3;
      const bool raw         = increment || (cached && phase == 1);
      g_cpu_state.regs.r[1]  = 0x1000;
      g_cpu_state.regs.r[4]  = 0;
      g_cpu_state.regs.r[8]  = target;
      g_cpu_state.regs.r[10] = 0xA0030000;
      g_cpu_state.regs.r[12] = 0x1F800000;
      g_cpu_state.regs.r[14] = 0x0013043F | (increment ? 0x2000 : 0);
      g_cpu_state.regs.r[17] = 0;
      cpu_set_pc(base);
      run_to_pc(target);
      const u32 fetched = g_cpu_state.next_instruction.bits;
      const u64 entered = cpu_execution_stats_get()->native_blocks_entered;
      run_to_pc(target + 4);
      printf("device-bus-warm-fetch mode=%u fastmem=%u linking=%u cached=%u phase=%u fetched=%08X "
             "argument=%08X result=%08X native=%llu\n",
             mode, fastmem, linking, cached, phase, fetched, g_cpu_state.regs.r[4], g_cpu_state.regs.r[17],
             (unsigned long long)cpu_execution_stats_get()->native_blocks_completed);
      fflush(stdout);
      if (mode == CPU_EXECUTION_MODE_RECOMPILER)
        EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_entered > entered);
      EXPECT_EQ(fetched, raw ? 0x24110024 : 0x24242424);
      EXPECT_EQ(g_cpu_state.regs.r[4], raw ? 0 : 0x3424);
      EXPECT_EQ(g_cpu_state.regs.r[17], raw ? 0x24 : 0);
      EXPECT_EQ(g_cpu_state.regs.r[11], 0x1357);
    }
    const cpu_execution_stats_t stats = *cpu_execution_stats_get();
    finish_cpu();
    expect_device_bus_path(mode, fastmem, &stats);
  }
}

#define WARM_FETCH_CASE(name, mode, fastmem, linking) \
  TEST(DeviceBusWarmFetch, name)                      \
  {                                                   \
    check_warm_rom_fetch(mode, fastmem, linking);     \
  }
WARM_FETCH_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
WARM_FETCH_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
WARM_FETCH_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
WARM_FETCH_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
WARM_FETCH_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
WARM_FETCH_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
WARM_FETCH_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
WARM_FETCH_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
