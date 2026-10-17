/* cupid-ps1 native data-access exception regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/gte/gte.h"

enum {
  FAULT_READ,
  FAULT_WRITE,
  FAULT_IO_ALIGNMENT,
  FAULT_RAM_ALIGNMENT,
};

static bus_memory_read_handler_t  saved_read[3];
static bus_memory_write_handler_t saved_write[3];
static u32                        reads_observed, writes_observed;
static bool                       fail_read, fail_write;

static u32 observe_read(u32 size, u32 address)
{
  reads_observed++;
  const u32 value = saved_read[size](address);
  if (fail_read) g_cpu_state.bus_error = true;
  return value;
}

static void observe_write(u32 size, u32 address, u32 value)
{
  writes_observed++;
  if (fail_write)
    g_cpu_state.bus_error = true;
  else
    saved_write[size](address, value);
}

static u32 read_byte(u32 address)
{
  return observe_read(0, address);
}
static u32 read_halfword(u32 address)
{
  return observe_read(1, address);
}
static u32 read_word(u32 address)
{
  return observe_read(2, address);
}
static void write_byte(u32 address, u32 value)
{
  observe_write(0, address, value);
}
static void write_halfword(u32 address, u32 value)
{
  observe_write(1, address, value);
}
static void write_word(u32 address, u32 value)
{
  observe_write(2, address, value);
}

static void check_memory_fault(u32 op, u32 fault, cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const bool store = op == CPU_OP_SB || op == CPU_OP_SH || op == CPU_OP_SW || op == CPU_OP_SWL || op == CPU_OP_SWR ||
                     op == CPU_OP_SWC2;
  const bool merge     = op == CPU_OP_LWL || op == CPU_OP_LWR || op == CPU_OP_SWL || op == CPU_OP_SWR;
  const bool gte       = op == CPU_OP_LWC2 || op == CPU_OP_SWC2;
  const bool alignment = fault >= FAULT_IO_ALIGNMENT;
  const u32  address   = fault == FAULT_RAM_ALIGNMENT ? 0xA0031001 : (0x1F8010F0 + (alignment ? 1 : 0));
  const u32  entry     = 0x00010000;
  for (u32 form = 0; form < 4; form++) {
    const bool constant = (form & 1) != 0;
    const u32  target   = form < 2 ? 9 : 0;
    for (u32 branch = 0; branch < 3; branch++) {
      u32 words[16];
      u32 count      = 0;
      words[count++] = 0x01AEA021; /* addu s4,t5,t6 */
      if (constant) {
        words[count++] = 0x3C080000 | ((address + 32) >> 16);
        words[count++] = 0x35080000 | ((address + 32) & 0xFFFF);
      }
      words[count++]         = 0x8D6A0000; /* Pending load into t2. */
      const u32 branch_index = count;
      if (branch) words[count++] = 0x11800002;
      const u32 access_index = count;
      words[count++]         = (op << 26) | (8u << 21) | (target << 16) | 0xFFE0;
      if (branch) words[count++] = 0x26310001;
      words[count++] = 0x26100001; /* Must not execute after a fault. */
      words[count++] = 0x1000FFFF;
      words[count++] = 0;
      start_cpu_with_fastmem(mode, fastmem, entry, words, count);
      dma_initialize();
      dma_reset();
      g_settings.cpu_recompiler_block_linking     = linking;
      g_settings.cpu_recompiler_memory_exceptions = false;
      g_cpu_state.cop0_regs.sr.CE2                = 1;
      g_cpu_state.cop0_regs.sr.BEV                = 0;
      g_cpu_state.cop0_regs.BadVaddr              = 0x12345678;
      g_cpu_state.regs.r[8]                       = address + 32;
      g_cpu_state.regs.r[9]                       = 0x11223344;
      g_cpu_state.regs.r[10]                      = 7;
      g_cpu_state.regs.r[11]                      = 0xA0030000;
      g_cpu_state.regs.r[12]                      = branch == 1 ? 0 : 1;
      g_cpu_state.regs.r[13]                      = 1;
      g_cpu_state.regs.r[14]                      = 2;
      const u32 pending = 42, canary = 0xAABBCCDD;
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &pending, sizeof(pending));
      memcpy(bus_get_unprotected_ram_pointer() + 0x31000, &canary, sizeof(canary));
      const u32 handler[] = {0x01409021, 0x01209821, 0x0280A821, 0x1000FFFF, 0};
      memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
      gte_write_register(0, 0x55667788);
      gte_write_register(9, 0x1234);
      const cpu_gte_regs_t before = g_cpu_state.gte_regs;

      const u32                        page           = address >> BUS_MEMORY_LUT_PAGE_SHIFT;
      const bus_memory_read_handler_t  read_hooks[3]  = {read_byte, read_halfword, read_word};
      const bus_memory_write_handler_t write_hooks[3] = {write_byte, write_halfword, write_word};
      for (u32 width = 0; width < 3; width++) {
        bus_memory_read_handler_t* reads =
            bus_offset_read_handler_array(g_cpu_state.memory_handlers, (memory_access_size_t)width);
        bus_memory_write_handler_t* writes =
            bus_offset_write_handler_array(g_cpu_state.memory_handlers, (memory_access_size_t)width);
        saved_read[width]  = reads[page];
        saved_write[width] = writes[page];
        if (fault != FAULT_RAM_ALIGNMENT) {
          reads[page]  = read_hooks[width];
          writes[page] = write_hooks[width];
        }
      }
      reads_observed = writes_observed = 0;
      fail_read                        = fault == FAULT_READ;
      fail_write                       = fault == FAULT_WRITE;
      run_slice(1000);
      for (u32 width = 0; width < 3; width++) {
        bus_offset_read_handler_array(g_cpu_state.memory_handlers, (memory_access_size_t)width)[page] =
            saved_read[width];
        bus_offset_write_handler_array(g_cpu_state.memory_handlers, (memory_access_size_t)width)[page] =
            saved_write[width];
      }

      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode,
                alignment ? (store ? CPU_EXCEPTION_ADES : CPU_EXCEPTION_ADEL) : CPU_EXCEPTION_DBE);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, branch != 0);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, branch == 1);
      EXPECT_EQ(g_cpu_state.cop0_regs.EPC, entry + (branch ? branch_index : access_index) * 4);
      if (branch) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, entry + (branch_index + (branch == 1 ? 3 : 2)) * 4);
      EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, alignment ? address : 0x12345678);
      EXPECT_EQ(g_cpu_state.regs.r[0], 0);
      EXPECT_EQ(g_cpu_state.regs.r[9], 0x11223344);
      EXPECT_EQ(g_cpu_state.regs.r[10], 42);
      EXPECT_EQ(g_cpu_state.regs.r[16], 0);
      EXPECT_EQ(g_cpu_state.regs.r[17], 0);
      EXPECT_EQ(g_cpu_state.regs.r[18], 42);
      EXPECT_EQ(g_cpu_state.regs.r[19], 0x11223344);
      EXPECT_EQ(g_cpu_state.regs.r[20], 3);
      EXPECT_EQ(g_cpu_state.regs.r[21], 3);
      EXPECT_EQ(memcmp(&before, &g_cpu_state.gte_regs, sizeof(before)), 0);
      u32 actual;
      memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x31000, sizeof(actual));
      EXPECT_EQ(actual, canary);
      if (alignment) {
        EXPECT_EQ(reads_observed, 0);
        EXPECT_EQ(writes_observed, 0);
      } else if (fault == FAULT_READ) {
        EXPECT_EQ(reads_observed, 1);
        EXPECT_EQ(writes_observed, 0);
      } else {
        EXPECT_EQ(writes_observed, 1);
      }
      if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
        EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
        EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
        if (fastmem == CPU_FASTMEM_MODE_MMAP && !constant && !alignment && !merge && !gte)
          EXPECT_TRUE(cpu_code_cache_has_previously_faulted_on_pc(entry + access_index * 4));
      }
      if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
      dma_shutdown();
      finish_cpu();
    }
  }
}

