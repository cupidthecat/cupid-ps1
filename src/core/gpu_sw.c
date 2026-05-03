/*
 * Software back-end implementation.  All commands forward into
 * gpu_sw_rasterizer.{c,h}; here we just route polygon/sprite/line draws
 * through the right rasterizer entry point and split quads into two
 * triangles.  The display copy paths produce RGBA8 into a fixed host
 * framebuffer (max 720x576 = PAL interlaced) for the frontend to consume.
 */

#include "core/gpu_sw.h"
#include "core/gpu_helpers.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/settings.h"
#include "common/assert.h"
#include "common/log.h"
#include "util/state_wrapper.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPU_SW);

/* Max display buffer size = PAL interlaced 720x576 RGBA8. */
#define GPU_SW_MAX_DISPLAY_PIXELS (GPU_MAX_DISPLAY_WIDTH * GPU_MAX_DISPLAY_HEIGHT)

typedef struct {
  gpu_backend_t base;            /* must be first; container_of(=base ptr) */
  u32           display_buffer[GPU_SW_MAX_DISPLAY_PIXELS];
  u32           display_width;
  u32           display_height;
} gpu_sw_t;

static void sw_read_vram(gpu_backend_t* self, const gpu_backend_read_vram_cmd_t* c)
{
  /* Nothing to do; reads come from g_vram directly via gpu.c's GPUREAD.
   * The HW renderer uses this hook to flush its framebuffer. */
  (void)self; (void)c;
}

static void sw_fill_vram(gpu_backend_t* self, const gpu_backend_fill_vram_cmd_t* c)
{
  (void)self;
  gpu_sw_rasterizer_fill_vram(c->x, c->y, c->width, c->height, c->color,
                              c->interlaced_rendering, c->active_line_lsb);
}

/* Cache CUPID_TRACE env-var lookup once at first call.  getenv() walks the
 * full environment string list per call, and update_vram fires hundreds of
 * times per FMV frame; the per-call cost was visible in stalls during MDEC
 * playback even before the fprintf to stderr added line-buffered I/O on top. */
static int trace_env_cached(void)
{
  static int cached = -1;
  if (cached < 0)
    cached = (getenv("CUPID_TRACE") != NULL) ? 1 : 0;
  return cached;
}

static void sw_update_vram(gpu_backend_t* self, const gpu_backend_update_vram_cmd_t* c, const u16* data)
{
  (void)self;
  /* Rate-limit: print only every 64th update.  RE2's FMV blits ~50 vrams/frame,
   * un-throttled trace blocks the emu thread on stderr writes. */
  static u32 dbg_n = 0;
  if (trace_env_cached() && (dbg_n++ & 0x3F) == 0)
    fprintf(stderr, "[update_vram] dst=(%u,%u) %ux%u\n", c->x, c->y, c->width, c->height);
  gpu_sw_rasterizer_write_vram(c->x, c->y, c->width, c->height, data,
                               c->set_mask_while_drawing, c->check_mask_before_draw);
}

static void sw_copy_vram(gpu_backend_t* self, const gpu_backend_copy_vram_cmd_t* c)
{
  (void)self;
  gpu_sw_rasterizer_copy_vram(c->src_x, c->src_y, c->dst_x, c->dst_y,
                              c->width, c->height,
                              c->set_mask_while_drawing, c->check_mask_before_draw);
}

static void sw_draw_polygon(gpu_backend_t* self, const gpu_backend_draw_polygon_cmd_t* c,
                            const gpu_backend_polygon_vertex_t* vertices)
{
  (void)self;
  gpu_sw_rasterizer_draw_triangle(&c->base, &vertices[0], &vertices[1], &vertices[2]);
  if (c->base.num_vertices > 3) {
    /* Quad: second triangle re-uses v2/v1/v3. */
    gpu_sw_rasterizer_draw_triangle(&c->base, &vertices[2], &vertices[1], &vertices[3]);
  }
}

static void sw_draw_rectangle(gpu_backend_t* self, const gpu_backend_draw_rectangle_cmd_t* c)
{
  /* Cull fully off-screen rectangles before dispatch (matches upstream
   * GPU_SW::DrawSprite). The rasterizer loop also clips per-row, but a
   * fully-out rect still pays the loop cost without this short-circuit. */
  const s32 x0 = c->x;
  const s32 y0 = c->y;
  const s32 x1 = x0 + (s32)c->width;
  const s32 y1 = y0 + (s32)c->height;
  if (x1 <= (s32)self->clamped_drawing_area.left  ||
      x0 >  (s32)self->clamped_drawing_area.right ||
      y1 <= (s32)self->clamped_drawing_area.top   ||
      y0 >  (s32)self->clamped_drawing_area.bottom)
    return;
  gpu_sw_rasterizer_draw_rectangle(c);
}

