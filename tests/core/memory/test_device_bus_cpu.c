/* cupid-ps1 device-bus controls during CPU execution and state restoration. */
#include "tests/core/memory/device_bus_execution_fixture.h"

static void set_bios_control(u32 value)
{
  cpu_get_memory_write_handler(0x1F801010, MEMORY_ACCESS_SIZE_WORD)(0x1F801010, value);
}

static void check_bios_cpu_data(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 constant = 0; constant < 2; constant++) {
    for (u32 restore = 0; restore < 2; restore++) {
      const u32 base    = 0xA0010000;
      const u32 words[] = {constant ? 0x3C08BFC0 : 0,
                           constant ? 0x35080400 : 0,
                           0x24091357,
                           0xAD490000,
                           0x8D4B0000,
                           0,
                           0x8D110000,
                           0,
                           0x3C0C1F80,
                           0x3C0D0013,
                           0x35AD043F,
                           0xAD8D1010,
                           0x8D120000,
                           0,
                           0x35AD2000,
                           0xAD8D1010,
                           0x8D130000,
                           0,
                           0x26100001,
                           0x1000FFFF,
                           0};
      start_cpu_with_fastmem(mode, fastmem, base, words, sizeof(words) / sizeof(words[0]));
      g_settings.cpu_recompiler_block_linking = linking;
      g_cpu_state.regs.r[8]                   = 0xBFC00400;
      g_cpu_state.regs.r[10]                  = 0xA0030000;
      const u32 pattern                       = 0x44332211;
      memcpy(bus_get_bios_pointer() + 0x400, &pattern, sizeof(pattern));
      run_to_pc(base + 48); /* The guest has disabled ROM address stepping. */
      if (restore) restore_device_bus_execution(base + 0x100, 0x1F801010, false);
      run_slice(1000);
      const u32 first = g_cpu_state.regs.r[17], held = g_cpu_state.regs.r[18], last = g_cpu_state.regs.r[19];
      const u32 prefix = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
      u32       ram;
      memcpy(&ram, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(ram));
      const cpu_execution_stats_t stats = *cpu_execution_stats_get();
      finish_cpu();
      printf("device-bus-cpu-data mode=%u fastmem=%u linking=%u constant=%u restore=%u first=%08X held=%08X "
             "last=%08X native=%llu interpreted=%llu slots=%llu\n",
             mode, fastmem, linking, constant, restore, first, held, last,
             (unsigned long long)stats.native_blocks_completed, (unsigned long long)stats.interpreted_instructions,
             (unsigned long long)stats.native_dispatch_delay_slots);
      fflush(stdout);
      expect_device_bus_path(mode, fastmem, &stats);
      EXPECT_EQ(first, 0x44332211);
      EXPECT_EQ(held, 0x11111111);
      EXPECT_EQ(last, 0x44332211);
      EXPECT_EQ(prefix, 1);
      EXPECT_EQ(loaded, 0x1357);
      EXPECT_EQ(ram, 0x1357);
    }
  }
}

static void check_bios_cpu_fetch(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 increment = 0; increment < 2; increment++) {
    for (u32 cached = 0; cached < 2; cached++) {
      for (u32 restore = 0; restore < 2; restore++) {
        const u32 base = 0xA0010000, target = cached ? 0x9FC00400 : 0xBFC00400;
        const u32 words[] = {0x24091357, 0xAD490000, 0x8D4B0000, 0, 0x26100001, 0x01000008, 0};
        start_cpu_with_fastmem(mode, fastmem, base, words, sizeof(words) / sizeof(words[0]));
        g_settings.cpu_recompiler_block_linking = linking;
        g_cpu_state.regs.r[1]                   = 0x1000;
        g_cpu_state.regs.r[8]                   = target;
        g_cpu_state.regs.r[10]                  = 0xA0030000;
        const u32 rom[]                         = {0x24110024, 0x03E00008, 0};
        memcpy(bus_get_bios_pointer() + 0x400, rom, sizeof(rom));
        set_bios_control(0x0013043F | (increment ? 0x2000 : 0));
        run_to_pc(target);
        if (restore) restore_device_bus_execution(base + 0x100, 0x1F801010, false);
        const u64 entered = cpu_execution_stats_get()->native_blocks_entered;
        const u32 fetched = g_cpu_state.next_instruction.bits;
        run_to_pc(target + 4);
        const u32 argument = g_cpu_state.regs.r[4], result = g_cpu_state.regs.r[17];
        const u32 prefix = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
        u32       ram;
        memcpy(&ram, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(ram));
        const cpu_execution_stats_t stats = *cpu_execution_stats_get();
        finish_cpu();
        printf("device-bus-cpu-fetch mode=%u fastmem=%u linking=%u increment=%u cached=%u restore=%u "
               "fetched=%08X argument=%08X result=%08X native=%llu interpreted=%llu slots=%llu\n",
               mode, fastmem, linking, increment, cached, restore, fetched, argument, result,
               (unsigned long long)stats.native_blocks_completed, (unsigned long long)stats.interpreted_instructions,
               (unsigned long long)stats.native_dispatch_delay_slots);
        fflush(stdout);
        expect_device_bus_path(mode, fastmem, &stats);
        if (mode == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(stats.native_blocks_entered > entered);
        EXPECT_EQ(argument, increment ? 0 : 0x3424);
        EXPECT_EQ(result, increment ? 0x24 : 0);
        EXPECT_EQ(fetched, increment ? 0x24110024 : 0x24242424);
        EXPECT_EQ(prefix, 1);
        EXPECT_EQ(loaded, 0x1357);
        EXPECT_EQ(ram, 0x1357);
      }
    }
  }
}

#define DEVICE_CPU_CASES(name, mode, fastmem, linking) \
  TEST(DeviceBusCPU, name##_data)                      \
  {                                                    \
    check_bios_cpu_data(mode, fastmem, linking);       \
  }                                                    \
  TEST(DeviceBusCPU, name##_fetch)                     \
  {                                                    \
    check_bios_cpu_fetch(mode, fastmem, linking);      \
  }
DEVICE_CPU_CASES(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
DEVICE_CPU_CASES(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
DEVICE_CPU_CASES(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
DEVICE_CPU_CASES(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
DEVICE_CPU_CASES(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
DEVICE_CPU_CASES(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
DEVICE_CPU_CASES(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
DEVICE_CPU_CASES(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
