/*
 * Central GPU register / command emulator.  The C++ class GPU folds into a
 * file-static gpu_state_t inside gpu.c; this header exposes the public
 * functions the rest of the system calls (bus MMIO, DMA channel 2, timing
 * events, save state).  Hardware-renderer paths, runahead, GPU dumps,
 * media-capture, debug imgui, screenshots and PGXP are all dropped.
 */

#ifndef CUPID_CORE_GPU_H
#define CUPID_CORE_GPU_H

#include "core/gpu_types.h"
#include "core/types.h"
#include "common/types.h"

typedef struct state_wrapper state_wrapper_t;

enum {
  GPU_MAX_FIFO_SIZE        = 4096,
  GPU_DOT_TIMER_INDEX      = 0,
  GPU_HBLANK_TIMER_INDEX   = 1,
  GPU_DRAWING_AREA_COORD_MASK = 1023,

  GPU_NTSC_TICKS_PER_LINE  = 3413,
  GPU_NTSC_TOTAL_LINES     = 263,
  GPU_PAL_TICKS_PER_LINE   = 3406,
  GPU_PAL_TOTAL_LINES      = 314,

  GPU_NTSC_HORIZONTAL_ACTIVE_START = 488,
  GPU_NTSC_HORIZONTAL_ACTIVE_END   = 3288,
  GPU_NTSC_VERTICAL_ACTIVE_START   = 16,
  GPU_NTSC_VERTICAL_ACTIVE_END     = 256,
  GPU_PAL_HORIZONTAL_ACTIVE_START  = 488,
  GPU_PAL_HORIZONTAL_ACTIVE_END    = 3300,
  GPU_PAL_VERTICAL_ACTIVE_START    = 20,
  GPU_PAL_VERTICAL_ACTIVE_END      = 308,
};

void gpu_initialize(void);
void gpu_shutdown  (void);
void gpu_reset     (bool clear_vram);
bool gpu_do_state  (state_wrapper_t* sw);

void gpu_cpu_clock_changed(void);

u32  gpu_read_register (u32 offset);
void gpu_write_register(u32 offset, u32 value);

/* GPUREAD->CPU read: fills `words` with VRAM data via GPUREAD path. */
void gpu_dma_read (u32* words, u32 word_count);
/* CPU->GP0 write: pushes `value` into the GP0 FIFO (no immediate execute).
 * `address` is the RAM source address (unused today; reserved for gpu-dump). */
void gpu_dma_write(u32 address, u32 value);
/* Drains pending GP0 commands (called at end of DMA chunk). */
void gpu_end_dma_write(void);
/* True if the GPU is configured to accept DMA writes (CPUtoGP0 or FIFO). */
bool gpu_begin_dma_write(void);
/* Compat aliases for older callers. */
void gpu_dma_end_write(void);
bool gpu_dma_can_write(void);

/* Lightweight frontend trace counters, indexed by GP0 opcode. */
const u32* gpu_dbg_op_counts(void);

#include <stdio.h>
void gpu_dbg_dump_recent_gp1(FILE* out);
void gpu_dbg_dump_recent_gp0(FILE* out);

void gpu_update_display(bool submit_frame);

void gpu_crtc_tick_event   (tick_count_t ticks);
void gpu_command_tick_event(tick_count_t ticks);
void gpu_frame_done_event  (tick_count_t ticks);

/* True if the CRTC raster has crossed into a new line since the last sync. */
bool gpu_is_crtc_scanline_pending(void);
/* True if pending command ticks have completed. */
bool gpu_is_command_completion_pending(void);

/* Pulls the CRTC tick event forward, draining any pending raster updates. */
void gpu_synchronize_crtc(void);

/* Beam position helpers (used by Timer dot/hblank inputs). */
void          gpu_get_beam_position(u32* out_ticks, u32* out_line);
tick_count_t  gpu_get_system_ticks_until_ticks_and_line(u32 ticks, u32 line);
tick_count_t  gpu_get_crtc_frequency(void);
bool          gpu_is_in_pal_mode(void);

/* PAL/NTSC ticks-per-line/total-lines accessors for timer module. */
u16 gpu_get_horizontal_total(void);
u16 gpu_get_vertical_total  (void);

/* CRTC active-display accessors (used by light-gun controllers). */
u16 gpu_get_crtc_active_start_line(void);
u16 gpu_get_crtc_active_end_line  (void);
u16 gpu_get_display_width (void);
u16 gpu_get_display_height(void);

/* Light-gun: maps a [0..1] window pointer (already mapped to the active
 * display rect by the frontend) to raster (tick, line).  out_tick is in raw
 * CRTC ticks.  Returns false if the pointer is outside the active display. */
bool gpu_query_light_gun_position(u32 pad_index,
                                   float pointer_x_norm,
                                  float pointer_y_norm,
                                  float x_scale,
                                  u16* out_tick, 
                                  u16* out_line);

#endif /* CUPID_CORE_GPU_H */
