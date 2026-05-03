/*
 * R3000A instruction encoding + COP0 register layout.
 *
 * Encoded as a packed union of named structs with bit-fields over an
 * `unsigned int` storage unit; one struct per encoding form (R-type,
 * I-type, J-type, COP).  GCC/clang lay bit-fields out LSB-first within
 * an unsigned int on the little-endian targets we ship (x86_64, aarch64,
 * riscv64), which matches the MIPS instruction word bit numbering used
 * by Sony's docs.  sizeof(cpu_instruction_t) is _Static_assert'd to 4.
 */

#ifndef CUPID_CORE_CPU_TYPES_H
#define CUPID_CORE_CPU_TYPES_H

#include "common/bitutils.h"
#include "common/types.h"

#include "types.h"

enum {
  CPU_INSTRUCTION_SIZE = 4u,
};

typedef enum : u8 {
  CPU_SEGMENT_KUSEG = 0, /* virtual memory */
  CPU_SEGMENT_KSEG0 = 1, /* physical memory cached */
  CPU_SEGMENT_KSEG1 = 2, /* physical memory uncached */
  CPU_SEGMENT_KSEG2 = 3,
} cpu_segment_t;

typedef enum : u8 {
  CPU_REG_ZERO = 0,
  CPU_REG_AT,
  CPU_REG_V0,
  CPU_REG_V1,
  CPU_REG_A0,
  CPU_REG_A1,
  CPU_REG_A2,
  CPU_REG_A3,
  CPU_REG_T0,
  CPU_REG_T1,
  CPU_REG_T2,
  CPU_REG_T3,
  CPU_REG_T4,
  CPU_REG_T5,
  CPU_REG_T6,
  CPU_REG_T7,
  CPU_REG_S0,
  CPU_REG_S1,
  CPU_REG_S2,
  CPU_REG_S3,
  CPU_REG_S4,
  CPU_REG_S5,
  CPU_REG_S6,
  CPU_REG_S7,
  CPU_REG_T8,
  CPU_REG_T9,
  CPU_REG_K0,
  CPU_REG_K1,
  CPU_REG_GP,
  CPU_REG_SP,
  CPU_REG_FP,
  CPU_REG_RA,
  CPU_REG_HI,
  CPU_REG_LO,
  CPU_REG_COUNT,
} cpu_reg_t;

const char* cpu_get_reg_name(cpu_reg_t reg);

typedef enum : u8 {
  CPU_OP_FUNCT = 0,
  CPU_OP_B = 1, /* i.rt: 0 bltz, 1 bgez, 16 bltzal, 17 bgezal */
  CPU_OP_J = 2,
  CPU_OP_JAL = 3,
  CPU_OP_BEQ = 4,
  CPU_OP_BNE = 5,
  CPU_OP_BLEZ = 6,
  CPU_OP_BGTZ = 7,
  CPU_OP_ADDI = 8,
  CPU_OP_ADDIU = 9,
  CPU_OP_SLTI = 10,
  CPU_OP_SLTIU = 11,
  CPU_OP_ANDI = 12,
  CPU_OP_ORI = 13,
  CPU_OP_XORI = 14,
  CPU_OP_LUI = 15,
  CPU_OP_COP0 = 16,
  CPU_OP_COP1 = 17,
  CPU_OP_COP2 = 18,
  CPU_OP_COP3 = 19,
  CPU_OP_LB = 32,
  CPU_OP_LH = 33,
  CPU_OP_LWL = 34,
  CPU_OP_LW = 35,
  CPU_OP_LBU = 36,
  CPU_OP_LHU = 37,
  CPU_OP_LWR = 38,
  CPU_OP_SB = 40,
  CPU_OP_SH = 41,
  CPU_OP_SWL = 42,
  CPU_OP_SW = 43,
  CPU_OP_SWR = 46,
  CPU_OP_LWC0 = 48,
  CPU_OP_LWC1 = 49,
  CPU_OP_LWC2 = 50,
  CPU_OP_LWC3 = 51,
  CPU_OP_SWC0 = 56,
  CPU_OP_SWC1 = 57,
  CPU_OP_SWC2 = 58,
  CPU_OP_SWC3 = 59,
} cpu_instruction_op_t;

