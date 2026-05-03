/*
 * PC-trigger GPR-dump hook.
 *
 * When CUPID_PC_TRIGGER_PC=0x<hex> is set in the environment, the JIT and
 * interpreter both emit a call to cpu_pc_trigger_on_block(pc) at every
 * dispatch-time PC match.  The hook dumps a fixed-format GPR + COP0 + GTE
 * snapshot to CUPID_PC_TRIGGER_LOG (default /tmp/cupid-ps1-pctrig.log) so
 * `diff(1)` between an interp and a recomp run pins the first divergent
 * register on a single line.
 *
 * Cap: CUPID_PC_TRIGGER_LIMIT (default 64) snapshots per session.  Exceeding
 * the cap silently short-circuits.
 *
 * Header lines that legitimately differ between modes (frame, cycle) are
 * prefixed with "# " so a `grep '^[-+]r' diff` ignores them.  The non-header
 * portion (GPR / COP0 / GTE) is *byte-identical* across modes when state is
 * identical.
 *
 * No effect when the env var is absent: install + is_active early-return.
 */

#ifndef CUPID_CORE_CPU_PC_TRIGGER_H
#define CUPID_CORE_CPU_PC_TRIGGER_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazy gate.  Returns true once env var has been parsed and a target PC
 * has been latched.  Safe in tight loops (cached after first call). */
bool cpu_pc_trigger_is_active(void);

/* Latched target PC, or 0 if not active.  Public so the JIT can compile-
 * time-compare r->block->pc against it without a per-block C call. */
u32  cpu_pc_trigger_target_pc(void);

/* One-shot setup.  Parses CUPID_PC_TRIGGER_PC / _LOG / _LIMIT once and
 * opens the log file.  Idempotent.  Called from system_boot near
 * cpu_diff_block_install(). */
void cpu_pc_trigger_install(void);

/* Hook callee: dump current GPR + COP0 + GTE state to the log.
 * `tag_pc` is the dispatching PC (block PC for JIT, current_instruction_pc
 * for interp); recorded in the dump header for cross-check.
 * Caller is responsible for matching pc == target_pc; this is a no-op
 * when not active. */
void cpu_pc_trigger_on_block(u32 tag_pc);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_CPU_PC_TRIGGER_H */
