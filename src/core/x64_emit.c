/*
 * x86_64 instruction encoder.  Hand-rolled, intentionally narrow:
 * encodes only the forms the recompiler emits.  See x64_emit.h for scope.
 *
 * Encoding cheat-sheet (Intel SDM Vol 2, AMD APM Vol 3):
 *
 *   REX  = 0x40 | (W<<3) | (R<<2) | (X<<1) | B
 *     W: 64-bit operand size
 *     R: extension bit for ModRM.reg     (R8-R15 in reg field)
 *     X: extension bit for SIB.index
 *     B: extension bit for ModRM.rm / SIB.base / opcode-encoded reg
 *
 *   ModRM = (mod<<6) | (reg<<3) | rm
 *     mod==00 + rm==5 → [RIP + disp32]
 *     mod==00 + rm==4 → SIB follows, no disp
 *     mod==01 → disp8 follows (after SIB if rm==4)
 *     mod==10 → disp32 follows
 *     mod==11 → register-direct (rm encodes the register)
 *
 *   SIB   = (scale<<6) | (index<<3) | base
 *     index==4 means "no index register"
 *
 * Tricky cases for [base + disp]:
 *   • base low-3 == 4 (RSP / R12)  → must emit SIB byte (index=4, base=base)
 *   • base low-3 == 5 (RBP / R13)  → mod=00 means RIP-relative, so any
 *     [RBP+0] / [R13+0] must be encoded as mod=01 with disp8=0.
 */

#include "x64_emit.h"

#include "common/assert.h"

#include <string.h>

static inline void put_u8(x64_emit_t* e, u8 b)
{
  if (e->off >= e->cap) { e->overflow = true; return; }
  e->code[e->off++] = b;
}

static inline void put_u16(x64_emit_t* e, u16 v)
{
  put_u8(e, (u8)(v & 0xFFu));
  put_u8(e, (u8)((v >> 8) & 0xFFu));
}

static inline void put_u32(x64_emit_t* e, u32 v)
{
  put_u8(e, (u8)(v & 0xFFu));
  put_u8(e, (u8)((v >> 8) & 0xFFu));
  put_u8(e, (u8)((v >> 16) & 0xFFu));
  put_u8(e, (u8)((v >> 24) & 0xFFu));
}

static inline void put_u64(x64_emit_t* e, u64 v)
{
  put_u32(e, (u32)(v & 0xFFFFFFFFu));
  put_u32(e, (u32)(v >> 32));
}

void x64_emit_byte(x64_emit_t* e, u8 b)            { put_u8(e, b); }
void x64_emit_bytes(x64_emit_t* e, const void* d, size_t n)
{
  const u8* p = (const u8*)d;
  for (size_t i = 0; i < n; ++i) put_u8(e, p[i]);
}

void x64_emit_init(x64_emit_t* e, void* buf, size_t cap)
{
  e->code = (u8*)buf;
  e->cap = cap;
  e->off = 0;
  e->overflow = false;
}

size_t x64_emit_size(const x64_emit_t* e) { return e->off; }
u8*    x64_emit_cursor(x64_emit_t* e)     { return &e->code[e->off]; }

/* REX is always required when:
 *   - W bit needed (64-bit op)
 *   - any of R/X/B refers to R8-R15
 *   - 8-bit form references SPL/BPL/SIL/DIL (low byte of RSP/RBP/RSI/RDI)
 *     which only exist with a REX prefix (otherwise the encoding picks
 *     AH/CH/DH/BH instead).
 *
 * `force_rex` is for that last case; the caller flags it when emitting an
 * 8-bit op against rsp/rbp/rsi/rdi. */
static inline void emit_rex(x64_emit_t* e, bool w, bool r, bool x, bool b, bool force_rex)
{
  const u8 byte = (u8)(0x40u
                       | (w ? 0x08u : 0u)
                       | (r ? 0x04u : 0u)
                       | (x ? 0x02u : 0u) 
                       | (b ? 0x01u : 0u));
  if (w || r || x || b || force_rex)
    put_u8(e, byte);
}

static inline void emit_modrm(x64_emit_t* e, u8 mod, u8 reg, u8 rm)
{
  put_u8(e, (u8)((mod << 6) | ((reg & 7u) << 3) | (rm & 7u)));
}

