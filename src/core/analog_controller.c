#include "core/analog_controller.h"

#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/settings_interface.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

#define LARGE_MOTOR 1u
#define SMALL_MOTOR 0u

#define DEFAULT_LARGE_MOTOR_VIBRATION_BIAS 8
#define DEFAULT_SMALL_MOTOR_VIBRATION_BIAS 8

static void  analog_controller_destroy(controller_t* base);
static controller_type_t analog_controller_get_type(const controller_t* base);
static void  analog_controller_reset(controller_t* base);
static bool  analog_controller_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state);
static void  analog_controller_reset_transfer_state(controller_t* base);
static bool  analog_controller_transfer(controller_t* base, u8 data_in, u8* data_out);
static float analog_controller_get_bind_state(const controller_t* base, u32 index);
static void  analog_controller_set_bind_state(controller_t* base, u32 index, float value);
static u32   analog_controller_get_button_state_bits(const controller_t* base);
static bool  analog_controller_get_analog_input_bytes(const controller_t* base, u32* out);
static void  analog_controller_load_settings(controller_t* base, const settings_interface_t* si,
                                             const char* section, bool initial);

static void set_analog_mode(analog_controller_t* ac, bool enabled);
static void process_analog_mode_toggle(analog_controller_t* ac);
static void set_motor_state(analog_controller_t* ac, u32 motor, u8 value);
static float get_motor_strength(const analog_controller_t* ac, u32 motor);
static u16 get_extra_button_mask(const analog_controller_t* ac);
static void reset_rumble_config(analog_controller_t* ac);
static void poll_state_into_tx(analog_controller_t* ac);
static u8 get_response_num_halfwords(const analog_controller_t* ac);
static u8 get_mode_id(const analog_controller_t* ac);
static u8 get_id_byte(const analog_controller_t* ac);

static const controller_vtable_t s_analog_controller_vtable = {
   .destroy                = analog_controller_destroy,
  .get_type               = analog_controller_get_type,
  .reset                  = analog_controller_reset,
  .do_state               = analog_controller_do_state,
  .reset_transfer_state   = analog_controller_reset_transfer_state,
  .transfer               = analog_controller_transfer,
  .get_bind_state         = analog_controller_get_bind_state,
  .set_bind_state         = analog_controller_set_bind_state,
  .get_button_state_bits  = analog_controller_get_button_state_bits,
  .get_analog_input_bytes = analog_controller_get_analog_input_bytes,
  .load_settings          = analog_controller_load_settings, 
};

controller_t* analog_controller_create(u32 index)
{
  analog_controller_t* ac = (analog_controller_t*)calloc(1, sizeof(analog_controller_t));
  if (!ac) return NULL;

  ac->base.vtbl  = &s_analog_controller_vtable;
  ac->base.index = index;

  ac->status_byte = 0x5A;
  for (u32 i = 0; i < (u32)ANALOG_CONTROLLER_AXIS_COUNT; i++)
    ac->axis_state[i] = 0x80;
  for (u32 i = 0; i < ANALOG_CONTROLLER_RUMBLE_CONFIG_LEN; i++)
    ac->rumble_config[i] = 0xFF;

  ac->analog_sensitivity        = CONTROLLER_DEFAULT_STICK_SENSITIVITY;
  ac->analog_deadzone           = CONTROLLER_DEFAULT_STICK_DEADZONE;
  ac->button_deadzone           = CONTROLLER_DEFAULT_BUTTON_DEADZONE;
  ac->vibration_bias[0]         = DEFAULT_LARGE_MOTOR_VIBRATION_BIAS;
  ac->vibration_bias[1]         = DEFAULT_SMALL_MOTOR_VIBRATION_BIAS;
  ac->force_analog_on_reset     = true;
  ac->analog_dpad_in_digital_mode = true;

  ac->button_state = UINT16_C(0xFFFF);
  return &ac->base;
}

static void analog_controller_destroy(controller_t* base)
{
  free(base);
}

static controller_type_t analog_controller_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_ANALOG_CONTROLLER;
}

static void analog_controller_reset(controller_t* base)
{
  analog_controller_t* ac = (analog_controller_t*)base;
  ac->command = ANALOG_CONTROLLER_CMD_IDLE;
  ac->command_step = 0;
  memset(ac->rx_buffer, 0, sizeof(ac->rx_buffer));
  memset(ac->tx_buffer, 0, sizeof(ac->tx_buffer));
  ac->analog_mode = false;
  ac->configuration_mode = false;

  for (u32 i = 0; i < ANALOG_CONTROLLER_NUM_MOTORS; i++)
  {
    if (ac->motor_state[i] != 0)
      set_motor_state(ac, i, 0);
  }

  ac->dualshock_enabled = false;
  reset_rumble_config(ac);
  ac->status_byte = 0x5A;

  if (ac->force_analog_on_reset && controller_can_start_in_analog_mode(CONTROLLER_TYPE_ANALOG_CONTROLLER))
    set_analog_mode(ac, true);
}

