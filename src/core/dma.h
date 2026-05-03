/*
 *

 * namespace DMA -> dma_ prefix.  7 channels (MDECin, MDECout, GPU, CDROM,
 * SPU, PIO, OTC); state lives in a 7-element file-static array.  ImGui
 * debug window dropped (no UI yet).
 */
#ifndef CUPID_CORE_DMA_H
#define CUPID_CORE_DMA_H

#include "core/types.h"

typedef struct state_wrapper state_wrapper_t;

enum {
  DMA_NUM_CHANNELS = 7,
};

typedef enum {
  DMA_CHANNEL_MDECIN  = 0,
  DMA_CHANNEL_MDECOUT = 1,
  DMA_CHANNEL_GPU     = 2,
  DMA_CHANNEL_CDROM   = 3,
  DMA_CHANNEL_SPU     = 4,
  DMA_CHANNEL_PIO     = 5,
  DMA_CHANNEL_OTC     = 6,
} dma_channel_t;

void dma_initialize(void);
void dma_shutdown  (void);
void dma_reset     (void);
bool dma_do_state  (state_wrapper_t* sw);

u32  dma_read_register (u32 offset);
void dma_write_register(u32 offset, u32 value);

void dma_set_request(dma_channel_t channel, bool request);

const u32* dma_dbg_transfer_counts(void);
u32 dma_dbg_base_address(u32 channel);
u32 dma_dbg_block_control(u32 channel);
u32 dma_dbg_channel_control(u32 channel);
u32 dma_dbg_request(u32 channel);
u32 dma_dbg_dicr(void);

#endif /* CUPID_CORE_DMA_H */
