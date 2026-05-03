/*
 * The C++ class GPU collapses into a file-static gpu_state_t struct here.
 * The CRTC / command tick events route through timing_event_t.  The FIFO is
 * a fixed-size (4096) circular buffer of u64 entries (the upper 32 bits are
 * an MADDR shadow used by PGXP; we keep them around for layout parity but
 * the lower 32 bits are what every command actually pops).
 *
 * Dropped vs C++:
 *   - PGXP-precise polygon/line paths (fixed-point sub-pixel)
 *   - GPU dump recorder/player
 *   - Hardware-renderer aware codepaths (IsUsingHardwareBackend, sync etc.)
 *   - imgui debug window, screenshot capture, media capture
 *   - Aspect-ratio / draw-rect calc (frontend will redo it)
 *   - System::* feedback (resolution autoscale etc.)
 *
 * All the actual per-command implementations are #included from
 * gpu_commands.c so we keep one TU per source module.
 */

#include "core/gpu.h"
#include "core/fmv_dump.h"
#include "core/gpu_backend.h"
#include "core/gpu_helpers.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_types.h"
#include "core/cpu_pgxp.h"
#include "core/settings.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "core/timing_event.h"
#include "common/assert.h"
#include "common/log.h"
#include "util/state_wrapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

LOG_CHANNEL(GPU);

/* System hooks: we call these so System layer can react to vblank etc.
 * Implementations may be no-ops in early bringup. */
extern u32          system_get_frame_number(void) __attribute__((weak));
extern void         system_increment_frame_number(void) __attribute__((weak));
extern void         system_increment_internal_frame_number(void) __attribute__((weak));
extern void         system_frame_done(void) __attribute__((weak));
extern bool         system_is_pal_region(void) __attribute__((weak));
extern tick_count_t system_get_ticks_per_second(void) __attribute__((weak));
extern tick_count_t system_scale_ticks_to_overclock(tick_count_t t) __attribute__((weak));

/* Timer module hooks (sync/gate updates): no-ops if absent. */
extern bool timers_is_sync_enabled       (u32 timer_index) __attribute__((weak));
extern bool timers_is_external_irq_enabled(u32 timer_index) __attribute__((weak));
extern bool timers_is_using_external_clock(u32 timer_index) __attribute__((weak));
extern tick_count_t timers_get_ticks_until_irq(u32 timer_index) __attribute__((weak));
extern void timers_set_gate (u32 timer_index, bool gate) __attribute__((weak));
extern void timers_add_ticks(u32 timer_index, tick_count_t ticks) __attribute__((weak));

/* Forward declarations for the GP0 ringbuffer/trace defined in
 * gpu_commands.c (which is #included at the bottom of this TU).  Both
 * gpu_write_register and gpu_dma_write tap into the logger before
 * gpu_commands.c is parsed. */
typedef enum { GPU_DBG_GP0_SRC_MMIO = 0, GPU_DBG_GP0_SRC_DMA = 1 } gpu_dbg_gp0_src_t;
static void gpu_dbg_log_gp0(u32 word, gpu_dbg_gp0_src_t source);

/* CUPID_TRACE_DISP=1 enables display/swap/vblank diagnostic trace.  Cached so
 * the per-frame hot paths only do an env lookup on the first call. */
static int gpu_trace_disp_enabled(void)
{
  static int cached = -1;
  if (cached < 0) cached = (getenv("CUPID_TRACE_DISP") != NULL) ? 1 : 0;
  return cached;
}

typedef enum {
  GPU_BLITTER_IDLE             = 0,
  GPU_BLITTER_READING_VRAM     = 1,
  GPU_BLITTER_WRITING_VRAM     = 2,
  GPU_BLITTER_DRAWING_POLYLINE = 3,
} gpu_blitter_state_t;

typedef struct {
  /* Decoded values */
  gpu_draw_mode_reg_t       mode_reg;
  gpu_texture_palette_reg_t palette_reg;
  u32                       texture_window_value;
  gpu_texture_window_t      texture_window;
  bool                      texture_x_flip;
  bool                      texture_y_flip;
} gpu_draw_mode_state_t;

typedef struct {
  /* Registers (fed by GP1 commands). */
  u32 display_address_start;     /* 21 bits, X = bits 0..9, Y = bits 10..18 */
  u32 horizontal_display_range;  /* X1 = bits 0..11, X2 = bits 12..23 */
  u32 vertical_display_range;    /* Y1 = bits 0..9,  Y2 = bits 10..19 */

  u16 dot_clock_divider;

  u16 display_width, display_height;
  u16 display_origin_left, display_origin_top;
  u16 display_vram_left, display_vram_top;
  u16 display_vram_width, display_vram_height;

  u16 horizontal_visible_start, horizontal_visible_end;
  u16 vertical_visible_start,   vertical_visible_end;
  u16 horizontal_display_start, horizontal_display_end;
  u16 vertical_display_start,   vertical_display_end;
  u16 horizontal_active_start,  horizontal_active_end;
  u16 horizontal_total, vertical_total;

  u16          current_scanline;
  tick_count_t fractional_ticks;
  tick_count_t current_tick_in_scanline;
  tick_count_t fractional_dot_ticks; /* only used when timer0 ext-clocked */

  bool in_hblank;
  bool in_vblank;
  u8   interlaced_field;
  u8   interlaced_display_field;
  u8   active_line_lsb;
} gpu_crtc_state_t;

typedef struct {
  u16 x, y, width, height, col, row;
} gpu_vram_transfer_t;

/* FIFO entry: low 32 bits are the command word, high 32 bits unused
 * (PGXP MADDR shadow in C++).  We store both halves to mirror layout. */
typedef struct {
  u64 buf[GPU_MAX_FIFO_SIZE];
  u32 head, tail, size;
} gpu_fifo_t;

/* Blit (CPU->VRAM) word buffer: max size is 0x400 * 0x200 / 2 = 1M / 2 = 512K words. */
#define GPU_MAX_BLIT_WORDS  (1024u * 512u / 2u)
typedef struct {
  u32* data;
  u32  capacity;
  u32  size;
} gpu_blit_buf_t;

/* Polyline buffer: terminator-driven, can grow large.  Cap at FIFO size. */
#define GPU_MAX_POLYLINE_WORDS 4096u
typedef struct {
  u64  data[GPU_MAX_POLYLINE_WORDS];
  u32  size;
} gpu_polyline_buf_t;

typedef struct {
  gpustat_t              GPUSTAT;
  bool                   console_is_pal;
  bool                   set_texture_disable_mask;
  bool                   drawing_area_changed;
  bool                   force_progressive_scan;

  gpu_draw_mode_state_t  draw_mode;
  gpu_drawing_area_t     drawing_area;
  gpu_drawing_offset_t   drawing_offset;

  gpu_crtc_state_t       crtc_state;

  u32                    command_total_words;
  tick_count_t           pending_command_ticks;
  u32                    active_ticks_since_last_update;
  bool                   executing_commands;
  gpu_blitter_state_t    blitter_state;

  gpu_vram_transfer_t    vram_transfer;
  u8                     last_gpu_busy_pct;

  bool                   current_clut_is_8bit;
  u32                    current_clut_reg_bits;

  u32                    GPUREAD_latch;

  gpu_fifo_t             fifo;
  u32                    fifo_size;
  tick_count_t           max_run_ahead;
  u32                    blit_remaining_words;
  gpu_render_command_t   render_command;
  gpu_blit_buf_t         blit_buffer;
  gpu_polyline_buf_t     polyline_buffer;

  /* Timing events. */
  timing_event_t         crtc_tick_event;
  timing_event_t         command_tick_event;
  timing_event_t         frame_done_event;
  bool                   events_initialized;
} gpu_state_t;

static gpu_state_t g;

ALWAYS_INLINE static u32 fifo_size(const gpu_fifo_t* f) { return f->size; }
ALWAYS_INLINE static bool fifo_empty(const gpu_fifo_t* f) { return f->size == 0; }
ALWAYS_INLINE static u32 fifo_capacity(void) { return GPU_MAX_FIFO_SIZE; }

