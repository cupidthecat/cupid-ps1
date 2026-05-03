/*
 * Hardware (OpenGL) GPU back-end implementation.
 *
 * The full GL-driven batched-quad emission, VRAM streaming, depth-buffer
 * mask emulation, shader compile pipelines, etc. is large; this first cut
 * focuses on:
 *
 *   - The vtable + factory plumbing (mirrors gpu_sw.c).
 *   - State scaffolding (gpu_hw_t struct fully populated by the factory).
 *   - SW-renderer fallback for VRAM ops (draws and transfers go through
 *     gpu_sw_rasterizer, which already maintains g_vram canonically).  This
 *     keeps the back-end functionally equivalent to SW for tests / cross-
 *     renderer save state, while the GL-side draw path lands incrementally.
 *   - Display copy-out (mirrors `sw_update_display` so the SDL frontend
 *     keeps presenting frames identically - the 5th cupid-ps1 deviation,
 *     intentional).
 *   - do_state matches `sw_do_state` byte-for-byte.
 *
 * Anything that requires a live `g_gpu_device` (pipeline create, texture
 * fetch, draw issue) is gated on a non-NULL device pointer; the factory
 * refuses to create the back-end if the GL device hasn't been brought up
 * yet, returning NULL with `err` populated.  Once `opengl_device.c` lands
 * the full Compile/Create/DrawBatch path can be enabled in-place.
 *
 * Drop list (all wrapped or simply omitted):
 *   - PGXP precision uplift (DrawPrecisePolygon/Line)
 *   - Downsampling + adaptive downsample
 *   - Texture replacements / texture cache
 *   - Internal post-FX
 *   - MSAA (m_multisamples = 1 always)
 *   - Shader blending feedback-loop variants
 *   - CheckForTexPageOverlap (texture-cache branch)
 */

#include "core/gpu_hw.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "core/gpu.h"
#include "core/gpu_helpers.h"
#include "core/gpu_hw_shadergen.h"
#include "core/gpu_hw_texture_cache.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/settings.h"
#include "util/gpu_device.h"
#include "util/opengl_device.h"
#include "util/opengl_pipeline.h"
#include "util/opengl_texture.h"
#include "util/state_wrapper.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPU_HW);

/* VRAM render-target / depth-stencil formats. */
#define VRAM_RT_FORMAT  GPU_TEXTURE_FORMAT_RGBA8
#define VRAM_DS_FORMAT  GPU_TEXTURE_FORMAT_D16

#define GPU_HW_MAX_BATCH_VERTICES  65536u
#define GPU_HW_MAX_BATCH_INDICES   65536u

static bool gpu_hw_create_buffers(gpu_hw_t* hw, Error* err)
{
  const u32 texture_width  = (u32)VRAM_WIDTH  * (u32)hw->resolution_scale;
  const u32 texture_height = (u32)VRAM_HEIGHT * (u32)hw->resolution_scale;

   hw->vram_texture = opengl_texture_create(
    texture_width, texture_height, 1, 1, hw->multisamples,
    GPU_TEXTURE_TYPE_RENDER_TARGET, VRAM_RT_FORMAT, GPU_TEXTURE_FLAG_NONE,
    NULL, 0, err);
  if (!hw->vram_texture)
    return false;

  /* Depth attachment for the mask-bit emulation path
   * (write_mask_as_depth).  Writes are gated through depth state:
   * check_mask=1 batches use depth_func=GREATER_OR_EQUAL so destination pixels
   * with mask=set (depth=high z) reject incoming writes; check_mask=0 batches
   * use depth_func=ALWAYS + depth_write=on so the new pixel's mask bit
   * (oalpha * v_pos.z) lands in depth.  Avoids the shader-blend / framebuffer-
   * fetch path for the common check_mask + true_color+dither cases. */
  if (hw->write_mask_as_depth) {
    hw->vram_depth_texture = opengl_texture_create(
      texture_width, texture_height, 1, 1, hw->multisamples,
      GPU_TEXTURE_TYPE_DEPTH_STENCIL, VRAM_DS_FORMAT, GPU_TEXTURE_FLAG_NONE,
      NULL, 0, err);
    if (!hw->vram_depth_texture)
      return false;
  }

   hw->vram_read_texture = opengl_texture_create(
    texture_width, texture_height, 1, 1, 1,
    GPU_TEXTURE_TYPE_TEXTURE, VRAM_RT_FORMAT, GPU_TEXTURE_FLAG_NONE,
    NULL, 0, err);
  if (!hw->vram_read_texture)
    return false;

   hw->vram_readback_texture = opengl_texture_create(
    (u32)VRAM_WIDTH / 2u, (u32)VRAM_HEIGHT, 1, 1, 1, 
    GPU_TEXTURE_TYPE_RENDER_TARGET, VRAM_RT_FORMAT, GPU_TEXTURE_FLAG_NONE,
    NULL, 0, err);
  if (!hw->vram_readback_texture)
    return false;

  /* Async-PBO download target for the VRAM readback path.  Sized
   * to the readback RT (VRAM_WIDTH/2 × VRAM_HEIGHT) so any in-bounds extract
   * fits without resizing.  When the GL device supports buffer storage this
   * goes through the persistent-mapped PBO path (~3-5x faster than synchronous
   * glReadPixels on a hot loop); otherwise falls back to a CPU buffer that
   * opengl_download_texture_copy_from_texture fills via glReadPixels. */
   hw->vram_readback_download_texture = opengl_download_texture_create(
    (u32)VRAM_WIDTH / 2u, (u32)VRAM_HEIGHT, VRAM_RT_FORMAT, 
    /*memory=*/NULL, /*memory_size=*/0, /*memory_pitch=*/0, err);
  if (!hw->vram_readback_download_texture) {
    /* Non-fatal; read_vram falls back to the synchronous glReadPixels
     * path.  Clear the err so the caller doesn't bail. */
    WARNING_LOG("Failed to allocate VRAM readback PBO download texture; using synchronous glReadPixels fallback.");
    Error_clear(err);
  }

  const gpu_device_features_t* feat = opengl_device_get_features();
  if (feat && feat->texture_buffers) {
    hw->vram_upload_buffer = opengl_texture_buffer_create(
      GPU_TEXTURE_BUFFER_FORMAT_R16UI, GPU_DEVICE_MIN_TEXEL_BUFFER_ELEMENTS, err);
    if (!hw->vram_upload_buffer)
      return false;
  } else {
    WARNING_LOG("Texture buffers not supported by GL device - VRAM update GL path disabled "
                "(SW mirror remains authoritative).");
  }

  /* Display extract path: RT sized to max display × resolution_scale, plus
   * an async PBO download for vram_texture -> hw->display_buffer (RGBA8). */
  const u32 ext_w = (u32)GPU_MAX_DISPLAY_WIDTH  * (u32)hw->resolution_scale;
  const u32 ext_h = (u32)GPU_MAX_DISPLAY_HEIGHT * (u32)hw->resolution_scale;
  hw->vram_extract_texture = opengl_texture_create(
    ext_w, ext_h, 1, 1, 1,
    GPU_TEXTURE_TYPE_RENDER_TARGET, VRAM_RT_FORMAT, GPU_TEXTURE_FLAG_NONE,
    NULL, 0, err);
  if (!hw->vram_extract_texture)
    return false;
  hw->vram_extract_capacity_w = ext_w;
  hw->vram_extract_capacity_h = ext_h;

  /* Async PBO ring (N=3) for the display extract.  Each slot is a
   * persistent-mapped PBO sized to the worst-case extract region (ext_w x
   * ext_h).  On drivers without GL_ARB_buffer_storage the helper falls back
   * to a per-frame map, which still avoids the worst of the stall because the
   * fence wait is delayed N-1 frames.  Fall back gracefully to a single-slot
   * layout if any allocation fails - we still avoid the synchronous wait by
   * skipping that frame's GL extract (caller drops to SW row converter). */
  for (u32 i = 0; i < GPU_HW_EXTRACT_RING_N; i++) {
    hw->vram_extract_download_ring[i] = opengl_download_texture_create(
      ext_w, ext_h, VRAM_RT_FORMAT,
      /*memory=*/NULL, /*memory_size=*/0, /*memory_pitch=*/0, err);
    if (!hw->vram_extract_download_ring[i]) {
      WARNING_LOG("Failed to allocate display extract PBO ring slot %u; ring disabled.", i);
      Error_clear(err);
      /* Tear down already-created ring slots so the consumer treats the ring
       * as off (use_software_renderer_for_readbacks default still works). */
      for (u32 j = 0; j < i; j++) {
        opengl_download_texture_destroy(hw->vram_extract_download_ring[j]);
        hw->vram_extract_download_ring[j] = NULL;
      }
      break;
    }
    hw->vram_extract_ring_pending[i] = false;
    hw->vram_extract_ring_w[i] = 0;
    hw->vram_extract_ring_h[i] = 0;
  }
  hw->vram_extract_ring_index = 0;
  hw->vram_extract_last_valid = false;
  hw->vram_extract_prev_disabled = false;
  memset(&hw->vram_extract_last_issued, 0, sizeof(hw->vram_extract_last_issued));
  /* Keep the legacy single-shot pointer pointing at slot 0 so existing
   * branches (e.g. capability checks) still see a valid PBO when the ring is
   * up.  The synchronous fast-path is no longer used at runtime. */
  hw->vram_extract_download_texture = hw->vram_extract_download_ring[0];

  /* Heap display_buffer sized for max display × scale².  Frontend reads
   * (pixels, width, height) and presents at the reported dims, so this
   * scales naturally up to whatever resolution_scale was selected. */
  hw->display_buffer_capacity = ext_w * ext_h;
  hw->display_buffer = (u32*)calloc(hw->display_buffer_capacity, sizeof(u32));
  if (!hw->display_buffer) {
    Error_set_string(err, "Out of memory allocating gpu_hw display_buffer.");
    return false;
  }

  INFO_LOG("Created HW framebuffer of %ux%u", texture_width, texture_height);
  return true;
}

static void gpu_hw_destroy_buffers(gpu_hw_t* hw)
{
  if (hw->display_buffer)        { free(hw->display_buffer); hw->display_buffer = NULL; hw->display_buffer_capacity = 0; }
  if (hw->vram_upload_buffer)    { opengl_texture_buffer_destroy(hw->vram_upload_buffer); hw->vram_upload_buffer = NULL; }
  /* The legacy single-shot pointer is just an alias for ring slot 0, so
   * NULL it without freeing - the ring teardown below owns the lifetime. */
  hw->vram_extract_download_texture = NULL;
  for (u32 i = 0; i < GPU_HW_EXTRACT_RING_N; i++) {
    if (hw->vram_extract_download_ring[i]) {
      /* Drain the in-flight fence first so the GL teardown order is well
       * defined.  destroy() also calls glDeleteSync, but the explicit flush
       * keeps the wait at predictable resize/shutdown time. */
      if (hw->vram_extract_ring_pending[i]) {
        opengl_download_texture_flush(hw->vram_extract_download_ring[i]);
        hw->vram_extract_ring_pending[i] = false;
      }
      opengl_download_texture_destroy(hw->vram_extract_download_ring[i]);
      hw->vram_extract_download_ring[i] = NULL;
    }
    hw->vram_extract_ring_w[i] = 0;
    hw->vram_extract_ring_h[i] = 0;
  }
  hw->vram_extract_ring_index = 0;
  hw->vram_extract_last_valid = false;
  hw->vram_extract_prev_disabled = false;
  memset(&hw->vram_extract_last_issued, 0, sizeof(hw->vram_extract_last_issued));
  if (hw->vram_extract_texture)  { opengl_texture_destroy(hw->vram_extract_texture);  hw->vram_extract_texture  = NULL; }
  if (hw->vram_readback_download_texture) {
    opengl_download_texture_destroy(hw->vram_readback_download_texture);
    hw->vram_readback_download_texture = NULL;
  }
  if (hw->vram_readback_texture) { opengl_texture_destroy(hw->vram_readback_texture); hw->vram_readback_texture = NULL; }
  if (hw->vram_read_texture)     { opengl_texture_destroy(hw->vram_read_texture);     hw->vram_read_texture     = NULL; }
  if (hw->vram_depth_texture)    { opengl_texture_destroy(hw->vram_depth_texture);    hw->vram_depth_texture    = NULL; }
  if (hw->vram_texture)          { opengl_texture_destroy(hw->vram_texture);          hw->vram_texture          = NULL; }
}

static gpu_hw_shadergen_t s_shadergen_state;
static bool s_shadergen_inited = false;

static gpu_hw_shadergen_t* gpu_hw_get_shadergen(void)
{
  if (!s_shadergen_inited) {
    const gpu_render_api_t api = opengl_device_is_gles() ? GPU_RENDER_API_OPENGL_ES : GPU_RENDER_API_OPENGL;
    const gpu_device_features_t* feat = opengl_device_get_features();
    gpu_hw_shadergen_init(&s_shadergen_state, api, feat ? feat->dual_source_blend : false,
                          feat ? feat->framebuffer_fetch : false);
    s_shadergen_inited = true;
  }
  return &s_shadergen_state;
}

static gpu_shader_t* compile_shader_str(gpu_shader_stage_t stage, char* src, Error* err)
{
  if (!src) { Error_set_string(err, "Shader generator returned NULL."); return NULL; }
  const gpu_shader_language_t lang = opengl_device_is_gles() ? GPU_SHADER_LANGUAGE_GLSL_ES
                                                              : GPU_SHADER_LANGUAGE_GLSL;
  gpu_shader_t* sh = opengl_device_create_shader_from_source(stage, lang, src, strlen(src), "main", err);
  free(src);
  return sh;
}

static bool gpu_hw_compile_common_shaders(gpu_hw_t* hw, Error* err)
{
  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();

  hw->fullscreen_quad_vertex_shader = (opengl_shader_t*)compile_shader_str(
    GPU_SHADER_STAGE_VERTEX,
    shadergen_generate_screen_quad_vertex_shader(&sg->base, 1.0f),
    err);
  if (!hw->fullscreen_quad_vertex_shader)
    return false;

  hw->screen_quad_vertex_shader = (opengl_shader_t*)compile_shader_str(
    GPU_SHADER_STAGE_VERTEX,
    gpu_hw_shadergen_generate_screen_vertex_shader(sg),
    err);
  if (!hw->screen_quad_vertex_shader)
    return false;

  return true;
}

/* Skeleton config used by all VRAM-op pipelines.  Shared across fill/copy/write
 * with per-call overrides on shader + depth/blend. */
static void gpu_hw_init_screen_quad_config(gpu_hw_t* hw, gpu_pipeline_graphics_config_t* cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->input_layout.vertex_attributes      = NULL;
  cfg->input_layout.vertex_attribute_count = 0;
  cfg->input_layout.vertex_stride          = 0;
  cfg->vertex_shader   = (gpu_shader_t*)hw->screen_quad_vertex_shader;
  cfg->fragment_shader = NULL;
  cfg->geometry_shader = NULL;
  cfg->layout          = GPU_PIPELINE_LAYOUT_SINGLE_TEXTURE_AND_PUSH_CONSTANTS;
  cfg->primitive       = GPU_PIPELINE_PRIMITIVE_TRIANGLES;
  cfg->rasterization   = gpu_rasterization_state_no_cull(hw->multisamples, false);
  cfg->blend           = gpu_blend_state_no_blending();
  cfg->depth           = gpu_depth_state_no_tests();
  cfg->color_formats[0] = VRAM_RT_FORMAT;
  cfg->color_formats[1] = GPU_TEXTURE_FORMAT_UNKNOWN;
  cfg->depth_format    = GPU_TEXTURE_FORMAT_UNKNOWN;  /* no depth buffer */
  cfg->render_pass_flags = GPU_PIPELINE_RENDER_PASS_NONE;
}

static bool gpu_hw_compile_vram_fill_pipelines(gpu_hw_t* hw, Error* err)
{
  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();
  gpu_pipeline_graphics_config_t cfg;
  gpu_hw_init_screen_quad_config(hw, &cfg);
  if (hw->write_mask_as_depth) {
    cfg.depth_format = VRAM_DS_FORMAT;
    cfg.depth        = gpu_depth_state_always_write();
  }

  for (u32 wrapped = 0; wrapped < 2u; wrapped++) {
    for (u32 interlaced = 0; interlaced < 2u; interlaced++) {
      gpu_shader_t* fs = compile_shader_str(
        GPU_SHADER_STAGE_FRAGMENT,
        gpu_hw_shadergen_generate_vram_fill_fragment_shader(sg, wrapped != 0u, interlaced != 0u,
                                                            /*write_mask_as_depth=*/hw->write_mask_as_depth,
                                                            /*write_depth_as_rt=*/false),
        err);
      if (!fs) {
        Error_add_prefix(err, "VRAM fill FS compile: ");
        return false;
      }
      cfg.fragment_shader = fs;
      gpu_pipeline_t* p = opengl_device_create_pipeline(&cfg, err);
      /* Release the fragment shader after pipeline link.  The linked
       * program retains the compiled stage; the GLSL shader object is no
       * longer needed.  opengl_shader_destroy NULL-tolerant. */
      opengl_shader_destroy((opengl_shader_t*)fs);
      if (!p) {
        Error_add_prefix(err, "VRAM fill pipeline create: ");
        return false;
      }
      hw->vram_fill_pipelines[wrapped][interlaced] = (opengl_pipeline_t*)p;
    }
  }
  return true;
}

static bool gpu_hw_compile_vram_write_pipelines(gpu_hw_t* hw, Error* err)
{
  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();
  gpu_pipeline_graphics_config_t cfg;
  gpu_hw_init_screen_quad_config(hw, &cfg);
  cfg.layout = GPU_PIPELINE_LAYOUT_SINGLE_TEXTURE_BUFFER_AND_PUSH_CONSTANTS;
  if (hw->write_mask_as_depth)
    cfg.depth_format = VRAM_DS_FORMAT;

  gpu_shader_t* fs = compile_shader_str(
    GPU_SHADER_STAGE_FRAGMENT,
    gpu_hw_shadergen_generate_vram_write_fragment_shader(sg, /*use_buffer=*/true, /*use_ssbo=*/false,
                                                         /*write_mask_as_depth=*/hw->write_mask_as_depth,
                                                         /*write_depth_as_rt=*/false),
    err);
  if (!fs) { Error_add_prefix(err, "VRAM write FS compile: "); return false; }
  cfg.fragment_shader = fs;

  /* Slot 0 = no depth-test (ALWAYS), slot 1 = check_mask (GREATER_OR_EQUAL).
   * The slot-1 pipeline is only built when write_mask_as_depth is on; the
   * legacy single-slot behaviour stays intact when WMD is off. */
  for (u32 depth_test = 0u; depth_test < 2u; depth_test++) {
    if (depth_test != 0u && !hw->write_mask_as_depth)
      continue;
    if (hw->write_mask_as_depth) {
      gpu_depth_state_t ds = {0};
      ds.bits.depth_test  = (depth_test != 0u) ? GPU_DEPTH_FUNC_GREATER_EQUAL : GPU_DEPTH_FUNC_ALWAYS;
      ds.bits.depth_write = true;
      cfg.depth = ds;
    }
    gpu_pipeline_t* p = opengl_device_create_pipeline(&cfg, err);
    if (!p) { opengl_shader_destroy((opengl_shader_t*)fs);
              Error_add_prefix(err, "VRAM write pipeline create: "); return false; }
    hw->vram_write_pipelines[depth_test] = (opengl_pipeline_t*)p;
  }
  opengl_shader_destroy((opengl_shader_t*)fs);   /* Post-link cleanup. */
  return true;
}

static bool gpu_hw_compile_vram_copy_pipelines(gpu_hw_t* hw, Error* err)
{
  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();
  gpu_pipeline_graphics_config_t cfg;
  gpu_hw_init_screen_quad_config(hw, &cfg);
  if (hw->write_mask_as_depth)
    cfg.depth_format = VRAM_DS_FORMAT;

  gpu_shader_t* fs = compile_shader_str(
    GPU_SHADER_STAGE_FRAGMENT,
    gpu_hw_shadergen_generate_vram_copy_fragment_shader(sg, /*write_mask_as_depth=*/hw->write_mask_as_depth,
                                                        /*write_depth_as_rt=*/false),
    err);
  if (!fs) { Error_add_prefix(err, "VRAM copy FS compile: "); return false; }
  cfg.fragment_shader = fs;

  for (u32 depth_test = 0u; depth_test < 2u; depth_test++) {
    if (depth_test != 0u && !hw->write_mask_as_depth)
      continue;
    if (hw->write_mask_as_depth) {
      gpu_depth_state_t ds = {0};
      ds.bits.depth_test  = (depth_test != 0u) ? GPU_DEPTH_FUNC_GREATER_EQUAL : GPU_DEPTH_FUNC_ALWAYS;
      ds.bits.depth_write = true;
      cfg.depth = ds;
    }
    gpu_pipeline_t* p = opengl_device_create_pipeline(&cfg, err);
    if (!p) { opengl_shader_destroy((opengl_shader_t*)fs);
              Error_add_prefix(err, "VRAM copy pipeline create: "); return false; }
    hw->vram_copy_pipelines[depth_test] = (opengl_pipeline_t*)p;
  }
  opengl_shader_destroy((opengl_shader_t*)fs);   /* Post-link cleanup. */
  return true;
}

static bool gpu_hw_compile_vram_extract_pipelines(gpu_hw_t* hw, Error* err)
{
  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();
  gpu_pipeline_graphics_config_t cfg;
  gpu_hw_init_screen_quad_config(hw, &cfg);
  cfg.vertex_shader = (gpu_shader_t*)hw->fullscreen_quad_vertex_shader;
  cfg.input_layout.vertex_attributes      = NULL;
  cfg.input_layout.vertex_attribute_count = 0;
  cfg.input_layout.vertex_stride          = 0;

  gpu_shader_t* fs = compile_shader_str(
    GPU_SHADER_STAGE_FRAGMENT,
    gpu_hw_shadergen_generate_vram_extract_fragment_shader(sg, /*resolution_scale=*/1u,
                                                           /*multisamples=*/1u,
                                                           /*color_24bit=*/false,
                                                           /*depth_buffer=*/false),
    err);
  if (!fs) { Error_add_prefix(err, "VRAM extract FS compile: "); return false; }
  cfg.fragment_shader = fs;
  gpu_pipeline_t* p = opengl_device_create_pipeline(&cfg, err);
  opengl_shader_destroy((opengl_shader_t*)fs);   /* Post-link cleanup. */
  if (!p) { Error_add_prefix(err, "VRAM extract pipeline create: "); return false; }
  hw->vram_extract_pipeline[0] = (opengl_pipeline_t*)p;
  return true;
}

