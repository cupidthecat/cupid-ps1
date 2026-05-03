/*
 * the OpenGLShader/OpenGLPipeline ctor/dtor + Compile + GetProgramCacheKey.
 * The OpenGLDevice methods (CreateShaderFromBinary/Source, LookupProgramCache,
 * CompileProgram, PostLinkProgram, LookupVAOCache, CreateVAO,
 * SetVertexBufferOffsets, ApplyRasterizationState, ApplyDepthState,
 * ApplyBlendState, SetPipeline, OpenPipelineCache, CreatePipelineCache,
 * CreateProgramFromPipelineCache, AddToPipelineCache, DiscardPipelineCache,
 * ClosePipelineCache) live in opengl_device.c (next batch).
 */

#include "opengl_pipeline.h"
#include "opengl_device.h"
#include "opengl_stream_buffer.h"
#include "shadergen.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUDevice);

static GLenum gl_shader_type(gpu_shader_stage_t s)
{
  switch (s)
  {
    case GPU_SHADER_STAGE_VERTEX:   return GL_VERTEX_SHADER;
    case GPU_SHADER_STAGE_FRAGMENT: return GL_FRAGMENT_SHADER;
    case GPU_SHADER_STAGE_GEOMETRY: return GL_GEOMETRY_SHADER;
    case GPU_SHADER_STAGE_COMPUTE:  return GL_COMPUTE_SHADER;
    default:                        return 0;
  }
}

opengl_shader_t* opengl_shader_alloc(gpu_shader_stage_t stage,
                                     const gpu_shader_cache_index_key_t* key,
                                     char* source, size_t source_len)
{
  opengl_shader_t* sh = (opengl_shader_t*)calloc(1, sizeof(*sh));
  if (!sh) return NULL;
  sh->base.stage   = stage;
  sh->key          = *key;
  sh->source       = source;
  sh->source_len   = source_len;
  sh->id           = 0;
  sh->id_valid     = false;
  sh->compile_tried = false;
  return sh;
}

void opengl_shader_destroy(opengl_shader_t* sh)
{
  if (!sh) return;
  if (sh->id_valid) glDeleteShader(sh->id);
  free(sh->source);
  free(sh);
}

bool opengl_shader_compile(opengl_shader_t* sh, Error* err)
{
  if (sh->compile_tried)
  {
    if (!sh->id_valid) { Error_set_string(err, "Shader previously failed to compile."); return false; }
    return true;
  }
  sh->compile_tried = true;

  glGetError();
  GLuint shader = glCreateShader(gl_shader_type(sh->base.stage));
  GLenum gle = glGetError();
  if (gle != GL_NO_ERROR)
  {
    ERROR_LOG("glCreateShader() failed: 0x%X", (unsigned)gle);
    Error_set_string_fmt(err, "glCreateShader() failed: 0x%X", (unsigned)gle);
    return false;
  }

  const GLchar* source_ptr = sh->source;
  const GLint   source_len = (GLint)sh->source_len;
  glShaderSource(shader, 1, &source_ptr, &source_len);
  glCompileShader(shader);

  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  GLint info_log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    char* info_log = (char*)malloc((size_t)info_log_length + 1);
    if (info_log)
    {
      glGetShaderInfoLog(shader, info_log_length, &info_log_length, info_log);
      info_log[info_log_length] = '\0';
      if (status == GL_TRUE)
      {
        ERROR_LOG("Shader compiled with warnings:\n%s", info_log);
      }
      else
      {
        ERROR_LOG("Shader failed to compile:\n%s", info_log);
        Error_set_string_fmt(err, "Shader failed to compile:\n%s", info_log);
        free(info_log);
        glDeleteShader(shader);
        return false;
      }
      free(info_log);
    }
    else if (status == GL_FALSE)
    {
      Error_set_string(err, "Shader failed to compile (info log alloc failed).");
      glDeleteShader(shader);
      return false;
    }
  }

  sh->id = shader;
  sh->id_valid = true;
  return true;
}

 opengl_pipeline_t* opengl_pipeline_alloc(const opengl_pipeline_program_key_t* key,
                                          GLuint program, GLuint vao_id,
                                          gpu_rasterization_state_t rs,
                                          gpu_depth_state_t ds,
                                          gpu_blend_state_t bs,
                                          GLenum topology) 
{
  opengl_pipeline_t* p = (opengl_pipeline_t*)calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->key                 = *key;
  p->program             = program;
  p->vao_id              = vao_id;
  p->rasterization_state = rs;
  p->depth_state         = ds;
  p->blend_state         = bs;
  p->topology            = topology;
  return p;
}

