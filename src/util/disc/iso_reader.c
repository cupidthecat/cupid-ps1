#include "iso_reader.h"

#include "cd_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/progress_callback.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/time_helpers.h"
#include "common/types.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool iso_reader_read_sector(iso_reader_t* r, u8* buf, u32 lsn, Error* error);
static bool iso_reader_read_pvd(iso_reader_t* r, Error* error);
static void iso_reader_get_directory_entry_filename(const u8* sector, u32 de_sector_offset,
                                                    const char** out_data, u32* out_len);
static bool iso_reader_locate_file_inner(iso_reader_t* r, const char* path, u32 path_len,
                                         u8* sector_buffer,
                                         u32 directory_record_lba, u32 directory_record_size,
                                         iso_directory_entry_t* out_entry, Error* error);

void iso_reader_free_string_list(char** array, size_t count)
{
  if (!array)
    return;
  for (size_t i = 0; i < count; i++)
    free(array[i]);
  free(array);
}

void iso_reader_free_named_entry_list(iso_reader_named_entry_t* arr, size_t count)
{
  if (!arr)
    return;
  for (size_t i = 0; i < count; i++)
    free(arr[i].filename);
  free(arr);
}

void iso_reader_init(iso_reader_t* r)
{
  memset(r, 0, sizeof(*r));
}

void iso_reader_destroy(iso_reader_t* r)
{
  if (!r)
    return;
  r->image = NULL;
}

void iso_reader_remove_version_identifier_from_path(const char* path, u32 path_len,
                                                    const char** out_data, u32* out_len)
{
  for (u32 i = 0; i < path_len; i++) {
    if (path[i] == ';') {
      *out_data = path;
      *out_len  = i;
      return;
    }
  }
  *out_data = path;
  *out_len  = path_len;
}

u32 iso_reader_get_read_mode_sector_size(iso_reader_read_mode_t mode)
{
  switch (mode) {
    case ISO_READER_READ_MODE_DATA:  return CD_IMAGE_DATA_SECTOR_SIZE;
    case ISO_READER_READ_MODE_MODE2: return CD_IMAGE_MODE2_DATA_SECTOR_SIZE;
    case ISO_READER_READ_MODE_RAW:   return CD_IMAGE_RAW_SECTOR_SIZE;
    default: UnreachableCode(); return 0;
  }
}

bool iso_reader_extract_sector_data(const u8* raw_sector, iso_reader_read_mode_t mode,
                                    const u8** out_data, u32* out_len, Error* error)
{
  switch (mode) {
    case ISO_READER_READ_MODE_DATA: {
      const cd_image_sector_header_t* header =
        (const cd_image_sector_header_t*)(raw_sector + CD_IMAGE_SECTOR_SYNC_SIZE);
      if (header->sector_mode == 1) {
        *out_data = raw_sector + CD_IMAGE_SECTOR_SYNC_SIZE + CD_IMAGE_MODE1_HEADER_SIZE;
        *out_len  = CD_IMAGE_DATA_SECTOR_SIZE;
        return true;
      } else if (header->sector_mode == 2) {
        *out_data = raw_sector + CD_IMAGE_SECTOR_SYNC_SIZE + CD_IMAGE_MODE2_HEADER_SIZE;
        *out_len  = CD_IMAGE_DATA_SECTOR_SIZE;
        return true;
      }
      Error_set_string_fmt(error, "Invalid sector mode %u", (unsigned)header->sector_mode);
      *out_data = NULL;
      *out_len  = 0;
      return false;
    }

    case ISO_READER_READ_MODE_MODE2: {
      const cd_image_sector_header_t* header =
        (const cd_image_sector_header_t*)(raw_sector + CD_IMAGE_SECTOR_SYNC_SIZE);
      if (header->sector_mode != 2) {
        Error_set_string(error, "Non-mode 2 sector found");
        *out_data = NULL;
        *out_len  = 0;
        return false;
      }
      /* Mode 2 form 0 data starts at the same offset as mode 1; preserve that exactly. */
      *out_data = raw_sector + CD_IMAGE_SECTOR_SYNC_SIZE + CD_IMAGE_MODE1_HEADER_SIZE;
      *out_len  = CD_IMAGE_MODE2_DATA_SECTOR_SIZE;
      return true;
    }

    case ISO_READER_READ_MODE_RAW:
      *out_data = raw_sector;
      *out_len  = CD_IMAGE_RAW_SECTOR_SIZE;
      return true;

    default:
      UnreachableCode();
      *out_data = NULL;
      *out_len  = 0;
      return false;
  }
}

