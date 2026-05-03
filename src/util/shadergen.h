/*
 * ShaderGen` -> `shadergen_t` struct + free functions.  `std::stringstream` ->
 * `small_string_t` (heap-grow already supported).  Returns malloc'd C string
 * the caller frees with `free()`.  ImGui/Fade/GaussianBlur generators dropped
 * (post-FX/UI not in cupid-ps1 scope).  D3D11/D3D12/Metal/Vulkan
 * branches preserved in the conditional code paths but only OpenGL is exercised
 * in the build (no ENABLE_VULKAN/ENABLE_METAL).
 */

#ifndef CUPID_UTIL_SHADERGEN_H
#define CUPID_UTIL_SHADERGEN_H

#include "common/small_string.h"
#include "common/types.h"
#include "util/gpu_device.h"

typedef struct shadergen {
  gpu_render_api_t      render_api;
  gpu_shader_language_t shader_language;
  bool glsl;
  bool spirv;
  bool supports_dual_source_blend;
  bool supports_framebuffer_fetch;
  bool use_glsl_interface_blocks;
  bool use_glsl_binding_layout;
  bool has_uniform_buffer;

  u32  glsl_version;
  /* up to 32-byte scratch for "#version 460 core" type strings */
  char glsl_version_string[32];
} shadergen_t;

void shadergen_init   (shadergen_t* sg, gpu_render_api_t api, gpu_shader_language_t lang,
                       bool supports_dual_source_blend, bool supports_framebuffer_fetch);
void shadergen_destroy(shadergen_t* sg);

gpu_shader_language_t shadergen_get_shader_language_for_api(gpu_render_api_t api);
bool                  shadergen_use_glsl_interface_blocks(void);
bool                  shadergen_use_glsl_binding_layout(void);

u32                   shadergen_get_glsl_version(gpu_render_api_t api);
void                  shadergen_get_glsl_version_string(gpu_render_api_t api, u32 version, char out[32]);

ALWAYS_INLINE gpu_shader_language_t shadergen_get_language(const shadergen_t* sg) { return sg->shader_language; }
ALWAYS_INLINE bool shadergen_is_vulkan(const shadergen_t* sg) { return sg->render_api == GPU_RENDER_API_VULKAN; }
ALWAYS_INLINE bool shadergen_is_metal (const shadergen_t* sg) { return sg->render_api == GPU_RENDER_API_METAL; }

const char* shadergen_get_interpolation_qualifier(const shadergen_t* sg, bool interface_block,
                                                   bool centroid_interpolation, bool sample_interpolation,
                                                   bool is_out);

void shadergen_define_macro_bool(const shadergen_t* sg, small_string_t* ss, const char* name, bool enabled);
void shadergen_define_macro_int (const shadergen_t* sg, small_string_t* ss, const char* name, s32 value);
void shadergen_write_header     (shadergen_t* sg, small_string_t* ss, bool enable_rov,
                                 bool enable_framebuffer_fetch, bool enable_dual_source_blend);
void shadergen_write_uniform_buffer_decl(const shadergen_t* sg, small_string_t* ss, bool push_constant);
void shadergen_declare_uniform_buffer   (shadergen_t* sg, small_string_t* ss,
                                         const char* const* members, u32 member_count, bool push_constant);
void shadergen_declare_texture          (const shadergen_t* sg, small_string_t* ss, const char* name, u32 index,
                                         bool multisampled, bool is_int, bool is_unsigned);
void shadergen_declare_texture_buffer   (const shadergen_t* sg, small_string_t* ss, const char* name, u32 index,
                                         bool is_int, bool is_unsigned);
void shadergen_declare_image            (const shadergen_t* sg, small_string_t* ss, const char* name, u32 index,
                                         bool is_float, bool is_int, bool is_unsigned);

 /* Pair (qualifier, name) for additional vertex/fragment IO, mirroring the C++
 * `std::initializer_list<std::pair<const char*, const char*>>`. */
typedef struct {
  const char* qualifier;
  const char* name;
} shadergen_io_pair_t;

void shadergen_declare_vertex_entry_point  (const shadergen_t* sg, small_string_t* ss,
                                             const char* const* attributes, u32 attribute_count,
                                             u32 num_color_outputs, u32 num_texcoord_outputs,
                                             const shadergen_io_pair_t* additional_outputs,
                                             u32 additional_output_count,
                                             bool declare_vertex_id, const char* output_block_suffix,
                                             bool msaa, bool ssaa, bool noperspective_color);

void shadergen_declare_fragment_entry_point(const shadergen_t* sg, small_string_t* ss,
                                             u32 num_color_inputs, u32 num_texcoord_inputs,
                                             const shadergen_io_pair_t* additional_inputs,
                                             u32 additional_input_count,
                                             bool declare_fragcoord, u32 num_color_outputs,
                                             bool dual_source_output, bool depth_output,
                                             bool msaa, bool ssaa, bool declare_sample_id,
                                             bool noperspective_color, bool feedback_loop, bool rov);

/* Caller frees returned string with free(). */
char* shadergen_generate_passthrough_vertex_shader(shadergen_t* sg);
char* shadergen_generate_screen_quad_vertex_shader(shadergen_t* sg, float z);
char* shadergen_generate_fill_fragment_shader     (shadergen_t* sg);
char* shadergen_generate_fill_fragment_shader_fixed(shadergen_t* sg, float r, float g, float b, float a);
char* shadergen_generate_copy_fragment_shader     (shadergen_t* sg, bool offset);

#endif /* CUPID_UTIL_SHADERGEN_H */
