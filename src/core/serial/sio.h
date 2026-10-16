/*
 * SIO1 (the *other* serial port at 0x1F801050).  Distinct from SIO0 / pad.
 * Largely a stub that implements the register interface so games / BIOS
 * code probing the port behave sensibly; the only real side effect is the
 * optional TTY redirect controlled by g_settings.sio_redirect_to_tty.
 */

#ifndef CUPID_CORE_SIO_H
#define CUPID_CORE_SIO_H

#include "common/types.h"

typedef struct state_wrapper state_wrapper_t;

void sio_initialize(void);
void sio_shutdown  (void);
void sio_reset     (void);
bool sio_do_state  (state_wrapper_t* sw);

u32  sio_read_register (u32 offset);
void sio_write_register(u32 offset, u32 value);

#endif /* CUPID_CORE_SIO_H */
