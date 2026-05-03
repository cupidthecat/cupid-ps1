#include "cd_image.h"

#include "common/assert.h"
#include "common/bcdutils.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_CHANNEL(CDImage);

static void grow_indices(cd_image_t* i)
{
  if (i->index_count < i->index_capacity)
    return;
  const u32 new_cap = (i->index_capacity == 0) ? 8u : (i->index_capacity * 2u);
  cd_image_index_t* p = (cd_image_index_t*)realloc(i->indices, new_cap * sizeof(cd_image_index_t));
  if (!p) Panic("Memory allocation failed.");
  i->indices = p;
  i->index_capacity = new_cap;
}

static void grow_tracks(cd_image_t* i)
{
  if (i->track_count < i->track_capacity)
    return;
  const u32 new_cap = (i->track_capacity == 0) ? 4u : (i->track_capacity * 2u);
  cd_image_track_t* p = (cd_image_track_t*)realloc(i->tracks, new_cap * sizeof(cd_image_track_t));
  if (!p) Panic("Memory allocation failed.");
  i->tracks = p;
  i->track_capacity = new_cap;
}

cd_image_position_t cd_image_position_from_bcd(u8 minute, u8 second, u8 frame)
{
  cd_image_position_t p;
  p.minute = PackedBCDToBinary(minute);
  p.second = PackedBCDToBinary(second);
  p.frame  = PackedBCDToBinary(frame);
  return p;
}

cd_image_position_t cd_image_position_from_lba(cd_image_lba_t lba)
{
  cd_image_position_t p;
  p.frame  = (u8)(lba % CD_IMAGE_FRAMES_PER_SECOND);
  lba     /= CD_IMAGE_FRAMES_PER_SECOND;
  p.second = (u8)(lba % CD_IMAGE_SECONDS_PER_MINUTE);
  lba     /= CD_IMAGE_SECONDS_PER_MINUTE;
  p.minute = (u8)lba;
  return p;
}

cd_image_lba_t cd_image_position_to_lba(cd_image_position_t p)
{
  return (cd_image_lba_t)p.minute * (cd_image_lba_t)CD_IMAGE_FRAMES_PER_MINUTE
       + (cd_image_lba_t)p.second * (cd_image_lba_t)CD_IMAGE_FRAMES_PER_SECOND
       + (cd_image_lba_t)p.frame;
}

void cd_image_position_to_bcd(cd_image_position_t p, u8* out_minute, u8* out_second, u8* out_frame)
{
  *out_minute = BinaryToBCD(p.minute);
  *out_second = BinaryToBCD(p.second);
  *out_frame  = BinaryToBCD(p.frame);
}

cd_image_position_t cd_image_position_add(cd_image_position_t a, cd_image_position_t b)
{
  return cd_image_position_from_lba(cd_image_position_to_lba(a) + cd_image_position_to_lba(b));
}

bool cd_image_position_eq(cd_image_position_t a, cd_image_position_t b)
{
  return a.minute == b.minute && a.second == b.second && a.frame == b.frame;
}
bool cd_image_position_lt(cd_image_position_t a, cd_image_position_t b)
{
  if (a.minute != b.minute) return a.minute < b.minute;
  if (a.second != b.second) return a.second < b.second;
  return a.frame < b.frame;
}
bool cd_image_position_le(cd_image_position_t a, cd_image_position_t b)
{
  return cd_image_position_lt(a, b) || cd_image_position_eq(a, b);
}
bool cd_image_position_gt(cd_image_position_t a, cd_image_position_t b)
{
  return cd_image_position_lt(b, a);
}
bool cd_image_position_ge(cd_image_position_t a, cd_image_position_t b)
{
  return !cd_image_position_lt(a, b);
}

u32 cd_image_get_bytes_per_sector(cd_image_track_mode_t mode)
{
  static const u32 sizes[8] = { 2352, 2048, 2352, 2336, 2048, 2324, 2332, 2352 };
  return sizes[(u32)mode];
}

 /* Adapted from
 * https://github.com/saramibreak/DiscImageCreator/blob/5a8fe21730872d67991211f1319c87f0780f2d0f/DiscImageCreator/convert.cpp */
