/*
 * OSD/LED messages, dedicated force-feedback device, and runahead-replay
 * flags are dropped - only the protocol + binding surface survive.  Motor
 * commands now drive the SDL rumble path via input_manager (same wire as
 * the DualShock large motor); the FFDevice steering force is still absorbed.
 */

#include "core/jogcon.h"

#include "common/log.h"
#include "common/settings_interface.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  JG_CMD_IDLE = 0,
  JG_CMD_READY,
  JG_CMD_READ_PAD,
  JG_CMD_SET_MODE,
  JG_CMD_GET_ANALOG_MODE,
  JG_CMD_SET_ANALOG_MODE,
  JG_CMD_GET_SET_RUMBLE,
  JG_CMD_46,
  JG_CMD_47,
  JG_CMD_4C,
} jg_command_t;

enum : u8 {
  JG_MOTOR_STOP                  = 0x0,
  JG_MOTOR_RIGHT                 = 0x1,
  JG_MOTOR_LEFT                  = 0x2,
  JG_MOTOR_HOLD                  = 0x3,
  JG_MOTOR_DROP_REVOLUTIONS      = 0x8,
  JG_MOTOR_DROP_REVS_AND_HOLD    = 0xB,
  JG_MOTOR_NEW_HOLD              = 0xC,
};

#define JG_MAX_RESPONSE_LENGTH 8

typedef struct {
  controller_t base;

  u16  button_state;            /* active low */
  s8   steering_state;          /* merged from half axes */

  u8   half_axis_state[JOGCON_HALFAXIS_COUNT];

  jg_command_t command;
  u8   command_step;
  u8   response_length;
  u8   status_byte;

  s8   last_steering_state;
  u8   last_motor_command;
  s8   steering_hold_position;
  u8   steering_hold_strength;

  bool configuration_mode;
  bool jogcon_mode;
  bool mode_toggle_queued;

  u8   rumble_config[6];
  u8   rx_buffer[JG_MAX_RESPONSE_LENGTH];
  u8   tx_buffer[JG_MAX_RESPONSE_LENGTH];

  s8   steering_hold_deadzone;
  float analog_deadzone;
  float analog_sensitivity;
  float button_deadzone;

  float last_strength;
} jogcon_t;

#define JG_DEFAULT_STEERING_HOLD_DEADZONE  0.03f

static const controller_vtable_t s_jg_vtable;

static inline float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void jg_set_motor_state(jogcon_t* jg, u8 value);
static void jg_set_motor_direction(jogcon_t* jg, u8 direction_command, u8 strength);
static void jg_update_steering_hold(jogcon_t* jg);
static void jg_set_jogcon_mode(jogcon_t* jg, bool enabled, bool show_message);
static void jg_reset_motor_config(jogcon_t* jg);
static void jg_poll(jogcon_t* jg);

static u8 jg_get_response_num_halfwords(const jogcon_t* jg)
{
  return jg->jogcon_mode ? (u8)3 : (u8)1;
}

static u8 jg_get_mode_id(const jogcon_t* jg)
{
  if (jg->configuration_mode) return 0xF;
  if (jg->jogcon_mode)        return 0xE;
  return 0x4;
}

static u8 jg_get_id_byte(const jogcon_t* jg)
{
  return (u8)((jg_get_mode_id(jg) << 4) | jg_get_response_num_halfwords(jg));
}

controller_t* jogcon_create(u32 index)
{
  jogcon_t* jg = (jogcon_t*)calloc(1, sizeof(*jg));
  if (!jg) return NULL;
  jg->base.vtbl  = &s_jg_vtable;
  jg->base.index = index;
  jg->button_state    = 0xFFFFu;
  jg->status_byte     = 0x5A;
  jg->analog_deadzone    = CONTROLLER_DEFAULT_STICK_DEADZONE;
  jg->analog_sensitivity = CONTROLLER_DEFAULT_STICK_SENSITIVITY;
  jg->button_deadzone    = CONTROLLER_DEFAULT_BUTTON_DEADZONE;
  jg->steering_hold_deadzone = (s8)ceilf(JG_DEFAULT_STEERING_HOLD_DEADZONE * 127.0f);
  return &jg->base;
}

static void jg_destroy(controller_t* base) { free(base); }

