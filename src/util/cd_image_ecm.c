/*
 * CDImageECM: read Neill Corlett "Error Code Modeler" .ecm sidecar images.
 *
 * ECM strips the ECC P/Q parity (276 bytes) and EDC (4 bytes) from data
 * sectors at compression time, plus the 12-byte sync + 4-byte header for
 * Mode1 and the redundant 4-byte subheader copy for Mode2.  Decompression
 * reconstructs all of those from the surviving payload bytes.  The only
 * wins are over the redundant fields - audio + already-zero-ECC sectors
 * just pass through as "raw".
 *
 * File layout (v1):
 *   magic      'E' 'C' 'M' 0x00                                    4 B
 *   stream of (typed_count, payload) blocks:
 *     typed_count: variable-length (1+ bytes).  First byte:
 *                    bits 0-1 = type   (0=Raw, 1=Mode1, 2=Mode2Form1,
 *                                       3=Mode2Form2)
 *                    bits 2-6 = count  bits 0..4
 *                    bit  7   = continuation flag
 *                  Continuation bytes contribute (b & 0x7F) << shift
 *                  where shift starts at 5 and increments by 7.
 *                  count is stored 0-based; the actual block count = N+1.
 *                  The terminator is the encoded value 0xFFFFFFFF (no +1).
 *     payload size by type:
 *       Raw         : N raw bytes        ->  N bytes of disc image
 *       Mode1       : 0x803 bytes        ->  2352 bytes / sector (N sectors)
 *       Mode2Form1  : 0x804 bytes        ->  2336 bytes / sector
 *       Mode2Form2  : 0x918 bytes        ->  2336 bytes / sector
 *   trailing CRC32 of the decompressed image (we don't verify).
 *
 * For the C port we keep upstream's two-stage scheme:
 *   1. Open(): scan the file once and build an `entries[]` sorted by disc
 *      offset.  Each entry caches (disc_offset, file_offset, chunk_size,
 *      type).  We do NOT decompress here.
 *   2. read_sector_from_index(): on demand, locate the entry covering the
 *      requested disc range (binary-search lower_bound), seek the file,
 *      decompress one or more entries into m_chunk_buffer, and memcpy out
 *      RAW_SECTOR_SIZE bytes.  Subsequent reads in the same window are
 *      satisfied straight from the buffer.
 *
 * ECC P/Q parity LUTs and the EDC CRC-32 routine come from libchdr's
 * cdrom.c (vendored in dep/libchdr; same source upstream uses post-merge).
 *
 * Limitations:
 *   - Single-track Mode2Raw image, 2-second pregap, no subchannel.  This
 *     mirrors cd_image_open_bin and the upstream OpenEcmImage path.
 *   - We don't verify the trailing CRC32.
 */

#include "cd_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

#include "libchdr/cdrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDImage);

enum ecm_sector_type {
  ECM_TYPE_RAW         = 0u,
  ECM_TYPE_MODE1       = 1u,
  ECM_TYPE_MODE2_FORM1 = 2u,
  ECM_TYPE_MODE2_FORM2 = 3u,
};

/* Bytes consumed from the ECM file per emitted sector. */
static const u32 ecm_sector_file_sizes[4] = {
  0u,    /* raw - variable, set per-entry */
  0x803u,
  0x804u,
  0x918u,
};

/* Bytes emitted to the chunk buffer per entry (raw is variable). */
static const u32 ecm_sector_chunk_sizes[4] = {
  0u,    /* raw - variable, set per-entry */
  2352u,
  2336u,
  2336u,
};

typedef struct ecm_entry {
  u32 disc_offset;   /* byte offset in the decompressed disc image */
  u32 file_offset;   /* byte offset in the .ecm file (start of payload) */
  u32 chunk_size;    /* decompressed size emitted by this entry */
  u32 type;          /* enum ecm_sector_type */
} ecm_entry_t;

typedef struct cd_image_ecm {
  cd_image_t   base;          /* MUST be first */
  FILE*        fp;
  ecm_entry_t* entries;
  u32          entry_count;
  u32          entry_capacity;

  /* Decoded read window. */
  u8*  chunk_buffer;
  u32  chunk_buffer_size;
  u32  chunk_buffer_capacity;
  u32  chunk_start;           /* disc-offset that chunk_buffer[0] corresponds to */
} cd_image_ecm_t;

