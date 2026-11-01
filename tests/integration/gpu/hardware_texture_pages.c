/* cupid-ps1 decoded texture-page coordinates and wrapped write invalidation. */
#include "hardware_texture_pages.h"
#include "common/error.h"
#include "core/config/settings.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "core/gpu/hardware/gpu_hw_texture_cache.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "util/graphics/opengl_texture.h"
#include <stdio.h>
#include <stdlib.h>

static u32 rgba(u16 pixel) {
  const u32 r = pixel & 31u, g = (pixel >> 5) & 31u, b = (pixel >> 10) & 31u;
  /* Readback is normalized RGBA8; round each five-bit channel to 0..255. */
  return ((r * 255u + 15u) / 31u) | (((g * 255u + 15u) / 31u) << 8) | (((b * 255u + 15u) / 31u) << 16) |
         ((pixel & 0x8000u) ? 0xFF000000u : 0);
}

static bool check_page(gpu_tc_source_key_t key, u32* pixels) {
  const gpu_tc_source_t* source = gpu_texture_cache_lookup_source(key, 0, 0, 256, 256, GPU_TC_PALETTE_RECORD_FLAG_NONE);
  if (!source) return false;
  opengl_texture_t* texture = gpu_texture_cache_source_get_texture(source);
  if (!texture) return false;
  GLint  old;
  GLuint fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  bool valid = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (valid) glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &fbo);
  if (!valid || glGetError() != GL_NO_ERROR) return false;
  const u32 shift = key.mode < GPU_TEXTURE_MODE_DIRECT_16BIT ? 2u - (u32)key.mode : 0;
  const u32 sx = (key.page & 15u) * 64u, sy = (key.page & 16u) * 16u;
  for (u32 y = 0; y < 256; y++) {
    for (u32 x = 0; x < 256; x++) {
      const u16 word  = g_vram[(sy + y) * 1024u + ((sx + (x >> shift)) & 1023u)];
      u16       color = word;
      if (key.mode == GPU_TEXTURE_MODE_PALETTE_4BIT) color = g_gpu_clut[(word >> ((x & 3u) * 4u)) & 15u];
      if (key.mode == GPU_TEXTURE_MODE_PALETTE_8BIT) color = g_gpu_clut[(word >> ((x & 1u) * 8u)) & 255u];
      if (pixels[y * 256 + x] != rgba(color)) {
        fprintf(stderr, "Texture page=%u mode=%u x=%u y=%u expected=%08X actual=%08X\n", key.page, key.mode, x, y,
                rgba(color), pixels[y * 256 + x]);
        return false;
      }
    }
  }
  return true;
}

u32 gpu_gl_texture_page_checks(const window_info_t* wi, u32* count) {
  const bool original_cache = g_settings.gpu_texture_cache;
  const u8   original_scale = g_settings.gpu_resolution_scale;
  u32        failures       = 0;
  u16*       words          = malloc(sizeof(g_vram));
  u32*       pixels         = malloc(256 * 256 * sizeof(u32));
  if (!words || !pixels) {
    free(words);
    free(pixels);
    (*count)++;
    return 1;
  }
  for (u32 i = 0; i < 1024u * 512u; i++)
    words[i] = (u16)((i * 271u + (i >> 10) * 31u) ^ (i >> 5));
  for (u32 scale = 1; scale <= 2; scale++) {
    g_settings.gpu_texture_cache    = true;
    g_settings.gpu_resolution_scale = (u8)scale;
    Error          error            = ERROR_INIT;
    gpu_backend_t* backend          = gpu_backend_create_hardware_opengl(wi, &error);
    if (!backend || !((gpu_hw_t*)backend)->use_texture_cache) {
      fprintf(stderr, "Texture-page setup failed: %s\n", Error_get_description(&error));
      failures++;
      (*count)++;
      if (backend) gpu_backend_destroy(backend);
      Error_destroy(&error);
      continue;
    }
    const gpu_backend_update_vram_cmd_t upload = {.width = 1024, .height = 512};
    backend->vtable->update_vram(backend, &upload, words);
    for (u32 mode = 0; mode < 4; mode++) {
      const gpu_backend_update_clut_cmd_t clut = {.reg = {.bits = 100u << 6}, .clut_is_8bit = mode != 0};
      backend->vtable->update_clut(backend, &clut);
      for (u32 page = 0; page < 32; page++) {
        const gpu_tc_source_key_t key   = {.page = (u8)page, .mode = (gpu_texture_mode_t)mode, .palette = clut.reg};
        bool                      valid = check_page(key, pixels);
        (*count)++;
        failures += !valid;
        printf("%s texture-page scale=%u mode=%u page=%u initial\n", valid ? "PASS" : "FAIL", scale, mode, page);
        const u32                           shift = mode < 2 ? 2u - mode : 0;
        const u32                           x     = ((page & 15u) * 64u + (255u >> shift)) & 1023u;
        const u32                           y     = (page & 16u) * 16u + 255u;
        const u16                           old = g_vram[y * 1024u + x], changed = (u16)(old ^ 0x7FFFu);
        const gpu_backend_update_vram_cmd_t patch = {.x = x, .y = y, .width = 1, .height = 1};
        backend->vtable->update_vram(backend, &patch, &changed);
        valid = check_page(key, pixels);
        (*count)++;
        failures += !valid;
        printf("%s texture-page scale=%u mode=%u page=%u rewritten\n", valid ? "PASS" : "FAIL", scale, mode, page);
        backend->vtable->update_vram(backend, &patch, &old);
      }
    }
    gpu_backend_destroy(backend);
    Error_destroy(&error);
  }
  g_settings.gpu_texture_cache    = original_cache;
  g_settings.gpu_resolution_scale = original_scale;
  free(words);
  free(pixels);
  return failures;
}
