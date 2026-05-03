/*
 * CDImageCHD: read MAME CHD (Compressed Hunks of Data) images via libchdr.
 *
 * One CHD file maps the entire disc; the file is divided into "hunks" of
 * fixed size, each storing N PSX sectors at 2352 raw + 96 subcode = 2448
 * bytes per sector.  We keep one decompressed hunk in memory; reads first
 * compute (hunk_index, hunk_offset) for the requested LBA and decompress on
 * miss.  Track metadata strings live as CHD metadata blobs which we sscanf.
 *
 * Parent CHD chain: a CHD may delta against another (the "parent") via
 * SHA1.  When chd_open_file() returns CHDERR_REQUIRES_PARENT we scan the
 * source-file's directory for *.chd, header-match, and recurse - up to
 * mutex-guarded cache; we skip that - first-load parent resolution is
 * already O(parents * dir_size) and that's fine for emulator usage.
 */

#include "cd_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/small_string.h"

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_CHANNEL(CDImage);

#define CHD_CD_SECTOR_DATA_SIZE  (2352u + 96u)
#define CHD_CD_TRACK_ALIGNMENT   4u
#define MAX_PARENTS              32u

typedef struct cd_image_chd {
  cd_image_t base;          /* MUST be first */

  chd_file*  chd;
  u32        hunk_size;
  u32        sectors_per_hunk;
  u8*        hunk_buffer;        /* heap, hunk_size bytes */
  u32        current_hunk_index; /* (u32)-1 sentinel when nothing cached */
  bool       precached;
} cd_image_chd_t;

static bool parse_track_mode_string(const char* str, cd_image_track_mode_t* out)
{
  if (strcmp(str, "MODE2_FORM_MIX") == 0) { *out = CD_IMAGE_TRACK_MODE_MODE2_FORM_MIX; return true; }
  if (strcmp(str, "MODE2_FORM1")    == 0) { *out = CD_IMAGE_TRACK_MODE_MODE2_FORM1;    return true; }
  if (strcmp(str, "MODE2_FORM2")    == 0) { *out = CD_IMAGE_TRACK_MODE_MODE2_FORM2;    return true; }
  if (strcmp(str, "MODE2_RAW")      == 0) { *out = CD_IMAGE_TRACK_MODE_MODE2_RAW;      return true; }
  if (strcmp(str, "MODE1_RAW")      == 0) { *out = CD_IMAGE_TRACK_MODE_MODE1_RAW;      return true; }
  if (strcmp(str, "MODE1")          == 0) { *out = CD_IMAGE_TRACK_MODE_MODE1;          return true; }
  if (strcmp(str, "MODE2")          == 0) { *out = CD_IMAGE_TRACK_MODE_MODE2;          return true; }
  if (strcmp(str, "AUDIO")          == 0) { *out = CD_IMAGE_TRACK_MODE_AUDIO;          return true; }
  return false;
}

/* CD_SUB_* values come from libchdr's enum and are == cd_image_subchannel_mode_t
 * ordering.  Static-asserted so a future renumbering breaks the build. */
_Static_assert((u32)CD_SUB_NONE            == (u32)CD_IMAGE_SUBCHANNEL_MODE_NONE,            "CD_SUB_NONE matches");
_Static_assert((u32)CD_SUB_RAW_INTERLEAVED == (u32)CD_IMAGE_SUBCHANNEL_MODE_RAW_INTERLEAVED, "CD_SUB_RAW_INTERLEAVED matches");
_Static_assert((u32)CD_SUB_RAW             == (u32)CD_IMAGE_SUBCHANNEL_MODE_RAW,             "CD_SUB_RAW matches");

static u64 align_up_u64(u64 v, u64 align)
{
  return (v + (align - 1u)) & ~(align - 1u);
}

/* Audio sectors come out of libchdr in big-endian byte order; the rest of
 * the emulator expects native little-endian. Swap byte pairs in-flight. */
static void copy_and_swap_audio(void* dst_ptr, const u8* src_ptr)
{
  u8* dst = (u8*)dst_ptr;
  for (u32 k = 0; k < 2352u; k += 2u) {
    dst[k]      = src_ptr[k + 1u];
    dst[k + 1u] = src_ptr[k];
  }
}

