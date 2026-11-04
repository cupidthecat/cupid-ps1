/* cupid-ps1 partial stores must not consume CD response bytes. */
#include "tests/core/memory/partial_store_fixture.h"

static void check_cd_partial(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  u32 failures = 0, reads = 0, cases = 0;
  for (u32 increment = 0; increment < 2; increment++) {
    for (u32 left = 0; left < 2; left++) {
      for (u32 restore = 0; restore < 2; restore++) {
        const u32 loop[]  = {0x1000FFFF, 0};
        const u32 pc      = 0xA0010000;
        const u32 words[] = {0x240B1357, 0xAD4B0400, 0x8D4B0400, 0, left ? 0xA9090000 : 0xB9090003, 0x910D0000, 0,
                             0xAD4D0000, 0x26100001, 0x1000FFFF, 0};
        start_cpu_with_fastmem(mode, fastmem, 0xA0020000, loop, countof(loop));
        g_settings.cpu_recompiler_block_linking = linking;
        spu_initialize();
        sio_initialize();
        g_settings.cdrom_readahead_sectors = 0;
        cdrom_initialize();
        set_device_control(0x1F801018, 0x00020943 | (increment ? 0x2000 : 0));
        cdrom_write_register(0, 0);
        cdrom_write_register(1, 1);
        run_slice(30000);
        cdrom_write_register(0, 1);
        EXPECT_EQ(cdrom_read_register(3) & 7, 3);
        EXPECT_TRUE(cdrom_read_register(0) & 0x20);
        memcpy(bus_get_unprotected_ram_pointer() + 0x10000, words, sizeof(words));
        g_cpu_state.regs.r[8]  = 0x1F801800;
        g_cpu_state.regs.r[9]  = 0;
        g_cpu_state.regs.r[10] = 0xA0030000;
        cpu_set_pc(pc);
        run_to_pc(pc + 16);
        if (restore) restore_partial_devices(true);
        bus_memory_read_handler_t* handlers =
            bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
        const u32 page        = 0x1F801800 >> BUS_MEMORY_LUT_PAGE_SHIFT;
        partial_original_read = handlers[page];
        partial_word_reads    = 0;
        handlers[page]        = partial_observe_read;
        run_slice(1000);
        reads += partial_word_reads;
        handlers[page] = partial_original_read;
        u32 status;
        memcpy(&status, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(status));
        failures += !(status & 0x20);
        cases++;
        const u32                   done = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[11];
        const cpu_execution_stats_t stats = *cpu_execution_stats_get();
        cdrom_shutdown();
        sio_shutdown();
        spu_shutdown();
        finish_cpu();
        expect_device_bus_path(mode, fastmem, &stats);
        EXPECT_EQ(done, 1);
        EXPECT_EQ(loaded, 0x1357);
      }
    }
  }
  printf("partial-cd cpu=%u fastmem=%u linking=%u cases=%u fifo_losses=%u unexpected_reads=%u\n", mode, fastmem,
         linking, cases, failures, reads);
  EXPECT_EQ(cases, 8);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(reads, 0);
}
PARTIAL_DEVICE_MODES(CpuPartialCD, check_cd_partial)
