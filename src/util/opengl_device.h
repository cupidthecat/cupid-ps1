/*
 * OpenGLDevice final : GPUDevice` → folded into the public `gpu_device.h` API.
 * This header provides:
 *   - `opengl_swap_chain_t` (extends `gpu_swap_chain_t` with EGL surface)
 *   - opengl-specific inline accessors (`opengl_device_get_context`,
 *     `opengl_device_is_gles`, `opengl_device_get_texture_stream_buffer`)
 *   - internal pipeline/VAO helpers used across opengl_pipeline.c +
 *     opengl_texture.c + opengl_device.c (cache lookups, framebuffer mgr,
 *     etc.)
 *
 * The full `opengl_device.c` body lives in opengl_device.c.
 */

#ifndef CUPID_UTIL_OPENGL_DEVICE_H
#define CUPID_UTIL_OPENGL_DEVICE_H

#include "common/types.h"
#include "util/gpu_device.h"
#include "util/gpu_framebuffer_manager.h"
#include "util/opengl_context.h"
#include "util/opengl_pipeline.h"
#include "util/opengl_stream_buffer.h"

#include "glad/gl.h"

typedef struct Error Error;

enum {
  OPENGL_DEVICE_NUM_TIMESTAMP_QUERIES   = 3,
  OPENGL_DEVICE_VERTEX_BUFFER_SIZE      = 8 * 1024 * 1024,
  OPENGL_DEVICE_INDEX_BUFFER_SIZE       = 4 * 1024 * 1024,
  OPENGL_DEVICE_UNIFORM_BUFFER_SIZE     = 2 * 1024 * 1024,
  OPENGL_DEVICE_PUSH_CONSTANT_BUFFER_SIZE = 1 * 1024 * 1024,
  OPENGL_DEVICE_TEXTURE_STREAM_BUFFER_SIZE = 16 * 1024 * 1024,
};
#define OPENGL_DEVICE_UPDATE_TEXTURE_UNIT GL_TEXTURE8

typedef struct opengl_swap_chain {
  gpu_swap_chain_t           base;
  opengl_surface_handle_t    surface_handle;   /* EGL surface, NULL = main */
} opengl_swap_chain_t;

opengl_swap_chain_t* opengl_swap_chain_create(const window_info_t* wi, gpu_vsync_mode_t mode,
                                              opengl_surface_handle_t surface_handle);
void                 opengl_swap_chain_destroy(opengl_swap_chain_t* sc);
bool                 opengl_swap_chain_resize_buffers(opengl_swap_chain_t* sc, u32 new_width, u32 new_height,
                                                       Error* err);
bool                 opengl_swap_chain_set_vsync_mode(opengl_swap_chain_t* sc, gpu_vsync_mode_t mode, Error* err);
bool                 opengl_swap_chain_set_swap_interval(opengl_context_t* ctx, gpu_vsync_mode_t mode, Error* err);

opengl_context_t*       opengl_device_get_context(void);
bool                    opengl_device_is_gles(void);
opengl_stream_buffer_t* opengl_device_get_texture_stream_buffer(void);
void                    opengl_device_bind_update_texture_unit(void);
bool                    opengl_device_should_use_pbos_for_downloads(void);
void                    opengl_device_set_error_object(Error* err, const char* prefix, GLenum gle);

#ifndef CUPID_OPENGL_TEXTURE_T_DECLARED
#define CUPID_OPENGL_TEXTURE_T_DECLARED
typedef struct opengl_texture opengl_texture_t;
#endif
struct opengl_sampler;

/* Texture/sampler/pipeline binding bookkeeping called from texture/pipeline
 * destructors so the device can flush stale references. */
void opengl_device_unbind_texture_id(GLuint id);
void opengl_device_unbind_texture   (opengl_texture_t* tex);
void opengl_device_unbind_ssbo      (GLuint id);
void opengl_device_unbind_sampler   (GLuint id);
void opengl_device_unbind_pipeline  (const opengl_pipeline_t* pl);

void opengl_device_set_active_texture(u32 slot);

/* RT clear commit hooks (invoked from texture create/update paths). */
void opengl_device_commit_clear           (opengl_texture_t* tex);
void opengl_device_commit_rt_clear_in_fb  (opengl_texture_t* tex, u32 idx);
void opengl_device_commit_ds_clear_in_fb  (opengl_texture_t* tex);

