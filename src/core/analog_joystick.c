#include "core/analog_joystick.h"
#include "core/system.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  AJ_TRANSFER_IDLE = 0,
  AJ_TRANSFER_READY,
  AJ_TRANSFER_ID_MSB,
  AJ_TRANSFER_BUTTONS_LSB,
  AJ_TRANSFER_BUTTONS_MSB,
  AJ_TRANSFER_RIGHT_AXIS_X,
  AJ_TRANSFER_RIGHT_AXIS_Y,
  AJ_TRANSFER_LEFT_AXIS_X,
  AJ_TRANSFER_LEFT_AXIS_Y,
} aj_transfer_state_t;

typedef struct {
  controller_t base;

  float analog_deadzone;
  float analog_sensitivity;
  u8    invert_left_stick;
  u8    invert_right_stick;

  bool  analog_mode;

  u16   button_state;        /* active low */
  u8    axis_state    [ANALOG_JOYSTICK_AXIS_COUNT];
  u8    half_axis_state[ANALOG_JOYSTICK_HALFAXIS_COUNT];

  aj_transfer_state_t transfer_state;
} analog_joystick_t;

static const controller_vtable_t s_aj_vtable;

static inline float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static u16 aj_get_id(const analog_joystick_t* aj)
{
  return aj->analog_mode ? 0x5A53u : 0x5A41u;
}

static void aj_toggle_analog_mode(analog_joystick_t* aj)
{
  aj->analog_mode = !aj->analog_mode;
  INFO_LOG("Joystick %u switched to %s mode.",
           aj->base.index + 1u, aj->analog_mode ? "analog" : "digital");
}

controller_t* analog_joystick_create(u32 index)
{
  analog_joystick_t* aj = (analog_joystick_t*)calloc(1, sizeof(*aj));
  if (!aj) return NULL;

  aj->base.vtbl  = &s_aj_vtable;
  aj->base.index = index;

  aj->analog_sensitivity = CONTROLLER_DEFAULT_STICK_SENSITIVITY;
  aj->analog_deadzone    = CONTROLLER_DEFAULT_STICK_DEADZONE;
  for (u32 i = 0; i < ANALOG_JOYSTICK_AXIS_COUNT; i++)
    aj->axis_state[i] = 0x80;
  aj->button_state = (u16)0xFFFFu;
  aj->analog_mode  = true;
  return &aj->base;
}

static void aj_destroy(controller_t* base) { free(base); }

static controller_type_t aj_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_ANALOG_JOYSTICK;
}

static void aj_reset(controller_t* base)
{
  analog_joystick_t* aj = (analog_joystick_t*)base;
  aj->transfer_state = AJ_TRANSFER_IDLE;
  aj->analog_mode    = true;
}

static void aj_reset_transfer_state(controller_t* base)
{
  ((analog_joystick_t*)base)->transfer_state = AJ_TRANSFER_IDLE;
}

static bool aj_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  analog_joystick_t* aj = (analog_joystick_t*)base;
  state_wrapper_do_bool(sw, &aj->analog_mode);

  u16 button_state = aj->button_state;
  u8  axis_state[ANALOG_JOYSTICK_AXIS_COUNT];
  memcpy(axis_state, aj->axis_state, sizeof(axis_state));
  state_wrapper_do_u16  (sw, &button_state);
  state_wrapper_do_bytes(sw, axis_state, sizeof(axis_state));
  if (apply_input_state) {
    aj->button_state = button_state;
    memcpy(aj->axis_state, axis_state, sizeof(axis_state));
  }

  u8 ts = (u8)aj->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  aj->transfer_state = (aj_transfer_state_t)ts;
  return true;
}

