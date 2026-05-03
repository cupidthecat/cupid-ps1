/*
 * `class OpenGLDevice final : GPUDevice` -> file-static globals + free
 * functions matching opengl_device.h.  The OpenGLPipeline-bound device
 * methods (LookupProgramCache, CompileProgram, PostLinkProgram, UnrefProgram,
 * LookupVAOCache, CreateVAO, SetVertexBufferOffsets, UnrefVAO,
 * ApplyRasterizationState/DepthState/BlendState, SetPipeline, the disk
 * pipeline cache stubs, and CreatePipeline x2) live in opengl_pipeline.c so
 * the OpenGLPipeline ctor/dtor + caches/state helpers stay together.
 *
 * Disk pipeline cache, GPU timing queries, ImGui debug groups, exclusive
 * fullscreen, compute dispatch - stubbed.
 */

#include "opengl_device.h"
#include "opengl_pipeline.h"
#include "opengl_stream_buffer.h"
#include "opengl_texture.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUDevice);

static void GLAPIENTRY opengl_debug_message_callback(GLenum source, GLenum type, GLuint id,
                                                     GLenum severity, GLsizei length,
                                                     const GLchar* message, const void* user)
{
  (void)source; (void)id; (void)length; (void)user;
  /* Errors / undefined behaviour are loud; perf warnings + portability + the
   * "deprecated" stream get DEV log priority so they don't drown the console
   * unless CUPID_TRACE=1. */
  if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH)
    ERROR_LOG("[GL] src=0x%04x type=0x%04x id=%u sev=0x%04x: %.*s",
              source, type, id, severity, length, message);
  else if (severity == GL_DEBUG_SEVERITY_MEDIUM)
    WARNING_LOG("[GL] type=0x%04x: %s", type, message);
  else
    DEV_LOG("[GL] type=0x%04x severity=0x%04x: %s", type, severity, message);
}

__attribute__((weak)) void opengl_texture_commit_clear(opengl_texture_t* tex) { (void)tex; }

#define OPENGL_DEVICE_HASH_INITIAL_CAP 64u

typedef struct {
  bool                            occupied;
  bool                            tombstone;
  opengl_pipeline_program_key_t   key;
  opengl_pipeline_program_item_t  value;
} program_cache_entry_t;

typedef struct {
  bool                            occupied;
  bool                            tombstone;
  opengl_pipeline_vao_key_t       key;
  opengl_pipeline_vao_item_t      value;
} vao_cache_entry_t;

typedef struct {
  bool                            occupied;
  bool                            tombstone;
  gpu_framebuffer_key_t           key;
  GLuint                          fbo;
} fbo_cache_entry_t;

typedef struct {
  program_cache_entry_t* slots;
  u32                    cap;
  u32                    count;
} program_cache_t;

typedef struct {
  vao_cache_entry_t* slots;
  u32                cap;
  u32                count;
} vao_cache_t;

typedef struct {
  fbo_cache_entry_t* slots;
  u32                cap;
  u32                count;
} fbo_cache_t;

struct gpu_device {
  /* GL context + main swap chain */
  opengl_context_t*    gl_context;
  opengl_swap_chain_t* main_swap_chain;

  gpu_render_api_t     render_api;
  u32                  render_api_version;
  gpu_driver_type_t    driver_type;

  /* Streaming buffers */
  opengl_stream_buffer_t* vertex_buffer;
  opengl_stream_buffer_t* index_buffer;
  opengl_stream_buffer_t* uniform_buffer;
  opengl_stream_buffer_t* push_constant_buffer;
  opengl_stream_buffer_t* texture_stream_buffer;

  /* Caches */
  program_cache_t program_cache;
  vao_cache_t     vao_cache;
  fbo_cache_t     fbo_cache;

  /* Cached state (GL pipeline) */
  opengl_pipeline_vao_key_t  last_vao_key;
  bool                       last_vao_valid;
  GLuint                     last_vao_id;
  GLuint                     uniform_buffer_alignment;
  GLuint                     last_program;
  u32                        last_texture_unit;
  GLuint                     last_sampler_tex[GPU_DEVICE_MAX_TEXTURE_SAMPLERS];
  GLuint                     last_sampler_smp[GPU_DEVICE_MAX_TEXTURE_SAMPLERS];
  GLuint                     last_ssbo;

  opengl_rect_t              last_viewport;
  opengl_rect_t              last_scissor;

  GLuint                     read_fbo;
  GLuint                     write_fbo;

  GLuint                     current_fbo;
  u32                        num_current_render_targets;
  opengl_texture_t*          current_render_targets[GPU_DEVICE_MAX_RENDER_TARGETS];
  opengl_texture_t*          current_depth_target;
  opengl_pipeline_t*         current_pipeline;

  gpu_blend_state_t          last_blend_state;
  gpu_rasterization_state_t  last_rasterization_state;
  gpu_depth_state_t          last_depth_state;

  gpu_device_features_t      features;
  u32                        max_texture_size;
  u16                        max_multisamples;

  /* Capability flags */
  bool debug_device;
  bool disable_pbo;
  bool disable_async_download;
  bool use_get_texture_sub_image;
};

static struct gpu_device s_device;

static bool  opengl_device_check_features(gpu_device_create_flags_t flags);
static bool  opengl_device_create_buffers(void);
static void  opengl_device_destroy_buffers(void);

static void  program_cache_init    (program_cache_t* c);
static void  program_cache_destroy (program_cache_t* c);
static program_cache_entry_t* program_cache_find (program_cache_t* c, const opengl_pipeline_program_key_t* key);
static program_cache_entry_t* program_cache_insert(program_cache_t* c, const opengl_pipeline_program_key_t* key);
static void  program_cache_erase   (program_cache_t* c, program_cache_entry_t* slot);

static void  vao_cache_init    (vao_cache_t* c);
static void  vao_cache_destroy (vao_cache_t* c);
static vao_cache_entry_t* vao_cache_find (vao_cache_t* c, const opengl_pipeline_vao_key_t* key);
static vao_cache_entry_t* vao_cache_insert(vao_cache_t* c, const opengl_pipeline_vao_key_t* key);
static void  vao_cache_erase   (vao_cache_t* c, vao_cache_entry_t* slot);

static void  fbo_cache_init    (fbo_cache_t* c);
static void  fbo_cache_destroy (fbo_cache_t* c);
static fbo_cache_entry_t* fbo_cache_find (fbo_cache_t* c, const gpu_framebuffer_key_t* key);
static fbo_cache_entry_t* fbo_cache_insert(fbo_cache_t* c, const gpu_framebuffer_key_t* key);

/* internal: framebuffer creation */
static GLuint create_framebuffer_object(gpu_texture_t* const* rts, u32 num_rts, gpu_texture_t* ds);

static void  opengl_device_update_viewport(void);
static void  opengl_device_update_scissor (void);
static s32   opengl_device_is_render_target_bound(const gpu_texture_t* tex);

opengl_context_t*       opengl_device_get_context(void)               { return s_device.gl_context; }
bool                    opengl_device_is_gles(void)
{
  return s_device.gl_context && opengl_context_is_gles(s_device.gl_context);
}
opengl_stream_buffer_t* opengl_device_get_texture_stream_buffer(void) { return s_device.texture_stream_buffer; }

void opengl_device_bind_update_texture_unit(void)
{
  opengl_device_set_active_texture(OPENGL_DEVICE_UPDATE_TEXTURE_UNIT - GL_TEXTURE0);
}

bool opengl_device_should_use_pbos_for_downloads(void)
{
  return !s_device.disable_pbo && !s_device.disable_async_download;
}

