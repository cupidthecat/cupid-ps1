 /*
 *
 * Byte-level conformance tests for src/core/x64_emit.c.
 *
 * Approach: emit a single instruction into a small buffer, then memcmp
 * against the bytes from a known-good encoding (Intel SDM Vol 2 worked
 * examples + cross-checked with `gcc -S` / `objdump -d`).
 *
 * The point is regression coverage: when somebody tweaks the prefix-byte
 * logic or the modrm helper, every emit form trips its own assertion.
 *
 * Convention: `expected` arrays go MSB-first in human reading order
 * but the encoded stream is little-endian for displacements/imms.  Each
 * byte literal is the on-the-wire byte at that file offset.
 */

#include "tests/test_harness.h"

#include "core/x64_emit.h"

#include <stdint.h>
#include <string.h>

static u8 g_buf[256];

static void reset_buf(x64_emit_t* e)
{
  memset(g_buf, 0xCC, sizeof(g_buf));
  x64_emit_init(e, g_buf, sizeof(g_buf));
}

#define EXPECT_BYTES(emit_block, ...)                                       \
  do {                                                                       \
    x64_emit_t _e;                                                           \
    reset_buf(&_e);                                                          \
    do { emit_block; } while (0);                                            \
    static const u8 _exp[] = __VA_ARGS__;                                    \
    if (_e.overflow)                                                         \
      test_fail(__FILE__, __LINE__, "encoder overflowed");                   \
    if (_e.off != sizeof(_exp))                                              \
      test_fail(__FILE__, __LINE__,                                          \
                "size %zu != expected %zu", _e.off, sizeof(_exp));           \
    for (size_t _i = 0; _i < sizeof(_exp); _i++) {                           \
      if (g_buf[_i] != _exp[_i]) {                                           \
        test_fail(__FILE__, __LINE__,                                        \
                  "byte %zu: 0x%02X != 0x%02X", _i, g_buf[_i], _exp[_i]);    \
      }                                                                      \
    }                                                                        \
  } while (0)

TEST(X64Emit, mov_r_r_32) {
  EXPECT_BYTES(x64_mov_r_r(&_e, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x89, 0xC8 });
}
TEST(X64Emit, mov_r_r_64) {
  EXPECT_BYTES(x64_mov_r_r(&_e, X64_SZ_64, X64_RAX, X64_RCX),
               { 0x48, 0x89, 0xC8 });
}
TEST(X64Emit, mov_r_r_8) {
  EXPECT_BYTES(x64_mov_r_r(&_e, X64_SZ_8, X64_RAX, X64_RCX),
               { 0x88, 0xC8 });
}
TEST(X64Emit, mov_r_r_8_force_rex) {
  EXPECT_BYTES(x64_mov_r_r(&_e, X64_SZ_8, X64_RSI, X64_RDI),
               { 0x40, 0x88, 0xFE });
}
TEST(X64Emit, mov_r_r_64_high_regs) {
  /* mov r8, r9 → REX.WRB + 89 C8 = 4D 89 C8 */
  EXPECT_BYTES(x64_mov_r_r(&_e, X64_SZ_64, X64_R8, X64_R9),
               { 0x4D, 0x89, 0xC8 });
}
TEST(X64Emit, mov_r_r_16) {
  EXPECT_BYTES(x64_mov_r_r(&_e, X64_SZ_16, X64_RAX, X64_RCX),
               { 0x66, 0x89, 0xC8 });
}

TEST(X64Emit, mov_r_imm32_32) {
  EXPECT_BYTES(x64_mov_r_imm32(&_e, X64_SZ_32, X64_RAX, 0x12345678),
               { 0xB8, 0x78, 0x56, 0x34, 0x12 });
}
TEST(X64Emit, mov_r_imm32_64_signext) {
  /* C7 form sign-extends imm32 → imm64.  Used for small constants. */
  EXPECT_BYTES(x64_mov_r_imm32(&_e, X64_SZ_64, X64_RAX, -1),
               { 0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF });
}
TEST(X64Emit, mov_r_imm64_full) {
  EXPECT_BYTES(x64_mov_r_imm64(&_e, X64_RAX, 0x1234567890ABCDEFull),
               { 0x48, 0xB8,
                 0xEF, 0xCD, 0xAB, 0x90, 0x78, 0x56, 0x34, 0x12 });
}
TEST(X64Emit, mov_r_imm32_8) {
  /* mov al, 0x42 → B0 42 (no REX) */
  EXPECT_BYTES(x64_mov_r_imm32(&_e, X64_SZ_8, X64_RAX, 0x42),
               { 0xB0, 0x42 });
}
TEST(X64Emit, mov_r_imm32_high_reg) {
  /* mov r8d, 0x1 → REX.B B8+0 imm32 */
  EXPECT_BYTES(x64_mov_r_imm32(&_e, X64_SZ_32, X64_R8, 1),
               { 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00 });
}