void opengl_pipeline_destroy(opengl_pipeline_t* p)
{
  /* program/vao are refcounted in opengl_device caches; the pipeline itself
   * doesn't own them.  opengl_device's pipeline destructor handles the unref
   * (UnrefProgram/UnrefVAO).  Just free the wrapper here. */
  free(p);
}

opengl_pipeline_program_key_t opengl_pipeline_program_key_from_graphics_config(
  const gpu_pipeline_graphics_config_t* cfg,
  const opengl_shader_t* vs,
  const opengl_shader_t* gs,
  const opengl_shader_t* fs)
{
  Assert(cfg->input_layout.vertex_attribute_count <= OPENGL_PIPELINE_MAX_VERTEX_ATTRIBUTES);

  opengl_pipeline_program_key_t k = {0};
  k.vs_hash_low  = vs ? vs->key.source_hash_low  : 0;
  k.vs_hash_high = vs ? vs->key.source_hash_high : 0;
  k.vs_length    = vs ? vs->key.source_length    : 0;
  k.fs_hash_low  = fs ? fs->key.source_hash_low  : 0;
  k.fs_hash_high = fs ? fs->key.source_hash_high : 0;
  k.fs_length    = fs ? fs->key.source_length    : 0;
  k.gs_hash_low  = gs ? gs->key.source_hash_low  : 0;
  k.gs_hash_high = gs ? gs->key.source_hash_high : 0;
  k.gs_length    = gs ? gs->key.source_length    : 0;

  k.va_key.num_vertex_attributes   = cfg->input_layout.vertex_attribute_count;
  k.va_key.vertex_attribute_stride = 0;
  if (k.va_key.num_vertex_attributes > 0)
  {
    memcpy(k.va_key.vertex_attributes, cfg->input_layout.vertex_attributes,
           sizeof(gpu_vertex_attribute_t) * k.va_key.num_vertex_attributes);
    k.va_key.vertex_attribute_stride = cfg->input_layout.vertex_stride;
  }
  return k;
}

typedef struct { GLenum type; GLboolean normalized; GLboolean integer; } va_mapping_t;
static const va_mapping_t s_vao_format_mapping[GPU_VERTEX_TYPE_MAX_COUNT] = {
  { GL_FLOAT,          GL_FALSE, GL_FALSE }, /* Float */
  { GL_UNSIGNED_BYTE,  GL_FALSE, GL_TRUE  }, /* UInt8 */
  { GL_BYTE,           GL_FALSE, GL_TRUE  }, /* SInt8 */
  { GL_UNSIGNED_BYTE,  GL_TRUE,  GL_FALSE }, /* UNorm8 */
  { GL_UNSIGNED_SHORT, GL_FALSE, GL_TRUE  }, /* UInt16 */
  { GL_SHORT,          GL_FALSE, GL_TRUE  }, /* SInt16 */
  { GL_UNSIGNED_SHORT, GL_TRUE,  GL_FALSE }, /* UNorm16 */
  { GL_UNSIGNED_INT,   GL_FALSE, GL_TRUE  }, /* UInt32 */
  { GL_INT,            GL_FALSE, GL_TRUE  }, /* SInt32 */
};

static GLuint opengl_device_compile_program_internal(const gpu_pipeline_graphics_config_t* cfg, Error* err);

