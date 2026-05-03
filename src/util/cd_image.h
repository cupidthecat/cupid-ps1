/*
 * CDImage (abstract base) is a (struct, vtable) pair:
 *
 *   typedef struct cd_image           cd_image_t;
 *   typedef struct cd_image_vtable    cd_image_vtable_t;
 *   struct cd_image { const cd_image_vtable_t* vtbl; ... };
 *
 * Concrete implementations (currently only cue/bin via cd_image_cue.c) embed
 * cd_image_t as their first member and supply a vtable.  Callers go through
 * the inline wrappers (cd_image_read_sector_from_index, ...).
 *
 * Type-mapping notes:
 *   std::vector<Track>/<Index>  -> heap arrays + counts (tracks/indices)
 *   std::optional<T>            -> bool return + out pointer
 *   std::string m_filename      -> heap-allocated char* (owned by cd_image_t)
 *   BitField<>  control bits    -> u8 + setter/getter helpers + named accessors
 *
 * Format support: only .bin/.cue (cue with BINARY/WAVE files) and .m3u
 * playlists referencing them.  CHD/PBP/MDS/CCD/PPF/SBI/Device paths are not
 * ported.
 */

#ifndef CUPID_UTIL_CD_IMAGE_H
#define CUPID_UTIL_CD_IMAGE_H

#include "common/types.h"
#include "common/progress_callback.h"

#include <stdio.h>

typedef struct Error Error;

enum {
  CD_IMAGE_RAW_SECTOR_SIZE       = 2352,
  CD_IMAGE_DATA_SECTOR_SIZE      = 2048,
  CD_IMAGE_SECTOR_SYNC_SIZE      = 12,
  CD_IMAGE_SECTOR_HEADER_SIZE    = 4,
  CD_IMAGE_MODE1_HEADER_SIZE     = 4,
  CD_IMAGE_MODE2_HEADER_SIZE     = 12,
  CD_IMAGE_MODE2_DATA_SECTOR_SIZE = 2336, /* header + edc */
  CD_IMAGE_FRAMES_PER_SECOND     = 75,    /* "sectors", or "timecode frames" */
  CD_IMAGE_SECONDS_PER_MINUTE    = 60,
  CD_IMAGE_FRAMES_PER_MINUTE     = CD_IMAGE_FRAMES_PER_SECOND * CD_IMAGE_SECONDS_PER_MINUTE,
  CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME = 12,
  CD_IMAGE_LEAD_OUT_SECTOR_COUNT = 6750,
  CD_IMAGE_ALL_SUBCODE_SIZE      = 96,
  CD_IMAGE_AUDIO_SAMPLE_RATE     = 44100,
  CD_IMAGE_AUDIO_CHANNELS        = 2,
};

#define CD_IMAGE_LEAD_OUT_TRACK_NUMBER ((u8)0xAA)

typedef u32 cd_image_lba_t;

typedef enum {
  CD_IMAGE_TRACK_MODE_AUDIO        = 0, /* 2352 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE1        = 1, /* 2048 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE1_RAW    = 2, /* 2352 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE2        = 3, /* 2336 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE2_FORM1  = 4, /* 2048 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE2_FORM2  = 5, /* 2324 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE2_FORM_MIX = 6, /* 2332 bytes per sector */
  CD_IMAGE_TRACK_MODE_MODE2_RAW    = 7, /* 2352 bytes per sector */
} cd_image_track_mode_t;

typedef enum {
  CD_IMAGE_SUBCHANNEL_MODE_NONE             = 0, /* no subcode data stored */
  CD_IMAGE_SUBCHANNEL_MODE_RAW_INTERLEAVED  = 1, /* raw interleaved 96 bytes per sector */
  CD_IMAGE_SUBCHANNEL_MODE_RAW              = 2, /* raw uninterleaved 96 bytes per sector */
} cd_image_subchannel_mode_t;

typedef enum {
  CD_IMAGE_PRECACHE_RESULT_UNSUPPORTED = 0,
  CD_IMAGE_PRECACHE_RESULT_READ_ERROR  = 1,
  CD_IMAGE_PRECACHE_RESULT_SUCCESS     = 2,
} cd_image_precache_result_t;

typedef struct {
  u8 minute;
  u8 second;
  u8 frame;
  u8 sector_mode;
} cd_image_sector_header_t;