TEST(X64Emit, mov_r_m_no_disp) {
  /* mov rax, [rcx] */
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_RCX, 0)),
               { 0x48, 0x8B, 0x01 });
}
TEST(X64Emit, mov_r_m_disp8) {
  /* mov rax, [rcx + 0x10] */
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_RCX, 0x10)),
               { 0x48, 0x8B, 0x41, 0x10 });
}
TEST(X64Emit, mov_r_m_disp32) {
  /* mov rax, [rcx + 0x100] */
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_RCX, 0x100)),
               { 0x48, 0x8B, 0x81, 0x00, 0x01, 0x00, 0x00 });
}
TEST(X64Emit, mov_r_m_rsp_base) {
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_RSP, 0)),
               { 0x48, 0x8B, 0x04, 0x24 });
}
TEST(X64Emit, mov_r_m_rbp_base_zero) {
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_RBP, 0)),
               { 0x48, 0x8B, 0x45, 0x00 });
}
TEST(X64Emit, mov_r_m_r12_base) {
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_R12, 0)),
               { 0x49, 0x8B, 0x04, 0x24 });
}
TEST(X64Emit, mov_r_m_r13_base_zero) {
  EXPECT_BYTES(x64_mov_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_R13, 0)),
               { 0x49, 0x8B, 0x45, 0x00 });
}
TEST(X64Emit, mov_m_r_disp8) {
  /* mov [rcx + 4], rax */
  EXPECT_BYTES(x64_mov_m_r(&_e, X64_SZ_64, x64_mem(X64_RCX, 4), X64_RAX),
               { 0x48, 0x89, 0x41, 0x04 });
}
TEST(X64Emit, mov_m_imm32_64) {
  EXPECT_BYTES(x64_mov_m_imm32(&_e, X64_SZ_64, x64_mem(X64_RCX, 0), 0x42),
               { 0x48, 0xC7, 0x01, 0x42, 0x00, 0x00, 0x00 });
}

TEST(X64Emit, movzx_r_r8) {
  /* movzx eax, cl */
  EXPECT_BYTES(x64_movzx_r_r8(&_e, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x0F, 0xB6, 0xC1 });
}
TEST(X64Emit, movzx_r_r16) {
  /* movzx eax, cx */
  EXPECT_BYTES(x64_movzx_r_r16(&_e, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x0F, 0xB7, 0xC1 });
}
TEST(X64Emit, movsx_r_r8) {
  /* movsx eax, cl */
  EXPECT_BYTES(x64_movsx_r_r8(&_e, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x0F, 0xBE, 0xC1 });
}
TEST(X64Emit, movsxd_r_r) {
  /* movsxd rax, ecx */
  EXPECT_BYTES(x64_movsxd_r_r(&_e, X64_RAX, X64_RCX),
               { 0x48, 0x63, 0xC1 });
}

TEST(X64Emit, add_r_r_32) {
  EXPECT_BYTES(x64_alu_r_r(&_e, X64_ALU_ADD, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x01, 0xC8 });
}
TEST(X64Emit, sub_r_r_64) {
  EXPECT_BYTES(x64_alu_r_r(&_e, X64_ALU_SUB, X64_SZ_64, X64_RAX, X64_RCX),
               { 0x48, 0x29, 0xC8 });
}
TEST(X64Emit, xor_r_r_32) {
  EXPECT_BYTES(x64_alu_r_r(&_e, X64_ALU_XOR, X64_SZ_32, X64_RAX, X64_RAX),
               { 0x31, 0xC0 });
}
TEST(X64Emit, cmp_r_r_32) {
  EXPECT_BYTES(x64_alu_r_r(&_e, X64_ALU_CMP, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x39, 0xC8 });
}