static void fifo_clear(gpu_fifo_t* f) { f->head = f->tail = f->size = 0; }

static void fifo_push(gpu_fifo_t* f, u64 v)
{
  if (f->size >= GPU_MAX_FIFO_SIZE) return;
  f->buf[f->tail] = v;
  f->tail = (f->tail + 1) % GPU_MAX_FIFO_SIZE;
  f->size++;
}
static u64 fifo_pop_u64(gpu_fifo_t* f)
{
  if (f->size == 0) return 0;
  const u64 v = f->buf[f->head];
  f->head = (f->head + 1) % GPU_MAX_FIFO_SIZE;
  f->size--;
  return v;
}
ALWAYS_INLINE static u32 fifo_pop(gpu_fifo_t* f) { return (u32)fifo_pop_u64(f); }
ALWAYS_INLINE static u32 fifo_peek(const gpu_fifo_t* f, u32 i)
{
  return (u32)f->buf[(f->head + i) % GPU_MAX_FIFO_SIZE];
}
static void fifo_remove_one(gpu_fifo_t* f) { (void)fifo_pop_u64(f); }

static void gpu_soft_reset(void);
static void gpu_set_draw_mode    (u16 bits);
static void gpu_set_texture_palette(u16 bits);
static void gpu_set_texture_window (u32 value);
static void gpu_set_clamped_drawing_area(void);
static void gpu_invalidate_clut(void);
static bool gpu_is_clut_valid(void);
static void gpu_update_clut_if_needed(gpu_texture_mode_t mode, gpu_texture_palette_reg_t clut);
static void gpu_update_dma_request(void);
static void gpu_update_gpu_idle(void);
static void gpu_update_crtc_config(void);
static void gpu_update_crtc_display_parameters(void);
static void gpu_update_crtc_tick_event(void);
static void gpu_update_command_tick_event(void);
static void gpu_add_command_ticks(tick_count_t ticks);
static u32  gpu_read_gpuread(void);
static void gpu_finish_vram_write(void);
static void gpu_finish_polyline(void);
static void gpu_write_gp1(u32 value);
static void gpu_handle_get_gpu_info_command(u32 value);
static void gpu_execute_commands(void);
static void gpu_try_execute_commands(void);
static void gpu_end_command(void);
static void gpu_read_vram (u16 x, u16 y, u16 w, u16 h);
static void gpu_update_vram(u16 x, u16 y, u16 w, u16 h, const void* data, bool set_mask, bool check_mask);
static void gpu_prepare_for_draw(void);
static void gpu_fill_draw_command(gpu_backend_draw_cmd_t* cmd, gpu_render_command_t rc);

ALWAYS_INLINE static bool gpustat_skip_drawing_to_active_field_local(void)
{
  return gpustat_skip_drawing_to_active_field(g.GPUSTAT);
}

ALWAYS_INLINE static bool gpu_is_interlaced_rendering_enabled(void)
{
  return !g.force_progressive_scan && gpustat_skip_drawing_to_active_field_local();
}

ALWAYS_INLINE static bool gpu_is_interlaced_display_enabled(void)
{
  return !g.force_progressive_scan && gpustat_vertical_interlace(g.GPUSTAT);
}

ALWAYS_INLINE static bool gpu_is_display_disabled(void)
{
  return gpustat_display_disable(g.GPUSTAT) ||
         g.crtc_state.display_vram_width == 0 ||
         g.crtc_state.display_vram_height == 0;
}

/* GPU runs at 2x the system clock. */
ALWAYS_INLINE static tick_count_t gpu_ticks_to_system_ticks(tick_count_t gpu_ticks)
{
  const tick_count_t v = (gpu_ticks + 1) >> 1;
  return v < 1 ? 1 : v;
}
ALWAYS_INLINE static tick_count_t system_ticks_to_gpu_ticks(tick_count_t sys_ticks)
{
  return sys_ticks << 1;
}

/* Bit-field accessors for CRTC reg packed words. */
ALWAYS_INLINE static u16 crtc_reg_X (u32 v) { return (u16)(v & 0x3FFu); }
ALWAYS_INLINE static u16 crtc_reg_Y (u32 v) { return (u16)((v >> 10) & 0x1FFu); }
ALWAYS_INLINE static u16 crtc_reg_X1(u32 v) { return (u16)(v & 0xFFFu); }
ALWAYS_INLINE static u16 crtc_reg_X2(u32 v) { return (u16)((v >> 12) & 0xFFFu); }
ALWAYS_INLINE static u16 crtc_reg_Y1(u32 v) { return (u16)(v & 0x3FFu); }
ALWAYS_INLINE static u16 crtc_reg_Y2(u32 v) { return (u16)((v >> 10) & 0x3FFu); }

#define DISPLAY_ADDRESS_START_MASK     0x07FFFFEu
#define HORIZONTAL_DISPLAY_RANGE_MASK  0xFFFFFFu
#define VERTICAL_DISPLAY_RANGE_MASK    0xFFFFFu

static void crtc_tick_trampoline(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks_late;
  gpu_crtc_tick_event(ticks);
}
static void command_tick_trampoline(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks_late;
  gpu_command_tick_event(ticks);
}
static void frame_done_trampoline(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks_late;
  gpu_frame_done_event(ticks);
}

static void gpu_alloc_blit_buf(void)
{
  if (!g.blit_buffer.data) {
    g.blit_buffer.capacity = 1u << 16; /* 64K words to start, grows on demand */
    g.blit_buffer.data = (u32*)malloc(g.blit_buffer.capacity * sizeof(u32));
  }
  g.blit_buffer.size = 0;
}
static void gpu_grow_blit_buf(u32 needed)
{
  if (needed <= g.blit_buffer.capacity) return;
  u32 new_cap = g.blit_buffer.capacity ? g.blit_buffer.capacity : (1u << 14);
  while (new_cap < needed) new_cap *= 2;
  if (new_cap > GPU_MAX_BLIT_WORDS) new_cap = GPU_MAX_BLIT_WORDS;
  g.blit_buffer.data = (u32*)realloc(g.blit_buffer.data, new_cap * sizeof(u32));
  g.blit_buffer.capacity = new_cap;
}
static void gpu_blit_push(u32 v)
{
  if (g.blit_buffer.size >= g.blit_buffer.capacity)
    gpu_grow_blit_buf(g.blit_buffer.size + 1);
  if (g.blit_buffer.size < g.blit_buffer.capacity)
    g.blit_buffer.data[g.blit_buffer.size++] = v;
}

void gpu_initialize(void)
{
  if (!g.events_initialized) {
    static const char k_crtc[]    = "GPU CRTC Tick";
    static const char k_command[] = "GPU Command Tick";
    static const char k_frame[]   = "Frame Done";
    timing_event_init(&g.crtc_tick_event,    k_crtc,    sizeof(k_crtc) - 1,    1, 1, crtc_tick_trampoline,    NULL);
    timing_event_init(&g.command_tick_event, k_command, sizeof(k_command) - 1, 1, 1, command_tick_trampoline, NULL);
    timing_event_init(&g.frame_done_event,   k_frame,   sizeof(k_frame) - 1,   1, 1, frame_done_trampoline,   NULL);
    g.events_initialized = true;
  }

  timing_event_activate(&g.crtc_tick_event);
  g.fifo_size = 128;
  g.max_run_ahead = 128;
  g.console_is_pal = system_is_pal_region ? system_is_pal_region() : false;
  /* Mirror duckstation gpu.cpp:84: DeinterlacingMode=Progressive forces 240p
   * scanout (height_shift=y_shift instead of vert_int) so 480i games render
   * as half-height progressive.  Without this the user's INI knob is a no-op
   * and Crash Bandicoot's BIOS logo + intro display as 480i with stale-field
   * weave / flicker artifacts. */
  g.force_progressive_scan =
    (g_settings.display_deinterlacing_mode == DISPLAY_DEINTERLACING_MODE_PROGRESSIVE);
  gpu_alloc_blit_buf();
  gpu_update_crtc_config();
}

