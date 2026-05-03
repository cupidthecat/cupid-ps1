/*
 * enum + RGBA8 PNG load/save are exposed.  Animated images, DDS, BC encoders,
 * format conversion table all dropped - the full image module lands later if
 * a consumer needs it.  GPU texture replacement is out of scope.
 */

#ifndef CUPID_UTIL_IMAGE_H
#define CUPID_UTIL_IMAGE_H

#include "common/types.h"

typedef struct Error Error;

typedef enum {
  IMAGE_FORMAT_NONE = 0,
  IMAGE_FORMAT_RGBA8,
  IMAGE_FORMAT_BGRA8,
  IMAGE_FORMAT_RGB565,
  IMAGE_FORMAT_RGB5A1,
  IMAGE_FORMAT_A1BGR5,
  IMAGE_FORMAT_BGR8,
  IMAGE_FORMAT_BC1,
  IMAGE_FORMAT_BC2,
  IMAGE_FORMAT_BC3,
  IMAGE_FORMAT_BC7,
  IMAGE_FORMAT_MAX_COUNT,
} image_format_t;

const char* image_format_name(image_format_t f);
u32         image_format_pixel_size(image_format_t f);

#endif /* CUPID_UTIL_IMAGE_H */
