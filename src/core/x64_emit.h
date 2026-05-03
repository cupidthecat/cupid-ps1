/*
 * Minimal x86_64 instruction encoder for the cupid-ps1 recompiler.
 *
 * own ARM / RISCV / LoongArch backends (each rolls its own encoder); the
 * x64 backend originally used Xbyak, which brought C++ template
 * machinery we don't want in a pure-C tree.
 *
 * Scope: just what cpu_recompiler_x64.c emits - no general-purpose x86
 * assembler.  All ops use a tiny [base + disp32] addressing model; fastmem
 * will extend to [base + index*scale + disp32] when needed.
 *
 * Conventions:
 *   - Sizes are 8/16/32/64-bit, named X64_SZ_*.
 *   - Memory operand is `x64_mem_t {base, disp}`.  `disp == 0` is allowed
 *     and the encoder still emits a 1-byte displacement (mod=01, disp8=0)
 *     when the base is RBP/R13 - those registers force a displacement.
 *   - Branch sites are returned as `u8*` so the caller can patch the
 *     32-bit relative displacement after both ends are emitted.
 *   - Out-of-buffer writes set emit->overflow.  Every emit op short-circuits
 *     on overflow so the caller checks once at end of block.
 */

#ifndef CUPID_CORE_X64_EMIT_H
#define CUPID_CORE_X64_EMIT_H

#include "common/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  X64_RAX = 0, X64_RCX, X64_RDX, X64_RBX,
  X64_RSP,     X64_RBP, X64_RSI, X64_RDI,
  X64_R8,      X64_R9,  X64_R10, X64_R11,
  X64_R12,     X64_R13, X64_R14, X64_R15,
} x64_reg_t;

typedef enum {
  X64_SZ_8  = 0,
  X64_SZ_16 = 1,
  X64_SZ_32 = 2,
  X64_SZ_64 = 3,
} x64_size_t;

/* Condition codes encoded as low nibble of the Jcc/SETcc/CMOVcc opcode. */
typedef enum {
  X64_CC_O  = 0x0, X64_CC_NO = 0x1,
  X64_CC_B  = 0x2, X64_CC_AE = 0x3, /* B == below == CF=1 (unsigned <) */
  X64_CC_E  = 0x4, X64_CC_NE = 0x5,
  X64_CC_BE = 0x6, X64_CC_A  = 0x7,
  X64_CC_S  = 0x8, X64_CC_NS = 0x9,
  X64_CC_P  = 0xA, X64_CC_NP = 0xB,
  X64_CC_L  = 0xC, X64_CC_GE = 0xD,
  X64_CC_LE = 0xE, X64_CC_G  = 0xF,
} x64_cond_t;

/* ALU op encoded as bits[5:3] of the modrm.reg field in the
 * "ALU r/m, imm8/imm32" form.  Same numbering used by the
 * "ALU r, r/m" opcode group (00+op*8). */
typedef enum {
  X64_ALU_ADD = 0,
  X64_ALU_OR  = 1,
  X64_ALU_ADC = 2,
  X64_ALU_SBB = 3,
  X64_ALU_AND = 4,
  X64_ALU_SUB = 5,
  X64_ALU_XOR = 6,
  X64_ALU_CMP = 7,
} x64_alu_t;

/* Shift op encoded as bits[5:3] of modrm.reg in the C0/C1/D0/D1/D2/D3 group. */
typedef enum {
  X64_SH_ROL = 0,
  X64_SH_ROR = 1,
  X64_SH_RCL = 2,
  X64_SH_RCR = 3,
  X64_SH_SHL = 4,
  X64_SH_SHR = 5,
  X64_SH_SAL = 6, /* synonym of SHL on x86 */
  X64_SH_SAR = 7,
} x64_shift_t;

typedef struct {
  u8*    code;       /* base of writable buffer */
  size_t cap;
  size_t off;        /* current write offset */
  bool   overflow;   /* set if any emit ran past cap */
} x64_emit_t;

typedef struct {
  x64_reg_t base;
  s32       disp;
} x64_mem_t;

static inline x64_mem_t x64_mem(x64_reg_t base, s32 disp)
{
  x64_mem_t m = {base, disp};
  return m;
}

/* SIB-indexed memory operand: `[base + index*scale + disp]`.
 * scale is the literal multiplier (1, 2, 4, or 8).  index == X64_RSP is
 * reserved by the encoding to mean "no index"; pass X64_RSP only via the
 * non-SIB `x64_mem_t` form, never here. */
