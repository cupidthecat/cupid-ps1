/*
 * Hardware MDEC.  Two MMIO registers exposed at 0x1F801820 (data/command)
 * and 0x1F801824 (status / control), plus DMA channels 0 (in) and 1 (out).
 *
 * Pipeline:
 *   DMA0 word stream -> 16-bit halfword input FIFO (1024 bytes / 512 hw)
 *     -> RLE-decode + dequantize using iq_y / iq_uv tables
 *     -> per-block IDCT (8x8) using the host-provided scale table
 *     -> for colour macroblocks, 4 Y blocks + Cb + Cr -> 16x16 RGB-888
 *     -> pack to 4/8/15/24-bit -> 768-byte output FIFO -> DMA1.
 *
 * Two YUV/IDCT paths: a scalar "old" path (toggled by
 * g_settings.mdec_use_old_routines) and a "new" path written in plain C
 * with no SIMD intrinsics; semantics are identical, performance is left
 * to the compiler's auto-vectoriser.
 *
 * Cross-module dependencies:
 *   cpu_core_add_pending_ticks ; stalls CPU when the host reads the data
 *                                  register before a block has finished.
 *   dma_set_request            ; raises DREQ on channel 0 / 1.
 *   cdrom_disable_read_speedup ; MDEC start-of-stream tells CDROM to
 *                                  stop running at max read rate so the
 *                                  FIFOs don't overrun.
 */

#include "core/mdec.h"

#include "core/bus.h"
#include "core/cdrom.h"
#include "core/fmv_dump.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/intrin.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(MDEC);

typedef enum {
  MDEC_DMA_CHANNEL_MDEC_IN  = 0,
  MDEC_DMA_CHANNEL_MDEC_OUT = 1,
} mdec_dma_channel_t;

extern void         dma_set_request(u32 channel, bool request);
extern void         cpu_core_add_pending_ticks(tick_count_t ticks);

enum {
  MDEC_DATA_IN_FIFO_SIZE_BYTES  = 1024,
  MDEC_DATA_OUT_FIFO_SIZE_BYTES = 768,
  /* Halfword-wide input FIFO and word-wide output FIFO. */
  MDEC_DATA_IN_FIFO_CAPACITY  = MDEC_DATA_IN_FIFO_SIZE_BYTES  / (u32)sizeof(u16),
  MDEC_DATA_OUT_FIFO_CAPACITY = MDEC_DATA_OUT_FIFO_SIZE_BYTES / (u32)sizeof(u32),

  MDEC_NUM_BLOCKS = 6,
  MDEC_TICKS_PER_BLOCK = 448,
  MDEC_ACTIVE_FRAME_COUNT = 30,
};

typedef enum : u8 {
  MDEC_DATA_OUTPUT_DEPTH_4BIT  = 0,
  MDEC_DATA_OUTPUT_DEPTH_8BIT  = 1,
  MDEC_DATA_OUTPUT_DEPTH_24BIT = 2,
  MDEC_DATA_OUTPUT_DEPTH_15BIT = 3,
} mdec_data_output_depth_t;

typedef enum : u8 {
  MDEC_COMMAND_NONE              = 0,
  MDEC_COMMAND_DECODE_MACROBLOCK = 1,
  MDEC_COMMAND_SET_IQ_TAB        = 2,
  MDEC_COMMAND_SET_SCALE         = 3,
} mdec_command_t;

typedef enum : u8 {
  MDEC_STATE_IDLE,
  MDEC_STATE_DECODING_MACROBLOCK,
  MDEC_STATE_WRITING_MACROBLOCK,
  MDEC_STATE_SET_IQ_TABLE,
  MDEC_STATE_SET_SCALE_TABLE,
  MDEC_STATE_NO_COMMAND,
} mdec_state_e;

/* BitField<u32, ..., shift, width> spec exactly so the underlying u32 is
 * wire-compatible with save states and MMIO. */
typedef union {
  u32 raw;
  struct {
    u32 parameter_words_remaining : 16;
    u32 current_block             : 3;
    u32 _reserved19               : 4;
    u32 data_output_bit15         : 1;
    u32 data_output_signed        : 1;
    u32 data_output_depth         : 2;
    u32 data_out_request          : 1;
    u32 data_in_request           : 1;
    u32 command_busy              : 1;
    u32 data_in_fifo_full         : 1;
    u32 data_out_fifo_empty       : 1;
  } bits;
} mdec_status_register_t;

typedef union {
  u32 raw;
  struct {
    u32 _reserved0      : 29;
    u32 enable_dma_out  : 1;
    u32 enable_dma_in   : 1;
    u32 reset           : 1;
  } bits;
} mdec_control_register_t;

typedef union {
  u32 raw;
  struct {
    u32 parameter_word_count : 16;
    u32 _reserved16          : 9;
    u32 data_output_bit15    : 1;
    u32 data_output_signed   : 1;
    u32 data_output_depth    : 2;
    u32 command              : 3;
  } bits;
} mdec_command_word_t;

typedef struct {
  u16 buf[MDEC_DATA_IN_FIFO_CAPACITY];
  u32 head; /* next pop */
  u32 tail; /* next push */
  u32 size;
} mdec_fifo_in_t;

typedef struct {
  u32 buf[MDEC_DATA_OUT_FIFO_CAPACITY];
  u32 head;
  u32 tail;
  u32 size;
} mdec_fifo_out_t;

static ALWAYS_INLINE void mdec_fifo_in_clear (mdec_fifo_in_t*  f) { f->head = f->tail = f->size = 0; }
static ALWAYS_INLINE void mdec_fifo_out_clear(mdec_fifo_out_t* f) { f->head = f->tail = f->size = 0; }
static ALWAYS_INLINE u32  mdec_fifo_in_size  (const mdec_fifo_in_t*  f) { return f->size; }
static ALWAYS_INLINE u32  mdec_fifo_out_size (const mdec_fifo_out_t* f) { return f->size; }
static ALWAYS_INLINE u32  mdec_fifo_in_space (const mdec_fifo_in_t*  f) { return MDEC_DATA_IN_FIFO_CAPACITY  - f->size; }
static ALWAYS_INLINE bool mdec_fifo_in_empty (const mdec_fifo_in_t*  f) { return f->size == 0; }
static ALWAYS_INLINE bool mdec_fifo_out_empty(const mdec_fifo_out_t* f) { return f->size == 0; }
static ALWAYS_INLINE bool mdec_fifo_in_full  (const mdec_fifo_in_t*  f) { return f->size == MDEC_DATA_IN_FIFO_CAPACITY; }

