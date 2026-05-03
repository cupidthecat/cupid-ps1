/*
 * OpenGL texture / sampler / texture-buffer / download-texture group.  Sister
 * to opengl_texture.h's struct/API declarations and opengl_device.c's weak
 * stub for opengl_texture_commit_clear (which this TU's strong def overrides).
 */

#include "util/opengl_texture.h"
#include "util/opengl_device.h"
#include "util/opengl_stream_buffer.h"

#include "util/gpu_device.h"
#include "util/gpu_texture.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/intrin.h"
#include "common/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUDevice);

/* Looking across a range of GPUs, the optimal copy alignment for Vulkan drivers
 * seems to be between 1 (AMD/NV) and 64 (Intel). So, we'll go with 64 here. */
enum {
  TEXTURE_UPLOAD_ALIGNMENT       = 64,
  TEXTURE_UPLOAD_PITCH_ALIGNMENT = 64,
  DEFAULT_UPLOAD_ALIGNMENT       = 4,
};

opengl_pixel_format_mapping_t opengl_texture_get_pixel_format_mapping(gpu_texture_format_t fmt, bool gles)
{
  static const opengl_pixel_format_mapping_t mapping[GPU_TEXTURE_FORMAT_MAX_COUNT] = {
    {0, 0, 0},                                                                                 /* Unknown */
    {GL_RGBA8,                          GL_RGBA,                            GL_UNSIGNED_BYTE},  /* RGBA8 */
    {GL_RGBA8,                          GL_BGRA,                            GL_UNSIGNED_BYTE},  /* BGRA8 */
    {GL_RGB565,                         GL_RGB,                             GL_UNSIGNED_SHORT_5_6_5}, /* RGB565 */
    {0, 0, 0},                                                                                 /* RGB5A1 */
    {GL_RGB5_A1,                        GL_RGBA,                            GL_UNSIGNED_SHORT_5_5_5_1}, /* A1BGR5 */
    {GL_R8,                             GL_RED,                             GL_UNSIGNED_BYTE},  /* R8 */
    {GL_DEPTH_COMPONENT16,              GL_DEPTH_COMPONENT,                 GL_SHORT},          /* D16 */
    {GL_DEPTH24_STENCIL8,               GL_DEPTH_STENCIL,                   GL_UNSIGNED_INT},   /* D24S8 */
    {GL_DEPTH_COMPONENT32F,             GL_DEPTH_COMPONENT,                 GL_FLOAT},          /* D32F */
    {GL_DEPTH32F_STENCIL8,              GL_DEPTH_STENCIL,                   GL_FLOAT},          /* D32FS8 */
    {GL_R16,                            GL_RED,                             GL_UNSIGNED_SHORT}, /* R16 */
    {GL_R16I,                           GL_RED_INTEGER,                     GL_SHORT},          /* R16I */
    {GL_R16UI,                          GL_RED_INTEGER,                     GL_UNSIGNED_SHORT}, /* R16U */
    {GL_R16F,                           GL_RED,                             GL_HALF_FLOAT},     /* R16F */
    {GL_R32I,                           GL_RED_INTEGER,                     GL_INT},            /* R32I */
    {GL_R32UI,                          GL_RED_INTEGER,                     GL_UNSIGNED_INT},   /* R32U */
    {GL_R32F,                           GL_RED,                             GL_FLOAT},          /* R32F */
    {GL_RG8,                            GL_RG_INTEGER,                      GL_UNSIGNED_BYTE},  /* RG8 */
    {GL_RG16F,                          GL_RG,                              GL_UNSIGNED_SHORT}, /* RG16 */
    {GL_RG16F,                          GL_RG,                              GL_HALF_FLOAT},     /* RG16F */
    {GL_RG32F,                          GL_RG,                              GL_FLOAT},          /* RG32F */
    {GL_RGBA16,                         GL_RGBA,                            GL_UNSIGNED_BYTE},  /* RGBA16 */
    {GL_RGBA16F,                        GL_RGBA,                            GL_HALF_FLOAT},     /* RGBA16F */
    {GL_RGBA32F,                        GL_RGBA,                            GL_FLOAT},          /* RGBA32F */
    {GL_RGB10_A2,                       GL_BGRA,                            GL_UNSIGNED_INT_2_10_10_10_REV}, /* RGB10A2 */
    {GL_SRGB8_ALPHA8,                   GL_RGBA,                            GL_UNSIGNED_BYTE},  /* SRGBA8 */
    {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,  GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,   GL_UNSIGNED_BYTE},  /* BC1 */
    {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,  GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,   GL_UNSIGNED_BYTE},  /* BC2 */
    {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,  GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,   GL_UNSIGNED_BYTE},  /* BC3 */
    {GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB,  GL_UNSIGNED_BYTE},  /* BC7 */
  };

  /* GLES doesn't have the non-normalized 16-bit formats. use float and hope for the best. */
  static const opengl_pixel_format_mapping_t mapping_gles[GPU_TEXTURE_FORMAT_MAX_COUNT] = {
    {0, 0, 0},                                                                                 /* Unknown */
    {GL_RGBA8,                          GL_RGBA,                            GL_UNSIGNED_BYTE},  /* RGBA8 */
    {GL_RGBA8,                          GL_BGRA,                            GL_UNSIGNED_BYTE},  /* BGRA8 */
    {GL_RGB565,                         GL_RGB,                             GL_UNSIGNED_SHORT_5_6_5}, /* RGB565 */
    {0, 0, 0},                                                                                 /* RGB5A1 */
    {GL_RGB5_A1,                        GL_RGBA,                            GL_UNSIGNED_SHORT_5_5_5_1}, /* A1BGR5 */
    {GL_R8,                             GL_RED,                             GL_UNSIGNED_BYTE},  /* R8 */
    {GL_DEPTH_COMPONENT16,              GL_DEPTH_COMPONENT,                 GL_SHORT},          /* D16 */
    {GL_DEPTH24_STENCIL8,               GL_DEPTH_STENCIL,                   GL_UNSIGNED_INT},   /* D24S8 */
    {GL_DEPTH_COMPONENT32F,             GL_DEPTH_COMPONENT,                 GL_FLOAT},          /* D32F */
    {GL_DEPTH32F_STENCIL8,              GL_DEPTH_STENCIL,                   GL_FLOAT},          /* D32FS8 */
    {GL_R16F,                           GL_RED,                             GL_HALF_FLOAT},     /* R16 */
    {GL_R16I,                           GL_RED_INTEGER,                     GL_SHORT},          /* R16I */
    {GL_R16UI,                          GL_RED_INTEGER,                     GL_UNSIGNED_SHORT}, /* R16U */
    {GL_R16F,                           GL_RED,                             GL_HALF_FLOAT},     /* R16F */
    {GL_R32I,                           GL_RED_INTEGER,                     GL_INT},            /* R32I */
    {GL_R32UI,                          GL_RED_INTEGER,                     GL_UNSIGNED_INT},   /* R32U */
    {GL_R32F,                           GL_RED,                             GL_FLOAT},          /* R32F */
    {GL_RG8,                            GL_RG,                              GL_UNSIGNED_BYTE},  /* RG8 */
    {GL_RG16F,                          GL_RG,                              GL_HALF_FLOAT},     /* RG16 */
    {GL_RG16F,                          GL_RG,                              GL_HALF_FLOAT},     /* RG16F */
    {GL_RG32F,                          GL_RG,                              GL_FLOAT},          /* RG32F */
    {GL_RGBA16F,                        GL_RGBA,                            GL_HALF_FLOAT},     /* RGBA16 */
    {GL_RGBA16F,                        GL_RGBA,                            GL_HALF_FLOAT},     /* RGBA16F */
    {GL_RGBA32F,                        GL_RGBA,                            GL_FLOAT},          /* RGBA32F */
    {GL_RGB10_A2,                       GL_BGRA,                            GL_UNSIGNED_INT_2_10_10_10_REV}, /* RGB10A2 */
    {GL_SRGB8_ALPHA8,                   GL_RGBA,                            GL_UNSIGNED_BYTE},  /* SRGBA8 */
    {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,  GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,   GL_UNSIGNED_BYTE},  /* BC1 */
    {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,  GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,   GL_UNSIGNED_BYTE},  /* BC2 */
    {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,  GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,   GL_UNSIGNED_BYTE},  /* BC3 */
    {GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB,  GL_UNSIGNED_BYTE},  /* BC7 */
  };

  const u32 idx = (u32)fmt;
  if (idx >= GPU_TEXTURE_FORMAT_MAX_COUNT)
  {
    opengl_pixel_format_mapping_t empty = {0, 0, 0};
    return empty;
  }
  return gles ? mapping_gles[idx] : mapping[idx];
}