static inline void emit_sib(x64_emit_t* e, u8 scale, u8 index, u8 base)
{
  put_u8(e, (u8)(((scale & 3u) << 6) | ((index & 7u) << 3) | (base & 7u)));
}

/* Emits ModRM + (optionally SIB) + (optionally disp) for a memory operand
 * `[mem.base + mem.disp]`.  `reg_field` is the value placed in modrm.reg
 * (typically the source/dest register low-3 bits, or an opcode extension). */
static void emit_mem(x64_emit_t* e, u8 reg_field, x64_mem_t mem)
{
  const u8 base_lo = (u8)(mem.base & 7u);
  const bool needs_sib = (base_lo == 4u); /* RSP / R12 */
  const bool force_disp8 = (base_lo == 5u && mem.disp == 0); /* RBP / R13 + 0 */

  u8 mod;
  if (mem.disp == 0 && !force_disp8)
    mod = 0u;
  else if (mem.disp >= -128 && mem.disp <= 127)
    mod = 1u;
  else
    mod = 2u;

  emit_modrm(e, mod, reg_field, base_lo);
  if (needs_sib)
    emit_sib(e, 0u /*scale*/, 4u /*index=none*/, base_lo);

  if (mod == 1u)
    put_u8(e, (u8)(mem.disp & 0xFFu));
  else if (mod == 2u)
    put_u32(e, (u32)mem.disp);
}

 /* True if a 32/64-bit operand-size byte register would alias AH/CH/DH/BH
 * in the absence of REX. */
static inline bool sz8_needs_rex(x64_reg_t r)
{
  return (r == X64_RSP || r == X64_RBP || r == X64_RSI || r == X64_RDI);
}

/* Emits the operand-size prefix (0x66) for X64_SZ_16 and the appropriate
 * REX byte.  `is_byte_op` is true when the encoded opcode is the 8-bit form
 * of an instruction; we still need REX for SPL/BPL/SIL/DIL byte access. */
static void emit_prefix(x64_emit_t* e, x64_size_t sz,
                        bool reg_ext, bool index_ext, bool rm_ext,
                        bool is_byte_op, bool any_high_byte_reg)
{
  if (sz == X64_SZ_16)
    put_u8(e, 0x66u);
  emit_rex(e, sz == X64_SZ_64, reg_ext, index_ext, rm_ext,
           is_byte_op && any_high_byte_reg);
}

void x64_mov_r_r(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_reg_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, src >= X64_R8, false, dst >= X64_R8,
              byte, byte && (sz8_needs_rex(dst) || sz8_needs_rex(src)));
  /* Use the "MOV r/m, r" form (opcode 0x89 / 0x88) so dst is in modrm.rm. */
  put_u8(e, byte ? 0x88u : 0x89u);
  emit_modrm(e, 3u, (u8)src, (u8)dst);
}

void x64_mov_r_imm32(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  if (sz == X64_SZ_64) {
    emit_prefix(e, X64_SZ_64, false, false, dst >= X64_R8, false, false);
    put_u8(e, 0xC7u);
    emit_modrm(e, 3u, 0u, (u8)dst);
    put_u32(e, (u32)imm);
    return;
  }
  if (byte) {
    emit_prefix(e, X64_SZ_8, false, false, dst >= X64_R8, true, sz8_needs_rex(dst));
    put_u8(e, (u8)(0xB0u | ((u8)dst & 7u)));
    put_u8(e, (u8)imm);
    return;
  }
  /* 16/32-bit: B8+rd id form is shortest (5 bytes for 32-bit). */
  emit_prefix(e, sz, false, false, dst >= X64_R8, false, false);
  put_u8(e, (u8)(0xB8u | ((u8)dst & 7u)));
  if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                 put_u32(e, (u32)imm);
}

void x64_mov_r_imm64(x64_emit_t* e, x64_reg_t dst, u64 imm)
{
  emit_prefix(e, X64_SZ_64, false, false, dst >= X64_R8, false, false);
  put_u8(e, (u8)(0xB8u | ((u8)dst & 7u)));
  put_u64(e, imm);
}

void x64_mov_r_m(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_mem_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, false, src.base >= X64_R8,
              byte, byte && sz8_needs_rex(dst));
  put_u8(e, byte ? 0x8Au : 0x8Bu);
  emit_mem(e, (u8)dst, src);
}

