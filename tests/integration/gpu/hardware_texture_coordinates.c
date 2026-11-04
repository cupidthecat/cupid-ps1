/* cupid-ps1 rendered native texture coordinates, windows and rectangle flips. */
#include "hardware_texture_coordinates.h"
#include "common/error.h"
#include "core/config/settings.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "util/graphics/opengl_texture.h"
#include <stdio.h>
#include <stdlib.h>

static u16 native_color(u32 rgba) {
  return (u16)(((rgba & 255u) >> 3) | ((((rgba >> 8) & 255u) >> 3) << 5) | ((((rgba >> 16) & 255u) >> 3) << 10) |
               ((rgba >> 31) << 15));
}
static bool read_output(gpu_hw_t* hw, u32* pixels) {
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  bool      valid = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  const u32 scale = hw->resolution_scale;
  if (valid) glReadPixels(400 * scale, 100 * scale, 48 * scale, 8 * scale, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  return valid && glGetError() == GL_NO_ERROR;
}
static gpu_texture_window_t window(u32 index) {
  const gpu_texture_window_t windows[] = {{.and_x = 255, .and_y = 255, .or_x = 0, .or_y = 0},
                                          {.and_x = 0xC7, .and_y = 0xB7, .or_x = 0x18, .or_y = 0x48},
                                          {.and_x = 7, .and_y = 7, .or_x = 0xA8, .or_y = 0xD0}};
  return windows[index];
}
static u16 expected(const u16* words, u32 page, u32 mode, gpu_texture_window_t tw, u32 u, u32 v) {
  u               = ((u & 255u) & tw.and_x) | tw.or_x;
  v               = ((v & 255u) & tw.and_y) | tw.or_y;
  const u32 shift = mode < 2 ? 2u - mode : 0;
  const u32 x     = ((page & 15u) * 64u + (u >> shift)) & 1023u;
  const u32 y     = (page & 16u) * 16u + v;
  const u16 word  = words[y * 1024u + x];
  if (mode == 0) return words[180u * 1024u + ((word >> ((u & 3u) * 4u)) & 15u)];
  if (mode == 1) return words[180u * 1024u + ((word >> ((u & 1u) * 8u)) & 255u)];
  return word;
}
static bool check(gpu_hw_t* hw, const u16* words, u32 page, u32 mode, u32 shape, u32 flips, u32 win) {
  gpu_backend_t*                    be    = &hw->base;
  const gpu_backend_fill_vram_cmd_t clear = {.x = 400, .y = 100, .width = 48, .height = 8, .color = 0xFF00FF};
  be->vtable->fill_vram(be, &clear);
  const u8               origins[3][2] = {{0, 0}, {125, 237}, {shape ? 248 : 252, shape ? 248 : 252}};
  const u32              width         = shape ? 7 : 8;
  gpu_backend_draw_cmd_t base          = {.texture_enable     = true,
                                          .raw_texture_enable = true,
                                          .num_vertices       = 4,
                                          .quad_polygon       = shape != 0,
                                          .draw_mode          = {.bits = (u16)(page | (mode << 7) | (flips << 12))},
                                          .palette            = {.bits = 180u << 6},
                                          .window             = window(win)};
  bool                   valid         = true;
  for (u32 part = 0; part < 3; part++) {
    const s32 x = 400 + (s32)part * 16;
    if (shape) {
      gpu_backend_polygon_vertex_t vertices[4] = {
          {.x = x, .y = 100, .color = 0x808080, .u = origins[part][0], .v = origins[part][1]},
          {.x     = x + (s32)width,
           .y     = 100,
           .color = 0x808080,
           .u     = (u8)(origins[part][0] + width),
           .v     = origins[part][1]},
          {.x     = x,
           .y     = 100 + (s32)width,
           .color = 0x808080,
           .u     = origins[part][0],
           .v     = (u8)(origins[part][1] + width)},
          {.x     = x + (s32)width,
           .y     = 100 + (s32)width,
           .color = 0x808080,
           .u     = (u8)(origins[part][0] + width),
           .v     = (u8)(origins[part][1] + width)}};
      const gpu_backend_draw_polygon_cmd_t command = {.base = base};
      be->vtable->draw_polygon(be, &command, vertices);
    } else {
      const gpu_backend_draw_rectangle_cmd_t command = {.base     = base,
                                                        .width    = 8,
                                                        .height   = 8,
                                                        .x        = x,
                                                        .y        = 100,
                                                        .color    = 0x808080,
                                                        .texcoord = (u16)(origins[part][0] | origins[part][1] << 8)};
      be->vtable->draw_rectangle(be, &command);
    }
    const gpu_hw_batch_texture_mode_t mode_expected = hw->use_texture_cache ? GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE
                                                      : mode == 0           ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT
                                                      : mode == 1           ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT
                                                                            : GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT;
    valid = hw->batch_index_count > 0 && hw->batch.texture_mode == mode_expected && valid;
    if (hw->use_texture_cache) valid = hw->batch_cache_texture != NULL && valid;
    be->vtable->flush_render(be);
  }
  u32 pixels[48 * 8 * 4];
  if (!read_output(hw, pixels)) return false;
  const u32 scale = hw->resolution_scale;
  for (u32 y = 0; y < 8 * scale; y++)
    for (u32 x = 0; x < 48 * scale; x++) {
      const u32 part = x / (16 * scale), dx = (x / scale) % 16, dy = y / scale;
      u16       color = 0x7C1F;
      if (dx < width && dy < width) {
        const u32 u       = (flips & 1) && !shape ? ((origins[part][0] | 1u) + 256u - dx) : origins[part][0] + dx;
        const u32 v       = (flips & 2) && !shape ? origins[part][1] + 256u - dy : origins[part][1] + dy;
        const u16 sampled = expected(words, page, mode, base.window, u, v);
        if (sampled) color = sampled;
      }
      const u32 actual = pixels[y * 48 * scale + x];
      if (native_color(actual) != color || ((actual >> 24) != 0 && (actual >> 24) != 255)) {
        fprintf(stderr,
                "Rendered UV cache=%u scale=%u page=%u mode=%u shape=%u flip=%u window=%u part=%u x=%u y=%u "
                "expected=%04X actual=%08X native=%04X\n",
                hw->use_texture_cache, scale, page, mode, shape, flips, win, part, x, y, color, actual,
                native_color(actual));
        return false;
      }
    }
  return valid;
}
static bool large_rectangle(gpu_hw_t* hw, const u16* words, u32* pixels, u32 mode, u32 flips, u32 index, bool clip,
                            bool vertical) {
  const u16      widths[]  = {1, 3, 17, 255, 256, 257, 513, 1023};
  const u16      heights[] = {1, 3, 17, 255, 256, 257, 509, 511};
  const u8       origins[] = {0, 1, 254, 255};
  const u32      width = vertical ? 5u : widths[index], height = vertical ? heights[index] : 5u;
  const u32      origin  = origins[index % 4];
  const s32      start_x = vertical ? 400 : clip ? -3 : 0, start_y = vertical ? (clip ? -3 : 0) : 100;
  const u32      left = vertical ? 400u : clip ? 5u : 0u, right = vertical ? 404u : clip ? 1007u : 1023u;
  const u32      top = vertical ? (clip ? 5u : 0u) : 100u, bottom = vertical ? (clip ? 503u : 511u) : 104u;
  const u32      read_x = vertical ? 400u : 0u, read_y = vertical ? 0u : 100u;
  const u32      read_width = vertical ? 16u : 1024u, read_height = vertical ? 512u : 5u;
  gpu_backend_t* be                       = &hw->base;
  const gpu_backend_fill_vram_cmd_t clear = {
      .x = read_x, .y = read_y, .width = read_width, .height = read_height, .color = 0xFF00FF};
  be->vtable->fill_vram(be, &clear);
  const gpu_backend_set_drawing_area_cmd_t area = {
      .new_area = {.left = left, .top = top, .right = right, .bottom = bottom}};
  be->vtable->drawing_area_changed(be, &area);
  const gpu_backend_draw_rectangle_cmd_t command = {
      .base     = {.texture_enable     = true,
                   .raw_texture_enable = true,
                   .num_vertices       = 4,
                   .draw_mode          = {.bits = (u16)(31u | (mode << 7) | (flips << 12))},
                   .palette            = {.bits = 180u << 6},
                   .window             = {.and_x = 255, .and_y = 255}},
      .width    = (u16)width,
      .height   = (u16)height,
      .x        = start_x,
      .y        = start_y,
      .color    = 0x808080,
      .texcoord = (u16)(origin | origin << 8)};
  be->vtable->draw_rectangle(be, &command);
  const bool visible = start_x + (s32)width > (s32)left && start_y + (s32)height > (s32)top;
  bool       valid =
      !visible ||
      (hw->batch_index_count > 0 &&
       (hw->use_texture_cache
            ? hw->batch.texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE && hw->batch_cache_texture != NULL
            : hw->batch.texture_mode == (mode == 0   ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT
                                         : mode == 1 ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT
                                                     : GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT)));
  be->vtable->flush_render(be);
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  const u32  scale    = hw->resolution_scale;
  const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete)
    glReadPixels(read_x * scale, read_y * scale, read_width * scale, read_height * scale, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  valid                                         = complete && glGetError() == GL_NO_ERROR && valid;
  const gpu_backend_set_drawing_area_cmd_t full = {.new_area = {.left = 0, .top = 0, .right = 1023, .bottom = 511}};
  be->vtable->drawing_area_changed(be, &full);
  if (!valid) return false;
  for (u32 y = 0; y < read_height * scale; y++)
    for (u32 x = 0; x < read_width * scale; x++) {
      const u32 nx = read_x + x / scale, ny = read_y + y / scale;
      const s32 dx = (s32)nx - start_x, dy = (s32)ny - start_y;
      u16       color = 0x7C1F;
      if (nx >= left && nx <= right && ny >= top && ny <= bottom && dx >= 0 && (u32)dx < width && dy >= 0 &&
          (u32)dy < height) {
        const u32 u       = (flips & 1) ? (origin | 1u) + 2048u - (u32)dx : origin + (u32)dx;
        const u32 v       = (flips & 2) ? origin + 1024u - (u32)dy : origin + (u32)dy;
        const u16 sampled = expected(words, 31, mode, command.base.window, u, v);
        if (sampled) color = sampled;
      }
      const u32 actual = pixels[y * read_width * scale + x];
      if (native_color(actual) != color || ((actual >> 24) != 0 && (actual >> 24) != 255)) {
        fprintf(stderr,
                "Long rectangle cache=%u scale=%u mode=%u flip=%u width=%u height=%u clip=%u x=%u y=%u expected=%04X "
                "actual=%08X\n",
                hw->use_texture_cache, scale, mode, flips, width, height, clip, x, y, color, actual);
        return false;
      }
    }
  return true;
}
u32 gpu_gl_texture_coordinate_checks(const window_info_t* wi, u32* count) {
  const bool original_cache = g_settings.gpu_texture_cache;
  const u8   original_scale = g_settings.gpu_resolution_scale;
  u16*       words          = malloc(sizeof(g_vram));
  u32*       large_pixels   = malloc(1024 * 8 * 4 * sizeof(u32));
  if (!words || !large_pixels) {
    free(words);
    free(large_pixels);
    (*count)++;
    return 1;
  }
  for (u32 i = 0; i < 1024u * 512u; i++)
    words[i] = (u16)((i * 271u + (i >> 10) * 31u) ^ (i >> 5));
  u32 failures = 0;
  for (u32 cache = 0; cache < 2; cache++)
    for (u32 scale = 1; scale <= 2; scale++) {
      g_settings.gpu_texture_cache    = cache != 0;
      g_settings.gpu_resolution_scale = (u8)scale;
      Error          error            = ERROR_INIT;
      gpu_backend_t* be               = gpu_backend_create_hardware_opengl(wi, &error);
      if (!be || ((gpu_hw_t*)be)->use_texture_cache != (cache != 0)) {
        fprintf(stderr, "Rendered coordinate setup: %s\n", Error_get_description(&error));
        (*count)++;
        failures++;
        if (be) gpu_backend_destroy(be);
        Error_destroy(&error);
        continue;
      }
      const gpu_backend_update_vram_cmd_t upload = {.width = 1024, .height = 512};
      be->vtable->update_vram(be, &upload, words);
      const gpu_backend_set_drawing_area_cmd_t area = {.new_area = {.left = 0, .top = 0, .right = 1023, .bottom = 511}};
      be->vtable->drawing_area_changed(be, &area);
      for (u32 mode = 0; mode < 4; mode++) {
        const gpu_backend_update_clut_cmd_t clut = {.reg = {.bits = 180u << 6}, .clut_is_8bit = mode != 0};
        be->vtable->update_clut(be, &clut);
        for (u32 page = 0; page < 32; page++)
          for (u32 shape = 0; shape < 2; shape++) {
            const bool valid = check((gpu_hw_t*)be, words, page, mode, shape, 0, 0);
            (*count)++;
            failures += !valid;
            printf("%s rendered-uv cache=%u scale=%u mode=%u page=%u shape=%u\n", valid ? "PASS" : "FAIL", cache, scale,
                   mode, page, shape);
          }
        const u32 edge_pages[] = {0, 15, 16, 31};
        for (u32 page = 0; page < 4; page++)
          for (u32 flip = 0; flip < 4; flip++)
            for (u32 win = 0; win < 3; win++) {
              const bool valid = check((gpu_hw_t*)be, words, edge_pages[page], mode, 0, flip, win);
              (*count)++;
              failures += !valid;
              printf("%s rendered-flip cache=%u scale=%u mode=%u page=%u flip=%u window=%u\n", valid ? "PASS" : "FAIL",
                     cache, scale, mode, edge_pages[page], flip, win);
            }
        for (u32 flip = 0; flip < 4; flip++)
          for (u32 index = 0; index < 8; index++)
            for (u32 clip = 0; clip < 2; clip++)
              for (u32 vertical = 0; vertical < 2; vertical++) {
                const bool valid =
                    large_rectangle((gpu_hw_t*)be, words, large_pixels, mode, flip, index, clip != 0, vertical != 0);
                (*count)++;
                failures += !valid;
                printf("%s rendered-long cache=%u scale=%u mode=%u flip=%u index=%u clip=%u vertical=%u\n",
                       valid ? "PASS" : "FAIL", cache, scale, mode, flip, index, clip, vertical);
              }
      }
      gpu_backend_destroy(be);
      Error_destroy(&error);
    }
  free(words);
  free(large_pixels);
  g_settings.gpu_texture_cache    = original_cache;
  g_settings.gpu_resolution_scale = original_scale;
  return failures;
}
