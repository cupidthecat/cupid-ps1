/*
 * Minimal AArch64 instruction encoder for the cupid-ps1 recompiler.
 *
 * pure-C approach taken by x64_emit.{h,c}.  Only emits what
 * cpu_recompiler_arm64.c (the cupid-ps1 ARM64 backend) actually uses; this is
 * NOT a general-purpose AArch64 assembler.
 *
 * Conventions:
 *   - All AArch64 instructions are 32 bits.  The encoder writes raw u32s.
 *   - 64-bit operand is selected via the `sf` bit on the wire; the API
 *     uses `arm64_size_t` (32 or 64) to abstract that.
 *   - Memory operand is `arm64_mem_t {base, offset}`.  Only base+imm
 *     addressing for now; pre/post indexed forms added as needed.
 *   - Branch sites are returned as `u32*` so the caller can patch a 26-bit
 *     (B/BL) or 19-bit (B.cond, CBZ/CBNZ) relative displacement after both
 *     ends are emitted.
 *   - Out-of-buffer writes set emit->overflow.  Every emit op
 *     short-circuits on overflow so the caller checks once per block.
 */

#ifndef CUPID_CORE_ARM64_EMIT_H
#define CUPID_CORE_ARM64_EMIT_H

#include "common/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AArch64 GPR enum.  X0..X30 + SP/XZR (encoded as 31 in most ops; SP for
 * load/store + arithmetic, XZR everywhere else).  W-views share the same
 * encoding; size selects between them on the wire. */
typedef enum {
  ARM64_X0 = 0,  ARM64_X1,  ARM64_X2,  ARM64_X3,
  ARM64_X4,      ARM64_X5,  ARM64_X6,  ARM64_X7,
  ARM64_X8,      ARM64_X9,  ARM64_X10, ARM64_X11,
  ARM64_X12,     ARM64_X13, ARM64_X14, ARM64_X15,
  ARM64_X16,     ARM64_X17, ARM64_X18, ARM64_X19,
  ARM64_X20,     ARM64_X21, ARM64_X22, ARM64_X23,
  ARM64_X24,     ARM64_X25, ARM64_X26, ARM64_X27,
  ARM64_X28,     ARM64_X29, ARM64_X30,
  ARM64_SP   = 31, /* contextual: SP for ld/st/arith, XZR elsewhere */
  ARM64_XZR  = 31,
} arm64_reg_t;

 /* Operand size on the wire.  Maps to the `sf` bit in arithmetic/logical
 * instructions and to `size` in load/store. */
typedef enum {
  ARM64_SZ_32 = 0,
  ARM64_SZ_64 = 1,
} arm64_size_t;

 /* AArch64 condition codes (cond[3:0]).  Matches the encoding used by
 * B.cond, CSEL, CSET, etc. */
typedef enum {
  ARM64_CC_EQ = 0x0, ARM64_CC_NE = 0x1,
  ARM64_CC_CS = 0x2, ARM64_CC_CC = 0x3, /* HS/LO unsigned >=/< */
  ARM64_CC_MI = 0x4, ARM64_CC_PL = 0x5,
  ARM64_CC_VS = 0x6, ARM64_CC_VC = 0x7,
  ARM64_CC_HI = 0x8, ARM64_CC_LS = 0x9,
  ARM64_CC_GE = 0xA, ARM64_CC_LT = 0xB,
  ARM64_CC_GT = 0xC, ARM64_CC_LE = 0xD,
  ARM64_CC_AL = 0xE, ARM64_CC_NV = 0xF,
} arm64_cond_t;

/* ALU op selects in the data-processing-immediate / register groups.  The
 * numbering here is internal to this header; encoders pick the right
 * opcode bits per instruction class. */
typedef enum {
  ARM64_ALU_ADD = 0,
  ARM64_ALU_SUB = 1,
  ARM64_ALU_AND = 2,
  ARM64_ALU_ORR = 3,
  ARM64_ALU_EOR = 4,
  ARM64_ALU_BIC = 5, /* AND with bitwise-NOT (used to nor via two ops) */
} arm64_alu_t;

/* Shift type for register-shifted operands and shifted-register move ops. */
typedef enum {
  ARM64_SH_LSL = 0,
  ARM64_SH_LSR = 1,
  ARM64_SH_ASR = 2,
  ARM64_SH_ROR = 3,
} arm64_shift_t;

typedef struct {
  u8*    code;       /* base of writable buffer */
  size_t cap;
  size_t off;
  bool   overflow;
} arm64_emit_t;

/* Base + signed immediate offset operand for load/store.  Range depends
 * on the variant: imm9 (signed) for unscaled, imm12*scale (unsigned) for
 * scaled.  The encoder picks based on alignment + magnitude. */
