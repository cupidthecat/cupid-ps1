/*
 * `GPUFramebufferManager<FBOType, FactoryFn, DestroyFn>` template is collapsed
 * to a concrete `opengl_fbo_cache_t` keyed by (RTs, DS, num_rts, flags), value
 * = `GLuint`.  The concrete cache implementation lives in opengl_device.c.
 * This header just defines the key + a lightweight hash so that gpu_hw.c and
 * opengl_device.c can share the same key shape without dragging in a
 * templated container.
 */

#ifndef CUPID_UTIL_GPU_FRAMEBUFFER_MANAGER_H
#define CUPID_UTIL_GPU_FRAMEBUFFER_MANAGER_H

#include "common/types.h"
#include "util/gpu_device.h"
#include "util/gpu_texture.h"

#include <string.h>

typedef struct {
  gpu_texture_t* rts[GPU_DEVICE_MAX_RENDER_TARGETS];
  gpu_texture_t* ds;
  u32            num_rts;
  u32            flags;
} gpu_framebuffer_key_t;

ALWAYS_INLINE bool gpu_framebuffer_key_eq(const gpu_framebuffer_key_t* a, const gpu_framebuffer_key_t* b)
{
  return memcmp(a, b, sizeof(*a)) == 0;
}

ALWAYS_INLINE bool gpu_framebuffer_key_contains_rt(const gpu_framebuffer_key_t* k, const gpu_texture_t* tex)
{
  for (u32 i = 0; i < k->num_rts; ++i)
    if (k->rts[i] == tex) return true;
  return false;
}

ALWAYS_INLINE u64 gpu_framebuffer_key_hash(const gpu_framebuffer_key_t* k)
{
  const u8* b = (const u8*)k;
  u64 h = 1469598103934665603ULL;
  for (u32 i = 0; i < sizeof(*k); ++i)
    h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}

#endif /* CUPID_UTIL_GPU_FRAMEBUFFER_MANAGER_H */