static bool analog_controller_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  analog_controller_t* ac = (analog_controller_t*)base;

  u8 motor_state_buf[ANALOG_CONTROLLER_NUM_MOTORS];
  memcpy(motor_state_buf, ac->motor_state, sizeof(motor_state_buf));

  /* We only support v76+ save states (matches the rest of the C port).  Older
   * blobs would route through a now-removed legacy migration path. */
  u8 cmd = (u8)ac->command;
  state_wrapper_do_u8(sw, &cmd);
  ac->command = (analog_controller_command_t)cmd;

  state_wrapper_do_u8(sw, &ac->command_step);
  state_wrapper_do_u8(sw, &ac->response_length);
  state_wrapper_do_bytes(sw, ac->rx_buffer, sizeof(ac->rx_buffer));
  state_wrapper_do_bytes(sw, ac->tx_buffer, sizeof(ac->tx_buffer));
  state_wrapper_do_bool(sw, &ac->analog_mode);
  state_wrapper_do_bool(sw, &ac->analog_locked);
  state_wrapper_do_bool(sw, &ac->dualshock_enabled);
  state_wrapper_do_bool(sw, &ac->configuration_mode);
  state_wrapper_do_bytes(sw, ac->rumble_config, sizeof(ac->rumble_config));
  state_wrapper_do_u8(sw, &ac->status_byte);

  u8 axis_state_buf[ANALOG_CONTROLLER_AXIS_COUNT];
  memcpy(axis_state_buf, ac->axis_state, sizeof(axis_state_buf));
  u16 button_state = ac->button_state;

  state_wrapper_do_bytes(sw, axis_state_buf, sizeof(axis_state_buf));
  state_wrapper_do_u16(sw, &button_state);
  state_wrapper_do_bytes(sw, motor_state_buf, sizeof(motor_state_buf));

  if (apply_input_state)
  {
    memcpy(ac->axis_state, axis_state_buf, sizeof(axis_state_buf));
    ac->button_state = button_state;
  }

  if (state_wrapper_is_reading(sw))
  {
    for (u8 i = 0; i < ANALOG_CONTROLLER_NUM_MOTORS; i++)
      set_motor_state(ac, i, motor_state_buf[i]);
  }

  return !state_wrapper_has_error(sw);
}

static float analog_controller_get_bind_state(const controller_t* base, u32 index)
{
  const analog_controller_t* ac = (const analog_controller_t*)base;

  if (index >= ANALOG_CONTROLLER_LED_BIND_START_INDEX)
    return BoolToFloat(index == ANALOG_CONTROLLER_LED_BIND_START_INDEX && ac->analog_mode);
  if (index >= ANALOG_CONTROLLER_MOTOR_BIND_START_INDEX)
    return get_motor_strength(ac, index - ANALOG_CONTROLLER_MOTOR_BIND_START_INDEX);
  if (index >= ANALOG_CONTROLLER_HALFAXIS_BIND_START_INDEX)
    return (float)ac->half_axis_state[index - ANALOG_CONTROLLER_HALFAXIS_BIND_START_INDEX] * (1.0f / 255.0f);
  if (index < (u32)ANALOG_CONTROLLER_BUTTON_ANALOG)
    return (float)(((ac->button_state >> index) & 1u) ^ 1u);
  return 0.0f;
}

