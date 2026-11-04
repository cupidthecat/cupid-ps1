/* cupid-ps1 partial loads select byte and halfword peripheral reads. */
#include "tests/core/memory/partial_store_fixture.h"

typedef struct {
  u32 value, delayed, completed;
  u64 ticks;
  bool response_pending;
} partial_load_result_t;

static partial_load_result_t run_port_load(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking,
                                           u32 instruction, u32 address, bool restore, bool cd) {
  const u32 pc = 0xA0010000, output = 0xA0030000;
  const u32 words[] = {0x240B1357, 0xAD4B0400, 0x8D4B0400, 0x8D490404, instruction,
                       0x01206821, 0xAD490000, 0xAD4D0004, 0x26100001, 0x1000FFFF, 0};
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, fastmem, cd ? 0xA0020000 : pc, cd ? loop : words,
                        cd ? countof(loop) : countof(words));
  g_settings.cpu_recompiler_block_linking = linking;
  spu_initialize();
  pad_initialize();
  sio_initialize();
  g_settings.cdrom_readahead_sectors = 0;
  cdrom_initialize();
  spu_write_register(0x180, 0x1357);
  spu_write_register(0x182, 0x2468);
  pad_write_register(8, 0x38);
  pad_write_register(10, 0x1003);
  if (cd) {
    set_device_control(0x1F801018, 0x00022943);
    cdrom_write_register(0, 0);
    cdrom_write_register(1, 1);
    run_slice(30000);
    cdrom_write_register(0, 1);
    EXPECT_EQ(cdrom_read_register(3) & 7, 3);
    EXPECT_TRUE(cdrom_read_register(0) & 0x20);
    memcpy(bus_get_unprotected_ram_pointer() + 0x10000, words, sizeof(words));
    cpu_set_pc(pc);
  }
  const u32 prior = 0x12345678;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30404, &prior, sizeof(prior));
  g_cpu_state.regs.r[8] = address;
  g_cpu_state.regs.r[9] = 0xAABBCCDD;
  g_cpu_state.regs.r[10] = output;
  run_to_pc(pc + 16);
  EXPECT_EQ(g_cpu_state.load_delay_reg, 9);
  EXPECT_EQ(g_cpu_state.load_delay_value, prior);
  if (restore) restore_partial_devices(true);
  const u64 start = timing_events_get_global_tick_counter();
  run_to_pc(pc + 32);
  partial_load_result_t result = {.ticks = timing_events_get_global_tick_counter() - start};
  memcpy(&result.value, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(result.value));
  memcpy(&result.delayed, bus_get_unprotected_ram_pointer() + 0x30004, sizeof(result.delayed));
  result.response_pending = cdrom_read_register(0) & 0x20;
  run_slice(1000);
  result.completed = g_cpu_state.regs.r[16];
  const cpu_execution_stats_t stats = *cpu_execution_stats_get();
  cdrom_shutdown();
  sio_shutdown();
  pad_shutdown();
  spu_shutdown();
  finish_cpu();
  expect_device_bus_path(mode, fastmem, &stats);
  const bool writes_zero = ((instruction >> 16) & 31u) == 0;
  EXPECT_EQ(result.delayed, writes_zero ? prior : 0xAABBCCDD);
  EXPECT_EQ(result.completed, 1);
  return result;
}

static void check_port_loads(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  const struct {
    u32 partial, ordinary, expected;
  } cases[] = {
      {0x89090000, 0x91090000, 0x57345678},
      {0x89090001, 0x95090000, 0x13575678},
      {0x99090002, 0x95090002, 0x12342468},
      {0x99090003, 0x91090003, 0x12345624},
  };
  u32 wrong_values = 0, wrong_ticks = 0, count = 0;
  for (u32 restore = 0; restore < 2; restore++) {
    for (u32 zero = 0; zero < 2; zero++) {
      for (u32 i = 0; i < countof(cases); i++) {
        const u32 dest_mask = zero ? ~(31u << 16) : UINT32_MAX;
        const partial_load_result_t partial = run_port_load(mode, fastmem, linking, cases[i].partial & dest_mask,
                                                            0x1F801D80, restore, false);
        const partial_load_result_t ordinary = run_port_load(mode, fastmem, linking, cases[i].ordinary & dest_mask,
                                                             0x1F801D80, restore, false);
        /* A write to r0 still reads the port; the RAM capture observes r9. */
        wrong_values += partial.value != (zero ? 0x12345678 : cases[i].expected);
        wrong_ticks += partial.ticks != ordinary.ticks;
        count++;
      }
    }
    const partial_load_result_t serial = run_port_load(mode, fastmem, linking, 0x99090002, 0x1F801048,
                                                       restore, false);
    wrong_values += serial.value != 0x12341003;
    count++;
  }
  printf("partial-load ports cpu=%u fastmem=%u linking=%u cases=%u wrong_values=%u wrong_ticks=%u\n",
         mode, fastmem, linking, count, wrong_values, wrong_ticks);
  EXPECT_EQ(count, 18);
  EXPECT_EQ(wrong_values, 0);
  EXPECT_EQ(wrong_ticks, 0);
}

static void check_response_loads(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  u32 lost = 0;
  for (u32 restore = 0; restore < 2; restore++) {
    for (u32 zero = 0; zero < 2; zero++) {
      const u32 mask = zero ? ~(31u << 16) : UINT32_MAX;
      const partial_load_result_t status = run_port_load(mode, fastmem, linking, 0x89090000 & mask,
                                                         0x1F801800, restore, true);
      const partial_load_result_t flags = run_port_load(mode, fastmem, linking, 0x99090003 & mask,
                                                        0x1F801800, restore, true);
      lost += !status.response_pending;
      lost += !flags.response_pending;
      if (!zero) EXPECT_EQ(flags.value, 0x123456E3);
    }
  }
  printf("partial-load CD cpu=%u fastmem=%u linking=%u lost_responses=%u\n", mode, fastmem, linking, lost);
  EXPECT_EQ(lost, 0);
}

PARTIAL_DEVICE_MODES(CpuPartialLoadPorts, check_port_loads)
PARTIAL_DEVICE_MODES(CpuPartialLoadResponses, check_response_loads)

static void check_word_register_loads(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  /* Preserve the existing word decoder; these are not physical alias captures. */
  const u32 instructions[] = {0x89090000, 0x89090001, 0x89090002, 0x89090003,
                               0x99090000, 0x99090001, 0x99090002, 0x99090003};
  const u32 addresses[] = {0xFFFE0130, 0x1F801060};
  const u32 expected[][8] = {
      {0x88345678, 0xE9885678, 0x01E98878, 0x0001E988,
       0x0001E988, 0x120001E9, 0x12340001, 0x12345600},
      {0x88345678, 0x0B885678, 0x000B8878, 0x00000B88,
       0x00000B88, 0x1200000B, 0x12340000, 0x12345600},
  };
  u32 wrong = 0;
  for (u32 restore = 0; restore < 2; restore++) {
    for (u32 port = 0; port < countof(addresses); port++) {
      for (u32 i = 0; i < countof(instructions); i++) {
        const partial_load_result_t result = run_port_load(mode, fastmem, linking, instructions[i],
                                                            addresses[port], restore, false);
        wrong += result.value != expected[port][i];
      }
    }
  }
  printf("partial-load word registers cpu=%u fastmem=%u linking=%u wrong_values=%u\n",
         mode, fastmem, linking, wrong);
  EXPECT_EQ(wrong, 0);
}

PARTIAL_DEVICE_MODES(CpuPartialLoadWordRegisters, check_word_register_loads)