TEST(X64Emit, add_r_imm8_32) {
  /* add eax, 0x10 → uses imm8 form */
  EXPECT_BYTES(x64_alu_r_imm(&_e, X64_ALU_ADD, X64_SZ_32, X64_RAX, 0x10),
               { 0x83, 0xC0, 0x10 });
}
TEST(X64Emit, add_r_imm32_32) {
  EXPECT_BYTES(x64_alu_r_imm(&_e, X64_ALU_ADD, X64_SZ_32, X64_RAX, 0x12345678),
               { 0x81, 0xC0, 0x78, 0x56, 0x34, 0x12 });
}
TEST(X64Emit, sub_r_imm8_64) {
  EXPECT_BYTES(x64_alu_r_imm(&_e, X64_ALU_SUB, X64_SZ_64, X64_RSP, 8),
               { 0x48, 0x83, 0xEC, 0x08 });
}
TEST(X64Emit, and_r_imm32_64) {
  EXPECT_BYTES(x64_alu_r_imm(&_e, X64_ALU_AND, X64_SZ_64, X64_RAX, 0xFFFF),
               { 0x48, 0x81, 0xE0, 0xFF, 0xFF, 0x00, 0x00 });
}

TEST(X64Emit, add_r_m) {
  EXPECT_BYTES(x64_alu_r_m(&_e, X64_ALU_ADD, X64_SZ_32, X64_RAX, x64_mem(X64_RCX, 0)),
               { 0x03, 0x01 });
}
TEST(X64Emit, sub_m_r) {
  EXPECT_BYTES(x64_alu_m_r(&_e, X64_ALU_SUB, X64_SZ_32, x64_mem(X64_RCX, 0), X64_RAX),
               { 0x29, 0x01 });
}
TEST(X64Emit, or_m_imm) {
  EXPECT_BYTES(x64_alu_m_imm(&_e, X64_ALU_OR, X64_SZ_32, x64_mem(X64_RCX, 0), 0x10),
               { 0x83, 0x09, 0x10 });
}

TEST(X64Emit, test_r_r_32) {
  EXPECT_BYTES(x64_test_r_r(&_e, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x85, 0xC8 });
}
TEST(X64Emit, test_r_imm_32) {
  EXPECT_BYTES(x64_test_r_imm(&_e, X64_SZ_32, X64_RAX, 0x10),
               { 0xF7, 0xC0, 0x10, 0x00, 0x00, 0x00 });
}

TEST(X64Emit, shl_r_imm1) {
  EXPECT_BYTES(x64_shift_r_imm(&_e, X64_SH_SHL, X64_SZ_32, X64_RAX, 1),
               { 0xD1, 0xE0 });
}
TEST(X64Emit, shl_r_imm4) {
  EXPECT_BYTES(x64_shift_r_imm(&_e, X64_SH_SHL, X64_SZ_32, X64_RAX, 4),
               { 0xC1, 0xE0, 0x04 });
}
TEST(X64Emit, sar_r_imm) {
  EXPECT_BYTES(x64_shift_r_imm(&_e, X64_SH_SAR, X64_SZ_64, X64_RAX, 31),
               { 0x48, 0xC1, 0xF8, 0x1F });
}
TEST(X64Emit, shr_r_cl) {
  EXPECT_BYTES(x64_shift_r_cl(&_e, X64_SH_SHR, X64_SZ_32, X64_RAX),
               { 0xD3, 0xE8 });
}

TEST(X64Emit, neg_r) {
  EXPECT_BYTES(x64_neg_r(&_e, X64_SZ_32, X64_RAX),
               { 0xF7, 0xD8 });
}
TEST(X64Emit, not_r_64) {
  EXPECT_BYTES(x64_not_r(&_e, X64_SZ_64, X64_RAX),
               { 0x48, 0xF7, 0xD0 });
}
TEST(X64Emit, inc_r) {
  EXPECT_BYTES(x64_inc_r(&_e, X64_SZ_64, X64_RAX),
               { 0x48, 0xFF, 0xC0 });
}