static controller_type_t jg_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_JOGCON;
}

static void jg_reset(controller_t* base)
{
  jogcon_t* jg = (jogcon_t*)base;
  jg_set_jogcon_mode(jg, true, false);
  jg->command = JG_CMD_IDLE;
  jg->command_step = 0;
  jg_reset_motor_config(jg);
}

static void jg_reset_transfer_state(controller_t* base)
{
  jogcon_t* jg = (jogcon_t*)base;
  if (jg->mode_toggle_queued) {
    jg_set_jogcon_mode(jg, !jg->jogcon_mode, true);
    jg->mode_toggle_queued = false;
  }
  jg->command = JG_CMD_IDLE;
  jg->command_step = 0;
}

static bool jg_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  jogcon_t* jg = (jogcon_t*)base;

  u16 button_state = jg->button_state;
  u8  steering_state_u = (u8)jg->steering_state;
  state_wrapper_do_u16(sw, &button_state);
  state_wrapper_do_u8 (sw, &steering_state_u);
  if (apply_input_state) {
    jg->button_state   = button_state;
    jg->steering_state = (s8)steering_state_u;
  }

  u8 cmd = (u8)jg->command;
  state_wrapper_do_u8(sw, &cmd);
  jg->command = (jg_command_t)cmd;
  state_wrapper_do_u8(sw, &jg->command_step);
  state_wrapper_do_u8(sw, &jg->status_byte);

  u8 last_steering_u = (u8)jg->last_steering_state;
  state_wrapper_do_u8(sw, &last_steering_u);
  jg->last_steering_state = (s8)last_steering_u;
  state_wrapper_do_u8(sw, &jg->last_motor_command);

  u8 hold_pos_u = (u8)jg->steering_hold_position;
  state_wrapper_do_u8(sw, &hold_pos_u);
  jg->steering_hold_position = (s8)hold_pos_u;
  state_wrapper_do_u8(sw, &jg->steering_hold_strength);

  state_wrapper_do_bool(sw, &jg->configuration_mode);

  bool jogcon_mode = jg->jogcon_mode;
  state_wrapper_do_bool(sw, &jogcon_mode);
  if (jogcon_mode != jg->jogcon_mode)
    jg_set_jogcon_mode(jg, jogcon_mode, true);

  state_wrapper_do_bytes(sw, jg->rx_buffer, sizeof(jg->rx_buffer));
  state_wrapper_do_bytes(sw, jg->tx_buffer, sizeof(jg->tx_buffer));
  state_wrapper_do_bytes(sw, jg->rumble_config, sizeof(jg->rumble_config));

  return true;
}

static float jg_get_bind_state(const controller_t* base, u32 index)
{
  const jogcon_t* jg = (const jogcon_t*)base;
  if (index >= JOGCON_LED_BIND_START_INDEX)
    return (index == JOGCON_LED_BIND_START_INDEX && jg->jogcon_mode) ? 1.0f : 0.0f;
  if (index >= JOGCON_MOTOR_BIND_START_INDEX)
    return jg->last_strength;
  if (index >= JOGCON_HALFAXIS_BIND_START_INDEX) {
    const u32 sub = index - JOGCON_HALFAXIS_BIND_START_INDEX;
    if (sub >= JOGCON_HALFAXIS_COUNT) return 0.0f;
    return (float)jg->half_axis_state[sub] * (1.0f / 255.0f);
  }
  if (index < (u32)JOGCON_BUTTON_MODE)
    return (float)(((jg->button_state >> index) & 1u) ^ 1u);
  return 0.0f;
}

