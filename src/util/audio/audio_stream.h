/*
 * The C++ AudioStream class hierarchy is mapped to:
 *
 *   typedef struct audio_stream audio_stream_t;
 *   struct audio_stream { const audio_stream_vtable_t* vtbl; ... };
 *
 * Concrete backends (SDL today) embed audio_stream_t as their first member and
 * supply a vtable.  Callers go through the inline wrappers (audio_stream_start,
 * audio_stream_stop) and never touch vtbl directly.
 *
 * AudioStreamSource (the abstract producer) is collapsed to an
 * audio_stream_source_t = (read_frames fn, user) pair.
 *
 * Type-mapping rules used here:
 *   std::string_view driver_name / device_name -> const char* (NUL-terminated)
 *   std::string                                 -> small_string_t* out-param
 *   std::vector<std::pair<std::string,std::string>>
 *                                               -> two parallel char** arrays
 *                                                  + count, freed with
 *                                                  audio_stream_free_string_list
 *   std::vector<DeviceInfo>                     -> heap audio_stream_device_info_t*
 *                                                  + count, freed with
 *                                                  audio_stream_free_device_list
 *
 * Cubeb / CoreAudio / Win32 / Android backends are dropped - SDL is the only
 * non-Null backend.  StretchMode is preserved as an enum but not consumed
 * here because SoundTouch is not (yet) ported; the SDL backend pass-through
 * resampling already happens inside SDL itself.
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