TEST(X64Emit, imul_r_r) {
  EXPECT_BYTES(x64_imul_r_r(&_e, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x0F, 0xAF, 0xC1 });
}
TEST(X64Emit, imul_r_r_imm8) {
  EXPECT_BYTES(x64_imul_r_r_imm(&_e, X64_SZ_32, X64_RAX, X64_RCX, 5),
               { 0x6B, 0xC1, 0x05 });
}
TEST(X64Emit, imul_r_r_imm32) {
  EXPECT_BYTES(x64_imul_r_r_imm(&_e, X64_SZ_32, X64_RAX, X64_RCX, 0x12345678),
               { 0x69, 0xC1, 0x78, 0x56, 0x34, 0x12 });
}
TEST(X64Emit, idiv_r) {
  EXPECT_BYTES(x64_idiv_r(&_e, X64_SZ_32, X64_RCX),
               { 0xF7, 0xF9 });
}
TEST(X64Emit, cdq) { EXPECT_BYTES(x64_cdq(&_e), { 0x99 }); }
TEST(X64Emit, cqo) { EXPECT_BYTES(x64_cqo(&_e), { 0x48, 0x99 }); }

TEST(X64Emit, lea_r_m_disp8) {
  EXPECT_BYTES(x64_lea_r_m(&_e, X64_SZ_64, X64_RAX, x64_mem(X64_RCX, 0x10)),
               { 0x48, 0x8D, 0x41, 0x10 });
}

TEST(X64Emit, setcc_e) {
  EXPECT_BYTES(x64_setcc_r(&_e, X64_CC_E, X64_RAX),
               { 0x0F, 0x94, 0xC0 });
}
TEST(X64Emit, setcc_ne_high) {
  /* setne sil → REX.B forced because SIL is a low-byte alias */
  EXPECT_BYTES(x64_setcc_r(&_e, X64_CC_NE, X64_RSI),
               { 0x40, 0x0F, 0x95, 0xC6 });
}
TEST(X64Emit, cmovcc_e_32) {
  EXPECT_BYTES(x64_cmovcc_r_r(&_e, X64_CC_E, X64_SZ_32, X64_RAX, X64_RCX),
               { 0x0F, 0x44, 0xC1 });
}

TEST(X64Emit, jmp_rel32_emits_5_bytes) {
  x64_emit_t e;
  reset_buf(&e);
  u8* site = x64_jmp_rel32(&e);
  EXPECT_EQ(e.off, 5);
  EXPECT_EQ(g_buf[0], 0xE9);
  EXPECT_TRUE(site == &g_buf[1]);
}
TEST(X64Emit, jcc_rel32_emits_6_bytes) {
  x64_emit_t e;
  reset_buf(&e);
  u8* site = x64_jcc_rel32(&e, X64_CC_E);
  EXPECT_EQ(e.off, 6);
  EXPECT_EQ(g_buf[0], 0x0F);
  EXPECT_EQ(g_buf[1], 0x84);
  EXPECT_TRUE(site == &g_buf[2]);
}
TEST(X64Emit, patch_rel32_forward) {
  /* JMP at offset 0; patch to byte at offset 0x100.
   * disp = 0x100 - (1 + 4) = 0xFB */
  x64_emit_t e;
  reset_buf(&e);
  u8* site = x64_jmp_rel32(&e);
  x64_patch_rel32(site, &g_buf[0x100]);
  EXPECT_EQ(g_buf[0], 0xE9);
  EXPECT_EQ(g_buf[1], 0xFB);
  EXPECT_EQ(g_buf[2], 0x00);
  EXPECT_EQ(g_buf[3], 0x00);
  EXPECT_EQ(g_buf[4], 0x00);
}
TEST(X64Emit, patch_rel32_backward) {
  /* JMP at offset 0x10; target at offset 0x00.
   * disp = -0x10 - 5 = -0x15 = 0xFFFFFFEB */
  x64_emit_t e;
  reset_buf(&e);
  for (int i = 0; i < 0x10; i++) x64_nop(&e);
  u8* site = x64_jmp_rel32(&e);
  x64_patch_rel32(site, &g_buf[0]);
  EXPECT_EQ(g_buf[0x10], 0xE9);
  EXPECT_EQ(g_buf[0x11], 0xEB);
  EXPECT_EQ(g_buf[0x12], 0xFF);
  EXPECT_EQ(g_buf[0x13], 0xFF);
  EXPECT_EQ(g_buf[0x14], 0xFF);
}
TEST(X64Emit, jmp_r) {
  /* jmp rax */
  EXPECT_BYTES(x64_jmp_r(&_e, X64_RAX),
               { 0xFF, 0xE0 });
}
TEST(X64Emit, jmp_r_high) {
  /* jmp r10 */
  EXPECT_BYTES(x64_jmp_r(&_e, X64_R10),
               { 0x41, 0xFF, 0xE2 });
}
TEST(X64Emit, call_r) {
  EXPECT_BYTES(x64_call_r(&_e, X64_RAX),
               { 0xFF, 0xD0 });
}
TEST(X64Emit, ret) { EXPECT_BYTES(x64_ret(&_e), { 0xC3 }); }

