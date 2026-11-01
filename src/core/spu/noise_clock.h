/* cupid-ps1 shared SPU noise divider and fractional step counter. */
#ifndef CUPID_CORE_SPU_NOISE_CLOCK_H
#define CUPID_CORE_SPU_NOISE_CLOCK_H

#include "common/types.h"

typedef struct {
  u16 divider;
  u8 counter;
} spu_noise_clock_t;

/* Advance one 44.1 kHz sample. True means the shared LFSR shifts once. */
bool spu_noise_clock_tick(spu_noise_clock_t* state, u8 clock);

#endif
