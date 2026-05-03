/*
 * gpu_device.h - enums, config bitfield unions, pipeline state structs, sampler
 * config, statistics/features structs, MAX_* constants, plus the global
 * vram-usage / stats counters and a few free-function helpers.  The full
 * GPUDevice virtual API is collapsed into free functions in `gpu_device.c` and
 * `opengl_device.c` (single-impl) - those land in subsequent batches.
 *
 * Single-impl collapse: `GPUSampler`/`GPUShader`/`GPUPipeline`/`GPUSwapChain`/
 * `GPUTextureBuffer` are flat structs.  The OpenGL backend extends each via
 * struct embedding (`opengl_sampler_t { gpu_sampler_t base; GLuint id; }` etc).
 *
 * `BitField<u64, T, OFFSET, COUNT>` translates to plain C bitfields in unions.
 */

#ifndef CUPID_UTIL_GPU_DEVICE_H
#define CUPID_UTIL_GPU_DEVICE_H

#include "common/types.h"
#include "util/gpu_shader_cache.h"
#include "util/gpu_texture.h"
#include "util/gpu_types.h"
#include "util/window_info.h"

typedef struct Error Error;

enum {
  GPU_DEVICE_MAX_TEXTURE_SAMPLERS       = 8,
  GPU_DEVICE_MIN_TEXEL_BUFFER_ELEMENTS  = 4 * 1024 * 512,
  GPU_DEVICE_MAX_RENDER_TARGETS         = 4,
  GPU_DEVICE_MAX_IMAGE_RENDER_TARGETS   = 2,
  GPU_DEVICE_DEFAULT_CLEAR_COLOR        = (int)0xFF000000,
  GPU_DEVICE_PIPELINE_CACHE_HASH_SIZE   = 20,
  GPU_DEVICE_BASE_UNIFORM_BUFFER_ALIGNMENT = 16,
};

typedef enum {
  GPU_SAMPLER_FILTER_NEAREST = 0,
  GPU_SAMPLER_FILTER_LINEAR,
  GPU_SAMPLER_FILTER_MAX_COUNT,
} gpu_sampler_filter_t;

typedef enum {
  GPU_SAMPLER_ADDRESS_REPEAT = 0,
  GPU_SAMPLER_ADDRESS_CLAMP_TO_EDGE,
  GPU_SAMPLER_ADDRESS_CLAMP_TO_BORDER,
  GPU_SAMPLER_ADDRESS_MIRROR_REPEAT,
  GPU_SAMPLER_ADDRESS_MAX_COUNT,
} gpu_sampler_address_mode_t;

#define GPU_SAMPLER_LOD_MAX 15

/* BitField<u64, ...> packed into a u64.  C bitfields in a struct + union with */
typedef union {
  u64 key;
  struct {
    u64 min_filter   : 1;   /* gpu_sampler_filter_t */
    u64 mag_filter   : 1;
    u64 mip_filter   : 1;
    u64 address_u    : 2;   /* gpu_sampler_address_mode_t */
    u64 address_v    : 2;
    u64 address_w    : 2;
    u64 anisotropy   : 5;
    u64 min_lod      : 4;
    u64 max_lod      : 4;   /* total: 22 bits */
    u64 _pad0        : 10;
    u64 border_color : 32;
  } bits;
} gpu_sampler_config_t;

ALWAYS_INLINE float gpu_sampler_config_border_r(gpu_sampler_config_t c) { return ((c.bits.border_color >> 0)  & 0xFFu) / 255.0f; }
ALWAYS_INLINE float gpu_sampler_config_border_g(gpu_sampler_config_t c) { return ((c.bits.border_color >> 8)  & 0xFFu) / 255.0f; }
ALWAYS_INLINE float gpu_sampler_config_border_b(gpu_sampler_config_t c) { return ((c.bits.border_color >> 16) & 0xFFu) / 255.0f; }
ALWAYS_INLINE float gpu_sampler_config_border_a(gpu_sampler_config_t c) { return ((c.bits.border_color >> 24) & 0xFFu) / 255.0f; }
ALWAYS_INLINE void  gpu_sampler_config_border_rgba(gpu_sampler_config_t c, float out[4]) {
  out[0] = gpu_sampler_config_border_r(c); out[1] = gpu_sampler_config_border_g(c);
  out[2] = gpu_sampler_config_border_b(c); out[3] = gpu_sampler_config_border_a(c);
}

