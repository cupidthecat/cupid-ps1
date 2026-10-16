#include "cpu_types.h"

#include "common/assert.h"

static const char* const cpu_reg_names[36] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
  "t4",   "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra", "hi", "lo", "pc", "npc",
};

const char* cpu_get_reg_name(cpu_reg_t reg)
{
  DebugAssert(reg < CPU_REG_COUNT);
  return cpu_reg_names[(u8)reg];
}

bool cpu_is_nop_instruction(cpu_instruction_t inst)
{
  /* The R3000A canonical NOP is sll $zero,$zero,0, which encodes as 0. */
  return (inst.bits == 0u);
}

bool cpu_is_branch_instruction(cpu_instruction_t inst)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_J:
    case CPU_OP_JAL:
    case CPU_OP_B:
    case CPU_OP_BEQ:
    case CPU_OP_BGTZ:
    case CPU_OP_BLEZ:
    case CPU_OP_BNE:
      return true;

    case CPU_OP_FUNCT:
      switch ((cpu_instruction_funct_t)inst.r.funct)
      {
        case CPU_FUNCT_JR:
        case CPU_FUNCT_JALR:
          return true;
        default:
          return false;
      }

    default:
      return false;
  }
}

bool cpu_is_unconditional_branch_instruction(cpu_instruction_t inst)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_J:
    case CPU_OP_JAL:
    case CPU_OP_B:
      return true;

    case CPU_OP_BEQ:
      /* beq $zero,$zero,offset is the canonical unconditional branch. */
      return (inst.i.rs == (unsigned int)CPU_REG_ZERO && inst.i.rt == (unsigned int)CPU_REG_ZERO);

    case CPU_OP_FUNCT:
      switch ((cpu_instruction_funct_t)inst.r.funct)
      {
        case CPU_FUNCT_JR:
        case CPU_FUNCT_JALR:
          return true;
        default:
          return false;
      }

    default:
      return false;
  }
}

bool cpu_is_direct_branch_instruction(cpu_instruction_t inst)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_J:
    case CPU_OP_JAL:
    case CPU_OP_B:
    case CPU_OP_BEQ:
    case CPU_OP_BGTZ:
    case CPU_OP_BLEZ:
    case CPU_OP_BNE:
      return true;
    default:
      return false;
  }
}

 virtual_memory_address_t cpu_get_direct_branch_target(cpu_instruction_t inst,
                                                      virtual_memory_address_t instruction_pc) 
{
  /* MIPS branch targets are computed from PC+4 (the delay-slot PC). */
  const virtual_memory_address_t pc = instruction_pc + 4u;

  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_J:
    case CPU_OP_JAL:
      return (pc & UINT32_C(0xF0000000)) | ((u32)inst.j.target << 2);

    case CPU_OP_B:
    case CPU_OP_BEQ:
    case CPU_OP_BGTZ:
    case CPU_OP_BLEZ:
    case CPU_OP_BNE:
      return pc + (cpu_instr_imm_sext32(inst) << 2);

    default:
      return pc;
  }
}

bool cpu_is_call_instruction(cpu_instruction_t inst)
{
  return (inst.any.op == (unsigned int)CPU_OP_FUNCT && inst.r.funct == (unsigned int)CPU_FUNCT_JALR) ||
         (inst.any.op == (unsigned int)CPU_OP_JAL);
}

bool cpu_is_return_instruction(cpu_instruction_t inst)
{
  if (inst.any.op != (unsigned int)CPU_OP_FUNCT)
    return false;

  /* j(al)r ra is the standard return convention. */
  if (inst.r.funct == (unsigned int)CPU_FUNCT_JR || inst.r.funct == (unsigned int)CPU_FUNCT_JALR)
    return (inst.r.rs == (unsigned int)CPU_REG_RA);

  return false;
}

bool cpu_is_memory_load_instruction(cpu_instruction_t inst)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_LB:
    case CPU_OP_LH:
    case CPU_OP_LW:
    case CPU_OP_LBU:
    case CPU_OP_LHU:
    case CPU_OP_LWL:
    case CPU_OP_LWR:
    case CPU_OP_LWC2:
      return true;
    default:
      return false;
  }
}

bool cpu_is_memory_store_instruction(cpu_instruction_t inst)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_SB:
    case CPU_OP_SH:
    case CPU_OP_SW:
    case CPU_OP_SWL:
    case CPU_OP_SWR:
    case CPU_OP_SWC2:
      return true;
    default:
      return false;
  }
}

bool cpu_get_load_store_effective_address(cpu_instruction_t inst, const cpu_registers_t* regs,
                                          virtual_memory_address_t* out_addr)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_LB:
    case CPU_OP_LH:
    case CPU_OP_LW:
    case CPU_OP_LBU:
    case CPU_OP_LHU:
    case CPU_OP_LWC2:
    case CPU_OP_SB:
    case CPU_OP_SH:
    case CPU_OP_SW:
    case CPU_OP_SWC2:
      *out_addr = regs->r[inst.i.rs] + cpu_instr_imm_sext32(inst);
      return true;

    /* Unaligned word loads/stores mask off the low two bits to the word boundary. */
    case CPU_OP_LWL:
    case CPU_OP_LWR:
    case CPU_OP_SWL:
    case CPU_OP_SWR:
      *out_addr = (regs->r[inst.i.rs] + cpu_instr_imm_sext32(inst)) & ~UINT32_C(3);
      return true;

    default:
      return false;
  }
}

