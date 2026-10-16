/*
 * Custom AArch64 instruction encoder.  See arm64_emit.h for design notes.
 *
 * Whole TU is gated on __aarch64__ so cross-arch builds (e.g. x64 host)
 * link an empty translation unit.  The companion x64_emit.c is similarly
 * gated on __x86_64__.
 */

#include "arm64_emit.h"

#if defined(__aarch64__)

#include "common/assert.h"

#include <string.h>

void arm64_emit_init(arm64_emit_t* e, void* buf, size_t cap)
{
  e->code = (u8*)buf;
  e->cap  = cap;
  e->off  = 0u;
  e->overflow = false;
}

void* arm64_emit_cursor(const arm64_emit_t* e)
{
  return (void*)(e->code + e->off);
}

size_t arm64_emit_size(const arm64_emit_t* e) { return e->off; }

static inline void emit_u32(arm64_emit_t* e, u32 word)
{
  if (e->off + 4u > e->cap) { e->overflow = true; return; }
  /* AArch64 instructions are little-endian on all supported hosts. */
  memcpy(e->code + e->off, &word, 4u);
  e->off += 4u;
}

static inline u32 sf_bit(arm64_size_t sz) { return (sz == ARM64_SZ_64) ? 1u : 0u; }

void arm64_mov_r_r(arm64_emit_t* e, arm64_size_t sz,
                   arm64_reg_t dst, arm64_reg_t src)
{
  /* MOV alias of ORR Rd, XZR, Rm.  Encoding: sf 01 01010 00 0 Rm 000000 11111 Rd. */
  const u32 sf = sf_bit(sz);
  emit_u32(e, 0x2A0003E0u | (sf << 31) | ((u32)src << 16) | ((u32)dst & 0x1F));
}

/* Helper: emit MOVZ / MOVK / MOVN as needed to materialize `imm` into
 * `dst`.  Up to 4 instructions for 64-bit, 2 for 32-bit.  Uses MOVZ for
 * the first non-zero hw and MOVK for the rest. */
static void emit_movz_movk(arm64_emit_t* e, arm64_reg_t dst, u64 imm,
                           arm64_size_t sz)
{
  const u32 sf = sf_bit(sz);
  const u32 max_hw = (sz == ARM64_SZ_64) ? 4u : 2u;
  bool emitted = false;
  for (u32 hw = 0u; hw < max_hw; hw++) {
    const u16 chunk = (u16)(imm >> (hw * 16u));
    if (chunk == 0u && emitted)        continue;
    if (chunk == 0u && hw + 1u != max_hw) continue;
    if (!emitted) {
      /* MOVZ Rd, #imm16, LSL #(hw*16). */
      emit_u32(e, 0x52800000u | (sf << 31) | (hw << 21)
                  | ((u32)chunk << 5) | ((u32)dst & 0x1F));
      emitted = true;
    } else {
      /* MOVK Rd, #imm16, LSL #(hw*16). */
      emit_u32(e, 0x72800000u | (sf << 31) | (hw << 21)
                  | ((u32)chunk << 5) | ((u32)dst & 0x1F));
    }
  }
  /* All-zero imm with no movz emitted yet → MOVZ Rd, #0. */
  if (!emitted) {
    emit_u32(e, 0x52800000u | (sf << 31) | ((u32)dst & 0x1F));
  }
}

void arm64_mov_r_imm32(arm64_emit_t* e, arm64_reg_t dst, u32 imm)
{
  emit_movz_movk(e, dst, (u64)imm, ARM64_SZ_32);
}

void arm64_mov_r_imm64(arm64_emit_t* e, arm64_reg_t dst, u64 imm)
{
  emit_movz_movk(e, dst, imm, ARM64_SZ_64);
}

/* Pick LDR/STR (imm12 scaled) vs LDUR/STUR (imm9 unscaled) based on the
 * offset.  scale=4 for W, 8 for X. */
