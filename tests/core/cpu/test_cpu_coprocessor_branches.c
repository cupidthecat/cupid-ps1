/* cupid-ps1 coprocessor branch, permission and pipeline regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gte/gte.h"
#include "core/cpu/cpu_recompiler.h"

enum {
  BRANCH_ENABLED,
  BRANCH_DISABLED,
  BRANCH_SLOT_FAULT,
  BRANCH_GTE_BUSY,
  BRANCH_GTE_FLAG,
  BRANCH_STATUS_ENABLE,
  BRANCH_STATUS_DISABLE,
  BRANCH_BACKWARD
};

static void check_coprocessor_branch(u32 cop, u32 scenario, cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem,
                                     bool linking)
{
  const bool status   = scenario == BRANCH_STATUS_ENABLE || scenario == BRANCH_STATUS_DISABLE;
  const bool backward = scenario == BRANCH_BACKWARD;
  const u32  prefix   = status ? 3 : 0;
  const u32  entry = 0x10000, branch_pc = entry + (prefix + 1) * 4;
  const u32  target = backward ? entry - 32 : entry + (prefix + 8) * 4;
  const u32  end    = entry + (prefix + 12) * 4;
  for (u32 rt = 0; rt < 4; rt++) {
    const bool taken   = (rt & 1) == 0;
    const bool enabled = scenario != BRANCH_DISABLED && scenario != BRANCH_STATUS_DISABLE;
    const u32  branch =
        ((CPU_OP_COP0 + cop) << 26) | (8u << 21) | (rt << 16) | (((target - (branch_pc + 4)) >> 2) & 0xFFFF);
    const u32 words[]     = {0x8D890000,
                             branch,
                         scenario == BRANCH_SLOT_FAULT ? 0x0000000C : 0x8D8B0004,
                             0x01608021,
                             0x01608821,
                             0x24120003,
                             0x08000000 | (end >> 2),
                             0,
                             0x01608021,
                             0x01608821,
                             0x24120005,
                             0,
                             0x1000FFFF,
                             0};
    u32       program[22] = {0};
    if (backward) {
      const u32 head[] = {0x01608021, 0x01608821, 0x24120005, 0x08000000 | (end >> 2), 0};
      memcpy(program, head, sizeof(head));
    } else if (status) {
      program[0] = 0x408E6000; /* mtc0 t6,SR */
    }
    memcpy(program + (backward ? 8 : prefix), words, sizeof(words));
    start_cpu_with_fastmem(mode, fastmem, backward ? target : entry, program,
                           (backward ? 8 : prefix) + sizeof(words) / sizeof(words[0]));
    if (backward) cpu_set_pc(entry);
    g_settings.cpu_recompiler_block_linking = linking;
    g_cpu_state.cop0_regs.sr.bits           = (status ? !enabled : enabled) ? (1u << (28 + cop)) : 0;
    g_cpu_state.regs.r[14]                  = enabled ? (1u << (28 + cop)) : 0;
    g_cpu_state.cop0_regs.BadVaddr          = 0x12345678;
    g_cpu_state.regs.r[9]                   = 0x11223344;
    g_cpu_state.regs.r[11]                  = 7;
    g_cpu_state.regs.r[12]                  = 0xA0030000;
    g_cpu_state.regs.r[31]                  = 0x55667788;
    const u32 data[]                        = {41, 42};
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, data, sizeof(data));
    const u32 handler[] = {0x1000FFFF, 0};
    memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
    if (scenario == BRANCH_GTE_BUSY) g_cpu_state.gte_completion_tick = 5000;
    if (scenario == BRANCH_GTE_FLAG) {
      gte_write_register(63, 0x7FFFF000);
      EXPECT_TRUE((gte_read_register(63) & 0x80000000) != 0);
    }
    const cpu_gte_regs_t before = g_cpu_state.gte_regs;
    run_slice(1000);
    EXPECT_EQ(g_cpu_state.regs.r[9], 41);
    EXPECT_EQ(g_cpu_state.regs.r[31], 0x55667788);
    EXPECT_EQ(g_cpu_state.cop0_regs.BadVaddr, 0x12345678);
    EXPECT_EQ(memcmp(&before, &g_cpu_state.gte_regs, sizeof(before)), 0);
    if (!enabled || scenario == BRANCH_SLOT_FAULT) {
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, enabled ? CPU_EXCEPTION_SYSCALL : CPU_EXCEPTION_CPU);
      EXPECT_EQ(g_cpu_state.cop0_regs.EPC, branch_pc);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BD, enabled);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.BT, enabled && taken);
      if (!enabled) EXPECT_EQ(g_cpu_state.cop0_regs.cause.CE, cop);
      if (enabled) EXPECT_EQ(g_cpu_state.cop0_regs.TAR, taken ? target : branch_pc + 8);
      EXPECT_EQ(g_cpu_state.regs.r[11], 7);
      EXPECT_EQ(g_cpu_state.regs.r[16], 0);
      EXPECT_EQ(g_cpu_state.regs.r[17], 0);
      EXPECT_EQ(g_cpu_state.regs.r[18], 0);
    } else {
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
      EXPECT_EQ(g_cpu_state.regs.r[11], 42);
      EXPECT_EQ(g_cpu_state.regs.r[16], 7);
      EXPECT_EQ(g_cpu_state.regs.r[17], 42);
      EXPECT_EQ(g_cpu_state.regs.r[18], taken ? 5 : 3);
    }
    if (scenario == BRANCH_GTE_BUSY) {
      EXPECT_TRUE(g_cpu_state.gte_completion_tick > 0);
      EXPECT_TRUE(timing_events_get_event_run_tick_counter() < 2000);
    }
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
      EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
    }
    if (fastmem != CPU_FASTMEM_MODE_DISABLED) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    finish_cpu();
  }
}

