/*
 * PIO / EXP1 expansion port - open-bus stub.
 *
 * The PIO/EXP1 region (0x1F000000..0x1F7FFFFF) is intentionally not
 * emulated: cupid-ps1 ships no expansion device emulation, and every BIOS we
 * support probes EXP1 during boot expecting either a cart signature or
 * floating bus.  Returning open-bus 0xFF for reads and discarding writes
 * matches what real hardware does with an empty expansion port and lets
 * the BIOS proceed unimpeded.  When/if PIO devices are added (cheat carts,
 * Action Replay, etc) these two functions become the dispatch entry
 * points; bus.c is already wired to call them.
 */

#include "pio.h"

#include "common/types.h"

u8 pio_device_read(u32 offset)
{
  (void)offset;
  return 0xFFu;
}

void pio_device_write(u32 offset, u8 value)
{
  (void)offset;
  (void)value;
}
