/* cupid-ps1 timer partial stores and read-to-clear status. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/timers/timers.h"

static bus_memory_read_handler_t original_timer_word_read;
static u32                       observed_timer_word_reads;

static u32 observe_timer_word_read(u32 address)
{
  observed_timer_word_reads++;
  return original_timer_word_read(address);
}

static void expect_timer_execution(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem,
                                   const cpu_execution_stats_t* stats)
{
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
    EXPECT_TRUE(stats->native_blocks_completed > 0);
    EXPECT_EQ(stats->interpreted_instructions, stats->native_dispatch_delay_slots);
    if (fastmem == CPU_FASTMEM_MODE_LUT) {
      EXPECT_TRUE(stats->native_lut_loads > 0);
      EXPECT_TRUE(stats->native_lut_stores > 0);
    }
  }
}

static void restore_timer_execution(u32 timer)
{
  u8           cpu[16384], counters[4096], timing[16384];
  const size_t bus_capacity = bus_get_ram_size() + 4096;
  u8*          bus          = malloc(bus_capacity);
  EXPECT_NOT_NULL(bus);
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity;
    size_t size;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {bus_do_state, bus, bus_capacity, 0},
                    {timers_do_state, counters, sizeof(counters), 0},
                    {timing_events_do_state, timing, sizeof(timing), 0}};
  state_wrapper_t state;
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].size = state_wrapper_get_position(&state);
  }

  timers_read_register(timer * 16 + 4);
  EXPECT_EQ(timers_dbg_mode(timer) & 0x1800, 0);
  timers_write_register(timer * 16 + 8, 0x1234);
  g_cpu_state.regs.r[9] = 0;
  memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xCC, sizeof(u32));
  cpu_set_pc(0xA0020000);
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
  cpu_code_cache_reset();
  u32 restored_ram;
  memcpy(&restored_ram, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(restored_ram));
  EXPECT_EQ(restored_ram, 0xDEADC0DE);
  EXPECT_EQ(g_cpu_state.regs.r[9], 0xAABBCCDD);
  EXPECT_EQ(timers_dbg_target(timer), 0xFFFF);
  EXPECT_EQ(timers_dbg_mode(timer) & 0x1800, 0x1800);
  free(bus);
}

static void check_timer_partial_targets(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 baselines[] = {0, 0xFFFFFFFF, 0x11223344};
  const u32 sources[]   = {0xAABBCCDD, 0xFEDCBA98};
  u32       failures = 0, read_failures = 0, cells = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    u32 words[512], inputs[96], expected[48], count = 0, dataset = 0;
    for (u32 left = 0; left < 2; left++) {
      for (u32 offset = 0; offset < 4; offset++) {
        for (u32 pattern = 0; pattern < 3; pattern++) {
          for (u32 source = 0; source < 2; source++) {
            inputs[dataset * 2]     = sources[source];
            inputs[dataset * 2 + 1] = baselines[pattern];
            /* Load both patterns from RAM so the native compiler must use runtime data. */
            words[count++] = 0x8D490000 | (dataset * 8);
            words[count++] = 0x8D4B0000 | (dataset * 8 + 4);
            words[count++] = 0;
            words[count++] = 0xAD0B0000;
            words[count++] = (left ? 0xA9090000 : 0xB9090000) | offset;
            words[count++] = 0x8D0B0000;
            words[count++] = 0;
            words[count++] = 0xAD8B0000 | (dataset * 4);
            expected[dataset] =
                (left ? sources[source] >> ((3 - offset) * 8) : sources[source] << (offset * 8)) & 0xFFFF;
            dataset++;
          }
        }
      }
    }
    words[count++] = 0x26100001;
    words[count++] = 0x1000FFFF;
    words[count++] = 0;
    EXPECT_EQ(dataset, 48);
    EXPECT_TRUE(count <= sizeof(words) / sizeof(words[0]));
    start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, count);
    g_settings.cpu_recompiler_block_linking = linking;
    timers_initialize();
    timers_reset();
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, inputs, sizeof(inputs));
    memset(bus_get_unprotected_ram_pointer() + 0x40000, 0xCC, sizeof(expected));
    g_cpu_state.regs.r[8]  = 0x1F801108 + timer * 16;
    g_cpu_state.regs.r[10] = 0xA0030000;
    g_cpu_state.regs.r[12] = 0xA0040000;
    bus_memory_read_handler_t* handlers =
        bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
    const u32 page            = g_cpu_state.regs.r[8] >> BUS_MEMORY_LUT_PAGE_SHIFT;
    original_timer_word_read  = handlers[page];
    observed_timer_word_reads = 0;
    handlers[page]            = observe_timer_word_read;
    run_slice((tick_count_t)(count * 32 + 1000));
    const u32 reads = observed_timer_word_reads;
    handlers[page]  = original_timer_word_read;
    u32 actual[48];
    memcpy(actual, bus_get_unprotected_ram_pointer() + 0x40000, sizeof(actual));
    const u32                   completed = g_cpu_state.regs.r[16];
    const cpu_execution_stats_t stats     = *cpu_execution_stats_get();
    timers_shutdown();
    finish_cpu();
    expect_timer_execution(mode, fastmem, &stats);
    EXPECT_EQ(completed, 1);
    /* Each case contains one explicit guest readback; the stores add no reads. */
    printf("partial-store-timer-read-count mode=%u fastmem=%u linking=%u timer=%u actual=%u expected=%u\n", mode,
           fastmem, linking, timer, reads, dataset);
    read_failures += reads != dataset;
    for (u32 index = 0; index < dataset; index++) {
      printf("partial-store-timer mode=%u fastmem=%u linking=%u timer=%u case=%u actual=%04X expected=%04X\n", mode,
             fastmem, linking, timer, index, actual[index], expected[index]);
      failures += actual[index] != expected[index];
      cells++;
    }
    fflush(stdout);
  }
  EXPECT_EQ(cells, 144);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(read_failures, 0);
}

