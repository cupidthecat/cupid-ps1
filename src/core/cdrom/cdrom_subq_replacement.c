#include "cdrom_subq_replacement.h"

#include "common/bcdutils.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDROM);

#pragma pack(push, 1)
typedef struct {
  u8 minute_bcd;
  u8 second_bcd;
  u8 frame_bcd;
  u8 type;
  u8 data[10];
} sbi_file_entry_t;

typedef struct {
  u8 minute_bcd;
  u8 second_bcd;
  u8 frame_bcd;
  u8 data[12];
} lsd_file_entry_t;
#pragma pack(pop)

_Static_assert(sizeof(sbi_file_entry_t) == 14, "SBI entry size");
_Static_assert(sizeof(lsd_file_entry_t) == 15, "LSD entry size");

void cdrom_subq_replacement_init(cdrom_subq_replacement_t* r)
{
  r->entries = NULL;
  r->count = 0;
  r->capacity = 0;
}

void cdrom_subq_replacement_destroy(cdrom_subq_replacement_t* r)
{
  if (!r)
    return;
  free(r->entries);
  r->entries = NULL;
  r->count = 0;
  r->capacity = 0;
}

static bool replacement_grow(cdrom_subq_replacement_t* r, size_t needed)
{
  if (r->capacity >= needed)
    return true;
  size_t new_cap = r->capacity ? r->capacity * 2u : 64u;
  while (new_cap < needed)
    new_cap *= 2u;
  cdrom_subq_replacement_entry_t* new_arr =
    (cdrom_subq_replacement_entry_t*)realloc(r->entries, new_cap * sizeof(cdrom_subq_replacement_entry_t));
  if (!new_arr)
    return false;
  r->entries = new_arr;
  r->capacity = new_cap;
  return true;
}

 /* Insertion-sorted insert by lba, replacing duplicate entries.  Average case
 * is O(N) but the entry list is small and we only build it once. */
static void replacement_insert(cdrom_subq_replacement_t* r, u32 lba, const cd_image_subq_t* subq)
{
  /* Find position. */
  size_t i = 0;
  while (i < r->count && r->entries[i].lba < lba)
    i++;

  if (i < r->count && r->entries[i].lba == lba)
  {
    r->entries[i].subq = *subq;
    return;
  }

  if (!replacement_grow(r, r->count + 1u))
    return;

  if (i < r->count)
  {
    memmove(&r->entries[i + 1], &r->entries[i],
            (r->count - i) * sizeof(cdrom_subq_replacement_entry_t));
  }
  r->entries[i].lba = lba;
  r->entries[i].subq = *subq;
  r->count++;
}

static void subq_pack(cd_image_subq_t* out, const u8 data[12])
{
  /* The on-disc layout matches the struct field order exactly (see static
   * assertion in cd_image.h). */
  memcpy(out, data, 12);
}

static void subq_unpack(const cd_image_subq_t* subq, u8 data[12])
{
  memcpy(data, subq, 12);
}