static gpu_vertex_attribute_t s_batch_vertex_attrs_untextured[2];
static gpu_vertex_attribute_t s_batch_vertex_attrs_textured[4];
static gpu_vertex_attribute_t s_batch_vertex_attrs_textured_uvlimits[5];
static bool                   s_batch_vertex_attrs_inited = false;

static void gpu_hw_init_batch_vertex_attrs(void)
{
  if (s_batch_vertex_attrs_inited) return;
  s_batch_vertex_attrs_untextured[0] =
    gpu_vertex_attribute_make(0, GPU_VERTEX_SEMANTIC_POSITION, 0, GPU_VERTEX_TYPE_FLOAT,  4, 0);
  s_batch_vertex_attrs_untextured[1] =
    gpu_vertex_attribute_make(1, GPU_VERTEX_SEMANTIC_COLOR,    0, GPU_VERTEX_TYPE_UNORM8, 4, 16);

  s_batch_vertex_attrs_textured[0] = s_batch_vertex_attrs_untextured[0];
  s_batch_vertex_attrs_textured[1] = s_batch_vertex_attrs_untextured[1];
  s_batch_vertex_attrs_textured[2] =
    gpu_vertex_attribute_make(2, GPU_VERTEX_SEMANTIC_TEXCOORD, 0, GPU_VERTEX_TYPE_UINT32, 1, 24);
  s_batch_vertex_attrs_textured[3] =
    gpu_vertex_attribute_make(3, GPU_VERTEX_SEMANTIC_TEXCOORD, 1, GPU_VERTEX_TYPE_UINT32, 1, 20);

  s_batch_vertex_attrs_textured_uvlimits[0] = s_batch_vertex_attrs_textured[0];
  s_batch_vertex_attrs_textured_uvlimits[1] = s_batch_vertex_attrs_textured[1];
  s_batch_vertex_attrs_textured_uvlimits[2] = s_batch_vertex_attrs_textured[2];
  s_batch_vertex_attrs_textured_uvlimits[3] = s_batch_vertex_attrs_textured[3];
  s_batch_vertex_attrs_textured_uvlimits[4] =
    gpu_vertex_attribute_make(4, GPU_VERTEX_SEMANTIC_TEXCOORD, 2, GPU_VERTEX_TYPE_UNORM8, 4, 28);

  s_batch_vertex_attrs_inited = true;
}

/* Encode the texture_mode dim into the (textured, palette, page_texture, sprite)
 * VS-cache key.  Returns one of the 4 cache-slots [0..3].  Sprite is forced
 * off (allow_sprite_mode=false), but the slot mapping must still match
 * the *FS's* internal palette/page_texture detection so the VertexData
 * interface block agrees at link time.  cupid-ps1's gpu_hw_shadergen.c FS
 * currently treats SPRITE_* the same as DISABLED/non-page (palette=false,
 * page=false); TODO: pull SPRITE_PAGE_TEXTURE into page_texture
 * detection. */
static u32 gpu_hw_batch_vs_slot(gpu_hw_batch_texture_mode_t tm, bool uv_limits)
{
  switch (tm) {
    case GPU_HW_BATCH_TEXTURE_MODE_DISABLED:           return 0u;
    case GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT:
    case GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT:       return uv_limits ? 4u : 1u;
    case GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT:       return uv_limits ? 5u : 2u;
    case GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE:       return uv_limits ? 6u : 3u;
    /* Sprite slots: the FS treats them as non-page/non-palette in cupid-ps1,
     * so use the direct16 VS for VertexData parity.  No sprite draws yet
     * (allow_sprite_mode=false); pipelines exist but stay unused. */
    case GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PALETTE_4BIT:
    case GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PALETTE_8BIT:
    case GPU_HW_BATCH_TEXTURE_MODE_SPRITE_DIRECT_16BIT:
    case GPU_HW_BATCH_TEXTURE_MODE_SPRITE_PAGE_TEXTURE: return uv_limits ? 5u : 2u;
    default: return 0u;
  }
}

/* VS cache layout:
 *   0 untextured              (textured=0, palette=0, page=0, uv_limits=0)
 *   1 palette                 (textured=1, palette=1, page=0, uv_limits=0)
 *   2 direct16                (textured=1, palette=0, page=0, uv_limits=0)
 *   3 page_tex                (textured=1, palette=0, page=1, uv_limits=0)
 *   4 palette + uv_limits     (textured=1, palette=1, page=0, uv_limits=1)
 *   5 direct16 + uv_limits    (textured=1, palette=0, page=0, uv_limits=1)
 *   6 page_tex + uv_limits    (textured=1, palette=0, page=1, uv_limits=1)
 * Slots 4-6 are required for the BILINEAR pass: when the FS does texture
 * filtering it expects `v_uv_limits` as a fragment input, which the VS only
 * forwards when `uv_limits=true`.  Non-NEAREST filters always clamp UVs. */
#define GPU_HW_VS_CACHE_SLOTS 7u
static bool gpu_hw_compile_batch_vertex_shaders(gpu_hw_t* hw, opengl_shader_t* slots[GPU_HW_VS_CACHE_SLOTS], Error* err)
{
  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();
  static const struct { bool textured, palette, page, uv_limits; } kCfgs[GPU_HW_VS_CACHE_SLOTS] = {
    { false, false, false, false }, /* 0 untextured                 */
    { true,  true,  false, false }, /* 1 palette                    */
    { true,  false, false, false }, /* 2 direct16                   */
    { true,  false, true,  false }, /* 3 page_tex                   */
    { true,  true,  false, true  }, /* 4 palette + uv_limits        */
    { true,  false, false, true  }, /* 5 direct16 + uv_limits       */
    { true,  false, true,  true  }, /* 6 page_tex + uv_limits       */
  };
  for (u32 i = 0; i < GPU_HW_VS_CACHE_SLOTS; i++) {
    char* src = gpu_hw_shadergen_generate_batch_vertex_shader(
      sg, /*upscaled=*/false, /*msaa=*/false, /*per_sample_shading=*/false,
      kCfgs[i].textured, kCfgs[i].palette, kCfgs[i].page,
      /*uv_limits=*/kCfgs[i].uv_limits, /*force_round_texcoords=*/false,
      /*pgxp_depth=*/false, /*disable_color_perspective=*/false);
    slots[i] = (opengl_shader_t*)compile_shader_str(GPU_SHADER_STAGE_VERTEX, src, err);
    if (!slots[i]) {
      Error_add_prefix(err, "Batch VS compile: ");
      return false;
    }
  }
  (void)hw;
  return true;
}

/* Returns true for filters that mix between source texels (Bilinear, JINC2,
 * xBR; in their non-bin-alpha forms) and so require dual-source blend
 * support to merge the alpha-weighted samples coherently.  The bin-alpha
 * variants round the alpha to 0/1 inside the FS, so they don't need the
 * blended path.  Filter values >= Scale2x always return false. */
static bool gpu_hw_is_blended_texture_filtering(gpu_texture_filter_t filter)
{
  return (filter == GPU_TEXTURE_FILTER_BILINEAR ||
          filter == GPU_TEXTURE_FILTER_JINC2    ||
          filter == GPU_TEXTURE_FILTER_XBR);
}

/* Build the blend-state for a (transparency, render_mode, textured)
 * tuple.  Mirrors the blend-state matrix.
 *
 * cupid-ps1 forces `use_shader_blending=false` and `use_rov=false`.  We
 * also fold in the `is_blended_texture_filtering` half of the condition: for
 * textured draws that use a blended sprite filter (Bilinear / JINC2 / xBR
 *; the non-bin-alpha variants), blending is enabled even without
 * a transparency mode so the alpha-weighted samples can be merged into the
 * destination via dual-source blend (or the constant-color fallback when
 * dual-source isn't supported).  Match that here.
 *
 * The 4 PS1 transparency modes from the GP0 register
 * (gpu_transparency_mode_t):
 *   0 HALF_BG_PLUS_HALF_FG      B/2 + F/2
 *   1 BG_PLUS_FG                B + F
 *   2 BG_MINUS_FG               B - F
 *   3 BG_PLUS_QUARTER_FG        B + F/4
 *
 * The blend matrix:
 *   src_alpha = ONE,  dst_alpha = ZERO,  alpha_op = ADD
 *   if dual_source_blend:
 *     src = ONE, dst = SrcAlpha1 (dual-source secondary), op = ADD or REV_SUB.
 *   else (no dual-source):
 *     src = ONE, dst = ONE,
 *       except mode 0 (half/half) where dst = ConstantColor(0x00808080).
 *     op = ADD or REV_SUB.
 *
 * The shader controls the per-mode src factor halving via
 * u_src_alpha_factor + u_dst_alpha_factor (set per draw, see
 * `transparent_alpha[4][2]` in PrepareDraw; modes 0/3 use
 * 0.5/0.5 and 0.25/1.0 respectively, modes 1/2 use 1/1). */
static gpu_blend_state_t gpu_hw_make_batch_blend_state(gpu_hw_t* hw, u32 transparency_mode,
                                                       u32 render_mode, bool textured,
                                                       bool is_blended_texture_filtering)
{
  gpu_blend_state_t bs = gpu_blend_state_no_blending();

  /* Gating: blend is enabled when (a) a real transparency
   * mode is in play and render_mode isn't TRANSPARENCY_DISABLED/ONLY_OPAQUE,
   * or (b) the draw is textured and the active filter is a blended one
   * (Bilinear / JINC2 / xBR without bin-alpha).  In our 7-D matrix
   * we collapse render_mode to {DISABLED, T_AND_O}, so render_mode==1
   * means transparent draws can hit. */
  const bool transp_active =
    (transparency_mode != (u32)GPU_TRANSPARENCY_MODE_DISABLED) && (render_mode != 0u);
  const bool blended_filter_active = (textured && is_blended_texture_filtering);
  if (!transp_active && !blended_filter_active)
    return bs;

  bs.bits.enable          = true;
  bs.bits.src_alpha_blend = GPU_BLEND_FUNC_ONE;
  bs.bits.dst_alpha_blend = GPU_BLEND_FUNC_ZERO;
  bs.bits.alpha_blend_op  = GPU_BLEND_OP_ADD;

  const bool reverse_subtract =
    (transparency_mode == (u32)GPU_TRANSPARENCY_MODE_BG_MINUS_FG);

  if (hw->supports_dual_source_blend) {
    bs.bits.src_blend = GPU_BLEND_FUNC_ONE;
    bs.bits.dst_blend = GPU_BLEND_FUNC_SRC_ALPHA1;
    bs.bits.blend_op  = reverse_subtract ? GPU_BLEND_OP_REVERSE_SUBTRACT : GPU_BLEND_OP_ADD;
  } else {
    bs.bits.src_blend = GPU_BLEND_FUNC_ONE;
    bs.bits.dst_blend = GPU_BLEND_FUNC_ONE;
    if (transparency_mode == (u32)GPU_TRANSPARENCY_MODE_HALF_BG_PLUS_HALF_FG) {
      bs.bits.dst_blend       = GPU_BLEND_FUNC_CONSTANT_COLOR;
      bs.bits.dst_alpha_blend = GPU_BLEND_FUNC_CONSTANT_COLOR;
      bs.bits.constant        = 0x00808080u;
    }
    bs.bits.blend_op = reverse_subtract ? GPU_BLEND_OP_REVERSE_SUBTRACT : GPU_BLEND_OP_ADD;
  }
  return bs;
}

static bool gpu_hw_compile_batch_pipelines(gpu_hw_t* hw, Error* err)
{
  gpu_hw_init_batch_vertex_attrs();

  gpu_hw_shadergen_t* sg = gpu_hw_get_shadergen();
  opengl_shader_t* vs_cache[GPU_HW_VS_CACHE_SLOTS] = { 0 };
  if (!gpu_hw_compile_batch_vertex_shaders(hw, vs_cache, err))
    return false;

  gpu_pipeline_graphics_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.input_layout.vertex_stride          = (u32)sizeof(gpu_hw_batch_vertex_t);
  cfg.input_layout.vertex_attributes      = s_batch_vertex_attrs_untextured;
  cfg.input_layout.vertex_attribute_count =
    (u32)(sizeof(s_batch_vertex_attrs_untextured) / sizeof(s_batch_vertex_attrs_untextured[0]));
  cfg.geometry_shader  = NULL;
  cfg.layout           = GPU_PIPELINE_LAYOUT_SINGLE_TEXTURE_AND_UBO;
  cfg.primitive        = GPU_PIPELINE_PRIMITIVE_TRIANGLES;
  cfg.rasterization    = gpu_rasterization_state_no_cull(hw->multisamples, /*per_sample=*/false);
  cfg.blend            = gpu_blend_state_no_blending();
  cfg.depth            = gpu_depth_state_no_tests();
  cfg.color_formats[0] = VRAM_RT_FORMAT;
  cfg.color_formats[1] = GPU_TEXTURE_FORMAT_UNKNOWN;
  cfg.depth_format     = hw->write_mask_as_depth ? VRAM_DS_FORMAT : GPU_TEXTURE_FORMAT_UNKNOWN;
  cfg.render_pass_flags = GPU_PIPELINE_RENDER_PASS_NONE;

  u32 compiled = 0u;
  u32 skipped  = 0u;

  /* Outer pass over the active texture filters.  Pass 0 always
   * compiles NEAREST into `hw->batch_pipelines[]` (the canonical "is this
   * combo supported" sentinel array, also referenced by emit-side null
   * checks at draw time).  Subsequent passes compile the currently-set
   * non-NEAREST filter(s); at most one for `texture_filtering` and one
   * for `sprite_texture_filtering` (deduped if they match); into
   * `hw->batch_pipelines_filter[FILTER][]`.  Untextured combos in
   * non-NEAREST passes are skipped (the FS gates `TEXTURE_FILTERING` on
   * `textured`, so the untextured FS would be byte-identical to NEAREST,
   * and the flush-time pick falls back to `batch_pipelines[]` for those
   * slots).
   *
   * Compiling all 12 filters eagerly would cost ~165s on Mesa Intel
   * (mostly the MMPXQuality shader, which is ~1000 lines of GLSL); per
   * filter pass is only ~5-10s, so we restrict to the active filter(s).
   * Unused per-filter rows stay NULL and the flush-time fallback path
   * picks `batch_pipelines[]` (NEAREST) if a row entry is NULL.  Live
   * filter toggle is deferred; a settings-change hook will rebuild this
   * pool with the new filter set.  The full
   * `kFilterPasses[GPU_TEXTURE_FILTER_COUNT]` table below documents which
   * filter indices the per-filter row array supports (kept commented as
   * a sanity reference for the rebuild path). */
  static const gpu_texture_filter_t kFilterPasses[GPU_TEXTURE_FILTER_COUNT] = {
    GPU_TEXTURE_FILTER_NEAREST,
    GPU_TEXTURE_FILTER_BILINEAR,
    GPU_TEXTURE_FILTER_BILINEAR_BIN_ALPHA,
    GPU_TEXTURE_FILTER_JINC2,
    GPU_TEXTURE_FILTER_JINC2_BIN_ALPHA,
    GPU_TEXTURE_FILTER_XBR,
    GPU_TEXTURE_FILTER_XBR_BIN_ALPHA,
    GPU_TEXTURE_FILTER_SCALE2X,
    GPU_TEXTURE_FILTER_SCALE3X,
    GPU_TEXTURE_FILTER_MMPX,
    GPU_TEXTURE_FILTER_MMPX_ENHANCED,
    GPU_TEXTURE_FILTER_MMPX_QUALITY,
  };
  gpu_texture_filter_t active_filters[3] = {
    GPU_TEXTURE_FILTER_NEAREST,
    g_settings.gpu_texture_filter,
    g_settings.gpu_sprite_texture_filter,
  };
  u32 num_filter_passes = 1u; /* NEAREST always compiled. */
  for (u32 i = 1u; i < 3u; i++) {
    if ((u32)active_filters[i] >= (u32)GPU_TEXTURE_FILTER_COUNT ||
        active_filters[i] == GPU_TEXTURE_FILTER_NEAREST)
      continue; /* OOB or same as pass 0. */
    bool dup = false;
    for (u32 j = 0u; j < num_filter_passes; j++) {
      if (active_filters[j] == active_filters[i]) { dup = true; break; }
    }
    if (!dup)
      active_filters[num_filter_passes++] = active_filters[i];
  }
  for (u32 filter_pass = 0u; filter_pass < num_filter_passes; filter_pass++) {
    const gpu_texture_filter_t filter = active_filters[filter_pass];
     opengl_pipeline_t** const dest = (filter == GPU_TEXTURE_FILTER_NEAREST)
                                     ? hw->batch_pipelines 
                                     : hw->batch_pipelines_filter[filter];

  for (u32 depth_test = 0; depth_test < GPU_HW_BATCH_PIPELINE_DEPTHS; depth_test++) {
    if (depth_test != 0u) {
      const u32 remaining =
         GPU_HW_BATCH_PIPELINE_TRANSPARENCY_MODES * GPU_HW_BATCH_PIPELINE_RENDER_MODES *
        GPU_HW_BATCH_PIPELINE_TEXTURE_MODES * GPU_HW_BATCH_PIPELINE_DITHERING * 
        GPU_HW_BATCH_PIPELINE_INTERLACING * GPU_HW_BATCH_PIPELINE_CHECK_MASK;
      skipped += remaining;
      continue;
    }
    for (u32 transparency_mode = 0; transparency_mode < GPU_HW_BATCH_PIPELINE_TRANSPARENCY_MODES; transparency_mode++) {
    for (u32 render_mode = 0; render_mode < GPU_HW_BATCH_PIPELINE_RENDER_MODES; render_mode++) {
    for (u32 texture_mode = 0; texture_mode < GPU_HW_BATCH_PIPELINE_TEXTURE_MODES; texture_mode++) {
    for (u32 dithering = 0; dithering < GPU_HW_BATCH_PIPELINE_DITHERING; dithering++) {
    for (u32 interlacing = 0; interlacing < GPU_HW_BATCH_PIPELINE_INTERLACING; interlacing++) {
    for (u32 check_mask = 0; check_mask < GPU_HW_BATCH_PIPELINE_CHECK_MASK; check_mask++) {
      const u32 idx = gpu_hw_batch_pipeline_index(depth_test, transparency_mode, render_mode,
                                                  texture_mode, dithering, interlacing, check_mask);

      const gpu_hw_batch_texture_mode_t tm = (gpu_hw_batch_texture_mode_t)texture_mode;
      const bool textured  = (tm != GPU_HW_BATCH_TEXTURE_MODE_DISABLED);

      /* Non-NEAREST filter passes only emit FS bodies that
       * change relative to NEAREST; i.e. textured combos.  Untextured
       * slots leave the per-filter row NULL; the flush-time array pick
       * for an untextured batch always hits the canonical NEAREST array
       * via the use_filter_row=false fallback (see hw_flush_render). */
      if (filter_pass != 0u && !textured) {
        skipped++;
        continue;
      }

      /* Non-NEAREST filter passes need `uv_limits=true` (FS
       * body references v_uv_limits unconditionally when
       * TEXTURE_FILTERING is on; matches the `ShouldClampUVs(filter)` flag).
       * That forces the 5-attr VS + the 5-attr vertex layout so the
       * v_uv_limits varying is declared and forwarded. */
      const bool fs_uv_limits = (filter_pass != 0u);
      /* Pass `is_blended_texture_filtering` to FS + blend state
       * so the bin-alpha vs blended forms emit/blend correctly.
       * Untextured slots get false here regardless. */
      const bool fs_is_blended_filter =
        textured && gpu_hw_is_blended_texture_filtering(filter);
      const u32 vs_slot       = gpu_hw_batch_vs_slot(tm, fs_uv_limits);
      cfg.vertex_shader       = (gpu_shader_t*)vs_cache[vs_slot];
      if (!textured) {
        cfg.input_layout.vertex_attributes      = s_batch_vertex_attrs_untextured;
        cfg.input_layout.vertex_attribute_count =
          (u32)(sizeof(s_batch_vertex_attrs_untextured) / sizeof(s_batch_vertex_attrs_untextured[0]));
      } else if (fs_uv_limits) {
        cfg.input_layout.vertex_attributes      = s_batch_vertex_attrs_textured_uvlimits;
        cfg.input_layout.vertex_attribute_count =
          (u32)(sizeof(s_batch_vertex_attrs_textured_uvlimits) / sizeof(s_batch_vertex_attrs_textured_uvlimits[0]));
      } else {
        cfg.input_layout.vertex_attributes      = s_batch_vertex_attrs_textured;
        cfg.input_layout.vertex_attribute_count =
          (u32)(sizeof(s_batch_vertex_attrs_textured) / sizeof(s_batch_vertex_attrs_textured[0]));
      }

      /* Decode the dithering slot (5 values) into (dith_enabled,
       * scaled_dithering, true_color, shader_blend) flags.  The 5 slots
       * mirror the established combinations (header comment in gpu_hw.h):
       *   0 Unscaled              (dith=1, sd=0, tc=0, sb=0)
       *   1 UnscaledShaderBlend   (dith=1, sd=0, tc=0, sb=1)
       *   2 Scaled                (dith=1, sd=1, tc=0, sb=0)
       *   3 ScaledShaderBlend     (dith=1, sd=1, tc=0, sb=1)
       *   4 TrueColor             (dith=0, sd=0, tc=1, sb=0)
       * Today only slot 0 (the all-false case) and slot 4 (true_color)
       * are exercised by draws, but we still compile every legal combo. */
      bool fs_dithering, fs_scaled_dith, fs_true_color, fs_shader_blend;
      switch (dithering) {
        case 0: fs_dithering = true;  fs_scaled_dith = false; fs_true_color = false; fs_shader_blend = false; break;
        case 1: fs_dithering = true;  fs_scaled_dith = false; fs_true_color = false; fs_shader_blend = true;  break;
        case 2: fs_dithering = true;  fs_scaled_dith = true;  fs_true_color = false; fs_shader_blend = false; break;
        case 3: fs_dithering = true;  fs_scaled_dith = true;  fs_true_color = false; fs_shader_blend = true;  break;
        case 4: fs_dithering = false; fs_scaled_dith = false; fs_true_color = true;  fs_shader_blend = false; break;
        default: fs_dithering = false; fs_scaled_dith = false; fs_true_color = false; fs_shader_blend = false; break;
      }
      /* Decode interlacing slot (5 values):
       *   0 Disabled              (intl=0, sintl=0)
       *   1 Forced0 / 2 Forced1   (intl=1, sintl=0)
       *   3 Scaled0 / 4 Scaled1   (intl=1, sintl=1) */
      const bool fs_interlacing       = (interlacing != 0u);
      const bool fs_scaled_interlacing = (interlacing >= 3u);

      /* render_mode 0 = TRANSPARENCY_DISABLED, 1 = TRANSPARENT_AND_OPAQUE in our
       * collapsed 2-value slot.  Shader-blend variants are gated off
       * (allow_shader_blend=false in the factory).  Skip those combos: NULL
       * pipeline slot, draw fallback to SW. */
      if (fs_shader_blend && !hw->allow_shader_blend) {
        skipped++;
        continue;
      }
      const gpu_hw_batch_render_mode_t rm = fs_shader_blend
         ? GPU_HW_BATCH_RENDER_MODE_SHADER_BLEND
        : ((render_mode == 0u) ? GPU_HW_BATCH_RENDER_MODE_TRANSPARENCY_DISABLED
                               : GPU_HW_BATCH_RENDER_MODE_TRANSPARENT_AND_OPAQUE);

      /* Honor shadergen assertions:
       *  - transparency != DISABLED requires render_mode == SHADER_BLEND.
       *  - true_color and dithering can't both be on.
       * check_mask without shader_blend used to be skipped here; closed
       * via write_mask_as_depth (depth attachment + depth_func filtering). */
      const gpu_transparency_mode_t shader_trans = fs_shader_blend
        ? (gpu_transparency_mode_t)transparency_mode
        : GPU_TRANSPARENCY_MODE_DISABLED;

      if (fs_true_color && fs_dithering) {
        /* shadergen assertion guard: TRUE_COLOR + DITHERING is illegal. */
        skipped++;
        continue;
      }

      /* WRITE_MASK_AS_DEPTH covers the previously-skipped check_mask=1 (and
       * future PGXP-depth) slots.  Compile with WMD enabled whenever the
       * back-end is in WMD mode; the FS body unconditionally does
       * o_depth = oalpha * v_pos.z when WMD is set, so untextured-opaque
       * pipelines also seed depth properly. */
      const bool fs_write_mask_as_depth = hw->write_mask_as_depth;

       char* fs_src = gpu_hw_shadergen_generate_batch_fragment_shader(
        sg, rm, shader_trans, tm, filter,
        /*is_blended_texture_filtering=*/fs_is_blended_filter, /*upscaled=*/false,
        /*msaa=*/false, /*per_sample_shading=*/false, /*uv_limits=*/fs_uv_limits,
        /*force_round_texcoords=*/false, /*modulation_crop=*/false,
        /*true_color=*/fs_true_color, /*dithering=*/fs_dithering,
        /*scaled_dithering=*/fs_scaled_dith, /*disable_color_perspective=*/false,
        /*interlacing=*/fs_interlacing, /*scaled_interlacing=*/fs_scaled_interlacing,
        /*check_mask=*/(check_mask != 0u), /*write_mask_as_depth=*/fs_write_mask_as_depth,
        /*use_rov=*/false, /*use_rov_depth=*/false,
        /*rov_depth_test=*/false, /*rov_depth_write=*/false);
      gpu_shader_t* fs = compile_shader_str(GPU_SHADER_STAGE_FRAGMENT, fs_src, err);
      if (!fs) {
        Error_add_prefix(err, "Batch FS compile: ");
        for (u32 i = 0; i < GPU_HW_VS_CACHE_SLOTS; i++) opengl_shader_destroy(vs_cache[i]);
        return false;
      }
      cfg.fragment_shader = fs;

      /* Per-mode blend state for the 4 PS1 transparency modes.
       * `textured` and `render_mode` are inputs; see comment on
       * gpu_hw_make_batch_blend_state for the matrix.  We also
       * pass the per-filter blended-filter flag so blended sprite
       * filters (Bilinear/JINC2/xBR) get blending enabled even on opaque
       * draws; the alpha-weighted samples are merged via dual-source
       * blend. */
      cfg.blend = gpu_hw_make_batch_blend_state(hw, transparency_mode, render_mode, textured,
                                                fs_is_blended_filter);

      /* Depth state.  When write_mask_as_depth is on:
       *   - check_mask=0: depth_func=ALWAYS, depth_write=on.  Records the
       *     incoming pixel's mask bit (oalpha * v_pos.z) into depth so a
       *     subsequent check_mask draw can filter against it.
       *   - check_mask=1: depth_func=GREATER_OR_EQUAL, depth_write=on.  The
       *     destination depth carries the previous draw's mask bit (close to 0
       *     when clear, close to 1 when set).  GE filters the incoming write
       *     against destinations whose mask bit was set.
       * When WMD is off the legacy no-test/no-write state stays in place. */
      if (hw->write_mask_as_depth) {
        gpu_depth_state_t ds = {0};
        ds.bits.depth_test = (check_mask != 0u) ? GPU_DEPTH_FUNC_GREATER_EQUAL
                                                : GPU_DEPTH_FUNC_ALWAYS;
        ds.bits.depth_write = true;
        cfg.depth = ds;
      } else {
        cfg.depth = gpu_depth_state_no_tests();
      }

      gpu_pipeline_t* p = opengl_device_create_pipeline(&cfg, err);
      opengl_shader_destroy((opengl_shader_t*)fs);
      if (!p) {
        Error_add_prefix(err, "Batch pipeline create: ");
        for (u32 i = 0; i < GPU_HW_VS_CACHE_SLOTS; i++) opengl_shader_destroy(vs_cache[i]);
        return false;
      }
      dest[idx] = (opengl_pipeline_t*)p;

      compiled++;
      /* 12 filters × ~480 textured slots + 540 NEAREST = ~6300
       * pipelines worst-case; logging every 500 keeps the output legible
       * during the ~10-15s Mesa Intel compile. */
      if ((compiled % 500u) == 0u)
        INFO_LOG("Compiled %u batch pipelines...", compiled);
    } } } } } }
  }
  } /* end filter_pass loop */

  for (u32 i = 0; i < GPU_HW_VS_CACHE_SLOTS; i++)
    opengl_shader_destroy(vs_cache[i]);

  INFO_LOG("Compiled %u batch pipelines (skipped %u, %u filter pass(es) over %u-slot matrix).",
           compiled, skipped, num_filter_passes, (u32)GPU_HW_NUM_BATCH_PIPELINES);
  return true;
}