static ALWAYS_INLINE void mdec_fifo_in_push(mdec_fifo_in_t* f, u16 v)
{
  f->buf[f->tail] = v;
  f->tail = (f->tail + 1u) & (MDEC_DATA_IN_FIFO_CAPACITY - 1u);
  f->size++;
}
static ALWAYS_INLINE u16 mdec_fifo_in_pop(mdec_fifo_in_t* f)
{
  const u16 v = f->buf[f->head];
  f->head = (f->head + 1u) & (MDEC_DATA_IN_FIFO_CAPACITY - 1u);
  f->size--;
  return v;
}
static ALWAYS_INLINE u16 mdec_fifo_in_peek(const mdec_fifo_in_t* f, u32 i)
{
  return f->buf[(f->head + i) & (MDEC_DATA_IN_FIFO_CAPACITY - 1u)];
}
static ALWAYS_INLINE void mdec_fifo_in_remove(mdec_fifo_in_t* f, u32 count)
{
  f->head = (f->head + count) & (MDEC_DATA_IN_FIFO_CAPACITY - 1u);
  f->size -= count;
}
static void mdec_fifo_in_push_range(mdec_fifo_in_t* f, const u16* values, u32 count)
{
  for (u32 i = 0; i < count; i++)
    mdec_fifo_in_push(f, values[i]);
}
static void mdec_fifo_in_pop_range(mdec_fifo_in_t* f, u16* out, u32 count)
{
  for (u32 i = 0; i < count; i++)
    out[i] = mdec_fifo_in_pop(f);
}

static ALWAYS_INLINE void mdec_fifo_out_push(mdec_fifo_out_t* f, u32 v)
{
  /* MDEC out FIFO holds 192 u32s; not power of two, so wrap with '%'
   * not '& (cap-1)'. The latter aliases tails 64..127 onto 0..63 and
   * silently drops two-thirds of every macroblock, surfacing as glitched
   */
  f->buf[f->tail] = v;
  f->tail = (f->tail + 1u) % MDEC_DATA_OUT_FIFO_CAPACITY;
  f->size++;
}
static ALWAYS_INLINE u32 mdec_fifo_out_pop(mdec_fifo_out_t* f)
{
  const u32 v = f->buf[f->head];
  f->head = (f->head + 1u) % MDEC_DATA_OUT_FIFO_CAPACITY;
  f->size--;
  return v;
}
static void mdec_fifo_out_pop_range(mdec_fifo_out_t* f, u32* out, u32 count)
{
  for (u32 i = 0; i < count; i++)
    out[i] = mdec_fifo_out_pop(f);
}

 /*
 * fixed-size POD; we mirror that on disk so loaders interoperate.  Layout:
 * head (u32), tail (u32), size (u32), capacity (u32), then the raw buffer.
 *
 * NOTE: MDEC_DATA_IN_FIFO_CAPACITY is power-of-two so the ring indices fit
 * in u32 trivially. */
static void mdec_fifo_in_do_state(state_wrapper_t* sw, mdec_fifo_in_t* f)
{
  state_wrapper_do_array(sw, &f->head, sizeof(u32), 1);
  state_wrapper_do_array(sw, &f->tail, sizeof(u32), 1);
  state_wrapper_do_array(sw, &f->size, sizeof(u32), 1);
  state_wrapper_do_array(sw, f->buf, sizeof(u16), MDEC_DATA_IN_FIFO_CAPACITY);
}
static void mdec_fifo_out_do_state(state_wrapper_t* sw, mdec_fifo_out_t* f)
{
  state_wrapper_do_array(sw, &f->head, sizeof(u32), 1);
  state_wrapper_do_array(sw, &f->tail, sizeof(u32), 1);
  state_wrapper_do_array(sw, &f->size, sizeof(u32), 1);
  state_wrapper_do_array(sw, f->buf, sizeof(u32), MDEC_DATA_OUT_FIFO_CAPACITY);
}

typedef struct {
  mdec_status_register_t status;
  bool         enable_dma_in;
  bool         enable_dma_out;
  mdec_state_e state;
  u8           active_frame_count;
  u32          remaining_halfwords;

  mdec_fifo_in_t  data_in_fifo;
  mdec_fifo_out_t data_out_fifo;

  u8 iq_uv[64];
  u8 iq_y [64];

  ALIGN_TO_CACHE_LINE s16 scale_table[64];

  /* For colour macroblocks: 0=Crblk, 1=Cbblk, 2..5=Y1..Y4. */
  ALIGN_TO_CACHE_LINE s16 blocks[MDEC_NUM_BLOCKS][64];
  u32 current_block;
  u32 current_coefficient; /* k-in-block; 64 == "next halfword starts a block" */
  u16 current_q_scale;

  ALIGN_TO_CACHE_LINE u32 block_rgb[256];
  timing_event_t block_copy_out_event;

#if !defined(NDEBUG)
  u32 total_blocks_decoded;
#endif
} mdec_state_t;

static ALIGN_TO_CACHE_LINE mdec_state_t s_state;

static bool mdec_has_pending_block_copy_out(void);
static void mdec_soft_reset(void);
static void mdec_reset_decoder(void);
static void mdec_update_status(void);
static u32  mdec_read_data_register(void);
static void mdec_write_command_register(u32 value);
static void mdec_execute(void);
static bool mdec_handle_decode_macroblock_command(void);
static void mdec_handle_set_quant_table_command(void);
static void mdec_handle_set_scale_command(void);
static void mdec_set_scale_matrix(const u16* values);
static bool mdec_decode_mono_macroblock(void);
static bool mdec_decode_colored_macroblock(void);
static void mdec_schedule_block_copy_out(tick_count_t ticks);
static void mdec_copy_out_block(void* param, tick_count_t ticks, tick_count_t ticks_late);
static bool mdec_decode_rle_old(s16* blk, const u8* qt);
static void mdec_idct_old(s16* blk);
static void mdec_yuv_to_rgb_old(u32 xx, u32 yy, const s16* Crblk, const s16* Cbblk, const s16* Yblk);
static bool mdec_decode_rle_new(s16* blk, const u8* qt);
static void mdec_idct_new(s16* blk);
static void mdec_yuv_to_rgb_new(u32 xx, u32 yy, const s16* Crblk, const s16* Cbblk, const s16* Yblk);
static void mdec_yuv_to_mono(const s16* Yblk);

static ALWAYS_INLINE s32 mdec_clamp_s32(s32 v, s32 lo, s32 hi)
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}
static ALWAYS_INLINE s32 mdec_min_s32(s32 a, s32 b) { return (a < b) ? a : b; }
static ALWAYS_INLINE u32 mdec_min_u32(u32 a, u32 b) { return (a < b) ? a : b; }