bool iso_reader_open(iso_reader_t* r, cd_image_t* image, u32 track_number, Error* error)
{
  r->image = image;
  r->track_number = track_number;

  if (cd_image_get_track_mode(image, (u8)track_number) == CD_IMAGE_TRACK_MODE_AUDIO) {
    Error_set_string_fmt(error, "Track %u is an audio track.", (unsigned)track_number);
    return false;
  }

  if (!iso_reader_read_pvd(r, error))
    return false;

  return true;
}

static bool iso_reader_read_sector(iso_reader_t* r, u8* buf, u32 lsn, Error* error)
{
  if (!cd_image_seek_track_lba(r->image, r->track_number, lsn)) {
    Error_set_string_fmt(error, "Failed to seek to LSN #%u", (unsigned)lsn);
    return false;
  }

  u8 raw_sector[CD_IMAGE_RAW_SECTOR_SIZE];
  const u8* sector_data = NULL;
  u32 sector_data_len = 0;
  if (!cd_image_read_raw_sector(r->image, raw_sector, NULL) ||
      !iso_reader_extract_sector_data(raw_sector, ISO_READER_READ_MODE_DATA,
                                      &sector_data, &sector_data_len, error)) {
    Error_set_string_fmt(error, "Failed to read LSN #%u", (unsigned)lsn);
    return false;
  }

  Assert(sector_data_len >= ISO_READER_SECTOR_SIZE);
  memcpy(buf, sector_data, ISO_READER_SECTOR_SIZE);
  return true;
}

static bool iso_reader_read_pvd(iso_reader_t* r, Error* error)
{
  /* Volume descriptors start at sector 16; bound the search to 256 to avoid
   * reading the whole image on a malformed disc. */
  enum { START_SECTOR = 16 };
  u8 buffer[ISO_READER_SECTOR_SIZE];
  for (u32 i = 0; i < 256; i++) {
    if (!iso_reader_read_sector(r, buffer, START_SECTOR + i, error))
      return false;

    const iso_volume_descriptor_header_t* header = (const iso_volume_descriptor_header_t*)buffer;
    if (memcmp(header->standard_identifier, "CD001", 5) != 0)
      continue;
    if (header->type_code != 1)
      continue;
    if (header->type_code == 255)
      break;

    r->pvd_lba = START_SECTOR + i;
    memcpy(&r->pvd, buffer, sizeof(iso_primary_volume_descriptor_t));
    return true;
  }

  Error_set_string(error, "Failed to find the Primary Volume Descriptor.");
  return false;
}

bool iso_reader_locate_file(iso_reader_t* r, const char* path,
                            iso_directory_entry_t* out_entry, Error* error)
{
  const iso_directory_entry_t* root_de =
    (const iso_directory_entry_t*)r->pvd.root_directory_entry;

  const u32 path_len = path ? (u32)strlen(path) : 0u;
  if (path_len == 0u || (path_len == 1u && (path[0] == '/' || path[0] == '\\'))) {
    /* Locating the root directory itself. */
    *out_entry = *root_de;
    return true;
  }

  u8 sector_buffer[ISO_READER_SECTOR_SIZE];
  return iso_reader_locate_file_inner(r, path, path_len, sector_buffer,
                                      root_de->location_le, root_de->length_le,
                                      out_entry, error);
}