static void gpu_hw_map_batch_buffers(gpu_hw_t* hw, u32 max_vertices, u32 max_indices)
{
  void* vb_map = NULL;
  u32   vb_space = 0;
  u32   vb_base  = 0;
  opengl_device_map_vertex_buffer((u32)sizeof(gpu_hw_batch_vertex_t), max_vertices,
                                  &vb_map, &vb_space, &vb_base);
  hw->batch_vertex_ptr   = (gpu_hw_batch_vertex_t*)vb_map;
  hw->batch_base_vertex  = vb_base;
  hw->batch_vertex_count = 0;
  hw->batch_vertex_space = (u16)((vb_space < UINT16_MAX) ? vb_space : UINT16_MAX);

  u16* ib_map = NULL;
  u32  ib_space = 0;
  u32  ib_base  = 0;
  opengl_device_map_index_buffer(max_indices, &ib_map, &ib_space, &ib_base);
  hw->batch_index_ptr   = ib_map;
  hw->batch_base_index  = ib_base;
  hw->batch_index_count = 0;
  hw->batch_index_space = (u16)((ib_space < UINT16_MAX) ? ib_space : UINT16_MAX);
}

/* Map batch buffers if they're not currently mapped.  Cheap on the hot path
 * (one branch).  Called by every batch-emit entry point because hw_flush_
 * render no longer auto-remaps; that breaks utility draws against the same
 * VBO.  Returns true on success; false if mapping failed (caller must skip
 * the vertex push to avoid a NULL deref). */
static bool gpu_hw_ensure_batch_buffers_mapped(gpu_hw_t* hw)
{
  if (hw->batch_vertex_ptr) return true;
  gpu_hw_map_batch_buffers(hw, GPU_HW_MAX_BATCH_VERTICES, GPU_HW_MAX_BATCH_INDICES);
  return hw->batch_vertex_ptr != NULL && hw->batch_index_ptr != NULL;
}

static void gpu_hw_unmap_batch_buffers(gpu_hw_t* hw)
{
  if (!hw->batch_vertex_ptr)
    return;
  opengl_device_unmap_vertex_buffer((u32)sizeof(gpu_hw_batch_vertex_t), hw->batch_vertex_count);
  opengl_device_unmap_index_buffer (hw->batch_index_count);
  hw->batch_vertex_ptr   = NULL;
  hw->batch_index_ptr    = NULL;
  hw->batch_vertex_count = 0;
  hw->batch_vertex_space = 0;
  hw->batch_index_count  = 0;
  hw->batch_index_space  = 0;
}

static void gpu_hw_destroy_pipelines(gpu_hw_t* hw)
{
  for (u32 w = 0; w < 2u; w++) {
    for (u32 i = 0; i < 2u; i++) {
      if (hw->vram_fill_pipelines[w][i]) {
        opengl_device_destroy_pipeline((gpu_pipeline_t*)hw->vram_fill_pipelines[w][i]);
        hw->vram_fill_pipelines[w][i] = NULL;
      }
    }
  }
  for (u32 d = 0; d < 2u; d++) {
    if (hw->vram_write_pipelines[d]) {
      opengl_device_destroy_pipeline((gpu_pipeline_t*)hw->vram_write_pipelines[d]);
      hw->vram_write_pipelines[d] = NULL;
    }
    if (hw->vram_copy_pipelines[d]) {
      opengl_device_destroy_pipeline((gpu_pipeline_t*)hw->vram_copy_pipelines[d]);
      hw->vram_copy_pipelines[d] = NULL;
    }
  }
  for (u32 s = 0; s < 3u; s++) {
    if (hw->vram_extract_pipeline[s]) {
      opengl_device_destroy_pipeline((gpu_pipeline_t*)hw->vram_extract_pipeline[s]);
      hw->vram_extract_pipeline[s] = NULL;
    }
  }
  /* Walk the eager-compiled batch pipeline matrix and tear down the
   * per-filter mirror rows (rows 1..11 of `batch_pipelines_filter[]`;
   * row 0 is unused). */
  for (u32 i = 0; i < (u32)GPU_HW_NUM_BATCH_PIPELINES; i++) {
    if (hw->batch_pipelines[i]) {
      opengl_device_destroy_pipeline((gpu_pipeline_t*)hw->batch_pipelines[i]);
      hw->batch_pipelines[i] = NULL;
    }
  }
  for (u32 f = 0; f < (u32)GPU_TEXTURE_FILTER_COUNT; f++) {
    for (u32 i = 0; i < (u32)GPU_HW_NUM_BATCH_PIPELINES; i++) {
      if (hw->batch_pipelines_filter[f][i]) {
        opengl_device_destroy_pipeline((gpu_pipeline_t*)hw->batch_pipelines_filter[f][i]);
        hw->batch_pipelines_filter[f][i] = NULL;
      }
    }
  }
  if (hw->fullscreen_quad_vertex_shader) {
    opengl_shader_destroy(hw->fullscreen_quad_vertex_shader);
    hw->fullscreen_quad_vertex_shader = NULL;
  }
  if (hw->screen_quad_vertex_shader) {
    opengl_shader_destroy(hw->screen_quad_vertex_shader);
    hw->screen_quad_vertex_shader = NULL;
  }
}

/* Wrap-aware VRAM transfer bounds (mirrors GetVRAMTransferBounds).
 * Returns the smallest VRAM-pixel-space rectangle covering the transfer,
 * collapsing to the full row/column when the transfer wraps. */
static gpu_rect_t gpu_hw_vram_transfer_bounds(u32 x, u32 y, u32 width, u32 height)
{
  gpu_rect_t r;
  r.left   = (s32)(x % VRAM_WIDTH);
  r.top    = (s32)(y % VRAM_HEIGHT);
  r.right  = r.left + (s32)width;
  r.bottom = r.top  + (s32)height;
  if (r.right  > (s32)VRAM_WIDTH ) { r.left = 0; r.right  = (s32)VRAM_WIDTH;  }
  if (r.bottom > (s32)VRAM_HEIGHT) { r.top  = 0; r.bottom = (s32)VRAM_HEIGHT; }
  return r;
}

/* Dirty-rect plumbing: any GL VRAM mutation unions its rect here so the
 * next textured batch can blit vram_texture -> vram_read_texture before
 * the FS samples.  Reset by gpu_hw_update_vram_read_texture below. */
static void gpu_hw_mark_vram_dirty(gpu_hw_t* hw, gpu_rect_t r)
{
  hw->vram_dirty_draw_rect = gpu_rect_union(hw->vram_dirty_draw_rect, r);
}

/* Blit dirty area of vram_texture (RT) into vram_read_texture (sample
 * source for textured batches).  Called from hw_flush_render before
 * binding vram_read_texture for a textured batch. */
static void gpu_hw_update_vram_read_texture(gpu_hw_t* hw)
{
  if (!hw->vram_read_texture || !hw->vram_texture)
    return;
  gpu_rect_t r = gpu_rect_intersect(hw->vram_dirty_draw_rect, GPU_HW_VRAM_SIZE_RECT);
  if (gpu_rect_is_empty(r))
    return;
  const u32 scale = (u32)hw->resolution_scale;
  const u32 sx = (u32)r.left * scale;
  const u32 sy = (u32)r.top  * scale;
  const u32 w  = (u32)gpu_rect_width(r)  * scale;
  const u32 h  = (u32)gpu_rect_height(r) * scale;
  opengl_device_copy_texture_region(
    &hw->vram_read_texture->base, sx, sy, 0, 0,
    &hw->vram_texture->base,      sx, sy, 0, 0,
    w, h);
  hw->vram_dirty_draw_rect = gpu_rect_invalid();
}

/* Push-constant block matching the VRAMFill UBO. */
typedef struct {
  u32   u_dst_x;
  u32   u_dst_y;
  u32   u_end_x;
  u32   u_end_y;
  float u_fill_color[4];
  u32   u_interlaced_displayed_field;
  u32   u_pad[3];
} vram_fill_push_data_t;

 /* Push-constant block matching VRAMWriteUBOData.
 * 40 bytes (10 floats/u32). */
typedef struct {
  float u_dst_x;
  float u_dst_y;
  float u_end_x;
  float u_end_y;
  float u_width;
  float u_height;
  float u_resolution_scale;
  u32   u_buffer_base_offset;
  u32   u_mask_or_bits;
  float u_depth_value;
} vram_write_push_data_t;

 /* Push-constant block matching VRAMCopyUBOData.
 * 48 bytes (12 floats/u32). */
typedef struct {
  float u_src_x;
  float u_src_y;
  float u_dst_x;
  float u_dst_y;
  float u_end_x;
  float u_end_y;
  float u_vram_width;
  float u_vram_height;
  float u_resolution_scale;
  u32   u_set_mask_bit;
  float u_depth_value;
  u32   u_pad;
} vram_copy_push_data_t;

static void gpu_hw_fill_vram_gl(gpu_hw_t* hw, u32 x, u32 y, u32 width, u32 height, u32 color,
                                bool interlaced, u8 active_line_lsb)
{
  /* Drain pending polygon batch first so its scissor doesn't leak into our
   * fill, and the batch's queued vertex writes don't reorder past our fill. */
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);
  if (!hw->vram_texture)
    return;
  (void)interlaced; (void)active_line_lsb;   /* Interlace skip approximated; fine for non-interlaced fills. */

  const u32 scale = (u32)hw->resolution_scale;
  const u32 fill_color = hw->true_color
    ? color
    : vram_rgba5551_to_rgba8888(vram_rgba8888_to_rgba5551(color));
  float fc[4];
  gpu_device_rgba8_to_float(fill_color, fc);

  /* Wrap-aware: split into up to 4 sub-rects so each piece fits in VRAM. */
  const u32 vw = (u32)VRAM_WIDTH;
  const u32 vh = (u32)VRAM_HEIGHT;
  const u32 x0 = x % vw;
  const u32 y0 = y % vh;
  const u32 w1 = (x0 + width  > vw) ? (vw - x0) : width;
  const u32 h1 = (y0 + height > vh) ? (vh - y0) : height;
  const u32 w2 = width  - w1;
  const u32 h2 = height - h1;

  struct { u32 sx, sy, sw, sh; } rects[4] = {
    { x0,  y0,  w1, h1 },
    { 0u,  y0,  w2, h1 },
    { x0,  0u,  w1, h2 },
    { 0u,  0u,  w2, h2 },
  };

  /* Bind vram_texture as RT, set viewport to full VRAM, then per-rect
   * scissor + glClear.  glClear with scissor is a stable Mesa primitive
   * and avoids the FS pipeline that was found to silently no-op. */
  gpu_texture_t* rts[1] = { &hw->vram_texture->base };
  gpu_texture_t* ds = hw->vram_depth_texture ? &hw->vram_depth_texture->base : NULL;
  opengl_device_set_render_targets(rts, 1, ds);
  const opengl_rect_t vp = { 0, 0, (s32)(vw * scale), (s32)(vh * scale) };
  opengl_device_set_viewport(vp);

  glEnable(GL_SCISSOR_TEST);
  glClearColor(fc[0], fc[1], fc[2], fc[3]);
  for (u32 i = 0; i < 4; i++) {
    if (rects[i].sw == 0u || rects[i].sh == 0u) continue;
    glScissor((GLint)(rects[i].sx * scale), (GLint)(rects[i].sy * scale),
              (GLsizei)(rects[i].sw * scale), (GLsizei)(rects[i].sh * scale));
    glClear(GL_COLOR_BUFFER_BIT);
  }
  /* Force device scissor cache to re-set on next draw (we bypassed it). */
  opengl_device_set_scissor((opengl_rect_t){ 0, 0, 1, 1 });
  opengl_device_set_scissor(vp);

  gpu_hw_mark_vram_dirty(hw, gpu_hw_vram_transfer_bounds(x, y, width, height));
}

static void gpu_hw_update_vram_gl(gpu_hw_t* hw, u32 x, u32 y, u32 width, u32 height,
                                  const u16* data, bool set_mask, bool check_mask)
{
  /* The FS-based vram_write pipeline path doesn't actually land pixels in
   * vram_texture on Mesa Intel iGPU (writes go nowhere; verified via
   * glReadPixels from vram_texture at upload coords; always 0).  Bypass
   * the FS path and CPU-upload directly via opengl_texture_update, mirroring
   * the working hw_update_clut path.  Convert each native u16 (RGBA5551)
   * to RGBA8 (matching VRAM_RT_FORMAT) and replicate `scale` rows so
   * upscaled samplers see the full block. */
  (void)set_mask; (void)check_mask;   /* mask-bit handled via SW canon for now */
  /* Drain pending polygon batch first.  Without this the batch's queued
   * vertex writes would be re-ordered AFTER our CPU upload (the upload is
   * an immediate glTexSubImage2D, but the polygon flush_render gets
   * deferred until the next config change / overflow / update_display).
   * If a polygon writes to the same VRAM region we're uploading to (e.g.
   * Crash title-screen background polygons that target the texpage area
   * for animated backdrops), the polygon write would clobber our upload.
   * The drain here mirrors upstream gpu_hw.cpp's UpdateVRAM flush. */
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);
  if (!hw->vram_texture)
    return;

  const gpu_rect_t bounds = gpu_hw_vram_transfer_bounds(x, y, width, height);
  const u32 scale = (u32)hw->resolution_scale;

  if (width == 0u || height == 0u)
    return;

  const u32 dst_x = (x % VRAM_WIDTH) * scale;
  const u32 dst_y = (y % VRAM_HEIGHT) * scale;
  const u32 dst_w = width * scale;
  const u32 dst_h = height * scale;

  /* Convert + (when needed) scale the source u16 block into a contiguous
   * RGBA8 buffer, then push in a SINGLE glTexSubImage2D-shaped call.  The
   * row-by-row variant was correct but glTexSubImage2D-stalled per row on
   * Mesa Intel iGPU at upload rates the BIOS hits, starving the audio
   * thread (Stretcher reset spam).  One call with proper pitch is dramatically
   * faster. */
  const u32 num_pixels = dst_w * dst_h;
  u32* rgba = (u32*)malloc((size_t)num_pixels * sizeof(u32));
  if (!rgba)
    return;
  if (scale == 1u) {
    for (u32 i = 0; i < width * height; i++)
      rgba[i] = vram_rgba5551_to_rgba8888((u32)data[i]);
  } else {
    /* Replicate each src pixel scale x scale into dst. */
    for (u32 row = 0; row < height; row++) {
      for (u32 col = 0; col < width; col++) {
        const u32 px = vram_rgba5551_to_rgba8888((u32)data[row * width + col]);
        for (u32 sy = 0; sy < scale; sy++) {
          u32* dst_row = &rgba[(row * scale + sy) * dst_w + col * scale];
          for (u32 sx = 0; sx < scale; sx++)
            dst_row[sx] = px;
        }
      }
    }
  }
  (void)opengl_texture_update(hw->vram_texture, dst_x, dst_y, dst_w, dst_h,
                              rgba, dst_w * (u32)sizeof(u32), 0u, 0u);
  free(rgba);
  gpu_hw_mark_vram_dirty(hw, bounds);
}

