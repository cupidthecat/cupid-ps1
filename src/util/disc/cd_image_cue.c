/*
 * Concrete CDImage backend for .bin/.cue. Only BINARY-format track files are
 * supported in this port; WAVE and ECM are intentionally skipped (no AS code
 * paths) and CHD/PBP/MDS/CCD/M3U/PPF are out of scope per PLAN.md.
 *
 * Layout: cd_image_cue_t embeds cd_image_t as its first member, so any
 * cd_image_t* returned from cd_image_open_cue may be cast back. The vtable's
 * destroy hook frees the per-track FILE handles; the base destroy frees the
 * struct itself.
 */

#include "cd_image.h"
#include "cue_parser.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <libchdr/cdrom.h>  /* ecc_generate, edc_compute, edc_set */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_CHANNEL(CDImage);

enum {
  ECM_TYPE_RAW         = 0,
  ECM_TYPE_MODE1       = 1,
  ECM_TYPE_MODE2_FORM1 = 2,
  ECM_TYPE_MODE2_FORM2 = 3,
  ECM_TYPE_COUNT       = 4,
};

static const u32 s_ecm_sector_sizes[ECM_TYPE_COUNT] = {
  0x930u, 0x803u, 0x804u, 0x918u,
};
static const u32 s_ecm_chunk_sizes[ECM_TYPE_COUNT] = {
  0u, 2352u, 2336u, 2336u,
};

typedef struct {
  u32 disc_offset;   /* sort key */
  u32 file_offset;
  u32 chunk_size;
  u32 type;
} ecm_sector_entry_t;

typedef struct {
  ecm_sector_entry_t* entries;
  size_t              entry_count;
  size_t              entry_capacity;
  u8*                 chunk_buffer;
  size_t              chunk_buffer_size;
  size_t              chunk_buffer_capacity;
  u32                 chunk_start;
  u32                 lba_count;
} ecm_data_t;

static int ecm_entry_compare(const void* a, const void* b)
{
  const u32 ka = *(const u32*)a;
  const ecm_sector_entry_t* eb = (const ecm_sector_entry_t*)b;
  if (ka < eb->disc_offset) return -1;
  if (ka > eb->disc_offset) return  1;
  return 0;
}

static void ecm_data_destroy(ecm_data_t* ecm)
{
  if (!ecm) return;
  free(ecm->entries);
  free(ecm->chunk_buffer);
  free(ecm);
}

static bool ecm_data_push_entry(ecm_data_t* ecm, u32 disc_offset, u32 file_offset,
                                u32 chunk_size, u32 type)
{
  if (ecm->entry_count + 1u > ecm->entry_capacity) {
    const size_t newcap = ecm->entry_capacity ? ecm->entry_capacity * 2u : 1024u;
    ecm_sector_entry_t* p =
      (ecm_sector_entry_t*)realloc(ecm->entries, newcap * sizeof(*p));
    if (!p) return false;
    ecm->entries        = p;
    ecm->entry_capacity = newcap;
  }
  ecm_sector_entry_t* e = &ecm->entries[ecm->entry_count++];
  e->disc_offset = disc_offset;
  e->file_offset = file_offset;
  e->chunk_size  = chunk_size;
  e->type        = type;
  return true;
}

