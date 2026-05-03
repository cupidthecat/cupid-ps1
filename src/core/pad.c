/*
 * SIO0 controller / memory-card arbiter at 0x1F801040.
 *
 *   - Memory-card backup-on-rollback path: the cupid-ps1 port doesn't yet have
 *     runahead, so the BackupMemoryCardState/RestoreMemoryCardState pair
 *     and the associated frame-window logic in DoState are simplified to
 *     "always serialize the memcards" when running.  When runahead lands
 *     this path can be revisited.
 *   - Save-state mismatch OSD messages (Host:: not ported); state slot
 *     mismatches just log instead of popping a notification.
 *   - The dummy memory card used for save-state cross-checks.
 *
 * Memory cards themselves are a future port; the slot getter/setter exists
 * so the plumbing stays ready for them, but the file currently holds NULL
 * card pointers and skips any per-card state.
 */

#include "core/pad.h"
#include "core/controller.h"
#include "core/interrupt_controller.h"
#include "core/multitap.h"
#include "core/timing_event.h"
#include "core/types.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Pad);

/* Forward decl: the memory card module is a sibling parallel agent. */
struct memory_card;
extern void memory_card_destroy(struct memory_card* mc) __attribute__((weak));
extern void memory_card_reset(struct memory_card* mc) __attribute__((weak));
extern bool memory_card_do_state(struct memory_card* mc, state_wrapper_t* sw) __attribute__((weak));
extern bool memory_card_transfer(struct memory_card* mc, u8 data_in, u8* data_out) __attribute__((weak));
extern void memory_card_reset_transfer_state(struct memory_card* mc) __attribute__((weak));

typedef enum : u32 {
  PAD_STATE_IDLE,
  PAD_STATE_TRANSMITTING,
  PAD_STATE_WAITING_FOR_ACK,
} pad_state_t;

typedef enum : u8 {
  PAD_ACTIVE_NONE,
  PAD_ACTIVE_CONTROLLER,
  PAD_ACTIVE_MEMORY_CARD,
  PAD_ACTIVE_MULTITAP,
} pad_active_device_t;

/* JOY_CTRL bit positions (16-bit). */
#define JOY_CTRL_TXEN     (1u << 0)
#define JOY_CTRL_SELECT   (1u << 1)
#define JOY_CTRL_RXEN     (1u << 2)
#define JOY_CTRL_ACK      (1u << 4)
#define JOY_CTRL_RESET    (1u << 6)
#define JOY_CTRL_TXINTEN  (1u << 10)
#define JOY_CTRL_RXINTEN  (1u << 11)
#define JOY_CTRL_ACKINTEN (1u << 12)
#define JOY_CTRL_SLOT_BIT 13u

ALWAYS_INLINE u32 joy_ctrl_slot(u16 v) { return ((u32)v >> JOY_CTRL_SLOT_BIT) & 1u; }

/* JOY_STAT bit positions (32-bit). */
#define JOY_STAT_TXRDY        (1u << 0)
#define JOY_STAT_RXFIFONEMPTY (1u << 1)
#define JOY_STAT_TXDONE       (1u << 2)
#define JOY_STAT_ACKINPUT     (1u << 7)
#define JOY_STAT_INTR         (1u << 9)

/* From @JaCzekanski (https://github.com/JaCzekanski):
 * - ACK pulse itself lasts ~96 ticks (~2.84us at 33.87MHz master clock),
 *   not modeled.
 * - ACK delay between transfer end and pulse: 6.8-13.7us total, ~338 ticks
 *   for ~9.98us on standard pads, so we use 450 to leave headroom for
 *   command-ID handling.
 * - Memory-card responds faster: ~5us = ~170 ticks. */
ALWAYS_INLINE tick_count_t pad_get_ack_ticks(bool memory_card)
{
  return memory_card ? 170 : 450;
}

