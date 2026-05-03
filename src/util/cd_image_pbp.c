/*
 * CDImagePBP: read PSP-style PBP / EBOOT.PBP for decrypted PS1 EBOOT
 * images.  The PSAR section's `PSISOIMG0000` ISO header pre-stores 32256
 * fixed-size entries describing 16-sector raw-deflate blocks (16 ×
 * 2352 = 37632 bytes per decompressed block).  No libkirk, no PSP firmware
 * keys; encrypted PSP-game eboots (`~PSP\x01\x00\x00\x00` magic) are
 * rejected at the magic check.
 *
 * Multi-disc support via `PSTITLEIMG000000` header: 5 disc offsets at
 * data_psar_offset + 0x200; sub-image vtable wired (used by frontend disc
 * swap hotkeys F3/F4 like cd_image_m3u.c).
 */

#include "cd_image.h"

#include "common/bcdutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

LOG_CHANNEL(CDImage);

#define PBP_HEADER_OFFSET_COUNT       8u
#define PBP_TOC_NUM_ENTRIES           102u
#define PBP_BLOCK_TABLE_NUM_ENTRIES   32256u
#define PBP_DISC_TABLE_NUM_ENTRIES    5u
#define PBP_DECOMPRESSED_BLOCK_SIZE   37632u   /* 2352 * 16 */
#define PBP_RAW_SECTOR_SIZE           ((u32)CD_IMAGE_RAW_SECTOR_SIZE)

#pragma pack(push, 1)
typedef struct {
  u8  magic[4];   /* "\0PBP" */
  u32 version;
  u32 param_sfo_offset;
  u32 icon0_png_offset;
  u32 icon1_png_offset;
  u32 pic0_png_offset;
  u32 pic1_png_offset;
  u32 snd0_at3_offset;
  u32 data_psp_offset;
  u32 data_psar_offset;
} pbp_header_t;
_Static_assert(sizeof(pbp_header_t) == 0x28, "pbp_header_t size");

typedef struct {
  u8  magic[4];   /* "\0PSF" */
  u32 version;
  u32 key_table_offset;
  u32 data_table_offset;
  u32 num_table_entries;
} sfo_header_t;
_Static_assert(sizeof(sfo_header_t) == 0x14, "sfo_header_t size");

typedef struct {
  u16 key_offset;
  u16 data_type;
  u32 data_size;
  u32 data_total_size;
  u32 data_offset;
} sfo_index_table_entry_t;
_Static_assert(sizeof(sfo_index_table_entry_t) == 0x10, "sfo_index_table_entry_t size");

typedef struct {
  u32 offset;
  u16 size;
  u16 marker;
  u8  checksum[0x10];
  u64 padding;
} block_table_entry_t;
_Static_assert(sizeof(block_table_entry_t) == 0x20, "block_table_entry_t size");

typedef struct {
  u8 type;
  u8 unknown;
  u8 point;
  u8 pregap_m, pregap_s, pregap_f;
  u8 zero;
  u8 userdata_m, userdata_s, userdata_f;
} pbp_toc_entry_t;
_Static_assert(sizeof(pbp_toc_entry_t) == 0x0A, "pbp_toc_entry_t size");
#pragma pack(pop)

typedef struct {
  u32 offset;
  u16 size;
} pbp_block_info_t;

/* Tagged-union SFO value. */
typedef enum { PBP_SFO_KIND_NONE, PBP_SFO_KIND_STR, PBP_SFO_KIND_U32 } pbp_sfo_kind_t;
typedef struct {
  char*           key;        /* heap, NUL-terminated */
  pbp_sfo_kind_t  kind;
  union {
    char* str;                /* heap, NUL-terminated */
    u32   u;
  } v;
} pbp_sfo_entry_t;