GLuint opengl_device_lookup_program_cache(const opengl_pipeline_program_key_t* key,
                                          const gpu_pipeline_graphics_config_t* cfg, Error* err)
{
  void* slot = opengl_device_program_cache_find_wrapper(key);
  opengl_pipeline_program_item_t* it = opengl_device_program_cache_slot_value(slot);

 /* Disk cache disabled in cupid-ps1; the file-backed re-load path is never
   * taken. 
  if (it && it->program_id == 0 && it->file_uncompressed_size > 0)
  {
    opengl_device_program_cache_erase_wrapper(slot);
    slot = NULL; it = NULL;
  } */

  if (it)
  {
    if (it->program_id != 0) it->reference_count++;
    return it->program_id;
  }

  const GLuint program_id = opengl_device_compile_program_internal(cfg, err);
  if (program_id == 0) return 0;

  void* ins_slot = opengl_device_program_cache_insert_wrapper(key);
  opengl_pipeline_program_item_t* ins = opengl_device_program_cache_slot_value(ins_slot);
  ins->program_id = program_id;
  ins->reference_count = 1;
  ins->file_format = 0;
  ins->file_offset = 0;
  ins->file_uncompressed_size = 0;
  ins->file_compressed_size = 0;
  return program_id;
}

GLuint opengl_device_compile_program(const gpu_pipeline_graphics_config_t* cfg, Error* err)
{
  return opengl_device_compile_program_internal(cfg, err);
}

static GLuint opengl_device_compile_program_internal(const gpu_pipeline_graphics_config_t* cfg, Error* err)
{
  opengl_shader_t* vs = (opengl_shader_t*)cfg->vertex_shader;
  opengl_shader_t* fs = (opengl_shader_t*)cfg->fragment_shader;
  opengl_shader_t* gs = (opengl_shader_t*)cfg->geometry_shader;
  if (!vs || !fs || !opengl_shader_compile(vs, err) || !opengl_shader_compile(fs, err) ||
      (gs && !opengl_shader_compile(gs, err)))
    return 0;

  glGetError();
  const GLuint program_id = glCreateProgram();
  GLenum gle = glGetError();
  if (gle != GL_NO_ERROR)
  {
    opengl_device_set_error_object(err, "glCreateProgram() failed: ", gle);
    return 0;
  }

  /* Disk cache off; skip GL_PROGRAM_BINARY_RETRIEVABLE_HINT. */

  Assert(vs && fs);
  glAttachShader(program_id, vs->id);
  glAttachShader(program_id, fs->id);
  if (gs) glAttachShader(program_id, gs->id);

  if (!shadergen_use_glsl_binding_layout())
  {
    static const char* const semantic_vars[GPU_VERTEX_SEMANTIC_MAX_COUNT] = {
      "a_pos", "a_tex", "a_col",
    };

    for (u32 i = 0; i < cfg->input_layout.vertex_attribute_count; ++i)
    {
      const gpu_vertex_attribute_t va = cfg->input_layout.vertex_attributes[i];
      if (va.bits.semantic == GPU_VERTEX_SEMANTIC_POSITION && va.bits.semantic_index == 0)
      {
        glBindAttribLocation(program_id, i, "a_pos");
      }
      else
      {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%u",
                 semantic_vars[va.bits.semantic], (unsigned)va.bits.semantic_index);
        glBindAttribLocation(program_id, i, buf);
      }
    }

    const bool is_gles = opengl_device_is_gles();
    if (!is_gles)
      glBindFragDataLocation(program_id, 0, "o_col0");

    if (opengl_device_get_features()->dual_source_blend)
    {
      if (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended)
      {
        if (is_gles)
          glBindFragDataLocationIndexed(program_id, 0, 0, "o_col0");
        glBindFragDataLocationIndexed(program_id, 1, 0, "o_col1");
      }
      else if (GLAD_GL_EXT_blend_func_extended)
      {
        if (is_gles)
          glBindFragDataLocationIndexedEXT(program_id, 0, 0, "o_col1");
        glBindFragDataLocationIndexedEXT(program_id, 1, 0, "o_col1");
      }
    }
  }

  glLinkProgram(program_id);

  GLint status = GL_FALSE;
  glGetProgramiv(program_id, GL_LINK_STATUS, &status);

  GLint info_log_length = 0;
  glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    char* info_log = (char*)malloc((size_t)info_log_length + 1);
    if (info_log)
    {
      glGetProgramInfoLog(program_id, info_log_length, &info_log_length, info_log);
      info_log[info_log_length] = '\0';
      if (status == GL_TRUE)
      {
        ERROR_LOG("Program linked with warnings:\n%s", info_log);
      }
      else
      {
        ERROR_LOG("Program failed to link:\n%s", info_log);
        Error_set_string_fmt(err, "Program failed to link:\n%s", info_log);
        free(info_log);
        glDeleteProgram(program_id);
        return 0;
      }
      free(info_log);
    }
    else if (status == GL_FALSE)
    {
      Error_set_string(err, "Program failed to link (info log alloc failed).");
      glDeleteProgram(program_id);
      return 0;
    }
  }

  opengl_device_post_link_program(cfg, program_id);
  return program_id;
}