static void sw_draw_line(gpu_backend_t* self, const gpu_backend_draw_line_cmd_t* c,
                         const gpu_backend_line_vertex_t* vertices)
{
  (void)self;
  for (u16 i = 0; i + 1 < c->base.num_vertices; i += 2)
    gpu_sw_rasterizer_draw_line(&c->base, &vertices[i], &vertices[i + 1]);
}

/* Precise variants discard the float perspective coords; the SW rasterizer
 * is integer-only.  Floats are preserved on the wire so a future HW backend
 * (GL) can pick them up without re-plumbing gpu_commands.c. */
static void sw_draw_precise_polygon(gpu_backend_t* self,
                                    const gpu_backend_draw_precise_polygon_cmd_t* c,
                                    const gpu_backend_precise_polygon_vertex_t* pv)
{
  (void)self;
  gpu_backend_polygon_vertex_t v[4] = {{0}};
  const u16 n = c->base.num_vertices < 4 ? c->base.num_vertices : 4;
  for (u16 i = 0; i < n; i++) {
    v[i].x = pv[i].native_x;
    v[i].y = pv[i].native_y;
    v[i].color = pv[i].color;
    v[i].texcoord = pv[i].texcoord;
  }
  gpu_sw_rasterizer_draw_triangle(&c->base, &v[0], &v[1], &v[2]);
  if (n > 3)
    gpu_sw_rasterizer_draw_triangle(&c->base, &v[2], &v[1], &v[3]);
}

static void sw_draw_precise_line(gpu_backend_t* self,
                                 const gpu_backend_draw_precise_line_cmd_t* c,
                                 const gpu_backend_precise_line_vertex_t* pv)
{
  (void)self;
  for (u16 i = 0; i + 1 < c->base.num_vertices; i += 2) {
    gpu_backend_line_vertex_t a, b;
    a.x = pv[i].native_x;     a.y = pv[i].native_y;     a.color = pv[i].color;
    b.x = pv[i+1].native_x;   b.y = pv[i+1].native_y;   b.color = pv[i+1].color;
    gpu_sw_rasterizer_draw_line(&c->base, &a, &b);
  }
}

static void sw_drawing_area_changed(gpu_backend_t* self, const gpu_backend_set_drawing_area_cmd_t* c)
{
  self->clamped_drawing_area = c->new_area;
  gpu_sw_drawing_area = c->new_area;
}

static void sw_update_clut(gpu_backend_t* self, const gpu_backend_update_clut_cmd_t* c)
{
  (void)self;
  gpu_sw_rasterizer_update_clut(c->reg, c->clut_is_8bit);
}

static void sw_clear_cache(gpu_backend_t* self) { (void)self; }

static void sw_clear_vram(gpu_backend_t* self)
{
  (void)self;
  memset(g_vram, 0, sizeof(g_vram));
  memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
}

/* Convert one VRAM row (15-bit RGBA5551) to RGBA8 into dst[0..width). */
static void sw_convert_row_15bit(u32* dst, u32 src_x, u32 src_y, u32 width)
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

/* Convert one VRAM row of 24-bit packed RGB to RGBA8 into dst[0..width). */
static void sw_convert_row_24bit(u32* dst, u32 src_x, u32 src_y, u32 skip_x, u32 width)
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