typedef struct {
  controller_t*  controllers[NUM_CONTROLLER_AND_CARD_PORTS];
  struct memory_card* memory_cards[NUM_CONTROLLER_AND_CARD_PORTS];

  multitap_t multitaps[NUM_MULTITAPS];

  timing_event_t transfer_event;

  pad_state_t state;

  u32 joy_stat;
  u16 joy_ctrl;
  u16 joy_mode;
  u16 joy_baud;

  pad_active_device_t active_device;
  u8 receive_buffer;
  u8 transmit_buffer;
  u8 transmit_value;
  bool receive_buffer_full;
  bool transmit_buffer_full;

  u32 last_memory_card_transfer_frame;
} pad_state_struct_t;

static pad_state_struct_t s_pad;

static bool         pad_can_transfer(void);
static tick_count_t pad_get_transfer_ticks(void);
static void         pad_soft_reset(void);
static void         pad_update_joy_stat(void);
static void         pad_transfer_event(void* user, tick_count_t ticks, tick_count_t ticks_late);
static void         pad_begin_transfer(void);
static void         pad_do_transfer(tick_count_t ticks_late);
static void         pad_do_ack(void);
static void         pad_end_transfer(void);
static void         pad_reset_device_transfer_state(void);
static void         pad_trigger_irq(const char* type);

static bool pad_do_state_controller(state_wrapper_t* sw, u32 i);
static bool pad_do_state_memcard(state_wrapper_t* sw, u32 i);

void pad_initialize(void)
{
  static const char k_event_name[] = "Pad Serial Transfer";
  timing_event_init(&s_pad.transfer_event, k_event_name, (u32)(sizeof(k_event_name) - 1u),
                    1, 1, pad_transfer_event, NULL);
  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    multitap_init(&s_pad.multitaps[i]);
  pad_reset();
}

void pad_shutdown(void)
{
  if (timing_event_is_active(&s_pad.transfer_event))
    timing_event_deactivate(&s_pad.transfer_event);
  timing_event_destroy(&s_pad.transfer_event);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_pad.controllers[i])
    {
      controller_destroy(s_pad.controllers[i]);
      s_pad.controllers[i] = NULL;
    }
    if (s_pad.memory_cards[i] && memory_card_destroy)
    {
      memory_card_destroy(s_pad.memory_cards[i]);
      s_pad.memory_cards[i] = NULL;
    }
  }
}

void pad_reset(void)
{
  pad_soft_reset();
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_pad.controllers[i])
      controller_reset(s_pad.controllers[i]);
    if (s_pad.memory_cards[i] && memory_card_reset)
      memory_card_reset(s_pad.memory_cards[i]);
  }
  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    multitap_reset(&s_pad.multitaps[i]);
}

controller_t* pad_get_controller(u32 slot) { return s_pad.controllers[slot]; }

void pad_set_controller(u32 slot, controller_t* dev)
{
  if (s_pad.controllers[slot])
    controller_destroy(s_pad.controllers[slot]);
  s_pad.controllers[slot] = dev;
}

struct memory_card* pad_get_memory_card(u32 slot) { return s_pad.memory_cards[slot]; }

void pad_set_memory_card(u32 slot, struct memory_card* dev)
{
  INFO_LOG("Memory card slot %u: %s", slot, dev ? "<set>" : "<unplugged>");
  if (s_pad.memory_cards[slot] && memory_card_destroy)
    memory_card_destroy(s_pad.memory_cards[slot]);
  s_pad.memory_cards[slot] = dev;
}

struct memory_card* pad_remove_memory_card(u32 slot)
{
  struct memory_card* ret = s_pad.memory_cards[slot];
  s_pad.memory_cards[slot] = NULL;
  if (ret && memory_card_reset)
    memory_card_reset(ret);
  return ret;
}

multitap_t* pad_get_multitap(u32 slot) { return &s_pad.multitaps[slot]; }

bool pad_is_transmitting(void) { return s_pad.state != PAD_STATE_IDLE; }