typedef struct {
  u8 minute;
  u8 second;
  u8 frame;
} cd_image_position_t;

cd_image_position_t cd_image_position_from_bcd(u8 minute, u8 second, u8 frame);
cd_image_position_t cd_image_position_from_lba(cd_image_lba_t lba);
cd_image_lba_t      cd_image_position_to_lba(cd_image_position_t p);
void                cd_image_position_to_bcd(cd_image_position_t p, u8* out_minute, u8* out_second, u8* out_frame);
cd_image_position_t cd_image_position_add(cd_image_position_t a, cd_image_position_t b);
bool                cd_image_position_eq(cd_image_position_t a, cd_image_position_t b);
bool                cd_image_position_lt(cd_image_position_t a, cd_image_position_t b);
bool                cd_image_position_le(cd_image_position_t a, cd_image_position_t b);
bool                cd_image_position_gt(cd_image_position_t a, cd_image_position_t b);
bool                cd_image_position_ge(cd_image_position_t a, cd_image_position_t b);


/* adr               : bits 0-3
 * audio_preemphasis : bit 4
 * digital_copy_permitted : bit 5
 * data              : bit 6
 * four_channel_audio: bit 7 */
typedef struct {
  u8 bits;
} cd_image_subq_control_t;

ALWAYS_INLINE u8   cd_image_subq_control_adr(cd_image_subq_control_t c)               { return c.bits & 0x0Fu; }
ALWAYS_INLINE bool cd_image_subq_control_audio_preemphasis(cd_image_subq_control_t c) { return (c.bits & (1u << 4)) != 0; }
ALWAYS_INLINE bool cd_image_subq_control_digital_copy_permitted(cd_image_subq_control_t c) { return (c.bits & (1u << 5)) != 0; }
ALWAYS_INLINE bool cd_image_subq_control_data(cd_image_subq_control_t c)              { return (c.bits & (1u << 6)) != 0; }
ALWAYS_INLINE bool cd_image_subq_control_four_channel_audio(cd_image_subq_control_t c){ return (c.bits & (1u << 7)) != 0; }

ALWAYS_INLINE void cd_image_subq_control_set_adr(cd_image_subq_control_t* c, u8 adr)
{
  c->bits = (u8)((c->bits & 0xF0u) | (adr & 0x0Fu));
}
ALWAYS_INLINE void cd_image_subq_control_set_audio_preemphasis(cd_image_subq_control_t* c, bool v)
{
  c->bits = (u8)((c->bits & ~(1u << 4)) | (v ? (1u << 4) : 0));
}
ALWAYS_INLINE void cd_image_subq_control_set_digital_copy_permitted(cd_image_subq_control_t* c, bool v)
{
  c->bits = (u8)((c->bits & ~(1u << 5)) | (v ? (1u << 5) : 0));
}
ALWAYS_INLINE void cd_image_subq_control_set_data(cd_image_subq_control_t* c, bool v)
{
  c->bits = (u8)((c->bits & ~(1u << 6)) | (v ? (1u << 6) : 0));
}
ALWAYS_INLINE void cd_image_subq_control_set_four_channel_audio(cd_image_subq_control_t* c, bool v)
{
  c->bits = (u8)((c->bits & ~(1u << 7)) | (v ? (1u << 7) : 0));
}

 /*
 * union: callers can access either named fields or raw .data[] bytes via the
 * helpers below. We don't use an actual union to avoid type-punning warnings;
 * field order is exact so memcpy round-trips losslessly. */
typedef struct {
  u8  control_bits;
  u8  track_number_bcd;
  u8  index_number_bcd;
  u8  relative_minute_bcd;
  u8  relative_second_bcd;
  u8  relative_frame_bcd;
  u8  reserved;
  u8  absolute_minute_bcd;
  u8  absolute_second_bcd;
  u8  absolute_frame_bcd;
  u16 crc;
} cd_image_subq_t;

_Static_assert(sizeof(cd_image_subq_t) == CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME, "cd_image_subq_t is correct size");

u16  cd_image_subq_compute_crc(const u8* data /* CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME */);
bool cd_image_subq_is_crc_valid(const cd_image_subq_t* subq);

