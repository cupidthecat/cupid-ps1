/* Trace-file failures must fail validation rather than silently disable tracing. */
#include "tests/test_harness.h"
#include "core/cpu/cpu_core.h"
#include "core/cpu/cpu_diff_runner.h"
#include "core/config/settings.h"

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

static void trace_path(char* path)
{
  strcpy(path, "/tmp/cupid-trace-test-XXXXXX");
  const int fd = mkstemp(path);
  EXPECT_TRUE(fd >= 0);
  close(fd);
  EXPECT_EQ(unlink(path), 0);
  setenv("CUPID_DIFF_TRACE", path, 1);
  setenv("CUPID_DIFF_LIMIT", "1", 1);
  settings_init(&g_settings);
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_INTERPRETER;
}

TEST(CpuTrace, refuses_existing_output_without_truncating)
{
  char path[64];
  trace_path(path);
  FILE* file = fopen(path, "w");
  EXPECT_NOT_NULL(file);
  fputs("preserve", file);
  fclose(file);
  EXPECT_FALSE(cpu_diff_initialize());
  EXPECT_FALSE(cpu_diff_shutdown());
  file          = fopen(path, "r");
  char bytes[9] = {0};
  EXPECT_EQ(fread(bytes, 1, 8, file), 8);
  EXPECT_EQ(strcmp(bytes, "preserve"), 0);
  fclose(file);
  unlink(path);
}

TEST(CpuTrace, refuses_native_partial_instruction_traces)
{
  char path[64];
  trace_path(path);
  g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_RECOMPILER;
  EXPECT_FALSE(cpu_diff_initialize());
  EXPECT_FALSE(cpu_diff_shutdown());
  EXPECT_TRUE(access(path, F_OK) != 0);
}

TEST(CpuTrace, completion_requires_the_requested_instruction)
{
  char path[64];
  trace_path(path);
  EXPECT_TRUE(cpu_diff_initialize());
  g_cpu_state.regs.r[31]       = 0x12345678;
  g_cpu_state.gte_regs.r32[63] = 0x89ABCDEF;
  cpu_diff_record_instruction(0xBFC00000, 0x24081234);
  EXPECT_TRUE(cpu_diff_after_instruction());
  EXPECT_TRUE(cpu_diff_shutdown());
  FILE* file = fopen(path, "r");
  EXPECT_NOT_NULL(file);
  char bytes[4096] = {0};
  EXPECT_TRUE(fread(bytes, 1, sizeof(bytes) - 1, file) > 0);
  EXPECT_NOT_NULL(strstr(bytes, " 12345678 "));
  EXPECT_NOT_NULL(strstr(bytes, " 89abcdef "));
  EXPECT_NOT_NULL(strstr(bytes, "# completed 1\n"));
  fclose(file);
  unlink(path);
}

TEST(CpuTrace, incomplete_trace_fails)
{
  char path[64];
  trace_path(path);
  EXPECT_TRUE(cpu_diff_initialize());
  EXPECT_FALSE(cpu_diff_shutdown());
  unlink(path);
}

TEST(CpuTrace, buffered_write_or_close_failure_fails)
{
  char path[64];
  trace_path(path);
  signal(SIGXFSZ, SIG_IGN);
  const struct rlimit limit = {100, 100};
  EXPECT_EQ(setrlimit(RLIMIT_FSIZE, &limit), 0);
  EXPECT_TRUE(cpu_diff_initialize());
  cpu_diff_record_instruction(0xBFC00000, 0);
  EXPECT_TRUE(cpu_diff_after_instruction());
  EXPECT_FALSE(cpu_diff_shutdown());
  unlink(path);
}

TEST(CpuTrace, invalid_instruction_counts_fail_without_output)
{
  char path[64];
  trace_path(path);
  const char* counts[] = {"0", "-1", "1tail", " 1", "18446744073709551616"};
  for (u32 i = 0; i < sizeof(counts) / sizeof(*counts); i++) {
    setenv("CUPID_DIFF_LIMIT", counts[i], 1);
    EXPECT_FALSE(cpu_diff_initialize());
    EXPECT_FALSE(cpu_diff_shutdown());
    EXPECT_TRUE(access(path, F_OK) != 0);
  }
}
