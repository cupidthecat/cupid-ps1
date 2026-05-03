/*
 * Full code-cache driver:
 *   - LUT (g_code_lut + per-page sub-tables, plus parallel block LUT)
 *   - Block storage (dynamic-array pool, trailing inst+inst-info arrays)
 *   - Block reading + reg-flow pre-pass (FillBlockRegInfo)
 *   - Page-protection state-machine (4-invalidations-in-60-frames → manual)
 *   - Block-link multimap (linear-probe array, dead-flag erase)
 *   - JIT memory allocation via memmap_allocate_jit_memory
 *   - compile_or_revalidate_block / discard_and_recompile_block
 *   - compile_asm_functions (calls into backend's emit_asm_functions hook)
 *   - SMC fastmem backpatching (handle_fastmem_exception + slow-path stub
 *     emit + per-PC faulting record + binary-searchable backpatch table +
 *     page-fault-handler integration).  End-to-end live; exercised every
 *     game boot via Bus's mmap fastmem arena.
 *
 * cpu_code_cache_execute() falls through to the interpreter when no backend
 * has registered (g_cpu_compiler == NULL || g_cpu_compiler->v == NULL).
 */

#include "cpu_code_cache.h"

#include "bus.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "common/small_string.h"
#include "cpu_core_private.h"
#include "cpu_recompiler.h"
#include "settings.h"
#include "system.h"
#include "x64_backend.h"
#include "arm64_backend.h"

#include "x64_emit.h"

#include "util/page_fault_handler.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/memmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CodeCache);

enum {

  RECOMPILE_COUNT_FOR_INTERPRETER_FALLBACK   = 3,
  RECOMPILE_FRAMES_FOR_INTERPRETER_FALLBACK  = 15,
  INVALIDATE_COUNT_FOR_MANUAL_PROTECTION     = 4,
  INVALIDATE_FRAMES_FOR_MANUAL_PROTECTION    = 60,

  RECOMPILER_CODE_CACHE_SIZE     = 48u * 1024u * 1024u,
  RECOMPILER_FAR_CODE_CACHE_SIZE = 16u * 1024u * 1024u,
};

struct cpu_code_cache_block_link_entry {
  u32   pc;
  void* code;
  bool  dead;
};

static cpu_code_cache_block_link_entry_t* s_block_links = NULL;
static u32 s_block_links_count = 0;
static u32 s_block_links_cap   = 0;

static u32 block_links_emplace(u32 pc, void* code)
{
  /* Try to reuse a dead slot first. */
  for (u32 i = 0; i < s_block_links_count; i++) {
    if (s_block_links[i].dead) {
      s_block_links[i].pc = pc;
      s_block_links[i].code = code;
      s_block_links[i].dead = false;
      return i;
    }
  }
  if (s_block_links_count == s_block_links_cap) {
    u32 newcap = s_block_links_cap ? s_block_links_cap * 2u : 256u;
    cpu_code_cache_block_link_entry_t* nb = (cpu_code_cache_block_link_entry_t*)
        realloc(s_block_links, newcap * sizeof(*s_block_links));
    AssertMsg(nb != NULL, "block-link map allocation failed");
    s_block_links = nb;
    s_block_links_cap = newcap;
  }
  const u32 idx = s_block_links_count++;
  s_block_links[idx].pc = pc;
  s_block_links[idx].code = code;
  s_block_links[idx].dead = false;
  return idx;
}

static void block_links_erase(u32 idx)
{
  if (idx < s_block_links_count)
    s_block_links[idx].dead = true;
}

static void block_links_clear(void)
{
  s_block_links_count = 0u;
}

const void** g_code_lut[CPU_CODE_CACHE_LUT_TABLE_COUNT];
static cpu_code_cache_block_t** s_block_lut[CPU_CODE_CACHE_LUT_TABLE_COUNT];
static const void**           s_lut_code_pointers   = NULL;
static cpu_code_cache_block_t** s_lut_block_pointers = NULL;

/* Reachable PC ranges (RAM mirrors + EXP1 + BIOS). */
static const u32 s_lut_ranges[][2] = {
  {0x00000000u, 0x00800000u}, /* RAM */
  {0x1F000000u, 0x1F060000u}, /* EXP1 */
  {0x1FC00000u, 0x1FC80000u}, /* BIOS */
  {0x80000000u, 0x80800000u},
  {0x9F000000u, 0x9F060000u},
  {0x9FC00000u, 0x9FC80000u},
  {0xA0000000u, 0xA0800000u},
  {0xBF000000u, 0xBF060000u},
  {0xBFC00000u, 0xBFC80000u},
};
#define LUT_RANGE_COUNT (sizeof(s_lut_ranges) / sizeof(s_lut_ranges[0]))

static u32 lut_table_count(u32 start, u32 end)
{
  return ((end >> CPU_CODE_CACHE_LUT_TABLE_SHIFT) - (start >> CPU_CODE_CACHE_LUT_TABLE_SHIFT)) + 1u;
}

static u32 lut_slot_count(bool include_unreachable)
{
  u32 tables = include_unreachable ? 1u : 0u;
  for (u32 i = 0; i < LUT_RANGE_COUNT; i++)
    tables += lut_table_count(s_lut_ranges[i][0], s_lut_ranges[i][1]);
  return tables * CPU_CODE_CACHE_LUT_TABLE_SIZE;
}

static void memset_ptrs(const void** dst, const void* val, u32 count)
{
  for (u32 i = 0; i < count; i++)
    dst[i] = val;
}

static u8* s_code_buffer_ptr = NULL;
static u8* s_code_ptr        = NULL;
static u8* s_free_code_ptr   = NULL;
static u32 s_code_size       = 0;
static u32 s_code_used       = 0;

static u8* s_far_code_ptr      = NULL;
static u8* s_free_far_code_ptr = NULL;
static u32 s_far_code_size     = 0;
static u32 s_far_code_used     = 0;

static void fastmem_backpatch_clear_all(void);

/* Low-RAM write watcher.  When CUPID_TRACE_LOW_RAM=1, every slow-path
 * bus write whose paddr falls in the configured range fires.  Range is
 * set via CUPID_TRACE_LOW_RAM_RANGE="hex:size" (default "0x34:12" =
 * BIOS-exception-save kernel-scratch [0x34, 0x40)).
 * CUPID_TRACE_LOW_RAM_LOG=<path> redirects to a file.
 * CUPID_TRACE_LOW_RAM_RING=N keeps only the LAST N events (stream mode is
 * default).  Useful when a hot busy-loop dominates: ringbuf captures the
 * pre-freeze writes; dumped at exit via cpu_code_cache_dbg_low_ram_dump().
 * CUPID_TRACE_LOW_RAM_SKIP_WRITERS="lo:hi"; skip events whose writer_pc
 * falls in [lo, hi), useful for filtering out BIOS busy-loop counters.
 * Each line: paddr size value writer_pc prev_block_pc | pc ra sp v0 v1
 *   k0 k1 cause
 */
static u32      s_low_ram_watch_lo  = 0x34u;     /* BIOS exception-save scratch */
static u32      s_low_ram_watch_hi  = 0x40u;
static u32      s_low_ram_watch_skip_writer_lo = 0u;
static u32      s_low_ram_watch_skip_writer_hi = 0u;  /* lo>=hi => no skip */
static FILE*    s_low_ram_watch_fp  = NULL;       /* set in install; defaults to stderr */

/* Streaming + optional last-N ringbuf.  When ring_cap == 0, we stream every
 * event directly.  When ring_cap > 0, we accumulate into a ring and dump on
 * shutdown / explicit dump. */
typedef struct {
  u32 paddr, value, writer_pc, prev_block, pc, ra, sp, v0, v1, k0, k1, cause;
  u8  size;
} cpu_code_cache_low_ram_event_t;
static cpu_code_cache_low_ram_event_t* s_low_ram_ring = NULL;
static u32  s_low_ram_ring_cap = 0u;     /* 0 = stream mode */
static u32  s_low_ram_ring_pos = 0u;     /* next write slot */
static u64  s_low_ram_ring_total = 0u;   /* lifetime event count */

/* Forward decl; defined below near s_dbg_exec_ring. */
static u32 cpu_code_cache_dbg_exec_ring_peek_last_pc(void);

static void cpu_code_cache_dbg_low_ram_print_event(FILE* fp,
    const cpu_code_cache_low_ram_event_t* e, u64 idx)
{
  fprintf(fp,
          "[low-ram-watch] #%llu paddr=%08X size=%u value=%08X writer_pc=%08X "
          "prev_block=%08X | pc=%08X ra=%08X sp=%08X v0=%08X v1=%08X "
          "k0=%08X k1=%08X cause=%08X\n",
          (unsigned long long)idx, e->paddr, (unsigned)e->size, e->value, e->writer_pc,
          e->prev_block, e->pc, e->ra, e->sp, e->v0, e->v1, e->k0, e->k1, e->cause);
}

static void cpu_code_cache_dbg_low_ram_watch(u32 paddr, u8 size, u32 value, u32 writer_pc)
{
  if (!(paddr >= s_low_ram_watch_lo && paddr < s_low_ram_watch_hi)) return;
  if (s_low_ram_watch_skip_writer_lo < s_low_ram_watch_skip_writer_hi &&
      writer_pc >= s_low_ram_watch_skip_writer_lo &&
      writer_pc <  s_low_ram_watch_skip_writer_hi)
    return;

  cpu_code_cache_low_ram_event_t ev;
  ev.paddr      = paddr;
  ev.size       = size;
  ev.value      = value;
  ev.writer_pc  = writer_pc;
  ev.prev_block = cpu_code_cache_dbg_exec_ring_peek_last_pc();
  ev.pc         = g_cpu_state.pc;
  ev.ra         = g_cpu_state.regs.r[31];
  ev.sp         = g_cpu_state.regs.r[29];
  ev.v0         = g_cpu_state.regs.r[2];
  ev.v1         = g_cpu_state.regs.r[3];
  ev.k0         = g_cpu_state.regs.r[26];
  ev.k1         = g_cpu_state.regs.r[27];
  ev.cause      = g_cpu_state.cop0_regs.cause.bits;

  if (s_low_ram_ring_cap == 0u) {
    /* Stream mode: print directly, capped at 10M lifetime to avoid runaway. */
    if (s_low_ram_ring_total >= 10000000ull) return;
    s_low_ram_ring_total++;
    FILE* fp = s_low_ram_watch_fp ? s_low_ram_watch_fp : stderr;
    cpu_code_cache_dbg_low_ram_print_event(fp, &ev, s_low_ram_ring_total);
    fflush(fp);
  } else {
    /* Ring mode: drop into ring, no print until dump. */
    s_low_ram_ring[s_low_ram_ring_pos] = ev;
    s_low_ram_ring_pos = (s_low_ram_ring_pos + 1u) % s_low_ram_ring_cap;
    s_low_ram_ring_total++;
  }

  /* BCD trip detector.  When a single byte is written to one of the
   * MSF-byte stack slots and the high nibble exceeds 9 (or the seconds-
   * slot exceeds 5, frames-slot exceeds 7), dump full GPR state + a small
   * stack-RAM window.  These are the exact patterns the CDROM "Invalid
   * seek" rejection sees.  Caps at N alerts per session.  Triggers only
   * when CUPID_TRACE_LOW_RAM_BCDTRIP=1 is set.
   *
   * Alt-mode: CUPID_TRACE_LOW_RAM_TRIP_PADDR=<hex_paddr> triggers the same
   * full dump on every write to that exact paddr regardless of value.
   * Lets interp + recomp produce symmetric dumps at the same write moment
   * for differential diagnosis. */
  static u8 s_bcd_trip_armed = 0xFFu;     /* 0xFF means "not yet sniffed env" */
  static u32 s_bcd_alert_count = 0u;
  static u32 s_trip_paddr = 0xFFFFFFFFu;
  if (s_bcd_trip_armed == 0xFFu) {
    const char* en = getenv("CUPID_TRACE_LOW_RAM_BCDTRIP");
    s_bcd_trip_armed = (en && en[0] != '\0' && en[0] != '0') ? 1u : 0u;
    const char* tp = getenv("CUPID_TRACE_LOW_RAM_TRIP_PADDR");
    if (tp && tp[0] != '\0') s_trip_paddr = (u32)strtoul(tp, NULL, 0);
  }
  if (s_bcd_alert_count < 32u && (
      (s_bcd_trip_armed && size == 1u &&
       ((((value >> 4) & 0xFu) > 9u) || ((value & 0xFu) > 9u))) ||
      (s_trip_paddr != 0xFFFFFFFFu && paddr == s_trip_paddr))) {
    bool tripped = true;  /* condition already satisfied above */
    if (tripped) {
      s_bcd_alert_count++;
      FILE* fp = s_low_ram_watch_fp ? s_low_ram_watch_fp : stderr;
      fprintf(fp, "\n[bcd-trip #%u] BAD BCD byte 0x%02X to paddr=%08X writer_pc=%08X "
                  "prev_block=%08X pc=%08X\n",
              s_bcd_alert_count, (value & 0xFFu), paddr, writer_pc,
              cpu_code_cache_dbg_exec_ring_peek_last_pc(), g_cpu_state.pc);
      fprintf(fp, "  GPRs: ");
      for (int i = 0; i < 32; ++i) {
        fprintf(fp, "r%d=%08X%s", i, g_cpu_state.regs.r[i],
                ((i % 8) == 7) ? "\n        " : " ");
      }
      fprintf(fp, "\n  HI=%08X LO=%08X EPC=%08X cause=%08X sr=%08X badva=%08X\n",
              g_cpu_state.regs.named.hi, g_cpu_state.regs.named.lo,
              g_cpu_state.cop0_regs.EPC,
              g_cpu_state.cop0_regs.cause.bits,
              g_cpu_state.cop0_regs.sr.bits,
              g_cpu_state.cop0_regs.BadVaddr);

      /* Dump 16 MIPS instructions starting at prev_block PC; this is the
       * function that just finished executing.  Use unprotected RAM ptr so
       * we don't trip SMC. */
      const u32 prev_pc = cpu_code_cache_dbg_exec_ring_peek_last_pc();
      const u8* ram = bus_get_unprotected_ram_pointer();
      const u32 ram_size = bus_get_ram_size();
      if (ram && (prev_pc & 0x1FFFFFFFu) + 64u <= ram_size) {
        fprintf(fp, "  prev_block disasm (16 inst from %08X):\n", prev_pc);
        for (u32 i = 0u; i < 16u; ++i) {
          const u32 paddr2 = (prev_pc & 0x1FFFFFFFu) + i * 4u;
          u32 word;
          memcpy(&word, &ram[paddr2], 4u);
          small_string_t buf;
          small_string_init(&buf);
          cpu_disassemble(&buf, word, prev_pc + i * 4u);
          fprintf(fp, "    %08X: %08X  %s\n",
                  prev_pc + i * 4u, word, small_string_c_str(&buf));
          small_string_destroy(&buf);
        }
      }
      /* Dump 32 bytes around sp; the stack frame holds local vars. */
      const u32 sp = g_cpu_state.regs.r[29];
      if (ram && (sp & 0x1FFFFFFFu) + 64u <= ram_size) {
        fprintf(fp, "  stack dump (sp=%08X..+64):\n   ", sp);
        for (u32 i = 0u; i < 64u; i += 4u) {
          u32 word;
          memcpy(&word, &ram[(sp & 0x1FFFFFFFu) + i], 4u);
          fprintf(fp, " %08X", word);
          if ((i & 0xFu) == 0xCu) fprintf(fp, "\n   ");
        }
        fprintf(fp, "\n");
      }
      fflush(fp);
    }
  }
}