void mdec_initialize(void)
{
#if !defined(NDEBUG)
  s_state.total_blocks_decoded = 0;
#endif
  s_state.active_frame_count = 0;
  timing_event_init(&s_state.block_copy_out_event,
                    "MDEC Block Copy Out", (u32)sizeof("MDEC Block Copy Out") - 1u,
                    1, 1, &mdec_copy_out_block, NULL);
  mdec_reset();
}

void mdec_shutdown(void)
{
  timing_event_deactivate(&s_state.block_copy_out_event);
  timing_event_destroy(&s_state.block_copy_out_event);
}

void mdec_reset(void)
{
  s_state.active_frame_count = 0;
  timing_event_deactivate(&s_state.block_copy_out_event);
  mdec_soft_reset();
}

bool mdec_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_u32 (sw, &s_state.status.raw);
  state_wrapper_do_bool(sw, &s_state.enable_dma_in);
  state_wrapper_do_bool(sw, &s_state.enable_dma_out);
  mdec_fifo_in_do_state (sw, &s_state.data_in_fifo);
  mdec_fifo_out_do_state(sw, &s_state.data_out_fifo);
  state_wrapper_do_array(sw, &s_state.state, sizeof(s_state.state), 1);
  state_wrapper_do_u32  (sw, &s_state.remaining_halfwords);
  state_wrapper_do_array(sw, s_state.iq_uv, sizeof(u8), 64);
  state_wrapper_do_array(sw, s_state.iq_y,  sizeof(u8), 64);

  /* Save-state version 66 widened the scale matrix from u16[64] (raw IDCT
   * coefficients) to s16[64] (post-transpose into row-major).  Older blobs
   * are upconverted here via the same SetScaleMatrix the SetScale command
   * uses at runtime. */
  if (state_wrapper_get_version(sw) < 66) {
    u16 old_scale_matrix[64];
    state_wrapper_do_array(sw, old_scale_matrix, sizeof(u16), 64);
    mdec_set_scale_matrix(old_scale_matrix);
  } else {
    state_wrapper_do_array(sw, s_state.scale_table, sizeof(s16), 64);
  }

  state_wrapper_do_array(sw, s_state.blocks, sizeof(s16), MDEC_NUM_BLOCKS * 64);
  state_wrapper_do_u32  (sw, &s_state.current_block);
  state_wrapper_do_u32  (sw, &s_state.current_coefficient);
  state_wrapper_do_u16  (sw, &s_state.current_q_scale);
  state_wrapper_do_array(sw, s_state.block_rgb, sizeof(u32), 256);

  bool block_copy_out_pending = mdec_has_pending_block_copy_out();
  state_wrapper_do_bool(sw, &block_copy_out_pending);
  if (state_wrapper_is_reading(sw)) {
    timing_event_set_state(&s_state.block_copy_out_event, block_copy_out_pending);
    s_state.active_frame_count = 0;
  }

  return !state_wrapper_has_error(sw);
}

bool mdec_is_active(void)              { return s_state.active_frame_count > 0; }
bool mdec_is_decoding_macroblock(void) { return s_state.state == MDEC_STATE_DECODING_MACROBLOCK; }

void mdec_end_frame(void)
{
  s_state.active_frame_count = (s_state.active_frame_count > 0) ? (u8)(s_state.active_frame_count - 1) : (u8)0;
}

