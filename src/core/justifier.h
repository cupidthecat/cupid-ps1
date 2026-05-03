/*
 * Konami Justifier light gun.  Like the GunCon but reports its hit position
 * via an IRQ10 pulse on the matching CRTC tick rather than via the SIO data
 * line.  The framebuffer hit-test (window pixel -> beam tick/line) is
 * delegated to a weak gpu_query_light_gun_position() shim defined in
 * guncon.c; until gpu.c provides it, the gun reports "no hit".
 */

#ifndef CUPID_CORE_JUSTIFIER_H
#define CUPID_CORE_JUSTIFIER_H

#include "core/controller.h"

#include "common/types.h"

typedef enum : u8 {
  JUSTIFIER_BIND_TRIGGER         = 0,
  JUSTIFIER_BIND_START           = 1,
  JUSTIFIER_BIND_BACK            = 2,
  JUSTIFIER_BIND_SHOOT_OFFSCREEN = 3,
  JUSTIFIER_BIND_BUTTON_COUNT    = 4,

  JUSTIFIER_BIND_RELATIVE_LEFT   = 4,
  JUSTIFIER_BIND_RELATIVE_RIGHT  = 5,
  JUSTIFIER_BIND_RELATIVE_UP     = 6,
  JUSTIFIER_BIND_RELATIVE_DOWN   = 7,
  JUSTIFIER_BIND_COUNT           = 8,
} justifier_binding_t;

controller_t* justifier_create(u32 index);

extern const controller_info_t g_justifier_info;

#endif