void cpu_code_cache_dbg_low_ram_dump(void)
{
  if (s_low_ram_ring_cap == 0u) return;
  FILE* fp = s_low_ram_watch_fp ? s_low_ram_watch_fp : stderr;
  fprintf(fp,
          "[low-ram-watch] ringbuf dump: total=%llu cap=%u "
          "(showing last min(total,cap) events oldest-first)\n",
          (unsigned long long)s_low_ram_ring_total, s_low_ram_ring_cap);
  const u32 n = (s_low_ram_ring_total < (u64)s_low_ram_ring_cap) ?
                  (u32)s_low_ram_ring_total : s_low_ram_ring_cap;
  /* If total > cap, the oldest entry is at s_low_ram_ring_pos.  Otherwise
   * it's at index 0. */
  const u32 start = (s_low_ram_ring_total > (u64)s_low_ram_ring_cap)
                      ? s_low_ram_ring_pos : 0u;
  const u64 base_idx = (s_low_ram_ring_total > (u64)s_low_ram_ring_cap)
                         ? (s_low_ram_ring_total - (u64)s_low_ram_ring_cap)
                         : 0ull;
  for (u32 i = 0u; i < n; ++i) {
    const u32 slot = (start + i) % s_low_ram_ring_cap;
    cpu_code_cache_dbg_low_ram_print_event(fp, &s_low_ram_ring[slot],
                                           base_idx + (u64)i + 1ull);
  }
  fflush(fp);
}

/* Parse "0x1FFF80:4" or "0x34:12" -> [lo, lo+size).  Returns true on parse OK. */
static bool cpu_code_cache_dbg_parse_low_ram_range(const char* s, u32* lo, u32* hi)
{
  if (s == NULL || s[0] == '\0') return false;
  char* end = NULL;
  unsigned long base = strtoul(s, &end, 0);
  if (end == s) return false;
  if (*end != ':') {
    *lo = (u32)base;
    *hi = (u32)base + 4u;       /* default size = 4 bytes if no ":N" */
    return true;
  }
  ++end;
  unsigned long sz = strtoul(end, NULL, 0);
  if (sz == 0u) sz = 4u;
  *lo = (u32)base;
  *hi = (u32)base + (u32)sz;
  return true;
}

void cpu_code_cache_dbg_maybe_install_low_ram_watch(void)
{
  const char* env = getenv("CUPID_TRACE_LOW_RAM");
  if (env == NULL || env[0] == '\0' || env[0] == '0') return;
  /* Avoid clobbering the diff harness tap. */
  if (g_bus_ram_write_tap != NULL) {
    fprintf(stderr,
            "[low-ram-watch] g_bus_ram_write_tap already installed; "
            "low-ram watch NOT enabled (run without CUPID_RECOMP_DIFF)\n");
    return;
  }
  /* Optional range override.  Keeps the kernel-scratch default if unset. */
  const char* range_env = getenv("CUPID_TRACE_LOW_RAM_RANGE");
  if (range_env != NULL && range_env[0] != '\0') {
    u32 lo = 0u, hi = 0u;
    if (cpu_code_cache_dbg_parse_low_ram_range(range_env, &lo, &hi)) {
      s_low_ram_watch_lo = lo;
      s_low_ram_watch_hi = hi;
    } else {
      fprintf(stderr,
              "[low-ram-watch] failed to parse CUPID_TRACE_LOW_RAM_RANGE='%s'; "
              "using default [%08X..%08X)\n",
              range_env, s_low_ram_watch_lo, s_low_ram_watch_hi);
    }
  }
  /* Optional log-file redirection. */
  const char* log_env = getenv("CUPID_TRACE_LOW_RAM_LOG");
  if (log_env != NULL && log_env[0] != '\0') {
    FILE* fp = fopen(log_env, "w");
    if (fp != NULL) s_low_ram_watch_fp = fp;
    else fprintf(stderr, "[low-ram-watch] fopen('%s') failed; logging to stderr\n",
                 log_env);
  }
  /* Optional ring-mode capacity (last-N events). */
  const char* ring_env = getenv("CUPID_TRACE_LOW_RAM_RING");
  if (ring_env != NULL && ring_env[0] != '\0') {
    unsigned long cap = strtoul(ring_env, NULL, 0);
    if (cap > 0ul && cap <= (1ul << 24)) {
      s_low_ram_ring_cap = (u32)cap;
      s_low_ram_ring = (cpu_code_cache_low_ram_event_t*)
        calloc(s_low_ram_ring_cap, sizeof(*s_low_ram_ring));
      if (s_low_ram_ring == NULL) {
        fprintf(stderr, "[low-ram-watch] ring alloc failed; falling back to stream mode\n");
        s_low_ram_ring_cap = 0u;
      }
    }
  }
  /* Optional writer-PC skip range. */
  const char* skip_env = getenv("CUPID_TRACE_LOW_RAM_SKIP_WRITERS");
  if (skip_env != NULL && skip_env[0] != '\0') {
    u32 lo = 0u, hi = 0u;
    if (cpu_code_cache_dbg_parse_low_ram_range(skip_env, &lo, &hi)) {
      s_low_ram_watch_skip_writer_lo = lo;
      s_low_ram_watch_skip_writer_hi = hi;
    }
  }
  g_bus_ram_write_tap = &cpu_code_cache_dbg_low_ram_watch;
  fprintf(stderr,
          "[low-ram-watch] installed; tracking slow-path RAM writes to "
          "paddr [%08X..%08X) -> %s (mode=%s%s%s)\n",
          s_low_ram_watch_lo, s_low_ram_watch_hi,
          s_low_ram_watch_fp ? log_env : "stderr",
          s_low_ram_ring_cap ? "ring" : "stream",
          s_low_ram_watch_skip_writer_hi ? " skip-writers=set" : "",
          s_low_ram_ring_cap ? "" : " (run with --fastmem off for full coverage)");
}

/* Per-block-execution ring (separate from the compile ring).  Records pc +
 * key GPRs at every block prologue when CUPID_TRACE_EXEC=1.  Captures the
 * actual EXECUTION history (the compile ring only shows first-compile of
 * unique pcs, not what the dispatcher dispatched to).  Enabled at startup
 * via env var; emit-side gate is the static `s_exec_trace_active` used by
 * the x64 backend's hook_begin_block. */
enum { CPU_CODE_CACHE_DBG_EXEC_RING_SIZE = 1024u };
typedef struct {
  u32 pc;
  u32 ra;
  u32 sp;
} cpu_code_cache_dbg_exec_entry_t;
static cpu_code_cache_dbg_exec_entry_t s_dbg_exec_ring[CPU_CODE_CACHE_DBG_EXEC_RING_SIZE];
static u32  s_dbg_exec_ring_pos   = 0u;
static u32  s_dbg_exec_ring_count = 0u;
static bool s_exec_trace_active   = false;

bool cpu_code_cache_exec_trace_is_active(void)
{
  return s_exec_trace_active;
}

void cpu_code_cache_dbg_init_exec_trace(void)
{
  const char* env = getenv("CUPID_TRACE_EXEC");
  if (env != NULL && env[0] != '\0' && env[0] != '0')
    s_exec_trace_active = true;
}

/* C-side recorder called from JIT block prologue when active. */
void cpu_code_cache_dbg_record_exec(u32 pc)
{
  cpu_code_cache_dbg_exec_entry_t* e =
    &s_dbg_exec_ring[s_dbg_exec_ring_pos % CPU_CODE_CACHE_DBG_EXEC_RING_SIZE];
  e->pc = pc;
  e->ra = g_cpu_state.regs.r[31];
  e->sp = g_cpu_state.regs.r[29];
  s_dbg_exec_ring_pos = (s_dbg_exec_ring_pos + 1u) % CPU_CODE_CACHE_DBG_EXEC_RING_SIZE;
  if (s_dbg_exec_ring_count < CPU_CODE_CACHE_DBG_EXEC_RING_SIZE)
    s_dbg_exec_ring_count++;
}

/* Returns the most-recently-recorded executed-block PC, or 0 if the ring
 * is empty (e.g. exec trace not active, or watch fired before first block). */
static u32 cpu_code_cache_dbg_exec_ring_peek_last_pc(void)
{
  if (s_dbg_exec_ring_count == 0u) return 0u;
  /* s_dbg_exec_ring_pos points at the next slot to write; the most recent
   * entry is at (pos - 1) mod SIZE. */
  const u32 last = (s_dbg_exec_ring_pos + CPU_CODE_CACHE_DBG_EXEC_RING_SIZE - 1u)
                   % CPU_CODE_CACHE_DBG_EXEC_RING_SIZE;
  return s_dbg_exec_ring[last].pc;
}

/* Diagnostic ring: last 256 compiled blocks. */
enum { CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE = 256u };
typedef struct {
  u32         pc;
  u32         size;        /* MIPS instruction count */
  const void* host_code;
} cpu_code_cache_dbg_block_entry_t;
static cpu_code_cache_dbg_block_entry_t s_dbg_block_ring[CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE];
static u32 s_dbg_block_ring_pos   = 0u; /* next-write index, monotonic mod */
static u32 s_dbg_block_ring_count = 0u; /* total writes; saturates at SIZE for "is full" */

static void cpu_code_cache_dbg_record_block(u32 pc, u32 size, const void* host_code)
{
  cpu_code_cache_dbg_block_entry_t* e =
    &s_dbg_block_ring[s_dbg_block_ring_pos % CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE];
  e->pc        = pc;
  e->size      = size;
  e->host_code = host_code;
  s_dbg_block_ring_pos = (s_dbg_block_ring_pos + 1u) % CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE;
  if (s_dbg_block_ring_count < CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE)
    s_dbg_block_ring_count++;
}

