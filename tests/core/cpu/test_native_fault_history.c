/* cupid-ps1 native fault-history regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "util/platform/page_fault_handler.h"

#include <signal.h>

static volatile sig_atomic_t faults_seen, faults_recovered, fault_guest_pc;
static uintptr_t             expected_fault_address;
static bool                  expected_write;
static u32                   device_reads, device_writes;

static u32 read_fault_device(u32 address)
{
  EXPECT_EQ(address, 0x1F802000);
  device_reads++;
  return 0x13572468;
}

static void write_fault_device(u32 address, u32 value)
{
  EXPECT_EQ(address, 0x1F802000);
  EXPECT_EQ(value, 0x13572468);
  device_writes++;
}

static page_fault_handler_result_t observe_native_fault(void* exception_pc, void* fault_address, bool is_write)
{
  const bool observed = (uintptr_t)fault_address == expected_fault_address && is_write == expected_write;
  if (observed) {
    faults_seen++;
    fault_guest_pc = (sig_atomic_t)g_cpu_state.current_instruction_pc;
  }
  const page_fault_handler_result_t result =
      cpu_code_cache_handle_fastmem_exception(exception_pc, fault_address, is_write);
  if (observed && result == PAGE_FAULT_HANDLER_CONTINUE_EXECUTION) faults_recovered++;
  return result;
}

static void check_zero_pc_fault(cpu_fastmem_mode_t fastmem, bool linking, bool store)
{
  /* First use a RAM base, then revisit the same access with a device base. */
  const u32 words[] = {store ? 0xAD280000 : 0x8D280000, 0x26100001, 0x8D490000, 0, 0x08000000, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, fastmem, 0, words, 6);
  g_settings.cpu_recompiler_block_linking = linking;
  g_cpu_state.regs.r[8]                   = 0x13572468;
  g_cpu_state.regs.r[9]                   = 0xA0030000;
  g_cpu_state.regs.r[10]                  = 0xA0030010;
  const u32 ram_value = 0x89ABCDEF, device = 0x1F802000;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &ram_value, sizeof(ram_value));
  memcpy(bus_get_unprotected_ram_pointer() + 0x30010, &device, sizeof(device));
  const u32                  page = device >> BUS_MEMORY_LUT_PAGE_SHIFT;
  bus_memory_read_handler_t* reads =
      bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
  bus_memory_write_handler_t* writes =
      bus_offset_write_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
  const bus_memory_read_handler_t  saved_read  = reads[page];
  const bus_memory_write_handler_t saved_write = writes[page];
  reads[page]                                  = read_fault_device;
  writes[page]                                 = write_fault_device;
  /* LUT holes target null; mmap holes stay in the reserved arena. */
  expected_fault_address = fastmem == CPU_FASTMEM_MODE_MMAP ? (uintptr_t)bus_get_fastmem_base(false) + device : 0;
  expected_write         = store;
  device_reads = device_writes = 0;
  faults_seen = faults_recovered = 0;
  fault_guest_pc                 = -1;
  EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
  EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(0));
  page_fault_handler_set_callback(observe_native_fault);
  run_slice(1000);
  page_fault_handler_set_callback(cpu_code_cache_handle_fastmem_exception);
  reads[page]  = saved_read;
  writes[page] = saved_write;

  EXPECT_TRUE((store ? device_writes : device_reads) > 1);
  EXPECT_EQ(store ? device_reads : device_writes, 0);
  EXPECT_TRUE(faults_seen > 0);
  EXPECT_EQ(faults_recovered, faults_seen);
  EXPECT_EQ(fault_guest_pc, 0);
  EXPECT_EQ(g_cpu_state.regs.r[8], 0x13572468);
  EXPECT_TRUE(g_cpu_state.regs.r[16] > 2);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 10);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  EXPECT_TRUE(cpu_code_cache_has_previously_faulted_on_pc(0));
  EXPECT_EQ(faults_seen, 1);
  EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(4));
  u32 actual;
  memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(actual));
  EXPECT_EQ(actual, store ? 0x13572468 : ram_value);
  if (fastmem == CPU_FASTMEM_MODE_LUT) EXPECT_TRUE(cpu_execution_stats_get()->native_lut_loads > 0);
  cpu_code_cache_reset();
  EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(0));
  finish_cpu();
}

