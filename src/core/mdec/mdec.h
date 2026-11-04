/* MDEC command registers, decode state and DMA input/output. */

#ifndef CUPID_CORE_MDEC_H
#define CUPID_CORE_MDEC_H

#include "core/shared/types.h"

typedef struct state_wrapper state_wrapper_t;

void mdec_initialize(void);
void mdec_shutdown(void);
void mdec_reset(void);
bool mdec_do_state(state_wrapper_t* sw);

bool mdec_is_active(void);
bool mdec_is_decoding_macroblock(void);
void mdec_end_frame(void);

/* MMIO at 0x1F801820 (data) and 0x1F801824 (status/control). */
u32  mdec_read_register(u32 offset);
void mdec_write_register(u32 offset, u32 value);

/* Read up to word_count raw device words, without applying RAM placement.
 * DMA channel 1 uses the per-word offsets below. */
void mdec_dma_read(u32* words, u32 word_count);
/* Read one device word and its raster-placement offset from the DMA address.
 * Returns false without consuming data when the output queue is empty. */
bool mdec_dma_read_word(u32* word, s32* address_offset_words);

/* DMA channel 0 (MDECin): caller hands us `word_count` 32-bit words; we
 * unpack them as halfwords into the 1024-byte input FIFO and kick the
 * decode state machine. */
void mdec_dma_write(const u32* words, u32 word_count);

u32 mdec_dbg_status(void);
u32 mdec_dbg_fifo_status(void);

#endif /* CUPID_CORE_MDEC_H */
