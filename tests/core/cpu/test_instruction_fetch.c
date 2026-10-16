/* Instruction-bus responses and pipeline exception boundaries. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/spu/spu.h"

#define FETCH_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const u32 fetch_segments[] = {0, 0x80000000, 0xA0000000};
static const u32 fetch_program[]  = {
    0x0100F809, /* jalr ra,t0 */
    0x26310001, /* addiu s1,s1,1: branch delay */
    0x26520001, /* addiu s2,s2,1: return continuation */
    0x1000FFFF, 0,
};

static u32                       device_fetch_reads;
static bus_memory_read_handler_t original_device_fetch;

static u32 count_device_fetch(u32 address)
{
  device_fetch_reads++;
  return original_device_fetch(address);
}

static void install_fetch_handler(void)
{
  const u32 handler[] = {0x26100001, 0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.cop0_regs.sr.BEV   = 0;
  g_cpu_state.cop0_regs.BadVaddr = 0x12345678;
}

static void check_fetch_fault(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, u32 physical)
{
  for (size_t segment = 0; segment < FETCH_COUNT(fetch_segments); segment++) {
    const u32 prefix = fetch_segments[segment];
    const u32 target = prefix | physical;
    start_cpu_with_fastmem(mode, fastmem, prefix | 0x10020, fetch_program, FETCH_COUNT(fetch_program));
    install_fetch_handler();
    g_settings.cpu_recompiler_icache = true;
    g_cpu_state.regs.r[8]            = target;
    if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    run_slice(300);
    EXPECT_EQ(g_cpu_state.regs.r[17], 1);
    EXPECT_EQ(g_cpu_state.regs.r[18], 0);
    EXPECT_EQ(g_cpu_state.regs.r[31], prefix | 0x10028);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_IBE);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 0);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, target);
    EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
    if (segment != 2) EXPECT_FALSE(cpu_compare_icache_tag(target));
    finish_cpu();
  }
}

static void check_fetch_device(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, u32 physical)
{
  for (size_t segment = 0; segment < FETCH_COUNT(fetch_segments); segment++) {
    const u32 prefix = fetch_segments[segment];
    const u32 target = prefix | physical;
    start_cpu_with_fastmem(mode, fastmem, prefix | 0x10020, fetch_program, FETCH_COUNT(fetch_program));
    install_fetch_handler();
    dma_initialize();
    spu_initialize();
    g_settings.cpu_recompiler_icache = true;
    g_cpu_state.regs.r[8]            = target;
    cpu_get_memory_write_handler(target, MEMORY_ACCESS_SIZE_WORD)(target, 0x03E00008); /* jr ra */
    cpu_get_memory_write_handler(target + 4, MEMORY_ACCESS_SIZE_WORD)(target + 4, 0);
    EXPECT_EQ(cpu_get_memory_read_handler(target, MEMORY_ACCESS_SIZE_WORD)(target), 0x03E00008);
    bus_memory_read_handler_t* handlers =
        bus_offset_read_handler_array(bus_get_memory_handlers(false, false), MEMORY_ACCESS_SIZE_WORD);
    const u32 page        = 0x1F801000 >> BUS_MEMORY_LUT_PAGE_SHIFT;
    original_device_fetch = handlers[page];
    handlers[page]        = count_device_fetch;
    device_fetch_reads    = 0;
    if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    run_slice(300);
    handlers[page] = original_device_fetch;
    EXPECT_EQ(device_fetch_reads, physical == 0x40000 ? 0 : (segment == 2 ? 2 : 4 - ((target >> 2) & 3)));
    EXPECT_EQ(g_cpu_state.regs.r[16], 0);
    EXPECT_EQ(g_cpu_state.regs.r[17], 1);
    EXPECT_EQ(g_cpu_state.regs.r[18], 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
    EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
    spu_shutdown();
    dma_shutdown();
    finish_cpu();
  }
}

static void check_fetch_delay_slot(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  for (size_t segment = 0; segment < FETCH_COUNT(fetch_segments); segment++) {
    const u32 prefix = fetch_segments[segment];
    const u32 loop[] = {0x1000FFFF, 0};
    start_cpu_with_fastmem(mode, fastmem, prefix | 0x10020, loop, FETCH_COUNT(loop));
    install_fetch_handler();
    const u32 branch = 0x0C004008; /* jal 10020h at the end of the RAM mirrors */
    memcpy(bus_get_unprotected_ram_pointer() + bus_get_ram_mask() - 3, &branch, sizeof(branch));
    g_settings.cpu_recompiler_icache = true;
    cpu_set_pc(prefix | 0x007FFFFC);
    run_slice(300);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    EXPECT_EQ(g_cpu_state.regs.r[31], prefix | 0x00800004);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_IBE);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, prefix | 0x007FFFFC);
    EXPECT_EQ(g_cpu_state.cop0_regs.TAR, prefix | 0x10020);
    EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
    finish_cpu();
  }
}