gpu_sampler_config_t gpu_sampler_get_nearest_config(void);
gpu_sampler_config_t gpu_sampler_get_linear_config(void);

typedef struct gpu_sampler {
  /* base; backend extends with GLuint id.  Empty for now to keep alignment. */
  u8 _placeholder;
} gpu_sampler_t;

typedef struct gpu_shader {
  gpu_shader_stage_t stage;
} gpu_shader_t;

const char* gpu_shader_stage_name(gpu_shader_stage_t s);

typedef enum {
  GPU_PIPELINE_LAYOUT_SINGLE_TEXTURE_AND_UBO = 0,
  GPU_PIPELINE_LAYOUT_SINGLE_TEXTURE_AND_PUSH_CONSTANTS,
  GPU_PIPELINE_LAYOUT_SINGLE_TEXTURE_BUFFER_AND_PUSH_CONSTANTS,
  GPU_PIPELINE_LAYOUT_MULTI_TEXTURE_AND_UBO,
  GPU_PIPELINE_LAYOUT_MULTI_TEXTURE_AND_PUSH_CONSTANTS,
  GPU_PIPELINE_LAYOUT_MULTI_TEXTURE_AND_UBO_AND_PUSH_CONSTANTS,
  GPU_PIPELINE_LAYOUT_COMPUTE_MULTI_TEXTURE_AND_UBO,
  GPU_PIPELINE_LAYOUT_COMPUTE_MULTI_TEXTURE_AND_PUSH_CONSTANTS,
  GPU_PIPELINE_LAYOUT_MAX_COUNT,
} gpu_pipeline_layout_t;

typedef u8 gpu_pipeline_render_pass_flag_t;
#define GPU_PIPELINE_RENDER_PASS_NONE                       (0u)
#define GPU_PIPELINE_RENDER_PASS_COLOR_FEEDBACK_LOOP        (1u << 0)
#define GPU_PIPELINE_RENDER_PASS_COLOR_FEEDBACK_LOOP_ACTIVE (1u << 1)
#define GPU_PIPELINE_RENDER_PASS_SAMPLE_DEPTH_BUFFER        (1u << 2)
#define GPU_PIPELINE_RENDER_PASS_BIND_RTS_AS_IMAGES         (1u << 3)

typedef enum {
  GPU_PIPELINE_PRIMITIVE_POINTS = 0,
  GPU_PIPELINE_PRIMITIVE_LINES,
  GPU_PIPELINE_PRIMITIVE_TRIANGLES,
  GPU_PIPELINE_PRIMITIVE_TRIANGLE_STRIPS,
  GPU_PIPELINE_PRIMITIVE_MAX_COUNT,
} gpu_pipeline_primitive_t;

typedef enum {
  GPU_VERTEX_SEMANTIC_POSITION = 0,
  GPU_VERTEX_SEMANTIC_TEXCOORD,
  GPU_VERTEX_SEMANTIC_COLOR,
  GPU_VERTEX_SEMANTIC_MAX_COUNT,
} gpu_vertex_semantic_t;

typedef enum {
  GPU_VERTEX_TYPE_FLOAT = 0,
  GPU_VERTEX_TYPE_UINT8,  GPU_VERTEX_TYPE_SINT8,  GPU_VERTEX_TYPE_UNORM8,
  GPU_VERTEX_TYPE_UINT16, GPU_VERTEX_TYPE_SINT16, GPU_VERTEX_TYPE_UNORM16,
  GPU_VERTEX_TYPE_UINT32, GPU_VERTEX_TYPE_SINT32,
  GPU_VERTEX_TYPE_MAX_COUNT,
} gpu_vertex_type_t;

#define GPU_VERTEX_MAX_ATTRIBUTES 16

typedef union {
  u32 key;
  struct {
    u32 index           : 4;
    u32 semantic        : 2;
    u32 semantic_index  : 2;
    u32 type            : 4;
    u32 components      : 3;
    u32 _pad            : 1;
    u32 offset          : 16;
  } bits;
} gpu_vertex_attribute_t;

