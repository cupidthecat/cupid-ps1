#include "util/object_archive.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CompressHelpers);

const char OBJECT_ARCHIVE_ERROR_DOES_NOT_EXIST[] = "Key not found in archive.";
const char OBJECT_ARCHIVE_ERROR_ALREADY_EXISTS[] = "Key already exists in archive.";

#pragma pack(push, 4)
typedef struct {
  u32 signature;
  u32 cache_version;
} cache_file_header_t;

typedef struct {
  u32 file_offset;
  u32 compressed_size;
  u32 uncompressed_size;
  u16 key_size_low;
  u8  key_size_high;
  u8  compress_type;
} cache_index_entry_header_t;
#pragma pack(pop)

enum {
  EXPECTED_SIGNATURE = 0x41435544u, /* DUCA */
  MAX_KEY_SIZE       = (1u << 24) - 1u,
};

static u32  ie_get_key_size(const cache_index_entry_header_t* h);
static void ie_set_key_size(cache_index_entry_header_t* h, u32 size);

static u32 ie_get_key_size(const cache_index_entry_header_t* h)
{
  return ((u32)h->key_size_high << 16) | (u32)h->key_size_low;
}

static void ie_set_key_size(cache_index_entry_header_t* h, u32 size)
{
  h->key_size_low  = (u16)(size & 0xFFFFu);
  h->key_size_high = (u8)((size >> 16) & 0xFFu);
}

void object_archive_init(object_archive_t* a)
{
  memset(a, 0, sizeof(*a));
}

bool object_archive_is_open(const object_archive_t* a) { return (a->index_file != NULL); }
u32  object_archive_size   (const object_archive_t* a) { return a->entry_count; }

static void free_entries(object_archive_t* a)
{
  for (u32 i = 0; i < a->entry_count; i++)
    free(a->entries[i].key);
  free(a->entries);
  a->entries        = NULL;
  a->entry_count    = 0;
  a->entry_capacity = 0;
}

void object_archive_close(object_archive_t* a)
{
  if (a->index_file) { fclose(a->index_file); a->index_file = NULL; }
  if (a->blob_file)  { fclose(a->blob_file);  a->blob_file  = NULL; }
  free_entries(a);
}

void object_archive_destroy(object_archive_t* a)
{
  object_archive_close(a);
}

bool object_archive_clear(object_archive_t* a, Error* error)
{
  if (!object_archive_is_open(a)) return true;

  WARNING_LOG("Clearing object cache");

  if (!fs_fseek64_e(a->index_file, 0, SEEK_SET, error) ||
      !fs_fseek64_e(a->blob_file,  0, SEEK_SET, error) ||
      !fs_ftruncate64(a->blob_file,  0, error) ||
      !fs_ftruncate64(a->index_file, sizeof(cache_file_header_t), error) || 
      !fs_fseek64_e(a->index_file, 0, SEEK_END, error)) {
    ERROR_LOG("Failed to seek/truncate object cache");
    object_archive_close(a);
    return false;
  }

  free_entries(a);
  return true;
}

static int entry_cmp_key(const void* va, const void* vb)
{
  const object_archive_entry_t* a = (const object_archive_entry_t*)va;
  const object_archive_entry_t* b = (const object_archive_entry_t*)vb;
  return strcmp(a->key, b->key);
}

/* Binary search by key.  Returns insert position in *idx; sets *found. */
static void find_entry(const object_archive_t* a, const char* key,
                       u32* out_idx, bool* out_found)
{
  u32 lo = 0, hi = a->entry_count;
  while (lo < hi) {
    const u32 mid = lo + (hi - lo) / 2u;
    const int c = strcmp(a->entries[mid].key, key);
    if (c == 0) { *out_idx = mid; *out_found = true; return; }
    if (c < 0) lo = mid + 1u;
    else       hi = mid;
  }
  *out_idx   = lo;
  *out_found = false;
}

static bool ensure_capacity(object_archive_t* a, u32 want, Error* error)
{
  if (want <= a->entry_capacity) return true;
  u32 new_cap = a->entry_capacity ? (a->entry_capacity * 2u) : 16u;
  while (new_cap < want) new_cap *= 2u;
  object_archive_entry_t* tmp = (object_archive_entry_t*)realloc(a->entries, new_cap * sizeof(*tmp));
  if (!tmp) {
    Error_set_string(error, "out of memory growing entry array");
    return false;
  }
  a->entries        = tmp;
  a->entry_capacity = new_cap;
  return true;
}

