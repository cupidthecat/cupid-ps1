/* cupid-ps1 partial-store storage mappings must retain disabled bytes. */
#include "tests/core/memory/partial_store_fixture.h"

static void check_partial_storage(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 addresses[] = {0xA0031000, 0x1F800100, 0x1F801800};
  for (u32 kind = 0; kind < 3; kind++) {
    for (u32 left = 0; left < 2; left++) {
      for (u32 offset = 0; offset < 4; offset++) {
        for (u32 restore = 0; restore < 2; restore++) {
          const u32 words[] = {0x240B1357, 0xAD4B0400, 0x8D4B0400, 0,
                              (left ? 0xA9090000 : 0xB9090000) | offset,
                              0x8D0C0000, 0, 0xAD4C0000, 0x26100001, 0x1000FFFF, 0};
          const u32 pc = 0xA0010000, address = addresses[kind];
          start_cpu_with_fastmem(mode, fastmem, pc, words, countof(words));
          g_settings.cpu_recompiler_block_linking = linking;
          g_cpu_state.regs.r[8] = address;
          g_cpu_state.regs.r[9] = 0xAABBCCDD;
          g_cpu_state.regs.r[10] = 0xA0030000;
          cpu_set_cache_control_bits(0x1E908);
          const u32 shadow = address & bus_get_ram_mask(), canary = 0x55667788;
          memcpy(bus_get_unprotected_ram_pointer() + shadow, &canary, sizeof(canary));
          u32* target;
          if (kind == 2)
            target = cpu_get_icache_data() + cpu_get_icache_line(address) * 4 +
                     (cpu_get_icache_line_offset(address) >> 2);
          else if (kind == 1) target = (u32*)(cpu_get_scratchpad_pointer() + 0x100);
          else target = (u32*)(bus_get_unprotected_ram_pointer() + shadow);
          *target = 0x11223344;
          run_to_pc(pc + 16);
          if (kind == 2) {
            g_cpu_state.cop0_regs.sr.Isc = 1;
            cpu_update_memory_pointers();
            cpu_code_cache_reset();
          }
          if (restore) restore_device_bus_execution(pc + 0x2000, 0xBF801018, false);
          run_slice(1000);
          const u8 source[] = {0xDD, 0xCC, 0xBB, 0xAA};
          u8 expected[] = {0x44, 0x33, 0x22, 0x11};
          for (u32 byte = 0; byte < 4; byte++) {
            if (left && byte <= offset) expected[byte] = source[3 - offset + byte];
            if (!left && byte >= offset) expected[byte] = source[byte - offset];
          }
          u32 actual; memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(actual));
          EXPECT_EQ(memcmp(&actual, expected, sizeof(actual)), 0);
          EXPECT_EQ(memcmp(target, expected, sizeof(actual)), 0);
          if (kind) {
            u32 untouched; memcpy(&untouched, bus_get_unprotected_ram_pointer() + shadow, sizeof(untouched));
            EXPECT_EQ(untouched, canary);
          }
          EXPECT_EQ(g_cpu_state.regs.r[16], 1); EXPECT_EQ(g_cpu_state.regs.r[11], 0x1357);
          expect_device_bus_path(mode, fastmem, cpu_execution_stats_get());
          finish_cpu();
        }
      }
    }
  }
}
PARTIAL_DEVICE_MODES(CpuPartialStorage, check_partial_storage)
