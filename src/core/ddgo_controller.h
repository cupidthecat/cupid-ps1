/*
 * SLPH-00051 Densha de Go! controller.  Reports a 5-detent power lever and
 * 9-detent brake lever (plus an emergency-brake position) packed into the
 * digital button word.  The physical levers are exposed both as analog
 * half-axes (Power/Brake) and as 16 virtual buttons (one per discrete
 * level) so the input overlay can show every position.
 */

#ifndef CUPID_CORE_DDGO_CONTROLLER_H
#define CUPID_CORE_DDGO_CONTROLLER_H

#include "core/controller.h"

#include "common/types.h"

/* Bit positions in the PS1 button word (active-low). */
typedef enum : u8 {
  DDGO_BIND_SELECT     = 0,
  DDGO_BIND_POWER      = 1,
  DDGO_BIND_BRAKE      = 2,
  DDGO_BIND_START      = 3,
  DDGO_BIND_POWER_BIT2 = 5,
  DDGO_BIND_POWER_BIT1 = 7,
  DDGO_BIND_BRAKE_BIT1 = 8,
  DDGO_BIND_BRAKE_BIT3 = 9,
  DDGO_BIND_BRAKE_BIT0 = 10,
  DDGO_BIND_BRAKE_BIT2 = 11,
  DDGO_BIND_POWER_BIT0 = 12,
  DDGO_BIND_C          = 13,
  DDGO_BIND_B          = 14,
  DDGO_BIND_A          = 15,

  DDGO_BIND_VIRTUAL_BUTTON_START   = 16,
  DDGO_BIND_VIRTUAL_POWER_OFF      = 16,
  DDGO_BIND_VIRTUAL_POWER_1        = 17,
  DDGO_BIND_VIRTUAL_POWER_2        = 18,
  DDGO_BIND_VIRTUAL_POWER_3        = 19,
  DDGO_BIND_VIRTUAL_POWER_4        = 20,
  DDGO_BIND_VIRTUAL_POWER_5        = 21,
  DDGO_BIND_VIRTUAL_BRAKE_RELEASED = 22,
  DDGO_BIND_VIRTUAL_BRAKE_1        = 23,
  DDGO_BIND_VIRTUAL_BRAKE_2        = 24,
  DDGO_BIND_VIRTUAL_BRAKE_3        = 25,
  DDGO_BIND_VIRTUAL_BRAKE_4        = 26,
  DDGO_BIND_VIRTUAL_BRAKE_5        = 27,
  DDGO_BIND_VIRTUAL_BRAKE_6        = 28,
  DDGO_BIND_VIRTUAL_BRAKE_7        = 29,
  DDGO_BIND_VIRTUAL_BRAKE_8        = 30,
  DDGO_BIND_VIRTUAL_BRAKE_EMERGENCY= 31,
  DDGO_BIND_COUNT                  = 32,
} ddgo_binding_t;

controller_t* ddgo_controller_create(u32 index);

extern const controller_info_t g_ddgo_controller_info;

#endif