ALWAYS_INLINE cd_image_subq_control_t cd_image_subq_get_control(const cd_image_subq_t* subq)
{
  cd_image_subq_control_t c; c.bits = subq->control_bits; return c;
}
ALWAYS_INLINE bool cd_image_subq_is_data(const cd_image_subq_t* subq)
{
  return cd_image_subq_control_data(cd_image_subq_get_control(subq));
}

typedef struct {
  u32                       track_number;
  cd_image_lba_t            start_lba;
  u32                       first_index;
  u32                       length;
  cd_image_track_mode_t     mode;
  cd_image_subchannel_mode_t submode;
  cd_image_subq_control_t   control;
} cd_image_track_t;

typedef struct {
  u64                        file_offset;
  u32                        file_index;
  u32                        file_sector_size;
  cd_image_lba_t             start_lba_on_disc;
  u32                        track_number;
  u32                        index_number;
  cd_image_lba_t             start_lba_in_track;
  u32                        length;
  cd_image_track_mode_t      mode;
  cd_image_subchannel_mode_t submode;
  cd_image_subq_control_t    control;
  bool                       is_pregap;
} cd_image_index_t;

typedef struct cd_image cd_image_t;
typedef struct cd_image_vtable cd_image_vtable_t;

struct cd_image_vtable {
  /* Required: read a single sector for the given index at lba_in_index.
   * buffer is index.file_sector_size bytes. */
  bool (*read_sector_from_index)(cd_image_t* self, void* buffer,
                                 const cd_image_index_t* index, cd_image_lba_t lba_in_index);

  /* Optional. Default implementation generates Q from the index/position. */
  bool (*read_subchannel_q)(cd_image_t* self, cd_image_subq_t* out,
                            const cd_image_index_t* index, cd_image_lba_t lba_in_index);

  /* Optional. Default returns false. */
  bool (*has_subchannel_data)(const cd_image_t* self);

  /* Optional precache. Default returns CD_IMAGE_PRECACHE_RESULT_UNSUPPORTED. */
  cd_image_precache_result_t (*precache)(cd_image_t* self, Error* error);
  bool (*is_precached)(const cd_image_t* self);

  /* Returns the size on disk (sum of backing files). -1 if unknown. */
  s64 (*get_size_on_disk)(const cd_image_t* self);

  void (*destroy)(cd_image_t* self);

  /* Sub-image (multi-disc playlist) interface.  Default impls in cd_image.c
   * report no sub-images; only cd_image_m3u.c overrides them today. */
  bool (*has_sub_images)       (const cd_image_t* self);
  u32  (*get_sub_image_count)  (const cd_image_t* self);
  u32  (*get_current_sub_image)(const cd_image_t* self);
  /* Returns a heap-allocated NUL-terminated title; caller frees.  NULL when
   * the index is out of range or no sub-images are present. */
  char* (*get_sub_image_title) (const cd_image_t* self, u32 index);
  bool (*switch_sub_image)     (cd_image_t* self, u32 index, Error* error);
};

struct cd_image {
  const cd_image_vtable_t* vtbl;

  char*  filename;          /* heap, owned */
  u32    lba_count;

  /* Heap arrays. Use cd_image_get_track_count / cd_image_get_index_count. */
  cd_image_track_t* tracks;
  u32               track_count;
  u32               track_capacity;

  cd_image_index_t* indices;
  u32               index_count;
  u32               index_capacity;

  /* Position state. */
  cd_image_lba_t          position_on_disc;
  const cd_image_index_t* current_index;
  cd_image_lba_t          position_in_index;
  cd_image_lba_t          position_in_track;
};

u32  cd_image_get_bytes_per_sector(cd_image_track_mode_t mode);
void cd_image_deinterleave_subcode(const u8* subcode_in, u8* subcode_out);

/* Opens a cue (the only supported entry point in the C port). Caller owns
 * the returned image; free with cd_image_destroy. NULL on failure (error
 * populated). */
cd_image_t* cd_image_open(const char* path, Error* error);

cd_image_t* cd_image_open_cue(const char* path, Error* error);

/* Opens a single .bin/.img/.iso treated as Mode2 raw with 2-second pregap. */
cd_image_t* cd_image_open_bin(const char* path, Error* error);

/* Opens an .m3u playlist referencing one or more bin/cue/img/iso entries.
 * The returned image is positioned on entry 0 and exposes the sub-image
 * vtable methods so callers can switch between discs at runtime. */