static bool load_sbi_stream(cdrom_subq_replacement_t* r, const char* path, FILE* fp, Error* error)
{
  static const char expected_header[4] = {'S', 'B', 'I', '\0'};

  char header[4];
  if (fread(header, sizeof(header), 1, fp) != 1 || memcmp(header, expected_header, sizeof(header)) != 0)
  {
    const char* file_name; u32 fn_len;
    path_get_file_name_cstr(path, &file_name, &fn_len);
    Error_set_string_fmt(error, "Invalid header in '%.*s'", (int)fn_len, file_name);
    return false;
  }

  sbi_file_entry_t entry;
  while (fread(&entry, sizeof(entry), 1, fp) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      const char* file_name; u32 fn_len;
      path_get_file_name_cstr(path, &file_name, &fn_len);
      Error_set_string_fmt(error, "Invalid position [%02x:%02x:%02x] in '%.*s'",
                           entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
                           (int)fn_len, file_name);
      return false;
    }

    if (entry.type != 1)
    {
      const char* file_name; u32 fn_len;
      path_get_file_name_cstr(path, &file_name, &fn_len);
      Error_set_string_fmt(error, "Invalid type 0x%02X in '%.*s'", entry.type,
                           (int)fn_len, file_name);
      return false;
    }

    const u32 lba = cd_image_position_to_lba(
      cd_image_position_from_bcd(entry.minute_bcd, entry.second_bcd, entry.frame_bcd));

    cd_image_subq_t subq;
    memset(&subq, 0, sizeof(subq));
    /* SBI provides only the first 10 bytes (control..absolute_frame_bcd);
     * the CRC field is fabricated to be intentionally invalid so callers
     * fall through to the generated-Q codepath when comparing. */
    u8 packed[12];
    memset(packed, 0, sizeof(packed));
    memcpy(packed, entry.data, sizeof(entry.data));
    subq_pack(&subq, packed);
    /* Generate an invalid CRC by inverting the valid one (so it never collides). */
    const u16 crc = (u16)(cd_image_subq_compute_crc(packed) ^ 0xFFFFu);
    subq.crc = (u16)crc;

    replacement_insert(r, lba, &subq);
  }

  const char* file_name; u32 fn_len;
  path_get_file_name_cstr(path, &file_name, &fn_len);
  INFO_LOG("Loaded %zu replacement sectors from SBI '%.*s'", r->count, (int)fn_len, file_name);
  return true;
}

static bool load_lsd_stream(cdrom_subq_replacement_t* r, const char* path, FILE* fp, Error* error)
{
  lsd_file_entry_t entry;
  while (fread(&entry, sizeof(entry), 1, fp) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      const char* file_name; u32 fn_len;
      path_get_file_name_cstr(path, &file_name, &fn_len);
      Error_set_string_fmt(error, "Invalid position [%02x:%02x:%02x] in '%.*s'",
                           entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
                           (int)fn_len, file_name);
      return false;
    }

    const u32 lba = cd_image_position_to_lba(
      cd_image_position_from_bcd(entry.minute_bcd, entry.second_bcd, entry.frame_bcd));

    cd_image_subq_t subq;
    subq_pack(&subq, entry.data);

    replacement_insert(r, lba, &subq);
  }

  INFO_LOG("Loaded %zu replacement sectors from LSD '%s'", r->count, path);
  return true;
}

typedef bool (*loader_fn_t)(cdrom_subq_replacement_t* r, const char* path, FILE* fp, Error* error);

typedef struct {
  const char* extension;
  loader_fn_t func;
} file_loader_t;

static const file_loader_t s_loaders[] = {
  { "sbi", load_sbi_stream },
  { "lsd", load_lsd_stream },
};

/* Tries each loader for `path`.  On success populates *out and returns true
 * (with *result indicating actual load success).  Returns false if file did
 * not exist (try the next candidate). */
static bool try_path(cdrom_subq_replacement_t* out, bool* result,
                     const char* path, loader_fn_t func, Error* error)
{
  FILE* fp = fopen(path, "rb");
  if (!fp)
    return false;

  /* Reset entry list before this loader runs. */
  cdrom_subq_replacement_destroy(out);
  cdrom_subq_replacement_init(out);

  *result = func(out, path, fp, error);
  fclose(fp);

  if (!*result)
  {
    const char* file_name; u32 fn_len;
    path_get_file_name_cstr(path, &file_name, &fn_len);
    Error_add_prefix_fmt(error, "Failed to load subchannel data from %.*s: ",
                         (int)fn_len, file_name);
  }
  return true;
}

 /*
 * configure this; we hard-code "subchannels/" relative to cwd until the
 * frontend layer lands. */
static const char* subchannels_directory(void)
{
  return "subchannels";
}

