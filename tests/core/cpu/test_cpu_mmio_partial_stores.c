/* cupid-ps1 partial stores to full-width MMIO register latches. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"

static bus_memory_read_handler_t original_device_read;
static u32                       observed_device_reads;

static u32 observe_device_read(u32 address)
{
  observed_device_reads++;
  return original_device_read(address);
}

static void check_partial_stores(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 baselines[] = {0, 0xFFFFFFFF, 0x11223344};
  const u32 sources[]   = {0xAABBCCDD, 0xFEDCBA98};
  u32       failures = 0, cells = 0, unexpected_reads = 0;
  for (u32 left = 0; left < 2; left++) {
    for (u32 offset = 0; offset < 4; offset++) {
      for (u32 pattern = 0; pattern < 3; pattern++) {
        for (u32 source = 0; source < 2; source++) {
          const u32 words[] = {0x240B1357, 0xAD4B0000, 0x8D4C0000, 0, (left ? 0xA9090000 : 0xB9090000) | offset,
                               0x26100001, 0x1000FFFF, 0};
          start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, sizeof(words) / sizeof(words[0]));
          g_settings.cpu_recompiler_block_linking = linking;
          dma_initialize();
          dma_write_register(0x70, baselines[pattern]);
          g_cpu_state.regs.r[8]  = 0x1F8010F0;
          g_cpu_state.regs.r[9]  = sources[source];
          g_cpu_state.regs.r[10] = 0xA0030000;
          bus_memory_read_handler_t* handlers =
              bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
          const u32 page        = 0x1F8010F0 >> BUS_MEMORY_LUT_PAGE_SHIFT;
          original_device_read  = handlers[page];
          observed_device_reads = 0;
          handlers[page]        = observe_device_read;
          run_slice(1000);
          const u32 reads    = observed_device_reads;
          handlers[page]     = original_device_read;
          const u32 actual   = dma_read_register(0x70);
          const u32 expected = left ? sources[source] >> ((3u - offset) * 8u) : sources[source] << (offset * 8u);
          const u32 prefix = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[12];
          const cpu_execution_stats_t stats = *cpu_execution_stats_get();
          dma_shutdown();
          finish_cpu();
          printf("partial-store-dpcr mode=%u fastmem=%u linking=%u left=%u offset=%u baseline=%08X source=%08X "
                 "actual=%08X expected=%08X device_reads=%u native=%llu interpreted=%llu slots=%llu\n",
                 mode, fastmem, linking, left, offset, baselines[pattern], sources[source], actual, expected, reads,
                 (unsigned long long)stats.native_blocks_completed, (unsigned long long)stats.interpreted_instructions,
                 (unsigned long long)stats.native_dispatch_delay_slots);
          fflush(stdout);
          EXPECT_EQ(prefix, 1);
          EXPECT_EQ(loaded, 0x1357);
          if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
            EXPECT_TRUE(stats.native_blocks_completed > 0);
            EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
            if (fastmem == CPU_FASTMEM_MODE_LUT) {
              EXPECT_TRUE(stats.native_lut_loads > 0);
              EXPECT_TRUE(stats.native_lut_stores > 0);
            }
          }
          failures += actual != expected;
          unexpected_reads += reads;
          cells++;
        }
      }
    }
  }
  EXPECT_EQ(cells, 48);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(unexpected_reads, 0);
}

#define PARTIAL_CASE(name, mode, fastmem, linking) \
  TEST(CpuPartialMMIO, name)                       \
  {                                                \
    check_partial_stores(mode, fastmem, linking);  \
  }
PARTIAL_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
PARTIAL_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
PARTIAL_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
PARTIAL_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
PARTIAL_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
PARTIAL_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
PARTIAL_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
PARTIAL_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