ALWAYS_INLINE static u32 get_upload_alignment(u32 pitch)
{
  return ((pitch % 4) == 0) ? 4u : (((pitch % 2) == 0) ? 2u : 1u);
}

bool opengl_texture_use_texture_storage(bool multisampled)
{
  return (GLAD_GL_VERSION_4_2 || GLAD_GL_ARB_texture_storage) ||
         (multisampled ? GLAD_GL_ES_VERSION_3_1 : GLAD_GL_ES_VERSION_3_0);
}

bool opengl_texture_use_texture_storage_inst(const opengl_texture_t* tex)
{
  return opengl_texture_use_texture_storage(gpu_texture_is_multisampled(&tex->base));
}

 opengl_texture_t* opengl_texture_create(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                        gpu_texture_type_t type, gpu_texture_format_t format,
                                        gpu_texture_flags_t flags, const void* data, u32 data_pitch,
                                        Error* error) 
{
  if (!gpu_texture_validate_config(width, height, layers, levels, samples, type, format, flags,
                                   opengl_device_get_max_texture_size(),
                                   opengl_device_get_max_multisamples(), error))
  {
    return NULL;
  }

  if (layers > 1 && data)
  {
    Error_set_string(error, "Loading texture array data not currently supported");
    return NULL;
  }

  const GLenum target = (samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE
                       : ((layers > 1) ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
  const opengl_pixel_format_mapping_t fm =
      opengl_texture_get_pixel_format_mapping(format, opengl_device_is_gles());
  const GLenum gl_internal_format = fm.internal;
  const GLenum gl_format          = fm.format;
  const GLenum gl_type            = fm.type;

  opengl_device_bind_update_texture_unit();

  glGetError();

  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(target, id);

  if (samples > 1)
  {
    Assert(!data);
    if (opengl_texture_use_texture_storage(true))
    {
      glTexStorage2DMultisample(target, samples, gl_internal_format, width, height, GL_FALSE);
    }
    else
    {
      glTexImage2DMultisample(target, samples, gl_internal_format, width, height, GL_FALSE);
    }

    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels);
  }
  else
  {
    const bool use_tex_storage = opengl_texture_use_texture_storage(false);
    const bool is_compressed   = gpu_texture_format_is_compressed(format);
    if (use_tex_storage)
    {
      if (layers > 1)
        glTexStorage3D(target, levels, gl_internal_format, width, height, layers);
      else
        glTexStorage2D(target, levels, gl_internal_format, width, height);
    }

    if (!use_tex_storage || data)
    {
      const u32 pixel_size = gpu_texture_format_pixel_size(format);
      const u32 alignment  = get_upload_alignment(data_pitch);
      if (data)
      {
        gpu_device_get_statistics()->buffer_streamed +=
            gpu_texture_calc_upload_size(format, height, data_pitch);
        gpu_device_get_statistics()->num_uploads++;

        glPixelStorei(GL_UNPACK_ROW_LENGTH,
                      (GLint)gpu_texture_calc_upload_row_length_from_pitch(format, data_pitch));
        if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
          glPixelStorei(GL_UNPACK_ALIGNMENT, (GLint)alignment);
      }

      const u8* data_ptr = (const u8*)data;
      u32 current_width  = width;
      u32 current_height = height;
      for (u32 i = 0; i < levels; i++)
      {
        if (use_tex_storage)
        {
          if (is_compressed)
          {
            const u32 size = gpu_texture_calc_upload_size(format, current_height, data_pitch);
            if (layers > 1)
            {
              glCompressedTexSubImage3D(target, (GLint)i, 0, 0, 0, current_width, current_height,
                                        layers, gl_format, (GLsizei)size, data_ptr);
            }
            else
            {
              glCompressedTexSubImage2D(target, (GLint)i, 0, 0, current_width, current_height,
                                        gl_format, (GLsizei)size, data_ptr);
            }
          }
          else
          {
            if (layers > 1)
              glTexSubImage3D(target, (GLint)i, 0, 0, 0, current_width, current_height, layers,
                              gl_format, gl_type, data_ptr);
            else
              glTexSubImage2D(target, (GLint)i, 0, 0, current_width, current_height,
                              gl_format, gl_type, data_ptr);
          }
        }
        else
        {
          if (is_compressed)
          {
            const u32 size = gpu_texture_calc_upload_size(format, current_height, data_pitch);
            if (layers > 1)
            {
              glCompressedTexImage3D(target, (GLint)i, gl_internal_format, current_width,
                                     current_height, layers, 0, (GLsizei)size, data_ptr);
            }
            else
            {
              glCompressedTexImage2D(target, (GLint)i, gl_internal_format, current_width,
                                     current_height, 0, (GLsizei)size, data_ptr);
            }
          }
          else
          {
            if (layers > 1)
            {
              glTexImage3D(target, (GLint)i, gl_internal_format, current_width, current_height,
                           layers, 0, gl_format, gl_type, data_ptr);
            }
            else
            {
              glTexImage2D(target, (GLint)i, gl_internal_format, current_width, current_height, 0,
                           gl_format, gl_type, data_ptr);
            }
          }
        }

        if (data_ptr)
          data_ptr += data_pitch * current_width;

        current_width  = (current_width  > 1) ? (current_width  / 2u) : current_width;
        current_height = (current_height > 1) ? (current_height / 2u) : current_height;

        /* TODO: Incorrect assumption. */
        data_pitch = pixel_size * current_width;
      }

      if (data)
      {
        if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
          glPixelStorei(GL_UNPACK_ALIGNMENT, DEFAULT_UPLOAD_ALIGNMENT);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      }
    }

    if (!use_tex_storage)
    {
      glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)(levels - 1));
    }
  }

  const GLenum gl_error = glGetError();
  if (gl_error != GL_NO_ERROR)
  {
    Error_set_string_fmt(error, "Failed to create texture: 0x%X", (unsigned)gl_error);
    glDeleteTextures(1, &id);
    return NULL;
  }

  opengl_texture_t* tex = (opengl_texture_t*)calloc(1, sizeof(*tex));
  if (!tex)
  {
    Error_set_string(error, "Out of memory");
    glDeleteTextures(1, &id);
    return NULL;
  }
  gpu_texture_base_init(&tex->base, (u16)width, (u16)height, (u8)layers, (u8)levels, (u8)samples,
                        type, format, flags);
  tex->id = id;

  gpu_device_track_texture_alloc(gpu_texture_get_vram_usage(&tex->base));
  return tex;
}