static void emit_ldst_imm(arm64_emit_t* e, bool is_load, arm64_size_t sz,
                          arm64_reg_t r, arm64_mem_t mem)
{
  const u32 size_bits = (sz == ARM64_SZ_64) ? 0x3u : 0x2u; /* size[1:0] */
  const u32 scale     = (sz == ARM64_SZ_64) ? 8u : 4u;
  const s32 off       = mem.offset;

  /* Scaled imm12 path: offset must be non-negative, multiple of `scale`,
   * and fit in 12 bits after dividing by `scale`. */
  if (off >= 0 && (off % (s32)scale) == 0 && (u32)(off / (s32)scale) < 4096u) {
    const u32 imm12 = (u32)(off / (s32)scale);
    /* size 11 V=0 100 0 1 0 imm12 Rn Rt   (LDR/STR unsigned offset). */
    const u32 op = is_load ? 0x01u : 0x00u; /* L bit (bit 22) */
    emit_u32(e,
      (size_bits << 30) | 0x39000000u | (op << 22) |
      (imm12 << 10) | ((u32)mem.base << 5) | ((u32)r & 0x1F));
    return;
  }

  /* Unscaled imm9 path: signed −256..255. */
  AssertMsg(off >= -256 && off <= 255,
            "arm64_emit: load/store offset out of imm9 range");
  const u32 imm9 = (u32)(off & 0x1FF);
  /* size 11 V=0 1110 0 0 imm9 0 0 Rn Rt   (LDUR/STUR). */
  const u32 op = is_load ? 0x01u : 0x00u;
  emit_u32(e,
    (size_bits << 30) | 0x38000000u | (op << 22) |
    (imm9 << 12) | ((u32)mem.base << 5) | ((u32)r & 0x1F));
}

void arm64_ldr_r_m(arm64_emit_t* e, arm64_size_t sz,
                   arm64_reg_t dst, arm64_mem_t src)
{
  emit_ldst_imm(e, /*is_load=*/true, sz, dst, src);
}

void arm64_str_r_m(arm64_emit_t* e, arm64_size_t sz,
                   arm64_reg_t src, arm64_mem_t dst)
{
  emit_ldst_imm(e, /*is_load=*/false, sz, src, dst);
}

/* LDP/STP encoding:
 *   variant     opc V L imm7         (top 10 bits)
 *   STP-post  : 1010100010
 *   LDP-post  : 1010100011
 *   STP-off   : 1010100100
 *   LDP-off   : 1010100101
 *   STP-pre   : 1010100110
 *   LDP-pre   : 1010100111
 * Then imm7 (signed, scaled by 8 for X / 4 for W), Rt2[10:14], Rn[5:9], Rt[0:4]. */
static void emit_ldst_pair(arm64_emit_t* e, arm64_size_t sz, bool is_load,
                            arm64_reg_t a, arm64_reg_t b,
                           arm64_reg_t base, s32 offset, u32 mode_bits) 
{
  const u32 scale = (sz == ARM64_SZ_64) ? 8u : 4u;
  AssertMsg((offset % (s32)scale) == 0, "arm64_emit: LDP/STP offset alignment");
  const s32 imm7s = offset / (s32)scale;
  AssertMsg(imm7s >= -64 && imm7s <= 63,
            "arm64_emit: LDP/STP imm7 out of range");
  const u32 imm7 = (u32)(imm7s & 0x7Fu);

  const u32 op_base = (sz == ARM64_SZ_64) ? 0xA8000000u : 0x28000000u;
  const u32 l       = is_load ? 1u : 0u;
  emit_u32(e,
    op_base | (mode_bits << 23) | (l << 22) | (imm7 << 15) |
    ((u32)b << 10) | ((u32)base << 5) | ((u32)a & 0x1F));
}

