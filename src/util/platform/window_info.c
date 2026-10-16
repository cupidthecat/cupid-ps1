#include "window_info.h"

#include <math.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

void window_info_init(window_info_t* wi)
{
  wi->type                 = WINDOW_INFO_TYPE_SURFACELESS;
  wi->surface_format       = GPU_TEXTURE_FORMAT_UNKNOWN;
  wi->surface_prerotation  = WINDOW_INFO_PREROTATION_IDENTITY;
  wi->surface_width        = 0;
  wi->surface_height       = 0;
  wi->surface_refresh_rate = 0.0f;
  wi->surface_scale        = 1.0f;
  wi->display_connection   = NULL;
  wi->window_handle        = NULL;
}

float window_info_z_rotation_for_prerotation(window_info_prerotation_t p)
{
  static const float radians[4] = {
    0.0f,                  /* Identity */
    (float)(M_PI * 1.5),   /* Rotate90Clockwise */
    (float)(M_PI),         /* Rotate180Clockwise */
    (float)(M_PI / 2.0),   /* Rotate270Clockwise */
  };
  if ((unsigned)p > 3) return 0.0f;
  return radians[p];
}