void opengl_device_set_error_object(Error* err, const char* prefix, GLenum gle)
{
  Error_set_string_fmt(err, "%sGL Error 0x%04X", prefix ? prefix : "", (unsigned)gle);
}

 gpu_shader_t* opengl_device_create_shader_from_source(gpu_shader_stage_t stage,
                                                       gpu_shader_language_t language, 
                                                       const char* source, size_t source_len,
                                                       const char* entry_point, Error* err)
{
  const gpu_shader_language_t expected = opengl_device_is_gles() ? GPU_SHADER_LANGUAGE_GLSL_ES
                                                                  : GPU_SHADER_LANGUAGE_GLSL;
  if (language != expected)
  {
    /* TranspileAndCreate path not ported (no SPIRV/glslang in cupid-ps1).  Caller
     * must pass GLSL/GLSLES directly. */
    Error_set_string_fmt(err, "Shader language mismatch: requested %d, expected %d.",
                         (int)language, (int)expected);
    return NULL;
  }

  if (!entry_point || strcmp(entry_point, "main") != 0)
  {
    Error_set_string_fmt(err, "Entry point must be 'main', got '%s'.",
                         entry_point ? entry_point : "(null)");
    return NULL;
  }

  if (source_len == 0 && source != NULL)
    source_len = strlen(source);

  /* Take an owning heap copy; opengl_shader_t takes ownership. */
  char* src_copy = (char*)malloc(source_len + 1);
  if (!src_copy)
  {
    Error_set_string(err, "Out of memory for shader source.");
    return NULL;
  }
  memcpy(src_copy, source, source_len);
  src_copy[source_len] = '\0';

  gpu_shader_cache_index_key_t key =
      gpu_shader_cache_compute_key(stage, language, src_copy, source_len,
                                   entry_point, strlen(entry_point));

  opengl_shader_t* sh = opengl_shader_alloc(stage, &key, src_copy, source_len);
  if (!sh)
  {
    free(src_copy);
    Error_set_string(err, "Out of memory for shader.");
    return NULL;
  }
  return &sh->base;
}

 opengl_swap_chain_t* opengl_swap_chain_create(const window_info_t* wi, gpu_vsync_mode_t mode,
                                              opengl_surface_handle_t surface_handle) 
{
  opengl_swap_chain_t* sc = (opengl_swap_chain_t*)calloc(1, sizeof(*sc));
  if (!sc) return NULL;
  gpu_swap_chain_base_init(&sc->base, wi, mode);
  sc->surface_handle = surface_handle;
  return sc;
}

void opengl_swap_chain_destroy(opengl_swap_chain_t* sc)
{
  if (!sc) return;
  if (s_device.gl_context && sc->surface_handle != OPENGL_MAIN_SURFACE)
    opengl_context_destroy_surface(s_device.gl_context, sc->surface_handle);
  free(sc);
}

bool opengl_swap_chain_resize_buffers(opengl_swap_chain_t* sc, u32 new_width, u32 new_height, Error* err)
{
  (void)err;
  if (sc->base.window_info.surface_width == new_width &&
      sc->base.window_info.surface_height == new_height)
    return true;

  sc->base.window_info.surface_width  = (u16)new_width;
  sc->base.window_info.surface_height = (u16)new_height;

  if (s_device.gl_context)
    opengl_context_resize_surface(s_device.gl_context, &sc->base.window_info, sc->surface_handle);
  return true;
}

bool opengl_swap_chain_set_swap_interval(opengl_context_t* ctx, gpu_vsync_mode_t mode, Error* err)
{
  /* Window framebuffer has to be bound to call SetSwapInterval. */
  const s32 interval = (mode == GPU_VSYNC_MODE_FIFO) ? 1 : 0;
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

  const bool result = opengl_context_set_swap_interval(ctx, interval, err);

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)current_fbo);
  return result;
}

bool opengl_swap_chain_set_vsync_mode(opengl_swap_chain_t* sc, gpu_vsync_mode_t mode, Error* err)
{
  /* OpenGL doesn't have Mailbox; map to FIFO. */
  if (mode == GPU_VSYNC_MODE_MAILBOX) mode = GPU_VSYNC_MODE_FIFO;

  if (sc->base.vsync_mode == mode)
    return true;

  const bool is_main = ((gpu_swap_chain_t*)sc == (gpu_swap_chain_t*)s_device.main_swap_chain);
  opengl_context_t* ctx = s_device.gl_context;
  if (!is_main && !opengl_context_make_current(ctx, sc->surface_handle, err))
    return false;

  const bool result = opengl_swap_chain_set_swap_interval(ctx, mode, err);

  if (!is_main && s_device.main_swap_chain)
  {
    if (!opengl_context_make_current(ctx, s_device.main_swap_chain->surface_handle, err))
      return false;
  }

  if (!result) return false;
  sc->base.vsync_mode = mode;
  return true;
}

static const GLenum s_draw_buffers[GPU_DEVICE_MAX_RENDER_TARGETS] = {
  GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
  GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
};

static void opengl_device_render_blank_frame_internal(void)
{
  static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearBufferfv(GL_COLOR, 0, black);
  glColorMask(s_device.last_blend_state.bits.write_r, s_device.last_blend_state.bits.write_g,
              s_device.last_blend_state.bits.write_b, s_device.last_blend_state.bits.write_a);
  glEnable(GL_SCISSOR_TEST);
  if (s_device.gl_context)
    opengl_context_swap_buffers(s_device.gl_context);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.current_fbo);
}

void opengl_device_render_blank_frame(void) { opengl_device_render_blank_frame_internal(); }

bool opengl_device_create(const window_info_t* wi, gpu_vsync_mode_t vsync,
                          gpu_device_create_flags_t flags, Error* err)
{
  if (g_gpu_device != NULL)
  {
    Error_set_string(err, "GPU device already created.");
    return false;
  }

  memset(&s_device, 0, sizeof(s_device));

  /* Mark "won't be matched" sentinel for last state. */
  memset(&s_device.last_rasterization_state, 0xFF, sizeof(s_device.last_rasterization_state));
  memset(&s_device.last_depth_state, 0xFF, sizeof(s_device.last_depth_state));
  memset(&s_device.last_blend_state, 0xFF, sizeof(s_device.last_blend_state));
  s_device.last_blend_state.bits.enable   = 0;
  s_device.last_blend_state.bits.constant = 0;

  s_device.last_scissor.left = 0; s_device.last_scissor.top = 0;
  s_device.last_scissor.right = 1; s_device.last_scissor.bottom = 1;

  s_device.render_api          = GPU_RENDER_API_OPENGL;
  s_device.uniform_buffer_alignment = 1;
  s_device.debug_device        = (flags & GPU_DEVICE_CREATE_ENABLE_DEBUG_DEVICE) != 0;

  program_cache_init(&s_device.program_cache);
  vao_cache_init(&s_device.vao_cache);
  fbo_cache_init(&s_device.fbo_cache);

  /* Create context. */
  window_info_t wi_copy = *wi;
  opengl_surface_handle_t wi_surface = OPENGL_MAIN_SURFACE;
  s_device.gl_context = opengl_context_create(&wi_copy, &wi_surface,
                                              (flags & GPU_DEVICE_CREATE_PREFER_GLES_CONTEXT) != 0, err);
  if (!s_device.gl_context)
  {
    ERROR_LOG("Failed to create any GL context");
    return false;
  }

  /* GL3.0 requires UBO check (not GLES). */
  if (!opengl_context_is_gles(s_device.gl_context) && !GLAD_GL_VERSION_3_1 &&
      !GLAD_GL_ARB_uniform_buffer_object)
  {
    Error_set_string(err, "OpenGL 3.1 or GL_ARB_uniform_buffer_object is required.");
    opengl_context_destroy(s_device.gl_context);
    s_device.gl_context = NULL;
    return false;
  }

  /* Keep the debug-group function pointers alive when KHR_debug
   * (or core 4.3+) is available, regardless of whether `debug_device` was
   * requested.  Active push/pop callsites land later as wrapper APIs; the
   * pointers being non-NULL means apitrace / RenderDoc can pick up labels
   * with zero per-frame cost when nothing's actually pushing groups.  When
   * KHR_debug isn't present we still null-out to short-circuit any future
   * caller that forgets to gate on availability. */
  const bool have_khr_debug = (GLAD_GL_VERSION_4_3 || GLAD_GL_KHR_debug ||
                               GLAD_GL_ES_VERSION_3_2);
  if (!have_khr_debug)
  {
    glPushDebugGroup    = NULL;
    glPopDebugGroup     = NULL;
    glDebugMessageInsert = NULL;
    glObjectLabel       = NULL;
  }
  else
  {
    /* Wire glDebugMessageCallback unconditionally when available.  Reports
     * GL errors / perf warnings to the cupid-ps1 log; cost is near-zero on
     * silent runs.  Verbose categories (notification, deprecation) gated
     * behind CUPID_TRACE / debug_device. */
    if (glDebugMessageCallback)
    {
      glDebugMessageCallback(opengl_debug_message_callback, NULL);
      if (glDebugMessageControl)
      {
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE,
                              0, NULL, GL_TRUE);
        /* Mute notification + push/pop spam unless verbose. */
        if (!s_device.debug_device)
        {
          glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
                                GL_DEBUG_SEVERITY_NOTIFICATION,
                                0, NULL, GL_FALSE);
          glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PUSH_GROUP,
                                GL_DONT_CARE, 0, NULL, GL_FALSE);
          glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_POP_GROUP,
                                GL_DONT_CARE, 0, NULL, GL_FALSE);
        }
      }
      glEnable(GL_DEBUG_OUTPUT);
      if (s_device.debug_device)
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    }
    if (s_device.debug_device)
      INFO_LOG("KHR_debug available; debug groups + object labels enabled.");
  }

  /* Main swap chain (unless surfaceless). */
  if (!window_info_is_surfaceless(&wi_copy))
  {
    /* OpenGL doesn't support Mailbox. */
    const gpu_vsync_mode_t actual = (vsync == GPU_VSYNC_MODE_MAILBOX) ? GPU_VSYNC_MODE_FIFO : vsync;
    s_device.main_swap_chain = opengl_swap_chain_create(&wi_copy, actual, wi_surface);
    if (!s_device.main_swap_chain)
    {
      Error_set_string(err, "Failed to allocate main swap chain.");
      opengl_context_destroy(s_device.gl_context);
      s_device.gl_context = NULL;
      return false;
    }

    Error swap_err = ERROR_INIT;
    if (!opengl_swap_chain_set_swap_interval(s_device.gl_context, actual, &swap_err))
      WARNING_LOG("Failed to set swap interval on main swap chain: %s", Error_get_description(&swap_err));
    Error_destroy(&swap_err);

    opengl_device_render_blank_frame_internal();
  }

  if (!opengl_device_check_features(flags))
  {
    opengl_swap_chain_destroy(s_device.main_swap_chain);
    s_device.main_swap_chain = NULL;
    opengl_context_destroy(s_device.gl_context);
    s_device.gl_context = NULL;
    return false;
  }

  if (!opengl_device_create_buffers())
  {
    Error_set_string(err, "Failed to create stream buffers.");
    opengl_swap_chain_destroy(s_device.main_swap_chain);
    s_device.main_swap_chain = NULL;
    opengl_context_destroy(s_device.gl_context);
    s_device.gl_context = NULL;
    return false;
  }

  glEnable(GL_SCISSOR_TEST);

  g_gpu_device = &s_device;
  return true;
}

