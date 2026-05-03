/*
 * GTE (Geometry Transformation Engine) - PSX MIPS COP2.
 *
 * Implements the fixed-point matrix/vector ops the original PS1 hardware
 * uses for 3D vertex transform, lighting and perspective division.
 *
 * Notes:
 *   - The C++ source mirrored 32 data + 32 control registers as a packed
 *     union of typed fields and a flat u32 array.  Same layout is preserved
 *     here: writes via the named members (REGS.IR1, REGS.RT[0][1], ...)
 *     and the flat r32[]/dr32[]/cr32[] views must alias byte-for-byte.
 *   - The FLAG register's bit assignments are the architectural saturation
 *     status word; bit 31 is the OR'd error flag (see gte_flag_update_error).
 *   - The Instruction word's relevant subfields are: sf (shift, bit 19),
 *     mvmva mat/vec/tvec selectors (bits 17..13), lm (bit 10), and the 6-bit
 *     command in bits 0..5.  We decode them at runtime in gte_execute.
 */

#ifndef CUPID_CORE_GTE_H
#define CUPID_CORE_GTE_H

#include "common/types.h"

#include "cpu_types.h"
#include "types.h"

enum {
  GTE_NUM_DATA_REGS = 32u,
  GTE_NUM_CONTROL_REGS = 32u,
  GTE_NUM_REGS = GTE_NUM_DATA_REGS + GTE_NUM_CONTROL_REGS,
  GTE_MAX_Z = 65535u,
};

/* FLAG register (control reg 31, index 63 in the flat r32[] view).
 *
 * Bits 30..23 and 18..13 are OR'd into bit 31 ("error") by
 * gte_flag_update_error().  The write mask is 0xFFFFF000; bits 0..11 are
 * read-only zero (the per-call clear leaves them at zero anyway). */
typedef union {
  u32 bits;
  struct {
    unsigned int : 12;
    unsigned int ir0_saturated : 1;       /* 12 */
    unsigned int sy2_saturated : 1;       /* 13 */
    unsigned int sx2_saturated : 1;       /* 14 */
    unsigned int mac0_underflow : 1;      /* 15 */
    unsigned int mac0_overflow : 1;       /* 16 */
    unsigned int divide_overflow : 1;     /* 17 */
    unsigned int sz1_otz_saturated : 1;   /* 18 */
    unsigned int color_b_saturated : 1;   /* 19 */
    unsigned int color_g_saturated : 1;   /* 20 */
    unsigned int color_r_saturated : 1;   /* 21 */
    unsigned int ir3_saturated : 1;       /* 22 */
    unsigned int ir2_saturated : 1;       /* 23 */
    unsigned int ir1_saturated : 1;       /* 24 */
    unsigned int mac3_underflow : 1;      /* 25 */
    unsigned int mac2_underflow : 1;      /* 26 */
    unsigned int mac1_underflow : 1;      /* 27 */
    unsigned int mac3_overflow : 1;       /* 28 */
    unsigned int mac2_overflow : 1;       /* 29 */
    unsigned int mac1_overflow : 1;       /* 30 */
    unsigned int error : 1;               /* 31 */
  };
} gte_flags_t;

_Static_assert(sizeof(gte_flags_t) == 4, "gte_flags_t must be 32 bits");

#define GTE_FLAG_WRITE_MASK UINT32_C(0xFFFFF000)

/* Register file: 32 data + 32 control regs.
 *
 * The named-member view aliases the flat dr32[]/cr32[]/r32[] views and is
 * the primary access path for the math kernels.  The pad fields are
 * required for layout: the PSX exposes most 16-bit values in the low half
 * of a 32-bit register, with the high half undefined / sign-extended on
 * read.  We allocate the upper half as 'pad' to keep sizeof() exact. */
