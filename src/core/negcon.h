/*
 *

 * NPC-101 NeGcon - racing twist controller.  Steering is a true analog
 * twist axis; I/II/L are pressure-sensitive triggers, R is binary.
 */
#ifndef CUPID_CORE_NEGCON_H
#define CUPID_CORE_NEGCON_H

#include "core/controller.h"

#include "common/types.h"

typedef enum : u8 {
  NEGCON_AXIS_STEERING = 0,
  NEGCON_AXIS_I        = 1,
  NEGCON_AXIS_II       = 2,
  NEGCON_AXIS_L        = 3,
  NEGCON_AXIS_COUNT,
} negcon_axis_t;

typedef enum : u8 {
  NEGCON_BUTTON_START = 0,
  NEGCON_BUTTON_UP    = 1,
  NEGCON_BUTTON_RIGHT = 2,
  NEGCON_BUTTON_DOWN  = 3,
  NEGCON_BUTTON_LEFT  = 4,
  NEGCON_BUTTON_R     = 5,
  NEGCON_BUTTON_B     = 6,
  NEGCON_BUTTON_A     = 7,
  NEGCON_BUTTON_COUNT,
} negcon_button_t;

typedef enum : u8 {
  NEGCON_HALFAXIS_STEERING_LEFT  = 0,
  NEGCON_HALFAXIS_STEERING_RIGHT = 1,
  NEGCON_HALFAXIS_I              = 2,
  NEGCON_HALFAXIS_II             = 3,
  NEGCON_HALFAXIS_L              = 4,
  NEGCON_HALFAXIS_COUNT,
} negcon_halfaxis_t;

controller_t* negcon_create(u32 index);

extern const controller_info_t g_negcon_info;

#endif