void arm64_ldp_pre (arm64_emit_t* e, arm64_size_t sz, arm64_reg_t a, arm64_reg_t b,
                    arm64_reg_t base, s32 off)
{ emit_ldst_pair(e, sz, /*load=*/true,  a, b, base, off, /*pre=*/0x3u); }
void arm64_stp_pre (arm64_emit_t* e, arm64_size_t sz, arm64_reg_t a, arm64_reg_t b,
                    arm64_reg_t base, s32 off)
{ emit_ldst_pair(e, sz, /*load=*/false, a, b, base, off, /*pre=*/0x3u); }
void arm64_ldp_post(arm64_emit_t* e, arm64_size_t sz, arm64_reg_t a, arm64_reg_t b,
                    arm64_reg_t base, s32 off)
{ emit_ldst_pair(e, sz, /*load=*/true,  a, b, base, off, /*post=*/0x1u); }
void arm64_stp_post(arm64_emit_t* e, arm64_size_t sz, arm64_reg_t a, arm64_reg_t b,
                    arm64_reg_t base, s32 off)
{ emit_ldst_pair(e, sz, /*load=*/false, a, b, base, off, /*post=*/0x1u); }

static u32 alu_reg_opcode(arm64_alu_t op)
{
  /* Top byte of the shifted-register form (sf cleared, set by caller). */
  switch (op) {
    case ARM64_ALU_ADD: return 0x0Bu;
    case ARM64_ALU_SUB: return 0x4Bu;
    case ARM64_ALU_AND: return 0x0Au;
    case ARM64_ALU_ORR: return 0x2Au;
    case ARM64_ALU_EOR: return 0x4Au;
    case ARM64_ALU_BIC: return 0x0Au;
  }
  AssertMsg(0, "arm64_emit: unknown alu_reg op");
  return 0u;
}

void arm64_alu_r_r_r(arm64_emit_t* e, arm64_alu_t op, arm64_size_t sz,
                     arm64_reg_t dst, arm64_reg_t a, arm64_reg_t b)
{
  const u32 sf = sf_bit(sz);
  u32 word = ((u32)alu_reg_opcode(op) << 24) | (sf << 31) |
             ((u32)b << 16) | ((u32)a << 5) | ((u32)dst & 0x1F);
  if (op == ARM64_ALU_BIC) word |= (1u << 21); /* N=1 in logical group */
  emit_u32(e, word);
}

void arm64_alu_r_r_imm(arm64_emit_t* e, arm64_alu_t op, arm64_size_t sz,
                       arm64_reg_t dst, arm64_reg_t src, u32 imm)
{
  const u32 sf = sf_bit(sz);
  if (op == ARM64_ALU_ADD || op == ARM64_ALU_SUB) {
    /* ADD/SUB (immediate): 12-bit imm with optional LSL #12. */
    u32 sh = 0u;
    u32 i12 = imm;
    if ((imm & 0xFFFu) == 0u && imm <= 0xFFFFFFu) { sh = 1u; i12 = imm >> 12; }
    AssertMsg(i12 < 4096u, "arm64_emit: ADD/SUB imm out of imm12 range");
    const u32 op_bits = (op == ARM64_ALU_ADD) ? 0x11000000u : 0x51000000u;
    emit_u32(e, op_bits | (sf << 31) | (sh << 22) | (i12 << 10) |
                ((u32)src << 5) | ((u32)dst & 0x1F));
    return;
  }

  /* AND/ORR/EOR (immediate) require a bitmask encoding the encoder
   * doesn't compute yet.  Caller must materialize via mov_r_imm + reg-reg
   * form for now. */
  AssertMsg(0, "arm64_emit: AND/ORR/EOR immediate not implemented; "
               "load via mov_r_imm + reg form");
}

/* CMP Rn, Rm  ==  SUBS XZR, Rn, Rm (shifted-reg). */
void arm64_cmp_r_r(arm64_emit_t* e, arm64_size_t sz, arm64_reg_t a, arm64_reg_t b)
{
  const u32 sf = sf_bit(sz);
  emit_u32(e, 0x6B00001Fu | (sf << 31) | ((u32)b << 16) | ((u32)a << 5));
}