void gpu_shutdown(void)
{
  if (g.events_initialized) {
    timing_event_deactivate(&g.command_tick_event);
    timing_event_deactivate(&g.crtc_tick_event);
    timing_event_deactivate(&g.frame_done_event);
  }
  free(g.blit_buffer.data);
  g.blit_buffer.data = NULL;
  g.blit_buffer.capacity = 0;
  g.blit_buffer.size = 0;
}

void gpu_reset(bool clear_vram)
{
  g.GPUSTAT.bits = 0x14802000u;
  g.set_texture_disable_mask = false;
  g.GPUREAD_latch = 0;
  memset(&g.crtc_state, 0, sizeof(g.crtc_state));

  g.blitter_state = GPU_BLITTER_IDLE;
  g.active_ticks_since_last_update = 0;

  if (g.events_initialized) {
    timing_event_deactivate(&g.crtc_tick_event);
    timing_event_deactivate(&g.command_tick_event);
  }

  gpu_soft_reset();

  if (clear_vram)
    gpu_backend_clear_vram();
}

static void gpu_soft_reset(void)
{
  if (g.blitter_state == GPU_BLITTER_WRITING_VRAM)
    gpu_finish_vram_write();

  /* Reset GPUSTAT decoded fields back to power-on defaults. */
  u32 s = g.GPUSTAT.bits;
  s &= ~0x3FFu;                                    /* tex page x/y, transparency, tex mode, dither */
  s &= ~((1u << 9) | (1u << 10) | (1u << 11) | (1u << 12));
  s &= ~(1u << 14); /* reverse_flag */
  s &= ~(1u << 15); /* texture_disable */
  s &= ~((1u << 16) | (1u << 17) | (1u << 18));    /* hres */
  s &= ~(1u << 19); /* vres */
  s &= ~(1u << 21); /* 24bit */
  s &= ~(1u << 22); /* vinterlace */
  s |=  (1u << 23); /* display_disable = true */
  s &= ~(3u << 29); /* dma direction = OFF */
  if (system_is_pal_region && system_is_pal_region())
    s |= (1u << 20);
  else
    s &= ~(1u << 20);
  g.GPUSTAT.bits = s;

  memset(&g.drawing_area, 0, sizeof(g.drawing_area));
  g.drawing_area_changed = true;
  memset(&g.drawing_offset, 0, sizeof(g.drawing_offset));

  g.crtc_state.horizontal_display_range = 0xC60260u;
  g.crtc_state.vertical_display_range   = 0x3FC10u;

  g.blitter_state = GPU_BLITTER_IDLE;
  g.pending_command_ticks = 0;
  g.command_total_words = 0;
  memset(&g.vram_transfer, 0, sizeof(g.vram_transfer));
  fifo_clear(&g.fifo);
  g.blit_buffer.size = 0;
  g.blit_remaining_words = 0;
  g.draw_mode.texture_window_value = 0xFFFFFFFFu;
  gpu_set_draw_mode(0);
  gpu_set_texture_palette(0);
  gpu_set_texture_window(0);
  gpu_invalidate_clut();
  gpu_update_dma_request();
  gpu_update_crtc_config();
  gpu_update_command_tick_event();
  gpu_update_gpu_idle();
}

void gpu_cpu_clock_changed(void) { gpu_update_crtc_config(); }