/* Forward decls for the dynamic backpatch buffer (defined further below
 * near the registration code), so process_shutdown can free it. */
static cpu_code_cache_loadstore_backpatch_info_t* s_fastmem_backpatch_info;
static u32 s_fastmem_backpatch_info_count;
static u32 s_fastmem_backpatch_info_cap;

static void reset_code_buffer(void)
{
  if (s_code_used > 0u) {
    memset(s_code_ptr, 0, s_code_used);
    memmap_flush_instruction_cache(s_code_ptr, s_code_used);
  }
  if (s_far_code_used > 0u) {
    memset(s_far_code_ptr, 0, s_far_code_used);
    memmap_flush_instruction_cache(s_far_code_ptr, s_far_code_used);
  }
  fastmem_backpatch_clear_all();

  s_code_ptr = s_code_buffer_ptr;
  s_free_code_ptr = s_code_ptr;
  s_code_size = RECOMPILER_CODE_CACHE_SIZE - RECOMPILER_FAR_CODE_CACHE_SIZE;
  s_code_used = 0u;

  /* Half the far-code budget when memory exceptions disabled (only used by
   * backpatch thunks then). */
  const u32 far = (!g_settings.cpu_recompiler_memory_exceptions) ?
                    (RECOMPILER_FAR_CODE_CACHE_SIZE / 2u) : RECOMPILER_FAR_CODE_CACHE_SIZE;
  s_far_code_size = far;
  s_far_code_ptr = (far > 0u) ? (s_code_ptr + s_code_size) : NULL;
  s_free_far_code_ptr = s_far_code_ptr;
  s_far_code_used = 0u;
}

u8* cpu_code_cache_get_free_code_pointer(void)        { return s_free_code_ptr; }
u32 cpu_code_cache_get_free_code_space(void)          { return s_code_size - s_code_used; }
u8* cpu_code_cache_get_free_far_code_pointer(void)    { return s_free_far_code_ptr; }
u32 cpu_code_cache_get_free_far_code_space(void)      { return s_far_code_size - s_far_code_used; }

void cpu_code_cache_commit_code(u32 length)
{
  if (length == 0u) return;
  memmap_flush_instruction_cache(s_free_code_ptr, length);
  Assert(length <= (s_code_size - s_code_used));
  s_free_code_ptr += length;
  s_code_used += length;
}

void cpu_code_cache_commit_far_code(u32 length)
{
  if (length == 0u) return;
  memmap_flush_instruction_cache(s_free_far_code_ptr, length);
  Assert(length <= (s_far_code_size - s_far_code_used));
  s_free_far_code_ptr += length;
  s_far_code_used += length;
}

void cpu_code_cache_align_code(u32 alignment)
{
  DebugAssert((alignment & (alignment - 1u)) == 0u);
  const uintptr_t cur = (uintptr_t)s_free_code_ptr;
  const uintptr_t aligned = (cur + (alignment - 1u)) & ~((uintptr_t)alignment - 1u);
  u32 pad = (u32)(aligned - cur);
  const u32 free = cpu_code_cache_get_free_code_space();
  if (pad > free) pad = free;
  if (pad > 0u) {
    memset(s_free_code_ptr, 0x90, pad);
  }
  s_free_code_ptr += pad;
  s_code_used += pad;
}

NORETURN void (*g_enter_recompiler)(void) = NULL;
const void* g_compile_or_revalidate_block = NULL;
const void* g_run_events_and_dispatch     = NULL;
const void* g_dispatcher                  = NULL;
const void* g_interpret_block             = NULL;
const void* g_discard_and_recompile_block = NULL;

static cpu_code_cache_block_t** s_blocks = NULL;
static u32 s_blocks_count = 0u;
static u32 s_blocks_cap   = 0u;

typedef struct {
  cpu_code_cache_block_t* first_block_in_page;
  cpu_code_cache_block_t* last_block_in_page;
  cpu_code_cache_page_protection_mode_t mode;
  u16 invalidate_count;
  u32 invalidate_frame;
} cpu_code_cache_page_prot_info_t;

static cpu_code_cache_page_prot_info_t s_page_protection[BUS_RAM_8MB_CODE_PAGE_COUNT];

/* Per-block compilation buffer reused to avoid allocations. */
typedef struct {
  cpu_instruction_t          inst;
  cpu_code_cache_inst_info_t info;
} cpu_code_cache_compile_pair_t;

static cpu_code_cache_compile_pair_t* s_block_instructions = NULL;
static u32 s_block_instructions_count = 0u;
static u32 s_block_instructions_cap   = 0u;

typedef struct {
  tick_count_t                  uncached_fetch_ticks;
  u32                           icache_line_count;
  cpu_code_cache_block_flags_t  flags;
} cpu_code_cache_block_metadata_t;

static void block_instructions_reset(void) { s_block_instructions_count = 0u; }

static void block_instructions_push(cpu_instruction_t inst, const cpu_code_cache_inst_info_t* info)
{
  if (s_block_instructions_count == s_block_instructions_cap) {
    u32 newcap = s_block_instructions_cap ? s_block_instructions_cap * 2u : 256u;
    cpu_code_cache_compile_pair_t* nb = (cpu_code_cache_compile_pair_t*)
        realloc(s_block_instructions, newcap * sizeof(*s_block_instructions));
    AssertMsg(nb != NULL, "block_instructions allocation failed");
    s_block_instructions = nb;
    s_block_instructions_cap = newcap;
  }
  s_block_instructions[s_block_instructions_count].inst = inst;
  s_block_instructions[s_block_instructions_count].info = *info;
  s_block_instructions_count++;
}

static void blocks_register(cpu_code_cache_block_t* b)
{
  if (s_blocks_count == s_blocks_cap) {
    u32 newcap = s_blocks_cap ? s_blocks_cap * 2u : 256u;
    cpu_code_cache_block_t** nb = (cpu_code_cache_block_t**)
        realloc(s_blocks, newcap * sizeof(*s_blocks));
    AssertMsg(nb != NULL, "blocks allocation failed");
    s_blocks = nb;
    s_blocks_cap = newcap;
  }
  s_blocks[s_blocks_count++] = b;
}

static void blocks_unregister(cpu_code_cache_block_t* b)
{
  for (u32 i = 0; i < s_blocks_count; i++) {
    if (s_blocks[i] == b) {
      s_blocks[i] = s_blocks[--s_blocks_count];
      return;
    }
  }
}

ALWAYS_INLINE static u32 block_start_page_index(const cpu_code_cache_block_t* b)
{
  return bus_get_ram_code_page_index(b->pc);
}
ALWAYS_INLINE static u32 block_end_page_index(const cpu_code_cache_block_t* b)
{
  return bus_get_ram_code_page_index(b->pc + ((b->size - 1u) * CPU_INSTRUCTION_SIZE));
}

static bool address_in_ram(virtual_memory_address_t pc)
{
  return cpu_virtual_to_physical(pc) < bus_get_ram_size();
}

static void allocate_luts(void)
{
  const u32 num_code_slots  = lut_slot_count(true);
  const u32 num_block_slots = lut_slot_count(false);

  Assert(s_lut_code_pointers == NULL && s_lut_block_pointers == NULL);
  s_lut_code_pointers   = (const void**)calloc(num_code_slots, sizeof(const void*));
  s_lut_block_pointers  = (cpu_code_cache_block_t**)calloc(num_block_slots, sizeof(cpu_code_cache_block_t*));
  AssertMsg(s_lut_code_pointers && s_lut_block_pointers, "LUT allocation failed");

  const void** code_table_ptr = s_lut_code_pointers;
  cpu_code_cache_block_t** block_table_ptr = s_lut_block_pointers;
  const void** const code_end = code_table_ptr + num_code_slots;
  cpu_code_cache_block_t** const block_end = block_table_ptr + num_block_slots;

  /* Unreachable table sits first, points at NULL (will be replaced by
   * g_interpret_block once asm functions are emitted). */
  memset_ptrs(code_table_ptr, NULL, CPU_CODE_CACHE_LUT_TABLE_COUNT);

  for (u32 i = 0; i < CPU_CODE_CACHE_LUT_TABLE_COUNT; i++) {
    g_code_lut[i] = code_table_ptr;
    s_block_lut[i] = NULL;
  }
  code_table_ptr += CPU_CODE_CACHE_LUT_TABLE_SIZE;

  for (u32 r = 0; r < LUT_RANGE_COUNT; r++) {
    const u32 start = s_lut_ranges[r][0];
    const u32 end   = s_lut_ranges[r][1];
    const u32 start_slot = start >> CPU_CODE_CACHE_LUT_TABLE_SHIFT;
    const u32 count = lut_table_count(start, end);
    for (u32 i = 0; i < count; i++) {
      const u32 slot = start_slot + i;
      g_code_lut[slot] = code_table_ptr;
      code_table_ptr += CPU_CODE_CACHE_LUT_TABLE_SIZE;
      s_block_lut[slot] = block_table_ptr;
      block_table_ptr += CPU_CODE_CACHE_LUT_TABLE_SIZE;
    }
  }

  Assert(code_table_ptr == code_end);
  Assert(block_table_ptr == block_end);
}

static void deallocate_luts(void)
{
  free(s_lut_code_pointers);   s_lut_code_pointers   = NULL;
  free(s_lut_block_pointers);  s_lut_block_pointers  = NULL;
}

static void reset_code_lut(void)
{
  /* Unreachable table jumps to interpret_block. */
  memset_ptrs(s_lut_code_pointers, g_interpret_block, CPU_CODE_CACHE_LUT_TABLE_COUNT);

  for (u32 i = 0; i < CPU_CODE_CACHE_LUT_TABLE_COUNT; i++) {
    const void** ptr = g_code_lut[i];
    if (ptr == s_lut_code_pointers) continue; /* unreachable bucket */
    memset_ptrs(ptr, g_compile_or_revalidate_block, CPU_CODE_CACHE_LUT_TABLE_SIZE);
  }
}

static void set_code_lut(u32 pc, const void* function)
{
  const u32 table = pc >> CPU_CODE_CACHE_LUT_TABLE_SHIFT;
  const u32 idx   = (pc & 0xFFFFu) >> 2;
  DebugAssert(g_code_lut[table] != s_lut_code_pointers);
  g_code_lut[table][idx] = function;
}

static cpu_code_cache_block_t* lookup_block(u32 pc)
{
  const u32 table = pc >> CPU_CODE_CACHE_LUT_TABLE_SHIFT;
  if (s_block_lut[table] == NULL) return NULL;
  const u32 idx = (pc & 0xFFFFu) >> 2;
  return s_block_lut[table][idx];
}

static bool has_block_lut(u32 pc)
{
  return s_block_lut[pc >> CPU_CODE_CACHE_LUT_TABLE_SHIFT] != NULL;
}

static cpu_code_cache_page_protection_mode_t get_protection_mode_for_pc(u32 pc)
{
  if (!address_in_ram(pc))
    return CPU_CODE_CACHE_PROT_UNPROTECTED;
  return s_page_protection[bus_get_ram_code_page_index(pc)].mode;
}

static cpu_code_cache_page_protection_mode_t get_protection_mode_for_block(const cpu_code_cache_block_t* b)
{
  if (cpu_code_cache_block_has_flag(b, CPU_CODE_CACHE_BF_BRANCH_DELAY_SPANS_PAGES))
    return CPU_CODE_CACHE_PROT_MANUAL_CHECK;
  return get_protection_mode_for_pc(b->pc);
}

static void backlink_blocks(u32 pc, const void* dst);

static void invalidate_block(cpu_code_cache_block_t* b, cpu_code_cache_block_state_t new_state)
{
  if (b->state == CPU_CODE_CACHE_BS_VALID) {
    set_code_lut(b->pc, g_compile_or_revalidate_block);
    backlink_blocks(b->pc, g_compile_or_revalidate_block);
  }
  b->state = new_state;
}

static void add_block_to_page_list(cpu_code_cache_block_t* b)
{
  if (b->size == 0u) return;
  if (!address_in_ram(b->pc) || b->protection != CPU_CODE_CACHE_PROT_WRITE_PROTECTED)
    return;

  const u32 page_idx = block_start_page_index(b);
  cpu_code_cache_page_prot_info_t* p = &s_page_protection[page_idx];
  bus_set_ram_code_page(page_idx);

  if (p->last_block_in_page) {
    p->last_block_in_page->next_block_in_page = b;
    p->last_block_in_page = b;
  } else {
    p->first_block_in_page = b;
    p->last_block_in_page  = b;
  }
}

