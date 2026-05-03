/*
 * namespace InterruptController -> interrupt_controller_ prefix.  All state
 * is process-wide (the PS1 has exactly one interrupt controller), so storage
 * lives as file-static globals in the .c.
 */

#ifndef CUPID_CORE_INTERRUPT_CONTROLLER_H
#define CUPID_CORE_INTERRUPT_CONTROLLER_H

#include "core/types.h"

typedef struct state_wrapper state_wrapper_t;

enum {
  INTERRUPT_CONTROLLER_NUM_IRQS = 11,
};

typedef enum {
  INTERRUPT_CONTROLLER_IRQ_VBLANK = 0, /* IRQ0 - VBLANK */
  INTERRUPT_CONTROLLER_IRQ_GPU    = 1, /* IRQ1 - GPU via GP0(1Fh) */
  INTERRUPT_CONTROLLER_IRQ_CDROM  = 2, /* IRQ2 - CDROM */
  INTERRUPT_CONTROLLER_IRQ_DMA    = 3, /* IRQ3 - DMA */
  INTERRUPT_CONTROLLER_IRQ_TMR0   = 4, /* IRQ4 - TMR0 (Sysclk or Dotclk) */
  INTERRUPT_CONTROLLER_IRQ_TMR1   = 5, /* IRQ5 - TMR1 (Sysclk Hblank) */
  INTERRUPT_CONTROLLER_IRQ_TMR2   = 6, /* IRQ6 - TMR2 (Sysclk or Sysclk/8) */
  INTERRUPT_CONTROLLER_IRQ_PAD    = 7, /* IRQ7 - Controller / memory card byte received */
  INTERRUPT_CONTROLLER_IRQ_SIO    = 8, /* IRQ8 - SIO */
  INTERRUPT_CONTROLLER_IRQ_SPU    = 9, /* IRQ9 - SPU */
  INTERRUPT_CONTROLLER_IRQ_IRQ10  = 10,/* IRQ10 - lightpen / PIO */
  INTERRUPT_CONTROLLER_IRQ_MAX_COUNT,
} interrupt_controller_irq_t;

void interrupt_controller_reset(void);
bool interrupt_controller_do_state(state_wrapper_t* sw);

/* Edge-triggered: only a 0->1 transition latches into I_STATUS. */
void interrupt_controller_set_line_state(interrupt_controller_irq_t irq, bool state);

/* Lightweight frontend trace counters, indexed by IRQ number. */
const u32* irq_dbg_set_counts(void);
u32 irq_dbg_status_register(void);
u32 irq_dbg_mask_register(void);

u32  interrupt_controller_read_register (u32 offset);
void interrupt_controller_write_register(u32 offset, u32 value);

#endif /* CUPID_CORE_INTERRUPT_CONTROLLER_H */
