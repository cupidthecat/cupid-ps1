/*
 *

 * lever transition counters and the virtual-button -> level mapping survive
 * verbatim.
 */
#include "core/ddgo_controller.h"

#include "common/log.h"
#include "common/settings_interface.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  DDGO_TRANSFER_IDLE = 0,
  DDGO_TRANSFER_READY,
  DDGO_TRANSFER_ID_MSB,
  DDGO_TRANSFER_BUTTONS_LSB,
  DDGO_TRANSFER_BUTTONS_MSB,
} ddgo_transfer_state_t;

#define DDGO_MAX_POWER_LEVEL 5u
#define DDGO_MAX_BRAKE_LEVEL 9u

 /* Mask of the digital-pad bits we don't physically expose: bits 4 and 6
 * (D-pad up / D-pad down on a regular pad). */
#define DDGO_BUTTON_MASK ((u16)~((1u << 4) | (1u << 6)))

#define DDGO_POWER_MASK \
  ((u16)((1u << DDGO_BIND_POWER_BIT0) | (1u << DDGO_BIND_POWER_BIT1) | (1u << DDGO_BIND_POWER_BIT2)))
#define DDGO_BRAKE_MASK \
  ((u16)((1u << DDGO_BIND_BRAKE_BIT0) | (1u << DDGO_BIND_BRAKE_BIT1) | \
         (1u << DDGO_BIND_BRAKE_BIT2) | (1u << DDGO_BIND_BRAKE_BIT3))) 

typedef struct {
  controller_t base;

  u16 button_state;     /* active low */

  ddgo_transfer_state_t transfer_state;

  u8  power_level;
  u8  brake_level;

  u8  power_transition_frames_remaining;
  u8  brake_transition_frames_remaining;

  u8  power_transition_frames;
  u8  brake_transition_frames;

  float analog_deadzone;
  float analog_sensitivity;
} ddgo_controller_t;

static const controller_vtable_t s_dd_vtable;

static inline float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline u32 minu32(u32 a, u32 b) { return (a < b) ? a : b; }

#define POWER_BITS(b0, b1, b2)                                                     \
  ((u16)(~(((b0) ? (1u << DDGO_BIND_POWER_BIT0) : 0u) |                            \
           ((b1) ? (1u << DDGO_BIND_POWER_BIT1) : 0u) |                            \
           ((b2) ? (1u << DDGO_BIND_POWER_BIT2) : 0u)) & DDGO_POWER_MASK)) 

static const u16 s_power_table[DDGO_MAX_POWER_LEVEL + 2u] = {
  POWER_BITS(0, 1, 1),  /* N  */
  POWER_BITS(1, 0, 1),  /* P1 */
  POWER_BITS(0, 0, 1),  /* P2 */
  POWER_BITS(1, 1, 0),  /* P3 */
  POWER_BITS(0, 1, 0),  /* P4 */
  POWER_BITS(1, 0, 0),  /* P5 */
  POWER_BITS(0, 0, 0),  /* Transition */
};
#undef POWER_BITS

#define BRAKE_BITS(b0, b1, b2, b3)                                                 \
  ((u16)(~(((b0) ? (1u << DDGO_BIND_BRAKE_BIT0) : 0u) |                            \
           ((b1) ? (1u << DDGO_BIND_BRAKE_BIT1) : 0u) |                            \
           ((b2) ? (1u << DDGO_BIND_BRAKE_BIT2) : 0u) |                            \
           ((b3) ? (1u << DDGO_BIND_BRAKE_BIT3) : 0u)) & DDGO_BRAKE_MASK)) 

static const u16 s_brake_table[DDGO_MAX_BRAKE_LEVEL + 2u] = {
  BRAKE_BITS(0, 1, 1, 1), /* Released  */
  BRAKE_BITS(1, 0, 1, 1), /* B1 */
  BRAKE_BITS(0, 0, 1, 1), /* B2 */
  BRAKE_BITS(1, 1, 0, 1), /* B3 */
  BRAKE_BITS(0, 1, 0, 1), /* B4 */
  BRAKE_BITS(1, 0, 0, 1), /* B5 */
  BRAKE_BITS(0, 0, 0, 1), /* B6 */
  BRAKE_BITS(1, 1, 1, 0), /* B7 */
  BRAKE_BITS(0, 1, 1, 0), /* B8 */
  BRAKE_BITS(0, 0, 0, 0), /* Emergency */
  BRAKE_BITS(1, 1, 1, 1), /* Transition */
};
#undef BRAKE_BITS