static bool update_hunk_buffer(cd_image_chd_t* img, const cd_image_index_t* index,
                               cd_image_lba_t lba_in_index, u32* out_hunk_offset)
{
  const u32 disc_frame  = (u32)index->file_offset + lba_in_index;
  const u32 hunk_index  = disc_frame / img->sectors_per_hunk;
  const u32 hunk_offset = (disc_frame % img->sectors_per_hunk) * CHD_CD_SECTOR_DATA_SIZE;
  DebugAssert((img->hunk_size - hunk_offset) >= CHD_CD_SECTOR_DATA_SIZE);
  *out_hunk_offset = hunk_offset;

  if (img->current_hunk_index == hunk_index)
    return true;

  const chd_error err = chd_read(img->chd, hunk_index, img->hunk_buffer);
  if (err != CHDERR_NONE) {
    ERROR_LOG("chd_read(%u) failed: %s", hunk_index, chd_error_string(err));
    img->current_hunk_index = (u32)-1;
    return false;
  }

  img->current_hunk_index = hunk_index;
  return true;
}

static bool cdimage_chd_read_sector_from_index(cd_image_t* self, void* buffer,
                                       const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_chd_t* img = (cd_image_chd_t*)self;
  u32 hunk_offset;
  if (!update_hunk_buffer(img, index, lba_in_index, &hunk_offset))
    return false;

  const u8* src = img->hunk_buffer + hunk_offset;
  if (index->mode == CD_IMAGE_TRACK_MODE_AUDIO)
    copy_and_swap_audio(buffer, src);
  else
    memcpy(buffer, src, CD_IMAGE_RAW_SECTOR_SIZE);
  return true;
}

static bool cdimage_chd_read_subchannel_q(cd_image_t* self, cd_image_subq_t* subq,
                                  const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_chd_t* img = (cd_image_chd_t*)self;

  if (index->submode == CD_IMAGE_SUBCHANNEL_MODE_NONE) {
    cd_image_generate_subq_from_index(self, subq, index, lba_in_index);
    return true;
  }

  u32 hunk_offset;
  if (!update_hunk_buffer(img, index, lba_in_index, &hunk_offset))
    return false;

  u8 deinterleaved[CD_IMAGE_ALL_SUBCODE_SIZE];
  const u8* raw = img->hunk_buffer + hunk_offset + CD_IMAGE_RAW_SECTOR_SIZE;
  const u8* real = raw;
  if (index->submode == CD_IMAGE_SUBCHANNEL_MODE_RAW_INTERLEAVED) {
    cd_image_deinterleave_subcode(raw, deinterleaved);
    real = deinterleaved;
  }
  /* Q is the second 12-byte channel of the 8 (P,Q,R,S,T,U,V,W). */
  memcpy(subq, real + CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME, CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME);
  return true;
}

static bool cdimage_chd_has_subchannel_data(const cd_image_t* self)
{
  return self->track_count > 0u && self->tracks[0].submode != CD_IMAGE_SUBCHANNEL_MODE_NONE;
}

static cd_image_precache_result_t cdimage_chd_precache(cd_image_t* self, Error* error)
{
  cd_image_chd_t* img = (cd_image_chd_t*)self;
  if (img->precached)
    return CD_IMAGE_PRECACHE_RESULT_SUCCESS;

  const chd_error err = chd_precache_progress(img->chd, NULL, NULL);
  if (err != CHDERR_NONE) {
    ERROR_LOG("chd_precache_progress() failed: %s", chd_error_string(err));
    Error_set_string_fmt(error, "chd_precache_progress() failed: %s", chd_error_string(err));
    return CD_IMAGE_PRECACHE_RESULT_READ_ERROR;
  }

  img->precached = true;
  return CD_IMAGE_PRECACHE_RESULT_SUCCESS;
}

static bool cdimage_chd_is_precached(const cd_image_t* self)
{
  const cd_image_chd_t* img = (const cd_image_chd_t*)self;
  return img->precached;
}

static s64 cdimage_chd_get_size_on_disk(const cd_image_t* self)
{
  cd_image_chd_t* img = (cd_image_chd_t*)self;
  return (s64)chd_get_compressed_size(img->chd);
}

static void cdimage_chd_destroy(cd_image_t* self)
{
  cd_image_chd_t* img = (cd_image_chd_t*)self;
  if (img->chd) {
    chd_close(img->chd);
    img->chd = NULL;
  }
  free(img->hunk_buffer);
  img->hunk_buffer = NULL;
}

