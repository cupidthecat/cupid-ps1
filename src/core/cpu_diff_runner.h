/* Bounded pre-instruction traces for ordinary and cached interpretation.
 * Native code is compared using independent machine checkpoints instead. */
#ifndef CUPID_CORE_CPU_DIFF_RUNNER_H
#define CUPID_CORE_CPU_DIFF_RUNNER_H

#include "common/types.h"

/* CUPID_DIFF_TRACE names a new file; CUPID_DIFF_LIMIT is a positive decimal count.
 * Initialization rejects native execution, invalid counts and existing files. */
bool cpu_diff_initialize(void);
bool cpu_diff_is_enabled(void);
void cpu_diff_record_instruction(u32 pc, u32 instruction);
/* Call after execution and load-delay advancement. A true result requests
 * an immediate exit from the CPU dispatcher. */
bool cpu_diff_after_instruction(void);
/* Writes a completion footer only after reaching the limit. Checks write/close errors. */
bool cpu_diff_shutdown(void);

#endif