static float aj_get_bind_state(const controller_t* base, u32 index)
{
  const analog_joystick_t* aj = (const analog_joystick_t*)base;
  if (index >= ANALOG_JOYSTICK_LED_BIND_START_INDEX)
    return (index == ANALOG_JOYSTICK_LED_BIND_START_INDEX && aj->analog_mode) ? 1.0f : 0.0f;
  if (index >= ANALOG_JOYSTICK_HALFAXIS_BIND_START_INDEX) {
    const u32 sub = index - ANALOG_JOYSTICK_HALFAXIS_BIND_START_INDEX;
    if (sub >= ANALOG_JOYSTICK_HALFAXIS_COUNT) return 0.0f;
    return (float)aj->half_axis_state[sub] * (1.0f / 255.0f);
  }
  if (index < (u32)ANALOG_JOYSTICK_BUTTON_MODE)
    return (float)(((aj->button_state >> index) & 1u) ^ 1u);
  return 0.0f;
}

#define HALFAXIS_VAL(axis) (aj->half_axis_state[(u32)(axis)])

static u8 merge_halfaxes(const analog_joystick_t* aj, u32 pos, u32 neg)
{
  if (HALFAXIS_VAL(pos) != 0)
    return (u8)(127u + ((HALFAXIS_VAL(pos) + 1u) / 2u));
  return (u8)(127u - (HALFAXIS_VAL(neg) / 2u));
}

static float merge_halfaxes_f(const analog_joystick_t* aj, u32 pos, u32 neg)
{
  if (HALFAXIS_VAL(pos) != 0)
    return (float)HALFAXIS_VAL(pos) / 255.0f;
  return (float)HALFAXIS_VAL(neg) / -255.0f;
}

static void aj_set_bind_state(controller_t* base, u32 index, float value)
{
  analog_joystick_t* aj = (analog_joystick_t*)base;

  if (index == (u32)ANALOG_JOYSTICK_BUTTON_MODE) {
    if (value >= 0.5f) aj_toggle_analog_mode(aj);
    return;
  }

  if (index >= (u32)ANALOG_JOYSTICK_BUTTON_COUNT) {
    const u32 sub = index - (u32)ANALOG_JOYSTICK_BUTTON_COUNT;
    if (sub >= ANALOG_JOYSTICK_HALFAXIS_COUNT) return;

    const float scaled = value * aj->analog_sensitivity * 255.0f;
    const u8 u8val = (u8)clampf(scaled, 0.0f, 255.0f);
    if (aj->half_axis_state[sub] == u8val) return;
    aj->half_axis_state[sub] = u8val;

    /* Merge half-axis pairs into the 8-bit signed-style axis_state byte
     * (centered at 127), respecting per-stick invert masks. */
    switch ((analog_joystick_halfaxis_t)sub) {
      case ANALOG_JOYSTICK_HALFAXIS_L_LEFT:
      case ANALOG_JOYSTICK_HALFAXIS_L_RIGHT:
         aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_X] = (aj->invert_left_stick & 1u)
          ? merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_L_LEFT,  ANALOG_JOYSTICK_HALFAXIS_L_RIGHT) 
          : merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_L_RIGHT, ANALOG_JOYSTICK_HALFAXIS_L_LEFT);
        break;
      case ANALOG_JOYSTICK_HALFAXIS_L_DOWN:
      case ANALOG_JOYSTICK_HALFAXIS_L_UP:
         aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_Y] = (aj->invert_left_stick & 2u)
          ? merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_L_UP,   ANALOG_JOYSTICK_HALFAXIS_L_DOWN) 
          : merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_L_DOWN, ANALOG_JOYSTICK_HALFAXIS_L_UP);
        break;
      case ANALOG_JOYSTICK_HALFAXIS_R_LEFT:
      case ANALOG_JOYSTICK_HALFAXIS_R_RIGHT:
         aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_X] = (aj->invert_right_stick & 1u)
          ? merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_R_LEFT,  ANALOG_JOYSTICK_HALFAXIS_R_RIGHT) 
          : merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_R_RIGHT, ANALOG_JOYSTICK_HALFAXIS_R_LEFT);
        break;
      case ANALOG_JOYSTICK_HALFAXIS_R_DOWN:
      case ANALOG_JOYSTICK_HALFAXIS_R_UP:
         aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_Y] = (aj->invert_right_stick & 2u)
          ? merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_R_UP,   ANALOG_JOYSTICK_HALFAXIS_R_DOWN) 
          : merge_halfaxes(aj, ANALOG_JOYSTICK_HALFAXIS_R_DOWN, ANALOG_JOYSTICK_HALFAXIS_R_UP);
        break;
      default: break;
    }

    if (aj->analog_deadzone > 0.0f) {
      float pos_x, pos_y;
      const bool left = (sub < (u32)ANALOG_JOYSTICK_HALFAXIS_R_LEFT);
      if (left) {
        pos_x = (aj->invert_left_stick & 1u)
          ? merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_L_LEFT,  ANALOG_JOYSTICK_HALFAXIS_L_RIGHT)
          : merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_L_RIGHT, ANALOG_JOYSTICK_HALFAXIS_L_LEFT);
        pos_y = (aj->invert_left_stick & 2u)
          ? merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_L_UP,   ANALOG_JOYSTICK_HALFAXIS_L_DOWN)
          : merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_L_DOWN, ANALOG_JOYSTICK_HALFAXIS_L_UP);
      } else {
        pos_x = (aj->invert_right_stick & 1u)
          ? merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_R_LEFT,  ANALOG_JOYSTICK_HALFAXIS_R_RIGHT)
          : merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_R_RIGHT, ANALOG_JOYSTICK_HALFAXIS_R_LEFT);
        pos_y = (aj->invert_right_stick & 2u)
          ? merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_R_UP,   ANALOG_JOYSTICK_HALFAXIS_R_DOWN)
          : merge_halfaxes_f(aj, ANALOG_JOYSTICK_HALFAXIS_R_DOWN, ANALOG_JOYSTICK_HALFAXIS_R_UP);
      }
      if (controller_in_circular_deadzone(aj->analog_deadzone, pos_x, pos_y)) {
        if (left) {
          aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_X] =
          aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_Y] = 127;
        } else {
          aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_X] =
          aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_Y] = 127;
        }
      }
    }
    return;
  }

  /* Plain digital button. */
  const u16 bit = (u16)(1u << index);
  if (value >= 0.5f) aj->button_state &= (u16)~bit;
  else               aj->button_state |= bit;
}

