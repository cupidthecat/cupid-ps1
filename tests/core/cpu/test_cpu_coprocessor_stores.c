/* Coprocessor word stores must check permissions before address alignment. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static u32 store_device_reads, store_device_writes;

static u32 observe_store_device_read(u32 address)
{
  (void)address;
  store_device_reads++;
  return 0x12345678;
}

static void observe_store_device_write(u32 address, u32 value)
{
  (void)address;
  (void)value;
  store_device_writes++;
}

static void check_store_alignment(u32 cop, cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking,
                                  bool checked)
{
  for (u32 form = 0; form < 2; form++) {
    for (u32 region = 0; region < 2; region++) {
      for (u32 offset = 1; offset < 4; offset++) {
        for (u32 branch = 0; branch < 3; branch++) {
          for (u32 enabled = 0; enabled < 2; enabled++) {
            const u32 address = (region ? 0x1F8010F0 : 0xA0031000) + offset;
            u32       words[12];
            u32       count = 0;
            words[count++]  = 0x01AEA021; /* addu s4,t5,t6 */
            if (form) {
              words[count++] = 0x3C080000 | ((address + 32) >> 16);
              words[count++] = 0x35080000 | ((address + 32) & 0xFFFF);
            }
            words[count++]         = 0x8D6A0000; /* Pending load into t2. */
            const u32 branch_index = count;
            if (branch) words[count++] = 0x11800002;
            const u32 access_index = count;
            words[count++]         = ((CPU_OP_SWC0 + cop) << 26) | (8u << 21) | (9u << 16) | 0xFFE0;
            if (branch) words[count++] = 0x26310001;
            words[count++] = 0x26100001;
            words[count++] = 0x1000FFFF;
            words[count++] = 0;

            start_cpu_with_fastmem(mode, fastmem, 0x00010000, words, count);
            g_settings.cpu_recompiler_block_linking     = linking;
            g_settings.cpu_recompiler_memory_exceptions = checked;
            g_cpu_state.cop0_regs.sr.bits |= 0xF0000000;
            if (!enabled) g_cpu_state.cop0_regs.sr.bits &= ~(1u << (28 + cop));
            g_cpu_state.cop0_regs.sr.BEV   = 0;
            g_cpu_state.cop0_regs.BadVaddr = 0x12345678;
            g_cpu_state.regs.r[8]          = address + 32;
            g_cpu_state.regs.r[9]          = 0x11223344;
            g_cpu_state.regs.r[10]         = 7;
            g_cpu_state.regs.r[11]         = 0xA0030000;
            g_cpu_state.regs.r[12]         = branch == 1 ? 0 : 1;
            g_cpu_state.regs.r[13]         = 1;
            g_cpu_state.regs.r[14]         = 2;
            const u32 pending = 42, canary = 0xAABBCCDD;
            memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &pending, sizeof(pending));
            memcpy(bus_get_unprotected_ram_pointer() + 0x31000, &canary, sizeof(canary));
            const u32 handler[] = {0x01409021, 0x01209821, 0x0280A821, 0x1000FFFF, 0};
            memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));

            const u32                  page = address >> BUS_MEMORY_LUT_PAGE_SHIFT;
            bus_memory_read_handler_t* reads =
                bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
            bus_memory_write_handler_t* writes =
                bus_offset_write_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
            const bus_memory_read_handler_t  old_read  = reads[page];
            const bus_memory_write_handler_t old_write = writes[page];
            if (region) {
              reads[page]  = observe_store_device_read;
              writes[page] = observe_store_device_write;
            }
            store_device_reads = store_device_writes = 0;
            run_slice(1000);
            reads[page]  = old_read;
            writes[page] = old_write;

            EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, enabled ? CPU_EXCEPTION_ADES : CPU_EXCEPTION_CPU);
            EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, cop);
            EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, branch != 0);
            EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, branch == 1);
            EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, enabled ? address : 0x12345678);
            EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0x10000 + (branch ? branch_index : access_index) * 4);
            if (branch) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, 0x10000 + (branch_index + (branch == 1 ? 3 : 2)) * 4);
            EXPECT_EQ(g_cpu_state.regs.r[9], 0x11223344);
            EXPECT_EQ(g_cpu_state.regs.r[10], 42);
            EXPECT_EQ(g_cpu_state.regs.r[16], 0);
            EXPECT_EQ(g_cpu_state.regs.r[17], 0);
            EXPECT_EQ(g_cpu_state.regs.r[18], 42);
            EXPECT_EQ(g_cpu_state.regs.r[19], 0x11223344);
            EXPECT_EQ(g_cpu_state.regs.r[20], 3);
            EXPECT_EQ(g_cpu_state.regs.r[21], 3);
            EXPECT_EQ(store_device_reads, 0);
            EXPECT_EQ(store_device_writes, 0);
            u32 actual;
            memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x31000, sizeof(actual));
            EXPECT_EQ(actual, canary);
            if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
              EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
              EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
              EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(0x10000 + access_index * 4));
              EXPECT_EQ(cpu_code_cache_is_using_fastmem(), fastmem != CPU_FASTMEM_MODE_DISABLED && !checked);
            }
            finish_cpu();
          }
        }
      }
    }
  }
}

#define STORE_ALIGNMENT_CASES(number)                                                                             \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_interpreter)                                                   \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, false);        \
  }                                                                                                               \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_cached)                                                        \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true, false); \
  }                                                                                                               \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_native)                                                        \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true, false);         \
  }                                                                                                               \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_mmap)                                                          \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true, false);             \
  }                                                                                                               \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_lut)                                                           \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true, false);              \
  }                                                                                                               \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_unlinked)                                                      \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false, false);             \
  }                                                                                                               \
  TEST(CpuCoprocessorStoreAlignment, cop##number##_checked)                                                       \
  {                                                                                                               \
    check_store_alignment(number, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true, true);               \
  }

STORE_ALIGNMENT_CASES(0)
STORE_ALIGNMENT_CASES(1)
STORE_ALIGNMENT_CASES(3)
#undef STORE_ALIGNMENT_CASES
