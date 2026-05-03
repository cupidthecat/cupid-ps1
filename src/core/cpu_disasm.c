/*
 * FormatInstruction(): each table entry is a printf-ish format string
 * with `$xxx` placeholders that expand to register names, immediates,
 * branch/jump targets, etc.
 */

#include "cpu_disasm.h"

#include "cpu_types.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/small_string.h"
#include "common/types.h"

#include <stdint.h>
#include <string.h>

/* Mnemonic tables                                                    */

static const char* const cpu_disasm_base_table[64] = {
  "",                       /*  0 (special - dispatched via funct table) */
  "UNKNOWN",                /*  1 (b - dispatched separately for bltz/bgez{al}) */
  "j $jt",                  /*  2 */
  "jal $jt",                /*  3 */
  "beq $rs, $rt, $rel",     /*  4 */
  "bne $rs, $rt, $rel",     /*  5 */
  "blez $rs, $rel",         /*  6 */
  "bgtz $rs, $rel",         /*  7 */
  "addi $rt, $rs, $imm",    /*  8 */
  "addiu $rt, $rs, $imm",   /*  9 */
  "slti $rt, $rs, $imm",    /* 10 */
  "sltiu $rt, $rs, $immu",  /* 11 */
  "andi $rt, $rs, $immx",   /* 12 */
  "ori $rt, $rs, $immx",    /* 13 */
  "xori $rt, $rs, $immx",   /* 14 */
  "lui $rt, $immx",         /* 15 */
  "UNKNOWN",                /* 16 (cop0) */
  "UNKNOWN",                /* 17 (cop1) */
  "UNKNOWN",                /* 18 (cop2) */
  "UNKNOWN",                /* 19 (cop3) */
  "UNKNOWN",                /* 20 */
  "UNKNOWN",                /* 21 */
  "UNKNOWN",                /* 22 */
  "UNKNOWN",                /* 23 */
  "UNKNOWN",                /* 24 */
  "UNKNOWN",                /* 25 */
  "UNKNOWN",                /* 26 */
  "UNKNOWN",                /* 27 */
  "UNKNOWN",                /* 28 */
  "UNKNOWN",                /* 29 */
  "UNKNOWN",                /* 30 */
  "UNKNOWN",                /* 31 */
  "lb $rt, $offsetrs",      /* 32 */
  "lh $rt, $offsetrs",      /* 33 */
  "lwl $rt, $offsetrs",     /* 34 */
  "lw $rt, $offsetrs",      /* 35 */
  "lbu $rt, $offsetrs",     /* 36 */
  "lhu $rt, $offsetrs",     /* 37 */
  "lwr $rt, $offsetrs",     /* 38 */
  "UNKNOWN",                /* 39 */
  "sb $rt, $offsetrs",      /* 40 */
  "sh $rt, $offsetrs",      /* 41 */
  "swl $rt, $offsetrs",     /* 42 */
  "sw $rt, $offsetrs",      /* 43 */
  "UNKNOWN",                /* 44 */
  "UNKNOWN",                /* 45 */
  "swr $rt, $offsetrs",     /* 46 */
  "UNKNOWN",                /* 47 */
  "lwc0 $coprt, $offsetrs", /* 48 */
  "lwc1 $coprt, $offsetrs", /* 49 */
  "lwc2 $coprt, $offsetrs", /* 50 */
  "lwc3 $coprt, $offsetrs", /* 51 */
  "UNKNOWN",                /* 52 */
  "UNKNOWN",                /* 53 */
  "UNKNOWN",                /* 54 */
  "UNKNOWN",                /* 55 */
  "swc0 $coprt, $offsetrs", /* 56 */
  "swc1 $coprt, $offsetrs", /* 57 */
  "swc2 $coprt, $offsetrs", /* 58 */
  "swc3 $coprt, $offsetrs", /* 59 */
  "UNKNOWN",                /* 60 */
  "UNKNOWN",                /* 61 */
  "UNKNOWN",                /* 62 */
  "UNKNOWN",                /* 63 */
};

