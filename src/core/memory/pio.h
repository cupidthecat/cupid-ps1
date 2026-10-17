/* Byte interface for the EXP1 region. No expansion device is attached;
 * reads return FFh and writes have no effect. */

#ifndef CUPID_CORE_PIO_H
#define CUPID_CORE_PIO_H

#include "common/types.h"

u8   pio_device_read(u32 offset);
void pio_device_write(u32 offset, u8 value);

#endif /* CUPID_CORE_PIO_H */
