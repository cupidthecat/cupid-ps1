/* Generated programs check coprocessor permissions before visible effects. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static u32 device_reads;
static u32 device_writes;

static u32 read_device(u32 address)
{
  (void)address;
  device_reads++;
  return 0x13572468;
}

static void write_device(u32 address, u32 value)
{
  (void)address;
  (void)value;
  device_writes++;
}

static void check_disabled(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, const u32* operations, size_t count)
{
  for (size_t operation = 0; operation < count; operation++) {
    for (u32 branch = 0; branch < 3; branch++) {
      const u32 cop     = (operations[operation] >> 26) & 3;
      u32       words[] = {operations[operation], 0x26100001, 0x1000FFFF, 0, 0};
      if (branch) {
        words[0] = branch == 1 ? 0x10000003 : 0x14000003; /* Taken or untaken branch. */
        words[1] = operations[operation];
        words[2] = 0x26100001;
        words[3] = 0x1000FFFF;
      }
      start_cpu_with_fastmem(mode, fastmem, 0x00010000, words, 5);
      if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
      const u32 handler[] = {0x1000FFFF, 0};
      memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
      g_cpu_state.cop0_regs.sr.bits |= 0xF0000000;
      g_cpu_state.cop0_regs.sr.bits &= ~(1u << (28 + cop));
      g_cpu_state.cop0_regs.sr.BEV = 0;
      if ((operations[operation] >> 26) == CPU_OP_COP0) g_cpu_state.cop0_regs.sr.KUc = 1;
      g_cpu_state.regs.r[8]           = 0x1F802000;
      g_cpu_state.regs.r[9]           = 0x1234;
      g_cpu_state.load_delay_reg      = CPU_REG_T2;
      g_cpu_state.load_delay_value    = 42;
      const u32            bpc        = g_cpu_state.cop0_regs.BPC;
      const cpu_gte_regs_t gte_before = g_cpu_state.gte_regs;

      const u32                  page = g_cpu_state.regs.r[8] >> BUS_MEMORY_LUT_PAGE_SHIFT;
      bus_memory_read_handler_t* reads =
          bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
      bus_memory_write_handler_t* writes =
          bus_offset_write_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
      const bus_memory_read_handler_t  old_read  = reads[page];
      const bus_memory_write_handler_t old_write = writes[page];
      reads[page]                                = read_device;
      writes[page]                               = write_device;
      device_reads = device_writes = 0;
      run_slice(200);
      reads[page]  = old_read;
      writes[page] = old_write;

      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, cop);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, branch != 0);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, branch == 1);
      EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x00010000);
      if (branch) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, branch == 1 ? 0x00010010 : 0x00010008);
      EXPECT_EQ(g_cpu_state.regs.r[9], 0x1234);
      EXPECT_EQ(g_cpu_state.regs.r[10], 42);
      EXPECT_EQ(g_cpu_state.regs.r[16], 0);
      EXPECT_EQ(g_cpu_state.cop0_regs.BPC, bpc);
      EXPECT_EQ(memcmp(&g_cpu_state.gte_regs, &gte_before, sizeof(gte_before)), 0);
      EXPECT_EQ(device_reads, 0);
      EXPECT_EQ(device_writes, 0);
      if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
        EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
        EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      }
      finish_cpu();
    }
  }
}

static void disabled_register_operations(cpu_execution_mode_t mode)
{
  const u32 operations[] = {0x44090000, 0x44490000, 0x44890000, 0x44C90000, 0x46000000,
                            0x4C090000, 0x4C490000, 0x4C890000, 0x4CC90000, 0x4E000000};
  check_disabled(mode, CPU_FASTMEM_MODE_DISABLED, operations, sizeof(operations) / sizeof(operations[0]));
}

TEST(CpuCoprocessorPermission, interpreter_disabled_register_operations)
{
  disabled_register_operations(CPU_EXECUTION_MODE_INTERPRETER);
}

TEST(CpuCoprocessorPermission, cached_disabled_register_operations)
{
  disabled_register_operations(CPU_EXECUTION_MODE_CACHED_INTERPRETER);
}

TEST(CpuCoprocessorPermission, native_disabled_register_operations)
{
  disabled_register_operations(CPU_EXECUTION_MODE_RECOMPILER);
}