typedef enum : u8 {
  CPU_FUNCT_SLL = 0,
  CPU_FUNCT_SRL = 2,
  CPU_FUNCT_SRA = 3,
  CPU_FUNCT_SLLV = 4,
  CPU_FUNCT_SRLV = 6,
  CPU_FUNCT_SRAV = 7,
  CPU_FUNCT_JR = 8,
  CPU_FUNCT_JALR = 9,
  CPU_FUNCT_SYSCALL = 12,
  CPU_FUNCT_BREAK = 13,
  CPU_FUNCT_MFHI = 16,
  CPU_FUNCT_MTHI = 17,
  CPU_FUNCT_MFLO = 18,
  CPU_FUNCT_MTLO = 19,
  CPU_FUNCT_MULT = 24,
  CPU_FUNCT_MULTU = 25,
  CPU_FUNCT_DIV = 26,
  CPU_FUNCT_DIVU = 27,
  CPU_FUNCT_ADD = 32,
  CPU_FUNCT_ADDU = 33,
  CPU_FUNCT_SUB = 34,
  CPU_FUNCT_SUBU = 35,
  CPU_FUNCT_AND = 36,
  CPU_FUNCT_OR = 37,
  CPU_FUNCT_XOR = 38,
  CPU_FUNCT_NOR = 39,
  CPU_FUNCT_SLT = 42,
  CPU_FUNCT_SLTU = 43,
} cpu_instruction_funct_t;

typedef enum : u8 {
  CPU_COP_COMMON_MFCN = 0x0,
  CPU_COP_COMMON_CFCN = 0x2,
  CPU_COP_COMMON_MTCN = 0x4,
  CPU_COP_COMMON_CTCN = 0x6,
} cpu_cop_common_instruction_t;

typedef enum : u8 {
  CPU_COP0_TLBR = 0x01,
  CPU_COP0_TLBWI = 0x02,
  CPU_COP0_TLBWR = 0x04,
  CPU_COP0_TLBP = 0x08,
  CPU_COP0_RFE = 0x10,
} cpu_cop0_instruction_t;

typedef union {
  u32 bits;

  struct {
    unsigned int : 26;
    unsigned int op : 6;
  } any;

  /* I-type: op | rs | rt | imm16 */
  struct {
    unsigned int imm : 16;
    unsigned int rt : 5;
    unsigned int rs : 5;
    unsigned int op : 6;
  } i;

  /* J-type: op | target26 */
  struct {
    unsigned int target : 26;
    unsigned int op : 6;
  } j;

  /* R-type: op | rs | rt | rd | shamt | funct */
  struct {
    unsigned int funct : 6;
    unsigned int shamt : 5;
    unsigned int rd : 5;
    unsigned int rt : 5;
    unsigned int rs : 5;
    unsigned int op : 6;
  } r;

  /* COP: top nibble is the opcode (0b0100), then cop_n[2], then a 25-bit
   * coprocessor-defined payload.  Bit 25 is the common-instruction flag. */
  struct {
    unsigned int imm25 : 25;
    unsigned int : 1;
    unsigned int cop_n : 2;
    unsigned int : 4;
  } cop;
} cpu_instruction_t;

_Static_assert(sizeof(cpu_instruction_t) == 4, "cpu_instruction_t must be 32 bits");

/* I-type immediate accessors (sign / zero extension helpers). */
ALWAYS_INLINE s16 cpu_instr_imm_s16(cpu_instruction_t inst) { return (s16)(u16)inst.i.imm; }
ALWAYS_INLINE u16 cpu_instr_imm_u16(cpu_instruction_t inst) { return (u16)inst.i.imm; }
ALWAYS_INLINE u32 cpu_instr_imm_sext32(cpu_instruction_t inst)
{
  return (u32)(s32)cpu_instr_imm_s16(inst);
}
ALWAYS_INLINE u32 cpu_instr_imm_zext32(cpu_instruction_t inst)
{
  return (u32)cpu_instr_imm_u16(inst);
}

