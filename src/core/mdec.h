/*
 * MDEC; the PS1's hardware MPEG-1-style movie decoder.  Receives RLE/DCT
 * encoded macroblocks via DMA channel 0 (MDECin), runs them through an IDCT
 * + (optionally) YUV->RGB pipeline, and feeds the result back to RAM via DMA
 * channel 1 (MDECout).  All state is process-wide singleton -> file-static
 * globals in mdec.c.
 *
 * namespace MDEC -> mdec_ prefix.  imgui debug window dropped (no GUI here).
 */

#ifndef CUPID_CORE_MDEC_H
#define CUPID_CORE_MDEC_H

#include "core/types.h"

typedef struct state_wrapper state_wrapper_t;

void mdec_initialize(void);
void mdec_shutdown(void);
void mdec_reset(void);
bool mdec_do_state(state_wrapper_t* sw);

bool mdec_is_active(void);
bool mdec_is_decoding_macroblock(void);
void mdec_end_frame(void);

/* MMIO at 0x1F801820 (data) and 0x1F801824 (status/control). */
u32  mdec_read_register (u32 offset);
void mdec_write_register(u32 offset, u32 value);

/* DMA channel 1 (MDECout): the decoder pulls up to `word_count` words out
 * of its 768-word output FIFO into `words`. */
void mdec_dma_read (u32* words, u32 word_count);

/* DMA channel 0 (MDECin): caller hands us `word_count` 32-bit words; we
 * unpack them as halfwords into the 1024-byte input FIFO and kick the
 * decode state machine. */
void mdec_dma_write(const u32* words, u32 word_count);

u32 mdec_dbg_status(void);
u32 mdec_dbg_fifo_status(void);

#endif /* CUPID_CORE_MDEC_H */