static void jg_set_bind_state(controller_t* base, u32 index, float value)
{
  jogcon_t* jg = (jogcon_t*)base;

  if (index == (u32)JOGCON_BUTTON_MODE) {
    if (value >= jg->button_deadzone) {
      if (jg->command == JG_CMD_IDLE)
        jg_set_jogcon_mode(jg, !jg->jogcon_mode, true);
      else
        jg->mode_toggle_queued = true;
    }
    return;
  }
  if (index >= (u32)JOGCON_BUTTON_COUNT) {
    const u32 sub_index = index - (u32)JOGCON_BUTTON_COUNT;
    if (sub_index >= JOGCON_HALFAXIS_COUNT)
      return;

    const float scaled = ((value < jg->analog_deadzone) ? 0.0f : value) * jg->analog_sensitivity * 255.0f;
    const u8 u8val = (u8)clampf(scaled, 0.0f, 255.0f);
    if (u8val == jg->half_axis_state[sub_index])
      return;
    jg->half_axis_state[sub_index] = u8val;

     jg->steering_state = (jg->half_axis_state[JOGCON_HALFAXIS_STEERING_RIGHT] != 0)
        ? (s8)(jg->half_axis_state[JOGCON_HALFAXIS_STEERING_RIGHT] / 2u) 
        : -(s8)((((u32)jg->half_axis_state[JOGCON_HALFAXIS_STEERING_LEFT]) + 1u) / 2u);
    return;
  }

  const u16 bit = (u16)(1u << (u8)index);
  if (value >= jg->button_deadzone) jg->button_state &= (u16)~bit;
  else                              jg->button_state |= bit;
}

static u32 jg_get_button_state_bits(const controller_t* base)
{
  return (u32)(((const jogcon_t*)base)->button_state ^ 0xFFFFu);
}

static void jg_set_jogcon_mode(jogcon_t* jg, bool enabled, bool show_message)
{
  (void)show_message;
  if (jg->jogcon_mode == enabled) return;
  jg->jogcon_mode = enabled;
  jg->configuration_mode = enabled && jg->configuration_mode;
  INFO_LOG("Controller %u switched to %s mode.", jg->base.index + 1u,
           jg->jogcon_mode ? "JogCon" : "Digital");
}

static void jg_set_motor_direction(jogcon_t* jg, u8 direction_command, u8 strength)
{
  if (direction_command == JG_MOTOR_STOP || strength == 0) {
    if (jg->last_strength != 0.0f) {
      jg->last_strength = 0.0f;
      input_manager_set_pad_vibration_intensity(jg->base.index, JOGCON_MOTOR_BIND_START_INDEX, 0.0f);
    }
    return;
  }
  const float f_strength = (float)strength / 15.0f;
  if (f_strength != jg->last_strength) {
    jg->last_strength = f_strength;
    input_manager_set_pad_vibration_intensity(jg->base.index, JOGCON_MOTOR_BIND_START_INDEX, f_strength);
  }
}

static void jg_update_steering_hold(jogcon_t* jg)
{
  if (jg->steering_hold_strength == 0)
    return;
  const int diff = (int)jg->steering_state - (int)jg->steering_hold_position;
  const int abs_diff = (diff < 0) ? -diff : diff;
  u8 dir;
  if (abs_diff < (int)jg->steering_hold_deadzone)
    dir = JG_MOTOR_STOP;
  else
    dir = (jg->steering_state < jg->steering_hold_position) ? JG_MOTOR_RIGHT : JG_MOTOR_LEFT;
  jg_set_motor_direction(jg, dir, jg->steering_hold_strength);
}

static void jg_set_motor_state(jogcon_t* jg, u8 value)
{
  const u8 command  = (value >> 4);
  const u8 strength = (value & 0x0F);

  switch (command) {
    case JG_MOTOR_STOP:
      jg->steering_hold_strength = 0;
      jg_set_motor_direction(jg, JG_MOTOR_STOP, 0);
      break;
    case JG_MOTOR_RIGHT:
    case JG_MOTOR_LEFT:
      jg->steering_hold_strength = 0;
      jg_set_motor_direction(jg, command, strength);
      break;
    case JG_MOTOR_HOLD:
    case JG_MOTOR_DROP_REVS_AND_HOLD:
      jg->steering_hold_strength = strength;
      jg_update_steering_hold(jg);
      if (command == JG_MOTOR_DROP_REVS_AND_HOLD)
        ERROR_LOG("JogCon Drop revolutions and hold command is not handled.");
      break;
    case JG_MOTOR_DROP_REVOLUTIONS:
      ERROR_LOG("JogCon drop revolutions command is not handled.");
      break;
    case JG_MOTOR_NEW_HOLD:
      jg->steering_hold_position = jg->steering_state;
      break;
    default:
      ERROR_LOG("Unknown JogCon command 0x%X", command);
      break;
  }
  jg->last_motor_command = command;
}

