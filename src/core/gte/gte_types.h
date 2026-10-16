/*
 * becomes the gte_ prefix; BitField<> unions become raw `bits` words plus inline
 * accessors (see util/cd_image.h for the convention).
 */

#ifndef CUPID_CORE_GTE_TYPES_H
#define CUPID_CORE_GTE_TYPES_H

#include "common/types.h"

enum {
  GTE_NUM_DATA_REGS    = 32,
  GTE_NUM_CONTROL_REGS = 32,
  GTE_NUM_REGS         = GTE_NUM_DATA_REGS + GTE_NUM_CONTROL_REGS,
  GTE_MAX_Z            = 65535,
};

typedef union {
  u32 bits;
} gte_flags_t;

#define GTE_FLAGS_WRITE_MASK   UINT32_C(0xFFFFF000)
#define GTE_FLAGS_ERROR_MASK   UINT32_C(0x7F87E000)

ALWAYS_INLINE void gte_flags_clear(gte_flags_t* f) { f->bits = 0; }

/* error bit = OR of the bits in ERROR_MASK */
ALWAYS_INLINE void gte_flags_update_error(gte_flags_t* f)
{
  if ((f->bits & GTE_FLAGS_ERROR_MASK) != 0)
    f->bits |= (1u << 31);
  else
    f->bits &= ~(1u << 31);
}

ALWAYS_INLINE bool gte_flags_error(gte_flags_t f)             { return (f.bits & (1u << 31)) != 0; }
ALWAYS_INLINE bool gte_flags_mac1_overflow(gte_flags_t f)     { return (f.bits & (1u << 30)) != 0; }
ALWAYS_INLINE bool gte_flags_mac2_overflow(gte_flags_t f)     { return (f.bits & (1u << 29)) != 0; }
ALWAYS_INLINE bool gte_flags_mac3_overflow(gte_flags_t f)     { return (f.bits & (1u << 28)) != 0; }
ALWAYS_INLINE bool gte_flags_mac1_underflow(gte_flags_t f)    { return (f.bits & (1u << 27)) != 0; }
ALWAYS_INLINE bool gte_flags_mac2_underflow(gte_flags_t f)    { return (f.bits & (1u << 26)) != 0; }
ALWAYS_INLINE bool gte_flags_mac3_underflow(gte_flags_t f)    { return (f.bits & (1u << 25)) != 0; }
ALWAYS_INLINE bool gte_flags_ir1_saturated(gte_flags_t f)     { return (f.bits & (1u << 24)) != 0; }
ALWAYS_INLINE bool gte_flags_ir2_saturated(gte_flags_t f)     { return (f.bits & (1u << 23)) != 0; }
ALWAYS_INLINE bool gte_flags_ir3_saturated(gte_flags_t f)     { return (f.bits & (1u << 22)) != 0; }
ALWAYS_INLINE bool gte_flags_color_r_saturated(gte_flags_t f) { return (f.bits & (1u << 21)) != 0; }
ALWAYS_INLINE bool gte_flags_color_g_saturated(gte_flags_t f) { return (f.bits & (1u << 20)) != 0; }
ALWAYS_INLINE bool gte_flags_color_b_saturated(gte_flags_t f) { return (f.bits & (1u << 19)) != 0; }
ALWAYS_INLINE bool gte_flags_sz1_otz_saturated(gte_flags_t f) { return (f.bits & (1u << 18)) != 0; }
ALWAYS_INLINE bool gte_flags_divide_overflow(gte_flags_t f)   { return (f.bits & (1u << 17)) != 0; }
ALWAYS_INLINE bool gte_flags_mac0_overflow(gte_flags_t f)     { return (f.bits & (1u << 16)) != 0; }
ALWAYS_INLINE bool gte_flags_mac0_underflow(gte_flags_t f)    { return (f.bits & (1u << 15)) != 0; }
ALWAYS_INLINE bool gte_flags_sx2_saturated(gte_flags_t f)     { return (f.bits & (1u << 14)) != 0; }
ALWAYS_INLINE bool gte_flags_sy2_saturated(gte_flags_t f)     { return (f.bits & (1u << 13)) != 0; }
ALWAYS_INLINE bool gte_flags_ir0_saturated(gte_flags_t f)     { return (f.bits & (1u << 12)) != 0; }

