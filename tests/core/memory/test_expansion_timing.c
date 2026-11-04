/* cupid-ps1 expansion read timing and control restoration regressions. */
#include "tests/core/memory/device_bus_execution_fixture.h"

static u32 read_expansion(u32 address, memory_access_size_t size, u32 expected_ticks) {
  const u32 before = g_cpu_state.pending_ticks;
  const u32 value  = cpu_get_memory_read_handler(address, size)(address);
  EXPECT_EQ(g_cpu_state.pending_ticks - before, expected_ticks);
  return value;
}

static void check_expansion_widths(const u32 ticks[3]) {
  const u32 aliases[] = {0x1F802060, 0x9F802060, 0xBF802060};
  for (u32 i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    for (u32 size = 0; size < 3; size++) {
      const u32 mask = size == 0 ? 0xFF : size == 1 ? 0xFFFF : 0xFFFFFFFF;
      EXPECT_EQ(read_expansion(aliases[i], (memory_access_size_t)size, ticks[size]) & mask, mask);
    }
  }
}

TEST(ExpansionTiming, reset_and_control_changes) {
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  const u32 defaults[]       = {14, 28, 56};
  const u32 wide[]           = {14, 14, 28};
  const u32 short_delay[]    = {7, 14, 28};
  const u32 common_changed[] = {15, 31, 63};
  check_expansion_widths(defaults);
  set_device_control(0x1F80101C, 0x00071777);
  check_expansion_widths(wide);
  set_device_control(0x1F80101C, 0x00070707);
  check_expansion_widths(short_delay);
  /* The low nibble is write delay; it must not change read timing. */
  set_device_control(0x1F80101C, 0x00070700);
  check_expansion_widths(short_delay);
  set_device_control(0x1F80101C, 0x00070777);
  set_device_control(0x1F801020, 0x00001325);
  check_expansion_widths(common_changed);
  bus_reset();
  check_expansion_widths(defaults);
  finish_cpu();
}

TEST(ExpansionTiming, restore_derived_delays) {
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  set_device_control(0x1F80101C, 0x00070707);
  const size_t capacity = bus_get_ram_size() + 4096;
  u8*          bytes    = malloc(capacity);
  EXPECT_NOT_NULL(bytes);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(bus_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t length = state_wrapper_get_position(&sw);
  /* Older records stored zero EXP2 delays despite nonzero control registers.
   * Keep their layout readable and rebuild the derived table on load. */
  memset(bytes + sizeof(u32) + 3 * sizeof(tick_count_t), 0, 3 * sizeof(tick_count_t));
  bus_reset();
  state_wrapper_init_read(&sw, bytes, length, SAVE_STATE_VERSION);
  EXPECT_TRUE(bus_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), length);
  const u32 restored[] = {7, 14, 28};
  check_expansion_widths(restored);
  free(bytes);
  finish_cpu();
}

static global_ticks_t execution_ticks(void) {
  return timing_events_get_global_tick_counter() + g_cpu_state.pending_ticks;
}

static void check_expansion_cpu(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  for (u32 size = 0; size < 3; size++) {
    for (u32 restore = 0; restore < 2; restore++) {
      global_ticks_t elapsed[2];
      for (u32 slow = 0; slow < 2; slow++) {
        const u32 base    = 0x80010000;
        const u32 loads[] = {0x912B0000, 0x952B0000, 0x8D2B0000};
        const u32 words[] = {0xAD0A001C,                             /* sw t2,DEV8_DELAY(t0) */
                             loads[size], 0, 0xADCB0000, 0x8DCC0000, /* load; delay; RAM store/read */
                             0x1000FFFF,  0};
        start_cpu_with_fastmem(mode, fastmem, base, words, sizeof(words) / sizeof(words[0]));
        g_settings.cpu_recompiler_block_linking = linking;
        g_cpu_state.regs.r[8]                   = 0x1F801000;
        g_cpu_state.regs.r[9]                   = 0xBF802060;
        g_cpu_state.regs.r[10]                  = slow ? 0x00070777 : 0x00070707;
        g_cpu_state.regs.r[14]                  = 0xA0030000;
        run_to_pc(base + 4);
        if (restore) restore_device_bus_execution(base + 0x100, 0x1F80101C, false);
        const global_ticks_t before = execution_ticks();
        run_to_pc(base + 20);
        elapsed[slow] = execution_ticks() - before;
        u32 stored;
        memcpy(&stored, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(stored));
        const cpu_execution_stats_t stats = *cpu_execution_stats_get();
        finish_cpu();
        expect_device_bus_path(mode, fastmem, &stats);
        EXPECT_EQ(stored, size == 0 ? 0xFF : size == 1 ? 0xFFFF : 0xFFFFFFFF);
      }
      const u32 differences[] = {7, 14, 28};
      printf("expansion-timing mode=%u fastmem=%u linking=%u size=%u restore=%u fast=%llu slow=%llu\n", mode, fastmem,
             linking, size, restore, (unsigned long long)elapsed[0], (unsigned long long)elapsed[1]);
      EXPECT_EQ(elapsed[1] - elapsed[0], differences[size]);
    }
  }
}

#define EXPANSION_CPU_CASE(name, mode, fastmem, linking) \
  TEST(ExpansionTimingCPU, name) {                       \
    check_expansion_cpu(mode, fastmem, linking);         \
  }
EXPANSION_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
EXPANSION_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
EXPANSION_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
EXPANSION_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
EXPANSION_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
EXPANSION_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
EXPANSION_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
EXPANSION_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
