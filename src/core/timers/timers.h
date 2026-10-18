/*
 * Root counters, clock sources, gates and interrupt deadlines.
 * Counter state and system-clock scheduling live in timers.c.
 */

#ifndef CUPID_CORE_TIMERS_H
#define CUPID_CORE_TIMERS_H

#include "core/shared/types.h"

typedef struct state_wrapper state_wrapper_t;

void timers_initialize(void);
void timers_shutdown  (void);
void timers_reset     (void);
bool timers_do_state  (state_wrapper_t* sw);

void timers_set_gate         (u32 timer, bool state);
void timers_cpu_clocks_changed(void);

/* dot clock / hblank / sysclk-div-8 */
bool timers_is_using_external_clock(u32 timer);
bool timers_is_sync_enabled        (u32 timer);

/* GPU queries */
bool timers_is_external_irq_enabled(u32 timer);

/* Counter-source ticks until the next IRQ, or INT32_MAX when the current
 * mode cannot issue another request. */
tick_count_t timers_get_ticks_until_irq(u32 timer);

void timers_add_ticks(u32 timer, tick_count_t ticks);

u32 timers_dbg_counter(u32 timer);
u32 timers_dbg_mode(u32 timer);
u32 timers_dbg_target(u32 timer);

u32  timers_read_register (u32 offset);
void timers_write_register(u32 offset, u32 value);

#endif /* CUPID_CORE_TIMERS_H */