void opengl_device_destroy(void)
{
  if (!s_device.gl_context) { g_gpu_device = NULL; return; }

  opengl_device_destroy_buffers();

  /* Drain caches.  All consumer pipelines should be released by the caller
   * before reaching here; whatever's left is a leak we just free. */
  for (u32 i = 0; i < s_device.fbo_cache.cap; ++i)
  {
    fbo_cache_entry_t* e = &s_device.fbo_cache.slots[i];
    if (e->occupied && e->fbo != 0)
      glDeleteFramebuffers(1, &e->fbo);
  }
  fbo_cache_destroy(&s_device.fbo_cache);

  for (u32 i = 0; i < s_device.vao_cache.cap; ++i)
  {
    vao_cache_entry_t* e = &s_device.vao_cache.slots[i];
    if (e->occupied && e->value.vao_id != 0)
      glDeleteVertexArrays(1, &e->value.vao_id);
  }
  vao_cache_destroy(&s_device.vao_cache);

  for (u32 i = 0; i < s_device.program_cache.cap; ++i)
  {
    program_cache_entry_t* e = &s_device.program_cache.slots[i];
    if (e->occupied && e->value.program_id != 0)
      glDeleteProgram(e->value.program_id);
  }
  program_cache_destroy(&s_device.program_cache);

  if (s_device.gl_context)
    opengl_context_done_current(s_device.gl_context);

  if (s_device.main_swap_chain)
  {
    opengl_swap_chain_destroy(s_device.main_swap_chain);
    s_device.main_swap_chain = NULL;
  }

  opengl_context_destroy(s_device.gl_context);
  s_device.gl_context = NULL;
  g_gpu_device = NULL;
}

static bool opengl_device_check_features(gpu_device_create_flags_t flags)
{
  const bool is_gles = opengl_context_is_gles(s_device.gl_context);
  s_device.render_api = is_gles ? GPU_RENDER_API_OPENGL_ES : GPU_RENDER_API_OPENGL;

  GLint major_version = 0, minor_version = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major_version);
  glGetIntegerv(GL_MINOR_VERSION, &minor_version);
  s_device.render_api_version = ((u32)major_version * 100u) + ((u32)minor_version * 10u);

  const char* vendor   = (const char*)glGetString(GL_VENDOR);
  const char* renderer = (const char*)glGetString(GL_RENDERER);
  s_device.driver_type = gpu_device_guess_driver_type(0, vendor, renderer);

  const bool is_shitty_mobile_driver =
    (s_device.driver_type == GPU_DRIVER_TYPE_ARM_PROPRIETARY ||
     s_device.driver_type == GPU_DRIVER_TYPE_QUALCOMM_PROPRIETARY ||
     s_device.driver_type == GPU_DRIVER_TYPE_IMAGINATION_PROPRIETARY ||
     s_device.driver_type == GPU_DRIVER_TYPE_ARM_MESA);
  s_device.disable_pbo =
    (!GLAD_GL_VERSION_4_4 && !GLAD_GL_ARB_buffer_storage && !GLAD_GL_EXT_buffer_storage) || is_shitty_mobile_driver;
  if (s_device.disable_pbo && !is_shitty_mobile_driver)
    WARNING_LOG("Not using PBOs for texture uploads because buffer_storage is unavailable.");
  else if (s_device.disable_pbo)
    WARNING_LOG("Disabling PBOs due to known slow or broken driver.");

  GLint max_texture_size = 1024;
  GLint max_samples = 1;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  s_device.max_texture_size = (max_texture_size > 1024) ? (u32)max_texture_size : 1024u;
  s_device.max_multisamples = (u16)((max_samples > 1) ? (u32)max_samples : 1u);

  GLint max_dual_source_draw_buffers = 0;
  glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &max_dual_source_draw_buffers);
  s_device.features.dual_source_blend =
    !(flags & GPU_DEVICE_CREATE_DISABLE_DUAL_SOURCE_BLEND) && (max_dual_source_draw_buffers > 0) && 
    (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended || GLAD_GL_EXT_blend_func_extended);

  s_device.features.framebuffer_fetch =
    !(flags & (GPU_DEVICE_CREATE_DISABLE_FEEDBACK_LOOPS | GPU_DEVICE_CREATE_DISABLE_FRAMEBUFFER_FETCH)) && 
    (GLAD_GL_EXT_shader_framebuffer_fetch || GLAD_GL_ARM_shader_framebuffer_fetch);

  s_device.features.texture_buffers =
    !(flags & GPU_DEVICE_CREATE_DISABLE_TEXTURE_BUFFERS) && 
    (GLAD_GL_VERSION_3_1 || GLAD_GL_ES_VERSION_3_2);

  if (renderer && strstr(renderer, "ANGLE"))
    s_device.features.texture_buffers = false;

  if (s_device.features.texture_buffers)
  {
    GLint max_texel_buffer_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texel_buffer_size);
    if (max_texel_buffer_size < (GLint)GPU_DEVICE_MIN_TEXEL_BUFFER_ELEMENTS)
    {
      WARNING_LOG("GL_MAX_TEXTURE_BUFFER_SIZE (%d) below required minimum, not using texture buffers.",
                  max_texel_buffer_size);
      s_device.features.texture_buffers = false;
    }
  }

  if (!s_device.features.texture_buffers && !(flags & GPU_DEVICE_CREATE_DISABLE_TEXTURE_BUFFERS))
  {
    GLint max_fragment_storage_blocks = 0;
    GLint64 max_ssbo_size = 0;
    if (GLAD_GL_VERSION_4_3 || GLAD_GL_ES_VERSION_3_1 || GLAD_GL_ARB_shader_storage_buffer_object)
    {
      glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &max_fragment_storage_blocks);
      glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
    }
    s_device.features.texture_buffers_emulated_with_ssbo =
      (max_fragment_storage_blocks > 0 && max_ssbo_size >= (GLint64)(1024 * 512 * sizeof(u16)));
    if (s_device.features.texture_buffers_emulated_with_ssbo)
    {
      INFO_LOG("Using shader storage buffers for VRAM writes.");
      s_device.features.texture_buffers = true;
    }
    else
    {
      WARNING_LOG("Both texture buffers and SSBOs are not supported. Performance will suffer.");
    }
  }

  s_device.features.per_sample_shading =
    (GLAD_GL_VERSION_4_0 || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_ARB_sample_shading) &&
    (s_device.driver_type != GPU_DRIVER_TYPE_AMD_PROPRIETARY &&
     s_device.driver_type != GPU_DRIVER_TYPE_INTEL_PROPRIETARY && !is_shitty_mobile_driver);

  s_device.features.noperspective_interpolation = !is_gles;
  s_device.features.texture_copy_to_self =
    (s_device.driver_type != GPU_DRIVER_TYPE_ARM_PROPRIETARY) &&
    !(flags & GPU_DEVICE_CREATE_DISABLE_TEXTURE_COPY_TO_SELF);
  s_device.features.feedback_loops = false;
  s_device.features.geometry_shaders =
    !(flags & GPU_DEVICE_CREATE_DISABLE_GEOMETRY_SHADERS) &&
    (GLAD_GL_VERSION_3_2 || GLAD_GL_ES_VERSION_3_2);
  s_device.features.compute_shaders = false;
  s_device.features.gpu_timing = false;  /* timestamp queries stubbed */
  s_device.features.partial_msaa_resolve = true;
  s_device.features.memory_import = true;
  s_device.features.exclusive_fullscreen = false;
  s_device.features.explicit_present = false;
  s_device.features.timed_present = false;
  s_device.features.shader_cache = false;
  s_device.features.dxt_textures =
    !(flags & GPU_DEVICE_CREATE_DISABLE_COMPRESSED_TEXTURES) && GLAD_GL_EXT_texture_compression_s3tc;
  s_device.features.bptc_textures =
    !(flags & GPU_DEVICE_CREATE_DISABLE_COMPRESSED_TEXTURES) && 
    (GLAD_GL_VERSION_4_2 || GLAD_GL_ARB_texture_compression_bptc || GLAD_GL_EXT_texture_compression_bptc);
  s_device.features.pipeline_cache = false; /* disk cache stubbed */
  s_device.features.prefer_unused_textures =
    is_gles || ((s_device.driver_type & GPU_DRIVER_TYPE_MOBILE_FLAG) == GPU_DRIVER_TYPE_MOBILE_FLAG);

  if (s_device.driver_type == GPU_DRIVER_TYPE_INTEL_PROPRIETARY)
  {
    WARNING_LOG("Disabling async downloads with PBOs due to known Intel driver issues.");
    s_device.disable_async_download = true;
  }

  s_device.use_get_texture_sub_image = (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image) &&
                                       s_device.driver_type != GPU_DRIVER_TYPE_NVIDIA_PROPRIETARY;
  return true;
}