void cd_image_deinterleave_subcode(const u8* subcode_in, u8* subcode_out)
{
  memset(subcode_out, 0, CD_IMAGE_ALL_SUBCODE_SIZE);

  u32 row = 0;
  for (u32 bit_num = 0; bit_num < 8u; bit_num++) {
    for (u32 col = 0; col < CD_IMAGE_ALL_SUBCODE_SIZE; row++) {
      u32 mask = 0x80u;
      for (int shift = 0; shift < 8; shift++, col++) {
        const s32 n = (s32)shift - (s32)bit_num;
        if (n > 0)
          subcode_out[row] |= (u8)((subcode_in[col] >> n) & mask);
        else
          subcode_out[row] |= (u8)((subcode_in[col] << (-n)) & mask);
        mask >>= 1;
      }
    }
  }
}

static const u16 s_crc16_table[256] = {
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD,
  0xE1CE, 0xF1EF, 0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6, 0x9339, 0x8318, 0xB37B, 0xA35A,
  0xD3BD, 0xC39C, 0xF3FF, 0xE3DE, 0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485, 0xA56A, 0xB54B,
  0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D, 0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
  0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC, 0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861,
  0x2802, 0x3823, 0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B, 0x5AF5, 0x4AD4, 0x7AB7, 0x6A96,
  0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A, 0x6CA6, 0x7C87,
  0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
  0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70, 0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A,
  0x9F59, 0x8F78, 0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3,
  0x5004, 0x4025, 0x7046, 0x6067, 0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E, 0x02B1, 0x1290,
  0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256, 0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
  0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405, 0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E,
  0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634, 0xD94C, 0xC96D, 0xF90E, 0xE92F,
  0x99C8, 0x89E9, 0xB98A, 0xA9AB, 0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3, 0xCB7D, 0xDB5C,
  0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A, 0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
  0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9, 0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83,
  0x1CE0, 0x0CC1, 0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36, 0x4E55, 0x5E74,
  0x2E93, 0x3EB2, 0x0ED1, 0x1EF0 
};

u16 cd_image_subq_compute_crc(const u8* data)
{
  u16 value = 0;
  for (u32 k = 0; k < 10u; k++)
    value = (u16)(s_crc16_table[(value >> 8) ^ data[k]] ^ (value << 8));

  /* Invert and swap. */
  return ByteSwap_u16((u16)~value);
}

bool cd_image_subq_is_crc_valid(const cd_image_subq_t* subq)
{
  return subq->crc == cd_image_subq_compute_crc((const u8*)subq);
}

void cd_image_init(cd_image_t* i, const cd_image_vtable_t* vtbl)
{
  memset(i, 0, sizeof(*i));
  i->vtbl = vtbl;
}

void cd_image_clear_toc(cd_image_t* i)
{
  i->lba_count         = 0;
  i->index_count       = 0;
  i->track_count       = 0;
  i->current_index     = NULL;
  i->position_in_index = 0;
  i->position_in_track = 0;
  i->position_on_disc  = 0;
  /* Keep capacity reserved for reuse. */
}

void cd_image_copy_toc(cd_image_t* dst, const cd_image_t* src)
{
  dst->lba_count         = src->lba_count;
  dst->index_count       = 0;
  dst->track_count       = 0;
  dst->current_index     = NULL;
  dst->position_in_index = 0;
  dst->position_in_track = 0;
  dst->position_on_disc  = 0;

  if (src->index_count > dst->index_capacity) {
    cd_image_index_t* p = (cd_image_index_t*)realloc(
      dst->indices, src->index_count * sizeof(cd_image_index_t));
    if (!p) Panic("Memory allocation failed.");
    dst->indices = p;
    dst->index_capacity = src->index_count;
  }
  if (src->track_count > dst->track_capacity) {
    cd_image_track_t* p = (cd_image_track_t*)realloc(
      dst->tracks, src->track_count * sizeof(cd_image_track_t));
    if (!p) Panic("Memory allocation failed.");
    dst->tracks = p;
    dst->track_capacity = src->track_count;
  }

  if (src->index_count) memcpy(dst->indices, src->indices, src->index_count * sizeof(cd_image_index_t));
  if (src->track_count) memcpy(dst->tracks, src->tracks, src->track_count * sizeof(cd_image_track_t));
  dst->index_count = src->index_count;
  dst->track_count = src->track_count;
}

void cd_image_destroy(cd_image_t* image)
{
  if (!image)
    return;
  if (image->vtbl && image->vtbl->destroy)
    image->vtbl->destroy(image);
  free(image->filename);
  free(image->indices);
  free(image->tracks);
  free(image);
}

cd_image_index_t* cd_image_push_index(cd_image_t* i)
{
  grow_indices(i);
  cd_image_index_t* idx = &i->indices[i->index_count++];
  memset(idx, 0, sizeof(*idx));
  return idx;
}