static void iso_reader_get_directory_entry_filename(const u8* sector, u32 de_sector_offset,
                                                    const char** out_data, u32* out_len)
{
  const iso_directory_entry_t* de = (const iso_directory_entry_t*)(sector + de_sector_offset);
  if ((sizeof(iso_directory_entry_t) + de->filename_length) > de->entry_length ||
      (sizeof(iso_directory_entry_t) + de->filename_length + de_sector_offset) > ISO_READER_SECTOR_SIZE) {
    *out_data = NULL;
    *out_len  = 0;
    return;
  }

  const char* str = (const char*)(sector + de_sector_offset + sizeof(iso_directory_entry_t));
  if (de->filename_length == 1) {
    if (str[0] == '\0') {
      *out_data = ".";
      *out_len  = 1u;
      return;
    }
    if (str[0] == '\1') {
      *out_data = "..";
      *out_len  = 2u;
      return;
    }
  }

  /* Strip ";version" the way the PS2 BIOS does. */
  u32 length_without_version = 0;
  for (; length_without_version < de->filename_length; length_without_version++) {
    if (str[length_without_version] == ';' || str[length_without_version] == '\0')
      break;
  }

  *out_data = str;
  *out_len  = length_without_version;
}

static bool iso_reader_locate_file_inner(iso_reader_t* r, const char* path, u32 path_len,
                                         u8* sector_buffer,
                                          u32 directory_record_lba, u32 directory_record_size,
                                         iso_directory_entry_t* out_entry, Error* error) 
{
  /* Strip leading slashes. */
  u32 path_component_start = 0;
  while (path_component_start < path_len &&
         (path[path_component_start] == '/' || path[path_component_start] == '\\')) {
    path_component_start++;
  }

  u32 path_component_length = 0;
  while ((path_component_start + path_component_length) < path_len &&
         path[path_component_start + path_component_length] != '/' &&
         path[path_component_start + path_component_length] != '\\') {
    path_component_length++;
  }

  if (path_component_length == 0u) {
    Error_set_string_fmt(error, "Empty path component in %.*s", (int)path_len, path);
    return false;
  }

  const char* path_component = path + path_component_start;

  /* Walk directory entries.  A 0-sized directory record is unusual but the
   * original code treats it as one sector, so keep that. */
  const u32 num_sectors =
    (directory_record_size == 0u)
      ? 1u
      : ((directory_record_size + (ISO_READER_SECTOR_SIZE - 1u)) / ISO_READER_SECTOR_SIZE);

  for (u32 i = 0; i < num_sectors; i++) {
    if (!iso_reader_read_sector(r, sector_buffer, directory_record_lba + i, error))
      return false;

    u32 sector_offset = 0;
    while ((sector_offset + sizeof(iso_directory_entry_t)) < ISO_READER_SECTOR_SIZE) {
      const iso_directory_entry_t* de =
        (const iso_directory_entry_t*)(sector_buffer + sector_offset);
      if (de->entry_length < sizeof(iso_directory_entry_t))
        break;

      const char* de_filename_data = NULL;
      u32 de_filename_len = 0;
      iso_reader_get_directory_entry_filename(sector_buffer, sector_offset,
                                              &de_filename_data, &de_filename_len);
      sector_offset += de->entry_length;

      /* Skip empty / "." / ".." entries. */
      if (de_filename_len == 0u)
        continue;
      if (de_filename_len == 1u && de_filename_data[0] == '.')
        continue;
      if (de_filename_len == 2u && de_filename_data[0] == '.' && de_filename_data[1] == '.')
        continue;

      if (de_filename_len != path_component_length ||
          string_util_strncasecmp(de_filename_data, path_component, path_component_length) != 0) {
        continue;
      }

      /* Found.  Was this the final path component? */
      if ((path_component_start + path_component_length) == path_len) {
        *out_entry = *de;
        return true;
      }

      if (de->flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) {
        const u32 remaining_start = path_component_start + path_component_length;
        return iso_reader_locate_file_inner(r,
                                            path + remaining_start, path_len - remaining_start,
                                            sector_buffer,
                                            de->location_le, de->length_le,
                                            out_entry, error);
      }

      Error_set_string_fmt(error, "Looking for directory '%.*s' but got file",
                           (int)path_component_length, path_component);
      return false;
    }
  }

  Error_set_string_fmt(error, "Path component '%.*s' not found",
                       (int)path_component_length, path_component);
  return false;
}