/* COP encoding helpers. */
ALWAYS_INLINE bool cpu_cop_is_common_instruction(cpu_instruction_t inst)
{
  return (inst.bits & (UINT32_C(1) << 25)) == 0u;
}
ALWAYS_INLINE cpu_cop_common_instruction_t cpu_cop_common_op(cpu_instruction_t inst)
{
  return (cpu_cop_common_instruction_t)((inst.bits >> 21) & 0xFu);
}
ALWAYS_INLINE cpu_cop0_instruction_t cpu_cop0_op(cpu_instruction_t inst)
{
  return (cpu_cop0_instruction_t)(inst.bits & 0x3Fu);
}
ALWAYS_INLINE u32 cpu_cop2_index(cpu_instruction_t inst)
{
  return ((inst.bits >> 11) & 0x1Fu) | ((inst.bits >> 17) & 0x20u);
}

/* Instruction classification helpers. */
bool cpu_is_nop_instruction(cpu_instruction_t inst);
bool cpu_is_branch_instruction(cpu_instruction_t inst);
bool cpu_is_unconditional_branch_instruction(cpu_instruction_t inst);
bool cpu_is_direct_branch_instruction(cpu_instruction_t inst);
virtual_memory_address_t cpu_get_direct_branch_target(cpu_instruction_t inst,
                                                      virtual_memory_address_t instruction_pc);
bool cpu_is_call_instruction(cpu_instruction_t inst);
bool cpu_is_return_instruction(cpu_instruction_t inst);
bool cpu_is_memory_load_instruction(cpu_instruction_t inst);
bool cpu_is_memory_store_instruction(cpu_instruction_t inst);
bool cpu_instruction_has_load_delay(cpu_instruction_t inst);
bool cpu_is_exit_block_instruction(cpu_instruction_t inst);
bool cpu_is_valid_instruction(cpu_instruction_t inst);

typedef struct {
  union {
    /* +1 for the dummy load-delay write slot used by the interpreter. */
    u32 r[CPU_REG_COUNT + 1];
    struct {
      u32 zero; /* r0 */
      u32 at;   /* r1 */
      u32 v0;   /* r2 */
      u32 v1;   /* r3 */
      u32 a0;   /* r4 */
      u32 a1;   /* r5 */
      u32 a2;   /* r6 */
      u32 a3;   /* r7 */
      u32 t0;   /* r8 */
      u32 t1;   /* r9 */
      u32 t2;   /* r10 */
      u32 t3;   /* r11 */
      u32 t4;   /* r12 */
      u32 t5;   /* r13 */
      u32 t6;   /* r14 */
      u32 t7;   /* r15 */
      u32 s0;   /* r16 */
      u32 s1;   /* r17 */
      u32 s2;   /* r18 */
      u32 s3;   /* r19 */
      u32 s4;   /* r20 */
      u32 s5;   /* r21 */
      u32 s6;   /* r22 */
      u32 s7;   /* r23 */
      u32 t8;   /* r24 */
      u32 t9;   /* r25 */
      u32 k0;   /* r26 */
      u32 k1;   /* r27 */
      u32 gp;   /* r28 */
      u32 sp;   /* r29 */
      u32 fp;   /* r30 */
      u32 ra;   /* r31 */
      u32 hi;
      u32 lo;
    } named;
  };
} cpu_registers_t;

/* Effective address for load/store ops; returns true and writes *out_addr,
 * or false for non-memory instructions (replaces std::optional<>). */
bool cpu_get_load_store_effective_address(cpu_instruction_t inst, const cpu_registers_t* regs,
                                          virtual_memory_address_t* out_addr);