static void jg_reset_motor_config(jogcon_t* jg)
{
  memset(jg->rumble_config, 0xFF, sizeof(jg->rumble_config));
  jg_set_motor_state(jg, 0);
}

static void jg_poll(jogcon_t* jg)
{
  jg->tx_buffer[2] = (u8)jg->button_state;
  jg->tx_buffer[3] = (u8)(jg->button_state >> 8);

  jg->tx_buffer[4] = (u8)jg->steering_state;
  jg->tx_buffer[5] = (jg->steering_state < 0) ? 0xFF : 0x00;

  u8 rotation_state = 0;
  if (jg->steering_state > jg->last_steering_state)
    rotation_state = 1;
  else if (jg->steering_state < jg->last_steering_state)
    rotation_state = 2;

  jg->tx_buffer[6] = (u8)(rotation_state | (jg->last_motor_command << 4));

  jg->last_steering_state = jg->steering_state;
  jg_update_steering_hold(jg);
}

static void jg_fill_tx_default(jogcon_t* jg)
{
  jg->tx_buffer[0] = jg_get_id_byte(jg);
  jg->tx_buffer[1] = jg->status_byte;
  for (u32 i = 2; i < JG_MAX_RESPONSE_LENGTH; i++)
    jg->tx_buffer[i] = 0x00;
}

static bool jg_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  jogcon_t* jg = (jogcon_t*)base;
  jg->rx_buffer[jg->command_step] = data_in;

  switch (jg->command) {
    case JG_CMD_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01) {
        DEBUG_LOG("ACK controller access");
        jg->command = JG_CMD_READY;
        memset(jg->tx_buffer, 0, sizeof(jg->tx_buffer));
        memset(jg->rx_buffer, 0, sizeof(jg->rx_buffer));
        return true;
      }
      return false;

    case JG_CMD_READY: {
      if (data_in == 0x42) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_READ_PAD;
        jg_fill_tx_default(jg);
        jg_poll(jg);
      } else if (jg->jogcon_mode && data_in == 0x43) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_SET_MODE;
        jg_fill_tx_default(jg);
        jg_poll(jg);
      } else if (jg->configuration_mode && data_in == 0x44) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_SET_ANALOG_MODE;
        jg_fill_tx_default(jg);
        jg_reset_motor_config(jg);
      } else if (jg->configuration_mode && data_in == 0x45) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_GET_ANALOG_MODE;
        jg->tx_buffer[0] = jg_get_id_byte(jg);
        jg->tx_buffer[1] = jg->status_byte;
        jg->tx_buffer[2] = 0x01;
        jg->tx_buffer[3] = 0x02;
        jg->tx_buffer[4] = jg->jogcon_mode ? 1u : 0u;
        jg->tx_buffer[5] = 0x01;
        jg->tx_buffer[6] = 0x01;
        jg->tx_buffer[7] = 0x00;
      } else if (jg->configuration_mode && data_in == 0x46) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_46;
        jg_fill_tx_default(jg);
      } else if (jg->configuration_mode && data_in == 0x47) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_47;
        jg->tx_buffer[0] = jg_get_id_byte(jg);
        jg->tx_buffer[1] = jg->status_byte;
        jg->tx_buffer[2] = 0x00;
        jg->tx_buffer[3] = 0x00;
        jg->tx_buffer[4] = 0x02;
        jg->tx_buffer[5] = 0x00;
        jg->tx_buffer[6] = 0x01;
        jg->tx_buffer[7] = 0x00;
      } else if (jg->configuration_mode && data_in == 0x4C) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_4C;
        jg_fill_tx_default(jg);
      } else if (jg->configuration_mode && data_in == 0x4D) {
        jg->response_length = (u8)((jg_get_response_num_halfwords(jg) + 1u) * 2u);
        jg->command = JG_CMD_GET_SET_RUMBLE;
        jg_fill_tx_default(jg);
      } else {
        ERROR_LOG("Unimplemented command 0x%02X", data_in);
        *data_out = 0xFF;
        return false;
      }
    } break;

    case JG_CMD_READ_PAD:
      if (jg->command_step >= 2 && jg->command_step < 7 &&
          jg->rumble_config[jg->command_step - 2] == 0x00)
        jg_set_motor_state(jg, data_in);
      break;

    case JG_CMD_GET_ANALOG_MODE:
      break;

    case JG_CMD_SET_ANALOG_MODE:
      if (jg->command_step == 2) {
        if (data_in == 0x00 || data_in == 0x01)
          jg_set_jogcon_mode(jg, data_in == 0x01, true);
      } else if (jg->command_step == 3) {
        if (data_in == 0x02 || data_in == 0x03)
          WARNING_LOG("Unimplemented analog mode lock %u", (data_in == 0x03));
      }
      break;

    case JG_CMD_SET_MODE:
      jg->configuration_mode = (jg->rx_buffer[2] == 1 && jg->jogcon_mode);
      if (jg->configuration_mode)
        jg->status_byte = 0x5A;
      break;

    case JG_CMD_GET_SET_RUMBLE:
      if (jg->command_step >= 2 && jg->command_step < 7) {
        const u8 idx = (u8)(jg->command_step - 2);
        jg->tx_buffer[jg->command_step] = jg->rumble_config[idx];
        jg->rumble_config[idx] = data_in;
        if (data_in == 0x00)
          WARNING_LOG("Motor mapped to byte index %u", idx);
      } else {
        bool any_zero = false;
        for (u32 i = 0; i < sizeof(jg->rumble_config); i++) {
          if (jg->rumble_config[i] == 0) { any_zero = true; break; }
        }
        if (!any_zero)
          jg_set_motor_state(jg, 0);
      }
      break;

    case JG_CMD_46:
      if (jg->command_step == 2) {
        if (data_in == 0x00) {
          jg->tx_buffer[4] = 0x01; jg->tx_buffer[5] = 0x02;
          jg->tx_buffer[6] = 0x00; jg->tx_buffer[7] = 0x0A;
        } else if (data_in == 0x01) {
          jg->tx_buffer[4] = 0x01; jg->tx_buffer[5] = 0x01;
          jg->tx_buffer[6] = 0x01; jg->tx_buffer[7] = 0x14;
        }
      }
      break;

    case JG_CMD_47:
      if (jg->command_step == 2 && data_in != 0x00) {
        jg->tx_buffer[4] = 0x00; jg->tx_buffer[5] = 0x00;
        jg->tx_buffer[6] = 0x00; jg->tx_buffer[7] = 0x00;
      }
      break;

    case JG_CMD_4C:
      if (jg->command_step == 2) {
        if      (data_in == 0x00) jg->tx_buffer[5] = 0x04;
        else if (data_in == 0x01) jg->tx_buffer[4] = 0x03;
      }
      break;
  }

  *data_out = jg->tx_buffer[jg->command_step];

  jg->command_step = (u8)((jg->command_step + 1u) % jg->response_length);
  const bool ack = (jg->command_step != 0);
  if (jg->command_step == 0)
    jg->command = JG_CMD_IDLE;
  return ack;
}

