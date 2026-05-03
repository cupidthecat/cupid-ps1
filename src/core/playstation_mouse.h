/*
 *

 * SCPH-1090 PlayStation Mouse.  Two buttons + relative pointer.
 */
#ifndef CUPID_CORE_PLAYSTATION_MOUSE_H
#define CUPID_CORE_PLAYSTATION_MOUSE_H

#include "core/controller.h"

#include "common/types.h"

typedef enum : u8 {
  PSMOUSE_BIND_LEFT     = 0,
  PSMOUSE_BIND_RIGHT    = 1,
  PSMOUSE_BIND_BUTTON_COUNT = 2,
  PSMOUSE_BIND_POINTER_X = 2,
  PSMOUSE_BIND_POINTER_Y = 3,
  PSMOUSE_BIND_COUNT     = 4,
} playstation_mouse_binding_t;

controller_t* playstation_mouse_create(u32 index);

extern const controller_info_t g_playstation_mouse_info;

#endif