static void remove_block_from_page_list(cpu_code_cache_block_t* b)
{
  if (b->size == 0u) return;
  if (!address_in_ram(b->pc) || b->protection != CPU_CODE_CACHE_PROT_WRITE_PROTECTED)
    return;

  const u32 page_idx = block_start_page_index(b);
  cpu_code_cache_page_prot_info_t* p = &s_page_protection[page_idx];
  cpu_code_cache_block_t* prev = NULL;
  cpu_code_cache_block_t* cur  = p->first_block_in_page;
  while (cur != NULL) {
    if (cur != b) { prev = cur; cur = cur->next_block_in_page; continue; }
    if (prev) prev->next_block_in_page = cur->next_block_in_page;
    else      p->first_block_in_page  = cur->next_block_in_page;
    if (cur->next_block_in_page == NULL) p->last_block_in_page = prev;
    cur->next_block_in_page = NULL;
    return;
  }
}

void cpu_code_cache_invalidate_blocks_with_page_index(u32 page_index)
{
  if (page_index >= BUS_RAM_8MB_CODE_PAGE_COUNT) return;
  bus_clear_ram_code_page(page_index);

  cpu_code_cache_block_state_t new_state = CPU_CODE_CACHE_BS_INVALIDATED;
  cpu_code_cache_page_prot_info_t* p = &s_page_protection[page_index];

  const u32 frame = system_get_frame_number();
  const u32 frame_delta = frame - p->invalidate_frame;
  p->invalidate_count++;
  if (frame_delta >= INVALIDATE_FRAMES_FOR_MANUAL_PROTECTION) {
    p->invalidate_count = 1u;
    p->invalidate_frame = frame;
  } else if (p->invalidate_count > INVALIDATE_COUNT_FOR_MANUAL_PROTECTION) {
    p->mode = CPU_CODE_CACHE_PROT_MANUAL_CHECK;
    new_state = CPU_CODE_CACHE_BS_NEEDS_RECOMPILE;
  }

  if (p->first_block_in_page == NULL) return;

  cpu_code_cache_block_t* b = p->first_block_in_page;
  while (b != NULL) {
    cpu_code_cache_block_t* next = b->next_block_in_page;
    invalidate_block(b, new_state);
    b->next_block_in_page = NULL;
    b = next;
  }
  p->first_block_in_page = NULL;
  p->last_block_in_page  = NULL;
}

void cpu_code_cache_invalidate_all_ram_blocks(void)
{
  for (u32 i = 0; i < s_blocks_count; i++) {
    cpu_code_cache_block_t* b = s_blocks[i];
    if (address_in_ram(b->pc)) {
      invalidate_block(b, CPU_CODE_CACHE_BS_INVALIDATED);
      b->next_block_in_page = NULL;
    }
  }
  for (u32 i = 0; i < BUS_RAM_8MB_CODE_PAGE_COUNT; i++) {
    s_page_protection[i].first_block_in_page = NULL;
    s_page_protection[i].last_block_in_page  = NULL;
  }
  bus_clear_ram_code_page_flags();
}

static bool is_block_code_current(const cpu_code_cache_block_t* b)
{
  const u32 phys = cpu_virtual_to_physical(b->pc);
  DebugAssert((phys + (CPU_INSTRUCTION_SIZE * b->size)) <= bus_get_ram_size());
  return memcmp(bus_get_ram_pointer() + phys,
                cpu_code_cache_block_instructions_const(b),
                CPU_INSTRUCTION_SIZE * b->size) == 0;
}

static bool revalidate_block(cpu_code_cache_block_t* b)
{
  DebugAssert(b->state != CPU_CODE_CACHE_BS_VALID);
  if (b->state >= CPU_CODE_CACHE_BS_NEEDS_RECOMPILE) return false;
  if (b->protection != get_protection_mode_for_block(b)) return false;
  if (!is_block_code_current(b)) return false;

  b->state = CPU_CODE_CACHE_BS_VALID;
  add_block_to_page_list(b);
  return true;
}

static void clear_blocks(void)
{
  for (u32 i = 0; i < BUS_RAM_8MB_CODE_PAGE_COUNT; i++) {
    cpu_code_cache_page_prot_info_t* p = &s_page_protection[i];
    if (p->mode == CPU_CODE_CACHE_PROT_WRITE_PROTECTED && p->first_block_in_page)
      bus_clear_ram_code_page(i);
    memset(p, 0, sizeof(*p));
  }

  block_links_clear();

  for (u32 i = 0; i < s_blocks_count; i++)
    free(s_blocks[i]);
  s_blocks_count = 0u;

  if (s_lut_block_pointers)
    memset(s_lut_block_pointers, 0, sizeof(cpu_code_cache_block_t*) * lut_slot_count(false));
}

static void fill_block_reg_info(cpu_code_cache_block_t* block);

static cpu_code_cache_block_t* create_block(u32 pc, const cpu_code_cache_compile_pair_t* instructions,
                                            u32 size, const cpu_code_cache_block_metadata_t* metadata)
{
  const u32 table = pc >> CPU_CODE_CACHE_LUT_TABLE_SHIFT;
  Assert(s_block_lut[table] != NULL);
  const u32 idx = (pc & 0xFFFFu) >> 2;

  const u32 frame = system_get_frame_number();
  u32 recompile_frame = frame;
  u8  recompile_count = 0u;

  cpu_code_cache_block_t* block = s_block_lut[table][idx];
  if (block != NULL) {
    Assert(block->next_block_in_page == NULL);
    recompile_frame = block->compile_frame;
    recompile_count = block->compile_count;
    if (block->size != size) {
      blocks_unregister(block);
      free(block);
      block = NULL;
    }
  }

  if (block == NULL) {
    const size_t bytes = sizeof(cpu_code_cache_block_t) +
                         CPU_INSTRUCTION_SIZE * size +
                         sizeof(cpu_code_cache_inst_info_t) * size;
    /* posix_memalign for 16-byte alignment (alignas(16) struct). */
    void* mem = NULL;
    if (posix_memalign(&mem, 16u, bytes) != 0) mem = NULL;
    AssertMsg(mem != NULL, "block allocation failed");
    block = (cpu_code_cache_block_t*)mem;
    memset(block, 0, sizeof(*block));
    blocks_register(block);
  }

  block->pc = pc;
  block->size = size;
  block->host_code = NULL;
  block->next_block_in_page = NULL;
  block->num_exit_links = 0u;
  block->state = CPU_CODE_CACHE_BS_VALID;
  block->flags = metadata->flags;
  block->protection = get_protection_mode_for_block(block);
  block->uncached_fetch_ticks = metadata->uncached_fetch_ticks;
  block->icache_line_count = metadata->icache_line_count;
  block->host_code_size = 0u;
  block->compile_frame = recompile_frame;
  block->compile_count = recompile_count + 1u;

  /* Copy decoded instructions + their reg-flow info into trailing arrays. */
  cpu_instruction_t* dst_inst = cpu_code_cache_block_instructions(block);
  cpu_code_cache_inst_info_t* dst_info = cpu_code_cache_block_instructions_info(block);
  for (u32 i = 0; i < size; i++) {
    dst_inst[i].bits = instructions[i].inst.bits;
    dst_info[i] = instructions[i].info;
  }

  s_block_lut[table][idx] = block;

  /* Ring-log every block landing in the LUT.  Records pre-recompile-
   * storm-guard size so the diagnostic shows what the JIT actually emitted
   * (not the size=0 marker the storm guard may set further down). */
  cpu_code_cache_dbg_record_block(pc, size, NULL);

  /* Recompile-storm guard: if we've been recompiling the same PC too often,
   * mark it as fallback-to-interpreter. */
  const u32 frame_delta = frame - recompile_frame;
  if (frame_delta >= RECOMPILE_FRAMES_FOR_INTERPRETER_FALLBACK) {
    block->compile_frame = frame;
    block->compile_count = 1u;
  } else if (block->compile_count >= RECOMPILE_COUNT_FOR_INTERPRETER_FALLBACK) {
    block->size = 0u;
  }

  if (block->size == 0u) {
    block->state = CPU_CODE_CACHE_BS_FALLBACK_TO_INTERPRETER;
    block->protection = CPU_CODE_CACHE_PROT_UNPROTECTED;
    return block;
  }

  fill_block_reg_info(block);
  add_block_to_page_list(block);
  return block;
}

static bool read_block_instructions(u32 start_pc, cpu_code_cache_block_metadata_t* metadata)
{
  const cpu_code_cache_page_protection_mode_t protection = get_protection_mode_for_pc(start_pc);
  const bool use_icache = cpu_is_cached_address(start_pc);
  const bool dynamic_fetch_ticks = !use_icache &&
      bus_get_memory_access_time_ptr(cpu_virtual_to_physical(start_pc), MEMORY_ACCESS_SIZE_WORD) != NULL;
  u32 pc = start_pc;
  bool is_branch_delay_slot = false;
  bool is_load_delay_slot = false;

  block_instructions_reset();
  metadata->icache_line_count = 0u;
  metadata->uncached_fetch_ticks = 0;
   metadata->flags = use_icache ? CPU_CODE_CACHE_BF_IS_USING_ICACHE :
                    (dynamic_fetch_ticks ? CPU_CODE_CACHE_BF_NEEDS_DYNAMIC_FETCH_TICKS : 
                                           CPU_CODE_CACHE_BF_NONE);

  u32 last_cache_line = (u32)-1;
  u32 last_page = (protection == CPU_CODE_CACHE_PROT_WRITE_PROTECTED) ?
                    bus_get_ram_code_page_index(start_pc) : 0u;

  for (;;) {
    if (protection == CPU_CODE_CACHE_PROT_WRITE_PROTECTED) {
      const u32 this_page = bus_get_ram_code_page_index(pc);
      if (this_page != last_page) {
        if (!is_branch_delay_slot) {
          metadata->flags = (cpu_code_cache_block_flags_t)
              ((u32)metadata->flags | CPU_CODE_CACHE_BF_SPANS_PAGES);
          break;
        } else {
          metadata->flags = (cpu_code_cache_block_flags_t)
              ((u32)metadata->flags | CPU_CODE_CACHE_BF_BRANCH_DELAY_SPANS_PAGES);
        }
      }
    }

    cpu_instruction_t inst = {.bits = 0u};
    if (!cpu_safe_read_instruction(pc, &inst.bits) || !cpu_is_valid_instruction(inst)) {
      ERROR_LOG("Instruction read failed at PC=0x%08X, truncating block", pc);
      if (is_branch_delay_slot && s_block_instructions_count > 0u)
        s_block_instructions_count--;
      break;
    }

    cpu_code_cache_inst_info_t info;
    memset(&info, 0, sizeof(info));
    info.is_branch_delay_slot              = is_branch_delay_slot ? 1u : 0u;
    info.is_load_delay_slot                = is_load_delay_slot   ? 1u : 0u;
    info.is_branch_instruction             = cpu_is_branch_instruction(inst) ? 1u : 0u;
    info.is_direct_branch_instruction      = cpu_is_direct_branch_instruction(inst) ? 1u : 0u;
    info.is_unconditional_branch_instruction = cpu_is_unconditional_branch_instruction(inst) ? 1u : 0u;
    info.is_load_instruction               = cpu_is_memory_load_instruction(inst) ? 1u : 0u;
    info.is_store_instruction              = cpu_is_memory_store_instruction(inst) ? 1u : 0u;
    info.has_load_delay                    = cpu_instruction_has_load_delay(inst) ? 1u : 0u;

    if (use_icache) {
      if (g_settings.cpu_recompiler_icache) {
        const u32 line = cpu_get_icache_line(pc);
        if (line != last_cache_line) { metadata->icache_line_count++; last_cache_line = line; }
      }
    } else if (!dynamic_fetch_ticks) {
      metadata->uncached_fetch_ticks += cpu_get_instruction_read_ticks(pc);
    }

    if (info.is_load_instruction || info.is_store_instruction)
      metadata->flags = (cpu_code_cache_block_flags_t)
          ((u32)metadata->flags | CPU_CODE_CACHE_BF_CONTAINS_LOADSTORE);

    pc += CPU_INSTRUCTION_SIZE;

    if (is_branch_delay_slot && info.is_branch_instruction) {
      WARNING_LOG("Branch in delay slot at %08X, skipping block", pc);
      return false;
    }

    block_instructions_push(inst, &info);

    if (is_branch_delay_slot && !info.is_branch_instruction)
      break;

    is_branch_delay_slot = info.is_branch_instruction;
    is_load_delay_slot = info.has_load_delay;

    if (cpu_is_exit_block_instruction(inst))
      break;
  }

  if (s_block_instructions_count == 0u) {
    WARNING_LOG("Empty block compiled at 0x%08X", start_pc);
    return false;
  }
  s_block_instructions[s_block_instructions_count - 1u].info.is_last_instruction = 1u;
  return true;
}