static void jg_load_settings(controller_t* base, const settings_interface_t* si,
                             const char* section, bool initial)
{
  (void)initial;
  jogcon_t* jg = (jogcon_t*)base;
  jg->analog_deadzone = clampf(
      settings_interface_get_float_value(si, section, "AnalogDeadzone",   CONTROLLER_DEFAULT_STICK_DEADZONE),    0.0f, 1.0f);
  jg->analog_sensitivity = clampf(
      settings_interface_get_float_value(si, section, "AnalogSensitivity", CONTROLLER_DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  jg->button_deadzone = clampf(
      settings_interface_get_float_value(si, section, "ButtonDeadzone",   CONTROLLER_DEFAULT_BUTTON_DEADZONE),   0.01f, 1.0f);
  jg->steering_hold_deadzone = (s8)ceilf(
      clampf(settings_interface_get_float_value(si, section, "SteeringHoldDeadzone", JG_DEFAULT_STEERING_HOLD_DEADZONE),
             0.0f, 1.0f) * 127.0f);
  /* Dedicated force-feedback (steering torque) device not wired; SDL rumble runs via input_manager. */
}

#define BTN(label, idx, gen)                                                       \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                   \
    .generic_mapping = (gen) }
#define HAXIS(label, idx, gen)                                                     \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = JOGCON_HALFAXIS_BIND_START_INDEX + (u32)(idx),                   \
    .type = INPUT_BINDING_TYPE_HALF_AXIS, .generic_mapping = (gen) }

