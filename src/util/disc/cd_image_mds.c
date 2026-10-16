/*
 * CDImageMDS: read Alcohol 120% .mds (Media Descriptor) + .mdf (Media Data
 * File) image pairs.  The .mds is a binary descriptor with a "MEDIA
 * DESCRIPTOR" magic, a session offset pointing to a 24-byte session header,
 * and a per-track 0x50-byte TrackEntry array.  The .mdf alongside it holds
 * raw 2352 (or 2448 with subchannel) sectors per track.
 */

#include "cd_image.h"

#include "common/bcdutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDImage);

#pragma pack(push, 1)
typedef struct {
  u8  track_type;
  u8  has_subchannel_data;
  u8  unk1;
  u8  unk2;
  u8  track_number;
  u8  unk3[4];
  u8  start_m;
  u8  start_s;
  u8  start_f;
  u32 extra_offset;
  u8  unk4[24];
  u32 track_offset_in_mdf;
  u8  unk5[36];
} mds_track_entry_t;
#pragma pack(pop)
_Static_assert(sizeof(mds_track_entry_t) == 0x50, "mds_track_entry_t is 0x50 bytes");

typedef struct cd_image_mds {
  cd_image_t base;        /* MUST be first */
  FILE*      mdf_file;
  u64        mdf_position;
} cd_image_mds_t;

static bool cdimage_mds_read_sector_from_index(cd_image_t* self, void* buffer,
                                               const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_mds_t* img = (cd_image_mds_t*)self;
  const u64 file_position = index->file_offset + ((u64)lba_in_index * (u64)index->file_sector_size);
  if (img->mdf_position != file_position) {
    if (fseek(img->mdf_file, (long)file_position, SEEK_SET) != 0)
      return false;
    img->mdf_position = file_position;
  }

  /* We don't want subchannel; only RAW_SECTOR_SIZE. */
  if (fread(buffer, CD_IMAGE_RAW_SECTOR_SIZE, 1, img->mdf_file) != 1) {
    fseek(img->mdf_file, (long)img->mdf_position, SEEK_SET);
    return false;
  }
  img->mdf_position += CD_IMAGE_RAW_SECTOR_SIZE;
  return true;
}

static s64 cdimage_mds_get_size_on_disk(const cd_image_t* self)
{
  cd_image_mds_t* img = (cd_image_mds_t*)self;
  return fs_fsize64(img->mdf_file, NULL);
}

static void cdimage_mds_destroy(cd_image_t* self)
{
  cd_image_mds_t* img = (cd_image_mds_t*)self;
  if (img->mdf_file) {
    fclose(img->mdf_file);
    img->mdf_file = NULL;
  }
}

static const cd_image_vtable_t s_mds_vtable = {
   .read_sector_from_index = cdimage_mds_read_sector_from_index,
  .get_size_on_disk       = cdimage_mds_get_size_on_disk,
  .destroy                = cdimage_mds_destroy, 
};

static cd_image_mds_t* alloc_mds_image(void)
{
  cd_image_mds_t* img = (cd_image_mds_t*)malloc(sizeof(*img));
  if (!img) abort();
  cd_image_init(&img->base, &s_mds_vtable);
  img->mdf_file = NULL;
  img->mdf_position = 0;
  return img;
}