bool cpu_instruction_has_load_delay(cpu_instruction_t inst)
{
  switch ((cpu_instruction_op_t)inst.any.op)
  {
    /* All loads exhibit the classic 1-cycle MIPS-I load delay slot. */
    case CPU_OP_LB:
    case CPU_OP_LH:
    case CPU_OP_LW:
    case CPU_OP_LBU:
    case CPU_OP_LHU:
    case CPU_OP_LWL:
    case CPU_OP_LWR:
      return true;

    case CPU_OP_COP0:
    case CPU_OP_COP2:
      /* mfcN/cfcN are the only coprocessor moves that have a load delay. */
      if (cpu_cop_is_common_instruction(inst))
      {
        const cpu_cop_common_instruction_t common_op = cpu_cop_common_op(inst);
        return (common_op == CPU_COP_COMMON_CFCN || common_op == CPU_COP_COMMON_MFCN);
      }
      return false;

    default:
      return false;
  }
}

bool cpu_is_exit_block_instruction(cpu_instruction_t inst)
{
  if (inst.any.op != (unsigned int)CPU_OP_FUNCT)
    return false;

  switch ((cpu_instruction_funct_t)inst.r.funct)
  {
    case CPU_FUNCT_SYSCALL:
    case CPU_FUNCT_BREAK:
      return true;
    default:
      return false;
  }
}

/* Bit-set tables for known-valid opcodes / funct codes.  Each map covers
 * 64 entries split across two u32 words. */
static const u32 cpu_valid_op_map[2] = {
  /* word 0: ops 0..31 */
  (1u << CPU_OP_B) | (1u << CPU_OP_J) | (1u << CPU_OP_JAL) | (1u << CPU_OP_BEQ) | (1u << CPU_OP_BNE) |
    (1u << CPU_OP_BLEZ) | (1u << CPU_OP_BGTZ) | (1u << CPU_OP_ADDI) | (1u << CPU_OP_ADDIU) | (1u << CPU_OP_SLTI) |
    (1u << CPU_OP_SLTIU) | (1u << CPU_OP_ANDI) | (1u << CPU_OP_ORI) | (1u << CPU_OP_XORI) | (1u << CPU_OP_LUI) |
    (1u << CPU_OP_COP0) | (1u << CPU_OP_COP1) | (1u << CPU_OP_COP2) | (1u << CPU_OP_COP3), 
  /* word 1: ops 32..63 */
  (1u << (CPU_OP_LB - 32)) | (1u << (CPU_OP_LH - 32)) | (1u << (CPU_OP_LWL - 32)) | (1u << (CPU_OP_LW - 32)) |
    (1u << (CPU_OP_LBU - 32)) | (1u << (CPU_OP_LHU - 32)) | (1u << (CPU_OP_LWR - 32)) | (1u << (CPU_OP_SB - 32)) |
    (1u << (CPU_OP_SH - 32)) | (1u << (CPU_OP_SWL - 32)) | (1u << (CPU_OP_SW - 32)) | (1u << (CPU_OP_SWR - 32)) |
    (1u << (CPU_OP_LWC0 - 32)) | (1u << (CPU_OP_LWC1 - 32)) | (1u << (CPU_OP_LWC2 - 32)) | (1u << (CPU_OP_LWC3 - 32)) |
    (1u << (CPU_OP_SWC0 - 32)) | (1u << (CPU_OP_SWC1 - 32)) | (1u << (CPU_OP_SWC2 - 32)) | (1u << (CPU_OP_SWC3 - 32)), 
};

static const u32 cpu_valid_func_map[2] = {
  /* word 0: funct 0..31 */
  (1u << CPU_FUNCT_SLL) | (1u << CPU_FUNCT_SRL) | (1u << CPU_FUNCT_SRA) | (1u << CPU_FUNCT_SLLV) |
    (1u << CPU_FUNCT_SRLV) | (1u << CPU_FUNCT_SRAV) | (1u << CPU_FUNCT_JR) | (1u << CPU_FUNCT_JALR) |
    (1u << CPU_FUNCT_SYSCALL) | (1u << CPU_FUNCT_BREAK) | (1u << CPU_FUNCT_MFHI) | (1u << CPU_FUNCT_MTHI) |
    (1u << CPU_FUNCT_MFLO) | (1u << CPU_FUNCT_MTLO) | (1u << CPU_FUNCT_MULT) | (1u << CPU_FUNCT_MULTU) |
    (1u << CPU_FUNCT_DIV) | (1u << CPU_FUNCT_DIVU), 
  /* word 1: funct 32..63 */
  (1u << (CPU_FUNCT_ADD - 32)) | (1u << (CPU_FUNCT_ADDU - 32)) | (1u << (CPU_FUNCT_SUB - 32)) |
    (1u << (CPU_FUNCT_SUBU - 32)) | (1u << (CPU_FUNCT_AND - 32)) | (1u << (CPU_FUNCT_OR - 32)) |
    (1u << (CPU_FUNCT_XOR - 32)) | (1u << (CPU_FUNCT_NOR - 32)) | (1u << (CPU_FUNCT_SLT - 32)) |
    (1u << (CPU_FUNCT_SLTU - 32)), 
};

bool cpu_is_valid_instruction(cpu_instruction_t inst)
{
  const unsigned int op = inst.any.op;
  if (op == (unsigned int)CPU_OP_FUNCT)
  {
    const unsigned int f = inst.r.funct;
    return (cpu_valid_func_map[f / 32u] & (1u << (f % 32u))) != 0u;
  }
  return (cpu_valid_op_map[op / 32u] & (1u << (op % 32u))) != 0u;
}