static bool ecm_data_build_sector_map(ecm_data_t* ecm, FILE* fp, Error* error)
{
  const s64 file_size = fs_fsize64(fp, error);
  if (file_size <= 0) return false;

  if (fseek(fp, 0, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "fseek to ECM header failed: ", errno);
    return false;
  }
  char header[4];
  if (fread(header, sizeof(header), 1, fp) != 1u ||
      header[0] != 'E' || header[1] != 'C' || header[2] != 'M' || header[3] != 0) {
    Error_set_string(error, "Invalid ECM header");
    return false;
  }

  u32 file_offset = 4u;
  u32 disc_offset = 0u;

  for (;;) {
    int b = fgetc(fp);
    if (b == EOF) {
      Error_set_string_fmt(error, "Unexpected EOF after %zu chunks", ecm->entry_count);
      return false;
    }
    file_offset++;
    const u32 type  = (u32)(b & 0x03);
    u32 count       = ((u32)b >> 2) & 0x1Fu;
    u32 shift       = 5u;
    while (b & 0x80) {
      b = fgetc(fp);
      if (b == EOF) {
        Error_set_string_fmt(error, "Unexpected EOF in ECM count after %zu chunks", ecm->entry_count);
        return false;
      }
      count |= ((u32)b & 0x7Fu) << shift;
      shift += 7u;
      file_offset++;
    }
    if (count == 0xFFFFFFFFu) break;
    count++;
    if (count >= 0x80000000u) {
      Error_set_string_fmt(error, "Corrupted ECM header after %zu chunks", ecm->entry_count);
      return false;
    }

    if (type == ECM_TYPE_RAW) {
      while (count > 0u) {
        const u32 piece = (count < 2352u) ? count : 2352u;
        if (!ecm_data_push_entry(ecm, disc_offset, file_offset, piece, type)) {
          Error_set_string(error, "out of memory in ECM map build");
          return false;
        }
        disc_offset += piece;
        file_offset += piece;
        count       -= piece;
        if ((s64)file_offset > file_size) {
          Error_set_string_fmt(error, "ECM raw run out of bounds at chunk %zu", ecm->entry_count);
          return false;
        }
      }
    } else {
      const u32 fsize = s_ecm_sector_sizes[type];
      const u32 csize = s_ecm_chunk_sizes[type];
      for (u32 i = 0u; i < count; i++) {
        if (!ecm_data_push_entry(ecm, disc_offset, file_offset, csize, type)) {
          Error_set_string(error, "out of memory in ECM map build");
          return false;
        }
        disc_offset += csize;
        file_offset += fsize;
        if ((s64)file_offset > file_size) {
          Error_set_string_fmt(error, "ECM chunk run out of bounds at chunk %zu", ecm->entry_count);
          return false;
        }
      }
    }

    if (fseek(fp, (long)file_offset, SEEK_SET) != 0) {
      Error_set_errno_prefix(error, "fseek inside ECM body failed: ", errno);
      return false;
    }
  }

  ecm->lba_count = disc_offset / 2352u;
  if ((disc_offset % 2352u) != 0u)
    WARNING_LOG("ECM image misaligned: disc_offset %u not a multiple of 2352", disc_offset);
  if (ecm->entry_count == 0u || ecm->lba_count == 0u) {
    Error_set_string(error, "No sectors found in ECM image");
    return false;
  }
  return true;
}

static ecm_data_t* ecm_data_create(FILE* fp, Error* error)
{
  ecm_data_t* ecm = (ecm_data_t*)calloc(1, sizeof(*ecm));
  if (!ecm) {
    Error_set_string(error, "out of memory for ECM data");
    return NULL;
  }
  if (!ecm_data_build_sector_map(ecm, fp, error)) {
    ecm_data_destroy(ecm);
    return NULL;
  }
  return ecm;
}

