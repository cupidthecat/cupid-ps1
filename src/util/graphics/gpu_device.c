/*
 *

 * + sampler/blend/depth/raster preset constructors.  Full GPUDevice driver
 * interface (Create, render-target dispatch, draw, present) is implemented
 * by OpenGLDevice.
 */
#include "gpu_device.h"

#include <string.h>

gpu_device_t*           g_gpu_device                = NULL;
size_t                  g_gpu_device_total_vram_usage = 0;
gpu_device_statistics_t g_gpu_device_stats          = {0};

void gpu_device_reset_statistics(void) { memset(&g_gpu_device_stats, 0, sizeof(g_gpu_device_stats)); }
void gpu_device_track_texture_alloc(size_t bytes) { g_gpu_device_total_vram_usage += bytes; }
void gpu_device_track_texture_free (size_t bytes) { g_gpu_device_total_vram_usage -= bytes; }

const char* gpu_render_api_to_string(gpu_render_api_t api)
{
  switch (api)
  {
    case GPU_RENDER_API_NONE:       return "None";
    case GPU_RENDER_API_D3D11:      return "D3D11";
    case GPU_RENDER_API_D3D12:      return "D3D12";
    case GPU_RENDER_API_VULKAN:     return "Vulkan";
    case GPU_RENDER_API_OPENGL:     return "OpenGL";
    case GPU_RENDER_API_OPENGL_ES:  return "OpenGL ES";
    case GPU_RENDER_API_METAL:      return "Metal";
    default:                        return "?";
  }
}

const char* gpu_shader_language_to_string(gpu_shader_language_t lang)
{
  switch (lang)
  {
    case GPU_SHADER_LANGUAGE_NONE:    return "None";
    case GPU_SHADER_LANGUAGE_HLSL:    return "HLSL";
    case GPU_SHADER_LANGUAGE_GLSL:    return "GLSL";
    case GPU_SHADER_LANGUAGE_GLSL_ES: return "GLSL ES";
    case GPU_SHADER_LANGUAGE_GLSL_VK: return "GLSL (Vulkan)";
    case GPU_SHADER_LANGUAGE_MSL:     return "Metal Shading Language";
    case GPU_SHADER_LANGUAGE_SPV:     return "SPIR-V";
    default:                          return "?";
  }
}

const char* gpu_vsync_mode_to_string(gpu_vsync_mode_t m)
{
  switch (m)
  {
    case GPU_VSYNC_MODE_DISABLED: return "Disabled";
    case GPU_VSYNC_MODE_FIFO:     return "FIFO (V-Sync)";
    case GPU_VSYNC_MODE_MAILBOX:  return "Mailbox";
    default:                      return "?";
  }
}

const char* gpu_shader_stage_name(gpu_shader_stage_t s)
{
  switch (s)
  {
    case GPU_SHADER_STAGE_VERTEX:   return "Vertex";
    case GPU_SHADER_STAGE_FRAGMENT: return "Fragment";
    case GPU_SHADER_STAGE_GEOMETRY: return "Geometry";
    case GPU_SHADER_STAGE_COMPUTE:  return "Compute";
    default:                        return "?";
  }
}

void gpu_device_rgba8_to_float(u32 rgba, float out[4])
{
  out[0] = ((rgba >> 0)  & 0xFFu) / 255.0f;
  out[1] = ((rgba >> 8)  & 0xFFu) / 255.0f;
  out[2] = ((rgba >> 16) & 0xFFu) / 255.0f;
  out[3] = ((rgba >> 24) & 0xFFu) / 255.0f;
}

bool gpu_device_is_same_render_api(gpu_render_api_t a, gpu_render_api_t b)
{
  if (a == b) return true;
  if ((a == GPU_RENDER_API_OPENGL && b == GPU_RENDER_API_OPENGL_ES) ||
      (a == GPU_RENDER_API_OPENGL_ES && b == GPU_RENDER_API_OPENGL))
    return true;
  return false;
}

bool gpu_device_has_create_flag(gpu_device_create_flags_t f, gpu_device_create_flags_t flag)
{
  return (f & flag) != 0;
}