u32 mdec_read_register(u32 offset)
{
  switch (offset) {
    case 0:
      return mdec_read_data_register();

    case 4:
      TRACE_LOG("MDEC status register -> 0x%08X", s_state.status.raw);
      return s_state.status.raw;

    default:
      ERROR_LOG("Unknown MDEC register read: 0x%08X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void mdec_write_register(u32 offset, u32 value)
{
  switch (offset) {
    case 0:
      mdec_write_command_register(value);
      return;

    case 4: {
      DEBUG_LOG("MDEC control register <- 0x%08X", value);
      const mdec_control_register_t cr = { .raw = value };
      if (cr.bits.reset)
        mdec_soft_reset();
      s_state.enable_dma_in  = (bool)cr.bits.enable_dma_in;
      s_state.enable_dma_out = (bool)cr.bits.enable_dma_out;
      mdec_execute();
      return;
    }

    default:
      ERROR_LOG("Unknown MDEC register write: 0x%08X <- 0x%08X", offset, value);
      return;
  }
}

void mdec_dma_read(u32* words, u32 word_count)
{
  if (mdec_fifo_out_size(&s_state.data_out_fifo) < word_count)
    WARNING_LOG("Insufficient data in output FIFO (requested %u, have %u)",
                word_count, mdec_fifo_out_size(&s_state.data_out_fifo));

  const u32 words_to_read = mdec_min_u32(word_count, mdec_fifo_out_size(&s_state.data_out_fifo));
  if (words_to_read > 0)
    mdec_fifo_out_pop_range(&s_state.data_out_fifo, words, words_to_read);
  FMV_DUMP("T4", words, words_to_read * 4u, ((u64)word_count << 32) | words_to_read);

  DEBUG_LOG("DMA read complete, %u bytes left", mdec_fifo_out_size(&s_state.data_out_fifo) * (u32)sizeof(u32));
  if (mdec_fifo_out_empty(&s_state.data_out_fifo))
    mdec_execute();
}

void mdec_dma_write(const u32* words, u32 word_count)
{
  if (mdec_fifo_in_space(&s_state.data_in_fifo) < (word_count * 2u))
    WARNING_LOG("Input FIFO overflow (writing %u, space %u)",
                word_count * 2u, mdec_fifo_in_space(&s_state.data_in_fifo));

  /* Mask off bit 1 to keep the count even; pairs of halfwords were
   * unpacked from a single u32, so we either accept both or drop both. */
  const u32 halfwords_to_write = mdec_min_u32(word_count * 2u,
                                              mdec_fifo_in_space(&s_state.data_in_fifo) & ~(u32)2u);
  FMV_DUMP("T1", words, word_count * 4u, ((u64)halfwords_to_write << 32) | word_count);
  mdec_fifo_in_push_range(&s_state.data_in_fifo, (const u16*)words, halfwords_to_write);
  mdec_execute();
}

u32 mdec_dbg_status(void)
{
  return s_state.status.raw;
}

u32 mdec_dbg_fifo_status(void)
{
  return ((u32)s_state.state << 24) |
         ((s_state.enable_dma_in ? 1u : 0u) << 23) |
         ((s_state.enable_dma_out ? 1u : 0u) << 22) |
         ((mdec_fifo_in_size(&s_state.data_in_fifo) & 0x7FFu) << 11) | 
         (mdec_fifo_out_size(&s_state.data_out_fifo) & 0x7FFu);
}

static bool mdec_has_pending_block_copy_out(void)
{
  return timing_event_is_active(&s_state.block_copy_out_event);
}

static void mdec_soft_reset(void)
{
  s_state.status.raw = 0;
  s_state.enable_dma_in  = false;
  s_state.enable_dma_out = false;
  mdec_fifo_in_clear (&s_state.data_in_fifo);
  mdec_fifo_out_clear(&s_state.data_out_fifo);
  s_state.state = MDEC_STATE_IDLE;
  s_state.remaining_halfwords = 0;
  s_state.current_block = 0;
  s_state.current_coefficient = 64;
  s_state.current_q_scale = 0;
  timing_event_deactivate(&s_state.block_copy_out_event);
  mdec_update_status();
}

static void mdec_reset_decoder(void)
{
  s_state.current_block = 0;
  s_state.current_coefficient = 64;
  s_state.current_q_scale = 0;
}

static void mdec_update_status(void)
{
  s_state.status.bits.data_out_fifo_empty = mdec_fifo_out_empty(&s_state.data_out_fifo) ? 1u : 0u;
  s_state.status.bits.data_in_fifo_full   = mdec_fifo_in_full  (&s_state.data_in_fifo)  ? 1u : 0u;
  s_state.status.bits.command_busy        = (s_state.state != MDEC_STATE_IDLE) ? 1u : 0u;
  /* parameter_words_remaining is biased by -1 in the hardware register. */
  s_state.status.bits.parameter_words_remaining = (u32)Truncate16((s_state.remaining_halfwords / 2u) - 1u);
  s_state.status.bits.current_block = (s_state.current_block + 4u) % MDEC_NUM_BLOCKS;

  /* Always request data-in if input is enabled and we have room for a
   * burst of 32 words (64 halfwords). */
  const bool data_in_request = s_state.enable_dma_in && mdec_fifo_in_space(&s_state.data_in_fifo) >= (32u * 2u);
  s_state.status.bits.data_in_request = data_in_request ? 1u : 0u;
  dma_set_request((u32)MDEC_DMA_CHANNEL_MDEC_IN, data_in_request);

  /* Only request data-out if we actually have something queued. */
  const bool data_out_request = s_state.enable_dma_out && !mdec_fifo_out_empty(&s_state.data_out_fifo);
  s_state.status.bits.data_out_request = data_out_request ? 1u : 0u;
  dma_set_request((u32)MDEC_DMA_CHANNEL_MDEC_OUT, data_out_request);
}

static u32 mdec_read_data_register(void)
{
  if (mdec_fifo_out_empty(&s_state.data_out_fifo)) {
    /* Stall the CPU until the in-flight block lands, mirroring real HW
     * which simply doesn't ack the read until DMAout has drained. */
    if (mdec_has_pending_block_copy_out()) {
      DEV_LOG("MDEC data out FIFO empty on read - stalling CPU");
      cpu_core_add_pending_ticks(timing_event_get_ticks_until_next_execution(&s_state.block_copy_out_event));
    } else {
      WARNING_LOG("MDEC data out FIFO empty on read and no data processing");
      return UINT32_C(0xFFFFFFFF);
    }
  }

  const u32 value = mdec_fifo_out_pop(&s_state.data_out_fifo);
  if (mdec_fifo_out_empty(&s_state.data_out_fifo))
    mdec_execute();
  else
    mdec_update_status();
  return value;
}

static void mdec_write_command_register(u32 value)
{
  TRACE_LOG("MDEC command/data register <- 0x%08X", value);
  mdec_fifo_in_push(&s_state.data_in_fifo, Truncate16(value));
  mdec_fifo_in_push(&s_state.data_in_fifo, Truncate16(value >> 16));
  mdec_execute();
}

static void mdec_execute(void)
{
  /* First sign of activity (re)triggers the CDROM speedup interlock. */
  const u8 prev_active = s_state.active_frame_count;
  s_state.active_frame_count = MDEC_ACTIVE_FRAME_COUNT;
  if (prev_active == 0) {
    if (g_settings.mdec_disable_cdrom_speedup)
      cdrom_disable_read_speedup();
  }

  for (;;) {
    switch (s_state.state) {
      case MDEC_STATE_IDLE: {
        if (mdec_fifo_in_size(&s_state.data_in_fifo) < 2)
          goto finished;

        /* Header word: assemble from two halfwords (little-endian on disk). */
        const u32 raw = ZeroExtend32_u16(mdec_fifo_in_peek(&s_state.data_in_fifo, 0)) |
                        (ZeroExtend32_u16(mdec_fifo_in_peek(&s_state.data_in_fifo, 1)) << 16);
        const mdec_command_word_t cw = { .raw = raw };
        s_state.status.bits.data_output_depth  = cw.bits.data_output_depth;
        s_state.status.bits.data_output_signed = cw.bits.data_output_signed;
        s_state.status.bits.data_output_bit15  = cw.bits.data_output_bit15;
        mdec_fifo_in_remove(&s_state.data_in_fifo, 2);
        mdec_fifo_out_clear(&s_state.data_out_fifo);

        u32 num_words;
        mdec_state_e new_state;
        switch ((mdec_command_t)cw.bits.command) {
          case MDEC_COMMAND_DECODE_MACROBLOCK:
            num_words = ZeroExtend32(cw.bits.parameter_word_count);
            new_state = MDEC_STATE_DECODING_MACROBLOCK;
            break;

          case MDEC_COMMAND_SET_IQ_TAB:
            /* Bit 0 of the header chooses chroma table presence. */
            num_words = 16u + (((cw.raw & 1u) != 0u) ? 16u : 0u);
            new_state = MDEC_STATE_SET_IQ_TABLE;
            break;

          case MDEC_COMMAND_SET_SCALE:
            num_words = 32u;
            new_state = MDEC_STATE_SET_SCALE_TABLE;
            break;

          default:
            DEV_LOG("Invalid MDEC command 0x%08X", cw.raw);
            num_words = (u32)cw.bits.parameter_word_count;
            new_state = MDEC_STATE_NO_COMMAND;
            break;
        }

        DEBUG_LOG("MDEC command: 0x%08X (%u, %u words in parameter, %u expected)", cw.raw,
                  (u32)cw.bits.command, (u32)cw.bits.parameter_word_count, num_words);

        s_state.remaining_halfwords = num_words * 2u;
        s_state.state = new_state;
        mdec_update_status();
        continue;
      }

      case MDEC_STATE_DECODING_MACROBLOCK: {
        if (mdec_handle_decode_macroblock_command()) {
          /* The decoder transitions to WritingMacroblock and arms the
           * copy-out event; nothing more to do until that fires. */
          DebugAssert(s_state.state == MDEC_STATE_WRITING_MACROBLOCK);
          goto finished;
        }
        if (s_state.remaining_halfwords == 0 && s_state.current_block != MDEC_NUM_BLOCKS) {
          /* Expected more data but the FIFO is empty for good. */
          mdec_reset_decoder();
          s_state.state = MDEC_STATE_IDLE;
          continue;
        }
        goto finished;
      }

      case MDEC_STATE_WRITING_MACROBLOCK:
        /* Driven by the timing event. */
        goto finished;

      case MDEC_STATE_SET_IQ_TABLE:
        if (mdec_fifo_in_size(&s_state.data_in_fifo) < s_state.remaining_halfwords)
          goto finished;
        mdec_handle_set_quant_table_command();
        s_state.state = MDEC_STATE_IDLE;
        mdec_update_status();
        continue;

      case MDEC_STATE_SET_SCALE_TABLE:
        if (mdec_fifo_in_size(&s_state.data_in_fifo) < s_state.remaining_halfwords)
          goto finished;
        mdec_handle_set_scale_command();
        s_state.state = MDEC_STATE_IDLE;
        mdec_update_status();
        continue;

      case MDEC_STATE_NO_COMMAND: {
        /* Eat junk halfwords as they come; can be many. */
        const u32 to_consume = mdec_min_u32(s_state.remaining_halfwords,
                                            mdec_fifo_in_size(&s_state.data_in_fifo));
        mdec_fifo_in_remove(&s_state.data_in_fifo, to_consume);
        s_state.remaining_halfwords -= to_consume;
        if (s_state.remaining_halfwords == 0)
          goto finished;
        s_state.state = MDEC_STATE_IDLE;
        mdec_update_status();
        continue;
      }

      default:
        UnreachableCode();
        return;
    }
  }

finished:
  mdec_update_status();
}

static bool mdec_handle_decode_macroblock_command(void)
{
  if (s_state.status.bits.data_output_depth <= MDEC_DATA_OUTPUT_DEPTH_8BIT)
    return mdec_decode_mono_macroblock();
  return mdec_decode_colored_macroblock();
}

static bool mdec_decode_mono_macroblock(void)
{
  /* Don't trample a copy-out that hasn't drained yet. */
  if (!mdec_fifo_out_empty(&s_state.data_out_fifo))
    return false;

  if (g_settings.mdec_use_old_routines) {
    if (!mdec_decode_rle_old(s_state.blocks[0], s_state.iq_y))
      return false;
    mdec_idct_old(s_state.blocks[0]);
  } else {
    if (!mdec_decode_rle_new(s_state.blocks[0], s_state.iq_y))
      return false;
    mdec_idct_new(s_state.blocks[0]);
  }

  DEBUG_LOG("Decoded mono macroblock, %u words remaining", s_state.remaining_halfwords / 2u);
  mdec_reset_decoder();
  s_state.state = MDEC_STATE_WRITING_MACROBLOCK;

  mdec_yuv_to_mono(s_state.blocks[0]);
  mdec_schedule_block_copy_out(MDEC_TICKS_PER_BLOCK * 6);

#if !defined(NDEBUG)
  s_state.total_blocks_decoded++;
#endif
  return true;
}

static bool mdec_decode_colored_macroblock(void)
{
  if (g_settings.mdec_use_old_routines) {
    for (; s_state.current_block < MDEC_NUM_BLOCKS; s_state.current_block++) {
      if (!mdec_decode_rle_old(s_state.blocks[s_state.current_block],
                               (s_state.current_block >= 2) ? s_state.iq_y : s_state.iq_uv))
        return false;
      mdec_idct_old(s_state.blocks[s_state.current_block]);
    }
    if (!mdec_fifo_out_empty(&s_state.data_out_fifo))
      return false;

    DEBUG_LOG("Decoded colored macroblock, %u words remaining", s_state.remaining_halfwords / 2u);
    mdec_reset_decoder();
    s_state.state = MDEC_STATE_WRITING_MACROBLOCK;

    mdec_yuv_to_rgb_old(0, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[2]);
    mdec_yuv_to_rgb_old(8, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[3]);
    mdec_yuv_to_rgb_old(0, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[4]);
    mdec_yuv_to_rgb_old(8, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[5]);
  } else {
    for (; s_state.current_block < MDEC_NUM_BLOCKS; s_state.current_block++) {
      if (!mdec_decode_rle_new(s_state.blocks[s_state.current_block],
                               (s_state.current_block >= 2) ? s_state.iq_y : s_state.iq_uv))
        return false;
      mdec_idct_new(s_state.blocks[s_state.current_block]);
    }
    if (!mdec_fifo_out_empty(&s_state.data_out_fifo))
      return false;

    DEBUG_LOG("Decoded colored macroblock, %u words remaining", s_state.remaining_halfwords / 2u);
    mdec_reset_decoder();
    s_state.state = MDEC_STATE_WRITING_MACROBLOCK;

    mdec_yuv_to_rgb_new(0, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[2]);
    mdec_yuv_to_rgb_new(8, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[3]);
    mdec_yuv_to_rgb_new(0, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[4]);
    mdec_yuv_to_rgb_new(8, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[5]);
  }

#if !defined(NDEBUG)
  s_state.total_blocks_decoded += 4;
#endif

  mdec_schedule_block_copy_out(MDEC_TICKS_PER_BLOCK * 6);
  return true;
}

static void mdec_schedule_block_copy_out(tick_count_t ticks)
{
  DebugAssert(!mdec_has_pending_block_copy_out());
  DEBUG_LOG("Scheduling block copy out in %d ticks", (int)ticks);
  timing_event_set_interval_and_schedule(&s_state.block_copy_out_event, ticks);
}

static void mdec_copy_out_block(void* param, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)param; (void)ticks; (void)ticks_late;
  Assert(s_state.state == MDEC_STATE_WRITING_MACROBLOCK);
  timing_event_deactivate(&s_state.block_copy_out_event);

  switch ((mdec_data_output_depth_t)s_state.status.bits.data_output_depth) {
    /* Not worth vectorising 4-bit / 8-bit modes; basically never used. */
    case MDEC_DATA_OUTPUT_DEPTH_4BIT: {
      const u32* in_ptr = s_state.block_rgb;
      for (u32 i = 0; i < (64u / 8u); i++) {
        u32 value = *(in_ptr++) >> 4;
        value |= (*(in_ptr++) >> 4) << 4;
        value |= (*(in_ptr++) >> 4) << 8;
        value |= (*(in_ptr++) >> 4) << 12;
        value |= (*(in_ptr++) >> 4) << 16;
        value |= (*(in_ptr++) >> 4) << 20;
        value |= (*(in_ptr++) >> 4) << 24;
        value |= (*(in_ptr++) >> 4) << 28;
        mdec_fifo_out_push(&s_state.data_out_fifo, value);
      }
      break;
    }

    case MDEC_DATA_OUTPUT_DEPTH_8BIT: {
      const u32* in_ptr = s_state.block_rgb;
      for (u32 i = 0; i < (64u / 4u); i++) {
        u32 value = *in_ptr++;
        value |= *in_ptr++ << 8;
        value |= *in_ptr++ << 16;
        value |= *in_ptr++ << 24;
        mdec_fifo_out_push(&s_state.data_out_fifo, value);
      }
      break;
    }

    case MDEC_DATA_OUTPUT_DEPTH_24BIT: {
      /* Pack tightly: each block_rgb entry is RGB- (low byte = R, high
       * byte = unused / Bit15), and DMAout receives it as a stream of
       * order in the FIFO: R first, then G, then B, then R of next pixel.
       * The 4-state machine below threads three RGB triplets into four
       * output u32s, hence the bit-shuffling. */
      u32 index = 0;
      u32 state = 0;
      u32 rgb = 0;
      while (index < 256u) {
        switch (state) {
          case 0:
            rgb = s_state.block_rgb[index++]; /* RGB- */
            state = 1;
            break;
          case 1:
            rgb |= (s_state.block_rgb[index] & 0xFFu) << 24; /* RGBR */
            mdec_fifo_out_push(&s_state.data_out_fifo, rgb);
            rgb = s_state.block_rgb[index] >> 8; /* GB-- */
            index++;
            state = 2;
            break;
          case 2:
            rgb |= s_state.block_rgb[index] << 16; /* GBRG */
            mdec_fifo_out_push(&s_state.data_out_fifo, rgb);
            rgb = s_state.block_rgb[index] >> 16;
            index++;
            state = 3;
            break;
          case 3:
            rgb |= s_state.block_rgb[index] << 8; /* BRGB */
            mdec_fifo_out_push(&s_state.data_out_fifo, rgb);
            index++;
            state = 0;
            break;
          default: break;
        }
      }
      break;
    }

    case MDEC_DATA_OUTPUT_DEPTH_15BIT: {
      if (g_settings.mdec_use_old_routines) {
        /* Old path: bit positions extracted at >>3 windows; matches
         * Mednafen's pre-2018 reference decoder. */
        const u16 a = (u16)((u16)s_state.status.bits.data_output_bit15 << 15);
        for (u32 i = 0; i < 256u;) {
          u32 color = s_state.block_rgb[i++];
          u16 r = (u16)((color >>  3) & 0x1Fu);
          u16 g = (u16)((color >> 11) & 0x1Fu);
          u16 b = (u16)((color >> 19) & 0x1Fu);
          const u16 color15a = (u16)(r | (g << 5) | (b << 10) | a);

          color = s_state.block_rgb[i++];
          r = (u16)((color >>  3) & 0x1Fu);
          g = (u16)((color >> 11) & 0x1Fu);
          b = (u16)((color >> 19) & 0x1Fu);
          const u16 color15b = (u16)(r | (g << 5) | (b << 10) | a);

          mdec_fifo_out_push(&s_state.data_out_fifo,
                             ZeroExtend32_u16(color15a) | (ZeroExtend32_u16(color15b) << 16));
        }
      } else {
        /* New path: rounded 8->5 conversion ((c+4)>>3, clamp 31). */
        const u32 a = ZeroExtend32(s_state.status.bits.data_output_bit15) << 15;
        for (u32 i = 0; i < 256u;) {
#define MDEC_E8TO5(color) (mdec_min_u32((((color) + 4u) >> 3), 0x1Fu))
          u32 color = s_state.block_rgb[i++];
          u32 r = MDEC_E8TO5(color & 0xFFu);
          u32 g = MDEC_E8TO5((color >>  8) & 0xFFu);
          u32 b = MDEC_E8TO5((color >> 16) & 0xFFu);
          const u32 color15a = r | (g << 5) | (b << 10) | a;

          color = s_state.block_rgb[i++];
          r = MDEC_E8TO5(color & 0xFFu);
          g = MDEC_E8TO5((color >>  8) & 0xFFu);
          b = MDEC_E8TO5((color >> 16) & 0xFFu);
          const u32 color15b = r | (g << 5) | (b << 10) | a;
#undef MDEC_E8TO5

          mdec_fifo_out_push(&s_state.data_out_fifo, color15a | (color15b << 16));
        }
      }
      break;
    }

    default:
      break;
  }

  DEBUG_LOG("Block copied out, fifo size = %u (%u bytes)",
            mdec_fifo_out_size(&s_state.data_out_fifo),
            mdec_fifo_out_size(&s_state.data_out_fifo) * (u32)sizeof(u32));

  /* If all blocks for the command have been copied, the command is done. */
  s_state.state = (s_state.remaining_halfwords == 0) ? MDEC_STATE_IDLE
                                                     : MDEC_STATE_DECODING_MACROBLOCK;
  mdec_execute();
}

static bool mdec_decode_rle_old(s16* blk, const u8* qt)
{
  /* Inverse zigzag for 8x8 DCT scan order: index in stream -> linear
   * coefficient index in the (column-major) block. */
  static const u8 zagzig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 
  };

  if (s_state.current_coefficient == 64) {
    memset(blk, 0, sizeof(s16) * 64);

    /* Skip 0xFE00 padding markers at the start of a block. */
    u16 n;
    for (;;) {
      if (mdec_fifo_in_empty(&s_state.data_in_fifo) || s_state.remaining_halfwords == 0)
        return false;
      n = mdec_fifo_in_pop(&s_state.data_in_fifo);
      s_state.remaining_halfwords--;
      if (n == 0xFE00)
        continue;
      break;
    }

    s_state.current_coefficient = 0;
    s_state.current_q_scale = (n >> 10) & 0x3Fu;
    s32 val = (s32)SignExtendN_u32((u32)(n & 0x3FFu), 10) *
              (s32)ZeroExtend32_u8(qt[s_state.current_coefficient]);

    if (s_state.current_q_scale == 0)
      val = (s32)SignExtendN_u32((u32)(n & 0x3FFu), 10) * 2;

    val = mdec_clamp_s32(val, -0x400, 0x3FF);
    if (s_state.current_q_scale > 0)
      blk[zagzig[s_state.current_coefficient]] = (s16)val;
    else
      blk[s_state.current_coefficient] = (s16)val;
  }

  while (!mdec_fifo_in_empty(&s_state.data_in_fifo) && s_state.remaining_halfwords > 0) {
    u16 n = mdec_fifo_in_pop(&s_state.data_in_fifo);
    s_state.remaining_halfwords--;

    s_state.current_coefficient += ((n >> 10) & 0x3Fu) + 1u;
    if (s_state.current_coefficient < 64) {
      /* Mednafen-style rounding: (val * qt * scale + 4) / 8. */
       s32 val =
        ((s32)SignExtendN_u32((u32)(n & 0x3FFu), 10) *
           (s32)ZeroExtend32_u8(qt[s_state.current_coefficient]) * 
           (s32)s_state.current_q_scale + 4) / 8;
      if (s_state.current_q_scale == 0)
        val = (s32)SignExtendN_u32((u32)(n & 0x3FFu), 10) * 2;

      val = mdec_clamp_s32(val, -0x400, 0x3FF);
      if (s_state.current_q_scale > 0)
        blk[zagzig[s_state.current_coefficient]] = (s16)val;
      else
        blk[s_state.current_coefficient] = (s16)val;
    }

    if (s_state.current_coefficient >= 63) {
      s_state.current_coefficient = 64;
      FMV_DUMP("T2_old", blk, 128u, (u64)s_state.current_block);
      return true;
    }
  }

  return false;
}

static void mdec_idct_old(s16* blk)
{
  /* Reference 64-bit accumulator IDCT.  scale_table is column-major in
   * use the >>32 high half of the 64-bit product, then clamp to 9 bits. */
  s64 temp_buffer[64];
  for (u32 x = 0; x < 8; x++) {
    for (u32 y = 0; y < 8; y++) {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += (s32)blk[u * 8 + x] * (s32)s_state.scale_table[y * 8 + u];
      temp_buffer[x + y * 8] = sum;
    }
  }
  for (u32 x = 0; x < 8; x++) {
    for (u32 y = 0; y < 8; y++) {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += temp_buffer[u + y * 8] * (s32)s_state.scale_table[x * 8 + u];

      const s32 trunc = (s32)((sum >> 32) + ((sum >> 31) & 1));
      blk[x + y * 8] = (s16)mdec_clamp_s32((s32)SignExtendN_u32((u32)trunc, 9), -128, 127);
    }
  }
}

static void mdec_yuv_to_rgb_old(u32 xx, u32 yy, const s16* Crblk, const s16* Cbblk, const s16* Yblk)
{
  /* BT.601 floating-point reference.  Slow but used as a sanity check
   * against the integer "new" path.  Coefficients straight from JPEG. */
  const s16 addval = s_state.status.bits.data_output_signed ? (s16)0 : (s16)0x80;
  for (u32 y = 0; y < 8; y++) {
    for (u32 x = 0; x < 8; x++) {
      s16 R = Crblk[((x + xx) / 2) + ((y + yy) / 2) * 8];
      s16 B = Cbblk[((x + xx) / 2) + ((y + yy) / 2) * 8];
      s16 G = (s16)((-0.3437f * (float)B) + (-0.7143f * (float)R));

      R = (s16)(1.402f * (float)R);
      B = (s16)(1.772f * (float)B);

      const s16 Y = Yblk[x + y * 8];
      R = (s16)(mdec_clamp_s32((s32)Y + R, -128, 127)) + addval;
      G = (s16)(mdec_clamp_s32((s32)Y + G, -128, 127)) + addval;
      B = (s16)(mdec_clamp_s32((s32)Y + B, -128, 127)) + addval;

      s_state.block_rgb[(x + xx) + ((y + yy) * 16)] =
        ZeroExtend32_u16((u16)R) |
        (ZeroExtend32_u16((u16)G) << 8) |
        (ZeroExtend32_u16((u16)B) << 16);
    }
  }
  FMV_DUMP("T3_old", s_state.block_rgb, sizeof(s_state.block_rgb), ((u64)yy << 16) | xx);
}

static bool mdec_decode_rle_new(s16* blk, const u8* qt)
{
  /* Row-major zigzag (transposed vs the old path) so the IDCT below can
   * iterate row-major and naturally vectorise. */
  static const u8 zigzag[64] = {
     0,  8,  1,  2,  9, 16, 24, 17, 10,  3,  4, 11, 18, 25, 32, 40,
    33, 26, 19, 12,  5,  6, 13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
    28, 21, 14,  7, 15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
    23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63 
  };

  if (s_state.current_coefficient == 64) {
    memset(blk, 0, sizeof(s16) * 64);

    u16 n;
    for (;;) {
      if (mdec_fifo_in_empty(&s_state.data_in_fifo) || s_state.remaining_halfwords == 0)
        return false;
      n = mdec_fifo_in_pop(&s_state.data_in_fifo);
      s_state.remaining_halfwords--;
      if (n == 0xFE00)
        continue;
      break;
    }

    s_state.current_coefficient = 0;
    s_state.current_q_scale = n >> 10;

    /* Store DCT blocks with 4 extra bits of precision; the +/- 8 bias
     * mirrors Mednafen's rounding so RGB output matches bit-for-bit. */
    const s32 val = (s32)SignExtendN_u32((u32)n, 10);
    const s32 coeff =
      (s_state.current_q_scale == 0) ? (val << 5)
                                     : (((val * qt[0]) << 4) + (val ? ((val < 0) ? 8 : -8) : 0));
    blk[zigzag[0]] = (s16)mdec_clamp_s32(coeff, -0x4000, 0x3FFF);
  }

  while (!mdec_fifo_in_empty(&s_state.data_in_fifo) && s_state.remaining_halfwords > 0) {
    u16 n = mdec_fifo_in_pop(&s_state.data_in_fifo);
    s_state.remaining_halfwords--;

    s_state.current_coefficient += ((n >> 10) + 1u);
    if (s_state.current_coefficient < 64) {
      const s32 val = (s32)SignExtendN_u32((u32)n, 10);
      const s32 scq = (s32)((u32)s_state.current_q_scale * (u32)qt[s_state.current_coefficient]);
      const s32 coeff = (scq == 0) ? (val << 5)
                                   : ((((val * scq) >> 3) << 4) + (val ? ((val < 0) ? 8 : -8) : 0));
      blk[zigzag[s_state.current_coefficient]] = (s16)mdec_clamp_s32(coeff, -0x4000, 0x3FFF);
    }

    if (s_state.current_coefficient >= 63) {
      s_state.current_coefficient = 64;
      FMV_DUMP("T2_new", blk, 128u, (u64)s_state.current_block);
      return true;
    }
  }

  return false;
}

static ALWAYS_INLINE s16 mdec_idct_row(const s16* blk, const s16* idct_matrix)
{
  /* IDCT matrix is -32768..32767, block is -16384..16383.  All eight
   * partial products fit in s32 without overflow; the scalar accumulator
   * rounds-to-nearest by adding 0x20000 then arithmetic-shifting by 18,
   * matching the C++ GSVector path's >>18 of (sum + 0x20000). */
  s64 sum = 0;
  for (u32 i = 0; i < 8; i++)
    sum += (s64)((s32)blk[i] * (s32)idct_matrix[i]);
  return (s16)((sum + 0x20000) >> 18);
}

static void mdec_idct_new(s16* blk)
{
  s16 temp[64];
  for (u32 x = 0; x < 8; x++) {
    for (u32 y = 0; y < 8; y++)
      temp[y * 8 + x] = mdec_idct_row(&blk[x * 8], &s_state.scale_table[y * 8]);
  }
  for (u32 x = 0; x < 8; x++) {
    for (u32 y = 0; y < 8; y++) {
      const s32 sum = mdec_idct_row(&temp[x * 8], &s_state.scale_table[y * 8]);
      blk[x * 8 + y] = (s16)mdec_clamp_s32((s32)SignExtendN_u32((u32)sum, 9), -128, 127);
    }
  }
}

static void mdec_yuv_to_rgb_new(u32 xx, u32 yy, const s16* Crblk, const s16* Cbblk, const s16* Yblk)
{
  /* BT.601 YUV->RGB integer coefficients, rounding formula from Mednafen:
   *   r = clamp(sext9(Y + (((359 * Cr) + 0x80) >> 8)), -128, 127) + addval
   *   g = clamp(sext9(Y + ((((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8)), -128, 127) + addval
   *   b = clamp(sext9(Y + (((454 * Cb) + 0x80) >> 8)), -128, 127) + addval
   *
   * The sext9-then-add-128 dance on the chroma side is hardware-accurate;
   * 0x80 is added *after* sign-extension, matching the real PS1 decoder.
   * The masks (~0x1F, ~0x07) preserve the LSB-truncation quirk of the
   * Sony silicon.
   *
   * Note: the chroma block is subsampled 2:1 in both axes, so we sample
   * Crblk/Cbblk at (xx/2 + (y+yy)/2 * 8) and reuse it across an 8-wide
   * Y row.  block_rgb is laid out as a 16x16 RGB-888 image. */
  const s32 addval = s_state.status.bits.data_output_signed ? 0 : 0x80;
  for (u32 y = 0; y < 8; y++) {
    const s16* cr_row = &Crblk[(xx / 2u) + ((y + yy) / 2u) * 8u];
    const s16* cb_row = &Cbblk[(xx / 2u) + ((y + yy) / 2u) * 8u];
    u32* const out_row = &s_state.block_rgb[xx + ((y + yy) * 16u)];

    for (u32 x = 0; x < 8; x++) {
      const s32 Cr = (s32)cr_row[x / 2u];
      const s32 Cb = (s32)cb_row[x / 2u];
      const s32 Y  = (s32)Yblk[y * 8u + x];

      const s32 r_chroma = ((359 * Cr) + 0x80) >> 8;
      const s32 b_chroma = ((454 * Cb) + 0x80) >> 8;
      const s32 g_chroma = ((((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8);

      const s32 r = mdec_clamp_s32((s32)SignExtendN_u32((u32)(Y + r_chroma), 9), -128, 127) + addval;
      const s32 g = mdec_clamp_s32((s32)SignExtendN_u32((u32)(Y + g_chroma), 9), -128, 127) + addval;
      const s32 b = mdec_clamp_s32((s32)SignExtendN_u32((u32)(Y + b_chroma), 9), -128, 127) + addval;

      /* RGB-888 packed low byte first; the 24bpp output stage then
       * stitches three of these into four output u32s.  This is *not*
       * BGR; the PS1 framebuffer is little-endian RGB, and the 24bpp
       * data is also produced in R,G,B byte order, hence the layout. */
      out_row[x] = ((u32)(u8)r) | (((u32)(u8)g) << 8) | (((u32)(u8)b) << 16);
    }
  }
  FMV_DUMP("T3_new", s_state.block_rgb, sizeof(s_state.block_rgb), ((u64)yy << 16) | xx);
}

static void mdec_yuv_to_mono(const s16* Yblk)
{
  const s32 addval = s_state.status.bits.data_output_signed ? 0 : 0x80;
  for (u32 i = 0; i < 64; i++)
    s_state.block_rgb[i] =
      (u32)(mdec_clamp_s32((s32)SignExtendN_u32((u32)Yblk[i], 9), -128, 127) + addval);
}

static void mdec_handle_set_quant_table_command(void)
{
  DebugAssert(s_state.remaining_halfwords >= 32);

  /* The packed 16-halfword burst lays out as 32 bytes per table; we copy
   * straight from the FIFO buffer (memcpy through a bounce array, since
   * the FIFO is a ring). */
  u16 packed_data[32];
  mdec_fifo_in_pop_range(&s_state.data_in_fifo, packed_data, 32);
  s_state.remaining_halfwords -= 32;
  memcpy(s_state.iq_y, packed_data, sizeof(s_state.iq_y));

  if (s_state.remaining_halfwords > 0) {
    DebugAssert(s_state.remaining_halfwords >= 32);
    mdec_fifo_in_pop_range(&s_state.data_in_fifo, packed_data, 32);
    s_state.remaining_halfwords -= 32;
    memcpy(s_state.iq_uv, packed_data, sizeof(s_state.iq_uv));
  }
}

static void mdec_handle_set_scale_command(void)
{
  DebugAssert(s_state.remaining_halfwords == 64);
  u16 packed_data[64];
  mdec_fifo_in_pop_range(&s_state.data_in_fifo, packed_data, 64);
  s_state.remaining_halfwords -= 64;
  mdec_set_scale_matrix(packed_data);
}

static void mdec_set_scale_matrix(const u16* values)
{
  /* The host writes the matrix column-major; we transpose into row-major
   * so the IDCT can iterate consecutive coefficients with stride-1
   * loads. */
  for (u32 y = 0; y < 8; y++) {
    for (u32 x = 0; x < 8; x++)
      s_state.scale_table[y * 8 + x] = (s16)values[x * 8 + y];
  }
}