cd_image_track_t* cd_image_push_track(cd_image_t* i)
{
  grow_tracks(i);
  cd_image_track_t* tr = &i->tracks[i->track_count++];
  memset(tr, 0, sizeof(*tr));
  return tr;
}

cd_image_lba_t cd_image_get_track_start_position(const cd_image_t* i, u8 track)
{
  Assert(track > 0 && track <= i->track_count);
  return i->tracks[track - 1].start_lba;
}
cd_image_position_t cd_image_get_track_start_msf_position(const cd_image_t* i, u8 track)
{
  Assert(track > 0 && track <= i->track_count);
  return cd_image_position_from_lba(i->tracks[track - 1].start_lba);
}
cd_image_lba_t cd_image_get_track_length(const cd_image_t* i, u8 track)
{
  Assert(track > 0 && track <= i->track_count);
  return i->tracks[track - 1].length;
}
cd_image_position_t cd_image_get_track_msf_length(const cd_image_t* i, u8 track)
{
  Assert(track > 0 && track <= i->track_count);
  return cd_image_position_from_lba(i->tracks[track - 1].length);
}
cd_image_track_mode_t cd_image_get_track_mode(const cd_image_t* i, u8 track)
{
  Assert(track > 0 && track <= i->track_count);
  return i->tracks[track - 1].mode;
}

cd_image_lba_t cd_image_get_track_index_position(const cd_image_t* i, u8 track, u8 index)
{
  for (u32 k = 0; k < i->index_count; k++) {
    const cd_image_index_t* ci = &i->indices[k];
    if (ci->track_number == track && ci->index_number == index)
      return ci->start_lba_on_disc;
  }
  return i->lba_count;
}

cd_image_lba_t cd_image_get_track_index_length(const cd_image_t* i, u8 track, u8 index)
{
  for (u32 k = 0; k < i->index_count; k++) {
    const cd_image_index_t* ci = &i->indices[k];
    if (ci->track_number == track && ci->index_number == index)
      return ci->length;
  }
  return 0;
}

const cd_image_track_t* cd_image_get_track(const cd_image_t* i, u32 track)
{
  Assert(track > 0 && track <= i->track_count);
  return &i->tracks[track - 1];
}

const cd_image_index_t* cd_image_get_index(const cd_image_t* i, u32 index)
{
  Assert(index < i->index_count);
  return &i->indices[index];
}

const cd_image_index_t* cd_image_get_index_for_disc_position(const cd_image_t* i, cd_image_lba_t pos)
{
  for (u32 k = 0; k < i->index_count; k++) {
    const cd_image_index_t* idx = &i->indices[k];
    if (pos < idx->start_lba_on_disc)
      continue;
    const cd_image_lba_t off = pos - idx->start_lba_on_disc;
    if (off >= idx->length)
      continue;
    return idx;
  }
  return NULL;
}

const cd_image_index_t* cd_image_get_index_for_track_position(const cd_image_t* i, u32 track_number, cd_image_lba_t track_pos)
{
  if (track_number < 1u || track_number > i->track_count)
    return NULL;
  const cd_image_track_t* tr = &i->tracks[track_number - 1u];
  if (track_pos >= tr->length)
    return NULL;
  return cd_image_get_index_for_disc_position(i, tr->start_lba + track_pos);
}

bool cd_image_seek_lba(cd_image_t* i, cd_image_lba_t lba)
{
  const cd_image_index_t* new_index;
  if (i->current_index && lba >= i->current_index->start_lba_on_disc &&
      (lba - i->current_index->start_lba_on_disc) < i->current_index->length) {
    new_index = i->current_index;
  } else {
    new_index = cd_image_get_index_for_disc_position(i, lba);
    if (!new_index)
      return false;
  }

  const cd_image_lba_t off = lba - new_index->start_lba_on_disc;
  if (off >= new_index->length)
    return false;

  i->current_index     = new_index;
  i->position_on_disc  = lba;
  i->position_in_index = off;
  i->position_in_track = new_index->start_lba_in_track + off;
  return true;
}

bool cd_image_seek_msf(cd_image_t* i, cd_image_position_t pos)
{
  return cd_image_seek_lba(i, cd_image_position_to_lba(pos));
}

