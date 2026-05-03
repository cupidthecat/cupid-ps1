/*
 * stb_image is instantiated here; and only here; via the
 * STB_IMAGE_IMPLEMENTATION define.  This TU compiles with -w (Makefile per-TU
 * rule) so stb's many implicit-conversion warnings stay quarantined.
 *
 * STBI_ONLY_PNG cuts ~half of stb_image.  PSP/PS1 replacement packs are PNG
 * by convention; if the project ever needs JPG/TGA/BMP/GIF, drop the define.
 */

#include "core/tc_image_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO         /* feed bytes via stbi_load_from_memory */
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8* slurp_file(const char* path, size_t* out_size)
{
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  const long len = ftell(f);
  if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  u8* buf = (u8*)malloc((size_t)len);
  if (!buf) { fclose(f); return NULL; }
  const size_t got = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (got != (size_t)len) { free(buf); return NULL; }
  *out_size = (size_t)len;
  return buf;
}

bool tc_load_png_rgba8(const char* path, u8** out_pixels, u32* out_w, u32* out_h)
{
  size_t bytes = 0;
  u8* file = slurp_file(path, &bytes);
  if (!file) return false;

  int w = 0, h = 0, channels = 0;
  /* Force RGBA8 regardless of source channel count; the consumer always
   * uploads as GPU_TEXTURE_FORMAT_RGBA8. */
  unsigned char* pixels =
      stbi_load_from_memory(file, (int)bytes, &w, &h, &channels, /*req_comp=*/4);
  free(file);
  if (!pixels) return false;
  if (w <= 0 || h <= 0) { stbi_image_free(pixels); return false; }

  *out_pixels = (u8*)pixels;
  *out_w      = (u32)w;
  *out_h      = (u32)h;
  return true;
}

void tc_free_image(u8* pixels)
{
  if (pixels) stbi_image_free(pixels);
}