static const char* const cpu_disasm_special_table[64] = {
  "sll $rd, $rt, $shamt", /*  0 */
  "UNKNOWN",              /*  1 */
  "srl $rd, $rt, $shamt", /*  2 */
  "sra $rd, $rt, $shamt", /*  3 */
  "sllv $rd, $rt, $rs",   /*  4 */
  "UNKNOWN",              /*  5 */
  "srlv $rd, $rt, $rs",   /*  6 */
  "srav $rd, $rt, $rs",   /*  7 */
  "jr $rs",               /*  8 */
  "jalr $rd, $rs",        /*  9 */
  "UNKNOWN",              /* 10 */
  "UNKNOWN",              /* 11 */
  "syscall",              /* 12 */
  "break",                /* 13 */
  "UNKNOWN",              /* 14 */
  "UNKNOWN",              /* 15 */
  "mfhi $rd",             /* 16 */
  "mthi $rs",             /* 17 */
  "mflo $rd",             /* 18 */
  "mtlo $rs",             /* 19 */
  "UNKNOWN",              /* 20 */
  "UNKNOWN",              /* 21 */
  "UNKNOWN",              /* 22 */
  "UNKNOWN",              /* 23 */
  "mult $rs, $rt",        /* 24 */
  "multu $rs, $rt",       /* 25 */
  "div $rs, $rt",         /* 26 */
  "divu $rs, $rt",        /* 27 */
  "UNKNOWN",              /* 28 */
  "UNKNOWN",              /* 29 */
  "UNKNOWN",              /* 30 */
  "UNKNOWN",              /* 31 */
  "add $rd, $rs, $rt",    /* 32 */
  "addu $rd, $rs, $rt",   /* 33 */
  "sub $rd, $rs, $rt",    /* 34 */
  "subu $rd, $rs, $rt",   /* 35 */
  "and $rd, $rs, $rt",    /* 36 */
  "or $rd, $rs, $rt",     /* 37 */
  "xor $rd, $rs, $rt",    /* 38 */
  "nor $rd, $rs, $rt",    /* 39 */
  "UNKNOWN",              /* 40 */
  "UNKNOWN",              /* 41 */
  "slt $rd, $rs, $rt",    /* 42 */
  "sltu $rd, $rs, $rt",   /* 43 */
  "UNKNOWN",              /* 44 */
  "UNKNOWN",              /* 45 */
  "UNKNOWN",              /* 46 */
  "UNKNOWN",              /* 47 */
  "UNKNOWN",              /* 48 */
  "UNKNOWN",              /* 49 */
  "UNKNOWN",              /* 50 */
  "UNKNOWN",              /* 51 */
  "UNKNOWN",              /* 52 */
  "UNKNOWN",              /* 53 */
  "UNKNOWN",              /* 54 */
  "UNKNOWN",              /* 55 */
  "UNKNOWN",              /* 56 */
  "UNKNOWN",              /* 57 */
  "UNKNOWN",              /* 58 */
  "UNKNOWN",              /* 59 */
  "UNKNOWN",              /* 60 */
  "UNKNOWN",              /* 61 */
  "UNKNOWN",              /* 62 */
  "UNKNOWN",              /* 63 */
};

typedef struct {
  cpu_cop_common_instruction_t op;
  const char* fmt;
} cpu_cop_common_entry_t;

static const cpu_cop_common_entry_t cpu_disasm_cop_common_table[4] = {
  {CPU_COP_COMMON_MFCN, "mfc$cop $rt_, $coprd"},
  {CPU_COP_COMMON_CFCN, "cfc$cop $rt_, $coprdc"},
  {CPU_COP_COMMON_MTCN, "mtc$cop $rt, $coprd"},
  {CPU_COP_COMMON_CTCN, "ctc$cop $rt, $coprdc"},
};

typedef struct {
  cpu_cop0_instruction_t op;
  const char* fmt;
} cpu_cop0_entry_t;