/* clamp helper (no <algorithm>) */
static ALWAYS_INLINE float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static void analog_controller_set_bind_state(controller_t* base, u32 index, float value)
{
  analog_controller_t* ac = (analog_controller_t*)base;

  if (index == (u32)ANALOG_CONTROLLER_BUTTON_ANALOG)
  {
    /* Analog toggle: queue if a transfer is in progress so we don't change
     * the response halfword count mid-packet. */
    if (value >= ac->button_deadzone)
    {
      if (ac->command == ANALOG_CONTROLLER_CMD_IDLE)
        process_analog_mode_toggle(ac);
      else
        ac->analog_toggle_queued = true;
    }
    return;
  }

  if (index >= (u32)ANALOG_CONTROLLER_BUTTON_COUNT)
  {
    const u32 sub_index = index - (u32)ANALOG_CONTROLLER_BUTTON_COUNT;
    if (sub_index >= (u32)ANALOG_CONTROLLER_HALF_AXIS_COUNT)
      return;

    const float scaled = value * ac->analog_sensitivity * 255.0f;
    const u8 u8_value = (u8)clampf(scaled, 0.0f, 255.0f);
    if (u8_value == ac->half_axis_state[sub_index])
      return;
    ac->half_axis_state[sub_index] = u8_value;

#define HAS(h) (ac->half_axis_state[(u32)(h)] != 0)
#define MERGE(pos, neg)                                                                   \
  (HAS(pos) ? (u8)(127u + ((ac->half_axis_state[(u32)(pos)] + 1u) / 2u))                  \
            : (u8)(127u - (ac->half_axis_state[(u32)(neg)] / 2u))) 

    switch ((analog_controller_half_axis_t)sub_index)
    {
      case ANALOG_CONTROLLER_HALF_AXIS_LLEFT:
      case ANALOG_CONTROLLER_HALF_AXIS_LRIGHT:
         ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_X] =
          ((ac->invert_left_stick & 1u) != 0u) ? MERGE(ANALOG_CONTROLLER_HALF_AXIS_LLEFT, ANALOG_CONTROLLER_HALF_AXIS_LRIGHT) 
                                               : MERGE(ANALOG_CONTROLLER_HALF_AXIS_LRIGHT, ANALOG_CONTROLLER_HALF_AXIS_LLEFT);
        break;
      case ANALOG_CONTROLLER_HALF_AXIS_LDOWN:
      case ANALOG_CONTROLLER_HALF_AXIS_LUP:
         ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_Y] =
          ((ac->invert_left_stick & 2u) != 0u) ? MERGE(ANALOG_CONTROLLER_HALF_AXIS_LUP, ANALOG_CONTROLLER_HALF_AXIS_LDOWN) 
                                               : MERGE(ANALOG_CONTROLLER_HALF_AXIS_LDOWN, ANALOG_CONTROLLER_HALF_AXIS_LUP);
        break;
      case ANALOG_CONTROLLER_HALF_AXIS_RLEFT:
      case ANALOG_CONTROLLER_HALF_AXIS_RRIGHT:
         ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X] =
          ((ac->invert_right_stick & 1u) != 0u) ? MERGE(ANALOG_CONTROLLER_HALF_AXIS_RLEFT, ANALOG_CONTROLLER_HALF_AXIS_RRIGHT) 
                                                : MERGE(ANALOG_CONTROLLER_HALF_AXIS_RRIGHT, ANALOG_CONTROLLER_HALF_AXIS_RLEFT);
        break;
      case ANALOG_CONTROLLER_HALF_AXIS_RDOWN:
      case ANALOG_CONTROLLER_HALF_AXIS_RUP:
         ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_Y] =
          ((ac->invert_right_stick & 2u) != 0u) ? MERGE(ANALOG_CONTROLLER_HALF_AXIS_RUP, ANALOG_CONTROLLER_HALF_AXIS_RDOWN) 
                                                : MERGE(ANALOG_CONTROLLER_HALF_AXIS_RDOWN, ANALOG_CONTROLLER_HALF_AXIS_RUP);
        break;
      default:
        break;
    }

    if (ac->analog_deadzone > 0.0f)
    {
#define MERGE_F(pos, neg)                                                                 \
  (HAS(pos) ? ((float)ac->half_axis_state[(u32)(pos)] /  255.0f)                          \
            : ((float)ac->half_axis_state[(u32)(neg)] / -255.0f)) 

      float pos_x, pos_y;
      if ((analog_controller_half_axis_t)sub_index < ANALOG_CONTROLLER_HALF_AXIS_RLEFT)
      {
        pos_x = ((ac->invert_left_stick & 1u) != 0u) ? MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_LLEFT, ANALOG_CONTROLLER_HALF_AXIS_LRIGHT)
                                                     : MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_LRIGHT, ANALOG_CONTROLLER_HALF_AXIS_LLEFT);
        pos_y = ((ac->invert_left_stick & 2u) != 0u) ? MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_LUP, ANALOG_CONTROLLER_HALF_AXIS_LDOWN)
                                                     : MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_LDOWN, ANALOG_CONTROLLER_HALF_AXIS_LUP);
      }
      else
      {
        pos_x = ((ac->invert_right_stick & 1u) != 0u) ? MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_RLEFT, ANALOG_CONTROLLER_HALF_AXIS_RRIGHT)
                                                      : MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_RRIGHT, ANALOG_CONTROLLER_HALF_AXIS_RLEFT);
        pos_y = ((ac->invert_right_stick & 2u) != 0u) ? MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_RUP, ANALOG_CONTROLLER_HALF_AXIS_RDOWN)
                                                      : MERGE_F(ANALOG_CONTROLLER_HALF_AXIS_RDOWN, ANALOG_CONTROLLER_HALF_AXIS_RUP);
      }

      if (controller_in_circular_deadzone(ac->analog_deadzone, pos_x, pos_y))
      {
        if ((analog_controller_half_axis_t)sub_index < ANALOG_CONTROLLER_HALF_AXIS_RLEFT)
          ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_X] = ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_Y] = 127;
        else
          ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X] = ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_Y] = 127;
      }

#undef MERGE_F
    }