cd_image_t* cd_image_open_m3u(const char* path, Error* error);

/* Opens a MAME .chd (Compressed Hunks of Data) image via libchdr.
 * Resolves parent CHDs by scanning the source directory for matching SHA1
 * headers (up to 32 levels deep). */
cd_image_t* cd_image_open_chd(const char* path, Error* error);

/* Opens an Alcohol 120% .mds (descriptor) + .mdf (data file) pair. */
cd_image_t* cd_image_open_mds(const char* path, Error* error);

 /* Opens a CloneCD .ccd (descriptor) + .img (raw 2352-byte sectors) +
 * .sub (96-byte interleaved subchannel) triple. */
cd_image_t* cd_image_open_ccd(const char* path, Error* error);

/* Opens a PSP EBOOT.PBP for a decrypted PS1 dump.  Multi-disc images
 * exposed via the sub-image vtable. */
cd_image_t* cd_image_open_pbp(const char* path, Error* error);

/* Opens a Neill Corlett .ecm sidecar (.bin with ECC/EDC stripped).  Single-
 * track Mode2Raw with implicit 2s pregap, like cd_image_open_bin. */
cd_image_t* cd_image_open_ecm(const char* path, Error* error);

/* Opens a raw block device (/dev/sr*, /dev/cdrom, ...) for direct reads
 * via SG_IO + cdrom ioctls.  Linux-only. */
cd_image_t* cd_image_open_device(const char* path, Error* error);

/* Heuristic: returns true if `path` names a CD/DVD block device.  Linux:
 * starts with "/dev/" and the device responds to CDROM_GET_CAPABILITY. */
bool cd_image_is_device_name(const char* path);

/* Lists available CD/DVD block devices via libudev.  Caller must free
 * via cd_image_device_list_destroy. */
typedef struct {
  char**  paths;     /* devnode paths (e.g. "/dev/sr0") */
  char**  names;     /* display names (currently == paths) */
  size_t  count;
} cd_image_device_list_t;

cd_image_device_list_t cd_image_get_device_list(void);
void cd_image_device_list_destroy(cd_image_device_list_t* list);

cd_image_t* cd_image_overlay_ppf(const char* ppf_path, cd_image_t* parent, Error* error);

/* Pre-caches `src` into a memory-backed image (for performance / removable
 * media insurance).  Frees `src` on success.  NULL on failure.  Frontend
 * uses this for `--precache`. */
cd_image_t* cd_image_create_memory_image(cd_image_t* src, progress_callback_t* pc, Error* err);

/* Frees the image and its backing files. NULL-tolerant. */
void cd_image_destroy(cd_image_t* image);

ALWAYS_INLINE const char*    cd_image_get_path(const cd_image_t* i)               { return i->filename ? i->filename : ""; }
ALWAYS_INLINE cd_image_lba_t cd_image_get_position_on_disc(const cd_image_t* i)   { return i->position_on_disc; }
ALWAYS_INLINE cd_image_lba_t cd_image_get_position_in_track(const cd_image_t* i)  { return i->position_in_track; }
ALWAYS_INLINE cd_image_lba_t cd_image_get_lba_count(const cd_image_t* i)          { return i->lba_count; }
ALWAYS_INLINE u32            cd_image_get_track_count(const cd_image_t* i)        { return i->track_count; }
ALWAYS_INLINE u32            cd_image_get_index_count(const cd_image_t* i)        { return i->index_count; }
ALWAYS_INLINE const cd_image_track_t* cd_image_get_tracks(const cd_image_t* i)    { return i->tracks; }
ALWAYS_INLINE const cd_image_index_t* cd_image_get_indices(const cd_image_t* i)   { return i->indices; }

ALWAYS_INLINE cd_image_position_t cd_image_get_msf_position_on_disc(const cd_image_t* i)
{
  return cd_image_position_from_lba(i->position_on_disc);
}
ALWAYS_INLINE cd_image_position_t cd_image_get_msf_position_in_track(const cd_image_t* i)
{
  return cd_image_position_from_lba(i->position_in_track);
}
ALWAYS_INLINE u32 cd_image_get_track_number(const cd_image_t* i)
{
  return i->current_index->track_number;
}
ALWAYS_INLINE u32 cd_image_get_index_number(const cd_image_t* i)
{
  return i->current_index->index_number;
}
ALWAYS_INLINE u32 cd_image_get_first_track_number(const cd_image_t* i)
{
  return i->tracks[0].track_number;
}
ALWAYS_INLINE u32 cd_image_get_last_track_number(const cd_image_t* i)
{
  return i->tracks[i->track_count - 1u].track_number;
}