TEST(X64Emit, push_pop_lo) {
  EXPECT_BYTES({ x64_push_r(&_e, X64_RAX); x64_pop_r(&_e, X64_RBX); },
               { 0x50, 0x5B });
}
TEST(X64Emit, push_pop_hi) {
  EXPECT_BYTES({ x64_push_r(&_e, X64_R8); x64_pop_r(&_e, X64_R9); },
               { 0x41, 0x50, 0x41, 0x59 });
}
TEST(X64Emit, push_imm8) {
  EXPECT_BYTES(x64_push_imm32(&_e, 0x10),
               { 0x6A, 0x10 });
}
TEST(X64Emit, push_imm32) {
  EXPECT_BYTES(x64_push_imm32(&_e, 0x12345678),
               { 0x68, 0x78, 0x56, 0x34, 0x12 });
}

TEST(X64Emit, nop)  { EXPECT_BYTES(x64_nop(&_e),  { 0x90 }); }
TEST(X64Emit, int3) { EXPECT_BYTES(x64_int3(&_e), { 0xCC }); }
TEST(X64Emit, ud2)  { EXPECT_BYTES(x64_ud2(&_e),  { 0x0F, 0x0B }); }
TEST(X64Emit, bswap_32) {
  EXPECT_BYTES(x64_bswap_r(&_e, X64_SZ_32, X64_RAX),
               { 0x0F, 0xC8 });
}
TEST(X64Emit, bswap_64) {
  EXPECT_BYTES(x64_bswap_r(&_e, X64_SZ_64, X64_RAX),
               { 0x48, 0x0F, 0xC8 });
}
TEST(X64Emit, xchg_with_rax_shortcut) {
  /* xchg rax, rcx → REX.W 90+rcx_lo3 = 48 91 */
  EXPECT_BYTES(x64_xchg_r_r(&_e, X64_SZ_64, X64_RAX, X64_RCX),
               { 0x48, 0x91 });
}
TEST(X64Emit, xchg_general) {
  /* xchg rcx, rdx → REX.W 87 modrm */
  EXPECT_BYTES(x64_xchg_r_r(&_e, X64_SZ_64, X64_RCX, X64_RDX),
               { 0x48, 0x87, 0xD1 });
}

