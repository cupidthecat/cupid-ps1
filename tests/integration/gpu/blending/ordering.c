/* cupid-ps1 sequential and intra-command native blend ordering. */
#include "oracle.h"
#include "common/error.h"
#include "core/config/settings.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "util/graphics/opengl_texture.h"
#include <stdio.h>

static u16 step(u16 background, u32 mode, u32 flags, bool textured) {
  if ((flags & 2) && (background & 0x8000)) return background;
  return blend_test_combine(background, 0x4D23, mode) | ((textured || (flags & 1)) ? 0x8000 : 0);
}
static bool ordered(gpu_hw_t* hw, u32 kind, u32 mode, u32 flags, bool textured, bool bg_mask) {
  gpu_backend_t* be       = &hw->base;
  const u16      bg_color = (u16)(0x1234 | (bg_mask ? 0x8000 : 0));
  u16            words[32 * 32];
  for (u32 i = 0; i < 32 * 32; i++)
    words[i] = bg_color;
  const gpu_backend_update_vram_cmd_t bg = {.x = 64, .y = 64, .width = 32, .height = 32};
  be->vtable->update_vram(be, &bg, words);
  if (textured) {
    for (u32 i = 0; i < 32 * 32; i++)
      words[i] = 0xCD23;
    const gpu_backend_update_vram_cmd_t tex = {.x = 0, .y = 400, .width = 32, .height = 32};
    be->vtable->update_vram(be, &tex, words);
  }
  const u16              fg   = 0x4D23;
  const u32              rgb  = (fg & 31u) << 3 | (((u32)fg >> 5) & 31u) << 11 | (((u32)fg >> 10) & 31u) << 19;
  gpu_backend_draw_cmd_t base = {.texture_enable         = textured,
                                 .raw_texture_enable     = textured,
                                 .transparency_enable    = true,
                                 .num_vertices           = 4,
                                 .check_mask_before_draw = (flags & 2) != 0,
                                 .set_mask_while_drawing = (flags & 1) != 0,
                                 .draw_mode              = {.bits = (u16)(16u | 2u << 7 | mode << 5)},
                                 .window                 = {.and_x = 255, .and_y = 255}};
  const u32              c    = textured ? 0x808080 : rgb;
  if (kind == 0) {
    const gpu_backend_draw_rectangle_cmd_t cmd = {
        .base = base, .x = 64, .y = 64, .width = 16, .height = 16, .color = c, .texcoord = 144u << 8};
    be->vtable->draw_rectangle(be, &cmd);
    be->vtable->draw_rectangle(be, &cmd);
  } else if (kind == 1) {
    base.quad_polygon                                = true;
    const gpu_backend_draw_polygon_cmd_t cmd         = {.base = base};
    const gpu_backend_polygon_vertex_t   vertices[4] = {{.x = 64, .y = 64, .color = c, .u = 0, .v = 144},
                                                        {.x = 80, .y = 64, .color = c, .u = 0, .v = 144},
                                                        {.x = 64, .y = 80, .color = c, .u = 0, .v = 144},
                                                        {.x = 66, .y = 66, .color = c, .u = 0, .v = 144}};
    be->vtable->draw_polygon(be, &cmd, vertices);
  } else {
    const gpu_backend_draw_line_cmd_t cmd         = {.base = base};
    const gpu_backend_line_vertex_t   vertices[4] = {{.x = 64, .y = 70, .color = c},
                                                     {.x = 80, .y = 70, .color = c},
                                                     {.x = 64, .y = 70, .color = c},
                                                     {.x = 80, .y = 70, .color = c}};
    be->vtable->draw_line(be, &cmd, vertices);
  }
  const bool valid = hw->batch_index_count > 0;
  be->vtable->flush_render(be);
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  bool      pass      = valid && glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  const u32 points[3] = {70, 65, 90}, scale = hw->resolution_scale;
  for (u32 p = 0; p < 3; p++) {
    const u32 hits     = p == 2 ? 0 : kind == 0 ? 2 : kind == 1 ? (p == 0 ? 2 : 1) : (p == 0 ? 2 : 0);
    u16       expected = bg_color;
    for (u32 i = 0; i < hits; i++)
      expected = step(expected, mode, flags, textured);
    for (u32 sy = 0; sy < scale; sy++)
      for (u32 sx = 0; sx < scale; sx++) {
        u32 actual = 0;
        glReadPixels(points[p] * scale + sx, points[p] * scale + sy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &actual);
        if (blend_test_native(actual) != expected || ((actual >> 24) != 0 && (actual >> 24) != 255)) {
          if (pass)
            fprintf(stderr,
                    "Blend order kind=%u mode=%u flags=%u tex=%u bgmask=%u cache=%u scale=%u point=%u expected=%04X "
                    "actual=%08X\n",
                    kind, mode, flags, textured, bg_mask, hw->use_texture_cache, scale, p, expected, actual);
          pass = false;
        }
      }
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  return pass && glGetError() == GL_NO_ERROR;
}
u32 gpu_gl_blend_order_checks(const window_info_t* wi, u32* count) {
  const u8                   original_scale  = g_settings.gpu_resolution_scale;
  const bool                 original_cache  = g_settings.gpu_texture_cache;
  const gpu_dithering_mode_t original_dither = g_settings.gpu_dithering_mode;
  u32                        failures        = 0;
  for (u32 cache = 0; cache < 2; cache++)
    for (u32 scale = 1; scale <= 2; scale++) {
      g_settings.gpu_resolution_scale = (u8)scale;
      g_settings.gpu_texture_cache    = cache != 0;
      g_settings.gpu_dithering_mode   = GPU_DITHERING_MODE_UNSCALED;
      Error          error            = ERROR_INIT;
      gpu_backend_t* be               = gpu_backend_create_hardware_opengl(wi, &error);
      if (!be || ((gpu_hw_t*)be)->true_color || ((gpu_hw_t*)be)->use_texture_cache != (cache != 0)) {
        fprintf(stderr, "Blend order setup: %s\n", Error_get_description(&error));
        (*count)++;
        failures++;
        if (be) gpu_backend_destroy(be);
        Error_destroy(&error);
        continue;
      }
      const gpu_backend_set_drawing_area_cmd_t area = {.new_area = {.left = 64, .top = 64, .right = 95, .bottom = 95}};
      be->vtable->drawing_area_changed(be, &area);
      for (u32 mode = 0; mode < 4; mode++)
        for (u32 flags = 0; flags < 4; flags++)
          for (u32 bg = 0; bg < 2; bg++)
            for (u32 kind = 0; kind < 3; kind++)
              for (u32 tex = 0; tex < (kind == 2 ? 1u : 2u); tex++) {
                const bool pass = ordered((gpu_hw_t*)be, kind, mode, flags, tex != 0, bg != 0);
                (*count)++;
                failures += !pass;
                printf("%s blend-order mode=%u flags=%u bg=%u kind=%u tex=%u cache=%u scale=%u\n",
                       pass ? "PASS" : "FAIL", mode, flags, bg, kind, tex, cache, scale);
              }
      gpu_backend_destroy(be);
      Error_destroy(&error);
    }
  g_settings.gpu_resolution_scale = original_scale;
  g_settings.gpu_texture_cache    = original_cache;
  g_settings.gpu_dithering_mode   = original_dither;
  return failures;
}