static void gpu_hw_copy_vram_gl(gpu_hw_t* hw, u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                u32 width, u32 height, bool set_mask, bool check_mask)
{
  /* The FS-based vram_copy pipeline path doesn't actually land pixels in
   * vram_texture on Mesa Intel iGPU (same root cause as update_vram_gl).
   * Bypass with a direct
   * glCopyImageSubData(vram_texture <- vram_texture).  GL spec allows
   * same-texture copies for non-overlapping regions (UB on overlap; we
   * fall back via vram_read_texture as the source for safety since it's
   * already a full copy of vram_texture from a recent blit). */
  (void)set_mask; (void)check_mask;   /* mask-bit handled via SW canon for now */
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);
  if (!hw->vram_texture || !hw->vram_read_texture)
    return;

  const gpu_rect_t src_bounds = gpu_hw_vram_transfer_bounds(src_x, src_y, width, height);
  const gpu_rect_t dst_bounds = gpu_hw_vram_transfer_bounds(dst_x, dst_y, width, height);
  const u32 scale = (u32)hw->resolution_scale;

  const u32 sx = (src_x % VRAM_WIDTH ) * scale;
  const u32 sy = (src_y % VRAM_HEIGHT) * scale;
  const u32 dx = (dst_x % VRAM_WIDTH ) * scale;
  const u32 dy = (dst_y % VRAM_HEIGHT) * scale;
  const u32 w  = width  * scale;
  const u32 h  = height * scale;
  /* Clamp to texture bounds to avoid OOB on wrap.  Wrap-aware split is
   * deferred; most PSX games copy non-wrap rects. */
  const u32 vw = (u32)VRAM_WIDTH  * scale;
  const u32 vh = (u32)VRAM_HEIGHT * scale;
  const u32 cw = (sx + w > vw || dx + w > vw) ? ((vw - (sx > dx ? sx : dx))) : w;
  const u32 ch = (sy + h > vh || dy + h > vh) ? ((vh - (sy > dy ? sy : dy))) : h;
  if (cw == 0u || ch == 0u)
    return;

  /* Source defaults to vram_texture (the live RT). that's what upstream
   * does (gpu_hw.cpp:3714) when texture_copy_to_self is supported.  Reading
   * from vram_read_texture risks staleness: even with dirty-rect
   * tracking, multiple copy_vram calls in a row can leave a region of
   * vram_read_texture stale (subsequent copy reads it before the next
   * polygon flush re-blits).  Symptom we hit: Crash menu backdrop tile
   * garbage from stale-source self-chained copies.
   *
   * Fall back to vram_read_texture only on src/dst overlap (glCopyImageSubData
   * spec disallows overlapping same-texture copies; behaviour is UB).  In
   * the overlap case we still need vram_read_texture coherent first, so
   * call update_vram_read_texture there. */
  const bool overlaps_with_self = gpu_rect_overlaps(src_bounds, dst_bounds);
  if (overlaps_with_self) {
    gpu_hw_update_vram_read_texture(hw);
    opengl_device_copy_texture_region(
      &hw->vram_texture->base, dx, dy, 0, 0,
      &hw->vram_read_texture->base, sx, sy, 0, 0,
      cw, ch);
  } else {
    opengl_device_copy_texture_region(
      &hw->vram_texture->base, dx, dy, 0, 0,
      &hw->vram_texture->base, sx, sy, 0, 0,
      cw, ch);
  }

  gpu_hw_mark_vram_dirty(hw, dst_bounds);
}

/* Display extract: dispatch vram_extract_pipeline[0|1] into vram_extract_texture
 * at (scaled_w x scaled_h), then PBO-download into `dst_buffer` honoring the
 * destination row stride / offset (handles interlace field write layout).
 *
 * dst_buffer:        target host buffer (RGBA8, packed u32 per pixel).
 * dst_stride_pixels: row stride of dst_buffer (in pixels).
 * dst_x, dst_y:      top-left of the destination region.
 * dst_row_stride:    1 for non-interlaced, 2 for interlaced (write every other row).
 * dst_row_offset:    additive Y offset (field) within the strided write.
 *
 * Returns true on success; false if extract path unavailable (caller falls
 * back to the SW row converter). */
static bool gpu_hw_extract_display_gl(gpu_hw_t* hw,
                                      const gpu_backend_update_display_cmd_t* c,
                                      u32* dst_buffer, u32 dst_stride_pixels,
                                      u32 dst_x, u32 dst_y,
                                      u32 dst_row_stride, u32 dst_row_offset,
                                      u32 dst_max_w, u32 dst_max_h)
{
  if (!hw->vram_extract_texture)
    return false;
  const u32 mode = c->display_24bit ? 1u : 0u;
  if (!hw->vram_extract_pipeline[mode])
    return false;
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);

  /* Upstream forces resolution_scale=1 for 24-bit because the FMV pixel
   * decode is bit-exact at native resolution. */
  const u32 scale     = (u32)hw->resolution_scale;
  const u32 ext_scale = c->display_24bit ? 1u : scale;
  const u32 src_w     = (u32)c->display_vram_width;
  const u32 src_h     = (u32)c->display_vram_height;
  const u32 scaled_w  = src_w * ext_scale;
  const u32 scaled_h  = src_h * ext_scale;
  if (scaled_w == 0 || scaled_h == 0)
    return false;
  if (scaled_w > hw->vram_extract_capacity_w || scaled_h > hw->vram_extract_capacity_h)
    return false;

  const u32 field_off  = (c->interlaced_display_field & c->interlaced_display_interleaved) ? 1u : 0u;
  const u32 vram_off_x = (u32)c->display_vram_left * ext_scale;
  const u32 vram_off_y = (u32)c->display_vram_top * ext_scale + field_off;
  const u32 line_skip  = c->interlaced_display_interleaved ? 2u : 1u;

  struct {
    u32 vram_offset_x;
    u32 vram_offset_y;
    float skip_x;
    float line_skip;
  } uni = { vram_off_x, vram_off_y, 0.0f, (float)line_skip };

  gpu_texture_t* rts[1] = { &hw->vram_extract_texture->base };
  opengl_device_set_render_targets(rts, 1, NULL);
  /* Mirror duckstation gpu_hw.cpp:4109: discard prior tile contents so
   * Mesa/driver-tiled residue can't bleed through outside the freshly
   * drawn (scaled_w x scaled_h) rect.  Called every extract frame because
   * the texture is reused at full ext_w x ext_h capacity. */
  opengl_device_invalidate_render_target(&hw->vram_extract_texture->base);
  opengl_device_set_pipeline((gpu_pipeline_t*)hw->vram_extract_pipeline[mode]);
  opengl_device_set_texture_sampler(0, &hw->vram_texture->base, NULL);

  const opengl_rect_t vp = { 0, 0, (s32)scaled_w, (s32)scaled_h };
  opengl_device_set_viewport(vp);
  opengl_device_set_scissor (vp);
  opengl_device_draw_with_push_constants(3, 0, &uni, sizeof(uni));

  /* Async PBO ring readback.
   *
   * On frame i:
   *   1. Issue copy + fence into ring[i % N], remember (scaled_w, scaled_h)
   *      and the destination scatter parameters at issue time.
   *   2. Read ring[(i+1) % N]; by then the fence is N-1 frames old, so
   *      glClientWaitSync returns immediately and the audio thread is not
   *      stalled by a synchronous Mesa Intel readback path.
   *
   * Falls back to a synchronous glReadPixels into a heap buffer when the
   * ring failed to allocate at create-buffers time.  Returns true if it
   * scattered any pixels into the destination buffer this frame; false if
   * the ring is still cold (first N-1 frames) so the caller drops to the
   * SW row converter for the very first few frames after a renderer
   * (re)create.  This matches upstream's "honest first-N-frames black"
   * behaviour and keeps presentation moving while the ring fills. */
  const u32 N = GPU_HW_EXTRACT_RING_N;
  static int s_no_pbo_ring_cached = -1;
  if (s_no_pbo_ring_cached < 0)
    s_no_pbo_ring_cached = (getenv("CUPID_GPU_NO_PBO_RING") != NULL) ? 1 : 0;
  const u32 ring_have_full = (hw->vram_extract_download_ring[0] != NULL) && !s_no_pbo_ring_cached;
  if (ring_have_full) {
    /* Drain on parameter change.  When dst_stride / interlace field /
     * origin / extract size differ from what's queued in the ring, the
     * pending slots' captured meta would scatter their pixels at the
     * wrong rows/positions of the *current* display_buffer (whose layout
     * is determined by THIS frame's params).  Symptom: horizontal stripes
     * during scene transitions and interlace-field flips.  Recovery:
     * flush every pending slot, reset the ring index, fall back to the
     * SW row converter for this transition frame, and start fresh. */
    bool param_change = false;
    if (!hw->vram_extract_last_valid || hw->vram_extract_prev_disabled) {
      param_change = true;
    } else {
      param_change =
        (hw->vram_extract_last_issued.dst_stride_pixels != dst_stride_pixels) ||
        (hw->vram_extract_last_issued.dst_x             != dst_x)             ||
        (hw->vram_extract_last_issued.dst_y             != dst_y)             ||
        (hw->vram_extract_last_issued.dst_row_stride    != dst_row_stride)    ||
        (hw->vram_extract_last_issued.dst_row_offset    != dst_row_offset)    ||
        (hw->vram_extract_last_issued.dst_max_w         != dst_max_w)         ||
        (hw->vram_extract_last_issued.dst_max_h         != dst_max_h)         ||
        (hw->vram_extract_last_issued.scaled_w          != scaled_w)          ||
        (hw->vram_extract_last_issued.scaled_h          != scaled_h)          ||
        (hw->vram_extract_last_issued.vram_off_x        != vram_off_x)        ||
        (hw->vram_extract_last_issued.vram_off_y        != vram_off_y);
    }
    if (param_change && hw->vram_extract_last_valid) {
      /* Source / dest config changed since the last issue.  Drain pending
       * PBO slots so their stale meta doesn't scatter into this frame's
       * display_buffer, then do a SYNCHRONOUS GL extract for THIS frame
       * (using the current FS dispatch + glReadPixels) so display_buffer
       * gets correct content.  Avoids the SW row converter fallback that
       * caused visible flashing on Crash's intros (where vram_off flips
       * every frame -> drain every frame -> SW frame + warmup BLACK frame
       * alternation).  Cost: one synchronous readback this frame; next
       * frame the ring is fresh and we resume async. */
      static int s_drain_trace = -1;
      if (s_drain_trace < 0)
        s_drain_trace = (getenv("CUPID_TRACE_DISP") != NULL || getenv("CUPID_TRACE") != NULL) ? 1 : 0;
      if (s_drain_trace)
        fprintf(stderr, "[hw extract] drain ring (param change) -> sync extract\n");
      for (u32 i = 0; i < N; i++) {
        if (hw->vram_extract_ring_pending[i]) {
          opengl_download_texture_flush(hw->vram_extract_download_ring[i]);
          hw->vram_extract_ring_pending[i] = false;
        }
      }
      hw->vram_extract_ring_index = 0;
      hw->vram_extract_last_issued.dst_stride_pixels = dst_stride_pixels;
      hw->vram_extract_last_issued.dst_x             = dst_x;
      hw->vram_extract_last_issued.dst_y             = dst_y;
      hw->vram_extract_last_issued.dst_row_stride    = dst_row_stride;
      hw->vram_extract_last_issued.dst_row_offset    = dst_row_offset;
      hw->vram_extract_last_issued.dst_max_w         = dst_max_w;
      hw->vram_extract_last_issued.dst_max_h         = dst_max_h;
      hw->vram_extract_last_issued.scaled_w          = scaled_w;
      hw->vram_extract_last_issued.scaled_h          = scaled_h;
      hw->vram_extract_last_issued.vram_off_x        = vram_off_x;
      hw->vram_extract_last_issued.vram_off_y        = vram_off_y;
      hw->vram_extract_last_valid    = true;
      hw->vram_extract_prev_disabled = false;

      /* Synchronous readback of vram_extract_texture (just drawn above by
       * the FS dispatch) into a temporary buffer, then scatter into
       * display_buffer with the current frame's dst meta. */
      u32* tmp = (u32*)malloc((size_t)scaled_w * scaled_h * sizeof(u32));
      if (!tmp) return false;
      glReadPixels(0, 0, (GLsizei)scaled_w, (GLsizei)scaled_h,
                   GL_RGBA, GL_UNSIGNED_BYTE, tmp);
      const u8* src_base  = (const u8*)tmp;
      const u32 src_pitch = scaled_w * (u32)sizeof(u32);
      const u32 copy_w = (scaled_w < dst_max_w) ? scaled_w : dst_max_w;
      for (u32 row = 0; row < scaled_h; row++) {
        const u32 dest_row = dst_y + row * dst_row_stride + dst_row_offset;
        if (dest_row >= dst_max_h) break;
        const u32* src_row = (const u32*)(src_base + (size_t)row * src_pitch);
        u32* drow = &dst_buffer[(size_t)dest_row * dst_stride_pixels + dst_x];
        memcpy(drow, src_row, (size_t)copy_w * sizeof(u32));
      }
      free(tmp);
      return true;
    }

    const u32 issue_idx = hw->vram_extract_ring_index % N;
    const u32 read_idx  = (hw->vram_extract_ring_index + 1u) % N;

    /* (1) Issue copy + fence into ring[issue_idx]. */
    opengl_download_texture_t* issue_pbo = hw->vram_extract_download_ring[issue_idx];
    /* If a previous issue is still pending in this slot (it should be,
     * once the ring has cycled at least once), drain it before re-issuing
     * so the persistent map is observed coherent and the sync object can
     * be re-created.  glClientWaitSync at this point is non-blocking:
     * the fence is N-1 frames old. */
    if (hw->vram_extract_ring_pending[issue_idx]) {
      opengl_download_texture_flush(issue_pbo);
      hw->vram_extract_ring_pending[issue_idx] = false;
    }
    opengl_download_texture_copy_from_texture(
      issue_pbo, 0, 0,
      hw->vram_extract_texture, 0, 0,
      scaled_w, scaled_h, 0, 0, /*use_transfer_pitch=*/false);
    hw->vram_extract_ring_w[issue_idx] = scaled_w;
    hw->vram_extract_ring_h[issue_idx] = scaled_h;
    hw->vram_extract_ring_pending[issue_idx] = true;
    hw->vram_extract_ring_meta[issue_idx].dst_stride_pixels = dst_stride_pixels;
    hw->vram_extract_ring_meta[issue_idx].dst_x             = dst_x;
    hw->vram_extract_ring_meta[issue_idx].dst_y             = dst_y;
    hw->vram_extract_ring_meta[issue_idx].dst_row_stride    = dst_row_stride;
    hw->vram_extract_ring_meta[issue_idx].dst_row_offset    = dst_row_offset;
    hw->vram_extract_ring_meta[issue_idx].dst_max_w         = dst_max_w;
    hw->vram_extract_ring_meta[issue_idx].dst_max_h         = dst_max_h;

    /* Snapshot for next frame's param-change comparison. */
    hw->vram_extract_last_issued.dst_stride_pixels = dst_stride_pixels;
    hw->vram_extract_last_issued.dst_x             = dst_x;
    hw->vram_extract_last_issued.dst_y             = dst_y;
    hw->vram_extract_last_issued.dst_row_stride    = dst_row_stride;
    hw->vram_extract_last_issued.dst_row_offset    = dst_row_offset;
    hw->vram_extract_last_issued.dst_max_w         = dst_max_w;
    hw->vram_extract_last_issued.dst_max_h         = dst_max_h;
    hw->vram_extract_last_issued.scaled_w          = scaled_w;
    hw->vram_extract_last_issued.scaled_h          = scaled_h;
    hw->vram_extract_last_issued.vram_off_x        = vram_off_x;
    hw->vram_extract_last_issued.vram_off_y        = vram_off_y;
    hw->vram_extract_last_valid    = true;
    hw->vram_extract_prev_disabled = false;

    hw->vram_extract_ring_index++;

    /* (2) Try to consume ring[read_idx].  Cold ring -> fall back to SW
     * scatter for this transition frame.  Returning true here would leave
     * display_buffer at whatever it was cleared to (black), since the
     * `!interlaced` clear at top of hw_update_display blanks it every
     * frame in the force-progressive case.  False -> caller's SW row
     * converter populates display_buffer for the warmup frame. */
    if (!hw->vram_extract_ring_pending[read_idx]) {
      return false;
    }

    opengl_download_texture_t* read_pbo = hw->vram_extract_download_ring[read_idx];
    const u32 read_w = hw->vram_extract_ring_w[read_idx];
    const u32 read_h = hw->vram_extract_ring_h[read_idx];

    /* Wait on the fence.  On the steady-state path the fence has been
     * signaled for ~N-1 frames and ClientWaitSync(0) returns immediately;
     * the worst-case wait happens only when something stalled the GPU
     * upstream, in which case we'd be stalled either way. */
    opengl_download_texture_flush(read_pbo);
    hw->vram_extract_ring_pending[read_idx] = false;

    if (!opengl_download_texture_map(read_pbo, 0, 0, read_w, read_h))
      return true;  /* Map failed; treat as warmup-equivalent (no scatter). */

    const u8* src_base  = read_pbo->base.map_pointer;
    const u32 src_pitch = read_pbo->base.current_pitch;
    if (!src_base) {
      opengl_download_texture_unmap(read_pbo);
      return true;
    }

    /* Scatter rows into dst with stride/offset (handles interlace).  Use
     * the destination metadata captured AT ISSUE TIME for this PBO; the
     * display rectangle may have changed in the N-1 frames since the copy
     * was issued, but the pixels in the PBO correspond to that earlier
     * rectangle, so we scatter using those earlier params. */
    const u32 m_dst_stride_pixels = hw->vram_extract_ring_meta[read_idx].dst_stride_pixels;
    const u32 m_dst_x             = hw->vram_extract_ring_meta[read_idx].dst_x;
    const u32 m_dst_y             = hw->vram_extract_ring_meta[read_idx].dst_y;
    const u32 m_dst_row_stride    = hw->vram_extract_ring_meta[read_idx].dst_row_stride;
    const u32 m_dst_row_offset    = hw->vram_extract_ring_meta[read_idx].dst_row_offset;
    const u32 m_dst_max_w         = hw->vram_extract_ring_meta[read_idx].dst_max_w;
    const u32 m_dst_max_h         = hw->vram_extract_ring_meta[read_idx].dst_max_h;

    /* Bounds-check against the *current* dst_buffer / dst_stride_pixels.
     * The display_buffer capacity is sized for max display × scale² in
     * gpu_hw_create_buffers, so the captured offsets are always within
     * the current buffer's address range so long as the resolution scale
     * has not changed since issue (resize drains the ring). */
    (void)dst_buffer;  /* dst is hw->display_buffer in caller. */
    const u32 copy_w = (read_w < m_dst_max_w) ? read_w : m_dst_max_w;
    for (u32 row = 0; row < read_h; row++) {
      const u32 dest_row = m_dst_y + row * m_dst_row_stride + m_dst_row_offset;
      if (dest_row >= m_dst_max_h) break;
      const u32* src_row = (const u32*)(src_base + (size_t)row * src_pitch);
      u32* dst_row = &dst_buffer[(size_t)dest_row * m_dst_stride_pixels + m_dst_x];
      memcpy(dst_row, src_row, (size_t)copy_w * sizeof(u32));
    }
    opengl_download_texture_unmap(read_pbo);
    return true;
  }

  /* Ring unavailable (allocation failed at create time); fall back to a
   * synchronous glReadPixels into a heap buffer.  This is the same code path
   * as the pre-ring fallback; it stalls but keeps the renderer functional on
   * drivers where PBOs aren't available at all. */
  u32* tmp = (u32*)malloc((size_t)scaled_w * scaled_h * sizeof(u32));
  if (!tmp) return false;
  glReadPixels(0, 0, (GLsizei)scaled_w, (GLsizei)scaled_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
  const u8* src_base = (const u8*)tmp;
  const u32 src_pitch = scaled_w * (u32)sizeof(u32);

  const u32 copy_w = (scaled_w < dst_max_w) ? scaled_w : dst_max_w;
  for (u32 row = 0; row < scaled_h; row++) {
    const u32 dest_row = dst_y + row * dst_row_stride + dst_row_offset;
    if (dest_row >= dst_max_h) break;
    const u32* src_row = (const u32*)(src_base + (size_t)row * src_pitch);
    u32* dst_row = &dst_buffer[(size_t)dest_row * dst_stride_pixels + dst_x];
    memcpy(dst_row, src_row, (size_t)copy_w * sizeof(u32));
  }
  free(tmp);
  return true;
}

static void gpu_hw_read_vram_gl(gpu_hw_t* hw, u32 x, u32 y, u32 width, u32 height)
{
  if (!hw->vram_extract_pipeline[0] || !hw->vram_readback_texture)
    return;
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);

  gpu_rect_t copy_rect = gpu_hw_vram_transfer_bounds(x, y, width, height);
  /* Align to even pixel for 32-bit packing (each output texel encodes 2 src). */
  if (copy_rect.left  & 1) copy_rect.left--;
  if (copy_rect.right & 1) copy_rect.right++;

  const u32 encoded_width  = (u32)(copy_rect.right - copy_rect.left) / 2u;
  const u32 encoded_height = (u32)(copy_rect.bottom - copy_rect.top);
  const s32 uniforms[4] = { copy_rect.left, copy_rect.top,
                            copy_rect.right - copy_rect.left,
                            copy_rect.bottom - copy_rect.top };

  gpu_texture_t* rts[1] = { &hw->vram_readback_texture->base };
  opengl_device_set_render_targets(rts, 1, NULL);
  opengl_device_set_pipeline((gpu_pipeline_t*)hw->vram_extract_pipeline[0]);
  opengl_device_set_texture_sampler(0, &hw->vram_texture->base, NULL);

  const opengl_rect_t vp = { 0, 0, (s32)encoded_width, (s32)encoded_height };
  opengl_device_set_viewport(vp);
  opengl_device_set_scissor (vp);
  opengl_device_draw_with_push_constants(3, 0, uniforms, sizeof(uniforms));

  /* Prefer the async PBO download path via opengl_download_texture.
   * `copy_from_texture` does glCopyImageSubData (or fallback) into the PBO,
   * then `flush` blocks on the fence (or no-op for the CPU-buffer fallback).
   * The mapped pointer (`map_pointer`) is a host-readable view into the PBO. */
  if (hw->vram_readback_download_texture) {
    opengl_download_texture_copy_from_texture(
      hw->vram_readback_download_texture,
      /*dst_x=*/0, /*dst_y=*/0,
      hw->vram_readback_texture,
      /*src_x=*/0, /*src_y=*/0,
      encoded_width, encoded_height,
      /*src_layer=*/0, /*src_level=*/0,
      /*use_transfer_pitch=*/false);
    opengl_download_texture_flush(hw->vram_readback_download_texture);
    if (!opengl_download_texture_map(hw->vram_readback_download_texture, 0, 0,
                                     encoded_width, encoded_height))
      return;
    const u32 src_pitch = hw->vram_readback_download_texture->base.current_pitch;
    const u8* src_base  = hw->vram_readback_download_texture->base.map_pointer;
    if (!src_base) {
      opengl_download_texture_unmap(hw->vram_readback_download_texture);
      return;
    }
    for (u32 row = 0; row < encoded_height; row++) {
      const u32* src_row = (const u32*)(src_base + row * src_pitch);
      u16* dst_row = &g_vram[((u32)copy_rect.top + row) * VRAM_WIDTH + (u32)copy_rect.left];
      for (u32 col = 0; col < encoded_width; col++) {
        const u32 v = src_row[col];
        dst_row[col * 2u + 0u] = (u16)(v & 0xFFFFu);
        dst_row[col * 2u + 1u] = (u16)((v >> 16) & 0xFFFFu);
      }
    }
    opengl_download_texture_unmap(hw->vram_readback_download_texture);
    return;
  }

  /* Fallback: synchronous glReadPixels into a temp buffer.  Used when the
   * download_texture allocation failed at create-buffers time. */
  const u32 tmp_pixels = encoded_width * encoded_height;
  u32* tmp = (u32*)malloc(tmp_pixels * sizeof(u32));
  if (!tmp) return;

  /* Bind the readback FBO + read.  We rely on set_render_targets having set
   * the GL_DRAW_FRAMEBUFFER; ReadPixels reads from GL_READ_FRAMEBUFFER which
   * defaults to the same binding for combined targets in core profile. */
  glReadPixels(0, 0, (GLsizei)encoded_width, (GLsizei)encoded_height,
               GL_RGBA, GL_UNSIGNED_BYTE, tmp);

  /* Decode + write into g_vram.  Each output u32 encodes two 16-bit pixels:
   * low 16 bits = pixel (col*2), high 16 bits = pixel (col*2+1). */
  for (u32 row = 0; row < encoded_height; row++) {
    const u32* src_row = &tmp[row * encoded_width];
    u16* dst_row = &g_vram[((u32)copy_rect.top + row) * VRAM_WIDTH + (u32)copy_rect.left];
    for (u32 col = 0; col < encoded_width; col++) {
      const u32 v = src_row[col];
      dst_row[col * 2u + 0u] = (u16)(v & 0xFFFFu);
      dst_row[col * 2u + 1u] = (u16)((v >> 16) & 0xFFFFu);
    }
  }
  free(tmp);
}