static void sw_update_display(gpu_backend_t* self, const gpu_backend_update_display_cmd_t* c)
{
  gpu_sw_t* sw = (gpu_sw_t*)self;
  static u32 dbg_n = 0;
  if (trace_env_cached() && (dbg_n++ & 0x3F) == 0)
    fprintf(stderr, "[update_display] disabled=%u vram=(%u,%u) %ux%u "
                    "active=%ux%u origin=(%u,%u) 24bit=%u il=%u il_il=%u\n",
            c->display_disabled, c->display_vram_left, c->display_vram_top,
            c->display_vram_width, c->display_vram_height,
            c->display_width, c->display_height,
            c->display_origin_left, c->display_origin_top,
            c->display_24bit, c->interlaced_display_enabled, 
            c->interlaced_display_interleaved);

  if (c->display_disabled || c->display_vram_width == 0 || c->display_vram_height == 0) {
    sw->display_width = sw->display_height = 0;
    return;
  }

  /* Output framebuffer dimensions = full active raster (display_width x
   * display_height).  The VRAM region is composited at (display_origin_left,
   * display_origin_top); inactive raster around it stays black.  Matches
   * is drawn into the destination active raster, and shaders handle the
   * deinterlace.  Without this padding the frontend sees only the cropped
   * VRAM region (e.g. 640x239 instead of 640x480 for the 480i BIOS logo). */
  u32 out_w = c->display_width;
  u32 out_h = c->display_height;
  if (out_w == 0) out_w = c->display_vram_width;
  if (out_h == 0) out_h = c->display_vram_height << (c->interlaced_display_enabled ? 1u : 0u);
  if (out_w > GPU_MAX_DISPLAY_WIDTH)  out_w = GPU_MAX_DISPLAY_WIDTH;
  if (out_h > GPU_MAX_DISPLAY_HEIGHT) out_h = GPU_MAX_DISPLAY_HEIGHT;

  /* For interlaced display we WEAVE: keep prev frame's other-field rows in
   * the persistent display_buffer and overwrite only this field's rows.  Bob
   * (full clear + row-dup) made interlaced static images flicker between
   * field-0-only and field-1-only renders, which on the BIOS logo looks like
   * a quarter of the diamond strobing.  Only clear when geometry changes. */
  const bool interlaced = c->interlaced_display_enabled;
  const bool size_changed = (sw->display_width != out_w || sw->display_height != out_h);
  if (size_changed || !interlaced) {
    for (u32 i = 0; i < out_w * out_h; i++)
      sw->display_buffer[i] = 0xFF000000u;
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

  /* Clip the source region to the output rectangle. */
  if (ox >= out_w || oy >= out_h) {
    sw->display_width  = out_w;
    sw->display_height = out_h;
    return;
  }
  u32 copy_w        = (ox + vram_w              <= out_w) ? vram_w : (out_w - ox);
  u32 copy_h_dest   = (oy + vram_h * row_stride <= out_h) ? (vram_h * row_stride) : (out_h - oy);
  u32 copy_h_src    = copy_h_dest / row_stride;

  for (u32 row = 0; row < copy_h_src; row++) {
    u32 row_pixels[GPU_MAX_DISPLAY_WIDTH];
    const u32 sy = src_y + (row << line_skip);
    if (c->display_24bit)
      sw_convert_row_24bit(row_pixels, src_x, sy, 0, copy_w);
    else
      sw_convert_row_15bit(row_pixels, src_x, sy, copy_w);
    const u32 dest_row = oy + row * row_stride + row_offset;
    if (dest_row >= out_h) break;
    u32* dst = &sw->display_buffer[dest_row * out_w + ox];
    for (u32 col = 0; col < copy_w; col++)
      dst[col] = row_pixels[col];
  }

  /* Smooth 24-bit FMV chroma blocks when enabled (RE2 / FMV intros).
   * Limited to the active FMV sub-rect (ox, oy)..(ox+copy_w, oy+copy_h_dest)
   * so the surrounding letterbox border isn't touched.  Skipped for
   * interlaced output; bob/weave would need per-field passes and FMVs
   * are usually progressive anyway. */
  if (c->display_24bit && g_settings.display_24bit_chroma_smoothing && !interlaced) {
    gpu_apply_chroma_smoothing_24bit(sw->display_buffer, out_w, ox, oy, copy_w, copy_h_dest);
  }

  sw->display_width  = out_w;
  sw->display_height = out_h;

}

static void sw_flush_render(gpu_backend_t* self) { (void)self; }

static bool sw_do_state(gpu_backend_t* self, state_wrapper_t* sw)
{
  (void)self;
  state_wrapper_do_bytes(sw, g_vram, sizeof(g_vram));
  state_wrapper_do_bytes(sw, g_gpu_clut, sizeof(g_gpu_clut));
  return !state_wrapper_has_error(sw);
}

static void sw_destroy(gpu_backend_t* self)
{
  free(self);
}

static const gpu_backend_vtable_t gpu_sw_vtable = {
   .read_vram            = sw_read_vram,
  .fill_vram            = sw_fill_vram,
  .update_vram          = sw_update_vram,
  .copy_vram            = sw_copy_vram,
  .draw_polygon         = sw_draw_polygon,
  .draw_rectangle       = sw_draw_rectangle,
  .draw_line            = sw_draw_line,
  .draw_precise_polygon = sw_draw_precise_polygon,
  .draw_precise_line    = sw_draw_precise_line,
  .drawing_area_changed = sw_drawing_area_changed,
  .update_clut          = sw_update_clut,
  .clear_cache          = sw_clear_cache,
  .clear_vram           = sw_clear_vram,
  .update_display       = sw_update_display,
  .flush_render         = sw_flush_render,
  .do_state             = sw_do_state,
  .destroy              = sw_destroy, 
};

gpu_backend_t* gpu_sw_create(void)
{
  gpu_sw_t* sw = (gpu_sw_t*)calloc(1, sizeof(gpu_sw_t));
  if (!sw) return NULL;
  sw->base.vtable = &gpu_sw_vtable;
  gpu_sw_rasterizer_init();
  memset(g_vram, 0, sizeof(g_vram));
  memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
  g_gpu_backend = &sw->base;
  return &sw->base;
}

const u32* gpu_sw_get_display_buffer(u32* out_width, u32* out_height)
{
  if (!g_gpu_backend) {
    if (out_width)  *out_width  = 0;
    if (out_height) *out_height = 0;
    return NULL;
  }
  gpu_sw_t* sw = (gpu_sw_t*)g_gpu_backend;
  if (out_width)  *out_width  = sw->display_width;
  if (out_height) *out_height = sw->display_height;
  return sw->display_buffer;
}
