#include "core/multitap.h"
#include "core/controller.h"
#include "core/pad.h"

#include "util/state_wrapper.h"

#include "common/log.h"

#include <string.h>

LOG_CHANNEL(Multitap);

/* Constant header bytes for multitap responses: ID 0x80 + status 0x5A
 * (matches the SCPH-1070 transfer protocol).  */
#define MULTITAP_ID_BYTE     0x80u
#define MULTITAP_STATUS_BYTE 0x5Au

/* Forward decls for the memory-card transfer path; the memcard module is a
 * later port and we tolerate its absence by treating "no card" as 0xFF. */
typedef struct memory_card memory_card_t;
extern bool memory_card_transfer(memory_card_t* mc, u8 data_in, u8* data_out)
  __attribute__((weak));

void multitap_init(multitap_t* m)
{
  memset(m, 0, sizeof(*m));
  multitap_reset(m);
}

void multitap_reset(multitap_t* m)
{
  m->transfer_state = MULTITAP_TRANSFER_IDLE;
  m->selected_slot = 0;
  m->controller_transfer_step = 0;
  m->transfer_all_controllers = false;
  m->invalid_transfer_all_command = false;
  m->current_controller_done = false;
  memset(m->transfer_buffer, 0xFF, sizeof(m->transfer_buffer));
}

void multitap_set_enable(multitap_t* m, bool enable, u32 base_index)
{
  if (m->enabled != enable || m->base_index != base_index)
  {
    m->enabled = enable;
    m->base_index = base_index;
    multitap_reset(m);
  }
}

bool multitap_do_state(multitap_t* m, state_wrapper_t* sw)
{
  u8 ts = (u8)m->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  m->transfer_state = (multitap_transfer_state_t)ts;

  state_wrapper_do_u8(sw, &m->selected_slot);
  state_wrapper_do_u32(sw, &m->controller_transfer_step);
  state_wrapper_do_bool(sw, &m->invalid_transfer_all_command);
  state_wrapper_do_bool(sw, &m->transfer_all_controllers);
  state_wrapper_do_bool(sw, &m->current_controller_done);
  state_wrapper_do_bytes(sw, m->transfer_buffer, sizeof(m->transfer_buffer));

  return !state_wrapper_has_error(sw);
}

void multitap_reset_transfer_state(multitap_t* m)
{
  m->transfer_state = MULTITAP_TRANSFER_IDLE;
  m->selected_slot = 0;
  m->controller_transfer_step = 0;
  m->current_controller_done = false;
  /* Don't reset transfer_all_controllers here; it's queued for the next
   * transfer sequence.  Controller/memory-card transfer resets are handled
   * in pad_reset_device_transfer_state(). */
}

static bool transfer_controller(const multitap_t* m, u32 slot, u8 data_in, u8* data_out)
{
  const u32 pad_port = controller_convert_port_and_slot_to_pad(m->base_index, slot);
  controller_t* sel = pad_get_controller(pad_port);
  if (!sel)
  {
    *data_out = 0xFF;
    return false;
  }
  return controller_transfer(sel, data_in, data_out);
}

static bool transfer_memory_card(const multitap_t* m, u32 slot, u8 data_in, u8* data_out)
{
  const u32 pad_port = controller_convert_port_and_slot_to_pad(m->base_index, slot);
  memory_card_t* sel = pad_get_memory_card(pad_port);
  if (!sel || !memory_card_transfer)
  {
    *data_out = 0xFF;
    return false;
  }
  return memory_card_transfer(sel, data_in, data_out);
}