void opengl_device_post_link_program(const gpu_pipeline_graphics_config_t* cfg, GLuint program_id)
{
  if (shadergen_use_glsl_binding_layout()) return;

  const GLint ubo_location = glGetUniformBlockIndex(program_id, "UBOBlock");
  if (ubo_location >= 0)
    glUniformBlockBinding(program_id, ubo_location, 0);
  const GLint push_constant_location = glGetUniformBlockIndex(program_id, "PushConstants");
  if (push_constant_location >= 0)
    glUniformBlockBinding(program_id, push_constant_location, 1);

  glUseProgram(program_id);

  u32 num_textures = gpu_device_active_textures_for_layout(cfg->layout);
  if (num_textures < 1) num_textures = 1;
  for (u32 i = 0; i < num_textures; ++i)
  {
    char buf[16]; snprintf(buf, sizeof(buf), "samp%u", i);
    const GLint samp_loc = glGetUniformLocation(program_id, buf);
    if (samp_loc >= 0)
      glUniform1i(samp_loc, (GLint)i);
  }

  glUseProgram(opengl_device_get_last_program());
}

void opengl_device_unref_program(const opengl_pipeline_program_key_t* key)
{
  void* slot = opengl_device_program_cache_find_wrapper(key);
  opengl_pipeline_program_item_t* it = opengl_device_program_cache_slot_value(slot);
  Assert(it && it->program_id != 0 && it->reference_count > 0);

  if ((--it->reference_count) > 0) return;

  if (opengl_device_get_last_program() == it->program_id)
  {
    opengl_device_set_last_program(0);
    glUseProgram(0);
  }

  glDeleteProgram(it->program_id);
  it->program_id = 0;

  /* No disk cache, so always remove on full unref. */
  if (it->file_uncompressed_size == 0)
    opengl_device_program_cache_erase_wrapper(slot);
}

GLuint opengl_device_create_vao(const gpu_vertex_attribute_t* attrs, u32 count, u32 stride, Error* err)
{
  glGetError();
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  GLenum gle = glGetError();
  if (gle != GL_NO_ERROR)
  {
    ERROR_LOG("Failed to create vertex array object: 0x%X", gle);
    opengl_device_set_error_object(err, "glGenVertexArrays() failed: ", gle);
    return 0;
  }

  glBindVertexArray(vao);
  opengl_stream_buffer_bind(opengl_device_get_vertex_buffer());
  opengl_stream_buffer_bind(opengl_device_get_index_buffer());

  for (u32 i = 0; i < count; ++i)
  {
    const gpu_vertex_attribute_t va = attrs[i];
    const va_mapping_t m = s_vao_format_mapping[va.bits.type];
    const void* ptr = (const void*)((uintptr_t)va.bits.offset);
    glEnableVertexAttribArray(i);
    if (m.integer)
      glVertexAttribIPointer(i, va.bits.components, m.type, stride, ptr);
    else
      glVertexAttribPointer(i, va.bits.components, m.type, m.normalized, stride, ptr);
  }

  /* Restore previously-bound VAO if any. */
  GLuint last_vao_id = opengl_device_get_last_vao_id();
  if (opengl_device_get_last_vao_key() != NULL)
    glBindVertexArray(last_vao_id);

  return vao;
}

