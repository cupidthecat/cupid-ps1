#include "util/wav_reader_writer.h"
#include "common/file_system.h"
#include "common/log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(WAV);

#pragma pack(push, 1)
typedef struct {
  u32 chunk_id;        /* RIFF */
  u32 chunk_size;
  u32 format;          /* WAVE */
} wav_file_header_t;

typedef struct {
  u32 chunk_id;
  u32 chunk_size;
} wav_chunk_header_t;

typedef struct {
  u32 chunk_id;        /* "fmt " */
  u32 chunk_size;
  u16 audio_format;    /* 1 = PCM, 3 = float */
  u16 num_channels;
  u32 sample_rate;
  u32 byte_rate;
  u16 block_align;
  u16 bits_per_sample;
} wav_fmt_chunk_t;

typedef struct {
  u32 chunk_id;        /* RIFF */
  u32 chunk_size;
  u32 format;          /* WAVE */
  /* fmt */
  u32 fmt_chunk_id;
  u32 fmt_chunk_size;
  u16 audio_format;
  u16 num_channels;
  u32 sample_rate;
  u32 byte_rate;
  u16 block_align;
  u16 bits_per_sample;
  /* data */
  u32 data_chunk_id;
  u32 data_chunk_size;
} wav_full_header_t;
#pragma pack(pop)

enum {
  RIFF_VALUE = 0x46464952u, /* 'RIFF' little-endian */
  FMT_VALUE  = 0x20746d66u, /* 'fmt ' */
  DATA_VALUE = 0x61746164u, /* 'data' */
  WAVE_VALUE = 0x45564157u, /* 'WAVE' */
};

void wav_reader_init(wav_reader_t* r)
{
  memset(r, 0, sizeof(*r));
  r->format = WAV_FORMAT_INVALID;
}

bool wav_reader_is_open(const wav_reader_t* r)
{
  return (r->file != NULL);
}

void wav_reader_close(wav_reader_t* r)
{
  if (r->file) fclose(r->file);
  wav_reader_init(r);
}

/* Walk the RIFF stream until we find a chunk with the requested tag.  When
 * out_chunk_size_extra > 0, after copying `chunk_size_in_struct` bytes we
 * skip the remainder of the chunk if `skip_extra_bytes` is set. */
static bool find_chunk_file(FILE* fp, void* chunk, size_t chunk_size_in_struct,
                            u32 tag, bool skip_extra_bytes, Error* error)
{
  for (;;) {
    wav_chunk_header_t header;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
      Error_set_errno_prefix(error, "fread() failed: ", errno);
      return false;
    }

    if (header.chunk_id != tag) {
      if (!fs_fseek64_e(fp, header.chunk_size, SEEK_CUR, error))
        return false;
      continue;
    }

    if (header.chunk_size < (chunk_size_in_struct - sizeof(header))) {
      Error_set_string_fmt(error, "Chunk too small (required %zu got %u)",
                           chunk_size_in_struct - sizeof(header), header.chunk_size);
      return false;
    }

    memcpy(chunk, &header, sizeof(header));
    if (chunk_size_in_struct > sizeof(header)) {
      const size_t tail = chunk_size_in_struct - sizeof(header);
      if (fread((u8*)chunk + sizeof(header), tail, 1, fp) != 1) {
        Error_set_errno_prefix(error, "fread() for data failed: ", errno);
        return false;
      }
    }

    const u32 extra = header.chunk_size - (u32)(chunk_size_in_struct - sizeof(header));
    if (skip_extra_bytes && extra && !fs_fseek64_e(fp, extra, SEEK_CUR, error))
      return false;

    return true;
  }
}

static bool validate_format(const wav_fmt_chunk_t* fmt, Error* error)
{
  if (fmt->audio_format != WAV_FORMAT_PCM && fmt->audio_format != WAV_FORMAT_FLOAT) {
    Error_set_string_fmt(error, "Unsupported audio format %u", fmt->audio_format);
    return false;
  }
  if (fmt->sample_rate == 0 || fmt->num_channels == 0 || fmt->num_channels > 8 ||
      (fmt->audio_format == WAV_FORMAT_PCM &&
       fmt->bits_per_sample != 8 && fmt->bits_per_sample != 16 &&
       fmt->bits_per_sample != 24 && fmt->bits_per_sample != 32) || 
      (fmt->audio_format == WAV_FORMAT_FLOAT && fmt->bits_per_sample != 32)) {
    Error_set_string_fmt(error,
        "Unsupported file format format=%u samplerate=%u channels=%u bits=%u",
        fmt->audio_format, fmt->sample_rate, fmt->num_channels, fmt->bits_per_sample);
    return false;
  }
  return true;
}