void opengl_texture_destroy(opengl_texture_t* tex)
{
  if (!tex) return;
  if (tex->id != 0)
  {
    opengl_device_unbind_texture(tex);
    glDeleteTextures(1, &tex->id);
    tex->id = 0;
  }
  gpu_device_track_texture_free(gpu_texture_get_vram_usage(&tex->base));
  free(tex);
}

void opengl_texture_commit_clear(opengl_texture_t* tex)
{
  if (!tex) return;

  switch (gpu_texture_get_state(&tex->base))
  {
    case GPU_TEXTURE_STATE_INVALIDATED:
    {
      gpu_texture_set_state(&tex->base, GPU_TEXTURE_STATE_DIRTY);

      if (glInvalidateTexImage)
      {
        glInvalidateTexImage(tex->id, 0);
      }
      else if (glInvalidateFramebuffer)
      {
        const GLuint write_fbo   = opengl_device_get_write_fbo();
        const GLuint current_fbo = opengl_device_get_current_fbo();

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, write_fbo);

        const GLenum attachment =
            gpu_texture_is_depth_stencil(&tex->base) ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, opengl_texture_get_target(tex),
                               tex->id, 0);

        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
      }
      break;
    }

    case GPU_TEXTURE_STATE_CLEARED:
    {
      gpu_texture_set_state(&tex->base, GPU_TEXTURE_STATE_DIRTY);

      if (glClearTexImage)
      {
        const opengl_pixel_format_mapping_t fm =
            opengl_texture_get_pixel_format_mapping(gpu_texture_get_format(&tex->base),
                                                    opengl_device_is_gles());
        glClearTexImage(tex->id, 0, fm.format, fm.type, &tex->base.clear_value);
      }
      else
      {
        const GLuint write_fbo   = opengl_device_get_write_fbo();
        const GLuint current_fbo = opengl_device_get_current_fbo();

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, write_fbo);

        const GLenum attachment =
            gpu_texture_is_depth_stencil(&tex->base) ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, opengl_texture_get_target(tex),
                               tex->id, 0);

        const gpu_blend_state_t* last_blend = opengl_device_last_blend_state_slot();
        const gpu_depth_state_t* last_depth = opengl_device_last_depth_state_slot();

        if (gpu_texture_is_depth_stencil(&tex->base))
        {
          const float depth = gpu_texture_get_clear_depth(&tex->base);
          glDisable(GL_SCISSOR_TEST);
          if (!last_depth->bits.depth_write)
            glDepthMask(GL_TRUE);
          glClearBufferfv(GL_DEPTH, 0, &depth);
          if (!last_depth->bits.depth_write)
            glDepthMask(GL_FALSE);
          glEnable(GL_SCISSOR_TEST);
        }
        else
        {
          float color[4];
          gpu_texture_get_unorm_clear_color(&tex->base, color);
          glDisable(GL_SCISSOR_TEST);
          const u32 wmask = (last_blend->bits.write_r ? 1u : 0u) | (last_blend->bits.write_g ? 2u : 0u) |
                            (last_blend->bits.write_b ? 4u : 0u) | (last_blend->bits.write_a ? 8u : 0u);
          if (wmask != 0xFu)
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
          glClearBufferfv(GL_COLOR, 0, color);
          if (wmask != 0xFu)
          {
            glColorMask(last_blend->bits.write_r ? GL_TRUE : GL_FALSE,
                         last_blend->bits.write_g ? GL_TRUE : GL_FALSE,
                        last_blend->bits.write_b ? GL_TRUE : GL_FALSE, 
                        last_blend->bits.write_a ? GL_TRUE : GL_FALSE);
          }
          glEnable(GL_SCISSOR_TEST);
        }

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
      }
      break;
    }

    case GPU_TEXTURE_STATE_DIRTY:
      break;

    default:
      UnreachableCode();
      break;
  }
}

