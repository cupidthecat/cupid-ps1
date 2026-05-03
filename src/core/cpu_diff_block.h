/*
 * Per-block recompiler/interpreter divergence detector.
 *
 * On every compiled block, the JIT emits a call to cpu_diff_block_enter at
 * the prologue (allocator empty) and cpu_diff_block_exit at the epilogue
 * (after CPU_RC_FLUSH_END_BLOCK; allocator state already flushed).  The
 * exit handler:
 *   1. Snapshots post-recomp cpu_state_t.
 *   2. Restores cpu_state_t from the enter snapshot.
 *   3. Steps the interpreter `block_size` instructions on that scratch state.
 *   4. Byte-compares the interpreter's final state against the recomp's
 *      final state; on mismatch dumps fields + RAM-write log and abort()s.
 *   5. Restores cpu_state_t to the post-recomp snapshot so real execution
 *      continues unaffected.
 *
 * Replaces the (now-removed) per-instruction emit_diff_trace_record path,
 * which perturbed the host_regs[] allocator + EFLAGS at PC BFC00270 BNE.
 *
 * Gating: env var CUPID_RECOMP_DIFF=1 OR g_settings.cpu_recompiler_compare_register_files.
 * When off the JIT-emitted call sites are still emitted but each call returns
 * immediately (one mem-load + branch).  Set CUPID_BUILD_RECOMP_DIFF if a
 * compile-time gate is needed; not enabled today.
 */

#ifndef CUPID_CORE_CPU_DIFF_BLOCK_H
#define CUPID_CORE_CPU_DIFF_BLOCK_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True once env var / settings flag gate evaluated to enabled.  Cached
 * after first call; safe in tight inner loops. */
bool cpu_diff_block_is_active(void);

/* Optional one-shot setup; installs the bus RAM-write tap when active.
 * Idempotent.  Called from system_boot once g_settings is finalized. */
void cpu_diff_block_install(void);

/* JIT prologue / epilogue hooks.  Both are abs64-called from x64_backend. */
void cpu_diff_block_enter(u32 block_pc, u32 block_size);
void cpu_diff_block_exit (void);

/* Bus tap entry point; bus.c's ram_write_byte/half/word call this when
 * `g_bus_ram_write_tap` is non-NULL.  writer_pc is the writing instruction
 * PC (g_cpu_state.current_instruction_pc); harness ignores it. */
void cpu_diff_block_ram_tap(u32 paddr, u8 size, u32 value, u32 writer_pc);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_CPU_DIFF_BLOCK_H */