static bool pad_can_transfer(void)
{
  return s_pad.transmit_buffer_full && (s_pad.joy_ctrl & JOY_CTRL_SELECT) && (s_pad.joy_ctrl & JOY_CTRL_TXEN);
}

static tick_count_t pad_get_transfer_ticks(void)
{
  return (tick_count_t)((u32)s_pad.joy_baud * 8u);
}

static void pad_trigger_irq(const char* type)
{
  DEBUG_LOG("Triggering %s interrupt", type);
  s_pad.joy_stat |= JOY_STAT_INTR;
  interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_PAD, true);
}

u32 pad_read_register(u32 offset)
{
  switch (offset)
  {
    case 0x00: /* JOY_DATA */
    {
      if (pad_is_transmitting())
        timing_event_invoke_early(&s_pad.transfer_event, false);

      const u8 value = s_pad.receive_buffer_full ? s_pad.receive_buffer : (u8)0xFF;
      DEBUG_LOG("JOY_DATA (R) -> 0x%02X%s", value, s_pad.receive_buffer_full ? "" : "(EMPTY)");
      s_pad.receive_buffer_full = false;
      pad_update_joy_stat();
      return ZeroExtend32(value) | (ZeroExtend32(value) << 8) |
             (ZeroExtend32(value) << 16) | (ZeroExtend32(value) << 24);
    }

    case 0x04: /* JOY_STAT */
    {
      if (pad_is_transmitting())
        timing_event_invoke_early(&s_pad.transfer_event, false);
      const u32 bits = s_pad.joy_stat;
      /* ACKINPUT auto-clears on read, matching the JaCzekanski model. */
      s_pad.joy_stat &= ~JOY_STAT_ACKINPUT;
      return bits;
    }

    case 0x08: /* JOY_MODE */
      return ZeroExtend32(s_pad.joy_mode);

    case 0x0A: /* JOY_CTRL */
      return ZeroExtend32(s_pad.joy_ctrl);

    case 0x0E: /* JOY_BAUD */
      return ZeroExtend32(s_pad.joy_baud);

    default:
      ERROR_LOG("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void pad_write_register(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: /* JOY_DATA */
    {
      DEBUG_LOG("JOY_DATA (W) <- 0x%02X", value);

      if (s_pad.transmit_buffer_full)
        WARNING_LOG("TX FIFO overrun");

      s_pad.transmit_buffer = Truncate8(value);
      s_pad.transmit_buffer_full = true;

      if (s_pad.joy_ctrl & JOY_CTRL_TXINTEN)
        pad_trigger_irq("TX");

      if (!pad_is_transmitting() && pad_can_transfer())
        pad_begin_transfer();
      return;
    }

    case 0x0A: /* JOY_CTRL */
    {
      DEBUG_LOG("JOY_CTRL <- 0x%04X", value);

      s_pad.joy_ctrl = Truncate16(value);
      if (s_pad.joy_ctrl & JOY_CTRL_RESET)
        pad_soft_reset();

      if (s_pad.joy_ctrl & JOY_CTRL_ACK)
      {
        /* Clear pending IRQ + interrupt line. */
        s_pad.joy_stat &= ~JOY_STAT_INTR;
        interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_PAD, false);
      }

      if (!(s_pad.joy_ctrl & JOY_CTRL_SELECT))
        pad_reset_device_transfer_state();

      if (!(s_pad.joy_ctrl & JOY_CTRL_SELECT) || !(s_pad.joy_ctrl & JOY_CTRL_TXEN))
      {
        if (pad_is_transmitting())
          pad_end_transfer();
      }
      else
      {
        if (!pad_is_transmitting() && pad_can_transfer())
          pad_begin_transfer();
      }

      pad_update_joy_stat();
      return;
    }

    case 0x08: /* JOY_MODE */
      DEBUG_LOG("JOY_MODE <- 0x%08X", value);
      s_pad.joy_mode = Truncate16(value);
      return;

    case 0x0E: /* JOY_BAUD */
      DEBUG_LOG("JOY_BAUD <- 0x%08X", value);
      s_pad.joy_baud = Truncate16(value);
      return;

    default:
      ERROR_LOG("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

static void pad_soft_reset(void)
{
  if (pad_is_transmitting())
    pad_end_transfer();

  s_pad.joy_ctrl = 0;
  s_pad.joy_stat = 0;
  s_pad.joy_mode = 0;
  s_pad.receive_buffer = 0;
  s_pad.receive_buffer_full = false;
  s_pad.transmit_buffer = 0;
  s_pad.transmit_buffer_full = false;
  pad_reset_device_transfer_state();
  pad_update_joy_stat();
}

static void pad_update_joy_stat(void)
{
  if (s_pad.receive_buffer_full)
    s_pad.joy_stat |= JOY_STAT_RXFIFONEMPTY;
  else
    s_pad.joy_stat &= ~JOY_STAT_RXFIFONEMPTY;

  if (!s_pad.transmit_buffer_full && s_pad.state != PAD_STATE_TRANSMITTING)
    s_pad.joy_stat |= JOY_STAT_TXDONE;
  else
    s_pad.joy_stat &= ~JOY_STAT_TXDONE;

  if (!s_pad.transmit_buffer_full)
    s_pad.joy_stat |= JOY_STAT_TXRDY;
  else
    s_pad.joy_stat &= ~JOY_STAT_TXRDY;
}

static void pad_transfer_event(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks;
  if (s_pad.state == PAD_STATE_TRANSMITTING)
    pad_do_transfer(ticks_late);
  else
    pad_do_ack();
}

static void pad_begin_transfer(void)
{
  DebugAssert(s_pad.state == PAD_STATE_IDLE && pad_can_transfer());
  DEBUG_LOG("Starting transfer");

  s_pad.joy_ctrl |= JOY_CTRL_RXEN;
  s_pad.transmit_value = s_pad.transmit_buffer;
  s_pad.transmit_buffer_full = false;

  /* The transfer or its IRQ must be delayed: the BIOS does
   *   1) ctrl <- TX + IRQ-on-ACK;  2) write 0x01 to TX;
   *   3) (delay);                  4) ctrl <- ACK (clears IRQ flag);
   *   5) clear IRQ7 in IC;         6) wait for RX !empty, read first byte;
   *   7) check IRQ7 was set; if not, no device.
   * If we run the transfer instantly, the INTR/IRQ7 set by the *new*
   * transfer is wiped in steps 4/5, so step 7 reports no device. */
  s_pad.state = PAD_STATE_TRANSMITTING;
  timing_event_set_period_and_schedule(&s_pad.transfer_event, pad_get_transfer_ticks());
}

static void pad_do_transfer(tick_count_t ticks_late)
{
  (void)ticks_late;
  const u32 ctrl_slot = joy_ctrl_slot(s_pad.joy_ctrl);
  DEBUG_LOG("Transferring slot %u", ctrl_slot);

  /* If multitap on port 1 is enabled, slot 4 holds the second-port primary
   * controller; the lookup converts ctrl-bit-13 + multitap presence to a
   * physical port index. */
  const u8 device_index = (u8)(s_pad.multitaps[0].enabled ? 4u : ctrl_slot);
  controller_t*       controller  = s_pad.controllers[device_index];
  struct memory_card* memory_card = s_pad.memory_cards[device_index];

  s_pad.joy_ctrl |= JOY_CTRL_RXEN;

  const u8 data_out = s_pad.transmit_value;
  u8 data_in = 0xFF;
  bool ack = false;

  switch (s_pad.active_device)
  {
    case PAD_ACTIVE_NONE:
    {
      multitap_t* mtap = &s_pad.multitaps[ctrl_slot];
      if (multitap_is_enabled(mtap))
      {
        if ((ack = multitap_transfer(mtap, data_out, &data_in)))
        {
          TRACE_LOG("Active device set to tap %u, sent 0x%02X, received 0x%02X",
                    (unsigned)ctrl_slot, data_out, data_in);
          s_pad.active_device = PAD_ACTIVE_MULTITAP;
        }
      }
      else
      {
        if (!controller || (ack = controller_transfer(controller, data_out, &data_in)) == false)
        {
          if (!memory_card || !memory_card_transfer ||
              (ack = memory_card_transfer(memory_card, data_out, &data_in)) == false)
          {
            TRACE_LOG("Nothing connected or ACK'ed");
          }
          else
          {
            TRACE_LOG("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
            s_pad.active_device = PAD_ACTIVE_MEMORY_CARD;
            /* Frame number bookkeeping is only useful with runahead, which
             * isn't yet ported; track it anyway so the future hook works. */
            s_pad.last_memory_card_transfer_frame = 0;
          }
        }
        else
        {
          TRACE_LOG("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
          s_pad.active_device = PAD_ACTIVE_CONTROLLER;
        }
      }
      break;
    }

    case PAD_ACTIVE_CONTROLLER:
      if (controller)
      {
        ack = controller_transfer(controller, data_out, &data_in);
        TRACE_LOG("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
      break;

    case PAD_ACTIVE_MEMORY_CARD:
      if (memory_card && memory_card_transfer)
      {
        s_pad.last_memory_card_transfer_frame = 0;
        ack = memory_card_transfer(memory_card, data_out, &data_in);
        TRACE_LOG("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
      break;

    case PAD_ACTIVE_MULTITAP:
    {
      multitap_t* mtap = &s_pad.multitaps[ctrl_slot];
      if (multitap_is_enabled(mtap))
      {
        ack = multitap_transfer(mtap, data_out, &data_in);
        TRACE_LOG("Transfer tap %u, sent 0x%02X, received 0x%02X, acked: %s",
                  (unsigned)ctrl_slot, data_out, data_in, ack ? "true" : "false");
      }
      break;
    }
  }

  s_pad.receive_buffer = data_in;
  s_pad.receive_buffer_full = true;
  if (s_pad.joy_ctrl & JOY_CTRL_RXINTEN)
    pad_trigger_irq("TX");

  if (!ack)
  {
    s_pad.active_device = PAD_ACTIVE_NONE;
    pad_end_transfer();
  }
  else
  {
    const bool memcard_transfer = (s_pad.active_device == PAD_ACTIVE_MEMORY_CARD) ||
                                  (s_pad.active_device == PAD_ACTIVE_MULTITAP &&
                                   multitap_is_reading_memory_card(&s_pad.multitaps[ctrl_slot]));
    const tick_count_t ack_timer = pad_get_ack_ticks(memcard_transfer);
    DEBUG_LOG("Delaying ACK for %d ticks", (int)ack_timer);
    s_pad.state = PAD_STATE_WAITING_FOR_ACK;
    timing_event_set_period_and_schedule(&s_pad.transfer_event, ack_timer);
  }

  pad_update_joy_stat();
}

static void pad_do_ack(void)
{
  s_pad.joy_stat |= JOY_STAT_ACKINPUT;
  if (s_pad.joy_ctrl & JOY_CTRL_ACKINTEN)
    pad_trigger_irq("ACK");

  pad_end_transfer();
  pad_update_joy_stat();

  if (pad_can_transfer())
    pad_begin_transfer();
}

static void pad_end_transfer(void)
{
  DebugAssert(s_pad.state == PAD_STATE_TRANSMITTING || s_pad.state == PAD_STATE_WAITING_FOR_ACK);
  DEBUG_LOG("Ending transfer");
  s_pad.state = PAD_STATE_IDLE;
  timing_event_deactivate(&s_pad.transfer_event);
}

static void pad_reset_device_transfer_state(void)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_pad.controllers[i])
      controller_reset_transfer_state(s_pad.controllers[i]);
    if (s_pad.memory_cards[i] && memory_card_reset_transfer_state)
      memory_card_reset_transfer_state(s_pad.memory_cards[i]);
  }
  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    multitap_reset_transfer_state(&s_pad.multitaps[i]);
  s_pad.active_device = PAD_ACTIVE_NONE;
}

static bool pad_do_state_controller(state_wrapper_t* sw, u32 i)
{
  controller_t* c = s_pad.controllers[i];
  controller_type_t live_type = c ? controller_get_type(c) : CONTROLLER_TYPE_NONE;
  u32 state_type_value = (u32)live_type;
  state_wrapper_do_u32(sw, &state_type_value);
  controller_type_t state_type = (controller_type_t)state_type_value;

  if (live_type != state_type)
  {
    DEV_LOG("Controller type mismatch in slot %u: state=%u live=%u",
            i + 1u, (unsigned)state_type, (unsigned)live_type);
    if (c) controller_reset(c);
  }

  if (state_type == CONTROLLER_TYPE_NONE)
    return true;

  if (!state_wrapper_do_marker(sw, "Controller"))
    return false;

  if (c && controller_get_type(c) == state_type)
    return controller_do_state(c, sw, true);

  /* Type mismatch: chew through the bytes via a throwaway instance so the
   * cursor stays aligned for whatever comes next. */
  controller_t* dummy = controller_create(state_type, i);
  if (dummy)
  {
    bool ok = controller_do_state(dummy, sw, true);
    controller_destroy(dummy);
    return ok;
  }
  return true;
}

static bool pad_do_state_memcard(state_wrapper_t* sw, u32 i)
{
  bool card_present_in_state = (s_pad.memory_cards[i] != NULL);
  state_wrapper_do_bool(sw, &card_present_in_state);

  if (!card_present_in_state)
  {
    if (state_wrapper_is_reading(sw) && s_pad.memory_cards[i])
    {
      WARNING_LOG("Memory card %u present in system but not in save state.", i + 1u);
      if (memory_card_reset)
        memory_card_reset(s_pad.memory_cards[i]);
    }
    return true;
  }

  if (!state_wrapper_do_marker(sw, "MemoryCard"))
    return false;

  if (s_pad.memory_cards[i] && memory_card_do_state)
    return memory_card_do_state(s_pad.memory_cards[i], sw);

  /* Card in state but absent (or memcard module not linked): we have no
   * dummy to drain into.  Mark error so the caller knows. */
  WARNING_LOG("Memory card %u present in state but not in system; cannot reload.", i + 1u);
  return false;
}

bool pad_do_state(state_wrapper_t* sw, bool is_memory_state)
{
  (void)is_memory_state;
  /* Without runahead support we always serialize the full set; the
   * runahead-aware path is documented in the header comment. */
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (!pad_do_state_controller(sw, i)) return false;
    if (!pad_do_state_memcard   (sw, i)) return false;
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
  {
    if (!multitap_do_state(&s_pad.multitaps[i], sw))
      return false;
  }

  u32 state_value = (u32)s_pad.state;
  state_wrapper_do_u32(sw, &state_value);
  s_pad.state = (pad_state_t)state_value;

  state_wrapper_do_u16(sw, &s_pad.joy_ctrl);
  state_wrapper_do_u32(sw, &s_pad.joy_stat);
  state_wrapper_do_u16(sw, &s_pad.joy_mode);
  state_wrapper_do_u16(sw, &s_pad.joy_baud);
  state_wrapper_do_u8 (sw, &s_pad.receive_buffer);
  state_wrapper_do_u8 (sw, &s_pad.transmit_buffer);
  state_wrapper_do_bool(sw, &s_pad.receive_buffer_full);
  state_wrapper_do_bool(sw, &s_pad.transmit_buffer_full);

  if (state_wrapper_is_reading(sw) && pad_is_transmitting())
    timing_event_activate(&s_pad.transfer_event);

  return !state_wrapper_has_error(sw);
}