bool opengl_texture_update(opengl_texture_t* tex, u32 x, u32 y, u32 width, u32 height,
                           const void* data, u32 pitch, u32 layer, u32 level)
{
  /* Worth using the PBO? Driver probably knows better... */
  const GLenum target = opengl_texture_get_target(tex);
  const opengl_pixel_format_mapping_t fm =
      opengl_texture_get_pixel_format_mapping(gpu_texture_get_format(&tex->base),
                                              opengl_device_is_gles());
  const GLenum gl_format = fm.format;
  const GLenum gl_type   = fm.type;
  const u32 preferred_pitch =
      AlignUpPow2(gpu_texture_calc_upload_pitch(tex->base.format, width),
                  TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 map_size = gpu_texture_calc_upload_size(tex->base.format, height, pitch);
  opengl_stream_buffer_t* sb = opengl_device_get_texture_stream_buffer();

  opengl_texture_commit_clear(tex);

  gpu_device_get_statistics()->buffer_streamed += map_size;
  gpu_device_get_statistics()->num_uploads++;

  opengl_device_bind_update_texture_unit();
  glBindTexture(target, tex->id);

  if (!sb || map_size > opengl_stream_buffer_chunk_size(sb))
  {
    DEBUG_LOG("Not using PBO for map size %u", map_size);

    const u32 alignment = get_upload_alignment(pitch);
    if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
      glPixelStorei(GL_UNPACK_ALIGNMENT, (GLint)alignment);

    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                  (GLint)gpu_texture_calc_upload_row_length_from_pitch(tex->base.format, pitch));
    if (gpu_texture_format_is_compressed(tex->base.format))
    {
      const u32 size = gpu_texture_calc_upload_size(tex->base.format, height, pitch);
      if (gpu_texture_is_array(&tex->base))
        glCompressedTexSubImage3D(target, (GLint)level, (GLint)x, (GLint)y, (GLint)layer,
                                  width, height, 1, gl_format, (GLsizei)size, data);
      else
        glCompressedTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, width, height,
                                  gl_format, (GLsizei)size, data);
    }
    else
    {
      if (gpu_texture_is_array(&tex->base))
        glTexSubImage3D(target, (GLint)level, (GLint)x, (GLint)y, (GLint)layer, width, height, 1,
                        gl_format, gl_type, data);
      else
        glTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, width, height,
                        gl_format, gl_type, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
      glPixelStorei(GL_UNPACK_ALIGNMENT, DEFAULT_UPLOAD_ALIGNMENT);
  }
  else
  {
    const opengl_stream_buffer_mapping_t map =
        opengl_stream_buffer_map(sb, TEXTURE_UPLOAD_ALIGNMENT, map_size);
    gpu_texture_copy_data_for_upload(width, height, tex->base.format, map.pointer, preferred_pitch,
                                     data, pitch);
    opengl_stream_buffer_unmap(sb, map_size);
    opengl_stream_buffer_bind(sb);

    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                  (GLint)gpu_texture_calc_upload_row_length_from_pitch(tex->base.format,
                                                                       preferred_pitch));
    if (gpu_texture_format_is_compressed(tex->base.format))
    {
      const u32 size = gpu_texture_calc_upload_size(tex->base.format, height, pitch);
      if (gpu_texture_is_array(&tex->base))
      {
        glCompressedTexSubImage3D(target, (GLint)level, (GLint)x, (GLint)y, (GLint)layer,
                                  width, height, 1, gl_format, (GLsizei)size,
                                  (const void*)(uintptr_t)map.buffer_offset);
      }
      else
      {
        glCompressedTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, width, height,
                                  gl_format, (GLsizei)size,
                                  (const void*)(uintptr_t)map.buffer_offset);
      }
    }
    else
    {
      if (gpu_texture_is_array(&tex->base))
      {
        glTexSubImage3D(target, (GLint)level, (GLint)x, (GLint)y, (GLint)layer, width, height, 1,
                        gl_format, gl_type, (const void*)(uintptr_t)map.buffer_offset);
      }
      else
      {
        glTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, width, height,
                        gl_format, gl_type, (const void*)(uintptr_t)map.buffer_offset);
      }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    opengl_stream_buffer_unbind(sb);
  }

  glBindTexture(target, 0);
  return true;
}

