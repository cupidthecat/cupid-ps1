/*
 * Per-block recompiler/interpreter divergence detector.  See header for
 * the block-boundary snapshot strategy.
 */

#include "core/cpu_diff_block.h"

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_disasm.h"
#include "core/cpu_types.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include "common/log.h"
#include "common/small_string.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CPUDiffBlock);

enum { CPU_DIFF_RAM_LOG_CAP = 4096u };

typedef struct {
  u32 paddr;
  u8  size;
  u32 value;
} ram_log_entry_t;

typedef struct {
  cpu_state_t state;
  u32         block_pc;
  u32         block_size;
  /* Ticks-mode side-channel (only populated when s_ticks_mode != 0). */
  u32            irq_status;
  u32            irq_mask;
  global_ticks_t global_ticks;
} block_snapshot_t;

static int              s_state         = 0;  /* 0=unknown, 1=enabled, 2=disabled */
static int              s_ticks_mode    = 0;  /* 0=off, 1=on (CUPID_RECOMP_DIFF_TICKS) */
static block_snapshot_t s_enter_snap;
static block_snapshot_t s_post_recomp_snap;
static u8*              s_enter_ram;       /* RAM at block entry; sized to s_ram_buf_size */
static u8*              s_post_recomp_ram; /* RAM after recomp finished the block */
static u32              s_ram_buf_size;    /* bytes allocated in each of the two buffers */
static ram_log_entry_t  s_recomp_log[CPU_DIFF_RAM_LOG_CAP];
static u32              s_recomp_log_count;
static ram_log_entry_t  s_interp_log[CPU_DIFF_RAM_LOG_CAP];
static u32              s_interp_log_count;
static bool             s_in_replay;     /* tap routes to s_interp_log when set */
static bool             s_inside_block;  /* set by enter, cleared by exit */
static u64              s_blocks_checked;
static u64              s_divergences;

/* Lazy-grow the two RAM scratch buffers to the current bus RAM size.  Called
 * from enter; cheap on the second-and-later calls (size match → no-op). */
static void ensure_ram_buffers(u32 want_size)
{
  if (want_size == 0u || s_ram_buf_size == want_size) return;
  free(s_enter_ram);
  free(s_post_recomp_ram);
  s_enter_ram       = (u8*)malloc(want_size);
  s_post_recomp_ram = (u8*)malloc(want_size);
  if (s_enter_ram == NULL || s_post_recomp_ram == NULL) {
    fprintf(stderr,
            "[recomp-diff] failed to allocate %u-byte RAM scratch buffers; "
            "harness disabled\n", want_size);
    free(s_enter_ram);
    free(s_post_recomp_ram);
    s_enter_ram = s_post_recomp_ram = NULL;
    s_ram_buf_size = 0u;
    s_state = 2;
    return;
  }
  s_ram_buf_size = want_size;
}

static bool eval_active(void)
{
  const char* env = getenv("CUPID_RECOMP_DIFF");
  const bool by_env = env != NULL && env[0] != '\0' && env[0] != '0';
  const bool by_setting = g_settings.cpu_recompiler_compare_register_files;
  return by_env || by_setting;
}

bool cpu_diff_block_is_active(void)
{
  if (s_state == 0)
    s_state = eval_active() ? 1 : 2;
  return s_state == 1;
}

