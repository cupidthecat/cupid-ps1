/*
 * OpenGLTexture/OpenGLSampler/OpenGLDownloadTexture/OpenGLTextureBuffer to
 * flat structs that embed the gpu_*_t base at offset 0.  This header declares
 * the struct shapes and the public function-style API; the implementation
 * lives in `opengl_texture.c`.
 */

#ifndef CUPID_UTIL_OPENGL_TEXTURE_H
#define CUPID_UTIL_OPENGL_TEXTURE_H

#include "common/types.h"
#include "util/gpu_device.h"
#include "util/gpu_texture.h"

#include "glad/gl.h"

typedef struct Error Error;
typedef struct opengl_stream_buffer opengl_stream_buffer_t;

/* Forward typedef may also be declared in opengl_device.h; guard with macro
 * so both translation units can include either header alone. */
#ifndef CUPID_OPENGL_TEXTURE_T_DECLARED
#define CUPID_OPENGL_TEXTURE_T_DECLARED
typedef struct opengl_texture opengl_texture_t;
#endif

struct opengl_texture {
  gpu_texture_t base;
  GLuint id;

  /* state used by Map/Unmap */
  u32 map_offset;
  u16 map_x;
  u16 map_y;
  u16 map_width;
  u16 map_height;
  u8  map_layer;
  u8  map_level;
};

opengl_texture_t* opengl_texture_create(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                        gpu_texture_type_t type, gpu_texture_format_t format,
                                        gpu_texture_flags_t flags, const void* data, u32 data_pitch,
                                        Error* err);
void              opengl_texture_destroy(opengl_texture_t* tex);

bool opengl_texture_update      (opengl_texture_t* tex, u32 x, u32 y, u32 width, u32 height,
                                  const void* data, u32 pitch, u32 layer, u32 level);
bool opengl_texture_map         (opengl_texture_t* tex, void** map, u32* map_stride,
                                  u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level);
void opengl_texture_unmap       (opengl_texture_t* tex);
void opengl_texture_generate_mipmaps(opengl_texture_t* tex);
void opengl_texture_commit_clear(opengl_texture_t* tex);

bool opengl_texture_use_texture_storage     (bool multisampled);
bool opengl_texture_use_texture_storage_inst(const opengl_texture_t* tex);

/* Format mapping helper.  Returns three GLenums (internal, format, type) packed
 * into a struct so the caller doesn't deal with std::tuple. */
typedef struct { GLenum internal; GLenum format; GLenum type; } opengl_pixel_format_mapping_t;
opengl_pixel_format_mapping_t opengl_texture_get_pixel_format_mapping(gpu_texture_format_t fmt, bool gles);

ALWAYS_INLINE GLuint opengl_texture_get_id(const opengl_texture_t* tex) { return tex->id; }
ALWAYS_INLINE GLenum opengl_texture_get_target(const opengl_texture_t* tex)
{
  return gpu_texture_is_multisampled(&tex->base) ? GL_TEXTURE_2D_MULTISAMPLE
       : (gpu_texture_is_array(&tex->base)       ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
}

typedef struct opengl_sampler {
  gpu_sampler_t base;
  GLuint        id;
} opengl_sampler_t;

opengl_sampler_t* opengl_sampler_create(gpu_sampler_config_t cfg, Error* err);
void              opengl_sampler_destroy(opengl_sampler_t* s);
ALWAYS_INLINE GLuint opengl_sampler_get_id(const opengl_sampler_t* s) { return s->id; }

typedef struct opengl_texture_buffer {
  gpu_texture_buffer_t    base;
  opengl_stream_buffer_t* buffer;
  GLuint                  texture_id;
} opengl_texture_buffer_t;

opengl_texture_buffer_t* opengl_texture_buffer_create(gpu_texture_buffer_format_t fmt, u32 size_in_elements,
                                                     Error* err);
void                     opengl_texture_buffer_destroy(opengl_texture_buffer_t* tb);
void*                    opengl_texture_buffer_map  (opengl_texture_buffer_t* tb, u32 required_elements);
void                     opengl_texture_buffer_unmap(opengl_texture_buffer_t* tb, u32 used_elements);

typedef struct opengl_download_texture {
  gpu_download_texture_t base;
  GLuint                 buffer_id;
  GLsync                 sync;
  /* used when buffer storage is not available */
  u8*                    cpu_buffer;
} opengl_download_texture_t;

opengl_download_texture_t* opengl_download_texture_create(u32 width, u32 height, gpu_texture_format_t fmt,
                                                          void* memory, size_t memory_size,
                                                          u32 memory_pitch, Error* err);
void                       opengl_download_texture_destroy(opengl_download_texture_t* d);
void                       opengl_download_texture_copy_from_texture(
                               opengl_download_texture_t* d, u32 dst_x, u32 dst_y,
                              opengl_texture_t* src, u32 src_x, u32 src_y,
                              u32 width, u32 height, u32 src_layer, u32 src_level, 
                              bool use_transfer_pitch);
bool                       opengl_download_texture_map  (opengl_download_texture_t* d,
                                                          u32 x, u32 y, u32 width, u32 height);
void                       opengl_download_texture_unmap(opengl_download_texture_t* d);
void                       opengl_download_texture_flush(opengl_download_texture_t* d);

#endif /* CUPID_UTIL_OPENGL_TEXTURE_H */
