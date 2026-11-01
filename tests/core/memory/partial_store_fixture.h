/* cupid-ps1 peripheral partial-store continuation fixtures. */
#ifndef CUPID_TEST_PARTIAL_STORE_FIXTURE_H
#define CUPID_TEST_PARTIAL_STORE_FIXTURE_H
#include "tests/core/memory/device_bus_execution_fixture.h"
#include "core/cdrom/cdrom.h"
#include "core/input/pad.h"
#include "core/serial/sio.h"

static bool partial_pad_state(state_wrapper_t* sw) { return pad_do_state(sw, false); }

static void restore_partial_devices(bool cd)
{
  struct {
    bool (*state)(state_wrapper_t*);
    size_t capacity, size;
    u8* bytes;
  } records[] = {
      {cpu_do_state, 16384, 0, NULL},
      {bus_do_state, bus_get_ram_size() + 4096, 0, NULL},
      {spu_do_state, SPU_RAM_SIZE + 65536, 0, NULL},
      {cd ? cdrom_do_state : partial_pad_state, 65536, 0, NULL},
      {sio_do_state, 256, 0, NULL},
      {timing_events_do_state, 16384, 0, NULL},
  };
  state_wrapper_t sw;
  const global_ticks_t ticks = timing_events_get_global_tick_counter();
  for (u32 i = 0; i < countof(records); i++) {
    records[i].bytes = malloc(records[i].capacity);
    EXPECT_NOT_NULL(records[i].bytes);
    state_wrapper_init_write(&sw, records[i].bytes, records[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(records[i].state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    records[i].size = state_wrapper_get_position(&sw);
  }
  g_cpu_state.regs.r[9] = 0xDEADBEEF;
  g_cpu_state.regs.r[16] = 0xBAD;
  memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xA5, 1024);
  if (cd) cdrom_reset(); else pad_reset();
  sio_reset();
  for (u32 i = 0; i < countof(records); i++) {
    state_wrapper_init_read(&sw, records[i].bytes, records[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(records[i].state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), records[i].size);
    free(records[i].bytes);
  }
  EXPECT_EQ(timing_events_get_global_tick_counter(), ticks);
  cpu_code_cache_reset();
}

static bus_memory_read_handler_t partial_original_read;
static u32 partial_word_reads;
static u32 partial_observe_read(u32 address)
{
  partial_word_reads++;
  return partial_original_read(address);
}

static void partial_constant(u32* words, u32* count, u32 reg, u32 value)
{
  words[(*count)++] = 0x3C000000 | (reg << 16) | (value >> 16);
  words[(*count)++] = 0x34000000 | (reg << 21) | (reg << 16) | (value & 0xFFFF);
}

#define PARTIAL_DEVICE_MODES(group, function) \
  TEST(group, interpreter) { function(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); } \
  TEST(group, cached) { function(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); } \
  TEST(group, native_slow) { function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true); } \
  TEST(group, native_slow_unlinked) { function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false); } \
  TEST(group, native_mmap) { function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true); } \
  TEST(group, native_mmap_unlinked) { function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false); } \
  TEST(group, native_lut) { function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true); } \
  TEST(group, native_lut_unlinked) { function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false); }
#endif
