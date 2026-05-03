/*
 * GPUDownloadTexture are flat structs.  GL-specific fields (handle, target,
 * etc.) appended in opengl_texture.h via struct embedding (`opengl_texture_t`
 * starts with `gpu_texture_t base`).  Static helpers (format names, pixel
 * size, mip count) live in this TU; per-instance update/map ops are in the
 * backend (`opengl_texture_*`).
 */

#ifndef CUPID_UTIL_GPU_TEXTURE_H
#define CUPID_UTIL_GPU_TEXTURE_H

#include "common/types.h"
#include "util/gpu_types.h"
#include "util/image.h"

typedef struct Error Error;

enum {
  GPU_TEXTURE_MAX_WIDTH   = 65535,
  GPU_TEXTURE_MAX_HEIGHT  = 65535,
  GPU_TEXTURE_MAX_LAYERS  = 255,
  GPU_TEXTURE_MAX_LEVELS  = 255,
  GPU_TEXTURE_MAX_SAMPLES = 255,
  GPU_TEXTURE_COMPRESSED_BLOCK_SIZE = 4,
};

typedef enum {
  GPU_TEXTURE_TYPE_TEXTURE = 0,
  GPU_TEXTURE_TYPE_RENDER_TARGET,
  GPU_TEXTURE_TYPE_DEPTH_STENCIL,
} gpu_texture_type_t;

typedef enum {
  GPU_TEXTURE_STATE_DIRTY = 0,
  GPU_TEXTURE_STATE_CLEARED,
  GPU_TEXTURE_STATE_INVALIDATED,
} gpu_texture_state_t;

typedef u8 gpu_texture_flags_t;
#define GPU_TEXTURE_FLAG_NONE                       (0u)
#define GPU_TEXTURE_FLAG_ALLOW_MAP                  (1u << 0)
#define GPU_TEXTURE_FLAG_ALLOW_BIND_AS_IMAGE        (1u << 2)
#define GPU_TEXTURE_FLAG_ALLOW_GENERATE_MIPMAPS     (1u << 3)
#define GPU_TEXTURE_FLAG_ALLOW_MSAA_RESOLVE_TARGET  (1u << 4)

typedef union {
  u32   color;
  float depth;
} gpu_texture_clear_value_t;

typedef struct gpu_texture {
  u16                       width;
  u16                       height;     /* must immediately follow `width` for SizeVec compat */
  u8                        layers;
  u8                        levels;
  u8                        samples;
  gpu_texture_type_t        type;
  gpu_texture_format_t      format;
  gpu_texture_flags_t       flags;
  gpu_texture_state_t       state;
  gpu_texture_clear_value_t clear_value;
} gpu_texture_t;

const char*          gpu_texture_format_name(gpu_texture_format_t f);
u32                  gpu_texture_format_pixel_size(gpu_texture_format_t f);
bool                 gpu_texture_format_is_depth(gpu_texture_format_t f);
bool                 gpu_texture_format_is_depth_stencil(gpu_texture_format_t f);
bool                 gpu_texture_format_is_compressed(gpu_texture_format_t f);
u32                  gpu_texture_format_block_size(gpu_texture_format_t f);
u32                  gpu_texture_calc_upload_pitch(gpu_texture_format_t f, u32 width);
u32                  gpu_texture_calc_upload_row_length_from_pitch(gpu_texture_format_t f, u32 pitch);
u32                  gpu_texture_calc_upload_size(gpu_texture_format_t f, u32 height, u32 pitch);
u32                  gpu_texture_full_mipmap_count(u32 width, u32 height);
void                 gpu_texture_copy_data_for_upload(u32 width, u32 height, gpu_texture_format_t f,
                                                      void* dst, u32 dst_pitch,
                                                      const void* src, u32 src_pitch);
gpu_texture_format_t gpu_texture_format_for_image_format(image_format_t img);
image_format_t       image_format_for_gpu_texture_format(gpu_texture_format_t f);

/* ValidateConfig: caller passes the device's max texture/sample limits, since
 * gpu_device.h hasn't been ported yet.  Pass 0 to skip a limit. */
bool gpu_texture_validate_config(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                  gpu_texture_type_t type, gpu_texture_format_t format,
                                 gpu_texture_flags_t flags, u32 max_texture_size, u32 max_samples, 
                                 Error* err);

ALWAYS_INLINE u32   gpu_texture_get_width  (const gpu_texture_t* t)  { return t->width; }
ALWAYS_INLINE u32   gpu_texture_get_height (const gpu_texture_t* t)  { return t->height; }
ALWAYS_INLINE u32   gpu_texture_get_layers (const gpu_texture_t* t)  { return t->layers; }
ALWAYS_INLINE u32   gpu_texture_get_levels (const gpu_texture_t* t)  { return t->levels; }
ALWAYS_INLINE u32   gpu_texture_get_samples(const gpu_texture_t* t)  { return t->samples; }
ALWAYS_INLINE gpu_texture_type_t   gpu_texture_get_type  (const gpu_texture_t* t) { return t->type; }
ALWAYS_INLINE gpu_texture_format_t gpu_texture_get_format(const gpu_texture_t* t) { return t->format; }
ALWAYS_INLINE gpu_texture_flags_t  gpu_texture_get_flags (const gpu_texture_t* t) { return t->flags; }
ALWAYS_INLINE bool  gpu_texture_has_flag(const gpu_texture_t* t, gpu_texture_flags_t flag) { return (t->flags & flag) != 0; }
ALWAYS_INLINE bool  gpu_texture_is_array(const gpu_texture_t* t) { return t->layers > 1; }
ALWAYS_INLINE bool  gpu_texture_is_multisampled(const gpu_texture_t* t) { return t->samples > 1; }

