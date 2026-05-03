/*
 * Two MMIO registers (I_STATUS at +0x00, I_MASK at +0x04) plus a hidden
 * "line state" mirror used to keep edge-triggered detection honest.  Whenever
 * STATUS & MASK is non-zero we raise the CPU IRQ line.
 *
 * External (parallel-agent) dependencies:
 *   cpu_core_set_irq_request(bool); forwarded by cpu_core when ported.
 */

#include "core/interrupt_controller.h"
#include "core/cpu_core.h"

#include "util/state_wrapper.h"

#include "common/log.h"

LOG_CHANNEL(InterruptController);

/* Top bit (15..11) is reserved on the PS1; only the low 11 bits are
 * writable.  The mask doubles as the "any IRQ pending" check window. */
#define IRQ_REGISTER_WRITE_MASK ((u32)((1u << INTERRUPT_CONTROLLER_NUM_IRQS) - 1u))

static u32 s_interrupt_status_register;
static u32 s_interrupt_mask_register;
static u32 s_interrupt_line_state;

#if !defined(NDEBUG)
static const char* const s_irq_names[INTERRUPT_CONTROLLER_IRQ_MAX_COUNT] = {
  "VBLANK", "GPU", "CDROM", "DMA", "TMR0", "TMR1", "TMR2", "PAD", "SIO", "SPU", "IRQ10"
};
#endif

static void interrupt_controller_update_cpu_request(void);

void interrupt_controller_reset(void)
{
  s_interrupt_status_register = 0;
  s_interrupt_mask_register   = 0;
  s_interrupt_line_state      = 0;
}

bool interrupt_controller_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_u32(sw, &s_interrupt_status_register);
  state_wrapper_do_u32(sw, &s_interrupt_mask_register);
  /* Old (pre-v63) save states didn't persist the device-side line state;
   * fall back to mirroring STATUS so latches survive. */
  if (state_wrapper_get_version(sw) >= 63)
    state_wrapper_do_u32(sw, &s_interrupt_line_state);
  else
    s_interrupt_line_state = s_interrupt_status_register;

  return !state_wrapper_has_error(sw);
}

static u32 s_dbg_irq_set_count[32];
__attribute__((visibility("default")))
const u32* irq_dbg_set_counts(void) { return s_dbg_irq_set_count; }

u32 irq_dbg_status_register(void) { return s_interrupt_status_register; }
u32 irq_dbg_mask_register(void) { return s_interrupt_mask_register; }

void interrupt_controller_set_line_state(interrupt_controller_irq_t irq, bool state)
{
  if (state && (u32)irq < 32) s_dbg_irq_set_count[(u32)irq]++;
  /* Interrupts are edge-triggered: latch into I_STATUS only on 0->1. */
  const u32 bit = (1u << (u32)irq);
  const u32 prev_state = s_interrupt_line_state;
  s_interrupt_line_state = (s_interrupt_line_state & ~bit) | (state ? bit : 0u);
  if (s_interrupt_line_state == prev_state)
    return;

#if !defined(NDEBUG)
  if (!(prev_state & bit) && state)
    DEBUG_LOG("%s IRQ triggered", s_irq_names[(size_t)irq]);
  else if ((prev_state & bit) && !state)
    DEBUG_LOG("%s IRQ line inactive", s_irq_names[(size_t)irq]);
#endif

  s_interrupt_status_register |=
    (state ? (prev_state ^ s_interrupt_line_state) : 0u) & s_interrupt_line_state;
  interrupt_controller_update_cpu_request();
}

u32 interrupt_controller_read_register(u32 offset)
{
  switch (offset)
  {
    case 0x00: /* I_STATUS */
      return s_interrupt_status_register;

    case 0x04: /* I_MASK */
      return s_interrupt_mask_register;

    default:
      ERROR_LOG("Invalid read at offset 0x%08X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void interrupt_controller_write_register(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: /* I_STATUS; writes acknowledge (clear) bits. */
    {
#if !defined(NDEBUG)
      const u32 cleared_bits = (s_interrupt_status_register & ~value);
      for (u32 i = 0; i < (u32)INTERRUPT_CONTROLLER_IRQ_MAX_COUNT; i++) {
        if (cleared_bits & (1u << i))
          DEBUG_LOG("%s IRQ cleared", s_irq_names[i]);
      }
#endif
      s_interrupt_status_register = s_interrupt_status_register & (value & IRQ_REGISTER_WRITE_MASK);
      interrupt_controller_update_cpu_request();
      break;
    }

    case 0x04: /* I_MASK */
      DEBUG_LOG("Interrupt mask <- 0x%08X", value);
      s_interrupt_mask_register = value & IRQ_REGISTER_WRITE_MASK;
      interrupt_controller_update_cpu_request();
      break;

    default:
      ERROR_LOG("Invalid write at offset 0x%08X", offset);
      break;
  }
}

static void interrupt_controller_update_cpu_request(void)
{
  const bool state = (s_interrupt_status_register & s_interrupt_mask_register) != 0;
  cpu_set_irq_request(state);
}