ALWAYS_INLINE gpu_vertex_attribute_t
gpu_vertex_attribute_make(u8 index, gpu_vertex_semantic_t sem, u8 sem_idx,
                          gpu_vertex_type_t type, u8 components, u16 offset)
{
  gpu_vertex_attribute_t v;
  v.key = ((u32)(index & 0xF))            |
          (((u32)sem & 0x3)         << 4) |
          (((u32)(sem_idx & 0x3))   << 6) |
          (((u32)type & 0xF)        << 8) |
          (((u32)(components & 0x7))<< 12)|
          (((u32)offset & 0xFFFF)   << 16);
  return v;
}

typedef struct {
  const gpu_vertex_attribute_t* vertex_attributes;
  u32                           vertex_attribute_count;
  u32                           vertex_stride;
} gpu_input_layout_t;

typedef enum { GPU_CULL_MODE_NONE = 0, GPU_CULL_MODE_FRONT, GPU_CULL_MODE_BACK, GPU_CULL_MODE_MAX_COUNT } gpu_cull_mode_t;
typedef enum {
  GPU_DEPTH_FUNC_NEVER = 0, GPU_DEPTH_FUNC_ALWAYS, GPU_DEPTH_FUNC_LESS, GPU_DEPTH_FUNC_LESS_EQUAL,
  GPU_DEPTH_FUNC_GREATER, GPU_DEPTH_FUNC_GREATER_EQUAL, GPU_DEPTH_FUNC_EQUAL, GPU_DEPTH_FUNC_MAX_COUNT,
} gpu_depth_func_t;

typedef enum {
  GPU_BLEND_FUNC_ZERO = 0, GPU_BLEND_FUNC_ONE,
  GPU_BLEND_FUNC_SRC_COLOR, GPU_BLEND_FUNC_INV_SRC_COLOR,
  GPU_BLEND_FUNC_DST_COLOR, GPU_BLEND_FUNC_INV_DST_COLOR,
  GPU_BLEND_FUNC_SRC_ALPHA, GPU_BLEND_FUNC_INV_SRC_ALPHA,
  GPU_BLEND_FUNC_SRC_ALPHA1, GPU_BLEND_FUNC_INV_SRC_ALPHA1,
  GPU_BLEND_FUNC_DST_ALPHA, GPU_BLEND_FUNC_INV_DST_ALPHA,
  GPU_BLEND_FUNC_CONSTANT_COLOR, GPU_BLEND_FUNC_INV_CONSTANT_COLOR,
  GPU_BLEND_FUNC_MAX_COUNT,
} gpu_blend_func_t;
typedef enum {
  GPU_BLEND_OP_ADD = 0, GPU_BLEND_OP_SUBTRACT, GPU_BLEND_OP_REVERSE_SUBTRACT,
  GPU_BLEND_OP_MIN, GPU_BLEND_OP_MAX, GPU_BLEND_OP_MAX_COUNT,
} gpu_blend_op_t;

typedef union {
  u16 key;
  struct {
    u16 cull_mode          : 2;
    u16 multisamples       : 6;
    u16 per_sample_shading : 1;
    u16 _pad               : 7;
  } bits;
} gpu_rasterization_state_t;

gpu_rasterization_state_t gpu_rasterization_state_no_cull(u8 multisamples, bool per_sample_shading);

typedef union {
  u8 key;
  struct {
    u8 depth_test  : 3;   /* gpu_depth_func_t */
    u8 _pad        : 1;
    u8 depth_write : 1;
    u8 _pad2       : 3;
  } bits;
} gpu_depth_state_t;

gpu_depth_state_t gpu_depth_state_no_tests(void);
gpu_depth_state_t gpu_depth_state_always_write(void);

typedef union {
  u64 key;
  struct {
    u64 enable          : 1;
    u64 src_blend       : 4;
    u64 src_alpha_blend : 4;
    u64 dst_blend       : 4;
    u64 dst_alpha_blend : 4;
    u64 blend_op        : 3;
    u64 alpha_blend_op  : 3;
    u64 _pad            : 1;
    u64 write_r         : 1;
    u64 write_g         : 1;
    u64 write_b         : 1;
    u64 write_a         : 1;
    u64 _pad2           : 4;
    u64 constant        : 32;
  } bits;
} gpu_blend_state_t;