bool gpu_do_state(state_wrapper_t* sw)
{
  if (state_wrapper_is_writing(sw)) {
    gpu_read_vram(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  }

  state_wrapper_do_u32(sw, &g.GPUSTAT.bits);

  state_wrapper_do_u16(sw, &g.draw_mode.mode_reg.bits);
  state_wrapper_do_u16(sw, &g.draw_mode.palette_reg.bits);
  state_wrapper_do_u32(sw, &g.draw_mode.texture_window_value);

  state_wrapper_do_u8(sw, &g.draw_mode.texture_window.and_x);
  state_wrapper_do_u8(sw, &g.draw_mode.texture_window.and_y);
  state_wrapper_do_u8(sw, &g.draw_mode.texture_window.or_x);
  state_wrapper_do_u8(sw, &g.draw_mode.texture_window.or_y);
  state_wrapper_do_bool(sw, &g.draw_mode.texture_x_flip);
  state_wrapper_do_bool(sw, &g.draw_mode.texture_y_flip);

  state_wrapper_do_u32(sw, &g.drawing_area.left);
  state_wrapper_do_u32(sw, &g.drawing_area.top);
  state_wrapper_do_u32(sw, &g.drawing_area.right);
  state_wrapper_do_u32(sw, &g.drawing_area.bottom);
  state_wrapper_do_s32(sw, &g.drawing_offset.x);
  state_wrapper_do_s32(sw, &g.drawing_offset.y);

  state_wrapper_do_bool(sw, &g.console_is_pal);
  state_wrapper_do_bool(sw, &g.set_texture_disable_mask);

  /* CRTC state as a blob; safe because both ends use identical layout. */
  state_wrapper_do_bytes(sw, &g.crtc_state, sizeof(g.crtc_state));

  u8 blitter = (u8)g.blitter_state;
  state_wrapper_do_u8(sw, &blitter);
  g.blitter_state = (gpu_blitter_state_t)blitter;
  state_wrapper_do_s32(sw, &g.pending_command_ticks);
  state_wrapper_do_u32(sw, &g.command_total_words);
  state_wrapper_do_u32(sw, &g.GPUREAD_latch);

  state_wrapper_do_u32(sw, &g.current_clut_reg_bits);
  state_wrapper_do_bool(sw, &g.current_clut_is_8bit);
  state_wrapper_do_bytes(sw, g_gpu_clut, sizeof(g_gpu_clut));

  state_wrapper_do_bytes(sw, &g.vram_transfer, sizeof(g.vram_transfer));

  /* FIFO: u32 count + raw bytes. */
  u32 fifo_count = g.fifo.size;
  state_wrapper_do_u32(sw, &fifo_count);
  if (state_wrapper_is_reading(sw)) {
    fifo_clear(&g.fifo);
    if (fifo_count > GPU_MAX_FIFO_SIZE) fifo_count = GPU_MAX_FIFO_SIZE;
    for (u32 i = 0; i < fifo_count; i++) {
      u64 v = 0;
      state_wrapper_do_u64(sw, &v);
      fifo_push(&g.fifo, v);
    }
  } else {
    for (u32 i = 0; i < fifo_count; i++) {
      u64 v = g.fifo.buf[(g.fifo.head + i) % GPU_MAX_FIFO_SIZE];
      state_wrapper_do_u64(sw, &v);
    }
  }

  /* Blit buffer + remaining. */
  u32 blit_size = g.blit_buffer.size;
  state_wrapper_do_u32(sw, &blit_size);
  if (state_wrapper_is_reading(sw)) {
    gpu_grow_blit_buf(blit_size);
    g.blit_buffer.size = blit_size;
  }
  state_wrapper_do_array(sw, g.blit_buffer.data, sizeof(u32), blit_size);
  state_wrapper_do_u32(sw, &g.blit_remaining_words);
  state_wrapper_do_u32(sw, &g.render_command.bits);

  /* VRAM is a flat blob. */
  state_wrapper_do_bytes(sw, g_vram, sizeof(g_vram));

  if (state_wrapper_is_reading(sw)) {
    g.drawing_area_changed = true;
    gpu_set_clamped_drawing_area();
    gpu_update_dma_request();
    gpu_update_crtc_config();
    gpu_update_command_tick_event();
  }

  return !state_wrapper_has_error(sw);
}

static void gpu_update_dma_request(void)
{
  switch (g.blitter_state) {
    case GPU_BLITTER_IDLE:
      g.GPUSTAT.bits &= ~((1u << 27) | (1u << 28));
      if (fifo_empty(&g.fifo) || fifo_size(&g.fifo) < g.command_total_words)
        g.GPUSTAT.bits |= (1u << 28);
      break;
    case GPU_BLITTER_WRITING_VRAM:
      g.GPUSTAT.bits &= ~((1u << 27) | (1u << 28));
      if (fifo_size(&g.fifo) < g.fifo_size)
        g.GPUSTAT.bits |= (1u << 28);
      break;
    case GPU_BLITTER_READING_VRAM:
      g.GPUSTAT.bits &= ~((1u << 27) | (1u << 28));
      g.GPUSTAT.bits |= (1u << 27);
      break;
    case GPU_BLITTER_DRAWING_POLYLINE:
      g.GPUSTAT.bits &= ~((1u << 27) | (1u << 28));
      if (fifo_size(&g.fifo) < g.fifo_size)
        g.GPUSTAT.bits |= (1u << 28);
      break;
  }

  bool dma_request = false;
  switch (gpustat_dma_direction(g.GPUSTAT)) {
    case GPU_DMA_DIRECTION_OFF:           dma_request = false; break;
    case GPU_DMA_DIRECTION_FIFO:          dma_request = gpustat_ready_to_receive_dma(g.GPUSTAT); break;
    case GPU_DMA_DIRECTION_CPU_TO_GP0:    dma_request = gpustat_ready_to_receive_dma(g.GPUSTAT); break;
    case GPU_DMA_DIRECTION_GPUREAD_TO_CPU:dma_request = gpustat_ready_to_send_vram(g.GPUSTAT); break;
  }
  if (dma_request) g.GPUSTAT.bits |= (1u << 25); else g.GPUSTAT.bits &= ~(1u << 25);
  dma_set_request(DMA_CHANNEL_GPU, dma_request);
}

static void gpu_update_gpu_idle(void)
{
  if (g.blitter_state == GPU_BLITTER_IDLE && g.pending_command_ticks <= 0 && fifo_empty(&g.fifo))
    g.GPUSTAT.bits |= (1u << 26);
  else
    g.GPUSTAT.bits &= ~(1u << 26);
}

u32 gpu_read_register(u32 offset)
{
  switch (offset) {
    case 0x00:
      return gpu_read_gpuread();
    case 0x04:
      if (gpu_is_crtc_scanline_pending())
        gpu_synchronize_crtc();
      if (gpu_is_command_completion_pending())
        timing_event_invoke_early(&g.command_tick_event, false);
      return g.GPUSTAT.bits;
    default:
      ERROR_LOG("Unhandled GPU register read: %02X", offset);
      return 0xFFFFFFFFu;
  }
}

void gpu_write_register(u32 offset, u32 value)
{
  switch (offset) {
    case 0x00:
      /* GP0 write: push into FIFO. */
      gpu_dbg_log_gp0(value, GPU_DBG_GP0_SRC_MMIO);
      if (fifo_size(&g.fifo) >= g.fifo_size) {
        timing_event_invoke_early(&g.command_tick_event, false);
        if (fifo_size(&g.fifo) >= fifo_capacity()) {
          WARNING_LOG("GPU FIFO overflow via GP0 write, size=%u", fifo_size(&g.fifo));
          return;
        }
      }
      fifo_push(&g.fifo, value);
      gpu_execute_commands();
      return;
    case 0x04:
      gpu_write_gp1(value);
      return;
    default:
      ERROR_LOG("Unhandled GPU register write: %02X <- %08X", offset, value);
      return;
  }
}

void gpu_dma_read(u32* words, u32 word_count)
{
  if (gpustat_dma_direction(g.GPUSTAT) != GPU_DMA_DIRECTION_GPUREAD_TO_CPU) {
    ERROR_LOG("Invalid DMA direction from GPU DMA read");
    for (u32 i = 0; i < word_count; i++) words[i] = 0xFFFFFFFFu;
    return;
  }
  for (u32 i = 0; i < word_count; i++) words[i] = gpu_read_gpuread();
}

void gpu_dma_write(u32 address, u32 value)
{
  /* Pack RAM source address into the high 32 bits of the FIFO entry.  PGXP
   * polygon/line submission reads it back as MADDR to look up the precise
   * float vertex.  The non-precise integer path's fifo_pop() truncates to
   * u32 so it sees the value-only low half unchanged. */
  {
    const u32 record[2] = { address, value };
    FMV_DUMP("T5", record, sizeof(record), 0);
  }
  gpu_dbg_log_gp0(value, GPU_DBG_GP0_SRC_DMA);
  fifo_push(&g.fifo, ((u64)address << 32) | (u64)value);
}
void gpu_end_dma_write(void)
{
  gpu_execute_commands();
}
bool gpu_begin_dma_write(void)
{
  const gpu_dma_direction_t d = gpustat_dma_direction(g.GPUSTAT);
  return d == GPU_DMA_DIRECTION_CPU_TO_GP0 || d == GPU_DMA_DIRECTION_FIFO;
}
/* Backwards-compat aliases retained for any in-tree callers. */
bool gpu_dma_can_write(void)  { return gpu_begin_dma_write(); }
void gpu_dma_end_write(void)  { gpu_end_dma_write(); }

tick_count_t gpu_get_crtc_frequency(void)
{
  return g.console_is_pal ? 53203425 : 53693175;
}

static tick_count_t crtc_to_sys_ticks(tick_count_t gpu_ticks, tick_count_t fractional_ticks)
{
  if (!g.console_is_pal)
    return (tick_count_t)(((u64)gpu_ticks * 451584ull + (u64)fractional_ticks + 715908ull) / 715909ull);
  return (tick_count_t)(((u64)gpu_ticks * 451584ull + (u64)fractional_ticks + 709378ull) / 709379ull);
}
static tick_count_t sys_to_crtc_ticks(tick_count_t sys_ticks, tick_count_t* fractional_ticks)
{
  u64 mul = (u64)sys_ticks * (g.console_is_pal ? 709379ull : 715909ull);
  mul += (u64)*fractional_ticks;
  const tick_count_t t = (tick_count_t)(mul / 451584ull);
  *fractional_ticks = (tick_count_t)(mul % 451584ull);
  return t;
}

static void gpu_add_command_ticks(tick_count_t ticks)
{
  g.pending_command_ticks += ticks;
  g.active_ticks_since_last_update += (u32)ticks;
}

void gpu_synchronize_crtc(void)
{
  if (g.events_initialized)
    timing_event_invoke_early(&g.crtc_tick_event, false);
}

bool gpu_is_in_pal_mode(void) { return gpustat_pal_mode(g.GPUSTAT); }
u16  gpu_get_horizontal_total(void) { return g.crtc_state.horizontal_total; }
u16  gpu_get_vertical_total(void)   { return g.crtc_state.vertical_total; }

static tick_count_t gpu_get_pending_crtc_ticks(void)
{
  const tick_count_t pending = timing_event_get_ticks_since_last_execution(&g.crtc_tick_event);
  tick_count_t fractional = g.crtc_state.fractional_ticks;
  return sys_to_crtc_ticks(pending, &fractional);
}
static tick_count_t gpu_get_pending_command_ticks(void)
{
  if (!timing_event_is_active(&g.command_tick_event)) return 0;
  return system_ticks_to_gpu_ticks(timing_event_get_ticks_since_last_execution(&g.command_tick_event));
}

bool gpu_is_crtc_scanline_pending(void)
{
  const tick_count_t ticks = gpu_get_pending_crtc_ticks() + g.crtc_state.current_tick_in_scanline;
  return ticks >= g.crtc_state.horizontal_total;
}
bool gpu_is_command_completion_pending(void)
{
  return g.pending_command_ticks > 0 && gpu_get_pending_command_ticks() >= g.pending_command_ticks;
}

static void update_hblank_flag(void)
{
  g.crtc_state.in_hblank =
    (g.crtc_state.current_tick_in_scanline < g.crtc_state.horizontal_active_start || 
     g.crtc_state.current_tick_in_scanline >= g.crtc_state.horizontal_active_end);
}

static u16 u16_min(u16 a, u16 b) { return a < b ? a : b; }
static u16 u16_max(u16 a, u16 b) { return a > b ? a : b; }
static u16 u16_clamp(u16 v, u16 lo, u16 hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void gpu_update_crtc_config(void)
{
  /* Dot-clock divider for each of 8 horizontal-resolution combos.
   * The last 4 entries collapse to 7 because hres_2 forces 7 (368 dots).*/
  static const u16 dot_clock_dividers[8] = { 10, 8, 5, 4, 7, 7, 7, 7 };
  gpu_crtc_state_t* cs = &g.crtc_state;

  cs->vertical_total          = gpustat_pal_mode(g.GPUSTAT) ? GPU_PAL_TOTAL_LINES   : GPU_NTSC_TOTAL_LINES;
  cs->horizontal_total        = gpustat_pal_mode(g.GPUSTAT) ? GPU_PAL_TICKS_PER_LINE: GPU_NTSC_TICKS_PER_LINE;
  cs->horizontal_active_start = gpustat_pal_mode(g.GPUSTAT) ? GPU_PAL_HORIZONTAL_ACTIVE_START : GPU_NTSC_HORIZONTAL_ACTIVE_START;
  cs->horizontal_active_end   = gpustat_pal_mode(g.GPUSTAT) ? GPU_PAL_HORIZONTAL_ACTIVE_END   : GPU_NTSC_HORIZONTAL_ACTIVE_END;

  const u8 hres_index = (u8)(gpustat_horizontal_resolution_1(g.GPUSTAT) |
                             (gpustat_horizontal_resolution_2(g.GPUSTAT) << 2));
  cs->dot_clock_divider = dot_clock_dividers[hres_index];

  const u16 X1 = crtc_reg_X1(cs->horizontal_display_range);
  const u16 X2 = crtc_reg_X2(cs->horizontal_display_range);
  cs->horizontal_display_start = (u16)((u16_min(X1, cs->horizontal_total) / cs->dot_clock_divider) * cs->dot_clock_divider);
  cs->horizontal_display_end   = (u16)((u16_min(X2, cs->horizontal_total) / cs->dot_clock_divider) * cs->dot_clock_divider);
  cs->vertical_display_start   = u16_min(crtc_reg_Y1(cs->vertical_display_range), cs->vertical_total);
  cs->vertical_display_end     = u16_min(crtc_reg_Y2(cs->vertical_display_range), cs->vertical_total);

  if (gpustat_pal_mode(g.GPUSTAT) && g_settings.gpu_force_video_timing == FORCE_VIDEO_TIMING_MODE_NTSC) {
    cs->horizontal_display_start =
      (u16)(((u32)cs->horizontal_display_start * GPU_NTSC_TICKS_PER_LINE) / GPU_PAL_TICKS_PER_LINE);
     cs->horizontal_display_end =
      (u16)(((u32)cs->horizontal_display_end * GPU_NTSC_TICKS_PER_LINE + (GPU_PAL_TICKS_PER_LINE - 1)) / 
            GPU_PAL_TICKS_PER_LINE);
    cs->vertical_display_start =
      (u16)(((u32)cs->vertical_display_start * GPU_NTSC_TOTAL_LINES) / GPU_PAL_TOTAL_LINES);
     cs->vertical_display_end =
      (u16)(((u32)cs->vertical_display_end * GPU_NTSC_TOTAL_LINES + (GPU_PAL_TOTAL_LINES - 1)) / 
            GPU_PAL_TOTAL_LINES);

    cs->vertical_total = GPU_NTSC_TOTAL_LINES;
    cs->current_scanline %= GPU_NTSC_TOTAL_LINES;
    cs->horizontal_total = GPU_NTSC_TICKS_PER_LINE;
    cs->current_tick_in_scanline %= GPU_NTSC_TICKS_PER_LINE;
  } else if (!gpustat_pal_mode(g.GPUSTAT) && g_settings.gpu_force_video_timing == FORCE_VIDEO_TIMING_MODE_PAL) {
    cs->horizontal_display_start =
      (u16)(((u32)cs->horizontal_display_start * GPU_PAL_TICKS_PER_LINE) / GPU_NTSC_TICKS_PER_LINE);
     cs->horizontal_display_end =
      (u16)(((u32)cs->horizontal_display_end * GPU_PAL_TICKS_PER_LINE + (GPU_NTSC_TICKS_PER_LINE - 1)) / 
            GPU_NTSC_TICKS_PER_LINE);
    cs->vertical_display_start =
      (u16)(((u32)cs->vertical_display_start * GPU_PAL_TOTAL_LINES) / GPU_NTSC_TOTAL_LINES);
     cs->vertical_display_end =
      (u16)(((u32)cs->vertical_display_end * GPU_PAL_TOTAL_LINES + (GPU_NTSC_TOTAL_LINES - 1)) / 
            GPU_NTSC_TOTAL_LINES);

    cs->vertical_total = GPU_PAL_TOTAL_LINES;
    cs->current_scanline %= GPU_PAL_TOTAL_LINES;
    cs->horizontal_total = GPU_PAL_TICKS_PER_LINE;
    cs->current_tick_in_scanline %= GPU_PAL_TICKS_PER_LINE;
  }

  if (system_scale_ticks_to_overclock) {
    cs->horizontal_display_start = (u16)system_scale_ticks_to_overclock(cs->horizontal_display_start);
    cs->horizontal_display_end   = (u16)system_scale_ticks_to_overclock(cs->horizontal_display_end);
    cs->horizontal_active_start  = (u16)system_scale_ticks_to_overclock(cs->horizontal_active_start);
    cs->horizontal_active_end    = (u16)system_scale_ticks_to_overclock(cs->horizontal_active_end);
    cs->horizontal_total         = (u16)system_scale_ticks_to_overclock(cs->horizontal_total);
  }

  cs->current_tick_in_scanline %= cs->horizontal_total;
  update_hblank_flag();
  cs->current_scanline %= cs->vertical_total;

  gpu_update_crtc_display_parameters();
  gpu_update_crtc_tick_event();
}

static void gpu_update_crtc_display_parameters(void)
{
  gpu_crtc_state_t* cs = &g.crtc_state;
  const bool pal = gpustat_pal_mode(g.GPUSTAT);

  /* Visible range = full active region (no overscan/borders modes). */
  cs->horizontal_visible_start = pal ? GPU_PAL_HORIZONTAL_ACTIVE_START : GPU_NTSC_HORIZONTAL_ACTIVE_START;
  cs->horizontal_visible_end   = pal ? GPU_PAL_HORIZONTAL_ACTIVE_END   : GPU_NTSC_HORIZONTAL_ACTIVE_END;
  cs->vertical_visible_start   = pal ? GPU_PAL_VERTICAL_ACTIVE_START   : GPU_NTSC_VERTICAL_ACTIVE_START;
  cs->vertical_visible_end     = pal ? GPU_PAL_VERTICAL_ACTIVE_END     : GPU_NTSC_VERTICAL_ACTIVE_END;

  cs->horizontal_visible_start = u16_clamp(cs->horizontal_visible_start, cs->horizontal_active_start, cs->horizontal_active_end);
  cs->horizontal_visible_end   = u16_clamp(cs->horizontal_visible_end,   cs->horizontal_visible_start, cs->horizontal_active_end);
  cs->vertical_visible_start   = u16_clamp(cs->vertical_visible_start, pal ? GPU_PAL_VERTICAL_ACTIVE_START : GPU_NTSC_VERTICAL_ACTIVE_START,
                                                                       pal ? GPU_PAL_VERTICAL_ACTIVE_END   : GPU_NTSC_VERTICAL_ACTIVE_END);
  cs->vertical_visible_end     = u16_clamp(cs->vertical_visible_end,   cs->vertical_visible_start,
                                                                       pal ? GPU_PAL_VERTICAL_ACTIVE_END   : GPU_NTSC_VERTICAL_ACTIVE_END);

  const u8 vert_int  = gpustat_vertical_interlace(g.GPUSTAT) ? 1u : 0u;
  const u8 vert_res  = gpustat_vertical_resolution(g.GPUSTAT) ? 1u : 0u;
  const u8 y_shift   = (vert_int && vert_res) ? 1u : 0u;
  const u8 height_shift = g.force_progressive_scan ? y_shift : vert_int;

  cs->display_width  = (u16)((cs->horizontal_visible_end - cs->horizontal_visible_start) / cs->dot_clock_divider);
  cs->display_height = (u16)((cs->vertical_visible_end - cs->vertical_visible_start) << height_shift);

  const u16 horizontal_display_ticks = (cs->horizontal_display_end < cs->horizontal_display_start)
                                         ? 0 : (cs->horizontal_display_end - cs->horizontal_display_start);
  const u16 horizontal_display_pixels = horizontal_display_ticks / cs->dot_clock_divider;
  cs->display_vram_width = (horizontal_display_pixels == 1u) ? 4u
                             : (u16)((horizontal_display_pixels + 2u) & ~3u);

  u16 horizontal_skip_pixels;
  if (cs->horizontal_display_start >= cs->horizontal_visible_start) {
    cs->display_origin_left = (u16)((cs->horizontal_display_start - cs->horizontal_visible_start) / cs->dot_clock_divider);
    cs->display_vram_left = crtc_reg_X(cs->display_address_start);
    horizontal_skip_pixels = 0;
  } else {
    horizontal_skip_pixels = (u16)((cs->horizontal_visible_start - cs->horizontal_display_start) / cs->dot_clock_divider);
    cs->display_origin_left = 0;
    cs->display_vram_left = (u16)((crtc_reg_X(cs->display_address_start) + horizontal_skip_pixels) % VRAM_WIDTH);
  }
  cs->display_vram_width -= u16_min(cs->display_vram_width, horizontal_skip_pixels);
  cs->display_vram_width = u16_min(cs->display_vram_width, (u16)(cs->display_width - cs->display_origin_left));

  if (cs->vertical_display_start >= cs->vertical_visible_start) {
    cs->display_origin_top = (u16)((cs->vertical_display_start - cs->vertical_visible_start) << y_shift);
    cs->display_vram_top   = crtc_reg_Y(cs->display_address_start);
  } else {
    cs->display_origin_top = 0;
    cs->display_vram_top   = (u16)((crtc_reg_Y(cs->display_address_start) +
                                    ((cs->vertical_visible_start - cs->vertical_display_start) << y_shift)) % VRAM_HEIGHT);
  }
  if (cs->vertical_display_end <= cs->vertical_visible_end) {
    cs->display_vram_height = (u16)((cs->vertical_display_end -
                                     u16_min(cs->vertical_display_end, u16_max(cs->vertical_display_start, cs->vertical_visible_start)))
                                    << height_shift);
  } else {
    cs->display_vram_height = (u16)((cs->vertical_visible_end -
                                     u16_min(cs->vertical_visible_end, u16_max(cs->vertical_display_start, cs->vertical_visible_start)))
                                    << height_shift);
  }
}

static void gpu_update_crtc_tick_event(void)
{
  gpu_crtc_state_t* cs = &g.crtc_state;
  tick_count_t lines_until_event;
  /* Sync at vblank end (or vblank start if HBLANK timer sync is enabled). */
  const bool hblank_sync = timers_is_sync_enabled ? timers_is_sync_enabled(GPU_HBLANK_TIMER_INDEX) : false;
  if (hblank_sync) {
    lines_until_event = (cs->current_scanline >= cs->vertical_display_end)
      ? (cs->vertical_total - cs->current_scanline + cs->vertical_display_start)
      : (cs->vertical_display_end - cs->current_scanline);
  } else {
    lines_until_event = (cs->current_scanline >= cs->vertical_display_end)
      ? (cs->vertical_total - cs->current_scanline + cs->vertical_display_end)
      : (cs->vertical_display_end - cs->current_scanline);
  }
  if (timers_is_external_irq_enabled && timers_is_external_irq_enabled(GPU_HBLANK_TIMER_INDEX)) {
    const tick_count_t lim = timers_get_ticks_until_irq(GPU_HBLANK_TIMER_INDEX);
    if (lim < lines_until_event) lines_until_event = lim;
  }

  tick_count_t ticks_until_event = lines_until_event * cs->horizontal_total - cs->current_tick_in_scanline;
  if (timers_is_external_irq_enabled && timers_is_external_irq_enabled(GPU_DOT_TIMER_INDEX)) {
    const tick_count_t dots_until = timers_get_ticks_until_irq(GPU_DOT_TIMER_INDEX);
    const tick_count_t ticks_until = (dots_until * cs->dot_clock_divider) - cs->fractional_dot_ticks;
    const tick_count_t v = ticks_until > 0 ? ticks_until : 0;
    if (v < ticks_until_event) ticks_until_event = v;
  }

  if (g.events_initialized)
    timing_event_schedule(&g.crtc_tick_event, crtc_to_sys_ticks(ticks_until_event, cs->fractional_ticks));
}

void gpu_crtc_tick_event(tick_count_t ticks)
{
  gpu_crtc_state_t* cs = &g.crtc_state;
  const tick_count_t prev_tick = cs->current_tick_in_scanline;
  const tick_count_t gpu_ticks = sys_to_crtc_ticks(ticks, &cs->fractional_ticks);
  cs->current_tick_in_scanline += gpu_ticks;

  if (timers_is_using_external_clock && timers_is_using_external_clock(GPU_DOT_TIMER_INDEX)) {
    cs->fractional_dot_ticks += gpu_ticks;
    const tick_count_t dots = cs->fractional_dot_ticks / cs->dot_clock_divider;
    cs->fractional_dot_ticks = cs->fractional_dot_ticks % cs->dot_clock_divider;
    if (dots > 0 && timers_add_ticks)
      timers_add_ticks(GPU_DOT_TIMER_INDEX, dots);
  }

  if (cs->current_tick_in_scanline < cs->horizontal_total) {
    update_hblank_flag();
    if (timers_set_gate) timers_set_gate(GPU_DOT_TIMER_INDEX, cs->in_hblank);
    if (timers_is_using_external_clock && timers_is_using_external_clock(GPU_HBLANK_TIMER_INDEX)) {
      const u32 dt = ((cs->current_tick_in_scanline >= cs->horizontal_active_end) ? 1u : 0u)
                   - ((prev_tick >= cs->horizontal_active_end) ? 1u : 0u);
      if (dt > 0 && timers_add_ticks) timers_add_ticks(GPU_HBLANK_TIMER_INDEX, (tick_count_t)dt);
    }
    gpu_update_crtc_tick_event();
    return;
  }

  u32 lines_to_draw = cs->current_tick_in_scanline / cs->horizontal_total;
  cs->current_tick_in_scanline %= cs->horizontal_total;
  update_hblank_flag();
  if (timers_set_gate) timers_set_gate(GPU_DOT_TIMER_INDEX, cs->in_hblank);

  if (timers_is_using_external_clock && timers_is_using_external_clock(GPU_HBLANK_TIMER_INDEX)) {
    const u32 hblank_dt = lines_to_draw
                          - ((prev_tick >= cs->horizontal_active_end) ? 1u : 0u)
                          + ((cs->current_tick_in_scanline >= cs->horizontal_active_end) ? 1u : 0u);
    if (hblank_dt > 0 && timers_add_ticks) timers_add_ticks(GPU_HBLANK_TIMER_INDEX, (tick_count_t)hblank_dt);
  }

  bool frame_done = false;
  while (lines_to_draw > 0) {
    const u32 lines_this = (lines_to_draw < (u32)(cs->vertical_total - cs->current_scanline))
                              ? lines_to_draw : (u32)(cs->vertical_total - cs->current_scanline);
    const u32 prev_scanline = cs->current_scanline;
    cs->current_scanline = (u16)(cs->current_scanline + lines_this);
    lines_to_draw -= lines_this;

    /* Clear vblank if beam passes through display area. */
    if (prev_scanline < cs->vertical_display_start &&
        cs->current_scanline >= cs->vertical_display_end) {
      if (timers_set_gate) timers_set_gate(GPU_HBLANK_TIMER_INDEX, false);
      interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_VBLANK, false);
      cs->in_vblank = false;
    }

    const bool new_vblank = cs->current_scanline < cs->vertical_display_start ||
                            cs->current_scanline >= cs->vertical_display_end;
    if (cs->in_vblank != new_vblank) {
      if (gpu_trace_disp_enabled()) {
        static u32 vblank_n = 0;
        fprintf(stderr, "[disp vblank #%u] edge=%s scanline=%u vds=%u vde=%u\n",
                vblank_n++, new_vblank ? "ENTER" : "EXIT",
                (unsigned)cs->current_scanline,
                (unsigned)cs->vertical_display_start,
                (unsigned)cs->vertical_display_end);
      }
      if (new_vblank) {
        if (system_increment_frame_number) system_increment_frame_number();
        gpu_update_display(true);
        frame_done = true;
        if (gpustat_in_interleaved_480i_mode(g.GPUSTAT))
          cs->interlaced_display_field = (u8)(cs->interlaced_field ^ 1u);
        else
          cs->interlaced_display_field = 0;
      }
      if (timers_set_gate) timers_set_gate(GPU_HBLANK_TIMER_INDEX, new_vblank);
      interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_VBLANK, new_vblank);
      cs->in_vblank = new_vblank;
    }

    if (cs->current_scanline == cs->vertical_total) {
      cs->current_scanline = 0;
      if (gpustat_vertical_interlace(g.GPUSTAT)) {
        cs->interlaced_field ^= 1u;
        const bool inv = !((bool)cs->interlaced_field);
        if (inv) g.GPUSTAT.bits |= (1u << 13); else g.GPUSTAT.bits &= ~(1u << 13);
      } else {
        cs->interlaced_field = 0;
        g.GPUSTAT.bits &= ~(1u << 13);
      }
    }
  }

  /* Active line LSB: alternates fields for interlaced 480i, else mirrors line bit. */
  if (gpustat_in_interleaved_480i_mode(g.GPUSTAT)) {
    cs->active_line_lsb = (u8)((crtc_reg_Y(cs->display_address_start) + cs->interlaced_display_field) & 1u);
    const bool dl = ((crtc_reg_Y(cs->display_address_start) +
                      ((!cs->in_vblank ? 1u : 0u) & cs->interlaced_display_field)) & 1u) != 0;
    if (dl) g.GPUSTAT.bits |= (1u << 31); else g.GPUSTAT.bits &= ~(1u << 31);
  } else {
    cs->active_line_lsb = 0;
    const bool dl = ((crtc_reg_Y(cs->display_address_start) + cs->current_scanline) & 1u) != 0;
    if (dl) g.GPUSTAT.bits |= (1u << 31); else g.GPUSTAT.bits &= ~(1u << 31);
  }

  gpu_update_crtc_tick_event();

  if (frame_done) {
    if (system_frame_done) system_frame_done();
  }
}