void x64_mov_m_r(x64_emit_t* e, x64_size_t sz, x64_mem_t dst, x64_reg_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, src >= X64_R8, false, dst.base >= X64_R8,
              byte, byte && sz8_needs_rex(src));
  put_u8(e, byte ? 0x88u : 0x89u);
  emit_mem(e, (u8)src, dst);
}

void x64_mov_m_imm32(x64_emit_t* e, x64_size_t sz, x64_mem_t dst, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, false, dst.base >= X64_R8, byte, false);
  put_u8(e, byte ? 0xC6u : 0xC7u);
  emit_mem(e, 0u, dst);
  if (byte)             put_u8 (e, (u8)imm);
  else if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                  put_u32(e, (u32)imm); /* 64-bit: imm32 sign-extended */
}

static void emit_movx_r_r(x64_emit_t* e, u8 op2,
                          x64_size_t dst_sz, x64_reg_t dst, x64_reg_t src,
                          bool src_is_byte)
{
  emit_prefix(e, dst_sz, dst >= X64_R8, false, src >= X64_R8,
              src_is_byte, src_is_byte && sz8_needs_rex(src));
  put_u8(e, 0x0Fu);
  put_u8(e, op2);
  emit_modrm(e, 3u, (u8)dst, (u8)src);
}

static void emit_movx_r_m(x64_emit_t* e, u8 op2,
                          x64_size_t dst_sz, x64_reg_t dst, x64_mem_t src)
{
  emit_prefix(e, dst_sz, dst >= X64_R8, false, src.base >= X64_R8, false, false);
  put_u8(e, 0x0Fu);
  put_u8(e, op2);
  emit_mem(e, (u8)dst, src);
}

void x64_movzx_r_r8 (x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_reg_t s) { emit_movx_r_r(e, 0xB6u, dsz, d, s, true); }
void x64_movzx_r_r16(x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_reg_t s) { emit_movx_r_r(e, 0xB7u, dsz, d, s, false); }
void x64_movsx_r_r8 (x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_reg_t s) { emit_movx_r_r(e, 0xBEu, dsz, d, s, true); }
void x64_movsx_r_r16(x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_reg_t s) { emit_movx_r_r(e, 0xBFu, dsz, d, s, false); }
void x64_movzx_r_m8 (x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_t s) { emit_movx_r_m(e, 0xB6u, dsz, d, s); }
void x64_movzx_r_m16(x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_t s) { emit_movx_r_m(e, 0xB7u, dsz, d, s); }
void x64_movsx_r_m8 (x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_t s) { emit_movx_r_m(e, 0xBEu, dsz, d, s); }
void x64_movsx_r_m16(x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_t s) { emit_movx_r_m(e, 0xBFu, dsz, d, s); }

void x64_movsxd_r_r(x64_emit_t* e, x64_reg_t dst, x64_reg_t src)
{
  emit_prefix(e, X64_SZ_64, dst >= X64_R8, false, src >= X64_R8, false, false);
  put_u8(e, 0x63u);
  emit_modrm(e, 3u, (u8)dst, (u8)src);
}
void x64_movsxd_r_m(x64_emit_t* e, x64_reg_t dst, x64_mem_t src)
{
  emit_prefix(e, X64_SZ_64, dst >= X64_R8, false, src.base >= X64_R8, false, false);
  put_u8(e, 0x63u);
  emit_mem(e, (u8)dst, src);
}

void x64_alu_r_r(x64_emit_t* e, x64_alu_t op, x64_size_t sz, x64_reg_t dst, x64_reg_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, src >= X64_R8, false, dst >= X64_R8,
              byte, byte && (sz8_needs_rex(dst) || sz8_needs_rex(src)));
  /* Use "ALU r/m, r" form so dst lives in modrm.rm. */
  const u8 base = byte ? 0x00u : 0x01u;
  put_u8(e, (u8)(base | ((u8)op << 3)));
  emit_modrm(e, 3u, (u8)src, (u8)dst);
}

