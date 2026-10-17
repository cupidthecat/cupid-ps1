/*
 * Audio output backends embed audio_stream_t and supply a vtable. Callers use
 * the inline audio_stream_* wrappers to start and stop playback.
 *
 * Producers supply a read_frames callback and a user pointer. Driver and
 * device names are NUL-terminated strings; string results use small_string_t
 * output parameters. Driver enumeration returns parallel name arrays, freed
 * with audio_stream_free_string_list. Device enumeration returns an array
 * freed with audio_stream_free_device_list.
 *
 * SDL is the playback backend; Null disables output. core_audio_stream.c
 * handles guest buffering, resampling and time stretching before SDL playback.
 */

#ifndef CUPID_UTIL_AUDIO_STREAM_H
#define CUPID_UTIL_AUDIO_STREAM_H

#include "common/small_string.h"
#include "common/types.h"

#include <stddef.h>

typedef struct Error Error;

/* Sample type used by every backend. */
typedef s16 audio_stream_sample_t;

typedef enum {
  AUDIO_BACKEND_NULL = 0,
  AUDIO_BACKEND_SDL  = 1,
  AUDIO_BACKEND_COUNT
} audio_backend_t;

#define AUDIO_BACKEND_DEFAULT AUDIO_BACKEND_SDL

typedef enum {
  AUDIO_STRETCH_MODE_OFF      = 0,
  AUDIO_STRETCH_MODE_RESAMPLE = 1,
  AUDIO_STRETCH_MODE_TIME     = 2,
  AUDIO_STRETCH_MODE_COUNT
} audio_stretch_mode_t;

typedef struct {
  /* Writes num_frames * channels samples into `samples`.  Always called on
   * the backend's audio callback thread; implementations must be lock-free
   * relative to the main thread. */
  void (*read_frames)(void* user, audio_stream_sample_t* samples, u32 num_frames);
  void* user;
} audio_stream_source_t;

typedef struct {
  char* name;          /* malloc'd, NUL-terminated */
  char* display_name;  /* malloc'd, NUL-terminated */
  u32   minimum_latency_frames;
} audio_stream_device_info_t;

typedef struct audio_stream_vtable audio_stream_vtable_t;
typedef struct audio_stream        audio_stream_t;

struct audio_stream_vtable {
  void (*close)(audio_stream_t* self);

  /* Resume / pause data requests from the source. */
  bool (*start)(audio_stream_t* self, Error* error);
  bool (*stop) (audio_stream_t* self, Error* error);
};

struct audio_stream {
  const audio_stream_vtable_t* vtbl;
  audio_stream_source_t        source;
  u32                          sample_rate;
  u32                          channels;
};

const char*     audio_stream_get_backend_name        (audio_backend_t backend);
const char*     audio_stream_get_backend_display_name(audio_backend_t backend);

/* Returns true on success and writes the matched backend to *out.  Returns
 * false (out untouched) when the name is unknown. */
bool            audio_stream_parse_backend_name(const char* str, audio_backend_t* out);

const char*     audio_stream_get_stretch_mode_name        (audio_stretch_mode_t mode);
const char*     audio_stream_get_stretch_mode_display_name(audio_stretch_mode_t mode);
bool            audio_stream_parse_stretch_mode_name(const char* str, audio_stretch_mode_t* out);

ALWAYS_INLINE u32 audio_stream_frames_to_ms(u32 sample_rate, u32 frames)
{
  return (frames * 1000u) / sample_rate;
}

/* Returns parallel (names, display_names) heap arrays.  Each slot is
 * malloc()'d; free with audio_stream_free_string_list on each array. */
void audio_stream_get_driver_names(audio_backend_t backend,
                                   char*** out_names, char*** out_display_names,
                                   size_t* out_count);

 /* Returns a heap array of audio_stream_device_info_t (malloc'd, length
 * out_count).  Free with audio_stream_free_device_list. */
void audio_stream_get_output_devices(audio_backend_t backend, const char* driver,
                                     u32 sample_rate,
                                     audio_stream_device_info_t** out_devices,
                                     size_t* out_count);

void audio_stream_free_string_list(char** array, size_t count);
void audio_stream_free_device_list(audio_stream_device_info_t* devices, size_t count);

/* Factory: backend-dispatched constructor.  Returns NULL on failure (with
 * *error populated when error != NULL).  Always release with
 * audio_stream_destroy. */
audio_stream_t* audio_stream_create(audio_backend_t backend, u32 sample_rate, u32 channels,
                                    u32 output_latency_frames, bool output_latency_minimal,
                                    const char* driver_name, const char* device_name,
                                    audio_stream_source_t source, bool auto_start,
                                    Error* error);

/* SDL-specific constructor (called by the factory; exposed for tests). */
audio_stream_t* audio_stream_create_sdl(u32 sample_rate, u32 channels,
                                        u32 output_latency_frames, bool output_latency_minimal,
                                        const char* device_name,
                                        audio_stream_source_t source, bool auto_start,
                                        Error* error);

/* Calls vtbl->close, then frees the containing struct.  NULL-safe. */
void audio_stream_destroy(audio_stream_t* self);

ALWAYS_INLINE bool audio_stream_start(audio_stream_t* s, Error* e)
{
  return s->vtbl->start(s, e);
}

ALWAYS_INLINE bool audio_stream_stop(audio_stream_t* s, Error* e)
{
  return s->vtbl->stop(s, e);
}

#endif /* CUPID_UTIL_AUDIO_STREAM_H */
