/* cupid-ps1 device-bus execution and component-state fixture. */
#ifndef CUPID_TESTS_DEVICE_BUS_EXECUTION_FIXTURE_H
#define CUPID_TESTS_DEVICE_BUS_EXECUTION_FIXTURE_H

#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/spu/spu.h"

static void set_device_control(u32 address, u32 value)
{
  cpu_get_memory_write_handler(address, MEMORY_ACCESS_SIZE_WORD)(address, value);
}

static void restore_device_bus_execution(u32 scratch_pc, u32 control_address, bool with_spu)
{
  u8           cpu[16384], timing[16384];
  const size_t capacity = bus_get_ram_size() + 4096u;
  u8*          bus      = malloc(capacity);
  u8*          spu      = with_spu ? malloc(SPU_RAM_SIZE + 65536u) : NULL;
  EXPECT_NOT_NULL(bus);
  if (with_spu) EXPECT_NOT_NULL(spu);
  const u32 control   = cpu_get_memory_read_handler(control_address, MEMORY_ACCESS_SIZE_WORD)(control_address);
  u32       sound_ram = 0;
  if (with_spu) memcpy(&sound_ram, spu_get_ram(), sizeof(sound_ram));
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, cpu, sizeof(cpu), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t cpu_size = state_wrapper_get_position(&sw);
  state_wrapper_init_write(&sw, bus, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(bus_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t bus_size = state_wrapper_get_position(&sw);
  size_t       spu_size = 0, timing_size = 0;
  if (with_spu) {
    state_wrapper_init_write(&sw, spu, SPU_RAM_SIZE + 65536u, SAVE_STATE_VERSION);
    EXPECT_TRUE(spu_do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    spu_size = state_wrapper_get_position(&sw);
    state_wrapper_init_write(&sw, timing, sizeof(timing), SAVE_STATE_VERSION);
    EXPECT_TRUE(timing_events_do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    timing_size = state_wrapper_get_position(&sw);
  }
  set_device_control(control_address, control ^ 0x2000);
  g_cpu_state.regs.r[16] = 0xDEADC0DE;
  const u32 changed      = 0xDEADBEEF;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &changed, sizeof(changed));
  cpu_set_pc(scratch_pc);
  if (with_spu) {
    spu_write_register(0x180, 0);
    spu_write_register(0x182, 0);
    memcpy(spu_get_writable_ram(), &changed, sizeof(changed));
  }
  state_wrapper_init_read(&sw, cpu, cpu_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), cpu_size);
  state_wrapper_init_read(&sw, bus, bus_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(bus_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), bus_size);
  if (with_spu) {
    state_wrapper_init_read(&sw, spu, spu_size, SAVE_STATE_VERSION);
    EXPECT_TRUE(spu_do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), spu_size);
    state_wrapper_init_read(&sw, timing, timing_size, SAVE_STATE_VERSION);
    EXPECT_TRUE(timing_events_do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), timing_size);
    u32 restored;
    memcpy(&restored, spu_get_ram(), sizeof(restored));
    EXPECT_EQ(restored, sound_ram);
  }
  EXPECT_EQ(cpu_get_memory_read_handler(control_address, MEMORY_ACCESS_SIZE_WORD)(control_address), control);
  cpu_code_cache_reset();
  free(spu);
  free(bus);
}

static void expect_device_bus_path(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem,
                                   const cpu_execution_stats_t* stats)
{
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
    EXPECT_TRUE(stats->native_blocks_entered > 0);
    EXPECT_TRUE(stats->native_blocks_completed > 0);
    EXPECT_EQ(stats->interpreted_instructions, stats->native_dispatch_delay_slots);
    if (fastmem == CPU_FASTMEM_MODE_LUT) {
      EXPECT_TRUE(stats->native_lut_loads > 0);
      EXPECT_TRUE(stats->native_lut_stores > 0);
    }
  }
}

#endif
