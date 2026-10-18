/* cupid-ps1 guest GPU reset and interrupt retriggering. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/timers/timers.h"

static void check_cpu_gpu_reset_irq(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  u32 failures = 0, observations = 0;
  for (u32 alias = 0; alias < 4; alias++) {
    for (u32 partial = 0; partial < 2; partial++) {
      const u32 words[]  = {0x3C081F80,
                            0x35081810,
                            0x3C0A1F80,
                            0x354A1070,
                            0x3C0BA003,
                            0x3C0EA004,
                            0x8DCC0000,
                            0x8DCD0004,
                            0x8DCF0008,
                            0,
                            0xAD0C0000,
                            0x8D100004,
                            0x8D510000,
                            0,
                            0xAD700000,
                            0xAD710004,
                           partial ? 0xAD0F0000 : 0,
                            0xAD0D0004,
                            0x8D120004,
                            0x8D530000,
                            0,
                            0xAD720008,
                            0xAD73000C,
                            0xAD400000,
                            0xAD0C0000,
                            0x8D140004,
                            0x8D550000,
                            0,
                            0xAD740010,
                            0xAD750014,
                            0x24160001,
                            0x1000FFFF,
                            0};
      const u32 params[] = {0x1F000000, alias << 30, 0x20000000};
      start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, sizeof(words) / sizeof(words[0]));
      g_settings.cpu_recompiler_block_linking = linking;
      interrupt_controller_reset();
      timers_initialize();
      gpu_backend_t* backend = gpu_sw_create();
      EXPECT_NOT_NULL(backend);
      gpu_initialize();
      gpu_reset(false);
      memcpy(bus_get_unprotected_ram_pointer() + 0x40000, params, sizeof(params));
      memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xCC, 6 * sizeof(u32));
      const u32 end_pc = 0xA0010000 + (sizeof(words) / sizeof(words[0]) - 2) * 4;
      run_to_pc(end_pc);
      u32 actual[6];
      memcpy(actual, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(actual));
      const u32                            completed = g_cpu_state.regs.r[22];
      const cpu_execution_stats_t          stats     = *cpu_execution_stats_get();
      const cpu_cached_interpreter_stats_t cached    = cpu_code_cache_get_cached_interpreter_stats();
      gpu_shutdown();
      gpu_backend_destroy(backend);
      timers_shutdown();
      finish_cpu();
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
          EXPECT_TRUE(stats.native_lut_stores >= 6);
        }
      }
      printf("gpu-reset-cpu-raw mode=%u fastmem=%u linking=%u alias=%u partial=%u before=%08X/%08X after=%08X/%08X "
             "next=%08X/%08X\n",
             mode, fastmem, linking, alias, partial, actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
      u32 fields[6];
      for (u32 i = 0; i < 6; i++)
        fields[i] = (actual[i] >> ((i & 1) ? INTERRUPT_CONTROLLER_IRQ_GPU : 24)) & 1;
      const u32 expected[] = {1, 1, 0, 1, 1, 1};
      printf("gpu-reset-cpu mode=%u fastmem=%u linking=%u alias=%u partial=%u before=%u/%u after=%u/%u next=%u/%u\n",
             mode, fastmem, linking, alias, partial, fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]);
      failures += memcmp(fields, expected, sizeof(fields)) != 0;
      observations++;
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 8);
  EXPECT_EQ(failures, 0);
}

#define GPU_RESET_CPU_CASE(name, mode, fastmem, linking) \
  TEST(GpuResetCpu, name)                                \
  {                                                      \
    check_cpu_gpu_reset_irq(mode, fastmem, linking);     \
  }
GPU_RESET_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
GPU_RESET_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
GPU_RESET_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
GPU_RESET_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
GPU_RESET_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
GPU_RESET_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
GPU_RESET_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
GPU_RESET_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