/* Out: directory_record_lsn, directory_record_length, base_path (heap, for prepend).
 * On success, returns true; *out_base_path is malloc'd (may be empty string).
 * On failure returns false and writes nothing the caller need free. */
static bool iso_reader_resolve_directory(iso_reader_t* r, const char* path,
                                          u32* out_directory_record_lsn,
                                         u32* out_directory_record_length,
                                         char** out_base_path,
                                         Error* error) 
{
  const u32 path_len = path ? (u32)strlen(path) : 0u;
  if (path_len == 0u) {
    const iso_directory_entry_t* root_de =
      (const iso_directory_entry_t*)r->pvd.root_directory_entry;
    *out_directory_record_lsn    = root_de->location_le;
    *out_directory_record_length = root_de->length_le;
    *out_base_path = (char*)malloc(1);
    if (!*out_base_path) {
      Error_set_errno(error, ENOMEM);
      return false;
    }
    (*out_base_path)[0] = '\0';
    return true;
  }

  iso_directory_entry_t directory_de;
  if (!iso_reader_locate_file(r, path, &directory_de, error))
    return false;

  if ((directory_de.flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) == 0) {
    Error_set_string_fmt(error, "Path '%s' is not a directory, can't list", path);
    return false;
  }

  *out_directory_record_lsn    = directory_de.location_le;
  *out_directory_record_length = directory_de.length_le;

  /* Append trailing '/' if missing so the returned filenames look like full
   * paths. */
  const bool needs_slash = (path[path_len - 1u] != '/');
  const size_t cap = (size_t)path_len + (needs_slash ? 1u : 0u) + 1u;
  *out_base_path = (char*)malloc(cap);
  if (!*out_base_path) {
    Error_set_errno(error, ENOMEM);
    return false;
  }
  memcpy(*out_base_path, path, path_len);
  if (needs_slash)
    (*out_base_path)[path_len] = '/';
  (*out_base_path)[cap - 1u] = '\0';
  return true;
}