ALWAYS_INLINE u32   gpu_texture_get_pixel_size(const gpu_texture_t* t)  { return gpu_texture_format_pixel_size(t->format); }
ALWAYS_INLINE u32   gpu_texture_get_mip_width (const gpu_texture_t* t, u32 lvl)
{ u32 w = (u32)t->width  >> lvl; return w ? w : 1u; }
ALWAYS_INLINE u32   gpu_texture_get_mip_height(const gpu_texture_t* t, u32 lvl)
{ u32 h = (u32)t->height >> lvl; return h ? h : 1u; }

ALWAYS_INLINE gpu_texture_state_t gpu_texture_get_state(const gpu_texture_t* t)            { return t->state; }
ALWAYS_INLINE void                gpu_texture_set_state(gpu_texture_t* t, gpu_texture_state_t s) { t->state = s; }
ALWAYS_INLINE bool                gpu_texture_is_dirty(const gpu_texture_t* t)             { return t->state == GPU_TEXTURE_STATE_DIRTY; }
ALWAYS_INLINE bool                gpu_texture_is_cleared_or_invalidated(const gpu_texture_t* t) { return t->state != GPU_TEXTURE_STATE_DIRTY; }

ALWAYS_INLINE bool gpu_texture_is_texture       (const gpu_texture_t* t) { return t->type == GPU_TEXTURE_TYPE_TEXTURE; }
ALWAYS_INLINE bool gpu_texture_is_render_target (const gpu_texture_t* t) { return t->type == GPU_TEXTURE_TYPE_RENDER_TARGET; }
ALWAYS_INLINE bool gpu_texture_is_depth_stencil (const gpu_texture_t* t) { return t->type == GPU_TEXTURE_TYPE_DEPTH_STENCIL; }
ALWAYS_INLINE bool gpu_texture_is_render_target_or_ds(const gpu_texture_t* t)
{ return t->type >= GPU_TEXTURE_TYPE_RENDER_TARGET && t->type <= GPU_TEXTURE_TYPE_DEPTH_STENCIL; }

ALWAYS_INLINE u32   gpu_texture_get_clear_color(const gpu_texture_t* t)             { return t->clear_value.color; }
ALWAYS_INLINE float gpu_texture_get_clear_depth(const gpu_texture_t* t)             { return t->clear_value.depth; }
ALWAYS_INLINE void  gpu_texture_set_clear_color(gpu_texture_t* t, u32 c)            { t->state = GPU_TEXTURE_STATE_CLEARED; t->clear_value.color = c; }
ALWAYS_INLINE void  gpu_texture_set_clear_depth(gpu_texture_t* t, float d)          { t->state = GPU_TEXTURE_STATE_CLEARED; t->clear_value.depth = d; }

void  gpu_texture_get_unorm_clear_color(const gpu_texture_t* t, float out_rgba[4]);
size_t gpu_texture_get_vram_usage(const gpu_texture_t* t);
ALWAYS_INLINE u32 gpu_texture_calc_upload_pitch_inst(const gpu_texture_t* t, u32 width)
{ return gpu_texture_calc_upload_pitch(t->format, width); }
ALWAYS_INLINE u32 gpu_texture_calc_upload_size_inst(const gpu_texture_t* t, u32 height, u32 pitch)
{ return gpu_texture_calc_upload_size(t->format, height, pitch); }

/* Initialize the base fields; backend allocates the wrapping struct and calls
 * this from its create path.  Does NOT touch s_total_vram_usage (that lives on
 * gpu_device). */
void gpu_texture_base_init(gpu_texture_t* t, u16 width, u16 height, u8 layers, u8 levels, u8 samples,
                           gpu_texture_type_t type, gpu_texture_format_t format, gpu_texture_flags_t flags);

typedef struct gpu_download_texture {
  u32                  width;
  u32                  height;
  gpu_texture_format_t format;
  const u8*            map_pointer;
  u32                  current_pitch;
  bool                 is_imported;
  bool                 needs_flush;
} gpu_download_texture_t;

void  gpu_download_texture_base_init(gpu_download_texture_t* d, u32 width, u32 height,
                                     gpu_texture_format_t fmt, bool is_imported);
u32   gpu_download_texture_get_buffer_size(u32 width, u32 height, gpu_texture_format_t fmt, u32 pitch_align);
u32   gpu_download_texture_get_transfer_pitch(const gpu_download_texture_t* d, u32 width, u32 pitch_align);
void  gpu_download_texture_get_transfer_size(const gpu_download_texture_t* d,
                                             u32 x, u32 y, u32 width, u32 height, u32 pitch,
                                             u32* copy_offset, u32* copy_size, u32* copy_rows);

#endif /* CUPID_UTIL_GPU_TEXTURE_H */