static bool opengl_device_create_buffers(void)
{
  Error err = ERROR_INIT;
  s_device.vertex_buffer        = opengl_stream_buffer_create(GL_ARRAY_BUFFER, OPENGL_DEVICE_VERTEX_BUFFER_SIZE, &err);
  if (!s_device.vertex_buffer)        { ERROR_LOG("vertex_buffer create failed: %s", Error_get_description(&err)); Error_destroy(&err); return false; }

  s_device.index_buffer         = opengl_stream_buffer_create(GL_ELEMENT_ARRAY_BUFFER, OPENGL_DEVICE_INDEX_BUFFER_SIZE, &err);
  if (!s_device.index_buffer)         { ERROR_LOG("index_buffer create failed: %s", Error_get_description(&err)); Error_destroy(&err); return false; }

  s_device.uniform_buffer       = opengl_stream_buffer_create(GL_UNIFORM_BUFFER, OPENGL_DEVICE_UNIFORM_BUFFER_SIZE, &err);
  if (!s_device.uniform_buffer)       { ERROR_LOG("uniform_buffer create failed: %s", Error_get_description(&err)); Error_destroy(&err); return false; }

  s_device.push_constant_buffer = opengl_stream_buffer_create(GL_UNIFORM_BUFFER, OPENGL_DEVICE_PUSH_CONSTANT_BUFFER_SIZE, &err);
  if (!s_device.push_constant_buffer) { ERROR_LOG("push_constant_buffer create failed: %s", Error_get_description(&err)); Error_destroy(&err); return false; }

  GLint align = 16;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
  s_device.uniform_buffer_alignment = (align > 16) ? (GLuint)align : 16u;

  if (!s_device.disable_pbo)
  {
    s_device.texture_stream_buffer = opengl_stream_buffer_create(GL_PIXEL_UNPACK_BUFFER,
                                                                  OPENGL_DEVICE_TEXTURE_STREAM_BUFFER_SIZE, &err);
    if (!s_device.texture_stream_buffer)
    {
      ERROR_LOG("texture_stream_buffer create failed: %s", Error_get_description(&err));
      Error_destroy(&err);
      return false;
    }
    /* Need to unbind otherwise normal uploads will fail. */
    opengl_stream_buffer_unbind(s_device.texture_stream_buffer);
  }

  GLuint fbos[2];
  glGetError();
  glGenFramebuffers(2, fbos);
  GLenum gle = glGetError();
  if (gle != GL_NO_ERROR)
  {
    ERROR_LOG("Failed to create framebuffers: 0x%X", gle);
    return false;
  }
  s_device.read_fbo  = fbos[0];
  s_device.write_fbo = fbos[1];
  Error_destroy(&err);
  return true;
}

static void opengl_device_destroy_buffers(void)
{
  if (s_device.write_fbo) { glDeleteFramebuffers(1, &s_device.write_fbo); s_device.write_fbo = 0; }
  if (s_device.read_fbo)  { glDeleteFramebuffers(1, &s_device.read_fbo);  s_device.read_fbo  = 0; }
  if (s_device.texture_stream_buffer) { opengl_stream_buffer_destroy(s_device.texture_stream_buffer); s_device.texture_stream_buffer = NULL; }
  if (s_device.push_constant_buffer)  { opengl_stream_buffer_destroy(s_device.push_constant_buffer);  s_device.push_constant_buffer  = NULL; }
  if (s_device.uniform_buffer)        { opengl_stream_buffer_destroy(s_device.uniform_buffer);        s_device.uniform_buffer        = NULL; }
  if (s_device.index_buffer)          { opengl_stream_buffer_destroy(s_device.index_buffer);          s_device.index_buffer          = NULL; }
  if (s_device.vertex_buffer)         { opengl_stream_buffer_destroy(s_device.vertex_buffer);         s_device.vertex_buffer         = NULL; }
}

