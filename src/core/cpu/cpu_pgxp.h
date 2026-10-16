/*
 * PGXP (Parallel/Precision Geometry Transformation Pipeline) - tracks
 * pseudo-floating-point coordinates through CPU GPRs + GTE registers +
 * memory + GPU vertex submissions to recover the subpixel/subtexel
 * precision lost by the PS1 fixed-point pipeline.
 *
 * - Initialize/Reset/Shutdown lifecycle
 * - Per-MIPS-op CPU_* hooks called by the interpreter and recompiler
 * - GTE_* hooks for the geometry coprocessor
 * - GetPreciseVertex queried by the GPU when submitting vertices
 *
 * State is held alongside g_cpu_state (the per-GPR shadow array, etc. are
 * declared inline in cpu_core.h's `cpu_state_t`).
 */

#ifndef CUPID_CORE_CPU_PGXP_H
#define CUPID_CORE_CPU_PGXP_H

#include "cpu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct state_wrapper state_wrapper_t;

enum {
  PGXP_VALID_X         = (1u << 0),
  PGXP_VALID_Y         = (1u << 1),
  PGXP_VALID_Z         = (1u << 2),
  PGXP_VALID_LOWZ      = (1u << 16),
  PGXP_VALID_HIGHZ     = (1u << 17),
  PGXP_VALID_TAINTED_Z = (1u << 31),

  PGXP_VALID_XY  = PGXP_VALID_X | PGXP_VALID_Y,
  PGXP_VALID_XYZ = PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_Z,
  PGXP_VALID_ALL = PGXP_VALID_XYZ,
};

typedef struct pgxp_value_s {
  float x;
  float y;
  float z;
  u32   flags;
  u32   value;
} pgxp_value_t;

/* PGXP shadow array sizes (held inside cpu_state_t). */
enum {
  PGXP_GPR_COUNT  = 34u,    /* 32 GPRs + HI + LO (matches CPU_REG_COUNT) */
  PGXP_COP0_COUNT = 16u,
  PGXP_GTE_COUNT  = 64u,    /* 32 data + 32 ctrl */
};

/* Validate clears X/Y flags when the shadow's tracked integer disagrees
 * with the live PSX register/memory value (i.e. an opaque opcode wrote
 * since the shadow was last synced). */
static inline void pgxp_value_validate(pgxp_value_t* v, u32 psx_value)
{
  v->flags = (v->value == psx_value) ? v->flags : 0u;
}

/* Returns x/y if validated, else falls back to the PSX integer's
 * sign-extended 16-bit half. */
static inline float pgxp_value_get_valid_x(const pgxp_value_t* v, u32 psx_value)
{
  return (v->flags & PGXP_VALID_X) ? v->x : (float)(s16)(u16)psx_value;
}
static inline float pgxp_value_get_valid_y(const pgxp_value_t* v, u32 psx_value)
{
  return (v->flags & PGXP_VALID_Y) ? v->y : (float)(s16)(u16)(psx_value >> 16);
}

void pgxp_initialize(void);
void pgxp_reset     (void);
void pgxp_shutdown  (void);

bool   pgxp_should_save_state(void);
size_t pgxp_get_state_size   (bool enable_8mb_memory);
void   pgxp_do_state         (state_wrapper_t* sw);

bool pgxp_get_precise_vertex(u32 addr, u32 value, int x, int y,
                             int x_offs, int y_offs,
                             float* out_x, float* out_y, float* out_w);

void  pgxp_gte_rtps              (float x, float y, float z, u32 value);
bool  pgxp_gte_has_precise_vertices(u32 sxy0, u32 sxy1, u32 sxy2);
float pgxp_gte_nclip             (void);

void pgxp_cpu_mfc2 (cpu_instruction_t instr, u32 rd_val);
void pgxp_cpu_mtc2 (cpu_instruction_t instr, u32 rt_val);
void pgxp_cpu_lwc2 (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_swc2 (cpu_instruction_t instr, u32 addr, u32 rt_val);

void pgxp_cpu_lw   (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_lh   (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_lhu  (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_lbx  (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_lwx  (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_sb   (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_sh   (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_sw   (cpu_instruction_t instr, u32 addr, u32 rt_val);
void pgxp_cpu_swx  (cpu_instruction_t instr, u32 addr, u32 rt_val);

void pgxp_cpu_move      (u32 rd, u32 rs, u32 rs_val);
void pgxp_cpu_move_packed(u32 rd_and_rs, u32 rs_val);

void pgxp_cpu_addi (cpu_instruction_t instr, u32 rs_val);
void pgxp_cpu_andi (cpu_instruction_t instr, u32 rs_val);
void pgxp_cpu_ori  (cpu_instruction_t instr, u32 rs_val);
void pgxp_cpu_xori (cpu_instruction_t instr, u32 rs_val);
void pgxp_cpu_slti (cpu_instruction_t instr, u32 rs_val);
void pgxp_cpu_sltiu(cpu_instruction_t instr, u32 rs_val);
void pgxp_cpu_lui  (cpu_instruction_t instr);

void pgxp_cpu_add  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_sub  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_and  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_or   (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_xor  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_nor  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_slt  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_sltu (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_mult (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_multu(cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_div  (cpu_instruction_t instr, u32 rs_val, u32 rt_val);
void pgxp_cpu_divu (cpu_instruction_t instr, u32 rs_val, u32 rt_val);

void pgxp_cpu_sll  (cpu_instruction_t instr, u32 rt_val);
void pgxp_cpu_srl  (cpu_instruction_t instr, u32 rt_val);
void pgxp_cpu_sra  (cpu_instruction_t instr, u32 rt_val);
void pgxp_cpu_sllv (cpu_instruction_t instr, u32 rt_val, u32 rs_val);
void pgxp_cpu_srlv (cpu_instruction_t instr, u32 rt_val, u32 rs_val);
void pgxp_cpu_srav (cpu_instruction_t instr, u32 rt_val, u32 rs_val);

void pgxp_cpu_mfc0 (cpu_instruction_t instr, u32 rd_val);
void pgxp_cpu_mtc0 (cpu_instruction_t instr, u32 rd_val, u32 rt_val);

 /*
 * marshal MOVE args through one register.  Mirror that idiom verbatim. */
static inline u32 pgxp_pack_move_args(cpu_reg_t rd, cpu_reg_t rs)
{
  return ((u32)rd << 8) | (u32)rs;
}

static inline void pgxp_try_move(cpu_reg_t rd, cpu_reg_t rs, cpu_reg_t rt,
                                 const u32* gpr_array)
{
  u32 src;
  if      (rs == (cpu_reg_t)0) src = (u32)rt;
  else if (rt == (cpu_reg_t)0) src = (u32)rs;
  else                          return;
  pgxp_cpu_move((u32)rd, src, gpr_array[src]);
}

static inline void pgxp_try_move_imm(cpu_reg_t rd, cpu_reg_t rs, u32 imm,
                                     const u32* gpr_array)
{
  if (imm == 0u) {
    const u32 src = (u32)rs;
    pgxp_cpu_move((u32)rd, src, gpr_array[src]);
  }
}

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_CPU_PGXP_H */