static void hw_read_vram(gpu_backend_t* self, const gpu_backend_read_vram_cmd_t* c)
{
  (void)self; (void)c;
  (void)gpu_hw_read_vram_gl;  /* keep symbol referenced; avoid unused-static warning. */
}

static void hw_fill_vram(gpu_backend_t* self, const gpu_backend_fill_vram_cmd_t* c)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* Mirror SW fill so g_vram + cross-renderer save state stay coherent.  Full
   * GL takeover (including g_vram retirement) lands with the save-state
   * crosswalk. */
  gpu_sw_rasterizer_fill_vram(c->x, c->y, c->width, c->height, c->color,
                              c->interlaced_rendering, c->active_line_lsb);
  /* Issue the GPU-side fill via vram_fill_pipelines. */
  gpu_hw_fill_vram_gl(hw, c->x, c->y, c->width, c->height, c->color,
                      c->interlaced_rendering, c->active_line_lsb);
  /* Invalidate any cache entries whose source pixels just got
   * filled.  No-op when cache is off. */
  if (hw->use_texture_cache) {
    gpu_texture_cache_add_written_rectangle((s32)c->x, (s32)c->y,
                                            (s32)(c->x + c->width), (s32)(c->y + c->height),
                                            /*update_vram_writes=*/false,
                                            /*remove_from_hash_cache=*/false);
  }
}

static int hw_trace_env_cached(void)
{
  static int cached = -1;
  if (cached < 0) cached = (getenv("CUPID_TRACE") != NULL) ? 1 : 0;
  return cached;
}

static void hw_update_vram(gpu_backend_t* self, const gpu_backend_update_vram_cmd_t* c, const u16* data)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  static u32 vram_dbg_n = 0;
  if (hw_trace_env_cached() && (vram_dbg_n++ & 0x3F) == 0)
    fprintf(stderr, "[hw update_vram] dst=(%u,%u) %ux%u\n", c->x, c->y, c->width, c->height);
  gpu_sw_rasterizer_write_vram(c->x, c->y, c->width, c->height, data,
                               c->set_mask_while_drawing, c->check_mask_before_draw);
  gpu_hw_update_vram_gl(hw, c->x, c->y, c->width, c->height, data,
                        c->set_mask_while_drawing, c->check_mask_before_draw);
  /* Cache invalidation + replacement-tracking hook. */
  if (hw->use_texture_cache) {
    gpu_texture_cache_write_vram(c->x, c->y, c->width, c->height, data,
                                  c->set_mask_while_drawing, c->check_mask_before_draw,
                                 (s32)c->x, (s32)c->y, 
                                 (s32)(c->x + c->width), (s32)(c->y + c->height));
  }
}

static void hw_copy_vram(gpu_backend_t* self, const gpu_backend_copy_vram_cmd_t* c)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  gpu_sw_rasterizer_copy_vram(c->src_x, c->src_y, c->dst_x, c->dst_y,
                              c->width, c->height,
                              c->set_mask_while_drawing, c->check_mask_before_draw);
  gpu_hw_copy_vram_gl(hw, c->src_x, c->src_y, c->dst_x, c->dst_y, c->width, c->height,
                      c->set_mask_while_drawing, c->check_mask_before_draw);
  /* VRAM->VRAM copy invalidates the destination cache pages. */
  if (hw->use_texture_cache) {
    gpu_texture_cache_copy_vram(c->src_x, c->src_y, c->dst_x, c->dst_y, c->width, c->height,
                                 c->set_mask_while_drawing, c->check_mask_before_draw,
                                (s32)c->src_x, (s32)c->src_y,
                                (s32)(c->src_x + c->width), (s32)(c->src_y + c->height),
                                (s32)c->dst_x, (s32)c->dst_y,
                                (s32)(c->dst_x + c->width), (s32)(c->dst_y + c->height));
  }
}

/* Map a PS1 texture mode + textured/raw flags to the gpu_hw batch texture
 * mode enum used by the FS / VS shadergen.  Sprite variants are deferred
 * and not selected here.  PAGE_TEXTURE mode is selected when the
 * texture cache is enabled (see gpu_hw_resolve_page_texture_lookup); this
 * helper itself stays cache-agnostic and returns the per-mode value. */
static gpu_hw_batch_texture_mode_t gpu_hw_pick_batch_texture_mode(const gpu_backend_draw_cmd_t* c)
{
  if (!c->texture_enable)
    return GPU_HW_BATCH_TEXTURE_MODE_DISABLED;
  const gpu_texture_mode_t tm = gpu_draw_mode_reg_texture_mode(c->draw_mode);
  switch (tm) {
    case GPU_TEXTURE_MODE_PALETTE_4BIT:          return GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT;
    case GPU_TEXTURE_MODE_PALETTE_8BIT:          return GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT;
    case GPU_TEXTURE_MODE_DIRECT_16BIT:          return GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT;
    case GPU_TEXTURE_MODE_RESERVED_DIRECT_16BIT: return GPU_HW_BATCH_TEXTURE_MODE_DIRECT_16BIT;
    default:                                     return GPU_HW_BATCH_TEXTURE_MODE_DISABLED;
  }
}

/* Try the texture cache for the current draw.  When the cache is
 * enabled and the draw is textured, returns PAGE_TEXTURE + populates
 * `*out_texture` with the cached opengl_texture_t*.  When the cache misses
 * (lookup returns NULL or the hash entry hasn't materialised a texture yet)
 * or the cache is disabled, returns the per-mode value with `*out_texture`
 * set to NULL; the caller binds vram_read_texture as before.  Mirrors the
 * LookupSource branch. */
static gpu_hw_batch_texture_mode_t gpu_hw_resolve_batch_texture_mode(
   gpu_hw_t* hw, const gpu_backend_draw_cmd_t* c,
  gpu_transparency_mode_t trans_mode, opengl_texture_t** out_texture) 
{
  *out_texture = NULL;
  const gpu_hw_batch_texture_mode_t per_mode = gpu_hw_pick_batch_texture_mode(c);
  if (!hw->use_texture_cache || per_mode == GPU_HW_BATCH_TEXTURE_MODE_DISABLED)
    return per_mode;

  /* PAGE_TEXTURE only fires when texture cache is on AND the draw is textured.
   * The source key composes the texture page + palette + per-mode value. */
  const u32 page_idx = (u32)gpu_draw_mode_reg_texture_page(c->draw_mode);
  if (page_idx >= NUM_VRAM_PAGES)
    return per_mode;
  gpu_tc_source_key_t key;
  key.page    = (u8)page_idx;
  key.mode    = gpu_draw_mode_reg_texture_mode(c->draw_mode);
  key.palette = c->palette;

  const gpu_tc_palette_record_flags_t flags = (trans_mode != GPU_TRANSPARENCY_MODE_DISABLED)
    ? GPU_TC_PALETTE_RECORD_FLAG_HAS_SEMI_TRANSPARENT_DRAWS
    : GPU_TC_PALETTE_RECORD_FLAG_NONE;

  const gpu_tc_source_t* src = gpu_texture_cache_lookup_source(key, 0, 0, 0, 0, flags);
  if (!src)
    return per_mode;
  opengl_texture_t* tex = gpu_texture_cache_source_get_texture(src);
  if (!tex)
    return per_mode;
  *out_texture = tex;
  return GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE;
}

 /* Pack texpage register + palette register into the BatchVertex
 * texpage field shape: (palette_bits << 16) | draw_mode_bits. */
ALWAYS_INLINE u32 gpu_hw_pack_texpage(const gpu_backend_draw_cmd_t* c)
{
  return (u32)c->draw_mode.bits | ((u32)c->palette.bits << 16);
}

/* Latch the batch config the next flush will use, and update the
 * UBO texture-window + alpha-factor entries (mirrors PrepareDraw stripped
 * of TC/depth/ROV branches). */
static void gpu_hw_latch_batch_config(gpu_hw_t* hw, const gpu_backend_draw_cmd_t* c,
                                       gpu_hw_batch_texture_mode_t batch_texture_mode,
                                      gpu_transparency_mode_t transparency_mode) 
{
  hw->batch.texture_mode      = batch_texture_mode;
  hw->batch.transparency_mode = transparency_mode;
  hw->batch.dithering         = (!hw->true_color && c->dither_enable);
  hw->batch.interlacing       = c->interlaced_rendering;
  hw->batch.set_mask_while_drawing = c->set_mask_while_drawing;
  hw->batch.check_mask_before_draw = c->check_mask_before_draw;
  hw->batch.use_depth_buffer  = false;       /* write_mask_as_depth path deferred. */
  hw->batch.sprite_mode       = false;       /* allow_sprite_mode=false. */

  hw->batch_ubo_data.u_texture_window[0] = (u32)c->window.and_x;
  hw->batch_ubo_data.u_texture_window[1] = (u32)c->window.and_y;
  hw->batch_ubo_data.u_texture_window[2] = (u32)c->window.or_x;
  hw->batch_ubo_data.u_texture_window[3] = (u32)c->window.or_y;
  hw->texture_window_bits = c->window;
   hw->texture_window_active =
    (c->window.and_x != 0xFFu || c->window.and_y != 0xFFu || 
     c->window.or_x  != 0x00u || c->window.or_y  != 0x00u);

  /* Alpha factor matrix (transparent_alpha[4][2]).  Modes
   * 0/3 use 0.5/0.5 and 0.25/1.0; shader halves the FG contribution. */
  static const float k_transparent_alpha[4][2] = {
    {0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}
  };
  if (transparency_mode != GPU_TRANSPARENCY_MODE_DISABLED &&
      (u32)transparency_mode < 4u) {
    hw->batch_ubo_data.u_src_alpha_factor = k_transparent_alpha[(u32)transparency_mode][0];
    hw->batch_ubo_data.u_dst_alpha_factor = k_transparent_alpha[(u32)transparency_mode][1];
  } else {
    hw->batch_ubo_data.u_src_alpha_factor = 1.0f;
    hw->batch_ubo_data.u_dst_alpha_factor = 0.0f;
  }
  hw->batch_ubo_data.u_set_mask_while_drawing = c->set_mask_while_drawing ? 1u : 0u;
  hw->batch_ubo_data.u_interlaced_displayed_field = (u32)c->active_line_lsb;
  hw->batch_ubo_dirty = true;
}

/* Returns true if the latched batch config differs from the new one --
 * caller flushes before emitting verts so the previous batch can drain
 * with its own pipeline / blend state.  Includes a cache-texture
 * comparison so two consecutive PAGE_TEXTURE draws against different
 * cached pages don't end up batched together with one stale binding. */
static bool gpu_hw_batch_config_changed(const gpu_hw_t* hw, const gpu_backend_draw_cmd_t* c,
                                         gpu_hw_batch_texture_mode_t batch_texture_mode,
                                        gpu_transparency_mode_t transparency_mode, 
                                        const opengl_texture_t* cache_texture)
{
  return hw->batch.texture_mode != batch_texture_mode
      || hw->batch.transparency_mode != transparency_mode
      || hw->batch.dithering != (!hw->true_color && c->dither_enable)
      || hw->batch.interlacing != c->interlaced_rendering
      || hw->batch.set_mask_while_drawing != c->set_mask_while_drawing
      || hw->batch.check_mask_before_draw != c->check_mask_before_draw
      || !gpu_texture_window_equals(hw->texture_window_bits, c->window)
      || (batch_texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE && 
          hw->batch_cache_texture != cache_texture);
}

/* Normalised vertex depth used by the write_mask_as_depth path.  Mirrors
 * upstream GPU_HW::GetCurrentNormalizedVertexDepth(): start near 1.0 and
 * decrease per draw.  Combined with depth_func=GREATER_OR_EQUAL the
 * destination's set-mask depth (close to 1) blocks subsequent writes from a
 * later (lower-z) draw. */
ALWAYS_INLINE static float gpu_hw_normalized_vertex_depth(const gpu_hw_t* hw)
{
  return 1.0f - ((float)hw->current_depth / 65535.0f);
}

/* Counter overflow guard.  When current_depth approaches the 65535 ceiling
 * we flush + reset.  Mirrors upstream's MAX_BATCH_VERTEX_COUNTER_IDS check
 * (65536 - 2).  After reset the depth attachment carries the full mask state
 * for older pixels via the FS's previous oalpha*v_pos.z writes; the new
 * counter starts at 1 again so subsequent set-mask writes land at z~=1.
 *
 * NB: callers must re-run gpu_hw_ensure_batch_buffers_mapped() after this
 * helper; flush_render unmaps the batch buffers (lazy-map convention), so
 * the next vertex push would dereference NULL otherwise.  The helper returns
 * true when a flush actually occurred. */
#define GPU_HW_MAX_VERTEX_DEPTH_COUNTER  (65536u - 2u)
ALWAYS_INLINE static bool gpu_hw_check_depth_counter(gpu_hw_t* hw, u32 required_vertices)
{
  if (!hw->write_mask_as_depth)
    return false;
  if ((u32)hw->current_depth + required_vertices <= GPU_HW_MAX_VERTEX_DEPTH_COUNTER)
    return false;
  /* Flush so the current batch drains with the existing depth map. */
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);
  /* TODO: rebuild depth from vram_texture.alpha (m_vram_update_depth_pipeline
   * upstream).  Without that helper a full clear loses the mask history -
   * acceptable trade-off until the rebuild pipeline lands.  Clear to 0 so
   * subsequent check_mask draws don't reject against stale GE depths. */
  if (hw->vram_depth_texture)
    opengl_device_clear_depth(&hw->vram_depth_texture->base, 0.0f);
  hw->current_depth = 1;
  return true;
}

/* ------------------------------------------------------------------------- */
/* Polygon-emit helpers ported from duckstation gpu_hw.cpp.                  */
/*                                                                           */
/* The previous emit_polygon path was a flat translation of upstream's       */
/* per-vertex inner loop with no surrounding pre/post-processing.  That made */
/* the GL renderer skip three things SW gets for free: per-polygon UV        */
/* clamping, off-screen / FF8-style polygon cull, and 16-bit colour          */
/* truncation.  Visible artefacts (texture bleed across page/CLUT, garbage   */
/* triangles, colour banding mismatch with SW) all trace back to these.      */
/* ------------------------------------------------------------------------- */

/* Mirrors gpu_hw.cpp:167-179.  When the dithering mode is TrueColor and the
 * draw is flat-shaded (no shading, no texture, no dither), upstream
 * round-trips the colour through 5/5/5 so the GL framebuffer matches what
 * the SW canon would store at native bit depth. */
static inline bool gpu_hw_should_truncate_32_to_16(const gpu_backend_draw_cmd_t* cmd)
{
  return (!cmd->texture_enable && !cmd->shading_enable && !cmd->dither_enable &&
          g_settings.gpu_dithering_mode == GPU_DITHERING_MODE_TRUE_COLOR);
}

static inline u32 gpu_hw_truncate_32_to_16(u32 color)
{
  /* Per channel: trunc((c >> 3) * 255 / 31).  Upstream uses GSVector4
   * float-to-int (truncate-toward-zero), so match that, not round. */
  const u32 r5 = ((color >>  0) & 0xFFu) >> 3;
  const u32 g5 = ((color >>  8) & 0xFFu) >> 3;
  const u32 b5 = ((color >> 16) & 0xFFu) >> 3;
  const u32 a  =  (color >> 24) & 0xFFu;
  const u32 r8 = (u32)((float)r5 * (255.0f / 31.0f));
  const u32 g8 = (u32)((float)g5 * (255.0f / 31.0f));
  const u32 b8 = (u32)((float)b5 * (255.0f / 31.0f));
  return (r8 & 0xFFu) | ((g8 & 0xFFu) << 8) | ((b8 & 0xFFu) << 16) | (a << 24);
}

/* Per-polygon UV bbox -> per-vertex packed limits.  Mirrors
 * gpu_hw.cpp:2489-2518 (ComputePolygonUVLimits).  Pack format matches
 * BatchVertex::PackUVLimits at gpu_hw.cpp:281-284; and the rectangle path
 * at gpu_hw.c:1952-1955. */
static void gpu_hw_compute_polygon_uv_limits(gpu_hw_batch_vertex_t* v, u32 num_vertices)
{
  u32 min_u = v[0].u, max_u = v[0].u;
  u32 min_v = v[0].v, max_v = v[0].v;
  for (u32 i = 1u; i < num_vertices; i++) {
    if ((u32)v[i].u < min_u) min_u = v[i].u;
    if ((u32)v[i].u > max_u) max_u = v[i].u;
    if ((u32)v[i].v < min_v) min_v = v[i].v;
    if ((u32)v[i].v > max_v) max_v = v[i].v;
  }
  if (min_u != max_u) max_u -= 1u;
  if (min_v != max_v) max_v -= 1u;
  const u32 packed = (min_u & 0xFFu)
                   | ((min_v & 0xFFu) << 8)
                   | ((max_u & 0xFFu) << 16)
                   | ((max_v & 0xFFu) << 24);
  for (u32 i = 0u; i < num_vertices; i++)
    v[i].uv_limits = packed;
  /* TODO: ShouldCheckForTexPageOverlap / CheckForTexPageOverlap. */
}

/* Sign-extend the low 11 bits of the integer position while preserving
 * the fractional part.  PSX vertex coords are 11-bit two's complement,
 * but FF8 (and a few others) feed values outside that range that need to
 * "wrap" into the on-screen area.  Mirrors gpu_hw.cpp:3016-3022. */
static inline float gpu_hw_truncate_pos_ff8(float pos)
{
  const s32 ipos = (s32)pos;                  /* trunc-toward-zero */
  const float fdiff = pos - (float)ipos;
  const s32 truncated = (s32)((u32)ipos << 21u) >> 21;
  return (float)truncated + fdiff;
}

/* Polygon bbox helpers.  Use exclusive (right, bottom) so cull logic mirrors
 * upstream's add32(0,0,1,1) trick. */
typedef struct { s32 l, t, r, b; } gpu_hw_excl_rect_t;

static inline gpu_hw_excl_rect_t gpu_hw_poly_bbox(float ax, float ay,
                                                  float bx, float by,
                                                  float cx, float cy)
{
  const s32 ix0 = (s32)ax, ix1 = (s32)bx, ix2 = (s32)cx;
  const s32 iy0 = (s32)ay, iy1 = (s32)by, iy2 = (s32)cy;
  s32 minx = ix0, maxx = ix0;
  s32 miny = iy0, maxy = iy0;
  if (ix1 < minx) minx = ix1;
  if (ix1 > maxx) maxx = ix1;
  if (ix2 < minx) minx = ix2;
  if (ix2 > maxx) maxx = ix2;
  if (iy1 < miny) miny = iy1;
  if (iy1 > maxy) maxy = iy1;
  if (iy2 < miny) miny = iy2;
  if (iy2 > maxy) maxy = iy2;
  return (gpu_hw_excl_rect_t){minx, miny, maxx + 1, maxy + 1};
}

static inline gpu_hw_excl_rect_t gpu_hw_excl_intersect_area(gpu_hw_excl_rect_t r,
                                                            const gpu_drawing_area_t* a)
{
  /* gpu_drawing_area_t bounds are inclusive; convert to exclusive r,b. */
  const s32 al = (s32)a->left, at = (s32)a->top;
  const s32 ar = (s32)a->right + 1, ab = (s32)a->bottom + 1;
  gpu_hw_excl_rect_t out;
  out.l = (r.l > al) ? r.l : al;
  out.t = (r.t > at) ? r.t : at;
  out.r = (r.r < ar) ? r.r : ar;
  out.b = (r.b < ab) ? r.b : ab;
  return out;
}

static inline bool gpu_hw_excl_empty(gpu_hw_excl_rect_t r) { return r.r <= r.l || r.b <= r.t; }
static inline s32  gpu_hw_excl_w(gpu_hw_excl_rect_t r)     { return r.r - r.l; }
static inline s32  gpu_hw_excl_h(gpu_hw_excl_rect_t r)     { return r.b - r.t; }

/* Polygon cull / FF8 truncate-position pre-processing.  Mirrors the body of
 * gpu_hw.cpp:2992-3102 (BeginPolygonDraw).  Returns false if the whole
 * primitive is off-screen and should be skipped entirely.  May rewrite
 * vertex positions (FF8 path) and reduce a quad to a triangle (when one
 * half is off-screen). */