typedef struct cd_image_pbp {
  cd_image_t base;            /* MUST be first */

  FILE*       file;
  pbp_header_t pbp_header;
  sfo_header_t sfo_header;
  sfo_index_table_entry_t* sfo_index;
  size_t      sfo_index_count;

  pbp_sfo_entry_t* sfo_table;
  size_t           sfo_table_count;

  u32         disc_offsets[PBP_DISC_TABLE_NUM_ENTRIES];
  u32         disc_count;
  u32         current_disc;

  pbp_block_info_t  blockinfo[PBP_BLOCK_TABLE_NUM_ENTRIES];

  u32         current_block;          /* (u32)-1 sentinel */
  u8          decompressed[PBP_DECOMPRESSED_BLOCK_SIZE];
  u8*         compressed;
  size_t      compressed_cap;

  z_stream    inflate_stream;
  bool        inflate_initialised;
} cd_image_pbp_t;

static const char* pbp_basename(const char* path)
{
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static void sfo_table_clear(cd_image_pbp_t* p)
{
  for (size_t i = 0; i < p->sfo_table_count; i++) {
    free(p->sfo_table[i].key);
    if (p->sfo_table[i].kind == PBP_SFO_KIND_STR) free(p->sfo_table[i].v.str);
  }
  free(p->sfo_table);
  p->sfo_table = NULL;
  p->sfo_table_count = 0;
}

static const pbp_sfo_entry_t* sfo_table_find(const cd_image_pbp_t* p, const char* key)
{
  for (size_t i = 0; i < p->sfo_table_count; i++) {
    if (strcmp(p->sfo_table[i].key, key) == 0) return &p->sfo_table[i];
  }
  return NULL;
}

static bool load_pbp_header(cd_image_pbp_t* p, Error* error)
{
  if (fread(&p->pbp_header, sizeof(p->pbp_header), 1, p->file) != 1) {
    Error_set_errno_prefix(error, "fread() for PBP header failed: ", errno);
    return false;
  }
  if (memcmp(p->pbp_header.magic, "\0PBP", 4) != 0) {
    Error_set_string(error, "PBP magic number mismatch");
    return false;
  }
  return true;
}

static bool load_sfo_header(cd_image_pbp_t* p, Error* error)
{
  if (fseek(p->file, (long)p->pbp_header.param_sfo_offset, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "Failed to seek to SFO header: ", errno);
    return false;
  }
  if (fread(&p->sfo_header, sizeof(p->sfo_header), 1, p->file) != 1) {
    Error_set_errno_prefix(error, "fread() for SFO header failed: ", errno);
    return false;
  }
  if (memcmp(p->sfo_header.magic, "\0PSF", 4) != 0) {
    Error_set_string(error, "SFO magic number mismatch");
    return false;
  }
  return true;
}

static bool load_sfo_index_table(cd_image_pbp_t* p, Error* error)
{
  free(p->sfo_index);
  p->sfo_index = NULL;
  p->sfo_index_count = p->sfo_header.num_table_entries;
  if (p->sfo_index_count == 0) return true;

  p->sfo_index = (sfo_index_table_entry_t*)calloc(p->sfo_index_count, sizeof(*p->sfo_index));
  if (!p->sfo_index) abort();

  if (fseek(p->file, (long)(p->pbp_header.param_sfo_offset + sizeof(p->sfo_header)), SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "Failed to seek to SFO index table: ", errno);
    return false;
  }
  if (fread(p->sfo_index, sizeof(*p->sfo_index), p->sfo_index_count, p->file) != p->sfo_index_count) {
    Error_set_errno_prefix(error, "fread() for SFO index table failed: ", errno);
    return false;
  }
  return true;
}

static char* dup_cstring_at(FILE* fp, long abs_offset, size_t max_len, Error* error)
{
  if (fseek(fp, abs_offset, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "fseek() failed: ", errno);
    return NULL;
  }
  char* buf = (char*)malloc(max_len);
  if (!buf) abort();
  if (!fgets(buf, (int)max_len, fp)) {
    Error_set_errno_prefix(error, "fgets() failed: ", errno);
    free(buf);
    return NULL;
  }
  /* fgets stops at newline OR end-of-stream; the SFO format is NUL-terminated
   * and fgets-on-binary will read until first '\n' or eof.  Strip any
   * embedded '\n' for safety, then re-NUL on first NUL or end. */
  for (size_t i = 0; i < max_len; i++) {
    if (buf[i] == '\n') { buf[i] = '\0'; break; }
    if (buf[i] == '\0') break;
    if (i == max_len - 1) buf[i] = '\0';
  }
  return buf;
}

static bool load_sfo_table(cd_image_pbp_t* p, Error* error)
{
  sfo_table_clear(p);
  if (p->sfo_index_count == 0) return true;

  p->sfo_table = (pbp_sfo_entry_t*)calloc(p->sfo_index_count, sizeof(*p->sfo_table));
  if (!p->sfo_table) abort();

  for (size_t i = 0; i < p->sfo_index_count; i++) {
    const u32 abs_key_offset = p->pbp_header.param_sfo_offset
                             + p->sfo_header.key_table_offset
                             + p->sfo_index[i].key_offset;
    const u32 abs_data_offset = p->pbp_header.param_sfo_offset
                              + p->sfo_header.data_table_offset
                              + p->sfo_index[i].data_offset;

    char* key = dup_cstring_at(p->file, (long)abs_key_offset, 32u, error);
    if (!key) return false;

    pbp_sfo_entry_t* slot = &p->sfo_table[p->sfo_table_count];
    slot->key = key;

    if (p->sfo_index[i].data_type == 0x0204) {
      /* NUL-terminated UTF-8 string */
      slot->kind = PBP_SFO_KIND_STR;
      slot->v.str = dup_cstring_at(p->file, (long)abs_data_offset, p->sfo_index[i].data_size + 1u, error);
      if (!slot->v.str) return false;
      p->sfo_table_count++;
    } else if (p->sfo_index[i].data_type == 0x0404) {
      /* uint32 */
      if (fseek(p->file, (long)abs_data_offset, SEEK_SET) != 0) {
        Error_set_errno_prefix(error, "fseek() failed: ", errno);
        free(key);
        slot->key = NULL;
        return false;
      }
      u32 val;
      if (fread(&val, sizeof(val), 1, p->file) != 1) {
        Error_set_errno_prefix(error, "fread() failed: ", errno);
        free(key);
        slot->key = NULL;
        return false;
      }
      slot->kind = PBP_SFO_KIND_U32;
      slot->v.u = val;
      p->sfo_table_count++;
    } else if (p->sfo_index[i].data_type == 0x0004) {
      Error_set_string_fmt(error, "Unhandled special-mode UTF-8 type for SFO entry %zu", i);
      free(key);
      slot->key = NULL;
      return false;
    } else {
      Error_set_string_fmt(error, "Unhandled SFO data type 0x%04X for entry %zu",
                           p->sfo_index[i].data_type, i);
      free(key);
      slot->key = NULL;
      return false;
    }
  }
  return true;
}

static bool is_valid_eboot(cd_image_pbp_t* p, Error* error)
{
  const pbp_sfo_entry_t* bootable = sfo_table_find(p, "BOOTABLE");
  if (!bootable) {
    Error_set_string(error, "No BOOTABLE value found");
    return false;
  }
  if (bootable->kind != PBP_SFO_KIND_U32 || bootable->v.u != 1) {
    Error_set_string(error, "Invalid BOOTABLE value");
    return false;
  }
  const pbp_sfo_entry_t* category = sfo_table_find(p, "CATEGORY");
  if (!category) {
    Error_set_string(error, "No CATEGORY value found");
    return false;
  }
  if (category->kind != PBP_SFO_KIND_STR || strcmp(category->v.str, "ME") != 0) {
    Error_set_string(error, "Invalid CATEGORY value");
    return false;
  }
  return true;
}

static bool init_decompression_stream(cd_image_pbp_t* p)
{
  if (p->inflate_initialised) {
    inflateEnd(&p->inflate_stream);
    p->inflate_initialised = false;
  }
  memset(&p->inflate_stream, 0, sizeof(p->inflate_stream));
  p->inflate_stream.next_in = Z_NULL;
  p->inflate_stream.avail_in = 0;
  p->inflate_stream.zalloc = Z_NULL;
  p->inflate_stream.zfree = Z_NULL;
  p->inflate_stream.opaque = Z_NULL;
  if (inflateInit2(&p->inflate_stream, -MAX_WBITS) != Z_OK) return false;
  p->inflate_initialised = true;
  return true;
}

static bool decompress_block(cd_image_pbp_t* p, const pbp_block_info_t* bi)
{
  if (fseek(p->file, (long)bi->offset, SEEK_SET) != 0) return false;

  /* Compression level 0 → compressed_size == decompressed_size: read raw. */
  if (bi->size == PBP_DECOMPRESSED_BLOCK_SIZE) {
    return fread(p->decompressed, 1, PBP_DECOMPRESSED_BLOCK_SIZE, p->file) == PBP_DECOMPRESSED_BLOCK_SIZE;
  }

  if (bi->size > p->compressed_cap) {
    p->compressed = (u8*)realloc(p->compressed, bi->size);
    if (!p->compressed) abort();
    p->compressed_cap = bi->size;
  }
  if (fread(p->compressed, 1, bi->size, p->file) != bi->size) return false;

  p->inflate_stream.next_in = p->compressed;
  p->inflate_stream.avail_in = (uInt)bi->size;
  p->inflate_stream.next_out = p->decompressed;
  p->inflate_stream.avail_out = (uInt)PBP_DECOMPRESSED_BLOCK_SIZE;
  if (inflateReset(&p->inflate_stream) != Z_OK) return false;
  if (inflate(&p->inflate_stream, Z_FINISH) != Z_STREAM_END) return false;
  return true;
}

static bool open_disc(cd_image_pbp_t* p, u32 index, Error* error)
{
  if (index >= p->disc_count) {
    Error_set_string_fmt(error, "File does not contain disc %u", index + 1);
    return false;
  }

  p->current_block = (u32)-1;
  memset(p->blockinfo, 0, sizeof(p->blockinfo));
  memset(p->decompressed, 0, sizeof(p->decompressed));

  const u32 iso_header_start = p->disc_offsets[index];
  if (fseek(p->file, (long)iso_header_start, SEEK_SET) != 0) return false;

  char iso_header_magic[12] = { 0 };
  if (fread(iso_header_magic, sizeof(iso_header_magic), 1, p->file) != 1) return false;
  if (memcmp(iso_header_magic, "PSISOIMG0000", 12) != 0) {
    ERROR_LOG("ISO header magic number mismatch");
    Error_set_string(error, "ISO header magic number mismatch");
    return false;
  }

  /* Reject encrypted */
  u32 pgd_magic;
  if (fseek(p->file, (long)(iso_header_start + 0x400u), SEEK_SET) != 0) return false;
  if (fread(&pgd_magic, sizeof(pgd_magic), 1, p->file) != 1) return false;
  if (pgd_magic == 0x44475000u) {
    Error_set_string(error, "Encrypted PBP images are not supported");
    return false;
  }

  /* TOC (102 entries × 0x0A = 0x648 bytes at 0x800). */
  pbp_toc_entry_t toc[PBP_TOC_NUM_ENTRIES];
  if (fseek(p->file, (long)(iso_header_start + 0x800u), SEEK_SET) != 0) return false;
  if (fread(toc, sizeof(pbp_toc_entry_t), PBP_TOC_NUM_ENTRIES, p->file) != PBP_TOC_NUM_ENTRIES) return false;

  /* iso_offset at 0xBFC. */
  if (fseek(p->file, (long)(iso_header_start + 0xBFCu), SEEK_SET) != 0) return false;
  u32 iso_offset;
  if (fread(&iso_offset, sizeof(iso_offset), 1, p->file) != 1) return false;

  /* Block table at 0x4000. */
  if (fseek(p->file, (long)(iso_header_start + 0x4000u), SEEK_SET) != 0) return false;
  for (u32 i = 0; i < PBP_BLOCK_TABLE_NUM_ENTRIES; i++) {
    block_table_entry_t bte;
    if (fread(&bte, sizeof(bte), 1, p->file) != 1) return false;
    p->blockinfo[i].offset = (bte.size != 0) ? (iso_header_start + iso_offset + bte.offset) : 0;
    p->blockinfo[i].size   = bte.size;
  }

  /* TOC sanity: first three entries are the information tracks (A0/A1/A2). */
  if (toc[0].point != 0xA0 || toc[1].point != 0xA1 || toc[2].point != 0xA2) {
    Error_set_string(error, "Invalid points on information tracks");
    return false;
  }

  const u8 first_track = PackedBCDToBinary(toc[0].userdata_m);
  const u8 last_track  = PackedBCDToBinary(toc[1].userdata_m);
  const cd_image_lba_t sectors_on_file = cd_image_position_to_lba(
      cd_image_position_from_bcd(toc[2].userdata_m, toc[2].userdata_s, toc[2].userdata_f));

  if (first_track != 1 || last_track < first_track) {
    Error_set_string(error, "Invalid starting track number or track count");
    return false;
  }

  cd_image_clear_toc(&p->base);
  p->base.lba_count = sectors_on_file;
  cd_image_lba_t track1_pregap_frames = 0;

  for (u32 curr_track = 1; curr_track <= last_track; curr_track++) {
    const pbp_toc_entry_t* t = &toc[curr_track + 2];
    const u8 track_num = PackedBCDToBinary(t->point);
    if (track_num != curr_track)
      WARNING_LOG("Mismatched TOC track number, expected %u but got %u", curr_track, track_num);

    const bool is_audio_track = (t->type == 0x01);
    const bool is_first_track = (curr_track == 1);
    const bool is_last_track  = (curr_track == last_track);
    const cd_image_track_mode_t track_mode = is_audio_track
        ? CD_IMAGE_TRACK_MODE_AUDIO : CD_IMAGE_TRACK_MODE_MODE2_RAW;
    const u32 track_sector_size = cd_image_get_bytes_per_sector(track_mode);

    cd_image_subq_control_t track_control = { 0 };
    cd_image_subq_control_set_data(&track_control, !is_audio_track);

    cd_image_lba_t pregap_start =
        cd_image_position_to_lba(cd_image_position_from_bcd(t->pregap_m, t->pregap_s, t->pregap_f));
    cd_image_lba_t userdata_start =
        cd_image_position_to_lba(cd_image_position_from_bcd(t->userdata_m, t->userdata_s, t->userdata_f));

    cd_image_lba_t pregap_frames;
    u32 pregap_sector_size;

    if (userdata_start < pregap_start) {
      if (!is_first_track || is_audio_track) {
        Error_set_string_fmt(error, "Invalid TOC entry at index %u, user data (%u) should not start before pregap (%u)",
                             curr_track, userdata_start, pregap_start);
        return false;
      }
      WARNING_LOG("Invalid TOC entry %u, assuming pregap not in file", curr_track);
      pregap_start = 0;
      pregap_frames = userdata_start;
      pregap_sector_size = 0;
    } else {
      pregap_frames = userdata_start - pregap_start;
      pregap_sector_size = track_sector_size;
      if (is_first_track) p->base.lba_count += pregap_frames;
    }

    if (is_first_track) track1_pregap_frames = pregap_frames;

    cd_image_index_t pregap = { 0 };
    pregap.file_offset = is_first_track
        ? 0u
        : ((u64)(pregap_start - track1_pregap_frames) * (u64)pregap_sector_size);
    pregap.file_index = 0;
    pregap.file_sector_size = pregap_sector_size;
    pregap.start_lba_on_disc = pregap_start;
    pregap.track_number = curr_track;
    pregap.index_number = 0;
    pregap.start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
    pregap.length = pregap_frames;
    pregap.mode = track_mode;
    pregap.submode = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    pregap.control = track_control;
    pregap.is_pregap = true;
    *cd_image_push_index(&p->base) = pregap;

    cd_image_index_t userdata = { 0 };
    userdata.file_offset = (u64)(userdata_start - track1_pregap_frames) * (u64)track_sector_size;
    userdata.file_index = 0;
    userdata.file_sector_size = track_sector_size;
    userdata.start_lba_on_disc = userdata_start;
    userdata.track_number = curr_track;
    userdata.index_number = 1;
    userdata.start_lba_in_track = 0;
    userdata.mode = track_mode;
    userdata.submode = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    userdata.control = track_control;
    userdata.is_pregap = false;

    if (is_last_track) {
      if (userdata_start >= p->base.lba_count) {
        Error_set_string_fmt(error, "Last user data index for TOC entry %u should not be 0 or less in length", curr_track);
        return false;
      }
      userdata.length = p->base.lba_count - userdata_start;
    } else {
      const pbp_toc_entry_t* nt = &toc[curr_track + 3];
      const cd_image_lba_t nstart =
          cd_image_position_to_lba(cd_image_position_from_bcd(nt->pregap_m, nt->pregap_s, nt->pregap_f));
      const u8 nnum = PackedBCDToBinary(nt->point);
      if (nnum != curr_track + 1 || nstart < userdata_start) {
        Error_set_string_fmt(error, "Unable to calculate user data index length for TOC entry %u", curr_track);
        return false;
      }
      userdata.length = nstart - userdata_start;
    }
    *cd_image_push_index(&p->base) = userdata;

    cd_image_track_t* tr = cd_image_push_track(&p->base);
    tr->track_number = curr_track;
    tr->start_lba    = userdata_start;
    tr->first_index  = 2u * curr_track - 1u;
    tr->length       = pregap.length + userdata.length;
    tr->mode         = track_mode;
    tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    tr->control      = track_control;
  }

  cd_image_add_lead_out_index(&p->base);

  if (!init_decompression_stream(p)) {
    Error_set_string(error, "Failed to initialize zlib decompression stream");
    return false;
  }

  p->current_disc = index;
  return cd_image_seek_track_msf(&p->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

static bool cdimage_pbp_read_sector_from_index(cd_image_t* self, void* buffer,
                                               const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  const u32 offset_in_file = (u32)index->file_offset + (lba_in_index * index->file_sector_size);
  const u32 offset_in_block = offset_in_file % PBP_DECOMPRESSED_BLOCK_SIZE;
  const u32 requested_block = offset_in_file / PBP_DECOMPRESSED_BLOCK_SIZE;

  if (requested_block >= PBP_BLOCK_TABLE_NUM_ENTRIES) return false;
  const pbp_block_info_t* bi = &p->blockinfo[requested_block];
  if (bi->size == 0) return false;

  if (p->current_block != requested_block) {
    if (!decompress_block(p, bi)) return false;
    p->current_block = requested_block;
  }

  memcpy(buffer, &p->decompressed[offset_in_block], PBP_RAW_SECTOR_SIZE);
  return true;
}

static s64 cdimage_pbp_get_size_on_disk(const cd_image_t* self)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  return fs_fsize64(p->file, NULL);
}

static bool cdimage_pbp_has_sub_images(const cd_image_t* self)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  return p->disc_count > 1u;
}

static u32 cdimage_pbp_get_sub_image_count(const cd_image_t* self)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  return p->disc_count;
}

