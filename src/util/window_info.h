#ifndef CUPID_UTIL_WINDOW_INFO_H
#define CUPID_UTIL_WINDOW_INFO_H

#include "common/types.h"
#include "gpu_types.h"

typedef enum : u8 {
  WINDOW_INFO_TYPE_SURFACELESS = 0,
  WINDOW_INFO_TYPE_WIN32,        /* present for ABI compat; never used here */
  WINDOW_INFO_TYPE_XLIB,
  WINDOW_INFO_TYPE_XCB,
  WINDOW_INFO_TYPE_WAYLAND,
  WINDOW_INFO_TYPE_MACOS,        /* present for ABI compat */
  WINDOW_INFO_TYPE_ANDROID,      /* present for ABI compat */
} window_info_type_t;

typedef enum : u8 {
  WINDOW_INFO_PREROTATION_IDENTITY = 0,
  WINDOW_INFO_PREROTATION_ROTATE_90_CW,
  WINDOW_INFO_PREROTATION_ROTATE_180_CW,
  WINDOW_INFO_PREROTATION_ROTATE_270_CW,
} window_info_prerotation_t;

typedef struct {
  window_info_type_t        type;
  gpu_texture_format_t      surface_format;
  window_info_prerotation_t surface_prerotation;
  u16                       surface_width;
  u16                       surface_height;
  float                     surface_refresh_rate;
  float                     surface_scale;
  void*                     display_connection;
  void*                     window_handle;
} window_info_t;

void window_info_init(window_info_t* wi);

ALWAYS_INLINE bool window_info_is_surfaceless(const window_info_t* wi)
{
  return wi->type == WINDOW_INFO_TYPE_SURFACELESS;
}

ALWAYS_INLINE bool window_info_should_swap_for_prerotation(window_info_prerotation_t p)
{
  return p == WINDOW_INFO_PREROTATION_ROTATE_90_CW || p == WINDOW_INFO_PREROTATION_ROTATE_270_CW;
}

ALWAYS_INLINE u32 window_info_post_rotated_width(const window_info_t* wi)
{
  return window_info_should_swap_for_prerotation(wi->surface_prerotation) ? wi->surface_height : wi->surface_width;
}
ALWAYS_INLINE u32 window_info_post_rotated_height(const window_info_t* wi)
{
  return window_info_should_swap_for_prerotation(wi->surface_prerotation) ? wi->surface_width : wi->surface_height;
}

float window_info_z_rotation_for_prerotation(window_info_prerotation_t p);

#endif /* CUPID_UTIL_WINDOW_INFO_H */