static const controller_binding_info_t s_jg_bindings[] = {
  BTN("Up",            JOGCON_BUTTON_UP,       GENERIC_INPUT_BINDING_DPAD_UP),
  BTN("Right",         JOGCON_BUTTON_RIGHT,    GENERIC_INPUT_BINDING_DPAD_RIGHT),
  BTN("Down",          JOGCON_BUTTON_DOWN,     GENERIC_INPUT_BINDING_DPAD_DOWN),
  BTN("Left",          JOGCON_BUTTON_LEFT,     GENERIC_INPUT_BINDING_DPAD_LEFT),
  BTN("Triangle",      JOGCON_BUTTON_TRIANGLE, GENERIC_INPUT_BINDING_TRIANGLE),
  BTN("Circle",        JOGCON_BUTTON_CIRCLE,   GENERIC_INPUT_BINDING_CIRCLE),
  BTN("Cross",         JOGCON_BUTTON_CROSS,    GENERIC_INPUT_BINDING_CROSS),
  BTN("Square",        JOGCON_BUTTON_SQUARE,   GENERIC_INPUT_BINDING_SQUARE),
  BTN("Select",        JOGCON_BUTTON_SELECT,   GENERIC_INPUT_BINDING_SELECT),
  BTN("Start",         JOGCON_BUTTON_START,    GENERIC_INPUT_BINDING_START),
  BTN("L1",            JOGCON_BUTTON_L1,       GENERIC_INPUT_BINDING_L1),
  BTN("R1",            JOGCON_BUTTON_R1,       GENERIC_INPUT_BINDING_R1),
  BTN("L2",            JOGCON_BUTTON_L2,       GENERIC_INPUT_BINDING_L2),
  BTN("R2",            JOGCON_BUTTON_R2,       GENERIC_INPUT_BINDING_R2),
  BTN("Mode",          JOGCON_BUTTON_MODE,     GENERIC_INPUT_BINDING_SYSTEM),

  HAXIS("SteeringLeft",  JOGCON_HALFAXIS_STEERING_LEFT,  GENERIC_INPUT_BINDING_LEFT_STICK_LEFT),
  HAXIS("SteeringRight", JOGCON_HALFAXIS_STEERING_RIGHT, GENERIC_INPUT_BINDING_LEFT_STICK_RIGHT),

  { .name = "ModeLED", .display_name = "Mode LED", .icon_name = NULL,
     .bind_index = JOGCON_LED_BIND_START_INDEX, .type = INPUT_BINDING_TYPE_LED,
    .generic_mapping = GENERIC_INPUT_BINDING_MODE_LED }, 
  { .name = "Motor", .display_name = "Vibration Motor", .icon_name = NULL,
     .bind_index = JOGCON_MOTOR_BIND_START_INDEX, .type = INPUT_BINDING_TYPE_MOTOR,
    .generic_mapping = GENERIC_INPUT_BINDING_LARGE_MOTOR }, 
  { .name = "ForceFeedbackDevice", .display_name = "Force Feedback Device", .icon_name = NULL,
     .bind_index = JOGCON_FFDEVICE_BIND_START_INDEX, .type = INPUT_BINDING_TYPE_DEVICE,
    .generic_mapping = GENERIC_INPUT_BINDING_UNKNOWN }, 
};

#undef BTN
#undef HAXIS

const controller_info_t g_jogcon_info = {
   .type           = CONTROLLER_TYPE_JOGCON,
  .name           = "JogCon",
  .display_name   = "JogCon",
  .icon_name      = NULL,
  .bindings       = s_jg_bindings,
  .bindings_count = sizeof(s_jg_bindings) / sizeof(s_jg_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

static const controller_vtable_t s_jg_vtable = {
   .destroy                = jg_destroy,
  .get_type               = jg_get_type,
  .reset                  = jg_reset,
  .do_state               = jg_do_state,
  .reset_transfer_state   = jg_reset_transfer_state,
  .transfer               = jg_transfer,
  .get_bind_state         = jg_get_bind_state,
  .set_bind_state         = jg_set_bind_state,
  .get_button_state_bits  = jg_get_button_state_bits,
  .get_analog_input_bytes = NULL,
  .load_settings          = jg_load_settings, 
};