static bool create_new_files(object_archive_t* a, u32 version, Error* error)
{
  cache_file_header_t fh = { EXPECTED_SIGNATURE, version };
  if (fwrite(&fh, sizeof(fh), 1, a->index_file) != 1) {
    Error_set_errno_prefix(error, "fwrite() for header failed: ", errno);
    return false;
  }
  return true;
}

static bool read_existing(object_archive_t* a, u32 version, Error* error)
{
  cache_file_header_t fh;
  if (fread(&fh, sizeof(fh), 1, a->index_file) != 1 ||
      fh.signature != EXPECTED_SIGNATURE || fh.cache_version != version) {
    Error_set_string_fmt(error, "Bad file/data version (expected %u, got %u)",
                         version, fh.cache_version);
    object_archive_close(a);
    return false;
  }

  const s64 index_file_size = fs_fsize64(a->index_file, error);
  if (index_file_size < 0 || index_file_size > 1 * 1024 * 1024) {
    Error_set_string_fmt(error, "Index file is too large (%lld bytes)", (long long)index_file_size);
    object_archive_close(a);
    return false;
  }

  const s64 blob_file_size = fs_fsize64(a->blob_file, error);
  if (blob_file_size < 0) {
    object_archive_close(a);
    return false;
  }

  /* Re-seek to past the header to start scanning entries. */
  if (!fs_fseek64_e(a->index_file, sizeof(fh), SEEK_SET, error)) {
    object_archive_close(a);
    return false;
  }

  for (;;) {
    cache_index_entry_header_t kh;
    if (fread(&kh, sizeof(kh), 1, a->index_file) != 1) {
      if (feof(a->index_file)) break;
      Error_set_errno_prefix(error, "fread() failed: ", errno);
      object_archive_close(a);
      return false;
    }

    const u32 key_size = ie_get_key_size(&kh);
    if (key_size == 0 || key_size > MAX_KEY_SIZE ||
        ((u64)kh.file_offset + (u64)kh.compressed_size) > (u64)blob_file_size ||
        (kh.compress_type != COMPRESS_TYPE_UNCOMPRESSED && kh.compress_type != COMPRESS_TYPE_DEFLATE)) {
      Error_set_string(error, "Corrupt index entry");
      object_archive_close(a);
      return false;
    }

    char* key = (char*)malloc(key_size + 1u);
    if (!key) {
      Error_set_string(error, "out of memory reading entry key");
      object_archive_close(a);
      return false;
    }
    if (fread(key, key_size, 1, a->index_file) != 1) {
      free(key);
      Error_set_errno_prefix(error, "fread() for key failed: ", errno);
      object_archive_close(a);
      return false;
    }
    key[key_size] = '\0';

    if (!ensure_capacity(a, a->entry_count + 1u, error)) {
      free(key);
      object_archive_close(a);
      return false;
    }
    object_archive_entry_t* e = &a->entries[a->entry_count++];
    e->file_offset       = kh.file_offset;
    e->compressed_size   = kh.compressed_size;
    e->uncompressed_size = kh.uncompressed_size;
    e->key_size          = key_size;
    e->compress_type     = (compress_type_t)kh.compress_type;
    e->key               = key;
  }

  if (!fs_fseek64_e(a->index_file, 0, SEEK_END, error)) {
    object_archive_close(a);
    return false;
  }

  /* Sort + dedupe. */
  if (a->entry_count > 1) {
    qsort(a->entries, a->entry_count, sizeof(*a->entries), entry_cmp_key);
    for (u32 i = 1; i < a->entry_count; i++) {
      if (strcmp(a->entries[i - 1].key, a->entries[i].key) == 0) {
        Error_set_string_fmt(error, "Duplicate key '%s' in index file", a->entries[i].key);
        object_archive_close(a);
        return false;
      }
    }
  }

  return true;
}