bool opengl_texture_map(opengl_texture_t* tex, void** map, u32* map_stride, u32 x, u32 y,
                        u32 width, u32 height, u32 layer, u32 level)
{
  if ((x + width) > gpu_texture_get_mip_width(&tex->base, level) ||
      (y + height) > gpu_texture_get_mip_height(&tex->base, level) ||
      layer > tex->base.layers || level > tex->base.levels)
  {
    return false;
  }

  const u32 pitch = AlignUpPow2(gpu_texture_calc_upload_pitch(tex->base.format, width),
                                TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 upload_size = gpu_texture_calc_upload_size(tex->base.format, height, pitch);
  opengl_stream_buffer_t* sb = opengl_device_get_texture_stream_buffer();
  if (!sb || upload_size > opengl_stream_buffer_get_size(sb))
    return false;

  const opengl_stream_buffer_mapping_t res =
      opengl_stream_buffer_map(sb, TEXTURE_UPLOAD_ALIGNMENT, upload_size);
  *map        = res.pointer;
  *map_stride = pitch;

  tex->map_offset = res.buffer_offset;
  tex->map_x      = (u16)x;
  tex->map_y      = (u16)y;
  tex->map_width  = (u16)width;
  tex->map_height = (u16)height;
  tex->map_layer  = (u8)layer;
  tex->map_level  = (u8)level;
  return true;
}

void opengl_texture_unmap(opengl_texture_t* tex)
{
  opengl_texture_commit_clear(tex);

  const u32 pitch = AlignUpPow2(gpu_texture_calc_upload_pitch(tex->base.format, tex->map_width),
                                TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 upload_size = gpu_texture_calc_upload_size(tex->base.format, tex->map_height, pitch);

  gpu_device_get_statistics()->buffer_streamed += upload_size;
  gpu_device_get_statistics()->num_uploads++;

  opengl_stream_buffer_t* sb = opengl_device_get_texture_stream_buffer();
  opengl_stream_buffer_unmap(sb, upload_size);
  opengl_stream_buffer_bind(sb);

  opengl_device_bind_update_texture_unit();

  const GLenum target = opengl_texture_get_target(tex);
  glBindTexture(target, tex->id);

  glPixelStorei(GL_UNPACK_ROW_LENGTH,
                (GLint)gpu_texture_calc_upload_row_length_from_pitch(tex->base.format, pitch));

  const opengl_pixel_format_mapping_t fm =
      opengl_texture_get_pixel_format_mapping(gpu_texture_get_format(&tex->base),
                                              opengl_device_is_gles());
  const GLenum gl_format = fm.format;
  const GLenum gl_type   = fm.type;

  if (gpu_texture_format_is_compressed(tex->base.format))
  {
    const u32 size = gpu_texture_calc_upload_size(tex->base.format, tex->map_height, pitch);
    if (gpu_texture_is_array(&tex->base))
    {
      glCompressedTexSubImage3D(target, tex->map_level, tex->map_x, tex->map_y, tex->map_layer,
                                tex->map_width, tex->map_height, 1, gl_format, (GLsizei)size,
                                (const void*)(uintptr_t)tex->map_offset);
    }
    else
    {
      glCompressedTexSubImage2D(target, tex->map_level, tex->map_x, tex->map_y, tex->map_width,
                                tex->map_height, gl_format, (GLsizei)size,
                                (const void*)(uintptr_t)tex->map_offset);
    }
  }
  else
  {
    if (gpu_texture_is_array(&tex->base))
    {
      glTexSubImage3D(target, tex->map_level, tex->map_x, tex->map_y, tex->map_layer,
                      tex->map_width, tex->map_height, 1, gl_format, gl_type,
                      (const void*)(uintptr_t)tex->map_offset);
    }
    else
    {
      glTexSubImage2D(target, tex->map_level, tex->map_x, tex->map_y, tex->map_width,
                      tex->map_height, gl_format, gl_type,
                      (const void*)(uintptr_t)tex->map_offset);
    }
  }

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  glBindTexture(target, 0);

  opengl_stream_buffer_unbind(sb);
}

void opengl_texture_generate_mipmaps(opengl_texture_t* tex)
{
  DebugAssert(gpu_texture_has_flag(&tex->base, GPU_TEXTURE_FLAG_ALLOW_GENERATE_MIPMAPS));
  opengl_device_bind_update_texture_unit();
  const GLenum target = opengl_texture_get_target(tex);
  glBindTexture(target, tex->id);
  glGenerateMipmap(target);
  glBindTexture(target, 0);
}

opengl_sampler_t* opengl_sampler_create(gpu_sampler_config_t cfg, Error* error)
{
  static const GLenum address_modes[GPU_SAMPLER_ADDRESS_MAX_COUNT] = {
    GL_REPEAT,          /* Repeat */
    GL_CLAMP_TO_EDGE,   /* ClampToEdge */
    GL_CLAMP_TO_BORDER, /* ClampToBorder */
    GL_MIRRORED_REPEAT, /* MirrorRepeat */
  };

  /* [mipmap_on_off][mip_filter][min/mag_filter] */
  static const GLenum filters[2][2][2] = {
    {
      /* mipmap=off */
      {GL_NEAREST, GL_LINEAR}, /* mipmap=nearest */
      {GL_NEAREST, GL_LINEAR}, /* mipmap=linear */
    },
    {
      /* mipmap=on */
      {GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST}, /* mipmap=nearest */
      {GL_NEAREST_MIPMAP_LINEAR,  GL_LINEAR_MIPMAP_LINEAR},  /* mipmap=linear */
    },
  };

  GLuint sampler = 0;
  glGetError();
  glGenSamplers(1, &sampler);
  if (glGetError() != GL_NO_ERROR)
  {
    Error_set_string_fmt(error, "Failed to create sampler: %X", (unsigned)sampler);
    return NULL;
  }

  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S,
                      (GLint)address_modes[(u8)cfg.bits.address_u]);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T,
                      (GLint)address_modes[(u8)cfg.bits.address_v]);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R,
                      (GLint)address_modes[(u8)cfg.bits.address_w]);

  const u8 mipmap_on_off = (cfg.bits.min_lod != 0 || cfg.bits.max_lod != 0) ? 1u : 0u;
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                      (GLint)filters[mipmap_on_off][(u8)cfg.bits.mip_filter]
                                    [(u8)cfg.bits.min_filter]);
  glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                      (GLint)filters[0][(u8)cfg.bits.mip_filter][(u8)cfg.bits.mag_filter]);
  glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)cfg.bits.min_lod);
  glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, (float)cfg.bits.max_lod);

  float border[4];
  gpu_sampler_config_border_rgba(cfg, border);
  glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);

  if (cfg.bits.anisotropy > 1)
    glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, (float)cfg.bits.anisotropy);

  opengl_sampler_t* s = (opengl_sampler_t*)calloc(1, sizeof(*s));
  if (!s)
  {
    Error_set_string(error, "Out of memory for sampler");
    glDeleteSamplers(1, &sampler);
    return NULL;
  }
  s->id = sampler;
  return s;
}