static void copy_reg_info(cpu_code_cache_inst_info_t* dst, const cpu_code_cache_inst_info_t* src)
{
  memcpy(dst->reg_flags, src->reg_flags, sizeof(dst->reg_flags));
  memcpy(dst->read_reg, src->read_reg, sizeof(dst->read_reg));
}

static void set_reg_access(cpu_code_cache_inst_info_t* inst, cpu_reg_t r, bool write)
{
  if ((u32)r == 0u) return;
  if (!write) {
    for (u32 i = 0; i < (u32)(sizeof(inst->read_reg) / sizeof(inst->read_reg[0])); i++) {
      if ((u32)inst->read_reg[i] == 0u) { inst->read_reg[i] = r; break; }
    }
  }
}

#define BPSET_READS(REG) do { \
    if (!(inst->reg_flags[(u32)(REG)] & CPU_CODE_CACHE_RI_USED)) \
       inst->reg_flags[(u32)(REG)] |= CPU_CODE_CACHE_RI_LASTUSE; \
    prev->reg_flags[(u32)(REG)] |= CPU_CODE_CACHE_RI_LIVE | CPU_CODE_CACHE_RI_USED; \
    inst->reg_flags[(u32)(REG)] |= CPU_CODE_CACHE_RI_USED; \
    set_reg_access(inst, (REG), false); \
  } while (0)

#define BPSET_WRITES(REG) do { \
    prev->reg_flags[(u32)(REG)] &= (u8)~(CPU_CODE_CACHE_RI_LIVE | CPU_CODE_CACHE_RI_USED); \
    if (!(inst->reg_flags[(u32)(REG)] & CPU_CODE_CACHE_RI_USED)) \
       inst->reg_flags[(u32)(REG)] |= CPU_CODE_CACHE_RI_LASTUSE; \
    inst->reg_flags[(u32)(REG)] |= CPU_CODE_CACHE_RI_USED; \
    set_reg_access(inst, (REG), true); \
  } while (0)

static void fill_block_reg_info(cpu_code_cache_block_t* block)
{
  const cpu_instruction_t* iinst = cpu_code_cache_block_instructions(block) + (block->size - 1u);
  cpu_code_cache_inst_info_t* const start = cpu_code_cache_block_instructions_info(block);
  cpu_code_cache_inst_info_t* inst = start + (block->size - 1u);

  /* Last instruction: all regs live + no read tracking yet. */
  memset(inst->reg_flags, CPU_CODE_CACHE_RI_LIVE, sizeof(inst->reg_flags));
  memset(inst->read_reg, 0, sizeof(inst->read_reg));

  while (inst != start) {
    cpu_code_cache_inst_info_t* prev = inst - 1;
    copy_reg_info(prev, inst);

    const cpu_reg_t rs = (cpu_reg_t)iinst->r.rs;
    const cpu_reg_t rt = (cpu_reg_t)iinst->r.rt;

    switch ((cpu_instruction_op_t)iinst->any.op) {
      case CPU_OP_FUNCT: {
        const cpu_reg_t rd = (cpu_reg_t)iinst->r.rd;
        switch ((cpu_instruction_funct_t)iinst->r.funct) {
          case CPU_FUNCT_SLL: case CPU_FUNCT_SRL: case CPU_FUNCT_SRA:
            BPSET_WRITES(rd); BPSET_READS(rt); break;
          case CPU_FUNCT_SLLV: case CPU_FUNCT_SRLV: case CPU_FUNCT_SRAV:
          case CPU_FUNCT_ADD:  case CPU_FUNCT_ADDU:
          case CPU_FUNCT_SUB:  case CPU_FUNCT_SUBU:
          case CPU_FUNCT_AND:  case CPU_FUNCT_OR:
          case CPU_FUNCT_XOR:  case CPU_FUNCT_NOR:
          case CPU_FUNCT_SLT:  case CPU_FUNCT_SLTU:
            BPSET_WRITES(rd); BPSET_READS(rt); BPSET_READS(rs); break;
          case CPU_FUNCT_JR:    BPSET_READS(rs); break;
          case CPU_FUNCT_JALR:  BPSET_READS(rs); BPSET_WRITES(rd); break;
          case CPU_FUNCT_MFHI:  BPSET_WRITES(rd); BPSET_READS((cpu_reg_t)CPU_REG_HI); break;
          case CPU_FUNCT_MFLO:  BPSET_WRITES(rd); BPSET_READS((cpu_reg_t)CPU_REG_LO); break;
          case CPU_FUNCT_MTHI:  BPSET_WRITES((cpu_reg_t)CPU_REG_HI); BPSET_READS(rs); break;
          case CPU_FUNCT_MTLO:  BPSET_WRITES((cpu_reg_t)CPU_REG_LO); BPSET_READS(rs); break;
          case CPU_FUNCT_MULT: case CPU_FUNCT_MULTU:
          case CPU_FUNCT_DIV:  case CPU_FUNCT_DIVU:
            BPSET_WRITES((cpu_reg_t)CPU_REG_HI); BPSET_WRITES((cpu_reg_t)CPU_REG_LO);
            BPSET_READS(rs); BPSET_READS(rt); break;
          case CPU_FUNCT_SYSCALL: case CPU_FUNCT_BREAK: break;
          default: break;
        }
      } break;

      case CPU_OP_B: {
        if (((u8)iinst->i.rt & 0x1Eu) == 0x10u) BPSET_WRITES((cpu_reg_t)CPU_REG_RA);
        BPSET_READS(rs);
      } break;

      case CPU_OP_J: break;
      case CPU_OP_JAL: BPSET_WRITES((cpu_reg_t)CPU_REG_RA); break;
      case CPU_OP_BEQ: case CPU_OP_BNE: BPSET_READS(rs); BPSET_READS(rt); break;
      case CPU_OP_BLEZ: case CPU_OP_BGTZ: BPSET_READS(rs); break;

      case CPU_OP_ADDI: case CPU_OP_ADDIU: case CPU_OP_SLTI: case CPU_OP_SLTIU:
      case CPU_OP_ANDI: case CPU_OP_ORI:   case CPU_OP_XORI:
        BPSET_WRITES(rt); BPSET_READS(rs); break;

      case CPU_OP_LUI: BPSET_WRITES(rt); break;

      case CPU_OP_LB: case CPU_OP_LH: case CPU_OP_LW: case CPU_OP_LBU: case CPU_OP_LHU:
        BPSET_WRITES(rt); BPSET_READS(rs); break;

      case CPU_OP_LWL: case CPU_OP_LWR:
        BPSET_WRITES(rt); BPSET_READS(rs); BPSET_READS(rt); break;

      case CPU_OP_SB: case CPU_OP_SH: case CPU_OP_SWL: case CPU_OP_SW: case CPU_OP_SWR:
        BPSET_READS(rt); BPSET_READS(rs); break;

      case CPU_OP_COP0: case CPU_OP_COP2: {
        if (cpu_cop_is_common_instruction(*iinst)) {
          switch (cpu_cop_common_op(*iinst)) {
            case CPU_COP_COMMON_MFCN: case CPU_COP_COMMON_CFCN: BPSET_WRITES(rt); break;
            case CPU_COP_COMMON_MTCN: case CPU_COP_COMMON_CTCN: BPSET_READS(rt); break;
            default: break;
          }
        }
      } break;

      case CPU_OP_LWC2: case CPU_OP_SWC2: BPSET_READS(rs); BPSET_READS(rt); break;

      default: break;
    }

    inst--;
    iinst--;
  }
}

#undef BPSET_READS
#undef BPSET_WRITES

const void* cpu_code_cache_create_block_link(cpu_code_cache_block_t* from_block, void* code, u32 newpc)
{
  DebugAssert(newpc != from_block->pc);

  /* Safety net: if `newpc` is unaligned or in a non-executable segment,
   * the static link target is dead.  Don't register the link entry (a
   * patched jmp into a dead slot would skip the dispatcher's exception
   * synthesis added in compile_or_revalidate_block above).  Returning the
   * unconditional dispatcher here means the link is not patched and the
   * branch falls through to the next dispatcher round-trip, which will
   * reach compile_or_revalidate_block(newpc) and raise AdEL/IBE there. */
  if (g_settings.cpu_recompiler_block_linking && !cpu_recompiler_pc_is_executable(newpc))
    return g_dispatcher;

  const void* dst = g_dispatcher;
  if (g_settings.cpu_recompiler_block_linking) {
    const cpu_code_cache_block_t* nb = lookup_block(newpc);
    if (nb != NULL) {
      dst = (nb->state == CPU_CODE_CACHE_BS_VALID) ? nb->host_code :
            ((nb->state == CPU_CODE_CACHE_BS_FALLBACK_TO_INTERPRETER) ?
                g_interpret_block : g_compile_or_revalidate_block);
    } else {
      dst = has_block_lut(newpc) ? g_compile_or_revalidate_block : g_interpret_block;
    }

    const u32 idx = block_links_emplace(newpc, code);
    DebugAssert(from_block->num_exit_links < CPU_CODE_CACHE_MAX_BLOCK_EXIT_LINKS);
    from_block->exit_links[from_block->num_exit_links++] = &s_block_links[idx];
  }
  return dst;
}

const void* cpu_code_cache_create_self_block_link(cpu_code_cache_block_t* b, void* code, const void* block_start)
{
  const void* dst = g_dispatcher;
  if (g_settings.cpu_recompiler_block_linking) {
    dst = block_start;
    const u32 idx = block_links_emplace(b->pc, code);
    DebugAssert(b->num_exit_links < CPU_CODE_CACHE_MAX_BLOCK_EXIT_LINKS);
    b->exit_links[b->num_exit_links++] = &s_block_links[idx];
  }
  return dst;
}

static void (*s_emit_jump_fn)(void* site, const void* dst, bool flush_icache) = NULL;

void cpu_code_cache_set_emit_jump(void (*fn)(void* site, const void* dst, bool flush_icache))
{
  s_emit_jump_fn = fn;
}

static void backlink_blocks(u32 pc, const void* dst)
{
  if (!g_settings.cpu_recompiler_block_linking || s_emit_jump_fn == NULL)
    return;

  for (u32 i = 0; i < s_block_links_count; i++) {
    cpu_code_cache_block_link_entry_t* e = &s_block_links[i];
    if (!e->dead && e->pc == pc)
      s_emit_jump_fn(e->code, dst, true);
  }
}

static void unlink_block_exits(cpu_code_cache_block_t* b)
{
  for (u32 i = 0; i < b->num_exit_links; i++) {
    cpu_code_cache_block_link_entry_t* e = b->exit_links[i];
    if (e != NULL) {
      /* Compute index from pointer arithmetic. */
      const ptrdiff_t idx = e - s_block_links;
      if (idx >= 0 && (u32)idx < s_block_links_count)
        block_links_erase((u32)idx);
    }
  }
  b->num_exit_links = 0u;
}

static bool compile_block(cpu_code_cache_block_t* b)
{
  const void* host_code = NULL;
  u32 host_code_size = 0u;
  u32 host_far_code_size = 0u;

  if (g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_RECOMPILER &&
      g_cpu_compiler != NULL && g_cpu_compiler->v != NULL) {
    host_code = cpu_recompiler_compile_block(g_cpu_compiler, b, &host_code_size, &host_far_code_size);
  }

  b->host_code = host_code;
  b->host_code_size = host_code_size;
  if (host_code == NULL) {
    b->state = CPU_CODE_CACHE_BS_FALLBACK_TO_INTERPRETER;
    return false;
  }
  return true;
}

/* Two-level LUT lookup: g_code_lut[pc>>16][(pc&0xFFFC)>>2].
 * Returns the host_code address; uninitialized slots point at
 * g_compile_or_revalidate_block, so the caller can branch unconditionally. */
const void* cpu_code_cache_dispatch_lookup(u32 pc)
{
  const u32 outer = pc >> CPU_CODE_CACHE_LUT_TABLE_SHIFT;
  const u32 inner = (pc & 0xFFFFu) >> 2;
  const void** sub = g_code_lut[outer];
#ifdef CUPID_DEBUG_RECOMP_TRACE
  ERROR_LOG("dispatch_lookup pc=%08X outer=%u sub=%p", pc, outer, (void*)sub);
  if (sub == NULL)
    ERROR_LOG("  --> NULL inner table; host dispatcher will segfault");
#endif
  return sub[inner];
}

