#include "tests/core/mdec/mdec_input_fixture.h"

static void check_cpu_dma_input(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 rooms[] = {66, 67}, requests[] = {1, 33}, patterns[] = {0xCAFE1357, 0x89ABCDEF};
  u32       observations = 0, failures = 0;
  for (u32 old = 0; old < 2; old++) {
    for (u32 room_index = 0; room_index < 2; room_index++) {
      for (u32 request_index = 0; request_index < 2; request_index++) {
        for (u32 pattern = 0; pattern < 2; pattern++) {
          /* DMA registers and transfer parameters come from guest RAM. */
          const u32 program[] = {0x3C081F80, 0x35081080, 0x3C0EA005, 0x8DC90000, 0x8DCA0004, 0x8DCB0008,
                                 0x8DCC000C, 0,          0xAD090000, 0xAD0A0004, 0xAD0B0070, 0xAD0C0008,
                                 0x24160001, 0x3C0DA003, 0xADB60000, 0x1000FFFF, 0};
          const u32 room      = rooms[room_index];
          begin_input(old != 0, room, mode, fastmem, program, sizeof(program) / sizeof(program[0]));
          g_settings.cpu_recompiler_block_linking = linking;
          EXPECT_EQ(dma_dbg_request(0), 1);
          const input_snapshot_t before = read_input();
          EXPECT_EQ(before.size, 512 - room);
          const u32 requested = requests[request_index];
          const u32 params[]  = {0x40000, 0x10000 | requested, 8, 0x01000201};
          u32       words[33];
          for (u32 i = 0; i < 33; i++)
            words[i] = patterns[pattern] ^ (i << 16) ^ i;
          memcpy(bus_get_unprotected_ram_pointer() + 0x40000, words, sizeof(words));
          memcpy(bus_get_unprotected_ram_pointer() + 0x50000, params, sizeof(params));
          memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xCC, sizeof(u32));
          run_to_pc(0xA0010000 + (sizeof(program) / sizeof(program[0]) - 2) * 4);
          u32 completed;
          memcpy(&completed, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(completed));
          EXPECT_EQ(completed, 1);
          EXPECT_EQ(dma_dbg_base_address(0), 0x40000 + 4 * requested);
          EXPECT_EQ(dma_dbg_block_control(0) >> 16, 0);
          EXPECT_EQ(dma_dbg_channel_control(0) & (1u << 24), 0);
          const input_snapshot_t after = read_input();
          bool                   equal = after.head == before.head && after.size == before.size + 2 * requested &&
                       after.tail == (before.tail + 2 * requested) % 512;
          for (u32 i = 0; i < before.size; i++)
            equal &= after.halfwords[(before.head + i) % 512] == before.halfwords[(before.head + i) % 512];
          for (u32 i = 0; i < 2 * requested; i++) {
            const u16 expected = (u16)(words[i / 2] >> (16 * (i & 1)));
            equal &= after.halfwords[(before.tail + i) % 512] == expected;
          }
          const cpu_execution_stats_t          stats  = *cpu_execution_stats_get();
          const cpu_cached_interpreter_stats_t cached = cpu_code_cache_get_cached_interpreter_stats();
          if (mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER) {
            EXPECT_TRUE(cached.blocks_created > 0 && cached.blocks_executed > 0 && cached.instructions_executed > 0);
            EXPECT_EQ(cached.fallback_instructions, 0);
          }
          if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
            EXPECT_TRUE(stats.native_blocks_completed > 0);
            EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
            if (fastmem == CPU_FASTMEM_MODE_LUT) {
              EXPECT_TRUE(stats.native_lut_loads >= 4);
              EXPECT_TRUE(stats.native_lut_stores >= 1);
            }
          }
          printf("mdec-input-dma-cpu mode=%u fastmem=%u linking=%u old=%u room=%u requested=%u pattern=%u "
                 "before=%u/%u/%u after=%u/%u/%u expected_size=%u status=%08X/%08X complete_words=%u\n",
                 mode, fastmem, linking, old, room, requested, pattern, before.head, before.tail, before.size,
                 after.head, after.tail, after.size, before.size + 2 * requested, before.status, after.status, equal);
          failures += !equal;
          observations++;
          end_input();
        }
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 16);
  EXPECT_EQ(failures, 0);
}