bool cd_image_seek_track_msf(cd_image_t* i, u32 track_number, cd_image_position_t pos_in_track)
{
  if (track_number < 1u || track_number > i->track_count)
    return false;
  const cd_image_track_t* tr = &i->tracks[track_number - 1u];
  const cd_image_lba_t pos_lba = cd_image_position_to_lba(pos_in_track);
  if (pos_lba >= tr->length)
    return false;
  return cd_image_seek_lba(i, tr->start_lba + pos_lba);
}

bool cd_image_seek_track_lba(cd_image_t* i, u32 track_number, cd_image_lba_t lba)
{
  if (track_number < 1u || track_number > i->track_count)
    return false;
  const cd_image_track_t* tr = &i->tracks[track_number - 1u];
  return cd_image_seek_lba(i, tr->start_lba + lba);
}

bool cd_image_read_raw_sector(cd_image_t* i, void* buffer, cd_image_subq_t* subq)
{
  if (i->position_in_index == i->current_index->length) {
    if (!cd_image_seek_lba(i, i->position_on_disc))
      return false;
  }

  if (buffer) {
    if (i->current_index->file_sector_size > 0) {
      if (!cd_image_read_sector_from_index(i, buffer, i->current_index, i->position_in_index)) {
        ERROR_LOG("Read of LBA %u failed", i->position_on_disc);
        cd_image_seek_lba(i, i->position_on_disc);
        return false;
      }
    } else {
      if (i->current_index->track_number == CD_IMAGE_LEAD_OUT_TRACK_NUMBER) {
        /* Lead-out area. */
        memset(buffer, 0xAA, CD_IMAGE_RAW_SECTOR_SIZE);
      } else {
        /* This is an implicit pregap. Return silence. */
        memset(buffer, 0x00, CD_IMAGE_RAW_SECTOR_SIZE);
      }
    }
  }

  if (subq && !cd_image_read_subchannel_q(i, subq, i->current_index, i->position_in_index)) {
    ERROR_LOG("Subchannel read of LBA %u failed", i->position_on_disc);
    cd_image_seek_lba(i, i->position_on_disc);
    return false;
  }

  i->position_on_disc++;
  i->position_in_index++;
  i->position_in_track++;
  return true;
}

bool cd_image_read_subchannel_q(cd_image_t* i, cd_image_subq_t* out,
                                const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  if (i->vtbl->read_subchannel_q)
    return i->vtbl->read_subchannel_q(i, out, index, lba_in_index);

  cd_image_generate_subq_from_index(i, out, index, lba_in_index);
  return true;
}

bool cd_image_has_subchannel_data(const cd_image_t* i)
{
  if (i->vtbl->has_subchannel_data)
    return i->vtbl->has_subchannel_data(i);
  return false;
}

cd_image_precache_result_t cd_image_precache(cd_image_t* i, Error* error)
{
  if (i->vtbl->precache)
    return i->vtbl->precache(i, error);
  return CD_IMAGE_PRECACHE_RESULT_UNSUPPORTED;
}

bool cd_image_is_precached(const cd_image_t* i)
{
  if (i->vtbl->is_precached)
    return i->vtbl->is_precached(i);
  return false;
}

s64 cd_image_get_size_on_disk(const cd_image_t* i)
{
  if (i->vtbl->get_size_on_disk)
    return i->vtbl->get_size_on_disk(i);
  return -1;
}

bool cd_image_has_sub_images(const cd_image_t* i)
{
  if (i->vtbl->has_sub_images)
    return i->vtbl->has_sub_images(i);
  return false;
}

u32 cd_image_get_sub_image_count(const cd_image_t* i)
{
  if (i->vtbl->get_sub_image_count)
    return i->vtbl->get_sub_image_count(i);
  return 0;
}

u32 cd_image_get_current_sub_image(const cd_image_t* i)
{
  if (i->vtbl->get_current_sub_image)
    return i->vtbl->get_current_sub_image(i);
  return 0;
}

char* cd_image_get_sub_image_title(const cd_image_t* i, u32 index)
{
  if (i->vtbl->get_sub_image_title)
    return i->vtbl->get_sub_image_title(i, index);
  return NULL;
}

bool cd_image_switch_sub_image(cd_image_t* i, u32 index, Error* error)
{
  if (i->vtbl->switch_sub_image)
    return i->vtbl->switch_sub_image(i, index, error);
  Error_set_string(error, "Image does not support sub-images.");
  return false;
}

bool cd_image_generate_subq(const cd_image_t* i, cd_image_subq_t* out, cd_image_lba_t lba)
{
  const cd_image_index_t* idx = cd_image_get_index_for_disc_position(i, lba);
  if (!idx)
    return false;
  const u32 off = lba - idx->start_lba_on_disc;
  cd_image_generate_subq_from_index(i, out, idx, off);
  return true;
}