gpu_driver_type_t gpu_device_guess_driver_type(u32 pci_vendor_id, const char* vendor_name, const char* adapter_name)
{
  /* PCI vendor IDs: 0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel,
   * 0x106B Apple, 0x1010 ImgTec, 0x13B5 ARM, 0x5143 Qualcomm, 0x14E4 Broadcom. */
  switch (pci_vendor_id)
  {
    case 0x1002: return GPU_DRIVER_TYPE_AMD_PROPRIETARY;
    case 0x10DE: return GPU_DRIVER_TYPE_NVIDIA_PROPRIETARY;
    case 0x8086: return GPU_DRIVER_TYPE_INTEL_PROPRIETARY;
    case 0x106B: return GPU_DRIVER_TYPE_APPLE_PROPRIETARY;
    case 0x1010: return GPU_DRIVER_TYPE_IMAGINATION_PROPRIETARY;
    case 0x13B5: return GPU_DRIVER_TYPE_ARM_PROPRIETARY;
    case 0x5143: return GPU_DRIVER_TYPE_QUALCOMM_PROPRIETARY;
    case 0x14E4: return GPU_DRIVER_TYPE_BROADCOM_PROPRIETARY;
    default: break;
  }

  if (vendor_name && strstr(vendor_name, "Mesa"))
  {
    if (adapter_name && strstr(adapter_name, "llvmpipe"))      return GPU_DRIVER_TYPE_LLVMPIPE;
    if (adapter_name && strstr(adapter_name, "swrast"))        return GPU_DRIVER_TYPE_LLVMPIPE;
  }
  return GPU_DRIVER_TYPE_UNKNOWN;
}

gpu_sampler_config_t gpu_sampler_get_nearest_config(void)
{
  gpu_sampler_config_t c = {0};
  c.bits.min_filter = GPU_SAMPLER_FILTER_NEAREST;
  c.bits.mag_filter = GPU_SAMPLER_FILTER_NEAREST;
  c.bits.mip_filter = GPU_SAMPLER_FILTER_NEAREST;
  c.bits.address_u  = GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
  c.bits.address_v  = GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
  c.bits.address_w  = GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
  c.bits.min_lod    = 0;
  c.bits.max_lod    = GPU_SAMPLER_LOD_MAX;
  return c;
}

gpu_sampler_config_t gpu_sampler_get_linear_config(void)
{
  gpu_sampler_config_t c = {0};
  c.bits.min_filter = GPU_SAMPLER_FILTER_LINEAR;
  c.bits.mag_filter = GPU_SAMPLER_FILTER_LINEAR;
  c.bits.mip_filter = GPU_SAMPLER_FILTER_LINEAR;
  c.bits.address_u  = GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
  c.bits.address_v  = GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
  c.bits.address_w  = GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
  c.bits.min_lod    = 0;
  c.bits.max_lod    = GPU_SAMPLER_LOD_MAX;
  return c;
}

gpu_rasterization_state_t gpu_rasterization_state_no_cull(u8 multisamples, bool per_sample_shading)
{
  gpu_rasterization_state_t s = {0};
  s.bits.cull_mode          = GPU_CULL_MODE_NONE;
  s.bits.multisamples       = multisamples ? multisamples : 1u;
  s.bits.per_sample_shading = per_sample_shading;
  return s;
}

gpu_depth_state_t gpu_depth_state_no_tests(void)
{
  gpu_depth_state_t s = {0};
  s.bits.depth_test  = GPU_DEPTH_FUNC_ALWAYS;
  s.bits.depth_write = false;
  return s;
}

gpu_depth_state_t gpu_depth_state_always_write(void)
{
  gpu_depth_state_t s = {0};
  s.bits.depth_test  = GPU_DEPTH_FUNC_ALWAYS;
  s.bits.depth_write = true;
  return s;
}