bool iso_reader_get_files_in_directory(iso_reader_t* r, const char* path,
                                       char*** out_paths, size_t* out_count, Error* error)
{
  *out_paths = NULL;
  *out_count = 0;

  u32 directory_record_lsn, directory_record_length;
  char* base_path = NULL;
  if (!iso_reader_resolve_directory(r, path,
                                    &directory_record_lsn, &directory_record_length,
                                    &base_path, error)) {
    return false;
  }
  const size_t base_path_len = strlen(base_path);

  const u32 num_sectors =
    (directory_record_length + (ISO_READER_SECTOR_SIZE - 1u)) / ISO_READER_SECTOR_SIZE;

  char** files = NULL;
  size_t file_count = 0;
  size_t file_capacity = 0;
  u8 sector_buffer[ISO_READER_SECTOR_SIZE];

  for (u32 i = 0; i < num_sectors; i++) {
    if (!iso_reader_read_sector(r, sector_buffer, directory_record_lsn + i, error))
      break;

    u32 sector_offset = 0;
    while ((sector_offset + sizeof(iso_directory_entry_t)) < ISO_READER_SECTOR_SIZE) {
      const iso_directory_entry_t* de =
        (const iso_directory_entry_t*)(sector_buffer + sector_offset);
      if (de->entry_length < sizeof(iso_directory_entry_t))
        break;

      const char* de_filename_data = NULL;
      u32 de_filename_len = 0;
      iso_reader_get_directory_entry_filename(sector_buffer, sector_offset,
                                              &de_filename_data, &de_filename_len);
      sector_offset += de->entry_length;

      if (de_filename_len == 0u)
        continue;
      if (de_filename_len == 1u && de_filename_data[0] == '.')
        continue;
      if (de_filename_len == 2u && de_filename_data[0] == '.' && de_filename_data[1] == '.')
        continue;

      if (file_count == file_capacity) {
        const size_t new_cap = file_capacity ? (file_capacity * 2u) : 16u;
        char** new_files = (char**)realloc(files, new_cap * sizeof(*files));
        if (!new_files) {
          Error_set_errno(error, ENOMEM);
          iso_reader_free_string_list(files, file_count);
          free(base_path);
          return false;
        }
        files = new_files;
        file_capacity = new_cap;
      }

      const size_t total_len = base_path_len + de_filename_len;
      char* full = (char*)malloc(total_len + 1u);
      if (!full) {
        Error_set_errno(error, ENOMEM);
        iso_reader_free_string_list(files, file_count);
        free(base_path);
        return false;
      }
      memcpy(full, base_path, base_path_len);
      memcpy(full + base_path_len, de_filename_data, de_filename_len);
      full[total_len] = '\0';
      files[file_count++] = full;
    }
  }

  free(base_path);
  *out_paths = files;
  *out_count = file_count;
  return true;
}

bool iso_reader_get_entries_in_directory(iso_reader_t* r, const char* path,
                                          iso_reader_named_entry_t** out_entries, size_t* out_count,
                                         Error* error) 
{
  *out_entries = NULL;
  *out_count = 0;

  u32 directory_record_lsn, directory_record_length;
  char* base_path = NULL;
  if (!iso_reader_resolve_directory(r, path,
                                    &directory_record_lsn, &directory_record_length,
                                    &base_path, error)) {
    return false;
  }
  const size_t base_path_len = strlen(base_path);

  const u32 num_sectors =
    (directory_record_length + (ISO_READER_SECTOR_SIZE - 1u)) / ISO_READER_SECTOR_SIZE;

  iso_reader_named_entry_t* entries = NULL;
  size_t entry_count = 0;
  size_t entry_capacity = 0;
  u8 sector_buffer[ISO_READER_SECTOR_SIZE];

  for (u32 i = 0; i < num_sectors; i++) {
    if (!iso_reader_read_sector(r, sector_buffer, directory_record_lsn + i, error))
      break;

    u32 sector_offset = 0;
    while ((sector_offset + sizeof(iso_directory_entry_t)) < ISO_READER_SECTOR_SIZE) {
      const iso_directory_entry_t* de =
        (const iso_directory_entry_t*)(sector_buffer + sector_offset);
      if (de->entry_length < sizeof(iso_directory_entry_t))
        break;

      const char* de_filename_data = NULL;
      u32 de_filename_len = 0;
      iso_reader_get_directory_entry_filename(sector_buffer, sector_offset,
                                              &de_filename_data, &de_filename_len);
      const iso_directory_entry_t entry_copy = *de;
      sector_offset += de->entry_length;

      if (de_filename_len == 0u)
        continue;
      if (de_filename_len == 1u && de_filename_data[0] == '.')
        continue;
      if (de_filename_len == 2u && de_filename_data[0] == '.' && de_filename_data[1] == '.')
        continue;

      if (entry_count == entry_capacity) {
        const size_t new_cap = entry_capacity ? (entry_capacity * 2u) : 16u;
        iso_reader_named_entry_t* new_entries =
          (iso_reader_named_entry_t*)realloc(entries, new_cap * sizeof(*entries));
        if (!new_entries) {
          Error_set_errno(error, ENOMEM);
          iso_reader_free_named_entry_list(entries, entry_count);
          free(base_path);
          return false;
        }
        entries = new_entries;
        entry_capacity = new_cap;
      }

      const size_t total_len = base_path_len + de_filename_len;
      char* full = (char*)malloc(total_len + 1u);
      if (!full) {
        Error_set_errno(error, ENOMEM);
        iso_reader_free_named_entry_list(entries, entry_count);
        free(base_path);
        return false;
      }
      memcpy(full, base_path, base_path_len);
      memcpy(full + base_path_len, de_filename_data, de_filename_len);
      full[total_len] = '\0';
      entries[entry_count].filename = full;
      entries[entry_count].entry    = entry_copy;
      entry_count++;
    }
  }

  free(base_path);
  *out_entries = entries;
  *out_count = entry_count;
  return true;
}