#undef MERGE
#undef HAS
    return;
  }

  const u16 bit = (u16)1 << (u8)index;
  if (value >= ac->button_deadzone)
    ac->button_state &= (u16)~bit;
  else
    ac->button_state |= bit;
}

static u32 analog_controller_get_button_state_bits(const controller_t* base)
{
  const analog_controller_t* ac = (const analog_controller_t*)base;
  return ac->button_state ^ 0xFFFFu;
}

static bool analog_controller_get_analog_input_bytes(const controller_t* base, u32* out)
{
  const analog_controller_t* ac = (const analog_controller_t*)base;
  if (!out) return false;
  *out = ((u32)ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_Y] << 24) |
         ((u32)ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_X] << 16) |
         ((u32)ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_Y] << 8) | 
         ((u32)ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X]);
  return true;
}

static void analog_controller_reset_transfer_state(controller_t* base)
{
  analog_controller_t* ac = (analog_controller_t*)base;

  if (ac->analog_toggle_queued)
  {
    process_analog_mode_toggle(ac);
    ac->analog_toggle_queued = false;
  }
  ac->command = ANALOG_CONTROLLER_CMD_IDLE;
  ac->command_step = 0;
}

static void set_analog_mode(analog_controller_t* ac, bool enabled)
{
  if (ac->analog_mode == enabled)
    return;
  ac->analog_mode = enabled;
  INFO_LOG("Controller %u switched to %s mode.", ac->base.index + 1u, enabled ? "analog" : "digital");
}

static void process_analog_mode_toggle(analog_controller_t* ac)
{
  if (ac->analog_locked)
  {
    DEV_LOG("Controller %u is locked to %s mode.", ac->base.index + 1u, ac->analog_mode ? "analog" : "digital");
    return;
  }

  /* Refuse digital->analog engage when GameDB says the running game can't
   * speak DualShock (e.g. Crash Bandicoot 1996 rejects ID 0x73 and locks out
   * input).  Always allow analog->digital so the user can recover. */
  if (!ac->analog_mode && !controller_can_start_in_analog_mode(CONTROLLER_TYPE_ANALOG_CONTROLLER))
  {
    INFO_LOG("Controller %u: refusing analog mode (game does not support it per GameDB).",
             ac->base.index + 1u);
    return;
  }

  set_analog_mode(ac, !ac->analog_mode);
  reset_rumble_config(ac);

  /* Status byte serves as a "mode change happened" signal to the game; clear
   * it here when in dualshock mode so the next ReadPad reflects the change. */
  if (ac->dualshock_enabled)
  {
    /* Some games (Tomb Raider's loader) put the pad into config mode but not
     * analog mode, so a user-initiated digital->analog flip would leave the
     * status byte stuck at 0.  Reset config/dualshock state when the title
     * isn't on the dualshock-supported list. */
    if (!ac->analog_mode && !controller_can_start_in_analog_mode(CONTROLLER_TYPE_ANALOG_CONTROLLER))
    {
      WARNING_LOG("Resetting pad on digital->analog switch.");
      ac->configuration_mode = false;
      ac->dualshock_enabled = false;
      ac->status_byte = 0x5A;
    }
    else
    {
      ac->status_byte = 0x00;
    }
  }
}

static void set_motor_state(analog_controller_t* ac, u32 motor, u8 value)
{
  DebugAssert(motor < ANALOG_CONTROLLER_NUM_MOTORS);
  if (ac->motor_state[motor] != value)
  {
    ac->motor_state[motor] = value;
    const float hvalue = get_motor_strength(ac, motor);
    DEV_LOG("Set %s motor to %f (raw %u)", (motor == LARGE_MOTOR) ? "large" : "small",
            (double)hvalue, (unsigned)ac->motor_state[motor]);
    input_manager_set_pad_vibration_intensity(ac->base.index, ANALOG_CONTROLLER_MOTOR_BIND_START_INDEX + motor, hvalue);
  }
}

static float get_motor_strength(const analog_controller_t* ac, u32 motor)
{
  /* Small motor is only on/off (its lowest bit drives the rumble line). */
  const u8 state = (motor == SMALL_MOTOR) ? (((ac->motor_state[SMALL_MOTOR] & 0x01) != 0) ? (u8)255 : (u8)0)
                                          : ac->motor_state[LARGE_MOTOR];

  /* Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210 */
  s32 biased = (s32)state + ac->vibration_bias[motor];
  if (biased < 0) biased = 0;
  if (biased > 255) biased = 255;
  const double x = (double)biased;
  const double strength = 0.006474549734772402 * x * x * x
                        - 1.258165252213538     * x * x
                        + 156.82454281087692    * x 
                        + 3.637978807091713e-11;
  return (state != 0) ? (float)(strength / 65535.0) : 0.0f;
}