GLuint opengl_device_lookup_program_cache(const opengl_pipeline_program_key_t* key,
                                          const gpu_pipeline_graphics_config_t* cfg, Error* err);
GLuint opengl_device_compile_program     (const gpu_pipeline_graphics_config_t* cfg, Error* err);
void   opengl_device_post_link_program   (const gpu_pipeline_graphics_config_t* cfg, GLuint program_id);
void   opengl_device_unref_program       (const opengl_pipeline_program_key_t* key);

GLuint opengl_device_lookup_vao_cache    (const opengl_pipeline_vao_key_t* key, Error* err);
GLuint opengl_device_create_vao          (const gpu_vertex_attribute_t* attrs, u32 count, u32 stride, Error* err);
void   opengl_device_unref_vao           (const opengl_pipeline_vao_key_t* key);

void   opengl_device_set_vertex_buffer_offsets(u32 base_vertex);
void   opengl_device_render_blank_frame  (void);

GLuint opengl_device_lookup_or_create_fbo(const gpu_framebuffer_key_t* key);
void   opengl_device_remove_fbo_references(const gpu_texture_t* tex);

bool  opengl_device_create(const window_info_t* wi, gpu_vsync_mode_t vsync,
                           gpu_device_create_flags_t flags, Error* err);
void  opengl_device_destroy(void);

/* Pipeline / shader / texture / sampler create entry points (forwarded to
 * the relevant TU). */
gpu_shader_t*    opengl_device_create_shader_from_source(gpu_shader_stage_t stage,
                                                         gpu_shader_language_t language,
                                                         const char* source, size_t source_len,
                                                         const char* entry_point, Error* err);
gpu_pipeline_t*  opengl_device_create_pipeline(const gpu_pipeline_graphics_config_t* cfg, Error* err);
void             opengl_device_destroy_pipeline(gpu_pipeline_t* p);

/* Set-render-targets / set-pipeline / set-texture-sampler / draw / present.
 * These are the hot path; declared here so the eventual core/gpu_hw.c can
 * call them without going through a vtable. */
void opengl_device_set_render_targets(gpu_texture_t* const* rts, u32 num_rts, gpu_texture_t* ds);
void opengl_device_set_pipeline      (gpu_pipeline_t* p);
void opengl_device_set_texture_sampler(u32 slot, gpu_texture_t* tex, struct opengl_sampler* samp);
struct opengl_texture_buffer;
void opengl_device_set_texture_buffer (u32 slot, struct opengl_texture_buffer* buf);
typedef struct { s32 left, top, right, bottom; } opengl_rect_t;
void opengl_device_set_viewport(opengl_rect_t r);
void opengl_device_set_scissor (opengl_rect_t r);
void opengl_device_draw            (u32 vertex_count, u32 base_vertex);
void opengl_device_draw_indexed    (u32 index_count, u32 base_index, u32 base_vertex);
void opengl_device_draw_with_push_constants(u32 vertex_count, u32 base_vertex,
                                            const void* push_constants, u32 push_constants_size);
void opengl_device_draw_indexed_with_push_constants(u32 index_count, u32 base_index, u32 base_vertex,
                                                    const void* push_constants, u32 push_constants_size);
void opengl_device_clear_render_target(gpu_texture_t* t, u32 color);
void opengl_device_clear_depth        (gpu_texture_t* t, float d);
void opengl_device_invalidate_render_target(gpu_texture_t* t);
void opengl_device_copy_texture_region(gpu_texture_t* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                       gpu_texture_t* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level,
                                       u32 width, u32 height);
void opengl_device_resolve_texture_region(gpu_texture_t* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                          gpu_texture_t* src, u32 src_x, u32 src_y, u32 width, u32 height);
gpu_present_result_t opengl_device_begin_present(gpu_swap_chain_t* sc, u32 clear_color);
void                 opengl_device_end_present  (gpu_swap_chain_t* sc, bool explicit_present, u64 present_time);
void                 opengl_device_submit_present(gpu_swap_chain_t* sc);

/* KHR_debug groups; thin wrappers around glPushDebugGroup /
 * glPopDebugGroup that no-op when the GL context lacks the extension (the
 * function pointers are nulled at device-create time in that case).  Useful
 * for apitrace / RenderDoc traces. */
