/* cupid-ps1 noise rate selection from the SPU control register. */
#include "core/spu/noise_clock.h"

bool spu_noise_clock_tick(spu_noise_clock_t* state, u8 clock) {
  clock &= 63;
  const bool fastest = clock >= 60;
  const u32  divider = state->divider + (fastest ? 0x8000u : 2u << (clock >> 2));
  if (divider < 0x8000u) {
    state->divider = (u16)divider;
    return false;
  }

  state->divider    = 0;
  const u32 counter = state->counter + (fastest ? 8u : 4u + (clock & 3u));
  state->counter    = (u8)(counter & 7u);
  return counter >= 8u;
}