void opengl_sampler_destroy(opengl_sampler_t* s)
{
  if (!s) return;
  if (s->id != 0)
  {
    opengl_device_unbind_sampler(s->id);
    glDeleteSamplers(1, &s->id);
    s->id = 0;
  }
  free(s);
}

 opengl_texture_buffer_t* opengl_texture_buffer_create(gpu_texture_buffer_format_t fmt,
                                                     u32 size_in_elements, Error* error) 
{
  const bool use_ssbo = opengl_device_get_features()->texture_buffers_emulated_with_ssbo;
  const u32 buffer_size = gpu_texture_buffer_element_size(fmt) * size_in_elements;

  if (use_ssbo)
  {
    GLint64 max_ssbo_size = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
    if ((GLint64)buffer_size > max_ssbo_size)
    {
      Error_set_string_fmt(error, "Buffer size of %u not supported, max is %lld",
                           (unsigned)buffer_size, (long long)max_ssbo_size);
      return NULL;
    }
  }

  const GLenum target = use_ssbo ? GL_SHADER_STORAGE_BUFFER : GL_TEXTURE_BUFFER;
  opengl_stream_buffer_t* sb = opengl_stream_buffer_create(target, buffer_size, error);
  if (!sb)
    return NULL;
  opengl_stream_buffer_unbind(sb);

  GLuint texture_id = 0;
  if (!use_ssbo)
  {
    glGetError();
    glGenTextures(1, &texture_id);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
      Error_set_string_fmt(error, "Failed to create texture for buffer: 0x%X", (unsigned)err);
      opengl_stream_buffer_destroy(sb);
      return NULL;
    }

    opengl_device_bind_update_texture_unit();
    glBindTexture(GL_TEXTURE_BUFFER, texture_id);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, opengl_stream_buffer_get_id(sb));
  }

  opengl_texture_buffer_t* tb = (opengl_texture_buffer_t*)calloc(1, sizeof(*tb));
  if (!tb)
  {
    Error_set_string(error, "Out of memory for texture buffer");
    if (texture_id != 0)
      glDeleteTextures(1, &texture_id);
    opengl_stream_buffer_destroy(sb);
    return NULL;
  }
  gpu_texture_buffer_base_init(&tb->base, fmt, size_in_elements);
  tb->buffer     = sb;
  tb->texture_id = texture_id;
  return tb;
}

