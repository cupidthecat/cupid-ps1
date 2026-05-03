/*
 * Differential trace recorder.  See cpu_diff_runner.h for the design
 * tradeoff (offline diff over lockstep snapshot+restore).
 */

#include "cpu_diff_runner.h"

#include "cpu_core.h"
#include "settings.h"

#include "common/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* s_trace_file = NULL;
static int   s_state      = 0;  /* 0=unknown, 1=enabled, 2=disabled */

static void open_trace_file(void)
{
  const char* path = getenv("CUPID_DIFF_TRACE");
  if (path == NULL || path[0] == '\0') {
    s_state = 2;
    return;
  }

  /* Empty special-value "1" or "yes" → derive a path from the cpu mode
   * so a single env-var setting works for both runs. */
  char derived[256];
  if (strcmp(path, "1") == 0 || strcmp(path, "yes") == 0) {
    const char* mode = (g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_RECOMPILER) ? "recomp" :
                       (g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER) ? "cached" :
                                                                                                  "interp";
    snprintf(derived, sizeof(derived), "/tmp/cupid-ps1-diff-%s.txt", mode);
    path = derived;
  }

  s_trace_file = fopen(path, "w");
  if (s_trace_file == NULL) {
    fprintf(stderr, "[cpu_diff] Failed to open trace file '%s'\n", path);
    s_state = 2;
    return;
  }
  /* Line-buffered so SIGTERM-killed runs (used by `make test-diff` under
   * a wall-clock timeout) preserve every completed trace line. */
  setvbuf(s_trace_file, NULL, _IOLBF, BUFSIZ);
  atexit(cpu_diff_shutdown);
  /* Header line for human readability. */
  fputs("# pc instr gpr_xor_hash hi lo sr cause epc\n", s_trace_file);
  s_state = 1;
}

bool cpu_diff_is_enabled(void)
{
  if (s_state == 0) open_trace_file();
  return s_state == 1;
}

static u32 gpr_xor_hash(void)
{
  const cpu_registers_t* r = &g_cpu_state.regs;
  u32 h = 2166136261u;
  for (u32 i = 0u; i < 32u; i++) {
    const u32 v = r->r[i];
    h = (h ^ ((v >>  0) & 0xFFu)) * 16777619u;
    h = (h ^ ((v >>  8) & 0xFFu)) * 16777619u;
    h = (h ^ ((v >> 16) & 0xFFu)) * 16777619u;
    h = (h ^ ((v >> 24) & 0xFFu)) * 16777619u;
  }
  return h;
}

void cpu_diff_record_instruction(u32 pc, u32 instruction)
{
  if (!cpu_diff_is_enabled()) return;
  fprintf(s_trace_file,
          "%08x %08x %08x %08x %08x %08x %08x %08x\n",
          pc, instruction, gpr_xor_hash(),
          g_cpu_state.regs.named.hi, g_cpu_state.regs.named.lo,
          g_cpu_state.cop0_regs.sr.bits,
          g_cpu_state.cop0_regs.cause.bits, 
          g_cpu_state.cop0_regs.EPC);
}

void cpu_diff_shutdown(void)
{
  if (s_trace_file != NULL) {
    fflush(s_trace_file);
    fclose(s_trace_file);
    s_trace_file = NULL;
  }
  s_state = 0;
}