static const char* file_basename(const char* path)
{
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool open_and_parse_mds(cd_image_mds_t* img, const char* filename, Error* error)
{
  u8*    mds = NULL;
  size_t mds_size = 0;
  if (!fs_read_binary_file_path(filename, &mds, &mds_size, error)) {
    Error_set_string_fmt(error, "Failed to read mds '%s'", file_basename(filename));
    return false;
  }
  if (mds_size < 0x54) {
    ERROR_LOG("Failed to read mds file '%s'", file_basename(filename));
    Error_set_string_fmt(error, "Failed to read mds file '%s'", filename);
    free(mds);
    return false;
  }

  /* Compute companion .mdf path. */
  small_string_t mdf_path;
  small_string_init(&mdf_path);
  path_change_extension_cstr(&mdf_path, filename, "mdf");
  img->mdf_file = fs_open_shared_file(small_string_c_str(&mdf_path), "rb",
                                      FS_FILE_SHARE_DENY_WRITE, error);
  if (!img->mdf_file) {
    Error_set_string_fmt(error, "Failed to open mdf file '%s'", file_basename(small_string_c_str(&mdf_path)));
    small_string_destroy(&mdf_path);
    free(mds);
    return false;
  }
  small_string_destroy(&mdf_path);

  static const char expected_signature[] = "MEDIA DESCRIPTOR";
  if (memcmp(mds, expected_signature, sizeof(expected_signature) - 1) != 0) {
    ERROR_LOG("Incorrect signature in '%s'", file_basename(filename));
    Error_set_string_fmt(error, "Incorrect signature in '%s'", file_basename(filename));
    free(mds);
    return false;
  }

  u32 session_offset;
  memcpy(&session_offset, &mds[0x50], sizeof(session_offset));
  if ((u64)session_offset + 24u > mds_size) {
    ERROR_LOG("Invalid session offset in '%s'", file_basename(filename));
    Error_set_string_fmt(error, "Invalid session offset in '%s'", file_basename(filename));
    free(mds);
    return false;
  }

  u16 track_count;
  u32 track_offset;
  memcpy(&track_count, &mds[session_offset + 14], sizeof(track_count));
  memcpy(&track_offset, &mds[session_offset + 20], sizeof(track_offset));
  if (track_count > 99 || track_offset >= mds_size) {
    ERROR_LOG("Invalid track count/block offset %u/%u in '%s'", track_count, track_offset, file_basename(filename));
    Error_set_string_fmt(error, "Invalid track count/block offset %u/%u in '%s'",
                         track_count, track_offset, file_basename(filename));
    free(mds);
    return false;
  }

  /* Skip header tracks (track_number >= 0xA0). */
  while (track_offset + sizeof(mds_track_entry_t) <= mds_size) {
    mds_track_entry_t track;
    memcpy(&track, &mds[track_offset], sizeof(track));
    if (track.track_number < 0xA0)
      break;
    track_offset += sizeof(mds_track_entry_t);
  }

  for (u32 track_number = 1; track_number <= track_count; track_number++) {
    if (track_offset + sizeof(mds_track_entry_t) > mds_size) {
      ERROR_LOG("End of file in '%s' at track %u", file_basename(filename), track_number);
      Error_set_string_fmt(error, "End of file in '%s' at track %u", file_basename(filename), track_number);
      free(mds);
      return false;
    }

    mds_track_entry_t track;
    memcpy(&track, &mds[track_offset], sizeof(track));
    track_offset += sizeof(mds_track_entry_t);

    if (PackedBCDToBinary(track.track_number) != track_number) {
      ERROR_LOG("Unexpected track number 0x%02X in track %u", track.track_number, track_number);
      Error_set_string_fmt(error, "Unexpected track number 0x%02X in track %u",
                           track.track_number, track_number);
      free(mds);
      return false;
    }

    const bool contains_subchannel = (track.has_subchannel_data != 0);
    const u32  track_sector_size = contains_subchannel ? 2448u : (u32)CD_IMAGE_RAW_SECTOR_SIZE;
    const cd_image_track_mode_t mode = (track.track_type == 0xA9)
        ? CD_IMAGE_TRACK_MODE_AUDIO : CD_IMAGE_TRACK_MODE_MODE2_RAW;

    if ((u64)track.extra_offset + sizeof(u32) + sizeof(u32) > mds_size) {
      ERROR_LOG("Invalid extra offset %u in track %u", track.extra_offset, track_number);
      Error_set_string_fmt(error, "Invalid extra offset %u in track %u", track.extra_offset, track_number);
      free(mds);
      return false;
    }

    const cd_image_lba_t track_start_lba =
        cd_image_position_to_lba(cd_image_position_from_bcd(track.start_m, track.start_s, track.start_f));
    u32 track_file_offset = track.track_offset_in_mdf;

    u32 track_pregap, track_length;
    memcpy(&track_pregap, &mds[track.extra_offset],            sizeof(track_pregap));
    memcpy(&track_length, &mds[track.extra_offset + sizeof(u32)], sizeof(track_length));

    /* Subchannel Q control byte: data flag set for non-audio. */
    cd_image_subq_control_t control = { 0 };
    cd_image_subq_control_set_data(&control, mode != CD_IMAGE_TRACK_MODE_AUDIO);

    /* Pregap index. */
    if (track_pregap > 0) {
      if (track_pregap > track_start_lba) {
        ERROR_LOG("Track pregap %u is too large for start lba %u", track_pregap, track_start_lba);
        Error_set_string_fmt(error, "Track pregap %u is too large for start lba %u",
                             track_pregap, track_start_lba);
        free(mds);
        return false;
      }

      cd_image_index_t pregap = { 0 };
      pregap.start_lba_on_disc  = track_start_lba - track_pregap;
      pregap.start_lba_in_track = (cd_image_lba_t)(-(s32)track_pregap);
      pregap.length             = track_pregap;
      pregap.track_number       = track_number;
      pregap.index_number       = 0;
      pregap.mode               = mode;
      pregap.submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
      pregap.control            = control;
      pregap.is_pregap          = true;

      const bool pregap_in_file = (track_number > 1);
      if (pregap_in_file) {
        pregap.file_index = 0;
        pregap.file_offset = track_file_offset;
        pregap.file_sector_size = track_sector_size;
        track_file_offset += track_pregap * track_sector_size;
      }
      *cd_image_push_index(&img->base) = pregap;
    }

    /* The track itself. */
    cd_image_track_t* tr = cd_image_push_track(&img->base);
    tr->track_number = track_number;
    tr->start_lba    = track_start_lba;
    tr->first_index  = img->base.index_count;
    tr->length       = track_length;
    tr->mode         = mode;
    tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    tr->control      = control;

    /* Track index 1. */
    cd_image_index_t idx = { 0 };
    idx.start_lba_on_disc  = track_start_lba;
    idx.start_lba_in_track = 0;
    idx.track_number       = track_number;
    idx.index_number       = 1;
    idx.file_index         = 0;
    idx.file_sector_size   = track_sector_size;
    idx.file_offset        = track_file_offset;
    idx.mode               = mode;
    idx.submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    idx.control            = control;
    idx.is_pregap          = false;
    idx.length             = track_length;
    *cd_image_push_index(&img->base) = idx;
  }

  free(mds);

  if (img->base.track_count == 0) {
    ERROR_LOG("File '%s' contains no tracks", file_basename(filename));
    Error_set_string_fmt(error, "File '%s' contains no tracks", file_basename(filename));
    return false;
  }

  const cd_image_track_t* last = &img->base.tracks[img->base.track_count - 1];
  img->base.lba_count = last->start_lba + last->length;
  cd_image_add_lead_out_index(&img->base);

  return cd_image_seek_track_msf(&img->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

cd_image_t* cd_image_open_mds(const char* path, Error* error)
{
  cd_image_mds_t* img = alloc_mds_image();
  if (!open_and_parse_mds(img, path, error)) {
    cd_image_destroy(&img->base);
    return NULL;
  }
  return &img->base;
}
