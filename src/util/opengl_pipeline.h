/*
 * OpenGLShader final : GPUShader` and `class OpenGLPipeline final : GPUPipeline`
 * → flat structs that embed the base struct.  VAO/program caches are concrete
 * open-address hash tables in `opengl_device.c` (the only consumer); this
 * header just defines the key + item shapes both sides agree on.
 *
 * `std::optional<GLuint>` for shader id → `GLuint` + a `compile_tried` bool +
 * an `id_valid` bool (compile-fail produces `compile_tried=true, id_valid=false`).
 */

#ifndef CUPID_UTIL_OPENGL_PIPELINE_H
#define CUPID_UTIL_OPENGL_PIPELINE_H

#include "common/types.h"
#include "util/gpu_device.h"
#include "util/gpu_shader_cache.h"

#include "glad/gl.h"

typedef struct Error Error;

typedef struct opengl_shader {
  gpu_shader_t                 base;     /* gpu_shader_t.stage */
  gpu_shader_cache_index_key_t key;      /* MD5 of source + entry-point */
  char*                        source;   /* heap, NUL-terminated */
  size_t                       source_len;
  GLuint                       id;
  bool                         id_valid;
  bool                         compile_tried;
} opengl_shader_t;

/* Allocates + populates struct fields; takes ownership of `source` heap. */
opengl_shader_t* opengl_shader_alloc(gpu_shader_stage_t stage,
                                     const gpu_shader_cache_index_key_t* key,
                                     char* source, size_t source_len);
void             opengl_shader_destroy(opengl_shader_t* sh);

/* Lazy-compile of the shader's source.  glCreateShader, glShaderSource,
 * glCompileShader.  Logs on fail; sets `compile_tried` either way.  Returns
 * true if `id_valid`. */
bool opengl_shader_compile(opengl_shader_t* sh, Error* err);

#define OPENGL_PIPELINE_MAX_VERTEX_ATTRIBUTES 7

typedef struct {
  gpu_vertex_attribute_t vertex_attributes[OPENGL_PIPELINE_MAX_VERTEX_ATTRIBUTES];
  u32                    vertex_attribute_stride;
  u32                    num_vertex_attributes;
} opengl_pipeline_vao_key_t;

typedef struct {
  GLuint vao_id;
  u32    reference_count;
} opengl_pipeline_vao_item_t;

typedef struct {
  u64 vs_hash_low, vs_hash_high;
  u64 gs_hash_low, gs_hash_high;
  u64 fs_hash_low, fs_hash_high;
  u32 vs_length;
  u32 gs_length;
  u32 fs_length;
  opengl_pipeline_vao_key_t va_key;
} opengl_pipeline_program_key_t;

typedef struct {
  GLuint program_id;
  u32    reference_count;
  GLenum file_format;
  u32    file_offset;
  u32    file_compressed_size;
  u32    file_uncompressed_size;
} opengl_pipeline_program_item_t;

ALWAYS_INLINE bool opengl_pipeline_vao_key_eq(const opengl_pipeline_vao_key_t* a,
                                              const opengl_pipeline_vao_key_t* b)
{
  return memcmp(a, b, sizeof(*a)) == 0;
}

ALWAYS_INLINE bool opengl_pipeline_program_key_eq(const opengl_pipeline_program_key_t* a,
                                                  const opengl_pipeline_program_key_t* b)
{
  return memcmp(a, b, sizeof(*a)) == 0;
}

/* FNV-1a over the byte image; no padding so it's stable. */
ALWAYS_INLINE u64 opengl_pipeline_vao_key_hash(const opengl_pipeline_vao_key_t* k)
{
  const u8* b = (const u8*)k;
  u64 h = 1469598103934665603ULL;
  for (u32 i = 0; i < sizeof(*k); ++i) h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}

ALWAYS_INLINE u64 opengl_pipeline_program_key_hash(const opengl_pipeline_program_key_t* k)
{
  const u8* b = (const u8*)k;
  u64 h = 1469598103934665603ULL;
  for (u32 i = 0; i < sizeof(*k); ++i) h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}

/* Builds a program cache key from a graphics config + shader source hashes
 * (caller passes the per-stage CacheIndexKey from gpu_shader_cache_compute_key
 * to populate vs/gs/fs hashes; va_key comes from the input layout). */
opengl_pipeline_program_key_t opengl_pipeline_program_key_from_graphics_config(
  const gpu_pipeline_graphics_config_t* cfg,
  const opengl_shader_t* vs,
  const opengl_shader_t* gs,
  const opengl_shader_t* fs);

typedef struct opengl_pipeline {
  gpu_pipeline_t                base;
  opengl_pipeline_program_key_t key;
  GLuint                        program;       /* NB: pipeline doesn't own; refcounted in cache */
  GLuint                        vao_id;        /* resolved VAO id (cache owns) */
  gpu_blend_state_t             blend_state;
  gpu_rasterization_state_t     rasterization_state;
  gpu_depth_state_t             depth_state;
  GLenum                        topology;
} opengl_pipeline_t;

 opengl_pipeline_t* opengl_pipeline_alloc(const opengl_pipeline_program_key_t* key,
                                          GLuint program, GLuint vao_id,
                                          gpu_rasterization_state_t rs,
                                          gpu_depth_state_t ds,
                                          gpu_blend_state_t bs, 
                                          GLenum topology);
void               opengl_pipeline_destroy(opengl_pipeline_t* p);

ALWAYS_INLINE GLuint                    opengl_pipeline_get_program (const opengl_pipeline_t* p) { return p->program; }
ALWAYS_INLINE GLuint                    opengl_pipeline_get_vao_id  (const opengl_pipeline_t* p) { return p->vao_id; }
ALWAYS_INLINE gpu_rasterization_state_t opengl_pipeline_get_raster  (const opengl_pipeline_t* p) { return p->rasterization_state; }
ALWAYS_INLINE gpu_depth_state_t         opengl_pipeline_get_depth   (const opengl_pipeline_t* p) { return p->depth_state; }
ALWAYS_INLINE gpu_blend_state_t         opengl_pipeline_get_blend   (const opengl_pipeline_t* p) { return p->blend_state; }
ALWAYS_INLINE GLenum                    opengl_pipeline_get_topology(const opengl_pipeline_t* p) { return p->topology; }

#endif /* CUPID_UTIL_OPENGL_PIPELINE_H */