void cd_image_generate_subq_from_index(const cd_image_t* i, cd_image_subq_t* out,
                                       const cd_image_index_t* index, u32 index_offset)
{
  out->control_bits = index->control.bits;
   out->track_number_bcd = (index->track_number <= i->track_count)
                            ? BinaryToBCD((u8)index->track_number) 
                            : (u8)index->track_number;
  out->index_number_bcd = BinaryToBCD((u8)index->index_number);

  cd_image_position_t relative;
  if (index->is_pregap) {
    /* Position counts down to the end of the pregap. */
    relative = cd_image_position_from_lba(index->length - index_offset - 1u);
  } else {
    /* Count up from the start of the track. */
    relative = cd_image_position_from_lba(index->start_lba_in_track + index_offset);
  }
  cd_image_position_to_bcd(relative, &out->relative_minute_bcd, &out->relative_second_bcd, &out->relative_frame_bcd);

  out->reserved = 0;

  const cd_image_position_t absolute = cd_image_position_from_lba(index->start_lba_on_disc + index_offset);
  cd_image_position_to_bcd(absolute, &out->absolute_minute_bcd, &out->absolute_second_bcd, &out->absolute_frame_bcd);
  out->crc = cd_image_subq_compute_crc((const u8*)out);
}

void cd_image_add_lead_out_index(cd_image_t* i)
{
  Assert(i->index_count > 0);
  const cd_image_index_t* last = &i->indices[i->index_count - 1u];

  cd_image_index_t lead_out;
  memset(&lead_out, 0, sizeof(lead_out));
  lead_out.start_lba_on_disc = last->start_lba_on_disc + last->length;
  lead_out.length            = CD_IMAGE_LEAD_OUT_SECTOR_COUNT;
  lead_out.track_number      = CD_IMAGE_LEAD_OUT_TRACK_NUMBER;
  lead_out.index_number      = 0;
  lead_out.control.bits      = last->control.bits;

  cd_image_index_t* slot = cd_image_push_index(i);
  *slot = lead_out;
}

cd_image_t* cd_image_open(const char* path, Error* error)
{
  if (cd_image_is_device_name(path))
    return cd_image_open_device(path, error);

  const char* dot = strrchr(path, '.');
  if (!dot) {
    if (strncmp(path, "/dev/", 5) == 0)
      Error_set_string_fmt(error, "Device path '%s' is not an accessible CD-ROM", path);
    else
      Error_set_string_fmt(error, "No extension on filename '%s'", path);
    return NULL;
  }
  const char* ext = dot + 1;

  cd_image_t* image = NULL;
  if (strcasecmp(ext, "cue") == 0)
    image = cd_image_open_cue(path, error);
  else if (strcasecmp(ext, "bin") == 0 || strcasecmp(ext, "img") == 0 ||
           strcasecmp(ext, "iso") == 0)
    image = cd_image_open_bin(path, error);
  else if (strcasecmp(ext, "ecm") == 0)
    image = cd_image_open_ecm(path, error);
  else if (strcasecmp(ext, "m3u") == 0)
    image = cd_image_open_m3u(path, error);
  else if (strcasecmp(ext, "chd") == 0)
    image = cd_image_open_chd(path, error);
  else if (strcasecmp(ext, "mds") == 0)
    image = cd_image_open_mds(path, error);
  else if (strcasecmp(ext, "ccd") == 0)
    image = cd_image_open_ccd(path, error);
  else if (strcasecmp(ext, "pbp") == 0)
    image = cd_image_open_pbp(path, error);
  else {
    Error_set_string_fmt(error, "Unsupported extension '%s' on filename '%s'", ext, path);
    return NULL;
  }

  if (!image) return NULL;

  small_string_t ppf_path;
  small_string_init(&ppf_path);
  path_change_extension_cstr(&ppf_path, path, "ppf");
  if (fs_file_exists(small_string_c_str(&ppf_path))) {
    /* cd_image_overlay_ppf takes ownership of `image` immediately (success
     * AND failure paths destroy it via cdimage_ppf_destroy).  Returning
     * NULL means the open as a whole failed. */
    cd_image_t* patched = cd_image_overlay_ppf(small_string_c_str(&ppf_path), image, error);
    if (!patched) {
      small_string_destroy(&ppf_path);
      return NULL;
    }
    image = patched;
  }
  small_string_destroy(&ppf_path);

  return image;
}
