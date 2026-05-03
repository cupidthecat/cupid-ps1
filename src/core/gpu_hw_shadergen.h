/*
 * Single-impl collapse: `class GPU_HW_ShaderGen final : public ShaderGen` ->
 * struct embedding `shadergen_t base` at offset 0, plus free functions taking
 * `gpu_hw_shadergen_t*`.  Returns malloc'd C strings the caller frees with
 * `free()`.
 *
 * Drop list: sprite-mode texture filter shader bodies (Bilinear/JINC2/xBR/
 * MMPX/Scale variants), ROV feedback-loop blend variants, MSAA per-sample-
 * shading paths, adaptive downsample, replacement merge, texture cache,
 * internal post-FX; all wrapped with `#if 0 ... #endif // TODO` blocks for
 * easy reactivation.  Enum values for dropped shader-filter modes
 * are still preserved (gpu_texture_filter_t in core/types.h) so
 * the caller's BatchKey signature stays ABI-compatible.
 */

#ifndef CUPID_CORE_GPU_HW_SHADERGEN_H
#define CUPID_CORE_GPU_HW_SHADERGEN_H

#include "common/types.h"
#include "core/types.h"
#include "util/gpu_device.h"
#include "util/shadergen.h"

/* GPU_HW::BatchRenderMode; single namespace because we flattened the C++
 * `class GPU_HW`.  */
typedef enum : u8 {
  GPU_HW_BATCH_RENDER_MODE_TRANSPARENCY_DISABLED = 0,
  GPU_HW_BATCH_RENDER_MODE_TRANSPARENT_AND_OPAQUE,
  GPU_HW_BATCH_RENDER_MODE_ONLY_OPAQUE,
  GPU_HW_BATCH_RENDER_MODE_ONLY_TRANSPARENT,
  GPU_HW_BATCH_RENDER_MODE_SHADER_BLEND,
  GPU_HW_BATCH_RENDER_MODE_MAX_COUNT,
} gpu_hw_batch_render_mode_t;

/* GPU_HW::BatchTextureMode.  Sprite* variants kept for ABI/enum parity even
 * though sprite-filter shader bodies are dropped under TODO.
 * `Disabled` value is 4; consumers tested with `!= Disabled`
 * for the textured branch. */
typedef enum : u8 {
  GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT = 0,
  GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT,
  GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT,
  GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE,
  GPU_HW_BATCH_TEXTURE_MODE_DISABLED,

  GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PALETTE_4BIT,
  GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PALETTE_8BIT,
  GPU_HW_BATCH_TEXTURE_MODE_SPRITE_DIRECT_16BIT,
  GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PAGE_TEXTURE,

  GPU_HW_BATCH_TEXTURE_MODE_MAX_COUNT,
  GPU_HW_BATCH_TEXTURE_MODE_SPRITE_START = GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PALETTE_4BIT,
} gpu_hw_batch_texture_mode_t;

 /* HW shadergen state.  `shadergen_t base` MUST stay at offset 0; the .c
 * implementation aliases `&sg->base` directly. */
typedef struct gpu_hw_shadergen {
  shadergen_t base;
} gpu_hw_shadergen_t;

void gpu_hw_shadergen_init   (gpu_hw_shadergen_t* sg, gpu_render_api_t render_api,
                              bool supports_dual_source_blend, bool supports_framebuffer_fetch);
void gpu_hw_shadergen_destroy(gpu_hw_shadergen_t* sg);

/* All caller-frees-result generators.  Returns malloc'd NUL-terminated string. */

char* gpu_hw_shadergen_generate_screen_vertex_shader(gpu_hw_shadergen_t* sg);