void x64_alu_r_imm(x64_emit_t* e, x64_alu_t op, x64_size_t sz, x64_reg_t dst, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  const bool fits_imm8 = !byte && imm >= -128 && imm <= 127;

  emit_prefix(e, sz, false, false, dst >= X64_R8, byte, byte && sz8_needs_rex(dst));
  if (byte) {
    put_u8(e, 0x80u);
    emit_modrm(e, 3u, (u8)op, (u8)dst);
    put_u8(e, (u8)imm);
    return;
  }
  if (fits_imm8) {
    put_u8(e, 0x83u);
    emit_modrm(e, 3u, (u8)op, (u8)dst);
    put_u8(e, (u8)imm);
    return;
  }
  put_u8(e, 0x81u);
  emit_modrm(e, 3u, (u8)op, (u8)dst);
  if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                 put_u32(e, (u32)imm);
}

void x64_alu_r_m(x64_emit_t* e, x64_alu_t op, x64_size_t sz, x64_reg_t dst, x64_mem_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, false, src.base >= X64_R8,
              byte, byte && sz8_needs_rex(dst));
  const u8 base = byte ? 0x02u : 0x03u;
  put_u8(e, (u8)(base | ((u8)op << 3)));
  emit_mem(e, (u8)dst, src);
}

void x64_alu_m_r(x64_emit_t* e, x64_alu_t op, x64_size_t sz, x64_mem_t dst, x64_reg_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, src >= X64_R8, false, dst.base >= X64_R8,
              byte, byte && sz8_needs_rex(src));
  const u8 base = byte ? 0x00u : 0x01u;
  put_u8(e, (u8)(base | ((u8)op << 3)));
  emit_mem(e, (u8)src, dst);
}

void x64_alu_m_imm(x64_emit_t* e, x64_alu_t op, x64_size_t sz, x64_mem_t dst, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  const bool fits_imm8 = !byte && imm >= -128 && imm <= 127;

  emit_prefix(e, sz, false, false, dst.base >= X64_R8, byte, false);
  if (byte) {
    put_u8(e, 0x80u);
    emit_mem(e, (u8)op, dst);
    put_u8(e, (u8)imm);
    return;
  }
  if (fits_imm8) {
    put_u8(e, 0x83u);
    emit_mem(e, (u8)op, dst);
    put_u8(e, (u8)imm);
    return;
  }
  put_u8(e, 0x81u);
  emit_mem(e, (u8)op, dst);
  if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                 put_u32(e, (u32)imm);
}

void x64_test_r_r(x64_emit_t* e, x64_size_t sz, x64_reg_t a, x64_reg_t b)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, b >= X64_R8, false, a >= X64_R8,
              byte, byte && (sz8_needs_rex(a) || sz8_needs_rex(b)));
  put_u8(e, byte ? 0x84u : 0x85u);
  emit_modrm(e, 3u, (u8)b, (u8)a);
}

void x64_test_r_imm(x64_emit_t* e, x64_size_t sz, x64_reg_t a, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, false, a >= X64_R8, byte, byte && sz8_needs_rex(a));
  put_u8(e, byte ? 0xF6u : 0xF7u);
  emit_modrm(e, 3u, 0u, (u8)a);
  if (byte)             put_u8 (e, (u8)imm);
  else if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                  put_u32(e, (u32)imm);
}

void x64_test_m_imm(x64_emit_t* e, x64_size_t sz, x64_mem_t a, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, false, a.base >= X64_R8, byte, false);
  put_u8(e, byte ? 0xF6u : 0xF7u);
  emit_mem(e, 0u, a);
  if (byte)             put_u8 (e, (u8)imm);
  else if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                  put_u32(e, (u32)imm);
}

void x64_shift_r_imm(x64_emit_t* e, x64_shift_t op, x64_size_t sz, x64_reg_t dst, u8 imm)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, false, dst >= X64_R8, byte, byte && sz8_needs_rex(dst));
  if (imm == 1u) {
    put_u8(e, byte ? 0xD0u : 0xD1u);
    emit_modrm(e, 3u, (u8)op, (u8)dst);
  } else {
    put_u8(e, byte ? 0xC0u : 0xC1u);
    emit_modrm(e, 3u, (u8)op, (u8)dst);
    put_u8(e, imm);
  }
}

void x64_shift_r_cl(x64_emit_t* e, x64_shift_t op, x64_size_t sz, x64_reg_t dst)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, false, dst >= X64_R8, byte, byte && sz8_needs_rex(dst));
  put_u8(e, byte ? 0xD2u : 0xD3u);
  emit_modrm(e, 3u, (u8)op, (u8)dst);
}

