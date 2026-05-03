/*
 * Concrete software GPU back-end.  Embeds gpu_backend_t at offset 0 and
 * points its vtable at static functions in gpu_sw.c.  The display path
 * (CopyOut*) writes RGBA8 pixels into a host-allocated framebuffer the
 * frontend can blit to the screen; the GPU device / texture machinery is
 * dropped (no GL/Vulkan/etc).
 */

#ifndef CUPID_CORE_GPU_SW_H
#define CUPID_CORE_GPU_SW_H

#include "core/gpu_backend.h"
#include "common/types.h"

/* Factory: returns a heap-allocated gpu_sw_t* (cast to gpu_backend_t*).
 * Pass the result to gpu_backend_destroy to free; sets g_gpu_backend.  */
gpu_backend_t* gpu_sw_create(void);

/* Public framebuffer accessors (used by the SDL frontend).  After an
 * update_display call, these expose the most recently scanned-out RGBA8
 * pixel buffer. */
const u32* gpu_sw_get_display_buffer(u32* out_width, u32* out_height);

#endif /* CUPID_CORE_GPU_SW_H */