bool iso_reader_file_exists(iso_reader_t* r, const char* path, Error* error)
{
  iso_directory_entry_t de;
  if (!iso_reader_locate_file(r, path, &de, error))
    return false;
  return (de.flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) == 0;
}

bool iso_reader_directory_exists(iso_reader_t* r, const char* path, Error* error)
{
  iso_directory_entry_t de;
  if (!iso_reader_locate_file(r, path, &de, error))
    return false;
  return (de.flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) != 0;
}

bool iso_reader_read_file_path(iso_reader_t* r, const char* path,
                               u8** out_data, size_t* out_len,
                               iso_reader_read_mode_t read_mode, Error* error)
{
  iso_directory_entry_t de;
  if (!iso_reader_locate_file(r, path, &de, error))
    return false;
  return iso_reader_read_file_entry(r, &de, out_data, out_len, read_mode, error);
}

bool iso_reader_read_file_entry(iso_reader_t* r, const iso_directory_entry_t* de,
                                u8** out_data, size_t* out_len,
                                iso_reader_read_mode_t read_mode, Error* error)
{
  *out_data = NULL;
  *out_len  = 0;

  if (de->flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) {
    Error_set_string(error, "File is a directory");
    return false;
  }

  if (de->length_le == 0u) {
    return true;
  }

   /*
   * track index (the original ReadFile does the same). */
  if (!cd_image_seek_track_lba(r->image, 1u, de->location_le)) {
    Error_set_string_fmt(error, "Failed to seek to LSN #%u", (unsigned)de->location_le);
    return false;
  }

  const u32 sector_size = iso_reader_get_read_mode_sector_size(read_mode);
  const u32 num_sectors = iso_directory_entry_get_size_in_sectors(de);
  const size_t capacity = (size_t)num_sectors * (size_t)sector_size;
  u8* data = (u8*)malloc(capacity);
  if (!data) {
    Error_set_errno(error, ENOMEM);
    return false;
  }

  u8 raw_sector[CD_IMAGE_RAW_SECTOR_SIZE];
  size_t data_offset = 0;
  for (u32 i = 0; i < num_sectors; i++) {
    const u8* sector_data = NULL;
    u32 sector_data_len = 0;
    if (!cd_image_read_raw_sector(r->image, raw_sector, NULL) ||
        !iso_reader_extract_sector_data(raw_sector, read_mode,
                                        &sector_data, &sector_data_len, error)) {
      Error_add_prefix_fmt(error, "Failed to read LSN #%u: ",
                           (unsigned)(de->location_le + i));
      free(data);
      return false;
    }

    memcpy(data + data_offset, sector_data, sector_data_len);
    data_offset += sector_data_len;
  }

  size_t final_len = capacity;
  if (read_mode == ISO_READER_READ_MODE_DATA)
    final_len = de->length_le;

  *out_data = data;
  *out_len  = final_len;
  return true;
}

bool iso_reader_write_file_to_stream_path(iso_reader_t* r, const char* path, FILE* fp,
                                           iso_reader_read_mode_t read_mode, Error* error,
                                          progress_callback_t* progress) 
{
  iso_directory_entry_t de;
  if (!iso_reader_locate_file(r, path, &de, error))
    return false;
  return iso_reader_write_file_to_stream_entry(r, &de, fp, read_mode, error, progress);
}

