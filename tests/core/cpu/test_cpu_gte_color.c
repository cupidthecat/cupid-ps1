/* cupid-ps1 GTE color read delays and pending-command restoration. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gte/gte.h"

static void restore_pending_gte(u32 scratch_pc) {
  u8              cpu[16384], timing[16384];
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, cpu, sizeof(cpu), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t cpu_size = state_wrapper_get_position(&sw);
  state_wrapper_init_write(&sw, timing, sizeof(timing), SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t timing_size = state_wrapper_get_position(&sw);
  gte_reset();
  g_cpu_state.gte_completion_tick = 0;
  g_cpu_state.regs.r[9]           = 0xDEADBEEF;
  cpu_set_pc(scratch_pc);
  state_wrapper_init_read(&sw, cpu, cpu_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  state_wrapper_init_read(&sw, timing, timing_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  cpu_code_cache_reset();
}

static void check_cpu_gte_color(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  const u32 commands[] = {0x11, 0x16, 0x3D, 0x3E};
  for (u32 operation = 0; operation < countof(commands); operation++) {
    for (u32 sf = 0; sf < 2; sf++) {
      for (u32 lm = 0; lm < 2; lm++) {
        for (u32 restore = 0; restore < 2; restore++) {
          const u32 pc = 0x80010000, op = commands[operation];
          const u32 words[] = {0x4A000000 | op | sf << 19 | lm << 10,
                               0x10000001,
                               0,
                               0x4809B000,
                               0xAD490000,
                               0xAD490004,
                               0x8D4B0008,
                               0,
                               0xAD4B000C,
                               0x484CF800,
                               0,
                               0xAD4C0010,
                               0x26100001,
                               0x1000FFFF,
                               0};
          start_cpu_with_fastmem(mode, fastmem, pc, words, countof(words));
          g_settings.cpu_recompiler_block_linking = linking;
          g_cpu_state.cop0_regs.sr.CE2            = 1;
          g_cpu_state.regs.r[9]                   = 0xC0DE1234;
          g_cpu_state.regs.r[10]                  = 0xA0030000;
          const u32 source                        = 0x13579BDF;
          memcpy(bus_get_unprotected_ram_pointer() + 0x30008, &source, sizeof(source));
          gte_initialize();
          gte_write_register(6, 0xA5000000);
          gte_write_register(8, (op == 0x11 || op == 0x3E) ? 0 : 4096);
          for (u32 i = 0; i < 3; i++) {
            gte_write_register(9 + i, UINT32_MAX);
            gte_write_register(25 + i, UINT32_MAX);
            gte_write_register(45 + i, UINT32_MAX);
            gte_write_register(53 + i, UINT32_MAX);
          }
          run_to_pc(pc + 12);
          const u32 remaining = g_cpu_state.gte_completion_tick;
          EXPECT_TRUE(remaining > 0);
          const u32 flags = gte_read_register(63);
          if (restore) restore_pending_gte(pc + 0x2000);
          EXPECT_EQ(g_cpu_state.gte_completion_tick, remaining);
          run_slice(1000);
          u32 result[5];
          memcpy(result, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(result));
          EXPECT_EQ(result[0], 0xC0DE1234);
          EXPECT_EQ(result[1], 0xA5000000);
          EXPECT_EQ(result[3], source);
          EXPECT_EQ(result[4], flags);
          EXPECT_EQ(g_cpu_state.regs.r[16], 1);
          const cpu_execution_stats_t* stats = cpu_execution_stats_get();
          if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
            EXPECT_TRUE(stats->native_blocks_completed > 0);
            EXPECT_EQ(stats->interpreted_instructions, stats->native_dispatch_delay_slots);
            if (fastmem == CPU_FASTMEM_MODE_LUT) {
              EXPECT_TRUE(stats->native_lut_loads > 0);
              EXPECT_TRUE(stats->native_lut_stores > 0);
            }
          }
          finish_cpu();
        }
      }
    }
  }
}
#define COLOR_MODE(name, mode, memory, link) \
  TEST(CpuGteColor, name) {                  \
    check_cpu_gte_color(mode, memory, link); \
  }
COLOR_MODE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
COLOR_MODE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
COLOR_MODE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
COLOR_MODE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
COLOR_MODE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
COLOR_MODE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
COLOR_MODE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
COLOR_MODE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
