/* cupid-ps1 16-bit SPU partial stores and neighboring registers. */
#include "tests/core/memory/device_bus_execution_fixture.h"

static bus_memory_read_handler_t original_spu_word_read;
static u32                       observed_spu_word_reads;

static u32 observe_spu_word_read(u32 address)
{
  observed_spu_word_reads++;
  return original_spu_word_read(address);
}

static void emit_constant(u32* words, u32* count, u32 reg, u32 value)
{
  words[(*count)++] = 0x3C000000 | (reg << 16) | (value >> 16);
  words[(*count)++] = 0x34000000 | (reg << 21) | (reg << 16) | (value & 0xFFFF);
}

static void check_spu_partial_stores(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 baselines[]          = {0, 0xFFFFFFFF, 0x11223344};
  const u32 sources[]            = {0xAABBCCDD, 0xFEDCBA98};
  const u16 left_values[2][2][4] = {
      {{0xCCDD, 0xDD00, 0, 0}, {0x00AA, 0xAABB, 0xBBCC, 0xCCDD}},
      {{0xBA98, 0x9800, 0, 0}, {0x00FE, 0xFEDC, 0xDCBA, 0xBA98}},
  };
  const u16 right_values[2][2][4] = {
      {{0xAABB, 0, 0xCCDD, 0}, {0, 0, 0, 0xAABB}},
      {{0xFEDC, 0, 0xBA98, 0}, {0, 0, 0, 0xFEDC}},
  };
  const u8 changes_left[] = {0x3, 0xF}, changes_right[] = {0x5, 0x8};
  u32      failures = 0, cells = 0, unexpected_reads = 0;
  for (u32 increment = 0; increment < 2; increment++) {
    for (u32 constant = 0; constant < 2; constant++) {
      for (u32 restore = 0; restore < 2; restore++) {
        const u32 base = 0xA0010000;
        u32       words[2048], count = 0, first_partial = 0;
        if (constant)
          emit_constant(words, &count, 8, 0x1F801D80);
        else {
          words[count++] = 0;
          words[count++] = 0;
        }
        words[count++] = 0x240B1357;
        words[count++] = 0xAD4B0400;
        words[count++] = 0x8D4B0400;
        words[count++] = 0;
        u32 expected[96], dataset = 0;
        for (u32 left = 0; left < 2; left++) {
          for (u32 offset = 0; offset < 4; offset++) {
            for (u32 pattern = 0; pattern < 3; pattern++) {
              for (u32 source = 0; source < 2; source++) {
                emit_constant(words, &count, 14, baselines[pattern]);
                words[count++] = 0xA50E0000;
                emit_constant(words, &count, 15, baselines[pattern] >> 16);
                words[count++] = 0xA50F0002;
                emit_constant(words, &count, 9, sources[source]);
                if (dataset == 2) first_partial = base + count * sizeof(words[0]);
                words[count++]            = (left ? 0xA9090000 : 0xB9090000) | offset;
                words[count++]            = 0x950C0000;
                words[count++]            = 0x950D0002;
                words[count++]            = 0;
                words[count++]            = 0xAD4C0000 | (dataset * 8);
                words[count++]            = 0xAD4D0000 | (dataset * 8 + 4);
                expected[dataset * 2]     = (changes_left[left] & (1u << offset)) ? left_values[source][left][offset]
                                                                                  : baselines[pattern] & 0xFFFF;
                expected[dataset * 2 + 1] = (changes_right[left] & (1u << offset)) ? right_values[source][left][offset]
                                                                                   : baselines[pattern] >> 16;
                if (!increment && ((left && offset == 3) || (!left && offset == 0))) {
                  expected[dataset * 2]     = sources[source] >> 16;
                  expected[dataset * 2 + 1] = baselines[pattern] >> 16;
                }
                dataset++;
              }
            }
          }
        }
        words[count++] = 0x26100001;
        words[count++] = 0x1000FFFF;
        words[count++] = 0;
        EXPECT_EQ(dataset, 48);
        EXPECT_TRUE(count <= sizeof(words) / sizeof(words[0]));
        start_cpu_with_fastmem(mode, fastmem, base, words, count);
        g_settings.cpu_recompiler_block_linking = linking;
        g_cpu_state.regs.r[8]                   = 0x1F801D80;
        g_cpu_state.regs.r[10]                  = 0xA0030000;
        spu_initialize();
        set_device_control(0x1F801014, 0x200911E1 | (increment ? 0x2000 : 0));
        run_to_pc(first_partial);
        if (restore) restore_device_bus_execution(base + 0x2000, 0x1F801014, true);
        bus_memory_read_handler_t* handlers =
            bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
        const u32 page          = 0x1F801D80 >> BUS_MEMORY_LUT_PAGE_SHIFT;
        original_spu_word_read  = handlers[page];
        observed_spu_word_reads = 0;
        handlers[page]          = observe_spu_word_read;
        run_slice((tick_count_t)(count * 32 + 1000));
        const u32 reads = observed_spu_word_reads;
        handlers[page]  = original_spu_word_read;
        unexpected_reads += reads;
        u32 actual[96];
        memcpy(actual, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(actual));
        const u32                   finished = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
        const cpu_execution_stats_t stats = *cpu_execution_stats_get();
        spu_shutdown();
        finish_cpu();
        expect_device_bus_path(mode, fastmem, &stats);
        EXPECT_EQ(finished, 1);
        EXPECT_EQ(loaded, 0x1357);
        for (u32 index = 0; index < dataset; index++) {
          printf("partial-store-spu mode=%u fastmem=%u linking=%u increment=%u constant=%u restore=%u "
                 "case=%u left=%04X right=%04X expected_left=%04X expected_right=%04X\n",
                 mode, fastmem, linking, increment, constant, restore, index, actual[index * 2], actual[index * 2 + 1],
                 expected[index * 2], expected[index * 2 + 1]);
          failures += actual[index * 2] != expected[index * 2] || actual[index * 2 + 1] != expected[index * 2 + 1];
          cells++;
        }
        fflush(stdout);
      }
    }
  }
  EXPECT_EQ(cells, 384);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(unexpected_reads, 0);
}

#define SPU_PARTIAL_CASE(name, mode, fastmem, linking) \
  TEST(CpuPartialSPU, name)                            \
  {                                                    \
    check_spu_partial_stores(mode, fastmem, linking);  \
  }
SPU_PARTIAL_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_PARTIAL_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_PARTIAL_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_PARTIAL_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
SPU_PARTIAL_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
SPU_PARTIAL_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
SPU_PARTIAL_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
SPU_PARTIAL_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