void gpu_command_tick_event(tick_count_t ticks)
{
  g.pending_command_ticks -= system_ticks_to_gpu_ticks(ticks);
  g.executing_commands = true;
  gpu_execute_commands();
  gpu_update_command_tick_event();
  g.executing_commands = false;
}

void gpu_frame_done_event(tick_count_t ticks)
{
  (void)ticks;
  timing_event_deactivate(&g.frame_done_event);
  if (system_frame_done) system_frame_done();
}

static void gpu_update_command_tick_event(void)
{
  if (g.pending_command_ticks <= 0) {
    g.pending_command_ticks = 0;
    timing_event_deactivate(&g.command_tick_event);
  } else {
    timing_event_set_interval_and_schedule(&g.command_tick_event, gpu_ticks_to_system_ticks(g.pending_command_ticks));
  }
}

static u32 gpu_read_gpuread(void)
{
  if (g.blitter_state != GPU_BLITTER_READING_VRAM)
    return g.GPUREAD_latch;

  u32 value = 0;
  for (u32 i = 0; i < 2; i++) {
    const u16 read_x = (u16)((g.vram_transfer.x + g.vram_transfer.col) % VRAM_WIDTH);
    const u16 read_y = (u16)((g.vram_transfer.y + g.vram_transfer.row) % VRAM_HEIGHT);
    value |= ((u32)g_vram[read_y * VRAM_WIDTH + read_x]) << (i * 16);
    if (++g.vram_transfer.col == g.vram_transfer.width) {
      g.vram_transfer.col = 0;
      if (++g.vram_transfer.row == g.vram_transfer.height) {
        memset(&g.vram_transfer, 0, sizeof(g.vram_transfer));
        g.blitter_state = GPU_BLITTER_IDLE;
        gpu_execute_commands();
        break;
      }
    }
  }
  g.GPUREAD_latch = value;
  return value;
}