void cpu_code_cache_compile_or_revalidate_block(u32 start_pc)
{
#ifdef CUPID_DEBUG_RECOMP_TRACE
  ERROR_LOG("compile_or_revalidate pc=%08X has_block=%d", start_pc,
            (lookup_block(start_pc) != NULL));
#endif

  /* Dispatcher safety net: a wild branch / corrupt JR target can write
   * a misaligned or unmapped value into state.pc; the dispatcher then jumps
   * here with no validation.  Pinning g_interpret_block at the bad LUT slot
   * livelocks the emulator (the next dispatch round-trip just re-enters
   * uncached interp at the same garbage PC).  Real hardware delivers AdEL
   * (misaligned) or IBE (segment fault) on the fetch; raise that
   * exception now so the BIOS handler picks it up, and bail out without
   * touching the LUT or the block table. */
  if (cpu_recompiler_synth_pc_exception_if_invalid(start_pc))
    return;
  cpu_code_cache_block_t* b = lookup_block(start_pc);
  if (b != NULL) {
    /* If the block is somehow already VALID (rare race during chunk-0/1
     * landing where end_compile may have committed but the LUT pointer
     * not yet re-read by the dispatcher pipeline), just publish its host
     * code to the LUT and return.  Once the recompiler is fully wired
     * this branch is dead. */
    if (b->state == CPU_CODE_CACHE_BS_VALID) {
      if (b->host_code != NULL) {
        set_code_lut(start_pc, b->host_code);
        backlink_blocks(start_pc, b->host_code);
      }
      return;
    }
    if (revalidate_block(b)) {
      DebugAssert(b->host_code != NULL);
      set_code_lut(start_pc, b->host_code);
      backlink_blocks(start_pc, b->host_code);
      return;
    }
    unlink_block_exits(b);
  }

  cpu_code_cache_block_metadata_t metadata = {0};
  if (!read_block_instructions(start_pc, &metadata)) {
    ERROR_LOG("Failed to read block at 0x%08X, falling back to uncached interpreter", start_pc);
    set_code_lut(start_pc, g_interpret_block);
    backlink_blocks(start_pc, g_interpret_block);
    return;
  }

  /* Reserve enough code-buffer space; if not, reset cache. */
  const u32 block_size = s_block_instructions_count;
  const u32 free_code = cpu_code_cache_get_free_code_space();
  const u32 free_far  = cpu_code_cache_get_free_far_code_space();
  if (free_code < (block_size * CPU_RECOMP_MAX_NEAR_HOST_BYTES_PER_INST) ||
      free_code < CPU_RECOMP_MIN_CODE_RESERVE_FOR_BLOCK ||
      free_far  < CPU_RECOMP_MIN_CODE_RESERVE_FOR_BLOCK) {
    ERROR_LOG("Out of code space while compiling %08X. Resetting code cache.", start_pc);
    cpu_code_cache_reset();
  }

  b = create_block(start_pc, s_block_instructions, s_block_instructions_count, &metadata);
  if (b == NULL || b->size == 0u || !compile_block(b)) {
    ERROR_LOG("Failed to compile block at 0x%08X, falling back to uncached interpreter", start_pc);
    set_code_lut(start_pc, g_interpret_block);
    backlink_blocks(start_pc, g_interpret_block);
    return;
  }

  set_code_lut(start_pc, b->host_code);
  backlink_blocks(start_pc, b->host_code);
#ifdef CUPID_DEBUG_RECOMP_TRACE
  ERROR_LOG("compiled pc=%08X host=%p size=%u", start_pc,
            (void*)b->host_code, b->host_code_size);
  /* For the BIOS scratchpad-clear loop block, dump full host bytes so we can
   * disassemble offline and find the bne mis-emit. */
  if (start_pc == 0xBFC00250u && b->host_code != NULL) {
    const u8* p = (const u8*)b->host_code;
    const u32 n = b->host_code_size;
    char buf[2048];
    u32 off = 0;
    for (u32 i = 0; i < n && off + 4 < sizeof(buf); i++)
      off += (u32)snprintf(buf + off, sizeof(buf) - off, "%02x ", p[i]);
    ERROR_LOG("HOSTBYTES BFC00250 (%u bytes): %s", n, buf);
  }
#endif
}

void cpu_code_cache_discard_and_recompile_block(u32 start_pc)
{
  cpu_code_cache_block_t* b = lookup_block(start_pc);
  DebugAssert(b != NULL && b->state == CPU_CODE_CACHE_BS_VALID);
  invalidate_block(b, CPU_CODE_CACHE_BS_NEEDS_RECOMPILE);
  cpu_code_cache_compile_or_revalidate_block(start_pc);
}

static u32 (*s_emit_asm_functions_fn)(void* code, u32 code_size) = NULL;

void cpu_code_cache_set_emit_asm_functions(u32 (*fn)(void* code, u32 code_size))
{
  s_emit_asm_functions_fn = fn;
}

static void compile_asm_functions(void)
{
  /* Backend supplies `emit_asm_functions` once the x64 backend registers
   * itself.  When NULL, dispatcher globals stay NULL and
   * cpu_code_cache_execute() falls through to the interpreter. */
  if (s_emit_asm_functions_fn == NULL)
    return;
  if (g_cpu_compiler == NULL || g_cpu_compiler->v == NULL)
    return;

  u8* const code = cpu_code_cache_get_free_code_pointer();
  const u32 cap = cpu_code_cache_get_free_code_space();
  const u32 size = s_emit_asm_functions_fn(code, cap);
  Assert(size > 0u && size <= cap);
  cpu_code_cache_commit_code(size);
#ifdef CUPID_DEBUG_RECOMP_TRACE
  ERROR_LOG("ASM funcs emitted: enter=%p dispatcher=%p compile=%p interp_block=%p run_events=%p (%u bytes)",
            (void*)g_enter_recompiler, (void*)g_dispatcher,
            (void*)g_compile_or_revalidate_block, (void*)g_interpret_block, 
            (void*)g_run_events_and_dispatch, size);
#endif
}

NORETURN void cpu_run_interpreter_loop(void);

bool cpu_code_cache_is_using_recompiler(void)
{
  /* Returns true if the user has selected recompiler mode AND a backend is
   * registered.  Note: g_enter_recompiler may still be NULL if the prelude
   * has not yet been emitted (first reset).  The execute() callsite below
   * guards against that explicitly so removing the guard from here lets
   * compile_asm_functions actually run on first reset (which is what
   * populates g_enter_recompiler in the first place). */
  return (g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_RECOMPILER) &&
         (g_cpu_compiler != NULL) && (g_cpu_compiler->v != NULL);
}

bool cpu_code_cache_is_using_fastmem(void)
{
  /* M4c: MMap mode is the only path the recompiler can emit fastmem load/
   * store sites against today.  LUT mode requires per-page pointer
   * lookups that don't fit the inline `[RBX+addr]` SIB shape, so it stays
   * slow-path only.  Fastmem only on if mode==MMAP and arena allocated. */
  return g_settings.cpu_fastmem_mode == CPU_FASTMEM_MODE_MMAP &&
         bus_get_fastmem_base(/*isc=*/false) != NULL;
}

bool cpu_code_cache_process_startup(Error* error)
{
  s_code_buffer_ptr = (u8*)memmap_allocate_jit_memory(RECOMPILER_CODE_CACHE_SIZE);
  if (s_code_buffer_ptr == NULL) {
    ERROR_LOG("Failed to allocate %u bytes of JIT memory", RECOMPILER_CODE_CACHE_SIZE);
    return false;
  }
  allocate_luts();
  reset_code_buffer();

  if (!page_fault_handler_install(error)) {
    WARNING_LOG("Failed to install page fault handler; fastmem disabled");
    Error_clear(error);
  } else {
    page_fault_handler_set_callback(
      (page_fault_handler_cb_t)&cpu_code_cache_handle_fastmem_exception);
  }

  /* Register the host-arch backend now that the JIT region exists.  The
   * backend sets g_cpu_compiler + emit_jump + emit_asm_functions callbacks;
   * the dispatcher prelude itself is emitted lazily on the next
   * cpu_code_cache_reset (which runs from system_reset). */
#if defined(__x86_64__) || defined(_M_X64)
  x64_backend_register();
#elif defined(__aarch64__) || defined(_M_ARM64)
  arm64_backend_register();
#else
#endif

  INFO_LOG("code cache: process_startup ok (%u MB JIT, %u MB far)",
           RECOMPILER_CODE_CACHE_SIZE / (1024u * 1024u),
           RECOMPILER_FAR_CODE_CACHE_SIZE / (1024u * 1024u));
  return true;
}

void cpu_code_cache_process_shutdown(void)
{
#if defined(__x86_64__) || defined(_M_X64)
  x64_backend_shutdown();
#elif defined(__aarch64__) || defined(_M_ARM64)
  arm64_backend_shutdown();
#endif
  deallocate_luts();
  /* JIT memory release is left to OS at process exit (memmap layer doesn't
   * expose a specific release for the JIT region; safe on Linux). */
  s_code_buffer_ptr = NULL;
  /* Free dynamic backpatch buffer.  Cache resets only zero the count
   * (buffer reused for next session); only process shutdown frees it. */
  free(s_fastmem_backpatch_info);
  s_fastmem_backpatch_info     = NULL;
  s_fastmem_backpatch_info_cap = 0u;
}

void cpu_code_cache_reset(void)
{
  clear_blocks();
  if (cpu_code_cache_is_using_recompiler()) {
    reset_code_buffer();
    compile_asm_functions();
    reset_code_lut();
  }
}

void cpu_code_cache_shutdown(void)
{
  clear_blocks();
  free(s_blocks); s_blocks = NULL; s_blocks_cap = 0u; s_blocks_count = 0u;
  free(s_block_links); s_block_links = NULL; s_block_links_cap = 0u; s_block_links_count = 0u;
  free(s_block_instructions); s_block_instructions = NULL;
  s_block_instructions_cap = 0u; s_block_instructions_count = 0u;
}

NORETURN void cpu_code_cache_execute(void)
{
#ifdef CUPID_DEBUG_RECOMP_TRACE
  ERROR_LOG("execute: using_recomp=%d g_enter=%p",
            cpu_code_cache_is_using_recompiler(), (void*)g_enter_recompiler);
#endif
  /* Backend gate: until 6.D.2 wires g_enter_recompiler we fall through to the
   * interpreter loop.  cpu_run_interpreter_loop() is [[noreturn]]. */
  if (cpu_code_cache_is_using_recompiler() && g_enter_recompiler != NULL) {
    g_enter_recompiler();
    /* unreachable */
  }
  cpu_run_interpreter_loop();
}

enum {
  FASTMEM_BACKPATCH_INFO_INITIAL_CAP = 4096u, /* grows on demand via realloc */
  FASTMEM_FAULTING_PCS_CAP           = 4096u, /* power of two for mask wrap */
};

/* Dynamic-grow array (forward-declared near top of file so
 * process_shutdown can free it).  Was a fixed 16384-entry table; MGS Disc 1
 * emits more than that across its first ~60s (saturated at 16384 in repro),
 * and past saturation `cpu_code_cache_add_load_store_info` silently dropped
 * new entries.  The first fault at an unregistered site then aborted via
 * the crash handler.  Now grows by doubling; cleared on cache reset
 * (count zeroed, buffer reused). */

/* Open-addressing hash set, key 0 = empty slot.  PC=0 is never a valid
 * guest PC (BIOS starts at 0xBFC00000), so the sentinel is safe. */
static u32 s_fastmem_faulting_pcs[FASTMEM_FAULTING_PCS_CAP];

/* FNV-1a 32-bit hash. */
static u32 fastmem_pc_hash(u32 pc)
{
  u32 h = 2166136261u;
  h = (h ^ ((pc >>  0) & 0xFFu)) * 16777619u;
  h = (h ^ ((pc >>  8) & 0xFFu)) * 16777619u;
  h = (h ^ ((pc >> 16) & 0xFFu)) * 16777619u;
  h = (h ^ ((pc >> 24) & 0xFFu)) * 16777619u;
  return h;
}

bool cpu_code_cache_has_previously_faulted_on_pc(u32 guest_pc)
{
  if (guest_pc == 0u) return false;
  const u32 mask = FASTMEM_FAULTING_PCS_CAP - 1u;
  u32 slot = fastmem_pc_hash(guest_pc) & mask;
  for (u32 probes = 0u; probes < FASTMEM_FAULTING_PCS_CAP; probes++) {
    const u32 v = s_fastmem_faulting_pcs[slot];
    if (v == 0u)        return false;
    if (v == guest_pc)  return true;
    slot = (slot + 1u) & mask;
  }
  return false;
}

