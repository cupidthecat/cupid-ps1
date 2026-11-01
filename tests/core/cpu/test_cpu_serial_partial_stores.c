/* cupid-ps1 serial register selection and read-free partial stores. */
#include "tests/core/memory/partial_store_fixture.h"

static void check_serial_partial(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u16 modes[] = {0, 0x13F, 0x120}, controls[] = {0, 0xC00, 0x2100};
  const u32 sources[] = {0xAABB0C01, 0xFEDC2202};
  const u16 values[2][2][4] = {
      {{0x0C01, 0x0100, 0x0C01, 0x0100}, {0x00AA, 0xAABB, 0xBB0C, 0x0C01}},
      {{0x2202, 0x0200, 0x2202, 0x0200}, {0x00FE, 0xFEDC, 0xDC22, 0x2202}},
  };
  u32 failures = 0, reads = 0, cells = 0;
  for (u32 port = 0; port < 2; port++) {
    for (u32 restore = 0; restore < 2; restore++) {
      const u32 pc = 0xA0010000;
      u32 words[2048], count = 0, first = 0, expected[96], dataset = 0;
      words[count++] = 0x240B1357;
      words[count++] = 0xAD4B0400;
      words[count++] = 0x8D4B0400;
      words[count++] = 0;
      for (u32 left = 0; left < 2; left++) {
        for (u32 offset = 0; offset < 4; offset++) {
          for (u32 baseline = 0; baseline < 3; baseline++) {
            for (u32 source = 0; source < 2; source++) {
              partial_constant(words, &count, 14, modes[baseline]);
              words[count++] = 0xA50E0000;
              partial_constant(words, &count, 15, controls[baseline]);
              words[count++] = 0xA50F0002;
              partial_constant(words, &count, 9, sources[source]);
              if (!dataset) first = pc + count * 4;
              words[count++] = (left ? 0xA9090000 : 0xB9090000) | offset;
              words[count++] = 0x950C0000;
              words[count++] = 0x950D0002;
              words[count++] = 0;
              words[count++] = 0xAD4C0000 | (dataset * 8);
              words[count++] = 0xAD4D0000 | (dataset * 8 + 4);
              const u16 mask = port ? 0xFF : 0x13F;
              const bool upper = !left && offset >= 2;
              expected[dataset * 2] = (upper ? modes[baseline] : values[source][left][offset]) & mask;
              expected[dataset * 2 + 1] = upper ? values[source][left][offset] : controls[baseline];
              dataset++;
            }
          }
        }
      }
      words[count++] = 0x26100001;
      words[count++] = 0x1000FFFF;
      words[count++] = 0;
      EXPECT_EQ(dataset, 48);
      EXPECT_TRUE(count <= countof(words));
      start_cpu_with_fastmem(mode, fastmem, pc, words, count);
      g_settings.cpu_recompiler_block_linking = linking;
      g_cpu_state.regs.r[8] = port ? 0x1F801058 : 0x1F801048;
      g_cpu_state.regs.r[10] = 0xA0030000;
      spu_initialize(); pad_initialize(); sio_initialize();
      run_to_pc(first);
      if (restore) restore_partial_devices(false);
      bus_memory_read_handler_t* handlers = bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
      const u32 page = 0x1F801040 >> BUS_MEMORY_LUT_PAGE_SHIFT;
      partial_original_read = handlers[page]; partial_word_reads = 0;
      handlers[page] = partial_observe_read;
      run_slice((tick_count_t)(count * 32 + 5000));
      reads += partial_word_reads; handlers[page] = partial_original_read;
      u32 actual[96]; memcpy(actual, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(actual));
      const u32 done = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
      const cpu_execution_stats_t stats = *cpu_execution_stats_get();
      sio_shutdown(); pad_shutdown(); spu_shutdown(); finish_cpu();
      expect_device_bus_path(mode, fastmem, &stats);
      EXPECT_EQ(done, 1); EXPECT_EQ(loaded, 0x1357);
      for (u32 i = 0; i < dataset; i++) {
        const bool wrong = actual[i * 2] != expected[i * 2] || actual[i * 2 + 1] != expected[i * 2 + 1];
        if (wrong && failures < 8)
          printf("partial-serial port=%u restore=%u case=%u mode=%04X/%04X ctrl=%04X/%04X\n",
                 port, restore, i, actual[i * 2], expected[i * 2], actual[i * 2 + 1], expected[i * 2 + 1]);
        failures += wrong; cells++;
      }
    }
  }
  printf("partial-serial cpu=%u fastmem=%u linking=%u cells=%u failures=%u unexpected_reads=%u\n",
         mode, fastmem, linking, cells, failures, reads);
  EXPECT_EQ(cells, 192); EXPECT_EQ(failures, 0); EXPECT_EQ(reads, 0);
}
PARTIAL_DEVICE_MODES(CpuPartialSerial, check_serial_partial)