typedef union {
  struct {
    u32 dr32[GTE_NUM_DATA_REGS];
    u32 cr32[GTE_NUM_CONTROL_REGS];
  };

  u32 r32[GTE_NUM_REGS];

  struct {
    s16 V0[3];     /* 0-1 */
    u16 pad1;      /* 1   */
    s16 V1[3];     /* 2-3 */
    u16 pad2;      /* 3   */
    s16 V2[3];     /* 4-5 */
    u16 pad3;      /* 5   */
    u8 RGBC[4];    /* 6   */
    u16 OTZ;       /* 7   */
    u16 pad4;      /* 7   */
    s16 IR0;       /* 8   */
    u16 pad5;      /* 8   */
    s16 IR1;       /* 9   */
    u16 pad6;      /* 9   */
    s16 IR2;       /* 10  */
    u16 pad7;      /* 10  */
    s16 IR3;       /* 11  */
    u16 pad8;      /* 11  */
    s16 SXY0[2];   /* 12  */
    s16 SXY1[2];   /* 13  */
    s16 SXY2[2];   /* 14  */
    s16 SXYP[2];   /* 15  */
    u16 SZ0;       /* 16  */
    u16 pad13;     /* 16  */
    u16 SZ1;       /* 17  */
    u16 pad14;     /* 17  */
    u16 SZ2;       /* 18  */
    u16 pad15;     /* 18  */
    u16 SZ3;       /* 19  */
    u16 pad16;     /* 19  */
    u8 RGB0[4];    /* 20  */
    u8 RGB1[4];    /* 21  */
    u8 RGB2[4];    /* 22  */
    u32 RES1;      /* 23  */
    s32 MAC0;      /* 24  */
    s32 MAC1;      /* 25  */
    s32 MAC2;      /* 26  */
    s32 MAC3;      /* 27  */
    u32 IRGB;      /* 28  */
    u32 ORGB;      /* 29  */
    s32 LZCS;      /* 30  */
    u32 LZCR;      /* 31  */
    s16 RT[3][3];  /* 32-36 */
    u16 pad17;     /* 36  */
    s32 TR[3];     /* 37-39 */
    s16 LLM[3][3]; /* 40-44 */
    u16 pad18;     /* 44  */
    s32 BK[3];     /* 45-47 */
    s16 LCM[3][3]; /* 48-52 */
    u16 pad19;     /* 52  */
    s32 FC[3];     /* 53-55 */
    s32 OFX;       /* 56  */
    s32 OFY;       /* 57  */
    u16 H;         /* 58  */
    u16 pad20;     /* 58  */
    s16 DQA;       /* 59  */
    u16 pad21;     /* 59  */
    s32 DQB;       /* 60  */
    s16 ZSF3;      /* 61  */
    u16 pad22;     /* 61  */
    s16 ZSF4;      /* 62  */
    u16 pad23;     /* 62  */
    gte_flags_t FLAG; /* 63 */
  };
} gte_registers_t;

_Static_assert(sizeof(gte_registers_t) == sizeof(u32) * GTE_NUM_REGS,
               "gte_registers_t must pack to 64 u32 words");

/* Aspect ratio correction selector (used by Sx perspective in RTPS). */
typedef enum : u8 {
  GTE_ASPECT_RATIO_NONE = 0,
  GTE_ASPECT_RATIO_R16_9,
  GTE_ASPECT_RATIO_R19_9,
  GTE_ASPECT_RATIO_R20_9,
  GTE_ASPECT_RATIO_CUSTOM,
  GTE_ASPECT_RATIO_COUNT,
} gte_aspect_ratio_t;

/* g_gte_regs intentionally NOT exported as a separate global.  GTE state
 * lives inside CPU::g_cpu_state.gte_regs; see gte.c REGS macro. */

typedef struct state_wrapper state_wrapper_t;

void gte_initialize(void);
void gte_reset(void);
bool gte_do_state(state_wrapper_t* sw);

void gte_set_aspect_ratio(gte_aspect_ratio_t aspect, u32 custom_numerator,
                          u32 custom_denominator);

/* Control regs are at index +32 in the flat 64-entry view.  These wrap the
 * sign-extension / FIFO-push / read-only-mirror quirks of the data path. */
u32 gte_read_register(u32 index);
void gte_write_register(u32 index, u32 value);

/* Direct pointer for the cpu_core MFC2/MTC2 fast paths. */
u32* gte_get_register_ptr(u32 index);

/* Decode and execute a single GTE instruction word.  Per the porting plan
 * we resolve the sf/mx/v/cv/lm flags at runtime here rather than via the
 * C++ template specialization the original used. */
void gte_execute(u32 inst_bits);

/* cpu_core.c-facing variant: the dispatcher hands us the decoded MIPS word
 * as a cpu_instruction_t; we just need its raw bits. */
void gte_execute_instruction(cpu_instruction_t inst);

/* Lookup the cycle cost the cpu_core uses to advance its tick counter. */
s32 gte_get_instruction_cycles(u32 inst_bits);

const u32* gte_dbg_op_counts(void);

#endif /* CUPID_CORE_GTE_H */