static const cpu_cop0_entry_t cpu_disasm_cop0_table[1] = {
  {CPU_COP0_RFE, "rfe"},
};

static const char* const cpu_gte_register_names[64] = {
  "v0_xy", "v0_z",  "v1_xy", "v1_z",  "v2_xy", "v2_z",  "rgbc",  "otz",
  "ir0",   "ir1",   "ir2",   "ir3",   "sxy0",  "sxy1",  "sxy2",  "sxyp",
  "sz0",   "sz1",   "sz2",   "sz3",   "rgb0",  "rgb1",  "rgb2",  "res1",
  "mac0",  "mac1",  "mac2",  "mac3",  "irgb",  "orgb",  "lzcs",  "lzcr",
  "rt_0",  "rt_1",  "rt_2",  "rt_3",  "rt_4",  "trx",   "try",   "trz",
  "llm_0", "llm_1", "llm_2", "llm_3", "llm_4", "rbk",   "gbk",   "bbk",
  "lcm_0", "lcm_1", "lcm_2", "lcm_3", "lcm_4", "rfc",   "gfc",   "bfc",
  "ofx",   "ofy",   "h",     "dqa",   "dqb",   "zsf3",  "zsf4",  "flag", 
};

static const char* const cpu_cop0_register_names[32] = {
  "$0",  "$1",   "$2",  "BPC", "$4",   "BDA", "TAR", "DCIC", "BadA", "BDAM", "$10",
  "BPCM","SR",   "CAUSE","EPC","PRID", "$16", "$17", "$18",  "$19",  "$20",  "$21",
  "$22", "$23",  "$24", "$25", "$26",  "$27", "$28", "$29",  "$30",  "$31", 
};

typedef struct {
  const char* name;
  bool sf;
  bool lm;
  bool mvmva;
} cpu_gte_instruction_info_t;

