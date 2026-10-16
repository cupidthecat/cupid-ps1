/*
 * SIO0 controller and memory-card interfaces.
 * Slot accessors manage controller, memory-card and multitap devices; a NULL
 * device pointer leaves the slot empty. State lives in pad.c.
 */

#ifndef CUPID_CORE_PAD_H
#define CUPID_CORE_PAD_H

#include "core/shared/types.h"

#include "common/types.h"

typedef struct state_wrapper state_wrapper_t;
typedef struct controller    controller_t;
typedef struct memory_card   memory_card_t;
typedef struct multitap      multitap_t;

void pad_initialize(void);
void pad_shutdown(void);
void pad_reset(void);
bool pad_do_state(state_wrapper_t* sw, bool is_memory_state);

controller_t* pad_get_controller(u32 slot);
/* Takes ownership of `dev`; the previous slot occupant is freed. */
void pad_set_controller(u32 slot, controller_t* dev);

memory_card_t* pad_get_memory_card(u32 slot);
void           pad_set_memory_card(u32 slot, memory_card_t* dev);
/* Removes and returns the card without destroying it; caller now owns. */
memory_card_t* pad_remove_memory_card(u32 slot);

multitap_t* pad_get_multitap(u32 slot);

u32  pad_read_register (u32 offset);
void pad_write_register(u32 offset, u32 value);

bool pad_is_transmitting(void);

#endif /* CUPID_CORE_PAD_H */