static bool ecm_data_read_chunks(ecm_data_t* ecm, FILE* fp, u32 disc_offset, u32 size, Error* error)
{
  if (ecm->entry_count == 0u) return false;
  size_t cur = 0;
  /* binary search: largest i such that entries[i].disc_offset <= disc_offset */
  {
    size_t lo = 0, hi = ecm->entry_count;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2u;
      if (ecm->entries[mid].disc_offset <= disc_offset) lo = mid + 1u;
      else                                              hi = mid;
    }
    cur = (lo == 0u) ? 0u : (lo - 1u);
  }

  ecm->chunk_start       = ecm->entries[cur].disc_offset;
  ecm->chunk_buffer_size = 0u;

  /* If the requested disc_offset is past the entry start, extend the read so
   * the chunk buffer covers up to disc_offset+size from chunk_start. */
  if (ecm->chunk_start < disc_offset)
    size += (disc_offset - ecm->chunk_start);

  u32 total_bytes_read = 0u;
  while (total_bytes_read < size) {
    if (cur >= ecm->entry_count) {
      Error_set_string(error, "ECM read past end of sector map");
      return false;
    }
    const ecm_sector_entry_t* e = &ecm->entries[cur];
    if (fseek(fp, (long)e->file_offset, SEEK_SET) != 0) {
      Error_set_errno_prefix(error, "fseek inside ECM data failed: ", errno);
      return false;
    }

    const u32 chunk_size  = e->chunk_size;
    const size_t needed   = ecm->chunk_buffer_size + chunk_size;
    if (needed > ecm->chunk_buffer_capacity) {
      const size_t newcap = (needed * 3u) / 2u;
      u8* p = (u8*)realloc(ecm->chunk_buffer, newcap);
      if (!p) { Error_set_string(error, "out of memory in ECM chunk buffer"); return false; }
      ecm->chunk_buffer          = p;
      ecm->chunk_buffer_capacity = newcap;
    }
    const size_t chunk_out_off = ecm->chunk_buffer_size;
    ecm->chunk_buffer_size += chunk_size;

    if (e->type == ECM_TYPE_RAW) {
      if (fread(&ecm->chunk_buffer[chunk_out_off], chunk_size, 1, fp) != 1u) {
        Error_set_errno_prefix(error, "ECM raw fread failed: ", errno);
        return false;
      }
      total_bytes_read += chunk_size;
    } else {
      u8 sector[2352];
      memset(sector, 0, sizeof(sector));
      memset(sector + 1, 0xFF, 10);
      u32 skip = 0u;
      switch (e->type) {
        case ECM_TYPE_MODE1: {
          sector[0x0F] = 0x01;
          if (fread(sector + 0x00C, 0x003, 1, fp) != 1u ||
              fread(sector + 0x010, 0x800, 1, fp) != 1u) {
            Error_set_errno_prefix(error, "ECM mode1 fread failed: ", errno);
            return false;
          }
          edc_set(&sector[2064], edc_compute(sector, 2064));
          ecc_generate(sector);
          skip = 0u;
        } break;
        case ECM_TYPE_MODE2_FORM1: {
          sector[0x0F] = 0x02;
          if (fread(sector + 0x014, 0x804, 1, fp) != 1u) {
            Error_set_errno_prefix(error, "ECM mode2form1 fread failed: ", errno);
            return false;
          }
          sector[0x10] = sector[0x14];
          sector[0x11] = sector[0x15];
          sector[0x12] = sector[0x16];
          sector[0x13] = sector[0x17];
          edc_set(&sector[2072], edc_compute(&sector[16], 2056));
          ecc_generate(sector);
          skip = 0x10u;
        } break;
        case ECM_TYPE_MODE2_FORM2: {
          sector[0x0F] = 0x02;
          if (fread(sector + 0x014, 0x918, 1, fp) != 1u) {
            Error_set_errno_prefix(error, "ECM mode2form2 fread failed: ", errno);
            return false;
          }
          sector[0x10] = sector[0x14];
          sector[0x11] = sector[0x15];
          sector[0x12] = sector[0x16];
          sector[0x13] = sector[0x17];
          edc_set(&sector[2348], edc_compute(&sector[16], 2332));
          /* Mode2Form2 has no ECC. */
          skip = 0x10u;
        } break;
        default:
          Error_set_string(error, "unknown ECM sector type");
          return false;
      }
      memcpy(&ecm->chunk_buffer[chunk_out_off], sector + skip, chunk_size);
      total_bytes_read += chunk_size;
    }
    cur++;
  }
  return true;
}

static u64 ecm_data_size(const ecm_data_t* ecm)
{
  return (u64)ecm->lba_count * 2352u;
}

static bool ecm_data_read(ecm_data_t* ecm, FILE* fp, void* buffer, u64 offset, u32 size, Error* error)
{
  const u64 file_end = offset + (u64)size;
  if (offset < ecm->chunk_start ||
      file_end > ((u64)ecm->chunk_start + ecm->chunk_buffer_size)) {
    if (!ecm_data_read_chunks(ecm, fp, (u32)offset, 2352u, error))
      return false;
  }
  if (offset < ecm->chunk_start ||
      file_end > ((u64)ecm->chunk_start + ecm->chunk_buffer_size)) {
    Error_set_string(error, "ECM read out of buffered range");
    return false;
  }
  const size_t chunk_offset = (size_t)(offset - ecm->chunk_start);
  memcpy(buffer, &ecm->chunk_buffer[chunk_offset], size);
  return true;
}

