/* PC-trigger GPR-dump hook.  See header for the contract. */

#include "core/cpu_pc_trigger.h"

#include "core/cpu_core.h"
#include "core/cpu_disasm.h"
#include "core/cpu_types.h"

#include "common/small_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int      s_state           = 0;   /* 0=unknown, 1=enabled, 2=disabled */
static u32      s_target_pc       = 0u;
static u32      s_limit           = 64u;
static u32      s_count           = 0u;
static FILE*    s_log             = NULL;
static char     s_log_path[256]   = {0};

static u32 parse_hex_u32(const char* s)
{
  if (!s) return 0u;
  while (*s == ' ' || *s == '\t') ++s;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  return (u32)strtoul(s, NULL, 16);
}

static u32 parse_dec_u32(const char* s, u32 fallback)
{
  if (!s || s[0] == '\0') return fallback;
  char* end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (!end || *end != '\0') return fallback;
  return (u32)v;
}

static void close_log_atexit(void)
{
  if (s_log != NULL && s_log != stderr) {
    fflush(s_log);
    fclose(s_log);
    s_log = NULL;
  }
}

bool cpu_pc_trigger_is_active(void)
{
  return s_state == 1;
}

u32 cpu_pc_trigger_target_pc(void)
{
  return s_target_pc;
}

void cpu_pc_trigger_install(void)
{
  if (s_state != 0) return;  /* idempotent */

  const char* pc_env = getenv("CUPID_PC_TRIGGER_PC");
  if (pc_env == NULL || pc_env[0] == '\0') {
    s_state = 2;
    return;
  }
  s_target_pc = parse_hex_u32(pc_env);
  if (s_target_pc == 0u) {
    fprintf(stderr,
            "[pc-trigger] CUPID_PC_TRIGGER_PC=%s did not parse to a non-zero "
            "PC; trigger disabled.\n", pc_env);
    s_state = 2;
    return;
  }

  const char* limit_env = getenv("CUPID_PC_TRIGGER_LIMIT");
  s_limit = parse_dec_u32(limit_env, 64u);
  if (s_limit == 0u) s_limit = 64u;

  const char* path_env = getenv("CUPID_PC_TRIGGER_LOG");
  if (path_env == NULL || path_env[0] == '\0')
    path_env = "/tmp/cupid-ps1-pctrig.log";
  strncpy(s_log_path, path_env, sizeof(s_log_path) - 1);
  s_log_path[sizeof(s_log_path) - 1] = '\0';
  s_log = fopen(s_log_path, "w");
  if (s_log == NULL) {
    fprintf(stderr,
            "[pc-trigger] failed to open log %s; falling back to stderr.\n",
            s_log_path);
    s_log = stderr;
  } else {
    atexit(close_log_atexit);
  }

  fprintf(s_log,
          "# pc-trigger active target_pc=0x%08x limit=%u\n",
          s_target_pc, s_limit);
  fflush(s_log);
  s_state = 1;
}

static const char* gpr_name(u32 i)
{
  static const char* names[34] = {
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra",
    "hi","lo"
  };
  return (i < 34u) ? names[i] : "??";
}

