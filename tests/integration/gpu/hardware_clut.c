/* cupid-ps1 palette-cache ownership and ordered OpenGL draws. */
#include "hardware_clut.h"
#include "common/error.h"
#include "core/config/settings.h"
#include "core/state/save_state_version.h"
#include "util/state/state_wrapper.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "util/graphics/opengl_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 pixel(gpu_hw_t* hw, u32 x, u32 y) {
  GLint  old;
  GLuint fbo;
  u32    rgba = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels((GLint)(x * hw->resolution_scale), (GLint)(y * hw->resolution_scale), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
               &rgba);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  return rgba;
}

static bool row(gpu_hw_t* hw, u32 y, u32* rgba) {
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  const bool valid = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (valid)
    glReadPixels(0, (GLint)(y * hw->resolution_scale), (GLsizei)(1024 * hw->resolution_scale), hw->resolution_scale,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  return valid && glGetError() == GL_NO_ERROR;
}

static gpu_texture_palette_reg_t palette_reg(u32 y) {
  const gpu_texture_palette_reg_t reg = {.bits = (u16)((y << 6) | 63)};
  return reg;
}
static void palette(gpu_backend_t* be, bool m8, u32 y, u16 color) {
  u16 colors[256];
  for (u32 i = 0; i < 256; i++)
    colors[i] = (u16)((i * 1193) & 0x7FFF);
  colors[0]                               = 0;
  colors[1]                               = color;
  colors[2]                               = 0x03E0;
  colors[3]                               = 0x8000;
  colors[m8 ? 255 : 15]                   = 0x7C00;
  const gpu_backend_update_vram_cmd_t cmd = {.x = 1008, .y = y, .width = m8 ? 256 : 16, .height = 1};
  be->vtable->update_vram(be, &cmd, colors);
}
static void reload(gpu_backend_t* be, bool m8, u32 y) {
  const gpu_backend_update_clut_cmd_t cmd = {.reg = palette_reg(y), .clut_is_8bit = m8};
  be->vtable->update_clut(be, &cmd);
}
static void setup(gpu_backend_t* be, bool m8) {
  be->vtable->clear_vram(be);
  const u16                           indices = m8 ? 0x0101 : 0x1111;
  const gpu_backend_update_vram_cmd_t cmd     = {.x = 0, .y = 0, .width = 1, .height = 1};
  be->vtable->update_vram(be, &cmd, &indices);
  palette(be, m8, 511, 0x001F);
}
static bool draw(gpu_hw_t* hw, bool m8, u32 y, s32 x) {
  const gpu_backend_draw_rectangle_cmd_t cmd = {.base   = {.texture_enable     = true,
                                                           .raw_texture_enable = true,
                                                           .num_vertices       = 4,
                                                           .draw_mode          = {.bits = (u16)(m8 ? 1u << 7 : 0)},
                                                           .palette            = palette_reg(y),
                                                           .window             = {.bits = 0x0000FFFF}},
                                                .width  = 1,
                                                .height = 1,
                                                .x      = x,
                                                .y      = 100,
                                                .color  = 0x00808080};
  hw->base.vtable->draw_rectangle(&hw->base, &cmd);
  return hw->batch_index_count > 0 &&
         (hw->use_texture_cache
              ? (hw->batch.texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE && hw->batch_cache_texture != NULL)
              : hw->batch.texture_mode ==
                    (m8 ? GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT : GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT));
}

static bool preserves_vram(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  const size_t size   = (size_t)1024 * hw->resolution_scale * hw->resolution_scale * sizeof(u32);
  u32*         before = malloc(size);
  u32*         after  = malloc(size);
  if (!before || !after) {
    free(before);
    free(after);
    return false;
  }
  bool valid = row(hw, 511, before);
  reload(be, m8, 511);
  valid = row(hw, 511, after) && valid && memcmp(before, after, size) == 0;
  free(before);
  free(after);
  return valid;
}
static bool retained_and_reloaded(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  reload(be, m8, 511);
  bool valid = draw(hw, m8, 511, 400);
  be->vtable->flush_render(be);
  const u32 first = pixel(hw, 400, 100);
  palette(be, m8, 511, 0x03E0);
  valid = draw(hw, m8, 511, 401) && valid;
  be->vtable->flush_render(be);
  const u32 cached = pixel(hw, 401, 100);
  reload(be, m8, 511);
  valid = draw(hw, m8, 511, 402) && valid;
  be->vtable->flush_render(be);
  const u32 fresh = pixel(hw, 402, 100);
  printf("CLUT colors first=%08X retained=%08X reloaded=%08X\n", first, cached, fresh);
  return valid && first == 0x000000FF && cached == 0x000000FF && fresh == 0x0000FF00 && glGetError() == GL_NO_ERROR;
}
static bool ordered_reload(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  palette(be, m8, 510, 0x03E0);
  reload(be, m8, 511);
  bool valid = draw(hw, m8, 511, 400);
  reload(be, m8, 510);
  valid = draw(hw, m8, 510, 401) && valid;
  be->vtable->flush_render(be);
  const u32 first = pixel(hw, 400, 100), second = pixel(hw, 401, 100);
  printf("CLUT ordered first=%08X second=%08X\n", first, second);
  return valid && first == 0x000000FF && second == 0x0000FF00 && glGetError() == GL_NO_ERROR;
}

static bool saved_latch(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  reload(be, m8, 511);
  palette(be, m8, 511, 0x03E0);
  const size_t capacity = 2 * 1024 * 1024;
  u8*          saved    = malloc(capacity);
  if (!saved) return false;
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, capacity, SAVE_STATE_VERSION);
  bool         valid = be->vtable->do_state(be, &state);
  const size_t size  = state_wrapper_get_position(&state);
  reload(be, m8, 511);
  valid = draw(hw, m8, 511, 400) && valid;
  be->vtable->flush_render(be);
  valid = (pixel(hw, 400, 100) == 0x0000FF00) && valid;
  if (size > 0) {
    const size_t vram_size = sizeof(g_vram), clut_size = sizeof(g_gpu_clut);
    u8*          unchanged = malloc(vram_size + clut_size);
    if (!unchanged) {
      free(saved);
      return false;
    }
    memcpy(unchanged, g_vram, vram_size);
    memcpy(unchanged + vram_size, g_gpu_clut, clut_size);
    const size_t cuts[] = {0, vram_size / 2, vram_size - 1, vram_size, size - 1};
    for (u32 i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
      state_wrapper_init_read(&state, saved, cuts[i], SAVE_STATE_VERSION);
      valid = (!be->vtable->do_state(be, &state) && state_wrapper_has_error(&state)) && valid;
      valid =
          (memcmp(unchanged, g_vram, vram_size) == 0 && memcmp(unchanged + vram_size, g_gpu_clut, clut_size) == 0) &&
          valid;
    }
    free(unchanged);
    valid = draw(hw, m8, 511, 401) && valid;
    be->vtable->flush_render(be);
    valid = (pixel(hw, 401, 100) == 0x0000FF00) && valid;
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    valid = be->vtable->do_state(be, &state) && valid;
    valid = (pixel(hw, 1009, 511) == 0x0000FF00) && valid;
    valid = draw(hw, m8, 511, 402) && valid;
    be->vtable->flush_render(be);
    valid = (pixel(hw, 402, 100) == 0x000000FF) && valid;
  } else
    valid = false;
  free(saved);
  return valid && glGetError() == GL_NO_ERROR;
}