typedef struct {
  char*        filename;        /* heap, what the cue called this file */
  FILE*        fp;              /* heap, owned */
  u64          file_position;   /* cached so we can skip redundant seeks (BIN only) */
  ecm_data_t*  ecm;             /* NULL for plain BIN; non-NULL for .ecm */
} cue_track_file_t;

static bool cue_track_file_open(cue_track_file_t* tf, const char* filename, const char* full_path, Error* error)
{
  tf->filename = NULL;
  tf->fp = NULL;
  tf->file_position = 0;
  tf->ecm = NULL;

  FILE* fp = fs_open_shared_file(full_path, "rb", FS_FILE_SHARE_DENY_WRITE, error);
  if (!fp) {
    const char* base_data; u32 base_len;
    path_get_file_name_cstr(full_path, &base_data, &base_len);
    Error_add_prefix_fmt(error, "Failed to open '%.*s': ", (int)base_len, base_data);
    return false;
  }

  const char* ext = NULL; u32 ext_len = 0u;
  path_get_extension_cstr(full_path, &ext, &ext_len);
  if (ext_len == 3u && (ext[0] == 'e' || ext[0] == 'E') &&
                       (ext[1] == 'c' || ext[1] == 'C') &&
                       (ext[2] == 'm' || ext[2] == 'M')) {
    tf->ecm = ecm_data_create(fp, error);
    if (!tf->ecm) {
      fclose(fp);
      return false;
    }
  }

  tf->filename = (char*)malloc(strlen(filename) + 1u);
  if (!tf->filename) Panic("Memory allocation failed.");
  strcpy(tf->filename, filename);
  tf->fp = fp;
  return true;
}

static void cue_track_file_close(cue_track_file_t* tf)
{
  if (tf->ecm) { ecm_data_destroy(tf->ecm); tf->ecm = NULL; }
  if (tf->fp) { fclose(tf->fp); tf->fp = NULL; }
  free(tf->filename); tf->filename = NULL;
}

static u64 cue_track_file_size(cue_track_file_t* tf)
{
  if (tf->ecm) return ecm_data_size(tf->ecm);
  const s64 sz = fs_fsize64(tf->fp, NULL);
  return (sz < 0) ? 0u : (u64)sz;
}

static bool cue_track_file_read(cue_track_file_t* tf, void* buffer, u64 offset, u32 size, Error* error)
{
  if (tf->ecm) {
    return ecm_data_read(tf->ecm, tf->fp, buffer, offset, size, error);
  }
  if (tf->file_position != offset) {
    if (!fs_fseek64_e(tf->fp, (s64)offset, SEEK_SET, error)) {
      tf->file_position = UINT64_MAX;
      return false;
    }
    tf->file_position = offset;
  }
  if (fread(buffer, size, 1, tf->fp) != 1u) {
    Error_set_errno_prefix(error, "fread() failed: ", errno);
    tf->file_position = UINT64_MAX;
    return false;
  }
  tf->file_position += size;
  return true;
}

typedef struct cd_image_cue {
  cd_image_t base;     /* MUST be first */

  cue_track_file_t* files;
  u32               file_count;
  u32               file_capacity;
} cd_image_cue_t;

static cue_track_file_t* cue_image_push_file(cd_image_cue_t* img)
{
  if (img->file_count == img->file_capacity) {
    const u32 new_cap = (img->file_capacity == 0) ? 4u : (img->file_capacity * 2u);
    cue_track_file_t* p = (cue_track_file_t*)realloc(img->files, new_cap * sizeof(cue_track_file_t));
    if (!p) Panic("Memory allocation failed.");
    img->files = p;
    img->file_capacity = new_cap;
  }
  cue_track_file_t* slot = &img->files[img->file_count++];
  memset(slot, 0, sizeof(*slot));
  return slot;
}