bool object_archive_open_files(object_archive_t* a, FILE* index_file,
                               FILE* blob_file, u32 data_version, Error* error)
{
  object_archive_close(a);
  a->index_file = index_file;
  a->blob_file  = blob_file;
  return read_existing(a, data_version, error);
}

bool object_archive_create_files(object_archive_t* a, FILE* index_file,
                                 FILE* blob_file, u32 data_version, Error* error)
{
  object_archive_close(a);
  a->index_file = index_file;
  a->blob_file  = blob_file;
  return create_new_files(a, data_version, error);
}

bool object_archive_open_path(object_archive_t* a, const char* base_path,
                              u32 data_version, Error* error)
{
  object_archive_close(a);

  /* "<base>.idx" / "<base>.bin" */
  const size_t blen = strlen(base_path);
  char* idx_path = (char*)malloc(blen + 5u);
  char* bin_path = (char*)malloc(blen + 5u);
  if (!idx_path || !bin_path) {
    free(idx_path); free(bin_path);
    Error_set_string(error, "out of memory in object_archive_open_path");
    return false;
  }
  memcpy(idx_path, base_path, blen); memcpy(idx_path + blen, ".idx", 5);
  memcpy(bin_path, base_path, blen); memcpy(bin_path + blen, ".bin", 5);

  bool ok = false;
  if (fs_file_exists(idx_path)) {
    Error open_err = ERROR_INIT;
    a->index_file = fs_open_file(idx_path, "r+b", &open_err);
    if (a->index_file) {
      a->blob_file = fs_open_file(bin_path, "a+b", &open_err);
      if (a->blob_file) {
        ok = read_existing(a, data_version, &open_err);
      } else {
        object_archive_close(a);
      }
    }
    if (!ok)
      ERROR_LOG("Failed to open existing object archive '%s': %s",
                idx_path, Error_get_description(&open_err));
    Error_destroy(&open_err);
  }

  if (!ok) {
    /* Recreate. */
    if (fs_file_exists(bin_path)) fs_delete_file(bin_path, NULL);
    if (fs_file_exists(idx_path)) fs_delete_file(idx_path, NULL);

    a->index_file = fs_open_file(idx_path, "wb", error);
    if (a->index_file) {
      a->blob_file = fs_open_file(bin_path, "w+b", error);
      if (a->blob_file) {
        ok = create_new_files(a, data_version, error);
      } else {
        object_archive_close(a);
        fs_delete_file(idx_path, NULL);
      }
    }
    if (!ok)
      object_archive_close(a);
  }

  free(idx_path);
  free(bin_path);
  return ok;
}

bool object_archive_lookup(object_archive_t* a, const char* key,
                           u8** out_data, size_t* out_len, Error* error)
{
  u32  idx;
  bool found;
  find_entry(a, key, &idx, &found);
  if (!found) {
    Error_set_string(error, OBJECT_ARCHIVE_ERROR_DOES_NOT_EXIST);
    return false;
  }

  const object_archive_entry_t* e = &a->entries[idx];
  u8* raw = (u8*)malloc(e->compressed_size);
  if (!raw) {
    Error_set_string(error, "out of memory in lookup");
    return false;
  }
  if (fseek(a->blob_file, e->file_offset, SEEK_SET) != 0 ||
      fread(raw, e->compressed_size, 1, a->blob_file) != 1) {
    Error_set_errno(error, errno);
    free(raw);
    return false;
  }

  if (e->compress_type == COMPRESS_TYPE_UNCOMPRESSED) {
    *out_data = raw;
    *out_len  = e->compressed_size;
    return true;
  }

  u8* dst = (u8*)malloc(e->uncompressed_size > 0 ? e->uncompressed_size : 1);
  if (!dst) {
    free(raw);
    Error_set_string(error, "out of memory in lookup decompress");
    return false;
  }
  if (!compress_helpers_decompress_buffer(e->compress_type,
                                          dst, e->uncompressed_size,
                                          raw, e->compressed_size, error)) {
    free(raw); free(dst);
    return false;
  }
  free(raw);
  *out_data = dst;
  *out_len  = e->uncompressed_size;
  return true;
}

bool object_archive_contains(const object_archive_t* a, const char* key)
{
  if (!key || !key[0] || strlen(key) > MAX_KEY_SIZE || !object_archive_is_open(a))
    return false;
  u32 idx; bool found;
  find_entry(a, key, &idx, &found);
  return found;
}