static u32 aj_get_button_state_bits(const controller_t* base)
{
  const analog_joystick_t* aj = (const analog_joystick_t*)base;
  return (u32)(aj->button_state ^ (u16)0xFFFFu);
}

static bool aj_get_analog_input_bytes(const controller_t* base, u32* out_bytes)
{
  const analog_joystick_t* aj = (const analog_joystick_t*)base;
  *out_bytes = ((u32)aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_Y]  << 24) |
               ((u32)aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_X]  << 16) |
               ((u32)aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_Y] << 8)  | 
                (u32)aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_X];
  return true;
}

static bool aj_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  analog_joystick_t* aj = (analog_joystick_t*)base;
  switch (aj->transfer_state) {
    case AJ_TRANSFER_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01) { aj->transfer_state = AJ_TRANSFER_READY; return true; }
      return false;

    case AJ_TRANSFER_READY:
      if (data_in == 0x42) {
        *data_out = (u8)aj_get_id(aj);
        aj->transfer_state = AJ_TRANSFER_ID_MSB;
        return true;
      }
      *data_out = 0xFF;
      return false;

    case AJ_TRANSFER_ID_MSB:
      *data_out = (u8)(aj_get_id(aj) >> 8);
      aj->transfer_state = AJ_TRANSFER_BUTTONS_LSB;
      return true;

    case AJ_TRANSFER_BUTTONS_LSB:
      *data_out = (u8)aj->button_state;
      aj->transfer_state = AJ_TRANSFER_BUTTONS_MSB;
      return true;

    case AJ_TRANSFER_BUTTONS_MSB:
      *data_out = (u8)(aj->button_state >> 8);
      aj->transfer_state = aj->analog_mode ? AJ_TRANSFER_RIGHT_AXIS_X : AJ_TRANSFER_IDLE;
      return aj->analog_mode;

    case AJ_TRANSFER_RIGHT_AXIS_X:
      *data_out = aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_X];
      aj->transfer_state = AJ_TRANSFER_RIGHT_AXIS_Y;
      return true;

    case AJ_TRANSFER_RIGHT_AXIS_Y:
      *data_out = aj->axis_state[ANALOG_JOYSTICK_AXIS_RIGHT_Y];
      aj->transfer_state = AJ_TRANSFER_LEFT_AXIS_X;
      return true;

    case AJ_TRANSFER_LEFT_AXIS_X:
      *data_out = aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_X];
      aj->transfer_state = AJ_TRANSFER_LEFT_AXIS_Y;
      return true;

    case AJ_TRANSFER_LEFT_AXIS_Y:
      *data_out = aj->axis_state[ANALOG_JOYSTICK_AXIS_LEFT_Y];
      aj->transfer_state = AJ_TRANSFER_IDLE;
      return false;
  }
  return false;
}