static u32 cdimage_pbp_get_current_sub_image(const cd_image_t* self)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  return p->current_disc;
}

static bool cdimage_pbp_switch_sub_image(cd_image_t* self, u32 index, Error* error)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  if (index >= p->disc_count) return false;
  const u32 old = p->current_disc;
  if (!open_disc(p, index, error)) {
    if (!open_disc(p, old, NULL)) abort(); /* Panic if rollback fails */
    return false;
  }
  return true;
}

static char* cdimage_pbp_get_sub_image_title(const cd_image_t* self, u32 index)
{
  const cd_image_pbp_t* p = (const cd_image_pbp_t*)self;
  const pbp_sfo_entry_t* title = sfo_table_find(p, "TITLE");
  if (!title || title->kind != PBP_SFO_KIND_STR || !title->v.str || title->v.str[0] == '\0') return NULL;
  /* "<title> (Disc N)" */
  size_t need = strlen(title->v.str) + 16u;
  char* out = (char*)malloc(need);
  if (!out) abort();
  snprintf(out, need, "%s (Disc %u)", title->v.str, index + 1u);
  return out;
}

static void cdimage_pbp_destroy(cd_image_t* self)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)self;
  if (p->file) { fclose(p->file); p->file = NULL; }
  if (p->inflate_initialised) { inflateEnd(&p->inflate_stream); p->inflate_initialised = false; }
  free(p->compressed); p->compressed = NULL; p->compressed_cap = 0;
  free(p->sfo_index); p->sfo_index = NULL; p->sfo_index_count = 0;
  sfo_table_clear(p);
}

