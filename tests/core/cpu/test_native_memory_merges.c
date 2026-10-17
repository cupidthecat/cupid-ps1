/* cupid-ps1 successful word-merge and load-delay regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static u32 memory_instruction(u32 op, u32 base, u32 target, u32 offset)
{
  return (op << 26) | (base << 21) | (target << 16) | offset;
}

static void check_native_progress(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
    EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
    EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  }
  if (fastmem != CPU_FASTMEM_MODE_DISABLED) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
}

static void check_merge_pairs(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u8 bytes[] = {0x11, 0x82, 0xF3, 0x64, 0xA5, 0x36, 0xC7, 0x58};
  for (u32 offset = 0; offset < 4; offset++) {
    for (u32 reverse = 0; reverse < 2; reverse++) {
      const u32 left_load   = memory_instruction(CPU_OP_LWL, 8, 9, offset + 3);
      const u32 right_load  = memory_instruction(CPU_OP_LWR, 8, 9, offset);
      const u32 left_store  = memory_instruction(CPU_OP_SWL, 11, 9, offset + 3);
      const u32 right_store = memory_instruction(CPU_OP_SWR, 11, 9, offset);
      const u32 words[]     = {
          reverse ? right_load : left_load,   reverse ? left_load : right_load,   0,          0x01208021,
          reverse ? right_store : left_store, reverse ? left_store : right_store, 0x1000FFFF, 0};
      start_cpu_with_fastmem(mode, fastmem, 0x10000, words, 8);
      g_settings.cpu_recompiler_block_linking = linking;
      g_cpu_state.regs.r[8]                   = 0xA0030000;
      g_cpu_state.regs.r[9]                   = 0xDEADBEEF;
      g_cpu_state.regs.r[11]                  = 0xA0031000;
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, bytes, sizeof(bytes));
      memset(bus_get_unprotected_ram_pointer() + 0x31000, 0xCD, 8);
      run_slice(1000);
      u32 expected;
      memcpy(&expected, bytes + offset, sizeof(expected));
      EXPECT_EQ(g_cpu_state.regs.r[9], expected);
      EXPECT_EQ(g_cpu_state.regs.r[16], expected);
      for (u32 i = 0; i < 8; i++) {
        const u8 expected_byte = i >= offset && i < offset + 4 ? bytes[i] : 0xCD;
        EXPECT_EQ(bus_get_unprotected_ram_pointer()[0x31000 + i], expected_byte);
      }
      check_native_progress(mode, fastmem);
      finish_cpu();
    }
  }
}

static void check_pending_base(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 right = 0; right < 2; right++) {
    for (u32 target = 8; target < 10; target++) {
      const u32 merge   = memory_instruction(right ? CPU_OP_LWR : CPU_OP_LWL, 8, target, right ? 0 : 3);
      const u32 capture = (target << 21) | (16u << 11) | 0x21;
      const u32 words[] = {0x8D680000, merge, 0, capture, 0x1000FFFF, 0};
      start_cpu_with_fastmem(mode, fastmem, 0x10000, words, 6);
      g_settings.cpu_recompiler_block_linking = linking;
      g_cpu_state.regs.r[8]                   = 0xA0030000;
      g_cpu_state.regs.r[9]                   = 0xDEADBEEF;
      g_cpu_state.regs.r[11]                  = 0xA0032000;
      const u32 old_data = 0x77665544, new_data = 0x99887766, new_base = 0xA0031000;
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &old_data, sizeof(old_data));
      memcpy(bus_get_unprotected_ram_pointer() + 0x31000, &new_data, sizeof(new_data));
      memcpy(bus_get_unprotected_ram_pointer() + 0x32000, &new_base, sizeof(new_base));
      run_slice(1000);
      EXPECT_EQ(g_cpu_state.regs.r[target], old_data);
      EXPECT_EQ(g_cpu_state.regs.r[16], old_data);
      if (target != 8) EXPECT_EQ(g_cpu_state.regs.r[8], new_base);
      check_native_progress(mode, fastmem);
      finish_cpu();
    }
  }
}

#define MERGE_CASES(name, check)                                                   \
  TEST(NativeMemoryMerge, name##_interpreter)                                      \
  {                                                                                \
    check(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);        \
  }                                                                                \
  TEST(NativeMemoryMerge, name##_cached)                                           \
  {                                                                                \
    check(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); \
  }                                                                                \
  TEST(NativeMemoryMerge, name##_native)                                           \
  {                                                                                \
    check(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);         \
  }                                                                                \
  TEST(NativeMemoryMerge, name##_lut)                                              \
  {                                                                                \
    check(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true);              \
  }                                                                                \
  TEST(NativeMemoryMerge, name##_mmap)                                             \
  {                                                                                \
    check(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);             \
  }                                                                                \
  TEST(NativeMemoryMerge, name##_unlinked)                                         \
  {                                                                                \
    check(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);        \
  }

MERGE_CASES(pairs, check_merge_pairs)
MERGE_CASES(pending_base, check_pending_base)
