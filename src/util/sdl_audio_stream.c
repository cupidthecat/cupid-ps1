/*
 * pull-callback) downgraded to the SDL2 push-callback API: SDL2 calls our
 * callback with a fixed-size byte buffer, which we satisfy by pulling
 * straight from the source.  The source is expected to be lock-free w.r.t.
 * the main thread (the SPU's own ring buffer guarantees that).
 */

#include "audio_stream.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/types.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(AudioStream);

typedef struct {
  audio_stream_t      base;
  SDL_AudioDeviceID   device_id;
  SDL_AudioSpec       obtained_spec;
} sdl_audio_stream_t;

/* SDL2's audio subsystem is reference-counted internally, but we still want a
 * single global init+atexit so concrete streams don't have to coordinate.
 * Repeated calls are cheap and safe. */
static bool s_sdl_audio_initialized = false;

static void sdl_audio_atexit(void)
{
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static bool sdl_audio_initialize(Error* error)
{
  if (s_sdl_audio_initialized)
    return true;

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
  {
    Error_set_string_fmt(error, "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
    return false;
  }

  atexit(sdl_audio_atexit);
  s_sdl_audio_initialized = true;
  return true;
}

static void sdl_audio_callback(void* userdata, Uint8* stream, int len)
{
  sdl_audio_stream_t* const self = (sdl_audio_stream_t*)userdata;
  const u32 frame_bytes = (u32)sizeof(audio_stream_sample_t) * self->base.channels;
  if (len <= 0 || frame_bytes == 0)
  {
    if (len > 0)
      memset(stream, 0, (size_t)len);
    return;
  }

  const u32 num_frames = (u32)len / frame_bytes;
  if (num_frames == 0)
  {
    memset(stream, 0, (size_t)len);
    return;
  }

  self->base.source.read_frames(self->base.source.user,
                                (audio_stream_sample_t*)stream, num_frames);

  /* Zero any tail bytes that don't form a whole frame so the device never
   * plays uninitialised memory. */
  const u32 consumed = num_frames * frame_bytes;
  if ((u32)len > consumed)
    memset(stream + consumed, 0, (size_t)len - consumed);
}

static void sdl_stream_close(audio_stream_t* base)
{
  sdl_audio_stream_t* const self = (sdl_audio_stream_t*)base;
  if (self->device_id != 0)
  {
    SDL_CloseAudioDevice(self->device_id);
    self->device_id = 0;
  }
}

static bool sdl_stream_start(audio_stream_t* base, Error* error)
{
  sdl_audio_stream_t* const self = (sdl_audio_stream_t*)base;
  if (self->device_id == 0)
  {
    Error_set_string(error, "SDL audio device is not open.");
    return false;
  }
  SDL_PauseAudioDevice(self->device_id, 0);
  (void)error;
  return true;
}

static bool sdl_stream_stop(audio_stream_t* base, Error* error)
{
  sdl_audio_stream_t* const self = (sdl_audio_stream_t*)base;
  if (self->device_id == 0)
  {
    Error_set_string(error, "SDL audio device is not open.");
    return false;
  }
  SDL_PauseAudioDevice(self->device_id, 1);
  (void)error;
  return true;
}

static const audio_stream_vtable_t s_sdl_vtable = {
   .close = sdl_stream_close,
  .start = sdl_stream_start,
  .stop  = sdl_stream_stop, 
};

 audio_stream_t* audio_stream_create_sdl(u32 sample_rate, u32 channels,
                                        u32 output_latency_frames, bool output_latency_minimal, 
                                        const char* device_name,
                                         audio_stream_source_t source, bool auto_start,
                                        Error* error) 
{
  if (!sdl_audio_initialize(error))
    return NULL;

  if (!source.read_frames)
  {
    Error_set_string(error, "Audio source has no read_frames callback.");
    return NULL;
  }

  sdl_audio_stream_t* self = (sdl_audio_stream_t*)calloc(1, sizeof(*self));
  if (!self)
  {
    Error_set_string(error, "Out of memory allocating SDL audio stream.");
    return NULL;
  }

  self->base.vtbl        = &s_sdl_vtable;
  self->base.source      = source;
  self->base.sample_rate = sample_rate;
  self->base.channels    = channels;

  /* SDL2 wants a power-of-two sample count.  Round latency up so the device
   * never starves; output_latency_minimal asks for the smallest viable
   * buffer, which we approximate with 256 samples (~5 ms at 44.1 kHz). */
  u32 samples = output_latency_minimal ? 256u : output_latency_frames;
  if (samples < 64u)
    samples = 64u;
  /* Round up to next power of two. */
  u32 pow2 = 1u;
  while (pow2 < samples)
    pow2 <<= 1;

  SDL_AudioSpec desired;
  memset(&desired, 0, sizeof(desired));
  desired.freq     = (int)sample_rate;
  desired.format   = AUDIO_S16SYS;
  desired.channels = (Uint8)channels;
  desired.samples  = (Uint16)((pow2 > 0xFFFFu) ? 0xFFFFu : pow2);
  desired.callback = sdl_audio_callback;
  desired.userdata = self;

  const char* sdl_device = (device_name && device_name[0]) ? device_name : NULL;
  self->device_id = SDL_OpenAudioDevice(sdl_device, 0, &desired, &self->obtained_spec, 0);
  if (self->device_id == 0)
  {
    Error_set_string_fmt(error, "SDL_OpenAudioDevice() failed: %s", SDL_GetError());
    free(self);
    return NULL;
  }

  INFO_LOG("SDL audio: %d Hz, %u ch, %u-sample buffer (~%u ms latency)",
            self->obtained_spec.freq, (unsigned)self->obtained_spec.channels,
           (unsigned)self->obtained_spec.samples, 
           audio_stream_frames_to_ms((u32)self->obtained_spec.freq,
                                     (u32)self->obtained_spec.samples));

  if (auto_start)
    SDL_PauseAudioDevice(self->device_id, 0);

  return &self->base;
}
