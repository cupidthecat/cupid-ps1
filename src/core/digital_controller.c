#include "core/digital_controller.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

/* Digital pad reports (lo, hi) = 0x41 0x5A: lo nibble = 1 halfword of button
 * data, hi byte = constant ID 0x5A.  See the SCPH 1001 service manual or
 * Nocash psx-spx for the controller communication sequence. */
#define DIGITAL_CONTROLLER_ID UINT16_C(0x5A41)

/* Pop'n controller grounds the right/down/left direction lines. */
#define POPN_BUTTON_MASK \
  ((u16)~((1u << DIGITAL_CONTROLLER_BUTTON_RIGHT) |  \
          (1u << DIGITAL_CONTROLLER_BUTTON_DOWN)  |  \
          (1u << DIGITAL_CONTROLLER_BUTTON_LEFT))) 

static void  digital_controller_destroy(controller_t* base);
static controller_type_t digital_controller_get_type(const controller_t* base);
static void  digital_controller_reset(controller_t* base);
static bool  digital_controller_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state);
static void  digital_controller_reset_transfer_state(controller_t* base);
static bool  digital_controller_transfer(controller_t* base, u8 data_in, u8* data_out);
static float digital_controller_get_bind_state(const controller_t* base, u32 index);
static void  digital_controller_set_bind_state(controller_t* base, u32 index, float value);
static u32   digital_controller_get_button_state_bits(const controller_t* base);

static const controller_vtable_t s_digital_controller_vtable = {
   .destroy                = digital_controller_destroy,
  .get_type               = digital_controller_get_type,
  .reset                  = digital_controller_reset,
  .do_state               = digital_controller_do_state,
  .reset_transfer_state   = digital_controller_reset_transfer_state,
  .transfer               = digital_controller_transfer,
  .get_bind_state         = digital_controller_get_bind_state,
  .set_bind_state         = digital_controller_set_bind_state,
  .get_button_state_bits  = digital_controller_get_button_state_bits,
  .get_analog_input_bytes = NULL,
  .load_settings          = NULL, 
};

controller_t* digital_controller_create(u32 index, controller_type_t type)
{
  digital_controller_t* dc = (digital_controller_t*)calloc(1, sizeof(digital_controller_t));
  if (!dc) return NULL;

  dc->base.vtbl     = &s_digital_controller_vtable;
  dc->base.index    = index;
  dc->button_state  = UINT16_C(0xFFFF);
  dc->button_mask   = (type == CONTROLLER_TYPE_POPN_CONTROLLER) ? POPN_BUTTON_MASK : UINT16_C(0xFFFF);
  dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_IDLE;

  return &dc->base;
}

static void digital_controller_destroy(controller_t* base)
{
  free(base);
}

static controller_type_t digital_controller_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_DIGITAL_CONTROLLER;
}

static void digital_controller_reset(controller_t* base)
{
  digital_controller_t* dc = (digital_controller_t*)base;
  dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_IDLE;
}

static bool digital_controller_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  digital_controller_t* dc = (digital_controller_t*)base;

  u16 button_state = dc->button_state;
  state_wrapper_do_u16(sw, &button_state);
  if (apply_input_state)
    dc->button_state = button_state;

  u8 ts = (u8)dc->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  dc->transfer_state = (digital_controller_transfer_state_t)ts;
  return !state_wrapper_has_error(sw);
}

static float digital_controller_get_bind_state(const controller_t* base, u32 index)
{
  const digital_controller_t* dc = (const digital_controller_t*)base;
  if (index < (u32)DIGITAL_CONTROLLER_BUTTON_COUNT)
    return (float)(((dc->button_state >> index) & 1u) ^ 1u);
  return 0.0f;
}

static void digital_controller_set_bind_state(controller_t* base, u32 index, float value)
{
  digital_controller_t* dc = (digital_controller_t*)base;
  if (index >= (u32)DIGITAL_CONTROLLER_BUTTON_COUNT)
    return;

  const bool pressed = (value >= 0.5f);
  const u16 bit = (u16)1 << (u8)index;
  if (pressed)
    dc->button_state &= (u16)~bit;
  else
    dc->button_state |= bit;
}

static u32 digital_controller_get_button_state_bits(const controller_t* base)
{
  const digital_controller_t* dc = (const digital_controller_t*)base;
  /* Native data is active-low; flip for callers expecting "1 = pressed". */
  return dc->button_state ^ 0xFFFFu;
}

static void digital_controller_reset_transfer_state(controller_t* base)
{
  digital_controller_t* dc = (digital_controller_t*)base;
  dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_IDLE;
}

static bool digital_controller_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  digital_controller_t* dc = (digital_controller_t*)base;

  /* Standard SIO0 controller sequence:
   *   0x01 -> ACK (controller selected)
   *   0x42 -> respond with ID lo byte (0x41), continue
   *          -> respond with ID hi byte (0x5A), continue
   *          -> respond with button bits LSB, continue
   *          -> respond with button bits MSB, end (no ACK)
   * Any other byte at Idle/Ready returns 0xFF / no-ACK. */
  switch (dc->transfer_state)
  {
    case DIGITAL_CONTROLLER_TRANSFER_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01)
      {
        dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_READY;
        return true;
      }
      return false;

    case DIGITAL_CONTROLLER_TRANSFER_READY:
      if (data_in == 0x42)
      {
        *data_out = Truncate8(DIGITAL_CONTROLLER_ID);
        dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_ID_MSB;
        return true;
      }
      *data_out = 0xFF;
      return false;

    case DIGITAL_CONTROLLER_TRANSFER_ID_MSB:
      *data_out = Truncate8(DIGITAL_CONTROLLER_ID >> 8);
      dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_BUTTONS_LSB;
      return true;

    case DIGITAL_CONTROLLER_TRANSFER_BUTTONS_LSB:
      *data_out = Truncate8(dc->button_state & dc->button_mask);
      dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_BUTTONS_MSB;
      return true;

    case DIGITAL_CONTROLLER_TRANSFER_BUTTONS_MSB:
      *data_out = Truncate8((u16)(dc->button_state & dc->button_mask) >> 8);
      dc->transfer_state = DIGITAL_CONTROLLER_TRANSFER_IDLE;
      return false;
  }

  /* unreachable */
  *data_out = 0xFF;
  return false;
}