bool object_archive_insert(object_archive_t* a, const char* key,
                           const void* data, size_t data_size,
                           compress_type_t compression, Error* error)
{
  if (!key || !key[0]) {
    Error_set_string(error, "Invalid key.");
    return false;
  }
  const size_t keylen = strlen(key);
  if (keylen > MAX_KEY_SIZE) {
    Error_set_string(error, "Key too long.");
    return false;
  }
  if (!object_archive_is_open(a)) {
    Error_set_string(error, "Archive is not open.");
    return false;
  }

  u32 idx; bool found;
  find_entry(a, key, &idx, &found);
  if (found) {
    Error_set_string(error, OBJECT_ARCHIVE_ERROR_ALREADY_EXISTS);
    return false;
  }

  /* Compress (or pass-through). */
  u8* write_buf = NULL;
  size_t write_len = 0;
  bool   need_free = false;
  if (compression == COMPRESS_TYPE_UNCOMPRESSED) {
    write_buf = (u8*)data;
    write_len = data_size;
  } else {
    if (!compress_helpers_compress_alloc(compression, (const u8*)data, data_size,
                                         -1, &write_buf, &write_len, error))
      return false;
    need_free = true;
  }

  if (!fs_fseek64_e(a->blob_file, 0, SEEK_END, error)) {
    if (need_free) free(write_buf);
    return false;
  }
  const s64 file_offset = fs_ftell64(a->blob_file);
  if (file_offset < 0) {
    if (need_free) free(write_buf);
    Error_set_errno_prefix(error, "ftell() failed: ", errno);
    return false;
  }

  cache_index_entry_header_t kh;
  memset(&kh, 0, sizeof(kh));
  kh.file_offset       = (u32)file_offset;
  kh.compressed_size   = (u32)write_len;
  kh.uncompressed_size = (u32)data_size;
  kh.compress_type     = (u8)compression;
  ie_set_key_size(&kh, (u32)keylen);

  if (fwrite(write_buf, write_len, 1, a->blob_file) != 1 ||
      fflush(a->blob_file) != 0 ||
      fwrite(&kh, sizeof(kh), 1, a->index_file) != 1 ||
      fwrite(key, keylen, 1, a->index_file) != 1 ||
      fflush(a->index_file) != 0) {
    Error_set_errno_prefix(error, "fwrite() failed: ", errno);
    if (need_free) free(write_buf);
    return false;
  }
  if (need_free) free(write_buf);

  if (!ensure_capacity(a, a->entry_count + 1u, error))
    return false;

  /* Insert at sorted position (idx). */
  if (idx < a->entry_count)
    memmove(&a->entries[idx + 1], &a->entries[idx],
            (a->entry_count - idx) * sizeof(*a->entries));
  object_archive_entry_t* e = &a->entries[idx];
  e->file_offset       = (u32)file_offset;
  e->compressed_size   = (u32)write_len;
  e->uncompressed_size = (u32)data_size;
  e->key_size          = (u32)keylen;
  e->compress_type     = compression;
  e->key               = (char*)malloc(keylen + 1u);
  if (!e->key) {
    /* Roll back insert position; caller-visible state is now degraded but
     * the disk write succeeded.  Best-effort. */
    if (idx < a->entry_count)
      memmove(&a->entries[idx], &a->entries[idx + 1],
              (a->entry_count - idx) * sizeof(*a->entries));
    Error_set_string(error, "out of memory storing key");
    return false;
  }
  memcpy(e->key, key, keylen);
  e->key[keylen] = '\0';
  a->entry_count++;
  return true;
}

u64 object_archive_total_object_size(const object_archive_t* a)
{
  u64 total = 0;
  for (u32 i = 0; i < a->entry_count; i++)
    total += a->entries[i].uncompressed_size;
  return total;
}

u64 object_archive_total_size(const object_archive_t* a)
{
  u64 total = 0;
  for (u32 i = 0; i < a->entry_count; i++)
    total += a->entries[i].compressed_size + sizeof(cache_index_entry_header_t) +
             a->entries[i].key_size;
  total += sizeof(cache_file_header_t);
  return total;
}