GLuint opengl_device_lookup_vao_cache(const opengl_pipeline_vao_key_t* key, Error* err)
{
  void* slot = opengl_device_vao_cache_find_wrapper(key);
  opengl_pipeline_vao_item_t* it = opengl_device_vao_cache_slot_value(slot);
  if (it)
  {
    it->reference_count++;
    return it->vao_id;
  }

  const GLuint vao_id = opengl_device_create_vao(key->vertex_attributes, key->num_vertex_attributes,
                                                  key->vertex_attribute_stride, err);
  if (vao_id == 0) return 0;

  void* ins_slot = opengl_device_vao_cache_insert_wrapper(key);
  opengl_pipeline_vao_item_t* ins = opengl_device_vao_cache_slot_value(ins_slot);
  ins->vao_id = vao_id;
  ins->reference_count = 1;
  return vao_id;
}

void opengl_device_set_vertex_buffer_offsets(u32 base_vertex)
{
  const opengl_pipeline_vao_key_t* va = opengl_device_get_last_vao_key();
  if (!va) return;
  const u32 stride = va->vertex_attribute_stride;
  const u32 base_vertex_start = base_vertex * stride;
  opengl_stream_buffer_t* vb = opengl_device_get_vertex_buffer();

  if (glBindVertexBuffer)
  {
    for (u32 i = 0; i < va->num_vertex_attributes; ++i)
    {
      glBindVertexBuffer(i, opengl_stream_buffer_get_id(vb),
                         base_vertex_start + va->vertex_attributes[i].bits.offset,
                         (GLsizei)stride);
    }
  }
  else
  {
    for (u32 i = 0; i < va->num_vertex_attributes; ++i)
    {
      const gpu_vertex_attribute_t attrib = va->vertex_attributes[i];
      const void* ptr = (const void*)(uintptr_t)(base_vertex_start + attrib.bits.offset);
      const va_mapping_t m = s_vao_format_mapping[attrib.bits.type];
      if (m.integer)
        glVertexAttribIPointer(i, attrib.bits.components, m.type, stride, ptr);
      else
        glVertexAttribPointer(i, attrib.bits.components, m.type, m.normalized, stride, ptr);
    }
  }
}

void opengl_device_unref_vao(const opengl_pipeline_vao_key_t* key)
{
  void* slot = opengl_device_vao_cache_find_wrapper(key);
  opengl_pipeline_vao_item_t* it = opengl_device_vao_cache_slot_value(slot);
  Assert(it && it->reference_count > 0);

  if ((--it->reference_count) > 0) return;

  /* If this is the currently-bound VAO, unbind. */
  const opengl_pipeline_vao_key_t* last = opengl_device_get_last_vao_key();
  if (last && opengl_pipeline_vao_key_eq(last, key))
  {
    opengl_device_set_last_vao(NULL, 0);
    glBindVertexArray(0);
  }

  glDeleteVertexArrays(1, &it->vao_id);
  opengl_device_vao_cache_erase_wrapper(slot);
}

void opengl_device_apply_rasterization_state(gpu_rasterization_state_t rs)
{
  gpu_rasterization_state_t* last = opengl_device_last_rasterization_state_slot();
  if (last->key == rs.key) return;

  if (rs.bits.cull_mode == GPU_CULL_MODE_NONE)
  {
    glDisable(GL_CULL_FACE);
  }
  else
  {
    glEnable(GL_CULL_FACE);
    glCullFace((rs.bits.cull_mode == GPU_CULL_MODE_FRONT) ? GL_FRONT : GL_BACK);
  }

  *last = rs;
}

void opengl_device_apply_depth_state(gpu_depth_state_t ds)
{
  static const GLenum func_mapping[GPU_DEPTH_FUNC_MAX_COUNT] = {
    GL_NEVER, GL_ALWAYS, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL, GL_EQUAL,
  };

  gpu_depth_state_t* last = opengl_device_last_depth_state_slot();
  if (last->key == ds.key) return;

  ((ds.bits.depth_test != GPU_DEPTH_FUNC_ALWAYS) || ds.bits.depth_write) ? glEnable(GL_DEPTH_TEST) :
                                                                            glDisable(GL_DEPTH_TEST);
  glDepthFunc(func_mapping[ds.bits.depth_test]);
  if (last->bits.depth_write != ds.bits.depth_write)
    glDepthMask(ds.bits.depth_write ? GL_TRUE : GL_FALSE);

  *last = ds;
}