static void ecm_grow_entries(cd_image_ecm_t* img)
{
  if (img->entry_count == img->entry_capacity) {
    u32 new_cap = img->entry_capacity ? img->entry_capacity * 2u : 256u;
    img->entries = (ecm_entry_t*)realloc(img->entries, (size_t)new_cap * sizeof(ecm_entry_t));
    if (!img->entries) Panic("Memory allocation failed.");
    img->entry_capacity = new_cap;
  }
}

static void ecm_push_entry(cd_image_ecm_t* img, u32 disc_offset, u32 file_offset,
                           u32 chunk_size, u32 type)
{
  ecm_grow_entries(img);
  ecm_entry_t* e = &img->entries[img->entry_count++];
  e->disc_offset = disc_offset;
  e->file_offset = file_offset;
  e->chunk_size  = chunk_size;
  e->type        = type;
}

/* Returns the largest index i such that entries[i].disc_offset <= disc_offset.
 * Caller guarantees entry_count > 0 and entries[0].disc_offset == 0. */
static u32 ecm_find_entry_le(const cd_image_ecm_t* img, u32 disc_offset)
{
  u32 lo = 0, hi = img->entry_count;
  while (lo + 1u < hi) {
    u32 mid = lo + (hi - lo) / 2u;
    if (img->entries[mid].disc_offset <= disc_offset)
      lo = mid;
    else
      hi = mid;
  }
  return lo;
}

static void ecm_chunk_reserve(cd_image_ecm_t* img, u32 needed)
{
  if (needed <= img->chunk_buffer_capacity) return;
  u32 new_cap = img->chunk_buffer_capacity ? img->chunk_buffer_capacity : 4096u;
  while (new_cap < needed) new_cap *= 2u;
  img->chunk_buffer = (u8*)realloc(img->chunk_buffer, new_cap);
  if (!img->chunk_buffer) Panic("Memory allocation failed.");
  img->chunk_buffer_capacity = new_cap;
}

/* Decode one entry from the file at its file_offset into dst (chunk_size bytes).
 * Reconstructs sync/header/EDC/ECC for the structured types. */
static bool ecm_decode_entry(cd_image_ecm_t* img, const ecm_entry_t* e, u8* dst)
{
  if (fseek(img->fp, (long)e->file_offset, SEEK_SET) != 0)
    return false;

  switch (e->type) {
    case ECM_TYPE_RAW: {
      if (fread(dst, e->chunk_size, 1u, img->fp) != 1u)
        return false;
      return true;
    }

    case ECM_TYPE_MODE1: {
      /* Sync 12 + header 4 + data 2048 + EDC 4 + zero pad 8 + ECC 276 = 2352. */
      u8 sector[2352];
      memset(sector, 0, sizeof(sector));
      memset(sector + 1, 0xFF, 10);  /* sync = 00 FF*10 00 */
      sector[0x0F] = 0x01;           /* mode 1 */
      /* 3 bytes M:S:F header (without mode byte) */
      if (fread(sector + 0x00C, 0x003u, 1u, img->fp) != 1u) return false;
      /* 2048 bytes of user data */
      if (fread(sector + 0x010, 0x800u, 1u, img->fp) != 1u) return false;
      edc_set(&sector[2064], edc_compute(sector, 2064u));
      ecc_generate(sector);
      memcpy(dst, sector, 2352u);
      return true;
    }

    case ECM_TYPE_MODE2_FORM1: {
      /* For Mode2 Form1, the on-disc layout is:
       *   sync 12 + header 4 + subheader 8 + data 2048 + EDC 4 + ECC 276 = 2352.
       * ECM stores subheader + data + EDC contiguously (4 + 2048 + 4 = 0x804)
       * starting at offset 0x14 in the synthesised raw buffer.  The duplicate
       * subheader copy at 0x10 is reconstructed from the one at 0x14 (Form1
       * spec puts identical subheaders at 0x10 and 0x14).  The EDC is
       * recomputed (covers subheader + data) and the P/Q ECC parities are
       * generated from the result.  We then emit only 2336 bytes (skipping
       * the 16-byte sync+header) - matches the "Mode2/2336" track layout
       * cd_image expects for Mode2Raw images. */
      u8 sector[2352];
      memset(sector, 0, sizeof(sector));
      memset(sector + 1, 0xFF, 10);
      sector[0x0F] = 0x02;
      if (fread(sector + 0x014, 0x804u, 1u, img->fp) != 1u) return false;
      sector[0x10] = sector[0x14];
      sector[0x11] = sector[0x15];
      sector[0x12] = sector[0x16];
      sector[0x13] = sector[0x17];
      edc_set(&sector[2072], edc_compute(&sector[16], 2056u));
      ecc_generate(sector);
      /* Emit 2336 bytes starting at the subheader. */
      memcpy(dst, sector + 0x10, 2336u);
      return true;
    }

    case ECM_TYPE_MODE2_FORM2: {
      /* Mode2 Form2: subheader 8 + data 2324 + optional EDC 4 = 2336.
       * ECM stores subheader (4) + data (2324) + EDC (4) = 0x918, starting
       * at offset 0x14.  No P/Q ECC for Form2.  EDC is recomputed across
       * subheader + data (2332 bytes). */
      u8 sector[2352];
      memset(sector, 0, sizeof(sector));
      memset(sector + 1, 0xFF, 10);
      sector[0x0F] = 0x02;
      if (fread(sector + 0x014, 0x918u, 1u, img->fp) != 1u) return false;
      sector[0x10] = sector[0x14];
      sector[0x11] = sector[0x15];
      sector[0x12] = sector[0x16];
      sector[0x13] = sector[0x17];
      edc_set(&sector[2348], edc_compute(&sector[16], 2332u));
      memcpy(dst, sector + 0x10, 2336u);
      return true;
    }

    default:
      return false;
  }
}

