/*
 * PIO / EXP1 expansion port - minimal stub.
 *
 * The PSX maps an off-board parallel I/O / cartridge expansion window at
 * 0x1F000000..0x1F7FFFFF.  Real hardware leaves this floating when no
 * expansion device is plugged in (every BIOS we care about probes the
 * region during boot to look for cheat carts and silently moves on).
 *
 * cupid-ps1 does not emulate any PIO device.  The bus traps EXP1 reads here so
 * the BIOS sees consistent open-bus behaviour: reads return 0xFF, writes
 * are silently dropped.  This module exists in the core layer (not the
 * frontend) because EXP1 is part of the PSX bus topology, not an SDL/host
 * concern.
 */

#ifndef CUPID_CORE_PIO_H
#define CUPID_CORE_PIO_H

#include "common/types.h"

u8   pio_device_read(u32 offset);
void pio_device_write(u32 offset, u8 value);

#endif /* CUPID_CORE_PIO_H */