static const cpu_gte_instruction_info_t cpu_gte_instructions[64] = {
  {"UNKNOWN", false, false, false}, /* 0x00 */
  {"rtps",    true,  true,  false}, /* 0x01 */
  {"UNKNOWN", false, false, false}, /* 0x02 */
  {"UNKNOWN", false, false, false}, /* 0x03 */
  {"UNKNOWN", false, false, false}, /* 0x04 */
  {"UNKNOWN", false, false, false}, /* 0x05 */
  {"nclip",   false, false, false}, /* 0x06 */
  {"UNKNOWN", false, false, false}, /* 0x07 */
  {"UNKNOWN", false, false, false}, /* 0x08 */
  {"UNKNOWN", false, false, false}, /* 0x09 */
  {"UNKNOWN", false, false, false}, /* 0x0A */
  {"UNKNOWN", false, false, false}, /* 0x0B */
  {"op",      true,  true,  false}, /* 0x0C */
  {"UNKNOWN", false, false, false}, /* 0x0D */
  {"UNKNOWN", false, false, false}, /* 0x0E */
  {"UNKNOWN", false, false, false}, /* 0x0F */
  {"dpcs",    true,  true,  false}, /* 0x10 */
  {"intpl",   true,  true,  false}, /* 0x11 */
  {"mvmva",   true,  true,  true},  /* 0x12 */
  {"ncds",    true,  true,  false}, /* 0x13 */
  {"cdp",     true,  true,  false}, /* 0x14 */
  {"UNKNOWN", false, false, false}, /* 0x15 */
  {"ncdt",    true,  true,  false}, /* 0x16 */
  {"UNKNOWN", false, false, false}, /* 0x17 */
  {"UNKNOWN", false, false, false}, /* 0x18 */
  {"UNKNOWN", false, false, false}, /* 0x19 */
  {"UNKNOWN", false, false, false}, /* 0x1A */
  {"nccs",    true,  true,  false}, /* 0x1B */
  {"cc",      true,  true,  false}, /* 0x1C */
  {"UNKNOWN", false, false, false}, /* 0x1D */
  {"ncs",     true,  true,  false}, /* 0x1E */
  {"UNKNOWN", false, false, false}, /* 0x1F */
  {"nct",     true,  true,  false}, /* 0x20 */
  {"UNKNOWN", false, false, false}, /* 0x21 */
  {"UNKNOWN", false, false, false}, /* 0x22 */
  {"UNKNOWN", false, false, false}, /* 0x23 */
  {"UNKNOWN", false, false, false}, /* 0x24 */
  {"UNKNOWN", false, false, false}, /* 0x25 */
  {"UNKNOWN", false, false, false}, /* 0x26 */
  {"UNKNOWN", false, false, false}, /* 0x27 */
  {"sqr",     true,  true,  false}, /* 0x28 */
  {"dcpl",    true,  true,  false}, /* 0x29 */
  {"dpct",    true,  true,  false}, /* 0x2A */
  {"UNKNOWN", false, false, false}, /* 0x2B */
  {"UNKNOWN", false, false, false}, /* 0x2C */
  {"avsz3",   false, false, false}, /* 0x2D */
  {"avsz4",   false, false, false}, /* 0x2E */
  {"UNKNOWN", false, false, false}, /* 0x2F */
  {"rtpt",    true,  true,  false}, /* 0x30 */
  {"UNKNOWN", false, false, false}, /* 0x31 */
  {"UNKNOWN", false, false, false}, /* 0x32 */
  {"UNKNOWN", false, false, false}, /* 0x33 */
  {"UNKNOWN", false, false, false}, /* 0x34 */
  {"UNKNOWN", false, false, false}, /* 0x35 */
  {"UNKNOWN", false, false, false}, /* 0x36 */
  {"UNKNOWN", false, false, false}, /* 0x37 */
  {"UNKNOWN", false, false, false}, /* 0x38 */
  {"UNKNOWN", false, false, false}, /* 0x39 */
  {"UNKNOWN", false, false, false}, /* 0x3A */
  {"UNKNOWN", false, false, false}, /* 0x3B */
  {"UNKNOWN", false, false, false}, /* 0x3C */
  {"gpf",     true,  true,  false}, /* 0x3D */
  {"gpl",     true,  true,  false}, /* 0x3E */
  {"ncct",    true,  true,  false}, /* 0x3F */
};

/* Format-string interpolator                                         */

static bool starts_with(const char* s, const char* prefix, size_t prefix_len)
{
  return strncmp(s, prefix, prefix_len) == 0;
}

