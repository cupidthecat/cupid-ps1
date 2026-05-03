/*
 * Hardware (OpenGL) GPU back-end.  Single-impl collapse of
 * `class GPU_HW final : public GPUBackend` to a flat `gpu_hw_t` struct that
 * embeds `gpu_backend_t base` at offset 0 and points the vtable at static
 * functions in gpu_hw.c - same shape as gpu_sw.c.
 *
 * Drop list:
 *   - PGXP precision uplift (DrawPrecisePolygon/DrawPreciseLine fold into
 *     the regular polygon/line path).
 *   - Downsampling (m_downsample_mode forced NONE).
 *   - Texture replacements / texture cache (m_use_texture_cache forced false).
 *   - Internal post-FX chain.
 *   - MSAA (m_multisamples = 1 always).
 *   - Shader blending (m_use_shader_blending forced false).
 * Pipeline array's leading "msaa" dimension is dropped → 3600 entries instead
 * of the MSAA-included matrix (=> still indexed by [depth_test]
 * [transparency_mode][render_mode][texture_mode][dithering][interlacing]
 * [check_mask]).  Of these 3600 slots, ~540 are populated per filter pass
 * (the remaining 3060 are depth-test-only stubs that stay NULL until PGXP
 * depth / write_mask_as_depth lands).
 */

#ifndef CUPID_CORE_GPU_HW_H
#define CUPID_CORE_GPU_HW_H

#include "common/types.h"
#include "core/gpu_backend.h"
#include "core/gpu_hw_shadergen.h"
#include "core/gpu_types.h"
#include "core/types.h"
#include "util/gpu_device.h"
#include "util/gpu_texture.h"
#include "util/gpu_types.h"
#include "util/window_info.h"

typedef struct Error Error;
typedef struct opengl_texture        opengl_texture_t;
typedef struct opengl_sampler        opengl_sampler_t;
typedef struct opengl_texture_buffer opengl_texture_buffer_t;
typedef struct opengl_download_texture opengl_download_texture_t;
typedef struct opengl_pipeline       opengl_pipeline_t;
typedef struct opengl_shader         opengl_shader_t;

typedef struct {
  s32 left;
  s32 top;
  s32 right;
  s32 bottom;
} gpu_rect_t;

ALWAYS_INLINE gpu_rect_t gpu_rect_make(s32 l, s32 t, s32 r, s32 b)
{ gpu_rect_t v = { l, t, r, b }; return v; }

ALWAYS_INLINE gpu_rect_t gpu_rect_invalid(void)
{
  gpu_rect_t v = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
  return v;
}
ALWAYS_INLINE gpu_rect_t gpu_rect_zero(void)
{ gpu_rect_t v = { 0, 0, 0, 0 }; return v; }