static bool gpu_hw_begin_polygon_draw(const gpu_hw_t* hw,
                                      gpu_hw_batch_vertex_t* v,
                                      u32* num_vertices_io)
{
  u32 nv = *num_vertices_io;

  gpu_hw_excl_rect_t r012 = gpu_hw_poly_bbox(v[0].x, v[0].y,
                                             v[1].x, v[1].y,
                                             v[2].x, v[2].y);
  gpu_hw_excl_rect_t c012 = gpu_hw_excl_intersect_area(r012, &hw->base.clamped_drawing_area);
  bool first_culled = gpu_hw_excl_empty(c012);

  if (first_culled) {
    /* See FF8 comment in upstream gpu_hw.cpp:3008-3014.  Sign-extend the
     * low 11 bits of every integer position; if the post-truncate triangle
     * intersects the drawing area and isn't oversized, accept the rewritten
     * positions, otherwise cull. */
    const float tx0 = gpu_hw_truncate_pos_ff8(v[0].x);
    const float ty0 = gpu_hw_truncate_pos_ff8(v[0].y);
    const float tx1 = gpu_hw_truncate_pos_ff8(v[1].x);
    const float ty1 = gpu_hw_truncate_pos_ff8(v[1].y);
    const float tx2 = gpu_hw_truncate_pos_ff8(v[2].x);
    const float ty2 = gpu_hw_truncate_pos_ff8(v[2].y);
    const float tx3 = (nv == 4u) ? gpu_hw_truncate_pos_ff8(v[3].x) : 0.0f;
    const float ty3 = (nv == 4u) ? gpu_hw_truncate_pos_ff8(v[3].y) : 0.0f;

    const gpu_hw_excl_rect_t tr012  = gpu_hw_poly_bbox(tx0, ty0, tx1, ty1, tx2, ty2);
    const gpu_hw_excl_rect_t tc012  = gpu_hw_excl_intersect_area(tr012, &hw->base.clamped_drawing_area);
    const bool oversized = (gpu_hw_excl_w(tr012) > (s32)MAX_PRIMITIVE_WIDTH) ||
                           (gpu_hw_excl_h(tr012) > (s32)MAX_PRIMITIVE_HEIGHT);
    const bool still_culled = oversized || gpu_hw_excl_empty(tc012);

    if (!still_culled) {
      v[0].x = tx0; v[0].y = ty0;
      v[1].x = tx1; v[1].y = ty1;
      v[2].x = tx2; v[2].y = ty2;
      if (nv == 4u) { v[3].x = tx3; v[3].y = ty3; }
      first_culled = false;
    } else if (nv != 4u) {
      return false;   /* triangle gone, no second half to fall back on */
    }
  }

  if (nv == 4u) {
    const gpu_hw_excl_rect_t r123 = gpu_hw_poly_bbox(v[1].x, v[1].y,
                                                     v[2].x, v[2].y,
                                                     v[3].x, v[3].y);
    const gpu_hw_excl_rect_t c123 = gpu_hw_excl_intersect_area(r123, &hw->base.clamped_drawing_area);
    const bool second_culled = gpu_hw_excl_empty(c123);
    if (second_culled) {
      if (first_culled)
        return false;          /* both halves off-screen */
      *num_vertices_io = 3u;   /* drop the second tri */
    } else if (first_culled) {
      /* Promote the second tri into the first slot (mirrors upstream
       * gpu_hw.cpp:3093-3099). */
      v[0] = v[2];
      v[2] = v[3];
      *num_vertices_io = 3u;
    }
  }
  return true;
}

/* GL fast path for polygon draws.  Returns true when the polygon
 * was emitted to the batch buffer, false when the SW rasterizer fallback
 * handles it (PAGE_TEXTURE / sprite-mode / use_shader_blending / etc.).
 * Mirrors `DrawPolygon` plus `BeginPolygonDraw` / `FinishPolygonDraw`.
 * Quads use the (0,1,2 / 2,1,3) winding. */
static bool gpu_hw_emit_polygon_gl(gpu_hw_t* hw, const gpu_backend_draw_polygon_cmd_t* c,
                                   const gpu_backend_polygon_vertex_t* vertices)
{
  u32 num_vertices = (u32)c->base.num_vertices;
  if (num_vertices != 3u && num_vertices != 4u)
    return false;

  const gpu_transparency_mode_t trans_mode = c->base.transparency_enable
    ? gpu_draw_mode_reg_transparency_mode(c->base.draw_mode)
    : GPU_TRANSPARENCY_MODE_DISABLED;

  /* Textured + transparent + dither + check_mask are allowed.
   * When the texture cache is enabled and the draw is textured,
   * we promote the batch to PAGE_TEXTURE mode and stash the cached
   * opengl_texture_t* on `hw->batch_cache_texture`.  Cache miss falls
   * through to per-mode (vram_read_texture sample). */
  opengl_texture_t* cache_texture = NULL;
  const gpu_hw_batch_texture_mode_t batch_texture_mode =
    gpu_hw_resolve_batch_texture_mode(hw, &c->base, trans_mode, &cache_texture);

  /* Fail closed if pipelines / mapped buffers aren't ready yet. */
  gpu_hw_ensure_batch_buffers_mapped(hw);

  /* Flush ahead of switching batch config so the previous batch drains with
   * its own pipeline state. */
  if (hw->batch_index_count > 0u &&
      gpu_hw_batch_config_changed(hw, &c->base, batch_texture_mode, trans_mode, cache_texture)) {
    if (hw->base.vtable->flush_render)
      hw->base.vtable->flush_render(&hw->base);
    gpu_hw_ensure_batch_buffers_mapped(hw);
  }

  /* Pick the matrix slot we'll need at flush time.  If it doesn't exist
   * (e.g. shader_blend gating, true_color+dither) bail to SW. */
  const u32 dith_slot = (!hw->true_color && c->base.dither_enable) ? 0u
                       : (hw->true_color ? 4u : 0u);
  const u32 batch_idx = gpu_hw_batch_pipeline_index(
    /*depth_test=*/0u,
    (u32)trans_mode,
    /*render_mode=*/(trans_mode != GPU_TRANSPARENCY_MODE_DISABLED) ? 1u : 0u,
    (u32)batch_texture_mode,
    dith_slot,
    /*interlacing=*/c->base.interlaced_rendering ? 1u : 0u,
    /*check_mask=*/c->base.check_mask_before_draw ? 1u : 0u);
  if (!hw->batch_pipelines[batch_idx])
    return false;

  /* Stage to a local working set first so cull / FF8-truncate /
   * truncate_32_to_16 / compute_polygon_uv_limits can rewrite vertices
   * before they hit the mapped batch buffer.  The batch buffer is
   * append-only; once written, the only way to "undo" is a flush. */
  const bool textured = (batch_texture_mode != GPU_HW_BATCH_TEXTURE_MODE_DISABLED);
  const bool raw_tex  = (c->base.texture_enable && c->base.raw_texture_enable);
  const u32  texpage  = textured ? gpu_hw_pack_texpage(&c->base) : 0u;

  gpu_hw_batch_vertex_t local[4];
  for (u32 i = 0u; i < num_vertices; i++) {
    local[i].x         = (float)vertices[i].x;
    local[i].y         = (float)vertices[i].y;
    local[i].z         = 0.0f;             /* assigned post-cull from depth counter */
    local[i].w         = 1.0f;
    local[i].color     = (raw_tex ? 0x00808080u : (vertices[i].color & 0x00FFFFFFu)) | 0xFF000000u;
    local[i].texpage   = texpage;
    local[i].u         = textured ? (u16)vertices[i].u : (u16)0u;
    local[i].v         = textured ? (u16)vertices[i].v : (u16)0u;
    local[i].uv_limits = 0xFFFF0000u;       /* "no clamp" sentinel; possibly overwritten */
  }

  /* Begin: cull / FF8-truncate.  May reduce nv from 4 to 3.
   * TEMP DISABLED: cull was over-aggressive and dropped polygons covering
   * the display area, leaving prior-frame content visible (Crash title
   * "ghost text" symptom).  Need to revisit clamped_drawing_area init. */
  (void)gpu_hw_begin_polygon_draw;

  /* Colour 5/5/5 truncation when dithering=TrueColor and primitive is flat. */
  if (gpu_hw_should_truncate_32_to_16(&c->base)) {
    for (u32 i = 0u; i < num_vertices; i++)
      local[i].color = gpu_hw_truncate_32_to_16(local[i].color);
  }

  /* Per-polygon UV bbox so non-NEAREST filter passes clamp correctly. */
  if (c->base.texture_enable && hw->compute_uv_range)
    gpu_hw_compute_polygon_uv_limits(local, num_vertices);

  /* TODO: HandleFlippedQuadTextureCoordinates + SetBatchSpriteMode
   * (upscaled 2D-quad UV correction).  Requires sprite-mode pipeline
   * matrix; gated off via allow_sprite_mode=false until that lands. */

  /* Now reserve batch space.  num_vertices may have shrunk to 3. */
  const u32 indices_required = (num_vertices == 4u) ? 6u : 3u;
  if ((u32)hw->batch_vertex_count + num_vertices > (u32)hw->batch_vertex_space ||
      (u32)hw->batch_index_count  + indices_required > (u32)hw->batch_index_space) {
    if (hw->base.vtable->flush_render)
      hw->base.vtable->flush_render(&hw->base);
    gpu_hw_ensure_batch_buffers_mapped(hw);
  }

  /* Reserve one depth-counter slot for the (possibly cull-reduced) primitive.
   * gpu_hw_check_depth_counter may flush+remap on overflow. */
  if (gpu_hw_check_depth_counter(hw, 1u))
    gpu_hw_ensure_batch_buffers_mapped(hw);
  const float vertex_z = gpu_hw_normalized_vertex_depth(hw);

  /* Push the finalised vertices. */
  const u32 first_vertex = (u32)hw->batch_vertex_count;
  for (u32 i = 0u; i < num_vertices; i++) {
    gpu_hw_batch_vertex_t* dst = &hw->batch_vertex_ptr[first_vertex + i];
    *dst   = local[i];
    dst->z = vertex_z;
  }
  hw->batch_vertex_count = (u16)(first_vertex + num_vertices);
  hw->current_depth++;
  hw->last_depth_z = vertex_z;

  u16* idx = &hw->batch_index_ptr[hw->batch_index_count];
  idx[0] = (u16)(first_vertex + 0u);
  idx[1] = (u16)(first_vertex + 1u);
  idx[2] = (u16)(first_vertex + 2u);
  hw->batch_index_count = (u16)(hw->batch_index_count + 3u);
  if (num_vertices == 4u) {
    idx[3] = (u16)(first_vertex + 2u);
    idx[4] = (u16)(first_vertex + 1u);
    idx[5] = (u16)(first_vertex + 3u);
    hw->batch_index_count = (u16)(hw->batch_index_count + 3u);
  }

  /* Latch the batch config so flush picks up the right pipeline matrix slot. */
  gpu_hw_latch_batch_config(hw, &c->base, batch_texture_mode, trans_mode);
  /* Stash the cached source texture for binding at flush time
   * (only relevant when batch_texture_mode == PAGE_TEXTURE). */
  hw->batch_cache_texture = cache_texture;
  return true;
}

/* Textured rectangle / sprite GL fast path.  Page-walked emission
 * to handle PS1 sprites that wrap across texture-page boundaries (256x256
 * page, 11-bit position).  Returns true on success.  Falls back to SW if the
 * pipeline isn't ready or PAGE_TEXTURE / sprite-mode would be required. */
static bool gpu_hw_emit_rectangle_gl(gpu_hw_t* hw, const gpu_backend_draw_rectangle_cmd_t* c)
{
  gpu_hw_ensure_batch_buffers_mapped(hw);

  const gpu_transparency_mode_t trans_mode = c->base.transparency_enable
    ? gpu_draw_mode_reg_transparency_mode(c->base.draw_mode)
    : GPU_TRANSPARENCY_MODE_DISABLED;

  /* Cache lookup mirrors emit_polygon_gl. */
  opengl_texture_t* cache_texture = NULL;
  const gpu_hw_batch_texture_mode_t batch_texture_mode =
    gpu_hw_resolve_batch_texture_mode(hw, &c->base, trans_mode, &cache_texture);

  /* Flush on config change so previous batch drains. */
  if (hw->batch_index_count > 0u &&
      gpu_hw_batch_config_changed(hw, &c->base, batch_texture_mode, trans_mode, cache_texture)) {
    if (hw->base.vtable->flush_render)
      hw->base.vtable->flush_render(&hw->base);
    gpu_hw_ensure_batch_buffers_mapped(hw);
  }

  const u32 dith_slot = (!hw->true_color && c->base.dither_enable) ? 0u
                       : (hw->true_color ? 4u : 0u);
  const u32 batch_idx = gpu_hw_batch_pipeline_index(
    /*depth_test=*/0u,
    (u32)trans_mode,
    /*render_mode=*/(trans_mode != GPU_TRANSPARENCY_MODE_DISABLED) ? 1u : 0u,
    (u32)batch_texture_mode,
    dith_slot, 
    /*interlacing=*/c->base.interlaced_rendering ? 1u : 0u,
    /*check_mask=*/c->base.check_mask_before_draw ? 1u : 0u);
  if (!hw->batch_pipelines[batch_idx])
    return false;

  /* One depth slot for the whole rectangle; all sub-page quads share the
   * same z so depth_test against a check_mask destination is consistent. */
  if (gpu_hw_check_depth_counter(hw, 1u))
    gpu_hw_ensure_batch_buffers_mapped(hw);
  const float vertex_z = gpu_hw_normalized_vertex_depth(hw);

  const bool textured = (batch_texture_mode != GPU_HW_BATCH_TEXTURE_MODE_DISABLED);
  const bool raw_tex  = (c->base.texture_enable && c->base.raw_texture_enable);
  const u32  texpage  = textured ? gpu_hw_pack_texpage(&c->base) : 0u;
  /* Mirror upstream gpu_hw.cpp:2825: rectangles use truncate_32_to_16 too
   * when dithering=TrueColor and the prim is flat-shaded. */
  const u32  src_color = gpu_hw_should_truncate_32_to_16(&c->base)
                       ? gpu_hw_truncate_32_to_16(c->color) : c->color;
  const u32  color    = raw_tex ? 0x00808080u : (src_color & 0x00FFFFFFu);
  const u32  base_col = color | 0xFF000000u;

  const s32 pos_x = c->x;
  const s32 pos_y = c->y;
  const u32 orig_tex_left = (u32)(c->texcoord & 0xFFu);
  const u32 orig_tex_top  = (u32)((c->texcoord >> 8) & 0xFFu);
  const u32 rect_w = c->width;
  const u32 rect_h = c->height;

  /* Page walk: split into 256x256-aligned chunks so the texture page repeats. */
  u32 tex_top = orig_tex_top;
  for (u32 y_off = 0; y_off < rect_h; ) {
    const u32 quad_h = (rect_h - y_off) < (TEXTURE_PAGE_HEIGHT - tex_top)
                       ? (rect_h - y_off) : (TEXTURE_PAGE_HEIGHT - tex_top);
    const float qy0 = (float)(pos_y + (s32)y_off);
    const float qy1 = qy0 + (float)quad_h;
    const u32   tex_bottom = tex_top + quad_h;

    u32 tex_left = orig_tex_left;
    for (u32 x_off = 0; x_off < rect_w; ) {
      const u32 quad_w = (rect_w - x_off) < (TEXTURE_PAGE_WIDTH - tex_left)
                         ? (rect_w - x_off) : (TEXTURE_PAGE_WIDTH - tex_left);
      const float qx0 = (float)(pos_x + (s32)x_off);
      const float qx1 = qx0 + (float)quad_w;
      const u32   tex_right = tex_left + quad_w;
      const u32 uv_limits = (tex_left & 0xFFu)
                          | ((tex_top & 0xFFu) << 8)
                          | (((tex_right - 1u) & 0xFFu) << 16) 
                          | (((tex_bottom - 1u) & 0xFFu) << 24);

      if ((u32)hw->batch_vertex_count + 4u > (u32)hw->batch_vertex_space ||
          (u32)hw->batch_index_count  + 6u > (u32)hw->batch_index_space) {
        if (hw->base.vtable->flush_render)
          hw->base.vtable->flush_render(&hw->base);
        gpu_hw_ensure_batch_buffers_mapped(hw);
      }

      const u32 first_vertex = (u32)hw->batch_vertex_count;
      gpu_hw_batch_vertex_t* v = &hw->batch_vertex_ptr[first_vertex];
      v[0].x = qx0; v[0].y = qy0; v[0].z = vertex_z; v[0].w = 1.0f;
      v[0].color = base_col; v[0].texpage = texpage;
      v[0].u = (u16)tex_left;  v[0].v = (u16)tex_top;     v[0].uv_limits = uv_limits;
      v[1].x = qx1; v[1].y = qy0; v[1].z = vertex_z; v[1].w = 1.0f;
      v[1].color = base_col; v[1].texpage = texpage;
      v[1].u = (u16)tex_right; v[1].v = (u16)tex_top;     v[1].uv_limits = uv_limits;
      v[2].x = qx0; v[2].y = qy1; v[2].z = vertex_z; v[2].w = 1.0f;
      v[2].color = base_col; v[2].texpage = texpage;
      v[2].u = (u16)tex_left;  v[2].v = (u16)tex_bottom;  v[2].uv_limits = uv_limits;
      v[3].x = qx1; v[3].y = qy1; v[3].z = vertex_z; v[3].w = 1.0f;
      v[3].color = base_col; v[3].texpage = texpage;
      v[3].u = (u16)tex_right; v[3].v = (u16)tex_bottom;  v[3].uv_limits = uv_limits;
      hw->batch_vertex_count = (u16)(first_vertex + 4u);

      u16* idx = &hw->batch_index_ptr[hw->batch_index_count];
      idx[0] = (u16)(first_vertex + 0u);
      idx[1] = (u16)(first_vertex + 1u);
      idx[2] = (u16)(first_vertex + 2u);
      idx[3] = (u16)(first_vertex + 2u);
      idx[4] = (u16)(first_vertex + 1u);
      idx[5] = (u16)(first_vertex + 3u);
      hw->batch_index_count = (u16)(hw->batch_index_count + 6u);

      x_off += quad_w;
      tex_left = 0;
    }
    y_off += quad_h;
    tex_top = 0;
  }

  gpu_hw_latch_batch_config(hw, &c->base, batch_texture_mode, trans_mode);
  hw->batch_cache_texture = cache_texture;   /* See emit_polygon_gl */
  hw->current_depth++;
  hw->last_depth_z = vertex_z;
  return true;
}

/* Line GL fast path.  Two-tris-per-segment (4 verts, 6 indices) so
 * any pipeline that draws triangles can render lines.  Pixel-perfect line
 * detect / Vagrant-Story tweaks are deferred polish; for now we accept
 * reduced fidelity.  Mirrors the simplified path of `DrawLine` --
 * non-degenerate, single-thickness expansion. */
static bool gpu_hw_emit_line_gl(gpu_hw_t* hw, const gpu_backend_draw_line_cmd_t* c,
                                const gpu_backend_line_vertex_t* vertices)
{
  gpu_hw_ensure_batch_buffers_mapped(hw);

  /* Lines are always untextured. */
  const gpu_hw_batch_texture_mode_t batch_texture_mode = GPU_HW_BATCH_TEXTURE_MODE_DISABLED;
  const gpu_transparency_mode_t trans_mode = c->base.transparency_enable
    ? gpu_draw_mode_reg_transparency_mode(c->base.draw_mode)
    : GPU_TRANSPARENCY_MODE_DISABLED;

  if (hw->batch_index_count > 0u &&
      gpu_hw_batch_config_changed(hw, &c->base, batch_texture_mode, trans_mode, /*cache_texture=*/NULL)) {
    if (hw->base.vtable->flush_render)
      hw->base.vtable->flush_render(&hw->base);
    gpu_hw_ensure_batch_buffers_mapped(hw);
  }

  const u32 dith_slot = (!hw->true_color && c->base.dither_enable) ? 0u
                       : (hw->true_color ? 4u : 0u);
  const u32 batch_idx = gpu_hw_batch_pipeline_index(
    /*depth_test=*/0u,
    (u32)trans_mode,
    /*render_mode=*/(trans_mode != GPU_TRANSPARENCY_MODE_DISABLED) ? 1u : 0u,
    (u32)batch_texture_mode,
    dith_slot, 
    /*interlacing=*/c->base.interlaced_rendering ? 1u : 0u,
    /*check_mask=*/c->base.check_mask_before_draw ? 1u : 0u);
  if (!hw->batch_pipelines[batch_idx])
    return false;

  for (u16 i = 0; i + 1u < c->base.num_vertices; i += 2u) {
    const gpu_backend_line_vertex_t* p0 = &vertices[i];
    const gpu_backend_line_vertex_t* p1 = &vertices[i + 1u];

    /* Overflow check. */
    if ((u32)hw->batch_vertex_count + 4u > (u32)hw->batch_vertex_space ||
        (u32)hw->batch_index_count  + 6u > (u32)hw->batch_index_space) {
      if (hw->base.vtable->flush_render)
        hw->base.vtable->flush_render(&hw->base);
      gpu_hw_ensure_batch_buffers_mapped(hw);
    }

    /* One depth slot per segment. */
    if (gpu_hw_check_depth_counter(hw, 1u))
      gpu_hw_ensure_batch_buffers_mapped(hw);
    const float vertex_z = gpu_hw_normalized_vertex_depth(hw);

    const float x0 = (float)p0->x, y0 = (float)p0->y;
    const float x1 = (float)p1->x, y1 = (float)p1->y;
    /* Mirror duckstation gpu_hw.cpp:2676-2680 - truncate untextured/
     * flat-shaded line colours to the framebuffer's native 5/5/5 to match
     * SW canon when TrueColor dithering is selected.  Without this, lines
     * used for UI rules / dithered borders carry full 8-bit colour and
     * differ from the SW renderer. */
    u32 col0 = p0->color & 0x00FFFFFFu;
    u32 col1 = p1->color & 0x00FFFFFFu;
    if (gpu_hw_should_truncate_32_to_16(&c->base)) {
      col0 = gpu_hw_truncate_32_to_16(col0);
      col1 = gpu_hw_truncate_32_to_16(col1);
    }
    col0 |= 0xFF000000u;
    col1 |= 0xFF000000u;

    /* Rect expansion: pick the major axis and pad the minor axis by 1 px so
     * the rasterizer covers the diagonal as a 1-pixel-thick rectangle. */
    float dx = x1 - x0, dy = y1 - y0;
    float fdx = 0.0f, fdy = 0.0f;
    float pad_x0 = 0.0f, pad_x1 = 0.0f, pad_y0 = 0.0f, pad_y1 = 0.0f;
    const float adx = (dx < 0.0f) ? -dx : dx;
    const float ady = (dy < 0.0f) ? -dy : dy;
    if (adx == 0.0f && ady == 0.0f) {
      fdx = 1.0f;  /* point fallback. */
      pad_x1 = 1.0f; pad_y1 = 1.0f;
    } else if (adx > ady) {
      fdy = 1.0f;
      const float dydk = dy / adx;
      if (dx > 0.0f) { pad_x1 = 1.0f; pad_y1 = dydk; }
      else            { pad_x0 = 1.0f; pad_y0 = -dydk; }
    } else {
      fdx = 1.0f;
      const float dxdk = dx / ady;
      if (dy > 0.0f) { pad_y1 = 1.0f; pad_x1 = dxdk; }
      else            { pad_y0 = 1.0f; pad_x0 = -dxdk; }
    }
    const float ox0 = x0 + pad_x0, oy0 = y0 + pad_y0;
    const float ox1 = x1 + pad_x1, oy1 = y1 + pad_y1;

    const u32 first_vertex = (u32)hw->batch_vertex_count;
    gpu_hw_batch_vertex_t* v = &hw->batch_vertex_ptr[first_vertex];
    v[0].x = ox0;       v[0].y = oy0;       v[0].z = vertex_z; v[0].w = 1.0f;
    v[0].color = col0; v[0].texpage = 0; v[0].u = 0; v[0].v = 0; v[0].uv_limits = 0;
    v[1].x = ox0 + fdx; v[1].y = oy0 + fdy; v[1].z = vertex_z; v[1].w = 1.0f;
    v[1].color = col0; v[1].texpage = 0; v[1].u = 0; v[1].v = 0; v[1].uv_limits = 0;
    v[2].x = ox1;       v[2].y = oy1;       v[2].z = vertex_z; v[2].w = 1.0f;
    v[2].color = col1; v[2].texpage = 0; v[2].u = 0; v[2].v = 0; v[2].uv_limits = 0;
    v[3].x = ox1 + fdx; v[3].y = oy1 + fdy; v[3].z = vertex_z; v[3].w = 1.0f;
    v[3].color = col1; v[3].texpage = 0; v[3].u = 0; v[3].v = 0; v[3].uv_limits = 0;
    hw->batch_vertex_count = (u16)(first_vertex + 4u);
    hw->current_depth++;
    hw->last_depth_z = vertex_z;

    u16* idx = &hw->batch_index_ptr[hw->batch_index_count];
    idx[0] = (u16)(first_vertex + 0u);
    idx[1] = (u16)(first_vertex + 1u);
    idx[2] = (u16)(first_vertex + 2u);
    idx[3] = (u16)(first_vertex + 3u);
    idx[4] = (u16)(first_vertex + 2u);
    idx[5] = (u16)(first_vertex + 1u);
    hw->batch_index_count = (u16)(hw->batch_index_count + 6u);
  }

  gpu_hw_latch_batch_config(hw, &c->base, batch_texture_mode, trans_mode);
  return true;
}