bool wav_reader_open(wav_reader_t* r, const char* path, Error* error)
{
  wav_reader_init(r);

  FILE* fp = fs_open_file(path, "rb", error);
  if (!fp) return false;

  wav_file_header_t fh;
  if (fread(&fh, sizeof(fh), 1, fp) != 1 ||
      fh.chunk_id != RIFF_VALUE || fh.format != WAVE_VALUE) {
    Error_set_string(error, "Invalid file header, must be RIFF/WAVE");
    fclose(fp);
    return false;
  }

  wav_fmt_chunk_t fmt;
  if (!find_chunk_file(fp, &fmt, sizeof(fmt), FMT_VALUE, true, error)) {
    Error_add_prefix(error, "Failed to get FMT chunk: ");
    fclose(fp);
    return false;
  }

  if (!validate_format(&fmt, error)) {
    fclose(fp);
    return false;
  }

  wav_chunk_header_t data;
  if (!find_chunk_file(fp, &data, sizeof(data), DATA_VALUE, false, error)) {
    Error_add_prefix(error, "Failed to get DATA chunk: ");
    fclose(fp);
    return false;
  }

  const u8  bytes_per_frame = (u8)((fmt.bits_per_sample / 8) * fmt.num_channels);
  const u32 num_frames      = (bytes_per_frame > 0) ? (data.chunk_size / bytes_per_frame) : 0;
  if (num_frames == 0) {
    Error_set_string(error, "File has no frames");
    fclose(fp);
    return false;
  }

  r->file            = fp;
  r->frames_start    = fs_ftell64(fp);
  r->format          = (wav_format_t)fmt.audio_format;
  r->bits_per_sample = (u8)fmt.bits_per_sample;
  r->num_channels    = (u8)fmt.num_channels;
  r->bytes_per_frame = bytes_per_frame;
  r->sample_rate     = fmt.sample_rate;
  r->num_frames      = num_frames;
  r->current_frame   = 0;
  return true;
}

u32 wav_reader_get_remaining_frames(const wav_reader_t* r)
{
  return r->num_frames - r->current_frame;
}

bool wav_reader_seek_to_frame(wav_reader_t* r, u32 frame, Error* error)
{
  if (frame > r->num_frames) {
    Error_set_string_fmt(error, "Frame %u out of range (max %u)", frame, r->num_frames);
    return false;
  }
  const s64 offset = r->frames_start + (s64)frame * r->bytes_per_frame;
  if (!fs_fseek64_e(r->file, offset, SEEK_SET, error))
    return false;
  r->current_frame = frame;
  return true;
}

bool wav_reader_read_frames(wav_reader_t* r, void* samples, u32 num_frames,
                            u32* out_read, Error* error)
{
  const u32 remaining = r->num_frames - r->current_frame;
  if (remaining == 0) {
    *out_read = 0;
    return true;
  }
  const u32 want = (num_frames < remaining) ? num_frames : remaining;
  const u32 got  = (u32)fread(samples, r->bytes_per_frame, want, r->file);
  if (got == 0 && ferror(r->file)) {
    Error_set_errno_prefix(error, "fread() failed: ", errno);
    *out_read = 0;
    return false;
  }
  r->current_frame += got;
  *out_read = got;
  return true;
}

/* Memory parser: walk a span looking for tag, copy chunk in. */
static bool find_chunk_mem(const u8* base, size_t size, void* chunk,
                           size_t chunk_size_in_struct, u32 tag,
                           const u8** out_data, u32* out_data_len, Error* error)
{
  size_t off = 0;
  for (;;) {
    if (off + sizeof(wav_chunk_header_t) > size) {
      Error_set_string(error, "Unexpected end of file while searching for chunk");
      return false;
    }
    wav_chunk_header_t hdr;
    memcpy(&hdr, base + off, sizeof(hdr));

    if (hdr.chunk_id != tag) {
      off += sizeof(hdr) + hdr.chunk_size;
      continue;
    }
    if (hdr.chunk_size < (chunk_size_in_struct - sizeof(hdr))) {
      Error_set_string_fmt(error, "Chunk too small (required %zu got %u)",
                           chunk_size_in_struct - sizeof(hdr), hdr.chunk_size);
      return false;
    }
    if (hdr.chunk_size > (size - off - sizeof(hdr))) {
      Error_set_string(error, "Chunk size exceeds file size");
      return false;
    }
    if (chunk_size_in_struct > sizeof(hdr) && (off + chunk_size_in_struct) > size) {
      Error_set_string(error, "Unexpected end of file while reading chunk header");
      return false;
    }

    memcpy(chunk, base + off, chunk_size_in_struct);
    *out_data     = base + off + sizeof(hdr);
    *out_data_len = hdr.chunk_size;
    return true;
  }
}

