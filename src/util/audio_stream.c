#include "audio_stream.h"

#include "common/error.h"
#include "common/types.h"

#include <stdlib.h>
#include <string.h>

/* Indices match audio_backend_t. */
static const char* const s_backend_names[AUDIO_BACKEND_COUNT] = {
  "Null",
  "SDL",
};

static const char* const s_backend_display_names[AUDIO_BACKEND_COUNT] = {
  "Null (No Output)",
  "SDL",
};

const char* audio_stream_get_backend_name(audio_backend_t backend)
{
  if ((unsigned)backend >= (unsigned)AUDIO_BACKEND_COUNT)
    return "";
  return s_backend_names[backend];
}

const char* audio_stream_get_backend_display_name(audio_backend_t backend)
{
  if ((unsigned)backend >= (unsigned)AUDIO_BACKEND_COUNT)
    return "";
  return s_backend_display_names[backend];
}

bool audio_stream_parse_backend_name(const char* str, audio_backend_t* out)
{
  if (!str)
    return false;
  for (int i = 0; i < (int)AUDIO_BACKEND_COUNT; i++)
  {
    if (strcmp(str, s_backend_names[i]) == 0)
    {
      if (out)
        *out = (audio_backend_t)i;
      return true;
    }
  }
  return false;
}

static const char* const s_stretch_mode_names[AUDIO_STRETCH_MODE_COUNT] = {
  "Off", "Resample", "TimeStretch",
};
static const char* const s_stretch_mode_display_names[AUDIO_STRETCH_MODE_COUNT] = {
  "Off", "Resample", "Time Stretch",
};

const char* audio_stream_get_stretch_mode_name(audio_stretch_mode_t mode)
{
  if ((unsigned)mode >= (unsigned)AUDIO_STRETCH_MODE_COUNT) return "";
  return s_stretch_mode_names[mode];
}

const char* audio_stream_get_stretch_mode_display_name(audio_stretch_mode_t mode)
{
  if ((unsigned)mode >= (unsigned)AUDIO_STRETCH_MODE_COUNT) return "";
  return s_stretch_mode_display_names[mode];
}

bool audio_stream_parse_stretch_mode_name(const char* str, audio_stretch_mode_t* out)
{
  if (!str) return false;
  for (int i = 0; i < (int)AUDIO_STRETCH_MODE_COUNT; i++) {
    if (strcmp(str, s_stretch_mode_names[i]) == 0) {
      if (out) *out = (audio_stretch_mode_t)i;
      return true;
    }
  }
  return false;
}

void audio_stream_get_driver_names(audio_backend_t backend,
                                   char*** out_names, char*** out_display_names,
                                   size_t* out_count)
{
  /* No backend currently exposes multiple drivers (Cubeb did; it's gone). */
  (void)backend;
  if (out_names)
    *out_names = NULL;
  if (out_display_names)
    *out_display_names = NULL;
  if (out_count)
    *out_count = 0;
}

void audio_stream_get_output_devices(audio_backend_t backend, const char* driver,
                                     u32 sample_rate,
                                     audio_stream_device_info_t** out_devices,
                                     size_t* out_count)
{
  /* SDL2's audio device list is queried directly through SDL_GetNumAudioDevices
   * inside the SDL backend; surface it through a single entry point so the
   * caller can pick a device without linking SDL on the call site. */
  (void)backend;
  (void)driver;
  (void)sample_rate;
  if (out_devices)
    *out_devices = NULL;
  if (out_count)
    *out_count = 0;
}

void audio_stream_free_string_list(char** array, size_t count)
{
  if (!array)
    return;
  for (size_t i = 0; i < count; i++)
    free(array[i]);
  free(array);
}

void audio_stream_free_device_list(audio_stream_device_info_t* devices, size_t count)
{
  if (!devices)
    return;
  for (size_t i = 0; i < count; i++)
  {
    free(devices[i].name);
    free(devices[i].display_name);
  }
  free(devices);
}

audio_stream_t* audio_stream_create(audio_backend_t backend, u32 sample_rate, u32 channels,
                                    u32 output_latency_frames, bool output_latency_minimal,
                                    const char* driver_name, const char* device_name,
                                    audio_stream_source_t source, bool auto_start,
                                    Error* error)
{
  (void)driver_name;

  switch (backend)
  {
    case AUDIO_BACKEND_SDL:
      return audio_stream_create_sdl(sample_rate, channels, output_latency_frames,
                                     output_latency_minimal, device_name, source,
                                     auto_start, error);

    case AUDIO_BACKEND_NULL:
    case AUDIO_BACKEND_COUNT:
    default:
      Error_set_string(error, "Unknown audio backend.");
      return NULL;
  }
}

void audio_stream_destroy(audio_stream_t* self)
{
  if (!self)
    return;
  if (self->vtbl && self->vtbl->close)
    self->vtbl->close(self);
  free(self);
}