static bool resized_latch(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  reload(be, m8, 511);
  palette(be, m8, 511, 0x03E0);
  const u8 original = hw->resolution_scale, other = original == 1 ? 2 : 1;
  Error    error                  = ERROR_INIT;
  g_settings.gpu_resolution_scale = other;
  bool valid                      = gpu_hw_change_resolution_scale(hw, other, &error);
  if (valid) {
    valid = (pixel(hw, 1009, 511) == 0x0000FF00) && draw(hw, m8, 511, 400);
    be->vtable->flush_render(be);
    valid = (pixel(hw, 400, 100) == 0x000000FF) && valid;
  }
  g_settings.gpu_resolution_scale = original;
  valid                           = gpu_hw_change_resolution_scale(hw, original, &error) && valid;
  valid                           = draw(hw, m8, 511, 401) && valid;
  be->vtable->flush_render(be);
  valid = (pixel(hw, 401, 100) == 0x000000FF) && valid;
  if (!valid) fprintf(stderr, "Cached palette resize: %s\n", Error_get_description(&error));
  Error_destroy(&error);
  return valid && glGetError() == GL_NO_ERROR;
}

static bool clear_cache_order(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  reload(be, m8, 511);
  bool valid = draw(hw, m8, 511, 400);
  be->vtable->clear_cache(be);
  be->vtable->flush_render(be);
  valid = (pixel(hw, 400, 100) == 0x000000FF) && valid;
  palette(be, m8, 511, 0x03E0);
  reload(be, m8, 511);
  valid = draw(hw, m8, 511, 401) && valid;
  be->vtable->flush_render(be);
  return valid && pixel(hw, 401, 100) == 0x0000FF00 && glGetError() == GL_NO_ERROR;
}

static bool wrapped_max_index(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  const u16                           indices = 0xFFFF;
  const gpu_backend_update_vram_cmd_t cmd     = {.x = 0, .y = 0, .width = 1, .height = 1};
  be->vtable->update_vram(be, &cmd, &indices);
  reload(be, m8, 511);
  bool valid = draw(hw, m8, 511, 400);
  be->vtable->flush_render(be);
  valid                                       = (pixel(hw, 400, 100) == 0x00FF0000) && valid;
  const u16                           changed = 0x001F;
  const gpu_backend_update_vram_cmd_t tail = {.x = (1008 + (m8 ? 255 : 15)) % 1024, .y = 511, .width = 1, .height = 1};
  be->vtable->update_vram(be, &tail, &changed);
  valid = draw(hw, m8, 511, 401) && valid;
  be->vtable->flush_render(be);
  valid = (pixel(hw, 401, 100) == 0x00FF0000) && valid;
  reload(be, m8, 511);
  valid = draw(hw, m8, 511, 402) && valid;
  be->vtable->flush_render(be);
  return valid && pixel(hw, 402, 100) == 0x000000FF && glGetError() == GL_NO_ERROR;
}