static bool cue_read_sector_from_index(cd_image_t* self, void* buffer,
                                       const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_cue_t* img = (cd_image_cue_t*)self;
  DebugAssert(index->file_index < img->file_count);

  cue_track_file_t* tf = &img->files[index->file_index];
  const u64 file_position = index->file_offset + (u64)lba_in_index * (u64)index->file_sector_size;
  Error err = ERROR_INIT;
  if (!cue_track_file_read(tf, buffer, file_position, index->file_sector_size, &err)) {
    ERROR_LOG("Failed to read LBA %u: %s", lba_in_index, Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }
  return true;
}

static s64 cue_get_size_on_disk(const cd_image_t* self)
{
  const cd_image_cue_t* img = (const cd_image_cue_t*)self;
  /* Doesn't include the cue itself, but those are tiny. */
  u64 size = 0;
  for (u32 k = 0; k < img->file_count; k++) {
    const s64 fsz = fs_fsize64(img->files[k].fp, NULL);
    if (fsz > 0) size += (u64)fsz;
  }
  return (s64)size;
}

static void cue_destroy(cd_image_t* self)
{
  cd_image_cue_t* img = (cd_image_cue_t*)self;
  for (u32 k = 0; k < img->file_count; k++)
    cue_track_file_close(&img->files[k]);
  free(img->files);
  img->files = NULL;
  img->file_count = 0;
  img->file_capacity = 0;
}

static const cd_image_vtable_t s_cue_vtable = {
  .read_sector_from_index = cue_read_sector_from_index,
  .read_subchannel_q      = NULL,                        /* default: synthesised */
  .has_subchannel_data    = NULL,                        /* default: false */
  .precache               = NULL,                        /* default: unsupported */
  .is_precached           = NULL,                        /* default: false */
   .get_size_on_disk       = cue_get_size_on_disk,
  .destroy                = cue_destroy, 
};

static u32 find_or_open_track_file(cd_image_cue_t* img, const char* track_filename,
                                   const char* cue_path, Error* error)
{
  for (u32 k = 0; k < img->file_count; k++) {
    if (strcmp(img->files[k].filename, track_filename) == 0)
      return k;
  }

  /* Resolve relative to cue path. */
  small_string_t full_path;
  small_string_init(&full_path);

  if (path_is_absolute_cstr(track_filename)) {
    small_string_assign_cstr(&full_path, track_filename);
  } else {
    path_build_relative_cstr(&full_path, cue_path, track_filename);
  }

  cue_track_file_t tmp;
  if (!cue_track_file_open(&tmp, track_filename, small_string_c_str(&full_path), error)) {
    /* If this is the first file referenced and it failed, fall back to a .bin
     * mis-named cuesheets). */
    if (img->file_count == 0) {
      Error_clear(error);
      small_string_t alt;
      small_string_init(&alt);
      path_change_extension_cstr(&alt, cue_path, "bin");
      if (cue_track_file_open(&tmp, track_filename, small_string_c_str(&alt), error)) {
        WARNING_LOG("Cue references invalid file '%s', using '%s' instead",
                    track_filename, small_string_c_str(&alt));
        small_string_destroy(&alt);
        small_string_destroy(&full_path);
        cue_track_file_t* slot = cue_image_push_file(img);
        *slot = tmp;
        return img->file_count - 1u;
      }
      small_string_destroy(&alt);
    }
    small_string_destroy(&full_path);
    return UINT32_MAX;
  }

  small_string_destroy(&full_path);
  cue_track_file_t* slot = cue_image_push_file(img);
  *slot = tmp;
  return img->file_count - 1u;
}

static bool open_and_parse_cue(cd_image_cue_t* img, const char* path, Error* error)
{
  FILE* fp = fs_open_shared_file(path, "rb", FS_FILE_SHARE_DENY_WRITE, error);
  if (!fp) {
    const char* base_data; u32 base_len;
    path_get_file_name_cstr(path, &base_data, &base_len);
    Error_add_prefix_fmt(error, "Failed to open cuesheet '%.*s': ", (int)base_len, base_data);
    return false;
  }

  cue_parser_file_t parser;
  cue_parser_file_init(&parser);
  const bool parse_ok = cue_parser_file_parse_fp(&parser, fp, error);
  fclose(fp);
  if (!parse_ok) {
    cue_parser_file_destroy(&parser);
    return false;
  }

  /* m_filename = path */
  img->base.filename = (char*)malloc(strlen(path) + 1u);
  if (!img->base.filename) Panic("Memory allocation failed.");
  strcpy(img->base.filename, path);

  cd_image_lba_t disc_lba = 0;

  for (u32 track_num = 1; track_num <= CUE_PARSER_MAX_TRACK_NUMBER; track_num++) {
    const cue_parser_track_t* track = cue_parser_file_get_track(&parser, track_num);
    if (!track)
      break;

    const cd_image_lba_t track_start = cd_image_position_to_lba(track->start);
    const u32 file_idx = find_or_open_track_file(img, track->file, path, error);
    if (file_idx == UINT32_MAX) {
      cue_parser_file_destroy(&parser);
      return false;
    }

    /* WAVE files would need a sample-rate-aware reader; not supported here. */
    if (track->file_format != CUE_PARSER_FILE_FORMAT_BINARY) {
      Error_set_string_fmt(error,
        "Track %u in '%s' uses WAVE format which is not supported in cupid-ps1.", track_num, path);
      cue_parser_file_destroy(&parser);
      return false;
    }

    /* Sector size depends on the track mode. */
    const cd_image_track_mode_t mode = track->mode;
    const u32 track_sector_size = cd_image_get_bytes_per_sector(mode);

    /* Pre-compute the subchannel Q control bits for the whole track. */
    cd_image_subq_control_t control;
    control.bits = 0;
    cd_image_subq_control_set_data(&control, mode != CD_IMAGE_TRACK_MODE_AUDIO);
    cd_image_subq_control_set_audio_preemphasis(&control,
      cue_parser_track_has_flag(track, CUE_PARSER_TRACK_FLAG_PRE_EMPHASIS));
    cd_image_subq_control_set_digital_copy_permitted(&control,
      cue_parser_track_has_flag(track, CUE_PARSER_TRACK_FLAG_COPY_PERMITTED));
    cd_image_subq_control_set_four_channel_audio(&control,
      cue_parser_track_has_flag(track, CUE_PARSER_TRACK_FLAG_FOUR_CHANNEL_AUDIO));

    /* Track length: from cue if known, otherwise compute from file size. */
    cd_image_lba_t track_length;
    if (!track->has_length) {
      u64 file_size = cue_track_file_size(&img->files[file_idx]);
      file_size /= track_sector_size;
      if ((u64)track_start >= file_size) {
        ERROR_LOG("Failed to open track %u in '%s': start out of range (%u vs %llu)",
                  track_num, path, track_start, (unsigned long long)file_size);
        Error_set_string_fmt(error,
          "Failed to open track %u in '%s': track start is out of range (%u vs %llu)",
          track_num, path, track_start, (unsigned long long)file_size);
        cue_parser_file_destroy(&parser);
        return false;
      }
      track_length = (cd_image_lba_t)(file_size - (u64)track_start);
    } else {
      track_length = cd_image_position_to_lba(track->length);
    }

    const cd_image_position_t* index0 = cue_parser_track_get_index(track, 0);
    cd_image_lba_t pregap_frames;

    if (index0) {
      /* Index 1 is always present after a successful parse. */
      const cd_image_position_t* index1 = cue_parser_track_get_index(track, 1);
      pregap_frames = cd_image_position_to_lba(*index1) - cd_image_position_to_lba(*index0);

      cd_image_index_t* pregap = cd_image_push_index(&img->base);
      pregap->start_lba_on_disc  = disc_lba;
      pregap->start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
      pregap->length             = pregap_frames;
      pregap->track_number       = track_num;
      pregap->index_number       = 0;
      pregap->mode               = mode;
      pregap->submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
      pregap->control            = control;
      pregap->is_pregap          = true;
      pregap->file_index         = file_idx;
      pregap->file_offset        = (u64)((s64)track_start - (s64)pregap_frames) * (u64)track_sector_size;
      pregap->file_sector_size   = track_sector_size;

      disc_lba += pregap->length;
    } else {
      /* Two seconds pregap for track 1 is assumed if not specified. Some
       * older dumps had implicit pregaps that aren't in the cue; we add them
       * back unless this is clearly an audio CD (track 1 = audio), since
       * forcing pregaps there breaks games like "Dancing Stage featuring
       * DREAMS COME TRUE". */
      const bool is_multi_track_bin = (track_num > 1u && file_idx == img->base.indices[0].file_index);
      const cue_parser_track_t* first_track = cue_parser_file_get_track(&parser, 1);
      const bool likely_audio_cd = (first_track && first_track->mode == CD_IMAGE_TRACK_MODE_AUDIO);

      pregap_frames = track->has_zero_pregap ? cd_image_position_to_lba(track->zero_pregap) : 0u;
      if ((track_num == 1u || is_multi_track_bin) && !track->has_zero_pregap &&
          (track_num == 1u || !likely_audio_cd)) {
        pregap_frames = 2u * CD_IMAGE_FRAMES_PER_SECOND;
      }

      if (pregap_frames > 0u) {
        cd_image_index_t* pregap = cd_image_push_index(&img->base);
        pregap->start_lba_on_disc  = disc_lba;
        pregap->start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
        pregap->length             = pregap_frames;
        pregap->track_number       = track_num;
        pregap->index_number       = 0;
        pregap->mode               = mode;
        pregap->submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
        pregap->control            = control;
        pregap->is_pregap          = true;
        disc_lba += pregap->length;
      }
    }

    /* Push the track summary. */
    cd_image_track_t* tr = cd_image_push_track(&img->base);
    tr->track_number = track_num;
    tr->start_lba    = disc_lba;
    tr->first_index  = img->base.index_count;
    tr->length       = track_length + pregap_frames;
    tr->mode         = mode;
    tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    tr->control      = control;

    /* Walk the per-track INDEX list, splitting into sub-indices. */
    cd_image_index_t last_index;
    memset(&last_index, 0, sizeof(last_index));
    last_index.start_lba_on_disc  = disc_lba;
    last_index.start_lba_in_track = 0;
    last_index.track_number       = track_num;
    last_index.index_number       = 1;
    last_index.file_index         = file_idx;
    last_index.file_sector_size   = track_sector_size;
    last_index.file_offset        = (u64)track_start * (u64)track_sector_size;
    last_index.mode               = mode;
    last_index.submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    last_index.control            = control;
    last_index.is_pregap          = false;

    cd_image_lba_t last_index_offset = track_start;
    for (u32 idx_num = 1; idx_num <= CUE_PARSER_MAX_INDEX_NUMBER; idx_num++) {
      const cd_image_position_t* pos = cue_parser_track_get_index(track, idx_num);
      if (!pos)
        break;
      const cd_image_lba_t index_offset = cd_image_position_to_lba(*pos);
      if (index_offset > last_index_offset) {
        last_index.length = index_offset - last_index_offset;
        cd_image_index_t* slot = cd_image_push_index(&img->base);
        *slot = last_index;

        disc_lba += last_index.length;
        last_index.start_lba_in_track += last_index.length;
        last_index.start_lba_on_disc   = disc_lba;
        last_index.length              = 0;
      }
      last_index.file_offset  = (u64)index_offset * (u64)last_index.file_sector_size;
      last_index.index_number = idx_num;
      last_index_offset       = index_offset;
    }

    const cd_image_lba_t track_end_index = track_start + track_length;
    DebugAssert(track_end_index >= last_index_offset);
    if (track_end_index > last_index_offset) {
      last_index.length = track_end_index - last_index_offset;
      cd_image_index_t* slot = cd_image_push_index(&img->base);
      *slot = last_index;
      disc_lba += last_index.length;
    }
  }

  cue_parser_file_destroy(&parser);

  if (img->base.track_count == 0) {
    ERROR_LOG("File '%s' contains no tracks", path);
    Error_set_string_fmt(error, "File '%s' contains no tracks", path);
    return false;
  }

  img->base.lba_count = disc_lba;
  cd_image_add_lead_out_index(&img->base);

  return cd_image_seek_track_msf(&img->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

static bool open_and_parse_single_file(cd_image_cue_t* img, const char* path, Error* error)
{
  img->base.filename = (char*)malloc(strlen(path) + 1u);
  if (!img->base.filename) Panic("Memory allocation failed.");
  strcpy(img->base.filename, path);

  /* Open the bin/img/iso as the only track file. */
  cue_track_file_t tf;
  const char* base_data; u32 base_len;
  path_get_file_name_cstr(path, &base_data, &base_len);
  /* Need NUL-terminated filename for the file handle. */
  char* fn = (char*)malloc((size_t)base_len + 1u);
  if (!fn) Panic("Memory allocation failed.");
  memcpy(fn, base_data, base_len);
  fn[base_len] = '\0';

  if (!cue_track_file_open(&tf, fn, path, error)) {
    free(fn);
    return false;
  }
  free(fn);

  cue_track_file_t* slot = cue_image_push_file(img);
  *slot = tf;

  const u32 track_sector_size = CD_IMAGE_RAW_SECTOR_SIZE;
  img->base.lba_count = (u32)(cue_track_file_size(&img->files[0]) / (u64)track_sector_size);

  cd_image_subq_control_t control;
  control.bits = 0;
  const cd_image_track_mode_t mode = CD_IMAGE_TRACK_MODE_MODE2_RAW;
  cd_image_subq_control_set_data(&control, mode != CD_IMAGE_TRACK_MODE_AUDIO);

  /* Two-second default pregap. */
  const u32 pregap_frames = 2u * CD_IMAGE_FRAMES_PER_SECOND;

  cd_image_index_t* pregap = cd_image_push_index(&img->base);
  pregap->file_sector_size   = track_sector_size;
  pregap->start_lba_on_disc  = 0;
  pregap->start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
  pregap->length             = pregap_frames;
  pregap->track_number       = 1;
  pregap->index_number       = 0;
  pregap->mode               = mode;
  pregap->submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
  pregap->control            = control;
  pregap->is_pregap          = true;
  pregap->file_sector_size   = track_sector_size;

  cd_image_index_t* data = cd_image_push_index(&img->base);
  data->file_index         = 0;
  data->file_offset        = 0;
  data->file_sector_size   = track_sector_size;
  data->start_lba_on_disc  = pregap_frames;
  data->track_number       = 1;
  data->index_number       = 1;
  data->start_lba_in_track = 0;
  data->length             = img->base.lba_count;
  data->mode               = mode;
  data->submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
  data->control            = control;

  cd_image_track_t* tr = cd_image_push_track(&img->base);
  tr->track_number = 1;
  tr->start_lba    = pregap_frames;
  tr->first_index  = 0;
  tr->length       = img->base.lba_count + pregap_frames;
  tr->mode         = mode;
  tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
  tr->control      = control;

  cd_image_add_lead_out_index(&img->base);

  return cd_image_seek_track_msf(&img->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

static cd_image_cue_t* alloc_cue_image(void)
{
  cd_image_cue_t* img = (cd_image_cue_t*)malloc(sizeof(cd_image_cue_t));
  if (!img) Panic("Memory allocation failed.");
  cd_image_init(&img->base, &s_cue_vtable);
  img->files         = NULL;
  img->file_count    = 0;
  img->file_capacity = 0;
  return img;
}

cd_image_t* cd_image_open_cue(const char* path, Error* error)
{
  cd_image_cue_t* img = alloc_cue_image();
  if (!open_and_parse_cue(img, path, error)) {
    cd_image_destroy(&img->base);
    return NULL;
  }
  return &img->base;
}

cd_image_t* cd_image_open_bin(const char* path, Error* error)
{
  cd_image_cue_t* img = alloc_cue_image();
  if (!open_and_parse_single_file(img, path, error)) {
    cd_image_destroy(&img->base);
    return NULL;
  }
  return &img->base;
}
