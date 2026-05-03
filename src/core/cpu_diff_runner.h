/*
 * Differential interpreter / recompiler trace.  Active when
 * `CUPID_DIFF_TRACE` is set in the environment: both the interpreter loop
 * and the recompiler's per-instruction fallback thunk emit one line per
 * executed instruction to the file `CUPID_DIFF_TRACE` points to (or
 * /tmp/cupid-ps1-diff-{interp,recomp}.txt by default).  Each line is a
 * compact `(pc, gpr_hash, hi, lo, sr, cause)` tuple.
 *
 * Runtime cost: O(1) per instruction but allocates ~40 bytes of stderr
 * traffic - only enable for diagnostic runs.  Diff the two output files
 * with `diff` or `cmp` to find the first divergence between modes.
 *
 * v1 deliberately does NOT do lockstep snapshot+restore - that would
 * need full system-state cloning.  Trace-and-diff catches the same
 * class of bugs at a fraction of the engineering cost.
 */

#ifndef CUPID_CORE_CPU_DIFF_RUNNER_H
#define CUPID_CORE_CPU_DIFF_RUNNER_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true once at startup if CUPID_DIFF_TRACE is set; the trace
 * file is opened lazily on first record_instruction call and closed on
 * shutdown.  Subsequent calls are cheap (cached). */
bool cpu_diff_is_enabled(void);

/* Record one executed instruction.  Called from the interpreter's per-
 * instruction loop AND from the recompiler's fallback thunk (so both
 * paths produce identical traces when the recompiler-tracked register
 * state matches the interpreter's). */
void cpu_diff_record_instruction(u32 pc, u32 instruction);

/* Flush + close the trace file.  Idempotent.  Called from system shutdown. */
void cpu_diff_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_CPU_DIFF_RUNNER_H */