static u16 get_extra_button_mask(const analog_controller_t* ac)
{
  u16 mask = 0xFFFF;

  const u8 NEG_THRESHOLD = (u8)(128.0f - (127.0 * 0.5f));
  const u8 POS_THRESHOLD = (u8)(128.0f + (127.0 * 0.5f));

  if (ac->analog_dpad_in_digital_mode && !ac->analog_mode && !ac->configuration_mode)
  {
    const bool left  = (ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_X] <= NEG_THRESHOLD);
    const bool right = (ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_X] >= POS_THRESHOLD);
    const bool up    = (ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_Y] <= NEG_THRESHOLD);
    const bool down  = (ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_Y] >= POS_THRESHOLD);
    mask = (u16)~((u16)((left  ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_LEFT)  |
                  (u16)((right ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_RIGHT) |
                  (u16)((up    ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_UP)    | 
                  (u16)((down  ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_DOWN));
  }

  if (ac->analog_shoulder_buttons == 2 || (ac->analog_shoulder_buttons == 1 && !ac->analog_mode && !ac->configuration_mode))
  {
    const bool left  = (ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X] <= NEG_THRESHOLD);
    const bool right = (ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X] >= POS_THRESHOLD);
    mask &= (u16)~((u16)((left  ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_L1) |
                   (u16)((right ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_R1));
  }

  if (ac->analog_trigger_buttons == 2 || (ac->analog_trigger_buttons == 1 && !ac->analog_mode && !ac->configuration_mode))
  {
    const bool left  = (ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X] <= NEG_THRESHOLD);
    const bool right = (ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X] >= POS_THRESHOLD);
    mask &= (u16)~((u16)((left  ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_L2) |
                   (u16)((right ? 1u : 0u) << ANALOG_CONTROLLER_BUTTON_R2));
  }

  return mask;
}

static void reset_rumble_config(analog_controller_t* ac)
{
  for (u32 i = 0; i < ANALOG_CONTROLLER_RUMBLE_CONFIG_LEN; i++)
    ac->rumble_config[i] = 0xFF;
  set_motor_state(ac, SMALL_MOTOR, 0);
  set_motor_state(ac, LARGE_MOTOR, 0);
}

static u8 get_response_num_halfwords(const analog_controller_t* ac)
{
  if (ac->configuration_mode || ac->analog_mode)
    return 0x3;
  return (u8)(0x1 + ac->digital_mode_extra_halfwords);
}

static u8 get_mode_id(const analog_controller_t* ac)
{
  if (ac->configuration_mode) return 0xF;
  if (ac->analog_mode) return 0x7;
  return 0x4;
}

static u8 get_id_byte(const analog_controller_t* ac)
{
  return (u8)((get_mode_id(ac) << 4) | get_response_num_halfwords(ac));
}

static void poll_state_into_tx(analog_controller_t* ac)
{
  ac->tx_buffer[0] = get_id_byte(ac);
  ac->tx_buffer[1] = ac->status_byte;

  const u16 button_state = ac->button_state & get_extra_button_mask(ac);
  ac->tx_buffer[2] = Truncate8(button_state);
  ac->tx_buffer[3] = Truncate8(button_state >> 8);

  if (ac->analog_mode || ac->configuration_mode)
  {
    ac->tx_buffer[4] = ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_X];
    ac->tx_buffer[5] = ac->axis_state[ANALOG_CONTROLLER_AXIS_RIGHT_Y];
    ac->tx_buffer[6] = ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_X];
    ac->tx_buffer[7] = ac->axis_state[ANALOG_CONTROLLER_AXIS_LEFT_Y];
  }
  else
  {
    ac->tx_buffer[4] = 0;
    ac->tx_buffer[5] = 0;
    ac->tx_buffer[6] = 0;
    ac->tx_buffer[7] = 0;
  }
}

/* Helper: refill tx_buffer with a 8-byte literal "{id, status, b2..b7}" in
 * the order the SCEI config-mode replies dictate. */
static ALWAYS_INLINE void load_tx_pattern(analog_controller_t* ac, u8 b2, u8 b3, u8 b4, u8 b5, u8 b6, u8 b7)
{
  ac->tx_buffer[0] = get_id_byte(ac);
  ac->tx_buffer[1] = ac->status_byte;
  ac->tx_buffer[2] = b2;
  ac->tx_buffer[3] = b3;
  ac->tx_buffer[4] = b4;
  ac->tx_buffer[5] = b5;
  ac->tx_buffer[6] = b6;
  ac->tx_buffer[7] = b7;
}

