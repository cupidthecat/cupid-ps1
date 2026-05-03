/*
 * SLPH-00034 GunCon (Namco light gun).  Trigger + A + B + a "Shoot Offscreen"
 * helper that synthesises a trigger pull while marking the position invalid.
 * The framebuffer hit-test (window pixel -> beam tick/line) is provided by
 * gpu_query_light_gun_position(), declared as a weak symbol so this file
 * compiles before gpu.c grows the helper.
 */

#ifndef CUPID_CORE_GUNCON_H
#define CUPID_CORE_GUNCON_H

#include "core/controller.h"

#include "common/types.h"

typedef enum : u8 {
  GUNCON_BIND_TRIGGER         = 0,
  GUNCON_BIND_A               = 1,
  GUNCON_BIND_B               = 2,
  GUNCON_BIND_SHOOT_OFFSCREEN = 3,
  GUNCON_BIND_BUTTON_COUNT    = 4,

  GUNCON_BIND_RELATIVE_LEFT   = 4,
  GUNCON_BIND_RELATIVE_RIGHT  = 5,
  GUNCON_BIND_RELATIVE_UP     = 6,
  GUNCON_BIND_RELATIVE_DOWN   = 7,
  GUNCON_BIND_COUNT           = 8,
} guncon_binding_t;

controller_t* guncon_create(u32 index);

extern const controller_info_t g_guncon_info;

#endif