static void aj_load_settings(controller_t* base, const settings_interface_t* si,
                             const char* section, bool initial)
{
  (void)initial;
  analog_joystick_t* aj = (analog_joystick_t*)base;
  aj->analog_deadzone = clampf(
      settings_interface_get_float_value(si, section, "AnalogDeadzone",   CONTROLLER_DEFAULT_STICK_DEADZONE),    0.0f, 1.0f);
  aj->analog_sensitivity = clampf(
      settings_interface_get_float_value(si, section, "AnalogSensitivity", CONTROLLER_DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  aj->invert_left_stick  = (u8)settings_interface_get_uint_value(si, section, "InvertLeftStick",  0u);
  aj->invert_right_stick = (u8)settings_interface_get_uint_value(si, section, "InvertRightStick", 0u);
}

#define BTN(label, idx, gen)                                                              \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                          \
    .generic_mapping = (gen) }
#define AXIS(label, idx, gen)                                                             \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = ANALOG_JOYSTICK_HALFAXIS_BIND_START_INDEX + (u32)(idx),                 \
    .type = INPUT_BINDING_TYPE_HALF_AXIS, .generic_mapping = (gen) }
#define LED(label, idx, gen)                                                              \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = ANALOG_JOYSTICK_LED_BIND_START_INDEX + (u32)(idx),                      \
    .type = INPUT_BINDING_TYPE_LED, .generic_mapping = (gen) }

