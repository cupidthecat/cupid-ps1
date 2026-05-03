/*
 * Notes:
 *  - GSVector SIMD paths (S16<->float, volume scaling, tempo averaging) are
 *    inlined as scalar loops.  At 44.1 kHz / 2ch / CHUNK_SIZE=64 the savings
 *    are below measurement noise, and pulling GSVector into cupid-ps1 is out of
 *    scope here.
 *  - Aligned allocations use posix_memalign instead of make_unique_aligned.
 *  - Translation strings are dropped; display names live in audio_stream.c.
 *  - LOG_TIMESTRETCH_STATS is gated behind #if 0.
 */

#include "core_audio_stream.h"

#include "common/error.h"
#include "common/log.h"
#include "common/timer.h"
#include "common/types.h"
#include "util/audio_stream.h"

#include "soundtouch/SoundTouchDLL.h"

/* SoundTouchDLL.h does not export the SETTING_* constants; those live in
 * SoundTouch.h which is C++.  Mirror the integer IDs verbatim so we can use
 * the C wrapper API without dragging in the C++ header. */
#define SETTING_USE_AA_FILTER  0
#define SETTING_USE_QUICKSEEK  2
#define SETTING_SEQUENCE_MS    3
#define SETTING_SEEKWINDOW_MS  4
#define SETTING_OVERLAP_MS     5

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

LOG_CHANNEL(AudioStream);

#define NUM_CHANNELS              2u
#define CHUNK_SIZE                64u    /* power of two; align target */
#define AVERAGING_BUFFER_SIZE     256u
#define STRETCH_RESET_THRESHOLD   5u

#if (CHUNK_SIZE & (CHUNK_SIZE - 1u)) != 0u
# error "CHUNK_SIZE must be a power of two"
#endif

#define ALIGNMENT  64u

struct core_audio_stream {
  audio_stream_t* stream;
  u32             sample_rate;
  u32             volume;
  settings_audio_stream_parameters_t params;
  bool            stretch_inactive;
  bool            filling;
  bool            paused;

  u32             buffer_size;        /* in frames */
  s16*            buffer;             /* interleaved L,R, length = buffer_size * NUM_CHANNELS */
  s16*            staging_buffer;     /* CHUNK_SIZE * NUM_CHANNELS */
  float*          float_buffer;       /* CHUNK_SIZE * NUM_CHANNELS */

  _Atomic u32     rpos;
  _Atomic u32     wpos;

  ST_HANDLE       soundtouch;         /* NULL when stretch_mode == Off */

  u32             target_buffer_size;
  u32             stretch_reset;
  u64             stretch_reset_time;

  u32             stretch_ok_count;
  float           nominal_rate;
  float           dynamic_target_usage;

  u32             average_position;
  u32             average_available;
  u32             staging_buffer_pos; /* in samples (s16 elements), not frames */

  float           average_fullness[AVERAGING_BUFFER_SIZE];
};

static void  read_frames_thunk(void* user, s16* dst, u32 num_frames);
static void  read_frames      (core_audio_stream_t* self, s16* samples, u32 num_frames);
static void  internal_write_frames(core_audio_stream_t* self, const s16* data, u32 num_frames);

static void  allocate_buffer(core_audio_stream_t* self);
static void  destroy_buffer (core_audio_stream_t* self);

static void  stretch_allocate          (core_audio_stream_t* self);
static void  stretch_update_parameters (core_audio_stream_t* self,
                                        const settings_audio_stream_parameters_t* params);
static void  stretch_destroy           (core_audio_stream_t* self);
static void  stretch_write_block       (core_audio_stream_t* self, const float* block);
static void  stretch_underrun          (core_audio_stream_t* self);
static void  stretch_overrun           (core_audio_stream_t* self);
static void  update_stretch_tempo      (core_audio_stream_t* self);
static float add_and_get_average_tempo (core_audio_stream_t* self, float val);

static inline u32 align_up_pow2(u32 v, u32 a) { return (v + (a - 1u)) & ~(a - 1u); }
static inline u32 align_down_pow2(u32 v, u32 a) { return v & ~(a - 1u); }

