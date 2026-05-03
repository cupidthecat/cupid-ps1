/*
 * Standard PS1 digital controller (no analog sticks, no rumble).  The Pop'n
 * variant ties three direction inputs to ground via a constant button mask;
 * we share the same struct + vtable and just construct it with a different
 * mask via digital_controller_create(type=...).
 */

#ifndef CUPID_CORE_DIGITAL_CONTROLLER_H
#define CUPID_CORE_DIGITAL_CONTROLLER_H

#include "core/controller.h"

#include "common/types.h"

typedef enum : u8 {
  DIGITAL_CONTROLLER_BUTTON_SELECT   = 0,
  DIGITAL_CONTROLLER_BUTTON_L3       = 1,
  DIGITAL_CONTROLLER_BUTTON_R3       = 2,
  DIGITAL_CONTROLLER_BUTTON_START    = 3,
  DIGITAL_CONTROLLER_BUTTON_UP       = 4,
  DIGITAL_CONTROLLER_BUTTON_RIGHT    = 5,
  DIGITAL_CONTROLLER_BUTTON_DOWN     = 6,
  DIGITAL_CONTROLLER_BUTTON_LEFT     = 7,
  DIGITAL_CONTROLLER_BUTTON_L2       = 8,
  DIGITAL_CONTROLLER_BUTTON_R2       = 9,
  DIGITAL_CONTROLLER_BUTTON_L1       = 10,
  DIGITAL_CONTROLLER_BUTTON_R1       = 11,
  DIGITAL_CONTROLLER_BUTTON_TRIANGLE = 12,
  DIGITAL_CONTROLLER_BUTTON_CIRCLE   = 13,
  DIGITAL_CONTROLLER_BUTTON_CROSS    = 14,
  DIGITAL_CONTROLLER_BUTTON_SQUARE   = 15,
  DIGITAL_CONTROLLER_BUTTON_COUNT,
} digital_controller_button_t;

typedef enum : u8 {
  DIGITAL_CONTROLLER_TRANSFER_IDLE,
  DIGITAL_CONTROLLER_TRANSFER_READY,
  DIGITAL_CONTROLLER_TRANSFER_ID_MSB,
  DIGITAL_CONTROLLER_TRANSFER_BUTTONS_LSB,
  DIGITAL_CONTROLLER_TRANSFER_BUTTONS_MSB,
} digital_controller_transfer_state_t;

typedef struct digital_controller {
  controller_t base;

  /* buttons are active low: bit set = released. */
  u16 button_state;
  u16 button_mask;

  digital_controller_transfer_state_t transfer_state;
} digital_controller_t;

/* Allocates and returns a fresh digital controller.  `type` must be either
 * CONTROLLER_TYPE_DIGITAL_CONTROLLER or CONTROLLER_TYPE_POPN_CONTROLLER --
 * the latter installs the pop'n button mask. */
controller_t* digital_controller_create(u32 index, controller_type_t type);

extern const controller_info_t g_digital_controller_info;
extern const controller_info_t g_popn_controller_info;

#endif /* CUPID_CORE_DIGITAL_CONTROLLER_H */