bool multitap_transfer(multitap_t* m, u8 data_in, u8* data_out)
{
  bool ack = false;
  switch (m->transfer_state)
  {
    case MULTITAP_TRANSFER_IDLE:
      switch (data_in)
      {
        case 0x81: case 0x82: case 0x83: case 0x84:
          /* Memory card access: low nibble - 1 = card slot. */
          m->selected_slot = (u8)((data_in & 0x0F) - 1u);
          ack = transfer_memory_card(m, m->selected_slot, 0x81, data_out);
          if (ack)
            m->transfer_state = MULTITAP_TRANSFER_MEMORY_CARD;
          break;

        case 0x01: case 0x02: case 0x03: case 0x04:
          /* Controller access. */
          m->selected_slot = (u8)(data_in - 1u);
          ack = transfer_controller(m, m->selected_slot, 0x01, data_out);
          if (ack)
          {
            m->transfer_state = MULTITAP_TRANSFER_CONTROLLER_COMMAND;
            if (m->transfer_all_controllers)
            {
              /* Send the access byte to the *other* slots so they all latch
               * into "I'm being addressed" state for the upcoming all-pads
               * read.  The replies are discarded. */
              u8 dummy;
              for (u32 i = 0; i < 4; i++)
              {
                if (i != m->selected_slot)
                  transfer_controller(m, i, 0x01, &dummy);
              }
            }
          }
          break;

        default:
          *data_out = 0xFF;
          ack = false;
          break;
      }
      break;

    case MULTITAP_TRANSFER_MEMORY_CARD:
      ack = transfer_memory_card(m, m->selected_slot, data_in, data_out);
      if (!ack)
      {
        DEV_LOG("Memory card transfer ended");
        m->transfer_state = MULTITAP_TRANSFER_IDLE;
      }
      break;

    case MULTITAP_TRANSFER_CONTROLLER_COMMAND:
      if (m->controller_transfer_step == 0)
      {
        if (m->transfer_all_controllers)
        {
          /* Other command bytes cause early aborts on real hardware; only
           * 0x42 (read pad) is valid in the all-controllers path. */
          *data_out = MULTITAP_ID_BYTE;
          m->invalid_transfer_all_command = (data_in != 0x42);
          ack = true;
        }
        else
        {
          ack = transfer_controller(m, m->selected_slot, data_in, data_out);
        }
        m->controller_transfer_step++;
      }
      else if (m->controller_transfer_step == 1)
      {
        if (m->transfer_all_controllers)
        {
          *data_out = MULTITAP_STATUS_BYTE;
          ack = !m->invalid_transfer_all_command;
          m->selected_slot = 0;
          m->transfer_state = MULTITAP_TRANSFER_ALL_CONTROLLERS;
        }
        else
        {
          ack = transfer_controller(m, m->selected_slot, 0x00, data_out);
          m->transfer_state = MULTITAP_TRANSFER_SINGLE_CONTROLLER;
        }
        /* Queue all-pads request for the next sequence.  Bit 0 of the
         * second command byte selects "all controllers" mode. */
        m->transfer_all_controllers = (data_in & 0x01) != 0;
        m->controller_transfer_step = 0;
      }
      break;

    case MULTITAP_TRANSFER_SINGLE_CONTROLLER:
      ack = transfer_controller(m, m->selected_slot, data_in, data_out);
      if (!ack)
      {
        DEV_LOG("Controller transfer ended");
        m->transfer_state = MULTITAP_TRANSFER_IDLE;
      }
      break;

    case MULTITAP_TRANSFER_ALL_CONTROLLERS:
      /* In this mode we transfer up to 8 bytes per controller; the hardware
       * is probably either latching the controller-info halfword count or
       * relying on a transfer timeout.  We stop when the controller stops
       * ACKing instead. */
      *data_out = m->transfer_buffer[m->controller_transfer_step];
      ack = true;

      if (m->current_controller_done)
        m->transfer_buffer[m->controller_transfer_step] = 0xFF;
      else
        m->current_controller_done =
          !transfer_controller(m, m->selected_slot, data_in, &m->transfer_buffer[m->controller_transfer_step]);

      m->controller_transfer_step++;
      if (m->controller_transfer_step % 8 == 0)
      {
        m->current_controller_done = false;
        m->selected_slot = (u8)((m->selected_slot + 1u) % 4u);
        if (m->selected_slot == 0)
          ack = false;
      }
      break;
  }
  return ack;
}
