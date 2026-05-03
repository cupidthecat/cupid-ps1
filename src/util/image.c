#include "image.h"

const char* image_format_name(image_format_t f)
{
  switch (f)
  {
    case IMAGE_FORMAT_NONE:   return "None";
    case IMAGE_FORMAT_RGBA8:  return "RGBA8";
    case IMAGE_FORMAT_BGRA8:  return "BGRA8";
    case IMAGE_FORMAT_RGB565: return "RGB565";
    case IMAGE_FORMAT_RGB5A1: return "RGB5A1";
    case IMAGE_FORMAT_A1BGR5: return "A1BGR5";
    case IMAGE_FORMAT_BGR8:   return "BGR8";
    case IMAGE_FORMAT_BC1:    return "BC1";
    case IMAGE_FORMAT_BC2:    return "BC2";
    case IMAGE_FORMAT_BC3:    return "BC3";
    case IMAGE_FORMAT_BC7:    return "BC7";
    default:                  return "?";
  }
}

u32 image_format_pixel_size(image_format_t f)
{
  switch (f)
  {
    case IMAGE_FORMAT_RGBA8:
    case IMAGE_FORMAT_BGRA8:  return 4;
    case IMAGE_FORMAT_RGB565:
    case IMAGE_FORMAT_RGB5A1:
    case IMAGE_FORMAT_A1BGR5: return 2;
    case IMAGE_FORMAT_BGR8:   return 3;
    case IMAGE_FORMAT_BC1:    return 8;
    case IMAGE_FORMAT_BC2:
    case IMAGE_FORMAT_BC3:
    case IMAGE_FORMAT_BC7:    return 16;
    default:                  return 0;
  }
}
