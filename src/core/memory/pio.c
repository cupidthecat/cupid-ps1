/* No expansion device is attached. Reads return FFh and writes are discarded.
 * This fixed value does not model the electrical state of a floating bus. */

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
