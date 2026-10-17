/* cupid-ps1 reserved instruction and retained-fetch regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/cpu/cpu_recompiler.h"

static const u8 reserved_primary[] = {
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x27, 0x2C, 0x2D, 0x2F, 0x34, 0x35, 0x36, 0x37, 0x3C, 0x3D, 0x3E, 0x3F,
};
static const u8 reserved_secondary[] = {
    0x01, 0x05, 0x0A, 0x0B, 0x0E, 0x0F, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F, 0x28, 0x29, 0x2C, 0x2D,
    0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
};

static void install_ri_handler(void)
{
  const u32 handler[] = {0x26100001, 0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  g_cpu_state.cop0_regs.sr.BEV   = 0;
  g_cpu_state.cop0_regs.BadVaddr = 0x12345678;
}

static void check_native_ri_progress(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
    EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
    EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
  }
  if (fastmem != CPU_FASTMEM_MODE_DISABLED) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
}

static void check_ri_program(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking, u32 entry,
                             u32 instruction, u32 branch, bool rewrite, bool cache, bool store_cached)
{
  const u32 fault       = branch ? 3 : 2;
  const u32 replacement = 0x24140063; /* addiu s4,zero,99 */
  u32       words[7]    = {0};
  words[0]              = rewrite ? (0xAD280000 | fault * 4) : 0x26B50001;
  words[1]              = 0x8D6A0000; /* lw t2,0(t3) */
  words[2]              = branch ? 0x11800002 : instruction;
  words[3]              = branch ? instruction : 0x26310001;
  words[4]              = branch ? 0x26310001 : 0x1000FFFF;
  words[5]              = branch ? 0x1000FFFF : 0;
  start_cpu_with_fastmem(mode, fastmem, entry, words, 7);
  g_settings.cpu_recompiler_block_linking = linking;
  install_ri_handler();
  if (!cache) cpu_set_cache_control_bits(cpu_get_cache_control_bits() & ~0x800u);
  /* Refresh the initial fetch after selecting the cache configuration. */
  cpu_set_pc(entry);
  g_cpu_state.regs.r[8]  = replacement;
  g_cpu_state.regs.r[9]  = (entry & CPU_KSEG_MASK) | (store_cached ? 0x80000000 : 0xA0000000);
  g_cpu_state.regs.r[10] = 7;
  g_cpu_state.regs.r[11] = 0xA0030000;
  g_cpu_state.regs.r[12] = branch == 1 ? 0 : 1;
  const u32 pending      = 42;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &pending, sizeof(pending));
  run_slice(500);
  const bool trapped = !rewrite || (cache && (entry >> 29) != 5);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, trapped ? CPU_EXCEPTION_RI : 0);
  EXPECT_EQ(g_cpu_state.regs.r[16], trapped ? 1 : 0);
  EXPECT_EQ(g_cpu_state.regs.r[17], trapped || branch == 1 ? 0 : 1);
  EXPECT_EQ(g_cpu_state.regs.r[20], trapped ? 0 : 99);
  EXPECT_EQ(g_cpu_state.regs.r[21], rewrite ? 0 : 1);
  EXPECT_EQ(g_cpu_state.regs.r[10], 42);
  EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
  if (trapped) {
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, entry + (branch ? 8 : fault * 4));
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, branch != 0);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, branch == 1);
    if (branch) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, entry + (branch == 1 ? 20 : 16));
  }
  u32 memory;
  memcpy(&memory, bus_get_unprotected_ram_pointer() + (entry & bus_get_ram_mask()) + fault * 4, sizeof(memory));
  EXPECT_EQ(memory, rewrite ? replacement : instruction);
  if (rewrite && trapped) {
    const u32 address = entry + fault * 4;
    EXPECT_TRUE(cpu_compare_icache_tag(address));
    EXPECT_EQ(g_cpu_state.icache_data[cpu_get_icache_line(address) * 4 + cpu_get_icache_line_word_offset(address)],
              instruction);
  }
  if (mode == CPU_EXECUTION_MODE_RECOMPILER && cpu_execution_stats_get()->interpreted_instructions)
    test_fail(__FILE__, __LINE__, "interpreted during entry=%08X branch=%u rewrite=%d cache=%d store_cached=%d", entry,
              branch, rewrite, cache, store_cached);
  check_native_ri_progress(mode, fastmem);
  finish_cpu();
}

static void check_stale_primary(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 branch = 0; branch < 3; branch++) {
    for (u32 segment = 0; segment < 2; segment++) {
      for (u32 store = 0; store < 2; store++)
        check_ri_program(mode, fastmem, linking, 0x10200 | (segment ? 0x80000000 : 0), 0x74000000, branch, true, true,
                         store != 0);
    }
  }
}

static void check_primary_encodings(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 i = 0; i < sizeof(reserved_primary); i++)
    check_ri_program(mode, fastmem, linking, 0x80010200, ((u32)reserved_primary[i] << 26) | 0x012A5100, i % 3, false,
                     true, false);
}

static void check_secondary_encodings(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 i = 0; i < sizeof(reserved_secondary); i++)
    check_ri_program(mode, fastmem, linking, 0x80010200, 0x01095100 | reserved_secondary[i], i % 3, false, true, false);
}