static void check_fetch_alignment(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  for (size_t segment = 0; segment < FETCH_COUNT(fetch_segments); segment++) {
    const u32 prefix = fetch_segments[segment];
    start_cpu_with_fastmem(mode, fastmem, prefix | 0x10020, fetch_program, FETCH_COUNT(fetch_program));
    install_fetch_handler();
    g_settings.cpu_recompiler_icache = true;
    g_cpu_state.regs.r[8]            = prefix | 0x20002;
    run_slice(300);
    EXPECT_EQ(g_cpu_state.regs.r[17], 1);
    EXPECT_EQ(g_cpu_state.regs.r[18], 0);
    EXPECT_EQ(g_cpu_state.regs.r[31], prefix | 0x10028);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_ADEL);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 0);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, prefix | 0x20002);
    EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, prefix | 0x20002);
    finish_cpu();
  }
}

static void check_compiled_alignment(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  const u32 words[] = {0x3C080002, 0x35080002, 0x0100F809, 0x26310001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, fastmem, 0x10020, words, FETCH_COUNT(words));
  install_fetch_handler();
  const u32 aligned[] = {0x26730001, 0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x20000, aligned, sizeof(aligned));
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) cpu_code_cache_compile_or_revalidate_block(0x20000);
  run_slice(300);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  EXPECT_EQ(g_cpu_state.regs.r[19], 0);
  EXPECT_EQ(g_cpu_state.regs.r[31], 0x10030);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_ADEL);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x20002);
  EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x20002);
  finish_cpu();
}

static void check_delay_exception_priority(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  const u32 words[] = {0x0100F809, 0x0000000C, 0x1000FFFF, 0}; /* jalr; syscall */
  start_cpu_with_fastmem(mode, fastmem, 0x10020, words, FETCH_COUNT(words));
  install_fetch_handler();
  g_cpu_state.regs.r[8] = 0x20002;
  run_slice(300);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  EXPECT_EQ(g_cpu_state.regs.r[31], 0x10028);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_SYSCALL);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, 1);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, 1);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x10020);
  EXPECT_EQ(g_cpu_state.cop0_regs.TAR, 0x20002);
  EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
  finish_cpu();
}

static void install_cached_return(u32 target)
{
  const u32 line                                                = cpu_get_icache_line(target);
  g_cpu_state.icache_tags[line]                                 = cpu_get_icache_tag_for_address(target);
  g_cpu_state.icache_data[line * CPU_ICACHE_WORDS_PER_LINE]     = 0x03E00008;
  g_cpu_state.icache_data[line * CPU_ICACHE_WORDS_PER_LINE + 1] = 0;
}