static void dd_update_power_bits(ddgo_controller_t* dd)
{
  const u32 idx = (dd->power_transition_frames_remaining > 0) ? (DDGO_MAX_POWER_LEVEL + 1u) : (u32)dd->power_level;
  dd->button_state = (u16)((dd->button_state & ~DDGO_POWER_MASK) | s_power_table[idx]);
}

static void dd_update_brake_bits(ddgo_controller_t* dd)
{
  const u32 idx = (dd->brake_transition_frames_remaining > 0) ? (DDGO_MAX_BRAKE_LEVEL + 1u) : (u32)dd->brake_level;
  dd->button_state = (u16)((dd->button_state & ~DDGO_BRAKE_MASK) | s_brake_table[idx]);
}

static void dd_set_power_level(ddgo_controller_t* dd, u32 level)
{
  if (dd->power_level == level) return;
  dd->power_level = (u8)level;
  dd->power_transition_frames_remaining = dd->power_transition_frames;
  dd_update_power_bits(dd);
}

static void dd_set_brake_level(ddgo_controller_t* dd, u32 level)
{
  if (dd->brake_level == level) return;
  dd->brake_level = (u8)level;
  dd->brake_transition_frames_remaining = dd->brake_transition_frames;
  dd_update_brake_bits(dd);
}

controller_t* ddgo_controller_create(u32 index)
{
  ddgo_controller_t* dd = (ddgo_controller_t*)calloc(1, sizeof(*dd));
  if (!dd) return NULL;
  dd->base.vtbl  = &s_dd_vtable;
  dd->base.index = index;
  dd->button_state = 0xFFFFu;
  dd->brake_level  = (u8)DDGO_MAX_BRAKE_LEVEL;
  dd->power_transition_frames = 10;
  dd->brake_transition_frames = 10;
  dd->analog_deadzone    = CONTROLLER_DEFAULT_STICK_DEADZONE;
  dd->analog_sensitivity = CONTROLLER_DEFAULT_STICK_SENSITIVITY;
  /* Seed the lever bits to match the initial level. */
  dd_update_power_bits(dd);
  dd_update_brake_bits(dd);
  return &dd->base;
}

static void dd_destroy(controller_t* base) { free(base); }

static controller_type_t dd_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_DDGO_CONTROLLER;
}

static void dd_reset(controller_t* base)
{
  ddgo_controller_t* dd = (ddgo_controller_t*)base;
  dd->transfer_state = DDGO_TRANSFER_IDLE;
  dd->power_transition_frames_remaining = 0;
  dd_update_power_bits(dd);
  dd->brake_transition_frames_remaining = 0;
  dd_update_brake_bits(dd);
}

static void dd_reset_transfer_state(controller_t* base)
{
  ((ddgo_controller_t*)base)->transfer_state = DDGO_TRANSFER_IDLE;
}

static bool dd_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  ddgo_controller_t* dd = (ddgo_controller_t*)base;

  u16 button_state = dd->button_state;
  u8  power_level  = dd->power_level;
  u8  power_xframe = dd->power_transition_frames_remaining;
  u8  brake_level  = dd->brake_level;
  u8  brake_xframe = dd->brake_transition_frames_remaining;
  state_wrapper_do_u16(sw, &button_state);
  state_wrapper_do_u8 (sw, &power_level);
  state_wrapper_do_u8 (sw, &power_xframe);
  state_wrapper_do_u8 (sw, &brake_level);
  state_wrapper_do_u8 (sw, &brake_xframe);

  if (apply_input_state) {
    dd->button_state = button_state;
    dd->power_level  = power_level;
    dd->power_transition_frames_remaining = power_xframe;
    dd->brake_level  = brake_level;
    dd->brake_transition_frames_remaining = brake_xframe;
    dd_update_power_bits(dd);
    dd_update_brake_bits(dd);
  }

  u8 ts = (u8)dd->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  dd->transfer_state = (ddgo_transfer_state_t)ts;
  return true;
}