static inline u32 get_aligned_buffer_size(u32 size)        { return align_up_pow2(size, CHUNK_SIZE); }
static inline u32 get_buffer_size_for_ms(u32 sr, u32 ms)   { return get_aligned_buffer_size((ms * sr) / 1000u); }

static inline float clampf(float x, float lo, float hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static inline float minf  (float a, float b)            { return (a < b) ? a : b; }
static inline u32   minu  (u32   a, u32   b)            { return (a < b) ? a : b; }

static inline bool in_range_f(float v, float lo, float hi) { return (lo <= v) && (v <= hi); }

static inline bool is_stretch_enabled(const core_audio_stream_t* s)
{
  return s->params.stretch_mode != AUDIO_STRETCH_MODE_OFF;
}

static s16* aligned_alloc_s16(size_t n_samples)
{
  void* p = NULL;
  size_t bytes = align_up_pow2((u32)(n_samples * sizeof(s16)), ALIGNMENT);
  if (posix_memalign(&p, ALIGNMENT, bytes) != 0) return NULL;
  return (s16*)p;
}

static float* aligned_alloc_f32(size_t n_samples)
{
  void* p = NULL;
  size_t bytes = align_up_pow2((u32)(n_samples * sizeof(float)), ALIGNMENT);
  if (posix_memalign(&p, ALIGNMENT, bytes) != 0) return NULL;
  return (float*)p;
}

static void s16_chunk_to_float(const s16* src, float* dst, u32 num_samples)
{
  static const float SCALE = 1.0f / 32767.0f;
  for (u32 i = 0; i < num_samples; i++)
    dst[i] = (float)src[i] * SCALE;
}

static void float_chunk_to_s16(s16* dst, const float* src, u32 num_samples)
{
  static const float SCALE = 32767.0f;
  for (u32 i = 0; i < num_samples; i++) {
    float v = src[i] * SCALE;
    if (v > 32767.0f)  v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    dst[i] = (s16)v;
  }
}

core_audio_stream_t* core_audio_stream_create(void)
{
  core_audio_stream_t* s = (core_audio_stream_t*)calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->paused      = true;
  s->nominal_rate = 1.0f;
  s->stretch_reset = STRETCH_RESET_THRESHOLD;
  return s;
}

void core_audio_stream_destroy(core_audio_stream_t* self)
{
  if (!self) return;
  core_audio_stream_shutdown(self);
  free(self);
}

bool core_audio_stream_initialize(core_audio_stream_t* self,
                                  audio_backend_t backend, u32 sample_rate,
                                  const settings_audio_stream_parameters_t* params,
                                  const char* driver_name, const char* device_name,
                                  Error* error)
{
  core_audio_stream_shutdown(self);

  self->sample_rate = sample_rate;
  self->volume      = 100;
  self->params      = *params;
  self->filling     = false;
  self->paused      = false;

  allocate_buffer(self);
  stretch_allocate(self);

  const u32 latency_ms      = (params->output_latency_ms != 0) ? params->output_latency_ms : params->buffer_ms;
  const u32 latency_frames  = get_buffer_size_for_ms(sample_rate, latency_ms);

  if (backend != AUDIO_BACKEND_NULL) {
    audio_stream_source_t src;
    src.read_frames = read_frames_thunk;
    src.user        = self;

     self->stream = audio_stream_create(backend, sample_rate, NUM_CHANNELS,
                                       latency_frames, params->output_latency_minimal,
                                       driver_name, device_name, src, 
                                       /*auto_start*/ true, error);
    if (!self->stream) {
      core_audio_stream_shutdown(self);
      return false;
    }
  } else {
    /* Null backend; no stretching, just keep the buffer alive but paused. */
    settings_audio_stream_parameters_t fresh = (settings_audio_stream_parameters_t){0};
    fresh.stretch_mode = AUDIO_STRETCH_MODE_OFF;
    fresh.buffer_ms    = params->buffer_ms;
    self->params       = fresh;
    self->paused       = true;
  }

  return true;
}

void core_audio_stream_shutdown(core_audio_stream_t* self)
{
  if (!self) return;
  if (self->stream) {
    audio_stream_destroy(self->stream);
    self->stream = NULL;
  }
  stretch_destroy(self);
  destroy_buffer(self);
  self->sample_rate = 0;
  self->params      = (settings_audio_stream_parameters_t){0};
  self->volume      = 0;
  self->filling     = false;
  self->paused      = true;
}

static void copy_stretch_params(settings_audio_stream_parameters_t* dst,
                                const settings_audio_stream_parameters_t* src)
{
  dst->stretch_mode               = src->stretch_mode;
  dst->stretch_sequence_length_ms = src->stretch_sequence_length_ms;
  dst->stretch_seekwindow_ms      = src->stretch_seekwindow_ms;
  dst->stretch_overlap_ms         = src->stretch_overlap_ms;
  dst->stretch_use_quickseek      = src->stretch_use_quickseek;
  dst->stretch_use_aa_filter      = src->stretch_use_aa_filter;
}

void core_audio_stream_update_parameters(core_audio_stream_t* self,
                                         const settings_audio_stream_parameters_t* params)
{
  if (params->buffer_ms != self->params.buffer_ms) {
    Error e; Error_init(&e);

    if (self->stream && !self->paused) {
      if (!audio_stream_stop(self->stream, &e)) {
        ERROR_LOG("Failed to stop audio stream for buffer size change: %s",
                  Error_get_description(&e));
        Error_destroy(&e);
        return;
      }
    }

    stretch_destroy(self);
    destroy_buffer(self);

    self->params.buffer_ms = params->buffer_ms;
    copy_stretch_params(&self->params, params);

    allocate_buffer(self);
    stretch_allocate(self);

    if (self->stream && !self->paused) {
      if (!audio_stream_start(self->stream, &e)) {
        ERROR_LOG("Failed to start audio stream after buffer size change: %s",
                  Error_get_description(&e));
        self->paused = true;
      }
    }
    Error_destroy(&e);
    return;
  }

  if (params->stretch_mode != self->params.stretch_mode) {
    stretch_destroy(self);
    copy_stretch_params(&self->params, params);
    stretch_allocate(self);
  } else {
    stretch_update_parameters(self, params);
  }
}

void core_audio_stream_set_paused(core_audio_stream_t* self, bool paused)
{
  if (self->paused == paused || !self->stream) return;

  Error e; Error_init(&e);
  bool ok = paused ? audio_stream_stop(self->stream, &e)
                   : audio_stream_start(self->stream, &e);
  if (!ok)
    ERROR_LOG("Failed to %s stream: %s", paused ? "pause" : "restart", Error_get_description(&e));
  else
    self->paused = paused;
  Error_destroy(&e);
}

void core_audio_stream_set_output_volume(core_audio_stream_t* self, u32 volume)
{
  self->volume = volume;
}

void core_audio_stream_set_nominal_rate(core_audio_stream_t* self, float tempo)
{
  self->nominal_rate = tempo;
  if (!self->soundtouch) return;
  if (self->params.stretch_mode == AUDIO_STRETCH_MODE_RESAMPLE)
    soundtouch_setRate(self->soundtouch, tempo);
  else if (self->params.stretch_mode == AUDIO_STRETCH_MODE_TIME && !self->stretch_inactive)
    soundtouch_setTempo(self->soundtouch, tempo);
}

void core_audio_stream_set_stretch_mode(core_audio_stream_t* self, audio_stretch_mode_t mode)
{
  if (self->params.stretch_mode == mode) return;

  bool was_paused = self->paused;
  if (!was_paused) core_audio_stream_set_paused(self, true);

  destroy_buffer(self);
  stretch_destroy(self);
  self->params.stretch_mode = mode;

  allocate_buffer(self);
  if (self->params.stretch_mode != AUDIO_STRETCH_MODE_OFF)
    stretch_allocate(self);

  if (!was_paused) core_audio_stream_set_paused(self, false);
}

void core_audio_stream_begin_write(core_audio_stream_t* self, s16** buffer_ptr, u32* num_frames)
{
  *buffer_ptr = &self->staging_buffer[self->staging_buffer_pos];
  *num_frames = CHUNK_SIZE - (self->staging_buffer_pos / NUM_CHANNELS);
}

void core_audio_stream_end_write(core_audio_stream_t* self, u32 num_frames)
{
  /* Don't bother committing while muted or paused. */
  if (self->volume == 0u || self->paused) return;

  self->staging_buffer_pos += num_frames * NUM_CHANNELS;
  /* If less than a full chunk, accumulate more. */
  if ((self->staging_buffer_pos / NUM_CHANNELS) < CHUNK_SIZE) return;

  self->staging_buffer_pos = 0;

  if (!is_stretch_enabled(self)) {
    internal_write_frames(self, self->staging_buffer, CHUNK_SIZE);
    return;
  }

  s16_chunk_to_float(self->staging_buffer, self->float_buffer, CHUNK_SIZE * NUM_CHANNELS);
  stretch_write_block(self, self->float_buffer);
}

static void allocate_buffer(core_audio_stream_t* self)
{
  /* TimeStretch can balloon under-supply; resample needs less; off needs none. */
  u32 multiplier;
  switch (self->params.stretch_mode) {
    case AUDIO_STRETCH_MODE_TIME:     multiplier = 16u; break;
    case AUDIO_STRETCH_MODE_OFF:      multiplier = 1u;  break;
    case AUDIO_STRETCH_MODE_RESAMPLE:
    default:                          multiplier = 2u;  break;
  }

  self->buffer_size = get_aligned_buffer_size(((self->params.buffer_ms * multiplier) * self->sample_rate) / 1000u);
  self->target_buffer_size = get_aligned_buffer_size((self->sample_rate * self->params.buffer_ms) / 1000u);

  self->buffer         = aligned_alloc_s16((size_t)self->buffer_size * NUM_CHANNELS);
  self->staging_buffer = aligned_alloc_s16((size_t)CHUNK_SIZE * NUM_CHANNELS);
  self->float_buffer   = aligned_alloc_f32((size_t)CHUNK_SIZE * NUM_CHANNELS);

  /* Zero out staging+float so a partially-filled chunk doesn't leak garbage. */
  memset(self->buffer,         0, self->buffer_size * NUM_CHANNELS * sizeof(s16));
  memset(self->staging_buffer, 0, CHUNK_SIZE * NUM_CHANNELS * sizeof(s16));
  memset(self->float_buffer,   0, CHUNK_SIZE * NUM_CHANNELS * sizeof(float));
  self->staging_buffer_pos = 0;
  atomic_store_explicit(&self->rpos, 0, memory_order_release);
  atomic_store_explicit(&self->wpos, 0, memory_order_release);

  DEV_LOG("Allocated buffer of %u frames for buffer of %u ms [stretch %s, target size %u].",
          self->buffer_size, (unsigned)self->params.buffer_ms,
          audio_stream_get_stretch_mode_name(self->params.stretch_mode),
          self->target_buffer_size);
}

static void destroy_buffer(core_audio_stream_t* self)
{
  free(self->staging_buffer); self->staging_buffer = NULL;
  free(self->float_buffer);   self->float_buffer   = NULL;
  free(self->buffer);         self->buffer         = NULL;
  self->buffer_size = 0;
  self->staging_buffer_pos = 0;
  atomic_store_explicit(&self->wpos, 0, memory_order_release);
  atomic_store_explicit(&self->rpos, 0, memory_order_release);
}

void core_audio_stream_empty_buffer(core_audio_stream_t* self)
{
  if (is_stretch_enabled(self) && self->soundtouch) {
    soundtouch_clear(self->soundtouch);
    if (self->params.stretch_mode == AUDIO_STRETCH_MODE_TIME)
      soundtouch_setTempo(self->soundtouch, self->nominal_rate);
  }
  /* Snap wpos -> rpos to drain the ring without a memory wipe. */
  atomic_store_explicit(&self->wpos,
                        atomic_load_explicit(&self->rpos, memory_order_acquire),
                        memory_order_release);
}

u32 core_audio_stream_get_buffered_frames_relaxed(const core_audio_stream_t* self)
{
  const u32 rpos = atomic_load_explicit(&self->rpos, memory_order_relaxed);
  const u32 wpos = atomic_load_explicit(&self->wpos, memory_order_relaxed);
  return (wpos + self->buffer_size - rpos) % self->buffer_size;
}

u32 core_audio_stream_get_target_buffer_size(const core_audio_stream_t* self) { return self->target_buffer_size; }
u32 core_audio_stream_get_sample_rate       (const core_audio_stream_t* self) { return self->sample_rate; }

static void read_frames_thunk(void* user, s16* dst, u32 num_frames)
{
  read_frames((core_audio_stream_t*)user, dst, num_frames);
}

static void read_frames(core_audio_stream_t* self, s16* samples, u32 num_frames)
{
  /* Ring may not have been allocated yet (e.g., backend started callbacks
   * before initialize finished, or after shutdown raced).  Emit silence. */
  if (!self->buffer || self->buffer_size == 0) {
    memset(samples, 0, (size_t)num_frames * NUM_CHANNELS * sizeof(s16));
    return;
  }

  const u32 available_frames = core_audio_stream_get_buffered_frames_relaxed(self);
  u32 frames_to_read = num_frames;
  u32 silence_frames = 0;

  if (self->filling) {
    u32 to_fill = self->buffer_size /
                  ((self->params.stretch_mode != AUDIO_STRETCH_MODE_TIME) ? 32u : 400u);
    to_fill = get_aligned_buffer_size(to_fill);

    if (available_frames < to_fill) {
      silence_frames = num_frames;
      frames_to_read = 0;
    } else {
      self->filling = false;
      VERBOSE_LOG("Underrun compensation done (%u frames buffered)", to_fill);
    }
  }

  if (available_frames < frames_to_read) {
    silence_frames = frames_to_read - available_frames;
    frames_to_read = available_frames;
    self->filling = true;
    if (self->params.stretch_mode == AUDIO_STRETCH_MODE_TIME)
      stretch_underrun(self);
  }

  if (frames_to_read > 0u) {
    u32 rpos = atomic_load_explicit(&self->rpos, memory_order_acquire);

    u32 end = self->buffer_size - rpos;
    if (end > frames_to_read) end = frames_to_read;

    if (end > 0u) {
      memcpy(samples, &self->buffer[rpos * NUM_CHANNELS],
             (size_t)end * NUM_CHANNELS * sizeof(s16));
      rpos += end;
      if (rpos == self->buffer_size) rpos = 0;
    }

    const u32 start = frames_to_read - end;
    if (start > 0u) {
      memcpy(&samples[end * NUM_CHANNELS], &self->buffer[0],
             (size_t)start * NUM_CHANNELS * sizeof(s16));
      rpos = start;
    }

    atomic_store_explicit(&self->rpos, rpos, memory_order_release);
  }

  if (silence_frames > 0u) {
    if (frames_to_read > 0u) {
      /* Spread the available samples across num_frames; aliased but not */
      const u32 increment = (u32)(65536.0f * ((float)frames_to_read / (float)num_frames));
      s16 resample_buf[CHUNK_SIZE * NUM_CHANNELS * 8u]; /* SDL callback typ. <= 1024 frames; large enough */
      const u32 stride = NUM_CHANNELS;
      const u32 max_resample = (u32)(sizeof(resample_buf) / (NUM_CHANNELS * sizeof(s16)));
      const u32 copy_n = (frames_to_read < max_resample) ? frames_to_read : max_resample;
      memcpy(resample_buf, samples, (size_t)copy_n * stride * sizeof(s16));

      const s16* src_ptr = resample_buf;
      s16*       out_ptr = samples;
      u32 sub = 0;
      for (u32 i = 0; i < num_frames; i++) {
        out_ptr[0] = src_ptr[0];
        out_ptr[1] = src_ptr[1];
        out_ptr += NUM_CHANNELS;
        sub += increment;
        const u32 step = sub >> 16;
        src_ptr += step * NUM_CHANNELS;
        sub &= 0xFFFFu;
        if (src_ptr >= &resample_buf[copy_n * NUM_CHANNELS])
          src_ptr = &resample_buf[(copy_n - 1u) * NUM_CHANNELS];
      }

      VERBOSE_LOG("Audio buffer underflow, resampled %u frames to %u", frames_to_read, num_frames);
    } else {
      memset(samples + (frames_to_read * NUM_CHANNELS), 0,
             (size_t)silence_frames * NUM_CHANNELS * sizeof(s16));
    }
  }

  if (self->volume != 100u) {
    const u32 num_samples = num_frames * NUM_CHANNELS;
    const float vol_mult = (float)self->volume / 100.0f;
    for (u32 i = 0; i < num_samples; i++) {
      float v = (float)samples[i] * vol_mult;
      if (v >  32767.0f) v =  32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      samples[i] = (s16)v;
    }
  }
}

static void internal_write_frames(core_audio_stream_t* self, const s16* data, u32 num_frames)
{
  const u32 free_frames = self->buffer_size - core_audio_stream_get_buffered_frames_relaxed(self);
  if (free_frames <= num_frames) {
    if (self->params.stretch_mode == AUDIO_STRETCH_MODE_TIME) {
      stretch_overrun(self);
    } else {
      DEBUG_LOG("Buffer overrun, chunk dropped");
      return;
    }
  }

  u32 wpos = atomic_load_explicit(&self->wpos, memory_order_acquire);

  if ((self->buffer_size - wpos) <= num_frames) {
    const u32 end   = self->buffer_size - wpos;
    const u32 start = num_frames - end;

    memcpy(&self->buffer[wpos * NUM_CHANNELS], data, (size_t)end * NUM_CHANNELS * sizeof(s16));
    if (start > 0u)
      memcpy(&self->buffer[0], data + end * NUM_CHANNELS, (size_t)start * NUM_CHANNELS * sizeof(s16));
    wpos = start;
  } else {
    memcpy(&self->buffer[wpos * NUM_CHANNELS], data, (size_t)num_frames * NUM_CHANNELS * sizeof(s16));
    wpos += num_frames;
  }

  atomic_store_explicit(&self->wpos, wpos, memory_order_release);
}

static void stretch_allocate(core_audio_stream_t* self)
{
  if (self->params.stretch_mode == AUDIO_STRETCH_MODE_OFF) return;

  self->soundtouch = soundtouch_createInstance();
  soundtouch_setSampleRate(self->soundtouch, self->sample_rate);
  soundtouch_setChannels  (self->soundtouch, NUM_CHANNELS);

  soundtouch_setSetting(self->soundtouch, SETTING_USE_QUICKSEEK, self->params.stretch_use_quickseek);
  soundtouch_setSetting(self->soundtouch, SETTING_USE_AA_FILTER, self->params.stretch_use_aa_filter);

  soundtouch_setSetting(self->soundtouch, SETTING_SEQUENCE_MS,    self->params.stretch_sequence_length_ms);
  soundtouch_setSetting(self->soundtouch, SETTING_SEEKWINDOW_MS,  self->params.stretch_seekwindow_ms);
  soundtouch_setSetting(self->soundtouch, SETTING_OVERLAP_MS,     self->params.stretch_overlap_ms);

  if (self->params.stretch_mode == AUDIO_STRETCH_MODE_RESAMPLE)
    soundtouch_setRate (self->soundtouch, self->nominal_rate);
  else
    soundtouch_setTempo(self->soundtouch, self->nominal_rate);

  self->stretch_reset        = STRETCH_RESET_THRESHOLD;
  self->stretch_inactive     = false;
  self->stretch_ok_count     = 0;
  self->dynamic_target_usage = 0.0f;
  self->average_position     = 0;
  self->average_available    = 0;
  self->staging_buffer_pos   = 0;
}

static void stretch_update_parameters(core_audio_stream_t* self,
                                      const settings_audio_stream_parameters_t* params)
{
  if (self->params.stretch_mode == AUDIO_STRETCH_MODE_OFF || !self->soundtouch) return;

  if (params->stretch_use_quickseek != self->params.stretch_use_quickseek) {
    self->params.stretch_use_quickseek = params->stretch_use_quickseek;
    soundtouch_setSetting(self->soundtouch, SETTING_USE_QUICKSEEK, self->params.stretch_use_quickseek);
  }
  if (params->stretch_use_aa_filter != self->params.stretch_use_aa_filter) {
    self->params.stretch_use_aa_filter = params->stretch_use_aa_filter;
    soundtouch_setSetting(self->soundtouch, SETTING_USE_AA_FILTER, self->params.stretch_use_aa_filter);
  }
  if (params->stretch_sequence_length_ms != self->params.stretch_sequence_length_ms) {
    self->params.stretch_sequence_length_ms = params->stretch_sequence_length_ms;
    soundtouch_setSetting(self->soundtouch, SETTING_SEQUENCE_MS, self->params.stretch_sequence_length_ms);
  }
  if (params->stretch_seekwindow_ms != self->params.stretch_seekwindow_ms) {
    self->params.stretch_seekwindow_ms = params->stretch_seekwindow_ms;
    soundtouch_setSetting(self->soundtouch, SETTING_SEEKWINDOW_MS, self->params.stretch_seekwindow_ms);
  }
  if (params->stretch_overlap_ms != self->params.stretch_overlap_ms) {
    self->params.stretch_overlap_ms = params->stretch_overlap_ms;
    soundtouch_setSetting(self->soundtouch, SETTING_OVERLAP_MS, self->params.stretch_overlap_ms);
  }
}

static void stretch_destroy(core_audio_stream_t* self)
{
  if (self->soundtouch) {
    soundtouch_destroyInstance(self->soundtouch);
    self->soundtouch = NULL;
  }
}

static void stretch_write_block(core_audio_stream_t* self, const float* block)
{
  if (is_stretch_enabled(self) && self->soundtouch) {
    soundtouch_putSamples(self->soundtouch, block, CHUNK_SIZE);

    for (;;) {
      const u32 received = soundtouch_receiveSamples(self->soundtouch, self->float_buffer, CHUNK_SIZE);
      if (received == 0) break;
      float_chunk_to_s16(self->staging_buffer, self->float_buffer, received * NUM_CHANNELS);
      internal_write_frames(self, self->staging_buffer, received);
    }

    if (self->params.stretch_mode == AUDIO_STRETCH_MODE_TIME)
      update_stretch_tempo(self);
  } else {
    float_chunk_to_s16(self->staging_buffer, block, CHUNK_SIZE * NUM_CHANNELS);
    internal_write_frames(self, self->staging_buffer, CHUNK_SIZE);
  }
}

static float add_and_get_average_tempo(core_audio_stream_t* self, float val)
{
  static const u32 AVERAGING_WINDOW = 50;

  if (self->average_available < AVERAGING_BUFFER_SIZE)
    self->average_available++;

  self->average_fullness[self->average_position] = val;
  self->average_position = (self->average_position + 1u) % AVERAGING_BUFFER_SIZE;

  const u32 actual_window = minu(self->average_available, AVERAGING_WINDOW);
  u32 index = (self->average_position - actual_window + AVERAGING_BUFFER_SIZE) % AVERAGING_BUFFER_SIZE;
  float sum = 0.0f;
  for (u32 i = 0; i < actual_window; i++) {
    sum += self->average_fullness[index];
    index = (index + 1u) % AVERAGING_BUFFER_SIZE;
  }
  sum = (actual_window > 0u) ? (sum / (float)actual_window) : 0.0f;
  return (sum != 0.0f) ? sum : 1.0f;
}

static void update_stretch_tempo(core_audio_stream_t* self)
{
  static const float MIN_TEMPO            = 0.05f;
  static const float MAX_TEMPO            = 500.0f;
  static const float INACTIVE_GOOD_FACTOR = 1.04f;
  static const float INACTIVE_BAD_FACTOR  = 1.2f;
  static const u32   INACTIVE_MIN_OK_COUNT = 50;
  static const u32   COMPENSATION_DIVIDER  = 100;

  float base_target_usage = (float)self->target_buffer_size / minf(self->nominal_rate, 1.0f);

  const u32 ibuffer_usage = core_audio_stream_get_buffered_frames_relaxed(self);
  const float buffer_usage = (float)ibuffer_usage;
  float tempo = (self->dynamic_target_usage > 0.0f) ? (buffer_usage / self->dynamic_target_usage) : 1.0f;

  if (self->stretch_reset >= STRETCH_RESET_THRESHOLD) {
    VERBOSE_LOG("___ Stretcher is being reset.");
    self->stretch_inactive     = false;
    self->stretch_ok_count     = 0;
    self->dynamic_target_usage = base_target_usage;
    self->average_available    = 0;
    self->average_position     = 0;
    self->stretch_reset        = 0;
    tempo = self->nominal_rate;
  } else if (self->stretch_reset > 0u) {
    const u64 now = (u64)timer_get_current_value();
    if (timer_value_to_seconds(now - self->stretch_reset_time) >= 2.0) {
      self->stretch_reset--;
      self->stretch_reset_time = now;
    }
  }

  tempo = add_and_get_average_tempo(self, tempo);

  if (tempo < 2.0f) tempo = sqrtf(tempo);
  tempo = clampf(tempo, MIN_TEMPO, MAX_TEMPO);

  if (tempo < 1.0f) base_target_usage /= sqrtf(tempo);

   self->dynamic_target_usage +=
    (float)((double)(base_target_usage / tempo - self->dynamic_target_usage) 
            / (double)COMPENSATION_DIVIDER);

  if (in_range_f(tempo, 0.9f, 1.1f) &&
      in_range_f(self->dynamic_target_usage, base_target_usage * 0.9f, base_target_usage * 1.1f)) {
    self->dynamic_target_usage = base_target_usage;
  }

  if (!self->stretch_inactive) {
    if (in_range_f(tempo, 1.0f / INACTIVE_GOOD_FACTOR, INACTIVE_GOOD_FACTOR))
      self->stretch_ok_count++;
    else
      self->stretch_ok_count = 0;

    if (self->stretch_ok_count >= INACTIVE_MIN_OK_COUNT) {
      VERBOSE_LOG("=== Stretcher is now inactive.");
      self->stretch_inactive = true;
    }
  } else if (!in_range_f(tempo, 1.0f / INACTIVE_BAD_FACTOR, INACTIVE_BAD_FACTOR)) {
    VERBOSE_LOG("~~~ Stretcher is now active @ tempo %.4f.", (double)tempo);
    self->stretch_inactive = false;
    self->stretch_ok_count = 0;
  }

  if (self->stretch_inactive) tempo = self->nominal_rate;

  if (self->soundtouch)
    soundtouch_setTempo(self->soundtouch, tempo);

  /* base_target_usage / buffer_usage are kept live in case the diagnostic
   * block (LOG_TIMESTRETCH_STATS) is enabled in the future. */
  (void)buffer_usage;
}

static void stretch_underrun(core_audio_stream_t* self)
{
  self->stretch_reset++;
  if (self->stretch_reset < STRETCH_RESET_THRESHOLD)
    self->stretch_reset_time = (u64)timer_get_current_value();
}

static void stretch_overrun(core_audio_stream_t* self)
{
  self->stretch_reset++;
  if (self->stretch_reset < STRETCH_RESET_THRESHOLD)
    self->stretch_reset_time = (u64)timer_get_current_value();

  /* Drop two chunks of older samples to give the stretcher headroom. */
  const u32 discard = CHUNK_SIZE * 2u;
  const u32 rpos = atomic_load_explicit(&self->rpos, memory_order_acquire);
  atomic_store_explicit(&self->rpos, (rpos + discard) % self->buffer_size, memory_order_release);
}

void core_audio_stream_empty_stretch_buffers(core_audio_stream_t* self)
{
  if (!is_stretch_enabled(self)) return;
  self->stretch_reset = STRETCH_RESET_THRESHOLD;
  if (self->soundtouch) soundtouch_clear(self->soundtouch);
}
