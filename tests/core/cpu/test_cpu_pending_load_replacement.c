/* cupid-ps1 pending loads are forwarded to merges and cancelled by replacement. */
#include "tests/core/memory/partial_store_fixture.h"
#include "core/gte/gte.h"

static void check_pending_replacement(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  /* Two input words: 89ABCDEF and 13579BDF; initial r9 is AABBCCDD. */
  const struct {
    u32 encoding, first, second, keep;
  } loads[] = {
      {0x80000000, 0xFFFFFFEF, 0xFFFFFFDF, 0}, {0x84000000, 0xFFFFCDEF, 0xFFFF9BDF, 0},
      {0x8C000000, 0x89ABCDEF, 0x13579BDF, 0}, {0x90000000, 0x000000EF, 0x000000DF, 0},
      {0x94000000, 0x0000CDEF, 0x00009BDF, 0},
      {0x88000000, 0xEFBBCCDD, 0xDF000000, 0x00FFFFFF},
      {0x88000001, 0xCDEFCCDD, 0x9BDF0000, 0x0000FFFF},
      {0x88000002, 0xABCDEFDD, 0x579BDF00, 0x000000FF},
      {0x88000003, 0x89ABCDEF, 0x13579BDF, 0},
      {0x98000000, 0x89ABCDEF, 0x13579BDF, 0},
      {0x98000001, 0xAA89ABCD, 0x0013579B, 0xFF000000},
      {0x98000002, 0xAABB89AB, 0x00001357, 0xFFFF0000},
      {0x98000003, 0xAABBCC89, 0x00000013, 0xFFFFFF00},
      {0x40007800, 2, 2, 0},
      {0x48000000, 0x13579BDF, 0x13579BDF, 0},
      {0x4800E000, 0x3DEF, 0x3DEF, 0},
      {0x48402800, 0x2468ACE0, 0x2468ACE0, 0},
  };
  const u32 pc = 0xA0010000, targets[] = {9, 12, 0};
  const u32 entries[] = {6, 13, 14, 15, 16};
  u32 words[8192], expected[1734], count = 0, cases = 0, pauses[countof(entries)];
  for (u32 target = 0; target < countof(targets); target++) {
    const u32 reg = targets[target];
    for (u32 first = 0; first < countof(loads); first++) {
      for (u32 second = 0; second < countof(loads); second++) {
        partial_constant(words, &count, 9, 0xAABBCCDD);
        partial_constant(words, &count, 12, 0x55667788);
        words[count++] = loads[first].encoding |
                          ((loads[first].encoding >> 26) >= 0x20 ? (8u << 21) : 0) | (9u << 16);
        if (!target && !first) {
          for (u32 entry = 0; entry < countof(entries); entry++) {
            if (second == entries[entry]) pauses[entry] = pc + count * 4;
          }
        }
        words[count++] = loads[second].encoding |
                          ((loads[second].encoding >> 26) >= 0x20 ? (14u << 21) : 0) | (reg << 16);
        words[count++] = 0x01206821;
        words[count++] = 0xAD400000 | (reg << 16) | (cases * 8);
        words[count++] = 0xAD4D0000 | (cases * 8 + 4);
        const u32 prior = reg == 9 ? loads[first].first : 0x55667788;
        expected[cases * 2] = reg ? ((prior & loads[second].keep) | loads[second].second) : 0;
        expected[cases * 2 + 1] = reg == 9 ? 0xAABBCCDD : loads[first].first;
        cases++;
      }
    }
  }
  words[count++] = 0x26100001;
  words[count++] = 0x1000FFFF;
  words[count++] = 0;
  EXPECT_EQ(cases, 867);
  EXPECT_TRUE(count < countof(words));
  for (u32 restore = 0; restore <= countof(entries); restore++) {
    start_cpu_with_fastmem(mode, fastmem, pc, words, count);
    g_settings.cpu_recompiler_block_linking = linking;
    spu_initialize();
    sio_initialize();
    g_settings.cdrom_readahead_sectors = 0;
    cdrom_initialize();
    g_cpu_state.cop0_regs.sr.CE2 = 1;
    gte_write_register(0, 0x13579BDF);
    gte_write_register(28, 0x3DEF);
    gte_write_register(37, 0x2468ACE0);
    const u32 input[] = {0x89ABCDEF, 0x13579BDF};
    memcpy(bus_get_unprotected_ram_pointer() + 0x38000, input, sizeof(input));
    g_cpu_state.regs.r[8] = 0xA0038000;
    g_cpu_state.regs.r[14] = 0xA0038004;
    g_cpu_state.regs.r[10] = 0xA0030000;
    if (restore) {
      run_to_pc(pauses[restore - 1]);
      EXPECT_EQ(g_cpu_state.regs.r[9], 0xAABBCCDD);
      EXPECT_EQ(g_cpu_state.load_delay_reg, 9);
      EXPECT_EQ(g_cpu_state.load_delay_value, 0xFFFFFFEF);
      restore_partial_devices(true);
    }
    run_slice(200000);
    const u32* actual = (const u32*)(bus_get_unprotected_ram_pointer() + 0x30000);
    u32 wrong_values = 0, wrong_delays = 0;
    for (u32 i = 0; i < cases; i++) {
      wrong_values += actual[i * 2] != expected[i * 2];
      wrong_delays += actual[i * 2 + 1] != expected[i * 2 + 1];
    }
    const u32 done = g_cpu_state.regs.r[16];
    const cpu_execution_stats_t stats = *cpu_execution_stats_get();
    cdrom_shutdown();
    sio_shutdown();
    spu_shutdown();
    finish_cpu();
    expect_device_bus_path(mode, fastmem, &stats);
    printf("pending-load cpu=%u fastmem=%u linking=%u restored=%u cases=%u wrong_values=%u wrong_delays=%u\n",
           mode, fastmem, linking, restore, cases, wrong_values, wrong_delays);
    EXPECT_EQ(done, 1);
    EXPECT_EQ(wrong_values, 0);
    EXPECT_EQ(wrong_delays, 0);
  }
}

PARTIAL_DEVICE_MODES(CpuPendingLoadReplacement, check_pending_replacement)