gpu_present_result_t opengl_device_begin_present(gpu_swap_chain_t* sc, u32 clear_color)
{
  opengl_swap_chain_t* osc = (opengl_swap_chain_t*)sc;
  if (s_device.gl_context)
  {
    Error err = ERROR_INIT;
    opengl_context_make_current(s_device.gl_context, osc->surface_handle, &err);
    Error_destroy(&err);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  float fc[4]; gpu_device_rgba8_to_float(clear_color, fc);
  glClearBufferfv(GL_COLOR, 0, fc);
  glColorMask(s_device.last_blend_state.bits.write_r, s_device.last_blend_state.bits.write_g,
              s_device.last_blend_state.bits.write_b, s_device.last_blend_state.bits.write_a);
  glEnable(GL_SCISSOR_TEST);

  s_device.current_fbo = 0;
  s_device.num_current_render_targets = 0;
  memset(s_device.current_render_targets, 0, sizeof(s_device.current_render_targets));
  s_device.current_depth_target = NULL;

  opengl_rect_t window_rc = { 0, 0, (s32)gpu_swap_chain_get_width(sc), (s32)gpu_swap_chain_get_height(sc) };
  s_device.last_viewport = window_rc;
  s_device.last_scissor  = window_rc;
  opengl_device_update_viewport();
  opengl_device_update_scissor();
  return GPU_PRESENT_RESULT_OK;
}

void opengl_device_end_present(gpu_swap_chain_t* sc, bool explicit_present, u64 present_time)
{
  (void)explicit_present; (void)present_time;
  DebugAssert(s_device.current_fbo == 0);
  if (s_device.gl_context)
    opengl_context_swap_buffers(s_device.gl_context);
  (void)sc;
}

void opengl_device_submit_present(gpu_swap_chain_t* sc) { (void)sc; Panic("Not supported by this API."); }

void opengl_device_set_active_texture(u32 slot)
{
  if (s_device.last_texture_unit != slot)
  {
    s_device.last_texture_unit = slot;
    glActiveTexture(GL_TEXTURE0 + slot);
  }
}

void opengl_device_unbind_texture_id(GLuint id)
{
  for (u32 slot = 0; slot < GPU_DEVICE_MAX_TEXTURE_SAMPLERS; ++slot)
  {
    if (s_device.last_sampler_tex[slot] == id)
    {
      s_device.last_sampler_tex[slot] = 0;
      const u32 unit = GL_TEXTURE0 + slot;
      if (s_device.last_texture_unit != unit)
      {
        s_device.last_texture_unit = unit;
        glActiveTexture(unit);
      }
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
}

void opengl_device_unbind_texture(opengl_texture_t* tex)
{
  opengl_device_unbind_texture_id(tex->id);

  if (gpu_texture_is_render_target(&tex->base))
  {
    for (u32 i = 0; i < s_device.num_current_render_targets; ++i)
    {
      if (s_device.current_render_targets[i] == tex)
      {
        DEV_LOG("Unbinding current RT");
        opengl_device_set_render_targets(NULL, 0, (gpu_texture_t*)s_device.current_depth_target);
        break;
      }
    }
    opengl_device_remove_fbo_references(&tex->base);
  }
  else if (gpu_texture_is_depth_stencil(&tex->base))
  {
    if (s_device.current_depth_target == tex)
    {
      DEV_LOG("Unbinding current DS");
      opengl_device_set_render_targets(NULL, 0, NULL);
    }
    opengl_device_remove_fbo_references(&tex->base);
  }
}

void opengl_device_unbind_ssbo(GLuint id)
{
  if (s_device.last_ssbo != id) return;
  s_device.last_ssbo = 0;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
}

void opengl_device_unbind_sampler(GLuint id)
{
  for (u32 slot = 0; slot < GPU_DEVICE_MAX_TEXTURE_SAMPLERS; ++slot)
  {
    if (s_device.last_sampler_smp[slot] == id)
    {
      s_device.last_sampler_smp[slot] = 0;
      glBindSampler(slot, 0);
    }
  }
}

void opengl_device_unbind_pipeline(const opengl_pipeline_t* pl)
{
  if (s_device.current_pipeline == pl)
  {
    s_device.current_pipeline = NULL;
    glUseProgram(0);
  }
}

void opengl_device_commit_clear(opengl_texture_t* tex)
{
  /* The texture-side commit_clear (if any) lives in opengl_texture.c; this
   * device-side helper is what opengl_pipeline.c / opengl_device.c call when
   * a texture might still have a pending clear before being read. */
  if (!tex) return;
  opengl_texture_commit_clear(tex);
}

void opengl_device_commit_rt_clear_in_fb(opengl_texture_t* tex, u32 idx)
{
  if (!tex || gpu_texture_get_state(&tex->base) == GPU_TEXTURE_STATE_DIRTY) return;
  if (gpu_texture_get_state(&tex->base) == GPU_TEXTURE_STATE_INVALIDATED)
  {
    if (glInvalidateFramebuffer)
    {
      const GLenum att = GL_COLOR_ATTACHMENT0 + idx;
      glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &att);
    }
  }
  else
  {
    float c[4]; gpu_texture_get_unorm_clear_color(&tex->base, c);
    /* Unmask channels temporarily if needed. */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearBufferfv(GL_COLOR, (GLint)idx, c);
    glColorMask(s_device.last_blend_state.bits.write_r, s_device.last_blend_state.bits.write_g,
                s_device.last_blend_state.bits.write_b, s_device.last_blend_state.bits.write_a);
  }
  gpu_texture_set_state(&tex->base, GPU_TEXTURE_STATE_DIRTY);
}

void opengl_device_commit_ds_clear_in_fb(opengl_texture_t* tex)
{
  if (!tex || gpu_texture_get_state(&tex->base) == GPU_TEXTURE_STATE_DIRTY) return;
  if (gpu_texture_get_state(&tex->base) == GPU_TEXTURE_STATE_INVALIDATED)
  {
    if (glInvalidateFramebuffer)
    {
      const GLenum att = GL_DEPTH_ATTACHMENT;
      glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &att);
    }
  }
  else
  {
    const GLfloat d = (GLfloat)gpu_texture_get_clear_depth(&tex->base);
    glDepthMask(GL_TRUE);
    glClearBufferfv(GL_DEPTH, 0, &d);
    glDepthMask(s_device.last_depth_state.bits.depth_write ? GL_TRUE : GL_FALSE);
  }
  gpu_texture_set_state(&tex->base, GPU_TEXTURE_STATE_DIRTY);
}

void opengl_device_copy_texture_region(gpu_texture_t* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                        gpu_texture_t* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level,
                                       u32 width, u32 height) 
{
  opengl_texture_t* D = (opengl_texture_t*)dst;
  opengl_texture_t* S = (opengl_texture_t*)src;
  opengl_device_commit_clear(D);
  opengl_device_commit_clear(S);
  g_gpu_device_stats.num_copies++;

  const GLuint sid = S->id;
  const GLuint did = D->id;
  if (GLAD_GL_VERSION_4_3 || GLAD_GL_ARB_copy_image)
  {
    glCopyImageSubData(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer,
                       did, GL_TEXTURE_2D, dst_level, dst_x, dst_y, dst_layer,
                       width, height, 1);
  }
  else if (GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer,
                          did, GL_TEXTURE_2D, dst_level, dst_x, dst_y, dst_layer,
                          width, height, 1);
  }
  else
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_device.read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.write_fbo);
    if (gpu_texture_is_array(&D->base))
      glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, did, dst_level, dst_layer);
    else
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, opengl_texture_get_target(D), did, dst_level);
    if (gpu_texture_is_array(&S->base))
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sid, src_level, src_layer);
    else
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, opengl_texture_get_target(S), sid, src_level);

    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height,
                      dst_x, dst_y, dst_x + width, dst_y + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);

    if (s_device.current_fbo)
    {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.current_fbo);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    else
    {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }
}

void opengl_device_resolve_texture_region(gpu_texture_t* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                           gpu_texture_t* src, u32 src_x, u32 src_y,
                                          u32 width, u32 height) 
{
  opengl_texture_t* D = (opengl_texture_t*)dst;
  opengl_texture_t* S = (opengl_texture_t*)src;

  glBindFramebuffer(GL_READ_FRAMEBUFFER, s_device.read_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.write_fbo);
  if (gpu_texture_is_array(&D->base))
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, D->id, dst_level, dst_layer);
  else
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, opengl_texture_get_target(D), D->id, dst_level);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, opengl_texture_get_target(S), S->id, 0);

  opengl_device_commit_clear(S);
  if (width == gpu_texture_get_mip_width(&D->base, dst_level) &&
      height == gpu_texture_get_mip_height(&D->base, dst_level))
  {
    gpu_texture_set_state(&D->base, GPU_TEXTURE_STATE_DIRTY);
    if (glInvalidateFramebuffer)
    {
      const GLenum att = GL_COLOR_ATTACHMENT0;
      glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &att);
    }
  }
  else
  {
    opengl_device_commit_clear(D);
  }

  g_gpu_device_stats.num_copies++;

  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height,
                    dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glEnable(GL_SCISSOR_TEST);

  if (s_device.current_fbo)
  {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.current_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }
  else
  {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
}