static void format_instruction(small_string_t* dest, cpu_instruction_t inst, u32 pc, const char* format)
{
  small_string_clear(dest);

  const char* str = format;
  while (*str != '\0')
  {
    const char ch = *str++;
    if (ch != '$')
    {
      small_string_append_char(dest, ch);
      continue;
    }

    if (starts_with(str, "rs", 2))
    {
      small_string_append_cstr(dest, cpu_get_reg_name((cpu_reg_t)inst.r.rs));
      str += 2;
    }
    else if (starts_with(str, "rt_", 3))
    {
      small_string_append_cstr(dest, cpu_get_reg_name((cpu_reg_t)inst.r.rt));
      str += 3;
    }
    else if (starts_with(str, "rt", 2))
    {
      small_string_append_cstr(dest, cpu_get_reg_name((cpu_reg_t)inst.r.rt));
      str += 2;
    }
    else if (starts_with(str, "rd", 2))
    {
      small_string_append_cstr(dest, cpu_get_reg_name((cpu_reg_t)inst.r.rd));
      str += 2;
    }
    else if (starts_with(str, "shamt", 5))
    {
      small_string_append_sprintf(dest, "%u", (unsigned)inst.r.shamt);
      str += 5;
    }
    else if (starts_with(str, "immu", 4))
    {
      small_string_append_sprintf(dest, "%u", (unsigned)cpu_instr_imm_zext32(inst));
      str += 4;
    }
    else if (starts_with(str, "immx", 4))
    {
      small_string_append_sprintf(dest, "0x%04x", (unsigned)cpu_instr_imm_zext32(inst));
      str += 4;
    }
    else if (starts_with(str, "imm", 3))
    {
      small_string_append_sprintf(dest, "%d", (int)(s32)cpu_instr_imm_sext32(inst));
      str += 3;
    }
    else if (starts_with(str, "rel", 3))
    {
      /* PC-relative branch displacement is shifted left by 2 (word offset)
       * and added to the delay-slot PC (PC+4). */
      const u32 target = (pc + 4u) + (cpu_instr_imm_sext32(inst) << 2);
      small_string_append_sprintf(dest, "0x%08x", (unsigned)target);
      str += 3;
    }
    else if (starts_with(str, "offsetrs", 8))
    {
      const s32 offset = (s32)cpu_instr_imm_sext32(inst);
      small_string_append_sprintf(dest, "%s0x%x(%s)", (offset < 0) ? "-" : "",
                                  (unsigned)((offset < 0) ? -offset : offset),
                                  cpu_get_reg_name((cpu_reg_t)inst.i.rs));
      str += 8;
    }
    else if (starts_with(str, "jt", 2))
    {
      /* J-type target: top 4 bits inherited from delay-slot PC, low 26
       * bits come from the instruction word shifted left by 2. */
      const u32 target = ((pc + 4u) & UINT32_C(0xF0000000)) | ((u32)inst.j.target << 2);
      small_string_append_sprintf(dest, "0x%08x", (unsigned)target);
      str += 2;
    }
    else if (starts_with(str, "copcc", 5))
    {
      small_string_append_char(dest, ((inst.bits & (UINT32_C(1) << 24)) != 0u) ? 't' : 'f');
      str += 5;
    }
    else if (starts_with(str, "coprdc", 6))
    {
      if (inst.cop.cop_n == 2u)
        small_string_append_cstr(dest, cpu_get_gte_register_name((u32)inst.r.rd + 32u));
      else
        small_string_append_sprintf(dest, "%u", (unsigned)inst.r.rd);
      str += 6;
    }
    else if (starts_with(str, "coprd", 5))
    {
      if (inst.cop.cop_n == 2u)
        small_string_append_cstr(dest, cpu_get_gte_register_name((u32)inst.r.rd));
      else if (inst.cop.cop_n == 0u)
        small_string_append_cstr(dest, cpu_get_cop0_register_name((u32)inst.r.rd));
      else
        small_string_append_sprintf(dest, "%u", (unsigned)inst.r.rd);
      str += 5;
    }
    else if (starts_with(str, "coprt", 5))
    {
      if (inst.cop.cop_n == 2u)
        small_string_append_cstr(dest, cpu_get_gte_register_name((u32)inst.r.rt));
      else if (inst.cop.cop_n == 0u)
        small_string_append_cstr(dest, cpu_get_cop0_register_name((u32)inst.r.rt));
      else
        small_string_append_sprintf(dest, "%u", (unsigned)inst.r.rt);
      str += 5;
    }
    else if (starts_with(str, "cop", 3))
    {
      small_string_append_sprintf(dest, "%u", (unsigned)inst.cop.cop_n);
      str += 3;
    }
    else
    {
      Panic("Unknown operand");
    }
  }
}

/* COP / GTE dispatch                                                 */

static void format_cop_unknown(small_string_t* dest, cpu_instruction_t inst)
{
  small_string_sprintf(dest, "<cop%u 0x%08x>", (unsigned)inst.cop.cop_n, (unsigned)inst.cop.imm25);
}

static void format_cop_common(small_string_t* dest, cpu_instruction_t inst, u32 pc,
                              cpu_cop_common_instruction_t key)
{
  for (size_t i = 0; i < sizeof(cpu_disasm_cop_common_table) / sizeof(cpu_disasm_cop_common_table[0]); i++)
  {
    if (cpu_disasm_cop_common_table[i].op == key)
    {
      format_instruction(dest, inst, pc, cpu_disasm_cop_common_table[i].fmt);
      return;
    }
  }
  format_cop_unknown(dest, inst);
}

