/* cupid-ps1 pending loads must not feed the immediately following store. */
#include "tests/core/memory/partial_store_fixture.h"

static void check_partial_delays(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 port = 0; port < 2; port++) {
    for (u32 left = 0; left < 2; left++) {
      for (u32 restore = 0; restore < 2; restore++) {
        const u32 pc = 0xA0010000;
        const u32 words[] = {0x8D490000, left ? 0xA9090001 : 0xB9090002,
                            0x01208821, 0x950C0000 | (left ? 0 : 2), 0,
                            0xAD4C0004, 0x26100001, 0x1000FFFF, 0};
        start_cpu_with_fastmem(mode, fastmem, pc, words, countof(words));
        g_settings.cpu_recompiler_block_linking = linking;
        g_cpu_state.regs.r[8] = port ? 0x1F801058 : 0x1F801048;
        g_cpu_state.regs.r[9] = 0xAABB0C01;
        g_cpu_state.regs.r[10] = 0xA0030000;
        const u32 next = 0xFEDC2202;
        memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &next, sizeof(next));
        spu_initialize(); pad_initialize(); sio_initialize();
        run_to_pc(pc + 4);
        if (restore) restore_partial_devices(false);
        bus_memory_read_handler_t* handlers = bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
        const u32 page = 0x1F801040 >> BUS_MEMORY_LUT_PAGE_SHIFT;
        partial_original_read = handlers[page]; partial_word_reads = 0;
        handlers[page] = partial_observe_read;
        run_slice(1000);
        const u32 reads = partial_word_reads; handlers[page] = partial_original_read;
        u32 actual; memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x30004, sizeof(actual));
        EXPECT_EQ(actual, left ? 0xAABB & (port ? 0xFF : 0x13F) : 0x0C01);
        EXPECT_EQ(g_cpu_state.regs.r[17], next);
        EXPECT_EQ(g_cpu_state.regs.r[16], 1); EXPECT_EQ(reads, 0);
        expect_device_bus_path(mode, fastmem, cpu_execution_stats_get());
        sio_shutdown(); pad_shutdown(); spu_shutdown(); finish_cpu();
      }
    }
  }
}
PARTIAL_DEVICE_MODES(CpuPartialDelay, check_partial_delays)
