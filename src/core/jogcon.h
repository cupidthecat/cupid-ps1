/*
 * SLPH-00001 JogCon - paddle-style controller (digital pad + spinning wheel
 * with force-feedback motor).  Configuration mode mirrors the DualShock
 * protocol, but the analog payload is the wheel position + a rotation
 * direction + the last motor command rather than two analog sticks.
 *
 * Dedicated force-feedback steering hardware is not wired (no device API in
 * cupid-ps1 yet), but the rumble nibble drives SDL haptics via input_manager
 * the same way analog_controller does.
 */

#ifndef CUPID_CORE_JOGCON_H
#define CUPID_CORE_JOGCON_H

#include "core/controller.h"

#include "common/types.h"

typedef enum : u8 {
  JOGCON_BUTTON_SELECT   = 0,
  JOGCON_BUTTON_L3       = 1,
  JOGCON_BUTTON_R3       = 2,
  JOGCON_BUTTON_START    = 3,
  JOGCON_BUTTON_UP       = 4,
  JOGCON_BUTTON_RIGHT    = 5,
  JOGCON_BUTTON_DOWN     = 6,
  JOGCON_BUTTON_LEFT     = 7,
  JOGCON_BUTTON_L2       = 8,
  JOGCON_BUTTON_R2       = 9,
  JOGCON_BUTTON_L1       = 10,
  JOGCON_BUTTON_R1       = 11,
  JOGCON_BUTTON_TRIANGLE = 12,
  JOGCON_BUTTON_CIRCLE   = 13,
  JOGCON_BUTTON_CROSS    = 14,
  JOGCON_BUTTON_SQUARE   = 15,
  JOGCON_BUTTON_MODE     = 16,
  JOGCON_BUTTON_COUNT,
} jogcon_button_t;

typedef enum : u8 {
  JOGCON_HALFAXIS_STEERING_LEFT  = 0,
  JOGCON_HALFAXIS_STEERING_RIGHT = 1,
  JOGCON_HALFAXIS_COUNT,
} jogcon_halfaxis_t;

#define JOGCON_HALFAXIS_BIND_START_INDEX  ((u32)JOGCON_BUTTON_COUNT)
#define JOGCON_MOTOR_BIND_START_INDEX     (JOGCON_HALFAXIS_BIND_START_INDEX + (u32)JOGCON_HALFAXIS_COUNT)
#define JOGCON_LED_BIND_START_INDEX       (JOGCON_MOTOR_BIND_START_INDEX + 1u)
#define JOGCON_FFDEVICE_BIND_START_INDEX  (JOGCON_LED_BIND_START_INDEX + 1u)

controller_t* jogcon_create(u32 index);

extern const controller_info_t g_jogcon_info;

#endif