void opengl_device_clear_render_target(gpu_texture_t* t, u32 c)
{
  if (!t) return;
  gpu_texture_set_clear_color(t, c);
  const s32 idx = opengl_device_is_render_target_bound(t);
  if (idx >= 0)
    opengl_device_commit_rt_clear_in_fb((opengl_texture_t*)t, (u32)idx);
}

void opengl_device_clear_depth(gpu_texture_t* t, float d)
{
  if (!t) return;
  gpu_texture_set_clear_depth(t, d);
  if (s_device.current_depth_target == (opengl_texture_t*)t)
    opengl_device_commit_ds_clear_in_fb((opengl_texture_t*)t);
}

void opengl_device_invalidate_render_target(gpu_texture_t* t)
{
  if (!t) return;
  gpu_texture_set_state(t, GPU_TEXTURE_STATE_INVALIDATED);
  if (gpu_texture_is_render_target(t))
  {
    const s32 idx = opengl_device_is_render_target_bound(t);
    if (idx >= 0)
      opengl_device_commit_rt_clear_in_fb((opengl_texture_t*)t, (u32)idx);
  }
  else
  {
    DebugAssert(gpu_texture_is_depth_stencil(t));
    if (s_device.current_depth_target == (opengl_texture_t*)t)
      opengl_device_commit_ds_clear_in_fb((opengl_texture_t*)t);
  }
}

static s32 opengl_device_is_render_target_bound(const gpu_texture_t* tex)
{
  for (u32 i = 0; i < s_device.num_current_render_targets; ++i)
  {
    if ((const gpu_texture_t*)s_device.current_render_targets[i] == tex)
      return (s32)i;
  }
  return -1;
}

void opengl_device_set_render_targets(gpu_texture_t* const* rts, u32 num_rts, gpu_texture_t* ds)
{
  bool changed = (s_device.num_current_render_targets != num_rts ||
                  s_device.current_depth_target != (opengl_texture_t*)ds);
  bool needs_ds_clear = (ds && gpu_texture_is_cleared_or_invalidated(ds));
  bool needs_rt_clear = false;

  s_device.current_depth_target = (opengl_texture_t*)ds;
  for (u32 i = 0; i < num_rts; ++i)
  {
    opengl_texture_t* dt = (opengl_texture_t*)rts[i];
    changed |= (s_device.current_render_targets[i] != dt);
    s_device.current_render_targets[i] = dt;
    if (dt) needs_rt_clear |= gpu_texture_is_cleared_or_invalidated(&dt->base);
  }
  for (u32 i = num_rts; i < s_device.num_current_render_targets; ++i)
    s_device.current_render_targets[i] = NULL;
  s_device.num_current_render_targets = num_rts;

  if (changed)
  {
    GLuint fbo = 0;
    if (s_device.num_current_render_targets > 0 || s_device.current_depth_target)
    {
      gpu_framebuffer_key_t key; memset(&key, 0, sizeof(key));
      for (u32 i = 0; i < num_rts; ++i) key.rts[i] = rts[i];
      key.ds = ds;
      key.num_rts = num_rts;
      key.flags = 0;
      fbo = opengl_device_lookup_or_create_fbo(&key);
      if (fbo == 0)
      {
        ERROR_LOG("Failed to get FBO for %u render targets", num_rts);
        s_device.current_fbo = 0;
        memset(s_device.current_render_targets, 0, sizeof(s_device.current_render_targets));
        s_device.num_current_render_targets = 0;
        s_device.current_depth_target = NULL;
        return;
      }
    }
    g_gpu_device_stats.num_render_passes++;
    s_device.current_fbo = fbo;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  }

  if (needs_rt_clear)
  {
    for (u32 i = 0; i < num_rts; ++i)
    {
      opengl_texture_t* dt = (opengl_texture_t*)rts[i];
      if (dt && gpu_texture_is_cleared_or_invalidated(&dt->base))
        opengl_device_commit_rt_clear_in_fb(dt, i);
    }
  }

  if (needs_ds_clear)
    opengl_device_commit_ds_clear_in_fb((opengl_texture_t*)ds);
}

void opengl_device_set_texture_sampler(u32 slot, gpu_texture_t* texture, struct opengl_sampler* sampler)
{
  DebugAssert(slot < GPU_DEVICE_MAX_TEXTURE_SAMPLERS);
  opengl_texture_t* T = (opengl_texture_t*)texture;
  GLuint Tid = 0;
  if (T)
  {
    Tid = T->id;
    opengl_device_commit_clear(T);
  }

  if (s_device.last_sampler_tex[slot] != Tid)
  {
    s_device.last_sampler_tex[slot] = Tid;
    opengl_device_set_active_texture(slot);
    glBindTexture(T ? opengl_texture_get_target(T) : GL_TEXTURE_2D, Tid);
  }

  const GLuint Sid = sampler ? ((opengl_sampler_t*)sampler)->id : 0;
  if (s_device.last_sampler_smp[slot] != Sid)
  {
    s_device.last_sampler_smp[slot] = Sid;
    glBindSampler(slot, Sid);
  }
}

void opengl_device_set_texture_buffer(u32 slot, struct opengl_texture_buffer* buf)
{
  DebugAssert(slot < GPU_DEVICE_MAX_TEXTURE_SAMPLERS);
  const opengl_texture_buffer_t* B = (const opengl_texture_buffer_t*)buf;

  if (!s_device.features.texture_buffers_emulated_with_ssbo)
  {
    const GLuint Tid = B ? B->texture_id : 0;
    if (s_device.last_sampler_tex[slot] != Tid)
    {
      s_device.last_sampler_tex[slot] = Tid;
      opengl_device_set_active_texture(slot);
      glBindTexture(GL_TEXTURE_BUFFER, Tid);
    }
  }
  else
  {
    DebugAssert(slot == 0);
    const GLuint bid = B ? opengl_stream_buffer_get_id(B->buffer) : 0;
    if (s_device.last_ssbo == bid)
      return;
    s_device.last_ssbo = bid;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, bid);
  }
}

static void opengl_device_update_viewport(void)
{
  glViewport(s_device.last_viewport.left, s_device.last_viewport.top,
             s_device.last_viewport.right - s_device.last_viewport.left,
             s_device.last_viewport.bottom - s_device.last_viewport.top);
}
static void opengl_device_update_scissor(void)
{
  glScissor(s_device.last_scissor.left, s_device.last_scissor.top,
            s_device.last_scissor.right - s_device.last_scissor.left,
            s_device.last_scissor.bottom - s_device.last_scissor.top);
}

void opengl_device_set_viewport(opengl_rect_t r)
{
  if (memcmp(&s_device.last_viewport, &r, sizeof(r)) == 0) return;
  s_device.last_viewport = r;
  opengl_device_update_viewport();
}

void opengl_device_set_scissor(opengl_rect_t r)
{
  if (memcmp(&s_device.last_scissor, &r, sizeof(r)) == 0) return;
  s_device.last_scissor = r;
  opengl_device_update_scissor();
}

void opengl_device_draw(u32 vertex_count, u32 base_vertex)
{
  g_gpu_device_stats.num_draws++;
  if (glDrawElementsBaseVertex)
  {
    glDrawArrays(opengl_pipeline_get_topology(s_device.current_pipeline), base_vertex, vertex_count);
    return;
  }
  opengl_device_set_vertex_buffer_offsets(base_vertex);
  glDrawArrays(opengl_pipeline_get_topology(s_device.current_pipeline), 0, vertex_count);
}

void opengl_device_draw_with_push_constants(u32 vertex_count, u32 base_vertex,
                                            const void* push_constants, u32 push_constants_size)
{
  opengl_device_push_uniform_buffer(push_constants, push_constants_size);
  opengl_device_draw(vertex_count, base_vertex);
}

void opengl_device_draw_indexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  g_gpu_device_stats.num_draws++;
  if (glDrawElementsBaseVertex)
  {
    const void* indices = (const void*)((uintptr_t)base_index * sizeof(u16));
    glDrawElementsBaseVertex(opengl_pipeline_get_topology(s_device.current_pipeline),
                             index_count, GL_UNSIGNED_SHORT, indices, base_vertex);
    return;
  }
  opengl_device_set_vertex_buffer_offsets(base_vertex);
  const void* indices = (const void*)((uintptr_t)base_index * sizeof(u16));
  glDrawElements(opengl_pipeline_get_topology(s_device.current_pipeline),
                 index_count, GL_UNSIGNED_SHORT, indices);
}