void opengl_device_push_debug_group(const char* name);
void opengl_device_pop_debug_group (void);

void  opengl_device_map_vertex_buffer  (u32 vertex_size, u32 vertex_count,
                                        void** map_ptr, u32* map_space, u32* map_base_vertex);
void  opengl_device_unmap_vertex_buffer(u32 vertex_size, u32 vertex_count);
void  opengl_device_map_index_buffer   (u32 index_count, u16** map_ptr, u32* map_space, u32* map_base_index);
void  opengl_device_unmap_index_buffer (u32 used_index_count);
void* opengl_device_map_uniform_buffer (u32 size);
void  opengl_device_unmap_uniform_buffer(u32 size);
void  opengl_device_push_uniform_buffer(const void* data, u32 size);

void* opengl_device_program_cache_find_wrapper  (const opengl_pipeline_program_key_t* key);
void* opengl_device_program_cache_insert_wrapper(const opengl_pipeline_program_key_t* key);
void  opengl_device_program_cache_erase_wrapper (void* slot);
opengl_pipeline_program_item_t* opengl_device_program_cache_slot_value(void* slot);

void* opengl_device_vao_cache_find_wrapper  (const opengl_pipeline_vao_key_t* key);
void* opengl_device_vao_cache_insert_wrapper(const opengl_pipeline_vao_key_t* key);
void  opengl_device_vao_cache_erase_wrapper (void* slot);
opengl_pipeline_vao_item_t* opengl_device_vao_cache_slot_value(void* slot);

void   opengl_device_set_last_program(GLuint p);
GLuint opengl_device_get_last_program(void);

const opengl_pipeline_vao_key_t* opengl_device_get_last_vao_key(void);
GLuint opengl_device_get_last_vao_id(void);
void   opengl_device_set_last_vao(const opengl_pipeline_vao_key_t* key, GLuint vao_id);

opengl_pipeline_t**     opengl_device_current_pipeline_slot(void);
opengl_stream_buffer_t* opengl_device_get_vertex_buffer(void);
opengl_stream_buffer_t* opengl_device_get_index_buffer (void);
GLuint                  opengl_device_get_uniform_buffer_alignment(void);

gpu_blend_state_t*         opengl_device_last_blend_state_slot(void);
gpu_rasterization_state_t* opengl_device_last_rasterization_state_slot(void);
gpu_depth_state_t*         opengl_device_last_depth_state_slot(void);

/* Resets every GL-state cache the device tracks (blend / rasterization /
 * depth / viewport / scissor / pipeline / VAO / program) to a sentinel that
 * forces the next set_* call to issue the GL command.  Use after any code
 * path that bypasses the device API to manipulate GL directly (for example
 * the SDL frontend's gl_present_frame, which raw-binds a VAO + program for
 * the fullscreen blit). */
void opengl_device_invalidate_state_cache(void);

bool                          opengl_device_get_pipeline_disk_cache_active(void);
const gpu_device_features_t*  opengl_device_get_features(void);

/* Internal FBO/feature accessors used by opengl_texture.c. */
GLuint opengl_device_get_read_fbo(void);
GLuint opengl_device_get_write_fbo(void);
GLuint opengl_device_get_current_fbo(void);
bool   opengl_device_use_get_texture_sub_image(void);
u32    opengl_device_get_max_texture_size(void);
u16    opengl_device_get_max_multisamples(void);

/* Live in opengl_pipeline.c; called from opengl_device.c via the create/destroy
 * pipeline path. */
gpu_pipeline_t* opengl_pipeline_create_from_graphics_config(const gpu_pipeline_graphics_config_t* cfg, Error* err);
gpu_pipeline_t* opengl_pipeline_create_from_compute_config (const gpu_pipeline_compute_config_t*  cfg, Error* err);
void            opengl_pipeline_destroy_from_device        (gpu_pipeline_t* p);

/* State-apply (live in opengl_pipeline.c, called from device's SetPipeline +
 * opengl_device's blank-frame/clear paths via the state-getter slots). */
void opengl_device_apply_rasterization_state(gpu_rasterization_state_t rs);
void opengl_device_apply_depth_state        (gpu_depth_state_t ds);
void opengl_device_apply_blend_state        (gpu_blend_state_t bs);

#endif /* CUPID_UTIL_OPENGL_DEVICE_H */