void opengl_texture_buffer_destroy(opengl_texture_buffer_t* tb)
{
  if (!tb) return;
  if (tb->texture_id != 0)
  {
    opengl_device_unbind_texture_id(tb->texture_id);
    glDeleteTextures(1, &tb->texture_id);
  }
  else if (tb->buffer && opengl_device_get_features()->texture_buffers_emulated_with_ssbo)
  {
    opengl_device_unbind_ssbo(opengl_stream_buffer_get_id(tb->buffer));
  }
  if (tb->buffer)
    opengl_stream_buffer_destroy(tb->buffer);
  free(tb);
}

void* opengl_texture_buffer_map(opengl_texture_buffer_t* tb, u32 required_elements)
{
  const u32 esize = gpu_texture_buffer_element_size(tb->base.format);
  const opengl_stream_buffer_mapping_t map =
      opengl_stream_buffer_map(tb->buffer, esize, esize * required_elements);
  tb->base.current_position = map.index_aligned;
  return map.pointer;
}

void opengl_texture_buffer_unmap(opengl_texture_buffer_t* tb, u32 used_elements)
{
  const u32 size = used_elements * gpu_texture_buffer_element_size(tb->base.format);
  gpu_device_get_statistics()->buffer_streamed += size;
  gpu_device_get_statistics()->num_uploads++;
  opengl_stream_buffer_unmap(tb->buffer, size);
}

opengl_download_texture_t* opengl_download_texture_create(u32 width, u32 height,
                                                          gpu_texture_format_t fmt,
                                                          void* memory, size_t memory_size,
                                                          u32 memory_pitch, Error* error)
{
  const u32 buffer_pitch =
      memory ? memory_pitch
             : (u32)AlignUpPow2(gpu_texture_calc_upload_pitch(fmt, width),
                                TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 buffer_size = memory ? (u32)memory_size : (height * buffer_pitch);

  const bool use_buffer_storage =
      (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage) &&
      !memory && opengl_device_should_use_pbos_for_downloads();

  if (use_buffer_storage)
  {
    GLuint buffer_id = 0;
    glGenBuffers(1, &buffer_id);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_id);

    const GLbitfield flags     = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield map_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;

    if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
      glBufferStorage(GL_PIXEL_PACK_BUFFER, buffer_size, NULL, flags);
    else if (GLAD_GL_EXT_buffer_storage)
      glBufferStorageEXT(GL_PIXEL_PACK_BUFFER, buffer_size, NULL, flags);

    u8* buffer_map = (u8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, buffer_size, map_flags);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (!buffer_map)
    {
      Error_set_string(error, "Failed to map persistent download buffer");
      glDeleteBuffers(1, &buffer_id);
      return NULL;
    }

    opengl_download_texture_t* dt = (opengl_download_texture_t*)calloc(1, sizeof(*dt));
    if (!dt)
    {
      Error_set_string(error, "Out of memory for download texture");
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      glDeleteBuffers(1, &buffer_id);
      return NULL;
    }
    gpu_download_texture_base_init(&dt->base, width, height, fmt, /*is_imported=*/false);
    dt->base.map_pointer   = buffer_map;
    dt->base.current_pitch = buffer_pitch;
    dt->buffer_id          = buffer_id;
    dt->cpu_buffer         = NULL;
    dt->sync               = 0;
    return dt;
  }

  /* Fallback to glReadPixels() + CPU buffer. */
  const bool imported = (memory != NULL);
  u8* cpu_buffer = imported ? (u8*)memory : (u8*)AlignedMalloc(buffer_size, VECTOR_ALIGNMENT);
  if (!cpu_buffer)
  {
    Error_set_string(error, "Failed to get client-side memory pointer.");
    return NULL;
  }

  opengl_download_texture_t* dt = (opengl_download_texture_t*)calloc(1, sizeof(*dt));
  if (!dt)
  {
    Error_set_string(error, "Out of memory for download texture");
    if (!imported)
      AlignedFree(cpu_buffer);
    return NULL;
  }
  gpu_download_texture_base_init(&dt->base, width, height, fmt, imported);
  dt->base.map_pointer   = cpu_buffer;
  dt->base.current_pitch = buffer_pitch;
  dt->buffer_id          = 0;
  dt->cpu_buffer         = cpu_buffer;
  dt->sync               = 0;
  return dt;
}

