/*
 * R3000A instruction disassembler.  Self-contained: emits human-readable
 * assembly into a small_string_t.  The "comment" form (which prints live
 * register values + memory snoops) needs a running CPU/GTE state so it is
 * dropped here - only the static encode→string disassembler is provided.
 */

#ifndef CUPID_CORE_CPU_DISASM_H
#define CUPID_CORE_CPU_DISASM_H

#include "common/small_string.h"
#include "common/types.h"

void cpu_disassemble(small_string_t* out, u32 instr, u32 pc);
const char* cpu_get_gte_register_name(u32 index);
const char* cpu_get_cop0_register_name(u32 index);

#endif /* CUPID_CORE_CPU_DISASM_H */
