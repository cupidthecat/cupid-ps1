/*
 * core_audio_stream sits between the producer (SPU) and the backend
 * audio_stream (SDL pull callback).  It owns:
 *   - an aligned interleaved s16 ring buffer (rpos/wpos atomics, SPSC)
 *   - a 64-frame staging buffer + matching float buffer
 *   - a SoundTouch handle for AUDIO_STRETCH_MODE_RESAMPLE / TIME
 *   - the tempo-feedback control loop driving SoundTouch when stretching
 *
 * Producers call core_audio_stream_begin_write / _end_write; consumers go
 * through the audio_stream_t* held internally (its source.read_frames thunk
 * forwards to read_frames() inside this TU).
 *
 * channels x CHUNK_SIZE = 64 makes vectorisation immaterial.
 */

#ifndef CUPID_UTIL_CORE_AUDIO_STREAM_H
#define CUPID_UTIL_CORE_AUDIO_STREAM_H

#include "common/types.h"
#include "core/settings.h"        /* settings_audio_stream_parameters_t */
#include "util/audio_stream.h"    /* audio_backend_t, audio_stretch_mode_t */

typedef struct Error              Error;
typedef struct core_audio_stream  core_audio_stream_t;

core_audio_stream_t* core_audio_stream_create (void);
void                 core_audio_stream_destroy(core_audio_stream_t* self);

/* Allocates the ring + staging buffers, opens the audio backend, and (if
 * stretch_mode != Off) creates a SoundTouch instance.  Returns false on
 * backend-open failure with *error populated. */
bool core_audio_stream_initialize(core_audio_stream_t* self,
                                  audio_backend_t backend, u32 sample_rate,
                                  const settings_audio_stream_parameters_t* params,
                                  const char* driver_name, const char* device_name,
                                  Error* error);

/* Releases everything but leaves the struct re-initialisable. */
void core_audio_stream_shutdown  (core_audio_stream_t* self);

 /* Live-update the stretch parameters (sequence/seekwindow/overlap/quickseek/aa).
 * Resizes buffers if buffer_ms changes; switches SoundTouch instances if
 * stretch_mode changes. */
void core_audio_stream_update_parameters(core_audio_stream_t* self,
                                         const settings_audio_stream_parameters_t* params);

/* Pause / resume backend (stops or starts data requests). */
void core_audio_stream_set_paused       (core_audio_stream_t* self, bool paused);

/* Output-side volume in [0, 100].  100 = bypass. */
void core_audio_stream_set_output_volume(core_audio_stream_t* self, u32 volume);

 /* Tempo (1.0 = real-time).  When throttling at <1.0 SoundTouch slows down,
 * preserving pitch in TimeStretch mode and shifting it in Resample mode. */
void core_audio_stream_set_nominal_rate (core_audio_stream_t* self, float tempo);

/* Switch stretch mode at runtime.  No-op if already in the requested mode. */
void core_audio_stream_set_stretch_mode (core_audio_stream_t* self, audio_stretch_mode_t mode);

/* Hand back a writable region of the staging buffer.  Always succeeds with
 * *num_frames >= 1 (the staging buffer is 64 frames; only flushes on full).
 * Caller fills the region with stereo s16 frames, then calls _end_write
 * with the number of frames actually written. */
void core_audio_stream_begin_write(core_audio_stream_t* self, s16** buffer_ptr, u32* num_frames);
void core_audio_stream_end_write  (core_audio_stream_t* self, u32 num_frames);

 /* Drop everything currently buffered (ring + soundtouch internal state).
 * Used on system reset / disc swap. */
void core_audio_stream_empty_buffer        (core_audio_stream_t* self);

/* Drop just the soundtouch internal pipeline (used when sharply reducing
 * speed so old samples don't trail). */
void core_audio_stream_empty_stretch_buffers(core_audio_stream_t* self);

u32  core_audio_stream_get_buffered_frames_relaxed(const core_audio_stream_t* self);
u32  core_audio_stream_get_target_buffer_size     (const core_audio_stream_t* self);
u32  core_audio_stream_get_sample_rate            (const core_audio_stream_t* self);

#endif /* CUPID_UTIL_CORE_AUDIO_STREAM_H */