static const controller_binding_info_t s_aj_bindings[] = {
  BTN("Up",       ANALOG_JOYSTICK_BUTTON_UP,       GENERIC_INPUT_BINDING_DPAD_UP),
  BTN("Right",    ANALOG_JOYSTICK_BUTTON_RIGHT,    GENERIC_INPUT_BINDING_DPAD_RIGHT),
  BTN("Down",     ANALOG_JOYSTICK_BUTTON_DOWN,     GENERIC_INPUT_BINDING_DPAD_DOWN),
  BTN("Left",     ANALOG_JOYSTICK_BUTTON_LEFT,     GENERIC_INPUT_BINDING_DPAD_LEFT),
  BTN("Triangle", ANALOG_JOYSTICK_BUTTON_TRIANGLE, GENERIC_INPUT_BINDING_TRIANGLE),
  BTN("Circle",   ANALOG_JOYSTICK_BUTTON_CIRCLE,   GENERIC_INPUT_BINDING_CIRCLE),
  BTN("Cross",    ANALOG_JOYSTICK_BUTTON_CROSS,    GENERIC_INPUT_BINDING_CROSS),
  BTN("Square",   ANALOG_JOYSTICK_BUTTON_SQUARE,   GENERIC_INPUT_BINDING_SQUARE),
  BTN("Select",   ANALOG_JOYSTICK_BUTTON_SELECT,   GENERIC_INPUT_BINDING_SELECT),
  BTN("Start",    ANALOG_JOYSTICK_BUTTON_START,    GENERIC_INPUT_BINDING_START),
  BTN("Mode",     ANALOG_JOYSTICK_BUTTON_MODE,     GENERIC_INPUT_BINDING_SYSTEM),
  BTN("L1",       ANALOG_JOYSTICK_BUTTON_L1,       GENERIC_INPUT_BINDING_L1),
  BTN("R1",       ANALOG_JOYSTICK_BUTTON_R1,       GENERIC_INPUT_BINDING_R1),
  BTN("L2",       ANALOG_JOYSTICK_BUTTON_L2,       GENERIC_INPUT_BINDING_L2),
  BTN("R2",       ANALOG_JOYSTICK_BUTTON_R2,       GENERIC_INPUT_BINDING_R2),
  BTN("L3",       ANALOG_JOYSTICK_BUTTON_L3,       GENERIC_INPUT_BINDING_L3),
  BTN("R3",       ANALOG_JOYSTICK_BUTTON_R3,       GENERIC_INPUT_BINDING_R3),

  AXIS("LLeft",   ANALOG_JOYSTICK_HALFAXIS_L_LEFT,  GENERIC_INPUT_BINDING_LEFT_STICK_LEFT),
  AXIS("LRight",  ANALOG_JOYSTICK_HALFAXIS_L_RIGHT, GENERIC_INPUT_BINDING_LEFT_STICK_RIGHT),
  AXIS("LDown",   ANALOG_JOYSTICK_HALFAXIS_L_DOWN,  GENERIC_INPUT_BINDING_LEFT_STICK_DOWN),
  AXIS("LUp",     ANALOG_JOYSTICK_HALFAXIS_L_UP,    GENERIC_INPUT_BINDING_LEFT_STICK_UP),
  AXIS("RLeft",   ANALOG_JOYSTICK_HALFAXIS_R_LEFT,  GENERIC_INPUT_BINDING_RIGHT_STICK_LEFT),
  AXIS("RRight",  ANALOG_JOYSTICK_HALFAXIS_R_RIGHT, GENERIC_INPUT_BINDING_RIGHT_STICK_RIGHT),
  AXIS("RDown",   ANALOG_JOYSTICK_HALFAXIS_R_DOWN,  GENERIC_INPUT_BINDING_RIGHT_STICK_DOWN),
  AXIS("RUp",     ANALOG_JOYSTICK_HALFAXIS_R_UP,    GENERIC_INPUT_BINDING_RIGHT_STICK_UP),

  LED("ModeLED",  0, GENERIC_INPUT_BINDING_MODE_LED),
};

#undef BTN
#undef AXIS
#undef LED

const controller_info_t g_analog_joystick_info = {
   .type           = CONTROLLER_TYPE_ANALOG_JOYSTICK,
  .name           = "AnalogJoystick",
  .display_name   = "Analog Joystick",
  .icon_name      = NULL,
  .bindings       = s_aj_bindings,
  .bindings_count = sizeof(s_aj_bindings) / sizeof(s_aj_bindings[0]), 
  .settings       = NULL,   /* settings_count = 0; defaults are seeded in load_settings */
  .settings_count = 0,
};

static const controller_vtable_t s_aj_vtable = {
   .destroy                = aj_destroy,
  .get_type               = aj_get_type,
  .reset                  = aj_reset,
  .do_state               = aj_do_state,
  .reset_transfer_state   = aj_reset_transfer_state,
  .transfer               = aj_transfer,
  .get_bind_state         = aj_get_bind_state,
  .set_bind_state         = aj_set_bind_state,
  .get_button_state_bits  = aj_get_button_state_bits,
  .get_analog_input_bytes = aj_get_analog_input_bytes,
  .load_settings          = aj_load_settings, 
};
