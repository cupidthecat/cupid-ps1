/*
 * format/validation/usage helpers + base struct init.  Backend (opengl_texture)
 * extends gpu_texture_t and implements update/map/etc directly.
 *
 * `s_total_vram_usage` tracking deferred to gpu_device.  ValidateConfig
 * takes the device limits as parameters until then.
 */

#include "gpu_texture.h"
#include "image.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"

#include <stdint.h>
#include <string.h>

/* Lightweight stride memcpy local to the translation unit (StringUtil version
 * lands when first non-GPU caller needs it). */
static void stride_memcpy(void* dst, u32 dst_stride, const void* src, u32 src_stride, u32 row_bytes, u32 rows)
{
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  for (u32 i = 0; i < rows; ++i)
  {
    memcpy(d, s, row_bytes);
    d += dst_stride;
    s += src_stride;
  }
}

const char* gpu_texture_format_name(gpu_texture_format_t f)
{
  static const char* const names[GPU_TEXTURE_FORMAT_MAX_COUNT] = {
    "Unknown", "RGBA8", "BGRA8", "RGB565", "RGB5A1", "A1BGR5", "R8",
    "D16", "D24S8", "D32F", "D32FS8S",
    "R16", "R16I", "R16U", "R16F", "R32I", "R32U", "R32F",
    "RG8", "RG16", "RG16F", "RG32F",
    "RGBA16", "RGBA16F", "RGBA32F", "RGB10A2", "SRGBA8",
    "BC1", "BC2", "BC3", "BC7", 
  };
  return ((u32)f < GPU_TEXTURE_FORMAT_MAX_COUNT) ? names[f] : "?";
}

u32 gpu_texture_format_pixel_size(gpu_texture_format_t f)
{
  static const u8 sizes[GPU_TEXTURE_FORMAT_MAX_COUNT] = {
    0,                       /* Unknown */
    4, 4,                    /* RGBA8, BGRA8 */
    2, 2, 2,                 /* RGB565, RGB5A1, A1BGR5 */
    1,                       /* R8 */
    2, 4, 4, 8,              /* D16, D24S8, D32F, D32FS8 */
    2, 2, 2, 2,              /* R16, R16I, R16U, R16F */
    4, 4, 4,                 /* R32I, R32U, R32F */
    2, 2, 2, 8,              /* RG8, RG16, RG16F, RG32F (note: RG8=2) */
    8, 8, 16,                /* RGBA16, RGBA16F, RGBA32F */
    4, 4,                    /* RGB10A2, SRGBA8 */
    8, 16, 16, 16,           /* BC1, BC2, BC3, BC7 */
  };
  return ((u32)f < GPU_TEXTURE_FORMAT_MAX_COUNT) ? sizes[f] : 0u;
}

bool gpu_texture_format_is_depth(gpu_texture_format_t f)
{
  return f >= GPU_TEXTURE_FORMAT_D16 && f <= GPU_TEXTURE_FORMAT_D32FS8;
}

bool gpu_texture_format_is_depth_stencil(gpu_texture_format_t f)
{
  return f == GPU_TEXTURE_FORMAT_D24S8 || f == GPU_TEXTURE_FORMAT_D32FS8;
}

bool gpu_texture_format_is_compressed(gpu_texture_format_t f)
{
  return f >= GPU_TEXTURE_FORMAT_BC1;
}

u32 gpu_texture_format_block_size(gpu_texture_format_t f)
{
  return gpu_texture_format_is_compressed(f) ? GPU_TEXTURE_COMPRESSED_BLOCK_SIZE : 1u;
}