static void check_cached_fetch(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  for (size_t segment = 0; segment < 2; segment++) {
    const u32 prefix = fetch_segments[segment];
    const u32 target = prefix | 0x1F800000;
    start_cpu_with_fastmem(mode, fastmem, prefix | 0x10020, fetch_program, FETCH_COUNT(fetch_program));
    install_fetch_handler();
    g_cpu_state.regs.r[8] = target;
    install_cached_return(target);
    run_slice(300);
    EXPECT_EQ(g_cpu_state.regs.r[16], 0);
    EXPECT_EQ(g_cpu_state.regs.r[17], 1);
    EXPECT_EQ(g_cpu_state.regs.r[18], 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
    finish_cpu();
  }
}

/* Each device region has its own test so one failure cannot hide the others. */
#define FETCH_CASES(name, mode, fastmem)                  \
  TEST(InstructionFetch, name##_scratchpad)               \
  {                                                       \
    check_fetch_fault(mode, fastmem, 0x1F800000);         \
  }                                                       \
  TEST(InstructionFetch, name##_mdec)                     \
  {                                                       \
    check_fetch_fault(mode, fastmem, 0x1F801820);         \
  }                                                       \
  TEST(InstructionFetch, name##_interrupts)               \
  {                                                       \
    check_fetch_fault(mode, fastmem, 0x1F801070);         \
  }                                                       \
  TEST(InstructionFetch, name##_ram)                      \
  {                                                       \
    check_fetch_device(mode, fastmem, 0x00040000);        \
  }                                                       \
  TEST(InstructionFetch, name##_spu)                      \
  {                                                       \
    check_fetch_device(mode, fastmem, 0x1F801C00);        \
  }                                                       \
  TEST(InstructionFetch, name##_dma_channel)              \
  {                                                       \
    check_fetch_device(mode, fastmem, 0x1F801084);        \
  }                                                       \
  TEST(InstructionFetch, name##_dma_control)              \
  {                                                       \
    check_fetch_device(mode, fastmem, 0x1F8010F0);        \
  }                                                       \
  TEST(InstructionFetch, name##_faulting_delay_slot)      \
  {                                                       \
    check_fetch_delay_slot(mode, fastmem);                \
  }                                                       \
  TEST(InstructionFetch, name##_unaligned_target)         \
  {                                                       \
    check_fetch_alignment(mode, fastmem);                 \
  }                                                       \
  TEST(InstructionFetch, name##_cached_word_needs_no_bus) \
  {                                                       \
    check_cached_fetch(mode, fastmem);                    \
  }

FETCH_CASES(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED)
FETCH_CASES(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED)
FETCH_CASES(native, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED)
FETCH_CASES(mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP)

#define FETCH_CONTEXT_CASES(name, mode, fastmem)                       \
  TEST(InstructionFetch, name##_unaligned_compiled_lut_slot)           \
  {                                                                    \
    check_compiled_alignment(mode, fastmem);                           \
  }                                                                    \
  TEST(InstructionFetch, name##_delay_exception_precedes_target_fault) \
  {                                                                    \
    check_delay_exception_priority(mode, fastmem);                     \
  }

FETCH_CONTEXT_CASES(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED)
FETCH_CONTEXT_CASES(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED)
FETCH_CONTEXT_CASES(native, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED)
FETCH_CONTEXT_CASES(mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP)

static void check_queued_fetch_fault(bool restore)
{
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x10020, fetch_program, FETCH_COUNT(fetch_program));
  install_fetch_handler();
  g_cpu_state.regs.r[8] = 0x1F800000;
  cpu_core_execute_interpreter_instruction(); /* branch */
  cpu_core_execute_interpreter_instruction(); /* delay slot and failed target prefetch */
  EXPECT_EQ(g_cpu_state.pc, 0x1F800000);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  EXPECT_FALSE(g_cpu_state.exception_raised);
  if (restore) {
    u8              data[16384];
    state_wrapper_t sw;
    state_wrapper_init_write(&sw, data, sizeof(data), SAVE_STATE_VERSION);
    EXPECT_TRUE(cpu_do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    const size_t size = state_wrapper_get_position(&sw);
    state_wrapper_init_read(&sw, data, size, SAVE_STATE_VERSION);
    EXPECT_TRUE(cpu_do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), size);
  }
  /* Changing the cache after a failed prefetch cannot repair that queued fetch. */
  install_cached_return(0x1F800000);
  cpu_core_execute_interpreter_instruction();
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_IBE);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x1F800000);
  EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
  cpu_core_execute_interpreter_instruction();
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  finish_cpu();
}

TEST(InstructionFetch, queued_fault_survives_cache_write)
{
  check_queued_fetch_fault(false);
}
TEST(InstructionFetch, queued_fault_survives_state_restore)
{
  check_queued_fetch_fault(true);
}

TEST(InstructionFetch, version_84_state_has_no_pending_fetch_exception)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x10020, loop, FETCH_COUNT(loop));
  install_fetch_handler();
  u8              data[16384];
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, data, sizeof(data), 84);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t size                            = state_wrapper_get_position(&sw);
  g_cpu_state.next_instruction_fetch_exception = CPU_EXCEPTION_IBE;
  state_wrapper_init_read(&sw, data, size, 84);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), size);
  EXPECT_EQ(g_cpu_state.next_instruction_fetch_exception, 0);
  run_slice(100);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
  finish_cpu();
}

TEST(InstructionFetch, state_rejects_invalid_fetch_exception)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x10020, loop, FETCH_COUNT(loop));
  u8              data[16384];
  state_wrapper_t sw;
  g_cpu_state.next_instruction_fetch_exception = 0xFF;
  state_wrapper_init_write(&sw, data, sizeof(data), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  const size_t size = state_wrapper_get_position(&sw);
  state_wrapper_init_read(&sw, data, size, SAVE_STATE_VERSION);
  EXPECT_FALSE(cpu_do_state(&sw));
  finish_cpu();
}

static u32 speculative_fetch_reads;
static u32 count_speculative_fetch(u32 address)
{
  (void)address;
  speculative_fetch_reads++;
  return 0;
}

TEST(InstructionFetch, safe_decoder_does_not_read_device_registers)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x10020, loop, FETCH_COUNT(loop));
  bus_memory_read_handler_t* handlers =
      bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
  const u32                       page     = 0x1F801000 >> BUS_MEMORY_LUT_PAGE_SHIFT;
  const bus_memory_read_handler_t original = handlers[page];
  handlers[page]                           = count_speculative_fetch;
  speculative_fetch_reads                  = 0;
  const u32 targets[]                      = {0x1F801C00, 0x1F801084, 0x1F8010F0, 0x1F801820, 0x1F801070};
  const u32 ticks                          = g_cpu_state.pending_ticks;
  for (size_t segment = 0; segment < FETCH_COUNT(fetch_segments); segment++) {
    for (size_t target = 0; target < FETCH_COUNT(targets); target++) {
      u32 word;
      EXPECT_FALSE(cpu_safe_read_instruction(fetch_segments[segment] | targets[target], &word));
    }
  }
  handlers[page] = original;
  EXPECT_EQ(speculative_fetch_reads, 0);
  EXPECT_EQ(g_cpu_state.pending_ticks, ticks);
  finish_cpu();
}