void opengl_device_apply_blend_state(gpu_blend_state_t bs)
{
  static const GLenum blend_mapping[GPU_BLEND_FUNC_MAX_COUNT] = {
    GL_ZERO, GL_ONE,
    GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
    GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
    GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
  };
  static const GLenum op_mapping[GPU_BLEND_OP_MAX_COUNT] = {
    GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX,
  };

  gpu_blend_state_t* last = opengl_device_last_blend_state_slot();
  if (bs.key == last->key) return;

  if (bs.bits.enable != last->bits.enable)
    bs.bits.enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);

  if (bs.bits.enable)
  {
    /* check blend factors */
    const u32 new_factors  = (u32)bs.bits.src_blend | ((u32)bs.bits.dst_blend << 4) |
                             ((u32)bs.bits.src_alpha_blend << 8) | ((u32)bs.bits.dst_alpha_blend << 12);
    const u32 last_factors = (u32)last->bits.src_blend | ((u32)last->bits.dst_blend << 4) |
                             ((u32)last->bits.src_alpha_blend << 8) | ((u32)last->bits.dst_alpha_blend << 12);
    if (new_factors != last_factors)
    {
      glBlendFuncSeparate(blend_mapping[bs.bits.src_blend],       blend_mapping[bs.bits.dst_blend],
                          blend_mapping[bs.bits.src_alpha_blend], blend_mapping[bs.bits.dst_alpha_blend]);
    }

    const u32 new_ops  = (u32)bs.bits.blend_op | ((u32)bs.bits.alpha_blend_op << 4);
    const u32 last_ops = (u32)last->bits.blend_op | ((u32)last->bits.alpha_blend_op << 4);
    if (new_ops != last_ops)
    {
      glBlendEquationSeparate(op_mapping[bs.bits.blend_op], op_mapping[bs.bits.alpha_blend_op]);
    }

    if (bs.bits.constant != last->bits.constant)
    {
      const float r = ((bs.bits.constant >> 0)  & 0xFFu) / 255.0f;
      const float g = ((bs.bits.constant >> 8)  & 0xFFu) / 255.0f;
      const float b = ((bs.bits.constant >> 16) & 0xFFu) / 255.0f;
      const float a = ((bs.bits.constant >> 24) & 0xFFu) / 255.0f;
      glBlendColor(r, g, b, a);
    }
  }
  else
  {
    /* keep old factor/op fields to potentially avoid calls when re-enabling */
    bs.bits.src_blend       = last->bits.src_blend;
    bs.bits.dst_blend       = last->bits.dst_blend;
    bs.bits.src_alpha_blend = last->bits.src_alpha_blend;
    bs.bits.dst_alpha_blend = last->bits.dst_alpha_blend;
    bs.bits.blend_op        = last->bits.blend_op;
    bs.bits.alpha_blend_op  = last->bits.alpha_blend_op;
    bs.bits.constant        = last->bits.constant;
  }

  const u32 new_mask  = (u32)bs.bits.write_r | ((u32)bs.bits.write_g << 1) |
                        ((u32)bs.bits.write_b << 2) | ((u32)bs.bits.write_a << 3);
  const u32 last_mask = (u32)last->bits.write_r | ((u32)last->bits.write_g << 1) |
                        ((u32)last->bits.write_b << 2) | ((u32)last->bits.write_a << 3);
  if (new_mask != last_mask)
    glColorMask(bs.bits.write_r, bs.bits.write_g, bs.bits.write_b, bs.bits.write_a);

  *last = bs;
}