static float dd_get_bind_state(const controller_t* base, u32 index)
{
  const ddgo_controller_t* dd = (const ddgo_controller_t*)base;
  if (index >= (u32)DDGO_BIND_VIRTUAL_BUTTON_START) {
    if (index < (u32)DDGO_BIND_VIRTUAL_BRAKE_RELEASED)
      return (dd->power_level == (index - (u32)DDGO_BIND_VIRTUAL_POWER_OFF)) ? 1.0f : 0.0f;
    return (dd->brake_level == (index - (u32)DDGO_BIND_VIRTUAL_BRAKE_RELEASED)) ? 1.0f : 0.0f;
  }
  /* Only report the user-controllable buttons (Start/Select/A/B/C). */
  static const u16 REPORT_MASK =
      (u16)((1u << DDGO_BIND_START) | (1u << DDGO_BIND_SELECT) |
            (1u << DDGO_BIND_A)     | (1u << DDGO_BIND_B)      | 
            (1u << DDGO_BIND_C));
  return (float)((((dd->button_state ^ 0xFFFFu) & REPORT_MASK) >> index) & 1u);
}

static void dd_set_bind_state(controller_t* base, u32 index, float value)
{
  ddgo_controller_t* dd = (ddgo_controller_t*)base;

  if (index == (u32)DDGO_BIND_POWER) {
    value = (value < dd->analog_deadzone) ? 0.0f : (value * dd->analog_sensitivity);
    dd_set_power_level(dd, minu32((u32)(value * (float)DDGO_MAX_POWER_LEVEL), DDGO_MAX_POWER_LEVEL));
    return;
  }
  if (index == (u32)DDGO_BIND_BRAKE) {
    value = (value < dd->analog_deadzone) ? 0.0f : (value * dd->analog_sensitivity);
    dd_set_brake_level(dd, minu32((u32)(value * (float)DDGO_MAX_BRAKE_LEVEL), DDGO_MAX_BRAKE_LEVEL));
    return;
  }
  if (index >= (u32)DDGO_BIND_VIRTUAL_BUTTON_START) {
    if (value < 0.5f) return;     /* press only */
    if (index < (u32)DDGO_BIND_VIRTUAL_BRAKE_RELEASED)
      dd_set_power_level(dd, index - (u32)DDGO_BIND_VIRTUAL_POWER_OFF);
    else
      dd_set_brake_level(dd, index - (u32)DDGO_BIND_VIRTUAL_BRAKE_RELEASED);
    return;
  }

  const bool pressed = (value >= 0.5f);
  const u16 bit = (u16)(1u << (u8)index);
  if (pressed) dd->button_state &= (u16)~bit;
  else         dd->button_state |= bit;
}

static u32 dd_get_button_state_bits(const controller_t* base)
{
  return (u32)(((const ddgo_controller_t*)base)->button_state ^ 0xFFFFu);
}

static bool dd_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  ddgo_controller_t* dd = (ddgo_controller_t*)base;
  static const u16 ID = 0x5A41u;

  switch (dd->transfer_state) {
    case DDGO_TRANSFER_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01) {
        dd->transfer_state = DDGO_TRANSFER_READY;
        if (dd->power_transition_frames_remaining > 0) {
          dd->power_transition_frames_remaining--;
          dd_update_power_bits(dd);
        }
        if (dd->brake_transition_frames_remaining > 0) {
          dd->brake_transition_frames_remaining--;
          dd_update_brake_bits(dd);
        }
        return true;
      }
      return false;

    case DDGO_TRANSFER_READY:
      if (data_in == 0x42) {
        *data_out = (u8)ID;
        dd->transfer_state = DDGO_TRANSFER_ID_MSB;
        return true;
      }
      *data_out = 0xFF;
      return false;

    case DDGO_TRANSFER_ID_MSB:
      *data_out = (u8)(ID >> 8);
      dd->transfer_state = DDGO_TRANSFER_BUTTONS_LSB;
      return true;

    case DDGO_TRANSFER_BUTTONS_LSB:
      *data_out = (u8)(dd->button_state & DDGO_BUTTON_MASK);
      dd->transfer_state = DDGO_TRANSFER_BUTTONS_MSB;
      return true;

    case DDGO_TRANSFER_BUTTONS_MSB:
      *data_out = (u8)((dd->button_state & DDGO_BUTTON_MASK) >> 8);
      dd->transfer_state = DDGO_TRANSFER_IDLE;
      return false;
  }
  return false;
}