/* Fill chunk_buffer so that bytes [disc_offset, disc_offset + size) are
 * covered.  We start from the entry whose disc range contains disc_offset
 * (or the one immediately before, mirroring upstream's lower_bound dance)
 * and decode forward until we cover the requested window. */
static bool ecm_fill_window(cd_image_ecm_t* img, u32 disc_offset, u32 size)
{
  if (img->entry_count == 0) return false;

  /* Pick the entry that covers disc_offset. */
  u32 idx = ecm_find_entry_le(img, disc_offset);
  img->chunk_start = img->entries[idx].disc_offset;
  img->chunk_buffer_size = 0;

  /* If the entry starts before disc_offset we still need to materialise
   * the leading bytes - just enlarge the request. */
  if (img->chunk_start < disc_offset)
    size += (disc_offset - img->chunk_start);

  u32 produced = 0;
  while (produced < size && idx < img->entry_count) {
    const ecm_entry_t* e = &img->entries[idx];
    ecm_chunk_reserve(img, produced + e->chunk_size);
    if (!ecm_decode_entry(img, e, img->chunk_buffer + produced))
      return false;
    produced += e->chunk_size;
    idx++;
  }

  if (produced < size)
    return false;

  img->chunk_buffer_size = produced;
  return true;
}

static bool cdimage_ecm_read_sector_from_index(cd_image_t* self, void* buffer,
                                               const cd_image_index_t* index,
                                               cd_image_lba_t lba_in_index)
{
  cd_image_ecm_t* img = (cd_image_ecm_t*)self;

  const u32 file_start = (u32)index->file_offset + lba_in_index * index->file_sector_size;
  const u32 file_end   = file_start + (u32)CD_IMAGE_RAW_SECTOR_SIZE;

  if (file_start < img->chunk_start
      || file_end > (img->chunk_start + img->chunk_buffer_size)) {
    if (!ecm_fill_window(img, file_start, (u32)CD_IMAGE_RAW_SECTOR_SIZE))
      return false;
  }

  const u32 offset_in_chunk = file_start - img->chunk_start;
  memcpy(buffer, img->chunk_buffer + offset_in_chunk, (u32)CD_IMAGE_RAW_SECTOR_SIZE);
  return true;
}

static s64 cdimage_ecm_get_size_on_disk(const cd_image_t* self)
{
  cd_image_ecm_t* img = (cd_image_ecm_t*)self;
  return fs_fsize64(img->fp, NULL);
}

static void cdimage_ecm_destroy(cd_image_t* self)
{
  cd_image_ecm_t* img = (cd_image_ecm_t*)self;
  if (img->fp) { fclose(img->fp); img->fp = NULL; }
  free(img->entries);       img->entries = NULL;
  free(img->chunk_buffer);  img->chunk_buffer = NULL;
}

static const cd_image_vtable_t s_ecm_vtable = {
   .read_sector_from_index = cdimage_ecm_read_sector_from_index,
  .get_size_on_disk       = cdimage_ecm_get_size_on_disk,
  .destroy                = cdimage_ecm_destroy,
};