static const cd_image_vtable_t s_chd_vtable = {
   .read_sector_from_index = cdimage_chd_read_sector_from_index,
  .read_subchannel_q      = cdimage_chd_read_subchannel_q,
  .has_subchannel_data    = cdimage_chd_has_subchannel_data,
  .precache               = cdimage_chd_precache,
  .is_precached           = cdimage_chd_is_precached,
  .get_size_on_disk       = cdimage_chd_get_size_on_disk,
  .destroy                = cdimage_chd_destroy, 
};

static chd_file* open_chd_recursive(const char* filename, FILE* fp, Error* error, u32 recursion_level);

/* Scan the directory containing `filename` for any *.chd whose header is a
 * valid parent for `want`.  Recursively opens the match.  Returns NULL if
 * none matches. */
static chd_file* find_parent_chd(const char* filename, const chd_header* want, u32 recursion_level, Error* error)
{
  const char* dir_data; u32 dir_len;
  path_get_directory_cstr(filename, &dir_data, &dir_len);

  small_string_t parent_dir;
  small_string_init(&parent_dir);
  if (dir_len == 0u)
    small_string_assign_cstr(&parent_dir, ".");
  else
    small_string_assign_view(&parent_dir, dir_data, dir_len);

  fs_find_data_t* results = NULL;
  size_t result_count = 0;
  const bool found = fs_find_files(small_string_c_str(&parent_dir), "*.chd",
                                   FS_FIND_FILES | FS_FIND_HIDDEN_FILES,
                                   &results, &result_count);
  small_string_destroy(&parent_dir);
  if (!found || result_count == 0u) {
    if (results) fs_free_find_data_array(results, result_count);
    return NULL;
  }

  chd_file* parent = NULL;
  for (size_t k = 0; k < result_count; k++) {
    const char* candidate = results[k].file_name;
    if (strcmp(candidate, filename) == 0)
      continue;

    FILE* cfp = fs_open_shared_file(candidate, "rb", FS_FILE_SHARE_DENY_WRITE, NULL);
    if (!cfp)
      continue;

    chd_header chdr;
    if (chd_read_header_file(cfp, &chdr) != CHDERR_NONE) {
      fclose(cfp);
      continue;
    }

    if (!chd_is_matching_parent(want, &chdr)) {
      fclose(cfp);
      continue;
    }

    parent = open_chd_recursive(candidate, cfp, error, recursion_level + 1u);
    /* On success cfp is owned by libchdr; on failure open_chd_recursive
     * already closed it. */
    if (parent) {
      VERBOSE_LOG("Using parent CHD '%s' for '%s'", candidate, filename);
      break;
    }
  }

  fs_free_find_data_array(results, result_count);
  return parent;
}

/* Take ownership of `fp` regardless of outcome.  On success libchdr owns
 * the fp; on failure we close it. */
static chd_file* open_chd_recursive(const char* filename, FILE* fp, Error* error, u32 recursion_level)
{
  chd_file* chd = NULL;
  chd_error err = chd_open_file(fp, CHD_OPEN_READ | CHD_OPEN_TRANSFER_FILE, NULL, &chd);
  if (err == CHDERR_NONE)
    return chd; /* libchdr now owns fp */

  if (err != CHDERR_REQUIRES_PARENT) {
    ERROR_LOG("Failed to open CHD '%s': %s", filename, chd_error_string(err));
    Error_set_string(error, chd_error_string(err));
    fclose(fp);
    return NULL;
  }

  if (recursion_level >= MAX_PARENTS) {
    ERROR_LOG("Failed to open CHD '%s': too many parent files", filename);
    Error_set_string(error, "Too many parent files");
    fclose(fp);
    return NULL;
  }

  /* Read header so we know what SHA1 to look for among siblings. */
  chd_header want;
  err = chd_read_header_file(fp, &want);
  if (err != CHDERR_NONE) {
    ERROR_LOG("Failed to read CHD header '%s': %s", filename, chd_error_string(err));
    Error_set_string(error, chd_error_string(err));
    fclose(fp);
    return NULL;
  }

  chd_file* parent = find_parent_chd(filename, &want, recursion_level, error);
  if (!parent) {
    ERROR_LOG("Failed to open CHD '%s': failed to find parent CHD in same directory", filename);
    Error_set_string(error, "Failed to find parent CHD; it must be in the same directory.");
    fclose(fp);
    return NULL;
  }

  err = chd_open_file(fp, CHD_OPEN_READ | CHD_OPEN_TRANSFER_FILE, parent, &chd);
  if (err != CHDERR_NONE) {
    ERROR_LOG("Failed to open CHD '%s' with parent: %s", filename, chd_error_string(err));
    Error_set_string(error, chd_error_string(err));
    chd_close(parent);
    fclose(fp);
    return NULL;
  }
  /* libchdr now owns fp.  parent is referenced internally by `chd`. */
  return chd;
}