static bool analog_controller_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  analog_controller_t* ac = (analog_controller_t*)base;
  ac->rx_buffer[ac->command_step] = data_in;

  switch (ac->command)
  {
    case ANALOG_CONTROLLER_CMD_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01)
      {
        ac->command = ANALOG_CONTROLLER_CMD_READY;
        return true;
      }
      return false;

    case ANALOG_CONTROLLER_CMD_READY:
      if (data_in == 0x42)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_READ_PAD;
        poll_state_into_tx(ac);
      }
      else if (data_in == 0x43)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_CONFIG_MODE_SET_MODE;
        if (!ac->configuration_mode)
          poll_state_into_tx(ac);
        else
          load_tx_pattern(ac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
      }
      else if (ac->configuration_mode && data_in == 0x44)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_SET_ANALOG_MODE;
        load_tx_pattern(ac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
        reset_rumble_config(ac);
      }
      else if (ac->configuration_mode && data_in == 0x45)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_GET_ANALOG_MODE;
        /* Fixed reply: {0x01, 0x02, analog_mode, 0x02, 0x01, 0x00}. */
        load_tx_pattern(ac, 0x01, 0x02, BoolToUInt8(ac->analog_mode), 0x02, 0x01, 0x00);
      }
      else if (ac->configuration_mode && data_in == 0x46)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_46;
        load_tx_pattern(ac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
      }
      else if (ac->configuration_mode && data_in == 0x47)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_47;
        load_tx_pattern(ac, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00);
      }
      else if (ac->configuration_mode && data_in == 0x4C)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_4C;
        load_tx_pattern(ac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
      }
      else if (ac->configuration_mode && data_in == 0x4D)
      {
        ac->response_length = (u8)((get_response_num_halfwords(ac) + 1) * 2);
        ac->command = ANALOG_CONTROLLER_CMD_GET_SET_RUMBLE;
        load_tx_pattern(ac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
      }
      else
      {
        if (ac->configuration_mode)
          ERROR_LOG("Unimplemented config mode command 0x%02X", data_in);
        *data_out = 0xFF;
        return false;
      }
      break;

    case ANALOG_CONTROLLER_CMD_READ_PAD:
      if (ac->dualshock_enabled)
      {
        if (ac->command_step >= 2 && ac->command_step < 7)
        {
          /* Each rumble_config byte names which motor (or 0xFF for "none")
           * receives the corresponding TX byte during a read. */
          const u8 motor_to_set = ac->rumble_config[ac->command_step - 2];
          if (motor_to_set <= LARGE_MOTOR)
            set_motor_state(ac, motor_to_set, data_in);
        }
      }
      else if (ac->command_step == 3)
      {
        /* Legacy rumble: ((rx[2] & 0xC0) == 0x40) && (rx[3] & 1) toggles the
         * small motor.  This is what Konami-era games use before unlocking
         * dualshock mode. */
        const bool legacy_rumble_on = (ac->rx_buffer[2] & 0xC0) == 0x40 && (ac->rx_buffer[3] & 0x01) != 0;
        set_motor_state(ac, SMALL_MOTOR, legacy_rumble_on ? (u8)255 : (u8)0);
      }
      break;

    case ANALOG_CONTROLLER_CMD_CONFIG_MODE_SET_MODE:
      if (ac->command_step == (s32)ac->response_length - 1)
      {
        ac->configuration_mode = (ac->rx_buffer[2] == 1);
        if (ac->configuration_mode)
        {
          ac->dualshock_enabled = true;
          ac->status_byte = 0x5A;
        }
        DEBUG_LOG("0x%02x(%s) config mode", ac->rx_buffer[2], ac->configuration_mode ? "enter" : "leave");
      }
      break;

    case ANALOG_CONTROLLER_CMD_SET_ANALOG_MODE:
      if (ac->command_step == 2)
      {
        if (data_in == 0x00 || data_in == 0x01)
          set_analog_mode(ac, (data_in == 0x01));
      }
      else if (ac->command_step == 3)
      {
        if (data_in == 0x02 || data_in == 0x03)
          ac->analog_locked = (data_in == 0x03);
      }
      break;

    case ANALOG_CONTROLLER_CMD_GET_ANALOG_MODE:
      /* Reply was preloaded into tx_buffer when the command was first seen. */
      break;

    case ANALOG_CONTROLLER_CMD_46:
      if (ac->command_step == 2)
      {
        if (data_in == 0x00)
        {
          ac->tx_buffer[4] = 0x01; ac->tx_buffer[5] = 0x02; ac->tx_buffer[6] = 0x00; ac->tx_buffer[7] = 0x0A;
        }
        else if (data_in == 0x01)
        {
          ac->tx_buffer[4] = 0x01; ac->tx_buffer[5] = 0x01; ac->tx_buffer[6] = 0x01; ac->tx_buffer[7] = 0x14;
        }
      }
      break;

    case ANALOG_CONTROLLER_CMD_47:
      if (ac->command_step == 2 && data_in != 0x00)
      {
        ac->tx_buffer[4] = 0x00; ac->tx_buffer[5] = 0x00; ac->tx_buffer[6] = 0x00; ac->tx_buffer[7] = 0x00;
      }
      break;

    case ANALOG_CONTROLLER_CMD_4C:
      if (ac->command_step == 2)
      {
        if (data_in == 0x00) ac->tx_buffer[5] = 0x04;
        else if (data_in == 0x01) ac->tx_buffer[5] = 0x07;
      }
      break;

    case ANALOG_CONTROLLER_CMD_GET_SET_RUMBLE:
      if (ac->command_step >= 2 && ac->command_step < 7)
      {
        const u8 idx = (u8)(ac->command_step - 2);
        ac->tx_buffer[ac->command_step] = ac->rumble_config[idx];
        ac->rumble_config[idx] = data_in;
        if (data_in == LARGE_MOTOR)
          DEBUG_LOG("Large motor mapped to byte index %u", (unsigned)idx);
        else if (data_in == SMALL_MOTOR)
          DEBUG_LOG("Small motor mapped to byte index %u", (unsigned)idx);
      }
      else if (ac->command_step == 7)
      {
        bool has_small = false, has_large = false;
        for (u32 i = 0; i < ANALOG_CONTROLLER_RUMBLE_CONFIG_LEN; i++)
        {
          if (ac->rumble_config[i] == SMALL_MOTOR) has_small = true;
          if (ac->rumble_config[i] == LARGE_MOTOR) has_large = true;
        }
        if (!has_small) set_motor_state(ac, SMALL_MOTOR, 0);
        if (!has_large) set_motor_state(ac, LARGE_MOTOR, 0);
      }
      break;
  }

  *data_out = ac->tx_buffer[ac->command_step];
  ac->command_step = (u8)((ac->command_step + 1) % ac->response_length);
  const bool ack = (ac->command_step != 0);

  if (ac->command_step == 0)
  {
    ac->command = ANALOG_CONTROLLER_CMD_IDLE;
    memset(ac->rx_buffer, 0, sizeof(ac->rx_buffer));
    memset(ac->tx_buffer, 0, sizeof(ac->tx_buffer));
  }

  return ack;
}