void cpu_diff_block_install(void)
{
  s_state = eval_active() ? 1 : 2;
  g_bus_ram_write_tap = (s_state == 1) ? &cpu_diff_block_ram_tap : NULL;
  if (s_state == 1) {
    /* Env-var escape hatches so the harness can be run with linking or
     * fastmem forcibly enabled, exercising those code paths.  Without
     * the override, both stay forced off (the default).
     *
     * CUPID_RECOMP_DIFF_KEEP_LINKING; leaves block_linking at its
     * settings.ini default.  Block linking lets a single JIT block
     * invocation execute multiple guest iterations (self-link jmp from
     * end-of-block back to block_start, with diff_block_enter early-
     * returning on s_inside_block); that breaks the per-iteration
     * snapshot pairing the harness relies on, so divergences in this
     * mode can be false positives.  Use only as a stress probe.
     *
     * CUPID_RECOMP_DIFF_KEEP_FASTMEM; leaves cpu_fastmem_mode at its
     * settings.ini default.  Fastmem's SIGSEGV → backpatch path can
     * resume execution mid-block without firing diff_block_exit, so
     * partial-block mutations leak across snapshots; same caveat. */
    const char* keep_linking = getenv("CUPID_RECOMP_DIFF_KEEP_LINKING");
    const char* keep_fastmem = getenv("CUPID_RECOMP_DIFF_KEEP_FASTMEM");
    const bool override_linking = (keep_linking == NULL) ||
                                  (keep_linking[0] == '\0') ||
                                  (keep_linking[0] == '0');
    const bool override_fastmem = (keep_fastmem == NULL) ||
                                  (keep_fastmem[0] == '\0') ||
                                  (keep_fastmem[0] == '0');
    if (override_linking)
      g_settings.cpu_recompiler_block_linking = false;
    if (override_fastmem)
      g_settings.cpu_fastmem_mode = CPU_FASTMEM_MODE_DISABLED;
    /* Ticks-mode opt-in.  Compares pending_ticks delta + IRQ controller
     * state per block; aborts on first divergence.  Off by default so the
     * existing per-block GPR/RAM regression stays green even when the
     * recomp's tick model legitimately drifts from interp's. */
    const char* ticks_env = getenv("CUPID_RECOMP_DIFF_TICKS");
    s_ticks_mode = (ticks_env != NULL && ticks_env[0] != '\0' &&
                    ticks_env[0] != '0') ? 1 : 0;
    INFO_LOG("Per-block recomp/interp register-file diff active "
             "(block_linking=%s, fastmem=%s, ticks=%s).",
             g_settings.cpu_recompiler_block_linking ? "ON" : "off",
             g_settings.cpu_fastmem_mode != CPU_FASTMEM_MODE_DISABLED ? "ON" : "off",
             s_ticks_mode ? "ON" : "off");
  }
}

void cpu_diff_block_ram_tap(u32 paddr, u8 size, u32 value, u32 writer_pc)
{
  (void)writer_pc;  /* harness compares value/paddr only; PC unused here */
  if (s_state != 1 || !s_inside_block) return;
  ram_log_entry_t* log = s_in_replay ? s_interp_log : s_recomp_log;
  u32* count_ptr       = s_in_replay ? &s_interp_log_count : &s_recomp_log_count;
  if (*count_ptr >= CPU_DIFF_RAM_LOG_CAP) return;
  log[*count_ptr].paddr = paddr;
  log[*count_ptr].size  = size;
  log[*count_ptr].value = value;
  ++*count_ptr;
}

void cpu_diff_block_enter(u32 block_pc, u32 block_size)
{
  if (s_state != 1) return;

  /* If s_inside_block is still true here, the previous block aborted
   * without invoking diff_block_exit (fastmem fault → handler resumed,
   * block-protect failure → discard_and_recompile, icache mismatch, etc.).
   * The state captured in s_enter_snap is stale; the live cpu_state has
   * had however-many partial-block mutations applied since.  Drop the
   * stale snapshot and re-snap from current state so the upcoming exit
   * compares apples-to-apples. */
  s_inside_block          = true;
  s_in_replay             = false;
  memcpy(&s_enter_snap.state, &g_cpu_state, sizeof(cpu_state_t));
  s_enter_snap.block_pc   = block_pc;
  s_enter_snap.block_size = block_size;
  if (s_ticks_mode) {
    s_enter_snap.irq_status   = irq_dbg_status_register();
    s_enter_snap.irq_mask     = irq_dbg_mask_register();
    s_enter_snap.global_ticks = timing_events_get_global_tick_counter();
  }

  /* Snapshot bus RAM so the interp replay starts from the same memory
   * recomp saw at block entry, eliminating the false-positive class where
   * a load reads a value the recomp pass already mutated. */
  const u32 ram_size = bus_get_ram_size();
  ensure_ram_buffers(ram_size);
  if (s_state != 1) return;  /* allocator failure inside ensure_ram_buffers */
  memcpy(s_enter_ram, bus_get_unprotected_ram_pointer(), ram_size);

  s_recomp_log_count      = 0;
  s_interp_log_count      = 0;
}