static void gpu_read_vram(u16 x, u16 y, u16 w, u16 h)
{
  /* SW renderer: VRAM is in g_vram already, nothing to read back. */
  (void)x; (void)y; (void)w; (void)h;
  gpu_backend_read_vram_cmd_t c = { x, y, w, h };
  gpu_backend_read_vram(&c);
}

static void gpu_update_vram(u16 x, u16 y, u16 w, u16 h, const void* data, bool set_mask, bool check_mask)
{
  gpu_backend_update_vram_cmd_t c = { x, y, w, h, set_mask, check_mask };
  gpu_backend_update_vram(&c, (const u16*)data);
}

#define DRAW_MODE_REG_GPUSTAT_MASK   0x7FFu

static void gpu_set_draw_mode(u16 value)
{
  gpu_draw_mode_reg_t new_mode_reg = { (u16)(value & GPU_DRAW_MODE_REG_MASK) };
  if (!g.set_texture_disable_mask)
    new_mode_reg.bits &= ~(1u << 11);

  g.draw_mode.mode_reg.bits = new_mode_reg.bits;
  g.GPUSTAT.bits = (g.GPUSTAT.bits & ~DRAW_MODE_REG_GPUSTAT_MASK) |
                   (new_mode_reg.bits & DRAW_MODE_REG_GPUSTAT_MASK);
  if (gpu_draw_mode_reg_texture_disable(new_mode_reg)) g.GPUSTAT.bits |= (1u << 15);
  else g.GPUSTAT.bits &= ~(1u << 15);
}