static void analog_controller_load_settings(controller_t* base, const settings_interface_t* si,
                                            const char* section, bool initial)
{
  (void)initial;
  analog_controller_t* ac = (analog_controller_t*)base;

  ac->force_analog_on_reset       = settings_interface_get_bool_value(si, section, "ForceAnalogOnReset", true);
  ac->analog_dpad_in_digital_mode = settings_interface_get_bool_value(si, section, "AnalogDPadInDigitalMode", true);
  ac->analog_shoulder_buttons     = (u8)settings_interface_get_uint_value(si, section, "AnalogShoulderButtons", 0u);
  ac->analog_trigger_buttons      = (u8)settings_interface_get_uint_value(si, section, "AnalogTriggerButtons", 0u);
  ac->analog_deadzone             = clampf(settings_interface_get_float_value(si, section, "AnalogDeadzone",
                                            CONTROLLER_DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  ac->analog_sensitivity          = clampf(settings_interface_get_float_value(si, section, "AnalogSensitivity",
                                            CONTROLLER_DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  ac->button_deadzone             = clampf(settings_interface_get_float_value(si, section, "ButtonDeadzone",
                                            CONTROLLER_DEFAULT_BUTTON_DEADZONE), 0.01f, 1.0f);

  s32 lvb = settings_interface_get_int_value(si, section, "LargeMotorVibrationBias", DEFAULT_LARGE_MOTOR_VIBRATION_BIAS);
  s32 svb = settings_interface_get_int_value(si, section, "SmallMotorVibrationBias", DEFAULT_SMALL_MOTOR_VIBRATION_BIAS);
  if (lvb < -255) lvb = -255; else if (lvb > 255) lvb = 255;
  if (svb < -255) svb = -255; else if (svb > 255) svb = 255;
  ac->vibration_bias[0] = (s16)lvb;
  ac->vibration_bias[1] = (s16)svb;

  ac->invert_left_stick  = (u8)settings_interface_get_uint_value(si, section, "InvertLeftStick", 0u);
  ac->invert_right_stick = (u8)settings_interface_get_uint_value(si, section, "InvertRightStick", 0u);
}

#define BTN(label, idx, gen)                                                              \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                          \
    .generic_mapping = (gen) }
#define AXIS(label, idx, gen)                                                             \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = ANALOG_CONTROLLER_HALFAXIS_BIND_START_INDEX + (u32)(idx),               \
    .type = INPUT_BINDING_TYPE_HALF_AXIS, .generic_mapping = (gen) }
#define MOTOR(label, idx, gen)                                                            \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = ANALOG_CONTROLLER_MOTOR_BIND_START_INDEX + (u32)(idx),                  \
    .type = INPUT_BINDING_TYPE_MOTOR, .generic_mapping = (gen) }
#define LED(label, idx, gen)                                                              \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = ANALOG_CONTROLLER_LED_BIND_START_INDEX + (u32)(idx),                    \
    .type = INPUT_BINDING_TYPE_LED, .generic_mapping = (gen) }