static bool open_and_parse_chd(cd_image_chd_t* img, const char* path, Error* error)
{
  FILE* fp = fs_open_shared_file(path, "rb", FS_FILE_SHARE_DENY_WRITE, error);
  if (!fp) {
    Error_add_prefix_fmt(error, "Failed to open CHD '%s': ", path);
    return false;
  }

  img->chd = open_chd_recursive(path, fp, error, 0u);
  if (!img->chd)
    return false;

  const chd_header* header = chd_get_header(img->chd);
  img->hunk_size = header->hunkbytes;
  if ((img->hunk_size % CHD_CD_SECTOR_DATA_SIZE) != 0u) {
    ERROR_LOG("Hunk size (%u) is not a multiple of %u", img->hunk_size, (u32)CHD_CD_SECTOR_DATA_SIZE);
    Error_set_string_fmt(error, "Hunk size (%u) is not a multiple of %u",
                         img->hunk_size, (u32)CHD_CD_SECTOR_DATA_SIZE);
    return false;
  }

  img->sectors_per_hunk = img->hunk_size / CHD_CD_SECTOR_DATA_SIZE;
  img->hunk_buffer = (u8*)malloc(img->hunk_size);
  if (!img->hunk_buffer) Panic("Memory allocation failed.");

  img->base.filename = (char*)malloc(strlen(path) + 1u);
  if (!img->base.filename) Panic("Memory allocation failed.");
  strcpy(img->base.filename, path);

  cd_image_lba_t disc_lba = 0;
  u64            file_lba = 0;

  int num_tracks = 0;
  for (;;) {
    char metadata_str[256];
    char type_str[256];
    char subtype_str[256];
    char pgtype_str[256];
    char pgsub_str[256];
    u32  metadata_length = 0;

    int track_num = 0, frames = 0, pregap_frames = 0, postgap_frames = 0;
    chd_error err = chd_get_metadata(img->chd, CDROM_TRACK_METADATA2_TAG, (u32)num_tracks,
                                     metadata_str, sizeof(metadata_str),
                                     &metadata_length, NULL, NULL);
    if (err == CHDERR_NONE) {
      if (sscanf(metadata_str, CDROM_TRACK_METADATA2_FORMAT, &track_num, type_str, subtype_str, &frames,
                 &pregap_frames, pgtype_str, pgsub_str, &postgap_frames) != 8) {
        ERROR_LOG("Invalid track v2 metadata: '%s'", metadata_str);
        Error_set_string_fmt(error, "Invalid track v2 metadata: '%s'", metadata_str);
        return false;
      }
    } else {
      err = chd_get_metadata(img->chd, CDROM_TRACK_METADATA_TAG, (u32)num_tracks,
                             metadata_str, sizeof(metadata_str),
                             &metadata_length, NULL, NULL);
      if (err != CHDERR_NONE)
        break; /* no more tracks */

      if (sscanf(metadata_str, CDROM_TRACK_METADATA_FORMAT, &track_num, type_str, subtype_str, &frames) != 4) {
        ERROR_LOG("Invalid track metadata: '%s'", metadata_str);
        Error_set_string_fmt(error, "Invalid track metadata: '%s'", metadata_str);
        return false;
      }
      pgtype_str[0] = '\0';
    }

    uint32_t csubtype = CD_SUB_NONE, csubsize = 0;
    if (!cdrom_parse_subtype_string(subtype_str, &csubtype, &csubsize)) {
      csubtype = CD_SUB_NONE;
      csubsize = 0;
    }

    if (track_num != (num_tracks + 1)) {
      ERROR_LOG("Incorrect track number at index %d, expected %d got %d",
                num_tracks, num_tracks + 1, track_num);
      Error_set_string_fmt(error, "Incorrect track number at index %d, expected %d got %d",
                           num_tracks, num_tracks + 1, track_num);
      return false;
    }

    cd_image_track_mode_t mode;
    if (!parse_track_mode_string(type_str, &mode)) {
      ERROR_LOG("Invalid track mode: '%s'", type_str);
      Error_set_string_fmt(error, "Invalid track mode: '%s'", type_str);
      return false;
    }

    cd_image_subq_control_t control; control.bits = 0;
    cd_image_subq_control_set_data(&control, mode != CD_IMAGE_TRACK_MODE_AUDIO);

    /* If the track is data, default to 2 seconds pregap. */
    const bool pregap_in_file = (pregap_frames > 0 && pgtype_str[0] == 'V');
    if (pregap_frames <= 0 && mode != CD_IMAGE_TRACK_MODE_AUDIO)
      pregap_frames = 2 * (int)CD_IMAGE_FRAMES_PER_SECOND;

    if (pregap_frames > 0) {
      cd_image_index_t* pregap = cd_image_push_index(&img->base);
      pregap->start_lba_on_disc  = disc_lba;
      pregap->start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
      pregap->length             = (u32)pregap_frames;
      pregap->track_number       = (u32)track_num;
      pregap->index_number       = 0;
      pregap->mode               = mode;
      pregap->submode            = (cd_image_subchannel_mode_t)csubtype;
      pregap->control            = control;
      pregap->is_pregap          = true;

      if (pregap_in_file) {
        if (pregap_frames > frames) {
          ERROR_LOG("Pregap length %d exceeds track length %d", pregap_frames, frames);
          Error_set_string_fmt(error, "Pregap length %d exceeds track length %d", pregap_frames, frames);
          return false;
        }

        pregap->file_index       = 0;
        pregap->file_offset      = file_lba;
        pregap->file_sector_size = CHD_CD_SECTOR_DATA_SIZE;
        file_lba += (u64)pregap_frames;
        frames   -= pregap_frames;
      }

      disc_lba += (u32)pregap_frames;
    }

    /* The track summary. */
    cd_image_track_t* tr = cd_image_push_track(&img->base);
    tr->track_number = (u32)track_num;
    tr->start_lba    = disc_lba;
    tr->first_index  = img->base.index_count;
    tr->length       = (u32)(frames + pregap_frames);
    tr->mode         = mode;
    tr->submode      = (cd_image_subchannel_mode_t)csubtype;
    tr->control      = control;

    /* Main index for the track. */
    cd_image_index_t* idx = cd_image_push_index(&img->base);
    idx->start_lba_on_disc  = disc_lba;
    idx->start_lba_in_track = 0;
    idx->track_number       = (u32)track_num;
    idx->index_number       = 1;
    idx->file_index         = 0;
    /* On CHD-backed indices file_sector_size is the on-disk hunk stride
     * (always 2352+96=2448), NOT the per-track-mode payload size used by
     * cue-backed indices. CHD's read path (update_hunk_buffer) ignores this
     * field; only cd_image_memory + cd_image.c consult it as a `> 0`
     * predicate to distinguish real-data indices from implicit pregaps.
     * Do NOT switch this to cd_image_get_bytes_per_sector(mode) without
     * first auditing the hunk grid layout. Same applies to the pregap
     * assignment above. */
    idx->file_sector_size   = CHD_CD_SECTOR_DATA_SIZE;
    idx->file_offset        = file_lba;
    idx->mode               = mode;
    idx->submode            = (cd_image_subchannel_mode_t)csubtype;
    idx->control            = control;
    idx->is_pregap          = false;
    idx->length             = (u32)frames;

    disc_lba += (u32)frames;
    file_lba += (u64)frames;
    num_tracks++;

    /* chdman aligns each track's file region to a 4-frame boundary. */
    file_lba = align_up_u64(file_lba, CHD_CD_TRACK_ALIGNMENT);
  }

  if (img->base.track_count == 0u) {
    ERROR_LOG("File '%s' contains no tracks", path);
    Error_set_string_fmt(error, "File '%s' contains no tracks", path);
    return false;
  }

  img->base.lba_count = disc_lba;
  cd_image_add_lead_out_index(&img->base);

  return cd_image_seek_track_msf(&img->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

static cd_image_chd_t* alloc_chd_image(void)
{
  cd_image_chd_t* img = (cd_image_chd_t*)malloc(sizeof(cd_image_chd_t));
  if (!img) Panic("Memory allocation failed.");
  cd_image_init(&img->base, &s_chd_vtable);
  img->chd                = NULL;
  img->hunk_size          = 0;
  img->sectors_per_hunk   = 0;
  img->hunk_buffer        = NULL;
  img->current_hunk_index = (u32)-1;
  img->precached          = false;
  return img;
}

cd_image_t* cd_image_open_chd(const char* path, Error* error)
{
  cd_image_chd_t* img = alloc_chd_image();
  if (!open_and_parse_chd(img, path, error)) {
    cd_image_destroy(&img->base);
    return NULL;
  }
  return &img->base;
}