static void emit_unary_r(x64_emit_t* e, x64_size_t sz, u8 reg_ext, x64_reg_t r, u8 op0_8bit, u8 op0_other)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, false, r >= X64_R8, byte, byte && sz8_needs_rex(r));
  put_u8(e, byte ? op0_8bit : op0_other);
  emit_modrm(e, 3u, reg_ext, (u8)r);
}

void x64_neg_r(x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 3u, r, 0xF6u, 0xF7u); }
void x64_not_r(x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 2u, r, 0xF6u, 0xF7u); }
void x64_inc_r(x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 0u, r, 0xFEu, 0xFFu); }
void x64_dec_r(x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 1u, r, 0xFEu, 0xFFu); }

void x64_imul_r_r(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_reg_t src)
{
  DebugAssert(sz != X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, false, src >= X64_R8, false, false);
  put_u8(e, 0x0Fu);
  put_u8(e, 0xAFu);
  emit_modrm(e, 3u, (u8)dst, (u8)src);
}

void x64_imul_r_r_imm(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_reg_t src, s32 imm)
{
  DebugAssert(sz != X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, false, src >= X64_R8, false, false);
  if (imm >= -128 && imm <= 127) {
    put_u8(e, 0x6Bu);
    emit_modrm(e, 3u, (u8)dst, (u8)src);
    put_u8(e, (u8)imm);
  } else {
    put_u8(e, 0x69u);
    emit_modrm(e, 3u, (u8)dst, (u8)src);
    if (sz == X64_SZ_16) put_u16(e, (u16)imm);
    else                 put_u32(e, (u32)imm);
  }
}

void x64_imul1_r(x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 5u, r, 0xF6u, 0xF7u); }
void x64_mul1_r (x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 4u, r, 0xF6u, 0xF7u); }
void x64_idiv_r (x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 7u, r, 0xF6u, 0xF7u); }
void x64_div_r  (x64_emit_t* e, x64_size_t sz, x64_reg_t r) { emit_unary_r(e, sz, 6u, r, 0xF6u, 0xF7u); }

void x64_cdq(x64_emit_t* e) { put_u8(e, 0x99u); }
void x64_cqo(x64_emit_t* e) { put_u8(e, 0x48u); put_u8(e, 0x99u); } /* REX.W + 0x99 */

void x64_lea_r_m(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_mem_t src)
{
  DebugAssert(sz != X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, false, src.base >= X64_R8, false, false);
  put_u8(e, 0x8Du);
  emit_mem(e, (u8)dst, src);
}

void x64_setcc_r(x64_emit_t* e, x64_cond_t cc, x64_reg_t dst)
{
  /* Always writes 1 byte; needs REX for SPL/BPL/SIL/DIL. */
  emit_rex(e, false, false, false, dst >= X64_R8, sz8_needs_rex(dst));
  put_u8(e, 0x0Fu);
  put_u8(e, (u8)(0x90u | (u8)cc));
  emit_modrm(e, 3u, 0u, (u8)dst);
}

void x64_cmovcc_r_r(x64_emit_t* e, x64_cond_t cc, x64_size_t sz, x64_reg_t dst, x64_reg_t src)
{
  DebugAssert(sz != X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, false, src >= X64_R8, false, false);
  put_u8(e, 0x0Fu);
  put_u8(e, (u8)(0x40u | (u8)cc));
  emit_modrm(e, 3u, (u8)dst, (u8)src);
}

u8* x64_jmp_rel32(x64_emit_t* e)
{
  put_u8(e, 0xE9u);
  u8* site = x64_emit_cursor(e);
  put_u32(e, 0u);
  return site;
}

u8* x64_jcc_rel32(x64_emit_t* e, x64_cond_t cc)
{
  put_u8(e, 0x0Fu);
  put_u8(e, (u8)(0x80u | (u8)cc));
  u8* site = x64_emit_cursor(e);
  put_u32(e, 0u);
  return site;
}

void x64_jmp_rel8(x64_emit_t* e, s8 disp)
{
  put_u8(e, 0xEBu);
  put_u8(e, (u8)disp);
}

