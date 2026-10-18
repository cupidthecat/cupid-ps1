/* cupid-ps1 guest GPU reset scanout coordinates. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"

static void check_cpu_display_reset(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u16 addresses[][2] = {{0, 0}, {64, 32}, {768, 300}, {1000, 500}};
  u32       failures = 0, observations = 0;
  for (u32 alias = 0; alias < 4; alias++) {
    for (u32 pal = 0; pal < 2; pal++) {
      for (u32 address = 0; address < 4; address++) {
        /* t0 points to GP1; t6 to RAM parameters; t3 to the completion word. */
        const u32 words[]  = {0x3C081F80, 0x35081814, 0x3C0EA004, 0x3C0BA003, 0x8DCC0000, 0x8DCD0004, 0x8DCF0008, 0,
                              0xAD0F0000, 0xAD0C0000, 0xAD0D0000, 0x24160001, 0xAD760000, 0x1000FFFF, 0};
        const u32 params[] = {0x05000000 | addresses[address][0] | ((u32)addresses[address][1] << 10), alias << 30,
                              0x08000000 | (pal ? 8u : 0u)};
        display_fixture_t fixture;
        begin_display_cpu(&fixture, mode, fastmem, words, sizeof(words) / sizeof(words[0]));
        g_settings.cpu_recompiler_block_linking         = linking;
        const gpu_backend_update_display_cmd_t expected = read_display();
        memcpy(bus_get_unprotected_ram_pointer() + 0x40000, params, sizeof(params));
        memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xCC, sizeof(u32));
        run_to_pc(0xA0010000 + (sizeof(words) / sizeof(words[0]) - 2) * 4);
        u32 completed;
        memcpy(&completed, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(completed));
        const gpu_backend_update_display_cmd_t actual = read_display();
        const u32                              status = gpu_read_register(4), irq = irq_dbg_status_register();
        const cpu_execution_stats_t            stats  = *cpu_execution_stats_get();
        const cpu_cached_interpreter_stats_t   cached = cpu_code_cache_get_cached_interpreter_stats();
        end_display(&fixture);
        EXPECT_EQ(completed, 1);
        if (mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER) {
          EXPECT_TRUE(cached.blocks_created > 0);
          EXPECT_TRUE(cached.blocks_executed > 0);
          EXPECT_TRUE(cached.instructions_executed > 0);
          EXPECT_EQ(cached.fallback_instructions, 0);
        }
        if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
          EXPECT_TRUE(stats.native_blocks_completed > 0);
          EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
          if (fastmem == CPU_FASTMEM_MODE_LUT) {
            EXPECT_TRUE(stats.native_lut_loads >= 3);
            EXPECT_TRUE(stats.native_lut_stores >= 1);
          }
        }
        const bool equal = display_equal(&actual, &expected);
        printf("gpu-reset-display-cpu mode=%u fastmem=%u linking=%u alias=%u pal=%u address=%u,%u after=%u,%u "
               "expected=%u,%u status=%08X irq=%08X complete_config_equal=%u\n",
               mode, fastmem, linking, alias, pal, addresses[address][0], addresses[address][1],
               actual.display_vram_left, actual.display_vram_top, expected.display_vram_left, expected.display_vram_top,
               status, irq, equal);
        failures += !equal;
        observations++;
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 32);
  EXPECT_EQ(failures, 0);
}

#define GPU_RESET_DISPLAY_CPU_CASE(name, mode, fastmem, linking) \
  TEST(GpuResetDisplayCpu, name)                                 \
  {                                                              \
    check_cpu_display_reset(mode, fastmem, linking);             \
  }
GPU_RESET_DISPLAY_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
GPU_RESET_DISPLAY_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
GPU_RESET_DISPLAY_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
GPU_RESET_DISPLAY_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
GPU_RESET_DISPLAY_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
GPU_RESET_DISPLAY_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
GPU_RESET_DISPLAY_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
GPU_RESET_DISPLAY_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
