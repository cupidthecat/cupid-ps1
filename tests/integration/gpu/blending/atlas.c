/* cupid-ps1 native framebuffer blend pairs, texture STP and mask controls. */
#include "util/platform/window_info.h"
#include "oracle.h"
#include "common/error.h"
#include "core/config/settings.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "util/graphics/opengl_texture.h"
#include <stdio.h>

static bool read_atlas(gpu_hw_t* hw, u32* pixels) {
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  const u32  scale    = hw->resolution_scale;
  if (complete) glReadPixels(64 * scale, 64 * scale, 32 * scale, 32 * scale, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  return complete && glGetError() == GL_NO_ERROR;
}
static void palette(gpu_backend_t* be, bool masked, u32 half, bool is8, bool special) {
  u16 colors[256] = {0};
  for (u32 i = 0; i < 32; i++)
    colors[i] = blend_test_foreground(i + (is8 ? 0 : half * 16), masked, special);
  const gpu_backend_update_vram_cmd_t upload = {.x = 0, .y = 300, .width = is8 ? 256 : 16, .height = 1};
  be->vtable->update_vram(be, &upload, colors);
  const gpu_backend_update_clut_cmd_t clut = {.reg = {.bits = 300u << 6}, .clut_is_8bit = is8};
  be->vtable->update_clut(be, &clut);
}
static bool atlas(gpu_hw_t* hw, u32 source, u32 mode, bool masked, u32 flags, bool polygon, bool transparent,
                  bool dither, bool special) {
  gpu_backend_t* be = &hw->base;
  u16            background[32 * 32];
  for (u32 y = 0; y < 32; y++)
    for (u32 x = 0; x < 32; x++)
      background[y * 32 + x] = blend_test_color(y, false) | ((y & 1) ? 0x8000 : 0);
  const gpu_backend_update_vram_cmd_t bg = {.x = 64, .y = 64, .width = 32, .height = 32};
  be->vtable->update_vram(be, &bg, background);
  if (source < 4) {
    u16       texture[32 * 32];
    const u32 words = source == 0 ? 8 : source == 1 ? 16 : 32;
    for (u32 y = 0; y < 32; y++)
      for (u32 x = 0; x < words; x++) {
        u16 value;
        if (source == 0)
          value = (u16)(((x * 4) & 15) | (((x * 4 + 1) & 15) << 4) | (((x * 4 + 2) & 15) << 8) |
                        (((x * 4 + 3) & 15) << 12));
        else if (source == 1)
          value = (u16)(x * 2 | ((x * 2 + 1) << 8));
        else
          value = blend_test_foreground(x, masked, special);
        texture[y * words + x] = value;
      }
    const gpu_backend_update_vram_cmd_t tex = {.x = 0, .y = 400, .width = words, .height = 32};
    be->vtable->update_vram(be, &tex, texture);
    if (source < 2) palette(be, masked, 0, source == 1, special);
  }
  bool valid = true;
  for (u32 f = 0; f < 32; f++) {
    if (source == 0 && f == 16) palette(be, masked, 1, false, special);
    const u16                    fg   = blend_test_color(f, true);
    const u32                    rgb  = (fg & 31u) << 3 | (((u32)fg >> 5) & 31u) << 11 | (((u32)fg >> 10) & 31u) << 19;
    const gpu_backend_draw_cmd_t base = {
        .texture_enable         = source < 4,
        .raw_texture_enable     = source < 4,
        .transparency_enable    = transparent,
        .dither_enable          = dither,
        .num_vertices           = 4,
        .quad_polygon           = polygon,
        .check_mask_before_draw = (flags & 2) != 0,
        .set_mask_while_drawing = (flags & 1) != 0,
        .draw_mode              = {.bits = (u16)(16u | ((source < 4 ? source : 0) << 7) | (mode << 5))},
        .palette                = {.bits = 300u << 6},
        .window                 = {.and_x = 255, .and_y = 255}};
    if (polygon) {
      const gpu_backend_draw_polygon_cmd_t command     = {.base = base};
      const u32                            c           = source < 4 ? 0x808080 : rgb;
      const gpu_backend_polygon_vertex_t   vertices[4] = {
          {.x = 64 + (s32)f, .y = 64, .color = c, .u = (u8)f, .v = 144},
          {.x = 65 + (s32)f, .y = 64, .color = c, .u = (u8)(f + 1), .v = 144},
          {.x = 64 + (s32)f, .y = 96, .color = c, .u = (u8)f, .v = 176},
          {.x = 65 + (s32)f, .y = 96, .color = c, .u = (u8)(f + 1), .v = 176}};
      be->vtable->draw_polygon(be, &command, vertices);
    } else {
      const gpu_backend_draw_rectangle_cmd_t command = {.base     = base,
                                                        .x        = 64 + (s32)f,
                                                        .y        = 64,
                                                        .width    = 1,
                                                        .height   = 32,
                                                        .color    = source < 4 ? 0x808080 : rgb,
                                                        .texcoord = (u16)(f | 144u << 8)};
      be->vtable->draw_rectangle(be, &command);
    }
    const gpu_hw_batch_texture_mode_t expected_mode = source == 4             ? GPU_HW_BATCH_TEXTURE_MODE_DISABLED
                                                      : hw->use_texture_cache ? GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE
                                                      : source == 0           ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT
                                                      : source == 1           ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT
                                                                              : GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT;
    valid = hw->batch_index_count > 0 && hw->batch.texture_mode == expected_mode && valid;
    if (source < 4 && hw->use_texture_cache) valid = hw->batch_cache_texture != NULL && valid;
    be->vtable->flush_render(be);
  }
  u32 pixels[32 * 32 * 4];
  if (!read_atlas(hw, pixels)) return false;
  const u32 scale = hw->resolution_scale;
  u32       wrong = 0;
  for (u32 y = 0; y < 32 * scale; y++)
    for (u32 x = 0; x < 32 * scale; x++) {
      const u16 bg_color = background[(y / scale) * 32 + x / scale];
      u16       expected = bg_color;
      if (!(flags & 2) || !(bg_color & 0x8000)) {
        const u16 word = blend_test_foreground(x / scale, masked, special);
        u16       fg   = word & 0x7FFF;
        if (dither && source == 4) {
          const bool scaled = g_settings.gpu_dithering_mode == GPU_DITHERING_MODE_SCALED;
          fg =
              blend_test_dither(fg, scaled ? 64 * scale + x : 64 + x / scale, scaled ? 64 * scale + y : 64 + y / scale);
        }
        const bool stp = source < 4 && (word & 0x8000);
        if (source == 4 || word != 0) {
          expected = transparent && (source == 4 || stp) ? blend_test_combine(bg_color, fg, mode) : fg;
          expected |= ((flags & 1) || stp) ? 0x8000 : 0;
        }
      }
      const u32 actual = pixels[y * 32 * scale + x];
      if (blend_test_native(actual) != expected || ((actual >> 24) != 0 && (actual >> 24) != 255)) {
        if (wrong == 0)
          fprintf(stderr,
                  "Blend atlas source=%u mode=%u mask=%u flags=%u poly=%u trans=%u dither=%u cache=%u scale=%u f=%u "
                  "b=%u expected=%04X actual=%08X\n",
                  source, mode, masked, flags, polygon, transparent, dither, hw->use_texture_cache, scale, x / scale,
                  y / scale, expected, actual);
        wrong++;
      }
    }
  return valid && wrong == 0;
}
static bool queued_dither(gpu_hw_t* hw, u32 source, bool raw_first) {
  gpu_backend_t*                      be                  = &hw->base;
  const u16                           background[32 * 32] = {0};
  const gpu_backend_update_vram_cmd_t bg                  = {.x = 64, .y = 64, .width = 32, .height = 32};
  be->vtable->update_vram(be, &bg, background);
  const u16                           fg      = blend_test_color(1, true);
  const u16                           texel   = source == 0 ? 0x1111 : fg;
  const gpu_backend_update_vram_cmd_t texture = {.x = 0, .y = 400, .width = 1, .height = 1};
  be->vtable->update_vram(be, &texture, &texel);
  if (source == 0) {
    const u16                           colors[16] = {[1] = fg};
    const gpu_backend_update_vram_cmd_t upload     = {.x = 0, .y = 300, .width = 16, .height = 1};
    be->vtable->update_vram(be, &upload, colors);
    const gpu_backend_update_clut_cmd_t clut = {.reg = {.bits = 300u << 6}};
    be->vtable->update_clut(be, &clut);
  }
  bool valid = true;
  for (u32 draw = 0; draw < 2; draw++) {
    const gpu_backend_draw_polygon_cmd_t command     = {.base = {.texture_enable     = true,
                                                                 .raw_texture_enable = (draw == 0) == raw_first,
                                                                 .dither_enable      = true,
                                                                 .num_vertices       = 4,
                                                                 .quad_polygon       = true,
                                                                 .draw_mode          = {.bits = (u16)(16u | source << 7)},
                                                                 .palette            = {.bits = 300u << 6},
                                                                 .window             = {.and_x = 255, .and_y = 255}}};
    const s32                            x           = 64 + (s32)draw * 8;
    const gpu_backend_polygon_vertex_t   vertices[4] = {{.x = x, .y = 64, .color = 0x808080, .u = 0, .v = 144},
                                                        {.x = x + 4, .y = 64, .color = 0x808080, .u = 0, .v = 144},
                                                        {.x = x, .y = 68, .color = 0x808080, .u = 0, .v = 144},
                                                        {.x = x + 4, .y = 68, .color = 0x808080, .u = 0, .v = 144}};
    be->vtable->draw_polygon(be, &command, vertices);
    const gpu_hw_batch_texture_mode_t expected_mode = hw->use_texture_cache ? GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE
                                                      : source == 0         ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT
                                                                            : GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT;
    valid = hw->batch_index_count > 0 && hw->batch.texture_mode == expected_mode && valid;
    if (hw->use_texture_cache) valid = hw->batch_cache_texture != NULL && valid;
  }
  be->vtable->flush_render(be);
  u32 pixels[32 * 32 * 4];
  if (!read_atlas(hw, pixels)) return false;
  const u32 scale = hw->resolution_scale;
  for (u32 y = 0; y < 32 * scale; y++)
    for (u32 x = 0; x < 32 * scale; x++) {
      const u32 nx = x / scale, ny = y / scale;
      u16       expected = 0;
      if (ny < 4 && (nx < 4 || (nx >= 8 && nx < 12))) {
        const bool raw = (nx < 4) == raw_first;
        expected       = raw ? fg : blend_test_dither(fg, 64 + nx, 64 + ny);
      }
      const u32 actual = pixels[y * 32 * scale + x];
      if (blend_test_native(actual) != expected || ((actual >> 24) != 0 && (actual >> 24) != 255)) {
        fprintf(stderr, "Queued dither source=%u raw_first=%u cache=%u scale=%u x=%u y=%u expected=%04X actual=%08X\n",
                source, raw_first, hw->use_texture_cache, scale, x, y, expected, actual);
        return false;
      }
    }
  return valid;
}
u32 gpu_gl_blend_atlas_checks(const window_info_t* wi, u32* count) {
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
        fprintf(stderr, "Blend atlas setup: %s\n", Error_get_description(&error));
        (*count)++;
        failures++;
        if (be) gpu_backend_destroy(be);
        Error_destroy(&error);
        continue;
      }
      const gpu_backend_set_drawing_area_cmd_t area = {.new_area = {.left = 64, .top = 64, .right = 95, .bottom = 95}};
      be->vtable->drawing_area_changed(be, &area);
      for (u32 source = 0; source < 5; source++)
        for (u32 mask = 0; mask < (source < 4 ? 2u : 1u); mask++)
          for (u32 poly = 0; poly < 2; poly++) {
            for (u32 flags = 0; flags < 4; flags++)
              for (u32 mode = 0; mode < 5; mode++) {
                const bool pass =
                    atlas((gpu_hw_t*)be, source, mode & 3u, mask != 0, flags, poly != 0, mode < 4, false, false);
                (*count)++;
                failures += !pass;
                printf("%s blend-atlas source=%u mask=%u poly=%u flags=%u mode=%u cache=%u scale=%u\n",
                       pass ? "PASS" : "FAIL", source, mask, poly, flags, mode, cache, scale);
              }
            if (poly)
              for (u32 mode = 0; mode < 4; mode++) {
                const bool pass = atlas((gpu_hw_t*)be, source, mode, mask != 0, 0, true, true, true, false);
                (*count)++;
                failures += !pass;
                printf("%s blend-dither source=%u mask=%u mode=%u cache=%u scale=%u\n", pass ? "PASS" : "FAIL", source,
                       mask, mode, cache, scale);
              }
          }
      for (u32 source = 0; source < 4; source++)
        for (u32 flags = 0; flags < 4; flags++)
          for (u32 mode = 0; mode < 5; mode++) {
            const bool pass = atlas((gpu_hw_t*)be, source, mode & 3u, false, flags, true, mode < 4, false, true);
            (*count)++;
            failures += !pass;
            printf("%s blend-special source=%u flags=%u mode=%u cache=%u scale=%u\n", pass ? "PASS" : "FAIL", source,
                   flags, mode, cache, scale);
          }
      g_settings.gpu_dithering_mode = GPU_DITHERING_MODE_SCALED;
      for (u32 mode = 0; mode < 4; mode++) {
        const bool pass = atlas((gpu_hw_t*)be, 4, mode, false, 0, true, true, true, false);
        (*count)++;
        failures += !pass;
        printf("%s blend-scaled-dither mode=%u cache=%u scale=%u\n", pass ? "PASS" : "FAIL", mode, cache, scale);
      }
      g_settings.gpu_dithering_mode = GPU_DITHERING_MODE_UNSCALED;
      for (u32 source = 0; source <= 2; source += 2)
        for (u32 raw_first = 0; raw_first < 2; raw_first++) {
          const bool pass = queued_dither((gpu_hw_t*)be, source, raw_first != 0);
          (*count)++;
          failures += !pass;
          printf("%s blend-queued source=%u raw_first=%u cache=%u scale=%u\n", pass ? "PASS" : "FAIL", source,
                 raw_first, cache, scale);
        }
      gpu_backend_destroy(be);
      Error_destroy(&error);
    }
  g_settings.gpu_resolution_scale = original_scale;
  g_settings.gpu_texture_cache    = original_cache;
  g_settings.gpu_dithering_mode   = original_dither;
  return failures;
}