cd_image_lba_t      cd_image_get_track_start_position(const cd_image_t* i, u8 track);
cd_image_position_t cd_image_get_track_start_msf_position(const cd_image_t* i, u8 track);
cd_image_lba_t      cd_image_get_track_length(const cd_image_t* i, u8 track);
cd_image_position_t cd_image_get_track_msf_length(const cd_image_t* i, u8 track);
cd_image_track_mode_t cd_image_get_track_mode(const cd_image_t* i, u8 track);
cd_image_lba_t      cd_image_get_track_index_position(const cd_image_t* i, u8 track, u8 index);
cd_image_lba_t      cd_image_get_track_index_length(const cd_image_t* i, u8 track, u8 index);

const cd_image_track_t* cd_image_get_track(const cd_image_t* i, u32 track);
const cd_image_index_t* cd_image_get_index(const cd_image_t* i, u32 index);

bool cd_image_seek_lba(cd_image_t* i, cd_image_lba_t lba);
bool cd_image_seek_msf(cd_image_t* i, cd_image_position_t pos);
bool cd_image_seek_track_lba(cd_image_t* i, u32 track_number, cd_image_lba_t lba);
bool cd_image_seek_track_msf(cd_image_t* i, u32 track_number, cd_image_position_t pos_in_track);

/* Reads a single raw sector from the current position into buffer (RAW_SECTOR_SIZE
 * bytes; may be NULL to skip). Optionally fills *subq. Advances position. */
bool cd_image_read_raw_sector(cd_image_t* i, void* buffer, cd_image_subq_t* subq);

/* Generates Q given an LBA. Returns false if no index covers lba. */
bool cd_image_generate_subq(const cd_image_t* i, cd_image_subq_t* out, cd_image_lba_t lba);

/* Generates Q from an index + offset. */
void cd_image_generate_subq_from_index(const cd_image_t* i, cd_image_subq_t* out,
                                       const cd_image_index_t* index, u32 index_offset);

ALWAYS_INLINE bool cd_image_read_sector_from_index(cd_image_t* i, void* buffer,
                                                   const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  return i->vtbl->read_sector_from_index(i, buffer, index, lba_in_index);
}

bool cd_image_read_subchannel_q(cd_image_t* i, cd_image_subq_t* out,
                                const cd_image_index_t* index, cd_image_lba_t lba_in_index);

bool cd_image_has_subchannel_data(const cd_image_t* i);

cd_image_precache_result_t cd_image_precache(cd_image_t* i, Error* error);
bool cd_image_is_precached(const cd_image_t* i);

s64 cd_image_get_size_on_disk(const cd_image_t* i);

bool  cd_image_has_sub_images       (const cd_image_t* i);
u32   cd_image_get_sub_image_count  (const cd_image_t* i);
u32   cd_image_get_current_sub_image(const cd_image_t* i);
char* cd_image_get_sub_image_title  (const cd_image_t* i, u32 index);
bool  cd_image_switch_sub_image     (cd_image_t* i, u32 index, Error* error);

void cd_image_init(cd_image_t* i, const cd_image_vtable_t* vtbl);
void cd_image_clear_toc(cd_image_t* i);

/* Copies the TOC (lba_count, tracks, indices) from `src` into `dst`,
 * resetting position state.  Used by sub-image-capable images when they
 * swap their backing image.  Both pointers must be non-NULL. */
void cd_image_copy_toc(cd_image_t* dst, const cd_image_t* src);

cd_image_index_t* cd_image_push_index(cd_image_t* i);
cd_image_track_t* cd_image_push_track(cd_image_t* i);

void cd_image_add_lead_out_index(cd_image_t* i);

const cd_image_index_t* cd_image_get_index_for_disc_position(const cd_image_t* i, cd_image_lba_t pos);
const cd_image_index_t* cd_image_get_index_for_track_position(const cd_image_t* i, u32 track_number, cd_image_lba_t track_pos);

#endif /* CUPID_UTIL_CD_IMAGE_H */