void opengl_device_draw_indexed_with_push_constants(u32 index_count, u32 base_index, u32 base_vertex,
                                                    const void* push_constants, u32 push_constants_size)
{
  opengl_device_push_uniform_buffer(push_constants, push_constants_size);
  opengl_device_draw_indexed(index_count, base_index, base_vertex);
}

void opengl_device_map_vertex_buffer(u32 vertex_size, u32 vertex_count,
                                     void** map_ptr, u32* map_space, u32* map_base_vertex)
{
  opengl_stream_buffer_mapping_t res = opengl_stream_buffer_map(s_device.vertex_buffer, vertex_size,
                                                                 vertex_size * vertex_count);
  *map_ptr        = res.pointer;
  *map_space      = res.space_aligned;
  *map_base_vertex = res.index_aligned;
}

void opengl_device_unmap_vertex_buffer(u32 vertex_size, u32 vertex_count)
{
  const u32 size = vertex_size * vertex_count;
  g_gpu_device_stats.buffer_streamed += size;
  opengl_stream_buffer_unmap(s_device.vertex_buffer, size);
}

void opengl_device_map_index_buffer(u32 index_count, u16** map_ptr, u32* map_space, u32* map_base_index)
{
  opengl_stream_buffer_mapping_t res = opengl_stream_buffer_map(s_device.index_buffer, sizeof(u16),
                                                                 sizeof(u16) * index_count);
  *map_ptr        = (u16*)res.pointer;
  *map_space      = res.space_aligned;
  *map_base_index = res.index_aligned;
}

void opengl_device_unmap_index_buffer(u32 used_index_count)
{
  const u32 size = (u32)(sizeof(u16) * used_index_count);
  g_gpu_device_stats.buffer_streamed += size;
  opengl_stream_buffer_unmap(s_device.index_buffer, size);
}

void opengl_device_push_uniform_buffer(const void* data, u32 data_size)
{
  opengl_stream_buffer_mapping_t res = opengl_stream_buffer_map(s_device.push_constant_buffer,
                                                                 s_device.uniform_buffer_alignment, data_size);
  memcpy(res.pointer, data, data_size);
  opengl_stream_buffer_unmap(s_device.push_constant_buffer, data_size);
  g_gpu_device_stats.buffer_streamed += data_size;
  glBindBufferRange(GL_UNIFORM_BUFFER, 1,
                   opengl_stream_buffer_get_id(s_device.push_constant_buffer),
                   res.buffer_offset, data_size);
}

void* opengl_device_map_uniform_buffer(u32 size)
{
  opengl_stream_buffer_mapping_t res = opengl_stream_buffer_map(s_device.uniform_buffer,
                                                                 s_device.uniform_buffer_alignment, size);
  return res.pointer;
}

void opengl_device_unmap_uniform_buffer(u32 size)
{
  const u32 pos = opengl_stream_buffer_unmap(s_device.uniform_buffer, size);
  g_gpu_device_stats.buffer_streamed += size;
  glBindBufferRange(GL_UNIFORM_BUFFER, 0,
                   opengl_stream_buffer_get_id(s_device.uniform_buffer),
                   pos, size);
}

static GLuint create_framebuffer_object(gpu_texture_t* const* rts, u32 num_rts, gpu_texture_t* ds)
{
  glGetError();

  GLuint fbo_id = 0;
  glGenFramebuffers(1, &fbo_id);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_id);

  for (u32 i = 0; i < num_rts; ++i)
  {
    opengl_texture_t* RT = (opengl_texture_t*)rts[i];
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                           opengl_texture_get_target(RT), RT->id, 0);
  }
  if (ds)
  {
    opengl_texture_t* DS = (opengl_texture_t*)ds;
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           opengl_texture_get_target(DS), DS->id, 0);
  }
  glDrawBuffers((GLsizei)num_rts, s_draw_buffers);

  const GLenum err = glGetError();
  if (err != GL_NO_ERROR ||
      glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    ERROR_LOG("Failed to create GL framebuffer: 0x%X", err);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.current_fbo);
    glDeleteFramebuffers(1, &fbo_id);
    return 0;
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_device.current_fbo);
  return fbo_id;
}

GLuint opengl_device_lookup_or_create_fbo(const gpu_framebuffer_key_t* key)
{
  fbo_cache_entry_t* slot = fbo_cache_find(&s_device.fbo_cache, key);
  if (slot && slot->occupied)
    return slot->fbo;

  GLuint fbo = create_framebuffer_object(key->rts, key->num_rts, key->ds);
  if (fbo == 0) return 0;

  fbo_cache_entry_t* ins = fbo_cache_insert(&s_device.fbo_cache, key);
  ins->fbo = fbo;
  return fbo;
}

void opengl_device_remove_fbo_references(const gpu_texture_t* tex)
{
  for (u32 i = 0; i < s_device.fbo_cache.cap; ++i)
  {
    fbo_cache_entry_t* e = &s_device.fbo_cache.slots[i];
    if (!e->occupied) continue;
    bool refs = (e->key.ds == tex) || gpu_framebuffer_key_contains_rt(&e->key, tex);
    if (refs)
    {
      if (e->fbo != 0) glDeleteFramebuffers(1, &e->fbo);
      e->occupied = false;
      e->tombstone = true;
      e->fbo = 0;
      s_device.fbo_cache.count--;
    }
  }
}

#define HASH_BUILD(NAME, ENTRY_T, KEY_T, ITEM_T, HASH_FN, EQ_FN, OWNER)                          \
static void NAME##_init(NAME##_t* c)                                                              \
{                                                                                                  \
   c->cap = OPENGL_DEVICE_HASH_INITIAL_CAP;                                                        \
  c->count = 0;                                                                                   \
  c->slots = (ENTRY_T*)calloc(c->cap, sizeof(ENTRY_T));                                            \
}                                                                                                  \
static void NAME##_destroy(NAME##_t* c)                                                            \
{                                                                                                  \
  free(c->slots); c->slots = NULL; c->cap = 0; c->count = 0;                                       \
}                                                                                                  \
static ENTRY_T* NAME##_find_slot(ENTRY_T* slots, u32 cap, const KEY_T* key)                        \
{                                                                                                  \
  if (cap == 0) return NULL;                                                                       \
   u64 h = HASH_FN(key);                                                                            \
  u32 idx = (u32)(h & (u64)(cap - 1));                                                             \
  ENTRY_T* first_tomb = NULL;                                                                      \
  for (u32 step = 0; step < cap; ++step)                                                            \
  {                                                                                                \
    ENTRY_T* s = &slots[idx];                                                                      \
    if (!s->occupied && !s->tombstone) { return first_tomb ? first_tomb : s; }                     \
    if (s->tombstone && !first_tomb) first_tomb = s;                                                \
    if (s->occupied && EQ_FN(&s->key, key)) return s;                                              \
    idx = (idx + 1) & (cap - 1);                                                                   \
  }                                                                                                \
  return first_tomb;                                                                               \
}                                                                                                  \
static void NAME##_grow(NAME##_t* c)                                                               \
{                                                                                                  \
   u32 new_cap = c->cap ? (c->cap * 2) : OPENGL_DEVICE_HASH_INITIAL_CAP;                            \
  ENTRY_T* new_slots = (ENTRY_T*)calloc(new_cap, sizeof(ENTRY_T));                                  \
  for (u32 i = 0; i < c->cap; ++i)                                                                  \
  {                                                                                                \
    ENTRY_T* old = &c->slots[i];                                                                   \
    if (!old->occupied) continue;                                                                   \
    ENTRY_T* dst = NAME##_find_slot(new_slots, new_cap, &old->key);                                \
    *dst = *old;                                                                                    \
    dst->tombstone = false;                                                                          \
  }                                                                                                \
  free(c->slots);                                                                                  \
   c->slots = new_slots;                                                                            \
  c->cap = new_cap;                                                                                \
}                                                                                                  \
static ENTRY_T* NAME##_find(NAME##_t* c, const KEY_T* key)                                         \
{                                                                                                  \
  ENTRY_T* s = NAME##_find_slot(c->slots, c->cap, key);                                            \
  return (s && s->occupied) ? s : NULL;                                                            \
}                                                                                                  \
static ENTRY_T* NAME##_insert(NAME##_t* c, const KEY_T* key)                                       \
{                                                                                                  \
  if ((c->count + 1) * 4 > c->cap * 3) NAME##_grow(c);                                             \
  ENTRY_T* s = NAME##_find_slot(c->slots, c->cap, key);                                            \
  if (s->occupied) return s;                                                                       \
   s->occupied = true;                                                                              \
  s->tombstone = false;                                                                            \
  s->key = *key;                                                                                   \
  c->count++;                                                                                      \
  return s;                                                                                        \
}                                                                                                  \
static void NAME##_erase(NAME##_t* c, ENTRY_T* slot)                                                \
{                                                                                                  \
  if (!slot || !slot->occupied) return;                                                            \
   slot->occupied = false;                                                                          \
  slot->tombstone = true;                                                                          \
  c->count--;                                                                                      \
  (void)OWNER;                                                                                     \
}