void cpu_pc_trigger_on_block(u32 tag_pc)
{
  if (s_state != 1) return;
  if (s_count >= s_limit) return;

  /* Caller may invoke us unconditionally from interp dispatch; gate again. */
  if (tag_pc != s_target_pc) return;

  /* Optional opcode gate.  CUPID_PC_TRIGGER_OPCODE=<hex> requires the RAM
   * word at tag_pc to match the given bits before firing.  Filters out
   * false-positive PC hits when an overlay temporarily places different
   * code at the trigger address (Crash's CDROM driver and game level code
   * both occupy 0x80043A24+ at different points in execution). */
  static int s_opcode_gate_armed = -1;
  static u32 s_opcode_gate_bits  = 0u;
  if (s_opcode_gate_armed < 0) {
    const char* op_env = getenv("CUPID_PC_TRIGGER_OPCODE");
    if (op_env && op_env[0] != '\0') {
      s_opcode_gate_bits  = parse_hex_u32(op_env);
      s_opcode_gate_armed = 1;
    } else {
      s_opcode_gate_armed = 0;
    }
  }
  if (s_opcode_gate_armed) {
    u32 word = 0u;
    if (!cpu_safe_read_memory_word(tag_pc, &word) || word != s_opcode_gate_bits)
      return;
  }

  ++s_count;

  fprintf(s_log,
          "===== pc-trigger #%u tag_pc=0x%08x =====\n",
          s_count, tag_pc);
  fprintf(s_log,
          "# pc=0x%08x npc=0x%08x cur_inst_pc=0x%08x cur_inst=0x%08x\n",
          g_cpu_state.pc, g_cpu_state.npc,
          g_cpu_state.current_instruction_pc,
          g_cpu_state.current_instruction.bits);
  fprintf(s_log,
          "cop0.sr=0x%08x cause=0x%08x epc=0x%08x badva=0x%08x\n",
          g_cpu_state.cop0_regs.sr.bits,
          g_cpu_state.cop0_regs.cause.bits,
          g_cpu_state.cop0_regs.EPC,
          g_cpu_state.cop0_regs.BadVaddr);
  fprintf(s_log,
          "load_delay_reg=%u load_delay_value=0x%08x\n",
          (unsigned)g_cpu_state.load_delay_reg,
          g_cpu_state.load_delay_value);
  for (u32 i = 0; i < 32u; i++) {
    fprintf(s_log, "r%02u %-4s = 0x%08x\n",
            i, gpr_name(i), g_cpu_state.regs.r[i]);
  }
  fprintf(s_log, "hi      = 0x%08x\n", g_cpu_state.regs.named.hi);
  fprintf(s_log, "lo      = 0x%08x\n", g_cpu_state.regs.named.lo);
  for (u32 i = 0; i < 64u; i++) {
    fprintf(s_log, "gte%02u   = 0x%08x\n", i, g_cpu_state.gte_regs.r32[i]);
  }

  /* Caller-disasm window.  $ra after JAL is JAL_PC + 8, so JAL is at
   * $ra - 8.  Walk back 12 instructions from $ra to capture the inst
   * sequence that produced $a0 / $a1 / $a2 / $a3 in the caller.  These
   * lines are the actionable signal for pinning recomp/interp divergence
   * (the # comment below makes them safely skippable in the diff). */
  const u32 ra = g_cpu_state.regs.named.ra;
  if (ra >= 0x80000010u && ra < 0x80800000u) {
    fprintf(s_log, "# caller disasm (8 inst pre-JAL through JAL@%08x):\n",
            (u32)(ra - 8u));
    for (u32 off = 32u; off >= 4u; off -= 4u) {
      const u32 pc = ra - off;
      u32 word = 0u;
      char b[96]; small_string_t s; small_string_init_stack(&s, b, sizeof(b));
      if (cpu_safe_read_memory_word(pc, &word)) {
        cpu_disassemble(&s, word, pc);
        fprintf(s_log, "#   %08x: %08x  %s\n", pc, word, small_string_c_str(&s));
      } else {
        fprintf(s_log, "#   %08x: <unreadable>\n", pc);
      }
    }
  }

  /* Local disasm window around tag_pc (16 inst before, 8 after).  Lets us
   * pin the *callee* instruction sequence when triggering on a target
   * inside a divergent block (rather than at a function entry). */
  if (tag_pc >= 0x80000000u && tag_pc < 0x80800000u) {
    fprintf(s_log,
            "# local disasm (tag_pc-64..tag_pc+32):\n");
    for (u32 i = 0; i < 24u; i++) {
      const u32 pc = tag_pc + (i * 4u) - 64u;
      u32 word = 0u;
      char b[96]; small_string_t s; small_string_init_stack(&s, b, sizeof(b));
      if (cpu_safe_read_memory_word(pc, &word)) {
        cpu_disassemble(&s, word, pc);
        const char* mark = (pc == tag_pc) ? " <==" : "";
        fprintf(s_log, "#   %08x: %08x  %s%s\n", pc, word,
                small_string_c_str(&s), mark);
      } else {
        fprintf(s_log, "#   %08x: <unreadable>\n", pc);
      }
    }
  }
  fflush(s_log);
}