static void gpu_set_texture_palette(u16 value)
{
  value &= GPU_TEXTURE_PALETTE_REG_MASK;
  g.draw_mode.palette_reg.bits = value;
}

static void gpu_set_texture_window(u32 value)
{
  value &= 0xFFFFFu;
  if (g.draw_mode.texture_window_value == value) return;

  const u8 mask_x   = (u8)( value        & 0x1Fu);
  const u8 mask_y   = (u8)((value >>  5) & 0x1Fu);
  const u8 offset_x = (u8)((value >> 10) & 0x1Fu);
  const u8 offset_y = (u8)((value >> 15) & 0x1Fu);
  g.draw_mode.texture_window.and_x = (u8)(~(mask_x * 8u));
  g.draw_mode.texture_window.and_y = (u8)(~(mask_y * 8u));
  g.draw_mode.texture_window.or_x  = (u8)((offset_x & mask_x) * 8u);
  g.draw_mode.texture_window.or_y  = (u8)((offset_y & mask_y) * 8u);
  g.draw_mode.texture_window_value = value;
}

static void gpu_set_clamped_drawing_area(void)
{
  /* No hardware clamping needed for SW renderer; the rasterizer reads
   * gpu_sw_drawing_area directly from the set_drawing_area command. */
}

static void gpu_invalidate_clut(void)
{
  g.current_clut_reg_bits = 0xFFFFFFFFu;
  g.current_clut_is_8bit = false;
}
static bool gpu_is_clut_valid(void)
{
  return g.current_clut_reg_bits != 0xFFFFFFFFu;
}

static void gpu_update_clut_if_needed(gpu_texture_mode_t mode, gpu_texture_palette_reg_t clut)
{
  if (mode >= GPU_TEXTURE_MODE_DIRECT_16BIT) return;
  const bool needs_8bit = (mode == GPU_TEXTURE_MODE_PALETTE_8BIT);
  if (clut.bits != g.current_clut_reg_bits ||
      (needs_8bit && !g.current_clut_is_8bit)) {
    gpu_add_command_ticks(needs_8bit ? 256 : 16);
    g.current_clut_reg_bits = clut.bits;
    g.current_clut_is_8bit  = needs_8bit;
    gpu_backend_update_clut_cmd_t c = { clut, needs_8bit };
    gpu_backend_update_clut(&c);
  }
}

