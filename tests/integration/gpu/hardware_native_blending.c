/* cupid-ps1 native five-bit framebuffer blending and draw ordering. */
#include "hardware_native_blending.h"
#include "blending/oracle.h"
#include "common/error.h"
#include "core/config/settings.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "util/graphics/opengl_texture.h"
#include <stdio.h>

static u16 color(u32 value, bool foreground) {
  const u32 g = (value * (foreground ? 5u : 7u) + (foreground ? 7u : 3u)) & 31u;
  const u32 b = (value * (foreground ? 13u : 11u) + (foreground ? 1u : 5u)) & 31u;
  return (u16)(value | (g << 5) | (b << 10));
}
static u16 native(u32 rgba) {
  return (u16)(((rgba & 255u) >> 3) | ((((rgba >> 8) & 255u) >> 3) << 5) | ((((rgba >> 16) & 255u) >> 3) << 10) |
               ((rgba >> 31) << 15));
}
static u16 blended(u16 background, u16 foreground, u32 mode) {
  u32 result = 0;
  for (u32 channel = 0; channel < 3; channel++) {
    const s32 b = (background >> (channel * 5)) & 31, f = (foreground >> (channel * 5)) & 31;
    s32       value = mode == 0 ? (b + f) / 2 : mode == 1 ? b + f : mode == 2 ? b - f : b + f / 4;
    if (value < 0) value = 0;
    if (value > 31) value = 31;
    result |= (u32)value << (channel * 5);
  }
  return (u16)result;
}
static bool atlas(gpu_hw_t* hw, u32 mode) {
  gpu_backend_t* be = &hw->base;
  u16            input[32 * 32];
  for (u32 y = 0; y < 32; y++)
    for (u32 x = 0; x < 32; x++)
      input[y * 32 + x] = color(y, false);
  const gpu_backend_update_vram_cmd_t upload = {.x = 64, .y = 64, .width = 32, .height = 32};
  be->vtable->update_vram(be, &upload, input);
  bool valid = true;
  for (u32 f = 0; f < 32; f++) {
    const u16 fg  = color(f, true);
    const u32 rgb = ((u32)(fg & 31) << 3) | (((u32)(fg >> 5) & 31) << 11) | (((u32)(fg >> 10) & 31) << 19);
    const gpu_backend_draw_rectangle_cmd_t command = {.base   = {.transparency_enable = true,
                                                                 .num_vertices        = 4,
                                                                 .draw_mode           = {.bits = (u16)(mode << 5)},
                                                                 .window              = {.and_x = 255, .and_y = 255}},
                                                      .x      = 64 + (s32)f,
                                                      .y      = 64,
                                                      .width  = 1,
                                                      .height = 32,
                                                      .color  = rgb};
    be->vtable->draw_rectangle(be, &command);
    valid = hw->batch_index_count > 0 && hw->batch.texture_mode == GPU_HW_BATCH_TEXTURE_MODE_DISABLED && valid;
    be->vtable->flush_render(be);
  }
  u32    pixels[32 * 32 * 4];
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  const u32  scale    = hw->resolution_scale;
  const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete) glReadPixels(64 * scale, 64 * scale, 32 * scale, 32 * scale, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  if (!complete || glGetError() != GL_NO_ERROR) return false;
  u32 failures = 0;
  for (u32 y = 0; y < 32 * scale; y++)
    for (u32 x = 0; x < 32 * scale; x++) {
      const u16 expected = blended(color(y / scale, false), color(x / scale, true), mode);
      const u32 actual   = pixels[y * 32 * scale + x];
      if (native(actual) != expected) {
        if (failures == 0)
          fprintf(stderr, "Native blend mode=%u scale=%u f=%u b=%u expected=%04X actual=%08X native=%04X\n", mode,
                  scale, x / scale, y / scale, expected, actual, native(actual));
        failures++;
      }
    }
  printf("Native blend mode=%u scale=%u wrong_pixels=%u\n", mode, scale, failures);
  return valid && failures == 0;
}
u32 gpu_gl_native_blend_checks(const window_info_t* wi, u32* count) {
  const u8                   original_scale     = g_settings.gpu_resolution_scale;
  const bool                 original_cache     = g_settings.gpu_texture_cache;
  const gpu_dithering_mode_t original_dithering = g_settings.gpu_dithering_mode;
  u32                        failures           = 0;
  for (u32 scale = 1; scale <= 2; scale++) {
    g_settings.gpu_resolution_scale = (u8)scale;
    g_settings.gpu_texture_cache    = false;
    g_settings.gpu_dithering_mode   = GPU_DITHERING_MODE_UNSCALED;
    Error          error            = ERROR_INIT;
    gpu_backend_t* be               = gpu_backend_create_hardware_opengl(wi, &error);
    if (!be) {
      fprintf(stderr, "Blend setup: %s\n", Error_get_description(&error));
      (*count)++;
      failures++;
      Error_destroy(&error);
      continue;
    }
    if (((gpu_hw_t*)be)->true_color) {
      fprintf(stderr, "Native blending fixture unexpectedly uses true color\n");
      (*count)++;
      failures++;
      gpu_backend_destroy(be);
      Error_destroy(&error);
      continue;
    }
    const gpu_backend_set_drawing_area_cmd_t area = {.new_area = {.left = 64, .top = 64, .right = 95, .bottom = 95}};
    be->vtable->drawing_area_changed(be, &area);
    for (u32 mode = 0; mode < 4; mode++) {
      const bool valid = atlas((gpu_hw_t*)be, mode);
      (*count)++;
      failures += !valid;
      printf("%s native-blend scale=%u mode=%u\n", valid ? "PASS" : "FAIL", scale, mode);
    }
    gpu_backend_destroy(be);
    Error_destroy(&error);
  }
  g_settings.gpu_resolution_scale = original_scale;
  g_settings.gpu_texture_cache    = original_cache;
  g_settings.gpu_dithering_mode   = original_dithering;
  failures += gpu_gl_blend_atlas_checks(wi, count);
  failures += gpu_gl_blend_order_checks(wi, count);
  return failures;
}