static void check_bypass(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  for (u32 branch = 0; branch < 3; branch++) {
    check_ri_program(mode, fastmem, linking, 0xA0010200, 0x74000000, branch, true, true, false);
    check_ri_program(mode, fastmem, linking, 0x80010200, 0x74000000, branch, true, false, true);
  }
}

static void check_restored_fetch(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 entry   = 0xA0010200;
  const u32 words[] = {0x74000000, 0x26310001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, fastmem, entry, words, 4);
  g_settings.cpu_recompiler_block_linking = linking;
  install_ri_handler();
  u32 prefetched;
  EXPECT_TRUE(cpu_get_prefetched_instruction(entry, &prefetched));
  EXPECT_EQ(prefetched, words[0]);
  const u32 replacement = 0x24140063;
  memcpy(bus_get_unprotected_ram_pointer() + (entry & bus_get_ram_mask()), &replacement, sizeof(replacement));
  u8              buffer[16384];
  state_wrapper_t writer, reader;
  state_wrapper_init_write(&writer, buffer, sizeof(buffer), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&writer));
  EXPECT_FALSE(state_wrapper_has_error(&writer));
  cpu_set_pc(0xA0040000);
  state_wrapper_init_read(&reader, buffer, state_wrapper_get_position(&writer), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&reader));
  EXPECT_TRUE(cpu_get_prefetched_instruction(entry, &prefetched));
  EXPECT_EQ(prefetched, words[0]);
  run_slice(500);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_RI);
  EXPECT_EQ(g_cpu_state.cop0_regs.EPC, entry);
  EXPECT_EQ(g_cpu_state.regs.r[16], 1);
  EXPECT_EQ(g_cpu_state.regs.r[17], 0);
  EXPECT_EQ(g_cpu_state.regs.r[20], 0);
  check_native_ri_progress(mode, fastmem);

  g_cpu_state.cop0_regs.cause.bits = 0;
  g_cpu_state.regs.r[16]           = 0;
  cpu_set_pc(entry);
  run_slice(500);
  EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
  EXPECT_EQ(g_cpu_state.regs.r[16], 0);
  EXPECT_EQ(g_cpu_state.regs.r[17], 1);
  EXPECT_EQ(g_cpu_state.regs.r[20], 99);
  check_native_ri_progress(mode, fastmem);
  finish_cpu();
}

#define RI_MODES(name, function)                                                      \
  TEST(ReservedInstruction, name##_interpreter)                                       \
  {                                                                                   \
    function(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);        \
  }                                                                                   \
  TEST(ReservedInstruction, name##_cached)                                            \
  {                                                                                   \
    function(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); \
  }                                                                                   \
  TEST(ReservedInstruction, name##_native)                                            \
  {                                                                                   \
    function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);         \
  }                                                                                   \
  TEST(ReservedInstruction, name##_lut)                                               \
  {                                                                                   \
    function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true);              \
  }                                                                                   \
  TEST(ReservedInstruction, name##_mmap)                                              \
  {                                                                                   \
    function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);             \
  }                                                                                   \
  TEST(ReservedInstruction, name##_unlinked)                                          \
  {                                                                                   \
    function(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);        \
  }

RI_MODES(stale_primary, check_stale_primary)
RI_MODES(primary_encodings, check_primary_encodings)
RI_MODES(secondary_encodings, check_secondary_encodings)
RI_MODES(cache_bypass, check_bypass)
RI_MODES(restored_fetch, check_restored_fetch)

static u32  reserved_fallback_calls;
static void reserved_fallback(cpu_recompiler_t* r)
{
  reserved_fallback_calls++;
  r->block_ended = true;
}
static void reserved_fetch_hook(cpu_recompiler_t* r)
{
  (void)r;
}
static void forbid_reserved_exception(cpu_recompiler_t* r, cpu_exception_t code)
{
  (void)r;
  (void)code;
  test_fail(__FILE__, __LINE__, "incomplete backend emitted a reserved exception");
}

TEST(ReservedInstruction, incomplete_backend_keeps_fallback)
{
  for (u32 missing = 0; missing < 2; missing++) {
    cpu_recompiler_vtable_t hooks = {
        .generate_instruction_fetch = missing == 0 ? NULL : reserved_fetch_hook,
        .end_block_with_exception   = missing == 1 ? NULL : forbid_reserved_exception,
        .compile_fallback           = reserved_fallback,
    };
    cpu_instruction_t          instruction = {.bits = 0x74000000};
    cpu_code_cache_block_t     block       = {.pc = 0x10000, .size = 2};
    cpu_code_cache_inst_info_t info        = {0};
    cpu_recompiler_t           compiler    = {.v                        = &hooks,
                                              .block                    = &block,
                                              .inst                     = &instruction,
                                              .iinfo                    = &info,
                                              .current_instruction_pc   = 0x10000,
                                              .compiler_pc              = 0x10004,
                                              .load_delay_register      = CPU_REG_COUNT,
                                              .next_load_delay_register = CPU_REG_COUNT};
    reserved_fallback_calls                = 0;
    cpu_recompiler_compile_instruction(&compiler);
    EXPECT_EQ(reserved_fallback_calls, 1);
    EXPECT_TRUE(compiler.block_ended);
    EXPECT_EQ(block.size, 1);
  }
}