void x64_jcc_rel8(x64_emit_t* e, x64_cond_t cc, s8 disp)
{
  put_u8(e, (u8)(0x70u | (u8)cc));
  put_u8(e, (u8)disp);
}

void x64_jmp_r(x64_emit_t* e, x64_reg_t r)
{
  emit_rex(e, false, false, false, r >= X64_R8, false);
  put_u8(e, 0xFFu);
  emit_modrm(e, 3u, 4u, (u8)r);
}

void x64_jmp_m(x64_emit_t* e, x64_mem_t m)
{
  emit_rex(e, false, false, false, m.base >= X64_R8, false);
  put_u8(e, 0xFFu);
  emit_mem(e, 4u, m);
}

u8* x64_call_rel32(x64_emit_t* e)
{
  put_u8(e, 0xE8u);
  u8* site = x64_emit_cursor(e);
  put_u32(e, 0u);
  return site;
}

void x64_call_r(x64_emit_t* e, x64_reg_t r)
{
  emit_rex(e, false, false, false, r >= X64_R8, false);
  put_u8(e, 0xFFu);
  emit_modrm(e, 3u, 2u, (u8)r);
}

void x64_call_m(x64_emit_t* e, x64_mem_t m)
{
  emit_rex(e, false, false, false, m.base >= X64_R8, false);
  put_u8(e, 0xFFu);
  emit_mem(e, 2u, m);
}

void x64_ret      (x64_emit_t* e)        { put_u8(e, 0xC3u); }
void x64_ret_imm16(x64_emit_t* e, u16 i) { put_u8(e, 0xC2u); put_u16(e, i); }

void x64_patch_rel32(u8* site, const void* target)
{
  const intptr_t rel = (intptr_t)target - (intptr_t)(site + 4);
  /* Caller is responsible for keeping rel in s32 range; assert in debug. */
  DebugAssert(rel >= INT32_MIN && rel <= INT32_MAX);
  const s32 d = (s32)rel;
  /* Little-endian unaligned store. */
  site[0] = (u8)(d & 0xFFu);
  site[1] = (u8)((d >> 8) & 0xFFu);
  site[2] = (u8)((d >> 16) & 0xFFu);
  site[3] = (u8)((d >> 24) & 0xFFu);
}

void x64_push_r(x64_emit_t* e, x64_reg_t r)
{
  emit_rex(e, false, false, false, r >= X64_R8, false);
  put_u8(e, (u8)(0x50u | ((u8)r & 7u)));
}

void x64_pop_r(x64_emit_t* e, x64_reg_t r)
{
  emit_rex(e, false, false, false, r >= X64_R8, false);
  put_u8(e, (u8)(0x58u | ((u8)r & 7u)));
}

void x64_push_imm32(x64_emit_t* e, s32 imm)
{
  if (imm >= -128 && imm <= 127) {
    put_u8(e, 0x6Au);
    put_u8(e, (u8)imm);
  } else {
    put_u8(e, 0x68u);
    put_u32(e, (u32)imm);
  }
}

void x64_pushf(x64_emit_t* e) { put_u8(e, 0x9Cu); }
void x64_popf (x64_emit_t* e) { put_u8(e, 0x9Du); }

void x64_nop  (x64_emit_t* e) { put_u8(e, 0x90u); }
void x64_int3 (x64_emit_t* e) { put_u8(e, 0xCCu); }
void x64_ud2  (x64_emit_t* e) { put_u8(e, 0x0Fu); put_u8(e, 0x0Bu); }

void x64_xchg_r_r(x64_emit_t* e, x64_size_t sz, x64_reg_t a, x64_reg_t b)
{
  /* Special: xchg r/RAX uses the 0x90+rd shortcut, except 0x90 alone is NOP. */
  if ((a == X64_RAX || b == X64_RAX) && sz != X64_SZ_8 && a != b) {
    const x64_reg_t other = (a == X64_RAX) ? b : a;
    emit_prefix(e, sz, false, false, other >= X64_R8, false, false);
    put_u8(e, (u8)(0x90u | ((u8)other & 7u)));
    return;
  }
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, b >= X64_R8, false, a >= X64_R8,
              byte, byte && (sz8_needs_rex(a) || sz8_needs_rex(b)));
  put_u8(e, byte ? 0x86u : 0x87u);
  emit_modrm(e, 3u, (u8)b, (u8)a);
}

