/*
 * Translation strings (TRANSLATE_FS) replaced with literal English; the
 * translation framework is not ported in cupid-ps1.
 */

#include "cd_image_hasher.h"

#include "cd_image.h"

#include "common/error.h"
#include "common/md5_digest.h"
#include "common/progress_callback.h"
#include "common/string_util.h"

#include <stdio.h>
#include <string.h>

void cd_image_hash_to_string(const u8 hash[CD_IMAGE_HASH_SIZE], char out[33])
{
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < CD_IMAGE_HASH_SIZE; i++) {
    out[i * 2 + 0] = hex[(hash[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[hash[i]        & 0xF];
  }
  out[32] = '\0';
}

bool cd_image_hash_from_string(const char* str, u32 len, u8 out_hash[CD_IMAGE_HASH_SIZE])
{
  return string_util_decode_hex(out_hash, CD_IMAGE_HASH_SIZE, str, len) == CD_IMAGE_HASH_SIZE;
}

static bool read_index(cd_image_t* image, u8 track, u8 index, md5_digest_t* digest,
                       progress_callback_t* pc, Error* err)
{
  const cd_image_lba_t start  = cd_image_get_track_index_position(image, track, index);
  const u32            length = cd_image_get_track_index_length  (image, track, index);
  const u32            update = (length / 100u) > 1u ? (length / 100u) : 1u;

  progress_callback_set_progress_range(pc, length);

  if (!cd_image_seek_lba(image, start)) {
    Error_set_string_fmt(err, "Failed to seek to sector %u for track %u index %u", start, track, index);
    return false;
  }

  u8 sector[CD_IMAGE_RAW_SECTOR_SIZE];
  for (u32 lba = 0; lba < length; lba++) {
    if ((lba % update) == 0)
      progress_callback_set_progress_value(pc, lba);
    if (progress_callback_is_cancelled(pc))
      return false;
    if (!cd_image_read_raw_sector(image, sector, NULL)) {
      Error_set_string_fmt(err, "Failed to read sector %u from image", cd_image_get_position_on_disc(image));
      return false;
    }
    md5_digest_update(digest, sector, sizeof(sector));
  }
  progress_callback_set_progress_value(pc, length);
  return true;
}

static bool read_track(cd_image_t* image, u8 track, md5_digest_t* digest,
                       progress_callback_t* pc, Error* err)
{
  enum { INDICES_TO_READ = 2 };

  progress_callback_push_state(pc);

  const bool data_track = (track == 1);
  char status[64];
  snprintf(status, sizeof(status), "Computing hash for Track %u...", (unsigned)track);
  progress_callback_set_status_text(pc, status, (u32)strlen(status));
  progress_callback_set_progress_range(pc, data_track ? 1u : 2u);

  u8 progress = 0;
  for (u8 index = 0; index < INDICES_TO_READ; index++) {
    progress_callback_set_progress_value(pc, progress);
    /* Skip pre-gap (index 0) for data tracks. */
    if (data_track && index == 0)
      continue;
    progress++;
    progress_callback_push_state(pc);
    if (!read_index(image, track, index, digest, pc, err)) {
      progress_callback_pop_state(pc);
      progress_callback_pop_state(pc);
      return false;
    }
    progress_callback_pop_state(pc);
  }

  progress_callback_set_progress_value(pc, progress);
  progress_callback_pop_state(pc);
  return true;
}

bool cd_image_hasher_get_image_hash(cd_image_t* image, u8 out_hash[CD_IMAGE_HASH_SIZE],
                                    progress_callback_t* pc, Error* err)
{
  md5_digest_t d;
  md5_digest_init(&d);

  progress_callback_set_cancellable(pc, true);
  progress_callback_set_progress_range(pc, cd_image_get_track_count(image));
  progress_callback_set_progress_value(pc, 0);
  progress_callback_push_state(pc);

  for (u32 i = 1; i <= cd_image_get_track_count(image); i++) {
    progress_callback_set_progress_value(pc, i - 1);
    if (!read_track(image, (u8)i, &d, pc, err)) {
      progress_callback_pop_state(pc);
      return false;
    }
  }

  progress_callback_set_progress_value(pc, cd_image_get_track_count(image));
  md5_digest_final(&d, out_hash);
  return true;
}

bool cd_image_hasher_get_track_hash(cd_image_t* image, u8 track, u8 out_hash[CD_IMAGE_HASH_SIZE],
                                    progress_callback_t* pc, Error* err)
{
  md5_digest_t d;
  md5_digest_init(&d);
  if (!read_track(image, track, &d, pc, err))
    return false;
  md5_digest_final(&d, out_hash);
  return true;
}