static const cd_image_vtable_t s_pbp_vtable = {
   .read_sector_from_index = cdimage_pbp_read_sector_from_index,
  .get_size_on_disk       = cdimage_pbp_get_size_on_disk,
  .destroy                = cdimage_pbp_destroy,
  .has_sub_images         = cdimage_pbp_has_sub_images,
  .get_sub_image_count    = cdimage_pbp_get_sub_image_count,
  .get_current_sub_image  = cdimage_pbp_get_current_sub_image,
  .switch_sub_image       = cdimage_pbp_switch_sub_image,
  .get_sub_image_title    = cdimage_pbp_get_sub_image_title, 
};

cd_image_t* cd_image_open_pbp(const char* path, Error* error)
{
  cd_image_pbp_t* p = (cd_image_pbp_t*)malloc(sizeof(*p));
  if (!p) abort();
  memset(p, 0, sizeof(*p));
  cd_image_init(&p->base, &s_pbp_vtable);
  p->current_block = (u32)-1;

  p->file = fs_open_shared_file(path, "rb", FS_FILE_SHARE_DENY_WRITE, error);
  if (!p->file) {
    Error_set_string_fmt(error, "Failed to open '%s'", pbp_basename(path));
    cd_image_destroy(&p->base);
    return NULL;
  }

  if (!load_pbp_header(p, error)         ||
      !load_sfo_header(p, error)         ||
      !load_sfo_index_table(p, error)    ||
      !load_sfo_table(p, error)          ||
      !is_valid_eboot(p, error))
  {
    cd_image_destroy(&p->base);
    return NULL;
  }

  /* Parse PSAR. */
  if (fseek(p->file, (long)p->pbp_header.data_psar_offset, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "Failed to seek to psar offset: ", errno);
    cd_image_destroy(&p->base);
    return NULL;
  }
  char data_psar_magic[16] = { 0 };
  if (fread(data_psar_magic, sizeof(data_psar_magic), 1, p->file) != 1) {
    Error_set_errno_prefix(error, "Failed to read data_psar_magic: ", errno);
    cd_image_destroy(&p->base);
    return NULL;
  }

  if (memcmp(data_psar_magic, "PSTITLEIMG000000", 16) == 0) {
    /* Multi-disc */
    if (fseek(p->file, (long)(p->pbp_header.data_psar_offset + 0x200u), SEEK_SET) != 0) {
      Error_set_errno_prefix(error, "Failed to seek to multi-disc header: ", errno);
      cd_image_destroy(&p->base);
      return NULL;
    }
    u32 disc_table[PBP_DISC_TABLE_NUM_ENTRIES] = { 0 };
    if (fread(disc_table, sizeof(u32), PBP_DISC_TABLE_NUM_ENTRIES, p->file) != PBP_DISC_TABLE_NUM_ENTRIES) {
      Error_set_errno_prefix(error, "Failed to read disc_table: ", errno);
      cd_image_destroy(&p->base);
      return NULL;
    }
    if (disc_table[0] == 0x44475000u) {  /* "\0PGD" */
      Error_set_string(error, "Encrypted PBP images are not supported");
      cd_image_destroy(&p->base);
      return NULL;
    }
    for (u32 i = 0; i < PBP_DISC_TABLE_NUM_ENTRIES; i++) {
      if (disc_table[i] == 0) break;
      p->disc_offsets[p->disc_count++] = p->pbp_header.data_psar_offset + disc_table[i];
    }
    if (p->disc_count < 1u) {
      Error_set_string(error, "Invalid number of discs in multi-disc PBP file");
      cd_image_destroy(&p->base);
      return NULL;
    }
  } else {
    p->disc_offsets[0] = p->pbp_header.data_psar_offset;
    p->disc_count = 1;
  }

  if (!open_disc(p, 0, error)) {
    cd_image_destroy(&p->base);
    return NULL;
  }
  return &p->base;
}