static cd_image_ecm_t* alloc_ecm_image(void)
{
  cd_image_ecm_t* img = (cd_image_ecm_t*)malloc(sizeof(*img));
  if (!img) Panic("Memory allocation failed.");
  cd_image_init(&img->base, &s_ecm_vtable);
  img->fp = NULL;
  img->entries = NULL;
  img->entry_count = 0;
  img->entry_capacity = 0;
  img->chunk_buffer = NULL;
  img->chunk_buffer_size = 0;
  img->chunk_buffer_capacity = 0;
  img->chunk_start = 0;
  return img;
}

/* Read one variable-length type+count header.  Returns false on EOF or
 * malformed stream; on success outputs:
 *   *out_type   : 0..3
 *   *out_count  : decoded count, or 0xFFFFFFFFu for the terminator (caller
 *                 must NOT add 1 in that case)
 *   *out_bytes  : number of bytes consumed (1..5).
 * Mirrors upstream Open() loop verbatim. */
static bool ecm_read_block_header(FILE* fp, u32* out_type, u32* out_count, u32* out_bytes)
{
  int b = fgetc(fp);
  if (b == EOF) return false;

  u32 bytes = 1;
  const u32 type  = (u32)b & 0x03u;
  u32 count = ((u32)b >> 2) & 0x1Fu;
  u32 shift = 5;
  while (b & 0x80) {
    b = fgetc(fp);
    if (b == EOF) return false;
    count |= ((u32)b & 0x7Fu) << shift;
    shift += 7;
    bytes++;
  }

  *out_type  = type;
  *out_count = count;
  *out_bytes = bytes;
  return true;
}