static void check_timer_partial_flags(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 words[]  = {0x240B1357, 0xAD4B0000, 0x8D4C0000, 0, 0xA9090000, 0x26100001, 0x1000FFFF, 0};
  u32       failures = 0, unexpected_reads = 0, cells = 0;
  for (u32 timer = 0; timer < 3; timer++) {
    for (u32 restore = 0; restore < 2; restore++) {
      start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, sizeof(words) / sizeof(words[0]));
      g_settings.cpu_recompiler_block_linking = linking;
      timers_initialize();
      timers_reset();
      timers_write_register(timer * 16 + 8, 0xFFFF);
      timers_add_ticks(timer, 0x10000);
      EXPECT_EQ(timers_dbg_mode(timer) & 0x1800, 0x1800);
      g_cpu_state.regs.r[8]  = 0x1F801104 + timer * 16;
      g_cpu_state.regs.r[9]  = 0xAABBCCDD;
      g_cpu_state.regs.r[10] = 0xA0030000;
      const u32 sentinel     = 0xDEADC0DE;
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &sentinel, sizeof(sentinel));
      if (restore) restore_timer_execution(timer);
      bus_memory_read_handler_t* handlers =
          bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
      const u32 page            = g_cpu_state.regs.r[8] >> BUS_MEMORY_LUT_PAGE_SHIFT;
      original_timer_word_read  = handlers[page];
      observed_timer_word_reads = 0;
      handlers[page]            = observe_timer_word_read;
      run_slice(1000);
      const u32 reads                       = observed_timer_word_reads;
      handlers[page]                        = original_timer_word_read;
      const u32                   flags     = timers_dbg_mode(timer) & 0x1800;
      const u32                   completed = g_cpu_state.regs.r[16], loaded = g_cpu_state.regs.r[12];
      const cpu_execution_stats_t stats = *cpu_execution_stats_get();
      timers_shutdown();
      finish_cpu();
      expect_timer_execution(mode, fastmem, &stats);
      EXPECT_EQ(completed, 1);
      EXPECT_EQ(loaded, 0x1357);
      printf("partial-timer-flags mode=%u fastmem=%u linking=%u timer=%u restore=%u flags=%04X "
             "expected=1800 device_reads=%u\n",
             mode, fastmem, linking, timer, restore, flags, reads);
      fflush(stdout);
      failures += flags != 0x1800;
      unexpected_reads += reads;
      cells++;
    }
  }
  EXPECT_EQ(cells, 6);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(unexpected_reads, 0);
}

#define TIMER_PARTIAL_CASE(name, mode, fastmem, linking) \
  TEST(CpuPartialTimerTarget, name)                      \
  {                                                      \
    check_timer_partial_targets(mode, fastmem, linking); \
  }                                                      \
  TEST(CpuPartialTimerFlags, name)                       \
  {                                                      \
    check_timer_partial_flags(mode, fastmem, linking);   \
  }
TIMER_PARTIAL_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
TIMER_PARTIAL_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
TIMER_PARTIAL_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
TIMER_PARTIAL_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
TIMER_PARTIAL_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
TIMER_PARTIAL_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
TIMER_PARTIAL_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
TIMER_PARTIAL_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