bool iso_reader_write_file_to_stream_entry(iso_reader_t* r, const iso_directory_entry_t* de, FILE* fp,
                                            iso_reader_read_mode_t read_mode, Error* error,
                                           progress_callback_t* progress) 
{
  if (de->flags & ISO_DIRECTORY_ENTRY_FLAG_DIRECTORY) {
    Error_set_string(error, "File is a directory");
    return false;
  }

  if (!fs_fseek64_e(fp, 0, SEEK_SET, error))
    return false;

  if (de->length_le == 0u)
    return fs_ftruncate64(fp, 0, error);

  /* Same hardcoded track 1 as the original; the ISO data track is always 1
   * for PS1 discs. */
  if (!cd_image_seek_track_lba(r->image, 1u, de->location_le)) {
    Error_set_string_fmt(error, "Failed to seek to LSN #%u", (unsigned)de->location_le);
    return false;
  }

  if (progress) {
    progress_callback_set_progress_range(progress, de->length_le);
    progress_callback_set_progress_value(progress, 0);
  }

  const u32 num_sectors = iso_directory_entry_get_size_in_sectors(de);

  u8 raw_sector[CD_IMAGE_RAW_SECTOR_SIZE];
  u32 file_pos = 0;

  for (u32 i = 0; i < num_sectors; i++) {
    const u8* sector_data = NULL;
    u32 sector_data_len = 0;
    if (!cd_image_read_raw_sector(r->image, raw_sector, NULL) ||
        !iso_reader_extract_sector_data(raw_sector, read_mode,
                                        &sector_data, &sector_data_len, error)) {
      Error_add_prefix_fmt(error, "Failed to read LSN #%u: ",
                           (unsigned)(de->location_le + i));
      return false;
    }

    /* Only trim the trailing partial sector when in Data mode. */
    u32 write_size = sector_data_len;
    if (read_mode == ISO_READER_READ_MODE_DATA) {
      const u32 remaining = de->length_le - file_pos;
      if (remaining < write_size)
        write_size = remaining;
    }
    if (fwrite(sector_data, write_size, 1u, fp) != 1u) {
      Error_set_errno_prefix(error, "fwrite() failed: ", errno);
      return false;
    }

    file_pos += write_size;
    if (progress) {
      progress_callback_set_progress_value(progress, file_pos);
      if (progress_callback_is_cancelled(progress)) {
        Error_set_string(error, "Operation was cancelled.");
        return false;
      }
    }
  }

  if (fflush(fp) != 0) {
    Error_set_errno_prefix(error, "fflush() failed: ", errno);
    return false;
  }

  return true;
}

void iso_directory_entry_date_time_get_formatted_time(const iso_directory_entry_date_time_t* dt,
                                                      small_string_t* out)
{
  /* Apply the UTC offset by routing through unix time so the strftime()
   * output below uses the host's locale-aware "%c" representation. */
  struct tm utime;
  memset(&utime, 0, sizeof(utime));
  utime.tm_year = dt->years_since_1900;
  utime.tm_mon  = (dt->month > 0) ? (dt->month - 1) : 0;
  utime.tm_mday = (dt->day > 0)   ? (dt->day - 1)   : 0;
  utime.tm_hour = dt->hour;
  utime.tm_min  = dt->minute;
  utime.tm_sec  = dt->second;

  const s32 uts_offset = (s32)dt->gmt_offset * 3600;
  const time_t uts = mktime(&utime) + uts_offset;

  small_string_resize(out, 128, '\0', false);

  struct tm ltime;
  if (common_local_time(uts, &ltime)) {
    char* buf = small_string_data(out);
    const size_t written = strftime(buf, small_string_buffer_size(out), "%c", &ltime);
    small_string_set_size(out, (u32)written, false);
  } else {
    small_string_assign_cstr(out, "Invalid");
  }
}
