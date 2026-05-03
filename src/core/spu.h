/*
 * namespace SPU -> spu_ prefix.  All SPU state is process-wide singleton
 * (one SPU per console), so storage lives as file-static globals in spu.c.
 *
 *   - As of the SoundTouch port the SPU pushes stereo s16 frames through
 *     core_audio_stream owns the ring buffer + SoundTouch handle.
 *   - The ImGui debug window is dropped (no GUI in cupid-ps1).
 *   - SPU_DUMP_ALL_VOICES and SPU_ENABLE_VU_METER builds are dropped.
 *   - The SIMD reverb resampler is replaced with a scalar version that
 *     produces bit-identical output (the FIR taps and accumulation order
 *     match the SIMD path).
 *   - Cross-module hooks (CDROM stereo input, DMA request line, IRQ9,
 *     timing-event scheduler) come in as `extern` callbacks declared at
 *     the top of spu.c so the SPU stays self-contained until those modules
 *     land.
 *
 *   - CPU overclock honored via system_scale_ticks_to_overclock() in
 *     spu_initialize / spu_cpu_clock_changed and the (X*D)/(N*768) branch
 *     in spu_execute / internal_generate_pending_samples.
 */

#ifndef CUPID_CORE_SPU_H
#define CUPID_CORE_SPU_H

#include "core/types.h"

#include <stdio.h>

typedef struct state_wrapper      state_wrapper_t;
typedef struct core_audio_stream  core_audio_stream_t;

enum {
  SPU_RAM_SIZE    = 512 * 1024,
  SPU_RAM_MASK    = SPU_RAM_SIZE - 1,
  SPU_SAMPLE_RATE = 44100,
};

void spu_initialize(void);
void spu_cpu_clock_changed(void);
void spu_shutdown(void);
void spu_reset(void);
bool spu_do_state(state_wrapper_t* sw);

u16  spu_read_register (u32 offset);
void spu_write_register(u32 offset, u16 value);

void spu_dma_read (u32* words,       u32 word_count);
void spu_dma_write(const u32* words, u32 word_count);

/* Drives the SPU forward by `cpu_ticks` of master clock.  Generates
 * 44100Hz frames into the internal ring buffer; called by the timing
 * event in the system module. */
void spu_execute(tick_count_t cpu_ticks);

/* Force-flush any pending samples up to the current event time.  Used
 * before MMIO reads that depend on the current envelope state. */
void spu_generate_pending_samples(void);

/* Number of CPU ticks until the next 44.1kHz frame is due.  Used by the
 * system to schedule the next call to spu_execute(). */
tick_count_t spu_get_cpu_ticks_until_next_event(void);

const u8* spu_get_ram         (void); /* read-only view of 512KB sound RAM */
u8*       spu_get_writable_ram(void);

bool spu_is_audio_output_muted (void);
void spu_set_audio_output_muted(bool muted);

/* Bind an externally-owned core_audio_stream to the SPU.  spu_execute()
 * will push frames via core_audio_stream_begin_write / _end_write; the
 * stream owns its ring buffer + (optional) SoundTouch handle.  The SPU
 * does not own the stream object.  Pass NULL to detach. */
void                 spu_set_output_stream(core_audio_stream_t* stream);
core_audio_stream_t* spu_get_output_stream(void);

void spu_dbg_dump_counters(FILE* fp);

#endif /* CUPID_CORE_SPU_H */