typedef enum : u8 {
  CPU_COP0_REG_BPC = 3,
  CPU_COP0_REG_BDA = 5,
  CPU_COP0_REG_JUMPDEST = 6,
  CPU_COP0_REG_DCIC = 7,
  CPU_COP0_REG_BAD_VADDR = 8,
  CPU_COP0_REG_BDAM = 9,
  CPU_COP0_REG_BPCM = 11,
  CPU_COP0_REG_SR = 12,
  CPU_COP0_REG_CAUSE = 13,
  CPU_COP0_REG_EPC = 14,
  CPU_COP0_REG_PRID = 15,
} cpu_cop0_reg_t;

typedef enum : u8 {
  CPU_EXCEPTION_INT = 0x00,     /* interrupt */
  CPU_EXCEPTION_MOD = 0x01,     /* TLB modification */
  CPU_EXCEPTION_TLBL = 0x02,    /* TLB load */
  CPU_EXCEPTION_TLBS = 0x03,    /* TLB store */
  CPU_EXCEPTION_ADEL = 0x04,    /* address error, data load/instruction fetch */
  CPU_EXCEPTION_ADES = 0x05,    /* address error, data store */
  CPU_EXCEPTION_IBE = 0x06,     /* bus error on instruction fetch */
  CPU_EXCEPTION_DBE = 0x07,     /* bus error on data load/store */
  CPU_EXCEPTION_SYSCALL = 0x08, /* syscall instruction */
  CPU_EXCEPTION_BP = 0x09,      /* break instruction */
  CPU_EXCEPTION_RI = 0x0A,      /* reserved instruction */
  CPU_EXCEPTION_CPU = 0x0B,     /* coprocessor unusable */
  CPU_EXCEPTION_OV = 0x0C,      /* arithmetic overflow */
} cpu_exception_t;

typedef union {
  u32 bits;
  struct {
    unsigned int IEc : 1;  /* current interrupt enable */
    unsigned int KUc : 1;  /* current kernel/user mode (user = 1) */
    unsigned int IEp : 1;  /* previous interrupt enable */
    unsigned int KUp : 1;  /* previous kernel/user mode */
    unsigned int IEo : 1;  /* old interrupt enable */
    unsigned int KUo : 1;  /* old kernel/user mode */
    unsigned int : 2;
    unsigned int Im : 8;   /* interrupt mask, 1 = allowed to trigger */
    unsigned int Isc : 1;  /* isolate cache; writes don't reach memory */
    unsigned int Swc : 1;  /* swap data and instruction caches */
    unsigned int PZ : 1;   /* zero cache parity bits */
    unsigned int CM : 1;   /* last isolated load contains data from memory */
    unsigned int PE : 1;   /* cache parity error */
    unsigned int TS : 1;   /* TLB shutdown - matched two entries */
    unsigned int BEV : 1;  /* boot exception vectors, 0 = KSEG0, 1 = KSEG1 */
    unsigned int : 2;
    unsigned int RE : 1;   /* reverse endianness in user mode */
    unsigned int : 2;
    unsigned int CU0 : 1;  /* coprocessor 0 enable in user mode */
    unsigned int CE1 : 1;  /* coprocessor 1 enable */
    unsigned int CE2 : 1;  /* coprocessor 2 enable */
    unsigned int CE3 : 1;  /* coprocessor 3 enable */
  };
} cpu_cop0_sr_t;

_Static_assert(sizeof(cpu_cop0_sr_t) == 4, "cpu_cop0_sr_t must be 32 bits");

#define CPU_COP0_SR_WRITE_MASK UINT32_C(0xF027FF3F)

/* COP0 CAUSE (exception cause register). */
typedef union {
  u32 bits;
  struct {
    unsigned int : 2;
    unsigned int Excode : 5; /* which exception occurred */
    unsigned int : 1;
    unsigned int Ip : 8;     /* interrupt pending */
    unsigned int : 12;
    unsigned int CE : 2;     /* coprocessor number if caused by a coprocessor */
    unsigned int BT : 1;     /* exception in branch delay slot, branch taken */
    unsigned int BD : 1;     /* exception in branch delay slot */
  };
} cpu_cop0_cause_t;