static void fastmem_record_faulting_pc(u32 guest_pc)
{
  if (guest_pc == 0u) return;
  const u32 mask = FASTMEM_FAULTING_PCS_CAP - 1u;
  u32 slot = fastmem_pc_hash(guest_pc) & mask;
  for (u32 probes = 0u; probes < FASTMEM_FAULTING_PCS_CAP; probes++) {
    const u32 v = s_fastmem_faulting_pcs[slot];
    if (v == 0u)        { s_fastmem_faulting_pcs[slot] = guest_pc; return; }
    if (v == guest_pc)  return;
    slot = (slot + 1u) & mask;
  }
}

/* Comparator for qsort: ascending by code_address. */
static int backpatch_cmp(const void* lhs, const void* rhs)
{
  const cpu_code_cache_loadstore_backpatch_info_t* a =
    (const cpu_code_cache_loadstore_backpatch_info_t*)lhs;
  const cpu_code_cache_loadstore_backpatch_info_t* b =
    (const cpu_code_cache_loadstore_backpatch_info_t*)rhs;
  if (a->code_address < b->code_address) return -1;
  if (a->code_address > b->code_address) return  1;
  return 0;
}

void cpu_code_cache_add_load_store_info(void* code_address, u32 code_size,
                                         u32 guest_pc, u32 guest_block,
                                        tick_count_t cycles, u32 gpr_bitmask,
                                        u8 address_register, u8 data_register,
                                        memory_access_size_t size,
                                        bool is_signed, bool is_load)
{
  if (s_fastmem_backpatch_info_count >= s_fastmem_backpatch_info_cap) {
    const u32 new_cap = (s_fastmem_backpatch_info_cap == 0u) ?
                          FASTMEM_BACKPATCH_INFO_INITIAL_CAP :
                          s_fastmem_backpatch_info_cap * 2u;
    cpu_code_cache_loadstore_backpatch_info_t* nb = (cpu_code_cache_loadstore_backpatch_info_t*)
      realloc(s_fastmem_backpatch_info,
              (size_t)new_cap * sizeof(s_fastmem_backpatch_info[0]));
    if (nb == NULL) {
      WARNING_LOG("Fastmem backpatch table realloc(%u) failed; fastmem path will skip backpatch",
                  new_cap);
      return;
    }
    s_fastmem_backpatch_info     = nb;
    s_fastmem_backpatch_info_cap = new_cap;
  }
  cpu_code_cache_loadstore_backpatch_info_t* slot =
    &s_fastmem_backpatch_info[s_fastmem_backpatch_info_count++];
  slot->code_address     = code_address;
  slot->code_size        = code_size;
  slot->guest_pc         = guest_pc;
  slot->guest_block      = guest_block;
  slot->cycles           = cycles;
  slot->gpr_bitmask      = gpr_bitmask;
  slot->size             = size;
  slot->address_register = address_register;
  slot->data_register    = data_register;
  slot->is_signed        = is_signed ? 1u : 0u;
  slot->is_load          = is_load   ? 1u : 0u;

  /* Keep the array sorted by code_address.  The recompiler emits sites in
   * monotonically increasing order within a single block, but blocks land
   * at different JIT addresses across resets, so we re-sort lazily via
   * insertion: only the new tail needs to bubble down.  qsort is overkill
   * but called rarely and simpler than a hand-rolled insertion. */
  if (s_fastmem_backpatch_info_count > 1u &&
      slot[-1].code_address > slot->code_address) {
    qsort(s_fastmem_backpatch_info, s_fastmem_backpatch_info_count,
          sizeof(s_fastmem_backpatch_info[0]), backpatch_cmp);
  }
}

const cpu_code_cache_loadstore_backpatch_info_t*
cpu_code_cache_find_backpatch_info(const void* host_pc)
{
  /* Binary search for the largest entry whose code_address <= host_pc;
   * then check that the entry's range contains host_pc. */
  const u8* target = (const u8*)host_pc;
  u32 lo = 0u, hi = s_fastmem_backpatch_info_count;
  while (lo < hi) {
    const u32 mid = lo + (hi - lo) / 2u;
    if ((const u8*)s_fastmem_backpatch_info[mid].code_address <= target)
      lo = mid + 1u;
    else
      hi = mid;
  }
  if (lo == 0u) return NULL;
  const cpu_code_cache_loadstore_backpatch_info_t* e = &s_fastmem_backpatch_info[lo - 1u];
  const u8* base = (const u8*)e->code_address;
  if (target >= base && target < base + e->code_size)
    return e;
  return NULL;
}

void cpu_code_cache_remove_backpatch_info_for_range(const void* start, u32 size)
{
  const u8* lo = (const u8*)start;
  const u8* hi = lo + size;
  u32 dst = 0u;
  for (u32 src = 0u; src < s_fastmem_backpatch_info_count; src++) {
    const u8* a = (const u8*)s_fastmem_backpatch_info[src].code_address;
    if (a >= lo && a < hi) continue;          /* drop */
    if (dst != src)
      s_fastmem_backpatch_info[dst] = s_fastmem_backpatch_info[src];
    dst++;
  }
  s_fastmem_backpatch_info_count = dst;
}

static void fastmem_backpatch_clear_all(void)
{
  s_fastmem_backpatch_info_count = 0u;
  memset(s_fastmem_faulting_pcs, 0, sizeof(s_fastmem_faulting_pcs));
}

/* x64 register IDs of the SysV caller-saved set we must spill across the
 * C thunk call (excluding RAX which holds the thunk's return value). */
static const x64_reg_t s_caller_saved_for_stub[] = {
  X64_RDI, X64_RSI, X64_RDX, X64_RCX, X64_R8, X64_R9, X64_R10, X64_R11,
};
enum { CALLER_SAVED_FOR_STUB_COUNT = sizeof(s_caller_saved_for_stub) /
                                     sizeof(s_caller_saved_for_stub[0]) };

static const void* fastmem_thunk_for(memory_access_size_t size, bool is_load)
{
  if (is_load) {
    switch (size) {
      case MEMORY_ACCESS_SIZE_BYTE:     return (const void*)&cpu_recompiler_thunk_read_memory_byte;
      case MEMORY_ACCESS_SIZE_HALFWORD: return (const void*)&cpu_recompiler_thunk_read_memory_halfword;
      case MEMORY_ACCESS_SIZE_WORD:     return (const void*)&cpu_recompiler_thunk_read_memory_word;
    }
  } else {
    switch (size) {
      case MEMORY_ACCESS_SIZE_BYTE:     return (const void*)&cpu_recompiler_thunk_write_memory_byte;
      case MEMORY_ACCESS_SIZE_HALFWORD: return (const void*)&cpu_recompiler_thunk_write_memory_halfword;
      case MEMORY_ACCESS_SIZE_WORD:     return (const void*)&cpu_recompiler_thunk_write_memory_word;
    }
  }
  return NULL;
}

/* Emit one slow-path stub in far code.  Returns the host address of the
 * stub on success, NULL if anything failed (out of far code, etc.). */
static void* emit_fastmem_slow_path_stub(const cpu_code_cache_loadstore_backpatch_info_t* info)
{
  /* Worst-case stub size: 8 push (1B each) + 2 push for arg load (1-2B
   * each) + 2 pop (1-2B each) + 10B abs64-call + 10B sign/zero-extend +
   * 8 pop + 5B jmp rel32.  ~80B is plenty; 256 leaves headroom. */
  enum { STUB_RESERVE = 256u };
  if (cpu_code_cache_get_free_far_code_space() < STUB_RESERVE)
    return NULL;

  u8* base = cpu_code_cache_get_free_far_code_pointer();
  x64_emit_t e;
  x64_emit_init(&e, base, STUB_RESERVE);

  /* Tick parity: the recompiler pre-charged BUS_RAM_READ_TICKS into
   * r->cycles at compile time for every fastmem load.  When the load faults
   * to this stub, the C thunk below will call cpu_read_memory_* -> bus
   * handler -> BUS_CYCLES(BUS_RAM_READ_TICKS), adding the same amount a
   * second time.  Subtract it back out here so the per-load cost is
   * BUS_RAM_READ_TICKS net regardless of fastmem vs slow-path resolution.
   * Only loads pre-charge (RAM writes do not stall in bus.c; ram_write_*
   * call no BUS_CYCLES), so stores skip this adjustment. */
  if (info->is_load)
    x64_backend_emit_sub_pending_ticks(&e, (s32)BUS_RAM_READ_TICKS);

  /* 1. Spill caller-saved (excluding RAX). */
  for (u32 i = 0u; i < CALLER_SAVED_FOR_STUB_COUNT; i++)
    x64_push_r(&e, s_caller_saved_for_stub[i]);

  /* 2. Marshal arguments via push/pop so source/dest aliasing is safe.
   *    is_load: only RWARG1 = addr.
   *    !is_load: RWARG1 = addr, RWARG2 = data. */
  const x64_reg_t addr_reg = (x64_reg_t)info->address_register;
  const x64_reg_t data_reg = (x64_reg_t)info->data_register;
  if (info->is_load) {
    x64_push_r(&e, addr_reg);
    x64_pop_r (&e, X64_RDI);
  } else {
    x64_push_r(&e, addr_reg);
    x64_push_r(&e, data_reg);
    x64_pop_r (&e, X64_RSI);
    x64_pop_r (&e, X64_RDI);
  }

  /* 3. Call the appropriate C thunk via abs64. */
  const void* thunk = fastmem_thunk_for(info->size, info->is_load != 0u);
  if (thunk == NULL || e.overflow) return NULL;
  x64_call_abs64(&e, thunk);

  /* 4. For loads, sign/zero-extend the result IN-PLACE in RAX (which is
   *    NOT in the spilled set, so it survives the pop sequence), then
   *    restore caller-saved, then move RAX → data_reg.  Handles every
   *    aliasing of data_reg (including data_reg == one of the spilled
   *    callee-saved set) without needing a stack-temp dance. */
  if (info->is_load) {
    switch (info->size) {
      case MEMORY_ACCESS_SIZE_BYTE:
        if (info->is_signed) x64_movsx_r_r8 (&e, X64_SZ_32, X64_RAX, X64_RAX);
        else                 x64_movzx_r_r8 (&e, X64_SZ_32, X64_RAX, X64_RAX);
        break;
      case MEMORY_ACCESS_SIZE_HALFWORD:
        if (info->is_signed) x64_movsx_r_r16(&e, X64_SZ_32, X64_RAX, X64_RAX);
        else                 x64_movzx_r_r16(&e, X64_SZ_32, X64_RAX, X64_RAX);
        break;
      case MEMORY_ACCESS_SIZE_WORD:
        /* No-op: thunk return is already a u32 in RAX. */
        break;
    }
    for (s32 i = (s32)CALLER_SAVED_FOR_STUB_COUNT - 1; i >= 0; i--)
      x64_pop_r(&e, s_caller_saved_for_stub[(u32)i]);
    if (data_reg != X64_RAX)
      x64_mov_r_r(&e, X64_SZ_32, data_reg, X64_RAX);
  } else {
    for (s32 i = (s32)CALLER_SAVED_FOR_STUB_COUNT - 1; i >= 0; i--)
      x64_pop_r(&e, s_caller_saved_for_stub[(u32)i]);
  }

  /* 5. Jump back to one-past the faulting instruction (resumes the
   *    block's normal control flow). */
  u8* jmp_site = x64_jmp_rel32(&e);
  void* resume = (u8*)info->code_address + info->code_size;
  x64_patch_rel32(jmp_site, resume);

  if (e.overflow) return NULL;

  const u32 emitted = (u32)x64_emit_size(&e);
  cpu_code_cache_commit_far_code(emitted);
  return base;
}

/* Patch the original [RBX+addr] site as `JMP rel32` to the stub, NOP-pad
 * any extra bytes.  Site MUST be at least 5 bytes (the JMP rel32 size). */
static bool patch_fastmem_site_to_stub(
  const cpu_code_cache_loadstore_backpatch_info_t* info,
  const void* stub)
{
  if (info->code_size < 5u)
    return false;

  x64_emit_t patch;
  x64_emit_init(&patch, info->code_address, info->code_size);
  u8* jmp_site = x64_jmp_rel32(&patch);
  x64_patch_rel32(jmp_site, (void*)stub);
  const u32 emitted = (u32)x64_emit_size(&patch);
  if (emitted < info->code_size)
    x64_nop_n(&patch, info->code_size - emitted);
  return !patch.overflow;
}