typedef union {
  struct {
    u32 dr32[GTE_NUM_DATA_REGS];
    u32 cr32[GTE_NUM_CONTROL_REGS];
  };

  u32 r32[GTE_NUM_DATA_REGS + GTE_NUM_CONTROL_REGS];

  struct {
    s16 V0[3];     /* 0-1   */
    u16 pad1;      /* 1     */
    s16 V1[3];     /* 2-3   */
    u16 pad2;      /* 3     */
    s16 V2[3];     /* 4-5   */
    u16 pad3;      /* 5     */
    u8  RGBC[4];   /* 6     */
    u16 OTZ;       /* 7     */
    u16 pad4;      /* 7     */
    s16 IR0;       /* 8     */
    u16 pad5;      /* 8     */
    s16 IR1;       /* 9     */
    u16 pad6;      /* 9     */
    s16 IR2;       /* 10    */
    u16 pad7;      /* 10    */
    s16 IR3;       /* 11    */
    u16 pad8;      /* 11    */
    s16 SXY0[2];   /* 12    */
    s16 SXY1[2];   /* 13    */
    s16 SXY2[2];   /* 14    */
    s16 SXYP[2];   /* 15    */
    u16 SZ0;       /* 16    */
    u16 pad13;     /* 16    */
    u16 SZ1;       /* 17    */
    u16 pad14;     /* 17    */
    u16 SZ2;       /* 18    */
    u16 pad15;     /* 18    */
    u16 SZ3;       /* 19    */
    u16 pad16;     /* 19    */
    u8  RGB0[4];   /* 20    */
    u8  RGB1[4];   /* 21    */
    u8  RGB2[4];   /* 22    */
    u32 RES1;      /* 23    */
    s32 MAC0;      /* 24    */
    s32 MAC1;      /* 25    */
    s32 MAC2;      /* 26    */
    s32 MAC3;      /* 27    */
    u32 IRGB;      /* 28    */
    u32 ORGB;      /* 29    */
    s32 LZCS;      /* 30    */
    u32 LZCR;      /* 31    */
    s16 RT[3][3];  /* 32-36 */
    u16 pad17;     /* 36    */
    s32 TR[3];     /* 37-39 */
    s16 LLM[3][3]; /* 40-44 */
    u16 pad18;     /* 44    */
    s32 BK[3];     /* 45-47 */
    s16 LCM[3][3]; /* 48-52 */
    u16 pad19;     /* 52    */
    s32 FC[3];     /* 53-55 */
    s32 OFX;       /* 56    */
    s32 OFY;       /* 57    */
    u16 H;         /* 58    */
    u16 pad20;     /* 58    */
    s16 DQA;       /* 59    */
    u16 pad21;     /* 59    */
    s32 DQB;       /* 60    */
    s16 ZSF3;      /* 61    */
    u16 pad22;     /* 61    */
    s16 ZSF4;      /* 62    */
    u16 pad23;     /* 62    */
    gte_flags_t FLAG; /* 63 */
  };
} gte_regs_t;

_Static_assert(sizeof(gte_regs_t) == (sizeof(u32) * GTE_NUM_REGS),
               "gte_regs_t must occupy exactly NUM_REGS u32 slots");

typedef union {
  u32 bits;
} gte_instruction_t;

#define GTE_INSTRUCTION_REQUIRED_BITS_MASK ((1u << 20) - 1u)

ALWAYS_INLINE u8   gte_instruction_command(gte_instruction_t i)             { return (u8)(i.bits & 0x3Fu); }
ALWAYS_INLINE bool gte_instruction_lm(gte_instruction_t i)                  { return (i.bits & (1u << 10)) != 0; }
ALWAYS_INLINE u8   gte_instruction_mvmva_translation_vector(gte_instruction_t i) { return (u8)((i.bits >> 13) & 0x3u); }
ALWAYS_INLINE u8   gte_instruction_mvmva_multiply_vector(gte_instruction_t i)    { return (u8)((i.bits >> 15) & 0x3u); }
ALWAYS_INLINE u8   gte_instruction_mvmva_multiply_matrix(gte_instruction_t i)    { return (u8)((i.bits >> 17) & 0x3u); }
ALWAYS_INLINE bool gte_instruction_sf(gte_instruction_t i)                  { return (i.bits & (1u << 19)) != 0; }
ALWAYS_INLINE u8   gte_instruction_fake_command(gte_instruction_t i)        { return (u8)((i.bits >> 20) & 0x1Fu); }

ALWAYS_INLINE u8 gte_instruction_get_shift(gte_instruction_t i)
{
  return gte_instruction_sf(i) ? 12 : 0;
}

#endif /* CUPID_CORE_GTE_TYPES_H */