TEST(X64Emit, nop_n_4) {
  EXPECT_BYTES(x64_nop_n(&_e, 4),
               { 0x0F, 0x1F, 0x40, 0x00 });
}
TEST(X64Emit, nop_n_15_uses_two_chunks) {
  /* 15 = 9 + 6 */
  x64_emit_t e;
  reset_buf(&e);
  x64_nop_n(&e, 15);
  EXPECT_EQ(e.off, 15);
  /* First 9 bytes match 9-byte NOP */
  static const u8 nop9[] = {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
  for (size_t i = 0; i < sizeof(nop9); i++) EXPECT_EQ(g_buf[i], nop9[i]);
  /* Next 6 bytes match 6-byte NOP */
  static const u8 nop6[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
  for (size_t i = 0; i < sizeof(nop6); i++) EXPECT_EQ(g_buf[9 + i], nop6[i]);
}

TEST(X64Emit, mov_r_msib_64_basic) {
  /* mov rsi, [rsi + rdx*8]
   * REX.W=48, op=8B, ModRM=mod00 reg110 rm100 → 0x34, SIB=ss11 idx010 b110 → 0xD6 */
  EXPECT_BYTES(x64_mov_r_msib(&_e, X64_SZ_64, X64_RSI, x64_msib(X64_RSI, X64_RDX, 8, 0)),
               { 0x48, 0x8B, 0x34, 0xD6 });
}
TEST(X64Emit, jmp_msib) {
  /* jmp [rsi + rdi*2]
   * No REX. op=FF, ModRM=mod00 reg100 rm100 → 0x24, SIB=ss01 idx111 b110 → 0x7E */
  EXPECT_BYTES(x64_jmp_msib(&_e, x64_msib(X64_RSI, X64_RDI, 2, 0)),
               { 0xFF, 0x24, 0x7E });
}
TEST(X64Emit, mov_msib_r_disp8) {
  /* mov [rbp + rax*4 + 0x10], ecx
   * No REX. op=89, ModRM=mod01 reg001 rm100 → 0x4C, SIB=ss10 idx000 b101 → 0x85, disp=10 */
  EXPECT_BYTES(x64_mov_msib_r(&_e, X64_SZ_32, x64_msib(X64_RBP, X64_RAX, 4, 0x10), X64_RCX),
               { 0x89, 0x4C, 0x85, 0x10 });
}
TEST(X64Emit, mov_r_msib_64_high_regs_force_disp8) {
  EXPECT_BYTES(x64_mov_r_msib(&_e, X64_SZ_64, X64_RAX, x64_msib(X64_R13, X64_R10, 8, 0)),
               { 0x4B, 0x8B, 0x44, 0xD5, 0x00 });
}
TEST(X64Emit, lea_r_msib) {
  /* lea rax, [rsi + rdi*2] */
  EXPECT_BYTES(x64_lea_r_msib(&_e, X64_SZ_64, X64_RAX, x64_msib(X64_RSI, X64_RDI, 2, 0)),
               { 0x48, 0x8D, 0x04, 0x7E });
}
TEST(X64Emit, alu_r_msib_add_32) {
  EXPECT_BYTES(x64_alu_r_msib(&_e, X64_ALU_ADD, X64_SZ_32, X64_RAX,
                              x64_msib(X64_RBX, X64_RDX, 4, 0)),
               { 0x03, 0x04, 0x93 });
}
TEST(X64Emit, movzx_r_msib8) {
  EXPECT_BYTES(x64_movzx_r_msib8(&_e, X64_SZ_32, X64_RAX, x64_msib(X64_RCX, X64_RDX, 1, 0)),
               { 0x0F, 0xB6, 0x04, 0x11 });
}
TEST(X64Emit, movsx_r_msib16) {
  EXPECT_BYTES(x64_movsx_r_msib16(&_e, X64_SZ_32, X64_RAX, x64_msib(X64_RCX, X64_RDX, 2, 4)),
               { 0x0F, 0xBF, 0x44, 0x51, 0x04 });
}
TEST(X64Emit, mov_msib_imm32) {
  /* mov dword ptr [rbp + rax*4], 0x12345678 */
  EXPECT_BYTES(x64_mov_msib_imm32(&_e, X64_SZ_32, x64_msib(X64_RBP, X64_RAX, 4, 0), 0x12345678),
               { 0xC7, 0x44, 0x85, 0x00, 0x78, 0x56, 0x34, 0x12 });
}
TEST(X64Emit, call_msib) {
  EXPECT_BYTES(x64_call_msib(&_e, x64_msib(X64_RSI, X64_RAX, 8, 0)),
               { 0xFF, 0x14, 0xC6 });
}

TEST(X64Emit, emit_jump_at_site_basic) {
  /* Patch a 5-byte JMP at &g_buf[0] to target at &g_buf[100].
   * disp = 100 - 5 = 95 = 0x5F */
  memset(g_buf, 0xCC, sizeof(g_buf));
  x64_emit_jump_at_site(&g_buf[0], &g_buf[100], false);
  EXPECT_EQ(g_buf[0], 0xE9);
  EXPECT_EQ(g_buf[1], 0x5F);
  EXPECT_EQ(g_buf[2], 0x00);
  EXPECT_EQ(g_buf[3], 0x00);
  EXPECT_EQ(g_buf[4], 0x00);
}
TEST(X64Emit, call_abs64) {
  /* call <imm64> = MOVABS RAX, imm64 ; CALL RAX  → 48 B8 <8 bytes LE> FF D0 */
  uintptr_t target = 0x123456789ABCDEF0ull;
  EXPECT_BYTES(x64_call_abs64(&_e, (const void*)target),
               { 0x48, 0xB8,
                 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                 0xFF, 0xD0 });
}

TEST(X64Emit, overflow_flag_set) {
  u8 small[2];
  x64_emit_t e;
  x64_emit_init(&e, small, sizeof(small));
  /* mov rax, rcx is 3 bytes; buffer is 2.  Encoder should set overflow. */
  x64_mov_r_r(&e, X64_SZ_64, X64_RAX, X64_RCX);
  EXPECT_TRUE(e.overflow);
}