static void disabled_memory_operations(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  const u32 operations[] = {0xC1090000, 0xC5090000, 0xC9090000, 0xCD090000,
                            0xE1090000, 0xE5090000, 0xE9090000, 0xED090000};
  check_disabled(mode, fastmem, operations, sizeof(operations) / sizeof(operations[0]));
}

TEST(CpuCoprocessorPermission, interpreter_disabled_memory_operations)
{
  disabled_memory_operations(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(CpuCoprocessorPermission, cached_disabled_memory_operations)
{
  disabled_memory_operations(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(CpuCoprocessorPermission, native_disabled_memory_operations)
{
  disabled_memory_operations(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(CpuCoprocessorPermission, mmap_disabled_memory_operations)
{
  disabled_memory_operations(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP);
}

TEST(CpuCoprocessorPermission, enabled_register_operations_do_not_trap)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  const u32 words[] = {0x44090000, 0x44490000, 0x44890000, 0x44C90000, 0x46000000, 0x4C090000, 0x4C490000,
                       0x4C890000, 0x4CC90000, 0x4E000000, 0x26100001, 0x1000FFFF, 0};
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
    start_cpu(modes[i], 0x00010000, words, sizeof(words) / sizeof(words[0]));
    g_cpu_state.cop0_regs.sr.bits |= 0xA0000000;
    run_slice(200);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    finish_cpu();
  }
}

TEST(CpuCoprocessorPermission, status_write_disables_register_operations)
{
  for (u32 cop = 1; cop <= 3; cop += 2) {
    const u32 words[] = {0x40806000, 0x40090000 | (cop << 26), 0x26100001, 0x1000FFFF, 0};
    start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x00010000, words, 5);
    const u32 handler[] = {0x1000FFFF, 0};
    memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
    g_cpu_state.cop0_regs.sr.bits |= 0xA0000000;
    run_slice(200);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, cop);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x00010004);
    EXPECT_EQ(g_cpu_state.regs.r[16], 0);
    finish_cpu();
  }
}

TEST(CpuCoprocessorPermission, user_mode_cop0_requires_enable)
{
  const cpu_execution_mode_t modes[]      = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                             CPU_EXECUTION_MODE_RECOMPILER};
  const u32                  operations[] = {0x40097800, 0x40007800, 0x40891800, 0x42000010};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++)
    check_disabled(modes[mode], CPU_FASTMEM_MODE_DISABLED, operations, sizeof(operations) / sizeof(operations[0]));
}

TEST(CpuCoprocessorPermission, native_cop0_kernel_and_enabled_user_reads)
{
  const u32 states[] = {0, 0x10000000, 0x10000002};
  for (size_t state = 0; state < sizeof(states) / sizeof(states[0]); state++) {
    const u32 words[] = {0x40097800, 0, 0x26100001, 0x1000FFFF, 0};
    start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x00010000, words, 5);
    g_cpu_state.cop0_regs.sr.bits = states[state];
    run_slice(200);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
    EXPECT_EQ(g_cpu_state.regs.r[9], g_cpu_state.cop0_regs.PRID);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    finish_cpu();
  }
}

TEST(CpuCoprocessorPermission, native_status_write_enters_user_mode_before_cop0)
{
  const u32 words[] = {0x40896000, 0x400A7800, 0x26100001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x00010000, words, 5);
  const u32 handler[] = {0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.regs.r[9]  = 2;
  g_cpu_state.regs.r[10] = 0x1234;
  run_slice(200);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, 0);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x00010004);
  EXPECT_EQ(g_cpu_state.regs.r[10], 0x1234);
  EXPECT_EQ(g_cpu_state.regs.r[16], 0);
  finish_cpu();
}

TEST(CpuCoprocessorPermission, native_disabled_gte_store_traps_before_busy_wait)
{
  const u32 words[] = {0xE9090000, 0x26100001, 0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_RECOMPILER, 0x00010000, words, 4);
  const u32 handler[] = {0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.regs.r[8]           = 0x00030000;
  g_cpu_state.cop0_regs.sr.BEV    = 0;
  g_cpu_state.cop0_regs.sr.CE2    = 0;
  g_cpu_state.gte_completion_tick = 100000;
  run_slice(1);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_CPU);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, 2);
  EXPECT_TRUE(g_cpu_state.pending_ticks < 100000);
  EXPECT_EQ(g_cpu_state.regs.r[16], 0);
  finish_cpu();
}