HASH_BUILD(program_cache, program_cache_entry_t, opengl_pipeline_program_key_t,
           opengl_pipeline_program_item_t, opengl_pipeline_program_key_hash, opengl_pipeline_program_key_eq, 0)
HASH_BUILD(vao_cache,     vao_cache_entry_t,     opengl_pipeline_vao_key_t,
           opengl_pipeline_vao_item_t,           opengl_pipeline_vao_key_hash,     opengl_pipeline_vao_key_eq, 0)
HASH_BUILD(fbo_cache,     fbo_cache_entry_t,     gpu_framebuffer_key_t,
           GLuint,                                gpu_framebuffer_key_hash,         gpu_framebuffer_key_eq, 0)

#undef HASH_BUILD

/* Program cache wrappers.  These return an opaque void* slot pointer; the
 * caller uses opengl_device_program_cache_slot_*_value to manipulate it. */
void* opengl_device_program_cache_find_wrapper(const opengl_pipeline_program_key_t* key)
{
  return program_cache_find(&s_device.program_cache, key);
}

void* opengl_device_program_cache_insert_wrapper(const opengl_pipeline_program_key_t* key)
{
  return program_cache_insert(&s_device.program_cache, key);
}

void opengl_device_program_cache_erase_wrapper(void* slot)
{
  program_cache_erase(&s_device.program_cache, (program_cache_entry_t*)slot);
}

opengl_pipeline_program_item_t* opengl_device_program_cache_slot_value(void* slot)
{
  return slot ? &((program_cache_entry_t*)slot)->value : NULL;
}

void* opengl_device_vao_cache_find_wrapper(const opengl_pipeline_vao_key_t* key)
{
  return vao_cache_find(&s_device.vao_cache, key);
}

void* opengl_device_vao_cache_insert_wrapper(const opengl_pipeline_vao_key_t* key)
{
  return vao_cache_insert(&s_device.vao_cache, key);
}

void opengl_device_vao_cache_erase_wrapper(void* slot)
{
  vao_cache_erase(&s_device.vao_cache, (vao_cache_entry_t*)slot);
}

opengl_pipeline_vao_item_t* opengl_device_vao_cache_slot_value(void* slot)
{
  return slot ? &((vao_cache_entry_t*)slot)->value : NULL;
}

/* Pipeline-state mutators called from SetPipeline path. */
void opengl_device_set_last_program(GLuint p) { s_device.last_program = p; }
GLuint opengl_device_get_last_program(void)   { return s_device.last_program; }

/* Vertex-array binding accessors for SetVertexBufferOffsets. */
const opengl_pipeline_vao_key_t* opengl_device_get_last_vao_key(void)
{
  return s_device.last_vao_valid ? &s_device.last_vao_key : NULL;
}
GLuint opengl_device_get_last_vao_id(void) { return s_device.last_vao_id; }
void   opengl_device_set_last_vao(const opengl_pipeline_vao_key_t* key, GLuint vao_id)
{
  if (key) { s_device.last_vao_key = *key; s_device.last_vao_valid = true; }
  else     { s_device.last_vao_valid = false; }
  s_device.last_vao_id = vao_id;
}

opengl_pipeline_t** opengl_device_current_pipeline_slot(void) { return &s_device.current_pipeline; }

void opengl_device_invalidate_state_cache(void)
{
  /* 0xFF sentinel forces every memcmp-based state-set to miss its cache and
   * re-issue the GL call on the next set_*. */
  memset(&s_device.last_blend_state,         0xFF, sizeof(s_device.last_blend_state));
  memset(&s_device.last_rasterization_state, 0xFF, sizeof(s_device.last_rasterization_state));
  memset(&s_device.last_depth_state,         0xFF, sizeof(s_device.last_depth_state));
  memset(&s_device.last_viewport,            0xFF, sizeof(s_device.last_viewport));
  memset(&s_device.last_scissor,             0xFF, sizeof(s_device.last_scissor));
  s_device.current_pipeline = NULL;
  s_device.last_program     = 0;
  s_device.last_vao_valid   = false;
  s_device.last_vao_id      = 0;
  /* Sampler-unit bindings: gl_present_frame (or any external GL caller) may
   * have raw-bound a different texture / sampler to TEXTURE0 for its own
   * fullscreen blit.  Without this reset, the next set_texture_sampler call
   * sees its cache match and SKIPS the actual glBindTexture, leaving the FS
   * sampling whatever gl_present bound last frame; visible as garbage
   * texels (e.g. last frame's display image used as paletted-texture data,
   * which gives banded / wrong-CLUT artefacts on textured polygons). */
  for (u32 slot = 0; slot < GPU_DEVICE_MAX_TEXTURE_SAMPLERS; slot++) {
    s_device.last_sampler_tex[slot] = 0xFFFFFFFFu;
    s_device.last_sampler_smp[slot] = 0xFFFFFFFFu;
  }
}
opengl_stream_buffer_t* opengl_device_get_vertex_buffer(void) { return s_device.vertex_buffer; }
opengl_stream_buffer_t* opengl_device_get_index_buffer (void) { return s_device.index_buffer; }
GLuint                  opengl_device_get_uniform_buffer_alignment(void)
{ return s_device.uniform_buffer_alignment; }
gpu_blend_state_t*         opengl_device_last_blend_state_slot(void)         { return &s_device.last_blend_state; }
gpu_rasterization_state_t* opengl_device_last_rasterization_state_slot(void) { return &s_device.last_rasterization_state; }
gpu_depth_state_t*         opengl_device_last_depth_state_slot(void)         { return &s_device.last_depth_state; }

bool opengl_device_get_pipeline_disk_cache_active(void) { return false; }

const gpu_device_features_t* opengl_device_get_features(void) { return &s_device.features; }

GLuint opengl_device_get_read_fbo(void)    { return s_device.read_fbo; }
GLuint opengl_device_get_write_fbo(void)   { return s_device.write_fbo; }
GLuint opengl_device_get_current_fbo(void) { return s_device.current_fbo; }
bool   opengl_device_use_get_texture_sub_image(void) { return s_device.use_get_texture_sub_image; }
u32    opengl_device_get_max_texture_size(void) { return s_device.max_texture_size; }
u16    opengl_device_get_max_multisamples(void) { return s_device.max_multisamples; }

/* Pipeline create/destroy forwards to opengl_pipeline.c. */
gpu_pipeline_t* opengl_device_create_pipeline(const gpu_pipeline_graphics_config_t* cfg, Error* err)
{
  return opengl_pipeline_create_from_graphics_config(cfg, err);
}

void opengl_device_destroy_pipeline(gpu_pipeline_t* p)
{
  opengl_pipeline_destroy_from_device(p);
}

/* Thin debug-group wrappers.  glPushDebugGroup / glPopDebugGroup
 * are nulled at device-create time when the GL context lacks KHR_debug
 * (and core 4.3+); the explicit NULL check makes the pop-without-push case
 * (and the no-extension case) a no-op. */
void opengl_device_push_debug_group(const char* name)
{
  if (!glPushDebugGroup || !name)
    return;
  glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
}

void opengl_device_pop_debug_group(void)
{
  if (!glPopDebugGroup)
    return;
  glPopDebugGroup();
}