#define MEMORY_FAULT_CASES(name, op, fault)                                                                \
  TEST(NativeMemoryFault, name##_interpreter)                                                              \
  {                                                                                                        \
    check_memory_fault(op, fault, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);        \
  }                                                                                                        \
  TEST(NativeMemoryFault, name##_cached)                                                                   \
  {                                                                                                        \
    check_memory_fault(op, fault, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); \
  }                                                                                                        \
  TEST(NativeMemoryFault, name##_native)                                                                   \
  {                                                                                                        \
    check_memory_fault(op, fault, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);         \
  }                                                                                                        \
  TEST(NativeMemoryFault, name##_mmap)                                                                     \
  {                                                                                                        \
    check_memory_fault(op, fault, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);             \
  }                                                                                                        \
  TEST(NativeMemoryFault, name##_unlinked)                                                                 \
  {                                                                                                        \
    check_memory_fault(op, fault, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);        \
  }

#define ALIGNMENT_CASES(name, op)                                 \
  MEMORY_FAULT_CASES(name##_io_alignment, op, FAULT_IO_ALIGNMENT) \
  MEMORY_FAULT_CASES(name##_ram_alignment, op, FAULT_RAM_ALIGNMENT)

MEMORY_FAULT_CASES(lb_read_failure, CPU_OP_LB, FAULT_READ)
MEMORY_FAULT_CASES(lbu_read_failure, CPU_OP_LBU, FAULT_READ)
MEMORY_FAULT_CASES(lh_read_failure, CPU_OP_LH, FAULT_READ)
MEMORY_FAULT_CASES(lhu_read_failure, CPU_OP_LHU, FAULT_READ)
MEMORY_FAULT_CASES(lw_read_failure, CPU_OP_LW, FAULT_READ)
MEMORY_FAULT_CASES(lwl_read_failure, CPU_OP_LWL, FAULT_READ)
MEMORY_FAULT_CASES(lwr_read_failure, CPU_OP_LWR, FAULT_READ)
MEMORY_FAULT_CASES(lwc2_read_failure, CPU_OP_LWC2, FAULT_READ)
MEMORY_FAULT_CASES(sb_write_failure, CPU_OP_SB, FAULT_WRITE)
MEMORY_FAULT_CASES(sh_write_failure, CPU_OP_SH, FAULT_WRITE)
MEMORY_FAULT_CASES(sw_write_failure, CPU_OP_SW, FAULT_WRITE)
MEMORY_FAULT_CASES(swl_write_failure, CPU_OP_SWL, FAULT_WRITE)
MEMORY_FAULT_CASES(swr_write_failure, CPU_OP_SWR, FAULT_WRITE)
MEMORY_FAULT_CASES(swc2_write_failure, CPU_OP_SWC2, FAULT_WRITE)
ALIGNMENT_CASES(lh, CPU_OP_LH)
ALIGNMENT_CASES(lhu, CPU_OP_LHU)
ALIGNMENT_CASES(lw, CPU_OP_LW)
ALIGNMENT_CASES(sh, CPU_OP_SH)
ALIGNMENT_CASES(sw, CPU_OP_SW)
ALIGNMENT_CASES(lwc2, CPU_OP_LWC2)
ALIGNMENT_CASES(swc2, CPU_OP_SWC2)

static void check_fault_deadlines(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 memory = 0; memory < 2; memory++) {
    const u32 words[] = {0x01AE001A, 0x4A000030, 0x8D090000, 0x26100001, 0x1000FFFF, 0};
    start_cpu_with_fastmem(mode, fastmem, 0x10000, words, 6);
    g_settings.cpu_recompiler_block_linking     = linking;
    g_settings.cpu_recompiler_memory_exceptions = false;
    g_cpu_state.cop0_regs.sr.CE2                = 1;
    g_cpu_state.cop0_regs.sr.BEV                = 0;
    g_cpu_state.regs.r[8]                       = memory ? 0xA0031001 : 0x1F8010F1;
    g_cpu_state.regs.r[9]                       = 0x1234;
    g_cpu_state.regs.r[13]                      = 12345;
    g_cpu_state.regs.r[14]                      = 7;
    const u32 handler[]                         = {0x1000FFFF, 0};
    memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
    run_to_pc(0x10008);
    const u32 divide = g_cpu_state.muldiv_completion_tick;
    const u32 gte    = g_cpu_state.gte_completion_tick;
    EXPECT_TRUE(divide > g_cpu_state.pending_ticks);
    EXPECT_TRUE(gte > g_cpu_state.pending_ticks);
    const global_ticks_t before = timing_events_get_event_run_tick_counter();
    run_to_pc(0x80000080);
    const global_ticks_t elapsed = timing_events_get_event_run_tick_counter() - before;
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_ADEL);
    EXPECT_EQ(g_cpu_state.regs.r[9], 0x1234);
    EXPECT_EQ(g_cpu_state.regs.r[16], 0);
    EXPECT_EQ(g_cpu_state.load_delay_reg, CPU_REG_COUNT);
    EXPECT_EQ(g_cpu_state.next_load_delay_reg, CPU_REG_COUNT);
    EXPECT_TRUE(g_cpu_state.muldiv_completion_tick > 0);
    EXPECT_TRUE(g_cpu_state.gte_completion_tick > 0);
    EXPECT_EQ(divide, elapsed + g_cpu_state.muldiv_completion_tick);
    EXPECT_EQ(gte, elapsed + g_cpu_state.gte_completion_tick);
    run_slice(1000);
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
      EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
    }
    if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    finish_cpu();
  }
}

TEST(NativeMemoryFault, deadlines_interpreter)
{
  check_fault_deadlines(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativeMemoryFault, deadlines_cached)
{
  check_fault_deadlines(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativeMemoryFault, deadlines_native)
{
  check_fault_deadlines(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(NativeMemoryFault, deadlines_mmap)
{
  check_fault_deadlines(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);
}
TEST(NativeMemoryFault, deadlines_unlinked)
{
  check_fault_deadlines(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);
}