gpu_blend_state_t gpu_blend_state_no_blending(void);
gpu_blend_state_t gpu_blend_state_alpha_blending(void);

typedef struct {
  gpu_input_layout_t          input_layout;
  gpu_shader_t*               vertex_shader;
  gpu_shader_t*               geometry_shader;
  gpu_shader_t*               fragment_shader;
  gpu_blend_state_t           blend;
  gpu_rasterization_state_t   rasterization;
  gpu_depth_state_t           depth;
  gpu_pipeline_layout_t       layout;
  gpu_pipeline_primitive_t    primitive;
  gpu_texture_format_t        color_formats[GPU_DEVICE_MAX_RENDER_TARGETS];
  gpu_texture_format_t        depth_format;
  gpu_pipeline_render_pass_flag_t render_pass_flags;
} gpu_pipeline_graphics_config_t;

void gpu_pipeline_graphics_config_set_target_formats(gpu_pipeline_graphics_config_t* c,
                                                     gpu_texture_format_t color, gpu_texture_format_t depth);
u32  gpu_pipeline_graphics_config_render_target_count(const gpu_pipeline_graphics_config_t* c);

typedef struct {
  gpu_pipeline_layout_t layout;
  gpu_shader_t*         compute_shader;
} gpu_pipeline_compute_config_t;

typedef struct gpu_pipeline {
  /* backend extends with GLuint vao + GLuint program + state */
  u8 _placeholder;
} gpu_pipeline_t;

u32  gpu_device_active_textures_for_layout(gpu_pipeline_layout_t l);
bool gpu_device_layout_is_compute(gpu_pipeline_layout_t l);

typedef enum {
  GPU_TEXTURE_BUFFER_FORMAT_R16UI = 0,
  GPU_TEXTURE_BUFFER_FORMAT_MAX_COUNT,
} gpu_texture_buffer_format_t;

typedef struct gpu_texture_buffer {
  gpu_texture_buffer_format_t format;
  u32                          size_in_elements;
  u32                          current_position;
} gpu_texture_buffer_t;

void gpu_texture_buffer_base_init(gpu_texture_buffer_t* b, gpu_texture_buffer_format_t f, u32 size_in_elements);
u32  gpu_texture_buffer_element_size(gpu_texture_buffer_format_t f);

ALWAYS_INLINE u32 gpu_texture_buffer_size_in_bytes(const gpu_texture_buffer_t* b)
{ return b->size_in_elements * gpu_texture_buffer_element_size(b->format); }

typedef struct gpu_swap_chain {
  window_info_t    window_info;
  gpu_vsync_mode_t vsync_mode;
} gpu_swap_chain_t;

void gpu_swap_chain_base_init(gpu_swap_chain_t* sc, const window_info_t* wi, gpu_vsync_mode_t mode);

ALWAYS_INLINE u32   gpu_swap_chain_get_width (const gpu_swap_chain_t* sc) { return sc->window_info.surface_width;  }
ALWAYS_INLINE u32   gpu_swap_chain_get_height(const gpu_swap_chain_t* sc) { return sc->window_info.surface_height; }
ALWAYS_INLINE float gpu_swap_chain_get_scale (const gpu_swap_chain_t* sc) { return sc->window_info.surface_scale;  }
ALWAYS_INLINE float gpu_swap_chain_get_refresh_rate(const gpu_swap_chain_t* sc) { return sc->window_info.surface_refresh_rate; }
ALWAYS_INLINE gpu_texture_format_t gpu_swap_chain_get_format(const gpu_swap_chain_t* sc) { return sc->window_info.surface_format; }
ALWAYS_INLINE gpu_vsync_mode_t     gpu_swap_chain_get_vsync_mode(const gpu_swap_chain_t* sc) { return sc->vsync_mode; }
ALWAYS_INLINE bool gpu_swap_chain_is_vsync_blocking(const gpu_swap_chain_t* sc) { return sc->vsync_mode == GPU_VSYNC_MODE_FIFO; }

