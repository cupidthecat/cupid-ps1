#include "cpu_diff_runner.h"

#include "cpu_core.h"
#include "core/config/settings.h"
#include "core/system/system.h"
#include "core/scheduler/timing_event.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static FILE* s_file;
static u64   s_limit;
static u64   s_count;
static bool  s_failed;
static bool  s_completed;

bool cpu_diff_initialize(void)
{
  s_count          = 0;
  s_limit          = 0;
  s_failed         = false;
  s_completed      = false;
  const char* path = getenv("CUPID_DIFF_TRACE");
  if (!path || !*path) return true;
  const char* limit = getenv("CUPID_DIFF_LIMIT");
  if (!limit || !*limit || g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_RECOMPILER) {
    fprintf(stderr, "[cpu-trace] requires an instruction count and interp or cached mode\n");
    s_failed = true;
    return false;
  }
  for (const char* p = limit; *p; p++) {
    if (*p < '0' || *p > '9' || s_limit > (UINT64_MAX - (u32)(*p - '0')) / 10u) {
      fprintf(stderr, "[cpu-trace] invalid instruction count\n");
      s_failed = true;
      return false;
    }
    s_limit = s_limit * 10u + (u32)(*p - '0');
  }
  if (!s_limit || !(s_file = fopen(path, "wx"))) {
    fprintf(stderr, "[cpu-trace] cannot create trace or invalid zero count (errno=%d)\n", errno);
    s_failed = true;
    return false;
  }
  fputs("# cupid-cpu-trace-v2: sequence pc instruction gpr[34] cop0[11] gte[64] "
        "load_reg load_value next_load_reg next_load_value pending_ticks global_ticks\n",
        s_file);
  return true;
}

bool cpu_diff_is_enabled(void)
{
  return s_file != NULL;
}

static void word(u32 value)
{
  fprintf(s_file, " %08x", value);
}

void cpu_diff_record_instruction(u32 pc, u32 instruction)
{
  if (!s_file || s_completed || s_failed) return;
  fprintf(s_file, "%llu", (unsigned long long)++s_count);
  word(pc);
  word(instruction);
  for (u32 i = 0; i < CPU_REG_COUNT; i++)
    word(g_cpu_state.regs.r[i]);
  const cpu_cop0_registers_t* c      = &g_cpu_state.cop0_regs;
  const u32                   cop0[] = {c->BPC, c->BDA,  c->TAR,     c->BadVaddr,   c->BDAM,     c->BPCM,
                                        c->EPC, c->PRID, c->sr.bits, c->cause.bits, c->dcic.bits};
  for (u32 i = 0; i < sizeof(cop0) / sizeof(*cop0); i++)
    word(cop0[i]);
  for (u32 i = 0; i < 64; i++)
    word(g_cpu_state.gte_regs.r32[i]);
  word(g_cpu_state.load_delay_reg);
  word(g_cpu_state.load_delay_value);
  word(g_cpu_state.next_load_delay_reg);
  word(g_cpu_state.next_load_delay_value);
  word(g_cpu_state.pending_ticks);
  fprintf(s_file, " %016llx\n", (unsigned long long)timing_events_get_global_tick_counter());
  s_failed = ferror(s_file) != 0;
}

bool cpu_diff_after_instruction(void)
{
  if (!s_file) return false;
  if (s_count == s_limit || s_failed) {
    s_completed = !s_failed;
    system_shutdown_system(false);
    return true;
  }
  return false;
}

bool cpu_diff_shutdown(void)
{
  if (s_file) {
    if (s_completed && !s_failed) fprintf(s_file, "# completed %llu\n", (unsigned long long)s_count);
    s_failed |= !s_completed || ferror(s_file) != 0;
    if (fclose(s_file) != 0) s_failed = true;
    s_file = NULL;
  }
  return !s_failed;
}
