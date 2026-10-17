/* cupid-ps1 CD-ROM bus controls exercised by guest CPU programs. */
#include "tests/core/memory/device_bus_execution_fixture.h"
#include "core/cdrom/cdrom.h"

static void emit_constant(u32* words, u32* count, u32 reg, u32 value)
{
  words[(*count)++] = 0x3C000000 | (reg << 16) | (value >> 16);
  words[(*count)++] = 0x34000000 | (reg << 21) | (reg << 16) | (value & 0xFFFF);
}

static void emit_cd_control(u32* words, u32* count, bool increment)
{
  emit_constant(words, count, 14, 0x00020943 | (increment ? 0x2000 : 0));
  words[(*count)++] = 0xAD8E0018;
}

static void emit_acknowledge(u32* words, u32* count)
{
  words[(*count)++] = 0x240F0001;
  words[(*count)++] = 0xA1AF0000;
  words[(*count)++] = 0x240F005F;
  words[(*count)++] = 0xA1AF0003;
  words[(*count)++] = 0xA1A00000;
}

static void emit_wait_for_command(u32* words, u32* count)
{
  words[(*count)++] = 0x91AE0000;
  words[(*count)++] = 0;
  words[(*count)++] = 0x31CE0080;
  words[(*count)++] = 0x15C0FFFC;
  words[(*count)++] = 0;
}

static void restore_cd_execution(u32 scratch_pc)
{
  const size_t capacity = 512u * 1024u;
  u8*          bytes    = malloc(capacity);
  EXPECT_NOT_NULL(bytes);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(cdrom_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t size = state_wrapper_get_position(&sw);
  restore_device_bus_execution(scratch_pc, 0x1F801018, true);
  (void)cdrom_read_register(1);
  cdrom_write_register(0, 3);
  state_wrapper_init_read(&sw, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(cdrom_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), size);
  free(bytes);
}

static void check_cd_cpu_controls(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 constant = 0; constant < 2; constant++) {
    for (u32 restore = 0; restore < 2; restore++) {
      const u32 base = 0xA0011000;
      u32       words[128], count = 0;
      if (constant)
        emit_constant(words, &count, 13, 0x1F801800);
      else {
        words[count++] = 0;
        words[count++] = 0;
      }
      words[count++] = 0x24091357;
      words[count++] = 0xAD090000;
      words[count++] = 0x8D0B0000;
      words[count++] = 0;
      emit_constant(words, &count, 12, 0x1F801000);
      emit_cd_control(words, &count, false);
      words[count++] = 0x8DB10000;
      emit_cd_control(words, &count, true);
      words[count++] = 0x95B20000;
      emit_cd_control(words, &count, false);
      const u32 boundary = base + count * sizeof(words[0]);
      words[count++]     = 0x8DB30000;
      words[count++]     = 0x91B40001;
      /* Drain the three remaining Getmode bytes, including reads into r0. */
      words[count++] = 0x91A00001;
      words[count++] = 0x91A00001;
      words[count++] = 0x91A00001;
      words[count++] = 0x91B80000;
      emit_acknowledge(words, &count);
      words[count++] = 0x240F3412;
      words[count++] = 0xA5AF0002;
      words[count++] = 0x240F000D;
      words[count++] = 0xA1AF0001;
      emit_wait_for_command(words, &count);
      words[count++] = 0x91B50001;
      emit_acknowledge(words, &count);
      words[count++] = 0x240F000F;
      words[count++] = 0xA1AF0001;
      emit_wait_for_command(words, &count);
      words[count++] = 0x91B90001;
      words[count++] = 0x91BA0001;
      words[count++] = 0x91BB0001;
      words[count++] = 0x91B60001;
      words[count++] = 0x91B70001;
      words[count++] = 0;
      words[count++] = 0x26100001;
      words[count++] = 0x1000FFFF;
      words[count++] = 0;
      EXPECT_TRUE(count <= sizeof(words) / sizeof(words[0]));

      const u32 loop[] = {0x1000FFFF, 0};
      start_cpu_with_fastmem(mode, fastmem, 0xA0010000, loop, 2);
      g_settings.cpu_recompiler_block_linking = linking;
      g_settings.cdrom_readahead_sectors      = 0;
      spu_initialize();
      cdrom_initialize();
      cdrom_write_register(1, 0x0F);
      run_slice(30000);
      EXPECT_EQ(cdrom_read_register(0), 0x38);
      memcpy(bus_get_unprotected_ram_pointer() + (base & bus_get_ram_mask()), words, count * sizeof(*words));
      cpu_code_cache_reset();
      cpu_set_pc(base);
      cpu_execution_stats_initialize(true);
      g_cpu_state.regs.r[8]  = 0xA0030000;
      g_cpu_state.regs.r[13] = 0x1F801800;
      run_to_pc(boundary);
      if (restore) restore_cd_execution(base + 0x300);
      run_slice(60000); /* Two no-disc command acknowledgements each need 15000 ticks. */
      u32 results[8], mode_reply[3];
      memcpy(results, &g_cpu_state.regs.r[17], sizeof(results));
      memcpy(mode_reply, &g_cpu_state.regs.r[25], sizeof(mode_reply));
      const u32 prefix = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
      u32       ram;
      memcpy(&ram, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(ram));
      const cpu_execution_stats_t stats = *cpu_execution_stats_get();
      cdrom_shutdown();
      spu_shutdown();
      finish_cpu();
      printf("device-bus-cd-cpu mode=%u fastmem=%u linking=%u constant=%u restore=%u "
             "held=%08X increment=%08X held_after=%08X next=%02X filter_reply=%02X filter=%02X,%02X "
             "empty=%02X native=%llu interpreted=%llu slots=%llu\n",
             mode, fastmem, linking, constant, restore, results[0], results[1], results[2], results[3], results[4],
             results[5], results[6], results[7], (unsigned long long)stats.native_blocks_completed,
             (unsigned long long)stats.interpreted_instructions, (unsigned long long)stats.native_dispatch_delay_slots);
      fflush(stdout);
      expect_device_bus_path(mode, fastmem, &stats);
      const u32 expected[] = {0x38383838, 0x1038, 0x38383838, 0x20, 0x10, 0x12, 0x34, 0x18};
      for (u32 i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        EXPECT_EQ(results[i], expected[i]);
      EXPECT_EQ(mode_reply[0], 0x10);
      EXPECT_EQ(mode_reply[1], 0x20);
      EXPECT_EQ(mode_reply[2], 0);
      EXPECT_EQ(prefix, 1);
      EXPECT_EQ(loaded, 0x1357);
      EXPECT_EQ(ram, 0x1357);
    }
  }
}

#define CD_CPU_CASE(name, mode, fastmem, linking)  \
  TEST(DeviceBusCDCPU, name)                       \
  {                                                \
    check_cd_cpu_controls(mode, fastmem, linking); \
  }
CD_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
CD_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
CD_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
CD_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
CD_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
CD_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
CD_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
CD_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
