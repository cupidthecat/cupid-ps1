/* Enabled coprocessor loads must issue the read even when no unit consumes it. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"

static u32                       observed_reads;
static bool                      report_bus_failure;
static bus_memory_read_handler_t original_read;

static u32 observe_read(u32 address)
{
  EXPECT_EQ(address, 0x1F8010F0);
  observed_reads++;
  const u32 value = original_read(address);
  if (report_bus_failure) g_cpu_state.bus_error = true;
  return value;
}

static void check_coprocessor_load(u32 cop, u32 fault, cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem,
                                   bool linking)
{
  const u32 entry = 0x00010000;
  for (u32 constant_base = 0; constant_base < 2; constant_base++) {
    for (u32 branch = 0; branch < 3; branch++) {
      const u32 address = 0x1F8010F0 + (fault == 1 ? 1 : 0);
      const u32 base    = address + 32;
      u32       words[16];
      u32       count = 0;
      words[count++]  = 0x01AEA021; /* addu s4,t5,t6: a dirty runtime result. */
      if (constant_base) {
        words[count++] = 0x3C080000 | (base >> 16);
        words[count++] = 0x35080000 | (base & 0xFFFF);
      }
      words[count++]         = 0x8D6A0000; /* lw t2,0(t3): pending at a following load. */
      const u32 branch_index = count;
      if (branch) words[count++] = 0x11800002; /* beq t4,zero,+2 */
      const u32 load_index = count;
      words[count++]       = ((0x30u + cop) << 26) | (8u << 21) | (9u << 16) | 0xFFE0;
      if (branch) words[count++] = 0x26310001; /* Untaken path only. */
      words[count++] = 0x25500001;             /* addiu s0,t2,1 */
      words[count++] = 0x1000FFFF;
      words[count++] = 0;
      start_cpu_with_fastmem(mode, fastmem, entry, words, count);
      dma_initialize();
      dma_reset();
      g_settings.cpu_recompiler_block_linking = linking;
      g_cpu_state.cop0_regs.sr.bits |= 1u << (28u + cop);
      g_cpu_state.cop0_regs.sr.BEV   = 0;
      g_cpu_state.cop0_regs.BadVaddr = 0x12345678;
      g_cpu_state.regs.r[8]          = base;
      g_cpu_state.regs.r[9]          = 0x11223344;
      g_cpu_state.regs.r[10]         = 7;
      g_cpu_state.regs.r[11]         = 0xA0030000;
      g_cpu_state.regs.r[12]         = branch == 1 ? 0 : 1;
      g_cpu_state.regs.r[13]         = 1;
      g_cpu_state.regs.r[14]         = 2;
      const u32 value                = 42;
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &value, sizeof(value));
      const u32 handler[] = {0x1000FFFF, 0};
      memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
      const cpu_gte_regs_t gte_before = g_cpu_state.gte_regs;

      bus_memory_read_handler_t* reads =
          bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
      const u32 page     = address >> BUS_MEMORY_LUT_PAGE_SHIFT;
      original_read      = reads[page];
      reads[page]        = observe_read;
      observed_reads     = 0;
      report_bus_failure = fault == 2;
      run_slice(1000);
      reads[page] = original_read;

      EXPECT_EQ(observed_reads, fault == 1 ? 0 : 1);
      EXPECT_EQ(g_cpu_state.regs.r[9], 0x11223344);
      EXPECT_EQ(g_cpu_state.regs.r[10], 42);
      EXPECT_EQ(g_cpu_state.regs.r[20], 3);
      EXPECT_EQ(memcmp(&g_cpu_state.gte_regs, &gte_before, sizeof(gte_before)), 0);
      if (fault) {
        EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, fault == 1 ? CPU_EXCEPTION_ADEL : CPU_EXCEPTION_DBE);
        EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, branch != 0);
        EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, branch == 1);
        EXPECT_EQ(g_cpu_state.cop0_regs.EPC, entry + (branch ? branch_index : load_index) * 4);
        if (branch) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, entry + (branch_index + (branch == 1 ? 3 : 2)) * 4);
        EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, fault == 1 ? address : 0x12345678);
        EXPECT_EQ(g_cpu_state.regs.r[16], 0);
        EXPECT_EQ(g_cpu_state.regs.r[17], 0);
      } else {
        EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
        EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
        EXPECT_EQ(g_cpu_state.regs.r[16], 43);
        EXPECT_EQ(g_cpu_state.regs.r[17], branch == 2 ? 1 : 0);
      }
      if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
        EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
        EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      }
      if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
      dma_shutdown();
      finish_cpu();
    }
  }
}

#define COP_LOAD_CASES(name, cop, fault)                                                                        \
  TEST(CpuCoprocessorLoad, name##_interpreter)                                                                  \
  {                                                                                                             \
    check_coprocessor_load(cop, fault, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);        \
  }                                                                                                             \
  TEST(CpuCoprocessorLoad, name##_cached)                                                                       \
  {                                                                                                             \
    check_coprocessor_load(cop, fault, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); \
  }                                                                                                             \
  TEST(CpuCoprocessorLoad, name##_native)                                                                       \
  {                                                                                                             \
    check_coprocessor_load(cop, fault, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);         \
  }                                                                                                             \
  TEST(CpuCoprocessorLoad, name##_mmap)                                                                         \
  {                                                                                                             \
    check_coprocessor_load(cop, fault, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);             \
  }                                                                                                             \
  TEST(CpuCoprocessorLoad, name##_unlinked)                                                                     \
  {                                                                                                             \
    check_coprocessor_load(cop, fault, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);        \
  }

COP_LOAD_CASES(lwc0_discards_read_value, 0, 0)
COP_LOAD_CASES(lwc1_discards_read_value, 1, 0)
COP_LOAD_CASES(lwc3_discards_read_value, 3, 0)
COP_LOAD_CASES(lwc0_rejects_misalignment, 0, 1)
COP_LOAD_CASES(lwc1_rejects_misalignment, 1, 1)
COP_LOAD_CASES(lwc3_rejects_misalignment, 3, 1)
COP_LOAD_CASES(lwc0_propagates_bus_failure, 0, 2)
COP_LOAD_CASES(lwc1_propagates_bus_failure, 1, 2)
COP_LOAD_CASES(lwc3_propagates_bus_failure, 3, 2)
