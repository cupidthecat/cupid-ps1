/*
 * The C++ IsoReader class collapses to:
 *
 *   typedef struct iso_reader iso_reader_t;
 *
 * No vtable - iso_reader_t is a plain value type that just holds a borrowed
 * cd_image_t* plus the cached primary volume descriptor.  Lifecycle is
 * iso_reader_init / iso_reader_open / (use) / iso_reader_destroy; the cd_image
 * is NOT owned by the reader.
 *
 * Type-mapping rules used here:
 *   std::string_view path                  -> const char* (NUL-terminated)
 *   std::string return                     -> small_string_t* out-param
 *   std::optional<ISODirectoryEntry>       -> bool return + iso_directory_entry_t*
 *   std::vector<std::string>               -> char** + size_t (free with
 *                                              iso_reader_free_string_list)
 *   std::vector<pair<string, entry>>       -> iso_reader_named_entry_t* + size_t
 *                                              (free with iso_reader_free_named_entry_list)
 *   std::vector<u8> read buffer            -> u8** + size_t* (caller frees with free())
 *   std::span<const u8> ExtractSectorData  -> const u8** out_data + u32* out_len
 *
 * Progress callback variants of WriteFileToStream are kept; pass NULL to skip.
 */

#ifndef CUPID_UTIL_ISO_READER_H
#define CUPID_UTIL_ISO_READER_H

#include "common/small_string.h"
#include "common/types.h"

#include <stdio.h>

typedef struct cd_image cd_image_t;
typedef struct Error Error;
typedef struct progress_callback progress_callback_t;

enum {
  ISO_READER_SECTOR_SIZE = 2048,
};

#pragma pack(push, 1)

typedef struct {
  u8   type_code;
  char standard_identifier[5];
  u8   version;
} iso_volume_descriptor_header_t;
_Static_assert(sizeof(iso_volume_descriptor_header_t) == 7, "iso_volume_descriptor_header_t size");

typedef struct {
  iso_volume_descriptor_header_t header;
  char boot_system_identifier[32];
  char boot_identifier[32];
  u8   data[1977];
} iso_boot_record_t;
_Static_assert(sizeof(iso_boot_record_t) == 2048, "iso_boot_record_t size");

typedef struct {
  char year[4];
  char month[2];
  char day[2];
  char hour[2];
  char minute[2];
  char second[2];
  char milliseconds[2];
  s8   gmt_offset;
} iso_pvd_date_time_t;
_Static_assert(sizeof(iso_pvd_date_time_t) == 17, "iso_pvd_date_time_t size");

typedef struct {
  iso_volume_descriptor_header_t header;
  u8   unused;
  char system_identifier[32];
  char volume_identifier[32];
  char unused2[8];
  u32  total_sectors_le;
  u32  total_sectors_be;
  char unused3[32];
  u16  volume_set_size_le;
  u16  volume_set_size_be;
  u16  volume_sequence_number_le;
  u16  volume_sequence_number_be;
  u16  block_size_le;
  u16  block_size_be;
  u32  path_table_size_le;
  u32  path_table_size_be;
  u32  path_table_location_le;
  u32  optional_path_table_location_le;
  u32  path_table_location_be;
  u32  optional_path_table_location_be;
  u8   root_directory_entry[34];
  char volume_set_identifier[128];
  char publisher_identifier[128];
  char data_preparer_identifier[128];
  char application_identifier[128];
  char copyright_file_identifier[38];
  char abstract_file_identifier[36];
  char bibliographic_file_identifier[37];
  iso_pvd_date_time_t volume_creation_time;
  iso_pvd_date_time_t volume_modification_time;
  iso_pvd_date_time_t volume_expiration_time;
  iso_pvd_date_time_t volume_effective_time;
  u8   structure_version;
  u8   unused4;
  u8   application_used[512];
  u8   reserved[653];
} iso_primary_volume_descriptor_t;
_Static_assert(sizeof(iso_primary_volume_descriptor_t) == 2048, "iso_primary_volume_descriptor_t size");

typedef struct {
  u8 years_since_1900;
  u8 month;
  u8 day;
  u8 hour;
  u8 minute;
  u8 second;
  s8 gmt_offset;
} iso_directory_entry_date_time_t;

enum {
  ISO_DIRECTORY_ENTRY_FLAG_HIDDEN                    = (1u << 0),
  ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY                 = (1u << 1),
  ISO_DIRECTORY_ENTRY_FLAG_ASSOCIATED_FILE           = (1u << 2),
  ISO_DIRECTORY_ENTRY_FLAG_EXTENDED_ATTRIBUTE_PRESENT = (1u << 3),
  ISO_DIRECTORY_ENTRY_FLAG_OWNER_GROUP_PERMISSIONS   = (1u << 4),
  ISO_DIRECTORY_ENTRY_FLAG_MORE_EXTENTS              = (1u << 7),
};

typedef struct {
  u8  entry_length;
  u8  extended_attribute_length;
  u32 location_le;
  u32 location_be;
  u32 length_le;
  u32 length_be;
  iso_directory_entry_date_time_t recording_time;
  u8  flags;
  u8  interleaved_unit_size;
  u8  interleaved_gap_size;
  u16 sequence_le;
  u16 sequence_be;
  u8  filename_length;
} iso_directory_entry_t;