void opengl_device_set_pipeline(gpu_pipeline_t* pipeline)
{
  opengl_pipeline_t** cur_slot = opengl_device_current_pipeline_slot();
  if (*cur_slot == (opengl_pipeline_t*)pipeline) return;
  opengl_pipeline_t* P = (opengl_pipeline_t*)pipeline;
  *cur_slot = P;
  if (!P) return;

  opengl_device_apply_rasterization_state(P->rasterization_state);
  opengl_device_apply_depth_state        (P->depth_state);
  opengl_device_apply_blend_state        (P->blend_state);

  const opengl_pipeline_vao_key_t* last_va = opengl_device_get_last_vao_key();
  const bool same_va = (last_va && opengl_pipeline_vao_key_eq(last_va, &P->key.va_key) &&
                        opengl_device_get_last_vao_id() == P->vao_id);
  if (!same_va)
  {
    opengl_device_set_last_vao(&P->key.va_key, P->vao_id);
    glBindVertexArray(P->vao_id);
  }

  if (opengl_device_get_last_program() != P->program)
  {
    opengl_device_set_last_program(P->program);
    glUseProgram(P->program);
  }
}

gpu_pipeline_t* opengl_pipeline_create_from_graphics_config(const gpu_pipeline_graphics_config_t* cfg, Error* err)
{
  const opengl_pipeline_program_key_t pkey =
    opengl_pipeline_program_key_from_graphics_config(cfg,
        (const opengl_shader_t*)cfg->vertex_shader,
        (const opengl_shader_t*)cfg->geometry_shader, 
        (const opengl_shader_t*)cfg->fragment_shader);

  const GLuint program_id = opengl_device_lookup_program_cache(&pkey, cfg, err);
  if (program_id == 0) return NULL;

  const GLuint vao_id = opengl_device_lookup_vao_cache(&pkey.va_key, err);
  if (vao_id == 0)
  {
    opengl_device_unref_program(&pkey);
    return NULL;
  }

  static const GLenum primitives[GPU_PIPELINE_PRIMITIVE_MAX_COUNT] = {
    GL_POINTS, GL_LINES, GL_TRIANGLES, GL_TRIANGLE_STRIP,
  };

   opengl_pipeline_t* p = opengl_pipeline_alloc(&pkey, program_id, vao_id,
                                               cfg->rasterization, cfg->depth, cfg->blend, 
                                               primitives[cfg->primitive]);
  if (!p)
  {
    opengl_device_unref_vao(&pkey.va_key);
    opengl_device_unref_program(&pkey);
    Error_set_string(err, "Out of memory for pipeline.");
    return NULL;
  }
  return &p->base;
}

/* The compute-pipeline overload is unsupported (no compute consumer in cupid-ps1). */
gpu_pipeline_t* opengl_pipeline_create_from_compute_config(const gpu_pipeline_compute_config_t* cfg, Error* err)
{
  (void)cfg;
  Error_set_string(err, "Compute shaders are not yet supported.");
  return NULL;
}

void opengl_pipeline_destroy_from_device(gpu_pipeline_t* p)
{
  if (!p) return;
  opengl_pipeline_t* op = (opengl_pipeline_t*)p;
  opengl_device_unbind_pipeline(op);
  opengl_device_unref_program(&op->key);
  opengl_device_unref_vao(&op->key.va_key);
  opengl_pipeline_destroy(op);
}

#if 0  /* enable when a disk cache consumer lands */
static bool opengl_device_open_pipeline_cache(const char* path, Error* err) { (void)path; (void)err; return false; }
static bool opengl_device_create_pipeline_cache(const char* path, Error* err) { (void)path; (void)err; return false; }
static GLuint opengl_device_create_program_from_pipeline_cache(const opengl_pipeline_program_item_t* it,
                                                                const gpu_pipeline_graphics_config_t* cfg)
{ (void)it; (void)cfg; return 0; }
static void opengl_device_add_to_pipeline_cache(opengl_pipeline_program_item_t* it) { (void)it; }
static bool opengl_device_discard_pipeline_cache(void) { return false; }
static bool opengl_device_close_pipeline_cache(const char* filename, Error* err) { (void)filename; (void)err; return false; }
#endif

/* The hash-table find/insert/erase routines are defined in opengl_device.c
 * (which owns the cache storage) and exposed via the opengl_device_*_cache_*
 * helpers above. */
