/* cupid-ps1 ROM translation edges preserve loads, branches and exceptions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void bios_page_case(cpu_execution_mode_t mode, cpu_fastmem_mode_t memory, bool linking, bool cached, u32 kind) {
  const u32 idle[] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, memory, 0xA0010000, idle, 2);
  g_settings.cpu_recompiler_block_linking = linking;
  g_cpu_state.cop0_regs.sr.bits           = 0;
  g_cpu_state.regs.r[2]                   = 5;
  g_cpu_state.regs.r[8]                   = 0xA0020000;
  const u32 data                          = 42;
  memcpy(bus_get_unprotected_ram_pointer() + 0x20000, &data, sizeof(data));
  const u32    start     = (cached ? 0x9FC00000u : 0xBFC00000u) + HOST_PAGE_SIZE - 8;
  const u32    load[]    = {0, 0x8D020000, 0x24430001, 0x00402021, 0x1000FFFF, 0};
  const u32    taken[]   = {0x24020003, 0x10000002, 0x24420007, 0x24420064, 0x00401821, 0x1000FFFF, 0};
  const u32    untaken[] = {0x24020003, 0x14420002, 0x24420007, 0x24420064, 0x00401821, 0x1000FFFF, 0};
  const u32    fault[]   = {0, 0x10000002, 0x8C020001, 0, 0, 0x1000FFFF, 0};
  const u32*   words     = kind == 0 ? load : kind == 1 ? taken : kind == 2 ? untaken : fault;
  const size_t size      = kind == 0 ? sizeof(load) : sizeof(taken);
  memset(bus_get_bios_pointer(), 0, BUS_BIOS_SIZE);
  if (kind != 4) memcpy(bus_get_bios_pointer() + HOST_PAGE_SIZE - 8, words, size);
  cpu_set_pc(start);
  if (kind == 4) {
    /* A readable zero-filled ROM is a NOP stream. It must make ordinary
     * CPU progress without exhausting the host translation buffers. */
    run_to_pc(start + 16);
    EXPECT_EQ(g_cpu_state.regs.r[2], 5);
  } else if (kind == 3) {
    run_to_pc(0x80000080);
    EXPECT_EQ((g_cpu_state.cop0_regs.cause.bits >> 2) & 31, CPU_EXCEPTION_ADEL);
    EXPECT_TRUE(g_cpu_state.cop0_regs.cause.bits & 0x80000000u);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, start + 4);
    EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 1);
  } else {
    run_to_pc(start + (kind == 0 ? 16 : 20));
    EXPECT_EQ(g_cpu_state.regs.r[2], kind == 0 ? 42 : kind == 1 ? 10 : 110);
    EXPECT_EQ(g_cpu_state.regs.r[3], kind == 0 ? 6 : kind == 1 ? 10 : 110);
    if (kind == 0) EXPECT_EQ(g_cpu_state.regs.r[4], 42);
  }
  if (mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER)
    EXPECT_EQ(cpu_code_cache_get_cached_interpreter_stats().fallback_instructions, 0);
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
    const cpu_execution_stats_t* stats = cpu_execution_stats_get();
    EXPECT_TRUE(stats->native_blocks_entered > 0);
    EXPECT_EQ(stats->interpreted_instructions, stats->native_dispatch_delay_slots);
  }
  finish_cpu();
}

static void bios_pages(cpu_execution_mode_t mode, cpu_fastmem_mode_t memory, bool linking) {
  for (u32 cached = 0; cached < 2; cached++)
    for (u32 kind = 0; kind < 5; kind++)
      bios_page_case(mode, memory, linking, cached != 0, kind);
}

#define BIOS_PAGE_TEST(name, mode, memory, linking) \
  TEST(BiosPageBoundary, name) {                    \
    bios_pages(mode, memory, linking);              \
  }
BIOS_PAGE_TEST(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
BIOS_PAGE_TEST(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
BIOS_PAGE_TEST(native_off, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
BIOS_PAGE_TEST(native_off_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
BIOS_PAGE_TEST(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
BIOS_PAGE_TEST(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
BIOS_PAGE_TEST(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
BIOS_PAGE_TEST(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
