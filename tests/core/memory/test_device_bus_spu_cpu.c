/* cupid-ps1 SPU bus controls exercised by guest CPU programs. */
#include "tests/core/memory/device_bus_execution_fixture.h"

static void emit_constant(u32* words, u32* count, u32 reg, u32 value)
{
  words[(*count)++] = 0x3C000000 | (reg << 16) | (value >> 16);
  words[(*count)++] = 0x34000000 | (reg << 21) | (reg << 16) | (value & 0xFFFF);
}

static void emit_spu_control(u32* words, u32* count, u32 value)
{
  emit_constant(words, count, 14, value);
  words[(*count)++] = 0xAD8E0014;
}

static void emit_spu_word(u32* words, u32* count, u32 value)
{
  emit_constant(words, count, 15, value);
  words[(*count)++] = 0xADAF0000;
}

static void check_spu_cpu_controls(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 constant = 0; constant < 2; constant++) {
    for (u32 restore = 0; restore < 2; restore++) {
      const u32 base = 0xA0010000;
      u32       words[128], count = 0;
      if (constant)
        emit_constant(words, &count, 13, 0x1F801D80);
      else {
        words[count++] = 0;
        words[count++] = 0;
      }
      words[count++] = 0x24091357;
      words[count++] = 0xAD090000;
      words[count++] = 0x8D0B0000;
      words[count++] = 0;
      emit_constant(words, &count, 12, 0x1F801000);
      emit_spu_control(words, &count, 0x200931E1);
      words[count++] = 0x8DB10000;
      emit_spu_control(words, &count, 0x200911E1);
      emit_spu_word(words, &count, 0x11223344);
      words[count++] = 0x8DB20000;
      emit_spu_control(words, &count, 0x200921E1);
      emit_spu_word(words, &count, 0x11223344);
      words[count++] = 0x8DB30000;
      emit_spu_control(words, &count, 0x200901E1);
      emit_spu_word(words, &count, 0x55667788);
      const u32 boundary = base + count * sizeof(words[0]);
      words[count++]     = 0x8DB40000;
      emit_spu_control(words, &count, 0x200931E1);
      emit_constant(words, &count, 15, 0x12345678);
      words[count++] = 0xA1AF0000;
      words[count++] = 0xA1AF0001;
      words[count++] = 0x8DB50000;
      emit_spu_control(words, &count, 0x200921E1);
      emit_constant(words, &count, 15, 0xABCDEF42);
      words[count++] = 0xA1AF0001;
      words[count++] = 0x8DB60000;
      words[count++] = 0;
      words[count++] = 0x26100001;
      words[count++] = 0x1000FFFF;
      words[count++] = 0;
      EXPECT_TRUE(count <= sizeof(words) / sizeof(words[0]));
      start_cpu_with_fastmem(mode, fastmem, base, words, count);
      g_settings.cpu_recompiler_block_linking = linking;
      g_cpu_state.regs.r[8]                   = 0xA0030000;
      g_cpu_state.regs.r[13]                  = 0x1F801D80;
      spu_initialize();
      spu_write_register(0x180, 0x1234);
      spu_write_register(0x182, 0x5678);
      run_to_pc(boundary);
      if (restore) restore_device_bus_execution(base + 0x300, 0x1F801014, true);
      run_slice(5000);
      u32 results[6];
      memcpy(results, &g_cpu_state.regs.r[17], sizeof(results));
      const u32 prefix = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
      u32       ram;
      memcpy(&ram, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(ram));
      const cpu_execution_stats_t stats = *cpu_execution_stats_get();
      spu_shutdown();
      finish_cpu();
      printf("device-bus-spu-cpu mode=%u fastmem=%u linking=%u constant=%u restore=%u "
             "values=%08X,%08X,%08X,%08X,%08X,%08X native=%llu interpreted=%llu slots=%llu\n",
             mode, fastmem, linking, constant, restore, results[0], results[1], results[2], results[3], results[4],
             results[5], (unsigned long long)stats.native_blocks_completed,
             (unsigned long long)stats.interpreted_instructions, (unsigned long long)stats.native_dispatch_delay_slots);
      fflush(stdout);
      expect_device_bus_path(mode, fastmem, &stats);
      const u32 expected[] = {0x56781234, 0x11221122, 0x11113333, 0x55555555, 0xFF115678, 0x11114242};
      for (u32 i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        EXPECT_EQ(results[i], expected[i]);
      EXPECT_EQ(prefix, 1);
      EXPECT_EQ(loaded, 0x1357);
      EXPECT_EQ(ram, 0x1357);
    }
  }
}

#define SPU_CPU_CASE(name, mode, fastmem, linking)  \
  TEST(DeviceBusSPUCPU, name)                       \
  {                                                 \
    check_spu_cpu_controls(mode, fastmem, linking); \
  }
SPU_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
SPU_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
SPU_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
SPU_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
SPU_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