_Static_assert(sizeof(cpu_cop0_cause_t) == 4, "cpu_cop0_cause_t must be 32 bits");

#define CPU_COP0_CAUSE_WRITE_MASK           UINT32_C(0x00000300)
#define CPU_COP0_CAUSE_EXCEPTION_WRITE_MASK UINT32_C(0xF000007C)

ALWAYS_INLINE u32 cpu_cop0_cause_make(cpu_exception_t excode, bool BD, bool BT, u8 CE)
{
  cpu_cop0_cause_t c = {.bits = 0};
  c.Excode = (unsigned int)excode;
  c.BD = BD ? 1u : 0u;
  c.BT = BT ? 1u : 0u;
  c.CE = (unsigned int)CE;
  return c.bits;
}

/* COP0 DCIC (debug & cache invalidate control). */
typedef union {
  u32 bits;
  struct {
    unsigned int status_any_break : 1;
    unsigned int status_bpc_code_break : 1;
    unsigned int status_bda_data_break : 1;
    unsigned int status_bda_data_read_break : 1;
    unsigned int status_bda_data_write_break : 1;
    unsigned int status_any_jump_break : 1;
    unsigned int : 6;
    unsigned int jump_redirection : 2;
    unsigned int : 9;
    unsigned int super_master_enable_1 : 1;
    unsigned int execution_breakpoint_enable : 1;
    unsigned int data_access_breakpoint : 1;
    unsigned int break_on_data_read : 1;
    unsigned int break_on_data_write : 1;
    unsigned int break_on_any_jump : 1;
    unsigned int master_enable_any_jump : 1;
    unsigned int master_enable_break : 1;
    unsigned int super_master_enable_2 : 1;
  };
} cpu_cop0_dcic_t;

_Static_assert(sizeof(cpu_cop0_dcic_t) == 4, "cpu_cop0_dcic_t must be 32 bits");

#define CPU_COP0_DCIC_WRITE_MASK              UINT32_C(0xFF80F03F)
#define CPU_COP0_DCIC_ANY_BREAKPOINTS_ENABLED ((1u << 24) | (1u << 26) | (1u << 27) | (1u << 28))
#define CPU_COP0_DCIC_MASTER_ENABLE_BITS      ((1u << 23) | (1u << 31))

ALWAYS_INLINE bool cpu_cop0_dcic_execution_breakpoints_enabled(cpu_cop0_dcic_t d)
{
  const u32 mask = (1u << 23) | (1u << 24) | (1u << 31);
  return (d.bits & mask) == mask;
}
ALWAYS_INLINE bool cpu_cop0_dcic_data_read_breakpoints_enabled(cpu_cop0_dcic_t d)
{
  const u32 mask = (1u << 23) | (1u << 25) | (1u << 26) | (1u << 31);
  return (d.bits & mask) == mask;
}
ALWAYS_INLINE bool cpu_cop0_dcic_data_write_breakpoints_enabled(cpu_cop0_dcic_t d)
{
  const u32 mask = (1u << 23) | (1u << 25) | (1u << 27) | (1u << 31);
  return (d.bits & mask) == mask;
}

typedef struct {
  u32 BPC;       /* breakpoint on execute */
  u32 BDA;       /* breakpoint on data access */
  u32 TAR;       /* randomly memorized jump address */
  u32 BadVaddr;  /* bad virtual address value */
  u32 BDAM;      /* data breakpoint mask */
  u32 BPCM;      /* execute breakpoint mask */
  u32 EPC;       /* return address from trap */
  u32 PRID;      /* processor ID */
  cpu_cop0_sr_t sr;
  cpu_cop0_cause_t cause;
  cpu_cop0_dcic_t dcic;
} cpu_cop0_registers_t;

#endif /* CUPID_CORE_CPU_TYPES_H */