typedef u32 gpu_device_create_flags_t;
#define GPU_DEVICE_CREATE_NONE                          (0u)
#define GPU_DEVICE_CREATE_PREFER_GLES_CONTEXT           (1u << 0)
#define GPU_DEVICE_CREATE_ENABLE_DEBUG_DEVICE           (1u << 1)
#define GPU_DEVICE_CREATE_ENABLE_GPU_VALIDATION         (1u << 2)
#define GPU_DEVICE_CREATE_DISABLE_SHADER_CACHE          (1u << 3)
#define GPU_DEVICE_CREATE_DISABLE_DUAL_SOURCE_BLEND     (1u << 4)
#define GPU_DEVICE_CREATE_DISABLE_FEEDBACK_LOOPS        (1u << 5)
#define GPU_DEVICE_CREATE_DISABLE_FRAMEBUFFER_FETCH     (1u << 6)
#define GPU_DEVICE_CREATE_DISABLE_TEXTURE_BUFFERS       (1u << 7)
#define GPU_DEVICE_CREATE_DISABLE_GEOMETRY_SHADERS      (1u << 8)
#define GPU_DEVICE_CREATE_DISABLE_COMPUTE_SHADERS       (1u << 9)
#define GPU_DEVICE_CREATE_DISABLE_TEXTURE_COPY_TO_SELF  (1u << 10)
#define GPU_DEVICE_CREATE_DISABLE_MEMORY_IMPORT         (1u << 11)
#define GPU_DEVICE_CREATE_DISABLE_RASTER_ORDER_VIEWS    (1u << 12)
#define GPU_DEVICE_CREATE_DISABLE_COMPRESSED_TEXTURES   (1u << 13)

typedef enum {
  GPU_DRAW_BARRIER_NONE = 0,
  GPU_DRAW_BARRIER_ONE,
  GPU_DRAW_BARRIER_FULL,
} gpu_draw_barrier_t;

typedef struct {
  bool dual_source_blend;
  bool framebuffer_fetch;
  bool per_sample_shading;
  bool noperspective_interpolation;
  bool texture_copy_to_self;
  bool texture_buffers;
  bool texture_buffers_emulated_with_ssbo;
  bool feedback_loops;
  bool geometry_shaders;
  bool compute_shaders;
  bool partial_msaa_resolve;
  bool memory_import;
  bool exclusive_fullscreen;
  bool explicit_present;
  bool timed_present;
  bool gpu_timing;
  bool shader_cache;
  bool pipeline_cache;
  bool prefer_unused_textures;
  bool raster_order_views;
  bool dxt_textures;
  bool bptc_textures;
} gpu_device_features_t;

typedef struct {
  size_t buffer_streamed;
  u32    num_draws;
  u32    num_barriers;
  u32    num_render_passes;
  u32    num_copies;
  u32    num_downloads;
  u32    num_uploads;
} gpu_device_statistics_t;

extern gpu_device_statistics_t g_gpu_device_stats;
extern size_t g_gpu_device_total_vram_usage;

ALWAYS_INLINE gpu_device_statistics_t* gpu_device_get_statistics(void) { return &g_gpu_device_stats; }
ALWAYS_INLINE size_t                   gpu_device_get_total_vram_usage(void) { return g_gpu_device_total_vram_usage; }

void gpu_device_reset_statistics(void);
void gpu_device_track_texture_alloc(size_t bytes);
void gpu_device_track_texture_free (size_t bytes);

/* Free-function helpers. */
const char* gpu_render_api_to_string    (gpu_render_api_t api);
const char* gpu_shader_language_to_string(gpu_shader_language_t lang);
const char* gpu_vsync_mode_to_string    (gpu_vsync_mode_t m);
void        gpu_device_rgba8_to_float   (u32 rgba, float out[4]);
bool        gpu_device_is_same_render_api(gpu_render_api_t a, gpu_render_api_t b);
bool        gpu_device_has_create_flag  (gpu_device_create_flags_t f, gpu_device_create_flags_t flag);
gpu_driver_type_t gpu_device_guess_driver_type(u32 pci_vendor_id, const char* vendor_name, const char* adapter_name);

/* Forward decl of the GPU device singleton.  Full struct lives in gpu_device.c
 * and the OpenGL backend extends it.  Currently NULL until OpenGLDevice
 * lands; opengl_texture's create path checks for it. */
typedef struct gpu_device gpu_device_t;
extern gpu_device_t* g_gpu_device;

#endif /* CUPID_UTIL_GPU_DEVICE_H */