#define BTN(label, idx, gen)                                                     \
  { .name = label, .display_name = label, .icon_name = NULL,                     \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                 \
    .generic_mapping = (gen) }

static const controller_binding_info_t s_digital_bindings[] = {
  BTN("Up",       DIGITAL_CONTROLLER_BUTTON_UP,       GENERIC_INPUT_BINDING_DPAD_UP),
  BTN("Right",    DIGITAL_CONTROLLER_BUTTON_RIGHT,    GENERIC_INPUT_BINDING_DPAD_RIGHT),
  BTN("Down",     DIGITAL_CONTROLLER_BUTTON_DOWN,     GENERIC_INPUT_BINDING_DPAD_DOWN),
  BTN("Left",     DIGITAL_CONTROLLER_BUTTON_LEFT,     GENERIC_INPUT_BINDING_DPAD_LEFT),
  BTN("Triangle", DIGITAL_CONTROLLER_BUTTON_TRIANGLE, GENERIC_INPUT_BINDING_TRIANGLE),
  BTN("Circle",   DIGITAL_CONTROLLER_BUTTON_CIRCLE,   GENERIC_INPUT_BINDING_CIRCLE),
  BTN("Cross",    DIGITAL_CONTROLLER_BUTTON_CROSS,    GENERIC_INPUT_BINDING_CROSS),
  BTN("Square",   DIGITAL_CONTROLLER_BUTTON_SQUARE,   GENERIC_INPUT_BINDING_SQUARE),
  BTN("Select",   DIGITAL_CONTROLLER_BUTTON_SELECT,   GENERIC_INPUT_BINDING_SELECT),
  BTN("Start",    DIGITAL_CONTROLLER_BUTTON_START,    GENERIC_INPUT_BINDING_START),
  BTN("L1",       DIGITAL_CONTROLLER_BUTTON_L1,       GENERIC_INPUT_BINDING_L1),
  BTN("R1",       DIGITAL_CONTROLLER_BUTTON_R1,       GENERIC_INPUT_BINDING_R1),
  BTN("L2",       DIGITAL_CONTROLLER_BUTTON_L2,       GENERIC_INPUT_BINDING_L2),
  BTN("R2",       DIGITAL_CONTROLLER_BUTTON_R2,       GENERIC_INPUT_BINDING_R2),
};

static const controller_binding_info_t s_popn_bindings[] = {
  BTN("LeftWhite",   DIGITAL_CONTROLLER_BUTTON_TRIANGLE, GENERIC_INPUT_BINDING_TRIANGLE),
  BTN("LeftYellow",  DIGITAL_CONTROLLER_BUTTON_CIRCLE,   GENERIC_INPUT_BINDING_CIRCLE),
  BTN("LeftGreen",   DIGITAL_CONTROLLER_BUTTON_R1,       GENERIC_INPUT_BINDING_R1),
  BTN("LeftBlue",    DIGITAL_CONTROLLER_BUTTON_CROSS,    GENERIC_INPUT_BINDING_CROSS),
  BTN("MiddleRed",   DIGITAL_CONTROLLER_BUTTON_L1,       GENERIC_INPUT_BINDING_L1),
  BTN("RightBlue",   DIGITAL_CONTROLLER_BUTTON_SQUARE,   GENERIC_INPUT_BINDING_SQUARE),
  BTN("RightGreen",  DIGITAL_CONTROLLER_BUTTON_R2,       GENERIC_INPUT_BINDING_R2),
  BTN("RightYellow", DIGITAL_CONTROLLER_BUTTON_UP,       GENERIC_INPUT_BINDING_DPAD_UP),
  BTN("RightWhite",  DIGITAL_CONTROLLER_BUTTON_L2,       GENERIC_INPUT_BINDING_L2),
  BTN("Select",      DIGITAL_CONTROLLER_BUTTON_SELECT,   GENERIC_INPUT_BINDING_SELECT),
  BTN("Start",       DIGITAL_CONTROLLER_BUTTON_START,    GENERIC_INPUT_BINDING_START),
};

#undef BTN

const controller_info_t g_digital_controller_info = {
   .type           = CONTROLLER_TYPE_DIGITAL_CONTROLLER,
  .name           = "DigitalController",
  .display_name   = "Digital Controller",
  .icon_name      = NULL,
  .bindings       = s_digital_bindings,
  .bindings_count = sizeof(s_digital_bindings) / sizeof(s_digital_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

const controller_info_t g_popn_controller_info = {
   .type           = CONTROLLER_TYPE_POPN_CONTROLLER,
  .name           = "PopnController",
  .display_name   = "Pop'n Controller",
  .icon_name      = NULL,
  .bindings       = s_popn_bindings,
  .bindings_count = sizeof(s_popn_bindings) / sizeof(s_popn_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};