#define COP_BRANCH_CASES(name, cop, scenario)                                                                        \
  TEST(CpuCoprocessorBranch, name##_interpreter)                                                                     \
  {                                                                                                                  \
    check_coprocessor_branch(cop, scenario, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);        \
  }                                                                                                                  \
  TEST(CpuCoprocessorBranch, name##_cached)                                                                          \
  {                                                                                                                  \
    check_coprocessor_branch(cop, scenario, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true); \
  }                                                                                                                  \
  TEST(CpuCoprocessorBranch, name##_native)                                                                          \
  {                                                                                                                  \
    check_coprocessor_branch(cop, scenario, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);         \
  }                                                                                                                  \
  TEST(CpuCoprocessorBranch, name##_lut)                                                                             \
  {                                                                                                                  \
    check_coprocessor_branch(cop, scenario, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true);              \
  }                                                                                                                  \
  TEST(CpuCoprocessorBranch, name##_mmap)                                                                            \
  {                                                                                                                  \
    check_coprocessor_branch(cop, scenario, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);             \
  }                                                                                                                  \
  TEST(CpuCoprocessorBranch, name##_unlinked)                                                                        \
  {                                                                                                                  \
    check_coprocessor_branch(cop, scenario, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);        \
  }

COP_BRANCH_CASES(cop0_enabled, 0, BRANCH_ENABLED)
COP_BRANCH_CASES(cop1_enabled, 1, BRANCH_ENABLED)
COP_BRANCH_CASES(cop2_enabled, 2, BRANCH_ENABLED)
COP_BRANCH_CASES(cop3_enabled, 3, BRANCH_ENABLED)
COP_BRANCH_CASES(cop0_disabled, 0, BRANCH_DISABLED)
COP_BRANCH_CASES(cop1_disabled, 1, BRANCH_DISABLED)
COP_BRANCH_CASES(cop2_disabled, 2, BRANCH_DISABLED)
COP_BRANCH_CASES(cop3_disabled, 3, BRANCH_DISABLED)
COP_BRANCH_CASES(cop0_slot_fault, 0, BRANCH_SLOT_FAULT)
COP_BRANCH_CASES(cop1_slot_fault, 1, BRANCH_SLOT_FAULT)
COP_BRANCH_CASES(cop2_slot_fault, 2, BRANCH_SLOT_FAULT)
COP_BRANCH_CASES(cop3_slot_fault, 3, BRANCH_SLOT_FAULT)
COP_BRANCH_CASES(cop2_busy, 2, BRANCH_GTE_BUSY)
COP_BRANCH_CASES(cop2_flag, 2, BRANCH_GTE_FLAG)
COP_BRANCH_CASES(cop0_status_enable, 0, BRANCH_STATUS_ENABLE)
COP_BRANCH_CASES(cop1_status_enable, 1, BRANCH_STATUS_ENABLE)
COP_BRANCH_CASES(cop2_status_enable, 2, BRANCH_STATUS_ENABLE)
COP_BRANCH_CASES(cop3_status_enable, 3, BRANCH_STATUS_ENABLE)
COP_BRANCH_CASES(cop0_status_disable, 0, BRANCH_STATUS_DISABLE)
COP_BRANCH_CASES(cop1_status_disable, 1, BRANCH_STATUS_DISABLE)
COP_BRANCH_CASES(cop2_status_disable, 2, BRANCH_STATUS_DISABLE)
COP_BRANCH_CASES(cop3_status_disable, 3, BRANCH_STATUS_DISABLE)
COP_BRANCH_CASES(cop0_backward, 0, BRANCH_BACKWARD)
COP_BRANCH_CASES(cop1_backward, 1, BRANCH_BACKWARD)
COP_BRANCH_CASES(cop2_backward, 2, BRANCH_BACKWARD)
COP_BRANCH_CASES(cop3_backward, 3, BRANCH_BACKWARD)

TEST(CpuCoprocessorBranch, classification)
{
  for (u32 cop = 0; cop < 4; cop++) {
    for (u32 rt = 0; rt < 4; rt++) {
      const cpu_instruction_t branch = {.bits = ((CPU_OP_COP0 + cop) << 26) | (8u << 21) | (rt << 16) | 0xFFFE};
      EXPECT_TRUE(cpu_is_branch_instruction(branch));
      EXPECT_TRUE(cpu_is_direct_branch_instruction(branch));
      EXPECT_EQ(cpu_is_unconditional_branch_instruction(branch), (rt & 1) == 0);
      EXPECT_EQ(cpu_get_direct_branch_target(branch, 0x10000), 0xFFFC);
      const cpu_instruction_t move = {.bits = ((CPU_OP_COP0 + cop) << 26) | (9u << 16) | (12u << 11)};
      EXPECT_TRUE(!cpu_is_branch_instruction(move));
    }
  }
}

static u32 backend_fallback_calls;

static void record_backend_fallback(cpu_recompiler_t* r)
{
  backend_fallback_calls++;
  r->block_ended = true;
}

static void unused_branch_hook(cpu_recompiler_t* r)
{
  (void)r;
}

static void forbid_partial_branch(cpu_recompiler_t* r, u32 value, const void* pointer)
{
  (void)r;
  (void)value;
  (void)pointer;
  test_fail(__FILE__, __LINE__, "incomplete backend emitted a native branch target");
}

TEST(CpuCoprocessorBranch, incomplete_backend_keeps_fallback)
{
  for (u32 missing = 0; missing < 3; missing++) {
    cpu_recompiler_vtable_t hooks = {
        .generate_instruction_fetch    = missing == 1 ? unused_branch_hook : NULL,
        .check_cop_enabled             = missing == 0 ? unused_branch_hook : NULL,
        .compile_fallback              = record_backend_fallback,
        .store_constant_to_cpu_pointer = forbid_partial_branch,
    };
    cpu_instruction_t          branch   = {.bits = 0x41000004};
    cpu_code_cache_block_t     block    = {.pc = 0x10000, .size = 2};
    cpu_code_cache_inst_info_t info     = {0};
    cpu_recompiler_t           compiler = {.v                        = &hooks,
                                           .block                    = &block,
                                           .inst                     = &branch,
                                           .iinfo                    = &info,
                                           .current_instruction_pc   = 0x10000,
                                           .compiler_pc              = 0x10004,
                                           .load_delay_register      = CPU_REG_COUNT,
                                           .next_load_delay_register = CPU_REG_COUNT};
    backend_fallback_calls              = 0;
    cpu_recompiler_compile_instruction(&compiler);
    EXPECT_EQ(backend_fallback_calls, 1);
    EXPECT_TRUE(compiler.block_ended);
    EXPECT_EQ(block.size, 1);
    EXPECT_TRUE(info.is_last_instruction);
  }
}