void opengl_download_texture_destroy(opengl_download_texture_t* d)
{
  if (!d) return;
  if (d->buffer_id != 0)
  {
    if (d->sync)
      glDeleteSync(d->sync);

    if (d->base.map_pointer)
    {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, d->buffer_id);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    glDeleteBuffers(1, &d->buffer_id);
  }
  else if (d->cpu_buffer && !d->base.is_imported)
  {
    AlignedFree(d->cpu_buffer);
  }
  free(d);
}

void opengl_download_texture_copy_from_texture(opengl_download_texture_t* d, u32 dst_x, u32 dst_y,
                                                opengl_texture_t* src, u32 src_x, u32 src_y,
                                               u32 width, u32 height, u32 src_layer, u32 src_level, 
                                               bool use_transfer_pitch)
{
  (void)src_layer; /* uses 0 layer for the bound 2D path */

  DebugAssert(gpu_texture_get_format(&src->base) == d->base.format);
  DebugAssert(src_level < src->base.levels);
  DebugAssert((src_x + width) <= gpu_texture_get_mip_width(&src->base, src_level) &&
              (src_y + height) <= gpu_texture_get_mip_height(&src->base, src_level));
  DebugAssert((dst_x + width) <= d->base.width && (dst_y + height) <= d->base.height);
  DebugAssert((dst_x == 0 && dst_y == 0) || !use_transfer_pitch);
  DebugAssert(!d->base.is_imported || !use_transfer_pitch);

  opengl_device_commit_clear(src);

  u32 copy_offset = 0, copy_size = 0, copy_rows = 0;
  if (!d->base.is_imported)
  {
    d->base.current_pitch =
        gpu_download_texture_get_transfer_pitch(&d->base, use_transfer_pitch ? width : d->base.width,
                                                TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  }
  gpu_download_texture_get_transfer_size(&d->base, dst_x, dst_y, width, height, d->base.current_pitch,
                                         &copy_offset, &copy_size, &copy_rows);
  gpu_device_get_statistics()->num_downloads++;

  GLint alignment;
  if (d->base.current_pitch & 1u)       alignment = 1;
  else if (d->base.current_pitch & 2u)  alignment = 2;
  else                                  alignment = 4;

  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH,
                (GLint)gpu_texture_calc_upload_row_length_from_pitch(d->base.format,
                                                                     d->base.current_pitch));

  if (!d->cpu_buffer)
  {
    /* Read to PBO. */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, d->buffer_id);
  }

  const opengl_pixel_format_mapping_t fm =
      opengl_texture_get_pixel_format_mapping(gpu_texture_get_format(&src->base),
                                              opengl_device_is_gles());

  if (opengl_device_use_get_texture_sub_image())
  {
    glGetTextureSubImage(src->id, (GLint)src_level, (GLint)src_x, (GLint)src_y, 0,
                          width, height, 1, fm.format, fm.type,
                         (GLsizei)(d->base.current_pitch * height),
                         d->cpu_buffer ? (d->cpu_buffer + copy_offset) 
                                       : (void*)(uintptr_t)copy_offset);
  }
  else
  {
    const GLuint read_fbo = opengl_device_get_read_fbo();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src->id, 0);

    glReadPixels((GLint)src_x, (GLint)src_y, width, height, fm.format, fm.type,
                 d->cpu_buffer ? (d->cpu_buffer + copy_offset) : (void*)(uintptr_t)copy_offset);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }

  if (d->cpu_buffer)
  {
    /* If using CPU buffers, we never need to flush. */
    d->base.needs_flush = false;
  }
  else
  {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    /* Create a sync object so we know when the GPU is done copying. */
    if (d->sync)
      glDeleteSync(d->sync);

    d->sync             = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    d->base.needs_flush = true;
  }

  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

bool opengl_download_texture_map(opengl_download_texture_t* d, u32 x, u32 y, u32 width, u32 height)
{
  (void)d; (void)x; (void)y; (void)width; (void)height;
  /* Either always mapped, or CPU buffer. */
  return true;
}

void opengl_download_texture_unmap(opengl_download_texture_t* d)
{
  (void)d;
  /* Either always mapped, or CPU buffer. */
}

void opengl_download_texture_flush(opengl_download_texture_t* d)
{
  /* If we're using CPU buffers, we did the readback synchronously... */
  if (!d->base.needs_flush || !d->sync)
    return;

  d->base.needs_flush = false;

  glClientWaitSync(d->sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
  glDeleteSync(d->sync);
  d->sync = 0;
}