#define MDEC_INPUT_CPU_CASE(name, mode, fastmem, linking) \
  TEST(MdecInputDmaCpu, name)                             \
  {                                                       \
    check_cpu_dma_input(mode, fastmem, linking);          \
  }
MDEC_INPUT_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
MDEC_INPUT_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
MDEC_INPUT_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
MDEC_INPUT_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
MDEC_INPUT_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
MDEC_INPUT_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
MDEC_INPUT_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
MDEC_INPUT_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)

static void check_cpu_mmio_input(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 rooms[] = {0, 1, 2, 3}, patterns[] = {0xCAFE1357, 0x89ABCDEF};
  u32       observations = 0, failures = 0;
  for (u32 old = 0; old < 2; old++) {
    for (u32 index = 0; index < 4; index++) {
      for (u32 pattern = 0; pattern < 2; pattern++) {
        const u32 program[] = {0x3C081F80, 0x35081820, 0x3C0EA005, 0x8DC90000, 0, 0xAD090000,
                               0x24160001, 0x3C0DA003, 0xADB60000, 0x1000FFFF, 0};
        begin_input(old != 0, rooms[index], mode, fastmem, program, sizeof(program) / sizeof(program[0]));
        g_settings.cpu_recompiler_block_linking = linking;
        const input_snapshot_t before           = read_input();
        const u32              word             = patterns[pattern];
        memcpy(bus_get_unprotected_ram_pointer() + 0x50000, &word, sizeof(word));
        memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xCC, sizeof(u32));
        run_to_pc(0xA0010000 + (sizeof(program) / sizeof(program[0]) - 2) * 4);
        u32 completed;
        memcpy(&completed, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(completed));
        EXPECT_EQ(completed, 1);
        const input_snapshot_t               after    = read_input_raw();
        const u32                            accepted = rooms[index] >= 2 ? 1 : 0;
        const bool                           equal    = input_matches(&before, &after, &word, accepted);
        const cpu_execution_stats_t          stats    = *cpu_execution_stats_get();
        const cpu_cached_interpreter_stats_t cached   = cpu_code_cache_get_cached_interpreter_stats();
        if (mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER) {
          EXPECT_TRUE(cached.blocks_created > 0 && cached.blocks_executed > 0 && cached.instructions_executed > 0);
          EXPECT_EQ(cached.fallback_instructions, 0);
        }
        if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
          EXPECT_TRUE(stats.native_blocks_completed > 0);
          EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
          if (fastmem == CPU_FASTMEM_MODE_LUT) {
            EXPECT_TRUE(stats.native_lut_loads >= 1);
            EXPECT_TRUE(stats.native_lut_stores >= 1);
          }
        }
        printf("mdec-input-mmio-cpu mode=%u fastmem=%u linking=%u old=%u room=%u pattern=%u before=%u/%u/%u "
               "after=%u/%u/%u expected_size=%u status=%08X/%08X complete_words=%u\n",
               mode, fastmem, linking, old, rooms[index], pattern, before.head, before.tail, before.size, after.head,
               after.tail, after.size, before.size + 2 * accepted, before.status, after.status, equal);
        failures += !equal;
        observations++;
        end_input();
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 16);
  EXPECT_EQ(failures, 0);
}

#define MDEC_INPUT_MMIO_CPU_CASE(name, mode, fastmem, linking) \
  TEST(MdecInputMmioCpu, name)                                 \
  {                                                            \
    check_cpu_mmio_input(mode, fastmem, linking);              \
  }
MDEC_INPUT_MMIO_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
MDEC_INPUT_MMIO_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
MDEC_INPUT_MMIO_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
MDEC_INPUT_MMIO_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
MDEC_INPUT_MMIO_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
MDEC_INPUT_MMIO_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
MDEC_INPUT_MMIO_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
MDEC_INPUT_MMIO_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