int cpu_code_cache_handle_fastmem_exception(void* exception_pc, void* fault_address, bool is_write)
{
  /* SMC branch. */
  if (cpu_code_cache_is_using_fastmem()) {
    u8* arena = (u8*)bus_get_fastmem_base(/*isc=*/false);
    u8* fa    = (u8*)fault_address;
    if (arena != NULL && fa >= arena &&
        (uintptr_t)(fa - arena) < (uintptr_t)BUS_FASTMEM_ARENA_SIZE) {
      const u32 guest_address = (u32)(uintptr_t)(fa - arena);
      /* KSEG2 starts at 0xC0000000; arena only mirrors KUSEG/KSEG0/KSEG1
       * so this check is redundant in cupid-ps1's layout but kept for parity
       */
      if (!g_cpu_state.cop0_regs.sr.Isc &&
          /* guest_address < 0xC0000000u && */
          guest_address < bus_get_ram_size()) {
        (void)is_write;
        cpu_code_cache_invalidate_blocks_with_page_index(
          bus_get_ram_code_page_index(guest_address));
        return 0;  /* CONTINUE_EXECUTION */
      }
    }
  }

  /* MMIO-via-fastmem branch. */
  const cpu_code_cache_loadstore_backpatch_info_t* info =
    cpu_code_cache_find_backpatch_info(exception_pc);
  if (info == NULL) {
    /* Diag: a SIGSEGV in the JIT region with no backpatch entry will
     * fall through to the crash handler -> abort.  Dump enough context to
     * identify the missing emitter form (RIP byte prefix is usually enough
     * to spot which mov/movsx/movzx variant is unhandled). */
    const u8* rip_bytes = (const u8*)exception_pc;
    fprintf(stderr,
            "[fastmem-diag] no backpatch entry: rip=%p fault=%p is_write=%d "
            "guest_pc=%08X Isc=%d cop0_sr=%08X bytes=",
            exception_pc, fault_address, is_write,
            g_cpu_state.pc, (int)g_cpu_state.cop0_regs.sr.Isc,
            g_cpu_state.cop0_regs.sr.bits);
    for (u32 i = 0; i < 16u; i++)
      fprintf(stderr, "%02x ", rip_bytes[i]);
    fprintf(stderr, "\n[fastmem-diag] backpatch_count=%u arena=%p arena_size=%u\n",
            s_fastmem_backpatch_info_count,
            bus_get_fastmem_base(/*isc=*/false),
            (u32)BUS_FASTMEM_ARENA_SIZE);
    /* Print 3 nearest entries by code_address. */
    if (s_fastmem_backpatch_info_count > 0u) {
      u32 best = 0u;
      ptrdiff_t best_d = (ptrdiff_t)0x7fffffffffffffffLL;
      for (u32 i = 0u; i < s_fastmem_backpatch_info_count; i++) {
        ptrdiff_t d = (const u8*)s_fastmem_backpatch_info[i].code_address - (const u8*)exception_pc;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
      }
      const u32 lo = best > 1u ? best - 1u : 0u;
      const u32 hi = best + 2u < s_fastmem_backpatch_info_count ? best + 2u : s_fastmem_backpatch_info_count;
      for (u32 i = lo; i < hi; i++) {
        const cpu_code_cache_loadstore_backpatch_info_t* e = &s_fastmem_backpatch_info[i];
        fprintf(stderr,
                "[fastmem-diag]   entry[%u]: code=%p sz=%u guest_pc=%08X is_load=%u sz=%u addr_reg=%u data_reg=%u\n",
                i, e->code_address, e->code_size, e->guest_pc,
                e->is_load, e->size, e->address_register, e->data_register);
      }
    }
    fflush(stderr);
    return 1;
  }

  /* Mark the guest PC as a known fastmem-fault site so subsequent compiles
   * of the same MIPS instruction skip fastmem and emit straight slow path. */
  fastmem_record_faulting_pc(info->guest_pc);

  void* stub = emit_fastmem_slow_path_stub(info);
  if (stub == NULL)
    return 1;
  if (!patch_fastmem_site_to_stub(info, stub))
    return 1;

  /* Evict the owning JIT block so a clean recompile picks up the per-PC
   * faulting flag we just set, rather than re-emitting the same broken
   * lookup_block may return NULL after races; guard. */
  cpu_code_cache_block_t* block = lookup_block(info->guest_block);
  if (block != NULL) {
    remove_block_from_page_list(block);
    invalidate_block(block, CPU_CODE_CACHE_BS_NEEDS_RECOMPILE);
    block->compile_frame = system_get_frame_number();
    block->compile_count = 1u;
  }

  /* Drop the now-handled entry so the binary-search table stays tight
   * (s_fastmem_backpatch_info.erase(iter)). */
  cpu_code_cache_remove_backpatch_info_for_range(info->code_address,
                                                 info->code_size);

  return 0;
}

void cpu_code_cache_dbg_dump_recent_blocks(FILE* out)
{
  if (!out) out = stderr;
  fprintf(out, "===== last %u recompiled blocks (oldest first) =====\n",
          s_dbg_block_ring_count);
  const u32 n = s_dbg_block_ring_count;
  if (n == 0u) {
    fprintf(out, "  (no blocks recorded)\n");
    return;
  }
  /* Oldest entry is one slot past the next-write position when the ring
   * is full; otherwise it's index 0. */
  const u32 start = (n < CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE) ? 0u :
                    (s_dbg_block_ring_pos % CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE);
  for (u32 i = 0; i < n; i++) {
    const cpu_code_cache_dbg_block_entry_t* e =
      &s_dbg_block_ring[(start + i) % CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE];
    fprintf(out, "  [%2u] pc=%08X size=%u host=%p\n", i, e->pc, e->size, e->host_code);
  }
}

/* Safety-net AdEL/IBE diagnostic.  First N triggers print register state
 * + last 4 ring entries to stderr; subsequent triggers stay silent so the
 * BIOS exception loop doesn't drown the log. */
void cpu_code_cache_dbg_log_safety_net_exception(u32 bad_pc)
{
  enum {
    CPU_CODE_CACHE_SAFETY_NET_LOG_LIMIT      = 4u,
    CPU_CODE_CACHE_SAFETY_NET_BLOCK_DUMP_CAP = 64u,
  };
  static u32 s_safety_net_count = 0u;
  if (s_safety_net_count >= CPU_CODE_CACHE_SAFETY_NET_LOG_LIMIT) {
    s_safety_net_count++;  /* keep counting for the final summary */
    return;
  }
  s_safety_net_count++;
  fprintf(stderr,
          "[recomp-safety-net] bad PC=0x%08X (event #%u): "
          "g_cpu_state.pc=%08X npc=%08X "
          "cop0.sr=%08X cop0.cause=%08X cop0.epc=%08X bad_vaddr=%08X\n",
          bad_pc, s_safety_net_count,
          g_cpu_state.pc, g_cpu_state.npc,
          g_cpu_state.cop0_regs.sr.bits, g_cpu_state.cop0_regs.cause.bits,
          g_cpu_state.cop0_regs.EPC, g_cpu_state.cop0_regs.BadVaddr);
  /* All 32 GPRs (4 per row, 8 rows). */
  static const char* const s_gpr_names[32] = {
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra",
  };
  for (u32 row = 0u; row < 8u; row++) {
    fprintf(stderr, "  ");
    for (u32 col = 0u; col < 4u; col++) {
      const u32 i = row * 4u + col;
      fprintf(stderr, "%-3s=%08X ", s_gpr_names[i], g_cpu_state.regs.r[i]);
    }
    fputc('\n', stderr);
  }

  /* Scan low RAM (kernel scratch area + first 256 bytes) for any word
   * matching the bad PC value; finds the slot the rogue load read it
   * from.  Plus 256 bytes around current sp.  Enough to spot the
   * smoking-gun cell. */
  fprintf(stderr, "  scan low RAM[0..0x100] for word=0x%08X:\n", bad_pc);
  for (u32 off = 0; off < 0x100u; off += 4u) {
    u32 v = 0;
    if (cpu_safe_read_memory_word(0x80000000u | off, &v) && v == bad_pc) {
      fprintf(stderr, "    paddr=%02X has the bad value\n", off);
    }
  }
  fprintf(stderr, "  scan kernel stack [0x801FFC00..0x80200000] for word=0x%08X:\n", bad_pc);
  for (u32 a = 0x801FFC00u; a < 0x80200000u; a += 4u) {
    u32 v = 0;
    if (cpu_safe_read_memory_word(a, &v) && v == bad_pc) {
      fprintf(stderr, "    addr=%08X has the bad value\n", a);
    }
  }
  /* Also dump 64 bytes around (sp-64..sp+64) and (sp_at_lw_estimate..) so
   * the slot-by-slot comparison is right there. */
  const u32 sp = g_cpu_state.regs.r[29];
  fprintf(stderr, "  64 bytes around sp=%08X:\n", sp);
  for (s32 off = -64; off < 64; off += 4) {
    const u32 a = sp + (u32)off;
    u32 v = 0;
    if (cpu_safe_read_memory_word(a, &v))
      fprintf(stderr, "    %08X (%+4d): %08X\n", a, off, v);
  }
  /* Per-block EXECUTION ring (if CUPID_TRACE_EXEC=1).  Shows the actual
   * dispatch order, not the compile order - this is what tells us which
   * block wrote $ra=9. */
  if (s_exec_trace_active && s_dbg_exec_ring_count > 0u) {
    const u32 cap = CPU_CODE_CACHE_DBG_EXEC_RING_SIZE;
    const u32 nx = s_dbg_exec_ring_count;
    const u32 want = nx < 8u ? nx : 8u;
    fprintf(stderr, "  last %u EXECUTED blocks (newest first; pc / ra at entry / sp) WITH DISASM:\n", want);
    for (u32 k = 0u; k < want; k++) {
      const u32 idx = (s_dbg_exec_ring_pos + cap - 1u - k) % cap;
      const cpu_code_cache_dbg_exec_entry_t* e = &s_dbg_exec_ring[idx];
      fprintf(stderr, "    -%-2u  pc=%08X  ra=%08X  sp=%08X\n",
              k, e->pc, e->ra, e->sp);
      /* Up to 16 instructions of the block. */
      for (u32 i = 0u; i < 16u; i++) {
        const u32 ipc = e->pc + i * 4u;
        u32 raw = 0u;
        char buf[96];
        small_string_t s2;
        small_string_init_stack(&s2, buf, sizeof(buf));
        if (cpu_safe_read_memory_word(ipc, &raw)) {
          cpu_disassemble(&s2, raw, ipc);
          fprintf(stderr, "         %08X: %08X  %s\n",
                  ipc, raw, small_string_c_str(&s2));
        } else {
          fprintf(stderr, "         %08X: <unreadable>\n", ipc);
          break;
        }
        /* Stop at JR / JALR / J / JAL / branch-likely; common terminators */
        const u32 op = raw >> 26;
        const u32 funct = raw & 0x3Fu;
        if (op == 0u && (funct == 8u || funct == 9u)) break;  /* JR / JALR */
        if (op == 2u || op == 3u) break;                      /* J / JAL */
      }
    }
  }
  /* Last N ring entries (most recent compiles before the wild jump). */
  const u32 n = s_dbg_block_ring_count;
  if (n == 0u) { fflush(stderr); return; }
  const u32 want = n < CPU_CODE_CACHE_SAFETY_NET_BLOCK_DUMP_CAP ?
                     n : CPU_CODE_CACHE_SAFETY_NET_BLOCK_DUMP_CAP;
  fprintf(stderr, "  last %u compiled blocks before this:\n", want);
  /* Walk from newest to oldest, with a per-block disassembly so the next
   * debugging session can spot the buggy emit pattern (e.g. lw $reg; JR
   * $reg load-delay misordering) without re-running. */
  for (u32 k = 0u; k < want; k++) {
    const u32 idx = (s_dbg_block_ring_pos + CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE - 1u - k) %
                    CPU_CODE_CACHE_DBG_BLOCK_RING_SIZE;
    const cpu_code_cache_dbg_block_entry_t* e = &s_dbg_block_ring[idx];
    fprintf(stderr, "    -%u  pc=%08X size=%u  disasm:\n", k, e->pc, e->size);
    /* Cap at 32 instructions per dump entry so a runaway block doesn't
     * blow up the log. */
    const u32 inst_cap = (e->size < 32u) ? e->size : 32u;
    for (u32 i = 0u; i < inst_cap; i++) {
      const u32 ipc = e->pc + i * 4u;
      u32 raw = 0u;
      char buf[96];
      small_string_t s;
      small_string_init_stack(&s, buf, sizeof(buf));
      if (cpu_safe_read_memory_word(ipc, &raw)) {
        cpu_disassemble(&s, raw, ipc);
        fprintf(stderr, "         %08X: %08X  %s\n",
                ipc, raw, small_string_c_str(&s));
      } else {
        fprintf(stderr, "         %08X: <unreadable>\n", ipc);
      }
    }
  }
  fflush(stderr);
}