void x64_bswap_r(x64_emit_t* e, x64_size_t sz, x64_reg_t r)
{
  DebugAssert(sz == X64_SZ_32 || sz == X64_SZ_64);
  emit_rex(e, sz == X64_SZ_64, false, false, r >= X64_R8, false);
  put_u8(e, 0x0Fu);
  put_u8(e, (u8)(0xC8u | ((u8)r & 7u)));
}

static u8 sib_scale_bits(u8 scale)
{
  /* 1=>0, 2=>1, 4=>2, 8=>3 */
  switch (scale) {
    case 1: return 0u;
    case 2: return 1u;
    case 4: return 2u;
    case 8: return 3u;
    default: DebugAssert(false); return 0u;
  }
}

static void emit_mem_bis(x64_emit_t* e, u8 reg_field, x64_mem_bis_t mem)
{
  DebugAssert(mem.index != X64_RSP); /* RSP can't be SIB index */

  const u8 base_lo  = (u8)(mem.base  & 7u);
  const u8 index_lo = (u8)(mem.index & 7u);
  const bool force_disp8 = (base_lo == 5u && mem.disp == 0); /* RBP/R13 */

  u8 mod;
  if (mem.disp == 0 && !force_disp8)
    mod = 0u;
  else if (mem.disp >= -128 && mem.disp <= 127)
    mod = 1u;
  else
    mod = 2u;

  emit_modrm(e, mod, reg_field, 4u /* SIB follows */);
  emit_sib(e, sib_scale_bits(mem.scale), index_lo, base_lo);

  if (mod == 1u)
    put_u8(e, (u8)(mem.disp & 0xFFu));
  else if (mod == 2u)
    put_u32(e, (u32)mem.disp);
}

void x64_mov_r_msib(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_mem_bis_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, src.index >= X64_R8, src.base >= X64_R8,
              byte, byte && sz8_needs_rex(dst));
  put_u8(e, byte ? 0x8Au : 0x8Bu);
  emit_mem_bis(e, (u8)dst, src);
}

void x64_mov_msib_r(x64_emit_t* e, x64_size_t sz, x64_mem_bis_t dst, x64_reg_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, src >= X64_R8, dst.index >= X64_R8, dst.base >= X64_R8,
              byte, byte && sz8_needs_rex(src));
  put_u8(e, byte ? 0x88u : 0x89u);
  emit_mem_bis(e, (u8)src, dst);
}

void x64_mov_msib_imm32(x64_emit_t* e, x64_size_t sz, x64_mem_bis_t dst, s32 imm)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, false, dst.index >= X64_R8, dst.base >= X64_R8, byte, false);
  put_u8(e, byte ? 0xC6u : 0xC7u);
  emit_mem_bis(e, 0u, dst);
  if (byte)                 put_u8 (e, (u8)imm);
  else if (sz == X64_SZ_16) put_u16(e, (u16)imm);
  else                      put_u32(e, (u32)imm);
}

void x64_alu_r_msib(x64_emit_t* e, x64_alu_t op, x64_size_t sz,
                    x64_reg_t dst, x64_mem_bis_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, src.index >= X64_R8, src.base >= X64_R8,
              byte, byte && sz8_needs_rex(dst));
  const u8 base = byte ? 0x02u : 0x03u;
  put_u8(e, (u8)(base | ((u8)op << 3)));
  emit_mem_bis(e, (u8)dst, src);
}

void x64_alu_msib_r(x64_emit_t* e, x64_alu_t op, x64_size_t sz,
                    x64_mem_bis_t dst, x64_reg_t src)
{
  const bool byte = (sz == X64_SZ_8);
  emit_prefix(e, sz, src >= X64_R8, dst.index >= X64_R8, dst.base >= X64_R8,
              byte, byte && sz8_needs_rex(src));
  const u8 base = byte ? 0x00u : 0x01u;
  put_u8(e, (u8)(base | ((u8)op << 3)));
  emit_mem_bis(e, (u8)src, dst);
}