/* CMP Rn, #imm  ==  SUBS XZR, Rn, #imm12 (with optional LSL #12). */
void arm64_cmp_r_imm(arm64_emit_t* e, arm64_size_t sz, arm64_reg_t a, u32 imm)
{
  const u32 sf = sf_bit(sz);
  u32 sh = 0u;
  u32 i12 = imm;
  if ((imm & 0xFFFu) == 0u && imm <= 0xFFFFFFu) { sh = 1u; i12 = imm >> 12; }
  AssertMsg(i12 < 4096u, "arm64_emit: CMP imm out of imm12 range");
  emit_u32(e, 0x7100001Fu | (sf << 31) | (sh << 22) | (i12 << 10) |
              ((u32)a << 5));
}

u32* arm64_b_rel26(arm64_emit_t* e)
{
  u32* site = (u32*)(e->code + e->off);
  emit_u32(e, 0x14000000u);
  return site;
}

u32* arm64_bl_rel26(arm64_emit_t* e)
{
  u32* site = (u32*)(e->code + e->off);
  emit_u32(e, 0x94000000u);
  return site;
}

u32* arm64_b_cond_rel19(arm64_emit_t* e, arm64_cond_t cond)
{
  u32* site = (u32*)(e->code + e->off);
  emit_u32(e, 0x54000000u | ((u32)cond & 0xFu));
  return site;
}

u32* arm64_cbz_rel19(arm64_emit_t* e, arm64_size_t sz, arm64_reg_t r)
{
  const u32 sf = sf_bit(sz);
  u32* site = (u32*)(e->code + e->off);
  emit_u32(e, 0x34000000u | (sf << 31) | ((u32)r & 0x1F));
  return site;
}

u32* arm64_cbnz_rel19(arm64_emit_t* e, arm64_size_t sz, arm64_reg_t r)
{
  const u32 sf = sf_bit(sz);
  u32* site = (u32*)(e->code + e->off);
  emit_u32(e, 0x35000000u | (sf << 31) | ((u32)r & 0x1F));
  return site;
}

void arm64_br (arm64_emit_t* e, arm64_reg_t r) { emit_u32(e, 0xD61F0000u | ((u32)r << 5)); }
void arm64_blr(arm64_emit_t* e, arm64_reg_t r) { emit_u32(e, 0xD63F0000u | ((u32)r << 5)); }
void arm64_ret(arm64_emit_t* e)                { emit_u32(e, 0xD65F0000u | ((u32)ARM64_X30 << 5)); }

void arm64_patch_rel26(u32* site, const void* dst)
{
  const s64 disp = ((const u8*)dst - (const u8*)site) / 4;
  AssertMsg(disp >= -(1LL << 25) && disp < (1LL << 25),
            "arm64_emit: B/BL displacement out of imm26 range");
  u32 word;
  memcpy(&word, site, 4u);
  word = (word & 0xFC000000u) | ((u32)disp & 0x03FFFFFFu);
  memcpy(site, &word, 4u);
}

void arm64_patch_rel19(u32* site, const void* dst)
{
  const s64 disp = ((const u8*)dst - (const u8*)site) / 4;
  AssertMsg(disp >= -(1LL << 18) && disp < (1LL << 18),
            "arm64_emit: B.cond/CBZ displacement out of imm19 range");
  u32 word;
  memcpy(&word, site, 4u);
  word = (word & 0xFF00001Fu) | (((u32)disp & 0x7FFFFu) << 5);
  memcpy(site, &word, 4u);
}

void arm64_jmp_abs64(arm64_emit_t* e, const void* dst, arm64_reg_t tmp)
{
  emit_movz_movk(e, tmp, (u64)(uintptr_t)dst, ARM64_SZ_64);
  arm64_br(e, tmp);
}

void arm64_call_abs64(arm64_emit_t* e, const void* dst, arm64_reg_t tmp)
{
  emit_movz_movk(e, tmp, (u64)(uintptr_t)dst, ARM64_SZ_64);
  arm64_blr(e, tmp);
}

#else /* !__aarch64__ */

/* On non-AArch64 hosts the whole TU is empty; an external symbol keeps
 * the object file non-empty so the linker sees consistent input. */
const int c_ps1_arm64_emit_unavailable = 0;

#endif /* __aarch64__ */