gpu_blend_state_t gpu_blend_state_no_blending(void)
{
  gpu_blend_state_t s = {0};
  s.bits.write_r = s.bits.write_g = s.bits.write_b = s.bits.write_a = true;
  s.bits.src_blend = GPU_BLEND_FUNC_ONE;
  s.bits.dst_blend = GPU_BLEND_FUNC_ZERO;
  s.bits.blend_op  = GPU_BLEND_OP_ADD;
  s.bits.src_alpha_blend = GPU_BLEND_FUNC_ONE;
  s.bits.dst_alpha_blend = GPU_BLEND_FUNC_ZERO;
  s.bits.alpha_blend_op  = GPU_BLEND_OP_ADD;
  return s;
}

gpu_blend_state_t gpu_blend_state_alpha_blending(void)
{
  gpu_blend_state_t s = gpu_blend_state_no_blending();
  s.bits.enable          = true;
  s.bits.src_blend       = GPU_BLEND_FUNC_SRC_ALPHA;
  s.bits.dst_blend       = GPU_BLEND_FUNC_INV_SRC_ALPHA;
  s.bits.src_alpha_blend = GPU_BLEND_FUNC_ONE;
  s.bits.dst_alpha_blend = GPU_BLEND_FUNC_ZERO;
  return s;
}

void gpu_pipeline_graphics_config_set_target_formats(gpu_pipeline_graphics_config_t* c,
                                                     gpu_texture_format_t color, gpu_texture_format_t depth)
{
  for (u32 i = 0; i < GPU_DEVICE_MAX_RENDER_TARGETS; ++i)
    c->color_formats[i] = (i == 0) ? color : GPU_TEXTURE_FORMAT_UNKNOWN;
  c->depth_format = depth;
}

u32 gpu_pipeline_graphics_config_render_target_count(const gpu_pipeline_graphics_config_t* c)
{
  u32 n = 0;
  for (u32 i = 0; i < GPU_DEVICE_MAX_RENDER_TARGETS; ++i)
    if (c->color_formats[i] != GPU_TEXTURE_FORMAT_UNKNOWN) ++n;
  return n;
}

u32 gpu_device_active_textures_for_layout(gpu_pipeline_layout_t l)
{
  static const u8 counts[GPU_PIPELINE_LAYOUT_MAX_COUNT] = {
    1,                                  /* SingleTextureAndUBO */
    1,                                  /* SingleTextureAndPushConstants */
    0,                                  /* SingleTextureBufferAndPushConstants */
    GPU_DEVICE_MAX_TEXTURE_SAMPLERS,    /* MultiTextureAndUBO */
    GPU_DEVICE_MAX_TEXTURE_SAMPLERS,    /* MultiTextureAndPushConstants */
    GPU_DEVICE_MAX_TEXTURE_SAMPLERS,    /* MultiTextureAndUBOAndPushConstants */
    GPU_DEVICE_MAX_TEXTURE_SAMPLERS,    /* ComputeMultiTextureAndUBO */
    GPU_DEVICE_MAX_TEXTURE_SAMPLERS,    /* ComputeMultiTextureAndPushConstants */
  };
  return ((u32)l < GPU_PIPELINE_LAYOUT_MAX_COUNT) ? counts[l] : 0u;
}

bool gpu_device_layout_is_compute(gpu_pipeline_layout_t l)
{
  return l >= GPU_PIPELINE_LAYOUT_COMPUTE_MULTI_TEXTURE_AND_UBO;
}

void gpu_texture_buffer_base_init(gpu_texture_buffer_t* b, gpu_texture_buffer_format_t f, u32 size_in_elements)
{
  b->format            = f;
  b->size_in_elements  = size_in_elements;
  b->current_position  = 0;
}

u32 gpu_texture_buffer_element_size(gpu_texture_buffer_format_t f)
{
  switch (f)
  {
    case GPU_TEXTURE_BUFFER_FORMAT_R16UI: return 2;
    default:                              return 0;
  }
}

void gpu_swap_chain_base_init(gpu_swap_chain_t* sc, const window_info_t* wi, gpu_vsync_mode_t mode)
{
  sc->window_info = *wi;
  sc->vsync_mode  = mode;
}