static void format_cop0(small_string_t* dest, cpu_instruction_t inst, u32 pc, cpu_cop0_instruction_t key)
{
  for (size_t i = 0; i < sizeof(cpu_disasm_cop0_table) / sizeof(cpu_disasm_cop0_table[0]); i++)
  {
    if (cpu_disasm_cop0_table[i].op == key)
    {
      format_instruction(dest, inst, pc, cpu_disasm_cop0_table[i].fmt);
      return;
    }
  }
  format_cop_unknown(dest, inst);
}

/* GTE command word fields (only the bits we need to print). */
static void format_gte_instruction(small_string_t* dest, cpu_instruction_t inst)
{
  const u32 bits = inst.bits;
  const unsigned command = (unsigned)(bits & 0x3Fu);
  const bool sf = ((bits >> 19) & 1u) != 0u;
  const bool lm = ((bits >> 10) & 1u) != 0u;
  const unsigned mvmva_mm = (bits >> 17) & 0x3u;
  const unsigned mvmva_mv = (bits >> 15) & 0x3u;
  const unsigned mvmva_tv = (bits >> 13) & 0x3u;

  const cpu_gte_instruction_info_t* t = &cpu_gte_instructions[command];
  small_string_assign_cstr(dest, t->name);

  if (t->sf && sf)
    small_string_append_cstr(dest, " sf");
  if (t->lm && lm)
    small_string_append_cstr(dest, " lm");
  if (t->mvmva)
    small_string_append_sprintf(dest, " m=%u v=%u t=%u", mvmva_mm, mvmva_mv, mvmva_tv);
}

/* Public entry points                                                */

void cpu_disassemble(small_string_t* out, u32 instr, u32 pc)
{
  const cpu_instruction_t inst = {.bits = instr};

  switch ((cpu_instruction_op_t)inst.any.op)
  {
    case CPU_OP_FUNCT:
      format_instruction(out, inst, pc, cpu_disasm_special_table[inst.r.funct]);
      return;

    case CPU_OP_COP0:
    case CPU_OP_COP1:
    case CPU_OP_COP2:
    case CPU_OP_COP3:
      if (cpu_cop_is_common_instruction(inst))
      {
        format_cop_common(out, inst, pc, cpu_cop_common_op(inst));
      }
      else
      {
        switch ((cpu_instruction_op_t)inst.any.op)
        {
          case CPU_OP_COP0:
            format_cop0(out, inst, pc, cpu_cop0_op(inst));
            break;
          case CPU_OP_COP2:
            format_gte_instruction(out, inst);
            break;
          case CPU_OP_COP1:
          case CPU_OP_COP3:
          default:
            format_cop_unknown(out, inst);
            break;
        }
      }
      return;

    /* op=1 packs four mnemonics: bltz/bgez and bltzal/bgezal,
     * disambiguated by bits in the rt field of the I-type encoding. */
    case CPU_OP_B:
    {
      const u8 rt = (u8)inst.i.rt;
      const bool bgez = (rt & 1u) != 0u;
      const bool link = (rt & 0x1Eu) == 0x10u;
      if (link)
        format_instruction(out, inst, pc, bgez ? "bgezal $rs, $rel" : "bltzal $rs, $rel");
      else
        format_instruction(out, inst, pc, bgez ? "bgez $rs, $rel" : "bltz $rs, $rel");
      return;
    }

    default:
      format_instruction(out, inst, pc, cpu_disasm_base_table[inst.any.op]);
      return;
  }
}

const char* cpu_get_gte_register_name(u32 index)
{
  return (index < (u32)(sizeof(cpu_gte_register_names) / sizeof(cpu_gte_register_names[0])))
           ? cpu_gte_register_names[index]
           : "";
}

const char* cpu_get_cop0_register_name(u32 index)
{
  return (index < (u32)(sizeof(cpu_cop0_register_names) / sizeof(cpu_cop0_register_names[0])))
           ? cpu_cop0_register_names[index]
           : "";
}
