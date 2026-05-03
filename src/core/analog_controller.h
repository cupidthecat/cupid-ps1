/*
 * DualShock-class controller: digital + dual analog sticks + 2 vibration
 * motors.  Implements the SCEI configuration-mode protocol, including the
 * 0x43-family commands needed to enter analog/configuration mode and to
 * remap rumble bytes via 0x4D.
 *
 *   - LED state hand-off to InputManager::SetPadLEDState (the Linux input-
 *     manager port doesn't expose an LED hook yet); the analog mode flag is
 *     still maintained internally so games observe the correct halfword count.
 *   - OSD messages on analog/digital toggle (Host:: API not ported).
 *     boot path).
 */

#ifndef CUPID_CORE_ANALOG_CONTROLLER_H
#define CUPID_CORE_ANALOG_CONTROLLER_H

#include "core/controller.h"

#include "common/types.h"

enum {
  ANALOG_CONTROLLER_NUM_MOTORS    = 2,
  ANALOG_CONTROLLER_MAX_RESPONSE_LENGTH = 8,
  ANALOG_CONTROLLER_RUMBLE_CONFIG_LEN   = 6,
};

typedef enum : u8 {
  ANALOG_CONTROLLER_AXIS_LEFT_X,
  ANALOG_CONTROLLER_AXIS_LEFT_Y,
  ANALOG_CONTROLLER_AXIS_RIGHT_X,
  ANALOG_CONTROLLER_AXIS_RIGHT_Y,
  ANALOG_CONTROLLER_AXIS_COUNT,
} analog_controller_axis_t;

typedef enum : u8 {
  ANALOG_CONTROLLER_BUTTON_SELECT   = 0,
  ANALOG_CONTROLLER_BUTTON_L3       = 1,
  ANALOG_CONTROLLER_BUTTON_R3       = 2,
  ANALOG_CONTROLLER_BUTTON_START    = 3,
  ANALOG_CONTROLLER_BUTTON_UP       = 4,
  ANALOG_CONTROLLER_BUTTON_RIGHT    = 5,
  ANALOG_CONTROLLER_BUTTON_DOWN     = 6,
  ANALOG_CONTROLLER_BUTTON_LEFT     = 7,
  ANALOG_CONTROLLER_BUTTON_L2       = 8,
  ANALOG_CONTROLLER_BUTTON_R2       = 9,
  ANALOG_CONTROLLER_BUTTON_L1       = 10,
  ANALOG_CONTROLLER_BUTTON_R1       = 11,
  ANALOG_CONTROLLER_BUTTON_TRIANGLE = 12,
  ANALOG_CONTROLLER_BUTTON_CIRCLE   = 13,
  ANALOG_CONTROLLER_BUTTON_CROSS    = 14,
  ANALOG_CONTROLLER_BUTTON_SQUARE   = 15,
  ANALOG_CONTROLLER_BUTTON_ANALOG   = 16,
  ANALOG_CONTROLLER_BUTTON_COUNT,
} analog_controller_button_t;

typedef enum : u8 {
  ANALOG_CONTROLLER_HALF_AXIS_LLEFT,
  ANALOG_CONTROLLER_HALF_AXIS_LRIGHT,
  ANALOG_CONTROLLER_HALF_AXIS_LDOWN,
  ANALOG_CONTROLLER_HALF_AXIS_LUP,
  ANALOG_CONTROLLER_HALF_AXIS_RLEFT,
  ANALOG_CONTROLLER_HALF_AXIS_RRIGHT,
  ANALOG_CONTROLLER_HALF_AXIS_RDOWN,
  ANALOG_CONTROLLER_HALF_AXIS_RUP,
  ANALOG_CONTROLLER_HALF_AXIS_COUNT,
} analog_controller_half_axis_t;

typedef enum : u8 {
  ANALOG_CONTROLLER_CMD_IDLE,
  ANALOG_CONTROLLER_CMD_READY,
  ANALOG_CONTROLLER_CMD_READ_PAD,            /* 0x42 */
  ANALOG_CONTROLLER_CMD_CONFIG_MODE_SET_MODE,/* 0x43 */
  ANALOG_CONTROLLER_CMD_SET_ANALOG_MODE,     /* 0x44 */
  ANALOG_CONTROLLER_CMD_GET_ANALOG_MODE,     /* 0x45 */
  ANALOG_CONTROLLER_CMD_46,                  /* 0x46; unknown query */
  ANALOG_CONTROLLER_CMD_47,                  /* 0x47; ditto */
  ANALOG_CONTROLLER_CMD_4C,                  /* 0x4C; ditto */
  ANALOG_CONTROLLER_CMD_GET_SET_RUMBLE,      /* 0x4D */
} analog_controller_command_t;

enum {
  ANALOG_CONTROLLER_HALFAXIS_BIND_START_INDEX = (u32)ANALOG_CONTROLLER_BUTTON_COUNT,
  ANALOG_CONTROLLER_MOTOR_BIND_START_INDEX    = ANALOG_CONTROLLER_HALFAXIS_BIND_START_INDEX +
                                                (u32)ANALOG_CONTROLLER_HALF_AXIS_COUNT,
  ANALOG_CONTROLLER_LED_BIND_START_INDEX      = ANALOG_CONTROLLER_MOTOR_BIND_START_INDEX +
                                                ANALOG_CONTROLLER_NUM_MOTORS,
};

typedef struct analog_controller {
  controller_t base;

  analog_controller_command_t command;
  u8 command_step;
  u8 response_length;

  /* Transmit and receive buffers, not including the first Hi-Z/ack response byte. */
  u8 rx_buffer[ANALOG_CONTROLLER_MAX_RESPONSE_LENGTH];
  u8 tx_buffer[ANALOG_CONTROLLER_MAX_RESPONSE_LENGTH];

  float analog_deadzone;
  float analog_sensitivity;
  float button_deadzone;

  /* Index 0 = large motor, 1 = small motor. */
  s16 vibration_bias[ANALOG_CONTROLLER_NUM_MOTORS];
  u8  invert_left_stick;
  u8  invert_right_stick;

  bool force_analog_on_reset;
  bool analog_dpad_in_digital_mode;
  u8   analog_shoulder_buttons;
  u8   analog_trigger_buttons;

  bool analog_mode;
  bool analog_locked;
  bool dualshock_enabled;
  bool configuration_mode;

  u8 axis_state[ANALOG_CONTROLLER_AXIS_COUNT];

  u8 rumble_config[ANALOG_CONTROLLER_RUMBLE_CONFIG_LEN];

  bool analog_toggle_queued;
  u8   status_byte;

  /* Always zero in this port; configurable via cmd 0x4D in the original. */
  u8 digital_mode_extra_halfwords;

  /* buttons are active low */
  u16 button_state;

  u8 motor_state[ANALOG_CONTROLLER_NUM_MOTORS];

  /* both directions of axis state, merged to axis_state */
  u8 half_axis_state[ANALOG_CONTROLLER_HALF_AXIS_COUNT];
} analog_controller_t;

controller_t* analog_controller_create(u32 index);

extern const controller_info_t g_analog_controller_info;

#endif /* CUPID_CORE_ANALOG_CONTROLLER_H */