u32 gpu_texture_calc_upload_pitch(gpu_texture_format_t f, u32 width)
{
  if (gpu_texture_format_is_compressed(f))
    width = AlignUpPow2(width, GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE;
  return width * gpu_texture_format_pixel_size(f);
}

u32 gpu_texture_calc_upload_row_length_from_pitch(gpu_texture_format_t f, u32 pitch)
{
  const u32 ps = gpu_texture_format_pixel_size(f);
  if (ps == 0) return 0;
  if (gpu_texture_format_is_compressed(f))
    return (AlignUpPow2(pitch, ps) / ps) * GPU_TEXTURE_COMPRESSED_BLOCK_SIZE;
  return pitch / ps;
}

u32 gpu_texture_calc_upload_size(gpu_texture_format_t f, u32 height, u32 pitch)
{
  const u32 bs = gpu_texture_format_block_size(f);
  return pitch * ((height + (bs - 1)) / bs);
}

u32 gpu_texture_full_mipmap_count(u32 width, u32 height)
{
  const u32 max_dim = (width > height) ? width : height;
  const u32 prev = PreviousPow2_u32(max_dim);
  return (u32)CountTrailingZeros_u32(prev) + 1u;
}

void gpu_texture_copy_data_for_upload(u32 width, u32 height, gpu_texture_format_t f,
                                      void* dst, u32 dst_pitch,
                                      const void* src, u32 src_pitch)
{
  if (gpu_texture_format_is_compressed(f))
  {
    const u32 blocks_wide = AlignUpPow2(width,  GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE;
    const u32 blocks_high = AlignUpPow2(height, GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE;
    const u32 block_size  = gpu_texture_format_pixel_size(f);
    stride_memcpy(dst, dst_pitch, src, src_pitch, block_size * blocks_wide, blocks_high);
  }
  else
  {
    stride_memcpy(dst, dst_pitch, src, src_pitch, width * gpu_texture_format_pixel_size(f), height);
  }
}

gpu_texture_format_t gpu_texture_format_for_image_format(image_format_t img)
{
  static const gpu_texture_format_t mapping[IMAGE_FORMAT_MAX_COUNT] = {
    GPU_TEXTURE_FORMAT_UNKNOWN, /* None  */
    GPU_TEXTURE_FORMAT_RGBA8,   /* RGBA8 */
    GPU_TEXTURE_FORMAT_BGRA8,   /* BGRA8 */
    GPU_TEXTURE_FORMAT_RGB565,  /* RGB565 */
    GPU_TEXTURE_FORMAT_RGB5A1,  /* RGB5A1 */
    GPU_TEXTURE_FORMAT_A1BGR5,  /* A1BGR5 */
    GPU_TEXTURE_FORMAT_UNKNOWN, /* BGR8  */
    GPU_TEXTURE_FORMAT_BC1,
    GPU_TEXTURE_FORMAT_BC2,
    GPU_TEXTURE_FORMAT_BC3,
    GPU_TEXTURE_FORMAT_BC7,
  };
  return ((u32)img < IMAGE_FORMAT_MAX_COUNT) ? mapping[img] : GPU_TEXTURE_FORMAT_UNKNOWN;
}

image_format_t image_format_for_gpu_texture_format(gpu_texture_format_t f)
{
  static const image_format_t mapping[GPU_TEXTURE_FORMAT_MAX_COUNT] = {
    IMAGE_FORMAT_NONE,    /* Unknown */
    IMAGE_FORMAT_RGBA8,   IMAGE_FORMAT_BGRA8, IMAGE_FORMAT_RGB565, IMAGE_FORMAT_RGB5A1, IMAGE_FORMAT_A1BGR5,
    IMAGE_FORMAT_NONE,    /* R8 */
    IMAGE_FORMAT_NONE,    IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE,    /* D* */
    IMAGE_FORMAT_NONE,    IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE,    /* R16* */
    IMAGE_FORMAT_NONE,    IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE,                       /* R32* */
    IMAGE_FORMAT_NONE,    IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE,    /* RG* */
    IMAGE_FORMAT_NONE,    IMAGE_FORMAT_NONE, IMAGE_FORMAT_NONE,                       /* RGBA16/16F/32F */
    IMAGE_FORMAT_NONE,    IMAGE_FORMAT_NONE,                                           /* RGB10A2, SRGBA8 */
    IMAGE_FORMAT_BC1,     IMAGE_FORMAT_BC2, IMAGE_FORMAT_BC3, IMAGE_FORMAT_BC7,
  };
  return ((u32)f < GPU_TEXTURE_FORMAT_MAX_COUNT) ? mapping[f] : IMAGE_FORMAT_NONE;
}

bool gpu_texture_validate_config(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                  gpu_texture_type_t type, gpu_texture_format_t format,
                                 gpu_texture_flags_t flags, u32 max_texture_size, u32 max_samples,
                                 Error* err) 
{
  if (width == 0 || width > GPU_TEXTURE_MAX_WIDTH || height == 0 || height > GPU_TEXTURE_MAX_HEIGHT ||
      layers == 0 || layers > GPU_TEXTURE_MAX_LAYERS || levels == 0 || levels > GPU_TEXTURE_MAX_LEVELS ||
      samples == 0 || samples > GPU_TEXTURE_MAX_SAMPLES)
  {
    Error_set_string_fmt(err, "Invalid dimensions: %ux%ux%u %u %u.", width, height, layers, levels, samples);
    return false;
  }

  if (max_texture_size != 0 && (width > max_texture_size || height > max_texture_size))
  {
    Error_set_string_fmt(err, "Texture width (%u) or height (%u) exceeds max texture size (%u).",
                         width, height, max_texture_size);
    return false;
  }

  if (max_samples != 0 && samples > max_samples)
  {
    Error_set_string_fmt(err, "Texture samples (%u) exceeds max samples (%u).", samples, max_samples);
    return false;
  }

  if (samples > 1)
  {
    if (levels > 1)
    {
      Error_set_string(err, "Multisampled textures can't have mip levels.");
      return false;
    }
    if (type != GPU_TEXTURE_TYPE_RENDER_TARGET && type != GPU_TEXTURE_TYPE_DEPTH_STENCIL)
    {
      Error_set_string(err, "Multisampled textures must be render targets or depth stencil targets.");
      return false;
    }
  }

  if (layers > 1 && type != GPU_TEXTURE_TYPE_TEXTURE)
  {
    Error_set_string(err, "Texture arrays are not supported on targets.");
    return false;
  }

  if (levels > 1 && type == GPU_TEXTURE_TYPE_DEPTH_STENCIL)
  {
    Error_set_string(err, "Mipmaps are not supported on depth stencil.");
    return false;
  }

  if ((flags & GPU_TEXTURE_FLAG_ALLOW_GENERATE_MIPMAPS) && levels <= 1)
  {
    Error_set_string(err, "Allow generate mipmaps requires >1 level.");
    return false;
  }

  if ((flags & GPU_TEXTURE_FLAG_ALLOW_BIND_AS_IMAGE) &&
      ((type != GPU_TEXTURE_TYPE_TEXTURE && type != GPU_TEXTURE_TYPE_RENDER_TARGET) || levels > 1))
  {
    Error_set_string(err, "Bind as image is not allowed on depth or mipmapped targets.");
    return false;
  }

  if ((flags & GPU_TEXTURE_FLAG_ALLOW_MAP) &&
      (type != GPU_TEXTURE_TYPE_TEXTURE || (flags & GPU_TEXTURE_FLAG_ALLOW_GENERATE_MIPMAPS)))
  {
    Error_set_string(err, "Allow map is not supported on targets.");
    return false;
  }

  if (gpu_texture_format_is_compressed(format) &&
      (type != GPU_TEXTURE_TYPE_TEXTURE || (flags & GPU_TEXTURE_FLAG_ALLOW_BIND_AS_IMAGE)))
  {
    Error_set_string(err, "Compressed formats are only supported for textures.");
    return false;
  }

  return true;
}

void gpu_texture_base_init(gpu_texture_t* t, u16 width, u16 height, u8 layers, u8 levels, u8 samples,
                           gpu_texture_type_t type, gpu_texture_format_t format, gpu_texture_flags_t flags)
{
  /* `m_height` immediately follows `m_width` so packed load (SizeVec) works. */
  _Static_assert(offsetof(gpu_texture_t, height) == offsetof(gpu_texture_t, width) + sizeof(u16),
                 "gpu_texture_t width/height must be contiguous u16 pair");
  t->width  = width;
  t->height = height;
  t->layers = layers;
  t->levels = levels;
  t->samples = samples;
  t->type   = type;
  t->format = format;
  t->flags  = flags;
  t->state  = GPU_TEXTURE_STATE_DIRTY;
  t->clear_value.color = 0;
}

void gpu_texture_get_unorm_clear_color(const gpu_texture_t* t, float out_rgba[4])
{
  const u32 c = t->clear_value.color;
  out_rgba[0] = ((c >>  0) & 0xFFu) * (1.0f / 255.0f);
  out_rgba[1] = ((c >>  8) & 0xFFu) * (1.0f / 255.0f);
  out_rgba[2] = ((c >> 16) & 0xFFu) * (1.0f / 255.0f);
  out_rgba[3] = ((c >> 24) & 0xFFu) * (1.0f / 255.0f);
}

size_t gpu_texture_get_vram_usage(const gpu_texture_t* t)
{
  const size_t ps = (size_t)gpu_texture_format_pixel_size(t->format) * t->layers * t->samples;
  size_t mem;

  if (gpu_texture_format_is_compressed(t->format))
  {
    u32 w = t->width, h = t->height;
    mem = ((size_t)(AlignUpPow2(w, GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) *
           (size_t)(AlignUpPow2(h, GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE)) * ps;
    for (u32 i = 1; i < t->levels; ++i)
    {
      w = (w > 1) ? (w / 2) : w;
      h = (h > 1) ? (h / 2) : h;
      mem += ((size_t)(AlignUpPow2(w, GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) *
              (size_t)(AlignUpPow2(h, GPU_TEXTURE_COMPRESSED_BLOCK_SIZE) / GPU_TEXTURE_COMPRESSED_BLOCK_SIZE)) * ps;
    }
  }
  else
  {
    u32 w = t->width, h = t->height;
    mem = (size_t)w * h * ps;
    for (u32 i = 1; i < t->levels; ++i)
    {
      w = (w > 1) ? (w / 2) : w;
      h = (h > 1) ? (h / 2) : h;
      mem += (size_t)w * h * ps;
    }
  }
  return mem;
}

void gpu_download_texture_base_init(gpu_download_texture_t* d, u32 width, u32 height,
                                    gpu_texture_format_t fmt, bool is_imported)
{
  d->width         = width;
  d->height        = height;
  d->format        = fmt;
  d->map_pointer   = NULL;
  d->current_pitch = 0;
  d->is_imported   = is_imported;
  d->needs_flush   = false;
}

u32 gpu_download_texture_get_buffer_size(u32 width, u32 height, gpu_texture_format_t fmt, u32 pitch_align)
{
  if (pitch_align == 0) pitch_align = 1;
  DebugAssert((pitch_align & (pitch_align - 1)) == 0);
  const u32 bpp   = gpu_texture_format_pixel_size(fmt);
  const u32 pitch = AlignUpPow2(width * bpp, pitch_align);
  return pitch * height;
}

u32 gpu_download_texture_get_transfer_pitch(const gpu_download_texture_t* d, u32 width, u32 pitch_align)
{
  if (pitch_align == 0) pitch_align = 1;
  DebugAssert((pitch_align & (pitch_align - 1)) == 0);
  const u32 bpp = gpu_texture_format_pixel_size(d->format);
  return AlignUpPow2(width * bpp, pitch_align);
}

void gpu_download_texture_get_transfer_size(const gpu_download_texture_t* d,
                                             u32 x, u32 y, u32 width, u32 height, u32 pitch,
                                            u32* copy_offset, u32* copy_size, u32* copy_rows) 
{
  const u32 bpp = gpu_texture_format_pixel_size(d->format);
  *copy_offset  = (y * pitch) + (x * bpp);
  *copy_size    = width * bpp;
  *copy_rows    = height;
}