static bool palette_mask(gpu_hw_t* hw, bool m8) {
  gpu_backend_t* be = &hw->base;
  setup(be, m8);
  palette(be, m8, 511, 0x801F);
  const u16                           indices[] = {m8 ? 0x0201 : 0x3021, m8 ? 0x0300 : 0};
  const gpu_backend_update_vram_cmd_t source    = {.x = 0, .y = 0, .width = 2, .height = 1};
  be->vtable->update_vram(be, &source, indices);
  reload(be, m8, 511);
  const gpu_backend_fill_vram_cmd_t fill = {.x = 400, .y = 100, .width = 4, .height = 1, .color = 0x00FF0000};
  be->vtable->fill_vram(be, &fill);
  const gpu_backend_draw_rectangle_cmd_t textured = {.base   = {.texture_enable     = true,
                                                                .raw_texture_enable = true,
                                                                .num_vertices       = 4,
                                                                .draw_mode          = {.bits = (u16)(m8 ? 1u << 7 : 0)},
                                                                .palette            = palette_reg(511),
                                                                .window             = {.bits = 0x0000FFFF}},
                                                     .width  = 4,
                                                     .height = 1,
                                                     .x      = 400,
                                                     .y      = 100,
                                                     .color  = 0x00808080};
  be->vtable->draw_rectangle(be, &textured);
  be->vtable->flush_render(be);
  bool valid = pixel(hw, 400, 100) == 0xFF0000FF && pixel(hw, 401, 100) == 0x0000FF00 &&
               pixel(hw, 402, 100) == 0x00FF0000 && pixel(hw, 403, 100) == 0xFF000000;
  const gpu_backend_draw_rectangle_cmd_t checked = {.base   = {.check_mask_before_draw = true, .num_vertices = 4},
                                                    .width  = 4,
                                                    .height = 1,
                                                    .x      = 400,
                                                    .y      = 100,
                                                    .color  = 0x00FF0000};
  be->vtable->draw_rectangle(be, &checked);
  be->vtable->flush_render(be);
  valid = pixel(hw, 400, 100) == 0xFF0000FF && pixel(hw, 401, 100) == 0x00FF0000 && pixel(hw, 402, 100) == 0x00FF0000 &&
          pixel(hw, 403, 100) == 0xFF000000 && valid;
  return valid && glGetError() == GL_NO_ERROR;
}

u32 gpu_gl_clut_checks(const window_info_t* wi, u32* count) {
  const bool original_cache = g_settings.gpu_texture_cache;
  const u8   original_scale = g_settings.gpu_resolution_scale;
  u32        failures       = 0;
  for (u32 cache = 0; cache < 2; cache++) {
    for (u32 scale = 1; scale <= 2; scale++) {
      g_settings.gpu_texture_cache    = cache != 0;
      g_settings.gpu_resolution_scale = (u8)scale;
      Error          error            = ERROR_INIT;
      gpu_backend_t* be               = gpu_backend_create_hardware_opengl(wi, &error);
      if (!be) {
        fprintf(stderr, "CLUT backend setup: %s\n", Error_get_description(&error));
        Error_destroy(&error);
        (*count)++;
        failures++;
        continue;
      }
      if (((gpu_hw_t*)be)->use_texture_cache != (cache != 0)) {
        fprintf(stderr, "Unexpected decoded-page cache configuration\n");
        (*count)++;
        failures++;
        gpu_backend_destroy(be);
        Error_destroy(&error);
        continue;
      }
      const gpu_backend_set_drawing_area_cmd_t area = {.new_area = {.left = 0, .top = 0, .right = 1023, .bottom = 511}};
      be->vtable->drawing_area_changed(be, &area);
      for (u32 mode = 0; mode < 2; mode++) {
        bool (*const checks[])(gpu_hw_t*, bool) = {preserves_vram,    retained_and_reloaded, ordered_reload,
                                                   saved_latch,       resized_latch,         clear_cache_order,
                                                   wrapped_max_index, palette_mask};
        for (u32 c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
          (*count)++;
          const bool valid = checks[c]((gpu_hw_t*)be, mode != 0);
          failures += !valid;
          printf("%s CLUT cache=%u scale=%u mode=%u case=%u\n", valid ? "PASS" : "FAIL", cache, scale, mode, c);
        }
      }
      gpu_backend_destroy(be);
      Error_destroy(&error);
    }
  }
  g_settings.gpu_texture_cache    = original_cache;
  g_settings.gpu_resolution_scale = original_scale;
  return failures;
}