static void dd_load_settings(controller_t* base, const settings_interface_t* si,
                             const char* section, bool initial)
{
  (void)initial;
  ddgo_controller_t* dd = (ddgo_controller_t*)base;
  dd->analog_deadzone = clampf(
      settings_interface_get_float_value(si, section, "AnalogDeadzone",   CONTROLLER_DEFAULT_STICK_DEADZONE),    0.0f, 1.0f);
  dd->analog_sensitivity = clampf(
      settings_interface_get_float_value(si, section, "AnalogSensitivity", CONTROLLER_DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  dd->power_transition_frames = (u8)settings_interface_get_int_value(si, section, "PowerTransitionFrames", 0);
  dd->brake_transition_frames = (u8)settings_interface_get_int_value(si, section, "BrakeTransitionFrames", 0);
}

#define BTN(label, idx, gen)                                                       \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                   \
    .generic_mapping = (gen) }
#define HAXIS(label, idx, gen)                                                     \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_HALF_AXIS,                \
    .generic_mapping = (gen) }

static const controller_binding_info_t s_dd_bindings[] = {
  BTN("Select", DDGO_BIND_SELECT, GENERIC_INPUT_BINDING_SELECT),
  BTN("Start",  DDGO_BIND_START,  GENERIC_INPUT_BINDING_START),
  BTN("A",      DDGO_BIND_A,      GENERIC_INPUT_BINDING_SQUARE),
  BTN("B",      DDGO_BIND_B,      GENERIC_INPUT_BINDING_CROSS),
  BTN("C",      DDGO_BIND_C,      GENERIC_INPUT_BINDING_CIRCLE),

  BTN("VirtualPowerOff",       DDGO_BIND_VIRTUAL_POWER_OFF,       GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualPower1",         DDGO_BIND_VIRTUAL_POWER_1,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualPower2",         DDGO_BIND_VIRTUAL_POWER_2,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualPower3",         DDGO_BIND_VIRTUAL_POWER_3,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualPower4",         DDGO_BIND_VIRTUAL_POWER_4,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualPower5",         DDGO_BIND_VIRTUAL_POWER_5,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrakeReleased",  DDGO_BIND_VIRTUAL_BRAKE_RELEASED,  GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake1",         DDGO_BIND_VIRTUAL_BRAKE_1,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake2",         DDGO_BIND_VIRTUAL_BRAKE_2,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake3",         DDGO_BIND_VIRTUAL_BRAKE_3,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake4",         DDGO_BIND_VIRTUAL_BRAKE_4,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake5",         DDGO_BIND_VIRTUAL_BRAKE_5,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake6",         DDGO_BIND_VIRTUAL_BRAKE_6,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake7",         DDGO_BIND_VIRTUAL_BRAKE_7,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrake8",         DDGO_BIND_VIRTUAL_BRAKE_8,         GENERIC_INPUT_BINDING_UNKNOWN),
  BTN("VirtualBrakeEmergency", DDGO_BIND_VIRTUAL_BRAKE_EMERGENCY, GENERIC_INPUT_BINDING_UNKNOWN),

  HAXIS("Power", DDGO_BIND_POWER, GENERIC_INPUT_BINDING_L2),
  HAXIS("Brake", DDGO_BIND_BRAKE, GENERIC_INPUT_BINDING_R2),
};

#undef BTN
#undef HAXIS

const controller_info_t g_ddgo_controller_info = {
   .type           = CONTROLLER_TYPE_DDGO_CONTROLLER,
  .name           = "DDGoController",
  .display_name   = "Densha de Go! Controller",
  .icon_name      = NULL,
  .bindings       = s_dd_bindings,
  .bindings_count = sizeof(s_dd_bindings) / sizeof(s_dd_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

static const controller_vtable_t s_dd_vtable = {
   .destroy                = dd_destroy,
  .get_type               = dd_get_type,
  .reset                  = dd_reset,
  .do_state               = dd_do_state,
  .reset_transfer_state   = dd_reset_transfer_state,
  .transfer               = dd_transfer,
  .get_bind_state         = dd_get_bind_state,
  .set_bind_state         = dd_set_bind_state,
  .get_button_state_bits  = dd_get_button_state_bits,
  .get_analog_input_bytes = NULL,
  .load_settings          = dd_load_settings, 
};
