/* Unsupported guest commands must not terminate the host process. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void unsupported_commands_continue(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  const u32 commands[] = {0x4BE00000, 0x4A000002, 0x4A00003C};
  for (size_t command = 0; command < sizeof(commands) / sizeof(commands[0]); command++) {
    const u32 words[] = {commands[command], 0x26100001, 0x1000FFFF, 0};
    start_cpu_with_fastmem(mode, fastmem, 0x00010000, words, 4);
    g_cpu_state.cop0_regs.sr.CE2 = 1;
    run_slice(200);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
      EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    }
    finish_cpu();
  }
}

TEST(GteCommandDispatch, interpreter_unsupported_commands_continue)
{
  unsupported_commands_continue(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(GteCommandDispatch, cached_unsupported_commands_continue)
{
  unsupported_commands_continue(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(GteCommandDispatch, native_unsupported_commands_continue)
{
  unsupported_commands_continue(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(GteCommandDispatch, mmap_unsupported_commands_continue)
{
  unsupported_commands_continue(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP);
}