#pragma pack(pop)

ALWAYS_INLINE bool iso_directory_entry_is_directory(const iso_directory_entry_t* de)
{
  return (de->flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) != 0;
}

ALWAYS_INLINE u32 iso_directory_entry_get_size_in_sectors(const iso_directory_entry_t* de)
{
  return (de->length_le + (ISO_READER_SECTOR_SIZE - 1u)) / ISO_READER_SECTOR_SIZE;
}

/* Format the embedded date/time into out (ASCII strftime("%c")). */
void iso_directory_entry_date_time_get_formatted_time(const iso_directory_entry_date_time_t* dt,
                                                      small_string_t* out);

typedef enum {
  ISO_READER_READ_MODE_DATA  = 0,
  ISO_READER_READ_MODE_MODE2 = 1,
  ISO_READER_READ_MODE_RAW   = 2,
} iso_reader_read_mode_t;

typedef struct {
  char* filename;                  /* heap, NUL-terminated, malloc'd */
  iso_directory_entry_t entry;
} iso_reader_named_entry_t;

void iso_reader_free_string_list(char** array, size_t count);
void iso_reader_free_named_entry_list(iso_reader_named_entry_t* arr, size_t count);

struct iso_reader {
  cd_image_t* image;          /* borrowed */
  u32 track_number;
  iso_primary_volume_descriptor_t pvd;
  u32 pvd_lba;
};

typedef struct iso_reader iso_reader_t;

/* Initialize empty (image=NULL).  Lightweight; safe to call multiple times. */
void iso_reader_init(iso_reader_t* r);

void iso_reader_destroy(iso_reader_t* r);

/* Strips ";N" version suffix from a path component.  Returns a view into the
 * input via *out_data / *out_len. */
void iso_reader_remove_version_identifier_from_path(const char* path, u32 path_len,
                                                    const char** out_data, u32* out_len);

u32 iso_reader_get_read_mode_sector_size(iso_reader_read_mode_t mode);

bool iso_reader_extract_sector_data(const u8* raw_sector, iso_reader_read_mode_t mode,
                                    const u8** out_data, u32* out_len, Error* error);

ALWAYS_INLINE const cd_image_t* iso_reader_get_image(const iso_reader_t* r)        { return r->image; }
ALWAYS_INLINE u32  iso_reader_get_track_number(const iso_reader_t* r)              { return r->track_number; }
ALWAYS_INLINE u32  iso_reader_get_pvd_lba(const iso_reader_t* r)                   { return r->pvd_lba; }
ALWAYS_INLINE const iso_primary_volume_descriptor_t* iso_reader_get_pvd(const iso_reader_t* r)
{
  return &r->pvd;
}

/* Open the data track on the given image.  image must outlive the reader.
 * Returns false (and populates *error when non-NULL) if the track is audio or
 * the PVD cannot be located. */
bool iso_reader_open(iso_reader_t* r, cd_image_t* image, u32 track_number, Error* error);

/* Lists all file paths in the directory.  *out_paths is a heap array of
 * malloc'd strings (length *out_count); free with iso_reader_free_string_list.
 * On failure returns false and leaves *out_paths = NULL, *out_count = 0. */
bool iso_reader_get_files_in_directory(iso_reader_t* r, const char* path,
                                       char*** out_paths, size_t* out_count, Error* error);

 /* Lists (filename, entry) pairs in the directory.  *out_entries is a heap
 * array (length *out_count); free with iso_reader_free_named_entry_list. */
bool iso_reader_get_entries_in_directory(iso_reader_t* r, const char* path,
                                         iso_reader_named_entry_t** out_entries, size_t* out_count,
                                         Error* error);

/* Locates an entry by absolute path within the volume.  Returns true on hit
 * (writes the entry to *out_entry). */
bool iso_reader_locate_file(iso_reader_t* r, const char* path,
                            iso_directory_entry_t* out_entry, Error* error);

bool iso_reader_file_exists      (iso_reader_t* r, const char* path, Error* error);
bool iso_reader_directory_exists (iso_reader_t* r, const char* path, Error* error);

/* Reads the entire file at path into *out_data (heap, malloc'd, *out_len
 * bytes; caller frees with free()). */
bool iso_reader_read_file_path(iso_reader_t* r, const char* path,
                               u8** out_data, size_t* out_len,
                               iso_reader_read_mode_t read_mode, Error* error);

/* Same, given an already-resolved directory entry. */
bool iso_reader_read_file_entry(iso_reader_t* r, const iso_directory_entry_t* de,
                                u8** out_data, size_t* out_len,
                                iso_reader_read_mode_t read_mode, Error* error);

/* Streams the file at path into fp.  fp is rewound + truncated to file size
 * before writing.  progress may be NULL. */
bool iso_reader_write_file_to_stream_path(iso_reader_t* r, const char* path, FILE* fp,
                                          iso_reader_read_mode_t read_mode, Error* error,
                                          progress_callback_t* progress);

bool iso_reader_write_file_to_stream_entry(iso_reader_t* r, const iso_directory_entry_t* de, FILE* fp,
                                           iso_reader_read_mode_t read_mode, Error* error,
                                           progress_callback_t* progress);

#endif /* CUPID_UTIL_ISO_READER_H */