TEST(NativeFaultHistory, lut_load_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_LUT, true, false);
}

TEST(NativeFaultHistory, lut_store_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_LUT, true, true);
}

TEST(NativeFaultHistory, mmap_load_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_MMAP, true, false);
}

TEST(NativeFaultHistory, mmap_store_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_MMAP, true, true);
}

TEST(NativeFaultHistory, unlinked_lut_load_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_LUT, false, false);
}

TEST(NativeFaultHistory, unlinked_lut_store_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_LUT, false, true);
}

TEST(NativeFaultHistory, unlinked_mmap_load_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_MMAP, false, false);
}

TEST(NativeFaultHistory, unlinked_mmap_store_at_zero)
{
  check_zero_pc_fault(CPU_FASTMEM_MODE_MMAP, false, true);
}

static void check_many_fault_sites(cpu_fastmem_mode_t fastmem, u32 entry)
{
  const u32 sites = 5000;
  const u32 count = sites * 4 + 3;
  u32*      words = malloc(count * sizeof(*words));
  EXPECT_NOT_NULL(words);
  for (u32 i = 0; i < sites; i++) {
    words[i * 4]     = 0x8D280000;
    words[i * 4 + 1] = 0x26100001;
    words[i * 4 + 2] = 0x08000000 | ((entry + (i + 1) * 16) >> 2);
    words[i * 4 + 3] = 0;
  }
  words[sites * 4]     = 0x24110001;
  words[sites * 4 + 1] = 0x1000FFFF;
  words[sites * 4 + 2] = 0;
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_RECOMPILER, fastmem, entry, words, count);
  free(words);
  const u32 device                = 0x1F802000;
  g_cpu_state.regs.r[9]           = device;
  const u32                  page = device >> BUS_MEMORY_LUT_PAGE_SHIFT;
  bus_memory_read_handler_t* reads =
      bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
  const bus_memory_read_handler_t saved_read = reads[page];
  reads[page]                                = read_fault_device;
  expected_fault_address = fastmem == CPU_FASTMEM_MODE_MMAP ? (uintptr_t)bus_get_fastmem_base(false) + device : 0;
  faults_seen = faults_recovered = 0;
  expected_write                 = false;
  device_reads                   = 0;
  page_fault_handler_set_callback(observe_native_fault);
  run_slice(500000);
  page_fault_handler_set_callback(cpu_code_cache_handle_fastmem_exception);
  reads[page] = saved_read;
  EXPECT_EQ(device_reads, sites);
  EXPECT_EQ(faults_seen, sites);
  EXPECT_EQ(faults_recovered, sites);
  EXPECT_EQ(g_cpu_state.regs.r[16], sites);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  EXPECT_EQ(g_cpu_state.regs.r[8], 0x13572468);
  EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= sites);
  EXPECT_EQ(fault_guest_pc, entry + (sites - 1) * 16);
  EXPECT_EQ(cpu_code_cache_has_previously_faulted_on_pc(0), entry == 0);
  u32 remembered = 0;
  for (u32 i = 0; i < sites; i++) {
    remembered += cpu_code_cache_has_previously_faulted_on_pc(entry + i * 16);
    EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(entry + i * 16 + 4));
  }
  EXPECT_EQ(remembered, sites);
  cpu_code_cache_reset();
  for (u32 i = 0; i < sites; i++)
    EXPECT_FALSE(cpu_code_cache_has_previously_faulted_on_pc(entry + i * 16));
  finish_cpu();
}

TEST(NativeFaultHistory, lut_retains_thousands_of_fault_sites)
{
  check_many_fault_sites(CPU_FASTMEM_MODE_LUT, 0x10000);
}

TEST(NativeFaultHistory, mmap_retains_thousands_of_fault_sites)
{
  check_many_fault_sites(CPU_FASTMEM_MODE_MMAP, 0x10000);
}

TEST(NativeFaultHistory, lut_retains_zero_across_growth)
{
  check_many_fault_sites(CPU_FASTMEM_MODE_LUT, 0);
}

TEST(NativeFaultHistory, mmap_retains_zero_across_growth)
{
  check_many_fault_sites(CPU_FASTMEM_MODE_MMAP, 0);
}