static void gpu_prepare_for_draw(void)
{
  if (g.drawing_area_changed) {
    g.drawing_area_changed = false;
    gpu_backend_set_drawing_area_cmd_t c;
    c.new_area = g.drawing_area;
    gpu_backend_set_drawing_area(&c);
  }
}

static void gpu_fill_draw_command(gpu_backend_draw_cmd_t* cmd, gpu_render_command_t rc)
{
  cmd->interlaced_rendering   = gpu_is_interlaced_rendering_enabled();
  cmd->active_line_lsb        = (g.crtc_state.active_line_lsb != 0);
  cmd->check_mask_before_draw = gpustat_check_mask_before_draw(g.GPUSTAT);
  cmd->set_mask_while_drawing = gpustat_set_mask_while_drawing(g.GPUSTAT);
  cmd->texture_enable         = gpu_render_command_is_texturing_enabled(rc);
  cmd->raw_texture_enable     = gpu_render_command_raw_texture_enable(rc);
  cmd->transparency_enable    = gpu_render_command_transparency_enable(rc);
  cmd->shading_enable         = gpu_render_command_shading_enable(rc);
  cmd->quad_polygon           = gpu_render_command_quad_polygon(rc);
  cmd->dither_enable          = gpu_render_command_is_dithering_enabled(rc) &&
                                gpu_draw_mode_reg_dither_enable(g.draw_mode.mode_reg);
  cmd->draw_mode = g.draw_mode.mode_reg;
  cmd->palette   = g.draw_mode.palette_reg;
  cmd->window    = g.draw_mode.texture_window;
}

void gpu_update_display(bool submit_frame)
{
  const bool interlaced = gpu_is_interlaced_display_enabled();
  const u8 interlaced_field = g.crtc_state.interlaced_field;
  const bool line_skip = (interlaced && gpustat_vertical_resolution(g.GPUSTAT));
  if (gpu_trace_disp_enabled()) {
    static u32 upd_n = 0;
    if ((upd_n++ % 30u) == 0u) {
      fprintf(stderr, "[disp update #%u] addr_start=0x%05X "
                      "vram=(%u,%u) %ux%u origin=(%u,%u) disabled=%u\n",
              upd_n,
              (unsigned)g.crtc_state.display_address_start,
              (unsigned)g.crtc_state.display_vram_left,
              (unsigned)g.crtc_state.display_vram_top,
              (unsigned)g.crtc_state.display_vram_width,
              (unsigned)g.crtc_state.display_vram_height,
              (unsigned)g.crtc_state.display_origin_left,
              (unsigned)g.crtc_state.display_origin_top,
              (unsigned)gpu_is_display_disabled());
    }
  }

  gpu_backend_update_display_cmd_t c;
  memset(&c, 0, sizeof(c));
  c.display_width            = g.crtc_state.display_width;
  c.display_height           = g.crtc_state.display_height;
  c.display_origin_left      = g.crtc_state.display_origin_left;
  c.display_origin_top       = g.crtc_state.display_origin_top;
  c.display_vram_left        = g.crtc_state.display_vram_left;
  c.display_vram_top         = g.crtc_state.display_vram_top;
  c.display_vram_width       = g.crtc_state.display_vram_width;
  c.display_vram_height      = (u16)(g.crtc_state.display_vram_height >> (interlaced ? 1u : 0u));
  c.interlaced_display_enabled = interlaced;
  c.interlaced_display_field   = (interlaced_field != 0);
  c.interlaced_display_interleaved = line_skip;
  c.interleaved_480i_mode      = gpustat_in_interleaved_480i_mode(g.GPUSTAT);
  c.display_24bit              = gpustat_display_area_color_depth_24(g.GPUSTAT);
  c.display_disabled           = gpu_is_display_disabled();
  c.display_pixel_aspect_ratio = 1.0f;
  c.submit_frame               = submit_frame;
  gpu_backend_update_display(&c);
}

void gpu_get_beam_position(u32* out_ticks, u32* out_line)
{
  const tick_count_t pending = gpu_get_pending_crtc_ticks();
  const u32 total_ticks = (u32)g.crtc_state.current_tick_in_scanline + (u32)pending;
  *out_ticks = total_ticks % g.crtc_state.horizontal_total;
  *out_line  = (u32)(g.crtc_state.current_scanline + total_ticks / g.crtc_state.horizontal_total) % g.crtc_state.vertical_total;
}

tick_count_t gpu_get_system_ticks_until_ticks_and_line(u32 ticks, u32 line)
{
  u32 cur_t, cur_l;
  gpu_get_beam_position(&cur_t, &cur_l);
  s32 diff_l = (s32)line - (s32)cur_l;
  if (diff_l < 0) diff_l += g.crtc_state.vertical_total;
  s32 diff_t = (s32)ticks - (s32)cur_t;
  if (diff_t < 0) {
    diff_t += g.crtc_state.horizontal_total;
    diff_l--;
    if (diff_l < 0) diff_l += g.crtc_state.vertical_total;
  }
  const tick_count_t crtc_ticks = diff_l * g.crtc_state.horizontal_total + diff_t;
  return crtc_to_sys_ticks(crtc_ticks, 0);
}

u16 gpu_get_crtc_active_start_line(void) { return g.crtc_state.vertical_display_start; }
u16 gpu_get_crtc_active_end_line  (void) { return g.crtc_state.vertical_display_end;   }
u16 gpu_get_display_width (void) { return g.crtc_state.display_width;  }
u16 gpu_get_display_height(void) { return g.crtc_state.display_height; }

/* Convert a normalized [0..1] window pointer into raster (tick, line) coordinates
 * using the live CRTC state.  The frontend should hand us a position already
 * mapped to the active display rect (i.e. letterboxing stripped).  Returns
 * false when the pointer is outside the active display.  Output `out_tick` is
 * raw CRTC ticks (the GunCon scales to its 8 MHz units; the Justifier feeds
 * raw ticks into the IRQ scheduler). */
bool gpu_query_light_gun_position(u32 pad_index,
                                   float pointer_x_norm,
                                  float pointer_y_norm,
                                  float x_scale,
                                  u16* out_tick,
                                  u16* out_line) 
{
  (void)pad_index;
  if (pointer_x_norm < 0.0f || pointer_y_norm < 0.0f ||
      pointer_x_norm >= 1.0f || pointer_y_norm >= 1.0f)
    return false;

  const float dw = (float)g.crtc_state.display_width;
  const float dh = (float)g.crtc_state.display_height;
  if (dw <= 0.0f || dh <= 0.0f)
    return false;

  float display_x = pointer_x_norm * dw;
  float display_y = pointer_y_norm * dh;
  if (x_scale != 1.0f) {

    float sx = ((display_x / dw) * 2.0f) - 1.0f;
    sx *= x_scale;
    display_x = ((sx + 1.0f) * 0.5f) * dw;
  }
  if (display_x < 0.0f || (u32)display_x >= g.crtc_state.display_width)
    return false;

  const u8 vert_int_shift = gpustat_vertical_interlace(g.GPUSTAT) ? 1u : 0u;
  const u32 line_raw = (u32)(display_y + 0.5f) >> vert_int_shift;
  const u32 line    = line_raw + g.crtc_state.vertical_visible_start;
  const tick_count_t tick_raw = (tick_count_t)(display_x * (float)g.crtc_state.dot_clock_divider + 0.5f);
  const tick_count_t tick     = system_scale_ticks_to_overclock
                                  ? system_scale_ticks_to_overclock(tick_raw)
                                  : tick_raw;
  const u32 final_tick = (u32)tick + (u32)g.crtc_state.horizontal_visible_start;

  *out_tick = (u16)final_tick;
  *out_line = (u16)line;
  return true;
}

#include "gpu_commands.c"

static void gpu_execute_commands(void)
{
  const bool was_executing_from_event = g.executing_commands;
  g.executing_commands = true;
  gpu_try_execute_commands();
  gpu_update_dma_request();
  gpu_update_gpu_idle();
  g.executing_commands = was_executing_from_event;
  if (!was_executing_from_event)
    gpu_update_command_tick_event();
}

static void gpu_end_command(void)
{
  g.blitter_state = GPU_BLITTER_IDLE;
  g.command_total_words = 0;
}