bool wav_reader_parse_memory(const void* data, size_t size,
                             wav_memory_parse_t* out, Error* error)
{
  wav_file_header_t fh;
  if (size < sizeof(fh)) {
    Error_set_string(error, "Data too small to be a valid WAV file");
    return false;
  }
  memcpy(&fh, data, sizeof(fh));
  if (fh.chunk_id != RIFF_VALUE || fh.format != WAVE_VALUE) {
    Error_set_string(error, "Invalid file header, must be RIFF/WAVE");
    return false;
  }

  const u8* whole = (const u8*)data + sizeof(fh);
  const size_t wholelen = size - sizeof(fh);

  wav_fmt_chunk_t fmt;
  const u8* fmt_data = NULL;  u32 fmt_data_len = 0;
  if (!find_chunk_mem(whole, wholelen, &fmt, sizeof(fmt), FMT_VALUE,
                      &fmt_data, &fmt_data_len, error)) {
    Error_add_prefix(error, "Failed to get FMT chunk: ");
    return false;
  }
  (void)fmt_data; (void)fmt_data_len;

  if (!validate_format(&fmt, error))
    return false;

  wav_chunk_header_t dchunk;
  const u8* sample_data = NULL;  u32 sample_data_len = 0;
  if (!find_chunk_mem(whole, wholelen, &dchunk, sizeof(dchunk), DATA_VALUE,
                      &sample_data, &sample_data_len, error)) {
    Error_add_prefix(error, "Failed to get DATA chunk: ");
    return false;
  }

  const u8  bytes_per_frame = (u8)((fmt.bits_per_sample / 8) * fmt.num_channels);
  const u32 num_frames      = (bytes_per_frame > 0) ? (sample_data_len / bytes_per_frame) : 0;
  if (num_frames == 0) {
    Error_set_string(error, "File has no frames");
    return false;
  }

  out->format          = (wav_format_t)fmt.audio_format;
  out->bits_per_sample = (u8)fmt.bits_per_sample;
  out->num_channels    = (u8)fmt.num_channels;
  out->bytes_per_frame = bytes_per_frame;
  out->sample_rate     = fmt.sample_rate;
  out->num_frames      = num_frames;
  out->sample_data     = sample_data;
  return true;
}

void wav_writer_init(wav_writer_t* w)
{
  memset(w, 0, sizeof(*w));
}

bool wav_writer_is_open(const wav_writer_t* w)
{
  return (w->file != NULL);
}

static bool wav_writer_emit_header(wav_writer_t* w, Error* error)
{
  const u32 data_size = (u32)sizeof(s16) * w->num_channels * w->num_frames;

  wav_full_header_t h;
  memset(&h, 0, sizeof(h));
  h.chunk_id        = RIFF_VALUE;
  h.chunk_size      = (u32)sizeof(h) - 8u + data_size;
  h.format          = WAVE_VALUE;
  h.fmt_chunk_id    = FMT_VALUE;
  h.fmt_chunk_size  = 16u;            /* fmt body size: 16 bytes for PCM */
  h.audio_format    = 1u;             /* PCM */
  h.num_channels    = (u16)w->num_channels;
  h.sample_rate     = w->sample_rate;
  h.byte_rate       = w->sample_rate * w->num_channels * (u32)sizeof(s16);
  h.block_align     = (u16)(w->num_channels * sizeof(s16));
  h.bits_per_sample = 16u;
  h.data_chunk_id   = DATA_VALUE;
  h.data_chunk_size = data_size;

  if (fwrite(&h, sizeof(h), 1, w->file) != 1) {
    Error_set_errno_prefix(error, "fwrite() failed: ", errno);
    return false;
  }
  return true;
}

bool wav_writer_open(wav_writer_t* w, const char* path,
                     u32 sample_rate, u32 num_channels, Error* error)
{
  if (wav_writer_is_open(w))
    wav_writer_close(w, NULL);

  w->file = fs_open_file(path, "wb", error);
  if (!w->file) return false;

  w->sample_rate  = sample_rate;
  w->num_channels = num_channels;
  w->num_frames   = 0;

  if (!wav_writer_emit_header(w, error)) {
    fclose(w->file);
    memset(w, 0, sizeof(*w));
    return false;
  }
  return true;
}

bool wav_writer_close(wav_writer_t* w, Error* error)
{
  if (!wav_writer_is_open(w))
    return true;

  bool ok = (w->num_frames != UINT32_MAX);
  if (ok) {
    ok = fs_fseek64_e(w->file, 0, SEEK_SET, error) && wav_writer_emit_header(w, error);
    if (fclose(w->file) != 0) {
      Error_set_errno_prefix(error, "fclose() failed: ", errno);
      ok = false;
    }
  } else if (w->file) {
    fclose(w->file);
  }
  memset(w, 0, sizeof(*w));
  return ok;
}

bool wav_writer_write_frames(wav_writer_t* w, const s16* samples,
                             u32 num_frames, Error* error)
{
  if (w->num_frames == UINT32_MAX) {
    Error_set_string(error, "Previous write failed.");
    return false;
  }
  const size_t n = fwrite(samples, sizeof(s16) * w->num_channels, num_frames, w->file);
  if ((u32)n != num_frames) {
    Error_set_errno_prefix(error, "fwrite() failed: ", errno);
    w->num_frames = UINT32_MAX;
    return false;
  }
  w->num_frames += (u32)n;
  return true;
}