static void log_field_diff(const char* name,
                           u64 recomp_v, u64 interp_v)
{
  fprintf(stderr,
          "  %-32s recomp=0x%016llx  interp=0x%016llx\n",
          name,
          (unsigned long long)recomp_v, 
          (unsigned long long)interp_v);
}

#define DIFF_SCALAR(field)                                                      \
  do {                                                                          \
    if ((u64)(s_post_recomp_snap.state.field) !=                                \
        (u64)(g_cpu_state.field)) {                                             \
      log_field_diff(#field,                                                    \
                     (u64)(s_post_recomp_snap.state.field),                     \
                     (u64)(g_cpu_state.field));                                 \
      mismatch = true;                                                          \
    }                                                                           \
  } while (0)

#define DIFF_ARRAY(arr, len)                                                    \
  do {                                                                          \
    for (u32 _i = 0; _i < (len); _i++) {                                        \
      if (s_post_recomp_snap.state.arr[_i] != g_cpu_state.arr[_i]) {            \
        char _name[64];                                                         \
        snprintf(_name, sizeof(_name), #arr "[%u]", _i);                        \
        log_field_diff(_name,                                                   \
                       s_post_recomp_snap.state.arr[_i],                        \
                       g_cpu_state.arr[_i]);                                    \
        mismatch = true;                                                        \
      }                                                                         \
    }                                                                           \
  } while (0)

/* On divergence, disassemble every MIPS instruction in the failing block
 * to stderr.  Best-effort: any word we can't safely fetch is shown as
 * "<unreadable>".  Cheap (block_size is typically <40). */
static void dump_block_disasm(void)
{
  /* Dump enter-snap GPRs + load-delay so we can reason about HOW recomp
   * and interp diverged on this block, not just THAT they did. */
  fprintf(stderr, "[recomp-diff] enter snap GPRs:\n");
  for (u32 i = 0; i < 32u; ++i) {
    fprintf(stderr, " r%-2u=%08x%s", i, s_enter_snap.state.regs.r[i],
            ((i % 8) == 7) ? "\n" : "");
  }
  fprintf(stderr,
          "[recomp-diff] enter snap load_delay_reg=%u value=0x%08x  "
          "next_load_delay_reg=%u value=0x%08x  "
          "branch_was_taken=%u next_inst_is_BDS=%u\n",
          (unsigned)s_enter_snap.state.load_delay_reg,
          s_enter_snap.state.load_delay_value,
          (unsigned)s_enter_snap.state.next_load_delay_reg,
          s_enter_snap.state.next_load_delay_value,
          (unsigned)s_enter_snap.state.branch_was_taken,
          (unsigned)s_enter_snap.state.next_instruction_is_branch_delay_slot);
  fprintf(stderr,
          "[recomp-diff] block PC=0x%08x size=%u disassembly:\n",
          s_enter_snap.block_pc, s_enter_snap.block_size);
  for (u32 i = 0; i < s_enter_snap.block_size; i++) {
    const u32 pc = s_enter_snap.block_pc + i * 4u;
    u32 instr = 0;
    small_string_t s;
    char buf[96];
    small_string_init_stack(&s, buf, sizeof(buf));
    if (cpu_safe_read_memory_word(pc, &instr)) {
      cpu_disassemble(&s, instr, pc);
      fprintf(stderr, "  0x%08x: 0x%08x  %s\n",
              pc, instr, small_string_c_str(&s));
    } else {
      fprintf(stderr, "  0x%08x: <unreadable>\n", pc);
    }
  }
}

static void dump_ram_log(const char* tag,
                         const ram_log_entry_t* log, u32 count)
{
  fprintf(stderr, "  %s log (%u entries):\n", tag, count);
  for (u32 i = 0; i < count; i++) {
    fprintf(stderr,
            "    [%2u] paddr=0x%08x size=%u value=0x%08x\n",
            i, log[i].paddr, log[i].size, log[i].value);
  }
}

static bool compare_state(void)
{
  bool mismatch = false;

  /* GPRs; regs is a union of r[32] and named.{zero,at,...,hi,lo} */
  for (u32 i = 0; i < 32u; i++) {
    if (s_post_recomp_snap.state.regs.r[i] != g_cpu_state.regs.r[i]) {
      char name[32];
      snprintf(name, sizeof(name), "regs.r[%u]", i);
      log_field_diff(name, s_post_recomp_snap.state.regs.r[i],
                     g_cpu_state.regs.r[i]);
      mismatch = true;
    }
  }
  DIFF_SCALAR(regs.named.hi);
  DIFF_SCALAR(regs.named.lo);

  /* pc + npc are control-flow representation artifacts.  After a block
   * ending in a self-link branch the JIT pc = block_start, while the
   * interpreter running `size` linear instructions ends past the branch
   * delay slot at block_start + size*4.  Both encode the same flow.
   * Mid-block exceptions further skew interp PC vs JIT-exit PC because
   * the interpreter is told to run the full size while the JIT bailed
   * early.  Real branch/jump bugs surface as GPR divergences (the
   * comparand registers differ); PC-only divergences are JIT/interp
   * model mismatches, not correctness bugs.  Skipped. */

  DIFF_SCALAR(cop0_regs.sr.bits);
  DIFF_SCALAR(cop0_regs.cause.bits);
  DIFF_SCALAR(cop0_regs.EPC);
  DIFF_SCALAR(cop0_regs.BadVaddr);

  DIFF_ARRAY(gte_regs.r32, 64u);

  /* Tick accumulators are skipped in default mode (interp +=1 per inst,
   * recomp accumulates r->cycles + icache fill ticks and flushes at
   * end_block; the deltas should match in aggregate but per-block ordering
   * may differ).  Ticks-mode opts in to a strict per-block delta
   * compare; the lever for hunting IRQ-delivery cycle drift.
   *
   * We compare deltas (post - enter), not raw values, because the interp
   * replay starts from the enter snapshot (so its post-replay
   * pending_ticks reflects interp_delta only) while the recomp post
   * snapshot includes whatever pending_ticks recomp accumulated.  The
   * global_tick counter is invariant across the replay (the replay does
   * not run timing_events), so an irq_status/mask delta comparison is
   * symmetric. */
  /* Muldiv-stall opcodes (MULT/MULTU/DIV/DIVU/MFHI/MFLO/MTHI/MTLO)
   * are modeled in the recompiler now (cpu_recompiler.c
   * stall_until_muldiv_complete + set_muldiv_done_cycle_static; producer
   * emit in x64_backend.c compile_mult_inner / hook_compile_div / divu).
   *
   * For CACHED blocks (KUSEG/KSEG0 hot RAM), the GTE-style r->cycles
   * tracking matches interp tick-for-tick.  For UNCACHED blocks
   * (BIOS/KSEG1), the recomp prologue eagerly adds N*fetch_tick to
   * pending_ticks while step_for_diff pays them lazily per-iteration .
   * the muldiv stall in recomp's r->cycles space then over-stalls
   * relative to interp's pt space.  Same structural mismatch exists
   * today for GTE_STALL in uncached blocks but never surfaces because
   * BIOS rarely uses GTE.  Narrow the muldiv skip to uncached
   * muldiv blocks only; cached muldiv blocks are now compared. */
  bool block_has_muldiv = false;
  if (!cpu_is_cached_address(s_enter_snap.block_pc)) {
    for (u32 i = 0u; i < s_enter_snap.block_size; i++) {
      const u32 pc = s_enter_snap.block_pc + i * 4u;
      u32 word = 0u;
      if (!cpu_safe_read_memory_word(pc, &word)) continue;
      const u32 op = (word >> 26) & 0x3Fu;
      if (op != 0u) continue;
      const u32 funct = word & 0x3Fu;
      if (funct >= 0x10u && funct <= 0x1Bu) {
        block_has_muldiv = true;
        break;
      }
    }
  }

  if (s_ticks_mode && !block_has_muldiv) {
    const u32 recomp_pt_delta = s_post_recomp_snap.state.pending_ticks -
                                s_enter_snap.state.pending_ticks;
    /* step_for_diff was rewritten (cpu_core.c) to charge exactly N
     * fetches per N-instruction block; 1 seed + (N-1) in-loop, no
     * trailing prefetch; matching the recomp's icache prefix
     * accounting for non-cached blocks.
     *
     * For cached blocks (KUSEG / KSEG0), there is a structural offset:
     * cpu_diff_block_exit clears the icache before replay; recomp's
     * prefix populates icache_tags but not icache_data, so uncleared
     * tags would yield false hits with stale 0-data.  After
     * the clear, every line the interp replay fetches misses and triggers
     * cpu_fill_icache, which charges per-word fill ticks via
     * cpu_do_instruction_read(icache_read=true).  The recomp's icache
     * prefix charges 0 ticks when `cpu_recompiler_icache=false` (the
     * default).  Per-block this gives a deterministic
     * positive interp delta over recomp; subtract it before comparing.
     * Aggregate cost over many real executions matches: real interp pays
     * the fill once on the cold first execution, real recomp pays zero,
     * and on hot subsequent invocations (block-link chain) both pay zero.
     * This subtraction is the harness's compensation for the always-cold
     * replay model. */
    u32 interp_fill_compensation = 0u;
    u32 recomp_fill_compensation = 0u;
    const bool block_is_cached = cpu_is_cached_address(s_enter_snap.block_pc);
    if (block_is_cached) {
      u32 cur_pc = s_enter_snap.block_pc;
      u32 last_line = (u32)-1;
      u32 line_count = 0u;
      for (u32 i = 0u; i < s_enter_snap.block_size; i++) {
        const u32 line = cur_pc >> 4;  /* 16 byte cache line */
        if (line != last_line) {
          const u32 word_off = (cur_pc >> 2) & 0x3u;
          /* Interp side: cpu_fill_icache reads `4 - word_offset` words at
           * 1 tick each per first-fetched-line (per-word fill model). */
          interp_fill_compensation += (4u - word_off);
          line_count++;
          last_line = line;
        }
        cur_pc += 4u;
      }
      /* Recomp side: when `cpu_recompiler_icache=true` (default off, but
       * gamedb compat may force on for some titles e.g. Silent Hill), the
       * icache prefix charges `fill_ticks` per *runtime-missed* line (the
       * cmovne sequence in hook_generate_icache_check_and_update is
       * per-line tag compare; matching tag adds 0).  Count actual misses
       * by checking the *enter-state* icache_tags against each touched
       * line's expected tag.  fill_ticks = (CPU_ICACHE_LINE_SIZE/4) = 4
       * for RAM (one tick per word when icache_read=true).  When icache
       * is off, the prefix emits nothing and recomp pays 0 here. */
      if (g_settings.cpu_recompiler_icache) {
        cur_pc = s_enter_snap.block_pc;
        last_line = (u32)-1;
        for (u32 i = 0u; i < s_enter_snap.block_size; i++) {
          const u32 line_addr = cur_pc & CPU_ICACHE_TAG_ADDRESS_MASK;
          const u32 line_idx  = (line_addr >> 4) & 0xFFu;  /* CPU_ICACHE_LINES = 256 */
          if (line_idx != last_line) {
            const u32 expected_tag = line_addr;  /* tag = line-aligned PC */
            if (s_enter_snap.state.icache_tags[line_idx] != expected_tag)
              recomp_fill_compensation += 4u;  /* miss -> fill_ticks added */
            last_line = line_idx;
          }
          cur_pc += 4u;
        }
      }
    }
    const u32 interp_pt_delta_raw = g_cpu_state.pending_ticks -
                                    s_enter_snap.state.pending_ticks;
    const u32 interp_pt_delta = (interp_pt_delta_raw >= interp_fill_compensation) ?
                                  (interp_pt_delta_raw - interp_fill_compensation) :
                                  interp_pt_delta_raw;
    const u32 recomp_pt_delta_adj = (recomp_pt_delta >= recomp_fill_compensation) ?
                                      (recomp_pt_delta - recomp_fill_compensation) :
                                      recomp_pt_delta;
    if (recomp_pt_delta_adj != interp_pt_delta) {
      fprintf(stderr,
              "  pending_ticks_delta              recomp=%u  interp=%u  "
              "diff=%+d (block size=%u, %.2f cyc/inst recomp vs %.2f interp; "
              "raw recomp=%u interp=%u)\n",
              recomp_pt_delta_adj, interp_pt_delta,
              (int)recomp_pt_delta_adj - (int)interp_pt_delta,
              s_enter_snap.block_size,
              s_enter_snap.block_size ? (double)recomp_pt_delta_adj /
                                          (double)s_enter_snap.block_size : 0.0,
              s_enter_snap.block_size ? (double)interp_pt_delta /
                                          (double)s_enter_snap.block_size : 0.0,
              recomp_pt_delta, interp_pt_delta_raw);
      mismatch = true;
    }
    /* IRQ status/mask: any in-block transition is a control-flow signal
     * (mtc0 SR write, mem-mapped ack, or a tap into i_stat).  The interp
     * replay should reproduce the same transition.  Compare post-replay
     * value (g_cpu_state side) against post-recomp snapshot. */
    const u32 recomp_irq_status = s_post_recomp_snap.irq_status;
    const u32 interp_irq_status = irq_dbg_status_register();
    if (recomp_irq_status != interp_irq_status) {
      fprintf(stderr,
              "  irq_status                       recomp=0x%08x  interp=0x%08x  "
              "(enter=0x%08x)\n",
              recomp_irq_status, interp_irq_status, s_enter_snap.irq_status);
      mismatch = true;
    }
    const u32 recomp_irq_mask = s_post_recomp_snap.irq_mask;
    const u32 interp_irq_mask = irq_dbg_mask_register();
    if (recomp_irq_mask != interp_irq_mask) {
      fprintf(stderr,
              "  irq_mask                         recomp=0x%08x  interp=0x%08x  "
              "(enter=0x%08x)\n",
              recomp_irq_mask, interp_irq_mask, s_enter_snap.irq_mask);
      mismatch = true;
    }
  }

  DIFF_SCALAR(load_delay_reg);
  if (s_post_recomp_snap.state.load_delay_reg != (cpu_reg_t)CPU_REG_COUNT ||
      g_cpu_state.load_delay_reg != (cpu_reg_t)CPU_REG_COUNT) {
    DIFF_SCALAR(load_delay_value);
  }

  /* next_load_delay_* must also match across block boundary; the NEXT
   * block's first instruction inherits these.  Missing this compare lets
   * a "load issued inside a delay slot" leak undetected. */
  DIFF_SCALAR(next_load_delay_reg);
  if (s_post_recomp_snap.state.next_load_delay_reg != (cpu_reg_t)CPU_REG_COUNT ||
      g_cpu_state.next_load_delay_reg != (cpu_reg_t)CPU_REG_COUNT) {
    DIFF_SCALAR(next_load_delay_value);
  }

  /* Branch state inherited by next block.  next_instruction_is_BDS
   * tells the next block's first inst that it's a delay slot; getting it
   * wrong reorders branch resolution.  branch_was_taken affects exception
   * BD bit at the next syscall/break. */
  DIFF_SCALAR(branch_was_taken);
  DIFF_SCALAR(next_instruction_is_branch_delay_slot);

  /* 1 KB scratchpad ($1F800000) is CPU-side fast RAM, lives inside
   * cpu_state, not in the bus RAM blob the harness's compare_ram covers.
   * Any recomp store that writes to scratchpad with the wrong value, OR
   * uses a stale scratchpad read, would propagate silently across blocks
   * without an explicit compare here. */
  DIFF_ARRAY(scratchpad, CPU_SCRATCHPAD_SIZE);

  return !mismatch;
}

/* Direct RAM-contents compare between live_ram (post-interp-replay) and
 * the recomp-RAM snapshot.  Independent of the bus tap, so it works even
 * when recomp uses fastmem (which bypasses the slow-path tap entirely).
 * Reports up to first 4 differing words, then returns mismatch flag. */
static bool compare_ram(void)
{
  if (s_post_recomp_ram == NULL || s_enter_ram == NULL) return true;
  const u32 ram_size = bus_get_ram_size();
  if (ram_size > s_ram_buf_size) return true;
  const u8* live = bus_get_unprotected_ram_pointer();
  if (memcmp(live, s_post_recomp_ram, ram_size) == 0) return true;

  fprintf(stderr, "  RAM divergence (interp post-replay vs recomp post):\n");
  u32 reported = 0;
  for (u32 off = 0; off < ram_size && reported < 4u; off += 4u) {
    u32 a, b;
    memcpy(&a, &live[off], 4);
    memcpy(&b, &s_post_recomp_ram[off], 4);
    if (a != b) {
      fprintf(stderr,
              "    paddr=0x%08x  interp=0x%08x  recomp=0x%08x\n",
              off, a, b);
      ++reported;
    }
  }
  return false;
}

#undef DIFF_SCALAR
#undef DIFF_ARRAY

void cpu_diff_block_exit(void)
{
  if (s_state != 1 || !s_inside_block) return;

  const u32 ram_size = bus_get_ram_size();
  u8* const live_ram = bus_get_unprotected_ram_pointer();

  /* 1. Snapshot post-recomp cpu_state + RAM. */
  memcpy(&s_post_recomp_snap.state, &g_cpu_state, sizeof(cpu_state_t));
  s_post_recomp_snap.block_pc   = s_enter_snap.block_pc;
  s_post_recomp_snap.block_size = s_enter_snap.block_size;
  if (s_ticks_mode) {
    s_post_recomp_snap.irq_status   = irq_dbg_status_register();
    s_post_recomp_snap.irq_mask     = irq_dbg_mask_register();
    s_post_recomp_snap.global_ticks = timing_events_get_global_tick_counter();
  }
  if (ram_size <= s_ram_buf_size)
    memcpy(s_post_recomp_ram, live_ram, ram_size);

  /* 2. Restore enter state + RAM so the replay runs on the same memory
   * recomp saw, not on recomp's mutations. */
  memcpy(&g_cpu_state, &s_enter_snap.state, sizeof(cpu_state_t));
  g_cpu_state.using_interpreter = true;
  if (ram_size <= s_ram_buf_size)
    memcpy(live_ram, s_enter_ram, ram_size);

  /* The JIT's emitted icache_check_and_update path writes icache_tags
   * without populating icache_data (the JIT executes from inline pre-decoded
   * instruction bits, not from the decoded icache).  An interpreter fetch
   * sees those tags, falsely hits, and reads stale 0-filled data.  Clear
   * the icache so the interp replay refills cleanly from RAM.  Cheap
   * (4 KB tags + 4 KB data on a typical config). */
  cpu_clear_icache();

  /* 3. Run the interpreter for `block_size` instructions on g_cpu_state. */
  s_in_replay = true;
  cpu_core_step_for_diff(s_enter_snap.block_size);
  s_in_replay = false;

  /* 4. Compare interp final (g_cpu_state) vs post-recomp (s_post_recomp_snap)
   * AND interp post-replay RAM vs recomp post RAM. */
  ++s_blocks_checked;
  const bool state_ok = compare_state();
  const bool ram_ok   = compare_ram();
  if (!state_ok || !ram_ok) {
    ++s_divergences;
    fprintf(stderr,
            "[recomp-diff] divergence at block PC=0x%08x size=%u (block #%llu)\n",
            s_enter_snap.block_pc, s_enter_snap.block_size,
            (unsigned long long)s_blocks_checked);
    dump_block_disasm();
    dump_ram_log("recomp RAM-write (slow-path tap only)",
                 s_recomp_log, s_recomp_log_count);
    dump_ram_log("interp RAM-write (slow-path tap only)",
                 s_interp_log, s_interp_log_count);
    fflush(stderr);
    abort();
  }

  /* 5. Restore post-recomp state + RAM so real execution continues
   * unaffected by the replay. */
  memcpy(&g_cpu_state, &s_post_recomp_snap.state, sizeof(cpu_state_t));
  if (ram_size <= s_ram_buf_size)
    memcpy(live_ram, s_post_recomp_ram, ram_size);

  s_inside_block     = false;
  s_recomp_log_count = 0;
  s_interp_log_count = 0;
}