typedef struct {
  x64_reg_t base;
  x64_reg_t index;
  u8        scale; /* 1, 2, 4, 8 */
  s32       disp;
} x64_mem_bis_t;

static inline x64_mem_bis_t x64_msib(x64_reg_t base, x64_reg_t index, u8 scale, s32 disp)
{
  x64_mem_bis_t m = {base, index, scale, disp};
  return m;
}

void   x64_emit_init   (x64_emit_t*, void* buf, size_t cap);
size_t x64_emit_size   (const x64_emit_t*);
u8*    x64_emit_cursor (x64_emit_t*); /* &buf[off]; for branch-site capture */

void x64_mov_r_r    (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_reg_t src);
void x64_mov_r_imm32(x64_emit_t*, x64_size_t, x64_reg_t dst, s32 imm); /* sign-extends to 64 in 64-bit form */
void x64_mov_r_imm64(x64_emit_t*, x64_reg_t dst, u64 imm);             /* MOVABS, 10-byte form */
void x64_mov_r_m    (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_mem_t src);
void x64_mov_m_r    (x64_emit_t*, x64_size_t, x64_mem_t dst, x64_reg_t src);
void x64_mov_m_imm32(x64_emit_t*, x64_size_t, x64_mem_t dst, s32 imm);

void x64_movzx_r_r8 (x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_reg_t src);
void x64_movzx_r_r16(x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_reg_t src);
void x64_movsx_r_r8 (x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_reg_t src);
void x64_movsx_r_r16(x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_reg_t src);
void x64_movzx_r_m8 (x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_t src);
void x64_movzx_r_m16(x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_t src);
void x64_movsx_r_m8 (x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_t src);
void x64_movsx_r_m16(x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_t src);
void x64_movsxd_r_r (x64_emit_t*, x64_reg_t dst, x64_reg_t src);              /* movsxd r64, r32 */
void x64_movsxd_r_m (x64_emit_t*, x64_reg_t dst, x64_mem_t src);

void x64_alu_r_r    (x64_emit_t*, x64_alu_t, x64_size_t, x64_reg_t dst, x64_reg_t src);
void x64_alu_r_imm  (x64_emit_t*, x64_alu_t, x64_size_t, x64_reg_t dst, s32 imm);
void x64_alu_r_m    (x64_emit_t*, x64_alu_t, x64_size_t, x64_reg_t dst, x64_mem_t src);
void x64_alu_m_r    (x64_emit_t*, x64_alu_t, x64_size_t, x64_mem_t dst, x64_reg_t src);
void x64_alu_m_imm  (x64_emit_t*, x64_alu_t, x64_size_t, x64_mem_t dst, s32 imm);

/* TEST is encoded outside the 8-op ALU group. */
void x64_test_r_r   (x64_emit_t*, x64_size_t, x64_reg_t a, x64_reg_t b);
void x64_test_r_imm (x64_emit_t*, x64_size_t, x64_reg_t a, s32 imm);
void x64_test_m_imm (x64_emit_t*, x64_size_t, x64_mem_t a, s32 imm);

void x64_shift_r_imm(x64_emit_t*, x64_shift_t, x64_size_t, x64_reg_t dst, u8 imm);
void x64_shift_r_cl (x64_emit_t*, x64_shift_t, x64_size_t, x64_reg_t dst);

void x64_neg_r      (x64_emit_t*, x64_size_t, x64_reg_t r);
void x64_not_r      (x64_emit_t*, x64_size_t, x64_reg_t r);
void x64_inc_r      (x64_emit_t*, x64_size_t, x64_reg_t r);
void x64_dec_r      (x64_emit_t*, x64_size_t, x64_reg_t r);

void x64_imul_r_r     (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_reg_t src);
void x64_imul_r_r_imm (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_reg_t src, s32 imm);
void x64_imul1_r      (x64_emit_t*, x64_size_t, x64_reg_t r);  /* implicit RAX, full result in RDX:RAX */
void x64_mul1_r       (x64_emit_t*, x64_size_t, x64_reg_t r);
void x64_idiv_r       (x64_emit_t*, x64_size_t, x64_reg_t r);
void x64_div_r        (x64_emit_t*, x64_size_t, x64_reg_t r);
void x64_cdq          (x64_emit_t*); /* sign-extend EAX into EDX */
void x64_cqo          (x64_emit_t*); /* sign-extend RAX into RDX */