ALWAYS_INLINE bool gpu_rect_eq(gpu_rect_t a, gpu_rect_t b)
{ return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

ALWAYS_INLINE bool gpu_rect_is_empty(gpu_rect_t r)
{ return r.right <= r.left || r.bottom <= r.top; }

ALWAYS_INLINE s32 gpu_rect_width (gpu_rect_t r) { return r.right  - r.left; }
ALWAYS_INLINE s32 gpu_rect_height(gpu_rect_t r) { return r.bottom - r.top;  }
ALWAYS_INLINE s32 gpu_rect_area  (gpu_rect_t r)
{ return gpu_rect_is_empty(r) ? 0 : gpu_rect_width(r) * gpu_rect_height(r); }

ALWAYS_INLINE bool gpu_rect_contains_point(gpu_rect_t r, s32 x, s32 y)
{ return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

ALWAYS_INLINE gpu_rect_t gpu_rect_intersect(gpu_rect_t a, gpu_rect_t b)
{
  gpu_rect_t v;
  v.left   = a.left   > b.left   ? a.left   : b.left;
  v.top    = a.top    > b.top    ? a.top    : b.top;
  v.right  = a.right  < b.right  ? a.right  : b.right;
  v.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
  return v;
}

ALWAYS_INLINE gpu_rect_t gpu_rect_union(gpu_rect_t a, gpu_rect_t b)
{
  gpu_rect_t v;
  v.left   = a.left   < b.left   ? a.left   : b.left;
  v.top    = a.top    < b.top    ? a.top    : b.top;
  v.right  = a.right  > b.right  ? a.right  : b.right;
  v.bottom = a.bottom > b.bottom ? a.bottom : b.bottom;
  return v;
}

ALWAYS_INLINE bool gpu_rect_overlaps(gpu_rect_t a, gpu_rect_t b)
{ return !(a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top); }

ALWAYS_INLINE gpu_rect_t gpu_rect_scale(gpu_rect_t r, s32 mul)
{ return gpu_rect_make(r.left * mul, r.top * mul, r.right * mul, r.bottom * mul); }

ALWAYS_INLINE gpu_rect_t gpu_rect_expand(gpu_rect_t r, s32 amt)
{ return gpu_rect_make(r.left - amt, r.top - amt, r.right + amt, r.bottom + amt); }

/* Constants for VRAM-sized rectangles. */
#define GPU_HW_VRAM_SIZE_RECT  gpu_rect_make(0, 0, (s32)VRAM_WIDTH, (s32)VRAM_HEIGHT)

typedef struct {
  float x, y, z, w;
  u32   color;
  u32   texpage;
  u16   u, v;
  u32   uv_limits;
} gpu_hw_batch_vertex_t;

typedef struct {
  gpu_hw_batch_texture_mode_t texture_mode;
  gpu_transparency_mode_t     transparency_mode;
  bool dithering;
  bool interlacing;
  bool set_mask_while_drawing;
  bool check_mask_before_draw;
  bool use_depth_buffer;
  bool sprite_mode;
} gpu_hw_batch_config_t;

typedef struct {
  u32   u_texture_window[4];   /* and_x, and_y, or_x, or_y */
  float u_src_alpha_factor;
  float u_dst_alpha_factor;
  u32   u_interlaced_displayed_field;
  u32   u_set_mask_while_drawing;
  float u_resolution_scale;
  float u_rcp_resolution_scale;
  float u_resolution_scale_minus_one;
} gpu_hw_batch_ubo_data_t;

#define GPU_HW_BATCH_PIPELINE_DEPTHS              2u
/* TRANSPARENCY_MODES sized to fit the raw enum (0..3 = the four blend ops,
 * 4 = DISABLED).  The encoder stores `i = i*DIM + value` and lookups pass
 * raw `(u32)trans_mode`, so a smaller dim would let the value leak into the
 * next dim and produce out-of-bounds indices into batch_pipelines.  Each
 * slot gets a pipeline with the per-mode blend state (HALF_BG / BG_PLUS_FG /
 * BG_MINUS_FG-as-REVERSE_SUBTRACT / BG_PLUS_QUARTER_FG / no-blend). */
#define GPU_HW_BATCH_PIPELINE_TRANSPARENCY_MODES  5u
/* RENDER_MODES kept at 2 because the lookup at draw time always converts
 * trans_mode to a 0/1 on/off bit (`(trans != DISABLED) ? 1 : 0`).  Slots
 * 0 and 1 cover all the live combos. */
#define GPU_HW_BATCH_PIPELINE_RENDER_MODES        2u
#define GPU_HW_BATCH_PIPELINE_TEXTURE_MODES       (u32)GPU_HW_BATCH_TEXTURE_MODE_MAX_COUNT  /* 9 */
#define GPU_HW_BATCH_PIPELINE_DITHERING           5u
#define GPU_HW_BATCH_PIPELINE_INTERLACING         5u
#define GPU_HW_BATCH_PIPELINE_CHECK_MASK          2u

#define GPU_HW_NUM_BATCH_PIPELINES                                                                 \
  (GPU_HW_BATCH_PIPELINE_DEPTHS *                                                                  \
   GPU_HW_BATCH_PIPELINE_TRANSPARENCY_MODES *                                                      \
   GPU_HW_BATCH_PIPELINE_RENDER_MODES *                                                            \
   GPU_HW_BATCH_PIPELINE_TEXTURE_MODES *                                                           \
   GPU_HW_BATCH_PIPELINE_DITHERING *                                                               \
   GPU_HW_BATCH_PIPELINE_INTERLACING *                                                             \
   GPU_HW_BATCH_PIPELINE_CHECK_MASK)
/* sanity: 2*2*2*9*5*5*2 == 3600. */

ALWAYS_INLINE u32 gpu_hw_batch_pipeline_index(u32 depth_test, u32 transparency_mode, u32 render_mode,
                                              u32 texture_mode, u32 dithering, u32 interlacing,
                                              u32 check_mask)
{
  u32 i = 0;
  i = i * GPU_HW_BATCH_PIPELINE_DEPTHS              + depth_test;
  i = i * GPU_HW_BATCH_PIPELINE_TRANSPARENCY_MODES  + transparency_mode;
  i = i * GPU_HW_BATCH_PIPELINE_RENDER_MODES        + render_mode;
  i = i * GPU_HW_BATCH_PIPELINE_TEXTURE_MODES       + texture_mode;
  i = i * GPU_HW_BATCH_PIPELINE_DITHERING           + dithering;
  i = i * GPU_HW_BATCH_PIPELINE_INTERLACING         + interlacing;
  i = i * GPU_HW_BATCH_PIPELINE_CHECK_MASK          + check_mask;
  return i;
}

typedef struct {
  u32 num_batches;
  u32 num_vram_read_texture_updates;
  u32 num_uniform_buffer_updates;
} gpu_hw_renderer_stats_t;

typedef struct gpu_hw {
  gpu_backend_t base;

  /* GL device-owned objects.  Most of these stay NULL until the parallel-port
   * opengl_device.c lands (gpu_backend_create_hardware_opengl returns NULL+err
   * if g_gpu_device is NULL).  The pointers below are non-owning; the GPU
   * device is responsible for free.  Until then this is a safe inert state. */
  opengl_texture_t*          vram_texture;
  opengl_texture_t*          vram_depth_texture;
  opengl_texture_t*          vram_depth_copy_texture;
  opengl_texture_t*          vram_read_texture;
  opengl_texture_t*          vram_readback_texture;
  opengl_download_texture_t* vram_readback_download_texture;
  opengl_texture_buffer_t*   vram_upload_buffer;
  opengl_texture_t*          vram_write_texture;

  /* Streaming batch buffer (mapped from the device's vertex/index streams). */
  gpu_hw_batch_vertex_t* batch_vertex_ptr;
  u16*                   batch_index_ptr;
  u32                    batch_base_vertex;
  u32                    batch_base_index;
  u16                    batch_vertex_count;
  u16                    batch_index_count;
  u16                    batch_vertex_space;
  u16                    batch_index_space;

  /* Depth buffer state (mask-bit emulation). */
  s32   current_depth;
  float last_depth_z;

  /* Resolution / sample config.  m_multisamples is always 1. */
  u8 resolution_scale;
  u8 multisamples;

  /* Filter / detect / wireframe modes (mirrored from settings).  Downsample
   * forced to NONE and texture-cache forced false. */
  gpu_texture_filter_t   texture_filtering;
  gpu_texture_filter_t   sprite_texture_filtering;
  gpu_line_detect_mode_t line_detect_mode;
  gpu_downsample_mode_t  downsample_mode;
  gpu_wireframe_mode_t   wireframe_mode;

  /* Capability + setting bits. */
  bool supports_dual_source_blend : 1;
  bool supports_framebuffer_fetch : 1;
  bool true_color                 : 1;
  bool pgxp_depth_buffer          : 1;
  bool clamp_uvs                  : 1;
  bool compute_uv_range           : 1;
  bool allow_sprite_mode          : 1;
  bool allow_shader_blend         : 1;
  bool prefer_shader_blend        : 1;
  bool use_rov_for_shader_blend   : 1;
  bool write_mask_as_depth        : 1;
  bool texture_window_active      : 1;
  bool rov_active                 : 1;
  bool draw_with_software_renderer: 1;
  bool use_texture_cache          : 1;   /* Forced false. */
  bool texture_dumping            : 1;   /* Forced false. */

  u8   texpage_dirty;
  bool batch_ubo_dirty;
  bool drawing_area_changed;
  bool depth_was_copied;

  gpu_hw_batch_config_t   batch;
  gpu_hw_batch_ubo_data_t batch_ubo_data;

  /* When the texture cache is enabled and the current batch is in
   * PAGE_TEXTURE mode, this holds the cached source's GPU texture pointer.
   * Bound at flush time instead of vram_read_texture.  NULL when the cache
   * is off, the batch isn't PAGE_TEXTURE, or lookup hasn't materialised a
   * texture yet (in which case the emit-side downgrades to per-mode). */
  opengl_texture_t* batch_cache_texture;

  gpu_rect_t vram_dirty_draw_rect;
  gpu_rect_t vram_dirty_write_rect;
  gpu_rect_t current_uv_rect;
  gpu_rect_t current_draw_rect;
  s32        current_texture_page_offset[2];

  /* Latched draw mode bits (texture-relevant only; raw `bits` is canonical). */
  union {
    struct {
      gpu_draw_mode_reg_t       mode_reg;
      gpu_texture_palette_reg_t palette_reg;
    };
    u32 bits;
  } draw_mode;
  gpu_texture_window_t texture_window_bits;

  /* Pipeline pool.
   *
   * A parallel `batch_pipelines_bilinear[]` array exists for the
   * bilinear sprite filter, and is generalised into a 2-D
   * `batch_pipelines_filter[FILTER][SLOT]` array indexed by the filter mode
   * from `gpu_texture_filter_t`.  Slot rules within each filter row are
   * identical to the canonical NEAREST array (`batch_pipelines[]`); the
   * lookup at flush time switches rows based on `hw->texture_filtering`
   * (see hw_flush_render).  Untextured slots only compile in the NEAREST
   * row; the FS body for textured filtering gates on `textured`, so an
   * untextured FS would be byte-identical to the NEAREST one and the
   * non-NEAREST rows leave those slots NULL.
   *
   * The flat `batch_pipelines[]` array stays the canonical NEAREST view
   * (referenced by emit-side null checks at draw time as a "is this combo
   * supported?" sentinel).  Non-NEAREST rows live in
   * `batch_pipelines_filter[]`; row index 0 (NEAREST) is unused and stays
   * NULL throughout the program lifetime. */
  opengl_pipeline_t* batch_pipelines        [GPU_HW_NUM_BATCH_PIPELINES];
  opengl_pipeline_t* batch_pipelines_filter [GPU_TEXTURE_FILTER_COUNT][GPU_HW_NUM_BATCH_PIPELINES];
  opengl_pipeline_t* wireframe_pipeline;
  opengl_pipeline_t* vram_fill_pipelines[2][2];           /* [wrapped][interlaced] */
  opengl_pipeline_t* vram_write_pipelines[2];             /* [depth_test] */
  opengl_pipeline_t* vram_copy_pipelines[2];              /* [depth_test] */
  opengl_pipeline_t* vram_readback_pipeline;
  opengl_pipeline_t* vram_update_depth_pipeline;
  opengl_pipeline_t* vram_extract_pipeline[3];            /* 0=15bit, 1=24bit, 2=depth */
  opengl_pipeline_t* copy_depth_pipeline;
  opengl_pipeline_t* clear_depth_pipeline;

  opengl_shader_t* fullscreen_quad_vertex_shader;
  opengl_shader_t* screen_quad_vertex_shader;

  /* Display extract textures. */
  opengl_texture_t*           vram_extract_texture;
  opengl_texture_t*           vram_extract_depth_texture;
  opengl_download_texture_t*  vram_extract_download_texture;
  u32                         vram_extract_capacity_w;
  u32                         vram_extract_capacity_h;

  /* Async PBO-ring readback for the display extract path.
   *
   * On Mesa Intel iGPU the synchronous flush-and-map pattern stalls the audio
   * thread (heavy underruns + Stretcher resets).  The fix is an N=3 ring of
   * download textures (each is a persistent-mapped PBO when the driver
   * supports GL_ARB_buffer_storage, otherwise a per-frame mapped PBO):
   *
   *   - Each frame, issue glCopyImageSubData (or a glReadPixels into the PBO)
   *     into ring[i] and fence ring[i].
   *   - Read back ring[(i+1)%N]; by then its fence is signaled (N-1 frames
   *     of latency), so glClientWaitSync(0) returns immediately, and we map
   *     and copy into hw->display_buffer.
   *   - First N-1 frames the ring is cold; gpu_hw_extract_display_gl returns
   *     false so the caller falls back to the SW row converter (or shows
   *     black, depending on caller).
   *
   * `extract_ring_pending[i]` indicates whether ring[i] has a copy in flight.
   * `extract_ring_w/h[i]` cache the copied region size so the consumer side
   * knows how much to map and read.  `extract_ring_meta[i]` caches the
   * destination scatter parameters (dst_x/y, dst_row_stride/offset, dst_max_w/h,
   * dst_stride_pixels) so the consumer side scatters into hw->display_buffer
   * even though the display rectangle may have changed in the meantime. */
  #define GPU_HW_EXTRACT_RING_N 3u
  opengl_download_texture_t*  vram_extract_download_ring[3];
  u32                         vram_extract_ring_w[3];
  u32                         vram_extract_ring_h[3];
  bool                        vram_extract_ring_pending[3];
  /* Destination scatter params captured at copy-issue time. */
  struct {
    u32 dst_stride_pixels;
    u32 dst_x;
    u32 dst_y;
    u32 dst_row_stride;
    u32 dst_row_offset;
    u32 dst_max_w;
    u32 dst_max_h;
  } vram_extract_ring_meta[3];
  u32                         vram_extract_ring_index;

  /* Last-issued scatter parameters (copy of meta plus scaled extract size).
   * When any of these change between frames the in-flight ring slots hold
   * stale stride/field/origin data that would scatter to the wrong rows of
   * the new frame's display_buffer (visible as horizontal stripes during
   * scene transitions / interlace-field flips / display-origin moves).
   * On mismatch the consumer drains every pending slot, resets the ring
   * index, and falls back to the SW row converter for the transition
   * frame.  `vram_extract_last_valid` gates the comparison on the very
   * first issue. */
  struct {
    u32 dst_stride_pixels;
    u32 dst_x;
    u32 dst_y;
    u32 dst_row_stride;
    u32 dst_row_offset;
    u32 dst_max_w;
    u32 dst_max_h;
    u32 scaled_w;
    u32 scaled_h;
    u32 vram_off_x;       /* Source VRAM offset (changes on double-buffer flip). */
    u32 vram_off_y;
  } vram_extract_last_issued;
  bool                        vram_extract_last_valid;
  bool                        vram_extract_prev_disabled;

  /* Statistics. */
  gpu_hw_renderer_stats_t renderer_stats;

  u32* display_buffer;            /* heap-alloc; sized for max display × scale². */
  u32  display_buffer_capacity;   /* in pixels */
  u32  display_width;
  u32  display_height;
} gpu_hw_t;

gpu_backend_t* gpu_backend_create_hardware_opengl(const window_info_t* wi, Error* err);

const u32* gpu_hw_get_display_buffer(u32* out_width, u32* out_height);

/* Live-toggle of the internal resolution scale.  Recreates the
 * VRAM render target / read texture / readback target / readback PBO at
 * (VRAM_WIDTH × VRAM_HEIGHT × new_scale) and flushes the in-flight batch
 * so the next draw lands on the resized RT.  Returns false on failure
 * (with `err` populated); the back-end is then left in an undefined
 * state and the caller should re-create it.
 *
 * NOTE: there is no settings-change hook in cupid-ps1's frontend yet, so this
 * is currently called only from the boot path (where it's redundant with
 * the factory's `gpu_resolution_scale` read); it's included here so the
 * API is in place when the settings-change wiring lands. */
bool gpu_hw_change_resolution_scale(gpu_hw_t* hw, u8 new_scale, Error* err);

#endif /* CUPID_CORE_GPU_HW_H */
