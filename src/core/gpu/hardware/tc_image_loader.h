/*
 * Thin RGBA8 PNG loader for texture-replacement packs.  Backed by stb_image
 * (vendored at dep/stb/stb_image.h), instantiated in tc_image_loader.c so the
 * stb warnings stay isolated to one TU.
 */

#ifndef CUPID_CORE_TC_IMAGE_LOADER_H
#define CUPID_CORE_TC_IMAGE_LOADER_H

#include "common/types.h"
#include <stdbool.h>

/* Loads `path` (PNG) into a freshly-malloc'd RGBA8 buffer.  On success the
 * caller owns `*out_pixels` and must release it with tc_free_image().  On
 * failure returns false; out parameters are left untouched. */
bool tc_load_png_rgba8(const char* path, u8** out_pixels, u32* out_w, u32* out_h);

void tc_free_image(u8* pixels);

#endif /* CUPID_CORE_TC_IMAGE_LOADER_H */