static void emit_movx_r_msib(x64_emit_t* e, u8 op2,
                             x64_size_t dst_sz, x64_reg_t dst, x64_mem_bis_t src)
{
  emit_prefix(e, dst_sz, dst >= X64_R8, src.index >= X64_R8, src.base >= X64_R8,
              false, false);
  put_u8(e, 0x0Fu);
  put_u8(e, op2);
  emit_mem_bis(e, (u8)dst, src);
}
void x64_movzx_r_msib8 (x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_bis_t s) { emit_movx_r_msib(e, 0xB6u, dsz, d, s); }
void x64_movzx_r_msib16(x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_bis_t s) { emit_movx_r_msib(e, 0xB7u, dsz, d, s); }
void x64_movsx_r_msib8 (x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_bis_t s) { emit_movx_r_msib(e, 0xBEu, dsz, d, s); }
void x64_movsx_r_msib16(x64_emit_t* e, x64_size_t dsz, x64_reg_t d, x64_mem_bis_t s) { emit_movx_r_msib(e, 0xBFu, dsz, d, s); }

void x64_lea_r_msib(x64_emit_t* e, x64_size_t sz, x64_reg_t dst, x64_mem_bis_t src)
{
  DebugAssert(sz != X64_SZ_8);
  emit_prefix(e, sz, dst >= X64_R8, src.index >= X64_R8, src.base >= X64_R8,
              false, false);
  put_u8(e, 0x8Du);
  emit_mem_bis(e, (u8)dst, src);
}

void x64_jmp_msib(x64_emit_t* e, x64_mem_bis_t m)
{
  emit_rex(e, false, false, m.index >= X64_R8, m.base >= X64_R8, false);
  put_u8(e, 0xFFu);
  emit_mem_bis(e, 4u, m);
}

void x64_call_msib(x64_emit_t* e, x64_mem_bis_t m)
{
  emit_rex(e, false, false, m.index >= X64_R8, m.base >= X64_R8, false);
  put_u8(e, 0xFFu);
  emit_mem_bis(e, 2u, m);
}

/* Writes a 5-byte JMP rel32 at `site` jumping to `dst`.  Signature matches
 * `cpu_code_cache_set_emit_jump`.  `flush_icache` is unused on x64 (the
 * frontend doesn't cache instruction-byte fetches; CPUID-guaranteed
 * coherency on x86), but accepted for API parity with non-x86 backends. */
void x64_emit_jump_at_site(void* site, const void* dst, bool flush_icache)
{
  (void)flush_icache;
  u8* p = (u8*)site;
  p[0] = 0xE9u;
  const intptr_t rel = (intptr_t)dst - (intptr_t)((u8*)site + 5);
  DebugAssert(rel >= INT32_MIN && rel <= INT32_MAX);
  const s32 d = (s32)rel;
  p[1] = (u8)(d & 0xFFu);
  p[2] = (u8)((d >> 8) & 0xFFu);
  p[3] = (u8)((d >> 16) & 0xFFu);
  p[4] = (u8)((d >> 24) & 0xFFu);
}

void x64_call_abs64(x64_emit_t* e, const void* target)
{
  x64_mov_r_imm64(e, X64_RAX, (u64)(uintptr_t)target);
  x64_call_r(e, X64_RAX);
}

 /* Intel's recommended multi-byte NOP forms (1..9 bytes).  Anything longer
 * is stitched out of multiple of these. */
static const u8 s_nop1[] = {0x90};
static const u8 s_nop2[] = {0x66, 0x90};
static const u8 s_nop3[] = {0x0F, 0x1F, 0x00};
static const u8 s_nop4[] = {0x0F, 0x1F, 0x40, 0x00};
static const u8 s_nop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
static const u8 s_nop6[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
static const u8 s_nop7[] = {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00};
static const u8 s_nop8[] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 s_nop9[] = {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};

void x64_nop_n(x64_emit_t* e, u32 bytes)
{
  while (bytes > 0u) {
    u32 chunk = bytes > 9u ? 9u : bytes;
    const u8* src = NULL;
    switch (chunk) {
      case 1: src = s_nop1; break;
      case 2: src = s_nop2; break;
      case 3: src = s_nop3; break;
      case 4: src = s_nop4; break;
      case 5: src = s_nop5; break;
      case 6: src = s_nop6; break;
      case 7: src = s_nop7; break;
      case 8: src = s_nop8; break;
      case 9: src = s_nop9; break;
      default: chunk = 9; src = s_nop9; break;
    }
    x64_emit_bytes(e, src, chunk);
    bytes -= chunk;
  }
}