char* gpu_hw_shadergen_generate_batch_vertex_shader(gpu_hw_shadergen_t* sg,
                                                    bool upscaled, bool msaa, bool per_sample_shading,
                                                    bool textured, bool palette, bool page_texture,
                                                    bool uv_limits, bool force_round_texcoords,
                                                    bool pgxp_depth, bool disable_color_perspective);

 char* gpu_hw_shadergen_generate_batch_fragment_shader(
  gpu_hw_shadergen_t* sg,
  gpu_hw_batch_render_mode_t render_mode, gpu_transparency_mode_t transparency,
  gpu_hw_batch_texture_mode_t texture_mode, gpu_texture_filter_t texture_filtering, 
  bool is_blended_texture_filtering, bool upscaled, bool msaa, bool per_sample_shading,
  bool uv_limits, bool force_round_texcoords, bool modulation_crop, bool true_color,
  bool dithering, bool scaled_dithering, bool disable_color_perspective, bool interlacing,
  bool scaled_interlacing, bool check_mask, bool write_mask_as_depth, bool use_rov,
  bool use_rov_depth, bool rov_depth_test, bool rov_depth_write);

char* gpu_hw_shadergen_generate_wireframe_geometry_shader(gpu_hw_shadergen_t* sg);
char* gpu_hw_shadergen_generate_wireframe_fragment_shader(gpu_hw_shadergen_t* sg);

char* gpu_hw_shadergen_generate_vram_read_fragment_shader   (gpu_hw_shadergen_t* sg, u32 resolution_scale, u32 multisamples);
char* gpu_hw_shadergen_generate_vram_write_fragment_shader  (gpu_hw_shadergen_t* sg, bool use_buffer, bool use_ssbo,
                                                              bool write_mask_as_depth, bool write_depth_as_rt);
char* gpu_hw_shadergen_generate_vram_copy_fragment_shader   (gpu_hw_shadergen_t* sg, bool write_mask_as_depth,
                                                              bool write_depth_as_rt);
char* gpu_hw_shadergen_generate_vram_fill_fragment_shader   (gpu_hw_shadergen_t* sg, bool wrapped, bool interlaced,
                                                              bool write_mask_as_depth, bool write_depth_as_rt);
char* gpu_hw_shadergen_generate_vram_update_depth_fragment_shader(gpu_hw_shadergen_t* sg, bool msaa);
char* gpu_hw_shadergen_generate_vram_copy_depth_fragment_shader  (gpu_hw_shadergen_t* sg, bool msaa);
char* gpu_hw_shadergen_generate_vram_clear_depth_fragment_shader (gpu_hw_shadergen_t* sg, bool write_depth_as_rt);
char* gpu_hw_shadergen_generate_vram_extract_fragment_shader     (gpu_hw_shadergen_t* sg, u32 resolution_scale,
                                                                  u32 multisamples, bool color_24bit, bool depth_buffer);
char* gpu_hw_shadergen_generate_vram_replacement_blit_fragment_shader(gpu_hw_shadergen_t* sg);

/* Box-sample downsampling (the "GenerateBoxSampleFragmentShader" referenced in
 * the keep list; full name is GenerateBoxSampleDownsampleFragmentShader).
 * Adaptive downsample variants and replacement merge are dropped under TODO. */
char* gpu_hw_shadergen_generate_box_sample_downsample_fragment_shader(gpu_hw_shadergen_t* sg, u32 factor);
char* gpu_hw_shadergen_generate_adaptive_downsample_vertex_shader(gpu_hw_shadergen_t* sg);
char* gpu_hw_shadergen_generate_adaptive_downsample_mip_fragment_shader(gpu_hw_shadergen_t* sg);
char* gpu_hw_shadergen_generate_adaptive_downsample_blur_fragment_shader(gpu_hw_shadergen_t* sg);
char* gpu_hw_shadergen_generate_adaptive_downsample_composite_fragment_shader(gpu_hw_shadergen_t* sg);
char* gpu_hw_shadergen_generate_replacement_merge_fragment_shader(gpu_hw_shadergen_t* sg, bool replacement,
                                                                  bool semitransparent, bool bilinear_filter);

#endif /* CUPID_CORE_GPU_HW_SHADERGEN_H */