static bool search_in_subchannels(cdrom_subq_replacement_t* out, bool* result,
                                  const char* base_name, size_t base_name_len, Error* error)
{
  if (base_name_len == 0)
    return false;

  for (size_t i = 0; i < sizeof(s_loaders) / sizeof(s_loaders[0]); i++)
  {
    small_string_t filename;
    small_string_init(&filename);
    small_string_append_sprintf(&filename, "%.*s.%s", (int)base_name_len, base_name, s_loaders[i].extension);

    small_string_t path;
    small_string_init(&path);
    path_combine_cstr2(&path, subchannels_directory(), small_string_c_str(&filename));

    bool found = try_path(out, result, small_string_c_str(&path), s_loaders[i].func, error);
    small_string_destroy(&path);
    small_string_destroy(&filename);

    if (found)
      return true;
  }

  return false;
}

bool cdrom_subq_replacement_load_for_image(cdrom_subq_replacement_t* out_replacement,
                                           bool* out_has_data,
                                           cd_image_t* image,
                                           const char* serial, size_t serial_len,
                                           const char* title, size_t title_len,
                                           const char* save_title, size_t save_title_len,
                                           Error* error)
{
  cdrom_subq_replacement_init(out_replacement);
  if (out_has_data)
    *out_has_data = false;

  const char* image_path = cd_image_get_path(image);
  bool result = true;

  /* We only support file-backed images, so the device-name branch is
   * skipped. */
  const char* file_title_data; u32 file_title_len;
  path_strip_extension_cstr(image_path, &file_title_data, &file_title_len);
  /* Strip directory off file_title_data to get bare title. */
  const char* bare_title; u32 bare_title_len;
  path_get_file_name_view(file_title_data, file_title_len, &bare_title, &bare_title_len);

  for (size_t i = 0; i < sizeof(s_loaders) / sizeof(s_loaders[0]); i++)
  {
    small_string_t filename;
    small_string_init(&filename);
    small_string_append_sprintf(&filename, "%.*s.%s",
                                (int)bare_title_len, bare_title, s_loaders[i].extension);

    small_string_t path;
    small_string_init(&path);
    path_build_relative_cstr(&path, image_path, small_string_c_str(&filename));

    bool found = try_path(out_replacement, &result, small_string_c_str(&path),
                          s_loaders[i].func, error);
    small_string_destroy(&path);
    small_string_destroy(&filename);

    if (found)
    {
      if (out_has_data)
        *out_has_data = result && out_replacement->count > 0;
      return result;
    }
  }

  /* Try the file title first inside subchannels/ (most specific). */
  if (bare_title_len > 0 &&
      search_in_subchannels(out_replacement, &result, bare_title, bare_title_len, error))
  {
    if (out_has_data)
      *out_has_data = result && out_replacement->count > 0;
    return result;
  }

  /* If this fails, try the subchannel directory with serial/title. */
  if (search_in_subchannels(out_replacement, &result, serial, serial_len, error) ||
      search_in_subchannels(out_replacement, &result, title, title_len, error))
  {
    if (out_has_data)
      *out_has_data = result && out_replacement->count > 0;
    return result;
  }

  /* Try save title next if it's different from title. */
  if (save_title_len > 0 &&
      (save_title_len != title_len || memcmp(save_title, title, title_len) != 0))
  {
    if (search_in_subchannels(out_replacement, &result, save_title, save_title_len, error))
    {
      if (out_has_data)
        *out_has_data = result && out_replacement->count > 0;
      return result;
    }
  }

  return true;
}

size_t cdrom_subq_replacement_get_replacement_sector_count(const cdrom_subq_replacement_t* r)
{
  return r->count;
}

const cd_image_subq_t* cdrom_subq_replacement_get_replacement_subq(const cdrom_subq_replacement_t* r, u32 lba)
{
  /* Binary search; the array is kept sorted on insert. */
  size_t lo = 0, hi = r->count;
  while (lo < hi)
  {
    size_t mid = lo + (hi - lo) / 2u;
    if (r->entries[mid].lba == lba)
      return &r->entries[mid].subq;
    if (r->entries[mid].lba < lba)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return NULL;
}