/* When `gpu_use_software_renderer_for_readbacks` is on (default), the GL
 * renderer's per-frame display path reads `g_vram` via the SW row converter
 *; it never samples `vram_texture`.  Doing the GL batch emit on top of
 * the SW dual-write doubles per-poly cost (Mesa Intel iGPU emu speed dropped
 * to ~50 %, audio underruns).  Skip it; only emit GL when something
 * downstream is going to read `vram_texture`. */
ALWAYS_INLINE static bool gpu_hw_gl_emit_active(void)
{
  return !g_settings.gpu_use_software_renderer_for_readbacks;
}

static void hw_draw_polygon(gpu_backend_t* self, const gpu_backend_draw_polygon_cmd_t* c,
                            const gpu_backend_polygon_vertex_t* vertices)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* Always SW-rasterize so g_vram stays canonical for save state + display
   * compositing.  Eventually the GL VRAM RT becomes authoritative and this
   * dual path collapses to GL only. */
  gpu_sw_rasterizer_draw_triangle(&c->base, &vertices[0], &vertices[1], &vertices[2]);
  if (c->base.num_vertices > 3)
    gpu_sw_rasterizer_draw_triangle(&c->base, &vertices[2], &vertices[1], &vertices[3]);

  if (gpu_hw_gl_emit_active())
    (void)gpu_hw_emit_polygon_gl(hw, c, vertices);
}

static void hw_draw_rectangle(gpu_backend_t* self, const gpu_backend_draw_rectangle_cmd_t* c)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  gpu_sw_rasterizer_draw_rectangle(c);
  if (gpu_hw_gl_emit_active())
    (void)gpu_hw_emit_rectangle_gl(hw, c);
}

static void hw_draw_line(gpu_backend_t* self, const gpu_backend_draw_line_cmd_t* c,
                         const gpu_backend_line_vertex_t* vertices)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  for (u16 i = 0; i + 1 < c->base.num_vertices; i += 2)
    gpu_sw_rasterizer_draw_line(&c->base, &vertices[i], &vertices[i + 1]);
  if (gpu_hw_gl_emit_active())
    (void)gpu_hw_emit_line_gl(hw, c, vertices);
}

static void hw_drawing_area_changed(gpu_backend_t* self, const gpu_backend_set_drawing_area_cmd_t* c)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* Flush pending draws before swapping scissor so the previous batch
   * stays clipped to its drawing area. */
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);
  self->clamped_drawing_area = c->new_area;
  gpu_sw_drawing_area = c->new_area;
  hw->drawing_area_changed = true;
}

/* CLUT upload.
 *
 * SW path: refresh g_gpu_clut from g_vram (canonical for SW rasterizer).
 * GL path: also push the freshly cached CLUT row into vram_texture so
 * subsequent textured draws sample current CLUT data.  The CLUT lives in
 * VRAM at (palette_x*16, palette_y); a 16- or 256-pixel-wide row of u16
 * entries.  We convert g_gpu_clut[] into RGBA8 (matching the VRAM RT format
 * RGBA8) and call opengl_texture_update at the CLUT row position.
 *
 * Note: vram_read_texture is the texture that batch FS samples.  In cupid-ps1's
 * minimal setup we still need to copy vram_texture -> vram_read_texture
 * before each textured draw (UpdateVRAMReadTexture).  That's TODO
 * polish; for now the FS samples a stale read texture and the SW mirror
 * keeps user-visible output correct.  Even with that, pushing the CLUT to
 * vram_texture keeps the GL-side coherent for when vram_read_texture is
 * wired up.  TODO: UpdateVRAMReadTexture path. */
static void hw_update_clut(gpu_backend_t* self, const gpu_backend_update_clut_cmd_t* c)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* SW path is canonical for g_gpu_clut. */
  gpu_sw_rasterizer_update_clut(c->reg, c->clut_is_8bit);

  /* CLUT change invalidates any source whose palette region the
   * write touched. */
  if (hw->use_texture_cache)
    gpu_texture_cache_invalidate_clut(c->reg, c->clut_is_8bit);

  /* Push to vram_texture so the GL-side CLUT row matches.  RGBA5551 -> RGBA8888
   * conversion to match the RT format. */
  if (!hw->vram_texture)
    return;
  /* Drain pending polygon batch first; same Mesa Intel iGPU class of bug as
   * update_vram and fill_vram: glTexSubImage2D silently no-ops when
   * vram_texture is the bound FBO color attachment, which it is right after
   * a polygon batch flush.  Drain so the CLUT upload lands and any
   * subsequent textured batch sees the new palette. */
  if (hw->base.vtable->flush_render)
    hw->base.vtable->flush_render(&hw->base);
  const u32 base_x = gpu_texture_palette_reg_get_x_base(c->reg);
  const u32 base_y = gpu_texture_palette_reg_get_y_base(c->reg);
  const u32 entries = c->clut_is_8bit ? 256u : 16u;
  if (base_y >= VRAM_HEIGHT || base_x >= VRAM_WIDTH)
    return;
  const u32 scale = (u32)hw->resolution_scale;

  u32 row_count = entries;
  if (base_x + row_count > VRAM_WIDTH)
    row_count = VRAM_WIDTH - base_x;
  u32 rgba[256];
  for (u32 i = 0; i < row_count; i++)
    rgba[i] = vram_rgba5551_to_rgba8888((u32)g_gpu_clut[i]);

  /* vram_texture is allocated at VRAM_WIDTH*scale.  Replicate the native
   * CLUT row `scale` times so the upscaled block is fully populated; this
   * keeps the texture coherent even if a downstream consumer samples
   * unscaled VRAM coordinates above the first scaled row. */
  const u32 dst_x = base_x * scale;
  const u32 dst_y = base_y * scale;
  for (u32 r = 0; r < scale; r++) {
    (void)opengl_texture_update(hw->vram_texture, dst_x, dst_y + r, row_count, 1u,
                                rgba, row_count * 4u, 0u, 0u);
  }
  {
    gpu_rect_t r2 = gpu_rect_make((s32)base_x, (s32)base_y,
                                  (s32)(base_x + row_count), (s32)(base_y + 1));
    gpu_hw_mark_vram_dirty(hw, r2);
  }
}

static void hw_clear_cache(gpu_backend_t* self)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* Clear-cache is the GPU's "drop everything cached on the GL
   * side" hook (e.g. on disc swap / explicit cache flush).  We forward to
   * the texture cache invalidate path which drops both sources and the
   * hash cache textures.  No-op when the cache is off. */
  if (hw->use_texture_cache)
    gpu_texture_cache_invalidate();
}

static void hw_clear_vram(gpu_backend_t* self)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  memset(g_vram, 0, sizeof(g_vram));
  memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
  /* Cleared vram_texture state must propagate to vram_read_texture too;
   * mark full-VRAM dirty so the next textured bind blits the cleared
   * surface.  Otherwise vram_read_texture keeps pre-clear pixels and
   * subsequent textured samples / copy_vram reads pull stale content. */
  hw->vram_dirty_draw_rect  = GPU_HW_VRAM_SIZE_RECT;
  hw->vram_dirty_write_rect = GPU_HW_VRAM_SIZE_RECT;

  /* Clear the GPU-side VRAM render target (mirrors GPU_HW::ClearFramebuffer
   * minus the depth + texture-cache branches gated to later batches). */
  if (hw->vram_texture)
    opengl_device_clear_render_target(&hw->vram_texture->base, 0u);
  /* Mirror the color clear into the depth attachment so the mask-bit
   * tracking starts fresh.  0 = mask clear. */
  if (hw->vram_depth_texture)
    opengl_device_clear_depth(&hw->vram_depth_texture->base, 0.0f);

  /* Full VRAM wipe means every cache entry's source data just
   * went to zero; nuke the cache so subsequent draws re-decode from the
   * cleared VRAM.  No-op when cache is off. */
  if (hw->use_texture_cache)
    gpu_texture_cache_invalidate();

  hw->last_depth_z  = 1.0f;
  hw->current_depth = 1;
}

static void hw_convert_row_15bit(u32* dst, u32 src_x, u32 src_y, u32 width)
{
  const u16* src_row = &g_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
  if ((src_x + width) <= VRAM_WIDTH) {
    src_row += src_x;
    for (u32 col = 0; col < width; col++)
      dst[col] = vram_rgba5551_to_rgba8888(src_row[col]);
  } else {
    for (u32 col = 0; col < width; col++)
      dst[col] = vram_rgba5551_to_rgba8888(src_row[(src_x + col) % VRAM_WIDTH]);
  }
}

static void hw_convert_row_24bit(u32* dst, u32 src_x, u32 src_y, u32 skip_x, u32 width)
{
  const u16* src_row = &g_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
  for (u32 col = 0; col < width; col++) {
    const u32 offset = (src_x + (((skip_x + col) * 3u) / 2u));
    const u16 s0 = src_row[ offset      % VRAM_WIDTH];
    const u16 s1 = src_row[(offset + 1) % VRAM_WIDTH];
    const u8  shift = (u8)((col & 1u) * 8);
    const u32 rgb = (((u32)s1 << 16) | (u32)s0) >> shift;
    dst[col] = (rgb & 0x00FFFFFFu) | 0xFF000000u;
  }
}

/* Mirror duckstation VideoPresenter::ClearDisplayTexture (called from
 * gpu_hw.cpp:4068 / 4104): wipe the host display buffer so a frame that
 * disables display, or whose origin is fully out-of-bounds, doesn't
 * present whatever was left over from the prior frame.  Without this the
 * GL path holds last-frame pixels through transitions (Naughty Dog ->
 * menu, FMV -> game) -> visible bands of stale content. */
static void gpu_hw_clear_display_buffer(gpu_hw_t* hw, u32 out_w_s, u32 out_h_s)
{
  if (!hw->display_buffer)
    return;
  const u64 px = (u64)out_w_s * (u64)out_h_s;
  const u64 cap = (u64)hw->display_buffer_capacity;
  const u64 lim = (px <= cap) ? px : cap;
  for (u64 i = 0; i < lim; i++)
    hw->display_buffer[i] = 0xFF000000u;
}

static void hw_update_display(gpu_backend_t* self, const gpu_backend_update_display_cmd_t* c)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* Mirror duckstation GPU_HW::UpdateDisplay (gpu_hw.cpp:4027): drain any
   * vertices still mapped in the streaming batch buffer into vram_texture.
   * Otherwise gpu_hw_extract_display_gl below samples a stale (or initial-
   * cleared) target -> display_buffer is all zeros -> black screen. */
  if (self->vtable->flush_render)
    self->vtable->flush_render(self);
  static u32 dbg_n = 0;
  static int disp_trace_cached = -1;
  if (disp_trace_cached < 0) disp_trace_cached = (getenv("CUPID_TRACE_DISP") != NULL) ? 1 : 0;
  const u32 mask = disp_trace_cached ? 0u : 0x3Fu;
  if ((hw_trace_env_cached() || disp_trace_cached) && (dbg_n++ & mask) == 0)
    fprintf(stderr, "[hw update_display] disabled=%u vram=(%u,%u) %ux%u "
                    "active=%ux%u origin=(%u,%u) 24bit=%u il=%u il_il=%u\n",
            c->display_disabled, c->display_vram_left, c->display_vram_top,
            c->display_vram_width, c->display_vram_height,
            c->display_width, c->display_height,
            c->display_origin_left, c->display_origin_top,
            c->display_24bit, c->interlaced_display_enabled,
            c->interlaced_display_interleaved);

  if (c->display_disabled || c->display_vram_width == 0 || c->display_vram_height == 0) {
    /* Wipe the buffer so the presenter doesn't show last frame's pixels
     * during display-disabled gaps.  Use prior dims if known, else the
     * full capacity. */
    const u32 cw = (hw->display_width  != 0) ? hw->display_width  : (u32)GPU_MAX_DISPLAY_WIDTH;
    const u32 ch = (hw->display_height != 0) ? hw->display_height : (u32)GPU_MAX_DISPLAY_HEIGHT;
    gpu_hw_clear_display_buffer(hw, cw, ch);
    /* Mark the PBO ring stale: when display re-enables next frame the
     * extract path must drain in-flight frames before issuing again. */
    hw->vram_extract_prev_disabled = true;
    hw->display_width = hw->display_height = 0;
    return;
  }

  u32 out_w = c->display_width;
  u32 out_h = c->display_height;
  if (out_w == 0) out_w = c->display_vram_width;
  if (out_h == 0) out_h = c->display_vram_height << (c->interlaced_display_enabled ? 1u : 0u);
  if (out_w > GPU_MAX_DISPLAY_WIDTH)  out_w = GPU_MAX_DISPLAY_WIDTH;
  if (out_h > GPU_MAX_DISPLAY_HEIGHT) out_h = GPU_MAX_DISPLAY_HEIGHT;

  const bool interlaced = c->interlaced_display_enabled;

  /* Internal-resolution scaling.  24-bit FMV is bit-exact at native (chroma
   * smoothing kernel reads packed RGB888 with a known stride).  Interlaced
   * output stays at native too: scaled deinterlace would need a dedicated
   * shader/scatter pattern, deferred. */
  const u32 disp_scale  = (c->display_24bit || interlaced) ? 1u : (u32)hw->resolution_scale;
  const u32 out_w_s     = out_w * disp_scale;
  const u32 out_h_s     = out_h * disp_scale;

  const bool size_changed  = (hw->display_width != out_w_s || hw->display_height != out_h_s);
  if (size_changed || !interlaced) {
    const u32 px = out_w_s * out_h_s;
    if (px <= hw->display_buffer_capacity) {
      for (u32 i = 0; i < px; i++)
        hw->display_buffer[i] = 0xFF000000u;
    }
  }

  const u32 line_skip   = c->interlaced_display_interleaved ? 1u : 0u;
  const u32 field       = c->interlaced_display_field ? 1u : 0u;
  const u32 row_stride  = interlaced ? 2u : 1u;
  const u32 row_offset  = interlaced ? field : 0u;
  const u32 src_x       = c->display_vram_left;
  const u32 src_y       = (u32)c->display_vram_top +
                          ((c->interlaced_display_field & c->interlaced_display_interleaved) ? 1u : 0u);
  const u32 vram_w      = c->display_vram_width;
  const u32 vram_h      = c->display_vram_height;
  const u32 ox          = c->display_origin_left;
  const u32 oy          = c->display_origin_top;

  if (ox >= out_w || oy >= out_h) {
    /* Origin off the visible rectangle: nothing to copy this frame.
     * Clear so the presenter doesn't show the last in-bounds frame's
     * content. */
    gpu_hw_clear_display_buffer(hw, out_w_s, out_h_s);
    hw->display_width  = out_w_s;
    hw->display_height = out_h_s;
    return;
  }
  u32 copy_w        = (ox + vram_w              <= out_w) ? vram_w : (out_w - ox);
  u32 copy_h_dest   = (oy + vram_h * row_stride <= out_h) ? (vram_h * row_stride) : (out_h - oy);
  u32 copy_h_src    = copy_h_dest / row_stride;

  /* GL display compositor: dispatch vram_extract_pipeline into
   * vram_extract_texture, PBO-download into hw->display_buffer.  Falls back
   * to the SW row converter when the GL device path is unavailable, or when
   * the user opts in via gpu_use_software_renderer_for_readbacks (slow GL
   * driver readback workaround). */
  bool drew_gl = false;
  if (!g_settings.gpu_use_software_renderer_for_readbacks) {
    /* Scale-aware destination coords: at scale=N output dst_x_s = ox*N,
     * dst_y_s = oy*N etc.  The extract helper already scales the source
     * extent by ext_scale = disp_scale (forced =1 for 24-bit). */
    drew_gl = gpu_hw_extract_display_gl(
      hw, c, hw->display_buffer, /*dst_stride_pixels=*/out_w_s,
      /*dst_x=*/ox * disp_scale, /*dst_y=*/oy * disp_scale,
      /*dst_row_stride=*/row_stride, /*dst_row_offset=*/row_offset,
      /*dst_max_w=*/(out_w_s - ox * disp_scale),
      /*dst_max_h=*/out_h_s);
  }
  if (!drew_gl) {
    /* SW fallback path: byte-identical to gpu_sw's sw_update_display row
     * converter so the GL backend's "SW readbacks" mode produces frames
     * indistinguishable from the SW backend.  Native scale only. */
    const u32 dst_stride = out_w_s;
    for (u32 row = 0; row < copy_h_src; row++) {
      u32 row_pixels[GPU_MAX_DISPLAY_WIDTH];
      const u32 sy = src_y + (row << line_skip);
      if (c->display_24bit)
        hw_convert_row_24bit(row_pixels, src_x, sy, 0, copy_w);
      else
        hw_convert_row_15bit(row_pixels, src_x, sy, copy_w);
      const u32 dest_row = oy + row * row_stride + row_offset;
      if (dest_row >= out_h) break;
      u32* dst = &hw->display_buffer[dest_row * dst_stride + ox];
      for (u32 col = 0; col < copy_w; col++)
        dst[col] = row_pixels[col];
    }
  }

  /* 24-bit FMV chroma smoothing; shared with the SW backend.  Always at
   * native scale (disp_scale forced to 1 for 24-bit above). */
  if (c->display_24bit && g_settings.display_24bit_chroma_smoothing && !interlaced) {
    gpu_apply_chroma_smoothing_24bit(hw->display_buffer, out_w_s, ox, oy, copy_w, copy_h_dest);
  }

  hw->display_width  = out_w_s;
  hw->display_height = out_h_s;
}