void x64_lea_r_m      (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_mem_t src);

void x64_setcc_r      (x64_emit_t*, x64_cond_t, x64_reg_t dst); /* writes one byte */
void x64_cmovcc_r_r   (x64_emit_t*, x64_cond_t, x64_size_t, x64_reg_t dst, x64_reg_t src);

u8*  x64_jmp_rel32    (x64_emit_t*);
u8*  x64_jcc_rel32    (x64_emit_t*, x64_cond_t);
void x64_jmp_rel8     (x64_emit_t*, s8 disp);  /* disp counts from the byte AFTER the JMP */
void x64_jcc_rel8     (x64_emit_t*, x64_cond_t, s8 disp);
void x64_jmp_r        (x64_emit_t*, x64_reg_t r);
void x64_jmp_m        (x64_emit_t*, x64_mem_t m);
u8*  x64_call_rel32   (x64_emit_t*);
void x64_call_r       (x64_emit_t*, x64_reg_t r);
void x64_call_m       (x64_emit_t*, x64_mem_t m);
void x64_ret          (x64_emit_t*);
void x64_ret_imm16    (x64_emit_t*, u16 imm);

/* Patches a previously-emitted rel32 site so it points at `target`.
 * `site` is the address of the disp32 (i.e. what x64_jmp_rel32 returned).
 * Computes (target - (site + 4)). */
void x64_patch_rel32  (u8* site, const void* target);

void x64_push_r       (x64_emit_t*, x64_reg_t r);  /* always 64-bit on x64 */
void x64_pop_r        (x64_emit_t*, x64_reg_t r);
void x64_push_imm32   (x64_emit_t*, s32 imm);
void x64_pushf        (x64_emit_t*);
void x64_popf         (x64_emit_t*);

void x64_nop          (x64_emit_t*);
void x64_nop_n        (x64_emit_t*, u32 bytes); /* multi-byte NOPs up to 9 bytes per instr */
void x64_int3         (x64_emit_t*);
void x64_ud2          (x64_emit_t*);
void x64_xchg_r_r     (x64_emit_t*, x64_size_t, x64_reg_t a, x64_reg_t b);
void x64_bswap_r      (x64_emit_t*, x64_size_t, x64_reg_t r);

void x64_mov_r_msib    (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_mem_bis_t src);
void x64_mov_msib_r    (x64_emit_t*, x64_size_t, x64_mem_bis_t dst, x64_reg_t src);
void x64_mov_msib_imm32(x64_emit_t*, x64_size_t, x64_mem_bis_t dst, s32 imm);
void x64_alu_r_msib    (x64_emit_t*, x64_alu_t, x64_size_t, x64_reg_t dst, x64_mem_bis_t src);
void x64_alu_msib_r    (x64_emit_t*, x64_alu_t, x64_size_t, x64_mem_bis_t dst, x64_reg_t src);
void x64_movzx_r_msib8 (x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_bis_t src);
void x64_movzx_r_msib16(x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_bis_t src);
void x64_movsx_r_msib8 (x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_bis_t src);
void x64_movsx_r_msib16(x64_emit_t*, x64_size_t dst_sz, x64_reg_t dst, x64_mem_bis_t src);
void x64_lea_r_msib    (x64_emit_t*, x64_size_t, x64_reg_t dst, x64_mem_bis_t src);
void x64_jmp_msib      (x64_emit_t*, x64_mem_bis_t m);
void x64_call_msib     (x64_emit_t*, x64_mem_bis_t m);

 /* Helper: emit a 5-byte JMP rel32 in-place at `site`, target = `dst`.  Used
 * by the code-cache backlink rewriter; signature matches
 * `cpu_code_cache_set_emit_jump`'s callback. */
void x64_emit_jump_at_site(void* site, const void* dst, bool flush_icache);

/* Helper: 10-byte absolute call via MOVABS RAX, imm64 ; CALL RAX.  Used
 * for C function targets that may sit >2GB from the JIT region. */
void x64_call_abs64(x64_emit_t*, const void* target);

void x64_emit_byte    (x64_emit_t*, u8 b);
void x64_emit_bytes   (x64_emit_t*, const void* data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_X64_EMIT_H */