typedef struct {
  arm64_reg_t base;
  s32         offset;
} arm64_mem_t;

void   arm64_emit_init  (arm64_emit_t*, void* buf, size_t cap);
void*  arm64_emit_cursor(const arm64_emit_t*);
size_t arm64_emit_size  (const arm64_emit_t*);

/* MOV (register): alias of ORR Xd, XZR, Xm.  W form when sz=32. */
void arm64_mov_r_r    (arm64_emit_t*, arm64_size_t, arm64_reg_t dst, arm64_reg_t src);

/* MOVZ / MOVK / MOVN sequence to materialize a 32 or 64-bit immediate. */
void arm64_mov_r_imm32(arm64_emit_t*, arm64_reg_t dst, u32 imm);
void arm64_mov_r_imm64(arm64_emit_t*, arm64_reg_t dst, u64 imm);

/* LDR/STR with base+imm.  Picks LDUR/STUR (imm9 unscaled) vs LDR/STR
 * (imm12 scaled) based on offset.  Only word/dword forms used. */
void arm64_ldr_r_m    (arm64_emit_t*, arm64_size_t, arm64_reg_t dst, arm64_mem_t src);
void arm64_str_r_m    (arm64_emit_t*, arm64_size_t, arm64_reg_t src, arm64_mem_t dst);

/* LDP/STP with base+imm (signed imm7 scaled).  Used by prologue/epilogue. */
void arm64_ldp_pre    (arm64_emit_t*, arm64_size_t, arm64_reg_t a, arm64_reg_t b,
                       arm64_reg_t base, s32 offset);
void arm64_stp_pre    (arm64_emit_t*, arm64_size_t, arm64_reg_t a, arm64_reg_t b,
                       arm64_reg_t base, s32 offset);
void arm64_ldp_post   (arm64_emit_t*, arm64_size_t, arm64_reg_t a, arm64_reg_t b,
                       arm64_reg_t base, s32 offset);
void arm64_stp_post   (arm64_emit_t*, arm64_size_t, arm64_reg_t a, arm64_reg_t b,
                       arm64_reg_t base, s32 offset);

/* ALU reg-reg-reg: dst = src1 OP src2.  Selects ADD/SUB/AND/ORR/EOR/BIC. */
void arm64_alu_r_r_r  (arm64_emit_t*, arm64_alu_t, arm64_size_t,
                       arm64_reg_t dst, arm64_reg_t a, arm64_reg_t b);

/* ALU reg-reg-imm.  Only ADD/SUB/AND/ORR/EOR.  AND/ORR/EOR use a
 * bitmask-immediate encoding; the encoder fails (asserts) if `imm` can't
 * be represented and the caller must pre-load via mov_r_imm. */
void arm64_alu_r_r_imm(arm64_emit_t*, arm64_alu_t, arm64_size_t,
                       arm64_reg_t dst, arm64_reg_t src, u32 imm);

/* CMP / CMN aliases.  CMP = SUBS dst=XZR; CMN = ADDS dst=XZR. */
void arm64_cmp_r_r    (arm64_emit_t*, arm64_size_t, arm64_reg_t a, arm64_reg_t b);
void arm64_cmp_r_imm  (arm64_emit_t*, arm64_size_t, arm64_reg_t a, u32 imm);

/* B (unconditional, ±128 MiB).  Returns the site so caller can patch the
 * 26-bit displacement after the target is known. */
u32* arm64_b_rel26   (arm64_emit_t*);
u32* arm64_bl_rel26  (arm64_emit_t*);

u32* arm64_b_cond_rel19(arm64_emit_t*, arm64_cond_t);
u32* arm64_cbz_rel19  (arm64_emit_t*, arm64_size_t, arm64_reg_t);
u32* arm64_cbnz_rel19 (arm64_emit_t*, arm64_size_t, arm64_reg_t);

/* BR / BLR / RET. */
void arm64_br        (arm64_emit_t*, arm64_reg_t);
void arm64_blr       (arm64_emit_t*, arm64_reg_t);
void arm64_ret       (arm64_emit_t*);

 /* Patch site emitted by *_rel{26,19} so it targets `dst`.  Asserts the
 * displacement fits in the available bits. */
void arm64_patch_rel26(u32* site, const void* dst);
void arm64_patch_rel19(u32* site, const void* dst);

/* Emit `BR Xreg` after loading Xreg with absolute target `dst`.  Used for
 * dispatcher tail jumps and inline calls to >128 MiB targets.  The
 * sequence is movz/movk × N + BR.  `tmp` is clobbered. */
void arm64_jmp_abs64 (arm64_emit_t*, const void* dst, arm64_reg_t tmp);
void arm64_call_abs64(arm64_emit_t*, const void* dst, arm64_reg_t tmp);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_ARM64_EMIT_H */