static bool open_and_parse_ecm(cd_image_ecm_t* img, const char* filename, Error* error)
{
  img->base.filename = (char*)malloc(strlen(filename) + 1u);
  if (!img->base.filename) Panic("Memory allocation failed.");
  strcpy(img->base.filename, filename);

  img->fp = fs_open_shared_file(filename, "rb", FS_FILE_SHARE_DENY_WRITE, error);
  if (!img->fp) {
    Error_set_string_fmt(error, "Failed to open ecm '%s'", filename);
    return false;
  }

  s64 file_size = fs_fsize64(img->fp, NULL);
  if (file_size <= 4) {
    ERROR_LOG("ecm too small: %s", filename);
    Error_set_string_fmt(error, "ecm too small: '%s'", filename);
    return false;
  }

  if (fseek(img->fp, 0, SEEK_SET) != 0) {
    Error_set_string_fmt(error, "Failed to rewind ecm '%s'", filename);
    return false;
  }

  /* Magic. */
  char header[4];
  if (fread(header, sizeof(header), 1, img->fp) != 1
      || header[0] != 'E' || header[1] != 'C' || header[2] != 'M' || header[3] != 0) {
    ERROR_LOG("Failed to read/invalid ecm header in '%s'", filename);
    Error_set_string_fmt(error, "Failed to read/invalid ecm header in '%s'", filename);
    return false;
  }

  u32 file_offset = 4u;
  u32 disc_offset = 0u;

  for (;;) {
    u32 type, count, hdr_bytes;
    if (!ecm_read_block_header(img->fp, &type, &count, &hdr_bytes)) {
      ERROR_LOG("Unexpected EOF after %u chunks in '%s'", img->entry_count, filename);
      Error_set_string_fmt(error, "Unexpected EOF after %u chunks", img->entry_count);
      return false;
    }
    file_offset += hdr_bytes;

    if (count == 0xFFFFFFFFu)
      break;  /* terminator */

    /* count is 0-based; +1 = actual block count. */
    count++;
    if (count >= 0x80000000u) {
      ERROR_LOG("Corrupted ecm header after %u chunks in '%s'", img->entry_count, filename);
      Error_set_string_fmt(error, "Corrupted ecm header after %u chunks", img->entry_count);
      return false;
    }

    if (type == ECM_TYPE_RAW) {
      while (count > 0) {
        const u32 size = (count < 2352u) ? count : 2352u;
        ecm_push_entry(img, disc_offset, file_offset, size, type);
        disc_offset += size;
        file_offset += size;
        count       -= size;
        if ((s64)file_offset > file_size) {
          ERROR_LOG("Out of file bounds after %u chunks in '%s'", img->entry_count, filename);
          Error_set_string_fmt(error, "Out of file bounds after %u chunks", img->entry_count);
          return false;
        }
      }
    } else {
      const u32 size       = ecm_sector_file_sizes[type];
      const u32 chunk_size = ecm_sector_chunk_sizes[type];
      for (u32 i = 0; i < count; i++) {
        ecm_push_entry(img, disc_offset, file_offset, chunk_size, type);
        disc_offset += chunk_size;
        file_offset += size;
        if ((s64)file_offset > file_size) {
          ERROR_LOG("Out of file bounds after %u chunks in '%s'", img->entry_count, filename);
          Error_set_string_fmt(error, "Out of file bounds after %u chunks", img->entry_count);
          return false;
        }
      }
    }

    if (fseek(img->fp, (long)file_offset, SEEK_SET) != 0) {
      ERROR_LOG("Failed to seek to offset %u after %u chunks in '%s'",
                file_offset, img->entry_count, filename);
      Error_set_string_fmt(error, "Failed to seek to offset %u in '%s'", file_offset, filename);
      return false;
    }
  }

  if (img->entry_count == 0) {
    ERROR_LOG("No data in image '%s'", filename);
    Error_set_string_fmt(error, "No data in image '%s'", filename);
    return false;
  }

  /* Build the implicit single-track / 2-second-pregap TOC, mirroring
   * cd_image_open_bin.  ECM is a sidecar of a single .bin so there is
   * no per-track metadata to recover. */
  const u32 lba_count = disc_offset / (u32)CD_IMAGE_RAW_SECTOR_SIZE;
  if ((disc_offset % (u32)CD_IMAGE_RAW_SECTOR_SIZE) != 0) {
    WARNING_LOG("ECM image '%s' is misaligned (disc_offset=%u not a multiple of 2352)",
                filename, disc_offset);
  }
  if (lba_count == 0) {
    Error_set_string_fmt(error, "ecm image '%s' contains zero sectors", filename);
    return false;
  }
  img->base.lba_count = lba_count;

  const cd_image_track_mode_t mode = CD_IMAGE_TRACK_MODE_MODE2_RAW;
  cd_image_subq_control_t control = { 0 };
  cd_image_subq_control_set_data(&control, mode != CD_IMAGE_TRACK_MODE_AUDIO);

  const u32 pregap_frames = 2u * (u32)CD_IMAGE_FRAMES_PER_SECOND;

  cd_image_index_t* pregap = cd_image_push_index(&img->base);
  pregap->file_index         = 0;
  pregap->file_offset        = 0;
  pregap->file_sector_size   = (u32)CD_IMAGE_RAW_SECTOR_SIZE;
  pregap->start_lba_on_disc  = 0;
  pregap->start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
  pregap->length             = pregap_frames;
  pregap->track_number       = 1;
  pregap->index_number       = 0;
  pregap->mode               = mode;
  pregap->submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
  pregap->control            = control;
  pregap->is_pregap          = true;

  cd_image_index_t* data = cd_image_push_index(&img->base);
  data->file_index         = 0;
  data->file_offset        = 0;
  data->file_sector_size   = (u32)CD_IMAGE_RAW_SECTOR_SIZE;
  data->start_lba_on_disc  = pregap_frames;
  data->start_lba_in_track = 0;
  data->length             = lba_count;
  data->track_number       = 1;
  data->index_number       = 1;
  data->mode               = mode;
  data->submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
  data->control            = control;
  data->is_pregap          = false;

  cd_image_track_t* tr = cd_image_push_track(&img->base);
  tr->track_number = 1;
  tr->start_lba    = pregap_frames;
  tr->first_index  = 0;
  tr->length       = lba_count + pregap_frames;
  tr->mode         = mode;
  tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
  tr->control      = control;

  cd_image_add_lead_out_index(&img->base);

  /* Pre-reserve a window large enough for at least two raw sectors to
   * keep sequential reads off the file once we're streaming. */
  ecm_chunk_reserve(img, (u32)CD_IMAGE_RAW_SECTOR_SIZE * 2u);

  return cd_image_seek_track_msf(&img->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

cd_image_t* cd_image_open_ecm(const char* path, Error* error)
{
  cd_image_ecm_t* img = alloc_ecm_image();
  if (!open_and_parse_ecm(img, path, error)) {
    cd_image_destroy(&img->base);
    return NULL;
  }
  return &img->base;
}
