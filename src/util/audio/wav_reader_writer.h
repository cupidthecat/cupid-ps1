/*
 *

 * Minimal RIFF/WAV reader + writer.  Writer always emits 16-bit PCM (the
 * SPU debug recording sink).  Reader supports 8/16/24/32-bit PCM and 32-bit
 * float, multi-channel.
 */
#ifndef CUPID_UTIL_WAV_READER_WRITER_H
#define CUPID_UTIL_WAV_READER_WRITER_H

#include "common/error.h"
#include "common/types.h"

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WAV_FORMAT_INVALID = 0,
  WAV_FORMAT_PCM     = 1,
  WAV_FORMAT_FLOAT   = 3,
} wav_format_t;

typedef struct {
  FILE*        file;
  s64          frames_start;
  wav_format_t format;
  u8           bits_per_sample;
  u8           num_channels;
  u8           bytes_per_frame;
  u32          sample_rate;
  u32          num_frames;
  u32          current_frame;
} wav_reader_t;

void wav_reader_init  (wav_reader_t* r);
void wav_reader_close (wav_reader_t* r);

bool wav_reader_open  (wav_reader_t* r, const char* path, Error* error);
bool wav_reader_is_open(const wav_reader_t* r);

u32  wav_reader_get_remaining_frames(const wav_reader_t* r);
bool wav_reader_seek_to_frame(wav_reader_t* r, u32 frame, Error* error);

bool wav_reader_read_frames(wav_reader_t* r, void* samples, u32 num_frames,
                            u32* out_read, Error* error);

typedef struct {
  wav_format_t format;
  u8           bits_per_sample;
  u8           num_channels;
  u8           bytes_per_frame;
  u32          sample_rate;
  u32          num_frames;
  const void*  sample_data;
} wav_memory_parse_t;

bool wav_reader_parse_memory(const void* data, size_t size,
                             wav_memory_parse_t* out, Error* error);

typedef struct {
  FILE* file;
  u32   sample_rate;
  u32   num_channels;
  u32   num_frames;       /* UINT32_MAX = previous write failed */
} wav_writer_t;

void wav_writer_init (wav_writer_t* w);

bool wav_writer_open (wav_writer_t* w, const char* path,
                      u32 sample_rate, u32 num_channels, Error* error);
bool wav_writer_close(wav_writer_t* w, Error* error);
bool wav_writer_is_open(const wav_writer_t* w);

bool wav_writer_write_frames(wav_writer_t* w, const s16* samples,
                             u32 num_frames, Error* error);

#ifdef __cplusplus
}
#endif

#endif