static const controller_binding_info_t s_analog_bindings[] = {
  BTN("Up",       ANALOG_CONTROLLER_BUTTON_UP,       GENERIC_INPUT_BINDING_DPAD_UP),
  BTN("Right",    ANALOG_CONTROLLER_BUTTON_RIGHT,    GENERIC_INPUT_BINDING_DPAD_RIGHT),
  BTN("Down",     ANALOG_CONTROLLER_BUTTON_DOWN,     GENERIC_INPUT_BINDING_DPAD_DOWN),
  BTN("Left",     ANALOG_CONTROLLER_BUTTON_LEFT,     GENERIC_INPUT_BINDING_DPAD_LEFT),
  BTN("Triangle", ANALOG_CONTROLLER_BUTTON_TRIANGLE, GENERIC_INPUT_BINDING_TRIANGLE),
  BTN("Circle",   ANALOG_CONTROLLER_BUTTON_CIRCLE,   GENERIC_INPUT_BINDING_CIRCLE),
  BTN("Cross",    ANALOG_CONTROLLER_BUTTON_CROSS,    GENERIC_INPUT_BINDING_CROSS),
  BTN("Square",   ANALOG_CONTROLLER_BUTTON_SQUARE,   GENERIC_INPUT_BINDING_SQUARE),
  BTN("Select",   ANALOG_CONTROLLER_BUTTON_SELECT,   GENERIC_INPUT_BINDING_SELECT),
  BTN("Start",    ANALOG_CONTROLLER_BUTTON_START,    GENERIC_INPUT_BINDING_START),
  BTN("Analog",   ANALOG_CONTROLLER_BUTTON_ANALOG,   GENERIC_INPUT_BINDING_SYSTEM),
  BTN("L1",       ANALOG_CONTROLLER_BUTTON_L1,       GENERIC_INPUT_BINDING_L1),
  BTN("R1",       ANALOG_CONTROLLER_BUTTON_R1,       GENERIC_INPUT_BINDING_R1),
  BTN("L2",       ANALOG_CONTROLLER_BUTTON_L2,       GENERIC_INPUT_BINDING_L2),
  BTN("R2",       ANALOG_CONTROLLER_BUTTON_R2,       GENERIC_INPUT_BINDING_R2),
  BTN("L3",       ANALOG_CONTROLLER_BUTTON_L3,       GENERIC_INPUT_BINDING_L3),
  BTN("R3",       ANALOG_CONTROLLER_BUTTON_R3,       GENERIC_INPUT_BINDING_R3),

  AXIS("LLeft",   ANALOG_CONTROLLER_HALF_AXIS_LLEFT,  GENERIC_INPUT_BINDING_LEFT_STICK_LEFT),
  AXIS("LRight",  ANALOG_CONTROLLER_HALF_AXIS_LRIGHT, GENERIC_INPUT_BINDING_LEFT_STICK_RIGHT),
  AXIS("LDown",   ANALOG_CONTROLLER_HALF_AXIS_LDOWN,  GENERIC_INPUT_BINDING_LEFT_STICK_DOWN),
  AXIS("LUp",     ANALOG_CONTROLLER_HALF_AXIS_LUP,    GENERIC_INPUT_BINDING_LEFT_STICK_UP),
  AXIS("RLeft",   ANALOG_CONTROLLER_HALF_AXIS_RLEFT,  GENERIC_INPUT_BINDING_RIGHT_STICK_LEFT),
  AXIS("RRight",  ANALOG_CONTROLLER_HALF_AXIS_RRIGHT, GENERIC_INPUT_BINDING_RIGHT_STICK_RIGHT),
  AXIS("RDown",   ANALOG_CONTROLLER_HALF_AXIS_RDOWN,  GENERIC_INPUT_BINDING_RIGHT_STICK_DOWN),
  AXIS("RUp",     ANALOG_CONTROLLER_HALF_AXIS_RUP,    GENERIC_INPUT_BINDING_RIGHT_STICK_UP),

  MOTOR("LargeMotor", LARGE_MOTOR, GENERIC_INPUT_BINDING_LARGE_MOTOR),
  MOTOR("SmallMotor", SMALL_MOTOR, GENERIC_INPUT_BINDING_SMALL_MOTOR),

  LED  ("AnalogLED",  0,           GENERIC_INPUT_BINDING_MODE_LED),
};

#undef BTN
#undef AXIS
#undef MOTOR
#undef LED

const controller_info_t g_analog_controller_info = {
   .type           = CONTROLLER_TYPE_ANALOG_CONTROLLER,
  .name           = "AnalogController",
  .display_name   = "Analog Controller",
  .icon_name      = NULL,
  .bindings       = s_analog_bindings,
  .bindings_count = sizeof(s_analog_bindings) / sizeof(s_analog_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};