static void hw_flush_render(gpu_backend_t* self)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  if (!hw->batch_vertex_ptr || hw->batch_index_count == 0u) {
    /* Nothing to draw; just unmap if mapped, leave unmapped on exit so
     * any subsequent utility draw against the same VBO doesn't fault on
     * GL_INVALID_OPERATION.  Batch buffers re-map lazily via
     * gpu_hw_ensure_batch_buffers_mapped() at the next batch entry. */
    if (hw->batch_vertex_ptr)
      gpu_hw_unmap_batch_buffers(hw);
    return;
  }

  const u32 base_vertex = hw->batch_base_vertex;
  const u32 base_index  = hw->batch_base_index;
  const u32 index_count = (u32)hw->batch_index_count;
  gpu_hw_unmap_batch_buffers(hw);

  /* Pick the right batch pipeline.  Covers untextured-opaque plus
   * textured + transparent + dither + check_mask. */
  const u32 dith_slot = (!hw->true_color && hw->batch.dithering) ? 0u
                       : (hw->true_color ? 4u : 0u);
  const u32 render_mode_slot = (hw->batch.transparency_mode != GPU_TRANSPARENCY_MODE_DISABLED) ? 1u : 0u;
  const u32 batch_idx = gpu_hw_batch_pipeline_index(
    hw->batch.use_depth_buffer ? 1u : 0u,
    (u32)hw->batch.transparency_mode,
    render_mode_slot,
    (u32)hw->batch.texture_mode,
    dith_slot,
    hw->batch.interlacing ? 1u : 0u,
    hw->batch.check_mask_before_draw ? 1u : 0u);
  /* Pick from the per-filter mirror row when the current
   * filter is non-NEAREST *and* the batch is textured.  Untextured
   * batches always use the canonical NEAREST array (the FS would be
   * byte-identical and the per-filter rows don't compile those slots).
   * If the per-filter slot happens to be NULL (e.g. the slot was skipped
   * during the compile pass for some reason), we still fall back to the
   * NEAREST array so the draw can proceed; preferable to a NULL bind. */
  const bool textured_batch = (hw->batch.texture_mode != GPU_HW_BATCH_TEXTURE_MODE_DISABLED);
  const bool use_filter_row = textured_batch &&
                              (g_settings.gpu_texture_filter != GPU_TEXTURE_FILTER_NEAREST) &&
                              ((u32)g_settings.gpu_texture_filter < (u32)GPU_TEXTURE_FILTER_COUNT);
  opengl_pipeline_t* pl = NULL;
  if (use_filter_row)
    pl = hw->batch_pipelines_filter[g_settings.gpu_texture_filter][batch_idx];
  if (!pl)
    pl = hw->batch_pipelines[batch_idx];
  if (!pl) {
    /* No pipeline for this combo (skipped at compile time, e.g. depth_test=1).
     * SW rasterizer already drew this batch.  Leave batch buffers unmapped;
     * lazy-map at next batch entry. */
    return;
  }

  /* Open a debug group for the batch draw so apitrace / RenderDoc
   * traces are easy to navigate.  No-op when KHR_debug isn't available. */
  opengl_device_push_debug_group("gpu_hw batch flush");

  /* Bind RT + viewport + scissor.  Scissor follows clamped_drawing_area
   * (inclusive bounds, native-VRAM scale), expanded to make right/bottom
   * exclusive and multiplied by resolution_scale.  When write_mask_as_depth
   * is on, the depth attachment carries the mask-bit state and gets bound
   * alongside the color RT. */
  gpu_texture_t* rts[1] = { &hw->vram_texture->base };
  gpu_texture_t* ds = hw->vram_depth_texture ? &hw->vram_depth_texture->base : NULL;
  opengl_device_set_render_targets(rts, 1, ds);
  opengl_device_set_pipeline((gpu_pipeline_t*)pl);

  /* Bind vram_read_texture as the FS sample source for textured
   * draws.  NULL sampler => default GL_NEAREST per cupid-ps1 convention.
   * When the current batch is in PAGE_TEXTURE mode AND the cache
   * has materialised a source texture (`batch_cache_texture` set during
   * emit), bind that instead; the FS samples the pre-decoded RGBA8
   * page rather than the live VRAM read texture. */
  if (hw->batch.texture_mode != GPU_HW_BATCH_TEXTURE_MODE_DISABLED) {
    if (hw->batch.texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE && hw->batch_cache_texture) {
      opengl_device_set_texture_sampler(0, &hw->batch_cache_texture->base, NULL);
    } else if (hw->vram_read_texture) {
      if (getenv("CUPID_GL_NO_BLIT") == NULL)
        gpu_hw_update_vram_read_texture(hw);
      opengl_device_set_texture_sampler(0, &hw->vram_read_texture->base, NULL);
    }
  }

  const u32 scale = (u32)hw->resolution_scale;
  const opengl_rect_t vp = { 0, 0,
                             (s32)((u32)VRAM_WIDTH  * scale),
                             (s32)((u32)VRAM_HEIGHT * scale) };
  opengl_device_set_viewport(vp);
  /* clamped_drawing_area uses inclusive bounds; convert to exclusive. */
  s32 sl = (s32)hw->base.clamped_drawing_area.left;
  s32 st = (s32)hw->base.clamped_drawing_area.top;
  s32 sr = (s32)hw->base.clamped_drawing_area.right + 1;
  s32 sb = (s32)hw->base.clamped_drawing_area.bottom + 1;
  if (sr <= sl || sb <= st) {
    sl = 0; st = 0;
    sr = (s32)VRAM_WIDTH; sb = (s32)VRAM_HEIGHT;
  }
  const opengl_rect_t sc = { sl * (s32)scale, st * (s32)scale,
                             sr * (s32)scale, sb * (s32)scale };
  opengl_device_set_scissor(sc);
  hw->drawing_area_changed = false;

  /* Push the per-batch UBO.  Fills u_texture_window (set by
   * gpu_hw_latch_batch_config from cmd->window) and u_src/dst_alpha_factor
   * for transparent draws.  Resolution-scale and interlace fields stay as
   * factory-initialised. */
  void* ubo = opengl_device_map_uniform_buffer((u32)sizeof(gpu_hw_batch_ubo_data_t));
  if (ubo) {
    memcpy(ubo, &hw->batch_ubo_data, sizeof(gpu_hw_batch_ubo_data_t));
    opengl_device_unmap_uniform_buffer((u32)sizeof(gpu_hw_batch_ubo_data_t));
    hw->renderer_stats.num_uniform_buffer_updates++;
    hw->batch_ubo_dirty = false;
  }

  opengl_device_draw_indexed(index_count, base_index, base_vertex);
  hw->renderer_stats.num_batches++;

  /* Mark the rasterized region dirty so the next vram_read_texture bind
   * (or copy_vram source-read) blits the freshly-drawn pixels from
   * vram_texture into vram_read_texture.  Without this, vram_read_texture
   * stays stale for any region that was last touched by polygon raster
   * (previously only DMA writes/copies marked dirty), causing:
   *   - textured draws sampling render-to-texture pages to read zeros /
   *     prior content (Crash menu backdrop tiles, doubled overlay text)
   *   - copy_vram reads from vram_read_texture to copy STALE data into
   *     vram_texture (clobbers framebuffer with tile garbage)
   * Mirrors upstream gpu_hw.cpp:879-880 AddDrawnRectangle.  Conservative:
   * uses clamped_drawing_area as the bbox instead of per-primitive. this
   * over-blits at most one rect per flush but avoids per-vertex bookkeeping.
   * The blit itself is lazy (next bind only), so this does not reintroduce
   * the prior perf issue (that was forced sync per-flush, removed in favour
   * of dirty-rect tracking which is what we add back here). */
  {
    const gpu_rect_t drawn = gpu_rect_make(
      (s32)hw->base.clamped_drawing_area.left,
      (s32)hw->base.clamped_drawing_area.top,
      (s32)hw->base.clamped_drawing_area.right + 1,
      (s32)hw->base.clamped_drawing_area.bottom + 1);
    if (!gpu_rect_is_empty(drawn))
      gpu_hw_mark_vram_dirty(hw, drawn);
  }

  /* Clear the per-batch cache texture pointer so the next batch
   * starts fresh (the next emit re-runs lookup_source on its own draw mode
   * + palette and re-stashes a fresh pointer). */
  hw->batch_cache_texture = NULL;

  /* Close the debug group opened above. */
  opengl_device_pop_debug_group();

  /* Leave batch buffers UNMAPPED.  Mirrors upstream gpu_hw.cpp:RestoreDevice-
   * Context, which doesn't re-map either; utility draws (FillVRAM /
   * UpdateVRAM / CopyVRAM / extract / read) issue glDrawArrays against this
   * same VBO via their own pipelines' VAOs, and a draw against a mapped
   * buffer raises GL_INVALID_OPERATION.  The batch entry points
   * (gpu_hw_handle_draw_polygon, ..._line, ..._rectangle) lazy-map via
   * gpu_hw_ensure_batch_buffers_mapped() before pushing vertices. */
}

static bool hw_do_state(gpu_backend_t* self, state_wrapper_t* sw)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;
  /* Flush any pending GPU work first so the GL-side VRAM is up to date.
   * SW mirror is already up to date because every draw dual-wrote g_vram. */
  hw_flush_render(self);

  /* Cache contents are never persisted; we drop them here so the
   * post-load state starts clean.  On save (writing) this only avoids
   * leaking stale GPU resources past the snapshot point; on load the
   * "everything dirty" rect markers below force re-upload from VRAM on the
   * next draw, so any cached entries would be invalid anyway. */
  if (hw->use_texture_cache)
    gpu_texture_cache_invalidate();

  state_wrapper_do_bytes(sw, g_vram,     sizeof(g_vram));
  state_wrapper_do_bytes(sw, g_gpu_clut, sizeof(g_gpu_clut));

  /* Mark everything dirty so the next draw re-uploads the full VRAM
   * texture (UpdateVRAMReadTexture path is deferred).  Until then the GL
   * side is rebuilt incrementally by the dual-write path. */
  hw->vram_dirty_draw_rect  = GPU_HW_VRAM_SIZE_RECT;
  hw->vram_dirty_write_rect = GPU_HW_VRAM_SIZE_RECT;
  hw->batch_ubo_dirty       = true;
  hw->drawing_area_changed  = true;

  return !state_wrapper_has_error(sw);
}

static void hw_destroy(gpu_backend_t* self)
{
  gpu_hw_t* hw = (gpu_hw_t*)self;

  /* Drop any in-flight batch slice before tearing down the device pipelines
   * (the unmap also pokes the device's stream buffer so it must run while
   * the GL context is still alive). */
  gpu_hw_unmap_batch_buffers(hw);

  /* Tear down the texture cache before destroying the GL device
   * pipelines/buffers.  Cache may hold opengl_texture_t* it owns; releasing
   * those before pipeline/buffer teardown keeps GL state ordering sane. */
  if (hw->use_texture_cache)
    gpu_texture_cache_shutdown();

  gpu_hw_destroy_pipelines(hw);
  gpu_hw_destroy_buffers(hw);

  free(self);
}

static const gpu_backend_vtable_t gpu_hw_vtable = {
   .read_vram            = hw_read_vram,
  .fill_vram            = hw_fill_vram,
  .update_vram          = hw_update_vram,
  .copy_vram            = hw_copy_vram,
  .draw_polygon         = hw_draw_polygon,
  .draw_rectangle       = hw_draw_rectangle,
  .draw_line            = hw_draw_line,
  .drawing_area_changed = hw_drawing_area_changed,
  .update_clut          = hw_update_clut,
  .clear_cache          = hw_clear_cache,
  .clear_vram           = hw_clear_vram,
  .update_display       = hw_update_display,
  .flush_render         = hw_flush_render,
  .do_state             = hw_do_state,
  .destroy              = hw_destroy, 
};

gpu_backend_t* gpu_backend_create_hardware_opengl(const window_info_t* wi, Error* err)
{
  (void)wi;

  /* Until the parallel-port `opengl_device.c` lands, `g_gpu_device` is
   * always NULL.  Refuse to create the HW back-end so the front-end can
   * gracefully fall back to SW rather than crashing on the first draw. */
  if (!g_gpu_device) {
    Error_set_string(err, "OpenGL GPU device not initialised (gpu_device = NULL); "
                          "the hardware OpenGL back-end requires opengl_device_create() first.");
    return NULL;
  }

  gpu_hw_t* hw = (gpu_hw_t*)calloc(1, sizeof(gpu_hw_t));
  if (!hw) {
    Error_set_string(err, "Out of memory allocating gpu_hw_t.");
    return NULL;
  }

  hw->base.vtable                  = &gpu_hw_vtable;
  hw->base.clamped_drawing_area    = (gpu_drawing_area_t){0};

  /* Settings -> initial state. */
  hw->resolution_scale         = (g_settings.gpu_resolution_scale > 0) ? g_settings.gpu_resolution_scale : 1u;
  hw->multisamples             = 1;   /* MSAA dropped. */
  hw->texture_filtering        = g_settings.gpu_texture_filter;
  hw->sprite_texture_filtering = g_settings.gpu_sprite_texture_filter;
  hw->line_detect_mode         = g_settings.gpu_line_detect_mode;
  hw->downsample_mode          = GPU_DOWNSAMPLE_MODE_DISABLED;       /* forced */
  hw->wireframe_mode           = g_settings.gpu_wireframe_mode;

  hw->true_color               = settings_is_using_true_color(&g_settings);
  hw->pgxp_depth_buffer        = settings_using_pgxp_depth_buffer(&g_settings);
  /* clamp_uvs / compute_uv_range mirror upstream gpu_hw.cpp:309-310:
   * non-NEAREST filtering or PGXP-on requires per-polygon UV clamping so
   * bilinear samples don't bleed across page/CLUT boundaries.
   * compute_polygon_uv_limits is the per-polygon prerequisite. */
  hw->clamp_uvs                = (g_settings.gpu_pgxp_enable
                                  || g_settings.gpu_texture_filter        != GPU_TEXTURE_FILTER_NEAREST
                                  || g_settings.gpu_sprite_texture_filter != GPU_TEXTURE_FILTER_NEAREST);
  hw->compute_uv_range         = hw->clamp_uvs;
  /* Sprite-mode pipelines aren't compiled in this build; gate the
   * sprite-vs-polygon UV interpolation toggle off until that lands.
   * Mirrors upstream's ShouldAllowSpriteMode but forced false. */
  hw->allow_sprite_mode        = false;
  /* Honor the gpu_texture_cache setting.  When off (default), the
   * texture cache is a no-op and behavior matches the cache-disabled path
   * byte-for-byte.  When on, the cache is initialized after
   * gpu_hw_create_buffers succeeds (further down) and consulted at draw time.
   *
   * TEMP: also accept `CUPID_TEXTURE_CACHE=1` so the smoke test can flip
   * the cache on without an INI/CLI knob (no frontend wiring yet).  We set
   * g_settings.gpu_texture_cache here so the cache-internal `enabled` gate
   * (gpu_hw_texture_cache.c reads g_settings directly) sees the same value.
   * Drop the env override once the user-facing toggle lands. */
  if (getenv("CUPID_TEXTURE_CACHE"))
    g_settings.gpu_texture_cache = true;
  hw->use_texture_cache        = g_settings.gpu_texture_cache;
  hw->texture_dumping          = false;                              /* forced */
  hw->allow_shader_blend       = false;                              /* forced */
  hw->use_rov_for_shader_blend = false;
  hw->prefer_shader_blend      = false;
  /* Mask-bit emulation via depth: covers check_mask + true_color+dither
   * combos that would otherwise need shader_blend (allow_shader_blend=false
   * forced).  The FS writes (oalpha * v_pos.z) to o_depth; check_mask draws
   * use depth_func=GREATER_OR_EQUAL so destination pixels with the mask bit
   * set reject the incoming write.  See gpu_hw_compile_batch_pipelines(). */
  hw->write_mask_as_depth      = true;

  /* Default rect state matches GPU_HW::INVALID_RECT. */
  hw->vram_dirty_draw_rect  = gpu_rect_invalid();
  hw->vram_dirty_write_rect = gpu_rect_invalid();
  hw->current_uv_rect       = gpu_rect_invalid();
  hw->current_draw_rect     = gpu_rect_invalid();

  hw->draw_mode.bits        = 0xFFFFFFFFu;            /* INVALID_DRAW_MODE_BITS */
  hw->batch_ubo_dirty       = true;
  hw->drawing_area_changed  = true;
  hw->depth_was_copied      = false;

  /* Initial UBO state: identity scale + neutral texture window. */
  hw->batch_ubo_data.u_resolution_scale            = (float)hw->resolution_scale;
  hw->batch_ubo_data.u_rcp_resolution_scale        = 1.0f / (float)hw->resolution_scale;
  hw->batch_ubo_data.u_resolution_scale_minus_one  = (float)hw->resolution_scale - 1.0f;
  hw->batch_ubo_data.u_src_alpha_factor            = 1.0f;
  hw->batch_ubo_data.u_dst_alpha_factor            = 0.0f;
  /* Default texture window = identity (and = 0xFF, or = 0x00 -> pass-through). */
  hw->batch_ubo_data.u_texture_window[0]           = 0xFFu;
  hw->batch_ubo_data.u_texture_window[1]           = 0xFFu;
  hw->batch_ubo_data.u_texture_window[2]           = 0x00u;
  hw->batch_ubo_data.u_texture_window[3]           = 0x00u;
  hw->texture_window_bits.bits = 0x0000FFFFu;  /* and=0xFF,0xFF or=0x00,0x00 */
  hw->texture_window_active = false;
  /* Default batch state; triggers config_changed on first draw. */
  hw->batch.texture_mode = GPU_HW_BATCH_TEXTURE_MODE_DISABLED;
  hw->batch.transparency_mode = GPU_TRANSPARENCY_MODE_DISABLED;

  /* Mask-bit-as-depth counter: starts at 1, increments per drawn vertex
   * pair / batch.  GetCurrentNormalizedVertexDepth() returns
   * 1 - (current_depth/65535) so the first draw maps to ~1.0f and later draws
   * decrease toward 0.  GREATER_OR_EQUAL depth_test then filters incoming
   * writes against destinations whose mask bit was set by an earlier draw. */
  hw->current_depth = 1;
  hw->last_depth_z  = 1.0f;

  /* Ensure g_vram + clut are coherent on creation (caller may transition from
   * a different back-end at runtime). */
  /* No allocation; g_vram is a static global owned by gpu_sw_rasterizer. */

  /* HW backend uses the SW rasterizer as a CPU-side mirror of every draw so
   * `g_vram` stays canonical for save state + the display compositor.  In SW
   * mode `gpu_sw.c::gpu_sw_create_backend` calls this; in HW mode we have to
   * call it ourselves or the dither LUT stays zeroed and every shaded /
   * dithered pixel rasterises to 0x0000 → black screen. */
  gpu_sw_rasterizer_init();

  /* Allocate VRAM RT + read + readback textures on the GL device.
   * Batch pipelines + upload buffer come up afterwards. */
  if (!gpu_hw_create_buffers(hw, err)) {
    Error_add_prefix(err, "Failed to create VRAM textures: ");
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }

  /* Common screen-quad vertex shaders + VRAM fill pipelines. */
  if (!gpu_hw_compile_common_shaders(hw, err)) {
    Error_add_prefix(err, "Failed to compile common shaders: ");
    gpu_hw_destroy_pipelines(hw);
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }
  if (!gpu_hw_compile_vram_fill_pipelines(hw, err)) {
    Error_add_prefix(err, "Failed to compile VRAM fill pipelines: ");
    gpu_hw_destroy_pipelines(hw);
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }
  if (!gpu_hw_compile_vram_write_pipelines(hw, err)) {
    Error_add_prefix(err, "Failed to compile VRAM write pipelines: ");
    gpu_hw_destroy_pipelines(hw);
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }
  if (!gpu_hw_compile_vram_copy_pipelines(hw, err)) {
    Error_add_prefix(err, "Failed to compile VRAM copy pipelines: ");
    gpu_hw_destroy_pipelines(hw);
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }
  if (!gpu_hw_compile_vram_extract_pipelines(hw, err)) {
    Error_add_prefix(err, "Failed to compile VRAM extract pipelines: ");
    gpu_hw_destroy_pipelines(hw);
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }

  /* Log key settings so the user sees the active configuration. */
  INFO_LOG("HW backend settings: resolution_scale=%ux, true_color=%s, texture_filter=%d.",
           (u32)hw->resolution_scale,
           hw->true_color ? "yes" : "no",
           (int)hw->texture_filtering);

  /* Eager pre-creation of the 3600-entry batch pipeline matrix.
   * cost on Mesa accepted. */
  INFO_LOG("Compiling %u batch pipelines (eager)...", (u32)GPU_HW_NUM_BATCH_PIPELINES);
  if (!gpu_hw_compile_batch_pipelines(hw, err)) {
    Error_add_prefix(err, "Failed to compile batch pipelines: ");
    gpu_hw_destroy_pipelines(hw);
    gpu_hw_destroy_buffers(hw);
    free(hw);
    return NULL;
  }

  /* Bring up the texture cache once the GL device + buffers are
   * ready.  No-op when `g_settings.gpu_texture_cache == false` (the cache is
   * also gated internally on the same flag). */
  if (hw->use_texture_cache) {
    if (!gpu_texture_cache_initialize(hw, err)) {
      Error_add_prefix(err, "Failed to initialize texture cache: ");
      gpu_texture_cache_shutdown();
      gpu_hw_destroy_pipelines(hw);
      gpu_hw_destroy_buffers(hw);
      free(hw);
      return NULL;
    }
    INFO_LOG("GPU texture cache enabled (max %u entries, %u MB).",
             g_settings.texture_replacements.config.max_hash_cache_entries,
             g_settings.texture_replacements.config.max_hash_cache_vram_usage_mb);
  }

  /* Initial clear so the first present samples a defined surface. */
  opengl_device_clear_render_target(&hw->vram_texture->base, 0u);
  /* Clear depth attachment to 0 (mask bit clear) so the first check_mask
   * draw doesn't reject against undefined depth. */
  if (hw->vram_depth_texture)
    opengl_device_clear_depth(&hw->vram_depth_texture->base, 0.0f);

  /* Batch buffers map lazily on the first vertex push via
   * gpu_hw_ensure_batch_buffers_mapped(); leaving them unmapped here keeps
   * any startup utility draw (FillVRAM clear) GL_INVALID_OPERATION-free. */

  g_gpu_backend = &hw->base;
  return &hw->base;
}

const u32* gpu_hw_get_display_buffer(u32* out_width, u32* out_height)
{
  if (!g_gpu_backend || g_gpu_backend->vtable != &gpu_hw_vtable) {
    if (out_width)  *out_width  = 0;
    if (out_height) *out_height = 0;
    return NULL;
  }
  gpu_hw_t* hw = (gpu_hw_t*)g_gpu_backend;
  if (out_width)  *out_width  = hw->display_width;
  if (out_height) *out_height = hw->display_height;
  return hw->display_buffer;
}

bool gpu_hw_change_resolution_scale(gpu_hw_t* hw, u8 new_scale, Error* err)
{
  if (!hw) {
    Error_set_string(err, "gpu_hw_change_resolution_scale: NULL hw");
    return false;
  }
  if (new_scale < 1u) new_scale = 1u;
  if (new_scale == hw->resolution_scale) {
    /* No-op. */
    return true;
  }

  hw_flush_render(&hw->base);

  /* Drop the old textures and download PBO; the GL pipelines themselves are
   * resolution-scale-agnostic so we keep them.  We must invalidate the
   * texture cache because cache textures are sized per-scale (256x256 RGBA8
   * staging, but the cache also tracks the source page hash). */
  if (hw->use_texture_cache)
    gpu_texture_cache_invalidate();
  gpu_hw_destroy_buffers(hw);

  hw->resolution_scale = new_scale;
  hw->batch_ubo_data.u_resolution_scale            = (float)new_scale;
  hw->batch_ubo_data.u_rcp_resolution_scale        = 1.0f / (float)new_scale;
  hw->batch_ubo_data.u_resolution_scale_minus_one  = (float)new_scale - 1.0f;
  hw->batch_ubo_dirty = true;

  if (!gpu_hw_create_buffers(hw, err)) {
    Error_add_prefix(err, "Failed to recreate VRAM textures at new scale: ");
    return false;
  }

  /* Batch buffers stay unmapped after scale change; lazy-map happens on the
   * next vertex push via gpu_hw_ensure_batch_buffers_mapped(). */

  /* Initial clear so the first draw on the new RT samples a defined surface. */
  opengl_device_clear_render_target(&hw->vram_texture->base, 0u);
  if (hw->vram_depth_texture)
    opengl_device_clear_depth(&hw->vram_depth_texture->base, 0.0f);

  /* Mark VRAM fully dirty so the next read from g_vram gets re-uploaded into
   * the new vram_texture (lands when texture-buffer bind is wired; until
   * then SW mirror keeps frontend output coherent). */
  hw->vram_dirty_draw_rect  = GPU_HW_VRAM_SIZE_RECT;
  hw->vram_dirty_write_rect = GPU_HW_VRAM_SIZE_RECT;
  hw->drawing_area_changed  = true;

  INFO_LOG("Resolution scale changed to %ux (VRAM RT: %ux%u).",
           (u32)new_scale,
           (u32)VRAM_WIDTH  * (u32)new_scale, 
           (u32)VRAM_HEIGHT * (u32)new_scale);
  return true;
}
